/* soundserver.h - a look at what the sound server holds for the pages of
 * web wallpapers, taken once, when somebody asks.
 *
 * The sound of a web wallpaper is WebKit's stream in the sound server, and
 * WalPE never changes a stream there (gst/walpesound.c says why). The
 * server keeps a volume and a mute for it all the same - the user's, set in
 * the desktop's mixer - and stores both for later streams: WirePlumber 0.5
 * under the name of the program ("WebKit": every program made with
 * WebKitGTK), WirePlumber 0.4 and PulseAudio under the stream's role
 * ("music"). A stream that was muted or set to 0 % there once - in the
 * mixer, by another WebKitGTK program, by a WalPE that ran without
 * libgstwalpesound.so while it was muted or at volume 0 - comes back muted
 * or at 0 % every time: the page plays, the settings say "on", and nothing
 * is heard, for good. Nothing inside WalPE shows it. This does.
 *
 * Nothing here runs unless it is called: no thread, no timer, no
 * connection that stays. */
#pragma once
#include <glib.h>

/* What `pactl -f json list sink-inputs` printed -> one sentence when a
 * stream of WebKit's page process is muted or at 0 % in the server, with
 * what undoes it; NULL when none is (also: no such stream, or text that is
 * not that list). g_free. */
char *walpe_sound_server_note_from_list (const char *pactl_json);
/* Asks the server, through `pactl` (libpulse's tool, there with PulseAudio
 * and with pipewire-pulse): one child process, waited for here, for at most
 * a second and a half. NULL when no stream is muted or at 0 %, and when the
 * server cannot be asked (no pactl, one that has no JSON output, no
 * server); *asked (optional) says which. For a thread that may wait that
 * long, on a request - never on a timer. g_free. */
char *walpe_sound_server_note (gboolean *asked);
