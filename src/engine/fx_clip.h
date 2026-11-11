/* fx_clip.h - "Skip hidden areas" inside a layer's effect chains.
 *
 * Included by scenenode_effects.c (after EffPass / EffectChain and
 * fx_find_target, fx_active lines of fx_srcrepair.h): everything here is
 * static and reachable from tests/clip_test.c through that include.
 *
 * The caller knows which part of a layer's final picture can be seen
 * (fx_set_clip_uv: a rectangle of the output in texture coordinates). An
 * effect pass then only has to produce the pixels something later reads.
 * That is decided backwards through the run, per render target:
 *
 *   the last pass must fill the rectangle that is seen;
 *   a pass that fills rectangle R needs from each texture it reads
 *     - R grown by two texels when every lookup of that texture is made
 *       at the fragment's own texture coordinate (a "local" lookup: the
 *       two texels are what a linear filter reads next to it), or
 *     - the WHOLE texture otherwise.
 *
 * "Otherwise" is every lookup whose coordinate is computed (blur taps,
 * water / shake / scroll displacement, mirror, radial steps): how far such
 * a lookup reaches cannot be read off the shader in general, so nothing
 * is assumed and the passes before it draw their whole target. The result
 * is the same picture, pixel for pixel, inside the rectangle that is
 * seen; what is saved is exactly what can be proven unused.
 *
 * Whether a lookup is local is read from the compiled part of the shader
 * source (fx_clip_traits), conservatively: anything not recognised counts
 * as "otherwise". No wallpaper, effect or shader names are involved. */

/* ---- tokens of the compiled code ----------------------------------- */

typedef struct { const char *s; int n; } FxTok;

/* the compiled part of a built source (dead #if branches, directives and
 * comments removed) as tokens: identifiers / numbers, and every other
 * character on its own. *keep = the string the tokens point into. */
static GArray *fx_clip_tokens (const char *src, char **keep)
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
            g_string_append (o, d);
            g_string_append_c (o, ' ');
        }
        g_free (code);
    }
    fx_an_clear (&an);
    g_strfreev (lines);
    *keep = g_string_free (o, FALSE);
    GArray *t = g_array_new (FALSE, FALSE, sizeof (FxTok));
    for (const char *p = *keep; *p;) {
        if (g_ascii_isspace (*p)) {
            p++;
            continue;
        }
        FxTok k = { p, 1 };
        if (fx_is_ident (*p)) {
            while (fx_is_ident (p[k.n]))
                k.n++;
        }
        g_array_append_val (t, k);
        p += k.n;
    }
    return t;
}

static gboolean fx_tok_is (const GArray *t, int i, const char *w)
{
    if (i < 0 || i >= (int) t->len)
        return FALSE;
    const FxTok *k = &g_array_index (t, FxTok, i);
    return (int) strlen (w) == k->n && !strncmp (k->s, w, (size_t) k->n);
}

static gboolean fx_tok_in (const GArray *t, int i, const char *const *words)
{
    for (; *words; words++)
        if (fx_tok_is (t, i, *words))
            return TRUE;
    return FALSE;
}

static const char *const fx_clip_types[] = { "vec2", "vec3", "vec4", "float", "float2", "float3",
                                             "float4", NULL };
static const char *const fx_clip_lookups[] = { "texSample2D", "texture2D", "texture",
                                               "texSample2DLod", "texture2DLod", "textureLod",
                                               NULL };

/* text of the #define lines of the shader body (after the prelude's
 * "#line 1"), whatever #if they stand in: what a macro may hide */
static char *fx_clip_macros (const char *src)
{
    const char *body = strstr (src, "#line 1\n");
    GString *o = g_string_new (" ");
    if (!body)
        return g_string_free (o, FALSE);
    char **lines = g_strsplit (body, "\n", -1);
    gboolean blk = FALSE, cont = FALSE;
    for (char **l = lines; *l; l++) {
        char *code = fx_code_view (*l, &blk);
        const char *d = fx_sp (code);
        gboolean def = cont;
        if (*d == '#') {
            const char *w = fx_sp (d + 1);
            def = !strncmp (w, "define", 6) && !fx_is_ident (w[6]);
        }
        if (def) {
            g_string_append (o, d);
            g_string_append_c (o, ' ');
            gsize n = strlen (g_strchomp (code));
            cont = n > 0 && code[n - 1] == '\\';
        } else {
            cont = FALSE;
        }
        g_free (code);
    }
    g_strfreev (lines);
    return g_string_free (o, FALSE);
}

/* `word` stands as an identifier of its own somewhere in text */
static gboolean fx_clip_has_word (const char *text, const char *word)
{
    gsize n = strlen (word);
    for (const char *p = text; (p = strstr (p, word)) != NULL; p += n)
        if ((p == text || !fx_is_ident (p[-1])) && !fx_is_ident (p[n]))
            return TRUE;
    return FALSE;
}

/* Does the vertex shader hand the quad's texture coordinate through in
 * the x and y of varying `v`, untouched? Every write to it must be one of
 *   v = a_TexCoord;  v = a_TexCoord.xy;  v = a_TexCoord.xyxy;
 *   v.xy = a_TexCoord;  v.xy = a_TexCoord.xy;
 * or leave x and y alone (v.zw = ..., v.z, v.w), and there must be one. */
static gboolean fx_clip_identity_varying (const GArray *t, const char *v)
{
    static const char *const zw[] = { "zw", "z", "w", NULL };
    int writes = 0, decls = 0;
    for (int i = 0; i < (int) t->len; i++) {
        if (!fx_tok_is (t, i, v))
            continue;
        if (fx_tok_in (t, i - 1, fx_clip_types)) {
            if (++decls > 1)
                return FALSE;           /* a local of the same name hides it */
            continue;
        }
        int j = i + 1;
        gboolean swz = FALSE, keeps_xy = FALSE, is_xy = FALSE;
        if (fx_tok_is (t, j, "[") || fx_tok_is (t, i - 1, "."))
            return FALSE;               /* an array, or a member of that name */
        if (fx_tok_is (t, j, ".")) {
            swz = TRUE;
            keeps_xy = fx_tok_in (t, j + 1, zw);
            is_xy = fx_tok_is (t, j + 1, "xy");
            j += 2;
        }
        gboolean assign = fx_tok_is (t, j, "=") && !fx_tok_is (t, j + 1, "=");
        gboolean compound = (fx_tok_is (t, j, "+") || fx_tok_is (t, j, "-") ||
                             fx_tok_is (t, j, "*") || fx_tok_is (t, j, "/")) &&
                            (fx_tok_is (t, j + 1, "=") || fx_tok_is (t, j + 1, "+") ||
                             fx_tok_is (t, j + 1, "-"));
        /* "a + -b" also matches `compound`: it is a read then, unless the
         * token after the pair starts no operand - keep it simple and
         * strict: "+=", "-=", "*=", "/=", "++", "--" only */
        if (compound && !fx_tok_is (t, j + 1, "=") &&
            !((fx_tok_is (t, j, "+") && fx_tok_is (t, j + 1, "+")) ||
              (fx_tok_is (t, j, "-") && fx_tok_is (t, j + 1, "-"))))
            compound = FALSE;
        if (assign) {
            if (keeps_xy)
                continue;
            if (swz && !is_xy)
                return FALSE;           /* .x, .y, .xyz, ...: x or y rewritten */
            int k = j + 1;
            if (!fx_tok_is (t, k, "a_TexCoord"))
                return FALSE;
            k++;
            if (fx_tok_is (t, k, ".")) {
                if (!(fx_tok_is (t, k + 1, "xy") || (!swz && fx_tok_is (t, k + 1, "xyxy"))))
                    return FALSE;
                k += 2;
            }
            if (!fx_tok_is (t, k, ";"))
                return FALSE;
            writes++;
        } else if (compound) {
            if (!keeps_xy)
                return FALSE;
        } else if (!swz && (fx_tok_is (t, i - 1, "(") || fx_tok_is (t, i - 1, ",")) &&
                   (fx_tok_is (t, j, ",") || fx_tok_is (t, j, ")"))) {
            return FALSE;               /* handed to a call: may be an out parameter */
        } else if (fx_tok_is (t, i - 1, "+") && fx_tok_is (t, i - 2, "+")) {
            return FALSE;               /* ++v */
        } else if (fx_tok_is (t, i - 1, "-") && fx_tok_is (t, i - 2, "-")) {
            return FALSE;
        }
    }
    return writes > 0 && decls == 1;
}

/* pass->local_units: bit u = every lookup the fragment shader makes
 * through g_Texture<u> is at the fragment's own texture coordinate. */
static void fx_clip_traits (EffPass *pass, const char *vsrc, const char *fsrc)
{
    pass->local_units = 0;
    if (pass->moves_verts)
        return;         /* the pass does not fill its target as a plain quad */
    char *vkeep = NULL, *fkeep = NULL;
    GArray *vt = fx_clip_tokens (vsrc, &vkeep), *ft = fx_clip_tokens (fsrc, &fkeep);
    char *vmac = fx_clip_macros (vsrc), *fmac = fx_clip_macros (fsrc);
    gboolean opaque = FALSE;
    /* a macro of the shader itself may hide a lookup or a write */
    for (const char *const *w = fx_clip_lookups; *w && !opaque; w++)
        opaque = fx_clip_has_word (fmac, *w) || fx_clip_has_word (vmac, *w);
    opaque = opaque || strstr (fmac, "g_Texture") || strstr (vmac, "g_Texture") ||
             strstr (fmac, "sampler") || strstr (vmac, "sampler");
    /* samplers: the fragment shader's g_Texture0..7 only; none read by
     * the vertex shader */
    for (int i = 0; i < (int) vt->len && !opaque; i++) {
        const FxTok *k = &g_array_index (vt, FxTok, i);
        if ((k->n == 10 && !strncmp (k->s, "g_Texture", 9) && g_ascii_isdigit (k->s[9])) ||
            (k->n >= 7 && !strncmp (k->s, "sampler", 7)))
            opaque = TRUE;
    }
    for (int i = 0; i < (int) ft->len && !opaque; i++) {
        const FxTok *k = &g_array_index (ft, FxTok, i);
        if (k->n >= 7 && !strncmp (k->s, "sampler", 7)) {
            const FxTok *nm = i + 1 < (int) ft->len ? &g_array_index (ft, FxTok, i + 1) : NULL;
            if (!(k->n == 9 && !strncmp (k->s, "sampler2D", 9) && nm && nm->n == 10 &&
                  !strncmp (nm->s, "g_Texture", 9) && nm->s[9] >= '0' && nm->s[9] <= '7'))
                opaque = TRUE;
        }
    }
    guint local = 0, seen = 0, bad = 0;
    /* identity verdict per coordinate varying, looked up once */
    GHashTable *ident = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (int i = 0; i < (int) ft->len && !opaque; i++) {
        const FxTok *k = &g_array_index (ft, FxTok, i);
        if (!(k->n == 10 && !strncmp (k->s, "g_Texture", 9) && k->s[9] >= '0' && k->s[9] <= '7'))
            continue;
        guint bit = 1u << (k->s[9] - '0');
        if (fx_tok_is (ft, i - 1, "sampler2D"))
            continue;                               /* the declaration */
        seen |= bit;
        if (!(fx_tok_is (ft, i - 1, "(") && fx_tok_in (ft, i - 2, fx_clip_lookups) &&
              fx_tok_is (ft, i + 1, ","))) {
            bad |= bit;                             /* handed on, sized, fetched ... */
            continue;
        }
        /* second argument: a varying, or its .xy, and nothing else */
        int a = i + 2;
        const FxTok *v = a < (int) ft->len ? &g_array_index (ft, FxTok, a) : NULL;
        int e = a + 1;
        if (fx_tok_is (ft, e, ".") && fx_tok_is (ft, e + 1, "xy"))
            e += 2;
        if (!v || !fx_is_ident_start (v->s[0]) ||
            !(fx_tok_is (ft, e, ")") || fx_tok_is (ft, e, ","))) {
            bad |= bit;
            continue;
        }
        char *name = g_strndup (v->s, (gsize) v->n);
        gpointer verdict = NULL;
        if (!g_hash_table_lookup_extended (ident, name, NULL, &verdict)) {
            /* in the fragment shader: declared exactly once (no local of
             * the same name), never written; in the vertex shader: the
             * texture coordinate handed through; in no macro */
            int decls = 0;
            gboolean ok = TRUE;
            for (int q = 0; q < (int) ft->len && ok; q++) {
                if (!fx_tok_is (ft, q, name))
                    continue;
                if (fx_tok_in (ft, q - 1, fx_clip_types)) {
                    decls++;
                    continue;
                }
                int j = q + 1;
                if (fx_tok_is (ft, j, "."))
                    j += 2;
                if ((fx_tok_is (ft, j, "=") && !fx_tok_is (ft, j + 1, "=")) ||
                    ((fx_tok_is (ft, j, "+") || fx_tok_is (ft, j, "-") || fx_tok_is (ft, j, "*") ||
                      fx_tok_is (ft, j, "/")) && fx_tok_is (ft, j + 1, "=")) ||
                    fx_tok_is (ft, j, "["))
                    ok = FALSE;
            }
            ok = ok && decls == 1 && !fx_clip_has_word (fmac, name) &&
                 !fx_clip_has_word (vmac, name) && fx_clip_identity_varying (vt, name);
            verdict = GINT_TO_POINTER (ok ? 1 : 2);
            g_hash_table_insert (ident, g_strdup (name), verdict);
        }
        g_free (name);
        if (GPOINTER_TO_INT (verdict) != 1)
            bad |= bit;
    }
    g_hash_table_unref (ident);
    if (!opaque)
        local = seen & ~bad;
    /* a sampler the compiled code never names is never read */
    pass->local_units = opaque ? 0 : (local | (~seen & 0xffu));
    g_array_unref (vt);
    g_array_unref (ft);
    g_free (vkeep);
    g_free (fkeep);
    g_free (vmac);
    g_free (fmac);
}

/* ---- what each pass of a run has to fill --------------------------- */

static void fx_need_add (FxClipNeed *n, const FxClipNeed *a)
{
    if (a->mode == FXN_NONE || n->mode == FXN_FULL)
        return;
    if (a->mode == FXN_FULL || n->mode == FXN_NONE) {
        *n = *a;
        return;
    }
    for (int i = 0; i < a->n; i++) {
        if (n->n < FXN_MAX) {
            memcpy (n->r[n->n++], a->r[i], 4 * sizeof (float));
            continue;
        }
        /* no room: everything becomes one rectangle around all of it */
        for (int k = 1; k < n->n; k++) {
            n->r[0][0] = fminf (n->r[0][0], n->r[k][0]);
            n->r[0][1] = fminf (n->r[0][1], n->r[k][1]);
            n->r[0][2] = fmaxf (n->r[0][2], n->r[k][2]);
            n->r[0][3] = fmaxf (n->r[0][3], n->r[k][3]);
        }
        n->n = 1;
        n->r[0][0] = fminf (n->r[0][0], a->r[i][0]);
        n->r[0][1] = fminf (n->r[0][1], a->r[i][1]);
        n->r[0][2] = fmaxf (n->r[0][2], a->r[i][2]);
        n->r[0][3] = fmaxf (n->r[0][3], a->r[i][3]);
    }
}

/* a local lookup of a w x h texture for the pixels of r: r and the
 * texels a linear filter reads around it */
static FxClipNeed fx_need_local (const FxClipNeed *r, int w, int h)
{
    FxClipNeed n = *r;
    if (n.mode != FXN_RECT)
        return n;
    float du = 2.0f / (float) MAX (1, w), dv = 2.0f / (float) MAX (1, h);
    for (int i = 0; i < n.n; i++) {
        n.r[i][0] = fmaxf (0.0f, n.r[i][0] - du);
        n.r[i][1] = fmaxf (0.0f, n.r[i][1] - dv);
        n.r[i][2] = fminf (1.0f, n.r[i][2] + du);
        n.r[i][3] = fminf (1.0f, n.r[i][3] + dv);
    }
    return n;
}

/* the pixels of a w x h target a need covers, as rectangles x, y, width,
 * height that do not overlap (a blending pass must not meet a pixel
 * twice); returns their number, 0 = none */
static int fx_need_pixels (const FxClipNeed *n, int w, int h, int px[][4])
{
    if (n->mode == FXN_NONE)
        return 0;
    if (n->mode == FXN_FULL) {
        px[0][0] = px[0][1] = 0;
        px[0][2] = w;
        px[0][3] = h;
        return 1;
    }
    WalpeBox b[FXN_MAX];
    int k = 0;
    gboolean apart = TRUE;
    for (int i = 0; i < n->n; i++) {
        WalpeBox e = { MAX (0, (int) floorf (n->r[i][0] * (float) w)),
                       MAX (0, (int) floorf (n->r[i][1] * (float) h)),
                       MIN (w, (int) ceilf (n->r[i][2] * (float) w)),
                       MIN (h, (int) ceilf (n->r[i][3] * (float) h)) };
        if (walpe_box_empty (e))
            continue;
        for (int q = 0; q < k; q++)
            apart = apart && !walpe_box_overlaps (b[q], e);
        b[k++] = e;
    }
    if (!apart) {
        /* rectangles grown by a filter's texels meet at their corners:
         * the same pixels as boxes that do not overlap (the strips
         * around a window stay strips) */
        WalpeRegion r;
        walpe_region_init (&r);
        walpe_region_set_boxes (&r, b, k);
        k = walpe_region_simplify (&r, b, FXN_MAX, 0);
        walpe_region_clear (&r);
    }
    for (int i = 0; i < k; i++) {
        px[i][0] = b[i].x0;
        px[i][1] = b[i].y0;
        px[i][2] = b[i].x1 - b[i].x0;
        px[i][3] = b[i].y1 - b[i].y0;
    }
    return k;
}

enum { FXT_IN = -1, FXT_OUT = -2, FXT_STATIC = -3 };

static int fx_clip_target_index (EffectChain *ch, const char *name)
{
    for (int i = 0; i < ch->n_targets; i++)
        if (!strcmp (ch->targets[i].name, name))
            return i;
    return FXT_STATIC;
}

/* Plan one chain for a run whose result is needed in `out` (of the
 * texture fx_chain_run returns): sets pass->need of every pass and
 * returns what the chain needs of its input. rw x rh = the chain's
 * working size (its output target's). Mirrors fx_chain_run's rules for
 * which texture a sampler unit reads. */
static FxClipNeed fx_chain_plan (EffectChain *ch, FxClipNeed out, int rw, int rh)
{
    const FxClipNeed full = { FXN_FULL, 0, { { 0, 0, 1, 1 } } }, none = { FXN_NONE, 0, { { 0, 0, 0, 0 } } };
    FxClipNeed need_t[FX_MAX_TARGETS], need_out = none, need_in = none;
    int tgt[FX_MAX_PASSES], prev[FX_MAX_PASSES];
    gboolean runs[FX_MAX_PASSES];
    for (int i = 0; i < FX_MAX_TARGETS; i++)
        need_t[i] = none;
    /* forwards: where each pass draws and what "the previous output" is */
    int cur = FXT_IN;
    for (int k = 0; k < ch->n_passes; k++) {
        EffPass *p = &ch->passes[k];
        tgt[k] = p->target[0] ? fx_clip_target_index (ch, p->target) : FXT_OUT;
        prev[k] = cur;
        runs[k] = tgt[k] != FXT_STATIC;          /* a named target that does not exist */
        if (p->is_copy) {
            runs[k] = runs[k] && tgt[k] >= 0 && fx_clip_target_index (ch, p->copy_src) >= 0;
            continue;
        }
        if (runs[k])
            cur = tgt[k];
    }
    /* the texture the chain hands back */
    if (cur == FXT_IN)
        need_in = out;
    else if (cur == FXT_OUT)
        need_out = out;
    else
        need_t[cur] = out;
    /* backwards */
    for (int k = ch->n_passes - 1; k >= 0; k--) {
        EffPass *p = &ch->passes[k];
        p->need = full;
        if (!runs[k])
            continue;
        FxClipNeed *o = tgt[k] == FXT_OUT ? &need_out : &need_t[tgt[k]];
        FxClipNeed r = *o;
        *o = none;                  /* what was in the target before is overdrawn */
        p->need = r;
        if (r.mode == FXN_NONE)
            continue;
        if (p->is_copy) {
            int s = fx_clip_target_index (ch, p->copy_src);
            FxClipNeed a = fx_need_local (&r, MAX (1, rw / ch->targets[s].scale),
                                      MAX (1, rh / ch->targets[s].scale));
            fx_need_add (&need_t[s], &a);
            continue;
        }
        /* which texture each sampler unit reads (fx_chain_run) */
        int src[FX_MAX_BINDS];
        src[0] = prev[k];
        for (int u = 1; u < FX_MAX_BINDS; u++) {
            if (p->sys[u] || p->slot[u].tex)
                src[u] = FXT_STATIC;        /* album art, a file texture */
            else if (p->slot[u].rt)
                src[u] = FXT_IN;            /* the effect input / the frame under a
                                             * compose layer: complete (the caller
                                             * does not clip below a layer that
                                             * reads the frame) */
            else
                src[u] = FXT_STATIC;
        }
        for (int b = 0; b < p->n_binds; b++) {
            int idx = p->binds[b].index;
            if (idx < 0 || idx >= FX_MAX_BINDS)
                continue;
            if (!strcmp (p->binds[b].name, "previous")) {
                src[idx] = FXT_IN;
            } else {
                int t = fx_clip_target_index (ch, p->binds[b].name);
                if (t >= 0)
                    src[idx] = t;
            }
        }
        for (int d = 0; d < p->n_dyn; d++) {
            if (p->dyn[d].kind != FXU_TEXTURE)
                continue;
            int u = p->dyn[d].unit;
            if (u >= FX_MAX_BINDS || src[u] == FXT_STATIC)
                continue;
            gboolean local = (p->local_units >> u) & 1u;
            FxClipNeed a = full;
            if (local) {
                int sw = rw, sh = rh;
                if (src[u] >= 0) {
                    sw = MAX (1, rw / ch->targets[src[u]].scale);
                    sh = MAX (1, rh / ch->targets[src[u]].scale);
                }
                a = fx_need_local (&r, sw, sh);
            }
            FxClipNeed *n = src[u] == FXT_IN ? &need_in : src[u] == FXT_OUT ? &need_out
                                                                      : &need_t[src[u]];
            fx_need_add (n, &a);
        }
    }
    ch->planned = TRUE;
    return need_in;
}

/* nothing is clipped in this run of the chain */
static void fx_chain_unplan (EffectChain *ch)
{
    ch->planned = FALSE;
}
