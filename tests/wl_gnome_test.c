/* wl_gnome_test.c - GNOME Shell: what the daemon and the WalPE extension
 * agree on (src/platform/wl_gnome.h) and the daemon's questions on the
 * session bus (src/platform/wl_gnome.c).
 *
 * Without a bus: what counts as GNOME's compositor, the title of a
 * wallpaper window, the reading of Hello's answer (also one made to
 * mislead), the sentences for the log, one descriptor through a socket
 * pair, and the process the shell starts for the connection
 * (wl_gnome_donor_run, on a thread, with socket pairs standing in for the
 * compositor's connection).
 * With a private bus (GTestDBus; that part is skipped where there is no
 * dbus-daemon) and a thread that owns io.github.walpe.Gnome as the
 * extension does: active, another version, installed and silent, not
 * installed, switched off with WALPE_GNOME_EXT=0; and Socket, where the
 * stand-in plays shell and started process in one: it sends a descriptor
 * back through the pair it was handed, refuses, or sends nothing. */
#include "walpe-build-config.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <stdio.h>
#include "platform/wl_gnome.h"

static int fails;
#define CHECK(cond, ...)                                             \
    do {                                                             \
        if (!(cond)) {                                               \
            fails++;                                                 \
            fprintf (stderr, "FAIL %s:%d: ", __FILE__, __LINE__);    \
            fprintf (stderr, __VA_ARGS__);                           \
            fputc ('\n', stderr);                                    \
        }                                                            \
    } while (0)

/* ---- the stand-in extension */
typedef enum { SOCK_GIVE, SOCK_REFUSE, SOCK_SILENT } SockMode;

static struct {
    GThread *thread;
    GMainContext *ctx;
    GMainLoop *loop;
    GDBusConnection *bus;
    GDBusNodeInfo *info;
    char *address;
    gint ready, hellos, sockets;
    guint protocol;
    gboolean socket;
    SockMode mode;
    int far_end;            /* the other end of what Socket gave out; -1 = none */
} F = { .far_end = -1 };

static const char fake_xml[] =
    "<node><interface name='" WL_GNOME_IFACE "'>"
    " <method name='Hello'><arg type='a{sv}' direction='out'/></method>"
    " <method name='Socket'><arg type='h' direction='in'/></method>"
    "</interface></node>";

static void fake_method (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer user)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) user;
    if (!strcmp (method, "Hello")) {
        GVariantBuilder b;
        g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
        g_variant_builder_add (&b, "{sv}", "protocol", g_variant_new_uint32 (F.protocol));
        g_variant_builder_add (&b, "{sv}", "socket", g_variant_new_boolean (F.socket));
        g_variant_builder_add (&b, "{sv}", "shell", g_variant_new_string ("46.0"));
        g_variant_builder_add (&b, "{sv}", "extension", g_variant_new_string ("0.14.0"));
        g_atomic_int_inc (&F.hellos);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(a{sv})", &b));
    } else if (!strcmp (method, "Socket")) {
        g_atomic_int_inc (&F.sockets);
        gint32 idx = -1;
        g_variant_get (args, "(h)", &idx);
        GUnixFDList *fds = g_dbus_message_get_unix_fd_list (g_dbus_method_invocation_get_message (inv));
        int back = fds ? g_unix_fd_list_get (fds, idx, NULL) : -1;
        if (F.mode == SOCK_REFUSE || back < 0) {
            g_dbus_method_invocation_return_dbus_error (inv, WL_GNOME_IFACE ".Error", "made-up refusal");
        } else {
            if (F.mode == SOCK_GIVE) {
                /* shell and started process in one: a connection, sent back */
                int sv[2];
                if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0) {
                    wl_gnome_send_fd (back, sv[1]);
                    close (sv[1]);
                    F.far_end = sv[0];
                }
            }
            g_dbus_method_invocation_return_value (inv, NULL);
        }
        if (back >= 0)
            close (back);
    } else {
        g_dbus_method_invocation_return_error_literal (inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, method);
    }
}

static const GDBusInterfaceVTable fake_vt = { fake_method, NULL, NULL, { 0 } };

static gpointer fake_thread (gpointer u)
{
    (void) u;
    g_main_context_push_thread_default (F.ctx);
    F.bus = g_dbus_connection_new_for_address_sync (
        F.address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    if (!F.bus) {
        g_atomic_int_set (&F.ready, -1);
        return NULL;
    }
    F.info = g_dbus_node_info_new_for_xml (fake_xml, NULL);
    g_dbus_connection_register_object (F.bus, WL_GNOME_OBJECT, F.info->interfaces[0], &fake_vt, NULL, NULL,
                                       NULL);
    g_atomic_int_set (&F.ready, 1);
    g_main_loop_run (F.loop);
    g_dbus_connection_close_sync (F.bus, NULL, NULL);
    g_object_unref (F.bus);
    g_main_context_pop_thread_default (F.ctx);
    return NULL;
}

/* the name is asked for and given up from this thread, on a connection of
 * the service thread: GDBus allows calls from any thread */
static void fake_own (gboolean own)
{
    GVariant *r = g_dbus_connection_call_sync (
        F.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        own ? "RequestName" : "ReleaseName",
        own ? g_variant_new ("(su)", WL_GNOME_BUS_NAME, 0u) : g_variant_new ("(s)", WL_GNOME_BUS_NAME),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
    if (r)
        g_variant_unref (r);
}

/* ---- pieces */

/* does writing into `a` come out of `b`? */
static gboolean joined (int a, int b)
{
    char c = 0;
    struct pollfd p = { .fd = b, .events = POLLIN };
    return write (a, "k", 1) == 1 && poll (&p, 1, 2000) == 1 && read (b, &c, 1) == 1 && c == 'k';
}

static GVariant *about (guint protocol, gboolean with_protocol, const char *shell)
{
    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
    if (with_protocol)
        g_variant_builder_add (&b, "{sv}", "protocol", g_variant_new_uint32 (protocol));
    g_variant_builder_add (&b, "{sv}", "socket", g_variant_new_boolean (TRUE));
    if (shell)
        g_variant_builder_add (&b, "{sv}", "shell", g_variant_new_string (shell));
    return g_variant_ref_sink (g_variant_builder_end (&b));
}

static void test_pure (void)
{
    CHECK (wl_gnome_session (TRUE, NULL) && wl_gnome_session (FALSE, "ubuntu:GNOME") &&
           wl_gnome_session (FALSE, "gnome") && !wl_gnome_session (FALSE, "KDE") &&
           !wl_gnome_session (FALSE, NULL) && !wl_gnome_session (FALSE, ""),
           "GNOME's compositor: its global, or the desktop's name");
    char *t = wl_gnome_title ("DP-1");
    CHECK (!strcmp (t, WL_GNOME_TITLE_PREFIX "DP-1"), "the title names the output: %s", t);
    g_free (t);
    t = wl_gnome_title (NULL);
    CHECK (g_str_has_prefix (t, WL_GNOME_TITLE_PREFIX) && strlen (t) > strlen (WL_GNOME_TITLE_PREFIX),
           "an output without a name still has a title: %s", t);
    g_free (t);

    WlGnomeHello h;
    GVariant *v = about (1, TRUE, "46.0");
    CHECK (wl_gnome_hello_parse (v, &h) && h.protocol == 1 && h.socket && !strcmp (h.shell, "46.0") &&
           !*h.extension, "an answer: protocol, socket, the shell's version");
    g_variant_unref (v);
    v = about (0, FALSE, "46.0");
    CHECK (!wl_gnome_hello_parse (v, &h), "no protocol: not an answer of the extension");
    g_variant_unref (v);
    v = about (0, TRUE, "46.0");
    CHECK (!wl_gnome_hello_parse (v, &h), "protocol 0 neither");
    g_variant_unref (v);
    v = g_variant_ref_sink (g_variant_new_string ("hello"));
    CHECK (!wl_gnome_hello_parse (v, &h) && !wl_gnome_hello_parse (NULL, &h), "nor something that is no dictionary");
    g_variant_unref (v);
    v = about (1, TRUE, "46\n\033[31m.0 and a version that goes on for a very long time indeed");
    CHECK (wl_gnome_hello_parse (v, &h) && strlen (h.shell) == sizeof h.shell - 1 &&
           !strchr (h.shell, '\n') && !strchr (h.shell, '\033'),
           "a version made to mislead a log is cut and cleaned: %s", h.shell);
    g_variant_unref (v);

    CHECK (wl_gnome_ext_hint (WL_GNOME_EXT_ACTIVE) == NULL, "active: nothing to add to a refusal");
    CHECK (strstr (wl_gnome_ext_hint (WL_GNOME_EXT_NONE), "contrib/gnome/install.sh install") != NULL,
           "not installed: the sentence has the command that installs it");
    CHECK (strstr (wl_gnome_ext_hint (WL_GNOME_EXT_SILENT), "gnome-extensions enable " WL_GNOME_EXT_UUID) != NULL,
           "installed, not active: the command that switches it on");
    CHECK (strstr (wl_gnome_ext_hint (WL_GNOME_EXT_OTHER), "another version") != NULL, "another version: said");
    for (int e = WL_GNOME_EXT_NONE; e <= WL_GNOME_EXT_ACTIVE; e++)
        CHECK (*wl_gnome_ext_name (e), "state %d has a name", e);

    memset (&h, 0, sizeof h);
    g_strlcpy (h.shell, "46.0", sizeof h.shell);
    char *m = wl_gnome_mode_text (&h, TRUE);
    CHECK (strstr (m, "GNOME Shell 46.0") && strstr (m, "handed over"), "the mode, handed over: %s", m);
    g_free (m);
    m = wl_gnome_mode_text (&h, FALSE);
    CHECK (strstr (m, "desktop window") && !strstr (m, "handed over"), "the mode, own connection: %s", m);
    g_free (m);
}

static void test_fd (void)
{
    int sv[2], pp[2];
    g_assert (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0 && pipe (pp) == 0);
    CHECK (wl_gnome_recv_fd (sv[0], 50) == -1, "nothing sent: -1 after the wait");
    CHECK (wl_gnome_send_fd (sv[1], pp[1]), "a descriptor is sent");
    int got = wl_gnome_recv_fd (sv[0], 2000);
    CHECK (got >= 0 && got != pp[1], "and arrives as a new one");
    CHECK (got >= 0 && (fcntl (got, F_GETFD) & FD_CLOEXEC), "which is close-on-exec");
    CHECK (got >= 0 && joined (got, pp[0]), "for the same thing");
    if (got >= 0)
        close (got);
    CHECK (write (sv[1], "x", 1) == 1 && wl_gnome_recv_fd (sv[0], 2000) == -1, "a byte without a descriptor: -1");
    close (sv[1]);
    CHECK (wl_gnome_recv_fd (sv[0], 2000) == -1, "the other end closed: -1");
    close (sv[0]);
    close (pp[0]);
    close (pp[1]);
}

/* ---- the process the shell starts, on a thread */
typedef struct {
    char wl[16], back[16];
    gint status;
} Donor;

static gpointer donor_thread (gpointer p)
{
    Donor *d = p;
    g_atomic_int_set (&d->status, wl_gnome_donor_run (d->wl, d->back));
    return NULL;
}

static void test_donor (void)
{
    CHECK (wl_gnome_donor_run (NULL, "4") == 2 && wl_gnome_donor_run ("", "4") == 2 &&
           wl_gnome_donor_run ("3", NULL) == 2 && wl_gnome_donor_run ("x", "4") == 2 &&
           wl_gnome_donor_run ("3", "4x") == 2 && wl_gnome_donor_run ("-1", "4") == 2 &&
           wl_gnome_donor_run ("5", "5") == 2, "what is no pair of descriptors: status 2, nothing touched");

    /* wl: the connection the shell gave the process (the test holds the
     * compositor's end); back: the pair of the daemon (the test's end) */
    int wl[2], back[2];
    g_assert (socketpair (AF_UNIX, SOCK_STREAM, 0, wl) == 0 && socketpair (AF_UNIX, SOCK_STREAM, 0, back) == 0);
    Donor d = { .status = -1 };
    g_snprintf (d.wl, sizeof d.wl, "%d", wl[1]);
    g_snprintf (d.back, sizeof d.back, "%d", back[1]);
    GThread *t = g_thread_new ("donor", donor_thread, &d);
    int got = wl_gnome_recv_fd (back[0], 3000);
    CHECK (got >= 0, "the connection arrives at the daemon's end");
    CHECK (got >= 0 && joined (got, wl[0]), "and is the one the shell gave");
    g_usleep (50000);
    CHECK (g_atomic_int_get (&d.status) == -1, "the process stays while the daemon holds its end");
    /* the process has let go of its own copy: when the daemon closes the
     * connection, the compositor sees the end of it */
    if (got >= 0)
        close (got);
    char c;
    struct pollfd p = { .fd = wl[0], .events = POLLIN };
    CHECK (poll (&p, 1, 2000) == 1 && read (wl[0], &c, 1) == 0, "the daemon's copy was the only one left");
    close (back[0]);
    g_thread_join (t);
    CHECK (g_atomic_int_get (&d.status) == 0, "the daemon's end closed: the process ends with status 0");
    close (wl[0]);
    close (back[1]);

    /* nobody at the other end: it does not wait */
    g_assert (socketpair (AF_UNIX, SOCK_STREAM, 0, wl) == 0 && socketpair (AF_UNIX, SOCK_STREAM, 0, back) == 0);
    close (back[0]);
    g_snprintf (d.wl, sizeof d.wl, "%d", wl[1]);
    g_snprintf (d.back, sizeof d.back, "%d", back[1]);
    CHECK (wl_gnome_donor_run (d.wl, d.back) == 1, "the daemon is gone already: status 1");
    close (wl[0]);
    close (wl[1]);
    close (back[1]);
}

static void test_bus (const char *tmp)
{
    WlGnomeHello h;
    char *with = g_build_filename (tmp, "with", NULL), *without = g_build_filename (tmp, "without", NULL);
    char *dir = g_build_filename (with, "gnome-shell", "extensions", WL_GNOME_EXT_UUID, NULL);
    char *meta = g_build_filename (dir, "metadata.json", NULL);
    g_mkdir_with_parents (dir, 0700);
    g_mkdir_with_parents (without, 0700);
    g_file_set_contents (meta, "{}", -1, NULL);

    /* nobody on the bus, no files: not installed, and no waiting */
    walpe_wl_gnome_use (F.address, without);
    gint64 t0 = g_get_monotonic_time ();
    CHECK (walpe_wl_gnome_find (&h) == WL_GNOME_EXT_NONE, "no files, no name: not installed");
    CHECK (g_get_monotonic_time () - t0 < 1500000, "said at once");
    char *why = NULL;
    CHECK (walpe_wl_gnome_socket (&why) == -1 && why, "no connection is asked for then");
    g_free (why);
    walpe_wl_gnome_release ();

    /* files, nobody on the bus: installed and silent, after a wait */
    walpe_wl_gnome_use (F.address, with);
    t0 = g_get_monotonic_time ();
    CHECK (walpe_wl_gnome_find (&h) == WL_GNOME_EXT_SILENT, "files, no name: installed, not active");
    CHECK (g_get_monotonic_time () - t0 > 2000000, "after waiting for a shell that may be starting");
    walpe_wl_gnome_release ();

    /* the extension answers */
    fake_own (TRUE);
    F.protocol = WL_GNOME_PROTOCOL;
    F.socket = TRUE;
    CHECK (walpe_wl_gnome_find (&h) == WL_GNOME_EXT_ACTIVE && h.socket && !strcmp (h.shell, "46.0") &&
           !strcmp (h.extension, "0.14.0"), "the name is owned and Hello answers: active");
    int n = g_atomic_int_get (&F.hellos);
    CHECK (walpe_wl_gnome_find (NULL) == WL_GNOME_EXT_ACTIVE && g_atomic_int_get (&F.hellos) == n,
           "asked once: the answer is kept");

    /* Socket: a connection comes back through the pair */
    F.mode = SOCK_GIVE;
    why = NULL;
    int fd = walpe_wl_gnome_socket (&why);
    CHECK (fd >= 0 && !why, "Socket: a descriptor (%s)", why ? why : "-");
    CHECK (fd >= 0 && (fcntl (fd, F_GETFD) & FD_CLOEXEC), "close-on-exec");
    CHECK (fd >= 0 && F.far_end >= 0 && joined (fd, F.far_end), "the one the other side made");
    if (fd >= 0)
        close (fd);
    if (F.far_end >= 0)
        close (F.far_end);
    F.far_end = -1;
    F.mode = SOCK_REFUSE;
    fd = walpe_wl_gnome_socket (&why);
    CHECK (fd == -1 && why && strstr (why, "made-up refusal"), "a refusal is passed on: %s", why ? why : "-");
    g_free (why);
    F.mode = SOCK_SILENT;
    why = NULL;
    fd = walpe_wl_gnome_socket (&why);
    CHECK (fd == -1 && why && strstr (why, "sent none"), "an answer and no descriptor: said (%s)", why ? why : "-");
    g_free (why);
    walpe_wl_gnome_release ();

    /* another version of the interface */
    F.protocol = WL_GNOME_PROTOCOL + 1;
    CHECK (walpe_wl_gnome_find (&h) == WL_GNOME_EXT_OTHER && h.protocol == WL_GNOME_PROTOCOL + 1,
           "another protocol version: not used");
    walpe_wl_gnome_release ();
    F.protocol = WL_GNOME_PROTOCOL;

    /* switched off from the environment: nothing is asked */
    g_setenv ("WALPE_GNOME_EXT", "0", TRUE);
    n = g_atomic_int_get (&F.hellos);
    CHECK (walpe_wl_gnome_find (&h) == WL_GNOME_EXT_NONE && g_atomic_int_get (&F.hellos) == n,
           "WALPE_GNOME_EXT=0: not asked, as if not installed");
    walpe_wl_gnome_release ();
    g_unsetenv ("WALPE_GNOME_EXT");

    fake_own (FALSE);
    walpe_wl_gnome_use (NULL, NULL);
    g_unlink (meta);
    g_rmdir (dir);
    g_free (meta);
    g_free (dir);
    g_free (with);
    g_free (without);
}

int main (void)
{
    test_pure ();
    test_fd ();
    test_donor ();

    char *dbus_daemon = g_find_program_in_path ("dbus-daemon");
    if (!dbus_daemon) {
        printf ("wl-gnome: the bus part is skipped (no dbus-daemon)\n");
        return fails ? 1 : 77;
    }
    g_free (dbus_daemon);
    char *tmp = g_dir_make_tmp ("walpe-wl-gnome-XXXXXX", NULL);
    GTestDBus *tb = g_test_dbus_new (G_TEST_DBUS_NONE);
    g_test_dbus_up (tb);
    F.address = g_strdup (g_test_dbus_get_bus_address (tb));
    F.ctx = g_main_context_new ();
    F.loop = g_main_loop_new (F.ctx, FALSE);
    F.thread = g_thread_new ("fake-extension", fake_thread, NULL);
    while (!g_atomic_int_get (&F.ready))
        g_usleep (1000);
    if (g_atomic_int_get (&F.ready) > 0) {
        test_bus (tmp);
        g_main_loop_quit (F.loop);
    } else {
        fails++;
        fprintf (stderr, "FAIL: no connection to the test bus\n");
    }
    g_thread_join (F.thread);
    g_test_dbus_down (tb);
    g_object_unref (tb);
    char *cmd = g_strdup_printf ("rm -rf '%s'", tmp);
    if (g_str_has_prefix (tmp, g_get_tmp_dir ()) && system (cmd) != 0)
        fprintf (stderr, "wl-gnome: %s was left behind\n", tmp);
    g_free (cmd);
    g_free (tmp);
    if (fails) {
        fprintf (stderr, "wl-gnome: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("wl-gnome: ok\n");
    return 0;
}
