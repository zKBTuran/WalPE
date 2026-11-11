# Changelog

What changed in each released version of WalPE, newest first, written for people who use
it. Version numbers are `0.<minor>.<patch>`; every release is tagged `v<version>` in git.
Release names are album titles by the band Boris, one per release.

## 0.12.2 "Amplifier Worship"

Security fixes and bug fixes for 0.12.1; no new features. Updating is recommended for
everyone who opens wallpapers, previews or Workshop pages that other people made.

### Security

What a hostile file or answer could do in 0.12.1, most serious first. All of these are
closed; the limits are listed in [docs/SECURITY.md](docs/SECURITY.md).

- GIF files (as a wallpaper, a preview or cover art) whose frames do not lie inside their
  picture, as the format requires, were given to the decoder unchecked. They are now
  refused before anything of them is decoded.
- A shader file with one extremely long statement crashed the graphics driver's shader
  compiler, and the daemon with it.
- A video file could keep the daemon busy for minutes to hours while it was opened; the
  daemon answered nothing meanwhile.
- A font file named by a text layer could take hundreds of megabytes and seconds of
  processor time, and could be of any format FreeType reads. Only TrueType / OpenType
  fonts with a bounded number of faces are loaded now.
- A texture file could hand its bytes to picture decoders that were meant to be out of
  reach (Windows cursor and icon formats), and cost seconds of processor time.
- A preview picture could reach decoders outside the allowed formats (SVG, XPM and
  others) in the window, and a small JPEG preview or cover picture could take half a
  gigabyte of memory while it was decoded.
- Cover art fetched over http(s) for a wallpaper with media integration could unpack to
  gigabytes inside the daemon.
- An answer to a Workshop request could crash the window (deeply nested data behind a
  comment or an unusual string).
- A scene of a few hundred bytes could take gigabytes of texture memory through text
  layers, hold millions of particles, or keep the render thread busy for minutes with
  particle systems. Text layers and particles now have totals.
- Shader files of megabytes could stall the render thread for minutes, and names from a
  wallpaper's JSON could push a shader past its size limit.
- A scene script could run its own code at a moment when the engine held a pointer into
  one of the script's audio arrays (no memory error was reached with the QuickJS that
  was tested).
- With `fx_quality=fast` (not the default) a crafted shader crashed the daemon.
- A wallpaper with thousands of options, or with very long option labels, froze the
  window for seconds to minutes.
- `walpe-ctl` printed titles and names from wallpapers and Workshop items with their
  control characters, so an escape sequence in them was obeyed by the terminal.
- A named pipe called `scene.pkg` in a wallpaper folder blocked the load for good; a
  symbolic link there was followed to a package anywhere on the disk, and the options
  window read linked files outside the wallpaper's folder.
- A program of your own account that sent requests to the control socket and never read
  the answers stalled the daemon's other clients and web wallpapers.
- Smaller ones: time zone names from a script that are no time zones, a text size beyond
  what the text library can hold, the script log limit after weeks of logging, reserved
  names in a package.

### Fixes

- A monitor that was plugged in or removed while the daemon was starting was never
  noticed: the wallpapers kept the old layout.
- A paused scene that had to draw a frame (a window moved away, a layer adjusted) moved
  on in time: script clocks jumped and fell back, timers stalled, the paused picture
  changed.
- Right-to-left text (Hebrew, Arabic) in a wrapped text layer was placed on the wrong
  side, or not drawn at all.
- A script on a property that also has a keyframe animation made the value run away
  once the animation stood still.
- Particles: an object that a script stops and plays again fires its burst again; a
  mirrored property animation fires its events at both ends.
- Window: a wallpaper the engine cannot load is no longer reported as set, the reason is
  shown; "Add folder" works on a new installation and says which folders could not be
  added; an import no longer copies files the library never lists; a number field that
  is emptied keeps its value (the frame rate limit used to fall to 5); a refused Workshop
  install is shown also after its dialog was closed.
- A setting that cannot be saved (full disk, unwritable home) is reported instead of
  answered with "ok": `walpe-ctl` prints the reason, `walpe-ctl status` has
  `config_save_error`.
- `walpe-ctl set`: mistyped values of `skip_hidden` and negative margins are refused
  instead of ignored; `walpe-ctl status` shows every saved setting; `walpe-ctl setprop`
  stores text options as typed.
- Switching web acceleration while an import or a Workshop download runs no longer ends
  it.
- Web wallpapers: a WebKit start-up test that never finishes is ended after 40 seconds
  and reported, instead of leaving every web wallpaper black; its child process is no
  longer left behind when the daemon ends.
- Memory that was never given back: the album cover of media integration, wallpaper
  files with a second top-level JSON value, the script memory of a wallpaper whose
  script got stuck (up to 64 MiB per load), two frame-sized textures of a wallpaper that
  is no longer shown.

### Performance

Measured on Mesa's software renderer only; the pictures are byte-identical before and
after. What it gives on a graphics card has not been measured.

- Loading a scene: the render thread's processor time up to the first frame fell from
  1122 to 948 ms, 1874 to 1302 ms and 1736 to 1348 ms for three scenes of the test
  library (sprite analysis, repeated shader text work, sprites decoded twice).
- Refracting particle systems copy only the part of the frame they can read, and not
  again what the system before them just copied: in two scenes at 1920x1080 the pixels
  copied per frame fell from 13.7 to 7.9 and from 22.2 to 9.1 megapixels.
- Puppet layers are no longer sent to the graphics card again while their pose stands
  still: 9 to 4 and 3 to 1 uploads per frame in two scenes.

## 0.12.1 "Absolutego"

The first public version.

### Wallpapers

- Plays Wallpaper Engine scene, video and web projects, GIFs and still images on
  Linux / X11.
- Scenes: layers with parent transforms, effects (Wallpaper Engine's shader dialect
  translated for OpenGL 3.3), particle systems, puppet warp layers, video textures, text
  layers, and a subset of SceneScript on QuickJS. Both scene formats are read.
- Wallpaper options (user properties) in the library window and with `walpe-ctl props`,
  `setprop` and `resetprops`; "Adjust layers" moves, resizes or hides single layers of a
  scene, saved per wallpaper.
- Media integration (title, artist, album art and playback state of any MPRIS player) and
  audio-reactive wallpapers (PulseAudio or pipewire-pulse).
- Wallpaper Engine's builtin effects, shader headers, models and particle sprites are read
  from your own Wallpaper Engine installation; WalPE ships none of them. Video, web, GIF
  and picture wallpapers do not need it.

### Engine

- One render thread with its own EGL context per monitor; monitors of the same size that
  show one scene share one rendering.
- Quality presets `max`, `high` (the default), `balanced` and `low`; every knob can also
  be set on its own, for all wallpapers or for one.
- Work that would not change the picture is left out: unchanged frames are neither drawn
  nor presented, still layers and still effects are drawn once and kept, textures are
  stored at the size they are shown at.
- A wallpaper pauses under a fullscreen window, while its monitor is covered, while the
  display sleeps, and optionally while an application has the focus or on battery power.
- Off by default: rendering on the CPU (Mesa llvmpipe), hardware video decoding, web pages
  on the GPU.
- `walpe-ctl profile` measures GPU time per layer and effect pass; `walpe-gpu-report`
  collects numbers for a report.

### Programs

- `walpe-daemon` (the engine, no GTK), `walpe` (GTK4 library window with wallpaper
  options, settings and a Workshop tab), `walpe-ctl` (command line client), `walpe-tray`
  (tray icon).
- Steam Workshop: browse, search and download through `steamcmd`; existing Steam
  libraries of Wallpaper Engine are found and listed. WalPE never handles a Steam
  password.
- Video import: whatever ffmpeg can read becomes H.264 in MP4, at most 1080p.

### Safety

- A wallpaper's files are treated as untrusted input: every parser has size limits, file
  names stay below the wallpaper's folder, scripts and shaders have CPU, memory and size
  limits, web pages are served from a token-protected loopback server. See
  [docs/SECURITY.md](docs/SECURITY.md).

### Known limitations

- X11 only, no sound, and run on one graphics card only (an NVIDIA laptop card; the last
  set of changes on Mesa's software renderer only; nothing on AMD or Intel graphics). The
  full list is in [README.md](README.md#known-limitations).
