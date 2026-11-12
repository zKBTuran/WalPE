/* video_custom.c - the in-house video backend.
 *
 * Custom MP4 demuxer (mp4demux.c) feeds libavcodec's H.264 decoder
 * (itself hand-written assembly — the heavy 95% of decode work); the
 * decoded planes go to the GPU as they are and a shader converts them
 * (software GL: the validated YUV->BGRA path of yuvconv.c) and the frame
 * draws as a fullscreen textured quad.
 *
 * ONE DECODE, ALL MONITORS: sources are refcounted and keyed by file
 * path, so monitors showing the same file share a single demuxer +
 * decoder + PTS clock. Each monitor (viewer) keeps its own GL textures
 * and uploads a frame only when the source's sequence counter advanced.
 * The decoder pauses only while EVERY viewer is paused (covered/DPMsed/
 * paused monitors just skip their upload+draw).
 *
 * Presentation (version 0.12): the decode thread runs a few frames ahead into
 * a small queue; every frame carries its presentation time on one clock
 * shared by all viewers. A viewer shows the newest frame that is due and
 * asks its render thread to come back when the next one is
 * (WalpeVideo.next_frame_us), so a 60 fps clip shows 60 different frames
 * a second whatever the decode times were. No main-loop timer is involved
 * (lesson 9).
 *
 * Resources:
 * - No copy between the decoder and the GPU. The queue holds references
 *   to the decoder's own frames (AVFrame) and the viewers upload the
 *   planes straight from them (GL_UNPACK_ROW_LENGTH = the decoder's line
 *   size). Before, every frame was copied row by row into a tightly
 *   packed buffer first (12 MB per 4K frame) and the queue owned four
 *   such buffers. 8-bit and 9..16-bit planar YUV with any chroma layout
 *   and NV12/P010 (hardware decoders) are uploaded as they are; the
 *   shader takes the sample depth as a scale. Only the half-size path (a
 *   4K clip on a 1080p monitor) still writes a buffer: the 2x2 box
 *   filter, which is vectorised now.
 * - The source lock is not held while a viewer uploads (the frame is
 *   pinned instead), so the decode thread and the other monitors do not
 *   wait behind an upload.
 * - Parked while unseen: when every viewer has been paused for
 *   CV_PARK_S seconds the decoder, its frame pool and the queue are
 *   freed; on resume the decoder restarts at the keyframe before the
 *   frame that was on screen and decodes (without showing, skipping
 *   pictures nothing refers to) up to it, so playback continues exactly
 *   where it stopped.
 * - Hardware decoding (off by default, video.h): a hardware decoder
 *   decodes, the frame is downloaded as NV12/P010 and takes the same
 *   upload path; any failure falls back to software decoding at the same
 *   position. Which decoder, on which device, in which order is decided
 *   by what FFmpeg in this build offers and by which GPU renders, not by
 *   who made the GPU - with one exception: where NVIDIA's own driver
 *   renders, "cuda" stays the first attempt (video_hwsel.h holds the
 *   rules). With "video_zerocopy" the picture is not downloaded where the
 *   decoder can export it as DMA-BUFs and EGL can import them: the plane
 *   textures are then the decoder's own memory, drawn by the same shader. */
#include "video.h"
#include "videotex.h"
#include "glutil.h"
#include "common/lazy.h"
#include "mp4demux.h"
#include "yuvconv.h"
#include "modules.h"
#include "video_hwsel.h"
#include <epoxy/egl.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>   /* a picture described as DMA-BUFs */
#include <libavutil/log.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
/* the daemon's record of the GPU in use, in a tree that keeps one */
#if defined(__has_include)
#if __has_include("common/gpusel.h")
#include "common/gpusel.h"
#define CV_HAVE_GPUSEL 1
#endif
#endif
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif
#ifdef __GLIBC__
#include <malloc.h>
#endif
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__x86_64__) && defined(__GNUC__) && !defined(WALPE_VIDEO_NO_AVX2)
#include <immintrin.h>
#endif

#include "video_threads.h"

#define MAX_VIEWER_SNAP 8 /* viewers notified per frame (monitors) */
#define CV_TICKS 32       /* frame limit: a scene's frame rate is taken over this many of its frames */
/* frames ready at a time: the one on screen + 2 decoded ahead (33 ms of
 * slack at 60 fps for a slow frame). One more slot is being written. */
#define CV_AHEAD 3
/* the decoder is this late with a frame: continue from where it is
 * (slower playback) instead of skipping frames to catch up */
#define CV_SLIP_S 0.1
/* every viewer paused for this long: free the decoder, its frames and the
 * queue (WALPE_VIDEO_PARK_S overrides, 0 = never) */
#define CV_PARK_S 5.0
/* resuming a parked clip decodes from the keyframe before the frame that
 * was on screen, which delays the first new frame. A keyframe so far
 * back that this is more than CV_PARK_WORK_S seconds of decoding at the
 * conservative rate of cv_threads_for (a third to a half of that on a
 * current desktop CPU) keeps the decoder instead. */
#define CV_PARK_WORK_S 1.5
#define CV_PARK_CATCHUP 600     /* and never more frames than this */
/* The largest picture the decoder may allocate. The picture size comes
 * from the stream (the SPS; the container's own width / height fields are
 * informational), and nothing bounded it: a clip of a few kilobytes that
 * declares 16384 x 16384 made libavcodec allocate ~400 MB per frame and
 * the daemon queue several of them (plus the same again in video memory).
 * H.264's highest level (6.2) allows 139264 macroblocks = 35.7 megapixels
 * (8192 x 4352; 8K video is 7680 x 4320); 40 leaves room for the
 * decoder's line alignment. Larger pictures are refused: the clip shows
 * nothing and the log says why. */
#define CV_MAX_PIXELS (40 * 1000 * 1000)
#define CV_BARREN 64             /* packets in a row without a picture: see cv_decode_thread */

/* upload paths (video.h, WALPE_VUP_*): buffers a monitor writes in turn
 * (persist), and buffers the decode thread writes (direct): the queue's
 * frames, the one being written and one the GPU may still be reading */
#define CV_RING_N 3
#define CV_STAGE_N (CV_AHEAD + 2)

typedef struct _CvSource CvSource;

/* a persistently mapped pixel buffer object */
typedef struct {
    GLuint name;
    guint8 *map;
    gsize cap;
    GLsync fence;       /* its owner's render thread: uploads issued from it, not seen finished */
    /* direct (src->lock): */
    gboolean attached;  /* a frame slot has its data in it */
    gboolean writing;   /* the decode thread is filling it right now */
    gboolean gpu_busy;  /* the owner holds a fence on it */
    double busy_at;     /* when that upload was issued (and flushed) */
} CvPbuf;

/* how a frame's samples are laid out for the upload */
enum {
    CV_BGRA = 0,    /* one BGRA plane (software GL: converted on the CPU) */
    CV_YUV,         /* three planes Y, U, V */
    CV_NV,          /* two planes: Y and interleaved UV (NV12, P010) */
};

/* one frame ready to show */
typedef struct {
    AVFrame *av;                /* the decoder's frame when its planes are used as they are */
    guint8 *data;               /* own storage: half-size planes, BGRA, repacked planes */
    gsize cap;
    int kind;                   /* CV_* */
    const guint8 *pl[3];        /* planes to upload */
    int ls[3];                  /* their line sizes in bytes */
    int pw[3], ph[3];           /* their sizes in samples */
    int bytes;                  /* per sample component: 1, or 2 (little-endian) */
    float scale;                /* texture value -> 8-bit code units (255 for 8-bit samples) */
    int w, h;                   /* picture size */
    guint mode;                 /* WALPE_YUV_*: range + matrix */
    double pts;                 /* presentation time, s; keeps counting across loops */
    double raw;                 /* the stream's own time stamp, s (-1 = none) */
    guint64 seq;
    int pins;                   /* src->lock: viewers uploading from it right now */
    /* direct upload: its data is in the source's mapped buffer stage1 - 1
     * (0 = in ordinary memory), stage_len bytes from the buffer's start */
    int stage1;
    gsize stage_len;
    /* zero-copy (video.h "video_zerocopy"): the picture stays in GPU
     * memory. hw keeps the decoder's surface for as long as the frame is
     * queued, drm describes it as DMA-BUFs, zc_pl are the planes a viewer
     * imports (video_hwsel.h). zc: 0 = an ordinary frame; 1 = import (there
     * are no planes in main memory, pl[] is not set); 2 = it was downloaded
     * as well and a viewer compares the two before the import is trusted. */
    AVFrame *hw, *drm;
    int zc, zc_n;
    int zc_gen;                 /* the decoder start it belongs to (CvSource.zc_gen) */
    WalpeDmaPlane zc_pl[3];
    int zc_fd[WALPE_DMA_MAX];
    float zc_crop[2][4];        /* luma, chroma: texture coordinate scale x, y; largest x, y */
} CvFrame;

/* per-monitor half: GL textures in its EGL context, upload + draw */
typedef struct {
    WalpeVideo base;
    CvSource *src;
    GLuint tex;                 /* BGRA frames; RGBA target of a video texture */
    gboolean tex_alloced;
    int tex_w, tex_h;
    guint64 drawn_seq;          /* last frame uploaded here */
    GLuint ptex[3];             /* plane textures (GPU conversion) */
    int pw[3], ph[3];
    GLint pfmt[3];              /* their internal formats */
    int pkind;                  /* CV_YUV / CV_NV of the uploaded planes, 0 = BGRA in tex */
    float pscale;
    guint ymode;                /* WALPE_YUV_* of the uploaded planes */
    int fw, fh;                 /* size of the uploaded picture */
    GLuint fbo;                 /* video texture: planes -> RGBA tex */
    guint64 rgb_seq;            /* frame converted into tex */
    gboolean paused;
    gboolean starved;           /* src->lock: nothing to wait for; the decode
                                 * thread wakes this viewer with the next frame */
    /* src->lock: the host's redraw callback as the decode thread uses it.
     * base.request_redraw / request_user are filled in by the host after
     * the viewer is already attached, as two unlocked stores: read from
     * the decode thread directly, the pair could be seen half written
     * (callback set, user still NULL). cvv_gl_init copies them here. */
    void (*redraw_cb) (gpointer user);
    gpointer redraw_user;
    guint shown, skipped;       /* src->lock: frames uploaded / never shown here */
    /* src->lock, WALPE_FPS_LOG: CPU seconds and bytes of the uploads and
     * draws since the last log line */
    double st_upload, st_draw, st_bytes;
    guint st_draws;
    /* on-screen size of the whole frame in pixels (atomic; 0 = unknown or
     * full resolution required): lets the decoder hand out half size */
    gint want_w, want_h;
    /* src->lock, frame limit (cv_keep_rate): how many pictures a second
     * this viewer shows. A video wallpaper's is the frame rate limit of its
     * monitor (limit_fps, 0 = none); a scene asks its video layer for a
     * picture once per scene frame, so there it is measured (tick_*). */
    gboolean measured;
    int limit_fps;
    double tick_t[CV_TICKS];    /* when it asked, the last CV_TICKS times (a ring) */
    int tick_n, tick_i;         /* how many of them are valid, where the next goes */
    double tick_rate;           /* the rate they give, 0 = not known (yet) */
    /* upload path (video.h; render thread) */
    int up_mode;                /* WALPE_VUP_* its GL objects are made for, -1 = none yet */
    int up_real;                /* the path it really takes (a fallback when the GL cannot) */
    GLuint palt[3];             /* +pingpong: the other set of plane textures */
    int altw[3], alth[3];
    GLint altfmt[3];
    GLuint pbo;                 /* pbo / pbodata: the buffer that gets a new store per frame */
    CvPbuf ring[CV_RING_N];     /* persist */
    int ring_next;
    /* counters (render thread; the log line and the test read them):
     * frames uploaded, glTexSubImage2D calls, glBufferData / SubData calls,
     * maps, fences, frames taken from a buffer the decode thread wrote,
     * frames that found no idle buffer, bytes this thread copied itself */
    guint up_frames, up_tex_calls, up_buf_calls, up_maps, up_fences, up_staged, up_waits;
    double up_copied;
    /* zero-copy (render thread unless noted): the planes drawn are EGL
     * images of the decoder's own picture instead of uploaded textures */
    int zc_can;                 /* src->lock: this GL context can import DMA-BUFs; -1 = not asked yet */
    const char *zc_cant;        /* src->lock: why it cannot (a constant) */
    int zc_how;                 /* 1 = glEGLImageTargetTexture2DOES, 2 = glEGLImageTargetTexStorageEXT */
    gboolean zc_mods;           /* the EGL display takes format modifiers */
    gboolean zc_on;             /* ztex is what is drawn */
    GLuint ztex[3];
    EGLImageKHR zimg[3];
    AVFrame *zhold;             /* the picture they show: the decoder does not reuse it meanwhile */
    float zcrop[2][4];
    GLuint zfbo;                /* the self-check reads the imported planes back through it */
    guint64 skip_seq;           /* a frame that could not be shown here: not tried again */
    /* src->lock, WALPE_FPS_LOG: what the uploads since the last log line
     * added to the counters above, and the path they took. The render
     * thread adds to it under the lock it takes after every upload anyway
     * (cv_viewer_upload); the decode thread prints and clears it under the
     * same lock. The log line used to read and clear the render thread's
     * own counters from the decode thread, without any lock. */
    struct {
        guint frames, tex_calls, buf_calls, maps, fences, staged, waits;
        double copied;
        int real, mode;
    } up_log;
    /* src->lock: what this user wants the clip played from (videotex.h,
     * WalpeVideoTexCopy; a video wallpaper says nothing: the clip) */
    int copy_state;
    char *copy_path;
    int copy_w, copy_h;
} CvViewer;

/* shared decode pipeline (path-keyed, refcounted) */
struct _CvSource {
    GMutex lock;
    GCond cond;
    GPtrArray *viewers;         /* CvViewer* */
    int refs;                   /* attached viewers */
    gboolean quit;
    gboolean paused_all;        /* every viewer paused -> decoder sleeps */

    char *path;                 /* registry key (file, or file@off+len) */
    Mp4Demux *demux;
    AVCodecContext *dec;        /* NULL while parked */
    AVPacket *pkt;
    AVFrame *frame;
    AVFrame *swframe;           /* hardware decoding: the downloaded frame */
    GThread *thread;
    gboolean sent_sps;          /* SPS/PPS injected into the first packet */
    guint barren;               /* decode thread: packets in a row that gave no picture */
    GString *annexb;            /* decode thread: reusable packet buffer */

    /* frames ready to show. The decode thread fills a spare slot WITHOUT
     * the lock and queues it under it. q[] = oldest first: the one on
     * screen and up to CV_AHEAD - 1 decoded ahead. */
    CvFrame buf[CV_AHEAD + 1];
    CvFrame *q[CV_AHEAD];
    int nq;
    CvFrame *spare[CV_AHEAD + 1];
    int nspare;
    /* presentation clock: a frame is due at clk + its pts (seconds on the
     * monotonic clock). Stops while every viewer is paused. */
    gboolean clk_set;
    double clk;
    double pause_at;            /* when the last viewer paused, 0 = running */
    double slipped;             /* the decoder fell behind by this much in total */
    guint decoded;              /* frames queued (fps log) */
    /* planes handed to the GPU (default): no CPU colour conversion and 1.5
     * instead of 4 bytes per pixel to upload. Software GL or
     * WALPE_CPU_YUV=1 keeps the BGRA path (yuvconv.c). */
    gboolean gpu_yuv;
    gint gl_r16;                /* atomic: -1 unknown, 1 = the viewers' GL takes 16-bit planes */
    /* 2x2 box-downscaled frames while every viewer shows the video at
     * <= half its size (a 4K clip on a 1080p screen): 1/4 of the
     * upload and texture memory */
    gboolean half;
    guint8 *half_buf;           /* CPU path: downscaled I420 scratch */
    gsize half_cap;
    guint8 *norm_buf;           /* frame repacked to 8-bit 4:2:0 (cv_normalize) */
    gsize norm_cap;
    gboolean has_frame;
    guint64 seq;
    double frame_interval;      /* ewma of pts deltas (display-sync beat) */
    double last_push_pts;       /* -1 = none yet */

    /* decode thread only: presentation times across loop wraps */
    double last_raw, loop_off, last_play;
    gboolean wrapped;           /* the demuxer restarted the clip */
    /* decode thread only: restart at a keyframe (after a park, or when a
     * hardware decoder gave up): frames before skip_until are decoded but
     * not shown; the first one shown gets the presentation time rebase_play */
    double skip_until;          /* stream time, -1 = show everything */
    guint skip_idx;             /* the sample that carries that frame */
    gboolean rebase;
    double rebase_play;
    double rebase_raw;          /* the picture rebase_play - one interval belongs to, -1 = none */
    /* parked: every viewer paused for CV_PARK_S; decoder and frames freed */
    gboolean parked;            /* src->lock */
    gboolean no_park;           /* src->lock: this pause cannot be parked (keyframe too far back) */
    double park_raw, park_play; /* the frame that was on screen */
    guint park_cursor;          /* the keyframe sample to restart from */
    /* src->lock: a viewer that has no picture yet asks a parked source
     * for one (cv_viewer_upload) */
    gboolean want_first;
    gint passes;                /* atomic: times the clip started over (counted by the decode
                                 * thread, readable from any thread) */
    /* software decode threads: as many as the clip needs (cv_threads_for),
     * raised up to threads_max when the decoder falls behind */
    int threads, threads_max;
    /* decode thread: is the clip keeping up (video_threads.h), and the
     * count the decoder is reopened with where the next sequence starts
     * (an IDR picture or the loop point; 0 = no change pending) */
    WalpeVLag lag;
    int raise_to;
    double raise_at;            /* when that was decided */
    /* src->lock: frames queued, and those queued after their presentation
     * time (cv_push_frame) */
    guint lag_frames, lag_late;
    /* hardware decoding (video_hwsel.h holds the rules; decode thread, or
     * source creation before the thread exists, unless noted) */
    WalpeHwSel hw;              /* the attempts, the one in use, why the others failed */
    gboolean hw_planned;        /* hw was made for hw_want and the machine as known then */
    WalpeHwWant hw_want;        /* the setting the plan was made for */
    guint hw_serial;            /* walpe_video_hwdec_api's count at that time */
    gint hw_gpu_serial;         /* cv_gpu_serial at that time */
    gboolean hw_asked;          /* hardware decoding is asked for (hw_want.on) */
    gboolean hw_on;             /* the open decoder is a hardware decoder */
    gboolean hw_own;            /* ... one that finds its hardware itself (no device of ours) */
    gboolean hw_gpu_pics;       /* ... and its pictures arrive in GPU memory */
    AVBufferRef *hw_dev;        /* its device, NULL = none of ours */
    enum AVPixelFormat hw_fmt;  /* the format of its pictures in GPU memory, NONE = it says */
    const char *hw_name;        /* the API's name */
    /* atomic: 1 = the device does not take this stream, 2 = a picture
     * beyond the size limit, 3 = pictures cropped at the left or top */
    gint hw_refused;
    int hw_fmt_taken;           /* times libavcodec offered hw_fmt and got it */
    int hw_sw_fmt;              /* the format the stream decodes to in software */
    int hw_errors;              /* failures in a row */
    char *hw_profile;           /* "H.264 High@L4.2, 8-bit 4:2:0" (status line) */
    GString *hw_log;            /* FFmpeg's warnings and errors while a hardware decoder is tried */
    /* zero-copy */
    gboolean zc_asked;          /* the switch when the decoder was opened */
    gboolean zc_ok;             /* this decoder's pictures may go out as DMA-BUFs */
    int zc_slots;               /* viewers its picture pool was sized for */
    AVFrame *zc_hw;             /* the picture in GPU memory that is being handed over */
    int zc_cur;                 /* how: 0 / 1 / 2 as CvFrame.zc */
    gint zc_verified;           /* atomic: pictures whose imported planes equalled their download */
    int zc_gen;                 /* src->lock: counts the decoder starts; a comparison counts for its own */
    guint64 zc_check_seq;       /* src->lock: the last picture that was counted (once, whoever compared) */
    gint zc_refused;            /* atomic: a viewer could not import, or found a difference */
    gint zc_dirty;              /* atomic: the status line is to be written again */
    char *zc_why;               /* src->lock: why zero-copy is not in use */
    guint zc_sent;              /* fps log: pictures handed over without a copy */
    /* direct upload (src->lock): persistently mapped buffers that the
     * viewer stage_owner made in its GL context and that the decode thread
     * fills (cv_stage_take); stage_want = the largest frame it had to place */
    CvPbuf stage[CV_STAGE_N];
    int stage_n;
    gsize stage_cap, stage_want;
    CvViewer *stage_owner;
    gboolean stage_closing;
    guint stage_misses;         /* frames that found no free buffer (fps log) */
    /* the frame the last hardware picture was downloaded into, used again
     * when nothing else refers to it any more (cv_dl_take) */
    AVFrame *dl_keep;
    guint dl_reused, dl_fresh;  /* decode thread, fps log */
    /* frame limit (video.h: "videos follow the frame rate limit", off by
     * default): the viewers show keep_rate pictures a second and the clip
     * has more (0 = every picture is wanted). Pictures off that grid are
     * left out: not decoded when no other picture refers to them, decoded
     * and not queued otherwise. */
    double keep_rate;           /* src->lock */
    double clip_iv, clip_t0;    /* frame interval and first time stamp by the sample table, s */
    double drop_raw_max;        /* decode thread: latest stream time left out in this pass, -1 = none */
    guint lim_skipped, lim_unshown; /* decode thread, fps log: not decoded / decoded and not queued */
    /* decode thread, WALPE_FPS_LOG: CPU seconds per stage since the last line */
    double st_demux, st_decode, st_prep, st_copied, st_workers;
    /* A scene's clip may be played from a converted copy of itself
     * (videotex.h). `demux` is then the copy's; the clip is found again
     * through the registry key and its byte range. */
    gboolean scene;             /* src->lock: a scene's video texture */
    guint64 range_off, range_len;   /* the clip in its file (0, 0 = the whole file) */
    int orig_w, orig_h;         /* the clip's own size, pictures and frame rate (set */
    guint orig_n;               /*  when the source is made, then only read) */
    double orig_fps;
    int orig_rot;               /* ... and its display matrix (mp4demux.h rotation; 0 = none) */
    int play_w, play_h;         /* src->lock: what `demux` is, for the readers that */
    double play_fps;            /*  are not the decode thread */
    char *alt_path;             /* src->lock (written by the decode thread): the copy
                                 * that is played, NULL = the clip itself */
    char *alt_bad;              /* src->lock: why the copy that is asked for is not used */
    /* another file from where the clip starts again (cv_alt_update asks,
     * cv_swap_now does it) */
    gint alt_pending;           /* atomic */
    Mp4Demux *alt_demux;        /* src->lock: opened by who asked */
    char *alt_next;             /* src->lock: its file, NULL = the clip itself */
    /* atomic: what the decoder's pictures are (WalpeVideoTexInfo.format,
     * .matrix): 0 until the first one */
    gint pic_format, pic_matrix;
};

static const char *cv_up_name (int base);

/* --------------------------------------------------- registry (shared) */

static GMutex registry_lock;
static GHashTable *registry;    /* path -> CvSource* */
static gint cv_opened;          /* decoders opened so far (walpe-ctl mem) */

/* ------------------------------------------------------------ helpers */

static double cv_now (void)
{
    return g_get_monotonic_time () / 1e6;
}

/* WALPE_FPS_LOG=<seconds>: decoded and shown frame rates and the CPU time
 * of each stage (0 = off) */
static double cv_fps_log_s (void)
{
    /* asked by the decode thread of every video and by the render threads
     * (a double: g_once, see common/lazy.h) */
    static double v;
    static gsize once;
    if (g_once_init_enter (&once)) {
        const char *e = g_getenv ("WALPE_FPS_LOG");
        v = e && *e ? CLAMP (g_ascii_strtod (e, NULL), 0.0, 3600.0) : 0;
        g_once_init_leave (&once, 1);
    }
    return v;
}

/* CPU seconds of the calling thread (stage accounting; only read while
 * WALPE_FPS_LOG is on) */
static double cv_cpu (void)
{
    struct timespec ts;
    if (cv_fps_log_s () <= 0)
        return 0;
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

/* CPU seconds used so far by libavcodec's frame / slice threads of this
 * process (thread names "av:..."); *n = how many there are */
static double cv_workers_cpu (int *n)
{
    double tot = 0;
    *n = 0;
    DIR *d = opendir ("/proc/self/task");
    if (!d)
        return 0;
    long hz = sysconf (_SC_CLK_TCK);
    struct dirent *e;
    while ((e = readdir (d))) {
        if (e->d_name[0] == '.')
            continue;
        char pth[96], buf[512];
        g_snprintf (pth, sizeof pth, "/proc/self/task/%s/stat", e->d_name);
        FILE *f = fopen (pth, "r");
        if (!f)
            continue;
        size_t len = fread (buf, 1, sizeof buf - 1, f);
        fclose (f);
        buf[len] = 0;
        const char *lp = strchr (buf, '('), *rp = strrchr (buf, ')');
        if (!lp || !rp || strncmp (lp + 1, "av:", 3))
            continue;
        unsigned long ut = 0, st = 0;
        if (sscanf (rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2) {
            tot += (double) (ut + st) / (double) (hz > 0 ? hz : 100);
            (*n)++;
        }
    }
    closedir (d);
    return tot;
}

/* testing aid (WALPE_VIDEO_MD5=N): log the MD5 of the samples of the
 * first N frames as they are queued ("push") and as a viewer uploads
 * them ("upload"), in the layout `ffmpeg -f framemd5` hashes: the planes
 * row by row without padding. Shows that the frames handed to the GPU
 * are the decoder's output, unchanged while they waited in the queue. */
static int cv_md5_n (void)
{
    static WalpeLazy cell = WALPE_LAZY_UNSET;   /* read once (common/lazy.h) */
    int n = walpe_lazy_get (&cell);
    if (n < 0) {
        const char *e = g_getenv ("WALPE_VIDEO_MD5");
        n = walpe_lazy_set (&cell, e && *e ? CLAMP (atoi (e), 0, 1000000) : 0);
    }
    return n;
}

/* MD5 of the samples of f, planes row by row without padding (g_free) */
static char *cv_frame_md5_str (const CvFrame *f)
{
    if (f->zc == 1)
        return g_strdup ("(in GPU memory)");    /* zero-copy: no samples in main memory */
    GChecksum *ck = g_checksum_new (G_CHECKSUM_MD5);
    int n = f->kind == CV_BGRA ? 1 : f->kind == CV_NV ? 2 : 3;
    for (int i = 0; i < n; i++) {
        int unit = f->kind == CV_BGRA ? 4 : f->bytes * (f->kind == CV_NV && i == 1 ? 2 : 1);
        int pw = f->kind == CV_BGRA ? f->w : f->pw[i], ph = f->kind == CV_BGRA ? f->h : f->ph[i];
        for (int y = 0; y < ph; y++)
            g_checksum_update (ck, f->pl[i] + (gssize) y * f->ls[i], (gssize) pw * unit);
    }
    char *md5 = g_strdup (g_checksum_get_string (ck));
    g_checksum_free (ck);
    return md5;
}

static void cv_frame_md5 (const CvFrame *f, const char *what, const char *path)
{
    char *md5 = cv_frame_md5_str (f);
    g_message ("video: md5 %s seq %" G_GUINT64_FORMAT " t %.6f %dx%d kind %d: %s (%s)", what, f->seq,
               f->raw, f->w, f->h, f->kind, md5, path);
    g_free (md5);
}

/* ---------------------------------------------------- decode (source) */

/* Row kernels of the 2x2 box filter: n output samples from the rows r0 and
 * r1 (2n samples each, every column inside the plane). Each returns how
 * many samples it wrote, a multiple of its step; the caller finishes the
 * row in plain C. All of them compute (a + b + c + d + 2) >> 2 exactly, so
 * the picture does not depend on the kernel (tests/video_test.c compares
 * them with the plain C loop). row1 = one component per sample (Y, U, V
 * planes); row2 = two interleaved components (the UV plane of NV12, which
 * is what a hardware decoder delivers: it took the plain C loop before). */
#if defined(__SSE2__)
static int box_row1_sse2 (const guint8 *r0, const guint8 *r1, guint8 *o, int n)
{
    const __m128i lo = _mm_set1_epi16 (0x00ff), two = _mm_set1_epi16 (2);
    int x = 0;
    for (; x + 16 <= n; x += 16) {
        __m128i a0 = _mm_loadu_si128 ((const __m128i *) (gconstpointer) (r0 + 2 * x));
        __m128i a1 = _mm_loadu_si128 ((const __m128i *) (gconstpointer) (r0 + 2 * x + 16));
        __m128i b0 = _mm_loadu_si128 ((const __m128i *) (gconstpointer) (r1 + 2 * x));
        __m128i b1 = _mm_loadu_si128 ((const __m128i *) (gconstpointer) (r1 + 2 * x + 16));
        /* each 16-bit lane: the even and the odd sample of both rows */
        __m128i s0 = _mm_add_epi16 (
            _mm_add_epi16 (_mm_and_si128 (a0, lo), _mm_srli_epi16 (a0, 8)),
            _mm_add_epi16 (_mm_and_si128 (b0, lo), _mm_srli_epi16 (b0, 8)));
        __m128i s1 = _mm_add_epi16 (
            _mm_add_epi16 (_mm_and_si128 (a1, lo), _mm_srli_epi16 (a1, 8)),
            _mm_add_epi16 (_mm_and_si128 (b1, lo), _mm_srli_epi16 (b1, 8)));
        s0 = _mm_srli_epi16 (_mm_add_epi16 (s0, two), 2);
        s1 = _mm_srli_epi16 (_mm_add_epi16 (s1, two), 2);
        _mm_storeu_si128 ((__m128i *) (gpointer) (o + x), _mm_packus_epi16 (s0, s1));
    }
    return x;
}

static int box_row2_sse2 (const guint8 *r0, const guint8 *r1, guint8 *o, int n)
{
    const __m128i lo = _mm_set1_epi16 (0x00ff), one = _mm_set1_epi16 (1), two = _mm_set1_epi32 (2);
    int x = 0;
    for (; x + 8 <= n; x += 8) {
        __m128i u[2], v[2];
        for (int h = 0; h < 2; h++) {
            __m128i a = _mm_loadu_si128 ((const __m128i *) (gconstpointer) (r0 + 4 * x + 16 * h));
            __m128i b = _mm_loadu_si128 ((const __m128i *) (gconstpointer) (r1 + 4 * x + 16 * h));
            /* 16-bit lanes: the first component of 8 samples of both rows,
             * then the second; madd adds neighbouring samples */
            __m128i su = _mm_add_epi16 (_mm_and_si128 (a, lo), _mm_and_si128 (b, lo));
            __m128i sv = _mm_add_epi16 (_mm_srli_epi16 (a, 8), _mm_srli_epi16 (b, 8));
            u[h] = _mm_srli_epi32 (_mm_add_epi32 (_mm_madd_epi16 (su, one), two), 2);
            v[h] = _mm_srli_epi32 (_mm_add_epi32 (_mm_madd_epi16 (sv, one), two), 2);
        }
        __m128i u16 = _mm_packs_epi32 (u[0], u[1]), v16 = _mm_packs_epi32 (v[0], v[1]);
        _mm_storeu_si128 ((__m128i *) (gpointer) (o + 2 * x),
                          _mm_or_si128 (u16, _mm_slli_epi16 (v16, 8)));
    }
    return x;
}
#endif

/* AVX2 kernels, compiled for that instruction set whatever the build's
 * flags are and used only on a CPU that has it (cv_box_level): twice the
 * samples per step, and the pair sums of a row in one instruction. */
#if defined(__x86_64__) && defined(__GNUC__) && !defined(WALPE_VIDEO_NO_AVX2)
#define CV_BOX_AVX2 1
__attribute__ ((target ("avx2")))
static int box_row1_avx2 (const guint8 *r0, const guint8 *r1, guint8 *o, int n)
{
    const __m256i one = _mm256_set1_epi8 (1), two = _mm256_set1_epi16 (2);
    int x = 0;
    for (; x + 32 <= n; x += 32) {
        __m256i a0 = _mm256_loadu_si256 ((const __m256i *) (gconstpointer) (r0 + 2 * x));
        __m256i a1 = _mm256_loadu_si256 ((const __m256i *) (gconstpointer) (r0 + 2 * x + 32));
        __m256i b0 = _mm256_loadu_si256 ((const __m256i *) (gconstpointer) (r1 + 2 * x));
        __m256i b1 = _mm256_loadu_si256 ((const __m256i *) (gconstpointer) (r1 + 2 * x + 32));
        /* maddubs with 1: even + odd sample of a row, per 16-bit lane */
        __m256i s0 = _mm256_add_epi16 (_mm256_maddubs_epi16 (a0, one), _mm256_maddubs_epi16 (b0, one));
        __m256i s1 = _mm256_add_epi16 (_mm256_maddubs_epi16 (a1, one), _mm256_maddubs_epi16 (b1, one));
        s0 = _mm256_srli_epi16 (_mm256_add_epi16 (s0, two), 2);
        s1 = _mm256_srli_epi16 (_mm256_add_epi16 (s1, two), 2);
        /* packus works per 128-bit half: put the halves back in order */
        _mm256_storeu_si256 ((__m256i *) (gpointer) (o + x),
                             _mm256_permute4x64_epi64 (_mm256_packus_epi16 (s0, s1), 0xd8));
    }
    return x;
}

__attribute__ ((target ("avx2")))
static int box_row2_avx2 (const guint8 *r0, const guint8 *r1, guint8 *o, int n)
{
    const __m256i lo = _mm256_set1_epi16 (0x00ff), one = _mm256_set1_epi16 (1);
    const __m256i two = _mm256_set1_epi32 (2);
    int x = 0;
    for (; x + 16 <= n; x += 16) {
        __m256i w[2];
        for (int h = 0; h < 2; h++) {
            __m256i a = _mm256_loadu_si256 ((const __m256i *) (gconstpointer) (r0 + 4 * x + 32 * h));
            __m256i b = _mm256_loadu_si256 ((const __m256i *) (gconstpointer) (r1 + 4 * x + 32 * h));
            __m256i su = _mm256_add_epi16 (_mm256_and_si256 (a, lo), _mm256_and_si256 (b, lo));
            __m256i sv = _mm256_add_epi16 (_mm256_srli_epi16 (a, 8), _mm256_srli_epi16 (b, 8));
            __m256i u = _mm256_srli_epi32 (_mm256_add_epi32 (_mm256_madd_epi16 (su, one), two), 2);
            __m256i v = _mm256_srli_epi32 (_mm256_add_epi32 (_mm256_madd_epi16 (sv, one), two), 2);
            w[h] = _mm256_or_si256 (u, _mm256_slli_epi32 (v, 8));   /* 32-bit lane: u | v << 8 */
        }
        _mm256_storeu_si256 ((__m256i *) (gpointer) (o + 2 * x),
                             _mm256_permute4x64_epi64 (_mm256_packus_epi32 (w[0], w[1]), 0xd8));
    }
    return x;
}
#endif

/* which kernels the box filter uses: 0 = plain C, 1 = SSE2 (part of the
 * x86-64 baseline), 2 = AVX2 when the CPU has it. WALPE_VIDEO_SIMD=c|sse2
 * lowers it (comparisons); cv_box_forced is the test's switch. */
static int cv_box_forced = -1;
static int cv_box_level (void)
{
    static gint level = -1;
    if (cv_box_forced >= 0)
        return cv_box_forced;
    int lv = g_atomic_int_get (&level);
    if (lv < 0) {
        lv = 0;
#if defined(__SSE2__)
        lv = 1;
#endif
#ifdef CV_BOX_AVX2
        if (__builtin_cpu_supports ("avx2"))
            lv = 2;
#endif
        const char *e = g_getenv ("WALPE_VIDEO_SIMD");
        if (e && !g_ascii_strcasecmp (e, "c"))
            lv = 0;
        else if (e && !g_ascii_strcasecmp (e, "sse2"))
            lv = MIN (lv, 1);
        g_atomic_int_set (&level, lv);
    }
    return lv;
}

/* 2x2 box filter of one plane of 8-bit samples with nc interleaved
 * components (sw x sh pixels, stride ss bytes) into dw x dh
 * (dw <= ceil(sw/2)); edge pixels replicate. This is the half-size path of
 * every 4K frame shown on a 1080p monitor: 32 output samples per step with
 * AVX2, 16 with SSE2, plain C elsewhere and for the last columns. */
static void box2 (const guint8 *s, int ss, int sw, int sh, guint8 *d, int ds, int dw, int dh,
                  int nc)
{
    int safe = MIN (dw, sw / 2);    /* columns whose 2x+1 is inside */
    int lv = cv_box_level ();
    for (int y = 0; y < dh; y++) {
        const guint8 *r0 = s + (gsize) MIN (2 * y, sh - 1) * ss;
        const guint8 *r1 = s + (gsize) MIN (2 * y + 1, sh - 1) * ss;
        guint8 *o = d + (gsize) y * ds;
        int x = 0;
        if (nc == 1) {
#ifdef CV_BOX_AVX2
            if (lv >= 2)
                x = box_row1_avx2 (r0, r1, o, safe);
#endif
#if defined(__SSE2__)
            if (lv >= 1)
                x += box_row1_sse2 (r0 + 2 * x, r1 + 2 * x, o + x, safe - x);
#endif
            for (; x < safe; x++)
                o[x] = (guint8) ((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) >> 2);
            for (x = safe; x < dw; x++) {
                int x0 = MIN (2 * x, sw - 1);
                o[x] = (guint8) ((r0[x0] * 2 + r1[x0] * 2 + 2) >> 2);
            }
            continue;
        }
        if (nc == 2) {
#ifdef CV_BOX_AVX2
            if (lv >= 2)
                x = box_row2_avx2 (r0, r1, o, safe);
#endif
#if defined(__SSE2__)
            if (lv >= 1)
                x += box_row2_sse2 (r0 + 4 * x, r1 + 4 * x, o + 2 * x, safe - x);
#endif
        }
        (void) lv;
        for (; x < dw; x++) {
            int x0 = MIN (2 * x, sw - 1), x1 = MIN (2 * x + 1, sw - 1);
            for (int c = 0; c < nc; c++)
                o[x * nc + c] = (guint8) ((r0[x0 * nc + c] + r0[x1 * nc + c] +
                                           r1[x0 * nc + c] + r1[x1 * nc + c] + 2) >> 2);
        }
    }
}

/* The largest size any attached viewer shows the whole frame at, in
 * pixels (src->lock held). A viewer's size follows the render resolution
 * chosen for its monitor (cvv_gl_render) or the layer's size in its scene
 * (walpe_video_tex_set_view). 0 x 0: at least one viewer needs every pixel
 * of the frame, or none is attached - nothing smaller than the decoded
 * frame may be handed out. This is what a decoder that can scale while it
 * decodes asks; frames larger than the answer are always right. */
static void cv_want_size (CvSource *src, int *w, int *h)
{
    int mw = 0, mh = 0;
    for (guint i = 0; i < src->viewers->len; i++) {
        CvViewer *vw = g_ptr_array_index (src->viewers, i);
        int ww = g_atomic_int_get (&vw->want_w), wh = g_atomic_int_get (&vw->want_h);
        if (ww <= 0 || wh <= 0) {
            mw = mh = 0;
            break;
        }
        mw = MAX (mw, ww);
        mh = MAX (mh, wh);
    }
    *w = mw;
    *h = mh;
}

/* every attached viewer shows the frame at <= half size (src->lock held) */
static gboolean cv_half_ok (CvSource *src, int w, int h)
{
    /* (src->lock is this source's own: two videos get here at once) */
    static WalpeLazy disabled_cell = WALPE_LAZY_UNSET;
    int disabled = walpe_lazy_get (&disabled_cell);
    if (disabled < 0)
        disabled = walpe_lazy_set (&disabled_cell, g_getenv ("WALPE_VIDEO_FULLRES") ? 1 : 0);
    if (disabled || src->viewers->len == 0)
        return FALSE;
    int ww = 0, wh = 0;
    cv_want_size (src, &ww, &wh);
    return ww > 0 && wh > 0 && w / 2 >= ww && h / 2 >= wh;
}

/* range and matrix of a decoded frame (WALPE_YUV_* bits) */
static guint cv_frame_mode (const AVFrame *fr)
{
    guint mode = walpe_yuv_matrix_for ((int) fr->colorspace, fr->width, fr->height);
    if (fr->format == AV_PIX_FMT_YUVJ420P || fr->format == AV_PIX_FMT_YUVJ422P ||
        fr->format == AV_PIX_FMT_YUVJ444P || fr->color_range == AVCOL_RANGE_JPEG)
        mode |= WALPE_YUV_FULL;
    return mode;
}

/* Planar YUV that is not 8-bit 4:2:0 (High10, High 4:2:2, High 4:4:4:
 * HDR and screen-capture encodes) repacked into 8-bit 4:2:0 planes in
 * src->norm_buf. The GPU path uploads such frames as they are; this is
 * for software GL (the BGRA converter takes 8-bit 4:2:0 only) and for a
 * GL without 16-bit textures. Samples keep their top 8 bits; chroma is
 * averaged down to half size in each direction it is not already halved.
 * FALSE: a layout this cannot read (packed, RGB, big-endian, more than
 * 16 bits). */
static gboolean cv_normalize (CvSource *src, const guint8 *ip[3], int is[3])
{
    const AVFrame *fr = src->frame;
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get ((enum AVPixelFormat) fr->format);
    if (!d || d->nb_components < 3 || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) ||
        (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL |
                     AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL)) ||
        d->log2_chroma_w > 1 || d->log2_chroma_h > 1)
        return FALSE;
    int depth = d->comp[0].depth;
    if (depth < 8 || depth > 16 || d->comp[1].depth != depth || d->comp[2].depth != depth ||
        d->comp[0].plane != 0 || d->comp[1].plane != 1 || d->comp[2].plane != 2 ||
        !fr->data[0] || !fr->data[1] || !fr->data[2])
        return FALSE;
    int w = fr->width, h = fr->height;
    if (w < 1 || h < 1)
        return FALSE;
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    gsize need = (gsize) w * h + 2 * (gsize) cw * ch;
    if (src->norm_cap < need) {
        g_free (src->norm_buf);
        src->norm_buf = g_malloc (need);
        src->norm_cap = need;
    }
    gboolean wide = depth > 8;          /* 16-bit little-endian samples */
    int sh = depth - 8;
    guint8 *o = src->norm_buf;
    for (int y = 0; y < h; y++, o += w) {
        const guint8 *r = fr->data[0] + (gssize) y * fr->linesize[0];
        if (!wide) {
            memcpy (o, r, (gsize) w);
        } else {
            const guint16 *r16 = (const guint16 *) (gconstpointer) r;
            for (int x = 0; x < w; x++)
                o[x] = (guint8) MIN (r16[x] >> sh, 255);
        }
    }
    /* source chroma size, and how many source samples one output sample
     * covers in each direction (1 = already halved, 2 = full size) */
    int sx = d->log2_chroma_w ? 1 : 2, sy = d->log2_chroma_h ? 1 : 2;
    int scw = d->log2_chroma_w ? (w + 1) / 2 : w, sch = d->log2_chroma_h ? (h + 1) / 2 : h;
    for (int pl = 1; pl <= 2; pl++) {
        for (int y = 0; y < ch; y++, o += cw) {
            int y0 = MIN (y * sy, sch - 1), y1 = MIN (y * sy + sy - 1, sch - 1);
            const guint8 *r0 = fr->data[pl] + (gssize) y0 * fr->linesize[pl];
            const guint8 *r1 = fr->data[pl] + (gssize) y1 * fr->linesize[pl];
            for (int x = 0; x < cw; x++) {
                int x0 = MIN (x * sx, scw - 1), x1 = MIN (x * sx + sx - 1, scw - 1);
                guint v;
                if (!wide) {
                    v = ((guint) r0[x0] + r0[x1] + r1[x0] + r1[x1] + 2) >> 2;
                } else {
                    const guint16 *a = (const guint16 *) (gconstpointer) r0;
                    const guint16 *b = (const guint16 *) (gconstpointer) r1;
                    v = (((guint) a[x0] + a[x1] + b[x0] + b[x1] + 2) >> 2) >> sh;
                }
                o[x] = (guint8) MIN (v, 255);
            }
        }
    }
    ip[0] = src->norm_buf;
    ip[1] = ip[0] + (gsize) w * h;
    ip[2] = ip[1] + (gsize) cw * ch;
    is[0] = w;
    is[1] = is[2] = cw;
    return TRUE;
}

/* The planes of fr can go to the GPU as they are: fills the layout of f
 * (kind, plane sizes, bytes per sample, scale) and returns TRUE. Planar
 * YUV of 8..16 bits (little-endian) with any chroma layout, and Y +
 * interleaved UV (NV12, P010/P016 from hardware decoders). r16 = the GL
 * takes 16-bit textures. */
static gboolean cv_direct_layout (const AVFrame *fr, gboolean r16, CvFrame *f)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get ((enum AVPixelFormat) fr->format);
    if (!d || d->nb_components < 3 ||
        (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL |
                     AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BAYER)))
        return FALSE;
    int depth = d->comp[0].depth, shift = d->comp[0].shift;
    int bytes = depth > 8 ? 2 : 1;
    if (depth < 8 || depth > 16 || d->comp[1].depth != depth || d->comp[2].depth != depth ||
        d->comp[1].shift != shift || d->comp[2].shift != shift || depth + shift > 8 * bytes ||
        d->comp[0].plane != 0 || d->comp[0].offset != 0 || d->comp[0].step != bytes)
        return FALSE;
    if (bytes == 2 && !r16)
        return FALSE;
    int w = fr->width, h = fr->height;
    if (w < 1 || h < 1)
        return FALSE;
    int cw = AV_CEIL_RSHIFT (w, d->log2_chroma_w), ch = AV_CEIL_RSHIFT (h, d->log2_chroma_h);
    int n;
    if (d->comp[1].plane == 1 && d->comp[2].plane == 2 && d->comp[1].offset == 0 &&
        d->comp[2].offset == 0 && d->comp[1].step == bytes && d->comp[2].step == bytes) {
        f->kind = CV_YUV;
        n = 3;
    } else if (d->comp[1].plane == 1 && d->comp[2].plane == 1 && d->comp[1].offset == 0 &&
               d->comp[2].offset == bytes && d->comp[1].step == 2 * bytes &&
               d->comp[2].step == 2 * bytes) {
        f->kind = CV_NV;
        n = 2;
    } else {
        return FALSE;
    }
    for (int i = 0; i < n; i++) {
        int unit = bytes * (f->kind == CV_NV && i == 1 ? 2 : 1);
        if (!fr->data[i] || fr->linesize[i] <= 0 || fr->linesize[i] % unit)
            return FALSE;
        f->pw[i] = i ? cw : w;
        f->ph[i] = i ? ch : h;
        if (fr->linesize[i] < f->pw[i] * unit)
            return FALSE;
    }
    f->bytes = bytes;
    /* a sample of depth bits stored `shift` bits up in 8 or 16 bits: the
     * texture returns stored / (2^(8 bytes) - 1); in 8-bit code units the
     * value is stored / 2^(depth + shift - 8) */
    f->scale = (float) ((bytes == 2 ? 65535.0 : 255.0) / (double) (1 << (depth + shift - 8)));
    return TRUE;
}

/* Direct upload, decode thread: a mapped buffer of the stage for the need
 * bytes of frame f, or NULL - no viewer made a stage (every other upload
 * path), it is too small, or every buffer still holds a queued frame or
 * may still be read by the GPU (an upload from it was issued less than a
 * second ago and its fence has not been seen signalled). The frame then
 * goes where it went before. */
static guint8 *cv_stage_take (CvSource *src, CvFrame *f, gsize need)
{
    guint8 *p = NULL;
    g_mutex_lock (&src->lock);
    if (need > src->stage_want)
        src->stage_want = need;
    if (src->stage_n > 0 && !src->stage_closing) {
        double now = cv_now ();
        for (int i = 0; i < src->stage_n && need <= src->stage_cap; i++) {
            CvPbuf *b = &src->stage[i];
            if (b->attached || (b->gpu_busy && now - b->busy_at < 1.0))
                continue;
            b->attached = b->writing = TRUE;
            f->stage1 = i + 1;
            f->stage_len = need;
            p = b->map;
            break;
        }
        if (!p)
            src->stage_misses++;
    }
    g_mutex_unlock (&src->lock);
    return p;
}

/* decode thread: the bytes of f are written (a viewer that gives up the
 * stage waits for this) */
static void cv_stage_written (CvSource *src, CvFrame *f)
{
    g_mutex_lock (&src->lock);
    if (f->stage1 && f->stage1 <= src->stage_n && src->stage[f->stage1 - 1].writing) {
        src->stage[f->stage1 - 1].writing = FALSE;
        g_cond_broadcast (&src->cond);
    }
    g_mutex_unlock (&src->lock);
}

/* storage of f for at least need bytes: a mapped buffer of the stage
 * (direct upload), else its own */
static guint8 *cv_frame_buf (CvSource *src, CvFrame *f, gsize need)
{
    guint8 *p = cv_stage_take (src, f, need);
    if (p) {
        g_free (f->data);       /* not needed while the stage serves */
        f->data = NULL;
        f->cap = 0;
        return p;
    }
    if (f->cap < need) {
        g_free (f->data);
        f->data = g_malloc (need);
        f->cap = need;
    }
    return f->data;
}

static gboolean cv_dl_take (CvSource *src, AVFrame *sw, int fmt, int w, int h);
static void cv_dl_adopt (CvSource *src, const AVFrame *sw, int w, int h);

/* testing aid (WALPE_VIDEO_TEST_NV12=1): hand the software decoder's
 * 8-bit 4:2:0 frames on as NV12, the layout hardware decoders deliver,
 * so that path can be exercised without one */
static void cv_test_nv12 (CvSource *src)
{
    AVFrame *fr = src->frame;
    if (fr->format != AV_PIX_FMT_YUV420P && fr->format != AV_PIX_FMT_YUVJ420P)
        return;
    AVFrame *nv = src->swframe;
    av_frame_unref (nv);
    /* (the frame comes the way a download's does: the kept buffers when
     * they are free, else new ones that are kept for the next time) */
    if (!cv_dl_take (src, nv, AV_PIX_FMT_NV12, fr->width, fr->height)) {
        nv->format = AV_PIX_FMT_NV12;
        nv->width = fr->width;
        nv->height = fr->height;
        if (av_frame_get_buffer (nv, 0) < 0)
            return;
        cv_dl_adopt (src, nv, fr->width, fr->height);
    }
    int cw = (fr->width + 1) / 2, ch = (fr->height + 1) / 2;
    for (int y = 0; y < fr->height; y++)
        memcpy (nv->data[0] + (gssize) y * nv->linesize[0],
                fr->data[0] + (gssize) y * fr->linesize[0], (gsize) fr->width);
    for (int y = 0; y < ch; y++) {
        const guint8 *u = fr->data[1] + (gssize) y * fr->linesize[1];
        const guint8 *v = fr->data[2] + (gssize) y * fr->linesize[2];
        guint8 *o = nv->data[1] + (gssize) y * nv->linesize[1];
        for (int x = 0; x < cw; x++) {
            o[2 * x] = u[x];
            o[2 * x + 1] = v[x];
        }
    }
    av_frame_copy_props (nv, fr);
    if (fr->format == AV_PIX_FMT_YUVJ420P)
        nv->color_range = AVCOL_RANGE_JPEG;
    av_frame_unref (fr);
    av_frame_move_ref (fr, nv);
}

/* hand the decoded frame (src->frame) to f (decode thread, no lock);
 * returns FALSE for unsupported frames. *copied = bytes written here. */
static gboolean cv_fill_frame (CvSource *src, CvFrame *f, double *copied)
{
    static atomic_int fmt_err_n, fmt_note_n;   /* every decode thread counts (common/lazy.h) */
    AVFrame *fr = src->frame;
    int w = fr->width, h = fr->height;
    *copied = 0;
    f->mode = cv_frame_mode (fr);
    f->w = w;
    f->h = h;
    f->bytes = 1;
    f->scale = 255.0f;
    av_frame_unref (f->av);
    gboolean half = src->half && w >= 1280 && h >= 720;

    if (src->gpu_yuv && cv_direct_layout (fr, g_atomic_int_get (&src->gl_r16) == 1, f)) {
        gboolean c420 = f->pw[1] == (w + 1) / 2 && f->ph[1] == (h + 1) / 2;
        if (half && f->bytes == 1 && c420) {
            /* every viewer shows it at half size or less: 2x2 box filter
             * straight into the frame's own planes (1/4 of the upload) */
            int dw = w / 2, dh = h / 2, dcw = (dw + 1) / 2, dch = (dh + 1) / 2;
            int nc = f->kind == CV_NV ? 2 : 1;
            gsize ysz = (gsize) dw * dh, csz = (gsize) dcw * dch * nc;
            guint8 *d = cv_frame_buf (src, f, ysz + (f->kind == CV_NV ? csz : 2 * csz));
            box2 (fr->data[0], fr->linesize[0], w, h, d, dw, dw, dh, 1);
            box2 (fr->data[1], fr->linesize[1], f->pw[1], f->ph[1], d + ysz, dcw * nc, dcw, dch, nc);
            if (f->kind == CV_YUV)
                box2 (fr->data[2], fr->linesize[2], f->pw[2], f->ph[2], d + ysz + csz, dcw, dcw,
                      dch, 1);
            f->pl[0] = d;
            f->pl[1] = d + ysz;
            f->pl[2] = d + ysz + csz;
            f->ls[0] = dw;
            f->ls[1] = dcw * nc;
            f->ls[2] = dcw;
            f->pw[0] = dw;
            f->ph[0] = dh;
            f->pw[1] = f->pw[2] = dcw;
            f->ph[1] = f->ph[2] = dch;
            f->w = dw;
            f->h = dh;
            *copied = (double) (ysz + (f->kind == CV_NV ? csz : 2 * csz));
            return TRUE;
        }
        {
            /* direct upload: the planes are copied (tight) into a mapped
             * buffer here, so that the render thread has nothing to copy
             * and the decoder gets its frame back at once. Without a free
             * buffer - and on every other upload path - they go as before,
             * straight from the decoder's frame. */
            int n = f->kind == CV_NV ? 2 : 3;
            gsize need = 0, offs[3] = { 0, 0, 0 };
            for (int i = 0; i < n; i++) {
                offs[i] = need;
                need += (gsize) f->pw[i] * f->ph[i] * (gsize) (f->bytes * (f->kind == CV_NV && i == 1 ? 2 : 1));
            }
            guint8 *d = cv_stage_take (src, f, need);
            if (d) {
                for (int i = 0; i < n; i++) {
                    gsize row = (gsize) f->pw[i] * (gsize) (f->bytes * (f->kind == CV_NV && i == 1 ? 2 : 1));
                    for (int y = 0; y < f->ph[i]; y++)
                        memcpy (d + offs[i] + (gsize) y * row,
                                fr->data[i] + (gssize) y * fr->linesize[i], row);
                    f->pl[i] = d + offs[i];
                    f->ls[i] = (int) row;
                }
                if (n == 2) {
                    f->pl[2] = f->pl[1];
                    f->ls[2] = f->ls[1];
                }
                g_free (f->data);
                f->data = NULL;
                f->cap = 0;
                *copied = (double) need;
                return TRUE;
            }
        }
        /* the decoder's own planes: nothing is copied. The frame stays
         * referenced until it leaves the queue. */
        av_frame_move_ref (f->av, fr);
        for (int i = 0; i < 3; i++) {
            f->pl[i] = f->av->data[i];
            f->ls[i] = f->av->linesize[i];
        }
        if (f->data) {              /* the half-size buffer of an earlier frame */
            g_free (f->data);
            f->data = NULL;
            f->cap = 0;
        }
        return TRUE;
    }

    /* software GL (or a layout the GPU path does not take): 8-bit 4:2:0
     * planes, repacked on the CPU when the stream is anything else */
    const guint8 *ip[3] = { fr->data[0], fr->data[1], fr->data[2] };
    int is[3] = { fr->linesize[0], fr->linesize[1], fr->linesize[2] };
    if (fr->format != AV_PIX_FMT_YUV420P && fr->format != AV_PIX_FMT_YUVJ420P) {
        const char *fn = av_get_pix_fmt_name ((enum AVPixelFormat) fr->format);
        if (!cv_normalize (src, ip, is)) {
            if (walpe_first_n (&fmt_err_n, 5))
                g_warning ("video: frame format %s (%d) is not supported: %s shows nothing",
                           fn ? fn : "?", fr->format, src->path);
            return FALSE;
        }
        if (walpe_first_n (&fmt_note_n, 1))
            g_message ("video: %s frames are converted to 8-bit 4:2:0 on the CPU (%s)",
                       fn ? fn : "?", src->path);
    }
    f->kind = src->gpu_yuv ? CV_YUV : CV_BGRA;
    if (half && !src->gpu_yuv) {
        /* CPU path: convert 2x2 blocks directly (yuvconv.c) */
        int dw = (w / 2) & ~1, dh = (h / 2) & ~1;
        int stride = (dw * 4 + 63) & ~63;
        guint8 *d = cv_frame_buf (src, f, (gsize) stride * dh);
        walpe_yuv420_bgra_half (ip[0], ip[1], ip[2], is[0], is[1], d, stride, dw, dh, f->mode);
        f->pl[0] = d;
        f->ls[0] = stride;
        f->w = dw;
        f->h = dh;
        *copied = (double) stride * dh;
        return TRUE;
    }
    if (src->gpu_yuv) {
        /* repacked planes (or a GL without 16-bit textures): tight copy,
         * half size when every viewer shows it that small */
        int dw = half ? w / 2 : w, dh = half ? h / 2 : h;
        int dcw = (dw + 1) / 2, dch = (dh + 1) / 2, cw = (w + 1) / 2, ch = (h + 1) / 2;
        gsize ysz = (gsize) dw * dh, csz = (gsize) dcw * dch;
        guint8 *d = cv_frame_buf (src, f, ysz + 2 * csz);
        if (half) {
            box2 (ip[0], is[0], w, h, d, dw, dw, dh, 1);
            box2 (ip[1], is[1], cw, ch, d + ysz, dcw, dcw, dch, 1);
            box2 (ip[2], is[2], cw, ch, d + ysz + csz, dcw, dcw, dch, 1);
        } else {
            guint8 *o = d;
            for (int r = 0; r < h; r++, o += w)
                memcpy (o, ip[0] + (gssize) r * is[0], (gsize) w);
            for (int pl = 1; pl <= 2; pl++)
                for (int r = 0; r < ch; r++, o += cw)
                    memcpy (o, ip[pl] + (gssize) r * is[pl], (gsize) cw);
        }
        f->pl[0] = d;
        f->pl[1] = d + ysz;
        f->pl[2] = d + ysz + csz;
        f->ls[0] = dw;
        f->ls[1] = f->ls[2] = dcw;
        f->pw[0] = dw;
        f->ph[0] = dh;
        f->pw[1] = f->pw[2] = dcw;
        f->ph[1] = f->ph[2] = dch;
        f->w = dw;
        f->h = dh;
        *copied = (double) (ysz + 2 * csz);
        return TRUE;
    }
    int stride = (w * 4 + 63) & ~63;
    guint8 *d = cv_frame_buf (src, f, (gsize) stride * h);
    walpe_yuv420_bgra_ex (ip[0], ip[1], ip[2], is[0], is[1], d, stride, w, h, f->mode);
    f->pl[0] = d;
    f->ls[0] = stride;
    *copied = (double) stride * h;
    return TRUE;
}

/* seconds every viewer must have been paused before the decoder is freed */
static double cv_park_s (void)
{
    static double v;
    static gsize once;   /* (a double, asked by every decode thread: g_once) */
    if (g_once_init_enter (&once)) {
        const char *e = g_getenv ("WALPE_VIDEO_PARK_S");
        v = e && *e ? CLAMP (g_ascii_strtod (e, NULL), 0.0, 86400.0) : CV_PARK_S;
        g_once_init_leave (&once, 1);
    }
    return v;
}

static void cv_park (CvSource *src);

/* a frame leaves the queue (src->lock held): the decoder gets its buffer back */
static void cv_frame_release (CvSource *src, CvFrame *f)
{
    av_frame_unref (f->av);
    if (f->zc) {
        /* zero-copy: the description first, then the picture it maps (a
         * viewer that still shows it holds its own reference) */
        av_frame_unref (f->drm);
        av_frame_unref (f->hw);
        f->zc = f->zc_n = 0;
    }
    if (f->stage1) {
        /* its mapped buffer is free for another frame (once the GPU has
         * finished reading it: cv_stage_take) */
        if (f->stage1 <= src->stage_n)
            src->stage[f->stage1 - 1].attached = FALSE;
        f->stage1 = 0;
    }
    src->spare[src->nspare++] = f;
}

/* decode thread: a slot for the next frame, NULL = quitting or (*park)
 * every viewer was paused long enough and the decoder has been freed
 * (cv_park). Blocks
 * while the queue is full; the oldest queued frame leaves when the one
 * after it is due (nobody shows it any more). This wait is the decoder's
 * pacing: it stays CV_AHEAD - 1 frames ahead of the clock and never
 * decodes faster than the clip plays. */
static CvFrame *cv_take_buffer (CvSource *src, gboolean *park)
{
    CvFrame *f = NULL;
    *park = FALSE;
    g_mutex_lock (&src->lock);
    while (!src->quit) {
        if (src->nq < CV_AHEAD && src->nspare > 0) {
            f = src->spare[--src->nspare];
            break;
        }
        if (src->paused_all && !src->no_park && cv_park_s () > 0) {
            double until = src->pause_at + cv_park_s ();
            if (cv_now () >= until) {
                /* paused long enough: free the decoder here, under the
                 * lock (the caller's frame is decoded again on resume) */
                cv_park (src);
                if (src->parked) {
                    *park = TRUE;
                    break;
                }
                continue;
            }
            g_cond_wait_until (&src->cond, &src->lock, (gint64) (until * 1e6) + 1);
            continue;
        }
        if (src->paused_all || !src->clk_set || src->nq < 2) {
            g_cond_wait (&src->cond, &src->lock);
            continue;
        }
        double due = src->clk + src->q[1]->pts;
        if (cv_now () >= due) {
            if (src->q[0]->pins > 0) {
                /* a viewer is still uploading it: it says when it is done */
                g_cond_wait (&src->cond, &src->lock);
                continue;
            }
            cv_frame_release (src, src->q[0]);
            src->nq--;
            memmove (&src->q[0], &src->q[1], sizeof (src->q[0]) * (gsize) src->nq);
            continue;
        }
        g_cond_wait_until (&src->cond, &src->lock, (gint64) (due * 1e6) + 1);
    }
    g_mutex_unlock (&src->lock);
    return f;
}

/* queue a frame (src->lock held) */
static void cv_push_frame (CvSource *src, CvFrame *f)
{
    double now = cv_now ();
    f->seq = ++src->seq;
    src->q[src->nq++] = f;
    src->has_frame = TRUE;
    src->want_first = FALSE;
    src->decoded++;
    if (!src->clk_set) {
        /* the first frame shows at once (viewers take the head of the
         * queue until something is due); the clock starts a little later
         * so the decoder gets its lead */
        src->clk = now - f->pts + 0.040;
        src->clk_set = TRUE;
        if (src->paused_all)
            src->pause_at = now;    /* first frame of a paused source: the clock starts at resume */
    } else {
        /* (while every viewer is paused the presentation clock stands at
         * pause_at: a frame that was under way when the pause began is
         * not late by the length of the pause) */
        double late = (src->pause_at > 0 ? src->pause_at : now) - (src->clk + f->pts);
        /* a decoder that keeps up queues every frame ahead of its time
         * (it waits for a free slot): late frames are what
         * cv_threads_raise counts */
        src->lag_frames++;
        if (late > 0)
            src->lag_late++;
        if (late > CV_SLIP_S) {
            src->clk += late;
            src->slipped += late;
        }
    }
}

/* the frame to show now and when the next one is due (src->lock held):
 * the newest queued frame whose time has come; *next_due = monotonic
 * seconds of the following frame, 0 when it is not decoded yet */
static CvFrame *cv_pick_frame (CvSource *src, double *next_due)
{
    CvFrame *pick = NULL;
    *next_due = 0;
    double t = (src->pause_at > 0 ? src->pause_at : cv_now ()) - src->clk;
    for (int i = 0; i < src->nq; i++) {
        if (src->clk_set && src->q[i]->pts <= t + 0.0005) {
            pick = src->q[i];
        } else {
            *next_due = src->clk + src->q[i]->pts;
            break;
        }
    }
    if (!pick && src->nq > 0)
        pick = src->q[0];
    return pick;
}

/* ---------------------------------------------------------- frame limit
 * "Videos follow the frame rate limit" (video.h; off by default, then
 * nothing here does anything and every picture of the clip is decoded and
 * queued as before). When it is on and the viewers show fewer pictures a
 * second than the clip has - a video wallpaper under the fps setting or the
 * battery / focus / fps_idle limits, a scene that runs at 30 with a 60 fps
 * video layer, a 144 fps clip under a limit of 60 - the pictures they would
 * not show are left out. The ones that stay are the clip's own pictures,
 * decoded exactly as before: only which of them are shown changes. */

/* the setting; WALPE_VIDEO_LIMIT=0|1 overrides it */
static gboolean cv_limit_on (void)
{
    static const char *e;
    static gsize once;
    if (g_once_init_enter (&once)) {
        e = g_getenv ("WALPE_VIDEO_LIMIT");
        g_once_init_leave (&once, 1);
    }
    if (e && *e)
        return !(!g_ascii_strcasecmp (e, "0") || !g_ascii_strcasecmp (e, "off") ||
                 !g_ascii_strcasecmp (e, "no"));
    return walpe_video_limit () != 0;
}

/* Is the picture stamped t (seconds from the clip's first picture) one of
 * those that `rate` pictures a second show of a clip with frame interval
 * iv: the first picture at or after each point n / rate of the rate's
 * grid. A pure function of the time stamp, so the decision is the same
 * when a picture is still a packet (and need not be decoded) and when it
 * comes out of the decoder, in every pass of the clip. */
static gboolean cv_grid_keeps (double t, double iv, double rate)
{
    if (rate <= 0 || iv <= 0)
        return TRUE;
    return floor (t * rate + 1e-4) > floor ((t - iv) * rate + 1e-4);
}

/* No other picture is predicted from the one in sample s (nal_ref_idc 0
 * in every slice): leaving it undecoded changes no other picture. Only
 * samples made of such slices and of SEI / access unit delimiters /
 * filler qualify; IDR pictures, parameter sets, data partitions and
 * anything unknown are always decoded. */
static gboolean cv_sample_disposable (const Mp4Sample *s)
{
    gsize off = 0;
    int slices = 0;
    while (off + 5 <= (gsize) s->size) {
        guint32 len = ((guint32) s->data[off] << 24) | ((guint32) s->data[off + 1] << 16) |
                      ((guint32) s->data[off + 2] << 8) | (guint32) s->data[off + 3];
        if (len == 0 || len > (gsize) s->size - off - 4)
            return FALSE;
        int type = s->data[off + 4] & 0x1f, idc = (s->data[off + 4] >> 5) & 3;
        if (type == 1) {
            if (idc)
                return FALSE;
            slices++;
        } else if (type != 6 && type != 9 && type != 12) {
            return FALSE;
        }
        off += 4 + (gsize) len;
    }
    return slices > 0 && off == (gsize) s->size;
}

/* A scene asks its video layer for a picture once per frame it renders
 * (src->lock held): its frame rate is one over the median interval of its
 * last frames - the median, so that one late frame (a hitch, a short
 * pause) does not make the scene look slow for the next half second.
 * Known after 9 frames; three seconds without a frame start over. */
static void cv_viewer_tick (CvViewer *vw, double now)
{
    if (vw->tick_n > 0 && now - vw->tick_t[(vw->tick_i + CV_TICKS - 1) % CV_TICKS] >= 3.0)
        vw->tick_n = 0;
    vw->tick_t[vw->tick_i] = now;
    vw->tick_i = (vw->tick_i + 1) % CV_TICKS;
    vw->tick_n = MIN (vw->tick_n + 1, CV_TICKS);
    vw->tick_rate = 0;
    if (vw->tick_n < 9)
        return;
    double d[CV_TICKS];
    int n = 0;
    for (int k = vw->tick_n - 1; k > 0; k--) {
        double v = vw->tick_t[(vw->tick_i + CV_TICKS - k) % CV_TICKS] -
                   vw->tick_t[(vw->tick_i + CV_TICKS - k - 1) % CV_TICKS];
        int j = n++;
        for (; j > 0 && d[j - 1] > v; j--)      /* insertion sort: at most 31 values */
            d[j] = d[j - 1];
        d[j] = v;
    }
    double med = n & 1 ? d[n / 2] : (d[n / 2 - 1] + d[n / 2]) * 0.5;
    vw->tick_rate = med > 0 ? 1.0 / med : 0;
}

static double cv_viewer_rate (const CvViewer *vw)
{
    return vw->tick_rate;
}

/* pictures a second the viewers show when that is clearly less than the
 * clip has, else 0 (src->lock held). Paused viewers do not count; one
 * whose rate is not known (yet) wants every picture. */
static double cv_keep_rate (CvSource *src)
{
    if (!cv_limit_on () || src->clip_iv <= 0)
        return 0;
    double clip = 1.0 / src->clip_iv, best = 0;
    for (guint i = 0; i < src->viewers->len; i++) {
        CvViewer *vw = g_ptr_array_index (src->viewers, i);
        if (vw->paused)
            continue;
        double r = vw->measured ? cv_viewer_rate (vw) : (double) vw->limit_fps;
        if (r <= 0)
            return 0;
        best = MAX (best, r);
    }
    if (best <= 0 || best >= clip * 0.95)
        return 0;
    best = MAX (best, 1.0);
    /* a measured rate within 4 % of the clip's rate divided by a whole
     * number is that rate (a scene at 30 with a 60 fps clip shows every
     * second picture, without a slip every few seconds) */
    double k = clip / best, kr = floor (k + 0.5);
    if (kr >= 2 && fabs (k - kr) < 0.04 * kr)
        best = clip / kr;
    return best;
}

/* ------------------------------------------------- decoder open / close */

/* frames per second of the clip from its sample table (0 = unknown) */
static double cv_clip_fps (const Mp4Demux *d)
{
    if (d->nsamples < 2)
        return 0;
    double span = d->samples[d->nsamples - 1].pts - d->samples[0].pts;
    return span > 0 ? (d->nsamples - 1) / span : 0;
}

/* a clip that needed more threads than it started with keeps them when
 * it is opened again in this process (registry key -> count) */
static GMutex learned_lock;
static GHashTable *learned;

static int cv_threads_learned (const char *key)
{
    g_mutex_lock (&learned_lock);
    int n = learned && key ? GPOINTER_TO_INT (g_hash_table_lookup (learned, key)) : 0;
    g_mutex_unlock (&learned_lock);
    return n;
}

static void cv_threads_learn (const char *key, int n)
{
    if (!key)
        return;
    g_mutex_lock (&learned_lock);
    if (!learned)
        learned = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_replace (learned, g_strdup (key), GINT_TO_POINTER (n));
    g_mutex_unlock (&learned_lock);
}

/* Decode threads for the clip (video_threads.h): *max = the most it may
 * get, sized by the picture as in version 0.12; returned = what it starts
 * with, sized by the work per second instead. A 1080p60 clip then
 * decodes on the decode thread itself: frame threads cost about a tenth
 * more CPU time for the same frames and a dozen thread wake-ups per
 * frame. The decoder gets more threads when it does fall behind
 * (cv_threads_raise). WALPE_VIDEO_THREADS=n fixes the count;
 * WALPE_VIDEO_THREADS_START=n (testing) sets only what it starts with. */
/* (a copy of the clip is another, smaller clip: it learns for itself) */
static const char *cv_learn_key (const CvSource *src)
{
    return src->alt_path ? src->alt_path : src->path;
}

static int cv_threads_for (const CvSource *src, int *max)
{
    const Mp4Demux *d = src->demux;
    gint64 px = (gint64) d->width * d->height;
    int cores = (int) g_get_num_processors ();
    const char *te = g_getenv ("WALPE_VIDEO_THREADS");
    if (te && atoi (te) > 0) {
        *max = CLAMP (atoi (te), 1, 16);
        return *max;
    }
    double fps = cv_clip_fps (d), bits = 0;
    for (guint i = 0; i < d->nsamples; i++)
        bits += 8.0 * d->samples[i].size;
    int n = walpe_vthreads_start (px, fps, d->nsamples ? bits * fps / d->nsamples : 0, cores, max);
    const char *ts = g_getenv ("WALPE_VIDEO_THREADS_START");
    if (ts && atoi (ts) > 0)
        n = atoi (ts);
    n = MAX (n, cv_threads_learned (cv_learn_key (src)));
    return CLAMP (n, 1, *max);
}

/* the hardware offers its format for this stream, or software decoding */
static enum AVPixelFormat cv_get_format (AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    CvSource *src = c->opaque;
    src->hw_sw_fmt = (int) c->sw_pix_fmt;
    if (src->hw_fmt != AV_PIX_FMT_NONE)
        for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
            if (*p == src->hw_fmt) {
                src->hw_fmt_taken++;
                return *p;
            }
    /* the device does not decode this stream (profile, size, bit depth):
     * the decode loop reopens the decoder for the next attempt or for
     * software decoding. A decoder that finds its hardware itself and
     * hands out pictures in main memory is not asked anything here. */
    if (!src->hw_own)
        g_atomic_int_set (&src->hw_refused, 1);
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get (*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *p;
    }
    return AV_PIX_FMT_NONE;
}

static void cv_hw_close (CvSource *src)
{
    av_buffer_unref (&src->hw_dev);
    src->hw_fmt = AV_PIX_FMT_NONE;
    src->hw_name = NULL;
    src->hw_on = src->hw_own = FALSE;
    src->zc_ok = FALSE;
    src->zc_cur = 0;
    walpe_hwsel_closed (&src->hw);
    if (src->zc_hw)
        av_frame_unref (src->zc_hw);
    if (src->dl_keep)
        av_frame_unref (src->dl_keep);
}

/* nothing but the kept copy refers to the buffers of k */
static gboolean cv_frame_sole (const AVFrame *k)
{
    if (!k || !k->buf[0])
        return FALSE;
    for (int i = 0; i < AV_NUM_DATA_POINTERS && k->buf[i]; i++)
        if (av_buffer_get_ref_count (k->buf[i]) != 1)
            return FALSE;
    return TRUE;
}

/* A frame of fmt, w x h to download a hardware picture into: sw gets the
 * buffers kept from the last download when they fit and nothing else
 * refers to them any more (the half-size path reads a download and lets
 * go of it, so a 4K clip on a 1080p monitor allocates its 12 MB once and
 * not 60 times a second). FALSE: none to reuse, the caller allocates. */
static gboolean cv_dl_take (CvSource *src, AVFrame *sw, int fmt, int w, int h)
{
    AVFrame *k = src->dl_keep;
    if (!k || k->format != fmt || k->width != w || k->height != h || !cv_frame_sole (k))
        return FALSE;
    av_frame_unref (sw);
    if (av_frame_ref (sw, k) < 0) {
        av_frame_unref (sw);
        return FALSE;
    }
    src->dl_reused++;
    return TRUE;
}

/* sw was allocated for a download of w x h (the size it was allocated
 * with): keep its buffers for the next one */
static void cv_dl_adopt (CvSource *src, const AVFrame *sw, int w, int h)
{
    AVFrame *k = src->dl_keep;
    src->dl_fresh++;
    if (!k || !sw->buf[0])
        return;
    av_frame_unref (k);
    for (int i = 0; i < AV_NUM_DATA_POINTERS && sw->buf[i]; i++) {
        k->buf[i] = av_buffer_ref (sw->buf[i]);
        if (!k->buf[i]) {
            av_frame_unref (k);
            return;
        }
    }
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        k->data[i] = sw->data[i];
        k->linesize[i] = sw->linesize[i];
    }
    k->format = sw->format;
    k->width = w;
    k->height = h;
}

/* The decoder's picture (src->frame, in GPU memory) into src->swframe.
 * What av_hwframe_transfer_data does when it is given an empty frame -
 * a frame of the device's download format at the size of the decoder's
 * surfaces, filled, then cut to the picture's size - but into the kept
 * buffers when they are free. Any failure with the kept buffers falls
 * back to libavutil's own allocation. */
static int cv_hw_download (CvSource *src)
{
    AVFrame *hw = src->frame, *sw = src->swframe;
    const AVHWFramesContext *fc =
        hw->hw_frames_ctx ? (const AVHWFramesContext *) (gconstpointer) hw->hw_frames_ctx->data : NULL;
    av_frame_unref (sw);
    if (fc && src->dl_keep && src->dl_keep->buf[0] &&
        cv_dl_take (src, sw, src->dl_keep->format, fc->width, fc->height)) {
        if (av_hwframe_transfer_data (sw, hw, 0) >= 0) {
            sw->width = hw->width;
            sw->height = hw->height;
            return 0;
        }
        /* the kept frame did not suit this picture: forget it */
        av_frame_unref (sw);
        av_frame_unref (src->dl_keep);
        src->dl_reused--;
    }
    int rc = av_hwframe_transfer_data (sw, hw, 0);
    if (rc >= 0 && fc)
        cv_dl_adopt (src, sw, fc->width, fc->height);
    return rc;
}

/* the hardware picture src->frame into main memory, in its place, with
 * its properties (time stamp, colour tags) */
static int cv_hw_to_memory (CvSource *src)
{
    int rc = cv_hw_download (src);
    if (rc >= 0)
        rc = av_frame_copy_props (src->swframe, src->frame);
    av_frame_unref (src->frame);
    if (rc < 0) {
        av_frame_unref (src->swframe);
        return rc;
    }
    av_frame_move_ref (src->frame, src->swframe);
    return 0;
}

/* ------------------------------------------------ hardware: the settings */

static void cv_note_decoder (CvSource *src);

/* WALPE_VIDEO_HWDEC, when it is set to one of the setting's words (off,
 * auto, or the name of one API): it decides instead of the setting */
static gboolean cv_hw_env (WalpeHwWant *w)
{
    static const char *e;
    static WalpeHwWant env;
    static gboolean env_ok;
    static gsize once;
    if (g_once_init_enter (&once)) {
        e = g_getenv ("WALPE_VIDEO_HWDEC");
        env_ok = e && *e && walpe_hwsel_parse (e, &env);
        if (e && *e && !env_ok)
            g_warning ("WALPE_VIDEO_HWDEC=%s: not off, auto or the name of a decoding API: ignored", e);
        g_once_init_leave (&once, 1);
    }
    if (env_ok)
        *w = env;
    return env_ok;
}

/* is hardware decoding asked for at all (any thread; an integer read) */
static gboolean cv_hw_is_asked (void)
{
    WalpeHwWant w;
    return cv_hw_env (&w) ? w.on : walpe_video_hwdec () != 0;
}

/* "video_hwdec": WALPE_VIDEO_HWDEC when set, else the setting. Returns a
 * number that changes whenever the setting was set. */
static guint cv_hw_want (WalpeHwWant *w)
{
    if (cv_hw_env (w))
        return 0;
    memset (w, 0, sizeof (*w));
    guint serial = walpe_video_hwdec_api (w->api, sizeof (w->api));
    w->on = walpe_video_hwdec () != 0;
    if (!w->on)
        w->api[0] = 0;
    return serial;
}

/* "video_zerocopy"; WALPE_VIDEO_ZEROCOPY=0|1 overrides it */
static gboolean cv_zc_asked (void)
{
    static const char *e;
    static gsize once;
    if (g_once_init_enter (&once)) {
        e = g_getenv ("WALPE_VIDEO_ZEROCOPY");
        g_once_init_leave (&once, 1);
    }
    if (e && *e)
        return !(!g_ascii_strcasecmp (e, "0") || !g_ascii_strcasecmp (e, "off") ||
                 !g_ascii_strcasecmp (e, "no"));
    return walpe_video_zerocopy () != 0;
}

/* ------------------------------------- hardware: what this FFmpeg offers */

typedef struct {
    const AVCodec *codec;
    enum AVHWDeviceType type;   /* the device it is given, NONE = it finds its hardware itself */
    enum AVPixelFormat fmt;     /* its pictures in GPU memory, NONE = main memory, or it says */
} CvHwImpl;
#define CV_MAX_OFFERS 16
static WalpeHwOffer cv_offers[CV_MAX_OFFERS];
static CvHwImpl cv_impls[CV_MAX_OFFERS];    /* cv_impls[i] belongs to cv_offers[i] */
static int cv_n_offers;

static void cv_offer_add (const char *api, WalpeHwKind kind, const AVCodec *codec,
                          enum AVHWDeviceType type, enum AVPixelFormat fmt)
{
    if (cv_n_offers >= CV_MAX_OFFERS || !api || !*api || !codec)
        return;
    WalpeHwOffer *o = &cv_offers[cv_n_offers];
    const char *tn = type != AV_HWDEVICE_TYPE_NONE ? av_hwdevice_get_type_name (type) : NULL;
    memset (o, 0, sizeof (*o));
    g_strlcpy (o->api, api, sizeof (o->api));
    g_strlcpy (o->codec, codec->name ? codec->name : "?", sizeof (o->codec));
    o->kind = kind;
    g_strlcpy (o->dev_type, tn ? tn : "", sizeof (o->dev_type));
    o->cookie = cv_n_offers;
    cv_impls[cv_n_offers].codec = codec;
    cv_impls[cv_n_offers].type = type;
    cv_impls[cv_n_offers].fmt = fmt;
    cv_n_offers++;
}

/* Everything this FFmpeg build can decode H.264 with besides its software
 * decoder (listed once per process): the hardware accelerations of its own
 * decoder, one per device type (avcodec_get_hw_config; the type has to be
 * one libavutil was built with, av_hwdevice_iterate_types), and the
 * hardware decoders that are codecs of their own (h264_v4l2m2m and the
 * like). Nothing is looked for by name: what is there is listed, and
 * video_hwsel.h puts it in order. */
static void cv_offers_init (void)
{
    static gsize once;
    if (!g_once_init_enter (&once))
        return;
    const AVCodec *native = avcodec_find_decoder (AV_CODEC_ID_H264);
    for (int i = 0; native; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config (native, i);
        if (!cfg)
            break;
        if (!(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            continue;
        gboolean built = FALSE, listed = FALSE;
        for (enum AVHWDeviceType t = av_hwdevice_iterate_types (AV_HWDEVICE_TYPE_NONE);
             t != AV_HWDEVICE_TYPE_NONE; t = av_hwdevice_iterate_types (t))
            built |= t == cfg->device_type;
        /* (a device type listed with a second pixel format: the first one) */
        for (int k = 0; k < cv_n_offers; k++)
            listed |= cv_impls[k].codec == native && cv_impls[k].type == cfg->device_type;
        if (built && !listed)
            cv_offer_add (av_hwdevice_get_type_name (cfg->device_type), WALPE_HW_DEVICE, native,
                          cfg->device_type, cfg->pix_fmt);
    }
    void *it = NULL;
    const AVCodec *c;
    while ((c = av_codec_iterate (&it))) {
        if (c == native || c->id != AV_CODEC_ID_H264 || !av_codec_is_decoder (c) ||
            !(c->capabilities & AV_CODEC_CAP_HARDWARE))
            continue;
        const char *name = c->wrapper_name && *c->wrapper_name ? c->wrapper_name : c->name;
        enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
        enum AVPixelFormat fmt = AV_PIX_FMT_NONE;
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config (c, i);
            if (!cfg)
                break;
            if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
                type = cfg->device_type;
                fmt = cfg->pix_fmt;
                break;
            }
            if (fmt == AV_PIX_FMT_NONE && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL))
                fmt = cfg->pix_fmt;
        }
        cv_offer_add (name, type != AV_HWDEVICE_TYPE_NONE ? WALPE_HW_WRAPPER_DEVICE : WALPE_HW_WRAPPER,
                      c, type, fmt);
    }
#ifdef CV_TEST_HWFAKE
    /* tests: two "hardware decoders" that are the software decoder, so that
     * the attempts, a refused stream and a failure in the middle of a clip
     * can be played through on a machine without decoding hardware
     * (tests/video_test.c) */
    if (native) {
        cv_offer_add ("fake", WALPE_HW_WRAPPER, native, AV_HWDEVICE_TYPE_NONE, AV_PIX_FMT_NONE);
        cv_offer_add ("fake", WALPE_HW_WRAPPER, native, AV_HWDEVICE_TYPE_NONE, AV_PIX_FMT_NONE);
    }
#endif
    g_once_init_leave (&once, 1);
}

/* how many of the offers a plan for `want` is made from: all of them */
static int cv_offers_for (const WalpeHwWant *want)
{
    int n = cv_n_offers;
#ifdef CV_TEST_HWFAKE
    /* (the tests' stand-ins, the last two, are tried by name only) */
    if (strcmp (want->api, "fake") && n >= 2)
        n -= 2;
#else
    (void) want;
#endif
    return n;
}

/* --------------------------------------- hardware: the GPU that renders */

static GMutex cv_gpu_lock;
static WalpeHwGpu cv_gpu;       /* cv_gpu_lock */
static gpointer cv_gpu_dpy;     /* cv_gpu_lock: the EGL display it was read from */
static gint cv_gpu_serial;      /* atomic: counts the changes of cv_gpu */

/* name is one of the words of a space-separated extension list */
static gboolean cv_ext_listed (const char *list, const char *name)
{
    gsize n = strlen (name);
    for (const char *p = list; p && (p = strstr (p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
            return TRUE;
    return FALSE;
}

/* the kernel driver behind a DRM device node ("" = not known): the name
 * the node's sysfs entry links to. For messages; one thing only is decided
 * by it: whether NVIDIA's own driver renders (cv_gpu_probe,
 * WalpeHwGpu.nvidia_driver). */
static void cv_node_driver (const char *node, char *out, gsize len)
{
    struct stat st;
    out[0] = 0;
    if (!node || !*node || stat (node, &st) != 0 || !S_ISCHR (st.st_mode))
        return;
    char link[96], target[256];
    g_snprintf (link, sizeof link, "/sys/dev/char/%u:%u/device/driver", major (st.st_rdev),
                minor (st.st_rdev));
    ssize_t n = readlink (link, target, sizeof target - 1);
    if (n <= 0)
        return;
    target[n] = 0;
    const char *base = strrchr (target, '/');
    base = base ? base + 1 : target;
    /* (a name, not text to pass on as it is) */
    if (*base && strspn (base, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
        strlen (base))
        g_strlcpy (out, base, len);
}

/* the render node of the device whose primary node is `card`
 * ("/dev/dri/card0" -> "/dev/dri/renderD128"), "" = it has none */
static void cv_card_render_node (const char *card, char *out, gsize len)
{
    out[0] = 0;
    const char *base = card ? strrchr (card, '/') : NULL;
    if (!base || !base[1] || strspn (base + 1, "abcdefghijklmnopqrstuvwxyzD0123456789") != strlen (base + 1))
        return;
    if (g_str_has_prefix (base + 1, "renderD")) {
        g_strlcpy (out, card, len);
        return;
    }
    char *dir = g_strdup_printf ("/sys/class/drm/%s/device/drm", base + 1);
    GDir *d = g_dir_open (dir, 0, NULL);
    const char *e;
    while (d && (e = g_dir_read_name (d)))
        if (g_str_has_prefix (e, "renderD") && strlen (e) < 24) {
            g_snprintf (out, len, "/dev/dri/%s", e);
            break;
        }
    if (d)
        g_dir_close (d);
    g_free (dir);
}

/* every DRM render node of the machine, with its kernel driver */
static void cv_gpu_nodes (WalpeHwGpu *g)
{
    g->n_nodes = 0;
    for (int i = 0; i < 64 && g->n_nodes < WALPE_HW_MAX_NODES; i++) {
        char path[64];
        g_snprintf (path, sizeof path, "/dev/dri/renderD%d", 128 + i);
        if (!g_file_test (path, G_FILE_TEST_EXISTS))
            continue;
        g_strlcpy (g->nodes[g->n_nodes], path, sizeof (g->nodes[0]));
        cv_node_driver (path, g->node_driver[g->n_nodes], sizeof (g->node_driver[0]));
        g->n_nodes++;
    }
}

/* Render thread, its context current: which device this EGL display
 * renders on - its DRM render node, and the compute device number where the
 * device reports one (EGL_EXT_device_query and the device's own
 * extensions) - and which render nodes the machine has. Read once per
 * display; a source whose decoder was opened before this was known follows
 * (cv_hw_stale). Only questions are asked: nothing of the display changes. */
static void cv_gpu_probe (void)
{
    EGLDisplay dpy = eglGetCurrentDisplay ();
    if (dpy == EGL_NO_DISPLAY)
        return;
    g_mutex_lock (&cv_gpu_lock);
    gboolean done = cv_gpu_dpy == (gpointer) dpy;
    g_mutex_unlock (&cv_gpu_lock);
    if (done)
        return;
    WalpeHwGpu g;
    memset (&g, 0, sizeof g);
    g.ordinal = -1;
    g.software = walpe_gl_is_software ();
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_query") ||
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_base")) {
        EGLAttrib a = 0;
        if (eglQueryDisplayAttribEXT (dpy, EGL_DEVICE_EXT, &a) && a) {
            EGLDeviceEXT dev = (EGLDeviceEXT) a;
            const char *dx = eglQueryDeviceStringEXT (dev, EGL_EXTENSIONS);
            if (dx) {
                const char *node = NULL;
                g.known = TRUE;
                if (cv_ext_listed (dx, "EGL_EXT_device_drm_render_node"))
                    node = eglQueryDeviceStringEXT (dev, EGL_DRM_RENDER_NODE_FILE_EXT);
                if (node && *node && strlen (node) < sizeof (g.render_node)) {
                    g_strlcpy (g.render_node, node, sizeof (g.render_node));
                } else if (cv_ext_listed (dx, "EGL_EXT_device_drm")) {
                    const char *card = eglQueryDeviceStringEXT (dev, EGL_DRM_DEVICE_FILE_EXT);
                    cv_card_render_node (card, g.render_node, sizeof (g.render_node));
                }
                /* the device says which compute device it is, or - by not
                 * having the extension - that it is none */
                g.ordinal_known = TRUE;
                if (cv_ext_listed (dx, "EGL_NV_device_cuda")) {
                    EGLAttrib ord = -1;
                    if (eglQueryDeviceAttribEXT (dev, EGL_CUDA_DEVICE_NV, &ord) && ord >= 0 && ord < 64)
                        g.ordinal = (int) ord;
                    else
                        g.ordinal_known = FALSE;
                }
                if (cv_ext_listed (dx, "EGL_MESA_device_software"))
                    g.software = TRUE;
            }
        }
        eglGetError ();     /* a refused question leaves an error code behind: not the caller's */
    }
#ifdef CV_HAVE_GPUSEL
    {
        /* the daemon's own record knows the device also where the EGL
         * driver cannot be asked */
        WalpeGpuInUse u;
        memset (&u, 0, sizeof u);
        if (walpe_gpu_in_use (&u)) {
            if (!g.render_node[0] && u.render_node && strlen (u.render_node) < sizeof (g.render_node))
                g_strlcpy (g.render_node, u.render_node, sizeof (g.render_node));
            if (u.driver && *u.driver)
                g_strlcpy (g.driver, u.driver, sizeof (g.driver));
            g.software |= u.software;
            g.known = TRUE;
            walpe_gpu_in_use_clear (&u);
        }
    }
#endif
    if (g.render_node[0] && !g.driver[0])
        cv_node_driver (g.render_node, g.driver, sizeof (g.driver));
    /* NVIDIA's own driver renders: "cuda" is then the first hardware decoder
     * (the project's rule, the one rule of video_hwsel.h that goes by a
     * name). The kernel driver of the render node says it; where that is
     * not known (no node, no sysfs), the vendor string of the display this
     * context renders on. Neither known = FALSE: the plan as without it. */
    /* (the daemon has the same fact from common/gpusel.h: walpe_gpu_in_use ().driver) */
    if (g.driver[0]) {
        g.nvidia_driver = !strcmp (g.driver, "nvidia");
    } else {
        const char *vendor = eglQueryString (dpy, EGL_VENDOR);
        g.nvidia_driver = vendor && !g_ascii_strncasecmp (vendor, "NVIDIA", 6);
        eglGetError ();     /* (as above: not the caller's) */
    }
    cv_gpu_nodes (&g);
    g_mutex_lock (&cv_gpu_lock);
    gboolean changed = memcmp (&cv_gpu, &g, sizeof g) != 0;
    cv_gpu = g;
    cv_gpu_dpy = (gpointer) dpy;
    g_mutex_unlock (&cv_gpu_lock);
    if (changed) {
        g_atomic_int_inc (&cv_gpu_serial);
        g_debug ("video: the renderer's device is %s%s%s%s, compute device %d%s%s",
                 g.known ? (g.render_node[0] ? g.render_node : "one without a render node") : "not known",
                 g.driver[0] ? " [" : "", g.driver, g.driver[0] ? "]" : "", g.ordinal,
                 g.software ? " (a software renderer)" : "",
                 g.nvidia_driver ? " (NVIDIA's driver: cuda is tried first)" : "");
    }
}

/* the machine as known now (a copy); returns the change count it belongs to */
static gint cv_gpu_get (WalpeHwGpu *g)
{
    g_mutex_lock (&cv_gpu_lock);
    *g = cv_gpu;
    gint serial = g_atomic_int_get (&cv_gpu_serial);
    g_mutex_unlock (&cv_gpu_lock);
    if (g->n_nodes == 0)
        cv_gpu_nodes (g);       /* no GL context has been seen yet */
#if defined(__x86_64__)
    /* x86-64: auto does not open FFmpeg's V4L2 stateful decoder (it would
     * open every /dev/video* node, webcams too); hwdec=v4l2m2m still does
     * (video_hwsel.h, walpe_hwsel_auto_skips) */
    g->x86_64 = TRUE;
#endif
    return serial;
}

/* ----------------------------------------- hardware: what FFmpeg says
 * FFmpeg tells why a device cannot be made, or why it does not take a
 * stream, only in its log: the calls return a general error code or simply
 * offer a software format. While a thread tries a hardware decoder, the
 * warnings and errors FFmpeg logs on that thread are therefore also
 * collected (the last few hundred bytes) for the status line. Everything
 * still goes to FFmpeg's own log output, unchanged. The callback is
 * installed the first time hardware decoding is asked for: a daemon that
 * never asks keeps FFmpeg's logging as it is. */
static __thread GString *cv_log_to;

static void cv_log_cb (void *avcl, int level, const char *fmt, va_list vl)
{
    GString *to = cv_log_to;
    if (to && fmt && level >= 0 && level <= AV_LOG_WARNING) {
        va_list cp;
        char line[240];
        va_copy (cp, vl);
        g_vsnprintf (line, sizeof line, fmt, cp);
        va_end (cp);
        g_strchomp (line);
        /* (it ends up in the status: plain ASCII, no control characters) */
        for (char *q = line; *q; q++)
            if ((guchar) *q < 0x20 || (guchar) *q >= 0x7f)
                *q = '?';
        if (*line) {
            if (to->len)
                g_string_append (to, " / ");
            g_string_append (to, line);
            if (to->len > 480)
                g_string_erase (to, 0, (gssize) (to->len - 480));
        }
    }
    av_log_default_callback (avcl, level, fmt, vl);
}

static void cv_log_install (void)
{
    static gsize once;
    if (g_once_init_enter (&once)) {
        av_log_set_callback (cv_log_cb);
        g_once_init_leave (&once, 1);
    }
}

/* ------------------------------------------- hardware: device and plan */

/* The device of attempt t. NULL: *why and *detail (g_free) say what
 * failed. A DRM node the user may not open is told apart from a device
 * that is not there before FFmpeg is asked (it reports both alike). */
static AVBufferRef *cv_hw_device (CvSource *src, WalpeHwTry *t, const CvHwImpl *im, WalpeHwWhy *why,
                                  char **detail)
{
    AVBufferRef *dev = NULL;
    int rc;
    *why = WALPE_HW_WHY_NO_DEVICE;
    *detail = NULL;
    gboolean by_node = t->node[0] && (t->derive || !strcmp (t->device, t->node));
    if (by_node && access (t->node, R_OK | W_OK) != 0) {
        int e = errno;
        if (e == EACCES || e == EPERM) {
            *why = WALPE_HW_WHY_PERMISSION;
            *detail = g_strdup_printf ("%s may not be opened by this user (%s): the node belongs to a "
                                       "group, usually render or video, that the user is not in",
                                       t->node, g_strerror (e));
        } else {
            *detail = g_strdup_printf ("%s: %s", t->node, g_strerror (e));
        }
        return NULL;
    }
    g_string_truncate (src->hw_log, 0);
    if (t->derive) {
        AVBufferRef *drm = NULL;
        rc = av_hwdevice_ctx_create (&drm, AV_HWDEVICE_TYPE_DRM, t->node, NULL, 0);
        if (rc >= 0 && drm) {
            rc = av_hwdevice_ctx_create_derived (&dev, im->type, drm, 0);
            av_buffer_unref (&drm);
        } else {
            /* this FFmpeg cannot open DRM devices: the API's own first
             * device, and the status says that the API chose */
            av_buffer_unref (&drm);
            rc = av_hwdevice_ctx_create (&dev, im->type, NULL, NULL, 0);
            if (rc >= 0 && dev) {
                t->derive = FALSE;
                t->where = WALPE_HW_UNKNOWN;
                t->dmabuf = FALSE;
                t->node[0] = t->driver[0] = 0;
            }
        }
    } else {
        rc = av_hwdevice_ctx_create (&dev, im->type, t->device[0] ? t->device : NULL, NULL, 0);
    }
    if (rc >= 0 && dev)
        return dev;
    av_buffer_unref (&dev);
    GString *d = g_string_new (av_err2str (rc));
    if (rc == AVERROR (EACCES) || rc == AVERROR (EPERM) ||
        walpe_hwsel_classify (src->hw_log->str, WALPE_HW_WHY_NO_DEVICE) == WALPE_HW_WHY_PERMISSION)
        *why = WALPE_HW_WHY_PERMISSION;
    {
        /* libraries FFmpeg loads by name for this API: it says only in its
         * log which one it missed */
        const WalpeHwApi *a = walpe_hwsel_api (cv_offers[t->offer].dev_type[0] ? cv_offers[t->offer].dev_type
                                                                               : t->api);
        char **libs = a && a->libs && *a->libs ? g_strsplit (a->libs, " ", -1) : NULL;
        for (int i = 0; libs && libs[i]; i++) {
            if (!*libs[i])
                continue;
            void *h = dlopen (libs[i], RTLD_LAZY | RTLD_LOCAL);
            if (h)
                dlclose (h);
            else
                g_string_append_printf (d, " (%s cannot be loaded)", libs[i]);
        }
        g_strfreev (libs);
    }
    if (src->hw_log->len)
        g_string_append_printf (d, "; FFmpeg: %s", src->hw_log->str);
    *detail = g_string_free (d, FALSE);
    return NULL;
}

/* Make the plan for the setting and the machine as they are now; failures
 * of an earlier plan are forgotten (the setting was switched: try again). */
static void cv_hw_replan (CvSource *src)
{
    WalpeHwWant want;
    WalpeHwGpu gpu;
    src->hw_serial = cv_hw_want (&want);
    src->hw_want = want;
    src->hw_asked = want.on;
    src->hw_planned = TRUE;
    if (!want.on) {
        /* the default: nothing is listed, probed or installed */
        src->hw_gpu_serial = g_atomic_int_get (&cv_gpu_serial);
        walpe_hwsel_start (&src->hw, NULL, 0, NULL, &want);
        return;
    }
    cv_offers_init ();
    cv_log_install ();
    cv_gpu_probe ();        /* (when this thread has a GL context; else what a render thread found) */
    src->hw_gpu_serial = cv_gpu_get (&gpu);
    /* the picture drawn by the CPU (software GL, WALPE_CPU_YUV): nothing
     * to gain, it would be downloaded for that */
    if (!src->gpu_yuv)
        gpu.software = TRUE;
    walpe_hwsel_start (&src->hw, cv_offers, cv_offers_for (&want), &gpu, &want);
    if (src->hw.n == 0 && src->hw.general == WALPE_HW_WHY_NOT_BUILT && want.api[0] &&
        av_hwdevice_find_type_by_name (want.api) == AV_HWDEVICE_TYPE_NONE) {
        g_free (src->hw.general_text);
        src->hw.general_text = g_strdup_printf ("%s: neither a device type nor an H.264 hardware "
                                                "decoder of this FFmpeg build", want.api);
    }
}

/* Decode thread: does the open decoder still fit - the setting, the
 * zero-copy switch (it sizes a hardware decoder's picture pool) and the GPU
 * that renders (a decoder opened before the first GL context of the
 * process did not know it)? Two integer comparisons while nothing changed. */
static gboolean cv_hw_stale (CvSource *src)
{
    WalpeHwWant want;
    guint serial = cv_hw_want (&want);
    gint gs = g_atomic_int_get (&cv_gpu_serial);
    gboolean zc = src->hw_on ? cv_zc_asked () : src->zc_asked;
    if (serial == src->hw_serial && gs == src->hw_gpu_serial && zc == src->zc_asked)
        return FALSE;
    gboolean stale = want.on != src->hw_want.on || strcmp (want.api, src->hw_want.api) != 0 ||
                     zc != src->zc_asked;
    if (!stale && want.on && gs != src->hw_gpu_serial) {
        /* the render GPU became known: only if the first attempt would be
         * another one than the decoder in use */
        WalpeHwGpu gpu;
        WalpeHwTry tries[WALPE_HW_MAX_TRIES];
        cv_gpu_get (&gpu);
        if (!src->gpu_yuv)
            gpu.software = TRUE;
        int n = walpe_hwsel_plan (cv_offers, cv_offers_for (&want), &gpu, &want, tries);
        const WalpeHwTry *cur = walpe_hwsel_current (&src->hw);
        stale = n > 0 && (!cur || strcmp (cur->api, tries[0].api) || strcmp (cur->device, tries[0].device) ||
                          cur->derive != tries[0].derive);
    }
    if (!stale) {
        src->hw_serial = serial;
        src->hw_gpu_serial = gs;
    }
    return stale;
}

/* a decoder that is a codec of its own is told what FFmpeg's decoder reads
 * from the stream by itself: the size the file declares and the parameter
 * sets (Annex B, as the packets carry them) */
static void cv_wrapper_setup (CvSource *src)
{
    static const guint8 sc[4] = { 0, 0, 0, 1 };
    const Mp4Demux *d = src->demux;
    src->dec->width = src->dec->coded_width = d->width;
    src->dec->height = src->dec->coded_height = d->height;
    gsize len = 8 + d->sps_len + d->pps_len;
    if (d->sps_len == 0 || d->pps_len == 0 || len > (1 << 20))
        return;
    guint8 *x = av_mallocz (len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!x)
        return;
    memcpy (x, sc, 4);
    memcpy (x + 4, d->sps, d->sps_len);
    memcpy (x + 4 + d->sps_len, sc, 4);
    memcpy (x + 8 + d->sps_len, d->pps, d->pps_len);
    src->dec->extradata = x;
    src->dec->extradata_size = (int) len;
}

/* --------------------------------------- hardware: zero-copy, decoder side */

/* why zero-copy is not in use, for the status line (any thread; never with
 * src->lock held). NULL = it is, or was not asked for. */
static void cv_zc_set_why (CvSource *src, const char *why)
{
    g_mutex_lock (&src->lock);
    if (g_strcmp0 (src->zc_why, why)) {
        g_free (src->zc_why);
        src->zc_why = g_strdup (why);
        g_atomic_int_set (&src->zc_dirty, 1);
    }
    g_mutex_unlock (&src->lock);
}

/* A decoder is being opened (t = the attempt, NULL = software): may its
 * pictures go out as DMA-BUFs, and how many must its pool hold for that.
 * Besides what the decoder needs for itself, pictures are held by the
 * queue (CV_AHEAD, one being handed over, one in the decode loop) and by
 * every viewer (the one on screen, and the next one while it is imported). */
static void cv_zc_setup (CvSource *src, const WalpeHwTry *t)
{
    const char *why = NULL;
    src->zc_asked = t ? cv_zc_asked () : FALSE;
    src->zc_ok = FALSE;
    src->zc_cur = 0;
    g_mutex_lock (&src->lock);
    src->zc_gen++;
    g_atomic_int_set (&src->zc_verified, 0);
    g_atomic_int_set (&src->zc_refused, 0);
    g_mutex_unlock (&src->lock);
    if (t && src->zc_asked) {
        const WalpeHwApi *a = walpe_hwsel_api (t->api);
        if (a && !a->dmabuf) {
            why = "this decoding API cannot export its pictures as DMA-BUFs";
        } else if (!t->dmabuf) {
            why = t->where == WALPE_HW_OTHER ? "the decoder is not the GPU that renders"
                                             : "the API chose the device: it is not known to be the "
                                               "GPU that renders";
        } else {
            g_mutex_lock (&src->lock);
            int nv = (int) src->viewers->len;
            g_mutex_unlock (&src->lock);
            src->zc_ok = TRUE;
            src->zc_slots = MAX (nv, 1) + 1;
            src->dec->extra_hw_frames = CV_AHEAD + 2 + 2 * src->zc_slots;
            why = "no picture yet";
        }
    }
    cv_zc_set_why (src, why);
}

/* Decode thread: how the hardware picture just decoded goes out. 0 = it is
 * downloaded (the safe way); 2 = both ways, and a viewer compares the
 * imported planes with the download (video_hwsel.h, WALPE_ZC_*); 1 = as
 * DMA-BUFs only, once enough pictures were found equal. */
static int cv_zc_mode (CvSource *src)
{
    if (!src->zc_ok || g_atomic_int_get (&src->zc_refused))
        return 0;       /* (zc_why says why) */
    const char *why = NULL;
    g_mutex_lock (&src->lock);
    if (src->viewers->len == 0)
        why = "no picture yet";
    else if ((int) src->viewers->len > src->zc_slots)
        why = "more monitors show this video than the decoder's picture pool was sized for";
    for (guint i = 0; !why && i < src->viewers->len; i++) {
        const CvViewer *vw = g_ptr_array_index (src->viewers, i);
        if (vw->zc_can < 0)
            why = "no picture yet";
        else if (!vw->zc_can)
            why = vw->zc_cant ? vw->zc_cant : "the renderer cannot import DMA-BUFs";
    }
    g_mutex_unlock (&src->lock);
    if (why) {
        cv_zc_set_why (src, why);
        return 0;
    }
    return g_atomic_int_get (&src->zc_verified) >= WALPE_ZC_CHECK_FRAMES ? 1 : 2;
}

/* Decode thread: describe the hardware picture hw as DMA-BUFs in the frame
 * slot f - f->drm (the description; closing it closes the buffers'
 * handles), f->hw (keeps the surface), the planes to import, and the
 * frame's layout as the upload path would have it. FALSE: this decoder's
 * pictures do not go out that way (*why, a constant); f holds nothing. */
static gboolean cv_zc_map (CvSource *src, CvFrame *f, const AVFrame *hw, const char **why)
{
    av_frame_unref (f->drm);
    av_frame_unref (f->hw);
    f->zc = f->zc_n = 0;
    *why = "the decoder cannot export its pictures as DMA-BUFs";
    if (hw->format == AV_PIX_FMT_DRM_PRIME) {
        if (av_frame_ref (f->drm, hw) < 0)
            return FALSE;
    } else {
        f->drm->format = AV_PIX_FMT_DRM_PRIME;
        if (av_hwframe_map (f->drm, hw, AV_HWFRAME_MAP_READ) < 0) {
            av_frame_unref (f->drm);
            return FALSE;
        }
    }
    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *) (gconstpointer) f->drm->data[0];
    const AVHWFramesContext *fc =
        hw->hw_frames_ctx ? (const AVHWFramesContext *) (gconstpointer) hw->hw_frames_ctx->data : NULL;
    enum AVPixelFormat swf = fc ? fc->sw_format : AV_PIX_FMT_NONE;
    if (d && swf == AV_PIX_FMT_NONE && d->nb_layers >= 1) {
        /* a decoder without a frames context: the first layer says it */
        switch (d->layers[0].format) {
        case WALPE_DRM_NV12: swf = AV_PIX_FMT_NV12; break;
        case WALPE_DRM_P010: swf = AV_PIX_FMT_P010LE; break;
        case WALPE_DRM_YUV420:
        case WALPE_DRM_YVU420: swf = AV_PIX_FMT_YUV420P; break;
        default: break;
        }
    }
    const AVPixFmtDescriptor *pd = swf != AV_PIX_FMT_NONE ? av_pix_fmt_desc_get (swf) : NULL;
    gboolean ok = d && pd && pd->nb_components >= 3 && hw->width >= 1 && hw->height >= 1 &&
                  !(pd->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL |
                                 AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL |
                                 AV_PIX_FMT_FLAG_BAYER));
    int depth = ok ? pd->comp[0].depth : 8, shift = ok ? pd->comp[0].shift : 0;
    int bytes = depth > 8 ? 2 : 1;
    ok = ok && depth >= 8 && depth <= 16 && pd->comp[1].depth == depth && pd->comp[2].depth == depth &&
         pd->comp[1].shift == shift && pd->comp[2].shift == shift && depth + shift <= 8 * bytes;
    WalpeDmaDesc desc;
    WalpeDmaWant want;
    memset (&desc, 0, sizeof desc);
    memset (&want, 0, sizeof want);
    if (ok) {
        *why = "the picture's layout is not one the plane textures can hold";
        want.nplanes = pd->comp[1].plane == 1 && pd->comp[2].plane == 1 ? 2 : 3;
        want.bytes = bytes;
        want.surf_w = fc && fc->width >= hw->width ? fc->width : hw->width;
        want.surf_h = fc && fc->height >= hw->height ? fc->height : hw->height;
        want.log2_cw = pd->log2_chroma_w;
        want.log2_ch = pd->log2_chroma_h;
        ok = d->nb_objects >= 1 && d->nb_objects <= WALPE_DMA_MAX && d->nb_layers >= 1 &&
             d->nb_layers <= WALPE_DMA_MAX;
        if (bytes == 2 && g_atomic_int_get (&src->gl_r16) != 1) {
            *why = "the renderer's GL has no 16-bit textures for this stream";
            ok = FALSE;
        }
    }
    if (ok) {
        desc.nb_objects = d->nb_objects;
        desc.nb_layers = d->nb_layers;
        for (int i = 0; i < d->nb_objects; i++) {
            desc.modifier[i] = d->objects[i].format_modifier;
            desc.size[i] = d->objects[i].size;
            f->zc_fd[i] = d->objects[i].fd;
            ok &= d->objects[i].fd >= 0;
        }
        for (int l = 0; l < d->nb_layers; l++) {
            desc.layers[l].format = d->layers[l].format;
            desc.layers[l].nb_planes = d->layers[l].nb_planes;
            for (int k = 0; k < WALPE_DMA_MAX && k < d->layers[l].nb_planes; k++) {
                desc.layers[l].planes[k].object = d->layers[l].planes[k].object_index;
                desc.layers[l].planes[k].offset = d->layers[l].planes[k].offset > 0
                    ? (gsize) d->layers[l].planes[k].offset : 0;
                desc.layers[l].planes[k].pitch = d->layers[l].planes[k].pitch > 0
                    ? (gsize) d->layers[l].planes[k].pitch : 0;
                ok &= d->layers[l].planes[k].offset >= 0;
            }
        }
        ok = ok && walpe_dma_planes (&desc, &want, f->zc_pl, why);
    }
    if (ok && av_frame_ref (f->hw, hw) < 0)
        ok = FALSE;
    if (!ok) {
        av_frame_unref (f->drm);
        av_frame_unref (f->hw);
        return FALSE;
    }
    f->zc_n = want.nplanes;
    /* the frame as the upload path describes it (cv_direct_layout) */
    int w = hw->width, h = hw->height;
    int cw = AV_CEIL_RSHIFT (w, want.log2_cw), ch = AV_CEIL_RSHIFT (h, want.log2_ch);
    f->kind = want.nplanes == 2 ? CV_NV : CV_YUV;
    f->bytes = bytes;
    f->scale = (float) ((bytes == 2 ? 65535.0 : 255.0) / (double) (1 << (depth + shift - 8)));
    f->mode = cv_frame_mode (hw);
    f->w = w;
    f->h = h;
    f->pw[0] = w;
    f->ph[0] = h;
    f->pw[1] = f->pw[2] = cw;
    f->ph[1] = f->ph[2] = ch;
    /* the picture is the top left part of the surface: where its right and
     * bottom edge are in texture coordinates, and the centre of its last
     * sample (the shader does not sample beyond it, as CLAMP_TO_EDGE does
     * on a texture of the picture's own size) */
    for (int c = 0; c < 2; c++) {
        const WalpeDmaPlane *pl = &f->zc_pl[c];
        int pw = c ? cw : w, ph = c ? ch : h;
        f->zc_crop[c][0] = (float) pw / (float) pl->w;
        f->zc_crop[c][1] = (float) ph / (float) pl->h;
        f->zc_crop[c][2] = ((float) pw - 0.5f) / (float) pl->w;
        f->zc_crop[c][3] = ((float) ph - 0.5f) / (float) pl->h;
    }
    *why = NULL;
    return TRUE;
}

/* Decode thread: f is the slot of the picture just decoded; src->zc_cur
 * says how it was to go out and src->zc_hw is the picture in GPU memory.
 * Returns what was arranged: 0 = an ordinary frame, src->frame is in main
 * memory; 1 = f shows the picture where it is and nothing is in main
 * memory; 2 = f can show it where it is and src->frame holds its download
 * for the comparison; -1 = the picture could not be had either way. */
static int cv_zc_take (CvSource *src, CvFrame *f)
{
    int zc = src->zc_cur;
    src->zc_cur = 0;
    if (!zc)
        return 0;
    const char *why = NULL;
    if (!cv_zc_map (src, f, src->zc_hw, &why)) {
        /* this decoder's pictures do not go out that way: downloads from here on */
        src->zc_ok = FALSE;
        cv_zc_set_why (src, why);
        g_message ("video: zero-copy is not used for %s (%s): pictures are downloaded", src->path,
                   why ? why : "?");
        av_frame_unref (src->zc_hw);
        if (zc == 1 && cv_hw_to_memory (src) < 0)
            return -1;      /* (src->frame was still the picture in GPU memory) */
        return 0;
    }
    av_frame_unref (src->zc_hw);
    f->zc_gen = src->zc_gen;    /* (written by this thread only) */
    if (zc == 1) {
        av_frame_unref (f->av);
        for (int i = 0; i < 3; i++) {
            f->pl[i] = NULL;
            f->ls[i] = 0;
        }
        f->zc = 1;
        src->zc_sent++;
        cv_zc_set_why (src, NULL);
    } else {
        cv_zc_set_why (src, "the first pictures are compared with their download before the import "
                            "is trusted");
    }
    return zc;
}

/* The status line names every video that is open, not only the one whose
 * decoder was opened last: with two videos playing, "hardware" for one
 * must not hide that the other fell back to software. note is taken (NULL
 * = the source is gone). Own lock, never held around another. */
static GMutex cv_notes_lock;
static GPtrArray *cv_notes;     /* CvSource *, char *note, ... in pairs */

static void cv_notes_set (CvSource *src, char *note)
{
    g_mutex_lock (&cv_notes_lock);
    if (!cv_notes)
        cv_notes = g_ptr_array_new ();
    guint i;
    for (i = 0; i + 1 < cv_notes->len; i += 2)
        if (g_ptr_array_index (cv_notes, i) == src)
            break;
    if (i + 1 < cv_notes->len) {
        g_free (g_ptr_array_index (cv_notes, i + 1));
        if (note) {
            g_ptr_array_index (cv_notes, i + 1) = note;
        } else {
            g_ptr_array_remove_range (cv_notes, i, 2);
        }
    } else if (note) {
        g_ptr_array_add (cv_notes, src);
        g_ptr_array_add (cv_notes, note);
    }
    GString *all = g_string_new (NULL);
    for (i = 0; i + 1 < cv_notes->len; i += 2)
        g_string_append_printf (all, "%s%s", all->len ? "; " : "",
                                (const char *) g_ptr_array_index (cv_notes, i + 1));
    /* (no video open any more: the line keeps what the last one did) */
    if (all->len)
        walpe_video_note_decoder (all->str);
    g_string_free (all, TRUE);
    g_mutex_unlock (&cv_notes_lock);
}

/* for `walpe-ctl status`: what this source's decoder does - size, frame
 * rate and codec profile; then the hardware decoder's API, its device and
 * whether that is the GPU that renders, and how the pictures reach the
 * renderer; or "software" and, when hardware decoding is asked for, every
 * attempt that failed with the kind of reason (video_hwsel.h has the exact
 * form). So the status alone tells whether hardware decodes and, if not,
 * what stopped it. A hardware decoder that has been opened and has not
 * delivered a picture yet says so: opening succeeds before the device has
 * seen the stream. Decode thread (or source creation), src->lock not held. */
static void cv_note_decoder (CvSource *src)
{
    WalpeHwNote nt;
    memset (&nt, 0, sizeof nt);
    nt.w = src->demux->width;
    nt.h = src->demux->height;
    nt.fps = cv_clip_fps (src->demux);
    /* the codec profile is said where hardware decoding is asked for (it is
     * what a device refuses a stream for); with the setting off the line is
     * the one it always was, letter for letter */
    nt.profile = src->hw_asked ? src->hw_profile : NULL;
    nt.threads = src->dec ? src->dec->thread_count : src->threads;
    nt.hw = src->hw_on;
    nt.in_memory = src->hw_on && src->hw_own && src->hw.seen && !src->hw_gpu_pics;
    g_mutex_lock (&src->lock);
    char *zw = g_strdup (src->zc_why);
    g_mutex_unlock (&src->lock);
    nt.zero_copy = src->hw_on && src->zc_ok && !zw && !g_atomic_int_get (&src->zc_refused) &&
                   g_atomic_int_get (&src->zc_verified) >= WALPE_ZC_CHECK_FRAMES;
    nt.zc_why = src->hw_on && src->zc_asked && !nt.zero_copy ? (zw ? zw : "no picture yet") : NULL;
    char *note = walpe_hwsel_note (&src->hw, &nt);
    g_free (zw);
    cv_notes_set (src, note);
}

/* Open the H.264 decoder (source creation, and again after a park).
 *
 * Decode exactly what the stream says (version 0.12): the in-loop deblocking
 * filter stays on (it is not post-processing: the encoder predicted every
 * P/B frame from the FILTERED reference frames), no skip_idct/skip_frame,
 * no lowres, no FAST flags. The CPU time comes from threads instead:
 * frame threading (slice threading where the stream has slices), as many
 * as the clip needs (cv_threads_for). Each frame thread adds one frame of
 * delay (no issue: nothing is interactive) and one frame buffer.
 * WALPE_VIDEO_THREADS=N overrides;
 * WALPE_VIDEO_SKIP_DEBLOCK=1 skips the filter (for a CPU that cannot
 * decode the clip otherwise; the picture degrades between keyframes).
 *
 * Hardware decoding asked for: the attempts of the plan that have not
 * failed are tried in order (video_hwsel.h) - make the device, open the
 * decoder with it - and the first that opens is used; when none is left
 * the software decoder is opened as always. */
static gboolean cv_decoder_open (CvSource *src, gboolean first, GError **err)
{
    const AVCodec *sw = avcodec_find_decoder (AV_CODEC_ID_H264);
    if (!sw) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_FAILED, "no H.264 decoder");
        return FALSE;
    }
    if (src->threads < 1)
        src->threads = cv_threads_for (src, &src->threads_max);
    if (!src->hw_planned)
        cv_hw_replan (src);
    GString *log_prev = cv_log_to;
    gboolean opened = FALSE;
    for (int round = 0; round <= WALPE_HW_MAX_TRIES && !opened; round++) {
        WalpeHwTry *t = NULL;
        if (src->hw_asked && walpe_hwsel_next (&src->hw))
            t = &src->hw.tries[src->hw.cur];
        const CvHwImpl *im = t ? &cv_impls[t->offer] : NULL;
        const AVCodec *codec = t ? im->codec : sw;
        AVBufferRef *dev = NULL;
        /* (what FFmpeg logs on this thread while a hardware decoder is tried) */
        cv_log_to = t ? src->hw_log : log_prev;
        if (t)
            g_string_truncate (src->hw_log, 0);
        if (t && im->type != AV_HWDEVICE_TYPE_NONE) {
            WalpeHwWhy why = WALPE_HW_WHY_NO_DEVICE;
            char *detail = NULL;
            dev = cv_hw_device (src, t, im, &why, &detail);
            if (!dev) {
                GString *nm = g_string_new (NULL);
                walpe_hwsel_try_name (t, nm);
                g_message ("video: %s cannot be used (%s: %s)", nm->str, walpe_hwsel_why_name (why),
                           detail ? detail : "?");
                g_string_free (nm, TRUE);
                walpe_hwsel_failed (&src->hw, why, detail);
                g_free (detail);
                continue;
            }
        }
        src->dec = avcodec_alloc_context3 (codec);
        if (!src->dec) {
            av_buffer_unref (&dev);
            break;
        }
        src->dec->max_pixels = CV_MAX_PIXELS;   /* no frame buffer beyond it (see there) */
        if (t) {
            /* a hardware decoder: one thread feeds it */
            src->hw_dev = dev;
            src->hw_fmt = im->fmt;
            src->hw_name = cv_offers[t->offer].api;
            src->hw_on = TRUE;
            src->hw_own = im->type == AV_HWDEVICE_TYPE_NONE;
            src->hw_fmt_taken = 0;
            src->hw_gpu_pics = FALSE;
            src->hw_sw_fmt = (int) AV_PIX_FMT_NONE;
            if (dev)
                src->dec->hw_device_ctx = av_buffer_ref (dev);
            src->dec->get_format = cv_get_format;
            src->dec->opaque = src;
            src->dec->thread_count = 1;
            if (codec != sw)
                cv_wrapper_setup (src);
        } else {
            src->dec->thread_count = src->threads;
            src->dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        cv_zc_setup (src, t);
        {
            /* opt-in only: "nonref" leaves the filter out for the pictures
             * no other picture is predicted from (they alone get blockier,
             * nothing builds on them); any other value for all pictures */
            const char *sd = g_getenv ("WALPE_VIDEO_SKIP_DEBLOCK");
            if (sd && !g_ascii_strcasecmp (sd, "nonref"))
                src->dec->skip_loop_filter = AVDISCARD_NONREF;
            else if (sd)
                src->dec->skip_loop_filter = AVDISCARD_ALL;
        }
        src->dec->time_base = (AVRational) { 1, (int) src->demux->timescale };
        g_atomic_int_set (&src->hw_refused, 0);
        src->hw_errors = 0;
        walpe_vlag_restart (&src->lag, cv_now ());   /* the start is not the steady state */
        if (avcodec_open2 (src->dec, codec, NULL) >= 0) {
            if (t && first) {
                GString *nm = g_string_new (NULL);
                walpe_hwsel_try_name (t, nm);
                g_message ("video: %dx%d %.2f fps, hardware decoding (%s)", src->demux->width,
                           src->demux->height, cv_clip_fps (src->demux), nm->str);
                g_string_free (nm, TRUE);
            } else if (first) {
                g_message ("video: %dx%d %.2f fps, %d decode thread(s)%s", src->demux->width,
                           src->demux->height, cv_clip_fps (src->demux), src->dec->thread_count,
                           src->dec->skip_loop_filter == AVDISCARD_ALL ? ", deblocking SKIPPED"
                           : src->dec->skip_loop_filter == AVDISCARD_NONREF
                               ? ", deblocking SKIPPED for non-reference pictures" : "");
            }
            opened = TRUE;
            g_atomic_int_inc (&cv_opened);   /* walpe-ctl mem */
            break;
        }
        avcodec_free_context (&src->dec);
        if (!t)
            break;
        {
            /* the device exists and the decoder does not open with it */
            char *d = g_strdup (src->hw_log->len ? src->hw_log->str : "avcodec_open2 failed");
            g_message ("video: the %s decoder could not be opened (%s)", src->hw_name, d);
            walpe_hwsel_failed (&src->hw, walpe_hwsel_classify (d, WALPE_HW_WHY_OPEN), d);
            g_free (d);
        }
        cv_hw_close (src);
    }
    cv_log_to = log_prev;
    if (opened) {
        cv_note_decoder (src);
        return TRUE;
    }
    g_set_error (err, G_IO_ERROR, G_IO_ERROR_FAILED, "avcodec_open2 failed");
    return FALSE;
}

/* ------------------------------------------- position, park and resume */

/* presentation time stamp of sample i in stream ticks, as handed to the
 * decoder (and returned by it with the frame) */
static gint64 cv_sample_ticks (const Mp4Demux *d, guint i)
{
    return (gint64) (d->samples[i].pts * d->timescale) + d->samples[i].cts_off;
}

/* decoding can start at sample i: a keyframe by the file's sync sample
 * table (IDR pictures and, with open GOPs, recovery points), or - a file
 * without that table - a sample that holds an IDR picture */
static gboolean cv_sample_is_idr (const Mp4Demux *d, guint i);
static gboolean cv_sample_is_key (const Mp4Demux *d, guint i)
{
    return d->has_sync ? d->samples[i].sync != 0 : cv_sample_is_idr (d, i);
}

/* the demuxer keeps no sample bytes (it reads each sample when it is
 * asked for): the NAL unit headers of sample i are read from the file
 * here, 5 bytes each. Unreadable = not a keyframe. */
static gboolean cv_sample_is_idr (const Mp4Demux *d, guint i)
{
    const Mp4Sample *s = &d->samples[i];
    gsize off = 0;
    for (int n = 0; n < 64 && off + 5 <= (gsize) s->size; n++) {
        guint8 hd[5];
        ssize_t got;
        do
            got = pread (d->fd, hd, sizeof hd, (off_t) (s->off + off));
        while (got < 0 && errno == EINTR);
        if (got != (ssize_t) sizeof hd)
            return FALSE;
        guint32 len = ((guint32) hd[0] << 24) | ((guint32) hd[1] << 16) |
                      ((guint32) hd[2] << 8) | (guint32) hd[3];
        int type = hd[4] & 0x1f;
        if (type == 5)
            return TRUE;
        if (type == 1 || len == 0 || len > (gsize) s->size - off - 4)
            return FALSE;       /* a non-IDR slice, or a broken length */
        off += 4 + (gsize) len;
    }
    return FALSE;
}

/* Where to restart so that the frame stamped raw (stream seconds) comes
 * out again: the sample that carries it (*target), searched back from the
 * read position (it is at most the decoder's delay plus the queue
 * behind), then the keyframe at or before it (*cursor). FALSE: not found,
 * or the keyframe is more than max_back frames back. */
static gboolean cv_restart_point (CvSource *src, double raw, guint max_back, guint *cursor,
                                  guint *target)
{
    const Mp4Demux *d = src->demux;
    *target = 0;
    if (raw < 0 || d->nsamples == 0) {
        *cursor = 0;
        return TRUE;
    }
    gint64 want = (gint64) llround (raw * d->timescale);
    guint i = d->cursor, found = G_MAXUINT;
    for (int n = 0; n < 192 && n < (int) d->nsamples; n++) {
        i = i ? i - 1 : d->nsamples - 1;
        if (cv_sample_ticks (d, i) == want) {
            found = i;
            break;
        }
    }
    if (found == G_MAXUINT)
        return FALSE;
    *target = found;
    for (guint n = 0; n <= max_back; n++) {
        if (found < n)
            break;
        guint k = found - n;
        if (k == 0 || cv_sample_is_key (d, k)) {
            *cursor = k;
            return TRUE;
        }
    }
    return FALSE;
}

/* Every viewer has been paused for CV_PARK_S (src->lock held; dropped
 * while memory is freed): remember the frame on screen, then free the
 * queue, the decoder with its frame pool and the scratch buffers. A 4K
 * clip holds ~200 MB there, which a covered or
 * locked desktop does not need. Viewers keep their textures (the last
 * picture). */
static void cv_park (CvSource *src)
{
    double nd = 0;
    CvFrame *pick = cv_pick_frame (src, &nd);
    double raw = src->park_raw, play = src->park_play;
    if (pick) {
        raw = pick->raw;
        play = pick->pts;
    } else if (src->skip_until < 0 && !src->rebase) {
        raw = -1;               /* nothing was ever shown: from the start */
        play = 0;
    }
    guint cursor = 0, target = 0;
    double per_frame = (double) src->demux->width * src->demux->height * 2.5e-9 /
                       MAX (src->threads, 1);
    guint max_back = (guint) CLAMP (CV_PARK_WORK_S / per_frame, 30.0, (double) CV_PARK_CATCHUP);
    if (!cv_restart_point (src, raw, max_back, &cursor, &target)) {
        src->no_park = TRUE;    /* until the next pause */
        g_debug ("video: not parked (the keyframe before %.3f s is more than %u frames back)",
                 raw, max_back);
        return;
    }
    for (int i = 0; i < src->nq; i++)
        if (src->q[i]->pins > 0) {
            src->no_park = TRUE;
            return;
        }
    while (src->nq > 0)
        cv_frame_release (src, src->q[--src->nq]);
    src->has_frame = FALSE;
    src->parked = TRUE;
    src->park_raw = raw;
    src->park_play = play;
    src->park_cursor = cursor;
    src->skip_idx = target;
    g_mutex_unlock (&src->lock);

    for (int i = 0; i <= CV_AHEAD; i++) {
        g_free (src->buf[i].data);
        src->buf[i].data = NULL;
        src->buf[i].cap = 0;
    }
    av_frame_unref (src->frame);
    av_frame_unref (src->swframe);
    avcodec_free_context (&src->dec);
    cv_hw_close (src);
    g_clear_pointer (&src->half_buf, g_free);
    g_clear_pointer (&src->norm_buf, g_free);
    src->half_cap = src->norm_cap = 0;
#ifdef __GLIBC__
    malloc_trim (0);            /* freed heap pages back to the system */
#endif
    g_debug ("video: decoder parked at %.3f s, %u frame(s) after its keyframe (%s)", raw,
             target >= cursor ? target - cursor : 0, src->path);
    g_mutex_lock (&src->lock);
}

/* decode thread, no lock: continue at stream time raw (shown again as the
 * first frame, at presentation time play), decoding from sample cursor */
static void cv_restart_at (CvSource *src, double raw, double play, guint cursor)
{
    src->demux->cursor = MIN (cursor, src->demux->nsamples ? src->demux->nsamples - 1 : 0);
    src->sent_sps = FALSE;
    src->wrapped = FALSE;
    src->last_raw = -1;
    if (raw < 0) {
        src->skip_until = -1;
        src->rebase = play > 0;
    } else {
        src->skip_until = raw;
        src->rebase = TRUE;
    }
    src->rebase_play = play;
    src->rebase_raw = -1;
}

/* a viewer is active again: reopen the decoder where the clip stopped */
static gboolean cv_unpark (CvSource *src)
{
    GError *err = NULL;
    /* (the setting may have changed while the clip was parked) */
    if (src->hw_planned && cv_hw_stale (src))
        cv_hw_replan (src);
    if (!cv_decoder_open (src, FALSE, &err)) {
        g_warning ("video: %s: %s", src->path, err ? err->message : "decoder");
        g_clear_error (&err);
        return FALSE;
    }
    cv_restart_at (src, src->park_raw, src->park_play, src->park_cursor);
    g_mutex_lock (&src->lock);
    src->parked = FALSE;
    src->want_first = FALSE;
    src->clk_set = FALSE;       /* the first frame restarts the clock */
    g_mutex_unlock (&src->lock);
    g_debug ("video: decoder resumed at %.3f s from sample %u (%s)", src->park_raw,
             src->park_cursor, src->path);
    return TRUE;
}

/* The hardware decoder gave up, or refuses the stream (kind and why say
 * what happened): the next attempt of the plan when no picture of this
 * decoder was shown yet, software decoding otherwise and when no attempt
 * is left (video_hwsel.h) - from the keyframe before the last frame that
 * was queued, so that nothing is missing and nothing shows twice. The
 * viewers keep the picture they have meanwhile. */
static gboolean cv_hw_fallback (CvSource *src, WalpeHwWhy kind, const char *why)
{
    char name[24];
    g_strlcpy (name, src->hw_name ? src->hw_name : "?", sizeof name);
    gboolean more = walpe_hwsel_failed (&src->hw, kind, why);
    g_message ("video: hardware decoding (%s) failed for %s (%s: %s): %s", name, src->path,
               walpe_hwsel_why_name (kind), why,
               more ? "trying the next decoder" : "decoding on the CPU");
    double raw = src->last_raw, play = src->last_play;
    guint cursor = 0, target = 0;
    gboolean exact = cv_restart_point (src, raw, G_MAXUINT, &cursor, &target);
    src->skip_idx = target;
    av_frame_unref (src->frame);
    av_frame_unref (src->swframe);
    avcodec_free_context (&src->dec);
    cv_hw_close (src);
    GError *err = NULL;
    if (!cv_decoder_open (src, TRUE, &err)) {
        g_warning ("video: %s: %s", src->path, err ? err->message : "decoder");
        g_clear_error (&err);
        return FALSE;
    }
    if (exact && raw >= 0) {
        /* frames up to raw were shown already: continue after it */
        double iv = src->frame_interval > 0.001 ? src->frame_interval : 1.0 / 30.0;
        cv_restart_at (src, raw, play, cursor);
        src->skip_until = raw + iv * 0.5;
        src->rebase_play = play + iv;
        src->rebase_raw = raw;
    } else {
        cv_restart_at (src, -1, raw >= 0 ? play + 1.0 / 30.0 : 0, 0);
    }
    return TRUE;
}

/* The hardware decoding setting changed while the clip plays (walpe-ctl
 * set hwdec=..., the switch in the settings, video_zerocopy), or the GPU
 * that renders became known after the decoder was opened: make the plan
 * anew, reopen the decoder by it and continue after the last frame that
 * was queued (the frames since the last keyframe are decoded again: one
 * short stop). A decoder that cannot be opened in hardware goes on in
 * software (cv_decoder_open). A parked clip reads the setting when it
 * resumes. */
static gboolean cv_hw_switch (CvSource *src)
{
    if (!cv_hw_stale (src))
        return TRUE;
    WalpeHwWant want;
    cv_hw_want (&want);
    if ((!want.on && !src->hw_on) || (want.on && !src->gpu_yuv)) {
        /* nothing to reopen: software decodes and stays (software GL never
         * decodes in hardware); the status line follows the setting */
        cv_hw_replan (src);
        cv_note_decoder (src);
        return TRUE;
    }
    double raw = src->last_raw, play = src->last_play;
    guint cursor = 0, target = 0;
    gboolean exact = raw >= 0 && cv_restart_point (src, raw, G_MAXUINT, &cursor, &target);
    g_message ("video: hardware decoding is now \"%s\"%s for %s: the decoder is reopened",
               walpe_hwsel_want_name (&want), want.on && cv_zc_asked () ? " with zero-copy" : "",
               src->path);
    src->skip_idx = target;
    av_frame_unref (src->frame);
    av_frame_unref (src->swframe);
    avcodec_free_context (&src->dec);
    cv_hw_close (src);
    cv_hw_replan (src);     /* asked again: every attempt is tried again */
    GError *err = NULL;
    if (!cv_decoder_open (src, TRUE, &err)) {
        g_warning ("video: %s: %s", src->path, err ? err->message : "decoder");
        g_clear_error (&err);
        return FALSE;
    }
    if (exact) {
        /* frames up to raw were shown already: continue after it */
        double iv = src->frame_interval > 0.001 ? src->frame_interval : 1.0 / 30.0;
        cv_restart_at (src, raw, play, cursor);
        src->skip_until = raw + iv * 0.5;
        src->rebase_play = play + iv;
        src->rebase_raw = raw;
    } else {
        cv_restart_at (src, -1, raw >= 0 ? play + 1.0 / 30.0 : 0, 0);
    }
    return TRUE;
}

/* The decoder does not keep up with the clip on its present threads
 * (video_threads.h: three of four frames of the last two seconds were
 * queued after their presentation time) and may have more: note the
 * count it needs. The decode loop reopens the decoder where the next
 * sequence starts (cv_raise_here, cv_raise_now), so nothing is decoded
 * twice and the picture does not stop; a decoder that keeps up never
 * gets here. */
static void cv_threads_raise (CvSource *src)
{
    if (src->hw_on || src->raise_to > 0 || src->threads >= src->threads_max)
        return;
    double now = cv_now ();
    g_mutex_lock (&src->lock);
    guint frames = src->lag_frames, late = src->lag_late;
    g_mutex_unlock (&src->lock);
    int to = walpe_vlag_step (&src->lag, now, frames, late, cv_clip_fps (src->demux), src->threads,
                              src->threads_max);
    if (to <= src->threads)
        return;
    src->raise_to = to;
    src->raise_at = now;
    g_message ("video: the decoder fell behind with %d thread(s): %d from the next keyframe on (%s)",
               src->threads, to, src->path);
}

/* more threads are wanted: can the decoder be replaced before the sample
 * that is read next? Yes at the loop point and in front of an IDR picture
 * (nothing after it refers to anything before it, and everything before
 * it is shown before it). A stream whose keyframes are recovery points
 * instead (open GOPs) would wait for the loop point: after six seconds
 * such a keyframe is taken too (the two or three pictures that lean on
 * the sequence before it are dropped by the decoder). */
static gboolean cv_raise_here (const CvSource *src)
{
    const Mp4Demux *d = src->demux;
    if (d->cursor == 0 || d->cursor >= d->nsamples)
        return TRUE;
    if (d->has_sync && !d->samples[d->cursor].sync)
        return FALSE;
    if (cv_sample_is_idr (d, d->cursor))
        return TRUE;
    return d->has_sync && cv_now () - src->raise_at > 6.0;
}

/* the old decoder has given up its last frame: open the one with
 * src->raise_to threads. FALSE = no decoder any more. */
static gboolean cv_raise_now (CvSource *src)
{
    int to = src->raise_to;
    src->raise_to = 0;
    cv_threads_learn (cv_learn_key (src), to);
    src->threads = to;
    if (!src->dec)
        return TRUE;        /* parked meanwhile: cv_unpark opens it with the new count */
    av_frame_unref (src->frame);
    avcodec_free_context (&src->dec);
    GError *err = NULL;
    if (!cv_decoder_open (src, FALSE, &err)) {
        g_warning ("video: %s: %s", src->path, err ? err->message : "decoder");
        g_clear_error (&err);
        return FALSE;
    }
    src->sent_sps = FALSE;  /* the new decoder's first packet carries SPS / PPS */
    g_debug ("video: %d decode threads from sample %u on (%s)", to, src->demux->cursor, src->path);
    return TRUE;
}

/* ----------------------------------- played from a converted copy
 * (videotex.h, common/scenevideo.h) */

/* frame limit: the frame interval of the file that is played and its
 * first time stamp (source creation, and again when the file changes) */
static void cv_clip_times (CvSource *src)
{
    double fps = cv_clip_fps (src->demux);
    src->clip_iv = fps > 0 ? 1.0 / fps : 0;
    gint64 t0 = G_MAXINT64;
    for (guint i = 0; i < src->demux->nsamples; i++)
        t0 = MIN (t0, cv_sample_ticks (src->demux, i));
    src->clip_t0 = src->demux->nsamples && src->demux->timescale
        ? (double) t0 / (double) src->demux->timescale : 0;
}

/* src->lock held (or nobody else knows the source yet) */
static void cv_play_note (CvSource *src)
{
    src->play_w = src->demux->width;
    src->play_h = src->demux->height;
    src->play_fps = cv_clip_fps (src->demux);
}

/* the file the clip itself is in: the registry key without "@off+len" */
static char *cv_clip_file (const CvSource *src)
{
    const char *at = src->range_off || src->range_len ? strrchr (src->path, '@') : NULL;
    return at ? g_strndup (src->path, (gsize) (at - src->path)) : g_strdup (src->path);
}

/* Is the file behind `a` the clip of src again, only smaller: as many
 * pictures at the same rate, shown as stored, not larger? A copy made by
 * the daemon is; a file that is something else (cut short, of another
 * clip) would break the loop or the picture. *why (g_free) says what is
 * wrong. */
static gboolean cv_alt_fits (const CvSource *src, const Mp4Demux *a, char **why)
{
    const double fps = cv_clip_fps (a);
    *why = NULL;
    if (a->rotation != 0 || a->sar_num != a->sar_den)
        *why = g_strdup ("it is stored rotated or with pixels that are not square");
    else if (a->nsamples != src->orig_n)
        *why = g_strdup_printf ("it has %u pictures, the clip %u", a->nsamples, src->orig_n);
    else if (src->orig_fps > 0 && fabs (fps - src->orig_fps) > 0.005 * src->orig_fps)
        *why = g_strdup_printf ("it has %.3f pictures a second, the clip %.3f", fps, src->orig_fps);
    else if (a->width > src->orig_w || a->height > src->orig_h || a->width < 2 || a->height < 2)
        *why = g_strdup_printf ("it is %d x %d, the clip %d x %d", a->width, a->height,
                                src->orig_w, src->orig_h);
    return *why == NULL;
}

/* what the users of src ask for (src->lock held): the clip itself as soon
 * as one of them does, else the largest copy they name (NULL = the clip) */
static const char *cv_alt_wanted (const CvSource *src)
{
    const CvViewer *best = NULL;
    for (guint i = 0; i < src->viewers->len; i++) {
        const CvViewer *vw = g_ptr_array_index (src->viewers, i);
        if (vw->copy_state != WALPE_VTEX_COPY_READY || !vw->copy_path)
            return NULL;
        if (!best || (gint64) vw->copy_w * vw->copy_h > (gint64) best->copy_w * best->copy_h)
            best = vw;
    }
    return best ? best->copy_path : NULL;
}

/* one change of a played file is prepared at a time (its file is opened
 * under this lock; taken before a source's own) */
static GMutex cv_alt_lock;

/* A user of src said what it wants, or left: when the file that is played
 * (or is about to be) is not the one they ask for now, the other one is
 * opened here and handed to the decode thread, which changes over where
 * the clip starts again (cv_swap_now). Any thread that keeps src alive. */
static void cv_alt_update (CvSource *src)
{
    g_mutex_lock (&cv_alt_lock);
    g_mutex_lock (&src->lock);
    char *want = g_strdup (cv_alt_wanted (src));
    const gboolean pending = g_atomic_int_get (&src->alt_pending) != 0;
    gboolean same = !g_strcmp0 (want, pending ? src->alt_next : src->alt_path);
    if (!same && !g_strcmp0 (want, src->alt_path)) {
        /* a change that is not wanted any more */
        g_atomic_int_set (&src->alt_pending, 0);
        g_clear_pointer (&src->alt_demux, mp4_demux_free);
        g_clear_pointer (&src->alt_next, g_free);
        same = TRUE;
    }
    if (same && want && !g_strcmp0 (want, src->alt_path))
        g_clear_pointer (&src->alt_bad, g_free);
    g_mutex_unlock (&src->lock);
    if (!same) {
        GError *e = NULL;
        char *why = NULL, *file = want ? g_strdup (want) : cv_clip_file (src);
        Mp4Demux *d = want ? mp4_demux_open_range (want, 0, 0, &e)
                           : mp4_demux_open_range (file, src->range_off, src->range_len, &e);
        if (d && want && !cv_alt_fits (src, d, &why))
            g_clear_pointer (&d, mp4_demux_free);
        if (!d && !why)
            why = g_strdup (e ? e->message : "it cannot be read");
        g_clear_error (&e);
        const gboolean want_copy = want != NULL;
        char *bad = d ? NULL : g_strdup (why);
        g_mutex_lock (&src->lock);
        g_free (src->alt_bad);
        src->alt_bad = d ? NULL : g_steal_pointer (&why);
        if (d) {
            g_clear_pointer (&src->alt_demux, mp4_demux_free);
            g_free (src->alt_next);
            src->alt_demux = d;
            src->alt_next = g_steal_pointer (&want);
            g_atomic_int_set (&src->alt_pending, 1);
        }
        g_mutex_unlock (&src->lock);
        /* (src->alt_next and alt_bad are this thread's to read: they change
         * under cv_alt_lock, or go with the swap, which only frees them) */
        if (d && want_copy)
            g_message ("video: %s will be played from its converted copy %s where it starts again",
                       src->path, file);
        else if (d)
            g_message ("video: %s will be played as it is again where it starts again", src->path);
        else
            g_message ("video: %s is not played from %s: %s", src->path, file, bad ? bad : "?");
        g_free (file);
        g_free (why);
        g_free (bad);
    }
    g_free (want);
    g_mutex_unlock (&cv_alt_lock);
}

/* decode thread: is this the place to change files - the clip is at its
 * end (the next read would start it again), or has not begun */
static gboolean cv_swap_here (const CvSource *src)
{
    const Mp4Demux *d = src->demux;
    return src->skip_until < 0 && !src->rebase && (d->cursor >= d->nsamples || d->cursor == 0);
}

/* decode thread: the old decoder has given up its last picture; play the
 * file cv_alt_update prepared from its first picture on. That picture
 * follows the last one of the old file after one frame interval on the
 * same presentation clock, like the first picture of every other pass.
 * FALSE = no decoder any more. */
static gboolean cv_swap_now (CvSource *src)
{
    if (!src->dec)
        return TRUE;        /* parked meanwhile: after the resume, at the clip's end */
    g_mutex_lock (&src->lock);
    Mp4Demux *nd = g_steal_pointer (&src->alt_demux);
    char *np = g_steal_pointer (&src->alt_next);
    g_atomic_int_set (&src->alt_pending, 0);
    g_mutex_unlock (&src->lock);
    if (!nd) {
        /* the change was taken back meanwhile: the decoder, which was told
         * that its input had ended, takes the clip's next pass */
        g_free (np);
        avcodec_flush_buffers (src->dec);
        return TRUE;
    }
    const double iv = src->frame_interval > 0.001 ? src->frame_interval
                    : src->clip_iv > 0 ? src->clip_iv : 1.0 / 30.0;
    /* (frame limit: the pass ended with its last picture, also when that
     * one was left out) */
    const double play = src->last_play < 0 ? 0
        : src->last_play + iv + (src->last_raw >= 0 && src->drop_raw_max > src->last_raw
                                     ? src->drop_raw_max - src->last_raw : 0);
    const gboolean ended = src->demux->cursor >= src->demux->nsamples;
    av_frame_unref (src->frame);
    av_frame_unref (src->swframe);
    avcodec_free_context (&src->dec);
    cv_hw_close (src);
    Mp4Demux *old = src->demux;
    g_mutex_lock (&src->lock);
    src->demux = nd;
    g_free (src->alt_path);
    src->alt_path = np;
    cv_play_note (src);
    g_mutex_unlock (&src->lock);
    mp4_demux_free (old);
    cv_clip_times (src);
    g_free (src->hw_profile);
    src->hw_profile = walpe_h264_profile_text (src->demux->sps, src->demux->sps_len);
    src->threads = 0;       /* what this file needs (cv_decoder_open asks) */
    src->raise_to = 0;
    if (ended)
        g_atomic_int_inc (&src->passes);
    GError *err = NULL;
    if (!cv_decoder_open (src, FALSE, &err)) {
        g_warning ("video: %s: %s", src->path, err ? err->message : "decoder");
        g_clear_error (&err);
        return FALSE;
    }
    cv_restart_at (src, -1, play, 0);
    src->drop_raw_max = -1;
    g_message ("video: %s is played from %s now: %dx%d %.2f fps, %d decode thread(s)", src->path,
               src->alt_path ? src->alt_path : "the clip itself", src->demux->width,
               src->demux->height, cv_clip_fps (src->demux), src->dec->thread_count);
    return TRUE;
}

/* decode thread: what the decoder's pictures are, once (videotex.h
 * WalpeVideoTexInfo): a copy is made only of 8-bit 4:2:0 in limited range,
 * and is tagged with the matrix the pictures are converted with here */
static void cv_note_format (CvSource *src)
{
    const AVFrame *fr = src->frame;
    const guint mode = cv_frame_mode (fr);
    const gboolean plain = (fr->format == AV_PIX_FMT_YUV420P || fr->format == AV_PIX_FMT_NV12) &&
                           !(mode & WALPE_YUV_FULL);
    g_atomic_int_set (&src->pic_matrix, (mode & WALPE_YUV_BT2020) ? 9 : (mode & WALPE_YUV_BT709) ? 1 : 6);
    /* (a clip that is stored rotated: ffmpeg, told to leave the pictures as
     * they are stored, hands the display matrix on to the copy, and a copy
     * with one is never played - cv_alt_fits. Said here, before anything
     * is converted for nothing.) */
    g_atomic_int_set (&src->pic_format, !plain ? 2 : src->orig_rot ? 3 : 1);
}

/* decode thread: demux sample -> libavcodec -> frame queue
 * (cv_take_buffer paces it) */
static gpointer cv_decode_thread (gpointer data)
{
    CvSource *src = data;
    double log_at = cv_now ();
    guint log_decoded = 0;
    int hw_bad = 0;             /* send / receive failures in a row */
    int hw_err = 0;             /* the last error of the hardware decoder (AVERROR) */

    while (!g_atomic_int_get (&src->quit)) {
        g_mutex_lock (&src->lock);
        /* paused: sleep - but only once there is a picture. A video opened
         * on a paused monitor (walpe-ctl pause, covered, DPMS) decoded
         * nothing, so the monitor kept showing the previous wallpaper
         * until playback resumed. A parked source had its picture (the
         * viewers keep it in their textures); a viewer that joined since
         * and has none asks for it (want_first). */
        while (!src->quit && src->paused_all &&
               (src->has_frame || (src->parked && !src->want_first))) {
            if (!src->parked && !src->no_park && cv_park_s () > 0 && src->dec) {
                double until = src->pause_at + cv_park_s ();
                if (cv_now () >= until)
                    cv_park (src);
                else
                    g_cond_wait_until (&src->cond, &src->lock, (gint64) (until * 1e6) + 1);
            } else {
                g_cond_wait (&src->cond, &src->lock);
            }
        }
        gboolean quit = src->quit, parked = src->parked;
        /* frame limit: how many pictures a second the viewers show now
         * (0 = all of them; always 0 while the setting is off) */
        double keep = src->keep_rate = cv_keep_rate (src);
        g_mutex_unlock (&src->lock);
        if (quit)
            break;
        if (parked && !cv_unpark (src)) {
            g_usleep (500000);
            continue;
        }
        if (src->hw_on && (g_atomic_int_get (&src->hw_refused) || src->hw_errors >= 3 ||
                           hw_bad >= 8)) {
            /* what happened, as a kind of reason and in words; FFmpeg's own
             * words (its log on this thread) where it gave some */
            char why[420];
            const char *said = src->hw_log->len ? src->hw_log->str : NULL;
            int refused = g_atomic_int_get (&src->hw_refused);
            WalpeHwWhy kind = WALPE_HW_WHY_REFUSED;
            if (refused == 2) {
                kind = WALPE_HW_WHY_SIZE;
                g_snprintf (why, sizeof why, "a picture larger than the player takes (%d megapixels)",
                            CV_MAX_PIXELS / 1000000);
            } else if (refused == 3) {
                kind = WALPE_HW_WHY_PROFILE;
                g_snprintf (why, sizeof why, "the stream crops its pictures at the left or top edge, "
                            "which a picture in GPU memory does not carry");
            } else if (refused && !src->hw_own && src->hw_fmt_taken == 0) {
                const char *fn = av_get_pix_fmt_name ((enum AVPixelFormat) src->hw_sw_fmt);
                kind = WALPE_HW_WHY_PROFILE;
                g_snprintf (why, sizeof why, "FFmpeg has no hardware decoding for this stream's "
                            "format (%s)", fn ? fn : "?");
            } else if (refused) {
                kind = walpe_hwsel_classify (said, WALPE_HW_WHY_REFUSED);
                g_snprintf (why, sizeof why, "%s", said ? said : "the device does not decode this "
                            "stream");
            } else if (src->hw_errors >= 3) {
                kind = WALPE_HW_WHY_DOWNLOAD;
                g_snprintf (why, sizeof why, "%d pictures in a row could not be brought to main "
                            "memory: %s", src->hw_errors, av_err2str (hw_err));
            } else {
                kind = src->hw.seen ? WALPE_HW_WHY_DECODE
                                    : walpe_hwsel_classify (said, WALPE_HW_WHY_DECODE);
                g_snprintf (why, sizeof why, "%d decode calls in a row failed: %s%s%s", hw_bad,
                            av_err2str (hw_err), said ? "; FFmpeg: " : "", said ? said : "");
            }
            hw_bad = 0;
            if (!cv_hw_fallback (src, kind, why))
                break;
        }
        if (!cv_hw_switch (src))
            break;
        /* (a viewer changed what the status line says about zero-copy) */
        if (g_atomic_int_compare_and_exchange (&src->zc_dirty, 1, 0))
            cv_note_decoder (src);
        cv_log_to = src->hw_on ? src->hw_log : NULL;
        cv_threads_raise (src);
        /* more threads from here on: the decoder is told that its input
         * ended, the frames it still holds come out below as always, and
         * the new decoder takes the next sample */
        gboolean drain = src->raise_to > 0 && !src->hw_on && src->skip_until < 0 &&
                         cv_raise_here (src);
        /* another file from here on (a converted copy of the clip, or the
         * clip again): the decoder is drained the same way, then replaced
         * together with the demuxer (cv_swap_now) */
        const gboolean swap = !drain && g_atomic_int_get (&src->alt_pending) && cv_swap_here (src);
        /* (declared in front of the drain pass's jump: both ways into the
         * receive loop start without a picture) */
        gboolean got_frame = FALSE;
        if (src->raise_to > 0 && src->hw_on)
            src->raise_to = 0;      /* a hardware decoder decodes now */

        double c0 = cv_cpu ();
        const Mp4Sample *smp = drain || swap ? NULL : mp4_demux_next (src->demux);
        gboolean at_start = !src->sent_sps;
        if (drain || swap) {
            avcodec_send_packet (src->dec, NULL);
            goto receive;
        }
        if (!smp) {
            src->wrapped = TRUE; /* the next frame whose pts does not advance starts the new pass */
            g_atomic_int_inc (&src->passes);
            src->skip_until = -1;
            at_start = TRUE; /* loop wrap: re-inject SPS/PPS */
            smp = mp4_demux_next (src->demux);
            if (!smp)
                break;
        }
        /* frame limit: a picture that the monitors would not show and that
         * no other picture is predicted from is not decoded at all (not
         * while catching up after a restart: that has its own rule below) */
        if (keep > 0 && !at_start && src->skip_until < 0 && !src->rebase &&
            cv_sample_disposable (smp)) {
            double t = (double) ((gint64) (smp->pts * src->demux->timescale) + smp->cts_off) /
                       (double) src->demux->timescale;
            if (!cv_grid_keeps (t - src->clip_t0, src->clip_iv, keep)) {
                src->drop_raw_max = MAX (src->drop_raw_max, t);
                src->lim_skipped++;
                continue;
            }
        }

        /* AVCC sample -> Annex B: replace 4-byte NAL length prefixes with
         * 00 00 00 01 start codes; the first packet of each pass also
         * carries SPS/PPS (FFmpeg 8 rejects our hand-built AVCC extradata) */
        /* one Annex-B buffer reused for every packet (was a fresh
         * GString per frame) */
        GString *ab = src->annexb;
        g_string_truncate (ab, 0);
        static const guint8 sc4[4] = { 0, 0, 0, 1 };
        if (at_start) {
            g_string_append_len (ab, (const char *) sc4, 4);
            g_string_append_len (ab, (const char *) src->demux->sps, (gssize) src->demux->sps_len);
            g_string_append_len (ab, (const char *) sc4, 4);
            g_string_append_len (ab, (const char *) src->demux->pps, (gssize) src->demux->pps_len);
            src->sent_sps = TRUE;
        }
        gsize off = 0;
        while (off + 4 <= (gsize) smp->size) {
            guint32 nal_len = ((guint32) smp->data[off] << 24) |
                              ((guint32) smp->data[off + 1] << 16) |
                              ((guint32) smp->data[off + 2] << 8) |
                              (guint32) smp->data[off + 3];
            off += 4;
            if (nal_len == 0 || off + nal_len > (gsize) smp->size)
                break;
            g_string_append_len (ab, (const char *) sc4, 4);
            g_string_append_len (ab, (const char *) smp->data + off, (gssize) nal_len);
            off += nal_len;
        }
        src->pkt->data = (guint8 *) ab->str;
        src->pkt->size = (int) ab->len;
        /* true presentation time: decode stamp + composition offset (B-frames) */
        src->pkt->pts = (int64_t) (smp->pts * src->demux->timescale) + smp->cts_off;
        src->pkt->dts = src->pkt->pts;
        /* catching up to the frame that was on screen: pictures nothing
         * refers to (most B-frames) need not be decoded, except the last
         * few before it (the target itself may be one) */
        {
            guint idx = src->demux->cursor ? src->demux->cursor - 1 : 0;
            enum AVDiscard skip = src->skip_until >= 0 && idx + 8 + (guint) src->threads < src->skip_idx
                                      ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
            if (src->dec->skip_frame != skip)
                src->dec->skip_frame = skip;
        }
        double c1 = cv_cpu ();
        src->st_demux += c1 - c0;
        int sp_rc = avcodec_send_packet (src->dec, src->pkt);
        src->st_decode += cv_cpu () - c1;
        if (sp_rc < 0) {
            static atomic_int sp_err_n;
            if (walpe_first_n (&sp_err_n, 5))
                g_warning ("send_packet failed: %d (%s)", sp_rc, av_err2str (sp_rc));
            if (src->hw_on) {
                hw_bad++;
                hw_err = sp_rc;
                continue;       /* a few of them in a row: software decoding */
            }
            g_usleep (100000);
        }
        /* pictures beyond CV_MAX_PIXELS: the decoder allocates none of them
         * (max_pixels), so this clip will never show anything. Say so once
         * and stop reading it, instead of failing on every packet for as
         * long as the wallpaper is set. */
        if ((gint64) src->dec->coded_width * src->dec->coded_height > CV_MAX_PIXELS ||
            (gint64) src->dec->width * src->dec->height > CV_MAX_PIXELS) {
            g_warning ("video: %s holds %d x %d pictures, larger than the player takes "
                       "(%d megapixels): it shows nothing", src->path,
                       MAX (src->dec->coded_width, src->dec->width),
                       MAX (src->dec->coded_height, src->dec->height), CV_MAX_PIXELS / 1000000);
            {   /* (`walpe-ctl status`: "video_decoder") */
                char *note = g_strdup_printf ("%d x %d pictures: larger than the player takes "
                                              "(%d megapixels), not shown",
                                              MAX (src->dec->coded_width, src->dec->width),
                                              MAX (src->dec->coded_height, src->dec->height),
                                              CV_MAX_PIXELS / 1000000);
                walpe_video_note_decoder (note);
                g_free (note);
            }
            break;
        }
receive:
        while (!g_atomic_int_get (&src->quit)) {
            double c2 = cv_cpu ();
            int rf_rc = avcodec_receive_frame (src->dec, src->frame);
#ifdef CV_TEST_HW_RECEIVE
            /* tests: what the "hardware decoder" does with this picture */
            if (src->hw_on)
                rf_rc = CV_TEST_HW_RECEIVE (src, rf_rc);
#endif
            src->st_decode += cv_cpu () - c2;
            if (rf_rc < 0) {
                if (src->hw_on && rf_rc != AVERROR (EAGAIN) && rf_rc != AVERROR_EOF) {
                    hw_bad++;
                    hw_err = rf_rc;
                }
                break;
            }
            got_frame = TRUE;
            hw_bad = 0;
            if (src->hw_on) {
                const AVPixFmtDescriptor *pd =
                    av_pix_fmt_desc_get ((enum AVPixelFormat) src->frame->format);
                gboolean in_gpu = pd && (pd->flags & AV_PIX_FMT_FLAG_HWACCEL);
                src->zc_cur = 0;
                av_frame_unref (src->zc_hw);
                if (src->frame->width < 1 || src->frame->height < 1 ||
                    (gint64) src->frame->width * src->frame->height > CV_MAX_PIXELS) {
                    /* the size limit holds whoever decodes: libavcodec
                     * applies max_pixels to the frames it allocates, a
                     * decoder with buffers of its own may not ask it */
                    g_atomic_int_set (&src->hw_refused, 2);
                    av_frame_unref (src->frame);
                    break;
                }
                if (in_gpu && (src->frame->crop_left || src->frame->crop_top)) {
                    /* a picture in GPU memory comes with its right and
                     * bottom cut off and nothing else: such a stream would
                     * be shown shifted. The software decoder crops it. */
                    g_atomic_int_set (&src->hw_refused, 3);
                    av_frame_unref (src->frame);
                    break;
                }
                if (in_gpu) {
                    /* the picture is in GPU memory: download it (NV12 /
                     * P010), unless the viewers take it where it is */
                    double c3 = cv_cpu ();
                    int zc = cv_zc_mode (src);
                    if (zc) {
                        av_frame_unref (src->zc_hw);
                        if (av_frame_ref (src->zc_hw, src->frame) < 0)
                            zc = 0;
                    }
                    if (zc != 1) {
                        int rc = cv_hw_to_memory (src);
                        if (rc < 0) {
                            src->hw_errors++;
                            hw_err = rc;
                            av_frame_unref (src->zc_hw);
                            break;
                        }
                        src->hw_errors = 0;
                        src->st_copied += (double) src->frame->width * src->frame->height * 1.5;
                    }
                    src->zc_cur = zc;
                    src->st_prep += cv_cpu () - c3;
                } else if (!src->hw_own && !g_atomic_int_get (&src->hw_refused)) {
                    /* a hardware acceleration that hands out pictures in
                     * main memory is not decoding on the GPU: treat it as a
                     * refusal */
                    g_atomic_int_set (&src->hw_refused, 1);
                }
                if (in_gpu)
                    src->hw_gpu_pics = TRUE;
                if (in_gpu || src->hw_own) {
                    /* (what FFmpeg logged up to a good picture explains no
                     * later failure) */
                    g_string_truncate (src->hw_log, 0);
                    if (!src->hw.seen) {
                        /* the device really decodes this stream: say so */
                        walpe_hwsel_picture (&src->hw);
                        cv_note_decoder (src);
                    }
                }
            }
            if (!g_atomic_int_get (&src->pic_format))
                cv_note_format (src);
            /* our packet pts are in 1/timescale units and the decoder
             * may override avctx->time_base from SPS timing info —
             * always scale by OUR demuxer's timescale */
            double raw = src->frame->pts != AV_NOPTS_VALUE
                ? src->frame->pts / (double) src->demux->timescale : -1;
            double iv = src->frame_interval > 0.001 ? src->frame_interval
                      : keep > 0 ? src->clip_iv : 1.0 / 30.0;
            /* restarted at a keyframe (resume after a park): the frames
             * before the one that was on screen are decoded, not shown */
            if (src->skip_until >= 0) {
                if (raw >= 0 && raw < src->skip_until - 1e-6) {
                    av_frame_unref (src->frame);
                    continue;
                }
                src->skip_until = -1;
            }
            /* frame limit: other pictures are predicted from this one, so it
             * had to be decoded, but the monitors do not show it */
            if (keep > 0 && raw >= 0 && !cv_grid_keeps (raw - src->clip_t0, src->clip_iv, keep)) {
                src->drop_raw_max = MAX (src->drop_raw_max, raw);
                src->lim_unshown++;
                av_frame_unref (src->frame);
                continue;
            }
            if (src->rebase) {
                /* (frame limit: the first picture queued after a restart
                 * can be several of the clip's pictures after the last one
                 * queued before it) */
                if (keep > 0 && raw >= 0 && src->rebase_raw >= 0 &&
                    raw - src->rebase_raw > iv * 1.5)
                    src->rebase_play += raw - src->rebase_raw - iv;
                /* the first frame after a restart continues the old
                 * presentation times */
                src->loop_off = src->rebase_play - (raw >= 0 ? raw : 0);
                src->last_play = src->rebase_play - iv;
                src->last_raw = -1;
                src->rebase = FALSE;
            }
            /* presentation time: the stream's pts, continued across loop
             * wraps (the first frame of the next pass follows the last
             * one after one frame interval) */
            double play;
            if (raw < 0) {
                play = src->last_play + iv;
            } else {
                /* a new pass: the pts fell back by more than 0.25 s, or -
                 * after the demuxer wrapped - did not advance at all.
                 * (The pts test alone missed clips of 0.25 s or less: 1-8
                 * frames. Every later frame was then stamped 1 ms after
                 * the previous one and the clip ran at hundreds of fps
                 * with the decoder flat out.) */
                if (src->last_raw >= 0 && (raw < src->last_raw - 0.25 ||
                                           (src->wrapped && raw <= src->last_raw))) {
                    /* (frame limit: the pass ended with its last picture,
                     * also when that one was left out) */
                    src->loop_off += MAX (src->last_raw, src->drop_raw_max) + iv - raw;
                    src->drop_raw_max = -1;
                    src->wrapped = FALSE;
                }
                play = raw + src->loop_off;
            }
            if (play <= src->last_play)
                play = src->last_play + iv;

            gboolean park = FALSE;
            CvFrame *f = cv_take_buffer (src, &park);
            if (!f) {
                av_frame_unref (src->frame);
                break;
            }
            g_mutex_lock (&src->lock);
            gboolean half = cv_half_ok (src, src->frame->width, src->frame->height);
            g_mutex_unlock (&src->lock);
            if (half != src->half)
                g_debug ("video: %s decoded frames for %s", half ? "half-size" : "full-size",
                         src->path);
            src->half = half;
            double c4 = cv_cpu (), copied = 0;
            static WalpeLazy test_nv_cell = WALPE_LAZY_UNSET;
            int test_nv = walpe_lazy_get (&test_nv_cell);
            if (test_nv < 0)
                test_nv = walpe_lazy_set (&test_nv_cell, g_getenv ("WALPE_VIDEO_TEST_NV12") ? 1 : 0);
            gboolean conv;
            /* zero-copy: 1 = f shows the decoder's picture where it is
             * (nothing to fill); 2 = it can, and the download in src->frame
             * is filled in as always for the comparison - at full size */
            int zc = src->hw_on ? cv_zc_take (src, f) : 0;
            if (zc == 1) {
                conv = TRUE;
            } else if (zc < 0) {
                conv = FALSE;
                src->hw_errors++;
            } else {
                gboolean keep_half = src->half;
                if (zc)
                    src->half = FALSE;
                if (test_nv)
                    cv_test_nv12 (src);
                conv = cv_fill_frame (src, f, &copied);
                src->half = keep_half;
                if (zc && conv) {
                    f->zc = 2;
                } else if (zc) {
                    av_frame_unref (f->drm);
                    av_frame_unref (f->hw);
                }
            }
            cv_stage_written (src, f);
            src->st_prep += cv_cpu () - c4;
            src->st_copied += copied;
            gboolean dropped = FALSE;

            void (*cbs[MAX_VIEWER_SNAP]) (gpointer);
            gpointer usr[MAX_VIEWER_SNAP];
            int n = 0;
            g_mutex_lock (&src->lock);
            if (conv) {
                f->pts = play;
                f->raw = raw;
                cv_push_frame (src, f);
#ifdef CV_TEST_PUSHED
                CV_TEST_PUSHED (src, f);    /* tests/video_test.c records every queued frame */
#endif
                if (f->seq <= (guint64) cv_md5_n ())
                    cv_frame_md5 (f, "push", src->path);
            } else {
                cv_frame_release (src, f);
                dropped = TRUE;
            }
            /* the pts cadence: viewers schedule a frame that is not decoded
             * yet by it, scenes pace video-only animation by it */
            if (raw >= 0) {
                if (src->last_raw >= 0) {
                    double d = raw - src->last_raw;
                    /* (frame limit: queued pictures are further apart than
                     * the clip's pictures; the interval stays the clip's) */
                    if (d > 0.001 && d < 1.0 && !(keep > 0))
                        src->frame_interval = src->frame_interval > 0
                            ? src->frame_interval * 0.8 + d * 0.2 : d;
                    static atomic_int dlog_n;
                    if (walpe_first_n (&dlog_n, 4))
                        g_message ("video: pts delta %.4fs (frame pts %.3f, tb %d/%d, ts %u, has_bf %d)",
                                   d, raw, src->dec->time_base.num,
                                   src->dec->time_base.den, src->demux->timescale,
                                   src->dec->has_b_frames);
                }
                if (keep > 0 && src->frame_interval <= 0.001)
                    src->frame_interval = src->clip_iv;
                src->last_raw = raw;
                src->last_push_pts = raw;
            }
            src->last_play = play;
            /* wake the viewers that have nothing to wait for: all of them
             * for the first frames, later those the decoder kept waiting */
            for (guint i = 0; conv && i < src->viewers->len && n < MAX_VIEWER_SNAP; i++) {
                CvViewer *vw = g_ptr_array_index (src->viewers, i);
                if ((vw->starved || src->seq <= 2) && vw->redraw_cb) {
                    cbs[n] = vw->redraw_cb;
                    usr[n] = vw->redraw_user;
                    n++;
                }
                vw->starved = FALSE;
            }
            /* WALPE_FPS_LOG: decoded and shown frames per second, CPU per stage */
            char *line = NULL;
            double lg = cv_fps_log_s (), now = cv_now ();
            if (lg > 0 && now - log_at >= lg) {
                GString *ls = g_string_new (NULL);
                guint nfr = MAX (src->decoded - log_decoded, 1);
                g_string_append_printf (ls, "fps: video %s: decoded %.1f fps, clip %.2f fps",
                                        src->path, (src->decoded - log_decoded) / (now - log_at),
                                        1.0 / iv);
                for (guint i = 0; i < src->viewers->len; i++) {
                    CvViewer *vw = g_ptr_array_index (src->viewers, i);
                    g_string_append_printf (ls, "; viewer %u showed %.1f fps%s (%u skipped), "
                                            "upload %.3f ms + %.2f MB per frame, draw %.3f ms",
                                            i, vw->shown / (now - log_at),
                                            vw->paused ? " (paused)" : "", vw->skipped,
                                            vw->st_upload * 1e3 / MAX (vw->shown, 1),
                                            vw->st_bytes / 1048576.0 / MAX (vw->shown, 1),
                                            vw->st_draw * 1e3 / MAX (vw->st_draws, 1));
                    /* (the upload counters themselves are the render
                     * thread's: this is what it handed over, up_log) */
                    g_string_append_printf (ls, ", upload path %s%s: %u texture and %u buffer "
                                            "calls, %u maps, %u fences, %.2f MB copied by the render "
                                            "thread, %u of %u frames from buffers the decoder wrote",
                                            cv_up_name (vw->up_log.real),
                                            vw->up_log.mode > 0 &&
                                            (vw->up_log.mode & WALPE_VUP_PINGPONG) ? "+pingpong" : "",
                                            vw->up_log.tex_calls, vw->up_log.buf_calls,
                                            vw->up_log.maps, vw->up_log.fences,
                                            vw->up_log.copied / 1048576.0, vw->up_log.staged,
                                            vw->up_log.frames);
                    vw->up_log.frames = vw->up_log.tex_calls = vw->up_log.buf_calls = 0;
                    vw->up_log.maps = vw->up_log.fences = vw->up_log.staged = vw->up_log.waits = 0;
                    vw->up_log.copied = 0;
                    vw->shown = vw->skipped = 0;
                    vw->st_upload = vw->st_draw = vw->st_bytes = 0;
                    vw->st_draws = 0;
                }
                int nthr = 0;
                double wk = cv_workers_cpu (&nthr);
                /* (threads of a reopened decoder start from zero) */
                double wd = wk >= src->st_workers ? wk - src->st_workers : wk;
                g_string_append_printf (ls, "; cpu ms per frame: packets %.3f, decode %.3f + "
                                        "%.3f in %d worker thread(s)%s%s, frame hand-over %.3f "
                                        "(%.2f MB copied)",
                                        src->st_demux * 1e3 / nfr, src->st_decode * 1e3 / nfr,
                                        wd * 1e3 / nfr, nthr,
                                        src->hw_on ? ", hardware " : "",
                                        src->hw_on ? src->hw_name : "",
                                        src->st_prep * 1e3 / nfr,
                                        src->st_copied / 1048576.0 / nfr);
                src->st_workers = wk;
                src->st_demux = src->st_decode = src->st_prep = src->st_copied = 0;
                if (src->hw_on && src->dl_reused + src->dl_fresh > 0)
                    g_string_append_printf (ls, "; downloads into the kept frame %u, into a new "
                                            "one %u", src->dl_reused, src->dl_fresh);
                src->dl_reused = src->dl_fresh = 0;
                if (src->zc_sent > 0)
                    g_string_append_printf (ls, "; %u pictures handed over without a copy "
                                            "(zero-copy)", src->zc_sent);
                src->zc_sent = 0;
                if (keep > 0 || src->lim_skipped + src->lim_unshown > 0)
                    g_string_append_printf (ls, "; frame limit: %.2f pictures a second wanted, %u "
                                            "not decoded, %u decoded and not shown", keep,
                                            src->lim_skipped, src->lim_unshown);
                src->lim_skipped = src->lim_unshown = 0;
                g_string_append_printf (ls, "; pass %u", (guint) g_atomic_int_get (&src->passes) + 1);
                if (src->slipped > 0)
                    g_string_append_printf (ls, "; decoder behind by %.0f ms in total (too slow "
                                            "for this clip)", src->slipped * 1e3);
                log_at = now;
                log_decoded = src->decoded;
                line = g_string_free (ls, FALSE);
            }
            g_mutex_unlock (&src->lock);
            for (int i = 0; i < n; i++)
                cbs[i] (usr[i]);
            if (line) {
                g_message ("%s", line);
                g_free (line);
            }
            av_frame_unref (src->frame);
            /* a frame that could not be shown: the queue (the decoder's
             * only pacing) did not take it, so wait its interval here
             * instead of decoding the clip in a tight loop */
            if (dropped && !g_atomic_int_get (&src->quit))
                g_usleep ((gulong) (CLAMP (iv, 0.001, 0.5) * 1e6));
        }
        if (drain && !g_atomic_int_get (&src->quit) && !cv_raise_now (src))
            break;
        if (swap && !g_atomic_int_get (&src->quit) && !cv_swap_now (src))
            break;
        if (swap)
            continue;       /* (a decoder without a picture yet is not a barren stream) */
        /* a stream the decoder takes without ever giving a picture (e.g.
         * predicted frames only, no keyframe: nothing is output and nothing
         * fails): the frame queue is this loop's only pacing, so it read
         * and decoded the clip over and over as fast as a core could, for
         * as long as the wallpaper was set. After CV_BARREN packets in a
         * row without a picture (reordering and frame threads hold back
         * far fewer) each further one waits a frame interval. */
        if (got_frame) {
            src->barren = 0;
        } else if (++src->barren > CV_BARREN && !g_atomic_int_get (&src->quit)) {
            double iv = src->frame_interval > 0.001 ? src->frame_interval : 1.0 / 30.0;
            if (src->barren == CV_BARREN + 1)
                g_warning ("video: %s: no picture from the last %d packets (damaged or "
                           "unsupported stream?); still trying, at the clip's own pace",
                           src->path, CV_BARREN);
            g_usleep ((gulong) (CLAMP (iv, 0.001, 0.5) * 1e6));
        }
    }
    return NULL;
}

/* recompute whether every viewer is paused (call with src->lock held) */
static void cv_recompute_paused (CvSource *src)
{
    gboolean all = TRUE;
    for (guint i = 0; i < src->viewers->len; i++) {
        CvViewer *vw = g_ptr_array_index (src->viewers, i);
        if (!vw->paused) {
            all = FALSE;
            break;
        }
    }
    /* the presentation clock stops with the last viewer and continues
     * where it stopped */
    if (all && !src->paused_all) {
        src->pause_at = cv_now ();
        src->no_park = FALSE;
    } else if (!all && src->paused_all && src->pause_at > 0) {
        if (src->clk_set)
            src->clk += cv_now () - src->pause_at;
        src->pause_at = 0;
    }
    src->paused_all = all;
    g_cond_broadcast (&src->cond);
}

/* ------------------------------------------------------ viewer (GL) */

/* planes -> RGB in a shader of the module's own (per thread = per EGL
 * context). The integer coefficients of yuvconv.c (walpe_yuv_coeffs:
 * BT.601, BT.709 or BT.2020, limited or full range), so the GPU path
 * matches the CPU converter; u_s turns a texture value into 8-bit code
 * units whatever the sample depth is. Variant 1 reads U and V from one
 * two-component texture (NV12, P010). */
typedef struct {
    GLuint prog;
    GLint mvp, ky, kc, sc, t[3];
    GLint cy, cc;               /* zero-copy variant: where the picture is on its surface */
} CvProg;
/* [0] three planes, [1] Y + UV; [2], [3] the same for planes that are the
 * decoder's surface (zero-copy) */
static __thread CvProg cv_prog[4];
static __thread GLuint cv_vao, cv_vbo;

static const char *CV_VS =
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "uniform mat3 u_mvp;\n"
    "out vec2 v_uv;\n"
    "void main(){ v_uv = a_uv;\n"
    "  vec2 p = (u_mvp * vec3(a_pos, 1.0)).xy;\n"
    "  gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char *CV_FS =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_y, u_u, u_v;\n"
    "uniform vec2 u_ky;\n"      /* Y' gain, Y' offset */
    "uniform vec4 u_kc;\n"      /* R from V, G from U, G from V, B from U */
    "uniform float u_s;\n"      /* texture value -> 8-bit code units */
    "out vec4 frag;\n"
    "void main(){\n"
    "  float Y = max(texture(u_y, v_uv).r * u_s - u_ky.y, 0.0) * u_ky.x;\n"
    "#ifdef CV_NV\n"
    "  vec2 uv = texture(u_u, v_uv).rg * u_s - 128.0;\n"
    "#else\n"
    "  vec2 uv = vec2(texture(u_u, v_uv).r, texture(u_v, v_uv).r) * u_s - 128.0;\n"
    "#endif\n"
    "  float U = uv.x;\n"
    "  float V = uv.y;\n"
    "  vec3 c = vec3(Y + u_kc.x * V, Y - u_kc.y * U - u_kc.z * V, Y + u_kc.w * U)\n"
    "           / 256.0 / 255.0;\n"
    "  frag = vec4(clamp(c, 0.0, 1.0), 1.0); }\n";

/* The same conversion for planes that are the decoder's own surface
 * (zero-copy): the picture is the top left part of a surface that is a few
 * rows and columns larger (1920x1080 lies on 1920x1088), so the texture
 * coordinates are scaled to the picture and held at the centre of its last
 * sample - what CLAMP_TO_EDGE does on a texture of the picture's own size.
 * Everything after the three texture reads is CV_FS, word for word; the
 * shader of uploaded pictures (CV_FS) is not touched by this path. */
static const char *CV_FS_ZC =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_y, u_u, u_v;\n"
    "uniform vec2 u_ky;\n"
    "uniform vec4 u_kc;\n"
    "uniform float u_s;\n"
    "uniform vec4 u_cy;\n"      /* luma plane: scale x, y; largest x, y */
    "uniform vec4 u_cc;\n"      /* chroma planes: the same */
    "out vec4 frag;\n"
    "void main(){\n"
    "  vec2 ty = min(v_uv * u_cy.xy, u_cy.zw);\n"
    "  vec2 tc = min(v_uv * u_cc.xy, u_cc.zw);\n"
    "  float Y = max(texture(u_y, ty).r * u_s - u_ky.y, 0.0) * u_ky.x;\n"
    "#ifdef CV_NV\n"
    "  vec2 uv = texture(u_u, tc).rg * u_s - 128.0;\n"
    "#else\n"
    "  vec2 uv = vec2(texture(u_u, tc).r, texture(u_v, tc).r) * u_s - 128.0;\n"
    "#endif\n"
    "  float U = uv.x;\n"
    "  float V = uv.y;\n"
    "  vec3 c = vec3(Y + u_kc.x * V, Y - u_kc.y * U - u_kc.z * V, Y + u_kc.w * U)\n"
    "           / 256.0 / 255.0;\n"
    "  frag = vec4(clamp(c, 0.0, 1.0), 1.0); }\n";

static GLuint cv_compile (GLenum type, const char *defs, const char *body)
{
    const char *hdr = epoxy_is_desktop_gl () ? "#version 330\n"
                    : type == GL_FRAGMENT_SHADER ? "#version 300 es\nprecision highp float;\n"
                                                 : "#version 300 es\n";
    const char *srcs[3] = { hdr, defs, body };
    GLuint s = glCreateShader (type);
    glShaderSource (s, 3, srcs, NULL);
    glCompileShader (s);
    GLint ok = 0;
    glGetShaderiv (s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog (s, sizeof (log), NULL, log);
        g_critical ("video shader compile failed: %s", log);
    }
    return s;
}

static CvProg *cv_program (int kind, gboolean surface)
{
    CvProg *p = &cv_prog[(kind == CV_NV ? 1 : 0) + (surface ? 2 : 0)];
    if (p->prog)
        return p;
    GLuint vs = cv_compile (GL_VERTEX_SHADER, "", CV_VS);
    GLuint fs = cv_compile (GL_FRAGMENT_SHADER, kind == CV_NV ? "#define CV_NV 1\n" : "",
                            surface ? CV_FS_ZC : CV_FS);
    p->prog = glCreateProgram ();
    glAttachShader (p->prog, vs);
    glAttachShader (p->prog, fs);
    glBindAttribLocation (p->prog, 0, "a_pos");
    glBindAttribLocation (p->prog, 1, "a_uv");
    glLinkProgram (p->prog);
    glDetachShader (p->prog, vs);
    glDetachShader (p->prog, fs);
    glDeleteShader (vs);
    glDeleteShader (fs);
    p->mvp = glGetUniformLocation (p->prog, "u_mvp");
    p->t[0] = glGetUniformLocation (p->prog, "u_y");
    p->t[1] = glGetUniformLocation (p->prog, "u_u");
    p->t[2] = glGetUniformLocation (p->prog, "u_v");
    p->ky = glGetUniformLocation (p->prog, "u_ky");
    p->kc = glGetUniformLocation (p->prog, "u_kc");
    p->sc = glGetUniformLocation (p->prog, "u_s");
    p->cy = surface ? glGetUniformLocation (p->prog, "u_cy") : -1;
    p->cc = surface ? glGetUniformLocation (p->prog, "u_cc") : -1;
    if (!cv_vao) {
        /* strip quad: pos(2) uv(2); v=0 at clip top, as glutil's */
        static const float verts[4][4] = {
            { -1.0f, -1.0f, 0.0f, 1.0f },
            {  1.0f, -1.0f, 1.0f, 1.0f },
            { -1.0f,  1.0f, 0.0f, 0.0f },
            {  1.0f,  1.0f, 1.0f, 0.0f },
        };
        glGenVertexArrays (1, &cv_vao);
        glBindVertexArray (cv_vao);
        glGenBuffers (1, &cv_vbo);
        glBindBuffer (GL_ARRAY_BUFFER, cv_vbo);
        glBufferData (GL_ARRAY_BUFFER, sizeof (verts), verts, GL_STATIC_DRAW);
        glEnableVertexAttribArray (0);
        glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 16, (void *) 0);
        glEnableVertexAttribArray (1);
        glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 16, (void *) 8);
        glBindVertexArray (0);
    }
    return p;
}

/* draw the viewer's plane textures as opaque RGB */
static void cv_draw_planes (CvViewer *vw, const float *mvp)
{
    /* (zero-copy: the planes are the decoder's surface, vw->ztex) */
    const GLuint *planes = vw->zc_on ? vw->ztex : vw->ptex;
    CvProg *p = cv_program (vw->pkind, vw->zc_on);
    int n = vw->pkind == CV_NV ? 2 : 3;
    glUseProgram (p->prog);
    if (vw->zc_on) {
        glUniform4fv (p->cy, 1, vw->zcrop[0]);
        glUniform4fv (p->cc, 1, vw->zcrop[1]);
    }
    int k[6];
    walpe_yuv_coeffs (vw->ymode, k);
    glUniform2f (p->ky, (float) k[0], (float) k[1]);
    glUniform4f (p->kc, (float) k[2], (float) k[3], (float) k[4], (float) k[5]);
    glUniform1f (p->sc, vw->pscale);
    for (int i = 0; i < n; i++) {
        glActiveTexture (GL_TEXTURE0 + (GLenum) i);
        glBindTexture (GL_TEXTURE_2D, planes[i]);
        glUniform1i (p->t[i], i);
    }
    glActiveTexture (GL_TEXTURE0);
    glUniformMatrix3fv (p->mvp, 1, GL_FALSE, mvp);
    glBlendFunc (GL_ONE, GL_ZERO);
    glBindVertexArray (cv_vao);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray (0);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

/* --------------------------------------- hardware: zero-copy, viewer side
 * ("video_zerocopy", video.h). A hardware picture is not downloaded and
 * uploaded: each of its planes, where the decoder left it, becomes a
 * texture (a DMA-BUF imported as an EGL image, EGL_EXT_image_dma_buf_import
 * with modifiers where the display takes them), and the planes are drawn by
 * the conversion shader of the upload path. The driver converts nothing:
 * formats (NV12 as R8 + GR88, P010 as R16 + GR1616, three 8-bit planes),
 * range and matrix are exactly the upload path's (video_hwsel.h has the
 * table). GL_TEXTURE_EXTERNAL_OES is not used: there the driver would
 * choose the conversion, and desktop GL has no such sampler.
 *
 * The picture equals the software path's as soon as the imported textures
 * hold the decoder's samples. That is checked: the first pictures after
 * every decoder start come with their download, and the imported planes
 * are read back and compared with it, sample for sample (WALPE_ZC_*). A
 * difference, or anything that cannot be imported or read back, refuses
 * the stream: it is downloaded from then on. Nothing of this could be run
 * where it was written (no GPU, no VA-API device). */

/* render thread, its context current: can this GL context draw from DMA-BUFs */
static void cv_zc_caps (CvViewer *vw)
{
    int how = 0;
    gboolean mods = FALSE;
    const char *cant = NULL;
    EGLDisplay dpy = eglGetCurrentDisplay ();
    if (dpy == EGL_NO_DISPLAY)
        cant = "the renderer's GL context is not an EGL context";
    else if (walpe_gl_is_software ())
        cant = "the renderer is a software renderer";
    else if (!epoxy_has_egl_extension (dpy, "EGL_KHR_image_base") ||
             !epoxy_has_egl_extension (dpy, "EGL_EXT_image_dma_buf_import"))
        cant = "the renderer's EGL has no EGL_EXT_image_dma_buf_import";
    else if (epoxy_has_gl_extension ("GL_OES_EGL_image"))
        how = 1;
    else if (epoxy_has_gl_extension ("GL_EXT_EGL_image_storage"))
        how = 2;
    else
        cant = "the renderer's GL has neither GL_OES_EGL_image nor GL_EXT_EGL_image_storage";
    if (how)
        mods = epoxy_has_egl_extension (dpy, "EGL_EXT_image_dma_buf_import_modifiers");
    g_mutex_lock (&vw->src->lock);
    vw->zc_can = how ? 1 : 0;
    vw->zc_cant = cant;
    vw->zc_how = how;
    vw->zc_mods = mods;
    g_mutex_unlock (&vw->src->lock);
}

/* Render thread, its context current: what hardware decoding needs to
 * know from the renderer - which device this context renders on and
 * whether it can draw from the decoder's own memory. Questions only, and
 * asked only once hardware decoding is asked for: with the setting off
 * (the default) the renderer is never asked anything. */
static void cv_hw_render_side (CvViewer *vw)
{
    if (vw->zc_can >= 0 || !cv_hw_is_asked ())
        return;         /* (zc_can is written by this thread only) */
    cv_gpu_probe ();
    cv_zc_caps (vw);
}

static void cv_zc_discard (GLuint *tex, EGLImageKHR *img)
{
    /* (EGL is asked only when there is an image to destroy: a viewer that
     * never drew from a decoder's picture - every viewer while hardware
     * decoding is off - makes no EGL call here) */
    EGLDisplay dpy = EGL_NO_DISPLAY;
    gboolean asked = FALSE;
    for (int i = 0; i < 3; i++) {
        if (tex[i])
            glDeleteTextures (1, &tex[i]);
        tex[i] = 0;
        if (img[i] != EGL_NO_IMAGE_KHR) {
            if (!asked) {
                dpy = eglGetCurrentDisplay ();
                asked = TRUE;
            }
            if (dpy != EGL_NO_DISPLAY)
                eglDestroyImageKHR (dpy, img[i]);
        }
        img[i] = EGL_NO_IMAGE_KHR;
    }
}

/* render thread, its context current: the viewer lets go of the decoder's
 * picture (uploaded planes follow, or its GL side ends) */
static void cv_zc_drop (CvViewer *vw)
{
    cv_zc_discard (vw->ztex, vw->zimg);
    av_frame_free (&vw->zhold);
    vw->zc_on = FALSE;
}

/* Render thread, f pinned: the planes of f as textures tex[] behind the
 * EGL images img[], without a copy. FALSE (*why, a constant): nothing was
 * made. */
static gboolean cv_zc_import (CvViewer *vw, const CvFrame *f, GLuint *tex, EGLImageKHR *img,
                              const char **why)
{
    EGLDisplay dpy = eglGetCurrentDisplay ();
    gboolean ok = dpy != EGL_NO_DISPLAY && vw->zc_how && f->zc_n >= 2 && f->zc_n <= 3;
    for (int i = 0; i < 3; i++) {
        tex[i] = 0;
        img[i] = EGL_NO_IMAGE_KHR;
    }
    *why = "EGL does not import the decoder's picture";
    while (glGetError () != GL_NO_ERROR)
        ;
    for (int i = 0; ok && i < f->zc_n; i++) {
        const WalpeDmaPlane *p = &f->zc_pl[i];
        EGLint at[24];
        int k = 0;
        if (p->object < 0 || p->object >= WALPE_DMA_MAX || f->zc_fd[p->object] < 0) {
            ok = FALSE;
            break;
        }
        at[k++] = EGL_WIDTH;
        at[k++] = p->w;
        at[k++] = EGL_HEIGHT;
        at[k++] = p->h;
        at[k++] = EGL_LINUX_DRM_FOURCC_EXT;
        at[k++] = (EGLint) p->fourcc;
        at[k++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        at[k++] = f->zc_fd[p->object];
        at[k++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        at[k++] = (EGLint) p->offset;
        at[k++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        at[k++] = (EGLint) p->pitch;
        if (p->modifier != WALPE_DRM_MOD_INVALID) {
            if (vw->zc_mods) {
                at[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
                at[k++] = (EGLint) (guint32) (p->modifier & 0xffffffffu);
                at[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
                at[k++] = (EGLint) (guint32) (p->modifier >> 32);
            } else if (p->modifier != WALPE_DRM_MOD_LINEAR) {
                *why = "the picture's memory layout needs a format modifier and the renderer's EGL "
                       "takes none (no EGL_EXT_image_dma_buf_import_modifiers)";
                ok = FALSE;
                break;
            }
        }
        at[k++] = EGL_NONE;
        img[i] = eglCreateImageKHR (dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer) NULL, at);
        if (img[i] == EGL_NO_IMAGE_KHR) {
            eglGetError ();     /* (taken off the thread: the reason is the refusal itself) */
            ok = FALSE;
            break;
        }
        glGenTextures (1, &tex[i]);
        glBindTexture (GL_TEXTURE_2D, tex[i]);
        if (vw->zc_how == 1)
            glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, (GLeglImageOES) img[i]);
        else
            glEGLImageTargetTexStorageEXT (GL_TEXTURE_2D, (GLeglImageOES) img[i], NULL);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (glGetError () != GL_NO_ERROR) {
            *why = "GL does not take the imported picture as a texture";
            ok = FALSE;
        }
    }
    if (!ok)
        cv_zc_discard (tex, img);
    return ok;
}

/* Render thread, f pinned, f->zc == 2 (its planes are in main memory too):
 * are the imported planes tex[] the decoder's samples? Each is read back
 * through a framebuffer, in strips, and compared with the download over
 * the picture's area - WALPE_ZC_TOLERANCE, i.e. no difference at all.
 * FALSE (*why): a difference, or it could not be read. The framebuffer,
 * pack buffer and pack alignment bindings are put back. */
static gboolean cv_zc_verify (CvViewer *vw, const CvFrame *f, const GLuint *tex, const char **why)
{
    int n = f->kind == CV_NV ? 2 : 3;
    if (f->kind == CV_BGRA || n != f->zc_n || !f->pl[0] || (f->bytes != 1 && f->bytes != 2)) {
        *why = "the downloaded picture has another layout than the exported one";
        return FALSE;
    }
    GLint prev_fbo = 0, prev_pack = 4, prev_pbo = 0;
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv (GL_PACK_ALIGNMENT, &prev_pack);
    glGetIntegerv (GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);
    if (!vw->zfbo)
        glGenFramebuffers (1, &vw->zfbo);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, vw->zfbo);
    glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    while (glGetError () != GL_NO_ERROR)
        ;
    gboolean ok = TRUE, same = TRUE;
    GLenum type = f->bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
    for (int i = 0; ok && same && i < n; i++) {
        int comps = f->kind == CV_NV && i == 1 ? 2 : 1;
        int pw = f->pw[i], ph = f->ph[i];
        if (pw < 1 || ph < 1 || pw > f->zc_pl[i].w || ph > f->zc_pl[i].h || !f->pl[i] ||
            f->ls[i] < pw * comps * f->bytes) {
            ok = FALSE;
            break;
        }
        glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
        glReadBuffer (GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus (GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            ok = FALSE;
            break;
        }
        /* read as RGBA (every GL reads that back): 4 components per sample */
        gsize row = (gsize) pw * 4 * (gsize) f->bytes;
        int strip = (int) MAX ((gsize) 1, ((gsize) 4 << 20) / row);
        guint8 *buf = g_malloc (row * (gsize) strip);
        for (int y = 0; ok && same && y < ph; y += strip) {
            int rows = MIN (strip, ph - y);
            glReadPixels (0, y, pw, rows, GL_RGBA, type, buf);
            if (glGetError () != GL_NO_ERROR) {
                ok = FALSE;
                break;
            }
            same = walpe_zc_rows_equal (buf, row, 4, f->pl[i] + (gssize) y * f->ls[i], (gsize) f->ls[i],
                                        comps, f->bytes, pw, rows);
        }
        g_free (buf);
    }
    glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, (GLuint) prev_fbo);
    glBindBuffer (GL_PIXEL_PACK_BUFFER, (GLuint) prev_pbo);
    glPixelStorei (GL_PACK_ALIGNMENT, prev_pack);
    *why = !ok ? "the imported picture could not be read back for the comparison with its download"
               : "the imported planes are not, sample for sample, what the download gives";
    return ok && same;
}

/* Render thread, f pinned: show f where the decoder left it. TRUE = this
 * viewer now draws from the decoder's picture. FALSE = not shown that way
 * (the caller uploads the frame's planes when it has some); the source is
 * told, and downloads from its next picture on. */
static gboolean cv_zc_show (CvViewer *vw, const CvFrame *f)
{
    CvSource *src = vw->src;
    const char *why = "the renderer cannot import DMA-BUFs";
    GLuint tex[3] = { 0, 0, 0 };
    EGLImageKHR img[3] = { EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR };
    AVFrame *hold = NULL;
    if (g_atomic_int_get (&src->zc_refused))
        return FALSE;       /* (said already) */
    if (!vw->zc_how || !cv_zc_import (vw, f, tex, img, &why))
        goto refuse;
    if (f->zc == 2) {
        if (!cv_zc_verify (vw, f, tex, &why))
            goto refuse;
    }
    /* the picture stays out of the decoder's hands while it is on screen */
    hold = av_frame_clone (f->hw);
    if (!hold) {
        why = "out of memory";
        goto refuse;
    }
    cv_zc_drop (vw);
    for (int i = 0; i < 3; i++) {
        vw->ztex[i] = tex[i];
        vw->zimg[i] = img[i];
    }
    vw->zhold = hold;
    memcpy (vw->zcrop, f->zc_crop, sizeof (vw->zcrop));
    vw->zc_on = TRUE;
    vw->pkind = f->kind;
    vw->pscale = f->scale;
    vw->ymode = f->mode;
    vw->fw = f->w;
    vw->fh = f->h;
    if (f->zc == 2) {
        /* one more picture of this decoder found equal (counted once, also
         * when several monitors compared it) */
        g_mutex_lock (&src->lock);
        if (f->zc_gen == src->zc_gen && f->seq != src->zc_check_seq) {
            src->zc_check_seq = f->seq;
            g_atomic_int_inc (&src->zc_verified);
        }
        g_mutex_unlock (&src->lock);
    }
    return TRUE;
refuse:
    cv_zc_discard (tex, img);
    if (g_atomic_int_compare_and_exchange (&src->zc_refused, 0, 1)) {
        cv_zc_set_why (src, why);
        g_message ("video: zero-copy is not used for %s (%s): pictures are downloaded", src->path, why);
    }
    return FALSE;
}

static gboolean cv_viewer_upload (CvViewer *vw, int *bw, int *bh, gint64 *next_us);

static gboolean cvv_gl_init (WalpeVideo *v, GError **err)
{
    (void) err;
    CvViewer *vw = (CvViewer *) v;
    g_mutex_lock (&vw->src->lock);
    vw->redraw_cb = v->request_redraw;
    vw->redraw_user = v->request_user;
    g_mutex_unlock (&vw->src->lock);
    walpe_glutil_init ();
    glGenTextures (1, &vw->tex);
    glBindTexture (GL_TEXTURE_2D, vw->tex);
    /* without these the default NEAREST_MIPMAP_LINEAR min filter leaves the
     * texture incomplete and every sample returns black */
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* 16-bit planes (10-bit video) need GL_R16: desktop GL has it, GLES
     * with EXT_texture_norm16; without it such frames are repacked to 8
     * bits on the CPU */
    gboolean r16 = !g_getenv ("WALPE_VIDEO_NO_R16") &&
                   (epoxy_is_desktop_gl () || epoxy_has_gl_extension ("GL_EXT_texture_norm16"));
    if (!r16)
        g_atomic_int_set (&vw->src->gl_r16, 0);
    else
        g_atomic_int_compare_and_exchange (&vw->src->gl_r16, -1, 1);
    cv_hw_render_side (vw);     /* (hardware decoding asked for: what the renderer is) */
    return TRUE;
}

static gboolean cvv_gl_render (WalpeVideo *v, int w, int h, GError **err)
{
    (void) err;
    CvViewer *vw = (CvViewer *) v;

    int bw = 0, bh = 0;
    gint64 next_us = 0;
    /* frame limit: this monitor's frame rate limit, as its render thread
     * (the caller) was given it; 0 = none, or the setting is off */
    int lim = cv_limit_on () ? walpe_video_frame_limit () : 0;
    if (lim != vw->limit_fps) {
        g_mutex_lock (&vw->src->lock);
        vw->limit_fps = lim;
        g_cond_broadcast (&vw->src->cond);
        g_mutex_unlock (&vw->src->lock);
    }
    gboolean has = cv_viewer_upload (vw, &bw, &bh, &next_us);

    /* come back when the next frame is due (0: the decode thread wakes
     * us with it) */
    if (!vw->paused && has)
        vw->base.next_frame_us = next_us;
    /* nothing decoded yet: the host keeps what is on screen and paints
     * as soon as the first frame arrives, also on a paused monitor */
    v->no_frame = !has || vw->drawn_seq == 0;
    if (v->no_frame)
        return TRUE;

    /* placed by the wallpaper's alignment option (fit.h; default cover):
     * the frame is shown at k x its size */
    WalpeMat3 m;
    float k = 1.0f;
    walpe_fit_matrix (&v->fit, (float) bw, (float) bh, (float) w, (float) h, m.m, &k);
    /* What the decoder is asked to keep (want_w / want_h): the size the
     * frame has at the render resolution chosen for this monitor (video.h
     * render_w / render_h), else on the surface. The frame itself is drawn
     * straight to the surface, as large as it was decoded: nothing is made
     * softer on the way that the output could show. */
    float kw = k;
    if (v->render_w > 0 && v->render_h > 0 && (v->render_w != w || v->render_h != h)) {
        float m2[9];
        kw = 1.0f;
        walpe_fit_matrix (&v->fit, (float) bw, (float) bh, (float) v->render_w,
                          (float) v->render_h, m2, &kw);
    }
    g_atomic_int_set (&vw->want_w, (int) ceilf (bw * kw));
    g_atomic_int_set (&vw->want_h, (int) ceilf (bh * kw));
    double c0 = cv_cpu ();
    if (vw->pkind)
        cv_draw_planes (vw, m.m);
    else
        walpe_glutil_draw_quad (vw->tex, FALSE, 1.0f, m.m, WALPE_BLEND_NONE);
    if (cv_fps_log_s () > 0) {
        double c1 = cv_cpu ();
        g_mutex_lock (&vw->src->lock);
        vw->st_draw += c1 - c0;
        vw->st_draws++;
        g_mutex_unlock (&vw->src->lock);
    }
    return TRUE;
}

/* ------------------------------------------------------- upload paths
 * (video.h, "video_upload"). What the driver is asked to do per frame:
 *
 *   legacy   glTexSubImage2D with a pointer into main memory. The call
 *            cannot return before the driver has its own copy of the bytes
 *            (the pointer is the caller's again afterwards), so every plane
 *            is copied on the render thread inside the call, and from that
 *            copy into the texture on the GPU.
 *   pbo      the bytes are put into a buffer object first; glTexSubImage2D
 *            then names a place in a buffer the driver already owns and
 *            can return at once. The buffer gets a new store every frame
 *            (glBufferData NULL), so writing it never waits for the frame
 *            before.
 *   persist  the same without the per-frame store and map: three buffers
 *            that stay mapped, used in turn, each not written again before
 *            the fence of its last upload has been seen signalled.
 *   direct   the buffer was already filled by the decode thread
 *            (cv_stage_take): the render thread issues the three
 *            glTexSubImage2D calls and a fence, and copies nothing.
 *   +pingpong two sets of plane textures filled in turn.
 *
 * The textures, their formats and the glTexSubImage2D arguments are the
 * same on every path, and a buffer object as the source is defined to
 * unpack exactly like main memory: same bytes in the textures
 * (tests/video_test.c reads them back for every path). Nothing here can be
 * timed without a GPU; the profiler's row "(scene update, uploads)" on a
 * real card is the judge. */

static gboolean cv_gl_can_persist (void)
{
    return epoxy_is_desktop_gl () &&
           (epoxy_gl_version () >= 44 || epoxy_has_gl_extension ("GL_ARB_buffer_storage"));
}

static void cv_pbuf_destroy (CvPbuf *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (b[i].fence)
            glDeleteSync (b[i].fence);
        if (b[i].name) {
            glBindBuffer (GL_PIXEL_UNPACK_BUFFER, b[i].name);
            if (b[i].map)
                glUnmapBuffer (GL_PIXEL_UNPACK_BUFFER);
            glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
            glDeleteBuffers (1, &b[i].name);
        }
    }
    memset (b, 0, sizeof (*b) * (gsize) n);
}

/* render thread: n buffers of cap bytes that stay mapped (readable too:
 * a monitor outside the share group uploads from the mapping as from main
 * memory, and the frame checksums read it) */
static gboolean cv_pbuf_create (CvPbuf *b, int n, gsize cap)
{
    const GLbitfield fl = GL_MAP_WRITE_BIT | GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT |
                          GL_MAP_COHERENT_BIT;
    memset (b, 0, sizeof (*b) * (gsize) n);
    while (glGetError () != GL_NO_ERROR)
        ;
    gboolean ok = TRUE;
    for (int i = 0; i < n && ok; i++) {
        glGenBuffers (1, &b[i].name);
        glBindBuffer (GL_PIXEL_UNPACK_BUFFER, b[i].name);
        glBufferStorage (GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr) cap, NULL, fl);
        b[i].map = glMapBufferRange (GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr) cap, fl);
        b[i].cap = cap;
        ok = b[i].map != NULL && glGetError () == GL_NO_ERROR;
    }
    glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
    if (!ok)
        cv_pbuf_destroy (b, n);
    return ok;
}

/* the uploads issued from b are finished (or it has none) */
static gboolean cv_pbuf_idle (CvPbuf *b)
{
    if (!b->fence)
        return TRUE;
    if (glClientWaitSync (b->fence, 0, 0) == GL_TIMEOUT_EXPIRED)
        return FALSE;
    glDeleteSync (b->fence);
    b->fence = NULL;
    return TRUE;
}

/* uploads from b were just issued: a fence behind them, flushed now so
 * that they finish whether or not this frame is presented */
static void cv_pbuf_fence (CvPbuf *b)
{
    if (b->fence)
        glDeleteSync (b->fence);
    b->fence = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (b->fence && glClientWaitSync (b->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0) != GL_TIMEOUT_EXPIRED) {
        glDeleteSync (b->fence);
        b->fence = NULL;
    }
}

static const char *cv_up_name (int base)
{
    switch (base) {
    case WALPE_VUP_PBO: return "pbo";
    case WALPE_VUP_PBODATA: return "pbodata";
    case WALPE_VUP_PERSIST: return "persist";
    case WALPE_VUP_DIRECT: return "direct";
    default: return "legacy";
    }
}

/* Render thread of the stage's owner (gl = its context is current; FALSE
 * only when the viewer is freed without one): the decode thread stops
 * using the mapped buffers, frames that have their data there get it in
 * ordinary memory, and the buffers are deleted. Waits until the decode
 * thread is not writing one and no other viewer uploads from one. */
static void cv_stage_retire (CvViewer *vw, gboolean gl)
{
    CvSource *src = vw->src;
    CvPbuf old[CV_STAGE_N];
    g_mutex_lock (&src->lock);
    if (src->stage_owner != vw) {
        g_mutex_unlock (&src->lock);
        return;
    }
    src->stage_closing = TRUE;
    for (;;) {
        gboolean wait = FALSE;
        for (int i = 0; i < src->stage_n; i++)
            wait |= src->stage[i].writing;
        for (int i = 0; i <= CV_AHEAD; i++)
            wait |= src->buf[i].stage1 && src->buf[i].pins > 0;
        if (!wait)
            break;
        g_cond_wait (&src->cond, &src->lock);
    }
    for (int i = 0; i <= CV_AHEAD; i++) {
        CvFrame *f = &src->buf[i];
        if (!f->stage1)
            continue;
        const guint8 *base = src->stage[f->stage1 - 1].map;
        guint8 *d = g_malloc (f->stage_len);
        memcpy (d, base, f->stage_len);
        for (int k = 0; k < 3; k++)
            if (f->pl[k] >= base && f->pl[k] < base + f->stage_len)
                f->pl[k] = d + (f->pl[k] - base);
        g_free (f->data);
        f->data = d;
        f->cap = f->stage_len;
        f->stage1 = 0;
    }
    int n = src->stage_n;
    memcpy (old, src->stage, sizeof (old));
    memset (src->stage, 0, sizeof (src->stage));
    src->stage_n = 0;
    src->stage_cap = 0;
    src->stage_owner = NULL;
    src->stage_closing = FALSE;
    g_cond_broadcast (&src->cond);
    g_mutex_unlock (&src->lock);
    if (gl)
        cv_pbuf_destroy (old, n);
}

/* render thread: let go of what the upload path made (another path was
 * chosen, or the viewer's GL side ends). The textures in use stay. */
static void cv_up_reset (CvViewer *vw)
{
    if (vw->pbo) {
        glDeleteBuffers (1, &vw->pbo);
        vw->pbo = 0;
    }
    cv_pbuf_destroy (vw->ring, CV_RING_N);
    vw->ring_next = 0;
    if (vw->palt[0]) {
        glDeleteTextures (3, vw->palt);
        memset (vw->palt, 0, sizeof (vw->palt));
        memset (vw->altfmt, 0, sizeof (vw->altfmt));
    }
    cv_stage_retire (vw, TRUE);
}

/* Render thread, before a frame is picked (so nothing is pinned): follow
 * the setting, and on the direct path keep the stage - make it when the
 * source has none, make it larger when a frame did not fit, and see which
 * of its buffers the GPU has finished with. */
static void cv_up_sync (CvViewer *vw)
{
    CvSource *src = vw->src;
    int mode = walpe_video_upload_mode ();
    if (mode != vw->up_mode) {
        cv_up_reset (vw);
        int base = mode & WALPE_VUP_BASE, real = base;
        if ((base == WALPE_VUP_PERSIST || base == WALPE_VUP_DIRECT) && !cv_gl_can_persist ())
            real = WALPE_VUP_PBO;
        if (base > WALPE_VUP_DIRECT)
            real = WALPE_VUP_LEGACY;
        gboolean first = vw->up_mode < 0;
        vw->up_mode = mode;
        vw->up_real = real;
        char *note = g_strdup_printf ("%s%s%s%s%s%s", cv_up_name (real),
                                      mode & WALPE_VUP_PINGPONG ? "+pingpong" : "",
                                      mode & WALPE_VUP_HOLD ? " (hold: only the first frame is uploaded)" : "",
                                      real != base ? " (" : "", real != base ? cv_up_name (base) : "",
                                      real != base ? " was asked for: this GL context has no persistent "
                                                     "buffer storage)" : "");
        if (!first || mode != WALPE_VUP_LEGACY)
            g_message ("video: upload path %s (%s)", note, src->path);
        walpe_video_note_upload (note);
        g_free (note);
    }
    if (vw->up_real != WALPE_VUP_DIRECT || !src->gpu_yuv)
        return;
    /* (stage_owner is only ever set to, or changed from, a viewer by that
     * viewer's own thread: comparing it with vw needs no lock) */
    if (src->stage_owner == vw) {
        int done[CV_STAGE_N], nd = 0;
        for (int i = 0; i < src->stage_n; i++)
            if (src->stage[i].fence && cv_pbuf_idle (&src->stage[i]))
                done[nd++] = i;
        g_mutex_lock (&src->lock);
        for (int i = 0; i < nd; i++)
            src->stage[done[i]].gpu_busy = FALSE;
        gboolean grow = src->stage_want > src->stage_cap;
        g_mutex_unlock (&src->lock);
        if (!grow)
            return;
        cv_stage_retire (vw, TRUE);     /* a frame did not fit: larger buffers */
    }
    g_mutex_lock (&src->lock);
    gsize want = src->stage_want;
    gboolean vacant = src->stage_owner == NULL && src->stage_n == 0;
    g_mutex_unlock (&src->lock);
    if (!vacant || !want)
        return;
    CvPbuf nb[CV_STAGE_N];
    gsize cap = (want + 0xffff) & ~(gsize) 0xffff;
    if (!cv_pbuf_create (nb, CV_STAGE_N, cap)) {
        g_message ("video: persistently mapped buffers could not be made: upload path pbo (%s)",
                   src->path);
        vw->up_real = WALPE_VUP_PBO;
        walpe_video_note_upload ("pbo (direct was asked for: the buffers could not be made)");
        return;
    }
    g_mutex_lock (&src->lock);
    vacant = src->stage_owner == NULL && src->stage_n == 0;
    if (vacant) {
        memcpy (src->stage, nb, sizeof (nb));
        src->stage_n = CV_STAGE_N;
        src->stage_cap = cap;
        src->stage_owner = vw;
    }
    g_mutex_unlock (&src->lock);
    if (!vacant)
        cv_pbuf_destroy (nb, CV_STAGE_N);   /* another monitor was quicker: it serves the source */
}

/* the planes of f, tight, to dst + offs[] (render thread: pbo / persist) */
static void cv_copy_planes (const CvFrame *f, int n, guint8 *dst, const gsize *offs)
{
    for (int i = 0; i < n; i++) {
        gsize row = (gsize) f->pw[i] * (gsize) (f->bytes * (f->kind == CV_NV && i == 1 ? 2 : 1));
        if ((gsize) f->ls[i] == row) {
            memcpy (dst + offs[i], f->pl[i], row * (gsize) f->ph[i]);
        } else {
            for (int y = 0; y < f->ph[i]; y++)
                memcpy (dst + offs[i] + (gsize) y * row, f->pl[i] + (gssize) y * f->ls[i], row);
        }
    }
}

/* render thread, no lock (f is pinned): the frame's samples into this
 * viewer's textures, by the upload path in force. Returns the bytes handed
 * to GL. */
static double cv_upload_frame (CvViewer *vw, const CvFrame *f)
{
    double bytes = 0;
    if (vw->zc_on)
        cv_zc_drop (vw);        /* uploaded planes from here on */
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    if (f->kind == CV_BGRA) {
        glBindTexture (GL_TEXTURE_2D, vw->tex);
        glPixelStorei (GL_UNPACK_ROW_LENGTH, f->ls[0] / 4);
        /* (re)allocate storage only when needed; frames reuse it via SubImage */
        if (!vw->tex_alloced || vw->tex_w != f->w || vw->tex_h != f->h) {
            walpe_gl_tex_image_bgra (f->w, f->h, NULL);
            vw->tex_alloced = TRUE;
            vw->tex_w = f->w;
            vw->tex_h = f->h;
        }
        walpe_gl_tex_sub_image_bgra (0, 0, f->w, f->h, f->pl[0]);
        vw->pkind = 0;
        vw->up_tex_calls++;
        bytes = (double) f->w * f->h * 4;
    } else {
        CvSource *src = vw->src;
        int n = f->kind == CV_NV ? 2 : 3;
        GLenum type = f->bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
        gboolean pp = (vw->up_mode & WALPE_VUP_PINGPONG) != 0;
        /* the textures this frame goes to: with +pingpong the set the last
         * frame was NOT drawn from */
        GLuint *tex = pp ? vw->palt : vw->ptex;
        int *tw = pp ? vw->altw : vw->pw, *th = pp ? vw->alth : vw->ph;
        GLint *tf = pp ? vw->altfmt : vw->pfmt;
        gsize offs[3] = { 0, 0, 0 }, total = 0;
        int unit[3] = { 1, 1, 1 };
        if (!tex[0])
            glGenTextures (3, tex);
        /* storage first, while no buffer object is bound for unpacking
         * (glTexImage2D would read from it) */
        for (int i = 0; i < n; i++) {
            gboolean two = f->kind == CV_NV && i == 1;
            GLint ifmt = f->bytes == 2 ? (two ? GL_RG16 : GL_R16) : (two ? GL_RG8 : GL_R8);
            unit[i] = f->bytes * (two ? 2 : 1);
            offs[i] = total;
            total += (gsize) f->pw[i] * f->ph[i] * (gsize) unit[i];
            if (tf[i] != ifmt || tw[i] != f->pw[i] || th[i] != f->ph[i]) {
                glBindTexture (GL_TEXTURE_2D, tex[i]);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D (GL_TEXTURE_2D, 0, ifmt, f->pw[i], f->ph[i], 0, two ? GL_RG : GL_RED,
                              type, NULL);
                tf[i] = ifmt;
                tw[i] = f->pw[i];
                th[i] = f->ph[i];
            }
        }
        /* where the planes are read from: main memory, or a buffer object
         * (then the "pointers" are places in it) */
        const guint8 *from[3] = { f->pl[0], f->pl[1], f->pl[2] };
        int rowlen[3] = { f->ls[0] / unit[0], f->ls[1] / unit[1], f->ls[2] / unit[2] };
        GLuint bound = 0;
        CvPbuf *fenced = NULL;
        gboolean staged = FALSE;
        int path = vw->up_real;
        if (path == WALPE_VUP_DIRECT) {
            if (f->stage1 && src->stage_owner == vw) {
                /* the decode thread wrote it into one of this viewer's
                 * mapped buffers: nothing to copy */
                CvPbuf *b = &src->stage[f->stage1 - 1];
                bound = b->name;
                for (int i = 0; i < n; i++)
                    from[i] = (const guint8 *) (gsize) (f->pl[i] - b->map);
                fenced = b;
                staged = TRUE;
                vw->up_staged++;
            } else {
                /* not in a buffer of this viewer (the stage is another
                 * monitor's, or had no free buffer): from where it is */
                path = WALPE_VUP_LEGACY;
            }
        }
        if (path == WALPE_VUP_PERSIST) {
            if (vw->ring[0].name && vw->ring[0].cap < total)
                cv_pbuf_destroy (vw->ring, CV_RING_N);
            if (!vw->ring[0].name &&
                !cv_pbuf_create (vw->ring, CV_RING_N, (total + 0xffff) & ~(gsize) 0xffff)) {
                g_message ("video: persistently mapped buffers could not be made: upload path "
                           "pbo (%s)", src->path);
                walpe_video_note_upload ("pbo (persist was asked for: the buffers could not be made)");
                vw->up_real = WALPE_VUP_PBO;
            }
            CvPbuf *b = NULL;
            for (int k = 0; k < CV_RING_N && vw->ring[0].name && !b; k++) {
                int idx = (vw->ring_next + k) % CV_RING_N;
                if (cv_pbuf_idle (&vw->ring[idx])) {
                    b = &vw->ring[idx];
                    vw->ring_next = (idx + 1) % CV_RING_N;
                }
            }
            if (b) {
                cv_copy_planes (f, n, b->map, offs);
                vw->up_copied += (double) total;
                bound = b->name;
                fenced = b;
            } else {
                /* the GPU has not finished with any of them (or none could
                 * be made): this frame through a buffer with a new store */
                if (vw->ring[0].name)
                    vw->up_waits++;
                path = WALPE_VUP_PBO;
            }
        }
        if (path == WALPE_VUP_PBO || path == WALPE_VUP_PBODATA) {
            gboolean tight = TRUE, joined = TRUE;
            for (int i = 0; i < n; i++) {
                tight &= f->ls[i] == f->pw[i] * unit[i];
                joined &= f->pl[i] == f->pl[0] + offs[i];
            }
            if (!vw->pbo)
                glGenBuffers (1, &vw->pbo);
            glBindBuffer (GL_PIXEL_UNPACK_BUFFER, vw->pbo);
            gboolean filled = FALSE;
            if (path == WALPE_VUP_PBODATA && tight && joined) {
                /* one block in memory: new store and contents in one call */
                glBufferData (GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr) total, f->pl[0], GL_STREAM_DRAW);
                vw->up_buf_calls++;
                filled = TRUE;
            } else {
                glBufferData (GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr) total, NULL, GL_STREAM_DRAW);
                vw->up_buf_calls++;
                if (path == WALPE_VUP_PBODATA && tight) {
                    for (int i = 0; i < n; i++)
                        glBufferSubData (GL_PIXEL_UNPACK_BUFFER, (GLintptr) offs[i],
                                         (GLsizeiptr) ((gsize) f->pw[i] * f->ph[i] * (gsize) unit[i]),
                                         f->pl[i]);
                    vw->up_buf_calls += (guint) n;
                    filled = TRUE;
                } else {
                    guint8 *m = glMapBufferRange (GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr) total,
                                                  GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT |
                                                  GL_MAP_UNSYNCHRONIZED_BIT);
                    if (m) {
                        cv_copy_planes (f, n, m, offs);
                        filled = glUnmapBuffer (GL_PIXEL_UNPACK_BUFFER) != GL_FALSE;
                        vw->up_maps++;
                    }
                }
            }
            if (filled) {
                vw->up_copied += (double) total;
                bound = vw->pbo;
            } else {
                glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);   /* the buffer failed: from main memory */
            }
        }
        if (bound) {
            glBindBuffer (GL_PIXEL_UNPACK_BUFFER, bound);
            if (!staged)
                for (int i = 0; i < n; i++)
                    from[i] = (const guint8 *) offs[i];
            for (int i = 0; i < n; i++)
                rowlen[i] = staged ? f->ls[i] / unit[i] : 0;
        }
        for (int i = 0; i < n; i++) {
            gboolean two = f->kind == CV_NV && i == 1;
            glBindTexture (GL_TEXTURE_2D, tex[i]);
            glPixelStorei (GL_UNPACK_ROW_LENGTH, rowlen[i]);
            glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, f->pw[i], f->ph[i], two ? GL_RG : GL_RED, type,
                             from[i]);
            vw->up_tex_calls++;
            bytes += (double) f->pw[i] * f->ph[i] * unit[i];
        }
        if (bound)
            glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
        if (fenced) {
            cv_pbuf_fence (fenced);
            vw->up_fences++;
            if (staged) {
                g_mutex_lock (&src->lock);
                fenced->gpu_busy = fenced->fence != NULL;
                fenced->busy_at = cv_now ();
                g_mutex_unlock (&src->lock);
            }
        }
        if (pp) {
            /* this set is the one to draw from now */
            for (int i = 0; i < 3; i++) {
                GLuint t = vw->ptex[i];
                vw->ptex[i] = vw->palt[i];
                vw->palt[i] = t;
                int v = vw->pw[i];
                vw->pw[i] = vw->altw[i];
                vw->altw[i] = v;
                v = vw->ph[i];
                vw->ph[i] = vw->alth[i];
                vw->alth[i] = v;
                GLint fm = vw->pfmt[i];
                vw->pfmt[i] = vw->altfmt[i];
                vw->altfmt[i] = fm;
            }
        }
        vw->pkind = f->kind;
        vw->pscale = f->scale;
        vw->ymode = f->mode;
    }
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    vw->up_frames++;
    vw->fw = f->w;
    vw->fh = f->h;
    return bytes;
}

/* upload the frame that is due now, when this viewer does not show it yet
 * (each monitor's context has its own textures; a covered/paused monitor
 * just skips ahead). Render thread; returns whether this viewer has a
 * picture to draw (*bw x *bh). *next_us = when to render again (monotonic
 * us), 0 = when the decode thread says so. */
static gboolean cv_viewer_upload (CvViewer *vw, int *bw, int *bh, gint64 *next_us)
{
    CvSource *src = vw->src;
    *next_us = 0;
    cv_hw_render_side (vw); /* (hardware decoding switched on while the video plays) */
    cv_up_sync (vw);        /* the upload path in force; nothing is pinned yet */
    /* measuring aid (WALPE_VIDEO_UPLOAD=hold): the first frame stays */
    gboolean hold = (vw->up_mode & WALPE_VUP_HOLD) && vw->drawn_seq;
    g_mutex_lock (&src->lock);
    if (vw->measured)
        cv_viewer_tick (vw, cv_now ());
    double next_due = 0;
    CvFrame *f = src->has_frame ? cv_pick_frame (src, &next_due) : NULL;
    if (f && vw->drawn_seq != f->seq && vw->skip_seq != f->seq && !hold) {
        vw->shown++;
        if (vw->drawn_seq && f->seq > vw->drawn_seq + 1)
            vw->skipped += (guint) (f->seq - vw->drawn_seq - 1);
        /* the upload runs without the lock: the frame is pinned, so it
         * stays in the queue, and the decode thread and the other
         * monitors do not wait behind this one */
        f->pins++;
        g_mutex_unlock (&src->lock);
        /* WALPE_FPS_LOG: what this upload adds to the counters goes to the
         * log's copy under the lock taken below (nothing when it is off) */
        const gboolean logging = cv_fps_log_s () > 0;
        const guint u0[7] = { vw->up_frames, vw->up_tex_calls, vw->up_buf_calls, vw->up_maps,
                              vw->up_fences, vw->up_staged, vw->up_waits };
        const double uc0 = vw->up_copied;
        double c0 = cv_cpu ();
        double bytes = 0;
        gboolean shown = TRUE;
        /* zero-copy: the textures become the decoder's picture; a frame
         * that has its planes in main memory too (the first ones, for the
         * comparison) is uploaded when that is refused. A frame without
         * them that this context cannot import is not shown here: the
         * picture on screen stays until the decoder, told so, downloads. */
        if (f->zc && cv_zc_show (vw, f))
            ;
        else if (f->zc == 1)
            shown = FALSE;
        else
            bytes = cv_upload_frame (vw, f);
        double c1 = cv_cpu ();
        if (shown && f->seq <= (guint64) cv_md5_n ())
            cv_frame_md5 (f, "upload", src->path);
        g_mutex_lock (&src->lock);
        if (shown)
            vw->drawn_seq = f->seq;
        else
            vw->skip_seq = f->seq;
        vw->st_upload += c1 - c0;
        vw->st_bytes += bytes;
        if (logging) {
            vw->up_log.frames += vw->up_frames - u0[0];
            vw->up_log.tex_calls += vw->up_tex_calls - u0[1];
            vw->up_log.buf_calls += vw->up_buf_calls - u0[2];
            vw->up_log.maps += vw->up_maps - u0[3];
            vw->up_log.fences += vw->up_fences - u0[4];
            vw->up_log.staged += vw->up_staged - u0[5];
            vw->up_log.waits += vw->up_waits - u0[6];
            vw->up_log.copied += vw->up_copied - uc0;
            vw->up_log.real = vw->up_real;
            vw->up_log.mode = vw->up_mode;
        }
        if (--f->pins == 0)
            g_cond_broadcast (&src->cond);
        /* the upload took a moment: a newer frame may be due by now */
        CvFrame *f2 = src->has_frame ? cv_pick_frame (src, &next_due) : NULL;
        if (f2 && f2 != f && f2->seq != vw->drawn_seq) {
            *next_us = g_get_monotonic_time ();     /* at once */
            vw->starved = FALSE;
            f = NULL;
        }
    }
    if (f) {
        /* the next frame: when it is due if it is decoded, else when it
         * should be; if that has passed, the decoder is late and wakes us */
        double now = cv_now ();
        if (next_due <= 0 && src->clk_set) {
            double step = src->frame_interval > 0.001 ? src->frame_interval : 1.0 / 60.0;
            /* frame limit: the next queued picture is that many of the
             * clip's pictures later, at the earliest */
            if (src->keep_rate > 0 && src->keep_rate * step < 1.0)
                step *= floor (1.0 / (src->keep_rate * step) + 1e-3);
            next_due = src->clk + f->pts + step;
        }
        if (next_due > now + 0.0002) {
            *next_us = (gint64) (next_due * 1e6);
            vw->starved = FALSE;
        } else {
            vw->starved = TRUE;
        }
    } else if (!*next_us) {
        /* nothing queued (start, or the decoder resumes after a park):
         * the decode thread wakes this viewer with the first frame */
        vw->starved = TRUE;
        if (!vw->drawn_seq && src->parked && src->paused_all && !src->want_first) {
            /* a monitor that got this clip while it was parked (every
             * other viewer paused): one frame is decoded for it */
            src->want_first = TRUE;
            g_cond_broadcast (&src->cond);
        }
    }
    g_mutex_unlock (&src->lock);
    *bw = vw->fw;
    *bh = vw->fh;
    return vw->drawn_seq != 0;
}

static void cvv_set_paused (WalpeVideo *v, gboolean p)
{
    CvViewer *vw = (CvViewer *) v;
    CvSource *src = vw->src;
    g_mutex_lock (&src->lock);
    if (vw->paused && !p) {
        vw->tick_n = 0;     /* frame limit: its rate is measured anew */
        vw->tick_rate = 0;
    }
    vw->paused = p;
    cv_recompute_paused (src);
    g_mutex_unlock (&src->lock);
}

static void cvv_set_volume (WalpeVideo *v, double vol)
{
    (void) v; (void) vol; /* silent */
}

static void cvv_gl_free (WalpeVideo *v)
{
    CvViewer *vw = (CvViewer *) v;
    cv_up_reset (vw);       /* buffers, the second texture set, the stage */
    vw->up_mode = -1;
    cv_zc_drop (vw);        /* zero-copy: the imported planes and the picture they hold */
    if (vw->zfbo) {
        glDeleteFramebuffers (1, &vw->zfbo);
        vw->zfbo = 0;
    }
    if (vw->ptex[0]) {
        glDeleteTextures (3, vw->ptex);
        memset (vw->ptex, 0, sizeof (vw->ptex));
        memset (vw->pfmt, 0, sizeof (vw->pfmt));
    }
    if (vw->fbo) {
        glDeleteFramebuffers (1, &vw->fbo);
        vw->fbo = 0;
    }
    if (vw->tex) {
        glDeleteTextures (1, &vw->tex);
        vw->tex = 0;
    }
    vw->tex_alloced = FALSE;
}

/* detach + unref; the LAST detach joins the decode thread and tears the
 * shared pipeline down (runs on a monitor render thread). The registry
 * entry is removed in the same critical section that drops the last ref,
 * so a concurrent walpe_video_custom_new can never pick up a dying source. */
static void cvv_free (WalpeVideo *v)
{
    CvViewer *vw = (CvViewer *) v;
    CvSource *src = vw->src;
    gboolean last = FALSE;

    /* (gl_free gave the stage up; a viewer freed without it must not leave
     * the decode thread writing into its buffers) */
    cv_stage_retire (vw, FALSE);
    g_mutex_lock (&registry_lock);
    g_mutex_lock (&src->lock);
    g_ptr_array_remove_fast (src->viewers, vw);
    src->refs--;
    last = src->refs <= 0;
    if (last) {
        g_atomic_int_set (&src->quit, TRUE);
        g_cond_broadcast (&src->cond);
        if (registry && g_hash_table_lookup (registry, src->path) == src)
            g_hash_table_remove (registry, src->path);
    } else {
        cv_recompute_paused (src);
    }
    const gboolean replan = !last && src->scene;
    g_mutex_unlock (&src->lock);
    /* (what the users that stay ask for; under the registry's lock the
     * source cannot go meanwhile) */
    if (replan)
        cv_alt_update (src);
    g_mutex_unlock (&registry_lock);

    if (last) {
        g_debug ("video: shared decoder closed (%s)", src->path);
        g_thread_join (src->thread);
        cv_notes_set (src, NULL);
        for (int i = 0; i <= CV_AHEAD; i++) {
            av_frame_free (&src->buf[i].av);
            av_frame_free (&src->buf[i].drm);   /* (the description before the picture it maps) */
            av_frame_free (&src->buf[i].hw);
            g_free (src->buf[i].data);
        }
        mp4_demux_free (src->demux);
        av_packet_free (&src->pkt);
        av_frame_free (&src->frame);
        av_frame_free (&src->swframe);
        avcodec_free_context (&src->dec);
        cv_hw_close (src);
        av_frame_free (&src->dl_keep);
        av_frame_free (&src->zc_hw);
        walpe_hwsel_clear (&src->hw);
        if (src->hw_log)
            g_string_free (src->hw_log, TRUE);
        g_free (src->hw_profile);
        g_free (src->zc_why);
        g_free (src->half_buf);
        g_free (src->norm_buf);
        if (src->annexb)
            g_string_free (src->annexb, TRUE);
        g_clear_pointer (&src->alt_demux, mp4_demux_free);
        g_free (src->alt_next);
        g_free (src->alt_path);
        g_free (src->alt_bad);
        g_free (src->path);
        g_mutex_clear (&src->lock);
        g_cond_clear (&src->cond);
        g_ptr_array_free (src->viewers, TRUE);
        g_free (src);
    }
    /* (zero-copy: a picture this viewer still held goes back to its
     * decoder's pool, or is freed with it when the decoder is gone) */
    av_frame_free (&vw->zhold);
    g_free (vw->copy_path);
    /* no main-loop source references the viewer: free synchronously */
    g_free (vw);
}

static CvViewer *cv_viewer_new (CvSource *src)
{
    CvViewer *vw = g_new0 (CvViewer, 1);
    vw->base.gl_init = cvv_gl_init;
    vw->base.gl_render = cvv_gl_render;
    vw->base.set_paused = cvv_set_paused;
    vw->base.set_volume = cvv_set_volume;
    vw->base.gl_free = cvv_gl_free;
    vw->base.free = cvv_free;
    vw->src = src;
    vw->up_mode = -1;
    vw->zc_can = -1;        /* its GL context has not been asked yet */

    g_mutex_lock (&src->lock);
    src->refs++;
    g_ptr_array_add (src->viewers, vw);
    cv_recompute_paused (src);
    g_mutex_unlock (&src->lock);
    return vw;
}

/* ------------------------------------------------------- source setup */

/* alt: a converted copy of the clip to play instead of it (videotex.h),
 * when it is one; NULL = the clip */
static CvSource *cv_source_create (const char *key, const char *path, guint64 off,
                                   guint64 len, const char *alt, GError **err)
{
    CvSource *src = g_new0 (CvSource, 1);
    src->path = g_strdup (key);
    src->viewers = g_ptr_array_new ();
    g_mutex_init (&src->lock);
    g_cond_init (&src->cond);
    src->hw_fmt = AV_PIX_FMT_NONE;
    walpe_hwsel_clear (&src->hw);
    src->hw_log = g_string_new (NULL);
    src->gl_r16 = -1;
    src->last_raw = src->last_play = -1;
    src->skip_until = -1;
    src->park_raw = -1;
    src->drop_raw_max = -1;
    src->rebase_raw = -1;

    src->range_off = off;
    src->range_len = len;
    src->demux = mp4_demux_open_range (path, off, len, err);
    if (!src->demux)
        goto fail;
    if ((gint64) src->demux->width * src->demux->height > CV_MAX_PIXELS) {
        /* (the stream's own size is checked by the decoder: max_pixels) */
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "%s: the video is %d x %d, larger than the player takes (%d megapixels)",
                     path, src->demux->width, src->demux->height, CV_MAX_PIXELS / 1000000);
        goto fail;
    }
    src->orig_w = src->demux->width;
    src->orig_h = src->demux->height;
    src->orig_n = src->demux->nsamples;
    src->orig_fps = cv_clip_fps (src->demux);
    src->orig_rot = src->demux->rotation;
    if (alt) {
        /* a converted copy of the clip: played from the start when it is
         * the clip again (the clip's own table of pictures was read above
         * to tell; its decoder is never opened) */
        char *why = NULL;
        Mp4Demux *ad = mp4_demux_open_range (alt, 0, 0, NULL);
        if (ad && cv_alt_fits (src, ad, &why)) {
            g_message ("video: %s is played from its converted copy %s (%dx%d instead of %dx%d)",
                       path, alt, ad->width, ad->height, src->orig_w, src->orig_h);
            mp4_demux_free (src->demux);
            src->demux = ad;
            src->alt_path = g_strdup (alt);
        } else {
            g_message ("video: the converted copy %s of %s is not used: %s", alt, path,
                       why ? why : "it cannot be read");
            src->alt_bad = why ? g_steal_pointer (&why) : g_strdup ("it cannot be read");
            g_clear_pointer (&ad, mp4_demux_free);
        }
        g_free (why);
    }
    cv_play_note (src);
    cv_clip_times (src);    /* frame limit: the frame interval and the first time stamp */
    src->dl_keep = av_frame_alloc ();
    src->zc_hw = av_frame_alloc ();
    /* (status line: the profile the file declares) */
    src->hw_profile = walpe_h264_profile_text (src->demux->sps, src->demux->sps_len);
    /* GPU conversion unless the renderer is itself a CPU rasterizer
     * (measured on llvmpipe: 4K I420 shader path 214% vs 190% CPU) or
     * WALPE_CPU_YUV=1; WALPE_GPU_YUV=1 forces it */
    src->gpu_yuv = g_getenv ("WALPE_GPU_YUV") ? TRUE
                 : !g_getenv ("WALPE_CPU_YUV") && !walpe_gl_is_software ();
    if (!cv_decoder_open (src, TRUE, err))
        goto fail;
    {
        /* testing aid (WALPE_VIDEO_TEST_TAIL=seconds): start at the last
         * keyframe that leaves at least that much of the clip, so the
         * first loop wrap of a long clip comes soon */
        const char *tt = g_getenv ("WALPE_VIDEO_TEST_TAIL");
        double fps = cv_clip_fps (src->demux);
        if (tt && g_ascii_strtod (tt, NULL) > 0 && fps > 0) {
            guint back = (guint) (g_ascii_strtod (tt, NULL) * fps) + 1;
            guint i = src->demux->nsamples > back ? src->demux->nsamples - back : 0;
            while (i > 0 && !cv_sample_is_key (src->demux, i))
                i--;
            src->demux->cursor = i;
        }
    }
    src->annexb = g_string_sized_new (64 * 1024);
    for (int i = 0; i <= CV_AHEAD; i++) {
        src->buf[i].av = av_frame_alloc ();
        src->buf[i].hw = av_frame_alloc ();
        src->buf[i].drm = av_frame_alloc ();
        src->spare[src->nspare++] = &src->buf[i];
    }
    src->pkt = av_packet_alloc ();
    src->frame = av_frame_alloc ();
    src->swframe = av_frame_alloc ();
    src->thread = g_thread_new ("walpe-video", cv_decode_thread, src);
    g_debug ("video: shared decoder created for %s", path);
    return src;

fail:
    if (src->demux)
        mp4_demux_free (src->demux);
    if (src->dec)
        avcodec_free_context (&src->dec);
    cv_hw_close (src);
    cv_notes_set (src, NULL);
    av_frame_free (&src->dl_keep);
    av_frame_free (&src->zc_hw);
    walpe_hwsel_clear (&src->hw);
    if (src->hw_log)
        g_string_free (src->hw_log, TRUE);
    g_free (src->hw_profile);
    g_free (src->zc_why);
    g_free (src->alt_path);
    g_free (src->alt_bad);
    g_free (src->path);
    g_ptr_array_free (src->viewers, TRUE);
    g_cond_clear (&src->cond);
    g_mutex_clear (&src->lock);
    g_free (src);
    return NULL;
}

static CvViewer *cv_attach (const char *path, guint64 off, guint64 len, const char *alt,
                            GError **err)
{
    char *key = off || len ? g_strdup_printf ("%s@%" G_GUINT64_FORMAT "+%" G_GUINT64_FORMAT,
                                              path, off, len)
                           : g_strdup (path);
    g_mutex_lock (&registry_lock);
    if (!registry)
        registry = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    CvSource *src = g_hash_table_lookup (registry, key);
    if (!src) {
        src = cv_source_create (key, path, off, len, alt, err);
        if (!src) {
            g_mutex_unlock (&registry_lock);
            g_free (key);
            return NULL;
        }
        g_hash_table_insert (registry, g_strdup (key), src);
    }
    CvViewer *vw = cv_viewer_new (src);
    g_mutex_unlock (&registry_lock);
    g_free (key);
    return vw;
}

WalpeVideo *walpe_video_custom_new (const char *path, GError **err)
{
    /* serialize lookup+create so two monitors starting up on the same
     * file share one source instead of racing into two; the viewer is
     * attached (refs++) before the lock drops so the source cannot die */
    CvViewer *vw = cv_attach (path, 0, 0, NULL, err);
    if (!vw)
        return NULL;
    CvSource *src = vw->src;
    guint n;
    g_mutex_lock (&src->lock);
    n = src->viewers->len;
    g_mutex_unlock (&src->lock);
    if (n > 1)
        g_message ("video: sharing one decoder across %u monitors (%s)", n, path);
    return &vw->base;
}

/* ------------------------------------------- video as a scene texture */

struct WalpeVideoTex {
    CvViewer *vw;
};

/* src->lock held */
static void cv_viewer_copy (CvViewer *vw, const WalpeVideoTexCopy *copy)
{
    g_free (vw->copy_path);
    vw->copy_state = copy ? copy->state : WALPE_VTEX_COPY_UNDECIDED;
    vw->copy_path = copy && copy->path ? g_strdup (copy->path) : NULL;
    vw->copy_w = copy ? copy->w : 0;
    vw->copy_h = copy ? copy->h : 0;
}

/* (static: the names of videotex.h are the core's, modload.c forwards them
 * through the module's table) */
static WalpeVideoTex *cv_tex_new_copy (const char *path, guint64 off, guint64 len,
                                       const WalpeVideoTexCopy *copy, GError **err)
{
    const char *alt = copy && copy->state == WALPE_VTEX_COPY_READY ? copy->path : NULL;
    CvViewer *vw = cv_attach (path, off, len, alt, err);
    if (!vw)
        return NULL;
    WalpeVideoTex *v = g_new0 (WalpeVideoTex, 1);
    v->vw = vw;
    g_mutex_lock (&vw->src->lock);
    vw->measured = TRUE;        /* frame limit: its rate is the scene's frame rate */
    vw->src->scene = TRUE;
    cv_viewer_copy (vw, copy);
    g_mutex_unlock (&vw->src->lock);
    /* a clip that is playing already (another monitor's scene has it):
     * what its users ask for together */
    cv_alt_update (vw->src);
    return v;
}

WalpeVideoTex *walpe_video_tex_new (const char *path, guint64 off, guint64 len, GError **err)
{
    return cv_tex_new_copy (path, off, len, NULL, err);
}

static void cv_tex_set_copy (WalpeVideoTex *v, const WalpeVideoTexCopy *copy)
{
    CvSource *src = v->vw->src;
    g_mutex_lock (&src->lock);
    cv_viewer_copy (v->vw, copy);
    g_mutex_unlock (&src->lock);
    cv_alt_update (src);
}

static void cv_tex_info (WalpeVideoTex *v, WalpeVideoTexInfo *info)
{
    CvSource *src = v->vw->src;
    memset (info, 0, sizeof *info);
    g_mutex_lock (&src->lock);
    info->clip_w = src->orig_w;
    info->clip_h = src->orig_h;
    info->clip_fps = src->orig_fps;
    info->clip_frames = src->orig_n;
    info->play_w = src->play_w;
    info->play_h = src->play_h;
    info->play_fps = src->play_fps;
    info->from_copy = src->alt_path != NULL;
    g_mutex_unlock (&src->lock);
    info->format = g_atomic_int_get (&src->pic_format);
    info->matrix = g_atomic_int_get (&src->pic_matrix);
}

/* videotex.h walpe_video_scene_note */
static char *cv_scene_note (WalpeVideoJobText job, gpointer user)
{
    GString *all = NULL;
    GHashTableIter it;
    gpointer val;
    g_mutex_lock (&registry_lock);
    if (registry)
        g_hash_table_iter_init (&it, registry);
    while (registry && g_hash_table_iter_next (&it, NULL, &val)) {
        CvSource *src = val;
        g_mutex_lock (&src->lock);
        if (src->scene) {
            /* the file's name without its folder and without "@off+len" */
            const char *at = src->range_off || src->range_len ? strrchr (src->path, '@') : NULL;
            const char *end = at ? at : src->path + strlen (src->path), *name = end;
            while (name > src->path && name[-1] != '/')
                name--;
            if (!all)
                all = g_string_new (NULL);
            g_string_append_printf (all, "%s%.*s: source %dx%d %.2f fps, played %dx%d %.2f fps, copy: ",
                                    all->len ? "; " : "", (int) (end - name), name, src->orig_w,
                                    src->orig_h, src->orig_fps, src->play_w, src->play_h,
                                    src->play_fps);
            /* what the users say: one that waits for a copy first, else
             * the first one's answer */
            const CvViewer *say = NULL;
            for (guint i = 0; i < src->viewers->len; i++) {
                const CvViewer *vw = g_ptr_array_index (src->viewers, i);
                if (!say || (vw->copy_state == WALPE_VTEX_COPY_WAITING &&
                             say->copy_state != WALPE_VTEX_COPY_WAITING))
                    say = vw;
            }
            const gboolean pending = g_atomic_int_get (&src->alt_pending) != 0;
            const int st = say ? say->copy_state : WALPE_VTEX_COPY_UNDECIDED;
            if (src->alt_path && !pending) {
                g_string_append (all, "ready");
            } else if (pending && src->alt_next) {
                g_string_append (all, "ready (played from where the clip starts again)");
            } else if (st == WALPE_VTEX_COPY_WAITING) {
                char *t = job && say->copy_path ? job (say->copy_path, user) : NULL;
                g_string_append (all, t ? t : "wanted");
                g_free (t);
            } else if (st == WALPE_VTEX_COPY_READY) {
                g_string_append_printf (all, "not usable (%s)", src->alt_bad ? src->alt_bad : "?");
            } else {
                g_string_append (all, st == WALPE_VTEX_COPY_OFF ? "off"
                                 : st == WALPE_VTEX_COPY_NOT_NEEDED ? "not needed"
                                 : st == WALPE_VTEX_COPY_NO_LIMIT ? "not needed (video_resolution is none)"
                                 : st == WALPE_VTEX_COPY_FULL_TEX
                                   ? "not made (the scene keeps its textures at file size)"
                                 : st == WALPE_VTEX_COPY_UNSUITED
                                   ? (g_atomic_int_get (&src->pic_format) == 3
                                      ? "not made (the clip is stored rotated)"
                                      : "not made (the clip is not 8-bit 4:2:0 in limited range)")
                                 : "not decided yet");
            }
        }
        g_mutex_unlock (&src->lock);
    }
    g_mutex_unlock (&registry_lock);
    return all ? g_string_free (all, FALSE) : NULL;
}

GLuint walpe_video_tex_update (WalpeVideoTex *v, int *w, int *h)
{
    CvViewer *vw = v->vw;
    if (!vw->tex)
        cvv_gl_init (&vw->base, NULL);
    int bw = 0, bh = 0;
    gint64 next_us = 0;
    guint64 before = vw->drawn_seq;
    gboolean has = cv_viewer_upload (vw, &bw, &bh, &next_us);
    if (vw->drawn_seq != before)
        walpe_video_note_frame ();  /* the texture holds another frame than before this call */
    if (!has || vw->drawn_seq == 0)
        return 0;
    if (vw->pkind && vw->rgb_seq != vw->drawn_seq) {
        /* scenes/effects need an RGBA texture: one GPU pass per new frame */
        GLint prev_vp[4];
        glGetIntegerv (GL_VIEWPORT, prev_vp);
        if (!vw->tex_alloced || vw->tex_w != bw || vw->tex_h != bh) {
            glBindTexture (GL_TEXTURE_2D, vw->tex);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            vw->tex_alloced = TRUE;
            vw->tex_w = bw;
            vw->tex_h = bh;
            if (!vw->fbo)
                glGenFramebuffers (1, &vw->fbo);
            glBindFramebuffer (GL_FRAMEBUFFER, vw->fbo);
            glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                    vw->tex, 0);
        }
        glBindFramebuffer (GL_FRAMEBUFFER, vw->fbo);
        glViewport (0, 0, bw, bh);
        /* row 0 of the FBO = image top (the upload convention): flip y */
        static const float flip[9] = { 1, 0, 0, 0, -1, 0, 0, 0, 1 };
        cv_draw_planes (vw, flip);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]); /* lesson 4 */
        vw->rgb_seq = vw->drawn_seq;
    }
    if (w)
        *w = bw;
    if (h)
        *h = bh;
    return vw->tex;
}

void walpe_video_tex_set_view (WalpeVideoTex *v, int w, int h)
{
    g_atomic_int_set (&v->vw->want_w, MAX (0, w));
    g_atomic_int_set (&v->vw->want_h, MAX (0, h));
}

gint64 walpe_video_tex_interval_us (WalpeVideoTex *v)
{
    CvSource *src = v->vw->src;
    g_mutex_lock (&src->lock);
    double iv = src->frame_interval;
    g_mutex_unlock (&src->lock);
    return iv > 0.001 ? (gint64) (iv * 1e6) : 0;
}

void walpe_video_tex_set_paused (WalpeVideoTex *v, gboolean paused)
{
    cvv_set_paused (&v->vw->base, paused);
}

void walpe_video_tex_free (WalpeVideoTex *v)
{
    if (!v)
        return;
    cvv_gl_free (&v->vw->base);
    cvv_free (&v->vw->base); /* last user joins the decoder thread */
    g_free (v);
}

/* `walpe-ctl mem` (video.h WalpeVideoMemStats): what the clips hold, read
 * when asked. Sizes are read as they stand (the decode thread fills a
 * spare slot without the lock): a report, not an invariant. */
static void cv_mem_stats (WalpeVideoMemStats *o)
{
    memset (o, 0, sizeof (*o));
    o->opened = (guint) g_atomic_int_get (&cv_opened);
    g_mutex_lock (&registry_lock);
    GHashTableIter it;
    gpointer v;
    if (registry)
        g_hash_table_iter_init (&it, registry);
    while (registry && g_hash_table_iter_next (&it, NULL, &v)) {
        CvSource *src = v;
        g_mutex_lock (&src->lock);
        const int w = src->demux ? (int) src->demux->width : 0;
        const int h = src->demux ? (int) src->demux->height : 0;
        const gint64 pic = (gint64) w * h * 3 / 2;
        o->sources++;
        o->viewers += (int) src->viewers->len;
        o->parked += src->parked != 0;
        o->paused += src->paused_all != 0;
        if (!src->parked) {
            o->decoders++;
            o->hardware += src->hw_on != 0;
            if (!src->hw_on) {
                o->threads += src->threads;
                o->pool_estimate_bytes += pic * (16 + MAX (src->threads, 1));
            }
        }
        o->queued += src->nq;
        o->queue_slots += CV_AHEAD;
        /* (a slot counts the decoder's picture only while it holds one: a
         * frame reduced or copied into the slot's own buffer has pl[0] too,
         * and three such slots of a 4K clip read as 37 MB more than they
         * hold) */
        for (int i = 0; i <= CV_AHEAD; i++)
            o->frame_bytes += (gint64) src->buf[i].cap +
                              (src->buf[i].av && src->buf[i].av->buf[0] ? pic : 0);
        o->scratch_bytes += (gint64) src->half_cap + (gint64) src->norm_cap +
                            (src->annexb ? (gint64) src->annexb->allocated_len : 0);
        o->stage_buffers += src->stage_n;
        o->stage_bytes += (gint64) src->stage_n * (gint64) src->stage_cap;
        o->decoded += src->seq;
        if ((gint64) w * h > (gint64) o->width * o->height) {
            o->width = w;
            o->height = h;
        }
        g_mutex_unlock (&src->lock);
    }
    g_mutex_unlock (&registry_lock);
}

/* walpe-video.so entry point (modules.h): everything above is module-local */
WALPE_API const WalpeVideoModule *walpe_video_module_entry (void)
{
    static const WalpeVideoModule m = {
        WALPE_MODULE_ABI, walpe_video_custom_new, walpe_video_tex_new, walpe_video_tex_update,
        walpe_video_tex_interval_us, walpe_video_tex_set_view, walpe_video_tex_set_paused,
        walpe_video_tex_free,
        { walpe_sounddec_open, walpe_sounddec_read, walpe_sounddec_close },   /* sounddec.c */
        cv_tex_new_copy, cv_tex_set_copy, cv_tex_info, cv_scene_note,
        cv_mem_stats,
    };
    return &m;
}
