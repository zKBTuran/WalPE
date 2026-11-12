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
 *     A file of another kind under a picture's name (an SVG document, XPM
 *     or PNM text, a Windows icon or cursor) is refused in the same three
 *     places, as a picture and as a GIF wallpaper.
 *  3. MPRIS cover art (media.c): the decode refuses what is too large or
 *     not a regular file, and the cache of decoded covers holds at most
 *     two thumbnails of bounded size, never the input.
 *     A cover named by an http URL is read up to 16 MiB as it comes out
 *     of the content decoder: a small "Content-Encoding: gzip" body that
 *     unpacks to hundreds of MiB is given up, not collected.
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
#include "walpe-build-config.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <errno.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef HAVE_SOUP
#include <libsoup/soup.h>
#endif

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

/* ------------------------------- 2b. a file that is no picture format */

/* files gdk-pixbuf has a decoder for that is none of a wallpaper's: text
 * ones as they are, a Windows icon of one 1 x 1 picture, and an animated
 * cursor around that icon */
static const guint8 ico_1x1[] = {
    0, 0, 1, 0, 1, 0,   1, 1, 0, 0, 1, 0, 32, 0, 48, 0, 0, 0, 22, 0, 0, 0,
    40, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 32, 0, 0, 0, 0, 0, 4, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0xff, 0xff,   0, 0, 0, 0,
};

static void put_le32 (GByteArray *a, guint32 v)
{
    const guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
    g_byte_array_append (a, b, 4);
}

static GByteArray *ani_of_ico (void)
{
    GByteArray *a = g_byte_array_new ();
    const guint32 anih[9] = { 36, 1, 1, 0, 0, 0, 0, 10, 1 };
    g_byte_array_append (a, (const guint8 *) "RIFF", 4);
    put_le32 (a, 4 + 8 + 36 + 8 + 4 + 8 + sizeof ico_1x1);
    g_byte_array_append (a, (const guint8 *) "ACONanih", 8);
    put_le32 (a, 36);
    for (int i = 0; i < 9; i++)
        put_le32 (a, anih[i]);
    g_byte_array_append (a, (const guint8 *) "LIST", 4);
    put_le32 (a, 4 + 8 + sizeof ico_1x1);
    g_byte_array_append (a, (const guint8 *) "framicon", 8);
    put_le32 (a, sizeof ico_1x1);
    g_byte_array_append (a, ico_1x1, sizeof ico_1x1);
    return a;
}

static void test_other_kinds (void)
{
    static const char svg[] = "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" "
                              "height=\"36\"><rect width=\"64\" height=\"36\" fill=\"red\"/></svg>\n";
    static const char xpm[] = "/* XPM */\nstatic char *x[] = {\n\"2 2 1 1\",\n\". c #FF0000\",\n\"..\",\n\"..\"\n};\n";
    static const char pnm[] = "P3\n2 2\n255\n255 0 0 255 0 0 255 0 0 255 0 0\n";
    GByteArray *ani = ani_of_ico ();
    const struct { const char *kind; const guint8 *d; gsize n; } other[] = {
        { "svg", (const guint8 *) svg, sizeof svg - 1 },
        { "xpm", (const guint8 *) xpm, sizeof xpm - 1 },
        { "pnm", (const guint8 *) pnm, sizeof pnm - 1 },
        { "ico", ico_1x1, sizeof ico_1x1 },
        { "ani", ani->data, ani->len },
    };
    const char *const not_one = "not a picture format a wallpaper can be";
    char *good = save_pic ("kind-good.png", 64, 48, "png");
    int tried = 0;
    for (guint i = 0; i < G_N_ELEMENTS (other); i++) {
        char *png = P ("kind.png"), *gif = P ("kind.gif"), *pic = P ("kind-pic.png");
        CHECK (g_file_set_contents (png, (const char *) other[i].d, (gssize) other[i].n, NULL));
        CHECK (g_file_set_contents (gif, (const char *) other[i].d, (gssize) other[i].n, NULL));
        int w = 0, h = 0;
        GdkPixbufFormat *fmt = gdk_pixbuf_get_file_info (png, &w, &h);
        char *name = fmt ? gdk_pixbuf_format_get_name (fmt) : NULL;
        if (g_strcmp0 (name, other[i].kind)) {
            /* this gdk-pixbuf has no decoder for it: nothing to refuse */
            g_print ("limits-test: no %s loader, that kind is not tried\n", other[i].kind);
        } else {
            GError *err = NULL;
            tried++;
            /* as a picture wallpaper: when it is created */
            CHECK (walpe_image_new (png, &err) == NULL);
            CHECK (err && strstr (err->message, not_one) && strstr (err->message, other[i].kind));
            g_clear_error (&err);
            /* a picture that became such a file afterwards: in gl_init,
             * before any GL call (none is possible here) */
            copy_over (good, pic);
            WalpeContent *c = walpe_image_new (pic, &err);
            CHECK (c != NULL && !err);
            if (c) {
                copy_over (png, pic);
                CHECK (!c->gl_init (c, &err));
                CHECK (err && strstr (err->message, not_one));
                g_clear_error (&err);
                c->gl_free (c);
                c->free (c);
            }
            g_clear_error (&err);
            /* as a GIF wallpaper: when the shared source is opened */
            c = walpe_gif_new (gif, &err);
            CHECK (c != NULL);
            if (c) {
                CHECK (!c->gl_init (c, &err));
                CHECK (err && strstr (err->message, not_one));
                g_clear_error (&err);
                c->gl_free (c);
                c->free (c);
            }
            g_clear_error (&err);
        }
        g_free (name);
        g_unlink (png);
        g_unlink (gif);
        g_unlink (pic);
        g_free (png);
        g_free (gif);
        g_free (pic);
    }
    /* (every system this is built for has a loader for one of them at least) */
    CHECK (tried > 0);
    /* the formats a wallpaper can be are not refused: by this check, a
     * picture of each kind this gdk-pixbuf writes */
    static const char *const kinds[] = { "png", "jpeg", "bmp", "tiff" };
    for (guint i = 0; i < G_N_ELEMENTS (kinds); i++) {
        char *n = g_strdup_printf ("kind-ok.%s", kinds[i]);
        GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 8, 6);
        gdk_pixbuf_fill (pb, 0x102030ff);
        char *f = P (n);
        GError *err = NULL;
        if (gdk_pixbuf_save (pb, f, kinds[i], NULL, NULL)) {
            CHECK (walpe_picture_size_ok (f, &err) && !err);
            g_clear_error (&err);
        }
        g_object_unref (pb);
        g_unlink (f);
        g_free (f);
        g_free (n);
    }
    g_byte_array_unref (ani);
    g_free (good);
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

/* A JPEG of w x h written by hand. kind 0: progressive, one AC scan per
 * component in which every block lies inside an end-of-band run (15 zero
 * bits cover 16384 blocks) - a few hundred bytes that a decoder has to
 * hold whole at full size. kind 1: sequential with a scan per component
 * (held whole as well). kind 2: sequential, one scan for all components
 * (decoded as it is read). sub: the first component sampled 2 x 2. */
static GByteArray *hand_jpeg (guint w, guint h, guint ncomp, int kind, gboolean sub)
{
    GByteArray *o = g_byte_array_new ();
    #define SEG(m, body, n) do { guint8 hd_[4] = { 0xFF, (m), (guint8) (((n) + 2) >> 8), (guint8) ((n) + 2) }; \
                                 g_byte_array_append (o, hd_, 4); g_byte_array_append (o, (body), (n)); } while (0)
    g_byte_array_append (o, (const guint8 *) "\xFF\xD8", 2);
    guint8 dqt[65];
    memset (dqt, 1, sizeof dqt);
    dqt[0] = 0;
    SEG (0xDB, dqt, 65);
    guint8 sof[6 + 3 * 4] = { 8, (guint8) (h >> 8), (guint8) h, (guint8) (w >> 8), (guint8) w, (guint8) ncomp };
    for (guint c = 0; c < ncomp; c++) {
        sof[6 + 3 * c] = (guint8) (c + 1);
        sof[7 + 3 * c] = sub && c == 0 ? 0x22 : 0x11;
        sof[8 + 3 * c] = 0;
    }
    SEG (kind == 0 ? 0xC2 : 0xC0, sof, 6 + 3 * ncomp);
    /* AC table 0: one code, '0', for symbol 0xE0 (an end-of-band run of
     * 2^14 blocks plus 14 more bits); DC table 0: one code for "no change" */
    guint8 dht[18] = { 0x10, 1 };
    dht[17] = kind == 0 ? 0xE0 : 0x00;
    SEG (0xC4, dht, 18);
    dht[0] = 0x00;
    dht[17] = 0x00;
    SEG (0xC4, dht, 18);
    if (kind == 2) {
        guint8 sos[1 + 2 * 4 + 3] = { (guint8) ncomp };
        for (guint c = 0; c < ncomp; c++)
            sos[1 + 2 * c] = (guint8) (c + 1);
        sos[1 + 2 * ncomp + 1] = 63;
        SEG (0xDA, sos, 1 + 2 * ncomp + 3);
    } else {
        guint64 blocks = (guint64) ((w + 7) / 8) * ((h + 7) / 8) * (sub ? 4 : 1);
        guint zeros = (guint) (((blocks + 16383) / 16384 * 15 + 7) / 8 + 2);
        guint8 *z = g_malloc0 (zeros);
        for (guint c = 0; c < ncomp; c++) {
            guint8 sos[6] = { 1, (guint8) (c + 1), 0, (guint8) (kind == 0 ? 1 : 0), 63, 0 };
            SEG (0xDA, sos, 6);
            if (kind == 0)
                g_byte_array_append (o, z, zeros);
        }
        g_free (z);
    }
    g_byte_array_append (o, (const guint8 *) "\xFF\xD9", 2);
    #undef SEG
    return o;
}

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

    /* the cover file is cut short while it is decoded (a player writes the
     * next cover over the same name): the picture may be shown or not, the
     * process lives. This program, started again, is the one that decodes:
     * a decoder reading a mapping of the file died there with SIGBUS. */
    {
        char *cut = save_pic ("covercut.png", 5000, 5000, "png");
        char *self = g_file_read_link ("/proc/self/exe", NULL);
        char *cargv[] = { self, (char *) "--art-cut", cut, NULL };
        int st = -1;
        gboolean ran = self && g_spawn_sync (NULL, cargv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL,
                                             NULL, &st, NULL);
        CHECK (ran);
        if (ran && WIFSIGNALED (st))
            g_printerr ("limits-test: the decode of a cover file that was cut short ended with signal %d\n",
                        WTERMSIG (st));
        CHECK (ran && WIFEXITED (st) && WEXITSTATUS (st) == 0);
        g_free (self);
        g_free (cut);
    }

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

    /* A JPEG is decoded at about the size shown only when it has one
     * scan. A progressive one is held whole first by its decoder: a few
     * hundred bytes declaring 8192 x 8192 in three components would cost
     * 384 MiB. Refused; a small progressive cover is shown. */
    {
        GByteArray *pj = hand_jpeg (8192, 8192, 3, 0, FALSE);
        CHECK (pj->len < 2000);
        char *b64 = g_base64_encode (pj->data, pj->len);
        char *du = g_strconcat ("data:image/jpeg;base64,", b64, NULL);
        g_string_truncate (logged, 0);
        CHECK (!walpe_media_art_probe (du, &w, &h, &n, &bytes));
        CHECK (strstr (logged->str, "held whole") != NULL);
        CHECK (n == 2 && bytes == before);
        g_free (du);
        g_free (b64);
        g_byte_array_unref (pj);
        pj = hand_jpeg (64, 48, 1, 0, FALSE);
        b64 = g_base64_encode (pj->data, pj->len);
        du = g_strconcat ("data:image/jpeg;base64,", b64, NULL);
        CHECK (walpe_media_art_probe (du, &w, &h, &n, &bytes) && w == 64 && h == 48);
        g_free (du);
        g_free (b64);
        g_byte_array_unref (pj);
    }

    /* cover art in a format that is not a cover's (the decoder is chosen
     * by the first bytes, whatever the name or the data: URL says) is not
     * decoded: SVG, XPM, PNM */
    {
        static const char *const other[] = {
            "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" "
            "height=\"36\"><rect width=\"64\" height=\"36\" fill=\"red\"/></svg>\n",
            "/* XPM */\nstatic char *x[] = {\n\"2 2 1 1\",\n\". c #FF0000\",\n\"..\",\n\"..\"\n};\n",
            "P3\n2 2\n255\n255 0 0 255 0 0 255 0 0 255 0 0\n",
        };
        for (guint i = 0; i < G_N_ELEMENTS (other); i++) {
            char *f = P ("cover-other.png");
            CHECK (g_file_set_contents (f, other[i], -1, NULL));
            CHECK (!walpe_media_art_probe (f, &w, &h, &n, &bytes));
            char *b64 = g_base64_encode ((const guchar *) other[i], strlen (other[i]));
            char *du = g_strconcat ("data:image/png;base64,", b64, NULL);
            CHECK (!walpe_media_art_probe (du, &w, &h, &n, &bytes));
            g_free (du);
            g_free (b64);
            g_unlink (f);
            g_free (f);
        }
        /* ... a Windows icon, an animated cursor (RIFF, as WebP is, but
         * not WEBP), a TIFF and a TGA: formats a wallpaper's own pictures
         * may have and a cover may not; and no bytes at all */
        GByteArray *ani = ani_of_ico ();
        char *tiff = save_pic ("cover-other.tiff", 8, 6, "tiff");
        char *tga = P ("cover-other.tga");
        static const guint8 tga_2x2[] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0,
                                          0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255 };
        char *ico = P ("cover-other.ico"), *cur = P ("cover-other.ani"), *none = P ("cover-none.png");
        CHECK (g_file_set_contents (tga, (const char *) tga_2x2, sizeof tga_2x2, NULL));
        CHECK (g_file_set_contents (ico, (const char *) ico_1x1, sizeof ico_1x1, NULL));
        CHECK (g_file_set_contents (cur, (const char *) ani->data, (gssize) ani->len, NULL));
        CHECK (g_file_set_contents (none, "", 0, NULL));
        CHECK (!walpe_media_art_probe (tiff, &w, &h, &n, &bytes));
        CHECK (!walpe_media_art_probe (tga, &w, &h, &n, &bytes));
        CHECK (!walpe_media_art_probe (ico, &w, &h, &n, &bytes));
        CHECK (!walpe_media_art_probe (cur, &w, &h, &n, &bytes));
        CHECK (!walpe_media_art_probe (none, &w, &h, &n, &bytes));
        g_byte_array_unref (ani);
        g_unlink (tiff); g_unlink (tga); g_unlink (ico); g_unlink (cur); g_unlink (none);
        g_free (tiff); g_free (tga); g_free (ico); g_free (cur); g_free (none);
    }

    /* a cover whose file changed is not served from the cache */
    copy_over (c3, c1);
    struct stat st;
    CHECK (g_stat (c1, &st) == 0);
    CHECK (walpe_media_art_probe (u1, &w, &h, &n, &bytes) && w == 200 && h == 100 && n == 2);

    walpe_media_art_probe (NULL, NULL, NULL, &n, &bytes);
    CHECK (n == 0 && bytes == 0);

    /* a cover asked for again while it is the one shown comes from the
     * cache and is published a second time: the published state holds one
     * reference to its pixels, not one per request. When three other
     * covers have pushed it out of the published pair (thumbnail and
     * previous thumbnail) and out of the cache, the reference taken here
     * is the last one: GBytes then hands out the buffer itself, not a copy */
    {
        char *c4 = save_pic ("cover4.png", 320, 200, "png");
        static const char *more[3] = { "cover5.png", "cover6.png", "cover7.png" };
        CHECK (walpe_media_art_probe (c4, &w, &h, &n, &bytes) && w == 320 && h == 200);
        CHECK (walpe_media_art_probe (c4, &w, &h, &n, &bytes) && w == 320 && h == 200);
        WalpeNowPlaying np;
        walpe_media_get (&np);
        GBytes *pix = np.thumb ? g_bytes_ref (np.thumb) : NULL;
        walpe_media_now_clear (&np);
        CHECK (pix != NULL);
        for (int i = 0; i < 3; i++) {
            char *c = save_pic (more[i], 48 + 16 * i, 48, "png");
            CHECK (walpe_media_art_probe (c, &w, &h, &n, &bytes) && w == 48 + 16 * i);
            g_free (c);
        }
        walpe_media_art_probe (NULL, NULL, NULL, &n, &bytes);
        /* The job of a decode holds the picture too, until the worker
         * thread that ran it has let go of its task; that can be after the
         * probe returned (it failed here on a busy machine: the first
         * decode's job was still there, and the buffer came back as a
         * copy). Wait for the jobs to be gone - what is checked below is
         * what the published state and the cache hold. */
        if (!walpe_media_art_settled (20000)) {
            g_printerr ("FAIL %s:%d: a cover decode's job still exists 20 s after its result\n",
                        __FILE__, __LINE__);
            fails++;
        }
        if (pix) {
            gsize len = 0;
            gconstpointer was = g_bytes_get_data (pix, &len);
            gpointer got = g_bytes_unref_to_data (pix, &len);
            CHECK (got == was && len == 320 * 200 * 4);
            g_free (got);
        }
        g_free (c4);
    }
    g_free (c1);
    g_free (c2);
    g_free (c3);
    g_free (u1);
    g_free (wide);
    g_free (many);
    g_free (fifo);
    g_free (dir);
}

#ifdef HAVE_SOUP
/* ---- cover art over http: a server on loopback that answers with a small
 * PNG, the same PNG gzip-packed, a gzip body of ART_PACKED_PLAIN zero
 * bytes, and a plain body that is longer than cover art may be */
#define ART_PACKED_PLAIN ((gsize) 256 << 20)
#define ART_LONG_PLAIN ((gsize) 20 << 20)

static struct { GBytes *png, *png_gz, *zeros_gz, *long_body; } ArtSrv;

static GBytes *gzip_of (const guint8 *d, gsize len, gsize zeros_after)
{
    static const guint8 zero[65536];
    GZlibCompressor *z = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, 6);
    GOutputStream *mem = g_memory_output_stream_new_resizable ();
    GOutputStream *out = g_converter_output_stream_new (mem, G_CONVERTER (z));
    gboolean ok = len == 0 || g_output_stream_write_all (out, d, len, NULL, NULL, NULL);
    for (gsize left = zeros_after; ok && left > 0; left -= MIN (left, sizeof zero))
        ok = g_output_stream_write_all (out, zero, MIN (left, sizeof zero), NULL, NULL, NULL);
    ok = g_output_stream_close (out, NULL, NULL) && ok;
    GBytes *b = ok ? g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem)) : NULL;
    g_object_unref (out);
    g_object_unref (mem);
    g_object_unref (z);
    return b;
}

static void art_server (SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *query,
                        gpointer user)
{
    (void) srv; (void) query; (void) user;
    SoupMessageHeaders *h = soup_server_message_get_response_headers (msg);
    GBytes *body = NULL;
    gboolean packed = FALSE;
    if (!strcmp (path, "/cover.png")) {
        body = ArtSrv.png;
    } else if (!strcmp (path, "/cover-packed.png")) {
        body = ArtSrv.png_gz;
        packed = TRUE;
    } else if (!strcmp (path, "/zeros-packed.png")) {
        body = ArtSrv.zeros_gz;
        packed = TRUE;
    } else if (!strcmp (path, "/long.png")) {
        body = ArtSrv.long_body;
        /* no Content-Length: the length is found out by reading */
        soup_message_headers_set_encoding (h, SOUP_ENCODING_CHUNKED);
    }
    if (!body) {
        soup_server_message_set_status (msg, 404, NULL);
        return;
    }
    soup_server_message_set_status (msg, 200, NULL);
    soup_message_headers_replace (h, "Content-Type", "image/png");
    if (packed)
        soup_message_headers_replace (h, "Content-Encoding", "gzip");
    soup_message_body_append_bytes (soup_server_message_get_response_body (msg), body);
}

static long max_rss_kb (void)
{
    struct rusage ru;
    getrusage (RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

static void test_art_http (void)
{
    SoupServer *srv = soup_server_new (NULL, NULL);
    soup_server_add_handler (srv, NULL, art_server, NULL, NULL);
    GSList *uris = soup_server_listen_local (srv, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL)
                   ? soup_server_get_uris (srv) : NULL;
    if (!uris) {
        g_print ("limits-test: no loopback listener here: cover art over http not exercised\n");
        g_object_unref (srv);
        return;
    }
    char *base = g_uri_to_string (uris->data);
    g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
    if (g_str_has_suffix (base, "/"))
        base[strlen (base) - 1] = 0;

    char *file = save_pic ("httpcover.png", 200, 100, "png");
    char *raw = NULL;
    gsize len = 0;
    CHECK (g_file_get_contents (file, &raw, &len, NULL));
    ArtSrv.png = g_bytes_new_take (raw, len);
    ArtSrv.png_gz = gzip_of ((const guint8 *) raw, len, 0);
    ArtSrv.zeros_gz = gzip_of (NULL, 0, ART_PACKED_PLAIN);
    ArtSrv.long_body = g_bytes_new_take (g_malloc0 (ART_LONG_PLAIN), ART_LONG_PLAIN);
    CHECK (ArtSrv.png_gz && ArtSrv.zeros_gz && g_bytes_get_size (ArtSrv.zeros_gz) < 1024 * 1024);

    int w = 0, h = 0;
    guint n = 0;
    gsize bytes = 0;
    char *u = g_strconcat (base, "/cover.png", NULL);
    CHECK (walpe_media_art_probe (u, &w, &h, &n, &bytes) && w == 200 && h == 100);
    g_free (u);
    /* packed on the way: unpacked by the client, decoded */
    u = g_strconcat (base, "/cover-packed.png", NULL);
    CHECK (walpe_media_art_probe (u, &w, &h, &n, &bytes) && w == 200 && h == 100);
    g_free (u);
    /* a quarter of a MiB on the wire, 256 MiB once unpacked: the limit is
     * on what comes out of the unpacking, while it arrives */
    long rss0 = max_rss_kb ();
    u = g_strconcat (base, "/zeros-packed.png", NULL);
    CHECK (!walpe_media_art_probe (u, &w, &h, &n, &bytes));
    long grew = max_rss_kb () - rss0;
    CHECK (grew < 128 * 1024);
    g_print ("limits-test: packed cover of %" G_GSIZE_FORMAT " bytes (256 MiB unpacked): peak RSS "
             "grew by %.1f MiB\n", g_bytes_get_size (ArtSrv.zeros_gz), (double) grew / 1024.0);
    g_free (u);
    /* a plain body beyond the limit, its length not announced */
    u = g_strconcat (base, "/long.png", NULL);
    CHECK (!walpe_media_art_probe (u, &w, &h, &n, &bytes));
    g_free (u);
    u = g_strconcat (base, "/none.png", NULL);
    CHECK (!walpe_media_art_probe (u, &w, &h, &n, &bytes));
    g_free (u);
    /* and the next cover is shown as before */
    u = g_strconcat (base, "/cover.png", NULL);
    CHECK (walpe_media_art_probe (u, &w, &h, &n, &bytes) && w == 200 && h == 100);
    g_free (u);

    walpe_media_art_probe (NULL, NULL, NULL, &n, &bytes);
    soup_server_disconnect (srv);
    g_object_unref (srv);
    g_bytes_unref (ArtSrv.png);
    g_bytes_unref (ArtSrv.png_gz);
    g_bytes_unref (ArtSrv.zeros_gz);
    g_bytes_unref (ArtSrv.long_body);
    g_free (file);
    g_free (base);
}
#endif /* HAVE_SOUP */

/* limits-test --art-cut <file>: the cover <file> is decoded here, and cut
 * to nothing while that runs: as soon as this process has the file mapped
 * (it does not, any more: then a moment after the decode began) */
static gpointer art_cut_thread (gpointer path)
{
    for (int i = 0; i < 300; i++) {
        char *maps = NULL;
        gboolean mapped = g_file_get_contents ("/proc/self/maps", &maps, NULL, NULL) && strstr (maps, path);
        g_free (maps);
        if (mapped)
            break;
        g_usleep (500);
    }
    if (truncate (path, 0) != 0)
        g_printerr ("limits-test: truncate %s: %s\n", (const char *) path, g_strerror (errno));
    return NULL;
}

static int art_cut_main (const char *path)
{
    GThread *t = g_thread_new ("cut", art_cut_thread, (gpointer) path);
    walpe_media_art_probe (path, NULL, NULL, NULL, NULL);    /* shown or not: both are fine */
    g_thread_join (t);
    walpe_media_art_settled (5000);
    return 0;
}

int main (int argc, char **args)
{
    if (argc == 3 && !strcmp (args[1], "--art-cut"))
        return art_cut_main (args[2]);
    root = g_dir_make_tmp ("walpe-limits-XXXXXX", NULL);
    g_setenv ("HOME", root, TRUE);
    g_setenv ("XDG_CONFIG_HOME", root, TRUE);
    g_setenv ("XDG_CACHE_HOME", root, TRUE);
    g_setenv ("XDG_DATA_HOME", root, TRUE);
    logged = g_string_new (NULL);
    g_log_set_writer_func (log_writer, NULL, NULL);

#ifdef HAVE_SOUP
    /* the server is on loopback: no proxy in between. First of all: the
     * peak of memory use is measured, and nothing has raised it yet */
    const char *px[] = { "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy",
                         "ALL_PROXY", NULL };
    for (int i = 0; px[i]; i++)
        g_unsetenv (px[i]);
    g_setenv ("no_proxy", "127.0.0.1,localhost", TRUE);
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
    test_art_http ();
#endif
    test_gif ();
    test_image ();
    test_other_kinds ();
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
