# Wallpaper sound

Scene wallpapers can carry sound layers: rain, wind, a song, the tracks of a music player.
WalPE plays them: wallpaper sound is **on by default**, at half volume (up to 0.14.0 it was
off, at volume 0, and could be switched on from the command line only). A wallpaper without
sound layers behaves and renders exactly as before and costs nothing for it: no player, no
thread, no connection to the sound server. The same switch, volume and mute are for the sound track of a
[video wallpaper](#video-wallpapers) and for the sound a [web wallpaper](#web-wallpapers)
makes.

> **State of testing.** Measured against PulseAudio 16.1 and against PipeWire 1.0.5
> (`pipewire-pulse`) with a null sink, by recording the sink's monitor and analysing the
> recording: no loudspeaker, no sound card (so no drift against a device clock). The
> maintainer has heard scene sound on his own machine with 0.14.0.

## Hearing something

```sh
walpe-ctl set scene_sound=on|off # wallpaper sound: scenes play their sound layers (default: on)
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
written into every `config.json`, so a file that still holds exactly that - volume 0 and
sound not switched on - takes the new defaults at the first start (the log says so, once).
Sound switched on with volume 0, or switched off with a volume above 0, was somebody's
choice and stays as it is. What is set from now on is kept, "off" included.

`walpe-ctl status` has `scene_sound`, `mute`, `volume` and one line `sound`, for example:

```
off (walpe-ctl set scene_sound=on; for one wallpaper: walpe-ctl setprop walpe_sound=on)
on, but the volume setting is 0: silent (walpe-ctl set volume=0.5)
on: 2 stream(s) through PulseAudio, 1 wallpaper(s) with sound shown
on: 2 stream(s) play into nothing, no sound server (Connection refused); 0 heard
on: nothing is playing: 1 wallpaper(s) with sound paused - sound stops while its wallpaper is paused (...)
```

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

State of testing: WebKitGTK 2.52 with GStreamer 1.24 against a PulseAudio null sink (WAV,
Ogg Vorbis, WebM with VP8 and Vorbis, an oscillator), by recording the sink's monitor.
Nobody has heard it.
