/* fuzz_jpegstore - walpe_jpeg_store_bytes (src/common/util.c) against the
 * decoder it speaks for. The function reads a JPEG's markers and says how
 * many bytes libjpeg will hold before the first pixel comes out (a
 * progressive file, or one with a scan per component, is read whole into a
 * store sized by the DECLARED picture); callers refuse a file whose store
 * is beyond WALPE_JPEG_STORE_MAX. Nothing else stands between a file of a
 * hundred bytes and gigabytes, so the number has to be an upper bound.
 *
 * Here every input that the function lets through is decoded the way a
 * preview is (walpe_picture_decode, a caller that accepts up to 4096 px a
 * side and asks for 160), and what the heap grew by during the decode is
 * compared with the function's answer: more than the answer plus a fixed
 * allowance for the decoder's working buffers ends the run. The heap is
 * measured through the sanitizer's allocator (a build without it only
 * decodes). */
#include "fuzz_common.h"
#include "common/util.h"
#include <gdk-pixbuf/gdk-pixbuf.h>

#define SIDE_MAX 4096
#define ALLOWANCE ((gint64) 24 << 20)   /* rows, tables, the scaled picture, gdk-pixbuf's own copies */

/* the sanitizer's allocator, when the program has one */
__attribute__ ((weak)) int __sanitizer_install_malloc_and_free_hooks (
    void (*on_malloc) (const volatile void *, size_t), void (*on_free) (const volatile void *));
__attribute__ ((weak)) size_t __sanitizer_get_current_allocated_bytes (void);

static volatile gboolean armed;
static size_t heap_peak;

static void on_malloc (const volatile void *p, size_t n)
{
    (void) p; (void) n;
    if (armed) {
        size_t now = __sanitizer_get_current_allocated_bytes ();
        if (now > heap_peak)
            heap_peak = now;
    }
}

static void on_free (const volatile void *p)
{
    (void) p;
}

static gboolean size_cb (const char *kind, int w, int h, int *sw, int *sh, gpointer user)
{
    (void) kind; (void) user;
    if (w < 1 || h < 1 || w > SIDE_MAX || h > SIDE_MAX)
        return FALSE;
    if (MAX (w, h) > 160) {
        double k = 160.0 / MAX (w, h);
        *sw = MAX (1, (int) (w * k));
        *sh = MAX (1, (int) (h * k));
    }
    return TRUE;
}

static gboolean measured;

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    measured = __sanitizer_install_malloc_and_free_hooks && __sanitizer_get_current_allocated_bytes &&
               __sanitizer_install_malloc_and_free_hooks (on_malloc, on_free) > 0;
    /* (the loader's module and its tables are loaded once: not part of a picture's cost) */
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new_with_type ("jpeg", NULL);
    if (ld) {
        gdk_pixbuf_loader_close (ld, NULL);
        g_object_unref (ld);
    }
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    static const char *const kinds[] = { "jpeg", NULL };
    guint64 store = walpe_jpeg_store_bytes (data, size);
    guint64 store_file = walpe_jpeg_store_bytes_file (fuzz_file (0, data, size));
    FUZZ_CHECK (store == store_file);       /* the two readers agree */

    GError *err = NULL;
    size_t before = measured ? __sanitizer_get_current_allocated_bytes () : 0;
    heap_peak = before;
    armed = measured;
    GdkPixbuf *pb = walpe_picture_decode (data, size, kinds, size_cb, NULL, &err);
    armed = FALSE;
    FUZZ_CHECK (pb || err);
    if (pb) {
        FUZZ_CHECK (store <= WALPE_JPEG_STORE_MAX);     /* nothing refused was decoded */
        FUZZ_CHECK (walpe_jpeg_coding_ok (data, size)); /* (nor an arithmetic-coded one) */
        FUZZ_CHECK (gdk_pixbuf_get_width (pb) <= SIDE_MAX && gdk_pixbuf_get_height (pb) <= SIDE_MAX);
        g_object_unref (pb);
    }
    g_clear_error (&err);
    if (measured) {
        gint64 grew = (gint64) (heap_peak - before);
        gint64 said = store <= WALPE_JPEG_STORE_MAX ? (gint64) store : 0;   /* (refused: not decoded at all) */
        if (g_getenv ("WALPE_FUZZ_LOG"))    /* the two numbers of every input */
            g_printerr ("fuzz_jpegstore: grew %" G_GINT64_FORMAT ", said %" G_GUINT64_FORMAT "%s\n", grew, store,
                        store > WALPE_JPEG_STORE_MAX ? " (refused)" : "");
        if (grew > said + ALLOWANCE) {
            g_printerr ("fuzz_jpegstore: the heap grew by %" G_GINT64_FORMAT " bytes, walpe_jpeg_store_bytes said %"
                        G_GUINT64_FORMAT "\n", grew, store);
            abort ();
        }
    }
    return 0;
}
