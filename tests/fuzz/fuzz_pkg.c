/* fuzz_pkg - the .pkg archive reader (src/engine/pkg.c, walpe_pkg_extract).
 * The input is the archive. Nothing is written: the two calls that create
 * directories and files are replaced, and they check what the extraction
 * has to guarantee - every name it writes lies below the destination, and
 * the bytes it writes lie inside the archive (the sanitizer reads them). */
#include "fuzz_common.h"
#include <glib/gstdio.h>

#define FUZZ_DEST "/walpe-fuzz-dest"
static guint64 fuzz_written;
static guint fuzz_files;

static void fuzz_name_ok (const char *path)
{
    FUZZ_CHECK (g_str_has_prefix (path, FUZZ_DEST "/"));
    char **parts = g_strsplit (path + strlen (FUZZ_DEST), "/", -1);
    for (char **c = parts; *c; c++)
        FUZZ_CHECK (strcmp (*c, "..") != 0);
    g_strfreev (parts);
}

static int fuzz_mkdir (const char *path, int mode)
{
    (void) mode;
    char *with = g_strconcat (path, "/", NULL);
    fuzz_name_ok (with);
    g_free (with);
    return 0;
}

static gboolean fuzz_set_contents (const char *path, const char *data, gssize len, GError **err)
{
    (void) err;
    fuzz_name_ok (path);
    FUZZ_CHECK (len >= 0);
    fuzz_touch (data, (size_t) len);
    fuzz_written += (guint64) len;
    fuzz_files++;
    return TRUE;
}

#define g_mkdir_with_parents fuzz_mkdir
#define g_file_set_contents fuzz_set_contents
#include "../../src/engine/pkg.c"
#undef g_mkdir_with_parents
#undef g_file_set_contents

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
    fuzz_written = 0;
    fuzz_files = 0;
    gboolean ok = walpe_pkg_extract (path, FUZZ_DEST, &err);
    FUZZ_CHECK (ok || err);
    /* what one archive may write: a few times its own size (pkg.c) */
    FUZZ_CHECK (fuzz_written <= (guint64) size * 4 + (16u << 20));
    FUZZ_CHECK (fuzz_files <= PKG_MAX_OBJECTS);
    g_clear_error (&err);
    return 0;
}
