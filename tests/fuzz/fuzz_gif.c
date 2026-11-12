/* fuzz_gif - the GIF layout check that runs before a GIF reaches its
 * decoder (src/common/gifcheck.c), and the two small picture scanners of
 * src/common/util.c (the format by signature, what a JPEG makes its decoder
 * hold). The check has two readers - of a buffer and of a file in pieces -
 * which have to agree. */
#include "fuzz_common.h"
#include "common/gifcheck.h"
#include "common/util.h"

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    GError *e1 = NULL, *e2 = NULL;
    gboolean mem = walpe_gif_layout_ok (data, size, &e1);
    gboolean file = walpe_gif_file_layout_ok (fuzz_file (0, data, size), &e2);
    FUZZ_CHECK (mem || e1);
    FUZZ_CHECK (file || e2);
    if (size >= 6 && !memcmp (data, "GIF8", 4))
        FUZZ_CHECK (mem == file);
    g_clear_error (&e1);
    g_clear_error (&e2);
    walpe_picture_kind (data, size);
    walpe_jpeg_store_bytes (data, size);
    return 0;
}
