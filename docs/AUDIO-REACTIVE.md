# Audio-reactive wallpapers and media integration

Many Wallpaper Engine wallpapers move with what the computer plays: bars, particles,
lights that pulse with the bass. Others show what is playing: title, artist, cover.
WalPE gives both kinds what they ask for. This page says what is supported for each kind
of wallpaper, what numbers a wallpaper gets, where they come from, what it costs, and what
differs from Wallpaper Engine. (The sound a wallpaper *makes* is another subject:
[SOUND.md](SOUND.md).)

> **State of testing.** Measured end to end against PulseAudio 16.1 and against PipeWire
> 1.0.5 (`pipewire-pulse`, WirePlumber 0.4.17), both with null sinks: test tones played
> into the sink, and what arrives looked at in a web page's listener, in a scene's shader
> (pixels of a screenshot) and in a scene's script. No sound card, no loudspeaker, no
> microphone; llvmpipe. Media integration against a stand-in player on the session bus.
> Not compared with Wallpaper Engine on Windows side by side: its analysis is not
> documented, see [Differences](#differences-from-wallpaper-engine).

## What is supported

| Wallpaper | What it can use | State |
|---|---|---|
| Scene | shader uniforms `g_AudioSpectrum16Left` / `Right`, `...32...`, `...64...` (the audio modes of Wallpaper Engine's own effects - pulse, shake, audio bars ... - and of any effect a wallpaper brings) | yes |
| Scene | SceneScript `engine.registerAudioBuffers(resolution)` with `AUDIO_RESOLUTION_16` / `_32` / `_64`: `left`, `right`, `average` | yes |
| Scene | particle emitters with "Audio response" | partly: the emission rate follows the bass level; exponent, bounds, frequency range and the choice of a channel are not read |
| Scene | media events `mediaStatusChanged`, `mediaPlaybackChanged`, `mediaPropertiesChanged`, `mediaThumbnailChanged`, `mediaTimelineChanged`; the textures `$mediaThumbnail`, `$mediaPreviousThumbnail` | yes |
| Web | `window.wallpaperRegisterAudioListener(callback)` | yes |
| Web | `window.wallpaperRegisterMediaStatusListener`, `...PropertiesListener`, `...ThumbnailListener`, `...PlaybackListener`, `...TimelineListener`; `window.wallpaperMediaIntegration` | yes |
| Video | nothing to react with | - |

Needs `libpulse` at build time (`-Daudio`, on when the library is found; without it every
spectrum is silence) and a sound server that speaks the PulseAudio protocol: PulseAudio,
or PipeWire with `pipewire-pulse`. Media integration needs a session D-Bus and a player
that implements MPRIS 2 (most do; browsers too).

## Where the sound comes from

WalPE listens to **what the computer plays**: the monitor of the default output device
("what you hear"). It **never opens a microphone** by itself:

- The capture stream is made on the monitor of the default output and is marked so that
  the sound server does not move it to another source. (Until this was done, PulseAudio
  moved it: on a machine without a microphone, plugging in a headset or a camera put the
  stream on the new microphone.)
- Before a single sample is analysed, WalPE asks the server which source the stream is on,
  and refuses one that is not the monitor of an output. `walpe-ctl status` and the log
  name the source: `capturing alsa_output.pci-0000_00_1f.3.analog-stereo.monitor`.
- When you switch the default output (loudspeakers to headphones), or the output
  disappears, WalPE makes the stream again on the monitor of the new default output.
- Only you can point it elsewhere: `WALPE_AUDIO_SOURCE=<source name>` in the daemon's
  environment (`pactl list short sources` lists the names) makes it read that source, a
  microphone included. Status then says `capturing <name> (WALPE_AUDIO_SOURCE)`, and adds
  `not the monitor of an output: a microphone?` when that is what was named. A name the
  sound server does not know is an error and nothing is read (PipeWire would link such a
  stream to the default source, the microphone: WalPE refuses what it did not ask for).
- A wallpaper never gets samples. A scene's shaders and scripts and a web page's listener
  get the spectrum described below: 64 numbers per channel, 30 times a second. A web page
  cannot use the microphone through the browser engine either: every permission request
  of a page is denied ([SECURITY.md](SECURITY.md)).
- The capture runs only while a wallpaper that asked for it is shown and not paused.

**The spectrum is not harmless.** 64 levels per channel 30 times a second say whether
something plays and when, are enough to recognise a piece of music or a film, and are
close to what a channel vocoder sends: speech that comes out of the output - the other
side of a call, a video - may be made out from them (not tried here; assume it). The
output's monitor also carries whatever is routed to the output, a microphone that is
played back ("listen to this device", a loopback) included. A scene cannot send anything
anywhere. A **web wallpaper** is a web page: with network access, which it has unless you
switch it off, it can send what its listener gets to its author. So for a web wallpaper
you do not trust: its option "Allow network access" off (`walpe-ctl setprop
walpe_network=false`), or `WALPE_NO_AUDIO=1` for the daemon. `walpe-ctl status` shows
`audio: capturing ...` whenever a wallpaper listens. There is no switch for this in the
settings window yet.

Because it listens to the output, an audio-reactive wallpaper also reacts to wallpaper
sound (another wallpaper's, or its own).

## The numbers a wallpaper gets

One analysis for all consumers (`src/common/audio.c`): 44.1 kHz stereo, a 4096-sample
Hann window (93 ms), 30 spectra a second, each channel on its own.

- **64 bands per channel**, from 30 Hz to 16 kHz, each a 64th of the nine octaves in
  between (band `b` starts at `30 * 533.3^(b/64)` Hz). Band 0 is the lowest bass. The 32-
  and 16-band arrays cover the same range; each of their bands is the loudest of the 2 or
  4 bands it spans.

  | Tone | band of 64 | of 32 | of 16 |
  |---|---|---|---|
  | 200 Hz | 19 | 9 | 4 |
  | 440 Hz | 27 | 13 | 6 |
  | 1 kHz | 35 | 17 | 8 |
  | 5 kHz | 52 | 26 | 13 |
  | 10 kHz | 59 | 29 | 14 |

  Below about 110 Hz a band is narrower than the analysis can tell apart (10.8 Hz per
  bin), so the lowest dozen bands move together.
- **Values** are 0 for silence and grow with the level of the loudest tone in the band on
  a logarithmic curve, `log10(1 + 36 * amplitude)` with 1.0 = a full-scale sine: a tone at
  -12 dBFS reads 1.0, at -32 dBFS 0.28, at full scale 1.57. Like Wallpaper Engine's, they
  "may exceed 1": a wallpaper that draws them should clamp, as Wallpaper Engine's example
  does.
- **Rise and fall**: a band rises at once and falls with a time constant of 150 ms.
- **Silence**: digital silence gives exactly 0 in every band. While the output is
  suspended nothing is captured at all, and consumers read 0.

Measured through a sound server, a 440 Hz tone on the left and a 5 kHz tone on the right at
-12 dBFS: band 27 at 0.997 on the left and band 52 at 0.961 on the right, nothing of one
channel in the other - in the web listener, in the shader uniforms (64, 32 and 16 bands)
and in SceneScript's buffers alike. A tone that starts in silence is in the spectrum
32-80 ms after its first samples were played.

### Web wallpapers

`wallpaperRegisterAudioListener(callback)`: the callback gets a plain array of **128
numbers, 64 left then 64 right**, bass first, three decimals, about **30 times a second**
(measured: 29-30 calls a second, 34 ms apart) - whatever the frame rate limit. A call is
skipped while the page has not finished the one before. The last callback registered is
the one that is called.

In silence the listener goes on being called with zeros for 1.5 s, and for as long as the
page keeps changing its picture (falling bars finish; a page that animates in the listener
keeps its rate). Then it is called once a second until there is sound, the picture
changes, or the page is told something (an option, a click).
`WALPE_WEB_AUDIO_SILENCE=off` keeps the 30 calls a second in silence, `=always` stops
calling after 1.5 s of silence whatever the page does.

### Scenes

The shader uniforms and the arrays of `engine.registerAudioBuffers` are refreshed once per
frame with the newest spectrum. `average` is the mean of `left` and `right`. A resolution
other than 16 or 32 gives 64 bands.

Particle emitters with "Audio response" switched on (`audioprocessingmode`) emit at their
rate times the bass level (the mean of the eight lowest of the 64 bands of both channels).

## Media integration

"Now playing" comes from MPRIS 2 on the session bus: the player that started playing last,
else the one paused last, else none.

| Event (scene / web) | Members | From MPRIS |
|---|---|---|
| status | `enabled` | `true` while WalPE is connected to the session bus; `false` with `WALPE_NO_MEDIA=1` or without a bus |
| playback | `state`: stopped 0, playing 1, paused 2 (`MediaPlaybackEvent.PLAYBACK_*`; web: `wallpaperMediaIntegration.PLAYBACK_*` and `wallpaperMediaIntegration.playback.*`) | `PlaybackStatus` |
| properties | `title`, `artist`, `albumTitle`, `albumArtist`, `genres`, `subTitle` (always empty), `contentType` (`music`, or `video` when the player names a video file) | `Metadata`: `xesam:title`, `xesam:artist`, `xesam:album`, `xesam:albumArtist`, `xesam:genre`, `xesam:url` |
| thumbnail | the cover (scene: texture; web: a `data:` URL for an `img`), `primaryColor`, `secondaryColor`, `tertiaryColor`, `textColor`, `highContrastColor` | `mpris:artUrl` (a local file, or http(s) when built with libsoup; [SECURITY.md](SECURITY.md#cover-art-of-media-integration)) |
| timeline | `position`, `duration` in seconds, once a second while something plays and after a seek | `Position`, `mpris:length`, `Seeked` |

A listener gets the current state when it registers. Each kind has one listener per page.

## What it costs

A wallpaper that does not react to sound and shows no "now playing" costs nothing for any
of this: no stream, no thread, no connection, no timer. Measured (`walpe-daemon`, all
threads, wake-ups per second over 15 s; one 1280x720 monitor at 60 fps, software rendering,
the default build):

| Shown | PipeWire | PulseAudio |
|---|---|---|
| a scene without audio | 0.1, 13 threads | 0.1, 13 threads |
| a picture with an audio-bar effect, **nothing plays** | 0.1, one thread more (asleep) | 0.1 where the output suspends when idle (a desktop's set-up); 50 where it never does (the capture is handed silence) |
| the same, **music plays** | capture thread 30-42, 0.3 % of one core; the scene draws at its frame rate | capture thread 30-46, 0.3 % |
| an audio scene that also animates by itself (script, particles), nothing plays | its frame clock, as without audio (60); capture 0 | the same |
| any audio scene, **paused** or covered | 0.0, 13 threads: the stream is closed, the capture thread ends 3 s later | the same |
| a web page with an audio listener that draws only on change, nothing plays | 14 (it looks for sound ten times a second and calls the listener once a second), the page's own processes 7 | 16 where the output suspends, the page's processes 5 |
| a web page with an audio listener, paused | 1.7 or less (a paused page; the stream is closed, the capture thread ends) | 1.1 |
| a scene with media events, a player plays | media thread 1.2, D-Bus thread 0.4, two threads more | the same (the session bus, not the sound server) |
| the same, paused | 0.1 (it wakes when the player announces something) | the same |

- The capture is one thread (`walpe-audio`) with one connection to the sound server. It
  sleeps unless samples arrive; in silence nothing is transformed.
- WalPE does not keep your output device awake: while nothing plays, the server may suspend
  the device as if no wallpaper were listening, and the capture then gets nothing. (On
  PipeWire this needed the stream's node to be passive; before, a visible audio-reactive
  wallpaper kept the output running for ever.)
- A scene in which only the spectrum moves stops its frame clock after two seconds of
  silence and is woken by the capture with the first sound (`WALPE_AUDIO_REST=0`: it keeps
  the clock). A scene that also animates otherwise draws as it always does.
- Without a sound server the capture asks again every 5 s while such a wallpaper is shown.
- After a wallpaper with media integration was shown once, GLib's D-Bus worker thread
  stays (asleep) until the daemon ends.

## Settings and what to look at

There is no switch in the settings window: the capture follows the wallpaper. In the
daemon's environment ([ENVIRONMENT.md](ENVIRONMENT.md)):

| Variable | Effect |
|---|---|
| `WALPE_NO_AUDIO=1` | nothing is captured, for scenes and web pages alike: every spectrum is silence; status says `disabled (WALPE_NO_AUDIO)` |
| `WALPE_AUDIO_SOURCE=<name>` | capture this source instead of the default output's monitor; a name the server does not know is an error, nothing is read |
| `WALPE_AUDIO_HZ=<10..200>` | spectra per second (30) |
| `WALPE_AUDIO_REST=0` | audio-only scenes keep their frame clock in silence |
| `WALPE_WEB_AUDIO_SILENCE=off\|always` | the web listener in silence (above) |
| `WALPE_NO_MEDIA=1` | no media integration: wallpapers see `enabled: false` |

`walpe-ctl status` has two lines:

```
audio: off | waiting for <source>: the stream starts when the output plays
       | capturing <source> | error: ... | disabled (WALPE_NO_AUDIO)
media: off | idle (no player) | <player>: playing | disabled (...)
```

"waiting" is what PipeWire shows while the output is suspended: the stream starts with the
first sound. `data/calibration/audio-probe` is a web wallpaper that prints the 128 values,
their peaks and how fast they fall: open it in WalPE and in Wallpaper Engine with the same
test signal to compare the two.

## Differences from Wallpaper Engine

- **The analysis is WalPE's own.** Wallpaper Engine documents the shape of the data (64
  bands per channel, bass first, 0 to 1 and sometimes more, about 30 updates a second) and
  not how a frequency maps to a band or a level to a value. WalPE matches the documented
  shape; band edges, the level curve and the smoothing are choices (above), not copies. A
  visualizer tuned by eye in Wallpaper Engine may look fuller or flatter here.
- **Values above 1** start at -12 dBFS; loud, compressed music reaches 1.2 to 1.5 in its
  strongest band. Wallpapers that do not clamp overshoot.
- **Web listener in silence**: once a second instead of 30 times, after the picture has
  come to rest (`WALPE_WEB_AUDIO_SILENCE=off` for 30).
- **Particle emitters**: of "Audio response" only on / off is read. Exponent, bounds,
  frequency min / max and left / right / centre are ignored; the rate follows the bass
  level of both channels.
- **Media integration** is MPRIS, not the Windows media session: `subTitle` is always
  empty, `contentType` is never `image`, the cover is whatever the player's `mpris:artUrl`
  names (web: a `data:` URL, JPEG, or PNG for a cover with transparency), and the timeline
  ticks once a second. There is no user switch for the integration besides
  `WALPE_NO_MEDIA=1`.
- **Video wallpapers** have nothing that reacts.
