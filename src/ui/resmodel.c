/* resmodel.c - see resmodel.h */
#include "resmodel.h"
#include <math.h>
#include <string.h>
#include "common/uitext.h"
#include "common/util.h"

/* ---------------------------------------------------------------- the wire
 * Every name the engine and this model share, in one place. Where the
 * status reply and the listing use different names for the same thing,
 * both are read. */

/* one entry of "monitors" */
#define K_MONITORS       "monitors"
#define K_MON_INDEX      "index"
#define K_MON_NAME       "name"
#define K_MON_CONNECTED  "connected"       /* listing: false = a stored entry only */
#define K_MON_CONNECTOR  "connector"
#define K_MON_DESC       "description"
#define K_MON_MAKE       "make"            /* shown as "make model" when there is no description */
#define K_MON_MODEL      "model"
#define K_MON_MODE       "mode"            /* [w, h] */
#define K_MON_GEOMETRY   "geometry"        /* [x, y, w, h]: engines without "mode" */
#define K_MON_REFRESH    "refresh"
#define K_MON_RES        "resolution"      /* the stored choice, as text */
#define K_MON_RENDER     "render_size"     /* [w, h] in force */
#define K_MON_ASKED      "asked"           /* listing */
#define K_MON_ASKED_ST   "resolution_asked"    /* status */
#define K_MON_TEX        "texture_limit"
#define K_MON_TEX_NOW    "texture_limit_px"
#define K_MON_CHOICES    "choices"         /* [{"value": text, "size": [w, h]}, ...] */
/* top level */
#define K_PROMPT         "prompt"              /* listing */
#define K_PROMPT_ST      "resolution_prompt"   /* status */
#define K_UNASKED        "unasked"             /* listing */
#define K_UNASKED_ST     "resolution_unasked"  /* status */
#define K_LIMITS         "limits"          /* {min_width, min_height, max_side, max_pixels} */
#define K_VIDEO          "video_resolution"
#define K_VIDEO_NOW      "video_box"       /* [w, h], [0, 0] = no limit */
#define K_FILTER         "scale_filter"
#define K_NOTE           "note"
#define K_FULLSCREEN     "fullscreen"
#define K_FULLSCREEN_OLD "fullscreen_pause"    /* engines that do not report the above */
/* requests */
#define CMD_RESOLUTION   "resolution"
#define REQ_SET          "set"
#define REQ_MONITOR      "monitor"
#define REQ_VALUE        "value"
#define REQ_TEX          "texture_limit"
#define REQ_ASKED        "asked"
#define REQ_PROMPT       "prompt"

/* more monitors than this in one reply is not a monitor list */
#define MAX_MONITORS 64
#define MAX_OFFERED  16

static const struct {
    const char *name;       /* on the wire */
    const char *label;      /* for people */
    int lines;
} presets[WALPE_MONRES_N_PRESETS] = {
    { "720p", "720p", 720 },
    { "1080p", "1080p", 1080 },
    { "1440p", "1440p", 1440 },
    { "4k", "4K", 2160 },
};

/* other spellings the engine reads */
static const struct {
    const char *name;
    int lines;
} aliases[] = {
    { "hd", 720 }, { "fhd", 1080 }, { "2k", 1440 }, { "qhd", 1440 }, { "2160p", 2160 }, { "uhd", 2160 },
};

/* ------------------------------------------------------------ the value
 * (seam: the engine's value code - common/resolution.h once both are in
 * one tree - holds the same rules; these functions are the only place the
 * GUI spells or sizes a value, so they can forward to it) */

/* digits only, 1 .. 99999; *end = first character after them */
static gboolean read_side (const char *s, int *v, const char **end)
{
    int n = 0, digits = 0;
    while (g_ascii_isdigit (*s) && digits < 6) {
        n = n * 10 + (*s - '0');
        s++;
        digits++;
    }
    *end = s;
    if (digits < 1 || digits > 5 || n < 1)
        return FALSE;
    *v = n;
    return TRUE;
}

gboolean walpe_monres_parse (const char *text, WalpeMonres *out)
{
    WalpeMonres v = { WALPE_MONRES_NATIVE, 0, 0, 0 };
    gboolean ok = FALSE;
    char *t = text ? g_ascii_strdown (text, -1) : NULL;
    if (t) {
        g_strstrip (t);
        if (!strcmp (t, "native"))
            ok = TRUE;
        for (guint i = 0; i < G_N_ELEMENTS (presets) && !ok; i++) {
            if (!strcmp (t, presets[i].name)) {
                v.kind = WALPE_MONRES_LINES;
                v.lines = presets[i].lines;
                ok = TRUE;
            }
        }
        for (guint i = 0; i < G_N_ELEMENTS (aliases) && !ok; i++) {
            if (!strcmp (t, aliases[i].name)) {
                v.kind = WALPE_MONRES_LINES;
                v.lines = aliases[i].lines;
                ok = TRUE;
            }
        }
        if (!ok) {
            const char *p = t, *e;
            int w = 0, h = 0;
            if (read_side (p, &w, &e)) {
                p = e;
                while (*p == ' ')
                    p++;
                if (*p == 'x')
                    p++;
                else if (g_str_has_prefix (p, "\xc3\x97"))   /* × */
                    p += 2;
                else
                    p = NULL;
                while (p && *p == ' ')
                    p++;
                if (p && read_side (p, &h, &e) && *e == 0) {
                    v.kind = WALPE_MONRES_CUSTOM;
                    v.w = w;
                    v.h = h;
                    ok = TRUE;
                }
            }
        }
    }
    g_free (t);
    if (out)
        *out = v;
    return ok;
}

static const char *preset_label (int lines)
{
    for (guint i = 0; i < G_N_ELEMENTS (presets); i++)
        if (presets[i].lines == lines)
            return presets[i].label;
    return NULL;
}

char *walpe_monres_text (const WalpeMonres *v)
{
    if (!v || v->kind == WALPE_MONRES_NATIVE)
        return g_strdup ("native");
    if (v->kind == WALPE_MONRES_LINES) {
        for (guint i = 0; i < G_N_ELEMENTS (presets); i++)
            if (presets[i].lines == v->lines)
                return g_strdup (presets[i].name);
        /* not a value walpe_monres_parse gives; the engine refuses it by name */
        return g_strdup_printf ("%dp", v->lines);
    }
    return g_strdup_printf ("%dx%d", v->w, v->h);
}

gboolean walpe_monres_equal (const WalpeMonres *a, const WalpeMonres *b)
{
    if (!a || !b || a->kind != b->kind)
        return FALSE;
    if (a->kind == WALPE_MONRES_LINES)
        return a->lines == b->lines;
    if (a->kind == WALPE_MONRES_CUSTOM)
        return a->w == b->w && a->h == b->h;
    return TRUE;
}

int walpe_monres_preset (guint i, const char **label)
{
    if (i >= G_N_ELEMENTS (presets)) {
        if (label)
            *label = NULL;
        return 0;
    }
    if (label)
        *label = presets[i].label;
    return presets[i].lines;
}

gboolean walpe_monres_size (const WalpeMonres *v, int mode_w, int mode_h, int *w, int *h)
{
    gboolean known = mode_w > 0 && mode_h > 0;
    int ow = 0, oh = 0;
    if (v && v->kind == WALPE_MONRES_CUSTOM) {
        ow = v->w;
        oh = v->h;
    } else if (v && v->kind == WALPE_MONRES_LINES && v->lines > 0) {
        int mw = known ? mode_w : 16, mh = known ? mode_h : 9;
        int sht = MIN (mw, mh), lng = MAX (mw, mh);
        int l = known && sht == v->lines
                    ? lng : 2 * (int) floor ((double) v->lines * lng / sht / 2.0 + 0.5);
        l = MIN (l, WALPE_MONRES_MAX_SIDE);
        while ((gint64) l * v->lines > WALPE_MONRES_MAX_PIXELS && l > 2)
            l -= 2;
        ow = mw >= mh ? l : v->lines;
        oh = mw >= mh ? v->lines : l;
    } else if (known) {
        ow = mode_w;
        oh = mode_h;
    }
    if (ow <= 0 || oh <= 0)
        ow = oh = 0;
    if (w)
        *w = ow;
    if (h)
        *h = oh;
    return ow > 0;
}

/* ------------------------------------------------------------ the limits */

/* "auto", "none" (and the other words the engine reads for them) or neither */
static int limit_word (const char *t)
{
    if (!strcmp (t, "auto") || !strcmp (t, "default"))
        return WALPE_LIMIT_AUTO;
    if (!strcmp (t, "none") || !strcmp (t, "off") || !strcmp (t, "full") || !strcmp (t, "unlimited") ||
        !strcmp (t, "source"))
        return WALPE_LIMIT_NONE;
    return -1;
}

gboolean walpe_tex_limit_parse (const char *text, WalpeTexLimit *out)
{
    static const struct { const char *name; int px; } short_names[] = {
        { "1k", 1024 }, { "2k", 2048 }, { "4k", 4096 }, { "8k", 8192 }, { "16k", 16384 },
    };
    WalpeTexLimit l = { WALPE_LIMIT_AUTO, 0 };
    gboolean ok = FALSE;
    char *t = text ? g_ascii_strdown (text, -1) : NULL;
    if (t) {
        g_strstrip (t);
        int word = limit_word (t);
        const char *e;
        int px = 0;
        if (word >= 0) {
            l.kind = (WalpeLimitKind) word;
            ok = TRUE;
        } else if (read_side (t, &px, &e) && *e == 0) {
            l.kind = WALPE_LIMIT_VALUE;
            l.px = px;
            ok = TRUE;
        }
        for (guint i = 0; i < G_N_ELEMENTS (short_names) && !ok; i++) {
            if (!strcmp (t, short_names[i].name)) {
                l.kind = WALPE_LIMIT_VALUE;
                l.px = short_names[i].px;
                ok = TRUE;
            }
        }
    }
    g_free (t);
    if (out)
        *out = l;
    return ok;
}

char *walpe_tex_limit_text (const WalpeTexLimit *t)
{
    if (!t || t->kind == WALPE_LIMIT_AUTO)
        return g_strdup ("auto");
    if (t->kind == WALPE_LIMIT_NONE)
        return g_strdup ("none");
    return g_strdup_printf ("%d", t->px);
}

gboolean walpe_tex_limit_equal (const WalpeTexLimit *a, const WalpeTexLimit *b)
{
    if (!a || !b || a->kind != b->kind)
        return FALSE;
    return a->kind != WALPE_LIMIT_VALUE || a->px == b->px;
}

gboolean walpe_video_limit_parse (const char *text, WalpeVideoLimit *out)
{
    WalpeVideoLimit l = { WALPE_LIMIT_AUTO, { WALPE_MONRES_NATIVE, 0, 0, 0 } };
    gboolean ok = FALSE;
    char *t = text ? g_ascii_strdown (text, -1) : NULL;
    if (t) {
        g_strstrip (t);
        int word = limit_word (t);
        WalpeMonres v;
        if (word >= 0) {
            l.kind = (WalpeLimitKind) word;
            ok = TRUE;
        } else if (walpe_monres_parse (t, &v)) {
            ok = TRUE;
            if (v.kind != WALPE_MONRES_NATIVE) {    /* native is what auto means */
                l.kind = WALPE_LIMIT_VALUE;
                l.size = v;
            }
        }
    }
    g_free (t);
    if (out)
        *out = l;
    return ok;
}

char *walpe_video_limit_text (const WalpeVideoLimit *v)
{
    if (!v || v->kind == WALPE_LIMIT_AUTO)
        return g_strdup ("auto");
    if (v->kind == WALPE_LIMIT_NONE)
        return g_strdup ("none");
    return walpe_monres_text (&v->size);
}

/* ---------------------------------------------------------- one monitor */

static void option_free (gpointer p)
{
    WalpeMonresOption *o = p;
    if (!o)
        return;
    g_free (o->label);
    g_free (o);
}

static void mon_free (gpointer p)
{
    WalpeMonresMon *m = p;
    if (!m)
        return;
    g_free (m->name);
    g_free (m->connector);
    g_free (m->description);
    g_free (m->choice_raw);
    if (m->offered)
        g_ptr_array_unref (m->offered);
    g_free (m);
}

gboolean walpe_monres_name_ok (const char *name)
{
    if (!name || !*name || strlen (name) > 64 || !g_utf8_validate (name, -1, NULL))
        return FALSE;
    for (const guchar *p = (const guchar *) name; *p; p++)
        if (*p < 0x20 || *p == 0x7f)
            return FALSE;
    return TRUE;
}

gboolean walpe_monres_mon_size (const WalpeMonresMon *m, const WalpeMonres *v, int *w, int *h)
{
    for (guint i = 0; m && v && m->offered && i < m->offered->len; i++) {
        const WalpeMonresOption *o = m->offered->pdata[i];
        if (o->w > 0 && o->h > 0 && walpe_monres_equal (&o->value, v)) {
            if (w)
                *w = o->w;
            if (h)
                *h = o->h;
            return TRUE;
        }
    }
    return walpe_monres_size (v, m ? m->mode_w : 0, m ? m->mode_h : 0, w, h);
}

/* a name from the X server, the compositor or a monitor's EDID is text
 * from outside: one line, no control characters, a bounded length */
static char *shown (const char *s)
{
    return walpe_ui_text (s, 60, WALPE_UITEXT_LINE);
}

char *walpe_monres_mon_title (const WalpeMonresMon *m)
{
    if (!m)
        return g_strdup ("");
    char *n = shown (*m->description ? m->description : m->name);
    char *t = g_strdup_printf ("%d · %s", m->index + 1, *n ? n : "Monitor");
    g_free (n);
    return t;
}

char *walpe_monres_mon_detail (const WalpeMonresMon *m)
{
    if (!m)
        return g_strdup ("");
    GString *s = g_string_new (NULL);
    /* the title shows the description when there is one, else the name:
     * whatever of name and connector it does not show comes first here */
    if (*m->description && *m->name) {
        char *n = shown (m->name);
        g_string_append (s, n);
        g_free (n);
    }
    if (*m->connector && strcmp (m->connector, m->name)) {
        char *c = shown (m->connector);
        g_string_append_printf (s, "%s%s", s->len ? " · " : "", c);
        g_free (c);
    }
    if (m->mode_w > 0 && m->mode_h > 0)
        g_string_append_printf (s, "%s%d × %d", s->len ? " · " : "", m->mode_w, m->mode_h);
    if (m->refresh > 0) {
        /* 60, 144; 59.94 and 143.9 keep their fraction (two places, no
         * zeros at the end) */
        double r = m->refresh;
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        if (fabs (r - floor (r + 0.5)) < 0.05) {
            g_ascii_formatd (buf, sizeof (buf), "%.0f", r);
        } else {
            g_ascii_formatd (buf, sizeof (buf), "%.2f", r);
            size_t n = strlen (buf);
            while (n > 1 && buf[n - 1] == '0')
                buf[--n] = 0;
        }
        g_string_append_printf (s, "%s%s Hz", s->len ? " · " : "", buf);
    }
    return g_string_free (s, FALSE);
}

char *walpe_monres_cost_note (const WalpeMonresMon *m, int w, int h)
{
    if (!m || m->mode_w <= 0 || m->mode_h <= 0 || w <= 0 || h <= 0)
        return g_strdup ("");
    if (w == m->mode_w && h == m->mode_h)
        return g_strdup ("The monitor's own size.");
    double pct = 100.0 * ((double) w * h) / ((double) m->mode_w * m->mode_h);
    /* (seam: the engine's walpe_res_percent - same rounding, same bounds) */
    int p = pct < 1.0 ? 1 : pct > 1000000.0 ? 1000000 : (int) (pct + 0.5);
    if (p == 100)
        return g_strdup ("As many pixels as the monitor has.");
    return g_strdup_printf ("%d %% of the monitor's pixels, scaled %s.", p, p < 100 ? "up" : "down");
}

static WalpeMonresOption *option_new (const WalpeMonres *v, int w, int h, gboolean exceeds)
{
    WalpeMonresOption *o = g_new0 (WalpeMonresOption, 1);
    o->value = *v;
    o->w = w;
    o->h = h;
    o->exceeds = exceeds;
    return o;
}

GPtrArray *walpe_monres_options (const WalpeMonresMon *m, gboolean exceed)
{
    GPtrArray *a = g_ptr_array_new_with_free_func (option_free);
    gboolean known = m && m->mode_w > 0 && m->mode_h > 0;
    int native_lines = known ? MIN (m->mode_w, m->mode_h) : 0;
    WalpeMonres v = { WALPE_MONRES_NATIVE, 0, 0, 0 };
    int w = 0, h = 0;

    walpe_monres_mon_size (m, &v, &w, &h);
    WalpeMonresOption *o = option_new (&v, w, h, FALSE);
    o->label = w > 0 ? g_strdup_printf ("Native (%d × %d)", w, h) : g_strdup ("Native");
    g_ptr_array_add (a, o);

    /* the presets in question, smallest first: the built-in ones and any
     * other the engine offers for this monitor */
    int cand[WALPE_MONRES_N_PRESETS + MAX_OFFERED];
    guint n = 0;
    for (guint i = 0; i < G_N_ELEMENTS (presets); i++)
        cand[n++] = presets[i].lines;
    for (guint i = 0; m && m->offered && i < m->offered->len; i++) {
        const WalpeMonresOption *e = m->offered->pdata[i];
        gboolean have = e->value.kind != WALPE_MONRES_LINES;
        for (guint k = 0; k < n && !have; k++)
            have = cand[k] == e->value.lines;
        if (!have && n < G_N_ELEMENTS (cand))
            cand[n++] = e->value.lines;
    }
    for (guint i = 1; i < n; i++) {         /* a handful: insertion sort */
        int c = cand[i];
        guint k = i;
        for (; k > 0 && cand[k - 1] > c; k--)
            cand[k] = cand[k - 1];
        cand[k] = c;
    }
    for (guint i = 0; i < n; i++) {
        v.kind = WALPE_MONRES_LINES;
        v.lines = cand[i];
        gboolean stored = m && m->knows && walpe_monres_equal (&m->choice, &v);
        gboolean larger = known && cand[i] > native_lines;
        gboolean offered = FALSE;
        for (guint k = 0; m && m->offered && k < m->offered->len && !offered; k++)
            offered = walpe_monres_equal (&((WalpeMonresOption *) m->offered->pdata[k])->value, &v);
        /* without a list from the engine: everything that is not larger */
        if (!(m && m->offered) && !larger)
            offered = TRUE;
        if (!offered && !stored && !(larger && exceed))
            continue;
        walpe_monres_mon_size (m, &v, &w, &h);
        o = option_new (&v, w, h, larger);
        const char *pl = preset_label (cand[i]);
        char *own = pl ? NULL : g_strdup_printf ("%dp", cand[i]);
        o->label = g_strdup_printf ("%s (%d × %d)%s", pl ? pl : own, w, h,
                                    larger ? ", larger than the monitor" : "");
        g_free (own);
        g_ptr_array_add (a, o);
    }

    v.kind = WALPE_MONRES_CUSTOM;
    v.lines = 0;
    o = option_new (&v, 0, 0, FALSE);
    o->label = g_strdup ("Custom size");
    g_ptr_array_add (a, o);
    return a;
}

int walpe_monres_options_find (GPtrArray *options, const WalpeMonres *v)
{
    for (guint i = 0; options && v && i < options->len; i++) {
        const WalpeMonresOption *o = options->pdata[i];
        if (o->value.kind != v->kind)
            continue;
        if (v->kind != WALPE_MONRES_LINES || o->value.lines == v->lines)
            return (int) i;
    }
    return -1;
}

/* ------------------------------------------------------------ the state */

/* a JSON number as a pixel count 0 .. 99999; anything else is -1 */
static int px_of (JsonNode *n)
{
    if (!n || !JSON_NODE_HOLDS_VALUE (n))
        return -1;
    GType t = json_node_get_value_type (n);
    if (t != G_TYPE_INT64 && t != G_TYPE_DOUBLE)
        return -1;
    double d = json_node_get_double (n);
    return d >= 0 && d <= 99999 ? (int) d : -1;     /* (a NaN fails both) */
}

/* a[at], a[at + 1] as a size. zero_ok: 0 x 0 counts (it means "none") */
static gboolean pair_at (JsonArray *a, guint at, gboolean zero_ok, int *w, int *h)
{
    if (wpj_arr_len (a) < at + 2)
        return FALSE;
    int ww = px_of (json_array_get_element (a, at));
    int hh = px_of (json_array_get_element (a, at + 1));
    if (ww < 0 || hh < 0 || (!zero_ok && (ww == 0 || hh == 0)))
        return FALSE;
    *w = ww;
    *h = hh;
    return TRUE;
}

static gboolean read_pair (JsonObject *o, const char *key, guint at, int *w, int *h)
{
    return pair_at (wpj_arr (o, key), at, FALSE, w, h);
}

/* a texture limit member: text, or a number of pixels (0 = none) */
static gboolean read_tex (JsonObject *o, const char *key, WalpeTexLimit *out)
{
    JsonNode *n = json_object_get_member (o, key);
    if (!n || !JSON_NODE_HOLDS_VALUE (n))
        return FALSE;
    if (json_node_get_value_type (n) == G_TYPE_STRING)
        return walpe_tex_limit_parse (json_node_get_string (n), out);
    int px = px_of (n);
    if (px < 0)
        return FALSE;
    out->kind = px > 0 ? WALPE_LIMIT_VALUE : WALPE_LIMIT_NONE;
    out->px = px;
    return TRUE;
}

/* strictly false: "not asked" is never read out of a missing or odd member */
static gboolean is_false (JsonObject *o, const char *key)
{
    JsonNode *n = json_object_get_member (o, key);
    return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_BOOLEAN &&
           !json_node_get_boolean (n);
}

static WalpeMonresMon *mon_parse (JsonObject *o, guint pos)
{
    WalpeMonresMon *m = g_new0 (WalpeMonresMon, 1);
    m->index = walpe_d2i (wpj_num (o, K_MON_INDEX, pos));
    if (m->index < 0 || m->index > 9999)
        m->index = (int) pos;
    m->name = g_strdup (wpj_str (o, K_MON_NAME, ""));
    m->connector = g_strdup (wpj_str (o, K_MON_CONNECTOR, ""));
    m->description = g_strdup (wpj_str (o, K_MON_DESC, ""));
    if (!*m->description) {
        const char *make = wpj_str (o, K_MON_MAKE, ""), *model = wpj_str (o, K_MON_MODEL, "");
        if (*make || *model) {
            g_free (m->description);
            m->description = g_strdup_printf ("%s%s%s", make, *make && *model ? " " : "", model);
        }
    }
    if (!read_pair (o, K_MON_MODE, 0, &m->mode_w, &m->mode_h))
        read_pair (o, K_MON_GEOMETRY, 2, &m->mode_w, &m->mode_h);
    double hz = wpj_num (o, K_MON_REFRESH, 0);
    m->refresh = hz > 0 && hz < 10000 ? hz : 0;     /* also drops a NaN */
    m->asked = TRUE;
    if (json_object_has_member (o, K_MON_RES)) {
        m->knows = TRUE;
        const char *t = wpj_str (o, K_MON_RES, NULL);
        if (!t || !walpe_monres_parse (t, &m->choice))
            m->choice_raw = g_strdup (t ? t : "");
        /* only an engine that knows the feature can say "not asked" */
        if (is_false (o, K_MON_ASKED) || is_false (o, K_MON_ASKED_ST))
            m->asked = FALSE;
    }
    read_pair (o, K_MON_RENDER, 0, &m->render_w, &m->render_h);
    m->tex_known = read_tex (o, K_MON_TEX, &m->tex);
    m->tex_now = MAX (px_of (json_object_get_member (o, K_MON_TEX_NOW)), 0);

    JsonArray *ch = wpj_arr (o, K_MON_CHOICES);
    if (ch) {
        int native_lines = MIN (m->mode_w, m->mode_h);
        m->offered = g_ptr_array_new_with_free_func (option_free);
        for (guint i = 0; i < wpj_arr_len (ch) && m->offered->len < MAX_OFFERED; i++) {
            JsonObject *c = wpj_arr_obj (ch, i);
            WalpeMonres v;
            if (!c || !walpe_monres_parse (wpj_str (c, "value", NULL), &v))
                continue;
            int w = 0, h = 0;
            read_pair (c, "size", 0, &w, &h);
            g_ptr_array_add (m->offered,
                             option_new (&v, w, h, v.kind == WALPE_MONRES_LINES && native_lines > 0 &&
                                                   v.lines > native_lines));
        }
    }
    return m;
}

WalpeMonresState *walpe_monres_state_new (JsonObject *o)
{
    WalpeMonresState *st = g_new0 (WalpeMonresState, 1);
    st->mons = g_ptr_array_new_with_free_func (mon_free);
    st->prompt = TRUE;
    st->min_w = WALPE_MONRES_MIN_W;
    st->min_h = WALPE_MONRES_MIN_H;
    st->max_side = WALPE_MONRES_MAX_SIDE;
    st->max_pixels = WALPE_MONRES_MAX_PIXELS;
    if (!o)
        return st;

    JsonArray *ma = wpj_arr (o, K_MONITORS);
    for (guint i = 0; i < wpj_arr_len (ma) && st->mons->len < MAX_MONITORS; i++) {
        JsonObject *mo = wpj_arr_obj (ma, i);
        if (!mo)
            continue;
        if (is_false (mo, K_MON_CONNECTED)) {
            st->stored_only++;
            st->supported = TRUE;
            continue;
        }
        WalpeMonresMon *m = mon_parse (mo, i);
        if (m->knows)
            st->supported = TRUE;
        g_ptr_array_add (st->mons, m);
    }
    const char *pk = json_object_has_member (o, K_PROMPT_ST) ? K_PROMPT_ST
                   : json_object_has_member (o, K_PROMPT) ? K_PROMPT : NULL;
    if (pk) {
        st->supported = TRUE;
        st->prompt = !is_false (o, pk);
    }
    if (json_object_has_member (o, K_UNASKED_ST) || json_object_has_member (o, K_UNASKED))
        st->supported = TRUE;
    st->fullscreen = json_object_has_member (o, K_FULLSCREEN)
                         ? wpj_bool (o, K_FULLSCREEN, FALSE)
                         : wpj_bool (o, K_FULLSCREEN_OLD, FALSE);

    JsonObject *lim = wpj_obj (o, K_LIMITS);
    if (lim) {
        st->full = TRUE;
        int a = px_of (json_object_get_member (lim, "min_width"));
        int b = px_of (json_object_get_member (lim, "min_height"));
        int c = px_of (json_object_get_member (lim, "max_side"));
        double p = wpj_num (lim, "max_pixels", 0);
        /* limits that are none (a largest side below the smallest size)
         * leave the built-in ones */
        if (a > 0 && b > 0 && c >= a && c >= b) {
            st->min_w = a;
            st->min_h = b;
            st->max_side = c;
        }
        if (p >= (double) st->min_w * st->min_h && p <= 4e9)
            st->max_pixels = (gint64) p;
    }

    if (json_object_has_member (o, K_VIDEO))
        st->video_known = walpe_video_limit_parse (wpj_str (o, K_VIDEO, NULL), &st->video);
    st->video_now_known = pair_at (wpj_arr (o, K_VIDEO_NOW), 0, TRUE, &st->video_now_w,
                                   &st->video_now_h);
    const char *f = wpj_str (o, K_FILTER, NULL);
    if (f && *f)
        st->scale_filter = g_strdup (f);
    const char *note = wpj_str (o, K_NOTE, NULL);
    if (note && *note)
        st->note = walpe_ui_text (note, 300, WALPE_UITEXT_LINE);
    return st;
}

void walpe_monres_state_free (WalpeMonresState *st)
{
    if (!st)
        return;
    g_ptr_array_unref (st->mons);
    g_free (st->scale_filter);
    g_free (st->note);
    g_free (st);
}

const WalpeMonresMon *walpe_monres_state_find (const WalpeMonresState *st, const char *name)
{
    for (guint i = 0; st && name && i < st->mons->len; i++) {
        const WalpeMonresMon *m = st->mons->pdata[i];
        if (!strcmp (m->name, name))
            return m;
    }
    return NULL;
}

char *walpe_monres_state_signature (const WalpeMonresState *st)
{
    GString *s = g_string_new (NULL);
    if (!st)
        return g_string_free (s, FALSE);
    g_string_append_printf (s, "%d%d%d|%d,%d,%d,%" G_GINT64_FORMAT "|%u|", st->supported, st->full,
                            st->prompt, st->min_w, st->min_h, st->max_side, st->max_pixels,
                            st->stored_only);
    for (guint i = 0; i < st->mons->len; i++) {
        const WalpeMonresMon *m = st->mons->pdata[i];
        char *c = walpe_monres_text (&m->choice);
        char *t = walpe_tex_limit_text (&m->tex);
        char hz[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd (hz, sizeof (hz), "%.3f", m->refresh);
        g_string_append_printf (s, "%d\t%s\t%s\t%s\t%dx%d@%s\t%d%d\t%s\t%s\t%dx%d\t%d%s,%d", m->index,
                                m->name, m->connector, m->description, m->mode_w, m->mode_h, hz,
                                m->knows, m->asked, c, m->choice_raw ? m->choice_raw : "",
                                m->render_w, m->render_h, m->tex_known, t, m->tex_now);
        for (guint k = 0; m->offered && k < m->offered->len; k++) {
            const WalpeMonresOption *o = m->offered->pdata[k];
            char *ov = walpe_monres_text (&o->value);
            g_string_append_printf (s, ";%s=%dx%d", ov, o->w, o->h);
            g_free (ov);
        }
        g_string_append_c (s, '\n');
        g_free (c);
        g_free (t);
    }
    char *v = walpe_video_limit_text (&st->video);
    g_string_append_printf (s, "V%d %s %d %dx%d|F%s", st->video_known, v, st->video_now_known,
                            st->video_now_w, st->video_now_h, st->scale_filter ? st->scale_filter : "");
    g_free (v);
    return g_string_free (s, FALSE);
}

guint walpe_monres_state_unasked (const WalpeMonresState *st, GPtrArray *names)
{
    guint n = 0;
    if (!st || !st->supported || !st->prompt)
        return 0;
    for (guint i = 0; i < st->mons->len; i++) {
        const WalpeMonresMon *m = st->mons->pdata[i];
        /* (a monitor the engine cannot store an answer for is not asked
         * about: the question would come back after every answer) */
        if (m->asked || !walpe_monres_name_ok (m->name))
            continue;
        n++;
        if (names)
            g_ptr_array_add (names, g_strdup (m->name));
    }
    return n;
}

gboolean walpe_monres_state_texture (const WalpeMonresState *st, WalpeTexLimit *common, gboolean *any)
{
    const WalpeTexLimit *first = NULL;
    gboolean same = TRUE;
    for (guint i = 0; st && i < st->mons->len; i++) {
        const WalpeMonresMon *m = st->mons->pdata[i];
        if (!m->tex_known)
            continue;
        if (!first)
            first = &m->tex;
        else if (!walpe_tex_limit_equal (first, &m->tex))
            same = FALSE;
    }
    if (any)
        *any = first != NULL;
    if (first && same && common)
        *common = *first;
    return first && same;
}

/* ----------------------------------------------------- a custom size */

/* a whole field: blanks, digits, blanks. 0 = empty, -1 = not a number */
static int field_number (const char *s)
{
    if (!s)
        return 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
        return 0;
    const char *e;
    int v = 0;
    if (!read_side (s, &v, &e))
        return -1;
    while (*e == ' ' || *e == '\t')
        e++;
    return *e ? -1 : v;
}

WalpeMonresCheck walpe_monres_check_custom (const WalpeMonresState *st, const WalpeMonresMon *m,
                                            const char *wtext, const char *htext,
                                            int *w, int *h, char **msg)
{
    int min_w = st ? st->min_w : WALPE_MONRES_MIN_W, min_h = st ? st->min_h : WALPE_MONRES_MIN_H;
    int max_side = st ? st->max_side : WALPE_MONRES_MAX_SIDE;
    gint64 max_px = st ? st->max_pixels : WALPE_MONRES_MAX_PIXELS;
    int ww = field_number (wtext), hh = field_number (htext);
    char *text = NULL;
    WalpeMonresCheck res = WALPE_MONRES_CHECK_OK;
    if (w)
        *w = MAX (ww, 0);
    if (h)
        *h = MAX (hh, 0);
    if (ww == 0 || hh == 0) {
        res = WALPE_MONRES_CHECK_BAD;
        text = g_strdup ("Enter a width and a height in pixels.");
    } else if (ww < 0 || hh < 0) {
        res = WALPE_MONRES_CHECK_BAD;
        text = g_strdup ("Width and height are whole numbers of pixels.");
    } else if (ww < min_w || hh < min_h) {
        res = WALPE_MONRES_CHECK_BAD;
        text = g_strdup_printf ("Too small: at least %d × %d.", min_w, min_h);
    } else if (ww > max_side || hh > max_side) {
        res = WALPE_MONRES_CHECK_BAD;
        text = g_strdup_printf ("Too large: at most %d pixels a side.", max_side);
    } else if ((gint64) ww * hh > max_px) {
        res = WALPE_MONRES_CHECK_BAD;
        text = g_strdup_printf ("Too large: at most %d million pixels in all.",
                                (int) (max_px / 1000000));
    } else if (m && m->mode_w > 0 && m->mode_h > 0) {
        /* shapes compared as w1 * h2 against w2 * h1, one per cent apart */
        double a = (double) ww * m->mode_h, b = (double) m->mode_w * hh;
        gboolean other_shape = fabs (a - b) > 0.01 * MAX (a, b);
        gboolean more = (gint64) ww * hh > (gint64) m->mode_w * m->mode_h;
        if (more || other_shape) {
            res = WALPE_MONRES_CHECK_NOTE;
            char *cost = walpe_monres_cost_note (m, ww, hh);
            GString *s = g_string_new (cost);
            g_free (cost);
            if (other_shape)
                g_string_append_printf (s, "%sNot the monitor's shape (%d × %d): the picture is "
                                        "stretched to fit.", s->len ? " " : "", m->mode_w, m->mode_h);
            text = g_string_free (s, FALSE);
        }
    }
    if (msg)
        *msg = text;
    else
        g_free (text);
    return res;
}

/* ------------------------------------------------------------- requests */

static char *builder_text (JsonBuilder *b)
{
    JsonNode *root = json_builder_get_root (b);
    char *s = json_to_string (root, FALSE);
    json_node_unref (root);
    g_object_unref (b);
    return s;
}

static JsonBuilder *request_begin (const char *cmd)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    json_builder_add_string_value (b, cmd);
    return b;
}

char *walpe_monres_request_list (void)
{
    JsonBuilder *b = request_begin (CMD_RESOLUTION);
    json_builder_end_object (b);
    return builder_text (b);
}

char *walpe_monres_request_apply (const WalpeMonresAssign *a, guint n)
{
    guint used = 0;
    JsonBuilder *b = request_begin (CMD_RESOLUTION);
    json_builder_set_member_name (b, REQ_SET);
    json_builder_begin_array (b);
    for (guint i = 0; a && i < n; i++) {
        if (!a[i].name || !*a[i].name || (!a[i].set_value && !a[i].set_tex && a[i].asked < 0))
            continue;
        used++;
        json_builder_begin_object (b);
        json_builder_set_member_name (b, REQ_MONITOR);
        json_builder_add_string_value (b, a[i].name);
        if (a[i].set_value) {
            char *v = walpe_monres_text (&a[i].value);
            json_builder_set_member_name (b, REQ_VALUE);
            json_builder_add_string_value (b, v);
            g_free (v);
        }
        if (a[i].set_tex) {
            char *t = walpe_tex_limit_text (&a[i].tex);
            json_builder_set_member_name (b, REQ_TEX);
            json_builder_add_string_value (b, t);
            g_free (t);
        }
        if (a[i].asked >= 0) {
            json_builder_set_member_name (b, REQ_ASKED);
            json_builder_add_boolean_value (b, a[i].asked != 0);
        }
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    json_builder_end_object (b);
    char *s = builder_text (b);
    if (!used)
        g_clear_pointer (&s, g_free);
    return s;
}

char *walpe_monres_request_prompt (gboolean on)
{
    JsonBuilder *b = request_begin (CMD_RESOLUTION);
    json_builder_set_member_name (b, REQ_PROMPT);
    json_builder_add_boolean_value (b, on);
    json_builder_end_object (b);
    return builder_text (b);
}

char *walpe_monres_request_video (const WalpeVideoLimit *v)
{
    char *t = walpe_video_limit_text (v);
    JsonBuilder *b = request_begin ("set");
    json_builder_set_member_name (b, K_VIDEO);
    json_builder_add_string_value (b, t);
    json_builder_end_object (b);
    g_free (t);
    return builder_text (b);
}

char *walpe_monres_request_filter (const char *name)
{
    if (!name || !*name)
        return NULL;
    JsonBuilder *b = request_begin ("set");
    json_builder_set_member_name (b, K_FILTER);
    json_builder_add_string_value (b, name);
    json_builder_end_object (b);
    return builder_text (b);
}

/* ------------------------------------------------- new monitors (hot-plug) */

struct WalpeMonresTracker {
    gboolean primed;        /* the first state with the feature and a monitor was seen */
    GHashTable *seen;       /* output names decided about (start-up ones, announced ones) */
    GHashTable *announced;  /* announced and not taken back */
    GHashTable *waiting;    /* to announce once no fullscreen application has the focus */
};

WalpeMonresTracker *walpe_monres_tracker_new (void)
{
    WalpeMonresTracker *t = g_new0 (WalpeMonresTracker, 1);
    t->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    t->announced = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    t->waiting = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    return t;
}

void walpe_monres_tracker_free (WalpeMonresTracker *t)
{
    if (!t)
        return;
    g_hash_table_unref (t->seen);
    g_hash_table_unref (t->announced);
    g_hash_table_unref (t->waiting);
    g_free (t);
}

guint walpe_monres_tracker_waiting (const WalpeMonresTracker *t)
{
    return t ? g_hash_table_size (t->waiting) : 0;
}

void walpe_monres_tracker_unshown (WalpeMonresTracker *t, GPtrArray *names)
{
    for (guint i = 0; t && names && i < names->len; i++)
        g_hash_table_remove (t->announced, names->pdata[i]);
}

/* name is still a monitor to ask about */
static gboolean still_open (const WalpeMonresState *st, const char *name)
{
    const WalpeMonresMon *m = walpe_monres_state_find (st, name);
    return st->prompt && m && !m->asked;
}

GPtrArray *walpe_monres_tracker_feed (WalpeMonresTracker *t, const WalpeMonresState *st,
                                      guint *startup, GPtrArray **withdraw)
{
    if (startup)
        *startup = 0;
    if (withdraw)
        *withdraw = NULL;
    if (!t || !st || !st->supported || !st->mons->len)
        return NULL;

    GHashTableIter it;
    gpointer key;
    GPtrArray *back = NULL;
    g_hash_table_iter_init (&it, t->announced);
    while (g_hash_table_iter_next (&it, &key, NULL)) {
        if (still_open (st, key))
            continue;
        if (!back)
            back = g_ptr_array_new_with_free_func (g_free);
        g_ptr_array_add (back, g_strdup (key));
        g_hash_table_iter_remove (&it);
    }
    g_hash_table_iter_init (&it, t->waiting);
    while (g_hash_table_iter_next (&it, &key, NULL))
        if (!still_open (st, key))
            g_hash_table_iter_remove (&it);
    if (withdraw)
        *withdraw = back;
    else if (back)
        g_ptr_array_unref (back);

    guint first = 0;
    for (guint i = 0; i < st->mons->len; i++) {
        const WalpeMonresMon *m = st->mons->pdata[i];
        if (m->asked || !walpe_monres_name_ok (m->name) ||
            g_hash_table_contains (t->seen, m->name))
            continue;
        g_hash_table_add (t->seen, g_strdup (m->name));
        if (!st->prompt)
            continue;
        if (!t->primed)
            first++;
        else
            g_hash_table_add (t->waiting, g_strdup (m->name));
    }
    if (!t->primed) {
        t->primed = TRUE;
        if (startup)
            *startup = first;
        return NULL;
    }
    if (st->fullscreen || !g_hash_table_size (t->waiting))
        return NULL;
    /* in the engine's order */
    GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; i < st->mons->len; i++) {
        const WalpeMonresMon *m = st->mons->pdata[i];
        if (!g_hash_table_contains (t->waiting, m->name))
            continue;
        g_ptr_array_add (out, g_strdup (m->name));
        g_hash_table_add (t->announced, g_strdup (m->name));
    }
    g_hash_table_remove_all (t->waiting);
    return out;
}

/* Text for the body of a desktop notification. Most notification services
 * read the body as markup (the title is plain text everywhere), and a
 * monitor's name is text from outside: it must not start a tag or an
 * entity there. Escaping is no way out - a service that takes the body as
 * plain text would show the escapes - so the three characters that mean
 * something in markup are replaced. g_free */
static char *body_plain (const char *s)
{
    char *t = g_strdup (s ? s : "");
    for (char *p = t; *p; p++) {
        if (*p == '<')
            *p = '(';
        else if (*p == '>')
            *p = ')';
        else if (*p == '&')
            *p = '+';
    }
    return t;
}

void walpe_monres_announce_text (const WalpeMonresState *st, GPtrArray *names, char **title,
                                 char **body)
{
    guint n = names ? names->len : 0;
    GString *list = g_string_new (NULL);
    const WalpeMonresMon *one = n == 1 ? walpe_monres_state_find (st, names->pdata[0]) : NULL;
    for (guint i = 0; i < n; i++) {
        const WalpeMonresMon *m = walpe_monres_state_find (st, names->pdata[i]);
        char *t = m ? walpe_monres_mon_title (m) : shown (names->pdata[i]);
        g_string_append_printf (list, "%s%s", i ? ", " : "", t);
        g_free (t);
    }
    if (title)
        *title = n == 1 ? g_strdup_printf ("New monitor: %s", list->str)
                        : g_strdup ("New monitors connected");
    if (body) {
        if (one && one->mode_w > 0 && one->mode_h > 0)
            *body = g_strdup_printf ("It is rendered at its own resolution (%d × %d). You can "
                                     "choose another render resolution.", one->mode_w, one->mode_h);
        else if (n == 1)
            *body = g_strdup ("It is rendered at its own resolution. You can choose another "
                              "render resolution.");
        else {
            char *who = body_plain (list->str);
            *body = g_strdup_printf ("%s are rendered at their own resolution. You can choose "
                                     "another render resolution for each.", who);
            g_free (who);
        }
    }
    g_string_free (list, TRUE);
}
