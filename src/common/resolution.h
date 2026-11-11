/* resolution.h - the size a monitor's wallpaper is rendered at, the largest
 * texture kept for it, and the size videos are imported and converted to:
 * values, their spelling, and the rules that turn them into pixels.
 *
 * Nothing in WalPE is tied to one screen size. Every output has a choice
 * of its own (config.json "monitors": { "<output>": { ... } }):
 *
 *   resolution     "native" (default) | "720p" | "1080p" | "1440p" | "4k" |
 *                  "<W>x<H>"
 *                  The wallpaper is rendered at that size and scaled to the
 *                  output. native = the output's own mode: nothing is scaled.
 *                  A preset names the pixels on the output's SHORTER side
 *                  and keeps the output's aspect ratio: "1080p" is
 *                  1920 x 1080 on a 16:9 output, 2580 x 1080 on 3440 x 1440
 *                  and 1080 x 1920 on an output turned upright. A custom
 *                  size is taken as written (and stretched to the output
 *                  when its aspect ratio is another).
 *   texture_limit  "auto" (default) | "none" | "<pixels>"
 *                  auto: layer textures are kept at the size they are shown
 *                  at in the chosen resolution (what "tex_downscale" does
 *                  for the window's size). <pixels>: the same, and none is
 *                  kept longer than that on its longer side. none: file
 *                  size.
 *   asked          the user confirmed the choice. An output without an
 *                  entry runs at native / auto and counts as "not asked
 *                  yet": the GUI asks about it once.
 *
 * Global: "video_resolution" (auto | none | a resolution; see
 * walpe_video_box), "scale_filter" (bilinear | nearest) and
 * "resolution_prompt" (may the GUI ask at all).
 *
 * Everything here is plain data and pure functions (no X, no GL, no GTK):
 * the daemon, walpe-ctl, the GUI and the tests share it. */
#pragma once
#include <glib.h>

G_BEGIN_DECLS

/* a render size the user may ask for: not smaller than this, no side
 * longer than MAX_SIDE, not more pixels than the largest texture the
 * loaders accept (a render target is one) */
#define WALPE_RES_MIN_W 320
#define WALPE_RES_MIN_H 200
#define WALPE_RES_MAX_SIDE 16384
#define WALPE_RES_MAX_PIXELS ((gint64) 8192 * 8192)

typedef enum {
    WALPE_RES_NATIVE = 0,   /* the output's own mode */
    WALPE_RES_LINES,        /* a preset: `lines` pixels on the shorter side */
    WALPE_RES_CUSTOM,       /* exactly w x h */
} WalpeResKind;

typedef struct {
    int kind;       /* WalpeResKind */
    int lines;      /* WALPE_RES_LINES */
    int w, h;       /* WALPE_RES_CUSTOM */
} WalpeRes;

typedef struct {
    const char *name;   /* "720p", "1080p", "1440p", "4k" */
    int lines;
} WalpeResPreset;
/* the presets, smallest first */
const WalpeResPreset *walpe_res_presets (guint *n);

/* "native", a preset (also "2k" = 1440p, "2160p" / "uhd" = 4k, "qhd",
 * "fhd", "hd"; any case) or "<W>x<H>" within the limits above. FALSE: *out
 * is untouched and *err (may be NULL; g_free) is a sentence that names
 * the value. */
gboolean walpe_res_parse (const char *s, WalpeRes *out, char **err);
/* canonical text: "native", "1080p", "2560x1440" (g_free) */
char *walpe_res_format (const WalpeRes *r);
gboolean walpe_res_equal (const WalpeRes *a, const WalpeRes *b);
/* The size the wallpaper of an output in mode mode_w x mode_h is rendered
 * at. native: the mode itself. A preset: `lines` on the shorter side, the
 * longer side from the mode's aspect ratio, rounded to an even number
 * (16:9 when the mode is not known). Custom: as written. */
void walpe_res_size (const WalpeRes *r, int mode_w, int mode_h, int *w, int *h);
/* render pixels in per cent of the mode's (100 = native; at least 1) */
int walpe_res_percent (int w, int h, int mode_w, int mode_h);
/* "native (1920 x 1080)", "1080p (2580 x 1080)", "2560 x 1440" (g_free) */
char *walpe_res_label (const WalpeRes *r, int mode_w, int mode_h);
/* one sentence in plain words: what rendering at w x h costs or saves on
 * an output in mode mode_w x mode_h (g_free) */
char *walpe_res_cost_line (int w, int h, int mode_w, int mode_h);

/* ---- texture limit -------------------------------------------------- */
#define WALPE_TEXLIMIT_AUTO 0       /* follows the render size */
#define WALPE_TEXLIMIT_NONE (-1)    /* file size */
#define WALPE_TEXLIMIT_MIN 64       /* pixels; values in between are a cap */
#define WALPE_TEXLIMIT_MAX 16384

/* "auto" | "none" (also "off", "full") | pixels 64..16384 (also "1k",
 * "2k", "4k", "8k", "16k" = 1024 ..). FALSE + *err as walpe_res_parse. */
gboolean walpe_texlimit_parse (const char *s, int *out, char **err);
char *walpe_texlimit_format (int limit);   /* g_free */
/* One wallpaper shown on several monitors keeps one set of textures (the
 * monitors' contexts share them): the limit that holds for it is the
 * least strict of theirs - none before auto before the larger number
 * before the smaller. A stricter monitor would only add a second, smaller
 * copy next to the one its neighbour needs anyway. */
int walpe_texlimit_merge (int a, int b);
/* what a limit means for a load: *no_downscale = textures stay at file
 * size (limit none, or the global "tex_downscale" off), *cap_px = no
 * texture longer than this on its longer side (0 = no cap) */
void walpe_texlimit_effective (int limit, gboolean tex_downscale, gboolean *no_downscale,
                               int *cap_px);
/* halvings that bring a w x h texture within cap_px on its longer side
 * (0 when it fits or cap_px is 0), never below 64 pixels on that side */
int walpe_texlimit_level (int w, int h, int cap_px);

/* ---- one output's entry ---------------------------------------------- */
typedef struct {
    WalpeRes res;
    int tex_limit;      /* WALPE_TEXLIMIT_AUTO / _NONE / pixels */
    gboolean asked;
} WalpeMonitorConf;

/* an output name usable as a key: 1..64 bytes of printable UTF-8 */
gboolean walpe_monitor_name_ok (const char *name);
/* How many entries "monitors" is meant to hold. Entries stay when their
 * output is unplugged and can be stored ahead by name, so their number is
 * not the number of screens; every one of them is in each listing and each
 * save. The daemon refuses a request that would store a choice for an
 * output that is not connected beyond this many (a connected output always
 * gets its entry), and a config.json is read up to WALPE_MONITORS_READ_MAX
 * entries (a file with more was not written by the daemon). */
#define WALPE_MONITORS_MAX 256
#define WALPE_MONITORS_READ_MAX (4 * WALPE_MONITORS_MAX)

/* "VALUE" (every connected output: monitor NULL) or "NAME:VALUE,NAME:VALUE"
 * as `walpe-ctl set resolution=...` / `texture_limit=...` take it. The
 * values are not looked at here. NULL + *err when the text is not of that
 * form. Free with g_ptr_array_unref. */
typedef struct {
    char *monitor;      /* NULL = every connected output */
    char *value;
} WalpeResAssign;
GPtrArray *walpe_res_assign_parse (const char *spec, char **err);

/* ---- scaling filter (render size != output mode) ---------------------- */
enum { WALPE_SCALE_BILINEAR = 0, WALPE_SCALE_NEAREST };
const char *walpe_scale_filter_name (int f);
/* -1 = not such a name */
int walpe_scale_filter_parse (const char *s);

/* ---- videos: the size imports and converted copies are limited to ----- */
typedef struct { int w, h; } WalpeVideoBox;     /* 0 x 0 = no limit */

enum { WALPE_VIDRES_AUTO = 0, WALPE_VIDRES_NONE, WALPE_VIDRES_FIXED };
typedef struct {
    int kind;           /* WALPE_VIDRES_* */
    WalpeRes res;       /* WALPE_VIDRES_FIXED: a preset or a custom size */
} WalpeVideoRes;

/* "auto" | "none" (also "off", "source") | a preset | "<W>x<H>";
 * "native" is what auto means and is read as auto */
gboolean walpe_vidres_parse (const char *s, WalpeVideoRes *out, char **err);
char *walpe_vidres_format (const WalpeVideoRes *v);   /* g_free */
gboolean walpe_vidres_equal (const WalpeVideoRes *a, const WalpeVideoRes *b);
/* The box a video is fitted into when it is imported or converted.
 *   auto:  the smallest box that holds the render size of every monitor
 *          in sizes_wh (n pairs w, h: the monitors that show the video;
 *          for an import into the library, all of them). No monitor: no
 *          limit.
 *   none:  no limit (the source's size).
 *   fixed: a preset as a 16:9 box (1080p = 1920 x 1080, the limit that
 *          used to be built in), a custom size as written. */
void walpe_video_box (const WalpeVideoRes *v, const int *sizes_wh, guint n, WalpeVideoBox *box);
/* a w x h picture fitted inside the box: never enlarged, both sides even
 * and at least 2. box NULL or 0 x 0: only made even. */
void walpe_video_box_fit (const WalpeVideoBox *box, double w, double h, int *out_w, int *out_h);
/* does a w x h picture fit as it is */
gboolean walpe_video_box_holds (const WalpeVideoBox *box, int w, int h);

G_END_DECLS
