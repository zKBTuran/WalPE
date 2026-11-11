/* fx_srcrepair.h - when and where the HLSL-semantics shader repairs run.
 *
 * Included by scenenode_effects.c, after the line rewriters (fx_fix_*,
 * fx_expr_end, fx_operand_*, fx_uint_to_int): everything here is static
 * and reachable from tests/fx_repair_test.c through that include.
 *
 * Until version 0.12 a repair ran only when the driver's compile/link log
 * matched a known wording (Mesa's, later NVIDIA's compile messages too).
 * The link-stage repair (a fragment input narrower than the vertex
 * output) knew Mesa's sentence only, so on NVIDIA the chain failed to
 * build. Now:
 *
 *  1. fx_prepass(): BEFORE the first compile, the source itself decides.
 *     A small preprocessor model tells which lines are compiled (the
 *     combos are #defines at the top of the built source), a scope-aware
 *     symbol table and an expression type inferencer tell what each
 *     statement mixes. Repairs: stray #endif/#else/#elif, a uniform (or
 *     float) in an #if condition, vertex/fragment varying width mismatch,
 *     scalar<->vector initializers / returns / assignments / compound
 *     assignments, bool used as a number, float '%', mixed-size arguments
 *     of the componentwise builtins, vec3/vec4 texture coordinates,
 *     int/uint mixing. Whatever the inferencer cannot type stays as it is.
 *     No driver is asked, so every driver gets the same source.
 *
 *  2. fx_repair_by_lines(): only if a shader still does not compile. It
 *     takes nothing from the log but the LINE NUMBERS (every GLSL compiler
 *     prints "<string>:<line>" or "<string>(<line>)"), derives candidate
 *     rewrites of that line from the source, and keeps a candidate when
 *     the driver no longer reports that line. No message text is matched.
 *
 * Debug / test switches (no effect unless set):
 *   WALPE_FX_NO_PREPASS=1          skip stage 1 (exercises stage 2 alone)
 *   WALPE_FX_LOG_AS=nvidia|glslang|none
 *                                  hand stage 2 the driver log re-worded
 *                                  like another driver's, or no log at all
 *                                  ("none": an unknown driver whose log
 *                                  cannot be read - only stage 1 is left)
 */

/* GLSL ES output (fx_essl.h, included behind this file). fx_essl () is 0
 * for GLSL 3.30 - desktop OpenGL - and then nothing of it runs. */
static int fx_essl (void);
static int fx_essl_src (char **src);

/* ------------------------------------------------------------------ types */

typedef enum { FXB_UNK = 0, FXB_VOID, FXB_BOOL, FXB_INT, FXB_UINT, FXB_FLOAT, FXB_MAT,
               FXB_OTHER } FxBase;
/* b = base, n = components (matrices: dimension), arr = array of it */
typedef struct { guint8 b, n, arr; } FxTy;

static const FxTy fx_ty_unk = { FXB_UNK, 0, 0 };

static FxTy fx_ty (guint8 b, guint8 n)
{
    FxTy t = { b, n, 0 };
    return t;
}

static gboolean fx_is_ident (char c)
{
    return g_ascii_isalnum (c) || c == '_';
}

static gboolean fx_is_ident_start (char c)
{
    return g_ascii_isalpha (c) || c == '_';
}

static const char *fx_sp (const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static gboolean fx_ty_parse (const char *s, gsize len, FxTy *t)
{
    static const struct { const char *nm; guint8 b, n; } tab[] = {
        { "void", FXB_VOID, 1 }, { "bool", FXB_BOOL, 1 }, { "int", FXB_INT, 1 },
        { "uint", FXB_UINT, 1 }, { "float", FXB_FLOAT, 1 },
        { "vec2", FXB_FLOAT, 2 }, { "vec3", FXB_FLOAT, 3 }, { "vec4", FXB_FLOAT, 4 },
        { "ivec2", FXB_INT, 2 }, { "ivec3", FXB_INT, 3 }, { "ivec4", FXB_INT, 4 },
        { "uvec2", FXB_UINT, 2 }, { "uvec3", FXB_UINT, 3 }, { "uvec4", FXB_UINT, 4 },
        { "bvec2", FXB_BOOL, 2 }, { "bvec3", FXB_BOOL, 3 }, { "bvec4", FXB_BOOL, 4 },
        { "mat2", FXB_MAT, 2 }, { "mat3", FXB_MAT, 3 }, { "mat4", FXB_MAT, 4 },
        /* the prelude's HLSL spellings */
        { "float2", FXB_FLOAT, 2 }, { "float3", FXB_FLOAT, 3 }, { "float4", FXB_FLOAT, 4 },
        { "int2", FXB_INT, 2 }, { "int3", FXB_INT, 3 }, { "int4", FXB_INT, 4 },
        { "float2x2", FXB_MAT, 2 }, { "float3x3", FXB_MAT, 3 }, { "float4x4", FXB_MAT, 4 },
        { "sampler2D", FXB_OTHER, 1 }, { "sampler3D", FXB_OTHER, 1 },
        { "samplerCube", FXB_OTHER, 1 }, { "sampler2DShadow", FXB_OTHER, 1 },
    };
    for (guint i = 0; i < G_N_ELEMENTS (tab); i++)
        if (strlen (tab[i].nm) == len && !strncmp (tab[i].nm, s, len)) {
            *t = fx_ty (tab[i].b, tab[i].n);
            return TRUE;
        }
    return FALSE;
}

/* constructor name of a scalar / vector number type, else NULL */
static const char *fx_ty_name (FxTy t)
{
    static const char *nm[3][4] = { { "int", "ivec2", "ivec3", "ivec4" },
                                    { "uint", "uvec2", "uvec3", "uvec4" },
                                    { "float", "vec2", "vec3", "vec4" } };
    if (t.arr || t.n < 1 || t.n > 4)
        return NULL;
    if (t.b == FXB_INT)
        return nm[0][t.n - 1];
    if (t.b == FXB_UINT)
        return nm[1][t.n - 1];
    if (t.b == FXB_FLOAT)
        return nm[2][t.n - 1];
    return NULL;
}

/* does "T x = <t>" need the T(...) constructor? (GLSL 3.30 converts
 * int -> float by itself, nothing else; HLSL also splats scalars,
 * truncates wider vectors and converts every number type) */
static gboolean fx_needs_cast (FxTy T, FxTy t)
{
    if (!fx_ty_name (T) || t.arr)
        return FALSE;
    if (t.b == FXB_BOOL && t.n == 1)
        return TRUE;
    if (t.b != FXB_INT && t.b != FXB_UINT && t.b != FXB_FLOAT)
        return FALSE;
    if (t.n == T.n)
        return t.b != T.b && T.b != FXB_FLOAT;
    return t.n == 1 || t.n > T.n;
}

/* ------------------------------------------------------------ preprocessor */

/* integer constant expression of an #if / #elif. bad: could not be
 * evaluated (the caller then assumes nothing about the branch) */
typedef struct {
    const char *p;
    GHashTable *macros;
    int depth;
    gboolean bad;
    int nest;   /* open parentheses / unary operators around the current term */
    int work;   /* macros expanded so far for this directive */
    gboolean hazard;   /* INT64_MIN / -1 or % -1 was evaluated: a trap in
                        * a preprocessor that divides as written */
} FxPp;

/* limits of the two source models below. A shader is wallpaper data: one
 * line of 100000 '(' overflowed the stack, and macros that name each
 * other 8 times over 8 levels took hours to expand. Past a limit the
 * expression counts as "could not be evaluated / typed": nothing is
 * assumed and nothing is rewritten, as for any other unknown construct. */
#define FX_PP_MAX_NEST 64
#define FX_PP_MAX_WORK 256
#define FX_EX_MAX_NEST 256
#define FX_EX_MAX_WORK 256
/* the line rewriters look for the matching ')' of every '(' they meet:
 * quadratic in the line length. Real shader lines stay under 2000 bytes;
 * a longer line (a 400 KB expression took minutes) is compiled as written. */
#define FX_MAX_REPAIR_LINE 8192
/* The repairs keep the symbols of a source in lists, and the interface
 * repairs (varyings, fragment inputs, uniforms of two types) go through
 * the source once per declaration they look at: the work grows with the
 * square of the size. The largest shader file of a Wallpaper Engine
 * install has 30 KiB, the largest Workshop ones some tens of KiB (see
 * FX_MAX_SHADER_FILE). A source of more than FX_MAX_REPAIR_SOURCE bytes is
 * compiled as written, like a line beyond FX_MAX_REPAIR_LINE: 1 MiB of
 * varying declarations in each stage - one shader file may be that large -
 * took 20 s of stage 1 in an optimised build, and the 4 MiB a shader may
 * have with its includes took minutes, for every pass that uses the
 * shader, on the render thread. At the limit it is about a second. */
#define FX_MAX_REPAIR_SOURCE (256 * 1024)

static gboolean fx_repairable (const char *src)
{
    return strnlen (src, FX_MAX_REPAIR_SOURCE + 1) <= FX_MAX_REPAIR_SOURCE;
}

/* ---- text work kept for one scene load (fx_text_memo_begin / _end)
 * A scene builds one pass after the other, and most passes are a shader
 * text that an earlier pass of the same scene already had (175 passes of
 * 46 texts in one scene of the test library). What is computed from the
 * text alone is computed once per text while a load keeps a memo: the
 * repairs of fx_prepass () with the messages they print, fx_active_code ()
 * and fx_expansion_ok (). Outside a load nothing is kept. Per thread. */
typedef struct {
    GLogLevelFlags level;
    char *msg;
} FxSaid;

typedef struct {
    char *vsrc, *fsrc;      /* the two sources as fx_prepass () left them */
    GPtrArray *said;        /* FxSaid: what the repairs printed, in order */
    int nv, nf;
    gboolean vary;
} FxKeptPrepass;

static __thread struct {
    int depth;              /* fx_text_memo_begin () calls not ended yet */
    GHashTable *active;     /* source -> its fx_active_code () */
    GHashTable *expansion;  /* source -> its longest statement * 4 + (fx_expansion_ok () + 1) */
    GHashTable *prepass;    /* vertex length, vertex source, fragment source -> FxKeptPrepass */
} fx_memo;
static __thread GPtrArray *fx_said_tape;   /* FxSaid, while fx_prepass () computes for the memo */
static gint fx_memo_computed, fx_memo_reused;

static void fx_said_free (gpointer p)
{
    FxSaid *e = p;
    g_free (e->msg);
    g_free (e);
}

static void fx_kept_prepass_free (gpointer p)
{
    FxKeptPrepass *k = p;
    g_free (k->vsrc);
    g_free (k->fsrc);
    g_ptr_array_unref (k->said);
    g_free (k);
}

/* a message of the repairs: printed, and noted for the passes that take
 * the result over (they print it again: the log reads as before) */
static void fx_say (GLogLevelFlags level, const char *fmt, ...) G_GNUC_PRINTF (2, 3);
static void fx_say (GLogLevelFlags level, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    char *msg = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    g_log (G_LOG_DOMAIN, level, "%s", msg);
    if (fx_said_tape) {
        FxSaid *e = g_new (FxSaid, 1);
        e->level = level;
        e->msg = msg;
        g_ptr_array_add (fx_said_tape, e);
    } else {
        g_free (msg);
    }
}

void fx_text_memo_begin (void)
{
    if (fx_memo.depth++ > 0)
        return;
    fx_memo.active = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    fx_memo.expansion = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    fx_memo.prepass = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, fx_kept_prepass_free);
}

void fx_text_memo_end (void)
{
    if (fx_memo.depth <= 0 || --fx_memo.depth > 0)
        return;
    g_clear_pointer (&fx_memo.active, g_hash_table_unref);
    g_clear_pointer (&fx_memo.expansion, g_hash_table_unref);
    g_clear_pointer (&fx_memo.prepass, g_hash_table_unref);
}

void fx_text_memo_stats (guint *computed, guint *reused)
{
    if (computed)
        *computed = (guint) g_atomic_int_get (&fx_memo_computed);
    if (reused)
        *reused = (guint) g_atomic_int_get (&fx_memo_reused);
}


static gint64 fx_pp_expr (FxPp *x, int minprec);

static void fx_pp_ws (FxPp *x)
{
    while (*x->p == ' ' || *x->p == '\t' || *x->p == '\r')
        x->p++;
}

static gint64 fx_pp_primary (FxPp *x)
{
    fx_pp_ws (x);
    char c = *x->p;
    if (c == '(' || c == '!' || c == '-' || c == '+' || c == '~') {
        if (x->nest >= FX_PP_MAX_NEST) {
            x->bad = TRUE;
            return 0;
        }
        x->p++;
        x->nest++;
        gint64 v = c == '(' ? fx_pp_expr (x, 1) : fx_pp_primary (x);
        x->nest--;
        if (c == '(') {
            fx_pp_ws (x);
            if (*x->p == ')')
                x->p++;
            else
                x->bad = TRUE;
            return v;
        }
        /* two's complement, as cpp: -INT64_MIN is INT64_MIN, not a trap */
        return c == '!' ? !v : c == '-' ? (gint64) (0 - (guint64) v) : c == '~' ? ~v : v;
    }
    if (g_ascii_isdigit (c)) {
        char *end = NULL;
        gint64 v = g_ascii_strtoll (x->p, &end, 0);
        while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L')
            end++;
        if (*end == '.' || fx_is_ident (*end))
            x->bad = TRUE;
        x->p = end;
        return v;
    }
    if (fx_is_ident_start (c)) {
        const char *s = x->p;
        while (fx_is_ident (*x->p))
            x->p++;
        char *name = g_strndup (s, (gsize) (x->p - s));
        gint64 v = 0;
        if (!strcmp (name, "defined")) {
            fx_pp_ws (x);
            gboolean par = *x->p == '(';
            if (par)
                x->p++;
            fx_pp_ws (x);
            const char *m = x->p;
            while (fx_is_ident (*x->p))
                x->p++;
            char *mn = g_strndup (m, (gsize) (x->p - m));
            v = g_hash_table_contains (x->macros, mn);
            g_free (mn);
            fx_pp_ws (x);
            if (par) {
                if (*x->p == ')')
                    x->p++;
                else
                    x->bad = TRUE;
            }
        } else {
            const char *body = g_hash_table_lookup (x->macros, name);
            if (body && (!*body || *body == '\1' || x->depth > 8 || ++x->work > FX_PP_MAX_WORK)) {
                x->bad = TRUE; /* empty or function-like macro; too deep / too many */
            } else if (body) {
                FxPp sub = { body, x->macros, x->depth + 1, FALSE, 0, x->work, FALSE };
                v = fx_pp_expr (&sub, 1);
                x->work = sub.work;
                x->hazard |= sub.hazard;
                fx_pp_ws (&sub);
                if (sub.bad || *sub.p)
                    x->bad = TRUE;
            }
            /* an undefined name is 0 (cpp) */
            fx_pp_ws (x);
            if (*x->p == '(' || *x->p == '.' || *x->p == '[')
                x->bad = TRUE;
        }
        g_free (name);
        return v;
    }
    x->bad = TRUE;
    return 0;
}

static gint64 fx_pp_expr (FxPp *x, int minprec)
{
    gint64 l = fx_pp_primary (x);
    while (!x->bad) {
        fx_pp_ws (x);
        const char *p = x->p;
        char c = p[0], d = c ? p[1] : 0;
        int prec = 0, len = 1;
        if (c == '|' && d == '|') prec = 1, len = 2;
        else if (c == '&' && d == '&') prec = 2, len = 2;
        else if (c == '|') prec = 3;
        else if (c == '^') prec = 4;
        else if (c == '&') prec = 5;
        else if ((c == '=' || c == '!') && d == '=') prec = 6, len = 2;
        else if ((c == '<' || c == '>') && d == c) prec = 8, len = 2;
        else if (c == '<' || c == '>') prec = 7, len = d == '=' ? 2 : 1;
        else if (c == '+' || c == '-') prec = 9;
        else if (c == '*' || c == '/' || c == '%') prec = 10;
        if (!prec || prec < minprec)
            break;
        x->p += len;
        gint64 r = fx_pp_expr (x, prec + 1);
        switch (prec) {
        case 1: l = l || r; break;
        case 2: l = l && r; break;
        case 3: l = l | r; break;
        case 4: l = l ^ r; break;
        case 5: l = l & r; break;
        case 6: l = c == '=' ? l == r : l != r; break;
        case 7: l = c == '<' ? (len == 2 ? l <= r : l < r) : (len == 2 ? l >= r : l > r); break;
        /* wrapping arithmetic (unsigned), and no INT64_MIN / -1: that one
         * is a SIGFPE on x86, "#if (-9223372036854775807 - 1) / -1" in a
         * wallpaper's shader killed the daemon */
        case 8: l = c == '<' ? (gint64) ((guint64) l << (r & 63)) : l >> (r & 63); break;
        case 9: l = (gint64) (c == '+' ? (guint64) l + (guint64) r : (guint64) l - (guint64) r); break;
        default:
            if (c == '*')
                l = (gint64) ((guint64) l * (guint64) r);
            else if (r == 0)
                x->bad = TRUE;
            else if (r == -1) {
                x->hazard |= l == G_MININT64;
                l = c == '/' ? (gint64) (0 - (guint64) l) : 0;
            }
            else
                l = c == '/' ? l / r : l % r;
        }
    }
    return l;
}

/* ---------------------------------------------------------------- analysis */

typedef struct { char *name; FxTy t; int depth; } FxSym;
typedef struct { gboolean parent, taken, active, unknown; } FxPpFrame;
typedef struct { char *name; FxTy t; guint line; } FxVary;

/* parameters of a user function: n < 0 = not known (header over several
 * lines, more than 8 parameters, overloads that differ) */
typedef struct { int n; FxTy t[8]; gboolean out[8]; } FxSig;

typedef struct {
    GHashTable *macros;   /* name -> body; "" = defined empty, "\1" = function-like */
    GHashTable *funcs;    /* user function -> packed return type */
    GHashTable *sigs;     /* user function -> FxSig */
    GArray *syms;         /* FxSym, innermost last */
    GArray *pp;           /* FxPpFrame */
    GArray *vary;         /* FxVary: compiled varying / in / out declarations */
    int depth;            /* brace depth */
    int par0;             /* open parentheses at file scope (function header) */
    FxTy cur_ret;         /* return type of the function being defined */
    gboolean in_comment;  /* inside a block comment */
    gboolean mix_uint;    /* int and uint meet in an operation */
    int macro_depth;
    int macro_work;       /* macros expanded for the expression being typed */
    int ex_nest;          /* recursion depth of the expression typer */
    int n_repairs;
} FxAn;

static void fx_sym_clear (gpointer p)
{
    g_free (((FxSym *) p)->name);
}

static void fx_vary_clear (gpointer p)
{
    g_free (((FxVary *) p)->name);
}

static void fx_an_init (FxAn *an)
{
    memset (an, 0, sizeof (*an));
    an->macros = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    an->funcs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    an->sigs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    an->syms = g_array_new (FALSE, FALSE, sizeof (FxSym));
    g_array_set_clear_func (an->syms, fx_sym_clear);
    an->pp = g_array_new (FALSE, FALSE, sizeof (FxPpFrame));
    an->vary = g_array_new (FALSE, FALSE, sizeof (FxVary));
    g_array_set_clear_func (an->vary, fx_vary_clear);
}

static void fx_an_clear (FxAn *an)
{
    g_hash_table_unref (an->macros);
    g_hash_table_unref (an->funcs);
    g_hash_table_unref (an->sigs);
    g_array_unref (an->syms);
    g_array_unref (an->pp);
    g_array_unref (an->vary);
}

static gboolean fx_an_active (FxAn *an)
{
    return an->pp->len == 0 || g_array_index (an->pp, FxPpFrame, an->pp->len - 1).active;
}

static gpointer fx_ty_pack (FxTy t)
{
    return GUINT_TO_POINTER ((guint) t.b | ((guint) t.n << 8) | ((guint) t.arr << 16) | (1u << 24));
}

static FxTy fx_ty_unpack (gpointer p)
{
    guint v = GPOINTER_TO_UINT (p);
    FxTy t = { (guint8) (v & 0xff), (guint8) ((v >> 8) & 0xff), (guint8) ((v >> 16) & 0xff) };
    return t;
}

/* the line with comments blanked out; same length, so offsets into it
 * are offsets into the line */
static char *fx_code_view (const char *line, gboolean *in_block)
{
    char *c = g_strdup (line);
    gboolean blk = *in_block;
    for (char *p = c; *p; p++) {
        if (blk) {
            if (p[0] == '*' && p[1] == '/') {
                p[0] = p[1] = ' ';
                p++;
                blk = FALSE;
            } else {
                *p = ' ';
            }
        } else if (p[0] == '/' && p[1] == '/') {
            for (; *p; p++)
                *p = ' ';
            break;
        } else if (p[0] == '/' && p[1] == '*') {
            p[0] = p[1] = ' ';
            p++;
            blk = TRUE;
        } else if (*p == '\r') {
            *p = ' ';
        }
    }
    *in_block = blk;
    return c;
}

/* ---- expression types */

typedef struct {
    FxAn *an;
    const char *p, *e;
    gboolean fail;
} FxEx;

static FxTy fx_ex_ternary (FxEx *x);

static void fx_ex_ws (FxEx *x)
{
    while (x->p < x->e && g_ascii_isspace (*x->p))
        x->p++;
}

static gboolean fx_ty_num (FxTy t)
{
    return !t.arr && (t.b == FXB_INT || t.b == FXB_UINT || t.b == FXB_FLOAT);
}

/* result of "a op b" (arithmetic). Mixed vector sizes are not typed:
 * HLSL truncates, GLSL rejects - the statement repairs decide there. */
static FxTy fx_combine (FxAn *an, FxTy a, FxTy b, char op)
{
    if (a.arr || b.arr)
        return fx_ty_unk;
    if (a.b == FXB_MAT || b.b == FXB_MAT) {
        if (a.b == FXB_MAT && b.b == FXB_MAT)
            return a.n == b.n ? a : fx_ty_unk;
        FxTy m = a.b == FXB_MAT ? a : b, v = a.b == FXB_MAT ? b : a;
        if (!fx_ty_num (v))
            return fx_ty_unk;
        if (v.n == 1)
            return m;
        return op == '*' && v.n == m.n && v.b == FXB_FLOAT ? v : fx_ty_unk;
    }
    if (!fx_ty_num (a) || !fx_ty_num (b))
        return fx_ty_unk;
    if ((a.b == FXB_INT && b.b == FXB_UINT) || (a.b == FXB_UINT && b.b == FXB_INT)) {
        an->mix_uint = TRUE;
        a.b = b.b = FXB_INT;
    }
    guint8 base = a.b == FXB_FLOAT || b.b == FXB_FLOAT ? FXB_FLOAT : a.b;
    if (a.n == b.n)
        return fx_ty (base, a.n);
    if (a.n == 1)
        return fx_ty (base, b.n);
    if (b.n == 1)
        return fx_ty (base, a.n);
    return fx_ty_unk;
}

static FxTy fx_ident_type (FxAn *an, const char *s, gsize len)
{
    static const struct { const char *nm; guint8 b, n; } gl[] = {
        { "gl_FragCoord", FXB_FLOAT, 4 }, { "gl_Position", FXB_FLOAT, 4 },
        { "gl_PointCoord", FXB_FLOAT, 2 }, { "gl_FragDepth", FXB_FLOAT, 1 },
        { "gl_VertexID", FXB_INT, 1 }, { "gl_InstanceID", FXB_INT, 1 },
        { "gl_FrontFacing", FXB_BOOL, 1 }, { "true", FXB_BOOL, 1 }, { "false", FXB_BOOL, 1 },
    };
    char *name = g_strndup (s, len);
    FxTy t = fx_ty_unk;
    const char *body = g_hash_table_lookup (an->macros, name);
    if (body) {
        /* object-like macro: the type of what it stands for */
        if (*body && *body != '\1' && an->macro_depth < 6 && an->macro_work < FX_EX_MAX_WORK) {
            an->macro_depth++;
            an->macro_work++;
            FxEx sub = { an, body, body + strlen (body), FALSE };
            t = fx_ex_ternary (&sub);
            fx_ex_ws (&sub);
            if (sub.fail || sub.p < sub.e)
                t = fx_ty_unk;
            an->macro_depth--;
        }
    } else {
        gboolean hit = FALSE;
        for (int i = (int) an->syms->len - 1; i >= 0 && !hit; i--) {
            FxSym *sy = &g_array_index (an->syms, FxSym, i);
            if (!strcmp (sy->name, name)) {
                t = sy->t;
                hit = TRUE;
            }
        }
        for (guint i = 0; i < G_N_ELEMENTS (gl) && !hit; i++)
            if (!strcmp (gl[i].nm, name)) {
                t = fx_ty (gl[i].b, gl[i].n);
                hit = TRUE;
            }
    }
    g_free (name);
    return t;
}

enum { FXR_FLOAT = 1, FXR_ARG0, FXR_GEN, FXR_VEC4, FXR_VEC3, FXR_BOOL, FXR_MUL };

/* builtins (GLSL names and the prelude's HLSL names). ok = argument
 * positions that may stay scalar next to vector arguments */
static const struct { const char *nm; guint8 rule, ok; } fx_builtins[] = {
    { "length", FXR_FLOAT, 0 }, { "distance", FXR_FLOAT, 0 }, { "dot", FXR_FLOAT, 0 },
    { "determinant", FXR_FLOAT, 0 },
    { "sin", FXR_ARG0, 0 }, { "cos", FXR_ARG0, 0 }, { "tan", FXR_ARG0, 0 },
    { "asin", FXR_ARG0, 0 }, { "acos", FXR_ARG0, 0 }, { "atan", FXR_ARG0, 0 },
    { "sinh", FXR_ARG0, 0 }, { "cosh", FXR_ARG0, 0 }, { "tanh", FXR_ARG0, 0 },
    { "exp", FXR_ARG0, 0 }, { "exp2", FXR_ARG0, 0 }, { "log", FXR_ARG0, 0 },
    { "log2", FXR_ARG0, 0 }, { "sqrt", FXR_ARG0, 0 }, { "inversesqrt", FXR_ARG0, 0 },
    { "abs", FXR_ARG0, 0 }, { "sign", FXR_ARG0, 0 }, { "floor", FXR_ARG0, 0 },
    { "ceil", FXR_ARG0, 0 }, { "round", FXR_ARG0, 0 }, { "trunc", FXR_ARG0, 0 },
    { "fract", FXR_ARG0, 0 }, { "radians", FXR_ARG0, 0 }, { "degrees", FXR_ARG0, 0 },
    { "normalize", FXR_ARG0, 0 }, { "dFdx", FXR_ARG0, 0 }, { "dFdy", FXR_ARG0, 0 },
    { "fwidth", FXR_ARG0, 0 }, { "saturate", FXR_ARG0, 0 }, { "frac", FXR_ARG0, 0 },
    { "ddx", FXR_ARG0, 0 }, { "ddy", FXR_ARG0, 0 }, { "rsqrt", FXR_ARG0, 0 },
    { "log10", FXR_ARG0, 0 }, { "reflect", FXR_ARG0, 0 }, { "refract", FXR_ARG0, 0 },
    { "faceforward", FXR_ARG0, 0 }, { "transpose", FXR_ARG0, 0 }, { "inverse", FXR_ARG0, 0 },
    { "atan2", FXR_ARG0, 0 },
    { "min", FXR_GEN, 0x2 }, { "max", FXR_GEN, 0x2 }, { "mod", FXR_GEN, 0x2 },
    { "fmod", FXR_GEN, 0x2 }, { "clamp", FXR_GEN, 0x6 }, { "mix", FXR_GEN, 0x4 },
    { "lerp", FXR_GEN, 0x4 }, { "step", FXR_GEN, 0x1 }, { "smoothstep", FXR_GEN, 0x3 },
    { "pow", FXR_GEN, 0 },
    { "texture", FXR_VEC4, 0 }, { "texture2D", FXR_VEC4, 0 }, { "texSample2D", FXR_VEC4, 0 },
    { "texSample2DLod", FXR_VEC4, 0 }, { "textureLod", FXR_VEC4, 0 },
    { "texture2DLod", FXR_VEC4, 0 }, { "texelFetch", FXR_VEC4, 0 },
    { "textureGrad", FXR_VEC4, 0 }, { "texSample3D", FXR_VEC4, 0 },
    { "cross", FXR_VEC3, 0 }, { "any", FXR_BOOL, 0 }, { "all", FXR_BOOL, 0 },
    { "mul", FXR_MUL, 0 },
};

static int fx_builtin_find (const char *s, gsize len)
{
    for (guint i = 0; i < G_N_ELEMENTS (fx_builtins); i++)
        if (strlen (fx_builtins[i].nm) == len && !strncmp (fx_builtins[i].nm, s, len))
            return (int) i;
    return -1;
}

static FxTy fx_call_type (FxAn *an, const char *s, gsize len, const FxTy *at, int na)
{
    FxTy t;
    if (fx_ty_parse (s, len, &t))
        return t.b == FXB_VOID || t.b == FXB_OTHER ? fx_ty_unk : t; /* constructor */
    if (len == 5 && (!strncmp (s, "CAST", 4) || !strncmp (s, "TOGL", 4)) && s[4] >= '2' && s[4] <= '4')
        return fx_ty (FXB_FLOAT, (guint8) (s[4] - '0'));
    char *name = g_strndup (s, len);
    gboolean is_macro = g_hash_table_contains (an->macros, name);
    gpointer uf = g_hash_table_lookup (an->funcs, name);
    g_free (name);
    int bi = fx_builtin_find (s, len);
    if (bi < 0) {
        /* user function (not a function-like macro of the shader) */
        return uf && !is_macro ? fx_ty_unpack (uf) : fx_ty_unk;
    }
    gboolean intfn = (len == 3 && (!strncmp (s, "abs", 3) || !strncmp (s, "min", 3) ||
                                   !strncmp (s, "max", 3))) ||
                     (len == 4 && !strncmp (s, "sign", 4)) || (len == 5 && !strncmp (s, "clamp", 5));
    switch (fx_builtins[bi].rule) {
    case FXR_FLOAT:
        return fx_ty (FXB_FLOAT, 1);
    case FXR_VEC4:
        return fx_ty (FXB_FLOAT, 4);
    case FXR_VEC3:
        return fx_ty (FXB_FLOAT, 3);
    case FXR_BOOL:
        return fx_ty (FXB_BOOL, 1);
    case FXR_MUL:
        return na == 2 ? fx_combine (an, at[1], at[0], '*') : fx_ty_unk;
    case FXR_ARG0:
        if (na < 1 || at[0].arr)
            return fx_ty_unk;
        if (at[0].b == FXB_MAT)
            return at[0];
        if (!fx_ty_num (at[0]))
            return fx_ty_unk;
        return fx_ty (intfn ? at[0].b : FXB_FLOAT, at[0].n);
    default: {
        /* componentwise: the (smallest) vector size of the arguments */
        guint8 n = 0, base = FXB_INT;
        for (int i = 0; i < na; i++) {
            if (!fx_ty_num (at[i]))
                return fx_ty_unk;
            if (at[i].b == FXB_FLOAT)
                base = FXB_FLOAT;
            else if (at[i].b == FXB_UINT && base == FXB_INT)
                base = FXB_UINT;
            if (at[i].n > 1 && (!n || at[i].n < n))
                n = at[i].n;
        }
        if (na < 1)
            return fx_ty_unk;
        return fx_ty (intfn ? base : FXB_FLOAT, n ? n : 1);
    }
    }
}

static FxTy fx_ex_postfix (FxEx *x)
{
    FxTy t = fx_ty_unk;
    fx_ex_ws (x);
    const char *p = x->p;
    if (p >= x->e) {
        x->fail = TRUE;
        return t;
    }
    if (*p == '(') {
        x->p++;
        t = fx_ex_ternary (x);
        fx_ex_ws (x);
        if (x->p < x->e && *x->p == ')') {
            x->p++;
        } else {
            x->fail = TRUE;
            return fx_ty_unk;
        }
    } else if (g_ascii_isdigit (*p) || (*p == '.' && p + 1 < x->e && g_ascii_isdigit (p[1]))) {
        gboolean hex = p[0] == '0' && p + 1 < x->e && (p[1] == 'x' || p[1] == 'X'), flt = FALSE;
        const char *q = hex ? p + 2 : p;
        while (q < x->e && (g_ascii_isalnum (*q) || *q == '.' ||
                            (!hex && (*q == '+' || *q == '-') && (q[-1] == 'e' || q[-1] == 'E')))) {
            if (!hex && (*q == '.' || *q == 'e' || *q == 'E' || *q == 'f' || *q == 'F'))
                flt = TRUE;
            q++;
        }
        gboolean uns = !flt && (q[-1] == 'u' || q[-1] == 'U');
        t = fx_ty (flt ? FXB_FLOAT : uns ? FXB_UINT : FXB_INT, 1);
        x->p = q;
    } else if (fx_is_ident_start (*p)) {
        const char *q = p;
        while (q < x->e && fx_is_ident (*q))
            q++;
        gsize len = (gsize) (q - p);
        x->p = q;
        fx_ex_ws (x);
        if (x->p < x->e && *x->p == '(') {
            FxTy at[8];
            int na = 0;
            x->p++;
            fx_ex_ws (x);
            if (x->p < x->e && *x->p == ')') {
                x->p++;
            } else {
                for (;;) {
                    FxTy a = fx_ex_ternary (x);
                    if (na < 8)
                        at[na] = a;
                    na++;
                    fx_ex_ws (x);
                    if (x->p < x->e && *x->p == ',') {
                        x->p++;
                        continue;
                    }
                    if (x->p < x->e && *x->p == ')') {
                        x->p++;
                        break;
                    }
                    x->fail = TRUE;
                    return fx_ty_unk;
                }
            }
            t = na <= 8 ? fx_call_type (x->an, p, len, at, na) : fx_ty_unk;
        } else {
            t = fx_ident_type (x->an, p, len);
        }
    } else {
        x->fail = TRUE;
        return t;
    }
    for (;;) {
        fx_ex_ws (x);
        if (x->p >= x->e)
            break;
        if (*x->p == '.') {
            const char *f = x->p + 1, *q = f;
            while (q < x->e && fx_is_ident (*q))
                q++;
            gsize fl = (gsize) (q - f);
            x->p = q;
            gboolean sw = fl >= 1 && fl <= 4;
            for (gsize i = 0; sw && i < fl; i++)
                sw = strchr ("xyzwrgbastpq", f[i]) != NULL;
            if (sw && !t.arr && t.n >= 2 && t.n <= 4 &&
                (t.b == FXB_FLOAT || t.b == FXB_INT || t.b == FXB_UINT || t.b == FXB_BOOL))
                t = fx_ty (t.b, (guint8) fl);
            else
                t = fx_ty_unk;
        } else if (*x->p == '[') {
            x->p++;
            (void) fx_ex_ternary (x);
            fx_ex_ws (x);
            if (x->p < x->e && *x->p == ']') {
                x->p++;
            } else {
                x->fail = TRUE;
                return fx_ty_unk;
            }
            if (t.arr)
                t.arr = 0;
            else if (t.b == FXB_MAT)
                t = fx_ty (FXB_FLOAT, t.n);
            else if (t.n >= 2 && t.b != FXB_UNK)
                t = fx_ty (t.b, 1);
            else
                t = fx_ty_unk;
        } else if (x->p + 1 < x->e && (x->p[0] == '+' || x->p[0] == '-') && x->p[1] == x->p[0]) {
            x->p += 2;
        } else {
            break;
        }
    }
    return t;
}

static FxTy fx_ex_unary_1 (FxEx *x);

/* every level of an expression comes through here or fx_ex_ternary
 * (parentheses, call arguments, subscripts, unary chains, ?: chains):
 * the recursion is bounded (FX_EX_MAX_NEST), deeper text is not typed */
static FxTy fx_ex_unary (FxEx *x)
{
    if (x->an->ex_nest >= FX_EX_MAX_NEST) {
        x->fail = TRUE;
        return fx_ty_unk;
    }
    x->an->ex_nest++;
    FxTy t = fx_ex_unary_1 (x);
    x->an->ex_nest--;
    return t;
}

static FxTy fx_ex_unary_1 (FxEx *x)
{
    fx_ex_ws (x);
    if (x->p >= x->e) {
        x->fail = TRUE;
        return fx_ty_unk;
    }
    char c = *x->p;
    if (c == '-' || c == '+') {
        x->p++;
        if (x->p < x->e && *x->p == c)
            x->p++;
        return fx_ex_unary (x);
    }
    if (c == '!') {
        x->p++;
        (void) fx_ex_unary (x);
        return fx_ty (FXB_BOOL, 1);
    }
    if (c == '~') {
        x->p++;
        return fx_ex_unary (x);
    }
    return fx_ex_postfix (x);
}

static FxTy fx_ex_bin (FxEx *x, int minprec)
{
    FxTy l = fx_ex_unary (x);
    while (!x->fail) {
        fx_ex_ws (x);
        if (x->p >= x->e)
            break;
        const char *p = x->p;
        char c = p[0], d = p + 1 < x->e ? p[1] : 0;
        int prec = 0, len = 1;
        if ((c == '|' && d == '|') || (c == '^' && d == '^')) prec = 1, len = 2;
        else if (c == '&' && d == '&') prec = 2, len = 2;
        else if (c == '|' && d != '=') prec = 3;
        else if (c == '^' && d != '=') prec = 4;
        else if (c == '&' && d != '=') prec = 5;
        else if ((c == '=' || c == '!') && d == '=') prec = 6, len = 2;
        else if ((c == '<' || c == '>') && d == c) {
            if (p + 2 < x->e && p[2] == '=')
                break;
            prec = 8, len = 2;
        } else if (c == '<' || c == '>') prec = 7, len = d == '=' ? 2 : 1;
        else if ((c == '+' || c == '-') && d != '=') prec = 9;
        else if ((c == '*' || c == '/' || c == '%') && d != '=') prec = 10;
        if (!prec || prec < minprec)
            break;
        x->p += len;
        FxTy r = fx_ex_bin (x, prec + 1);
        if (prec <= 2) {
            l = fx_ty (FXB_BOOL, 1);
        } else if (prec == 6 || prec == 7) {
            (void) fx_combine (x->an, l, r, '<'); /* notes int/uint mixing */
            l = fx_ty (FXB_BOOL, 1);
        } else if (prec != 8) {
            l = fx_combine (x->an, l, r, c);
        }
    }
    return l;
}

static FxTy fx_ex_ternary (FxEx *x)
{
    FxTy c = fx_ex_bin (x, 1);
    fx_ex_ws (x);
    if (!x->fail && x->p < x->e && *x->p == '?') {
        if (x->an->ex_nest >= FX_EX_MAX_NEST) {
            x->fail = TRUE;
            return fx_ty_unk;
        }
        x->p++;
        x->an->ex_nest++;
        FxTy a = fx_ex_ternary (x);
        fx_ex_ws (x);
        if (x->p >= x->e || *x->p != ':') {
            x->an->ex_nest--;
            x->fail = TRUE;
            return fx_ty_unk;
        }
        x->p++;
        FxTy b = fx_ex_ternary (x);
        x->an->ex_nest--;
        if (a.b == b.b && a.n == b.n && a.arr == b.arr)
            return a;
        if (fx_ty_num (a) && fx_ty_num (b) && a.n == b.n && (a.b == FXB_FLOAT || b.b == FXB_FLOAT))
            return fx_ty (FXB_FLOAT, a.n);
        return fx_ty_unk;
    }
    return c;
}

/* type of the expression [s, e); unknown unless all of it parsed */
static FxTy fx_infer (FxAn *an, const char *s, const char *e)
{
    while (e > s && g_ascii_isspace (e[-1]))
        e--;
    if (e <= s)
        return fx_ty_unk;
    an->macro_work = 0;
    FxEx x = { an, s, e, FALSE };
    FxTy t = fx_ex_ternary (&x);
    fx_ex_ws (&x);
    if (x.fail || x.p < x.e)
        return fx_ty_unk;
    return t;
}

/* ---- declarations and scopes */

static void fx_an_declare (FxAn *an, const char *n, const char *ne, FxTy t, gboolean arr, int depth)
{
    FxSym sy = { g_strndup (n, (gsize) (ne - n)), t, depth };
    sy.t.arr = arr;
    g_array_append_val (an->syms, sy);
}

/* matching ')' of the '(' at p, on this line */
static const char *fx_paren_end (const char *p)
{
    int d = 0;
    for (; *p; p++) {
        if (*p == '(')
            d++;
        else if (*p == ')' && --d == 0)
            return p;
    }
    return NULL;
}

/* the parameter list (open, close) of a function header or prototype:
 * remembered per function name for the call repair (fx_ls_usercalls).
 * close = NULL: the list does not end on this line. */
static void fx_an_sig (FxAn *an, const char *name, const char *open, const char *close)
{
    FxSig sig = { 0 };
    if (!close)
        sig.n = -1;
    const char *s = open + 1;
    int d = 0;
    for (const char *c = open + 1; close && c <= close && sig.n >= 0; c++) {
        if (c < close && (*c == '(' || *c == '['))
            d++;
        else if (c < close && (*c == ')' || *c == ']'))
            d--;
        else if (c == close || (*c == ',' && d == 0)) {
            /* [qualifiers] type name: the last type word before the name */
            FxTy t = fx_ty_unk;
            gboolean out = FALSE, arr = FALSE, any = FALSE;
            for (const char *w = fx_sp (s); w < c;) {
                if (*w == '[')
                    arr = TRUE;
                if (!fx_is_ident_start (*w)) {
                    w++;
                    continue;
                }
                const char *we = w;
                while (we < c && fx_is_ident (*we))
                    we++;
                gsize wl = (gsize) (we - w);
                FxTy tt;
                any = TRUE;
                if ((wl == 3 && !strncmp (w, "out", 3)) || (wl == 5 && !strncmp (w, "inout", 5)))
                    out = TRUE;
                else if (fx_ty_parse (w, wl, &tt))
                    t = tt;
                w = we;
            }
            if (c == close && sig.n == 0 && (!any || t.b == FXB_VOID))
                break;                      /* "()" / "(void)" */
            if (sig.n == 8) {
                sig.n = -1;
                break;
            }
            sig.t[sig.n] = arr ? fx_ty_unk : t;
            sig.out[sig.n] = out;
            sig.n++;
            s = c + 1;
        }
    }
    FxSig *old = g_hash_table_lookup (an->sigs, name);
    if (old) {
        /* prototype + definition agree; overloads that differ: unknown */
        gboolean same = old->n == sig.n;
        for (int i = 0; same && i < sig.n; i++)
            same = old->t[i].b == sig.t[i].b && old->t[i].n == sig.t[i].n &&
                   old->out[i] == sig.out[i];
        if (!same)
            old->n = -1;
        return;
    }
    g_hash_table_insert (an->sigs, g_strdup (name), g_memdup2 (&sig, sizeof (sig)));
}

/* "T name" pairs of one compiled code line: variables (locals, globals,
 * parameters of the function header at file scope) and functions */
static void fx_an_decls (FxAn *an, const char *code)
{
    int par = an->depth == 0 ? an->par0 : 0;
    const char *p = code;
    while (*p) {
        if (*p == '(')
            par++;
        else if (*p == ')')
            par = MAX (0, par - 1);
        if (!fx_is_ident_start (*p) || (p > code && (fx_is_ident (p[-1]) || p[-1] == '.'))) {
            p++;
            continue;
        }
        const char *q = p;
        while (fx_is_ident (*q))
            q++;
        FxTy t, t2;
        const char *n = fx_sp (q);
        if (!fx_ty_parse (p, (gsize) (q - p), &t) || !fx_is_ident_start (*n)) {
            p = q;
            continue;
        }
        const char *ne = n;
        while (fx_is_ident (*ne))
            ne++;
        if (fx_ty_parse (n, (gsize) (ne - n), &t2)) {
            p = q;
            continue;
        }
        const char *a = fx_sp (ne);
        if (*a == '(') {
            if (an->depth == 0 && par == 0) {
                char *fn = g_strndup (n, (gsize) (ne - n));
                gpointer old = g_hash_table_lookup (an->funcs, fn);
                /* overloads with different return types: unknown */
                if (old && fx_ty_unpack (old).b != FXB_UNK &&
                    (fx_ty_unpack (old).b != t.b || fx_ty_unpack (old).n != t.n))
                    t2 = fx_ty_unk;
                else
                    t2 = t;
                const char *ce = fx_paren_end (a);
                fx_an_sig (an, fn, a, ce);
                g_hash_table_insert (an->funcs, fn, fx_ty_pack (t2));
                if (ce && *fx_sp (ce + 1) == ';') {
                    p = ce + 1; /* prototype: its parameters are no variables */
                    continue;
                }
                an->cur_ret = t;
            }
            p = ne;
            continue;
        }
        if ((*a == '=' && a[1] != '=') || *a == ';' || *a == ',' || *a == ')' || *a == '[' || !*a) {
            gboolean param = an->depth == 0 && par > 0;
            fx_an_declare (an, n, ne, t, *a == '[', an->depth + (param ? 1 : 0));
            /* "float a = 1.0, b;" */
            while (!param) {
                const char *e = a;
                int d = 0;
                for (; *e; e++) {
                    if (*e == '(' || *e == '[')
                        d++;
                    else if (*e == ')' || *e == ']') {
                        if (d == 0)
                            break;
                        d--;
                    } else if ((*e == ',' || *e == ';') && d == 0)
                        break;
                }
                if (*e != ',')
                    break;
                const char *m = fx_sp (e + 1), *me = m;
                while (fx_is_ident (*me))
                    me++;
                a = fx_sp (me);
                if (me == m || !fx_is_ident_start (*m) || fx_ty_parse (m, (gsize) (me - m), &t2) ||
                    !((*a == '=' && a[1] != '=') || *a == ';' || *a == ',' || *a == '['))
                    break;
                fx_an_declare (an, m, me, t, *a == '[', an->depth);
            }
            p = ne;
            continue;
        }
        p = q;
    }
}

/* a compiled "varying T a, b;" (also "in" / "out") at file scope */
static void fx_an_varying (FxAn *an, const char *code, guint line)
{
    static const char *skip[] = { "varying", "in", "out", "flat", "smooth", "noperspective",
                                  "centroid", "highp", "mediump", "lowp" };
    const char *p = fx_sp (code);
    gboolean io = FALSE;
    for (;;) {
        const char *q = p;
        while (fx_is_ident (*q))
            q++;
        gboolean hit = FALSE;
        for (guint i = 0; i < G_N_ELEMENTS (skip) && !hit; i++)
            hit = strlen (skip[i]) == (gsize) (q - p) && !strncmp (skip[i], p, (gsize) (q - p));
        if (!hit)
            break;
        if (*p == 'v' || *p == 'i' || *p == 'o')
            io = TRUE; /* varying / in / out */
        p = fx_sp (q);
    }
    if (!io)
        return;
    const char *q = p;
    while (fx_is_ident (*q))
        q++;
    FxTy t;
    if (!fx_ty_parse (p, (gsize) (q - p), &t) || !strchr (q, ';'))
        return;
    for (p = fx_sp (q); fx_is_ident_start (*p);) {
        q = p;
        while (fx_is_ident (*q))
            q++;
        FxVary v = { g_strndup (p, (gsize) (q - p)), t, line };
        q = fx_sp (q);
        if (*q == '[') {
            v.t.arr = 1;
            while (*q && *q != ']')
                q++;
            if (*q)
                q = fx_sp (q + 1);
        }
        g_array_append_val (an->vary, v);
        if (*q != ',')
            break;
        p = fx_sp (q + 1);
    }
}

static void fx_an_braces (FxAn *an, const char *code)
{
    for (const char *p = code; *p; p++) {
        if (an->depth == 0) {
            if (*p == '(')
                an->par0++;
            else if (*p == ')')
                an->par0 = MAX (0, an->par0 - 1);
        }
        if (*p == '{') {
            an->depth++;
            an->par0 = 0;
        } else if (*p == '}') {
            an->depth = MAX (0, an->depth - 1);
            while (an->syms->len &&
                   g_array_index (an->syms, FxSym, an->syms->len - 1).depth > an->depth)
                g_array_remove_index (an->syms, an->syms->len - 1);
        } else if (*p == ';' && an->depth == 0 && an->par0 == 0) {
            /* a prototype's parameters end with it */
            while (an->syms->len && g_array_index (an->syms, FxSym, an->syms->len - 1).depth > 0)
                g_array_remove_index (an->syms, an->syms->len - 1);
        }
    }
}

/* ---- preprocessor directives */

/* one directive line (d = its '#' in the comment-free view). Returns the
 * replacement line when the directive itself is repaired, else NULL. */
static char *fx_an_directive (FxAn *an, const char *line, const char *d, gboolean repair)
{
    const char *w = fx_sp (d + 1), *we = w;
    while (fx_is_ident (*we))
        we++;
    gsize wl = (gsize) (we - w);
#define FX_DIR(s) (wl == strlen (s) && !strncmp (w, s, wl))
    gboolean act = fx_an_active (an);
    FxPpFrame *top = an->pp->len ? &g_array_index (an->pp, FxPpFrame, an->pp->len - 1) : NULL;
    const char *orig = fx_trim (line);
    if (FX_DIR ("if") || FX_DIR ("ifdef") || FX_DIR ("ifndef")) {
        FxPpFrame f = { act, TRUE, FALSE, FALSE };
        char *fixed = NULL;
        if (act) {
            gint64 v = 0;
            if (FX_DIR ("if")) {
                FxPp x = { we, an->macros, 0, FALSE, 0, 0, FALSE };
                v = fx_pp_expr (&x, 1);
                fx_pp_ws (&x);
                if (x.bad || *x.p) {
                    /* member access or a float: no preprocessor accepts
                     * it ("#if g_Texture0Resolution.y <= 640"); WE takes
                     * the #else branch */
                    if (strpbrk (we, ".[")) {
                        v = 0;
                        if (repair)
                            fixed = g_strdup_printf ("#if 0 // was: %s", orig);
                    } else {
                        f.unknown = TRUE;
                    }
                } else if (x.hazard && repair) {
                    /* the driver's preprocessor would divide INT64_MIN by
                     * -1 (SIGFPE in Mesa's): it gets the value instead */
                    fixed = g_strdup_printf ("#if %d // was: %s", v != 0, orig);
                }
            } else {
                const char *m = fx_sp (we), *me = m;
                while (fx_is_ident (*me))
                    me++;
                char *mn = g_strndup (m, (gsize) (me - m));
                v = g_hash_table_contains (an->macros, mn);
                if (FX_DIR ("ifndef"))
                    v = !v;
                g_free (mn);
            }
            f.active = f.unknown || v != 0;
            f.taken = f.active && !f.unknown;
        }
        g_array_append_val (an->pp, f);
        return fixed;
    }
    if (FX_DIR ("elif") || FX_DIR ("else") || FX_DIR ("endif")) {
        if (!top) {
            /* unbalanced workshop shader: WE's preprocessor ignores it */
            return repair ? g_strdup_printf ("// (stray) %s", line) : NULL;
        }
        if (FX_DIR ("endif")) {
            g_array_set_size (an->pp, an->pp->len - 1);
            return NULL;
        }
        if (top->unknown)
            return NULL; /* both branches may be compiled */
        if (FX_DIR ("else")) {
            top->active = top->parent && !top->taken;
            top->taken = TRUE;
            return NULL;
        }
        char *fixed = NULL;
        gint64 v = 0;
        if (top->parent && !top->taken) {
            FxPp x = { we, an->macros, 0, FALSE, 0, 0, FALSE };
            v = fx_pp_expr (&x, 1);
            fx_pp_ws (&x);
            if (x.bad || *x.p) {
                if (strpbrk (we, ".[")) {
                    v = 0;
                    if (repair)
                        fixed = g_strdup_printf ("#elif 0 // was: %s", orig);
                } else {
                    top->unknown = TRUE;
                    top->active = top->parent;
                    return NULL;
                }
            } else if (x.hazard && repair) {
                fixed = g_strdup_printf ("#elif %d // was: %s", v != 0, orig);
            }
        }
        top->active = top->parent && !top->taken && v != 0;
        top->taken = top->taken || top->active;
        return fixed;
    }
    if (!act)
        return NULL;
    if (FX_DIR ("define")) {
        const char *m = fx_sp (we), *me = m;
        while (fx_is_ident (*me))
            me++;
        if (me > m) {
            char *body;
            if (*me == '(') {
                body = g_strdup ("\1");
            } else {
                body = g_strdup (fx_sp (me));
                g_strchomp (body);
            }
            g_hash_table_insert (an->macros, g_strndup (m, (gsize) (me - m)), body);
        }
    } else if (FX_DIR ("undef")) {
        const char *m = fx_sp (we), *me = m;
        while (fx_is_ident (*me))
            me++;
        char *mn = g_strndup (m, (gsize) (me - m));
        g_hash_table_remove (an->macros, mn);
        g_free (mn);
    }
#undef FX_DIR
    return NULL;
}

/* ---- statement repairs decided from the source */

/* line with [s, e) wrapped: ...pre(...)... (s, e point into code, a
 * same-length view of line) */
static char *fx_wrap (const char *line, const char *code, const char *s, const char *e,
                      const char *pre)
{
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    return g_strdup_printf ("%.*s%s(%.*s)%s", (int) (s - code), line, pre, (int) (e - s),
                            line + (s - code), line + (e - code));
}

/* start of the assignment target that ends before op */
static const char *fx_lvalue_start (const char *code, const char *op)
{
    const char *p = op;
    while (p > code && (p[-1] == ' ' || p[-1] == '\t'))
        p--;
    const char *end = p;
    while (p > code) {
        char c = p[-1];
        if (fx_is_ident (c) || c == '.') {
            p--;
        } else if (c == ']') {
            int d = 0;
            do {
                p--;
                if (*p == ']')
                    d++;
                else if (*p == '[')
                    d--;
            } while (p > code && d > 0);
        } else {
            break;
        }
    }
    return p < end ? p : NULL;
}

/* "a % b" with a float operand -> mod(float(a), float(b)) */
static char *fx_ls_modulo (FxAn *an, const char *line, const char *code)
{
    for (const char *op = code; (op = strchr (op, '%')) != NULL; op++) {
        if (op[1] == '=')
            continue;
        const char *a = fx_operand_start (code, op), *b = fx_operand_end (op + 1);
        const char *a1 = op, *b0 = fx_sp (op + 1);
        while (a1 > a && (a1[-1] == ' ' || a1[-1] == '\t'))
            a1--;
        if (a1 <= a || b <= b0)
            continue;
        FxTy ta = fx_infer (an, a, a1), tb = fx_infer (an, b0, b);
        if (ta.b == FXB_FLOAT || tb.b == FXB_FLOAT)
            return g_strdup_printf ("%.*smod(float(%.*s), float(%.*s))%s", (int) (a - code), line,
                                    (int) (a1 - a), line + (a - code), (int) (b - b0),
                                    line + (b0 - code), line + (b - code));
        (void) fx_combine (an, ta, tb, '%');
    }
    return NULL;
}

/* "(a < b) * x": a parenthesised comparison used as a number */
static char *fx_ls_boolarith (FxAn *an, const char *line, const char *code)
{
    for (const char *p = code; (p = strchr (p, '(')) != NULL; p++) {
        const char *k = p;
        while (k > code && (k[-1] == ' ' || k[-1] == '\t'))
            k--;
        if (k > code && fx_is_ident (k[-1]))
            continue; /* call, constructor, if (...) */
        const char *q = fx_paren_end (p);
        if (!q)
            continue;
        const char *n = fx_sp (q + 1);
        char pc = k > code ? k[-1] : 0, nc = *n;
        gboolean after = (nc == '*' || nc == '/' || nc == '+' || nc == '-') && n[1] != '=';
        gboolean before = pc == '*' || pc == '/' || pc == '+' || pc == '-';
        if (!after && !before)
            continue;
        FxTy t = fx_infer (an, p + 1, q);
        if (t.b == FXB_BOOL && t.n == 1 && !t.arr)
            return g_strdup_printf ("%.*sfloat%s", (int) (p - code), line, line + (p - code));
    }
    return NULL;
}

/* componentwise builtins with mixed argument sizes: HLSL truncates the
 * wider vectors and splats scalars; "texSample2D(s, vec4)" reads .xy */
static char *fx_ls_calls (FxAn *an, const char *line, const char *code)
{
    for (const char *p = code; *p;) {
        if (!fx_is_ident_start (*p) || (p > code && (fx_is_ident (p[-1]) || p[-1] == '.'))) {
            p++;
            continue;
        }
        const char *q = p;
        while (fx_is_ident (*q))
            q++;
        const char *o = fx_sp (q);
        gsize len = (gsize) (q - p);
        int bi = *o == '(' ? fx_builtin_find (p, len) : -1;
        gboolean tex2d = bi >= 0 && fx_builtins[bi].rule == FXR_VEC4 &&
                         g_strstr_len (p, (gssize) len, "2D") != NULL;
        if (bi < 0 || !fx_paren_end (o) ||
            (fx_builtins[bi].rule != FXR_GEN && fx_builtins[bi].rule != FXR_FLOAT && !tex2d)) {
            p = q;
            continue;
        }
        const char *close = fx_paren_end (o), *as[8], *ae[8];
        FxTy at[8];
        int na = 0, d = 0;
        gboolean ok = TRUE;
        const char *s = o + 1;
        for (const char *c = o + 1; c <= close && ok; c++) {
            if (c < close && (*c == '(' || *c == '['))
                d++;
            else if (c < close && (*c == ')' || *c == ']'))
                d--;
            else if (c == close || (*c == ',' && d == 0)) {
                if (na == 8) {
                    ok = FALSE;
                    break;
                }
                as[na] = fx_sp (s);
                ae[na] = c;
                while (ae[na] > as[na] && (ae[na][-1] == ' ' || ae[na][-1] == '\t'))
                    ae[na]--;
                at[na] = fx_infer (an, as[na], ae[na]);
                na++;
                s = c + 1;
            }
        }
        int wrap[8] = { 0 }, nmin = 0;
        if (ok && tex2d) {
            if (na >= 2 && at[1].b == FXB_FLOAT && !at[1].arr && at[1].n > 2)
                wrap[1] = nmin = 2;
        } else if (ok) {
            guint8 sok = fx_builtins[bi].ok;
            gboolean grp_vec = FALSE;
            for (int i = 0; i < na && ok; i++) {
                ok = fx_ty_num (at[i]) && (at[i].n == 1 || at[i].b == FXB_FLOAT);
                if (ok && at[i].n > 1 && (!nmin || at[i].n < nmin))
                    nmin = at[i].n;
                if (ok && at[i].n > 1 && (sok & (1 << i)))
                    grp_vec = TRUE;
            }
            for (int i = 0; i < na && ok && nmin; i++) {
                if (at[i].n > nmin)
                    wrap[i] = nmin;                       /* truncate */
                else if (at[i].n == 1 && (!(sok & (1 << i)) || grp_vec))
                    wrap[i] = nmin;                       /* splat */
            }
        }
        gboolean any = FALSE;
        for (int i = 0; i < na && ok; i++)
            any |= wrap[i] != 0;
        if (!any) {
            p = q;
            continue;
        }
        GString *out = g_string_new_len (line, (gssize) (o + 1 - code));
        for (int i = 0; i < na; i++) {
            if (i)
                g_string_append (out, ", ");
            if (wrap[i])
                g_string_append_printf (out, "vec%d(%.*s)", wrap[i], (int) (ae[i] - as[i]),
                                        line + (as[i] - code));
            else
                g_string_append_len (out, line + (as[i] - code), ae[i] - as[i]);
        }
        g_string_append (out, line + (close - code));
        return g_string_free (out, FALSE);
    }
    return NULL;
}

/* a call of a function the shader (or an included header) defines, with
 * an argument the parameter does not take in GLSL: HLSL truncates a wider
 * vector (shimmer.frag hands the vec4 v_TexCoord to common.h's
 * rotateVec2 (vec2, float)), splats a scalar and converts bool / uint.
 * Only where the parameter list is known and unambiguous; out / inout
 * parameters stay as written. */
static char *fx_ls_usercalls (FxAn *an, const char *line, const char *code)
{
    for (const char *p = code; *p;) {
        if (!fx_is_ident_start (*p) || (p > code && (fx_is_ident (p[-1]) || p[-1] == '.'))) {
            p++;
            continue;
        }
        const char *q = p;
        while (fx_is_ident (*q))
            q++;
        const char *o = fx_sp (q);
        gsize len = (gsize) (q - p);
        FxTy ctor;
        const char *close = *o == '(' ? fx_paren_end (o) : NULL;
        if (!close || fx_builtin_find (p, len) >= 0 || fx_ty_parse (p, len, &ctor)) {
            p = q;
            continue;
        }
        char *name = g_strndup (p, len);
        const FxSig *sig = g_hash_table_contains (an->macros, name)
                               ? NULL : g_hash_table_lookup (an->sigs, name);
        g_free (name);
        /* not the header itself: "vec2 rotateVec2 (vec2 v, float r)" */
        const char *b = p, *b0;
        while (b > code && (b[-1] == ' ' || b[-1] == '\t'))
            b--;
        for (b0 = b; b0 > code && fx_is_ident (b0[-1]); b0--)
            ;
        if (!sig || sig->n < 1 || (b > b0 && fx_ty_parse (b0, (gsize) (b - b0), &ctor))) {
            p = q;
            continue;
        }
        const char *as[8], *ae[8], *s = o + 1;
        const char *wrap[8] = { NULL };
        int na = 0, d = 0;
        gboolean ok = TRUE, any = FALSE;
        for (const char *c = o + 1; c <= close && ok; c++) {
            if (c < close && (*c == '(' || *c == '['))
                d++;
            else if (c < close && (*c == ')' || *c == ']'))
                d--;
            else if (c == close || (*c == ',' && d == 0)) {
                if (na == sig->n) {
                    ok = FALSE;
                    break;
                }
                as[na] = fx_sp (s);
                ae[na] = c;
                while (ae[na] > as[na] && (ae[na][-1] == ' ' || ae[na][-1] == '\t'))
                    ae[na]--;
                FxTy at = fx_infer (an, as[na], ae[na]);
                if (!sig->out[na] && at.b != FXB_UNK && fx_needs_cast (sig->t[na], at)) {
                    wrap[na] = fx_ty_name (sig->t[na]);
                    any = TRUE;
                }
                na++;
                s = c + 1;
            }
        }
        if (!ok || na != sig->n || !any) {
            p = q;
            continue;
        }
        GString *out = g_string_new_len (line, (gssize) (o + 1 - code));
        for (int i = 0; i < na; i++) {
            if (i)
                g_string_append (out, ", ");
            if (wrap[i])
                g_string_append_printf (out, "%s(%.*s)", wrap[i], (int) (ae[i] - as[i]),
                                        line + (as[i] - code));
            else
                g_string_append_len (out, line + (as[i] - code), ae[i] - as[i]);
        }
        g_string_append (out, line + (close - code));
        return g_string_free (out, FALSE);
    }
    return NULL;
}

/* "a + b", "a - b", "a * b", "a / b" between float vectors of different
 * sizes: HLSL truncates the wider one ("v_TexCoord.xy - (k * vec4 + f)",
 * "vec3 - CAST2 (x)" in Workshop shaders), GLSL has no such operation.
 * The wider operand gets the constructor of the narrower size; these
 * operators work per component, so truncating an operand is truncating
 * the result. Matrices and scalars are left alone. */
static char *fx_ls_mixed (FxAn *an, const char *line, const char *code)
{
    for (const char *op = code; *op; op++) {
        char c = *op;
        if (c != '+' && c != '-' && c != '*' && c != '/')
            continue;
        if (op[1] == '=' || op[1] == c || (op > code && op[-1] == c) ||
            (c == '/' && op[1] == '*') || (c == '*' && op[1] == '/'))
            continue;   /* += ++ // comment marks */
        if ((c == '+' || c == '-') && op > code + 1 && (op[-1] == 'e' || op[-1] == 'E') &&
            (g_ascii_isdigit (op[-2]) || op[-2] == '.'))
            continue;   /* 1e-5 */
        const char *a = fx_operand_start (code, op), *a1 = op;
        while (a1 > a && (a1[-1] == ' ' || a1[-1] == '\t'))
            a1--;
        const char *b0 = fx_sp (op + 1), *b = fx_operand_end (op + 1);
        /* selectors after a call or a subscript belong to the operand:
         * "mul (a, b).xyz", "m[0].xy" */
        while (b > b0) {
            if (*b == '.' && fx_is_ident_start (b[1])) {
                for (b++; fx_is_ident (*b); b++)
                    ;
            } else if (*b == '[' || *b == '(') {
                char o = *b, cl = o == '(' ? ')' : ']';
                int d = 0;
                do {
                    if (*b == o)
                        d++;
                    else if (*b == cl)
                        d--;
                    b++;
                } while (*b && d > 0);
            } else {
                break;
            }
        }
        if (a1 <= a || b <= b0)
            continue;   /* unary, or nothing this model can delimit */
        FxTy ta = fx_infer (an, a, a1), tb = fx_infer (an, b0, b);
        if (ta.b != FXB_FLOAT || tb.b != FXB_FLOAT || ta.arr || tb.arr || ta.n < 2 || tb.n < 2 ||
            ta.n > 4 || tb.n > 4 || ta.n == tb.n)
            continue;
        char ctor[8];
        g_snprintf (ctor, sizeof ctor, "vec%d", MIN (ta.n, tb.n));
        return ta.n > tb.n ? fx_wrap (line, code, a, a1, ctor) : fx_wrap (line, code, b0, b, ctor);
    }
    return NULL;
}

/* "varying vec4 v_Size.xy;" at file scope: a member selector on the name
 * being declared (a Workshop author's note of what the components hold).
 * No GLSL compiler takes such a declarator; the name is what is declared,
 * the selector goes. */
static char *fx_ls_declsel (FxAn *an, const char *line, const char *code)
{
    if (an->depth != 0 || an->par0 != 0)
        return NULL;
    const char *w = fx_sp (code), *we = w;
    while (fx_is_ident (*we))
        we++;
    gsize wl = (gsize) (we - w);
    if (!((wl == 7 && (!strncmp (w, "varying", 7) || !strncmp (w, "uniform", 7))) ||
          (wl == 9 && !strncmp (w, "attribute", 9))))
        return NULL;
    const char *t = fx_sp (we), *te = t;
    while (fx_is_ident (*te))
        te++;
    FxTy ty;
    if (!fx_ty_parse (t, (gsize) (te - t), &ty))
        return NULL;
    const char *n = fx_sp (te), *ne = n;
    while (fx_is_ident (*ne))
        ne++;
    const char *dot = fx_sp (ne);
    if (ne == n || *dot != '.')
        return NULL;
    const char *s = fx_sp (dot + 1), *se = s;
    while (fx_is_ident (*se))
        se++;
    if (se == s || *fx_sp (se) != ';')
        return NULL;
    fx_an_declare (an, n, ne, ty, FALSE, 0);
    return g_strdup_printf ("%.*s%s", (int) (ne - code), line, line + (se - code));
}

/* "x ? a : b" with a number as condition ("float outside = step (...);
 * vec4 c = outside ? in : out;"): HLSL takes non-zero as true, GLSL wants
 * a bool. The condition becomes "(x) != 0.0" (ints: "!= 0"). Vector
 * conditions (HLSL selects per component) are left alone. */
static char *fx_ls_ternary (FxAn *an, const char *line, const char *code)
{
    for (const char *q = code; (q = strchr (q, '?')) != NULL; q++) {
        /* the condition: back to what opened or separated this expression */
        const char *s = q;
        int d = 0;
        while (s > code) {
            char c = s[-1];
            if (c == ')' || c == ']') {
                d++;
            } else if (c == '(' || c == '[') {
                if (d == 0)
                    break;
                d--;
            } else if (d == 0) {
                if (c == ',' || c == ';' || c == '{' || c == '}' || c == '?' || c == ':')
                    break;
                if (c == '=' && s[0] != '=' && (s < code + 2 || !strchr ("=!<>", s[-2])))
                    break;   /* an assignment, not == != <= >= */
            }
            s--;
        }
        s = fx_sp (s);
        if (!strncmp (s, "return", 6) && !fx_is_ident (s[6]))
            s = fx_sp (s + 6);
        const char *e = q;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        if (e <= s)
            continue;
        FxTy t = fx_infer (an, s, e);
        if (t.arr || t.n != 1 || (t.b != FXB_FLOAT && t.b != FXB_INT && t.b != FXB_UINT))
            continue;
        return g_strdup_printf ("%.*s(%.*s) != %s%s", (int) (s - code), line, (int) (e - s),
                                line + (s - code), t.b == FXB_FLOAT ? "0.0" : t.b == FXB_UINT ? "0u" : "0",
                                line + (e - code));
    }
    return NULL;
}

/* "T name = expr" whose expr is not a T */
static char *fx_ls_decl (FxAn *an, const char *line, const char *code)
{
    for (const char *p = code; *p;) {
        if (!fx_is_ident_start (*p) || (p > code && (fx_is_ident (p[-1]) || p[-1] == '.'))) {
            p++;
            continue;
        }
        const char *q = p;
        while (fx_is_ident (*q))
            q++;
        FxTy T, t2;
        const char *n = fx_sp (q);
        if (!fx_ty_parse (p, (gsize) (q - p), &T) || !fx_ty_name (T) || !fx_is_ident_start (*n)) {
            p = q;
            continue;
        }
        for (;;) {
            const char *ne = n;
            while (fx_is_ident (*ne))
                ne++;
            const char *a = fx_sp (ne);
            if (ne == n || fx_ty_parse (n, (gsize) (ne - n), &t2))
                break;
            const char *e;
            if (*a == '=' && a[1] != '=') {
                const char *s = fx_sp (a + 1);
                e = fx_expr_end (s);
                if (*e != ';' && *e != ',')
                    break; /* the statement goes on in the next line */
                FxTy t = fx_infer (an, s, e);
                if (fx_needs_cast (T, t))
                    return fx_wrap (line, code, s, e, fx_ty_name (T));
            } else if (*a == ',') {
                e = a;
            } else {
                break;
            }
            if (*e != ',')
                break;
            n = fx_sp (e + 1);
        }
        p = q;
    }
    return NULL;
}

static const char *fx_find_word (const char *code, const char *w)
{
    gsize n = strlen (w);
    for (const char *p = code; (p = strstr (p, w)) != NULL; p += n)
        if ((p == code || !fx_is_ident (p[-1])) && !fx_is_ident (p[n]))
            return p;
    return NULL;
}

/* "return expr;" whose expr is not the function's return type */
static char *fx_ls_return (FxAn *an, const char *line, const char *code)
{
    const char *r = fx_find_word (code, "return");
    if (!r || !fx_ty_name (an->cur_ret))
        return NULL;
    const char *s = fx_sp (r + 6), *e = strchr (s, ';');
    if (!e || e == s)
        return NULL;
    FxTy t = fx_infer (an, s, e);
    if (!fx_needs_cast (an->cur_ret, t))
        return NULL;
    return fx_wrap (line, code, s, e, fx_ty_name (an->cur_ret));
}

/* "x op= expr;" and "x = expr;" with HLSL implicit conversions. force:
 * wrap whenever the target's type is known (error-driven candidates). */
static char *fx_ls_assign (FxAn *an, const char *line, const char *code, gboolean compound,
                           gboolean force)
{
    int d = 0;
    for (const char *s = code; *s; s++) {
        if (*s == '(' || *s == '[')
            d++;
        else if (*s == ')' || *s == ']')
            d--;
        if (*s != '=' || s[1] == '=' || s == code)
            continue;
        gboolean cmp = strchr ("+-*/", s[-1]) != NULL;
        if (cmp != compound || strchr ("=!<>%&|^", s[-1]))
            continue;
        if (!compound && d != 0)
            continue; /* "for (i = 0; ...": not a statement of its own */
        const char *op = compound ? s - 1 : s;
        const char *l0 = fx_lvalue_start (code, op);
        if (!l0)
            continue;
        if (!compound) {
            /* a declaration ("float x = ...") is fx_ls_decl's */
            const char *b = l0;
            while (b > code && (b[-1] == ' ' || b[-1] == '\t'))
                b--;
            if (b > code && fx_is_ident (b[-1]) &&
                !(b - code >= 4 && !strncmp (b - 4, "else", 4) &&
                  (b - code == 4 || !fx_is_ident (b[-5]))))
                continue;
        }
        const char *l1 = op;
        while (l1 > l0 && (l1[-1] == ' ' || l1[-1] == '\t'))
            l1--;
        const char *r = fx_sp (s + 1), *e = fx_expr_end (r);
        if (*e != ';' || e == r)
            continue;
        FxTy T = fx_infer (an, l0, l1), t = fx_infer (an, r, e);
        if (!fx_ty_name (T))
            continue;
        gboolean cast;
        if (force) {
            gsize tl = strlen (fx_ty_name (T));
            cast = !(!strncmp (r, fx_ty_name (T), tl) && *fx_sp (r + tl) == '(' &&
                     fx_paren_end (fx_sp (r + tl)) == e - 1);
        } else if (!compound) {
            cast = fx_needs_cast (T, t);
        } else {
            (void) fx_combine (an, T, t, '*');
            /* GLSL takes vec op= scalar, same sizes, float op= int */
            cast = (t.b == FXB_BOOL && t.n == 1 && !t.arr) ||
                   (fx_ty_num (t) && ((T.n == 1 && t.n > 1) || (T.n > 1 && t.n > T.n) ||
                                      (T.b != FXB_FLOAT && t.b == FXB_FLOAT && t.n <= T.n)));
        }
        if (cast)
            return fx_wrap (line, code, r, e, fx_ty_name (T));
    }
    return NULL;
}

/* the first repair the source asks for on this line, or NULL */
static char *fx_line_static (FxAn *an, const char *line, const char *code)
{
    char *r;
    if (strlen (code) > FX_MAX_REPAIR_LINE)
        return NULL;
    if ((r = fx_ls_modulo (an, line, code)) || (r = fx_ls_boolarith (an, line, code)) ||
        (r = fx_ls_calls (an, line, code)) || (r = fx_ls_usercalls (an, line, code)) ||
        (r = fx_ls_mixed (an, line, code)) || (r = fx_ls_declsel (an, line, code)) ||
        (r = fx_ls_ternary (an, line, code)) ||
        (r = fx_ls_decl (an, line, code)) ||
        (r = fx_ls_return (an, line, code)) || (r = fx_ls_assign (an, line, code, TRUE, FALSE)) ||
        (r = fx_ls_assign (an, line, code, FALSE, FALSE)))
        return r;
    return NULL;
}

/* walk lines [0, upto): preprocessor state, symbols, scopes; with repair,
 * rewrite the body lines (index >= base) the source proves wrong */
static gboolean fx_an_scan (FxAn *an, char **lines, guint nl, guint upto, guint base,
                            gboolean repair)
{
    gboolean changed = FALSE;
    for (guint i = 0; i < nl && i < upto; i++) {
        gboolean c0 = an->in_comment;
        char *code = fx_code_view (lines[i], &an->in_comment);
        const char *d = fx_sp (code);
        if (*d == '#') {
            char *fixed = fx_an_directive (an, lines[i], d, repair && i >= base);
            if (fixed) {
                fx_say (G_LOG_LEVEL_DEBUG, "fx: source repair line %u: %s  ->  %s", i + 1 - base, lines[i],
                         fixed);
                g_free (lines[i]);
                lines[i] = fixed;
                changed = TRUE;
                an->n_repairs++;
            }
            g_free (code);
            continue;
        }
        if (!*d || !fx_an_active (an)) {
            g_free (code);
            continue;
        }
        if (an->depth == 0 && an->par0 == 0)
            fx_an_varying (an, code, i);
        fx_an_decls (an, code);
        for (int it = 0; repair && i >= base && it < 8; it++) {
            char *fixed = fx_line_static (an, lines[i], code);
            if (!fixed)
                break;
            fx_say (G_LOG_LEVEL_DEBUG, "fx: source repair line %u: %s  ->  %s", i + 1 - base, lines[i],
                         fixed);
            g_free (lines[i]);
            lines[i] = fixed;
            changed = TRUE;
            an->n_repairs++;
            g_free (code);
            gboolean c = c0;
            code = fx_code_view (lines[i], &c);
        }
        fx_an_braces (an, code);
        g_free (code);
    }
    return changed;
}

/* the compiled code of a source: lines of dead #if branches, directives,
 * comments and all white space removed (for textual checks) */
static char *fx_active_code_of (const char *src)
{
    char **lines = g_strsplit (src, "\n", -1);
    FxAn an;
    fx_an_init (&an);
    GString *o = g_string_new (NULL);
    for (char **l = lines; *l; l++) {
        char *code = fx_code_view (*l, &an.in_comment);
        const char *d = fx_sp (code);
        if (*d == '#') {
            g_free (fx_an_directive (&an, *l, d, FALSE));
        } else if (fx_an_active (&an)) {
            for (const char *p = d; *p; p++)
                if (!g_ascii_isspace (*p))
                    g_string_append_c (o, *p);
        }
        g_free (code);
    }
    fx_an_clear (&an);
    g_strfreev (lines);
    return g_string_free (o, FALSE);
}

/* ... once per text while a scene load keeps a memo (the traits of a
 * pass ask for the same two texts several times, and the next pass of
 * the same shader asks again) */
static char *fx_active_code (const char *src)
{
    if (!fx_memo.active)
        return fx_active_code_of (src);
    const char *kept = g_hash_table_lookup (fx_memo.active, src);
    if (kept) {
        g_atomic_int_inc (&fx_memo_reused);
        return g_strdup (kept);
    }
    char *r = fx_active_code_of (src);
    g_hash_table_insert (fx_memo.active, g_strdup (src), g_strdup (r));
    g_atomic_int_inc (&fx_memo_computed);
    return r;
}

/* index of the first body line (after the prelude's "#line 1") */
static guint fx_body_base (char **lines, guint nl)
{
    for (guint i = 0; i < nl; i++)
        if (!strcmp (lines[i], "#line 1"))
            return i + 1;
    return 0;
}

/* "3u" -> "3", "0x7fu" -> "0x7f" in the body (with fx_uint_to_int: int
 * throughout; GLSL keeps a literal's bit pattern) */
static void fx_strip_uint_suffix (char **lines, guint base, guint nl)
{
    GRegex *rx = g_regex_new ("\\b(0[xX][0-9a-fA-F]+|\\d+)[uU]\\b", 0, 0, NULL);
    for (guint i = base; i < nl; i++) {
        char *r = g_regex_replace (rx, lines[i], -1, 0, "\\1", 0, NULL);
        if (!r)
            continue;   /* not UTF-8: left as it is */
        g_free (lines[i]);
        lines[i] = r;
    }
    g_regex_unref (rx);
}

/* stage 1 on one shader source. Returns the number of repairs. */
static int fx_prepass_src (char **src)
{
    char **lines = g_strsplit (*src, "\n", -1);
    guint nl = g_strv_length (lines), base = fx_body_base (lines, nl);
    int n = 0;
    gboolean changed = FALSE;
    for (int pass = 0; pass < 2; pass++) {
        FxAn an;
        fx_an_init (&an);
        changed |= fx_an_scan (&an, lines, nl, nl, base, TRUE);
        n += an.n_repairs;
        gboolean redo = pass == 0 && an.mix_uint && fx_uint_to_int (lines, base, nl);
        fx_an_clear (&an);
        if (!redo)
            break;
        /* HLSL mixes uint and int freely: int throughout, then again */
        fx_strip_uint_suffix (lines, base, nl);
        changed = TRUE;
        n++;
    }
    if (changed) {
        g_free (*src);
        *src = g_strjoinv ("\n", lines);
    }
    g_strfreev (lines);
    return n;
}

/* ---- vertex output / fragment input of different width */

static void fx_collect_varyings (char **lines, guint nl, gboolean lenient, GArray *out)
{
    FxAn an;
    fx_an_init (&an);
    if (lenient) {
        /* every declaration, compiled or not */
        gboolean blk = FALSE;
        for (guint i = 0; i < nl; i++) {
            char *code = fx_code_view (lines[i], &blk);
            if (!strncmp (fx_sp (code), "varying", 7))
                fx_an_varying (&an, code, i);
            g_free (code);
        }
    } else {
        fx_an_scan (&an, lines, nl, nl, 0, FALSE);
    }
    for (guint i = 0; i < an.vary->len; i++) {
        FxVary v = g_array_index (an.vary, FxVary, i);
        v.name = g_strdup (v.name);
        g_array_append_val (out, v);
    }
    fx_an_clear (&an);
}

/* the one type a shader declares for name (lenient: the widest); FALSE
 * when it is not declared or the declarations disagree */
static gboolean fx_varying_type (GArray *v, const char *name, gboolean lenient, FxTy *t)
{
    gboolean hit = FALSE;
    for (guint i = 0; i < v->len; i++) {
        FxVary *e = &g_array_index (v, FxVary, i);
        if (strcmp (e->name, name))
            continue;
        if (hit && (e->t.b != t->b || e->t.n != t->n || e->t.arr != t->arr)) {
            if (!lenient || e->t.b != t->b || e->t.arr || t->arr)
                return FALSE;
            if (e->t.n > t->n)
                *t = e->t;
            continue;
        }
        if (!hit)
            *t = e->t;
        hit = TRUE;
    }
    return hit;
}

/* declare name with the wider type and read / write its first components
 * everywhere (HLSL passes the wider interpolator) */
static void fx_widen_varying (char **lines, guint nl, guint base, GArray *decls, const char *name,
                              FxTy narrow, FxTy wide)
{
    static const char *sw[] = { "", "x", "xy", "xyz" };
    char *pat = g_strdup_printf ("\\b%s\\b(?!\\s*\\.)", name);
    GRegex *rx = g_regex_new (pat, 0, 0, NULL);
    g_free (pat);
    char *repl = g_strdup_printf ("%s.%s", name, sw[narrow.n]);
    for (guint i = base; i < nl; i++) {
        gboolean decl = FALSE;
        for (guint k = 0; k < decls->len && !decl; k++) {
            FxVary *e = &g_array_index (decls, FxVary, k);
            decl = e->line == i && !strcmp (e->name, name);
        }
        if (decl) {
            /* re-declare this line's names one by one */
            GString *o = g_string_new (NULL);
            for (guint k = 0; k < decls->len; k++) {
                FxVary *e = &g_array_index (decls, FxVary, k);
                if (e->line != i)
                    continue;
                gboolean me = !strcmp (e->name, name);
                g_string_append_printf (o, "%svarying %s %s;", o->len ? " " : "",
                                        fx_ty_name (me ? wide : e->t), e->name);
            }
            g_free (lines[i]);
            lines[i] = g_string_free (o, FALSE);
        } else if (rx && *fx_trim (lines[i]) != '#' && strstr (lines[i], name)) {
            char *r = g_regex_replace_literal (rx, lines[i], -1, 0, repl, 0, NULL);
            if (r) {
                g_free (lines[i]);
                lines[i] = r;
            }
        }
    }
    g_free (repl);
    if (rx)
        g_regex_unref (rx);
}

/* TRUE when a source changed. Strict: declarations the preprocessor model
 * proves compiled, one type per name. Lenient (after a failed link, any
 * log): every declaration, the widest type. */
static gboolean fx_varying_repair (char **vsrc, char **fsrc, gboolean lenient)
{
    if (!fx_repairable (*vsrc) || !fx_repairable (*fsrc))
        return FALSE;
    char **vl = g_strsplit (*vsrc, "\n", -1), **fl = g_strsplit (*fsrc, "\n", -1);
    guint nv = g_strv_length (vl), nf = g_strv_length (fl);
    GArray *vv = g_array_new (FALSE, FALSE, sizeof (FxVary));
    GArray *fv = g_array_new (FALSE, FALSE, sizeof (FxVary));
    g_array_set_clear_func (vv, fx_vary_clear);
    g_array_set_clear_func (fv, fx_vary_clear);
    fx_collect_varyings (vl, nv, lenient, vv);
    fx_collect_varyings (fl, nf, lenient, fv);
    gboolean cv = FALSE, cf = FALSE;
    GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);
    for (guint i = 0; i < fv->len; i++) {
        const char *name = g_array_index (fv, FxVary, i).name;
        FxTy tv, tf;
        if (!g_hash_table_add (seen, (gpointer) name) || !strncmp (name, "gl_", 3) ||
            !fx_varying_type (vv, name, lenient, &tv) || !fx_varying_type (fv, name, lenient, &tf))
            continue;
        if (tv.b != FXB_FLOAT || tf.b != FXB_FLOAT || tv.arr || tf.arr || tv.n == tf.n)
            continue;
        if (tv.n > tf.n) {
            fx_widen_varying (fl, nf, fx_body_base (fl, nf), fv, name, tf, tv);
            cf = TRUE;
            fx_say (G_LOG_LEVEL_MESSAGE, "fx: varying %s: fragment input widened %s -> %s (HLSL semantics)",
                    name, fx_ty_name (tf), fx_ty_name (tv));
        } else {
            fx_widen_varying (vl, nv, fx_body_base (vl, nv), vv, name, tv, tf);
            cv = TRUE;
            fx_say (G_LOG_LEVEL_MESSAGE, "fx: varying %s: vertex output widened %s -> %s (HLSL semantics)",
                    name, fx_ty_name (tv), fx_ty_name (tf));
        }
    }
    g_hash_table_unref (seen);
    g_array_unref (vv);
    g_array_unref (fv);
    if (cv) {
        g_free (*vsrc);
        *vsrc = g_strjoinv ("\n", vl);
    }
    if (cf) {
        g_free (*fsrc);
        *fsrc = g_strjoinv ("\n", fl);
    }
    g_strfreev (vl);
    g_strfreev (fl);
    return cv || cf;
}

/* ---- macro expansion size ----------------------------------------------
 * What the driver's preprocessor would have to produce. "#define A0
 * (A1+A1+...+A1)" over eight levels is 16.7 million tokens: a minute and
 * gigabytes in Mesa's compiler, sixteen-fold the OOM killer - from 1 KB of
 * wallpaper data. The size is computed, never produced: an object-like
 * macro once (memo), a function-like one per call from the sizes of its
 * arguments. Conservative: #define lines count whatever #if they are in.
 * Real effect shaders expand to a few 10000 tokens. */
#define FX_MAX_EXPANSION 1000000

typedef struct {
    char *body;
    char **params;   /* NULL = object-like */
    int np;
    gint64 size;     /* object-like: tokens it expands to, -1 = not known yet */
    gboolean busy;   /* being expanded: a macro is not expanded inside itself */
} FxMac;

typedef struct { GHashTable *macros; gint64 work; } FxMx;

static void fx_mac_free (gpointer p)
{
    FxMac *m = p;
    g_free (m->body);
    g_strfreev (m->params);
    g_free (m);
}

/* tokens the text [s, e) expands to; in / argc: the function-like macro
 * whose body this is and the sizes of its arguments */
static gint64 fx_mx_text (FxMx *m, const char *s, const char *e, const FxMac *in,
                          const gint64 *argc, int depth)
{
    gint64 total = 0;
    const char *p = s;
    while (p < e && total <= FX_MAX_EXPANSION && m->work <= FX_MAX_EXPANSION) {
        if (g_ascii_isspace (*p)) {
            p++;
            continue;
        }
        m->work++;
        if (!fx_is_ident_start (*p)) {
            if (g_ascii_isdigit (*p))
                while (p + 1 < e && (g_ascii_isalnum (p[1]) || p[1] == '.'))
                    p++;
            p++;
            total++;
            continue;
        }
        const char *q = p;
        while (q < e && fx_is_ident (*q))
            q++;
        gsize len = (gsize) (q - p);
        int pi = -1;
        for (int i = 0; in && i < in->np && pi < 0; i++)
            if (strlen (in->params[i]) == len && !strncmp (in->params[i], p, len))
                pi = i;
        char *name = pi < 0 ? g_strndup (p, len) : NULL;
        FxMac *mac = name ? g_hash_table_lookup (m->macros, name) : NULL;
        g_free (name);
        p = q;
        if (pi >= 0) {
            total += argc[pi];
        } else if (!mac || mac->busy) {
            total++;
        } else if (depth > 200 || mac->np > 64) {
            /* macros nested 200 deep, 65 parameters: not a shader */
            m->work = (gint64) FX_MAX_EXPANSION + 1;
        } else if (!mac->params) {
            if (mac->size < 0) {
                mac->busy = TRUE;
                mac->size = fx_mx_text (m, mac->body, mac->body + strlen (mac->body), NULL, NULL,
                                        depth + 1);
                mac->busy = FALSE;
            }
            total += mac->size;
        } else {
            const char *o = p;
            while (o < e && g_ascii_isspace (*o))
                o++;
            if (o >= e || *o != '(') {
                total++;   /* the name without a call */
                continue;
            }
            gint64 ac[64] = { 0 };
            int na = 0, d = 0;
            const char *a = o + 1, *c = o + 1;
            for (; c < e; c++) {
                if (*c == '(' || *c == '[') {
                    d++;
                } else if ((*c == ')' || *c == ']') && d > 0) {
                    d--;
                } else if (*c == ')' || (*c == ',' && d == 0)) {
                    gint64 v = fx_mx_text (m, a, c, in, argc, depth + 1);
                    if (na < 64)
                        ac[na] = v;
                    na++;
                    a = c + 1;
                    if (*c == ')')
                        break;
                }
            }
            mac->busy = TRUE;
            total += fx_mx_text (m, mac->body, mac->body + strlen (mac->body), mac, ac, depth + 1);
            mac->busy = FALSE;
            p = c < e ? c + 1 : e;
        }
    }
    return MIN (total, (gint64) FX_MAX_EXPANSION + 1);
}

/* ---- statement size ------------------------------------------------------
 * A driver's compiler walks an expression by recursion, and one statement
 * may be as long as the source: on a very long statement the compiler can
 * run out of stack (seen with Mesa 25.2, on statements several times as
 * long as the limit below; the process then ends inside glCompileShader),
 * and long chains of some operators take it seconds, growing with the
 * square of their length.
 * A statement of more than FX_MAX_STATEMENT tokens - at most half as many
 * terms - is not handed to the driver. The longest statement in the
 * shaders of a Wallpaper Engine installation and of 88 Workshop scenes
 * (1552 files, macros expanded) has 575 tokens. This bounds neither how
 * deep calls nest nor how many statements there are, and the margin is
 * that of a thread with a stack of 8 MiB. */
#define FX_MAX_STATEMENT 8192

/* FALSE: the source expands beyond FX_MAX_EXPANSION tokens. *statement
 * (when asked for): the tokens of its longest statement - what stands
 * between two of ';', '{' and '}', macros expanded. A macro counts as part
 * of the statement that uses it, whatever its body holds, and lines of
 * dead #if branches count like the others. */
static gboolean fx_expansion_sizes (const char *src, gint64 *statement)
{
    FxMx m = { g_hash_table_new_full (g_str_hash, g_str_equal, g_free, fx_mac_free), 0 };
    gint64 total = 0, run = 0, longest = 0;
    GString *ln = g_string_new (NULL);
    for (const char *p = src; *p && total <= FX_MAX_EXPANSION && m.work <= FX_MAX_EXPANSION;) {
        /* one logical line (backslash-newline joined) */
        g_string_truncate (ln, 0);
        for (;;) {
            const char *nl = strchr (p, '\n');
            gsize n = nl ? (gsize) (nl - p) : strlen (p);
            g_string_append_len (ln, p, (gssize) n);
            p += n + (nl ? 1 : 0);
            while (ln->len && ln->str[ln->len - 1] == '\r')
                g_string_truncate (ln, ln->len - 1);
            if (!nl || !ln->len || ln->str[ln->len - 1] != '\\')
                break;
            ln->str[ln->len - 1] = ' ';
        }
        const char *d = fx_sp (ln->str), *end = ln->str + ln->len;
        if (*d != '#') {
            /* statement by statement */
            for (const char *s = d; total <= FX_MAX_EXPANSION && m.work <= FX_MAX_EXPANSION;) {
                const char *q = s;
                while (q < end && *q != ';' && *q != '{' && *q != '}')
                    q++;
                gint64 n = fx_mx_text (&m, s, q, NULL, NULL, 0);
                total += n;
                run += n;
                longest = MAX (longest, run);
                if (q >= end)
                    break;
                total++;   /* the ';', '{' or '}' */
                m.work++;
                run = 0;
                s = q + 1;
            }
            continue;
        }
        const char *w = fx_sp (d + 1), *we = w;
        while (fx_is_ident (*we))
            we++;
        gsize wl = (gsize) (we - w);
        if ((wl == 2 && !strncmp (w, "if", 2)) || (wl == 4 && !strncmp (w, "elif", 4))) {
            total += fx_mx_text (&m, we, end, NULL, NULL, 0);
        } else if ((wl == 6 && !strncmp (w, "define", 6)) || (wl == 5 && !strncmp (w, "undef", 5))) {
            const char *n = fx_sp (we), *ne = n;
            while (fx_is_ident (*ne))
                ne++;
            if (ne == n)
                continue;
            char *name = g_strndup (n, (gsize) (ne - n));
            if (wl == 5) {
                g_hash_table_remove (m.macros, name);
                g_free (name);
                continue;
            }
            FxMac *mac = g_new0 (FxMac, 1);
            mac->size = -1;
            const char *b = ne;
            if (*ne == '(') {
                const char *ce = strchr (ne, ')');
                char *plist = g_strndup (ne + 1, ce ? (gsize) (ce - ne - 1) : 0);
                mac->params = g_strsplit (plist, ",", -1);
                g_free (plist);
                for (mac->np = 0; mac->params[mac->np]; mac->np++)
                    g_strstrip (mac->params[mac->np]);
                b = ce ? ce + 1 : end;
            }
            mac->body = g_strdup (b);
            g_hash_table_insert (m.macros, name, mac);
        }
    }
    gboolean ok = total <= FX_MAX_EXPANSION && m.work <= FX_MAX_EXPANSION;
    g_string_free (ln, TRUE);
    g_hash_table_unref (m.macros);
    if (statement)
        *statement = longest;
    return ok;
}

/* ... once per text while a scene load keeps a memo: the answer and the
 * longest statement are kept together (statement * 4 + 2 for "fits",
 * + 1 for "expands too far") */
static gboolean fx_expansion_sizes_once (const char *src, gint64 *statement)
{
    if (!fx_memo.expansion)
        return fx_expansion_sizes (src, statement);
    gpointer kept = g_hash_table_lookup (fx_memo.expansion, src);
    if (kept) {
        gsize v = GPOINTER_TO_SIZE (kept);
        g_atomic_int_inc (&fx_memo_reused);
        if (statement)
            *statement = (gint64) (v >> 2);
        return (v & 3) == 2;
    }
    gint64 st = 0;
    gboolean ok = fx_expansion_sizes (src, &st);
    gsize kept_st = (gsize) MIN (st, (gint64) (G_MAXSIZE >> 2));
    g_hash_table_insert (fx_memo.expansion, g_strdup (src), GSIZE_TO_POINTER ((kept_st << 2) | (ok ? 2 : 1)));
    g_atomic_int_inc (&fx_memo_computed);
    if (statement)
        *statement = st;
    return ok;
}

static inline gboolean fx_expansion_ok (const char *src)
{
    return fx_expansion_sizes_once (src, NULL);
}

/* ---- fragment inputs the shader writes to -------------------------------
 * "v_TexCoord.y += offset;" - in HLSL a pixel shader's inputs are its own
 * copies, in GLSL they are read-only (every driver rejects the write).
 * Such an input gets a global of the same type: every use in the body is
 * renamed to it and main () starts by copying the input. Only plain
 * "varying T name;" declarations of number types. Returns how many. */
static gboolean fx_code_assigns (const char *code, const char *name)
{
    gsize len = strlen (name);
    for (const char *p = code; (p = strstr (p, name)) != NULL; p += len) {
        if ((p > code && (fx_is_ident (p[-1]) || p[-1] == '.')) || fx_is_ident (p[len]))
            continue;
        const char *q = p + len;
        if (*q == '.')
            for (q++; fx_is_ident (*q); q++)
                ;
        if ((*q == '=' && q[1] != '=') ||
            ((*q == '+' || *q == '-' || *q == '*' || *q == '/') && q[1] == '=') ||
            ((*q == '+' || *q == '-') && q[1] == *q))
            return TRUE;
    }
    return FALSE;
}

static char *fx_rename_word (const char *line, const char *name, const char *to)
{
    gsize len = strlen (name);
    GString *o = NULL;
    const char *from = line;
    for (const char *p = line; (p = strstr (p, name)) != NULL;) {
        if ((p > line && fx_is_ident (p[-1])) || fx_is_ident (p[len])) {
            p += len;
            continue;
        }
        if (!o)
            o = g_string_new (NULL);
        g_string_append_len (o, from, p - from);
        g_string_append (o, to);
        p += len;
        from = p;
    }
    if (!o)
        return NULL;
    g_string_append (o, from);
    return g_string_free (o, FALSE);
}

static int fx_input_write_repair (char **fsrc)
{
    char **lines = g_strsplit (*fsrc, "\n", -1);
    guint nl = g_strv_length (lines), base = fx_body_base (lines, nl);
    GArray *decls = g_array_new (FALSE, FALSE, sizeof (FxVary));
    g_array_set_clear_func (decls, fx_vary_clear);
    fx_collect_varyings (lines, nl, FALSE, decls);
    char *active = decls->len ? fx_active_code (*fsrc) : NULL;
    int n = 0;
    for (guint k = 0; k < decls->len; k++) {
        FxVary *d = &g_array_index (decls, FxVary, k);
        const char *tn = fx_ty_name (d->t);
        if (!tn || d->line < base || d->line >= nl || !fx_code_assigns (active, d->name))
            continue;
        /* the declaration alone on its line: "varying T name;" */
        gboolean blk = FALSE;
        char *dcode = fx_code_view (lines[d->line], &blk);
        const char *w = fx_sp (dcode), *semi = strchr (dcode, ';');
        int words = 0;
        gboolean plain = semi && !semi[1 + strspn (semi + 1, " \t\r")] && !strpbrk (dcode, ",=[(");
        for (const char *p = w; plain && p < semi; words++) {
            while (p < semi && fx_is_ident (*p))
                p++;
            p = fx_sp (p);
            if (p < semi && !fx_is_ident (*p))
                plain = FALSE;
        }
        gsize semi_off = semi ? (gsize) (semi - dcode) : 0;
        g_free (dcode);
        if (!plain || words != 3)
            continue;
        /* the '{' of the compiled main () */
        FxAn an;
        fx_an_init (&an);
        gboolean seen = FALSE;
        guint mline = nl;
        gsize moff = 0;
        for (guint i = 0; i < nl && mline == nl; i++) {
            char *code = fx_code_view (lines[i], &an.in_comment);
            const char *s = fx_sp (code);
            if (*s == '#') {
                g_free (fx_an_directive (&an, lines[i], s, FALSE));
            } else if (i >= base && fx_an_active (&an)) {
                const char *m = seen ? code : fx_find_word (code, "main");
                if (m && !seen && *fx_sp (m + 4) == '(')
                    seen = TRUE;
                const char *br = seen && m ? strchr (m, '{') : NULL;
                if (br) {
                    mline = i;
                    moff = (gsize) (br - code) + 1;
                }
            }
            g_free (code);
        }
        fx_an_clear (&an);
        if (mline == nl)
            continue;
        char *copy = g_strconcat ("_weW", d->name, NULL);
        for (guint i = base; i < nl; i++) {
            char *r = i == d->line ? NULL : fx_rename_word (lines[i], d->name, copy);
            if (r) {
                g_free (lines[i]);
                lines[i] = r;
            }
        }
        /* offsets found before the renaming: recompute the brace on mline */
        if (mline != d->line) {
            gboolean b2 = FALSE;
            char *code = fx_code_view (lines[mline], &b2);
            const char *m = fx_find_word (code, "main");
            const char *br = strchr (m ? m : code, '{');
            moff = br ? (gsize) (br - code) + 1 : strlen (code);
            g_free (code);
        }
        char *init = g_strdup_printf ("%.*s %s = %s;%s", (int) moff, lines[mline], copy, d->name,
                                      lines[mline] + moff);
        g_free (lines[mline]);
        lines[mline] = init;
        char *decl = g_strdup_printf ("%.*s %s %s;%s", (int) semi_off + 1, lines[d->line], tn, copy,
                                      lines[d->line] + semi_off + 1);
        fx_say (G_LOG_LEVEL_DEBUG, "fx: source repair line %u: %s  ->  %s", d->line + 1 - base,
                lines[d->line], decl);
        g_free (lines[d->line]);
        lines[d->line] = decl;
        g_free (copy);
        n++;
    }
    if (n) {
        g_free (*fsrc);
        *fsrc = g_strjoinv ("\n", lines);
    }
    g_free (active);
    g_array_unref (decls);
    g_strfreev (lines);
    return n;
}

/* ---- one uniform name, two types ----------------------------------------
 * "uniform vec2 u_res;" in the vertex shader, "uniform float u_res;" in
 * the fragment shader: in HLSL every stage has its own constants, a GLSL
 * program has one uniform of each name and does not link (used or not).
 * The fragment shader's one is renamed to FX_FRAG_UNIFORM_PREFIX + name;
 * fx_cache_uniforms () reads its annotation under the declared name, and
 * for every uniform from the declaration that has its type. */
#define FX_FRAG_UNIFORM_PREFIX "_weF"

/* the compiled "uniform T a, b[2];" declarations of a source */
static void fx_collect_uniforms (char **lines, guint nl, GArray *out)
{
    FxAn an;
    fx_an_init (&an);
    for (guint i = 0; i < nl; i++) {
        char *code = fx_code_view (lines[i], &an.in_comment);
        const char *s = fx_sp (code);
        if (*s == '#') {
            g_free (fx_an_directive (&an, lines[i], s, FALSE));
        } else if (!strncmp (s, "uniform", 7) && !fx_is_ident (s[7]) && fx_an_active (&an)) {
            const char *p = fx_sp (s + 7), *q = p;
            FxTy t;
            for (;;) {
                for (q = p; fx_is_ident (*q); q++)
                    ;
                gsize wl = (gsize) (q - p);
                if (!((wl == 5 && !strncmp (p, "highp", 5)) || (wl == 7 && !strncmp (p, "mediump", 7)) ||
                      (wl == 4 && !strncmp (p, "lowp", 4))))
                    break;
                p = fx_sp (q);
            }
            if (fx_ty_parse (p, (gsize) (q - p), &t) && strchr (q, ';')) {
                for (p = fx_sp (q); fx_is_ident_start (*p);) {
                    for (q = p; fx_is_ident (*q); q++)
                        ;
                    FxVary v = { g_strndup (p, (gsize) (q - p)), t, i };
                    q = fx_sp (q);
                    if (*q == '[') {
                        v.t.arr = 1;
                        while (*q && *q != ']')
                            q++;
                        if (*q)
                            q = fx_sp (q + 1);
                    }
                    g_array_append_val (out, v);
                    if (*q != ',')
                        break;
                    p = fx_sp (q + 1);
                }
            }
        }
        g_free (code);
    }
    fx_an_clear (&an);
}

/* TRUE when both sources declare the uniform `name` and their (first)
 * declarations differ in type */
static gboolean fx_uniform_differs (GArray *vu, GArray *fu, const char *name)
{
    const FxVary *v = NULL, *f = NULL;
    for (guint i = 0; i < vu->len && !v; i++)
        if (!strcmp (g_array_index (vu, FxVary, i).name, name))
            v = &g_array_index (vu, FxVary, i);
    for (guint i = 0; i < fu->len && !f; i++)
        if (!strcmp (g_array_index (fu, FxVary, i).name, name))
            f = &g_array_index (fu, FxVary, i);
    return v && f && (v->t.b != f->t.b || v->t.n != f->t.n || v->t.arr != f->t.arr);
}

/* stage 1: rename such uniforms in the fragment source. Returns how many. */
static int fx_uniform_split_repair (const char *vsrc, char **fsrc)
{
    char **vl = g_strsplit (vsrc, "\n", -1), **fl = g_strsplit (*fsrc, "\n", -1);
    guint nv = g_strv_length (vl), nf = g_strv_length (fl), base = fx_body_base (fl, nf);
    GArray *vu = g_array_new (FALSE, FALSE, sizeof (FxVary)), *fu = g_array_new (FALSE, FALSE, sizeof (FxVary));
    g_array_set_clear_func (vu, fx_vary_clear);
    g_array_set_clear_func (fu, fx_vary_clear);
    fx_collect_uniforms (vl, nv, vu);
    fx_collect_uniforms (fl, nf, fu);
    GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);
    int n = 0;
    for (guint k = 0; k < fu->len; k++) {
        const FxVary *d = &g_array_index (fu, FxVary, k);
        if (d->line < base || !g_hash_table_add (seen, d->name) || !fx_uniform_differs (vu, fu, d->name))
            continue;
        char *to = g_strconcat (FX_FRAG_UNIFORM_PREFIX, d->name, NULL);
        for (guint i = base; i < nf; i++) {
            char *r = fx_rename_word (fl[i], d->name, to);
            if (r) {
                g_free (fl[i]);
                fl[i] = r;
            }
        }
        fx_say (G_LOG_LEVEL_MESSAGE, "fx: uniform %s: declared with another type in the vertex shader, "
                "the fragment shader's one is kept apart as %s (HLSL semantics)", d->name, to);
        g_free (to);
        n++;
    }
    g_hash_table_unref (seen);
    if (n) {
        g_free (*fsrc);
        *fsrc = g_strjoinv ("\n", fl);
    }
    g_array_unref (vu);
    g_array_unref (fu);
    g_strfreev (vl);
    g_strfreev (fl);
    return n;
}

static gboolean fx_env_on (const char *name)
{
    const char *v = g_getenv (name);
    return v && *v && *v != '0';
}

/* stage 1 for one pass: both shaders, then their interface */
static void fx_prepass_said (const char *name, int nv, int nf, gboolean vary)
{
    if (nv || nf || vary)
        g_message ("fx: shader %s: %d HLSL-semantics repair(s) from source inspection "
                   "(vertex %d, fragment %d%s)", name, nv + nf, nv, nf,
                   vary ? ", varying interface" : "");
}

/* behind the repairs: they leave GLSL 3.30, this makes it GLSL ES. Not
 * part of what a scene load's memo keeps (fx_prepass): the kept texts are
 * the repaired GLSL 3.30 ones, and this runs on them for every pass. */
static void fx_prepass_essl (char **vsrc, char **fsrc, const char *name)
{
    if (!fx_essl ())
        return;
    int ev = fx_essl_src (vsrc), ef = fx_essl_src (fsrc);
    if (ev || ef)
        g_debug ("fx: shader %s: %d line(s) rewritten for GLSL ES %d (vertex %d, fragment %d)",
                 name, ev + ef, fx_essl (), ev, ef);
}

static void fx_prepass (char **vsrc, char **fsrc, const char *name)
{
    if (fx_env_on ("WALPE_FX_NO_PREPASS"))
        return;
    if (!fx_repairable (*vsrc) || !fx_repairable (*fsrc)) {
        g_message ("fx: shader %s: a source of more than %d KiB is compiled as written (no "
                   "HLSL-semantics repairs)", name, FX_MAX_REPAIR_SOURCE / 1024);
        return;
    }
    /* the pair of texts decides everything below (the name is only
     * printed): while a scene load keeps a memo, the second pass with
     * this pair takes the first one's result and prints what it printed */
    char *key = NULL;
    if (fx_memo.prepass) {
        key = g_strdup_printf ("%" G_GSIZE_FORMAT ":%s%s", strlen (*vsrc), *vsrc, *fsrc);
        const FxKeptPrepass *k = g_hash_table_lookup (fx_memo.prepass, key);
        if (k) {
            g_free (key);
            for (guint i = 0; i < k->said->len; i++) {
                const FxSaid *e = g_ptr_array_index (k->said, i);
                g_log (G_LOG_DOMAIN, e->level, "%s", e->msg);
            }
            g_free (*vsrc);
            g_free (*fsrc);
            *vsrc = g_strdup (k->vsrc);
            *fsrc = g_strdup (k->fsrc);
            fx_prepass_said (name, k->nv, k->nf, k->vary);
            g_atomic_int_inc (&fx_memo_reused);
            fx_prepass_essl (vsrc, fsrc, name);
            return;
        }
        fx_said_tape = g_ptr_array_new_with_free_func (fx_said_free);
    }
    gboolean vary = fx_varying_repair (vsrc, fsrc, FALSE);
    int nw = fx_input_write_repair (fsrc) + fx_uniform_split_repair (*vsrc, fsrc);
    int nv = fx_prepass_src (vsrc), nf = nw + fx_prepass_src (fsrc);
    if (key) {
        FxKeptPrepass *k = g_new0 (FxKeptPrepass, 1);
        k->vsrc = g_strdup (*vsrc);
        k->fsrc = g_strdup (*fsrc);
        k->said = g_steal_pointer (&fx_said_tape);
        k->nv = nv;
        k->nf = nf;
        k->vary = vary;
        g_hash_table_insert (fx_memo.prepass, key, k);
        g_atomic_int_inc (&fx_memo_computed);
    }
    fx_prepass_said (name, nv, nf, vary);
    fx_prepass_essl (vsrc, fsrc, name);
}

/* ------------------------------------------- stage 2: error-driven fallback */

/* WALPE_FX_LOG_AS: the log as stage 2 sees it (test aid). Mesa's
 * "0:12(5): error: text" re-worded like NVIDIA's / glslang's line, or
 * withheld. Other drivers' logs pass unchanged. */
static void fx_log_view (char *log, gsize size)
{
    const char *as = g_getenv ("WALPE_FX_LOG_AS");
    if (!as || !*as)
        return;
    if (!strcmp (as, "none")) {
        g_strlcpy (log, "(driver log withheld: WALPE_FX_LOG_AS=none)", size);
        return;
    }
    gboolean nv = !strcmp (as, "nvidia");
    GString *o = g_string_new (NULL);
    char **ll = g_strsplit (log, "\n", -1);
    for (char **e = ll; *e; e++) {
        int si = 0, ln = 0, col = 0;
        const char *m = strstr (*e, "error: ");
        if (m && sscanf (*e, "%d:%d(%d)", &si, &ln, &col) == 3) {
            if (nv)
                g_string_append_printf (o, "%d(%d) : error C0000: (reworded) %s\n", si, ln, m + 7);
            else
                g_string_append_printf (o, "ERROR: %d:%d: '' : (reworded) %s\n", si, ln, m + 7);
        } else if (**e) {
            g_string_append_printf (o, "%s\n", *e);
        }
    }
    g_strfreev (ll);
    g_strlcpy (log, o->str, size);
    g_string_free (o, TRUE);
}

/* the source lines a compile log complains about, whatever the driver:
 * the first "<n>:<line>" or "<n>(<line>" of every line that is not a
 * warning. Mesa "0:12(5): error:", NVIDIA "0(12) : error C7011:", glslang
 * / AMD / Intel / Apple "ERROR: 0:12:". No message text is read. */
static void fx_log_error_lines (const char *log, GArray *out)
{
    char **ll = g_strsplit (log, "\n", -1);
    for (char **e = ll; *e; e++) {
        char *lc = g_ascii_strdown (*e, -1);
        gboolean warn = strstr (lc, "warning") && !strstr (lc, "error");
        g_free (lc);
        if (warn)
            continue;
        for (const char *p = *e; *p; p++) {
            if (!g_ascii_isdigit (*p))
                continue;
            const char *q = p;
            while (g_ascii_isdigit (*q))
                q++;
            if ((p == *e || !g_ascii_isalnum (p[-1])) && (*q == ':' || *q == '(') &&
                g_ascii_isdigit (q[1])) {
                int ln = atoi (q + 1);
                gboolean dup = FALSE;
                for (guint i = 0; i < out->len; i++)
                    dup |= g_array_index (out, int, i) == ln;
                if (ln > 0 && !dup)
                    g_array_append_val (out, ln);
                break;
            }
            p = q - 1;
        }
    }
    g_strfreev (ll);
}

/* compile src; FALSE + the driver's log when it does not compile */
typedef gboolean (*FxTryFn) (const char *src, char *log, gsize logsize, gpointer user);

/* at1: the candidate replaces line at1 - 1 instead of the line the log
 * names (0 = that line); only GLSL ES candidates set it */
typedef struct { char *text; gboolean uint_body; guint at1; } FxCand;

static void fx_essl_cands (FxAn *an, char **lines, guint nl, guint base, guint idx, gboolean blk,
                           GPtrArray *out);

static void fx_cand_free (gpointer p)
{
    g_free (((FxCand *) p)->text);
    g_free (p);
}

static void fx_cand_add (GPtrArray *out, const char *orig, char *text)
{
    if (!text)
        return;
    gboolean dup = !strcmp (text, orig);
    for (guint i = 0; i < out->len && !dup; i++) {
        FxCand *c = out->pdata[i];
        dup = c->text && !strcmp (c->text, text);
    }
    if (dup) {
        g_free (text);
        return;
    }
    FxCand *c = g_new0 (FxCand, 1);
    c->text = text;
    g_ptr_array_add (out, c);
}

/* rewrites of one statement line that the source suggests, most specific
 * first. They need not be right: the caller compiles each. */
static void fx_cands_line (FxAn *an, const char *orig, const char *line, const char *before,
                           gboolean blk, GPtrArray *out, int level)
{
    gboolean b = blk;
    char *code = fx_code_view (line, &b);
    guint first = out->len;
    /* what stage 1 would do (it may have been switched off) */
    fx_cand_add (out, orig, fx_line_static (an, line, code));
    fx_cand_add (out, orig, fx_fix_modulo (line));
    fx_cand_add (out, orig, fx_fix_bool_arith (line));
    guint chain_end = out->len;
    /* the declared type / the function's return type / the target's type,
     * without asking what the expression is */
    for (const char *p = code; *p;) {
        if (!fx_is_ident_start (*p) || (p > code && fx_is_ident (p[-1]))) {
            p++;
            continue;
        }
        const char *q = p;
        while (fx_is_ident (*q))
            q++;
        FxTy T;
        const char *n = fx_sp (q), *ne = n;
        while (fx_is_ident (*ne))
            ne++;
        if (fx_ty_parse (p, (gsize) (q - p), &T) && fx_ty_name (T) && ne > n &&
            *fx_sp (ne) == '=' && fx_sp (ne)[1] != '=') {
            char *ty = g_strndup (p, (gsize) (q - p));
            fx_cand_add (out, orig, fx_fix_initializer (line, ty));
            g_free (ty);
            break;
        }
        p = q;
    }
    if (fx_ty_name (an->cur_ret) && fx_find_word (code, "return"))
        fx_cand_add (out, orig, fx_fix_return (line, fx_ty_name (an->cur_ret)));
    fx_cand_add (out, orig, fx_ls_assign (an, line, code, TRUE, TRUE));
    fx_cand_add (out, orig, fx_fix_compound (line, before));
    fx_cand_add (out, orig, fx_ls_assign (an, line, code, FALSE, TRUE));
    /* componentwise builtins: every argument as vecN, widest first */
    for (const char *p = code; *p;) {
        if (!fx_is_ident_start (*p) || (p > code && (fx_is_ident (p[-1]) || p[-1] == '.'))) {
            p++;
            continue;
        }
        const char *q = p;
        while (fx_is_ident (*q))
            q++;
        const char *o = fx_sp (q), *close = *o == '(' ? fx_paren_end (o) : NULL;
        int bi = close ? fx_builtin_find (p, (gsize) (q - p)) : -1;
        if (bi >= 0 && (fx_builtins[bi].rule == FXR_GEN || fx_builtins[bi].rule == FXR_FLOAT)) {
            for (int n = 4; n >= 2; n--) {
                GString *s = g_string_new_len (line, (gssize) (o + 1 - code));
                int d = 0;
                const char *a = o + 1;
                for (const char *c = o + 1; c <= close; c++) {
                    if (c < close && (*c == '(' || *c == '['))
                        d++;
                    else if (c < close && (*c == ')' || *c == ']'))
                        d--;
                    else if (c == close || (*c == ',' && d == 0)) {
                        const char *a0 = fx_sp (a);
                        g_string_append_printf (s, "%svec%d(%.*s)", a == o + 1 ? "" : ", ", n,
                                                (int) (c - a0), line + (a0 - code));
                        a = c + 1;
                    }
                }
                g_string_append (s, line + (close - code));
                fx_cand_add (out, orig, g_string_free (s, FALSE));
            }
        } else if (bi >= 0 && fx_builtins[bi].rule == FXR_VEC4) {
            const char *c = o + 1;
            int d = 0;
            for (; c < close && !(*c == ',' && d == 0); c++) {
                if (*c == '(' || *c == '[')
                    d++;
                else if (*c == ')' || *c == ']')
                    d--;
            }
            if (c < close) {
                const char *a0 = fx_sp (c + 1);
                const char *a1 = a0;
                for (d = 0; a1 < close && !(*a1 == ',' && d == 0); a1++) {
                    if (*a1 == '(' || *a1 == '[')
                        d++;
                    else if (*a1 == ')' || *a1 == ']')
                        d--;
                }
                fx_cand_add (out, orig, fx_wrap (line, code, a0, a1, "vec2"));
            }
        }
        p = q;
    }
    g_free (code);
    /* a second problem on the same line ("int m = i % 3;") */
    for (guint i = first; level == 0 && i < chain_end && i < out->len; i++) {
        char *t = g_strdup (((FxCand *) out->pdata[i])->text);
        fx_cands_line (an, orig, t, before, blk, out, 1);
        g_free (t);
    }
}

static GPtrArray *fx_candidates (char **lines, guint nl, guint base, guint idx)
{
    GPtrArray *out = g_ptr_array_new_with_free_func (fx_cand_free);
    FxAn an;
    fx_an_init (&an);
    fx_an_scan (&an, lines, nl, idx, base, FALSE);
    const char *line = lines[idx];
    gboolean blk = an.in_comment, b = blk;
    char *code = fx_code_view (line, &b);
    const char *d = fx_sp (code);
    if (*d == '#') {
        const char *w = fx_sp (d + 1);
        if (!strncmp (w, "endif", 5) || !strncmp (w, "else", 4) || !strncmp (w, "elif", 4))
            fx_cand_add (out, line, g_strdup_printf ("// (stray) %s", line));
        if ((!strncmp (w, "if", 2) && !fx_is_ident (w[2])) || !strncmp (w, "elif", 4))
            fx_cand_add (out, line, g_strdup_printf ("%s 0 // was: %s", w[0] == 'i' ? "#if" : "#elif",
                                                     fx_trim (line)));
    } else if (*d && strlen (code) <= FX_MAX_REPAIR_LINE) {
        /* "const vec4 base = texSample2D (...);" inside a function: HLSL
         * and GLSL from 4.20 on take a run-time value there, "#version
         * 330" asks for a constant expression and a driver may hold to
         * it (Mesa does not, the Khronos reference compiler does).
         * Without the "const" the line means the same. */
        if (an.depth > 0 && !strncmp (d, "const", 5) && !fx_is_ident (d[5]) && strchr (d, '='))
            fx_cand_add (out, line, g_strdup_printf ("%.*s%s", (int) (d - code), line,
                                                     line + (fx_sp (d + 5) - code)));
        fx_an_decls (&an, code);
        if (fx_essl ())
            fx_essl_cands (&an, lines, nl, base, idx, blk, out);
        char *before = fx_join_upto (lines, idx);
        fx_cands_line (&an, line, line, before, blk, out, 0);
        g_free (before);
    }
    g_free (code);
    fx_an_clear (&an);
    for (guint i = base; i < nl; i++)
        if (fx_has_word (lines[i], "uint") || strstr (lines[i], "uvec")) {
            FxCand *c = g_new0 (FxCand, 1);
            c->uint_body = TRUE;
            g_ptr_array_add (out, c);
            break;
        }
    return out;
}

/* one round of stage 2: for the lines the log names, try the candidates
 * and keep the first after which the driver no longer names that line
 * (or accepts the shader). TRUE when *src changed. budget = compiles left. */
static gboolean fx_repair_by_lines (char **src, const char *log, FxTryFn try, gpointer user,
                                    int *budget)
{
    if (!fx_repairable (*src))
        return FALSE;
    GArray *errs = g_array_new (FALSE, FALSE, sizeof (int));
    fx_log_error_lines (log, errs);
    char **lines = g_strsplit (*src, "\n", -1);
    guint nl = g_strv_length (lines), base = fx_body_base (lines, nl);
    gboolean done = FALSE;
    for (guint k = 0; k < errs->len && k < 4 && !done && base > 0; k++) {
        int ln = g_array_index (errs, int, k);
        guint idx = base + (guint) ln - 1;
        if (idx >= nl)
            continue;
        GPtrArray *cands = fx_candidates (lines, nl, base, idx);
        for (guint c = 0; c < cands->len && !done && *budget > 0; c++) {
            FxCand *cd = cands->pdata[c];
            char **nlines = g_strdupv (lines);
            if (cd->uint_body) {
                fx_uint_to_int (nlines, base, nl);
                fx_strip_uint_suffix (nlines, base, nl);
            } else {
                guint at = cd->at1 ? cd->at1 - 1 : idx;
                g_free (nlines[at]);
                nlines[at] = g_strdup (cd->text);
            }
            char *nsrc = g_strjoinv ("\n", nlines);
            g_strfreev (nlines);
            char nlog[4096] = "";
            (*budget)--;
            gboolean ok = try (nsrc, nlog, sizeof (nlog), user);
            if (!ok) {
                GArray *ne = g_array_new (FALSE, FALSE, sizeof (int));
                fx_log_error_lines (nlog, ne);
                ok = ne->len > 0;
                for (guint i = 0; i < ne->len; i++)
                    ok &= g_array_index (ne, int, i) != ln;
                g_array_unref (ne);
            }
            if (ok) {
                g_debug ("fx: error-driven repair line %d: %s  ->  %s", ln, lines[idx],
                         cd->uint_body ? "(uint -> int in the body)" : cd->text);
                g_free (*src);
                *src = nsrc;
                done = TRUE;
            } else {
                g_free (nsrc);
            }
        }
        g_ptr_array_unref (cands);
    }
    g_strfreev (lines);
    g_array_unref (errs);
    return done;
}
