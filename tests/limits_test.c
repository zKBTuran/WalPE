/* limits_test - the input limits on wallpaper files at the three loaders
 * that work at reduced size, so that a rewrite cannot lose them:
 *
 *  1. GIF: the file is opened once for every monitor (gif.c,
 *     gif_source_get). A GIF whose canvas is above the limits of image.h
 *     is refused there, before anything is decoded and before any GL call.
 *  2. Still image: decoded at the size it is shown (image.c). The limits
 *     are on the size the FILE declares, not on the reduced size: checked
 *     when the wallpaper is created, again in gl_init (the file may have
 *     been replaced) and before every later decode at another size.
 *  3. MPRIS cover art (media.c): the decode refuses what is too large or
 *     not a regular file, and the cache of decoded covers holds at most
 *     two thumbnails of bounded size, never the input.
 *
 * Inputs: those of the other limit tests (tests/guard_test.c: a 43-byte GIF with a huge
 * canvas; tests/formats_test.c: a real PNG of 20000 x 4 pixels, a
 * project.json whose "file" leaves the wallpaper folder), plus a PNG of 65
 * bytes that declares 9000 x 9000 pixels.
 *
 * Parts 1 and 2 need no display for the refusals: they come before any GL
 * call, and the message tells a refusal by the file's header ("the picture
 * is WxH") from one after the file was opened. With an EGL device or
 * Mesa's surfaceless platform the good paths run as well: a picture
 * really is stored at the size shown, and a file that became too large is
 * refused when it is decoded again. Private temp dir = HOME. */
#include <glib.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/media.h"
#include "engine/content.h"
#include "engine/gif.h"
#include "engine/glutil.h"
#include "engine/image.h"

static int fails;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char *root;

static char *P (const char *name)
{
    return g_build_filename (root, name, NULL);
}

/* a GIF whose logical screen is w x h, with one 1x1 frame (43 bytes;
 * guard_test.c) */
static char *gif_canvas (const char *name, guint w, guint h)
{
    guint8 g[] = { 'G', 'I', 'F', '8', '9', 'a', (guint8) w, (guint8) (w >> 8), (guint8) h, (guint8) (h >> 8),
                   0x80, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff,
                   0x21, 0xf9, 4, 1, 0, 0, 0, 0,
                   0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0, 0x3b };
    char *p = P (name);
    g_file_set_contents (p, (const char *) g, sizeof g, NULL);
    return p;
}

/* a real picture of w x h pixels (formats_test.c save_png) */
static char *save_pic (const char *name, int w, int h, const char *type)
{
    gboolean alpha = !strcmp (type, "png");
    GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, alpha, 8, w, h);
    gdk_pixbuf_fill (pb, 0x102030ff);
    char *p = P (name);
    if (!gdk_pixbuf_save (pb, p, type, NULL, NULL))
        g_error ("cannot encode a %s", type);
    g_object_unref (pb);
    return p;
}

static guint32 crc32_of (const guint8 *d, gsize n)
{
    guint32 c = 0xffffffffu;
    for (gsize i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void png_chunk (GByteArray *a, const char *type, const guint8 *data, guint32 len)
{
    guint32 be = GUINT32_TO_BE (len);
    g_byte_array_append (a, (const guint8 *) &be, 4);
    guint start = a->len;
    g_byte_array_append (a, (const guint8 *) type, 4);
    if (len)
        g_byte_array_append (a, data, len);
    be = GUINT32_TO_BE (crc32_of (a->data + start, a->len - start));
    g_byte_array_append (a, (const guint8 *) &be, 4);
}

/* a PNG that is little more than its header: 65 bytes declaring w x h RGBA
 * pixels (IHDR, an empty IDAT - the size is announced when the first one
 * starts - and IEND) */
static char *png_header (const char *name, guint32 w, guint32 h)
{
    static const guint8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    static const guint8 empty_zlib[8] = { 0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01 };
    guint8 hd[13] = { 0 };
    guint32 be[2] = { GUINT32_TO_BE (w), GUINT32_TO_BE (h) };
    memcpy (hd, be, 8);
    hd[8] = 8;      /* bit depth */
    hd[9] = 6;      /* RGBA */
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, sig, sizeof sig);
    png_chunk (a, "IHDR", hd, sizeof hd);
    png_chunk (a, "IDAT", empty_zlib, sizeof empty_zlib);
    png_chunk (a, "IEND", NULL, 0);
    char *p = P (name);
    g_file_set_contents (p, (const char *) a->data, (gssize) a->len, NULL);
    g_byte_array_unref (a);
    return p;
}

static void copy_over (const char *from, const char *to)
{
    char *d = NULL;
    gsize n = 0;
    if (!g_file_get_contents (from, &d, &n, NULL) || !g_file_set_contents (to, d, (gssize) n, NULL))
        g_error ("cannot copy %s", from);
    g_free (d);
}

/* messages of the code under test, for the checks that read them */
static GString *logged;
static GLogWriterOutput log_writer (GLogLevelFlags level, const GLogField *fields, gsize n,
                                    gpointer user)
{
    (void) user;
    for (gsize i = 0; i < n; i++)
        if (!strcmp (fields[i].key, "MESSAGE") && fields[i].value) {
            g_string_append (logged, fields[i].value);
            g_string_append_c (logged, '\n');
        }
    if (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
        return g_log_writer_default (level, fields, n, NULL);
    return G_LOG_WRITER_HANDLED;
}

/* ------------------------------------------------------------- 1. GIF */

static void gif_refused (const char *path, const char *dims)
{
    GError *err = NULL;
    WalpeContent *c = walpe_gif_new (path, &err);
    CHECK (c != NULL);
    if (!c) {
        g_clear_error (&err);
        return;
    }
    /* no GL context here: the refusal comes before any GL call - and by
     * the header, before the file is opened for decoding */
    char *by_header = g_strconcat ("the picture is ", dims, NULL);
    CHECK (!c->gl_init (c, &err));
    CHECK (err && strstr (err->message, by_header) && strstr (err->message, "too large"));
    g_clear_error (&err);
    g_free (by_header);
    /* every monitor opens the same shared source: refused each time, and
     * nothing of the first attempt is left behind */
    CHECK (!c->gl_init (c, &err));
    CHECK (err != NULL);
    g_clear_error (&err);
    c->gl_free (c);
    c->free (c);
}

static void test_gif (void)
{
    char *big = gif_canvas ("big.gif", 12000, 12000);       /* 144 megapixels */
    char *tall = gif_canvas ("tall.gif", 20617, 18254);     /* above 16384 per side */
    char *wide = gif_canvas ("wide.gif", 16385, 2);         /* one pixel over, few pixels in all */
    int w = 0, h = 0;
    if (!gdk_pixbuf_get_file_info (big, &w, &h)) {
        g_print ("limits-test: no GIF loader, GIF checks skipped\n");
    } else {
        gif_refused (big, "12000x12000");
        gif_refused (tall, "20617x18254");
        gif_refused (wide, "16385x2");
    }
    g_free (big);
    g_free (tall);
    g_free (wide);
}

/* ----------------------------------------------------- 2. still image */

static void test_image (void)
{
    GError *err = NULL;
    /* refused when the wallpaper is created: by side, and by pixel count */
    char *wide = save_pic ("wide.png", 20000, 4, "png");
    char *many = png_header ("many.png", 9000, 9000);       /* 81 megapixels, both sides < 16384 */
    int w = 0, h = 0;
    CHECK (gdk_pixbuf_get_file_info (many, &w, &h) && w == 9000 && h == 9000);
    CHECK (walpe_image_new (wide, &err) == NULL);
    CHECK (err && strstr (err->message, "20000x4"));
    g_clear_error (&err);
    CHECK (walpe_image_new (many, &err) == NULL);
    CHECK (err && strstr (err->message, "9000x9000"));
    g_clear_error (&err);

    /* the file is replaced after the wallpaper was created: refused in
     * gl_init, by the size it declares now, before any GL call (none is
     * possible here) and before anything is decoded */
    char *pic = save_pic ("pic.png", 64, 48, "png");
    WalpeContent *c = walpe_image_new (pic, &err);
    CHECK (c != NULL && !err);
    if (c) {
        copy_over (many, pic);
        CHECK (!c->gl_init (c, &err));
        CHECK (err && strstr (err->message, "the picture is 9000x9000") &&
               strstr (err->message, "too large"));
        g_clear_error (&err);
        copy_over (wide, pic);
        CHECK (!c->gl_init (c, &err));
        CHECK (err && strstr (err->message, "the picture is 20000x4"));
        g_clear_error (&err);
        c->gl_free (c);
        c->free (c);
    }
    g_clear_error (&err);

    /* a project.json whose "file" leaves the wallpaper folder never gets
     * as far as a decoder (formats_test.c's input; content.c) */
    char *dir = P ("item"), *pj = P ("item/project.json"), *media = (char *) 1;
    g_mkdir_with_parents (dir, 0700);
    g_file_set_contents (pj, "{\"type\":\"video\",\"file\":\"../wide.png\"}", -1, NULL);
    walpe_detect_type (dir, &media);
    CHECK (media == NULL);
    g_free (dir);
    g_free (pj);
    g_free (pic);
    g_free (wide);
    g_free (many);
}

/* with a GL context: the good paths, and the refusal of a later decode */
static void gl_part (void)
{
    printf ("GL: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    walpe_glutil_init ();
    GLuint fbo = 0, col = 0;
    glGenTextures (1, &col);
    glBindTexture (GL_TEXTURE_2D, col);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col, 0);

    /* a 2400x1800 photo for an 800x600 view is stored at 800x600 */
    GError *err = NULL;
    char *photo = save_pic ("photo.jpg", 2400, 1800, "jpeg");
    WalpeContent *c = walpe_image_new (photo, &err);
    CHECK (c != NULL);
    if (c) {
        glViewport (0, 0, 800, 600);
        g_string_truncate (logged, 0);
        CHECK (c->gl_init (c, &err));
        CHECK (!err);
        CHECK (strstr (logged->str, "is 2400x1800, stored 800x600") != NULL);
        CHECK (c->gl_render (c, 800, 600, 0, 0));
        /* the file becomes too large; a bigger view asks for a new decode:
         * refused by the declared size, the stored picture stays */
        char *many = png_header ("many2.png", 9000, 9000);
        copy_over (many, photo);
        g_string_truncate (logged, 0);
        CHECK (c->gl_render (c, 2000, 1500, 0, 0));
        CHECK (strstr (logged->str, "the picture is 9000x9000") != NULL &&
               strstr (logged->str, "too large") != NULL);
        CHECK (strstr (logged->str, "stored") == NULL);
        g_free (many);
        c->gl_free (c);
        c->free (c);
    }
    g_clear_error (&err);
    g_free (photo);

    /* a GIF within the limits opens, for two monitors alike */
    char *ok = gif_canvas ("ok.gif", 320, 200);
    int w = 0, h = 0;
    if (gdk_pixbuf_get_file_info (ok, &w, &h)) {
        WalpeContent *a = walpe_gif_new (ok, &err), *b = walpe_gif_new (ok, &err);
        CHECK (a && b);
        if (a && b) {
            CHECK (a->gl_init (a, &err) && b->gl_init (b, &err));
            CHECK (a->gl_render (a, 640, 400, 0, 0) && b->gl_render (b, 640, 400, 0, 0));
            a->gl_free (a);
            b->gl_free (b);
        }
        if (a)
            a->free (a);
        if (b)
            b->free (b);
    }
    g_clear_error (&err);
    g_free (ok);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers (1, &fbo);
    glDeleteTextures (1, &col);
}

static gboolean gl_run (EGLDisplay dpy)
{
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1)
        return FALSE;
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext (dpy, ctx);
        eglTerminate (dpy);
        return FALSE;
    }
    gl_part ();
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return TRUE;
}

static void test_gl (void)
{
    gboolean ran = FALSE;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd))
            for (EGLint i = 0; i < nd && !ran; i++)
                ran = gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL));
    }
    if (!ran && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
        ran = gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL));
    if (!ran)
        printf ("limits-test: no EGL device / surfaceless GL 3.3 context, GL part skipped\n");
}

/* --------------------------------------------------- 3. MPRIS cover art */

/* one cover is at most the THUMB_MAX thumbnail: 640 x 640 RGBA, the same
 * picture as a data: URL (PNG of noise at worst: base64 of a little more
 * than the pixels) and its key */
#define COVER_MAX_BYTES ((gsize) 640 * 640 * 4 + (gsize) 640 * 640 * 4 * 3 / 2 + 8192)

static void test_art (void)
{
    int w = 0, h = 0;
    guint n = 99;
    gsize bytes = 0;
    walpe_media_art_probe (NULL, NULL, NULL, &n, &bytes);
    CHECK (n == 0 && bytes == 0);

    char *c1 = save_pic ("cover1.png", 1000, 800, "png");
    char *c2 = save_pic ("cover2.jpg", 3000, 3000, "jpeg");
    char *c3 = save_pic ("cover3.png", 200, 100, "png");
    char *u1 = g_filename_to_uri (c1, NULL, NULL);

    /* decoded, reduced to the thumbnail, cached once */
    CHECK (walpe_media_art_probe (u1, &w, &h, &n, &bytes));
    CHECK (w == 640 && h == 512 && n == 1);
    CHECK (bytes > 0 && bytes <= COVER_MAX_BYTES);
    g_string_truncate (logged, 0);
    CHECK (walpe_media_art_probe (u1, &w, &h, &n, &bytes));     /* the same cover again */
    CHECK (w == 640 && h == 512 && n == 1);
    CHECK (strstr (logged->str, "media: album art 640x512") == NULL);   /* not decoded again */

    /* never more than two, whatever is asked for (bare paths count too) */
    CHECK (walpe_media_art_probe (c2, &w, &h, &n, &bytes) && w == 640 && h == 640 && n == 2);
    CHECK (walpe_media_art_probe (c3, &w, &h, &n, &bytes) && w == 200 && h == 100 && n == 2);
    CHECK (walpe_media_art_probe (u1, &w, &h, &n, &bytes) && n == 2);
    CHECK (bytes <= 2 * COVER_MAX_BYTES);

    /* too large for cover art (more than 8192 per side): refused before a
     * pixel exists, nothing published, nothing cached */
    gsize before = bytes;
    char *wide = save_pic ("artwide.png", 20000, 4, "png");
    char *many = png_header ("artmany.png", 9000, 9000);
    g_string_truncate (logged, 0);
    CHECK (!walpe_media_art_probe (wide, &w, &h, &n, &bytes));
    CHECK (w == 0 && h == 0 && n == 2 && bytes == before);
    CHECK (strstr (logged->str, "20000x4") != NULL);
    CHECK (!walpe_media_art_probe (many, &w, &h, &n, &bytes));
    CHECK (n == 2 && bytes == before);

    /* the path is a D-Bus peer's: not a regular file = refused at once
     * (opening a FIFO would block for good) */
    char *fifo = P ("cover.fifo"), *dir = P ("coverdir");
    g_mkdir_with_parents (dir, 0700);
    if (mkfifo (fifo, 0600) == 0) {
        gint64 t0 = g_get_monotonic_time ();
        CHECK (!walpe_media_art_probe (fifo, NULL, NULL, &n, &bytes));
        CHECK (g_get_monotonic_time () - t0 < 5 * G_USEC_PER_SEC);
        CHECK (n == 2 && bytes == before);
    }
    CHECK (!walpe_media_art_probe (dir, NULL, NULL, &n, &bytes));
    CHECK (!walpe_media_art_probe ("/nonexistent/cover.png", NULL, NULL, &n, &bytes));
    CHECK (n == 2 && bytes == before);

    /* a data: URL is its own payload: shown, not cached */
    char *raw = NULL;
    gsize len = 0;
    if (g_file_get_contents (c3, &raw, &len, NULL)) {
        char *b64 = g_base64_encode ((const guchar *) raw, len);
        char *du = g_strconcat ("data:image/png;base64,", b64, NULL);
        CHECK (walpe_media_art_probe (du, &w, &h, &n, &bytes) && w == 200 && h == 100);
        CHECK (n == 2 && bytes == before);
        g_free (du);
        g_free (b64);
        g_free (raw);
    }

    /* a cover whose file changed is not served from the cache */
    copy_over (c3, c1);
    struct stat st;
    CHECK (g_stat (c1, &st) == 0);
    CHECK (walpe_media_art_probe (u1, &w, &h, &n, &bytes) && w == 200 && h == 100 && n == 2);

    walpe_media_art_probe (NULL, NULL, NULL, &n, &bytes);
    CHECK (n == 0 && bytes == 0);
    g_free (c1);
    g_free (c2);
    g_free (c3);
    g_free (u1);
    g_free (wide);
    g_free (many);
    g_free (fifo);
    g_free (dir);
}

int main (void)
{
    root = g_dir_make_tmp ("walpe-limits-XXXXXX", NULL);
    g_setenv ("HOME", root, TRUE);
    g_setenv ("XDG_CONFIG_HOME", root, TRUE);
    g_setenv ("XDG_CACHE_HOME", root, TRUE);
    g_setenv ("XDG_DATA_HOME", root, TRUE);
    logged = g_string_new (NULL);
    g_log_set_writer_func (log_writer, NULL, NULL);

    test_gif ();
    test_image ();
    test_art ();
    test_gl ();

    /* tidy: remove the temp tree */
    char *argv[] = { "rm", "-rf", "--", root, NULL };
    g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free (root);
    if (fails) {
        g_printerr ("limits-test: %d check(s) FAILED\n", fails);
        return 1;
    }
    g_print ("limits-test: all checks passed\n");
    return 0;
}
