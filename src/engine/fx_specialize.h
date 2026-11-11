/* fx_specialize.h - what an effect pass costs per pixel: program sharing,
 * build-time specialisation of pass programs, cheaper equivalents of
 * stock shaders, identity passes, and the per-pass cost diagnostics.
 *
 * By default nothing of this runs: every pass gets its own program, built
 * exactly as version 0.12 built it (fxs_build_program's first branch), and
 * the hooks return at once. Items 1, 3 and 4 run with fx_specialize on (or
 * fx_quality fast), behind the comparison of item 5, and bring the
 * sharing (item 2) with them; WALPE_FX_SHARE=1 turns the sharing on alone.
 *
 * Included by scenenode_effects.c right before fx_build_pass(); everything
 * here is static. scenenode_effects.c itself only carries the hooks:
 *   fx_build_pass      fxs_build_program() instead of fx_build_program(),
 *                      fxs_after_link() behind fx_cache_uniforms()
 *   fx_chain_build     fxs_chain_built()
 *   fx_chain_free      fxs_release()
 *   fx_chain_run       fxs_chain_noop() at the top, fxs_pre_run() before
 *                      glUseProgram, fxs_post_draw() behind the draw
 *   fx_run_for_object  fxf_refresh() / fxf_settle() around the chain
 *                      loop, fxf_at() in it (fx_fuse.h)
 *
 * 1. Constant baking. A pass used to be compiled with every material
 *    constant as a uniform, although most never change while the wallpaper
 *    is shown: the instance's "constantshadervalues" (a plain value, or one
 *    bound to a user option - changing an option reloads the scene) and
 *    the shader annotation defaults. Such a uniform is now declared as a
 *    constant with the very value fx_cache_uniforms() would have uploaded
 *    ("uniform float g_Exponent;" -> "const float g_Exponent = 1.0;"), so
 *    the driver's compiler folds what the value decides: pow(x, 1.0), a
 *    term multiplied by 0 and the texture read feeding it, a branch on the
 *    value, a loop bounded by it. The arithmetic is the same - where it
 *    is defined. Where a shader leans on what a GPU does with a division
 *    by zero or a pow() of a negative number, a compiler that sees the
 *    constant may give something else (real scenes do: fx_verify.h). So a
 *    specialised program is used only after it has drawn what the generic
 *    program draws, compared on this driver with the pass's own inputs
 *    (item 5), and constants named in a pow() are not baked at all
 *    (fxs_hazards).
 *    Never baked: constants with a SceneScript or an animation in the
 *    instance, engine uniforms (time, pointer, audio, sizes, matrices,
 *    samplers), arrays, constants the transform fold reads (xf_sign).
 *    A constant that a script changes later anyway (IMaterial /
 *    setMaterialProperty reach every constant) is un-baked on the spot:
 *    the pass goes back to the generic program for that draw and gets a
 *    program without that constant behind it (fxs_pre_run, fxs_switch).
 *    A specialised source that does not compile falls back to the generic
 *    program, silently (g_debug).
 *
 * 2. Program sharing. Passes whose final sources are identical share one
 *    GL program per render thread (the base compiled one per pass: 32
 *    water-wave layers = 32 compiles of the same text). Uniform values are
 *    program state, so the pass that last used a shared program is noted
 *    and another pass puts its constants back before it draws: each
 *    exactly as the program held it for that pass after its link (the
 *    executor's own defaults for constants without a value included), or
 *    as a script wrote it since (fxs_note_values, fxs_pre_run). That is
 *    one glUniform per constant each time another pass used the program
 *    last, and the executor's per-pass uniform cache starts over for such
 *    a pass: fewer programs compiled, more GL calls per frame (the 77
 *    passes of one 4K scene: 8 programs instead of 77, 526
 *    instead of 76 uniform calls a frame). Hence not on by default.
 *
 * 3. Cheaper equivalents (fxs_rewrite): generic rewrites of WE's stock
 *    shaders, recognised by their own statements (never by a wallpaper).
 *    Exact ones are part of (1); the ones that change pixels beyond
 *    rounding only run with fx_quality = fast.
 *
 * 4. Identity passes (fxs_identity): after baking, some passes provably
 *    copy their input (opacity 1 without a mask, ...). fxs_chain_noop()
 *    tells fx_chain_run to hand the input through.
 *
 * 5. Verification (fx_verify.h): generic program first, the specialised
 *    one once six comparisons behind the pass's first 32 draws agree
 *    within 1/255. fx_picture_serial() counts every change of programs,
 *    for a renderer that keeps pictures over frames.
 *
 * Switches: config fx_specialize (default off: one program per pass,
 * exactly the 0.12 code path; on = constants baked), fx_quality
 * ("exact" default | "fast", which bakes as well); env WALPE_FX_SPEC=0|1
 * and WALPE_FX_QUALITY=exact|fast override; WALPE_FX_SHARE=1 (passes with
 * the same text share one program although nothing is baked: for an A/B
 * of load time against GL calls per frame), WALPE_FX_SPEC_VERIFY=0|n
 * (comparisons; 0 = unchecked), WALPE_FX_HOIST=0 (constants baked, nothing
 * moved to the vertex shader).
 * Diagnostics: WALPE_FX_COST=<frame> (fx_cost.h), WALPE_FX_SPEC_DUMP=dir
 * (the specialised sources as compiled).
 */

/* ------------------------------------------------------------- settings */

static gint fxs_share_on = 0;   /* WALPE_FX_SHARE=1: passes with the same text share one
                                 * program although nothing is baked. Off (the default): one
                                 * program per pass, built exactly as version 0.12 built it
                                 * (nothing of this file runs) */
static gint fxs_spec_on = 0;    /* fx_set_specialize / WALPE_FX_SPEC: bake constants (off) */
static gint fxs_quality = 0;    /* fx_set_shader_quality: 0 exact, 1 fast (bakes as well) */
static gint fxs_verify_on = 1;  /* WALPE_FX_SPEC_VERIFY=0: a specialised program is used
                                 * without comparing it with the generic one first */
static gint fxs_verify_n = 6;   /* comparisons a specialised program has to pass, behind
                                 * draws 1, 2, 4, 8, ... of its pass (WALPE_FX_SPEC_VERIFY=n) */

/* Something that the layer state does not show changed what the effects
 * draw, or whether a chain is static: one of the settings here, a pass
 * that changed programs, a run of effects that is fused or no longer is.
 * A renderer that keeps pictures over frames (frame skip, baked static
 * layers) adds fx_picture_serial() to its frame signature. */
static gint fxs_picture_gen;

guint fx_picture_serial (void)
{
    return (guint) g_atomic_int_get (&fxs_picture_gen);
}

static inline void fxs_picture_changed (void)
{
    g_atomic_int_inc (&fxs_picture_gen);
}

/* passes of this render thread that changed programs since it started:
 * fx_run_for_object compares the count around its chain loop (fx_fuse.h) */
static __thread guint fxs_switches;

void fx_set_specialize (gboolean on)
{
    const char *e = g_getenv ("WALPE_FX_SPEC");
    if (e && *e)
        on = *e != '0';
    if (g_atomic_int_get (&fxs_spec_on) != (on ? 1 : 0))
        fxs_picture_changed ();
    g_atomic_int_set (&fxs_spec_on, on ? 1 : 0);
}

void fx_set_shader_quality (const char *q)
{
    const char *e = g_getenv ("WALPE_FX_QUALITY");
    if (e && *e)
        q = e;
    int v = q && !g_ascii_strcasecmp (q, "fast") ? 1 : 0;
    if (g_atomic_int_get (&fxs_quality) != v)
        fxs_picture_changed ();
    g_atomic_int_set (&fxs_quality, v);
}

static void fxs_env (void)
{
    static gsize once;
    if (g_once_init_enter (&once)) {
        /* a process that never calls the setters (tests, tools) still
         * honours the environment */
        const char *e = g_getenv ("WALPE_FX_SPEC");
        if (e && *e)
            g_atomic_int_set (&fxs_spec_on, *e != '0');
        e = g_getenv ("WALPE_FX_SHARE");
        if (e && *e) {
            g_atomic_int_set (&fxs_share_on, *e != '0');
            if (*e != '0')
                g_message ("fx: effect passes with the same shader text share one program "
                           "(WALPE_FX_SHARE=%s)", e);
        }
        e = g_getenv ("WALPE_FX_QUALITY");
        if (e && *e)
            g_atomic_int_set (&fxs_quality, !g_ascii_strcasecmp (e, "fast") ? 1 : 0);
        e = g_getenv ("WALPE_FX_SPEC_VERIFY");
        if (e && *e) {
            int n = atoi (e);
            g_atomic_int_set (&fxs_verify_on, n > 0);
            if (n > 0)
                g_atomic_int_set (&fxs_verify_n, MIN (n, 16));
        }
        g_once_init_leave (&once, 1);
    }
}

/* constants are baked (and the exact rewrites applied): fx_specialize on,
 * or fx_quality fast, whose variants start from the baked sources */
static gboolean fxs_enabled (void)
{
    fxs_env ();
    return g_atomic_int_get (&fxs_spec_on) != 0 || g_atomic_int_get (&fxs_quality) > 0;
}

/* passes with the same source text share one program: with constants
 * baked (the specialised programs are shared by their text, the generic
 * ones a pass starts with as well), or with WALPE_FX_SHARE=1 alone */
static gboolean fxs_sharing (void)
{
    return fxs_enabled () || g_atomic_int_get (&fxs_share_on) != 0;
}

static gboolean fxs_verifying (void)
{
    return fxs_enabled () && g_atomic_int_get (&fxs_verify_on) != 0;
}

static gboolean fxs_fast (void)
{
    fxs_env ();
    return g_atomic_int_get (&fxs_quality) > 0;
}

/* programs: compiled / reused from the cache / relinked after a script
 * changed a baked constant / specialised builds that fell back */
static gint fxs_n_compiled, fxs_n_shared, fxs_n_respec, fxs_n_fallback, fxs_n_baked,
            fxs_n_identity;
/* specialised programs compared with the generic one behind a first draw:
 * equal (in use since) / not equal (the pass keeps the generic program) */
static gint fxs_n_verified, fxs_n_rejected;

void fx_verify_stats (int *verified, int *rejected)
{
    if (verified) *verified = g_atomic_int_get (&fxs_n_verified);
    if (rejected) *rejected = g_atomic_int_get (&fxs_n_rejected);
}

void fx_program_stats (int *compiled, int *shared, int *respec, int *fallback, int *baked,
                       int *identity, gboolean reset)
{
    if (compiled) *compiled = g_atomic_int_get (&fxs_n_compiled);
    if (shared) *shared = g_atomic_int_get (&fxs_n_shared);
    if (respec) *respec = g_atomic_int_get (&fxs_n_respec);
    if (fallback) *fallback = g_atomic_int_get (&fxs_n_fallback);
    if (baked) *baked = g_atomic_int_get (&fxs_n_baked);
    if (identity) *identity = g_atomic_int_get (&fxs_n_identity);
    if (reset) {
        g_atomic_int_set (&fxs_n_compiled, 0);
        g_atomic_int_set (&fxs_n_shared, 0);
        g_atomic_int_set (&fxs_n_respec, 0);
        g_atomic_int_set (&fxs_n_fallback, 0);
        g_atomic_int_set (&fxs_n_baked, 0);
        g_atomic_int_set (&fxs_n_identity, 0);
        g_atomic_int_set (&fxs_n_verified, 0);
        g_atomic_int_set (&fxs_n_rejected, 0);
    }
}

/* ------------------------------------------------------------ the tables */

/* one material constant of a pass, resolved before the build */
typedef struct {
    char uname[64];     /* uniform name */
    char key[128];      /* constantshadervalues key (annotation "material"), else the name */
    guint8 type;        /* FXS_T_* */
    guint8 n;           /* components */
    guint8 dynamic;     /* never bake: script / animation in the instance, or changed at run time */
    guint8 baked;       /* a constant in the current program */
    guint8 used;        /* named outside its declaration (else GL would not list it) */
    float v[4];
    gint16 k0;          /* probation: its entry in pass->consts (+1; 0 = none) and */
    float v0[4];        /* the value that entry had when the generic program was linked */
    guint8 hazard;      /* named in a pow(): constant, but a uniform in the exact
                         * programs (fxs_hazards); a fused pass bakes it (fx_fuse.h) */
} FxsConst;

/* a constant in the specialised program of its pass */
static inline gboolean fxs_bakeable (const FxsConst *c)
{
    return !c->dynamic && !c->hazard;
}

enum { FXS_T_FLOAT = 1, FXS_T_VEC2, FXS_T_VEC3, FXS_T_VEC4, FXS_T_INT, FXS_T_BOOL };

/* a GL program shared by every pass of this render thread whose final
 * sources are the same text */
typedef struct {
    guint64 key[2];
    GLuint program;
    int refs;
    gpointer owner;     /* the EffPass whose constants the program holds now */
    GHashTable *table;  /* the cache it lives in */
    guint8 verdict;     /* a specialised program: 0 not compared yet, 1 draws what the
                         * generic program draws, 2 does not (fxs_verify) */
} FxsProg;

/* generic (repaired, not specialised) sources of a pass, shared by the
 * passes built from the same text: what a relink starts from */
typedef struct {
    guint64 key[2];
    char *vsrc, *fsrc;
    int refs;
    GHashTable *table;
} FxsSrc;

struct FxSpec {
    FxsProg *prog;      /* the program in use */
    FxsProg *gen;       /* verification: the generic program (every constant a uniform),
                         * kept so that the pass can go back to it without a compile */
    FxsProg *cand;      /* probation: the specialised program waiting to be compared */
    guint cand_rw;      /* its FXS_RW_* */
    guint8 probation;   /* 1: drawing with the generic program; fxs_post_draw() compares
                         * the candidate behind some of the next draws, then switches to it */
    guint8 passed;      /* probation: comparisons the candidate has passed */
    guint8 flat;        /* probation: the last comparison saw one single colour
                         * (not counted: fx_verify.h) */
    guint draws;        /* probation: draws of the pass so far */
    char *name;         /* shader name, for the one message a rejection gets */
    /* per entry of pass->consts: the value the program held for it right
     * after fx_cache_uniforms (which is not always FxConst.v: the
     * executor's own defaults for constants without a value), and whether
     * a script has written the entry since. What another pass's use of
     * the shared program overwrote is put back from these (fxs_pre_run). */
    float (*lp)[4];
    guint8 *lw;
    gint16 *hz;         /* per entry: its table constant (+1) when that one is a
                         * hazard constant - a script write to it ends its being
                         * constant for a fused pass (fxs_pre_run) */
    int n_l;
    FxsSrc *src;
    FxsConst *c;
    int n;
    int n_baked;        /* entries with baked set */
    int identity;       /* 1: the pass copies its input (fxs_identity) */
    guint rewrites;     /* FXS_RW_* applied to the current program */
    char *label;        /* diagnostics only (WALPE_FX_COST) */
    char *vsrc_final;   /* diagnostics only: the sources as compiled */
    char *fsrc_final;
    GLuint count_prog;  /* diagnostics only: fetch-counting variant */
};

static __thread GHashTable *fxs_progs, *fxs_srcs;

static guint fxs_key_hash (gconstpointer p)
{
    const guint64 *k = p;
    return (guint) (k[0] ^ (k[0] >> 32));
}

static gboolean fxs_key_equal (gconstpointer a, gconstpointer b)
{
    return !memcmp (a, b, sizeof (guint64) * 2);
}

/* 128 bits of FNV-1a over both sources (two offsets): the cache key */
static void fxs_text_key (const char *a, const char *b, guint64 key[2])
{
    guint64 h0 = 0xcbf29ce484222325ull, h1 = 0x84222325cbf29ce4ull;
    const char *parts[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        for (const unsigned char *p = (const unsigned char *) parts[i]; *p; p++) {
            h0 = (h0 ^ *p) * 0x100000001b3ull;
            h1 = (h1 ^ (*p + 0x9e)) * 0x100000001b3ull;
        }
        h0 = (h0 ^ 0xff) * 0x100000001b3ull;   /* part separator */
        h1 = (h1 ^ 0x01) * 0x100000001b3ull;
    }
    key[0] = h0;
    key[1] = h1;
}

static FxsSrc *fxs_src_get (const char *vsrc, const char *fsrc)
{
    if (!fxs_srcs)
        fxs_srcs = g_hash_table_new (fxs_key_hash, fxs_key_equal);
    guint64 key[2];
    fxs_text_key (vsrc, fsrc, key);
    FxsSrc *s = g_hash_table_lookup (fxs_srcs, key);
    if (s && !s->vsrc) {
        g_hash_table_remove (fxs_srcs, key);   /* dead: see fxs_src_put */
        g_free (s);
        s = NULL;
    }
    if (!s) {
        s = g_new0 (FxsSrc, 1);
        memcpy (s->key, key, sizeof (key));
        s->vsrc = g_strdup (vsrc);
        s->fsrc = g_strdup (fsrc);
        s->table = fxs_srcs;
        g_hash_table_insert (fxs_srcs, s->key, s);
    }
    s->refs++;
    return s;
}

/* The caches belong to the render thread that built the passes; a chain
 * is freed on that thread too (scenenode_gl_free). Should another thread
 * ever drop the last reference, the entry stays in its owner's table as
 * a dead one (program 0 / no text) instead of being unlinked from a
 * table this thread must not touch; the owner replaces it on its next
 * lookup. An emptied table is freed. */
static void fxs_src_put (FxsSrc *s)
{
    if (!s || --s->refs > 0)
        return;
    g_clear_pointer (&s->vsrc, g_free);
    g_clear_pointer (&s->fsrc, g_free);
    if (s->table != fxs_srcs)
        return;
    g_hash_table_remove (fxs_srcs, s->key);
    g_free (s);
    if (g_hash_table_size (fxs_srcs) == 0)
        g_clear_pointer (&fxs_srcs, g_hash_table_unref);
}

static void fxs_prog_put (FxsProg *e, gpointer pass)
{
    if (!e)
        return;
    if (e->owner == pass)
        e->owner = NULL;
    if (--e->refs > 0)
        return;
    if (e->program)
        glDeleteProgram (e->program);
    e->program = 0;
    if (e->table != fxs_progs)
        return;
    g_hash_table_remove (fxs_progs, e->key);
    g_free (e);
    if (g_hash_table_size (fxs_progs) == 0)
        g_clear_pointer (&fxs_progs, g_hash_table_unref);
}

/* ------------------------------------------------------- source scanning */

static gboolean fxs_word_at (const char *p, const char *w, gsize *len)
{
    gsize n = strlen (w);
    if (strncmp (p, w, n) || fx_is_ident (p[n]))
        return FALSE;
    *len = n;
    return TRUE;
}

/* the type keyword of a scalar / vector constant at p (HLSL spellings
 * are macros of the prelude); 0 = something else */
static int fxs_type_at (const char *p, gsize *len)
{
    static const struct { const char *w; guint8 t; } ty[] = {
        { "float", FXS_T_FLOAT }, { "vec2", FXS_T_VEC2 }, { "vec3", FXS_T_VEC3 },
        { "vec4", FXS_T_VEC4 }, { "float2", FXS_T_VEC2 }, { "float3", FXS_T_VEC3 },
        { "float4", FXS_T_VEC4 }, { "int", FXS_T_INT }, { "bool", FXS_T_BOOL },
    };
    for (guint i = 0; i < G_N_ELEMENTS (ty); i++)
        if (fxs_word_at (p, ty[i].w, len))
            return ty[i].t;
    return 0;
}

static const char *fxs_type_glsl (int t)
{
    switch (t) {
    case FXS_T_FLOAT: return "float";
    case FXS_T_VEC2:  return "vec2";
    case FXS_T_VEC3:  return "vec3";
    case FXS_T_VEC4:  return "vec4";
    case FXS_T_INT:   return "int";
    default:          return "bool";
    }
}

/* "uniform [precision] <type> <name> ;" starting at the first non-blank
 * of a line: the type (0 = not such a declaration) and the name's span.
 * One declarator, no array, no initialiser. */
static int fxs_decl_at (const char *line, const char **name, gsize *nlen, const char **end)
{
    const char *p = fx_sp (line);
    gsize l = 0;
    if (!fxs_word_at (p, "uniform", &l))
        return 0;
    p = fx_sp (p + l);
    if (fxs_word_at (p, "lowp", &l) || fxs_word_at (p, "mediump", &l) ||
        fxs_word_at (p, "highp", &l))
        p = fx_sp (p + l);
    int t = fxs_type_at (p, &l);
    if (!t)
        return 0;
    p = fx_sp (p + l);
    if (!fx_is_ident_start (*p))
        return 0;
    const char *b = p;
    while (fx_is_ident (*p))
        p++;
    const char *e = fx_sp (p);
    if (*e != ';')
        return 0;
    *name = b;
    *nlen = (gsize) (p - b);
    *end = p;
    return t;
}

/* how often `name` stands as a whole identifier in text */
static int fxs_count_word (const char *text, const char *name)
{
    gsize n = strlen (name);
    int c = 0;
    for (const char *p = text; (p = strstr (p, name)) != NULL; p += n)
        if ((p == text || !fx_is_ident (p[-1])) && !fx_is_ident (p[n]))
            c++;
    return c;
}

/* the lines of src that are compiled (dead #if branches and directives
 * dropped, comments blanked), one per line: for declaration scans */
static char *fxs_active_lines (const char *src)
{
    char **lines = g_strsplit (src, "\n", -1);
    FxAn an;
    fx_an_init (&an);
    GString *o = g_string_new (NULL);
    for (char **l = lines; *l; l++) {
        char *code = fx_code_view (*l, &an.in_comment);
        const char *d = fx_sp (code);
        if (*d == '#')
            g_free (fx_an_directive (&an, *l, d, FALSE));
        else if (fx_an_active (&an) && *d) {
            g_string_append (o, d);
            g_string_append_c (o, '\n');
        }
        g_free (code);
    }
    fx_an_clear (&an);
    g_strfreev (lines);
    return g_string_free (o, FALSE);
}

static FxsConst *fxs_find (struct FxSpec *sp, const char *uname)
{
    for (int i = 0; sp && i < sp->n; i++)
        if (!strcmp (sp->c[i].uname, uname))
            return &sp->c[i];
    return NULL;
}

/* the instance binds this constant to a script or an animation */
static gboolean fxs_json_dynamic (JsonObject *cst, const char *k)
{
    JsonNode *n = cst && k ? json_object_get_member (cst, k) : NULL;
    if (!n || json_node_get_node_type (n) != JSON_NODE_OBJECT)
        return FALSE;
    JsonObject *o = json_node_get_object (n);
    return json_object_has_member (o, "script") || json_object_has_member (o, "animation");
}

/* The material constants of a pass, from its sources: every scalar /
 * vector uniform declared in compiled code that fx_cache_uniforms() would
 * give a value (instance constantshadervalues by the annotation's
 * "material" key or the name, else the annotation "default"; the same
 * lookups in the same order). Engine uniforms carry no annotation and no
 * instance value, so they never get here. */
static void fxs_collect (struct FxSpec *sp, EffPass *pass, const char *vsrc, const char *fsrc,
                         JsonObject *cst)
{
    char *act[2] = { fxs_active_lines (vsrc), fxs_active_lines (fsrc) };
    GArray *arr = g_array_new (FALSE, TRUE, sizeof (FxsConst));
    for (int k = 0; k < 2; k++) {
        for (const char *p = act[k]; *p;) {
            const char *eol = strchr (p, '\n');
            const char *nb = NULL, *ne = NULL;
            gsize nl = 0;
            int t = fxs_decl_at (p, &nb, &nl, &ne);
            p = eol ? eol + 1 : p + strlen (p);
            if (!t || nl >= sizeof (((FxsConst *) 0)->uname))
                continue;
            FxsConst c = { 0 };
            memcpy (c.uname, nb, nl);
            gboolean dup = FALSE;
            for (guint i = 0; i < arr->len && !dup; i++)
                dup = !strcmp (g_array_index (arr, FxsConst, i).uname, c.uname);
            if (dup)
                continue;
            /* names the engine feeds or the executor treats on its own */
            if (!strncmp (c.uname, "g_Texture", 9) || !strcmp (c.uname, "g_Time") ||
                !strcmp (c.uname, "g_Frametime") || !strcmp (c.uname, "g_Screen") ||
                !strncmp (c.uname, "g_TexelSize", 11) || !strncmp (c.uname, "g_Pointer", 9) ||
                !strcmp (c.uname, "g_ParallaxPosition") || !strcmp (c.uname, "g_Daytime") ||
                !strncmp (c.uname, "g_AudioSpectrum", 15))
                continue;
            /* as fx_cache_uniforms: the annotation of the declaration that
             * has this type, and for a name the two stages declare with
             * different types (fx_uniform_split_repair renamed the
             * fragment one) the fragment body under the declared name */
            const char *aname = c.uname, *fbody = pass->frag_body, *vbody = pass->vert_body;
            if (g_str_has_prefix (c.uname, FX_FRAG_UNIFORM_PREFIX)) {
                aname = c.uname + strlen (FX_FRAG_UNIFORM_PREFIX);
                vbody = NULL;
            }
            GLenum gt = t == FXS_T_FLOAT ? GL_FLOAT : t == FXS_T_VEC2 ? GL_FLOAT_VEC2
                      : t == FXS_T_VEC3 ? GL_FLOAT_VEC3 : t == FXS_T_VEC4 ? GL_FLOAT_VEC4
                      : t == FXS_T_BOOL ? GL_BOOL : GL_INT;
            char *mkey = fx_uniform_anno (fbody, vbody, aname, "material", gt);
            char *val = fx_const_val (cst, mkey ? mkey : aname);
            if (!val && mkey)
                val = fx_const_val (cst, aname);
            if (!val)
                val = fx_uniform_anno (fbody, vbody, aname, "default", gt);
            float f[4] = { 0, 0, 0, 1 };
            gboolean ok = val && fx_scan_floats (val, f) >= 1;
            c.type = (guint8) t;
            c.n = t == FXS_T_VEC2 ? 2 : t == FXS_T_VEC3 ? 3 : t == FXS_T_VEC4 ? 4 : 1;
            for (int i = 0; i < 4; i++)
                ok = ok && isfinite (f[i]);
            memcpy (c.v, f, sizeof (f));
            g_strlcpy (c.key, mkey ? mkey : aname, sizeof (c.key));
            c.dynamic = fxs_json_dynamic (cst, mkey ? mkey : aname) ||
                        fxs_json_dynamic (cst, aname);
            /* the transform fold reads these three from the program */
            if (pass->xf_sign && (!strcmp (c.uname, "g_Offset") || !strcmp (c.uname, "g_Scale") ||
                                  !strcmp (c.uname, "g_Direction")))
                c.dynamic = 1;
            c.used = fxs_count_word (act[0], c.uname) + fxs_count_word (act[1], c.uname) >
                     (fxs_count_word (act[0], c.uname) > 0) + (fxs_count_word (act[1], c.uname) > 0);
            g_free (mkey);
            g_free (val);
            if (ok)
                g_array_append_val (arr, c);
        }
    }
    g_free (act[0]);
    g_free (act[1]);
    sp->n = (int) arr->len;
    sp->c = (FxsConst *) g_array_free (arr, FALSE);
}

/* a float as GLSL reads it back to the same 32 bits */
static void fxs_put_float (GString *o, float f)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd (buf, sizeof (buf), "%.9g", (double) f);
    g_string_append (o, buf);
    if (!strpbrk (buf, ".eE"))
        g_string_append (o, ".0");
}

static void fxs_put_value (GString *o, const FxsConst *c)
{
    if (c->type == FXS_T_INT) {
        g_string_append_printf (o, "%d", fx_f2i (c->v[0]));
    } else if (c->type == FXS_T_BOOL) {
        g_string_append (o, fx_f2i (c->v[0]) ? "true" : "false");
    } else if (c->type == FXS_T_FLOAT) {
        fxs_put_float (o, c->v[0]);
    } else {
        g_string_append_printf (o, "%s(", fxs_type_glsl (c->type));
        for (int i = 0; i < c->n; i++) {
            if (i)
                g_string_append (o, ", ");
            fxs_put_float (o, c->v[i]);
        }
        g_string_append_c (o, ')');
    }
}

/* src with the declarations of the baked constants turned into constants;
 * line count and everything behind the name stay (annotations, #line) */
static char *fxs_bake_source (const char *src, struct FxSpec *sp)
{
    GString *o = g_string_sized_new (strlen (src) + 256);
    for (const char *p = src; *p;) {
        const char *eol = strchr (p, '\n');
        gsize n = eol ? (gsize) (eol - p) + 1 : strlen (p);
        const char *nb = NULL, *ne = NULL;
        gsize nl = 0;
        char name[64];
        /* fxs_decl_at stops at the ';': it never looks past this line */
        int t = fxs_decl_at (p, &nb, &nl, &ne);
        FxsConst *c = NULL;
        if (t && nl < sizeof (name) && ne < p + n) {
            memcpy (name, nb, nl);
            name[nl] = 0;
            c = fxs_find (sp, name);
        }
        if (c && c->baked && c->type == t) {
            g_string_append_printf (o, "const %s %s = ", fxs_type_glsl (t), name);
            fxs_put_value (o, c);
            g_string_append_len (o, ne, (gssize) (n - (gsize) (ne - p)));
        } else {
            g_string_append_len (o, p, (gssize) n);
        }
        p += n;
    }
    return g_string_free (o, FALSE);
}

/* --------------------------------------------------- cheaper equivalents */

enum { FXS_RW_NONE = 0 };

/* Generic rewrites of the specialised sources (see the list at the end of
 * this file). Returns the FXS_RW_* bits applied; *vsrc / *fsrc replaced. */
static guint fxs_rewrite (struct FxSpec *sp, EffPass *pass, char **vsrc, char **fsrc);

/* the pass copies its input texel for texel (see fxs_rewrite's notes) */
static gboolean fxs_identity (struct FxSpec *sp, EffPass *pass, const char *vsrc,
                              const char *fsrc);

/* ------------------------------------------------------------- programs */

/* compile and link without the repair machinery and without warnings:
 * the sources were repaired before they were specialised, a failure here
 * means "use the generic program". 0 = failed. */
static GLuint fxs_link_quiet (const char *vsrc, const char *fsrc, const char *shader)
{
    GLuint sh[2] = { 0, 0 };
    const char *src[2] = { vsrc, fsrc };
    for (int i = 0; i < 2; i++) {
        sh[i] = glCreateShader (i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        glShaderSource (sh[i], 1, &src[i], NULL);
        glCompileShader (sh[i]);
        GLint ok = 0;
        glGetShaderiv (sh[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog (sh[i], sizeof (log), NULL, log);
            g_debug ("fx: %s: specialised %s shader does not compile (generic program used): %.300s",
                     shader, i ? "fragment" : "vertex", log);
            glDeleteShader (sh[0]);
            if (i)
                glDeleteShader (sh[1]);
            return 0;
        }
    }
    GLuint prog = glCreateProgram ();
    glAttachShader (prog, sh[0]);
    glAttachShader (prog, sh[1]);
    glBindAttribLocation (prog, 0, "a_Position");
    glBindAttribLocation (prog, 1, "a_TexCoord");
    glLinkProgram (prog);
    glDeleteShader (sh[0]);
    glDeleteShader (sh[1]);
    GLint ok = 0;
    glGetProgramiv (prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        g_debug ("fx: %s: specialised program does not link (generic program used)", shader);
        glDeleteProgram (prog);
        return 0;
    }
    return prog;
}

static FxsProg *fxs_prog_lookup (const char *vsrc, const char *fsrc, guint64 key[2])
{
    if (!fxs_progs)
        fxs_progs = g_hash_table_new (fxs_key_hash, fxs_key_equal);
    fxs_text_key (vsrc, fsrc, key);
    FxsProg *e = g_hash_table_lookup (fxs_progs, key);
    if (e && !e->program) {
        g_hash_table_remove (fxs_progs, key);   /* dead: see fxs_prog_put */
        g_free (e);
        e = NULL;
    }
    return e;
}

static FxsProg *fxs_prog_add (const guint64 key[2], GLuint program)
{
    if (!fxs_progs)
        fxs_progs = g_hash_table_new (fxs_key_hash, fxs_key_equal);
    FxsProg *e = g_new0 (FxsProg, 1);
    memcpy (e->key, key, sizeof (e->key));
    e->program = program;
    e->table = fxs_progs;
    g_hash_table_insert (fxs_progs, e->key, e);
    return e;
}

/* WALPE_FX_SPEC_DUMP=dir (diagnostics): the sources of every specialised
 * program as compiled, NNNN-<shader>.spec.vert / .frag */
static void fxs_dump (const char *shader, const char *vsrc, const char *fsrc)
{
    const char *dir = g_getenv ("WALPE_FX_SPEC_DUMP");
    if (!dir || !*dir)
        return;
    static int sn;
    int k = g_atomic_int_add (&sn, 1);
    char *base = g_strdup (shader);
    g_strdelimit (base, "/ ", '_');
    char *fv = g_strdup_printf ("%s/%04d-%s.spec.vert", dir, k, base);
    char *ff = g_strdup_printf ("%s/%04d-%s.spec.frag", dir, k, base);
    g_file_set_contents (fv, vsrc, -1, NULL);
    g_file_set_contents (ff, fsrc, -1, NULL);
    g_free (fv);
    g_free (ff);
    g_free (base);
}

static gboolean fxs_cost_on (void);
static void fxs_cost_note_build (struct FxSpec *sp, EffPass *pass, const char *shader,
                                 const char *dbgname, const char *vsrc, const char *fsrc,
                                 gboolean hit);

static gboolean fxs_rw_inexact (guint rw);

/* the compiled statements of a source as one line of text: comments and
 * directives out, the #if branches the combos switch off out, a blank
 * between lines */
static char *fxs_scan_text (const char *src)
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
    return g_string_free (o, FALSE);
}

/* Constants that stay uniforms although nothing changes them: the ones
 * named in the arguments of a pow(). A compiler that knows the exponent
 * turns pow(x, 1.0) into x, pow(x, 2.0) into x * x, pow(x, 0.0) into 1 -
 * the same for every x a shader should feed it, but real shaders feed it
 * negative bases (a specular term, a tone curve at black), where the
 * GPU's pow gives an undefined value and the folded one a number. The
 * pictures then differ in those pixels, and only at the times the effect
 * produces them, which a comparison of a few frames (fx_verify.h) can
 * miss. The pow() stays as the generic program has it. */
static void fxs_hazards (struct FxSpec *sp, const char *vsrc, const char *fsrc)
{
    const char *src[2] = { vsrc, fsrc };
    for (int k = 0; k < 2; k++) {
        char *t = fxs_scan_text (src[k]);
        for (const char *p = t; (p = strstr (p, "pow")) != NULL; p += 3) {
            if ((p > t && fx_is_ident (p[-1])) || fx_is_ident (p[3]))
                continue;
            const char *q = p + 3;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q != '(')
                continue;
            int depth = 0;
            const char *e = q;
            for (; *e; e++) {
                if (*e == '(') {
                    depth++;
                } else if (*e == ')' && --depth == 0) {
                    break;
                }
            }
            for (const char *a = q; a < e;) {
                if (!fx_is_ident (*a) || g_ascii_isdigit (*a)) {
                    a++;
                    continue;
                }
                const char *b = a;
                while (b < e && fx_is_ident (*b))
                    b++;
                for (int i = 0; i < sp->n; i++) {
                    FxsConst *c = &sp->c[i];
                    if (strlen (c->uname) == (gsize) (b - a) && !strncmp (c->uname, a, (gsize) (b - a)))
                        c->hazard = 1;
                }
                a = b;
            }
        }
        g_free (t);
    }
}

/* specialised texts this render thread compared and rejected (the program
 * itself is gone with its last pass): not compiled a second time */
static __thread guint64 fxs_bad[64][2];
static __thread int fxs_n_bad;

static gboolean fxs_is_bad (const guint64 key[2])
{
    for (int i = 0; i < MIN (fxs_n_bad, (int) G_N_ELEMENTS (fxs_bad)); i++)
        if (fxs_bad[i][0] == key[0] && fxs_bad[i][1] == key[1])
            return TRUE;
    return FALSE;
}

static void fxs_bad_add (const guint64 key[2])
{
    if (fxs_is_bad (key))
        return;
    int i = fxs_n_bad++ % (int) G_N_ELEMENTS (fxs_bad);
    fxs_bad[i][0] = key[0];
    fxs_bad[i][1] = key[1];
}

/* the generic program of a text (compiled with the error-driven repairs
 * of fx_build_program), shared by its text like the others; no reference
 * taken */
static FxsProg *fxs_generic (char **vsrc, char **fsrc, const char *shader, const char *dbgname,
                             gboolean *hit)
{
    guint64 key[2];
    FxsProg *e = fxs_prog_lookup (*vsrc, *fsrc, key);
    if (e) {
        *hit = TRUE;
        return e;
    }
    GLuint p = fx_build_program (vsrc, fsrc, shader, dbgname);
    if (!p)
        return NULL;
    /* keyed by the text before the error-driven repairs: the next pass
     * with the same text finds it without compiling */
    e = fxs_prog_add (key, p);
    g_atomic_int_inc (&fxs_n_compiled);
    return e;
}

/* The program of a pass from its generic sources (*vsrc / *fsrc, repaired
 * in place): specialised when anything can be baked, shared when another
 * pass of this thread already linked the same text. With verification
 * (the default) a specialised program that has not been compared yet is
 * only the candidate: the pass starts with the generic program and
 * fxs_post_draw() switches behind its first draw. 0 = cannot be built. */
static GLuint fxs_build_program (EffPass *pass, char **vsrc, char **fsrc, const char *shader,
                                 const char *dbgname, JsonObject *instpass)
{
    if (!fxs_sharing ()) {
        GLuint prog = fx_build_program (vsrc, fsrc, shader, dbgname);
        if (prog && fxs_cost_on ()) {
            pass->spec = g_new0 (struct FxSpec, 1);
            fxs_cost_note_build (pass->spec, pass, shader, dbgname, *vsrc, *fsrc, FALSE);
        }
        g_atomic_int_inc (&fxs_n_compiled);
        return prog;
    }
    struct FxSpec *sp = g_new0 (struct FxSpec, 1);
    if (!fxs_enabled ()) {
        /* sharing only (WALPE_FX_SHARE=1): the program version 0.12 built for
         * this text, built once per render thread instead of once per
         * pass. Nothing about the text changes. */
        gboolean ghit = FALSE;
        FxsProg *g = fxs_generic (vsrc, fsrc, shader, dbgname, &ghit);
        if (!g) {
            g_free (sp);
            return 0;
        }
        g->refs++;
        sp->prog = g;
        if (fxs_cost_on ())
            fxs_cost_note_build (sp, pass, shader, dbgname, *vsrc, *fsrc, ghit);
        if (ghit)
            g_atomic_int_inc (&fxs_n_shared);
        sp->name = g_strdup (shader);
        pass->spec = sp;
        return g->program;
    }
    JsonObject *cst = instpass ? wpj_obj (instpass, "constantshadervalues") : NULL;
    /* the repairs first: they read declarations as WE wrote them */
    fx_prepass (vsrc, fsrc, shader);
    fxs_collect (sp, pass, *vsrc, *fsrc, cst);
    fxs_hazards (sp, *vsrc, *fsrc);
    for (int i = 0; i < sp->n; i++) {
        sp->c[i].baked = fxs_bakeable (&sp->c[i]);
        sp->n_baked += sp->c[i].baked;
    }
    GLuint prog = 0;
    guint64 key[2];
    gboolean hit = FALSE, failed = FALSE;
    if (sp->n_baked > 0) {
        char *sv = fxs_bake_source (*vsrc, sp), *sf = fxs_bake_source (*fsrc, sp);
        sp->rewrites = fxs_rewrite (sp, pass, &sv, &sf);
        /* a rewrite that changes pixels on purpose (fx_quality = fast)
         * has nothing to be compared with */
        gboolean verify = fxs_verifying () && !fxs_rw_inexact (sp->rewrites);
        FxsProg *e = fxs_prog_lookup (sv, sf, key);
        /* compared on this thread before, and not what the generic
         * program draws: the generic program below */
        gboolean bad = verify && ((e && e->verdict == 2) || fxs_is_bad (key));
        if (e) {
            hit = TRUE;
        } else if (!bad) {
            fxs_dump (shader, sv, sf);
            GLuint p = fxs_link_quiet (sv, sf, shader);
            if (p) {
                e = fxs_prog_add (key, p);
                g_atomic_int_inc (&fxs_n_compiled);
            }
        }
        if (bad) {
            hit = FALSE;
        } else if (!e) {
            g_atomic_int_inc (&fxs_n_fallback);
        } else {
            e->refs++;
            sp->prog = e;
            prog = e->program;
            sp->src = fxs_src_get (*vsrc, *fsrc);
            sp->identity = fxs_identity (sp, pass, sv, sf);
            if (fxs_cost_on ())
                fxs_cost_note_build (sp, pass, shader, dbgname, sv, sf, hit);
            if (verify) {
                gboolean ghit = FALSE;
                FxsProg *g = fxs_generic (vsrc, fsrc, shader, dbgname, &ghit);
                if (!g) {
                    /* no generic program (the base shows no effect here):
                     * nothing to compare with, and nothing to show */
                    fxs_prog_put (e, NULL);
                    fxs_src_put (sp->src);
                    sp->prog = NULL;
                    sp->src = NULL;
                    prog = 0;
                    failed = TRUE;
                } else {
                    g->refs++;
                    sp->gen = g;
                    if (e->verdict == 0) {
                        /* on probation: e keeps its reference as the candidate */
                        sp->cand = e;
                        sp->cand_rw = sp->rewrites;
                        g->refs++;
                        sp->prog = g;
                        prog = g->program;
                        sp->probation = 1;
                        for (int i = 0; i < sp->n; i++)
                            sp->c[i].baked = 0;
                        sp->n_baked = 0;
                        sp->rewrites = 0;
                        sp->identity = 0;
                    }
                }
            }
            if (prog && !sp->probation)
                g_atomic_int_add (&fxs_n_baked, sp->n_baked);
        }
        g_free (sv);
        g_free (sf);
    }
    if (!prog && !failed) {
        /* nothing to bake, or the specialised text failed: the generic
         * program, shared by its text as well */
        for (int i = 0; i < sp->n; i++)
            sp->c[i].baked = 0;
        sp->n_baked = 0;
        sp->rewrites = 0;
        FxsProg *e = fxs_generic (vsrc, fsrc, shader, dbgname, &hit);
        if (e) {
            e->refs++;
            sp->prog = e;
            prog = e->program;
            if (fxs_cost_on ())
                fxs_cost_note_build (sp, pass, shader, dbgname, *vsrc, *fsrc, hit);
        }
    }
    if (hit)
        g_atomic_int_inc (&fxs_n_shared);
    if (!prog) {
        g_free (sp->c);
        g_free (sp->label);
        g_free (sp->vsrc_final);
        g_free (sp->fsrc_final);
        g_free (sp);
        return 0;
    }
    sp->name = g_strdup (shader);
    pass->spec = sp;
    return prog;
}

/* name of the active uniform at a location (the first array element) */
static gboolean fxs_uniform_name (GLuint program, GLint loc, char *name, gsize size)
{
    GLint n = 0;
    name[0] = 0;
    if (loc < 0)
        return FALSE;
    glGetProgramiv (program, GL_ACTIVE_UNIFORMS, &n);
    for (GLint u = 0; u < n; u++) {
        GLint sz = 0;
        GLenum type = 0;
        glGetActiveUniform (program, (GLuint) u, (GLsizei) size, NULL, &sz, &type, name);
        char *br = strchr (name, '[');
        if (br)
            *br = 0;
        if (glGetUniformLocation (program, name) == loc)
            return TRUE;
    }
    name[0] = 0;
    return FALSE;
}

/* the constants that are no uniforms any more join pass->consts (location
 * -1: glUniform ignores it), in table order at the end, so scripts and
 * the probe read and write them like the others */
static void fxs_append_baked (EffPass *pass)
{
    struct FxSpec *sp = pass->spec;
    int add = 0;
    for (int i = 0; i < sp->n; i++)
        add += sp->c[i].baked && sp->c[i].used;
    if (!add)
        return;
    pass->consts = g_renew (FxConst, pass->consts, pass->n_consts + add);
    for (int i = 0; i < sp->n; i++) {
        const FxsConst *c = &sp->c[i];
        if (!c->baked || !c->used)
            continue;
        FxConst *fc = &pass->consts[pass->n_consts++];
        memset (fc, 0, sizeof (*fc));
        g_strlcpy (fc->key, c->key, sizeof (fc->key));
        fc->loc = -1;
        fc->type = c->type == FXS_T_FLOAT ? GL_FLOAT : c->type == FXS_T_VEC2 ? GL_FLOAT_VEC2
                 : c->type == FXS_T_VEC3 ? GL_FLOAT_VEC3 : c->type == FXS_T_VEC4 ? GL_FLOAT_VEC4
                 : c->type == FXS_T_BOOL ? GL_BOOL : GL_INT;
        fc->n = c->n;
        memcpy (fc->v, c->v, sizeof (fc->v));
    }
}

/* probation starts (the generic program was just linked into the pass):
 * note where each constant lives in pass->consts and what it holds, so
 * that fxs_verify() sees a value a script wrote in between */
static void fxs_snapshot (EffPass *pass)
{
    struct FxSpec *sp = pass->spec;
    for (int i = 0; i < sp->n; i++)
        sp->c[i].k0 = 0;
    for (int k = 0; k < pass->n_consts && k < G_MAXINT16 - 1; k++) {
        char nm[128];
        if (!fxs_uniform_name (pass->program, pass->consts[k].loc, nm, sizeof (nm)))
            continue;
        FxsConst *c = fxs_find (sp, nm);
        if (!c)
            continue;
        c->k0 = (gint16) (k + 1);
        memcpy (c->v0, pass->consts[k].v, sizeof (c->v0));
    }
}

/* The program holds this pass's constants right now (fx_cache_uniforms,
 * or the carry-over of fxs_switch, just put them there): note them, for
 * the day another pass has used the program in between. An entry that is
 * still to be uploaded (dirty) will hold FxConst.v. */
static void fxs_note_values (EffPass *pass)
{
    struct FxSpec *sp = pass->spec;
    g_free (sp->lp);
    g_free (sp->lw);
    g_clear_pointer (&sp->hz, g_free);
    sp->n_l = pass->n_consts;
    sp->lp = g_malloc0 (sizeof (*sp->lp) * (gsize) MAX (pass->n_consts, 1));
    sp->lw = g_malloc0 ((gsize) MAX (pass->n_consts, 1));
    gboolean any_hazard = FALSE;
    for (int i = 0; i < sp->n; i++)
        any_hazard |= sp->c[i].hazard && !sp->c[i].dynamic;
    if (any_hazard)
        sp->hz = g_new0 (gint16, MAX (pass->n_consts, 1));
    for (int k = 0; k < pass->n_consts; k++) {
        const FxConst *fc = &pass->consts[k];
        memcpy (sp->lp[k], fc->v, sizeof (sp->lp[k]));
        if (fc->loc < 0)
            continue;
        char nm[128];
        if (sp->hz && fxs_uniform_name (pass->program, fc->loc, nm, sizeof (nm))) {
            FxsConst *c = fxs_find (sp, nm);
            if (c && c->hazard && !c->dynamic)
                sp->hz[k] = (gint16) (c - sp->c + 1);
        }
        if (fc->dirty) {
            sp->lw[k] = 1;
        } else if (fc->type == GL_INT || fc->type == GL_BOOL) {
            GLint iv[4] = { 0 };
            glGetUniformiv (pass->program, fc->loc, iv);
            sp->lp[k][0] = (float) iv[0];
        } else {
            GLfloat fv[4] = { 0 };
            glGetUniformfv (pass->program, fc->loc, fv);
            memcpy (sp->lp[k], fv, sizeof (fv));
        }
    }
}

/* behind fx_cache_uniforms() */
static void fxs_after_link (EffPass *pass)
{
    struct FxSpec *sp = pass->spec;
    if (!sp || !sp->prog)
        return;
    sp->prog->owner = pass;   /* fx_cache_uniforms just loaded this pass's values */
    fxs_append_baked (pass);
    fxs_note_values (pass);
    if (sp->probation)
        fxs_snapshot (pass);
}

/* The pass changes programs: to the generic one (to_generic), or to the
 * one that has every constant baked that is not marked dynamic. A baked
 * constant that a script changed since the last run is marked dynamic
 * here (a uniform from now on). Whatever the old program held is carried
 * over by uniform name - the shader bodies and the instance json are long
 * gone. FALSE = the new program could not be had; the pass is as before. */
static gboolean fxs_switch (EffPass *pass, gboolean to_generic)
{
    struct FxSpec *sp = pass->spec;
    FxConst *old = pass->consts;
    int n_old = pass->n_consts, nb = 0;
    GLuint old_prog = pass->program;
    for (int i = 0; i < sp->n; i++)
        nb += sp->c[i].baked && sp->c[i].used;
    /* names of the old entries: uniforms by location, the baked ones (at
     * the end, in table order) from the table */
    char (*names)[64] = g_malloc0 (sizeof (*names) * (gsize) MAX (n_old, 1));
    for (int k = 0; k < n_old - nb; k++) {
        char nm[128];
        if (fxs_uniform_name (old_prog, old[k].loc, nm, sizeof (nm)))
            g_strlcpy (names[k], nm, sizeof (names[k]));
    }
    int k = n_old - nb;
    for (int i = 0; i < sp->n && k >= 0 && k < n_old; i++) {
        FxsConst *c = &sp->c[i];
        if (!c->baked || !c->used)
            continue;
        g_strlcpy (names[k], c->uname, sizeof (names[k]));
        if (old[k].dirty || memcmp (old[k].v, c->v, sizeof (c->v)))
            c->dynamic = 1;       /* changed: a uniform from now on */
        memcpy (c->v, old[k].v, sizeof (c->v));
        k++;
    }
    guint8 *was = g_malloc0 ((gsize) MAX (sp->n, 1));
    int was_n = sp->n_baked;
    sp->n_baked = 0;
    for (int i = 0; i < sp->n; i++) {
        was[i] = sp->c[i].baked;
        sp->c[i].baked = !to_generic && fxs_bakeable (&sp->c[i]);
        sp->n_baked += sp->c[i].baked;
    }
    char *sv = sp->n_baked ? fxs_bake_source (sp->src->vsrc, sp) : g_strdup (sp->src->vsrc);
    char *sf = sp->n_baked ? fxs_bake_source (sp->src->fsrc, sp) : g_strdup (sp->src->fsrc);
    guint rw = sp->n_baked ? fxs_rewrite (sp, pass, &sv, &sf) : 0;
    guint64 key[2];
    FxsProg *e = fxs_prog_lookup (sv, sf, key);
    if (!e) {
        GLuint p = fxs_link_quiet (sv, sf, "relinked pass");
        if (p) {
            e = fxs_prog_add (key, p);
            g_atomic_int_inc (&fxs_n_compiled);
        }
    }
    if (!e) {
        /* keep drawing with the old program: the changed constants stay
         * what they were baked as. Reported once: the entries are no
         * longer dirty and the table is as before. */
        g_warning ("fx: a pass could not be relinked after a script changed a baked constant");
        for (int i = 0; i < n_old; i++)
            if (old[i].loc < 0)
                old[i].dirty = FALSE;
        for (int i = 0; i < sp->n; i++)
            sp->c[i].baked = was[i];
        sp->n_baked = was_n;
        g_free (was);
        g_free (names);
        g_free (sv);
        g_free (sf);
        return FALSE;
    }
    g_free (was);
    sp->rewrites = rw;
    e->refs++;
    FxsProg *old_ent = sp->prog;
    gboolean mine = old_ent->owner == pass;
    sp->prog = e;
    pass->program = e->program;
    sp->identity = sp->n_baked ? fxs_identity (sp, pass, sv, sf) : 0;
    if (sp->fsrc_final) {
        g_free (sp->fsrc_final);
        sp->fsrc_final = g_strdup (sf);
        g_free (sp->vsrc_final);
        sp->vsrc_final = g_strdup (sv);
    }
    g_free (pass->dyn);
    g_free (pass->dyn_last);    /* fx_cache_uniforms makes both anew */
    pass->dyn = NULL;
    pass->dyn_last = NULL;
    pass->consts = NULL;
    pass->n_dyn = pass->n_consts = 0;
    fx_cache_uniforms (pass);   /* bodies are gone: names only, values follow */
    glUseProgram (pass->program);
    for (int j = 0; j < pass->n_consts; j++) {
        FxConst *fc = &pass->consts[j];
        char nm[128];
        if (!fxs_uniform_name (pass->program, fc->loc, nm, sizeof (nm)))
            continue;
        for (int o = 0; o < n_old; o++) {
            if (strcmp (names[o], nm))
                continue;
            GLint loc = fc->loc;
            *fc = old[o];
            fc->loc = loc;
            if (old[o].loc >= 0 && !mine) {
                /* a shared program that holds another pass's values: this
                 * pass's are what the program held after its link, or
                 * what a script wrote since (fxs_note_values) */
                const float *pv = (o < sp->n_l && !sp->lw[o]) ? sp->lp[o] : old[o].v;
                if (fc->type == GL_INT || fc->type == GL_BOOL) glUniform1i (loc, fx_f2i (pv[0]));
                else if (fc->type == GL_FLOAT) glUniform1f (loc, pv[0]);
                else if (fc->type == GL_FLOAT_VEC2) glUniform2f (loc, pv[0], pv[1]);
                else if (fc->type == GL_FLOAT_VEC3) glUniform3f (loc, pv[0], pv[1], pv[2]);
                else glUniform4f (loc, pv[0], pv[1], pv[2], pv[3]);
            } else if (old[o].loc >= 0) {
                /* what the old program really held (also where the
                 * executor's fallbacks, not fc->v, decided the value) */
                if (fc->type == GL_INT || fc->type == GL_BOOL) {
                    GLint iv[4] = { 0 };
                    glGetUniformiv (old_prog, old[o].loc, iv);
                    glUniform1i (loc, iv[0]);
                } else {
                    GLfloat fv[4] = { 0 };
                    glGetUniformfv (old_prog, old[o].loc, fv);
                    if (fc->type == GL_FLOAT) glUniform1f (loc, fv[0]);
                    else if (fc->type == GL_FLOAT_VEC2) glUniform2f (loc, fv[0], fv[1]);
                    else if (fc->type == GL_FLOAT_VEC3) glUniform3f (loc, fv[0], fv[1], fv[2]);
                    else glUniform4f (loc, fv[0], fv[1], fv[2], fv[3]);
                }
            } else {
                fc->dirty = TRUE;   /* was baked: the run uploads fc->v */
            }
            break;
        }
    }
    fxs_prog_put (old_ent, pass);
    sp->prog->owner = pass;
    fxs_append_baked (pass);
    /* (entries a script wrote before keep saying so: their value is
     * FxConst.v whatever the program held at its link) */
    guint8 *wrote = g_malloc0 ((gsize) MAX (pass->n_consts, 1));
    for (int j = 0; j < pass->n_consts; j++) {
        char nm[128];
        if (!fxs_uniform_name (pass->program, pass->consts[j].loc, nm, sizeof (nm)))
            continue;
        for (int o = 0; o < n_old && o < sp->n_l; o++)
            if (!strcmp (names[o], nm))
                wrote[j] = sp->lw[o];
    }
    fxs_note_values (pass);
    for (int j = 0; j < pass->n_consts; j++)
        sp->lw[j] |= wrote[j];
    g_free (wrote);
    fx_vertex_setup (pass);
    fxs_switches++;
    g_free (old);
    g_free (names);
    g_free (sv);
    g_free (sf);
    return TRUE;
}

/* Before a pass draws: a shared program holds another pass's constants ->
 * mark this pass's for upload. A script changed a baked constant -> the
 * pass leaves its program: for the generic one, to have what is left
 * baked again once that is compared (fxs_verify, behind this draw); with
 * the verification off, for a program relinked without that constant. */
static inline void fxs_pre_run (EffectChain *ch, EffPass *pass)
{
    struct FxSpec *sp = pass->spec;
    if (!sp || !sp->prog)
        return;
    if (sp->n_baked) {
        gboolean changed = FALSE;
        for (int k = 0; k < pass->n_consts; k++)
            changed |= pass->consts[k].loc < 0 && pass->consts[k].dirty;
        if (G_UNLIKELY (changed)) {
            gboolean back = fxs_verifying () && sp->gen != NULL;
            if (fxs_switch (pass, back)) {
                g_atomic_int_inc (&fxs_n_respec);
                if (back) {
                    sp->probation = 1;
                    sp->draws = 0;
                    sp->passed = 0;
                    fxs_snapshot (pass);
                }
                ch->cacheable = -1;     /* fx_chain_static: other uniforms now */
                ch->dyn = -1;           /* fx_chain_dyn: likewise */
                fxs_picture_changed ();
            }
        }
    }
    /* an entry about to be uploaded was written by a script (or carried
     * over by a switch): FxConst.v is its value from now on */
    for (int k = 0; k < pass->n_consts && k < sp->n_l; k++) {
        if (!pass->consts[k].dirty)
            continue;
        sp->lw[k] = 1;
        if (G_UNLIKELY (sp->hz && sp->hz[k] && !sp->c[sp->hz[k] - 1].dynamic)) {
            /* no longer constant: a fused pass that has it baked is
             * rebuilt without this effect (fxf_settle / fxf_refresh) */
            sp->c[sp->hz[k] - 1].dynamic = 1;
            fxs_switches++;
        }
    }
    if (sp->prog->owner != pass) {
        /* another pass used the program last: put this pass's constants
         * back, each exactly as the program held it for this pass */
        glUseProgram (pass->program);
        for (int k = 0; k < pass->n_consts; k++) {
            const FxConst *fc = &pass->consts[k];
            if (fc->loc < 0 || fc->dirty)
                continue;       /* dirty: the run uploads FxConst.v itself */
            const float *v = (k < sp->n_l && !sp->lw[k]) ? sp->lp[k] : fc->v;
            switch (fc->type) {
            case GL_FLOAT:      glUniform1f (fc->loc, v[0]); break;
            case GL_FLOAT_VEC2: glUniform2f (fc->loc, v[0], v[1]); break;
            case GL_FLOAT_VEC3: glUniform3f (fc->loc, v[0], v[1], v[2]); break;
            case GL_FLOAT_VEC4: glUniform4f (fc->loc, v[0], v[1], v[2], v[3]); break;
            default:            glUniform1i (fc->loc, fx_f2i (v[0])); break;
            }
        }
        sp->prog->owner = pass;
    }
}

/* fx_chain_free: TRUE = the program was the cache's (do not delete it) */
static gboolean fxs_release (EffPass *pass)
{
    struct FxSpec *sp = pass->spec;
    if (!sp)
        return FALSE;
    gboolean shared = sp->prog != NULL;
    fxs_prog_put (sp->prog, pass);
    fxs_prog_put (sp->gen, pass);
    fxs_prog_put (sp->cand, pass);
    fxs_src_put (sp->src);
    g_free (sp->name);
    g_free (sp->lp);
    g_free (sp->lw);
    g_free (sp->hz);
    g_free (sp->c);
    g_free (sp->label);
    g_free (sp->vsrc_final);
    g_free (sp->fsrc_final);
    if (sp->count_prog)
        glDeleteProgram (sp->count_prog);
    g_free (sp);
    pass->spec = NULL;
    return shared;
}

#include "fx_cost.h"
#include "fx_rewrite.h"
#include "fx_verify.h"
