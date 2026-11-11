/* features_test - the run-time switches of renderer features
 * (src/engine/features.c) and how they are kept and set.
 *
 * No daemon, no display, a private XDG_CONFIG_HOME:
 *  - defaults (on, but for parallax_offset), a change counts in the serial
 *    once, unknown names and indices are refused;
 *  - the features that are settings are rows of common/setkeys.h, as
 *    switches: the set check takes true / false and refuses text;
 *  - config.json: "features" survives save + load, switches that were
 *    never set are not written, names of another build are kept, names
 *    that are no identifiers and values that are no switches are dropped;
 *  - src/daemon/main.c (argv[1]) takes the features from the registry and
 *    the key table, not from literals of its own (so tests/ctl_test.c's
 *    scan of "set" has nothing to miss).
 * WALPE_FEATURES is cleared by meson.build: the switches start from their
 * defaults. */
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "engine/features.h"
#include "common/config.h"
#include "common/setkeys.h"
#include "common/util.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* the features that are settings in this build */
static const char *const own[] = { "camera_parallax", "parallax_offset", "script_camera",
                                   "sprite_sheets", "script_texanim", "bloom", "camera_shake" };

static void test_registry (void)
{
    int n = 0;
    for (int f = 0; walpe_feature_name (f); f++, n++) {
        const char *name = walpe_feature_name (f);
        CHECK (name && *name && walpe_feature_about (f) && *walpe_feature_about (f));
        CHECK (walpe_feature_find (name) == f);
        CHECK (walpe_feature ((WalpeFeature) f) == walpe_feature_default (f));
        CHECK (!walpe_feature_fixed (f));
        /* a setting name: what config.json and the set check accept */
        for (const char *p = name; *p; p++)
            CHECK (g_ascii_islower (*p) || g_ascii_isdigit (*p) || *p == '_');
        for (int g = 0; g < f; g++)
            CHECK (strcmp (name, walpe_feature_name (g)) != 0);
    }
    CHECK (n == WALPE_FEAT_N);
    /* the recorded defaults: on - but for the offset term of the camera
     * parallax, which moves layers while the pointer rests: a scene at
     * rest shows what its file says unless that term is asked for */
    CHECK (walpe_feature (WALPE_FEAT_CAMERA_PARALLAX) && !walpe_feature (WALPE_FEAT_PARALLAX_OFFSET));
    CHECK (!walpe_feature_default (WALPE_FEAT_PARALLAX_OFFSET) && walpe_feature_default (WALPE_FEAT_CAMERA_PARALLAX));
    for (int f = 0; walpe_feature_name (f); f++)
        CHECK (walpe_feature_default (f) == (f != WALPE_FEAT_PARALLAX_OFFSET));
    CHECK (walpe_feature (WALPE_FEAT_SCRIPT_CAMERA) && walpe_feature (WALPE_FEAT_SPRITE_SHEETS));
    CHECK (walpe_feature (WALPE_FEAT_SCRIPT_TEXANIM) && walpe_feature (WALPE_FEAT_BLOOM));
    CHECK (walpe_feature (WALPE_FEAT_CAMERA_SHAKE));
    /* decided when a scene loads */
    CHECK (walpe_feature_reloads (WALPE_FEAT_SPRITE_SHEETS) && walpe_feature_reloads (WALPE_FEAT_CAMERA_PARALLAX));
    CHECK (!walpe_feature_reloads (WALPE_FEAT_PARALLAX_OFFSET) && !walpe_feature_reloads (WALPE_FEAT_SCRIPT_TEXANIM) &&
           !walpe_feature_reloads (WALPE_FEAT_SCRIPT_CAMERA));

    guint s0 = walpe_feature_serial ();
    CHECK (walpe_feature_set ("sprite_sheets", FALSE) && !walpe_feature (WALPE_FEAT_SPRITE_SHEETS));
    CHECK (walpe_feature_serial () == s0 + 1);
    CHECK (walpe_feature_set ("sprite_sheets", FALSE) && walpe_feature_serial () == s0 + 1);
    CHECK (walpe_feature_set ("sprite_sheets", TRUE) && walpe_feature (WALPE_FEAT_SPRITE_SHEETS));
    CHECK (walpe_feature_serial () == s0 + 2);
    /* set to the default it already has: a value now, no change of the picture */
    CHECK (walpe_feature_set ("camera_parallax", TRUE) && walpe_feature_serial () == s0 + 2);
    CHECK (walpe_feature_set ("bloom", TRUE) && walpe_feature_serial () == s0 + 2);
    /* settings of other parts are no features */
    CHECK (!walpe_feature_set ("scene_sound", TRUE) && walpe_feature_find ("parallax") < 0);

    CHECK (!walpe_feature_set ("no_such_feature", TRUE) && !walpe_feature_set (NULL, TRUE));
    CHECK (!walpe_feature_set ("", TRUE) && walpe_feature_find ("") < 0 && walpe_feature_find (NULL) < 0);
    CHECK (!walpe_feature ((WalpeFeature) 99) && !walpe_feature ((WalpeFeature) -1));
    CHECK (walpe_feature_name (WALPE_FEAT_N) == NULL && walpe_feature_name (-1) == NULL);
    CHECK (walpe_feature_about (WALPE_FEAT_N) == NULL && !walpe_feature_default (-1));
    CHECK (!walpe_feature_reloads (WALPE_FEAT_N) && !walpe_feature_fixed (-1));
}

static char *check_json (const char *req)
{
    JsonNode *n = wpj_parse_data (req, -1);
    char *e = n && JSON_NODE_HOLDS_OBJECT (n) ? walpe_set_check (json_node_get_object (n)) : g_strdup ("?");
    if (n)
        json_node_unref (n);
    return e;
}

static void test_setkeys (void)
{
    for (guint i = 0; i < G_N_ELEMENTS (own); i++) {
        const WalpeSetKey *k = walpe_set_key_find (own[i]);
        CHECK (k && k->kind == WALPE_SET_BOOL);
        CHECK (walpe_feature_find (own[i]) >= 0);
        char *ok = g_strdup_printf ("{\"cmd\":\"set\",\"%s\":false}", own[i]);
        char *bad = g_strdup_printf ("{\"cmd\":\"set\",\"%s\":\"maybe\"}", own[i]);
        char *e = check_json (ok);
        CHECK (e == NULL);
        g_free (e);
        e = check_json (bad);
        CHECK (e && strstr (e, own[i]) && strstr (e, "nothing was changed"));
        g_free (e);
        g_free (ok);
        g_free (bad);
    }
    /* a typo is still refused */
    char *e = check_json ("{\"cmd\":\"set\",\"sprite_sheet\":false}");
    CHECK (e && strstr (e, "unknown setting"));
    g_free (e);
    /* every setting that is a feature name is a switch */
    for (int f = 0; walpe_feature_name (f); f++) {
        const WalpeSetKey *k = walpe_set_key_find (walpe_feature_name (f));
        CHECK (!k || k->kind == WALPE_SET_BOOL);
    }
}

static char *cfg_path;

static void test_config (void)
{
    g_unlink (cfg_path);
    WalpeConfig *c = walpe_config_load ();
    CHECK (c->features == NULL || g_hash_table_size (c->features) == 0);
    CHECK (walpe_config_get_feature (c, "sprite_sheets") == -1);
    walpe_config_save (c);
    JsonNode *root = wpj_parse_file (cfg_path);
    CHECK (root && JSON_NODE_HOLDS_OBJECT (root) &&
           !json_object_has_member (json_node_get_object (root), "features"));
    g_clear_pointer (&root, json_node_unref);

    walpe_config_set_feature (c, "sprite_sheets", FALSE);
    walpe_config_set_feature (c, "parallax_offset", TRUE);
    walpe_config_set_feature (c, "feature_of_another_build", FALSE);
    /* names that are no setting names are not kept */
    walpe_config_set_feature (c, "", TRUE);
    walpe_config_set_feature (c, NULL, TRUE);
    walpe_config_set_feature (c, "Has Space", TRUE);
    walpe_config_set_feature (c, "quote\"d", TRUE);
    walpe_config_set_feature (c, "UPPER", TRUE);
    char longname[80];
    memset (longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    walpe_config_set_feature (c, longname, TRUE);
    CHECK (g_hash_table_size (c->features) == 3);
    CHECK (walpe_config_get_feature (c, "sprite_sheets") == 0);
    CHECK (walpe_config_get_feature (c, "parallax_offset") == 1);
    CHECK (walpe_config_get_feature (c, "camera_parallax") == -1);
    walpe_config_save (c);
    walpe_config_free (c);

    c = walpe_config_load ();
    CHECK (c->features && g_hash_table_size (c->features) == 3);
    CHECK (walpe_config_get_feature (c, "sprite_sheets") == 0);
    CHECK (walpe_config_get_feature (c, "parallax_offset") == 1);
    CHECK (walpe_config_get_feature (c, "feature_of_another_build") == 0);
    walpe_config_free (c);

    /* a hand-written file: values that are no switches and names that are
     * no setting names are dropped, the rest holds; the member does not
     * end up among the members "this build does not know" a second time */
    g_assert (g_file_set_contents (cfg_path,
        "{\"fps\":60,\"features\":{\"sprite_sheets\":true,\"script_camera\":\"off\",\"bloom\":0,"
        "\"camera shake\":false,\"camera_shake\":false,\"x\":{\"y\":1},\"z\":[true]}}", -1, NULL));
    c = walpe_config_load ();
    CHECK (c->features && g_hash_table_size (c->features) == 2);
    CHECK (walpe_config_get_feature (c, "sprite_sheets") == 1);
    CHECK (walpe_config_get_feature (c, "camera_shake") == 0);
    CHECK (walpe_config_get_feature (c, "script_camera") == -1);
    walpe_config_save (c);
    walpe_config_free (c);
    root = wpj_parse_file (cfg_path);
    JsonObject *fo = root && JSON_NODE_HOLDS_OBJECT (root)
        ? wpj_obj (json_node_get_object (root), "features") : NULL;
    CHECK (fo && json_object_get_size (fo) == 2);
    g_clear_pointer (&root, json_node_unref);

    /* "features" that is no object: nothing is set, nothing breaks */
    g_assert (g_file_set_contents (cfg_path, "{\"fps\":60,\"features\":[1,2,3]}", -1, NULL));
    c = walpe_config_load ();
    CHECK (c->features == NULL || g_hash_table_size (c->features) == 0);
    walpe_config_free (c);

    /* more names than any build has features: bounded */
    c = walpe_config_load ();
    for (int i = 0; i < 2000; i++) {
        char nm[32];
        g_snprintf (nm, sizeof nm, "made_up_%d", i);
        walpe_config_set_feature (c, nm, TRUE);
    }
    CHECK (c->features && g_hash_table_size (c->features) <= 256);
    walpe_config_free (c);
    g_unlink (cfg_path);
}

/* the daemon's "set" takes features through the registry: no feature name
 * is a literal in ipc_set_keys (), and the block that does it is there */
static void test_daemon_source (const char *path)
{
    char *txt = NULL;
    if (!path || !g_file_get_contents (path, &txt, NULL, NULL)) {
        CHECK (!"daemon source readable");
        return;
    }
    const char *a = strstr (txt, "\nstatic void ipc_set_keys (JsonBuilder *b, JsonObject *o)\n{");
    const char *z = a ? strstr (a, "\n}\n") : NULL;
    CHECK (a && z);
    if (a && z) {
        char *body = g_strndup (a, (gsize) (z - a));
        CHECK (strstr (body, "walpe_feature_set (fname, on)") != NULL);
        CHECK (strstr (body, "walpe_set_key_find (fname)") != NULL);
        CHECK (strstr (body, "walpe_config_set_feature (D.cfg, fname, on)") != NULL);
        for (int f = 0; walpe_feature_name (f); f++) {
            char *lit = g_strdup_printf ("(o, \"%s\"", walpe_feature_name (f));
            CHECK (strstr (body, lit) == NULL);
            g_free (lit);
        }
        g_free (body);
    }
    CHECK (strstr (txt, "jb_str (b, \"features\", fs->str)") != NULL);
    g_free (txt);
}

int main (int argc, char **argv)
{
    /* before any GLib call that caches the config dir */
    char *tmp = g_dir_make_tmp ("walpe-features-test-XXXXXX", NULL);
    g_assert (tmp);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("HOME", tmp, TRUE);
    char *cfg_dir = g_build_filename (tmp, "walpe", NULL);
    g_mkdir_with_parents (cfg_dir, 0700);
    cfg_path = g_build_filename (cfg_dir, "config.json", NULL);

    test_registry ();
    test_setkeys ();
    test_config ();
    test_daemon_source (argc > 1 ? argv[1] : NULL);

    g_unlink (cfg_path);
    g_rmdir (cfg_dir);
    g_rmdir (tmp);
    g_free (cfg_path);      /* (the leak check of the asan_leaks setup counts the test's own strings too) */
    g_free (cfg_dir);
    g_free (tmp);
    if (fails)
        g_printerr ("features-test: %d check(s) failed\n", fails);
    else
        g_print ("features-test: ok\n");
    return fails ? 1 : 0;
}
