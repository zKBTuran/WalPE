/* pichead.c - see pichead.h */
#include "pichead.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------- signature */

const char *walpe_pichead_kind (const guint8 *d, gsize len)
{
    if (!d)
        return NULL;
    if (len >= 12 && !memcmp (d + 4, "ftyp", 4)) {
        /* The first four bytes are the box's length, and any four bytes
         * can stand there. glycin (2.2) knows a TGA file by its bytes 1
         * and 2, an icon or cursor by bytes 0 to 3, and ranks a TGA header
         * of two rules above "ftypavif": a length of 0x0A00 with a brand
         * list that names avif is a run-length TGA picture to it, of the
         * width and height that stand where an ISO file has its minor
         * version (seen: 3 MB of file, 9000 x 9000 pixels decoded for a
         * picture that was an AVIF without a size here). A brand list of
         * less than 256 bytes - 60 brands; the files have two to ten -
         * leaves bytes 0 to 2 zero, which none of those headers has. */
        if (d[0] || d[1] || d[2])
            return NULL;
        /* the major brand, or one of the compatible brands after it (the
         * box's own length, at most 128 bytes of it) */
        gsize box = ((gsize) d[0] << 24) | ((gsize) d[1] << 16) | ((gsize) d[2] << 8) | d[3];
        box = MIN (MIN (box, len), 128);
        for (gsize o = 8; o + 4 <= box; o += o == 8 ? 8 : 4)
            if (!memcmp (d + o, "avif", 4) || !memcmp (d + o, "avis", 4))
                return "avif";
        return NULL;
    }
    if (len >= 3 && !memcmp (d, "\xff\xd8\xff", 3))
        return "jpeg";
    if (len >= 8 && !memcmp (d, "\x89PNG\r\n\x1a\n", 8))
        return "png";
    if (len >= 6 && (!memcmp (d, "GIF87a", 6) || !memcmp (d, "GIF89a", 6)))
        return "gif";
    if (len >= 12 && !memcmp (d, "RIFF", 4) && !memcmp (d + 8, "WEBP", 4))
        return "webp";
    if (len >= 4 && (!memcmp (d, "II*\0", 4) || !memcmp (d, "MM\0*", 4)))
        return "tiff";
    if ((len >= 2 && d[0] == 0xff && d[1] == 0x0a) ||
        (len >= 12 && !memcmp (d, "\0\0\0\x0cJXL \r\n\x87\n", 12)))
        return "jxl";
    if (len >= 2 && d[0] == 'B' && d[1] == 'M')
        return "bmp";
    return NULL;
}

gboolean walpe_pichead_is_bigtiff (const guint8 *d, gsize len)
{
    return d && len >= 4 && (!memcmp (d, "II+\0", 4) || !memcmp (d, "MM\0+", 4));
}

gboolean walpe_pichead_exact (const char *kind)
{
    static const char *const exact[] = { "png", "jpeg", "gif", "bmp", "webp", "tga", NULL };
    return kind && g_strv_contains (exact, kind);
}

/* ------------------------------------------------------------------- TGA */

gboolean walpe_tga_head (const guint8 *d, gsize len, WalpeTga *t)
{
    if (!d || len < 18)
        return FALSE;
    const guint idlen = d[0], has_map = d[1], type = d[2], map_bpp = d[7], bpp = d[16];
    const guint map_n = (guint) d[5] | ((guint) d[6] << 8);
    const int w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8);
    const gboolean mapped = type == 1 || type == 9, colour = type == 2 || type == 10,
                   grey = type == 3 || type == 11;
    /* (15 bits: no depth gdk-pixbuf's own TGA decoder takes - it refuses
     * the file, as it did when the bytes 1 and 2 alone made a file a TGA
     * here - and one the decoder behind glycin shows) */
    if (!(mapped ? bpp == 8 : colour ? bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32
                                     : grey && (bpp == 8 || bpp == 16)))
        return FALSE;
    if (w < 1 || h < 1 || (d[17] & 0xc0))
        return FALSE;
    if (mapped && !has_map)
        return FALSE;
    if (has_map && map_n && map_bpp != 15 && map_bpp != 16 && map_bpp != 24 && map_bpp != 32)
        return FALSE;
    if (!t)
        return TRUE;
    memset (t, 0, sizeof *t);
    t->w = w;
    t->h = h;
    memcpy (t->head, d, 18);
    if (mapped) {
        t->head[1] = 1;
    } else {
        t->head[1] = 0;
        memset (t->head + 3, 0, 5);
        if (has_map) {
            t->skip_off = 18 + (gsize) idlen;
            t->skip_len = (gsize) map_n * ((map_bpp + 7) >> 3);
        }
    }
    t->changed = t->skip_len > 0 || memcmp (t->head, d, 18) != 0;
    return TRUE;
}

/* ------------------------------------------------------------------ size */

/* the bytes: a buffer, or a file read where asked */
typedef struct {
    const guint8 *d;
    gsize len;
    FILE *f;
} Src;

static gboolean src_read (Src *s, guint64 off, guint8 *out, gsize n)
{
    if (s->f) {
        if (off > (guint64) G_MAXINT64 - n || fseeko (s->f, (off_t) off, SEEK_SET) != 0)
            return FALSE;
        return fread (out, 1, n, s->f) == n;
    }
    if (off > s->len || n > s->len - off)
        return FALSE;
    memcpy (out, s->d + off, n);
    return TRUE;
}

static guint32 be32 (const guint8 *p)
{
    return ((guint32) p[0] << 24) | ((guint32) p[1] << 16) | ((guint32) p[2] << 8) | p[3];
}

static guint32 le32 (const guint8 *p)
{
    return (guint32) p[0] | ((guint32) p[1] << 8) | ((guint32) p[2] << 16) | ((guint32) p[3] << 24);
}

static gboolean dims_set (guint64 w, guint64 h, int *ow, int *oh)
{
    if (w < 1 || h < 1 || w > G_MAXINT || h > G_MAXINT)
        return FALSE;
    *ow = (int) w;
    *oh = (int) h;
    return TRUE;
}

/* PNG: the header chunk is the first one, 13 bytes */
static gboolean png_size (Src *s, int *w, int *h)
{
    guint8 b[24];
    if (!src_read (s, 0, b, 24) || be32 (b + 8) != 13 || memcmp (b + 12, "IHDR", 4))
        return FALSE;
    return dims_set (be32 (b + 16), be32 (b + 20), w, h);
}

/* GIF: the logical screen (walpe_gif_layout_ok holds every frame to it) */
static gboolean gif_size (Src *s, int *w, int *h)
{
    guint8 b[10];
    if (!src_read (s, 0, b, 10))
        return FALSE;
    return dims_set (b[6] | ((guint) b[7] << 8), b[8] | ((guint) b[9] << 8), w, h);
}

/* BMP: 16-bit fields in the 12-byte info header, signed 32-bit ones in the
 * others (a negative height: rows from the top) */
static gboolean bmp_size (Src *s, int *w, int *h)
{
    guint8 b[26];
    if (!src_read (s, 0, b, 22))
        return FALSE;
    if (le32 (b + 14) == 12)
        return dims_set (b[18] | ((guint) b[19] << 8), b[20] | ((guint) b[21] << 8), w, h);
    if (!src_read (s, 0, b, 26))
        return FALSE;
    const gint32 bw = (gint32) le32 (b + 18), bh = (gint32) le32 (b + 22);
    if (bw < 1 || bh == 0 || bh == G_MININT32)
        return FALSE;
    return dims_set ((guint64) bw, (guint64) (bh < 0 ? -bh : bh), w, h);
}

static gboolean tga_size (Src *s, int *w, int *h)
{
    guint8 b[18];
    WalpeTga t;
    if (!src_read (s, 0, b, 18) || !walpe_tga_head (b, 18, &t))
        return FALSE;
    return dims_set ((guint64) t.w, (guint64) t.h, w, h);
}

/* WebP: the first chunk is the picture ("VP8 ", "VP8L") or the extended
 * header with the canvas ("VP8X") */
static gboolean webp_size (Src *s, int *w, int *h)
{
    guint8 b[30];
    if (!src_read (s, 0, b, 25))
        return FALSE;
    if (!memcmp (b + 12, "VP8X", 4)) {
        if (!src_read (s, 0, b, 30))
            return FALSE;
        return dims_set (1 + (b[24] | ((guint64) b[25] << 8) | ((guint64) b[26] << 16)),
                         1 + (b[27] | ((guint64) b[28] << 8) | ((guint64) b[29] << 16)), w, h);
    }
    if (!memcmp (b + 12, "VP8L", 4)) {
        if (b[20] != 0x2f)
            return FALSE;
        const guint32 v = le32 (b + 21);
        return dims_set (1 + (v & 0x3fff), 1 + ((v >> 14) & 0x3fff), w, h);
    }
    if (!memcmp (b + 12, "VP8 ", 4)) {
        /* a key frame: its start code, then 14 bits each */
        if (!src_read (s, 0, b, 30) || (b[20] & 1) || b[23] != 0x9d || b[24] != 0x01 || b[25] != 0x2a)
            return FALSE;
        return dims_set ((b[26] | ((guint) b[27] << 8)) & 0x3fff, (b[28] | ((guint) b[29] << 8)) & 0x3fff,
                         w, h);
    }
    return FALSE;
}

/* JPEG: the frame header, found the way libjpeg's marker reader walks
 * (util.c, jpeg_coding) - on a walk that zune-jpeg, the decoder behind
 * glycin, makes the same way. The two differ: to libjpeg a restart marker
 * or TEM in front of the first scan is a marker without a segment, to
 * zune-jpeg one with a length that is skipped; a frame header of a kind
 * zune-jpeg does not decode (lossless, hierarchical, arithmetic: all but
 * C0, C1, C2) is a frame header to libjpeg and a segment to skip to
 * zune-jpeg. A small frame header behind FF D0 00 0F was the size here and
 * skipped there, and the picture decoded was the one of the next frame
 * header (9000 x 9000 for "16 x 16"). So:
 *   - RSTn and TEM before the first scan: no answer;
 *   - the walk goes on to the first scan, every segment by its length,
 *     and a second frame header of any kind on the way: no answer
 *     (libjpeg refuses such a file as well, "duplicate SOF").
 * What is left is read alike by both: fill bytes, bytes between segments,
 * the segments by their lengths (zune-jpeg stops at a length below 2 and
 * at DNL and DAC, where this goes on as libjpeg does).
 * 0: found; 1: there is none to find (the end, a marker the walk does not
 * know, one of the cases above); 2: the bytes end before the answer */
static int jpeg_frame (const guint8 *d, gsize len, int *w, int *h)
{
    if (len < 2)
        return 2;
    if (d[0] != 0xFF || d[1] != 0xD8)
        return 1;
    gsize i = 2;
    gboolean have = FALSE;
    int fw = 0, fh = 0;
    for (;;) {
        guint8 m;
        for (;;) {
            while (i < len && d[i] != 0xFF)
                i++;
            while (i < len && d[i] == 0xFF)
                i++;
            if (i >= len)
                return 2;
            m = d[i++];
            if (m)
                break;
        }
        if (m == 0xDA) {
            if (!have)
                return 1;
            *w = fw;
            *h = fh;
            return 0;
        }
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (have)
                return 1;
            if (len - i < 7)
                return 2;
            if (!dims_set (((guint) d[i + 5] << 8) | d[i + 6], ((guint) d[i + 3] << 8) | d[i + 4], &fw, &fh))
                return 1;
            have = TRUE;
        } else if (m != 0xC4 && m != 0xCC && m != 0xDB && m != 0xDD && m != 0xDC && m != 0xFE &&
                   !(m >= 0xE0 && m <= 0xEF)) {
            return 1;       /* (0x01, D0 to D7, D9 and every marker without a known segment) */
        }
        if (len - i < 2)
            return 2;
        gsize seg = ((gsize) d[i] << 8) | d[i + 1];
        if (seg < 2)
            seg = 2;
        if (seg > len - i)
            return 2;
        i += seg;
    }
}

static gboolean jpeg_size (Src *s, int *w, int *h)
{
    if (!s->f)
        return jpeg_frame (s->d, s->len, w, h) == 0;
    /* the frame header and the first scan's start come behind the tables
     * and the application segments: read in growing steps, at most 32 MiB
     * (as util.c does for the same file) */
    const gsize head_max = (gsize) 32 << 20;
    GByteArray *buf = g_byte_array_new ();
    gsize want = 64 * 1024;
    int r = 2;
    if (fseeko (s->f, 0, SEEK_SET) == 0) {
        for (;;) {
            gsize old = buf->len;
            g_byte_array_set_size (buf, (guint) want);
            gsize got = fread (buf->data + old, 1, want - old, s->f);
            g_byte_array_set_size (buf, (guint) (old + got));
            r = jpeg_frame (buf->data, buf->len, w, h);
            if (r != 2 || got < want - old || want >= head_max)
                break;
            want *= 2;
        }
    }
    g_byte_array_unref (buf);
    return r == 0;
}

/* TIFF, classic and 64-bit: ImageWidth and ImageLength of the first
 * directory. A tag that is there twice counts with its larger value:
 * readers differ in which of the two they keep. The value in every whole-
 * number type one of the decoders takes for these tags - libtiff: BYTE,
 * SHORT, LONG, LONG8 and their signed kinds; the tiff crate behind glycin:
 * BYTE, SHORT, LONG, LONG8, IFD, IFD8 (a width written as type IFD was no
 * size here and 9000 x 9000 pixels there) - with a count of 1, which both
 * ask for. */
static gboolean tiff_size (Src *s, int *w, int *h)
{
    guint8 b[16];
    if (!src_read (s, 0, b, 8))
        return FALSE;
    const gboolean le = b[0] == 'I';
    #define T16(p) (le ? (guint) (p)[0] | ((guint) (p)[1] << 8) : ((guint) (p)[0] << 8) | (p)[1])
    #define T32(p) (le ? le32 (p) : be32 (p))
    #define T64(p) (le ? ((guint64) le32 ((p) + 4) << 32) | le32 (p) : ((guint64) be32 (p) << 32) | be32 ((p) + 4))
    gboolean big = FALSE;
    guint64 dir, n;
    if (!memcmp (b, "II*\0", 4) || !memcmp (b, "MM\0*", 4)) {
        dir = T32 (b + 4);
        if (!src_read (s, dir, b, 2))
            return FALSE;
        n = T16 (b);
        dir += 2;
    } else if (walpe_pichead_is_bigtiff (b, 8)) {
        /* 8-byte offsets (the header says so), then the directory's place */
        if (T16 (b + 4) != 8 || T16 (b + 6) != 0 || !src_read (s, 8, b, 8))
            return FALSE;
        dir = T64 (b);
        if (!src_read (s, dir, b, 8))
            return FALSE;
        n = T64 (b);
        if (dir > G_MAXUINT64 - 8 || n > 4096)     /* (libtiff's own limit on a directory) */
            return FALSE;
        dir += 8;
        big = TRUE;
    } else {
        return FALSE;
    }
    const gsize esz = big ? 20 : 12, vo = big ? 12 : 8;
    guint8 *e = g_malloc ((gsize) n * esz + 1);
    guint64 tw = 0, th = 0;
    gboolean ok = src_read (s, dir, e, (gsize) n * esz);
    for (guint i = 0; ok && i < n; i++) {
        const guint8 *p = e + (gsize) i * esz;
        const guint tag = T16 (p), type = T16 (p + 2);
        if (tag != 256 && tag != 257)
            continue;
        guint64 v = 0;
        guint8 far[8];
        const guint8 *at = p + vo;
        if ((big ? T64 (p + 4) : T32 (p + 4)) != 1) {
            ok = FALSE;
            break;
        }
        switch (type) {
        case 1:                             /* BYTE */
            v = at[0];
            break;
        case 6:                             /* SBYTE */
            v = at[0];
            ok = v < 0x80;
            break;
        case 3:                             /* SHORT */
            v = T16 (at);
            break;
        case 8:                             /* SSHORT */
            v = T16 (at);
            ok = v < 0x8000;
            break;
        case 4: case 13:                    /* LONG, IFD */
            v = T32 (at);
            break;
        case 9:                             /* SLONG */
            v = T32 (at);
            ok = v < 0x80000000u;
            break;
        case 16: case 17: case 18:          /* LONG8, SLONG8, IFD8: in the entry of a 64-bit file, else where it points */
            if (!big) {
                ok = src_read (s, T32 (at), far, 8);
                at = far;
            }
            if (ok)
                v = T64 (at);
            break;                          /* (a negative SLONG8 is far past every limit) */
        default:
            ok = FALSE;
        }
        if (ok && tag == 256)
            tw = MAX (tw, v);
        else if (ok)
            th = MAX (th, v);
    }
    #undef T16
    #undef T32
    #undef T64
    g_free (e);
    return ok && dims_set (tw, th, w, h);
}

/* ISO media boxes: one header at off. FALSE: none there. *body = where
 * its content starts, *end = where the box ends (limit: its container's) */
static gboolean box_at (Src *s, guint64 off, guint64 limit, char type[4], guint64 *body, guint64 *end)
{
    guint8 b[16];
    if (off > limit || limit - off < 8 || !src_read (s, off, b, 8))
        return FALSE;
    guint64 size = be32 (b), hdr = 8;
    memcpy (type, b + 4, 4);
    if (size == 1) {
        if (limit - off < 16 || !src_read (s, off + 8, b + 8, 8))
            return FALSE;
        size = ((guint64) be32 (b + 8) << 32) | be32 (b + 12);
        hdr = 16;
    } else if (size == 0) {
        size = limit - off;
    }
    if (size < hdr || size > limit - off)
        return FALSE;
    *body = off + hdr;
    *end = off + size;
    return TRUE;
}

/* the child box of that type between from and to */
static gboolean box_find (Src *s, guint64 from, guint64 to, const char *want, guint64 *body, guint64 *end)
{
    char type[4];
    for (int n = 0; n < 4096 && box_at (s, from, to, type, body, end); n++) {
        if (!memcmp (type, want, 4))
            return TRUE;
        from = *end;
    }
    return FALSE;
}

/* AVIF: the largest "ispe" (image spatial extents) among the item
 * properties: meta > iprp > ipco. A grid's canvas has one of its own.
 *
 * That bounds what libheif decodes for a picture kept as an item: it holds
 * an item's coded size to its "ispe" and a grid to its tiles before
 * anything is decoded (run with libheif 1.23: a 9000 x 9000 item under an
 * "ispe" of 64 x 64, a grid of 2 x 2 tiles that says 9000 x 9000 - both
 * refused at 17 to 22 MiB). It does not bound an image sequence: a file
 * with a "moov" box is played from its track, whose pictures are as large
 * as the AV1 data says - with every size the file declares (ispe, tkhd,
 * the sample entry) at 64 x 64, frames of 5000 x 5000 were decoded, and a
 * picture of 64 x 64 handed back. So a file with a "moov" box has no size
 * here, nor one without "ispe" (libheif decodes that one as well: seen),
 * nor one this walk does not see whole - more than 4096 boxes on a level,
 * or eight bytes and more at the end that are no box. */
static gboolean avif_size (Src *s, guint64 size, int *w, int *h)
{
    guint64 body, end, lim, from = 0, mbody = 0, mend = 0;
    char type[4];
    gboolean meta = FALSE;
    int n;
    for (n = 0; n < 4096 && box_at (s, from, size, type, &body, &end); n++, from = end) {
        if (!memcmp (type, "moov", 4))
            return FALSE;
        if (!meta && !memcmp (type, "meta", 4)) {
            meta = TRUE;
            mbody = body;
            mend = end;
        }
    }
    if (!meta || mend - mbody < 4 || size - from >= 8)
        return FALSE;
    lim = mend;
    if (!box_find (s, mbody + 4, lim, "iprp", &body, &end))
        return FALSE;
    lim = end;
    if (!box_find (s, body, lim, "ipco", &body, &end))
        return FALSE;
    lim = end;
    guint64 mw = 0, mh = 0;
    from = body;
    for (n = 0; n < 4096 && box_at (s, from, lim, type, &body, &end); n++, from = end) {
        guint8 b[12];
        if (memcmp (type, "ispe", 4))
            continue;
        if (end - body < 12 || !src_read (s, body, b, 12))
            return FALSE;
        mw = MAX (mw, be32 (b + 4));
        mh = MAX (mh, be32 (b + 8));
    }
    if (lim - from >= 8)
        return FALSE;
    return dims_set (mw, mh, w, h);
}

/* JPEG XL: the size header at the start of the codestream (bits from the
 * lowest of each byte on) */
static gboolean jxl_codestream_size (const guint8 *b, gsize n, int *w, int *h)
{
    if (n < 2 || b[0] != 0xff || b[1] != 0x0a)
        return FALSE;
    gsize pos = 16;
    gboolean ok = TRUE;
    #define BITS(k, out) do { \
            guint64 v_ = 0; \
            for (guint i_ = 0; i_ < (guint) (k); i_++, pos++) { \
                if (pos / 8 >= n) { ok = FALSE; break; } \
                v_ |= (guint64) ((b[pos / 8] >> (pos % 8)) & 1) << i_; \
            } \
            (out) = v_; \
        } while (0)
    static const guint sel_bits[4] = { 9, 13, 18, 30 };
    guint64 small, ys, xs = 0, ratio, sel;
    BITS (1, small);
    if (small) {
        BITS (5, ys);
        ys = (ys + 1) * 8;
    } else {
        BITS (2, sel);
        BITS (sel_bits[sel & 3], ys);
        ys += 1;
    }
    BITS (3, ratio);
    if (ratio == 0) {
        if (small) {
            BITS (5, xs);
            xs = (xs + 1) * 8;
        } else {
            BITS (2, sel);
            BITS (sel_bits[sel & 3], xs);
            xs += 1;
        }
    } else {
        static const guint num[8] = { 0, 1, 12, 4, 3, 16, 5, 2 }, den[8] = { 1, 1, 10, 3, 2, 9, 4, 1 };
        xs = ys * num[ratio] / den[ratio];
    }
    #undef BITS
    return ok && dims_set (xs, ys, w, h);
}

static gboolean jxl_size (Src *s, guint64 size, int *w, int *h)
{
    guint8 b[16];
    gsize n = 0;
    memset (b, 0, sizeof b);
    if (!src_read (s, 0, b, 2))
        return FALSE;
    if (b[0] == 0xff) {
        while (n < sizeof b && src_read (s, n, b + n, 1))
            n++;
        return jxl_codestream_size (b, n, w, h);
    }
    /* The container: the codestream is the "jxlc" box, or the content of
     * the "jxlp" boxes one after the other, each behind its 4-byte counter.
     * The parts can be cut anywhere - a first one of two bytes, the
     * signature alone, is as good as any to the decoder - so the first
     * bytes of the codestream are gathered across them (the bytes behind
     * a short first part are the next box's header, not the size). */
    guint64 body, end, from = 0;
    char type[4];
    for (int k = 0; k < 4096 && n < sizeof b && box_at (s, from, size, type, &body, &end); k++, from = end) {
        if (!memcmp (type, "jxlc", 4)) {
            if (n)
                return FALSE;       /* (both kinds in one file: not a file the format has) */
            while (n < sizeof b && body + n < end && src_read (s, body + n, b + n, 1))
                n++;
            break;
        }
        if (!memcmp (type, "jxlp", 4)) {
            if (end - body < 4)
                return FALSE;
            for (guint64 at = body + 4; n < sizeof b && at < end && src_read (s, at, b + n, 1); at++)
                n++;
        }
    }
    return jxl_codestream_size (b, n, w, h);
}

static gboolean src_size (Src *s, guint64 size, const char *kind, int *w, int *h)
{
    int tw = 0, th = 0;
    gboolean ok = FALSE;
    if (!kind)
        return FALSE;
    if (!strcmp (kind, "png"))
        ok = png_size (s, &tw, &th);
    else if (!strcmp (kind, "jpeg"))
        ok = jpeg_size (s, &tw, &th);
    else if (!strcmp (kind, "gif"))
        ok = gif_size (s, &tw, &th);
    else if (!strcmp (kind, "bmp"))
        ok = bmp_size (s, &tw, &th);
    else if (!strcmp (kind, "webp"))
        ok = webp_size (s, &tw, &th);
    else if (!strcmp (kind, "tga"))
        ok = tga_size (s, &tw, &th);
    else if (!strcmp (kind, "tiff"))
        ok = tiff_size (s, &tw, &th);
    else if (!strcmp (kind, "avif"))
        ok = avif_size (s, size, &tw, &th);
    else if (!strcmp (kind, "jxl"))
        ok = jxl_size (s, size, &tw, &th);
    if (ok) {
        *w = tw;
        *h = th;
    }
    return ok;
}

gboolean walpe_pichead_size (const char *kind, const guint8 *d, gsize len, int *w, int *h)
{
    Src s = { d, len, NULL };
    return d && src_size (&s, len, kind, w, h);
}

gboolean walpe_pichead_file_size (const char *kind, const char *path, int *w, int *h)
{
    GStatBuf st;
    FILE *f = path && g_stat (path, &st) == 0 && S_ISREG (st.st_mode) ? fopen (path, "rb") : NULL;
    if (!f)
        return FALSE;
    Src s = { NULL, 0, f };
    gboolean ok = src_size (&s, (guint64) st.st_size, kind, w, h);
    fclose (f);
    return ok;
}

/* ------------------------------------------------------------ whole file */

static gsize file_head (const char *path, guint8 *h, gsize max)
{
    GStatBuf st;
    FILE *f = path && g_stat (path, &st) == 0 && S_ISREG (st.st_mode) ? fopen (path, "rb") : NULL;
    if (!f)
        return 0;
    gsize n = fread (h, 1, max, f);
    fclose (f);
    return n;
}

static gboolean name_ends (const char *path, const char *ext)
{
    const gsize l = strlen (path), el = strlen (ext);
    return l > el && !g_ascii_strcasecmp (path + l - el, ext);
}

const char *walpe_pichead_file_kind (const char *path, WalpeTga *tga)
{
    guint8 h[132] = { 0 };
    const gsize n = file_head (path, h, sizeof h);
    if (!n)
        return NULL;
    const char *kind = walpe_pichead_kind (h, n);
    if (kind)
        return kind;
    if (n >= 12 && !memcmp (h + 4, "ftyp", 4))
        return NULL;        /* an ISO media file that is no AVIF */
    if (walpe_pichead_is_bigtiff (h, n))
        return name_ends (path, ".tif") || name_ends (path, ".tiff") ? "tiff" : NULL;
    if (walpe_tga_head (h, n, tga)) {
        const gboolean common = ((h[1] == 1 && (h[2] == 1 || h[2] == 9)) ||
                                 (h[1] == 0 && (h[2] == 2 || h[2] == 3 || h[2] == 10 || h[2] == 11))) &&
                                !(h[0] == 0 && h[1] == 0 && h[2] == 2 && h[3] == 0 && h[4] != 0 && h[5] == 0);
        if (common || name_ends (path, ".tga"))
            return "tga";
    }
    return NULL;
}

const char *walpe_pichead_file_other (const char *path)
{
    guint8 h[257] = { 0 };
    const gsize n = file_head (path, h, sizeof h - 1);
    if (n >= 12 && !memcmp (h + 4, "ftyp", 4))
        return "heif";
    if (n >= 4 && !memcmp (h, "\0\0\1\0", 4))
        return "ico";
    if (n >= 4 && !memcmp (h, "\0\0\2\0", 4))
        return "ico";       /* (a cursor: gdk-pixbuf's name for both) */
    if (n >= 12 && !memcmp (h, "RIFF", 4) && !memcmp (h + 8, "ACON", 4))
        return "ani";
    if (n >= 9 && !memcmp (h, "/* XPM */", 9))
        return "xpm";
    if (n >= 3 && h[0] == 'P' && h[1] >= '1' && h[1] <= '7' && g_ascii_isspace (h[2]))
        return "pnm";
    if (n >= 4 && !memcmp (h, "qoif", 4))
        return "qoi";
    if (n >= 4 && !memcmp (h, "DDS ", 4))
        return "dds";
    if (n >= 4 && !memcmp (h, "\x76\x2f\x31\x01", 4))
        return "exr";
    if (n >= 10 && !memcmp (h, "#?RADIANCE", 10))
        return "hdr";
    if ((n >= 12 && !memcmp (h, "\0\0\0\x0cjP  \r\n\x87\n", 12)) || (n >= 4 && !memcmp (h, "\xff\x4f\xff\x51", 4)))
        return "jp2";
    if (n >= 8 && !memcmp (h, "#define ", 8))
        return "xbm";
    /* an SVG document: its root element, or an XML or DOCTYPE start, in the
     * first bytes (text; a UTF-8 mark in front) */
    h[n] = 0;
    if (memchr (h, 0, n) == NULL &&
        (strstr ((const char *) h, "<svg") || strstr ((const char *) h, "<?xml") ||
         strstr ((const char *) h, "<!DOCTYPE svg")))
        return "svg";
    return NULL;
}
