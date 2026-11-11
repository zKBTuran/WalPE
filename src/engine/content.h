/* content.h - wallpaper content abstraction: one object per rendered wallpaper */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include "common/quality.h"
#include "common/pace.h"   /* walpe_frame_grid_next */

typedef enum {
    WALPE_TYPE_NONE = 0,
    WALPE_TYPE_SCENE,
    WALPE_TYPE_VIDEO,
    WALPE_TYPE_GIF,
    WALPE_TYPE_IMAGE,
    /* unconditional: every TU must see the same enum whether or not it
     * includes the build config (only the factory is gated on HAVE_WEB) */
    WALPE_TYPE_WEB,
    WALPE_TYPE_UNSUPPORTED,
} WalpeType;

typedef struct WalpeContent WalpeContent;

enum { WALPE_PTR_MOVE = 0, WALPE_PTR_DOWN = 1, WALPE_PTR_UP = 2 };

struct WalpeContent {
    WalpeType type;
    char *path;         /* project dir or media file (absolute) */
    char *title;
    JsonObject *props;  /* project.json general.properties (borrowed, do not free) */
    JsonNode *root;     /* owner of props, may be NULL */

    /* GL lifecycle: called on the monitor's render thread with its EGL
     * context current (gl_init, gl_render, gl_free, set_paused, free all run
     * there; free may run synchronously - no main-loop source may keep a
     * pointer to the content) */
    gboolean (*gl_init) (WalpeContent *c, GError **err);
    /* px,py = pointer in [-1,1] (parallax); returns TRUE if a change was drawn */
    gboolean (*gl_render) (WalpeContent *c, int w, int h, float px, float py);
    void (*gl_free) (WalpeContent *c);
    /* called by the host (the daemon) after presenting the surface (eglSwapBuffers);
     * video backends use it to report the swap to mpv for pacing */
    void (*swapped) (WalpeContent *c);

    void (*set_paused) (WalpeContent *c, gboolean p);
    /* pointer input, render thread: kind WALPE_PTR_MOVE / _DOWN / _UP (left
     * button), x,y = position on the monitor in [0,1] from the top-left.
     * NULL = the content ignores it. wants_pointer is set by gl_init when
     * the content reacts (scene layers with SceneScript cursor events). */
    void (*pointer) (WalpeContent *c, int kind, float x, float y);
    gboolean wants_pointer;
    /* set by gl_init: the picture follows the pointer position every frame
     * (parallax/g_PointerPosition effects, scripts reading input.cursor*,
     * legacy particles, particle control points linked to the mouse): one
     * rendering cannot serve two monitors */
    gboolean uses_pointer_pos;
    void (*set_volume) (WalpeContent *c, double v); /* 0..1 */
    void (*free) (WalpeContent *c);

    gpointer priv;
    /* set by owner before gl_init; backends call request(user) when they
     * produced a new frame (gif timer, video decode thread, particles) */
    void (*request_redraw) (gpointer user);
    gpointer request_user;
    /* set by gl_render: monotonic time (us) at which the next frame is due
     * (animation clock), 0 = render only on request. The render thread
     * sleeps until then; paused monitors ignore it. */
    gint64 next_frame_us;
    /* render thread: new fps cap for a running wallpaper (walpe-ctl set
     * fps=N / GUI); NULL = the cap only applies at the next load */
    void (*set_fps) (WalpeContent *c, int fps);
    /* set by gl_render: nothing could be drawn yet (a video whose first
     * frame is still being decoded). The host does not present the
     * buffer and paints the first frame when it arrives, also while the
     * monitor is paused. */
    gboolean first_frame_pending;
    /* Frame skipping (scenes). A gl_render call that finds the picture
     * unchanged since the frame it drew last (nothing animated moved: a
     * clock between two seconds, audio bars in silence, a particle system
     * with no particle alive) draws nothing and sets frame_unchanged: the
     * owner then presents nothing - the surface keeps showing that frame -
     * and publishes nothing. Every gl_render call sets or clears it.
     * surface_lost: set by the host before a gl_render call whose picture
     * must be drawn in any case, because the surface does not show the
     * content's last drawn frame (it was drawn but never presented there,
     * the surface was resized or recreated); gl_render clears it. Contents
     * that never skip ignore both. */
    gboolean frame_unchanged;
    gboolean surface_lost;
    /* set by the host together with surface_lost when the surface lost
     * pixels because something covered a part of it (X Expose): nothing
     * the content copied from the surface earlier is to be trusted either
     * (the static bake is taken anew). gl_render clears it. */
    gboolean surface_exposed;
    /* render thread: bumped by whoever changes what the content's layers
     * look like from outside its own state - a live setting that alters
     * the picture (effect resolution, quality). It is part of the frame's
     * signature: the next frame is drawn and nothing kept from earlier
     * frames (the static bake) is reused. Layer properties, effect
     * constants, text, sizes and the audio / pointer values need no bump:
     * the signature holds them. (Something that only limits where the
     * frame is drawn, like a clip region, belongs in the frame's signature
     * alone: see scenenode_gl_render, below view_sig.) */
    guint picture_serial;
    /* render thread: internal render resolution of a scene in percent of
     * the monitor's (config render_scale; 100 = native, the default). NULL
     * = the content has no such thing. */
    void (*set_render_scale) (WalpeContent *c, int percent);
    /* Quality knobs (common/quality.h), render thread. set_quality: new
     * settings for a running wallpaper (effect update rate, post-
     * processing; the frame rate limits arrive through set_fps, the
     * texture resolution with the next load, the render scale through
     * set_render_scale). NULL = the content has no such knobs.
     * quality_stats: what its frames cost now and what each knob would
     * save (FALSE = no numbers yet); stats_wanted, set by the host while
     * somebody looks at them, lets the content count pixels on the GPU.
     * quality_describe: how the knobs apply here, one line per effect
     * (g_free; NULL = nothing to say). */
    void (*set_quality) (WalpeContent *c, const WalpeQuality *q);
    gboolean (*quality_stats) (WalpeContent *c, WalpeQualityStats *out);
    char *(*quality_describe) (WalpeContent *c);
    gboolean stats_wanted;
    /* Render resolution (common/resolution.h), set by the owner before
     * gl_init and before every gl_render call (module ABI 11): the size
     * chosen for this monitor's picture when it is not the surface's own;
     * 0, 0 = the surface's size. gl_render is still called with the
     * surface's size and still fills the whole surface: what the chosen
     * size means is the content's to decide. A scene draws its frame at
     * that size and scales it to the surface (the size is "the monitor"
     * for its effects and textures); a video shows its frames as they are
     * and keeps them as large as the chosen size needs; a web page takes
     * it as its page size; a picture is stored at it. A content that
     * ignores the fields shows what it always did.
     * scale_filter: WALPE_SCALE_*, how a frame drawn at the chosen size is
     * scaled to the surface.
     * tex_limit: no texture of the wallpaper is kept longer than this many
     * pixels on its longer side (config texture_limit; 0 = no cap). A
     * scene applies it once its first frames are drawn; the owner loads
     * the wallpaper again when it changes. */
    int render_w, render_h;
    int scale_filter;
    int tex_limit;
};

typedef struct {
    const char *video_backend; /* "mpv" | "ffmpeg" */
    gboolean audio;
    int fps;                   /* scene fps cap */
    const char *origin_dir;    /* the wallpaper path as opened (project dir or
                                * media file): user-property override key and
                                * WE-install asset hint; NULL = derived */
    /* WE preset (a workshop item whose project.json has "dependency" +
     * "preset" instead of content): the base item is loaded with these
     * property values over its defaults (user overrides still win, keyed
     * by origin_dir = the preset). preset_dir resolves "files/..." values.
     * Both NULL for ordinary wallpapers; borrowed for the load call. */
    JsonObject *preset;
    const char *preset_dir;
    /* config tex_downscale off (or WALPE_TEX_DOWNSCALE=0): keep every layer
     * texture at its file size */
    gboolean no_tex_downscale;
    /* config texture_slack 0 (or WALPE_TEX_SLACK=0): reduced textures and
     * effect targets at exact sizes, never magnified by the 2 % tolerance */
    gboolean no_tex_slack;
    /* config render_scale: scenes render at this percentage of the
     * monitor's resolution and are scaled up when shown (25..100; 0 = 100) */
    int render_scale;
    /* the quality knobs the wallpaper starts with (NULL = High: every
     * knob at its default); borrowed for the load call */
    const WalpeQuality *quality;
    /* video wallpapers (module ABI 11): the box the converted copy of
     * this wallpaper is made for - config video_resolution and the render
     * sizes of the monitors that show it (common/resolution.h
     * walpe_video_box; 0 = no limit). The copy's cache name carries it
     * (common/import.h walpe_video_cache_file). */
    int video_box_w, video_box_h;
} WalpeLoadOpts;

/* phase-locked frame clock for gl_render: returns the next deadline one
 * interval after the previous one (so render time does not stretch the
 * period: a 60 fps cap really presents ~60 fps), never in the past */
/* frame_align (gpu.conf / WALPE_FRAME_ALIGN / walpe-ctl set
 * frame_align=on|off; off by default): every frame clock runs on one grid
 * (pace.h walpe_frame_grid_next), so the monitors present together. */
extern gint walpe_frame_align_;   /* atomic; content.c */
void walpe_frame_align_set (gboolean on);
gboolean walpe_frame_align_get (void);
static inline gint64 walpe_frame_clock_next (gint64 *last_due, gint64 interval_us)
{
    gint64 now = g_get_monotonic_time ();
    if (g_atomic_int_get (&walpe_frame_align_)) {
        *last_due = walpe_frame_grid_next (now, *last_due, interval_us);
        return *last_due;
    }
    gint64 next = *last_due ? *last_due + interval_us : now + interval_us;
    if (next < now)
        next = now; /* behind: render now, don't burst to catch up */
    *last_due = next;
    return next;
}

WalpeType walpe_detect_type (const char *path, char **media_path /* out, g_free */);
/* WalPE's own options for a wallpaper, in project.json "properties" form
 * ({key: {type, text, value, ...}}): the alignment options of video, GIF
 * and image wallpapers (fit.h). They are stored and changed like the
 * wallpaper's own properties. NULL when the type has none. Unref. */
JsonNode *walpe_content_builtin_props (const char *path);
/* every option of the wallpaper at path, as a new "properties" object:
 * its project.json general.properties - for a WE preset the BASE item's
 * definitions with the preset's values as the defaults (a preset has no
 * definitions of its own, so it used to show no options at all) - plus
 * the built-in ones. *localization (may be NULL) receives a copy of the
 * matching general.localization. NULL when there is nothing. Unref both. */
JsonNode *walpe_content_property_defs (const char *path, JsonNode **localization);
/* the alignment options saved for the wallpaper opened as `origin` */
void walpe_content_fit (const char *origin, void *fit /* WalpeFit * */);
/* a preset item's base wallpaper dir: project.json "dependency" looked up
 * next to the preset (same library), in the Steam workshop libraries and
 * in the WalPE library. NULL when path is no preset or the base is not
 * installed (*dep_id, if given, receives the dependency id: g_free). */
char *walpe_preset_base_dir (const char *path, char **dep_id);
WalpeContent *walpe_content_new (const char *path, const WalpeLoadOpts *opts, GError **err);
const char *walpe_type_name (WalpeType t);
