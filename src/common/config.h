/* config.h - persistent user config (~/.config/walpe/config.json) */
#pragma once
#include <glib.h>
#include "common/api.h"
#include "common/quality.h"

/* "Skip hidden areas" (config skip_hidden, JSON "off" / "auto" / "always";
 * opt-in):
 *  off    - the whole wallpaper is rendered. Default.
 *  auto   - the parts of a monitor that windows hide are not rendered, but
 *           only while no compositing manager runs (then "hidden" is exact:
 *           the X server shows no pixel of the wallpaper there).
 *  always - also under a compositing manager; windows without an alpha
 *           channel then count as opaque. The manager's own transparency
 *           rules cannot be read through X (see daemon/vistrack.h), so
 *           this is for setups where only ARGB windows are see-through. */
typedef enum {
    WALPE_SKIP_OFF = 0,
    WALPE_SKIP_AUTO = 1,
    WALPE_SKIP_ALWAYS = 2,
} WalpeSkipHidden;
#define WALPE_SKIP_MARGIN_DEFAULT 32
const char *walpe_skip_hidden_name (int v);
/* "off" / "auto" / "always", also 0 / 1 / 2 and false / true (= auto);
 * anything else gives dflt */
int walpe_skip_hidden_parse (const char *s, int dflt);

typedef struct WalpeConfig_ {
    char   *wallpapers_dir;   /* library root */
    char   *current;          /* active wallpaper path ("" = none): the default for
                               * every monitor without an entry in monitor_current */
    GHashTable *monitor_current; /* output name (e.g. "HDMI-0") -> wallpaper path
                                  * shown on that monitor instead of `current`
                                  * (owned strings; "monitor_current" in the JSON) */
    char   *video_backend;    /* "mpv" | "ffmpeg" */
    int     fps;              /* fps cap (scenes, web capture); each monitor is
                               * also limited to its refresh rate */
    int     config_version;   /* WALPE_CONFIG_VERSION once loaded; the file's
                               * "config_version" (absent = 1: versions up to 0.11) */
    gboolean fps_migrated;    /* this load replaced an older version's default
                               * (see walpe_config_migrate_fps); the daemon saves */
    int     fps_migrated_from; /* the value it replaced (for the log line) */
    char   *broken_copy;      /* config.json could not be read at this load: the
                               * damaged file was kept under this name (else NULL) */
    double  volume;           /* 0..1 */
    gboolean paused;
    gboolean pause_fullscreen;
    gboolean pause_dpms;      /* pause while the display is in a power-save state */
    gboolean pause_covered;   /* pause per-monitor while windows hide the wallpaper */
    char   *transparent_windows; /* comma-separated WM_CLASS list that never count
                                  * as covering (wallpaper shows through), e.g. "st" */
    gboolean cover_transparent; /* transparent_windows / _NET_WM_WINDOW_OPACITY
                                  * windows still count as covering (on: a
                                  * see-through terminal over the wallpaper pauses
                                  * it too; off: they never cover) */
    gboolean render_shared;   /* same-size monitors showing one scene: render it
                               * once and present it on each (version 0.10) */
    gboolean fx_cache;        /* reuse static effect chain output (version 0.10) */
    gboolean fx_specialize;   /* effect pass programs: compile constant material
                               * values in (each such program is compared with
                               * the plain one before it is used). Default off:
                               * one program per pass, exactly as in version 0.12 */
    char   *fx_quality;       /* "exact" (default): the picture of the generic
                               * shaders up to float rounding; "fast": cheaper
                               * shader variants that may differ slightly */
    gboolean tex_downscale;   /* store oversized plain textures downscaled */
    int texture_slack;        /* per cent a reduced texture / an effect's output
                               * may be magnified when drawn before the next
                               * larger size is used: 2 (default) or 0 = exact
                               * sizes (more GPU memory and fill, see
                               * scenenode.c tex_slack) */
    int render_scale;         /* scenes: internal render resolution in percent
                               * of the monitor's (25..100; 100 = native, the
                               * default). Lower = less GPU work per frame,
                               * a softer picture (scaled up when shown). */
    gboolean web_accel;       /* web pages: WebKit hardware acceleration (off by
                               * default; read at daemon start, see main.c) */
    gboolean video_hwdec;     /* video wallpapers: decode on the GPU (NVDEC / VA-API /
                               * VDPAU through FFmpeg) when it works; off = CPU */
    char    *video_upload;    /* how video frames are uploaded (engine/video.h:
                               * legacy | pbo | pbodata | persist | direct, each
                               * also "+pingpong"); NULL = legacy, the default */
    gboolean video_limit;     /* videos follow the frame rate limit and decode only
                               * the pictures that are shown (engine/video.h); off =
                               * videos keep their own frame rate (the default) */
    int     particle_lowres;  /* large soft particles (fog, glows) at reduced
                               * resolution: 0 off, 1 automatic (only where the
                               * picture stays the same; default), 2 everything
                               * drawn large ("particle_lowres": off|auto|all) */
    int     particle_density; /* percent of its particles every particle system
                               * emits, 5..100 (100 = as authored) */
    gboolean particle_instancing; /* sprites as instanced quads (default); off =
                               * six vertices a sprite, the submission before
                               * the instanced one (same picture, a way around
                               * a driver that mishandles instancing) */
    gboolean parallax;
    gboolean software_render; /* "Render on CPU": the daemon takes its GL from
                               * Mesa's llvmpipe instead of the GPU driver
                               * (common/swrender.h); off by default */
    int     software_threads; /* llvmpipe rasteriser threads in that mode;
                               * 0 = automatic (half of the CPUs, 1..8) */
    gboolean vsync;
    gboolean audio;           /* video wallpaper audio output */
    char   *we_assets;        /* Wallpaper Engine install assets/ dir (or the
                               * install root); NULL = auto-detect (env
                               * WALPE_WE_ASSETS wins, then Steam libraries) */
    char   *steam_user;       /* Steam account name for steamcmd workshop
                               * installs (never a password); NULL = unset */
    /* ---- resource policy (all default to "as before") */
    int     focus_action;     /* WALPE_FOCUS_*: what a monitor's wallpaper does
                               * while another application's window on that
                               * monitor has the focus */
    int     focus_fps;        /* the frame rate limit of WALPE_FOCUS_LIMIT */
    gboolean pause_battery;   /* pause every wallpaper while on battery power */
    int     battery_fps;      /* frame rate limit while on battery (0 = none) */
    int     preview_anim;     /* GUI, WALPE_PREVIEW_*: animated (GIF) previews */
    WalpeQuality quality;     /* quality-for-load knobs (quality.h); default High */
    int qsync_slack, qsync_scale; /* walpe_config_quality_sync: the values both
                               * sides had when they last agreed */
    GHashTable *wallpaper_quality; /* wallpaper path -> WalpeQualityOv (owned):
                                    * the knobs a wallpaper sets for itself
                                    * ("wallpaper_quality" in the JSON) */
    GHashTable *props;        /* Tier 4 wallpaper user-property overrides:
                                 key "<wallpaper path>\x1f<propid>" -> string
                                 (owned; values are strings, coerced by the
                                 property's declared type at consumption) */
    GHashTable *layers;       /* "Adjust layers": wallpaper path -> GPtrArray of
                                 WalpeLayerAdj (layeradj.h owns and locks it;
                                 "layers" in the JSON) */
    JsonObject *extra;        /* members of config.json this build does not
                                 know (written by a newer version, or by
                                 hand): kept and written back as they were.
                                 Every save writes the whole file, so they
                                 used to be gone after the first change of
                                 any setting. NULL = none */
    /* ---- opt-in settings for two monitors, all off by default: at the end,
     * so no member above moves */
    gboolean pointer_share;   /* a scene that follows the pointer, shown on
                               * same-size monitors, is rendered once: it
                               * follows the pointer on the monitor the pointer
                               * is on and the other monitors show that same
                               * picture (off: one rendering per monitor, each
                               * with its own pointer position, as before) */
    int     skip_hidden;      /* WalpeSkipHidden: do not render what other windows
                               * hide ("Skip hidden areas"; daemon/vistrack.h).
                               * Default off */
    int     skip_hidden_margin; /* under a compositing manager: pixels drawn around
                                 * what is visible (its blur reads past window
                                 * edges; a moved window uncovers fresh pixels) */
    int     skip_hidden_inset;  /* under a compositing manager: pixels taken off
                                 * every side of a hiding window (rounded corners) */
} WalpeConfig;

/* config.json format: 1 = versions up to 0.11 (no "config_version" key),
 * 2 = version 0.12 (fps 30 can only be the user's choice),
 * 3 = the frame rate limit defaults to 60 (fps 200 can only be the user's
 *     choice) */
#define WALPE_CONFIG_VERSION 3
/* the frame rate limit of a new install. 60, not the monitor's rate: at
 * the old default (200, i.e. the refresh rate) two 4K scenes on a 144 Hz
 * monitor were rendered 144 times a second and kept the GPU at 82 %.
 * Wallpapers that have a frame rate of their own keep it when it is lower
 * (a video its stream's, a GIF its frame delays); scenes and web pages
 * declare none in Wallpaper Engine's formats and run at the limit. The
 * user can still raise it (GUI, walpe-ctl set fps=N) up to WALPE_FPS_MAX;
 * a monitor never shows more than its refresh rate. */
#define WALPE_FPS_DEFAULT 60
#define WALPE_FPS_MAX 200
#define WALPE_FPS_LEGACY_DEFAULT 30     /* default up to version 0.9.0 */
#define WALPE_FPS_UNCAPPED_DEFAULT 200  /* default of versions 0.9.1 - 0.12 */
/* fps for a config.json of format `version`. Every save writes all keys,
 * so a config.json holds the default of the version that wrote it whether
 * or not the user ever touched the setting, and a later version cannot
 * tell "default" from "chosen" by the value alone. Hence, once per file:
 *  - version 1 with exactly 30 (the default until version 0.9.0), and
 *  - version 1 or 2 with exactly 200 (the default of versions 0.9.1 - 0.12, and
 *    what version 0.12's own migration wrote in place of 30)
 * are read as "never chosen" and become the current default; any other
 * value was set by the user and is kept. The file is then saved as
 * version 3, where 30 and 200 are values like any other (walpe-ctl set
 * fps=200 stays). The case this cannot tell apart: a user who set 200 (or,
 * before version 0.12, 30) on purpose gets the default once and has to set it
 * again. */
int walpe_config_migrate_fps (int version, int fps);

/* focus_action ("run" / "limit" / "pause" in config.json and IPC) */
enum { WALPE_FOCUS_RUN, WALPE_FOCUS_LIMIT, WALPE_FOCUS_PAUSE };
/* preview_anim ("always" / "hover" / "off") */
enum { WALPE_PREVIEW_ALWAYS, WALPE_PREVIEW_HOVER, WALPE_PREVIEW_OFF };
const char *walpe_focus_action_name (int v);
int         walpe_focus_action_parse (const char *s, int dflt);
const char *walpe_preview_anim_name (int v);
int         walpe_preview_anim_parse (const char *s, int dflt);
/* how many times walpe_config_save ran in this process (the daemon's
 * status carries it: a subscriber sees that settings changed) */
guint        walpe_config_save_count (void);

/* "particle_lowres" value <-> name; unknown names give `fallback` */
int          walpe_particle_lowres_from_name (const char *name, int fallback);
const char  *walpe_particle_lowres_name (int mode);

WalpeConfig *walpe_config_load  (void);
/* FALSE = config.json could not be written (a full disk, a directory that
 * cannot be written to): the settings in memory stay as they are and the
 * file as it was. walpe_config_save_error () then gives the reason, until
 * a save succeeds (NULL = the last save was written). */
gboolean     walpe_config_save  (WalpeConfig *cfg);
const char  *walpe_config_save_error (void);
void         walpe_config_free  (WalpeConfig *cfg);
/* wallpaper user-property override access. get returns a g_free() string
 * or NULL when unset; set with value=NULL removes the override. get uses
 * the config last loaded via walpe_config_load (the daemon's singleton). */
WALPE_API char        *walpe_config_get_prop (const char *path, const char *key);
void         walpe_config_set_prop (WalpeConfig *cfg, const char *path,
                                    const char *key, const char *value);
/* drop every override of one wallpaper; returns how many were removed */
guint        walpe_config_clear_props (WalpeConfig *cfg, const char *path);
/* per-monitor wallpaper: the path for output `name` (its own or `current`;
 * never NULL); set path NULL = follow `current` again */
const char  *walpe_config_monitor_path (WalpeConfig *cfg, const char *name);
void         walpe_config_set_monitor_path (WalpeConfig *cfg, const char *name,
                                            const char *path);
/* a wallpaper's own quality settings: NULL = none; set with a mask of 0
 * (or ov NULL) removes them; _for = what the wallpaper runs with (global
 * + its own); _forget removes the entry, returns 1 when there was one */
const WalpeQualityOv *walpe_config_quality_ov (WalpeConfig *cfg, const char *path);
void         walpe_config_set_quality_ov (WalpeConfig *cfg, const char *path,
                                          const WalpeQualityOv *ov);
void         walpe_config_quality_for (WalpeConfig *cfg, const char *path, WalpeQuality *out);
/* texture_slack / render_scale (fields) and quality.tex_slack /
 * quality.render_scale (knobs): the side that changed since the last call
 * wins, then both agree */
void         walpe_config_quality_sync (WalpeConfig *cfg);
guint        walpe_config_quality_forget (WalpeConfig *cfg, const char *path);
/* bit i set: config.json of this build carries keys[i] (n <= 32). The
 * presets set keys of other parts as data and name only those that exist. */
guint        walpe_config_has_keys (WalpeConfig *cfg, const char *const *keys, guint n);
const char  *walpe_default_dir  (void);          /* ~/.local/share/walpe/workshop */
char        *walpe_socket_path  (void);          /* $XDG_RUNTIME_DIR/walpe.sock, g_free */
/* what walpe_socket_path() answers when no directory of the user's own
 * exists for the socket (config.c): nothing can be created or reached
 * under this name, by anyone */
#define WALPE_SOCKET_NONE "/dev/null/walpe.sock"

G_BEGIN_DECLS
G_END_DECLS
