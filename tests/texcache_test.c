/* texcache_test.c - the shared file-texture cache (engine/glutil.c):
 *  1. RGBA8 pixels that hold one or two channels are stored as R8 / RG8
 *     with a swizzle that returns the same four channels;
 *  2. the same pixels under two file names are one GL texture, released
 *     when the last user lets go;
 *  3. WALPE_GL_TEX_LEVEL takes the file's own mip level when it stores one
 *     and halves level 0 on the GPU when it does not;
 *  4. DXT stays block compressed, starts at the stored level asked for,
 *     and comes without the levels below it on request;
 *  5. the texture budget of a loading scope stores what does not fit
 *     smaller.
 * Needs a GL 3.3 context (EGL device or Mesa surfaceless platform); skips
 * itself without one. Writes its .tex files to a temporary directory. */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdio.h>
#include <string.h>
#include "engine/glutil.h"

static int fails;

static void check (gboolean ok, const char *what)
{
    printf ("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

static void put32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
    g_byte_array_append (a, b, 4);
}

/* a TEXB0003 container with raw levels; data[k] = level k (w >> k x h >> k) */
static char *write_tex (const char *dir, const char *name, guint32 type, int w, int h,
                        int nlevels, guint8 **data, gsize *len)
{
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
    put32 (a, type);
    put32 (a, 2);
    put32 (a, (guint32) w);
    put32 (a, (guint32) h);
    put32 (a, (guint32) w);
    put32 (a, (guint32) h);
    put32 (a, 0);
    g_byte_array_append (a, (const guint8 *) "TEXB0003\0", 9);
    put32 (a, 1);
    put32 (a, 0xffffffffu);
    put32 (a, (guint32) nlevels);
    for (int k = 0; k < nlevels; k++) {
        put32 (a, (guint32) MAX (1, w >> k));
        put32 (a, (guint32) MAX (1, h >> k));
        put32 (a, 0);
        put32 (a, (guint32) len[k]);
        put32 (a, (guint32) len[k]);
        g_byte_array_append (a, data[k], (guint) len[k]);
    }
    char *path = g_build_filename (dir, name, NULL);
    g_file_set_contents (path, (const char *) a->data, a->len, NULL);
    g_byte_array_unref (a);
    return path;
}

/* w x h RGBA texels from a per-texel function of (x, y) */
typedef void (*TexelFn) (int x, int y, guint8 *px);

static guint8 *rgba (int w, int h, TexelFn fn)
{
    guint8 *d = g_malloc ((gsize) w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            fn (x, y, d + ((gsize) y * w + x) * 4);
    return d;
}

static void px_gray (int x, int y, guint8 *p) { p[0] = p[1] = p[2] = (guint8) (x + y); p[3] = 255; }
static void px_red (int x, int y, guint8 *p) { p[0] = (guint8) (x * 2 + y); p[1] = p[2] = 0; p[3] = 255; }
static void px_grayalpha (int x, int y, guint8 *p) { p[0] = p[1] = p[2] = (guint8) x; p[3] = (guint8) (y + 1); }
static void px_color (int x, int y, guint8 *p) { p[0] = (guint8) x; p[1] = (guint8) y; p[2] = (guint8) (x ^ y); p[3] = 255; }
static void px_marker (int x, int y, guint8 *p) { (void) x; (void) y; p[0] = 10; p[1] = 200; p[2] = 30; p[3] = 255; }
static void px_checker (int x, int y, guint8 *p) { p[0] = ((x ^ y) & 1) ? 200 : 100; p[1] = 40; p[2] = 80; p[3] = 255; }

static char *one_level (const char *dir, const char *name, int w, int h, TexelFn fn)
{
    guint8 *d = rgba (w, h, fn);
    gsize len = (gsize) w * h * 4;
    char *p = write_tex (dir, name, 0, w, h, 1, &d, &len);
    g_free (d);
    return p;
}

static GLint level_param (GLuint tex, int level, GLenum pname)
{
    GLint v = 0;
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, level, pname, &v);
    return v;
}

static GLint tex_param (GLuint tex, GLenum pname)
{
    GLint v = 0;
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexParameteriv (GL_TEXTURE_2D, pname, &v);
    return v;
}

/* texel 0,0 of level 0 as stored (no swizzle) */
static void first_texel (GLuint tex, guint8 px[4])
{
    int w = level_param (tex, 0, GL_TEXTURE_WIDTH), h = level_param (tex, 0, GL_TEXTURE_HEIGHT);
    guint8 *buf = g_malloc0 ((gsize) w * h * 4 + 16);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    memcpy (px, buf, 4);
    g_free (buf);
}

/* a solid-colour DXT5 level of w x h texels */
static guint8 *dxt5_solid (int w, int h, guint8 r, guint8 g, guint8 b, gsize *len)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    guint16 c = (guint16) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    guint8 blk[16] = { 255, 255, 0, 0, 0, 0, 0, 0, (guint8) c, (guint8) (c >> 8), (guint8) c,
                       (guint8) (c >> 8), 0, 0, 0, 0 };
    *len = (gsize) bw * bh * 16;
    guint8 *d = g_malloc (*len);
    for (int i = 0; i < bw * bh; i++)
        memcpy (d + i * 16, blk, 16);
    return d;
}

static void run (const char *dir)
{
    WalpeTexInfo ti;
    guint8 px[4];
    walpe_gl_set_shared (TRUE);

    /* 1. compact storage */
    char *f_gray = one_level (dir, "gray.tex", 128, 128, px_gray);
    char *f_red = one_level (dir, "red.tex", 128, 128, px_red);
    char *f_ga = one_level (dir, "grayalpha.tex", 128, 128, px_grayalpha);
    char *f_col = one_level (dir, "color.tex", 128, 128, px_color);
    GLuint t_gray = walpe_gl_tex_cached (f_gray, 0, GL_REPEAT, FALSE, &ti);
    check (t_gray && level_param (t_gray, 0, GL_TEXTURE_INTERNAL_FORMAT) == GL_R8 &&
           tex_param (t_gray, GL_TEXTURE_SWIZZLE_G) == GL_RED &&
           tex_param (t_gray, GL_TEXTURE_SWIZZLE_A) == GL_ONE && ti.bytes == 128 * 128,
           "gray opaque RGBA8 -> R8, swizzle (R,R,R,1)");
    check (ti.width == 128 && ti.height == 128 && ti.format == WALPE_TEX_RGBA8 && ti.channels == 0,
           "  ... still reported as a plain RGBA8 128x128 texture");
    GLuint t_red = walpe_gl_tex_cached (f_red, 0, GL_REPEAT, FALSE, &ti);
    check (t_red && level_param (t_red, 0, GL_TEXTURE_INTERNAL_FORMAT) == GL_R8 &&
           tex_param (t_red, GL_TEXTURE_SWIZZLE_G) == GL_ZERO &&
           tex_param (t_red, GL_TEXTURE_SWIZZLE_A) == GL_ONE,
           "red-only opaque RGBA8 -> R8, swizzle (R,0,0,1)");
    GLuint t_ga = walpe_gl_tex_cached (f_ga, 0, GL_REPEAT, FALSE, &ti);
    check (t_ga && level_param (t_ga, 0, GL_TEXTURE_INTERNAL_FORMAT) == GL_RG8 &&
           tex_param (t_ga, GL_TEXTURE_SWIZZLE_B) == GL_RED &&
           tex_param (t_ga, GL_TEXTURE_SWIZZLE_A) == GL_GREEN, "gray + alpha -> RG8, swizzle (R,R,R,G)");
    first_texel (t_ga, px);
    check (px[0] == 0 && px[1] == 1, "  ... texel 0,0 holds (gray, alpha) = (0, 1)");
    GLuint t_col = walpe_gl_tex_cached (f_col, 0, GL_REPEAT, FALSE, &ti);
    check (t_col && level_param (t_col, 0, GL_TEXTURE_INTERNAL_FORMAT) == GL_RGBA8 &&
           ti.bytes == 128 * 128 * 4, "a colour picture stays RGBA8");

    /* 2. the same pixels under another name; the same file again */
    char *f_gray2 = one_level (dir, "gray - Copy.tex", 128, 128, px_gray);
    GLuint t_gray2 = walpe_gl_tex_cached (f_gray2, 0, GL_REPEAT, FALSE, &ti);
    check (t_gray2 == t_gray, "identical pixels under a second file name: one GL texture");
    GLuint t_gray3 = walpe_gl_tex_cached (f_gray, 0, GL_REPEAT, FALSE, &ti);
    check (t_gray3 == t_gray, "the same file again: the cached texture");
    GLuint t_gray_clamp = walpe_gl_tex_cached (f_gray, 0, GL_CLAMP_TO_EDGE, FALSE, &ti);
    check (t_gray_clamp && t_gray_clamp != t_gray, "another wrap mode: its own texture");
    walpe_gl_tex_release (t_gray);
    walpe_gl_tex_release (t_gray2);
    check (glIsTexture (t_gray), "two of three users gone: texture kept");
    walpe_gl_tex_release (t_gray3);
    check (!glIsTexture (t_gray), "last user gone: texture deleted");
    GLuint t_again = walpe_gl_tex_cached (f_gray2, 0, GL_REPEAT, FALSE, &ti);
    check (t_again && level_param (t_again, 0, GL_TEXTURE_WIDTH) == 128,
           "the second name loads again after the release");
    walpe_gl_tex_release (t_again);
    walpe_gl_tex_release (t_gray_clamp);

    /* 3. levels: stored (marker colour, not the average) and halved on the GPU */
    guint8 *lv[3] = { rgba (256, 256, px_checker), rgba (128, 128, px_marker), rgba (64, 64, px_color) };
    gsize ll[3] = { 256 * 256 * 4, 128 * 128 * 4, 64 * 64 * 4 };
    char *f_mips = write_tex (dir, "mips.tex", 0, 256, 256, 3, lv, ll);
    char *f_nomips = one_level (dir, "nomips.tex", 256, 256, px_checker);
    GLuint t_l1 = walpe_gl_tex_cached (f_mips, WALPE_GL_TEX_LEVEL (1), GL_CLAMP_TO_EDGE, FALSE, &ti);
    first_texel (t_l1, px);
    if (walpe_gl_tex_stored_levels ())
        check (t_l1 && ti.phys_w == 128 && ti.phys_h == 128 && ti.lod == 1 && px[0] == 10 &&
               px[1] == 200 && px[2] == 30, "LEVEL(1): the file's own level 1 (marker colour)");
    else
        check (t_l1 && ti.phys_w == 128 && ti.lod == 1 && px[0] == 150,
               "LEVEL(1) with WALPE_TEX_STORED_MIPS=0: level 0 halved");
    check (ti.width == 256 && ti.height == 256 && ti.mips == 3, "  ... sizes reported are level 0's");
    GLuint t_h1 = walpe_gl_tex_cached (f_nomips, WALPE_GL_TEX_LEVEL (1), GL_CLAMP_TO_EDGE, FALSE, &ti);
    first_texel (t_h1, px);
    check (t_h1 && ti.phys_w == 128 && ti.lod == 1 && px[0] >= 149 && px[0] <= 151 && px[1] == 40,
           "LEVEL(1) without a stored level: 2x2 average of level 0 (100/200 checker -> 150)");
    GLuint t_h2 = walpe_gl_tex_cached (f_nomips, WALPE_GL_TEX_LEVEL (2), GL_CLAMP_TO_EDGE, FALSE, &ti);
    check (t_h2 && ti.phys_w == 64 && ti.phys_h == 64 && ti.lod == 2, "LEVEL(2): a quarter of the size");
    GLuint t_h0 = walpe_gl_tex_cached (f_nomips, 0, GL_CLAMP_TO_EDGE, FALSE, &ti);
    GLuint t_h1b = walpe_gl_tex_cached (f_nomips, WALPE_GL_TEX_LEVEL (1), GL_CLAMP_TO_EDGE, FALSE, &ti);
    check (t_h1b == t_h1 && t_h0 && t_h0 != t_h1 && ti.phys_w == 128, "levels are cached per level");
    walpe_gl_tex_release (t_l1);
    walpe_gl_tex_release (t_h1);
    walpe_gl_tex_release (t_h1b);
    walpe_gl_tex_release (t_h2);
    walpe_gl_tex_release (t_h0);

    /* 4. DXT */
    if (walpe_gl_has_s3tc ()) {
        guint8 *dl[3];
        gsize dn[3];
        dl[0] = dxt5_solid (64, 64, 255, 0, 0, &dn[0]);
        dl[1] = dxt5_solid (32, 32, 0, 255, 0, &dn[1]);
        dl[2] = dxt5_solid (16, 16, 0, 0, 255, &dn[2]);
        char *f_dxt = write_tex (dir, "dxt.tex", 4, 64, 64, 3, dl, dn);
        char *f_dxt1 = write_tex (dir, "dxt-nomips.tex", 4, 64, 64, 1, dl, dn);
        GLuint d0 = walpe_gl_tex_cached (f_dxt, WALPE_TEX_LOAD_NATIVE_DXT, GL_CLAMP_TO_EDGE, FALSE, &ti);
        check (d0 && ti.compressed && ti.phys_w == 64 && level_param (d0, 1, GL_TEXTURE_WIDTH) == 32 &&
               level_param (d0, 2, GL_TEXTURE_WIDTH) == 16 && !ti.premult,
               "DXT5 stays compressed, with the file's chain");
        GLuint d1 = walpe_gl_tex_cached (f_dxt, WALPE_TEX_LOAD_NATIVE_DXT | WALPE_GL_TEX_LEVEL (1) |
                                         WALPE_GL_TEX_NO_CHAIN, GL_CLAMP_TO_EDGE, FALSE, &ti);
        first_texel (d1, px);
        if (walpe_gl_tex_stored_levels ())
            check (d1 && ti.compressed && ti.phys_w == 32 && ti.lod == 1 && px[1] > 200 && px[0] < 50 &&
                   level_param (d1, 1, GL_TEXTURE_WIDTH) == 0 && ti.bytes == 8 * 8 * 16,
                   "DXT5 LEVEL(1) | NO_CHAIN: stored level 1 alone, level 0 never uploaded");
        GLuint d2 = walpe_gl_tex_cached (f_dxt1, WALPE_TEX_LOAD_NATIVE_DXT | WALPE_GL_TEX_LEVEL (1),
                                         GL_CLAMP_TO_EDGE, FALSE, &ti);
        check (d2 && !ti.compressed && ti.phys_w == 32 && ti.lod == 1,
               "DXT5 without stored levels, LEVEL(1): decoded and halved");
        GLuint d3 = walpe_gl_tex_cached (f_dxt1, WALPE_TEX_LOAD_NATIVE_DXT | WALPE_GL_TEX_LEVEL (1) |
                                         WALPE_GL_TEX_NEAR_OK, GL_CLAMP_TO_EDGE, FALSE, &ti);
        check (d3 && ti.compressed && ti.phys_w == 64 && ti.lod == 0,
               "  ... with NEAR_OK: stays compressed at level 0");
        walpe_gl_tex_release (d0);
        walpe_gl_tex_release (d1);
        walpe_gl_tex_release (d2);
        walpe_gl_tex_release (d3);
        for (int i = 0; i < 3; i++)
            g_free (dl[i]);
        g_free (f_dxt);
        g_free (f_dxt1);
    } else {
        printf ("skip DXT checks (no S3TC)\n");
    }

    /* 5. texture budget of a loading scope */
    char *f_big = one_level (dir, "big.tex", 512, 512, px_color);
    walpe_gl_tex_scope_begin (300 * 1024);
    GLuint b1 = walpe_gl_tex_cached (f_col, 0, GL_CLAMP_TO_EDGE, FALSE, &ti);       /* 64 KiB: fits */
    gboolean first_full = b1 && ti.phys_w == 128 && ti.lod == 0;
    GLuint b2 = walpe_gl_tex_cached (f_big, 0, GL_CLAMP_TO_EDGE, FALSE, &ti);       /* 1 MiB: does not */
    gint64 sum = walpe_gl_tex_scope_end ();
    check (first_full && b2 && ti.phys_w == 128 && ti.lod == 2 && ti.width == 512 &&
           sum <= 300 * 1024 && sum == 2 * 128 * 128 * 4,
           "scope budget 300 KiB: a 1 MiB texture is stored at a quarter of its size");
    GLuint b3 = walpe_gl_tex_cached (f_mips, 0, GL_CLAMP_TO_EDGE, FALSE, &ti);
    check (b3 && ti.phys_w == 256 && ti.lod == 0, "outside the scope: full size");
    walpe_gl_tex_release (b1);
    walpe_gl_tex_release (b2);
    walpe_gl_tex_release (b3);

    walpe_gl_tex_release (t_red);
    walpe_gl_tex_release (t_ga);
    walpe_gl_tex_release (t_col);
    check (glGetError () == GL_NO_ERROR, "no GL error");
    for (int i = 0; i < 3; i++)
        g_free (lv[i]);
    g_free (f_gray);
    g_free (f_gray2);
    g_free (f_red);
    g_free (f_ga);
    g_free (f_col);
    g_free (f_mips);
    g_free (f_nomips);
    g_free (f_big);
}

static void rm_rf (const char *dir)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        char *p = g_build_filename (dir, n, NULL);
        g_unlink (p);
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    g_rmdir (dir);
}

int main (void)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd) && nd > 0)
            dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[0], NULL);
    }
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_NONE };
    gboolean ok = dpy != EGL_NO_DISPLAY && eglInitialize (dpy, &maj, &min) &&
                  eglBindAPI (EGL_OPENGL_API) && eglChooseConfig (dpy, attr, &cfg, 1, &nc) && nc > 0;
    if (!ok && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        ok = dpy != EGL_NO_DISPLAY && eglInitialize (dpy, &maj, &min) &&
             eglBindAPI (EGL_OPENGL_API) && eglChooseConfig (dpy, attr, &cfg, 1, &nc) && nc > 0;
    }
    if (!ok) {
        printf ("skip: no EGL device / surfaceless platform with desktop GL\n");
        return 0;
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf ("skip: no surfaceless GL 3.3 context\n");
        return 0;
    }
    char *dir = g_dir_make_tmp ("walpe-texcache-XXXXXX", NULL);
    if (!dir) {
        printf ("skip: no temporary directory\n");
        return 0;
    }
    printf ("GL: %s\n", (const char *) glGetString (GL_RENDERER));
    run (dir);
    rm_rf (dir);
    g_free (dir);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    printf ("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
