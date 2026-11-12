/* sni_host_stub - a minimal StatusNotifierWatcher + host for testing walpe-tray
 *
 * Owns org.kde.StatusNotifierWatcher (IsStatusNotifierHostRegistered =
 * true), and for every item that registers: prints Title / ToolTip, saves
 * the largest IconPixmap as PNG, prints the dbusmenu tree (GetLayout),
 * optionally "clicks" a menu item by label (Event "clicked") or calls
 * Activate, then dumps everything again.
 *
 *   sni-host-stub [--png FILE] [--png-after FILE] [--click LABEL]
 *                 [--activate] [--seconds N] [--follow] [--drop-register N]
 *
 * --drop-register N: the first N RegisterStatusNotifierItem calls are taken
 * and never answered (a watcher whose process is held up while the session
 * starts): the item has to ask again.
 *
 * --follow: keep the menu the way a panel does that imports it once and
 * then follows the item's LayoutUpdated signals with the rules of the
 * dbusmenu importer of the libdbusmenu-qt family (KDE Plasma's system
 * tray: plasma-workspace, libdbusmenuqt/dbusmenuimporter.cpp):
 *   - a menu is asked for one level at a time (GetLayout id, depth 1): the
 *     root at once, a submenu when it is opened and has no entries yet (or
 *     AboutToShow says it needs an update), any of them again when
 *     LayoutUpdated names it;
 *   - an entry whose id is known stays the entry it was made as. Only the
 *     properties the new layout carries for it are taken over, and never
 *     "type", "toggle-type" or "children-display": what was made a
 *     separator stays one, a check box stays a check box, a submenu keeps
 *     its submenu;
 *   - entries whose ids the new layout does not list are removed.
 * So an item must keep its id for as long as it is that item, and an id
 * must not come back as another kind of item. Commands, one a line on
 * standard input:
 *   dump          print the menu as kept, and whether a panel that imports
 *                 it only now - and opens the same submenus - would show
 *                 the same
 *   open LABEL    open the submenu with this label
 *   click LABEL   click the first entry with this label that can be clicked
 *   quit          end
 */
#include <gio/gio.h>
#include <cairo.h>
#include <string.h>
#include <stdio.h>

static const char xml[] =
    "<node><interface name='org.kde.StatusNotifierWatcher'>"
    " <method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>"
    " <method name='RegisterStatusNotifierHost'><arg type='s' direction='in'/></method>"
    " <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    " <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    " <property name='ProtocolVersion' type='i' access='read'/>"
    " <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    " <signal name='StatusNotifierHostRegistered'/>"
    "</interface></node>";

static char *png, *png_after, *click;
static gboolean activate, follow;
static int drop_register;   /* registrations that get no answer */
static int seconds = 8;
static GDBusConnection *bus;
static char *item_bus, *item_path;
static GMainLoop *loop;

static GVariant *iget (const char *prop)
{
    GVariant *r = g_dbus_connection_call_sync (bus, item_bus, item_path, "org.freedesktop.DBus.Properties",
                                               "Get", g_variant_new ("(ss)", "org.kde.StatusNotifierItem", prop),
                                               G_VARIANT_TYPE ("(v)"), 0, 2000, NULL, NULL);
    if (!r)
        return NULL;
    GVariant *v;
    g_variant_get (r, "(v)", &v);
    g_variant_unref (r);
    return v;
}

static void save_pixmap (const char *file)
{
    GVariant *pm = iget ("IconPixmap");
    if (!pm)
        return;
    GVariantIter it;
    g_variant_iter_init (&it, pm);
    int w, h, bw = 0;
    GVariant *bytes, *best = NULL;
    while (g_variant_iter_next (&it, "(ii@ay)", &w, &h, &bytes)) {
        printf ("  pixmap %dx%d\n", w, h);
        if (w > bw) {
            if (best)
                g_variant_unref (best);
            best = bytes;
            bw = w;
        } else {
            g_variant_unref (bytes);
        }
    }
    if (best && file) {
        gsize n;
        const guint8 *d = g_variant_get_fixed_array (best, &n, 1);
        cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, bw, bw);
        unsigned char *o = cairo_image_surface_get_data (s);
        int stride = cairo_image_surface_get_stride (s);
        for (int y = 0; y < bw; y++)
            for (int x = 0; x < bw; x++) {
                const guint8 *p = d + ((gsize) y * bw + x) * 4;
                guint a = p[0], r = p[1] * a / 255, g = p[2] * a / 255, b = p[3] * a / 255;
                ((guint32 *) (o + y * stride))[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        cairo_surface_mark_dirty (s);
        cairo_surface_write_to_png (s, file);
        cairo_surface_destroy (s);
        printf ("  saved %s\n", file);
    }
    if (best)
        g_variant_unref (best);
    g_variant_unref (pm);
}

static int find_id;

static void print_layout (GVariant *node, int depth)
{
    int id;
    GVariant *props, *children;
    g_variant_get (node, "(i@a{sv}@av)", &id, &props, &children);
    const char *label = "", *type = "", *tt = "";
    int ts = -1;
    gboolean enabled = TRUE, visible = TRUE;
    g_variant_lookup (props, "label", "&s", &label);
    g_variant_lookup (props, "type", "&s", &type);
    g_variant_lookup (props, "toggle-type", "&s", &tt);
    g_variant_lookup (props, "toggle-state", "i", &ts);
    g_variant_lookup (props, "enabled", "b", &enabled);
    g_variant_lookup (props, "visible", "b", &visible);
    if (depth > 0) {
        printf ("  %*s%s", (depth - 1) * 4, "", !strcmp (type, "separator") ? "----" : label);
        if (*tt)
            printf ("  [%s %s]", tt, ts == 1 ? "on" : "off");
        if (!enabled)
            printf ("  (disabled)");
        if (!visible)
            printf ("  (hidden)");
        if (g_variant_n_children (children))
            printf ("  >");
        printf ("   #%d\n", id);
        if (click && !find_id && !strcmp (label, click))
            find_id = id;
    }
    for (gsize i = 0; i < g_variant_n_children (children); i++) {
        GVariant *cv = g_variant_get_child_value (children, i);
        GVariant *c = g_variant_get_variant (cv);
        print_layout (c, depth + 1);
        g_variant_unref (c);
        g_variant_unref (cv);
    }
    g_variant_unref (props);
    g_variant_unref (children);
}

static void dump (const char *file)
{
    GVariant *t = iget ("Title");
    printf ("item %s%s: Title=%s\n", item_bus, item_path, t ? g_variant_get_string (t, NULL) : "?");
    if (t)
        g_variant_unref (t);
    GVariant *tip = iget ("ToolTip");
    if (tip) {
        const char *tt, *td;
        g_variant_get (tip, "(&s@a(iiay)&s&s)", NULL, NULL, &tt, &td);
        printf ("  tooltip: %s | %s\n", tt, td);
        g_variant_unref (tip);
    }
    save_pixmap (file);
    GVariant *menu = iget ("Menu");
    if (!menu)
        return;
    const char *mp = g_variant_get_string (menu, NULL);
    const char *none[] = { NULL };
    GVariant *r = g_dbus_connection_call_sync (bus, item_bus, mp, "com.canonical.dbusmenu", "GetLayout",
                                               g_variant_new ("(ii^as)", 0, -1, none), NULL, 0, 2000, NULL, NULL);
    if (r) {
        guint rev;
        GVariant *l;
        g_variant_get (r, "(u@(ia{sv}av))", &rev, &l);
        printf ("  menu (revision %u):\n", rev);
        find_id = 0;
        print_layout (l, 0);
        g_variant_unref (l);
        g_variant_unref (r);
    }
    g_variant_unref (menu);
}

static gboolean step2 (gpointer u)
{
    (void) u;
    printf ("--- after\n");
    dump (png_after);
    fflush (stdout);
    return G_SOURCE_REMOVE;
}

static gboolean step1 (gpointer u)
{
    (void) u;
    dump (png);
    if (click && find_id) {
        printf ("clicking \"%s\" (#%d)\n", click, find_id);
        GVariant *r = g_dbus_connection_call_sync (bus, item_bus, "/MenuBar", "com.canonical.dbusmenu", "Event",
                                                   g_variant_new ("(isvu)", find_id, "clicked",
                                                                  g_variant_new_int32 (0), 0),
                                                   NULL, 0, 2000, NULL, NULL);
        if (r)
            g_variant_unref (r);
    } else if (click) {
        printf ("no menu item \"%s\"\n", click);
    }
    if (activate) {
        printf ("calling Activate\n");
        GVariant *r = g_dbus_connection_call_sync (bus, item_bus, item_path, "org.kde.StatusNotifierItem",
                                                   "Activate", g_variant_new ("(ii)", 10, 10), NULL, 0, 2000,
                                                   NULL, NULL);
        if (r)
            g_variant_unref (r);
    }
    fflush (stdout);
    g_timeout_add (3000, step2, NULL);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------ --follow */

typedef struct Shown Shown;
struct Shown {
    int id;
    gboolean separator, checkable, radio;   /* as made: never changed */
    GPtrArray *menu;                        /* Shown*: its submenu, as made; NULL = none */
    char *label;
    gboolean enabled, visible, checked;
};

typedef struct {
    GPtrArray *root;        /* Shown* */
    GHashTable *by_id;      /* id -> Shown*, every menu */
} Panel;

static Panel kept;          /* what the panel shows */

static void panel_init (Panel *p)
{
    p->root = g_ptr_array_new ();
    p->by_id = g_hash_table_new (NULL, NULL);
}

static void shown_drop (Panel *p, Shown *e)
{
    for (guint i = 0; e->menu && i < e->menu->len; i++)
        shown_drop (p, e->menu->pdata[i]);
    if (e->menu)
        g_ptr_array_free (e->menu, TRUE);
    g_hash_table_remove (p->by_id, GINT_TO_POINTER (e->id));
    g_free (e->label);
    g_free (e);
}

static void panel_clear (Panel *p)
{
    while (p->root->len)
        shown_drop (p, g_ptr_array_steal_index (p->root, p->root->len - 1));
    g_ptr_array_free (p->root, TRUE);
    g_hash_table_destroy (p->by_id);
}

/* the menu an entry is in, NULL = none of them */
static GPtrArray *menu_holding (Panel *p, GPtrArray *menu, Shown *e)
{
    guint at;
    if (g_ptr_array_find (menu, e, &at))
        return menu;
    for (guint i = 0; i < menu->len; i++) {
        Shown *c = menu->pdata[i];
        GPtrArray *m = c->menu ? menu_holding (p, c->menu, e) : NULL;
        if (m)
            return m;
    }
    return NULL;
}

/* the properties a layout carries for an entry; every one of them optional */
static void shown_take (Shown *e, GVariant *props)
{
    const char *label;
    gboolean b;
    int ts;
    if (g_variant_lookup (props, "label", "&s", &label)) {
        g_free (e->label);
        e->label = g_strdup (label);
    }
    if (g_variant_lookup (props, "enabled", "b", &b))
        e->enabled = b;
    if (g_variant_lookup (props, "visible", "b", &b))
        e->visible = b;
    if (g_variant_lookup (props, "toggle-state", "i", &ts))
        e->checked = ts == 1;
}

/* one level of one menu, as the importer takes it. FALSE = no answer */
static gboolean panel_refresh (Panel *p, int parent)
{
    Shown *pe = parent ? g_hash_table_lookup (p->by_id, GINT_TO_POINTER (parent)) : NULL;
    GPtrArray *menu = parent ? (pe ? pe->menu : NULL) : p->root;
    if (!menu)
        return FALSE;       /* (the importer: "No menu for id") */
    const char *none[] = { NULL };
    GVariant *r = g_dbus_connection_call_sync (bus, item_bus, "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
                                               g_variant_new ("(ii^as)", parent, 1, none),
                                               G_VARIANT_TYPE ("(u(ia{sv}av))"), 0, 2000, NULL, NULL);
    if (!r)
        return FALSE;
    GVariant *node, *children;
    g_variant_get (r, "(u@(ia{sv}av))", NULL, &node);
    g_variant_get (node, "(i@a{sv}@av)", NULL, NULL, &children);
    gsize n = g_variant_n_children (children);
    GHashTable *listed = g_hash_table_new (NULL, NULL);
    GVariant **cs = g_new0 (GVariant *, n + 1);
    for (gsize i = 0; i < n; i++) {
        GVariant *cv = g_variant_get_child_value (children, i);
        cs[i] = g_variant_get_variant (cv);
        g_variant_unref (cv);
        int id;
        g_variant_get_child (cs[i], 0, "i", &id);
        g_hash_table_add (listed, GINT_TO_POINTER (id));
    }
    for (guint i = menu->len; i-- > 0;) {
        Shown *e = menu->pdata[i];
        if (!g_hash_table_contains (listed, GINT_TO_POINTER (e->id)))
            shown_drop (p, g_ptr_array_steal_index (menu, i));
    }
    for (gsize i = 0; i < n; i++) {
        int id;
        GVariant *props;
        g_variant_get (cs[i], "(i@a{sv}@av)", &id, &props, NULL);
        Shown *e = g_hash_table_lookup (p->by_id, GINT_TO_POINTER (id));
        if (!e) {
            const char *type = "", *tt = "", *cd = "";
            e = g_new0 (Shown, 1);
            e->id = id;
            e->label = g_strdup ("");
            e->enabled = e->visible = TRUE;
            g_variant_lookup (props, "type", "&s", &type);
            g_variant_lookup (props, "toggle-type", "&s", &tt);
            g_variant_lookup (props, "children-display", "&s", &cd);
            e->separator = !strcmp (type, "separator");
            e->checkable = *tt != 0;
            e->radio = !strcmp (tt, "radio");
            if (!strcmp (cd, "submenu"))
                e->menu = g_ptr_array_new ();
            g_hash_table_insert (p->by_id, GINT_TO_POINTER (id), e);
        } else {
            /* known: it is moved to the end of this menu, from wherever it is */
            GPtrArray *from = menu_holding (p, p->root, e);
            if (from)
                g_ptr_array_remove (from, e);
        }
        shown_take (e, props);
        g_ptr_array_add (menu, e);
        g_variant_unref (props);
        g_variant_unref (cs[i]);
    }
    g_free (cs);
    g_hash_table_destroy (listed);
    g_variant_unref (children);
    g_variant_unref (node);
    g_variant_unref (r);
    return TRUE;
}

static void panel_print (GString *o, GPtrArray *menu, int depth)
{
    for (guint i = 0; i < menu->len; i++) {
        Shown *e = menu->pdata[i];
        if (!e->visible)
            continue;
        g_string_append_printf (o, "  %*s", depth * 4, "");
        if (e->separator && *e->label)
            g_string_append_printf (o, "[heading] %s", e->label);   /* Qt: a section title, no entry */
        else if (e->separator)
            g_string_append (o, "[separator]");
        else
            g_string_append_printf (o, "[%s%s%s%s] %s", e->menu ? "submenu" : e->checkable ? (e->radio ? "radio" : "check") : "item",
                                    e->checkable ? (e->checked ? " on" : " off") : "", e->enabled ? "" : " disabled",
                                    e->checkable && e->menu ? " checkable" : "", e->label);
        g_string_append_printf (o, " #%d\n", e->id);
        if (e->menu && e->enabled)      /* (one that cannot be opened shows nothing of what it holds) */
            panel_print (o, e->menu, depth + 1);
    }
}

/* the submenus that were opened in `like`, opened in p too */
static void panel_open_like (Panel *p, GPtrArray *like)
{
    for (guint i = 0; i < like->len; i++) {
        Shown *e = like->pdata[i];
        if (e->menu && e->menu->len && panel_refresh (p, e->id))
            panel_open_like (p, e->menu);
    }
}

static void panel_dump (void)
{
    GString *a = g_string_new (NULL), *b = g_string_new (NULL);
    panel_print (a, kept.root, 0);
    Panel fresh;
    panel_init (&fresh);
    panel_refresh (&fresh, 0);
    panel_open_like (&fresh, kept.root);
    panel_print (b, fresh.root, 0);
    panel_clear (&fresh);
    printf ("menu as the panel shows it:\n%s", a->str);
    if (strcmp (a->str, b->str))
        printf ("menu DIFFERS from what a panel that imports it now shows:\n%s", b->str);
    else
        printf ("menu is what a panel that imports it now shows\n");
    printf ("menu end\n");
    fflush (stdout);
    g_string_free (a, TRUE);
    g_string_free (b, TRUE);
}

static Shown *panel_find (GPtrArray *menu, const char *label, gboolean submenu)
{
    for (guint i = 0; i < menu->len; i++) {
        Shown *e = menu->pdata[i];
        if (!e->visible || e->separator)
            continue;
        if (!strcmp (e->label, label) && !e->menu == !submenu && (submenu || e->enabled))
            return e;
        Shown *f = e->menu ? panel_find (e->menu, label, submenu) : NULL;
        if (f)
            return f;
    }
    return NULL;
}

static gboolean on_command (GIOChannel *ch, GIOCondition cond, gpointer u)
{
    (void) cond; (void) u;
    char *line = NULL;
    if (g_io_channel_read_line (ch, &line, NULL, NULL, NULL) != G_IO_STATUS_NORMAL || !line) {
        g_free (line);
        return G_SOURCE_REMOVE;
    }
    g_strchomp (line);
    if (!item_bus) {
        printf ("%s: no item has registered\n", line);
    } else if (!strcmp (line, "dump")) {
        panel_dump ();
    } else if (g_str_has_prefix (line, "open ")) {
        Shown *e = panel_find (kept.root, line + 5, TRUE);
        if (!e) {
            printf ("open: no submenu \"%s\"\n", line + 5);
        } else {
            gboolean need = FALSE;
            GVariant *r = g_dbus_connection_call_sync (bus, item_bus, "/MenuBar", "com.canonical.dbusmenu",
                                                       "AboutToShow", g_variant_new ("(i)", e->id),
                                                       G_VARIANT_TYPE ("(b)"), 0, 2000, NULL, NULL);
            if (r) {
                g_variant_get (r, "(b)", &need);
                g_variant_unref (r);
            }
            if (need || !e->menu->len)
                panel_refresh (&kept, e->id);
            printf ("open: \"%s\" #%d%s\n", e->label, e->id, need ? " (it said it needs an update)" : "");
        }
    } else if (g_str_has_prefix (line, "click ")) {
        Shown *e = panel_find (kept.root, line + 6, FALSE);
        if (!e) {
            printf ("click: no entry \"%s\" that can be clicked\n", line + 6);
        } else {
            GError *err = NULL;
            GVariant *r = g_dbus_connection_call_sync (bus, item_bus, "/MenuBar", "com.canonical.dbusmenu", "Event",
                                                       g_variant_new ("(isvu)", e->id, "clicked",
                                                                      g_variant_new_string (""), 0),
                                                       NULL, 0, 2000, NULL, &err);
            printf ("click: \"%s\" #%d%s%s\n", e->label, e->id, r ? "" : ": ", r ? "" : err->message);
            if (r)
                g_variant_unref (r);
            g_clear_error (&err);
        }
    } else if (!strcmp (line, "quit")) {
        g_main_loop_quit (loop);
    } else if (*line) {
        printf ("unknown command \"%s\"\n", line);
    }
    fflush (stdout);
    g_free (line);
    return G_SOURCE_CONTINUE;
}

static gboolean follow_first (gpointer u)
{
    (void) u;
    panel_clear (&kept);
    panel_init (&kept);
    printf ("import: %s\n", panel_refresh (&kept, 0) ? "done" : "no answer");
    fflush (stdout);
    return G_SOURCE_REMOVE;
}

static void on_signal (GDBusConnection *c, const char *s, const char *p, const char *i, const char *sig,
                       GVariant *params, gpointer u)
{
    (void) c; (void) u;
    if (follow && !strcmp (sig, "LayoutUpdated") && item_bus) {
        /* (only the item's own: the stub may see other menus on the bus) */
        guint rev;
        int parent;
        g_variant_get (params, "(ui)", &rev, &parent);
        gboolean ok = g_str_equal (s, item_bus) || !g_str_has_prefix (item_bus, ":");
        if (ok)
            printf ("LayoutUpdated revision %u parent %d: %s\n", rev, parent,
                    panel_refresh (&kept, parent) ? "taken" : "no such menu here");
        fflush (stdout);
        return;
    }
    printf ("signal %s.%s on %s\n", i, sig, p);
    fflush (stdout);
}

static void call (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                  const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer u)
{
    (void) c; (void) path; (void) iface; (void) u;
    if (!strcmp (method, "RegisterStatusNotifierItem")) {
        const char *svc;
        g_variant_get (params, "(&s)", &svc);
        if (drop_register > 0) {
            /* a watcher that is held up: the call is taken and never answered */
            drop_register--;
            printf ("RegisterStatusNotifierItem(%s) from %s: not answered\n", svc, sender);
            fflush (stdout);
            g_object_unref (inv);
            return;
        }
        g_free (item_bus);
        g_free (item_path);
        if (svc[0] == '/') {
            item_bus = g_strdup (sender);
            item_path = g_strdup (svc);
        } else {
            item_bus = g_strdup (svc);
            item_path = g_strdup ("/StatusNotifierItem");
        }
        printf ("RegisterStatusNotifierItem(%s) from %s\n", svc, sender);
        fflush (stdout);
        g_dbus_connection_emit_signal (bus, NULL, "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                                       "StatusNotifierItemRegistered", g_variant_new ("(s)", svc), NULL);
        if (follow)
            g_idle_add (follow_first, NULL);    /* a panel imports the menu as soon as the item is there */
        else
            g_timeout_add (400, step1, NULL);
    }
    g_dbus_method_invocation_return_value (inv, NULL);
}

static GVariant *get (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *prop, GError **e, gpointer u)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) e; (void) u;
    if (!strcmp (prop, "IsStatusNotifierHostRegistered"))
        return g_variant_new_boolean (TRUE);
    if (!strcmp (prop, "ProtocolVersion"))
        return g_variant_new_int32 (0);
    const char *one[] = { item_bus, NULL };
    return g_variant_new_strv (item_bus ? one : NULL, item_bus ? 1 : 0);
}

static const GDBusInterfaceVTable vt = { call, get, NULL, { 0 } };

static gboolean quit_cb (gpointer u)
{
    (void) u;
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

int main (int argc, char **argv)
{
    GOptionEntry e[] = {
        { "png", 0, 0, G_OPTION_ARG_FILENAME, &png, "save the icon", "FILE" },
        { "png-after", 0, 0, G_OPTION_ARG_FILENAME, &png_after, "save the icon after the click", "FILE" },
        { "click", 0, 0, G_OPTION_ARG_STRING, &click, "menu item label to click", "LABEL" },
        { "activate", 0, 0, G_OPTION_ARG_NONE, &activate, "call Activate", NULL },
        { "seconds", 0, 0, G_OPTION_ARG_INT, &seconds, "run time", "N" },
        { "follow", 0, 0, G_OPTION_ARG_NONE, &follow, "keep the menu as a panel does; commands on standard input", NULL },
        { "drop-register", 0, 0, G_OPTION_ARG_INT, &drop_register, "leave the first N registrations of an item unanswered", "N" },
        { NULL, 0, 0, 0, NULL, NULL, NULL },
    };
    GOptionContext *oc = g_option_context_new (NULL);
    g_option_context_add_main_entries (oc, e, NULL);
    if (!g_option_context_parse (oc, &argc, &argv, NULL))
        return 1;
    bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return 2;
    GDBusNodeInfo *ni = g_dbus_node_info_new_for_xml (xml, NULL);
    g_dbus_connection_register_object (bus, "/StatusNotifierWatcher", ni->interfaces[0], &vt, NULL, NULL, NULL);
    g_dbus_connection_signal_subscribe (bus, NULL, "org.kde.StatusNotifierItem", NULL, NULL, NULL, 0,
                                        on_signal, NULL, NULL);
    g_dbus_connection_signal_subscribe (bus, NULL, "com.canonical.dbusmenu", "LayoutUpdated", NULL, NULL, 0,
                                        on_signal, NULL, NULL);
    g_bus_own_name_on_connection (bus, "org.kde.StatusNotifierWatcher", 0, NULL, NULL, NULL, NULL);
    loop = g_main_loop_new (NULL, FALSE);
    g_timeout_add_seconds (seconds, quit_cb, NULL);
    if (follow) {
        panel_init (&kept);
        GIOChannel *in = g_io_channel_unix_new (0);
        g_io_add_watch (in, G_IO_IN | G_IO_HUP, on_command, NULL);
        g_io_channel_unref (in);
    }
    printf ("StatusNotifierWatcher stub up\n");
    fflush (stdout);
    g_main_loop_run (loop);
    return item_bus ? 0 : 3;
}
