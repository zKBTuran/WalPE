/* fx_fuse.h - a run of displacement effects as one pass (included by
 * scenenode_effects.c behind struct FxSet; fx_quality = fast only).
 *
 * Most effect passes of real scenes are cheap UV displacements stacked on
 * one layer: water waves, shake, water ripple, foliage sway, each its own
 * full-target pass that reads a mask or two, moves the texture coordinate
 * and copies the layer through. A run of n such effects costs n target
 * writes and n reads of the picture. Here the run becomes ONE pass: every
 * effect's coordinate computation is chained (the last effect first, as
 * the picture flows the other way) and the picture is read once at the
 * final coordinate.
 *
 *   sequential:  out_n(p) = out_{n-1}(c_n(p)),  out_{n-1}(q) = out_{n-2}(c_{n-1}(q)), ...
 *   fused:       q = p;  q = c_n(q);  q = c_{n-1}(q); ...  q = c_1(q);   out(p) = in(q)
 *
 * c_i(q) is effect i's own fragment code, turned into a function, with
 * its varyings evaluated at q instead of at the pixel: a varying is an
 * affine function over the pass quad, so V(q) = V(p) + dV/dx * dx +
 * dV/dy * dy with the derivatives the rasteriser already has (dFdx /
 * dFdy). Nothing about an effect is assumed beyond what is checked on its
 * source: its fragment main ends in "gl_FragColor = texSample2D(
 * g_Texture0, <coordinate>);" and uses g_Texture0 and gl_FragColor
 * nowhere else ("pure displacement"). Its vertex shader runs unchanged.
 *
 * Not the same pixels: the sequential chain resamples the picture
 * bilinearly once per effect and rounds it to 8 bits in between; the
 * fused pass samples it once (it is the sharper of the two). That is why
 * this runs with fx_quality = fast only.
 *
 * How it is built: every member's specialised sources (all its material
 * constants baked - a member with a script-bound constant is not fused)
 * are preprocessed (its combos decide the #if branches), every name it
 * declares at global scope gets a per-member suffix, its samplers move to
 * free texture units of the fused pass (g_Texture<k> and
 * g_Texture<k>Resolution keep their naming scheme, so the executor feeds
 * the fused program like any other), engine uniforms keep their names
 * and are declared once. The member chains stay as they are: whenever a
 * member is switched off, relinked (a script wrote one of its constants)
 * or the fused program did not build, they run one by one as before.
 */

#define FXF_MAX 6    /* members of one fused pass */

typedef struct {
    EffectChain *chain;          /* one pass: the fused program */
    int first, last;             /* member chains: indices in set->chains */
    int n;
    int member[FXF_MAX];
    FxsProg *member_prog[FXF_MAX];   /* what each member was when fused */
} FxFuse;

struct FxFuseSet {
    FxFuse *g;
    int n;
    char *name;     /* the layer, for messages (the groups are rebuilt when
                     * a pass of the set changes programs: fxf_refresh) */
};

static gint fxf_n_groups, fxf_n_members;

void fx_fuse_stats (int *groups, int *members)
{
    if (groups) *groups = g_atomic_int_get (&fxf_n_groups);
    if (members) *members = g_atomic_int_get (&fxf_n_members);
}

/* ---- text helpers */

/* the specialised sources a pass was compiled from */
static gboolean fxf_final_sources (EffPass *pass, char **v, char **f)
{
    struct FxSpec *sp = pass->spec;
    if (!sp || !sp->src || !sp->n_baked)
        return FALSE;
    /* the constants the exact program keeps as uniforms because a pow()
     * names them are constants here: the fused pass is no exact copy
     * anyway, and it has no uniforms of its members to feed */
    for (int i = 0; i < sp->n; i++)
        if (sp->c[i].hazard && !sp->c[i].dynamic)
            sp->c[i].baked = 1;
    *v = fxs_bake_source (sp->src->vsrc, sp);
    *f = fxs_bake_source (sp->src->fsrc, sp);
    fxs_rewrite (sp, pass, v, f);
    for (int i = 0; i < sp->n; i++)
        if (sp->c[i].hazard)
            sp->c[i].baked = 0;
    return TRUE;
}

/* A member source without the common prelude, #if resolved: its combo
 * #defines, whatever fx_specialize added in front of "#line 1", and the
 * compiled lines of the body. Active #defines stay (behind an #undef:
 * every member defines its own). Comments are blanked. */
static char *fxf_resolve (const char *src, gboolean vertex)
{
    const char *stage_end = vertex ? "#define varying out\n#define attribute in\n"
                                   : "void clip(vec4 x) { if (any(lessThan(x, vec4(0.0)))) discard; }\n";
    const char *common = strstr (src, "#define GLSL 1\n");
    const char *rest = strstr (src, stage_end);
    if (!common || !rest || strncmp (src, "#version 330\n", 13))
        return NULL;
    rest += strlen (stage_end);
    /* combos (between the version line and the prelude), then the rest */
    char *combos = g_strndup (src + 13, (gsize) (common - src - 13));
    char *text = g_strconcat (combos, rest, NULL);
    g_free (combos);
    char **lines = g_strsplit (text, "\n", -1);
    FxAn an;
    fx_an_init (&an);
    GString *o = g_string_new (NULL);
    for (char **l = lines; *l; l++) {
        char *code = fx_code_view (*l, &an.in_comment);
        const char *d = fx_sp (code);
        if (*d == '#') {
            gboolean active = fx_an_active (&an);
            g_free (fx_an_directive (&an, *l, d, FALSE));
            const char *w = fx_sp (d + 1);
            if (active && !strncmp (w, "define", 6) && !fx_is_ident (w[6])) {
                const char *nm = fx_sp (w + 6), *e = nm;
                while (fx_is_ident (*e))
                    e++;
                g_string_append (o, "#undef ");
                g_string_append_len (o, nm, e - nm);
                g_string_append_printf (o, "\n%s\n", d);
            } else if (active && (!strncmp (w, "undef", 5) || !strncmp (w, "extension", 9) ||
                                  !strncmp (w, "pragma", 6))) {
                g_string_append_printf (o, "%s\n", d);
            }
        } else if (fx_an_active (&an) && *d) {
            g_string_append_printf (o, "%s\n", code);
        }
        g_free (code);
    }
    fx_an_clear (&an);
    g_strfreev (lines);
    g_free (text);
    return g_string_free (o, FALSE);
}

typedef struct {
    char *name;
    char cls;        /* 'u' uniform, 'a' attribute, 'v' varying, 'f' function, 'g' other global */
    char *type;      /* varyings: the type */
    gboolean array;
} FxfDecl;

static void fxf_decl_clear (gpointer p)
{
    FxfDecl *d = p;
    g_free (d->name);
    g_free (d->type);
}

/* the names a resolved source declares at global scope */
static GArray *fxf_globals (const char *text)
{
    GArray *out = g_array_new (FALSE, TRUE, sizeof (FxfDecl));
    g_array_set_clear_func (out, fxf_decl_clear);
    GString *st = g_string_new (NULL);
    int paren = 0;
    for (const char *p = text; *p;) {
        if (*p == '#' && (p == text || p[-1] == '\n')) {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        char c = *p++;
        if (c == '(')
            paren++;
        else if (c == ')')
            paren--;
        if ((c == ';' || c == '{') && paren == 0) {
            /* st = a declaration, a prototype, or a function / struct head */
            FxfDecl d = { 0 };
            const char *s = st->str;
            while (g_ascii_isspace (*s))
                s++;
            for (char *t = st->str; *t; t++)
                if (*t == '\n' || *t == '\r')
                    *t = ' ';      /* fx_sp and the word tests know blanks and tabs */
            const char *open = strchr (s, '(');
            gsize l = 0;
            gboolean is_struct = fxs_word_at (s, "struct", &l);
            if (is_struct) {
                const char *b = fx_sp (s + l), *e = b;
                while (fx_is_ident (*e))
                    e++;
                d.name = g_strndup (b, (gsize) (e - b));
                d.cls = 'g';
            } else if (open && (!strchr (s, '=') || strchr (s, '=') > open)) {
                const char *e = open;
                while (e > s && g_ascii_isspace (e[-1]))
                    e--;
                const char *b = e;
                while (b > s && fx_is_ident (b[-1]))
                    b--;
                d.name = g_strndup (b, (gsize) (e - b));
                d.cls = 'f';
            } else if (*s) {
                const char *e = s + strlen (s);
                const char *eq = strchr (s, '='), *br = strchr (s, '[');
                if (eq)
                    e = eq;
                if (br && br < e) {
                    e = br;
                    d.array = TRUE;
                }
                while (e > s && g_ascii_isspace (e[-1]))
                    e--;
                const char *b = e;
                while (b > s && fx_is_ident (b[-1]))
                    b--;
                d.name = g_strndup (b, (gsize) (e - b));
                const char *q = s;
                if (fxs_word_at (q, "flat", &l))
                    q = fx_sp (q + l);
                d.cls = fxs_word_at (q, "uniform", &l) ? 'u' : fxs_word_at (q, "attribute", &l) ? 'a'
                      : (fxs_word_at (q, "varying", &l) || fxs_word_at (q, "in", &l) ||
                         fxs_word_at (q, "out", &l)) ? 'v' : 'g';
                if (d.cls == 'v') {
                    const char *t = fx_sp (q + l), *te = t;
                    while (fx_is_ident (*te))
                        te++;
                    d.type = g_strndup (t, (gsize) (te - t));
                }
            }
            if (d.name && *d.name && fx_is_ident_start (*d.name))
                g_array_append_val (out, d);
            else
                fxf_decl_clear (&d);
            g_string_truncate (st, 0);
            if (c == '{') {
                /* skip the body */
                int depth = 1;
                while (*p && depth > 0) {
                    if (*p == '{')
                        depth++;
                    else if (*p == '}')
                        depth--;
                    p++;
                }
                if (is_struct) {
                    while (*p && *p != ';')
                        p++;
                    if (*p)
                        p++;
                }
            }
            continue;
        }
        g_string_append_c (st, c);
    }
    g_string_free (st, TRUE);
    return out;
}

/* every identifier of text that is a key of map replaced by its value
 * (members after a '.' are left alone) */
static char *fxf_rename (const char *text, GHashTable *map)
{
    GString *o = g_string_sized_new (strlen (text) + 512);
    for (const char *p = text; *p;) {
        if (fx_is_ident_start (*p) && (p == text || !fx_is_ident (p[-1]))) {
            const char *b = p;
            while (fx_is_ident (*p))
                p++;
            const char *q = b;
            while (q > text && (q[-1] == ' ' || q[-1] == '\t'))
                q--;
            gboolean member = q > text && q[-1] == '.';
            char *w = g_strndup (b, (gsize) (p - b));
            const char *r = member ? NULL : g_hash_table_lookup (map, w);
            g_string_append (o, r ? r : w);
            g_free (w);
        } else if (g_ascii_isdigit (*p) && (p == text || !fx_is_ident (p[-1]))) {
            /* a number: its suffix letters are no identifier */
            while (g_ascii_isalnum (*p) || *p == '.')
                g_string_append_c (o, *p++);
        } else {
            g_string_append_c (o, *p++);
        }
    }
    return g_string_free (o, FALSE);
}

static int fxf_count_ident (const char *text, const char *name)
{
    return fxs_count_word (text, name);
}

/* span of "void main ( ) { ... }" in text: body = inside the braces */
static gboolean fxf_main_span (const char *text, const char **head, const char **body, const char **end)
{
    const char *m = NULL;
    for (const char *p = text; (p = strstr (p, "main")) != NULL; p += 4) {
        if ((p > text && fx_is_ident (p[-1])) || fx_is_ident (p[4]))
            continue;
        const char *q = p;
        while (q > text && g_ascii_isspace (q[-1]))
            q--;
        if (q - text >= 4 && !strncmp (q - 4, "void", 4))
            m = q - 4;
    }
    const char *b = m ? strchr (m, '{') : NULL;
    if (!b)
        return FALSE;
    int depth = 0;
    for (const char *p = b; *p; p++) {
        if (*p == '{')
            depth++;
        else if (*p == '}' && --depth == 0) {
            *head = m;
            *body = b + 1;
            *end = p;
            return TRUE;
        }
    }
    return FALSE;
}

/* p behind the tokens of `toks` (a NULL-terminated list), white space only
 * between them; NULL when the text at p is something else */
static const char *fxf_past_tokens (const char *p, const char *const *toks)
{
    for (; *toks; toks++) {
        gsize l = strlen (*toks);
        while (g_ascii_isspace (*p))
            p++;
        if (strncmp (p, *toks, l) || (fx_is_ident ((*toks)[0]) && fx_is_ident (p[l])))
            return NULL;
        p += l;
    }
    return p;
}

/* The fragment source of a pure displacement, as a function
 * "vec2 _wf<i>(vec2 _wpx)" that returns the coordinate the effect reads
 * its input at when its varyings are taken _wpx pixels away from the
 * fragment. NULL when the source is not such an effect. */
static char *fxf_warp_function (const char *text, GArray *globals, int idx)
{
    const char *head = NULL, *body = NULL, *end = NULL;
    if (!fxf_main_span (text, &head, &body, &end))
        return NULL;
    /* g_Texture0 and gl_FragColor: once each, in the last statement */
    if (fxf_count_ident (text, "gl_FragColor") != 1 || fxf_count_ident (text, "discard") ||
        fxf_count_ident (text, "clip") || fxf_count_ident (text, "dFdx") || fxf_count_ident (text, "dFdy") ||
        fxf_count_ident (text, "fwidth") || fxf_count_ident (text, "gl_FragCoord"))
        return NULL;
    char *b = g_strndup (body, (gsize) (end - body));
    if (fxf_count_ident (b, "return")) {
        g_free (b);
        return NULL;
    }
    char *fc = strstr (b, "gl_FragColor");
    gboolean ok = fc != NULL;
    char *expr = NULL;
    if (ok) {
        /* "gl_FragColor = texSample2D ( g_Texture0 , E ) ;" then nothing */
        GString *s = g_string_new (NULL);
        for (const char *p = fc; *p; p++)
            if (!g_ascii_isspace (*p))
                g_string_append_c (s, *p);
        static const char *pre = "gl_FragColor=texSample2D(g_Texture0,";
        gsize n = s->len;
        ok = g_str_has_prefix (s->str, pre) && n > strlen (pre) + 2 && s->str[n - 1] == ';' &&
             s->str[n - 2] == ')';
        if (ok) {
            /* the ')' in front of the ';' closes texSample2D( */
            int depth = 0;
            const char *open = strchr (s->str, '(');
            for (const char *p = open; *p && ok; p++) {
                if (*p == '(')
                    depth++;
                else if (*p == ')' && --depth == 0)
                    ok = p == s->str + n - 2;
                else if (*p == ';' && depth > 0)
                    ok = FALSE;
            }
        }
        g_string_free (s, TRUE);
        if (ok) {
            /* The comparison above ignores white space, also inside a name:
             * "g_Tex ture0" (two macros, or a comment between them) passes
             * it and is not this statement. The coordinate starts behind
             * the tokens as they stand in the text. */
            static const char *const toks[] = { "gl_FragColor", "=", "texSample2D", "(", "g_Texture0", ",", NULL };
            const char *arg = fxf_past_tokens (fc, toks);
            const char *close = strrchr (fc, ';');
            ok = arg != NULL;
            if (ok) {
                while (close > arg && *close != ')')
                    close--;
                expr = g_strndup (arg, (gsize) (close - arg));
            }
        }
    }
    /* the input is read nowhere else */
    char *before = ok ? g_strndup (b, (gsize) (fc - b)) : NULL;
    if (ok) {
        char *head_text = g_strndup (text, (gsize) (head - text));
        ok = fxf_count_ident (before, "g_Texture0") == 0 && fxf_count_ident (end + 1, "g_Texture0") == 0 &&
             fxf_count_ident (head_text, "g_Texture0") == 1;   /* its declaration */
        g_free (head_text);
    }
    GString *o = NULL;
    if (ok) {
        o = g_string_new_len (text, head - text);
        g_string_append_printf (o, "vec2 _wf%d(vec2 _wpx) {\n", idx);
        GHashTable *vmap = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
        for (guint i = 0; i < globals->len && ok; i++) {
            FxfDecl *d = &g_array_index (globals, FxfDecl, i);
            if (d->cls != 'v')
                continue;
            gsize l = 0;
            /* varyings are read in main only, floats and float vectors */
            char *ht = g_strndup (text, (gsize) (head - text));
            char *outside = g_strconcat (ht, end + 1, NULL);
            int ty = d->type ? fxs_type_at (d->type, &l) : 0;
            ok = !d->array && ty >= FXS_T_FLOAT && ty <= FXS_T_VEC4 && fxf_count_ident (outside, d->name) == 1;
            g_free (ht);
            g_free (outside);
            if (!ok)
                break;
            g_string_append_printf (o, "\t%s %s__q = %s + dFdx(%s) * _wpx.x + dFdy(%s) * _wpx.y;\n", d->type,
                                    d->name, d->name, d->name, d->name);
            g_hash_table_insert (vmap, d->name, g_strdup_printf ("%s__q", d->name));
        }
        if (ok) {
            char *b2 = fxf_rename (before, vmap), *e2 = fxf_rename (expr, vmap);
            g_string_append (o, b2);
            g_string_append_printf (o, "\treturn (%s);\n}\n", e2);
            g_string_append (o, end + 1);
            g_free (b2);
            g_free (e2);
        }
        g_hash_table_unref (vmap);
    }
    g_free (before);
    g_free (expr);
    g_free (b);
    if (!ok) {
        if (o)
            g_string_free (o, TRUE);
        return NULL;
    }
    return g_string_free (o, FALSE);
}

/* member `idx` of a group, one stage: resolved, turned into functions,
 * names suffixed, samplers on their fused units, declarations of shared
 * uniforms / attributes dropped when an earlier member made them */
static char *fxf_member_text (const char *src, gboolean vertex, int idx, const int unit_of[FX_MAX_BINDS],
                              GHashTable *declared)
{
    char *text = fxf_resolve (src, vertex);
    if (!text)
        return NULL;
    GArray *gl = fxf_globals (text);
    char *fn = NULL;
    if (vertex) {
        /* main -> _wv<idx> through the rename map */
        const char *h, *b, *e;
        if (fxf_main_span (text, &h, &b, &e))
            fn = g_strdup (text);
    } else {
        fn = fxf_warp_function (text, gl, idx);
    }
    g_free (text);
    if (!fn) {
        g_array_unref (gl);
        return NULL;
    }
    GHashTable *map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    for (guint i = 0; i < gl->len; i++) {
        FxfDecl *d = &g_array_index (gl, FxfDecl, i);
        if (d->cls == 'a' || !strcmp (d->name, "main") || g_hash_table_contains (map, d->name))
            continue;
        if (d->cls == 'u') {
            /* engine uniforms keep their names; g_Texture<k>* moves to the
             * member's unit (or out of the way when it has none) */
            if (!strncmp (d->name, "g_Texture", 9) && d->name[9] >= '1' && d->name[9] <= '7') {
                int k = d->name[9] - '0';
                if (unit_of[k] > 0)
                    g_hash_table_insert (map, g_strdup (d->name),
                                         g_strdup_printf ("g_Texture%d%s", unit_of[k], d->name + 10));
                else
                    g_hash_table_insert (map, g_strdup (d->name),
                                         g_strdup_printf ("_we_nt%d_%s", idx, d->name + 9));
            }
            continue;
        }
        g_hash_table_insert (map, g_strdup (d->name), g_strdup_printf ("%s_f%d", d->name, idx));
    }
    if (vertex)
        g_hash_table_insert (map, g_strdup ("main"), g_strdup_printf ("_wv%d", idx));
    if (idx > 1)
        /* behind the first member the input is a render target: unpadded */
        g_hash_table_insert (map, g_strdup ("g_Texture0Resolution"),
                             g_strdup ("vec4(g_Texture0Resolution.zw, g_Texture0Resolution.zw)"));
    char *ren = fxf_rename (fn, map);
    g_free (fn);
    g_hash_table_unref (map);
    g_array_unref (gl);
    /* one declaration per shared uniform / attribute */
    GString *o = g_string_new (NULL);
    for (const char *p = ren; *p;) {
        const char *eol = strchr (p, '\n');
        gsize n = eol ? (gsize) (eol - p) + 1 : strlen (p);
        const char *s = fx_sp (p);
        gsize l = 0;
        gboolean drop = FALSE;
        if ((fxs_word_at (s, "uniform", &l) || fxs_word_at (s, "attribute", &l)) && memchr (p, ';', n) &&
            !(idx > 1 && g_strstr_len (p, (gssize) n, "vec4(g_Texture0Resolution.zw"))) {
            const char *semi = memchr (p, ';', n), *e = semi;
            const char *br = memchr (p, '[', (gsize) (semi - p));
            if (br)
                e = br;
            while (e > p && g_ascii_isspace (e[-1]))
                e--;
            const char *b = e;
            while (b > p && fx_is_ident (b[-1]))
                b--;
            char *name = g_strndup (b, (gsize) (e - b));
            if (g_hash_table_contains (declared, name)) {
                drop = TRUE;
                g_free (name);
            } else {
                g_hash_table_add (declared, name);
            }
        } else if (idx > 1 && fxs_word_at (s, "uniform", &l) &&
                   g_strstr_len (p, (gssize) n, "vec4(g_Texture0Resolution.zw")) {
            /* the renamed declaration of g_Texture0Resolution itself */
            drop = TRUE;
            if (!g_hash_table_contains (declared, "g_Texture0Resolution")) {
                g_string_append (o, "uniform vec4 g_Texture0Resolution;\n");
                g_hash_table_add (declared, g_strdup ("g_Texture0Resolution"));
            }
        }
        if (!drop)
            g_string_append_len (o, p, (gssize) n);
        p += n;
    }
    g_free (ren);
    return g_string_free (o, FALSE);
}

/* ---- which chains */

/* structure only: one drawn pass into the chain output, specialised with
 * every material constant baked, plain texture slots */
static gboolean fxf_candidate (EffectChain *ch)
{
    if (!ch || !ch->ready || ch->n_passes != 1 || ch->n_targets != 0 || ch->vertex_affine)
        return FALSE;
    EffPass *p = &ch->passes[0];
    if (p->is_copy || p->target[0] || p->moves_verts || p->xf_sign || !p->spec || !p->spec->prog ||
        !p->spec->src || !p->spec->n_baked)
        return FALSE;
    for (int i = 0; i < p->spec->n; i++)
        if (!p->spec->c[i].baked && !(p->spec->c[i].hazard && !p->spec->c[i].dynamic))
            return FALSE;
    for (int k = 0; k < p->n_consts; k++)
        if (p->consts[k].loc >= 0 && !(p->spec->hz && k < p->spec->n_l && p->spec->hz[k]))
            return FALSE;       /* a constant the scan did not see */
    for (int u = 0; u < FX_MAX_BINDS; u++)
        if (p->sys[u] || p->slot[u].rt != FX_RT_NONE)
            return FALSE;
    for (int b = 0; b < p->n_binds; b++)
        if (p->binds[b].index != 0 || strcmp (p->binds[b].name, "previous"))
            return FALSE;
    /* time-dependent only: a static chain belongs to the static caches */
    return !fx_chain_static (ch);
}

static void fxf_group_free (FxFuse *g)
{
    if (g->chain) {
        /* the textures in its slots are the members' (fxf_build_group
         * copies the slots without taking a reference) */
        for (int u = 0; u < FX_MAX_BINDS; u++)
            g->chain->passes[0].slot[u].tex = 0;
        fx_chain_free (g->chain);
    }
    g->chain = NULL;
}

/* build the fused pass of chains idx[0..n-1]; FALSE = not possible */
static gboolean fxf_build_group (struct FxSet *set, const int *idx, int n, FxFuse *g, const char *dbgname)
{
    GString *vs = g_string_new ("#version 330\n"), *fs = g_string_new ("#version 330\n");
    g_string_append (vs, fx_prelude_common);
    g_string_append (vs, "#define varying out\n#define attribute in\n");
    g_string_append (fs, fx_prelude_common);
    g_string_append (fs, "#define varying in\nout vec4 _we_fragColor;\n#define gl_FragColor _we_fragColor\n");
    GHashTable *dv = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *df = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    EffectChain *fc = fx_chain_new (1);
    EffPass *fp = &fc->passes[0];
    int next_unit = 1;
    gboolean ok = TRUE;
    for (int m = 0; m < n && ok; m++) {
        EffPass *p = &set->chains[idx[m]]->passes[0];
        int unit_of[FX_MAX_BINDS] = { 0 };
        for (int u = 0; u < p->n_dyn && ok; u++) {
            int k = p->dyn[u].unit;
            if ((p->dyn[u].kind != FXU_TEXTURE && p->dyn[u].kind != FXU_RESOLUTION) || k < 1 ||
                k >= FX_MAX_BINDS || unit_of[k])
                continue;
            if (next_unit >= FX_MAX_BINDS) {
                ok = FALSE;
                break;
            }
            unit_of[k] = next_unit;
            fp->slot[next_unit] = p->slot[k];
            next_unit++;
        }
        char *v = NULL, *f = NULL;
        ok = ok && fxf_final_sources (p, &v, &f);
        char *tv = ok ? fxf_member_text (v, TRUE, m + 1, unit_of, dv) : NULL;
        char *tf = ok ? fxf_member_text (f, FALSE, m + 1, unit_of, df) : NULL;
        ok = tv && tf;
        if (ok) {
            g_string_append (vs, tv);
            g_string_append (fs, tf);
        }
        g_free (tv);
        g_free (tf);
        g_free (v);
        g_free (f);
    }
    g_hash_table_unref (dv);
    g_hash_table_unref (df);
    if (ok) {
        g_string_append (vs, "out vec2 _we_fuv;\nvoid main() {\n");
        for (int m = 1; m <= n; m++)
            g_string_append_printf (vs, "\t_wv%d();\n", m);
        g_string_append (vs, "\t_we_fuv = a_TexCoord;\n}\n");
        /* the last effect first; between two effects the coordinate is
         * clamped as the target in between clamps it */
        g_string_append (fs, "in vec2 _we_fuv;\nvoid main() {\n"
                             "\tvec2 _wd = vec2(dFdx(_we_fuv.x), dFdy(_we_fuv.y));\n"
                             "\tvec2 _wh = abs(_wd) * 0.5;\n\tvec2 _wq = _we_fuv;\n");
        for (int m = n; m >= 1; m--) {
            g_string_append_printf (fs, "\t_wq = _wf%d((_wq - _we_fuv) / _wd);\n", m);
            if (m > 1)
                g_string_append (fs, "\t_wq = clamp(_wq, _wh, vec2(1.0) - _wh);\n");
        }
        g_string_append (fs, "\tgl_FragColor = texSample2D(g_Texture0, _wq);\n}\n");
        guint64 key[2];
        FxsProg *e = fxs_prog_lookup (vs->str, fs->str, key);
        if (!e) {
            fxs_dump ("fused", vs->str, fs->str);
            GLuint prog = fxs_link_quiet (vs->str, fs->str, "fused displacement run");
            if (prog) {
                e = fxs_prog_add (key, prog);
                g_atomic_int_inc (&fxs_n_compiled);
            }
        }
        ok = e != NULL;
        if (ok) {
            e->refs++;
            fp->spec = g_new0 (struct FxSpec, 1);
            fp->spec->prog = e;
            fp->program = e->program;
            if (fxs_cost_on ()) {
                char *nm = g_strdup_printf ("fused x%d", n);
                fxs_cost_note_build (fp->spec, fp, nm, dbgname, vs->str, fs->str, FALSE);
                g_free (nm);
            }
            fx_cache_uniforms (fp);
            fxs_after_link (fp);
            fx_vertex_setup (fp);
            fp->moves_verts = FALSE;
            fc->n_passes = 1;
            fc->ready = TRUE;
            fc->enabled = TRUE;
            fc->cacheable = -1;
            fc->feedback = -1;
            fc->dyn = -1;
            fc->alt_out = -1;           /* one pass: no second output */
            fc->inst_index = set->chains[idx[0]]->inst_index;
            /* GPUPROF: one profiler row for the run, under its first effect's index */
            g_snprintf (fc->prof_name, sizeof (fc->prof_name), "fused x%d", n);
            g_strlcpy (fc->name, "fused", sizeof (fc->name));
            if (fxs_cost_on ())
                fxs_chain_built (fc, "(fused displacement run)");
        }
    }
    g_string_free (vs, TRUE);
    g_string_free (fs, TRUE);
    if (!ok) {
        g_free (fc);
        return FALSE;
    }
    g->chain = fc;
    g->n = n;
    g->first = idx[0];
    g->last = idx[n - 1];
    for (int m = 0; m < n; m++) {
        g->member[m] = idx[m];
        g->member_prog[m] = set->chains[idx[m]]->passes[0].spec->prog;
    }
    g_atomic_int_inc (&fxf_n_groups);
    g_atomic_int_add (&fxf_n_members, n);
    return TRUE;
}

/* fx_build_for_object: look for runs of displacement chains */
static void fxf_build (struct FxSet *set, const char *dbgname)
{
    if (!set || !fxs_fast () || set->n < 2)
        return;
    if (!set->fuse) {
        set->fuse = g_new0 (struct FxFuseSet, 1);
        set->fuse->name = g_strdup (dbgname);
    }
    GArray *groups = g_array_new (FALSE, TRUE, sizeof (FxFuse));
    for (int i = 0; i < set->n;) {
        /* a run: candidates next to each other (chains that failed to
         * build or were filtered out end it) */
        int run[FXF_MAX], n = 0, j = i;
        while (j < set->n && n < FXF_MAX && fxf_candidate (set->chains[j]))
            run[n++] = j++;
        gboolean built = FALSE;
        /* the longest prefix of the run that builds (texture units, or a
         * member that is no pure displacement, end it) */
        for (int len = n; len >= 2 && !built; len--) {
            FxFuse g = { 0 };
            if (fxf_build_group (set, run, len, &g, dbgname)) {
                g_array_append_val (groups, g);
                built = TRUE;
                i = run[len - 1] + 1;
            }
        }
        if (!built)
            i = n > 0 ? run[0] + 1 : j + 1;
    }
    if (groups->len) {
        set->fuse->n = (int) groups->len;
        set->fuse->g = (FxFuse *) g_array_free (groups, FALSE);
        g_debug ("fx: %s: %d run(s) of displacement effects fused", dbgname, set->fuse->n);
    } else {
        g_array_free (groups, TRUE);
    }
}

static void fxf_free (struct FxSet *set)
{
    if (!set->fuse)
        return;
    for (int i = 0; i < set->fuse->n; i++)
        fxf_group_free (&set->fuse->g[i]);
    g_free (set->fuse->g);
    g_free (set->fuse->name);
    g_clear_pointer (&set->fuse, g_free);
}

/* fx_run_for_object, in front of its chain loop: passes of this set
 * changed programs in the last run (a specialised program took over
 * behind its first draw, a script wrote a baked constant) - the runs that
 * can be fused are other ones now */
static inline void fxf_refresh (struct FxSet *set)
{
    set->fxs_mark = fxs_switches;
    if (G_LIKELY (!set->fxs_stale))
        return;
    set->fxs_stale = FALSE;
    if (!set->fuse)
        return;     /* not fast, or a single effect */
    char *name = g_strdup (set->fuse->name);
    int before = set->fuse->n;
    /* a fused pass may own the target of a kept result (a held set, a
     * held prefix): it goes with the pass */
    set->cached = FALSE;
    set->held = FALSE;
    set->hpfx.valid = FALSE;
    fxf_free (set);
    fxf_build (set, name);
    g_free (name);
    if (before || (set->fuse && set->fuse->n))
        fxs_picture_changed ();     /* fused and one-by-one are not the same pixels */
}

/* ... and behind the loop */
static inline void fxf_settle (struct FxSet *set)
{
    if (G_LIKELY (set->fxs_mark == fxs_switches))
        return;
    set->cacheable = -1;    /* fx_is_static: the chains are asked again */
    set->fxs_stale = TRUE;
}

/* fx_run_for_object, at chain i when no fused pass runs there: a fused
 * pass that starts at i (a member is off, was relinked or written to)
 * keeps no target while its members run one by one */
static inline void fxf_idle (struct FxSet *set, int i)
{
    for (int k = 0; k < set->fuse->n; k++) {
        FxFuse *g = &set->fuse->g[k];
        if (g->first == i && g->chain && g->chain->out_tex)
            fx_chain_drop_targets (g->chain, 0);
    }
}

/* fx_free_targets_for_object: the scene is paused */
static void fxf_drop_targets (struct FxSet *set)
{
    for (int i = 0; set->fuse && i < set->fuse->n; i++)
        if (set->fuse->g[i].chain)
            fx_chain_drop_targets (set->fuse->g[i].chain, 0);
}

/* fx_run_for_object, at chain i: the fused chain to run instead of chains
 * i..*last, or NULL. A group is used only while every member is as it
 * was fused: enabled, the same program, no constant written. */
static inline EffectChain *fxf_at (struct FxSet *set, int i, int *last)
{
    if (G_LIKELY (!set->fuse))
        return NULL;
    for (int k = 0; k < set->fuse->n; k++) {
        FxFuse *g = &set->fuse->g[k];
        if (g->first != i || !g->chain)
            continue;
        for (int m = 0; m < g->n; m++) {
            EffectChain *c = set->chains[g->member[m]];
            EffPass *p = &c->passes[0];
            if (!c->enabled || !p->spec || p->spec->prog != g->member_prog[m])
                return NULL;
            for (int q = 0; q < p->n_consts; q++)
                if (p->consts[q].dirty)
                    return NULL;
        }
        /* nothing between the members may run either */
        for (int j = g->first; j <= g->last; j++) {
            gboolean member = FALSE;
            for (int m = 0; m < g->n; m++)
                member |= g->member[m] == j;
            if (!member && set->chains[j] && set->chains[j]->enabled)
                return NULL;
        }
        *last = g->last;
        return g->chain;
    }
    return NULL;
}
