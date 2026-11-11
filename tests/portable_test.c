/* portable_test.c - what must come out the same on x86-64 and aarch64.
 *
 * The two architectures disagree where C lets them: plain char is signed
 * on one and unsigned on the other, a float outside the int range converts
 * to different numbers, a byte pointer cast to a wider type may or may not
 * be read at an odd address. This test pins the results of the code that
 * could be affected, as numbers worked out without a C compiler, so that
 * a build which passes on both architectures has shown they agree:
 *   - the byte readers of common/portable.h at every alignment;
 *   - the hash functions whose values are kept somewhere: the hash of an
 *     effect pass's shader text (it selects rows of the pinned table
 *     engine/fx_region_models.h), the program cache key, the FNV loop
 *     behind the frame signatures - each also over bytes >= 0x80, which
 *     come out differently if a char is ever widened with its sign;
 *   - text with bytes >= 0x80 through the functions that read text byte
 *     by byte (the JSON nesting count, the label cleaner);
 *   - numbers beyond the int range and NaN through the two defined
 *     conversions (util.h walpe_d2i, the effects' fx_f2i);
 *   - the JSON nesting limit on a thread with a small stack: the limit was
 *     chosen with x86-64 stack frames in mind, an aarch64 frame is larger;
 *   - the page size the kernel reports (4, 16 or 64 KiB on aarch64).
 * It prints what this platform is, for the test log.
 *
 * The engine file is included so the static functions are reachable.
 * No display, no GL context, no files. */
#include "../src/engine/scenenode_effects.c"
#include "common/portable.h"
#include "common/uitext.h"
#include "engine/cpuphase.h"
#include <pthread.h>
#include <limits.h>
#include <float.h>
#include <math.h>

/* WPJ_MAX_DEPTH of common/util.c: the deepest nesting its reader accepts */
#define WPJ_TEST_DEPTH 512

/* ---- byte readers ------------------------------------------------------ */

static void t_readers (void)
{
    static const guint8 pat[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    /* the same eight bytes at eight different alignments */
    guint64 store[3];
    guint8 *base = (guint8 *) store;
    for (int off = 0; off < 8; off++) {
        guint8 *p = base + off;
        memcpy (p, pat, 8);
        g_assert_cmphex (walpe_rd16le (p), ==, 0x2301);
        g_assert_cmphex (walpe_rd16be (p), ==, 0x0123);
        g_assert_cmphex (walpe_rd32le (p), ==, 0x67452301u);
        g_assert_cmphex (walpe_rd32be (p), ==, 0x01234567u);
        g_assert_cmphex (walpe_rd64le (p), ==, 0xefcdab8967452301ull);
        g_assert_cmphex (walpe_rd64be (p), ==, 0x0123456789abcdefull);
        /* the high bytes must not drag a sign along */
        g_assert_cmphex (walpe_rd16le (p + 6), ==, 0xefcd);
        g_assert_cmphex (walpe_rd32le (p + 4), ==, 0xefcdab89u);
        g_assert_cmphex (walpe_rd32be (p + 4), ==, 0x89abcdefu);
        walpe_wr32le (p, 0x80ff7f01u);
        g_assert_cmpmem (p, 4, "\x01\x7f\xff\x80", 4);
        g_assert_cmphex (walpe_rd32le (p), ==, 0x80ff7f01u);
    }
    /* IEEE 754 single precision, little-endian */
    static const guint8 one[4] = { 0x00, 0x00, 0x80, 0x3f };
    static const guint8 mhalf[4] = { 0x00, 0x00, 0x00, 0xbf };
    static const guint8 big[4] = { 0xff, 0xff, 0x7f, 0x7f };
    g_assert_cmpfloat (walpe_rdf32le (one), ==, 1.0f);
    g_assert_cmpfloat (walpe_rdf32le (mhalf), ==, -0.5f);
    g_assert_cmpfloat (walpe_rdf32le (big), ==, FLT_MAX);
    static const guint8 nan_q[4] = { 0x00, 0x00, 0xc0, 0x7f };
    g_assert_true (isnan (walpe_rdf32le (nan_q)));
}

/* ---- hashes ------------------------------------------------------------ */

static const char *const hash_text[4][2] = {
    { "", "" },
    { "a", "b" },
    { "void main(){}", "uniform sampler2D g_Texture0;" },
    { "\xc3\xa9\xff\x80", "\xe2\x80\xa8\xfe" },      /* bytes >= 0x80 */
};

static void t_hashes (void)
{
    static const guint64 want_fxr[4] = {
        0x44bda7d473cdca4dull, 0xe1b300510df888c4ull,
        0xdfadb13a379a43bfull, 0x3ebf54d70742be68ull,
    };
    static const guint64 want_key[4][2] = {
        { 0x0a99c907b6f64763ull, 0x361ff9cb04c42dfaull },
        { 0xd2b371819297f98aull, 0x6e41eeb20ada1953ull },
        { 0xa60f3dd896bc405bull, 0xdc2a89946e36a216ull },
        { 0x1b5848e8ee896b38ull, 0x368ee3ce847008bdull },
    };
    for (int i = 0; i < 4; i++) {
        g_assert_cmphex (fxr_hash (hash_text[i][0], hash_text[i][1]), ==, want_fxr[i]);
        guint64 key[2];
        fxs_text_key (hash_text[i][0], hash_text[i][1], key);
        g_assert_cmphex (key[0], ==, want_key[i][0]);
        g_assert_cmphex (key[1], ==, want_key[i][1]);
    }
    /* the FNV loop shared by the frame signature, the effect inputs and
     * the particle state hash (their offset is the engine's own, not the
     * published FNV one: the values above and the model table depend on
     * it) */
    g_assert_cmphex (walpe_cph_fnv (0, "", 0), ==, 0x14650fb0739d0383ull);
    g_assert_cmphex (walpe_cph_fnv (0, "a", 1), ==, 0x44bd8ad473cd9906ull);
    g_assert_cmphex (walpe_cph_fnv (0, "foobar", 6), ==, 0x88fad7c0a8ff07f2ull);
    g_assert_cmphex (walpe_cph_fnv (0, "\x80\xff\x00\x7f", 4), ==, 0x23b59479598a8b2bull);
    /* the table those hashes are looked up in ends with its 0 row and has
     * no row twice (a row is found by its hash alone) */
    gsize n = G_N_ELEMENTS (fxr_models);
    g_assert_cmpuint (n, >, 1);
    g_assert_cmphex (fxr_models[n - 1].hash, ==, 0);
    for (gsize i = 0; i + 1 < n; i++)
        for (gsize j = i + 1; j + 1 < n; j++)
            g_assert_cmphex (fxr_models[i].hash, !=, fxr_models[j].hash);
}

/* ---- text with high bytes ---------------------------------------------- */

static void t_text (void)
{
    /* brackets inside a string that also holds bytes >= 0x80 are text,
     * not nesting, and the bytes themselves end nothing */
    GString *s = g_string_new ("{\"k\":\"\xc3\xa9\xe2\x80\xa8");
    for (int i = 0; i < 4000; i++)
        g_string_append (s, "[{\xc3\xbf");
    g_string_append (s, "\",\"n\":[[1,2],{\"x\":\"\xf0\x9f\x98\x80\"}]}");
    g_assert_true (g_utf8_validate (s->str, (gssize) s->len, NULL));
    JsonNode *n = wpj_parse_data (s->str, (gssize) s->len);
    g_assert_nonnull (n);
    g_assert_true (JSON_NODE_HOLDS_OBJECT (n));
    json_node_unref (n);
    g_string_free (s, TRUE);

    /* the label cleaner: control characters go (as code points: U+0080 to
     * U+009F are C1 controls, the BYTES 0x80..0x9f inside a character are
     * not), everything else stays byte for byte */
    char *t = walpe_ui_text ("caf\xc3\xa9\x01 \xc2\x85\xe6\x97\xa5\xe6\x9c\xac\x7f!", 100, WALPE_UITEXT_LINE);
    g_assert_cmpstr (t, ==, "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac!");
    g_free (t);
    /* bytes that are no UTF-8 become U+FFFD, the same on both */
    t = walpe_ui_text ("a\xff\x80z", 100, WALPE_UITEXT_LINE);
    g_assert_cmpstr (t, ==, "a\xef\xbf\xbd\xef\xbf\xbdz");
    g_free (t);
}

/* ---- float to int ------------------------------------------------------ */

static void t_float_int (void)
{
    volatile double nan = NAN, inf = INFINITY, big = 3e9, huge = 1e300, tiny = -3e9;
    /* the engine's conversion for numbers out of a wallpaper's JSON */
    g_assert_cmpint (fx_f2i (0.0), ==, 0);
    g_assert_cmpint (fx_f2i (1.9), ==, 1);
    g_assert_cmpint (fx_f2i (-1.9), ==, -1);
    g_assert_cmpint (fx_f2i (big), ==, 2147483520);
    g_assert_cmpint (fx_f2i (huge), ==, 2147483520);
    g_assert_cmpint (fx_f2i (inf), ==, 2147483520);
    g_assert_cmpint (fx_f2i (tiny), ==, -2147483520);
    g_assert_cmpint (fx_f2i (-inf), ==, -2147483520);
    g_assert_cmpint (fx_f2i (nan), ==, 0);
    /* util.h's, for numbers of wallpaper files: what the x86 conversion
     * instruction answers (INT_MIN beyond the range and for NaN), on every
     * architecture */
    g_assert_cmpint (walpe_d2i (0.0), ==, 0);
    g_assert_cmpint (walpe_d2i (-0.99), ==, 0);
    g_assert_cmpint (walpe_d2i (2147483647.9), ==, G_MAXINT);
    g_assert_cmpint (walpe_d2i (-2147483648.9), ==, G_MININT);
    g_assert_cmpint (walpe_d2i (big), ==, G_MININT);
    g_assert_cmpint (walpe_d2i (huge), ==, G_MININT);
    g_assert_cmpint (walpe_d2i (inf), ==, G_MININT);
    g_assert_cmpint (walpe_d2i (tiny), ==, G_MININT);
    g_assert_cmpint (walpe_d2i (-inf), ==, G_MININT);
    g_assert_cmpint (walpe_d2i (nan), ==, G_MININT);
    /* the arithmetic the pixel loops rely on: a product and a sum are
     * rounded one after the other (the build sets -ffp-contract=off). The
     * exact product is 1 - 2^-56 (1 - 2^-26 for float), which rounds to 1;
     * a fused multiply-add would not round it and leave -2^-56 (-2^-26). */
    volatile double a = 1.0 + ldexp (1.0, -28), b = 1.0 - ldexp (1.0, -28), c = -1.0;
    double r = a * b + c;
    g_assert_cmpfloat (r, ==, 0.0);
    volatile float fa = 1.0f + ldexpf (1.0f, -13), fb = 1.0f - ldexpf (1.0f, -13), fc = -1.0f;
    float fr = fa * fb + fc;
    g_assert_cmpfloat (fr, ==, 0.0f);
}

/* ---- the JSON nesting limit on a small stack --------------------------- */

static gpointer deep_json_thread (gpointer data)
{
    const char *text = data;
    JsonNode *n = wpj_parse_data (text, (gssize) strlen (text));
    gboolean ok = n != NULL;
    if (n)
        json_node_unref (n);
    return GINT_TO_POINTER (ok ? 1 : 2);
}

static int deep_json_on_stack (int depth, gsize stack)
{
    /* an object that holds `depth - 1` arrays inside each other */
    GString *s = g_string_new ("{\"a\":");
    for (int i = 1; i < depth; i++)
        g_string_append_c (s, '[');
    g_string_append_c (s, '1');
    for (int i = 1; i < depth; i++)
        g_string_append_c (s, ']');
    g_string_append_c (s, '}');
    pthread_attr_t at;
    pthread_t th;
    void *ret = NULL;
    pthread_attr_init (&at);
    g_assert_cmpint (pthread_attr_setstacksize (&at, stack), ==, 0);
    g_assert_cmpint (pthread_create (&th, &at, deep_json_thread, s->str), ==, 0);
    pthread_join (th, &ret);
    pthread_attr_destroy (&at);
    g_string_free (s, TRUE);
    return GPOINTER_TO_INT (ret);
}

static void t_stack (void)
{
    /* The deepest text the reader accepts, parsed where only 1 MiB of
     * stack exists: an eighth of what glibc gives a thread by default.
     * json-glib recurses once per level; if this overflows on an
     * architecture, the limit is too high for it. */
    g_assert_cmpint (deep_json_on_stack (WPJ_TEST_DEPTH, 1024 * 1024), ==, 1);
    /* ... and one level more is refused before any recursion (the reader
     * says so with a warning) */
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*nested deeper*");
    g_assert_cmpint (deep_json_on_stack (WPJ_TEST_DEPTH + 1, 1024 * 1024), ==, 2);
    g_test_assert_expected_messages ();
}

/* ---- the platform ------------------------------------------------------ */

static void t_platform (void)
{
    gsize page = walpe_page_size ();
    g_assert_cmpuint (page, >=, 4096);
    g_assert_cmpuint (page & (page - 1), ==, 0);
    g_assert_cmpuint (page, ==, (gsize) sysconf (_SC_PAGESIZE));
    g_assert_cmpint (G_BYTE_ORDER, ==, G_LITTLE_ENDIAN);
    /* the sizes the code is written for (LP64) */
    g_assert_cmpuint (sizeof (long), ==, 8);
    g_assert_cmpuint (sizeof (gsize), ==, 8);
    g_assert_cmpuint (sizeof (off_t), ==, 8);
    g_assert_cmpuint (sizeof (time_t), ==, 8);
    g_assert_cmpuint (sizeof (gpointer), ==, 8);
    const char *arch =
#if defined (__x86_64__)
        "x86-64";
#elif defined (__aarch64__)
        "aarch64";
#else
        "another architecture";
#endif
    g_test_message ("%s, plain char is %s, page size %" G_GSIZE_FORMAT " bytes, %u processor(s)", arch,
                    CHAR_MIN < 0 ? "signed" : "unsigned", page, g_get_num_processors ());
}

int main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/portable/readers", t_readers);
    g_test_add_func ("/portable/hashes", t_hashes);
    g_test_add_func ("/portable/text", t_text);
    g_test_add_func ("/portable/float-int", t_float_int);
    g_test_add_func ("/portable/stack", t_stack);
    g_test_add_func ("/portable/platform", t_platform);
    return g_test_run ();
}
