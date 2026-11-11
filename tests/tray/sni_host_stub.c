/* sni_host_stub - a minimal StatusNotifierWatcher + host for testing walpe-tray
 *
 * Owns org.kde.StatusNotifierWatcher (IsStatusNotifierHostRegistered =
 * true), and for every item that registers: prints Title / ToolTip, saves
 * the largest IconPixmap as PNG, prints the dbusmenu tree (GetLayout),
 * optionally "clicks" a menu item by label (Event "clicked") or calls
 * Activate, then dumps everything again.
 *
 *   sni-host-stub [--png FILE] [--png-after FILE] [--click LABEL]
 *                 [--activate] [--seconds N]
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
static gboolean activate;
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

static void on_signal (GDBusConnection *c, const char *s, const char *p, const char *i, const char *sig,
                       GVariant *params, gpointer u)
{
    (void) c; (void) s; (void) u; (void) params;
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
    printf ("StatusNotifierWatcher stub up\n");
    fflush (stdout);
    g_main_loop_run (loop);
    return item_bus ? 0 : 3;
}
