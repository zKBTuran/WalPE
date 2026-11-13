#include "glutil.h"
#include "glcaps.h"
#include "glfeat.h"
#include "clip.h"
#include "gpuprof.h"   /* GPUPROF hooks: WALPE_PROF_QUAD in the quad draws */
#include "yuvconv.h"
#include "common/gifcheck.h"
#include "common/pichead.h"
#include "common/premult.h"
#include "common/util.h"
#include "common/lazy.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <math.h>
#if defined (__SSE2__)
#include <emmintrin.h>
#endif

static __thread GLuint g_prog = 0; /* thread-local: one EGL context per monitor thread */
static __thread GLuint g_vao = 0, g_vbo = 0;
static __thread GLint loc_mvp = -1, loc_tex = -1, loc_op = -1, loc_pm = -1, loc_tint = -1;
/* what the quad program's uniforms hold (set by draw_setup only): a layer
 * draw sent all five again although opacity, premultiplication and tint
 * are the same for most layers and the sampler never changes */
static __thread float q_op = -1.0f, q_pm = -1.0f, q_tint[3] = { -1.0f, -1.0f, -1.0f };
static void *g_egl_display = NULL;

void walpe_gl_set_egl_display (void *egl_display)
{
    g_egl_display = egl_display;
}

void *walpe_gl_get_egl_display (void)
{
    return g_egl_display;
}

/* The two header texts are worked out by whichever render thread compiles
 * the first shader and then read by all of them: one thread builds, the
 * others wait and read (g_once_init_enter). The fragment header is a heap
 * string on GLES: a pointer handed to other threads through a plain static
 * could be seen before the text it points to. (The daemon also lets only
 * one monitor thread at a time through its first walpe_glutil_init; the
 * tests and tools that render on threads of their own do not.) Shader
 * compilation only: not on a frame's path. */
const char *walpe_gl_shader_header (void)
{
    static const char *hdr;
    static gsize once;
    if (g_once_init_enter (&once)) {
        if (epoxy_is_desktop_gl ()) {
            hdr = "#version 330\n";
        } else {
            /* GLES: the GLSL ES of the context's version (epoxy: major * 10
             * + minor, 32 for ES 3.2), and highp as the default for
             * everything a shader of this program computes with. GLSL ES's
             * own defaults are mediump int and lowp samplers in the
             * fragment stage: on a GPU that really has those precisions
             * the picture would differ from desktop OpenGL's, and a
             * uniform declared in both stages would have two precisions (a
             * link error). */
#define WALPE_ES_PRECISION "precision highp float;\nprecision highp int;\n" \
                           "precision highp sampler2D;\n"
            int v = walpe_gl_essl_version (epoxy_gl_version ());
            if (v >= 320)
                hdr = "#version 320 es\n" WALPE_ES_PRECISION;
            else if (v >= 310)
                hdr = "#version 310 es\n" WALPE_ES_PRECISION;
            else
                hdr = "#version 300 es\n" WALPE_ES_PRECISION;
#undef WALPE_ES_PRECISION
        }
        g_once_init_leave (&once, 1);
    }
    return hdr;
}

/* fragment shaders need an explicit float precision under ESSL (there is
 * no default in the fragment stage): the GLES header above declares it,
 * with the same defaults for both stages */
const char *walpe_gl_shader_fs_header (void)
{
    static char *hdr;
    static gsize once;
    if (g_once_init_enter (&once)) {
        if (epoxy_is_desktop_gl ())
            hdr = g_strdup ("#version 330\n");
        else
            hdr = g_strdup (walpe_gl_shader_header ());
        g_once_init_leave (&once, 1);
    }
    return hdr;
}

/* ---- BGRA bytes into a texture
 * Pictures, text and frames are BGRA in memory (cairo, the .tex loader,
 * the video and web modules). Desktop OpenGL takes that as the client
 * format of an RGBA8 texture; OpenGL ES does not. */

/* 0 = desktop OpenGL: RGBA8 texture, GL_BGRA client format (the calls the
 * call sites made themselves); 1 = OpenGL ES with
 * GL_EXT_texture_format_BGRA8888: a GL_BGRA_EXT texture; 2 = OpenGL ES
 * without it: the bytes are reordered here, RGBA8 texture */
static int bgra_mode (void)
{
    if (!walpe_gl_feat_es ())
        return 0;
    return walpe_gl_feat_how (WALPE_GLF_BGRA_UPLOAD) == WALPE_GLF_EXTENSION ? 1 : 2;
}

/* w x h pixels of px as tight RGBA rows; the rows of px are
 * GL_UNPACK_ROW_LENGTH pixels apart when that is set. NULL: no memory. */
static guint8 *bgra_to_rgba (int w, int h, const guint8 *px, GLint *row_length)
{
    *row_length = 0;
    glGetIntegerv (GL_UNPACK_ROW_LENGTH, row_length);
    const gsize stride = (gsize) (*row_length > 0 ? *row_length : w) * 4;
    guint8 *o = w > 0 && h > 0 ? g_try_malloc ((gsize) w * (gsize) h * 4) : NULL;
    if (!o)
        return NULL;
    for (int y = 0; y < h; y++) {
        const guint8 *s = px + (gsize) y * stride;
        guint8 *d = o + (gsize) y * (gsize) w * 4;
        for (int x = 0; x < w; x++, s += 4, d += 4) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
        }
    }
    return o;
}

void walpe_gl_tex_image_bgra (int w, int h, const void *px)
{
    switch (bgra_mode ()) {
    case 0:
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, px);
        break;
    case 1:
        /* the extension's unsized format: internal format = format */
        glTexImage2D (GL_TEXTURE_2D, 0, GL_BGRA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, px);
        break;
    default: {
        GLint rl = 0;
        guint8 *sw = px ? bgra_to_rgba (w, h, px, &rl) : NULL;
        if (sw && rl)
            glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, sw);
        if (sw && rl)
            glPixelStorei (GL_UNPACK_ROW_LENGTH, rl);
        g_free (sw);
    }
    }
}

void walpe_gl_tex_sub_image_bgra (int x, int y, int w, int h, const void *px)
{
    if (bgra_mode () != 2) {
        glTexSubImage2D (GL_TEXTURE_2D, 0, x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
        return;
    }
    GLint rl = 0;
    guint8 *sw = px ? bgra_to_rgba (w, h, px, &rl) : NULL;
    if (!sw)
        return;
    if (rl)
        glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D (GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, sw);
    if (rl)
        glPixelStorei (GL_UNPACK_ROW_LENGTH, rl);
    g_free (sw);
}

static const char *VS =
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "uniform mat3 u_mvp;\n"
    "out vec2 v_uv;\n"
    "void main(){ v_uv = a_uv;\n"
    "  vec2 p = (u_mvp * vec3(a_pos, 1.0)).xy;\n"
    "  gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char *FS =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_opacity;\n"
    "uniform float u_premult;\n"
    "uniform vec3 u_tint;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    /* straight alpha -> premult first, then the layer opacity (it was
     * applied before the conversion: a straight texture's colour got
     * opacity twice, a 25 % white panel drew as 6 % grey, i.e. darker) */
    "  vec4 c = texture(u_tex, v_uv);\n"
    "  c.rgb *= mix(c.a, 1.0, u_premult);\n"
    "  c *= u_opacity;\n"
    "  c.rgb *= u_tint;\n"
    "  frag = c; }\n";

static GLuint compile (GLenum type, const char *body)
{
    const char *hdr = type == GL_FRAGMENT_SHADER ? walpe_gl_shader_fs_header ()
                                                 : walpe_gl_shader_header ();
    char *src = g_strconcat (hdr, body, NULL);
    GLuint s = glCreateShader (type);
    glShaderSource (s, 1, (const char **) &src, NULL);
    glCompileShader (s);
    g_free (src);
    GLint ok = 0;
    glGetShaderiv (s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog (s, sizeof (log), NULL, log);
        g_critical ("shader compile failed: %s", log);
    }
    return s;
}

static gint g_software = -1;   /* -1 unknown, set by the first walpe_glutil_init */

gboolean walpe_gl_is_software (void)
{
    return g_atomic_int_get (&g_software) == 1;
}

/* bytes per texel of an uncompressed internal format (approximate) */
static int fmt_bpp (GLint f)
{
    switch (f) {
    case GL_R8: return 1;
    case GL_RG8: case GL_R16F: return 2;
    case GL_RGB8: return 3;
    case GL_RGBA16F: return 8;
    case GL_RGBA32F: return 16;
    default: return 4;
    }
}

/* bytes of one 2D texture, all its levels (0 where the levels cannot be
 * asked: GLES below 3.1, not a 2D texture); *comp = it is compressed */
gint64 walpe_gl_tex_bytes (GLuint tex, gboolean *comp)
{
    if (comp)
        *comp = FALSE;
    if (!tex || !epoxy_is_desktop_gl ())
        return 0;
    GLint prev = 0;
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &prev);
    while (glGetError () != GL_NO_ERROR)
        ;
    glBindTexture (GL_TEXTURE_2D, tex);
    gint64 bytes = 0;
    if (glGetError () == GL_NO_ERROR) {
        for (GLint lv = 0; lv < 16; lv++) {
            GLint w = 0, h = 0, f = 0, c = 0, cs = 0;
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_HEIGHT, &h);
            if (w < 1 || h < 1)
                break;
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_INTERNAL_FORMAT, &f);
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_COMPRESSED, &c);
            if (c) {
                glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &cs);
                bytes += cs;
                if (comp)
                    *comp = TRUE;
            } else {
                bytes += (gint64) w * h * fmt_bpp (f);
            }
        }
    }
    glBindTexture (GL_TEXTURE_2D, (GLuint) prev);
    while (glGetError () != GL_NO_ERROR)
        ;
    return bytes;
}

/* every 2D texture of this context and the contexts it shares with (GL
 * has no enumeration: the small, sequential name space is probed) */
void walpe_gl_tex_totals (int *count, gint64 *bytes, gint64 *comp_bytes)
{
    *count = 0;
    *bytes = *comp_bytes = 0;
    if (!epoxy_is_desktop_gl ())
        return;
    int misses = 0;
    for (GLuint name = 1; misses < 4096; name++) {
        if (!glIsTexture (name)) {
            misses++;
            continue;
        }
        misses = 0;
        gboolean c = FALSE;
        gint64 b = walpe_gl_tex_bytes (name, &c);
        if (b <= 0)
            continue;
        (*count)++;
        *bytes += b;
        if (c)
            *comp_bytes += b;
    }
}

void walpe_gl_mem_report (const char *tag)
{
    /* GL has no object enumeration: probe the (small, sequential) name
     * space of this context. Diagnostics only (WALPE_GPU_MEM=1). */
    gint64 total = 0, comp = 0;
    const char *ev = g_getenv ("WALPE_GPU_MEM");
    gboolean verbose = ev && (*ev == '2' || *ev == '3');
    gint64 vmin = ev && *ev == '3' ? 0 : 2 << 20; /* 3: every texture */
    int n = 0, nbig = 0, misses = 0;
    GLint prev = 0;
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &prev);
    for (GLuint name = 1; misses < 4096; name++) {
        if (!glIsTexture (name)) {
            misses++;
            continue;
        }
        misses = 0;
        glBindTexture (GL_TEXTURE_2D, name);
        if (glGetError () != GL_NO_ERROR)
            continue; /* not a 2D texture */
        gint64 bytes = 0;
        for (GLint lv = 0; lv < 16; lv++) {
            GLint w = 0, h = 0, f = 0, c = 0, cs = 0;
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_HEIGHT, &h);
            if (w < 1 || h < 1)
                break;
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_INTERNAL_FORMAT, &f);
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_COMPRESSED, &c);
            if (c) {
                glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &cs);
                bytes += cs;
                if (lv == 0)
                    comp += cs;
            } else {
                bytes += (gint64) w * h * fmt_bpp (f);
            }
            if (lv == 0 && w * h >= 1920 * 1080)
                nbig++;
        }
        n++;
        total += bytes;
        if (verbose && bytes >= vmin) {
            GLint w = 0, h = 0, f = 0, lv = 0;
            glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
            glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &f);
            while (lv < 15) {
                GLint ww = 0;
                glGetTexLevelParameteriv (GL_TEXTURE_2D, lv + 1, GL_TEXTURE_WIDTH, &ww);
                if (ww < 1)
                    break;
                lv++;
            }
            g_message ("gpumem %s:   tex %u %dx%d fmt 0x%x mips %d: %.1f MiB", tag, name, w, h, f,
                       lv, bytes / 1048576.0);
        }
    }
    glBindTexture (GL_TEXTURE_2D, (GLuint) prev);
    while (glGetError () != GL_NO_ERROR)
        ;
    g_message ("gpumem %s: %d textures, %.1f MiB (%.1f MiB compressed), %d >= 1920x1080",
               tag, n, total / 1048576.0, comp / 1048576.0, nbig);
}

void walpe_glutil_init (void)
{
    if (g_prog)
        return;
    if (g_atomic_int_get (&g_software) < 0) {
        /* CPU rasterizers run "GPU" work on the same cores: shader-side
         * work (I420->RGB) is then a loss, not a saving */
        const char *r = (const char *) glGetString (GL_RENDERER);
        gboolean soft = r && (strstr (r, "llvmpipe") || strstr (r, "softpipe") ||
                              strstr (r, "Software") || strstr (r, "SwiftShader") ||
                              strstr (r, "swrast"));
        g_atomic_int_set (&g_software, soft ? 1 : 0);
        const char *vd = (const char *) glGetString (GL_VENDOR);
        g_message ("gl: renderer \"%s\" (%s)%s", r ? r : "?", vd ? vd : "?",
                   soft ? " (software)" : "");
    }
    /* which driver this is and what it offers, read once per process: the
     * optional fast paths and the format choices ask it (glcaps.h) */
    walpe_gl_caps_init ();
    /* desktop OpenGL or OpenGL ES, and what the ES path replaces (asks
     * nothing of a desktop context) */
    walpe_gl_feat_init ();
    GLuint vs = compile (GL_VERTEX_SHADER, VS);
    GLuint fs = compile (GL_FRAGMENT_SHADER, FS);
    g_prog = glCreateProgram ();
    glAttachShader (g_prog, vs);
    glAttachShader (g_prog, fs);
    /* bind BEFORE linking: the linker assigns locations arbitrarily and the
     * VAO below hardcodes a_pos=0, a_uv=1 (swapped locations collapse the
     * quad into a sub-pixel sliver — invisible wallpaper) */
    glBindAttribLocation (g_prog, 0, "a_pos");
    glBindAttribLocation (g_prog, 1, "a_uv");
    glLinkProgram (g_prog);
    glDetachShader (g_prog, vs);
    glDetachShader (g_prog, fs);
    GLint ok = 0;
    glGetProgramiv (g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog (g_prog, sizeof (log), NULL, log);
        g_critical ("program link failed: %s", log);
        /* drop the broken program so a later init (better context) retries */
        glDeleteProgram (g_prog);
        g_prog = 0;
    }
    glDeleteShader (vs);
    glDeleteShader (fs);
    loc_mvp = glGetUniformLocation (g_prog, "u_mvp");
    loc_tex = glGetUniformLocation (g_prog, "u_tex");
    loc_op = glGetUniformLocation (g_prog, "u_opacity");
    loc_pm = glGetUniformLocation (g_prog, "u_premult");
    loc_tint = glGetUniformLocation (g_prog, "u_tint");
    if (g_prog) {
        /* the sampler is program state: set once (was sent per draw) */
        glUseProgram (g_prog);
        glUniform1i (loc_tex, 0);
    }
    q_op = q_pm = q_tint[0] = q_tint[1] = q_tint[2] = -1.0f;
    glEnable (GL_BLEND);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    /* strip quad: pos(2) uv(2); v=0 at clip top — matches top-down uploads
     * (GL puts the first uploaded row at t=0), so images draw right-side-up */
    static const float verts[4][4] = {
        { -1.0f, -1.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 1.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 1.0f, 0.0f },
    };
    glGenVertexArrays (1, &g_vao);
    glBindVertexArray (g_vao);
    glGenBuffers (1, &g_vbo);
    glBindBuffer (GL_ARRAY_BUFFER, g_vbo);
    glBufferData (GL_ARRAY_BUFFER, sizeof (verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 16, (void *) 0);
    glEnableVertexAttribArray (1);
    glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 16, (void *) 8);
    glBindVertexArray (0);
}

/* ------------------------------------------- I420 -> RGB on the GPU
 * The integer coefficients of yuvconv.c (walpe_yuv_coeffs: BT.601, BT.709
 * or BT.2020, limited or full range), so the GPU path matches the CPU
 * converter. */
static __thread GLuint y_prog;
static __thread GLint y_loc_mvp, y_loc_t[3];

static const char *YUV_FS =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_y, u_u, u_v;\n"
    "uniform vec2 u_ky;\n"      /* Y' gain, Y' offset */
    "uniform vec4 u_kc;\n"      /* R from V, G from U, G from V, B from U */
    "out vec4 frag;\n"
    "void main(){\n"
    "  float Y = max(texture(u_y, v_uv).r * 255.0 - u_ky.y, 0.0) * u_ky.x;\n"
    "  float U = texture(u_u, v_uv).r * 255.0 - 128.0;\n"
    "  float V = texture(u_v, v_uv).r * 255.0 - 128.0;\n"
    "  vec3 c = vec3(Y + u_kc.x * V, Y - u_kc.y * U - u_kc.z * V, Y + u_kc.w * U)\n"
    "           / 256.0 / 255.0;\n"
    "  frag = vec4(clamp(c, 0.0, 1.0), 1.0); }\n";

static __thread GLint y_loc_ky = -1, y_loc_kc = -1;

/* clip-space bounds (x0, y0, x1, y1) of the unit quad drawn with mvp */
static void quad_bounds (const float *mvp, float b[4])
{
    b[0] = b[1] = 1e9f;
    b[2] = b[3] = -1e9f;
    for (int k = 0; k < 4; k++) {
        float qx = (k & 1) ? 1.0f : -1.0f, qy = (k & 2) ? 1.0f : -1.0f;
        float cx = mvp[0] * qx + mvp[3] * qy + mvp[6];
        float cy = mvp[1] * qx + mvp[4] * qy + mvp[7];
        b[0] = fminf (b[0], cx); b[2] = fmaxf (b[2], cx);
        b[1] = fminf (b[1], cy); b[3] = fmaxf (b[3], cy);
    }
}

void walpe_glutil_draw_yuv (GLuint ty, GLuint tu, GLuint tv, const float *mvp)
{
    walpe_glutil_draw_yuv_range (ty, tu, tv, mvp, FALSE);
}

void walpe_glutil_draw_yuv_range (GLuint ty, GLuint tu, GLuint tv, const float *mvp,
                                  guint mode)
{
    WALPE_PROF_QUAD (mvp);   /* GPUPROF */
    if (!y_prog) {
        GLuint vs = compile (GL_VERTEX_SHADER, VS);
        GLuint fs = compile (GL_FRAGMENT_SHADER, YUV_FS);
        y_prog = glCreateProgram ();
        glAttachShader (y_prog, vs);
        glAttachShader (y_prog, fs);
        glBindAttribLocation (y_prog, 0, "a_pos");
        glBindAttribLocation (y_prog, 1, "a_uv");
        glLinkProgram (y_prog);
        glDetachShader (y_prog, vs);
        glDetachShader (y_prog, fs);
        glDeleteShader (vs);
        glDeleteShader (fs);
        y_loc_mvp = glGetUniformLocation (y_prog, "u_mvp");
        y_loc_t[0] = glGetUniformLocation (y_prog, "u_y");
        y_loc_t[1] = glGetUniformLocation (y_prog, "u_u");
        y_loc_t[2] = glGetUniformLocation (y_prog, "u_v");
        y_loc_ky = glGetUniformLocation (y_prog, "u_ky");
        y_loc_kc = glGetUniformLocation (y_prog, "u_kc");
    }
    glUseProgram (y_prog);
    int k[6];
    walpe_yuv_coeffs (mode, k);
    glUniform2f (y_loc_ky, (float) k[0], (float) k[1]);
    glUniform4f (y_loc_kc, (float) k[2], (float) k[3], (float) k[4], (float) k[5]);
    GLuint t[3] = { ty, tu, tv };
    for (int i = 0; i < 3; i++) {
        glActiveTexture (GL_TEXTURE0 + i);
        glBindTexture (GL_TEXTURE_2D, t[i]);
        glUniform1i (y_loc_t[i], i);
    }
    glActiveTexture (GL_TEXTURE0);
    glUniformMatrix3fv (y_loc_mvp, 1, GL_FALSE, mvp);
    glBlendFunc (GL_ONE, GL_ZERO);
    glBindVertexArray (g_vao);
    float qb[4];
    quad_bounds (mvp, qb);
    for (int it = 0; walpe_clip_next (&it, qb);)    /* once per visible box (clip.h) */
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray (0);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void walpe_glutil_bind_quad_vao (void)
{
    if (g_vao)
        glBindVertexArray (g_vao);
}

/* ------------------------------------------- WE emulated blend modes
 * Layers with a WE colorBlendMode that no GL blend state reproduces are
 * drawn with blending off by a shader that reads a copy of the pixels
 * under the layer: out = mix(dst, f(dst, src), src alpha * opacity).
 * The formulas are the usual Photoshop-style definitions (per channel
 * unless noted). */
static const char *BLEND_FS =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_dst;\n"
    "uniform vec4 u_dstrect;\n"   /* x, y, w, h of the copy in window pixels */
    "uniform float u_opacity;\n"
    "uniform float u_premult;\n"
    "uniform vec3 u_tint;\n"
    "uniform int u_mode;\n"
    "out vec4 frag;\n"
    "float lum(vec3 c){ return dot(c, vec3(0.3, 0.59, 0.11)); }\n"
    "vec3 rgb2hsl(vec3 c){\n"
    "  float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b);\n"
    "  float l = (mx + mn) * 0.5, d = mx - mn;\n"
    "  if (d <= 0.0) return vec3(0.0, 0.0, l);\n"
    "  float s = l < 0.5 ? d / (mx + mn) : d / (2.0 - mx - mn);\n"
    "  float h;\n"
    "  if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);\n"
    "  else if (mx == c.g) h = (c.b - c.r) / d + 2.0;\n"
    "  else h = (c.r - c.g) / d + 4.0;\n"
    "  return vec3(h / 6.0, s, l); }\n"
    "float hue2c(float p, float q, float t){\n"
    "  t = fract(t);\n"
    "  if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;\n"
    "  if (t < 0.5) return q;\n"
    "  if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;\n"
    "  return p; }\n"
    "vec3 hsl2rgb(vec3 h){\n"
    "  if (h.y <= 0.0) return vec3(h.z);\n"
    "  float q = h.z < 0.5 ? h.z * (1.0 + h.y) : h.z + h.y - h.z * h.y;\n"
    "  float p = 2.0 * h.z - q;\n"
    "  return vec3(hue2c(p, q, h.x + 1.0/3.0), hue2c(p, q, h.x), hue2c(p, q, h.x - 1.0/3.0)); }\n"
    "vec3 burn(vec3 a, vec3 b){ return mix(max(1.0 - (1.0 - a) / max(b, 1e-5), 0.0), vec3(0.0), vec3(lessThanEqual(b, vec3(0.0)))); }\n"
    "vec3 dodge(vec3 a, vec3 b){ return mix(min(a / max(1.0 - b, 1e-5), 1.0), vec3(1.0), vec3(greaterThanEqual(b, vec3(1.0)))); }\n"
    "vec3 overlay(vec3 a, vec3 b){ return mix(2.0 * a * b, 1.0 - 2.0 * (1.0 - a) * (1.0 - b), step(0.5, a)); }\n"
    "vec3 reflectf(vec3 a, vec3 b){ return mix(min(a * a / max(1.0 - b, 1e-5), 1.0), vec3(1.0), vec3(greaterThanEqual(b, vec3(1.0)))); }\n"
    "vec3 vivid(vec3 a, vec3 b){ return mix(burn(a, 2.0 * b), dodge(a, 2.0 * (b - 0.5)), step(0.5, b)); }\n"
    "vec3 f(int m, vec3 a, vec3 b){\n"
    "  if (m == 1) return min(a, b);\n"
    "  if (m == 2) return a * b;\n"
    "  if (m == 3) return burn(a, b);\n"
    "  if (m == 4 || m == 20) return max(a + b - 1.0, 0.0);\n"
    "  if (m == 5) return lum(b) < lum(a) ? b : a;\n"
    "  if (m == 6) return max(a, b);\n"
    "  if (m == 7) return 1.0 - (1.0 - a) * (1.0 - b);\n"
    "  if (m == 8) return dodge(a, b);\n"
    "  if (m == 9) return min(a + b, 1.0);\n"
    "  if (m == 10) return lum(b) > lum(a) ? b : a;\n"
    "  if (m == 11) return overlay(a, b);\n"
    "  if (m == 12) return mix(2.0 * a * b + a * a * (1.0 - 2.0 * b), sqrt(max(a, 0.0)) * (2.0 * b - 1.0) + 2.0 * a * (1.0 - b), step(0.5, b));\n"
    "  if (m == 13) return overlay(b, a);\n"
    "  if (m == 14) return vivid(a, b);\n"
    "  if (m == 15) return mix(max(a + 2.0 * b - 1.0, 0.0), a + 2.0 * (b - 0.5), step(0.5, b));\n"
    "  if (m == 16) return mix(min(a, 2.0 * b), max(a, 2.0 * (b - 0.5)), step(0.5, b));\n"
    "  if (m == 17) return step(0.5, vivid(a, b));\n"
    "  if (m == 18) return abs(a - b);\n"
    "  if (m == 19) return a + b - 2.0 * a * b;\n"
    "  if (m == 21) return reflectf(a, b);\n"
    "  if (m == 22) return reflectf(b, a);\n"
    "  if (m == 23) return min(a, b) - max(a, b) + 1.0;\n"
    "  if (m == 24) return (a + b) * 0.5;\n"
    "  if (m == 25) return 1.0 - abs(1.0 - a - b);\n"
    "  if (m == 26) { vec3 h = rgb2hsl(a); return hsl2rgb(vec3(rgb2hsl(b).x, h.y, h.z)); }\n"
    "  if (m == 27) { vec3 h = rgb2hsl(a); return hsl2rgb(vec3(h.x, rgb2hsl(b).y, h.z)); }\n"
    "  if (m == 28) { vec3 h = rgb2hsl(b); return hsl2rgb(vec3(h.x, h.y, rgb2hsl(a).z)); }\n"
    "  if (m == 29) { vec3 h = rgb2hsl(a); return hsl2rgb(vec3(h.x, h.y, rgb2hsl(b).z)); }\n"
    "  if (m == 30) return vec3(max(a.r, max(a.g, a.b))) * b;\n"
    "  if (m == 32) return a + a * b;\n"
    "  return b; }\n"
    "void main(){\n"
    "  vec4 t = texture(u_tex, v_uv);\n"
    "  vec3 b = t.rgb / mix(1.0, max(t.a, 1e-5), u_premult);\n" /* straight color */
    "  vec4 c = t * u_opacity;\n"
    "  b *= u_tint;\n"
    "  vec4 d = texture(u_dst, (gl_FragCoord.xy - u_dstrect.xy) / u_dstrect.zw);\n"
    "  float o = clamp(c.a, 0.0, 1.0);\n"
    "  vec3 r = u_mode == 31 ? d.rgb + b * o : mix(d.rgb, f(u_mode, d.rgb, b), o);\n"
    "  frag = vec4(r, d.a); }\n";

static __thread GLuint b_prog, b_tex, b_fbo;
static __thread int b_tw, b_th;
static __thread GLint b_loc_mvp, b_loc_tex, b_loc_dst, b_loc_rect, b_loc_op, b_loc_pm, b_loc_tint,
                      b_loc_mode;

int walpe_blend_from_we (int m)
{
    switch (m) {
    case 0:  return WALPE_BLEND_NORMAL;
    case 2:  return WALPE_BLEND_MULTIPLY;    /* mix(A, A*B, a) = GL dst-color blend */
    case 7:  return WALPE_BLEND_SCREEN;      /* A + a*B*(1-A) */
    case 31: return WALPE_BLEND_ADDITIVE;    /* A + B*a */
    default:
        return m > 0 && m <= 32 ? WALPE_BLEND_WE + m : WALPE_BLEND_NORMAL;
    }
}

static gboolean blend_prog_init (void)
{
    if (b_prog)
        return TRUE;
    GLuint vs = compile (GL_VERTEX_SHADER, VS);
    GLuint fs = compile (GL_FRAGMENT_SHADER, BLEND_FS);
    GLuint p = glCreateProgram ();
    glAttachShader (p, vs);
    glAttachShader (p, fs);
    glBindAttribLocation (p, 0, "a_pos");
    glBindAttribLocation (p, 1, "a_uv");
    glLinkProgram (p);
    glDetachShader (p, vs);
    glDetachShader (p, fs);
    glDeleteShader (vs);
    glDeleteShader (fs);
    GLint ok = 0;
    glGetProgramiv (p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog (p, sizeof (log), NULL, log);
        g_warning ("blend program link failed (WE blend modes drawn as normal): %s", log);
        glDeleteProgram (p);
        return FALSE;
    }
    b_prog = p;
    b_loc_mvp = glGetUniformLocation (p, "u_mvp");
    b_loc_tex = glGetUniformLocation (p, "u_tex");
    b_loc_dst = glGetUniformLocation (p, "u_dst");
    b_loc_rect = glGetUniformLocation (p, "u_dstrect");
    b_loc_op = glGetUniformLocation (p, "u_opacity");
    b_loc_pm = glGetUniformLocation (p, "u_premult");
    b_loc_tint = glGetUniformLocation (p, "u_tint");
    b_loc_mode = glGetUniformLocation (p, "u_mode");
    return TRUE;
}

/* copy the pixels of the bound draw framebuffer under clip rect
 * [x0,x1]x[y0,y1] (clamped to the viewport) into b_tex and set up the
 * blend program; FALSE = nothing to draw / no program (caller falls
 * back to normal blending) */
static gboolean blend_setup (GLuint tex, gboolean premult, float opacity, const float *mvp,
                             int mode, const float tint[3],
                             float x0, float y0, float x1, float y1)
{
    if (!blend_prog_init ())
        return FALSE;
    GLint vp[4], drawfb = 0, readfb = 0;
    glGetIntegerv (GL_VIEWPORT, vp);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &drawfb);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &readfb);
    /* both ends inside the view (a rectangle far outside it gave pixel
     * numbers no int holds: undefined in the casts below; it is empty
     * either way) */
    x0 = fminf (fmaxf (x0, -1.0f), 1.0f); y0 = fminf (fmaxf (y0, -1.0f), 1.0f);
    x1 = fmaxf (fminf (x1, 1.0f), -1.0f); y1 = fmaxf (fminf (y1, 1.0f), -1.0f);
    int px0 = vp[0] + (int) floorf ((x0 * 0.5f + 0.5f) * vp[2]);
    int py0 = vp[1] + (int) floorf ((y0 * 0.5f + 0.5f) * vp[3]);
    int px1 = vp[0] + (int) ceilf ((x1 * 0.5f + 0.5f) * vp[2]);
    int py1 = vp[1] + (int) ceilf ((y1 * 0.5f + 0.5f) * vp[3]);
    int cw = px1 - px0, ch = py1 - py0;
    if (cw < 1 || ch < 1)
        return FALSE;
    if (!b_tex || cw > b_tw || ch > b_th) {
        /* grow-only scratch texture (one per thread/context) */
        int nw = MAX (cw, b_tw), nh = MAX (ch, b_th);
        if (!b_tex)
            glGenTextures (1, &b_tex);
        glBindTexture (GL_TEXTURE_2D, b_tex);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, nw, nh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        if (!b_fbo)
            glGenFramebuffers (1, &b_fbo);
        glBindFramebuffer (GL_FRAMEBUFFER, b_fbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b_tex, 0);
        b_tw = nw;
        b_th = nh;
    }
    glBindFramebuffer (GL_READ_FRAMEBUFFER, drawfb);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, b_fbo);
    glBlitFramebuffer (px0, py0, px1, py1, 0, 0, cw, ch, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, drawfb);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, readfb);

    glUseProgram (b_prog);
    glActiveTexture (GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, b_tex);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, tex);
    glUniform1i (b_loc_tex, 0);
    glUniform1i (b_loc_dst, 1);
    /* the scratch texture may be larger than the copy: scale the rect */
    glUniform4f (b_loc_rect, (float) px0, (float) py0, (float) b_tw, (float) b_th);
    glUniformMatrix3fv (b_loc_mvp, 1, GL_FALSE, mvp);
    glUniform1f (b_loc_op, opacity);
    glUniform1f (b_loc_pm, premult ? 1.0f : 0.0f);
    glUniform3f (b_loc_tint, tint[0], tint[1], tint[2]);
    glUniform1i (b_loc_mode, mode);
    glDisable (GL_BLEND);
    return TRUE;
}

/* The scratch texture grows to the largest area a layer with one of
 * Wallpaper Engine's blend modes covered - up to the frame - and used to
 * stay for the life of the render thread: a frame of RGBA8 per monitor that
 * outlived the wallpaper it was made for, a pause and an idle release.
 * It goes with the scene that used it and when the scene gives its targets
 * back; the next such draw makes it again at the size it needs. */
void walpe_glutil_blend_release (void)
{
    if (b_tex)
        glDeleteTextures (1, &b_tex);
    if (b_fbo)
        glDeleteFramebuffers (1, &b_fbo);
    b_tex = b_fbo = 0;
    b_tw = b_th = 0;
}

static void blend_done (void)
{
    glActiveTexture (GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, 0);
    glActiveTexture (GL_TEXTURE0);
    glEnable (GL_BLEND);
}

void walpe_glutil_draw_quad (GLuint tex, gboolean premult, float opacity,
                             const float *mvp, int blend)
{
    static const float white[3] = { 1.0f, 1.0f, 1.0f };
    walpe_glutil_draw_quad_tint (tex, premult, opacity, mvp, blend, white);
}

static void draw_setup (GLuint tex, gboolean premult, float opacity,
                        const float *mvp, int blend, const float tint[3]);

/* WALPE_GL_DRAW_CACHE=0: every layer draw sends opacity, premultiplication
 * and tint and resets the blend state afterwards, as before (A/B) */
static gboolean draw_cache (void)
{
    static WalpeLazy cell = WALPE_LAZY_UNSET;   /* (per layer draw: common/lazy.h) */
    int on = walpe_lazy_get (&cell);
    if (on < 0) {
        const char *e = g_getenv ("WALPE_GL_DRAW_CACHE");
        on = walpe_lazy_set (&cell, !(e && *e == '0'));
    }
    return on;
}

/* back to the blend state every draw starts from (add, premultiplied
 * over). Normal blending never left it (draw_setup set exactly that):
 * nothing to send - two calls less per layer. */
static void draw_restore (int blend)
{
    if (!draw_cache ()) {
        glBlendEquation (GL_FUNC_ADD);
        glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        return;
    }
    switch (blend) {
    case WALPE_BLEND_SUBTRACTIVE:
        glBlendEquation (GL_FUNC_ADD);
        glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case WALPE_BLEND_NONE:
    case WALPE_BLEND_ADDITIVE:
    case WALPE_BLEND_MULTIPLY:
    case WALPE_BLEND_SCREEN:
        glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    default:
        break;
    }
}

void walpe_glutil_draw_quad_tint (GLuint tex, gboolean premult, float opacity,
                                  const float *mvp, int blend, const float tint[3])
{
    if (!g_prog)
        return;
    WALPE_PROF_QUAD (mvp);   /* GPUPROF: pixels the quad covers */
    /* clip-space bounds of the unit quad (corners at +-1) */
    float qb[4];
    quad_bounds (mvp, qb);
    if (blend > WALPE_BLEND_WE) {
        float x0 = qb[0], y0 = qb[1], x1 = qb[2], y1 = qb[3];
        if (blend_setup (tex, premult, opacity, mvp, blend - WALPE_BLEND_WE, tint, x0, y0, x1, y1)) {
            glBindVertexArray (g_vao);
            for (int it = 0; walpe_clip_next (&it, qb);)
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray (0);
            blend_done ();
            return;
        }
        if (x1 < -1.0f || x0 > 1.0f || y1 < -1.0f || y0 > 1.0f)
            return;   /* off screen */
        blend = WALPE_BLEND_NORMAL;
    }
    draw_setup (tex, premult, opacity, mvp, blend, tint);
    glBindVertexArray (g_vao);
    /* once, or once per visible box of the frame with that box as the
     * scissor rectangle ("Skip hidden areas", clip.h) */
    for (int it = 0; walpe_clip_next (&it, qb);)
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray (0);
    draw_restore (blend);
}

/* indexed triangle mesh (VAO: attrib 0 = pos vec2, 1 = uv vec2, element
 * buffer of GLushort) with the quad shader and blend modes */
void walpe_glutil_draw_mesh_tint (GLuint tex, gboolean premult, float opacity,
                                  const float *mvp, int blend, const float tint[3],
                                  GLuint vao, int index_count)
{
    if (!g_prog || !vao || index_count < 3)
        return;
    WALPE_PROF_QUAD (mvp);   /* GPUPROF: the mesh's quad (estimate) */
    if (blend > WALPE_BLEND_WE) {
        /* mesh bounds unknown here: copy the whole viewport */
        if (blend_setup (tex, premult, opacity, mvp, blend - WALPE_BLEND_WE, tint, -1, -1, 1, 1)) {
            glBindVertexArray (vao);
            for (int it = 0; walpe_clip_next (&it, NULL);)
                glDrawElements (GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, (void *) 0);
            glBindVertexArray (0);
            blend_done ();
            return;
        }
        blend = WALPE_BLEND_NORMAL;
    }
    draw_setup (tex, premult, opacity, mvp, blend, tint);
    glBindVertexArray (vao);
    for (int it = 0; walpe_clip_next (&it, NULL);)   /* mesh bounds unknown here */
        glDrawElements (GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, (void *) 0);
    glBindVertexArray (0);
    draw_restore (blend);
}

static void draw_setup (GLuint tex, gboolean premult, float opacity,
                        const float *mvp, int blend, const float tint[3])
{
    glUseProgram (g_prog);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, tex);
    glUniformMatrix3fv (loc_mvp, 1, GL_FALSE, mvp);
    /* sent when they differ from what the program holds */
    const gboolean ucache = draw_cache ();
    const float pm = premult ? 1.0f : 0.0f;
    if (!ucache || q_op != opacity) {
        glUniform1f (loc_op, opacity);
        q_op = opacity;
    }
    if (!ucache || q_pm != pm) {
        glUniform1f (loc_pm, pm);
        q_pm = pm;
    }
    if (!ucache || q_tint[0] != tint[0] || q_tint[1] != tint[1] || q_tint[2] != tint[2]) {
        glUniform3f (loc_tint, tint[0], tint[1], tint[2]);
        memcpy (q_tint, tint, sizeof (q_tint));
    }

    /* effect passes run with blending off; every layer draw must turn it
     * back on (a layer drawn after an effect chain otherwise replaced the
     * frame: transparent pixels became opaque black - the all-black
     * Hoshi-yoku / NGNL boxes) */
    glEnable (GL_BLEND);
    switch (blend) {
    case WALPE_BLEND_NONE:
        glBlendFunc (GL_ONE, GL_ZERO);
        break;
    case WALPE_BLEND_ADDITIVE:
        glBlendFunc (GL_ONE, GL_ONE);
        break;
    case WALPE_BLEND_SUBTRACTIVE:
        glBlendEquation (GL_FUNC_REVERSE_SUBTRACT);
        glBlendFunc (GL_ONE, GL_ONE);
        break;
    case WALPE_BLEND_MULTIPLY:
        glBlendFuncSeparate (GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        break;
    case WALPE_BLEND_SCREEN:
        glBlendFunc (GL_ONE_MINUS_DST_COLOR, GL_ONE);
        break;
    default: /* normal, premultiplied */
        glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    }
}

gboolean walpe_gl_has_s3tc (void)
{
    /* per-context in theory; every context here comes from one driver.
     * The capability table decides (and logs the CPU decode once when the
     * driver has no S3TC); before it exists the context is asked. */
    static WalpeLazy cell = WALPE_LAZY_UNSET;
    int has = walpe_lazy_get (&cell);
    if (has < 0) {
        if (!walpe_gl_caps ())
            return epoxy_has_gl_extension ("GL_EXT_texture_compression_s3tc");
        has = walpe_lazy_set (&cell,
                              walpe_gl_format (WALPE_FMT_TEX_DXT5).how == WALPE_FMT_NATIVE ? 1 : 0);
    }
    return has == 1;
}

/* ---- minification: mipmaps ------------------------------------------
 * A texture drawn smaller than its pixels (a 4K-authored scene on a 1080p
 * or 1440p monitor, a zoomed-out camera, small sprites) was sampled with
 * GL_LINEAR from level 0 only: beyond 2:1 that skips texels, which shows
 * as shimmering, jagged thin lines and "pixelated" motion. WE samples
 * through the mip chain its editor stored in the .tex (trilinear), unless
 * the author imported the texture without mipmaps or with point sampling.
 * Same here, per texture (walpe_gl_tex_sampling); for the big cached layer
 * textures the chain is built only once it is needed (upload_tex). WALPE_TEX_MIPMAPS=0
 * restores level-0-only sampling (A/B); WALPE_TEX_ANISO=N sets the
 * anisotropy (default 4 on hardware GL, off on software GL; 0 = off);
 * WALPE_TEX_LOD_BIAS=b the trilinear LOD bias (default -0.5, below). */
static gint tc_shared;   /* textures are shared between monitor contexts (below) */

gboolean walpe_gl_mipmaps_enabled (void)
{
    static gint on = -1;
    if (g_atomic_int_get (&on) < 0) {
        const char *e = g_getenv ("WALPE_TEX_MIPMAPS");
        g_atomic_int_set (&on, !(e && *e == '0'));
    }
    return g_atomic_int_get (&on) == 1;
}

static float tex_anisotropy (void)
{
    static gint v = -1;    /* x16, per process: every context is one driver */
    if (g_atomic_int_get (&v) < 0) {
        float a = walpe_gl_is_software () ? 0.0f : 4.0f;
        const char *e = g_getenv ("WALPE_TEX_ANISO");
        if (e && *e)
            a = (float) g_ascii_strtod (e, NULL);
        const WalpeGlCaps *gc = walpe_gl_caps ();
        if (a > 1.0f && (gc ? gc->anisotropic
                            : epoxy_has_gl_extension ("GL_EXT_texture_filter_anisotropic"))) {
            GLfloat mx = 1.0f;
            glGetFloatv (GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &mx);
            a = fminf (a, mx);
        } else {
            a = 0.0f;
        }
        g_atomic_int_set (&v, (gint) (a * 16.0f));
    }
    return g_atomic_int_get (&v) / 16.0f;
}

/* LOD bias for trilinear sampling (WALPE_TEX_LOD_BIAS, default -0.5).
 * Unbiased trilinear blends in the next smaller level as soon as a texture
 * is minified at all, which softens text and line art drawn at 1.2-1.9:1
 * (measured: 3219908811's notice text went visibly blurry). At -0.5 a
 * texture is sampled from level 0 alone up to 1.41:1, where bilinear does
 * not alias yet, and each level is used between 0.7 and 1.41 texels per
 * pixel: sharp, and still no skipped texels. Desktop GL only (GLES has no
 * such texture parameter: plain trilinear there). */
static float tex_lod_bias (void)
{
    static gint v = G_MININT;   /* x256 */
    if (g_atomic_int_get (&v) == G_MININT) {
        const char *e = g_getenv ("WALPE_TEX_LOD_BIAS");
        float b = e && *e ? (float) g_ascii_strtod (e, NULL) : -0.5f;
        if (!epoxy_is_desktop_gl ())
            b = 0.0f;
        g_atomic_int_set (&v, (gint) lroundf (CLAMP (b, -2.0f, 2.0f) * 256.0f));
    }
    return g_atomic_int_get (&v) / 256.0f;
}

void walpe_gl_tex_sampling (gboolean mipmapped, gboolean nearest)
{
    if (mipmapped && walpe_gl_mipmaps_enabled ()) {
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                         nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
        float a = nearest ? 0.0f : tex_anisotropy ();
        if (a > 1.0f)
            glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, a);
        float b = nearest ? 0.0f : tex_lod_bias ();
        if (b != 0.0f)
            glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, b);
    } else {
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    }
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
}

void walpe_gl_tex_gen_mipmaps (int w, int h)
{
    if (!walpe_gl_mipmaps_enabled () || (w <= 1 && h <= 1)) {
        walpe_gl_tex_sampling (FALSE, FALSE);
        return;
    }
    glGenerateMipmap (GL_TEXTURE_2D);
    walpe_gl_tex_sampling (TRUE, FALSE);
}

gboolean walpe_gl_tex_defer_mipmaps (void)
{
    static gint ok = -1;
    if (g_atomic_int_get (&ok) < 0)
        g_atomic_int_set (&ok, walpe_gl_mipmaps_enabled () && epoxy_is_desktop_gl () &&
                               !g_getenv ("WALPE_TEX_LOD_BIAS"));
    if (g_atomic_int_get (&ok) != 1)
        return FALSE;
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    float a = tex_anisotropy ();
    if (a > 1.0f)
        glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, a);
    return TRUE;
}

void walpe_gl_tex_ensure_mipmaps (GLuint tex)
{
    if (!tex || !walpe_gl_mipmaps_enabled ())
        return;
    GLint f = GL_LINEAR, mag = GL_LINEAR, w = 0, h = 0, comp = 0;
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &f);
    if (f == GL_LINEAR_MIPMAP_LINEAR || f == GL_NEAREST_MIPMAP_NEAREST)
        return;
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &comp);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (comp || (w <= 1 && h <= 1))
        return;
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag);
    glGenerateMipmap (GL_TEXTURE_2D);
    /* the levels exist before the filter that needs them is selected: a
     * monitor context sharing the texture never sees it incomplete */
    if (g_atomic_int_get (&tc_shared))
        glFinish ();
    walpe_gl_tex_sampling (TRUE, mag == GL_NEAREST);
}

void walpe_gl_tex_lod_bias (float bias)
{
    if (walpe_gl_mipmaps_enabled () && epoxy_is_desktop_gl () && !g_getenv ("WALPE_TEX_LOD_BIAS"))
        glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, bias);
}

gboolean walpe_gl_tex_is_mipmapped (GLuint tex)
{
    GLint f = GL_LINEAR;
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &f);
    return f == GL_LINEAR_MIPMAP_LINEAR || f == GL_NEAREST_MIPMAP_NEAREST;
}

/* ---- texture memory switches (each restores the previous behaviour) --
 * WALPE_TEX_COMPACT=0      no 1/2-channel storage of RGBA8 data
 * WALPE_TEX_DEDUPE=0       no sharing of identical pixels under other names
 * WALPE_TEX_STORED_MIPS=0  reduced textures are made by halving level 0 on
 *                          the GPU, never taken from the file's own levels */
static gboolean env_on (const char *name, gint *state)
{
    gint v = g_atomic_int_get (state);
    if (v < 0) {
        const char *e = g_getenv (name);
        v = !(e && *e == '0');
        g_atomic_int_set (state, v);
    }
    return v == 1;
}

gboolean walpe_gl_tex_stored_levels (void)
{
    static gint st = -1;
    return env_on ("WALPE_TEX_STORED_MIPS", &st);
}

static gboolean tex_compact_on (void)
{
    static gint st = -1;
    return env_on ("WALPE_TEX_COMPACT", &st);
}

static gboolean tex_dedupe_on (void)
{
    static gint st = -1;
    return env_on ("WALPE_TEX_DEDUPE", &st);
}

/* WALPE_TEX_CPU_REDUCE=0  a reduced uncompressed texture is uploaded at
 *                         full size and halved with GL blits, as before
 * WALPE_TEX_CPU_REDUCE=2  (opt-in) pictures with an odd width or height are
 *                         reduced before the upload as well: a bilinear
 *                         resampling like the GL blit's, but not its exact
 *                         rounding - those textures differ slightly
 * WALPE_TEX_ASYNC=0       no decoding ahead on worker threads; pictures
 *                         stored as PNG / JPEG are decoded and uploaded at
 *                         full size while the scene is parsed, as before
 * WALPE_TEX_OWN_FULL=0    without a share group a reduced copy is made by
 *                         decoding the file again, as before */
static int tex_cpu_reduce_mode (void)
{
    static gint st = -1;
    gint v = g_atomic_int_get (&st);
    if (v < 0) {
        const char *e = g_getenv ("WALPE_TEX_CPU_REDUCE");
        v = e && *e == '0' ? 0 : e && *e == '2' ? 2 : 1;
        g_atomic_int_set (&st, v);
    }
    return v;
}

static gboolean tex_cpu_reduce_on (void)
{
    return tex_cpu_reduce_mode () > 0;
}

gboolean walpe_gl_tex_async (void)
{
    static gint st = -1;
    return env_on ("WALPE_TEX_ASYNC", &st);
}

static gboolean tex_own_full_on (void)
{
    static gint st = -1;
    return env_on ("WALPE_TEX_OWN_FULL", &st);
}

/* ---- GL bytes of the file textures made here: now, and the most at any
 * moment (the peak of a load: on a GPU this is video memory). Counted per
 * texture name, so right for one share group / one context. */
static GMutex tl_lock;
static GHashTable *tl_bytes;   /* GUINT_TO_POINTER (tex) -> gint64 * */
static gint64 tl_live, tl_peak;

static void tex_live_add (GLuint tex, gint64 bytes)
{
    if (!tex)
        return;
    g_mutex_lock (&tl_lock);
    if (!tl_bytes)
        tl_bytes = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
    gint64 *old = g_hash_table_lookup (tl_bytes, GUINT_TO_POINTER (tex));
    if (old)
        tl_live -= *old;
    gint64 *b = g_new (gint64, 1);
    *b = bytes;
    g_hash_table_insert (tl_bytes, GUINT_TO_POINTER (tex), b);
    tl_live += bytes;
    if (tl_live > tl_peak)
        tl_peak = tl_live;
    g_mutex_unlock (&tl_lock);
}

static void tex_live_del (GLuint tex)
{
    g_mutex_lock (&tl_lock);
    gint64 *old = tl_bytes ? g_hash_table_lookup (tl_bytes, GUINT_TO_POINTER (tex)) : NULL;
    if (old) {
        tl_live -= *old;
        g_hash_table_remove (tl_bytes, GUINT_TO_POINTER (tex));
    }
    g_mutex_unlock (&tl_lock);
}

void walpe_gl_tex_live_stats (gint64 *live, gint64 *peak, gboolean reset_peak)
{
    g_mutex_lock (&tl_lock);
    if (live)
        *live = tl_live;
    if (peak)
        *peak = tl_peak;
    if (reset_peak)
        tl_peak = tl_live;
    g_mutex_unlock (&tl_lock);
}

/* RGBA8 pixels that need fewer channels are stored with fewer: a mask the
 * WE editor writes as RGBA8 with R = G = B and A = 255 (or with red alone)
 * is one byte per texel, an R8/RG88 particle sprite expanded to white +
 * alpha (WALPE_TEX_LOAD_WE_COLOR) one or two. The texture's swizzle returns
 * exactly the four channels the RGBA8 texels held, to every sampler and
 * through every filter (each channel is filtered alone), so nothing drawn
 * changes. BGRA texels (after premultiplication) -> the layout that holds
 * them exactly, 0 = none:
 *   1: R8,  (R,R,R,1)  gray, opaque         2: R8, (R,0,0,1)  red only, opaque
 *   3: R8,  (R,R,R,R)  gray == alpha        4: RG8, (R,R,R,G) gray + alpha
 *   5: RG8, (R,G,0,1)  red + green, opaque  6: R8, (1,1,1,R)  white, alpha
 * One pass that stops at the first texel no layout can hold: a colour
 * picture costs a few texels. */
static int tex_compact_kind (const guint8 *d, gsize npix)
{
    gboolean gray = TRUE, opaque = TRUE, gb0 = TRUE, b0 = TRUE, ga = TRUE, white = TRUE;
    for (gsize i = 0; i < npix; i++, d += 4) {
        guint8 b = d[0], g = d[1], r = d[2], a = d[3];
        if (b != g || g != r)
            gray = FALSE;
        else if (r != a)
            ga = FALSE;
        if (r != 255)
            white = FALSE;
        if (a != 255)
            opaque = FALSE;
        if (b != 0)
            b0 = gb0 = FALSE;
        else if (g != 0)
            gb0 = FALSE;
        if (!gray && !(opaque && b0))
            return 0;
    }
    if (gray && opaque)
        return 1;
    if (opaque && gb0)
        return 2;
    if (gray && ga)
        return 3;
    if (gray && white)
        return 6;
    if (gray)
        return 4;
    return opaque && b0 ? 5 : 0;
}

static void tex_set_swizzle (GLint r, GLint g, GLint b, GLint a)
{
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, r);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, g);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, b);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, a);
}

/* upload_tex options and results */
typedef struct {
    /* in */
    gboolean defer_chain;   /* do not build a chain glGenerateMipmap would make now */
    int skip;               /* DXT: the texture starts at stored level `skip` */
    gboolean no_chain;      /* DXT: that level alone, without the levels below it */
    gboolean layer_swizzle; /* native R8/RG88 read as WE's genericimage reads
                             * them (ConvertTexture0Format, common_fragment.h):
                             * R8 = white with alpha R, RG88 = luminance R with
                             * alpha G; straight alpha */
    /* out */
    gboolean deferred;      /* a chain is wanted and not built
                             * (walpe_gl_tex_ensure_mipmaps builds it when the
                             * texture is first drawn beyond 2:1) */
    gboolean premult;
    gboolean compressed;
    int w, h;               /* texel size of level 0 as stored */
    gint64 bytes;           /* estimate, chain included */
} TexUp;

/* defer_chain: a chain costs a third of the texture: a layer drawn 1:1, a
 * layer with effects (its passes read it at 2:1 at most) or one stored
 * reduced never samples it. */
static GLuint upload_tex (WalpeTex *t, GLint wrap, gboolean premultiply, TexUp *u)
{
    GLuint tex = 0;
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    /* the author's import settings (tex.h): point sampling, and whether
     * the texture has a mip chain (a plain image file gets one: nothing
     * says it should alias) */
    gboolean nearest = (t->tex_flags & WALPE_TEXF_NO_INTERPOLATION) != 0;
    gboolean want_mips = walpe_gl_mipmaps_enabled () && t->mips != 1 &&
                         (t->width > 1 || t->height > 1);
    gboolean mipmapped = FALSE;
    walpe_gl_tex_sampling (FALSE, nearest);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    gboolean pm = FALSE;
    u->w = t->width;
    u->h = t->height;
    u->compressed = FALSE;
    gint64 base_bytes = (gint64) t->width * t->height * 4;

    switch (t->format) {
    case WALPE_TEX_RGBA8:
        if (t->channels == 1 || t->channels == 2) {
            /* native R8 / RG88: 1/4 resp. 1/2 of the BGRA expansion;
             * the swizzle returns what the expansion held (R, G|0, 0, 1),
             * opaque, so premultiplied == straight */
            if (u->layer_swizzle && t->channels == 1)
                tex_set_swizzle (GL_ONE, GL_ONE, GL_ONE, GL_RED);
            else if (u->layer_swizzle)
                tex_set_swizzle (GL_RED, GL_RED, GL_RED, GL_GREEN);
            else
                tex_set_swizzle (GL_RED, t->channels == 2 ? GL_GREEN : GL_ZERO, GL_ZERO, GL_ONE);
            glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D (GL_TEXTURE_2D, 0, t->channels == 2 ? GL_RG8 : GL_R8,
                          t->width, t->height, 0, t->channels == 2 ? GL_RG : GL_RED,
                          GL_UNSIGNED_BYTE, t->data);
            glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
            pm = !u->layer_swizzle;
            base_bytes = (gint64) t->width * t->height * t->channels;
            break;
        }
        /* raw WE textures are BGRA in memory; premultiply color bytes
         * (position-agnostic) and upload as GL_BGRA */
        if (premultiply)
            walpe_premultiply_rgba8 (t->data, t->len);
        pm = premultiply;
        {
            gsize npix = (gsize) t->width * t->height;
            int kind = tex_compact_on () && npix >= 64 * 64 && t->len >= npix * 4
                ? tex_compact_kind (t->data, npix) : 0;
            gboolean two = kind == 4 || kind == 5;
            guint8 *pk = kind ? g_try_malloc (npix * (two ? 2 : 1)) : NULL;
            if (pk) {
                const guint8 *d = t->data;
                if (!two) {
                    int ch = kind == 6 ? 3 : 2;
                    for (gsize i = 0; i < npix; i++)
                        pk[i] = d[i * 4 + ch];
                } else {
                    int second = kind == 4 ? 3 : 1;
                    for (gsize i = 0; i < npix; i++) {
                        pk[i * 2] = d[i * 4 + 2];
                        pk[i * 2 + 1] = d[i * 4 + second];
                    }
                }
                switch (kind) {
                case 1: tex_set_swizzle (GL_RED, GL_RED, GL_RED, GL_ONE); break;
                case 2: tex_set_swizzle (GL_RED, GL_ZERO, GL_ZERO, GL_ONE); break;
                case 3: tex_set_swizzle (GL_RED, GL_RED, GL_RED, GL_RED); break;
                case 4: tex_set_swizzle (GL_RED, GL_RED, GL_RED, GL_GREEN); break;
                case 6: tex_set_swizzle (GL_ONE, GL_ONE, GL_ONE, GL_RED); break;
                default: tex_set_swizzle (GL_RED, GL_GREEN, GL_ZERO, GL_ONE); break;
                }
                glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D (GL_TEXTURE_2D, 0, two ? GL_RG8 : GL_R8, t->width, t->height,
                              0, two ? GL_RG : GL_RED, GL_UNSIGNED_BYTE, pk);
                glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
                g_free (pk);
                base_bytes = (gint64) npix * (two ? 2 : 1);
                g_debug ("gl: texture %dx%d RGBA8 stored as %s (layout %d)", t->width, t->height,
                         two ? "RG8" : "R8", kind);
                break;
            }
        }
        glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
        walpe_gl_tex_image_bgra (t->width, t->height, t->data);
        break;
    case WALPE_TEX_DXT1:
    case WALPE_TEX_DXT3:
    case WALPE_TEX_DXT5: {
        GLenum ifmt = t->format == WALPE_TEX_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                    : t->format == WALPE_TEX_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                                                  : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        /* the texture may start at one of the file's own smaller levels
         * (a 4K layer on a 1080p monitor: level 1 is the picture drawn;
         * level 0 is never sent to the GPU) */
        int skip = CLAMP (u->skip, 0, t->n_levels);
        int bw0 = skip ? t->level[skip - 1].w : t->width;
        int bh0 = skip ? t->level[skip - 1].h : t->height;
        const guint8 *bd = skip ? t->level[skip - 1].data : t->data;
        gsize blen = skip ? t->level[skip - 1].len : t->len;
        gsize bw = ((gsize) bw0 + 3) / 4, bh = ((gsize) bh0 + 3) / 4;
        gsize size = t->format == WALPE_TEX_DXT1 ? bw * bh * 8 : bw * bh * 16;
        glCompressedTexImage2D (GL_TEXTURE_2D, 0, ifmt, bw0, bh0, 0,
                                (GLsizei) MIN (size, blen), bd);
        u->w = bw0;
        u->h = bh0;
        u->compressed = TRUE;
        base_bytes = (gint64) MIN (size, blen);
        pm = FALSE; /* straight alpha; the draw shader premultiplies */
        /* compressed: WE's stored chain (tex.c), as far as it goes and as
         * far as the driver takes it (a refused level ends the chain) */
        int n = 0;
        while (glGetError () != GL_NO_ERROR)
            ;
        while (want_mips && !u->no_chain && skip + n < t->n_levels) {
            glCompressedTexImage2D (GL_TEXTURE_2D, n + 1, ifmt, t->level[skip + n].w,
                                    t->level[skip + n].h, 0, (GLsizei) t->level[skip + n].len,
                                    t->level[skip + n].data);
            if (glGetError () != GL_NO_ERROR)
                break;
            base_bytes += (gint64) t->level[skip + n].len;
            n++;
        }
        if (want_mips)
            g_debug ("gl: DXT texture %dx%d from stored level %d: %d of %d further levels%s",
                     bw0, bh0, skip, n, t->n_levels - skip, u->no_chain ? " (none wanted)" : "");
        if (n > 0) {
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, n);
            mipmapped = TRUE;
        }
        want_mips = FALSE;
        break;
    }
    }
    u->deferred = want_mips && u->defer_chain;
    if (want_mips && !u->defer_chain) {
        glGenerateMipmap (GL_TEXTURE_2D);
        mipmapped = TRUE;
        base_bytes += base_bytes / 3;
    }
    if (mipmapped)
        walpe_gl_tex_sampling (TRUE, nearest);
    u->premult = pm;
    u->bytes = base_bytes;
    if (g_getenv ("WALPE_PROBE")) {
        /* texture memory estimate (GPU side) */
        static gint64 total, total_bgra;
        gsize px = (gsize) u->w * u->h;
        gint64 tot, totb;
        tot = __atomic_add_fetch (&total, base_bytes, __ATOMIC_RELAXED);
        totb = __atomic_add_fetch (&total_bgra, (gint64) px * 4, __ATOMIC_RELAXED);
        g_debug ("gl: texture %dx%d %s %" G_GINT64_FORMAT " KB; uploaded total %.1f MB (as BGRA %.1f MB)",
                 u->w, u->h,
                 t->format == WALPE_TEX_DXT1 ? "DXT1" : t->format == WALPE_TEX_DXT3 ? "DXT3"
                 : t->format == WALPE_TEX_DXT5 ? "DXT5" : t->channels == 1 ? "R8"
                 : t->channels == 2 ? "RG8" : "BGRA8",
                 base_bytes / 1024, tot / 1048576.0, totb / 1048576.0);
    }
    tex_live_add (tex, base_bytes);
    return tex;
}

GLuint walpe_gl_upload_tex (WalpeTex *t, GLint wrap, gboolean premultiply,
                            gboolean *premult)
{
    TexUp u = { 0 };
    GLuint tex = upload_tex (t, wrap, premultiply, &u);
    if (premult)
        *premult = u.premult;
    return tex;
}

/* a new texture `levels` 2x box-halvings smaller than the uncompressed
 * texture src, made on the GPU: each step is a GL_LINEAR blit at exactly
 * half size = the average of 2x2 texels. Same channels and swizzle as src
 * (a blit copies the stored channels, whatever the swizzle says). 0 when
 * src is block compressed (it cannot be rendered to). */
static GLuint tex_halve (GLuint src0, GLint wrap, int levels, gboolean round_up, int *out_w,
                         int *out_h)
{
    GLint pr = 0, pd = 0, vp[4], w = 0, h = 0, ifmt = GL_RGBA8, comp = 0, sw[4];
    glBindTexture (GL_TEXTURE_2D, src0);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &comp);
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &sw[0]);
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, &sw[1]);
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &sw[2]);
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, &sw[3]);
    *out_w = w;
    *out_h = h;
    if (comp || levels < 1 || w <= 1 || h <= 1)
        return 0;
    GLenum fmt = ifmt == GL_R8 ? GL_RED : ifmt == GL_RG8 ? GL_RG : GL_RGBA;
    if (fmt == GL_RGBA)
        ifmt = GL_RGBA8;
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &pr);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &pd);
    glGetIntegerv (GL_VIEWPORT, vp);
    gboolean scissor = glIsEnabled (GL_SCISSOR_TEST);
    if (scissor)
        glDisable (GL_SCISSOR_TEST);
    GLuint fb[2];
    glGenFramebuffers (2, fb);
    GLuint src = src0, tex = 0;
    for (int l = 0; l < levels && w > 1 && h > 1; l++) {
        int nw = MAX (1, (w + (round_up ? 1 : 0)) / 2), nh = MAX (1, (h + (round_up ? 1 : 0)) / 2);
        GLuint t = 0;
        glGenTextures (1, &t);
        glBindTexture (GL_TEXTURE_2D, t);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        if (fmt != GL_RGBA)
            tex_set_swizzle (sw[0], sw[1], sw[2], sw[3]);
        glTexImage2D (GL_TEXTURE_2D, 0, ifmt, nw, nh, 0, fmt, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, fb[0]);
        glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, fb[1]);
        glFramebufferTexture2D (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        glBlitFramebuffer (0, 0, w, h, 0, 0, nw, nh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        tex_live_add (t, (gint64) nw * nh * (fmt == GL_RED ? 1 : fmt == GL_RG ? 2 : 4));
        if (src != src0) {
            tex_live_del (src);
            glDeleteTextures (1, &src);
        }
        src = t;
        w = nw;
        h = nh;
        tex = t;
    }
    glDeleteFramebuffers (2, fb);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, (GLuint) pr);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, (GLuint) pd);
    glViewport (vp[0], vp[1], vp[2], vp[3]);
    if (scissor)
        glEnable (GL_SCISSOR_TEST);
    *out_w = w;
    *out_h = h;
    return tex;
}

/* ---- file textures shared by all monitor contexts -------------------
 * Every monitor renders the same wallpaper in its own context; with one
 * EGL share group (walpe_gl_set_shared) a texture decoded and uploaded by
 * the first monitor is reused by the others: one copy in VRAM (and, on
 * software GL, in RAM) instead of one per monitor, and one decode.
 * An entry is found by what was asked for (file, flags, wrap, premultiply)
 * and, when that is new, by what it holds: the same pixels under another
 * file name (a layer the author duplicated: 1199910952 ships its 1920x1080
 * background twice) or behind other loader flags are one GL texture. */
typedef struct {
    char *key;          /* the request it was made for */
    GPtrArray *alias;   /* further requests that hold the same pixels */
    char *ckey;         /* content key, NULL = not shared by content */
    GLuint tex;
    int refs;
    WalpeTexInfo info;
    WalpeTexFrame *frames;
} TexEntry;

static GMutex tc_lock;
static GHashTable *tc_by_key;      /* request key -> TexEntry */
static GHashTable *tc_by_content;  /* content key -> TexEntry */
static GHashTable *tc_by_tex;      /* GUINT_TO_POINTER (tex) -> TexEntry */
static GHashTable *tc_files;       /* file path -> number of request keys of that file */

/* tc_lock held. A request key is "<path>|<flags>|<wrap>|<premultiply>". */
static char *tc_key_path (const char *key)
{
    const char *end = key + strlen (key);
    for (int n = 0; n < 3 && end > key; n++) {
        end--;
        while (end > key && *end != '|')
            end--;
    }
    return g_strndup (key, (gsize) (end - key));
}

static void tc_key_add (char *key, gpointer e)
{
    g_hash_table_insert (tc_by_key, key, e);
    char *path = tc_key_path (key);
    guint n = GPOINTER_TO_UINT (g_hash_table_lookup (tc_files, path));
    g_hash_table_insert (tc_files, path, GUINT_TO_POINTER (n + 1));   /* frees path when it is known */
}

static void tc_key_remove (const char *key)
{
    char *path = tc_key_path (key);
    guint n = GPOINTER_TO_UINT (g_hash_table_lookup (tc_files, path));
    if (n > 1)
        g_hash_table_insert (tc_files, path, GUINT_TO_POINTER (n - 1));
    else {
        g_hash_table_remove (tc_files, path);
        g_free (path);
    }
    g_hash_table_remove (tc_by_key, key);
}

static gboolean tex_cache_has_file (const char *path)
{
    gboolean has = FALSE;
    if (g_atomic_int_get (&tc_shared)) {
        g_mutex_lock (&tc_lock);
        has = tc_files && g_hash_table_contains (tc_files, path);
        g_mutex_unlock (&tc_lock);
    }
    return has;
}
static __thread int tc_scope;
static __thread gint64 tc_scope_bytes, tc_scope_budget;

void walpe_gl_set_shared (gboolean shared)
{
    g_atomic_int_set (&tc_shared, shared ? 1 : 0);
}

gboolean walpe_gl_get_shared (void)
{
    return g_atomic_int_get (&tc_shared) != 0;
}

void walpe_gl_tex_scope_begin (gint64 budget)
{
    if (tc_scope++ == 0) {
        tc_scope_bytes = 0;
        tc_scope_budget = budget;
    }
}

gint64 walpe_gl_tex_scope_end (void)
{
    if (tc_scope > 0 && --tc_scope == 0)
        tc_scope_budget = 0;
    return tc_scope_bytes;
}

/* The largest side this driver takes for a texture (GL_MAX_TEXTURE_SIZE),
 * 0 = not known. The loaders admit pictures of up to 16384 texels a side,
 * which is what current desktop drivers take; an older or smaller GPU
 * takes 8192 or 4096, and an upload beyond it fails (the layer would be
 * black). Such a picture is halved before the upload, like one that does
 * not fit the texture budget. */
static int tex_side_limit (void)
{
    const WalpeGlCaps *gc = walpe_gl_caps ();
    return gc && gc->limits.max_texture_size >= 1024 ? gc->limits.max_texture_size : 0;
}

/* one 2x2 box halving of uncompressed pixels (4, 2 or 1 bytes per texel),
 * in place: the texture budget's way to shrink a picture before it is
 * uploaded at all */
static void tex_cpu_halve (WalpeTex *t)
{
    int bpp = t->channels == 1 || t->channels == 2 ? t->channels : 4;
    int w = t->width, h = t->height, nw = MAX (1, w / 2), nh = MAX (1, h / 2);
    guint8 *d = t->data;
    for (int y = 0; y < nh; y++) {
        const guint8 *r0 = d + (gsize) MIN (2 * y, h - 1) * w * bpp;
        const guint8 *r1 = d + (gsize) MIN (2 * y + 1, h - 1) * w * bpp;
        guint8 *o = d + (gsize) y * nw * bpp;
        for (int x = 0; x < nw; x++) {
            int x0 = MIN (2 * x, w - 1) * bpp, x1 = MIN (2 * x + 1, w - 1) * bpp;
            for (int c = 0; c < bpp; c++)
                o[x * bpp + c] = (guint8) ((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) >> 2);
        }
    }
    if (t->base_width <= 0) {
        t->base_width = w;
        t->base_height = h;
    }
    t->width = t->full_width = nw;
    t->height = t->full_height = nh;
    t->len = (gsize) nw * nh * bpp;
}

/* 2 x 64 bits over the pixels, seeded per process (a wallpaper cannot be
 * built to collide with another one's texture) */
static void tex_hash (const void *data, gsize n, guint64 h[2])
{
    const guint8 *p = data;
    guint64 a = h[0], b = h[1];
    while (n >= 16) {
        guint64 x, y;
        memcpy (&x, p, 8);
        memcpy (&y, p + 8, 8);
        a = (a ^ x) * 0x9FB21C651E98DF25ull;
        a = ((a << 29) | (a >> 35)) + b;
        b = (b ^ y) * 0xD6E8FEB86659FD93ull;
        b = ((b << 31) | (b >> 33)) ^ a;
        p += 16;
        n -= 16;
    }
    guint64 x = 0, y = 0;
    memcpy (&x, p, MIN (n, (gsize) 8));
    if (n > 8)
        memcpy (&y, p + 8, n - 8);
    a = (a ^ x ^ ((guint64) n << 56)) * 0x9FB21C651E98DF25ull;
    b = (b ^ y) * 0xD6E8FEB86659FD93ull;
    a ^= b >> 31;
    b ^= a >> 29;
    a *= 0xFF51AFD7ED558CCDull;
    b *= 0xC4CEB9FE1A85EC53ull;
    h[0] = a ^ (b >> 33);
    h[1] = b ^ (a >> 33);
}

static void tex_info_from (WalpeTexInfo *info, const WalpeTex *t, const TexUp *u)
{
    /* sizes are the image's at level 0, whatever level is stored: every
     * UV and resolution uniform stays as it is for the full texture */
    info->width = t->base_width > 0 ? t->base_width : t->width;
    info->height = t->base_height > 0 ? t->base_height : t->height;
    info->full_width = info->width;
    info->full_height = info->height;
    info->format = t->format;
    info->channels = t->channels;
    info->we_format = t->we_format;
    info->premult = u->premult;
    info->want_mips = u->deferred;
    info->phys_w = u->w;
    info->phys_h = u->h;
    info->compressed = u->compressed;
    info->mips = t->mips;
    info->bytes = u->bytes;
    info->nframes = t->nframes;
    info->frame_w = t->frame_w;
    info->frame_h = t->frame_h;
}

/* ---- reduction before the upload ------------------------------------
 * tex_halve () needs the full picture on the GPU first: for a 3840x2160
 * RGBA8 layer shown on a 1920x1080 monitor that is a 31.6 MiB upload and
 * 31.6 MiB of video memory for the moment of the blit. For even sizes the
 * GL_LINEAR blit to exactly half size is a 2x2 box with a fixed rounding -
 * on Mesa's software renderer on x86, bit for bit (tests/texload_test.c)
 *     out = avg (avg (a, b), avg (c, d)),   avg (x, y) = (x + y + 1) >> 1
 * with a b the two texels of the upper row and c d those of the lower one.
 * The same arithmetic on the decoded pixels gives the same texture, and
 * the full-size one never exists (a GPU may round its own blit the other
 * way, 1/255 in a texel; Mesa's filter off x86 truncates: up to 2/255).
 * An odd width or height (4807 -> 2403 or 2404) makes the blit a real
 * resampling with weights that drift across the picture: those keep the
 * GL path. */
static void tex_halve_row_pair (const guint8 *r0, const guint8 *r1, guint8 *o, int nw, int bpp)
{
    int x = 0;
#if defined (__SSE2__)
    if (bpp == 4) {
        for (; x + 4 <= nw; x += 4) {
            __m128 a0 = _mm_castsi128_ps (_mm_loadu_si128 ((const __m128i *) (const void *) (r0 + x * 8)));
            __m128 a1 = _mm_castsi128_ps (_mm_loadu_si128 ((const __m128i *) (const void *) (r0 + x * 8 + 16)));
            __m128 b0 = _mm_castsi128_ps (_mm_loadu_si128 ((const __m128i *) (const void *) (r1 + x * 8)));
            __m128 b1 = _mm_castsi128_ps (_mm_loadu_si128 ((const __m128i *) (const void *) (r1 + x * 8 + 16)));
            __m128i h0 = _mm_avg_epu8 (_mm_castps_si128 (_mm_shuffle_ps (a0, a1, _MM_SHUFFLE (2, 0, 2, 0))),
                                       _mm_castps_si128 (_mm_shuffle_ps (a0, a1, _MM_SHUFFLE (3, 1, 3, 1))));
            __m128i h1 = _mm_avg_epu8 (_mm_castps_si128 (_mm_shuffle_ps (b0, b1, _MM_SHUFFLE (2, 0, 2, 0))),
                                       _mm_castps_si128 (_mm_shuffle_ps (b0, b1, _MM_SHUFFLE (3, 1, 3, 1))));
            _mm_storeu_si128 ((__m128i *) (void *) (o + x * 4), _mm_avg_epu8 (h0, h1));
        }
    } else if (bpp == 1) {
        const __m128i m = _mm_set1_epi16 (0x00ff);
        for (; x + 16 <= nw; x += 16) {
            __m128i a0 = _mm_loadu_si128 ((const __m128i *) (const void *) (r0 + x * 2));
            __m128i a1 = _mm_loadu_si128 ((const __m128i *) (const void *) (r0 + x * 2 + 16));
            __m128i b0 = _mm_loadu_si128 ((const __m128i *) (const void *) (r1 + x * 2));
            __m128i b1 = _mm_loadu_si128 ((const __m128i *) (const void *) (r1 + x * 2 + 16));
            __m128i h0 = _mm_avg_epu8 (_mm_packus_epi16 (_mm_and_si128 (a0, m), _mm_and_si128 (a1, m)),
                                       _mm_packus_epi16 (_mm_srli_epi16 (a0, 8), _mm_srli_epi16 (a1, 8)));
            __m128i h1 = _mm_avg_epu8 (_mm_packus_epi16 (_mm_and_si128 (b0, m), _mm_and_si128 (b1, m)),
                                       _mm_packus_epi16 (_mm_srli_epi16 (b0, 8), _mm_srli_epi16 (b1, 8)));
            _mm_storeu_si128 ((__m128i *) (void *) (o + x), _mm_avg_epu8 (h0, h1));
        }
    } else if (bpp == 2) {
        /* 8 two-byte texels -> the even ones in the low half, the odd ones in the high half */
#define TEX_DEINT16(v) \
    _mm_shuffle_epi32 (_mm_shufflehi_epi16 (_mm_shufflelo_epi16 ((v), _MM_SHUFFLE (3, 1, 2, 0)), \
                                            _MM_SHUFFLE (3, 1, 2, 0)), _MM_SHUFFLE (3, 1, 2, 0))
        for (; x + 4 <= nw; x += 4) {
            __m128i a = _mm_loadu_si128 ((const __m128i *) (const void *) (r0 + x * 4));
            __m128i b = _mm_loadu_si128 ((const __m128i *) (const void *) (r1 + x * 4));
            a = TEX_DEINT16 (a);
            b = TEX_DEINT16 (b);
            __m128i h0 = _mm_avg_epu8 (a, _mm_srli_si128 (a, 8));
            __m128i h1 = _mm_avg_epu8 (b, _mm_srli_si128 (b, 8));
            _mm_storel_epi64 ((__m128i *) (void *) (o + x * 2), _mm_avg_epu8 (h0, h1));
        }
#undef TEX_DEINT16
    }
#endif
    for (; x < nw; x++)
        for (int c = 0; c < bpp; c++) {
            int a = r0[(2 * x) * bpp + c], b = r0[(2 * x + 1) * bpp + c];
            int cc = r1[(2 * x) * bpp + c], d = r1[(2 * x + 1) * bpp + c];
            o[x * bpp + c] = (guint8) ((((a + b + 1) >> 1) + ((cc + d + 1) >> 1) + 1) >> 1);
        }
}

void walpe_gl_tex_halve_cpu (guint8 *d, int w, int h, int bpp)
{
    /* in place: every texel is written after the four it is made of were
     * read, and at a lower address than any texel still to be read */
    int nw = w / 2, nh = h / 2;
    for (int y = 0; y < nh; y++)
        tex_halve_row_pair (d + (gsize) (2 * y) * w * bpp, d + (gsize) (2 * y + 1) * w * bpp,
                            d + (gsize) y * nw * bpp, nw, bpp);
}

/* One halving step of a picture with an odd width or height
 * (WALPE_TEX_CPU_REDUCE=2), as a GL_LINEAR blit to nw x nh samples it:
 * bilinear at the centres of the copy's texels, weights of 8 bits, the two
 * rows first. Where a side is even this is the box of
 * walpe_gl_tex_halve_cpu; where it is odd the positions are exact
 * ((2x + 1) w / 2 nw - 1/2), not a driver's float interpolation, so the
 * result is within a few 1/255 of a GL blit, not equal to it. */
static void tex_resample_cpu (const guint8 *s, int w, int h, int bpp, guint8 *o, int nw, int nh)
{
    int *ix = g_new (int, nw), *fx = g_new (int, nw);
    for (int x = 0; x < nw; x++) {
        gint64 sv = (((2 * (gint64) x + 1) * w) << 15) / nw - 32768;
        sv = MAX (sv, 0);
        ix[x] = (int) (sv >> 16);
        fx[x] = (int) ((sv >> 8) & 0xff);
        if (ix[x] >= w - 1) {
            ix[x] = w - 1;
            fx[x] = 0;
        }
    }
    for (int y = 0; y < nh; y++) {
        gint64 sv = (((2 * (gint64) y + 1) * h) << 15) / nh - 32768;
        sv = MAX (sv, 0);
        int y0 = (int) (sv >> 16), fy = (int) ((sv >> 8) & 0xff);
        if (y0 >= h - 1) {
            y0 = h - 1;
            fy = 0;
        }
        const guint8 *r0 = s + (gsize) y0 * w * bpp, *r1 = s + (gsize) MIN (y0 + 1, h - 1) * w * bpp;
        guint8 *d = o + (gsize) y * nw * bpp;
        for (int x = 0; x < nw; x++) {
            int x0 = ix[x] * bpp, x1 = MIN (ix[x] + 1, w - 1) * bpp, f = fx[x];
            for (int c = 0; c < bpp; c++) {
                int top = (r0[x0 + c] * (256 - f) + r0[x1 + c] * f + 128) >> 8;
                int bot = (r1[x0 + c] * (256 - f) + r1[x1 + c] * f + 128) >> 8;
                d[x * bpp + c] = (guint8) ((top * (256 - fy) + bot * fy + 128) >> 8);
            }
        }
    }
    g_free (ix);
    g_free (fx);
}

/* `halvings` halvings of t on the CPU give what tex_halve () gives: every
 * size on the way is even (with WALPE_TEX_CPU_REDUCE=2: any size) */
static gboolean tex_cpu_can_halve (const WalpeTex *t, int halvings, gboolean round_up)
{
    int bpp = t->channels == 1 || t->channels == 2 ? t->channels : 4;
    int w = t->width, h = t->height;
    if (t->format != WALPE_TEX_RGBA8 || halvings < 1 || !t->data ||
        t->len < (gsize) w * h * bpp)
        return FALSE;
    for (int l = 0; l < halvings; l++) {
        if (w < 2 || h < 2 || (((w & 1) || (h & 1)) && tex_cpu_reduce_mode () < 2))
            return FALSE;
        w = (w + (round_up ? 1 : 0)) / 2;
        h = (h + (round_up ? 1 : 0)) / 2;
    }
    return TRUE;
}

/* upload_tex () + tex_halve () without the full-size texture, in two
 * steps. tex_prepare (any thread, no GL): the same premultiplication and
 * channel layout decided on the full picture, the pixels halved in place.
 * tex_upload_prepared (GL thread): a texture with the state tex_halve ()
 * gives its copies. */
typedef struct {
    gboolean valid;
    int level;                      /* halvings done */
    gboolean premultiply, layer_swizzle, round_up;
    int kind;                       /* tex_compact_kind of the full picture */
    int w, h;                       /* the reduced size */
    gboolean hashed;
    guint64 hash[2];                /* tex_hash of the decoded data, as tex_make starts its key */
} TexPrep;

static void tex_prepare (WalpeTex *t, int halvings, gboolean premultiply, gboolean layer_swizzle,
                         gboolean round_up, TexPrep *p)
{
    int bpp = t->channels == 1 || t->channels == 2 ? t->channels : 4;
    int w = t->width, h = t->height;
    gsize npix0 = (gsize) w * h;
    p->kind = 0;
    if (bpp == 4) {
        if (premultiply)
            walpe_premultiply_rgba8 (t->data, npix0 * 4);
        p->kind = tex_compact_on () && npix0 >= 64 * 64 ? tex_compact_kind (t->data, npix0) : 0;
    }
    for (int l = 0; l < halvings; l++) {
        int nw = (w + (round_up ? 1 : 0)) / 2, nh = (h + (round_up ? 1 : 0)) / 2;
        if (!(w & 1) && !(h & 1)) {
            walpe_gl_tex_halve_cpu (t->data, w, h, bpp);
        } else {
            guint8 *n = g_malloc ((gsize) nw * nh * bpp);
            tex_resample_cpu (t->data, w, h, bpp, n, nw, nh);
            g_free (t->data);
            t->data = n;
        }
        w = nw;
        h = nh;
    }
    t->len = (gsize) w * h * bpp;
    guint8 *shrunk = g_try_realloc (t->data, t->len);
    if (shrunk)
        t->data = shrunk;
    p->level = halvings;
    p->premultiply = premultiply;
    p->layer_swizzle = layer_swizzle;
    p->round_up = round_up;
    p->w = w;
    p->h = h;
    p->valid = TRUE;
}

static GLuint tex_upload_prepared (WalpeTex *t, GLint wrap, const TexPrep *p, TexUp *u)
{
    int bpp = t->channels == 1 || t->channels == 2 ? t->channels : 4;
    int w = p->w, h = p->h, kind = p->kind;
    gsize npix0 = (gsize) t->width * t->height, npix = (gsize) w * h;
    GLint sw[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
    gboolean swz = FALSE;
    gint64 base_bytes = (gint64) npix0 * 4;
    if (bpp != 4) {
        if (p->layer_swizzle && bpp == 1) {
            sw[0] = sw[1] = sw[2] = GL_ONE;
            sw[3] = GL_RED;
        } else if (p->layer_swizzle) {
            sw[0] = sw[1] = sw[2] = GL_RED;
            sw[3] = GL_GREEN;
        } else {
            sw[1] = bpp == 2 ? GL_GREEN : GL_ZERO;
            sw[2] = GL_ZERO;
            sw[3] = GL_ONE;
        }
        swz = TRUE;
        u->premult = !p->layer_swizzle;
        base_bytes = (gint64) npix0 * bpp;
    } else {
        u->premult = p->premultiply;
    }
    const guint8 *up = t->data;
    GLenum ifmt = bpp == 2 ? GL_RG8 : bpp == 1 ? GL_R8 : GL_RGBA8;
    GLenum fmt = bpp == 2 ? GL_RG : bpp == 1 ? GL_RED : GL_BGRA;
    gboolean two = kind == 4 || kind == 5;
    guint8 *pk = kind ? g_try_malloc (npix * (two ? 2 : 1)) : NULL;
    if (pk) {
        const guint8 *d = t->data;
        if (!two) {
            int ch = kind == 6 ? 3 : 2;
            for (gsize i = 0; i < npix; i++)
                pk[i] = d[i * 4 + ch];
        } else {
            int second = kind == 4 ? 3 : 1;
            for (gsize i = 0; i < npix; i++) {
                pk[i * 2] = d[i * 4 + 2];
                pk[i * 2 + 1] = d[i * 4 + second];
            }
        }
        sw[0] = kind == 6 ? GL_ONE : GL_RED;
        sw[1] = kind == 2 ? GL_ZERO : kind == 5 ? GL_GREEN : kind == 6 ? GL_ONE : GL_RED;
        sw[2] = kind == 2 || kind == 5 ? GL_ZERO : kind == 6 ? GL_ONE : GL_RED;
        sw[3] = kind == 3 || kind == 6 ? GL_RED : kind == 4 ? GL_GREEN : GL_ONE;
        swz = TRUE;
        up = pk;
        ifmt = two ? GL_RG8 : GL_R8;
        fmt = two ? GL_RG : GL_RED;
        base_bytes = (gint64) npix0 * (two ? 2 : 1);
    }
    GLuint tex = 0;
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    if (swz)
        tex_set_swizzle (sw[0], sw[1], sw[2], sw[3]);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei (GL_UNPACK_ALIGNMENT, fmt == GL_BGRA ? 4 : 1);
    if (fmt == GL_BGRA)
        walpe_gl_tex_image_bgra (w, h, up);
    else
        glTexImage2D (GL_TEXTURE_2D, 0, (GLint) ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, up);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    g_free (pk);
    u->w = t->width;
    u->h = t->height;
    u->compressed = FALSE;
    u->deferred = walpe_gl_mipmaps_enabled () && t->mips != 1 && (t->width > 1 || t->height > 1);
    u->bytes = base_bytes;
    tex_live_add (tex, (base_bytes / (gint64) npix0) * (gint64) npix);
    g_debug ("gl: texture %dx%d reduced to %dx%d before the upload (%d halving%s on the CPU)",
             t->width, t->height, w, h, p->level, p->level > 1 ? "s" : "");
    return tex;
}

/* ---- level 0 of a file decoded ahead, on worker threads ---------------
 * A layer picture stored as PNG / JPEG takes 0.1 - 1 s to decode, and a
 * scene has up to dozens. They used to be decoded one after the other on
 * the render thread while the scene was parsed, and uploaded at full size.
 * Now the scene asks for them here as soon as it knows the file
 * (walpe_gl_tex_predecode), workers decode while the rest of the scene is
 * parsed and its shaders are compiled, and the first frame's load
 * (tex_make) takes the decoded picture, reduces it to the size the layer
 * is drawn at and uploads that.
 * Bounded by bytes, not by count: workers start a picture only while the
 * decoded pictures held and in work stay below WALPE_TEX_DECODE_MB (64:
 * two 3840x2160 pictures; one picture is always allowed). The load never waits for that bound: a
 * picture it needs that no worker has started is decoded by the caller. */
static gint tc_shared;
enum { TJ_QUEUED, TJ_RUNNING, TJ_DONE };
typedef struct {
    char *key, *path;
    guint lf, id;
    int state, refs;
    gboolean dead;      /* running and nobody wants it any more: the worker drops it */
    WalpeTex *t;
    gint64 est;         /* bytes of the decoded picture */
    gint64 held;        /* ... of what is counted for it now */
    /* the load that will take it, once known (walpe_gl_tex_predecode_request):
     * the worker then also reduces the picture, and holds that */
    gboolean req, req_pm, req_swz, req_hash, req_ceil;
    int req_level;
    TexPrep prep;
} TexJob;

static GMutex tj_lock;
static GCond tj_cond;
static GHashTable *tj_jobs;     /* key -> TexJob */
static GThreadPool *tj_pool;
static gint64 tj_bytes, tj_peak;
static guint tj_next_id, tj_started, tj_taken, tj_stolen, tj_dropped;
static gint tj_cpu_reduced, tj_gl_halved;   /* reduced copies made before the upload / by GL blits */

static gint64 tex_job_budget (void)
{
    static gint64 b = -1;
    if (b < 0) {
        const char *e = g_getenv ("WALPE_TEX_DECODE_MB");
        double mb = e && *e ? g_ascii_strtod (e, NULL) : 64.0;
        b = (gint64) (CLAMP (mb, 1.0, 65536.0) * 1048576.0);
    }
    return b;
}

static guint64 tc_seed[2];
static void tc_seed_init (void)
{
    static gsize once;
    if (g_once_init_enter (&once)) {
        tc_seed[0] = ((guint64) g_random_int () << 32) | g_random_int ();
        tc_seed[1] = ((guint64) g_random_int () << 32) | g_random_int ();
        g_once_init_leave (&once, 1);
    }
}

static void tex_hash (const void *data, gsize n, guint64 h[2]);

/* Has any picture been announced yet? The job table is made under tj_lock
 * by the first announcement, so that is also where it is looked at: the
 * functions below used to test the pointer before taking the lock, while
 * another monitor's thread could be storing it. Once it exists it stays.
 * (Texture loads only; nothing here runs per frame.) */
static gboolean tex_jobs_exist (void)
{
    g_mutex_lock (&tj_lock);
    gboolean any = tj_jobs != NULL;
    g_mutex_unlock (&tj_lock);
    return any;
}

static void tex_job_free (TexJob *j)
{
    g_free (j->key);
    g_free (j->path);
    g_free (j);
}

static void tex_job_run (gpointer data, gpointer user)
{
    char *key = data;
    (void) user;
    g_mutex_lock (&tj_lock);
    TexJob *j = g_hash_table_lookup (tj_jobs, key);
    guint id = j ? j->id : 0;
    /* the byte bound; the job may be taken over or dropped while this waits */
    while (j && j->id == id && j->state == TJ_QUEUED && tj_bytes > 0 &&
           tj_bytes + j->est > tex_job_budget ()) {
        g_cond_wait (&tj_cond, &tj_lock);
        j = g_hash_table_lookup (tj_jobs, key);
    }
    if (!j || j->id != id || j->state != TJ_QUEUED) {
        g_mutex_unlock (&tj_lock);
        g_free (key);
        return;
    }
    j->state = TJ_RUNNING;
    j->held = j->est;
    tj_bytes += j->held;
    if (tj_bytes > tj_peak)
        tj_peak = tj_bytes;
    tj_started++;
    char *path = g_strdup (j->path);
    guint lf = j->lf;
    g_mutex_unlock (&tj_lock);
    WalpeTex *t = walpe_tex_load_ex (path, lf);
    g_free (path);
    g_mutex_lock (&tj_lock);
    /* a running job is never removed by anyone else */
    TexPrep prep = { 0 };
    if (t && !j->dead && j->req && j->req_level > 0 && tex_cpu_reduce_on () &&
        tex_cpu_can_halve (t, j->req_level, j->req_ceil)) {
        /* the load said what it will ask for: the picture is reduced here */
        int level = j->req_level;
        gboolean pm = j->req_pm, swz = j->req_swz, hash = j->req_hash, ceil = j->req_ceil;
        g_mutex_unlock (&tj_lock);
        if (hash) {
            tc_seed_init ();
            prep.hash[0] = tc_seed[0];
            prep.hash[1] = tc_seed[1];
            tex_hash (t->data, t->len, prep.hash);
            prep.hashed = TRUE;
        }
        tex_prepare (t, level, pm, swz, ceil, &prep);
        g_debug ("gl: worker: %s decoded and reduced to %dx%d", key, prep.w, prep.h);
        g_mutex_lock (&tj_lock);
    } else if (t) {
        g_debug ("gl: worker: %s decoded (%dx%d%s)", key, t->width, t->height,
                 j->req ? "; the load asks for it as it is" : "");
    }
    if (j->dead) {
        tj_bytes -= j->held;
        tj_dropped++;
        g_hash_table_remove (tj_jobs, key);
        tex_job_free (j);
    } else {
        j->t = t;
        j->prep = prep;
        j->state = TJ_DONE;
        gint64 now = !t ? 0 : prep.valid ? (gint64) t->len : j->est;
        tj_bytes -= j->held - now;
        j->held = now;
        t = NULL;
    }
    g_cond_broadcast (&tj_cond);
    g_mutex_unlock (&tj_lock);
    walpe_tex_free (t);
    g_free (key);
}

static gboolean tex_cache_has_file (const char *path);

gboolean walpe_gl_tex_predecode (const char *path, guint load_flags, gint64 est_bytes)
{
    if (!path || !walpe_gl_tex_async ())
        return FALSE;
    gboolean held = FALSE;
    guint lf = load_flags & 0xffffu;
    char *key = g_strdup_printf ("%s|%u", path, lf);
    /* on the GPU already, at whatever size (the other monitor loaded the
     * same wallpaper): the first frame will most likely find it there */
    gboolean cached = tex_cache_has_file (path);
    g_mutex_lock (&tj_lock);
    if (!tj_jobs)
        tj_jobs = g_hash_table_new (g_str_hash, g_str_equal);
    TexJob *j = g_hash_table_lookup (tj_jobs, key);
    if (j) {
        j->refs++;
        held = TRUE;
    } else if (!cached) {
        if (!tj_pool) {
            const char *e = g_getenv ("WALPE_TEX_DECODE_THREADS");
            int n = e && *e ? atoi (e) : (int) g_get_num_processors () / 2;
            tj_pool = g_thread_pool_new (tex_job_run, NULL, CLAMP (n, 1, 4), FALSE, NULL);
        }
        j = g_new0 (TexJob, 1);
        j->key = key;
        j->path = g_strdup (path);
        j->lf = lf;
        j->id = ++tj_next_id;
        j->refs = 1;
        j->est = MAX (est_bytes, 1);
        g_hash_table_insert (tj_jobs, j->key, j);
        if (tj_pool)
            g_thread_pool_push (tj_pool, g_strdup (key), NULL);
        key = NULL;
        held = TRUE;
    }
    g_mutex_unlock (&tj_lock);
    g_free (key);
    return held;
}

void walpe_gl_tex_predecode_cancel (const char *path, guint load_flags)
{
    if (!path || !tex_jobs_exist ())
        return;
    char *key = g_strdup_printf ("%s|%u", path, load_flags & 0xffffu);
    WalpeTex *t = NULL;
    g_mutex_lock (&tj_lock);
    TexJob *j = g_hash_table_lookup (tj_jobs, key);
    if (j && --j->refs <= 0) {
        if (j->state == TJ_RUNNING) {
            j->dead = TRUE;
        } else {
            if (j->state == TJ_DONE && j->t) {
                tj_bytes -= j->held;
                tj_dropped++;
            }
            t = j->t;
            g_hash_table_remove (tj_jobs, key);
            tex_job_free (j);
        }
        g_cond_broadcast (&tj_cond);
    }
    g_mutex_unlock (&tj_lock);
    walpe_tex_free (t);
    g_free (key);
}

void walpe_gl_tex_predecode_request (const char *path, guint load_flags, gboolean premultiply)
{
    if (!path || !tex_jobs_exist ())
        return;
    char *key = g_strdup_printf ("%s|%u", path, load_flags & 0xffffu);
    g_mutex_lock (&tj_lock);
    TexJob *j = g_hash_table_lookup (tj_jobs, key);
    if (j && !j->req && j->state != TJ_DONE) {
        j->req = TRUE;
        j->req_level = (int) WALPE_GL_TEX_LEVEL_OF (load_flags);
        j->req_pm = premultiply;
        j->req_swz = (load_flags & WALPE_GL_TEX_LAYER_SWIZZLE) != 0;
        j->req_ceil = (load_flags & WALPE_GL_TEX_CEIL) != 0;
        j->req_hash = g_atomic_int_get (&tc_shared) && tex_dedupe_on ();
    }
    g_mutex_unlock (&tj_lock);
    g_free (key);
}

/* TRUE: *out is what the job decoded (NULL: the file cannot be decoded),
 * *prep says whether it is reduced already. FALSE: nobody decoded it and
 * nobody is, the caller does it now. */
static gboolean tex_job_take (const char *path, guint lf, WalpeTex **out, TexPrep *prep)
{
    if (!tex_jobs_exist ())
        return FALSE;
    char *key = g_strdup_printf ("%s|%u", path, lf);
    gboolean got = FALSE;
    g_mutex_lock (&tj_lock);
    TexJob *j = g_hash_table_lookup (tj_jobs, key);
    if (j && j->state == TJ_QUEUED) {
        /* not started (the workers are busy or at their byte bound): here, now */
        tj_stolen++;
        g_hash_table_remove (tj_jobs, key);
        tex_job_free (j);
        g_cond_broadcast (&tj_cond);
    } else if (j) {
        j->refs++;   /* nobody drops it while this thread waits */
        while (j->state == TJ_RUNNING)
            g_cond_wait (&tj_cond, &tj_lock);
        *out = j->t;
        *prep = j->prep;
        tj_bytes -= j->held;
        tj_taken++;
        g_hash_table_remove (tj_jobs, key);
        tex_job_free (j);
        g_cond_broadcast (&tj_cond);
        got = TRUE;
    }
    g_mutex_unlock (&tj_lock);
    g_free (key);
    return got;
}

void walpe_gl_tex_predecode_stats (WalpeTexDecodeStats *st)
{
    g_mutex_lock (&tj_lock);
    st->started = tj_started;
    st->taken = tj_taken;
    st->stolen = tj_stolen;
    st->dropped = tj_dropped;
    st->pending = tj_jobs ? g_hash_table_size (tj_jobs) : 0;
    st->cpu_reduced = (guint) g_atomic_int_get (&tj_cpu_reduced);
    st->gl_halved = (guint) g_atomic_int_get (&tj_gl_halved);
    st->bytes = tj_bytes;
    st->peak_bytes = tj_peak;
    g_mutex_unlock (&tj_lock);
}

/* level 0 of the file: decoded ahead (*prep: and reduced), or now */
static WalpeTex *tex_load_level0 (const char *path, guint lf, TexPrep *prep)
{
    WalpeTex *t = NULL;
    memset (prep, 0, sizeof *prep);
    if (tex_job_take (path, lf, &t, prep))
        return t;
    return walpe_tex_load_ex (path, lf);
}

/* the file's pixels for a texture `want` halvings below level 0: the
 * stored level itself when the file has it (walpe_tex_load_level: nothing
 * of the larger levels is decoded), else the nearest larger stored one.
 * *got = halvings the result already has. */
static WalpeTex *tex_load_reduced (const char *path, guint lf, int want, int *got,
                                   gboolean stored_only, TexPrep *prep)
{
    *got = 0;
    memset (prep, 0, sizeof *prep);
    if (walpe_gl_tex_stored_levels ())
        for (int l = want; l >= 1; l--) {
            WalpeTex *t = walpe_tex_load_level (path, lf, l);
            if (t) {
                *got = l;
                return t;
            }
        }
    return stored_only ? NULL : tex_load_level0 (path, lf, prep);
}

/* load + upload; *t_out = the decoded texture (frames, sizes), freed by the
 * caller. With ckey the pixels are hashed first and an entry holding the
 * same ones is returned instead (*hit). */
static GLuint tex_make (const char *path, guint load_flags, GLint wrap, gboolean premultiply,
                        WalpeTexInfo *info, WalpeTex **t_out, char **ckey, TexEntry **hit,
                        const TexEntry *full0)
{
    guint lf = load_flags & 0xffffu;
    int want = (int) WALPE_GL_TEX_LEVEL_OF (load_flags), got = 0;
    TexPrep prep;
    WalpeTex *t = tex_load_reduced (path, lf, want, &got, full0 != NULL, &prep);
    if (!t && want > 0 && full0) {
        /* no stored level to start from, and the full texture is on the
         * GPU already (same file, flags and wrap): halve that one instead
         * of decoding the file again - the same pixels either way */
        int nw = 0, nh = 0;
        GLuint small = tex_halve (full0->tex, wrap, want, (load_flags & WALPE_GL_TEX_CEIL) != 0,
                                  &nw, &nh);
        if (small) {
            g_atomic_int_inc (&tj_gl_halved);
            *info = full0->info;
            info->bytes = (gint64) nw * nh *
                          MAX (1, (int) (full0->info.bytes / MAX (1, (gint64) full0->info.phys_w *
                                                                   full0->info.phys_h)));
            info->phys_w = nw;
            info->phys_h = nh;
            info->lod = want;
            info->want_mips = walpe_gl_mipmaps_enabled () && full0->info.mips != 1;
            return small;
        }
        t = tex_load_level0 (path, lf, &prep);
    }
    if (!t)
        return 0;
    TexUp u = { .defer_chain = TRUE, .no_chain = (load_flags & WALPE_GL_TEX_NO_CHAIN) != 0,
                .layer_swizzle = (load_flags & WALPE_GL_TEX_LAYER_SWIZZLE) != 0 };
    if (prep.valid) {
        /* a worker reduced it for the load that was announced: this one?
         * (the scene's texture budget would have stored it smaller still) */
        int bpp = t->channels == 1 || t->channels == 2 ? t->channels : 4;
        gboolean over = tc_scope_budget > 0 && t->nframes == 0 && t->width > 64 && t->height > 64 &&
                        tc_scope_bytes + (((gint64) t->width * t->height * bpp) >> (2 * want)) >
                        tc_scope_budget;
        if (prep.level != want || prep.premultiply != premultiply ||
            prep.layer_swizzle != u.layer_swizzle ||
            prep.round_up != ((load_flags & WALPE_GL_TEX_CEIL) != 0) || over) {
            g_debug ("gl: %s was reduced ahead for another request: decoded again", walpe_log_str (path));
            walpe_tex_free (t);
            memset (&prep, 0, sizeof prep);
            t = walpe_tex_load_ex (path, lf);
            if (!t)
                return 0;
        }
    }
    if (t->format != WALPE_TEX_RGBA8 && want > 0) {
        /* kept compressed: start at the stored level asked for, as far as
         * the file's chain goes. One halving short it may stay compressed
         * when the caller samples it within 2:1 anyway (NEAR_OK: bilinear
         * at 2:1 is the 2x2 average, and DXT at one level more is no larger
         * than RGBA8 at the level asked for). Else the decoded picture is
         * halved on the GPU: properly filtered, and beyond one level also
         * the smaller one (DXT5 at full size = 4 x RGBA8 at a quarter). */
        u.skip = walpe_gl_tex_stored_levels () ? MIN (want, t->n_levels) : 0;
        if (want - u.skip >= 2 || (want - u.skip == 1 && !(load_flags & WALPE_GL_TEX_NEAR_OK))) {
            walpe_tex_free (t);
            t = tex_load_reduced (path, lf & ~(guint) WALPE_TEX_LOAD_NATIVE_DXT, want, &got, FALSE,
                                  &prep);
            if (!t)
                return 0;
            if (prep.valid) {   /* never asked for with these loader flags */
                walpe_tex_free (t);
                memset (&prep, 0, sizeof prep);
                t = walpe_tex_load_ex (path, lf & ~(guint) WALPE_TEX_LOAD_NATIVE_DXT);
                if (!t)
                    return 0;
            }
            u.skip = 0;
        } else {
            got = u.skip;
        }
    }
    if (tc_scope_budget > 0 && t->format == WALPE_TEX_RGBA8 && t->nframes == 0 && !prep.valid) {
        /* texture budget of the scene being loaded (walpe_gl_tex_scope_begin):
         * what does not fit any more is stored smaller - halved here, before
         * the upload, until it fits or is down to 64 texels */
        int bpp = t->channels == 1 || t->channels == 2 ? t->channels : 4, n = 0;
        while (n < 8 && t->width > 64 && t->height > 64 &&
               tc_scope_bytes + (((gint64) t->width * t->height * bpp) >> (2 * MAX (0, want - got))) >
               tc_scope_budget) {
            tex_cpu_halve (t);
            got++;
            n++;
        }
        if (got > want && n > 0)
            g_warning ("gl: texture budget (%.1f MiB) reached: %s stored at 1/%d of its size",
                       tc_scope_budget / 1048576.0, walpe_log_str (path), 1 << got);
    }
    if (t->format == WALPE_TEX_RGBA8 && t->nframes == 0 && !prep.valid) {
        /* larger than the driver's largest texture: halved until it fits */
        int lim = tex_side_limit (), n = 0;
        while (lim && n < 8 && (t->width > lim || t->height > lim)) {
            tex_cpu_halve (t);
            got++;
            n++;
        }
        if (n > 0)
            g_warning ("gl: %s is larger than this driver's largest texture (%d texels a side): "
                       "stored at 1/%d of its size", walpe_log_str (path), lim, 1 << got);
    }
    int halvings = t->format == WALPE_TEX_RGBA8 ? MAX (0, want - got) : 0;
    if (ckey && hit && tex_dedupe_on ()) {
        guint64 h[2] = { tc_seed[0], tc_seed[1] };
        if (prep.valid && prep.hashed) {
            h[0] = prep.hash[0];
            h[1] = prep.hash[1];
        } else if (prep.valid) {
            /* reduced by a worker that was not asked for the hash of the
             * full picture: its own key, shared with nothing */
            static gint n;
            h[0] ^= (guint64) g_atomic_int_add (&n, 1) + 1;
            h[1] ^= (guint64) (gsize) t;
            tex_hash (t->data, t->len, h);
        } else {
            tex_hash (t->data, t->len, h);
        }
        for (int i = 0; i < t->n_levels; i++)
            tex_hash (t->level[i].data, t->level[i].len, h);
        if (t->nframes > 0)
            tex_hash (t->frames, (gsize) t->nframes * sizeof (WalpeTexFrame), h);
        *ckey = g_strdup_printf ("#%016" G_GINT64_MODIFIER "x%016" G_GINT64_MODIFIER "x|%d|%d|%d|%d"
                                 "|%d|%d|%d|%d|%u|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d",
                                 h[0], h[1], t->width, t->height, t->base_width, t->base_height,
                                 (int) t->format, t->channels, t->we_format, t->mips != 1,
                                 t->tex_flags & WALPE_TEXF_NO_INTERPOLATION, t->n_levels, u.skip,
                                 t->format != WALPE_TEX_RGBA8 && u.no_chain, u.layer_swizzle, halvings,
                                 wrap, premultiply ? 1 : 0,
                                 t->nframes, t->frame_w, t->frame_h,
                                 halvings > 0 && (load_flags & WALPE_GL_TEX_CEIL));
        *hit = g_hash_table_lookup (tc_by_content, *ckey);
        if (*hit) {
            *t_out = t;
            return (*hit)->tex;
        }
    }
    gboolean ceil = (load_flags & WALPE_GL_TEX_CEIL) != 0;
    if (prep.valid || (halvings > 0 && tex_cpu_reduce_on () && tex_cpu_can_halve (t, halvings, ceil))) {
        /* never at full size on the GPU: reduced (by a worker, or here), then uploaded */
        if (!prep.valid)
            tex_prepare (t, halvings, premultiply, u.layer_swizzle, ceil, &prep);
        GLuint small = tex_upload_prepared (t, wrap, &prep, &u);
        g_atomic_int_inc (&tj_cpu_reduced);
        tex_info_from (info, t, &u);
        info->bytes = info->bytes / ((gint64) info->phys_w * info->phys_h / MAX (1, prep.w * prep.h));
        info->phys_w = prep.w;
        info->phys_h = prep.h;
        info->lod = got + halvings;
        info->want_mips = walpe_gl_mipmaps_enabled () && t->mips != 1;
        *t_out = t;
        return small;
    }
    GLuint tex = upload_tex (t, wrap, premultiply, &u);
    tex_info_from (info, t, &u);
    info->lod = got;
    if (halvings > 0) {
        int nw = 0, nh = 0;
        /* the decoded pixels are on the GPU now: not kept next to the
         * full-size texture and its copy while that is made */
        g_clear_pointer (&t->data, g_free);
        t->len = 0;
        GLuint small = tex_halve (tex, wrap, halvings, (load_flags & WALPE_GL_TEX_CEIL) != 0,
                                  &nw, &nh);
        if (small) {
            g_atomic_int_inc (&tj_gl_halved);
            tex_live_del (tex);
            glDeleteTextures (1, &tex);
            tex = small;
            info->bytes = info->bytes / ((gint64) info->phys_w * info->phys_h / MAX (1, nw * nh));
            info->phys_w = nw;
            info->phys_h = nh;
            info->lod = got + halvings;
            /* the copy may get its own chain (walpe_gl_tex_ensure_mipmaps) */
            info->want_mips = walpe_gl_mipmaps_enabled () && t->mips != 1;
        }
    }
    *t_out = t;
    return tex;
}

static GLuint tex_cached (const char *path, guint load_flags, GLint wrap, gboolean premultiply,
                          WalpeTexInfo *info, WalpeTexFrame **frames, const TexEntry *own)
{
    memset (info, 0, sizeof *info);
    if (frames)
        *frames = NULL;
    if (!g_atomic_int_get (&tc_shared)) {
        WalpeTex *t = NULL;
        /* own: the caller's texture of this file at full size (there is no
         * cache to find it in): a reduced copy is made from it instead of
         * decoding the file a second time */
        GLuint tex = tex_make (path, load_flags, wrap, premultiply, info, &t, NULL, NULL, own);
        if (t && frames)
            *frames = g_steal_pointer (&t->frames);
        walpe_tex_free (t);
        if (tex && tc_scope)
            tc_scope_bytes += info->bytes;
        return tex;
    }
    char *key = g_strdup_printf ("%s|%u|%d|%d", path, load_flags, wrap, premultiply ? 1 : 0);
    g_mutex_lock (&tc_lock); /* held across a miss: the other monitors wait, then hit */
    if (!tc_by_key) {
        tc_by_key = g_hash_table_new (g_str_hash, g_str_equal);
        tc_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        tc_by_content = g_hash_table_new (g_str_hash, g_str_equal);
        tc_by_tex = g_hash_table_new (g_direct_hash, g_direct_equal);
        tc_seed_init ();
    }
    TexEntry *e = g_hash_table_lookup (tc_by_key, key);
    WalpeTex *t = NULL;
    char *ckey = NULL;
    GLuint tex = 0;
    if (!e) {
        TexEntry *same = NULL, *full0 = NULL;
        if (WALPE_GL_TEX_LEVEL_OF (load_flags) > 0) {
            char *k0 = g_strdup_printf ("%s|%u|%d|%d", path,
                                        load_flags & ~(WALPE_GL_TEX_LEVEL_MASK | WALPE_GL_TEX_NEAR_OK |
                                                       WALPE_GL_TEX_CEIL),
                                        wrap, premultiply ? 1 : 0);
            full0 = g_hash_table_lookup (tc_by_key, k0);
            g_free (k0);
            if (full0 && (full0->info.compressed || full0->info.lod != 0))
                full0 = NULL;
        }
        tex = tex_make (path, load_flags, wrap, premultiply, info, &t, &ckey, &same, full0);
        if (!tex) {
            g_mutex_unlock (&tc_lock);
            g_free (key);
            g_free (ckey);
            walpe_tex_free (t);
            return 0;
        }
        if (same) {
            /* these pixels are on the GPU already under another name */
            g_debug ("gl: texture %s holds the pixels of %s: shared", walpe_log_str (path), same->key);
            e = same;
            if (!e->alias)
                e->alias = g_ptr_array_new_with_free_func (g_free);
            g_ptr_array_add (e->alias, key);
            tc_key_add (key, e);
            key = NULL;
            g_free (ckey);
        }
    }
    if (e) {
        e->refs++;
        *info = e->info;
        tex = e->tex;
        if (frames && e->frames && e->info.nframes > 0)
            *frames = g_memdup2 (e->frames, (gsize) e->info.nframes * sizeof (WalpeTexFrame));
        g_mutex_unlock (&tc_lock);
        g_free (key);
        walpe_tex_free (t);
        if (tc_scope)
            tc_scope_bytes += info->bytes;
        return tex;
    }
    /* other contexts may sample it right away: the upload must be complete */
    glFinish ();
    e = g_new0 (TexEntry, 1);
    e->key = key;
    e->ckey = ckey;
    e->tex = tex;
    e->refs = 1;
    e->info = *info;
    if (t && t->nframes > 0) {
        e->frames = g_steal_pointer (&t->frames);
        if (frames)
            *frames = g_memdup2 (e->frames, (gsize) t->nframes * sizeof (WalpeTexFrame));
    }
    walpe_tex_free (t);
    tc_key_add (e->key, e);
    if (e->ckey)
        g_hash_table_insert (tc_by_content, e->ckey, e);
    g_hash_table_insert (tc_by_tex, GUINT_TO_POINTER (tex), e);
    g_mutex_unlock (&tc_lock);
    if (tc_scope)
        tc_scope_bytes += info->bytes;
    return tex;
}

GLuint walpe_gl_tex_cached (const char *path, guint load_flags, GLint wrap, gboolean premultiply,
                            WalpeTexInfo *info)
{
    return tex_cached (path, load_flags, wrap, premultiply, info, NULL, NULL);
}

GLuint walpe_gl_tex_cached_own (const char *path, guint load_flags, GLint wrap, gboolean premultiply,
                                WalpeTexInfo *info, GLuint own_tex, const WalpeTexInfo *own_info)
{
    TexEntry own = { .tex = own_tex };
    gboolean use = own_tex && own_info && !own_info->compressed && own_info->lod == 0 &&
                   WALPE_GL_TEX_LEVEL_OF (load_flags) > 0 && tex_own_full_on ();
    if (use)
        own.info = *own_info;
    return tex_cached (path, load_flags, wrap, premultiply, info, NULL, use ? &own : NULL);
}

GLuint walpe_gl_tex_cached_frames (const char *path, guint load_flags, GLint wrap,
                                   gboolean premultiply, WalpeTexInfo *info,
                                   WalpeTexFrame **frames)
{
    return tex_cached (path, load_flags, wrap, premultiply, info, frames, NULL);
}

void walpe_gl_tex_release (GLuint tex)
{
    if (!tex)
        return;
    g_mutex_lock (&tc_lock);
    TexEntry *e = tc_by_tex ? g_hash_table_lookup (tc_by_tex, GUINT_TO_POINTER (tex)) : NULL;
    if (e && --e->refs > 0) {
        g_mutex_unlock (&tc_lock);
        return;
    }
    if (e) {
        tc_key_remove (e->key);
        for (guint i = 0; e->alias && i < e->alias->len; i++)
            tc_key_remove (g_ptr_array_index (e->alias, i));
        if (e->ckey)
            g_hash_table_remove (tc_by_content, e->ckey);
        g_hash_table_remove (tc_by_tex, GUINT_TO_POINTER (tex));
        g_clear_pointer (&e->alias, g_ptr_array_unref);
        g_free (e->key);
        g_free (e->ckey);
        g_free (e->frames);
        g_free (e);
    }
    g_mutex_unlock (&tc_lock);
    tex_live_del (tex);
    glDeleteTextures (1, &tex);
}

GLuint walpe_gl_tex_from_walpetex (WalpeTex *t, gboolean *premult)
{
    return walpe_gl_upload_tex (t, GL_CLAMP_TO_EDGE, TRUE, premult);
}

GLuint walpe_gl_tex_from_pixbuf (GdkPixbuf *pb)
{
    if (!pb)
        return 0;
    if (!gdk_pixbuf_get_has_alpha (pb)) {
        GdkPixbuf *a = gdk_pixbuf_add_alpha (pb, FALSE, 0, 0, 0);
        g_object_unref (pb);
        pb = a;
        if (!pb) {
            g_warning ("texture: no memory for a picture's alpha channel");
            return 0;
        }
    }
    int w = gdk_pixbuf_get_width (pb);
    int h = gdk_pixbuf_get_height (pb);
    int rs = gdk_pixbuf_get_rowstride (pb);
    if (gdk_pixbuf_get_bits_per_sample (pb) != 8 || gdk_pixbuf_get_n_channels (pb) != 4 ||
        w < 1 || h < 1 || rs < w * 4) {
        g_object_unref (pb);
        return 0;
    }
    guint8 *px = malloc ((gsize) w * h * 4); /* malloc: asm kernel touches it */
    if (!px) {
        g_warning ("texture: no memory for a %d x %d picture", w, h);
        g_object_unref (pb);
        return 0;
    }
    const guint8 *src = gdk_pixbuf_read_pixels (pb);
    for (int y = 0; y < h; y++)
        memcpy (px + (gsize) y * w * 4, src + (gsize) y * rs, (gsize) w * 4);
    walpe_premultiply_rgba8 (px, (gsize) w * h * 4);

    GLuint tex = 0;
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    walpe_gl_tex_gen_mipmaps (w, h);
    free (px);
    g_object_unref (pb);
    return tex;
}

/* walpe_gl_tex_from_file loads outside the texture cache (layers of the old
 * scene format, sprites of the old particle format, .tex image wallpapers):
 * inside a walpe_gl_tex_scope they count against its budget like the cached
 * ones, and a picture that would take the sum beyond it is stored smaller.
 * They used to be neither counted nor held to it: a scene in the old layout,
 * or a current one with old-format particle objects, took 341 MiB of
 * texture for every 30 KB picture it named, without end. */
static gint64 tex_direct_bytes (int w, int h, int bpp)
{
    return (gint64) w * h * bpp * 4 / 3;    /* with a mip chain */
}

static gboolean tex_scope_over (int w, int h, int bpp)
{
    return tc_scope_budget > 0 && w > 64 && h > 64 &&
           tc_scope_bytes + tex_direct_bytes (w, h, bpp) > tc_scope_budget;
}

/* The picture formats a wallpaper's layers and sprites are made of, and the
 * one gdk-pixbuf loader each is read by: "png", "jpeg", "bmp", "gif", "tiff",
 * "webp", "avif", "jxl", "tga", settled by walpe_pichead_file_kind () from
 * the file's first bytes (pichead.h) before gdk-pixbuf is asked anything.
 * gdk-pixbuf left to choose (gdk_pixbuf_get_file_info, gdk_pixbuf_new_from_
 * file) picks a loader by the bytes itself, whatever the name, and the name
 * of the format can be asked only after that loader has read the file's
 * header - for some the whole file: a layer "image" that is an SVG document,
 * a Windows cursor or icon, an XPM or a PNM went to those decoders before
 * it was refused. And a loader asked for by name is that format's decoder
 * only where gdk-pixbuf has decoders of its own: through glycin it decodes
 * whatever the bytes are. So the bytes have to be the format's here, and
 * the name of a file settles nothing but which TGA header is a TGA's. */

typedef struct { int w, h; gboolean seen; } TexFileSize;

static void tex_file_size_seen (GdkPixbufLoader *ld, int w, int h, gpointer user)
{
    TexFileSize *z = user;
    z->w = w;
    z->h = h;
    z->seen = TRUE;
    gdk_pixbuf_loader_set_size (ld, 0, 0);     /* the loaders stop before a pixel exists */
}

/* the size the file declares, as the loader of that one name reads it
 * (what gdk_pixbuf_get_file_info does with the loader it picks): FALSE when
 * this gdk-pixbuf has no such loader or the header does not say */
static gboolean tex_file_size (const char *path, const char *kind, int *w, int *h)
{
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new_with_type (kind, NULL);
    FILE *f = ld ? fopen (path, "rb") : NULL;
    TexFileSize z = { 0, 0, FALSE };
    if (f) {
        guint8 buf[4096];
        gsize n;
        g_signal_connect (ld, "size-prepared", G_CALLBACK (tex_file_size_seen), &z);
        while (!z.seen && (n = fread (buf, 1, sizeof buf, f)) > 0)
            if (!gdk_pixbuf_loader_write (ld, buf, n, NULL))
                break;
        fclose (f);
    }
    if (ld) {
        gdk_pixbuf_loader_close (ld, NULL);
        g_object_unref (ld);
    }
    *w = z.w;
    *h = z.h;
    return z.seen;
}

/* a PNG file has its last chunk (IEND): the chunks are walked by their
 * lengths. gdk-pixbuf's PNG decoder fed piece by piece says nothing when
 * the file ends early and hands out the rows it has (none: an empty
 * picture); reading the file in one go, as this path did, it refused. */
static gboolean tex_file_png_whole (const char *path)
{
    FILE *f = fopen (path, "rb");
    guint8 c[8];
    gboolean whole = FALSE;
    if (f && fseek (f, 8, SEEK_SET) == 0) {
        for (guint n = 0; n < 100000 && fread (c, 1, 8, f) == 8; n++) {
            const guint32 len = (guint32) c[0] << 24 | (guint32) c[1] << 16 | (guint32) c[2] << 8 | c[3];
            if (!memcmp (c + 4, "IEND", 4)) {
                guint8 crc[4];
                whole = len == 0 && fread (crc, 1, 4, f) == 4;
                break;
            }
            /* the chunk's data and its checksum are in the file */
            if (len > 0x7fffffff || fseek (f, (long) len + 3, SEEK_CUR) != 0 || fgetc (f) == EOF)
                break;
        }
    }
    if (f)
        fclose (f);
    return whole;
}

static void tex_file_scale (GdkPixbufLoader *ld, int w, int h, gpointer user)
{
    const int *to = user;
    (void) w;
    (void) h;
    gdk_pixbuf_loader_set_size (ld, to[0], to[1]);
}

/* n bytes of the file from position pos on to the loader - of a TGA whose
 * header is not the one its decoders are given (pichead.h, walpe_tga_head):
 * that header in place of the first 18 bytes, and nothing of the colour
 * map no pixel uses */
static gboolean tex_file_feed (GdkPixbufLoader *ld, const WalpeTga *tga, guint64 pos, const guint8 *buf,
                               gsize n, GError **err)
{
    if (!tga || !tga->changed)
        return gdk_pixbuf_loader_write (ld, buf, n, err);
    const guint64 cut0 = tga->skip_off, cut1 = cut0 + tga->skip_len;
    gsize i = 0;
    if (pos < 18) {
        const gsize k = MIN ((gsize) (18 - pos), n);
        if (!gdk_pixbuf_loader_write (ld, tga->head + pos, k, err))
            return FALSE;
        i = k;
    }
    while (i < n) {
        const guint64 at = pos + i;
        gsize k = n - i;
        if (at >= cut0 && at < cut1) {
            i += (gsize) MIN ((guint64) k, cut1 - at);
            continue;
        }
        if (at < cut0)
            k = (gsize) MIN ((guint64) k, cut0 - at);
        if (!gdk_pixbuf_loader_write (ld, buf + i, k, err))
            return FALSE;
        i += k;
    }
    return TRUE;
}

GdkPixbuf *walpe_gl_picture_file_decode (const char *path, const char *kind, const WalpeTga *tga, int tw,
                                         int th, GdkPixbufAnimation **anim, GError **err)
{
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new_with_type (kind, err);
    FILE *f = ld ? fopen (path, "rb") : NULL;
    GdkPixbuf *pb = NULL;
    int to[2] = { tw, th };
    gboolean ok = f != NULL;
    if (anim)
        *anim = NULL;
    if (f) {
        guint8 *buf = g_malloc (65536);
        gsize n;
        guint64 pos = 0;
        if (tw > 0 && th > 0)
            g_signal_connect (ld, "size-prepared", G_CALLBACK (tex_file_scale), to);
        while (ok && (n = fread (buf, 1, 65536, f)) > 0) {
            ok = tex_file_feed (ld, tga, pos, buf, n, err);
            pos += n;
        }
        fclose (f);
        g_free (buf);
    }
    if (ld) {
        ok = gdk_pixbuf_loader_close (ld, ok ? err : NULL) && ok;
        pb = ok ? gdk_pixbuf_loader_get_pixbuf (ld) : NULL;
        if (pb)
            g_object_ref (pb);
        if (pb && anim) {
            /* (deprecated in gdk-pixbuf 2.44 with its whole animation
             * interface, without a successor inside the library) */
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            *anim = gdk_pixbuf_loader_get_animation (ld);
            G_GNUC_END_IGNORE_DEPRECATIONS
            if (*anim)
                g_object_ref (*anim);
        }
        g_object_unref (ld);
    }
    if (!pb && err && !*err)
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "no picture in it");
    return pb;
}

gboolean walpe_gl_picture_file_size (const char *path, const char *kind, int *w, int *h)
{
    return tex_file_size (path, kind, w, h);
}

WalpeGlTex walpe_gl_tex_from_file (const char *path)
{
    WalpeGlTex out = { 0, TRUE, 0, 0 };
    if (!path)
        return out;
    WalpeTex *t = walpe_tex_load_ex (path, WALPE_TEX_LOAD_NATIVE_RG |
                                     (walpe_gl_has_s3tc () ? WALPE_TEX_LOAD_NATIVE_DXT : 0));
    if (t) {
        out.width = t->width;
        out.height = t->height;
        int bpp = t->channels == 1 || t->channels == 2 ? t->channels : 4, halved = 0;
        while (t->format == WALPE_TEX_RGBA8 && t->nframes == 0 && halved < 8 &&
               tex_scope_over (t->width, t->height, bpp)) {
            tex_cpu_halve (t);
            halved++;
        }
        if (halved)
            g_warning ("gl: texture budget (%.1f MiB) reached: %s stored at 1/%d of its size",
                       tc_scope_budget / 1048576.0, walpe_log_str (path), 1 << halved);
        {
            /* larger than the driver's largest texture: halved until it fits */
            int lim = tex_side_limit (), n = 0;
            while (lim && t->format == WALPE_TEX_RGBA8 && t->nframes == 0 && n < 8 &&
                   (t->width > lim || t->height > lim)) {
                tex_cpu_halve (t);
                n++;
            }
            if (n)
                g_warning ("gl: %s is larger than this driver's largest texture (%d texels a "
                           "side): stored at %d x %d", walpe_log_str (path), lim, t->width, t->height);
        }
        out.tex = walpe_gl_tex_from_walpetex (t, &out.premult);
        if (out.tex && tc_scope)
            tc_scope_bytes += t->format == WALPE_TEX_RGBA8
                ? tex_direct_bytes (t->width, t->height, bpp) : (gint64) t->len * 4 / 3;
        walpe_tex_free (t);
        return out;
    }
    GError *err = NULL;
    GdkPixbuf *pb = NULL;
    /* Pictures the loader above does not take (other formats, or more
     * pixels than one texture gets: 16384 per side, 8192 x 8192 in all).
     * The size is read from the header first: an oversized picture is
     * loaded scaled down to those limits instead of at full size (a few KB
     * of PNG can declare a gigabyte of pixels). JPEG scales while it
     * decodes; other formats decode in full first, so they are refused
     * above 16384 x 16384 pixels. A file whose header does not say how
     * large the picture is, or that is none of the formats above, is not
     * decoded at all. */
    int iw = 0, ih = 0;
    /* (a "BM" file without the header sizes of a BMP file: refused by the
     * loader above for what its decoder would reserve, not read here either) */
    WalpeTga tga = { 0 };
    const char *kind = walpe_bmp_file_header_ok (path) ? walpe_pichead_file_kind (path, &tga) : NULL;
    const gint64 max_side = 16384, max_px = (gint64) 8192 * 8192;
    /* (and a JPEG, under whatever name, that is arithmetic-coded: its decoder
     * may never come back - util.h) */
    if (kind && !walpe_jpeg_file_coding_ok (path)) {
        g_warning ("texture %s: " WALPE_JPEG_ARITH_MSG, walpe_log_str (path));
        return out;
    }
    if (kind && !strcmp (kind, "png") && !tex_file_png_whole (path))
        kind = NULL;
    /* The size, from the header as WalPE reads it (pichead.h), before a
     * loader exists: asked for the size alone, a loader that decodes
     * through glycin decodes the whole picture first, whatever it is told.
     * For the formats whose header is in one fixed place that is the size,
     * and a file without it is no picture of that format. For TIFF, AVIF
     * and JPEG XL it is an upper bound - past the largest size any path
     * below decodes, the file is refused here - and the loader is asked as
     * before for what it will hand out. A file of those three without a
     * size the reader finds is not handed to a loader either: where the
     * loader decodes through glycin the question alone is a full decode,
     * at whatever size the data has (an AVIF without "ispe": pichead.c;
     * an AVIF image sequence has the size it declares, and is handed on). */
    gboolean sized = kind && walpe_pichead_file_size (kind, path, &iw, &ih);
    if (kind && !sized && !walpe_pichead_exact (kind)) {
        g_warning ("texture %s: not loaded, its %s header does not say how large the picture is",
                   walpe_log_str (path), kind);
        return out;
    }
    if (sized && !walpe_pichead_exact (kind)) {
        if ((gint64) iw * ih > max_side * max_side) {
            g_warning ("texture %s: %d x %d pixels is too large to load", walpe_log_str (path), iw, ih);
            return out;
        }
        sized = tex_file_size (path, kind, &iw, &ih);
    }
    if (!kind || !sized || iw <= 0 || ih <= 0) {
        g_debug ("texture %s: not a picture format a wallpaper texture can have", walpe_log_str (path));
        return out;
    }
    /* a GIF (its first frame is the texture): only with every frame inside
     * the picture its header declares (gifcheck.h); the loader above refuses
     * the same file by the same rule and must not be gone around here */
    if (!walpe_gif_file_layout_ok (path, &err)) {
        g_debug ("texture %s: %s", walpe_log_str (path), err ? err->message : "not loaded");
        g_clear_error (&err);
        return out;
    }
    if (iw > max_side || ih > max_side || (gint64) iw * ih > max_px) {
        gboolean jpeg = !strcmp (kind, "jpeg");
        if (!jpeg && (gint64) iw * ih > max_side * max_side) {
            g_warning ("texture %s: %d x %d pixels is too large to load", walpe_log_str (path), iw, ih);
            return out;
        }
        /* "JPEG scales while it decodes" holds for a file that is decoded
         * as it is read. A progressive one (or one with a scan per
         * component) is first read whole into a store of 128 bytes per
         * 8 x 8 block of every component at the size the file declares,
         * whatever size was asked for (util.h): 136 bytes of file that
         * declare 32000 x 16000 took a gigabyte here, 65500 x 65500 would
         * ask for 8 to 34. Such a file is refused beyond the store the
         * largest picture of another format costs decoded, as a preview
         * is. */
        if (jpeg && walpe_jpeg_store_bytes_file (path) > WALPE_JPEG_STORE_MAX) {
            g_warning ("texture %s: a JPEG of %d x %d pixels that has to be held whole to be "
                       "decoded is too large to load", walpe_log_str (path), iw, ih);
            return out;
        }
        double k = MIN (MIN ((double) max_side / iw, (double) max_side / ih),
                        sqrt ((double) max_px / ((double) iw * ih)));
        int tw = MAX (1, (int) (iw * k)), th = MAX (1, (int) (ih * k));
        g_message ("texture %s: %d x %d pixels, loaded at %d x %d", walpe_log_str (path), iw, ih, tw, th);
        pb = walpe_gl_picture_file_decode (path, kind, &tga, tw, th, NULL, &err);
    } else {
        pb = walpe_gl_picture_file_decode (path, kind, &tga, 0, 0, NULL, &err);
    }
    if (err || !pb) {
        g_debug ("texture %s: %s", walpe_log_str (path), err ? err->message : "not loaded");
        g_clear_error (&err);
        g_clear_object (&pb);
        return out;
    }
    GdkPixbuf *rot = gdk_pixbuf_apply_embedded_orientation (pb);   /* EXIF */
    if (rot) {
        g_object_unref (pb);
        pb = rot;
    }
    out.width = gdk_pixbuf_get_width (pb);
    out.height = gdk_pixbuf_get_height (pb);
    /* what was really decoded (the file may have been replaced since its
     * header was read, and an animation's frame need not be what the
     * header announced) */
    if (out.width < 1 || out.height < 1 || out.width > max_side || out.height > max_side ||
        (gint64) out.width * out.height > max_px) {
        g_warning ("texture %s: decoded to %d x %d pixels, not loaded", walpe_log_str (path), out.width, out.height);
        g_object_unref (pb);
        out.width = out.height = 0;
        return out;
    }
    int sw = out.width, sh = out.height;
    while (tex_scope_over (sw, sh, 4)) {
        sw = MAX (1, sw / 2);
        sh = MAX (1, sh / 2);
    }
    gboolean by_budget = sw != out.width;
    for (int lim = tex_side_limit (); lim && (sw > lim || sh > lim);) {
        /* larger than the driver's largest texture */
        sw = MAX (1, sw / 2);
        sh = MAX (1, sh / 2);
    }
    if (sw != out.width) {
        GdkPixbuf *small = gdk_pixbuf_scale_simple (pb, sw, sh, GDK_INTERP_BILINEAR);
        if (small && !by_budget) {
            g_warning ("gl: %s is larger than this driver's largest texture (%d texels a side): "
                       "stored at %d x %d", walpe_log_str (path), tex_side_limit (), sw, sh);
            g_object_unref (pb);
            pb = small;
        } else if (small) {
            g_warning ("gl: texture budget (%.1f MiB) reached: %s stored at %d x %d",
                       tc_scope_budget / 1048576.0, walpe_log_str (path), sw, sh);
            g_object_unref (pb);
            pb = small;
        } else {
            sw = out.width;
            sh = out.height;
        }
    }
    out.tex = walpe_gl_tex_from_pixbuf (pb);
    out.premult = TRUE;
    if (out.tex && tc_scope)
        tc_scope_bytes += tex_direct_bytes (sw, sh, 4);
    return out;
}

/* ------------------------------------------------------------- mat3 */

void walpe_mat3_cover (WalpeMat3 *m, float iw, float ih, float vw, float vh)
{
    walpe_mat3_identity (m);
    if (iw < 1 || ih < 1 || vw < 1 || vh < 1)
        return;
    float k = fmaxf (vw / iw, vh / ih);
    /* the quad is +-1 clip-style (glutil verts): covering (iw*k, ih*k) px
     * in a (vw, vh) px viewport is a plain scale — the quad is already
     * centered, no translate (a 2x scale + -0.5 shift draws a 2x overscan
     * crop biased to one corner). Both factors POSITIVE: quad top (v=0,
     * top row of a top-down upload) must stay at clip top — a negative
     * Y factor mirrors the quad and draws the content upside down
     * (scene.c's chain reaches the same orientation via two negations) */
    walpe_mat3_scale (m, iw * k / vw, ih * k / vh);
}

void walpe_mat3_identity (WalpeMat3 *m)
{
    memset (m->m, 0, sizeof (m->m));
    m->m[0] = m->m[4] = m->m[8] = 1.0f;
}

void walpe_mat3_mul (WalpeMat3 *o, const WalpeMat3 *a, const WalpeMat3 *b)
{
    WalpeMat3 r;
    for (int c = 0; c < 3; c++)
        for (int rr = 0; rr < 3; rr++) {
            float s = 0;
            for (int k = 0; k < 3; k++)
                s += a->m[k * 3 + rr] * b->m[c * 3 + k];
            r.m[c * 3 + rr] = s;
        }
    *o = r;
}

void walpe_mat3_translate (WalpeMat3 *m, float x, float y)
{
    WalpeMat3 t = { { 1, 0, 0, 0, 1, 0, x, y, 1 } };
    walpe_mat3_mul (m, &t, m);
}

void walpe_mat3_scale (WalpeMat3 *m, float x, float y)
{
    WalpeMat3 s = { { x, 0, 0, 0, y, 0, 0, 0, 1 } };
    walpe_mat3_mul (m, &s, m);
}

void walpe_mat3_rotate (WalpeMat3 *m, float rad)
{
    float c = cosf (rad), s = sinf (rad);
    WalpeMat3 r = { { c, s, 0, -s, c, 0, 0, 0, 1 } };
    walpe_mat3_mul (m, &r, m);
}
