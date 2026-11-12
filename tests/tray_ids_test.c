/* tray_ids_test.c - the ids of walpe-tray's menu items (src/tray/trayids.h).
 * No display, no bus.
 *
 * The menus here are the tray's own in the states that differ in their
 * entries: no engine ("Start engine" and a separator more, no "Restart
 * engine"), the engine running, a wallpaper more in the recent list, a
 * second monitor, the line for a monitor nobody was asked about. A panel
 * keeps what it made for an id (KDE Plasma: separator or not, check box or
 * not, submenu or not, are decided when the id is first seen), so across
 * all of them an id may only ever be one item. */
#include "tray/trayids.h"
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fails++;                                                          \
            fprintf (stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf (stderr, __VA_ARGS__);                                    \
            fputc ('\n', stderr);                                             \
        }                                                                     \
    } while (0)

static TrayItem *item (TrayItem *parent, TrayItemKind kind, const char *label, const char *action,
                       const char *arg, const char *name)
{
    TrayItem *it = g_new0 (TrayItem, 1);
    it->kind = kind;
    it->label = g_strdup (label);
    it->action = g_strdup (action);
    it->arg = g_strdup (arg);
    it->name = name;
    it->parent = parent;
    if (parent) {
        if (!parent->children)
            parent->children = g_ptr_array_new ();
        g_ptr_array_add (parent->children, it);
    }
    return it;
}

static void item_free (TrayItem *it)
{
    for (guint i = 0; it->children && i < it->children->len; i++)
        item_free (it->children->pdata[i]);
    if (it->children)
        g_ptr_array_free (it->children, TRUE);
    g_free (it->label);
    g_free (it->action);
    g_free (it->arg);
    g_free (it);
}

/* the tray's menu (src/tray/main.c build_menu), entry for entry */
static TrayItem *menu (gboolean alive, int recents, int monitors, gboolean new_monitor)
{
    static const char *const paths[] = { "/w/a", "/w/b", "/w/c", "/w/d" };
    TrayItem *root = item (NULL, TRAY_ITEM_SUBMENU, "WalPE", NULL, NULL, NULL);
    item (root, TRAY_ITEM_NORMAL, "Open WalPE", "open-gui", NULL, NULL);
    if (new_monitor)
        item (root, TRAY_ITEM_NORMAL, "Choose the render resolution of DP-1…", "resolution", "DP-1", NULL);
    item (root, TRAY_ITEM_SEPARATOR, "", NULL, NULL, NULL);
    if (!alive) {
        item (root, TRAY_ITEM_NORMAL, "Start engine", "start", NULL, NULL);
        item (root, TRAY_ITEM_SEPARATOR, "", NULL, NULL, NULL);
    }
    item (root, TRAY_ITEM_NORMAL, "Pause all wallpapers", "pause-toggle", NULL, NULL);
    item (root, TRAY_ITEM_NORMAL, "Mute", "mute-toggle", NULL, NULL);
    item (root, TRAY_ITEM_NORMAL, "Next wallpaper", "next", NULL, NULL);
    item (root, TRAY_ITEM_NORMAL, "Random wallpaper", "random", NULL, NULL);
    if (alive && monitors >= 2) {
        TrayItem *ms = item (root, TRAY_ITEM_SUBMENU, "Monitors", NULL, NULL, "monitors");
        for (int m = 0; m < monitors; m++) {
            TrayItem *sub = item (ms, TRAY_ITEM_SUBMENU, m ? "Monitor 2: a" : "Monitor 1: a", NULL, NULL, NULL);
            item (sub, TRAY_ITEM_INFO, "1920×1080 at 0,0", NULL, NULL, NULL);
            for (int r = 0; r < recents; r++) {
                char *arg = g_strdup_printf ("%d\t%s", m, paths[r]);
                item (sub, TRAY_ITEM_RADIO, paths[r], "open-mon", arg, NULL);
                g_free (arg);
            }
        }
    }
    if (recents) {
        TrayItem *rs = item (root, TRAY_ITEM_SUBMENU, "Recent wallpapers", NULL, NULL, "recent");
        for (int r = recents; r-- > 0;)     /* (the newest first) */
            item (rs, TRAY_ITEM_RADIO, paths[r], "open", paths[r], NULL);
    }
    item (root, TRAY_ITEM_SEPARATOR, "", NULL, NULL, NULL);
    item (root, TRAY_ITEM_CHECK, "Pause on fullscreen", "toggle-fs", NULL, NULL);
    item (root, TRAY_ITEM_CHECK, "Pause when covered", "toggle-covered", NULL, NULL);
    item (root, TRAY_ITEM_SEPARATOR, "", NULL, NULL, NULL);
    item (root, TRAY_ITEM_NORMAL, "Settings…", "settings", NULL, NULL);
    item (root, TRAY_ITEM_NORMAL, "Monitor resolution…", "resolution", NULL, NULL);
    if (alive)
        item (root, TRAY_ITEM_NORMAL, "Restart engine", "restart", NULL, NULL);
    item (root, TRAY_ITEM_SEPARATOR, "", NULL, NULL, NULL);
    item (root, TRAY_ITEM_NORMAL, "Quit WalPE", "quit", NULL, NULL);
    return root;
}

/* what an id has been so far: "<kind> <action> <arg> <name> under <parent id>" */
static GHashTable *seen;    /* id -> char* */
static GHashTable *in_tree; /* the ids of the tree being looked at */
static guint n_items;

static void look (TrayItem *it, const char *state)
{
    for (guint i = 0; it->children && i < it->children->len; i++) {
        TrayItem *c = it->children->pdata[i];
        n_items++;
        CHECK (c->id > 0, "%s: \"%s\" has the id %d", state, c->label, c->id);
        CHECK (g_hash_table_add (in_tree, GINT_TO_POINTER (c->id)), "%s: the id %d is there twice (\"%s\")",
               state, c->id, c->label);
        char *is = g_strdup_printf ("%d %s %s %s under %d", (int) c->kind, c->action ? c->action : "-",
                                    c->arg ? c->arg : "-", c->name ? c->name : "-", it->id);
        const char *was = g_hash_table_lookup (seen, GINT_TO_POINTER (c->id));
        CHECK (!was || !strcmp (was, is), "%s: the id %d was \"%s\" and is \"%s\" now", state, c->id, was, is);
        if (was)
            g_free (is);
        else
            g_hash_table_insert (seen, GINT_TO_POINTER (c->id), is);
        look (c, state);
    }
}

static int id_of (TrayItem *it, const char *action)
{
    for (guint i = 0; it->children && i < it->children->len; i++) {
        TrayItem *c = it->children->pdata[i];
        if (!g_strcmp0 (c->action, action))
            return c->id;
    }
    return -1;
}

static TrayItem *numbered (TrayIds *ids, gboolean alive, int recents, int monitors, gboolean new_monitor)
{
    TrayItem *m = menu (alive, recents, monitors, new_monitor);
    tray_ids_number (ids, m);
    char state[96];
    g_snprintf (state, sizeof state, "engine %s, %d recent, %d monitor(s)%s", alive ? "running" : "not running",
                recents, monitors, new_monitor ? ", a new monitor" : "");
    in_tree = g_hash_table_new (NULL, NULL);
    n_items = 0;
    CHECK (m->id == 0, "%s: the root has the id %d", state, m->id);
    look (m, state);
    CHECK (g_hash_table_size (ids->known) == n_items, "%s: %u ids are kept for %u items", state,
           g_hash_table_size (ids->known), n_items);
    g_hash_table_destroy (in_tree);
    return m;
}

int main (void)
{
    TrayIds ids = TRAY_IDS_INIT;
    seen = g_hash_table_new_full (NULL, NULL, NULL, g_free);

    /* ---- the report: the panel reads the menu while the tray knows no
     * engine, then the engine is there */
    TrayItem *a = numbered (&ids, FALSE, 1, 1, FALSE);
    int quit = id_of (a, "quit"), settings = id_of (a, "settings"), start = id_of (a, "start");
    CHECK (quit > 0 && settings > 0 && start > 0, "the menu has its entries");
    TrayItem *b = numbered (&ids, TRUE, 1, 1, FALSE);
    CHECK (id_of (b, "quit") == quit, "\"Quit WalPE\" is %d without an engine and %d with one", quit,
           id_of (b, "quit"));
    CHECK (id_of (b, "settings") == settings, "\"Settings…\" keeps its id");
    CHECK (id_of (b, "start") == -1 && id_of (b, "restart") > 0, "the running engine's entries");
    int restart = id_of (b, "restart");
    item_free (a);
    item_free (b);

    /* ---- and back, and again: what returns is not given an id that
     * belonged to something else meanwhile (look () holds every id to what
     * it was first) */
    a = numbered (&ids, FALSE, 1, 1, FALSE);
    CHECK (id_of (a, "quit") == quit, "\"Quit WalPE\" keeps its id when the engine goes");
    CHECK (id_of (a, "start") != restart, "\"Start engine\" does not take the id of \"Restart engine\"");
    item_free (a);

    /* ---- every other change of the entries, in both orders */
    static const struct { gboolean alive; int recents, monitors; gboolean new_monitor; } st[] = {
        { TRUE, 0, 1, FALSE }, { TRUE, 1, 1, FALSE }, { TRUE, 2, 1, FALSE }, { TRUE, 4, 1, FALSE },
        { TRUE, 2, 2, FALSE }, { TRUE, 2, 2, TRUE },  { FALSE, 2, 2, TRUE }, { TRUE, 3, 1, TRUE },
        { FALSE, 0, 1, FALSE }, { TRUE, 4, 2, FALSE }, { TRUE, 1, 1, FALSE },
    };
    for (int round = 0; round < 2; round++)
        for (guint i = 0; i < G_N_ELEMENTS (st); i++) {
            guint k = round ? G_N_ELEMENTS (st) - 1 - i : i;
            TrayItem *m = numbered (&ids, st[k].alive, st[k].recents, st[k].monitors, st[k].new_monitor);
            CHECK (id_of (m, "quit") == quit, "\"Quit WalPE\" keeps its id (state %u)", k);
            item_free (m);
        }

    /* ---- a tray that runs for weeks next to an engine that comes and
     * goes: the table stays as large as the menu (numbered () checks it)
     * and the same tree gets the same ids */
    for (int i = 0; i < 2000; i++) {
        TrayItem *m = menu (i & 1, 1 + (i % 3), 1, FALSE);
        tray_ids_number (&ids, m);
        CHECK (id_of (m, "quit") == quit, "\"Quit WalPE\" keeps its id (round %d)", i);
        item_free (m);
    }
    a = numbered (&ids, TRUE, 2, 2, FALSE);
    int pause = id_of (a, "pause-toggle");
    b = numbered (&ids, TRUE, 2, 2, FALSE);
    CHECK (id_of (b, "pause-toggle") == pause && id_of (b, "quit") == quit, "the same menu, the same ids");
    item_free (a);
    item_free (b);

    tray_ids_clear (&ids);
    g_hash_table_destroy (seen);
    if (fails) {
        fprintf (stderr, "tray-ids: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("tray-ids: ok\n");
    return 0;
}
