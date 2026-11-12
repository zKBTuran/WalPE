/* video.h - video wallpaper backend (in-house MP4 demux + libavcodec H.264) */
#pragma once
#include "content.h"
#include "fit.h"
#include "common/api.h"

typedef struct WalpeVideo WalpeVideo;
struct WalpeVideo {
    gboolean (*gl_init) (WalpeVideo *v, GError **err);   /* GL context current */
    gboolean (*gl_render) (WalpeVideo *v, int w, int h, GError **err);
    void (*set_paused) (WalpeVideo *v, gboolean p);
    void (*set_volume) (WalpeVideo *v, double vol);      /* 0..1 */
    void (*gl_free) (WalpeVideo *v);
    void (*free) (WalpeVideo *v);
    /* called by the host after presenting the surface (eglSwapBuffers):
     * mpv needs mpv_render_context_report_swap for correct frame pacing */
    void (*on_swap) (WalpeVideo *v);

    gpointer priv;
    void (*request_redraw) (gpointer user);
    gpointer request_user;
    gint64 next_frame_us;   /* set by gl_render: presentation beat (0 = none) */
    /* set by the host before the first gl_render (module ABI 4): how the
     * frame is placed on the monitor (fit.h; zeroed = cover, centred once
     * walpe_video_content_new filled it in) */
    WalpeFit fit;
    /* set by gl_render (module ABI 5): nothing was drawn, the first frame
     * is not decoded yet */
    gboolean no_frame;
    /* set by the owner before every gl_render (module ABI 11): the render
     * resolution chosen for the monitor (WalpeContent.render_w / render_h;
     * 0 = the surface's size). Frames need not be kept larger than they
     * are shown at that size. */
    int render_w, render_h;
    /* set by gl_render (module ABI 14): the place in the clip of the
     * picture that is on this monitor - its own time stamp in seconds; it
     * falls back where the clip starts again. -1 = no picture yet, or one
     * without a time stamp. What the sound track follows (video.c). */
    double pos;
    /* set when the video is made (module ABI 14): how long one pass of the
     * clip takes, in seconds - its pictures times their interval; 0 = not
     * known */
    double period;
};

/* Hardware video decoding (config "video_hwdec": off | auto | <api>, and
 * walpe-ctl set hwdec=...; off by default). The daemon stores the setting
 * here, the video module reads it when it opens a decoder and follows a
 * change while a video plays.
 *   off     software decoding (libavcodec on the CPU)
 *   auto    (also: on) the hardware decoders FFmpeg in this build offers
 *           are tried in an order that depends on the machine, not on who
 *           made its GPU (one exception: where NVIDIA's own driver renders,
 *           "cuda" = NVDEC stays the first one): first those that can be
 *           opened on the GPU that renders the wallpaper, then those whose
 *           API picks the device,
 *           then other GPUs, then decoder blocks of their own (ARM boards;
 *           on x86-64 FFmpeg's V4L2 decoder is not part of auto: it would
 *           open every /dev/video* node - by name it is still taken);
 *           the rules are in video_hwsel.h. Anything that fails - no device,
 *           no permission, a profile or a size the device does not decode,
 *           an error in the middle of the clip - ends in software decoding
 *           at the same position
 *   <api>   only that one: FFmpeg's name of the device type ("vaapi",
 *           "cuda" = NVDEC, "vdpau", "vulkan", "drm" = V4L2 request API) or
 *           of a decoder of its own ("v4l2m2m", "rkmpp", ...)
 * walpe_video_hwdec: 0 = off, 1 = on (as before). walpe_video_hwdec_api:
 * the one API asked for, copied into buf ("" = auto); returns a number that
 * changes whenever the setting is set, so that a reader can tell a change
 * from "auto" to "vaapi" without comparing text every frame.
 * walpe_video_set_hwdec keeps its meaning (on = auto);
 * walpe_video_set_hwdec_name takes the setting's words and returns FALSE
 * for anything else (nothing is changed then).
 * WALPE_VIDEO_HWDEC=<the same words> in front of the daemon overrides. */
WALPE_API int walpe_video_hwdec (void);
void walpe_video_set_hwdec (int on);
gboolean walpe_video_set_hwdec_name (const char *name);
WALPE_API guint walpe_video_hwdec_api (char *buf, gsize len);
/* the setting as one word: "off", "auto" or the API (g_free) */
char *walpe_video_hwdec_setting (void);
/* Zero-copy for hardware-decoded pictures (config "video_zerocopy", walpe-ctl
 * set video_zerocopy=on|off; off by default; only with hardware decoding).
 * Off: every picture is downloaded to main memory and uploaded like a
 * software picture - the exact samples, on every driver. On: where the
 * decoder can export its pictures as DMA-BUFs and the renderer's EGL can
 * import them (EGL_EXT_image_dma_buf_import), the planes are used as
 * textures where they are; a stream whose imported planes are not, sample
 * for sample, what a download gives is refused and downloaded as before
 * (video_hwsel.h, WALPE_ZC_*). WALPE_VIDEO_ZEROCOPY=0|1 overrides. */
WALPE_API int walpe_video_zerocopy (void);
void walpe_video_set_zerocopy (int on);
/* What the decoders of the open videos do, in words: size, frame rate and
 * codec profile, then "hardware (<api> on <device> [<kernel driver>], <which
 * GPU>; <how the pictures reach the renderer>)" or "software, N threads" -
 * the latter followed, when hardware decoding was asked for, by every
 * attempt that failed and the kind of reason (codec unsupported, profile,
 * resolution outside the decoder's limits, no device, permission, ...;
 * video_hwsel.h has the exact form). The video module says so whenever it
 * opens a decoder (also after a fallback to software), `walpe-ctl status`
 * shows it as "video_decoder" - the way to see whether hardware decoding is
 * really in use. NULL = none opened yet; the caller frees the copy. */
WALPE_API void walpe_video_note_decoder (const char *text);
char *walpe_video_decoder_note (void);
/* "Videos follow the frame rate limit" (config "video_limit", walpe-ctl set
 * video_limit=on|off; off by default: videos keep their own frame rate, as
 * before). On: a video wallpaper is shown at most as often as the frame
 * rate limit of its monitor allows (the fps setting and the battery /
 * focus / fps_idle limits, like a scene), and a video - a wallpaper or a
 * scene's video layer - whose monitors show fewer pictures a second than
 * the clip has decodes only what they show: pictures off that grid are not
 * decoded when no other picture is predicted from them, and are decoded
 * but not converted or uploaded otherwise. The pictures that are shown are
 * the clip's own, bit for bit. WALPE_VIDEO_LIMIT=0|1 overrides.
 * walpe_video_frame_limit: the frame rate limit of the monitor whose
 * render thread is calling (set around the video's gl_render; 0 = none). */
WALPE_API int walpe_video_limit (void);
void walpe_video_set_limit (int on);
WALPE_API int walpe_video_frame_limit (void);
/* How the planes of a video frame get into their textures (config
 * "video_upload", walpe-ctl set video_upload=NAME; WALPE_VIDEO_UPLOAD=NAME
 * in front of the daemon overrides). Every path leaves the same bytes in
 * the same textures (tests/video_test.c reads them back); they differ in
 * what the driver has to do per frame, which only a real GPU can time
 * (walpe-ctl profile, row "(scene update, uploads)"):
 *   legacy   glTexSubImage2D from main memory, as before (the default):
 *            the driver copies the planes inside the call
 *   pbo      one pixel buffer object per monitor, a new store every frame
 *            (glBufferData NULL), mapped and written, then the textures
 *            are filled from the buffer
 *   pbodata  the same with glBufferData / glBufferSubData instead of a map
 *   persist  a ring of three persistently mapped buffers per monitor
 *            (GL 4.4 / ARB_buffer_storage, else pbo), written by the
 *            render thread, a fence per buffer
 *   direct   the decode thread writes the frame straight into persistently
 *            mapped buffers (the 2x reduction of a 4K clip has them as its
 *            output; a full-size frame is copied there), the render thread
 *            only says "fill the textures from that buffer": no copy on the
 *            render thread, and one pass over the frame less on the
 *            half-size path
 *   +pingpong (with any of them, "direct+pingpong", or alone = legacy):
 *            two sets of plane textures, filled in turn, so an upload never
 *            goes to textures the last frame was drawn from
 *   hold     (WALPE_VIDEO_UPLOAD only, a measuring aid: the picture stands
 *            still) only the first frame is uploaded: what the profiler's
 *            row still costs is everything but the uploads
 * walpe_video_upload_parse: -1 = not such a name. */
#define WALPE_VUP_LEGACY   0
#define WALPE_VUP_PBO      1
#define WALPE_VUP_PBODATA  2
#define WALPE_VUP_PERSIST  3
#define WALPE_VUP_DIRECT   4
#define WALPE_VUP_BASE     0x0f
#define WALPE_VUP_PINGPONG 0x10
#define WALPE_VUP_HOLD     0x20
int walpe_video_upload_parse (const char *name);
gboolean walpe_video_set_upload (const char *name);
WALPE_API int walpe_video_upload_mode (void);
/* what the upload path opened last really is (a path the GL context cannot
 * do falls back): `walpe-ctl status` shows it as "video_upload_now" */
WALPE_API void walpe_video_note_upload (const char *text);
char *walpe_video_upload_note (void);
/* "A video texture got a new frame", per render thread: the video module
 * calls walpe_video_note_frame from walpe_video_tex_update when (and only
 * when) that call uploaded another frame than the texture held; the scene
 * compares walpe_video_frames_noted before and after its calls to know
 * which of its video layers changed (scenenode.c, SceneVideo.serial). A
 * paused video and a frame that is still current note nothing. */
WALPE_API void walpe_video_note_frame (void);
guint walpe_video_frames_noted (void);

/* backend constructors: open the file, spawn threads if any.
 * GL stuff happens in gl_init() so backends can be swapped until then. */
WalpeVideo *walpe_video_custom_new (const char *path, GError **err); /* HAVE_FFMPEG only */
/* `walpe-ctl mem` (memreport.h): what the video module holds, summed over
 * the clips it plays (a clip = one demuxer + decoder, shared by everything
 * that shows it: video wallpapers and the video textures of scenes). Read
 * when asked, any thread. The decoder's own pictures (libavcodec's frame
 * pool: reference pictures and one set per decoding thread) are not in
 * these numbers: `pool_estimate_bytes` says what the stream allows them to
 * be - (16 reference pictures at most + the threads) times one picture. */
typedef struct {
    int sources;              /* clips open */
    int viewers;              /* monitors / scene layers showing them */
    int decoders;             /* ... whose decoder is open (not parked) */
    int parked;               /* ... parked: every viewer paused for a while */
    int paused;               /* ... with every viewer paused */
    int hardware;             /* decoders that decode on the GPU */
    int threads;              /* software decode threads, summed */
    int queued, queue_slots;  /* pictures decoded ahead / room for them */
    gint64 frame_bytes;       /* main memory of the pictures in those slots */
    gint64 scratch_bytes;     /* conversion and packet buffers */
    int stage_buffers;        /* persistently mapped upload buffers */
    gint64 stage_bytes;
    guint64 decoded;          /* pictures queued since the clips were opened */
    guint64 opened;           /* decoders opened since the module was loaded */
    int width, height;        /* the largest clip */
    gint64 pool_estimate_bytes;
} WalpeVideoMemStats;
/* FALSE = the module is not loaded (nothing to report) */
gboolean walpe_video_mem_stats (WalpeVideoMemStats *out);

/* WalpeContent wrapper with backend selection + automatic fallback */
WalpeContent *walpe_video_content_new (const char *path, const WalpeLoadOpts *opts, GError **err);
