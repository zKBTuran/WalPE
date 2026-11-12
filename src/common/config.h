/* config.h - persistent user config (~/.config/walpe/config.json) */
#pragma once
#include <glib.h>
#include "common/api.h"
#include "common/quality.h"
#include "common/backend.h"   /* WalpeBackend: the "backend" key */
#include "common/resolution.h"

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
    double  volume;           /* 0..1: the volume of wallpaper sound (WALPE_VOLUME_DEFAULT
                               * until set) */
    gboolean scene_sound;     /* wallpaper sound: scenes play their sound layers
                               * (engine/scenesound.h). On by default; a wallpaper's
                               * own switch (its option "walpe_sound") wins over it */
    gboolean sound_migrated;  /* this load replaced the sound defaults of versions up
                               * to 0.14.0 (walpe_config_migrate_sound); the daemon
                               * says so and saves */
    gboolean mute;            /* wallpaper sound is silent; `volume` keeps its value */
    gboolean paused;
    gboolean pause_fullscreen; /* pause the monitor a focused fullscreen window is on */
    gboolean pause_fullscreen_all; /* ... and every other monitor with it, as before
                                    * 0.13 (off by default) */
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
    gboolean web_unload;      /* when no monitor shows a web wallpaper any more the
                               * daemon restarts in place, so WebKitGTK and GTK
                               * leave its memory (on by default; main.c) */
    gboolean video_hwdec;     /* videos: decode with a hardware decoder when one works
                               * (engine/video.h); off = on the CPU, the default */
    char    *video_hwdec_api; /* the one decoding API to use ("vaapi", "cuda", "vdpau",
                               * "vulkan", "drm", "v4l2m2m", ...); NULL or "" = auto:
                               * whatever the machine offers. The file's "video_hwdec"
                               * is off | auto | <api> (false / true are read as before) */
    gboolean video_zerocopy;  /* hardware-decoded pictures stay in GPU memory where
                               * that is possible and exact (engine/video.h); off =
                               * they are downloaded and uploaded, the default */
    char    *video_upload;    /* how video frames are uploaded (engine/video.h:
                               * legacy | pbo | pbodata | persist | direct, each
                               * also "+pingpong"); NULL = legacy, the default */
    gboolean video_limit;     /* videos follow the frame rate limit and decode only
                               * the pictures that are shown (engine/video.h); off =
                               * videos keep their own frame rate (the default) */
    gboolean video_scene_convert; /* a video inside a scene that is larger than the
                               * scene shows it is played from a converted copy of
                               * the size needed (common/scenevideo.h); on by default */
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
    char   *gpu;              /* which GPU renders (common/gpusel.h): "auto" (the
                               * default: the one that drives the displays),
                               * "integrated", "discrete", or one device by PCI
                               * address or name. Never NULL after a load.
                               * WALPE_GPU in the daemon's environment wins. */
    gboolean preset_follow_gpu; /* off (default): the quality preset is what the
                               * user set. on: when the kind of GPU that renders
                               * changes (first start with the switch on, another
                               * `gpu`, "Render on CPU") the preset is set once
                               * to the one that suits it (discrete: high,
                               * integrated: balanced, CPU: low) */
    char   *preset_gpu_class; /* the kind that preset was last set for
                               * ("discrete", "integrated", "software", ...);
                               * NULL = never. Written by the daemon. */
    int     gl_api;           /* WalpeGlApiPref (common/glapi.h): 0 = auto
                               * (desktop OpenGL 3.3 when the driver has it,
                               * else OpenGL ES 3.2 / 3.1), 1 = gl, 2 = gles.
                               * Saved as "gl_api": "gl" | "gles"; auto is
                               * not written */
    gboolean vsync;
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
    int     pause_idle;       /* pause every wallpaper after this many seconds
                               * without keyboard / pointer input; 0 = off (the
                               * default). Needs something that reports idle
                               * time: on Wayland the compositor's
                               * ext-idle-notify-v1 (status "pause_rules") */
    int     preview_anim;     /* GUI, WALPE_PREVIEW_*: animated (GIF) previews */
    WalpeQuality quality;     /* quality-for-load knobs (quality.h); default High */
    int qsync_slack, qsync_scale; /* walpe_config_quality_sync: the values both
                               * sides had when they last agreed */
    GHashTable *wallpaper_quality; /* wallpaper path -> WalpeQualityOv (owned):
                                    * the knobs a wallpaper sets for itself
                                    * ("wallpaper_quality" in the JSON) */
    GHashTable *features;     /* renderer feature switches that were set
                               * ("features" in the JSON): name (owned) ->
                               * GINT_TO_POINTER (1 = on, 2 = off); a name that
                               * is absent keeps its default (engine/features.h).
                               * NULL = none was set */
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
    /* ---- render resolution (common/resolution.h), format version 4 */
    GHashTable *monitors;     /* output name -> WalpeMonitorConf (owned): the
                               * render resolution and texture limit chosen
                               * for that output ("monitors" in the JSON). An
                               * output without an entry runs at native /
                               * auto and has not been asked about yet */
    gboolean resolution_prompt; /* the GUI may ask about outputs that were
                               * not asked about yet (default on) */
    gboolean resolution_migrate; /* the file was written by a version without
                               * "monitors": the outputs connected when the
                               * daemon next starts are entered as "asked,
                               * native": an existing installation is not
                               * asked about the screens it already had
                               * (walpe_config_monitors_migrate). Kept in
                               * the file until then */
    int     scale_filter;     /* WALPE_SCALE_*: how a frame rendered at
                               * another size than the output's is scaled */
    WalpeVideoRes video_res;  /* "video_resolution": the size videos are
                               * imported / converted to at most. auto on a
                               * new install; a file of an older version
                               * gets "1080p", the limit those had built in */
    /* ---- the window system (common/backend.h); at the end, so no member
     * above moves */
    int     backend;          /* WalpeBackend the daemon draws on: auto (default),
                               * x11 or wayland ("backend" in the JSON, written
                               * only when not auto). Read when the daemon starts */
    int     wayland_pointer;  /* Wayland: 0 = "auto", the wallpaper surface takes
                               * pointer input while a wallpaper uses the pointer
                               * (default); 1 = "off", it never does and every
                               * click on the desktop stays with the compositor
                               * ("wayland_pointer", written only when off) */
    /* ---- simulation effects (Wallpaper Engine's Fluid Simulation;
     * engine/fx_sim.h), at the end as well */
    gboolean fluid_sim;       /* run them (default on); off: such an effect is
                               * left out, its layer is shown without it */
    int     fluid_quality;    /* WalpeFluidQuality (common/fluidsim.h): full
                               * (default) | reduced | low; the presets set it */
    /* ---- at the end as well */
    GHashTable *monitors_extra; /* output name -> JsonObject (owned): the
                               * members of that output's "monitors" entry
                               * this build does not know (a newer version's);
                               * written back with the entry as long as it
                               * exists. `extra` holds only top-level members.
                               * NULL = none */
    char  **outputs;          /* walpe_config_set_outputs: the connected
                               * outputs' names (NULL-terminated; NULL = not
                               * told) */
    gboolean other_spellings; /* walpe_config_set_outputs */
} WalpeConfig;

/* config.json format: 1 = versions up to 0.11 (no "config_version" key),
 * 2 = version 0.12 (fps 30 can only be the user's choice),
 * 3 = the frame rate limit defaults to 60 (fps 200 can only be the user's
 *     choice),
 * 4 = per-output render resolution ("monitors"); "video_resolution"
 *     replaces the built-in 1920 x 1080 limit on imported and converted
 *     videos and is always written (a file that has it has been of this
 *     format, whatever version an older build stamped on it since: it is
 *     not migrated again). A file of version <= 3 is read as: every output connected at
 *     the daemon's next start has been asked and stays at native (see
 *     resolution_migrate), video_resolution "1080p" - the picture and the
 *     files written are what they were. */
#define WALPE_CONFIG_VERSION 4
/* Wallpaper sound of a new install: on, at half volume (the owner's decision
 * after 0.14.0, where it was off at volume 0 and could be switched on from
 * the command line only). Half: what the settings window and the tray set
 * when sound is switched on at volume 0. A wallpaper without sound layers
 * costs nothing for it (no player, no thread, no stream; libpulse-simple is
 * not even loaded). */
#define WALPE_VOLUME_DEFAULT 0.5
/* "sound_defaults" in config.json: the generation of these defaults the
 * file was written under. Absent = 1 (versions up to 0.14.0), 2 = on at
 * WALPE_VOLUME_DEFAULT; always written from now on. */
#define WALPE_SOUND_DEFAULTS 2
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
/* A file of sound_defaults < 2. Every config.json a daemon has saved holds
 * "volume" (since 0.12) and "scene_sound" (0.14.0) whether the user set them
 * or not, so a missing key cannot say "never chosen". What says it is the
 * old default itself: volume 0 without sound switched on - 0.14.0's untouched
 * pair (off, 0), and a file from before 0.14.0 that has no switch at all and
 * a volume of 0. TRUE: that is what the file holds, and the new defaults
 * replace it. Everything else was somebody's doing and is kept: sound on
 * with volume 0 (silent, and the settings window says so), sound off with a
 * volume above 0. has_switch: the file has "scene_sound". */
gboolean walpe_config_migrate_sound (int sound_defaults, gboolean has_switch, gboolean scene_sound, double volume);

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
/* "video_hwdec" by word: off | auto | <api>. on / true / yes switch it on
 * with the API that is stored (auto when there is none), so that the switch
 * in the settings does not forget a choice made by name; "auto" forgets it;
 * off keeps it for the next "on". FALSE = not such a word, nothing changed. */
gboolean     walpe_config_set_hwdec (WalpeConfig *c, const char *word);
/* the setting as one word: "off", "auto" or the API (not to be freed) */
const char  *walpe_config_hwdec_word (const WalpeConfig *c);
const char  *walpe_particle_lowres_name (int mode);

WalpeConfig *walpe_config_load  (void);
/* FALSE = config.json could not be written (a full disk, a directory that
 * cannot be written to): the settings in memory stay as they are and the
 * file as it was. walpe_config_save_error () then gives the reason, until
 * a save succeeds (NULL = the last save was written). */
gboolean     walpe_config_save  (WalpeConfig *cfg);
const char  *walpe_config_save_error (void);
void         walpe_config_free  (WalpeConfig *cfg);
/* renderer feature switches (engine/features.h): remember one for the
 * next save / what the file says: 1 on, 0 off, -1 not set. Names are lower
 * case letters, digits and '_'; anything else is not kept. */
void         walpe_config_set_feature (WalpeConfig *cfg, const char *name, gboolean on);
int          walpe_config_get_feature (const WalpeConfig *cfg, const char *name);
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
/* The connected outputs as the window system names them, and whether an
 * output without an entry of its own ("monitor_current", "monitors") may
 * use the one stored under another spelling of its connector
 * (common/backend.h walpe_output_name_same: "HDMI-1" on X11 is "HDMI-A-1"
 * on Wayland). The daemon says TRUE on Wayland, where every entry an
 * earlier version wrote carries an X11 name; on X11 the names are the ones
 * those versions used and only the exact name counts, as it always did.
 * Such an entry is used only when it is the only one of that connector and
 * is not the name of a connected output itself. What is stored from then
 * on is stored under the output's own name, which then holds. */
void         walpe_config_set_outputs (WalpeConfig *cfg, const char *const *names, guint n,
                                       gboolean other_spellings);
const char  *walpe_config_monitor_path (WalpeConfig *cfg, const char *name);
void         walpe_config_set_monitor_path (WalpeConfig *cfg, const char *name,
                                            const char *path);
/* per-output render resolution. _monitor: the stored entry, NULL = none
 * (native, auto, not asked yet); _monitor_get: the entry or those defaults;
 * _set_monitor: mc NULL removes the entry. Main thread of the process that
 * owns the config (the daemon); nothing here locks. */
const WalpeMonitorConf *walpe_config_monitor (WalpeConfig *cfg, const char *name);
void         walpe_config_monitor_get (WalpeConfig *cfg, const char *name, WalpeMonitorConf *out);
void         walpe_config_set_monitor (WalpeConfig *cfg, const char *name,
                                       const WalpeMonitorConf *mc);
/* The upgrade step of format 4, run by the daemon once it knows its
 * outputs: while resolution_migrate is set, every output in names[] that
 * has no entry gets "native, auto, asked", and the flag is cleared.
 * Returns TRUE when the config changed (the caller saves). Without the
 * flag nothing happens. */
gboolean     walpe_config_monitors_migrate (WalpeConfig *cfg, const char *const *names, guint n);
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
