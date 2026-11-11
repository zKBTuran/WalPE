/* appearance.c - dark or light, as the desktop prefers (Wayland sessions).
 *
 * On X11 a GTK program learns the theme from the XSETTINGS manager or
 * from settings.ini; neither says anything on Wayland. There the
 * preference is published by the desktop portal: interface
 * org.freedesktop.portal.Settings, namespace org.freedesktop.appearance,
 * key color-scheme (0 no preference, 1 dark, 2 light), with a
 * SettingChanged signal. GTK itself follows it from 4.20 on; libadwaita
 * programs always did. For the GTK versions before that this file reads
 * the key once (asynchronously: the window does not wait for the bus) and
 * keeps gtk-application-prefer-dark-theme in step with it.
 *
 * The X11 session is left exactly as it was (walpe_appearance_wanted). */
#include "appearance.h"
#include <gtk/gtk.h>

#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SETTINGS "org.freedesktop.portal.Settings"
#define APPEARANCE_NS "org.freedesktop.appearance"

static struct {
    GDBusConnection *bus;
    guint sub;
    gboolean base;          /* what GTK had before the portal was asked */
    gboolean have_base;
} A;

static void apply (guint32 scheme)
{
    GtkSettings *gs = gtk_settings_get_default ();
    if (!gs)
        return;
    if (!A.have_base) {
        g_object_get (gs, "gtk-application-prefer-dark-theme", &A.base, NULL);
        A.have_base = TRUE;
    }
    gboolean dark = walpe_appearance_dark (scheme, A.base), now = FALSE;
    g_object_get (gs, "gtk-application-prefer-dark-theme", &now, NULL);
    if (now != dark)
        g_object_set (gs, "gtk-application-prefer-dark-theme", dark, NULL);
}

/* the value arrives as u, or (older portals, the Read method) wrapped in
 * one or two variants */
static gboolean scheme_of (GVariant *v, guint32 *out)
{
    GVariant *cur = g_variant_ref (v);
    while (g_variant_is_of_type (cur, G_VARIANT_TYPE_VARIANT)) {
        GVariant *in = g_variant_get_variant (cur);
        g_variant_unref (cur);
        cur = in;
    }
    gboolean ok = g_variant_is_of_type (cur, G_VARIANT_TYPE_UINT32);
    if (ok)
        *out = g_variant_get_uint32 (cur);
    g_variant_unref (cur);
    return ok;
}

static void on_read (GObject *src, GAsyncResult *res, gpointer user)
{
    gboolean second = GPOINTER_TO_INT (user);
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
    if (!r) {
        /* ReadOne exists from portal interface version 2; Read is its
         * older spelling */
        if (!second && A.bus)
            g_dbus_connection_call (A.bus, PORTAL_NAME, PORTAL_PATH, PORTAL_SETTINGS, "Read",
                                    g_variant_new ("(ss)", APPEARANCE_NS, "color-scheme"),
                                    G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, on_read,
                                    GINT_TO_POINTER (1));
        return;
    }
    GVariant *v = g_variant_get_child_value (r, 0);
    guint32 scheme = 0;
    if (scheme_of (v, &scheme))
        apply (scheme);
    g_variant_unref (v);
    g_variant_unref (r);
}

static void on_changed (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *sig, GVariant *params, gpointer user)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) sig; (void) user;
    if (!g_variant_is_of_type (params, G_VARIANT_TYPE ("(ssv)")))
        return;
    const char *ns = NULL, *key = NULL;
    GVariant *v = NULL;
    g_variant_get (params, "(&s&sv)", &ns, &key, &v);
    guint32 scheme = 0;
    if (!g_strcmp0 (ns, APPEARANCE_NS) && !g_strcmp0 (key, "color-scheme") && scheme_of (v, &scheme))
        apply (scheme);
    g_variant_unref (v);
}

void walpe_appearance_follow (void)
{
    GdkDisplay *d = gdk_display_get_default ();
    if (A.bus || !d)
        return;
    gboolean x11 = g_str_has_prefix (G_OBJECT_TYPE_NAME (d), "GdkX11");
    if (!walpe_appearance_wanted (x11, g_getenv ("GTK_THEME"), gtk_get_major_version (),
                                  gtk_get_minor_version ()))
        return;
    GApplication *app = g_application_get_default ();
    GDBusConnection *bus = app ? g_application_get_dbus_connection (app) : NULL;
    if (!bus)
        return;
    A.bus = g_object_ref (bus);
    A.sub = g_dbus_connection_signal_subscribe (A.bus, PORTAL_NAME, PORTAL_SETTINGS, "SettingChanged",
                                                PORTAL_PATH, APPEARANCE_NS, G_DBUS_SIGNAL_FLAGS_NONE,
                                                on_changed, NULL, NULL);
    g_dbus_connection_call (A.bus, PORTAL_NAME, PORTAL_PATH, PORTAL_SETTINGS, "ReadOne",
                            g_variant_new ("(ss)", APPEARANCE_NS, "color-scheme"), G_VARIANT_TYPE ("(v)"),
                            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, on_read, GINT_TO_POINTER (0));
}
