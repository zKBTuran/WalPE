/* sounddec.c - MP3, FLAC and Ogg (Vorbis, Opus) for scene sound.
 *
 * Part of walpe-video.so, the only place that links FFmpeg. The build has
 * libavcodec and libavutil only (no libavformat: no demuxers, no protocol
 * handlers, nothing that could follow a URL), so the three containers are
 * read here: MP3 and FLAC are raw frame streams (ID3v2 tags in front are
 * skipped, FLAC's STREAMINFO block is the decoder's extradata) cut into
 * frames by libavcodec's parsers; an Ogg file is taken apart into pages and
 * packets, its first logical stream is played.
 *
 * WHICH DECODERS. The file's first bytes choose one of four codec ids -
 * MP3 (MPEG audio), FLAC, Vorbis, Opus - and avcodec_find_decoder () is
 * asked for exactly that one. No other decoder of the FFmpeg build can be
 * reached from a wallpaper's sound file, whatever the file is named.
 *
 * MP3 has no magic number: a file counts as one when two frame headers
 * follow each other correctly within its first 64 KiB (mp3_find).
 *
 * LIMITS (scenesound.h): a regular file of at most 2 GiB; 8000..192000 Hz;
 * 1..8 channels, of which the first two are played; a stream whose sample
 * rate changes ends there; PROBE_MAX bytes without one decoded frame and
 * the file is "not sound"; an Ogg packet above OGG_PACKET_MAX ends the
 * stream (a comment header may be larger: its content is not used);
 * nothing of a file is kept in memory beyond one packet and one frame.
 *
 * LOOPS. What a file holds besides its sound is not played, so that a loop
 * closes without a gap: the Xing / Info frame of an MP3 and the encoder's
 * delay and padding that a LAME tag in it states (an MP3 without that tag
 * keeps the few hundredths of a second of silence its encoder added), and
 * what a Vorbis or Opus stream decodes beyond the position its last page
 * gives. (The pre-skip of Opus is dropped by the decoder itself.)
 *
 * A PLACE IN A FILE (walpe_sounddec_skip). The decoders read forward only;
 * sound in front of a place is decoded and dropped. A FLAC file whose
 * header says how long it is and that all its frames have one size - what
 * FFmpeg's encoder writes, so every sound track taken out of a video
 * (common/import.h) - is entered near the place instead: a frame is looked
 * for at the byte offset the place should have, its header says which
 * frame it is, and a few such steps end within FLAC_NEAR_S seconds in
 * front of the place. A FLAC frame decodes by itself, so what follows is
 * sample for sample what decoding from the start gives. A file that does
 * not fit the description, or in which no frame is found where one should
 * be, is read from its start as before.
 *
 * Output: interleaved 16-bit frames, one or two channels. One decoder
 * belongs to one thread. Files are data: every length read from one is
 * checked before it is used. */
#include "scenesound.h"
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mem.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

enum { K_MP3, K_FLAC, K_OGG };
#define IN_SIZE 16384
#define OGG_PACKET_MAX (1u << 20)
/* Bytes of an Ogg file that may go by between two packets of the stream
 * without belonging to one: page headers, empty pages, empty packets, the
 * pages of other streams. (The body of an oversized packet is not counted:
 * it is passed over with one seek a page.) */
#define OGG_IDLE_MAX (1u << 20)
#define PROBE_MAX (4 << 20)       /* bytes read without one decoded frame: not a sound file */
/* Bytes FFmpeg's parser may hold. A parser keeps what it is fed until it has
 * found a frame in it (MP3: the next header; FLAC: several headers in a
 * row). Zeros behind a file's first frames hold no header: the parser
 * copied them into its buffer to the end of the file - up to 2 GiB of heap
 * for one player, from a file that compresses to nothing. What went into
 * the parser and has not come out as a packet is counted; above this the
 * sound ends there. (MP3 frames are below 3 KiB; FLAC needs about ten
 * frames of at most some hundred KiB.) */
#define PARSER_MAX (8 << 20)
#define FRAME_MAX (1 << 20)       /* samples of one decoded frame (FLAC: 65535, Opus: 5760) */

typedef struct {
    FILE *f;
    int kind;
    AVCodecContext *ctx;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    gboolean have_frame, drained, parser_flushed, finished;
    int frame_pos;
    int rate, channels;           /* as reported to the caller */
    gint64 consumed;              /* input bytes so far */
    gint64 decoded;               /* samples the decoder delivered so far */
    gint64 frame_first;           /* position of d->frame's first sample among them */
    gint64 skip;                  /* that many samples at the start are not sound */
    gint64 end;                   /* the sound ends at this position (0 = with the stream) */
    gint64 given;                 /* frames handed to the caller */
    int bad;                      /* packets refused in a row */
    gint64 held;                  /* raw streams: bytes inside the parser (PARSER_MAX) */
    /* raw streams */
    guint8 in[IN_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    int in_len, in_pos;
    gboolean in_eof;
    /* Ogg */
    guint8 seg[255];
    int nseg, iseg;
    guint32 serial;
    gboolean have_serial, drop, cut;
    gboolean past_headers;        /* the header packets are read: sound packets follow */
    GByteArray *opkt;
    int pre;                      /* Opus: pre-skip (samples the page positions count
                                   * and the decoder does not deliver) */
    /* FLAC, from the stream description (flac_jump) */
    gint64 fl_total;              /* samples in the file, 0 = not said */
    int fl_block;                 /* samples of every frame but the last, 0 = they differ */
    int fl_frame_max;             /* bytes of the largest frame, 0 = not said */
    gint64 fl_data, fl_size;      /* where the frames begin; the file's size */
} Dec;

static guint32 rd_le32 (const guint8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (guint32) p[3] << 24; }
static guint32 rd_be32 (const guint8 *p) { return (guint32) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int frame_channels (const AVFrame *fr)
{
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT (57, 28, 100)
    return fr->ch_layout.nb_channels;
#else
    return fr->channels;
#endif
}

/* ---- Ogg: the next packet of the first logical stream ----
 * 1 = d->opkt holds a packet, 2 = it holds the first OGG_PACKET_MAX bytes
 * of a larger one, 0 = end of the stream */
static int ogg_packet (Dec *d)
{
    guint64 idle = 0;
    g_byte_array_set_size (d->opkt, 0);
    d->cut = FALSE;
    for (;;) {
        while (d->iseg < d->nseg) {
            guint len = d->seg[d->iseg++];
            guint o = d->opkt->len;
            if (o + len > OGG_PACKET_MAX) {
                /* no sound packet is that large: the stream ends for the
                 * caller, the packet is not walked to its end (which may
                 * be the end of a 2 GiB file, a seek per segment) */
                if (d->past_headers)
                    return 2;
                /* the rest of an oversized header packet is passed over */
                if (len && fseek (d->f, (long) len, SEEK_CUR))
                    return 0;
                d->cut = TRUE;
            } else {
                g_byte_array_set_size (d->opkt, o + len);
                if (len && fread (d->opkt->data + o, 1, len, d->f) != len)
                    return 0;
            }
            d->consumed += len;
            if (len < 255) {
                if (d->drop) {            /* the tail of a packet whose start is missing */
                    d->drop = FALSE;
                    d->cut = FALSE;
                    g_byte_array_set_size (d->opkt, 0);
                    continue;
                }
                if (!d->opkt->len && !d->cut)
                    continue;             /* an empty packet: nothing to decode (a page
                                           * of 282 bytes can hold 255 of them) */
                return d->cut ? 2 : 1;
            }
        }
        /* Megabytes of pages without a packet of the stream: it ends here.
         * Without this limit the loop went on to the end of the file in
         * one call - 79 million page headers in 2 GiB - and again on every
         * pass of a loop layer; feed () looked at PROBE_MAX only between
         * two calls, and only before the first decoded frame. */
        if (idle > OGG_IDLE_MAX)
            return 0;
        guint8 h[27];
        if (fread (h, 1, 27, d->f) != 27 || memcmp (h, "OggS", 4) || h[4] != 0)
            return 0;
        int nseg = h[26];
        if (nseg && fread (d->seg, 1, (size_t) nseg, d->f) != (size_t) nseg)
            return 0;
        d->consumed += 27 + nseg;
        idle += 27 + (guint) nseg;
        guint32 serial = rd_le32 (h + 14);
        if (!d->have_serial) {
            d->serial = serial;
            d->have_serial = TRUE;
        }
        if (serial != d->serial) {        /* another stream's page */
            long body = 0;
            for (int i = 0; i < nseg; i++)
                body += d->seg[i];
            if (body && fseek (d->f, body, SEEK_CUR))
                return 0;
            d->consumed += body;
            idle += (guint64) body;
            d->nseg = d->iseg = 0;
            continue;
        }
        if (h[5] & 4) {
            /* the stream's last page: its position is where the sound ends
             * (the last packet decodes to a whole block, which may be more) */
            guint64 gp = (guint64) rd_le32 (h + 6) | (guint64) rd_le32 (h + 10) << 32;
            if (gp > (guint64) d->pre && gp < (guint64) G_MAXINT64 && !d->end)
                d->end = (gint64) gp - d->pre;
        }
        gboolean cont = (h[5] & 1) != 0;
        if (cont && !d->opkt->len && !d->cut)
            d->drop = TRUE;
        else if (!cont && (d->opkt->len || d->cut)) {
            g_byte_array_set_size (d->opkt, 0);   /* its end never came */
            d->cut = FALSE;
        }
        d->nseg = nseg;
        d->iseg = 0;
    }
}

/* ---- feeding the decoder ---- */
static int send (Dec *d, const guint8 *data, int size)
{
    d->pkt->data = (guint8 *) data;
    d->pkt->size = size;
    int r = avcodec_send_packet (d->ctx, d->pkt);
    if (r < 0 && r != AVERROR (EAGAIN)) {
        if (++d->bad > 400)
            return -1;            /* nothing but damage */
    } else {
        d->bad = 0;
    }
    return 1;
}

/* 1 = something was sent, 0 = the input is at its end (decoder told so), < 0 = give up */
static int feed (Dec *d)
{
    if (d->drained)
        return 0;
    if (!d->decoded && d->consumed > PROBE_MAX)
        return -1;
    if (d->kind == K_OGG) {
        int r = ogg_packet (d);
        if (r == 1) {
            guint n = d->opkt->len;
            g_byte_array_set_size (d->opkt, n + AV_INPUT_BUFFER_PADDING_SIZE);
            memset (d->opkt->data + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            return send (d, d->opkt->data, (int) n);   /* (never empty: ogg_packet) */
        }
        /* (r == 2: no sound packet is that large - the stream ends here) */
    } else {
        for (int guard = 0; guard < 100000; guard++) {
            if (d->in_pos >= d->in_len && !d->in_eof) {
                size_t n = fread (d->in, 1, IN_SIZE, d->f);
                d->in_len = (int) n;
                d->in_pos = 0;
                memset (d->in + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                if (!n)
                    d->in_eof = TRUE;
            }
            int left = d->in_len - d->in_pos;
            if (!left && d->parser_flushed)
                break;
            guint8 *data = NULL;
            int size = 0;
            int used = av_parser_parse2 (d->parser, d->ctx, &data, &size, left ? d->in + d->in_pos : NULL, left,
                                         AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0 || used > left)
                return -1;
            d->in_pos += used;
            d->consumed += used;
            d->held += used;
            if (size > 0) {
                d->held = MAX (d->held - size, 0);
                return send (d, data, size);
            }
            if (!left)
                d->parser_flushed = TRUE;   /* the parser had nothing left to give */
            if (!d->decoded && d->consumed > PROBE_MAX)
                return -1;
            if (d->held > PARSER_MAX)
                break;                      /* no frame in megabytes: the sound ends here */
        }
    }
    d->drained = TRUE;
    avcodec_send_packet (d->ctx, NULL);
    return 0;
}

/* 1 = d->frame holds samples, 0 = end, < 0 = error */
static int next_frame (Dec *d)
{
    for (int guard = 0; guard < 1000000; guard++) {
        int r = avcodec_receive_frame (d->ctx, d->frame);
        if (r == 0) {
            int ch = frame_channels (d->frame);
            if (d->frame->nb_samples <= 0 || d->frame->nb_samples > FRAME_MAX || ch < 1 ||
                ch > WALPE_SOUND_MAX_CHANNELS || !d->frame->extended_data || !d->frame->extended_data[0] ||
                av_get_packed_sample_fmt (d->frame->format) == AV_SAMPLE_FMT_NONE) {
                av_frame_unref (d->frame);
                continue;
            }
            if (d->rate && d->frame->sample_rate != d->rate) {
                /* another sample rate from here on: played at the first
                 * one it would be sound at the wrong speed */
                av_frame_unref (d->frame);
                return 0;
            }
            d->have_frame = TRUE;
            d->frame_pos = 0;
            d->frame_first = d->decoded;
            d->decoded += d->frame->nb_samples;
            return 1;
        }
        if (r == AVERROR_EOF)
            return 0;
        if (r != AVERROR (EAGAIN)) {
            /* a frame the decoder could not make: counted like a packet it
             * refuses (send), not the end of the file at the first one.
             * avcodec.h allows "legitimate decoding errors" from both
             * calls; which of them reports a damaged packet is the
             * library's choice. */
            if (++d->bad > 400)
                return -1;
            continue;
        }
        if (feed (d) < 0)
            return -1;
    }
    return -1;
}

static inline gint16 sample16 (const guint8 *p, enum AVSampleFormat packed, int idx)
{
    switch (packed) {
    case AV_SAMPLE_FMT_FLT: {
        float v;
        memcpy (&v, p + (gsize) idx * sizeof v, sizeof v);
        if (!(v == v))
            return 0;
        v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
        return (gint16) lrintf (v * 32767.0f);
    }
    case AV_SAMPLE_FMT_DBL: {
        double v;
        memcpy (&v, p + (gsize) idx * sizeof v, sizeof v);
        if (!(v == v))
            return 0;
        v = v < -1.0 ? -1.0 : v > 1.0 ? 1.0 : v;
        return (gint16) lrint (v * 32767.0);
    }
    case AV_SAMPLE_FMT_S16: {
        gint16 v;
        memcpy (&v, p + (gsize) idx * sizeof v, sizeof v);
        return v;
    }
    case AV_SAMPLE_FMT_S32: {
        gint32 v;
        memcpy (&v, p + (gsize) idx * sizeof v, sizeof v);
        return (gint16) (v / 65536);
    }
    case AV_SAMPLE_FMT_S64: {
        gint64 v;
        memcpy (&v, p + (gsize) idx * sizeof v, sizeof v);
        return (gint16) (v / (G_GINT64_CONSTANT (1) << 48));
    }
    case AV_SAMPLE_FMT_U8:
        return (gint16) ((p[idx] - 128) * 256);
    default:
        return 0;
    }
}

int walpe_sounddec_read (gpointer dec, gint16 *out, int max_frames)
{
    Dec *d = dec;
    int n = 0;
    while (n < max_frames && !d->finished) {
        if (!d->have_frame) {
            int r = next_frame (d);
            if (r <= 0) {
                d->finished = TRUE;
                return n ? n : r;
            }
        }
        const AVFrame *fr = d->frame;
        const gint64 at = d->frame_first + d->frame_pos;
        int avail = fr->nb_samples - d->frame_pos;
        int take = 0;
        if (at < d->skip) {
            /* encoder delay / pre-skip: not sound */
            d->frame_pos += (int) MIN ((gint64) avail, d->skip - at);
        } else if ((d->end > 0 && at >= d->end) ||
                   d->given >= (gint64) d->rate * WALPE_SOUND_MAX_SECONDS) {
            d->finished = TRUE;   /* padding behind the sound, or the length limit */
        } else {
            if (d->end > 0)
                avail = (int) MIN ((gint64) avail, d->end - at);
            take = MIN (avail, max_frames - n);
            const int sch = frame_channels (fr);
            const gboolean planar = av_sample_fmt_is_planar (fr->format);
            const enum AVSampleFormat packed = av_get_packed_sample_fmt (fr->format);
            for (int c = 0; out && c < d->channels; c++) {
                const int sc = MIN (c, sch - 1);     /* a mono frame in a stereo stream: both sides */
                const guint8 *p = planar ? fr->extended_data[sc] : fr->extended_data[0];
                if (!p) {
                    for (int i = 0; i < take; i++)
                        out[(n + i) * d->channels + c] = 0;
                    continue;
                }
                for (int i = 0; i < take; i++) {
                    int idx = planar ? d->frame_pos + i : (d->frame_pos + i) * sch + sc;
                    out[(n + i) * d->channels + c] = sample16 (p, packed, idx);
                }
            }
            n += take;
            d->given += take;
            d->frame_pos += take;
        }
        if (d->finished || d->frame_pos >= fr->nb_samples) {
            av_frame_unref (d->frame);
            d->have_frame = FALSE;
        }
    }
    return n;
}

/* ---- FLAC: entering the file near a place ---- */
#define FLAC_NEAR_S 2             /* a frame this close in front of the place: read on from there */
#define FLAC_JUMPS 10             /* offsets tried for one place, at most */
#define FLAC_SCAN_MAX (2 << 20)   /* bytes read from an offset without a frame showing up */

/* p: what the parser gave out as a frame. Where in the stream that frame
 * begins (samples), read from its header; -1 when p is no header of a
 * frame of this stream - the bytes between an offset and the first frame
 * behind it, or a stream that is not what its description says. */
static gint64 flac_frame_at (const Dec *d, const guint8 *p, int size)
{
    /* the sync code, a reserved bit, "all frames have one size" */
    if (size < 6 || p[0] != 0xff || p[1] != 0xf8)
        return -1;
    const int bs_code = p[2] >> 4, sr_code = p[2] & 15;
    if (!bs_code || sr_code == 15 || (p[3] & 1))
        return -1;
    /* the frame's number, written the way UTF-8 writes a character (31 bits) */
    int more = p[4] < 0x80 ? 0 : p[4] < 0xc0 ? -1 : p[4] < 0xe0 ? 1 : p[4] < 0xf0 ? 2 : p[4] < 0xf8 ? 3 :
               p[4] < 0xfc ? 4 : p[4] < 0xfe ? 5 : -1;
    if (more < 0)
        return -1;
    guint64 num = more ? (guint64) (p[4] & (0x3f >> more)) : p[4];
    int i = 5;
    for (; more > 0; more--, i++) {
        if (i >= size || (p[i] & 0xc0) != 0x80)
            return -1;
        num = num << 6 | (p[i] & 0x3f);
    }
    /* its own size: a code, or 8 / 16 bits behind the number */
    int bs;
    if (bs_code == 1) {
        bs = 192;
    } else if (bs_code <= 5) {
        bs = 576 << (bs_code - 2);
    } else if (bs_code == 6) {
        if (i >= size)
            return -1;
        bs = p[i++] + 1;
    } else if (bs_code == 7) {
        if (i + 1 >= size)
            return -1;
        bs = (p[i] << 8 | p[i + 1]) + 1;
        i += 2;
    } else {
        bs = 256 << (bs_code - 8);
    }
    i += sr_code == 12 ? 1 : sr_code == 13 || sr_code == 14 ? 2 : 0;
    if (i >= size)
        return -1;
    /* the header's check sum (x^8 + x^2 + x + 1) */
    guint8 crc = 0;
    for (int k = 0; k < i; k++) {
        crc ^= p[k];
        for (int b = 0; b < 8; b++)
            crc = (guint8) (crc & 0x80 ? crc << 1 ^ 7 : crc << 1);
    }
    if (crc != p[i])
        return -1;
    /* every frame but the last has the stream's size; the last ends the stream */
    const gint64 at = (gint64) num * d->fl_block;
    if (at >= d->fl_total || bs > d->fl_block || (bs != d->fl_block && at + bs != d->fl_total))
        return -1;
    return at;
}

/* The file is read from byte offset `off` on: parser and decoder begin
 * anew. FALSE: the decoder cannot go on at all. */
static gboolean flac_from (Dec *d, gint64 off)
{
    if (fseeko (d->f, (off_t) off, SEEK_SET))
        return FALSE;
    av_parser_close (d->parser);
    d->parser = av_parser_init (AV_CODEC_ID_FLAC);
    if (!d->parser)
        return FALSE;
    avcodec_flush_buffers (d->ctx);
    av_frame_unref (d->frame);
    d->have_frame = FALSE;
    d->frame_pos = 0;
    d->in_len = d->in_pos = 0;
    d->in_eof = d->drained = d->parser_flushed = d->finished = FALSE;
    d->held = 0;
    d->bad = 0;
    d->consumed = 0;              /* (feed: bytes read without a frame, counted from here) */
    return TRUE;
}

/* ... and the first frame that shows up behind `off` is handed to the
 * decoder (the next frame decoded is that one). Returns where it begins
 * (samples); -1 when none shows up; -2 when the decoder cannot go on. */
static gint64 flac_land (Dec *d, gint64 off)
{
    if (!flac_from (d, off))
        return -2;
    for (gint64 scanned = 0; scanned < FLAC_SCAN_MAX;) {
        if (d->in_pos >= d->in_len && !d->in_eof) {
            size_t n = fread (d->in, 1, IN_SIZE, d->f);
            d->in_len = (int) n;
            d->in_pos = 0;
            memset (d->in + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            if (!n)
                d->in_eof = TRUE;
        }
        const int left = d->in_len - d->in_pos;
        guint8 *data = NULL;
        int size = 0;
        int used = av_parser_parse2 (d->parser, d->ctx, &data, &size, left ? d->in + d->in_pos : NULL, left,
                                     AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0 || used > left)
            return -1;
        d->in_pos += used;
        d->consumed += used;
        scanned += used;
        if (size > 0) {
            const gint64 at = flac_frame_at (d, data, size);
            if (at >= 0) {
                if (send (d, data, size) < 0)
                    return -2;
                d->decoded = at;
                return at;
            }
        } else if (!left) {
            return -1;            /* the end of the file */
        }
    }
    return -1;
}

/* The decoder is moved to a frame that begins at most FLAC_NEAR_S seconds
 * in front of sample `target` (d->given then says where it is). FALSE:
 * not a file this works for, or the place is near anyway - nothing was
 * touched. A file in which no frame is found where there should be one is
 * read from its start again, as it was before there was this function. */
static gboolean flac_jump (Dec *d, gint64 target)
{
    if (d->kind != K_FLAC || d->fl_block <= 0 || d->fl_total <= 0 || d->fl_size <= d->fl_data || d->skip ||
        d->end || d->finished || d->rate <= 0)
        return FALSE;
    target = MIN (target, d->fl_total - 1);
    const gint64 near = (gint64) FLAC_NEAR_S * d->rate;
    if (target - d->given <= near)
        return FALSE;
    /* Two byte offsets are known at any time: one whose next frame begins
     * at or in front of the target (lo: at first the file's first frame)
     * and one behind which no frame is of use (hi: at first the file's
     * end). The next offset to try lies between them, where the target
     * would be if every sample took the same number of bytes, and a few
     * frames earlier than that. */
    gint64 lo_off = d->fl_data, lo_at = 0, hi_off = d->fl_size, hi_at = d->fl_total;
    const gint64 early = MAX (2 * (gint64) d->fl_frame_max, 32768);
    gint64 landed = -1, at = -1;
    for (int i = 0; i < FLAC_JUMPS && at != -2; i++) {
        const gint64 off = lo_off - early +
            (gint64) ((double) (hi_off - lo_off) * (double) (target - lo_at) / (double) (hi_at - lo_at));
        if (off <= lo_off || off >= hi_off)
            break;                /* no nearer than lo */
        at = flac_land (d, off);
        landed = off;
        if (at < 0 || at > target) {
            hi_off = off;
            if (at > target)
                hi_at = at;
            continue;
        }
        lo_off = off;
        lo_at = at;
        if (target - at <= near)
            break;
    }
    if (landed < 0)
        return FALSE;             /* (never left where it was) */
    if (landed != lo_off && at != -2)
        at = lo_off == d->fl_data ? -1 : flac_land (d, lo_off);
    if (at == lo_at) {
        d->given = lo_at;
        return TRUE;
    }
    /* No frame where the description says there are frames: the file as
     * it was read before, from its first byte of sound, whatever is there
     * - and no second try with this file. */
    d->fl_block = 0;
    d->decoded = d->given = 0;
    if (at == -2 || !flac_from (d, d->fl_data))
        d->finished = TRUE;       /* (it cannot be read at all any more: its sound ends) */
    return TRUE;
}

gint64 walpe_sounddec_skip (gpointer dec, gint64 frames)
{
    Dec *d = dec;
    if (!d || frames <= 0)
        return 0;
    const gint64 from = d->given;
    const gint64 target = from + MIN (frames, (gint64) WALPE_SOUND_MAX_RATE * WALPE_SOUND_MAX_SECONDS);
    flac_jump (d, target);
    while (!d->finished && d->given < target) {
        if (walpe_sounddec_read (d, NULL, (int) MIN (target - d->given, (gint64) 1 << 20)) <= 0)
            break;
    }
    return MAX (d->given - from, 0);
}

void walpe_sounddec_close (gpointer dec)
{
    Dec *d = dec;
    if (!d)
        return;
    if (d->parser)
        av_parser_close (d->parser);
    avcodec_free_context (&d->ctx);
    av_packet_free (&d->pkt);
    av_frame_free (&d->frame);
    if (d->opkt)
        g_byte_array_free (d->opkt, TRUE);
    if (d->f)
        fclose (d->f);
    g_free (d);
}

static gpointer fail (Dec *d, char **err, const char *msg)
{
    if (err && !*err)
        *err = g_strdup (msg);
    walpe_sounddec_close (d);
    return NULL;
}

/* extradata of a codec context: a copy with FFmpeg's padding */
static gboolean set_extradata (AVCodecContext *ctx, const guint8 *data, gsize len)
{
    ctx->extradata = av_mallocz (len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ctx->extradata)
        return FALSE;
    memcpy (ctx->extradata, data, len);
    ctx->extradata_size = (int) len;
    return TRUE;
}

/* ---- Vorbis: what the codebooks of a setup header cost ----
 * A Vorbis stream brings its own code tables. The setup header may state a
 * table's code lengths as runs ("ordered"): a few bytes then describe a
 * table of 65536 entries, for which the decoder builds 65536 vectors of up
 * to 16 numbers - 4 MiB - and its lookup tables. 256 such tables fit into
 * 6 KiB of header: about a gigabyte inside the decoder for one small file,
 * for every layer that names it. An encoder's tables have a few thousand
 * entries in all (5 bits of header for each).
 *
 * So the tables are walked here first (Vorbis I specification, 3.2.1; bits
 * are taken from the low end of each byte) and their entries are added up,
 * by the numbers the header states and not by what a decoder is believed
 * to refuse. Where the walk meets what no decoder can read on - a wrong
 * sync pattern, no dimensions, a run past the table, a reserved lookup
 * type, the end of the packet - it stops and leaves the refusal to the
 * decoder: what stood before that place has been counted. */
#define VORBIS_SETUP_MAX (256u << 10)   /* bytes of a setup header (encoders: 4..10 KiB) */
#define VORBIS_BOOKS_MAX (1u << 20)     /* entries x (dimensions + 1) of all tables */

typedef struct {
    const guint8 *p;
    guint64 nbits, pos;
    gboolean over;                /* asked for more than there is */
} Bits;

static guint32 bits_get (Bits *b, guint n)   /* n <= 32 */
{
    guint32 v = 0;
    if (b->over || n > b->nbits - b->pos) {
        b->over = TRUE;
        return 0;
    }
    for (guint i = 0; i < n; i++, b->pos++)
        v |= (guint32) (b->p[b->pos >> 3] >> (b->pos & 7) & 1) << i;
    return v;
}

static void bits_skip (Bits *b, guint64 n)
{
    if (b->over || n > b->nbits - b->pos)
        b->over = TRUE;
    else
        b->pos += n;
}

/* bits needed to write v (0 for 0) */
static guint bits_for (guint32 v)
{
    guint n = 0;
    for (; v; v >>= 1)
        n++;
    return n;
}

/* the largest r with r to the power dims <= entries (dims >= 1) */
static guint32 vorbis_root (guint32 entries, guint32 dims)
{
    if (dims == 1)
        return entries;
    for (guint32 r = 0;; r++) {
        guint64 pw = 1;
        for (guint32 i = 0; i < dims && pw <= entries; i++)
            pw *= (guint64) r + 1;
        if (pw > entries)
            return r;
    }
}

/* pkt: a setup header ("\5vorbis", then the tables). TRUE: its tables are
 * larger than VORBIS_BOOKS_MAX, or written as no encoder writes them. */
static gboolean vorbis_books_too_large (const guint8 *pkt, gsize len)
{
    if (len < 8)
        return FALSE;
    Bits b = { pkt + 7, (guint64) (len - 7) * 8, 0, FALSE };
    const guint books = bits_get (&b, 8) + 1;
    guint64 cost = 0;
    for (guint k = 0; k < books; k++) {
        if (bits_get (&b, 24) != 0x564342)
            return FALSE;
        const guint32 dims = bits_get (&b, 16), entries = bits_get (&b, 24);
        if (b.over || dims < 1)
            return FALSE;
        if (bits_get (&b, 1)) {
            /* ordered: a first length, then for each length up to 32 the
             * number of entries that have it */
            guint32 at = 0;
            for (guint length = bits_get (&b, 5) + 1; at < entries && length <= 32 && !b.over; length++) {
                const guint32 run = bits_get (&b, bits_for (entries - at));
                if (run > entries - at)
                    return FALSE;
                at += run;
            }
            /* entries left over after the longest code length there is: the
             * specification calls that an error, and decoders differ in
             * where they read on from here */
            if (at < entries && !b.over)
                return TRUE;
        } else if (bits_get (&b, 1)) {
            /* sparse: a flag for each entry, 5 bits of length where it is set */
            for (guint32 e = 0; e < entries && !b.over; e++)
                if (bits_get (&b, 1))
                    bits_skip (&b, 5);
        } else {
            bits_skip (&b, (guint64) entries * 5);
        }
        const guint32 lookup = bits_get (&b, 4);
        if (b.over || lookup > 2)
            return FALSE;
        if (lookup) {
            /* two 32-bit numbers, the width of a value, one flag, the values
             * (type 1: a short list all vectors are made of; type 2: every
             * vector written out) */
            bits_skip (&b, 64);
            const guint32 width = bits_get (&b, 4) + 1;
            bits_skip (&b, 1);
            bits_skip (&b, (lookup == 1 ? (guint64) vorbis_root (entries, dims) : (guint64) entries * dims) * width);
            if (b.over)
                return FALSE;
        }
        cost += (guint64) entries * (lookup ? dims + 1 : 1);
        if (cost > VORBIS_BOOKS_MAX)
            return TRUE;
    }
    return FALSE;
}

/* An MPEG audio layer III frame header at b (4 bytes): the frame's size in
 * bytes, 0 = this is none. */
static int mp3_header (const guint8 *b, int *rate, int *spf, gboolean *mono)
{
    static const int br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int br2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const int sr[4][4] = { { 11025, 12000, 8000, 0 }, { 0, 0, 0, 0 },
                                  { 22050, 24000, 16000, 0 }, { 44100, 48000, 32000, 0 } };
    if (b[0] != 0xff || (b[1] & 0xe0) != 0xe0)
        return 0;
    const int ver = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3;
    const int bri = (b[2] >> 4) & 15, sri = (b[2] >> 2) & 3, pad = (b[2] >> 1) & 1;
    const gboolean lsf = ver != 3;
    const int hz = sr[ver][sri], kbps = lsf ? br2[bri] : br1[bri];
    if (layer != 1 || !hz || !kbps)
        return 0;
    *rate = hz;
    *spf = lsf ? 576 : 1152;
    *mono = ((b[3] >> 6) & 3) == 3;
    return (int) ((lsf ? 72000L : 144000L) * kbps / hz) + pad;
}

/* Where the MP3 stream starts at or behind `start`: the first place within
 * MP3_SCAN bytes at which a frame header stands and the next frame's
 * header follows it at the distance it states, with the same sample rate.
 * -1: there is none - the file is not MP3. (A lone pattern that looks like
 * a header turns up in any file every few kilobytes; without this check
 * whatever follows it would be handed to the decoder as sound.) */
#define MP3_SCAN (64 << 10)
static long mp3_find (Dec *d, long start)
{
    long at = -1;
    if (fseek (d->f, start, SEEK_SET))
        return -1;
    guint8 *b = g_malloc (MP3_SCAN + 8);
    const size_t got = fread (b, 1, MP3_SCAN + 8, d->f);
    for (size_t i = 0; i + 4 <= got && i < MP3_SCAN && at < 0; i++) {
        int r1 = 0, r2 = 0, spf = 0;
        gboolean mono = FALSE;
        const int fs = mp3_header (b + i, &r1, &spf, &mono);
        if (fs < 24 || i + (size_t) fs + 4 > got)
            continue;
        if (mp3_header (b + i + fs, &r2, &spf, &mono) && r2 == r1)
            at = start + (long) i;
    }
    g_free (b);
    return at;
}

/* An MP3 made by LAME (and by FFmpeg) starts with a frame that holds no
 * sound but a table: "Xing" or "Info", the number of frames, and in LAME's
 * extension the samples the encoder put in front of the sound and behind
 * it. With `start` at the first frame of the file: the size of that table
 * frame when there is one (0 = none), and d->skip / d->end from it. */
static long mp3_info_frame (Dec *d, long start)
{
    guint8 b[256];
    int rate = 0, spf = 0;
    gboolean mono = FALSE;
    if (fseek (d->f, start, SEEK_SET))
        return 0;
    const size_t got = fread (b, 1, sizeof b, d->f);
    const long frame_size = got >= 64 ? mp3_header (b, &rate, &spf, &mono) : 0;
    if (!frame_size)
        return 0;
    const size_t tag = 4 + (size_t) (spf == 576 ? (mono ? 9 : 17) : (mono ? 17 : 32));
    if (tag + 8 > got || (memcmp (b + tag, "Xing", 4) && memcmp (b + tag, "Info", 4)))
        return 0;
    const guint32 flags = rd_be32 (b + tag + 4);
    size_t q = tag + 8;
    gint64 frames = 0;
    if (flags & 1) {
        if (q + 4 > got)
            return frame_size;
        frames = rd_be32 (b + q);
        q += 4;
    }
    q += (flags & 2 ? 4 : 0) + (flags & 4 ? 100 : 0) + (flags & 8 ? 4 : 0);
    /* the extension: 9 bytes encoder name, 12 bytes of other fields, then
     * 12 bits delay and 12 bits padding */
    if (q + 24 <= got && (!memcmp (b + q, "LAME", 4) || !memcmp (b + q, "Lavf", 4) || !memcmp (b + q, "Lavc", 4))) {
        const guint32 v = (guint32) b[q + 21] << 16 | (guint32) b[q + 22] << 8 | b[q + 23];
        const int delay = (int) (v >> 12), padding = (int) (v & 4095);
        /* 529: what a decoder itself puts in front */
        d->skip = delay + 529;
        if (frames > 0) {
            const gint64 all = frames * spf, end = all - padding + 529;
            d->end = end > d->skip ? MIN (end, all) : 0;
        }
    }
    return frame_size;
}

gpointer walpe_sounddec_open (const char *path, int *rate, int *channels, char **err)
{
    const char *why = NULL;
    Dec *d = g_new0 (Dec, 1);
    d->f = walpe_sound_fopen (path, &why);
    if (!d->f)
        return fail (d, err, why);
    /* ID3v2 tags in front */
    long start = 0;
    guint8 h[10];
    for (int i = 0; i < 4; i++) {
        if (fseek (d->f, start, SEEK_SET) || fread (h, 1, 10, d->f) != 10)
            break;
        if (memcmp (h, "ID3", 3) || (h[6] | h[7] | h[8] | h[9]) & 0x80)
            break;
        start += 10 + ((long) h[6] << 21 | h[7] << 14 | h[8] << 7 | h[9]) + ((h[5] & 0x10) ? 10 : 0);
    }
    guint8 magic[4] = { 0 };
    if (fseek (d->f, start, SEEK_SET) || fread (magic, 1, 4, d->f) != 4)
        return fail (d, err, "too short for a sound file");
    d->kind = !memcmp (magic, "fLaC", 4) ? K_FLAC : !memcmp (magic, "OggS", 4) ? K_OGG : K_MP3;
    d->pkt = av_packet_alloc ();
    d->frame = av_frame_alloc ();
    if (!d->pkt || !d->frame)
        return fail (d, err, "out of memory");

    enum AVCodecID id = AV_CODEC_ID_MP3;
    GByteArray *extra = g_byte_array_new ();
    const char *bad = NULL;
    long data_at = start;
    if (d->kind == K_MP3) {
        const long first = mp3_find (d, start);
        if (first < 0)
            bad = "not an MP3, FLAC, Ogg or WAV file";
        else
            data_at = first + mp3_info_frame (d, first);
    } else if (d->kind == K_FLAC) {
        id = AV_CODEC_ID_FLAC;
        if (fseek (d->f, start + 4, SEEK_SET))
            bad = "FLAC: the file ends in its header";
        /* metadata blocks: 1 byte (last flag, type), 3 bytes length */
        for (int i = 0; i < 1024 && !bad; i++) {
            guint8 bh[4];
            if (fread (bh, 1, 4, d->f) != 4) {
                bad = "FLAC: the file ends in its header";
                break;
            }
            long len = (long) bh[1] << 16 | bh[2] << 8 | bh[3];
            if ((bh[0] & 0x7f) == 0 && len == 34 && !extra->len) {
                guint8 si[34];
                if (fread (si, 1, 34, d->f) != 34) {
                    bad = "FLAC: the file ends in its header";
                } else {
                    g_byte_array_append (extra, si, 34);
                    /* (for walpe_sounddec_skip: the smallest and the largest
                     * frame in samples, the largest in bytes, all samples) */
                    const int smin = si[0] << 8 | si[1], smax = si[2] << 8 | si[3];
                    d->fl_block = smin == smax ? smax : 0;
                    d->fl_frame_max = si[7] << 16 | si[8] << 8 | si[9];
                    d->fl_total = (gint64) (si[13] & 15) << 32 | (gint64) si[14] << 24 | si[15] << 16 |
                                  si[16] << 8 | si[17];
                }
            } else if (fseek (d->f, len, SEEK_CUR)) {
                bad = "FLAC: the file ends in its header";
            }
            if (bh[0] & 0x80)
                break;
        }
        if (!bad && !extra->len)
            bad = "FLAC: no stream description";
        data_at = -1;   /* the frames follow the blocks: the file is there */
        {
            struct stat st;
            d->fl_data = (gint64) ftello (d->f);
            d->fl_size = fstat (fileno (d->f), &st) == 0 ? (gint64) st.st_size : 0;
        }
    } else {
        data_at = -1;
        if (fseek (d->f, start, SEEK_SET))
            bad = "Ogg: no first packet";
        d->opkt = g_byte_array_new ();
        if (bad || ogg_packet (d) != 1) {
            bad = "Ogg: no first packet";
        } else if (d->opkt->len >= 30 && !memcmp (d->opkt->data, "\x01vorbis", 7)) {
            /* Vorbis: three header packets, handed over in Xiph lacing. The
             * second one is the comment (titles, sometimes a cover picture
             * of megabytes): the decoder does not read it, an empty one
             * stands in for it. */
            static const guint8 no_comment[16] = { 3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1 };
            id = AV_CODEC_ID_VORBIS;
            /* the identification header: version (4 bytes), channels (1),
             * sample rate (4). The limits are checked before a decoder is
             * made: one for 255 channels would be set up and would decode
             * megabytes before its first frame is turned down. */
            const guint vch = d->opkt->data[11];
            const guint32 vhz = rd_le32 (d->opkt->data + 12);
            if (vch < 1 || vch > WALPE_SOUND_MAX_CHANNELS)
                bad = "Ogg Vorbis: channel count outside 1..8";
            else if (vhz < WALPE_SOUND_MIN_RATE || vhz > WALPE_SOUND_MAX_RATE)
                bad = "sample rate outside 8000..192000 Hz";
            GByteArray *p[3] = { g_byte_array_new (), g_byte_array_new (), g_byte_array_new () };
            g_byte_array_append (p[0], d->opkt->data, d->opkt->len);
            if (!bad) {
                int r = ogg_packet (d);
                if (r == 0 || d->opkt->len < 7 || memcmp (d->opkt->data + 1, "vorbis", 6))
                    bad = "Ogg Vorbis: header packets missing";
                else
                    g_byte_array_append (p[1], no_comment, sizeof no_comment);
            }
            if (!bad) {
                if (ogg_packet (d) != 1 || d->opkt->len < 7 || memcmp (d->opkt->data + 1, "vorbis", 6))
                    bad = "Ogg Vorbis: header packets missing";
                else if (d->opkt->len > VORBIS_SETUP_MAX || vorbis_books_too_large (d->opkt->data, d->opkt->len))
                    bad = "Ogg Vorbis: code tables larger than an encoder writes them";
                else
                    g_byte_array_append (p[2], d->opkt->data, d->opkt->len);
            }
            if (!bad) {
                guint8 two = 2;
                g_byte_array_append (extra, &two, 1);
                for (int i = 0; i < 2; i++) {
                    guint l = p[i]->len;
                    guint8 ff = 255;
                    for (; l >= 255; l -= 255)
                        g_byte_array_append (extra, &ff, 1);
                    guint8 last = (guint8) l;
                    g_byte_array_append (extra, &last, 1);
                }
                for (int i = 0; i < 3; i++)
                    g_byte_array_append (extra, p[i]->data, p[i]->len);
            }
            for (int i = 0; i < 3; i++)
                g_byte_array_free (p[i], TRUE);
        } else if (d->opkt->len >= 19 && !memcmp (d->opkt->data, "OpusHead", 8)) {
            id = AV_CODEC_ID_OPUS;
            g_byte_array_append (extra, d->opkt->data, d->opkt->len);
            /* pre-skip: the decoder drops that many samples at the start by
             * itself; the positions of the pages count them */
            d->pre = d->opkt->data[10] | d->opkt->data[11] << 8;
            /* OpusHead: channels at byte 9; with a channel mapping (byte
             * 18 not 0) the number of streams in every packet at byte 19 -
             * up to 255, each with a decoder of its own, also when only
             * two channels are made of them */
            if (d->opkt->data[9] < 1 || d->opkt->data[9] > WALPE_SOUND_MAX_CHANNELS ||
                (d->opkt->data[18] && d->opkt->len >= 21 && d->opkt->data[19] > WALPE_SOUND_MAX_CHANNELS))
                bad = "Ogg Opus: channel count outside 1..8";
            else if (ogg_packet (d) == 0)      /* OpusTags (its content is not used) */
                bad = "Ogg Opus: header packets missing";
        } else {
            bad = "Ogg: neither Vorbis nor Opus";
        }
        d->end = 0;   /* (a last-page flag on a header page says nothing) */
        d->past_headers = TRUE;
    }
    if (!bad && data_at >= 0 && fseek (d->f, data_at, SEEK_SET))
        bad = "too short for a sound file";
    d->consumed = 0;
    const AVCodec *codec = bad ? NULL : avcodec_find_decoder (id);
    if (!bad && !codec)
        bad = "this FFmpeg has no decoder for the file";
    if (!bad) {
        d->ctx = avcodec_alloc_context3 (codec);
        if (!d->ctx || (extra->len && !set_extradata (d->ctx, extra->data, extra->len)))
            bad = "out of memory";
    }
    g_byte_array_free (extra, TRUE);
    if (!bad) {
        /* What the decoder has to say about a file's damage is not printed.
         * FFmpeg writes its messages straight to stderr, past the daemon's
         * log guard (50 lines a second, 16 MiB of file): one line or more
         * for every packet it refuses, up to 400 packets in a row here, and
         * a loop layer plays a damaged file again and again - tens of
         * thousands of lines a second into the daemon's log from one
         * layer, for as long as the wallpaper is shown. The offset is added
         * to the level of every message logged for this context (the
         * parser's too): 64 puts errors (16) above the most verbose level
         * there is (56). Why a file is refused is said by this file's own
         * message. */
        d->ctx->log_level_offset = 64;
        d->ctx->thread_count = 1;
        d->ctx->request_sample_fmt = AV_SAMPLE_FMT_S16;
        if (avcodec_open2 (d->ctx, codec, NULL) < 0)
            bad = "the decoder refuses the file";
    }
    if (!bad && d->kind != K_OGG) {
        d->parser = av_parser_init (id);
        if (!d->parser)
            bad = "this FFmpeg has no parser for the file";
    }
    if (bad)
        return fail (d, err, bad);
    /* the first frame tells rate and channels (and that it is sound at all) */
    if (next_frame (d) != 1)
        return fail (d, err, d->kind == K_MP3 ? "not an MP3, FLAC, Ogg or WAV file" : "no sound could be decoded");
    d->rate = d->frame->sample_rate;
    d->channels = MIN (frame_channels (d->frame), 2);
    if (d->rate < WALPE_SOUND_MIN_RATE || d->rate > WALPE_SOUND_MAX_RATE)
        return fail (d, err, "sample rate outside 8000..192000 Hz");
    *rate = d->rate;
    *channels = d->channels;
    return d;
}
