/* fuzz_mp4 - the MP4 reader of video wallpapers and video textures
 * (src/engine/mp4demux.c): box tree, sample tables, avcC. The input is the
 * file; it is opened whole and once more as a range that starts inside it
 * (a video stored in a .tex), and the first samples are read. */
#include "fuzz_common.h"
#include "engine/mp4demux.h"

static void walk (Mp4Demux *d)
{
    FUZZ_CHECK (d->nsamples >= 1 && d->nsamples <= 4194304);
    FUZZ_CHECK (d->timescale >= 1 && d->timescale <= 0x7fffffffu);
    fuzz_touch (d->sps, d->sps_len);
    fuzz_touch (d->pps, d->pps_len);
    fuzz_touch (d->samples, (gsize) d->nsamples * sizeof (Mp4Sample));
    guint n = MIN (d->nsamples + 2, 300u);
    for (guint i = 0; i < n; i++) {
        const Mp4Sample *s = mp4_demux_next (d);
        if (!s)
            break;
        fuzz_touch (s->data, s->size);
    }
}

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    const char *path = fuzz_file (0, data, size);
    GError *err = NULL;
    Mp4Demux *d = mp4_demux_open (path, &err);
    FUZZ_CHECK (d || err);
    g_clear_error (&err);
    if (d) {
        walk (d);
        mp4_demux_free (d);
    }
    if (size > 16) {
        /* the first bytes say where the range lies */
        guint64 off = data[0] % 16, len = (guint64) data[1] | ((guint64) data[2] << 8) |
                                          ((guint64) data[3] << 16);
        d = mp4_demux_open_range (path, 4 + off, data[4] & 1 ? len : 0, NULL);
        if (d) {
            walk (d);
            mp4_demux_free (d);
        }
    }
    return 0;
}
