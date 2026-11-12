# Wallpaper sound

Scene wallpapers can carry sound layers: rain, wind, a song, the tracks of a music player.
WalPE plays them when you switch that on. It is **off by default**, and a wallpaper without
sound layers behaves and renders exactly as before.

> **State of testing.** Measured against PulseAudio 16.1 with a null sink, by recording the
> sink's monitor and analysing the recording. Nobody has heard it: no loudspeaker, no
> sound card (so no drift against a device clock), and `pipewire-pulse` was not tried.

## Hearing something

```sh
walpe-ctl set scene_sound=on     # scenes play their sound layers (default: off)
walpe-ctl set volume=0.5         # the volume of all wallpaper sound, 0..1 (default: 0)
walpe-ctl set mute=on|off        # silence that keeps the volume value
walpe-ctl setprop walpe_sound=on|off   # one wallpaper's own switch: wins over scene_sound
```

All three settings apply at once, without loading the wallpaper again, and are kept in
`config.json` (`scene_sound`, `volume`, `mute`). The volume slider of the settings window
and the tray's Mute / Unmute set the same values; there is no switch for `scene_sound` in
the settings window yet. `walpe_sound` is set like every wallpaper option (`--monitor N` /
`--path P`; the wallpaper is loaded again; `resetprops` drops it).

`walpe-ctl status` has `scene_sound`, `mute`, `volume` and one line `sound`, for example:

```
off (walpe-ctl set scene_sound=on; for one wallpaper: walpe-ctl setprop walpe_sound=on)
on, but the volume setting is 0: silent (walpe-ctl set volume=0.5)
on: 2 stream(s) through PulseAudio, 1 wallpaper(s) with sound shown
on: 2 stream(s) play into nothing, no sound server (Connection refused); 0 heard
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
  starting take about a fifth of a second (the sound is faded, not cut).
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
- With `scene_sound` off, a scene with sound layers still looks its sound files up when it
  loads (it opens none and starts no thread), so that switching sound on applies at once.
- 16 sounding layers cost the daemon 36 threads and 80 file descriptors.
- The status words "no sound server (...)" are also shown when the server runs and the
  sink named by `WALPE_SOUND_SINK` does not exist; the reason in the brackets is right.
- Video wallpapers stay silent (their sound track is not decoded), and web wallpapers are
  not touched by any of this.
