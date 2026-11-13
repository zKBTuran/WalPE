/* kwin_bus - the session-bus side of scripts/kwincheck.py (the test
 * `daemon-kwin`): what the check asks KWin, Plasma's shell and the tray's
 * item over D-Bus, without a D-Bus module for Python and without the
 * command line tools of GLib, Qt or systemd.
 *
 *   kwin-bus call DEST PATH INTERFACE METHOD [ARGS]
 *       one method call on the session bus. ARGS is the tuple of the
 *       arguments in GVariant's text form, e.g. "('/tmp/a.js', 'name')" or
 *       "(0, -1, @as [])"; the reply is printed as one line of JSON (a
 *       tuple is an array, a dictionary an object, a variant its value).
 *   kwin-bus shot [OUTPUT]
 *       the picture KWin shows on the output of that name (all outputs
 *       without a name), through KWin's own interface
 *       org.kde.KWin.ScreenShot2, as a binary PPM on standard output.
 *       KWin answers only callers its desktop file names, or everybody when
 *       it was started with KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1, as the
 *       check starts it.
 *
 * Exit status 0, 1 = the call failed (the reason on standard error), 2 =
 * usage. DBUS_SESSION_BUS_ADDRESS names the bus. */
#include <errno.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TIMEOUT_MS 30000

static int fail (const char *what, GError *e)
{
    fprintf (stderr, "kwin-bus: %s: %s\n", what, e ? e->message : "failed");
    g_clear_error (&e);
    return 1;
}

static int do_call (GDBusConnection *bus, int argc, char **argv)
{
    GError *e = NULL;
    GVariant *args = NULL;
    if (argc > 4) {
        args = g_variant_parse (NULL, argv[4], NULL, NULL, &e);
        if (!args)
            return fail ("the arguments are no GVariant text", e);
        if (!g_variant_is_of_type (args, G_VARIANT_TYPE_TUPLE)) {
            fprintf (stderr, "kwin-bus: the arguments must be one tuple, e.g. \"('text', 1)\"\n");
            return 2;
        }
    }
    GVariant *r = g_dbus_connection_call_sync (bus, argv[0], argv[1], argv[2], argv[3], args, NULL,
                                               G_DBUS_CALL_FLAGS_NO_AUTO_START, TIMEOUT_MS, NULL, &e);
    if (!r)
        return fail (argv[3], e);
    JsonNode *node = json_gvariant_serialize (r);
    char *text = json_to_string (node, FALSE);
    puts (text);
    g_free (text);
    json_node_unref (node);
    g_variant_unref (r);
    return 0;
}

/* QImage::Format, the numbers KWin names its pixels by */
enum { QIMG_RGB32 = 4, QIMG_ARGB32 = 5, QIMG_ARGB32_PRE = 6, QIMG_RGBX8888 = 16, QIMG_RGBA8888 = 17,
       QIMG_RGBA8888_PRE = 18 };

static int do_shot (GDBusConnection *bus, const char *output)
{
    GError *e = NULL;
    int fds[2];
    if (pipe (fds) < 0) {
        fprintf (stderr, "kwin-bus: pipe: %s\n", g_strerror (errno));
        return 1;
    }
    GUnixFDList *list = g_unix_fd_list_new ();
    int handle = g_unix_fd_list_append (list, fds[1], &e);
    close (fds[1]);
    if (handle < 0) {
        close (fds[0]);
        g_object_unref (list);
        return fail ("the pipe for the picture", e);
    }
    GVariantBuilder opts;
    g_variant_builder_init (&opts, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&opts, "{sv}", "native-resolution", g_variant_new_boolean (TRUE));
    GVariant *args = output && *output ? g_variant_new ("(sa{sv}h)", output, &opts, handle)
                                       : g_variant_new ("(a{sv}h)", &opts, handle);
    GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync (
        bus, "org.kde.KWin", "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2",
        output && *output ? "CaptureScreen" : "CaptureWorkspace", args, G_VARIANT_TYPE ("(a{sv})"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, TIMEOUT_MS, list, NULL, NULL, &e);
    g_object_unref (list);     /* (our copy of the writing end goes with it) */
    if (!r) {
        close (fds[0]);
        return fail ("org.kde.KWin.ScreenShot2", e);
    }
    GVariant *d = g_variant_get_child_value (r, 0);
    guint32 w = 0, h = 0, stride = 0, format = 0;
    const char *type = "";
    g_variant_lookup (d, "type", "&s", &type);
    g_variant_lookup (d, "width", "u", &w);
    g_variant_lookup (d, "height", "u", &h);
    g_variant_lookup (d, "stride", "u", &stride);
    g_variant_lookup (d, "format", "u", &format);
    gboolean bgr = format == QIMG_RGB32 || format == QIMG_ARGB32 || format == QIMG_ARGB32_PRE;
    gboolean rgb = format == QIMG_RGBX8888 || format == QIMG_RGBA8888 || format == QIMG_RGBA8888_PRE;
    int rc = 1;
    if (strcmp (type, "raw") || !w || !h || w > 16384 || h > 16384 || stride < 4 * w || (!bgr && !rgb)) {
        fprintf (stderr, "kwin-bus: a picture this program cannot read: type \"%s\", %ux%u, stride %u, "
                         "QImage format %u\n", type, w, h, stride, format);
    } else {
        gsize need = (gsize) stride * h, got = 0;
        guchar *raw = g_malloc (need);
        while (got < need) {
            gssize n = read (fds[0], raw + got, need - got);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            got += (gsize) n;
        }
        if (got < need) {
            fprintf (stderr, "kwin-bus: KWin sent %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " bytes of the picture\n",
                     got, need);
        } else {
            guchar *out = g_malloc ((gsize) w * h * 3), *o = out;
            for (guint32 y = 0; y < h; y++) {
                const guchar *p = raw + (gsize) y * stride;
                for (guint32 x = 0; x < w; x++, p += 4, o += 3) {
                    o[0] = bgr ? p[2] : p[0];
                    o[1] = p[1];
                    o[2] = bgr ? p[0] : p[2];
                }
            }
            printf ("P6\n%u %u\n255\n", w, h);
            rc = fwrite (out, 3, (gsize) w * h, stdout) == (gsize) w * h && fflush (stdout) == 0 ? 0 : 1;
            g_free (out);
        }
        g_free (raw);
    }
    close (fds[0]);
    g_variant_unref (d);
    g_variant_unref (r);
    return rc;
}

int main (int argc, char **argv)
{
    gboolean call = argc >= 6 && argc <= 7 && !strcmp (argv[1], "call");
    gboolean shot = argc >= 2 && argc <= 3 && !strcmp (argv[1], "shot");
    if (!call && !shot) {
        fprintf (stderr, "usage: kwin-bus call DEST PATH INTERFACE METHOD [ARGS]\n"
                         "       kwin-bus shot [OUTPUT]\n");
        return 2;
    }
    GError *e = NULL;
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &e);
    if (!bus)
        return fail ("no session bus", e);
    int rc = call ? do_call (bus, argc - 2, argv + 2) : do_shot (bus, argc > 2 ? argv[2] : NULL);
    g_object_unref (bus);
    return rc;
}
