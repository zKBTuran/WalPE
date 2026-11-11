/* quality.h - quality-for-load settings: the knobs that trade picture
 * quality for GPU work, their presets, how they are stored and sent, and
 * the frame rate chosen for the desktop's state.
 *
 * Everything here is plain data and pure functions (no GL, no X): the
 * daemon, walpe-ctl, the GUI and the tests share it. The default is the
 * preset "high": every knob of this file at the value that leaves the
 * picture as it is without the knobs. "max" is the same plus exact
 * texture sizes (texture_slack 0): byte for byte what was drawn before
 * any of this existed.
 *
 *   knob            config.json / IPC key   values
 *   effect rate     fx_rate                 1 (every frame) | 2 | 3
 *                   fx_rate_mode            "slow" (only effects classified
 *                                           slow) | "all" (every animated one)
 *   frame rate      fps_idle                0 = the fps cap, else 5..200: the
 *   while idle                              wallpaper's own limit, what it runs
 *                                           at while the desktop is idle
 *   effect          fx_res                  "full" (as before the knobs) |
 *   resolution                              "screen" | "reduced" | "half"
 *   post-processing post_processing         "full" | "reduced" | "off"
 *   render scale    render_scale            25..100 (percent of the monitor)
 *   texture         tex_res                 "full" | "half" | "quarter"
 *   resolution
 *   texture slack   texture_slack           0 (exact sizes) | 2 (default):
 *                                           percent a reduced texture / effect
 *                                           size may be magnified when drawn
 *   preset          quality_preset          "max" | "high" | "balanced" | "low"
 *                                           (sets all of the above; reads back
 *                                           "custom" when they match none)
 *
 * Carried here as key, preset value, IPC and GUI (the engine side is in
 * the scene loader): render_scale (WalpeLoadOpts.render_scale /
 * set_render_scale) and texture_slack (WalpeLoadOpts.no_tex_slack).
 * The frame rate while an application is focused / on battery are the
 * daemon's own settings (focus_action = limit + focus_fps, battery_fps):
 * walpe_quality_fps () takes them as inputs.
 *
 * Per wallpaper: config.json "wallpaper_quality": { "<path>": { only the
 * overridden keys } }; what a wallpaper runs with is the global setting
 * with its own keys on top. */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include "common/api.h"

G_BEGIN_DECLS

typedef enum {
    WALPE_PRESET_MAX = 0,    /* high + exact texture sizes */
    WALPE_PRESET_HIGH,       /* the default */
    WALPE_PRESET_BALANCED,
    WALPE_PRESET_LOW,
    WALPE_PRESET_CUSTOM,     /* not a preset: the knobs match none */
} WalpePreset;

enum { WALPE_POST_FULL = 0, WALPE_POST_REDUCED, WALPE_POST_OFF };
enum { WALPE_TEXRES_FULL = 0, WALPE_TEXRES_HALF, WALPE_TEXRES_QUARTER };
/* Resolution of a layer's effect passes. FULL: as before the knobs - the
 * layer's on-screen size rounded up to the next 1/8 of its texture, never
 * below half the texture. SCREEN: exactly the on-screen size (never below
 * half the texture). REDUCED / HALF: 3/4 / 1/2 of the on-screen size. */
enum { WALPE_FXRES_FULL = 0, WALPE_FXRES_SCREEN, WALPE_FXRES_REDUCED, WALPE_FXRES_HALF };

typedef struct {
    int fx_rate;       /* animated effect chains are redrawn every Nth frame: 1..3 */
    int fx_rate_all;   /* 0: only chains classified slow; 1: every animated chain */
    int fps_idle;      /* the wallpaper's frame rate limit: what it runs at
                        * while the desktop is idle (the daemon's focus and
                        * battery limits lower it further); 0 = the fps cap */
    int post;          /* WALPE_POST_* */
    int render_scale;  /* percent of the monitor's resolution, 25..100 */
    int tex_res;       /* WALPE_TEXRES_*: layer textures stored 1/1, 1/2, 1/4 */
    int fx_res;        /* WALPE_FXRES_*: resolution of layer effect passes */
    int tex_slack;     /* percent a reduced texture / effect size may be
                        * magnified when drawn: 0 = exact sizes, default 2 */
} WalpeQuality;

/* one bit per knob (override masks, "what changed") */
enum {
    WALPE_Q_FX_RATE      = 1 << 0,
    WALPE_Q_FX_RATE_ALL  = 1 << 1,
    WALPE_Q_FPS_IDLE     = 1 << 2,
    WALPE_Q_POST         = 1 << 3,
    WALPE_Q_RENDER_SCALE = 1 << 4,
    WALPE_Q_TEX_RES      = 1 << 5,
    WALPE_Q_FX_RES       = 1 << 6,
    WALPE_Q_TEX_SLACK    = 1 << 7,
    WALPE_Q_ALL          = 0xff,
};

/* a wallpaper's own settings: the knobs in `mask` replace the global ones */
typedef struct {
    guint mask;
    WalpeQuality q;
} WalpeQualityOv;

#define WALPE_QUALITY_FPS_MIN 5
#define WALPE_QUALITY_FPS_MAX 200
#define WALPE_RENDER_SCALE_MIN 25
#define WALPE_TEX_SLACK_DEFAULT 2

/* the values of a preset (WALPE_PRESET_CUSTOM gives High) */
void walpe_quality_preset (WalpePreset p, WalpeQuality *q);
/* the preset q equals, else WALPE_PRESET_CUSTOM */
WalpePreset walpe_quality_preset_of (const WalpeQuality *q);
const char *walpe_preset_name (WalpePreset p);        /* "max", "high", ... */
const char *walpe_preset_label (WalpePreset p);       /* "High (default)", ... */
/* Settings of other parts of the engine that a preset sets with it, as
 * data only: when a preset is set globally the daemon adds them to the
 * request, and a build without such a key ignores it. value[] by preset:
 * max, high, balanced, low. */
typedef struct {
    const char *key;
    char type;               /* 's' string, 'i' integer, 'b' boolean */
    const char *value[4];
    const char *cost[4];     /* what the viewer gives up with that value, a
                              * sentence in plain words; NULL = nothing */
} WalpePresetExtra;
const WalpePresetExtra *walpe_preset_extras (guint *n);
/* the extras of preset p as members of a request (members it has stay) */
void walpe_preset_extras_into (WalpePreset p, JsonObject *req);
/* in plain words: what the viewer gives up with the preset's own knobs
 * (this is all a wallpaper's own preset sets) */
const char *walpe_preset_cost (WalpePreset p);
/* the same for the global preset: with the sentences of the extras it
 * sets, those this build has (its config.json carries the key); cfg NULL =
 * every extra. Caller frees. (walpe-ctl quality, the GUI's tooltip) */
struct WalpeConfig_;
char *walpe_preset_cost_full (struct WalpeConfig_ *cfg, WalpePreset p);
/* which knobs can change the picture and which cannot (one line) */
const char *walpe_quality_picture_note (void);
gboolean walpe_preset_parse (const char *s, WalpePreset *p);
/* every knob into its valid range (a value outside becomes the nearest) */
void walpe_quality_sanitize (WalpeQuality *q);
gboolean walpe_quality_equal (const WalpeQuality *a, const WalpeQuality *b);

/* JSON, flat: the knobs of `mask` as members of the object being built
 * (keys as in the table above; with_preset adds "quality_preset") */
void walpe_quality_to_json (JsonBuilder *b, const WalpeQuality *q, guint mask,
                            gboolean with_preset);
/* Read knobs from an object's members into q. "quality_preset" / "preset"
 * is applied first (all knobs), then the single keys. Returns FALSE and an
 * error text (g_free) when a present key has a value that is not allowed;
 * q is then untouched. *set: the knobs that were given; *unset (may be
 * NULL): the knobs given as null or "default" (per-wallpaper: inherit
 * again); with unset == NULL those values are errors. Unknown keys are
 * not looked at. */
gboolean walpe_quality_from_json (JsonObject *o, WalpeQuality *q, guint *set, guint *unset,
                                  char **err);
/* the same from "key=value" words (walpe-ctl): builds the JSON members
 * into b, FALSE + error for an unknown key or a malformed value. Only
 * quality keys (and their short names: preset, post, scale, tex) are
 * taken; *taken says whether kv was one. */
gboolean walpe_quality_kv_to_json (JsonBuilder *b, const char *key, const char *value,
                                   gboolean *taken, char **err);
/* config.json: as walpe_quality_from_json, but a value that is not allowed
 * is skipped (the knob keeps what q had) instead of failing the load */
void walpe_quality_load_json (JsonObject *o, WalpeQuality *q, guint *set);
/* global + a wallpaper's own (ov may be NULL) */
void walpe_quality_effective (const WalpeQuality *global, const WalpeQualityOv *ov,
                              WalpeQuality *out);

/* ---- frame rate by state ------------------------------------------- */
/* What the frame rate rules look at besides the knobs: the daemon's own
 * signals and settings. Nothing here is measured by this file. */
typedef struct {
    gboolean focus_app;   /* an application on this monitor has the focus
                           * (FALSE: the desktop itself / nothing) */
    int focus_fps;        /* the limit while focus_app: focus_fps when
                           * focus_action is "limit", else 0 = none */
    gboolean on_battery;  /* the machine runs on battery */
    int battery_fps;      /* the limit while on_battery, 0 = none */
} WalpeFpsState;
/* The frame rate limit of a monitor: the fps cap (config fps) lowered by
 * every rule that holds - always: q->fps_idle (the idle desktop's rate);
 * an application focused: st->focus_fps as well; on battery:
 * st->battery_fps as well. 0 = that rule sets no limit. So no state runs
 * faster than the idle desktop. Never above cap, never below 1. st NULL =
 * nothing focused, on mains. Pure: same inputs, same answer. */
int walpe_quality_fps (const WalpeQuality *q, const WalpeFpsState *st, int cap);
const char *walpe_fps_state_name (const WalpeFpsState *st);   /* "idle", "focus", "battery", "focus+battery" */

/* ---- what a scene costs, and what a setting would save -------------- */
/* per frame, averaged by the renderer over its last frames; "at High" =
 * what the frame would cost with every knob at its default */
typedef struct {
    int w, h;             /* frame size in pixels */
    double fps;           /* frames per second the scene is paced at now */
    gboolean measured;    /* px_base from the GPU's own count (else estimated
                           * from the layers' on-screen sizes; particles are
                           * then not included) */
    double px_base;       /* pixels shaded outside effect passes: layers,
                           * particles, the final copy */
    double px_fx;         /* pixels shaded by effect passes at High */
    double passes;        /* effect passes drawn at High */
    /* of px_fx / passes: in effect sets an update divisor may hold:
     * [0] "slow" /2, [1] "slow" /3, [2] "all" /2, [3] "all" /3 */
    double px_hold[4], passes_hold[4];
    /* removed by effect resolution [0] "screen", [1] "reduced", [2] "half" */
    double px_fxres[3];
    double px_post_red;   /* removed by post-processing "reduced" */
    double px_post_off;   /* removed by "off" (includes the reduced part) */
    double passes_post_off;
    double px_now;        /* effect pass pixels actually drawn per frame now */
    double passes_now;    /* effect passes actually drawn per frame now */
    double target_bytes;  /* effect render targets alive (bytes) */
} WalpeQualityStats;

/* pixels shaded per frame with settings q, from the scene's numbers */
double walpe_quality_estimate_frame (const WalpeQualityStats *st, const WalpeQuality *q);
/* effect passes per frame with settings q */
double walpe_quality_estimate_passes (const WalpeQualityStats *st, const WalpeQuality *q);
/* pixels shaded per second: the frame estimate x the frame rate for the
 * state (cap = config fps, hz = monitor refresh, 0 = unknown) */
double walpe_quality_estimate (const WalpeQualityStats *st, const WalpeQuality *q,
                               const WalpeFpsState *fs, int cap, double hz);

/* ---- requests (daemon side; shared with the tests) ------------------ */
struct WalpeConfig_;
/* {"cmd":"set", ...}: the quality keys among the request's members go into
 * the global setting. FALSE + error text when one is not allowed (nothing
 * is changed then). *given: quality keys were present; *changed: the knobs
 * whose value is different now. */
gboolean walpe_quality_ipc_set (struct WalpeConfig_ *cfg, JsonObject *req, guint *given,
                                guint *changed, char **err);
/* one wallpaper's own settings: values = { key: value | null } (null or
 * "default" = follow the global setting again), reset = drop them all.
 * *changed: the knobs whose effective value for that wallpaper changed. */
gboolean walpe_quality_ipc_set_wallpaper (struct WalpeConfig_ *cfg, const char *path,
                                          JsonObject *values, gboolean reset, guint *changed,
                                          char **err);
/* members describing the settings: the global knobs + "quality_preset",
 * "presets": { name: knobs }, "wallpapers": { path: its own keys } */
void walpe_quality_ipc_describe (struct WalpeConfig_ *cfg, JsonBuilder *b);

G_END_DECLS
