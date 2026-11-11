/* fxclass.c - see fxclass.h */
#include "fxclass.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------- family */

/* the effect's directory name, lower case, letters and digits only */
static void fx_basename (const char *file, char *out, gsize n)
{
    out[0] = 0;
    if (!file || !n)
        return;
    const char *end = strrchr (file, '/');
    const char *start = file;
    if (end) {
        for (start = end; start > file && start[-1] != '/'; start--)
            ;
    } else {
        end = file + strlen (file);
    }
    gsize k = 0;
    for (const char *p = start; p < end && k + 1 < n; p++)
        if (g_ascii_isalnum (*p))
            out[k++] = g_ascii_tolower (*p);
    out[k] = 0;
}

WalpeFxFamily walpe_fx_family (const char *effect_file)
{
    char n[96];
    fx_basename (effect_file, n, sizeof n);
    if (!*n)
        return WALPE_FXF_NONE;
    /* names that say the effect decides what is shown: never a post pass */
    static const char *const content[] = { "mask", "cutout", "reveal", "clip", "switch",
                                           "transition", "motionblur", NULL };
    for (int i = 0; content[i]; i++)
        if (strstr (n, content[i]))
            return WALPE_FXF_NONE;
    if (strstr (n, "bloom"))
        return WALPE_FXF_BLOOM;
    if (strstr (n, "blur"))
        return WALPE_FXF_BLUR;
    if (strstr (n, "grain"))
        return WALPE_FXF_GRAIN;
    if (strstr (n, "vignette"))
        return WALPE_FXF_VIGNETTE;
    if (strstr (n, "chromatic"))
        return WALPE_FXF_CHROMA;
    return WALPE_FXF_NONE;
}

const char *walpe_fx_family_name (WalpeFxFamily f)
{
    switch (f) {
    case WALPE_FXF_BLUR: return "blur";
    case WALPE_FXF_BLOOM: return "bloom";
    case WALPE_FXF_GRAIN: return "film grain";
    case WALPE_FXF_VIGNETTE: return "vignette";
    case WALPE_FXF_CHROMA: return "chromatic aberration";
    default: return "";
    }
}

gboolean walpe_fx_family_cosmetic (WalpeFxFamily f)
{
    return f == WALPE_FXF_BLOOM || f == WALPE_FXF_GRAIN || f == WALPE_FXF_VIGNETTE ||
           f == WALPE_FXF_CHROMA;
}

gboolean walpe_fx_family_reducible (WalpeFxFamily f)
{
    return f == WALPE_FXF_BLUR || f == WALPE_FXF_BLOOM;
}

gboolean walpe_fx_name_fast (const char *effect_file)
{
    char n[96];
    fx_basename (effect_file, n, sizeof n);
    static const char *const fast[] = { "grain", "vhs", "glitch", "noise", "glitter", "fire",
                                        "static", "crt", "spark", "flicker", NULL };
    for (int i = 0; fast[i]; i++)
        if (strstr (n, fast[i]))
            return TRUE;
    return FALSE;
}

/* ---------------------------------------------------------- time scan */

/* The scan reads a wallpaper's shader. Two bounds keep it from being a
 * way to stop the render thread: brackets inside a term are followed
 * FXC_MAX_NEST deep (factor () and product () call each other once per
 * bracket: 100 000 of them were a stack overflow), and the characters the
 * scan steps over are counted - every g_Time walks to both ends of its
 * term, so a term of n factors g_Time cost n * n steps (a shader of a few
 * hundred KiB kept the load busy for minutes). Past either bound the
 * answer is "cannot be told", which is what the scan says about any
 * shader it does not understand: the chain is redrawn every frame. */
#define FXC_MAX_NEST 24
#define FXC_MAX_WORK (20 * 1000 * 1000)

typedef struct {
    const char *b, *e;       /* the source, comments blanked */
    WalpeFxValueFn value;
    gpointer user;
    int nest;                /* product () calls on the stack */
    gint64 work;             /* characters stepped over so far */
} Scan;

static gboolean is_id (char c)
{
    return g_ascii_isalnum (c) || c == '_';
}

static const char *ws_fwd (const char *p, const char *e)
{
    while (p < e && g_ascii_isspace (*p))
        p++;
    return p;
}

/* p at the last character looked at: the previous non-space one, or b - 1 */
static const char *ws_back (const char *p, const char *b)
{
    while (p >= b && g_ascii_isspace (*p))
        p--;
    return p;
}

/* p at an opening bracket: its partner, or NULL */
static const char *match_fwd (Scan *sc, const char *p, const char *e)
{
    char open = *p, close = open == '(' ? ')' : ']';
    int depth = 0;
    const char *from = p;
    for (; p < e; p++) {
        if (*p == open)
            depth++;
        else if (*p == close && --depth == 0)
            break;
    }
    sc->work += p - from;
    return p < e ? p : NULL;
}

/* p at a closing bracket: its partner, or NULL */
static const char *match_back (Scan *sc, const char *p, const char *b)
{
    char close = *p, open = close == ')' ? '(' : '[';
    int depth = 0;
    const char *from = p;
    for (; p >= b; p--) {
        if (*p == close)
            depth++;
        else if (*p == open && --depth == 0)
            break;
    }
    sc->work += from - p;
    return p >= b ? p : NULL;
}

static gboolean product (Scan *sc, const char *s, const char *e, const char *hs, const char *he,
                         double hole, double *out);

static gboolean name_is (const char *s, gsize n, const char *w)
{
    return strlen (w) == n && !strncmp (s, w, n);
}

/* largest magnitude among the comma separated arguments in [s, e), each a
 * product of known factors */
static gboolean args_max (Scan *sc, const char *s, const char *e, double *out)
{
    double best = 0;
    const char *a = s;
    int depth = 0;
    for (const char *p = s; p <= e; p++) {
        if (p < e && (*p == '(' || *p == '['))
            depth++;
        else if (p < e && (*p == ')' || *p == ']'))
            depth--;
        if (p == e || (*p == ',' && depth == 0)) {
            double m = 0;
            if (!product (sc, a, p, NULL, NULL, 0, &m))
                return FALSE;
            best = MAX (best, m);
            a = p + 1;
        }
    }
    *out = best;
    return TRUE;
}

/* one factor at p: number, identifier, call, bracketed product. Returns
 * its end (NULL = not something this understands); *known FALSE = a value
 * that cannot be told */
static const char *factor (Scan *sc, const char *p, const char *e, double *mag, gboolean *known)
{
    *known = TRUE;
    *mag = 1;
    p = ws_fwd (p, e);
    while (p < e && (*p == '-' || *p == '+'))
        p = ws_fwd (p + 1, e);
    if (p >= e)
        return NULL;
    if (g_ascii_isdigit (*p) || (*p == '.' && p + 1 < e && g_ascii_isdigit (p[1]))) {
        char *q = NULL;
        *mag = fabs (g_ascii_strtod (p, &q));
        if (!q || q == p || q > e)
            return NULL;
        p = q;
        if (p < e && (*p == 'f' || *p == 'F'))
            p++;
        return p;
    }
    if (*p == '(') {
        const char *c = match_fwd (sc, p, e);
        if (!c)
            return NULL;
        if (!product (sc, p + 1, c, NULL, NULL, 0, mag))
            *known = FALSE;
        return c + 1;
    }
    if (!is_id (*p) || g_ascii_isdigit (*p))
        return NULL;
    const char *q = p;
    while (q < e && is_id (*q))
        q++;
    gsize n = (gsize) (q - p);
    const char *r = ws_fwd (q, e);
    if (r < e && *r == '(') {
        const char *c = match_fwd (sc, r, e);
        if (!c)
            return NULL;
        if (name_is (p, n, "vec2") || name_is (p, n, "vec3") || name_is (p, n, "vec4") ||
            name_is (p, n, "float") || name_is (p, n, "CAST2") || name_is (p, n, "CAST3") ||
            name_is (p, n, "CAST4") || name_is (p, n, "abs")) {
            if (!args_max (sc, r + 1, c, mag))
                *known = FALSE;
        } else if (name_is (p, n, "rotateVec2") || name_is (p, n, "normalize") ||
                   name_is (p, n, "sign")) {
            *mag = 1;   /* a direction: length 1 */
        } else {
            *known = FALSE;
        }
        q = c + 1;
    } else {
        char name[96];
        if (n >= sizeof name) {
            *known = FALSE;
        } else {
            memcpy (name, p, n);
            name[n] = 0;
            if (!strcmp (name, "M_PI"))
                *mag = G_PI;
            else if (!strcmp (name, "M_PI_HALF"))
                *mag = G_PI / 2;
            else if (!strcmp (name, "M_PI_2"))
                *mag = 2 * G_PI;
            else if (!sc->value || !sc->value (name, mag, sc->user))
                *known = FALSE;
            *mag = fabs (*mag);
        }
    }
    /* .xy / [i] behind it */
    for (;;) {
        const char *t = ws_fwd (q, e);
        if (t < e && *t == '.' && t + 1 < e && g_ascii_isalpha (t[1])) {
            q = t + 1;
            while (q < e && is_id (*q))
                q++;
        } else if (t < e && *t == '[') {
            const char *c = match_fwd (sc, t, e);
            if (!c)
                return NULL;
            q = c + 1;
        } else {
            break;
        }
    }
    return q;
}

/* [s, e) as factor (* or / factor)...; [hs, he) inside it (if any) counts
 * as the value `hole`. FALSE = not a product of known values. A factor
 * known to be 0 makes the product 0 whatever the others are. */
static gboolean product_at (Scan *sc, const char *s, const char *e, const char *hs, const char *he,
                            double hole, double *out);

static gboolean product (Scan *sc, const char *s, const char *e, const char *hs, const char *he,
                         double hole, double *out)
{
    if (sc->nest >= FXC_MAX_NEST || sc->work > FXC_MAX_WORK)
        return FALSE;
    sc->nest++;
    sc->work += e - s;
    gboolean r = product_at (sc, s, e, hs, he, hole, out);
    sc->nest--;
    return r;
}

static gboolean product_at (Scan *sc, const char *s, const char *e, const char *hs, const char *he,
                            double hole, double *out)
{
    double prod = 1;
    gboolean known = TRUE, zero = FALSE, parsed = TRUE;
    char op = '*';
    const char *p = s;
    for (int guard = 0; guard < 64; guard++) {
        double m = 1;
        gboolean k = TRUE;
        const char *t = ws_fwd (p, e);
        while (t < e && (*t == '-' || *t == '+') && !(hs && t >= hs && t < he))
            t = ws_fwd (t + 1, e);
        if (hs && t == hs) {
            m = hole;
            p = he;
        } else {
            p = factor (sc, t, e, &m, &k);
            if (!p) {
                parsed = FALSE;
                break;
            }
        }
        if (!k)
            known = FALSE;
        else if (op == '*' && m == 0)
            zero = TRUE;
        else if (op == '*')
            prod *= m;
        else if (m > 0)
            prod /= m;
        else
            known = FALSE;
        p = ws_fwd (p, e);
        if (p >= e)
            break;
        if (*p != '*' && *p != '/') {
            parsed = FALSE;
            break;
        }
        op = *p++;
    }
    if (!parsed)
        return FALSE;
    if (zero) {
        *out = 0;
        return TRUE;
    }
    *out = prod;
    return known;
}

/* characters that end a multiplicative term (outside brackets) */
static gboolean term_stop (char c)
{
    return c == '+' || c == '-' || c == ',' || c == ';' || c == '=' || c == '{' || c == '}' ||
           c == '?' || c == ':' || c == '<' || c == '>' || c == '!' || c == '&' || c == '|' ||
           c == '#' || c == '%';
}

/* the term around [s, e): widen to the nearest stops. A sign right behind
 * * or / belongs to the term; brackets are stepped over whole; line ends
 * are white space (an expression may run over several lines). */
static void term_bounds (Scan *sc, const char **s, const char **e)
{
    const char *p = *s - 1;
    for (; p >= sc->b; p--) {
        char c = *p;
        if (g_ascii_isspace (c))
            continue;
        if (c == ')' || c == ']') {
            const char *m = match_back (sc, p, sc->b);
            if (!m)
                break;
            p = m;
            continue;
        }
        if (c == '(' || c == '[')
            break;
        if (c == '+' || c == '-') {
            const char *b4 = ws_back (p - 1, sc->b);
            if (b4 >= sc->b && (*b4 == '*' || *b4 == '/'))
                continue;   /* unary */
            break;
        }
        if (term_stop (c))
            break;
    }
    sc->work += *s - p;
    *s = p + 1;
    const char *q = *e;
    for (; q < sc->e; q++) {
        char c = *q;
        if (g_ascii_isspace (c))
            continue;
        if (c == '(' || c == '[') {
            const char *m = match_fwd (sc, q, sc->e);
            if (!m)
                break;
            q = m;
            continue;
        }
        if (c == ')' || c == ']')
            break;
        if (c == '+' || c == '-') {
            const char *b4 = ws_back (q - 1, sc->b);
            if (b4 >= sc->b && (*b4 == '*' || *b4 == '/'))
                continue;
            break;
        }
        if (term_stop (c))
            break;
    }
    sc->work += q - *e;
    *e = q;
}

/* the round bracket position p sits in (other arguments and terms before
 * it stepped over), NULL = none in this statement */
static const char *enclosing_open (Scan *sc, const char *p)
{
    const char *from = p, *r = NULL;
    for (p--; p >= sc->b; p--) {
        char c = *p;
        if (c == ')' || c == ']') {
            const char *m = match_back (sc, p, sc->b);
            if (!m)
                break;
            p = m;
        } else if (c == '(') {
            r = p;
            break;
        } else if (c == '[' || c == ';' || c == '{' || c == '}' || c == '#') {
            break;
        }
    }
    sc->work += from - p;
    return r;
}

/* the rate of the g_Time at [g, g + 6); FALSE = cannot be told */
static gboolean time_term (Scan *sc, const char *g, double *rate)
{
    const char *hs = g, *he = g + 6;
    double cur = 1;
    gboolean bare = TRUE;   /* nothing multiplied with it so far */
    for (int level = 0; level < 8; level++) {
        const char *s = hs, *e = he;
        term_bounds (sc, &s, &e);
        double m = 0;
        if (!product (sc, s, e, hs, he, cur, &m))
            return FALSE;
        if (ws_fwd (s, e) != hs || ws_back (e - 1, sc->b) != he - 1)
            bare = FALSE;
        cur = m;
        const char *open = enclosing_open (sc, s);
        if (!open)
            break;
        const char *close = match_fwd (sc, open, sc->e);
        if (!close)
            break;
        /* a call or plain brackets? */
        const char *n1 = ws_back (open - 1, sc->b);
        const char *n0 = n1;
        while (n0 >= sc->b && is_id (*n0))
            n0--;
        gsize nl = (gsize) (n1 - n0);
        if (nl == 0) {
            /* (g_Time + offset) * speed: the brackets are a factor */
            hs = open;
            he = close + 1;
            continue;
        }
        const char *nm = n0 + 1;
        static const char *const through[] = { "frac", "fract", "mod", "abs", "vec2", "vec3",
                                               "vec4", "float", "CAST2", "CAST3", "CAST4",
                                               "min", "max", "clamp", NULL };
        gboolean thru = FALSE;
        for (int i = 0; through[i]; i++)
            thru |= name_is (nm, nl, through[i]);
        if (thru) {
            hs = nm;
            he = close + 1;
            continue;
        }
        static const char *const ends[] = { "sin", "cos", "tan", "rotateVec2", "texSample2D",
                                            "texSample2DLod", "texture2D", "texture",
                                            "smoothstep", "step", "mix", "lerp", "pow", "exp",
                                            "sqrt", "length", "dot", "floor", "ceil", "saturate",
                                            "if", "for", "while", "return", NULL };
        gboolean known_fn = FALSE;
        for (int i = 0; ends[i]; i++)
            known_fn |= name_is (nm, nl, ends[i]);
        /* time handed bare to a function of the shader's own: what that
         * does with it is not looked into */
        if (!known_fn && bare)
            return FALSE;
        break;
    }
    *rate = cur;
    return TRUE;
}

void walpe_fx_time_scan (const char *src, WalpeFxValueFn value, gpointer user, WalpeFxTime *t)
{
    if (!src || !*src)
        return;
    /* comments out of the way (same length, so positions stay) */
    char *buf = g_strdup (src);
    for (char *p = buf; *p; p++) {
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                *p++ = ' ';
            if (!*p)
                break;
        } else if (p[0] == '/' && p[1] == '*') {
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p != '\n')
                    *p = ' ';
                p++;
            }
            if (!*p)
                break;
            p[0] = p[1] = ' ';
            p++;
        }
    }
    Scan sc = { buf, buf + strlen (buf), value, user, 0, 0 };
    for (const char *g = buf; (g = strstr (g, "g_Time")); g += 6) {
        if ((g > buf && is_id (g[-1])) || is_id (g[6]))
            continue;   /* g_TimeOffset, u_g_Time */
        if (sc.work > FXC_MAX_WORK) {
            /* too much to read: time is used, how cannot be told */
            t->uses_time = TRUE;
            t->derived = FALSE;
            break;
        }
        /* its declaration */
        const char *ls = g;
        while (ls > buf && ls[-1] != '\n' && ls[-1] != ';')
            ls--;
        sc.work += g - ls;
        ls = ws_fwd (ls, g);
        if (!strncmp (ls, "uniform", 7) || *ls == '#')
            continue;
        double r = 0;
        if (!t->uses_time)
            t->derived = TRUE;
        t->uses_time = TRUE;
        if (time_term (&sc, g, &r))
            t->rate = MAX (t->rate, r);
        else
            t->derived = FALSE;
    }
    g_free (buf);
}

double walpe_fx_bounds_factor (double lo, double hi)
{
    lo = CLAMP (lo, 0.0, 1.0);
    hi = CLAMP (hi, 0.0, 1.0);
    if (hi < lo) {
        double x = lo;
        lo = hi;
        hi = x;
    }
    double d = asin (2 * hi - 1) - asin (2 * lo - 1);
    return G_PI / MAX (d, 1e-3);
}

int walpe_fx_slow_div (const WalpeFxTime *t, double sharp, int want, double fps)
{
    want = CLAMP (want, 1, 3);
    if (!t->uses_time)
        return want;
    if (!t->derived || !(fps > 0))
        return 1;
    if (!(sharp >= 1))
        sharp = 1;
    for (int d = want; d >= 2; d--)
        if (t->rate * sharp * d / fps <= WALPE_FX_SLOW_STEP + 1e-9)
            return d;
    return 1;
}
