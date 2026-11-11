/* fx_essl.h - GLSL ES 3.00 / 3.10 / 3.20 output of the effect shader
 * builder (included by scenenode_effects.c right behind fx_srcrepair.h,
 * whose source model it uses; everything here is static).
 *
 * The builder writes GLSL 3.30 for desktop OpenGL. On an OpenGL ES context
 * the same sources are written as GLSL ES instead:
 *
 *   - the version line and default precisions (highp for float, int and
 *     the sampler types, in both stages: the picture is computed with the
 *     precision desktop OpenGL uses, and a uniform declared in both stages
 *     has one precision);
 *   - the same prelude (the helper names of Wallpaper Engine's dialect are
 *     macros and small functions that GLSL ES 3.00 has as well), plus the
 *     old texture function names GLSL ES no longer knows;
 *   - the conversions GLSL 3.30 makes by itself and GLSL ES does not:
 *     "float x = 1;", "v * 2", "max (x, 0)", "x > 0", "return 0;" in a
 *     float function, "c ? 1 : x". GLSL ES has no implicit conversion at
 *     all, and Wallpaper Engine's shaders (written against HLSL) are full
 *     of them.
 *
 * How the conversions are found. fx_srcrepair.h already knows, line by
 * line, which lines are compiled, what every name in scope is and what
 * type an expression has. fxe_run () below is a parser for statements and
 * expressions on top of that: it types every sub-expression and, where an
 * integer meets a float (operands of + - * / and of comparisons, the two
 * arms of ?:, the right side of an assignment or initialiser, a return
 * value, arguments of built-in functions and of functions the shader
 * defines), makes the integer side a float:
 *     a decimal literal            2         ->  2.0
 *     through ( ), unary -, ?:     (-(2))    ->  (-(2.0))
 *     anything else                i, a / b  ->  float(i), float(a / b)
 * (an int vector gets vecN(...)). An integer division stays an integer
 * division: "float h = 1 / 2;" is float(1 / 2) = 0.0, as in GLSL 3.30 and
 * in HLSL. Nothing is rewritten unless both types are known; an
 * expression the model cannot type (a function-like macro of the shader,
 * a struct member, an overloaded function, a header over several lines)
 * is left exactly as written.
 *
 * What a source model cannot see is left to the compiler as the judge
 * (fx_essl_cands, the GLSL ES candidates of fx_srcrepair.h's stage 2):
 * for a line the driver's log names, the typed rewrite of that line, then
 * the purely lexical one - every decimal integer literal that is not an
 * index, not inside an integer constructor / texelFetch / textureSize,
 * not an operand of % << >> & | ^ and not in a "for (...)" header or an
 * integer declaration becomes a float literal - and the same for the body
 * of a macro the line uses. A candidate is kept only if the driver then
 * accepts the line.
 *
 * Also done here because GLSL ES is stricter than GLSL 3.30:
 *   - identifiers that are reserved words in GLSL ES ("sample", "input",
 *     "smooth", ... as variable names) are renamed;
 *   - "uniform float x = 1.0;": GLSL ES has no uniform initialisers. With
 *     an annotation the initialiser goes (the annotation's default is
 *     uploaded as always), without one the declaration becomes a constant;
 *   - a global initialised from something that is no constant expression
 *     ("vec2 texel = 1.0 / g_Texture0Resolution.xy;") is declared without
 *     the initialiser and assigned at the start of main ();
 *   - a local "const" whose initialiser is no constant expression loses
 *     the "const";
 *   - "precision ..." statements of the shader are blanked (the prelude
 *     has defined the qualifier words away, as on desktop);
 *   - "gl_FragData[0]" is the builder's fragment output, like
 *     gl_FragColor (other indices: there is one colour attachment);
 *   - a name that is no macro in an "#if" / "#elif" expression is written
 *     as 0, which is what GLSL 3.30 drivers take it for (GLSL ES: an
 *     error).
 *
 * Not handled, and reported like any other shader that does not compile
 * (the effect is left out, the layer is drawn without it): a function the
 * shader defines under the name of a GLSL ES built-in; sampler1D /
 * sampler2DRect; a conversion hidden in a function-like macro whose use
 * the compiler does not report on the line of the macro's caller.
 *
 * On desktop OpenGL none of this runs: fx_essl () is 0, fx_build_source ()
 * writes the bytes it always wrote and the hooks return at once. */

#include "glfeat.h"

/* ---- which language the built sources are in */

static gint fx_essl_forced = -1;

/* 0 = GLSL 3.30; 300 / 310 / 320 = GLSL ES of that version */
static int fx_essl (void)
{
    int f = g_atomic_int_get (&fx_essl_forced);
    return f >= 0 ? f : walpe_gl_feat_essl ();
}

/* tests and tools: 0, 300, 310 or 320 = that language whatever the
 * context is; anything else = what the context is */
void fx_set_glsl_es (int version)
{
    g_atomic_int_set (&fx_essl_forced, version == 0 || version == 300 || version == 310 ||
                                       version == 320 ? version : -1);
}

static void fx_essl_head (GString *s, int ver)
{
    g_string_append_printf (s, "#version %d es\n", ver);
    g_string_append (s, "precision highp float;\nprecision highp int;\n"
                        "precision highp sampler2D;\nprecision highp sampler3D;\n"
                        "precision highp samplerCube;\nprecision highp sampler2DShadow;\n"
                        "precision highp sampler2DArray;\n");
}

/* behind the common prelude: texture functions of GLSL 1.x that GLSL ES
 * 3.00 dropped (GLSL 3.30 drivers still take most of them) */
static void fx_essl_prelude (GString *s)
{
    g_string_append (s, "#define texture2DLod textureLod\n"
                        "#define texture3D texture\n"
                        "#define textureCube texture\n"
                        "#define texture2DProj textureProj\n");
}

/* ---- typed expressions: where an integer meets a float */

enum { FXK_INT = 1, FXK_OTHER, FXK_PAREN, FXK_NEG, FXK_TERN };

/* a sub-expression: [s, e) of the statement text, its type, and for
 * ( ), unary - and ?: the operand(s) a conversion is handed down to */
typedef struct { guint8 kind; FxTy t; int s, e, a, b; } FxeNode;

/* text to put in at pos. close: it ends something that ends there (goes
 * in front of whatever starts at the same place); span: length of the
 * expression it belongs to (inner ones close first, outer ones open first) */
typedef struct { int pos; guint8 close; int span; guint seq; const char *text; } FxeEdit;

typedef struct {
    FxAn *an;
    const char *t;      /* comment-free statement text */
    int p, n;           /* cursor, length */
    GArray *nodes;      /* FxeNode */
    GArray *edits;      /* FxeEdit */
    gboolean fail;      /* something this model does not parse */
    gboolean incomplete;/* the text ended inside a statement */
    int nest;
} Fxe;

#define FXE_N(x, i) (&g_array_index ((x)->nodes, FxeNode, (i)))
#define FXE_MAX_ARGS 16
/* nesting of an expression this parser follows. One level is five to six
 * C frames here (unary, primary with its argument table, assign, ternary,
 * bin): FX_EX_MAX_NEST (256) levels of them are some 150 KB of stack, more
 * than a thread has by default on a C library with small thread stacks
 * (musl: 128 KB), and the text is wallpaper data. Shaders nest a dozen
 * levels; deeper text is left as written, like anything else the model
 * does not parse. */
#define FXE_MAX_NEST 64

static int fxe_new (Fxe *x, guint8 kind, FxTy t, int s, int e, int a, int b)
{
    FxeNode nd = { kind, t, s, e, a, b };
    g_array_append_val (x->nodes, nd);
    return (int) x->nodes->len - 1;
}

/* not parsed from here on; the node returned types as "unknown" */
static int fxe_bad (Fxe *x)
{
    x->fail = TRUE;
    if (x->p >= x->n)
        x->incomplete = TRUE;
    return fxe_new (x, FXK_OTHER, fx_ty_unk, x->p, x->p, -1, -1);
}

static void fxe_ws (Fxe *x)
{
    while (x->p < x->n && g_ascii_isspace (x->t[x->p]))
        x->p++;
}

static char fxe_at (const Fxe *x, int off)
{
    return x->p + off < x->n ? x->t[x->p + off] : 0;
}

static gboolean fxe_inty (FxTy t)
{
    return !t.arr && (t.b == FXB_INT || t.b == FXB_UINT) && t.n >= 1 && t.n <= 4;
}

static gboolean fxe_floaty (FxTy t)
{
    return !t.arr && (t.b == FXB_FLOAT || t.b == FXB_MAT);
}

static void fxe_edit (Fxe *x, int pos, gboolean close, int span, const char *text)
{
    FxeEdit e = { pos, (guint8) close, span, x->edits->len, text };
    g_array_append_val (x->edits, e);
}

/* the integer expression ni becomes a float expression of the same value */
static void fxe_to_float (Fxe *x, int ni)
{
    FxeNode *n = FXE_N (x, ni);
    if (!fxe_inty (n->t))
        return;
    switch (n->kind) {
    case FXK_INT:
        fxe_edit (x, n->e, TRUE, 0, ".0");
        break;
    case FXK_PAREN:
    case FXK_NEG:
        fxe_to_float (x, n->a);
        break;
    case FXK_TERN:
        fxe_to_float (x, n->a);
        fxe_to_float (x, n->b);
        break;
    default: {
        /* not into the operands: "1 / 2" is an integer division */
        static const char *const ctor[4] = { "float(", "vec2(", "vec3(", "vec4(" };
        fxe_edit (x, n->s, FALSE, n->e - n->s, ctor[n->t.n - 1]);
        fxe_edit (x, n->e, TRUE, n->e - n->s, ")");
    }
    }
    FXE_N (x, ni)->t.b = FXB_FLOAT;
}

/* two operands that must be of one base type */
static void fxe_balance (Fxe *x, int l, int r)
{
    FxTy a = FXE_N (x, l)->t, b = FXE_N (x, r)->t;
    if (fxe_floaty (a) && fxe_inty (b))
        fxe_to_float (x, r);
    else if (fxe_inty (a) && fxe_floaty (b))
        fxe_to_float (x, l);
}

static int fxe_assign (Fxe *x);

/* over the (...) or [...] that starts at the cursor; FALSE: not closed */
static gboolean fxe_skip_group (Fxe *x)
{
    char o = x->t[x->p], c = o == '(' ? ')' : ']';
    int d = 0;
    for (; x->p < x->n; x->p++) {
        if (x->t[x->p] == o)
            d++;
        else if (x->t[x->p] == c && --d == 0) {
            x->p++;
            return TRUE;
        }
    }
    return FALSE;
}

/* The body of the object-like macro nm is one operand wherever the name
 * is written: a name or a literal, a call, a parenthesised expression,
 * with selectors and subscripts behind it and signs in front. Only then
 * is "the type of the macro" the type of something that stands in the
 * expression: "#define RATIO 16 / 9" in "x * RATIO" is (x * 16) / 9.
 * Converted as one integer operand - x * float(RATIO) - it would be
 * x * float(16 / 9) = x, and GLSL ES compiles that. A name that is no
 * macro: TRUE. */
static gboolean fxe_macro_is_operand (FxAn *an, const char *nm, gsize len, int depth)
{
    char *name = g_strndup (nm, len);
    const char *b = g_hash_table_lookup (an->macros, name);
    g_free (name);
    if (!b)
        return TRUE;
    if (!*b || *b == '\1' || depth > 8)
        return FALSE;
    b = fx_sp (b);
    while (*b == '-' || *b == '+' || *b == '!' || *b == '~')
        b = fx_sp (b + 1);
    if (*b == '(') {
        const char *e = fx_paren_end (b);
        if (!e)
            return FALSE;
        b = e + 1;
    } else if (g_ascii_isdigit (*b) || (*b == '.' && g_ascii_isdigit (b[1]))) {
        const char *q = b;
        while (g_ascii_isalnum (*q) || *q == '.' ||
               ((*q == '+' || *q == '-') && q > b && (q[-1] == 'e' || q[-1] == 'E') &&
                !(b[0] == '0' && (b[1] == 'x' || b[1] == 'X'))))
            q++;
        b = q;
    } else if (fx_is_ident_start (*b)) {
        const char *q = b;
        while (fx_is_ident (*q))
            q++;
        /* "#define N M": M's body stands there */
        if (*fx_sp (q) != '(' && !fxe_macro_is_operand (an, b, (gsize) (q - b), depth + 1))
            return FALSE;
        b = q;
    } else {
        return FALSE;
    }
    for (;;) {
        b = fx_sp (b);
        if (*b == '(') {
            const char *e = fx_paren_end (b);
            if (!e)
                return FALSE;
            b = e + 1;
        } else if (*b == '[') {
            int d = 0;
            for (; *b; b++) {
                if (*b == '[')
                    d++;
                else if (*b == ']' && --d == 0)
                    break;
            }
            if (!*b)
                return FALSE;
            b++;
        } else if (*b == '.' && fx_is_ident_start (b[1])) {
            for (b++; fx_is_ident (*b); b++)
                ;
        } else {
            break;
        }
    }
    return *b == 0;
}

/* arguments of a built-in: which of them GLSL ES wants as floats */
static void fxe_builtin_args (Fxe *x, int bi, const char *nm, gsize len, const int *arg, int na)
{
#define FXE_IS(s) (len == strlen (s) && !strncmp (nm, s, len))
    guint8 rule = fx_builtins[bi].rule;
    if (rule == FXR_BOOL)
        return;
    if (rule == FXR_VEC4) {
        /* texture lookups: coordinates, lod, bias, gradients are floats;
         * texelFetch takes integers */
        if (FXE_IS ("texelFetch"))
            return;
        for (int i = 1; i < na; i++)
            fxe_to_float (x, arg[i]);
        return;
    }
    if (rule == FXR_MUL) {
        if (na == 2)
            fxe_balance (x, arg[0], arg[1]);
        return;
    }
    /* abs, sign, min, max, clamp exist for integers too: all-integer
     * arguments stay as they are */
    if (FXE_IS ("abs") || FXE_IS ("sign") || FXE_IS ("min") || FXE_IS ("max") || FXE_IS ("clamp")) {
        gboolean anyf = FALSE;
        for (int i = 0; i < na; i++)
            anyf |= fxe_floaty (FXE_N (x, arg[i])->t);
        if (!anyf)
            return;
    }
    for (int i = 0; i < na; i++)
        fxe_to_float (x, arg[i]);
#undef FXE_IS
}

/* type of the call nm (arg...), after its arguments were balanced */
static FxTy fxe_call (Fxe *x, const char *nm, gsize len, gboolean array_ctor, const int *arg, int na)
{
    FxAn *an = x->an;
    FxTy ct;
    if (fx_ty_parse (nm, len, &ct)) {
        if (ct.b == FXB_VOID || ct.b == FXB_OTHER)
            return fx_ty_unk;
        if (array_ctor) {
            /* "float[3] (1, 2, 3)": elements of an array are not converted
             * by the constructor (a scalar or vector constructor converts) */
            if (ct.b == FXB_FLOAT)
                for (int i = 0; i < na; i++)
                    if (FXE_N (x, arg[i])->t.n == ct.n)
                        fxe_to_float (x, arg[i]);
            ct.arr = 1;
        }
        return ct;
    }
    char *name = g_strndup (nm, len);
    gboolean is_macro = g_hash_table_contains (an->macros, name);
    const FxSig *sig = g_hash_table_lookup (an->sigs, name);
    g_free (name);
    int bi = fx_builtin_find (nm, len);
    if (bi >= 0) {
        fxe_builtin_args (x, bi, nm, len, arg, na);
    } else if (!is_macro && sig && sig->n == na) {
        /* a function of the shader with one known parameter list */
        for (int i = 0; i < na; i++)
            if (!sig->out[i] && sig->t[i].b == FXB_FLOAT && !sig->t[i].arr)
                fxe_to_float (x, arg[i]);
    }
    if (na > 8)
        return fx_ty_unk;
    FxTy at[8];
    for (int i = 0; i < na; i++)
        at[i] = FXE_N (x, arg[i])->t;
    return fx_call_type (an, nm, len, at, na);
}

static int fxe_primary (Fxe *x)
{
    fxe_ws (x);
    if (x->p >= x->n)
        return fxe_bad (x);
    const char *t = x->t;
    const int s = x->p;
    const char c = t[s];
    int node;
    if (c == '(') {
        x->p++;
        int in = fxe_assign (x);
        fxe_ws (x);
        if (x->fail || fxe_at (x, 0) != ')')
            return fxe_bad (x);
        x->p++;
        node = fxe_new (x, FXK_PAREN, FXE_N (x, in)->t, s, x->p, in, -1);
    } else if (g_ascii_isdigit (c) || (c == '.' && g_ascii_isdigit (fxe_at (x, 1)))) {
        gboolean hex = c == '0' && (fxe_at (x, 1) == 'x' || fxe_at (x, 1) == 'X');
        gboolean flt = FALSE, digits = TRUE;
        int q = hex ? s + 2 : s;
        while (q < x->n && (g_ascii_isalnum (t[q]) || t[q] == '.' ||
                            (!hex && (t[q] == '+' || t[q] == '-') &&
                             (t[q - 1] == 'e' || t[q - 1] == 'E')))) {
            if (!hex && (t[q] == '.' || t[q] == 'e' || t[q] == 'E' || t[q] == 'f' || t[q] == 'F'))
                flt = TRUE;
            if (!g_ascii_isdigit (t[q]))
                digits = FALSE;
            q++;
        }
        gboolean uns = !flt && (t[q - 1] == 'u' || t[q - 1] == 'U');
        if (flt && !hex) {
            /* "1f" (HLSL's suffix on an integer): GLSL ES reads "1.0f" */
            int k = s;
            while (k < q && g_ascii_isdigit (t[k]))
                k++;
            if (k > s && k == q - 1 && (t[k] == 'f' || t[k] == 'F'))
                fxe_edit (x, k, TRUE, 0, ".0");
        }
        x->p = q;
        /* (an octal literal is no FXK_INT: "010" is 8, "010.0" would be
         * 10.0 - it is converted like any other integer expression) */
        const gboolean octal = c == '0' && q - s > 1;
        node = fxe_new (x, !flt && !hex && digits && !octal ? FXK_INT : FXK_OTHER,
                        fx_ty (flt ? FXB_FLOAT : uns ? FXB_UINT : FXB_INT, 1), s, q, -1, -1);
    } else if (fx_is_ident_start (c)) {
        int q = s;
        while (q < x->n && fx_is_ident (t[q]))
            q++;
        const gsize len = (gsize) (q - s);
        FxTy ty, ct;
        gboolean array_ctor = FALSE;
        x->p = q;
        fxe_ws (x);
        if (fxe_at (x, 0) == '[' && fx_ty_parse (t + s, len, &ct)) {
            /* "vec2[3] (...)" */
            if (!fxe_skip_group (x))
                return fxe_bad (x);
            fxe_ws (x);
            if (fxe_at (x, 0) != '(')
                return fxe_bad (x);
            array_ctor = TRUE;
        }
        if (fxe_at (x, 0) == '(') {
            int arg[FXE_MAX_ARGS], na = 0;
            x->p++;
            fxe_ws (x);
            if (fxe_at (x, 0) == ')') {
                x->p++;
            } else {
                for (;;) {
                    int a = fxe_assign (x);
                    if (x->fail)
                        return a;
                    if (na == FXE_MAX_ARGS)
                        return fxe_bad (x);
                    arg[na++] = a;
                    fxe_ws (x);
                    if (fxe_at (x, 0) == ',') {
                        x->p++;
                        continue;
                    }
                    if (fxe_at (x, 0) == ')') {
                        x->p++;
                        break;
                    }
                    return fxe_bad (x);
                }
            }
            ty = fxe_call (x, t + s, len, array_ctor, arg, na);
        } else {
            ty = fx_ident_type (x->an, t + s, len);
            /* (a macro that is several operands has no type to convert
             * or to convert its neighbours by) */
            if (ty.b != FXB_UNK && !fxe_macro_is_operand (x->an, t + s, len, 0))
                ty = fx_ty_unk;
            /* a plain name ends at its last character, not behind the
             * blanks that were skipped to look for a '(' or '[': an edit
             * at the node's end ("float(" name ")") stays on the name's
             * line */
            x->p = q;
        }
        node = fxe_new (x, FXK_OTHER, ty, s, x->p, -1, -1);
    } else {
        return fxe_bad (x);
    }
    for (;;) {
        fxe_ws (x);
        const char d = fxe_at (x, 0);
        const FxTy bt = FXE_N (x, node)->t;
        FxTy nt = fx_ty_unk;
        if (d == '.' && !g_ascii_isdigit (fxe_at (x, 1))) {
            const int f = x->p + 1;
            int q = f;
            while (q < x->n && fx_is_ident (t[q]))
                q++;
            if (q == f)
                return fxe_bad (x);
            const int fl = q - f;
            gboolean sw = fl >= 1 && fl <= 4;
            for (int i = 0; sw && i < fl; i++)
                sw = strchr ("xyzwrgbastpq", t[f + i]) != NULL;
            x->p = q;
            fxe_ws (x);
            if (fxe_at (x, 0) == '(') {
                /* a method: GLSL has ".length ()" only */
                if (!fxe_skip_group (x))
                    return fxe_bad (x);
                if (fl == 6 && !strncmp (t + f, "length", 6))
                    nt = fx_ty (FXB_INT, 1);
            } else {
                x->p = q;   /* (a field, like a plain name, ends at its last character) */
                if (sw && !bt.arr && bt.n >= 2 && bt.n <= 4 &&
                    (bt.b == FXB_FLOAT || bt.b == FXB_INT || bt.b == FXB_UINT || bt.b == FXB_BOOL))
                    nt = fx_ty (bt.b, (guint8) fl);
            }
        } else if (d == '[') {
            x->p++;
            (void) fxe_assign (x);
            fxe_ws (x);
            if (x->fail || fxe_at (x, 0) != ']')
                return fxe_bad (x);
            x->p++;
            if (bt.arr) {
                nt = bt;
                nt.arr = 0;
            } else if (bt.b == FXB_MAT) {
                nt = fx_ty (FXB_FLOAT, bt.n);
            } else if (bt.n >= 2 && bt.b != FXB_UNK) {
                nt = fx_ty (bt.b, 1);
            }
        } else if ((d == '+' || d == '-') && fxe_at (x, 1) == d) {
            x->p += 2;
            nt = bt;
        } else {
            break;
        }
        node = fxe_new (x, FXK_OTHER, nt, s, x->p, -1, -1);
    }
    return node;
}

/* every level of an expression comes through here (parentheses, call
 * arguments, subscripts, unary chains) or through the nest count of ?:
 * and of assignments: the recursion is bounded (FXE_MAX_NEST) */
static int fxe_unary (Fxe *x)
{
    if (x->nest >= FXE_MAX_NEST)
        return fxe_bad (x);
    x->nest++;
    fxe_ws (x);
    const int s = x->p;
    const char c = fxe_at (x, 0);
    int node;
    if (c == '-' || c == '+') {
        gboolean twice = fxe_at (x, 1) == c;    /* ++x, --x */
        x->p += twice ? 2 : 1;
        int in = fxe_unary (x);
        node = fxe_new (x, twice ? FXK_OTHER : FXK_NEG, FXE_N (x, in)->t, s, FXE_N (x, in)->e, in, -1);
    } else if (c == '!' || c == '~') {
        x->p++;
        int in = fxe_unary (x);
        node = fxe_new (x, FXK_OTHER, c == '!' ? fx_ty (FXB_BOOL, 1) : FXE_N (x, in)->t, s,
                        FXE_N (x, in)->e, -1, -1);
    } else {
        node = fxe_primary (x);
    }
    x->nest--;
    return node;
}

static int fxe_bin (Fxe *x, int minprec)
{
    int l = fxe_unary (x);
    while (!x->fail) {
        fxe_ws (x);
        if (x->p >= x->n)
            break;
        const char c = fxe_at (x, 0), d = fxe_at (x, 1);
        int prec = 0, len = 1;
        if ((c == '|' && d == '|') || (c == '^' && d == '^')) prec = 1, len = 2;
        else if (c == '&' && d == '&') prec = 2, len = 2;
        else if (c == '|' && d != '=') prec = 3;
        else if (c == '^' && d != '=') prec = 4;
        else if (c == '&' && d != '=') prec = 5;
        else if ((c == '=' || c == '!') && d == '=') prec = 6, len = 2;
        else if ((c == '<' || c == '>') && d == c) {
            if (fxe_at (x, 2) == '=')
                break;
            prec = 8, len = 2;
        } else if (c == '<' || c == '>') prec = 7, len = d == '=' ? 2 : 1;
        else if ((c == '+' || c == '-') && d != '=') prec = 9;
        else if ((c == '*' || c == '/' || c == '%') && d != '=') prec = 10;
        if (!prec || prec < minprec)
            break;
        x->p += len;
        int r = fxe_bin (x, prec + 1);
        if (x->fail)
            break;
        FxTy t;
        if (prec <= 2) {
            t = fx_ty (FXB_BOOL, 1);
        } else if (prec == 6 || prec == 7) {
            fxe_balance (x, l, r);
            t = fx_ty (FXB_BOOL, 1);
        } else if (prec == 8) {
            t = FXE_N (x, l)->t;
        } else {
            if (prec >= 9 && c != '%')
                fxe_balance (x, l, r);
            t = fx_combine (x->an, FXE_N (x, l)->t, FXE_N (x, r)->t, c);
        }
        l = fxe_new (x, FXK_OTHER, t, FXE_N (x, l)->s, FXE_N (x, r)->e, -1, -1);
    }
    return l;
}

static int fxe_ternary (Fxe *x)
{
    int c = fxe_bin (x, 1);
    fxe_ws (x);
    if (x->fail || fxe_at (x, 0) != '?')
        return c;
    if (x->nest >= FXE_MAX_NEST)
        return fxe_bad (x);
    x->p++;
    x->nest++;
    int a = fxe_assign (x);
    fxe_ws (x);
    if (x->fail || fxe_at (x, 0) != ':') {
        x->nest--;
        return fxe_bad (x);
    }
    x->p++;
    int b = fxe_assign (x);
    x->nest--;
    if (x->fail)
        return b;
    fxe_balance (x, a, b);
    FxTy ta = FXE_N (x, a)->t, tb = FXE_N (x, b)->t;
    return fxe_new (x, FXK_TERN, ta.b == tb.b && ta.n == tb.n && ta.arr == tb.arr ? ta : fx_ty_unk,
                    FXE_N (x, c)->s, FXE_N (x, b)->e, a, b);
}

static int fxe_assign (Fxe *x)
{
    int l = fxe_ternary (x);
    if (x->fail)
        return l;
    fxe_ws (x);
    const char c = fxe_at (x, 0), d = fxe_at (x, 1);
    int len = 0;
    gboolean arith = FALSE;
    if (c == '=' && d != '=') len = 1, arith = TRUE;
    else if ((c == '+' || c == '-' || c == '*' || c == '/') && d == '=') len = 2, arith = TRUE;
    else if ((c == '%' || c == '&' || c == '|' || c == '^') && d == '=') len = 2;
    else if ((c == '<' || c == '>') && d == c && fxe_at (x, 2) == '=') len = 3;
    if (!len)
        return l;
    if (x->nest >= FXE_MAX_NEST)
        return fxe_bad (x);
    x->p += len;
    x->nest++;
    int r = fxe_assign (x);
    x->nest--;
    if (x->fail)
        return r;
    if (arith && fxe_floaty (FXE_N (x, l)->t))
        fxe_to_float (x, r);
    return fxe_new (x, FXK_OTHER, FXE_N (x, l)->t, FXE_N (x, l)->s, FXE_N (x, r)->e, -1, -1);
}

static int fxe_exprlist (Fxe *x)
{
    int n = fxe_assign (x);
    for (;;) {
        fxe_ws (x);
        if (x->fail || fxe_at (x, 0) != ',')
            return n;
        x->p++;
        n = fxe_assign (x);
    }
}

/* ---- statements */

/* the word w at the cursor: taken */
static gboolean fxe_kw (Fxe *x, const char *w)
{
    const int n = (int) strlen (w);
    if (x->p + n > x->n || strncmp (x->t + x->p, w, (gsize) n) ||
        (x->p + n < x->n && fx_is_ident (x->t[x->p + n])))
        return FALSE;
    x->p += n;
    return TRUE;
}

static gboolean fxe_expect (Fxe *x, char c)
{
    fxe_ws (x);
    if (fxe_at (x, 0) == c) {
        x->p++;
        return TRUE;
    }
    (void) fxe_bad (x);
    return FALSE;
}

/* a declaration ("[qualifiers] T a = ..., b[2], c = ...") or an
 * expression list. TRUE: it was a function header or prototype, skipped
 * up to its ')' - nothing has to end it. */
static gboolean fxe_decl_or_expr (Fxe *x)
{
    static const char *const quals[] = { "const", "uniform", "varying", "attribute", "inout", "in",
                                         "out", "flat", "smooth", "centroid", "invariant", "highp",
                                         "mediump", "lowp", "static" };
    const char *t = x->t;
    const int p0 = x->p;
    for (;;) {
        gboolean hit = FALSE;
        fxe_ws (x);
        for (guint i = 0; i < G_N_ELEMENTS (quals) && !hit; i++)
            hit = fxe_kw (x, quals[i]);
        if (!hit && fxe_kw (x, "layout")) {
            fxe_ws (x);
            if (fxe_at (x, 0) != '(' || !fxe_skip_group (x)) {
                (void) fxe_bad (x);
                return FALSE;
            }
            hit = TRUE;
        }
        if (!hit)
            break;
    }
    const int s = x->p;
    if (fx_is_ident_start (fxe_at (x, 0))) {
        int q = s, r;
        while (q < x->n && fx_is_ident (t[q]))
            q++;
        for (r = q; r < x->n && g_ascii_isspace (t[r]); r++)
            ;
        if (r > q && r < x->n && fx_is_ident_start (t[r])) {
            FxTy T = fx_ty_unk;
            if (!fx_ty_parse (t + s, (gsize) (q - s), &T))
                T = fx_ty_unk;      /* a struct, a type behind a macro */
            x->p = r;
            for (;;) {
                while (x->p < x->n && fx_is_ident (t[x->p]))
                    x->p++;
                fxe_ws (x);
                if (fxe_at (x, 0) == '(') {
                    if (!fxe_skip_group (x))
                        x->fail = x->incomplete = TRUE;
                    return TRUE;
                }
                gboolean arr = FALSE;
                while (fxe_at (x, 0) == '[') {
                    if (!fxe_skip_group (x)) {
                        x->fail = x->incomplete = TRUE;
                        return FALSE;
                    }
                    arr = TRUE;
                    fxe_ws (x);
                }
                if (fxe_at (x, 0) == '=' && fxe_at (x, 1) != '=') {
                    x->p++;
                    int v = fxe_assign (x);
                    if (x->fail)
                        return FALSE;
                    if (!arr && T.b == FXB_FLOAT)
                        fxe_to_float (x, v);
                    fxe_ws (x);
                }
                if (fxe_at (x, 0) != ',')
                    return FALSE;
                x->p++;
                fxe_ws (x);
                if (!fx_is_ident_start (fxe_at (x, 0))) {
                    (void) fxe_bad (x);
                    return FALSE;
                }
            }
        }
    }
    x->p = p0;
    (void) fxe_exprlist (x);
    return FALSE;
}

/* every statement of the text. A statement this model does not parse
 * ends the walk: it and what follows stay as written. */
static void fxe_run (Fxe *x)
{
    while (!x->fail) {
        fxe_ws (x);
        if (x->p >= x->n)
            return;
        const char c = fxe_at (x, 0);
        if (c == '{' || c == '}' || c == ';') {
            x->p++;
            continue;
        }
        const guint mark = x->edits->len;
        gboolean need_end = FALSE;
        if (fxe_kw (x, "else") || fxe_kw (x, "do") || fxe_kw (x, "break") ||
            fxe_kw (x, "continue") || fxe_kw (x, "discard"))
            continue;
        if (fxe_kw (x, "struct") || fxe_kw (x, "precision"))
            return;
        if (fxe_kw (x, "case")) {
            (void) fxe_exprlist (x);
            if (!x->fail)
                (void) fxe_expect (x, ':');
        } else if (fxe_kw (x, "default")) {
            (void) fxe_expect (x, ':');
        } else if (fxe_kw (x, "if") || fxe_kw (x, "while") || fxe_kw (x, "switch")) {
            if (fxe_expect (x, '(')) {
                (void) fxe_exprlist (x);
                if (!x->fail)
                    (void) fxe_expect (x, ')');
            }
        } else if (fxe_kw (x, "for")) {
            if (fxe_expect (x, '(')) {
                fxe_ws (x);
                if (fxe_at (x, 0) != ';')
                    (void) fxe_decl_or_expr (x);
                if (!x->fail && fxe_expect (x, ';')) {
                    fxe_ws (x);
                    if (fxe_at (x, 0) != ';')
                        (void) fxe_exprlist (x);
                    if (!x->fail && fxe_expect (x, ';')) {
                        fxe_ws (x);
                        if (fxe_at (x, 0) != ')')
                            (void) fxe_exprlist (x);
                        if (!x->fail)
                            (void) fxe_expect (x, ')');
                    }
                }
            }
        } else if (fxe_kw (x, "return")) {
            fxe_ws (x);
            if (fxe_at (x, 0) != ';') {
                int r = fxe_exprlist (x);
                if (!x->fail && x->an->cur_ret.b == FXB_FLOAT && !x->an->cur_ret.arr)
                    fxe_to_float (x, r);
            }
            need_end = TRUE;
        } else {
            need_end = !fxe_decl_or_expr (x);
        }
        if (!x->fail && need_end) {
            fxe_ws (x);
            if (fxe_at (x, 0) == ';')
                x->p++;
            else if (x->p >= x->n)
                x->fail = x->incomplete = TRUE;
            else
                x->fail = TRUE;
        }
        if (x->fail)
            g_array_set_size (x->edits, mark);
    }
}

static gint fxe_edit_cmp (gconstpointer pa, gconstpointer pb)
{
    const FxeEdit *a = pa, *b = pb;
    if (a->pos != b->pos)
        return a->pos < b->pos ? -1 : 1;
    if (a->close != b->close)
        return a->close ? -1 : 1;
    if (a->span != b->span)
        return (a->span < b->span) == (a->close != 0) ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

/* text with the edits put in (positions are offsets into text) */
static char *fxe_apply (const char *text, GArray *edits)
{
    g_array_sort (edits, fxe_edit_cmp);
    GString *o = g_string_new (NULL);
    const int n = (int) strlen (text);
    int at = 0;
    for (guint i = 0; i < edits->len; i++) {
        const FxeEdit *e = &g_array_index (edits, FxeEdit, i);
        const int pos = CLAMP (e->pos, at, n);
        g_string_append_len (o, text + at, pos - at);
        g_string_append (o, e->text);
        at = pos;
    }
    g_string_append (o, text + at);
    return g_string_free (o, FALSE);
}

/* The statements of code (the comment-free view of orig, same offsets)
 * with their conversions made explicit: the new text of orig, or NULL
 * when nothing changes. *incomplete: the last statement goes on behind
 * the text (it is not touched). */
static char *fxe_rewrite (FxAn *an, const char *orig, const char *code, gboolean *incomplete)
{
    Fxe x = { an, code, 0, (int) strlen (code), NULL, NULL, FALSE, FALSE, 0 };
    x.nodes = g_array_new (FALSE, FALSE, sizeof (FxeNode));
    x.edits = g_array_new (FALSE, FALSE, sizeof (FxeEdit));
    an->macro_work = 0;
    fxe_run (&x);
    *incomplete = x.incomplete;
    char *out = x.edits->len ? fxe_apply (orig, x.edits) : NULL;
    g_array_unref (x.nodes);
    g_array_unref (x.edits);
    return out;
}

/* ---- lexical: integer literals that can only be meant as floats */

/* the operand that ends at e, when it is nothing but decimal integer
 * literals: "12", or a parenthesised group of such literals and + - * / %
 * ("(3 + 4)"). Returns its start, NULL for anything else. */
static const char *fxe_intlit_back (const char *code, const char *e)
{
    while (e > code && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    if (e > code && e[-1] == ')') {
        const char *b = e - 1;
        int d = 1;
        while (b > code && d > 0) {
            b--;
            if (*b == ')')
                d++;
            else if (*b == '(')
                d--;
            else if (!g_ascii_isdigit (*b) && !strchr (" \t+-*/%", *b))
                return NULL;
        }
        if (d != 0)
            return NULL;
        /* "f (1)" is a call, "x2 (1)" too */
        const char *k = b;
        while (k > code && (k[-1] == ' ' || k[-1] == '\t'))
            k--;
        return k > code && (fx_is_ident (k[-1]) || k[-1] == '.' || k[-1] == ')' || k[-1] == ']')
                   ? NULL : b;
    }
    const char *b = e;
    while (b > code && g_ascii_isdigit (b[-1]))
        b--;
    /* "1.5", "x2", "1e5", "0x10" end in digits as well */
    if (b == e || (b > code && (fx_is_ident (b[-1]) || b[-1] == '.')))
        return NULL;
    return b;
}

/* the same for the operand that starts at p (behind signs) */
static gboolean fxe_intlit_fwd (const char *p)
{
    p = fx_sp (p);
    while (*p == '-' || *p == '+')
        p = fx_sp (p + 1);
    if (*p == '(') {
        int d = 0;
        for (; *p; p++) {
            if (*p == '(')
                d++;
            else if (*p == ')') {
                if (--d == 0)
                    return TRUE;
            } else if (!g_ascii_isdigit (*p) && !strchr (" \t+-*/%", *p))
                return FALSE;
        }
        return FALSE;
    }
    const char *q = p;
    while (g_ascii_isdigit (*q))
        q++;
    return q > p && !fx_is_ident (*q) && *q != '.';
}

/* An integer division that is written with literals alone: "1 / 2",
 * "3 * 5 / 2", "(3 + 4) / 2", "x - 7 / 2". In GLSL 3.30 (and in HLSL) it
 * is an integer division - 0, 7, 3, x - 3 -; with every literal made a
 * float it would be 0.5, 7.5, 3.5, x - 3.5, and GLSL ES compiles that
 * without a word. Looked for: a '/' whose right operand is such a literal
 * and whose left side is a chain of such literals over * / % that starts
 * an expression ("x * 6 / 4" is (x * 6) / 4: the compiler judges that
 * one, an integer x times 6.0 does not compile). */
static gboolean fxe_int_division (const char *code)
{
    for (const char *s = code; (s = strchr (s, '/')) != NULL; s++) {
        if (s[1] == '=' || !fxe_intlit_fwd (s + 1))
            continue;
        const char *e = s;
        for (;;) {
            const char *b = fxe_intlit_back (code, e);
            if (!b)
                break;              /* the chain starts with something else */
            const char *k = b;
            while (k > code && (k[-1] == ' ' || k[-1] == '\t'))
                k--;
            if (k > code && (k[-1] == '-' || k[-1] == '+')) {
                /* "x * -3 / 4": a sign behind * / % belongs to the literal */
                const char *u = k - 1;
                while (u > code && (u[-1] == ' ' || u[-1] == '\t'))
                    u--;
                if (u > code && strchr ("*/%", u[-1]))
                    k = u;
            }
            if (k == code || !strchr ("*/%", k[-1]))
                return TRUE;        /* literals up to the start of the expression */
            e = k - 1;
        }
    }
    return FALSE;
}

/* Every decimal integer literal of the line becomes a float literal,
 * except where an integer is certainly meant: inside [ ], inside the
 * parentheses of an integer constructor / texelFetch / textureSize, in an
 * integer declaration up to its ';', in a "for (...)" header, next to
 * % << >> & | ^ ~. For a "#define" line: its body. NULL: nothing changes.
 * The result need not compile; the caller asks the compiler. What the
 * compiler cannot be asked is whether the value is still the same: a
 * line with an integer division between literals (fxe_int_division) gets
 * no promotion at all. */
static char *fxe_promote (const char *line, const char *code)
{
    GString *o = NULL;
    gsize at = 0;
    int sq = 0, depth = 0, over = 0;
    guint64 intctx = 0;
    gboolean pend_int = FALSE, int_decl = FALSE;
    const char *p = code, *d = fx_sp (code);
    /* (the "#define" line of a macro comes here whatever its length) */
    if (strlen (code) > FX_MAX_REPAIR_LINE || fxe_int_division (code))
        return NULL;
    if (*d == '#') {
        const char *w = fx_sp (d + 1);
        if (strncmp (w, "define", 6) || fx_is_ident (w[6]))
            return NULL;
        for (p = fx_sp (w + 6); fx_is_ident (*p); p++)
            ;
        if (*p == '(') {
            const char *ce = fx_paren_end (p);
            if (!ce)
                return NULL;
            p = ce + 1;
        }
    } else if (!strncmp (d, "case", 4) && !fx_is_ident (d[4])) {
        return NULL;
    }
    while (*p) {
        if (fx_is_ident_start (*p) && (p == code || !fx_is_ident (p[-1]))) {
            const char *q = p;
            while (fx_is_ident (*q))
                q++;
            const gsize len = (gsize) (q - p);
            const char *nx = fx_sp (q);
            FxTy ty;
            gboolean isint = fx_ty_parse (p, len, &ty) &&
                             (ty.b == FXB_INT || ty.b == FXB_UINT || ty.b == FXB_BOOL);
            if (len == 3 && !strncmp (p, "for", 3) && *nx == '(') {
                const char *ce = fx_paren_end (nx);
                if (!ce)
                    break;
                p = ce + 1;
                continue;
            }
            if (*nx == '(')
                pend_int = isint || (len == 10 && !strncmp (p, "texelFetch", 10)) ||
                           (len == 11 && !strncmp (p, "textureSize", 11));
            else if (isint && fx_is_ident_start (*nx))
                int_decl = TRUE;
            p = q;
            continue;
        }
        if (*p == '(') {
            if (depth < 63) {
                depth++;
                if (pend_int)
                    intctx |= (guint64) 1 << depth;
            } else {
                over++;     /* deeper than the mask: its ')' is not level 63's */
            }
            pend_int = FALSE;
        } else if (*p == ')') {
            if (over > 0) {
                over--;
            } else if (depth > 0) {
                intctx &= ~((guint64) 1 << depth);
                depth--;
            }
        } else if (*p == '[') {
            sq++;
        } else if (*p == ']') {
            sq = MAX (0, sq - 1);
        } else if (*p == ';') {
            int_decl = FALSE;
        } else if (g_ascii_isdigit (*p) || (*p == '.' && g_ascii_isdigit (p[1]))) {
            const char *q = p;
            gboolean plain = TRUE;
            while (g_ascii_isalnum (*q) || *q == '.' ||
                   ((*q == '+' || *q == '-') && q > p && (q[-1] == 'e' || q[-1] == 'E') &&
                    !(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')))) {
                plain &= g_ascii_isdigit (*q) != 0;
                q++;
            }
            if (p[0] == '0' && q - p > 1)
                plain = FALSE;      /* octal: "010" is 8, not 10.0 */
            const char *b = p, *a = fx_sp (q);
            while (b > code && (b[-1] == ' ' || b[-1] == '\t'))
                b--;
            const char pc = b > code ? b[-1] : 0, pc2 = b > code + 1 ? b[-2] : 0;
            /* an operand of % ^ ~ << >> or of a single & | */
            gboolean intop = pc == '%' || pc == '^' || pc == '~' ||
                             ((pc == '<' || pc == '>') && pc2 == pc) ||
                             ((pc == '&' || pc == '|') && pc2 != pc) ||
                             *a == '%' || *a == '^' ||
                             ((*a == '&' || *a == '|') && a[1] != *a) ||
                             ((*a == '<' || *a == '>') && a[1] == *a);
            if (plain && !sq && !intctx && !int_decl && !intop &&
                (p == code || !(fx_is_ident (p[-1]) || p[-1] == '.'))) {
                if (!o)
                    o = g_string_new (NULL);
                g_string_append_len (o, line + at, (gssize) ((gsize) (q - code) - at));
                g_string_append (o, ".0");
                at = (gsize) (q - code);
            }
            p = q;
            continue;
        }
        p++;
    }
    if (!o)
        return NULL;
    g_string_append (o, line + at);
    return g_string_free (o, FALSE);
}

/* ---- words GLSL ES reserves */

/* reserved in GLSL ES 3.00 - 3.20 (most of them in GLSL 3.30 as well,
 * where some drivers take them as names all the same); as a variable or
 * parameter name such a word gets a prefix */
static const char *const fxe_reserved[] = {
    "sample", "smooth", "flat", "input", "output", "filter", "common", "active", "partition",
    "half", "fixed", "packed", "resource", "patch", "buffer", "shared", "precise", "external",
    "interface", "long", "short", "double", "cast", "inline", "noinline", "public", "static",
    "extern", "this", "class", "union", "template", "centroid", "invariant", "superp", "namespace",
    "using", "volatile", "restrict", "readonly", "writeonly", "coherent", "subroutine", "sizeof",
    "typedef", "enum", "goto", "asm", "unsigned",
};

/* The name at p is a later declarator of a declaration: "vec4 acc =
 * vec4 (0.0), w;", "float a, w = 1.0;" - a comma in front of it, and the
 * statement it stands in begins with a type word and a name (qualifiers
 * before the type). The first declarator has the type in front of it and a
 * parameter has its own; "f (a, w)" is a call: between the start of the
 * statement and p no '(' or '[' may be open. Looked at: the FXE_DECL_BACK
 * bytes in front of p (a declaration is short, and the line is wallpaper
 * data of any length). */
#define FXE_DECL_BACK 512
static gboolean fxe_later_declarator (const char *code, const char *p)
{
    static const char *const quals[] = { "const", "highp", "mediump", "lowp", "uniform", "varying",
                                         "attribute", "in", "out", "flat", "centroid", "invariant",
                                         "static" };
    const char *e = p;
    while (e > code && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    if (e == code || e[-1] != ',')
        return FALSE;
    const char *s = e - 1, *stop = e - code > FXE_DECL_BACK ? e - FXE_DECL_BACK : code;
    int depth = 0;
    for (; s > stop; s--) {
        const char c = s[-1];
        if (c == ')' || c == ']') {
            depth++;
        } else if (c == '(' || c == '[') {
            if (--depth < 0)
                return FALSE;
        } else if (depth == 0 && (c == ';' || c == '{' || c == '}')) {
            break;
        }
    }
    if (depth != 0 || (s == stop && stop != code))
        return FALSE;
    s = fx_sp (s);
    for (;;) {
        gboolean hit = FALSE;
        for (guint i = 0; i < G_N_ELEMENTS (quals) && !hit; i++) {
            const gsize ql = strlen (quals[i]);
            if (!strncmp (s, quals[i], ql) && !fx_is_ident (s[ql])) {
                s = fx_sp (s + ql);
                hit = TRUE;
            }
        }
        if (!hit)
            break;
    }
    const char *te = s;
    while (fx_is_ident (*te))
        te++;
    FxTy ty;
    if (te == s || !fx_ty_parse (s, (gsize) (te - s), &ty) || ty.b == FXB_VOID)
        return FALSE;
    const char *nm = fx_sp (te);
    return nm > te && nm < p && fx_is_ident_start (*nm);
}

/* how code uses the word w: bit 0 = declared as a variable or parameter
 * ("float w", "vec4 w", "sampler2D w", "vec4 a, w"), bit 1 = that declaration is part of the shader's
 * interface (uniform / varying / attribute / in / out at the start of the
 * line): its name is looked up from outside and has to stay */
static guint fxe_word_use (const char *code, const char *w)
{
    const gsize n = strlen (w);
    guint use = 0;
    int later = 0;      /* occurrences behind a comma that were looked at */
    for (const char *p = code; (p = strstr (p, w)) != NULL; p += n) {
        if ((p > code && (fx_is_ident (p[-1]) || p[-1] == '.')) || fx_is_ident (p[n]))
            continue;
        const char *e = p;
        while (e > code && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        const char *b = e;
        while (b > code && fx_is_ident (b[-1]))
            b--;
        FxTy ty;
        /* (a sampler parameter is a declaration like any other:
         * "vec4 blur (sampler2D input, vec2 uv)") */
        if (e == p || b == e || !fx_ty_parse (b, (gsize) (e - b), &ty) || ty.b == FXB_VOID) {
            /* no type word in front of it: the second or a later name of
             * a declaration all the same? (At most 32 such looks a line:
             * "x, sample, sample, ..." of any length is no declaration.) */
            if (later >= 32 || !(e > code && e[-1] == ',') || (later++, !fxe_later_declarator (code, p)))
                continue;
        }
        /* declared here: what the line starts with is the same for every
         * further occurrence, so this one answers for the line. (The
         * length of the line was taken five times per occurrence before:
         * a megabyte of "float sample;float sample;..." on one line, which
         * this pass does not cap, was 10^11 byte reads.) */
        const char *d = fx_sp (code);
        const gsize dn = strlen (d);
        use |= 1;
        if (fx_line_is (d, dn, "uniform") || fx_line_is (d, dn, "varying") ||
            fx_line_is (d, dn, "attribute") || fx_line_is (d, dn, "in") || fx_line_is (d, dn, "out"))
            use |= 2;
        break;
    }
    return use;
}

/* line with every use of the name w prefixed - also behind a '.': the
 * word is renamed because the shader declares it, and when that is a
 * member of a struct, "s.w" is the same name (none of the reserved words
 * can be read as a swizzle) */
static char *fxe_prefix_word (const char *line, const char *code, const char *w)
{
    const gsize n = strlen (w);
    GString *o = NULL;
    gsize at = 0;
    for (const char *p = code; (p = strstr (p, w)) != NULL; p += n) {
        if ((p > code && fx_is_ident (p[-1])) || fx_is_ident (p[n]))
            continue;
        if (!o)
            o = g_string_new (NULL);
        g_string_append_len (o, line + at, (gssize) ((gsize) (p - code) - at));
        g_string_append (o, "_we_");
        at = (gsize) (p - code);
    }
    if (!o)
        return NULL;
    g_string_append (o, line + at);
    return g_string_free (o, FALSE);
}

/* "gl_FragData[0]" in s, [*a, *b) = its span; FALSE: none */
static gboolean fxe_fragdata0 (const char *s, gsize *a, gsize *b)
{
    for (const char *p = s; (p = strstr (p, "gl_FragData")) != NULL; p += 11) {
        if ((p > s && fx_is_ident (p[-1])) || fx_is_ident (p[11]))
            continue;
        const char *q = fx_sp (p + 11);
        if (*q != '[')
            continue;
        q = fx_sp (q + 1);
        if (*q != '0' || *fx_sp (q + 1) != ']')
            continue;
        *a = (gsize) (p - s);
        *b = (gsize) (fx_sp (q + 1) + 1 - s);
        return TRUE;
    }
    return FALSE;
}

/* the line ends in a backslash: the compiler reads the next line as part
 * of it (the rest of a "#define", or of a "//" comment) */
static gboolean fxe_ends_bs (const char *line)
{
    gsize n = strlen (line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t'))
        n--;
    return n && line[n - 1] == '\\';
}

/* reserved words used as names, the shader's own precision statements,
 * gl_FragData[0] (GLSL ES 3.00 has neither gl_FragColor nor gl_FragData;
 * the builder's fragment output stands in for both). Returns the number
 * of lines changed. */
static int fxe_lexical (char **lines, guint nl, guint base)
{
    int n = 0;
    gboolean blk = FALSE;
    char **code = g_new0 (char *, nl + 1);
    for (guint i = 0; i < nl; i++)
        code[i] = fx_code_view (lines[i], &blk);
    for (guint i = base; i < nl; i++) {
        /* the shader's own precision statements at the start of the line:
         * blanked in place. ("//" in front of the line, as it was done
         * first, changes what the lines around it are when the line also
         * opens a block comment - its text became code -, closes one - the
         * rest of the file became a comment - or ends in a backslash.) */
        {
            const char *d = fx_sp (code[i]);
            gsize dn = strlen (d);
            gboolean any = FALSE;
            while (fx_line_is (d, dn, "precision")) {
                const char *semi = memchr (d, ';', dn);
                if (!semi)
                    break;
                const gsize off = (gsize) (d - code[i]), len = (gsize) (semi + 1 - d);
                memset (lines[i] + off, ' ', len);
                memset (code[i] + off, ' ', len);
                const char *nx = fx_sp (semi + 1);
                dn -= (gsize) (nx - d);
                d = nx;
                any = TRUE;
            }
            if (any)
                n++;
        }
        gsize a, b;
        for (int k = 0; k < 8 && fxe_fragdata0 (code[i], &a, &b); k++) {
            char *l2 = g_strdup_printf ("%.*s_we_fragColor%s", (int) a, lines[i], lines[i] + b);
            char *c2 = g_strdup_printf ("%.*s_we_fragColor%s", (int) a, code[i], code[i] + b);
            g_free (lines[i]);
            g_free (code[i]);
            lines[i] = l2;
            code[i] = c2;
            if (!k)
                n++;
        }
    }
    for (guint w = 0; w < G_N_ELEMENTS (fxe_reserved); w++) {
        guint use = 0;
        for (guint i = base; i < nl; i++)
            if (strstr (code[i], fxe_reserved[w]))
                use |= fxe_word_use (code[i], fxe_reserved[w]);
        if (use != 1)
            continue;
        for (guint i = base; i < nl; i++) {
            char *r = strstr (code[i], fxe_reserved[w])
                          ? fxe_prefix_word (lines[i], code[i], fxe_reserved[w]) : NULL;
            if (!r)
                continue;
            gboolean b = FALSE;
            g_free (lines[i]);
            lines[i] = r;
            /* (comments of the line are blank in its view already: a view
             * of the new line that starts outside a comment differs only
             * where a block comment was open - recomputed from there) */
            char *c2 = fxe_prefix_word (code[i], code[i], fxe_reserved[w]);
            g_free (code[i]);
            code[i] = c2 ? c2 : fx_code_view (lines[i], &b);
            n++;
        }
    }
    g_strfreev (code);
    return n;
}

/* ---- constant expressions, as GLSL ES 3.00 defines them */

/* bytes of macro bodies fxe_const_expr may read for one source. A shader
 * is wallpaper data: "#define A B B B ..." over four levels is bodies ^ 4
 * scans, and one long body named by every initialiser of a large file is
 * megabytes each time - minutes on the render thread without a limit.
 * Real shaders name a few short macros in their initialisers. */
#define FXE_CONST_WORK (1024 * 1024)

/* [s, e) names nothing but literals, constructors, built-in functions
 * (texture lookups are none), macros that expand to such, and names in
 * consts. When in doubt: FALSE. *work: bytes of macro bodies read so far
 * in this source; past FXE_CONST_WORK a macro counts as "not constant". */
static gboolean fxe_const_expr (FxAn *an, GHashTable *consts, const char *s, const char *e, int depth,
                                gsize *work)
{
    for (const char *p = s; p < e;) {
        if (g_ascii_isdigit (*p) || (*p == '.' && p + 1 < e && g_ascii_isdigit (p[1]))) {
            while (p < e && (g_ascii_isalnum (*p) || *p == '.'))
                p++;
            continue;
        }
        if (!fx_is_ident_start (*p)) {
            p++;
            continue;
        }
        const char *q = p;
        while (q < e && fx_is_ident (*q))
            q++;
        const gsize len = (gsize) (q - p);
        const char *nx = q;
        while (nx < e && (*nx == ' ' || *nx == '\t'))
            nx++;
        const gboolean call = nx < e && *nx == '(';
        FxTy ty;
        gboolean ok;
        if (p > s && p[-1] == '.') {
            ok = TRUE;                              /* a swizzle */
        } else if (fx_ty_parse (p, len, &ty)) {
            ok = ty.b != FXB_OTHER;                 /* a constructor */
        } else if ((len == 4 && !strncmp (p, "true", 4)) || (len == 5 && !strncmp (p, "false", 5))) {
            ok = TRUE;
        } else {
            char *name = g_strndup (p, len);
            const char *body = g_hash_table_lookup (an->macros, name);
            int bi = fx_builtin_find (p, len);
            if (g_hash_table_contains (consts, name)) {
                ok = TRUE;
            } else if (call && bi >= 0) {
                ok = fx_builtins[bi].rule != FXR_VEC4;
            } else if (call && len == 5 && (!strncmp (p, "CAST", 4) || !strncmp (p, "TOGL", 4))) {
                ok = TRUE;
            } else if (body && *body && *body != '\1' && depth < 4 && *work < FXE_CONST_WORK) {
                const gsize bl = strlen (body);
                *work += bl;
                ok = bl <= FX_MAX_REPAIR_LINE &&
                     fxe_const_expr (an, consts, body, body + bl, depth + 1, work);
            } else {
                ok = FALSE;
            }
            g_free (name);
        }
        if (!ok)
            return FALSE;
        p = q;
    }
    return TRUE;
}

/* "[const] T name = init;" as the whole of code (one declarator).
 * Returns the type word's start; *name / *init / *semi are set. */
static const char *fxe_single_decl (const char *code, gboolean want_const, const char **tend,
                                    const char **name, const char **nend, const char **init,
                                    const char **semi)
{
    const char *d = fx_sp (code);
    if (want_const) {
        if (strncmp (d, "const", 5) || fx_is_ident (d[5]))
            return NULL;
        d = fx_sp (d + 5);
    }
    const char *te = d;
    while (fx_is_ident (*te))
        te++;
    FxTy T;
    if (te == d || !fx_ty_parse (d, (gsize) (te - d), &T) || T.b == FXB_VOID || T.b == FXB_OTHER)
        return NULL;
    const char *nm = fx_sp (te), *ne = nm;
    while (fx_is_ident (*ne))
        ne++;
    const char *eq = fx_sp (ne);
    if (ne == nm || nm == te || *eq != '=' || eq[1] == '=')
        return NULL;
    const char *in = fx_sp (eq + 1), *se = fx_expr_end (in);
    if (*se != ';' || se == in || *fx_sp (se + 1))
        return NULL;
    *tend = te;
    *name = nm;
    *nend = ne;
    *init = in;
    *semi = se;
    return d;
}

/* an #if around this line could not be evaluated: the model then takes
 * every branch of it as compiled, the compiler takes one */
static gboolean fxe_pp_unknown (const FxAn *an)
{
    for (guint i = 0; i < an->pp->len; i++)
        if (g_array_index (an->pp, FxPpFrame, i).unknown)
            return TRUE;
    return FALSE;
}

/* A name that is no macro in the expression of an "#if" / "#elif" (line
 * = the directive, code = its comment-free view, same offsets). GLSL 3.30
 * drivers read it as 0, like a C preprocessor and like Wallpaper Engine's
 * own one - a combo that is off is simply not defined, and shaders ask
 * "#if MASK || BLURALPHA == 0". GLSL ES makes it an error ("undefined
 * macro MASK in expression (illegal in GLES)"); the error-driven fallback
 * then knew one answer for a directive, "#if 0", which is the wrong
 * branch whenever the rest of the expression is true. Each such name is
 * written as the 0 it stands for; everything else of the expression is
 * left to the compiler. Kept: the operand of "defined", a name used like
 * a function, a macro of the source (an->macros: every #define the walk
 * has passed), the names the compiler itself defines (GL_..., __...).
 * Returns the new line, NULL when nothing changes. */
static char *fxe_pp_undefined (FxAn *an, const char *line, const char *code)
{
    const char *d = fx_sp (code);
    if (*d != '#' || strlen (code) > FX_MAX_REPAIR_LINE)
        return NULL;
    const char *w = fx_sp (d + 1), *we = w;
    while (fx_is_ident (*we))
        we++;
    if (!((we - w == 2 && !strncmp (w, "if", 2)) || (we - w == 4 && !strncmp (w, "elif", 4))))
        return NULL;
    GString *o = NULL;
    gsize at = 0;
    gboolean defined_op = FALSE;    /* the next name is the operand of "defined" */
    for (const char *p = we; *p;) {
        if (g_ascii_isdigit (*p)) {
            while (g_ascii_isalnum (*p))        /* "0x1F", "1u" */
                p++;
            continue;
        }
        if (!fx_is_ident_start (*p)) {
            if (*p != ' ' && *p != '\t' && *p != '(')
                defined_op = FALSE;
            p++;
            continue;
        }
        const char *q = p;
        while (fx_is_ident (*q))
            q++;
        const gsize len = (gsize) (q - p);
        gboolean keep;
        if (len == 7 && !strncmp (p, "defined", 7)) {
            defined_op = TRUE;
            p = q;
            continue;
        }
        if (defined_op || *fx_sp (q) == '(' || !strncmp (p, "GL_", 3) || !strncmp (p, "__", 2)) {
            keep = TRUE;
        } else {
            char *name = g_strndup (p, len);
            keep = g_hash_table_contains (an->macros, name);
            g_free (name);
        }
        defined_op = FALSE;
        if (!keep) {
            if (!o)
                o = g_string_new (NULL);
            g_string_append_len (o, line + at, (gssize) ((gsize) (p - code) - at));
            g_string_append_c (o, '0');
            at = (gsize) (q - code);
        }
        p = q;
    }
    if (!o)
        return NULL;
    g_string_append (o, line + at);
    return g_string_free (o, FALSE);
}

/* main (): the place for what the globals above were initialised with is
 * behind its '{', which may come on a later line (*hdr: the header was
 * seen, the brace not yet). file_scope: this line can hold the header.
 * A prototype - "void main ();" - is no header: the assignments would go
 * into the next '{' of the file, whatever that opens. TRUE: *line got the
 * assignments (code is its view as it was). */
static gboolean fxe_main_line (char **line, const char *code, gboolean file_scope, gboolean *hdr,
                               gboolean *seen, GString *pending)
{
    const char *from = *hdr ? code : NULL;
    const char *m = file_scope ? fx_find_word (code, "main") : NULL;
    if (m && *fx_sp (m + 4) == '(') {
        const char *pe = fx_paren_end (fx_sp (m + 4));
        if (!pe || *fx_sp (pe + 1) != ';') {
            *hdr = *seen = TRUE;
            from = m;
        }
    }
    const char *br = from ? strchr (from, '{') : NULL;
    if (!br)
        return FALSE;
    *hdr = FALSE;
    if (!pending->len)
        return FALSE;
    char *r = g_strdup_printf ("%.*s%s%s", (int) (br + 1 - code), *line, pending->str,
                               *line + (br + 1 - code));
    g_free (*line);
    *line = r;
    g_string_truncate (pending, 0);
    return TRUE;
}

/* ---- the pass over one built source */

#define FXE_JOIN_MAX 24     /* lines of one statement */
/* bytes of joined text the pass parses for one source. Every line that
 * ends inside a statement is parsed again with each of the next
 * FXE_JOIN_MAX lines added: 300 line-parses per line when no statement
 * ever ends ("a +" a million times took minutes - the shader is wallpaper
 * data and this runs on the render thread). Real shaders need some 10 KB;
 * past the budget lines are converted one by one, as a statement the
 * model cannot join always was. */
#define FXE_JOIN_WORK (2 * 1024 * 1024)

/* The typed conversions, line by line, with the symbol table and the
 * preprocessor state of fx_srcrepair.h walked alongside; uniform
 * initialisers, non-constant "const" locals and non-constant global
 * initialisers. Returns the number of lines changed. */
static int fxe_typed (char **lines, guint nl, guint base)
{
    FxAn an;
    fx_an_init (&an);
    GHashTable *consts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GString *pending = g_string_new (NULL);     /* assignments for the start of main () */
    gsize cwork = 0;                            /* fxe_const_expr's budget */
    gsize join_left = FXE_JOIN_WORK;            /* bytes the joins may still parse */
    guint nojoin_to = 0;                        /* lines in front of this one start no join */
    gboolean prev_bs = FALSE;                   /* the line before ended in a backslash */
    gboolean main_hdr = FALSE, main_seen = FALSE;
    int n = 0;
    for (guint i = 0; i < nl; i++) {
        /* A line behind a backslash-newline is the rest of a "#define" (or
         * of a "//" comment), whatever it looks like: its conversions are
         * made explicit like any other line's, but it is no declaration of
         * this place - no initialiser is moved out of it, nothing is
         * joined to it, no "const" or "uniform" of it is rewritten. (The
         * last line of "#define SETUP \ float a = v.x; \ float b = v.y;"
         * was taken for a global: "b = v.y;" went to main () and the macro
         * was left with "float b;".) */
        const gboolean cont = prev_bs;
        prev_bs = fxe_ends_bs (lines[i]);
        gboolean c0 = an.in_comment;
        char *code = fx_code_view (lines[i], &an.in_comment);
        const char *d = fx_sp (code);
        if (*d == '#') {
            /* (the model reads an undefined name as 0 as well: it walks
             * the branch the rewritten directive selects) */
            char *z = i >= base && !cont ? fxe_pp_undefined (&an, lines[i], code) : NULL;
            if (z) {
                g_free (lines[i]);
                lines[i] = z;
                n++;
            }
            g_free (fx_an_directive (&an, lines[i], d, FALSE));
            g_free (code);
            continue;
        }
        if (!*d || !fx_an_active (&an)) {
            g_free (code);
            continue;
        }
        fx_an_decls (&an, code);
        if (i < base || strlen (code) > FX_MAX_REPAIR_LINE) {
            /* (a line too long to be converted can still be where main ()
             * opens: the initialisers of the globals must not wait for
             * the next '{' of the file, which may open anything) */
            if (i >= base && fxe_main_line (&lines[i], code, an.depth == 0 && !cont, &main_hdr,
                                            &main_seen, pending))
                n++;
            fx_an_braces (&an, code);
            g_free (code);
            continue;
        }
        const gboolean global = an.depth == 0 && an.par0 == 0;
        gboolean changed = FALSE;

        /* "uniform T x = init;" */
        if (global && !cont && fx_line_is (d, strlen (d), "uniform")) {
            const char *semi = strchr (d, ';'), *eq = semi ? memchr (d, '=', (gsize) (semi - d)) : NULL;
            if (eq && eq[1] != '=') {
                const char *note = strstr (lines[i] + (semi - code), "//");
                char *r;
                if (note && strchr (note, '{')) {
                    const char *e1 = eq;
                    while (e1 > d && (e1[-1] == ' ' || e1[-1] == '\t'))
                        e1--;
                    r = g_strdup_printf ("%.*s%s", (int) (e1 - code), lines[i], lines[i] + (semi - code));
                } else {
                    r = g_strdup_printf ("%.*sconst  %s", (int) (d - code), lines[i],
                                         lines[i] + (d + 7 - code));
                }
                g_free (lines[i]);
                lines[i] = r;
                g_free (code);
                gboolean b = c0;
                code = fx_code_view (lines[i], &b);
                d = fx_sp (code);
                changed = TRUE;
            }
        }

        /* the statements of the line; one that goes on is tried together
         * with the lines it goes on in */
        gboolean inc = FALSE;
        char *r = fxe_rewrite (&an, lines[i], code, &inc);
        guint last = i;
        if (inc && !cont && i >= nojoin_to && join_left) {
            GString *j = g_string_new (code);
            gboolean blk = an.in_comment, inc2 = TRUE;
            char *rj = NULL;
            guint k = i + 1;
            for (; k < nl && k - i <= FXE_JOIN_MAX && j->len <= FX_MAX_REPAIR_LINE; k++) {
                char *ck = fx_code_view (lines[k], &blk);
                /* (a line that ends in a backslash is not put into the
                 * middle of another: the backslash would be a stray
                 * character there) */
                gboolean dir = *fx_sp (ck) == '#' || fxe_ends_bs (lines[k]);
                if (!dir) {
                    g_string_append_c (j, ' ');
                    g_string_append (j, ck);
                }
                g_free (ck);
                if (dir)
                    break;
                if (j->len > join_left) {
                    join_left = 0;
                    break;
                }
                join_left -= j->len;
                g_free (rj);
                rj = fxe_rewrite (&an, j->str, j->str, &inc2);
                if (!inc2)
                    break;
            }
            /* the statement did not end within these lines: the ones in
             * between are its middle, not the start of another try */
            if (inc2)
                nojoin_to = k;
            if (!inc2 && rj) {
                /* the statement, whole, on its first line; the lines it
                 * came from are scanned for what they declare and stay as
                 * empty lines (line numbers keep their meaning) */
                for (guint m = i + 1; m <= k; m++) {
                    char *cm = fx_code_view (lines[m], &an.in_comment);
                    fx_an_decls (&an, cm);
                    g_free (cm);
                    g_free (lines[m]);
                    lines[m] = g_strdup ("");
                }
                g_free (r);
                r = rj;
                rj = NULL;
                g_free (code);
                code = g_strdup (j->str);
                last = k;
            }
            g_free (rj);
            g_string_free (j, TRUE);
        }
        /* the comment state a view of this line starts from */
        gboolean cv0 = c0;
        if (r) {
            g_free (lines[i]);
            lines[i] = r;
            g_free (code);
            if (last == i) {
                gboolean b = cv0;
                code = fx_code_view (lines[i], &b);
            } else {
                /* the joined statement stands on this line now, without
                 * comments: it is its own view (with the braces of every
                 * line it came from). What follows looks at it like at
                 * any other line. (It was skipped: with "void main (\n) {
                 * float x = 1;" main () was never seen, the initialisers
                 * of the globals above had no place to go and the source
                 * ended in the #error below.) */
                cv0 = FALSE;
                code = g_strdup (lines[i]);
            }
            d = fx_sp (code);
            changed = TRUE;
        }

        /* constants: what later initialisers may name; a local "const"
         * that is none for GLSL ES */
        const char *te, *nm, *ne, *in, *se;
        if (fxe_single_decl (code, TRUE, &te, &nm, &ne, &in, &se)) {
            if (fxe_const_expr (&an, consts, in, se, 0, &cwork)) {
                g_hash_table_add (consts, g_strndup (nm, (gsize) (ne - nm)));
            } else if (an.depth > 0 && !cont) {
                memset (lines[i] + (d - code), ' ', 5);
                changed = TRUE;
            }
        } else if (global && !cont && !main_seen && !fxe_pp_unknown (&an) &&
                   fxe_single_decl (code, FALSE, &te, &nm, &ne, &in, &se) &&
                   !fxe_const_expr (&an, consts, in, se, 0, &cwork)) {
            /* "T name = <not constant>;" at global scope, in front of
             * main (): declared here, assigned there. (Not inside an #if
             * the model cannot evaluate: the assignments of all its
             * branches would run in main (), the last one winning -
             * another value than the shader's, silently. Left as written
             * the compiler rejects it and the effect is reported.) */
            g_string_append_printf (pending, " %.*s = %.*s;", (int) (ne - nm), lines[i] + (nm - code),
                                    (int) (se - in), lines[i] + (in - code));
            char *r2 = g_strdup_printf ("%.*s%s", (int) (ne - code), lines[i], lines[i] + (se - code));
            g_free (lines[i]);
            lines[i] = r2;
            g_free (code);
            gboolean b = cv0;
            code = fx_code_view (lines[i], &b);
            changed = TRUE;
        }

        /* main (): what the globals above were initialised with */
        if (fxe_main_line (&lines[i], code, an.depth == 0 && !cont, &main_hdr, &main_seen, pending))
            changed = TRUE;
        if (changed)
            n++;
        fx_an_braces (&an, code);
        g_free (code);
        i = last;       /* behind the lines a joined statement came from */
    }
    if (pending->len && nl > 0) {
        /* no main () behind them: better no shader than globals that are
         * never initialised */
        char *e = g_strconcat (lines[nl - 1], "\n#error global initialisers without a main () "
                                              "to move them into", NULL);
        g_free (lines[nl - 1]);
        lines[nl - 1] = e;
        n++;
    }
    g_string_free (pending, TRUE);
    g_hash_table_unref (consts);
    fx_an_clear (&an);
    return n;
}

/* One built source (version line, prelude, "#line 1", body) made GLSL ES.
 * Returns the number of body lines changed. */
static int fx_essl_src (char **src)
{
    char **lines = g_strsplit (*src, "\n", -1);
    guint nl = g_strv_length (lines), base = fx_body_base (lines, nl);
    int n = fxe_lexical (lines, nl, base);
    n += fxe_typed (lines, nl, base);
    if (n) {
        g_free (*src);
        *src = g_strjoinv ("\n", lines);
    }
    g_strfreev (lines);
    return n;
}

/* ---- stage 2: candidates for a line a GLSL ES compiler rejects */

static void fxe_cand_at (GPtrArray *out, guint line_index, char *text)
{
    if (!text)
        return;
    FxCand *c = g_new0 (FxCand, 1);
    c->text = text;
    c->at1 = line_index + 1;
    g_ptr_array_add (out, c);
}

/* an = the analysis up to lines[idx] (its declarations included), blk =
 * a block comment is open at the start of that line */
static void fx_essl_cands (FxAn *an, char **lines, guint nl, guint base, guint idx, gboolean blk,
                           GPtrArray *out)
{
    (void) nl;
    const char *line = lines[idx];
    gboolean b = blk, inc = FALSE;
    char *code = fx_code_view (line, &b);
    if (strlen (code) > FX_MAX_REPAIR_LINE || *fx_sp (code) == '#') {
        g_free (code);
        return;
    }
    char *typed = fxe_rewrite (an, line, code, &inc);
    char *both = NULL;
    if (typed) {
        gboolean b2 = blk;
        char *c2 = fx_code_view (typed, &b2);
        both = fxe_promote (typed, c2);
        g_free (c2);
    }
    fx_cand_add (out, line, typed);
    fx_cand_add (out, line, fxe_promote (line, code));
    fx_cand_add (out, line, both);
    /* the integer literals in the body of a macro the line uses (the
     * nearest "#define" of that name above the line). At most 16
     * different macro names are looked for: each search reads the body
     * lines above this one, and the macros of the prelude (texSample2D,
     * frac, ...) are never found there - a line of 2000 of them in a
     * source of 100 000 lines was 200 million line reads for every line
     * the log names. */
    const char *seen[16];
    gsize seen_len[16];
    int tried = 0, looked = 0;
    for (const char *p = code; *p && tried < 4 && looked < 16;) {
        if (!fx_is_ident_start (*p) || (p > code && (fx_is_ident (p[-1]) || p[-1] == '.'))) {
            p++;
            continue;
        }
        const char *q = p;
        while (fx_is_ident (*q))
            q++;
        char *name = g_strndup (p, (gsize) (q - p));
        gboolean again = FALSE;
        for (int v = 0; v < looked && !again; v++)
            again = seen_len[v] == (gsize) (q - p) && !strncmp (seen[v], p, (gsize) (q - p));
        if (!again && g_hash_table_contains (an->macros, name)) {
            seen[looked] = p;
            seen_len[looked] = (gsize) (q - p);
            looked++;
            for (guint k = idx; k-- > base;) {
                const char *dl = fx_sp (lines[k]);
                if (*dl != '#')
                    continue;
                const char *w = fx_sp (dl + 1);
                if (strncmp (w, "define", 6) || fx_is_ident (w[6]))
                    continue;
                const char *m = fx_sp (w + 6);
                if (strncmp (m, name, (gsize) (q - p)) || fx_is_ident (m[q - p]))
                    continue;
                gboolean z = FALSE;
                char *ck = fx_code_view (lines[k], &z);
                fxe_cand_at (out, k, fxe_promote (lines[k], ck));
                g_free (ck);
                tried++;
                break;
            }
        }
        g_free (name);
        p = q;
    }
    g_free (code);
}
