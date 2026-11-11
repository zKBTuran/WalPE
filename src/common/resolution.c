/* resolution.c - see resolution.h */
#include "resolution.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const WalpeResPreset presets[] = {
    { "720p", 720 },
    { "1080p", 1080 },
    { "1440p", 1440 },
    { "4k", 2160 },
};

/* other spellings people use for the same sizes */
static const struct { const char *name; int lines; } preset_alias[] = {
    { "hd", 720 }, { "fhd", 1080 }, { "fullhd", 1080 }, { "qhd", 1440 }, { "wqhd", 1440 },
    { "2k", 1440 }, { "2160p", 2160 }, { "uhd", 2160 }, { "4k", 2160 },
};

const WalpeResPreset *walpe_res_presets (guint *n)
{
    if (n)
        *n = G_N_ELEMENTS (presets);
    return presets;
}

static void set_err (char **err, const char *fmt, ...) G_GNUC_PRINTF (2, 3);
static void set_err (char **err, const char *fmt, ...)
{
    if (!err)
        return;
    va_list ap;
    va_start (ap, fmt);
    *err = g_strdup_vprintf (fmt, ap);
    va_end (ap);
}

/* the value as it goes into a message: cut short, nothing unprintable */
static char *shown (const char *s)
{
    char *c = g_strndup (s ? s : "", 40);
    /* whole characters only: the cut may fall inside one, and the text need
     * not be UTF-8 at all (an environment variable). A sentence with half a
     * character in it makes the JSON reply that carries it unreadable. */
    const char *bad = NULL;
    if (!g_utf8_validate (c, -1, &bad))
        c[bad - c] = 0;
    for (char *p = c; *p; p++)
        if ((guchar) *p < 0x20 || *p == 0x7f)
            *p = '?';
    return c;
}

/* a run of digits as a positive int; FALSE when there is none, it does
 * not end at *end, or it is beyond anything a size can be */
static gboolean take_int (const char *s, const char **end, int *out)
{
    long v = 0;
    const char *p = s;
    if (!g_ascii_isdigit (*p))
        return FALSE;
    while (g_ascii_isdigit (*p)) {
        v = v * 10 + (*p - '0');
        if (v > 10000000)
            return FALSE;
        p++;
    }
    *end = p;
    *out = (int) v;
    return TRUE;
}

static gboolean lines_of (const char *name, int *lines)
{
    for (guint i = 0; i < G_N_ELEMENTS (presets); i++)
        if (!g_ascii_strcasecmp (name, presets[i].name)) {
            *lines = presets[i].lines;
            return TRUE;
        }
    for (guint i = 0; i < G_N_ELEMENTS (preset_alias); i++)
        if (!g_ascii_strcasecmp (name, preset_alias[i].name)) {
            *lines = preset_alias[i].lines;
            return TRUE;
        }
    return FALSE;
}

/* "<W>x<H>" (also 'X' or '*', blanks around it allowed) */
static gboolean parse_wxh (const char *s, int *w, int *h)
{
    const char *p = s, *e = NULL;
    if (!take_int (p, &e, w))
        return FALSE;
    p = e;
    while (*p == ' ')
        p++;
    if (*p != 'x' && *p != 'X' && *p != '*')
        return FALSE;
    p++;
    while (*p == ' ')
        p++;
    if (!take_int (p, &e, h))
        return FALSE;
    return *e == 0;
}

gboolean walpe_res_parse (const char *s, WalpeRes *out, char **err)
{
    WalpeRes r = { WALPE_RES_NATIVE, 0, 0, 0 };
    char *t = g_strstrip (g_strdup (s ? s : ""));
    gboolean ok = FALSE;
    int w = 0, h = 0, lines = 0;
    if (!g_ascii_strcasecmp (t, "native") || !g_ascii_strcasecmp (t, "monitor") ||
        !g_ascii_strcasecmp (t, "screen")) {
        ok = TRUE;
    } else if (lines_of (t, &lines)) {
        r.kind = WALPE_RES_LINES;
        r.lines = lines;
        ok = TRUE;
    } else if (parse_wxh (t, &w, &h)) {
        if (w < WALPE_RES_MIN_W || h < WALPE_RES_MIN_H) {
            set_err (err, "resolution %d x %d is too small: at least %d x %d", w, h,
                     WALPE_RES_MIN_W, WALPE_RES_MIN_H);
        } else if (w > WALPE_RES_MAX_SIDE || h > WALPE_RES_MAX_SIDE ||
                   (gint64) w * h > WALPE_RES_MAX_PIXELS) {
            set_err (err, "resolution %d x %d is too large: at most %d pixels a side and %d "
                     "megapixels in all", w, h, WALPE_RES_MAX_SIDE,
                     (int) (WALPE_RES_MAX_PIXELS / 1000000));
        } else {
            r.kind = WALPE_RES_CUSTOM;
            r.w = w;
            r.h = h;
            ok = TRUE;
        }
    } else {
        char *v = shown (t);
        set_err (err, "resolution '%s' is not one of native, 720p, 1080p, 1440p, 4k or "
                 "<width>x<height> (for example 2560x1440)", v);
        g_free (v);
    }
    g_free (t);
    if (ok)
        *out = r;
    return ok;
}

char *walpe_res_format (const WalpeRes *r)
{
    if (!r || r->kind == WALPE_RES_NATIVE)
        return g_strdup ("native");
    if (r->kind == WALPE_RES_LINES) {
        for (guint i = 0; i < G_N_ELEMENTS (presets); i++)
            if (presets[i].lines == r->lines)
                return g_strdup (presets[i].name);
        return g_strdup ("native");     /* (not a value walpe_res_parse gives) */
    }
    return g_strdup_printf ("%dx%d", r->w, r->h);
}

gboolean walpe_res_equal (const WalpeRes *a, const WalpeRes *b)
{
    if (a->kind != b->kind)
        return FALSE;
    if (a->kind == WALPE_RES_LINES)
        return a->lines == b->lines;
    if (a->kind == WALPE_RES_CUSTOM)
        return a->w == b->w && a->h == b->h;
    return TRUE;
}

static int even_near (double v)
{
    /* (bounded before the cast: the mode's sides are whatever the caller has) */
    if (!(v >= 2.0))
        return 2;
    if (v > WALPE_RES_MAX_SIDE)
        v = WALPE_RES_MAX_SIDE;
    int n = (int) (v / 2.0 + 0.5) * 2;
    return MAX (n, 2);
}

void walpe_res_size (const WalpeRes *r, int mode_w, int mode_h, int *w, int *h)
{
    int ow = MAX (mode_w, 0), oh = MAX (mode_h, 0);
    if (r && r->kind == WALPE_RES_CUSTOM && r->w > 0 && r->h > 0) {
        ow = r->w;
        oh = r->h;
    } else if (r && r->kind == WALPE_RES_LINES && r->lines > 0) {
        /* an unknown mode counts as 16:9, lying down */
        int mw = mode_w > 0 && mode_h > 0 ? mode_w : 16, mh = mode_w > 0 && mode_h > 0 ? mode_h : 9;
        int shorter = MIN (mw, mh), longer = MAX (mw, mh);
        int l = shorter == r->lines && mode_w > 0 ? longer
              : even_near ((double) r->lines * longer / shorter);
        l = MIN (l, WALPE_RES_MAX_SIDE);
        /* (the pixel limit: a preset on an extremely wide output) */
        while ((gint64) l * r->lines > WALPE_RES_MAX_PIXELS && l > 2)
            l -= 2;
        if (mw >= mh) {
            ow = l;
            oh = r->lines;
        } else {
            ow = r->lines;
            oh = l;
        }
    }
    if (w)
        *w = ow;
    if (h)
        *h = oh;
}

int walpe_res_percent (int w, int h, int mode_w, int mode_h)
{
    if (w < 1 || h < 1 || mode_w < 1 || mode_h < 1)
        return 100;
    double p = 100.0 * ((double) w * h) / ((double) mode_w * mode_h);
    return p < 1.0 ? 1 : p > 1000000.0 ? 1000000 : (int) (p + 0.5);
}

char *walpe_res_label (const WalpeRes *r, int mode_w, int mode_h)
{
    int w = 0, h = 0;
    walpe_res_size (r, mode_w, mode_h, &w, &h);
    if (r && r->kind == WALPE_RES_CUSTOM)
        return g_strdup_printf ("%d x %d", w, h);
    char *name = walpe_res_format (r);
    char *out = w > 0 && h > 0 ? g_strdup_printf ("%s (%d x %d)", name, w, h) : g_strdup (name);
    g_free (name);
    return out;
}

char *walpe_res_cost_line (int w, int h, int mode_w, int mode_h)
{
    if (w < 1 || h < 1 || mode_w < 1 || mode_h < 1 || (w == mode_w && h == mode_h))
        return g_strdup ("Rendered at the screen's own resolution: the sharpest picture, nothing "
                         "is scaled.");
    int pct = walpe_res_percent (w, h, mode_w, mode_h);
    if ((gint64) w * h < (gint64) mode_w * mode_h)
        return g_strdup_printf ("Rendered at %d x %d and scaled up to the screen: %d %% of its "
                                "pixels, so scenes need about that share of the GPU work and "
                                "video memory; the picture is softer.", w, h, pct);
    if ((gint64) w * h == (gint64) mode_w * mode_h)
        return g_strdup_printf ("Rendered at %d x %d and stretched to the screen's shape: the "
                                "same GPU work as native, a distorted picture.", w, h);
    return g_strdup_printf ("Rendered at %d x %d and scaled down to the screen: %.1f times its "
                            "pixels, so scenes need about that much more GPU work and video "
                            "memory; edges are smoother.", w, h, pct / 100.0);
}

/* ---------------------------------------------------- texture limit */

gboolean walpe_texlimit_parse (const char *s, int *out, char **err)
{
    char *t = g_strstrip (g_strdup (s ? s : ""));
    gboolean ok = FALSE;
    int v = 0;
    const char *e = NULL;
    if (!g_ascii_strcasecmp (t, "auto") || !g_ascii_strcasecmp (t, "default")) {
        v = WALPE_TEXLIMIT_AUTO;
        ok = TRUE;
    } else if (!g_ascii_strcasecmp (t, "none") || !g_ascii_strcasecmp (t, "off") ||
               !g_ascii_strcasecmp (t, "full") || !g_ascii_strcasecmp (t, "unlimited")) {
        v = WALPE_TEXLIMIT_NONE;
        ok = TRUE;
    } else if (take_int (t, &e, &v)) {
        /* "4k" as texture sizes are spoken of: 4096 */
        if ((*e == 'k' || *e == 'K') && e[1] == 0 && v >= 1 && v <= 16)
            v *= 1024;
        else if (*e)
            v = -2;
        if (v >= WALPE_TEXLIMIT_MIN && v <= WALPE_TEXLIMIT_MAX)
            ok = TRUE;
        else if (v != -2)
            set_err (err, "texture_limit %d is out of range: %d to %d pixels, auto or none", v,
                     WALPE_TEXLIMIT_MIN, WALPE_TEXLIMIT_MAX);
    }
    if (!ok && err && !*err) {
        char *sv = shown (t);
        set_err (err, "texture_limit '%s' is not auto, none or a number of pixels (%d to %d, "
                 "for example 4096)", sv, WALPE_TEXLIMIT_MIN, WALPE_TEXLIMIT_MAX);
        g_free (sv);
    }
    g_free (t);
    if (ok)
        *out = v;
    return ok;
}

char *walpe_texlimit_format (int limit)
{
    if (limit == WALPE_TEXLIMIT_NONE)
        return g_strdup ("none");
    if (limit < WALPE_TEXLIMIT_MIN)
        return g_strdup ("auto");
    return g_strdup_printf ("%d", MIN (limit, WALPE_TEXLIMIT_MAX));
}

/* how much a limit lets through: none 3, auto 2, a number 1 */
static int texlimit_rank (int v)
{
    return v == WALPE_TEXLIMIT_NONE ? 3 : v < WALPE_TEXLIMIT_MIN ? 2 : 1;
}

int walpe_texlimit_merge (int a, int b)
{
    int ra = texlimit_rank (a), rb = texlimit_rank (b);
    if (ra != rb)
        return ra > rb ? a : b;
    if (ra == 1)
        return MAX (a, b);
    return ra == 3 ? WALPE_TEXLIMIT_NONE : WALPE_TEXLIMIT_AUTO;
}

void walpe_texlimit_effective (int limit, gboolean tex_downscale, gboolean *no_downscale,
                               int *cap_px)
{
    gboolean none = limit == WALPE_TEXLIMIT_NONE;
    if (no_downscale)
        *no_downscale = none || !tex_downscale;
    if (cap_px)
        *cap_px = !none && limit >= WALPE_TEXLIMIT_MIN ? MIN (limit, WALPE_TEXLIMIT_MAX) : 0;
}

int walpe_texlimit_level (int w, int h, int cap_px)
{
    int side = MAX (w, h), L = 0;
    if (cap_px <= 0 || side <= 0)
        return 0;
    while (L < 15 && (side >> L) > cap_px && (side >> (L + 1)) >= WALPE_TEXLIMIT_MIN)
        L++;
    return L;
}

/* ------------------------------------------------- output names, lists */

gboolean walpe_monitor_name_ok (const char *name)
{
    if (!name || !*name || strlen (name) > 64 || !g_utf8_validate (name, -1, NULL))
        return FALSE;
    for (const guchar *p = (const guchar *) name; *p; p++)
        if (*p < 0x20 || *p == 0x7f)
            return FALSE;
    return TRUE;
}

static void assign_free (gpointer p)
{
    WalpeResAssign *a = p;
    g_free (a->monitor);
    g_free (a->value);
    g_free (a);
}

GPtrArray *walpe_res_assign_parse (const char *spec, char **err)
{
    GPtrArray *out = g_ptr_array_new_with_free_func (assign_free);
    char **parts = g_strsplit (spec ? spec : "", ",", -1);
    gboolean ok = parts[0] != NULL;
    for (int i = 0; ok && parts[i]; i++) {
        char *p = g_strstrip (parts[i]);
        /* the value never holds a ':'; an output name may */
        char *colon = strrchr (p, ':');
        WalpeResAssign *a = g_new0 (WalpeResAssign, 1);
        if (colon) {
            a->monitor = g_strstrip (g_strndup (p, (gsize) (colon - p)));
            a->value = g_strstrip (g_strdup (colon + 1));
        } else {
            a->value = g_strdup (p);
        }
        g_ptr_array_add (out, a);
        if (!*a->value || (a->monitor && !walpe_monitor_name_ok (a->monitor)))
            ok = FALSE;
        /* "all" / "*" in front of a value is the same as no name */
        if (a->monitor && (!strcmp (a->monitor, "*") || !g_ascii_strcasecmp (a->monitor, "all")))
            g_clear_pointer (&a->monitor, g_free);
    }
    /* a value for every output next to others: which one wins would depend
     * on the order */
    for (guint i = 0; ok && out->len > 1 && i < out->len; i++)
        if (!((WalpeResAssign *) g_ptr_array_index (out, i))->monitor)
            ok = FALSE;
    g_strfreev (parts);
    if (!ok) {
        char *v = shown (spec);
        set_err (err, "'%s': a value for every monitor (1080p) or <monitor>:<value> pairs "
                 "separated by commas (HDMI-0:1080p,DP-2:native)", v);
        g_free (v);
        g_ptr_array_unref (out);
        return NULL;
    }
    return out;
}

/* -------------------------------------------------------- scale filter */

const char *walpe_scale_filter_name (int f)
{
    return f == WALPE_SCALE_NEAREST ? "nearest" : "bilinear";
}

int walpe_scale_filter_parse (const char *s)
{
    if (!s)
        return -1;
    if (!g_ascii_strcasecmp (s, "bilinear") || !g_ascii_strcasecmp (s, "linear") ||
        !g_ascii_strcasecmp (s, "smooth"))
        return WALPE_SCALE_BILINEAR;
    if (!g_ascii_strcasecmp (s, "nearest") || !g_ascii_strcasecmp (s, "pixel") ||
        !g_ascii_strcasecmp (s, "sharp"))
        return WALPE_SCALE_NEAREST;
    return -1;
}

/* -------------------------------------------------------------- videos */

gboolean walpe_vidres_parse (const char *s, WalpeVideoRes *out, char **err)
{
    WalpeVideoRes v = { WALPE_VIDRES_AUTO, { WALPE_RES_NATIVE, 0, 0, 0 } };
    char *t = g_strstrip (g_strdup (s ? s : ""));
    gboolean ok = TRUE;
    if (!g_ascii_strcasecmp (t, "auto") || !g_ascii_strcasecmp (t, "native")) {
        /* as large as the monitors show it */
    } else if (!g_ascii_strcasecmp (t, "none") || !g_ascii_strcasecmp (t, "off") ||
               !g_ascii_strcasecmp (t, "source") || !g_ascii_strcasecmp (t, "unlimited")) {
        v.kind = WALPE_VIDRES_NONE;
    } else {
        char *e = NULL;
        if (walpe_res_parse (t, &v.res, &e) && v.res.kind != WALPE_RES_NATIVE) {
            v.kind = WALPE_VIDRES_FIXED;
        } else {
            ok = FALSE;
            char *sv = shown (t);
            set_err (err, "video_resolution '%s' is not auto, none, 720p, 1080p, 1440p, 4k or "
                     "<width>x<height>%s%s", sv, e ? ": " : "", e ? e : "");
            g_free (sv);
        }
        g_free (e);
    }
    g_free (t);
    if (ok)
        *out = v;
    return ok;
}

char *walpe_vidres_format (const WalpeVideoRes *v)
{
    if (!v || v->kind == WALPE_VIDRES_AUTO)
        return g_strdup ("auto");
    if (v->kind == WALPE_VIDRES_NONE)
        return g_strdup ("none");
    return walpe_res_format (&v->res);
}

gboolean walpe_vidres_equal (const WalpeVideoRes *a, const WalpeVideoRes *b)
{
    return a->kind == b->kind &&
           (a->kind != WALPE_VIDRES_FIXED || walpe_res_equal (&a->res, &b->res));
}

void walpe_video_box (const WalpeVideoRes *v, const int *sizes_wh, guint n, WalpeVideoBox *box)
{
    box->w = box->h = 0;
    if (!v || v->kind == WALPE_VIDRES_NONE)
        return;
    if (v->kind == WALPE_VIDRES_FIXED) {
        /* a preset here is a file size, not tied to an output: 16:9 */
        walpe_res_size (&v->res, 0, 0, &box->w, &box->h);
        return;
    }
    for (guint i = 0; sizes_wh && i < n; i++) {
        int w = sizes_wh[2 * i], h = sizes_wh[2 * i + 1];
        if (w < 1 || h < 1)
            continue;
        box->w = MAX (box->w, w);
        box->h = MAX (box->h, h);
    }
}

void walpe_video_box_fit (const WalpeVideoBox *box, double w, double h, int *out_w, int *out_h)
{
    if (!(w >= 1.0))    /* (also NaN) */
        w = 2.0;
    if (!(h >= 1.0))
        h = 2.0;
    /* Box or none: never enlarged, and never beyond what a picture may be
     * at all. w and h are a file's header times its pixel aspect ratio,
     * neither of which is trusted: without a box (video_resolution none)
     * they would go to the encoder as they are, and a product beyond
     * INT_MAX to the casts below. No real video comes near these limits,
     * and no box is larger than them, so with a box the result is the same
     * as before (1920 x 1080: the sizes of the limit that was built in). */
    double k = MIN (1.0, MIN (WALPE_RES_MAX_SIDE / w, WALPE_RES_MAX_SIDE / h));
    if (box && box->w > 0 && box->h > 0)
        k = MIN (k, MIN (box->w / w, box->h / h));
    if ((w * k) * (h * k) > (double) WALPE_RES_MAX_PIXELS)
        k = sqrt ((double) WALPE_RES_MAX_PIXELS / w / h);
    int ow = (int) floor (w * k + 1e-6);
    int oh = (int) floor (h * k + 1e-6);
    ow &= ~1;                                           /* 4:2:0 needs even sides */
    oh &= ~1;
    *out_w = MAX (ow, 2);
    *out_h = MAX (oh, 2);
}

gboolean walpe_video_box_holds (const WalpeVideoBox *box, int w, int h)
{
    return !box || box->w <= 0 || box->h <= 0 || (w <= box->w && h <= box->h);
}
