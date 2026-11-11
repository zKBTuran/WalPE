/* layers_test - "Adjust layers" (common/layeradj.h, engine/scenenode_layers.h)
 *
 *  1. layer keys: make / parse, the key of a layer, matching a saved key
 *     to a scene (id first, name as fallback, duplicates, missing)
 *  2. selectors (walpe-ctl), selections on the layer tree
 *  3. validation: what an adjustment may hold
 *  4. config.json round trip, bad entries in the file, forgetting a
 *     wallpaper (the delete flow)
 *  5. rendering (needs an EGL device or Mesa's surfaceless platform, else
 *     skipped): a scene made here - flat blocks, a child, a block whose
 *     origin a SceneScript rewrites every frame, a child of a rotated and
 *     scaled parent, text, a particle - is drawn into a pbuffer and the
 *     pixels are checked: a layer moved by (dx, dy) lands exactly there,
 *     children follow, scale, hide / show, other monitor sizes (crop), a
 *     camera layer, and that a scene without (matching) adjustments is
 *     drawn bit for bit as before.
 *
 * Works in a private temp dir that is also its HOME / XDG_CONFIG_HOME. */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include "walpe-build-config.h"
#include "common/config.h"
#include "common/layeradj.h"
#include "common/library.h"
#include "common/util.h"
#include "engine/content.h"
#include "engine/glutil.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ------------------------------------------------------------ 1. keys */

static void test_keys (void)
{
    char *k = walpe_layer_key_make (123, 0, "Cover");
    CHECK (!strcmp (k, "123:Cover"));
    g_free (k);
    k = walpe_layer_key_make (123, 2, "Cover");
    CHECK (!strcmp (k, "123~3:Cover"));
    g_free (k);
    k = walpe_layer_key_make (-1, 0, NULL);
    CHECK (!strcmp (k, "-:"));
    g_free (k);
    k = walpe_layer_key_make (-1, 1, "a:b#c");
    CHECK (!strcmp (k, "-~2:a:b#c"));
    g_free (k);

    int id, nth;
    const char *name;
    CHECK (walpe_layer_key_parse ("123:Cover", &id, &nth, &name) && id == 123 && nth == 0 &&
           !strcmp (name, "Cover"));
    CHECK (walpe_layer_key_parse ("7~3:x:y", &id, &nth, &name) && id == 7 && nth == 2 &&
           !strcmp (name, "x:y"));
    CHECK (walpe_layer_key_parse ("-:", &id, &nth, &name) && id == -1 && nth == 0 && !*name);
    CHECK (walpe_layer_key_parse ("-~2:n", &id, &nth, &name) && id == -1 && nth == 1);
    CHECK (walpe_layer_key_parse ("0:", &id, NULL, NULL) && id == 0);
    static const char *const bad[] = { "", "Cover", "123", ":x", "12a:x", "~2:x", "5~:x", "5~1:x",
                                       "5~0:x", "-5:x", "1234567890:x", "5~x:y", " 5:x", NULL };
    for (int i = 0; bad[i]; i++)
        CHECK (!walpe_layer_key_parse (bad[i], NULL, NULL, NULL));
    CHECK (!walpe_layer_key_parse (NULL, NULL, NULL, NULL));
    CHECK (!walpe_layer_key_parse ("5:\xff\xfe", NULL, NULL, NULL));   /* not UTF-8 */
    {
        char *lng = g_strnfill (WALPE_LAYER_KEY_MAX + 1, 'a');
        lng[0] = '5';
        lng[1] = ':';
        CHECK (!walpe_layer_key_parse (lng, NULL, NULL, NULL));
        g_free (lng);
    }

    /* a scene: ids unique, one pair with the same id and name, two objects
     * without id that share a name, an unnamed one */
    const WalpeLayerRef sc[] = {
        { 10, "bg" }, { 20, "cover" }, { 21, "title" }, { 21, "title" }, { -1, "deco" },
        { -1, "deco" }, { 30, NULL }, { 31, "title" }, { 40, "clock" },
    };
    const guint n = G_N_ELEMENTS (sc);
    static const char *const want[] = { "10:bg", "20:cover", "21:title", "21~2:title", "-:deco",
                                        "-~2:deco", "30:", "31:title", "40:clock" };
    for (guint i = 0; i < n; i++) {
        k = walpe_layer_key_for (sc, n, i);
        CHECK (!strcmp (k, want[i]));
        CHECK (walpe_layer_find (sc, n, k) == (int) i);   /* every key finds its own layer */
        g_free (k);
    }
    CHECK (walpe_layer_key_for (sc, n, n) == NULL);
    /* renamed layer: the id decides */
    CHECK (walpe_layer_find (sc, n, "20:Album Art") == 1);
    CHECK (walpe_layer_find (sc, n, "21~2:renamed") == 3);
    CHECK (walpe_layer_find (sc, n, "21~3:title") == -1);     /* there are only two */
    /* the id is gone (renumbered scene): the one layer with that name */
    CHECK (walpe_layer_find (sc, n, "99:cover") == 1);
    CHECK (walpe_layer_find (sc, n, "99:clock") == 8);
    CHECK (walpe_layer_find (sc, n, "99:title") == -1);       /* three candidates: none */
    CHECK (walpe_layer_find (sc, n, "99:deco") == -1);
    CHECK (walpe_layer_find (sc, n, "99:nothing") == -1);
    CHECK (walpe_layer_find (sc, n, "99:") == -1);            /* no id, no name: nothing */
    /* keys without id: by name, in order */
    CHECK (walpe_layer_find (sc, n, "-:deco") == 4);
    CHECK (walpe_layer_find (sc, n, "-~2:deco") == 5);
    CHECK (walpe_layer_find (sc, n, "-~3:deco") == -1);
    CHECK (walpe_layer_find (sc, n, "-:cover") == 1);         /* it has an id now */
    CHECK (walpe_layer_find (sc, n, "-:") == -1);
    CHECK (walpe_layer_find (sc, n, "not a key") == -1);
    CHECK (walpe_layer_find (sc, 0, "10:bg") == -1);
    CHECK (walpe_layer_find (NULL, 0, "10:bg") == -1);

    /* selectors */
    GArray *out = g_array_new (FALSE, FALSE, sizeof (int));
    char *err = NULL;
    CHECK (walpe_layer_select (sc, n, "20:cover", out, &err) && out->len == 1 &&
           g_array_index (out, int, 0) == 1 && !err);
    g_array_set_size (out, 0);
    CHECK (walpe_layer_select (sc, n, "40", out, &err) && out->len == 1 &&
           g_array_index (out, int, 0) == 8);
    g_array_set_size (out, 0);
    CHECK (walpe_layer_select (sc, n, "21", out, &err) && out->len == 2);   /* both with id 21 */
    g_array_set_size (out, 0);
    CHECK (walpe_layer_select (sc, n, "clock", out, &err) && out->len == 1 &&
           g_array_index (out, int, 0) == 8);
    g_array_set_size (out, 0);
    CHECK (!walpe_layer_select (sc, n, "title", out, &err) && out->len == 0 && err &&
           strstr (err, "21~2:title") && strstr (err, "31:title"));
    g_clear_pointer (&err, g_free);
    CHECK (!walpe_layer_select (sc, n, "nothing", out, &err) && err);
    g_clear_pointer (&err, g_free);
    CHECK (!walpe_layer_select (sc, n, "", out, &err) && err);
    g_clear_pointer (&err, g_free);
    CHECK (!walpe_layer_select (sc, n, NULL, out, &err) && err);
    g_clear_pointer (&err, g_free);
    CHECK (!walpe_layer_select (sc, n, "77", out, &err) && err);   /* no such id, no such name */
    g_clear_pointer (&err, g_free);
    g_array_unref (out);
}

/* ----------------------------------------------------- 2. selections */

static void test_tree (void)
{
    /*      0        4
     *     / \       |
     *    1   2      5        7 (cycle with 8)
     *    |
     *    3   6 (root) */
    const int parent[] = { -1, 0, 0, 1, -1, 4, -1, 8, 7 };
    const guint n = G_N_ELEMENTS (parent);
    gboolean sel[9] = { 0 }, roots[9];
    sel[0] = sel[3] = sel[5] = TRUE;
    walpe_layer_sel_roots (parent, n, sel, roots);
    CHECK (roots[0] && !roots[3] && roots[5] && !roots[1] && !roots[4]);
    memset (sel, 0, sizeof sel);
    sel[1] = sel[2] = sel[3] = TRUE;
    walpe_layer_sel_roots (parent, n, sel, roots);
    CHECK (roots[1] && roots[2] && !roots[3]);
    memset (sel, 0, sizeof sel);
    sel[0] = sel[6] = TRUE;
    walpe_layer_sel_children (parent, n, sel);
    CHECK (sel[0] && sel[1] && sel[2] && sel[3] && !sel[4] && !sel[5] && sel[6] && !sel[7]);
    /* a parent cycle neither hangs nor selects itself into a root loop */
    memset (sel, 0, sizeof sel);
    sel[7] = TRUE;
    walpe_layer_sel_children (parent, n, sel);
    CHECK (sel[7] && sel[8]);
    walpe_layer_sel_roots (parent, n, sel, roots);
    CHECK (!roots[0]);
    walpe_layer_sel_roots (parent, 0, sel, roots);
}

/* ----------------------------------------------------- 3. validation */

static void test_valid (void)
{
    WalpeLayerAdj a = { .key = (char *) "5:x", .dx = 0, .dy = 0, .scale = 1 };
    CHECK (!walpe_layer_adj_check (&a));
    CHECK (walpe_layer_adj_is_noop (&a));
    a.dx = WALPE_LAYER_OFFSET_MAX;
    a.dy = -WALPE_LAYER_OFFSET_MAX;
    CHECK (!walpe_layer_adj_check (&a) && !walpe_layer_adj_is_noop (&a));
    a.dx = WALPE_LAYER_OFFSET_MAX + 1;
    CHECK (walpe_layer_adj_check (&a) != NULL);
    a.dx = NAN;
    CHECK (walpe_layer_adj_check (&a) != NULL);
    a.dx = INFINITY;
    CHECK (walpe_layer_adj_check (&a) != NULL);
    a.dx = 0;
    a.dy = -INFINITY;
    CHECK (walpe_layer_adj_check (&a) != NULL);
    a.dy = 0;
    static const double bad_scale[] = { 0, -1, 0.009, 100.5, 1e300 };
    for (guint i = 0; i < G_N_ELEMENTS (bad_scale); i++) {
        a.scale = bad_scale[i];
        CHECK (walpe_layer_adj_check (&a) != NULL);
    }
    a.scale = NAN;
    CHECK (walpe_layer_adj_check (&a) != NULL);
    a.scale = WALPE_LAYER_SCALE_MIN;
    CHECK (!walpe_layer_adj_check (&a));
    a.scale = WALPE_LAYER_SCALE_MAX;
    CHECK (!walpe_layer_adj_check (&a));
    a.scale = 1;
    a.vis = 3;
    CHECK (walpe_layer_adj_check (&a) != NULL);
    a.vis = WALPE_LAYER_VIS_HIDDEN;
    CHECK (!walpe_layer_adj_check (&a) && !walpe_layer_adj_is_noop (&a));
    a.key = (char *) "cover";
    CHECK (walpe_layer_adj_check (&a) != NULL);
    a.key = NULL;
    CHECK (walpe_layer_adj_check (&a) != NULL);
    CHECK (walpe_layer_adj_check (NULL) != NULL);

    int v = -1;
    CHECK (walpe_layer_vis_parse ("hidden", &v) && v == WALPE_LAYER_VIS_HIDDEN);
    CHECK (walpe_layer_vis_parse ("Shown", &v) && v == WALPE_LAYER_VIS_SHOWN);
    CHECK (walpe_layer_vis_parse ("default", &v) && v == WALPE_LAYER_VIS_DEFAULT);
    CHECK (walpe_layer_vis_parse ("unset", &v) && v == WALPE_LAYER_VIS_DEFAULT);
    CHECK (!walpe_layer_vis_parse ("maybe", &v) && !walpe_layer_vis_parse (NULL, &v));
    CHECK (!strcmp (walpe_layer_vis_name (WALPE_LAYER_VIS_SHOWN), "shown"));
    CHECK (!strcmp (walpe_layer_vis_name (WALPE_LAYER_VIS_HIDDEN), "hidden"));
    CHECK (!strcmp (walpe_layer_vis_name (0), "default"));
}

/* --------------------------------------------------------- 4. config */

static const WalpeLayerAdj *find_adj (GPtrArray *l, const char *key)
{
    for (guint i = 0; i < l->len; i++)
        if (!strcmp (((WalpeLayerAdj *) l->pdata[i])->key, key))
            return l->pdata[i];
    return NULL;
}

static void test_config (const char *tmp)
{
    const char *A = "/lib/431960/3151551777", *B = "/lib/other wallpaper";
    WalpeConfig *c = walpe_config_load ();   /* no file yet */
    CHECK (walpe_layers_count (c, A) == 0);
    GPtrArray *l = walpe_layers_get (c, A);
    CHECK (l && l->len == 0);
    g_ptr_array_unref (l);
    CHECK (walpe_layers_active (A) == NULL);

    const char *why = NULL;
    guint g0 = walpe_layers_generation ();
    WalpeLayerAdj a = { .key = (char *) "31:cover", .dx = 420.5, .dy = -310, .scale = 1.25 };
    CHECK (walpe_layers_put (c, A, &a, NULL, &why) && !why);
    CHECK (walpe_layers_generation () != g0);   /* running scenes look again */
    WalpeLayerAdj h = { .key = (char *) "-~2:Text \"3\"\\ \xc3\xa4", .scale = 1,
                        .vis = WALPE_LAYER_VIS_HIDDEN };
    CHECK (walpe_layers_put (c, A, &h, NULL, &why));
    WalpeLayerAdj s = { .key = (char *) "7:label", .scale = 1, .vis = WALPE_LAYER_VIS_SHOWN };
    CHECK (walpe_layers_put (c, B, &s, NULL, &why));
    /* refused: nothing is stored */
    WalpeLayerAdj bad = { .key = (char *) "9:x", .dx = NAN, .scale = 1 };
    CHECK (!walpe_layers_put (c, A, &bad, NULL, &why) && why);
    bad.dx = 0;
    bad.scale = 0;
    CHECK (!walpe_layers_put (c, A, &bad, NULL, &why));
    bad.scale = 1;
    bad.dy = 1e9;
    CHECK (!walpe_layers_put (c, A, &bad, NULL, &why));
    bad.dy = 5;
    bad.key = (char *) "no key";
    CHECK (!walpe_layers_put (c, A, &bad, NULL, &why));
    CHECK (!walpe_layers_put (c, "", &a, NULL, &why));
    CHECK (!walpe_layers_put (c, NULL, &a, NULL, &why));
    CHECK (walpe_layers_count (c, A) == 2 && walpe_layers_count (c, B) == 1);
    /* replace in place; a second spelling of the same layer collapses */
    a.dx = 100;
    CHECK (walpe_layers_put (c, A, &a, NULL, &why) && walpe_layers_count (c, A) == 2);
    WalpeLayerAdj re = { .key = (char *) "55:cover", .dx = 7, .scale = 1 };
    CHECK (walpe_layers_put (c, A, &re, "31:cover", &why) && walpe_layers_count (c, A) == 2);
    l = walpe_layers_get (c, A);
    CHECK (!find_adj (l, "31:cover") && find_adj (l, "55:cover") &&
           find_adj (l, "55:cover")->dx == 7);
    g_ptr_array_unref (l);
    re.key = (char *) "31:cover";
    re.dx = 420.5;
    re.dy = -310;
    re.scale = 1.25;
    CHECK (walpe_layers_put (c, A, &re, "55:cover", &why));
    /* the scene side sees the active config's list */
    l = walpe_layers_active (A);
    CHECK (l && l->len == 2);
    if (l)
        g_ptr_array_unref (l);
    walpe_config_save (c);
    walpe_config_free (c);
    CHECK (walpe_layers_active (A) == NULL);   /* no config, no adjustments */

    /* round trip through config.json */
    char *file = g_build_filename (tmp, "walpe", "config.json", NULL);
    char *text = NULL;
    CHECK (g_file_get_contents (file, &text, NULL, NULL) && strstr (text, "\"layers\""));
    g_free (text);
    c = walpe_config_load ();
    l = walpe_layers_get (c, A);
    CHECK (l->len == 2);
    const WalpeLayerAdj *r = find_adj (l, "31:cover");
    CHECK (r && r->dx == 420.5 && r->dy == -310 && r->scale == 1.25 &&
           r->vis == WALPE_LAYER_VIS_DEFAULT);
    r = find_adj (l, "-~2:Text \"3\"\\ \xc3\xa4");
    CHECK (r && r->dx == 0 && r->scale == 1 && r->vis == WALPE_LAYER_VIS_HIDDEN);
    g_ptr_array_unref (l);
    l = walpe_layers_get (c, B);
    CHECK (l->len == 1 && find_adj (l, "7:label") &&
           find_adj (l, "7:label")->vis == WALPE_LAYER_VIS_SHOWN);
    g_ptr_array_unref (l);
    /* the saved options are untouched by all this and survive too */
    walpe_config_set_prop (c, A, "speed", "3");
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    char *pv = walpe_config_get_prop (A, "speed");
    CHECK (!g_strcmp0 (pv, "3") && walpe_layers_count (c, A) == 2);
    g_free (pv);

    /* an adjustment that changes nothing is not stored; removing */
    WalpeLayerAdj noop = { .key = (char *) "31:cover", .scale = 1 };
    CHECK (walpe_layers_put (c, A, &noop, NULL, &why) && walpe_layers_count (c, A) == 1);
    CHECK (walpe_layers_remove (c, A, "nothing:here") == 0);
    CHECK (walpe_layers_remove (c, A, "-~2:Text \"3\"\\ \xc3\xa4") == 1);
    CHECK (walpe_layers_count (c, A) == 0);
    CHECK (walpe_layers_remove (c, A, NULL) == 0);
    /* "Reset all" = remove with no key; options stay */
    CHECK (walpe_layers_put (c, A, &a, NULL, &why) && walpe_layers_put (c, A, &h, NULL, &why));
    CHECK (walpe_layers_remove (c, A, NULL) == 2 && walpe_layers_count (c, A) == 0);
    pv = walpe_config_get_prop (A, "speed");
    CHECK (!g_strcmp0 (pv, "3"));
    g_free (pv);

    /* the delete flow forgets a wallpaper's adjustments with its options */
    CHECK (walpe_layers_put (c, A, &a, NULL, &why) && walpe_layers_put (c, A, &h, NULL, &why));
    guint n = walpe_config_forget_path (c, A);
    CHECK (n == 3);   /* 1 option + 2 layer adjustments */
    CHECK (walpe_layers_count (c, A) == 0 && walpe_layers_count (c, B) == 1);
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (walpe_layers_count (c, A) == 0 && walpe_layers_count (c, B) == 1);
    CHECK (walpe_layers_remove (c, B, NULL) == 1);
    walpe_config_save (c);
    walpe_config_free (c);
    CHECK (g_file_get_contents (file, &text, NULL, NULL) && !strstr (text, "\"layers\""));
    g_free (text);

    /* a config.json written by hand: bad entries are dropped, good ones kept */
    const char *hand =
        "{ \"config_version\": 2, \"layers\": {\n"
        "  \"/w/a\": [ {\"key\": \"1:ok\", \"dx\": 5, \"dy\": -6.5, \"scale\": 2, \"visible\": \"hidden\"},\n"
        "             {\"key\": \"2:huge\", \"dx\": 1e12},\n"
        "             {\"key\": \"3:zero\", \"scale\": 0},\n"
        "             {\"key\": \"nokey\", \"dx\": 1},\n"
        "             {\"dx\": 1},\n"
        "             {\"key\": \"4:vis\", \"visible\": \"sometimes\"},\n"
        "             {\"key\": \"5:str\", \"dx\": \"12\"},\n"
        "             {\"key\": \"1:ok\", \"dx\": 99},\n"
        "             \"junk\", 7,\n"
        "             {\"key\": \"6:int\", \"dx\": 3} ],\n"
        "  \"/w/b\": \"not a list\",\n"
        "  \"/w/c\": [ {\"key\": \"9:noop\"} ],\n"
        "  \"\": [ {\"key\": \"1:x\", \"dx\": 1} ] } }\n";
    CHECK (g_file_set_contents (file, hand, -1, NULL));
    c = walpe_config_load ();
    l = walpe_layers_get (c, "/w/a");
    CHECK (l->len == 2);
    r = find_adj (l, "1:ok");
    CHECK (r && r->dx == 5 && r->dy == -6.5 && r->scale == 2 && r->vis == WALPE_LAYER_VIS_HIDDEN);
    CHECK (!find_adj (l, "5:str"));   /* a string where a number belongs: nothing left to do */
    CHECK (find_adj (l, "6:int") && find_adj (l, "6:int")->dx == 3);
    CHECK (!find_adj (l, "2:huge") && !find_adj (l, "3:zero") && !find_adj (l, "4:vis"));
    g_ptr_array_unref (l);
    CHECK (walpe_layers_count (c, "/w/b") == 0 && walpe_layers_count (c, "/w/c") == 0 &&
           walpe_layers_count (c, "") == 0);
    walpe_config_free (c);
    g_unlink (file);
    g_free (file);

    /* highlight keys and layer lists (daemon <-> scene) */
    CHECK (!walpe_layers_any_highlight () && walpe_layers_highlight (A) == NULL);
    const char *hk[] = { "31:cover", "32:title" };
    walpe_layers_set_highlight (A, hk, 2);
    char **got = walpe_layers_highlight (A);
    CHECK (got && g_strv_length (got) == 2 && !strcmp (got[1], "32:title"));
    g_strfreev (got);
    CHECK (walpe_layers_any_highlight () && walpe_layers_highlight (B) == NULL);
    walpe_layers_set_highlight (A, NULL, 0);
    CHECK (!walpe_layers_any_highlight ());
    gboolean fresh = TRUE;
    CHECK (walpe_layers_published (A, 0, 0, &fresh) == NULL && !fresh);
    guint64 ser = walpe_layers_request_list ();
    CHECK (ser == walpe_layers_list_serial () && ser > 0);
    int owner1, owner2;
    /* nothing shows A yet (the scene is loading): no list and no waiting for one */
    gint64 t0 = g_get_monotonic_time ();
    CHECK (!walpe_layers_scene_alive (A));
    CHECK (walpe_layers_published (A, ser, 2000, &fresh) == NULL && !fresh);
    CHECK (g_get_monotonic_time () - t0 < 1000000);
    /* a scene's first frame: "I draw". A request now waits for its list */
    walpe_layers_publish (&owner1, A, 0, NULL);
    CHECK (walpe_layers_scene_alive (A) && !walpe_layers_scene_alive (B));
    t0 = g_get_monotonic_time ();
    CHECK (walpe_layers_published (A, ser, 60, &fresh) == NULL && !fresh);
    CHECK (g_get_monotonic_time () - t0 >= 50000);
    walpe_layers_publish (&owner1, A, ser - 1, g_strdup ("old"));
    walpe_layers_publish (&owner1, A, 0, NULL);       /* does not take the list back */
    char *p = walpe_layers_published (A, ser, 30, &fresh);
    CHECK (!g_strcmp0 (p, "old") && !fresh);          /* waited, got the last one */
    g_free (p);
    walpe_layers_publish (&owner2, A, ser, g_strdup ("new"));
    p = walpe_layers_published (A, ser, 0, &fresh);
    CHECK (!g_strcmp0 (p, "new") && fresh);
    g_free (p);
    walpe_layers_unpublish (&owner2);
    p = walpe_layers_published (A, 0, 0, &fresh);
    CHECK (!g_strcmp0 (p, "old"));
    g_free (p);
    walpe_layers_unpublish (&owner1);
    CHECK (walpe_layers_published (A, 0, 0, NULL) == NULL && !walpe_layers_scene_alive (A));
}

/* ------------------------------------------------------- 5. rendering */

static void put_file (const char *dir, const char *rel, const void *data, gssize len)
{
    char *p = g_build_filename (dir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0755);
    g_file_set_contents (p, data, len, NULL);
    g_free (d);
    g_free (p);
}

/* TEXV0005 container with one raw RGBA8888 image */
static void put_tex (const char *dir, const char *rel, int w, int h, guint32 rgba)
{
    GByteArray *o = g_byte_array_new ();
    guint32 hdr[7] = { 0, 0, (guint32) w, (guint32) h, (guint32) w, (guint32) h, 0 };
    guint32 n = (guint32) (w * h * 4);
    guint32 b1[2] = { 1, 0xffffffff }, mips = 1, m[5] = { (guint32) w, (guint32) h, 0, n, n };
    g_byte_array_append (o, (const guint8 *) "TEXV0005\0TEXI0001", 18);
    g_byte_array_append (o, (const guint8 *) hdr, sizeof hdr);
    g_byte_array_append (o, (const guint8 *) "TEXB0003", 9);
    g_byte_array_append (o, (const guint8 *) b1, sizeof b1);
    g_byte_array_append (o, (const guint8 *) &mips, 4);
    g_byte_array_append (o, (const guint8 *) m, sizeof m);
    for (guint32 i = 0; i < n / 4; i++)
        g_byte_array_append (o, (const guint8 *) &rgba, 4);
    put_file (dir, rel, o->data, o->len);
    g_byte_array_unref (o);
}

#define SOLID(id, name, origin, size, color, extra) \
    "{\"id\":" #id ",\"name\":\"" name "\",\"image\":\"models/util/solidlayer.json\"," \
    "\"origin\":" origin ",\"size\":\"" size "\",\"color\":\"" color "\",\"scale\":\"1 1 1\"," \
    "\"angles\":\"0 0 0\",\"alpha\":1.0,\"visible\":true" extra "}"

/* the test scene, 1920x1080 on black. camera: a camera layer zoomed out
 * 2:1 and moved */
static char *make_scene (const char *tmp, const char *name, gboolean camera)
{
    char *dir = g_build_filename (tmp, name, NULL);
    put_file (dir, "models/util/solidlayer.json", "{\"solidlayer\":true}", -1);
    put_file (dir, "project.json",
              "{\"file\":\"scene.json\",\"title\":\"layers test\",\"type\":\"scene\"}", -1);
    put_tex (dir, "materials/particle/sq.tex", 16, 16, 0xffffffff);
    put_file (dir, "materials/particle/sq.json",
              "{\"passes\":[{\"shader\":\"genericparticle\",\"blending\":\"translucent\","
              "\"depthtest\":\"disabled\",\"depthwrite\":\"disabled\",\"cullmode\":\"nocull\","
              "\"textures\":[\"particle/sq\"]}]}", -1);
    put_file (dir, "particles/one.json",
              "{\"emitter\":[{\"name\":\"boxrandom\",\"id\":1,\"distancemax\":\"0 0 0\","
              "\"instantaneous\":1,\"rate\":0}],"
              "\"initializer\":[{\"name\":\"lifetimerandom\",\"id\":2,\"min\":640,\"max\":640},"
              "{\"name\":\"sizerandom\",\"id\":3,\"min\":120,\"max\":120},"
              "{\"name\":\"alpharandom\",\"id\":4,\"min\":1,\"max\":1},"
              "{\"name\":\"colorrandom\",\"id\":5,\"min\":\"255 128 0\",\"max\":\"255 128 0\"}],"
              "\"operator\":[],\"renderer\":[{\"name\":\"sprite\",\"id\":6}],"
              "\"material\":\"materials/particle/sq.json\",\"maxcount\":1}", -1);
    GString *s = g_string_new ("{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
                               "\"general\":{\"clearcolor\":\"0 0 0\",\"orthogonalprojection\":"
                               "{\"height\":1080,\"width\":1920}},\"objects\":[");
    if (camera)
        g_string_append (s, "{\"id\":99,\"name\":\"cam\",\"camera\":\"default\",\"zoom\":0.5,"
                            "\"origin\":\"100 -60 0\"},");
    g_string_append (s,
        SOLID (40, "red", "\"400 300 0\"", "200 100", "1 0 0", "") ","
        SOLID (41, "green", "\"200 150 0\"", "100 100", "0 1 0", ",\"parent\":40") ","
        /* a script puts it somewhere else on every frame: 1000 + 8 * (n % 4), 700 */
        SOLID (42, "blue",
               "{\"script\":\"'use strict';\\nlet n = 0;\\nexport function update(value) {\\n"
               "  n++;\\n  return new Vec3(1000 + 8 * (n % 4), 700, 0);\\n}\\n\","
               "\"value\":\"100 100 0\"}", "80 80", "0 0 1", "") ","
        "{\"id\":43,\"name\":\"label\",\"text\":{\"value\":\"MOVE\"},\"font\":\"systemfont_arial\","
        "\"pointsize\":10,\"size\":\"300 80\",\"origin\":\"1400 300 0\",\"scale\":\"1 1 1\","
        "\"angles\":\"0 0 0\",\"color\":\"1 1 1\",\"alpha\":1.0,\"horizontalalign\":\"center\","
        "\"verticalalign\":\"center\",\"padding\":0,\"visible\":true},"
        SOLID (44, "yellow", "\"1600 800 0\"", "120 60", "1 1 0", ",\"visible\":false") ","
        /* a parent scaled 2x and turned 30 degrees, and its child */
        "{\"id\":45,\"name\":\"magenta\",\"image\":\"models/util/solidlayer.json\","
        "\"origin\":\"300 800 0\",\"size\":\"100 50\",\"color\":\"1 0 1\",\"scale\":\"2 2 1\","
        "\"angles\":\"0 0 0.5236\",\"alpha\":1.0,\"visible\":true},"
        SOLID (46, "cyan", "\"60 0 0\"", "40 40", "0 1 1", ",\"parent\":45") ","
        "{\"id\":47,\"name\":\"spark\",\"particle\":\"particles/one.json\","
        "\"origin\":\"1500 800 0\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\",\"visible\":true},"
        /* two objects without an id that share a name */
        "{\"name\":\"twin\",\"image\":\"models/util/solidlayer.json\",\"origin\":\"800 200 0\","
        "\"size\":\"60 60\",\"color\":\"0.5 0 1\",\"visible\":true},"
        "{\"name\":\"twin\",\"image\":\"models/util/solidlayer.json\",\"origin\":\"900 200 0\","
        "\"size\":\"60 60\",\"color\":\"1 0 0.5\",\"visible\":true}"
        "]}");
    put_file (dir, "scene.json", s->str, -1);
    g_string_free (s, TRUE);
    return dir;
}

typedef struct { int x0, y0, x1, y1, n; } Box;   /* x1 / y1 exclusive, GL rows (y up) */

/* pixels of colour r,g,b (+-6); r < 0: any grey that is not black (text) */
static Box box_of (const guint8 *px, int w, int h, int r, int g, int b)
{
    Box bx = { w, h, 0, 0, 0 };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const guint8 *p = px + ((gsize) y * w + x) * 4;
            gboolean hit = r < 0 ? (p[0] == p[1] && p[1] == p[2] && p[0] > 8)
                                 : (abs (p[0] - r) <= 6 && abs (p[1] - g) <= 6 && abs (p[2] - b) <= 6);
            if (!hit)
                continue;
            bx.x0 = MIN (bx.x0, x);
            bx.y0 = MIN (bx.y0, y);
            bx.x1 = MAX (bx.x1, x + 1);
            bx.y1 = MAX (bx.y1, y + 1);
            bx.n++;
        }
    return bx;
}

/* b is a moved by (dx, dy) pixels, same pixel count (tol 0) or within tol */
static gboolean moved (Box a, Box b, int dx, int dy, int tol)
{
    gboolean ok = a.n > 0 && b.n > 0 && abs (b.x0 - a.x0 - dx) <= tol &&
                  abs (b.x1 - a.x1 - dx) <= tol && abs (b.y0 - a.y0 - dy) <= tol &&
                  abs (b.y1 - a.y1 - dy) <= tol && (tol || a.n == b.n);
    if (!ok)
        printf ("    box %d,%d-%d,%d (%d px) -> %d,%d-%d,%d (%d px), expected moved by %d,%d\n",
                a.x0, a.y0, a.x1, a.y1, a.n, b.x0, b.y0, b.x1, b.y1, b.n, dx, dy);
    return ok;
}

static gboolean is_box (Box a, int x0, int y0, int x1, int y1, int tol)
{
    gboolean ok = a.n > 0 && abs (a.x0 - x0) <= tol && abs (a.y0 - y0) <= tol &&
                  abs (a.x1 - x1) <= tol && abs (a.y1 - y1) <= tol;
    if (!ok)
        printf ("    box %d,%d-%d,%d (%d px), expected %d,%d-%d,%d\n", a.x0, a.y0, a.x1, a.y1,
                a.n, x0, y0, x1, y1);
    return ok;
}

#define RED 255, 0, 0
#define GREEN 0, 255, 0
#define BLUE 0, 0, 255
#define YELLOW 255, 255, 0
#define MAGENTA 255, 0, 255
#define CYAN 0, 255, 255
#define ORANGE 255, 128, 0
#define TWIN1 128, 0, 255
#define TWIN2 255, 0, 128
#define TEXT -1, 0, 0

typedef struct {
    WalpeContent *c;
    int w, h;
    guint8 *px;      /* the frame read back */
} Shot;

static gboolean shot_open (Shot *s, const char *dir, int w, int h)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 30, .origin_dir = dir };
    memset (s, 0, sizeof *s);
    s->c = walpe_content_new (dir, &opts, &err);
    if (!s->c || !s->c->gl_init (s->c, &err)) {
        printf ("FAIL: scene %s: %s\n", dir, err ? err->message : "?");
        g_clear_error (&err);
        fails++;
        return FALSE;
    }
    s->w = w;
    s->h = h;
    s->px = g_malloc ((gsize) w * h * 4);
    return TRUE;
}

static const guint8 *shot_frame (Shot *s)
{
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, s->w, s->h);
    s->c->gl_render (s->c, s->w, s->h, 0.0f, 0.0f);
    glFinish ();
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, s->px);
    return s->px;
}

static void shot_close (Shot *s)
{
    if (s->c) {
        if (s->c->gl_free)
            s->c->gl_free (s->c);
        s->c->free (s->c);
    }
    g_free (s->px);
    memset (s, 0, sizeof *s);
}

static void adj (WalpeConfig *c, const char *path, const char *key, double dx, double dy,
                 double scale, int vis)
{
    WalpeLayerAdj a = { .key = (char *) key, .dx = dx, .dy = dy, .scale = scale, .vis = vis };
    const char *why = NULL;
    if (!walpe_layers_put (c, path, &a, NULL, &why)) {
        printf ("FAIL: put %s: %s\n", key, why ? why : "?");
        fails++;
    }
}

static void test_render_main (WalpeConfig *cfg, const char *dir)
{
    const int W = 1920, H = 1080;
    const gsize sz = (gsize) W * H * 4;
    Shot s;
    if (!shot_open (&s, dir, W, H))
        return;
    /* the blue block's script cycles over 4 frames: keep 4 reference frames */
    guint8 *ref[4];
    for (int f = 1; f <= 4; f++)
        shot_frame (&s);
    for (int j = 0; j < 4; j++)
        ref[j] = g_memdup2 (shot_frame (&s), sz);   /* frames 5..8 */
    Box red = box_of (ref[0], W, H, RED), green = box_of (ref[0], W, H, GREEN);
    Box text = box_of (ref[0], W, H, TEXT), mag = box_of (ref[0], W, H, MAGENTA);
    Box cyan = box_of (ref[0], W, H, CYAN), spark = box_of (ref[0], W, H, ORANGE);
    Box t1 = box_of (ref[0], W, H, TWIN1), t2 = box_of (ref[0], W, H, TWIN2);
    Box blue[4];
    for (int j = 0; j < 4; j++)
        blue[j] = box_of (ref[j], W, H, BLUE);
    /* the scene as written: scale 1:1, GL rows count from the bottom like
     * the scene's y */
    CHECK (is_box (red, 300, 250, 500, 350, 0) && red.n == 200 * 100);
    CHECK (is_box (green, 550, 400, 650, 500, 0));          /* 400+200, 300+150 */
    CHECK (box_of (ref[0], W, H, YELLOW).n == 0);           /* hidden by the scene */
    CHECK (is_box (t1, 770, 170, 830, 230, 0) && is_box (t2, 870, 170, 930, 230, 0));
    CHECK (mag.n > 0 && cyan.n > 0);
    gboolean have_text = text.n > 0, have_spark = spark.n > 0;
    if (!have_text)
        printf ("note: no text was drawn (no fonts?): text checks skipped\n");
    CHECK (have_spark);
#ifdef HAVE_QUICKJS
    /* the script really moves the block: 4 different places, none the
     * scene file's own (100,100) */
    CHECK (blue[0].n == 80 * 80 && blue[0].y0 == 660 && blue[0].x0 != blue[1].x0 &&
           blue[1].x0 != blue[2].x0 && abs (blue[0].x0 - 960) <= 24);
#else
    printf ("note: built without QuickJS: the script-driven case is a static layer here\n");
#endif

    /* ---- nothing that matches: the frames stay bit-identical ---- */
    adj (cfg, "/some/other/wallpaper", "40:red", 50, 50, 2, 0);
    adj (cfg, dir, "999:nothing", 50, 50, 2, WALPE_LAYER_VIS_HIDDEN);   /* no such layer */
    adj (cfg, dir, "999:twin", 50, 50, 1, 0);          /* id gone, name ambiguous: ignored */
    for (int j = 0; j < 4; j++)
        CHECK (!memcmp (shot_frame (&s), ref[j], sz));  /* frames 9..12 */
    CHECK (walpe_layers_count (cfg, dir) == 2);         /* ignored, but kept */

    /* ---- a plain layer: lands exactly dx,dy away, its child with it ---- */
    adj (cfg, dir, "40:red", 120, -80, 1, 0);
    const guint8 *f = shot_frame (&s);                  /* frame 13 = cycle 0 */
    CHECK (moved (red, box_of (f, W, H, RED), 120, -80, 0));
    CHECK (is_box (box_of (f, W, H, RED), 420, 170, 620, 270, 0));
    CHECK (moved (green, box_of (f, W, H, GREEN), 120, -80, 0));
    CHECK (moved (blue[0], box_of (f, W, H, BLUE), 0, 0, 0));
    CHECK (moved (mag, box_of (f, W, H, MAGENTA), 0, 0, 0));
    CHECK (moved (cyan, box_of (f, W, H, CYAN), 0, 0, 0));
    CHECK (moved (spark, box_of (f, W, H, ORANGE), 0, 0, 0));
    if (have_text)
        CHECK (moved (text, box_of (f, W, H, TEXT), 0, 0, 0));
    for (int j = 1; j < 4; j++)
        shot_frame (&s);                                /* 14..16 */

    /* ---- the child has its own offset on top of the parent's ---- */
    adj (cfg, dir, "41:green", -30, 500, 1, 0);
    f = shot_frame (&s);                                /* 17 = cycle 0 */
    CHECK (moved (green, box_of (f, W, H, GREEN), 120 - 30, -80 + 500, 0));
    CHECK (moved (red, box_of (f, W, H, RED), 120, -80, 0));
    for (int j = 1; j < 4; j++)
        shot_frame (&s);

    /* ---- script-driven origin: still offset, on every frame of its cycle ---- */
    adj (cfg, dir, "42:blue", -200, 100, 1, 0);
    for (int j = 0; j < 4; j++) {
        f = shot_frame (&s);                            /* 21..24 */
        CHECK (moved (blue[j], box_of (f, W, H, BLUE), -200, 100, 0));
    }

    /* ---- child of a scaled + rotated parent: scene pixels, not the
     *      parent's units; the parent itself: its child follows ---- */
    adj (cfg, dir, "46:cyan", 100, 40, 1, 0);
    f = shot_frame (&s);                                /* 25 = cycle 0 */
    CHECK (moved (cyan, box_of (f, W, H, CYAN), 100, 40, 0));
    /* (the child no longer covers part of its parent: only the outline is compared) */
    CHECK (is_box (box_of (f, W, H, MAGENTA), mag.x0, mag.y0, mag.x1, mag.y1, 0));
    for (int j = 1; j < 4; j++)
        shot_frame (&s);
    adj (cfg, dir, "45:magenta", 64, 16, 1, 0);
    f = shot_frame (&s);                                /* 29 */
    CHECK (is_box (box_of (f, W, H, MAGENTA), mag.x0 + 64, mag.y0 + 16, mag.x1 + 64, mag.y1 + 16, 0));
    CHECK (moved (cyan, box_of (f, W, H, CYAN), 164, 56, 0));
    for (int j = 1; j < 4; j++)
        shot_frame (&s);

    /* ---- text and particle layers ---- */
    adj (cfg, dir, "43:label", 64, -32, 1, 0);
    adj (cfg, dir, "47:spark", -300, -200, 1, 0);
    f = shot_frame (&s);                                /* 33 */
    if (have_text)
        CHECK (moved (text, box_of (f, W, H, TEXT), 64, -32, 0));
    CHECK (moved (spark, box_of (f, W, H, ORANGE), -300, -200, 0));
    for (int j = 1; j < 4; j++)
        shot_frame (&s);

    /* ---- keys without id: only the second "twin" ---- */
    adj (cfg, dir, "-~2:twin", 0, 300, 1, 0);
    f = shot_frame (&s);                                /* 37 */
    CHECK (moved (t1, box_of (f, W, H, TWIN1), 0, 0, 0));
    CHECK (moved (t2, box_of (f, W, H, TWIN2), 0, 300, 0));
    /* a key whose id is gone finds the layer by its (unique) name */
    CHECK (walpe_layers_remove (cfg, dir, "43:label") == 1);
    adj (cfg, dir, "777:label", -100, 100, 1, 0);
    f = shot_frame (&s);                                /* 38 */
    if (have_text)
        CHECK (moved (text, box_of (f, W, H, TEXT), -100, 100, 0));
    shot_frame (&s);
    shot_frame (&s);                                    /* 40 */

    /* ---- scale: about the layer's own origin, children scale with it ---- */
    adj (cfg, dir, "40:red", 120, -80, 2, 0);
    CHECK (walpe_layers_remove (cfg, dir, "41:green") == 1);
    f = shot_frame (&s);                                /* 41 */
    /* red: centre 520,220, now 400x200; green: centre 520+400, 220+300, 200x200 */
    CHECK (is_box (box_of (f, W, H, RED), 320, 120, 720, 320, 0));
    CHECK (is_box (box_of (f, W, H, GREEN), 820, 420, 1020, 620, 0));
    adj (cfg, dir, "47:spark", -300, -200, 2, 0);
    f = shot_frame (&s);                                /* 42 */
    {
        Box b = box_of (f, W, H, ORANGE);
        int w0 = spark.x1 - spark.x0, h0 = spark.y1 - spark.y0;
        int cx0 = spark.x0 + spark.x1 - 600, cy0 = spark.y0 + spark.y1 - 400;   /* 2 * centre */
        CHECK (b.n > 0 && abs (b.x1 - b.x0 - 2 * w0) <= 2 && abs (b.y1 - b.y0 - 2 * h0) <= 2);
        CHECK (abs (b.x0 + b.x1 - cx0) <= 2 && abs (b.y0 + b.y1 - cy0) <= 2);
    }
    adj (cfg, dir, "40:red", 0, 0, 0.5, 0);
    f = shot_frame (&s);                                /* 43 */
    CHECK (is_box (box_of (f, W, H, RED), 350, 275, 450, 325, 0));
    CHECK (is_box (box_of (f, W, H, GREEN), 475, 350, 525, 400, 0));   /* 400+100, 300+75 */
    shot_frame (&s);                                    /* 44 */

    /* ---- hide (children go too), show what the scene hides ---- */
    adj (cfg, dir, "40:red", 0, 0, 1, WALPE_LAYER_VIS_HIDDEN);
    adj (cfg, dir, "44:yellow", 0, 0, 1, WALPE_LAYER_VIS_SHOWN);
    f = shot_frame (&s);                                /* 45 */
    CHECK (box_of (f, W, H, RED).n == 0 && box_of (f, W, H, GREEN).n == 0);
    CHECK (is_box (box_of (f, W, H, YELLOW), 1540, 770, 1660, 830, 0));
    adj (cfg, dir, "44:yellow", -40, 0, 1, WALPE_LAYER_VIS_HIDDEN);
    adj (cfg, dir, "40:red", 10, 0, 1, WALPE_LAYER_VIS_SHOWN);   /* shown is its normal state */
    f = shot_frame (&s);                                /* 46 */
    CHECK (box_of (f, W, H, YELLOW).n == 0);
    CHECK (moved (red, box_of (f, W, H, RED), 10, 0, 0));
    shot_frame (&s);
    shot_frame (&s);                                    /* 48 */

    /* ---- "Reset all": bit for bit the scene's own picture again ---- */
    CHECK (walpe_layers_remove (cfg, dir, NULL) > 0);
    for (int j = 0; j < 4; j++)
        CHECK (!memcmp (shot_frame (&s), ref[j], sz));  /* 49..52 */

    for (int j = 0; j < 4; j++)
        g_free (ref[j]);
    shot_close (&s);
}

/* other monitor sizes: the offset is in scene pixels, so on screen it is
 * dx * (pixels per scene unit) of that monitor - also where the scene is
 * cropped (cover) - and through a camera layer */
static void test_render_view (WalpeConfig *cfg, const char *dir, int w, int h, double zoom)
{
    Shot s;
    if (!shot_open (&s, dir, w, h))
        return;
    for (int f = 0; f < 4; f++)
        shot_frame (&s);
    Box red0 = box_of (shot_frame (&s), w, h, RED);     /* frame 5 */
    Box cy0 = box_of (s.px, w, h, CYAN);
    double k = MAX (w / 1920.0, h / 1080.0) * zoom;
    adj (cfg, dir, "40:red", 300, 200, 1, 0);
    adj (cfg, dir, "46:cyan", 100, -50, 1, 0);
    for (int f = 0; f < 3; f++)
        shot_frame (&s);
    Box red1 = box_of (shot_frame (&s), w, h, RED);     /* frame 9 */
    Box cy1 = box_of (s.px, w, h, CYAN);
    printf ("view %dx%d zoom %.2f: %.3f px per scene unit\n", w, h, zoom, k);
    CHECK (moved (red0, red1, (int) lround (300 * k), (int) lround (200 * k), 1));
    CHECK (moved (cy0, cy1, (int) lround (100 * k), (int) lround (-50 * k), 1));
    CHECK (walpe_layers_remove (cfg, dir, NULL) == 2);
    shot_close (&s);
}

static gboolean test_render (const char *tmp)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLint maj, min, nc = 0;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd))
            for (EGLint i = 0; i < nd && dpy == EGL_NO_DISPLAY; i++) {
                EGLDisplay d = eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL);
                if (d != EGL_NO_DISPLAY && eglInitialize (d, &maj, &min))
                    dpy = d;
            }
    }
    if (dpy == EGL_NO_DISPLAY &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        EGLDisplay d = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA,
                                                 EGL_DEFAULT_DISPLAY, NULL);
        if (d != EGL_NO_DISPLAY && eglInitialize (d, &maj, &min))
            dpy = d;
    }
    EGLConfig ec;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &ec, 1, &nc) || nc < 1)
        return FALSE;
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, 1920, EGL_HEIGHT, 1200, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, ec, EGL_NO_CONTEXT, ca);
    EGLSurface surf = ctx != EGL_NO_CONTEXT ? eglCreatePbufferSurface (dpy, ec, pa) : EGL_NO_SURFACE;
    if (surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        eglTerminate (dpy);
        return FALSE;
    }
    printf ("GL: %s\n", (const char *) glGetString (GL_RENDERER));
    walpe_glutil_init ();
    WalpeConfig *cfg = walpe_config_load ();
    char *dir = make_scene (tmp, "scene", FALSE);
    char *cam = make_scene (tmp, "scene-cam", TRUE);
    test_render_main (cfg, dir);
    test_render_view (cfg, dir, 960, 540, 1.0);      /* half size */
    test_render_view (cfg, dir, 1280, 1024, 1.0);    /* 5:4 - the sides are cropped */
    test_render_view (cfg, dir, 1920, 1200, 1.0);    /* 16:10 - enlarged, cropped */
    test_render_view (cfg, cam, 1920, 1080, 0.5);    /* camera layer zoomed out */
    g_free (dir);
    g_free (cam);
    walpe_config_free (cfg);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return TRUE;
}

/* remove the temp dir (only what this test made lives in it) */
static void rm_tree (const char *path)
{
    if (!g_file_test (path, G_FILE_TEST_IS_SYMLINK) && g_file_test (path, G_FILE_TEST_IS_DIR)) {
        GDir *d = g_dir_open (path, 0, NULL);
        const char *n;
        while (d && (n = g_dir_read_name (d))) {
            char *c = g_build_filename (path, n, NULL);
            rm_tree (c);
            g_free (c);
        }
        if (d)
            g_dir_close (d);
        g_rmdir (path);
    } else {
        g_unlink (path);
    }
}

int main (void)
{
    char *tmp = g_dir_make_tmp ("walpe-layers-XXXXXX", NULL);
    if (!tmp) {
        printf ("no temp dir\n");
        return 1;
    }
    g_setenv ("HOME", tmp, TRUE);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
    g_unsetenv ("WALPE_PROP_PROBE");
    g_unsetenv ("WALPE_SKIP_IDS");
    test_keys ();
    test_tree ();
    test_valid ();
    test_config (tmp);
    int before = checks;
    if (!test_render (tmp))
        printf ("skip the rendering checks (no EGL device / surfaceless platform with a GL 3.3 "
                "pbuffer)\n");
    else
        printf ("rendering: %d checks\n", checks - before);
    rm_tree (tmp);
    g_free (tmp);
    printf ("layers: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
