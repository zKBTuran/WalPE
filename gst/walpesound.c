/* libgstwalpesound.so - the sound output of a web wallpaper's page process.
 *
 * A GStreamer plugin with one element, "walpewebsink":
 *
 *     audioconvert ! volume ! pulsesink
 *
 * It is not loaded by the daemon. The daemon puts this file's directory
 * (nothing else is in it) on GST_PLUGIN_PATH, WebKit's page process
 * inherits that, and GStreamer finds the plugin when - and only when - a
 * page plays its first sound. WebKit asks "autoaudiosink" for an output,
 * which takes the working sink of the highest rank: this one, a little
 * above pulsesink.
 *
 * Why. WebKit sets the loudness and the mute of a media element through
 * playbin, and playbin hands both to the audio sink when the sink can keep
 * them (GstStreamVolume). pulsesink can: it makes them the volume and the
 * mute of the stream IN THE SOUND SERVER. That is state outside WalPE:
 *   - the server stores it and gives it to later streams of the same kind.
 *     WirePlumber files it under the stream's media.role ("Music" for an
 *     <audio>, "Movie" for a <video>), which other programs use too: WalPE
 *     at volume 0 or muted left that role at 0 for the next start and for
 *     any other player of the role; a level of 0.5 left it at 0.5;
 *   - WebKit reads it back and writes what it reads into the element
 *     (element.muted, element.volume). With pipewire-pulse the answer right
 *     after a change can be the old one: after mute, unmute the element
 *     itself said muted = true and the stream stayed muted for good.
 * With the "volume" element in front, playbin finds that one first, and
 * level and mute are a multiplication in the page process: the stream in
 * the server is never touched by WalPE (its slider in the system mixer
 * stays the user's), nothing of WalPE is stored, nothing is read back.
 *
 * playbin takes the first element it finds in the sink that keeps a
 * volume; a bin lists its newest member first, so "volume" is added after
 * pulsesink, and the element checks that the search really ends at
 * "volume". If it does not (a GStreamer that searches differently), or a
 * part is missing, the element refuses to start and autoaudiosink goes on
 * to pulsesink itself: sound as before this file existed. */
#include <gst/gst.h>

#define PACKAGE "walpe"         /* GST_PLUGIN_DEFINE: the module the plugin is built from */

typedef struct {
    GstBin bin;
    GstElement *conv, *level, *out;
    gboolean ok;
} WalpeWebSink;

typedef struct {
    GstBinClass bin_class;
} WalpeWebSinkClass;

G_DEFINE_TYPE (WalpeWebSink, walpe_web_sink, GST_TYPE_BIN)

/* what WebKit and autoaudiosink set on the sink they found: handed on */
enum { PROP_0, PROP_STREAM_PROPERTIES, PROP_CLIENT_NAME, PROP_SYNC, PROP_TS_OFFSET };

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS ("audio/x-raw"));

static const char *prop_name (guint id)
{
    switch (id) {
    case PROP_STREAM_PROPERTIES: return "stream-properties";
    case PROP_CLIENT_NAME:       return "client-name";
    case PROP_SYNC:              return "sync";
    case PROP_TS_OFFSET:         return "ts-offset";
    default:                     return NULL;
    }
}

static void walpe_web_sink_set_property (GObject *o, guint id, const GValue *v, GParamSpec *ps)
{
    WalpeWebSink *s = (WalpeWebSink *) o;
    const char *n = prop_name (id);
    if (!n)
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
    else if (s->out)
        g_object_set_property (G_OBJECT (s->out), n, v);
}

static void walpe_web_sink_get_property (GObject *o, guint id, GValue *v, GParamSpec *ps)
{
    WalpeWebSink *s = (WalpeWebSink *) o;
    const char *n = prop_name (id);
    if (!n)
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
    else if (s->out)
        g_object_get_property (G_OBJECT (s->out), n, v);
}

static GstStateChangeReturn walpe_web_sink_change_state (GstElement *e, GstStateChange t)
{
    WalpeWebSink *s = (WalpeWebSink *) e;
    if (t == GST_STATE_CHANGE_NULL_TO_READY && !s->ok)
        return GST_STATE_CHANGE_FAILURE;        /* autoaudiosink takes the next sink */
    return GST_ELEMENT_CLASS (walpe_web_sink_parent_class)->change_state (e, t);
}

static void walpe_web_sink_class_init (WalpeWebSinkClass *k)
{
    GObjectClass *oc = G_OBJECT_CLASS (k);
    GstElementClass *ec = GST_ELEMENT_CLASS (k);
    oc->set_property = walpe_web_sink_set_property;
    oc->get_property = walpe_web_sink_get_property;
    ec->change_state = walpe_web_sink_change_state;
    g_object_class_install_property (oc, PROP_STREAM_PROPERTIES,
        g_param_spec_boxed ("stream-properties", "stream properties",
                            "handed to pulsesink", GST_TYPE_STRUCTURE,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property (oc, PROP_CLIENT_NAME,
        g_param_spec_string ("client-name", "client name", "handed to pulsesink", NULL,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property (oc, PROP_SYNC,
        g_param_spec_boolean ("sync", "sync", "handed to pulsesink", TRUE,
                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property (oc, PROP_TS_OFFSET,
        g_param_spec_int64 ("ts-offset", "ts offset", "handed to pulsesink",
                            G_MININT64, G_MAXINT64, 0,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    gst_element_class_add_static_pad_template (ec, &sink_template);
    gst_element_class_set_static_metadata (ec, "WalPE web wallpaper sound output",
        "Sink/Audio", "pulsesink with the level and the mute kept in this process",
        "WalPE");
}

static void walpe_web_sink_init (WalpeWebSink *s)
{
    s->out = gst_element_factory_make ("pulsesink", "out");
    s->conv = gst_element_factory_make ("audioconvert", "conv");
    s->level = gst_element_factory_make ("volume", "level");
    /* (each one given to the bin or let go: a floating reference either way) */
    GstElement *parts[3] = { s->out, s->conv, s->level };     /* "volume" last = found first */
    gboolean all = s->out && s->conv && s->level;
    for (int i = 0; i < 3; i++) {
        if (!parts[i])
            continue;
        if (all)
            gst_bin_add (GST_BIN (s), parts[i]);
        else
            gst_object_unref (gst_object_ref_sink (parts[i]));
    }
    if (!all) {
        s->out = s->conv = s->level = NULL;
        return;
    }
    if (!gst_element_link_many (s->conv, s->level, s->out, NULL))
        return;
    GstPad *target = gst_element_get_static_pad (s->conv, "sink");
    GstPad *pad = gst_ghost_pad_new_from_template ("sink", target,
                                                   gst_static_pad_template_get (&sink_template));
    gst_object_unref (target);
    if (!pad || !gst_element_add_pad (GST_ELEMENT (s), pad))
        return;
    /* what playbin will do: the first element in here that keeps a volume */
    GType keeps = g_type_from_name ("GstStreamVolume");
    GstElement *found = keeps ? gst_bin_get_by_interface (GST_BIN (s), keeps) : NULL;
    s->ok = found == s->level;
    if (found)
        gst_object_unref (found);
    if (!s->ok)
        GST_WARNING_OBJECT (s, "the volume element is not the one a search finds: not used");
}

static gboolean plugin_init (GstPlugin *plugin)
{
    /* pulsesink is PRIMARY + 10 */
    return gst_element_register (plugin, "walpewebsink", GST_RANK_PRIMARY + 20,
                                 walpe_web_sink_get_type ());
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, walpesound,
                   "WalPE: the sound output of web wallpapers", plugin_init, "1", "BSD",
                   "walpe", "WalPE")
