/* respolicy_test - the pieces behind "nothing wakes up while nothing
 * happens" that need neither a daemon nor a display:
 *  - config.json: the opt-in resource rules keep their defaults (as
 *    before), survive a save / load, and their names parse;
 *  - common/ipc.c state pushes: a "subscribe"d connection gets every
 *    broadcast line, a subscriber that stops reading is dropped without
 *    stalling the server, the number of subscribers is bounded;
 *  - common/ipcsub.c: the client side connects, reports the first status
 *    line and every push, and notices a server that cannot push. */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/config.h"
#include "common/ipc.h"
#include "common/ipcsub.h"

static int fails;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static GMainLoop *loop;
static char *sock;
static gboolean can_push = TRUE;

static char *handler (const char *req, gpointer user)
{
    (void) user;
    if (strstr (req, "\"subscribe\"")) {
        if (!can_push)
            return g_strdup ("{\"ok\":false,\"error\":\"unknown cmd\"}");
        if (!walpe_ipc_subscribe_current ())
            return g_strdup ("{\"ok\":false,\"error\":\"too many subscribers\"}");
        return g_strdup ("{\"ok\":true,\"state\":{\"n\":0},\"subscribed\":true}");
    }
    return g_strdup ("{\"ok\":true}");
}

static int raw_connect (void)
{
    int fd = socket (AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    g_strlcpy (a.sun_path, sock, sizeof (a.sun_path));
    if (connect (fd, (struct sockaddr *) &a, sizeof (a)) != 0) {
        close (fd);
        return -1;
    }
    return fd;
}

/* one line (without the newline) from fd within ms, NULL = none */
static char *read_line (int fd, int ms)
{
    GString *s = g_string_new (NULL);
    gint64 end = g_get_monotonic_time () + (gint64) ms * 1000;
    struct timeval tv = { 0, 50000 };
    setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
    while (g_get_monotonic_time () < end) {
        char c;
        ssize_t n = recv (fd, &c, 1, 0);
        if (n == 1) {
            if (c == '\n')
                return g_string_free (s, FALSE);
            g_string_append_c (s, c);
        } else if (n == 0) {
            break;
        }
    }
    g_string_free (s, TRUE);
    return NULL;
}

static gboolean do_broadcast (gpointer line)
{
    walpe_ipc_broadcast (line);
    return G_SOURCE_REMOVE;
}

static gboolean big_broadcasts (gpointer u)
{
    (void) u;
    /* far more than a socket buffer holds: the silent subscriber must go */
    char *big = g_strnfill (200000, 'x');
    for (int i = 0; i < 40; i++)
        walpe_ipc_broadcast (big);
    g_free (big);
    return G_SOURCE_REMOVE;
}

static gpointer client_thread (gpointer u)
{
    (void) u;
    /* a subscriber gets the reply, then each broadcast line, in order */
    int fd = raw_connect ();
    CHECK (fd >= 0);
    CHECK (send (fd, "{\"cmd\":\"subscribe\"}\n", 20, MSG_NOSIGNAL) == 20);
    char *l = read_line (fd, 3000);
    CHECK (l && strstr (l, "\"subscribed\":true"));
    g_free (l);
    g_idle_add (do_broadcast, "{\"event\":\"state\",\"n\":1}");
    g_idle_add (do_broadcast, "{\"event\":\"state\",\"n\":2}");
    l = read_line (fd, 3000);
    CHECK (l && strstr (l, "\"n\":1"));
    g_free (l);
    l = read_line (fd, 3000);
    CHECK (l && strstr (l, "\"n\":2"));
    g_free (l);
    /* a plain client is not a subscriber: it gets no pushes */
    int plain = raw_connect ();
    CHECK (send (plain, "{\"cmd\":\"hello\"}\n", 16, MSG_NOSIGNAL) == 16);
    l = read_line (plain, 3000);
    CHECK (l && strstr (l, "\"ok\":true"));
    g_free (l);
    g_idle_add (do_broadcast, "{\"event\":\"state\",\"n\":3}");
    l = read_line (fd, 3000);
    CHECK (l && strstr (l, "\"n\":3"));
    g_free (l);
    l = read_line (plain, 300);
    CHECK (l == NULL);
    close (plain);

    /* the number of subscribers is bounded (8), the 9th is told so */
    int more[9];
    int refused = 0;
    for (int i = 0; i < 9; i++) {
        more[i] = raw_connect ();
        CHECK (send (more[i], "{\"cmd\":\"subscribe\"}\n", 20, MSG_NOSIGNAL) == 20);
        l = read_line (more[i], 3000);
        refused += l && strstr (l, "too many subscribers");
        g_free (l);
    }
    CHECK (refused == 2);   /* fd above is the first of the 8 */
    for (int i = 0; i < 9; i++)
        close (more[i]);
    g_usleep (200000);      /* the server sees them go */

    /* a subscriber that never reads: dropped, the server goes on, the
     * reading subscriber still gets what follows */
    int deaf = raw_connect ();
    CHECK (send (deaf, "{\"cmd\":\"subscribe\"}\n", 20, MSG_NOSIGNAL) == 20);
    g_usleep (200000);
    close (fd);             /* the reader leaves: only the deaf one is subscribed */
    g_usleep (200000);
    g_idle_add (big_broadcasts, NULL);
    gint64 t0 = g_get_monotonic_time ();
    char *r = walpe_ipc_request (sock, "{\"cmd\":\"after deaf\"}", 10000);
    CHECK (r && strstr (r, "\"ok\":true"));
    CHECK (g_get_monotonic_time () - t0 < 5 * G_USEC_PER_SEC);
    g_free (r);
    g_usleep (200000);
    CHECK (walpe_ipc_subscribers () == 0);
    close (deaf);
    g_main_loop_quit (loop);
    return NULL;
}

/* ---- the client side (common/ipcsub.c), on the main loop */
static int sub_lines, sub_lost;
static WalpeIpcSub *sub;

static void on_sub (const char *line, gpointer user)
{
    (void) user;
    if (!line) {
        sub_lost++;
        g_main_loop_quit (loop);
        return;
    }
    sub_lines++;
    if (sub_lines == 1) {
        CHECK (strstr (line, "\"subscribed\":true") != NULL);
        CHECK (walpe_ipc_sub_live (sub));
        g_idle_add (do_broadcast, "{\"event\":\"state\",\"n\":7}");
    } else {
        CHECK (strstr (line, "\"n\":7") != NULL);
        g_main_loop_quit (loop);
    }
}

static gboolean give_up (gpointer u)
{
    (void) u;
    CHECK (!"timed out");
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

static void test_config (const char *dir)
{
    g_setenv ("XDG_CONFIG_HOME", dir, TRUE);
    WalpeConfig *c = walpe_config_load ();
    /* defaults: nothing changes unless the user asks */
    CHECK (c->focus_action == WALPE_FOCUS_RUN);
    CHECK (c->focus_fps == 15);
    CHECK (!c->pause_battery);
    CHECK (c->battery_fps == 0);
    CHECK (c->preview_anim == WALPE_PREVIEW_ALWAYS);
    CHECK (c->pause_covered && c->cover_transparent && c->pause_fullscreen && c->pause_dpms);
    guint n0 = walpe_config_save_count ();
    c->focus_action = WALPE_FOCUS_LIMIT;
    c->focus_fps = 24;
    c->pause_battery = TRUE;
    c->battery_fps = 30;
    c->preview_anim = WALPE_PREVIEW_HOVER;
    walpe_config_save (c);
    CHECK (walpe_config_save_count () == n0 + 1);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (c->focus_action == WALPE_FOCUS_LIMIT);
    CHECK (c->focus_fps == 24);
    CHECK (c->pause_battery);
    CHECK (c->battery_fps == 30);
    CHECK (c->preview_anim == WALPE_PREVIEW_HOVER);
    walpe_config_free (c);
    CHECK (walpe_focus_action_parse ("pause", -1) == WALPE_FOCUS_PAUSE);
    CHECK (walpe_focus_action_parse ("LIMIT", -1) == WALPE_FOCUS_LIMIT);
    CHECK (walpe_focus_action_parse ("nonsense", -1) == -1);
    CHECK (walpe_focus_action_parse (NULL, 0) == 0);
    CHECK (!strcmp (walpe_focus_action_name (WALPE_FOCUS_RUN), "run"));
    CHECK (!strcmp (walpe_focus_action_name (99), "pause"));    /* clamped, never out of bounds */
    CHECK (walpe_preview_anim_parse ("off", -1) == WALPE_PREVIEW_OFF);
    CHECK (!strcmp (walpe_preview_anim_name (WALPE_PREVIEW_HOVER), "hover"));
    char *f = g_build_filename (dir, "walpe", "config.json", NULL);
    g_unlink (f);
    g_free (f);
    f = g_build_filename (dir, "walpe", NULL);
    g_rmdir (f);
    g_free (f);
}

int main (void)
{
    char *dir = g_dir_make_tmp ("walpe-res-test-XXXXXX", NULL);
    test_config (dir);

    sock = g_build_filename (dir, "walpe.sock", NULL);
    loop = g_main_loop_new (NULL, FALSE);
    CHECK (walpe_ipc_serve (sock, handler, NULL));
    CHECK (walpe_ipc_subscribers () == 0);
    walpe_ipc_broadcast ("{\"nobody\":1}");     /* no subscriber: a no-op */
    GThread *t = g_thread_new ("client", client_thread, NULL);
    g_main_loop_run (loop);
    g_thread_join (t);

    /* the client module: first line, then a push */
    guint to = g_timeout_add_seconds (15, give_up, NULL);
    sub = walpe_ipc_sub_new (sock, on_sub, NULL);
    g_main_loop_run (loop);
    CHECK (sub_lines == 2 && sub_lost == 0);
    walpe_ipc_sub_free (sub);
    /* a server that cannot push ("unknown cmd"): reported, not retried in a loop */
    can_push = FALSE;
    sub_lines = sub_lost = 0;
    sub = walpe_ipc_sub_new (sock, on_sub, NULL);
    g_main_loop_run (loop);
    CHECK (sub_lost == 1 && sub_lines == 0);
    CHECK (walpe_ipc_sub_unsupported (sub) && !walpe_ipc_sub_live (sub));
    walpe_ipc_sub_free (sub);
    g_source_remove (to);
    /* let the cancelled operations finish (no use after free under ASan) */
    for (int i = 0; i < 50; i++)
        g_main_context_iteration (NULL, FALSE);

    walpe_ipc_release (sock);
    char *lock = g_strconcat (sock, ".lock", NULL);
    g_unlink (lock);
    g_rmdir (dir);
    g_free (lock);
    g_free (sock);
    g_free (dir);
    if (fails)
        g_printerr ("respolicy-test: %d check(s) failed\n", fails);
    else
        g_print ("respolicy-test: ok\n");
    return fails ? 1 : 0;
}
