/* fuzz_pichead - what a picture file is and how large, as WalPE reads it
 * from the file's own header before gdk-pixbuf is asked anything
 * (src/common/pichead.c): the signatures, the size readers of every format
 * (PNG, JPEG, GIF, BMP, WebP, TGA; the directory of a TIFF, the boxes of an
 * AVIF, the size header of a JPEG XL codestream and its container), the TGA
 * header a decoder is given. The input is the file. No decoder runs here:
 * these readers are what stands in front of one.
 *
 * Every size reader is asked about every input, whatever it is - a caller
 * names the format, the bytes need not be it. The two readers (of a
 * buffer, of a file read where asked) have to agree. */
#include "fuzz_common.h"
#include "common/pichead.h"

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    static const char *const kinds[] = { "png", "jpeg", "gif", "bmp", "webp", "tga", "tiff", "avif", "jxl" };
    const char *path = fuzz_file (0, data, size);
    for (guint i = 0; i < G_N_ELEMENTS (kinds); i++) {
        int w = -1, h = -1, fw = -1, fh = -1;
        const gboolean mem = walpe_pichead_size (kinds[i], data, size, &w, &h);
        const gboolean file = walpe_pichead_file_size (kinds[i], path, &fw, &fh);
        FUZZ_CHECK (mem == file);
        if (mem)
            FUZZ_CHECK (w >= 1 && h >= 1 && w == fw && h == fh);
    }

    WalpeTga t, again;
    if (walpe_tga_head (data, size, &t)) {
        FUZZ_CHECK (size >= 18 && t.w >= 1 && t.h >= 1 && t.w <= 65535 && t.h <= 65535);
        FUZZ_CHECK (t.head[1] <= 1 && t.head[2] == data[2] && t.head[16] == data[16]);
        FUZZ_CHECK (t.skip_len <= (gsize) 65535 * 4 && (t.skip_len == 0 || t.skip_off >= 18));
        FUZZ_CHECK (t.changed == (t.skip_len > 0 || memcmp (t.head, data, 18) != 0));
        /* the header a decoder is given is one, and its own */
        FUZZ_CHECK (walpe_tga_head (t.head, 18, &again) && !again.changed && again.w == t.w && again.h == t.h);
    }

    /* the file's kind is the signature's wherever there is one (the name
     * here says nothing) */
    const char *kind = walpe_pichead_kind (data, size);
    const char *of_file = walpe_pichead_file_kind (path, &t);
    if (kind)
        FUZZ_CHECK (of_file == kind);
    else
        FUZZ_CHECK (!of_file || !strcmp (of_file, "tga"));
    walpe_pichead_file_other (path);
    walpe_pichead_is_bigtiff (data, size);
    return 0;
}
