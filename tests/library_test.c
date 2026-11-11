/* library_test.c - wallpaper deletion (src/common/library.c): which paths
 * may be deleted, what a delete removes, trash, permanent removal, and
 * forgetting a wallpaper in the config.
 *
 * Everything happens in a private temp dir that is also HOME and the XDG
 * dirs (set before GLib caches them), with a fake Steam library, a fake
 * steamcmd download dir and a WalPE library in it. The trash is therefore
 * <tmp>/xdg-data/Trash; nothing of the user is touched.
 *
 * Run: meson test -C build library -v */
#include "common/library.h"
#include "common/config.h"
#include "common/util.h"
#include "common/workshop.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static char *T;      /* temp root (realpath) */
static char *LIB;    /* <T>/lib */

#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                 failures++; } } while (0)

static char *P (const char *rel)
{
    return g_build_filename (T, rel, NULL);
}

static void mkfile (const char *rel, const char *text)
{
    char *p = P (rel);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0755);
    if (!g_file_set_contents (p, text, -1, NULL))
        g_error ("cannot write %s", p);
    g_free (d);
    g_free (p);
}

static void mklink (const char *target_rel, const char *link_rel)
{
    char *t = P (target_rel), *l = P (link_rel);
    if (symlink (t, l) != 0)
        g_error ("symlink %s", l);
    g_free (t);
    g_free (l);
}

static gboolean exists (const char *rel)
{
    char *p = P (rel);
    gboolean e = g_file_test (p, G_FILE_TEST_EXISTS) || g_file_test (p, G_FILE_TEST_IS_SYMLINK);
    g_free (p);
    return e;
}

static gboolean is_link (const char *rel)
{
    char *p = P (rel);
    gboolean e = g_file_test (p, G_FILE_TEST_IS_SYMLINK);
    g_free (p);
    return e;
}

#define STEAM "home/.local/share/Steam/steamapps/workshop/content/431960"
#define SCMD  "steamcmd/steamapps/workshop/content/431960"

static WalpeDelPlan *plan_rel (const char *rel, GError **err)
{
    char *p = rel[0] == '/' ? g_strdup (rel) : P (rel);
    WalpeDelPlan *pl = walpe_library_delete_plan (p, LIB, err);
    g_free (p);
    return pl;
}

/* refused, with a message, and nothing planned */
static void refused (const char *rel)
{
    GError *err = NULL;
    WalpeDelPlan *pl = plan_rel (rel, &err);
    if (pl || !err || !err->message || !*err->message) {
        g_printerr ("FAIL: “%s” was not refused\n", rel);
        failures++;
    }
    walpe_del_plan_free (pl);
    g_clear_error (&err);
}

static WalpeDelTarget *tgt (WalpeDelPlan *pl, guint i)
{
    return pl && i < pl->targets->len ? pl->targets->pdata[i] : NULL;
}

static gboolean has_target (WalpeDelPlan *pl, const char *rel, WalpeDelKind kind,
                            WalpeRootKind root)
{
    char *p = P (rel);
    gboolean hit = FALSE;
    for (guint i = 0; pl && i < pl->targets->len; i++) {
        WalpeDelTarget *t = pl->targets->pdata[i];
        hit |= !strcmp (t->path, p) && t->kind == kind && t->root == root;
    }
    g_free (p);
    return hit;
}

static void build_tree (void)
{
    mkfile ("lib/MyScene/project.json", "{\"type\":\"scene\",\"title\":\"My scene\"}");
    mkfile ("lib/MyScene/scene.pkg", "pkg");
    mkfile ("lib/MyScene/tex.png", "png");
    mkfile ("lib/MyScene/sub/deep/file.bin", "x");
    mkfile ("lib/Other/project.json", "{}");
    mkfile ("lib/NoProject/file.txt", "x");
    mkfile ("lib/.hidden/project.json", "{}");
    mkfile ("lib/imported/clip.mp4", "video");
    mkfile ("lib/imported/clip.jpg", "thumb");
    mkfile ("lib/imported/pic.png", "png");
    mkfile ("lib/imported/notes.txt", "x");
    mkfile ("lib/loose.gif", "gif");
    mkfile ("lib/333/project.json", "{\"workshopid\":\"333\"}");
    mkfile ("lib/333/a/b.tex", "x");
    mkfile ("lib/111/project.json", "{}");
    mkfile ("lib/555/project.json", "{}");
    mkfile ("lib/3122339805/project.json", "{\"title\":\"Base\"}");
    mkfile ("lib/3493971117/project.json",
            "{\"title\":\"A preset\",\"dependency\":\"3122339805\",\"preset\":{}}");
    mkfile (STEAM "/3493971117/project.json",
            "{\"title\":\"A preset\",\"dependency\":3122339805}");
    mkfile ("lib/77777777/project.json",
            "{\"title\":\"Not a preset\",\"dependency\":\"3122339805\",\"file\":\"x.mp4\"}");
    mkfile ("lib/Perm/project.json", "{}");
    mkfile ("lib/Perm/a/b/c/d.bin", "x");
    mkfile ("lib/Perm/ro/file", "x");
    mkfile ("outside/proj/project.json", "{}");
    mkfile ("outside/proj/data.bin", "data");
    mkfile ("outside/secret.txt", "secret");
    mkfile ("outside/media/x.mp4", "video");
    mkfile (STEAM "/111/project.json", "{}");
    mkfile (STEAM "/222/project.json", "{}");
    mkfile (STEAM "/222/scene.pkg", "pkg");
    mkfile (STEAM "/loose.mp4", "video");
    mkfile (SCMD "/333/project.json", "{}");
    mkfile (SCMD "/111/project.json", "{}");
    mklink ("outside/proj", "lib/linked");
    mklink (STEAM "/222", "lib/222");
    mklink ("outside/media", "lib/imported-link");
    mklink ("outside/media/x.mp4", "lib/imported/linked.mp4");
    mklink ("outside", "lib/MyScene/escape");
    mklink ("outside", "lib/Perm/escape-dir");
    mklink ("outside/secret.txt", "lib/Perm/escape-file");
    mklink ("lib", "liblink");
    char *ro = P ("lib/Perm/ro");
    g_chmod (ro, 0555);
    g_free (ro);
    /* the steamcmd root the Workshop tab's last install used */
    char *sc = P ("steamcmd");
    mkfile ("xdg-cache/walpe/steamcmd-root", sc);
    g_free (sc);
}

static void test_validation (void)
{
    GError *err = NULL;
    WalpeDelPlan *pl = plan_rel ("lib/MyScene", &err);
    CHECK (pl && !err);
    CHECK (pl && pl->targets->len == 1);
    CHECK (has_target (pl, "lib/MyScene", WALPE_DEL_DIR, WALPE_ROOT_LIBRARY));
    CHECK (pl && tgt (pl, 0)->primary && !pl->steam_managed && !pl->workshop_id);
    walpe_del_plan_free (pl);
    g_clear_error (&err);

    /* other spellings of the same entry resolve to the real one */
    const char *same[] = { "liblink/MyScene", "lib/MyScene/", "lib//MyScene", "lib/./MyScene",
                           NULL };
    for (int i = 0; same[i]; i++) {
        pl = plan_rel (same[i], NULL);
        CHECK (pl && pl->targets->len == 1);
        CHECK (has_target (pl, "lib/MyScene", WALPE_DEL_DIR, WALPE_ROOT_LIBRARY));
        walpe_del_plan_free (pl);
    }

    /* never a root, a parent of one, anything outside, ".." tricks */
    refused ("lib");
    refused ("liblink");
    refused ("");                       /* = T itself */
    refused ("/");
    refused ("home");
    refused ("home/.local/share/Steam");
    refused (STEAM);
    refused ("home/.local/share/Steam/steamapps/workshop/content");
    refused ("steamcmd");
    refused (SCMD);
    refused ("outside/proj");
    refused ("lib/..");
    refused ("lib/MyScene/..");
    refused ("lib/MyScene/../../outside/proj");
    refused ("lib/MyScene/../MyScene");
    refused (STEAM "/../431960/111");
    refused ("/lib/MyScene");           /* absolute, elsewhere */
    {
        GError *e2 = NULL;
        CHECK (!walpe_library_delete_plan ("lib/MyScene", LIB, &e2) && e2);   /* relative */
        g_clear_error (&e2);
        CHECK (!walpe_library_delete_plan (NULL, LIB, &e2) && e2);
        g_clear_error (&e2);
    }
    /* not wallpapers */
    refused ("lib/NoProject");
    refused ("lib/imported");
    refused ("lib/.hidden");
    refused ("lib/MyScene/sub");
    refused ("lib/MyScene/sub/deep/file.bin");
    refused ("lib/MyScene/project.json");
    refused ("lib/MyScene/tex.png");    /* media, but inside a wallpaper folder */
    refused ("lib/imported/notes.txt");
    refused ("lib/nope");
    refused (STEAM "/loose.mp4");       /* loose media only in the WalPE library */
    /* through a symlink: the parent resolves outside the library */
    refused ("lib/linked/project.json");
    refused ("lib/imported-link/x.mp4");
    refused ("lib/MyScene/escape/proj");

    /* a link in the library: the link itself, wherever it points */
    pl = plan_rel ("lib/linked", NULL);
    CHECK (pl && pl->targets->len == 1);
    CHECK (has_target (pl, "lib/linked", WALPE_DEL_LINK, WALPE_ROOT_LIBRARY));
    char *lt = P ("outside/proj");
    CHECK (pl && !g_strcmp0 (tgt (pl, 0)->link_target, lt));
    g_free (lt);
    walpe_del_plan_free (pl);
    pl = plan_rel ("lib/linked/", NULL);   /* trailing slash does not follow it */
    CHECK (pl && has_target (pl, "lib/linked", WALPE_DEL_LINK, WALPE_ROOT_LIBRARY));
    walpe_del_plan_free (pl);

    /* loose media */
    pl = plan_rel ("lib/imported/clip.mp4", NULL);
    CHECK (pl && pl->targets->len == 2);
    CHECK (has_target (pl, "lib/imported/clip.mp4", WALPE_DEL_FILE, WALPE_ROOT_LIBRARY));
    CHECK (has_target (pl, "lib/imported/clip.jpg", WALPE_DEL_THUMB, WALPE_ROOT_LIBRARY));
    CHECK (pl && !pl->steam_managed);
    walpe_del_plan_free (pl);
    pl = plan_rel ("lib/imported/pic.png", NULL);
    CHECK (pl && pl->targets->len == 1);
    walpe_del_plan_free (pl);
    pl = plan_rel ("lib/loose.gif", NULL);
    CHECK (pl && pl->targets->len == 1 && tgt (pl, 0)->kind == WALPE_DEL_FILE);
    walpe_del_plan_free (pl);

    /* workshop items: every copy in the known roots */
    pl = plan_rel ("lib/333", NULL);
    CHECK (pl && pl->targets->len == 2);
    CHECK (has_target (pl, "lib/333", WALPE_DEL_DIR, WALPE_ROOT_LIBRARY));
    CHECK (has_target (pl, SCMD "/333", WALPE_DEL_DIR, WALPE_ROOT_STEAMCMD));
    CHECK (pl && pl->steam_managed && !pl->workshop_id);   /* short name: no id shown */
    CHECK (pl && tgt (pl, 0)->primary && !tgt (pl, 1)->primary);
    CHECK (pl && g_strv_length (pl->forget) == 2);
    walpe_del_plan_free (pl);
    pl = plan_rel ("lib/111", NULL);
    CHECK (pl && pl->targets->len == 3);
    CHECK (has_target (pl, STEAM "/111", WALPE_DEL_DIR, WALPE_ROOT_STEAM));
    CHECK (has_target (pl, SCMD "/111", WALPE_DEL_DIR, WALPE_ROOT_STEAMCMD));
    walpe_del_plan_free (pl);
    pl = plan_rel ("lib/555", NULL);     /* digits, no other copy */
    CHECK (pl && pl->targets->len == 1 && !pl->steam_managed && !pl->workshop_id);
    CHECK (pl && pl->dependents && !pl->dependents[0]);
    walpe_del_plan_free (pl);
    /* a real workshop id; a preset is based on it (listed once, though it
     * is installed twice; an item with its own "file" is no preset) */
    pl = plan_rel ("lib/3122339805", NULL);
    CHECK (pl && pl->targets->len == 1 && !pl->steam_managed);
    CHECK (pl && !g_strcmp0 (pl->workshop_id, "3122339805"));
    CHECK (pl && pl->dependents && g_strv_length (pl->dependents) == 1 &&
           !g_strcmp0 (pl->dependents[0], "A preset"));
    walpe_del_plan_free (pl);
    pl = plan_rel ("lib/3493971117", NULL);   /* the preset itself: both copies */
    CHECK (pl && pl->targets->len == 2 && pl->steam_managed);
    CHECK (pl && pl->dependents && !pl->dependents[0]);
    walpe_del_plan_free (pl);
    /* a linked media file in imported/: the link, not the file */
    pl = plan_rel ("lib/imported/linked.mp4", NULL);
    CHECK (pl && pl->targets->len == 1 && tgt (pl, 0)->kind == WALPE_DEL_LINK);
    walpe_del_plan_free (pl);
    /* "Add folder" link to the Steam copy: the link and the Steam folder */
    pl = plan_rel ("lib/222", NULL);
    CHECK (pl && pl->targets->len == 2 && pl->steam_managed);
    CHECK (has_target (pl, "lib/222", WALPE_DEL_LINK, WALPE_ROOT_LIBRARY));
    CHECK (has_target (pl, STEAM "/222", WALPE_DEL_DIR, WALPE_ROOT_STEAM));
    CHECK (pl && tgt (pl, 0)->kind == WALPE_DEL_LINK);
    walpe_del_plan_free (pl);
    /* a Steam-library item asked for directly */
    pl = plan_rel (STEAM "/222", NULL);
    CHECK (pl && pl->targets->len == 2 && pl->steam_managed);
    CHECK (pl && tgt (pl, 0)->kind == WALPE_DEL_DIR && tgt (pl, 0)->root == WALPE_ROOT_STEAM);
    walpe_del_plan_free (pl);
}

static gboolean trash_all (WalpeDelPlan *pl, GError **err)
{
    for (guint i = 0; pl && i < pl->targets->len; i++)
        if (!walpe_library_trash (pl->targets->pdata[i], LIB, err))
            return FALSE;
    return pl != NULL;
}

static void test_trash (void)
{
    GError *err = NULL;
    /* a folder: moved as a whole, the link inside it is not followed */
    WalpeDelPlan *pl = plan_rel ("lib/MyScene", NULL);
    CHECK (trash_all (pl, &err));
    if (err)
        g_printerr ("trash: %s\n", err->message);
    g_clear_error (&err);
    CHECK (!exists ("lib/MyScene"));
    CHECK (exists ("xdg-data/Trash/files/MyScene/project.json"));
    CHECK (exists ("xdg-data/Trash/files/MyScene/sub/deep/file.bin"));
    CHECK (exists ("xdg-data/Trash/info/MyScene.trashinfo"));
    CHECK (exists ("outside/secret.txt") && exists ("outside/proj/data.bin"));
    /* the same plan again: the target is gone, nothing else happens */
    CHECK (!walpe_library_trash (tgt (pl, 0), LIB, &err) && err);
    g_clear_error (&err);
    walpe_del_plan_free (pl);

    /* a link: only the link */
    pl = plan_rel ("lib/linked", NULL);
    CHECK (trash_all (pl, &err));
    g_clear_error (&err);
    CHECK (!exists ("lib/linked"));
    CHECK (is_link ("xdg-data/Trash/files/linked"));
    CHECK (exists ("outside/proj/project.json") && exists ("outside/proj/data.bin"));
    walpe_del_plan_free (pl);

    /* imported video + its thumbnail; the neighbours stay */
    pl = plan_rel ("lib/imported/clip.mp4", NULL);
    CHECK (trash_all (pl, &err));
    g_clear_error (&err);
    CHECK (!exists ("lib/imported/clip.mp4") && !exists ("lib/imported/clip.jpg"));
    CHECK (exists ("lib/imported/pic.png") && exists ("lib/imported/notes.txt"));
    walpe_del_plan_free (pl);
    pl = plan_rel ("lib/imported/linked.mp4", NULL);
    CHECK (trash_all (pl, &err));
    g_clear_error (&err);
    CHECK (!exists ("lib/imported/linked.mp4") && exists ("outside/media/x.mp4"));
    CHECK (exists ("xdg-data/Trash/files/clip.mp4") && exists ("xdg-data/Trash/files/clip.jpg"));
    walpe_del_plan_free (pl);

    /* workshop item with Steam + steamcmd copies */
    pl = plan_rel ("lib/111", NULL);
    CHECK (trash_all (pl, &err));
    g_clear_error (&err);
    CHECK (!exists ("lib/111") && !exists (STEAM "/111") && !exists (SCMD "/111"));
    CHECK (exists (STEAM "/222/project.json") && exists (SCMD "/333/project.json"));
    walpe_del_plan_free (pl);

    /* the folder is swapped for a link after the dialog was confirmed */
    pl = plan_rel ("lib/555", NULL);
    char *p555 = P ("lib/555"), *aside = P ("lib/555.aside"), *out = P ("outside");
    CHECK (g_rename (p555, aside) == 0 && symlink (out, p555) == 0);
    CHECK (!walpe_library_trash (tgt (pl, 0), LIB, &err) && err);
    g_clear_error (&err);
    CHECK (!walpe_library_remove (tgt (pl, 0), LIB, &err) && err);
    g_clear_error (&err);
    CHECK (is_link ("lib/555") && exists ("outside/secret.txt"));
    CHECK (unlink (p555) == 0 && g_rename (aside, p555) == 0);
    g_free (p555);
    g_free (aside);
    g_free (out);
    walpe_del_plan_free (pl);
}

static void test_remove (void)
{
    GError *err = NULL;
    WalpeDelPlan *pl = plan_rel ("lib/Perm", NULL);
    CHECK (pl && walpe_library_remove (tgt (pl, 0), LIB, &err));
    if (err)
        g_printerr ("remove: %s\n", err->message);
    g_clear_error (&err);
    CHECK (!exists ("lib/Perm"));
    /* links inside it were removed, not followed */
    CHECK (exists ("outside/secret.txt") && exists ("outside/proj/project.json") &&
           exists ("outside/media/x.mp4"));
    CHECK (!exists ("xdg-data/Trash/files/Perm"));
    walpe_del_plan_free (pl);

    pl = plan_rel ("lib/222", NULL);     /* link + Steam folder */
    for (guint i = 0; pl && i < pl->targets->len; i++)
        CHECK (walpe_library_remove (pl->targets->pdata[i], LIB, &err));
    g_clear_error (&err);
    CHECK (!exists ("lib/222") && !exists (STEAM "/222"));
    CHECK (exists (STEAM "/loose.mp4"));
    walpe_del_plan_free (pl);

    pl = plan_rel ("lib/loose.gif", NULL);
    CHECK (pl && walpe_library_remove (tgt (pl, 0), LIB, &err));
    g_clear_error (&err);
    CHECK (!exists ("lib/loose.gif") && exists ("lib/Other/project.json"));
    walpe_del_plan_free (pl);

    /* a hand-made target is checked like any other */
    WalpeDelTarget bad = { .path = LIB, .kind = WALPE_DEL_DIR };
    CHECK (!walpe_library_remove (&bad, LIB, &err) && err);
    g_clear_error (&err);
    CHECK (!walpe_library_trash (&bad, LIB, &err) && err);
    g_clear_error (&err);
    char *o = P ("outside/proj");
    WalpeDelTarget bad2 = { .path = o, .kind = WALPE_DEL_DIR };
    CHECK (!walpe_library_remove (&bad2, LIB, &err) && err);
    g_clear_error (&err);
    g_free (o);
    CHECK (exists ("lib/Other/project.json") && exists ("outside/proj/project.json"));
}

static void test_config (void)
{
    char *a = P ("lib/Other"), *a2 = P ("liblink/Other"), *b = P ("lib/333");
    char *l = P ("lib/linked2"), *lt = P ("outside/proj");
    CHECK (symlink (lt, l) == 0);
    CHECK (walpe_path_same (a, a2));
    char *a3 = g_strconcat (T, "/lib//Other/", NULL);
    CHECK (walpe_path_same (a, a3));
    g_free (a3);
    CHECK (!walpe_path_same (a, b));
    CHECK (!walpe_path_same (l, lt));     /* a link is not its target */
    CHECK (!walpe_path_same (a, "") && !walpe_path_same (NULL, a));

    WalpeConfig *c = walpe_config_load ();   /* no file yet: defaults */
    g_free (c->current);
    c->current = g_strdup (a);
    walpe_config_set_monitor_path (c, "HDMI-0", a2);
    walpe_config_set_monitor_path (c, "DP-2", b);
    walpe_config_set_prop (c, a, "speed", "2");
    walpe_config_set_prop (c, a2, "colour", "1 0 0");
    walpe_config_set_prop (c, b, "keep", "yes");
    CHECK (walpe_config_forget_path (c, a) == 4);
    CHECK (!g_strcmp0 (c->current, ""));
    CHECK (!g_hash_table_contains (c->monitor_current, "HDMI-0"));
    CHECK (!g_strcmp0 (g_hash_table_lookup (c->monitor_current, "DP-2"), b));
    char *v = walpe_config_get_prop (a, "speed");
    CHECK (v == NULL);
    g_free (v);
    v = walpe_config_get_prop (a2, "colour");
    CHECK (v == NULL);
    g_free (v);
    v = walpe_config_get_prop (b, "keep");
    CHECK (!g_strcmp0 (v, "yes"));
    g_free (v);
    CHECK (walpe_config_forget_path (c, a) == 0);   /* nothing left */
    /* a per-monitor wallpaper goes: that monitor follows the default */
    g_free (c->current);
    c->current = g_strdup (a);
    CHECK (walpe_config_forget_path (c, b) == 2);
    CHECK (!g_strcmp0 (walpe_config_monitor_path (c, "DP-2"), a));
    walpe_config_save (c);
    walpe_config_free (c);

    /* GUI without a daemon: config.json is edited on disk */
    const char *paths[] = { a2, NULL };
    CHECK (walpe_config_forget_offline (paths) == 1);
    c = walpe_config_load ();
    CHECK (!c->current || !*c->current);   /* "" loads as unset */
    walpe_config_free (c);
    CHECK (walpe_config_forget_offline (paths) == 0);

    unlink (l);
    g_free (a);
    g_free (a2);
    g_free (b);
    g_free (l);
    g_free (lt);
}

int main (int argc, char **argv)
{
    (void) argc; (void) argv;
    /* private HOME / XDG dirs before GLib caches them */
    char *tmp = g_dir_make_tmp ("walpe-lib-test-XXXXXX", NULL);
    if (!tmp)
        g_error ("no temp dir");
    T = realpath (tmp, NULL);
    g_free (tmp);
    LIB = P ("lib");
    char *home = P ("home"), *cfg = P ("xdg-config"), *cache = P ("xdg-cache"),
         *data = P ("xdg-data");
    g_setenv ("HOME", home, TRUE);
    g_setenv ("XDG_CONFIG_HOME", cfg, TRUE);
    g_setenv ("XDG_CACHE_HOME", cache, TRUE);
    g_setenv ("XDG_DATA_HOME", data, TRUE);
    g_mkdir_with_parents (home, 0755);
    g_mkdir_with_parents (data, 0755);

    build_tree ();
    test_validation ();
    test_trash ();
    test_remove ();
    test_config ();

    g_printerr ("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
                failures == 1 ? "" : "s");
    /* the tree has a read-only folder only if test_remove did not run */
    char *ro = P ("lib/Perm/ro");
    g_chmod (ro, 0755);
    g_free (ro);
    char *rm[] = { "rm", "-rf", T, NULL };
    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free (home);
    g_free (cfg);
    g_free (cache);
    g_free (data);
    g_free (LIB);
    free (T);
    return failures ? 1 : 0;
}
