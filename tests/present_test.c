/* present_test.c - presentation rules (no display, no GL): the present
 * setting per monitor, the swap interval it gives with and without a
 * compositing manager, gpu.conf and the driver hints it turns into. */
#include <glib.h>
#include <string.h>
#include "common/present.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_modes (void)
{
    WalpePresentMode m = WALPE_PRESENT_AUTO;
    CHECK (walpe_present_mode_parse ("vsync", &m) && m == WALPE_PRESENT_VSYNC);
    CHECK (walpe_present_mode_parse ("Limiter", &m) && m == WALPE_PRESENT_LIMITER);
    CHECK (walpe_present_mode_parse ("AUTO", &m) && m == WALPE_PRESENT_AUTO);
    CHECK (!walpe_present_mode_parse ("fast", &m));
    CHECK (!walpe_present_mode_parse ("", &m));
    CHECK (!walpe_present_mode_parse (NULL, &m));
    for (int i = 0; i <= WALPE_PRESENT_AUTO; i++) {
        WalpePresentMode back = (WalpePresentMode) -1;
        CHECK (walpe_present_mode_parse (walpe_present_mode_name (i), &back) && (int) back == i);
    }

    char *bad = (char *) "x";
    /* nothing set = what every round up to 12 did */
    CHECK (walpe_present_spec_mode (NULL, "DP-2", 0, &bad) == WALPE_PRESENT_VSYNC && !bad);
    CHECK (walpe_present_spec_mode ("", "DP-2", 0, &bad) == WALPE_PRESENT_VSYNC && !bad);
    CHECK (walpe_present_spec_mode ("limiter", "DP-2", 0, &bad) == WALPE_PRESENT_LIMITER && !bad);
    CHECK (walpe_present_spec_mode (" auto ", "DP-2", 1, NULL) == WALPE_PRESENT_AUTO);
    /* per monitor: by output name (any case) or monN; the others keep the default */
    const char *spec = "auto, HDMI-0 = limiter ,mon2=vsync";
    CHECK (walpe_present_spec_mode (spec, "DP-2", 0, &bad) == WALPE_PRESENT_AUTO && !bad);
    CHECK (walpe_present_spec_mode (spec, "hdmi-0", 1, &bad) == WALPE_PRESENT_LIMITER && !bad);
    CHECK (walpe_present_spec_mode (spec, "eDP-1", 2, &bad) == WALPE_PRESENT_VSYNC && !bad);
    CHECK (walpe_present_spec_mode ("HDMI-0=limiter", "DP-2", 0, NULL) == WALPE_PRESENT_VSYNC);
    CHECK (walpe_present_spec_mode ("HDMI-0=limiter", "HDMI-0", 1, NULL) == WALPE_PRESENT_LIMITER);
    /* a later default / override wins; order of the two kinds does not matter */
    CHECK (walpe_present_spec_mode ("vsync,limiter", "A", 0, NULL) == WALPE_PRESENT_LIMITER);
    CHECK (walpe_present_spec_mode ("A=auto,limiter", "A", 0, NULL) == WALPE_PRESENT_AUTO);
    /* what is not understood is skipped and reported, the rest still counts */
    CHECK (walpe_present_spec_mode ("turbo,limiter", "A", 0, &bad) == WALPE_PRESENT_LIMITER);
    CHECK (!g_strcmp0 (bad, "turbo"));
    g_free (bad);
    CHECK (walpe_present_spec_mode ("auto,A=never", "A", 0, &bad) == WALPE_PRESENT_AUTO);
    CHECK (!g_strcmp0 (bad, "A=never"));
    g_free (bad);
    CHECK (walpe_present_spec_mode ("=limiter", "A", 0, &bad) == WALPE_PRESENT_VSYNC);
    CHECK (bad != NULL);
    g_free (bad);
    /* a monitor without a name is still found by its index */
    CHECK (walpe_present_spec_mode ("mon1=limiter", NULL, 1, NULL) == WALPE_PRESENT_LIMITER);
    CHECK (walpe_present_spec_mode ("mon1=limiter", NULL, 11, NULL) == WALPE_PRESENT_VSYNC);
}

static void test_interval (void)
{
    WalpePresentEnv plain = { .min_interval = 0, .max_interval = 1 };
    WalpePresentEnv comp = plain;
    comp.composited = TRUE;
    const char *why = NULL;

    /* the explicit modes do not depend on anything */
    CHECK (walpe_present_interval (WALPE_PRESENT_VSYNC, "A", &plain, &why) == 1 && why);
    CHECK (walpe_present_interval (WALPE_PRESENT_VSYNC, "A", &comp, NULL) == 1);
    CHECK (walpe_present_interval (WALPE_PRESENT_LIMITER, "A", &plain, &why) == 0 && why);
    CHECK (walpe_present_interval (WALPE_PRESENT_LIMITER, "A", &comp, NULL) == 0);
    /* auto: the swap waits only where nothing else presents the window */
    CHECK (walpe_present_interval (WALPE_PRESENT_AUTO, "A", &plain, NULL) == 1);
    CHECK (walpe_present_interval (WALPE_PRESENT_AUTO, "A", &comp, &why) == 0);
    CHECK (why && strstr (why, "compositing"));

    /* auto, no compositor, the driver told which display it synchronises
     * to: that monitor waits, the others would wait for the wrong display */
    WalpePresentEnv sync = plain;
    sync.sync_device = "DP-2";
    sync.sync_is_ours = TRUE;
    CHECK (walpe_present_interval (WALPE_PRESENT_AUTO, "DP-2", &sync, NULL) == 1);
    CHECK (walpe_present_interval (WALPE_PRESENT_AUTO, "dp-2", &sync, NULL) == 1);
    CHECK (walpe_present_interval (WALPE_PRESENT_AUTO, "HDMI-0", &sync, &why) == 0);
    CHECK (why && strstr (why, "another display"));
    CHECK (walpe_present_interval (WALPE_PRESENT_VSYNC, "HDMI-0", &sync, NULL) == 1);
    /* a device name that is none of our monitors (DFP-1, a typo) tells
     * nothing: every monitor keeps waiting */
    sync.sync_is_ours = FALSE;
    CHECK (walpe_present_interval (WALPE_PRESENT_AUTO, "HDMI-0", &sync, NULL) == 1);
    CHECK (walpe_present_interval (WALPE_PRESENT_AUTO, NULL, &sync, NULL) == 1);

    /* a config that cannot do the interval gets the nearest */
    WalpePresentEnv only1 = { .min_interval = 1, .max_interval = 1 };
    CHECK (walpe_present_interval (WALPE_PRESENT_LIMITER, "A", &only1, &why) == 1);
    CHECK (why && strstr (why, "no swap interval 0"));
    WalpePresentEnv only0 = { .min_interval = 0, .max_interval = 0 };
    CHECK (walpe_present_interval (WALPE_PRESENT_VSYNC, "A", &only0, &why) == 0);
    CHECK (why && strstr (why, "no swap interval 1"));
    WalpePresentEnv wide = { .min_interval = 0, .max_interval = 10 };
    CHECK (walpe_present_interval (WALPE_PRESENT_VSYNC, "A", &wide, NULL) == 1);
    WalpePresentEnv odd = { .min_interval = 1, .max_interval = 0 };   /* nonsense: min wins */
    CHECK (walpe_present_interval (WALPE_PRESENT_LIMITER, "A", &odd, NULL) == 1);
}

static void test_gpuconf (void)
{
    GHashTable *h = walpe_gpuconf_parse (
        "# tuning\n"
        "present = auto,HDMI-0=limiter   # per monitor\n"
        "  Yield=usleep\n"
        "\n"
        "idle_release=300\n"
        "no equals sign here\n"
        "=novalue\n"
        "yield = nothing\n"
        "shader_cache = /var/tmp/walpe glcache\n");
    CHECK (g_hash_table_size (h) == 4);
    CHECK (!g_strcmp0 (g_hash_table_lookup (h, "present"), "auto,HDMI-0=limiter"));
    CHECK (!g_strcmp0 (g_hash_table_lookup (h, "yield"), "nothing"));   /* the last one wins */
    CHECK (!g_strcmp0 (g_hash_table_lookup (h, "idle_release"), "300"));
    CHECK (!g_strcmp0 (g_hash_table_lookup (h, "shader_cache"), "/var/tmp/walpe glcache"));
    g_hash_table_unref (h);
    h = walpe_gpuconf_parse (NULL);
    CHECK (g_hash_table_size (h) == 0);
    g_hash_table_unref (h);
    h = walpe_gpuconf_parse ("present=limiter");   /* no newline at the end */
    CHECK (!g_strcmp0 (g_hash_table_lookup (h, "present"), "limiter"));
    g_hash_table_unref (h);
}

/* the "NAME=VALUE" strings a hint gives, joined with ' '; "!" = refused */
static char *hint (const char *key, const char *value)
{
    GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
    char *r;
    if (!walpe_driver_hint (key, value, a)) {
        r = g_strdup ("!");
    } else {
        g_ptr_array_add (a, NULL);
        r = g_strjoinv (" ", (char **) a->pdata);
    }
    g_ptr_array_unref (a);
    return r;
}

#define HINT(k, v, want) do { char *h_ = hint (k, v); \
    if (g_strcmp0 (h_, want)) { fails++; \
        g_printerr ("FAIL %s:%d: %s=%s gave \"%s\", expected \"%s\"\n", __FILE__, __LINE__, k, v ? v : "(null)", h_, want); } \
    g_free (h_); } while (0)

static void test_hints (void)
{
    HINT ("yield", "usleep", "__GL_YIELD=USLEEP");
    HINT ("yield", "USLEEP", "__GL_YIELD=USLEEP");
    HINT ("yield", "nothing", "__GL_YIELD=NOTHING");
    HINT ("yield", "default", "");
    HINT ("yield", "", "");
    HINT ("yield", "sleep", "!");
    HINT ("threaded", "on", "__GL_THREADED_OPTIMIZATIONS=1");
    HINT ("threaded", "0", "__GL_THREADED_OPTIMIZATIONS=0");
    HINT ("threaded", "maybe", "!");
    HINT ("max_frames", "1", "__GL_MaxFramesAllowed=1");
    HINT ("max_frames", "2", "__GL_MaxFramesAllowed=2");
    HINT ("max_frames", "0", "!");
    HINT ("max_frames", "9", "!");
    HINT ("max_frames", "2x", "!");
    HINT ("sync_display", "DP-2", "__GL_SYNC_DISPLAY_DEVICE=DP-2");
    HINT ("sync_display", "DFP-1", "__GL_SYNC_DISPLAY_DEVICE=DFP-1");
    HINT ("sync_display", "DP-2 $(x)", "!");
    HINT ("sync_display", "default", "");
    HINT ("shader_cache", "on", "__GL_SHADER_DISK_CACHE=1");
    HINT ("shader_cache", "off", "__GL_SHADER_DISK_CACHE=0");
    HINT ("shader_cache", "/var/cache/walpe gl",
          "__GL_SHADER_DISK_CACHE=1 __GL_SHADER_DISK_CACHE_PATH=/var/cache/walpe gl");
    HINT ("shader_cache", "relative/dir", "!");
    HINT ("shader_cache_size", "1073741824", "__GL_SHADER_DISK_CACHE_SIZE=1073741824");
    HINT ("shader_cache_size", "-1", "!");
    /* not hints: the daemon's own settings, unknown keys, no value */
    HINT ("present", "auto", "!");
    HINT ("idle_release", "60", "!");
    HINT ("__GL_YIELD", "USLEEP", "!");
    HINT ("yield", NULL, "!");

    /* every documented key is either a driver hint or a daemon setting */
    static const char *const own[] = { "present", "idle_release", "egl_display", "swap_damage",
                                       "frame_align", "wayland_reconnect", "wayland_follow" };
    int nhint = 0, nown = 0;
    for (const char *const *k = walpe_gpuconf_keys (); *k; k++) {
        gboolean is_own = FALSE;
        for (guint i = 0; i < G_N_ELEMENTS (own); i++)
            is_own |= !strcmp (*k, own[i]);
        GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
        gboolean is_hint = walpe_driver_hint (*k, "default", a);
        g_ptr_array_unref (a);
        CHECK (is_own != is_hint);
        nown += is_own;
        nhint += is_hint;
    }
    CHECK (nown == 7 && nhint == 6);
}

static void test_shader_cache (void)
{
    /* (root, forced on, cache path set, XDG_CACHE_HOME set, HOME set) */
    CHECK (!walpe_shader_cache_blocker (FALSE, FALSE, FALSE, FALSE, TRUE));
    CHECK (!walpe_shader_cache_blocker (FALSE, FALSE, FALSE, TRUE, FALSE));
    CHECK (!walpe_shader_cache_blocker (FALSE, FALSE, TRUE, FALSE, FALSE));
    CHECK (walpe_shader_cache_blocker (FALSE, FALSE, FALSE, FALSE, FALSE) != NULL);
    CHECK (walpe_shader_cache_blocker (TRUE, FALSE, FALSE, FALSE, TRUE) != NULL);
    CHECK (!walpe_shader_cache_blocker (TRUE, TRUE, FALSE, FALSE, TRUE));
    CHECK (walpe_shader_cache_blocker (TRUE, TRUE, FALSE, FALSE, FALSE) != NULL);
}

int main (void)
{
    test_modes ();
    test_interval ();
    test_gpuconf ();
    test_hints ();
    test_shader_cache ();
    if (fails) {
        g_printerr ("present-test: %d check(s) failed\n", fails);
        return 1;
    }
    g_print ("present-test: all checks passed\n");
    return 0;
}
