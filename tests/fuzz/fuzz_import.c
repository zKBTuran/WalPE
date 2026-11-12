/* fuzz_import - what is read about a video before it is converted
 * (src/common/import.c: ffprobe's JSON for a file the user imports or for
 * a clip inside a scene; the output size, the stream-copy decision and the
 * ffmpeg command line made from it) and the parts of the scene-video copies
 * that take numbers and bytes of a wallpaper (src/common/scenevideo.c: the
 * copy's size, the identity of a clip = a digest of bytes of its file).
 * The input is ffprobe's output; its first bytes also give the numbers. */
#include "fuzz_common.h"
#include "common/import.h"
#include "common/scenevideo.h"

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    return 0;
}

static int num (const uint8_t *d, size_t n, size_t at)
{
    guint32 v = 0;
    for (size_t i = 0; i < 4; i++)
        v = v << 8 | (at + i < n ? d[at + i] : 0);
    return (int) v;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    static const WalpeVideoBox boxes[] = { { 0, 0 }, { 1920, 1080 }, { 2, 2 }, { 16384, 16384 }, { 641, 359 } };
    char *text = g_strndup ((const char *) data, size);
    WalpeMediaInfo mi = { 0 };
    if (walpe_media_info_parse (text, &mi)) {
        for (guint b = 0; b < G_N_ELEMENTS (boxes); b++) {
            int w = 0, h = 0;
            walpe_import_target_size (&mi, &boxes[b], &w, &h);
            FUZZ_CHECK (w >= 0 && h >= 0);
            gboolean remux = walpe_import_can_remux (&mi, &boxes[b]);
            g_strfreev (walpe_import_build_argv ("/in/a b.mkv", "/out/x.part", &mi, "libx264", remux, &boxes[b]));
            g_strfreev (walpe_import_build_argv ("-in.webm", "out.part", &mi, "h264_vaapi", FALSE, &boxes[b]));
        }
    }
    walpe_media_info_clear (&mi);
    g_free (text);

    int ow = 0, oh = 0;
    WalpeVideoBox box = { num (data, size, 8), num (data, size, 12) };
    if (walpe_scene_video_target (num (data, size, 0), num (data, size, 4), size & 1 ? &box : NULL,
                                  num (data, size, 16), num (data, size, 20),
                                  (double) num (data, size, 24) / 1000.0, &ow, &oh))
        FUZZ_CHECK (ow > 0 && oh > 0);
    WalpeImportScene sc = { (guint64) (guint32) num (data, size, 0), (guint64) (guint32) num (data, size, 4),
                            num (data, size, 8), num (data, size, 12), size > 16 ? data[16] % 12 : 1 };
    g_strfreev (walpe_import_scene_argv ("/in/clip.tex", &sc, "/out/copy.part", "libx264"));
    const char *path = fuzz_file (0, data, size);
    char *id = walpe_scene_video_id (path, sc.off % (size + 2), sc.len % (size + 2));
    if (id) {
        g_free (walpe_scene_video_cache_file (id, ow, oh));
        g_free (id);
    }
    g_free (walpe_scene_video_id (path, 0, 0));
    return 0;
}
