/* fuzz_sound - a scene's sound files as a player reads them
 * (src/engine/scenesound.c: the WAV reader; src/engine/sounddec.c: MP3 with
 * ID3v2 / Xing / LAME, FLAC, Ogg pages and packets with Vorbis and Opus in
 * front of libavcodec's decoders). The input is the file; it is opened the
 * way a player opens it and read to its end (at most ten seconds of sound).
 * scenesound.c is included for its file reader. Without FFmpeg
 * (FUZZ_HAVE_FFMPEG undefined) only WAV files are read. */
#include "../../src/engine/scenesound.c"
#include "fuzz_common.h"

#ifdef FUZZ_HAVE_FFMPEG
static const WalpeSoundDecoder decoder = {
    .open = walpe_sounddec_open, .read = walpe_sounddec_read, .close = walpe_sounddec_close,
};
#endif

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
#ifdef FUZZ_HAVE_FFMPEG
    walpe_sound_set_decoder (&decoder);
#endif
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    Src src;
    char *err = NULL;
    if (!src_open (&src, fuzz_file (0, data, size), &err)) {
        FUZZ_CHECK (err != NULL);
        g_free (err);
        return 0;
    }
    FUZZ_CHECK (src.rate >= WALPE_SOUND_MIN_RATE && src.rate <= WALPE_SOUND_MAX_RATE);
    FUZZ_CHECK (src.channels == 1 || src.channels == 2);
    gint16 out[2 * 1024];
    gint64 total = 0, limit = (gint64) src.rate * 10;
    int n, calls = 0;
    while (total < limit && calls++ < 4096 && (n = src_read (&src, out, 1024)) > 0) {
        fuzz_touch (out, (size_t) n * (size_t) src.channels * sizeof (gint16));
        total += n;
    }
    src_close (&src);
    return 0;
}
