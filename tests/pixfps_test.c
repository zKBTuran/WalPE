/* pixfps_test.c - version 0.12 pacing and config rules (no display needed):
 * the one-time fps migration of config.json, the shared-rendering leader,
 * the paced frame rate, mode refresh rates and the YUV matrix choice. */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "common/config.h"
#include "common/pace.h"
#include "engine/yuvconv.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char *cfg_path;

static void put_config (const char *json)
{
    g_assert (g_file_set_contents (cfg_path, json, -1, NULL));
}

static char *get_config (void)
{
    char *txt = NULL;
    g_assert (g_file_get_contents (cfg_path, &txt, NULL, NULL));
    return txt;
}

/* what the daemon does at start-up: load, save when migrated */
static int daemon_start (gboolean *migrated)
{
    WalpeConfig *c = walpe_config_load ();
    int fps = c->fps;
    if (migrated)
        *migrated = c->fps_migrated;
    if (c->fps_migrated)
        walpe_config_save (c);
    walpe_config_free (c);
    return fps;
}

/* walpe-ctl set fps=N as the daemon handles it: set, save */
static void daemon_set_fps (int fps)
{
    WalpeConfig *c = walpe_config_load ();
    c->fps = fps;
    walpe_config_save (c);
    walpe_config_free (c);
}

static void test_migration (void)
{
    gboolean mig = FALSE;
    /* the rule itself */
    CHECK (WALPE_FPS_DEFAULT == 60);
    CHECK (walpe_config_migrate_fps (1, 30) == WALPE_FPS_DEFAULT);
    CHECK (walpe_config_migrate_fps (1, 60) == 60);
    CHECK (walpe_config_migrate_fps (1, 31) == 31);
    CHECK (walpe_config_migrate_fps (1, 200) == WALPE_FPS_DEFAULT);   /* default of 9.1 - 11 */
    CHECK (walpe_config_migrate_fps (2, 30) == 30);
    CHECK (walpe_config_migrate_fps (2, 200) == WALPE_FPS_DEFAULT);   /* version 0.12: default / its migration */
    CHECK (walpe_config_migrate_fps (2, 144) == 144);
    CHECK (walpe_config_migrate_fps (2, 199) == 199);
    CHECK (walpe_config_migrate_fps (3, 30) == 30);
    CHECK (walpe_config_migrate_fps (3, 200) == 200);                 /* chosen under version 3 */

    /* no config.json at all: the default, nothing to migrate */
    g_remove (cfg_path);
    CHECK (daemon_start (&mig) == WALPE_FPS_DEFAULT && !mig);

    /* a config written by a version up to 0.9.0 (default 30, every key written,
     * no version): raised once and saved with the version */
    put_config ("{\"wallpapers_dir\":\"/x\",\"current\":\"/x/1\",\"fps\":30,\"volume\":0.0,\"parallax\":true}");
    CHECK (daemon_start (&mig) == WALPE_FPS_DEFAULT && mig);
    char *txt = get_config ();
    CHECK (strstr (txt, "\"config_version\" : 4") != NULL);
    CHECK (strstr (txt, "\"fps\" : 60") != NULL);
    CHECK (strstr (txt, "\"current\" : \"/x/1\"") != NULL);     /* the rest is kept */
    g_free (txt);
    CHECK (daemon_start (&mig) == WALPE_FPS_DEFAULT && !mig);   /* second start: nothing */

    /* the user then sets 30 on purpose: it stays, across restarts */
    daemon_set_fps (30);
    CHECK (daemon_start (&mig) == 30 && !mig);
    CHECK (daemon_start (&mig) == 30 && !mig);
    txt = get_config ();
    CHECK (strstr (txt, "\"fps\" : 30") != NULL && strstr (txt, "\"config_version\" : 4") != NULL);
    g_free (txt);

    /* a config written by version 0.12 (default 200, or 200 put there by its
     * migration of the old 30): 60 once, saved as version 3 */
    put_config ("{\"config_version\":2,\"current\":\"/x/2\",\"fps\":200,\"parallax\":true}");
    CHECK (daemon_start (&mig) == 60 && mig);
    txt = get_config ();
    CHECK (strstr (txt, "\"config_version\" : 4") != NULL && strstr (txt, "\"fps\" : 60") != NULL);
    CHECK (strstr (txt, "\"current\" : \"/x/2\"") != NULL);
    g_free (txt);
    CHECK (daemon_start (&mig) == 60 && !mig);                  /* second start: nothing */
    /* the user then asks for 200 (the monitor's rate): it stays */
    daemon_set_fps (200);
    CHECK (daemon_start (&mig) == 200 && !mig);
    CHECK (daemon_start (&mig) == 200 && !mig);
    /* versions 0.9.1 - 0.11: unversioned file holding that default */
    put_config ("{\"fps\":200}");
    CHECK (daemon_start (&mig) == 60 && mig);
    /* version 0.12 file with a value its default never produced: the user's */
    put_config ("{\"config_version\":2,\"fps\":144}");
    CHECK (daemon_start (&mig) == 144 && !mig);
    put_config ("{\"config_version\":2,\"fps\":30}");
    CHECK (daemon_start (&mig) == 30 && !mig);

    /* an unversioned config with a value the old default never produced:
     * the user chose it, keep it (and do not rewrite the file for it) */
    put_config ("{\"fps\":60}");
    CHECK (daemon_start (&mig) == 60 && !mig);
    put_config ("{\"fps\":144}");
    CHECK (daemon_start (&mig) == 144 && !mig);
    put_config ("{\"fps\":25}");
    CHECK (daemon_start (&mig) == 25 && !mig);
    /* unversioned, no fps key (hand-written file): the default */
    put_config ("{\"parallax\":false}");
    CHECK (daemon_start (&mig) == WALPE_FPS_DEFAULT && !mig);
    /* versioned 30 written by hand */
    put_config ("{\"config_version\":3,\"fps\":30}");
    CHECK (daemon_start (&mig) == 30 && !mig);
    /* clamps, also for absurd numbers (no float-cast overflow) */
    put_config ("{\"config_version\":3,\"fps\":1e300}");
    CHECK (daemon_start (&mig) == WALPE_FPS_MAX);
    put_config ("{\"config_version\":3,\"fps\":-5}");
    CHECK (daemon_start (&mig) == 5);
}

/* two-monitor pacing rules and the settings they come with */
static void test_o5_rules (void)
{
    /* a 144 Hz + 60 Hz pair, limit 60: the 144 Hz monitor leads at 60, the
     * 60 Hz one presents every frame as it arrives */
    CHECK (walpe_follow_free (60, 144.0, 60.0));
    CHECK (walpe_follow_free (60, 144.0, 59.95));     /* 59.95 Hz counts as 60 */
    CHECK (walpe_follow_free (30, 144.0, 60.0));
    CHECK (walpe_follow_free (60, 60.0, 144.0));      /* WALPE_LEADER=first */
    /* the leader can publish more than the follower shows: the slot rule stays */
    CHECK (!walpe_follow_free (144, 144.0, 60.0));
    CHECK (!walpe_follow_free (200, 200.0, 144.0));
    CHECK (!walpe_follow_free (75, 144.0, 60.0));
    CHECK (!walpe_follow_free (120, 0.0, 60.0));      /* leader rate unknown: the setting alone */
    CHECK (!walpe_follow_free (60, 144.0, 0.0));      /* follower rate unknown */
    {
        /* a follower with an absurd rate shows every frame; no rate at all = unknown */
        volatile double absurd = 4294967295.0, nan = NAN;
        CHECK (walpe_follow_free (60, 144.0, absurd));
        CHECK (walpe_follow_free (200, absurd, absurd));
        CHECK (!walpe_follow_free (60, 144.0, nan));
        CHECK (walpe_follow_free (60, nan, 60.0));    /* leader rate unknown: paced by the setting */
        CHECK (!walpe_follow_free (120, nan, 60.0));
    }

    /* a follower's own limit counts only below its leader's */
    CHECK (walpe_follow_cap (15, 60) == 15);
    CHECK (walpe_follow_cap (60, 60) == 0);
    CHECK (walpe_follow_cap (60, 15) == 0);
    CHECK (walpe_follow_cap (0, 60) == 0 && walpe_follow_cap (15, 0) == 0);

    /* pointer position: the monitor's own, clamped (version 0.12.1 arithmetic) */
    float x = 9, y = 9;
    walpe_pointer_norm (480, 270, 0, 0, 1920, 1080, &x, &y);
    CHECK (x == -0.5f && y == -0.5f);
    walpe_pointer_norm (2400, 270, 0, 0, 1920, 1080, &x, &y);     /* on the monitor to the right */
    CHECK (x == 1.0f && y == -0.5f);
    /* pointer_share: the same pointer against the monitor it is on gives
     * what that monitor's own rendering gets */
    walpe_pointer_norm (2400, 270, 1920, 0, 1920, 1080, &x, &y);
    CHECK (x == -0.5f && y == -0.5f);
    walpe_pointer_norm (5, 5, 0, 0, 0, 0, &x, &y);                /* no size: no division by zero */
    CHECK (x == 1.0f && y == 1.0f);

    /* pointer poll: the frame rate, 30..144; lower only for a wallpaper
     * that is itself drawn more slowly */
    CHECK (walpe_pointer_poll_ms (60, 0) == 16);
    CHECK (walpe_pointer_poll_ms (60, 60) == 16);
    CHECK (walpe_pointer_poll_ms (144, 144) == 6);
    CHECK (walpe_pointer_poll_ms (20, 0) == 33);      /* as before: never below 30 on its own */
    CHECK (walpe_pointer_poll_ms (60, 15) == 66);     /* focus / battery / fps_idle limit 15 */
    CHECK (walpe_pointer_poll_ms (60, 4) == 16);      /* nonsense limit: ignored */
    CHECK (walpe_pointer_poll_ms (60, 200) == 16);

    /* the new settings are off in a new config, are not written while off
     * (an untouched config.json keeps its bytes) and survive a round trip */
    g_unlink (cfg_path);
    WalpeConfig *c = walpe_config_load ();
    CHECK (!c->pointer_share);
    CHECK (c->skip_hidden == WALPE_SKIP_OFF);
    CHECK (c->skip_hidden_margin == WALPE_SKIP_MARGIN_DEFAULT && c->skip_hidden_inset == 0);
    walpe_config_save (c);
    char *txt = get_config ();
    CHECK (!strstr (txt, "pointer_share") && !strstr (txt, "skip_hidden"));
    g_free (txt);
    c->pointer_share = TRUE;
    c->skip_hidden = WALPE_SKIP_ALWAYS;
    c->skip_hidden_margin = 48;
    c->skip_hidden_inset = 6;
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (c->pointer_share);
    CHECK (c->skip_hidden == WALPE_SKIP_ALWAYS);
    CHECK (c->skip_hidden_margin == 48 && c->skip_hidden_inset == 6);
    walpe_config_free (c);
    put_config ("{\"config_version\":3,\"skip_hidden\":\"auto\",\"skip_hidden_margin\":9999}");
    c = walpe_config_load ();
    CHECK (c->skip_hidden == WALPE_SKIP_AUTO && c->skip_hidden_margin == 512);
    walpe_config_free (c);
    g_unlink (cfg_path);
    CHECK (walpe_skip_hidden_parse ("always", 0) == WALPE_SKIP_ALWAYS);
    CHECK (walpe_skip_hidden_parse ("nonsense", WALPE_SKIP_OFF) == WALPE_SKIP_OFF);
    CHECK (!strcmp (walpe_skip_hidden_name (WALPE_SKIP_OFF), "off"));
}

static void test_leader (void)
{
    double a[] = { 60.0, 144.0 };
    CHECK (walpe_pick_leader (a, 2) == 1);            /* the user's case: 144 Hz second */
    double b[] = { 144.0, 60.0 };
    CHECK (walpe_pick_leader (b, 2) == 0);
    double c[] = { 200.0, 143.98 };
    CHECK (walpe_pick_leader (c, 2) == 0);            /* 200 Hz + 144 Hz: the faster one leads */
    double d[] = { 143.98, 200.0 };
    CHECK (walpe_pick_leader (d, 2) == 1);
    double e[] = { 60.0, 60.0 };
    CHECK (walpe_pick_leader (e, 2) == 0);            /* equal: the first (as before) */
    double f[] = { 59.94, 60.0 };
    CHECK (walpe_pick_leader (f, 2) == 0);            /* same class: no flip on 0.06 Hz */
    double g[] = { 0.0, 0.0 };
    CHECK (walpe_pick_leader (g, 2) == 0);            /* unknown: the first (as before) */
    double h[] = { 0.0, 144.0, 60.0 };
    CHECK (walpe_pick_leader (h, 3) == 1);
    double i[] = { 75.0 };
    CHECK (walpe_pick_leader (i, 1) == 0);
    double j[] = { 60.0, 144.0, 165.0, 144.0, 240.0 };
    CHECK (walpe_pick_leader (j, 5) == 4);
}

static void test_paced (void)
{
    CHECK (walpe_paced_fps (200, 144.0) == 144);
    CHECK (walpe_paced_fps (200, 143.981) == 144);    /* rounded up: never slower than the display */
    CHECK (walpe_paced_fps (200, 59.94) == 60);
    CHECK (walpe_paced_fps (200, 60.0) == 60);
    CHECK (walpe_paced_fps (200, 239.76) == 200);     /* the cap wins */
    CHECK (walpe_paced_fps (30, 144.0) == 30);        /* a user's 30 is honoured */
    CHECK (walpe_paced_fps (60, 0.0) == 60);          /* unknown refresh: the cap */
    CHECK (walpe_paced_fps (200, 5.0) == 200);        /* implausible: ignored */
    CHECK (walpe_paced_fps (0, 60.0) == 1);
    /* mode timings -> Hz */
    CHECK (fabs (walpe_mode_refresh (148500000, 2200, 1125, FALSE, FALSE) - 60.0) < 0.001);
    CHECK (fabs (walpe_mode_refresh (346500000, 2080, 1157, FALSE, FALSE) - 143.981) < 0.01);
    CHECK (fabs (walpe_mode_refresh (74250000, 2200, 1125, TRUE, FALSE) - 60.0) < 0.001);  /* 1080i: fields */
    CHECK (walpe_mode_refresh (0, 0, 0, FALSE, FALSE) == 0.0);
    /* a mode whose timings give an absurd rate (a dot clock of 4 GHz over
     * totals of 1 x 1: more than an int holds), and values that are no
     * rate at all: the limit alone, never a negative frame rate. volatile:
     * the rate comes from the X server at run time, not from a constant */
    volatile double absurd = walpe_mode_refresh (4294967295UL, 1, 1, FALSE, FALSE);
    volatile double huge = 1e300, inf = INFINITY, nan = NAN;
    CHECK (absurd > 2147483648.0);
    CHECK (walpe_paced_fps (60, absurd) == 60);
    CHECK (walpe_paced_fps (200, huge) == 200);
    CHECK (walpe_paced_fps (60, inf) == 60);
    CHECK (walpe_paced_fps (60, nan) == 60);
    CHECK (walpe_paced_fps (60, -inf) == 60);
    CHECK (walpe_paced_fps (200, 2147483647.5) == 200);
    /* WALPE_MONITORS "@HZ" */
    CHECK (walpe_monitor_spec_hz ("1920x1080+0+0@144") == 144.0);
    CHECK (fabs (walpe_monitor_spec_hz ("1920x1080+1920+0@59.94") - 59.94) < 1e-9);
    CHECK (walpe_monitor_spec_hz ("1920x1080+0+0") == 0.0);
}

static void test_yuv (void)
{
    /* untagged: BT.709 from 720p, BT.601 below; tags win */
    CHECK (walpe_yuv_matrix_for (2, 3840, 2160) == WALPE_YUV_BT709);
    CHECK (walpe_yuv_matrix_for (2, 1280, 720) == WALPE_YUV_BT709);
    CHECK (walpe_yuv_matrix_for (2, 720, 576) == 0);
    CHECK (walpe_yuv_matrix_for (2, 854, 480) == 0);
    CHECK (walpe_yuv_matrix_for (1, 640, 360) == WALPE_YUV_BT709);
    CHECK (walpe_yuv_matrix_for (5, 3840, 2160) == 0);
    CHECK (walpe_yuv_matrix_for (6, 1920, 1080) == 0);
    CHECK (walpe_yuv_matrix_for (9, 3840, 2160) == WALPE_YUV_BT2020);
    /* grey stays grey and white/black map to 255/0 in every matrix */
    guint modes[] = { 0, WALPE_YUV_BT709, WALPE_YUV_BT2020, WALPE_YUV_FULL,
                      WALPE_YUV_FULL | WALPE_YUV_BT709, WALPE_YUV_FULL | WALPE_YUV_BT2020 };
    for (guint m = 0; m < G_N_ELEMENTS (modes); m++) {
        gboolean full = modes[m] & WALPE_YUV_FULL;
        guint8 y[4], u[1] = { 128 }, v[1] = { 128 }, out[16];
        memset (y, full ? 255 : 235, 4);
        walpe_yuv420_bgra_ex (y, u, v, 2, 1, out, 8, 2, 2, modes[m]);
        CHECK (out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 255);
        memset (y, full ? 0 : 16, 4);
        walpe_yuv420_bgra_ex (y, u, v, 2, 1, out, 8, 2, 2, modes[m]);
        CHECK (out[0] == 0 && out[1] == 0 && out[2] == 0);
        memset (y, 126, 4);
        walpe_yuv420_bgra_ex (y, u, v, 2, 1, out, 8, 2, 2, modes[m]);
        CHECK (out[0] == out[1] && out[1] == out[2]);
    }
    /* pure red, limited range: BT.601 codes it (81,90,240), BT.709 (63,102,240) */
    guint8 y6[4] = { 81, 81, 81, 81 }, u6[1] = { 90 }, v6[1] = { 240 }, o[16];
    walpe_yuv420_bgra_ex (y6, u6, v6, 2, 1, o, 8, 2, 2, 0);
    CHECK (o[2] >= 253 && o[1] <= 2 && o[0] <= 2);
    guint8 y7[4] = { 63, 63, 63, 63 }, u7[1] = { 102 }, v7[1] = { 240 };
    walpe_yuv420_bgra_ex (y7, u7, v7, 2, 1, o, 8, 2, 2, WALPE_YUV_BT709);
    CHECK (o[2] >= 253 && o[1] <= 2 && o[0] <= 2);
    /* the same BT.709 red read as BT.601 is visibly off (what untagged HD video got) */
    walpe_yuv420_bgra_ex (y7, u7, v7, 2, 1, o, 8, 2, 2, 0);
    CHECK (o[1] > 15 || o[2] < 240);
}

/* frame_align: frame clocks on one grid (pace.h) */
static void test_frame_grid (void)
{
    const gint64 iv60 = 1000000 / 60, iv30 = 1000000 / 30, iv144 = 1000000 / 144;
    /* two monitors whose scenes were loaded at unrelated times */
    gint64 a = 0, b = 0, now_a = 5000123, now_b = 5007777;
    a = walpe_frame_grid_next (now_a, a, iv60);
    b = walpe_frame_grid_next (now_b, b, iv60);
    CHECK (a > now_a && a - now_a <= iv60 + iv60 / 2 + 1);     /* the first deadline: within 1.5 frames */
    CHECK (b > now_b && b - now_b <= iv60 + iv60 / 2 + 1);
    CHECK ((a * 60 + 500000) / 1000000 * 1000000 / 60 == a);        /* on the grid k * 1e6 / 60 */
    CHECK ((b * 60 + 500000) / 1000000 * 1000000 / 60 == b);
    /* both draw on time for 10 seconds: the same deadlines, 60 a second */
    gint64 first = a;
    while (b < a)
        b = walpe_frame_grid_next (b + 900, b, iv60);
    while (a < b)
        a = walpe_frame_grid_next (a + 900, a, iv60);
    int same = 0;
    for (int i = 0; i < 600; i++) {
        gint64 na = walpe_frame_grid_next (a + 2100, a, iv60);   /* rendered 2.1 ms */
        gint64 nb = walpe_frame_grid_next (b + 1000, b, iv60);   /* rendered 1.0 ms */
        CHECK (na > a && nb > b);
        same += na == nb;
        a = na;
        b = nb;
    }
    CHECK (same == 600);
    CHECK (a - first >= 9900000 && a - first <= 10100000 + 2 * iv60);   /* 600 frames = 10 s: no drift */
    /* the free-running clock for comparison keeps the two phases apart */
    gint64 fa = now_a + iv60, fb = now_b + iv60;
    for (int i = 0; i < 600; i++) {
        fa += iv60;
        fb += iv60;
    }
    CHECK (fb - fa == now_b - now_a);
    /* a clock that is behind draws now (no wait for the grid, no burst) ... */
    gint64 late = a + 5 * iv60 + 4321;
    gint64 n = walpe_frame_grid_next (late, a, iv60);
    CHECK (n == late);
    /* ... and is back on the grid with the next frame, less than 1.5 frames later */
    gint64 n2 = walpe_frame_grid_next (n + 3000, n, iv60);
    CHECK (n2 > n + 3000 && n2 - n <= iv60 + iv60 / 2 + 1);
    CHECK ((n2 * 60 + 500000) / 1000000 * 1000000 / 60 == n2);
    /* never in the past, also when the nearest grid point has just gone by */
    for (gint64 t = 7000000; t < 7000000 + 3 * iv60; t += 137) {
        gint64 d = walpe_frame_grid_next (t, t - iv60 + 100, iv60);
        CHECK (d >= t && d - t <= iv60 + 1);
    }
    /* a 30 fps clock (a 30 fps clip under a 60 fps cap) shares every second point of a 60 fps one */
    gint64 c30 = walpe_frame_grid_next (9000000, 0, iv30), c60 = walpe_frame_grid_next (9000000, 0, iv60);
    int shared = 0;
    for (int i = 0; i < 60; i++) {
        gint64 t30 = c30;
        while (c60 < t30)
            c60 = walpe_frame_grid_next (c60 + 500, c60, iv60);
        shared += c60 == t30;
        c30 = walpe_frame_grid_next (c30 + 500, c30, iv30);
    }
    CHECK (shared == 60);
    /* 144 fps: on its own grid, 144 frames a second */
    gint64 c = walpe_frame_grid_next (1000000, 0, iv144), c0 = c;
    for (int i = 0; i < 1440; i++)
        c = walpe_frame_grid_next (c + 300, c, iv144);
    CHECK (c - c0 >= 9990000 && c - c0 <= 10010000);
    /* nonsense input */
    CHECK (walpe_frame_grid_next (123456, 0, 0) == 123456);
    CHECK (walpe_frame_grid_next (123456, 100, -5) == 123456);
}

int main (void)
{
    /* before any GLib call that caches the config dir */
    char *tmp = g_dir_make_tmp ("walpe-pixfps-XXXXXX", NULL);
    g_assert (tmp);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    char *dir = g_build_filename (tmp, "walpe", NULL);
    g_mkdir_with_parents (dir, 0700);
    cfg_path = g_build_filename (dir, "config.json", NULL);

    test_migration ();
    test_leader ();
    test_paced ();
    test_frame_grid ();
    test_yuv ();
    test_o5_rules ();

    g_remove (cfg_path);
    g_rmdir (dir);
    g_rmdir (tmp);
    g_free (dir);
    g_free (tmp);
    if (fails) {
        g_printerr ("%d check(s) failed\n", fails);
        return 1;
    }
    g_print ("pixfps: config fps migration, leader choice, pacing, YUV matrices OK\n");
    return 0;
}
