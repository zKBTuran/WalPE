/* fx_region.h - effect passes restricted to the pixels something reads
 * later. Included by scenenode_effects.c behind
 * EffPass / EffectChain / struct FxSet, fx_find_target () and
 * fx_srcrepair.h (fx_active_code); everything here is static and reachable
 * from tests/fx_region_test.c through that include.
 *
 * Every effect pass used to shade its whole render target. Often only a
 * part of the final picture of a layer is ever sampled: a layer that
 * reaches beyond the view is sampled on its visible part only, a puppet
 * samples the texture only where its mesh has triangles (2901582518,
 * the main test scene: 16 passes over 1434x1350 targets for a mesh whose triangles cover
 * 38 % of them). The caller says which part it will sample (fx_set_need);
 * from there the run is walked backwards, per render target:
 *
 *   the pass that writes the result must fill what the caller samples
 *     (plus the texels a linear filter reads next to a sample);
 *   a pass that fills region R reads, of each texture earlier passes of
 *     the run produced,
 *       - R grown by the filter margin, when every lookup of that texture
 *         is made at the fragment's own texture coordinate ("local": the
 *         coordinate is a varying that the vertex shader sets to
 *         a_TexCoord and nothing else touches), or
 *       - R grown by the farthest the shader can move a lookup, where that
 *         distance is known (FXR_M_*: stock displacement shaders,
 *         recognised by a hash of ALL their compiled statements; the bound
 *         is computed from the current values of their constants on every
 *         run), or
 *       - the WHOLE texture in every other case.
 *
 * The pass is then drawn exactly as before - same program, same uniforms,
 * same vertices, same viewport, same target - with the scissor test set to
 * the rectangles it has to fill. A scissor does not change which values a
 * fragment gets (the GL's invariance rules), so every pixel that is read
 * later holds the value the unrestricted run would have put there; pixels
 * outside the rectangles keep stale content that, by the walk above, no
 * later pass or draw reads. Nothing is resized, no shader is touched: the
 * picture is the same byte for byte by construction, not by measurement.
 *
 * Left alone (drawn in full, as before): a run whose output a cache keeps
 * over frames (static sets, soft sets while kept, the static prefix while
 * it is being drawn), chains with feedback, runs under the non-default
 * quality knobs (effect update rate, fx_specialize, fx_quality=fast),
 * transform effects folded into the layer's matrix, the diagnostics that
 * read targets back. WALPE_FX_REGION=0 turns the whole thing off (A/B).
 * WALPE_FX_REGION_RECTS=n (1..6, default 6): rectangles per pass.
 * WALPE_FX_REGION_LOG=1: one line per pass at build time (hash, verdict). */

/* ---- switches ------------------------------------------------------- */

static gint fxr_on = -1;        /* -1 = read WALPE_FX_REGION */
static gint fxr_max_rects = -1;

static gboolean fxr_enabled (void)
{
    int v = g_atomic_int_get (&fxr_on);
    if (G_UNLIKELY (v < 0)) {
        const char *e = g_getenv ("WALPE_FX_REGION");
        v = !(e && *e == '0');
        g_atomic_int_set (&fxr_on, v);
    }
    return v;
}

void fx_set_region_enabled (gboolean on)
{
    g_atomic_int_set (&fxr_on, on ? 1 : 0);
}

/* WALPE_FX_REGION_CHECK=n (diagnostics, off by default): every nth run
 * that had restricted passes is drawn a second time whole, and the two
 * results are compared where the caller said it reads (the texels a
 * linear sample there can take). A difference is logged as a warning; the
 * whole result is the one that is shown. This is the check to run on a
 * real card: it does not need a scene that renders the same twice. */
static __thread guint fxr_n_dead;   /* passes not drawn because nothing reads their target */
static gint fxr_chk_every = -1;   /* -1 = read WALPE_FX_REGION_CHECK */
static gint fxr_chk_runs, fxr_chk_bad;

static int fxr_check_every (void)
{
    int v = g_atomic_int_get (&fxr_chk_every);
    if (G_UNLIKELY (v < 0)) {
        const char *e = g_getenv ("WALPE_FX_REGION_CHECK");
        v = e && *e ? CLAMP (atoi (e), 0, 1000000) : 0;
        g_atomic_int_set (&fxr_chk_every, v);
    }
    return v;
}

void fx_set_region_check (int every)
{
    g_atomic_int_set (&fxr_chk_every, MAX (0, every));
}

void fx_region_check_stats (guint *runs, guint *bad)
{
    if (runs)
        *runs = (guint) g_atomic_int_get (&fxr_chk_runs);
    if (bad)
        *bad = (guint) g_atomic_int_get (&fxr_chk_bad);
}

static int fxr_rect_limit (void)
{
    int v = g_atomic_int_get (&fxr_max_rects);
    if (G_UNLIKELY (v < 0)) {
        const char *e = g_getenv ("WALPE_FX_REGION_RECTS");
        v = e && *e ? atoi (e) : FXR_MAX_RECTS;
        v = CLAMP (v, 1, FXR_MAX_RECTS);
        g_atomic_int_set (&fxr_max_rects, v);
    }
    return v;
}

/* what the caller of the next fx_run_for_object () samples of its result
 * (consumed by that run, like fx_layer) */
static __thread FxNeed fxr_caller_need;
/* the id of the plan of the run in progress (its passes carry it in
 * rg_serial), 0 = this run has none; ids are never reused */
static __thread guint fxr_serial;
static gint fxr_next_id;
static __thread gboolean fxr_scissor;      /* GL_SCISSOR_TEST is on */
/* pixels the passes of this thread would have shaded / did shade */
static __thread guint64 fxr_px_full, fxr_px_drawn;
/* diagnostics (WALPE_FX_REGION_LOG): target pixels of the passes of runs
 * whose caller named rectangles, and what those rectangles are of them -
 * what the passes would shade if every reach were known and zero */
static __thread double fxr_px_asked, fxr_px_floor;
static __thread float fxr_run_frac;   /* of the run in progress; 0 = no rectangles */
static __thread guint fxr_n_restricted, fxr_n_rects;

void fx_set_need (const float (*rects)[4], int n)
{
    FxNeed *nd = &fxr_caller_need;
    nd->mode = FXR_NONE;
    nd->n = 0;
    if (!rects || n < 1 || n > FXR_MAX_RECTS || !fxr_enabled ())
        return;
    for (int i = 0; i < n; i++) {
        float u0 = rects[i][0], v0 = rects[i][1], u1 = rects[i][2], v1 = rects[i][3];
        if (!(u0 <= u1 && v0 <= v1) || !isfinite (u0) || !isfinite (v0) || !isfinite (u1) ||
            !isfinite (v1)) {
            nd->n = 0;   /* NaN or inverted: no statement about the result */
            return;
        }
        nd->r[i][0] = CLAMP (u0, 0.0f, 1.0f);
        nd->r[i][1] = CLAMP (v0, 0.0f, 1.0f);
        nd->r[i][2] = CLAMP (u1, 0.0f, 1.0f);
        nd->r[i][3] = CLAMP (v1, 0.0f, 1.0f);
    }
    nd->n = n;
    nd->mode = FXR_RECTS;
}

void fx_region_stats (guint64 *px_full, guint64 *px_drawn, guint *passes, guint *rects,
                      gboolean reset)
{
    if (px_full)
        *px_full = fxr_px_full;
    if (px_drawn)
        *px_drawn = fxr_px_drawn;
    if (passes)
        *passes = fxr_n_restricted;
    if (rects)
        *rects = fxr_n_rects;
    if (reset) {
        fxr_px_full = fxr_px_drawn = 0;
        fxr_n_restricted = fxr_n_rects = 0;
        fxr_px_asked = fxr_px_floor = 0;
    }
}

/* diagnostics: see fxr_px_asked */
void fx_region_potential (double *asked, double *floor_px)
{
    if (asked)
        *asked = fxr_px_asked;
    if (floor_px)
        *floor_px = fxr_px_floor;
}

static inline void fxr_scissor_off (void)
{
    if (fxr_scissor) {
        glDisable (GL_SCISSOR_TEST);
        fxr_scissor = FALSE;
    }
}

/* ---- needs ------------------------------------------------------------ */

static void fxr_need_full (FxNeed *n)
{
    n->mode = FXR_FULL;
    n->n = 0;
}

/* n := n united with a (both in the same texture's coordinates). When the
 * rectangles do not fit, the two that cost least to join become one. */
static void fxr_need_add (FxNeed *n, const FxNeed *a)
{
    if (a->mode == FXR_NONE || n->mode == FXR_FULL)
        return;
    if (a->mode == FXR_FULL) {
        fxr_need_full (n);
        return;
    }
    if (n->mode == FXR_NONE) {
        *n = *a;
        return;
    }
    float r[2 * FXR_MAX_RECTS][4];
    int k = 0;
    for (int i = 0; i < n->n; i++)
        memcpy (r[k++], n->r[i], sizeof (r[0]));
    for (int i = 0; i < a->n; i++)
        memcpy (r[k++], a->r[i], sizeof (r[0]));
    while (k > FXR_MAX_RECTS) {
        int bi = 0, bj = 1;
        float best = G_MAXFLOAT;
        for (int i = 0; i < k; i++)
            for (int j = i + 1; j < k; j++) {
                float u0 = fminf (r[i][0], r[j][0]), v0 = fminf (r[i][1], r[j][1]);
                float u1 = fmaxf (r[i][2], r[j][2]), v1 = fmaxf (r[i][3], r[j][3]);
                float grow = (u1 - u0) * (v1 - v0) - (r[i][2] - r[i][0]) * (r[i][3] - r[i][1]) -
                             (r[j][2] - r[j][0]) * (r[j][3] - r[j][1]);
                if (grow < best) {
                    best = grow;
                    bi = i;
                    bj = j;
                }
            }
        r[bi][0] = fminf (r[bi][0], r[bj][0]);
        r[bi][1] = fminf (r[bi][1], r[bj][1]);
        r[bi][2] = fmaxf (r[bi][2], r[bj][2]);
        r[bi][3] = fmaxf (r[bi][3], r[bj][3]);
        memcpy (r[bj], r[k - 1], sizeof (r[0]));
        k--;
    }
    n->n = k;
    memcpy (n->r, r, (gsize) k * sizeof (r[0]));
}

/* every rectangle of n grown by (du, dv) on each side, within 0..1 */
static FxNeed fxr_need_grow (const FxNeed *n, float du, float dv)
{
    FxNeed o = *n;
    if (o.mode != FXR_RECTS)
        return o;
    if (!(du >= 0.0f && dv >= 0.0f) || !isfinite (du) || !isfinite (dv)) {
        fxr_need_full (&o);
        return o;
    }
    for (int i = 0; i < o.n; i++) {
        o.r[i][0] = fmaxf (0.0f, o.r[i][0] - du);
        o.r[i][1] = fmaxf (0.0f, o.r[i][1] - dv);
        o.r[i][2] = fminf (1.0f, o.r[i][2] + du);
        o.r[i][3] = fminf (1.0f, o.r[i][3] + dv);
    }
    return o;
}

/* The centres of the pixels of a tw x th target a need covers (fxr_pixels:
 * each rectangle rounded outwards to the grid), as rectangles again: from
 * the centre of the first pixel to the centre of the last. */
static FxNeed fxr_need_centres (const FxNeed *n, int tw, int th)
{
    FxNeed o = *n;
    if (o.mode != FXR_RECTS)
        return o;
    o.n = 0;
    for (int i = 0; i < n->n && i < FXR_MAX_RECTS; i++) {
        int x0 = (int) floorf (n->r[i][0] * (float) tw), y0 = (int) floorf (n->r[i][1] * (float) th);
        int x1 = (int) ceilf (n->r[i][2] * (float) tw), y1 = (int) ceilf (n->r[i][3] * (float) th);
        x0 = CLAMP (x0, 0, tw);
        y0 = CLAMP (y0, 0, th);
        x1 = CLAMP (x1, 0, tw);
        y1 = CLAMP (y1, 0, th);
        if (x1 <= x0 || y1 <= y0)
            continue;
        o.r[o.n][0] = ((float) x0 + 0.5f) / (float) tw;
        o.r[o.n][1] = ((float) y0 + 0.5f) / (float) th;
        o.r[o.n][2] = ((float) x1 - 0.5f) / (float) tw;
        o.r[o.n][3] = ((float) y1 - 0.5f) / (float) th;
        o.n++;
    }
    if (o.n < 1)
        fxr_need_full (&o);   /* nothing to go by: as before */
    return o;
}

/* A rectangle costs a glScissor and a draw call: it has to save at least
 * this many pixels of shading, or it is joined with a neighbour / the
 * target is drawn whole. 32768 by default: on the maintainer's RTX 4050 a
 * plain full-screen pass costs about 0.05 ms for 2 Mpx (walpe-ctl profile), i.e.
 * 32768 pixels are worth about a microsecond - the order of two GL calls.
 * WALPE_FX_REGION_MINPX=n changes it (0 = any saving counts). */
static gint fxr_min_saved = -1;   /* -1 = read WALPE_FX_REGION_MINPX */

static guint fxr_min_px (void)
{
    int v = g_atomic_int_get (&fxr_min_saved);
    if (G_UNLIKELY (v < 0)) {
        const char *e = g_getenv ("WALPE_FX_REGION_MINPX");
        v = e && *e ? CLAMP (atoi (e), 0, 1 << 24) : 32768;
        g_atomic_int_set (&fxr_min_saved, v);
    }
    return (guint) v;
}

/* The pixels of a tw x th target a need covers: rectangles x, y, width,
 * height, each rounded outwards. Returns their number;
 * 0 = the whole target (the need is "everything", is empty or unknown, or
 * what a restriction would save is not worth its calls: less than a
 * sixteenth of the target, or less than fxr_min_px () pixels a
 * rectangle). *area = pixels of the rectangles. */
static int fxr_pixels (const FxNeed *n, int tw, int th, int px[][4], guint64 *area)
{
    if (n->mode != FXR_RECTS || n->n < 1 || tw < 16 || th < 16)
        return 0;
    int r[FXR_MAX_RECTS][4], k = 0;
    for (int i = 0; i < n->n && i < FXR_MAX_RECTS; i++) {
        int x0 = (int) floorf (n->r[i][0] * (float) tw), y0 = (int) floorf (n->r[i][1] * (float) th);
        int x1 = (int) ceilf (n->r[i][2] * (float) tw), y1 = (int) ceilf (n->r[i][3] * (float) th);
        x0 = CLAMP (x0, 0, tw);
        y0 = CLAMP (y0, 0, th);
        x1 = CLAMP (x1, 0, tw);
        y1 = CLAMP (y1, 0, th);
        if (x1 <= x0 || y1 <= y0)
            continue;
        r[k][0] = x0, r[k][1] = y0, r[k][2] = x1, r[k][3] = y1;
        k++;
    }
    /* Fewer rectangles where that costs nothing: one inside another goes,
     * two whose union is exactly their bounding box become it. Rectangles
     * may overlap otherwise (neighbours grown by a margin share a strip):
     * the strip is drawn twice with the same values - joining them into
     * one box would shade the whole corner between them instead. Two
     * whose bounding box adds fewer pixels than a rectangle is worth are
     * joined as well; beyond the limit, the two whose box adds least. */
    const int limit = fxr_rect_limit ();
    const gint64 min_px = (gint64) fxr_min_px ();
    for (gboolean again = TRUE; again;) {
        again = FALSE;
        int bi = -1, bj = -1;
        gint64 best = G_MAXINT64;
        for (int i = 0; i < k; i++)
            for (int j = i + 1; j < k; j++) {
                gint64 bw = MAX (r[i][2], r[j][2]) - MIN (r[i][0], r[j][0]);
                gint64 bh = MAX (r[i][3], r[j][3]) - MIN (r[i][1], r[j][1]);
                gint64 ow = MIN (r[i][2], r[j][2]) - MAX (r[i][0], r[j][0]);
                gint64 oh = MIN (r[i][3], r[j][3]) - MAX (r[i][1], r[j][1]);
                gint64 over = ow > 0 && oh > 0 ? ow * oh : 0;
                gint64 grow = bw * bh - (gint64) (r[i][2] - r[i][0]) * (r[i][3] - r[i][1]) -
                              (gint64) (r[j][2] - r[j][0]) * (r[j][3] - r[j][1]) + over;
                if (grow < best) {
                    best = grow;
                    bi = i;
                    bj = j;
                }
            }
        if (bi >= 0 && (best <= 0 || best < min_px || k > limit)) {
            r[bi][0] = MIN (r[bi][0], r[bj][0]);
            r[bi][1] = MIN (r[bi][1], r[bj][1]);
            r[bi][2] = MAX (r[bi][2], r[bj][2]);
            r[bi][3] = MAX (r[bi][3], r[bj][3]);
            memcpy (r[bj], r[k - 1], sizeof (r[0]));
            k--;
            again = TRUE;
        }
    }
    guint64 a = 0;
    for (int i = 0; i < k; i++)
        a += (guint64) (r[i][2] - r[i][0]) * (guint64) (r[i][3] - r[i][1]);
    if (k < 1 || a * 16 > (guint64) tw * th * 15 ||
        (guint64) tw * th - a < (guint64) min_px * (guint64) k)
        return 0;
    for (int i = 0; i < k; i++) {
        px[i][0] = r[i][0];
        px[i][1] = r[i][1];
        px[i][2] = r[i][2] - r[i][0];
        px[i][3] = r[i][3] - r[i][1];
    }
    if (area)
        *area = a;
    return k;
}

/* ---- what a pass reads, from its sources -------------------------------- */

static gboolean fxr_identc (char c)
{
    return g_ascii_isalnum (c) || c == '_';
}

/* next occurrence of word w in t at or behind p as a whole identifier
 * (text that still has its white space: macro lines) */
static const char *fxr_word (const char *t, const char *p, const char *w)
{
    gsize n = strlen (w);
    for (; (p = strstr (p, w)) != NULL; p += n)
        if ((p == t || !fxr_identc (p[-1])) && !fxr_identc (p[n]))
            return p;
    return NULL;
}

static gboolean fxr_ends_with (const char *t, const char *p, const char *s)
{
    gsize n = strlen (s);
    return (gsize) (p - t) >= n && !strncmp (p - n, s, n);
}

static gboolean fxr_after_type (const char *t, const char *p);

/* Next occurrence of identifier w in compiled statements t (white space
 * removed, so a declaration reads "vec4v_TexCoord"): either a whole word,
 * or directly behind a type name (*decl = TRUE). */
static const char *fxr_occ (const char *t, const char *p, const char *w, gboolean *decl)
{
    gsize n = strlen (w);
    for (; (p = strstr (p, w)) != NULL; p += n) {
        if (fxr_identc (p[n]))
            continue;
        *decl = fxr_after_type (t, p);
        if (*decl || p == t || !fxr_identc (p[-1]))
            return p;
    }
    return NULL;
}

/* p stands right behind a type name (white space is gone): a declaration */
static gboolean fxr_after_type (const char *t, const char *p)
{
    static const char *const ty[] = { "vec2", "vec3", "vec4", "float", "float2", "float3", "float4",
                                      "int", "ivec2", "ivec3", "ivec4", "mat2", "mat3", "mat4",
                                      "bool", "sampler2D" };
    for (guint i = 0; i < G_N_ELEMENTS (ty); i++)
        if (fxr_ends_with (t, p, ty[i])) {
            const char *q = p - strlen (ty[i]);
            if (q == t || !fxr_identc (q[-1]) || fxr_ends_with (t, q, "varying") ||
                fxr_ends_with (t, q, "attribute") || fxr_ends_with (t, q, "uniform") ||
                fxr_ends_with (t, q, "in") || fxr_ends_with (t, q, "out") ||
                fxr_ends_with (t, q, "const"))
                return TRUE;
        }
    return FALSE;
}

/* The #define lines of a shader body (everything behind the prelude's
 * "#line 1"), whatever #if they stand in: what a macro could hide. */
static char *fxr_macros (const char *src)
{
    const char *body = strstr (src, "#line 1\n");
    GString *o = g_string_new (" ");
    if (!body)
        return g_string_free (o, FALSE);
    gboolean cont = FALSE;
    for (const char *l = body; *l;) {
        const char *e = strchr (l, '\n');
        gsize n = e ? (gsize) (e - l) : strlen (l);
        const char *d = l;
        while (d < l + n && (*d == ' ' || *d == '\t'))
            d++;
        gboolean def = cont;
        if (d < l + n && *d == '#') {
            const char *w = d + 1;
            while (w < l + n && (*w == ' ' || *w == '\t'))
                w++;
            def = (gsize) (l + n - w) >= 6 && !strncmp (w, "define", 6);
        }
        if (def) {
            g_string_append_len (o, l, (gssize) n);
            g_string_append_c (o, ' ');
            gsize m = n;
            while (m > 0 && g_ascii_isspace (l[m - 1]))
                m--;
            cont = m > 0 && l[m - 1] == '\\';
        } else {
            cont = FALSE;
        }
        l = e ? e + 1 : l + n;
    }
    return g_string_free (o, FALSE);
}

/* the compiled statements of a built source without those of the engine's
 * own prelude (g_free), NULL when the source has no "#line 1" */
static char *fxr_body_code (const char *src)
{
    const char *mark = strstr (src, "#line 1\n");
    if (!mark)
        return NULL;
    char *head = g_strndup (src, (gsize) (mark - src) + 8);
    char *all = fx_active_code (src), *pre = fx_active_code (head);
    char *body = g_str_has_prefix (all, pre) ? g_strdup (all + strlen (pre)) : NULL;
    g_free (head);
    g_free (all);
    g_free (pre);
    return body;
}

/* Vertex shader (compiled statements vt): which halves of varying v carry
 * the quad's texture coordinate untouched? bit 0 = .xy (or the whole of a
 * vec2), bit 1 = .zw. Every write to a half must be `= a_TexCoord`,
 * `= a_TexCoord.xy` or (the whole vec4) `= a_TexCoord.xyxy`, and there
 * must be one; any other write, a compound assignment, an increment, or
 * the varying handed to a call (it may be an out parameter) spoils the
 * halves it can reach. */
static guint fxr_identity_varying (const char *vt, const char *v)
{
    gboolean id[2] = { FALSE, FALSE }, bad[2] = { FALSE, FALSE };
    int decls = 0;
    gsize vn = strlen (v);
    gboolean decl = FALSE;
    for (const char *p = vt; (p = fxr_occ (vt, p, v, &decl)) != NULL; p += vn) {
        if (decl) {
            decls++;
            continue;
        }
        if (p > vt && p[-1] == '.')
            return 0;                   /* a member of that name */
        const char *q = p + vn;
        gboolean xy = TRUE, zw = TRUE, swz = FALSE;
        char sw[8] = "";
        if (*q == '[')
            return 0;
        if (*q == '.') {
            gsize k = 0;
            for (q++; *q && strchr ("xyzwrgbastpq", *q) && k < sizeof (sw) - 1; q++)
                sw[k++] = *q;
            sw[k] = 0;
            swz = TRUE;
            xy = strpbrk (sw, "xyrgst") != NULL;
            zw = strpbrk (sw, "zwbapq") != NULL;
        }
        gboolean assign = *q == '=' && q[1] != '=';
        gboolean compound = (*q && strchr ("+-*/%&|^", *q) && q[1] == '=') ||
                            (*q == '+' && q[1] == '+') || (*q == '-' && q[1] == '-') ||
                            (*q == '<' && q[1] == '<' && q[2] == '=') ||
                            (*q == '>' && q[1] == '>' && q[2] == '=');
        gboolean pre_inc = p - vt >= 2 && ((p[-1] == '+' && p[-2] == '+') || (p[-1] == '-' && p[-2] == '-'));
        gboolean arg = p > vt && (p[-1] == '(' || p[-1] == ',') && (*q == ',' || *q == ')');
        if (assign) {
            const char *rhs = q + 1;
            const char *e = strchr (rhs, ';');
            gsize rn = e ? (gsize) (e - rhs) : 0;
            gboolean r_tc = rn == 10 && !strncmp (rhs, "a_TexCoord", 10);
            gboolean r_xy = rn == 13 && !strncmp (rhs, "a_TexCoord.xy", 13);
            gboolean r_xyxy = rn == 15 && !strncmp (rhs, "a_TexCoord.xyxy", 15);
            if (!swz) {
                if (r_tc || r_xy)
                    id[0] = TRUE;       /* a vec2 varying */
                else if (r_xyxy)
                    id[0] = id[1] = TRUE;
                else
                    bad[0] = bad[1] = TRUE;
            } else if (!strcmp (sw, "xy") && (r_tc || r_xy)) {
                id[0] = TRUE;
            } else if (!strcmp (sw, "zw") && (r_tc || r_xy)) {
                id[1] = TRUE;
            } else if (!strcmp (sw, "xyzw") && r_xyxy) {
                id[0] = id[1] = TRUE;
            } else {
                bad[0] |= xy;
                bad[1] |= zw;
            }
        } else if (compound || pre_inc || arg) {
            bad[0] |= xy;
            bad[1] |= zw;
        }
    }
    if (decls != 1)
        return 0;
    return (id[0] && !bad[0] ? 1u : 0u) | (id[1] && !bad[1] ? 2u : 0u);
}

/* Fragment shader (compiled statements ft): varying v is declared once and
 * never written or handed to a call other than as the coordinate of a
 * lookup (those occurrences are skipped by the caller's rule: they end in
 * ')' behind a sampler argument). */
static gboolean fxr_fragment_reads_only (const char *ft, const char *v)
{
    int decls = 0;
    gsize vn = strlen (v);
    gboolean decl = FALSE;
    for (const char *p = ft; (p = fxr_occ (ft, p, v, &decl)) != NULL; p += vn) {
        if (decl) {
            decls++;
            continue;
        }
        if (p > ft && p[-1] == '.')
            return FALSE;
        const char *q = p + vn;
        if (*q == '[')
            return FALSE;
        if (*q == '.')
            for (q++; *q && strchr ("xyzwrgbastpq", *q); q++)
                ;
        if ((*q == '=' && q[1] != '=') || (*q && strchr ("+-*/%&|^", *q) && q[1] == '=') ||
            (*q == '+' && q[1] == '+') || (*q == '-' && q[1] == '-') ||
            (*q == '<' && q[1] == '<' && q[2] == '=') || (*q == '>' && q[1] == '>' && q[2] == '='))
            return FALSE;
        if (p - ft >= 2 && ((p[-1] == '+' && p[-2] == '+') || (p[-1] == '-' && p[-2] == '-')))
            return FALSE;
        if (p > ft && (p[-1] == '(' || p[-1] == ',') && (*q == ',' || *q == ')')) {
            /* an argument: fine only as the coordinate of a lookup */
            const char *b = p - 1;
            gboolean lookup = FALSE;
            if (*b == ',' && b - ft >= 11 && !strncmp (b - 10, "g_Texture", 9) &&
                g_ascii_isdigit (b[-1]) && b[-11] == '(' &&
                (fxr_ends_with (ft, b - 10, "texSample2D(") || fxr_ends_with (ft, b - 10, "texture2D(") ||
                 fxr_ends_with (ft, b - 10, "texture(")))
                lookup = TRUE;
            if (!lookup)
                return FALSE;
        }
    }
    return decls == 1;
}

/* ---- displacement shaders with a known reach --------------------------
 * Recognised by the FNV-1a hash of ALL compiled statements of both stages
 * (the shader's own text with its includes, dead #if branches, comments
 * and white space removed; the engine's prelude not included): one table
 * row per version of the two files and per combination of combos, each
 * version read and worked through by hand. A scene's package ships the
 * copies of Wallpaper Engine's effect shaders its author's version had, so
 * there are several versions of each in a library (the test library: 6 of
 * shake.frag, 5 of waterwaves.frag, 2 of iris.frag); the table has every
 * pair of files found there, with every combination of their combos. Any
 * edit of a statement, another version, a workshop variant: no row, the
 * pass counts as reading everything. `g` below is one channel of an 8-bit
 * texture (0..1). The bounds (fxr_reach), per axis, in texture
 * coordinates:
 *
 * FXR_M_SHAKE  shake.frag with bounds (4 versions, AUDIOPROCESSING 0).
 *   The only computed lookup is at v_TexCoord.xy + offset * g_Amp * g_Amp
 *   * flowMask, flowMask = (g - 0.498) * 2 in [-0.996, 1.004], offset =
 *   saturate (...) in [0, 1], then for DIRECTION 0 offset * 2 - 1, for 1
 *   unchanged, for 2 offset - 1: |offset| <= 1 always, provided the value
 *   handed to saturate is no NaN - it is finite when g_Bounds.x !=
 *   g_Bounds.y, 0 <= g_Friction <= 1000 and the constants are finite (pow
 *   of a base in [0.002, 0.998]). Reach 1.004 * g_Amp^2.
 * FXR_M_SHAKE_OLD / _OLDN  the two versions without bounds and direction.
 *   NOISE 0: offset = mix (1 - pow (1 - s, f.x), pow (s, f.y), step) * 2 -
 *   1 with s in (0, 1): in [-1, 1] for 0 <= g_Friction <= 1000. Reach
 *   1.004 * g_Amp^2. NOISE 1: offset = dot (0.5, four such values) * 2 - 1
 *   in [-1, 3]. Reach 3.012 * g_Amp^2.
 * FXR_M_WAVES / _WAVES2  waterwaves.frag with exponent. texCoord =
 *   v_TexCoord.xy + val1 * s1 [* val2 * s2] * offset * strength * mask,
 *   val = pow (abs (sin), exponent) in [0, 1] for an exponent > 0, s =
 *   sign, offset = a unit vector turned (each component at most 1 in
 *   size), strength = g_Strength^2, mask = g or 1. Reach g_Strength^2.
 * FXR_M_WAVES_OLD  the four versions before (not with PERSPECTIVE 1 of
 *   the last of them, whose coordinate is divided). texCoord =
 *   v_TexCoord.xy + sin (..) * offset * (g_Strength^2 + g_Perspective *
 *   pos) * mask with pos = abs (dot (v_TexCoord.xy - 0.5, unit vector))
 *   <= 0.7072. Reach g_Strength^2 + 0.708 * |g_Perspective|.
 * FXR_M_IRIS   iris.frag (2 versions). Lookups at v_TexCoord.xy and at
 *   v_TexCoord.xy + v_TexCoordIris.xy [* g], v_TexCoordIris = da * g_Scale
 *   * 0.001, da = mix (moveStart, moveEnd, smoothstep) + (sin, cos) *
 *   g_NoiseAmount with moveStart / moveEnd sums of two sines: |da| <= 2 +
 *   |noise| (smoothstep is defined for g_Rough > 0). Reach (2 +
 *   |g_NoiseAmount|) * |g_Scale| * 0.001.
 * Every bound is widened by 1 / 1024 of itself, the plan adds the filter
 * margin, and the constants are those the pass holds at this run (a
 * script's value included; no number = reads everything). */
enum { FXR_M_NONE = 0, FXR_M_SHAKE, FXR_M_SHAKE_OLD, FXR_M_SHAKE_OLDN, FXR_M_WAVES, FXR_M_WAVES2,
       FXR_M_WAVES_OLD, FXR_M_IRIS, FXR_M_DOF_MIX, FXR_M_N };

/* FXR_M_DOF_MIX is not a displacement but a gate: the last pass of the
 * workshop depth-of-field effect (2798319181 "bokeh_blur": its gaussian
 * shader with VERTICAL, blend mode normal) finishes by mixing the effect's
 * input (unit 1) with the blurred picture (unit 0, weighted by the circle
 * of confusion on unit 2) by the factor "Opacity" (u_alpha) times a
 * strength, the clamped logarithm of u_strength * 5 + 1. While u_alpha is
 * exactly 0 and u_strength >= 0, that factor is exactly 0 and the mix
 * returns the input bit for bit - computed as x * 1 + y * 0 as well as
 * x + (y - x) * 0 - whatever units 0 and 2 hold, as long as that is
 * finite (8-bit targets: always). So nothing reads those two targets in
 * such a run, and the four passes that fill them are not drawn (the plan
 * below: a target nobody reads). Arona 2901582518 fades this effect out
 * over its first four seconds and leaves it on with Opacity 0. */
static guint64 fxr_test_gate_hash;   /* test: this hash is such a pass */
static gboolean fx_target_unorm8 (const EffTarget *t);   /* NULL: the chain output / input */

/* In all of them the displacement is a product with a value read from a
 * painted texture on unit 1 at v_TexCoord.zw (the quad's coordinate times
 * image size / padded size): shake's flow map (FXR_K_FLOW: both channels,
 * (g - 0.498) * 2), the opacity mask of water waves and iris
 * (FXR_K_OPACITY: the red channel). Such maps are painted on a small part
 * of the layer - a strand of hair, the eyes - and the pass moves nothing
 * elsewhere. fxr_support () finds where the map can make the product
 * larger than FXR_WEAK_* of its maximum; outside, the bound is that
 * fraction of the reach (a flow map's "no flow" is 127 / 255, not 0.498:
 * 0.00008, not zero). FXR_K_NONE: no such texture, the reach holds
 * everywhere. */
enum { FXR_K_NONE = 0, FXR_K_FLOW, FXR_K_OPACITY };
#define FXR_WEAK_FLOW 0.005f       /* |g - 0.498| <= 0.0025 on both channels */
#define FXR_WEAK_OPACITY 0.0002f   /* red <= 0.0001 */

static const struct { guint64 hash; guint8 model; } fxr_models[] = {
#include "fx_region_models.h"
    { 0, FXR_M_NONE }
};

static guint64 fxr_hash (const char *a, const char *b)
{
    guint64 h = 1469598103934665603ull;
    for (const char *p = a; *p; p++) {
        h ^= (guint8) *p;
        h *= 1099511628211ull;
    }
    h ^= 0x7c;
    h *= 1099511628211ull;
    for (const char *p = b; *p; p++) {
        h ^= (guint8) *p;
        h *= 1099511628211ull;
    }
    return h;
}

/* build time: what the pass reads, from its two sources as built (before
 * any repair or specialisation). Sets rg_plain, rg_local, rg_model,
 * rg_hash. */
static void fxr_traits (EffPass *pass, const char *vsrc, const char *fsrc, const char *name,
                        GHashTable *combos)
{
    pass->rg_plain = FALSE;
    pass->rg_local = 0;
    pass->rg_model = FXR_M_NONE;
    pass->rg_hash = 0;
    pass->rg_mask = FXR_K_NONE;
    pass->rg_sup.mode = FXR_NONE;
    pass->rg_sup_levels = -1;
    for (int i = 0; i < 4; i++)
        pass->rg_c[i] = -1;
    char *vt = fx_active_code (vsrc), *ft = fx_active_code (fsrc);
    /* the pass must fill its target as the plain quad with a_TexCoord */
    gboolean plain = !pass->moves_verts && fx_vertex_plain_cover (vt) &&
                     (strstr (vt, "attributevec2a_TexCoord;") || strstr (vt, "invec2a_TexCoord;"));
    {
        /* a_TexCoord: declared once, never anything but read */
        int decls = 0;
        gboolean decl = FALSE;
        for (const char *p = vt; plain && (p = fxr_occ (vt, p, "a_TexCoord", &decl)) != NULL; p += 10) {
            if (decl) {
                decls++;
                continue;
            }
            const char *q = p + 10;
            if (*q == '.')
                for (q++; *q && strchr ("xyzwrgbastpq", *q); q++)
                    ;
            if ((*q == '=' && q[1] != '=') || (*q && strchr ("+-*/", *q) && q[1] == '=') ||
                (*q == '+' && q[1] == '+') || (*q == '-' && q[1] == '-'))
                plain = FALSE;
        }
        plain = plain && decls == 1;
    }
    char *vmac = fxr_macros (vsrc), *fmac = fxr_macros (fsrc);
    static const char *const hide[] = { "g_Texture", "sampler", "texSample", "texture", "a_TexCoord",
                                        "gl_Position", "gl_FragCoord" };
    gboolean opaque = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS (hide); i++)
        opaque |= strstr (vmac, hide[i]) != NULL || strstr (fmac, hide[i]) != NULL;
    /* samplers: only the fragment shader's g_Texture0..7; none in the
     * vertex shader */
    opaque |= strstr (vt, "sampler") != NULL;
    for (const char *p = ft; !opaque && (p = strstr (p, "sampler")) != NULL; p += 7)
        if (!(g_str_has_prefix (p, "sampler2Dg_Texture") && p[18] >= '0' && p[18] <= '7' &&
              !fxr_identc (p[19])))
            opaque = TRUE;
    guint seen = 0, bad = 0;
    GHashTable *verdict = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (const char *p = ft; plain && !opaque && (p = strstr (p, "g_Texture")) != NULL; p += 9) {
        if (p > ft && fxr_identc (p[-1]))
            continue;
        if (!(p[9] >= '0' && p[9] <= '7') || fxr_identc (p[10]))
            continue;                               /* g_Texture0Resolution ... */
        const guint bit = 1u << (p[9] - '0');
        if (fxr_ends_with (ft, p, "sampler2D"))
            continue;                               /* its declaration */
        seen |= bit;
        /* lookup (sampler, coordinate) and nothing else */
        static const char *const fn[] = { "texSample2D(", "texture2D(", "texture(" };
        gboolean call = FALSE;
        for (guint i = 0; i < G_N_ELEMENTS (fn) && !call; i++)
            if (fxr_ends_with (ft, p, fn[i])) {
                const char *s = p - strlen (fn[i]);
                call = s == ft || !fxr_identc (s[-1]);
            }
        if (!call || p[10] != ',') {
            bad |= bit;
            continue;
        }
        const char *a = p + 11, *e = a;
        while (fxr_identc (*e))
            e++;
        guint half = 1;
        gboolean swz = FALSE;
        if (e > a && e[0] == '.' && e[1] == 'x' && e[2] == 'y' && !fxr_identc (e[3])) {
            swz = TRUE;
            if (e[3] != ')')
                e = a;
        } else if (e > a && e[0] == '.' && e[1] == 'z' && e[2] == 'w' && !fxr_identc (e[3])) {
            swz = TRUE;
            half = 2;
            if (e[3] != ')')
                e = a;
        } else if (*e != ')') {
            e = a;
        }
        if (e == a || g_ascii_isdigit (*a)) {
            bad |= bit;
            continue;
        }
        char *vn = g_strndup (a, (gsize) (e - a));
        gpointer vd = NULL;
        if (!g_hash_table_lookup_extended (verdict, vn, NULL, &vd)) {
            guint halves = 0;
            if (fxr_fragment_reads_only (ft, vn) && !fxr_word (fmac, fmac, vn) &&
                !fxr_word (vmac, vmac, vn))
                halves = fxr_identity_varying (vt, vn);
            /* the whole name as a coordinate is a vec2 varying: bit 0 */
            char *d2 = g_strconcat ("varyingvec2", vn, ";", NULL);
            char *d4 = g_strconcat ("varyingvec4", vn, ";", NULL);
            guint ty = strstr (ft, d2) ? 2u : strstr (ft, d4) ? 4u : 0u;
            g_free (d2);
            g_free (d4);
            vd = GUINT_TO_POINTER (halves | (ty << 4));
            g_hash_table_insert (verdict, g_strdup (vn), vd);
        }
        g_free (vn);
        guint halves = GPOINTER_TO_UINT (vd) & 3u, ty = GPOINTER_TO_UINT (vd) >> 4;
        gboolean ok = swz ? (ty == 4 || (ty == 2 && half == 1)) && (halves & half)
                          : ty == 2 && (halves & 1u);
        if (!ok)
            bad |= bit;
    }
    g_hash_table_unref (verdict);
    pass->rg_plain = plain;
    if (plain && !opaque)
        pass->rg_local = (guint8) ((seen & ~bad) | (~seen & 0xffu));
    /* a displacement shader worked through by hand? (the fragment shader
     * must name no sampler through a macro either: !opaque) */
    char *vb = fxr_body_code (vsrc), *fb = fxr_body_code (fsrc);
    if (vb && fb) {
        pass->rg_hash = fxr_hash (vb, fb);
        for (guint i = 0; plain && fxr_models[i].hash; i++)
            if (fxr_models[i].hash == pass->rg_hash &&
                (!opaque || fxr_models[i].model == FXR_M_DOF_MIX))   /* (a gate: no lookup is bounded) */
                pass->rg_model = fxr_models[i].model;
        if (plain && fxr_test_gate_hash && pass->rg_hash == fxr_test_gate_hash)
            pass->rg_model = FXR_M_DOF_MIX;
    }
    /* the texture that scales the model's displacement (fxr_support): the
     * statements are those of the versions the table pins, compared by key
     * (fx_keys.h) */
    pass->rg_mask = FXR_K_NONE;
    switch (pass->rg_model) {
    case FXR_M_SHAKE:
    case FXR_M_SHAKE_OLD:
    case FXR_M_SHAKE_OLDN:
        if (fxk_has (ft, FXK_RG_FLOW_READ) && fxk_has (ft, FXK_RG_FLOW_MASK))
            pass->rg_mask = FXR_K_FLOW;
        break;
    case FXR_M_WAVES:
    case FXR_M_WAVES2:
    case FXR_M_WAVES_OLD:
        if (fxk_has (ft, FXK_RG_MASK_READ) && !fxk_has (ft, FXK_RG_MASK_ONE))
            pass->rg_mask = FXR_K_OPACITY;
        break;
    case FXR_M_IRIS:
        if (fxk_has (ft, FXK_RG_IRIS_MASK))
            pass->rg_mask = FXR_K_OPACITY;
        break;
    default:
        break;
    }
    static WalpeLazy log_cell = WALPE_LAZY_UNSET;   /* read once (common/lazy.h) */
    int log = walpe_lazy_get (&log_cell);
    if (log < 0)
        log = walpe_lazy_set (&log_cell, g_getenv ("WALPE_FX_REGION_LOG") != NULL);
    if (log && g_getenv ("WALPE_FX_REGION_LOG")[0] == '2')
        g_message ("fx region: %s body code: %s | %s", name ? name : "?", vb ? vb : "-", fb ? fb : "-");
    if (log) {
        /* (development: the table rows of fx_region_models.h are made from
         * these lines and the shader file versions) */
        static const char *const cn[] = { "NOISE", "DIRECTION", "TIMEOFFSET", "MASK", "AUDIOPROCESSING",
                                          "PERSPECTIVE", "DUALWAVES", "BACKGROUND" };
        GString *cs = g_string_new (NULL);
        for (guint i = 0; combos && i < G_N_ELEMENTS (cn); i++)
            if (g_hash_table_contains (combos, cn[i]))
                g_string_append_printf (cs, " %s=%d", cn[i],
                                        GPOINTER_TO_INT (g_hash_table_lookup (combos, cn[i])));
        g_message ("fx region: %s hash 0x%016" G_GINT64_MODIFIER "x plain %d local 0x%02x model %d mask %d combos%s",
                   name ? name : "?", pass->rg_hash, pass->rg_plain, pass->rg_local, pass->rg_model,
                   pass->rg_mask, cs->str);
        g_string_free (cs, TRUE);
    }
    g_free (vb);
    g_free (fb);
    g_free (vmac);
    g_free (fmac);
    g_free (vt);
    g_free (ft);
}

/* link time: the constants a model's bound is made of (FxConst index by
 * uniform name; one that the program does not have ends the model) */
static void fxr_after_link (EffPass *pass)
{
    static const char *const names[FXR_M_N][4] = {
        [FXR_M_SHAKE]      = { "g_Amp", "g_Friction", "g_Bounds", NULL },
        [FXR_M_SHAKE_OLD]  = { "g_Amp", "g_Friction", NULL, NULL },
        [FXR_M_SHAKE_OLDN] = { "g_Amp", "g_Friction", NULL, NULL },
        [FXR_M_WAVES]      = { "g_Strength", "g_Exponent", NULL, NULL },
        [FXR_M_WAVES2]     = { "g_Strength", "g_Exponent", "g_Exponent2", NULL },
        [FXR_M_WAVES_OLD]  = { "g_Strength", "g_Perspective", NULL, NULL },
        [FXR_M_IRIS]       = { "g_Scale", "g_NoiseAmount", "g_Rough", NULL },
        [FXR_M_DOF_MIX]    = { "u_alpha", "u_strength", NULL, NULL },
    };
    if (pass->rg_model == FXR_M_NONE || pass->rg_model >= FXR_M_N || !pass->program)
        return;
    /* the model speaks of slot textures with values in 0..1: none of the
     * pass's other samplers may be a render target of the effect (the
     * gate is about exactly such targets) */
    gboolean ok = pass->n_binds == 0 || pass->rg_model == FXR_M_DOF_MIX;
    for (int u = 1; u < FX_MAX_BINDS; u++)
        ok = ok && (!pass->slot[u].rt || pass->rg_model == FXR_M_DOF_MIX);
    for (int i = 0; ok && i < 4 && names[pass->rg_model][i]; i++) {
        GLint loc = glGetUniformLocation (pass->program, names[pass->rg_model][i]);
        for (int k = 0; loc >= 0 && k < pass->n_consts; k++)
            if (pass->consts[k].loc == loc)
                pass->rg_c[i] = k;
        ok = pass->rg_c[i] >= 0;
    }
    if (!ok)
        pass->rg_model = FXR_M_NONE;
}

/* tests only: the bounds times this, the filter margin times that (1 = as
 * they are; smaller values must make the comparison in the test fail) */
static float fxr_test_reach = 1.0f, fxr_test_margin = 1.0f;
static gboolean fxr_test_no_strong;   /* test: treat a painted map as weak everywhere */

/* Does the pass multiply what it reads on `unit` by exactly zero in this
 * run (FXR_M_DOF_MIX above)? */
static gboolean fxr_test_no_gate;

static gboolean fxr_dead (const EffPass *pass, int unit)
{
    if (pass->rg_model != FXR_M_DOF_MIX || (unit != 0 && unit != 2) || fxr_test_no_gate ||
        pass->rg_c[0] < 0 || pass->rg_c[0] >= pass->n_consts || pass->rg_c[1] < 0 ||
        pass->rg_c[1] >= pass->n_consts)
        return FALSE;
    const float alpha = pass->consts[pass->rg_c[0]].v[0], strength = pass->consts[pass->rg_c[1]].v[0];
    return alpha == 0.0f && isfinite (strength) && strength >= 0.0f;
}

/* How far from the fragment's own texture coordinate does the pass read
 * the texture on `unit`? TRUE + (*ru, *rv) in texture coordinates; FALSE =
 * anywhere. *weak = the fraction of that reach which holds where the
 * pass's map is weak (fxr_support; 1 = no map: the reach holds everywhere). */
static gboolean fxr_reach (const EffPass *pass, int unit, float *ru, float *rv, float *weak)
{
    *ru = *rv = 0.0f;
    *weak = 1.0f;
    if (!pass->rg_plain || unit < 0 || unit >= FX_MAX_BINDS)
        return FALSE;
    if (pass->rg_local & (1u << unit))
        return TRUE;
    if (unit != 0 || pass->rg_model == FXR_M_NONE)
        return FALSE;
    const FxConst *c[4] = { NULL, NULL, NULL, NULL };
    for (int i = 0; i < 4; i++)
        if (pass->rg_c[i] >= 0 && pass->rg_c[i] < pass->n_consts) {
            c[i] = &pass->consts[pass->rg_c[i]];
            for (int k = 0; k < c[i]->n; k++)
                if (!isfinite (c[i]->v[k]))
                    return FALSE;
        }
    float r = -1.0f, r2 = -1.0f;
    switch (pass->rg_model) {
    case FXR_M_SHAKE:
        if (!c[2] || c[2]->n < 2 || fabsf (c[2]->v[1] - c[2]->v[0]) < 1e-6f)
            return FALSE;
        /* fall through */
    case FXR_M_SHAKE_OLD:
    case FXR_M_SHAKE_OLDN:
        if (!c[0] || !c[1] || c[1]->n < 2 || !(c[1]->v[0] >= 0.0f && c[1]->v[0] <= 1000.0f) ||
            !(c[1]->v[1] >= 0.0f && c[1]->v[1] <= 1000.0f))
            return FALSE;
        r = r2 = (pass->rg_model == FXR_M_SHAKE_OLDN ? 3.012f : 1.004f) * c[0]->v[0] * c[0]->v[0];
        break;
    case FXR_M_WAVES2:
        if (!c[2] || !(c[2]->v[0] > 0.0f))
            return FALSE;
        /* fall through */
    case FXR_M_WAVES:
        if (!c[0] || !c[1] || !(c[1]->v[0] > 0.0f))
            return FALSE;
        r = r2 = c[0]->v[0] * c[0]->v[0];
        break;
    case FXR_M_WAVES_OLD:
        if (!c[0] || !c[1])
            return FALSE;
        r = r2 = c[0]->v[0] * c[0]->v[0] + 0.708f * fabsf (c[1]->v[0]);
        break;
    case FXR_M_IRIS:
        if (!c[0] || !c[1] || !c[2] || c[0]->n < 2 || !(c[2]->v[0] >= 1e-4f))
            return FALSE;
        r = (2.0f + fabsf (c[1]->v[0])) * fabsf (c[0]->v[0]) * 0.001f;
        r2 = (2.0f + fabsf (c[1]->v[0])) * fabsf (c[0]->v[1]) * 0.001f;
        break;
    default:
        return FALSE;
    }
    if (!(r >= 0.0f && r2 >= 0.0f) || !isfinite (r) || !isfinite (r2))
        return FALSE;
    *ru = r * (1.0f + 1.0f / 1024.0f) * fxr_test_reach;
    *rv = r2 * (1.0f + 1.0f / 1024.0f) * fxr_test_reach;
    if (pass->rg_mask == FXR_K_FLOW)
        *weak = FXR_WEAK_FLOW * 1.004f;   /* (the bound has 1.004 for the map's maximum) */
    else if (pass->rg_mask == FXR_K_OPACITY)
        *weak = FXR_WEAK_OPACITY;
    return TRUE;
}

/* ---- where a pass's map is strong -----------------------------------------
 * A 32 x 32 grid over the pass's target; a cell is marked when any texel
 * the map lookup of a fragment in it can take - on any mip level the
 * sampler can choose for a target of ow x oh, two texels of that level
 * around the cell, wrapped like the sampler wraps - is beyond the weak
 * threshold. Read through the sampler itself (texelFetch in a small
 * program drawn into a 32 x 32 target, one draw per level), so that
 * swizzles and the driver's own decoding of compressed maps are what the
 * pass sees too. The marked cells, packed into rectangles, are
 * pass->rg_sup; FXR_FULL = not known (no texture, an album cover, sizes
 * that are no numbers). Made once per pass and largest level. */
static __thread GLuint fxr_sup_prog, fxr_sup_fbo, fxr_sup_tex;
static __thread GLint fxr_sup_loc[6];

/* marked cells (one guint32 per row, bit = column) -> at most
 * FXR_MAX_RECTS rectangles that hold them all and do not overlap */
static void fxr_cover (const guint32 grid_in[32], FxNeed *out)
{
    typedef struct { int x0, y0, x1, y1; } R;
    R rs[32 * 16 + 1];
    int n = 0, cells = 0;
    /* runs of cells per row, a run that continues the same columns of the
     * row above extends that rectangle. A map of many small spots gives
     * too many for the search below: then the same on blocks of 2 x 2
     * cells (a block is marked when any of its cells is), 4 x 4 ... */
    for (int f = 1; f <= 32; f *= 2) {
        guint32 grid[32] = { 0 };
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++)
                if (grid_in[y] >> x & 1u) {
                    const int bx = x & ~(f - 1), by = y & ~(f - 1);
                    const guint32 bits = (f >= 32 ? 0xffffffffu : ((1u << f) - 1u)) << bx;
                    for (int k = 0; k < f; k++)
                        grid[by + k] |= bits;
                }
        n = cells = 0;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32;) {
                if (!(grid[y] >> x & 1u)) {
                    x++;
                    continue;
                }
                int e = x;
                while (e < 32 && (grid[y] >> e & 1u))
                    e++;
                cells += e - x;
                gboolean ext = FALSE;
                for (int i = 0; i < n && !ext; i++)
                    if (rs[i].y1 == y && rs[i].x0 == x && rs[i].x1 == e) {
                        rs[i].y1 = y + 1;
                        ext = TRUE;
                    }
                if (!ext)
                    rs[n++] = (R) { x, y, e, y + 1 };
                x = e;
            }
        if (n <= 32)
            break;
    }
    out->n = 0;
    if (!cells) {
        out->mode = FXR_NONE;
        return;
    }
    /* join two into their bounding box, with every rectangle that box
     * touches, taking the pair that leaves the least area; until they fit */
    while (n > FXR_MAX_RECTS) {
        int best = G_MAXINT, bn = 0;
        R bset[32 * 16 + 1];
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                R box = { MIN (rs[i].x0, rs[j].x0), MIN (rs[i].y0, rs[j].y0), MAX (rs[i].x1, rs[j].x1),
                          MAX (rs[i].y1, rs[j].y1) };
                R ns[32 * 16 + 1];
                int m = 0;
                for (int k = 0; k < n; k++)
                    if (k != i && k != j)
                        ns[m++] = rs[k];
                for (gboolean again = TRUE; again;) {
                    again = FALSE;
                    for (int k = 0; k < m; k++)
                        if (ns[k].x0 < box.x1 && box.x0 < ns[k].x1 && ns[k].y0 < box.y1 && box.y0 < ns[k].y1) {
                            box.x0 = MIN (box.x0, ns[k].x0);
                            box.y0 = MIN (box.y0, ns[k].y0);
                            box.x1 = MAX (box.x1, ns[k].x1);
                            box.y1 = MAX (box.y1, ns[k].y1);
                            ns[k] = ns[--m];
                            again = TRUE;
                            break;
                        }
                }
                ns[m++] = box;
                int area = 0;
                for (int k = 0; k < m; k++)
                    area += (ns[k].x1 - ns[k].x0) * (ns[k].y1 - ns[k].y0);
                if (area < best) {
                    best = area;
                    bn = m;
                    memcpy (bset, ns, sizeof (R) * (gsize) m);
                }
            }
        if (!bn)
            break;
        n = bn;
        memcpy (rs, bset, sizeof (R) * (gsize) n);
    }
    if (n > FXR_MAX_RECTS) {
        fxr_need_full (out);
        return;
    }
    out->mode = FXR_RECTS;
    out->n = n;
    for (int i = 0; i < n; i++) {
        out->r[i][0] = (float) rs[i].x0 / 32.0f;
        out->r[i][1] = (float) rs[i].y0 / 32.0f;
        out->r[i][2] = (float) rs[i].x1 / 32.0f;
        out->r[i][3] = (float) rs[i].y1 / 32.0f;
    }
}

/* pass->rg_sup for a target of ow x oh (see above); leaves the GL state a
 * pass expects to set itself (program, framebuffer, viewport, texture
 * unit 0) changed and tells the state cache so */
static void fxr_support (EffPass *pass, int ow, int oh)
{
    const int kind = pass->rg_mask;
    if (kind == FXR_K_NONE)
        return;
    const GLuint tex = pass->slot[1].tex;
    const int *res = pass->slot[1].res;
    if (!tex || pass->sys[1] || res[0] < 1 || res[1] < 1 || res[2] < 1 || res[3] < 1 || ow < 1 || oh < 1) {
        fxr_need_full (&pass->rg_sup);
        pass->rg_sup_levels = 1000;
        return;
    }
    /* the levels the sampler can take: lod = log2 (texels per pixel) + its
     * bias (WALPE_TEX_LOD_BIAS, at most 2), the two levels around it, one
     * more for good measure; anisotropic filtering only goes finer, within
     * the footprint the coarsest of these levels has */
    fx_gls_forget_textures ();
    glActiveTexture (GL_TEXTURE0);
    fx_gls.unit = GL_TEXTURE0;
    glBindTexture (GL_TEXTURE_2D, tex);
    GLint w0 = 0, h0 = 0;
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w0);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h0);
    if (w0 < 1 || h0 < 1) {
        fxr_need_full (&pass->rg_sup);
        pass->rg_sup_levels = 1000;
        return;
    }
    const float sx = (float) res[2] / (float) res[0], sy = (float) res[3] / (float) res[1];
    const float rho = fmaxf (sx * (float) w0 / (float) ow, sy * (float) h0 / (float) oh);
    GLint base = 0;
    GLfloat bias = 0.0f;
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
    if (epoxy_is_desktop_gl ()) {
        /* its bias now, or the one a chain built later is given
         * (walpe_gl_tex_sampling: WALPE_TEX_LOD_BIAS, -2..2) */
        const char *e = g_getenv ("WALPE_TEX_LOD_BIAS");
        glGetTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, &bias);
        if (e && *e)
            bias = fmaxf (bias, (float) CLAMP (g_ascii_strtod (e, NULL), -2.0, 2.0));
    }
    if (base != 0 || !isfinite (bias) || !isfinite (rho)) {
        fxr_need_full (&pass->rg_sup);
        pass->rg_sup_levels = 1000;
        return;
    }
    /* (counted as if the map had a mip chain even while it has none: a
     * chain built later - walpe_gl_tex_ensure_mipmaps on a shared texture -
     * must not make this stale; levels that do not exist are read from
     * level 0 with the footprint a level made from it would have) */
    const float lod = log2f (fmaxf (rho, 1e-6f)) + fmaxf (bias, 0.0f);
    const int levels = MAX (0, (int) ceilf (lod)) + 2;   /* the level above the lod, one more, and level 0 */
    if (pass->rg_sup_levels >= levels)
        return;   /* made for at least as many levels */
    pass->rg_sup_levels = levels;
    if (!fxr_sup_prog) {
        static const char *vs =
            "in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }\n";
        static const char *fs =
            "uniform sampler2D u_tex;\nuniform int u_level;\nuniform ivec2 u_size;\nuniform vec2 u_scale;\n"
            "uniform int u_kind;\nuniform int u_dil;\nout vec4 frag;\n"
            "void main(){\n"
            "  vec2 c = floor(gl_FragCoord.xy);\n"
            "  ivec2 t0 = ivec2(floor(c / 32.0 * u_scale * vec2(u_size))) - u_dil;\n"
            "  ivec2 t1 = ivec2(ceil((c + 1.0) / 32.0 * u_scale * vec2(u_size))) + u_dil;\n"
            "  float strong = 0.0;\n"
            "  for (int y = t0.y; y < t1.y; y++)\n"
            "    for (int x = t0.x; x < t1.x; x++) {\n"
            "      ivec2 p = ivec2(x, y) - u_size * ivec2(floor(vec2(x, y) / vec2(u_size)));\n"
            "      vec4 v = texelFetch(u_tex, clamp(p, ivec2(0), u_size - 1), u_level);\n"
            "      if (u_kind == 1 ? (abs(v.r - 0.498) > 0.0025 || abs(v.g - 0.498) > 0.0025) : v.r > 0.0001)\n"
            "        strong = 1.0;\n"
            "    }\n"
            "  frag = vec4(strong);\n"
            "}\n";
        const char *src[2];
        GLuint sh[2];
        for (int i = 0; i < 2; i++) {
            src[0] = i ? walpe_gl_shader_fs_header () : walpe_gl_shader_header ();
            src[1] = i ? fs : vs;
            sh[i] = glCreateShader (i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
            glShaderSource (sh[i], 2, src, NULL);
            glCompileShader (sh[i]);
        }
        fxr_sup_prog = glCreateProgram ();
        glAttachShader (fxr_sup_prog, sh[0]);
        glAttachShader (fxr_sup_prog, sh[1]);
        glBindAttribLocation (fxr_sup_prog, 0, "a_pos");
        glLinkProgram (fxr_sup_prog);
        glDetachShader (fxr_sup_prog, sh[0]);
        glDetachShader (fxr_sup_prog, sh[1]);
        glDeleteShader (sh[0]);
        glDeleteShader (sh[1]);
        GLint ok = 0;
        glGetProgramiv (fxr_sup_prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            glDeleteProgram (fxr_sup_prog);
            fxr_sup_prog = 0;
            fxr_need_full (&pass->rg_sup);
            return;
        }
        fxr_sup_loc[0] = glGetUniformLocation (fxr_sup_prog, "u_tex");
        fxr_sup_loc[1] = glGetUniformLocation (fxr_sup_prog, "u_level");
        fxr_sup_loc[2] = glGetUniformLocation (fxr_sup_prog, "u_size");
        fxr_sup_loc[3] = glGetUniformLocation (fxr_sup_prog, "u_scale");
        fxr_sup_loc[4] = glGetUniformLocation (fxr_sup_prog, "u_kind");
        fxr_sup_loc[5] = glGetUniformLocation (fxr_sup_prog, "u_dil");
        glGenTextures (1, &fxr_sup_tex);
        glBindTexture (GL_TEXTURE_2D, fxr_sup_tex);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers (1, &fxr_sup_fbo);
        glBindFramebuffer (GL_FRAMEBUFFER, fxr_sup_fbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fxr_sup_tex, 0);
        glBindTexture (GL_TEXTURE_2D, tex);
    }
    fxr_scissor_off ();
    glUseProgram (fxr_sup_prog);
    fx_bind_pass_vao ();
    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    glBindFramebuffer (GL_FRAMEBUFFER, fxr_sup_fbo);
    glViewport (0, 0, 32, 32);
    fx_gls.vp_w = fx_gls.vp_h = 0;
    fx_gls.dirty = TRUE;
    fx_gls.fixed = TRUE;
    glUniform1i (fxr_sup_loc[0], 0);
    glUniform2f (fxr_sup_loc[3], sx, sy);
    glUniform1i (fxr_sup_loc[4], kind == FXR_K_FLOW ? 1 : 2);
    guint32 grid[32] = { 0 };
    guint8 px[32 * 32 * 4];   /* (rows of 128 bytes: any pack alignment) */
    for (int l = 0; l < levels; l++) {
        GLint wl = 0, hl = 0;
        glGetTexLevelParameteriv (GL_TEXTURE_2D, l, GL_TEXTURE_WIDTH, &wl);
        glGetTexLevelParameteriv (GL_TEXTURE_2D, l, GL_TEXTURE_HEIGHT, &hl);
        if (wl >= 1 && hl >= 1) {
            /* a linear sample takes texels up to one away; one more */
            glUniform1i (fxr_sup_loc[1], l);
            glUniform2i (fxr_sup_loc[2], wl, hl);
            glUniform1i (fxr_sup_loc[5], 2);
        } else if (l <= 4) {
            /* no such level (yet): one made from level 0 would hold, per
             * texel, texels of level 0 up to 2^l away and be read two of
             * its own texels wide; half as much again for a wider filter */
            glUniform1i (fxr_sup_loc[1], 0);
            glUniform2i (fxr_sup_loc[2], w0, h0);
            glUniform1i (fxr_sup_loc[5], 3 << l);
        } else {
            for (int y = 0; y < 32; y++)
                grid[y] = 0xffffffffu;   /* too coarse to tell */
            break;
        }
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        glReadPixels (0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++)
                if (px[(y * 32 + x) * 4])
                    grid[y] |= 1u << x;
    }
    glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
    fxr_cover (grid, &pass->rg_sup);
}

/* ---- the plan of one run ------------------------------------------------ */

enum { FXR_IN = -1, FXR_EXT = -2, FXR_OUT = FX_MAX_TARGETS };

typedef struct {
    int pass;                     /* index in the chain, -1 = unused */
    gboolean copy;
    int wr;                       /* target index or FXR_OUT */
    int n_rd, rd[FX_MAX_BINDS], rd_unit[FX_MAX_BINDS];   /* targets, FXR_OUT, FXR_IN */
} FxrStep;

/* the size fx_ensure_targets () gives container c of the chain for a
 * working size of rw x rh */
static void fxr_size (const EffectChain *ch, int c, int rw, int rh, int tdiv, int *w, int *h)
{
    if (c >= 0 && c < ch->n_targets) {
        *w = MAX (1, rw / (ch->targets[c].scale * tdiv));
        *h = MAX (1, rh / (ch->targets[c].scale * tdiv));
    } else {
        *w = rw;
        *h = rh;
    }
}

/* The passes of one fx_chain_run () in order, with what each writes and
 * which textures of the run it reads - the same rules as fx_chain_run ()
 * itself. Returns the number of steps, -1 = a chain this plan does not
 * cover (swap passes). *result = the container the chain's output is in
 * (FXR_IN = the input itself). */
static int fxr_steps (EffectChain *ch, FxrStep *st, int *result)
{
    int n = 0, prev = FXR_IN;
    gboolean out_written = FALSE, to_alt = FALSE;
    *result = FXR_IN;
    for (int i = 0; i < ch->n_passes; i++) {
        EffPass *pass = &ch->passes[i];
        EffTarget *target = pass->target[0] ? fx_find_target (ch, pass->target) : NULL;
        if (pass->target[0] && !target)
            continue;
        if (pass->is_swap)
            return -1;
        FxrStep *s = &st[n];
        memset (s, 0, sizeof (*s));
        s->pass = i;
        if (pass->is_copy) {
            EffTarget *src = fx_find_target (ch, pass->copy_src);
            if (!src || !target)
                continue;
            s->copy = TRUE;
            s->wr = (int) (target - ch->targets);
            s->rd[0] = (int) (src - ch->targets);
            s->rd_unit[0] = -1;
            s->n_rd = 1;
            n++;
            continue;
        }
        int unit[FX_MAX_BINDS];
        unit[0] = prev;
        for (int u = 1; u < FX_MAX_BINDS; u++) {
            if (pass->sys[u] && fx_sys[pass->sys[u]].tex)
                unit[u] = FXR_EXT;
            else if (pass->slot[u].tex)
                unit[u] = FXR_EXT;
            else if (pass->slot[u].rt == FX_RT_FULLFRAME && fx_fullframe.tex)
                unit[u] = FXR_EXT;
            else if (pass->slot[u].rt)
                unit[u] = FXR_IN;
            else
                unit[u] = FXR_EXT;
        }
        for (int b = 0; b < pass->n_binds; b++) {
            int idx = pass->binds[b].index;
            if (idx < 0 || idx >= FX_MAX_BINDS)
                continue;
            if (!strcmp (pass->binds[b].name, "previous")) {
                unit[idx] = FXR_IN;
            } else {
                EffTarget *t = fx_find_target (ch, pass->binds[b].name);
                if (t)
                    unit[idx] = (int) (t - ch->targets);
            }
        }
        if (target) {
            s->wr = (int) (target - ch->targets);
        } else {
            if (ch->alt_out >= 0 && out_written)
                to_alt = !to_alt;
            out_written = TRUE;
            s->wr = to_alt ? ch->alt_out : FXR_OUT;
        }
        for (int u = 0; u < pass->n_dyn; u++) {
            if (pass->dyn[u].kind != FXU_TEXTURE)
                continue;
            int un = pass->dyn[u].unit;
            if (un >= FX_MAX_BINDS || unit[un] == FXR_EXT || s->n_rd >= FX_MAX_BINDS)
                continue;
            s->rd[s->n_rd] = unit[un];
            s->rd_unit[s->n_rd] = un;
            s->n_rd++;
        }
        prev = *result = s->wr;
        n++;
    }
    return n;
}

/* Walk one chain backwards: `out` = what is read of its result later;
 * stamps each pass with the region it has to fill and returns in *in what
 * the chain reads of its input. rw x rh = the working size, (iw, ih) = the
 * size of the texture the chain reads as its input. FALSE = no plan for
 * this run (the chain reads a target it is drawing into, or has swaps). */
static gboolean fxr_plan_chain (EffectChain *ch, const FxNeed *out, int rw, int rh, int iw, int ih,
                                FxNeed *in)
{
    FxrStep st[FX_MAX_PASSES];
    int result = FXR_IN;
    const int n = fxr_steps (ch, st, &result);
    const int tdiv = fx_chain_tdiv (ch);
    in->mode = FXR_NONE;
    in->n = 0;
    if (n < 0)
        return FALSE;
    if (result == FXR_IN) {
        *in = *out;   /* no pass draws: the result is the input */
        return TRUE;
    }
    FxNeed need[FX_MAX_TARGETS + 1];
    for (int c = 0; c <= FX_MAX_TARGETS; c++)
        need[c].mode = FXR_NONE, need[c].n = 0;
    need[result] = *out;
    for (int k = n - 1; k >= 0; k--) {
        const FxrStep *s = &st[k];
        EffPass *pass = &ch->passes[s->pass];
        FxNeed p = need[s->wr];
        need[s->wr].mode = FXR_NONE;
        need[s->wr].n = 0;
        if (s->copy) {
            /* a blit of the whole source: drawn as before */
            if (p.mode != FXR_NONE)
                fxr_need_full (&need[s->rd[0]]);
            continue;
        }
        if (p.mode == FXR_NONE && pass->target[0] && s->wr != result) {
            /* a target of the effect that nothing reads in this run (the
             * chain has no feedback: nothing reads it in the next one
             * either): the pass is not drawn, and reads nothing */
            pass->rg_need = p;
            pass->rg_serial = fxr_serial;
            continue;
        }
        if (p.mode == FXR_NONE)
            fxr_need_full (&p);   /* the chain's own output: as before */
        pass->rg_need = p;
        pass->rg_serial = fxr_serial;
        int ow, oh;
        fxr_size (ch, s->wr, rw, rh, tdiv, &ow, &oh);
        for (int r = 0; r < s->n_rd; r++) {
            const int c = s->rd[r];
            if (c == s->wr)
                return FALSE;     /* reads the target it draws into */
            if (fxr_dead (pass, s->rd_unit[r]) &&
                fx_target_unorm8 (c >= 0 && c < ch->n_targets ? &ch->targets[c] : NULL))
                continue;         /* multiplied by exactly zero: not a read */
            FxNeed add;
            float ru, rv, weak;
            if (p.mode == FXR_FULL || !fxr_reach (pass, s->rd_unit[r], &ru, &rv, &weak)) {
                fxr_need_full (&add);
            } else {
                /* The pixels this pass has to fill are p rounded outwards
                 * to its target's grid (fxr_pixels, the same rounding): pc
                 * = the range of their centres. Each reads within its reach
                 * of its centre, and a linear filter takes the two texels
                 * around the place read, i.e. texels that begin up to half
                 * a texel before it and end half a texel behind it (0.55:
                 * the interpolated coordinate and this arithmetic are not
                 * exact). At equal sizes that is one texel more on each
                 * side per pass. */
                int tw = iw, tht = ih;
                if (c != FXR_IN)
                    fxr_size (ch, c, rw, rh, tdiv, &tw, &tht);
                const float mu = fxr_test_margin * 0.55f / (float) MAX (1, tw);
                const float mv = fxr_test_margin * 0.55f / (float) MAX (1, tht);
                const FxNeed pc = fxr_need_centres (&p, ow, oh);
                if (weak < 1.0f && (ru > 0.0f || rv > 0.0f))
                    fxr_support (pass, ow, oh);
                if (weak < 1.0f && pass->rg_sup.mode != FXR_FULL) {
                    /* a displacement scaled by a painted map: the whole
                     * reach for the centres inside the strong cells, a
                     * fraction of it elsewhere */
                    add = fxr_need_grow (&pc, mu + ru * weak, mv + rv * weak);
                    if (pass->rg_sup.mode == FXR_RECTS && !fxr_test_no_strong) {
                        for (int a = 0; a < pc.n; a++)
                            for (int b = 0; b < pass->rg_sup.n; b++) {
                                FxNeed cut = { FXR_RECTS, 1, { { fmaxf (pc.r[a][0], pass->rg_sup.r[b][0]),
                                                                 fmaxf (pc.r[a][1], pass->rg_sup.r[b][1]),
                                                                 fminf (pc.r[a][2], pass->rg_sup.r[b][2]),
                                                                 fminf (pc.r[a][3], pass->rg_sup.r[b][3]) } } };
                                if (cut.r[0][0] > cut.r[0][2] || cut.r[0][1] > cut.r[0][3])
                                    continue;
                                const FxNeed far = fxr_need_grow (&cut, mu + ru, mv + rv);
                                fxr_need_add (&add, &far);
                            }
                    }
                } else {
                    add = fxr_need_grow (&pc, mu + ru, mv + rv);
                }
            }
            if (c == FXR_IN)
                fxr_need_add (in, &add);
            else
                fxr_need_add (&need[c], &add);
        }
    }
    return TRUE;
}

/* the size of the texture a chain leaves as its result (rw x rh, or the
 * size of the target its last drawing pass writes); FALSE = the input */
static gboolean fxr_result_size (EffectChain *ch, int rw, int rh, int *w, int *h)
{
    FxrStep st[FX_MAX_PASSES];
    int result = FXR_IN;
    if (fxr_steps (ch, st, &result) < 0 || result == FXR_IN)
        return FALSE;
    fxr_size (ch, result, rw, rh, fx_chain_tdiv (ch), w, h);
    return TRUE;
}

/* WALPE_FX_REGION_LOG=1: the first plans of this thread, one line each,
 * and one per pass of a plan (what it has to write, as a share of its
 * target; the strong part of its displacement map) */
static WalpeLazy fxr_log_on = WALPE_LAZY_UNSET;   /* every render thread asks (common/lazy.h) */
static __thread int fxr_log_lines;

static int fxr_log_wanted (void)
{
    int on = walpe_lazy_get (&fxr_log_on);
    if (on < 0)
        on = walpe_lazy_set (&fxr_log_on, g_getenv ("WALPE_FX_REGION_LOG") != NULL);
    return on;
}

static float fxr_need_share (const FxNeed *n)
{
    float a = 0.0f;
    if (n->mode != FXR_RECTS)
        return n->mode == FXR_FULL ? 1.0f : 0.0f;
    for (int r = 0; r < n->n; r++)
        a += (n->r[r][2] - n->r[r][0]) * (n->r[r][3] - n->r[r][1]);
    return a;
}

static void fxr_log_pass (int chain, int q, const EffPass *p, guint id)
{
    if (!fxr_log_wanted () || fxr_log_lines >= 400)
        return;
    fxr_log_lines++;
    g_message ("fx region:   chain %d pass %d hash 0x%016" G_GINT64_MODIFIER "x model %d: %s, %d rect(s), share %.3f; map %s %.3f",
               chain, q, p->rg_hash, (int) p->rg_model,
               p->rg_serial != id ? "not planned" : p->rg_need.mode == FXR_RECTS ? "part" :
               p->rg_need.mode == FXR_FULL ? "whole target" : "nothing",
               p->rg_serial == id && p->rg_need.mode == FXR_RECTS ? p->rg_need.n : 0,
               p->rg_serial == id ? fxr_need_share (&p->rg_need) : 1.0f,
               !p->rg_mask ? "none" : p->rg_sup_levels ? "strong part" : "not read yet",
               p->rg_mask && p->rg_sup_levels ? fxr_need_share (&p->rg_sup) : 0.0f);
}

static void fxr_log_plan (gpointer set, gboolean ok, int from, int a, int b)
{
#define lines fxr_log_lines
    if (!fxr_log_wanted () || lines >= 400)
        return;
    lines++;
    if (from < 0)
        g_message ("fx region: set %p: no plan (output kept %d, update divisor %d, or folded / fused / checked)",
                   set, a, b);
    else
        g_message ("fx region: set %p: plan %s from chain %d, result %dx%d", set, ok ? "made" : "dropped",
                   from, a, b);
#undef lines
}
