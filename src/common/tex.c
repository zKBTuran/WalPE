#include "tex.h"
#include "gifcheck.h"
#include "util.h"
#include <lz4.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
static guint32 rd32le (const guint8 *p) { return (guint32) p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24); }

/* largest texture side any path accepts */
#define TEX_MAX_DIM 16384
/* ... and the most pixels in one texture: 8192 x 8192 (256 MiB as RGBA).
 * The side limit alone allowed 16384 x 16384 = 1 GiB decoded (plus the
 * decoder's own copy and the mip chain on the GPU) from a file of a few KB.
 * The largest texture in 89 Workshop scenes checked is 6144 x 4096. */
#define TEX_MAX_PIXELS ((gint64) 8192 * 8192)

static gboolean tex_size_ok (gint64 w, gint64 h)
{
    return w > 0 && h > 0 && w <= TEX_MAX_DIM && h <= TEX_MAX_DIM && w * h <= TEX_MAX_PIXELS;
}

WalpeTex *walpe_tex_from_pixbuf (GdkPixbuf *pb)
{
    if (!pb)
        return NULL;
    int w = gdk_pixbuf_get_width (pb);
    int h = gdk_pixbuf_get_height (pb);
    if (!tex_size_ok (w, h)) {
        g_object_unref (pb);   /* this function consumes pb on every path */
        return NULL;
    }
    /* an RGB pixbuf (every JPEG) is read as it is: the loop below writes
     * alpha 255 for three channels. It used to be copied to an RGBA pixbuf
     * first (gdk_pixbuf_add_alpha): 4 more bytes per pixel alive during the
     * decode - 31.6 MiB for a 3840x2160 picture - and one more pass. */
    int rs = gdk_pixbuf_get_rowstride (pb);
    int nch = gdk_pixbuf_get_n_channels (pb);
    /* the loop reads nch bytes per pixel: only 8-bit RGB / RGBA is that */
    if (gdk_pixbuf_get_bits_per_sample (pb) != 8 || (nch != 3 && nch != 4)) {
        g_object_unref (pb);
        return NULL;
    }
    WalpeTex *t = g_new0 (WalpeTex, 1);
    t->format = WALPE_TEX_RGBA8;
    t->width = w;
    t->height = h;
    t->full_width = w;
    t->full_height = h;
    t->base_width = w;
    t->base_height = h;
    t->len = (gsize) w * h * 4;
    t->data = g_try_malloc (t->len);
    const guint8 *src = gdk_pixbuf_read_pixels (pb);
    if (!t->data || !src) {
        g_object_unref (pb);
        walpe_tex_free (t);
        return NULL;
    }
    /* swizzle to BGRA (gdk gives RGBA): uploads use GL_BGRA, so R/B must
     * be exchanged here or every pixbuf-decoded texture renders swapped */
    for (int y = 0; y < h; y++) {
        const guint8 *srow = src + (gsize) y * rs;
        guint8 *orow = t->data + (gsize) y * w * 4;
        for (int x = 0; x < w; x++) {
            orow[x * 4 + 0] = srow[x * nch + 2]; /* B */
            orow[x * 4 + 1] = srow[x * nch + 1]; /* G */
            orow[x * 4 + 2] = srow[x * nch + 0]; /* R */
            orow[x * 4 + 3] = nch == 4 ? srow[x * nch + 3] : 0xff;
        }
    }
    g_object_unref (pb);
    return t;
}

/* the picture's size is known before its pixels are allocated: one the
 * loader would refuse anyway (walpe_tex_from_pixbuf) is not decoded at all.
 * A few-KB PNG/JPEG declaring 30000x30000 used to be decoded in full
 * (gigabytes) and only then rejected. Size 0x0 makes the loaders stop. */
static void tex_size_prepared (GdkPixbufLoader *ld, int w, int h, gpointer user)
{
    if (!tex_size_ok (w, h)) {
        if (w > 0 && h > 0)
            g_warning ("texture: a picture of %d x %d pixels is not loaded (the limit is %d per "
                       "side and 8192 x 8192 pixels in all)", w, h, TEX_MAX_DIM);
        *(gboolean *) user = TRUE;
        gdk_pixbuf_loader_set_size (ld, 0, 0);
    }
}

/* gdk_pixbuf_new_from_stream on a memory buffer (same 64 KiB writes, first
 * frame of an animation), with the size check above. The loader is made
 * for the one format the caller found by its signature ("png", "jpeg",
 * ...): gdk-pixbuf otherwise picks a decoder from the bytes itself, and
 * "RIFF" at the start of a texture file was enough to hand a wallpaper's
 * bytes to its Windows cursor (ANI / ICO) decoders. NULL when this
 * gdk-pixbuf has no loader of that name (WebP is a separate package). */
static GdkPixbuf *tex_pixbuf_from_data (const guint8 *d, gsize len, const char *type)
{
    gboolean too_big = FALSE, ok = TRUE;
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new_with_type (type, NULL);
    if (!ld) {
        /* said once per format: with PNG or JPEG missing most textures stay empty */
        static gint told;
        guint bit = 1u << (g_str_hash (type) % 31);
        if (!((guint) g_atomic_int_or (&told, bit) & bit))
            g_message ("texture: this gdk-pixbuf has no \"%s\" loader; such pictures are not loaded",
                       type);
        return NULL;
    }
    g_signal_connect (ld, "size-prepared", G_CALLBACK (tex_size_prepared), &too_big);
    for (gsize off = 0; ok && !too_big && off < len; off += 65536)
        ok = gdk_pixbuf_loader_write (ld, d + off, MIN ((gsize) 65536, len - off), NULL);
    ok = gdk_pixbuf_loader_close (ld, NULL) && ok;
    GdkPixbuf *pb = ok && !too_big ? gdk_pixbuf_loader_get_pixbuf (ld) : NULL;
    if (pb)
        g_object_ref (pb);
    g_object_unref (ld);
    return pb;
}

/* the picture formats a texture file may wrap, by signature: the loader's
 * name, or NULL */
static const char *tex_embedded_kind (const guint8 *d, gsize left)
{
    if (left >= 8 && memcmp (d, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "png";
    if (left >= 3 && memcmp (d, "\xff\xd8\xff", 3) == 0)
        return "jpeg";
    if (left >= 12 && memcmp (d, "RIFF", 4) == 0 && memcmp (d + 8, "WEBP", 4) == 0)
        return "webp";
    return NULL;
}

/* A signature in the first 4 KiB that does not decode is not the picture;
 * the search goes on, but not for ever: every hit is a decode of the rest
 * of the file, and 600 JPEG starts nested in each other's comment segments
 * in front of 8 MB of zeros kept the loading thread busy for 35 s (the time
 * grows with the file). A real container has its picture at the first hit. */
#define TEX_EMBEDDED_TRIES 4

/* the size a picture file's header declares: the loader stops as soon as
 * it knows it, nothing is allocated or decoded */
typedef struct { int w, h; gboolean seen; } TexSizeOnly;

static void tex_size_only (GdkPixbufLoader *ld, int w, int h, gpointer user)
{
    TexSizeOnly *so = user;
    if (!so->seen) {
        so->w = w;
        so->h = h;
        so->seen = TRUE;
    }
    gdk_pixbuf_loader_set_size (ld, 0, 0);
}

/* type: the decoder of the signature found (tex_embedded_kind), as in
 * tex_pixbuf_from_data - never one gdk-pixbuf picks from the bytes */
static gboolean tex_picture_size_from_data (const guint8 *d, gsize len, const char *type, int *w,
                                            int *h)
{
    TexSizeOnly so = { 0, 0, FALSE };
    gboolean ok = TRUE;
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new_with_type (type, NULL);
    if (!ld)
        return FALSE;
    g_signal_connect (ld, "size-prepared", G_CALLBACK (tex_size_only), &so);
    for (gsize off = 0; ok && !so.seen && off < len; off += 65536)
        ok = gdk_pixbuf_loader_write (ld, d + off, MIN ((gsize) 65536, len - off), NULL);
    gdk_pixbuf_loader_close (ld, NULL);
    g_object_unref (ld);
    if (!so.seen || !tex_size_ok (so.w, so.h))
        return FALSE;
    *w = so.w;
    *h = so.h;
    return TRUE;
}

static WalpeTex *try_embedded_image (const guint8 *d, gsize len)
{
    int tries = 0;
    for (gsize off = 0; off + 4 < len && off < 4096 && tries < TEX_EMBEDDED_TRIES; off++) {
        const char *kind = tex_embedded_kind (d + off, len - off);
        if (kind) {
            tries++;
            GdkPixbuf *pb = tex_pixbuf_from_data (d + off, len - off, kind);
            if (pb && off == 0) {
                /* a plain image file (user / preset texture, e.g. a phone
                 * photo): honour its EXIF orientation like WE does
                 * (3493971117's left image was drawn on its side) */
                GdkPixbuf *o = gdk_pixbuf_apply_embedded_orientation (pb);
                if (o) {
                    g_object_unref (pb);
                    pb = o;
                }
            }
            if (pb)
                return walpe_tex_from_pixbuf (pb);
        }
    }
    return NULL;
}

/* ------------------------------------------------------- DXT block decode */

/* one DXT color block (8 bytes) -> BGRA pixels; dxt1 selects the
 * transparent-black palette mode; stride = destination image width */
static void dxt_color_block (const guint8 *b, guint8 *out, int pw, int ph,
                             int stride, int dxt1)
{
    guint16 c0 = (guint16) (b[0] | (b[1] << 8));
    guint16 c1 = (guint16) (b[2] | (b[3] << 8));
    guint32 bits = (guint32) b[4] | ((guint32) b[5] << 8) |
                   ((guint32) b[6] << 16) | ((guint32) b[7] << 24);
    guint8 r0 = ((c0 >> 11) & 0x1f), g0 = ((c0 >> 5) & 0x3f), b0 = (c0 & 0x1f);
    guint8 r1 = ((c1 >> 11) & 0x1f), g1 = ((c1 >> 5) & 0x3f), b1 = (c1 & 0x1f);
    guint8 pal[4][4];
    pal[0][0] = (guint8) ((b0 << 3) | (b0 >> 2));
    pal[0][1] = (guint8) ((g0 << 2) | (g0 >> 4));
    pal[0][2] = (guint8) ((r0 << 3) | (r0 >> 2));
    pal[1][0] = (guint8) ((b1 << 3) | (b1 >> 2));
    pal[1][1] = (guint8) ((g1 << 2) | (g1 >> 4));
    pal[1][2] = (guint8) ((r1 << 3) | (r1 >> 2));
    if (dxt1 && c0 <= c1) {
        pal[2][0] = (pal[0][0] + pal[1][0]) / 2;
        pal[2][1] = (pal[0][1] + pal[1][1]) / 2;
        pal[2][2] = (pal[0][2] + pal[1][2]) / 2;
        pal[3][0] = pal[3][1] = pal[3][2] = 0;
        pal[0][3] = pal[1][3] = pal[2][3] = 255;
        pal[3][3] = 0;
    } else {
        pal[2][0] = (guint8) ((2 * pal[0][0] + pal[1][0]) / 3);
        pal[2][1] = (guint8) ((2 * pal[0][1] + pal[1][1]) / 3);
        pal[2][2] = (guint8) ((2 * pal[0][2] + pal[1][2]) / 3);
        pal[3][0] = (guint8) ((pal[0][0] + 2 * pal[1][0]) / 3);
        pal[3][1] = (guint8) ((pal[0][1] + 2 * pal[1][1]) / 3);
        pal[3][2] = (guint8) ((pal[0][2] + 2 * pal[1][2]) / 3);
        pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    }
    for (int y = 0; y < ph; y++) {
        for (int x = 0; x < pw; x++) {
            int idx = (bits >> (2 * (y * 4 + x))) & 3;
            guint8 *o = out + ((y * stride + x) * 4);
            o[0] = pal[idx][0];
            o[1] = pal[idx][1];
            o[2] = pal[idx][2];
            o[3] = pal[idx][3];
        }
    }
}

/* DXT1/3/5 -> BGRA; w is the full (padded, 4-aligned) mip width, h the
 * rows wanted from its top (the mip's height or less: the last block row
 * is then cut); src must hold ((w+3)/4)*((h+3)/4) blocks (checked by the
 * caller) */
static guint8 *walpe_dxt_decode (const guint8 *src, guint32 w, guint32 h, int fmt)
{
    guint8 *out = g_try_malloc ((gsize) w * h * 4);
    if (!out)
        return NULL;
    int bw = (int) ((w + 3) / 4), bh = (int) ((h + 3) / 4);
    int bsize = fmt == WALPE_TEX_DXT1 ? 8 : 16;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            const guint8 *b = src + ((gsize) by * bw + bx) * bsize;
            int pw = (bx * 4 + 4 <= (int) w) ? 4 : (int) w - bx * 4;
            int ph = (by * 4 + 4 <= (int) h) ? 4 : (int) h - by * 4;
            guint8 *outb = out + (((gsize) by * 4 * w) + bx * 4) * 4;
            guint8 ablock[64];
            const guint8 *cblock = b;
            if (fmt == WALPE_TEX_DXT3) {
                for (int y = 0; y < ph; y++)
                    for (int x = 0; x < pw; x++) {
                        int nib = y * 4 + x;
                        guint8 a = (nib & 1) ? (b[nib / 2] >> 4) : (b[nib / 2] & 0xf);
                        ablock[y * 4 + x] = (guint8) (a * 17);
                    }
                cblock = b + 8;
            } else if (fmt == WALPE_TEX_DXT5) {
                guint8 a0 = b[0], a1 = b[1];
                guint64 ai = 0;
                for (int i = 0; i < 6; i++)
                    ai |= (guint64) b[2 + i] << (8 * i);
                guint8 pal[8];
                pal[0] = a0;
                pal[1] = a1;
                if (a0 > a1) {
                    for (int i = 0; i < 6; i++)
                        pal[2 + i] = (guint8) (((6 - i) * a0 + (1 + i) * a1) / 7);
                } else {
                    pal[2] = (guint8) ((4 * a0 + a1) / 5);
                    pal[3] = (guint8) ((3 * a0 + 2 * a1) / 5);
                    pal[4] = (guint8) ((2 * a0 + 3 * a1) / 5);
                    pal[5] = (guint8) ((a0 + 4 * a1) / 5);
                    pal[6] = 0;
                    pal[7] = 255;
                }
                for (int y = 0; y < ph; y++)
                    for (int x = 0; x < pw; x++) {
                        int i = y * 4 + x;
                        ablock[y * 4 + x] = pal[(ai >> (3 * i)) & 7];
                    }
                cblock = b + 8;
            }
            dxt_color_block (cblock, outb, pw, ph, (int) w, fmt == WALPE_TEX_DXT1);
            if (fmt != WALPE_TEX_DXT1)
                for (int y = 0; y < ph; y++)
                    for (int x = 0; x < pw; x++)
                        outb[((y * (int) w + x) * 4) + 3] = ablock[y * 4 + x];
        }
    }
    return out;
}

/* ------------------------------------------------ container walking */

static const gsize OFF_TYPE = 18, OFF_FLAGS = 22, OFF_IW = 34, OFF_IH = 38,
                   OFF_CONTAINER = 46;

static int tex_container_ver (const guint8 *d, gsize len)
{
    if (len < OFF_CONTAINER + 9 || memcmp (d + OFF_CONTAINER, "TEXB000", 7) != 0)
        return 0;
    char v = (char) d[OFF_CONTAINER + 7];
    return v >= '1' && v <= '4' ? v - '0' : 0;
}

/* offset of image 0's mip count: u32s between the magic and it are
 * 0001/0002 = image count; 0003 = + freeimage format; 0004 = + one more
 * (observed 0) — verified against file-size-exact samples */
static gsize tex_images_off (int ver)
{
    return OFF_CONTAINER + 9 + (ver == 3 ? 8 : ver == 4 ? 12 : 4);
}

/* walk every image's mips (bounds-checked); returns the offset just past
 * the last mip, 0 when the layout does not fit the buffer */
static gsize tex_images_end (const guint8 *d, gsize len, int ver)
{
    guint32 nimg = rd32le (d + OFF_CONTAINER + 9);
    gsize p = tex_images_off (ver);
    gsize ent = ver == 1 ? 12 : 20;
    if (nimg == 0 || nimg > 4096)
        return 0;
    for (guint32 i = 0; i < nimg; i++) {
        if (p + 4 > len)
            return 0;
        guint32 mips = rd32le (d + p);
        p += 4;
        if (mips == 0 || mips > 16)
            return 0;
        for (guint32 m = 0; m < mips; m++) {
            if (p + ent > len)
                return 0;
            guint32 sz = ver == 1 ? rd32le (d + p + 8) : rd32le (d + p + 16);
            p += ent;
            if (sz > len - p)
                return 0;
            p += sz;
        }
    }
    return p;
}

static float rdf32le (const guint8 *p)
{
    guint32 u = rd32le (p);
    float f;
    memcpy (&f, &u, 4);
    return isfinite (f) ? f : 0.0f;
}

/* TEXS0002/0003 frame table (follows the images when flag 4 is set):
 * "TEXS000N\0" u32 count [0003: u32 width, u32 height] then count x
 * { u32 image, f32 time, f32 x, f32 y, f32 w1, f32 w2, f32 h2, f32 h1 } */
static void tex_parse_frames (WalpeTex *t, const guint8 *d, gsize len, int ver,
                              const char *path)
{
    gsize p = tex_images_end (d, len, ver);
    if (!p || p + 13 > len || memcmp (d + p, "TEXS000", 7) != 0) {
        g_debug ("tex %s: animated flag but no TEXS table", path);
        return;
    }
    int sver = d[p + 7] - '0';
    p += 9;
    guint32 n = rd32le (d + p);
    p += 4;
    int fw = 0, fh = 0;
    if (sver >= 3) {
        if (p + 8 > len)
            return;
        fw = (int) rd32le (d + p);
        fh = (int) rd32le (d + p + 4);
        p += 8;
    }
    if (n == 0 || n > 65536 || n > (len - p) / 32) {
        g_debug ("tex %s: TEXS%d frame count %u does not fit", path, sver, n);
        return;
    }
    t->frames = g_new0 (WalpeTexFrame, n);
    for (guint32 i = 0; i < n; i++, p += 32) {
        WalpeTexFrame *f = &t->frames[i];
        f->image = (int) rd32le (d + p);
        f->time = rdf32le (d + p + 4);
        f->x = rdf32le (d + p + 8);
        f->y = rdf32le (d + p + 12);
        f->w1 = rdf32le (d + p + 16);
        f->w2 = rdf32le (d + p + 20);
        f->h2 = rdf32le (d + p + 24);
        f->h1 = rdf32le (d + p + 28);
    }
    t->nframes = (int) n;
    if (fw <= 0 || fh <= 0 || fw > 16384 || fh > 16384) {
        /* clamped before the cast: (int) of a huge float is undefined */
        fw = (int) CLAMP (fabsf (t->frames[0].w1), 0.0f, (float) TEX_MAX_DIM);
        fh = (int) CLAMP (fabsf (t->frames[0].h1), 0.0f, (float) TEX_MAX_DIM);
    }
    t->frame_w = fw > 0 ? fw : t->width;
    t->frame_h = fh > 0 ? fh : t->height;
    g_debug ("tex %s: TEXS%d %d frames %dx%d", path, sver, t->nframes, t->frame_w, t->frame_h);
}

/* mip 0 of image 0: dims + payload range */
static gboolean tex_mip0 (const guint8 *d, gsize len, int ver, guint32 *mw, guint32 *mh,
                          guint32 *compressed, guint32 *unc, guint32 *comp, gsize *data_off)
{
    gsize p = tex_images_off (ver);
    gsize ent = ver == 1 ? 12 : 20;
    if (p + 4 + ent > len)
        return FALSE;
    p += 4; /* mip count */
    *mw = rd32le (d + p);
    *mh = rd32le (d + p + 4);
    if (ver == 1) {
        *compressed = 0;
        *comp = *unc = rd32le (d + p + 8);
    } else {
        *compressed = rd32le (d + p + 8);
        *unc = rd32le (d + p + 12);
        *comp = rd32le (d + p + 16);
    }
    *data_off = p + ent;
    return TRUE;
}

static void tex_drop_levels (WalpeTex *t)
{
    for (int i = 0; i < t->n_levels; i++)
        g_free (t->level[i].data);
    t->n_levels = 0;
}

/* the stored mip levels 1.. of a DXT image kept compressed (level 0 was
 * read by the caller; q = offset of level 1's entry). WE's own chain, so
 * minified layers look as they do in WE, and the only way to mipmap a
 * compressed texture without decoding it (glGenerateMipmap on S3TC is an
 * error on GLES and driver-dependent on desktop GL). Each level is the
 * padded texture halved; it is cropped to the image's blocks like level
 * 0: level k of a w x h image is max(1, w >> k) x max(1, h >> k) texels
 * (the GL mip sizes) = that many 4x4 blocks rounded up, from the top-left.
 * Stops at the first level that does not parse: the chain may be shorter
 * than the file says, never wrong. */
static void tex_dxt_levels (WalpeTex *t, const guint8 *d, gsize len, gsize q, int ver,
                            guint32 mips, guint32 mw, guint32 mh)
{
    gsize bs = t->format == WALPE_TEX_DXT1 ? 8 : 16;
    gsize ent = ver == 1 ? 12 : 20;
    for (guint32 k = 1; k < mips && k <= WALPE_TEX_MAX_LEVELS; k++) {
        if (q + ent > len)
            return;
        guint32 lw = rd32le (d + q), lh = rd32le (d + q + 4);
        guint32 compressed = ver == 1 ? 0 : rd32le (d + q + 8);
        guint32 comp = ver == 1 ? rd32le (d + q + 8) : rd32le (d + q + 16);
        q += ent;
        if (comp > len - q)
            return;
        /* the padded texture halved, as GL halves (floor, at least 1) */
        if (lw != MAX (1u, mw >> k) || lh != MAX (1u, mh >> k))
            return;
        gsize sbw = ((gsize) lw + 3) / 4, sbh = ((gsize) lh + 3) / 4;
        gsize expect = sbw * sbh * bs;
        int gw = MAX (1, t->width >> k), gh = MAX (1, t->height >> k);
        gsize dbw = ((gsize) gw + 3) / 4, dbh = ((gsize) gh + 3) / 4;
        if (dbw > sbw || dbh > sbh)
            return;
        guint8 *raw = NULL;
        const guint8 *src = d + q;
        if (compressed) {
            raw = expect <= G_MAXINT && comp <= G_MAXINT ? g_try_malloc (expect) : NULL;
            if (!raw || LZ4_decompress_safe ((const char *) src, (char *) raw, (int) comp,
                                             (int) expect) != (int) expect) {
                g_free (raw);
                return;
            }
            src = raw;
        } else if (comp < expect) {
            return;
        }
        guint8 *out = g_try_malloc (dbw * dbh * bs);
        if (!out) {
            g_free (raw);
            return;
        }
        for (gsize by = 0; by < dbh; by++)
            memcpy (out + by * dbw * bs, src + by * sbw * bs, dbw * bs);
        g_free (raw);
        t->level[t->n_levels].w = gw;
        t->level[t->n_levels].h = gh;
        t->level[t->n_levels].data = out;
        t->level[t->n_levels].len = dbw * dbh * bs;
        t->n_levels++;
        q += comp;
        if (gw == 1 && gh == 1)
            return;
    }
}

gboolean walpe_tex_video_range (const char *path, guint64 *off, guint64 *len,
                                int *w, int *h)
{
    guint8 d[512];
    FILE *f = fopen (path, "rb");
    if (!f)
        return FALSE;
    gsize n = fread (d, 1, sizeof (d), f);
    gboolean ok = fseek (f, 0, SEEK_END) == 0;
    long flen = ok ? ftell (f) : -1;
    fclose (f);
    if (n < OFF_CONTAINER + 40 || flen <= 0 || memcmp (d, "TEXV", 4) != 0)
        return FALSE;
    int ver = tex_container_ver (d, n);
    guint32 mw, mh, compressed, unc, comp;
    gsize doff;
    if (!ver || !tex_mip0 (d, n, ver, &mw, &mh, &compressed, &unc, &comp, &doff))
        return FALSE;
    /* the payload is the MP4 itself: "....ftyp" (flag 0x20 is the WE
     * marker; the box magic is what makes it decodable) */
    if (compressed || doff + 8 > n || memcmp (d + doff + 4, "ftyp", 4) != 0)
        return FALSE;
    if ((guint64) doff + comp > (guint64) flen || comp < 64)
        return FALSE;
    if (!(rd32le (d + OFF_FLAGS) & 0x20))
        g_debug ("tex %s: MP4 payload without the video flag", path);
    *off = doff;
    *len = comp;
    if (w)
        *w = (int) rd32le (d + OFF_IW);
    if (h)
        *h = (int) rd32le (d + OFF_IH);
    return TRUE;
}

WalpeTex *walpe_tex_load (const char *path)
{
    return walpe_tex_load_ex (path, 0);
}

static WalpeTex *tex_load_mem (const guint8 *d, gsize len, const char *path, guint flags,
                               int level);

/* LZ4_decompress_safe_partial stops at the requested size since 1.9.0 and
 * is free of the 1.9.0/1.9.1 over-read since 1.9.2; older libraries decode
 * the whole padded level as before */
#if defined (LZ4_VERSION_NUMBER) && LZ4_VERSION_NUMBER >= 10902
#define TEX_LZ4_PARTIAL 1
#else
#define TEX_LZ4_PARTIAL 0
#endif

/* keep the first dst_row bytes of each of `rows` rows that are src_row
 * bytes apart (dst_row <= src_row): the image inside its padded texture.
 * Done inside the buffer, which is then shrunk: a second buffer for the
 * crop meant padded level + crop alive together (64 + 32 MiB for a 4K
 * image in a 4096^2 texture). */
static void tex_crop_rows (WalpeTex *t, gsize src_row, gsize dst_row, gsize rows)
{
    if (dst_row < src_row)
        for (gsize y = 1; y < rows; y++)
            memmove (t->data + y * dst_row, t->data + y * src_row, dst_row);
    guint8 *shrunk = rows && dst_row ? g_try_realloc (t->data, rows * dst_row) : NULL;
    if (shrunk)
        t->data = shrunk;
}

/* every loader result says which level it is and what level 0 measures */
static WalpeTex *tex_set_base (WalpeTex *t)
{
    if (t && !t->base_width) {
        t->base_width = t->width;
        t->base_height = t->height;
    }
    return t;
}

WalpeTex *walpe_tex_load_ex (const char *path, guint flags)
{
    return walpe_tex_load_level (path, flags, 0);
}

WalpeTex *walpe_tex_load_level (const char *path, guint flags, int level)
{
    if (level < 0 || level > WALPE_TEX_MAX_LEVELS)
        return NULL;
    guint64 voff, vlen;
    if (walpe_tex_video_range (path, &voff, &vlen, NULL, NULL)) {
        /* never read a (possibly 4K, 100 MB) video into memory here: the
         * scene renderer streams these through the shared video decoder */
        g_debug ("tex %s: video texture (MP4 at %" G_GUINT64_FORMAT ")", path, voff);
        return NULL;
    }
    /* mapped, not read: a 4K texture file is 35-45 MB with its mip levels,
     * of which level 0 alone is decoded - a heap copy of the whole file
     * was alive next to the decoded pixels during every load */
    GMappedFile *map = g_mapped_file_new (path, FALSE, NULL);
    if (map) {
        const guint8 *d = (const guint8 *) g_mapped_file_get_contents (map);
        gsize len = g_mapped_file_get_length (map);
        WalpeTex *t = d && len >= 64 ? tex_load_mem (d, len, path, flags, level) : NULL;
        g_mapped_file_unref (map);
        return tex_set_base (t);
    }
    /* something that cannot be mapped: read it */
    gsize len = 0;
    guint8 *d = (guint8 *) walpe_read_file (path, &len);
    if (!d)
        return NULL;
    WalpeTex *t = len >= 64 ? tex_load_mem (d, len, path, flags, level) : NULL;
    g_free (d);
    return tex_set_base (t);
}

/* d[0..len) = the file (len >= 64), not owned and not kept. level > 0:
 * that stored mip level of a container, or NULL (tex.h) */
static WalpeTex *tex_load_mem (const guint8 *d, gsize len, const char *path, guint flags,
                               int level)
{
    /* sniff embedded raster image first - many WE .tex are just wrappers */
    if (memcmp (d, "\x89PNG", 4) == 0 || memcmp (d, "\xff\xd8\xff", 3) == 0 ||
        memcmp (d, "RIFF", 4) == 0)
        return level ? NULL : try_embedded_image (d, len);

    if (memcmp (d, "TEXV", 4) != 0) {
        if (level)
            return NULL;
        /* any other picture gdk-pixbuf reads (GIF = first frame, BMP, ...):
         * a file the user picked for a "scenetexture" option */
        WalpeTex *t = NULL;
        const char *kind = !memcmp (d, "GIF8", 4) ? "gif" : !memcmp (d, "BM", 2) ? "bmp"
                         : !memcmp (d, "II*\0", 4) || !memcmp (d, "MM\0*", 4) ? "tiff" : NULL;
        /* a GIF: only with every frame inside the picture its header
         * declares (gifcheck.h) - the size check of the loader below is on
         * that picture alone */
        if (kind && !walpe_gif_layout_ok (d, len, NULL))
            kind = NULL;
        /* a BMP: only with header sizes a BMP file has - its decoder
         * reserves buffers of whatever sizes the file states (util.h) */
        if (kind && !walpe_bmp_header_ok (d, len, len))
            kind = NULL;
        if (kind) {
            GdkPixbuf *pb = tex_pixbuf_from_data (d, len, kind);
            if (pb)
                t = walpe_tex_from_pixbuf (pb);
        }
        return t;
    }

    /* WE .tex layout (as the linux-wallpaperengine project describes it in
     * its docs/TEXTURE_FORMAT.md):
     *   "TEXV0005\0" "TEXI0001\0"            (18 bytes)
     *   u32 type      0=ARGB8888 4=DXT5 6=DXT3 7=DXT1 8=RG88 9=R8
     *   u32 flags     1=interpolation 2=clampUVs 4=isGif
     *   u32 full width, u32 full height      (padded atlas size)
     *   u32 image width, u32 image height    (real content size)
     *   u32 unknown
     *   "TEXB0003|2|1\0" container (9 bytes)
     *   TEXB0003: u32 unknown, u32 freeimage format
     *   u32 mips; per mip: u32 w, u32 h, u32 compressed (1 = LZ4),
     *             u32 unc size, u32 comp size, pixels (LZ4 when flagged) */
    /* the header's full width/height (offsets 26/30) are not used: they
     * read garbage on some writers; mip0 dims are the padded size */
    if (len < OFF_CONTAINER + 16) {
        g_debug ("tex %s: too short (%zu)", path, len);
        return NULL;
    }
    guint32 type = rd32le (d + OFF_TYPE);
    guint32 iw = rd32le (d + OFF_IW);
    guint32 ih = rd32le (d + OFF_IH);
    if (iw == 0 || ih == 0 || iw > 16384 || ih > 16384) {
        /* implausible: try embedded raster fallback */
        g_debug ("tex %s: implausible image dims %ux%u (type %u)", path, iw, ih, type);
        WalpeTex *t = level ? NULL : try_embedded_image (d, len);
        return t;
    }

    char container[9];
    memcpy (container, d + OFF_CONTAINER, 8);
    container[8] = 0;
    gboolean b3 = memcmp (container, "TEXB0003", 8) == 0;
    gboolean b2 = memcmp (container, "TEXB0002", 8) == 0;
    gboolean b1 = memcmp (container, "TEXB0001", 8) == 0;
    gboolean b4 = memcmp (container, "TEXB0004", 8) == 0;
    int ver = b3 ? 3 : b2 ? 2 : b1 ? 1 : b4 ? 4 : 0;
    if (ver == 0) {
        g_debug ("tex %s: unknown container %.8s", path, container);
        WalpeTex *t = level ? NULL : try_embedded_image (d, len);
        return t;
    }

    gsize p = OFF_CONTAINER + 9; /* past the container magic */
    /* u32s between the magic and the mip count: 0001/0002 = image count;
     * 0003 = image count + freeimage format; 0004 adds one more u32
     * (observed 0) — verified against file-size-exact samples */
    p += (ver == 3 || ver == 4) ? (ver == 3 ? 8 : 12) : 4;
    if (p + 4 > len) {
        g_debug ("tex %s: truncated before mips", path);
        return NULL;
    }
    guint32 mips = rd32le (d + p);
    p += 4;
    if (mips == 0 || mips > 16) {
        g_debug ("tex %s: bad mip count %u", path, mips);
        return NULL;
    }

    /* mip 0 entry: 0001 = [w][h][raw size]; 0002/0003 =
     * [w][h][compression][unc size][comp size] */
    gsize ent = ver == 1 ? 12 : 20;
    if (p + ent > len) {
        g_debug ("tex %s: truncated mip header", path);
        return NULL;
    }
    guint32 mw = rd32le (d + p);
    guint32 mh = rd32le (d + p + 4);
    guint32 compressed = 0;
    guint32 unc = 0;
    guint32 comp;
    if (ver == 1) {
        comp = rd32le (d + p + 8);   /* raw byte size */
        unc = comp;
    } else {
        compressed = rd32le (d + p + 8);
        unc = rd32le (d + p + 12);
        comp = rd32le (d + p + 16);
        if (compressed == 0)
            unc = comp;              /* size field holds the raw length */
    }
    p += ent;
    g_debug ("tex %s: ver=%d type=%u %ux%u mip0=%ux%u compressed=%u unc=%u comp=%u",
             path, ver, type, iw, ih, mw, mh, compressed, unc, comp);
    /* mip0 = padded texture size: validate before any size math (crafted
     * u32s must not drive allocations or the LZ4 capacity) */
    if (mw == 0 || mh == 0 || mw > 16384 || mh > 16384) {
        g_debug ("tex %s: implausible mip0 dims %ux%u", path, mw, mh);
        return NULL;
    }
    if (!tex_size_ok (mw, mh)) {
        g_warning ("texture %s: %u x %u pixels is not loaded (the limit is 8192 x 8192 pixels "
                   "in all)", path, mw, mh);
        return NULL;
    }
    if (mw < iw || mh < ih) {
        /* image larger than its own texture: corrupt header; never let the
         * upload read past the pixel buffer */
        g_debug ("tex %s: image %ux%u exceeds mip0 %ux%u, clamping", path, iw, ih, mw, mh);
        iw = MIN (iw, mw);
        ih = MIN (ih, mh);
        if (level)
            return NULL;
    }
    guint32 iw0 = iw, ih0 = ih;   /* the image at level 0 */
    if (level) {
        /* a stored smaller level: the padded texture halved `level` times
         * (floor, at least 1, as GL halves), the image in its top-left
         * corner. Only when the image halves exactly that often, so its
         * texels at this level average image texels alone (with
         * WALPE_TEX_LOAD_LEVEL_FLOOR: any size, the image floored like the
         * texture), and never for a sprite sheet (its frame rects are
         * level 0 pixels). */
        guint32 mw0 = mw, mh0 = mh;
        if ((guint32) level >= mips || (rd32le (d + OFF_FLAGS) & 4))
            return NULL;
        if (!(flags & WALPE_TEX_LOAD_LEVEL_FLOOR) &&
            ((iw & ((1u << level) - 1)) || (ih & ((1u << level) - 1))))
            return NULL;
        for (int k = 1; k <= level; k++) {
            gsize skip = comp ? comp : unc;
            if (skip > len - p || p + skip + ent > len)
                return NULL;
            p += skip;
            mw = rd32le (d + p);
            mh = rd32le (d + p + 4);
            if (ver == 1) {
                comp = rd32le (d + p + 8);
                unc = comp;
            } else {
                compressed = rd32le (d + p + 8);
                unc = rd32le (d + p + 12);
                comp = rd32le (d + p + 16);
                if (compressed == 0)
                    unc = comp;
            }
            p += ent;
        }
        if (mw != MAX (1u, mw0 >> level) || mh != MAX (1u, mh0 >> level))
            return NULL;
        iw = MAX (1u, iw >> level);
        ih = MAX (1u, ih >> level);
        if (iw > mw || ih > mh)
            return NULL;
        g_debug ("tex %s: level %d: %ux%u in %ux%u compressed=%u unc=%u comp=%u", path, level,
                 iw, ih, mw, mh, compressed, unc, comp);
    }

    WalpeTex *t = g_new0 (WalpeTex, 1);
    t->mip_level = level;
    t->base_width = (int) iw0;
    t->base_height = (int) ih0;
    t->width = (int) iw;
    t->height = (int) ih;
    t->full_width = (int) mw;
    t->full_height = (int) mh;
    int rawch = 0;       /* single/dual channel formats expand to BGRA */
    t->we_format = (int) type;
    switch (type) {
    case 0:
        t->format = WALPE_TEX_RGBA8;
        break;
    case 4:
        t->format = WALPE_TEX_DXT5;
        break;
    case 6:
        t->format = WALPE_TEX_DXT3;
        break;
    case 7:
        t->format = WALPE_TEX_DXT1;
        break;
    case 8:              /* RG88 */
        t->format = WALPE_TEX_RGBA8;
        rawch = 2;
        break;
    case 9:              /* R8 */
        t->format = WALPE_TEX_RGBA8;
        rawch = 1;
        break;
    default: /* BC7/float formats: try embedded raster */
        g_free (t);
        {
            WalpeTex *e = level ? NULL : try_embedded_image (d, len);
            return e;
        }
    }
    if (level && t->format != WALPE_TEX_RGBA8 && (flags & WALPE_TEX_LOAD_NATIVE_DXT)) {
        /* kept compressed, level 0 comes with its whole stored chain */
        g_free (t);
        return NULL;
    }

    /* expect = the whole padded level; need = its rows that hold the image
     * (the image is the top-left iw x ih of the mw x mh level). Only those
     * are decoded and kept: a 3840x2160 image in a 4096x4096 texture needs
     * 35 of the level's 64 MiB, and the crop below then moves rows inside
     * that buffer. */
    gsize expect, need;
    if (rawch) {
        expect = (gsize) mw * mh * rawch;
        need = (gsize) mw * ih * rawch;
    } else if (t->format == WALPE_TEX_RGBA8) {
        expect = (gsize) mw * mh * 4;
        need = (gsize) mw * ih * 4;
    } else {
        gsize bw = ((gsize) mw + 3) / 4, bh = ((gsize) mh + 3) / 4;
        gsize bs = t->format == WALPE_TEX_DXT1 ? 8 : 16;
        expect = bw * bh * bs;
        need = bw * (((gsize) ih + 3) / 4) * bs;
    }
    if (compressed && !TEX_LZ4_PARTIAL)
        need = expect;

    const guint8 *src = d + p;
    gsize srclen = comp ? comp : unc;
    if (p + srclen > len) {
        g_debug ("tex %s: mip data overruns file (need %zu, have %zu)",
                 path, p + srclen, len);
        g_free (t);
        return NULL;
    }

    /* mips are frequently stored as embedded PNG/JPEG (compression flag 0
     * with a 0 uncompressed size) — decode through gdk when the magic hits */
    /* Only an uncompressed payload can be a picture file, and only one
     * that starts with a whole PNG signature or a JPEG SOI + marker. Pixel
     * data can start like that too (two bytes FF D8 used to be enough: an
     * RGBA texture whose first pixel is #FFD8xx, an R8 mask, an LZ4 stream):
     * when it does not decode as a picture it is read as pixels below
     * instead of failing the texture. */
    if (!compressed && srclen > 8 &&
        (memcmp (src, "\x89PNG\r\n\x1a\n", 8) == 0 ||
         (src[0] == 0xff && src[1] == 0xd8 && src[2] == 0xff))) {
        if (level) {
            g_free (t);   /* a picture file per level: level 0 is the one decoded */
            return NULL;
        }
        WalpeTex *e = try_embedded_image (d, len);
        if (e) {
            g_free (t);
            if (rd32le (d + OFF_FLAGS) & 4)
                tex_parse_frames (e, d, len, ver, path);
            e->mips = (int) mips;
            e->tex_flags = rd32le (d + OFF_FLAGS);
            return e;
        }
        g_debug ("tex %s: payload starts like a picture but is none; reading it as pixels", path);
    }

    if (compressed) {
        /* compression=1 is LZ4 (linux-wallpaperengine TextureParser.cpp),
         * NOT zlib — zlib headers don't exist in these streams */
        t->data = expect <= G_MAXINT && srclen <= G_MAXINT ? g_try_malloc (need) : NULL;
        int bytes = -1;
        if (t->data && need == expect)
            bytes = LZ4_decompress_safe ((const char *) src, (char *) t->data, (int) srclen,
                                         (int) expect);
#if TEX_LZ4_PARTIAL
        else if (t->data)   /* stops after the image's rows */
            bytes = LZ4_decompress_safe_partial ((const char *) src, (char *) t->data,
                                                 (int) srclen, (int) need, (int) need);
#endif
        if (bytes != (int) need) {
            g_debug ("tex %s: lz4 decode short (%d of %zu)", path, bytes, need);
            g_free (t->data);
            g_free (t);
            return NULL;
        }
        t->len = need;
    } else {
        if (srclen < expect) {
            g_debug ("tex %s: raw mip short (got %zu of %zu)", path, srclen, expect);
            g_free (t);
            return NULL;
        }
        t->data = g_try_malloc (need);
        if (!t->data) {
            g_free (t);
            return NULL;
        }
        memcpy (t->data, src, need);
        t->len = need;
    }
    /* image size first: frame rects are in the (cropped) image's pixels */
    t->width = (int) iw;
    t->height = (int) ih;
    t->mips = (int) mips;
    t->tex_flags = rd32le (d + OFF_FLAGS);
    if (rd32le (d + OFF_FLAGS) & 4)
        tex_parse_frames (t, d, len, ver, path);
    if (t->format != WALPE_TEX_RGBA8 && (flags & WALPE_TEX_LOAD_NATIVE_DXT) && mips > 1)
        tex_dxt_levels (t, d, len, p + srclen, ver, mips, mw, mh);

    /* raw type 0 ("ARGB8888") pixels are R,G,B,A in memory (WE and
     * linux-wallpaperengine upload them as GL_RGBA): swap to the BGRA this
     * loader hands out (2929592935's purple collar was drawn pink) */
    if (type == 0 && !rawch) {
        for (gsize i = 0; i + 3 < t->len; i += 4) {
            guint8 r = t->data[i];
            t->data[i] = t->data[i + 2];
            t->data[i + 2] = r;
        }
    }

    /* expand R8/RG88 into BGRA with GL_RED/GL_RG semantics (what WE and
     * linux-wallpaperengine upload): R = channel 0, G = channel 1 (RG88)
     * or 0, B = 0, A = 255 - masks and flow maps sample .r/.g */
    if (rawch && (flags & WALPE_TEX_LOAD_WE_COLOR)) {
        /* WE ConvertTexture0Format (common_fragment.h): R8 = white with
         * alpha R, RG88 = luminance R with alpha G (straight alpha) */
        gsize npix = (gsize) mw * ih;   /* the rows kept above */
        guint8 *ex = g_try_malloc (npix * 4);
        if (!ex) {
            walpe_tex_free (t);
            return NULL;
        }
        for (gsize i = 0; i < npix; i++) {
            guint8 l = rawch == 2 ? t->data[i * 2] : 255;
            ex[i * 4 + 0] = l;
            ex[i * 4 + 1] = l;
            ex[i * 4 + 2] = l;
            ex[i * 4 + 3] = rawch == 2 ? t->data[i * 2 + 1] : t->data[i];
        }
        g_free (t->data);
        t->data = ex;
        t->len = npix * 4;
    } else if (rawch && (flags & WALPE_TEX_LOAD_NATIVE_RG)) {
        /* keep 1/2 bytes per pixel: uploaded as GL_R8/GL_RG8 with a
         * (R, G|0, 0, 1) swizzle = the same samples as the expansion */
        t->channels = rawch;
    } else if (rawch) {
        gsize npix = (gsize) mw * ih;   /* the rows kept above */
        guint8 *ex = g_try_malloc (npix * 4);
        if (!ex) {
            walpe_tex_free (t);
            return NULL;
        }
        for (gsize i = 0; i < npix; i++) {
            ex[i * 4 + 0] = 0;                                     /* B */
            ex[i * 4 + 1] = rawch == 2 ? t->data[i * rawch + 1] : 0; /* G */
            ex[i * 4 + 2] = t->data[i * rawch];                    /* R */
            ex[i * 4 + 3] = 255;
        }
        g_free (t->data);
        t->data = ex;
        t->len = npix * 4;
    }

    /* DXT kept compressed on request when the image is the whole mip or
     * a crop on 4x4 block boundaries (block rows are copied) */
    if (t->format != WALPE_TEX_RGBA8 && (flags & WALPE_TEX_LOAD_NATIVE_DXT) &&
        ((t->width == (int) mw && t->height == (int) mh) ||
         (t->width % 4 == 0 && t->height % 4 == 0))) {
        gsize bs = t->format == WALPE_TEX_DXT1 ? 8 : 16;
        gsize sbw = ((gsize) mw + 3) / 4, dbw = ((gsize) t->width + 3) / 4;
        gsize dbh = ((gsize) t->height + 3) / 4;
        if (dbw != sbw || dbh != ((gsize) mh + 3) / 4)
            tex_crop_rows (t, sbw * bs, dbw * bs, dbh);
        t->len = dbw * dbh * bs;
        t->full_width = t->width;
        t->full_height = t->height;
        return t;
    }
    tex_drop_levels (t);   /* decoded below: the chain comes from GL */

    /* DXT1/3/5 blocks -> BGRA so upload paths never see compressed data */
    if (t->format != WALPE_TEX_RGBA8) {
        /* the image's rows: its last block row may be cut (ih % 4) */
        guint8 *out = walpe_dxt_decode (t->data, mw, ih, t->format);
        g_free (t->data);
        t->data = out;
        if (!out) {
            g_free (t);
            return NULL;
        }
        t->len = (gsize) mw * ih * 4;
        t->format = WALPE_TEX_RGBA8;
    }

    /* padded texture (mip0 larger than the image, WE pads to powers of
     * two with the image at the top-left): crop to the image so every
     * upload path can use width x height with a tight stride. The texture
     * then IS the image, so its padded size == image size (consumers'
     * g_TextureNResolution becomes (w, h, w, h) - consistent with the
     * cropped data; reporting the padded size would skew WE's UV math) */
    if (t->width != (int) mw || t->height != (int) mh) {
        gsize bpp = t->channels == 1 || t->channels == 2 ? (gsize) t->channels : 4;
        gsize row = (gsize) t->width * bpp;
        tex_crop_rows (t, (gsize) mw * bpp, row, (gsize) t->height);
        t->len = row * t->height;
    }
    t->full_width = t->width;
    t->full_height = t->height;
    return t;
}

gboolean walpe_tex_picture_size (const char *path, int *w, int *h)
{
    GMappedFile *map = g_mapped_file_new (path, FALSE, NULL);
    if (!map)
        return FALSE;
    const guint8 *d = (const guint8 *) g_mapped_file_get_contents (map);
    gsize len = g_mapped_file_get_length (map);
    gboolean found = FALSE;
    /* the scan of try_embedded_image, with its rules: the same signatures,
     * each read by the decoder of its own format, and as few attempts. This
     * used to look for "\x89PNG", a JPEG start or "RIFF" on its own and let
     * gdk-pixbuf choose the decoder from the bytes that follow, as often as
     * the first 4 KiB held such a start: a container whose level 0 began
     * like a picture reached the Windows cursor decoders again through
     * "RIFF", and 600 JPEG starts in front of 8 MiB of zeros took 10 s
     * here before the texture was loaded at all. */
    if (d && len >= 64 && !memcmp (d, "TEXV", 4)) {
        int tries = 0;
        for (gsize off = 0; !found && off + 4 < len && off < 4096 && tries < TEX_EMBEDDED_TRIES;
             off++) {
            const char *kind = tex_embedded_kind (d + off, len - off);
            if (kind) {
                tries++;
                found = tex_picture_size_from_data (d + off, len - off, kind, w, h);
            }
        }
    }
    g_mapped_file_unref (map);
    return found;
}

gboolean walpe_tex_probe (const char *path, WalpeTexProbe *pr)
{
    memset (pr, 0, sizeof *pr);
    GMappedFile *map = g_mapped_file_new (path, FALSE, NULL);
    if (!map)
        return FALSE;
    /* mapped, but only the level headers are touched: a few pages */
    const guint8 *d = (const guint8 *) g_mapped_file_get_contents (map);
    gsize len = g_mapped_file_get_length (map);
    int ver = d && len >= 64 && !memcmp (d, "TEXV", 4) ? tex_container_ver (d, len) : 0;
    gsize p = ver ? tex_images_off (ver) : 0, ent = ver == 1 ? 12 : 20;
    guint32 mips = ver && p + 4 <= len ? rd32le (d + p) : 0;
    if (mips == 0 || mips > 16) {
        g_mapped_file_unref (map);
        return FALSE;
    }
    p += 4;
    guint32 type = rd32le (d + OFF_TYPE), hflags = rd32le (d + OFF_FLAGS);
    guint32 iw = rd32le (d + OFF_IW), ih = rd32le (d + OFF_IH);
    pr->width = (int) iw;
    pr->height = (int) ih;
    pr->we_format = (int) type;
    pr->tex_flags = hflags;
    pr->mips = (int) mips;
    gboolean known = type == 0 || type == 4 || type == 6 || type == 7 || type == 8 || type == 9;
    gboolean exact = TRUE;
    guint32 mw0 = 0, mh0 = 0;
    for (guint32 k = 0; k < mips && k <= WALPE_TEX_MAX_LEVELS; k++) {
        if (p + ent > len)
            break;
        guint32 lw = rd32le (d + p), lh = rd32le (d + p + 4);
        guint32 compressed = ver == 1 ? 0 : rd32le (d + p + 8);
        guint32 size = ver == 1 ? rd32le (d + p + 8) : rd32le (d + p + 16);
        p += ent;
        if (size > len - p)
            break;
        if (k == 0) {
            if (lw == 0 || lh == 0 || lw > TEX_MAX_DIM || lh > TEX_MAX_DIM || iw == 0 || ih == 0)
                break;
            mw0 = lw;
            mh0 = lh;
            pr->tex_width = (int) lw;
            pr->tex_height = (int) lh;
            pr->lz4 = compressed != 0;
            pr->picture = !compressed && size > 8 &&
                          (!memcmp (d + p, "\x89PNG\r\n\x1a\n", 8) ||
                           (d[p] == 0xff && d[p + 1] == 0xd8 && d[p + 2] == 0xff));
        } else {
            /* the rules of walpe_tex_load_level */
            gboolean pic = !compressed && size > 8 &&
                           (!memcmp (d + p, "\x89PNG\r\n\x1a\n", 8) ||
                            (d[p] == 0xff && d[p + 1] == 0xd8 && d[p + 2] == 0xff));
            if (!known || pic || (hflags & 4) || iw > mw0 || ih > mh0 ||
                lw != MAX (1u, mw0 >> k) || lh != MAX (1u, mh0 >> k))
                break;
            if ((iw & ((1u << k) - 1)) || (ih & ((1u << k) - 1)))
                exact = FALSE;   /* from here on only with WALPE_TEX_LOAD_LEVEL_FLOOR */
        }
        pr->level_bytes[k] = size;
        pr->levels_floor = (int) k + 1;
        if (exact)
            pr->levels = (int) k + 1;
        p += size;
    }
    if (hflags & WALPE_TEXF_VIDEO)
        pr->levels = pr->levels_floor = 0;   /* an MP4, not pixels */
    g_mapped_file_unref (map);
    return pr->tex_width > 0;
}

void walpe_tex_free (WalpeTex *t)
{
    if (!t)
        return;
    tex_drop_levels (t);
    g_free (t->frames);
    g_free (t->data);
    g_free (t);
}
