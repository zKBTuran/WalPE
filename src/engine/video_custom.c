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
 * - Hardware decoding (off by default, video.h): the GPU decodes, the
 *   frame is downloaded as NV12/P010 and takes the same upload path; any
 *   failure falls back to software decoding at the same position. */
#include "video.h"
#include "videotex.h"
#include "glutil.h"
#include "mp4demux.h"
#include "yuvconv.h"
#include "modules.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
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
    /* hardware decoding */
    gboolean hw_wanted;         /* decode thread: hardware decoding was asked for */
    gboolean hw_asked;          /* decode thread: the setting when the decoder was opened */
    AVBufferRef *hw_dev;
    enum AVPixelFormat hw_fmt;
    const char *hw_name;
    gint hw_refused;            /* atomic: the device does not take this stream */
    int hw_errors;              /* failures in a row */
    gboolean hw_off;            /* gave up for this source: software from now on */
    /* direct upload (src->lock): persistently mapped buffers that the
     * viewer stage_owner made in its GL context and that the decode thread
     * fills (cv_stage_take); stage_want = the largest frame it had to place */
    CvPbuf stage[CV_STAGE_N];
    int stage_n;
    gsize stage_cap, stage_want;
    CvViewer *stage_owner;
    gboolean stage_closing;
    guint stage_misses;         /* frames that found no free buffer (fps log) */
    char *hw_why;               /* decode thread: why hardware decoding is not in use (status) */
    gboolean hw_seen;           /* decode thread: a picture came out of the hardware decoder */
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
};

static const char *cv_up_name (int base);

/* --------------------------------------------------- registry (shared) */

static GMutex registry_lock;
static GHashTable *registry;    /* path -> CvSource* */

/* ------------------------------------------------------------ helpers */

static double cv_now (void)
{
    return g_get_monotonic_time () / 1e6;
}

/* WALPE_FPS_LOG=<seconds>: decoded and shown frame rates and the CPU time
 * of each stage (0 = off) */
static double cv_fps_log_s (void)
{
    static double v = -1;
    if (v < 0) {
        const char *e = g_getenv ("WALPE_FPS_LOG");
        v = e && *e ? CLAMP (g_ascii_strtod (e, NULL), 0.0, 3600.0) : 0;
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
    static int n = -1;
    if (n < 0) {
        const char *e = g_getenv ("WALPE_VIDEO_MD5");
        n = e && *e ? CLAMP (atoi (e), 0, 1000000) : 0;
    }
    return n;
}

/* MD5 of the samples of f, planes row by row without padding (g_free) */
static char *cv_frame_md5_str (const CvFrame *f)
{
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

/* every attached viewer shows the frame at <= half size (src->lock held) */
static gboolean cv_half_ok (CvSource *src, int w, int h)
{
    static int disabled = -1;
    if (disabled < 0)
        disabled = g_getenv ("WALPE_VIDEO_FULLRES") ? 1 : 0;
    if (disabled || src->viewers->len == 0)
        return FALSE;
    for (guint i = 0; i < src->viewers->len; i++) {
        CvViewer *vw = g_ptr_array_index (src->viewers, i);
        int ww = g_atomic_int_get (&vw->want_w), wh = g_atomic_int_get (&vw->want_h);
        if (ww <= 0 || wh <= 0 || w / 2 < ww || h / 2 < wh)
            return FALSE;
    }
    return TRUE;
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
    static int fmt_err_n, fmt_note_n;
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
            if (fmt_err_n++ < 5)
                g_warning ("video: frame format %s (%d) is not supported: %s shows nothing",
                           fn ? fn : "?", fr->format, src->path);
            return FALSE;
        }
        if (fmt_note_n++ < 1)
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
    static double v = -1;
    if (v < 0) {
        const char *e = g_getenv ("WALPE_VIDEO_PARK_S");
        v = e && *e ? CLAMP (g_ascii_strtod (e, NULL), 0.0, 86400.0) : CV_PARK_S;
    }
    return v;
}

static void cv_park (CvSource *src);

/* a frame leaves the queue (src->lock held): the decoder gets its buffer back */
static void cv_frame_release (CvSource *src, CvFrame *f)
{
    av_frame_unref (f->av);
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
    n = MAX (n, cv_threads_learned (src->path));
    return CLAMP (n, 1, *max);
}

/* the hardware offers its format for this stream, or software decoding */
static enum AVPixelFormat cv_get_format (AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    CvSource *src = c->opaque;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == src->hw_fmt)
            return *p;
    /* the device does not decode this stream (profile, size, bit depth):
     * the decode loop reopens the decoder for software decoding */
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
    src->hw_seen = FALSE;
    if (src->dl_keep)
        av_frame_unref (src->dl_keep);
}

/* why hardware decoding is not in use, for the status line and the log
 * (decode thread, or source creation before the thread exists) */
static void cv_hw_set_why (CvSource *src, const char *fmt, ...) G_GNUC_PRINTF (2, 3);
static void cv_hw_set_why (CvSource *src, const char *fmt, ...)
{
    va_list ap;
    g_free (src->hw_why);
    va_start (ap, fmt);
    src->hw_why = g_strdup_vprintf (fmt, ap);
    va_end (ap);
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

/* Hardware decoding when asked for (video.h; off by default): a device
 * of the first type that exists and that libavcodec's H.264 decoder can
 * use. FALSE = software decoding. */
/* is hardware decoding asked for: WALPE_VIDEO_HWDEC when set (*only = the
 * one device type it names, if it names one), else the setting */
static gboolean cv_hw_asked (const char **only)
{
    static const char *e;
    static gsize once;
    if (g_once_init_enter (&once)) {
        e = g_getenv ("WALPE_VIDEO_HWDEC");
        g_once_init_leave (&once, 1);
    }
    if (only)
        *only = NULL;
    if (e && *e) {
        if (!g_ascii_strcasecmp (e, "0") || !g_ascii_strcasecmp (e, "off") ||
            !g_ascii_strcasecmp (e, "no"))
            return FALSE;
        if (only && g_ascii_strcasecmp (e, "1") && g_ascii_strcasecmp (e, "auto") &&
            g_ascii_strcasecmp (e, "on") && g_ascii_strcasecmp (e, "yes"))
            *only = e;
        return TRUE;
    }
    return walpe_video_hwdec () != 0;
}

static gboolean cv_hw_setup (CvSource *src, const AVCodec *codec)
{
    static const char *const auto_types[] = { "cuda", "vaapi", "vdpau", NULL };
    const char *one[2] = { NULL, NULL };
    const char *const *types = auto_types;
    src->hw_asked = cv_hw_asked (&one[0]);
    if (!src->hw_asked)
        return FALSE;
    if (one[0])
        types = one;
    src->hw_wanted = TRUE;
    if (!src->gpu_yuv) {
        /* nothing to gain: the picture would be downloaded to be drawn by the CPU */
        cv_hw_set_why (src, "the renderer is a software renderer");
        return FALSE;
    }
    if (src->hw_off)
        return FALSE;       /* failed before (hw_why says how) */
    GString *why = g_string_new (NULL);
    for (int t = 0; types[t]; t++) {
        enum AVHWDeviceType type = av_hwdevice_find_type_by_name (types[t]);
        enum AVPixelFormat fmt = AV_PIX_FMT_NONE;
        if (type == AV_HWDEVICE_TYPE_NONE) {
            g_string_append_printf (why, "%s%s: not in this FFmpeg build", why->len ? "; " : "",
                                    types[t]);
            continue;
        }
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config (codec, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == type) {
                fmt = cfg->pix_fmt;
                break;
            }
        }
        if (fmt == AV_PIX_FMT_NONE) {
            g_string_append_printf (why, "%s%s: no H.264 support in this FFmpeg build",
                                    why->len ? "; " : "", types[t]);
            continue;
        }
        AVBufferRef *dev = NULL;
        int rc = av_hwdevice_ctx_create (&dev, type, NULL, NULL, 0);
        if (rc < 0 || !dev) {
            g_string_append_printf (why, "%s%s: %s", why->len ? "; " : "", types[t],
                                    av_err2str (rc));
            if (type == AV_HWDEVICE_TYPE_CUDA) {
                /* libavutil loads NVIDIA's libraries by name and says only
                 * on stderr which one it missed: say it here */
                static const char *const libs[] = { "libcuda.so.1", "libnvcuvid.so.1" };
                for (guint i = 0; i < G_N_ELEMENTS (libs); i++) {
                    void *h = dlopen (libs[i], RTLD_LAZY | RTLD_LOCAL);
                    if (h)
                        dlclose (h);
                    else
                        g_string_append_printf (why, " (%s cannot be loaded)", libs[i]);
                }
            }
            continue;
        }
        src->hw_dev = dev;
        src->hw_fmt = fmt;
        src->hw_name = av_hwdevice_get_type_name (type);
        g_string_free (why, TRUE);
        return TRUE;
    }
    g_message ("video: hardware decoding is not available (%s): decoding on the CPU", why->str);
    cv_hw_set_why (src, "no device: %s", why->str);
    g_string_free (why, TRUE);
    src->hw_off = TRUE;     /* not asked again for this source */
    return FALSE;
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

/* for `walpe-ctl status`: what this source's decoder does. in_vain:
 * hardware decoding is asked for and not in use - the line then says why
 * (no device and each device's error, the device refused the stream,
 * downloads or decode calls failed), so the status alone tells whether
 * the GPU decodes and, if not, what stopped it. A hardware decoder that
 * has been opened and has not delivered a picture yet says so: opening
 * succeeds before the device has seen the stream. */
static void cv_note_decoder (CvSource *src, gboolean in_vain)
{
    int thr = src->dec ? src->dec->thread_count : src->threads;
    char *note = src->hw_dev
        ? g_strdup_printf ("%dx%d %.2f fps: hardware (%s)%s", src->demux->width, src->demux->height,
                           cv_clip_fps (src->demux), src->hw_name ? src->hw_name : "?",
                           src->hw_seen ? "" : ", no picture yet")
        : g_strdup_printf ("%dx%d %.2f fps: software, %d thread%s%s%s%s", src->demux->width,
                           src->demux->height, cv_clip_fps (src->demux), thr, thr == 1 ? "" : "s",
                           in_vain ? " (hardware decoding was asked for and is not in use: " : "",
                           in_vain ? (src->hw_why ? src->hw_why : "not available or failed") : "",
                           in_vain ? ")" : "");
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
 * decode the clip otherwise; the picture degrades between keyframes). */
static gboolean cv_decoder_open (CvSource *src, gboolean first, GError **err)
{
    const AVCodec *codec = avcodec_find_decoder (AV_CODEC_ID_H264);
    if (!codec) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_FAILED, "no H.264 decoder");
        return FALSE;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        src->dec = avcodec_alloc_context3 (codec);
        if (!src->dec)
            break;
        src->dec->max_pixels = CV_MAX_PIXELS;   /* no frame buffer beyond it (see there) */
        if (src->threads < 1)
            src->threads = cv_threads_for (src, &src->threads_max);
        src->hw_wanted = FALSE;
        gboolean hw = attempt == 0 && cv_hw_setup (src, codec);
        gboolean wanted = src->hw_wanted || attempt > 0;
        if (hw) {
            /* the GPU decodes: one thread feeds it */
            src->dec->hw_device_ctx = av_buffer_ref (src->hw_dev);
            src->dec->get_format = cv_get_format;
            src->dec->opaque = src;
            src->dec->thread_count = 1;
        } else {
            src->dec->thread_count = src->threads;
            src->dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
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
            if (hw && first)
                g_message ("video: %dx%d %.2f fps, hardware decoding (%s)", src->demux->width,
                           src->demux->height, cv_clip_fps (src->demux), src->hw_name);
            else if (first)
                g_message ("video: %dx%d %.2f fps, %d decode thread(s)%s", src->demux->width,
                           src->demux->height, cv_clip_fps (src->demux), src->dec->thread_count,
                           src->dec->skip_loop_filter == AVDISCARD_ALL ? ", deblocking SKIPPED"
                           : src->dec->skip_loop_filter == AVDISCARD_NONREF
                               ? ", deblocking SKIPPED for non-reference pictures" : "");
            cv_note_decoder (src, !hw && wanted);
            return TRUE;
        }
        avcodec_free_context (&src->dec);
        if (!hw)
            break;
        g_message ("video: the %s decoder could not be opened: decoding on the CPU", src->hw_name);
        cv_hw_set_why (src, "the %s decoder could not be opened", src->hw_name);
        cv_hw_close (src);
        src->hw_off = TRUE;
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

/* the hardware decoder gave up (or refuses the stream): software decoding
 * from the keyframe before the last frame that was queued */
static gboolean cv_hw_fallback (CvSource *src, const char *why)
{
    g_message ("video: hardware decoding (%s) failed for %s (%s): decoding on the CPU",
               src->hw_name ? src->hw_name : "?", src->path, why);
    cv_hw_set_why (src, "%s: %s", src->hw_name ? src->hw_name : "?", why);
    double raw = src->last_raw, play = src->last_play;
    guint cursor = 0, target = 0;
    gboolean exact = cv_restart_point (src, raw, G_MAXUINT, &cursor, &target);
    src->skip_idx = target;
    av_frame_unref (src->frame);
    av_frame_unref (src->swframe);
    avcodec_free_context (&src->dec);
    cv_hw_close (src);
    src->hw_off = TRUE;
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
 * set hwdec=on|off, the switch in the settings): reopen the decoder the
 * other way and continue after the last frame that was queued (the
 * frames since the last keyframe are decoded again: one short stop). A
 * decoder that cannot be opened in hardware goes
 * on in software (cv_decoder_open). A parked clip reads the setting when
 * it resumes. */
static gboolean cv_hw_switch (CvSource *src)
{
    gboolean want = cv_hw_asked (NULL);
    if (want == src->hw_asked)
        return TRUE;
    src->hw_asked = want;
    if (want ? (src->hw_dev != NULL || !src->gpu_yuv) : src->hw_dev == NULL) {
        /* nothing to change (software GL never decodes in hardware) */
        if (want && !src->gpu_yuv)
            cv_hw_set_why (src, "the renderer is a software renderer");
        cv_note_decoder (src, want && !src->hw_dev);
        return TRUE;
    }
    double raw = src->last_raw, play = src->last_play;
    guint cursor = 0, target = 0;
    gboolean exact = raw >= 0 && cv_restart_point (src, raw, G_MAXUINT, &cursor, &target);
    g_message ("video: hardware decoding switched %s for %s", want ? "on" : "off", src->path);
    src->skip_idx = target;
    av_frame_unref (src->frame);
    av_frame_unref (src->swframe);
    avcodec_free_context (&src->dec);
    cv_hw_close (src);
    src->hw_off = FALSE;    /* asked again: try again */
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
    if (src->hw_dev || src->raise_to > 0 || src->threads >= src->threads_max)
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
    cv_threads_learn (src->path, to);
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
        if (src->hw_dev && (g_atomic_int_get (&src->hw_refused) || src->hw_errors >= 3 ||
                            hw_bad >= 8)) {
            char why[160];
            if (g_atomic_int_get (&src->hw_refused))
                g_snprintf (why, sizeof why, "the device does not decode this stream (profile, "
                            "size or bit depth)");
            else if (src->hw_errors >= 3)
                g_snprintf (why, sizeof why, "%d pictures in a row could not be downloaded: %s",
                            src->hw_errors, av_err2str (hw_err));
            else
                g_snprintf (why, sizeof why, "%d decode calls in a row failed: %s", hw_bad,
                            av_err2str (hw_err));
            hw_bad = 0;
            if (!cv_hw_fallback (src, why))
                break;
        }
        if (!cv_hw_switch (src))
            break;
        cv_threads_raise (src);
        /* more threads from here on: the decoder is told that its input
         * ended, the frames it still holds come out below as always, and
         * the new decoder takes the next sample */
        gboolean drain = src->raise_to > 0 && !src->hw_dev && src->skip_until < 0 &&
                         cv_raise_here (src);
        /* (declared in front of the drain pass's jump: both ways into the
         * receive loop start without a picture) */
        gboolean got_frame = FALSE;
        if (src->raise_to > 0 && src->hw_dev)
            src->raise_to = 0;      /* the GPU decodes now */

        double c0 = cv_cpu ();
        const Mp4Sample *smp = drain ? NULL : mp4_demux_next (src->demux);
        gboolean at_start = !src->sent_sps;
        if (drain) {
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
            static int sp_err_n;
            if (sp_err_n++ < 5)
                g_warning ("send_packet failed: %d (%s)", sp_rc, av_err2str (sp_rc));
            if (src->hw_dev) {
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
            src->st_decode += cv_cpu () - c2;
            if (rf_rc < 0) {
                if (src->hw_dev && rf_rc != AVERROR (EAGAIN) && rf_rc != AVERROR_EOF) {
                    hw_bad++;
                    hw_err = rf_rc;
                }
                break;
            }
            got_frame = TRUE;
            hw_bad = 0;
            if (src->hw_dev && src->frame->format == src->hw_fmt) {
                /* the picture is in GPU memory: download it (NV12 / P010) */
                double c3 = cv_cpu ();
                int rc = cv_hw_download (src);
                if (rc >= 0)
                    rc = av_frame_copy_props (src->swframe, src->frame);
                av_frame_unref (src->frame);
                if (rc < 0) {
                    src->hw_errors++;
                    hw_err = rc;
                    av_frame_unref (src->swframe);
                    break;
                }
                src->hw_errors = 0;
                av_frame_move_ref (src->frame, src->swframe);
                src->st_copied += (double) src->frame->width * src->frame->height * 1.5;
                src->st_prep += cv_cpu () - c3;
                if (!src->hw_seen) {
                    /* the device really decodes this stream: say so */
                    src->hw_seen = TRUE;
                    g_clear_pointer (&src->hw_why, g_free);
                    cv_note_decoder (src, FALSE);
                }
            } else if (src->hw_dev && !g_atomic_int_get (&src->hw_refused)) {
                /* a hardware decoder that hands out pictures in main memory
                 * is not decoding on the GPU: treat it as a refusal */
                g_atomic_int_set (&src->hw_refused, 1);
            }
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
            static int test_nv = -1;
            if (test_nv < 0)
                test_nv = g_getenv ("WALPE_VIDEO_TEST_NV12") ? 1 : 0;
            if (test_nv)
                cv_test_nv12 (src);
            gboolean conv = cv_fill_frame (src, f, &copied);
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
                    static int dlog_n;
                    if (dlog_n++ < 4)
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
                    /* (the upload counters are the render thread's: read here
                     * without its leave, good enough for a log line) */
                    g_string_append_printf (ls, ", upload path %s%s: %u texture and %u buffer "
                                            "calls, %u maps, %u fences, %.2f MB copied by the render "
                                            "thread, %u of %u frames from buffers the decoder wrote",
                                            cv_up_name (vw->up_real),
                                            vw->up_mode > 0 && (vw->up_mode & WALPE_VUP_PINGPONG)
                                                ? "+pingpong" : "",
                                            vw->up_tex_calls, vw->up_buf_calls, vw->up_maps,
                                            vw->up_fences, vw->up_copied / 1048576.0, vw->up_staged,
                                            vw->up_frames);
                    vw->up_frames = vw->up_tex_calls = vw->up_buf_calls = vw->up_maps = 0;
                    vw->up_fences = vw->up_staged = vw->up_waits = 0;
                    vw->up_copied = 0;
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
                                        src->hw_dev ? ", hardware " : "",
                                        src->hw_dev ? src->hw_name : "",
                                        src->st_prep * 1e3 / nfr,
                                        src->st_copied / 1048576.0 / nfr);
                src->st_workers = wk;
                src->st_demux = src->st_decode = src->st_prep = src->st_copied = 0;
                if (src->hw_dev && src->dl_reused + src->dl_fresh > 0)
                    g_string_append_printf (ls, "; downloads into the kept frame %u, into a new "
                                            "one %u", src->dl_reused, src->dl_fresh);
                src->dl_reused = src->dl_fresh = 0;
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
} CvProg;
static __thread CvProg cv_prog[2];
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

static CvProg *cv_program (int kind)
{
    CvProg *p = &cv_prog[kind == CV_NV ? 1 : 0];
    if (p->prog)
        return p;
    GLuint vs = cv_compile (GL_VERTEX_SHADER, "", CV_VS);
    GLuint fs = cv_compile (GL_FRAGMENT_SHADER, kind == CV_NV ? "#define CV_NV 1\n" : "", CV_FS);
    p->prog = glCreateProgram ();
    glAttachShader (p->prog, vs);
    glAttachShader (p->prog, fs);
    glBindAttribLocation (p->prog, 0, "a_pos");
    glBindAttribLocation (p->prog, 1, "a_uv");
    glLinkProgram (p->prog);
    glDeleteShader (vs);
    glDeleteShader (fs);
    p->mvp = glGetUniformLocation (p->prog, "u_mvp");
    p->t[0] = glGetUniformLocation (p->prog, "u_y");
    p->t[1] = glGetUniformLocation (p->prog, "u_u");
    p->t[2] = glGetUniformLocation (p->prog, "u_v");
    p->ky = glGetUniformLocation (p->prog, "u_ky");
    p->kc = glGetUniformLocation (p->prog, "u_kc");
    p->sc = glGetUniformLocation (p->prog, "u_s");
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
    CvProg *p = cv_program (vw->pkind);
    int n = vw->pkind == CV_NV ? 2 : 3;
    glUseProgram (p->prog);
    int k[6];
    walpe_yuv_coeffs (vw->ymode, k);
    glUniform2f (p->ky, (float) k[0], (float) k[1]);
    glUniform4f (p->kc, (float) k[2], (float) k[3], (float) k[4], (float) k[5]);
    glUniform1f (p->sc, vw->pscale);
    for (int i = 0; i < n; i++) {
        glActiveTexture (GL_TEXTURE0 + (GLenum) i);
        glBindTexture (GL_TEXTURE_2D, vw->ptex[i]);
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
    g_atomic_int_set (&vw->want_w, (int) ceilf (bw * k));
    g_atomic_int_set (&vw->want_h, (int) ceilf (bh * k));
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
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    if (f->kind == CV_BGRA) {
        glBindTexture (GL_TEXTURE_2D, vw->tex);
        glPixelStorei (GL_UNPACK_ROW_LENGTH, f->ls[0] / 4);
        /* (re)allocate storage only when needed; frames reuse it via SubImage */
        if (!vw->tex_alloced || vw->tex_w != f->w || vw->tex_h != f->h) {
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, f->w, f->h, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                          NULL);
            vw->tex_alloced = TRUE;
            vw->tex_w = f->w;
            vw->tex_h = f->h;
        }
        glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, f->w, f->h, GL_BGRA, GL_UNSIGNED_BYTE, f->pl[0]);
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
    cv_up_sync (vw);        /* the upload path in force; nothing is pinned yet */
    /* measuring aid (WALPE_VIDEO_UPLOAD=hold): the first frame stays */
    gboolean hold = (vw->up_mode & WALPE_VUP_HOLD) && vw->drawn_seq;
    g_mutex_lock (&src->lock);
    if (vw->measured)
        cv_viewer_tick (vw, cv_now ());
    double next_due = 0;
    CvFrame *f = src->has_frame ? cv_pick_frame (src, &next_due) : NULL;
    if (f && vw->drawn_seq != f->seq && !hold) {
        vw->shown++;
        if (vw->drawn_seq && f->seq > vw->drawn_seq + 1)
            vw->skipped += (guint) (f->seq - vw->drawn_seq - 1);
        /* the upload runs without the lock: the frame is pinned, so it
         * stays in the queue, and the decode thread and the other
         * monitors do not wait behind this one */
        f->pins++;
        g_mutex_unlock (&src->lock);
        double c0 = cv_cpu ();
        double bytes = cv_upload_frame (vw, f);
        double c1 = cv_cpu ();
        if (f->seq <= (guint64) cv_md5_n ())
            cv_frame_md5 (f, "upload", src->path);
        g_mutex_lock (&src->lock);
        vw->drawn_seq = f->seq;
        vw->st_upload += c1 - c0;
        vw->st_bytes += bytes;
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
    g_mutex_unlock (&src->lock);
    g_mutex_unlock (&registry_lock);

    if (last) {
        g_debug ("video: shared decoder closed (%s)", src->path);
        g_thread_join (src->thread);
        cv_notes_set (src, NULL);
        for (int i = 0; i <= CV_AHEAD; i++) {
            av_frame_free (&src->buf[i].av);
            g_free (src->buf[i].data);
        }
        mp4_demux_free (src->demux);
        av_packet_free (&src->pkt);
        av_frame_free (&src->frame);
        av_frame_free (&src->swframe);
        avcodec_free_context (&src->dec);
        cv_hw_close (src);
        av_frame_free (&src->dl_keep);
        g_free (src->hw_why);
        g_free (src->half_buf);
        g_free (src->norm_buf);
        if (src->annexb)
            g_string_free (src->annexb, TRUE);
        g_free (src->path);
        g_mutex_clear (&src->lock);
        g_cond_clear (&src->cond);
        g_ptr_array_free (src->viewers, TRUE);
        g_free (src);
    }
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

    g_mutex_lock (&src->lock);
    src->refs++;
    g_ptr_array_add (src->viewers, vw);
    cv_recompute_paused (src);
    g_mutex_unlock (&src->lock);
    return vw;
}

/* ------------------------------------------------------- source setup */

static CvSource *cv_source_create (const char *key, const char *path, guint64 off,
                                   guint64 len, GError **err)
{
    CvSource *src = g_new0 (CvSource, 1);
    src->path = g_strdup (key);
    src->viewers = g_ptr_array_new ();
    g_mutex_init (&src->lock);
    g_cond_init (&src->cond);
    src->hw_fmt = AV_PIX_FMT_NONE;
    src->gl_r16 = -1;
    src->last_raw = src->last_play = -1;
    src->skip_until = -1;
    src->park_raw = -1;
    src->drop_raw_max = -1;
    src->rebase_raw = -1;

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
    {
        /* frame limit: the clip's frame interval and its first time stamp */
        double fps = cv_clip_fps (src->demux);
        src->clip_iv = fps > 0 ? 1.0 / fps : 0;
        gint64 t0 = G_MAXINT64;
        for (guint i = 0; i < src->demux->nsamples; i++)
            t0 = MIN (t0, cv_sample_ticks (src->demux, i));
        src->clip_t0 = src->demux->nsamples && src->demux->timescale
            ? (double) t0 / (double) src->demux->timescale : 0;
    }
    src->dl_keep = av_frame_alloc ();
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
    g_free (src->hw_why);
    g_free (src->path);
    g_ptr_array_free (src->viewers, TRUE);
    g_cond_clear (&src->cond);
    g_mutex_clear (&src->lock);
    g_free (src);
    return NULL;
}

static CvViewer *cv_attach (const char *path, guint64 off, guint64 len, GError **err)
{
    char *key = off || len ? g_strdup_printf ("%s@%" G_GUINT64_FORMAT "+%" G_GUINT64_FORMAT,
                                              path, off, len)
                           : g_strdup (path);
    g_mutex_lock (&registry_lock);
    if (!registry)
        registry = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    CvSource *src = g_hash_table_lookup (registry, key);
    if (!src) {
        src = cv_source_create (key, path, off, len, err);
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
    CvViewer *vw = cv_attach (path, 0, 0, err);
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

WalpeVideoTex *walpe_video_tex_new (const char *path, guint64 off, guint64 len, GError **err)
{
    CvViewer *vw = cv_attach (path, off, len, err);
    if (!vw)
        return NULL;
    WalpeVideoTex *v = g_new0 (WalpeVideoTex, 1);
    v->vw = vw;
    g_mutex_lock (&vw->src->lock);
    vw->measured = TRUE;        /* frame limit: its rate is the scene's frame rate */
    g_mutex_unlock (&vw->src->lock);
    return v;
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

/* walpe-video.so entry point (modules.h): everything above is module-local */
WALPE_API const WalpeVideoModule *walpe_video_module_entry (void)
{
    static const WalpeVideoModule m = {
        WALPE_MODULE_ABI, walpe_video_custom_new, walpe_video_tex_new, walpe_video_tex_update,
        walpe_video_tex_interval_us, walpe_video_tex_set_view, walpe_video_tex_set_paused,
        walpe_video_tex_free,
    };
    return &m;
}
