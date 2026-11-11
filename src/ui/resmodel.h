/* resmodel.h - the render resolution of each monitor as the GUI and the
 * tray see it: what the engine reports, what can be chosen for a monitor,
 * who still has to be asked, and the requests that change it.
 *
 * No GTK in here (GLib and json-glib only): the windows (resdialog.c), the
 * tray and tests/resmodel_test.c all use it, the test without a display.
 *
 * The engine owns the settings; the GUI never reads or writes config.json
 * for them. Two replies carry them, and one reader takes both:
 *   - the status reply's "state" object (also pushed to subscribers): per
 *     monitor the stored choice, the size rendered now and whether it was
 *     asked about. Enough to know that something changed and who is new.
 *   - the reply to {"cmd":"resolution"} (the listing; every request that
 *     changes something answers with it too): the same, plus the limits of
 *     a custom size, the choices the engine offers per monitor and the
 *     video settings. The windows are filled from it.
 * An engine that reports none of this (an older build still running) gives
 * a state with supported == FALSE: nothing is offered and nobody is asked.
 *
 * Names: "monres" throughout, so nothing here collides with the engine's
 * own value code (walpe_res_...). */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* The window's application action that opens the "Monitor resolution"
 * dialog (D-Bus: org.freedesktop.Application.ActivateAction on the GUI's
 * name). Parameter "as": the output names to show, an empty array = every
 * connected monitor. The tray calls it. */
#define WALPE_MONRES_ACTION "monitor-resolution"

/* ------------------------------------------------------------ the value */

typedef enum {
    WALPE_MONRES_NATIVE = 0,    /* the monitor's own mode */
    WALPE_MONRES_LINES,         /* a preset: `lines` pixels on the shorter side */
    WALPE_MONRES_CUSTOM,        /* exactly w x h */
} WalpeMonresKind;

typedef struct {
    WalpeMonresKind kind;
    int lines;                  /* WALPE_MONRES_LINES: 720, 1080, 1440 or 2160 */
    int w, h;                   /* WALPE_MONRES_CUSTOM */
} WalpeMonres;

#define WALPE_MONRES_N_PRESETS 4    /* 720p, 1080p, 1440p, 4k */

/* "native" | "720p" | "1080p" | "1440p" | "4k" | "<W>x<H>": the text of
 * walpe-ctl and of the requests. Case and blanks around it do not matter;
 * the other spellings the engine reads ("2160p", "uhd", "2k", "qhd",
 * "fhd", "hd") are read too, and "×" for "x". The size limits are not
 * checked here (walpe_monres_check_custom does that against what the
 * engine reports). FALSE, and *out = native, for anything else. */
gboolean walpe_monres_parse (const char *text, WalpeMonres *out);
/* the canonical text, for a request. g_free */
char    *walpe_monres_text (const WalpeMonres *v);
gboolean walpe_monres_equal (const WalpeMonres *a, const WalpeMonres *b);
/* preset i (0 .. WALPE_MONRES_N_PRESETS - 1): its lines; *label (may be
 * NULL) = "720p" .. "4K" as shown to people */
int      walpe_monres_preset (guint i, const char **label);
/* The pixels a value means on a monitor in mode mode_w x mode_h. A preset
 * keeps the monitor's shape: `lines` on the shorter side, the longer side
 * rounded to an even number; an unknown mode counts as 16:9. FALSE (and
 * 0 x 0) when it cannot be told: native on a monitor of unknown size. */
gboolean walpe_monres_size (const WalpeMonres *v, int mode_w, int mode_h, int *w, int *h);

/* ---------------------------------------------------- the texture limit */

typedef enum {
    WALPE_LIMIT_AUTO = 0,       /* follows the chosen resolutions */
    WALPE_LIMIT_NONE,           /* no limit */
    WALPE_LIMIT_VALUE,          /* texture: pixels; video: a size */
} WalpeLimitKind;

/* largest texture side: "auto" | "none" | <pixels> */
typedef struct {
    WalpeLimitKind kind;
    int px;
} WalpeTexLimit;

gboolean walpe_tex_limit_parse (const char *text, WalpeTexLimit *out);
char    *walpe_tex_limit_text (const WalpeTexLimit *t);         /* g_free */
gboolean walpe_tex_limit_equal (const WalpeTexLimit *a, const WalpeTexLimit *b);

/* largest size videos are imported and converted to: "auto" | "none" | a
 * resolution value ("native" is what auto means and is read as auto) */
typedef struct {
    WalpeLimitKind kind;
    WalpeMonres size;           /* WALPE_LIMIT_VALUE: a preset or a custom size */
} WalpeVideoLimit;

gboolean walpe_video_limit_parse (const char *text, WalpeVideoLimit *out);
char    *walpe_video_limit_text (const WalpeVideoLimit *v);     /* g_free */

/* ---------------------------------------------------------- one monitor */

/* one entry of what is offered for a monitor */
typedef struct {
    WalpeMonres value;
    int w, h;               /* its size on the monitor (custom: 0 until typed) */
    gboolean exceeds;       /* more lines than the monitor has */
    char *label;            /* "Native (2560 × 1440)", "1080p (1920 × 1080)", "Custom size" */
} WalpeMonresOption;

typedef struct {
    int index;              /* the engine's number (0-based): for labels only, it
                             * moves when the layout changes */
    char *name;             /* output name: what requests use to name the monitor */
    char *connector;        /* "" when the engine reports none besides the name */
    char *description;      /* make / model, "" when not reported */
    int mode_w, mode_h;     /* the monitor's pixels, 0 = unknown */
    double refresh;         /* Hz, 0 = unknown */
    gboolean knows;         /* the engine reported a resolution for it */
    WalpeMonres choice;     /* the stored choice (native when !knows) */
    char *choice_raw;       /* the engine's text when this build cannot read it, else NULL */
    int render_w, render_h; /* what is rendered now, 0 = not reported */
    gboolean asked;         /* FALSE only when the engine says so */
    gboolean tex_known;     /* the engine reported a texture limit for it */
    WalpeTexLimit tex;
    int tex_now;            /* the cap in force for its wallpaper, 0 = none / not reported */
    GPtrArray *offered;     /* WalpeMonresOption* (label NULL): what the engine offers
                             * for it, NULL = not reported */
} WalpeMonresMon;

/* An output name the engine takes in a request and can store a choice
 * under: 1 to 64 bytes of text without control characters (seam: the
 * engine's walpe_monitor_name_ok). Output names are whatever the X server
 * has - `xrandr --setmonitor` takes any text. A monitor with another name
 * cannot be given a choice: a request that names it is refused as a whole,
 * so it is not listed, never counted as "to ask about" and never
 * announced; it stays at its own resolution. */
gboolean walpe_monres_name_ok (const char *name);

/* the pixels a value means on this monitor: the size the engine reported
 * for it where it did, else walpe_monres_size */
gboolean walpe_monres_mon_size (const WalpeMonresMon *m, const WalpeMonres *v, int *w, int *h);
/* "2 · DELL U2720Q" or "2 · DP-2": one line, safe for a label. g_free */
char *walpe_monres_mon_title (const WalpeMonresMon *m);
/* "DP-2 · 3840 × 2160 · 60 Hz" (the name / connector only when the title
 * does not already show it, the rate only when known). g_free */
char *walpe_monres_mon_detail (const WalpeMonresMon *m);
/* a few words on what a size costs against the monitor's own, e.g.
 * "44 % of the monitor's pixels, scaled up." - "" when nothing can be
 * said. g_free */
char *walpe_monres_cost_note (const WalpeMonresMon *m, int w, int h);

/* What is offered for monitor m, in this order: Native, the presets that
 * are not larger than the monitor (the engine's list when it sent one),
 * with `exceed` also the larger ones, and Custom. The stored choice is
 * always in the list, also when it is a preset larger than the monitor
 * that `exceed` would hide. A monitor of unknown size gets every preset.
 * Array of WalpeMonresOption with a free function; g_ptr_array_unref. */
GPtrArray *walpe_monres_options (const WalpeMonresMon *m, gboolean exceed);
/* index of the option that is value v (any custom size matches the Custom
 * entry), or -1 */
int walpe_monres_options_find (GPtrArray *options, const WalpeMonres *v);

/* ------------------------------------------------------------ the state */

/* what a custom size is checked against when the engine reports no limits */
#define WALPE_MONRES_MIN_W 320
#define WALPE_MONRES_MIN_H 200
#define WALPE_MONRES_MAX_SIDE 16384
#define WALPE_MONRES_MAX_PIXELS ((gint64) 8192 * 8192)

typedef struct {
    gboolean supported;     /* the engine reports per-monitor resolutions */
    gboolean full;          /* read from the listing (limits, offers, video settings) */
    gboolean prompt;        /* monitors that were not asked about are asked about */
    gboolean fullscreen;    /* a fullscreen application has the focus */
    GPtrArray *mons;        /* WalpeMonresMon*: the connected monitors in the engine's
                             * order; never NULL */
    guint stored_only;      /* stored entries of outputs that are not connected */
    int min_w, min_h, max_side;         /* a custom size must lie within */
    gint64 max_pixels;
    gboolean video_known;   /* the engine reports the video size setting */
    WalpeVideoLimit video;
    gboolean video_now_known;
    int video_now_w, video_now_h;       /* the limit in force; 0 x 0 = none */
    char *scale_filter;     /* "bilinear" | "nearest"; NULL = not reported */
    char *note;             /* something the engine wants the user to read, or NULL */
} WalpeMonresState;

/* From the status reply's "state" object or from the listing reply. NULL
 * (no engine) gives an empty state with supported == FALSE. Never returns
 * NULL. */
WalpeMonresState *walpe_monres_state_new (JsonObject *o);
void              walpe_monres_state_free (WalpeMonresState *st);
const WalpeMonresMon *walpe_monres_state_find (const WalpeMonresState *st, const char *name);
/* everything a window shows of the state, as one text: two states with
 * the same signature need no redraw. g_free */
char *walpe_monres_state_signature (const WalpeMonresState *st);
/* The monitors to ask about: connected, reported as not asked, and asking
 * is on. With names != NULL their output names are appended (copies).
 * Returns their number; 0 for an engine without the feature. */
guint walpe_monres_state_unasked (const WalpeMonresState *st, GPtrArray *names);
/* The texture limit as one setting: TRUE and *common when every connected
 * monitor that reports one has the same; FALSE when they differ or none
 * reports one (*any, may be NULL, tells which). */
gboolean walpe_monres_state_texture (const WalpeMonresState *st, WalpeTexLimit *common,
                                     gboolean *any);

/* ----------------------------------------------------- a custom size */

typedef enum {
    WALPE_MONRES_CHECK_OK = 0,
    WALPE_MONRES_CHECK_NOTE,    /* can be applied; *msg says what to expect */
    WALPE_MONRES_CHECK_BAD,     /* cannot be applied; *msg says why */
} WalpeMonresCheck;

/* Width and height as typed. Digits only (blanks around them are fine).
 * BAD: empty, not a number, outside the engine's limits. NOTE: more pixels
 * than the monitor, or another shape than the monitor's. *w, *h get the
 * numbers when they were numbers (else 0); *msg (may be NULL) a sentence
 * or NULL, g_free. st and m may be NULL (built-in limits; limits only). */
WalpeMonresCheck walpe_monres_check_custom (const WalpeMonresState *st, const WalpeMonresMon *m,
                                            const char *wtext, const char *htext,
                                            int *w, int *h, char **msg);

/* ------------------------------------------------------------- requests
 * JSON text for the control socket; g_free. NULL when there is nothing to
 * send. Every request that changes something is answered with the listing
 * (or {"ok":false,"error":"..."} and nothing changed). */

/* the listing */
char *walpe_monres_request_list (void);

typedef struct {
    const char *name;           /* output name */
    gboolean set_value;
    WalpeMonres value;
    gboolean set_tex;
    WalpeTexLimit tex;
    /* -1: leave it to the engine (a monitor that gets a value counts as
     * asked); 0 / 1: say it */
    int asked;
} WalpeMonresAssign;

/* several monitors in one request: checked as a whole by the engine, one
 * save, one reload per scene */
char *walpe_monres_request_apply (const WalpeMonresAssign *a, guint n);
/* asking about new monitors on / off */
char *walpe_monres_request_prompt (gboolean on);
char *walpe_monres_request_video (const WalpeVideoLimit *v);
char *walpe_monres_request_filter (const char *name);

/* ------------------------------------------------- new monitors (hot-plug) */

/* Decides when a monitor is news. Fed with every state:
 *   - the monitors not asked about in the first state with the feature are
 *     the start-up ones: they are counted in *startup once (the window asks
 *     about them when it opens) and never announced;
 *   - a monitor that shows up later, not asked about, is announced once:
 *     its name is returned - but not while a fullscreen application has the
 *     focus; it waits until that is over;
 *   - a monitor that was announced and has been asked about meanwhile, or is
 *     gone, is returned in *withdraw (the announcement can be taken back);
 *   - unplugging and plugging the same monitor again does not announce it a
 *     second time in this run.
 * With asking turned off nothing is ever returned. A state without the
 * feature or without monitors (no engine, an engine restarting itself
 * after the layout changed) changes nothing. */
typedef struct WalpeMonresTracker WalpeMonresTracker;

WalpeMonresTracker *walpe_monres_tracker_new (void);
void                walpe_monres_tracker_free (WalpeMonresTracker *t);
/* Returns the names to announce now (array of char* with a free function,
 * g_ptr_array_unref) or NULL. startup, withdraw may be NULL; *withdraw is
 * an array like the result or NULL. *startup is set to the number of
 * start-up monitors in the call that sees them, else to 0. */
GPtrArray *walpe_monres_tracker_feed (WalpeMonresTracker *t, const WalpeMonresState *st,
                                      guint *startup, GPtrArray **withdraw);
/* announcements held back (fullscreen) */
guint walpe_monres_tracker_waiting (const WalpeMonresTracker *t);
/* Names _feed returned that were not shown after all (a dialog lists the
 * monitor already, another program announces it, there is nowhere to send
 * a notification): nothing is up for them, so _feed does not return them
 * in *withdraw later. They stay decided about: not returned a second time. */
void walpe_monres_tracker_unshown (WalpeMonresTracker *t, GPtrArray *names);

/* The words that announce new monitors (a notification): a title and one
 * or two sentences; g_free both. names = output names as the tracker
 * returned them; a name the state does not know is shown as it is. */
void walpe_monres_announce_text (const WalpeMonresState *st, GPtrArray *names, char **title,
                                 char **body);

G_END_DECLS
