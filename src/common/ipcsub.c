/* ipcsub.c - see ipcsub.h */
#include "ipcsub.h"
#include <gio/gio.h>
#include <string.h>

struct WalpeIpcSub {
    char *path;
    WalpeIpcSubCb cb;
    gpointer user;
    GSocketConnection *conn;
    GDataInputStream *in;
    GCancellable *cancel;       /* of the connection attempt / the read */
    GFileMonitor *mon;          /* the socket's directory */
    guint retry_id;
    guint retry_n;
    gboolean connecting;
    gboolean live;              /* the "subscribe" reply arrived */
    gboolean unsupported;
    gboolean freed;
    guint refs;                 /* async operations in flight + the owner */
};

static void sub_connect (WalpeIpcSub *s);

static void sub_unref (WalpeIpcSub *s)
{
    if (--s->refs)
        return;
    g_free (s->path);
    g_free (s);
}

static void sub_close (WalpeIpcSub *s)
{
    if (s->cancel) {
        g_cancellable_cancel (s->cancel);
        g_clear_object (&s->cancel);
    }
    g_clear_object (&s->in);
    if (s->conn) {
        g_io_stream_close (G_IO_STREAM (s->conn), NULL, NULL);
        g_clear_object (&s->conn);
    }
    s->connecting = FALSE;
}

static gboolean sub_retry_cb (gpointer user)
{
    WalpeIpcSub *s = user;
    s->retry_id = 0;
    sub_connect (s);
    return G_SOURCE_REMOVE;
}

/* a few quick tries (a daemon that is just starting has bound its socket
 * but may not accept yet), then nothing until the directory watch sees
 * the socket again: no standing timer */
static void sub_retry (WalpeIpcSub *s)
{
    static const guint delay_ms[] = { 300, 1000, 3000 };
    if (s->freed || s->retry_id || s->retry_n >= G_N_ELEMENTS (delay_ms))
        return;
    s->retry_id = g_timeout_add (delay_ms[s->retry_n++], sub_retry_cb, s);
}

static void sub_lost (WalpeIpcSub *s)
{
    gboolean was_live = s->live;
    s->live = FALSE;
    sub_close (s);
    if (s->freed)
        return;
    if (was_live) {
        s->retry_n = 0;
        s->cb (NULL, s->user);
    }
    if (!s->freed)
        sub_retry (s);
}

static void sub_line (GObject *src, GAsyncResult *res, gpointer user)
{
    WalpeIpcSub *s = user;
    gsize len = 0;
    char *line = g_data_input_stream_read_line_finish_utf8 (G_DATA_INPUT_STREAM (src), res, &len, NULL);
    if (s->freed || (GDataInputStream *) src != s->in) {
        g_free (line);
        sub_unref (s);
        return;
    }
    if (!line) {
        sub_lost (s);
        sub_unref (s);
        return;
    }
    if (!s->live) {
        /* the reply to "subscribe" */
        if (!strstr (line, "\"subscribed\":true")) {
            /* "unknown cmd": a daemon from before this existed */
            s->unsupported = TRUE;
            g_free (line);
            sub_close (s);
            s->cb (NULL, s->user);
            sub_unref (s);
            return;
        }
        s->live = TRUE;
        s->unsupported = FALSE;
        s->retry_n = 0;
    }
    s->cb (line, s->user);
    g_free (line);
    if (!s->freed && s->in)
        g_data_input_stream_read_line_async (s->in, G_PRIORITY_DEFAULT, s->cancel, sub_line, s);
    else
        sub_unref (s);
}

static void sub_written (GObject *src, GAsyncResult *res, gpointer user)
{
    WalpeIpcSub *s = user;
    gboolean ok = g_output_stream_write_all_finish (G_OUTPUT_STREAM (src), res, NULL, NULL);
    if (s->freed || !s->conn ||
        (GOutputStream *) src != g_io_stream_get_output_stream (G_IO_STREAM (s->conn))) {
        sub_unref (s);
        return;
    }
    if (!ok) {
        sub_lost (s);
        sub_unref (s);
        return;
    }
    s->connecting = FALSE;
    s->in = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (s->conn)));
    g_data_input_stream_read_line_async (s->in, G_PRIORITY_DEFAULT, s->cancel, sub_line, s);
}

static void sub_connected (GObject *src, GAsyncResult *res, gpointer user)
{
    WalpeIpcSub *s = user;
    GSocketConnection *conn = g_socket_client_connect_finish (G_SOCKET_CLIENT (src), res, NULL);
    g_object_unref (src);
    if (s->freed || !s->connecting || s->conn) {
        if (conn) {
            g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
            g_object_unref (conn);
        }
        sub_unref (s);
        return;
    }
    if (!conn) {
        sub_lost (s);       /* no daemon (yet) */
        sub_unref (s);
        return;
    }
    s->conn = conn;
    static const char req[] = "{\"cmd\":\"subscribe\"}\n";
    g_output_stream_write_all_async (g_io_stream_get_output_stream (G_IO_STREAM (conn)), req,
                                     sizeof (req) - 1, G_PRIORITY_DEFAULT, s->cancel, sub_written, s);
}

static void sub_connect (WalpeIpcSub *s)
{
    if (s->freed || s->connecting || s->conn)
        return;
    s->connecting = TRUE;
    s->cancel = g_cancellable_new ();
    GSocketAddress *addr = g_unix_socket_address_new (s->path);
    GSocketClient *c = g_socket_client_new ();
    s->refs++;
    g_socket_client_connect_async (c, G_SOCKET_CONNECTABLE (addr), s->cancel, sub_connected, s);
    g_object_unref (addr);
}

static void sub_dir_changed (GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer user)
{
    (void) m; (void) o;
    WalpeIpcSub *s = user;
    if (ev != G_FILE_MONITOR_EVENT_CREATED && ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        ev != G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
        return;
    char *base = g_file_get_basename (f);
    char *want = g_path_get_basename (s->path);
    gboolean hit = !g_strcmp0 (base, want);
    g_free (base);
    g_free (want);
    if (!hit || s->live || s->connecting)
        return;
    /* the daemon's socket (re)appeared */
    s->retry_n = 0;
    if (s->retry_id)
        g_source_remove (s->retry_id);
    s->retry_id = g_timeout_add (150, sub_retry_cb, s);
}

WalpeIpcSub *walpe_ipc_sub_new (const char *sock_path, WalpeIpcSubCb cb, gpointer user)
{
    WalpeIpcSub *s = g_new0 (WalpeIpcSub, 1);
    s->refs = 1;
    s->path = g_strdup (sock_path);
    s->cb = cb;
    s->user = user;
    char *dir = g_path_get_dirname (sock_path);
    GFile *df = g_file_new_for_path (dir);
    s->mon = g_file_monitor_directory (df, G_FILE_MONITOR_NONE, NULL, NULL);
    if (s->mon)
        g_signal_connect (s->mon, "changed", G_CALLBACK (sub_dir_changed), s);
    g_object_unref (df);
    g_free (dir);
    sub_connect (s);
    return s;
}

void walpe_ipc_sub_free (WalpeIpcSub *s)
{
    if (!s)
        return;
    s->freed = TRUE;
    if (s->retry_id)
        g_source_remove (s->retry_id);
    s->retry_id = 0;
    if (s->mon) {
        g_signal_handlers_disconnect_by_data (s->mon, s);
        g_file_monitor_cancel (s->mon);
        g_clear_object (&s->mon);
    }
    sub_close (s);
    sub_unref (s);
}

gboolean walpe_ipc_sub_live (WalpeIpcSub *s)
{
    return s && s->live;
}

gboolean walpe_ipc_sub_unsupported (WalpeIpcSub *s)
{
    return s && s->unsupported;
}

void walpe_ipc_sub_kick (WalpeIpcSub *s)
{
    if (!s || s->freed || s->live || s->connecting)
        return;
    s->retry_n = 0;
    if (s->retry_id)
        g_source_remove (s->retry_id);
    s->retry_id = g_timeout_add (100, sub_retry_cb, s);
}
