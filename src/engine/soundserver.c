/* soundserver.c - see soundserver.h */
#include "soundserver.h"
#include "common/util.h"
#include <errno.h>
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* a stream of WebKit's page process (the name WebKitGTK gives its
 * connection, and the program that holds it) */
static gboolean stream_is_webkit (JsonObject *props)
{
    const char *name = props ? wpj_str (props, "application.name", NULL) : NULL;
    const char *bin = props ? wpj_str (props, "application.process.binary", NULL) : NULL;
    return !g_strcmp0 (name, "WebKit") || (bin && g_str_has_prefix (bin, "WebKitWebProcess"));
}

/* every channel of the stream is at 0 */
static gboolean stream_at_zero (JsonObject *s)
{
    JsonObject *vol = wpj_obj (s, "volume");
    GList *chans = vol ? json_object_get_members (vol) : NULL;
    gboolean any = FALSE, zero = TRUE;
    for (GList *c = chans; c; c = c->next) {
        JsonNode *n = json_object_get_member (vol, c->data);
        if (!n || !JSON_NODE_HOLDS_OBJECT (n))
            continue;
        any = TRUE;
        zero = zero && wpj_num (json_node_get_object (n), "value", 1.0) <= 0.0;
    }
    g_list_free (chans);
    return any && zero;
}

char *walpe_sound_server_note_from_list (const char *pactl_json)
{
    if (!pactl_json || !*pactl_json)
        return NULL;
    JsonParser *p = json_parser_new ();
    char *note = NULL;
    if (json_parser_load_from_data (p, pactl_json, -1, NULL)) {
        JsonNode *root = json_parser_get_root (p);
        JsonArray *list = root && JSON_NODE_HOLDS_ARRAY (root) ? json_node_get_array (root) : NULL;
        int streams = 0, muted = 0, zero = 0;
        gint64 first = -1;
        for (guint i = 0; list && i < json_array_get_length (list); i++) {
            JsonNode *n = json_array_get_element (list, i);
            JsonObject *s = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
            if (!s || !stream_is_webkit (wpj_obj (s, "properties")))
                continue;
            streams++;
            const gboolean m = wpj_bool (s, "mute", FALSE), z = stream_at_zero (s);
            muted += m;
            zero += z && !m;
            if ((m || z) && first < 0)
                first = (gint64) wpj_num (s, "index", -1);
        }
        if (muted + zero > 0) {
            GString *t = g_string_new (NULL);
            g_string_printf (t, "the sound server holds %d of %d stream(s) of WebKit ", muted + zero, streams);
            if (muted && zero)
                g_string_append_printf (t, "silent (%d muted, %d at 0 %%)", muted, zero);
            else
                g_string_append (t, muted ? "muted" : "at 0 %");
            g_string_append (t, ": a web wallpaper's sound is such a stream, and it stays silent whatever "
                             "the settings here say - WalPE never changes a stream in the server, and the "
                             "server gives the stored value to every later one. Undo it once, while the "
                             "page plays: in the desktop's mixer (Plasma: the volume applet, Applications; "
                             "pavucontrol: Playback) unmute \"WebKit\" and turn it up");
            if (first >= 0)
                g_string_append_printf (t, ", or: pactl set-sink-input-mute %" G_GINT64_FORMAT
                                        " 0; pactl set-sink-input-volume %" G_GINT64_FORMAT " 100%%",
                                        first, first);
            note = g_string_free (t, FALSE);
        }
    }
    g_object_unref (p);
    return note;
}

char *walpe_sound_server_note (gboolean *asked)
{
    char *argv[] = { (char *) "pactl", (char *) "-f", (char *) "json", (char *) "list",
                     (char *) "sink-inputs", NULL };
    GPid pid = 0;
    int ofd = -1, status = -1;
    if (asked)
        *asked = FALSE;
    if (!g_spawn_async_with_pipes (NULL, argv, NULL,
                                   G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL,
                                   NULL, NULL, &pid, NULL, &ofd, NULL, NULL))
        return NULL;
    GString *out = g_string_new (NULL);
    const gint64 t0 = g_get_monotonic_time ();
    gboolean done = FALSE, late = FALSE;
    fcntl (ofd, F_SETFL, fcntl (ofd, F_GETFL) | O_NONBLOCK);
    while (!done) {
        char buf[4096];
        ssize_t n;
        while ((n = read (ofd, buf, sizeof buf)) > 0)
            if (out->len < (1u << 20))      /* (a list of streams is a few kB) */
                g_string_append_len (out, buf, n);
        pid_t r = waitpid (pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) {
            done = TRUE;
        } else if (g_get_monotonic_time () - t0 > 1500 * 1000) {
            kill (pid, SIGKILL);
            while (waitpid (pid, &status, 0) < 0 && errno == EINTR)
                ;
            done = late = TRUE;
        } else {
            g_usleep (5000);
        }
    }
    {
        char buf[4096];
        ssize_t n;
        while ((n = read (ofd, buf, sizeof buf)) > 0)
            if (out->len < (1u << 20))
                g_string_append_len (out, buf, n);
    }
    close (ofd);
    g_spawn_close_pid (pid);
    char *note = NULL;
    if (!late && WIFEXITED (status) && WEXITSTATUS (status) == 0 && out->len && out->str[0] == '[') {
        if (asked)
            *asked = TRUE;
        note = walpe_sound_server_note_from_list (out->str);
    }
    g_string_free (out, TRUE);
    return note;
}
