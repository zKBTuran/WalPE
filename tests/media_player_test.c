/* media_player_test - what a music player on the session bus can make the
 * media integration hold (src/common/media.c).
 *
 * The test starts a bus of its own (GTestDBus: a dbus-daemon only this
 * process talks to; skipped with 77 where there is no dbus-daemon) and is
 * the player on it: org.mpris.MediaPlayer2.walpetest, with the Player
 * interface's properties and PropertiesChanged signals of its own making.
 *
 *  1. the player is found and its metadata published;
 *  2. metadata with the same key many times over, megabytes each (a
 *     dictionary on the bus may repeat a key): the memory of this process
 *     does not grow with the number of such messages - every repeated
 *     value used to stay allocated;
 *  3. a title of a megabyte is published as a title of at most 4096 bytes,
 *     cut between two characters; an ordinary one as it is.
 *
 * No display. Takes a few seconds. */
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include "common/media.h"

/* The check of resident memory (part 2): not in a build with the address
 * sanitizer. Its allocator keeps freed memory back for a while (the
 * quarantine, 256 MiB by default), so what the daemon frees stays resident
 * there and the number says nothing; a leak is LeakSanitizer's to report
 * when the test ends. */
#if defined (__SANITIZE_ADDRESS__)
#  define RSS_SAYS_SOMETHING 0
#elif defined (__has_feature)
#  if __has_feature (address_sanitizer)
#    define RSS_SAYS_SOMETHING 0
#  endif
#endif
#ifndef RSS_SAYS_SOMETHING
#  define RSS_SAYS_SOMETHING 1
#endif

#define NAME "org.mpris.MediaPlayer2.walpetest"
#define PATH "/org/mpris/MediaPlayer2"
#define IFACE "org.mpris.MediaPlayer2.Player"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char xml[] =
    "<node><interface name='" IFACE "'>"
    " <property name='PlaybackStatus' type='s' access='read'/>"
    " <property name='Metadata' type='a{sv}' access='read'/>"
    " <property name='Position' type='x' access='read'/>"
    " <property name='Rate' type='d' access='read'/>"
    "</interface></node>";

static GVariant *get_prop (GDBusConnection *c, const char *sender, const char *path, const char *iface,
                           const char *prop, GError **err, gpointer user)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) err; (void) user;
    if (!strcmp (prop, "PlaybackStatus"))
        return g_variant_new_string ("Playing");
    if (!strcmp (prop, "Position"))
        return g_variant_new_int64 (0);
    if (!strcmp (prop, "Rate"))
        return g_variant_new_double (1.0);
    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&b, "{sv}", "xesam:title", g_variant_new_string ("first"));
    return g_variant_builder_end (&b);
}

static const GDBusInterfaceVTable vt = { NULL, get_prop, NULL, { 0 } };

/* PropertiesChanged with a Metadata dictionary: `copies` values of `size`
 * bytes under the key "xesam:title", then the title that counts */
static void send_metadata (GDBusConnection *c, int copies, gsize size, const char *title)
{
    GVariantBuilder md, ch;
    g_variant_builder_init (&md, G_VARIANT_TYPE ("a{sv}"));
    char *big = g_malloc (size + 1);
    memset (big, 'x', size);
    big[size] = 0;
    for (int i = 0; i < copies; i++) {
        g_variant_builder_add (&md, "{sv}", "xesam:title", g_variant_new_string (big));
        g_variant_builder_add (&md, "{sv}", "xesam:artist", g_variant_new_string (big));
    }
    g_free (big);
    g_variant_builder_add (&md, "{sv}", "xesam:title", g_variant_new_string (title));
    g_variant_builder_add (&md, "{sv}", "xesam:artist", g_variant_new_string ("somebody"));
    g_variant_builder_init (&ch, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&ch, "{sv}", "Metadata", g_variant_builder_end (&md));
    GError *err = NULL;
    g_dbus_connection_emit_signal (c, NULL, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                   g_variant_new ("(sa{sv}as)", IFACE, &ch, NULL), &err);
    CHECK (!err);
    g_clear_error (&err);
    g_dbus_connection_flush_sync (c, NULL, NULL);
}

/* wait until the published title begins with `prefix` (seconds at most);
 * its length, -1 = it never came */
static long wait_title (const char *prefix, int seconds)
{
    gint64 end = g_get_monotonic_time () + (gint64) seconds * G_USEC_PER_SEC;
    long len = -1;
    while (len < 0 && g_get_monotonic_time () < end) {
        g_main_context_iteration (NULL, FALSE);
        WalpeNowPlaying np;
        walpe_media_get (&np);
        if (np.title && g_str_has_prefix (np.title, prefix))
            len = (long) strlen (np.title);
        walpe_media_now_clear (&np);
        if (len < 0)
            g_usleep (2000);
    }
    return len;
}

static long rss_kb (void)
{
    char *txt = NULL;
    long kb = 0;
    if (g_file_get_contents ("/proc/self/status", &txt, NULL, NULL)) {
        const char *p = strstr (txt, "VmRSS:");
        if (p)
            kb = strtol (p + 6, NULL, 10);
    }
    g_free (txt);
    return kb;
}

int main (void)
{
    char *dbus_daemon = g_find_program_in_path ("dbus-daemon");
    if (!dbus_daemon) {
        printf ("media-player: skipped (no dbus-daemon)\n");
        return 77;
    }
    g_free (dbus_daemon);
    g_unsetenv ("WALPE_NO_MEDIA");
    GTestDBus *tb = g_test_dbus_new (G_TEST_DBUS_NONE);
    g_test_dbus_up (tb);
    GError *err = NULL;
    /* the player's own connection (the media thread has the process's) */
    GDBusConnection *c = g_dbus_connection_new_for_address_sync (
        g_test_dbus_get_bus_address (tb),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &err);
    if (!c) {
        printf ("media-player: skipped (no connection to the test bus: %s)\n", err ? err->message : "?");
        g_test_dbus_down (tb);
        return 77;
    }
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml (xml, NULL);
    g_dbus_connection_register_object (c, PATH, info->interfaces[0], &vt, NULL, NULL, NULL);
    GVariant *own = g_dbus_connection_call_sync (c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                 "org.freedesktop.DBus", "RequestName",
                                                 g_variant_new ("(su)", NAME, 0), NULL, G_DBUS_CALL_FLAGS_NONE,
                                                 5000, NULL, NULL);
    CHECK (own != NULL);
    if (own)
        g_variant_unref (own);

    /* 1. found, published */
    walpe_media_ref ();
    CHECK (wait_title ("first", 20) == 5);

    /* 2. the same key again and again: 40 messages, each with 2 x 8 values
     * of 256 KiB that are replaced within the message (160 MiB in all) */
    send_metadata (c, 1, 1024, "warm");
    CHECK (wait_title ("warm", 20) == 4);
    long r0 = rss_kb ();
    for (int i = 0; i < 40; i++) {
        char t[32];
        g_snprintf (t, sizeof t, "round %d", i);
        send_metadata (c, 8, 256 * 1024, t);
        CHECK (wait_title (t, 20) > 0);
    }
    long r1 = rss_kb ();
    if (r1 - r0 > 48 * 1024)
        fprintf (stderr, "media-player: 40 messages with repeated keys: %ld MiB more memory%s\n", (r1 - r0) / 1024,
                 RSS_SAYS_SOMETHING ? "" : " (not checked: a sanitizer's allocator holds freed memory back)");
    CHECK (!RSS_SAYS_SOMETHING || r1 - r0 <= 48 * 1024);

    /* 3. a title of a megabyte ("ä" is two bytes: the cut is between two) */
    {
        GString *t = g_string_new ("long ");
        while (t->len < (1u << 20))
            g_string_append (t, "\xc3\xa4");
        send_metadata (c, 0, 0, t->str);
        g_string_free (t, TRUE);
        long len = wait_title ("long ", 20);
        CHECK (len > 4000 && len <= 4096);
        WalpeNowPlaying np;
        walpe_media_get (&np);
        CHECK (np.title && g_utf8_validate (np.title, -1, NULL));
        CHECK (np.artist && !strcmp (np.artist, "somebody"));
        walpe_media_now_clear (&np);
        send_metadata (c, 0, 0, "A title, as they are");
        CHECK (wait_title ("A title", 20) == (long) strlen ("A title, as they are"));
    }

    walpe_media_unref ();
    g_dbus_connection_close_sync (c, NULL, NULL);
    g_object_unref (c);
    g_dbus_node_info_unref (info);
    g_usleep (300000);      /* (the media thread lets go of the bus before it is taken down) */
    if (fails)
        fprintf (stderr, "media-player: %d check(s) failed\n", fails);
    else
        printf ("media-player: ok\n");
    fflush (NULL);
    /* (not g_test_dbus_down: it waits for every connection to the bus to
     * be gone, and the media thread keeps the process's for its grace time.
     * That connection is the shared one, which ends its process with
     * SIGTERM when its bus goes away: not here, where the bus is stopped
     * on purpose a moment before the test ends by itself - whichever of
     * the two came first decided between "ok" and "killed by a signal") */
    {
        GDBusConnection *shared = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
        if (shared) {
            g_dbus_connection_set_exit_on_close (shared, FALSE);
            g_object_unref (shared);
        }
    }
    g_test_dbus_stop (tb);
    _exit (fails ? 1 : 0);
}
