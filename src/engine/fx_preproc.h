/* fx_preproc.h - what the driver's preprocessor makes of a shader source.
 *
 * The limits on what a wallpaper's shader may cost (fx_srcrepair.h) have
 * to count the text a compiler really gets: the lines the #if directives
 * leave, every macro call replaced by what it stands for. This is that
 * pass, written out: a GLSL preprocessor (GLSL 3.30 chapter 3; GLSL ES 3.00
 * the same) that produces the tokens and nothing else - no messages, no
 * line markers.
 *
 *   - a line ends at CR, LF, CR LF or LF CR; backslash-newline joins lines;
 *     comments are a blank (a block comment may hold line ends);
 *   - #define with and without parameters, #undef; a call's arguments may
 *     lie on several lines and hold any text (';', braces); arguments are
 *     expanded before they are put in, except beside "##", which makes one
 *     token of two; a macro is not expanded inside its own expansion; the
 *     call of a function-like macro may begin in one expansion and end in
 *     the text behind it;
 *   - #if / #ifdef / #ifndef / #elif / #else / #endif with the integer
 *     expressions and defined; __LINE__, __FILE__, __VERSION__, GL_ES;
 *   - #version, #extension, #pragma, #error, #line and anything else that
 *     begins with '#' produce no tokens.
 *
 * It only reads: the text handed to the driver stays what it was.
 *
 * Bounded: FxPreConf.max_work is the number of tokens it may move (every
 * token read, copied into an argument or written out is one), max_out the
 * tokens it may produce, max_bytes the text they may come to; beyond one
 * of them it stops and says why (FxPre.fail).
 * Memory is 16 bytes per token moved at most, time grows with the tokens
 * moved. The only recursion is one level per macro call nested in the
 * argument of another, FXPRE_MAX_NEST deep at most. Nothing here calls
 * OpenGL; the one question it may have for the driver - is a name of the
 * GL_ or __ kind, which only the driver defines, defined - goes through
 * FxPreConf.probe.
 */
#ifndef WALPE_FX_PREPROC_H
#define WALPE_FX_PREPROC_H

#include <glib.h>
#include <string.h>

enum { FXPRE_T_IDENT = 1, FXPRE_T_NUM, FXPRE_T_PUNCT, FXPRE_T_PASTE /* the "##" of a macro's body */,
       FXPRE_T_MARK /* an empty argument beside "##" */ };

typedef struct {
    const char *s;          /* in the cleaned text, or in the arena (pasted, predefined) */
    guint32 len;
    guint32 sym : 21;       /* a name: its number - the same for the same name, so that
                             * no name is compared or hashed by its text a second time
                             * (one of 60000 characters, copied 65536 times by three
                             * lines of macros, was a minute of that) */
    guint32 kind : 3;
    guint32 noexp : 1;      /* the name of a macro, met inside that macro's expansion */
    guint32 spare : 7;
} FxPreTok;

/* macro calls nested in arguments ("F(F(F(1)))"): not a shader beyond
 * (2000 deep are 0.75 s and 580 MiB in Mesa 25.2) */
#define FXPRE_MAX_NEST 512

/* FxPreConf.probe: is `name` (an identifier beginning with "GL_" or "__"
 * that the text does not define) defined by the driver's preprocessor?
 * 1 = yes, *value its value; 0 = no; -1 = cannot say */
typedef int (*FxPreProbe) (const char *name, gint64 *value, gpointer user);

typedef struct {
    gint64 max_work;    /* tokens moved */
    gint64 max_out;     /* tokens produced */
    gint64 max_bytes;   /* bytes of the tokens written out, a blank after each, also
                         * those of an argument before it is put in: a token is as
                         * long as the source makes it, and the driver's preprocessor
                         * writes every copy out */
    FxPreProbe probe;
    gpointer probe_user;
    /* What cannot be known here is not guessed. Three things are of that
     * kind: a name of the driver's that the probe cannot answer (or there
     * is no probe); a condition that reads __LINE__ or __FILE__ (how
     * lines are counted differs between drivers in the corners: Mesa 25.2
     * counts one line too few behind a joined line when a source mixes its
     * line ends); and a condition whose arithmetic leaves what every
     * preprocessor agrees on (FxPreVal) or that is no expression at all.
     * Each such name and each such condition takes the next
     * bit of `forced`: the name is defined as 1 or not defined, the
     * condition holds or does not. FxPre.unknown says how many bits a run
     * used; the caller runs every combination (fx_pre_next ()) and holds
     * the shader to its limits in each. More than FXPRE_MAX_UNKNOWN of them:
     * not a shader (FxPre.fail). */
    guint32 forced;
    /* tests against one driver: __LINE__ and __FILE__ in a condition are
     * the numbers worked out here, "defined" does not hold for them, and
     * a condition with an odd value in it does not hold */
    gboolean trust_line;
} FxPreConf;

#define FXPRE_MAX_UNKNOWN 6

typedef struct {
    FxPreTok name;
    FxPreTok *body;
    guint nbody;
    FxPreTok *params;
    int nparams;        /* -1: without a parameter list */
    guint32 *par;       /* for every token of the body: 1 + the parameter it names, 0 = none
                         * (NULL: no parameters) */
    guint8 disabled;    /* being expanded */
    guint8 special;     /* 1 __LINE__, 2 __FILE__, 3 __VERSION__ */
} FxPreMacro;

typedef struct {
    FxPreTok *t;
    guint n, i;
    FxPreMacro *mac;      /* the macro this is the expansion of: enabled again behind it */
    gboolean owned;
} FxPreFrame;

typedef struct { guint8 active, taken, parent, seen_else; } FxPreIf;

typedef struct FxPre_ {
    FxPreConf conf;
    char *buf;              /* the text, line ends made LF, joined lines joined, comments blank */
    const char *p;          /* the next line */
    gint64 line;            /* number of the line at p */
    gint64 file;
    int version;
    gboolean es;
    GHashTable *names;      /* FxPreName -> the name's number */
    GHashTable *macros;     /* a name's number -> FxPreMacro */
    GPtrArray *all;         /* every FxPreMacro made, also the redefined and undefined
                             * ones: a call may be reading its arguments while a
                             * line between them undefines the macro */
    GArray *ifs;            /* FxPreIf */
    GStringChunk *arena;
    GArray *out;            /* FxPreTok: the result */
    GArray *linebuf;        /* FxPreTok: the line being read */
    gint64 work, bytes;
    const char *fail;       /* NULL, or why the text is not read to its end */
    int unknown;            /* bits of conf.forced in use */
    int odd;                /* conditions with arithmetic that is not every preprocessor's (trust_line: taken as 0) */
    FxPreTok unknown_name[FXPRE_MAX_UNKNOWN];    /* the name a bit stands for (len 0: a condition) */
    gboolean in_cond;       /* an #if / #elif expression is being worked out ... */
    gboolean cond_line;     /* ... and it read __LINE__ or __FILE__ */
    int nest;
    guint directives;       /* directive lines obeyed so far */
    gboolean begun;         /* a line that is not blank has been read: "#version" comes too late */
} FxPre;

/* ---------------------------------------------------------------- text */

static inline gboolean fxpre_ident_start (char c)
{
    return g_ascii_isalpha (c) || c == '_';
}

static inline gboolean fxpre_ident_char (char c)
{
    return g_ascii_isalnum (c) || c == '_';
}

/* length of the line end at p (0: none) */
static inline int fxpre_eol (const char *p)
{
    if (*p != '\n' && *p != '\r')
        return 0;
    return (p[1] == '\n' || p[1] == '\r') && p[1] != p[0] ? 2 : 1;
}

/* The text as the lexer wants it. Two passes, as the language has them:
 * backslash-newline goes first (so a "//" comment can be continued and a
 * "/" "\" newline "/" is one), then comments. A line end that goes this
 * way - joined, or inside a block comment - is put back behind the next
 * one that stays, so every later line keeps its number. */
static char *fxpre_clean (const char *src)
{
    const gsize len = strlen (src);
    char *a = g_malloc (len + 1), *b = g_malloc (len + 1);
    gsize n = 0, owed = 0;
    for (const char *p = src; *p;) {
        int e;
        if (*p == '\\' && (e = fxpre_eol (p + 1)) != 0) {
            p += 1 + e;
            owed++;
        } else if ((e = fxpre_eol (p)) != 0) {
            p += e;
            a[n++] = '\n';
            for (; owed; owed--)
                a[n++] = '\n';
        } else {
            a[n++] = *p++;
        }
    }
    a[n] = 0;
    gsize m = 0;
    owed = 0;
    for (const char *p = a; *p;) {
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
        } else if (p[0] == '/' && p[1] == '*') {
            b[m++] = ' ';
            for (p += 2; *p && !(p[0] == '*' && p[1] == '/'); p++)
                if (*p == '\n')
                    owed++;
            if (*p)
                p += 2;
        } else if (*p == '\n') {
            b[m++] = *p++;
            for (; owed; owed--)
                b[m++] = '\n';
        } else {
            b[m++] = *p++;
        }
    }
    b[m] = 0;
    g_free (a);
    return b;
}

/* the number of the name [s, s + len) */
static guint32 fxpre_intern (FxPre *pp, const char *s, guint32 len);

/* the token at *p (which is no blank and no line end); *p moves behind it */
static FxPreTok fxpre_lex (FxPre *pp, const char **p)
{
    const char *s = *p, *c = s;
    FxPreTok t = { s, 0, 0, FXPRE_T_PUNCT, 0, 0 };
    if (fxpre_ident_start (*c)) {
        t.kind = FXPRE_T_IDENT;
        while (fxpre_ident_char (*c))
            c++;
    } else if (g_ascii_isdigit (*c) || (*c == '.' && g_ascii_isdigit (c[1]))) {
        /* a preprocessing number: 1.0e-5, 0x1F, 1u, 3.xx */
        t.kind = FXPRE_T_NUM;
        c++;
        for (;;) {
            if ((*c == 'e' || *c == 'E' || *c == 'p' || *c == 'P') && (c[1] == '+' || c[1] == '-'))
                c += 2;
            else if (fxpre_ident_char (*c) || *c == '.')
                c++;
            else
                break;
        }
    } else {
        static const char *const three[] = { "<<=", ">>=" };
        static const char *const two[] = { "##", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "^^", "++", "--",
                                           "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=" };
        gsize l = 1;
        for (guint i = 0; i < G_N_ELEMENTS (three) && l == 1; i++)
            if (c[0] == three[i][0] && c[1] == three[i][1] && c[2] == three[i][2])
                l = 3;
        for (guint i = 0; i < G_N_ELEMENTS (two) && l == 1; i++)
            if (c[0] == two[i][0] && c[1] == two[i][1])
                l = 2;
        c += l;
    }
    t.len = (guint32) (c - s);
    if (t.kind == FXPRE_T_IDENT)
        t.sym = fxpre_intern (pp, s, t.len);
    *p = c;
    return t;
}

static inline gboolean fxpre_is (const FxPreTok *t, const char *w)
{
    const gsize l = strlen (w);
    return t->len == l && !memcmp (t->s, w, l);
}

/* the same name */
static inline gboolean fxpre_same (const FxPreTok *a, const FxPreTok *b)
{
    return a->sym && a->sym == b->sym;
}

typedef struct { const char *s; guint32 len; } FxPreName;

static guint fxpre_name_hash (gconstpointer p)
{
    const FxPreName *t = p;
    guint h = 2166136261u;
    for (guint32 i = 0; i < t->len; i++)
        h = (h ^ (guchar) t->s[i]) * 16777619u;
    return h;
}

static gboolean fxpre_name_equal (gconstpointer a, gconstpointer b)
{
    const FxPreName *x = a, *y = b;
    return x->len == y->len && !memcmp (x->s, y->s, x->len);
}

static void fxpre_macro_free (gpointer p)
{
    FxPreMacro *m = p;
    g_free (m->body);
    g_free (m->params);
    g_free (m->par);
    g_free (m);
}

/* a token moved: FALSE when that was one too many */
static inline gboolean fxpre_charge (FxPre *pp, gint64 n)
{
    pp->work += n;
    if (pp->work > pp->conf.max_work && !pp->fail)
        pp->fail = "its macros make the preprocessor move too many tokens";
    return !pp->fail;
}

static guint32 fxpre_intern (FxPre *pp, const char *s, guint32 len)
{
    FxPreName key = { s, len };
    gpointer v = g_hash_table_lookup (pp->names, &key);
    if (v)
        return GPOINTER_TO_UINT (v);
    const guint id = g_hash_table_size (pp->names) + 1;
    if (id >= (1u << 21)) {
        if (!pp->fail)
            pp->fail = "more than two million different names";
        return 0;
    }
    FxPreName *k = g_new (FxPreName, 1);
    *k = key;
    g_hash_table_insert (pp->names, k, GUINT_TO_POINTER (id));
    return id;
}

static FxPreTok fxpre_number (FxPre *pp, gint64 v)
{
    char tmp[32];
    g_snprintf (tmp, sizeof (tmp), "%" G_GINT64_FORMAT, v);
    FxPreTok t = { g_string_chunk_insert (pp->arena, tmp), (guint32) strlen (tmp), 0, FXPRE_T_NUM, 0, 0 };
    return t;
}

/* ----------------------------------------------------- the macro table */

static FxPreMacro *fxpre_lookup (FxPre *pp, const FxPreTok *name)
{
    return name->sym ? g_hash_table_lookup (pp->macros, GUINT_TO_POINTER ((guint) name->sym)) : NULL;
}

static void fxpre_predefine (FxPre *pp, const char *name, int special, gint64 value)
{
    FxPreMacro *m = g_new0 (FxPreMacro, 1);
    m->name = (FxPreTok) { name, (guint32) strlen (name), 0, FXPRE_T_IDENT, 0, 0 };
    m->name.sym = fxpre_intern (pp, name, m->name.len);
    m->nparams = -1;
    m->special = (guint8) special;
    if (!special) {
        m->body = g_new (FxPreTok, 1);
        m->body[0] = fxpre_number (pp, value);
        m->nbody = 1;
    }
    g_ptr_array_add (pp->all, m);
    g_hash_table_replace (pp->macros, GUINT_TO_POINTER ((guint) m->name.sym), m);
}

/* the value of bit `bit` of what cannot be known (a new one when bit ==
 * pp->unknown; name: the name it stands for, NULL for a condition) */
static int fxpre_unknown_bit (FxPre *pp, int bit, const FxPreTok *name)
{
    if (bit >= FXPRE_MAX_UNKNOWN) {
        if (!pp->fail)
            pp->fail = "more than 6 conditions that only the driver can answer (its own names, the line number, "
                       "arithmetic beyond 32 bits)";
        return 0;
    }
    if (bit == pp->unknown) {
        pp->unknown_name[bit] = name ? *name : (FxPreTok) { "", 0, 0, 0, 0, 0 };
        pp->unknown++;
    }
    return (pp->conf.forced >> bit) & 1;
}

/* a name only the driver can define: reserved for it by the language */
static inline gboolean fxpre_driver_name (const FxPreTok *t)
{
    return t->len > 2 && ((t->s[0] == 'G' && t->s[1] == 'L' && t->s[2] == '_') || (t->s[0] == '_' && t->s[1] == '_'));
}

/* is the name defined (for "defined", #ifdef), and with what value when
 * the text has no macro for it (an identifier left in an #if) */
static gboolean fxpre_defined (FxPre *pp, const FxPreTok *name, gint64 *value)
{
    *value = 0;
    const FxPreMacro *m = fxpre_lookup (pp, name);
    /* (__LINE__ and __FILE__ have their values here, but whether "defined"
     * holds for them is the driver's: yes in C, no in Mesa's preprocessor) */
    if (m && m->special != 1 && m->special != 2)
        return TRUE;
    if (!fxpre_driver_name (name) || fxpre_is (name, "GL_ES"))
        return m != NULL;   /* (GL_ES: a macro of the table after "#version ... es", else not defined) */
    if (m && pp->conf.trust_line)
        return FALSE;
    int r = -1;
    if (pp->conf.probe && name->len < 128) {
        char nm[128];
        memcpy (nm, name->s, name->len);
        nm[name->len] = 0;
        r = pp->conf.probe (nm, value, pp->conf.probe_user);
    }
    if (r < 0) {
        int bit = 0;
        while (bit < pp->unknown && !(pp->unknown_name[bit].len && fxpre_same (&pp->unknown_name[bit], name)))
            bit++;
        r = fxpre_unknown_bit (pp, bit, name);
        *value = r;
    }
    return r > 0;
}

/* "#define" (toks: what follows the word) */
static void fxpre_define (FxPre *pp, const FxPreTok *toks, guint n)
{
    if (n < 1 || toks[0].kind != FXPRE_T_IDENT)
        return;
    FxPreMacro *m = g_new0 (FxPreMacro, 1);
    m->name = toks[0];
    m->nparams = -1;
    guint i = 1;
    /* a parameter list begins right behind the name, without a blank */
    if (n > 1 && toks[1].len == 1 && toks[1].s[0] == '(' && toks[1].s == toks[0].s + toks[0].len) {
        guint np = 0, j = 2;
        gboolean ok = FALSE;
        if (j < n && toks[j].len == 1 && toks[j].s[0] == ')') {
            ok = TRUE;
            j++;
        } else {
            for (; j + 1 < n && toks[j].kind == FXPRE_T_IDENT; j += 2) {
                np++;
                if (toks[j + 1].len == 1 && toks[j + 1].s[0] == ')') {
                    ok = TRUE;
                    j += 2;
                    break;
                }
                if (!(toks[j + 1].len == 1 && toks[j + 1].s[0] == ','))
                    break;
            }
        }
        if (!ok) {
            /* no definition a compiler takes (the compile ends at this
             * line): the name stays what it was */
            g_free (m);
            return;
        }
        m->nparams = (int) np;
        m->params = g_new (FxPreTok, MAX (np, 1));
        for (guint k = 0; k < np; k++)
            m->params[k] = toks[2 + 2 * k];
        i = j;
    }
    m->nbody = n - i;
    m->body = g_new (FxPreTok, MAX (m->nbody, 1));
    /* which parameter a name of the body is: by the names' numbers, the
     * first of two parameters of one name (any number of parameters, and
     * no body compared with each of them) */
    GHashTable *par_at = m->nparams > 0 ? g_hash_table_new (NULL, NULL) : NULL;
    for (int q = m->nparams - 1; q >= 0; q--)
        if (m->params[q].sym)
            g_hash_table_replace (par_at, GUINT_TO_POINTER ((guint) m->params[q].sym), GINT_TO_POINTER (q + 1));
    if (par_at)
        m->par = g_new0 (guint32, MAX (m->nbody, 1));
    for (guint k = 0; k < m->nbody; k++) {
        m->body[k] = toks[i + k];
        if (m->body[k].kind == FXPRE_T_PUNCT && fxpre_is (&m->body[k], "##"))
            m->body[k].kind = FXPRE_T_PASTE;
        if (par_at && m->body[k].kind == FXPRE_T_IDENT && m->body[k].sym)
            m->par[k] = (guint32) GPOINTER_TO_INT (g_hash_table_lookup (par_at, GUINT_TO_POINTER ((guint) m->body[k].sym)));
    }
    if (par_at)
        g_hash_table_unref (par_at);
    fxpre_charge (pp, m->nbody + 1);
    g_ptr_array_add (pp->all, m);
    g_hash_table_replace (pp->macros, GUINT_TO_POINTER ((guint) m->name.sym), m);
}

/* ----------------------------------------------------------- #if value */

/* binary operators by precedence (C's); 0 = none */
static int fxpre_binop (const FxPreTok *t, int *prec)
{
    static const struct { const char *s; int prec; } ops[] = {
        { "||", 1 }, { "&&", 2 }, { "|", 3 }, { "^", 4 }, { "&", 5 }, { "==", 6 }, { "!=", 6 },
        { "<", 7 }, { ">", 7 }, { "<=", 7 }, { ">=", 7 }, { "<<", 8 }, { ">>", 8 },
        { "+", 9 }, { "-", 9 }, { "*", 10 }, { "/", 10 }, { "%", 10 },
    };
    if (t->kind != FXPRE_T_PUNCT)
        return 0;
    for (guint i = 0; i < G_N_ELEMENTS (ops); i++)
        if (fxpre_is (t, ops[i].s)) {
            *prec = ops[i].prec;
            return (int) i + 1;
        }
    return 0;
}

/* A value of an #if expression. odd: it is not the same for every
 * preprocessor - it does not fit 32 bits, or came out of a shift by more
 * than 31 or of a negative number, or of a division by zero. Mesa 25.2
 * takes "1 << 32" for 0 and ends the compile at "1 / 0" even behind
 * "1 ||"; a preprocessor that follows C has 4294967296 and 1. A condition
 * with such a value in it is not answered here (FxPreConf.forced). */
typedef struct { gint64 v; gboolean odd; } FxPreVal;

enum { FXPRE_OP_LPAREN = 100, FXPRE_OP_NEG, FXPRE_OP_POS, FXPRE_OP_NOT, FXPRE_OP_INV, FXPRE_OP_QUEST, FXPRE_OP_COLON };

static inline FxPreVal fxpre_val (gint64 v, gboolean odd)
{
    FxPreVal r = { v, odd || v > G_MAXINT32 || v < G_MININT32 };
    if (r.odd)
        r.v = 0;
    return r;
}

static FxPreVal fxpre_apply (int op, FxPreVal x, FxPreVal y)
{
    const gint64 a = x.v, b = y.v;
    gboolean odd = x.odd || y.odd;
    gint64 v = 0;
    switch (op) {
    case 1: v = a || b; break;
    case 2: v = a && b; break;
    case 3: v = a | b; break;
    case 4: v = a ^ b; break;
    case 5: v = a & b; break;
    case 6: v = a == b; break;
    case 7: v = a != b; break;
    case 8: v = a < b; break;
    case 9: v = a > b; break;
    case 10: v = a <= b; break;
    case 11: v = a >= b; break;
    case 12:
    case 13:
        if (b < 0 || b > 31 || a < 0)
            odd = TRUE;
        else
            v = op == 12 ? a << b : a >> b;     /* (a below 2^31: no overflow in 64 bits) */
        break;
    case 14: v = a + b; break;
    case 15: v = a - b; break;
    case 16: v = a * b; break;
    case 17:
    case 18:
        if (b == 0)
            odd = TRUE;
        else
            v = op == 17 ? a / b : a % b;
        break;
    }
    return fxpre_val (v, odd);
}

/* the operators on top of the stack applied, down to a '(' or to the first
 * one that `stop` holds for: stop > 0: one that binds less than stop;
 * stop == 0: none (to the '('); stop == -1: a '?'. FALSE: no expression. */
static gboolean fxpre_reduce (GArray *vals, GArray *ops, int stop)
{
    while (ops->len) {
        const int *top = &g_array_index (ops, int, 2 * (ops->len - 1));
        const int op = top[0], prec = top[1];
        if (op == FXPRE_OP_LPAREN || (stop > 0 && prec < stop) || (stop == -1 && op == FXPRE_OP_QUEST))
            break;
        g_array_set_size (ops, ops->len - 1);
        if (op >= FXPRE_OP_NEG && op <= FXPRE_OP_INV) {
            if (vals->len < 1)
                return FALSE;
            FxPreVal *v = &g_array_index (vals, FxPreVal, vals->len - 1);
            *v = fxpre_val (op == FXPRE_OP_NEG ? -v->v : op == FXPRE_OP_NOT ? !v->v : op == FXPRE_OP_INV ? ~v->v : v->v, v->odd);
        } else if (op == FXPRE_OP_COLON) {
            /* "c ? a : b": Mesa's preprocessor has no such operator, C's has */
            if (vals->len < 3)
                return FALSE;
            g_array_set_size (vals, vals->len - 2);
            g_array_index (vals, FxPreVal, vals->len - 1) = fxpre_val (0, TRUE);
        } else if (op == FXPRE_OP_QUEST) {
            return FALSE;
        } else {
            if (vals->len < 2)
                return FALSE;
            const FxPreVal y = g_array_index (vals, FxPreVal, vals->len - 1);
            g_array_set_size (vals, vals->len - 1);
            FxPreVal *x = &g_array_index (vals, FxPreVal, vals->len - 1);
            *x = fxpre_apply (op, *x, y);
        }
    }
    return TRUE;
}

/* The value of an #if expression (macros expanded, defined answered).
 * Operator precedence with two stacks: no recursion, whatever the nesting
 * (Mesa takes 3000 parentheses in an #if). Unary + - ! ~, the binary
 * operators above, ( ). *odd: the value is not every preprocessor's, or
 * this is no expression (one preprocessor ends the compile there, another
 * may make something of it). */
static gint64 fxpre_if_value (FxPre *pp, const FxPreTok *t, guint n, gboolean *odd)
{
    GArray *vals = g_array_new (FALSE, FALSE, sizeof (FxPreVal));
    GArray *ops = g_array_new (FALSE, FALSE, sizeof (int) * 2);    /* { op, how tightly it binds } */
    gboolean want_value = TRUE, ok = TRUE;
    for (guint i = 0; i < n && ok; i++) {
        const FxPreTok *k = &t[i];
        fxpre_charge (pp, 1);
        int op[2] = { 0, 20 };
        if (want_value) {
            if (k->kind == FXPRE_T_NUM) {
                char tmp[40];
                ok = k->len < sizeof (tmp);
                if (!ok)
                    break;
                memcpy (tmp, k->s, k->len);
                tmp[k->len] = 0;
                char *end = NULL;
                const guint64 u = g_ascii_strtoull (tmp, &end, 0);
                if (*end == 'u' || *end == 'U')
                    end++;
                ok = !*end;
                FxPreVal v = fxpre_val ((gint64) MIN (u, (guint64) G_MAXINT64), FALSE);
                g_array_append_val (vals, v);
                want_value = FALSE;
            } else if (k->kind == FXPRE_T_IDENT) {
                /* a name no macro stands behind: 0 (GLSL ES: the compile
                 * ends); one only the driver knows: what it has for it */
                gint64 dv = 0;
                fxpre_defined (pp, k, &dv);
                FxPreVal v = fxpre_val (dv, FALSE);
                g_array_append_val (vals, v);
                want_value = FALSE;
            } else if (fxpre_is (k, "(")) {
                op[0] = FXPRE_OP_LPAREN;
                g_array_append_val (ops, op);
            } else if (fxpre_is (k, "-") || fxpre_is (k, "+") || fxpre_is (k, "!") || fxpre_is (k, "~")) {
                op[0] = k->s[0] == '-' ? FXPRE_OP_NEG : k->s[0] == '+' ? FXPRE_OP_POS : k->s[0] == '!' ? FXPRE_OP_NOT : FXPRE_OP_INV;
                g_array_append_val (ops, op);
            } else {
                ok = FALSE;
            }
            continue;
        }
        int prec = 0;
        const int bop = fxpre_binop (k, &prec);
        if (bop) {
            ok = fxpre_reduce (vals, ops, prec);
            op[0] = bop;
            op[1] = prec;
            g_array_append_val (ops, op);
            want_value = TRUE;
        } else if (k->kind == FXPRE_T_PUNCT && fxpre_is (k, ")")) {
            ok = fxpre_reduce (vals, ops, 0) && ops->len;
            if (ok)
                g_array_set_size (ops, ops->len - 1);
        } else if (k->kind == FXPRE_T_PUNCT && fxpre_is (k, "?")) {
            ok = fxpre_reduce (vals, ops, 1);
            op[0] = FXPRE_OP_QUEST;
            op[1] = -1;
            g_array_append_val (ops, op);
            want_value = TRUE;
        } else if (k->kind == FXPRE_T_PUNCT && fxpre_is (k, ":")) {
            ok = fxpre_reduce (vals, ops, -1) && ops->len &&
                 g_array_index (ops, int, 2 * (ops->len - 1)) == FXPRE_OP_QUEST;
            if (ok) {
                g_array_index (ops, int, 2 * (ops->len - 1)) = FXPRE_OP_COLON;
                g_array_index (ops, int, 2 * (ops->len - 1) + 1) = 0;
            }
            want_value = TRUE;
        } else {
            ok = FALSE;
        }
    }
    ok = ok && !want_value && fxpre_reduce (vals, ops, 0) && !ops->len && vals->len == 1 &&
         !g_array_index (vals, FxPreVal, 0).odd;
    const gint64 v = ok ? g_array_index (vals, FxPreVal, 0).v : 0;
    g_array_free (vals, TRUE);
    g_array_free (ops, TRUE);
    *odd = !ok;
    return v;
}

/* ----------------------------------------------------------- expansion */

/* where an expansion reads from: a stack of token lists, the lines of the
 * text below them (fxpre_next_line) when it is the text that is expanded */
typedef struct {
    FxPre *pp;
    GArray *frames;     /* FxPreFrame */
    gboolean text;      /* below the frames: the following lines */
    gboolean in_if;     /* an #if line: "defined NAME" is answered, not expanded */
} FxPreIn;

static gboolean fxpre_next_line (FxPre *pp, GArray *toks);

static void fxpre_pop (FxPreIn *in)
{
    FxPreFrame *f = &g_array_index (in->frames, FxPreFrame, in->frames->len - 1);
    if (f->mac)
        f->mac->disabled = 0;
    if (f->owned)
        g_free (f->t);
    g_array_set_size (in->frames, in->frames->len - 1);
}

/* the next token to read, or NULL at the end. Lists that are read to their
 * end go (and their macros may be expanded again); with `text`, the next
 * line follows the last one. */
static const FxPreTok *fxpre_peek (FxPreIn *in)
{
    for (;;) {
        while (in->frames->len) {
            FxPreFrame *f = &g_array_index (in->frames, FxPreFrame, in->frames->len - 1);
            if (f->i < f->n)
                return &f->t[f->i];
            if (in->text && in->frames->len == 1)
                break;      /* the line: filled again below */
            fxpre_pop (in);
        }
        if (!in->text || in->pp->fail)
            return NULL;
        FxPre *pp = in->pp;
        if (!fxpre_next_line (pp, pp->linebuf))
            return NULL;
        if (!in->frames->len) {
            FxPreFrame f = { NULL, 0, 0, NULL, FALSE };
            g_array_append_val (in->frames, f);
        }
        FxPreFrame *f = &g_array_index (in->frames, FxPreFrame, 0);
        f->t = (FxPreTok *) pp->linebuf->data;
        f->n = pp->linebuf->len;
        f->i = 0;
    }
}

static inline void fxpre_take (FxPreIn *in)
{
    g_array_index (in->frames, FxPreFrame, in->frames->len - 1).i++;
}

static void fxpre_expand (FxPreIn *in, GArray *out, gint64 max_out);

/* [t, t + n) with every macro in it expanded, into out (an argument before
 * it is put into the body) */
static void fxpre_expand_list (FxPre *pp, const FxPreTok *t, guint n, GArray *out)
{
    if (pp->nest >= FXPRE_MAX_NEST) {
        if (!pp->fail)
            pp->fail = "macro calls nested more than 512 deep in each other's arguments";
        return;
    }
    pp->nest++;
    FxPreIn in = { pp, g_array_new (FALSE, FALSE, sizeof (FxPreFrame)), FALSE, FALSE };
    FxPreFrame f = { (FxPreTok *) t, n, 0, NULL, FALSE };
    g_array_append_val (in.frames, f);
    fxpre_expand (&in, out, pp->conf.max_out);
    while (in.frames->len)
        fxpre_pop (&in);
    g_array_free (in.frames, TRUE);
    pp->nest--;
}

/* the token two tokens make ("a ## b") */
static FxPreTok fxpre_paste (FxPre *pp, const FxPreTok *a, const FxPreTok *b)
{
    if (a->kind == FXPRE_T_MARK)
        return *b;
    if (b->kind == FXPRE_T_MARK)
        return *a;
    char *s = g_malloc (a->len + b->len + 1);
    memcpy (s, a->s, a->len);
    memcpy (s + a->len, b->s, b->len);
    s[a->len + b->len] = 0;
    FxPreTok t = { g_string_chunk_insert_len (pp->arena, s, (gssize) (a->len + b->len)), a->len + b->len, 0, FXPRE_T_PUNCT, 0, 0 };
    g_free (s);
    if (fxpre_ident_start (t.s[0])) {
        t.kind = FXPRE_T_IDENT;
        t.sym = fxpre_intern (pp, t.s, t.len);
        fxpre_charge (pp, t.len / 16);
    }
    else if (g_ascii_isdigit (t.s[0]) || (t.s[0] == '.' && t.len > 1 && g_ascii_isdigit (t.s[1])))
        t.kind = FXPRE_T_NUM;
    return t;
}

/* the "##" of a body applied, left to right; the marks of empty arguments go */
static void fxpre_paste_all (FxPre *pp, GArray *r)
{
    guint w = 0;
    FxPreTok *t = (FxPreTok *) r->data;
    for (guint i = 0; i < r->len; i++) {
        if (t[i].kind == FXPRE_T_PASTE && w > 0 && i + 1 < r->len) {
            t[w - 1] = fxpre_paste (pp, &t[w - 1], &t[i + 1]);
            i++;
        } else {
            t[w++] = t[i];
        }
    }
    guint k = 0;
    for (guint i = 0; i < w; i++)
        if (t[i].kind != FXPRE_T_MARK) {
            t[k] = t[i];
            if (t[k].kind == FXPRE_T_PASTE)     /* at an end of the body: pastes nothing */
                t[k].kind = FXPRE_T_PUNCT;
            k++;
        }
    g_array_set_size (r, k);
}

/* The macro `m`, named by the token just taken from `in`, is called: its
 * replacement goes on the stack of `in`. FALSE: the name stands without a
 * call (a function-like macro with no '(' behind it) and is plain text. */
static gboolean fxpre_call (FxPreIn *in, FxPreMacro *m)
{
    FxPre *pp = in->pp;
    GArray *r = g_array_new (FALSE, FALSE, sizeof (FxPreTok));
    gboolean has_paste = FALSE;
    if (m->special) {
        if (m->special != 3 && pp->in_cond && !pp->conf.trust_line)
            pp->cond_line = TRUE;
        FxPreTok t = fxpre_number (pp, m->special == 1 ? pp->line - 1 : m->special == 2 ? pp->file : pp->version);
        g_array_append_val (r, t);
    } else if (m->nparams < 0) {
        g_array_append_vals (r, m->body, m->nbody);
        for (guint i = 0; i < m->nbody && !has_paste; i++)
            has_paste = m->body[i].kind == FXPRE_T_PASTE;
        fxpre_charge (pp, m->nbody);
    } else {
        const FxPreTok *k = fxpre_peek (in);
        if (!k || !(k->kind == FXPRE_T_PUNCT && k->len == 1 && k->s[0] == '(')) {
            g_array_free (r, TRUE);
            return FALSE;
        }
        fxpre_take (in);
        /* the arguments: to the ')' of this '(', cut at the commas outside
         * every other pair of parentheses */
        GArray *args = g_array_new (FALSE, FALSE, sizeof (FxPreTok));
        GArray *ends = g_array_new (FALSE, FALSE, sizeof (guint));
        const guint directives = pp->directives;
        int depth = 0;
        gboolean closed = FALSE;
        while ((k = fxpre_peek (in)) != NULL && fxpre_charge (pp, 1)) {
            FxPreTok a = *k;
            fxpre_take (in);
            if (a.kind == FXPRE_T_PUNCT && a.len == 1) {
                if (a.s[0] == '(') {
                    depth++;
                } else if (a.s[0] == ')' && depth-- == 0) {
                    closed = TRUE;
                    break;
                } else if (a.s[0] == ',' && depth == 0) {
                    g_array_append_val (ends, args->len);
                    continue;
                }
            }
            g_array_append_val (args, a);
        }
        g_array_append_val (ends, args->len);
        guint na = ends->len;
        if (na == 1 && args->len == 0 && m->nparams == 0)
            na = 0;
        if (closed && (int) na == m->nparams && !pp->fail) {
            /* every argument expanded once, on its own */
            GArray *exp = g_array_new (FALSE, FALSE, sizeof (FxPreTok));
            GArray *exp_ends = g_array_new (FALSE, FALSE, sizeof (guint));
            for (guint i = 0; i < na && !pp->fail; i++) {
                const guint b = i ? g_array_index (ends, guint, i - 1) : 0, e = g_array_index (ends, guint, i);
                fxpre_expand_list (pp, &g_array_index (args, FxPreTok, b), e - b, exp);
                g_array_append_val (exp_ends, exp->len);
            }
            for (guint j = 0; j < m->nbody && !pp->fail; j++) {
                const FxPreTok *bt = &m->body[j];
                const int pi = m->par ? (int) m->par[j] - 1 : -1;
                if (pi < 0) {
                    g_array_append_val (r, *bt);
                    has_paste = has_paste || bt->kind == FXPRE_T_PASTE;
                    fxpre_charge (pp, 1);
                    continue;
                }
                const gboolean beside_paste = (j > 0 && m->body[j - 1].kind == FXPRE_T_PASTE) ||
                                              (j + 1 < m->nbody && m->body[j + 1].kind == FXPRE_T_PASTE);
                const GArray *from = beside_paste ? args : exp, *fe = beside_paste ? ends : exp_ends;
                const guint b = pi ? g_array_index (fe, guint, pi - 1) : 0, e = g_array_index (fe, guint, pi);
                if (e == b && beside_paste) {
                    FxPreTok mark = { "", 0, 0, FXPRE_T_MARK, 0, 0 };
                    g_array_append_val (r, mark);
                } else if (fxpre_charge (pp, e - b)) {
                    g_array_append_vals (r, &g_array_index (from, FxPreTok, b), e - b);
                }
            }
            g_array_free (exp, TRUE);
            g_array_free (exp_ends, TRUE);
        } else if (!pp->fail) {
            /* No call a compiler takes (no ')', another number of
             * arguments): the compile ends, but the preprocessing does
             * not. Mesa's says so, leaves the name as the text it is and
             * reads on behind it: what was taken for the arguments is
             * text like any other, and the macros in it are expanded
             * ("#define PAIR(p, q) p" and "PAIR(BIG)": all of BIG). So the
             * name stays and the rest is read once more.
             * Where the search for the ')' went over directive lines they
             * have been obeyed, and the text before them cannot be read
             * again as it was (an "#undef" among them): no shader. */
            static const FxPreTok open = { "(", 1, 0, FXPRE_T_PUNCT, 0, 0 }, comma = { ",", 1, 0, FXPRE_T_PUNCT, 0, 0 },
                               close = { ")", 1, 0, FXPRE_T_PUNCT, 0, 0 };
            if (pp->directives != directives)
                pp->fail = "a macro call without its ')' or with another number of arguments, over directive lines";
            FxPreTok name = m->name;
            name.noexp = 1;
            g_array_append_val (r, name);
            g_array_append_val (r, open);
            for (guint i = 0; i < ends->len; i++) {
                const guint b = i ? g_array_index (ends, guint, i - 1) : 0, e = g_array_index (ends, guint, i);
                if (i)
                    g_array_append_val (r, comma);
                g_array_append_vals (r, &g_array_index (args, FxPreTok, b), e - b);
            }
            if (closed)
                g_array_append_val (r, close);
            fxpre_charge (pp, r->len);
            g_array_free (args, TRUE);
            g_array_free (ends, TRUE);
            FxPreFrame f = { NULL, r->len, 0, NULL, TRUE };
            f.t = (FxPreTok *) g_array_free (r, FALSE);
            g_array_append_val (in->frames, f);
            return TRUE;
        }
        g_array_free (args, TRUE);
        g_array_free (ends, TRUE);
    }
    if (has_paste)
        fxpre_paste_all (pp, r);
    m->disabled = 1;
    FxPreFrame f = { NULL, r->len, 0, m, TRUE };
    f.t = (FxPreTok *) g_array_free (r, FALSE);
    g_array_append_val (in->frames, f);
    return TRUE;
}

/* everything `in` has to give, macros expanded, into out */
static void fxpre_expand (FxPreIn *in, GArray *out, gint64 max_out)
{
    FxPre *pp = in->pp;
    const FxPreTok *k;
    /* (a token costs a preprocessor more the more macros are open around
     * it: Mesa 25.2 needs 3 s for 20000 macros that each name the next) */
    while ((k = fxpre_peek (in)) != NULL && fxpre_charge (pp, 1 + in->frames->len / 4)) {
        FxPreTok t = *k;
        fxpre_take (in);
        if (t.kind == FXPRE_T_IDENT && !t.noexp) {
            if (in->in_if && fxpre_is (&t, "defined")) {
                /* "defined NAME", "defined ( NAME )": answered here, the
                 * name is not expanded */
                const FxPreTok *a = fxpre_peek (in);
                gboolean paren = a && a->kind == FXPRE_T_PUNCT && a->len == 1 && a->s[0] == '(';
                if (paren) {
                    fxpre_take (in);
                    a = fxpre_peek (in);
                }
                gint64 v = 0;
                if (a && a->kind == FXPRE_T_IDENT) {
                    FxPreTok name = *a;
                    fxpre_take (in);
                    v = fxpre_defined (pp, &name, &v) ? 1 : 0;
                    a = fxpre_peek (in);
                    if (paren && a && a->kind == FXPRE_T_PUNCT && a->len == 1 && a->s[0] == ')')
                        fxpre_take (in);
                }
                t = fxpre_number (pp, v);
                g_array_append_val (out, t);
                continue;
            }
            FxPreMacro *m = fxpre_lookup (pp, &t);
            if (m && m->disabled) {
                t.noexp = 1;
            } else if (m && fxpre_call (in, m)) {
                continue;
            }
        }
        if ((gint64) out->len >= max_out) {
            if (!pp->fail)
                pp->fail = "too many tokens after preprocessing (every macro replaced)";
            return;
        }
        pp->bytes += (gint64) t.len + 1;
        if (pp->bytes > pp->conf.max_bytes) {
            if (!pp->fail)
                pp->fail = "too much text after preprocessing (every macro replaced)";
            return;
        }
        g_array_append_val (out, t);
    }
}

/* ------------------------------------------------------------ the lines */

static gboolean fxpre_active (FxPre *pp)
{
    return !pp->ifs->len || g_array_index (pp->ifs, FxPreIf, pp->ifs->len - 1).active;
}

/* the value of the #if / #elif line whose expression is [t, t + n) */
static gboolean fxpre_condition (FxPre *pp, const FxPreTok *t, guint n)
{
    GArray *e = g_array_new (FALSE, FALSE, sizeof (FxPreTok));
    FxPreIn in = { pp, g_array_new (FALSE, FALSE, sizeof (FxPreFrame)), FALSE, TRUE };
    FxPreFrame f = { (FxPreTok *) t, n, 0, NULL, FALSE };
    g_array_append_val (in.frames, f);
    pp->in_cond = TRUE;
    pp->cond_line = FALSE;
    fxpre_expand (&in, e, pp->conf.max_out);
    while (in.frames->len)
        fxpre_pop (&in);
    g_array_free (in.frames, TRUE);
    gboolean odd = FALSE;
    gint64 v = fxpre_if_value (pp, (const FxPreTok *) e->data, e->len, &odd);
    g_array_free (e, TRUE);
    pp->in_cond = FALSE;
    pp->odd += odd;
    if (pp->cond_line || (odd && !pp->conf.trust_line))
        return fxpre_unknown_bit (pp, pp->unknown, NULL) != 0;
    return !odd && v != 0;
}

/* a directive: toks[0] is the '#' */
static void fxpre_directive (FxPre *pp, const FxPreTok *toks, guint n)
{
    if (n < 2 || toks[1].kind != FXPRE_T_IDENT)
        return;
    const FxPreTok *w = &toks[1], *rest = toks + 2;
    const guint nr = n - 2;
    const gboolean active = fxpre_active (pp);
    if (fxpre_is (w, "if") || fxpre_is (w, "ifdef") || fxpre_is (w, "ifndef")) {
        FxPreIf f = { 0, 0, (guint8) active, 0 };
        if (active) {
            gboolean v;
            gint64 unused;
            if (fxpre_is (w, "if"))
                v = fxpre_condition (pp, rest, nr);
            else
                v = (nr > 0 && rest[0].kind == FXPRE_T_IDENT && fxpre_defined (pp, &rest[0], &unused)) == fxpre_is (w, "ifdef");
            f.active = f.taken = (guint8) v;
        }
        g_array_append_val (pp->ifs, f);
    } else if (fxpre_is (w, "elif") || fxpre_is (w, "else")) {
        if (!pp->ifs->len)
            return;
        FxPreIf *f = &g_array_index (pp->ifs, FxPreIf, pp->ifs->len - 1);
        if (f->seen_else) {
            /* behind the group's "#else": the compile ends, the
             * preprocessing does not, and Mesa's leaves the group as it
             * was - an "#else" branch that is compiled stays compiled
             * behind a second "#else" or an "#elif", to the "#endif" */
        } else if (!f->parent || f->taken) {
            f->active = 0;
        } else {
            f->active = f->taken = (guint8) (fxpre_is (w, "else") || fxpre_condition (pp, rest, nr));
        }
        if (fxpre_is (w, "else"))
            f->seen_else = 1;
    } else if (fxpre_is (w, "endif")) {
        if (pp->ifs->len)
            g_array_set_size (pp->ifs, pp->ifs->len - 1);
    } else if (!active) {
        return;
    } else if (fxpre_is (w, "define")) {
        fxpre_define (pp, rest, nr);
    } else if (fxpre_is (w, "undef")) {
        if (nr > 0 && rest[0].kind == FXPRE_T_IDENT)
            g_hash_table_remove (pp->macros, GUINT_TO_POINTER ((guint) rest[0].sym));
    } else if (fxpre_is (w, "line")) {
        /* "#line N" / "#line N F" (macros expanded): the next line is N */
        GArray *e = g_array_new (FALSE, FALSE, sizeof (FxPreTok));
        fxpre_expand_list (pp, rest, nr, e);
        gint64 v[2] = { -1, -1 };
        for (guint i = 0; i < e->len && i < 2; i++) {
            const FxPreTok *k = &g_array_index (e, FxPreTok, i);
            if (k->kind == FXPRE_T_NUM && k->len < 20) {
                char tmp[24];
                memcpy (tmp, k->s, k->len);
                tmp[k->len] = 0;
                v[i] = (gint64) g_ascii_strtoull (tmp, NULL, 0);
            }
        }
        if (v[0] >= 0)
            pp->line = v[0];
        if (v[1] >= 0)
            pp->file = v[1];
        g_array_free (e, TRUE);
    } else if (fxpre_is (w, "version")) {
        /* the language: __VERSION__ and GL_ES follow it - where the
         * driver takes the line, which is as the first thing of the
         * source (blank lines and comments before it). Behind anything
         * else it is an error and Mesa's preprocessor keeps the version
         * it has: 110, or what the first line said (the engine's, in a
         * built source: a "#version 300 es" in the wallpaper's text does
         * not define GL_ES). "#version 100" is GLSL ES 1.00. */
        if (!pp->begun && nr > 0 && rest[0].kind == FXPRE_T_NUM && rest[0].len < 8) {
            char tmp[8];
            memcpy (tmp, rest[0].s, rest[0].len);
            tmp[rest[0].len] = 0;
            pp->version = atoi (tmp);
            pp->es = pp->version == 100 || (nr > 1 && fxpre_is (&rest[1], "es"));
            if (pp->es)
                fxpre_predefine (pp, "GL_ES", 0, 1);
        }
    }
    /* #extension, #pragma, #error, #include and the rest: no tokens */
}

/* The tokens of the next line of text that is compiled, into toks (emptied
 * first); directives on the way are obeyed. FALSE at the end. pp->line is
 * then the number of the line behind the one returned. */
static gboolean fxpre_next_line (FxPre *pp, GArray *toks)
{
    while (*pp->p && !pp->fail) {
        g_array_set_size (toks, 0);
        const char *p = pp->p;
        gboolean skip = FALSE;
        while (*p && *p != '\n') {
            if (*p == ' ' || *p == '\t' || *p == '\v' || *p == '\f' || *p == '\r') {
                p++;
                continue;
            }
            if (skip) {
                /* a line that is not compiled and is no directive */
                p += strcspn (p, "\n");
                break;
            }
            FxPreTok t = fxpre_lex (pp, &p);
            if (!toks->len && !(t.len == 1 && t.s[0] == '#') && !fxpre_active (pp)) {
                skip = TRUE;
                continue;
            }
            g_array_append_val (toks, t);
        }
        pp->p = *p ? p + 1 : p;
        pp->line++;
        if (!fxpre_charge (pp, toks->len + 1))
            break;
        if (toks->len && g_array_index (toks, FxPreTok, 0).len == 1 && g_array_index (toks, FxPreTok, 0).s[0] == '#') {
            pp->directives++;
            fxpre_directive (pp, (const FxPreTok *) toks->data, toks->len);
            pp->begun = TRUE;
            continue;
        }
        if (toks->len && !skip) {
            pp->begun = TRUE;
            return TRUE;
        }
    }
    g_array_set_size (toks, 0);
    return FALSE;
}

/* ---------------------------------------------------------------- entry */

static void fx_pre_clear (FxPre *pp)
{
    if (pp->macros)
        g_hash_table_unref (pp->macros);
    if (pp->names)
        g_hash_table_unref (pp->names);
    if (pp->all)
        g_ptr_array_free (pp->all, TRUE);
    if (pp->ifs)
        g_array_free (pp->ifs, TRUE);
    if (pp->out)
        g_array_free (pp->out, TRUE);
    if (pp->linebuf)
        g_array_free (pp->linebuf, TRUE);
    if (pp->arena)
        g_string_chunk_free (pp->arena);
    g_free (pp->buf);
    memset (pp, 0, sizeof (*pp));
}

/* Preprocess src. The tokens are in pp->out afterwards (they point into
 * pp->buf and pp->arena: fx_pre_clear () ends them). FALSE: stopped at a
 * bound, pp->fail says which, pp->out holds what there was until then. */
static gboolean fx_pre_run (FxPre *pp, const char *src, const FxPreConf *conf)
{
    memset (pp, 0, sizeof (*pp));
    pp->conf = *conf;
    pp->buf = fxpre_clean (src);
    pp->p = pp->buf;
    pp->line = 1;
    pp->version = 110;
    pp->macros = g_hash_table_new (NULL, NULL);
    pp->names = g_hash_table_new_full (fxpre_name_hash, fxpre_name_equal, g_free, NULL);
    pp->all = g_ptr_array_new_with_free_func (fxpre_macro_free);
    pp->ifs = g_array_new (FALSE, FALSE, sizeof (FxPreIf));
    pp->arena = g_string_chunk_new (256);
    pp->out = g_array_new (FALSE, FALSE, sizeof (FxPreTok));
    pp->linebuf = g_array_new (FALSE, FALSE, sizeof (FxPreTok));
    fxpre_predefine (pp, "__LINE__", 1, 0);
    fxpre_predefine (pp, "__FILE__", 2, 0);
    fxpre_predefine (pp, "__VERSION__", 3, 0);
    FxPreIn in = { pp, g_array_new (FALSE, FALSE, sizeof (FxPreFrame)), TRUE, FALSE };
    fxpre_expand (&in, pp->out, pp->conf.max_out);
    while (in.frames->len) {
        /* (the bottom one is the line buffer: not this stack's to free) */
        FxPreFrame *f = &g_array_index (in.frames, FxPreFrame, in.frames->len - 1);
        if (in.frames->len == 1)
            f->owned = FALSE;
        fxpre_pop (&in);
    }
    g_array_free (in.frames, TRUE);
    return !pp->fail;
}

/* The next combination of what the last run could not know (it used
 * `used` bits of *forced): FALSE when that was the last one. Start with
 * *forced = 0. The runs are the leaves of a tree: which names and
 * conditions a run meets depends on the bits before them. */
static gboolean fx_pre_next (guint32 *forced, int used)
{
    for (int bit = used - 1; bit >= 0; bit--) {
        if (!((*forced >> bit) & 1)) {
            *forced = (*forced | (1u << bit)) & ((2u << bit) - 1);
            return TRUE;
        }
    }
    return FALSE;
}

#endif /* WALPE_FX_PREPROC_H */
