/* fuzz_layerpic - a picture file that a scene names for a layer or a
 * particle sprite and that the texture loader does not take
 * (src/engine/glutil.c walpe_gl_tex_from_file: layers of the old scene
 * format, sprites of the built-in particle system). The format is read from
 * the file's first bytes - from its name where they say nothing -, the size
 * from the header by the decoder of that one format, and the picture is
 * decoded by it, scaled down when it is larger than one texture may be, and
 * uploaded.
 *
 * The first byte of the input is the file's name (none, or one with the
 * ending of each accepted format: a file that is not what its name says is
 * the case the name rule is about), the rest is the file. OpenGL is the
 * stand-in of fakegl.c, which reads every byte of an upload.
 *
 * Checked here: a texture that comes back has a size inside the limits of
 * one texture. What the sanitizer and the allocation limit add: a decoder
 * that reserves memory by a number of the header before the size was looked
 * at ends the run - the BMP files of tests/fuzz/regress/layerpic are four
 * of that kind, which the check of BMP header sizes has to keep away from
 * gdk-pixbuf's BMP decoder on this route too.
 *
 * One allocation is as intended and large: a picture in a format other
 * than JPEG is decoded at the size it declares before it is scaled, up to
 * 16384 x 16384 pixels (1 GiB). Run the libFuzzer program with
 * -malloc_limit_mb=1100 so that only requests beyond that are reported. */
#include "fuzz_common.h"
#include "engine/glutil.h"
#include <epoxy/gl.h>

static const char *const names[] = { "a.png", "a.jpg", "a.gif", "a.bmp", "a.tif", "a.webp", "a.tga", "a.avif",
                                     "a.jxl", "a.JPEG", "a.tiff", "a" };
static char *paths[1 + G_N_ELEMENTS (names)];
static const char *dir;     /* (held here for the leak check: it lives as long as the process) */

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    /* the names are links to the memory file that holds the input */
    dir = fuzz_dir ("layerpic");
    paths[0] = g_strdup (fuzz_file (0, (const uint8_t *) "", 0));
    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        paths[i + 1] = g_build_filename (dir, names[i], NULL);
        if ((unlink (paths[i + 1]), symlink (paths[0], paths[i + 1])) != 0)
            abort ();
    }
    walpe_glutil_init ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    const char *path = paths[data[0] % G_N_ELEMENTS (paths)];
    fuzz_file (0, data + 1, size - 1);
    WalpeGlTex t = walpe_gl_tex_from_file (path);
    if (t.tex) {
        FUZZ_CHECK (t.width >= 1 && t.height >= 1 && t.width <= 16384 && t.height <= 16384 &&
                    (gint64) t.width * t.height <= (gint64) 8192 * 8192);
        glDeleteTextures (1, &t.tex);
    }
    return 0;
}
