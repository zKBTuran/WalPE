/* soundserver_test.c - what the sound server holds for the pages of web
 * wallpapers (engine/soundserver.c): the sentence `walpe-ctl soundcheck`
 * gives, made from what `pactl -f json list sink-inputs` prints. The lists
 * are the ones a private PipeWire 1.0.5 (pactl 16.1) and a private
 * PulseAudio 16.1 printed for a web wallpaper with an <audio>, for the
 * same stream muted and set to 0 % with pactl, for another program's muted
 * stream beside it and for a video wallpaper (WalPE's own stream), with
 * the members left out that name the machine. No pactl is run, no server
 * needed. */
#include "engine/soundserver.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); fails++; } } while (0)

static const char page_ok[] =
    "[{\"index\":440,\"driver\":\"PipeWire\",\"client\":\"434\",\"sink\":46,\"corked\":false,\"mute\":false,\"volume\":"
    "{\"front-left\":{\"value\":65536,\"value_percent\":\"100%\",\"db\":\"0.00 dB\"},\"front-right\":{\"value\":65536"
    ",\"value_percent\":\"100%\",\"db\":\"0.00 dB\"}},\"properties\":{\"application.name\":\"WebKit\",\"application."
    "process.binary\":\"WebKitWebProcess\",\"application.language\":\"C\",\"media.role\":\"music\",\"media.name\":"
    "\"Playback Stream\",\"node.name\":\"WebKit\",\"media.class\":\"Stream/Output/Audio\"}}]";
static const char page_muted[] =
    "[{\"index\":440,\"driver\":\"PipeWire\",\"client\":\"434\",\"sink\":46,\"corked\":false,\"mute\":true,\"volume\":{"
    "\"front-left\":{\"value\":65536,\"value_percent\":\"100%\",\"db\":\"0.00 dB\"},\"front-right\":{\"value\":65536,"
    "\"value_percent\":\"100%\",\"db\":\"0.00 dB\"}},\"properties\":{\"application.name\":\"WebKit\",\"application.p"
    "rocess.binary\":\"WebKitWebProcess\",\"application.language\":\"C\",\"media.role\":\"music\",\"media.name\":\""
    "Playback Stream\",\"node.name\":\"WebKit\",\"media.class\":\"Stream/Output/Audio\"}}]";
static const char page_zero[] =
    "[{\"index\":440,\"driver\":\"PipeWire\",\"client\":\"434\",\"sink\":46,\"corked\":false,\"mute\":false,\"volume\":"
    "{\"front-left\":{\"value\":0,\"value_percent\":\"0%\",\"db\":\"-inf dB\"},\"front-right\":{\"value\":0,\"value_pe"
    "rcent\":\"0%\",\"db\":\"-inf dB\"}},\"properties\":{\"application.name\":\"WebKit\",\"application.process.bina"
    "ry\":\"WebKitWebProcess\",\"application.language\":\"C\",\"media.role\":\"music\",\"media.name\":\"Playback St"
    "ream\",\"node.name\":\"WebKit\",\"media.class\":\"Stream/Output/Audio\"}}]";
static const char other_muted[] =
    "[{\"index\":440,\"driver\":\"PipeWire\",\"client\":\"434\",\"sink\":46,\"corked\":false,\"mute\":false,\"volume\":"
    "{\"front-left\":{\"value\":65536,\"value_percent\":\"100%\",\"db\":\"0.00 dB\"},\"front-right\":{\"value\":65536"
    ",\"value_percent\":\"100%\",\"db\":\"0.00 dB\"}},\"properties\":{\"application.name\":\"WebKit\",\"application."
    "process.binary\":\"WebKitWebProcess\",\"application.language\":\"C\",\"media.role\":\"music\",\"media.name\":"
    "\"Playback Stream\",\"node.name\":\"WebKit\",\"media.class\":\"Stream/Output/Audio\"}},{\"index\":454,\"drive"
    "r\":\"PipeWire\",\"client\":\"453\",\"sink\":46,\"corked\":false,\"mute\":true,\"volume\":{\"front-left\":{\"value"
    "\":65536,\"value_percent\":\"100%\",\"db\":\"0.00 dB\"},\"front-right\":{\"value\":65536,\"value_percent\":\"100"
    "%\",\"db\":\"0.00 dB\"}},\"properties\":{\"media.role\":\"music\",\"media.format\":\"WAV (Microsoft)\",\"applica"
    "tion.name\":\"paplay\",\"media.name\":\"/tmp/claude-0/-workspace/42b0ee8c-eaeb-5015-bd4e-21026763726f/"
    "scratchpad/restore/away3/rv-sound/content/web-tag/tone.wav\",\"application.process.binary\":\"pacat\""
    ",\"application.language\":\"C\",\"node.name\":\"paplay\",\"media.class\":\"Stream/Output/Audio\"}}]";
static const char video[] =
    "[{\"index\":0,\"driver\":\"protocol-native.c\",\"client\":\"6\",\"sink\":1,\"corked\":false,\"mute\":false,\"volu"
    "me\":{\"front-left\":{\"value\":65536,\"value_percent\":\"100%\",\"db\":\"0.00 dB\"},\"front-right\":{\"value\":6"
    "5536,\"value_percent\":\"100%\",\"db\":\"0.00 dB\"}},\"properties\":{\"media.name\":\"clip.mp4\",\"application."
    "name\":\"walpe\",\"application.process.binary\":\"walpe-daemon\",\"application.language\":\"C\"}}]";

int main (void)
{
    char *n;
    /* nothing to say: the page's stream as WalPE leaves it; another
     * program's muted stream; WalPE's own stream; no stream; no list */
    n = walpe_sound_server_note_from_list (page_ok);
    CHECK (n == NULL, "a page's stream, not muted, at 100 %%: %s", n);
    g_free (n);
    n = walpe_sound_server_note_from_list (other_muted);
    CHECK (n == NULL, "another program's stream is muted: %s", n);
    g_free (n);
    n = walpe_sound_server_note_from_list (video);
    CHECK (n == NULL, "a video wallpaper's stream: %s", n);
    g_free (n);
    static const char *const none[] = { NULL, "", "[]", "{}", "[1,2]", "[{\"index\":1}]", "No PulseAudio daemon running",
                                        "Sink Input #440\n\tMute: yes\n", "[{\"properties\":{\"application.name\":\"WebKit\"}}]",
                                        "[{\"properties\":{\"application.name\":\"WebKit\"},\"mute\":false,\"volume\":{}}]",
                                        "[{\"properties\":7,\"mute\":true}]", "[" };
    for (guint i = 0; i < G_N_ELEMENTS (none); i++) {
        n = walpe_sound_server_note_from_list (none[i]);
        CHECK (n == NULL, "no list, or no stream that is silent (%u): %s", i, n);
        g_free (n);
    }
    /* the page's stream muted in the server: said, with the stream's number */
    n = walpe_sound_server_note_from_list (page_muted);
    CHECK (n && strstr (n, "1 of 1 stream(s) of WebKit muted") && strstr (n, "pactl set-sink-input-mute 440 0") &&
           strstr (n, "pactl set-sink-input-volume 440 100%") && strstr (n, "mixer"), "muted: %s", n ? n : "(null)");
    g_free (n);
    /* ... at 0 %: said */
    n = walpe_sound_server_note_from_list (page_zero);
    CHECK (n && strstr (n, "1 of 1 stream(s) of WebKit at 0 %") && strstr (n, "pactl set-sink-input-mute 440 0"),
           "at 0 %%: %s", n ? n : "(null)");
    g_free (n);
    /* two pages, one muted and one at 0 %, and a third that is heard; the
     * page process is known by its program too, where the name is another */
    n = walpe_sound_server_note_from_list (
        "[{\"index\":7,\"mute\":false,\"volume\":{\"front-left\":{\"value\":65536},\"front-right\":{\"value\":65536}},"
        "\"properties\":{\"application.name\":\"WebKit\"}},"
        "{\"index\":9,\"mute\":false,\"volume\":{\"front-left\":{\"value\":0},\"front-right\":{\"value\":0}},"
        "\"properties\":{\"application.name\":\"Some Browser\",\"application.process.binary\":\"WebKitWebProcess\"}},"
        "{\"index\":12,\"mute\":true,\"volume\":{\"mono\":{\"value\":65536}},\"properties\":{\"application.name\":\"WebKit\"}},"
        "{\"index\":13,\"mute\":true,\"volume\":{\"mono\":{\"value\":0}},\"properties\":{\"application.name\":\"mpv\"}}]");
    CHECK (n && strstr (n, "2 of 3 stream(s) of WebKit silent (1 muted, 1 at 0 %)") &&
           strstr (n, "pactl set-sink-input-mute 9 0"), "two of three: %s", n ? n : "(null)");
    g_free (n);
    /* one channel at 0 is a balance, not silence */
    n = walpe_sound_server_note_from_list (
        "[{\"index\":7,\"mute\":false,\"volume\":{\"front-left\":{\"value\":0},\"front-right\":{\"value\":65536}},"
        "\"properties\":{\"application.name\":\"WebKit\"}}]");
    CHECK (n == NULL, "one channel at 0: %s", n);
    g_free (n);
    if (fails) {
        printf ("soundserver: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("soundserver: ok\n");
    return 0;
}
