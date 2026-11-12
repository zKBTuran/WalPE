/* wl_gnome.c - asking the WalPE extension for GNOME Shell: see wl_gnome.h.
 * Reached only from the Wayland backend, and there only on a compositor
 * without a layer shell that looks like GNOME's: on every other desktop
 * nothing of this file runs, and no bus is connected to. */
#include "walpe-build-config.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <sys/socket.h>
#include "platform/wl_gnome.h"

/* how long the extension has to answer a call, and how long a start waits
 * for an installed extension to come onto the bus (the shell enables its
 * extensions while it starts) */
#define GN_CALL_MS 1500
#define GN_WAIT_MS 3000

static struct {
    gboolean asked;
    WlGnomeExt state;
    WlGnomeHello hello;
    GDBusConnection *bus;
    int back;                   /* the daemon's end of the socket pair; 0 = none */
    char *address, *data_dir;   /* tests */
} Gn;

void walpe_wl_gnome_use (const char *address, const char *data_dir)
{
    g_free (Gn.address);
    g_free (Gn.data_dir);
    Gn.address = g_strdup (address);
    Gn.data_dir = g_strdup (data_dir);
}

void walpe_wl_gnome_release (void)
{
    if (Gn.bus) {
        g_dbus_connection_close_sync (Gn.bus, NULL, NULL);
        g_clear_object (&Gn.bus);
    }
    Gn.asked = FALSE;
}

static gboolean gn_installed (void)
{
    const char *const *sys = g_get_system_data_dirs ();
    gboolean found = FALSE;
    for (int i = -1; !found && (i < 0 || sys[i]); i++) {
        if (Gn.data_dir && i >= 0)
            break;
        char *f = g_build_filename (Gn.data_dir ? Gn.data_dir : i < 0 ? g_get_user_data_dir () : sys[i],
                                    "gnome-shell", "extensions", WL_GNOME_EXT_UUID, "metadata.json", NULL);
        found = g_file_test (f, G_FILE_TEST_EXISTS);
        g_free (f);
    }
    return found;
}

/* the session bus's address without GLib's fallback of starting a bus */
static char *gn_bus_address (void)
{
    if (Gn.address)
        return g_strdup (Gn.address);
    const char *env = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
    if (env && *env)
        return g_strdup (env);
    char *sock = g_build_filename (g_get_user_runtime_dir (), "bus", NULL);
    char *addr = g_file_test (sock, G_FILE_TEST_EXISTS) ? g_strdup_printf ("unix:path=%s", sock) : NULL;
    g_free (sock);
    return addr;
}

static gboolean gn_hello (void)
{
    GVariant *r = g_dbus_connection_call_sync (Gn.bus, WL_GNOME_BUS_NAME, WL_GNOME_OBJECT, WL_GNOME_IFACE,
                                               "Hello", NULL, G_VARIANT_TYPE ("(a{sv})"),
                                               G_DBUS_CALL_FLAGS_NO_AUTO_START, GN_CALL_MS, NULL, NULL);
    if (!r)
        return FALSE;
    GVariant *about = g_variant_get_child_value (r, 0);
    gboolean ok = wl_gnome_hello_parse (about, &Gn.hello);
    g_variant_unref (about);
    g_variant_unref (r);
    return ok;
}

WlGnomeExt walpe_wl_gnome_find (WlGnomeHello *hello)
{
    if (!Gn.asked) {
        Gn.asked = TRUE;
        Gn.state = WL_GNOME_EXT_NONE;
        memset (&Gn.hello, 0, sizeof Gn.hello);
        const char *env = g_getenv ("WALPE_GNOME_EXT");
        gboolean installed = !(env && !strcmp (env, "0")) && gn_installed ();
        char *addr = env && !strcmp (env, "0") ? NULL : gn_bus_address ();
        if (addr && !Gn.bus)
            Gn.bus = g_dbus_connection_new_for_address_sync (
                addr,
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                    G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                NULL, NULL, NULL);
        g_free (addr);
        if (Gn.bus)
            g_dbus_connection_set_exit_on_close (Gn.bus, FALSE);
        gboolean answered = Gn.bus && gn_hello ();
        /* installed and silent: the shell may still be starting */
        for (gint64 end = g_get_monotonic_time () + GN_WAIT_MS * 1000;
             Gn.bus && !answered && installed && g_get_monotonic_time () < end;) {
            g_usleep (250000);
            answered = gn_hello ();
        }
        Gn.state = answered ? (Gn.hello.protocol == WL_GNOME_PROTOCOL ? WL_GNOME_EXT_ACTIVE
                                                                      : WL_GNOME_EXT_OTHER)
                            : installed ? WL_GNOME_EXT_SILENT : WL_GNOME_EXT_NONE;
    }
    if (hello)
        *hello = Gn.hello;
    return Gn.state;
}

int walpe_wl_gnome_socket (char **why)
{
    int sv[2];
    if (why)
        *why = NULL;
    if (Gn.state != WL_GNOME_EXT_ACTIVE || !Gn.bus) {
        if (why)
            *why = g_strdup ("the extension does not answer");
        return -1;
    }
    if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        if (why)
            *why = g_strdup_printf ("socketpair: %s", g_strerror (errno));
        return -1;
    }
    GError *e = NULL;
    GUnixFDList *fds = g_unix_fd_list_new ();
    int idx = g_unix_fd_list_append (fds, sv[1], &e);    /* (a copy: ours is closed below) */
    close (sv[1]);
    GVariant *r = idx < 0 ? NULL
                          : g_dbus_connection_call_with_unix_fd_list_sync (
                                Gn.bus, WL_GNOME_BUS_NAME, WL_GNOME_OBJECT, WL_GNOME_IFACE, "Socket",
                                g_variant_new ("(h)", idx), NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                2 * GN_CALL_MS, fds, NULL, NULL, &e);
    g_object_unref (fds);
    int fd = -1;
    if (!r) {
        if (why) {
            g_dbus_error_strip_remote_error (e);
            *why = g_strdup_printf ("the extension did not start the process that hands the "
                                    "connection over: %s", e->message);
        }
    } else {
        g_variant_unref (r);
        fd = wl_gnome_recv_fd (sv[0], 2 * GN_CALL_MS);
        if (fd < 0 && why)
            *why = g_strdup ("the process the shell started for the connection sent none (is the "
                             "walpe-daemon file the one that is running?)");
    }
    g_clear_error (&e);
    if (fd < 0) {
        close (sv[0]);
        return -1;
    }
    if (Gn.back > 0)
        close (Gn.back);
    Gn.back = sv[0];
    return fd;
}
