/* props.c - "Wallpaper options" editor (see props.h) */
#include "props.h"
#include "uiipc.h"
#include "numfield.h"
#include "a11yname.h"
#include "common/util.h"
#include "engine/content.h"   /* walpe_content_property_defs */
#include "engine/propnotes.h" /* walpe_prop_notes */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEBOUNCE_MS 150

typedef enum { K_BOOL, K_SLIDER, K_COMBO, K_COLOR, K_TEXT, K_FILE, K_SECTION, K_NOTE, K_READONLY } Kind;

typedef struct {
    char *key;
    char *type;
    Kind kind;
    char *condition;
    char *title;
    char *search;          /* casefolded title + subtitle + key */
    GtkWidget *row;        /* GtkListBoxRow */
    GtkWidget *title_lbl;
    GtkWidget *ctl;        /* switch / scale / dropdown / colour button / entry */
    GtkAdjustment *adj;    /* slider */
    GtkWidget *reset;
    JsonNode *dflt;        /* default value (owned) */
    JsonNode *combo_values;/* combo: array of option values (owned) */
    gboolean hex_color;    /* the default is "#rrggbb": write that form back */
    gboolean is_dir;       /* K_FILE: a folder ("directory") */
    gboolean want_video;   /* K_FILE: "fileType": "video" */
    GtkWidget *file_lbl;   /* K_FILE: the chosen file's name on the button */
    gboolean is_int;       /* slider without fraction: integer values */
    gboolean cond_ok;
    gboolean filter_ok;
} PRow;

typedef struct {
    JsonObject *vals;      /* key -> value (owned) */
    gboolean reset;        /* resetprops first */
} Staged;

typedef struct {
    GtkWidget *box;
    GtkWidget *list;
    GtkWidget *scroller;
    GtkWidget *hint;
    GtkWidget *count;
    GtkWidget *reset_all;
    GtkWidget *filter;
    GtkWidget *empty;
    WalpePropsStatus status;
    GPtrArray *rows;       /* PRow */
    GHashTable *by_key;    /* key -> PRow (borrowed) */
    JsonObject *live;      /* key -> current value: conditions + reset state */
    JsonObject *dirty;     /* edits waiting for the debounce */
    GHashTable *staged;    /* path -> Staged (older daemons) */
    guint debounce_id;
    char *path;
    JsonObject *defs, *loc, *loc_lang;
    JsonNode *own_defs, *own_loc;   /* the effective definitions (presets, built-ins) */
    JsonObject *notes;              /* key -> why the option cannot do anything (propnotes.h) */
    gboolean is_current, by_path;
    gboolean building;
    char *filter_text;
    guint gen;             /* bumps per show(): late IPC replies for older
                              wallpapers only report, never touch rows */
    GArray *pending;       /* Ord: rows still to build (chunked, idle) */
    guint build_pos;
    guint build_id;
    gint64 build_t0;
} Panel;

static Panel *panel_of (GtkWidget *w) { return g_object_get_data (G_OBJECT (w), "walpe-props"); }

static void prow_free (gpointer p)
{
    PRow *r = p;
    g_free (r->key);
    g_free (r->type);
    g_free (r->condition);
    g_free (r->title);
    g_free (r->search);
    if (r->dflt)
        json_node_unref (r->dflt);
    if (r->combo_values)
        json_node_unref (r->combo_values);
    g_free (r);
}

static void staged_free (gpointer p)
{
    Staged *s = p;
    json_object_unref (s->vals);
    g_free (s);
}

/* ============================================================ values */

static gboolean node_is_scalar (JsonNode *n)
{
    return n && JSON_NODE_HOLDS_VALUE (n);
}

static gboolean str_to_num (const char *s, double *out)
{
    if (!s)
        return FALSE;
    while (g_ascii_isspace (*s))
        s++;
    if (!*s)
        return FALSE;
    char *end = NULL;
    double d = g_ascii_strtod (s, &end);
    while (end && g_ascii_isspace (*end))
        end++;
    if (!end || *end || !isfinite (d))
        return FALSE;
    *out = d;
    return TRUE;
}

static double node_num (JsonNode *n, double d)
{
    if (!node_is_scalar (n))
        return d;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_STRING) {
        double v;
        return str_to_num (json_node_get_string (n), &v) ? v : d;
    }
    if (t == G_TYPE_BOOLEAN)
        return json_node_get_boolean (n);
    double v = json_node_get_double (n);
    return isfinite (v) ? v : d;
}

static gboolean node_bool (JsonNode *n, gboolean d)
{
    if (!node_is_scalar (n))
        return d;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_BOOLEAN)
        return json_node_get_boolean (n);
    if (t == G_TYPE_STRING) {
        const char *s = json_node_get_string (n);
        return s && (!g_ascii_strcasecmp (s, "true") || !g_strcmp0 (s, "1"));
    }
    return json_node_get_double (n) != 0;
}

static char *node_str (JsonNode *n)
{
    if (!node_is_scalar (n))
        return g_strdup ("");
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_STRING)
        return g_strdup (json_node_get_string (n) ? json_node_get_string (n) : "");
    if (t == G_TYPE_BOOLEAN)
        return g_strdup (json_node_get_boolean (n) ? "true" : "false");
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    return g_strdup (g_ascii_dtostr (buf, sizeof (buf), json_node_get_double (n)));
}

static JsonNode *mk_bool (gboolean b)
{
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_boolean (n, b);
    return n;
}

static JsonNode *mk_str (const char *s)
{
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_string (n, s ? s : "");
    return n;
}

static JsonNode *mk_num (double d, gboolean is_int)
{
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    if (is_int && fabs (d) < 9e15)
        json_node_set_int (n, (gint64) llround (d));
    else
        json_node_set_double (n, d);
    return n;
}

/* WE colours: "r g b" floats 0..1 (sometimes 0..255), or "#rrggbb" */
static gboolean parse_color (JsonNode *n, GdkRGBA *out)
{
    *out = (GdkRGBA) { 1, 1, 1, 1 };
    char *s = node_str (n);
    gboolean ok = FALSE;
    g_strstrip (s);
    if (s[0] == '#' || g_str_has_prefix (s, "rgb")) {
        ok = gdk_rgba_parse (out, s);
        out->alpha = 1;
    } else {
        double c[3];
        if (sscanf (s, "%lf %lf %lf", &c[0], &c[1], &c[2]) == 3 && isfinite (c[0]) &&
            isfinite (c[1]) && isfinite (c[2])) {
            if (c[0] > 1 || c[1] > 1 || c[2] > 1)
                for (int k = 0; k < 3; k++)
                    c[k] /= 255.0;
            *out = (GdkRGBA) { (float) CLAMP (c[0], 0, 1), (float) CLAMP (c[1], 0, 1),
                               (float) CLAMP (c[2], 0, 1), 1.0f };
            ok = TRUE;
        }
    }
    g_free (s);
    return ok;
}

static char *format_color (const GdkRGBA *c, gboolean hex)
{
    if (hex)
        return g_strdup_printf ("#%02x%02x%02x", (int) lround (c->red * 255),
                                (int) lround (c->green * 255), (int) lround (c->blue * 255));
    char r[G_ASCII_DTOSTR_BUF_SIZE], g[G_ASCII_DTOSTR_BUF_SIZE], b[G_ASCII_DTOSTR_BUF_SIZE];
    return g_strdup_printf ("%s %s %s", g_ascii_formatd (r, sizeof (r), "%.5f", c->red),
                            g_ascii_formatd (g, sizeof (g), "%.5f", c->green),
                            g_ascii_formatd (b, sizeof (b), "%.5f", c->blue));
}

/* loose equality of two property values (for "modified?" and combos) */
static gboolean values_equal (PRow *r, JsonNode *a, JsonNode *b)
{
    if (!node_is_scalar (a) || !node_is_scalar (b))
        return node_is_scalar (a) == node_is_scalar (b);
    if (r && r->kind == K_COLOR) {
        GdkRGBA x, y;
        parse_color (a, &x);
        parse_color (b, &y);
        return fabsf (x.red - y.red) < 0.003f && fabsf (x.green - y.green) < 0.003f &&
               fabsf (x.blue - y.blue) < 0.003f;
    }
    if (r && r->kind == K_BOOL)
        return node_bool (a, FALSE) == node_bool (b, FALSE);
    double x, y;
    char *sa = node_str (a), *sb = node_str (b);
    gboolean eq;
    if (str_to_num (sa, &x) && str_to_num (sb, &y))
        eq = fabs (x - y) <= 1e-6 * MAX (1.0, fabs (x));
    else
        eq = !g_strcmp0 (sa, sb);
    g_free (sa);
    g_free (sb);
    return eq;
}

/* ===================================================== condition eval
 * WE property conditions are small JS expressions over other
 * properties: `a.value`, `!a.value`, `a.value == 2`, `b.value != "x"`,
 * `&&`, `||`, parentheses, <, >, <=, >=, ===, !==, the ternary `c ? a : b`
 * and assignments inside it (1509243786: `m.value == 2 ? x.value ? x.text
 * = 'on' : x.text = 'off' : false` - the assignment's value decides, its
 * relabelling side effect is not applied). Anything we cannot parse shows
 * the row (never hide options because of our parser). */

typedef enum { V_UNDEF, V_NUM, V_STR, V_BOOL } VKind;
typedef struct { VKind k; double n; char *s; gboolean b; } Val;
typedef struct { const char *p; JsonObject *live; gboolean err; int depth; } Cx;
/* the parser recurses per '(', '!' and '?': a condition from a workshop
 * item with 20 000 parentheses overflowed the GUI's stack. Deeper than this
 * is not a condition anybody wrote: the row is shown (err). */
#define CX_MAX_DEPTH 64
#define CX_MAX_LEN 4096

static void val_clear (Val *v) { g_free (v->s); v->s = NULL; }

static gboolean truthy (const Val *v)
{
    switch (v->k) {
    case V_NUM: return v->n != 0 && !isnan (v->n);
    case V_STR: return v->s && *v->s;
    case V_BOOL: return v->b;
    default: return FALSE;
    }
}

static gboolean val_num (const Val *v, double *out)
{
    if (v->k == V_NUM) { *out = v->n; return TRUE; }
    if (v->k == V_BOOL) { *out = v->b; return TRUE; }
    if (v->k == V_STR) return str_to_num (v->s, out);
    return FALSE;
}

static void skip_ws (Cx *c) { while (g_ascii_isspace (*c->p)) c->p++; }

static Val cx_or (Cx *c);
static Val cx_expr (Cx *c);

static Val node_to_val (JsonNode *n)
{
    Val v = { V_UNDEF, 0, NULL, FALSE };
    if (!node_is_scalar (n))
        return v;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_BOOLEAN) {
        v.k = V_BOOL;
        v.b = json_node_get_boolean (n);
    } else if (t == G_TYPE_STRING) {
        v.k = V_STR;
        v.s = g_strdup (json_node_get_string (n));
    } else {
        v.k = V_NUM;
        v.n = json_node_get_double (n);
    }
    return v;
}

static Val cx_primary (Cx *c)
{
    Val v = { V_UNDEF, 0, NULL, FALSE };
    skip_ws (c);
    char ch = *c->p;
    if (ch == '(') {
        c->p++;
        v = cx_expr (c);
        skip_ws (c);
        if (*c->p == ')')
            c->p++;
        else
            c->err = TRUE;
        return v;
    }
    if (ch == '"' || ch == '\'') {
        const char *s = ++c->p;
        while (*c->p && *c->p != ch)
            c->p++;
        if (!*c->p) {
            c->err = TRUE;
            return v;
        }
        v.k = V_STR;
        v.s = g_strndup (s, (gsize) (c->p - s));
        c->p++;
        return v;
    }
    if (g_ascii_isdigit (ch) || (ch == '-' && g_ascii_isdigit (c->p[1])) || ch == '.') {
        char *end = NULL;
        v.n = g_ascii_strtod (c->p, &end);
        if (!end || end == c->p) {
            c->err = TRUE;
            return v;
        }
        c->p = end;
        v.k = V_NUM;
        return v;
    }
    if (g_ascii_isalpha (ch) || ch == '_' || ch == '$') {
        const char *s = c->p;
        while (g_ascii_isalnum (*c->p) || *c->p == '_' || *c->p == '$')
            c->p++;
        char *id = g_strndup (s, (gsize) (c->p - s));
        if (!strcmp (id, "true") || !strcmp (id, "false")) {
            v.k = V_BOOL;
            v.b = id[0] == 't';
        } else if (!strcmp (id, "null") || !strcmp (id, "undefined")) {
            v.k = V_UNDEF;
        } else {
            /* property reference: name[.value] (other members: undefined);
             * "name .value" is the same in JS */
            gboolean value_member = TRUE;
            const char *save = c->p;
            skip_ws (c);
            if (*c->p == '.') {
                c->p++;
                skip_ws (c);
                const char *m = c->p;
                while (g_ascii_isalnum (*c->p) || *c->p == '_')
                    c->p++;
                value_member = (c->p - m == 5 && !strncmp (m, "value", 5));
            } else {
                c->p = save;
            }
            JsonNode *n = value_member && c->live ? json_object_get_member (c->live, id) : NULL;
            v = node_to_val (n);
            /* `x.text = 'label'`: an assignment evaluates to its right side */
            const char *save2 = c->p;
            skip_ws (c);
            if (c->p[0] == '=' && c->p[1] != '=') {
                c->p++;
                val_clear (&v);
                v = cx_expr (c);
            } else {
                c->p = save2;
            }
        }
        g_free (id);
        return v;
    }
    c->err = TRUE;
    return v;
}

static Val cx_unary (Cx *c)
{
    skip_ws (c);
    if (c->p[0] == '!' && c->p[1] != '=') {
        Val v = { V_BOOL, 0, NULL, FALSE };
        if (c->depth >= CX_MAX_DEPTH) {
            c->err = TRUE;
            return v;
        }
        c->p++;
        c->depth++;
        Val in = cx_unary (c);
        c->depth--;
        v.b = !truthy (&in);
        val_clear (&in);
        return v;
    }
    return cx_primary (c);
}

static gboolean loose_eq (const Val *a, const Val *b)
{
    if (a->k == V_UNDEF || b->k == V_UNDEF)
        return a->k == b->k;
    if (a->k == V_STR && b->k == V_STR)
        return !g_strcmp0 (a->s, b->s);
    double x, y;
    if (val_num (a, &x) && val_num (b, &y))
        return x == y;
    return FALSE;
}

static Val cx_cmp (Cx *c)
{
    Val a = cx_unary (c);
    for (;;) {
        skip_ws (c);
        const char *p = c->p;
        int op = 0; /* 1 == 2 != 3 < 4 > 5 <= 6 >= */
        gboolean strict = FALSE;
        if (p[0] == '=' && p[1] == '=') { op = 1; c->p += 2; if (*c->p == '=') { c->p++; strict = TRUE; } }
        else if (p[0] == '!' && p[1] == '=') { op = 2; c->p += 2; if (*c->p == '=') { c->p++; strict = TRUE; } }
        else if (p[0] == '<' && p[1] == '=') { op = 5; c->p += 2; }
        else if (p[0] == '>' && p[1] == '=') { op = 6; c->p += 2; }
        else if (p[0] == '<') { op = 3; c->p++; }
        else if (p[0] == '>') { op = 4; c->p++; }
        if (!op)
            return a;
        Val b = cx_unary (c);
        gboolean r;
        if (op <= 2) {
            r = loose_eq (&a, &b);
            if (strict && a.k != b.k)
                r = FALSE;
            if (op == 2)
                r = !r;
        } else {
            double x, y;
            if (!val_num (&a, &x) || !val_num (&b, &y))
                r = FALSE;
            else
                r = op == 3 ? x < y : op == 4 ? x > y : op == 5 ? x <= y : x >= y;
        }
        val_clear (&a);
        val_clear (&b);
        a = (Val) { V_BOOL, 0, NULL, r };
    }
}

static Val cx_and (Cx *c)
{
    Val a = cx_cmp (c);
    for (;;) {
        skip_ws (c);
        if (!(c->p[0] == '&' && c->p[1] == '&'))
            return a;
        c->p += 2;
        Val b = cx_cmp (c);
        gboolean r = truthy (&a) && truthy (&b);
        val_clear (&a);
        val_clear (&b);
        a = (Val) { V_BOOL, 0, NULL, r };
    }
}

static Val cx_or (Cx *c)
{
    Val a = cx_and (c);
    for (;;) {
        skip_ws (c);
        if (!(c->p[0] == '|' && c->p[1] == '|'))
            return a;
        c->p += 2;
        Val b = cx_and (c);
        gboolean r = truthy (&a) || truthy (&b);
        val_clear (&a);
        val_clear (&b);
        a = (Val) { V_BOOL, 0, NULL, r };
    }
}

/* expr := or [ '?' expr ':' expr ] */
static Val cx_expr_in (Cx *c);

static Val cx_expr (Cx *c)
{
    if (c->depth >= CX_MAX_DEPTH) {
        Val v = { V_UNDEF, 0, NULL, FALSE };
        c->err = TRUE;
        return v;
    }
    c->depth++;
    Val v = cx_expr_in (c);
    c->depth--;
    return v;
}

static Val cx_expr_in (Cx *c)
{
    Val a = cx_or (c);
    skip_ws (c);
    if (*c->p != '?')
        return a;
    c->p++;
    Val x = cx_expr (c);
    skip_ws (c);
    if (*c->p != ':') {
        c->err = TRUE;
        val_clear (&a);
        return x;
    }
    c->p++;
    Val y = cx_expr (c);
    gboolean t = truthy (&a);
    val_clear (&a);
    val_clear (t ? &y : &x);
    return t ? x : y;
}

static gboolean cond_visible (const char *expr, JsonObject *live)
{
    if (!expr || strlen (expr) > CX_MAX_LEN)
        return TRUE;
    Cx c = { expr, live, FALSE, 0 };
    skip_ws (&c);
    if (!*c.p)
        return TRUE;
    Val v = cx_expr (&c);
    skip_ws (&c);
    gboolean r = truthy (&v);
    val_clear (&v);
    if (c.err || *c.p)
        return TRUE;   /* unparsable: show */
    return r;
}

/* ============================================================ labels */

typedef struct {
    char *title;       /* first line, NULL = decoration only */
    char *sub;         /* further lines or NULL */
    gboolean heading;  /* <h*>/<big>/<b> around the first line */
    gboolean rule;     /* starts with <hr> */
    gboolean markup;   /* the text had tags */
    gboolean inert;    /* the wallpaper cannot react to this option (propnotes.h) */
    guint images;      /* <img> / [img] count */
    guint links;       /* <a href> / [url] count */
} Lab;

static void lab_clear (Lab *l)
{
    g_free (l->title);
    g_free (l->sub);
}

/* "weatherSlowUpdate" / "ui_browse_properties_scheme_color" -> words */
static char *humanize (const char *k)
{
    if (!k)
        return g_strdup ("");
    if (g_str_has_prefix (k, "ui_browse_properties_"))
        k += strlen ("ui_browse_properties_");
    else if (g_str_has_prefix (k, "ui_"))
        k += 3;
    GString *s = g_string_new (NULL);
    for (const char *p = k; *p; p++) {
        char ch = *p;
        if (ch == '_' || ch == '-') {
            if (s->len && s->str[s->len - 1] != ' ')
                g_string_append_c (s, ' ');
            continue;
        }
        if (g_ascii_isupper (ch) && p > k && g_ascii_islower (p[-1]) && s->len &&
            s->str[s->len - 1] != ' ')
            g_string_append_c (s, ' ');
        g_string_append_c (s, s->len ? g_ascii_tolower (ch) : g_ascii_toupper (ch));
    }
    if (!s->len)
        g_string_append (s, k);
    return g_string_free (s, FALSE);
}

/* Wallpaper Engine's own option labels (ui_browse_properties_*), which
 * wallpapers use without shipping a translation: English wording for the
 * ones that appear on user options. A wallpaper's "localization" block
 * still wins. */
static const char *builtin_loc (const char *key)
{
    static const struct { const char *k, *v; } tab[] = {
        { "ui_browse_properties_scheme_color", "Scheme color" },
        { "ui_browse_properties_playback_rate", "Playback rate" },
        { "ui_browse_properties_volume", "Volume" },
        { "ui_browse_properties_alignment", "Alignment" },
        { "ui_browse_properties_alignment_cover", "Cover" },
        { "ui_browse_properties_alignment_center", "Center" },
        { "ui_browse_properties_alignment_stretch", "Stretch" },
        { "ui_browse_properties_alignment_position", "Position" },
        { "ui_browse_properties_alignment_fill", "Fill" },
        { "ui_browse_properties_alignment_free", "Free" },
        { "ui_browse_properties_alignment_horizontal", "Horizontal" },
        { "ui_browse_properties_alignment_vertical", "Vertical" },
        { "ui_browse_properties_alignment_zoom", "Zoom" },
        { "ui_browse_properties_alignment_flip_horizontally", "Flip horizontally" },
        { "ui_browse_properties_mouse_parallax", "Mouse parallax" },
        { "ui_browse_properties_parallax", "Parallax" },
        { "ui_browse_properties_parallax_strength", "Parallax strength" },
        { "ui_browse_properties_brightness", "Brightness" },
        { "ui_browse_properties_saturation", "Saturation" },
        { "ui_browse_properties_hue_shift", "Hue shift" },
        { "ui_browse_properties_contrast", "Contrast" },
        { "ui_browse_properties_show_color_options", "Color options" },
        { "ui_browse_properties_pixel_art_optimization", "Pixel art optimization" },
        { "ui_browse_properties_image_filter", "Image filter" },
        { "ui_browse_properties_filter_strength", "Filter strength" },
        { "ui_browse_properties_image_filter_none", "None" },
        { "ui_browse_properties_accent_color", "Accent color" },
        { "ui_browse_properties_background_color", "Background color" },
        { "ui_browse_properties_count", "Count" },
        { "ui_browse_properties_glow", "Glow" },
        { "ui_browse_properties_style", "Style" },
        { "ui_browse_properties_large", "Large" },
        { "ui_browse_properties_small", "Small" },
    };
    for (guint i = 0; key && i < G_N_ELEMENTS (tab); i++)
        if (!g_ascii_strcasecmp (key, tab[i].k))
            return tab[i].v;
    return NULL;
}

static gboolean is_loc_key (const char *t)
{
    if (!t || strncmp (t, "ui_", 3))
        return FALSE;
    for (const char *p = t; *p; p++)
        if (!g_ascii_isalnum (*p) && *p != '_' && *p != '-' && *p != '.')
            return FALSE;
    return TRUE;
}

static const char *loc_lookup (Panel *P, const char *key)
{
    if (!P || !P->loc_lang || !key)
        return builtin_loc (key);
    const char *v = wpj_str (P->loc_lang, key, NULL);
    if (v)
        return v;
    GList *ks = json_object_get_members (P->loc_lang);
    for (GList *k = ks; k && !v; k = k->next)
        if (!g_ascii_strcasecmp (k->data, key))
            v = wpj_str (P->loc_lang, k->data, NULL);
    g_list_free (ks);
    return v ? v : builtin_loc (key);
}

/* the project's localization table that matches the user's language */
static JsonObject *pick_language (JsonObject *loc)
{
    if (!loc)
        return NULL;
    const char *const *langs = g_get_language_names ();
    for (int i = 0; langs[i]; i++) {
        if (!strcmp (langs[i], "C") || !strcmp (langs[i], "POSIX"))
            continue;
        char *l = g_ascii_strdown (langs[i], -1);
        char *dot = strchr (l, '.');
        if (dot)
            *dot = 0;
        char *at = strchr (l, '@');
        if (at)
            *at = 0;
        for (char *p = l; *p; p++)
            if (*p == '_')
                *p = '-';
        JsonObject *hit = NULL;
        GList *ks = json_object_get_members (loc);
        for (GList *k = ks; k && !hit; k = k->next)
            if (!g_ascii_strcasecmp (k->data, l))
                hit = wpj_obj (loc, k->data);
        if (!hit && !strchr (l, '-')) {
            gsize ll = strlen (l);
            for (GList *k = ks; k && !hit; k = k->next)
                if (!g_ascii_strncasecmp (k->data, l, ll) && ((char *) k->data)[ll] == '-')
                    hit = wpj_obj (loc, k->data);
        }
        g_list_free (ks);
        g_free (l);
        if (hit)
            return hit;
    }
    JsonObject *en = wpj_obj (loc, "en-us");
    if (en)
        return en;
    GList *ks = json_object_get_members (loc);
    JsonObject *first = ks ? wpj_obj (loc, ks->data) : NULL;
    g_list_free (ks);
    return first;
}

static gboolean tag_is (const char *name, gsize n, const char *what)
{
    return strlen (what) == n && !g_ascii_strncasecmp (name, what, n);
}

static void decode_entity (const char **pp, GString *out)
{
    const char *p = *pp;   /* at '&' */
    /* the ';' within ten characters, looked for there only (a search to
     * the end of the text for every '&' is quadratic in a text of them) */
    const char *semi = NULL;
    for (int k = 1; k <= 10 && p[k] && !semi; k++)
        if (p[k] == ';')
            semi = p + k;
    if (!semi) {
        g_string_append_c (out, '&');
        *pp = p + 1;
        return;
    }
    gsize n = (gsize) (semi - p - 1);
    const char *e = p + 1;
    gunichar u = 0;
    if (n >= 2 && e[0] == '#') {
        u = (e[1] == 'x' || e[1] == 'X') ? (gunichar) strtoul (e + 2, NULL, 16)
                                         : (gunichar) strtoul (e + 1, NULL, 10);
    } else if (n == 3 && !strncmp (e, "amp", 3)) u = '&';
    else if (n == 2 && !strncmp (e, "lt", 2)) u = '<';
    else if (n == 2 && !strncmp (e, "gt", 2)) u = '>';
    else if (n == 4 && !strncmp (e, "quot", 4)) u = '"';
    else if (n == 4 && !strncmp (e, "apos", 4)) u = '\'';
    else if (n == 4 && !strncmp (e, "nbsp", 4)) u = ' ';
    if (u && g_unichar_validate (u)) {
        g_string_append_unichar (out, u);
        *pp = semi + 1;
    } else {
        g_string_append_c (out, '&');
        *pp = p + 1;
    }
}

/* value of attribute `name` inside a tag's text [a, e) (quoted or bare);
 * g_free, NULL when absent */
static char *tag_attr (const char *a, const char *e, const char *name)
{
    gsize nl = strlen (name);
    for (const char *p = a; p + nl < e; p++) {
        if (g_ascii_strncasecmp (p, name, nl) || (p > a && (g_ascii_isalnum (p[-1]) || p[-1] == '-')))
            continue;
        const char *q = p + nl;
        while (q < e && g_ascii_isspace (*q))
            q++;
        if (q >= e || *q != '=')
            continue;
        q++;
        while (q < e && g_ascii_isspace (*q))
            q++;
        if (q < e && (*q == '"' || *q == '\'')) {
            char qc = *q++;
            const char *v = q;
            while (q < e && *q != qc)
                q++;
            return g_strndup (v, (gsize) (q - v));
        }
        const char *v = q;
        while (q < e && !g_ascii_isspace (*q) && *q != '>')
            q++;
        return g_strndup (v, (gsize) (q - v));
    }
    return NULL;
}

/* blank and filler characters authors pad labels with: ASCII space, NBSP,
 * zero-width characters, U+3164 HANGUL FILLER, U+2800 BRAILLE BLANK, the
 * ideographic space */
static gboolean is_filler (gunichar c)
{
    return c == ' ' || c == 0xa0 || c == 0x3164 || c == 0x2800 || c == 0x3000 || c == 0xfeff ||
           (c >= 0x200b && c <= 0x200f) || c == 0x115f || c == 0x1160 || c == 0xffa0;
}

/* trimmed of blanks/fillers at both ends; g_free */
static char *trim_fillers (const char *l)
{
    const char *a = l;
    while (*a && is_filler (g_utf8_get_char (a)))
        a = g_utf8_next_char (a);
    const char *e = a + strlen (a);
    while (e > a) {
        const char *pv = g_utf8_prev_char (e);
        if (!is_filler (g_utf8_get_char (pv)))
            break;
        e = pv;
    }
    return g_strndup (a, (gsize) (e - a));
}

/* HTML tags WE's option labels use (a cut-off one at the end of a label
 * is still a tag, "a<b" in running text is not) */
static gboolean known_tag (const char *nm, gsize nl)
{
    static const char *const tags[] = {
        "a", "b", "i", "u", "s", "p", "br", "hr", "img", "div", "span", "font", "big", "small",
        "center", "strong", "em", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "table",
        "tr", "td", "th", "sub", "sup", "code", "pre", "blockquote", "label", "mark", "abbr", NULL,
    };
    for (int i = 0; tags[i]; i++)
        if (tag_is (nm, nl, tags[i]))
            return TRUE;
    return FALSE;
}

static const char *const bb_tags[] = {
    "b", "i", "u", "s", "url", "color", "size", "h1", "h2", "h3", "img", "list", "*",
    "quote", "code", "spoiler", "center", "hr", "strike", "noparse", "table", "tr", "td", NULL,
};

/* strstr (from, what) that remembers a miss: what is not found from one
 * place is not found from any later place of the same text either */
static const char *find_later (const char *from, const char *what, gboolean *none)
{
    if (*none)
        return NULL;
    const char *hit = strstr (from, what);
    if (!hit)
        *none = TRUE;
    return hit;
}

/* HTML/BBCode -> plain lines; flags for heading/rule styling.
 * The text is a wallpaper's and as long as its author likes: every search
 * for the end of something (the '>' of a tag, a closing tag, the ']' of a
 * BBCode tag) is made so that the whole pass stays linear - a megabyte of
 * "&", "<x" or "[img]" used to be searched to its end once per character
 * and held the window for minutes. */
static void markup_to_lab (const char *t, Lab *out)
{
    GString *s = g_string_new (NULL);
    gboolean seen_text = FALSE, heading = FALSE, rule = FALSE, markup = FALSE;
    guint images = 0, links = 0;
    GString *notes = NULL;      /* <abbr title> help texts */
    char *owned = NULL;
    if (!g_utf8_validate (t, -1, NULL))
        t = owned = g_utf8_make_valid (t, -1);
    const char *p = t;
    const char *next_gt = NULL;     /* the first '>' at or after the last place asked from */
    gboolean no_gt = FALSE, no_abbr = FALSE, no_script = FALSE, no_style = FALSE, no_bbclose = FALSE;
    while (*p) {
        if (*p == '<') {
            const char *q = p + 1;
            gboolean closing = *q == '/';
            if (closing) {
                q++;
                while (*q == ' ')   /* "</ h4>" */
                    q++;
            }
            const char *nm = q;
            while (g_ascii_isalnum (*q))
                q++;
            gsize nl = (gsize) (q - nm);
            if (!no_gt && (!next_gt || next_gt < q)) {
                next_gt = strchr (q, '>');
                no_gt = next_gt == NULL;
            }
            const char *gt = no_gt ? NULL : next_gt;
            if (!nl && !closing && q[0] == '!' && q[1] == '-' && q[2] == '-') {
                const char *ce = strstr (q, "-->");   /* <!-- comment --> */
                p = ce ? ce + 3 : p + strlen (p);
                continue;
            }
            if (nl && !gt && known_tag (nm, nl)) {
                /* a tag cut off at the end of the text ("... <br", "<img
                 * src=http://..."): never shown raw */
                gt = p + strlen (p) - 1;
            }
            if (!nl || !gt) {           /* a lone '<' in text */
                g_string_append_c (s, '<');
                p++;
                continue;
            }
            markup = TRUE;
            if (!closing && tag_is (nm, nl, "img")) {
                /* a picture: its alt text when it has one, else counted */
                char *alt = tag_attr (q, gt, "alt");
                char *at = alt ? trim_fillers (alt) : NULL;
                if (at && *at) {
                    g_string_append (s, at);
                    seen_text = TRUE;
                } else {
                    images++;
                }
                g_free (at);
                g_free (alt);
            }
            if (!closing && tag_is (nm, nl, "a"))
                links++;
            if (tag_is (nm, nl, "abbr")) {
                /* <abbr title="help">[info]</abbr>: the help text becomes
                 * a line of its own, the marker goes */
                if (!closing) {
                    char *ti = tag_attr (q, gt, "title");
                    if (ti && *ti) {
                        if (!notes)
                            notes = g_string_new (NULL);
                        g_string_append_printf (notes, "\n%s", ti);
                        const char *ce = find_later (gt, "</abbr", &no_abbr);
                        if (ce) {
                            const char *cg = strchr (ce, '>');
                            g_free (ti);
                            p = cg ? cg + 1 : ce + strlen (ce);
                            continue;
                        }
                    }
                    g_free (ti);
                }
            }
            if (!closing && tag_is (nm, nl, "li"))
                g_string_append (s, "\n\342\200\242 ");
            if (tag_is (nm, nl, "script") || tag_is (nm, nl, "style")) {
                /* never text: skip to the closing tag */
                const char *ce = closing ? NULL
                               : tag_is (nm, nl, "script") ? find_later (gt, "</script", &no_script)
                                                           : find_later (gt, "</style", &no_style);
                if (ce) {
                    const char *cg = strchr (ce, '>');
                    p = cg ? cg + 1 : ce + strlen (ce);
                    continue;
                }
            }
            if (tag_is (nm, nl, "br") || tag_is (nm, nl, "p") || tag_is (nm, nl, "div") ||
                tag_is (nm, nl, "tr") || tag_is (nm, nl, "center") || tag_is (nm, nl, "ul") ||
                tag_is (nm, nl, "ol") || tag_is (nm, nl, "table") || tag_is (nm, nl, "blockquote") ||
                (nl == 2 && g_ascii_tolower (nm[0]) == 'h' && g_ascii_isdigit (nm[1])))
                g_string_append_c (s, '\n');
            if (tag_is (nm, nl, "hr")) {
                g_string_append_c (s, '\n');
                if (!seen_text)
                    rule = TRUE;
            }
            if (!closing && !seen_text &&
                ((nl == 2 && g_ascii_tolower (nm[0]) == 'h' && g_ascii_isdigit (nm[1])) ||
                 tag_is (nm, nl, "big") || tag_is (nm, nl, "b") || tag_is (nm, nl, "strong")))
                heading = TRUE;
            p = gt + 1;
            continue;
        }
        if (*p == '[') {
            const char *q = p + 1;
            if (*q == '/')
                q++;
            const char *nm = q;
            while (g_ascii_isalnum (*q) || *q == '*')
                q++;
            gsize nl = (gsize) (q - nm);
            /* the ']' of a tag is within 200 characters of its '[' */
            const char *rb = NULL;
            if (nl && (*q == ']' || *q == '='))
                for (const char *c = q; *c && c - p < 200 && !rb; c++)
                    if (*c == ']')
                        rb = c;
            gboolean bb = FALSE;
            if (rb)
                for (int i = 0; bb_tags[i] && !bb; i++)
                    bb = tag_is (nm, nl, bb_tags[i]);
            if (bb) {
                markup = TRUE;
                if (tag_is (nm, nl, "img") && p[1] != '/') {
                    /* [img]url[/img]: the URL is not a label */
                    const char *ce = find_later (rb, "[/", &no_bbclose);
                    images++;
                    if (ce) {
                        const char *cg = strchr (ce, ']');
                        p = cg ? cg + 1 : ce + strlen (ce);
                        continue;
                    }
                }
                if (tag_is (nm, nl, "url") && p[1] != '/')
                    links++;
                if (tag_is (nm, nl, "hr") || tag_is (nm, nl, "*") || tag_is (nm, nl, "h1") ||
                    tag_is (nm, nl, "h2") || tag_is (nm, nl, "h3"))
                    g_string_append_c (s, '\n');
                if (tag_is (nm, nl, "hr") && !seen_text)
                    rule = TRUE;
                if (!seen_text && (tag_is (nm, nl, "b") || tag_is (nm, nl, "h1") ||
                                   tag_is (nm, nl, "h2") || tag_is (nm, nl, "h3")))
                    heading = TRUE;
                p = rb + 1;
                continue;
            }
        }
        if (*p == '&') {
            decode_entity (&p, s);
            seen_text = TRUE;
            continue;
        }
        if (*p == '\r' || *p == '\t') {
            g_string_append_c (s, *p == '\t' ? ' ' : '\n');
            p++;
            continue;
        }
        if (!g_ascii_isspace (*p))
            seen_text = TRUE;
        g_string_append_c (s, *p);
        p++;
    }
    if (notes) {
        g_string_append (s, notes->str);
        g_string_free (notes, TRUE);
    }
    /* lines: trimmed, whitespace collapsed, empties dropped */
    char **lines = g_strsplit (s->str, "\n", -1);
    g_string_free (s, TRUE);
    GPtrArray *keep = g_ptr_array_new_with_free_func (g_free);
    for (int i = 0; lines[i]; i++) {
        char *lt = trim_fillers (g_strstrip (lines[i]));
        g_free (lines[i]);
        lines[i] = lt;
        char *l = g_strstrip (lines[i]);
        if (!*l)
            continue;
        GString *c = g_string_new (NULL);
        for (const char *q = l; *q; q++)
            if (!(g_ascii_isspace (*q) && c->len && g_ascii_isspace (c->str[c->len - 1])))
                g_string_append_c (c, g_ascii_isspace (*q) ? ' ' : *q);
        g_ptr_array_add (keep, g_string_free (c, FALSE));
    }
    g_strfreev (lines);
    g_free (owned);
    out->heading = heading;
    out->rule = rule;
    out->markup = markup;
    out->images = images;
    out->links = links;
    out->title = keep->len ? g_strdup (keep->pdata[0]) : NULL;
    if (keep->len > 1) {
        GString *sub = g_string_new (NULL);
        for (guint i = 1; i < keep->len; i++) {
            if (sub->len)
                g_string_append_c (sub, '\n');
            g_string_append (sub, keep->pdata[i]);
        }
        out->sub = g_string_free (sub, FALSE);
    }
    g_ptr_array_free (keep, TRUE);
    if (out->title && !g_utf8_validate (out->title, -1, NULL)) {
        char *v = g_utf8_make_valid (out->title, -1);
        g_free (out->title);
        out->title = v;
    }
    if (out->sub && !g_utf8_validate (out->sub, -1, NULL)) {
        char *v = g_utf8_make_valid (out->sub, -1);
        g_free (out->sub);
        out->sub = v;
    }
}

/* raw WE text -> Lab. key_fallback: used when there is no text at all */
static void resolve_label (Panel *P, const char *raw, const char *key_fallback, Lab *out)
{
    memset (out, 0, sizeof (*out));
    if (!raw || !*raw) {
        if (key_fallback)
            out->title = humanize (key_fallback);
        return;
    }
    char *tmp = NULL;
    if (is_loc_key (raw)) {
        const char *l = loc_lookup (P, raw);
        if (l && *l)
            raw = l;
        else
            raw = tmp = humanize (raw);
    }
    markup_to_lab (raw, out);
    g_free (tmp);
}

static gboolean is_control_type (const char *type)
{
    static const char *const t[] = { "bool", "checkbox", "slider", "combo", "color", "textinput",
                                     "file", "directory", "scenetexture", NULL };
    for (int i = 0; type && t[i]; i++)
        if (!strcmp (type, t[i]))
            return TRUE;
    return FALSE;
}

/* the label of a property row. A control always gets a readable title:
 * its text without markup; "(image)" / "(link)" when the text is only a
 * picture or a link (authors decorate the list with <img> rows that are
 * technically checkboxes; the key WE derives from such text is the tag
 * soup "imgsrchttp..."); the key in words when there is no text at all. */
static void row_label (Panel *P, const char *key, JsonObject *def, Lab *lab, gboolean *is_ctl)
{
    const char *type = wpj_str (def, "type", "");
    /* "group", "text" and typeless entries are headers / notes whatever
     * their "value" says (WE writes "value": false on text rows) */
    gboolean ctl = (json_object_has_member (def, "value") || is_control_type (type)) &&
                   *type && strcmp (type, "group") && strcmp (type, "text");
    resolve_label (P, wpj_str (def, "text", NULL), ctl ? key : NULL, lab);
    if (!lab->title && ctl)
        lab->title = lab->images ? g_strdup ("(image)")
                   : lab->links ? g_strdup ("(link)")
                   : lab->markup ? g_strdup ("(no label)") : humanize (key);
    if (ctl && !lab->sub && !g_strcmp0 (key, "schemecolor") && !g_strcmp0 (type, "color"))
        lab->sub = g_strdup ("Wallpaper Engine's accent colour for its own interface and RGB "
                             "hardware. Few wallpapers read it.");
    if (ctl && !lab->sub && !g_strcmp0 (key, "walpe_sound"))
        lab->sub = g_strdup ("As Engine settings say, always on, or always off.");
    /* an option the scene cannot react to says so (propnotes.h) instead of
     * silently doing nothing */
    const char *note = ctl && P && P->notes ? wpj_str (P->notes, key, NULL) : NULL;
    lab->inert = note != NULL;
    if (note && g_strcmp0 (key, "schemecolor")) {
        char *nt = walpe_prop_note_text (note);
        char *sub = lab->sub ? g_strconcat (lab->sub, "\n", nt, NULL) : g_strdup (nt);
        g_free (lab->sub);
        lab->sub = sub;
        g_free (nt);
    }
    if (is_ctl)
        *is_ctl = ctl;
}

gboolean walpe_props_condition (const char *expr, JsonObject *values)
{
    return cond_visible (expr, values);
}

char *walpe_props_label_preview (JsonObject *def, const char *key, JsonObject *loc)
{
    Panel tmp = { 0 };
    tmp.loc_lang = pick_language (loc);
    Lab lab;
    gboolean ctl = FALSE;
    row_label (&tmp, key, def, &lab, &ctl);
    char *out = g_strdup_printf ("%s%s%s%s", ctl ? "" : "# ", lab.title ? lab.title : "",
                                 lab.sub ? "\n" : "", lab.sub ? lab.sub : "");
    lab_clear (&lab);
    return out;
}

/* ============================================================ sending */

static void set_hint (Panel *P, const char *msg, gboolean warn)
{
    gtk_label_set_text (GTK_LABEL (P->hint), msg ? msg : "");
    if (warn)
        gtk_widget_add_css_class (P->hint, "warning");
    else
        gtk_widget_remove_css_class (P->hint, "warning");
}

static void update_hint (Panel *P)
{
    if (!P->rows->len)
        set_hint (P, "", FALSE);
    else if (P->is_current)
        set_hint (P, "Active wallpaper: changes apply live and are saved for this wallpaper.",
                  FALSE);
    else if (P->by_path)
        set_hint (P, "Not the active wallpaper: changes are saved for it and show when you "
                     "set it.", FALSE);
    else
        set_hint (P, "Not the active wallpaper: changes are kept and saved when you set it.",
                  FALSE);
}

typedef struct {
    Panel *P;
    guint gen;
    guint n;
    char *what;
} SendCtx;

static void on_sent (JsonObject *reply, const char *err, gpointer user)
{
    SendCtx *c = user;
    Panel *P = c->P;
    if (ui_ipc_ok (reply)) {
        char *m = g_strdup_printf ("%s", c->what);
        P->status (m, FALSE);
        g_free (m);
        if (c->gen == P->gen)
            update_hint (P);
    } else {
        const char *e = reply ? wpj_str (reply, "error", "rejected") : err;
        char *m = g_strdup_printf ("Wallpaper option not saved: %s", e ? e : "error");
        P->status (m, TRUE);
        if (c->gen == P->gen)
            set_hint (P, m, TRUE);
        g_free (m);
    }
    g_free (c->what);
    g_free (c);
}

static void send_props (Panel *P, const char *path, JsonObject *vals, gboolean reset_first,
                        gboolean with_path)
{
    if (reset_first) {
        JsonBuilder *b = json_builder_new ();
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "cmd");
        json_builder_add_string_value (b, "resetprops");
        if (with_path) {
            json_builder_set_member_name (b, "path");
            json_builder_add_string_value (b, path);
        }
        json_builder_end_object (b);
        SendCtx *c = g_new0 (SendCtx, 1);
        c->P = P;
        c->gen = P->gen;
        c->what = g_strdup ("Wallpaper options reset to the defaults.");
        ui_ipc_send_node (json_builder_get_root (b), UI_IPC_SLOW, on_sent, c);
        g_object_unref (b);
    }
    guint n = vals ? json_object_get_size (vals) : 0;
    if (!n)
        return;
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    json_builder_add_string_value (b, "setprops");
    if (with_path) {
        json_builder_set_member_name (b, "path");
        json_builder_add_string_value (b, path);
    }
    json_builder_set_member_name (b, "props");
    json_builder_add_value (b, json_node_init_object (json_node_alloc (), vals));
    json_builder_end_object (b);
    SendCtx *c = g_new0 (SendCtx, 1);
    c->P = P;
    c->gen = P->gen;
    c->n = n;
    c->what = n == 1 ? g_strdup ("Wallpaper option saved.")
                     : g_strdup_printf ("%u wallpaper options saved.", n);
    ui_ipc_send_node (json_builder_get_root (b), UI_IPC_SLOW, on_sent, c);
    g_object_unref (b);
}

static Staged *staged_for (Panel *P, const char *path)
{
    Staged *s = g_hash_table_lookup (P->staged, path);
    if (!s) {
        s = g_new0 (Staged, 1);
        s->vals = json_object_new ();
        g_hash_table_insert (P->staged, g_strdup (path), s);
    }
    return s;
}

static gboolean flush_dirty (gpointer user)
{
    Panel *P = user;
    P->debounce_id = 0;
    if (!P->path || !json_object_get_size (P->dirty))
        return G_SOURCE_REMOVE;
    JsonObject *vals = P->dirty;
    P->dirty = json_object_new ();
    if (P->by_path || P->is_current) {
        send_props (P, P->path, vals, FALSE, P->by_path);
    } else {
        Staged *s = staged_for (P, P->path);
        GList *ks = json_object_get_members (vals);
        for (GList *k = ks; k; k = k->next)
            json_object_set_member (s->vals, k->data,
                                    json_node_copy (json_object_get_member (vals, k->data)));
        g_list_free (ks);
        P->status ("Wallpaper option kept: it is saved when you set this wallpaper.", FALSE);
    }
    json_object_unref (vals);
    return G_SOURCE_REMOVE;
}

static void refresh_visibility (Panel *P);

static void row_mark (PRow *r, JsonNode *cur)
{
    if (!r->reset)
        return;
    gboolean mod = r->dflt && !values_equal (r, cur, r->dflt);
    gtk_widget_set_opacity (r->reset, mod ? 1.0 : 0.0);
    gtk_widget_set_sensitive (r->reset, mod);
    gtk_widget_set_can_target (r->reset, mod);
    gtk_widget_set_can_focus (r->reset, mod);
    if (mod)
        gtk_widget_add_css_class (r->title_lbl, "walpe-prop-modified");
    else
        gtk_widget_remove_css_class (r->title_lbl, "walpe-prop-modified");
}

/* a control changed: val is consumed */
static void changed (Panel *P, PRow *r, JsonNode *val)
{
    if (P->building || !r) {
        json_node_unref (val);
        return;
    }
    json_object_set_member (P->live, r->key, json_node_copy (val));
    json_object_set_member (P->dirty, r->key, val);
    row_mark (r, val);
    refresh_visibility (P);
    if (P->debounce_id)
        g_source_remove (P->debounce_id);
    P->debounce_id = g_timeout_add (DEBOUNCE_MS, flush_dirty, P);
}

/* ============================================================ widgets */

static PRow *row_of (gpointer w) { return g_object_get_data (G_OBJECT (w), "walpe-prow"); }

static void on_switch (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) ps;
    changed (user, row_of (o), mk_bool (gtk_switch_get_active (GTK_SWITCH (o))));
}

static void on_adj (GtkAdjustment *a, gpointer user)
{
    PRow *r = row_of (a);
    if (r)
        changed (user, r, mk_num (gtk_adjustment_get_value (a), r->is_int));
}

static void on_dropdown (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) ps;
    PRow *r = row_of (o);
    if (!r || !r->combo_values)
        return;
    guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (o));
    JsonArray *vals = json_node_get_array (r->combo_values);
    if (sel >= json_array_get_length (vals))
        return;
    changed (user, r, json_node_copy (json_array_get_element (vals, sel)));
}

static void on_color (GObject *o, GParamSpec *ps, gpointer user)
{
    (void) ps;
    PRow *r = row_of (o);
    const GdkRGBA *c = gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (o));
    if (!r || !c)
        return;
    char *s = format_color (c, r->hex_color);
    changed (user, r, mk_str (s));
    g_free (s);
}

static void entry_commit (GtkWidget *e, Panel *P)
{
    PRow *r = row_of (e);
    if (!r)
        return;
    const char *t = gtk_editable_get_text (GTK_EDITABLE (e));
    JsonNode *cur = json_object_get_member (P->live, r->key);
    char *cs = node_str (cur);
    gboolean same = !g_strcmp0 (cs, t);
    g_free (cs);
    if (!same)
        changed (P, r, mk_str (t));
}

static void on_entry_activate (GtkEntry *e, gpointer user) { entry_commit (GTK_WIDGET (e), user); }

static void on_entry_leave (GtkEventControllerFocus *fc, gpointer user)
{
    entry_commit (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (fc)), user);
}

/* ---- file / directory / scenetexture options: a GtkFileDialog. The value
 * is the absolute path (what the engine and web pages expect), "" = unset */

static void file_row_show (PRow *r, const char *path)
{
    if (!r->file_lbl)
        return;
    if (path && *path) {
        char *bn = g_path_get_basename (path);
        char *disp = g_filename_display_name (bn);
        gtk_label_set_text (GTK_LABEL (r->file_lbl), disp);
        g_free (disp);
        g_free (bn);
        char *full = g_filename_display_name (path);
        gtk_widget_set_tooltip_text (r->ctl, full);
        g_free (full);
    } else {
        gtk_label_set_text (GTK_LABEL (r->file_lbl), r->is_dir ? "Choose a folder…" : "Choose a file…");
        gtk_widget_set_tooltip_text (r->ctl, r->is_dir ? "No folder chosen" : "No file chosen");
    }
}

typedef struct {
    Panel *P;
    GtkWidget *box;   /* ref: keeps the panel alive while the dialog is open */
    guint gen;
    char *key;
    gboolean dir;
} FileCtx;

static void on_file_chosen (GObject *src, GAsyncResult *res, gpointer user)
{
    FileCtx *c = user;
    GFile *f = c->dir ? gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (src), res, NULL)
                      : gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, NULL);
    char *path = f ? g_file_get_path (f) : NULL;
    /* the list may have been rebuilt for another wallpaper meanwhile */
    PRow *r = path && c->gen == c->P->gen ? g_hash_table_lookup (c->P->by_key, c->key) : NULL;
    if (r && r->kind == K_FILE) {
        file_row_show (r, path);
        changed (c->P, r, mk_str (path));
    }
    g_free (path);
    g_clear_object (&f);
    g_object_unref (c->box);
    g_free (c->key);
    g_free (c);
}

static void on_file_clicked (GtkButton *b, gpointer user)
{
    Panel *P = user;
    PRow *r = row_of (b);
    if (!r)
        return;
    GtkFileDialog *dlg = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dlg, r->title);
    gtk_file_dialog_set_modal (dlg, TRUE);
    JsonNode *cur = json_object_get_member (P->live, r->key);
    char *cs = node_str (cur);
    if (*cs && g_path_is_absolute (cs)) {
        GFile *cf = g_file_new_for_path (cs);
        if (r->is_dir)
            gtk_file_dialog_set_initial_folder (dlg, cf);
        else
            gtk_file_dialog_set_initial_file (dlg, cf);
        g_object_unref (cf);
    }
    g_free (cs);
    if (!r->is_dir) {
        GListStore *fl = g_list_store_new (GTK_TYPE_FILE_FILTER);
        GtkFileFilter *img = gtk_file_filter_new ();
        gtk_file_filter_set_name (img, "Pictures");
        gtk_file_filter_add_mime_type (img, "image/*");
        GtkFileFilter *vid = gtk_file_filter_new ();
        gtk_file_filter_set_name (vid, "Videos and audio");
        gtk_file_filter_add_mime_type (vid, "video/*");
        gtk_file_filter_add_mime_type (vid, "audio/*");
        GtkFileFilter *all = gtk_file_filter_new ();
        gtk_file_filter_set_name (all, "All files");
        gtk_file_filter_add_pattern (all, "*");
        g_list_store_append (fl, r->want_video ? vid : img);
        g_list_store_append (fl, r->want_video ? img : vid);
        g_list_store_append (fl, all);
        gtk_file_dialog_set_filters (dlg, G_LIST_MODEL (fl));
        g_object_unref (fl);
        g_object_unref (img);
        g_object_unref (vid);
        g_object_unref (all);
    }
    FileCtx *c = g_new0 (FileCtx, 1);
    c->P = P;
    c->box = g_object_ref (P->box);
    c->gen = P->gen;
    c->key = g_strdup (r->key);
    c->dir = r->is_dir;
    GtkRoot *root = gtk_widget_get_root (P->box);
    GtkWindow *win = GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL;
    if (r->is_dir)
        gtk_file_dialog_select_folder (dlg, win, NULL, on_file_chosen, c);
    else
        gtk_file_dialog_open (dlg, win, NULL, on_file_chosen, c);
    g_object_unref (dlg);
}

/* set a control to v without emitting a change */
static void row_set_widget (Panel *P, PRow *r, JsonNode *v)
{
    gboolean was = P->building;
    P->building = TRUE;
    switch (r->kind) {
    case K_BOOL:
        gtk_switch_set_active (GTK_SWITCH (r->ctl), node_bool (v, FALSE));
        break;
    case K_SLIDER:
        gtk_adjustment_set_value (r->adj, node_num (v, gtk_adjustment_get_lower (r->adj)));
        break;
    case K_COMBO: {
        JsonArray *vals = json_node_get_array (r->combo_values);
        for (guint i = 0; i < json_array_get_length (vals); i++)
            if (values_equal (r, json_array_get_element (vals, i), v)) {
                gtk_drop_down_set_selected (GTK_DROP_DOWN (r->ctl), i);
                break;
            }
        break;
    }
    case K_COLOR: {
        GdkRGBA c;
        parse_color (v, &c);
        gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (r->ctl), &c);
        break;
    }
    case K_TEXT: {
        char *s = node_str (v);
        gtk_editable_set_text (GTK_EDITABLE (r->ctl), s);
        g_free (s);
        break;
    }
    case K_FILE: {
        char *s = node_str (v);
        file_row_show (r, s);
        g_free (s);
        break;
    }
    default:
        break;
    }
    P->building = was;
}

static void on_reset_one (GtkButton *b, gpointer user)
{
    Panel *P = user;
    PRow *r = row_of (b);
    if (!r || !r->dflt)
        return;
    row_set_widget (P, r, r->dflt);
    changed (P, r, json_node_copy (r->dflt));
}

static void on_reset_all (GtkButton *b, gpointer user)
{
    (void) b;
    Panel *P = user;
    if (!P->path)
        return;
    if (P->debounce_id) {
        g_source_remove (P->debounce_id);
        P->debounce_id = 0;
    }
    json_object_unref (P->dirty);
    P->dirty = json_object_new ();
    for (guint i = 0; i < P->rows->len; i++) {
        PRow *r = P->rows->pdata[i];
        if (!r->dflt || r->kind >= K_SECTION)
            continue;
        row_set_widget (P, r, r->dflt);
        json_object_set_member (P->live, r->key, json_node_copy (r->dflt));
        row_mark (r, r->dflt);
    }
    refresh_visibility (P);
    if (P->is_current || P->by_path) {
        g_hash_table_remove (P->staged, P->path);
        send_props (P, P->path, NULL, TRUE, P->by_path);
    } else {
        Staged *s = staged_for (P, P->path);
        json_object_unref (s->vals);
        s->vals = json_object_new ();
        s->reset = TRUE;
        P->status ("Options reset: saved when you set this wallpaper.", FALSE);
    }
}

static void on_row_activated (GtkListBox *lb, GtkListBoxRow *row, gpointer user)
{
    (void) lb; (void) user;
    PRow *r = row_of (row);
    if (!r || !r->ctl || !gtk_widget_get_sensitive (r->ctl))
        return;
    if (r->kind == K_BOOL)
        gtk_switch_set_active (GTK_SWITCH (r->ctl), !gtk_switch_get_active (GTK_SWITCH (r->ctl)));
    else if (r->kind == K_COMBO || r->kind == K_COLOR || r->kind == K_FILE)
        gtk_widget_activate (r->ctl);
    else
        gtk_widget_grab_focus (r->ctl);
}

/* ========================================================= visibility */

static gboolean filter_func (GtkListBoxRow *row, gpointer user)
{
    (void) user;
    PRow *r = row_of (row);
    return !r || (r->cond_ok && r->filter_ok);
}

static void refresh_visibility (Panel *P)
{
    const char *f = P->filter_text;
    guint shown = 0, total = 0;
    for (guint i = 0; i < P->rows->len; i++) {
        PRow *r = P->rows->pdata[i];
        r->cond_ok = cond_visible (r->condition, P->live);
        /* while filtering, headers/notes step aside: only matching options */
        r->filter_ok = !f || !*f ? TRUE
                     : r->kind >= K_SECTION ? FALSE
                     : strstr (r->search, f) != NULL;
        if (r->kind < K_SECTION) {
            total++;
            if (r->cond_ok && r->filter_ok)
                shown++;
        }
    }
    gtk_list_box_invalidate_filter (GTK_LIST_BOX (P->list));
    char *c = total == 0 ? g_strdup ("")
            : shown == total ? g_strdup_printf ("%u", total)
                             : g_strdup_printf ("%u of %u shown", shown, total);
    gtk_label_set_text (GTK_LABEL (P->count), c);
    g_free (c);
}

static void on_filter (GtkSearchEntry *e, gpointer user)
{
    Panel *P = user;
    g_free (P->filter_text);
    const char *t = gtk_editable_get_text (GTK_EDITABLE (e));
    char *n = g_utf8_normalize (t, -1, G_NORMALIZE_ALL);
    P->filter_text = *t ? g_utf8_casefold (n ? n : t, -1) : NULL;
    g_free (n);
    refresh_visibility (P);
}

/* ======================================================= config cache */

/* overrides for path as saved in config.json (the daemon writes it on
 * every change; read on selection, cached by mtime: no IPC) */
static JsonObject *saved_overrides (const char *path)
{
    static JsonNode *cfg_root;
    static gint64 cfg_mtime = -1;
    static gint64 cfg_size = -1;
    char *f = g_build_filename (g_get_user_config_dir (), "walpe", "config.json", NULL);
    struct stat st;
    if (stat (f, &st) == 0) {
        gint64 mt = (gint64) st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
        if (mt != cfg_mtime || st.st_size != cfg_size) {
            if (cfg_root)
                json_node_unref (cfg_root);
            cfg_root = wpj_parse_file (f);
            cfg_mtime = mt;
            cfg_size = st.st_size;
        }
    }
    g_free (f);
    JsonObject *out = json_object_new ();
    JsonObject *props = cfg_root && JSON_NODE_HOLDS_OBJECT (cfg_root)
                            ? wpj_obj (json_node_get_object (cfg_root), "props") : NULL;
    if (props && path) {
        char *prefix = g_strconcat (path, "\x1f", NULL);
        gsize pl = strlen (prefix);
        GList *ks = json_object_get_members (props);
        for (GList *k = ks; k; k = k->next) {
            if (strncmp (k->data, prefix, pl))
                continue;
            JsonNode *v = json_object_get_member (props, k->data);
            if (node_is_scalar (v))
                json_object_set_member (out, (char *) k->data + pl, json_node_copy (v));
        }
        g_list_free (ks);
        g_free (prefix);
    }
    return out;
}

/* ============================================================ building */

typedef struct { const char *key; JsonObject *def; double order; guint idx; } Ord;

static int ord_cmp (gconstpointer a, gconstpointer b)
{
    const Ord *x = a, *y = b;
    if (x->order != y->order)
        return x->order < y->order ? -1 : 1;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

/* Option texts come from the wallpaper's project.json. GTK measures a
 * wrapping label down to a few characters per line and lays the whole text
 * out again for every measure, several times per frame: one option titled
 * with 4000 characters froze the window for half a minute, sixty of them
 * for good. No label takes more than this many characters (the rest is
 * "…"); titles are limited to a few lines as well (build_row). */
#define LABEL_MAX_CHARS 240    /* titles, short notes */
#define COMBO_MAX_CHARS 60     /* drop-down entries */
#define NOTE_MAX_CHARS 4000    /* the folded-away long notes */
#define TIP_MAX_CHARS 1000     /* tooltips showing a cut text in full */

/* text cut to max characters + "…" (g_free), or NULL when it fits */
static char *text_cut (const char *text, glong max)
{
    if (!text || g_utf8_strlen (text, -1) <= max)
        return NULL;
    char *head = g_utf8_substring (text, 0, max);
    char *out = g_strconcat (head, "…", NULL);
    g_free (head);
    return out;
}

static GtkWidget *mk_label_max (const char *text, const char *css, gboolean wrap, glong max)
{
    char *cut = text_cut (text, max);
    GtkWidget *l = gtk_label_new (cut ? cut : text);
    g_free (cut);
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    if (wrap) {
        gtk_label_set_wrap (GTK_LABEL (l), TRUE);
        gtk_label_set_wrap_mode (GTK_LABEL (l), PANGO_WRAP_WORD_CHAR);
    }
    if (css)
        gtk_widget_add_css_class (l, css);
    return l;
}

static GtkWidget *mk_label (const char *text, const char *css, gboolean wrap)
{
    return mk_label_max (text, css, wrap, LABEL_MAX_CHARS);
}

/* a title: at most `lines` lines, the rest ellipsized */
static void label_limit_lines (GtkWidget *l, int lines)
{
    gtk_label_set_lines (GTK_LABEL (l), lines);
    gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
}

/* tooltip for a label whose text may have been cut: the text (bounded
 * too) and, for option rows, the key below it */
static void set_tip (GtkWidget *w, const char *text, const char *key)
{
    char *cut = text_cut (text, TIP_MAX_CHARS);
    const char *t = cut ? cut : text;
    char *tip = t && key ? g_strconcat (t, "\n", key, NULL) : g_strdup (t ? t : key);
    gtk_widget_set_tooltip_text (w, tip);
    g_free (tip);
    g_free (cut);
}

static guint count_lines (const char *s)
{
    guint n = 1;
    for (; s && *s; s++)
        n += *s == '\n';
    return n;
}

static GtkWidget *selectable_note (const char *text)
{
    GtkWidget *body = mk_label_max (text, "walpe-prop-note", TRUE, NOTE_MAX_CHARS);
    gtk_label_set_selectable (GTK_LABEL (body), TRUE);
    gtk_widget_set_focusable (body, FALSE);
    gtk_widget_set_margin_start (body, 18);
    gtk_widget_set_margin_top (body, 4);
    return body;
}

static int digits_for (double step, int precision)
{
    if (precision >= 0)
        return CLAMP (precision, 0, 6);
    if (step >= 1)
        return 0;
    int d = 0;
    while (d < 4 && fabs (step * pow (10, d) - round (step * pow (10, d))) > 1e-9)
        d++;
    return MAX (d, 1);
}

static PRow *build_row (Panel *P, const char *key, JsonObject *def, JsonNode *cur)
{
    const char *type = wpj_str (def, "type", "");
    gboolean has_value = json_object_has_member (def, "value");
    Lab lab;
    /* a property without "text" (or with only markup) is still a control
     * and gets a readable label (row_label) */
    row_label (P, key, def, &lab, NULL);
    gboolean decor = !lab.title;   /* markup only (images, links) */

    PRow *r = g_new0 (PRow, 1);
    r->key = g_strdup (key);
    r->type = g_strdup (type);
    const char *cond = wpj_str (def, "condition", NULL);
    r->condition = cond && *cond ? g_strdup (cond) : NULL;
    JsonNode *dv = json_object_get_member (def, "value");
    r->dflt = node_is_scalar (dv) ? json_node_copy (dv) : NULL;
    gboolean is_file = !g_strcmp0 (type, "file") || !g_strcmp0 (type, "directory") ||
                       !g_strcmp0 (type, "scenetexture");
    if (is_file && !r->dflt)
        r->dflt = mk_str ("");   /* "file" options often have no "value": unset */

    /* "text", or no type at all (WE shows those as plain labels too; a
     * "value" on them means nothing: 3122339805's headers carry false and
     * were shown as text fields reading "false") */
    (void) has_value;
    if (!g_strcmp0 (type, "group") || !g_strcmp0 (type, "text") || !*type) {
        if (decor) {
            prow_free (r);
            lab_clear (&lab);
            return NULL;
        }
        gboolean short_line = !lab.sub && g_utf8_strlen (lab.title, -1) <= 48;
        r->kind = (!g_strcmp0 (type, "group") || lab.heading || short_line) ? K_SECTION : K_NOTE;
    } else if (!g_strcmp0 (type, "bool") || !g_strcmp0 (type, "checkbox")) {
        r->kind = K_BOOL;
    } else if (!g_strcmp0 (type, "slider")) {
        r->kind = K_SLIDER;
    } else if (!g_strcmp0 (type, "combo")) {
        r->kind = K_COMBO;
    } else if (!g_strcmp0 (type, "color")) {
        r->kind = K_COLOR;
    } else if (!g_strcmp0 (type, "textinput")) {
        r->kind = K_TEXT;
    } else if (is_file) {
        r->kind = K_FILE;       /* file / directory / scenetexture: a picker */
        r->is_dir = !g_strcmp0 (type, "directory");
        r->want_video = !g_ascii_strcasecmp (wpj_str (def, "fileType", ""), "video");
    } else {
        r->kind = K_READONLY;   /* unknown type */
    }
    if (!lab.title)
        lab.title = humanize (key);
    r->title = g_strdup (lab.title);
    {
        char *s = g_strdup_printf ("%s %s %s", lab.title, lab.sub ? lab.sub : "", key);
        char *n = g_utf8_normalize (s, -1, G_NORMALIZE_ALL);
        r->search = g_utf8_casefold (n ? n : s, -1);
        g_free (n);
        g_free (s);
    }

    GtkWidget *row = gtk_list_box_row_new ();
    r->row = row;
    g_object_set_data (G_OBJECT (row), "walpe-prow", r);

    if (r->kind == K_SECTION || r->kind == K_NOTE) {
        gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
        gtk_widget_set_focusable (row, FALSE);
        GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
        if (r->kind == K_SECTION) {
            gtk_widget_add_css_class (row, "walpe-section");
            if (lab.rule)
                gtk_widget_add_css_class (row, "walpe-rule");
            r->title_lbl = mk_label (lab.title, "walpe-section-title", TRUE);
            label_limit_lines (r->title_lbl, 3);
            gtk_box_append (GTK_BOX (v), r->title_lbl);
            if (lab.sub && (g_utf8_strlen (lab.sub, -1) > 160 || count_lines (lab.sub) > 2)) {
                /* long blurbs (index lists, FAQ links) fold away */
                GtkWidget *ex = gtk_expander_new ("Details");
                gtk_widget_add_css_class (ex, "walpe-prop-sub");
                gtk_expander_set_child (GTK_EXPANDER (ex), selectable_note (lab.sub));
                gtk_box_append (GTK_BOX (v), ex);
            } else if (lab.sub) {
                gtk_box_append (GTK_BOX (v), mk_label (lab.sub, "walpe-prop-sub", TRUE));
            }
        } else {
            char *full = lab.sub ? g_strconcat (lab.title, "\n", lab.sub, NULL) : g_strdup (lab.title);
            gboolean long_note = g_utf8_strlen (full, -1) > 220 || (lab.sub && strchr (lab.sub, '\n'));
            if (long_note) {
                GtkWidget *ex = gtk_expander_new (NULL);
                GtkWidget *hl = mk_label (lab.title, "walpe-prop-note", FALSE);
                gtk_label_set_ellipsize (GTK_LABEL (hl), PANGO_ELLIPSIZE_END);
                gtk_expander_set_label_widget (GTK_EXPANDER (ex), hl);
                gtk_expander_set_child (GTK_EXPANDER (ex), selectable_note (full));
                gtk_box_append (GTK_BOX (v), ex);
                r->title_lbl = hl;
            } else {
                r->title_lbl = mk_label (full, "walpe-prop-note", TRUE);
                gtk_box_append (GTK_BOX (v), r->title_lbl);
            }
            g_free (full);
        }
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), v);
        lab_clear (&lab);
        return r;
    }

    GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (v, TRUE);
    gtk_widget_set_valign (v, GTK_ALIGN_CENTER);
    r->title_lbl = mk_label (lab.title, "walpe-prop-title", TRUE);
    label_limit_lines (r->title_lbl, 4);
    if (lab.inert)
        gtk_widget_add_css_class (r->title_lbl, "walpe-prop-inert");
    gtk_box_append (GTK_BOX (v), r->title_lbl);
    if (lab.sub) {
        /* one paragraph: Pango's line limit counts per paragraph */
        char **parts = g_strsplit (lab.sub, "\n", -1);
        char *one = g_strjoinv (" · ", parts);
        g_strfreev (parts);
        GtkWidget *s = mk_label (one, "walpe-prop-sub", TRUE);
        g_free (one);
        gtk_label_set_lines (GTK_LABEL (s), 3);
        gtk_label_set_ellipsize (GTK_LABEL (s), PANGO_ELLIPSIZE_END);
        set_tip (s, lab.sub, NULL);
        gtk_box_append (GTK_BOX (v), s);
    }
    /* the key; a title too long for the row in full above it */
    if (lab.title && g_utf8_strlen (lab.title, -1) > 80)
        set_tip (r->title_lbl, lab.title, key);
    else
        gtk_widget_set_tooltip_text (r->title_lbl, key);
    gtk_box_append (GTK_BOX (h), v);

    GtkWidget *ctl = NULL;
    switch (r->kind) {
    case K_BOOL:
        ctl = gtk_switch_new ();
        gtk_switch_set_active (GTK_SWITCH (ctl), node_bool (cur, FALSE));
        g_signal_connect (ctl, "notify::active", G_CALLBACK (on_switch), P);
        break;
    case K_SLIDER: {
        double mn = wpj_num (def, "min", 0), mx = wpj_num (def, "max", 100);
        double st = wpj_num (def, "step", 0);
        int prec = (int) wpj_num (def, "precision", -1);
        if (!isfinite (mn)) mn = 0;
        if (!isfinite (mx) || mx <= mn) mx = mn + 1;
        gboolean frac = wpj_bool (def, "fraction", TRUE);
        if (!(st > 0) || !isfinite (st))
            st = frac ? (prec >= 0 ? pow (10, -CLAMP (prec, 0, 6)) : (mx - mn) / 100.0) : 1.0;
        r->is_int = !frac || (st >= 1 && fmod (st, 1) == 0 && fmod (mn, 1) == 0 && prec <= 0);
        double val = CLAMP (node_num (cur, r->dflt ? node_num (r->dflt, mn) : mn), mn, mx);
        r->adj = gtk_adjustment_new (val, mn, mx, st, MAX (st, (mx - mn) / 10.0), 0);
        g_object_set_data (G_OBJECT (r->adj), "walpe-prow", r);
        ctl = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *sc = gtk_scale_new (GTK_ORIENTATION_HORIZONTAL, r->adj);
        gtk_scale_set_draw_value (GTK_SCALE (sc), FALSE);
        gtk_widget_set_size_request (sc, 200, -1);
        gtk_widget_set_hexpand (sc, FALSE);
        GtkWidget *sp = gtk_spin_button_new (r->adj, st, (guint) digits_for (st, r->is_int ? 0 : prec));
        gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (sp), TRUE);
        walpe_num_field_keep_when_empty (sp);   /* (it took the option's lowest value) */
        gtk_editable_set_width_chars (GTK_EDITABLE (sp), 6);
        gtk_accessible_update_property (GTK_ACCESSIBLE (sc), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        lab.title, -1);
        gtk_accessible_update_property (GTK_ACCESSIBLE (sp), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        lab.title, -1);
        gtk_box_append (GTK_BOX (ctl), sc);
        gtk_box_append (GTK_BOX (ctl), sp);
        g_signal_connect (r->adj, "value-changed", G_CALLBACK (on_adj), P);
        break;
    }
    case K_COMBO: {
        JsonArray *opts = wpj_arr (def, "options");
        GtkStringList *sl = gtk_string_list_new (NULL);
        JsonArray *vals_a = json_array_new ();
        guint sel = 0;
        gboolean found = FALSE;
        for (guint k = 0; k < wpj_arr_len (opts); k++) {
            JsonObject *op = wpj_arr_obj (opts, k);
            JsonNode *ov = op ? json_object_get_member (op, "value") : NULL;
            if (!node_is_scalar (ov))
                continue;
            Lab ol;
            resolve_label (P, wpj_str (op, "label", NULL), NULL, &ol);
            char *os = node_str (ov);
            /* entries are single lines and the longest one sets the
             * width of the drop-down (and so of the whole pane) */
            char *oc = text_cut (ol.title ? ol.title : os, COMBO_MAX_CHARS);
            gtk_string_list_append (sl, oc ? oc : ol.title ? ol.title : os);
            g_free (oc);
            g_free (os);
            lab_clear (&ol);
            if (!found && values_equal (NULL, ov, cur)) {
                sel = json_array_get_length (vals_a);
                found = TRUE;
            }
            json_array_add_element (vals_a, json_node_copy (ov));
        }
        ctl = gtk_drop_down_new (G_LIST_MODEL (sl), NULL);
        if (g_list_model_get_n_items (G_LIST_MODEL (sl)) > 12) {
            gtk_drop_down_set_expression (GTK_DROP_DOWN (ctl),
                gtk_property_expression_new (GTK_TYPE_STRING_OBJECT, NULL, "string"));
            gtk_drop_down_set_enable_search (GTK_DROP_DOWN (ctl), TRUE);
        }
        if (json_array_get_length (vals_a))
            gtk_drop_down_set_selected (GTK_DROP_DOWN (ctl), sel);
        else
            gtk_widget_set_sensitive (ctl, FALSE);
        r->combo_values = json_node_new (JSON_NODE_ARRAY);
        json_node_take_array (r->combo_values, vals_a);
        gtk_widget_set_size_request (ctl, 200, -1);
        g_signal_connect (ctl, "notify::selected", G_CALLBACK (on_dropdown), P);
        break;
    }
    case K_COLOR: {
        GdkRGBA c;
        parse_color (cur, &c);
        char *ds = r->dflt ? node_str (r->dflt) : g_strdup ("");
        g_strstrip (ds);
        r->hex_color = ds[0] == '#';
        g_free (ds);
        GtkColorDialog *dlg = gtk_color_dialog_new ();
        gtk_color_dialog_set_with_alpha (dlg, FALSE);
        gtk_color_dialog_set_modal (dlg, FALSE);
        gtk_color_dialog_set_title (dlg, lab.title);
        ctl = gtk_color_dialog_button_new (dlg);   /* takes dlg */
        gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (ctl), &c);
        gtk_widget_set_size_request (ctl, 72, -1);
        g_signal_connect (ctl, "notify::rgba", G_CALLBACK (on_color), P);
        break;
    }
    case K_TEXT: {
        ctl = gtk_entry_new ();
        char *cs = node_str (cur);
        gtk_editable_set_text (GTK_EDITABLE (ctl), cs);
        g_free (cs);
        gtk_widget_set_size_request (ctl, 240, -1);
        g_signal_connect (ctl, "activate", G_CALLBACK (on_entry_activate), P);
        GtkEventController *fc = gtk_event_controller_focus_new ();
        g_signal_connect (fc, "leave", G_CALLBACK (on_entry_leave), P);
        gtk_widget_add_controller (ctl, fc);
        break;
    }
    case K_FILE: {
        /* a button naming the chosen file; click = file / folder dialog */
        ctl = gtk_button_new ();
        r->file_lbl = gtk_label_new (NULL);
        gtk_label_set_ellipsize (GTK_LABEL (r->file_lbl), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars (GTK_LABEL (r->file_lbl), 26);
        gtk_button_set_child (GTK_BUTTON (ctl), r->file_lbl);
        gtk_widget_set_size_request (ctl, 240, -1);
        char *cs = node_str (cur);
        r->ctl = ctl;   /* file_row_show() puts the path into its tooltip */
        file_row_show (r, cs);
        g_free (cs);
        g_signal_connect (ctl, "clicked", G_CALLBACK (on_file_clicked), P);
        break;
    }
    default: {
        char *t = g_strdup_printf ("%s: not supported yet", *type ? type : "Unknown type");
        ctl = mk_label (t, "walpe-prop-unsupported", FALSE);
        g_free (t);
        gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
        break;
    }
    }
    gtk_widget_set_valign (ctl, GTK_ALIGN_CENTER);
    if (r->kind != K_SLIDER && r->kind != K_READONLY)
        walpe_a11y_name (ctl, lab.title, r->title_lbl);
    g_object_set_data (G_OBJECT (ctl), "walpe-prow", r);
    r->ctl = ctl;
    gtk_box_append (GTK_BOX (h), ctl);

    if (r->kind != K_READONLY) {
        r->reset = gtk_button_new_from_icon_name ("edit-undo-symbolic");
        gtk_widget_add_css_class (r->reset, "flat");
        gtk_widget_add_css_class (r->reset, "walpe-reset");
        gtk_widget_set_valign (r->reset, GTK_ALIGN_CENTER);
        char *dv_s = r->dflt ? node_str (r->dflt) : g_strdup ("?");
        if (r->kind == K_COMBO && r->dflt) {
            JsonArray *vals = json_node_get_array (r->combo_values);
            GListModel *m = gtk_drop_down_get_model (GTK_DROP_DOWN (ctl));
            for (guint i = 0; i < json_array_get_length (vals); i++)
                if (values_equal (r, json_array_get_element (vals, i), r->dflt)) {
                    g_free (dv_s);
                    dv_s = g_strdup (gtk_string_list_get_string (GTK_STRING_LIST (m), i));
                    break;
                }
        }
        char *tip = g_strdup_printf ("Reset to default (%s)", dv_s);
        gtk_widget_set_tooltip_text (r->reset, tip);
        gtk_accessible_update_property (GTK_ACCESSIBLE (r->reset), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        tip, -1);
        g_free (tip);
        g_free (dv_s);
        g_object_set_data (G_OBJECT (r->reset), "walpe-prow", r);
        g_signal_connect (r->reset, "clicked", G_CALLBACK (on_reset_one), P);
        gtk_box_append (GTK_BOX (h), r->reset);
    } else {
        GtkWidget *pad = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request (pad, 28, -1);
        gtk_box_append (GTK_BOX (h), pad);
    }
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), h);
    lab_clear (&lab);
    return r;
}

/* build up to n pending rows; FALSE when all are built */
/* up to n rows; with budget_us, stops earlier once that much time went
 * into building (but not before a few rows exist): a row costs 1-10 ms
 * depending on its control and the machine, and a fixed 25 per turn of the
 * main loop was a 150-600 ms stall per turn on long option lists */
static gboolean build_chunk (Panel *P, guint n, gint64 budget_us)
{
    if (!P->pending)
        return FALSE;
    gboolean was = P->building;
    P->building = TRUE;
    guint end = MIN (P->pending->len, P->build_pos + n);
    guint first = P->build_pos;
    gint64 t0 = g_get_monotonic_time ();
    for (; P->build_pos < end; P->build_pos++) {
        if (budget_us && P->build_pos >= first + 3 && g_get_monotonic_time () - t0 >= budget_us)
            break;
        Ord *o = &g_array_index (P->pending, Ord, P->build_pos);
        PRow *r = build_row (P, o->key, o->def, json_object_get_member (P->live, o->key));
        if (!r)
            continue;
        r->cond_ok = cond_visible (r->condition, P->live);
        r->filter_ok = TRUE;
        g_ptr_array_add (P->rows, r);
        g_hash_table_insert (P->by_key, r->key, r);
        gtk_list_box_append (GTK_LIST_BOX (P->list), r->row);
        if (r->kind < K_SECTION)
            row_mark (r, json_object_get_member (P->live, r->key));
    }
    P->building = was;
    gboolean more = P->build_pos < P->pending->len;
    if (!more) {
        if (g_getenv ("WALPE_UI_DEBUG"))
            g_message ("options: %u rows built in %.1f ms (chunked)", P->rows->len,
                       (g_get_monotonic_time () - P->build_t0) / 1000.0);
        g_array_free (P->pending, TRUE);
        P->pending = NULL;
    }
    refresh_visibility (P);
    return more;
}

static gboolean build_idle (gpointer u)
{
    Panel *P = u;
    if (build_chunk (P, 25, 8000))
        return G_SOURCE_CONTINUE;
    P->build_id = 0;
    return G_SOURCE_REMOVE;
}

static void build_cancel (Panel *P)
{
    if (P->build_id) {
        g_source_remove (P->build_id);
        P->build_id = 0;
    }
    if (P->pending) {
        g_array_free (P->pending, TRUE);
        P->pending = NULL;
    }
}

/* WalPE's own sound options of a wallpaper that has sound: its switch
 * "walpe_sound" (as Engine settings say / on / off) and its volume
 * "walpe_volume" (a factor on the volume of Engine settings). The first
 * existed on the command line only (`walpe-ctl setprop walpe_sound=...`),
 * the second not at all - a scene whose author gave it no music option
 * could not be silenced or turned down by itself. They are rows like the
 * wallpaper's own options, at the top of the list: staged, sent, saved and
 * reset with them; the engine applies them to the running wallpaper
 * without loading it again. */
static const char SOUND_DEFS[] =
    "{\"walpe_sound_head\":{\"type\":\"group\",\"text\":\"Sound of this wallpaper\",\"order\":-1000002},"
    "\"walpe_sound\":{\"type\":\"combo\",\"text\":\"Sound\",\"order\":-1000001,"
    "\"value\":\"\",\"options\":[{\"label\":\"As in Engine settings\",\"value\":\"\"},"
    "{\"label\":\"On\",\"value\":\"on\"},{\"label\":\"Off\",\"value\":\"off\"}]},"
    "\"walpe_volume\":{\"type\":\"slider\",\"text\":\"Volume\",\"order\":-1000000,"
    "\"min\":0,\"max\":1,\"step\":0.01,\"precision\":2,\"value\":1}}";

static void sound_defs_add (JsonObject *defs)
{
    JsonNode *n = wpj_parse_data (SOUND_DEFS, -1);
    JsonObject *o = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
    GList *ks = o ? json_object_get_members (o) : NULL;
    for (GList *k = ks; k; k = k->next)
        json_object_set_member (defs, k->data, json_node_copy (json_object_get_member (o, k->data)));
    g_list_free (ks);
    if (n)
        json_node_unref (n);
}

void walpe_props_panel_show (GtkWidget *panel, const char *path, JsonObject *defs,
                             JsonObject *loc, gboolean is_current)
{
    Panel *P = panel_of (panel);
    build_cancel (P);
    if (P->debounce_id) {           /* send what the user changed before */
        g_source_remove (P->debounce_id);
        flush_dirty (P);
    }
    P->gen++;
    P->building = TRUE;
    /* widgets can outlive the rows (a colour dialog still open): detach
     * them from the PRow structs before those are freed */
    for (guint i = 0; i < P->rows->len; i++) {
        PRow *r = P->rows->pdata[i];
        GObject *objs[] = { G_OBJECT (r->row), r->ctl ? G_OBJECT (r->ctl) : NULL,
                            r->adj ? G_OBJECT (r->adj) : NULL,
                            r->reset ? G_OBJECT (r->reset) : NULL };
        for (guint k = 0; k < G_N_ELEMENTS (objs); k++)
            if (objs[k])
                g_object_set_data (objs[k], "walpe-prow", NULL);
    }
    gtk_list_box_remove_all (GTK_LIST_BOX (P->list));
    g_hash_table_remove_all (P->by_key);
    g_ptr_array_set_size (P->rows, 0);
    g_free (P->path);
    P->path = g_strdup (path);
    if (P->defs)
        json_object_unref (P->defs);
    if (P->loc)
        json_object_unref (P->loc);
    /* the effective definitions: for a WE preset the base item's options
     * with the preset's values as defaults, plus WalPE's own options for
     * the wallpaper's type (alignment of videos / GIFs / images); the
     * caller's project.json properties when there is nothing more */
    g_clear_pointer (&P->own_defs, json_node_unref);
    g_clear_pointer (&P->own_loc, json_node_unref);
    if (path)
        P->own_defs = walpe_content_property_defs (path, &P->own_loc);
    g_clear_pointer (&P->notes, json_object_unref);
    guint sound_layers = 0;   /* of a scene, counted in its JSON (propnotes.h): nothing is loaded */
    if (path)
        P->notes = walpe_prop_notes_sound (path, &sound_layers);
    if (P->own_defs && JSON_NODE_HOLDS_OBJECT (P->own_defs)) {
        defs = json_node_get_object (P->own_defs);
        if (P->own_loc && JSON_NODE_HOLDS_OBJECT (P->own_loc))
            loc = json_node_get_object (P->own_loc);
    }
    if (sound_layers) {
        /* a wallpaper with sound: WalPE's two sound rows, also when it has
         * no options of its own (a wallpaper without sound gets none: the
         * panel does not grow for nothing) */
        if (!(P->own_defs && JSON_NODE_HOLDS_OBJECT (P->own_defs))) {
            JsonObject *mine = json_object_new ();
            GList *dk = defs ? json_object_get_members (defs) : NULL;
            for (GList *k = dk; k; k = k->next)
                json_object_set_member (mine, k->data, json_node_copy (json_object_get_member (defs, k->data)));
            g_list_free (dk);
            g_clear_pointer (&P->own_defs, json_node_unref);
            P->own_defs = json_node_new (JSON_NODE_OBJECT);
            json_node_take_object (P->own_defs, mine);
            defs = mine;
        }
        sound_defs_add (defs);
    }
    P->defs = defs ? json_object_ref (defs) : NULL;
    P->loc = loc ? json_object_ref (loc) : NULL;
    P->loc_lang = pick_language (P->loc);
    P->is_current = is_current;
    json_object_unref (P->live);
    P->live = json_object_new ();

    GArray *ord = g_array_new (FALSE, FALSE, sizeof (Ord));
    GList *ks = defs ? json_object_get_members (defs) : NULL;
    guint idx = 0;
    for (GList *k = ks; k; k = k->next) {
        JsonObject *d = wpj_obj (defs, k->data);
        if (!d)
            continue;
        double o = wpj_num (d, "order", 1e9);
        Ord e = { k->data, d, isfinite (o) ? o : 1e9, idx++ };
        g_array_append_val (ord, e);
    }
    g_array_sort (ord, ord_cmp);

    /* defaults <- saved overrides <- staged edits */
    JsonObject *saved = path ? saved_overrides (path) : json_object_new ();
    Staged *st = path ? g_hash_table_lookup (P->staged, path) : NULL;
    for (guint i = 0; i < ord->len; i++) {
        Ord *o = &g_array_index (ord, Ord, i);
        JsonNode *v = json_object_get_member (o->def, "value");
        if (!(st && st->reset)) {
            JsonNode *sv = json_object_get_member (saved, o->key);
            if (node_is_scalar (sv))
                v = sv;
        }
        JsonNode *pv = st ? json_object_get_member (st->vals, o->key) : NULL;
        if (node_is_scalar (pv))
            v = pv;
        if (node_is_scalar (v))
            json_object_set_member (P->live, o->key, json_node_copy (v));
        else if (!v || JSON_NODE_HOLDS_NULL (v)) {
            /* "file"/"directory" options without a "value": unset = "" (their
             * conditions compare against '') */
            const char *ty = wpj_str (o->def, "type", "");
            if (!strcmp (ty, "file") || !strcmp (ty, "directory") || !strcmp (ty, "scenetexture"))
                json_object_set_string_member (P->live, o->key, "");
        }
    }

    guint editable = 0;
    for (guint i = 0; i < ord->len; i++) {
        const char *t = wpj_str (g_array_index (ord, Ord, i).def, "type", "");
        if (is_control_type (t))
            editable++;
    }
    /* rows are built in chunks: the first screenful now, the rest on idle
     * between frames (265 rows took ~330 ms in one go = a visible hitch) */
    guint n_defs = ord->len;   /* ord belongs to the chunk builder from here */
    P->pending = ord;
    P->build_pos = 0;
    P->build_t0 = g_get_monotonic_time ();
    build_chunk (P, 40, 0);
    if (P->pending)
        P->build_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, build_idle, P, NULL);
    g_list_free (ks);
    json_object_unref (saved);

    gtk_widget_set_visible (P->empty, editable == 0);
    gtk_widget_set_visible (P->scroller, n_defs > 0);
    gtk_widget_set_visible (P->reset_all, editable > 0);
    gtk_widget_set_visible (P->filter, editable > 8);
    gtk_label_set_text (GTK_LABEL (P->empty),
                        !path ? "Select a wallpaper to see its options."
                              : "This wallpaper has no user options.");
    refresh_visibility (P);
    update_hint (P);
    gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (
                                  GTK_SCROLLED_WINDOW (P->scroller)), 0);
    P->building = FALSE;
}

void walpe_props_panel_set_state (GtkWidget *panel, gboolean is_current, gboolean by_path)
{
    Panel *P = panel_of (panel);
    gboolean was_staging = !P->is_current && !P->by_path;
    P->is_current = is_current;
    P->by_path = by_path;
    update_hint (P);
    /* a daemon that can now take them (restarted, upgraded): send staged */
    if (was_staging && P->path && (is_current || by_path))
        walpe_props_panel_applied (panel, P->path);
}

void walpe_props_panel_applied (GtkWidget *panel, const char *path)
{
    Panel *P = panel_of (panel);
    if (!path)
        return;
    if (P->path && !g_strcmp0 (P->path, path)) {
        P->is_current = TRUE;
        update_hint (P);
    }
    Staged *s = g_hash_table_lookup (P->staged, path);
    if (!s)
        return;
    /* by path when the daemon can: immune to the active wallpaper
     * changing again before the request is handled */
    send_props (P, path, s->vals, s->reset, P->by_path);
    g_hash_table_remove (P->staged, path);
}

void walpe_props_panel_focus_filter (GtkWidget *panel)
{
    Panel *P = panel_of (panel);
    if (gtk_widget_get_visible (P->filter))
        gtk_widget_grab_focus (P->filter);
}

static void panel_free (gpointer p)
{
    Panel *P = p;
    if (P->debounce_id)
        g_source_remove (P->debounce_id);
    build_cancel (P);
    g_ptr_array_unref (P->rows);
    g_hash_table_unref (P->by_key);
    g_hash_table_unref (P->staged);
    json_object_unref (P->live);
    json_object_unref (P->dirty);
    if (P->defs)
        json_object_unref (P->defs);
    if (P->loc)
        json_object_unref (P->loc);
    g_clear_pointer (&P->own_defs, json_node_unref);
    g_clear_pointer (&P->own_loc, json_node_unref);
    g_clear_pointer (&P->notes, json_object_unref);
    g_free (P->path);
    g_free (P->filter_text);
    g_free (P);
}

GtkWidget *walpe_props_panel_new (WalpePropsStatus status)
{
    Panel *P = g_new0 (Panel, 1);
    P->status = status;
    P->rows = g_ptr_array_new_with_free_func (prow_free);
    P->by_key = g_hash_table_new (g_str_hash, g_str_equal);
    P->staged = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, staged_free);
    P->live = json_object_new ();
    P->dirty = json_object_new ();

    P->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand (P->box, TRUE);

    GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class (head, "walpe-options-head");
    GtkWidget *t = mk_label ("Wallpaper options", "walpe-options-title", FALSE);
    gtk_box_append (GTK_BOX (head), t);
    P->count = mk_label ("", "walpe-badge", FALSE);
    gtk_widget_add_css_class (P->count, "neutral");
    gtk_widget_set_valign (P->count, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (head), P->count);
    GtkWidget *sp = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (sp, TRUE);
    gtk_box_append (GTK_BOX (head), sp);
    P->filter = gtk_search_entry_new ();
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (P->filter), "Find an option");
    gtk_widget_set_size_request (P->filter, 220, -1);
    g_signal_connect (P->filter, "search-changed", G_CALLBACK (on_filter), P);
    gtk_box_append (GTK_BOX (head), P->filter);
    P->reset_all = gtk_button_new_with_mnemonic ("_Reset all");
    gtk_widget_set_tooltip_text (P->reset_all, "Reset every option of this wallpaper to its default");
    g_signal_connect (P->reset_all, "clicked", G_CALLBACK (on_reset_all), P);
    gtk_box_append (GTK_BOX (head), P->reset_all);
    gtk_box_append (GTK_BOX (P->box), head);

    P->hint = mk_label (NULL, "walpe-options-hint", TRUE);
    gtk_label_set_lines (GTK_LABEL (P->hint), 2);   /* height never grows with errors */
    gtk_label_set_ellipsize (GTK_LABEL (P->hint), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start (P->hint, 16);
    gtk_widget_set_margin_end (P->hint, 16);
    gtk_widget_set_margin_bottom (P->hint, 6);
    gtk_box_append (GTK_BOX (P->box), P->hint);

    P->list = gtk_list_box_new ();
    gtk_widget_add_css_class (P->list, "walpe-options");
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (P->list), GTK_SELECTION_NONE);
    gtk_list_box_set_filter_func (GTK_LIST_BOX (P->list), filter_func, P, NULL);
    g_signal_connect (P->list, "row-activated", G_CALLBACK (on_row_activated), P);
    P->scroller = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (P->scroller), GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (P->scroller, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (P->scroller), P->list);
    gtk_box_append (GTK_BOX (P->box), P->scroller);

    P->empty = mk_label ("Select a wallpaper to see its options.", "walpe-empty", TRUE);
    gtk_label_set_xalign (GTK_LABEL (P->empty), 0.5f);
    gtk_widget_set_vexpand (P->empty, TRUE);
    gtk_widget_set_valign (P->empty, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (P->box), P->empty);
    gtk_widget_set_visible (P->scroller, FALSE);
    gtk_widget_set_visible (P->reset_all, FALSE);
    gtk_widget_set_visible (P->filter, FALSE);

    g_object_set_data_full (G_OBJECT (P->box), "walpe-props", P, panel_free);
    return P->box;
}

char *walpe_props_plain_text (const char *markup)
{
    if (!markup || !*markup)
        return g_strdup ("");
    Lab l = { 0 };
    markup_to_lab (markup, &l);
    char *out = l.sub ? g_strconcat (l.title, "\n", l.sub, NULL) : g_strdup (l.title ? l.title : "");
    lab_clear (&l);
    return out;
}
