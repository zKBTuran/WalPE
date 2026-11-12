/* standalone.c - main () for a fuzz harness without libFuzzer: every file
 * named on the command line (every file of a named directory) is given to
 * LLVMFuzzerTestOneInput () once. This is how the regression inputs under
 * tests/fuzz/regress run in the normal test suite, with any compiler. */
#include "fuzz_common.h"
#include <glib/gstdio.h>
#include <stdio.h>

static int runs;

/* Under the address sanitizer a single allocation of more than 1 GiB ends
 * the run: no parser has a reason to ask for that much for a wallpaper file
 * (the largest texture is 256 MiB decoded), and "a few bytes of file make a
 * decoder reserve gigabytes" is a finding of the fuzzers like any other
 * (libFuzzer's -malloc_limit_mb says the same there). Without the sanitizer
 * this function is never called. */
const char *__asan_default_options (void);
__attribute__ ((visibility ("default"), used)) const char *__asan_default_options (void)
{
    return "max_allocation_size_mb=1024";
}

static int by_name (gconstpointer a, gconstpointer b)
{
    return strcmp (*(const char *const *) a, *(const char *const *) b);
}

static void one (const char *path)
{
    char *d = NULL;
    gsize n = 0;
    if (!g_file_get_contents (path, &d, &n, NULL)) {
        fprintf (stderr, "cannot read %s\n", path);
        exit (1);
    }
    printf ("%s (%zu bytes)\n", path, n);
    fflush (stdout);
    /* an exact allocation: reads past the end are seen by the sanitizer */
    uint8_t *copy = malloc (n ? n : 1);
    memcpy (copy, d, n);
    g_free (d);
    LLVMFuzzerTestOneInput (copy, n);
    free (copy);
    runs++;
}

int main (int argc, char **argv)
{
    LLVMFuzzerInitialize (&argc, &argv);
    for (int i = 1; i < argc; i++) {
        if (!g_file_test (argv[i], G_FILE_TEST_IS_DIR)) {
            if (g_file_test (argv[i], G_FILE_TEST_EXISTS))
                one (argv[i]);
            continue;
        }
        GDir *dir = g_dir_open (argv[i], 0, NULL);
        GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
        const char *n;
        while (dir && (n = g_dir_read_name (dir)))
            if (n[0] != '.')
                g_ptr_array_add (names, g_build_filename (argv[i], n, NULL));
        if (dir)
            g_dir_close (dir);
        g_ptr_array_sort (names, by_name);
        for (guint k = 0; k < names->len; k++)
            one (g_ptr_array_index (names, k));
        g_ptr_array_free (names, TRUE);
    }
    printf ("%d inputs, no failure\n", runs);
    return 0;
}
