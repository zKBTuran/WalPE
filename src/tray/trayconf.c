/* trayconf.c - tray.ini + GUI-side tray helpers (see trayconf.h) */
#include "trayconf.h"
#include <gio/gio.h>

static char *conf_path (void)
{
    return g_build_filename (g_get_user_config_dir (), "walpe", "tray.ini", NULL);
}

WalpeTrayConf *walpe_tray_conf_load (void)
{
    WalpeTrayConf *c = g_new0 (WalpeTrayConf, 1);
    c->start_with_gui = TRUE;
    c->confirm_quit = TRUE;
    c->backend = g_strdup ("auto");
    GKeyFile *kf = g_key_file_new ();
    char *p = conf_path ();
    if (g_key_file_load_from_file (kf, p, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
        gboolean v = g_key_file_get_boolean (kf, "tray", "start_with_gui", &e);
        if (!e)
            c->start_with_gui = v;
        g_clear_error (&e);
        v = g_key_file_get_boolean (kf, "tray", "confirm_quit", &e);
        if (!e)
            c->confirm_quit = v;
        g_clear_error (&e);
        char *b = g_key_file_get_string (kf, "tray", "backend", NULL);
        if (b && (!g_strcmp0 (b, "auto") || !g_strcmp0 (b, "sni") || !g_strcmp0 (b, "xembed"))) {
            g_free (c->backend);
            c->backend = b;
        } else {
            g_free (b);
        }
    }
    g_free (p);
    g_key_file_unref (kf);
    return c;
}

void walpe_tray_conf_save (const WalpeTrayConf *c)
{
    GKeyFile *kf = g_key_file_new ();
    char *p = conf_path ();
    g_key_file_load_from_file (kf, p, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_boolean (kf, "tray", "start_with_gui", c->start_with_gui);
    g_key_file_set_boolean (kf, "tray", "confirm_quit", c->confirm_quit);
    g_key_file_set_string (kf, "tray", "backend", c->backend ? c->backend : "auto");
    char *dir = g_path_get_dirname (p);
    g_mkdir_with_parents (dir, 0700);
    g_key_file_save_to_file (kf, p, NULL);
    g_free (dir);
    g_free (p);
    g_key_file_unref (kf);
}

void walpe_tray_conf_free (WalpeTrayConf *c)
{
    if (!c)
        return;
    g_free (c->backend);
    g_free (c);
}

char *walpe_tray_sibling_exe (const char *name)
{
    char *exe = g_file_read_link ("/proc/self/exe", NULL);
    char *dir = exe ? g_path_get_dirname (exe) : NULL;
    char *p = dir ? g_build_filename (dir, name, NULL) : NULL;
    g_free (exe);
    g_free (dir);
    if (p && g_file_test (p, G_FILE_TEST_IS_EXECUTABLE))
        return p;
    g_free (p);
    /* build tree: walpe-tray sits in the build root too; else PATH */
    return g_strdup (name);
}

gboolean walpe_tray_is_running (int timeout_ms)
{
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return FALSE;
    GVariant *r = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                               "org.freedesktop.DBus", "NameHasOwner",
                                               g_variant_new ("(s)", WALPE_TRAY_APP_ID),
                                               G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NONE,
                                               timeout_ms, NULL, NULL);
    gboolean own = FALSE;
    if (r) {
        g_variant_get (r, "(b)", &own);
        g_variant_unref (r);
    }
    g_object_unref (bus);
    return own;
}

gboolean walpe_tray_spawn (GError **err)
{
    char *path = walpe_tray_sibling_exe ("walpe-tray");
    char *argv[] = { path, NULL };
    /* no DO_NOT_REAP: GLib double-forks, the tray outlives the caller */
    gboolean ok = g_spawn_async (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, err);
    g_free (path);
    return ok;
}

void walpe_tray_request_quit (void)
{
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return;
    /* GApplication's org.freedesktop.Application: the tray's "quit"
     * action exits the tray only (the engine keeps running) */
    GVariant *r = g_dbus_connection_call_sync (
        bus, WALPE_TRAY_APP_ID, WALPE_TRAY_OBJECT, "org.freedesktop.Application", "ActivateAction",
        g_variant_new ("(sava{sv})", "quit", NULL, NULL), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
        1500, NULL, NULL);
    if (r)
        g_variant_unref (r);
    g_object_unref (bus);
}
