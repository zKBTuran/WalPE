/* bloom.c - bloom of a scene, see bloom.h */
#include "engine/bloom.h"
#include "engine/clip.h"
#include "engine/glutil.h"
#include "engine/glfeat.h"     /* float render targets on an OpenGL ES context */
#include "engine/gpuprof.h"
#include <epoxy/gl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* the Gaussian of the blur, in taps of the full chain (one tap = 8 pixels
 * of the reference size). A sigma of 7/3 taps gives the 13 weights of the
 * standard chain to six decimal places. */
#define BLOOM_SIGMA (7.0f / 3.0f)
/* a glow that cannot reach this value anywhere changes no pixel: a quarter
 * of the step of a 10-bit frame (an 8-bit step is four times as large) */
#define BLOOM_EPSILON (0.25f / 1023.0f)

/* ------------------------------------------------------------ no GL */

static float bloom_strength (float s)
{
    return isfinite (s) ? CLAMP (s, 0.0f, WALPE_BLOOM_MAX_STRENGTH) : 0.0f;
}

static float bloom_threshold (float t)
{
    return isfinite (t) ? CLAMP (t, 0.0f, 1.0f) : 1.0f;
}

static float bloom_tint (const float tint[3], int i)
{
    if (!tint)
        return 1.0f;
    return isfinite (tint[i]) ? CLAMP (tint[i], 0.0f, 16.0f) : 1.0f;
}

void walpe_bloom_bright (const float rgb[3], float strength, float threshold, const float tint[3],
                         float out[3])
{
    const float s = bloom_strength (strength), t = bloom_threshold (threshold);
    const float top = MAX (MAX (rgb[0], rgb[1]), rgb[2]);
    const float keep = CLAMP (top - t, 0.0f, 1.0f);
    const float c[3] = { rgb[0] * keep, rgb[1] * keep, rgb[2] * keep };
    const float grey = 0.2989f * c[0] + 0.5870f * c[1] + 0.1140f * c[2];
    for (int i = 0; i < 3; i++)
        out[i] = MAX (0.0f, (c[i] * 2.0f - grey) * s * bloom_tint (tint, i));
}

gboolean walpe_bloom_visible (const WalpeBloomParams *p)
{
    if (!p || !isfinite (p->strength) || !isfinite (p->threshold))
        return FALSE;
    const float s = bloom_strength (p->strength), t = bloom_threshold (p->threshold);
    const float tint = MAX (MAX (bloom_tint (p->tint, 0), bloom_tint (p->tint, 1)),
                            bloom_tint (p->tint, 2));
    if (!(s > 0) || !(t < 1) || !(tint > 0))
        return FALSE;
    /* the most the bright pass can give for a colour of an 8- or 10-bit
     * frame: every channel is at most 1, so what stays above the threshold
     * is at most 1 - t, and 2 c - grey (c) is at most 2 c. The blur and the
     * enlargement only average. */
    return 2.0f * (1.0f - t) * s * tint >= BLOOM_EPSILON;
}

int walpe_bloom_kernel (int level, float w[13])
{
    const int n = level == WALPE_BLOOM_REDUCED ? 7 : 13;
    const float pitch = level == WALPE_BLOOM_REDUCED ? 2.0f : 1.0f;   /* taps of the full chain */
    float sum = 0;
    memset (w, 0, 13 * sizeof (float));
    for (int i = 0; i < n; i++) {
        const float d = (float) (i - n / 2) * pitch;
        w[i] = expf (-d * d / (2.0f * BLOOM_SIGMA * BLOOM_SIGMA));
        sum += w[i];
    }
    for (int i = 0; i < n; i++)
        w[i] /= sum;
    return n;
}

static int ceil_div (int x, int d)
{
    return MAX (1, (x + d - 1) / d);
}

gboolean walpe_bloom_plan (int frame_w, int frame_h, int ref_w, int ref_h, WalpeBloomSource source,
                           int level, WalpeBloomPlan *plan)
{
    memset (plan, 0, sizeof (*plan));
    if (frame_w < WALPE_BLOOM_MIN_SIDE || frame_h < WALPE_BLOOM_MIN_SIDE || frame_w > 65536 ||
        frame_h > 65536)
        return FALSE;
    if (ref_w < 1)
        ref_w = frame_w;
    if (ref_h < 1)
        ref_h = frame_h;
    const gboolean reduced = level == WALPE_BLOOM_REDUCED;
    const int div = reduced ? 8 : 4;   /* the bright target; the blur targets are half of it */
    int src_div = 1;
    plan->source = source;
    switch (source) {
    case WALPE_BLOOM_SRC_HALF_COPY:
        src_div = 2;
        plan->copy_w = ceil_div (frame_w, 2);
        plan->copy_h = ceil_div (frame_h, 2);
        plan->src_w = plan->copy_w;
        plan->src_h = plan->copy_h;
        break;
    case WALPE_BLOOM_SRC_FULL_COPY:
        plan->copy_w = frame_w;
        plan->copy_h = frame_h;
        /* fall through */
    default:
        plan->src_w = frame_w;
        plan->src_h = frame_h;
        break;
    }
    plan->bright_w = ceil_div (frame_w, div);
    plan->bright_h = ceil_div (frame_h, div);
    /* one bright pixel covers div / src_div source texels a side; a linear
     * lookup on the corner between four texels reads 2 x 2 of them */
    plan->bright_taps = div / src_div / 2;
    plan->blur_w = ceil_div (frame_w, div * 2);
    plan->blur_h = ceil_div (frame_h, div * 2);
    plan->blur_taps = reduced ? 7 : 13;
    /* 8 pixels of the reference size from tap to tap (16 at the reduced
     * level, which has every second tap): the glow reaches as far on the
     * monitor whatever size the frame is rendered at */
    plan->blur_step[0] = (reduced ? 16.0f : 8.0f) / (float) ref_w;
    plan->blur_step[1] = (reduced ? 16.0f : 8.0f) / (float) ref_h;
    plan->px_small = (guint64) plan->bright_w * plan->bright_h +
                     2 * (guint64) plan->blur_w * plan->blur_h;
    plan->px_add = (guint64) frame_w * frame_h;
    plan->bytes = 4 * ((guint64) plan->copy_w * plan->copy_h + plan->px_small);
    return TRUE;
}

/* --------------------------------------------------------------- GL */

typedef struct {
    GLuint tex, fbo;
    int w, h;
} BloomTarget;

struct WalpeBloom {
    /* programs of this context (a scene's render thread); the bright pass
     * and the blur have their tap counts compiled in */
    GLuint prog_bright, prog_blur, prog_add;
    int bright_taps, blur_taps;
    GLint lb_tex, lb_texel, lb_strength, lb_threshold, lb_tint;
    GLint ll_tex, ll_step, ll_w;
    GLint la_tex;
    GLuint sampler;        /* linear, clamped: the frame's own texture is read with it
                            * whatever filter that texture has */
    BloomTarget copy, bright, blur[2];
    /* what the targets were made for */
    WalpeBloomPlan plan;
    GLenum ifmt;           /* 0 = no targets */
    guint for_fbo;
    int for_w, for_h;
    int multisampled;      /* of for_fbo: -1 = not looked up */
    int float_ok;          /* RGBA16F can be rendered to here: -1 = not asked */
    gboolean failed;
};

static const char *const BLOOM_VS =
    "in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    /* every pass maps its target onto its source in framebuffer space:
     * the texture coordinate follows from the position, nothing is flipped */
    "void main(){ v_uv = a_pos * 0.5 + 0.5; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *const BLOOM_FS_BRIGHT =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texel;\n"
    "uniform float u_strength;\n"
    "uniform float u_threshold;\n"
    "uniform vec3 u_tint;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  vec3 c = vec3(0.0);\n"
    "  for (int j = 0; j < TAPS; j++)\n"
    "    for (int i = 0; i < TAPS; i++)\n"
    "      c += texture(u_tex, v_uv + u_texel * vec2(float(2 * i - (TAPS - 1)),\n"
    "                                                float(2 * j - (TAPS - 1)))).rgb;\n"
    "  c *= 1.0 / float(TAPS * TAPS);\n"
    "  c *= clamp(max(max(c.r, c.g), c.b) - u_threshold, 0.0, 1.0);\n"
    "  float grey = dot(vec3(0.2989, 0.5870, 0.1140), c);\n"
    "  c = c * 2.0 - vec3(grey);\n"
    "  frag = vec4(max(vec3(0.0), c * u_strength * u_tint), 1.0);\n"
    "}\n";

static const char *const BLOOM_FS_BLUR =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_step;\n"
    "uniform float u_w[TAPS];\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  vec3 c = vec3(0.0);\n"
    "  for (int i = 0; i < TAPS; i++)\n"
    "    c += texture(u_tex, v_uv + u_step * float(i - TAPS / 2)).rgb * u_w[i];\n"
    "  frag = vec4(c, 1.0);\n"
    "}\n";

static const char *const BLOOM_FS_ADD =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 frag;\n"
    /* alpha 0 with a one / one blend: the frame's alpha stays what it is */
    "void main(){ frag = vec4(texture(u_tex, v_uv).rgb, 0.0); }\n";

static GLuint bloom_shader (GLenum type, const char *defs, const char *body, const char *what)
{
    const char *src[3] = { type == GL_FRAGMENT_SHADER ? walpe_gl_shader_fs_header ()
                                                      : walpe_gl_shader_header (),
                           defs ? defs : "", body };
    GLuint sh = glCreateShader (type);
    glShaderSource (sh, 3, src, NULL);
    glCompileShader (sh);
    GLint ok = 0;
    glGetShaderiv (sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog (sh, sizeof (log), NULL, log);
        g_warning ("bloom: the %s shader does not compile: %s", what, log);
        glDeleteShader (sh);
        return 0;
    }
    return sh;
}

static GLuint bloom_program (int taps, const char *fs_body, const char *what)
{
    char defs[32] = "";
    if (taps > 0)
        g_snprintf (defs, sizeof (defs), "#define TAPS %d\n", taps);
    GLuint vs = bloom_shader (GL_VERTEX_SHADER, NULL, BLOOM_VS, what);
    GLuint fs = vs ? bloom_shader (GL_FRAGMENT_SHADER, defs, fs_body, what) : 0;
    if (!vs || !fs) {
        if (vs)
            glDeleteShader (vs);
        return 0;
    }
    GLuint p = glCreateProgram ();
    glAttachShader (p, vs);
    glAttachShader (p, fs);
    glBindAttribLocation (p, 0, "a_pos");
    glLinkProgram (p);
    glDeleteShader (vs);
    glDeleteShader (fs);
    GLint ok = 0;
    glGetProgramiv (p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetProgramInfoLog (p, sizeof (log), NULL, log);
        g_warning ("bloom: the %s program does not link: %s", what, log);
        glDeleteProgram (p);
        return 0;
    }
    return p;
}

static gboolean bloom_programs (WalpeBloom *b, const WalpeBloomPlan *pl)
{
    if (!b->prog_bright || b->bright_taps != pl->bright_taps) {
        if (b->prog_bright)
            glDeleteProgram (b->prog_bright);
        b->prog_bright = bloom_program (pl->bright_taps, BLOOM_FS_BRIGHT, "bright pass");
        b->bright_taps = pl->bright_taps;
        if (b->prog_bright) {
            b->lb_tex = glGetUniformLocation (b->prog_bright, "u_tex");
            b->lb_texel = glGetUniformLocation (b->prog_bright, "u_texel");
            b->lb_strength = glGetUniformLocation (b->prog_bright, "u_strength");
            b->lb_threshold = glGetUniformLocation (b->prog_bright, "u_threshold");
            b->lb_tint = glGetUniformLocation (b->prog_bright, "u_tint");
        }
    }
    if (!b->prog_blur || b->blur_taps != pl->blur_taps) {
        if (b->prog_blur)
            glDeleteProgram (b->prog_blur);
        b->prog_blur = bloom_program (pl->blur_taps, BLOOM_FS_BLUR, "blur");
        b->blur_taps = pl->blur_taps;
        if (b->prog_blur) {
            b->ll_tex = glGetUniformLocation (b->prog_blur, "u_tex");
            b->ll_step = glGetUniformLocation (b->prog_blur, "u_step");
            b->ll_w = glGetUniformLocation (b->prog_blur, "u_w");
        }
    }
    if (!b->prog_add) {
        b->prog_add = bloom_program (0, BLOOM_FS_ADD, "add");
        if (b->prog_add)
            b->la_tex = glGetUniformLocation (b->prog_add, "u_tex");
    }
    if (!b->sampler) {
        glGenSamplers (1, &b->sampler);
        glSamplerParameteri (b->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri (b->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri (b->sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri (b->sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return b->prog_bright && b->prog_blur && b->prog_add && b->sampler;
}

static void bloom_target_free (BloomTarget *t)
{
    if (t->tex)
        glDeleteTextures (1, &t->tex);
    if (t->fbo)
        glDeleteFramebuffers (1, &t->fbo);
    memset (t, 0, sizeof (*t));
}

/* a w x h colour target of format ifmt with linear filtering; leaves its
 * framebuffer bound. FALSE = the context cannot render to it. */
static gboolean bloom_target_make (BloomTarget *t, int w, int h, GLenum ifmt)
{
    bloom_target_free (t);
    glGenTextures (1, &t->tex);
    glBindTexture (GL_TEXTURE_2D, t->tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D (GL_TEXTURE_2D, 0, (GLint) ifmt, w, h, 0, GL_RGBA,
                  ifmt == GL_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers (1, &t->fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    t->w = w;
    t->h = h;
    return glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

/* The format of the intermediates - the one place that decides it.
 * RGBA8 is what the standard chain has and what every context this tree
 * runs on (GL 3.3, GLES 3.0) can render to and filter. Half float is asked
 * for by a scene with general.hdr (or WALPE_BLOOM_FORMAT=16f); whether the
 * context can render to it is found out by attaching one, once: desktop GL
 * can, GLES only with a colour-buffer-float extension. A tree that has a
 * table of what the driver offers asks that table here instead. */
static GLenum bloom_format (WalpeBloom *b, gboolean hdr)
{
    static gint forced = -1;   /* 0 = by the scene, 8, 16 */
    int f = g_atomic_int_get (&forced);
    if (f < 0) {
        const char *e = g_getenv ("WALPE_BLOOM_FORMAT");
        f = e && *e == '8' ? 8 : e && !strncmp (e, "16", 2) ? 16 : 0;
        g_atomic_int_set (&forced, f);
    }
    if (f == 8 || (f == 0 && !hdr))
        return GL_RGBA8;
    if (b->float_ok < 0) {
        /* an OpenGL ES context that is known not to render to float
         * formats (engine/glfeat.h; desktop OpenGL answers "native" and is
         * asked nothing): no attempt. Else a 4x4 target is the judge. */
        if (walpe_gl_feat_how (WALPE_GLF_FLOAT_TARGETS) == WALPE_GLF_REPLACED) {
            b->float_ok = 0;
        } else {
            BloomTarget probe = { 0 };
            b->float_ok = bloom_target_make (&probe, 4, 4, GL_RGBA16F) ? 1 : 0;
            bloom_target_free (&probe);
            while (glGetError () != GL_NO_ERROR)
                ;
        }
        if (!b->float_ok)
            g_message ("bloom: this GL cannot render to half-float targets: 8-bit intermediates "
                       "(what exceeds 1 after the bright pass is clamped)");
    }
    return b->float_ok ? GL_RGBA16F : GL_RGBA8;
}

/* WALPE_BLOOM_COPY=full: a frame in the window is copied 1:1 instead of at
 * half size (A/B: the half-size copy leads to the same picture up to the
 * rounding of one 8-bit average, at a quarter of the pixels) */
static gboolean bloom_full_copy (void)
{
    static gint full = -1;
    int v = g_atomic_int_get (&full);
    if (v < 0) {
        const char *e = g_getenv ("WALPE_BLOOM_COPY");
        v = e && !strcmp (e, "full");
        g_atomic_int_set (&full, v);
    }
    return v;
}

static void bloom_targets_free (WalpeBloom *b)
{
    bloom_target_free (&b->copy);
    bloom_target_free (&b->bright);
    bloom_target_free (&b->blur[0]);
    bloom_target_free (&b->blur[1]);
    b->ifmt = 0;
    b->for_w = b->for_h = 0;
    b->multisampled = -1;
    memset (&b->plan, 0, sizeof (b->plan));
}

WalpeBloom *walpe_bloom_new (void)
{
    WalpeBloom *b = g_new0 (WalpeBloom, 1);
    b->multisampled = -1;
    b->float_ok = -1;
    return b;
}

void walpe_bloom_drop_targets (WalpeBloom *b)
{
    if (b && b->ifmt)
        bloom_targets_free (b);
}

void walpe_bloom_free (WalpeBloom *b)
{
    if (!b)
        return;
    bloom_targets_free (b);
    if (b->prog_bright)
        glDeleteProgram (b->prog_bright);
    if (b->prog_blur)
        glDeleteProgram (b->prog_blur);
    if (b->prog_add)
        glDeleteProgram (b->prog_add);
    if (b->sampler)
        glDeleteSamplers (1, &b->sampler);
    g_free (b);
}

/* the targets for this frame, level and format (made anew only when one
 * of the three changed); leaves any framebuffer bound */
static gboolean bloom_prepare (WalpeBloom *b, const WalpeBloomFrame *frame,
                               const WalpeBloomParams *p)
{
    const GLenum ifmt = bloom_format (b, p->hdr);
    const int level = p->level == WALPE_BLOOM_REDUCED ? WALPE_BLOOM_REDUCED : WALPE_BLOOM_FULL;
    WalpeBloomSource source = WALPE_BLOOM_SRC_TEXTURE;
    if (!frame->tex) {
        /* the frame cannot be sampled: it is copied, at half size where a
         * blit may scale (not from a multisampled buffer) */
        if (b->multisampled < 0 || b->for_fbo != frame->fbo) {
            GLint sb = 0;
            glBindFramebuffer (GL_FRAMEBUFFER, frame->fbo);
            glGetIntegerv (GL_SAMPLE_BUFFERS, &sb);
            b->multisampled = sb > 0;
        }
        source = b->multisampled || bloom_full_copy () ? WALPE_BLOOM_SRC_FULL_COPY
                                                       : WALPE_BLOOM_SRC_HALF_COPY;
    }
    WalpeBloomPlan pl;
    if (!walpe_bloom_plan (frame->w, frame->h, frame->ref_w, frame->ref_h, source, level, &pl))
        return FALSE;
    if (b->ifmt == ifmt && b->for_fbo == frame->fbo && b->for_w == frame->w &&
        b->for_h == frame->h && b->plan.source == pl.source && b->plan.copy_w == pl.copy_w &&
        b->plan.copy_h == pl.copy_h && b->plan.bright_w == pl.bright_w &&
        b->plan.bright_h == pl.bright_h && b->plan.blur_w == pl.blur_w &&
        b->plan.blur_h == pl.blur_h) {
        b->plan = pl;   /* the steps follow the reference size without new targets */
        return TRUE;
    }
    const int ms = b->multisampled;
    bloom_targets_free (b);
    b->multisampled = ms;
    gboolean ok = bloom_target_make (&b->bright, pl.bright_w, pl.bright_h, ifmt) &&
                  bloom_target_make (&b->blur[0], pl.blur_w, pl.blur_h, ifmt) &&
                  bloom_target_make (&b->blur[1], pl.blur_w, pl.blur_h, ifmt);
    if (ok && pl.copy_w)
        ok = bloom_target_make (&b->copy, pl.copy_w, pl.copy_h, ifmt);
    if (!ok) {
        bloom_targets_free (b);
        return FALSE;
    }
    b->plan = pl;
    b->ifmt = ifmt;
    b->for_fbo = frame->fbo;
    b->for_w = frame->w;
    b->for_h = frame->h;
    return TRUE;
}

/* what the scene renderer expects to find after a layer */
static void bloom_leave (const WalpeBloomFrame *frame)
{
    glBindSampler (0, 0);
    glBindVertexArray (0);
    glUseProgram (0);
    glBindFramebuffer (GL_FRAMEBUFFER, frame->fbo);
    glViewport (0, 0, frame->w, frame->h);
    glEnable (GL_BLEND);
    glBlendEquation (GL_FUNC_ADD);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

gboolean walpe_bloom_apply (WalpeBloom *b, const WalpeBloomFrame *frame,
                            const WalpeBloomParams *p, WalpeBloomStats *stats)
{
    if (stats)
        memset (stats, 0, sizeof (*stats));
    if (!b || !frame || !p || b->failed)
        return FALSE;
    if (!walpe_bloom_visible (p) || frame->w < WALPE_BLOOM_MIN_SIDE ||
        frame->h < WALPE_BLOOM_MIN_SIDE)
        return TRUE;   /* nothing to add: the frame is not touched */

    glActiveTexture (GL_TEXTURE0);
    if (!bloom_prepare (b, frame, p) || !bloom_programs (b, &b->plan)) {
        b->failed = TRUE;
        bloom_targets_free (b);
        while (glGetError () != GL_NO_ERROR)
            ;
        g_warning ("bloom: no render targets or shaders on this GL - the scene is shown "
                   "without bloom");
        bloom_leave (frame);
        return FALSE;
    }
    const WalpeBloomPlan *pl = &b->plan;

    /* 1. a frame that is no texture: one copy, at half size where the
     * blit may scale (its linear filter is the first halving) */
    if (pl->copy_w) {
        WALPE_PROF_PASS_BEGIN ("bloom: copy of the frame", -1, 0, pl->copy_w, pl->copy_h);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, frame->fbo);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, b->copy.fbo);
        glBlitFramebuffer (0, 0, frame->w, frame->h, 0, 0, pl->copy_w, pl->copy_h,
                           GL_COLOR_BUFFER_BIT,
                           pl->source == WALPE_BLOOM_SRC_FULL_COPY ? GL_NEAREST : GL_LINEAR);
        WALPE_PROF_PASS_END ();
    }

    walpe_glutil_bind_quad_vao ();
    glDisable (GL_BLEND);
    glBindSampler (0, b->sampler);

    /* 2. bright pass: source -> a quarter (reduced: an eighth) */
    WALPE_PROF_PASS_BEGIN ("bloom: bright pass", -1, 1, pl->bright_w, pl->bright_h);
    glBindFramebuffer (GL_FRAMEBUFFER, b->bright.fbo);
    glViewport (0, 0, pl->bright_w, pl->bright_h);
    glUseProgram (b->prog_bright);
    glBindTexture (GL_TEXTURE_2D, pl->copy_w ? b->copy.tex : frame->tex);
    glUniform1i (b->lb_tex, 0);
    glUniform2f (b->lb_texel, 1.0f / (float) pl->src_w, 1.0f / (float) pl->src_h);
    glUniform1f (b->lb_strength, bloom_strength (p->strength));
    glUniform1f (b->lb_threshold, bloom_threshold (p->threshold));
    glUniform3f (b->lb_tint, bloom_tint (p->tint, 0), bloom_tint (p->tint, 1),
                 bloom_tint (p->tint, 2));
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    WALPE_PROF_PASS_END ();

    /* 3. + 4. blur at half the bright target's size: along x out of the
     * bright target (each tap on the corner between four of its pixels:
     * the halving is part of the lookup), then along y */
    float kw[13];
    const int taps = walpe_bloom_kernel (p->level, kw);
    glUseProgram (b->prog_blur);
    glUniform1i (b->ll_tex, 0);
    glUniform1fv (b->ll_w, taps, kw);
    glViewport (0, 0, pl->blur_w, pl->blur_h);
    WALPE_PROF_PASS_BEGIN ("bloom: blur x", -1, 2, pl->blur_w, pl->blur_h);
    glBindFramebuffer (GL_FRAMEBUFFER, b->blur[0].fbo);
    glBindTexture (GL_TEXTURE_2D, b->bright.tex);
    glUniform2f (b->ll_step, pl->blur_step[0], 0.0f);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    WALPE_PROF_PASS_END ();
    WALPE_PROF_PASS_BEGIN ("bloom: blur y", -1, 3, pl->blur_w, pl->blur_h);
    glBindFramebuffer (GL_FRAMEBUFFER, b->blur[1].fbo);
    glBindTexture (GL_TEXTURE_2D, b->blur[0].tex);
    glUniform2f (b->ll_step, 0.0f, pl->blur_step[1]);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    WALPE_PROF_PASS_END ();

    /* 5. frame + glow, where the frame can be seen */
    WALPE_PROF_PASS_BEGIN ("bloom: add to the frame", -1, 4, frame->w, frame->h);
    glBindFramebuffer (GL_FRAMEBUFFER, frame->fbo);
    glViewport (0, 0, frame->w, frame->h);
    glEnable (GL_BLEND);
    glBlendEquation (GL_FUNC_ADD);
    glBlendFunc (GL_ONE, GL_ONE);
    glUseProgram (b->prog_add);
    glBindTexture (GL_TEXTURE_2D, b->blur[1].tex);
    glUniform1i (b->la_tex, 0);
    for (int it = 0; walpe_clip_next (&it, NULL);)
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    WALPE_PROF_PASS_END ();

    bloom_leave (frame);
    if (stats) {
        stats->drawn = TRUE;
        stats->px = pl->px_small + pl->px_add;
        stats->passes = 4;
        stats->copied = pl->copy_w > 0;
        stats->half_float = b->ifmt == GL_RGBA16F;
    }
    return TRUE;
}
