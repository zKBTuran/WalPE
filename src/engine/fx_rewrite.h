/* fx_rewrite.h - cheaper equivalents of effect shaders, and passes that
 * copy their input (included by fx_specialize.h).
 *
 * Every rewrite is keyed on statements of the shader itself, compared
 * with white space removed and by key, not by text (fx_keys.h): a workshop
 * copy of a stock shader gets it as long as the statements are untouched,
 * anything edited stays as it is. No wallpaper, effect or file name decides
 * anything. A rewrite runs on
 * the specialised sources (constants already baked) and only on code the
 * combos leave; when the result does not compile, fxs_build_program()
 * falls back to the generic program.
 *
 * Exact rewrites (fx_quality = exact, the default): the same arithmetic,
 * evaluated once per vertex instead of once per pixel.
 *   FXS_RW_PULSE  WE's pulse effect computes its pulse value - a function
 *                 of g_Time and constants only, including a read of the
 *                 noise texture at a coordinate that is the same for every
 *                 pixel - in the fragment shader. It moves to the vertex
 *                 shader and arrives as a flat varying: one sin, one
 *                 smoothstep, one pow and one texture read per pixel less.
 *   FXS_RW_SHAKE  WE's shake effect without a time-offset map: the shake
 *                 phase (sin, cos, two pow, ...) depends on g_Time and
 *                 constants only. Same move.
 * Rewrites that change pixels beyond rounding (fx_quality = fast only):
 *   FXS_RW_BLUR   WE's blur effect reads 13 / 7 / 3 texels a pass, one
 *                 texel apart: pairs of neighbours become one bilinear
 *                 read at their weighted centre (7 / 4 / 2 reads). The
 *                 same sum when the taps sit on texel centres; on a target
 *                 allocated below its nominal size (a 4K layer on a 1080p
 *                 monitor) the taps are half texels apart and the result
 *                 differs slightly.
 *   FXS_RW_CAST   shine / god rays ray march: WE's own next lower sample
 *                 count (50 -> 30, 30 -> 15, 15 -> 8), with the intensity
 *                 scale WE uses for that count.
 */

enum { FXS_RW_PULSE = 1, FXS_RW_SHAKE = 2, FXS_RW_BLUR = 4, FXS_RW_CAST = 8 };

/* The statements of s as one string: white space, "//" comments and
 * preprocessor lines left out; map[i] = offset in s of character i (one
 * more entry for the end). For text that is compiled code already
 * (fxs_active_lines). */
static char *fxs_strip_map (const char *s, guint **map)
{
    gsize n = strlen (s);
    char *o = g_malloc (n + 1);
    guint *m = g_new (guint, n + 1);
    gsize k = 0;
    for (gsize i = 0; i < n;) {
        gsize e = i;
        while (e < n && s[e] != '\n')
            e++;
        gsize f = i;
        while (f < e && (s[f] == ' ' || s[f] == '\t'))
            f++;
        if (f < e && s[f] != '#') {
            for (gsize j = f; j < e; j++) {
                if (s[j] == '/' && j + 1 < e && s[j + 1] == '/')
                    break;
                if (g_ascii_isspace (s[j]))
                    continue;
                m[k] = (guint) j;
                o[k++] = s[j];
            }
        }
        i = e + (e < n ? 1 : 0);
    }
    o[k] = 0;
    m[k] = (guint) n;
    *map = m;
    return o;
}

/* The COMPILED statements of src behind offset base as one string (dead
 * #if branches, preprocessor lines, comments and white space left out);
 * map[i] = offset in src of character i (one more entry for the end). */
static char *fxs_active_map (const char *src, gsize base, guint **map)
{
    gsize n = strlen (src);
    char *o = g_malloc (n + 1);
    guint *m = g_new (guint, n + 1);
    gsize k = 0;
    FxAn an;
    fx_an_init (&an);
    for (gsize i = 0; i < n;) {
        gsize e = i;
        while (e < n && src[e] != '\n')
            e++;
        char *line = g_strndup (src + i, e - i);
        char *code = fx_code_view (line, &an.in_comment);
        const char *d = fx_sp (code);
        if (*d == '#') {
            g_free (fx_an_directive (&an, line, d, FALSE));
        } else if (i >= base && fx_an_active (&an)) {
            for (gsize j = 0; code[j]; j++) {
                if (g_ascii_isspace (code[j]))
                    continue;
                m[k] = (guint) (i + j);
                o[k++] = code[j];
            }
        }
        g_free (code);
        g_free (line);
        i = e + (e < n ? 1 : 0);
    }
    fx_an_clear (&an);
    o[k] = 0;
    m[k] = (guint) n;
    *map = m;
    return o;
}

/* Replace the one place in the body of *src (behind "#line 1") whose
 * compiled statements are `pat` by `to`. Line breaks and preprocessor
 * lines inside the replaced span stay where they are (line numbers and
 * the #if structure do not move); what else the span holds - the
 * statements, and any dead branch between them - goes; `to` takes the
 * place of the first statement. FALSE (nothing changed) when pat is
 * absent or occurs more than once. */
static gboolean fxs_replace (char **src, const char *pat, const char *to)
{
    const char *mark = strstr (*src, "#line 1\n");
    if (!mark)
        return FALSE;
    gsize base = (gsize) (mark - *src) + 8;
    guint *map = NULL;
    char *st = fxs_active_map (*src, base, &map);
    const char *hit = strstr (st, pat);
    gboolean ok = hit && !strstr (hit + 1, pat);
    if (ok) {
        gsize a = map[hit - st];
        gsize b = map[hit - st + strlen (pat) - 1] + 1;
        const char *t = *src;
        GString *o = g_string_new_len (t, (gssize) a);
        g_string_append (o, to);
        for (gsize i = a; i < b; i++) {
            if (t[i] != '\n')
                continue;
            g_string_append_c (o, '\n');
            gsize f = i + 1;
            while (f < b && (t[f] == ' ' || t[f] == '\t'))
                f++;
            if (f < b && t[f] == '#') {
                gsize e = f;
                while (e < b && t[e] != '\n')
                    e++;
                g_string_append_len (o, t + f, (gssize) (e - f));
            }
        }
        g_string_append (o, t + b);
        g_free (*src);
        *src = g_string_free (o, FALSE);
    }
    g_free (st);
    g_free (map);
    return ok;
}

/* text into the prelude, right in front of "#line 1" */
static void fxs_prelude_add (char **src, const char *text)
{
    const char *mark = strstr (*src, "#line 1\n");
    if (!mark)
        return;
    GString *o = g_string_new_len (*src, mark - *src);
    g_string_append (o, text);
    g_string_append (o, mark);
    g_free (*src);
    *src = g_string_free (o, FALSE);
}

/* statements in front of the brace that closes main() */
static gboolean fxs_main_append (char **src, const char *text)
{
    const char *m = NULL;
    for (const char *p = *src; (p = strstr (p, "void main")) != NULL; p += 9)
        m = p;
    const char *b = m ? strchr (m, '{') : NULL;
    if (!b)
        return FALSE;
    int depth = 0;
    const char *e = NULL;
    for (const char *p = b; *p; p++) {
        if (*p == '{')
            depth++;
        else if (*p == '}' && --depth == 0) {
            e = p;
            break;
        }
    }
    if (!e)
        return FALSE;
    GString *o = g_string_new_len (*src, e - *src);
    g_string_append (o, text);
    g_string_append (o, e);
    g_free (*src);
    *src = g_string_free (o, FALSE);
    return TRUE;
}

/* the line of `act` (compiled lines, fxs_active_lines) that declares
 * `name` at global scope as a uniform or a baked constant; NULL = none */
static char *fxs_decl_line (const char *act, const char *name)
{
    gsize nl = strlen (name);
    for (const char *p = act; *p;) {
        const char *eol = strchr (p, '\n');
        gsize n = eol ? (gsize) (eol - p) : strlen (p);
        gsize l = 0;
        if (fxs_word_at (p, "uniform", &l) || fxs_word_at (p, "const", &l)) {
            for (const char *q = p; q + nl <= p + n; q++) {
                if (strncmp (q, name, nl) || fx_is_ident (q[nl]) || (q > p && fx_is_ident (q[-1])))
                    continue;
                const char *r = fx_sp (q + nl);
                if (*r == ';' || *r == '=')
                    return g_strndup (p, n);
                break;
            }
        }
        p += n + (eol ? 1 : 0);
    }
    return NULL;
}

/* Make every name of `names` usable in the vertex shader: one that the
 * vertex source does not declare gets the fragment shader's declaration
 * (a uniform, or the constant it was baked to). FALSE when a name is
 * declared in neither. */
static gboolean fxs_share_decls (char **vsrc, const char *fsrc, const char *const *names)
{
    char *va = fxs_active_lines (*vsrc), *fa = fxs_active_lines (fsrc);
    GString *add = g_string_new (NULL);
    gboolean ok = TRUE;
    for (int i = 0; names[i] && ok; i++) {
        char *d = fxs_decl_line (va, names[i]);
        if (!d) {
            d = fxs_decl_line (fa, names[i]);
            if (d)
                g_string_append_printf (add, "%s\n", d);
            else
                ok = FALSE;
        }
        g_free (d);
    }
    if (ok && add->len)
        fxs_prelude_add (vsrc, add->str);
    g_string_free (add, TRUE);
    g_free (va);
    g_free (fa);
    return ok;
}

/* an object-like #define of the fragment source the vertex source lacks */
static void fxs_share_define (char **vsrc, const char *fsrc, const char *name)
{
    char *pat = g_strdup_printf ("#define %s ", name);
    const char *d = strstr (fsrc, pat);
    if (d && !strstr (*vsrc, pat)) {
        const char *eol = strchr (d, '\n');
        char *line = eol ? g_strndup (d, (gsize) (eol - d) + 1) : g_strconcat (d, "\n", NULL);
        fxs_prelude_add (vsrc, line);
        g_free (line);
    }
    g_free (pat);
}

/* ---- frame-constant statement runs (FXS_RW_PULSE, FXS_RW_SHAKE)
 *
 * A run of consecutive statements of the fragment main that reads nothing
 * but uniforms, constants and its own locals computes the same value for
 * every pixel of a frame. It is moved, as written, to the end of the
 * vertex main; the fragment shader gets the result as a flat varying.
 * A texture read in the run becomes textureLod(.., 0.0): with a
 * coordinate that does not vary over the quad the fragment shader reads
 * the base level too.
 *
 * Which run: the table names the first and the last statement of the run
 * and the variable that carries the result out of it (WE's pulse and
 * shake effects, in every version the statements in between may differ).
 * That the run really is frame-constant is then checked on the text
 * found: every identifier in it must be a builtin, a uniform / baked
 * constant, an object-like macro or a local the run declares. */

typedef struct {
    guint bit;
    FxKeyId first, last;        /* the run's first and last statement (fx_keys.h) */
    const char *var, *vtype;    /* the result and its type */
    FxKeyId need_f, need_v;     /* must be in the compiled fragment / vertex code
                                 * (FXK_NONE: nothing is asked) */
    FxKeyId never_f;            /* must not be in the compiled fragment code */
    const char *subst[7];       /* pairs: text in the run -> its frame-constant equal */
} FxsHoist;

static const FxsHoist fxs_hoists[] = {
    { FXS_RW_PULSE, FXK_PULSE_FIRST, FXK_PULSE_LAST,
      "pulse", "float", FXK_NONE, FXK_NONE, FXK_NONE, { NULL } },
    /* shake: the phase is the same for every pixel unless a time-offset
     * map shifts it; v_Bounds is g_Bounds, prepared by the vertex shader */
    { FXS_RW_SHAKE, FXK_SHAKE_FIRST_A, FXK_SHAKE_LAST,
      "offset", "float", FXK_SHAKE_PHASE0, FXK_SHAKE_VBOUNDS, FXK_SHAKE_PHASEMAP,
      { "flowPhase", "0.0", "v_Bounds.x", "g_Bounds.x", "v_Bounds.y", "(1.0 / (g_Bounds.y - g_Bounds.x))", NULL } },
    { FXS_RW_SHAKE, FXK_SHAKE_FIRST_B, FXK_SHAKE_LAST,
      "offset", "float", FXK_SHAKE_PHASE0, FXK_SHAKE_VBOUNDS, FXK_SHAKE_PHASEMAP,
      { "flowPhase", "0.0", "v_Bounds.x", "g_Bounds.x", "v_Bounds.y", "(1.0 / (g_Bounds.y - g_Bounds.x))", NULL } },
};

/* names a frame-constant run may use besides uniforms, macros and its
 * own locals (types, builtins, the prelude's HLSL spellings) */
static gboolean fxs_hoist_builtin (const char *w, gsize n)
{
    static const char *const ok[] = {
        "float", "vec2", "vec3", "vec4", "int", "bool", "float2", "float3", "float4", "const",
        "sin", "cos", "tan", "pow", "exp", "exp2", "log", "log2", "sqrt", "abs", "sign", "floor", "ceil",
        "fract", "frac", "mod", "fmod", "min", "max", "clamp", "saturate", "mix", "lerp", "step",
        "smoothstep", "dot", "length", "normalize", "atan", "atan2", "radians", "degrees",
        "CAST2", "CAST3", "CAST4", "textureLod", "true", "false",
    };
    for (guint i = 0; i < G_N_ELEMENTS (ok); i++)
        if (strlen (ok[i]) == n && !strncmp (ok[i], w, n))
            return TRUE;
    return FALSE;
}

/* texSample2D(a, b) / texture2D(a, b) -> textureLod(a, b, 0.0); FALSE on
 * a call this cannot read (a third argument, unbalanced) */
static gboolean fxs_hoist_tex (GString *t)
{
    static const char *const fn[] = { "texSample2D(", "texture2D(" };
    for (int k = 0; k < 2; k++) {
        gsize from = 0;
        for (;;) {
            const char *p = strstr (t->str + from, fn[k]);
            if (!p)
                break;
            gsize a = (gsize) (p - t->str), open = a + strlen (fn[k]) - 1, i = open;
            int depth = 0, commas = 0;
            for (; t->str[i]; i++) {
                if (t->str[i] == '(')
                    depth++;
                else if (t->str[i] == ')' && --depth == 0)
                    break;
                else if (t->str[i] == ',' && depth == 1)
                    commas++;
            }
            if (!t->str[i] || commas != 1)
                return FALSE;
            g_string_insert (t, (gssize) i, ", 0.0");
            g_string_erase (t, (gssize) a, (gssize) (open - a));
            g_string_insert (t, (gssize) a, "textureLod");
            from = a + 10;
        }
    }
    return TRUE;
}

/* every identifier of the run is frame-constant; the uniforms and macros
 * it names are collected for the vertex shader */
static gboolean fxs_hoist_check (const char *run, const char *var, const char *fact_lines,
                                 const char *vact_lines, const char *fsrc, GPtrArray *uniforms,
                                 GPtrArray *macros)
{
    GHashTable *locals = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add (locals, g_strdup (var));
    gboolean ok = TRUE;
    const char *prev_word = NULL;
    gsize prev_len = 0;
    for (const char *p = run; *p && ok;) {
        if (!fx_is_ident_start (*p)) {
            if (g_ascii_isdigit (*p)) {
                /* a number: digits, '.', exponent, suffix */
                while (g_ascii_isalnum (*p) || *p == '.')
                    p++;
                prev_word = NULL;
                continue;
            }
            if (!g_ascii_isspace (*p))
                prev_word = *p == '.' ? "." : NULL, prev_len = 1;
            p++;
            continue;
        }
        const char *b = p;
        while (fx_is_ident (*p))
            p++;
        gsize n = (gsize) (p - b);
        gboolean field = prev_word && prev_word[0] == '.' && prev_len == 1;
        gsize tl = 0;
        gboolean after_type = prev_word && prev_word[0] != '.' && fxs_type_at (prev_word, &tl) &&
                              tl == prev_len;
        char *w = g_strndup (b, n);
        if (field) {
            /* swizzle / member */
        } else if (after_type) {
            g_hash_table_add (locals, g_strdup (w));   /* "float noise" */
        } else if (fxs_hoist_builtin (b, n) || g_hash_table_contains (locals, w)) {
            /* fine */
        } else {
            char *d = fxs_decl_line (fact_lines, w);
            if (!d)
                d = fxs_decl_line (vact_lines, w);
            char *def = g_strdup_printf ("#define %s ", w);
            GPtrArray *into = d ? uniforms : strstr (fsrc, def) ? macros : NULL;
            gboolean have = FALSE;
            for (guint i = 0; into && i < into->len && !have; i++)
                have = !strcmp (into->pdata[i], w);
            if (into) {
                if (!have)
                    g_ptr_array_add (into, g_strdup (w));
            } else {
                ok = FALSE;   /* a varying, a local from outside, gl_FragCoord, ... */
            }
            g_free (d);
            g_free (def);
        }
        g_free (w);
        prev_word = b;
        prev_len = n;
    }
    g_hash_table_unref (locals);
    return ok;
}

static gboolean fxs_rw_hoist (const FxsHoist *h, char **vsrc, char **fsrc, const char *fact,
                              const char *vact)
{
    if ((h->need_f != FXK_NONE && !fxk_has (fact, h->need_f)) ||
        (h->need_v != FXK_NONE && !fxk_has (vact, h->need_v)))
        return FALSE;
    if ((h->never_f != FXK_NONE && fxk_has (fact, h->never_f)) || strstr (fact, "_we_hoist"))
        return FALSE;
    gsize last_len = 0;
    const char *s0 = fxk_find (fact, h->first, NULL);
    const char *s1 = s0 ? fxk_find (s0, h->last, &last_len) : NULL;
    if (!s0 || !s1 || fxk_find (s0 + 1, h->first, NULL))
        return FALSE;
    /* the run, as the shader has it */
    char *pat = g_strndup (s0, (gsize) (s1 - s0) + last_len);
    /* the run as written: the compiled lines hold it in one piece */
    char *fl = fxs_active_lines (*fsrc), *vl = fxs_active_lines (*vsrc);
    guint *map = NULL;
    char *st = fxs_strip_map (fl, &map);
    const char *hit = strstr (st, pat);
    gboolean ok = hit != NULL;
    GString *run = NULL;
    GPtrArray *un = g_ptr_array_new_with_free_func (g_free), *mac = g_ptr_array_new_with_free_func (g_free);
    char *v = g_strdup (*vsrc), *f = g_strdup (*fsrc);
    if (ok) {
        gsize a = map[hit - st], b = map[hit - st + strlen (pat) - 1] + 1;
        run = g_string_new_len (fl + a, (gssize) (b - a));
        for (int i = 0; h->subst[i] && h->subst[i + 1]; i += 2) {
            gsize from = 0, ln = strlen (h->subst[i]);
            for (const char *q; (q = strstr (run->str + from, h->subst[i])) != NULL;) {
                gsize at = (gsize) (q - run->str);
                g_string_erase (run, (gssize) at, (gssize) ln);
                g_string_insert (run, (gssize) at, h->subst[i + 1]);
                from = at + strlen (h->subst[i + 1]);
            }
        }
        ok = fxs_hoist_tex (run) && fxs_hoist_check (run->str, h->var, fl, vl, *fsrc, un, mac);
    }
    if (ok) {
        g_ptr_array_add (un, NULL);
        ok = fxs_share_decls (&v, f, (const char *const *) un->pdata);
        for (guint i = 0; ok && i < mac->len; i++) {
            fxs_share_define (&v, f, mac->pdata[i]);
            char *def = g_strdup_printf ("#define %s ", (char *) mac->pdata[i]);
            ok = strstr (v, def) != NULL;
            g_free (def);
        }
    }
    if (ok) {
        char *assign = g_strdup_printf ("%s = _we_hoist0;", h->var);
        char *body = g_strdup_printf ("\t{ %s %s;\n%s\n\t  _we_hoist0 = %s; }\n", h->vtype, h->var,
                                      run->str, h->var);
        ok = fxs_replace (&f, pat, assign) && fxs_main_append (&v, body);
        g_free (assign);
        g_free (body);
    }
    if (ok) {
        char *dv = g_strdup_printf ("flat out %s _we_hoist0;\n", h->vtype);
        char *df = g_strdup_printf ("flat in %s _we_hoist0;\n", h->vtype);
        fxs_prelude_add (&v, dv);
        fxs_prelude_add (&f, df);
        g_free (dv);
        g_free (df);
        g_free (*vsrc);
        g_free (*fsrc);
        *vsrc = v;
        *fsrc = f;
    } else {
        g_free (v);
        g_free (f);
    }
    if (run)
        g_string_free (run, TRUE);
    g_ptr_array_unref (un);
    g_ptr_array_unref (mac);
    g_free (st);
    g_free (map);
    g_free (fl);
    g_free (vl);
    g_free (pat);
    return ok;
}

/* ---- FXS_RW_BLUR (fast) */

/* fxs_replace () for a statement that was found by key: n characters at
 * hit, in compiled code without white space */
static gboolean fxs_replace_found (char **src, const char *hit, gsize n, const char *to)
{
    char *pat = g_strndup (hit, n);
    gboolean ok = fxs_replace (src, pat, to);
    g_free (pat);
    return ok;
}

static gboolean fxs_rw_blur (char **fsrc, const char *fact)
{
    /* the blur effect's gaussian pass has one statement per kernel that
     * sums all its taps (fx_keys.h); the statement found in the shader is
     * what gets replaced. d = one tap step */
    gsize kl = 0;
    const char *hit;
    if ((hit = fxk_find (fact, FXK_BLUR13, &kl)) != NULL)
        /* pairs (5,4) (3,2) (1,0) and their mirrors: weight = the sum,
         * offset = the weighted mean of the two tap indices */
        return fxs_replace_found (fsrc, hit, kl,
            "vec2 _wd = v_TexCoord[7] - v_TexCoord[6]; vec2 _wc = v_TexCoord[6];"
            " vec4 albedo = texSample2D(g_Texture0, _wc) * 0.171834"
            " + (texSample2D(g_Texture0, _wc + _wd * 1.43155536) + texSample2D(g_Texture0, _wc - _wd * 1.43155536)) * 0.275763"
            " + (texSample2D(g_Texture0, _wc + _wd * 3.34459824) + texSample2D(g_Texture0, _wc - _wd * 3.34459824)) * 0.114722"
            " + (texSample2D(g_Texture0, _wc + _wd * 5.26694071) + texSample2D(g_Texture0, _wc - _wd * 5.26694071)) * 0.023597;");
    if ((hit = fxk_find (fact, FXK_BLUR7, &kl)) != NULL)
        /* pairs (3,4) (5,6) on one side, (2,1) and tap 0 on the other:
         * four reads */
        return fxs_replace_found (fsrc, hit, kl,
            "vec2 _wd = v_TexCoord[4] - v_TexCoord[3]; vec2 _wc = v_TexCoord[3];"
            " vec4 albedo = texSample2D(g_Texture0, _wc + _wd * 2.35156440) * 0.202817"
            " + texSample2D(g_Texture0, _wc + _wd * 0.46943378) * 0.404486"
            " + texSample2D(g_Texture0, _wc - _wd * 1.40919988) * 0.321393"
            " + texSample2D(g_Texture0, _wc - _wd * 3.0) * 0.071303;");
    if ((hit = fxk_find (fact, FXK_BLUR3, &kl)) != NULL)
        return fxs_replace_found (fsrc, hit, kl,
            "vec2 _wd = v_TexCoord[2] - v_TexCoord[1]; vec2 _wc = v_TexCoord[1];"
            " vec4 albedo = (texSample2D(g_Texture0, _wc + _wd * 0.5) + texSample2D(g_Texture0, _wc - _wd * 0.5)) * 0.5;");
    return FALSE;
}

/* ---- FXS_RW_CAST (fast) */

/* the combo #defines sit in front of the prelude: "#define SAMPLES n" */
static gboolean fxs_set_combo (char **src, const char *name, int from, int to)
{
    char *a = g_strdup_printf ("#define %s %d\n", name, from);
    char *b = g_strdup_printf ("#define %s %d\n", name, to);
    const char *stop = strstr (*src, "#define GLSL 1");
    const char *hit = strstr (*src, a);
    gboolean ok = hit && stop && hit < stop;
    if (ok) {
        GString *o = g_string_new_len (*src, hit - *src);
        g_string_append (o, b);
        g_string_append (o, hit + strlen (a));
        g_free (*src);
        *src = g_string_free (o, FALSE);
    }
    g_free (a);
    g_free (b);
    return ok;
}

static gboolean fxs_rw_cast (char **vsrc, char **fsrc, const char *fact)
{
    /* both effects march along a direction over a fixed number of reads,
     * add each sample with a falloff by its index and scale the sum by an
     * intensity that is 3 divided by the number of reads (fx_keys.h: the
     * loop, the function that tells shine from god rays, the counts) */
    if (!fxk_has (fact, FXK_CAST_LOOP))
        return FALSE;
    gboolean shine = fxk_has (fact, FXK_CAST_GATHER);
    int from = -1, to = -1;
    if (shine) {
        /* SAMPLES 4: 50, 3: 30, 2: 15, 1: 8 (kept), 0: 4 (kept) */
        if (fxk_has (fact, FXK_CAST_N50))
            from = 4, to = 3;
        else if (fxk_has (fact, FXK_CAST_N30))
            from = 3, to = 2;
        else if (fxk_has (fact, FXK_CAST_N15))
            from = 2, to = 1;
    } else if (fxk_has (fact, FXK_CAST_N50)) {
        /* god rays: SAMPLES 1: 50, 0: 30 (kept) */
        from = 1, to = 0;
    }
    if (from < 0 || !fxs_set_combo (fsrc, "SAMPLES", from, to))
        return FALSE;
    fxs_set_combo (vsrc, "SAMPLES", from, to);   /* the combos head both sources */
    return TRUE;
}

static guint fxs_rewrite (struct FxSpec *sp, EffPass *pass, char **vsrc, char **fsrc)
{
    (void) sp;
    guint rw = 0;
    if (pass->moves_verts || pass->xf_sign)
        return 0;
    char *fact = fx_active_code (*fsrc), *vact = fx_active_code (*vsrc);
    /* WALPE_FX_HOIST=0: constants baked, nothing moved (diagnostics) */
    static gsize once;
    static gboolean hoist = TRUE;
    if (g_once_init_enter (&once)) {
        const char *e = g_getenv ("WALPE_FX_HOIST");
        hoist = !(e && *e == '0');
        g_once_init_leave (&once, 1);
    }
    for (guint i = 0; hoist && i < G_N_ELEMENTS (fxs_hoists) && !rw; i++)
        if (fxs_rw_hoist (&fxs_hoists[i], vsrc, fsrc, fact, vact))
            rw |= fxs_hoists[i].bit;
    g_free (vact);
    if (fxs_fast ()) {
        if (fxs_rw_blur (fsrc, fact))
            rw |= FXS_RW_BLUR;
        else if (fxs_rw_cast (vsrc, fsrc, fact))
            rw |= FXS_RW_CAST;
    }
    g_free (fact);
    return rw;
}

/* ---- identity passes
 * A pass is an identity when it writes, for every pixel, the texel of
 * g_Texture0 under that pixel. Recognised: WE's opacity effect without a
 * mask and with the user alpha baked to 1 (the alpha is multiplied by one
 * twice). */
static gboolean fxs_identity (struct FxSpec *sp, EffPass *pass, const char *vsrc,
                              const char *fsrc)
{
    if (pass->moves_verts || pass->xf_sign || pass->target[0])
        return FALSE;
    const FxsConst *a = fxs_find (sp, "g_UserAlpha");
    if (!a || !a->baked || a->type != FXS_T_FLOAT || a->v[0] != 1.0f)
        return FALSE;
    char *f = fx_active_code (fsrc), *v = fx_active_code (vsrc);
    /* the whole main of the opacity effect's fragment shader with its mask
     * off (fx_keys.h), and a vertex shader that hands the texture coordinate
     * through and fills the target as the plain quad */
    gboolean ok = fxk_has (f, FXK_OPACITY_MAIN) &&
                  strstr (v, "v_TexCoord.xy=a_TexCoord;") != NULL &&
                  strstr (v, "gl_Position=mul(vec4(a_Position,1.0),g_ModelViewProjectionMatrix);") != NULL;
    g_free (f);
    g_free (v);
    if (ok)
        g_atomic_int_inc (&fxs_n_identity);
    return ok;
}

/* TRUE when every pass of the chain is an identity and skipping it
 * cannot change a pixel: the input is a texture of exactly the target's
 * size that is read like a render target (linear, clamped, no mip chain
 * in use) - an earlier chain's output. With fx_quality = fast any input
 * qualifies (the copy only resampled it). */
static gboolean fxs_chain_identity (EffectChain *ch)
{
    if (!ch || ch->n_passes < 1)
        return FALSE;
    for (int i = 0; i < ch->n_passes; i++) {
        const EffPass *p = &ch->passes[i];
        if (p->is_copy || !p->spec || !p->spec->identity)
            return FALSE;
        /* a script may change the constant: fxs_pre_run() relinks then,
         * which only happens when the pass runs - look here as well */
        for (int k = 0; k < p->n_consts; k++)
            if (p->consts[k].loc < 0 && p->consts[k].dirty)
                return FALSE;
    }
    return TRUE;
}

static inline gboolean fxs_chain_noop (EffectChain *ch, GLuint input, int w, int h, float k)
{
    if (G_LIKELY (!ch->n_passes || !ch->passes[0].spec || !ch->passes[0].spec->identity))
        return FALSE;
    if (!fxs_chain_identity (ch))
        return FALSE;
    if (fxs_fast ())
        return TRUE;
    GLint tw = 0, th = 0, minf = 0, magf = 0, ws = 0, wt = 0, prev = 0;
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture (GL_TEXTURE_2D, input);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &ws);
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wt);
    glBindTexture (GL_TEXTURE_2D, (GLuint) prev);
    return tw == MAX (1, (int) ceilf (w * k)) && th == MAX (1, (int) ceilf (h * k)) &&
           minf == GL_LINEAR && magf == GL_LINEAR && ws == GL_CLAMP_TO_EDGE && wt == GL_CLAMP_TO_EDGE;
}
