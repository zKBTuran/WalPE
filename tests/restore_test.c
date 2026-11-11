/* restore_test.c - a wallpaper failed to load: which monitor returns to
 * what, and what the saved choice becomes (common/restore.h). No display:
 * the rules only; the daemon side (loads, the black frame, status) needs
 * an X server and is not part of this test. */
#include <glib.h>
#include <string.h>
#include "common/config.h"
#include "common/restore.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define A "/w/a"
#define B "/w/b"
#define C "/w/c"
#define X "/w/broken"
#define Y "/w/broken2"

static void cfg_clear (WalpeConfig *c)
{
    g_clear_pointer (&c->current, g_free);
    g_clear_pointer (&c->monitor_current, g_hash_table_unref);
}

static guint own_choices (WalpeConfig *c)
{
    return c->monitor_current ? g_hash_table_size (c->monitor_current) : 0;
}

/* what `open PATH` (every monitor) does to the config, as the daemon's open_commit */
static void open_all (WalpeConfig *c, WalpeOpenUndo *u, const char *path)
{
    walpe_open_undo_take (u, c, path);
    g_free (c->current);
    c->current = g_strdup (path);
    if (c->monitor_current)
        g_hash_table_remove_all (c->monitor_current);
}

/* `open PATH --monitor N` */
static void open_one (WalpeConfig *c, const char *name, const char *path)
{
    walpe_config_set_monitor_path (c, name, g_strcmp0 (path, c->current) ? path : NULL);
}

/* two monitors on the default, a render_shared group: both return, the
 * config is what it was */
static void test_group (void)
{
    WalpeConfig c = { 0 };
    WalpeOpenUndo u = { 0 };
    c.current = g_strdup (A);
    open_all (&c, &u, X);
    WalpeRestoreMon m[2] = { { "DP-1", A, TRUE, FALSE }, { "HDMI-0", A, TRUE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, X, m, 2) == 2);
    CHECK (m[0].restore && m[1].restore);
    CHECK (!g_strcmp0 (c.current, A));
    CHECK (own_choices (&c) == 0);
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "DP-1"), A));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "HDMI-0"), A));
    walpe_open_undo_clear (&u);
    cfg_clear (&c);
}

/* per-monitor wallpapers: each monitor returns to its own, also the
 * choice of an output that is not connected right now */
static void test_per_monitor (void)
{
    WalpeConfig c = { 0 };
    WalpeOpenUndo u = { 0 };
    c.current = g_strdup (A);
    walpe_config_set_monitor_path (&c, "HDMI-0", B);
    walpe_config_set_monitor_path (&c, "DP-9", C);      /* unplugged */
    open_all (&c, &u, X);
    CHECK (own_choices (&c) == 0);
    WalpeRestoreMon m[2] = { { "DP-1", A, TRUE, FALSE }, { "HDMI-0", B, TRUE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, X, m, 2) == 2);
    CHECK (!g_strcmp0 (c.current, A));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "DP-1"), A));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "HDMI-0"), B));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "DP-9"), C));
    CHECK (own_choices (&c) == 2);      /* HDMI-0 and DP-9; DP-1 follows the default */
    walpe_open_undo_clear (&u);
    cfg_clear (&c);
}

/* `open X --monitor 1` fails: only that monitor returns, the default and
 * the other monitor are not touched */
static void test_one_monitor (void)
{
    WalpeConfig c = { 0 };
    WalpeOpenUndo u = { 0 };
    c.current = g_strdup (A);
    /* it followed the default before */
    open_one (&c, "HDMI-0", X);
    WalpeRestoreMon m[2] = { { "DP-1", A, FALSE, FALSE }, { "HDMI-0", A, TRUE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, X, m, 2) == 1);
    CHECK (!m[0].restore && m[1].restore);
    CHECK (!g_strcmp0 (c.current, A));
    CHECK (own_choices (&c) == 0);
    /* it had its own wallpaper before */
    open_one (&c, "HDMI-0", B);
    open_one (&c, "HDMI-0", X);
    WalpeRestoreMon k[2] = { { "DP-1", A, FALSE, FALSE }, { "HDMI-0", B, TRUE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, X, k, 2) == 1);
    CHECK (!g_strcmp0 (c.current, A));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "HDMI-0"), B));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "DP-1"), A));
    cfg_clear (&c);
}

/* monitors of different sizes load on their own and fail one after the
 * other: the default returns with the last of them. Where the wallpaper
 * loaded on one monitor, that monitor keeps it and it stays the default. */
static void test_one_by_one (void)
{
    WalpeConfig c = { 0 };
    WalpeOpenUndo u = { 0 };
    c.current = g_strdup (A);
    walpe_config_set_monitor_path (&c, "HDMI-0", B);
    open_all (&c, &u, X);
    WalpeRestoreMon m[2] = { { "DP-1", A, TRUE, FALSE }, { "HDMI-0", B, FALSE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, X, m, 2) == 1);
    CHECK (!g_strcmp0 (c.current, X));                  /* HDMI-0 is still to show it */
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "DP-1"), A));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "HDMI-0"), X));
    WalpeRestoreMon k[2] = { { "DP-1", A, FALSE, FALSE }, { "HDMI-0", B, TRUE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, X, k, 2) == 1);
    CHECK (!g_strcmp0 (c.current, A));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "DP-1"), A));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "HDMI-0"), B));
    CHECK (own_choices (&c) == 1);
    walpe_open_undo_clear (&u);
    cfg_clear (&c);

    c.current = g_strdup (A);
    open_all (&c, &u, X);
    WalpeRestoreMon p[2] = { { "DP-1", A, TRUE, FALSE }, { "HDMI-0", X, FALSE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, X, p, 2) == 1);
    CHECK (!g_strcmp0 (c.current, X));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "DP-1"), A));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "HDMI-0"), X));
    walpe_open_undo_clear (&u);
    cfg_clear (&c);
}

/* nothing to return to: no wallpaper before, the one before is the one
 * that failed (a reload, or the re-open itself), not a member */
static void test_nothing (void)
{
    WalpeConfig c = { 0 };
    WalpeOpenUndo u = { 0 };
    c.current = g_strdup ("");
    open_all (&c, &u, X);
    WalpeRestoreMon m[2] = { { "DP-1", NULL, TRUE, FALSE }, { "HDMI-0", "", TRUE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, X, m, 2) == 0);
    CHECK (!m[0].restore && !m[1].restore);
    CHECK (!g_strcmp0 (c.current, X) && own_choices (&c) == 0);
    WalpeRestoreMon k[2] = { { "DP-1", X, TRUE, FALSE }, { "HDMI-0", A, FALSE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, X, k, 2) == 0);
    CHECK (!g_strcmp0 (c.current, X) && own_choices (&c) == 0);
    /* one member with a previous wallpaper, one without: the first returns,
     * the second stays on the failed one, which stays the default for it */
    WalpeRestoreMon p[2] = { { "DP-1", A, TRUE, FALSE }, { "HDMI-0", NULL, TRUE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, X, p, 2) == 1);
    CHECK (p[0].restore && !p[1].restore);
    CHECK (!g_strcmp0 (c.current, X));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "DP-1"), A));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "HDMI-0"), X));
    walpe_open_undo_clear (&u);
    cfg_clear (&c);
}

/* the default only goes back for the open that changed it */
static void test_undo_owner (void)
{
    WalpeConfig c = { 0 };
    WalpeOpenUndo u = { 0 };
    c.current = g_strdup (A);
    open_all (&c, &u, B);                               /* fine, shown */
    open_one (&c, "HDMI-0", Y);                         /* fails later */
    WalpeRestoreMon m[2] = { { "DP-1", B, FALSE, FALSE }, { "HDMI-0", B, TRUE, FALSE } };
    CHECK (walpe_restore_plan (&c, &u, Y, m, 2) == 1);
    CHECK (!g_strcmp0 (c.current, B));
    CHECK (own_choices (&c) == 0);
    /* no undo at all (the daemon was started with X as its default) */
    g_free (c.current);
    c.current = g_strdup (X);
    WalpeRestoreMon k[1] = { { "DP-1", A, TRUE, FALSE } };
    CHECK (walpe_restore_plan (&c, NULL, X, k, 1) == 1);
    CHECK (!g_strcmp0 (c.current, X));
    CHECK (!g_strcmp0 (walpe_config_monitor_path (&c, "DP-1"), A));
    walpe_open_undo_clear (&u);
    cfg_clear (&c);
}

int main (void)
{
    test_group ();
    test_per_monitor ();
    test_one_monitor ();
    test_one_by_one ();
    test_nothing ();
    test_undo_owner ();
    if (fails)
        g_printerr ("%d check(s) failed\n", fails);
    else
        g_print ("restore: all checks passed\n");
    return fails ? 1 : 0;
}
