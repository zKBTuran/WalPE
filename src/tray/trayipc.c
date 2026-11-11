/* trayipc.c - asynchronous walpe-daemon IPC for the tray
 *
 * Same wire protocol as walpe-ctl (common/ipc.c): connect to
 * $XDG_RUNTIME_DIR/walpe.sock, write one JSON line, read one JSON line.
 * Never blocks the tray's main loop: GIO async connect/write/read with a
 * cancel timer, so a hung daemon cannot freeze the menu.
 */
#include "trayipc.h"
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>

#include "common/config.h"

typedef struct {
    TrayIpcCb cb;
    gpointer user;
    char *req;
    GSocketConnection *conn;
    GDataInputStream *in;
    GCancellable *cancel;
    guint timer;
    gboolean done;
} Req;

static void req_finish (Req *r, char *reply)
{
    if (r->done) {
        g_free (reply);
        return;
    }
    r->done = TRUE;
    if (r->timer) {
        g_source_remove (r->timer);
        r->timer = 0;
    }
    if (r->cb)
        r->cb (reply, r->user);
    g_free (reply);
    if (r->in)
        g_object_unref (r->in);
    if (r->conn) {
        g_io_stream_close (G_IO_STREAM (r->conn), NULL, NULL);
        g_object_unref (r->conn);
    }
    g_object_unref (r->cancel);
    g_free (r->req);
    g_free (r);
}

static gboolean on_timeout (gpointer user)
{
    Req *r = user;
    r->timer = 0;
    /* the pending async op completes with G_IO_ERROR_CANCELLED and
     * finishes the request (NULL reply) from its callback */
    g_cancellable_cancel (r->cancel);
    return G_SOURCE_REMOVE;
}

static void on_line (GObject *src, GAsyncResult *res, gpointer user)
{
    Req *r = user;
    gsize len = 0;
    char *line = g_data_input_stream_read_line_finish_utf8 (G_DATA_INPUT_STREAM (src), res, &len, NULL);
    req_finish (r, line);
}

static void on_written (GObject *src, GAsyncResult *res, gpointer user)
{
    Req *r = user;
    if (!g_output_stream_write_all_finish (G_OUTPUT_STREAM (src), res, NULL, NULL)) {
        req_finish (r, NULL);
        return;
    }
    r->in = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (r->conn)));
    g_data_input_stream_read_line_async (r->in, G_PRIORITY_DEFAULT, r->cancel, on_line, r);
}

static void on_connected (GObject *src, GAsyncResult *res, gpointer user)
{
    Req *r = user;
    r->conn = g_socket_client_connect_finish (G_SOCKET_CLIENT (src), res, NULL);
    g_object_unref (src);
    if (!r->conn) {
        req_finish (r, NULL);
        return;
    }
    g_output_stream_write_all_async (g_io_stream_get_output_stream (G_IO_STREAM (r->conn)),
                                     r->req, strlen (r->req), G_PRIORITY_DEFAULT, r->cancel,
                                     on_written, r);
}

void tray_ipc (const char *json, int timeout_ms, TrayIpcCb cb, gpointer user)
{
    Req *r = g_new0 (Req, 1);
    r->cb = cb;
    r->user = user;
    r->req = g_strconcat (json, "\n", NULL);
    r->cancel = g_cancellable_new ();
    r->timer = g_timeout_add (timeout_ms > 0 ? timeout_ms : 1500, on_timeout, r);
    char *path = walpe_socket_path ();
    GSocketAddress *addr = g_unix_socket_address_new (path);
    g_free (path);
    GSocketClient *c = g_socket_client_new ();
    g_socket_client_connect_async (c, G_SOCKET_CONNECTABLE (addr), r->cancel, on_connected, r);
    g_object_unref (addr);
}
