/* sni.c - StatusNotifierItem + com.canonical.dbusmenu (GDBus)
 *
 * For SNI hosts: KDE Plasma, waybar, xfce4-panel's SNI plugin, swaybar,
 * GNOME with the AppIndicator extension, snixembed, ...
 *   org.kde.StatusNotifierItem at /StatusNotifierItem: icon as ARGB
 *     pixmaps in several sizes (the state badge is part of the pixels),
 *     tooltip, Activate (left click: GUI), SecondaryActivate (middle
 *     click: pause/resume), ContextMenu (hosts that do not read dbusmenu:
 *     our own popup menu), ProvideXdgActivationToken (Wayland hosts call
 *     it right before Activate / a menu event: the token that lets the
 *     window we then raise or open come to the front)
 *   com.canonical.dbusmenu at /MenuBar: the same menu tree as the XEmbed
 *     popup, rendered by the host
 * Registered with org.kde.StatusNotifierWatcher, again whenever the
 * watcher (re)appears (panel restart).
 */
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>
#include "tray.h"
#include "traytip.h"

static const char sni_xml[] =
    "<node>"
    " <interface name='org.kde.StatusNotifierItem'>"
    "  <property name='Category' type='s' access='read'/>"
    "  <property name='Id' type='s' access='read'/>"
    "  <property name='Title' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='WindowId' type='i' access='read'/>"
    "  <property name='IconName' type='s' access='read'/>"
    "  <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='OverlayIconName' type='s' access='read'/>"
    "  <property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='AttentionIconName' type='s' access='read'/>"
    "  <property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='AttentionMovieName' type='s' access='read'/>"
    "  <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "  <property name='ItemIsMenu' type='b' access='read'/>"
    "  <property name='Menu' type='o' access='read'/>"
    "  <property name='IconThemePath' type='s' access='read'/>"
    "  <method name='ContextMenu'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "  <method name='Activate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "  <method name='SecondaryActivate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "  <method name='Scroll'><arg name='delta' type='i' direction='in'/><arg name='orientation' type='s' direction='in'/></method>"
    "  <method name='ProvideXdgActivationToken'><arg name='token' type='s' direction='in'/></method>"
    "  <signal name='NewTitle'/>"
    "  <signal name='NewIcon'/>"
    "  <signal name='NewAttentionIcon'/>"
    "  <signal name='NewOverlayIcon'/>"
    "  <signal name='NewToolTip'/>"
    "  <signal name='NewStatus'><arg name='status' type='s'/></signal>"
    " </interface>"
    "</node>";

static const char menu_xml[] =
    "<node>"
    " <interface name='com.canonical.dbusmenu'>"
    "  <property name='Version' type='u' access='read'/>"
    "  <property name='TextDirection' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='IconThemePath' type='as' access='read'/>"
    "  <method name='GetLayout'>"
    "   <arg type='i' name='parentId' direction='in'/>"
    "   <arg type='i' name='recursionDepth' direction='in'/>"
    "   <arg type='as' name='propertyNames' direction='in'/>"
    "   <arg type='u' name='revision' direction='out'/>"
    "   <arg type='(ia{sv}av)' name='layout' direction='out'/>"
    "  </method>"
    "  <method name='GetGroupProperties'>"
    "   <arg type='ai' name='ids' direction='in'/>"
    "   <arg type='as' name='propertyNames' direction='in'/>"
    "   <arg type='a(ia{sv})' name='properties' direction='out'/>"
    "  </method>"
    "  <method name='GetProperty'>"
    "   <arg type='i' name='id' direction='in'/>"
    "   <arg type='s' name='name' direction='in'/>"
    "   <arg type='v' name='value' direction='out'/>"
    "  </method>"
    "  <method name='Event'>"
    "   <arg type='i' name='id' direction='in'/>"
    "   <arg type='s' name='eventId' direction='in'/>"
    "   <arg type='v' name='data' direction='in'/>"
    "   <arg type='u' name='timestamp' direction='in'/>"
    "  </method>"
    "  <method name='EventGroup'>"
    "   <arg type='a(isvu)' name='events' direction='in'/>"
    "   <arg type='ai' name='idErrors' direction='out'/>"
    "  </method>"
    "  <method name='AboutToShow'>"
    "   <arg type='i' name='id' direction='in'/>"
    "   <arg type='b' name='needUpdate' direction='out'/>"
    "  </method>"
    "  <method name='AboutToShowGroup'>"
    "   <arg type='ai' name='ids' direction='in'/>"
    "   <arg type='ai' name='updatesNeeded' direction='out'/>"
    "   <arg type='ai' name='idErrors' direction='out'/>"
    "  </method>"
    "  <signal name='ItemsPropertiesUpdated'>"
    "   <arg type='a(ia{sv})' name='updatedProps'/>"
    "   <arg type='a(ias)' name='removedProps'/>"
    "  </signal>"
    "  <signal name='LayoutUpdated'>"
    "   <arg type='u' name='revision'/>"
    "   <arg type='i' name='parent'/>"
    "  </signal>"
    "  <signal name='ItemActivationRequested'>"
    "   <arg type='i' name='id'/>"
    "   <arg type='u' name='timestamp'/>"
    "  </signal>"
    " </interface>"
    "</node>";

#define SNI_PATH  "/StatusNotifierItem"
#define MENU_PATH "/MenuBar"

static struct {
    GDBusConnection *bus;
    GDBusNodeInfo *sni_info, *menu_info;
    guint sni_reg, menu_reg, own_id, watch_id;
    char *name;
    gboolean registered;
    gboolean watcher;       /* the StatusNotifierWatcher's name has an owner */
    guint retry_id, retry_n;
    guint menu_serial_sent;
    TrayIconState icon_sent;
    char *tip_sent;
} S;

/* ------------------------------------------------------------ pixmaps */

/* SNI pixmaps: ARGB32, network byte order, NOT premultiplied */
static GVariant *pixmap_variant (int size)
{
    cairo_surface_t *s = tray_icon_render (size, tray_icon_state ());
    unsigned char *d = cairo_image_surface_get_data (s);
    int stride = cairo_image_surface_get_stride (s);
    guint8 *out = g_malloc ((gsize) size * size * 4);
    for (int y = 0; y < size; y++) {
        guint32 *row = (guint32 *) (d + y * stride);
        for (int x = 0; x < size; x++) {
            guint32 v = row[x];
            guint a = v >> 24, r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
            if (a && a < 255) {
                r = MIN (255u, r * 255 / a);
                g = MIN (255u, g * 255 / a);
                b = MIN (255u, b * 255 / a);
            }
            guint8 *o = out + ((gsize) y * size + x) * 4;
            o[0] = a;
            o[1] = r;
            o[2] = g;
            o[3] = b;
        }
    }
    cairo_surface_destroy (s);
    GVariant *bytes = g_variant_new_from_data (G_VARIANT_TYPE ("ay"), out, (gsize) size * size * 4, TRUE,
                                               g_free, out);
    return g_variant_new ("(ii@ay)", size, size, bytes);
}

static GVariant *pixmaps (void)
{
    static const int sizes[] = { 16, 22, 24, 32, 48, 64, 128 };
    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("a(iiay)"));
    for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
        g_variant_builder_add_value (&b, pixmap_variant (sizes[i]));
    return g_variant_builder_end (&b);
}

static GVariant *empty_pixmaps (void)
{
    return g_variant_new_array (G_VARIANT_TYPE ("(iiay)"), NULL, 0);
}

/* ------------------------------------------------------------ SNI */

static GVariant *sni_get (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                          const char *prop, GError **err, gpointer u)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) err; (void) u;
    if (!strcmp (prop, "Category"))
        return g_variant_new_string ("ApplicationStatus");
    if (!strcmp (prop, "Id"))
        return g_variant_new_string ("walpe");
    if (!strcmp (prop, "Title"))
        return g_variant_new_string ("WalPE");
    if (!strcmp (prop, "Status"))
        return g_variant_new_string ("Active");
    if (!strcmp (prop, "WindowId"))
        return g_variant_new_int32 (0);
    if (!strcmp (prop, "IconName") || !strcmp (prop, "OverlayIconName") ||
        !strcmp (prop, "AttentionIconName") || !strcmp (prop, "AttentionMovieName") ||
        !strcmp (prop, "IconThemePath"))
        return g_variant_new_string ("");
    if (!strcmp (prop, "IconPixmap"))
        return pixmaps ();
    if (!strcmp (prop, "OverlayIconPixmap") || !strcmp (prop, "AttentionIconPixmap"))
        return empty_pixmaps ();
    if (!strcmp (prop, "ToolTip")) {
        /* (a host may read the text as markup: traytip.h) */
        char *title = tray_tip_for_host (tray_tooltip_title ()), *text = tray_tip_for_host (tray_tooltip_text ());
        GVariant *v = g_variant_new ("(s@a(iiay)ss)", "", empty_pixmaps (), title, text);
        g_free (title);
        g_free (text);
        return v;
    }
    if (!strcmp (prop, "ItemIsMenu"))
        return g_variant_new_boolean (FALSE);
    if (!strcmp (prop, "Menu"))
        return g_variant_new_object_path (MENU_PATH);
    return NULL;
}

static gboolean idle_popup (gpointer p)
{
    int *xy = p;
    tray_popup_show (xy[0], xy[1]);
    g_free (xy);
    return G_SOURCE_REMOVE;
}

static void sni_call (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer u)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) u;
    int x = -1, y = -1;
    if (!strcmp (method, "Activate")) {
        g_message ("sni: Activate");
        tray_open_gui ();
    } else if (!strcmp (method, "SecondaryActivate")) {
        g_message ("sni: SecondaryActivate (pause/resume)");
        tray_dispatch ("pause-toggle", NULL);
    } else if (!strcmp (method, "ContextMenu")) {
        g_variant_get (params, "(ii)", &x, &y);
        g_message ("sni: ContextMenu at %d,%d (host without dbusmenu)", x, y);
        int *xy = g_new (int, 2);
        xy[0] = x;
        xy[1] = y;
        g_idle_add (idle_popup, xy);
    } else if (!strcmp (method, "ProvideXdgActivationToken")) {
        const char *token = NULL;
        g_variant_get (params, "(&s)", &token);
        tray_set_activation_token (token);
    } else if (!strcmp (method, "Scroll")) {
        /* nothing bound (WE has no scroll action either) */
    } else {
        g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", method);
        return;
    }
    g_dbus_method_invocation_return_value (inv, NULL);
}

static const GDBusInterfaceVTable sni_vt = { sni_call, sni_get, NULL, { 0 } };

/* ------------------------------------------------------------ dbusmenu */

static char *mnemonic_escape (const char *s)
{
    GString *o = g_string_new (NULL);
    for (; *s; s++) {
        if (*s == '_')
            g_string_append_c (o, '_');
        g_string_append_c (o, *s);
    }
    return g_string_free (o, FALSE);
}

static gboolean want (const char *const *names, const char *n)
{
    return !names || !names[0] || g_strv_contains (names, n);
}

static GVariant *item_props (TrayItem *it, const char *const *names)
{
    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
    if (it->id == 0) {
        if (want (names, "children-display"))
            g_variant_builder_add (&b, "{sv}", "children-display", g_variant_new_string ("submenu"));
        return g_variant_builder_end (&b);
    }
    if (it->kind == TRAY_ITEM_SEPARATOR) {
        if (want (names, "type"))
            g_variant_builder_add (&b, "{sv}", "type", g_variant_new_string ("separator"));
    } else {
        if (want (names, "label")) {
            char *l = mnemonic_escape (it->label);
            g_variant_builder_add (&b, "{sv}", "label", g_variant_new_string (l));
            g_free (l);
        }
        if (want (names, "enabled"))
            g_variant_builder_add (&b, "{sv}", "enabled", g_variant_new_boolean (it->enabled));
        if (it->kind == TRAY_ITEM_CHECK || it->kind == TRAY_ITEM_RADIO) {
            if (want (names, "toggle-type"))
                g_variant_builder_add (&b, "{sv}", "toggle-type",
                                       g_variant_new_string (it->kind == TRAY_ITEM_CHECK ? "checkmark" : "radio"));
            if (want (names, "toggle-state"))
                g_variant_builder_add (&b, "{sv}", "toggle-state", g_variant_new_int32 (it->checked ? 1 : 0));
        }
        if (it->kind == TRAY_ITEM_SUBMENU && want (names, "children-display"))
            g_variant_builder_add (&b, "{sv}", "children-display", g_variant_new_string ("submenu"));
        if (it->is_default && want (names, "icon-name"))
            g_variant_builder_add (&b, "{sv}", "icon-name", g_variant_new_string ("preferences-desktop-wallpaper"));
    }
    if (!it->visible && want (names, "visible"))
        g_variant_builder_add (&b, "{sv}", "visible", g_variant_new_boolean (FALSE));
    return g_variant_builder_end (&b);
}

static GVariant *layout (TrayItem *it, int depth, const char *const *names)
{
    GVariantBuilder ch;
    g_variant_builder_init (&ch, G_VARIANT_TYPE ("av"));
    if (depth != 0)
        for (guint i = 0; it->children && i < it->children->len; i++)
            g_variant_builder_add (&ch, "v", layout (it->children->pdata[i], depth < 0 ? -1 : depth - 1, names));
    return g_variant_new ("(i@a{sv}av)", it->id, item_props (it, names), &ch);
}

typedef struct { char *action, *arg; } Pending;

static gboolean run_pending (gpointer p)
{
    Pending *pd = p;
    tray_dispatch (pd->action, pd->arg);
    g_free (pd->action);
    g_free (pd->arg);
    g_free (pd);
    return G_SOURCE_REMOVE;
}

static gboolean menu_event (int id, const char *ev)
{
    TrayItem *it = tray_item_find (tray_menu_get (), id);
    if (!it)
        return FALSE;
    if (!strcmp (ev, "clicked") && it->action && it->enabled && it->kind != TRAY_ITEM_SUBMENU) {
        /* after the reply: the action may rebuild the tree */
        Pending *pd = g_new0 (Pending, 1);
        pd->action = g_strdup (it->action);
        pd->arg = g_strdup (it->arg);
        g_idle_add (run_pending, pd);
    }
    return TRUE;
}

/* AboutToShow: does the panel have to ask for this menu's entries again?
 * For a submenu, yes. LayoutUpdated goes out for the root (parent 0), and
 * a panel that keeps each menu by itself - KDE Plasma's importer asks for
 * one level at a time - then asks for the root's entries only: a submenu
 * it has opened once ("Recent wallpapers", a monitor's list) kept the
 * entries of that moment for as long as the tray ran. The root is not
 * asked for again here: LayoutUpdated has said when it changed. */
static gboolean ask_again (TrayItem *root, int id)
{
    TrayItem *it = id ? tray_item_find (root, id) : NULL;
    return it && it->kind == TRAY_ITEM_SUBMENU;
}

static void menu_call (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                       const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer u)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) u;
    TrayItem *root = tray_menu_get ();
    if (!strcmp (method, "GetLayout")) {
        int parent, depth;
        const char **names = NULL;
        g_variant_get (params, "(ii^a&s)", &parent, &depth, &names);
        TrayItem *it = tray_item_find (root, parent);
        if (!it) {
            g_free (names);
            g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                   "no item %d", parent);
            return;
        }
        GVariant *l = layout (it, depth, names);
        g_free (names);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(u@(ia{sv}av))", tray_menu_serial (), l));
    } else if (!strcmp (method, "GetGroupProperties")) {
        GVariantIter *ids;
        const char **names = NULL;
        g_variant_get (params, "(ai^a&s)", &ids, &names);
        GVariantBuilder b;
        g_variant_builder_init (&b, G_VARIANT_TYPE ("a(ia{sv})"));
        int id;
        while (g_variant_iter_next (ids, "i", &id)) {
            TrayItem *it = tray_item_find (root, id);
            if (it)
                g_variant_builder_add (&b, "(i@a{sv})", id, item_props (it, names));
        }
        g_variant_iter_free (ids);
        g_free (names);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(a(ia{sv}))", &b));
    } else if (!strcmp (method, "GetProperty")) {
        int id;
        const char *name;
        g_variant_get (params, "(i&s)", &id, &name);
        TrayItem *it = tray_item_find (root, id);
        GVariant *props = it ? item_props (it, NULL) : NULL;
        GVariant *v = props ? g_variant_lookup_value (props, name, NULL) : NULL;
        if (props)
            g_variant_unref (g_variant_ref_sink (props));
        if (!v) {
            g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                   "no property %s on %d", name, id);
            return;
        }
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(v)", v));
        g_variant_unref (v);
    } else if (!strcmp (method, "Event")) {
        int id;
        const char *ev;
        GVariant *data;
        guint32 ts;
        g_variant_get (params, "(i&svu)", &id, &ev, &data, &ts);
        g_variant_unref (data);
        if (strcmp (ev, "hovered"))
            g_message ("dbusmenu: %s on %d", ev, id);
        if (!menu_event (id, ev)) {
            g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "no item %d", id);
            return;
        }
        g_dbus_method_invocation_return_value (inv, NULL);
    } else if (!strcmp (method, "EventGroup")) {
        GVariantIter *it;
        g_variant_get (params, "(a(isvu))", &it);
        GVariantBuilder errs;
        g_variant_builder_init (&errs, G_VARIANT_TYPE ("ai"));
        int id;
        const char *ev;
        GVariant *data;
        guint32 ts;
        while (g_variant_iter_next (it, "(i&svu)", &id, &ev, &data, &ts)) {
            if (!menu_event (id, ev))
                g_variant_builder_add (&errs, "i", id);
            g_variant_unref (data);
        }
        g_variant_iter_free (it);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(ai)", &errs));
    } else if (!strcmp (method, "AboutToShow")) {
        int id;
        g_variant_get (params, "(i)", &id);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(b)", ask_again (root, id)));
    } else if (!strcmp (method, "AboutToShowGroup")) {
        GVariantIter *ids;
        g_variant_get (params, "(ai)", &ids);
        GVariantBuilder need;
        g_variant_builder_init (&need, G_VARIANT_TYPE ("ai"));
        int id;
        while (g_variant_iter_next (ids, "i", &id))
            if (ask_again (root, id))
                g_variant_builder_add (&need, "i", id);
        g_variant_iter_free (ids);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(ai@ai)", &need,
            g_variant_new_array (G_VARIANT_TYPE_INT32, NULL, 0)));
    } else {
        g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", method);
    }
}

static GVariant *menu_get (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                           const char *prop, GError **err, gpointer u)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) err; (void) u;
    if (!strcmp (prop, "Version"))
        return g_variant_new_uint32 (3);
    if (!strcmp (prop, "TextDirection"))
        return g_variant_new_string ("ltr");
    if (!strcmp (prop, "Status"))
        return g_variant_new_string ("normal");
    if (!strcmp (prop, "IconThemePath"))
        return g_variant_new_strv (NULL, 0);
    return NULL;
}

static const GDBusInterfaceVTable menu_vt = { menu_call, menu_get, NULL, { 0 } };

/* ------------------------------------------------------------ watcher */

static void register_item (void);

static gboolean register_again (gpointer u)
{
    (void) u;
    S.retry_id = 0;
    if (S.watcher && !S.registered)
        register_item ();
    return G_SOURCE_REMOVE;
}

static void retry_cancel (void)
{
    if (S.retry_id)
        g_source_remove (S.retry_id);
    S.retry_id = 0;
    S.retry_n = 0;
}

static void on_registered (GObject *src, GAsyncResult *res, gpointer u)
{
    (void) u;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &e);
    if (!S.bus) {           /* the backend was stopped meanwhile */
        if (r)
            g_variant_unref (r);
        g_clear_error (&e);
        return;
    }
    if (r) {
        S.registered = TRUE;
        retry_cancel ();
        g_message ("sni: registered with the StatusNotifierWatcher as %s", S.name);
        g_variant_unref (r);
        return;
    }
    /* A watcher that is there and does not answer in time - the session is
     * still starting, or its process is held up - was asked this once, and
     * the tray stayed without an icon until the watcher's name changed its
     * owner. It is asked again: after 2, 5, 15 and 30 seconds, then once a
     * minute, for as long as the watcher is there and the item is not
     * registered (registering twice does no harm: a watcher knows an item by
     * its name). Nothing runs once the answer has come. */
    gboolean ask = S.watcher && !S.retry_id &&
                   !g_error_matches (e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) &&
                   !g_error_matches (e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE) &&
                   !g_error_matches (e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT);
    static const guint secs[] = { 2, 5, 15, 30, 60 };
    guint in = secs[MIN (S.retry_n, G_N_ELEMENTS (secs) - 1)];
    if (ask) {
        g_message ("sni: RegisterStatusNotifierItem failed: %s; asking again in %u s", e->message, in);
        S.retry_n++;
        S.retry_id = g_timeout_add_seconds (in, register_again, NULL);
    } else {
        g_message ("sni: RegisterStatusNotifierItem failed: %s", e->message);
    }
    g_error_free (e);
}

static void register_item (void)
{
    if (!S.bus || !S.name)
        return;
    g_dbus_connection_call (S.bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                            "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                            g_variant_new ("(s)", S.name), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000,
                            NULL, on_registered, NULL);
}

static void watcher_up (GDBusConnection *c, const char *name, const char *owner, gpointer u)
{
    (void) c; (void) name; (void) owner; (void) u;
    S.watcher = TRUE;
    retry_cancel ();
    register_item ();
}

static void watcher_down (GDBusConnection *c, const char *name, gpointer u)
{
    (void) c; (void) name; (void) u;
    if (S.registered)
        g_message ("sni: the StatusNotifierWatcher went away; re-registering when it is back");
    S.registered = FALSE;
    S.watcher = FALSE;
    retry_cancel ();
}

static void name_acquired (GDBusConnection *c, const char *name, gpointer u)
{
    (void) c; (void) name; (void) u;
    if (!S.watch_id)
        S.watch_id = g_bus_watch_name_on_connection (S.bus, "org.kde.StatusNotifierWatcher",
                                                     G_BUS_NAME_WATCHER_FLAGS_NONE, watcher_up,
                                                     watcher_down, NULL, NULL);
}

/* ------------------------------------------------------------ backend */

static gboolean sni_start (void)
{
    GApplication *app = g_application_get_default ();
    S.bus = app ? g_application_get_dbus_connection (app) : NULL;
    if (!S.bus) {
        g_message ("sni: no session bus");
        return FALSE;
    }
    g_object_ref (S.bus);
    if (!S.sni_info) {
        S.sni_info = g_dbus_node_info_new_for_xml (sni_xml, NULL);
        S.menu_info = g_dbus_node_info_new_for_xml (menu_xml, NULL);
    }
    GError *e = NULL;
    S.sni_reg = g_dbus_connection_register_object (S.bus, SNI_PATH, S.sni_info->interfaces[0], &sni_vt,
                                                   NULL, NULL, &e);
    if (S.sni_reg)
        S.menu_reg = g_dbus_connection_register_object (S.bus, MENU_PATH, S.menu_info->interfaces[0],
                                                        &menu_vt, NULL, NULL, &e);
    if (!S.sni_reg || !S.menu_reg) {
        g_message ("sni: cannot export objects: %s", e ? e->message : "?");
        g_clear_error (&e);
        if (S.sni_reg)
            g_dbus_connection_unregister_object (S.bus, S.sni_reg);
        S.sni_reg = 0;
        g_clear_object (&S.bus);
        return FALSE;
    }
    S.name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1", (int) getpid ());
    S.own_id = g_bus_own_name_on_connection (S.bus, S.name, G_BUS_NAME_OWNER_FLAGS_NONE, name_acquired,
                                             NULL, NULL, NULL);
    S.menu_serial_sent = tray_menu_serial ();
    S.icon_sent = tray_icon_state ();
    return TRUE;
}

static void sni_stop (void)
{
    if (!S.bus)
        return;
    if (S.watch_id)
        g_bus_unwatch_name (S.watch_id);
    if (S.own_id)
        g_bus_unown_name (S.own_id);
    if (S.sni_reg)
        g_dbus_connection_unregister_object (S.bus, S.sni_reg);
    if (S.menu_reg)
        g_dbus_connection_unregister_object (S.bus, S.menu_reg);
    S.watch_id = S.own_id = S.sni_reg = S.menu_reg = 0;
    S.registered = FALSE;
    S.watcher = FALSE;
    retry_cancel ();
    g_clear_pointer (&S.name, g_free);
    g_clear_pointer (&S.tip_sent, g_free);
    g_dbus_connection_flush_sync (S.bus, NULL, NULL);
    g_clear_object (&S.bus);
}

static void emit (const char *path, const char *iface, const char *sig, GVariant *params)
{
    g_dbus_connection_emit_signal (S.bus, NULL, path, iface, sig, params, NULL);
}

static void sni_update (void)
{
    if (!S.bus)
        return;
    if (S.icon_sent != tray_icon_state ()) {
        S.icon_sent = tray_icon_state ();
        emit (SNI_PATH, "org.kde.StatusNotifierItem", "NewIcon", NULL);
    }
    if (g_strcmp0 (S.tip_sent, tray_tooltip_text ())) {
        g_free (S.tip_sent);
        S.tip_sent = g_strdup (tray_tooltip_text ());
        emit (SNI_PATH, "org.kde.StatusNotifierItem", "NewToolTip", NULL);
    }
    if (S.menu_serial_sent != tray_menu_serial ()) {
        S.menu_serial_sent = tray_menu_serial ();
        emit (MENU_PATH, "com.canonical.dbusmenu", "LayoutUpdated",
              g_variant_new ("(ui)", tray_menu_serial (), 0));
    }
}

const TrayBackend tray_backend_sni = { "StatusNotifierItem", sni_start, sni_stop, sni_update };
