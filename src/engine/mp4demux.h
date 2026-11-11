/* mp4demux.h - minimal in-house MP4 (ISO-BMFF) demuxer for H.264 wallpapers.
 * Parses moov/trak/stbl and exposes the video samples (AVCC format, i.e.
 * length-prefixed NAL units) plus the avcC record for the decoder. No
 * libavformat, no audio support: wallpapers are silent. */
#pragma once
#include <glib.h>
#include "common/api.h"

typedef struct {
    const guint8 *data; /* AVCC sample (NAL units with avcC length prefixes);
                         * set by mp4_demux_next: the demuxer's packet
                         * buffer, valid until the next call */
    guint size;
    guint8 sync;    /* a keyframe by the file's sync sample table (stss);
                     * only meaningful when Mp4Demux.has_sync */
    double pts;     /* seconds (decode order; add cts_off for display time) */
    gint64 cts_off; /* composition offset in timescale ticks (B-frames) */
    guint64 off;    /* position of the sample in the file */
} Mp4Sample;

typedef struct {
    guint8 *sps;    /* sequence parameter set (Annex B header not included) */
    gsize sps_len;
    guint8 *pps;    /* picture parameter set */
    gsize pps_len;
    int width, height;
    /* how the track is meant to be shown; the player draws frames as
     * decoded, so anything but 0 / 1:1 needs the conversion (daemon) */
    int rotation;           /* display matrix: 0 / 90 / 180 / 270, -1 = another transform (flip) */
    guint sar_num, sar_den; /* pixel aspect ratio (pasp); 0:0 or n:n = square */
    guint timescale;        /* 1 .. 2^31 - 1 (a file with more is refused) */
    double duration;        /* as the file says (mdhd): not checked against the samples */
    Mp4Sample *samples;
    guint nsamples;         /* 1 .. 4194304 (a track with more is refused); the sum of all
                             * sample durations is below 2^54 ticks */
    guint cursor;   /* next sample index */
    /* samples are read from the file when asked for (pread into pkt). They
     * used to be pointers into a mapping of the file: a file shortened
     * while it played (an interrupted copy over it, a sync tool, a
     * removable disk going away) then killed the daemon with SIGBUS. */
    int fd;
    guint8 *pkt;
    gsize pkt_cap;
    gboolean io_failed; /* a sample could not be read: logged once */
    gboolean has_sync; /* the file lists its keyframes (Mp4Sample.sync) */
} Mp4Demux;

/* opens path, parses the moov, builds the video sample table.
 * NULL on error (not an mp4 / no H.264 video track). */
Mp4Demux *mp4_demux_open (const char *path, GError **err);
/* same, for an MP4 embedded at [off, off+len) of path (len 0 = to EOF) */
WALPE_API Mp4Demux *mp4_demux_open_range (const char *path, guint64 off, guint64 len, GError **err);
WALPE_API void mp4_demux_free (Mp4Demux *d);

/* next sample (owned by the demuxer, its data valid until the next call),
 * NULL at end of stream or when the sample cannot be read any more; loops
 * by rewinding cursor. */
WALPE_API const Mp4Sample *mp4_demux_next (Mp4Demux *d);
