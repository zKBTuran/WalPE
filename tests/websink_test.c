/* websink_test.c - the sound output of the page process of web wallpapers
 * (gst/walpesound.c), as GStreamer's playback elements will meet it:
 *
 *   - the plugin in the build's gst/ directory loads and has the element;
 *   - autoaudiosink will take it: an audio sink, raw audio, a rank above
 *     pulsesink's;
 *   - the search playbin (playsink) makes for an element that keeps a
 *     volume ends at the "volume" element of the plugin, not at pulsesink -
 *     also when the element sits inside another bin, as it does inside
 *     autoaudiosink. That is the whole point: level and mute are then a
 *     multiplication in the page process and never the volume and mute of
 *     the stream in the sound server;
 *   - a level and a mute set there leave pulsesink's own at 1 and unmuted;
 *   - what WebKit sets on the sink (stream-properties with the media.role,
 *     client-name) reaches pulsesink.
 *
 * No state is changed, so no sound server is contacted; no display. A
 * registry of its own (GST_REGISTRY_1_0, meson.build): the user's is not
 * touched. Exit 77 (skipped) where GStreamer has no pulsesink, volume or
 * audioconvert. */
#include <gst/gst.h>
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fails++;                                                          \
            fprintf (stderr, "FAIL %s:%d: (%s) ", __FILE__, __LINE__, #cond); \
            fprintf (stderr, __VA_ARGS__);                                    \
            fputc ('\n', stderr);                                             \
        }                                                                     \
    } while (0)

static guint rank_of (const char *name)
{
    GstElementFactory *f = gst_element_factory_find (name);
    guint r = f ? gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (f)) : 0;
    if (f)
        gst_object_unref (f);
    return r;
}

int main (int argc, char **argv)
{
    if (argc < 2) {
        fprintf (stderr, "usage: websink-test <directory of libgstwalpesound.so>\n");
        return 2;
    }
    gst_init (NULL, NULL);
    for (const char *const *n = (const char *const[]) { "pulsesink", "volume", "audioconvert", NULL }; *n; n++) {
        GstElementFactory *f = gst_element_factory_find (*n);
        if (!f) {
            printf ("websink: this GStreamer has no %s: skipped\n", *n);
            return 77;
        }
        gst_object_unref (f);
    }
    gst_registry_scan_path (gst_registry_get (), argv[1]);
    GstElementFactory *f = gst_element_factory_find ("walpewebsink");
    CHECK (f != NULL, "no walpewebsink after a scan of %s", argv[1]);
    if (!f)
        return 1;

    /* ---- what autoaudiosink asks of a candidate */
    const char *klass = gst_element_factory_get_metadata (f, GST_ELEMENT_METADATA_KLASS);
    CHECK (klass && strstr (klass, "Sink") && strstr (klass, "Audio"), "class \"%s\"", klass ? klass : "(none)");
    GstCaps *raw = gst_caps_new_empty_simple ("audio/x-raw");
    CHECK (gst_element_factory_can_sink_any_caps (f, raw), "takes raw audio");
    gst_caps_unref (raw);
    CHECK (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (f)) > rank_of ("pulsesink"),
           "rank %u, pulsesink %u", gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (f)), rank_of ("pulsesink"));
    for (const char *const *n = (const char *const[]) { "pipewiresink", "alsasink", "autoaudiosink", NULL }; *n; n++)
        CHECK (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (f)) > rank_of (*n), "rank above %s (%u)", *n, rank_of (*n));

    /* ---- the search for the element that keeps the volume */
    GstElement *e = gst_element_factory_create (f, NULL);
    CHECK (e != NULL, "the element is made");
    if (!e)
        return 1;
    GType keeps = g_type_from_name ("GstStreamVolume");
    CHECK (keeps != 0, "GstStreamVolume is known once the element exists");
    GstElement *out = gst_bin_get_by_name (GST_BIN (e), "out");
    GstElement *level = gst_bin_get_by_name (GST_BIN (e), "level");
    CHECK (out && level, "the parts are there");
    if (!keeps || !out || !level)
        return 1;
    CHECK (g_type_is_a (G_OBJECT_TYPE (out), keeps), "pulsesink would keep a volume too: the order matters");
    GstElement *found = gst_bin_get_by_interface (GST_BIN (e), keeps);
    CHECK (found == level, "the search ends at %s", found ? GST_OBJECT_NAME (found) : "nothing");
    g_clear_object (&found);
    GstElement *outer = gst_bin_new ("as-autoaudiosink-holds-it");
    gst_bin_add (GST_BIN (outer), e);
    found = gst_bin_get_by_interface (GST_BIN (outer), keeps);
    CHECK (found == level, "inside another bin the search ends at %s", found ? GST_OBJECT_NAME (found) : "nothing");
    g_clear_object (&found);

    /* ---- a level and a mute set there are not pulsesink's */
    double v = 0, pv = 0;
    gboolean m = FALSE, pm = TRUE;
    g_object_set (level, "volume", 0.25, "mute", TRUE, NULL);
    g_object_get (level, "volume", &v, "mute", &m, NULL);
    g_object_get (out, "volume", &pv, "mute", &pm, NULL);
    CHECK (v > 0.249 && v < 0.251 && m, "the level element has them (%g, %d)", v, m);
    CHECK (pv > 0.999 && pv < 1.001 && !pm, "pulsesink is at %g, mute %d", pv, pm);

    /* ---- what WebKit sets on the sink it was given */
    GstStructure *props = gst_structure_new ("stream-properties", "media.role", G_TYPE_STRING, "music", NULL), *got = NULL;
    char *name = NULL;
    g_object_set (e, "stream-properties", props, "client-name", "websink-test", NULL);
    g_object_get (out, "stream-properties", &got, "client-name", &name, NULL);
    CHECK (got && !g_strcmp0 (gst_structure_get_string (got, "media.role"), "music"), "media.role reaches pulsesink");
    CHECK (!g_strcmp0 (name, "websink-test"), "client-name reaches pulsesink (%s)", name ? name : "(none)");
    gst_structure_free (props);
    if (got)
        gst_structure_free (got);
    g_free (name);

    gst_object_unref (out);
    gst_object_unref (level);
    gst_object_unref (outer);
    gst_object_unref (f);
    if (fails) {
        fprintf (stderr, "websink: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("websink: ok\n");
    return 0;
}
