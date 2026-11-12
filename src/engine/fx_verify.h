/* fx_verify.h - a specialised pass program is used only after it has drawn
 * what the generic program draws (included by fx_specialize.h).
 *
 * Baking a constant lets the driver's compiler evaluate at compile time
 * what the GPU evaluated per pixel. Where a shader leans on arithmetic the
 * GLSL specification leaves undefined, the two need not agree: a division
 * by a constant that is zero (smoothstep with equal edges: WE's pulse with
 * thresholds "0 0" is a step function on a GPU, llvmpipe's compiler makes
 * it an undefined value and the layer goes black), pow() of a negative
 * base with an exponent the compiler turns into multiplications, 0 * inf.
 * No source scan finds all of these, and compilers differ. So:
 *
 *  - a pass whose specialised program has not been compared yet starts
 *    with the generic program (fxs_build_program: "probation"), which is
 *    what the 0.12 engine drew;
 *  - behind draws 1, 2, 4, 8, 16 and 32 of the pass (fxs_verify_n = 6
 *    comparisons), with the pass's real textures and constants still
 *    bound, both programs draw the pass FXS_VERIFY_PROBES more times each
 *    into a scratch target (at most FXS_VERIFY_MAX pixels a side per
 *    picture, one read-back for all of them) and the pictures are
 *    compared: at most FXS_VERIFY_TOL/255 apart in every channel of every
 *    pixel, every time -> the pass switches to the specialised program
 *    and draws with it from the next frame on; else it keeps the generic
 *    program for good and says so once (g_message). A pass of a static
 *    chain is drawn once and then served from the cache: it simply stays
 *    generic;
 *  - what the scene changes from frame to frame - g_Time, the pointer,
 *    the audio spectrum, the frame time, the time of day - is not taken
 *    from the frame the comparison happens in but from a fixed list of
 *    probes (fxs_probe_apply): 4 per comparison, 24 for the default six,
 *    with times from 0 to about an hour. The comparisons used to run at
 *    the times of the pass's first 32 draws: the first half second of the
 *    scene at 60 fps, at moments that depend on how fast the frames came,
 *    so a difference that shows at one phase of an effect (a ripple that
 *    sits a fraction of a pixel elsewhere) was found in one run and not
 *    in the next, and one that only shows later was never found. Now the
 *    verdict is a function of the two program texts, the pass's constants
 *    and the textures bound at those six draws; it is the same in every
 *    run for a pass whose inputs stand still (a layer picture, masks,
 *    noise). A pass that reads the moving output of an earlier effect, a
 *    video frame or the frame under a compose layer is still compared on
 *    whatever pixels those hold at its draws 1, 2, 4, ... 32;
 *  - a pass that draws into a float target (fbos "r16f", "rg1616f") is
 *    compared in a float scratch target: an 8-bit one clamps what such a
 *    target is declared float for;
 *  - a comparison in which the generic program draws one single colour
 *    (a video that has not delivered a frame yet, the black under a
 *    compose layer in the first frame, a layer a script still keeps
 *    transparent) says nothing about the shader - a baked division by
 *    zero multiplies black as well as the right value does - and is not
 *    counted: the pass stays on probation and is asked again behind draws
 *    64, 128, ... (each a power of two: next to nothing while the layer
 *    stays empty); the first comparison that sees a picture starts the
 *    count of draws anew, so the other five follow within 32 draws. A
 *    pass that still draws one colour behind draw FXS_VERIFY_FLAT_DRAWS
 *    keeps the generic program and lets its candidate go: there are
 *    passes whose output is one colour for good (a circle-of-confusion
 *    value that is the same all over the picture), and those were
 *    compared again behind draw 8192, 16384, ... for as long as the
 *    wallpaper ran - a scratch target, a read-back and a buffer of
 *    2 MiB each at 1080p, every time, for a program that never came;
 *  - the verdict belongs to the program text: passes that share it are
 *    not compared again, and a rejected text is not compiled again on
 *    this render thread;
 *  - a script that changes a baked constant sends the pass back to the
 *    generic program (fxs_pre_run); what is left is baked again behind
 *    that draw, after the same comparison.
 *
 * What it does not prove: equality at a time, pointer position or
 * spectrum between the probes (a difference confined to a narrow part of
 * an effect's cycle: with 24 probe times one that shows during a tenth of
 * the cycle is missed about once in twelve, during a quarter once in a
 * thousand), or for other input pixels than those of the compared draws
 * (a pixel that only later reaches a value where the generic program
 * divides by zero). The one construct found to do that in real scenes,
 * pow() of a negative base, is kept out of the baking altogether
 * (fxs_hazards).
 *
 * WALPE_FX_SPEC_VERIFY=0 uses specialised programs unchecked, from the
 * first frame (diagnostics; what the tests of the baking itself use);
 * =n sets the number of comparisons (default 6).
 * The rewrites of fx_quality = fast that change pixels on purpose are not
 * compared.
 */
#include "memreport.h"   /* walpe_memc_inc: comparisons made, for `walpe-ctl mem` */

#define FXS_VERIFY_MAX 256      /* pixels a side of one compared picture */
#define FXS_VERIFY_PROBES 4     /* pictures per program and comparison */
#define FXS_VERIFY_TOL 1
/* the last draw behind which a pass that has drawn one colour so far is
 * compared (a power of two; about a minute of drawing at 60 frames a
 * second) */
#define FXS_VERIFY_FLAT_DRAWS 4096

static gboolean fxs_rw_inexact (guint rw)
{
    return (rw & (FXS_RW_BLUR | FXS_RW_CAST)) != 0;
}

/* Probe k of what a scene changes from frame to frame, set in `prog`
 * (which is in use) for every such uniform it has: the names are the
 * ones fx_cache_uniforms knows as FXU_TIME, FXU_FRAMETIME, FXU_POINTER,
 * FXU_DAYTIME and FXU_AUDIO. Times: 0 (the first frame), then a geometric
 * series from 0.41 s to about 58 minutes (inside what fx_shader_time
 * hands out), so that whatever speed an effect multiplies g_Time with,
 * the probes land all over its cycle. Pointer and time of day: a
 * low-discrepancy sequence starting at the middle / at noon. Spectrum:
 * silence, full, a ramp, noise. */
static void fxs_probe_apply (GLuint prog, guint k)
{
    static const char *const ptr[] = { "g_PointerPosition", "g_PointerPositionLast", "g_ParallaxPosition" };
    static const float dts[4] = { 1.0f / 60, 1.0f / 30, 1.0f / 144, 1.0f / 24 };
    GLint l;
    if ((l = glGetUniformLocation (prog, "g_Time")) >= 0) {
        const guint j = k % 24;
        glUniform1f (l, k == 0 ? 0.0f : (0.41f + 0.07f * (float) (k / 24)) * powf (1.4826f, (float) j));
    }
    if ((l = glGetUniformLocation (prog, "g_Frametime")) >= 0)
        glUniform1f (l, dts[k % 4]);
    for (guint i = 0; i < G_N_ELEMENTS (ptr); i++)
        if ((l = glGetUniformLocation (prog, ptr[i])) >= 0) {
            const float x = 0.5f + 0.618034f * (float) k, y = 0.5f + 0.754878f * (float) k;
            glUniform2f (l, x - floorf (x), y - floorf (y));
        }
    if ((l = glGetUniformLocation (prog, "g_Daytime")) >= 0) {
        const float d = 0.5f + 0.381966f * (float) k;
        glUniform1f (l, d - floorf (d));
    }
    for (int a = 0; a < 6; a++) {
        static const char *const size[3] = { "16", "32", "64" };
        char name[40];
        g_snprintf (name, sizeof (name), "g_AudioSpectrum%s%s", size[a % 3], a < 3 ? "Left" : "Right");
        if ((l = glGetUniformLocation (prog, name)) < 0)
            continue;
        const int n = 16 << (a % 3);
        float v[64];
        for (int i = 0; i < n; i++) {
            guint32 hsh = (guint32) (k * 2654435761u) ^ (guint32) ((i + 64 * a) * 40503u);
            hsh ^= hsh >> 15;
            hsh *= 2246822519u;
            hsh ^= hsh >> 13;
            v[i] = k % 4 == 0 ? 0.0f : k % 4 == 1 ? 1.0f : k % 4 == 2 ? (float) (i + 1) / (float) n
                 : (float) (hsh & 0xffff) / 65535.0f;
        }
        glUniform1fv (l, n, v);
    }
}

/* The pass was just drawn with its (generic) program: draw it again with
 * that and with `cand` into a scratch target, FXS_VERIFY_PROBES times
 * each (probes round * FXS_VERIFY_PROBES + 0..: fxs_probe_apply), and
 * compare. Everything the executor bound for the draw is still bound and
 * is left as it was, except the per-frame uniforms of the generic
 * program, which the pass sends again with its next draw. */
static gboolean fxs_compare (EffPass *pass, GLuint cand, guint round, int *maxdiff, gboolean *flat)
{
    *flat = FALSE;
    walpe_memc_inc (WALPE_MEMC_FX_COMPARE);   /* (`walpe-ctl mem`: how often this ran) */
    GLint vp[4] = { 0 }, fbo = 0, rfbo = 0, tex = 0, ctype = 0;
    GLfloat cc[4] = { 0 };
    glGetIntegerv (GL_VIEWPORT, vp);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &tex);
    glGetFloatv (GL_COLOR_CLEAR_VALUE, cc);
    GLboolean blend = glIsEnabled (GL_BLEND), scissor = glIsEnabled (GL_SCISSOR_TEST);
    /* a float target keeps values an 8-bit one clamps: compare those */
    if (fbo)
        glGetFramebufferAttachmentParameteriv (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &ctype);
    const gboolean flt = ctype == GL_FLOAT;
    const int P = FXS_VERIFY_PROBES;
    int w = CLAMP (vp[2], 1, FXS_VERIFY_MAX), h = CLAMP (vp[3], 1, FXS_VERIFY_MAX);
    /* all draws in one target, the generic program's in the lower row,
     * the candidate's above: one read-back (one stall) */
    const gsize row = (gsize) w * (gsize) P * 4;
    void *px = g_malloc (row * (gsize) h * 2 * (flt ? sizeof (GLfloat) : 1));
    GLuint t = 0, f = 0;
    glGenTextures (1, &t);
    glBindTexture (GL_TEXTURE_2D, t);
    glTexImage2D (GL_TEXTURE_2D, 0, flt ? GL_RGBA16F : GL_RGBA8, w * P, h * 2, 0, GL_RGBA,
                  flt ? GL_FLOAT : GL_UNSIGNED_BYTE, NULL);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture (GL_TEXTURE_2D, (GLuint) tex);
    glGenFramebuffers (1, &f);
    glBindFramebuffer (GL_FRAMEBUFFER, f);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    gboolean ok = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    int md = 255;
    if (ok) {
        GLint pack = 4;
        glGetIntegerv (GL_PACK_ALIGNMENT, &pack);
        glDisable (GL_BLEND);
        glDisable (GL_SCISSOR_TEST);
        glViewport (0, 0, w * P, h * 2);
        glClearColor (0, 0, 0, 0);
        glClear (GL_COLOR_BUFFER_BIT);
        for (int k = 0; k < P; k++) {
            fxs_probe_apply (pass->program, round * (guint) P + (guint) k);
            glViewport (k * w, 0, w, h);
            glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        }
        /* every uniform the candidate kept exists in the generic program,
         * with the value this draw used */
        fxs_cost_copy_uniforms (pass->program, cand);
        for (int k = 0; k < P; k++) {
            fxs_probe_apply (cand, round * (guint) P + (guint) k);
            glViewport (k * w, h, w, h);
            glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        }
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        if (flt)    /* (a context that cannot read floats back must not pass for "equal") */
            while (glGetError () != GL_NO_ERROR)
                ;
        glReadPixels (0, 0, w * P, h * 2, GL_RGBA, flt ? GL_FLOAT : GL_UNSIGNED_BYTE, px);
        if (flt && glGetError () != GL_NO_ERROR)
            ok = FALSE;
        glPixelStorei (GL_PACK_ALIGNMENT, pack);
        md = 0;
        if (!ok) {
            md = 255;
        } else if (flt) {
            /* as far apart as FXS_VERIFY_TOL steps of an 8-bit channel,
             * or that share of a large value; NaN equals only NaN */
            const GLfloat *g = px, *c = g + row * (gsize) h;
            for (gsize i = 0; i < row * (gsize) h; i++) {
                const float a = g[i], b = c[i];
                if (isnan (a) && isnan (b))
                    continue;
                const float d = fabsf (a - b), m = MAX (1.0f, MAX (fabsf (a), fabsf (b)));
                const int steps = d <= 1.0f * m ? (int) (d / m * 255.0f + 0.25f) : 255;   /* (NaN: 255) */
                if (steps > md)
                    md = steps;
            }
        } else {
            const guint8 *g = px, *c = g + row * (gsize) h;
            for (gsize i = 0; i < row * (gsize) h; i++) {
                int d = abs ((int) g[i] - (int) c[i]);
                if (d > md)
                    md = d;
            }
        }
        /* one colour in all the generic program's pictures? */
        const gsize psz = 4 * (flt ? sizeof (GLfloat) : 1), npx = row / 4 * (gsize) h;
        gboolean one = ok;
        for (gsize i = 1; one && i < npx; i++)
            one = !memcmp (px, (const guint8 *) px + i * psz, psz);
        *flat = one;
    }
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, (GLuint) fbo);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, (GLuint) rfbo);
    glDeleteFramebuffers (1, &f);
    glDeleteTextures (1, &t);
    glViewport (vp[0], vp[1], vp[2], vp[3]);
    glClearColor (cc[0], cc[1], cc[2], cc[3]);
    if (blend)
        glEnable (GL_BLEND);
    if (scissor)
        glEnable (GL_SCISSOR_TEST);
    glUseProgram (pass->program);
    /* the generic program holds the last probe: the pass sends the
     * frame's own values with its next draw (the spectrum always is) */
    for (int u = 0; u < pass->n_dyn; u++) {
        const int kind = pass->dyn[u].kind;
        /* (FXU_POINTER_LAST: g_PointerPositionLast of a simulation chain,
         * fx_sim.h - the probe wrote that name as well) */
        if (kind == FXU_TIME || kind == FXU_FRAMETIME || kind == FXU_POINTER || kind == FXU_DAYTIME ||
            kind == FXU_POINTER_LAST)
            pass->dyn_last[u].sent = FALSE;
    }
    g_free (px);
    *maxdiff = md;
    return ok && md <= FXS_VERIFY_TOL;
}

/* fxs_post_draw: the pass on probation was drawn with the generic program */
static void fxs_verify (EffectChain *ch, EffPass *pass)
{
    struct FxSpec *sp = pass->spec;
    if (!sp->src || !sp->gen || sp->prog != sp->gen) {
        sp->probation = 0;
        return;
    }
    guint nth = ++sp->draws;
    if (nth & (nth - 1))
        return;     /* compared behind draws 1, 2, 4, 8, ... */
    /* a constant a script wrote since the generic program was linked
     * stays a uniform, and the candidate built before that is not the
     * program to have */
    gboolean stale = FALSE;
    for (int i = 0; i < sp->n; i++) {
        FxsConst *c = &sp->c[i];
        if (c->dynamic || !c->k0 || c->k0 > pass->n_consts)
            continue;
        if (memcmp (pass->consts[c->k0 - 1].v, c->v0, sizeof (c->v0))) {
            c->dynamic = 1;
            stale |= !c->hazard;    /* (a hazard constant was no part of the candidate) */
        }
    }
    if (stale || !sp->cand) {
        fxs_prog_put (sp->cand, NULL);
        sp->cand = NULL;
        int nb = 0;
        for (int i = 0; i < sp->n; i++) {
            sp->c[i].baked = fxs_bakeable (&sp->c[i]);
            nb += sp->c[i].baked;
        }
        if (nb) {
            char *sv = fxs_bake_source (sp->src->vsrc, sp), *sf = fxs_bake_source (sp->src->fsrc, sp);
            guint rw = fxs_rewrite (sp, pass, &sv, &sf);
            guint64 key[2];
            FxsProg *e = fxs_prog_lookup (sv, sf, key);
            if (!e && !fxs_is_bad (key)) {
                GLuint p = fxs_link_quiet (sv, sf, sp->name ? sp->name : "pass");
                if (p) {
                    e = fxs_prog_add (key, p);
                    g_atomic_int_inc (&fxs_n_compiled);
                }
            }
            if (e) {
                e->refs++;
                sp->cand = e;
                sp->cand_rw = rw;
            }
            g_free (sv);
            g_free (sf);
        }
        for (int i = 0; i < sp->n; i++)
            sp->c[i].baked = 0;     /* the generic program is still the one in use */
        if (stale) {
            sp->draws = 1;          /* the new candidate starts over, with this draw */
            sp->passed = 0;
        }
    }
    FxsProg *cand = sp->cand;
    if (!cand) {
        sp->probation = 0;
        return;
    }
    if (cand->verdict == 0) {
        if (fxs_rw_inexact (sp->cand_rw)) {
            cand->verdict = 1;
        } else {
            int md = 0;
            gboolean flat = FALSE;
            gboolean same = fxs_compare (pass, cand->program, sp->passed, &md, &flat);
            cand->owner = NULL;     /* it holds this draw's values now: nobody's */
            if (same && flat) {
                sp->flat = 1;       /* one colour: proves nothing, ask again later */
                if (nth < FXS_VERIFY_FLAT_DRAWS)
                    return;
                /* ... but not for ever: the generic program it is (the
                 * candidate's text keeps no verdict: a pass that shares
                 * it and draws a picture decides) */
                g_debug ("fx: %s: one colour in every comparison up to draw %u: the pass keeps the "
                         "generic program", sp->name ? sp->name : "a shader", nth);
                goto settled;
            }
            if (same && sp->flat) {
                /* the first picture after an empty start: the remaining
                 * comparisons behind this draw + 1, 3, 7, 15, 31 */
                sp->flat = 0;
                sp->draws = 1;
            }
            if (same && ++sp->passed < g_atomic_int_get (&fxs_verify_n))
                return;             /* so far so good: the next comparison decides on */
            cand->verdict = same ? 1 : 2;
            g_atomic_int_inc (same ? &fxs_n_verified : &fxs_n_rejected);
            if (!same) {
                fxs_bad_add (cand->key);
                g_message ("fx: %s: with its constants baked the pass draws something else than "
                           "with uniforms on this driver (up to %d/255 apart, draw %u); it keeps "
                           "the generic program", sp->name ? sp->name : "a shader", md, nth);
            }
            if (fxs_cost_on ())
                fprintf (stderr, "fxverify\t%s\tprog=%016" G_GINT64_MODIFIER "x\tresult=%s\tmax=%d\tdraw=%u\n",
                         sp->label ? sp->label : "layer=?\tshader=?", cand->key[0],
                         same ? "same" : "rejected", md, nth);
        }
    }
settled:
    sp->probation = 0;
    if (cand->verdict != 1) {
        fxs_prog_put (cand, NULL);
        sp->cand = NULL;
        if (sp->vsrc_final) {
            /* diagnostics: the sources in use are the generic ones */
            g_free (sp->vsrc_final);
            g_free (sp->fsrc_final);
            sp->vsrc_final = g_strdup (sp->src->vsrc);
            sp->fsrc_final = g_strdup (sp->src->fsrc);
            if (sp->count_prog)
                glDeleteProgram (sp->count_prog);
            sp->count_prog = 0;
        }
        return;
    }
    /* the candidate's text is in the cache (sp->cand holds it): the
     * switch finds it there */
    if (fxs_switch (pass, FALSE)) {
        g_atomic_int_add (&fxs_n_baked, sp->n_baked);
        ch->cacheable = -1;         /* fx_chain_static: fewer uniforms now */
        ch->dyn = -1;               /* fx_chain_dyn: likewise */
        fxs_picture_changed ();
    }
    fxs_prog_put (sp->cand, NULL);
    sp->cand = NULL;
    glUseProgram (pass->program);
}

/* fx_chain_run, right behind a pass's draw (program, textures, VAO,
 * target and viewport still bound) */
static inline void fxs_post_draw (EffectChain *ch, EffPass *pass, int index)
{
    struct FxSpec *sp = pass->spec;
    if (G_UNLIKELY (sp && sp->probation)) {
        /* (no cost line for this draw: the pass may have a new program
         * now whose uniforms the next run uploads) */
        fxs_verify (ch, pass);
        return;
    }
    fxs_cost_draw (ch, pass, index);
}
