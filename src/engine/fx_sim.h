/* fx_sim.h - effect chains that carry a simulation from frame to frame
 *
 * Included by scenenode_effects.c (the types and prototypes are in
 * fx_sim_types.h). Wallpaper Engine's Fluid Simulation is the effect this
 * was written for: its passes advect a velocity field, add forces
 * (emitters, the pointer, vorticity), make the field divergence-free with a
 * few pressure iterations and move a dye field along it; a last pass draws
 * the dye over the layer. Velocity, pressure and dye are render targets
 * that hold last frame's values, and two "swap" commands at the end of the
 * pass list turn what was written into what is read next.
 *
 * WHICH CHAINS. A chain is a simulation when its effect file has a pass
 * with "command": "swap". Nothing in this file touches any other chain:
 * effects without a swap run exactly as before.
 *
 * WHAT SUCH A CHAIN GETS HERE
 *  - Targets of its own for life, never from the pool. fbos[].fit gives a
 *    grid of a fixed number of cells (fluidsim.h), fbos[].clear what a
 *    target holds before the first step (nothing declared: zero - a float
 *    target must not start with what the driver left in it). A target that
 *    has to change its size (the render size, the quality level or the
 *    layer changed) keeps its field, resampled.
 *  - A fixed time step. The pass list was written for one run per frame
 *    with g_Frametime as the step, which makes the fluid's look depend on
 *    the frame rate (several terms of the stock shaders are per step, not
 *    per second). Here a step always has the same length (1/60 s; 1/30 s
 *    at quality "low") and a frame runs as many steps as scene time has
 *    passed: none, one or several (FXSIM_RUN_*). Passes that write a named
 *    target, and the commands, are the step; passes without a target draw
 *    the picture ("presenting") and run once per frame, after the frame's
 *    last step and before its swaps, as the pass list orders them.
 *    In a frame without a step the swaps of the last step are taken back
 *    and applied again in the order of the pass list while the presenting
 *    passes draw, so each of them reads what it read then;
 *    when their input stands still as well, the picture of the last frame
 *    is returned and no pass runs at all.
 *    Step passes see g_Frametime = the step, g_Time = simulated time and
 *    the pointer of their step; presenting passes see the frame's values.
 *  - The pointer the way the stock shaders expect it: see fluidsim.h and
 *    fxsim_begin ().
 *  - "conditions" on passes and fbos (the lighting variant's normal map
 *    pass is only there when the combo LIGHTING is set).
 *
 * WHAT THE EXECUTOR'S SHORTCUTS DO WITH IT
 *  - static cache / static prefix / soft cache: never. fx_chain_static ()
 *    is FALSE for a swap, fx_chain_dyn () reports FEEDBACK.
 *  - kept passes (a pass drawn once): never in a simulation chain
 *    (fx_keep_scan).
 *  - effect update rate (lower presets hold animated sets on some frames):
 *    never held, as any chain with feedback. Its rate is the step rate of
 *    this file, tied to scene time, whatever the frame rate.
 *  - region restriction and "Skip hidden areas": a set with a feedback
 *    chain gets no plan, a swap ends the clip: every pass fills its whole
 *    target. A field that was only computed where it is seen would be
 *    wrong one frame later, when the flow has carried it somewhere else.
 *  - comparison runs (the region check draws a run a second time): a
 *    chain schedules steps once per frame; a second run in the same frame
 *    finds no step due and only presents.
 *  - culling: not culled (feedback): the fluid goes on while its layer is
 *    off screen.
 *  - pause: the targets stay (feedback chains keep theirs), the clock
 *    stands still, the fluid goes on where it was.
 *  - memory: the targets are counted with all effect targets
 *    (fx_targets_stats); fx_sim_stats () tells a set's share.
 *  - profiler: one row per pass and target size as for every chain; the
 *    steps of a frame add up in the rows of their passes.
 *
 * SWITCH AND QUALITY: fx_sim_set_enabled (config fluid_sim, default on;
 * WALPE_FLUID=0|1 overrides), fx_sim_set_quality (config fluid_quality:
 * full | reduced | low; WALPE_FLUID_QUALITY overrides). Off = the chain is
 * left out like an effect switched invisible, its targets are freed and
 * the set is classified as if the chain were not there (a layer whose only
 * effect it is becomes static). Both apply to running wallpapers.
 *
 * RESET. The fields start from their clear values when the chain's targets
 * are made: at load (so also after a user property was changed: that loads
 * the wallpaper again), after the switch was off, and by fx_sim_reset ().
 * A constant set by a script does not reset anything: emitters that
 * scripts move would never get a fluid going. */

/* the longest frame time the clock takes (a frame after a stall) */
#define FX_SIM_MAX_FRAME_S 0.25

/* ---- settings (any thread) ------------------------------------------- */
static gint fxsim_on_v = 1;
static gint fxsim_quality_v = WALPE_FLUID_FULL;
static gint fxsim_gen_v;   /* bumped by every change: sets look again (fxsim_sync) */

static gboolean fxsim_enabled (void)
{
    static gint env = -2;   /* -1 = WALPE_FLUID not set */
    int e = g_atomic_int_get (&env);
    if (G_UNLIKELY (e == -2)) {
        const char *s = g_getenv ("WALPE_FLUID");
        e = s && *s ? (*s != '0') : -1;
        g_atomic_int_set (&env, e);
    }
    return e >= 0 ? e : g_atomic_int_get (&fxsim_on_v) != 0;
}

static const WalpeFluidParams *fxsim_params (void)
{
    static gint env = -2;   /* -1 = WALPE_FLUID_QUALITY not set (or not a level) */
    int e = g_atomic_int_get (&env);
    if (G_UNLIKELY (e == -2)) {
        WalpeFluidQuality q = WALPE_FLUID_FULL;
        e = walpe_fluid_quality_parse (g_getenv ("WALPE_FLUID_QUALITY"), &q) ? (int) q : -1;
        g_atomic_int_set (&env, e);
    }
    return walpe_fluid_params ((WalpeFluidQuality) (e >= 0 ? e : g_atomic_int_get (&fxsim_quality_v)));
}

void fx_sim_set_enabled (gboolean on)
{
    const int v = on ? 1 : 0;
    if (g_atomic_int_get (&fxsim_on_v) == v)
        return;
    g_atomic_int_set (&fxsim_on_v, v);
    g_atomic_int_inc (&fxsim_gen_v);
    fxs_picture_changed ();   /* a frame is drawn, nothing baked is reused */
}

gboolean fx_sim_enabled (void)
{
    return fxsim_enabled ();
}

gboolean fx_sim_set_quality (const char *quality)
{
    WalpeFluidQuality q;
    if (!walpe_fluid_quality_parse (quality, &q))
        return FALSE;
    if (g_atomic_int_get (&fxsim_quality_v) != (int) q) {
        g_atomic_int_set (&fxsim_quality_v, (int) q);
        g_atomic_int_inc (&fxsim_gen_v);
        fxs_picture_changed ();
    }
    return TRUE;
}

const char *fx_sim_quality (void)
{
    const WalpeFluidParams *p = fxsim_params ();
    for (int q = 0; q < WALPE_FLUID_N; q++)
        if (walpe_fluid_params ((WalpeFluidQuality) q) == p)
            return walpe_fluid_quality_name ((WalpeFluidQuality) q);
    return walpe_fluid_quality_name (WALPE_FLUID_FULL);
}

/* ---- per frame (render thread) ---------------------------------------- */

static void fxsim_new_frame (float frametime)
{
    double dt = frametime;
    if (!(dt > 0.0))
        dt = 0.0;
    fxsim_frame.clock += MIN (dt, FX_SIM_MAX_FRAME_S);
    fxsim_frame.map_set = FALSE;   /* (never from one frame into the next) */
}

void fx_set_pointer_state (gboolean present, gboolean down)
{
    fxsim_frame.ptr_absent = !present;
    fxsim_frame.ptr_down = down;
}

void fx_set_pointer_map (const float *m3)
{
    fxsim_frame.map_set = m3 != NULL;
    if (m3)
        memcpy (fxsim_frame.map, m3, sizeof (fxsim_frame.map));
}

/* The pointer's height in the frame, 0 = top. fx_set_frame_info () is
 * handed 1 at the top by the scene (the value every other effect has been
 * getting as g_PointerPosition.y). The stock cursor shaders flip the value,
 * go through clip space and flip again: their arithmetic lands on the
 * texel under the pointer only for 0 = top, texture coordinate v = 0 being
 * the picture's top row here as in Wallpaper Engine. So a simulation gets
 * it that way. WALPE_FLUID_POINTER_Y=up hands over the unflipped value
 * (A/B on a real desktop: the smoke must be pushed where the pointer is,
 * not at the mirrored height). */
static float fxsim_pointer_y (void)
{
    static gint up = -1;
    int u = g_atomic_int_get (&up);
    if (G_UNLIKELY (u < 0)) {
        u = !g_strcmp0 (g_getenv ("WALPE_FLUID_POINTER_Y"), "up");
        g_atomic_int_set (&up, u);
    }
    return u ? fx_frame.py : 1.0f - fx_frame.py;
}

/* ---- build time -------------------------------------------------------- */

/* the pass list of an effect file has a swap: the chain is a simulation */
static gboolean fxsim_scan (JsonArray *passes)
{
    for (guint i = 0; i < wpj_arr_len (passes); i++)
        if (!g_strcmp0 (wpj_str (wpj_arr_obj (passes, i), "command", NULL), "swap"))
            return TRUE;
    return FALSE;
}

/* the value the effect instance gives a combo: its passes carry the combos
 * the user set, each pass the ones its shader knows. 0 when none has it. */
static double fxsim_combo (JsonArray *instpasses, const char *name)
{
    for (guint i = 0; i < wpj_arr_len (instpasses); i++) {
        JsonObject *cb = wpj_obj (wpj_arr_obj (instpasses, i), "combos");
        if (cb && json_object_has_member (cb, name))
            return wpj_num (cb, name, 0.0);
    }
    return 0.0;
}

static gboolean fxsim_cond_object (JsonObject *c, JsonArray *instpasses)
{
    gboolean ok = TRUE;
    GList *ks = json_object_get_members (c);
    for (GList *l = ks; l && ok; l = l->next) {
        const char *name = l->data;
        const double have = fxsim_combo (instpasses, name);
        JsonObject *o = wpj_obj (c, name);
        if (o) {
            /* { "op": "ge", "value": 2 } */
            const char *op = wpj_str (o, "op", "eq");
            const double v = wpj_num (o, "value", 0.0);
            ok = !strcmp (op, "ge") ? have >= v : !strcmp (op, "gt") ? have > v
               : !strcmp (op, "le") ? have <= v : !strcmp (op, "lt") ? have < v
               : !strcmp (op, "ne") ? have != v : have == v;
        } else {
            ok = have == wpj_num (c, name, 0.0);
        }
    }
    g_list_free (ks);
    return ok;
}

/* "conditions" of a pass or fbo entry: [ { "LIGHTING": 1 } ] - every combo
 * named has the value given. TRUE without conditions. Only asked for
 * simulation chains. A combo the instance does not set counts as 0 (its
 * shader's own default is not known before the shader is read): right for
 * the stock effect, whose conditions all ask for a non-default value. */
static gboolean fxsim_condition (JsonObject *def, JsonArray *instpasses)
{
    if (!def)
        return TRUE;
    JsonObject *one = wpj_obj (def, "conditions");
    if (one)
        return fxsim_cond_object (one, instpasses);
    JsonArray *all = wpj_arr (def, "conditions");
    for (guint i = 0; i < wpj_arr_len (all); i++) {
        JsonObject *c = wpj_arr_obj (all, i);
        if (c && !fxsim_cond_object (c, instpasses))
            return FALSE;
    }
    return TRUE;
}

/* fbos[] entry of a simulation chain: fit, clear, and whether the format
 * it names is a float format (also when float targets are not in use) */
static void fxsim_target_parse (EffTarget *t, JsonObject *f)
{
    /* (untrusted: nothing, a negative number, a fraction, 1e30 - fluidsim.h) */
    t->fit = walpe_fluid_fit_cells (wpj_num (f, "fit", 0.0), &t->fit_asked);
    const char *clr = wpj_str (f, "clear", NULL);
    if (clr) {
        float v[4] = { 0, 0, 0, 0 };
        fx_scan_floats (clr, v);
        for (int c = 0; c < 4; c++)
            t->clear[c] = isfinite (v[c]) ? v[c] : 0.0f;
    }
    const char *fmt = wpj_str (f, "format", "");
    t->want_float = strstr (fmt, "16f") != NULL || strstr (fmt, "32f") != NULL;
}

/* uniforms only a simulation chain gets from here (fx_cache_uniforms) */
static gboolean fxsim_uniform (EffPass *pass, const char *name, GLint loc, GLenum type)
{
    int kind;
    if (type == GL_FLOAT_VEC4 && !strcmp (name, "g_PointerState"))
        kind = FXU_POINTER_STATE;
    else if (type == GL_FLOAT_MAT4 && !strcmp (name, "g_EffectTextureProjectionMatrixInverse"))
        kind = FXU_TEXPROJ_INV;
    else
        return FALSE;
    pass->dyn[pass->n_dyn++] = (FxUniform) { loc, (guint8) kind, 0 };
    return TRUE;
}

static void fxsim_chain_built (EffectChain *ch, JsonObject *eo)
{
    struct FxSim *s = g_new0 (struct FxSim, 1);
    ch->sim = s;
    int last_draw = -1;
    gboolean present_moves = FALSE;
    for (int i = 0; i < ch->n_passes; i++) {
        const EffPass *p = &ch->passes[i];
        s->step_pass[i] = p->is_copy || p->target[0];
        if (p->is_copy)
            continue;   /* a command, or a pass a condition left out */
        last_draw = i;
        if (s->step_pass[i]) {
            s->n_step++;
            continue;
        }
        s->n_present++;
        present_moves |= p->moves_verts;
        for (int u = 0; u < p->n_dyn; u++) {
            const int k = p->dyn[u].kind;
            s->present_vol |= k == FXU_TIME || k == FXU_FRAMETIME || k == FXU_AUDIO ||
                              k == FXU_POINTER || k == FXU_POINTER_LAST || k == FXU_POINTER_STATE ||
                              k == FXU_DAYTIME || k == FXU_TEXPROJ_INV;
        }
        for (int u = 0; u < FX_MAX_BINDS; u++)
            s->present_vol |= p->sys[u] || p->slot[u].rt == FX_RT_FULLFRAME;
    }
    s->reuse_ok = s->n_present == 1 && last_draw >= 0 && !s->step_pass[last_draw] &&
                  ch->alt_out < 0 && !present_moves;
    /* "functions": { "clearDye": { "action": "clear", "fbos": [ ... ] } } */
    JsonObject *fns = wpj_obj (eo, "functions");
    GList *ks = fns ? json_object_get_members (fns) : NULL;
    for (GList *l = ks; l && s->n_fn < FX_SIM_MAX_FN; l = l->next) {
        JsonObject *fo = wpj_obj (fns, l->data);
        if (g_strcmp0 (wpj_str (fo, "action", NULL), "clear"))
            continue;
        JsonArray *fb = wpj_arr (fo, "fbos");
        guint mask = 0;
        for (guint j = 0; j < wpj_arr_len (fb); j++) {
            JsonNode *n = json_array_get_element (fb, j);
            if (!n || !JSON_NODE_HOLDS_VALUE (n) || json_node_get_value_type (n) != G_TYPE_STRING)
                continue;
            EffTarget *t = fx_find_target (ch, json_node_get_string (n));
            if (t)
                mask |= 1u << (guint) (t - ch->targets);
        }
        snprintf (s->fn[s->n_fn].name, sizeof (s->fn[0].name), "%s", (const char *) l->data);
        s->fn[s->n_fn++].targets = mask;
    }
    g_list_free (ks);
    g_debug ("fx: a simulation: %d step pass(es), %d presenting, %d clear function(s)%s",
             s->n_step, s->n_present, s->n_fn,
             s->reuse_ok ? "" : "; its picture is drawn in every frame");
}

static void fxsim_chain_free (EffectChain *ch)
{
    g_clear_pointer (&ch->sim, g_free);
}

static gboolean fxsim_off (const EffectChain *ch)
{
    return ch->sim && (ch->sim->broken || !fxsim_enabled ());
}

/* ---- targets ------------------------------------------------------------ */

static gboolean fxsim_fmt_float (FxRtFormat f)
{
    return f == FX_FMT_R16F || f == FX_FMT_RG16F || (f == FX_FMT_RGBA8 && fx_rt_half_float ());
}

/* the clock, the pointer's path and the kept picture: as after a load */
static void fxsim_forget (struct FxSim *s)
{
    s->started = FALSE;
    s->acc = 0.0;
    s->frame = 0;
    s->sim_time = 0.0;
    s->swapped = FALSE;
    s->serial++;
    s->resets++;
    memset (&s->ptr, 0, sizeof (s->ptr));
    s->shown.valid = FALSE;
}

static void fxsim_clear_fbo (GLuint fbo, const float clear[4])
{
    fxr_scissor_off ();   /* a clear obeys the scissor */
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glClearColor (clear[0], clear[1], clear[2], clear[3]);
    glClear (GL_COLOR_BUFFER_BIT);
}

static void fxsim_clear_target (const EffTarget *t)
{
    fxsim_clear_fbo (t->fbo, t->clear);
}

/* the longest side a texture may have in this thread's context
 * (G_MAXINT: the driver did not say) */
static int fxsim_tex_max (void)
{
    if (G_UNLIKELY (fxsim_tex_max_v <= 0)) {
        GLint m = 0;
        glGetIntegerv (GL_MAX_TEXTURE_SIZE, &m);
        fxsim_tex_max_v = m > 0 ? (int) m : G_MAXINT;
    }
    return fxsim_tex_max_v;
}

/* The chain's targets for this run: all its own. w / h = the layer's size
 * in scene units (what the shaders are told), k = working resolution
 * factor, as for fx_ensure_targets. The grid (fit) depends on the layer's
 * proportions only; picture-sized targets follow the render size (k) and
 * the quality level, and keep their field across a change of either.
 * What an effect file declares is bounded twice: a grid has at most
 * WALPE_FLUID_FIT_MAX cells on its longer side (fxsim_target_parse), and
 * no target of the chain - grid, field, output - has a side longer than
 * the driver's largest texture. The sizes made here are the ones that are
 * allocated, kept in EffTarget.w / .h and counted (fx_targets_stats,
 * fx_sim_stats); when one of them is smaller than the file asked for, one
 * line says so, once per chain. */
static void fxsim_ensure_targets (EffectChain *ch, int w, int h, float k)
{
    struct FxSim *s = ch->sim;
    const WalpeFluidParams *qp = fxsim_params ();
    const int tex_max = fxsim_tex_max ();
    int rw = MAX (1, (int) ceilf (w * k)), rh = MAX (1, (int) ceilf (h * k));
    gboolean tex_cut = walpe_fluid_limit_size (&rw, &rh, tex_max);
    int fit_asked = 0;   /* the largest declared fit that was reduced */
    const GLuint out0 = ch->out_tex;
    fx_target_ensure (&ch->out_fbo, &ch->out_tex, &ch->out_pool, &ch->out_w, &ch->out_h, rw, rh, TRUE);
    ch->out_vw = w;
    ch->out_vh = h;
    gboolean touched = ch->out_tex != out0, fresh = FALSE;
    for (int i = 0; i < ch->n_targets && !s->broken; i++) {
        EffTarget *t = &ch->targets[i];
        int tw, th;
        if (t->fit > 0) {
            walpe_fluid_fit_size (w, h, t->fit, &tw, &th);
            tex_cut |= walpe_fluid_limit_size (&tw, &th, tex_max);
            t->vw = tw;
            t->vh = th;
            if (t->fit_asked > t->fit)
                fit_asked = MAX (fit_asked, t->fit_asked);
        } else {
            /* (the second output of passes without a target has the output's size) */
            const int div = t->scale * (i == ch->alt_out ? 1 : qp->field_div);
            tw = MAX (1, rw / div);
            th = MAX (1, rh / div);
            t->vw = MAX (1, w / t->scale);
            t->vh = MAX (1, h / t->scale);
        }
        if (t->pool) {
            t->pool = 0;
            t->tex = t->fbo = 0;
        }
        if (t->tex && t->w == tw && t->h == th)
            continue;
        GLuint nf = 0, nt = 0;
        const FxRtFormat got = fx_make_rt_fmt (&nf, &nt, tw, th, (FxRtFormat) t->fmt);
        if (t->want_float && !fxsim_fmt_float (got)) {
            /* velocity and pressure need signed values beyond 0..1: in an
             * 8-bit target the fluid would not flow, it would smear. Left
             * out instead, with one line that says why. */
            fx_drop_rt (&nf, &nt, tw, th);
            s->broken = TRUE;
            g_message ("fx: %s: this graphics driver cannot render to float targets (or "
                       "WALPE_FX_RT_FORMATS=0 is set): the simulation is left out, the layer is "
                       "shown without it", ch->file);
            break;
        }
        if (t->tex) {
            /* the field is carried over. The new target is cleared first:
             * where a driver refuses the blit it then holds the field's
             * clear values (this field starts again) instead of whatever
             * the allocation left in it - in a float target that can be
             * values no step ever gets rid of */
            fxsim_clear_fbo (nf, t->clear);   /* (also: scissor off, a blit obeys it) */
            glBindFramebuffer (GL_READ_FRAMEBUFFER, t->fbo);
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, nf);
            glBlitFramebuffer (0, 0, t->w, t->h, 0, 0, tw, th, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            fx_drop_rt (&t->fbo, &t->tex, t->w, t->h);
            t->fbo = nf;
            t->tex = nt;
        } else {
            t->fbo = nf;
            t->tex = nt;
            fxsim_clear_target (t);
            fresh = TRUE;
        }
        t->w = tw;
        t->h = th;
        t->kp_valid = FALSE;
        touched = TRUE;
    }
    if (s->broken) {
        /* every set looks again: this chain is off from now on */
        fx_chain_drop_targets (ch, 0);
        g_atomic_int_inc (&fxsim_gen_v);
        fxs_picture_changed ();
    } else if ((fit_asked > 0 || tex_cut) && !s->limit_said) {
        /* once per chain (= per effect on a layer, per context), not per frame */
        char a[112] = "", b[112] = "";
        s->limit_said = TRUE;
        if (fit_asked > 0)
            g_snprintf (a, sizeof (a), " a grid of %d cells is made with %d, the most a simulation gets;",
                        fit_asked, WALPE_FLUID_FIT_MAX);
        if (tex_cut)
            g_snprintf (b, sizeof (b), " no target is larger than a texture can be here (%d);", tex_max);
        g_message ("fx: %s: the simulation's targets are smaller than the effect file asks for:%s%s the "
                   "effect runs with them", ch->file, a, b);
    }
    if (fresh)
        fxsim_forget (s);
    if (touched) {
        glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
        s->shown.valid = FALSE;
    }
    s->field_div = qp->field_div;
}

/* ---- a run -------------------------------------------------------------- */

/* take the chain's swaps back, in reverse order (fx_chain_run applies them) */
static void fxsim_swaps_undo (EffectChain *ch)
{
    for (int n = 0; n < ch->n_passes; n++) {
        const EffPass *p = &ch->passes[ch->n_passes - 1 - n];
        if (!p->is_swap || !p->target[0])
            continue;
        EffTarget *a = fx_find_target (ch, p->copy_src), *b = fx_find_target (ch, p->target);
        if (a && b && a != b && a->w == b->w && a->h == b->h) {
            const GLuint f = a->fbo, x = a->tex;
            const int pl = a->pool;
            a->fbo = b->fbo, a->tex = b->tex, a->pool = b->pool;
            b->fbo = f, b->tex = x, b->pool = pl;
        }
    }
}

/* what the step passes of the round about to run are told */
static void fxsim_round_values (struct FxSim *s)
{
    const double t = s->sim_time + s->run_h;   /* the time this step arrives at */
    s->u_dt = (float) s->run_h;
    /* (folded like g_Time: a float holds an hour of it to a quarter of a millisecond) */
    s->u_time = (float) (t - floor (t / FX_TIME_PERIOD_S) * FX_TIME_PERIOD_S);
    walpe_fluid_pointer_step (&s->ptr, s->round + 1, s->rounds, s->u_cur, s->u_last);
}

/* Before the passes: how many steps this frame runs. A chain is scheduled
 * once per frame (the first run of the frame takes the time since the
 * frame it was last scheduled in); a second run in the same frame presents
 * only. Returns FXSIM_RUN_*. */
static int fxsim_begin (EffectChain *ch, GLuint input, int w, int h, int fullw, int fullh, float k)
{
    struct FxSim *s = ch->sim;
    const WalpeFluidParams *qp = fxsim_params ();
    const guint fr = fx_pool_frame + 1u;
    const float px = fx_frame.px, py = fxsim_pointer_y ();
    int n = 0;
    s->run_h = 1.0 / qp->step_hz;
    s->sched = s->frame != fr;
    if (s->sched) {
        double elapsed = 0.0;
        if (s->started && fxsim_frame.clock >= s->clock_last)
            elapsed = fxsim_frame.clock - s->clock_last;
        /* The clock takes at most FX_SIM_MAX_FRAME_S per frame: more than
         * that since this chain's last frame means frames went by without
         * it (its layer hidden, the effect switched invisible by a script).
         * The pointer went its way unseen meanwhile: no line leads from
         * where it was then to where it is now - and `since` would be so
         * long that no distance counted as a jump. */
        if (elapsed > FX_SIM_MAX_FRAME_S * 1.5) {
            s->ptr.have = FALSE;
            s->ptr.since = 0.0;
        }
        n = walpe_fluid_steps (&s->acc, elapsed, s->run_h, qp->max_steps);
        s->started = TRUE;
        s->clock_last = fxsim_frame.clock;
        s->frame = fr;
        walpe_fluid_pointer_frame (&s->ptr, !fxsim_frame.ptr_absent, px, py, elapsed);
    }
    s->u_pcur[0] = fxsim_frame.ptr_absent ? WALPE_FLUID_AWAY : px;
    s->u_pcur[1] = fxsim_frame.ptr_absent ? WALPE_FLUID_AWAY : py;
    /* the layer in the frame (identity: the frame stands in for the layer) */
    walpe_fluid_clip_to_layer (fxsim_frame.map_live ? fxsim_frame.map : NULL, s->map);
    const gboolean in_static = fxsim_frame.input_static;
    fxsim_frame.input_static = FALSE;
    s->round = 0;
    s->rounds = MAX (n, 1);
    if (n > 0) {
        s->mode = FXSIM_RUN_STEP;
        fxsim_round_values (s);
        return s->mode;
    }
    if (s->reuse_ok && !s->present_vol && in_static && s->shown.valid && s->shown.input == input &&
        s->shown.out == ch->out_tex && s->shown.gen == ch->gen &&
        s->shown.set_gen == fxsim_frame.set_gen && s->shown.serial == s->serial &&
        s->shown.w == w && s->shown.h == h && s->shown.fullw == fullw && s->shown.fullh == fullh &&
        s->shown.k == k && s->shown.sw == fx_frame.sw && s->shown.sh == fx_frame.sh) {
        s->mode = FXSIM_RUN_REUSE;
        fxsim_frame.reused++;
        return s->mode;
    }
    s->mode = FXSIM_RUN_PRESENT;
    fxsim_frame.presented++;
    if (s->swapped)
        fxsim_swaps_undo (ch);
    return s->mode;
}

/* is this pass left out of the round in progress? */
static gboolean fxsim_skips (const EffectChain *ch, int pass)
{
    const struct FxSim *s = ch->sim;
    if (s->mode == FXSIM_RUN_PRESENT) {
        /* fxsim_begin () took the swaps of the last step back; the list
         * applies each again where it stands (fx_chain_run, the same rule
         * as fxsim_swaps_undo). A presenting pass listed behind a swap then
         * reads the target it read in the frame of that step, not the
         * other one of the pair, and the run ends with every swap applied. */
        if (ch->passes[pass].is_swap)
            return !s->swapped;
        return s->step_pass[pass];
    }
    /* a step that is not the frame's last: nothing of it is shown */
    return !s->step_pass[pass] && s->round + 1 < s->rounds;
}

/* after the pass list ran once: TRUE = it runs again (the next step) */
static gboolean fxsim_next_round (EffectChain *ch)
{
    struct FxSim *s = ch->sim;
    if (s->mode != FXSIM_RUN_STEP)
        return FALSE;
    s->sim_time += s->run_h;
    s->steps++;
    s->serial++;
    s->swapped = TRUE;
    fxsim_frame.steps++;
    if (++s->round >= s->rounds)
        return FALSE;
    fxsim_round_values (s);
    return TRUE;
}

/* after the run: output = what it returns */
static void fxsim_end (EffectChain *ch, GLuint input, GLuint output, int w, int h, int fullw,
                       int fullh, float k)
{
    struct FxSim *s = ch->sim;
    /* (a run that only presented: its swaps are applied again by now,
     * each at its place in the pass list - fxsim_skips) */
    if (s->sched)
        walpe_fluid_pointer_end (&s->ptr, s->mode == FXSIM_RUN_STEP ? s->rounds : 0);
    s->shown.valid = s->reuse_ok && output == ch->out_tex && output != 0;
    s->shown.input = input;
    s->shown.out = output;
    s->shown.gen = ch->gen;
    s->shown.set_gen = fxsim_frame.set_gen;
    s->shown.serial = s->serial;
    s->shown.w = w;
    s->shown.h = h;
    s->shown.fullw = fullw;
    s->shown.fullh = fullh;
    s->shown.k = k;
    s->shown.sw = fx_frame.sw;
    s->shown.sh = fx_frame.sh;
}

/* the pointer for one pass of the run in progress (last may be NULL) */
static void fxsim_pointer (const EffectChain *ch, int pass, float cur[2], float last[2])
{
    const struct FxSim *s = ch->sim;
    const gboolean step = s->mode == FXSIM_RUN_STEP && s->step_pass[pass];
    memcpy (cur, step ? s->u_cur : s->u_pcur, 2 * sizeof (float));
    if (last)
        memcpy (last, step ? s->u_last : s->u_pcur, 2 * sizeof (float));
}

/* Accounting (fx_chain_px): the pixels one step and one presentation shade
 * on a working size of rw x rh with every knob at its default; *passes =
 * draws. One step per frame is what a 60 Hz frame costs. */
static guint64 fxsim_chain_px (EffectChain *ch, int rw, int rh, guint *passes)
{
    guint64 px = 0;
    guint n = 0;
    for (int i = 0; i < ch->n_passes && !fxsim_off (ch); i++) {
        const EffPass *ps = &ch->passes[i];
        EffTarget *t = ps->target[0] ? fx_find_target (ch, ps->target) : NULL;
        if ((ps->target[0] && !t) || ps->is_copy)
            continue;
        int tw = rw, th = rh;
        if (t && t->fit > 0)
            walpe_fluid_fit_size (rw, rh, t->fit, &tw, &th);
        else if (t)
            tw = MAX (1, rw / t->scale), th = MAX (1, rh / t->scale);
        px += (guint64) tw * th;
        n++;
    }
    if (passes)
        *passes = n;
    return px;
}

/* ---- sets ------------------------------------------------------------------ */

/* The switch, the quality level or a chain's "broken" changed since this
 * set last looked: everything derived from "is there a simulation running"
 * is worked out again. gl = the caller has the context (the run): the
 * targets of chains that are off go. */
static void fxsim_sync (struct FxSet *set, gboolean gl)
{
    if (G_LIKELY (!set->has_sim))
        return;
    const guint g = (guint) g_atomic_int_get (&fxsim_gen_v) + 1u;
    if (set->sim_gen != g) {
        set->sim_gen = g;
        set->sim_dirty = TRUE;
        for (int i = 0; i < set->n; i++) {
            EffectChain *c = set->chains[i];
            if (c && c->sim)
                c->cacheable = c->dyn = c->feedback = -1;
        }
        set->cacheable = -1;
        set->cullable = 0;
        set->elig.valid = FALSE;
        set->cached = FALSE;
        set->held = FALSE;
        set->pfx.valid = FALSE;
        set->hpfx.valid = FALSE;
        set->rg.valid = FALSE;
        set->gen++;
    }
    if (gl && set->sim_dirty) {
        set->sim_dirty = FALSE;
        for (int i = 0; i < set->n; i++) {
            EffectChain *c = set->chains[i];
            if (c && c->sim && fxsim_off (c)) {
                fx_chain_drop_targets (c, 0);
                fxsim_forget (c->sim);
            }
        }
    }
}

gboolean fx_has_simulation (gpointer fxset)
{
    struct FxSet *set = fxset;
    return set && set->has_sim;
}

/* clear the targets in mask (bit i = targets[i]) to their clear values */
static void fxsim_clear_mask (EffectChain *ch, guint mask)
{
    gboolean any = FALSE;
    for (int i = 0; i < ch->n_targets; i++) {
        const EffTarget *t = &ch->targets[i];
        if (!(mask & (1u << (guint) i)) || !t->tex || t->pool)
            continue;
        fxsim_clear_target (t);
        any = TRUE;
    }
    if (any)
        glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
}

void fx_sim_reset (gpointer fxset)
{
    struct FxSet *set = fxset;
    for (int i = 0; set && i < set->n; i++) {
        EffectChain *c = set->chains[i];
        if (!c || !c->sim)
            continue;
        fxsim_clear_mask (c, ~0u);
        fxsim_forget (c->sim);
    }
    if (set && set->has_sim)
        set->gen++;
}

gboolean fx_sim_call (gpointer fxset, int inst, const char *function)
{
    struct FxSet *set = fxset;
    for (int i = 0; set && function && i < set->n; i++) {
        EffectChain *c = set->chains[i];
        if (!c || !c->sim || (inst >= 0 && c->inst_index != inst))
            continue;
        for (int f = 0; f < c->sim->n_fn; f++) {
            if (strcmp (c->sim->fn[f].name, function))
                continue;
            fxsim_clear_mask (c, c->sim->fn[f].targets);
            c->sim->serial++;
            c->sim->shown.valid = FALSE;
            return TRUE;
        }
    }
    return FALSE;
}

void fx_sim_stats (gpointer fxset, FxSimStats *out)
{
    struct FxSet *set = fxset;
    if (!out)
        return;
    memset (out, 0, sizeof (*out));
    out->step_hz = fxsim_params ()->step_hz;
    for (int i = 0; set && i < set->n; i++) {
        EffectChain *c = set->chains[i];
        if (!c || !c->sim)
            continue;
        out->chains++;
        if (fxsim_off (c)) {
            out->off++;
            continue;
        }
        out->steps += c->sim->steps;
        out->resets += c->sim->resets;
        for (int t = 0; t < c->n_targets; t++) {
            const EffTarget *et = &c->targets[t];
            if (!et->tex || et->pool)
                continue;
            out->targets++;
            out->bytes += (gint64) et->w * et->h *
                          fx_rt_bytes_per_pixel (fx_fmt_ok[et->fmt] == 2 ? FX_FMT_RGBA8 : (FxRtFormat) et->fmt);
            if (et->fit > 0 && !out->grid_w) {
                out->grid_w = et->w;
                out->grid_h = et->h;
            } else if (et->fit <= 0 && !out->field_w && t != c->alt_out) {
                out->field_w = et->w;
                out->field_h = et->h;
            }
        }
        if (c->out_tex && !c->out_pool) {
            out->targets++;
            out->bytes += (gint64) c->out_w * c->out_h * fx_rt_bytes_per_pixel (FX_FMT_RGBA8);
        }
    }
}

void fx_sim_thread_stats (guint64 *steps, guint *presented, guint *reused, gboolean reset)
{
    if (steps)
        *steps = fxsim_frame.steps;
    if (presented)
        *presented = fxsim_frame.presented;
    if (reused)
        *reused = fxsim_frame.reused;
    if (reset) {
        fxsim_frame.steps = 0;
        fxsim_frame.presented = fxsim_frame.reused = 0;
    }
}

/* one line for `walpe-ctl quality` (fx_quality_describe) */
static void fxsim_describe (struct FxSet *set, EffectChain *c, const char *layer, GString *out)
{
    if (!c->sim)
        return;
    if (fxsim_off (c)) {
        g_string_append_printf (out, "%s | %s | simulation left out (%s)\n", layer ? layer : "?", c->file,
                                c->sim->broken ? "no float render targets here" : "fluid_sim is off");
        return;
    }
    FxSimStats st;
    fx_sim_stats (set, &st);
    g_string_append_printf (out, "%s | %s | simulation: %.0f steps a second of scene time whatever the "
                            "frame rate (quality %s), grid %dx%d, picture fields %dx%d, %d targets = "
                            "%.1f MiB, never held, cached or drawn in part\n", layer ? layer : "?",
                            c->file, st.step_hz, fx_sim_quality (), st.grid_w, st.grid_h, st.field_w,
                            st.field_h, st.targets, (double) st.bytes / 1048576.0);
}
