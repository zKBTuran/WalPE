/* webgtkinit_test.c - GTK brought up on the private page host
 * (src/engine/webgtkinit.h, what web.c does), with the real GTK and the
 * real page host (src/engine/webhost_wl.c, compiled into the test), no
 * WebKit:
 *   - no display is opened before GTK is initialised. GTK from 4.18 on
 *     ends the process there, and with it every web wallpaper on the
 *     private page host was off; an older GTK allows it, so the test's
 *     own gdk_display_open, in front of GDK's, says so on every GTK;
 *   - GTK comes up and its default display is the page host's, the one
 *     client the host takes;
 *   - the display the environment names (WAYLAND_DISPLAY: the session's
 *     compositor, here a socket of the test) is never connected to;
 *   - the process is still in the locale it started in.
 * No compositor, no X display, no GPU. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "engine/webgtkinit.h"
#include "engine/webhost_wl.h"
#include <dlfcn.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int fails;
static int opened, opened_early;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fails++;                                                          \
            fprintf (stderr, "FAIL %s:%d: (%s) ", __FILE__, __LINE__, #cond); \
            fprintf (stderr, __VA_ARGS__);                                    \
            fputc ('\n', stderr);                                             \
        }                                                                     \
    } while (0)

/* The program's own gdk_display_open: a call from this program (the code
 * of webgtkinit.h is compiled into it) comes here first. It notes a
 * display opened before GTK was initialised and does not hand that call
 * on - GTK 4.18 and later would end the process in it. */
GdkDisplay *gdk_display_open (const char *name)
{
    static GdkDisplay *(*real) (const char *);
    opened++;
    if (!gtk_is_initialized ()) {
        opened_early++;
        return NULL;
    }
    if (!real)
        real = (GdkDisplay * (*) (const char *)) dlsym (RTLD_NEXT, "gdk_display_open");
    return real ? real (name) : NULL;
}

/* the session's compositor, as far as the environment says: a socket
 * that turns everybody away and counts who came */
static int decoy_fd = -1;
static gint decoy_callers;

static gpointer decoy_thread (gpointer user)
{
    (void) user;
    for (;;) {
        int c = accept (decoy_fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR)
                continue;
            return NULL;
        }
        g_atomic_int_inc (&decoy_callers);
        close (c);
    }
}

static gboolean decoy_listen (const char *path)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    decoy_fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (decoy_fd < 0 || strlen (path) >= sizeof a.sun_path)
        return FALSE;
    g_strlcpy (a.sun_path, path, sizeof a.sun_path);
    if (bind (decoy_fd, (struct sockaddr *) &a, sizeof a) < 0 || listen (decoy_fd, 4) < 0)
        return FALSE;
    g_thread_unref (g_thread_new ("decoy", decoy_thread, NULL));
    return TRUE;
}

int main (void)
{
    GError *err = NULL;
    char *dir = g_dir_make_tmp ("walpe-webgtkinit-XXXXXX", &err);
    if (!dir) {
        fprintf (stderr, "webgtkinit: no temporary directory: %s\n", err->message);
        return 1;
    }
    /* (before anything starts a thread) */
    g_setenv ("XDG_RUNTIME_DIR", dir, TRUE);
    g_setenv ("WAYLAND_DISPLAY", "wayland-session", TRUE);
    g_unsetenv ("WAYLAND_SOCKET");
    g_unsetenv ("DISPLAY");
    g_unsetenv ("GDK_BACKEND");
    char *decoy_path = g_build_filename (dir, "wayland-session", NULL);
    gboolean decoy = decoy_listen (decoy_path);
    CHECK (decoy, "the socket that stands for the session's compositor: %s", g_strerror (errno));
    char *loc0 = g_strdup (setlocale (LC_ALL, NULL));

    char *sock = NULL, *why = NULL;
    CHECK (walpe_webhost_start (&sock, &why), "the page host starts: %s", why ? why : "");
    if (!sock)
        return 1;
    CHECK (!gtk_is_initialized (), "GTK is not initialised before the module asks for it");

    gboolean gtk_failed = FALSE;
    GdkDisplay *d = web_gtk_init_on (sock, &gtk_failed);
    CHECK (opened_early == 0, "no display is opened before GTK is initialised (GTK 4.18 and later end the "
                              "process there: gdk_display_open() was called before gtk_init()); %d of %d "
                              "calls came too early", opened_early, opened);
    CHECK (d != NULL && !gtk_failed, "GTK %d.%d.%d comes up on the page host's display (gtk_failed %d)",
           (int) gtk_get_major_version (), (int) gtk_get_minor_version (), (int) gtk_get_micro_version (),
           gtk_failed);
    CHECK (gtk_is_initialized (), "GTK is initialised");
    CHECK (d && gdk_display_get_default () == d, "the page host's display is GTK's default display");
    {
        GSList *all = gdk_display_manager_list_displays (gdk_display_manager_get ());
        CHECK (g_slist_length (all) == (d ? 1 : 0), "and the only one that is open (%u)", g_slist_length (all));
        g_slist_free (all);
    }
    /* GTK is the host's one client (the host has told it its globals by
     * the time the display is open) */
    for (int i = 0; i < 100 && d && !walpe_webhost_running (); i++)
        g_usleep (20000);
    CHECK (!d || walpe_webhost_running (), "the page host runs: GTK is its client");
    CHECK (g_atomic_int_get (&decoy_callers) == 0, "nothing connected to the display the environment names "
                                                    "(%d connections)", g_atomic_int_get (&decoy_callers));
    CHECK (g_strcmp0 (loc0, setlocale (LC_ALL, NULL)) == 0, "the locale is the one the process started in: %s, "
                                                             "now %s", loc0, setlocale (LC_ALL, NULL));
    /* asked a second time, GTK is not initialised again: the same display */
    if (d) {
        CHECK (gtk_init_check (), "gtk_init_check says yes from here on");
        CHECK (gdk_display_get_default () == d, "and leaves the default display alone");
        CHECK (g_atomic_int_get (&decoy_callers) == 0, "still nothing connected to the session's display");
    }

    /* (the host has no shutdown: it lives as long as the process; its
     * socket and directory are gone once GTK is connected) */
    g_unlink (decoy_path);
    g_free (decoy_path);
    g_free (loc0);
    g_free (sock);
    g_rmdir (dir);
    g_free (dir);
    if (fails) {
        fprintf (stderr, "webgtkinit: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("webgtkinit: ok\n");
    return 0;
}
