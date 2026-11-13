/* image.c - still-image wallpaper.
 *
 * The picture is decoded once, at the size it is shown: an 8K photo on a
 * 1080p monitor is stored as a 1920x1080 texture (8 MiB) instead of
 * 7680x4320 plus a mip chain (169 MiB per monitor), and the JPEG decoder
 * does most of the reduction itself (DCT scaling), so the full-size
 * pixels never exist. Monitors of one EGL share group that show the same
 * picture at the same size use one texture. No CPU copy is kept; after
 * the first frame nothing is drawn again (no frame clock). If the
 * alignment option later needs more pixels (or far fewer), the picture is
 * decoded again at that size.
 *
 * The size limits of image.h (16384 pixels per side, 64 megapixels) are on
 * the size the FILE declares, never on the reduced size it is stored at:
 * only JPEG is reduced while it decodes, PNG and the others are decoded in
 * full first and scaled afterwards, so a few KB of PNG declaring a
 * gigabyte of pixels would cost that gigabyte whatever size is asked for.
 * They are checked before every decode (image_tex_load), also the later
 * ones at another size, and in gl_init before anything else. */
#include "image.h"
#include "fit.h"
#include "glutil.h"
#include "common/gifcheck.h"
#include "common/premult.h"
#include "common/util.h"
#include "common/lazy.h"

/* one decoded picture in GL: shared between the monitors' contexts when
 * they form a share group (key: path + stored size), else one per content */
typedef struct {
    char *key;
    GLuint tex;
    int refs;
    int tex_w, tex_h;       /* stored size */
    int img_w, img_h;       /* the picture's own size (placement) */
    gboolean premult;
} ImageTex;

static GMutex it_lock;
static GHashTable *it_table;    /* key -> ImageTex (share group only) */

typedef struct {
    WalpeContent base;
    ImageTex *it;
    int img_w, img_h;       /* the picture's own size, after EXIF rotation */
    gboolean scalable;      /* a gdk-pixbuf format: can be decoded smaller */
    WalpeFit fit;           /* alignment option (fit.h) */
} ImageContent;

/* GL texture from a pixbuf we own, straight from its pixels (no copy):
 * RGB rows as they are, RGBA premultiplied in place. No mip chain: the
 * texture is stored at about the size it is drawn. */
static GLuint image_upload (GdkPixbuf *pb, gboolean *premult)
{
    int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
    int rs = gdk_pixbuf_get_rowstride (pb), nc = gdk_pixbuf_get_n_channels (pb);
    gboolean alpha = gdk_pixbuf_get_has_alpha (pb);
    if (gdk_pixbuf_get_bits_per_sample (pb) != 8 || (alpha ? nc != 4 || rs != w * 4
                                                           : nc != 3 || rs != ((w * 3 + 3) & ~3)))
        return walpe_gl_tex_from_pixbuf (g_object_ref (pb));    /* unusual layout: the copying path */
    guint8 *px = gdk_pixbuf_get_pixels (pb);
    if (alpha)
        walpe_premultiply_rgba8 (px, (gsize) w * h * 4);
    GLuint tex = 0;
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);     /* gdk-pixbuf pads rows to 4 bytes */
    if (alpha)
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    else
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    *premult = TRUE;
    return tex;
}

/* a picture of w x h pixels (as its file declares) is within the limits
 * of image.h; else FALSE with err set */
static gboolean picture_dims_ok (int w, int h, GError **err)
{
    if (w > WALPE_PICTURE_MAX_SIDE || h > WALPE_PICTURE_MAX_SIDE ||
        (gint64) w * h > WALPE_PICTURE_MAX_PIXELS) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "the picture is %dx%d: too large to show (limit %d per side, %d megapixels)",
                     w, h, WALPE_PICTURE_MAX_SIDE, WALPE_PICTURE_MAX_PIXELS / 1000000);
        return FALSE;
    }
    return TRUE;
}

/* a "BM" file goes to gdk-pixbuf (also for its size alone) only with the
 * header sizes of a BMP file (util.h, walpe_bmp_header_ok), and a JPEG -
 * by its bytes, whatever the file is called - only when it is not
 * arithmetic-coded (walpe_jpeg_coding_ok) */
static gboolean picture_bmp_ok (const char *path, GError **err)
{
    if (!walpe_jpeg_file_coding_ok (path)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s: " WALPE_JPEG_ARITH_MSG, path);
        return FALSE;
    }
    if (walpe_bmp_file_header_ok (path))
        return TRUE;
    g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "%s: a BMP whose header sizes are not those of a BMP file", path);
    return FALSE;
}

/* What the file at path is, as WalPE reads it from the file's own first
 * bytes (common/pichead.h): the format and the declared size. gdk-pixbuf
 * used to be asked (gdk_pixbuf_get_file_info); it answers after the loader
 * it picked has read the file, and where it decodes through glycin that
 * is after the whole picture has been decoded - three times before a
 * picture wallpaper was on screen - whatever the bytes are.
 *   1: a picture of one of the formats a wallpaper can be (pf filled);
 *   0: none of them, or one without a size in its header: the texture
 *      loader's to take (.tex) or to refuse; pf->kind is set when the
 *      bytes were a format's all the same;
 *  -1: refused, err set (a BMP or JPEG of picture_bmp_ok, a file of
 *      another picture format, a size beyond the limits). */
typedef struct {
    const char *kind;
    WalpeTga tga;
    int w, h;
} PictureFile;

static int picture_file (const char *path, PictureFile *pf, GError **err)
{
    memset (pf, 0, sizeof *pf);
    if (!picture_bmp_ok (path, err))
        return -1;
    pf->kind = walpe_pichead_file_kind (path, &pf->tga);
    if (!pf->kind) {
        const char *other = walpe_pichead_file_other (path);
        if (!other)
            return 0;
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "%s is a %s file, not a picture format a wallpaper can be", path, other);
        return -1;
    }
    gboolean sized = walpe_pichead_file_size (pf->kind, path, &pf->w, &pf->h);
    if (!walpe_pichead_exact (pf->kind)) {
        /* TIFF, AVIF, JPEG XL: the header's size is an upper bound (past the
         * limits: refused before a decoder runs), the loader's the size */
        if (sized && !picture_dims_ok (pf->w, pf->h, err))
            return -1;
        sized = walpe_gl_picture_file_size (path, pf->kind, &pf->w, &pf->h);
    }
    if (!sized || pf->w < 1 || pf->h < 1)
        return 0;
    return picture_dims_ok (pf->w, pf->h, err) ? 1 : -1;
}

/* the size gdk_pixbuf_new_from_file_at_scale (path, side, side, TRUE) asks
 * its loader for, for a picture of w x h */
static void picture_at_scale (int w, int h, int side, int *tw, int *th)
{
    if ((double) h * (double) side > (double) w * (double) side) {
        *tw = (int) (0.5 + (double) w * (double) side / (double) h);
        *th = side;
    } else {
        *th = (int) (0.5 + (double) h * (double) side / (double) w);
        *tw = side;
    }
    *tw = MAX (*tw, 1);
    *th = MAX (*th, 1);
}

/* decode path so that its longer side is `side` pixels (0 = its own size)
 * and upload it. The longer side, not width x height: a photo turned by
 * its EXIF orientation has them the other way round in the file. */
static ImageTex *image_tex_load (const char *path, int side, GError **error)
{
    /* the declared size, not `side`: see the top of the file. Checked
     * here, before each decode - the file may have been replaced since
     * walpe_image_new looked at it. The format likewise. */
    PictureFile pf;
    const int is = picture_file (path, &pf, error);
    if (is < 0)
        return NULL;
    const gboolean pixbuf_format = is > 0;
    const int fw = pf.w, fh = pf.h;
    /* a GIF (its first frame is the picture): only with every frame inside
     * the picture its header declares (gifcheck.h); the limit above is on
     * that picture alone */
    if (pf.kind && !strcmp (pf.kind, "gif") && !walpe_gif_file_layout_ok (path, error))
        return NULL;
    ImageTex *it = g_new0 (ImageTex, 1);
    if (!pixbuf_format) {
        /* not a gdk-pixbuf format (.tex ...): its own loader, full size
         * (with its own limits: walpe_gl_tex_from_file) */
        WalpeGlTex t = walpe_gl_tex_from_file (path);
        if (!t.tex) {
            g_free (it);
            return NULL;
        }
        it->tex = t.tex;
        it->premult = t.premult;
        it->tex_w = it->img_w = t.width;
        it->tex_h = it->img_h = t.height;
        return it;
    }
    GError *err = NULL;
    GdkPixbuf *pb;
    if (!strcmp (pf.kind, "tga")) {
        /* by the TGA loader's name and with the header walpe_tga_head made:
         * gdk-pixbuf left to choose takes some TGA headers for a Windows
         * cursor's, glycin others for no picture */
        int tw = 0, th = 0;
        if (side > 0 && side < MAX (fw, fh))
            picture_at_scale (fw, fh, side, &tw, &th);
        pb = walpe_gl_picture_file_decode (path, pf.kind, &pf.tga, tw, th, NULL, &err);
    } else {
        /* (the bytes are that format's by its signature: the loader
         * gdk-pixbuf picks from them is that format's) */
        pb = side > 0 && side < MAX (fw, fh)
            ? gdk_pixbuf_new_from_file_at_scale (path, side, side, TRUE, &err)
            : gdk_pixbuf_new_from_file (path, &err);
    }
    if (!pb) {
        g_debug ("image %s: %s", path, err ? err->message : "?");
        g_clear_error (&err);
        g_free (it);
        return NULL;
    }
    int lw = gdk_pixbuf_get_width (pb), lh = gdk_pixbuf_get_height (pb);
    /* and what was really decoded (a file replaced between the look at
     * its header and the decode is not uploaded) */
    if (!picture_dims_ok (lw, lh, NULL)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "the picture changed while it was opened and is %dx%d now: too large to show",
                     lw, lh);
        g_object_unref (pb);
        g_free (it);
        return NULL;
    }
    GdkPixbuf *rot = gdk_pixbuf_apply_embedded_orientation (pb);   /* EXIF */
    gboolean turned = FALSE;
    if (rot) {
        turned = gdk_pixbuf_get_width (rot) != lw || gdk_pixbuf_get_height (rot) != lh;
        g_object_unref (pb);
        pb = rot;
    }
    it->tex_w = gdk_pixbuf_get_width (pb);
    it->tex_h = gdk_pixbuf_get_height (pb);
    it->img_w = turned ? fh : fw;
    it->img_h = turned ? fw : fh;
    it->tex = image_upload (pb, &it->premult);
    g_object_unref (pb);
    if (!it->tex) {
        g_free (it);
        return NULL;
    }
    if (it->tex_w != it->img_w || it->tex_h != it->img_h)
        g_message ("image: %s is %dx%d, stored %dx%d (%.1f MiB instead of %.1f MiB with its mip chain)",
                   path, it->img_w, it->img_h, it->tex_w, it->tex_h,
                   it->tex_w * (double) it->tex_h * 4 / 1048576.0,
                   it->img_w * (double) it->img_h * 4 * 4 / 3 / 1048576.0);
    return it;
}

static ImageTex *image_tex_get (const char *path, int side, GError **err)
{
    if (!walpe_gl_get_shared ()) {
        ImageTex *it = image_tex_load (path, side, err);
        if (it)
            it->refs = 1;
        return it;
    }
    char *key = g_strdup_printf ("%s|%d", path, side);
    g_mutex_lock (&it_lock);    /* held across a miss: the other monitors wait, then hit */
    if (!it_table)
        it_table = g_hash_table_new (g_str_hash, g_str_equal);
    ImageTex *it = g_hash_table_lookup (it_table, key);
    if (it) {
        it->refs++;
        g_free (key);
    } else if ((it = image_tex_load (path, side, err))) {
        /* other contexts may sample it right away: the upload must be complete */
        glFinish ();
        it->key = key;
        it->refs = 1;
        g_hash_table_insert (it_table, it->key, it);
    } else {
        g_free (key);
    }
    g_mutex_unlock (&it_lock);
    return it;
}

/* render thread, a context of the group current */
static void image_tex_unref (ImageTex *it)
{
    if (!it)
        return;
    g_mutex_lock (&it_lock);
    gboolean last = --it->refs <= 0;
    if (last && it->key && it_table)
        g_hash_table_remove (it_table, it->key);
    g_mutex_unlock (&it_lock);
    if (!last)
        return;
    if (it->tex)
        glDeleteTextures (1, &it->tex);
    g_free (it->key);
    g_free (it);
}

/* size to store the picture at for a vw x vh view: what the alignment
 * option shows of it, never more than it has (0 x 0 = its own size) */
static void image_want_size (ImageContent *s, int vw, int vh, int *ww, int *wh)
{
    *ww = *wh = 0;
    static WalpeLazy full_cell = WALPE_LAZY_UNSET;   /* read once (common/lazy.h) */
    int full = walpe_lazy_get (&full_cell);
    if (full < 0)
        full = walpe_lazy_set (&full_cell, g_getenv ("WALPE_IMAGE_FULLRES") ? 1 : 0);
    if (full || !s->scalable || s->img_w < 1 || s->img_h < 1 || vw < 16 || vh < 16)
        return;
    float m[9], k = 1.0f;
    walpe_fit_matrix (&s->fit, (float) s->img_w, (float) s->img_h, (float) vw, (float) vh, m, &k);
    /* stretch scales the two axes differently: k is the larger factor */
    if (k >= 0.9f)
        return;     /* about 1:1 or magnified: every pixel is needed */
    *ww = MAX (1, (int) ceilf (s->img_w * k));
    *wh = MAX (1, (int) ceilf (s->img_h * k));
}

/* the view the stored size is chosen for: the size picked for this
 * monitor's picture when the owner set one (content.h render_w / render_h),
 * else the surface's */
static void image_store_view (ImageContent *s, int *vw, int *vh)
{
    if (s->base.render_w > 0 && s->base.render_h > 0) {
        *vw = s->base.render_w;
        *vh = s->base.render_h;
    }
}

/* content.h tex_limit: the longer side the picture is stored at, at most.
 * side 0 (the picture's own size) becomes the cap when the picture is
 * longer than it. Only formats that can be decoded smaller. */
static int image_cap_side (ImageContent *s, int side)
{
    const int cap = s->base.tex_limit, own = MAX (s->img_w, s->img_h);
    if (cap <= 0 || !s->scalable || own <= cap)
        return side;
    return side > 0 ? MIN (side, cap) : cap;
}

static gboolean image_load (ImageContent *s, int vw, int vh, GError **err)
{
    int ww = 0, wh = 0;
    image_store_view (s, &vw, &vh);
    image_want_size (s, vw, vh, &ww, &wh);
    ImageTex *it = image_tex_get (s->base.path, image_cap_side (s, MAX (ww, wh)), err);
    if (!it)
        return FALSE;
    image_tex_unref (s->it);
    s->it = it;
    s->img_w = it->img_w;
    s->img_h = it->img_h;
    return TRUE;
}

static gboolean image_gl_init (WalpeContent *c, GError **err)
{
    ImageContent *s = (ImageContent *) c;
    /* too large by its header (or another kind of file now): refused
     * here, before any GL call and before anything is decoded
     * (walpe_image_new checked the file as it was when the wallpaper was
     * chosen) */
    PictureFile pf;
    const int is = picture_file (s->base.path, &pf, err);
    if (is < 0)
        return FALSE;
    s->scalable = is > 0;
    const int fw = pf.w, fh = pf.h;
    s->img_w = fw;
    s->img_h = fh;
    /* the view is not known before the first frame: the context's
     * viewport is the monitor (the daemon sets it for every frame) */
    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv (GL_VIEWPORT, vp);
    GError *e = NULL;
    if (!image_load (s, vp[2], vp[3], &e)) {
        if (e)
            g_propagate_error (err, e);
        else
            g_set_error (err, G_IO_ERROR, G_IO_ERROR_FAILED, "failed to load image %s", s->base.path);
        return FALSE;
    }
    return TRUE;
}

static gboolean image_gl_render (WalpeContent *c, int w, int h, float px, float py)
{
    (void) px; (void) py;
    ImageContent *s = (ImageContent *) c;
    if (s->it && s->scalable) {
        /* the stored size no longer fits what is shown (alignment option
         * changed, or the first guess of the view was off): decode again */
        int ww = 0, wh = 0, sw = w, sh = h;
        image_store_view (s, &sw, &sh);
        image_want_size (s, sw, sh, &ww, &wh);
        if (!ww) {
            ww = s->img_w;
            wh = s->img_h;
        }
        {
            /* texture_limit: what is wanted is never more than the cap */
            const int side = MAX (ww, wh), capped = image_cap_side (s, side);
            if (capped > 0 && capped < side) {
                ww = MAX (1, (int) ((gint64) ww * capped / side));
                wh = MAX (1, (int) ((gint64) wh * capped / side));
            }
        }
        if (ww > s->it->tex_w + 1 || wh > s->it->tex_h + 1 ||
            (ww * 3 < s->it->tex_w * 2 && wh * 3 < s->it->tex_h * 2)) {
            GError *e = NULL;
            if (!image_load (s, w, h, &e) && e) {
                /* refused (the file changed and is too large now): the
                 * picture already stored stays */
                g_message ("image %s: %s", s->base.path, e->message);
                g_clear_error (&e);
            }
        }
    }
    if (!s->it)
        return TRUE;
    WalpeMat3 m;
    if (walpe_fit_matrix (&s->fit, (float) s->img_w, (float) s->img_h, (float) w, (float) h, m.m, NULL)) {
        glClearColor (0, 0, 0, 1);   /* the picture leaves bars */
        glClear (GL_COLOR_BUFFER_BIT);
    }
    walpe_glutil_draw_quad (s->it->tex, s->it->premult, 1.0f, m.m, WALPE_BLEND_NORMAL);
    return TRUE;
}

static void image_gl_free (WalpeContent *c)
{
    ImageContent *s = (ImageContent *) c;
    image_tex_unref (s->it);
    s->it = NULL;
}

/* still images have nothing to pause — a no-op so the daemon's pause
 * paths can call this unconditionally (NULL here = segfault when an
 * image is applied while the monitor is covered/paused) */
static void image_set_paused (WalpeContent *c, gboolean p)
{
    (void) c; (void) p;
}

static void image_free (WalpeContent *c)
{
    g_free (c->path);
    g_free (c->title);
    g_free (c);
}

/* (The picture formats a picture or GIF wallpaper is decoded as are
 * picture_file's: gdk-pixbuf picks its decoder from the file's bytes,
 * whatever the name says - a "photo.png" that is an SVG document went to
 * librsvg, a ".gif" that is XPM text to that decoder, and so on for every
 * loader the system has (Windows icons and cursors, PNM, QuickTime
 * pictures ...) - so a file reaches it only as one of those formats.) */
gboolean walpe_picture_size_ok (const char *path, GError **err)
{
    PictureFile pf;
    return picture_file (path, &pf, err) >= 0;     /* 0: not a picture format, its own loader decides */
}

GdkPixbufAnimation *walpe_picture_animation_open (const char *path, GError **err)
{
    PictureFile pf;
    const int is = picture_file (path, &pf, err);
    if (is < 0)
        return NULL;
    if (is == 0) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "%s is not a picture format a wallpaper can be", path);
        return NULL;
    }
    if (strcmp (pf.kind, "tga")) {
        /* (gdk-pixbuf 2.44 marks its animation interface as deprecated and
         * names no successor inside the library; the call stays - gif.c) */
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GdkPixbufAnimation *opened = gdk_pixbuf_animation_new_from_file (path, err);
        G_GNUC_END_IGNORE_DEPRECATIONS
        return opened;
    }
    /* (a TGA: image_tex_load) */
    GdkPixbufAnimation *anim = NULL;
    GdkPixbuf *pb = walpe_gl_picture_file_decode (path, pf.kind, &pf.tga, 0, 0, &anim, err);
    g_clear_object (&pb);
    return anim;
}

WalpeContent *walpe_image_new (const char *path, GError **err)
{
    if (!walpe_file_exists (path)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no such file: %s", path);
        return NULL;
    }
    if (!walpe_picture_size_ok (path, err))
        return NULL;
    ImageContent *s = g_new0 (ImageContent, 1);
    s->base.type = WALPE_TYPE_IMAGE;
    s->base.path = g_strdup (path);
    s->base.title = g_path_get_basename (path);
    s->base.gl_init = image_gl_init;
    s->base.gl_render = image_gl_render;
    s->base.gl_free = image_gl_free;
    s->base.set_paused = image_set_paused;
    s->base.free = image_free;
    walpe_content_fit (path, &s->fit);
    return &s->base;
}
