/* fuzz_ipc - the control socket as another local process can talk to it
 * (src/common/ipc.c: connections, request lines, the request limit, NUL
 * bytes and text that is not UTF-8, replies to a client that went away,
 * subscriptions and state pushes) and what the daemon does with a request
 * line first (the JSON reading of src/common/util.c, the member and type
 * check of a "set" request in src/common/setkeys.h). A local trust
 * boundary: the socket is the user's own, a sandboxed program of the user
 * may still reach it.
 * The input is what one client sends, byte for byte; a real server runs on
 * a socket in the harness's directory, the handler is the front of the
 * daemon's (its commands themselves need the daemon and are not here). */
#include "fuzz_common.h"
#include "common/ipc.h"
#include "common/setkeys.h"
#include "common/util.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

static char *sock;
static int requests;

static char *handler (const char *req, gpointer user)
{
    (void) user;
    requests++;
    JsonNode *root = wpj_parse_data (req, -1);
    if (!root || json_node_get_node_type (root) != JSON_NODE_OBJECT) {
        if (root)
            json_node_unref (root);
        return g_strdup ("{\"ok\":false,\"error\":\"bad json\"}");
    }
    JsonObject *o = json_node_get_object (root);
    const char *cmd = wpj_str (o, "cmd", "");
    char *reply = NULL;
    if (!strcmp (cmd, "set")) {
        char *why = walpe_set_check (o);
        char *esc = walpe_json_escape (why);
        reply = why ? g_strdup_printf ("{\"ok\":false,\"error\":\"%s\"}", esc) : g_strdup ("{\"ok\":true}");
        g_free (esc);
        g_free (why);
    } else if (!strcmp (cmd, "subscribe")) {
        reply = g_strdup (walpe_ipc_subscribe_current () ? "{\"ok\":true}" : "{\"ok\":false}");
        walpe_ipc_broadcast ("{\"event\":\"state\"}");
    } else if (!strcmp (cmd, "big")) {
        /* a reply larger than a socket buffer, to a client that may be gone */
        GString *g = g_string_new ("{\"ok\":true,\"pad\":\"");
        for (int i = 0; i < 40000; i++)
            g_string_append (g, "0123456789");
        g_string_append (g, "\"}");
        reply = g_string_free (g, FALSE);
    } else if (!strcmp (cmd, "none")) {
        reply = NULL;       /* the daemon's handler never returns this; the server has an answer for it */
    } else {
        char *esc = walpe_json_escape (cmd);
        reply = g_strdup_printf ("{\"ok\":false,\"error\":\"unknown command %s\"}", esc);
        g_free (esc);
    }
    json_node_unref (root);
    return reply;
}

static char *own_dir;      /* made here, when the usual one has too long a name */

static void tidy (void)
{
    unlink (sock);
    if (own_dir)
        rmdir (own_dir);
    g_free (own_dir);
    g_free (sock);
}

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    const char *d = fuzz_dir ("ipc");
    if (strlen (d) > 80)      /* a unix socket's address holds 107 bytes */
        d = own_dir = g_dir_make_tmp ("walpe-fuzz-ipc-XXXXXX", NULL);
    sock = g_build_filename (d, "s", NULL);
    if (!walpe_ipc_serve (sock, handler, NULL)) {
        g_printerr ("fuzz_ipc: cannot serve on %s\n", sock);
        exit (1);
    }
    atexit (tidy);
    return 0;
}

static void pump (int rounds)
{
    for (int i = 0; i < rounds; i++)
        if (!g_main_context_iteration (NULL, FALSE))
            break;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    g_strlcpy (a.sun_path, sock, sizeof a.sun_path);
    int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect (fd, (struct sockaddr *) &a, sizeof a) != 0) {
        if (fd >= 0)
            close (fd);
        pump (64);      /* (the queue of the listener was full: let the server take them) */
        return 0;
    }
    /* the last byte says how the client behaves: reads its replies or not,
     * closes at once or after the server has answered */
    int mode = size ? data[size - 1] : 0;
    size_t sent = 0;
    char buf[65536];
    int idle = 0;
    requests = 0;
    for (int round = 0; round < 4000 && idle < 50; round++) {
        gboolean did = FALSE;
        if (sent < size) {
            ssize_t w = send (fd, data + sent, MIN (size - sent, (size_t) 4096), MSG_NOSIGNAL);
            if (w > 0) {
                sent += (size_t) w;
                did = TRUE;
            } else if (w < 0 && errno != EAGAIN) {
                sent = size;    /* the server closed the connection */
            }
            if (sent == size && !(mode & 1))
                shutdown (fd, SHUT_WR);
        }
        if (!(mode & 2)) {
            ssize_t r = recv (fd, buf, sizeof buf, 0);
            if (r > 0)
                did = TRUE;
            else if (r == 0)
                break;          /* closed by the server */
        }
        while (g_main_context_iteration (NULL, FALSE))
            did = TRUE;
        idle = did ? 0 : idle + 1;
        if ((mode & 4) && sent == size && requests)
            break;              /* gone while a reply may still be on its way */
    }
    close (fd);
    /* the server notices, frees the session; nothing of it stays */
    pump (256);
    return 0;
}
