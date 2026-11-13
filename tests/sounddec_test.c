/* sounddec_test.c - the decoders of compressed sound files
 * (src/engine/sounddec.c, the part of walpe-video.so that scene sound uses;
 * compiled into this test).
 *
 * No sound is made: files are decoded into memory, and the one run through
 * a player uses the null device. Every file is made here.
 *
 *  1. Files that are no sound, or lie about themselves: refused with a
 *     reason, without a crash and without reading for ever - empty and
 *     short files, noise, megabytes of MP3 sync bytes, FLAC and Ogg
 *     headers that end early or point past the file, an Ogg packet above
 *     the size limit, ID3 tags that claim more than the file has, a
 *     directory, a FIFO.
 *  2. MP3 without an encoder: frames whose side information is all zero
 *     decode to silence. With them: every frame is delivered; a Xing /
 *     Info frame is not played; the delay and padding of a LAME tag are
 *     cut off (what makes an MP3 loop close without a gap).
 *  3. FLAC, written with FFmpeg's own encoder (skipped when the build has
 *     none): rate, channels, the exact number of frames and the samples
 *     themselves (FLAC is lossless).
 *  4. Ogg Vorbis and Ogg Opus, written with FFmpeg's encoders and an Ogg
 *     writer of the test's own (skipped without the encoders): the three
 *     Vorbis headers in a comment packet of megabytes, Opus pre-skip, the
 *     end position of the last page.
 *  5. A FLAC file through a player (scenesound.c) on the null device.
 *  6. A place in a FLAC file (walpe_sounddec_skip, what the sound track of
 *     a video wallpaper is entered with): the samples from any place on are
 *     the ones decoding from the start gives, to the end of the file; the
 *     file in front of a far place is not read; a file whose description
 *     does not say its length, or lies about its frames, and one with a
 *     hole in it are read as before, without a crash. */
#include <glib.h>
#include <glib/gstdio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "engine/scenesound.h"

static int fails, skipped;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); } } while (0)
#define WAIT(cond, ms) do { for (int w_ = 0; w_ < (ms) && !(cond); w_++) g_usleep (1000); } while (0)

static char *tmp;

static char *put_file (const char *name, const guint8 *data, gsize len)
{
    char *p = g_build_filename (tmp, name, NULL);
    g_file_set_contents (p, (const char *) data, (gssize) len, NULL);
    return p;
}

/* frames decoded from path, or -1 when the file is refused (*err then
 * says why). *pcm (optional): the samples, interleaved (g_free). */
static gint64 decode_all (const char *path, int *rate, int *ch, char **err, gint16 **pcm)
{
    *err = NULL;
    *rate = *ch = 0;
    if (pcm)
        *pcm = NULL;
    gpointer d = walpe_sounddec_open (path, rate, ch, err);
    if (!d)
        return -1;
    GArray *all = g_array_new (FALSE, FALSE, sizeof (gint16));
    gint16 buf[1024 * 2];
    gint64 total = 0;
    for (int guard = 0; guard < 4000000; guard++) {
        int n = walpe_sounddec_read (d, buf, 1024);
        if (n <= 0)
            break;
        total += n;
        if (pcm)
            g_array_append_vals (all, buf, (guint) (n * *ch));
    }
    walpe_sounddec_close (d);
    if (pcm)
        *pcm = (gint16 *) (gpointer) g_array_free (all, FALSE);
    else
        g_array_free (all, TRUE);
    return total;
}

static void refused (const char *what, const guint8 *data, gsize len)
{
    char *p = put_file ("hostile.bin", data, len), *err = NULL;
    int rate, ch;
    gint64 n = decode_all (p, &rate, &ch, &err, NULL);
    CHECK (n == -1 && err && *err, "%s: %" G_GINT64_FORMAT " frames, '%s'", what, n, err ? err : "(no reason)");
    g_free (err);
    g_remove (p);
    g_free (p);
}

static void put32le (guint8 *p, guint32 v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (guint8) (v >> 24); }
static void put32be (guint8 *p, guint32 v) { p[3] = v & 255; p[2] = (v >> 8) & 255; p[1] = (v >> 16) & 255; p[0] = (guint8) (v >> 24); }

/* one Ogg page that holds one whole packet (CRC left 0: not checked) */
static void ogg_page (GByteArray *o, int flags, guint64 granule, guint32 seq, const guint8 *data, gsize len)
{
    guint8 h[27] = { 'O', 'g', 'g', 'S', 0, (guint8) flags };
    put32le (h + 6, (guint32) granule);
    put32le (h + 10, (guint32) (granule >> 32));
    put32le (h + 14, 0x57414c50);
    put32le (h + 18, seq);
    gsize nseg = len / 255 + 1;
    g_assert (nseg <= 255);
    h[26] = (guint8) nseg;
    g_byte_array_append (o, h, 27);
    for (gsize i = 0; i + 1 < nseg; i++)
        g_byte_array_append (o, (const guint8 *) "\377", 1);
    guint8 last = (guint8) (len % 255);
    g_byte_array_append (o, &last, 1);
    if (len)
        g_byte_array_append (o, data, (guint) len);
}

/* refused, and for the reason that holds `why` */
static void refused_for (const char *what, const char *why, const guint8 *data, gsize len)
{
    char *p = put_file ("hostile.bin", data, len), *err = NULL;
    int rate, ch;
    gint64 n = decode_all (p, &rate, &ch, &err, NULL);
    CHECK (n == -1 && err && strstr (err, why), "%s: %" G_GINT64_FORMAT " frames, '%s' (expected: ... %s ...)", what, n,
           err ? err : "(no reason)", why);
    g_free (err);
    g_remove (p);
    g_free (p);
}

/* bits for a Vorbis setup header: the low bit of each byte first */
typedef struct { GByteArray *o; guint n; } BitW;
static void bit_put (BitW *w, guint32 v, guint bits)
{
    for (guint i = 0; i < bits; i++, w->n++) {
        if (!(w->n & 7))
            g_byte_array_append (w->o, (const guint8 *) "", 1);
        w->o->data[w->o->len - 1] |= (guint8) ((v >> i & 1) << (w->n & 7));
    }
}

/* ---- 1 ---- */
static void test_hostile (void)
{
    char *err = NULL;
    int rate, ch;
    refused ("empty file", (const guint8 *) "", 0);
    refused ("three bytes", (const guint8 *) "ID3", 3);
    {
        /* noise (the same every run) */
        GRand *r = g_rand_new_with_seed (12345);
        guint8 *n = g_malloc (65536);
        for (int i = 0; i < 65536; i++)
            n[i] = (guint8) g_rand_int_range (r, 0, 256);
        n[0] = 'x';   /* (not one of the magic numbers by chance) */
        refused ("64 KiB of noise", n, 65536);
        g_rand_free (r);
        g_free (n);
    }
    {
        /* MP3 sync bytes without end: read up to the probe limit, not further */
        gsize len = 6u << 20;
        guint8 *ff = g_malloc (len);
        memset (ff, 0xff, len);
        gint64 t0 = g_get_monotonic_time ();
        refused ("6 MiB of 0xff", ff, len);
        CHECK (g_get_monotonic_time () - t0 < 20 * G_USEC_PER_SEC, "6 MiB of 0xff took %.1f s",
               (g_get_monotonic_time () - t0) / 1e6);
        g_free (ff);
    }
    /* FLAC */
    refused ("fLaC and nothing else", (const guint8 *) "fLaC", 4);
    refused ("fLaC, a block that ends early", (const guint8 *) "fLaC\0\0\0\42abc", 11);
    refused ("fLaC, last block is padding: no stream description", (const guint8 *) "fLaC\201\0\0\4abcd", 12);
    {
        /* blocks that each point 16 MiB ahead: 1024 of them at most are followed */
        guint8 b[8] = { 'f', 'L', 'a', 'C', 1, 0xff, 0xff, 0xff };
        refused ("fLaC, block length past the file", b, 8);
    }
    {
        /* a stream description of the right size that is all zero */
        guint8 b[4 + 4 + 34 + 64] = { 'f', 'L', 'a', 'C', 0x80, 0, 0, 34 };
        refused ("fLaC, zeroed stream description, no frames", b, sizeof b);
    }
    /* Ogg */
    refused ("OggS and nothing else", (const guint8 *) "OggS", 4);
    {
        guint8 pg[27] = { 'O', 'g', 'g', 'S', 1 };   /* version 1 */
        refused ("Ogg page of an unknown version", pg, 27);
        guint8 seg[27 + 255] = { 'O', 'g', 'g', 'S', 0, 2 };
        seg[26] = 255;
        memset (seg + 27, 255, 255);
        refused ("Ogg page whose body is missing", seg, sizeof seg);
    }
    {
        GByteArray *o = g_byte_array_new ();
        ogg_page (o, 2, 0, 0, (const guint8 *) "\1vorbis and then nothing of a header at all", 43);
        refused ("Ogg Vorbis, the other headers missing", o->data, o->len);
        g_byte_array_set_size (o, 0);
        ogg_page (o, 2, 0, 0, (const guint8 *) "OpusHead", 8);
        refused ("Ogg Opus, header too short", o->data, o->len);
        g_byte_array_set_size (o, 0);
        ogg_page (o, 2, 0, 0, (const guint8 *) "\177FLAC in Ogg is not played by this build....", 43);
        refused ("Ogg with another codec", o->data, o->len);
        g_byte_array_set_size (o, 0);
        /* a first packet of 3 MiB, continued over pages of 255 full segments */
        guint8 h[27] = { 'O', 'g', 'g', 'S', 0, 2 };
        guint8 *body = g_malloc0 (255 * 255);
        guint8 lace[255];
        memset (lace, 255, 255);
        put32le (h + 14, 7);
        h[26] = 255;
        memcpy (body, "\1vorbis", 7);
        for (int i = 0; i < 48; i++) {
            h[5] = i ? 1 : 2;
            put32le (h + 18, (guint32) i);
            g_byte_array_append (o, h, 27);
            g_byte_array_append (o, lace, 255);
            g_byte_array_append (o, body, 255 * 255);
        }
        refused ("Ogg, a first packet of megabytes that never ends", o->data, o->len);
        g_free (body);
        g_byte_array_free (o, TRUE);
    }
    {
        /* Ogg Opus that states 255 channels in 255 streams (channel mapping
         * 255): refused when its header is read, no decoder is set up */
        GByteArray *o = g_byte_array_new ();
        guint8 head[21 + 255] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 255, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 255, 255, 0 };
        for (int i = 0; i < 255; i++)
            head[21 + i] = (guint8) i;
        static const guint8 tags[16] = { 'O', 'p', 'u', 's', 'T', 'a', 'g', 's' };
        ogg_page (o, 2, 0, 0, head, sizeof head);
        ogg_page (o, 0, 0, 1, tags, sizeof tags);
        refused_for ("Ogg Opus with 255 channels", "channel count", o->data, o->len);
        /* ... and one that makes two channels out of 200 streams */
        head[9] = 2;
        head[19] = 200;
        g_byte_array_set_size (o, 0);
        ogg_page (o, 2, 0, 0, head, 21 + 2);
        ogg_page (o, 0, 0, 1, tags, sizeof tags);
        refused_for ("Ogg Opus with 200 streams", "channel count", o->data, o->len);
        g_byte_array_free (o, TRUE);
    }
    {
        /* Ogg Vorbis whose setup header describes 256 code tables of 65536
         * entries and 16 dimensions in 5 KiB (code lengths written as one
         * run each): about a gigabyte inside the decoder. Refused before a
         * decoder sees it. */
        static const guint8 ident[30] = { 1, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 2, 0x44, 0xac, 0, 0,
                                          0, 0, 0, 0, 0, 0x77, 1, 0, 0, 0, 0, 0, 0xb8, 1 };
        static const guint8 comment[16] = { 3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        BitW w = { g_byte_array_new (), 0 };
        g_byte_array_append (w.o, (const guint8 *) "\5vorbis", 7);
        bit_put (&w, 255, 8);              /* 256 tables */
        for (int k = 0; k < 256; k++) {
            bit_put (&w, 0x564342, 24);    /* sync */
            bit_put (&w, 16, 16);          /* dimensions */
            bit_put (&w, 65536, 24);       /* entries */
            bit_put (&w, 1, 1);            /* lengths as runs */
            bit_put (&w, 15, 5);           /* the first length: 16 */
            bit_put (&w, 65536, 17);       /* all entries have it */
            bit_put (&w, 1, 4);            /* vectors from a value list */
            bit_put (&w, 0, 32);
            bit_put (&w, 0, 32);
            bit_put (&w, 0, 4);            /* one bit a value */
            bit_put (&w, 0, 1);
            bit_put (&w, 0, 2);            /* the two values (2 to the power 16 is 65536) */
        }
        GByteArray *o = g_byte_array_new ();
        ogg_page (o, 2, 0, 0, ident, sizeof ident);
        ogg_page (o, 0, 0, 1, comment, sizeof comment);
        ogg_page (o, 0, 0, 2, w.o->data, w.o->len);
        refused_for ("Ogg Vorbis with a gigabyte of code tables", "code tables", o->data, o->len);
        g_byte_array_free (o, TRUE);
        g_byte_array_free (w.o, TRUE);
    }
    {
        /* four ID3v2 tags, each of which says it is 256 MiB long */
        guint8 id3[10] = { 'I', 'D', '3', 4, 0, 0, 0x7f, 0x7f, 0x7f, 0x7f };
        refused ("ID3 tag larger than the file", id3, 10);
    }
    /* not files */
    gint64 n = decode_all (tmp, &rate, &ch, &err, NULL);
    CHECK (n == -1 && err, "a directory: %" G_GINT64_FORMAT, n);
    g_clear_pointer (&err, g_free);
    char *fifo = g_build_filename (tmp, "pipe.mp3", NULL);
    if (mkfifo (fifo, 0600) == 0) {
        n = decode_all (fifo, &rate, &ch, &err, NULL);
        CHECK (n == -1 && err, "a FIFO: %" G_GINT64_FORMAT, n);
        g_clear_pointer (&err, g_free);
        g_remove (fifo);
    }
    g_free (fifo);
    n = decode_all ("/nonexistent/x.mp3", &rate, &ch, &err, NULL);
    CHECK (n == -1 && err, "a missing file");
    g_clear_pointer (&err, g_free);
}

/* ---- 2 ---- */
#define MP3_FRAME 417   /* MPEG-1 layer III, 128 kbit/s, 44100 Hz, no padding */

/* nframes frames of silence; info: 0 = none, 1 = an "Info" frame with the
 * frame count in front, 2 = that with a LAME extension (delay, padding) */
static char *write_mp3 (const char *name, int nframes, int info, int delay, int padding, gboolean id3)
{
    GByteArray *o = g_byte_array_new ();
    guint8 fr[MP3_FRAME] = { 0xff, 0xfb, 0x90, 0x00 };   /* the rest 0: silence */
    if (id3) {
        guint8 tag[10 + 300] = { 'I', 'D', '3', 3, 0, 0, 0, 0, 2, 44 };   /* 300 bytes, syncsafe */
        g_byte_array_append (o, tag, sizeof tag);
    }
    if (info) {
        guint8 x[MP3_FRAME] = { 0xff, 0xfb, 0x90, 0x00 };
        guint8 *t = x + 4 + 32;   /* behind the side information of a stereo frame */
        memcpy (t, "Info", 4);
        put32be (t + 4, 1);       /* flags: the frame count follows */
        put32be (t + 8, (guint32) nframes);
        if (info == 2) {
            memcpy (t + 12, "LAME3.100", 9);
            const guint32 v = (guint32) delay << 12 | (guint32) padding;
            t[12 + 21] = (guint8) (v >> 16);
            t[12 + 22] = (guint8) (v >> 8);
            t[12 + 23] = (guint8) v;
        }
        g_byte_array_append (o, x, MP3_FRAME);
    }
    for (int i = 0; i < nframes; i++)
        g_byte_array_append (o, fr, MP3_FRAME);
    char *p = put_file (name, o->data, o->len);
    g_byte_array_free (o, TRUE);
    return p;
}

static void test_mp3 (void)
{
    static const struct { const char *what; int info, delay, padding; gboolean id3; gint64 want; } t[] = {
        { "no table frame", 0, 0, 0, FALSE, 40 * 1152 },
        { "behind an ID3 tag", 0, 0, 0, TRUE, 40 * 1152 },
        { "Info frame: not played", 1, 0, 0, FALSE, 40 * 1152 },
        { "LAME tag: delay 576, padding 1000", 2, 576, 1000, FALSE, 40 * 1152 - 576 - 1000 },
        { "LAME tag behind an ID3 tag", 2, 576, 1000, TRUE, 40 * 1152 - 576 - 1000 },
        { "LAME tag: delay 0, padding 0", 2, 0, 0, FALSE, 40 * 1152 - 529 },
        { "LAME tag: padding below the decoder's own delay", 2, 1105, 200, FALSE, 40 * 1152 - 1105 - 529 },
    };
    for (guint k = 0; k < G_N_ELEMENTS (t); k++) {
        char *p = write_mp3 ("silence.mp3", 40, t[k].info, t[k].delay, t[k].padding, t[k].id3), *err = NULL;
        int rate, ch;
        gint16 *pcm = NULL;
        gint64 n = decode_all (p, &rate, &ch, &err, &pcm);
        int loud = 0;
        for (gint64 i = 0; pcm && i < n * ch; i++)
            loud += pcm[i] != 0;
        CHECK (n == t[k].want && rate == 44100 && ch == 2 && !loud,
               "MP3, %s: %" G_GINT64_FORMAT " frames (want %" G_GINT64_FORMAT "), %d Hz, %d ch, %d samples not 0, '%s'",
               t[k].what, n, t[k].want, rate, ch, loud, err ? err : "");
        g_free (pcm);
        g_free (err);
        g_remove (p);
        g_free (p);
    }
}

/* ---- FFmpeg's encoders, for 3 and 4 ---- */
typedef struct {
    GByteArray *extra;
    GPtrArray *pkts;   /* GBytes */
} Enc;

static void enc_clear (Enc *e)
{
    if (e->extra)
        g_byte_array_free (e->extra, TRUE);
    if (e->pkts)
        g_ptr_array_free (e->pkts, TRUE);
    memset (e, 0, sizeof *e);
}

static void frame_layout (AVFrame *f, const AVCodecContext *c)
{
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT (57, 28, 100)
    av_channel_layout_copy (&f->ch_layout, &c->ch_layout);
#else
    f->channels = c->channels;
    f->channel_layout = c->channel_layout;
#endif
}

/* an encoder context that is open, or NULL */
static AVCodecContext *enc_open (const AVCodec *codec, enum AVSampleFormat fmt, int rate, int ch)
{
    AVCodecContext *c = avcodec_alloc_context3 (codec);
    if (!c)
        return NULL;
    c->sample_fmt = fmt;
    c->sample_rate = rate;
    c->time_base = (AVRational) { 1, rate };
    c->bit_rate = 96000;
    c->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;   /* FFmpeg's own Vorbis and Opus encoders */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT (57, 28, 100)
    av_channel_layout_default (&c->ch_layout, ch);
#else
    c->channels = ch;
    c->channel_layout = ch == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
#endif
    if (avcodec_open2 (c, codec, NULL) < 0)
        avcodec_free_context (&c);
    return c;
}

/* interleaved 16-bit samples -> the encoder's packets. FALSE: this FFmpeg
 * has no such encoder (or it refuses these parameters). */
static gboolean encode (enum AVCodecID id, enum AVSampleFormat fmt, int rate, int ch, const gint16 *pcm, int frames,
                        Enc *e)
{
    memset (e, 0, sizeof *e);
    const AVCodec *codec = avcodec_find_encoder (id);
    if (!codec)
        return FALSE;
    /* The wanted sample format, else packed float, else 16 bit: where the
     * build has libopus, avcodec_find_encoder returns that one (FFmpeg's
     * own Opus encoder is "experimental" and comes second), and libopus
     * takes no planar samples - the Opus part was then skipped as "no
     * encoder" on the very builds most people have. */
    static const enum AVSampleFormat other[] = { AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16 };
    AVCodecContext *c = enc_open (codec, fmt, rate, ch);
    for (guint i = 0; !c && i < G_N_ELEMENTS (other); i++)
        if (other[i] != fmt && (c = enc_open (codec, other[i], rate, ch)) != NULL)
            fmt = other[i];
    if (!c)
        return FALSE;
    e->extra = g_byte_array_new ();
    e->pkts = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
    if (c->extradata_size > 0)
        g_byte_array_append (e->extra, c->extradata, (guint) c->extradata_size);
    const int fs = c->frame_size > 0 ? c->frame_size : 1024;
    const gboolean small_last = (codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0 || c->frame_size <= 0;
    AVFrame *f = av_frame_alloc ();
    AVPacket *p = av_packet_alloc ();
    gboolean ok = f && p;
    /* (ends with the flush below: `pos <= frames` left the loop without it
     * whenever frames was not a multiple of the encoder's frame size) */
    for (int pos = 0; ok; pos += fs) {
        const gboolean flush = pos >= frames;
        if (!flush) {
            const int have = MIN (fs, frames - pos);
            f->nb_samples = small_last ? have : fs;   /* (the rest of a padded frame is silence) */
            f->format = fmt;
            f->sample_rate = rate;
            f->pts = pos;
            frame_layout (f, c);
            ok = av_frame_get_buffer (f, 0) >= 0;
            for (int i = 0; ok && i < f->nb_samples; i++)
                for (int k = 0; k < ch; k++) {
                    const gint16 v = i < have ? pcm[(pos + i) * ch + k] : 0;
                    if (fmt == AV_SAMPLE_FMT_S16)
                        ((gint16 *) (gpointer) f->data[0])[i * ch + k] = v;
                    else if (fmt == AV_SAMPLE_FMT_FLT)
                        ((float *) (gpointer) f->data[0])[i * ch + k] = v / 32768.0f;
                    else   /* AV_SAMPLE_FMT_FLTP */
                        ((float *) (gpointer) f->extended_data[k])[i] = v / 32768.0f;
                }
        }
        ok = ok && avcodec_send_frame (c, flush ? NULL : f) >= 0;
        av_frame_unref (f);
        while (ok && avcodec_receive_packet (c, p) == 0) {
            /* (the FLAC encoder answers the flush with a packet of no bytes
             * that only carries its final stream description) */
            if (p->size > 0)
                g_ptr_array_add (e->pkts, g_bytes_new (p->data, (gsize) p->size));
            av_packet_unref (p);
        }
        if (flush)
            break;
    }
    av_frame_free (&f);
    av_packet_free (&p);
    avcodec_free_context (&c);
    if (!ok || !e->pkts->len)
        enc_clear (e);
    return ok && e->pkts != NULL;
}

/* a second of a 440 Hz sine, both channels */
static gint16 *sine_pcm (int rate, int ch, int frames)
{
    gint16 *pcm = g_new (gint16, (gsize) frames * (gsize) ch);
    for (int i = 0; i < frames; i++)
        for (int k = 0; k < ch; k++)
            pcm[i * ch + k] = (gint16) lrint (12000.0 * sin (2.0 * G_PI * 440.0 * i / rate) * (k ? 0.5 : 1.0));
    return pcm;
}

/* ---- 3 ---- */
static char *write_flac (const char *name, const gint16 *pcm, int rate, int ch, int frames)
{
    Enc e;
    if (!encode (AV_CODEC_ID_FLAC, AV_SAMPLE_FMT_S16, rate, ch, pcm, frames, &e))
        return NULL;
    char *p = NULL;
    if (e.extra->len == 34) {
        GByteArray *o = g_byte_array_new ();
        g_byte_array_append (o, (const guint8 *) "fLaC\200\0\0\42", 8);   /* last block: STREAMINFO, 34 bytes */
        g_byte_array_append (o, e.extra->data, 34);
        for (guint i = 0; i < e.pkts->len; i++) {
            gsize n = 0;
            const guint8 *d = g_bytes_get_data (e.pkts->pdata[i], &n);
            g_byte_array_append (o, d, (guint) n);
        }
        p = put_file (name, o->data, o->len);
        g_byte_array_free (o, TRUE);
    }
    enc_clear (&e);
    return p;
}

static void test_flac (void)
{
    static const struct { int rate, ch, frames; } t[] = { { 44100, 2, 44100 }, { 48000, 1, 5000 }, { 8000, 2, 12345 } };
    for (guint k = 0; k < G_N_ELEMENTS (t); k++) {
        gint16 *pcm = sine_pcm (t[k].rate, t[k].ch, t[k].frames), *got = NULL;
        char *p = write_flac ("sine.flac", pcm, t[k].rate, t[k].ch, t[k].frames), *err = NULL;
        if (!p) {
            printf ("sounddec: SKIP FLAC (this FFmpeg has no FLAC encoder)\n");
            skipped++;
            g_free (pcm);
            return;
        }
        int rate, ch;
        gint64 n = decode_all (p, &rate, &ch, &err, &got);
        CHECK (n == t[k].frames && rate == t[k].rate && ch == t[k].ch, "FLAC %d Hz %d ch: %" G_GINT64_FORMAT
               " of %d frames, %d Hz, %d ch, '%s'", t[k].rate, t[k].ch, n, t[k].frames, rate, ch, err ? err : "");
        CHECK (n == t[k].frames && got && !memcmp (got, pcm, (gsize) n * (gsize) ch * sizeof (gint16)),
               "FLAC %d Hz %d ch: the samples are the ones that were encoded", t[k].rate, t[k].ch);
        g_free (got);
        g_free (err);
        g_free (pcm);
        g_remove (p);
        g_free (p);
    }
    /* rates outside the limits are refused although the decoder takes them */
    {
        gint16 *pcm = sine_pcm (4000, 1, 2000);
        char *p = write_flac ("slow.flac", pcm, 4000, 1, 2000), *err = NULL;
        if (p) {
            int rate, ch;
            gint64 n = decode_all (p, &rate, &ch, &err, NULL);
            CHECK (n == -1 && err, "FLAC at 4000 Hz: %" G_GINT64_FORMAT " frames, '%s'", n, err ? err : "");
            g_free (err);
            g_remove (p);
            g_free (p);
        }
        g_free (pcm);
    }
}

/* ---- 4 ---- */

/* the three packets of Xiph-laced extradata (what a Vorbis encoder gives) */
static gboolean xiph_split (const guint8 *x, gsize size, const guint8 *h[3], gsize len[3])
{
    if (size < 3 || x[0] != 2)
        return FALSE;
    gsize pos = 1;
    for (int i = 0; i < 2; i++) {
        len[i] = 0;
        while (pos < size && x[pos] == 255) {
            len[i] += 255;
            pos++;
        }
        if (pos >= size)
            return FALSE;
        len[i] += x[pos++];
    }
    if (pos + len[0] + len[1] >= size)
        return FALSE;
    len[2] = size - pos - len[0] - len[1];
    h[0] = x + pos;
    h[1] = h[0] + len[0];
    h[2] = h[1] + len[1];
    return TRUE;
}

/* an Ogg file from an encoder's packets: one packet a page. big_comment:
 * megabytes of comment (a cover picture), over several pages.
 * end: the position its last page gives. */
static char *write_ogg (const char *name, enum AVCodecID id, const Enc *e, gboolean big_comment, guint64 end)
{
    GByteArray *o = g_byte_array_new ();
    guint32 seq = 0;
    if (id == AV_CODEC_ID_VORBIS) {
        const guint8 *h[3];
        gsize len[3];
        if (!xiph_split (e->extra->data, e->extra->len, h, len) || len[0] >= 255 * 255 || len[2] >= 255 * 255) {
            g_byte_array_free (o, TRUE);
            return NULL;
        }
        ogg_page (o, 2, 0, seq++, h[0], len[0]);
        if (big_comment) {
            /* "\3vorbis" and 3 MiB behind it: 47 pages of 255 full segments
             * and one that ends the packet */
            guint8 hd[27] = { 'O', 'g', 'g', 'S', 0, 0 };
            guint8 *body = g_malloc0 (255 * 255);
            guint8 lace[255];
            memset (lace, 255, 255);
            put32le (hd + 14, 0x57414c50);
            memcpy (body, "\3vorbis", 7);
            for (int i = 0; i < 47; i++) {
                hd[5] = i ? 1 : 0;
                put32le (hd + 18, seq++);
                hd[26] = 255;
                g_byte_array_append (o, hd, 27);
                g_byte_array_append (o, lace, 255);
                g_byte_array_append (o, body, 255 * 255);
                memset (body, 0, 7);
            }
            hd[5] = 1;
            put32le (hd + 18, seq++);
            hd[26] = 1;
            g_byte_array_append (o, hd, 27);
            g_byte_array_append (o, (const guint8 *) "\10", 1);
            g_byte_array_append (o, body, 8);
            g_free (body);
        } else {
            ogg_page (o, 0, 0, seq++, h[1], len[1]);
        }
        ogg_page (o, 0, 0, seq++, h[2], len[2]);
    } else {
        static const guint8 tags[16] = { 'O', 'p', 'u', 's', 'T', 'a', 'g', 's' };
        ogg_page (o, 2, 0, seq++, e->extra->data, e->extra->len);
        ogg_page (o, 0, 0, seq++, tags, sizeof tags);
    }
    for (guint i = 0; i < e->pkts->len; i++) {
        gsize n = 0;
        const guint8 *d = g_bytes_get_data (e->pkts->pdata[i], &n);
        const gboolean last = i + 1 == e->pkts->len;
        ogg_page (o, last ? 4 : 0, last ? end : 0, seq++, d, n);
    }
    char *p = put_file (name, o->data, o->len);
    g_byte_array_free (o, TRUE);
    return p;
}

static void test_ogg (void)
{
    const int frames = 48000;
    gint16 *pcm = sine_pcm (48000, 2, frames);
    static const struct { enum AVCodecID id; const char *name; } codecs[] = {
        { AV_CODEC_ID_VORBIS, "Vorbis" }, { AV_CODEC_ID_OPUS, "Opus" } };
    for (guint k = 0; k < G_N_ELEMENTS (codecs); k++) {
        Enc e;
        if (!encode (codecs[k].id, AV_SAMPLE_FMT_FLTP, 48000, 2, pcm, frames, &e)) {
            printf ("sounddec: SKIP Ogg %s (this FFmpeg has no encoder for it)\n", codecs[k].name);
            skipped++;
            continue;
        }
        const gboolean opus = codecs[k].id == AV_CODEC_ID_OPUS;
        const int pre = opus && e.extra->len >= 19 ? e.extra->data[10] | e.extra->data[11] << 8 : 0;
        for (int variant = 0; variant < (opus ? 2 : 3); variant++) {
            /* 0: the last page says where the sound ends; 1: it says nothing
             * (everything the packets decode to is played); 2: megabytes of
             * comment in front */
            const guint64 end = variant == 1 ? 0 : (guint64) (frames + pre);
            char *p = write_ogg ("sine.ogg", codecs[k].id, &e, variant == 2, end), *err = NULL;
            if (!p) {
                CHECK (FALSE, "Ogg %s: the encoder's headers could not be split", codecs[k].name);
                break;
            }
            int rate, ch;
            gint16 *got = NULL;
            gint64 n = decode_all (p, &rate, &ch, &err, &got);
            int peak = 0;
            for (gint64 i = 0; got && i < n * ch; i++)
                peak = MAX (peak, abs (got[i]));
            CHECK (n > frames / 2 && rate == 48000 && ch == 2 && peak > 6000 && peak < 20000,
                   "Ogg %s (%d): %" G_GINT64_FORMAT " frames, %d Hz, %d ch, peak %d, '%s'", codecs[k].name, variant,
                   n, rate, ch, peak, err ? err : "");
            if (variant != 1)
                CHECK (n <= frames, "Ogg %s (%d): %" G_GINT64_FORMAT " frames played, the file's sound has %d",
                       codecs[k].name, variant, n, frames);
            printf ("sounddec: Ogg %s variant %d: %" G_GINT64_FORMAT " frames of %d (pre-skip %d)\n", codecs[k].name,
                    variant, n, frames, pre);
            if (opus && pre > 0 && got && n > 4000 + pre) {
                /* Not a check, a measurement for whoever runs this first: is
                 * the decoded sine in step with the encoded one (the decoder
                 * dropped the pre-skip itself, as sounddec.c takes for
                 * granted), or late by the pre-skip (it did not, and
                 * sounddec.c would have to skip `pre` samples)? The checks
                 * above pass either way. */
                double at0 = 0.0, late = 0.0, e0 = 0.0, e1 = 0.0, e2 = 0.0;
                for (int i = 0; i < 4000; i++) {
                    const double a = pcm[i * 2], b0 = got[i * ch], b1 = got[(i + pre) * ch];
                    at0 += a * b0;
                    late += a * b1;
                    e0 += a * a;
                    e1 += b0 * b0;
                    e2 += b1 * b1;
                }
                at0 /= sqrt (e0 * e1) + 1e-9;
                late /= sqrt (e0 * e2) + 1e-9;
                printf ("sounddec: Ogg Opus variant %d: likeness to the encoded sine %.2f in step, %.2f when "
                        "taken %d samples later: the decoder %s the pre-skip itself\n", variant, at0, late, pre,
                        at0 >= late ? "drops" : "DOES NOT drop");
            }
            g_free (got);
            g_free (err);
            g_remove (p);
            g_free (p);
        }
        enc_clear (&e);
    }
    g_free (pcm);
}

/* ---- 5 ---- */
static void test_player (void)
{
    static const WalpeSoundDecoder dec = { walpe_sounddec_open, walpe_sounddec_read, walpe_sounddec_close,
                                          walpe_sounddec_skip };
    gint16 *pcm = sine_pcm (44100, 2, 30000);
    char *p = write_flac ("player.flac", pcm, 44100, 2, 30000);
    g_free (pcm);
    if (!p) {
        printf ("sounddec: SKIP the player run (no FLAC encoder)\n");
        skipped++;
        return;
    }
    walpe_sound_set_decoder (&dec);
    walpe_sound_use_null (TRUE);
    walpe_sound_allow (TRUE);
    WalpeSoundGroup *g = walpe_sound_group_new ("/w/flac", 1);
    walpe_sound_group_set_active (g, TRUE);
    const char *files[1] = { p };
    WalpeSoundSpec spec = { files, 1, 1.0f, WALPE_SOUND_SINGLE, 0.0f, 0.0f, "flac" };
    WalpeSound *s = walpe_sound_new (g, &spec);
    walpe_sound_play (s);
    WAIT (!walpe_sound_is_playing (s), 10000);
    WalpeSoundStats st;
    walpe_sound_stats (s, &st);
    CHECK (st.frames == 30000 && st.rate == 44100 && st.channels == 2 && st.plays == 1 && st.null_out &&
           st.peak > 11900 && st.peak <= 12000 && !st.error[0],
           "FLAC through a player: %" G_GINT64_FORMAT " frames, %d Hz, %d ch, plays %d, peak %d, '%s'", st.frames,
           st.rate, st.channels, st.plays, st.peak, st.error);
    walpe_sound_group_free (g);
    WAIT (walpe_sound_threads () == 0, 5000);
    CHECK (walpe_sound_threads () == 0, "the player's thread ends");
    walpe_sound_set_decoder (NULL);
    g_remove (p);
    g_free (p);
}

/* ---- 6 ---- */

/* bytes this process has read from files so far (/proc/self/io), -1 = not known */
static gint64 bytes_read (void)
{
    char *text = NULL;
    gint64 n = -1;
    if (g_file_get_contents ("/proc/self/io", &text, NULL, NULL)) {
        const char *at = strstr (text, "rchar:");
        if (at)
            n = g_ascii_strtoll (at + 6, NULL, 10);
    }
    g_free (text);
    return n;
}

/* skip (place), then everything to the end: the samples have to be pcm's
 * from MIN (place, frames) on. *io (optional): bytes read from files by
 * the skip. */
static void place_check (const char *what, const char *path, const gint16 *pcm, gint64 frames, gint64 place,
                         gint64 *io)
{
    int rate = 0, ch = 0;
    char *err = NULL;
    gpointer d = walpe_sounddec_open (path, &rate, &ch, &err);
    CHECK (d && ch == 1, "%s: opened (%s)", what, err ? err : "");
    g_free (err);
    if (!d || ch != 1)
        return;
    const gint64 io0 = bytes_read ();
    const gint64 passed = walpe_sounddec_skip (d, place);
    if (io)
        *io = io0 >= 0 ? bytes_read () - io0 : -1;
    const gint64 at = MIN (place, frames);
    gint64 got = 0, wrong = -1;
    gint16 buf[1024];
    for (int guard = 0; guard < 4000000; guard++) {
        int n = walpe_sounddec_read (d, buf, 1000 + guard % 24);
        if (n <= 0)
            break;
        for (int i = 0; i < n && wrong < 0; i++)
            if (at + got + i >= frames || buf[i] != pcm[at + got + i])
                wrong = at + got + i;
        got += n;
    }
    walpe_sounddec_close (d);
    CHECK (passed == at && got == frames - at && wrong < 0,
           "%s, place %" G_GINT64_FORMAT ": %" G_GINT64_FORMAT " passed over, %" G_GINT64_FORMAT " read (of %"
           G_GINT64_FORMAT "), first wrong sample at %" G_GINT64_FORMAT, what, place, passed, got, frames - at, wrong);
}

static void test_place (void)
{
    /* 300 s at 16 kHz, one channel: noise that says where it is (it does
     * not compress), a stretch of silence (it compresses to nearly
     * nothing, so a place is far from where its share of the bytes is),
     * a tone, noise again */
    enum { RATE = 16000, FRAMES = 300 * RATE };
    gint16 *pcm = g_new (gint16, FRAMES);
    for (guint32 i = 0; i < FRAMES; i++) {
        guint32 h = i * 2654435761u;
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        const int part = (int) (i / (60u * RATE));
        pcm[i] = part == 1 ? 0 : part == 3 ? (gint16) lrint (9000.0 * sin (i * 0.05) + (h & 7)) : (gint16) (h >> 16);
    }
    char *p = write_flac ("place.flac", pcm, RATE, 1, FRAMES);
    gsize len = 0;
    guint8 *file = NULL;
    if (!p || !g_file_get_contents (p, (char **) &file, &len, NULL) || len < 42) {
        printf ("sounddec: SKIP a place in a FLAC file (no FLAC encoder)\n");
        skipped++;
        g_free (pcm);
        g_free (p);
        return;
    }
    const int block = file[8] << 8 | file[9];
    CHECK (block == (file[10] << 8 | file[11]) && block >= 16, "the encoder writes frames of one size (%d)", block);
    /* as FFmpeg writes the file when it can go back to its header (the
     * test's writer cannot: the encoder is asked for the header first):
     * the number of samples */
    guint8 *with = g_memdup2 (file, len);
    with[21] = (guint8) ((with[21] & 0xf0) | 0);
    put32be (with + 22, FRAMES);
    char *pw = put_file ("place-len.flac", with, len);

    /* places: the start, around the distance below which nothing is
     * jumped, around frame borders, each part and each border between
     * parts, the last samples, the end, behind the end */
    GArray *places = g_array_new (FALSE, FALSE, sizeof (gint64));
    const gint64 fixed[] = { 0, 1, 2 * RATE - 1, 2 * RATE, 2 * RATE + 1, 3 * RATE, 10 * (gint64) block - 1,
                             10 * (gint64) block, 10 * (gint64) block + 1, 59 * RATE, 60 * RATE - 1, 60 * RATE,
                             60 * RATE + 1, 90 * RATE, 119 * RATE + 7, 120 * RATE, 121 * RATE, 150 * RATE,
                             180 * RATE - 1, 180 * RATE + 3, 200 * RATE, 240 * RATE, 285 * RATE, FRAMES - block - 1,
                             FRAMES - block, FRAMES - 2, FRAMES - 1, FRAMES, FRAMES + 5, 100 * (gint64) FRAMES };
    g_array_append_vals (places, fixed, G_N_ELEMENTS (fixed));
    GRand *rng = g_rand_new_with_seed (7);
    for (int i = 0; i < 40; i++) {
        gint64 v = g_rand_int_range (rng, 0, FRAMES);
        g_array_append_val (places, v);
    }
    g_rand_free (rng);
    for (guint i = 0; i < places->len; i++) {
        const gint64 place = g_array_index (places, gint64, i);
        place_check ("FLAC that says its length", pw, pcm, FRAMES, place, NULL);
        if (i % 6 == 0)     /* (decoded from the start each time: fewer of them) */
            place_check ("FLAC that does not say its length", p, pcm, FRAMES, place, NULL);
    }
    g_array_free (places, TRUE);
    /* what the skip is for: the file in front of a far place is not read
     * (it was decoded whole) */
    {
        gint64 io_len = -1, io_no = -1;
        place_check ("far place", pw, pcm, FRAMES, 290 * (gint64) RATE, &io_len);
        place_check ("far place, no length", p, pcm, FRAMES, 290 * (gint64) RATE, &io_no);
        if (io_len < 0 || io_no < 0)
            printf ("sounddec: no /proc/self/io: bytes read not checked\n");
        else
            CHECK (io_len < (gint64) len / 8 && io_no > (gint64) len * 3 / 4,
                   "a place at 290 s of 300: %" G_GINT64_FORMAT " bytes read of %" G_GSIZE_FORMAT
                   " (and %" G_GINT64_FORMAT " when the file does not say its length)", io_len, len, io_no);
    }
    /* a description that lies. Twice the length: the offsets tried are
     * wrong, the place is still found (or the file read from its start).
     * A larger frame size than the frames have (a smaller one the decoder
     * refuses): no frame is taken for one, the file is read from its
     * start. */
    {
        guint8 *lie = g_memdup2 (with, len);
        put32be (lie + 22, 2 * FRAMES);
        char *pl = put_file ("place-lie.flac", lie, len);
        place_check ("FLAC that says twice its length", pl, pcm, FRAMES, 100 * (gint64) RATE, NULL);
        place_check ("FLAC that says twice its length", pl, pcm, FRAMES, 299 * (gint64) RATE, NULL);
        g_remove (pl);
        g_free (pl);
        memcpy (lie, with, len);
        lie[8] = lie[10] = (guint8) ((block * 2) >> 8);
        lie[9] = lie[11] = (guint8) (block * 2);
        pl = put_file ("place-lie.flac", lie, len);
        place_check ("FLAC that says twice its frame size", pl, pcm, FRAMES, 100 * (gint64) RATE, NULL);
        place_check ("FLAC that says twice its frame size", pl, pcm, FRAMES, 250 * (gint64) RATE, NULL);
        g_remove (pl);
        g_free (pl);
        g_free (lie);
    }
    /* a hole: 3 MiB of zeros where frames were, and a file cut off. No
     * crash, no endless read; what comes out is not compared (frames are
     * missing). */
    {
        guint8 *hole = g_memdup2 (with, len);
        const gsize a = len / 2, n = MIN ((gsize) 3 << 20, len / 4);
        memset (hole + a, 0, n);
        char *ph = put_file ("place-hole.flac", hole, len);
        char *pc = put_file ("place-cut.flac", with, len / 2);
        const char *files[2] = { ph, pc };
        for (int k = 0; k < 2; k++) {
            for (int q = 1; q <= 9; q += 2) {
                int rate = 0, ch = 0;
                char *err = NULL;
                gpointer d = walpe_sounddec_open (files[k], &rate, &ch, &err);
                CHECK (d != NULL, "damaged file %d opens (%s)", k, err ? err : "");
                g_free (err);
                if (!d)
                    continue;
                const gint64 want = (gint64) FRAMES * q / 10;
                const gint64 passed = walpe_sounddec_skip (d, want);
                gint64 got = 0;
                gint16 buf[1024];
                for (int guard = 0; guard < 4000000; guard++) {
                    int r = walpe_sounddec_read (d, buf, 1024);
                    if (r <= 0)
                        break;
                    got += r;
                }
                CHECK (passed >= 0 && passed <= want && passed + got <= FRAMES,
                       "damaged file %d, place %d/10: %" G_GINT64_FORMAT " passed over, %" G_GINT64_FORMAT " read",
                       k, q, passed, got);
                walpe_sounddec_close (d);
            }
        }
        g_remove (ph);
        g_remove (pc);
        g_free (ph);
        g_free (pc);
        g_free (hole);
    }
    g_remove (p);
    g_remove (pw);
    g_free (p);
    g_free (pw);
    g_free (with);
    g_free (file);
    g_free (pcm);
}

int main (void)
{
    tmp = g_dir_make_tmp ("walpe-sounddec-XXXXXX", NULL);
    if (!tmp) {
        printf ("FAIL: no temp dir\n");
        return 1;
    }
    test_hostile ();
    test_mp3 ();
    test_flac ();
    test_ogg ();
    test_player ();
    test_place ();
    g_rmdir (tmp);
    g_free (tmp);
    if (fails) {
        printf ("sounddec: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("sounddec: ok (%d part(s) skipped)\n", skipped);
    return 0;
}
