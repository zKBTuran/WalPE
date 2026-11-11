/* mp4demux.c - minimal MP4 (ISO-BMFF) demuxer for H.264 video tracks.
 * Walks moov/trak/mdia/minf/stbl, keeps the raw run-length tables
 * (stts/stsc) and chunk offsets (stco/co64) plus per-sample sizes (stsz),
 * and expands everything ONCE after parsing — stbl child boxes may appear
 * in any order, so nothing depends on parse order. Extracts avcC (SPS/PPS)
 * from the avc1/avc3 sample entry. All parsing is bounds-checked: table
 * counts are clamped to what the box can actually hold and allocations use
 * g_try_* so a crafted file fails cleanly instead of aborting the daemon.
 * The file is mmapped read-only; samples point into the mapping, so only
 * the pages being decoded are resident (page cache, shared, reclaimable)
 * instead of a private heap copy of the whole video. */
#include "mp4demux.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    guint32 *sizes;         /* per-sample size (stsz) */
    guint64 *offsets;       /* per-chunk file offset (stco/co64) */
    guint nchunks;
    /* stts runs (expanded to per-sample deltas at open) */
    guint32 n_deltas_runs;
    guint32 *delta_counts;
    guint32 *delta_values;
    /* ctts runs (composition offsets for B-frames, expanded at open) */
    guint32 n_cts_runs;
    guint32 *cts_counts;
    gint64 *cts_values;
    /* stsc runs (expanded to per-chunk counts at open) */
    guint32 n_stsc_runs;
    guint32 *stsc_first;
    guint32 *stsc_spc;
    /* stss: sync samples (1-based sample numbers; no box = not known) */
    gboolean have_sync;
    guint32 nsync;
    guint32 *sync;
    guint32 nsamples;
    guint32 timescale;
    gboolean too_many;      /* stsz names more than MP4_MAX_SAMPLES samples */
} SampleTable;

/* The most samples (frames) a track may have: 19 hours at 60 frames per
 * second. Every sample costs 48 to 56 bytes of tables while the file is
 * opened and 40 afterwards, and a sample table with one fixed size needs
 * no bytes in the file at all: a count of 2^32 - 1 with a fixed size of 1
 * was only limited by the file's length. Measured: 8 MiB of zeros behind
 * a 291-byte moov (8 KB gzipped) opened as 8.4 million frames, 384 MiB of
 * tables; 100 MB of zeros would have been 4.8 GB. With the limit the
 * tables of one video stay under 240 MB however the file is made. */
#define MP4_MAX_SAMPLES (4u * 1024 * 1024)

typedef struct {
    const guint8 *buf;
    gsize len;
    guint8 *sps; gsize sps_len;
    guint8 *pps; gsize pps_len;
    int width, height;
    int rotation;               /* tkhd display matrix, see mp4demux.h */
    guint32 sar_n, sar_d;       /* pasp */
    guint32 timescale;
    double duration;
    SampleTable t;
    gboolean have_video;
} Ctx;

static guint32 rd32 (const guint8 *p) { return (guint32) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static guint64 rd64 (const guint8 *p) { return (guint64) rd32 (p) << 32 | rd32 (p + 4); }

/* box header at p[off..len): sets the type pointer and the body range.
 * Handles 64-bit largesize and size 0 (box runs to the end). FALSE when
 * the header or the box does not fit. */
static gboolean next_box (const guint8 *p, gsize len, gsize off,
                          const guint8 **type, gsize *body, gsize *blen, gsize *total)
{
    if (off > len || len - off < 8)
        return FALSE;
    guint64 sz = rd32 (p + off);
    gsize hdr = 8;
    if (sz == 1) {
        if (len - off < 16)
            return FALSE;
        sz = rd64 (p + off + 8);
        hdr = 16;
    } else if (sz == 0) {
        sz = len - off;
    }
    if (sz < hdr || sz > len - off)
        return FALSE;
    *type = p + off + 4;
    *body = off + hdr;
    *blen = (gsize) sz - hdr;
    *total = (gsize) sz;
    return TRUE;
}

static void table_free (SampleTable *t)
{
    g_free (t->sizes);
    g_free (t->offsets);
    g_free (t->delta_counts);
    g_free (t->delta_values);
    g_free (t->cts_counts);
    g_free (t->cts_values);
    g_free (t->stsc_first);
    g_free (t->stsc_spc);
    g_free (t->sync);
    memset (t, 0, sizeof (*t));
}

/* entry count clamped to what fits in the box body */
static guint32 clamp_count (guint32 n, gsize len, gsize hdr, gsize entry)
{
    gsize fit = len > hdr ? (len - hdr) / entry : 0;
    return n > fit ? (guint32) fit : n;
}

static void parse_avcc (Ctx *c, const guint8 *p, gsize len)
{
    /* avcC: version(1) profile(1) compat(1) level(1) lengthSizeMinusOne(1)
     * then numSPS + SPS records, numPPS + PPS records */
    if (len < 7)
        return;
    gsize off = 5;
    int n_sps = p[off] & 0x1f;
    off++;
    for (int i = 0; i < n_sps && off + 2 <= len; i++) {
        guint16 l = (guint16) p[off] << 8 | p[off + 1];
        off += 2;
        if (off + l > len)
            return;
        if (!c->sps) {
            c->sps = g_memdup2 (p + off, l);
            c->sps_len = l;
        }
        off += l;
    }
    if (off >= len)
        return;
    int n_pps = p[off] & 0x1f;
    off++;
    for (int i = 0; i < n_pps && off + 2 <= len; i++) {
        guint16 l = (guint16) p[off] << 8 | p[off + 1];
        off += 2;
        if (off + l > len)
            return;
        if (!c->pps) {
            c->pps = g_memdup2 (p + off, l);
            c->pps_len = l;
        }
        off += l;
    }
}

/* visual sample entry (avc1/avc3): child boxes hold avcC; some muxers
 * leave width/height zero here, so treat them as informational only */
static void parse_video_entry (Ctx *c, const guint8 *p, gsize len)
{
    if (len < 78)
        return;
    /* VisualSampleEntry: 16-bit width at +24, height at +26 */
    c->width = (int) (rd32 (p + 24) >> 16);
    c->height = (int) (rd32 (p + 24) & 0xffff);
    gsize off = 78, body, blen, total;
    const guint8 *type;
    while (next_box (p, len, off, &type, &body, &blen, &total)) {
        if (!memcmp (type, "avcC", 4)) {
            parse_avcc (c, p + body, blen);
        } else if (!memcmp (type, "pasp", 4) && blen >= 8) {
            c->sar_n = rd32 (p + body);
            c->sar_d = rd32 (p + body + 4);
        }
        off += total;
    }
}

static void parse_stsd (Ctx *c, const guint8 *p, gsize len)
{
    if (len < 8)
        return;
    guint32 n = rd32 (p + 4);
    gsize off = 8, body, blen, total;
    const guint8 *fmt;
    for (guint32 i = 0; i < n && next_box (p, len, off, &fmt, &body, &blen, &total); i++) {
        if (!memcmp (fmt, "avc1", 4) || !memcmp (fmt, "avc3", 4)) {
            c->have_video = TRUE;
            parse_video_entry (c, p + body, blen);
        }
        off += total;
    }
}

static void parse_stts (SampleTable *t, const guint8 *p, gsize len)
{
    if (len < 8)
        return;
    guint32 n = clamp_count (rd32 (p + 4), len, 8, 8);
    g_free (t->delta_counts); /* duplicate box: last one wins, no leak */
    g_free (t->delta_values);
    t->delta_counts = g_new0 (guint32, n ? n : 1);
    t->delta_values = g_new0 (guint32, n ? n : 1);
    gsize off = 8;
    for (guint32 i = 0; i < n; i++, off += 8) {
        t->delta_counts[i] = rd32 (p + off);
        t->delta_values[i] = rd32 (p + off + 4);
    }
    t->n_deltas_runs = n;
}

/* stss: the samples decoding can start at (keyframes: IDR pictures, and
 * with open GOPs the recovery points the muxer marked) */
static void parse_stss (SampleTable *t, const guint8 *p, gsize len)
{
    if (len < 8)
        return;
    guint32 n = clamp_count (rd32 (p + 4), len, 8, 4);
    g_free (t->sync);
    t->sync = g_new0 (guint32, n ? n : 1);
    gsize off = 8;
    for (guint32 i = 0; i < n; i++, off += 4)
        t->sync[i] = rd32 (p + off);
    t->nsync = n;
    t->have_sync = TRUE;
}

/* ctts: composition time offsets (pts = dts + offset); version 1 uses
 * signed offsets */
static void parse_ctts (SampleTable *t, const guint8 *p, gsize len)
{
    if (len < 8)
        return;
    guint32 version = rd32 (p) & 0xff;
    guint32 n = clamp_count (rd32 (p + 4), len, 8, 8);
    g_free (t->cts_counts);
    g_free (t->cts_values);
    t->cts_counts = g_new0 (guint32, n ? n : 1);
    t->cts_values = g_new0 (gint64, n ? n : 1);
    gsize off = 8;
    for (guint32 i = 0; i < n; i++, off += 8) {
        t->cts_counts[i] = rd32 (p + off);
        t->cts_values[i] = version == 1 ? (gint32) rd32 (p + off + 4) : (gint64) rd32 (p + off + 4);
    }
    t->n_cts_runs = n;
}

static void parse_stsc (SampleTable *t, const guint8 *p, gsize len)
{
    if (len < 8)
        return;
    guint32 n = clamp_count (rd32 (p + 4), len, 8, 12);
    g_free (t->stsc_first);
    g_free (t->stsc_spc);
    t->stsc_first = g_new0 (guint32, n ? n : 1);
    t->stsc_spc = g_new0 (guint32, n ? n : 1);
    gsize off = 8;
    for (guint32 i = 0; i < n; i++, off += 12) {
        t->stsc_first[i] = rd32 (p + off);
        t->stsc_spc[i] = rd32 (p + off + 4);
    }
    t->n_stsc_runs = n;
}

static void parse_stsz (SampleTable *t, const guint8 *p, gsize len, gsize file_len)
{
    if (len < 12)
        return;
    guint32 fixed = rd32 (p + 4);
    guint32 n = rd32 (p + 8);
    /* a fixed size can describe at most file_len / fixed samples; a
     * per-sample table at most what the box body holds */
    if (fixed != 0)
        n = (guint32) MIN ((guint64) n, (guint64) (file_len / fixed));
    else
        n = clamp_count (n, len, 12, 4);
    g_free (t->sizes);
    t->sizes = NULL;
    t->nsamples = 0;
    t->too_many = n > MP4_MAX_SAMPLES;
    if (t->too_many)
        return;     /* refused by mp4_demux_open_range, with a message */
    t->sizes = g_try_new0 (guint32, n ? n : 1);
    t->nsamples = t->sizes ? n : 0;
    if (!t->sizes)
        return;
    if (fixed != 0) {
        for (guint32 i = 0; i < n; i++)
            t->sizes[i] = fixed;
        return;
    }
    gsize off = 12;
    for (guint32 i = 0; i < n; i++, off += 4)
        t->sizes[i] = rd32 (p + off);
}

static void parse_stco (SampleTable *t, const guint8 *p, gsize len, gboolean co64)
{
    if (len < 8)
        return;
    gsize step = co64 ? 8 : 4;
    guint32 n = clamp_count (rd32 (p + 4), len, 8, step);
    g_free (t->offsets);
    t->nchunks = n;
    t->offsets = g_new0 (guint64, n ? n : 1);
    gsize off = 8;
    for (guint32 i = 0; i < n; i++, off += step)
        t->offsets[i] = co64 ? rd64 (p + off) : rd32 (p + off);
}

static void parse_stbl (Ctx *c, const guint8 *p, gsize len)
{
    SampleTable *t = &c->t;
    gsize off = 0, body, blen, total;
    const guint8 *type;
    gboolean video = FALSE;
    /* pass 1: is this the H.264 video sample table? */
    while (next_box (p, len, off, &type, &body, &blen, &total)) {
        if (!memcmp (type, "stsd", 4)) {
            parse_stsd (c, p + body, blen);
            video = c->have_video;
        }
        off += total;
    }
    if (!video)
        return;
    /* pass 2: raw tables, order-independent */
    off = 0;
    while (next_box (p, len, off, &type, &body, &blen, &total)) {
        if (!memcmp (type, "stts", 4))
            parse_stts (t, p + body, blen);
        else if (!memcmp (type, "ctts", 4))
            parse_ctts (t, p + body, blen);
        else if (!memcmp (type, "stsc", 4))
            parse_stsc (t, p + body, blen);
        else if (!memcmp (type, "stsz", 4))
            parse_stsz (t, p + body, blen, c->len);
        else if (!memcmp (type, "stco", 4))
            parse_stco (t, p + body, blen, FALSE);
        else if (!memcmp (type, "co64", 4))
            parse_stco (t, p + body, blen, TRUE);
        else if (!memcmp (type, "stss", 4))
            parse_stss (t, p + body, blen);
        off += total;
    }
}

/* tkhd: the 3x3 display matrix (16.16 fixed point; a b u / c d v / x y w).
 * Phone clips recorded in portrait are stored in landscape with a 90 or
 * 270 degree matrix. */
static void parse_tkhd (Ctx *c, const guint8 *b, gsize blen)
{
    /* an empty tkhd has no version byte: b[0] was read before the length
     * was looked at, one byte past the box - past the mapping when the box
     * was the last thing in a file that fills its last page */
    if (blen < 1)
        return;
    gsize m = b[0] == 1 ? 52 : 40;
    if (blen < m + 36)
        return;
    gint32 xa = (gint32) rd32 (b + m), xb = (gint32) rd32 (b + m + 4);
    gint32 xc = (gint32) rd32 (b + m + 12), xd = (gint32) rd32 (b + m + 16);
    if (!xa && !xb && !xc && !xd)
        c->rotation = 0;            /* an all-zero matrix: some muxers leave it empty */
    else if (xa > 0 && xd > 0 && !xb && !xc)
        c->rotation = 0;
    else if (!xa && !xd && xb > 0 && xc < 0)
        c->rotation = 90;
    else if (xa < 0 && xd < 0 && !xb && !xc)
        c->rotation = 180;
    else if (!xa && !xd && xb < 0 && xc > 0)
        c->rotation = 270;
    else
        c->rotation = -1;
}

/* recurse containers. each trak is parsed into its own scope so its mdhd
 * timescale pairs with its stbl; the H.264 video trak is merged into the
 * demuxer, everything else is dropped (wallpapers are silent) */
static void parse_box (Ctx *c, const guint8 *p, gsize len, int depth)
{
    if (depth > 6)
        return;
    gsize off = 0, body, blen, total;
    const guint8 *type;
    while (next_box (p, len, off, &type, &body, &blen, &total)) {
        if (!memcmp (type, "trak", 4)) {
            Ctx sub = { 0 };
            sub.buf = c->buf;
            sub.len = c->len;
            parse_box (&sub, p + body, blen, depth + 1);
            if (sub.have_video && !c->have_video) {
                c->have_video = TRUE;
                c->sps = g_steal_pointer (&sub.sps);
                c->sps_len = sub.sps_len;
                c->pps = g_steal_pointer (&sub.pps);
                c->pps_len = sub.pps_len;
                c->width = sub.width;
                c->height = sub.height;
                c->rotation = sub.rotation;
                c->sar_n = sub.sar_n;
                c->sar_d = sub.sar_d;
                c->timescale = sub.timescale;
                c->duration = sub.duration;
                table_free (&c->t);
                c->t = sub.t;
                memset (&sub.t, 0, sizeof (sub.t));
            } else {
                g_free (sub.sps);
                g_free (sub.pps);
                table_free (&sub.t);
            }
        } else if (!memcmp (type, "moov", 4) || !memcmp (type, "mdia", 4) ||
                   !memcmp (type, "minf", 4)) {
            parse_box (c, p + body, blen, depth + 1);
        } else if (!memcmp (type, "stbl", 4)) {
            parse_stbl (c, p + body, blen);
        } else if (!memcmp (type, "tkhd", 4)) {
            parse_tkhd (c, p + body, blen);
        } else if (!memcmp (type, "mdhd", 4) && blen >= 20 && !c->timescale) {
            const guint8 *b = p + body;
            guint8 ver = b[0];
            if (ver == 1 && blen >= 32) {
                c->timescale = rd32 (b + 20);
                if (c->timescale)
                    c->duration = (double) rd64 (b + 24) / c->timescale;
            } else if (ver == 0) {
                c->timescale = rd32 (b + 12);
                if (c->timescale)
                    c->duration = (double) rd32 (b + 16) / c->timescale;
            }
        }
        off += total;
    }
}

Mp4Demux *mp4_demux_open (const char *path, GError **err)
{
    return mp4_demux_open_range (path, 0, 0, err);
}

/* an MP4 stored inside another file (WE video .tex): box offsets (stco/
 * co64) are relative to the MP4's first byte, so the parse simply runs on
 * the [off, off+len) window of the mapping */
Mp4Demux *mp4_demux_open_range (const char *path, guint64 off, guint64 len, GError **err)
{
    Ctx c = { 0 };
    GError *me = NULL;
    /* a regular file only: the name can come from a wallpaper's
     * project.json, and opening a FIFO never returned (the monitor's
     * render thread hung for good): O_NONBLOCK, then fstat */
    int fd = g_open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
    struct stat sb;
    if (fd < 0 || fstat (fd, &sb) != 0 || !S_ISREG (sb.st_mode)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "cannot open %s: %s", path,
                     fd < 0 ? g_strerror (errno) : "not a regular file");
        if (fd >= 0)
            close (fd);
        return NULL;
    }
    /* the mapping serves the parse only (box headers and tables); it is
     * dropped before this function returns */
    GMappedFile *map = g_mapped_file_new_from_fd (fd, FALSE, &me);
    if (!map) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "cannot open %s: %s", path,
                     me ? me->message : "?");
        g_clear_error (&me);
        close (fd);
        return NULL;
    }
    c.buf = (const guint8 *) g_mapped_file_get_contents (map);
    c.len = g_mapped_file_get_length (map);
    guint64 range_off = off;    /* file position of the MP4's first byte */
    if (off || len) {
        if (!c.buf || off > c.len || len > c.len - off) {
            g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "%s: embedded MP4 range out of bounds", path);
            g_mapped_file_unref (map);
            close (fd);
            return NULL;
        }
        c.buf += off;
        c.len = len ? (gsize) len : c.len - (gsize) off;
    }
    if (!c.buf || c.len < 64) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s too small", path);
        g_mapped_file_unref (map);
        close (fd);
        return NULL;
    }

    Mp4Demux *d = NULL;
    guint32 *deltas = NULL, *chunk_samples = NULL;
    gint64 *cts = NULL;
    parse_box (&c, c.buf, c.len, 0);
    if (c.have_video && c.t.too_many) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "%s: the video track names more than %u frames", path, MP4_MAX_SAMPLES);
        goto fail;
    }
    /* the player hands the timescale to the decoder as an int and turns
     * seconds x timescale into 64-bit ticks: 2^31 and more is no real
     * file's (they use 600, 30000, 90000, ...) and is refused here, so
     * with at most MP4_MAX_SAMPLES samples of at most 2^32 - 1 ticks each
     * every time stamp of the track stays below 2^54 ticks */
    if (c.have_video && c.timescale > (guint32) G_MAXINT32) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "%s: unusable timescale %u", path, c.timescale);
        goto fail;
    }
    if (!c.have_video || !c.t.nsamples || !c.sps || !c.pps ||
        !c.t.nchunks || !c.t.n_deltas_runs) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "%s: no H.264 video track found", path);
        goto fail;
    }

    d = g_new0 (Mp4Demux, 1);
    d->sps = g_steal_pointer (&c.sps);
    d->sps_len = c.sps_len;
    d->pps = g_steal_pointer (&c.pps);
    d->pps_len = c.pps_len;
    d->width = c.width;
    d->height = c.height;
    d->rotation = c.rotation;
    d->sar_num = c.sar_n;
    d->sar_den = c.sar_d;
    d->timescale = c.timescale ? c.timescale : 1000;
    d->duration = c.duration;
    d->samples = g_try_new0 (Mp4Sample, c.t.nsamples);
    deltas = g_try_new0 (guint32, c.t.nsamples);
    cts = g_try_new0 (gint64, c.t.nsamples);
    chunk_samples = g_try_new0 (guint32, c.t.nchunks);
    if (!d->samples || !deltas || !cts || !chunk_samples) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "%s: sample tables too large (%u samples)", path, c.t.nsamples);
        goto fail;
    }

    /* expand stts runs -> per-sample deltas */
    guint32 total = 0;
    for (guint32 i = 0; i < c.t.n_deltas_runs && total < c.t.nsamples; i++)
        for (guint32 j = 0; j < c.t.delta_counts[i] && total < c.t.nsamples; j++)
            deltas[total++] = c.t.delta_values[i];
    for (guint32 i = total; i < c.t.nsamples; i++)
        deltas[i] = deltas[total ? total - 1 : 0]; /* extend last delta */

    /* expand ctts runs -> per-sample composition offsets (0 = none) */
    if (c.t.n_cts_runs > 0) {
        guint32 ct = 0;
        for (guint32 i = 0; i < c.t.n_cts_runs && ct < c.t.nsamples; i++)
            for (guint32 j = 0; j < c.t.cts_counts[i] && ct < c.t.nsamples; j++)
                cts[ct++] = c.t.cts_values[i];
        for (guint32 i = ct; i < c.t.nsamples; i++)
            cts[i] = cts[ct ? ct - 1 : 0];
    }

    /* expand stsc runs -> per-chunk sample counts */
    guint32 prev_first = 0, prev_spc = 0;
    gboolean have_run = FALSE;
    for (guint32 i = 0; i < c.t.n_stsc_runs; i++) {
        guint32 first = c.t.stsc_first[i];
        guint32 spc = c.t.stsc_spc[i];
        /* first_chunk must increase (ISO 14496-12); a crafted table that
         * jumps back and forth made this O(runs x chunks): a 1.5 MB file
         * kept the loader busy for minutes */
        if (first == 0 || (have_run && first - 1 <= prev_first))
            continue;
        have_run = TRUE;
        for (guint32 ch = prev_first; ch + 1 < first && ch < c.t.nchunks; ch++)
            chunk_samples[ch] = prev_spc;
        prev_first = first - 1;
        prev_spc = spc;
    }
    for (guint32 ch = prev_first; ch < c.t.nchunks; ch++)
        chunk_samples[ch] = prev_spc;

    /* walk chunks -> samples (offsets/sizes validated against the file:
     * written as off > len || sz > len - off so it cannot wrap) */
    guint si = 0;
    double pts = 0;
    guint32 tscale = c.timescale ? c.timescale : d->timescale;
    for (guint ch = 0; ch < c.t.nchunks && si < c.t.nsamples; ch++) {
        guint64 off = c.t.offsets[ch];
        for (guint k = 0; k < chunk_samples[ch] && si < c.t.nsamples; k++) {
            guint32 sz = c.t.sizes[si];
            if (off > c.len || sz > c.len - off)
                goto truncated;
            Mp4Sample *smp = &d->samples[si];
            smp->off = range_off + off;
            smp->size = sz;
            smp->pts = (double) pts / tscale;
            smp->cts_off = cts[si];
            pts += deltas[si];
            off += sz;
            si++;
        }
    }
truncated:
    d->nsamples = si;
    /* keyframes: the sync sample table when the file has one */
    if (c.t.have_sync) {
        d->has_sync = TRUE;
        for (guint32 i = 0; i < c.t.nsync; i++)
            if (c.t.sync[i] >= 1 && c.t.sync[i] <= si)
                d->samples[c.t.sync[i] - 1].sync = 1;
    }
    d->fd = fd;
    g_mapped_file_unref (map);
    g_free (deltas);
    g_free (cts);
    g_free (chunk_samples);
    table_free (&c.t);
    if (!d->nsamples) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s: no samples", path);
        mp4_demux_free (d);
        return NULL;
    }
    return d;

fail:
    g_free (deltas);
    g_free (cts);
    g_free (chunk_samples);
    if (d) {
        g_free (d->samples);
        g_free (d->sps);
        g_free (d->pps);
        g_free (d);
    }
    g_free (c.sps);
    g_free (c.pps);
    table_free (&c.t);
    g_mapped_file_unref (map);
    close (fd);
    return NULL;
}

const Mp4Sample *mp4_demux_next (Mp4Demux *d)
{
    if (d->cursor >= d->nsamples) {
        d->cursor = 0; /* loop; caller treats NULL as end-of-pass */
        return NULL;
    }
    Mp4Sample *smp = &d->samples[d->cursor];
    if (smp->size > d->pkt_cap) {
        gsize cap = MAX ((gsize) smp->size, d->pkt_cap + d->pkt_cap / 2);
        guint8 *nb = g_try_realloc (d->pkt, cap);
        if (!nb)
            goto unreadable;
        d->pkt = nb;
        d->pkt_cap = cap;
    }
    for (gsize got = 0; got < smp->size;) {
        ssize_t n = pread (d->fd, d->pkt + got, smp->size - got, (off_t) (smp->off + got));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto unreadable;    /* shortened or unreadable since it was opened */
        got += (gsize) n;
    }
    smp->data = d->pkt;
    d->cursor++;
    return smp;

unreadable:
    if (!d->io_failed)
        g_warning ("video: sample %u of %u can no longer be read (the file changed or went "
                   "away while it played)", d->cursor, d->nsamples);
    d->io_failed = TRUE;
    d->cursor = 0;      /* like the end of a pass: the caller starts over or stops */
    return NULL;
}

void mp4_demux_free (Mp4Demux *d)
{
    if (!d)
        return;
    g_free (d->samples);
    g_free (d->sps);
    g_free (d->pps);
    g_free (d->pkt);
    if (d->fd >= 0)
        close (d->fd);
    g_free (d);
}
