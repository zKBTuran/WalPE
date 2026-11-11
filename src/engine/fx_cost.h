/* fx_cost.h - per-pass cost diagnostics of the effect executor (included
 * by fx_specialize.h; inert unless WALPE_FX_COST is set).
 *
 * WALPE_FX_COST=<frame> writes to stderr (so the lines interleave with
 * Mesa's ST_DEBUG=nir dump of each program as it is linked):
 *
 *  at build, one line per pass:
 *    fxpass  layer=  shader=  combos=  prog=<key>  hit=<0|1: program reused>
 *            baked=<uniform=value;...>  uniform=<material constants left>
 *            rw=<rewrite bits>  identity=<0|1>
 *  and, once the chain is complete, its effect file and pass index:
 *    fxchain layer=  effect=  pass=  prog=
 *
 *  in frame <frame> (1 = the first), for every pass that is drawn:
 *    fxcost  layer= effect= pass= shader= prog= w= h=
 *            fetch_avg= fetch_max=   texture reads per shaded pixel, counted
 *                                    by running the pass's own fragment
 *                                    source with every texSample2D /
 *                                    texSample2DLod / texture2D call
 *                                    counting itself (loops and branches as
 *                                    taken; reads the compiler removes as
 *                                    dead are still counted - the NIR dump
 *                                    tells those)
 *            cpu_us=                 process CPU of one draw of the pass
 *                                    (minimum of 5; with LP_NUM_THREADS=0
 *                                    that is llvmpipe's rasteriser)
 * The picture is not changed: the counting draw goes to a scratch target.
 *
 * WALPE_FX_PASS_DUMP=<dir> (with WALPE_FX_COST=<frame>) also writes what
 * every pass of that frame drew, as <dir>/<seq>-<w>x<h>.rgba (seq = order
 * of the draws in the frame, also in the fxcost line). Two runs of one
 * scene on a fixed clock, WALPE_FX_SPEC=0 and =1, compared file by file
 * name the first pass whose specialised program draws something else
 * than the generic one (the driver's constant folding is not bound to
 * give what its run-time arithmetic gives where a shader divides by
 * zero or takes pow() of a negative number).
 */

static int fxs_cost_frame_no (void)
{
    static int f = -2;
    if (f == -2) {
        const char *e = g_getenv ("WALPE_FX_COST");
        f = e && *e ? atoi (e) : -1;
    }
    return f;
}

static gboolean fxs_cost_on (void)
{
    return fxs_cost_frame_no () >= 0;
}

static __thread int fxs_cost_frame;   /* frames seen by this render thread */
static __thread int fxs_cost_seq;     /* passes drawn in this frame */

static inline void fxs_cost_new_frame (void)
{
    fxs_cost_frame++;
    fxs_cost_seq = 0;
}

/* WALPE_FX_PASS_DUMP: the bound target, as the pass left it */
static void fxs_cost_dump_target (int seq, const GLint vp[4])
{
    static const char *dir;
    static gsize once;
    if (g_once_init_enter (&once)) {
        dir = g_getenv ("WALPE_FX_PASS_DUMP");
        g_once_init_leave (&once, 1);
    }
    if (!dir || !*dir || vp[2] <= 0 || vp[3] <= 0)
        return;
    gsize n = (gsize) vp[2] * (gsize) vp[3] * 4;
    guint8 *px = g_malloc (n);
    glReadPixels (vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, px);
    char *path = g_strdup_printf ("%s/%04d-%dx%d.rgba", dir, seq, vp[2], vp[3]);
    g_file_set_contents (path, (const char *) px, (gssize) n, NULL);
    g_free (path);
    g_free (px);
}

static void fxs_cost_note_build (struct FxSpec *sp, EffPass *pass, const char *shader,
                                 const char *dbgname, const char *vsrc, const char *fsrc,
                                 gboolean hit)
{
    (void) pass;
    GString *o = g_string_new ("fxpass");
    g_string_append_printf (o, "\tlayer=%s\tshader=%s\tcombos=", dbgname, shader);
    /* the combos are the #defines in front of the prelude's "#define GLSL 1" */
    const char *stop = strstr (fsrc, "#define GLSL 1");
    gboolean first = TRUE;
    for (const char *p = fsrc; stop && p < stop;) {
        const char *eol = strchr (p, '\n');
        if (!eol || eol > stop)
            break;
        if (!strncmp (p, "#define ", 8)) {
            char name[64] = "";
            int v = 0;
            if (sscanf (p + 8, "%63s %d", name, &v) == 2 && v != 0 && strncmp (name, "TEX", 3)) {
                g_string_append_printf (o, "%s%s=%d", first ? "" : ",", name, v);
                first = FALSE;
            }
        }
        p = eol + 1;
    }
    g_string_append_printf (o, "\tprog=%016" G_GINT64_MODIFIER "x\thit=%d\tbaked=",
                            sp->prog ? sp->prog->key[0] : (guint64) 0, hit);
    for (int i = 0; i < sp->n; i++) {
        if (!sp->c[i].baked)
            continue;
        GString *v = g_string_new (NULL);
        fxs_put_value (v, &sp->c[i]);
        g_string_append_printf (o, "%s=%s;", sp->c[i].uname, v->str);
        g_string_free (v, TRUE);
    }
    g_string_append (o, "\tuniform=");
    for (int i = 0; i < sp->n; i++)
        if (!sp->c[i].baked)
            g_string_append_printf (o, "%s;", sp->c[i].uname);
    g_string_append_printf (o, "\trw=%u\tidentity=%d", sp->rewrites, sp->identity);
    fprintf (stderr, "%s\n", o->str);
    g_string_free (o, TRUE);
    g_free (sp->label);
    sp->label = g_strdup_printf ("layer=%s\tshader=%s", dbgname, shader);
    g_free (sp->vsrc_final);
    g_free (sp->fsrc_final);
    sp->vsrc_final = g_strdup (vsrc);
    sp->fsrc_final = g_strdup (fsrc);
}

/* fx_chain_build: the chain is complete - name its passes */
static void fxs_chain_built (EffectChain *ch, const char *file)
{
    if (!ch || !fxs_cost_on ())
        return;
    for (int i = 0; i < ch->n_passes; i++) {
        struct FxSpec *sp = ch->passes[i].spec;
        if (!sp || !sp->label)
            continue;
        char *l = g_strdup_printf ("%s\teffect=%s\tpass=%d", sp->label, file, i);
        g_free (sp->label);
        sp->label = l;
        fprintf (stderr, "fxchain\t%s\tprog=%016" G_GINT64_MODIFIER "x\ttarget=%s\n", l,
                 sp->prog ? sp->prog->key[0] : (guint64) 0,
                 ch->passes[i].target[0] ? ch->passes[i].target : "-");
    }
}

/* the fragment source with every texture read counting itself; the count
 * leaves in the colour (red = high byte, green = low byte) */
static char *fxs_cost_count_source (const char *fsrc)
{
    static const char *head =
        "#undef texture2D\n#undef texSample2D\n#undef texSample2DLod\n"
        "float _fxc_n = 0.0;\n"
        "vec4 _fxc_t(sampler2D s, vec2 c) { _fxc_n += 1.0; return texture(s, c); }\n"
        "vec4 _fxc_t(sampler2D s, vec2 c, float b) { _fxc_n += 1.0; return texture(s, c, b); }\n"
        "vec4 _fxc_l(sampler2D s, vec2 c, float l) { _fxc_n += 1.0; return textureLod(s, c, l); }\n"
        "vec4 _fxc_t(vec4 k, vec2 c) { return k; }\n"
        "vec4 _fxc_t(vec4 k, vec2 c, float b) { return k; }\n"
        "vec4 _fxc_l(vec4 k, vec2 c, float l) { return k; }\n"
        "#define texture2D _fxc_t\n#define texSample2D _fxc_t\n#define texSample2DLod _fxc_l\n"
        "#define main _fxc_main\n";
    static const char *tail =
        "\n#undef main\n"
        "void main() { _fxc_main(); _we_fragColor = vec4(floor(_fxc_n / 256.0) / 255.0, "
        "mod(_fxc_n, 256.0) / 255.0, 0.0, 1.0); }\n";
    const char *mark = strstr (fsrc, "#line 1\n");
    if (!mark)
        return NULL;
    GString *o = g_string_new_len (fsrc, mark - fsrc);
    g_string_append (o, head);
    g_string_append (o, mark);
    g_string_append (o, tail);
    return g_string_free (o, FALSE);
}

/* every uniform value of `from` into `to`, by name */
static void fxs_cost_copy_uniforms (GLuint from, GLuint to)
{
    GLint n = 0;
    glGetProgramiv (to, GL_ACTIVE_UNIFORMS, &n);
    glUseProgram (to);
    for (GLint u = 0; u < n; u++) {
        char name[160];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform (to, (GLuint) u, sizeof (name) - 16, NULL, &size, &type, name);
        char *br = strchr (name, '[');
        if (br)
            *br = 0;
        for (int i = 0; i < size; i++) {
            char en[192];
            if (size > 1 || br)
                snprintf (en, sizeof (en), "%s[%d]", name, i);
            else
                snprintf (en, sizeof (en), "%s", name);
            GLint lt = glGetUniformLocation (to, en), lf = glGetUniformLocation (from, en);
            if (lt < 0)
                continue;
            if (type == GL_SAMPLER_2D) {
                /* a sampler the real program lost: its unit by name */
                GLint unit = !strncmp (name, "g_Texture", 9) ? name[9] - '0' : 0;
                if (lf >= 0)
                    glGetUniformiv (from, lf, &unit);
                glUniform1i (lt, unit);
                continue;
            }
            if (lf < 0)
                continue;
            GLfloat f[16] = { 0 };
            GLint iv[4] = { 0 };
            switch (type) {
            case GL_FLOAT:      glGetUniformfv (from, lf, f); glUniform1f (lt, f[0]); break;
            case GL_FLOAT_VEC2: glGetUniformfv (from, lf, f); glUniform2fv (lt, 1, f); break;
            case GL_FLOAT_VEC3: glGetUniformfv (from, lf, f); glUniform3fv (lt, 1, f); break;
            case GL_FLOAT_VEC4: glGetUniformfv (from, lf, f); glUniform4fv (lt, 1, f); break;
            case GL_FLOAT_MAT3: glGetUniformfv (from, lf, f); glUniformMatrix3fv (lt, 1, GL_FALSE, f); break;
            case GL_FLOAT_MAT4: glGetUniformfv (from, lf, f); glUniformMatrix4fv (lt, 1, GL_FALSE, f); break;
            case GL_INT:
            case GL_BOOL:       glGetUniformiv (from, lf, iv); glUniform1i (lt, iv[0]); break;
            default:            break;
            }
        }
    }
}

static double fxs_cost_cpu (void)
{
    return (double) clock () / CLOCKS_PER_SEC;
}

/* fx_chain_run, right behind a pass's draw (program, textures, VAO,
 * target and viewport still bound) */
static void fxs_cost_draw (EffectChain *ch, EffPass *pass, int index)
{
    (void) ch;
    if (fxs_cost_frame_no () < 1 || fxs_cost_frame != fxs_cost_frame_no ())
        return;
    struct FxSpec *sp = pass->spec;
    GLint vp[4] = { 0 }, fbo = 0, tex0 = 0;
    glGetIntegerv (GL_VIEWPORT, vp);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    int seq = fxs_cost_seq++;
    fxs_cost_dump_target (seq, vp);
    /* time: the same draw again, five times */
    double best = 1e9;
    glFinish ();
    for (int r = 0; r < 5; r++) {
        double t0 = fxs_cost_cpu ();
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        glFinish ();
        double t1 = fxs_cost_cpu ();
        best = MIN (best, t1 - t0);
    }
    double avg = -1, mx = -1;
    if (sp && sp->fsrc_final && sp->vsrc_final) {
        if (!sp->count_prog) {
            char *cs = fxs_cost_count_source (sp->fsrc_final);
            sp->count_prog = cs ? fxs_link_quiet (sp->vsrc_final, cs, "fetch counter") : 0;
            g_free (cs);
        }
        if (sp->count_prog) {
            GLuint t = 0, f = 0;
            fxs_cost_copy_uniforms (pass->program, sp->count_prog);
            /* (on whichever unit is active: the executor shadows the active
             * unit and the bindings, and both are as before afterwards) */
            glGetIntegerv (GL_TEXTURE_BINDING_2D, &tex0);
            glGenTextures (1, &t);
            glBindTexture (GL_TEXTURE_2D, t);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, vp[2], vp[3], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glBindTexture (GL_TEXTURE_2D, (GLuint) tex0);
            glGenFramebuffers (1, &f);
            glBindFramebuffer (GL_FRAMEBUFFER, f);
            glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
            glClearColor (0, 0, 0, 0);
            glClear (GL_COLOR_BUFFER_BIT);
            glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
            gsize npx = (gsize) vp[2] * (gsize) vp[3];
            guint8 *px = g_malloc (npx * 4 + 4);
            glReadPixels (0, 0, vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, px);
            double sum = 0;
            int m = 0;
            gsize shaded = 0;
            for (gsize i = 0; i < npx; i++) {
                if (!px[i * 4 + 3])
                    continue;   /* not shaded (outside the quad, discarded) */
                int c = px[i * 4] * 256 + px[i * 4 + 1];
                sum += c;
                m = MAX (m, c);
                shaded++;
            }
            g_free (px);
            avg = shaded ? sum / (double) shaded : 0;
            mx = m;
            glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) fbo);
            glDeleteFramebuffers (1, &f);
            glDeleteTextures (1, &t);
            glUseProgram (pass->program);
        }
    }
    fprintf (stderr, "fxcost\t%s\tprog=%016" G_GINT64_MODIFIER "x\tw=%d\th=%d\tfetch_avg=%.3f\t"
             "fetch_max=%.0f\tcpu_us=%.1f\tidx=%d\tseq=%d\n",
             sp && sp->label ? sp->label : "layer=?\tshader=?\teffect=?\tpass=?",
             sp && sp->prog ? sp->prog->key[0] : (guint64) 0, vp[2], vp[3], avg, mx, best * 1e6,
             index, seq);
}
