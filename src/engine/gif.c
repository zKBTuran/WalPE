/* gif.c - animated GIF (and other GdkPixbufAnimation formats) wallpaper.
 *
 * One decode for all monitors: sources are keyed by file path and
 * refcounted, so two monitors showing the same GIF share one
 * GdkPixbufAnimation and its iterator; the frame is composed once and
 * each monitor uploads it into its own texture (before, every monitor
 * decoded the file and composed every frame itself). The upload goes
 * straight from the composed frame into a texture allocated once
 * (glTexSubImage2D): no per-frame malloc, row copy or glTexImage2D, and
 * the premultiply pass runs only for frames that really have transparent
 * pixels (into a reused buffer). gdk-pixbuf keeps the frames compressed
 * and composes them on demand, so a long GIF does not sit in memory
 * decoded. Frame timing is the GIF's own (its frame delays).
 *
 * gdk-pixbuf 2.44 marks its whole animation interface (GdkPixbufAnimation,
 * its iterator) as deprecated and names no successor inside the library:
 * the advice is another image library. The calls stay as they are - they
 * decode and compose the frames here, with gdk-pixbuf's rules for delays
 * and for what a frame leaves behind - and the compiler's note is switched
 * off around each of them and nowhere else. */
#include "gif.h"
#include "fit.h"
#include "image.h"   /* walpe_picture_size_ok */
#include "glutil.h"
#include "common/gifcheck.h"
#include "common/premult.h"
#include "common/util.h"
#include <stdlib.h>
#include <string.h>

/* shared by every monitor showing the file (path-keyed, refcounted) */
typedef struct {
    char *path;
    int refs;
    GMutex lock;
    GdkPixbufAnimation *anim;
    GdkPixbufAnimationIter *iter;   /* NULL: a still picture */
    GdkPixbuf *frame;               /* the current composed frame */
    guint64 serial;                 /* counts frame changes */
    gint64 due_us;                  /* when the current frame expires, 0 = not armed */
    /* the pixels the viewers upload for `serial`: the frame's own when it
     * is opaque, else a premultiplied copy in pm */
    guint64 ready_serial;
    const guint8 *px;
    int w, h, stride, channels;
    guint8 *pm;
    gsize pm_cap;
} GifSource;

static GMutex gif_reg_lock;
static GHashTable *gif_reg;         /* path -> GifSource */

typedef struct {
    WalpeContent base;
    GifSource *src;
    GLuint tex;
    int tex_w, tex_h, tex_ch;       /* storage of tex */
    guint64 serial;                 /* frame in tex */
    int w, h;
    gboolean paused;
    WalpeFit fit;        /* alignment option (fit.h) */
} GifContent;

static GifSource *gif_source_get (const char *path, GError **err)
{
    g_mutex_lock (&gif_reg_lock);   /* held across a miss: the other monitors wait, then hit */
    if (!gif_reg)
        gif_reg = g_hash_table_new (g_str_hash, g_str_equal);
    GifSource *src = g_hash_table_lookup (gif_reg, path);
    if (src) {
        src->refs++;
        g_mutex_unlock (&gif_reg_lock);
        return src;
    }
    /* Before gdk-pixbuf gets anything of the file: every frame of a GIF
     * has pixels and lies inside the picture the header declares
     * (gifcheck.h), as the format requires. The limits below are on the
     * picture alone; with this check they hold for the frames as well.
     * A GIF that is not so is refused here, as
     * a damaged file. */
    if (!walpe_gif_file_layout_ok (path, err)) {
        g_mutex_unlock (&gif_reg_lock);
        return NULL;
    }
    /* The one place a GIF is opened and decoded, for every monitor. The
     * size is in the file header: a picture above the limits of image.h
     * (16384 pixels per side - more than a texture holds - and 64
     * megapixels in all) is refused before anything is decoded. A 71-byte
     * GIF declaring 20617 x 18254 pixels took 3 GB and then showed
     * nothing; a 155-byte one with a 12000 x 12000 canvas took 1.8 GB and
     * seconds per frame. */
    G_STATIC_ASSERT (WALPE_PICTURE_MAX_SIDE <= 16384);
    /* (walpe_picture_animation_open checks format and size, as
     * walpe_picture_size_ok does, on the file's own header, before
     * gdk-pixbuf opens it) */
    GdkPixbufAnimation *anim = walpe_picture_animation_open (path, err);
    if (!anim) {
        g_mutex_unlock (&gif_reg_lock);
        return NULL;
    }
    /* the same limits on what was really opened (the file can be replaced
     * between the look at its header and the decode) */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS    /* gdk-pixbuf's animations: no successor (see the top) */
    int aw = gdk_pixbuf_animation_get_width (anim), ah = gdk_pixbuf_animation_get_height (anim);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (aw > WALPE_PICTURE_MAX_SIDE || ah > WALPE_PICTURE_MAX_SIDE ||
        (gint64) aw * ah > WALPE_PICTURE_MAX_PIXELS) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "the picture changed while it was opened and is %dx%d now: too large to "
                     "show (limit %d per side, %d megapixels)",
                     aw, ah, WALPE_PICTURE_MAX_SIDE, WALPE_PICTURE_MAX_PIXELS / 1000000);
        g_object_unref (anim);
        g_mutex_unlock (&gif_reg_lock);
        return NULL;
    }
    src = g_new0 (GifSource, 1);
    src->path = g_strdup (path);
    src->refs = 1;
    g_mutex_init (&src->lock);
    src->anim = anim;
    GdkPixbuf *first = NULL;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS    /* gdk-pixbuf's animations: no successor (see the top) */
    if (gdk_pixbuf_animation_is_static_image (anim)) {
        /* not actually animated - draw once */
        first = gdk_pixbuf_animation_get_static_image (anim);
    } else {
        src->iter = gdk_pixbuf_animation_get_iter (anim, NULL); /* NULL = now */
        first = gdk_pixbuf_animation_iter_get_pixbuf (src->iter);
    }
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (first) {
        src->frame = g_object_ref (first);
        src->serial = 1;
    }
    g_hash_table_insert (gif_reg, src->path, src);
    g_mutex_unlock (&gif_reg_lock);
    return src;
}

static void gif_source_unref (GifSource *src)
{
    if (!src)
        return;
    g_mutex_lock (&gif_reg_lock);
    gboolean last = --src->refs <= 0;
    if (last && gif_reg)
        g_hash_table_remove (gif_reg, src->path);
    g_mutex_unlock (&gif_reg_lock);
    if (!last)
        return;
    g_clear_object (&src->frame);
    g_clear_object (&src->iter);
    g_clear_object (&src->anim);
    g_free (src->pm);
    g_free (src->path);
    g_mutex_clear (&src->lock);
    g_free (src);
}

/* src->lock held: move to the frame that is due now, if the current one
 * has expired, and arm the clock for it */
static void gif_source_advance (GifSource *src)
{
    if (!src->iter)
        return;
    gint64 now = g_get_monotonic_time ();
    if (src->due_us && now >= src->due_us) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS    /* gdk-pixbuf's animations: no successor (see the top) */
        gboolean moved = gdk_pixbuf_animation_iter_advance (src->iter, NULL);
        gboolean played = gdk_pixbuf_animation_iter_get_delay_time (src->iter) < 0;
        G_GNUC_END_IGNORE_DEPRECATIONS
        if (played) {
            /* the GIF has played its loop count (none given = once):
             * gdk-pixbuf then reports delay -1 for good, which the 20 ms
             * floor below turned into 50 uploads a second of the same
             * last frame, for ever. Start over instead - a wallpaper
             * loops, as in Wallpaper Engine. */
            g_object_unref (src->iter);
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS    /* gdk-pixbuf's animations: no successor (see the top) */
            src->iter = gdk_pixbuf_animation_get_iter (src->anim, NULL);
            G_GNUC_END_IGNORE_DEPRECATIONS
            moved = TRUE;
        }
        if (moved) {    /* only a frame that changed is uploaded */
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS    /* gdk-pixbuf's animations: no successor (see the top) */
            GdkPixbuf *pb = gdk_pixbuf_animation_iter_get_pixbuf (src->iter);
            G_GNUC_END_IGNORE_DEPRECATIONS
            if (pb) {
                g_object_ref (pb);
                g_clear_object (&src->frame);
                src->frame = pb;
                src->serial++;
            }
        }
        src->due_us = 0;
    }
    if (!src->due_us) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS    /* gdk-pixbuf's animations: no successor (see the top) */
        int delay = gdk_pixbuf_animation_iter_get_delay_time (src->iter);
        G_GNUC_END_IGNORE_DEPRECATIONS
        if (delay < 20)
            delay = 20; /* 0-delay gifs would spin the loop */
        src->due_us = now + (gint64) delay * 1000;
    }
}

/* src->lock held: the pixels of the current frame as they are uploaded
 * (premultiplied). An opaque frame is used as it is; one with
 * transparent pixels is premultiplied into the source's buffer, once for
 * all monitors. FALSE: a pixel layout this cannot upload. */
static gboolean gif_source_pixels (GifSource *src)
{
    if (src->ready_serial == src->serial)
        return src->px != NULL;
    src->ready_serial = src->serial;
    src->px = NULL;
    GdkPixbuf *pb = src->frame;
    if (!pb || gdk_pixbuf_get_bits_per_sample (pb) != 8)
        return FALSE;
    int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
    int rs = gdk_pixbuf_get_rowstride (pb), nc = gdk_pixbuf_get_n_channels (pb);
    const guint8 *p = gdk_pixbuf_read_pixels (pb);
    if (w < 1 || h < 1 || !p)
        return FALSE;
    src->w = w;
    src->h = h;
    if (!gdk_pixbuf_get_has_alpha (pb) && nc == 3 && rs == ((w * 3 + 3) & ~3)) {
        src->px = p;            /* RGB rows, padded to 4 bytes as GL reads them */
        src->stride = rs;
        src->channels = 3;
        return TRUE;
    }
    if (nc != 4 || rs < w * 4 || (rs & 3) || ((guintptr) p & 3))
        return FALSE;
    /* any pixel that is not fully opaque? One AND per 32-bit pixel (rows
     * are 4-byte aligned); alpha is its last byte in memory */
    const guint32 amask = GUINT32_TO_LE (0xff000000u);
    gboolean opaque = TRUE;
    for (int y = 0; y < h && opaque; y++) {
        const guint32 *r = (const guint32 *) (gconstpointer) (p + (gsize) y * rs);
        guint32 acc = 0xffffffffu;
        for (int x = 0; x < w; x++)
            acc &= r[x];
        opaque = (acc & amask) == amask;
    }
    src->channels = 4;
    if (opaque) {
        src->px = p;
        src->stride = rs;
        return TRUE;
    }
    gsize need = (gsize) w * h * 4;
    if (src->pm_cap < need) {
        g_free (src->pm);
        src->pm = g_try_malloc (need);  /* no memory: this frame is not shown */
        src->pm_cap = src->pm ? need : 0;
        if (!src->pm)
            return FALSE;
    }
    for (int y = 0; y < h; y++)
        memcpy (src->pm + (gsize) y * w * 4, p + (gsize) y * rs, (gsize) w * 4);
    walpe_premultiply_rgba8 (src->pm, need);
    src->px = src->pm;
    src->stride = w * 4;
    return TRUE;
}

/* render thread: the source's current frame into this monitor's texture */
static void gif_upload (GifContent *s)
{
    GifSource *src = s->src;
    if (!gif_source_pixels (src)) {
        s->serial = src->serial;
        return;
    }
    glBindTexture (GL_TEXTURE_2D, s->tex);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, src->channels == 4 ? src->stride / 4 : 0);
    GLenum fmt = src->channels == 4 ? GL_RGBA : GL_RGB;
    if (s->tex_w != src->w || s->tex_h != src->h || s->tex_ch != src->channels) {
        /* storage once; frames are written into it in place */
        glTexImage2D (GL_TEXTURE_2D, 0, src->channels == 4 ? GL_RGBA8 : GL_RGB8, src->w, src->h, 0,
                      fmt, GL_UNSIGNED_BYTE, NULL);
        s->tex_w = src->w;
        s->tex_h = src->h;
        s->tex_ch = src->channels;
    }
    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, src->w, src->h, fmt, GL_UNSIGNED_BYTE, src->px);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    s->w = src->w;
    s->h = src->h;
    s->serial = src->serial;
}

static gboolean gif_gl_init (WalpeContent *c, GError **err)
{
    GifContent *s = (GifContent *) c;
    s->src = gif_source_get (s->base.path, err);
    if (!s->src)
        return FALSE;
    glGenTextures (1, &s->tex);
    glBindTexture (GL_TEXTURE_2D, s->tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_mutex_lock (&s->src->lock);
    if (s->src->serial)
        gif_upload (s);
    g_mutex_unlock (&s->src->lock);
    return TRUE;
}

static gboolean gif_gl_render (WalpeContent *c, int w, int h, float px, float py)
{
    (void) px; (void) py;
    GifContent *s = (GifContent *) c;
    GifSource *src = s->src;
    if (!src)
        return TRUE;
    g_mutex_lock (&src->lock);
    /* a paused monitor draws what the source shows but does not drive it
     * (the daemon renders a paused monitor once, under covers) */
    if (!s->paused)
        gif_source_advance (src);
    if (s->serial != src->serial)
        gif_upload (s);
    /* arm the render thread's frame clock for the current frame's delay */
    if (!s->paused && src->iter)
        s->base.next_frame_us = src->due_us;
    g_mutex_unlock (&src->lock);
    WalpeMat3 m;
    if (walpe_fit_matrix (&s->fit, (float) s->w, (float) s->h, (float) w, (float) h, m.m, NULL)) {
        glClearColor (0, 0, 0, 1);   /* the picture leaves bars */
        glClear (GL_COLOR_BUFFER_BIT);
    }
    walpe_glutil_draw_quad (s->tex, TRUE, 1.0f, m.m, WALPE_BLEND_NORMAL);
    return TRUE;
}

static void gif_gl_free (WalpeContent *c)
{
    GifContent *s = (GifContent *) c;
    if (s->tex) {
        glDeleteTextures (1, &s->tex);
        s->tex = 0;
    }
    s->tex_w = s->tex_h = s->tex_ch = 0;
    gif_source_unref (s->src);
    s->src = NULL;
}

static void gif_set_paused (WalpeContent *c, gboolean p)
{
    GifContent *s = (GifContent *) c;
    s->paused = p;
    if (!p && s->src) {
        /* restart the frame delay on resume */
        g_mutex_lock (&s->src->lock);
        s->src->due_us = 0;
        g_mutex_unlock (&s->src->lock);
    }
}

/* render thread, synchronous: no main-loop source references content */
static void gif_free (WalpeContent *c)
{
    g_free (c->path);
    g_free (c->title);
    g_free (c);
}

WalpeContent *walpe_gif_new_opts (const char *path, const char *origin_dir, GError **err)
{
    if (!walpe_file_exists (path)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no such file: %s", path);
        return NULL;
    }
    GifContent *s = g_new0 (GifContent, 1);
    s->base.type = WALPE_TYPE_GIF;
    s->base.path = g_strdup (path);
    s->base.title = g_path_get_basename (path);
    s->base.gl_init = gif_gl_init;
    s->base.gl_render = gif_gl_render;
    s->base.gl_free = gif_gl_free;
    s->base.set_paused = gif_set_paused;
    s->base.free = gif_free;
    walpe_content_fit (origin_dir && *origin_dir ? origin_dir : path, &s->fit);
    return &s->base;
}

WalpeContent *walpe_gif_new (const char *path, GError **err)
{
    return walpe_gif_new_opts (path, NULL, err);
}
