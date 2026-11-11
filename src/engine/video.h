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
};

/* Hardware video decoding (config "video_hwdec", walpe-ctl set hwdec=on|off;
 * off by default): the daemon stores the setting here, the video module
 * reads it when it opens a decoder. 0 = software decoding (libavcodec on
 * the CPU), 1 = try the GPU's decoder first (NVDEC through FFmpeg's "cuda"
 * hwaccel, else VA-API, else VDPAU) and fall back to software when it is
 * not available or fails. WALPE_VIDEO_HWDEC=0|1|cuda|vaapi|vdpau overrides. */
WALPE_API int walpe_video_hwdec (void);
void walpe_video_set_hwdec (int on);
/* How the video decoder opened last works, in words ("3840x2160 60.00 fps:
 * hardware (cuda)", "1920x1080 30.00 fps: software, 1 thread"): the video
 * module says so whenever it opens a decoder (also after a fallback to
 * software), `walpe-ctl status` shows it as "video_decoder" - the way to
 * see whether hardware decoding is really in use. NULL = none opened yet;
 * the caller frees the copy. */
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

/* WalpeContent wrapper with backend selection + automatic fallback */
WalpeContent *walpe_video_content_new (const char *path, const WalpeLoadOpts *opts, GError **err);
