/* videotex.h - video as a scene texture (WE .tex with an embedded MP4).
 *
 * Built on the shared decoder of video_custom.c: sources are keyed by
 * file + byte range, so every object and every monitor showing the same
 * video texture shares ONE demuxer + decoder thread; each user (one per
 * scene per monitor) owns a GL texture in its render thread's context and
 * uploads only when the shared frame advanced. The decoder loops the clip
 * and sleeps while every user is paused. HAVE_FFMPEG builds only; the
 * stubs in the !HAVE_FFMPEG case return NULL. */
#pragma once
#include <glib.h>
#include <epoxy/gl.h>

typedef struct WalpeVideoTex WalpeVideoTex;

/* any thread: open/attach the shared source (no GL). NULL on error. */
WalpeVideoTex *walpe_video_tex_new (const char *path, guint64 off, guint64 len, GError **err);
/* render thread: upload the newest decoded frame if it changed; returns
 * the GL texture (0 until the first frame arrived) and its size */
GLuint walpe_video_tex_update (WalpeVideoTex *v, int *w, int *h);
/* frame interval of the stream in µs (0 = not known yet) */
gint64 walpe_video_tex_interval_us (WalpeVideoTex *v);
/* on-screen size (pixels) the whole frame is drawn at this frame; 0 = full
 * resolution needed (effects sample it). <= half the video size on every
 * user lets the shared decoder produce half-size frames. */
void walpe_video_tex_set_view (WalpeVideoTex *v, int w, int h);
void walpe_video_tex_set_paused (WalpeVideoTex *v, gboolean paused);
/* render thread (deletes the GL texture), then detaches from the source */
void walpe_video_tex_free (WalpeVideoTex *v);

/* ---- played from a converted copy (common/scenevideo.h) ----------------
 * A clip larger than a scene shows it can be played from a smaller copy of
 * itself: the same pictures at the same times, converted once by the
 * daemon. The scene decides and says so here; the shared source plays
 *   - the clip itself as long as one of its users asks for it (or says
 *     nothing: walpe_video_tex_new),
 *   - else the largest copy its users name.
 * A source that is created starts with that file. One that is playing
 * changes over where the clip starts again: the last picture of one file
 * is followed by the first of the other, one frame interval later, on the
 * same clock - what a loop looks like anyway - so nothing jumps, and the
 * layer keeps its size (the scene knows the clip's own). A copy that is
 * not the clip again (another number of pictures, another frame rate,
 * larger, unreadable) is not used. */
enum {
    WALPE_VTEX_COPY_UNDECIDED = 0,  /* the scene has not looked yet */
    WALPE_VTEX_COPY_OFF,            /* "video_scene_convert" is off */
    WALPE_VTEX_COPY_NOT_NEEDED,     /* the clip is not (much) larger than it is shown */
    WALPE_VTEX_COPY_NO_LIMIT,       /* "video_resolution" is none */
    WALPE_VTEX_COPY_FULL_TEX,       /* the scene keeps its textures at file size */
    WALPE_VTEX_COPY_UNSUITED,       /* not 8-bit 4:2:0 in limited range: a copy would not be the same;
                                     * or stored rotated: its copy would not be played */
    WALPE_VTEX_COPY_WAITING,        /* asked of the daemon: `path` will be the copy */
    WALPE_VTEX_COPY_READY,          /* `path` is the copy to play */
};
typedef struct {
    int state;          /* WALPE_VTEX_COPY_* */
    const char *path;   /* WAITING, READY */
    int w, h;           /* the copy's size */
} WalpeVideoTexCopy;
/* walpe_video_tex_new with what this user wants played (NULL = the clip) */
WalpeVideoTex *walpe_video_tex_new_copy (const char *path, guint64 off, guint64 len,
                                         const WalpeVideoTexCopy *copy, GError **err);
/* any thread that owns v: this user wants something else now */
void walpe_video_tex_set_copy (WalpeVideoTex *v, const WalpeVideoTexCopy *copy);

typedef struct {
    int clip_w, clip_h;     /* the clip's own size, frame rate and number of pictures */
    double clip_fps;
    guint clip_frames;
    int play_w, play_h;     /* what is decoded now: the clip, or a copy of it */
    double play_fps;
    gboolean from_copy;
    /* what the decoder gave: 0 = no picture yet, 1 = 8-bit 4:2:0 in
     * limited range, 2 = anything else, 3 = like 1 in a clip that is stored
     * rotated (its copy would be stored rotated too and never played: none
     * is made); and the colour matrix the player converts it with (1
     * BT.709, 6 BT.601, 9 BT.2020) */
    int format;
    int matrix;
} WalpeVideoTexInfo;
void walpe_video_tex_info (WalpeVideoTex *v, WalpeVideoTexInfo *info);

/* For `walpe-ctl status`: one entry per scene video that is open,
 *   "<file>: source 3840x2160 60.00 fps, played 1920x1080 60.00 fps, copy: ready"
 * joined with "; ". After "copy:" - ready, off, not needed, not made (why),
 * not usable (why), or for a copy that is asked for what `job` answers for
 * its file (the daemon: "converting 42 %", "waiting", "failed: ..."; job
 * NULL: "wanted"). job is called with the module's locks held: it must not
 * call into the module. NULL = no scene video is open (also: the module is
 * not loaded). g_free. */
typedef char *(*WalpeVideoJobText) (const char *copy_path, gpointer user);
char *walpe_video_scene_note (WalpeVideoJobText job, gpointer user);
