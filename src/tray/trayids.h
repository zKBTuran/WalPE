/* trayids.h - the ids of walpe-tray's menu items (main.c and
 * tests/tray_ids_test.c include it; no GTK, no bus).
 *
 * The menu tree is built anew whenever the state changes. Over
 * com.canonical.dbusmenu each item goes out under a number, and a panel
 * keeps what it made for a number: KDE Plasma's importer
 * (plasma-workspace, libdbusmenuqt/dbusmenuimporter.cpp) creates an entry
 * the first time it sees an id and from then on only takes over the
 * label, "enabled", the toggle state, the icon and "visible". Whether
 * the entry is a separator, has a check box or opens a submenu is decided
 * once, when it is made. An id therefore has to mean one item for as long
 * as it is in use, and must not come back as an item of another kind.
 *
 * The items used to be numbered by position, from 1, in every new tree.
 * One entry more or less near the top ("Start engine" and its separator
 * while the engine is not running, "Restart engine" while it is, a
 * wallpaper more in the recent list, the line for a new monitor) moved
 * every number after it to another item. On Plasma the labels then stood
 * on the entries made for the old layout: section headings "Mute" and
 * "Monitor resolution…" where separators had been, a check box in front
 * of "Settings…", entries twice - and "Quit WalPE" on what had been made
 * as the last separator, which a menu does not show, or twice, the first
 * of them an entry whose click the tray took for a click on a separator.
 *
 * Here an item is known by what it is: its parent, its kind, its action
 * and the action's argument, a name where those do not tell it from its
 * siblings (the submenus), and its place among the siblings that are the
 * same in all of that (the first separator, the second, ...). An item
 * that is known keeps its number; a new one gets a number that was never
 * used (they count up and are not given out again); what is not in the
 * new tree is forgotten, so the table is as large as the menu.
 */
#pragma once
#include "tray.h"

typedef struct {
    GHashTable *known;  /* char* key -> id, the items of the tree numbered last */
    int last;           /* the highest number given out */
} TrayIds;

#define TRAY_IDS_INIT { NULL, 0 }

static inline void tray_ids_children (TrayIds *t, GHashTable *now, TrayItem *parent)
{
    /* how many siblings with the same description came before */
    GHashTable *nth = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; parent->children && i < parent->children->len; i++) {
        TrayItem *c = parent->children->pdata[i];
        char *what = g_strdup_printf ("%d\x1f%d\x1f%s\x1f%s\x1f%s", parent->id, (int) c->kind,
                                      c->action ? c->action : "", c->arg ? c->arg : "",
                                      c->name ? c->name : "");
        guint n = GPOINTER_TO_UINT (g_hash_table_lookup (nth, what));
        char *key = g_strdup_printf ("%s\x1f%u", what, n);
        g_hash_table_insert (nth, what, GUINT_TO_POINTER (n + 1));
        gpointer had = t->known ? g_hash_table_lookup (t->known, key) : NULL;
        /* (a number is an int32 on the bus; counting up to its end takes a
         * menu that changes its items some hundred million times) */
        c->id = had ? GPOINTER_TO_INT (had) : t->last < G_MAXINT ? ++t->last : t->last;
        g_hash_table_insert (now, key, GINT_TO_POINTER (c->id));
        if (c->children)
            tray_ids_children (t, now, c);
    }
    g_hash_table_destroy (nth);
}

/* number a tree that is about to become the menu; the root is 0 */
static inline void tray_ids_number (TrayIds *t, TrayItem *root)
{
    GHashTable *now = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    root->id = 0;
    tray_ids_children (t, now, root);
    if (t->known)
        g_hash_table_destroy (t->known);
    t->known = now;
}

static inline void tray_ids_clear (TrayIds *t)
{
    if (t->known)
        g_hash_table_destroy (t->known);
    t->known = NULL;
}
