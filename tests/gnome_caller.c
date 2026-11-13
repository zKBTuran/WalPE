/* gnome_caller.c - a program that is not walpe-daemon and asks the WalPE
 * extension for GNOME Shell (contrib/gnome) for connections to the
 * compositor, more than a daemon ever needs. For scripts/gnomecheck.py,
 * case "socket-bound"; GNOME Shell 46 to 48, where the extension's method
 * Socket has the shell start the caller's own program again.
 *
 *   gnome-caller-test N
 *       calls Socket N times and keeps every connection that comes. One
 *       line per call, "handed" or "refused: <the extension's sentence>",
 *       then "done"; stays until its standard input is closed.
 *   gnome-caller-test --wayland-socket-to FD
 *       what the shell starts: hands its connection over and waits, as
 *       walpe-daemon does (platform/wl_gnome.h). */
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <stdio.h>
#include "platform/wl_gnome.h"

int main (int argc, char **argv)
{
    if (argc == 3 && !strcmp (argv[1], WL_GNOME_DONOR_OPTION))
        return wl_gnome_donor_run (getenv ("WAYLAND_SOCKET"), argv[2]);
    int n = argc > 1 ? atoi (argv[1]) : 1;
    GError *e = NULL;
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &e);
    if (!bus) {
        printf ("no session bus: %s\n", e->message);
        return 2;
    }
    setvbuf (stdout, NULL, _IOLBF, 0);
    for (int i = 0; i < n; i++) {
        int sv[2];
        if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
            return 2;
        GUnixFDList *fds = g_unix_fd_list_new ();
        int idx = g_unix_fd_list_append (fds, sv[1], NULL);
        close (sv[1]);
        GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync (
            bus, WL_GNOME_BUS_NAME, WL_GNOME_OBJECT, WL_GNOME_IFACE, "Socket", g_variant_new ("(h)", idx),
            NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, fds, NULL, NULL, &e);
        g_object_unref (fds);
        if (r) {
            g_variant_unref (r);
            /* (the connection and the socket pair stay open: sv[0] is never closed) */
            printf (wl_gnome_recv_fd (sv[0], 10000) >= 0 ? "handed\n" : "refused: no connection came\n");
        } else {
            g_dbus_error_strip_remote_error (e);
            printf ("refused: %s\n", e->message);
            g_clear_error (&e);
            close (sv[0]);
        }
    }
    printf ("done\n");
    char c;
    while (read (0, &c, 1) > 0)
        ;
    return 0;
}
