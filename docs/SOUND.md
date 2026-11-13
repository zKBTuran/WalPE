# Wallpaper sound

Scene wallpapers can carry sound layers: rain, wind, a song, the tracks of a music player.
WalPE plays them: wallpaper sound is **on by default**, at half volume (up to 0.14.0 it was
off, at volume 0, and could be switched on from the command line only). A wallpaper without
sound layers behaves and renders exactly as before and costs nothing for it: no player, no
thread, no connection to the sound server. The same switch, volume and mute are for the sound track of a
[video wallpaper](#video-wallpapers) and for the sound a [web wallpaper](#web-wallpapers)
makes.

> **State of testing.** Measured against PulseAudio 16.1 and against PipeWire 1.0.5
> (`pipewire-pulse`, WirePlumber 0.4.17) with a null sink, by recording the sink's monitor
> and analysing the recording: no loudspeaker, no sound card (so no drift against a device
> clock). Web wallpapers, and a video's sound over pauses and resumes, also against
> PipeWire 1.6.11 with WirePlumber 0.5.19. The
> maintainer has heard scene sound on his own machine with 0.14.0.

## Hearing something

```sh
walpe-ctl set scene_sound=on|off # wallpaper sound: scenes, videos and web pages (default: on)
walpe-ctl set volume=0.5         # the volume of all wallpaper sound, 0..1 (default: 0.5)
walpe-ctl set mute=on|off        # silence that keeps the volume value
walpe-ctl setprop walpe_sound=on|off   # one wallpaper's own switch: wins over scene_sound
walpe-ctl setprop walpe_volume=0.3     # one wallpaper's own volume, 0..1: times the volume above
```

The three settings apply at once, without loading the wallpaper again, and are kept in
`config.json` (`scene_sound`, `volume`, `mute`). The settings window ("Engine settings")
has them under "Audio": the switch "Wallpaper sound", the volume slider and the switch
"Mute". Under the first is the line `sound` of the status below - what is heard now, or
why nothing is - and switching it on while the volume is 0 also sets the volume to 0.5,
so that one click is enough. The tray's entry reads "Switch wallpaper sound on" while it
is off, else "Mute" (the volume to 0) or "Unmute" (the volume back, `mute` off).

One wallpaper: `walpe_sound` and `walpe_volume` are options of the wallpaper, kept with its
other options (`--monitor N` / `--path P`; `resetprops` drops them). In the library window
a wallpaper that has sound layers shows them as the first two rows of its options, under
"Sound of this wallpaper" - "Sound" (as in Engine settings / on / off) and "Volume" -
whether or not its author gave it a music option of his own; a wallpaper without sound has
no such rows. Both apply to the running wallpaper at once: it is not loaded again and its
sound goes on where it is.

A configuration from 0.14.0 or older: "off, volume 0" was the default there and was
written into every `config.json`, and nothing but `walpe-ctl set scene_sound=on` switched
sound on - the settings window had only the volume slider, and the tray's "Unmute" only
set the volume to 1. "Off" in such a file is therefore the old default, not a choice, and
at the first start it becomes "on" (the log says so, once): at volume 0.5 when the file
holds volume 0, at the volume the file holds otherwise - **a volume that was turned up in
0.14.0 without effect is heard now, at that level**. Sound that was switched on there is
kept as it is, volume 0 included. What is set from now on is kept, "off" included. A file
that has no `scene_sound` at all (from before 0.14.0, or a 0.14.0 that never saved a
setting) with a volume above 0 is not changed: sound is on by default and is heard at
that volume from the first start, and the log says so ("config: this file of a version up
to 0.14.0 has no switch for wallpaper sound: it is on, ...") at every start until this
version saves the file.

`walpe-ctl status` has `scene_sound`, `mute`, `volume` and one line `sound`, for example:

```
off (walpe-ctl set scene_sound=on; for one wallpaper: walpe-ctl setprop walpe_sound=on)
on, but the volume setting is 0: silent (walpe-ctl set volume=0.5)
on: 2 stream(s) through PulseAudio, 1 wallpaper(s) with sound shown
on: 2 stream(s) play into nothing, no sound server (Connection refused); 0 heard
on: nothing is playing: 1 wallpaper(s) with sound paused - sound stops while its wallpaper is paused (...)
on: 1 web wallpaper(s) at 50 % (what the page plays, through WebKit: the stream "WebKit" in the system's mixer)
on: nothing is playing: 1 web wallpaper(s) paused - sound stops while its wallpaper is paused (...)
```

The streams counted are those of scenes and videos. A web wallpaper's sound is WebKit's own
stream, so the line names the pages that are shown and the level they are given (the volume
setting times the wallpaper's own volume); whether a page makes any sound cannot be known
from outside it. A page silenced by its own switch or volume is named as that.

The log has one line per scene with sound layers and one line for every file that is
refused, with the reason.

## What is played

- Files: WAV (PCM 8 / 16 / 24 / 32 bit, 32-bit float), MP3, FLAC, Ogg Vorbis, Ogg Opus.
  MP3, FLAC and Ogg need `walpe-video.so` (FFmpeg); it is loaded when the first such file
  is played.
- Modes: loop (several files: one after the other), random (a random file, then the pause
  the wallpaper sets, counted from the end of the sound), single (once). "Start silent"
  layers wait for the wallpaper's script.
- The layer's volume (also when bound to a wallpaper option) times your volume setting.
- Scripts: play / pause / stop / isPlaying and the layer's volume (the music players of
  some wallpapers).
- Loops close without a gap for WAV, FLAC, Ogg and for MP3 files that carry a LAME tag;
  an MP3 without one keeps the few hundredths of a second of silence its encoder added.
- More than two channels: the first two are played.

Output: PulseAudio, or PipeWire through `pipewire-pulse`: one stream per sounding layer,
named after the layer, application name `walpe`, so the desktop's mixer shows each of them.
If no server runs, WalPE says so in the log, goes on silently and asks again every 5
seconds. The layers of a scene open their streams one at a time, a few milliseconds apart
(asked for in the same instant, some were refused by the server), and a connection the
server still refuses is asked for again at once (up to four times, 10 ms apart), so such a
layer does not stay silent for those 5 seconds. A layer waits while the others get their
streams; only behind a stream the server has not answered for a quarter of a second it
plays into nothing meanwhile and asks for its turn again a quarter of a second later.

No call into the sound server waits for ever. A new stream has 8 seconds; a write gives up
after 4 seconds in which the server took nothing; when a layer falls silent (a pause, a
covered monitor, mute) the server plays out what the stream holds and is waited for for
one second more than that takes. A stream that runs out of time is given up without
another word to it, and a new one is opened with the next sound - at once when it was the
stream that stalled; after the usual 5 seconds when the new one stalls as well (a sink
that is suspended takes nothing from any stream) or when none is given. For a stream that
was played out and is then used again - a resume within two seconds of the pause - the
limit is 0.3 seconds until the server asks for more sound for the first time:
`pipewire-pulse` 1.0.5 (Ubuntu 24.04) now and then leaves such a stream standing - it
takes what fits into it and never plays it, or does not answer the playing-out at all
(about one pause in twenty to thirty here; not seen with PipeWire 1.6.11 or PulseAudio
16.1). The wallpaper's sound is then back about half a second after the resume instead
of at once; before these limits it was gone until the wallpaper was opened again.
`WALPE_SOUND_SINK=<sink name>` plays into that sink instead of the default one;
`WALPE_SOUND_NULL=1` plays into nothing (tests).

## When it is silent

- While the wallpaper is paused (a fullscreen window, a covered monitor, the focus,
  battery and display rules, `walpe-ctl pause`). It goes on where it stopped; stopping and
  starting take about a fifth of a second (the sound is faded, not cut). A monitor counts
  as covered when windows hide 85 % of it (`pause_covered`, on by default), and WalPE's own
  window counts: under a tiling window manager one tiled window is enough, so there a
  wallpaper is heard only on a monitor without windows. `walpe-ctl set covered=off` (the
  settings window: "Pause a monitor when windows cover it") keeps it running, and heard,
  behind windows. The status line `sound` says when this is why nothing is heard.
- While the layer is invisible (a wallpaper option that hides it, a script).
- While 16 other sound layers already sound, all wallpapers and monitors together: a
  further layer waits and starts when one of them has fallen silent. The log says so once,
  the status line for as long as it lasts.
- A wallpaper that was unloaded after a long pause, or loaded again because one of its
  options changed, starts its sound from the beginning.

Two monitors: the same wallpaper on two monitors sounds once. Where each monitor renders
it by itself, only one of the two is heard; when that one is paused the other takes over
from where its own players stand, and for about 0.2 s both are heard. Two different
wallpapers are both heard.

## Limits

A wallpaper's files are data from the internet, so:

- A sound file is looked up inside the wallpaper's folder or Wallpaper Engine's assets
  only: no absolute paths, no `..`, no links that lead out of the folder, no addresses,
  regular files only, at most 2 GiB.
- Only four FFmpeg decoders can be reached from a sound file: MP3, FLAC, Vorbis, Opus. The
  file's first bytes decide, not its name.
- 8000-192000 Hz, 1-8 channels, at most 6 hours per file, at most 64 files per layer and
  32 sound layers per wallpaper; at most 16 layers sound at the same time.
- A file that breaks a limit or is no sound is refused with a log line and not opened
  again; the wallpaper is shown as before.

Known behaviour worth knowing:

- Quitting or restarting the daemon while a wallpaper sounds cuts the sound at full level
  (no fade).
- A layer whose own volume is 0 still holds a stream and one of the 16 places while your
  volume setting is above 0.
- Audio-reactive wallpapers listen to what the default output plays, so they react to
  wallpaper sound too (not when `WALPE_SOUND_SINK` names another sink).
- With sound off (the setting, or the wallpaper's own switch), a scene with sound layers still looks its sound files up when it
  loads (it opens none and starts no thread), so that switching sound on applies at once.
- 16 sounding layers cost the daemon 36 threads and 80 file descriptors.
- The status words "no sound server (...)" are also shown when the server runs and the
  sink named by `WALPE_SOUND_SINK` does not exist; the reason in the brackets is right.
- A video inside a scene (a video layer) is silent.

## Video wallpapers

A video wallpaper whose file has a sound track plays it, under the same settings as a
scene: `scene_sound` (or the wallpaper's own `walpe_sound`), `volume`, `mute`. One stream,
named after the wallpaper.

- The player decodes pictures only. The first sound track of the file is taken out once
  with `ffmpeg` (the program imports already need) into
  `~/.cache/walpe/video/<hash>-sound.flac` - lossless, 16 bit, two channels - and that file
  is played by what plays scene sound. It is made when the track is about to be heard for
  the first time, in the background: the picture never waits, the sound follows when the
  file is there (0.4 s for a clip of four seconds here; a few seconds for a song). A video
  without a sound track gets an empty file of that name, so nobody asks again. A changed
  video gets a new file; files nothing played for 90 days are removed.
- What that costs, measured: `ffmpeg` runs once per video at the lowest CPU and disk
  priority - 0.1 s of CPU for a clip of 10 s, 0.6 to 0.9 s for a song of four minutes, 22 s
  for a video of two hours (34 s by the clock when the machine is idle, 100 s beside a
  daemon that renders in software; the sound comes when it is done). The file takes 3 to
  4 MB a minute for music (1 MB for a plain tone, up to 11 MB for noise): 388 MB for the two
  hours. The track is read as a stream while it plays: a track of two hours takes no more
  of the daemon's memory than one of ten seconds, and a place in it - after a pause, when
  sound is switched on in the middle - is found without decoding what lies in front of it.
  A wallpaper that is replaced before its `ffmpeg` is done takes it along, and leaves
  nothing in the cache: the track is taken out from the start the next time the video is
  shown (and kept once the video stays for as long as that takes). The same video on a
  second monitor keeps that `ffmpeg`: it ends when the last monitor that shows the video
  has got another wallpaper.
- **With sound off nothing of this happens**: no ffmpeg, no file, no thread, no stream.
- **Where the clip starts again nothing is heard of it.** One pass of the track is one pass
  of the clip, to the sample, and the track loops by itself in one stream: the file's first
  sample follows the last one of the pass. Measured with a clip of 4 s whose track is a
  660 Hz tone that fits its length: over five passes the recorded tone never falls below
  90 % of its level and its phase never moves by more than 7 degrees in 5 ms - exactly what
  the file's own samples give when they are put end to end (before this, 0.12 s of silence
  at every pass). A track whose content does jump there (a tone that does not fit) keeps
  its jump: it is the file's, and it is not smoothed.
- A track shorter than the clip is followed by silence until the clip starts again. A
  longer one is cut there: without more ado when it is longer by up to 50 ms (the padding
  of an encoder's last frame - 10.7 ms in the test clip), with a fade of 5 ms otherwise.
- The picture leads all the same: the track is told the picture's place when it begins to
  be heard and when the two are more than 0.3 s apart (a resume after the other monitor
  played on; after hours, the two clocks). That is a fade of 20 ms out and 10 ms in.
- Paused wallpaper: silent, and it goes on where the picture goes on. The same video on
  two monitors sounds once.
- **Videos imported before this version have no sound track** - the import dropped it.
  Import the original file again (`walpe-ctl add <file>`, or Import in the window): an
  import now keeps the first sound track in the MP4 it writes (AAC, 192 kbit/s, stereo).
  Workshop videos are played from their own file and need nothing.
- Needs `ffmpeg` in `PATH` with its `flac` encoder (part of every build of FFmpeg), and
  `walpe-video.so`'s FLAC decoder, as scene sound does. Without ffmpeg the log says so
  and the video stays silent.
- Measured over 21 minutes of a 4 s clip (315 passes, PulseAudio 16.1, a machine of four
  cores at a load of 15): the picture corrected the track twice, and the tone's phase
  against the recorder's clock moved by 1.9 ms in all - the two do not drift apart. In
  the same recording the sound had 14 gaps of 10 to 35 ms that no correction explains:
  the stream holds a fifth of a second, and a player thread that does not get the CPU
  for longer than that leaves a gap (scene sound has the same stream). An idle machine
  was not measured.
- When a video starts, and each time it goes on after a pause, the track can jump once to
  three times in the first three seconds (the fade above): the stream opens a moment
  after the picture goes on, the track is told the picture's place, and on a busy machine
  the answer is late enough to be corrected again. Counted here at a load of 3 to 10:
  three jumps at each of two resumes, one or two at a start.
- Not measured: how far the sound is behind the picture (the stream's buffer, about a
  fifth of a second at most, and the moment the first frame of a pass is drawn).

## Web wallpapers

A web wallpaper plays its own sound: `<audio>` and `<video>` elements, `Audio` objects
its script makes, Web Audio. WebKit plays it (GStreamer, in the page's process), under
the application name `WebKit` in the desktop's mixer. The same three settings decide:

- Sound off (`scene_sound=off`), `volume=0` or `mute=on`: the page is silent. A video in it goes on playing its pictures; an element that is sound alone is
  held - paused, a `play()` of the page is taken and kept - and starts when sound is
  switched on, so nothing is decoded into nothing.
- Otherwise the volume setting scales what the page itself sets: the volume of every media
  element (the page reads back the value it wrote) and the output of every AudioContext.
  `walpe-ctl set volume=0.25` measured a quarter of the amplitude of `volume=1`.
- **What is guaranteed.** Off, mute, volume 0, the wallpaper's own "off" and a paused
  wallpaper are hard: the page is muted by WebKit itself, outside the page, and nothing a
  page does gets round that (measured with a page that tries: every global it can find,
  the accessors of another frame, media and Web Audio made in a frame, again every 0.4 s -
  silence in all five cases, and its sound is back afterwards). The volume in between is
  the work of a script that runs among the page's own - nothing else reaches an `Audio`
  object or an AudioContext - and is an agreement, not a wall: a page that sets out to
  defeat it can be louder than the volume says, never louder than off. (The same page
  was at 0.252 of its amplitude with `volume=0.25`: it did not get round it either. A
  second test page, which makes elements and AudioContexts from a frame's constructors
  and feeds elements from MediaStreams, did: more than half its full level at
  `volume=0.25` - and digital silence, every sample 0, in each of the hard cases.) A
  frame from another site keeps the level it had when it was made, and the mute.
- **The sound server is left alone.** WebKit's own way to be louder, quieter or muted is
  the volume and the mute of its stream *in the sound server*: its GStreamer output
  (pulsesink) hands both there, and reads them back into the page. Sound servers store
  such values and give them to later streams - WirePlumber and PulseAudio's
  module-stream-restore under the stream's role, "music" for an `<audio>`, which other
  players use too. With WalPE setting a level for every page that went wrong in three
  ways, all measured (PipeWire 1.0.5 with WirePlumber 0.4.17; PulseAudio 16.1 with
  module-stream-restore): after mute and unmute a page stayed muted on PipeWire, for good
  (3 of 3; the element itself then said `muted = true`); a daemon that quit while muted
  or at volume 0 began silent the next time, whatever the settings said; and *another
  program's* stream of the role "music" was silent while WalPE was muted, at half its
  level while WalPE's volume was 0.5, and stayed so after WalPE had quit.
  So the page process plays through an output of WalPE's own, `libgstwalpesound.so` (a
  GStreamer plugin in `gst/` next to `walpe-web.so`): pulsesink with a volume element in
  front of it. Level and mute are a multiplication inside the page process. The stream in
  the server stays at 100 %, not muted - its slider in the system mixer is yours alone,
  kept by the server as for any program -, nothing of WalPE's is stored, nothing is read
  back. The plugin is loaded by the page process when a page plays its first sound; a
  page without sound never loads it.
  *Without the plugin* everything above is as it was: the daemon's log says so when the
  first web wallpaper is opened ("web: libgstwalpesound.so is not used (...)"). That is
  the case in a build made without GStreamer's headers (`meson setup` warns), with
  `WALPE_WEB_SOUND_SINK=0`, when the file is missing or cannot be trusted
  ([SECURITY.md](SECURITY.md)), and when GStreamer on the machine has no pulsesink or was
  told to rank another output higher. One case the log cannot know: a plugin that was
  built with the headers of a newer GStreamer than the one that runs is refused by
  GStreamer without a word (`gst-inspect-1.0 -b` lists it as blacklisted; tried with a
  plugin marked 1.99 on GStreamer 1.28) - the daemon's line then still says it is used. GStreamer lists the plugin in its registry
  (`~/.cache/gstreamer-1.0`), which it rewrites when a program with another plugin path
  than the last one starts.
- A change applies at once, and a daemon that starts shows the page with the saved values.
- Media start by themselves, without a click on the wallpaper. (Up to 0.14.0 they did
  not: WebKit let media with sound start only after a click on the page, as a browser
  does, so background music never began, and a `<video autoplay>` with a sound track that
  the page had not muted showed its first picture and stood still.)
- While the wallpaper is paused its media are paused, its AudioContexts suspended and the
  page muted; they go on where they stopped.
- **One web wallpaper's own switch and volume**: the rows "Sound" and "Volume" at the top of
  its options, or `walpe-ctl setprop walpe_sound=on|off walpe_volume=0.5`, as for scenes and
  videos. The switch wins over the setting in both directions, the volume is a factor on
  the volume setting; both reach the page that runs - it is not loaded again - and are
  saved with the wallpaper. Every page has its level: two web wallpapers on two monitors
  are independent (measured: one at half its amplitude and the other unchanged; one
  switched off and the other heard). The rows are there for every web wallpaper: whether
  a page makes any sound cannot be known without running it.

### A web wallpaper stays silent although the settings say "on"

`walpe-ctl soundcheck` tells. It prints the status line `sound` and, while a web wallpaper
is shown at a level above 0, asks the sound server once (`pactl`) whether a stream of
WebKit is muted or at 0 % there.

The cause: the server keeps a volume and a mute for every stream - yours, set in the
desktop's mixer - and stores them for later streams. WirePlumber 0.5 files them under the
program's name, and the page process of *every* program made with WebKitGTK is called
"WebKit"; WirePlumber 0.4 and PulseAudio file them under the stream's role, "music" for an
`<audio>`, which music players share. WalPE never changes a stream in the server, in
either direction. So a stream that was muted or turned down to 0 there once comes back
that way every time, across restarts: the page plays, `walpe-ctl status` says "on", and
nothing is heard. It gets there by a mute in the mixer, by another program of the same
name or role that was muted (measured: a muted `paplay --property=media.role=music` on
WirePlumber 0.4 silenced the next start of a web wallpaper), or by a WalPE that ran
*without* `libgstwalpesound.so` while it was muted or at volume 0 (measured on WirePlumber
0.5.19: one such run, and the build with the plugin was silent for good).

The cure, once, while the page plays: in the desktop's mixer unmute the stream "WebKit"
and turn it up (Plasma: the volume applet, "Applications"; pavucontrol: "Playback"), or
`pactl set-sink-input-mute <n> 0; pactl set-sink-input-volume <n> 100%` with the number
`walpe-ctl soundcheck` prints. The server stores that too, and it stays.

### After the sound server was restarted a web wallpaper stays silent

A scene's or a video's sound is back within five seconds when PipeWire or PulseAudio comes
back (the player asks again). A web page's sound is not: WebKit's sound output in the page
process does not recover from a lost connection. An `<audio>` or `<video>` ends with an
error ("... Connection terminated"), an AudioContext goes on saying "running" over a
clock that stands still, and loading the element again from script gets no new stream
either (tried). Measured with PipeWire 1.6.11 (`pipewire-pulse` stopped and started) and
PulseAudio 16.1. `walpe-ctl soundcheck` then finds no stream of WebKit.

What brings it back today: show another wallpaper and then this one again (the page is
made anew), or restart the engine (`walpe-ctl quit`, then start it). `walpe-ctl open` with
the wallpaper that is already shown does not reload its page, and no command does.

If `soundcheck` finds nothing wrong: the daemon's log says at the first web wallpaper
whether `libgstwalpesound.so` is used; a page plays only what GStreamer can decode and put
out, see [BUILDING.md](BUILDING.md#web-wallpaper-sound) for the plugin packages it needs;
`WALPE_WEB_CONSOLE=1` shows the page's own errors in the log.

State of testing: WebKitGTK 2.52 with GStreamer 1.24 against a PulseAudio null sink (WAV,
Ogg Vorbis, WebM with VP8 and Vorbis, an oscillator), by recording the sink's monitor.
The sequences that went wrong in the sound server - mute and unmute, a start while muted,
a quit while muted and the next start, volume 0 and 0.5, sound off and on, a page that
fights the level, Web Audio, each followed by a look at the page's stream, at what the
server stored and at another program's stream of the same role - are
`tests/websound_rig.sh <build> [pipewire|pulse]`, run by hand against a private PipeWire
1.0.5 with WirePlumber 0.4.17 and a private PulseAudio 16.1 with module-stream-restore
(null sinks), and against PipeWire 1.6.11 with WirePlumber 0.5.19, WebKitGTK 2.54.2 and
GStreamer 1.28.9 (Arch Linux, WalPE built there): every check passes with the plugin, and
without it (`WALPE_WEB_SOUND_SINK=0`) 13 of 45 fail there too - a page that starts muted
is at 0 % for good. Nobody has heard it.
