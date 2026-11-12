# Changelog

What changed in each released version of WalPE, newest first, written for people who use
it. Version numbers are `0.<minor>.<patch>`; every release is tagged `v<version>` in git.
Release names are album titles by the band Boris, one per release.

## 0.13.0 "PINK" (not released)

The development version. It contains every fix of 0.12.2. **Everything listed here was
built and tested only on Mesa's software renderer (llvmpipe), a headless Wayland
compositor (sway 1.9), a PulseAudio null sink and - the build for 64-bit ARM - under
emulation. Nothing of it has been run on real AMD, Intel, NVIDIA or ARM hardware or on a
real Wayland desktop.** On NVIDIA's driver under X11 with default settings it is meant
to behave as 0.12.2 does, apart from the scene features, two fixes that change the
picture of some scenes (layers with a tint and effects, rope trails; under Scenes) and -
with more than one monitor - the fullscreen pause rule below. What was compared, on the
software renderer: the development
tree before the fixes of 0.12.2 were merged in with
release 0.12.1, call by call (four scenes: the same OpenGL calls) and picture by picture
(the 91 test scenes: the same frames with the new features off); then the pictures of
the final tree with those of that earlier tree.

### Wayland

- A Wayland backend next to the X11 one: one layer-shell surface per output on
  compositors that offer the layer shell (sway, Hyprland, KDE Plasma and others; not
  GNOME's Wayland session). The daemon picks the backend when it starts: `walpe-ctl
  backend`, `walpe-ctl set backend=auto|x11|wayland`, `walpe-daemon --backend`.
- Pause rules get their information from the compositor and from logind where the
  session offers it; `walpe-ctl status` (`pause_rules`) says which rule can work. New:
  pause after a time without input (`walpe-ctl set idle=N`).
- The tray icon uses StatusNotifierItem on Wayland sessions; the window follows the
  desktop's dark / light preference there.
- Web wallpapers on a Wayland session run through Xwayland by default.
- Build options `-Dx11` and `-Dwayland`. `./build.sh -Dwayland=disabled` gives the
  X11-only build, in which the daemon and the web module link no Wayland library.
- A compositor that accepts the connection and does not answer within 10 seconds counts
  as absent: the daemon takes X11 when there is an X display, else it ends and says so.
  The connection of the pause rules gives up after 5 seconds.
- A per-monitor wallpaper or render resolution that was stored under the X11 name of an
  output (`HDMI-1`) is found on Wayland, where the same connector is `HDMI-A-1`.
- Wallpapers were shown on a headless sway 1.9 only. Known and left as they are: a
  wallpaper that is not shown keeps being drawn when "pause when covered" is off; a lock
  screen pauses through "pause when covered" only, unless the locker tells logind; with
  no monitor left `walpe-ctl status` still lists the monitors; while the daemon waits for
  a compositor to come back, `walpe-ctl` says it is not running, and what it was sent is
  not carried out later; a session started with a one-time socket (`WAYLAND_SOCKET`)
  cannot be restarted in place.

### Graphics cards

- `walpe-ctl gpu` lists the graphics devices; `walpe-ctl set gpu=auto|integrated|
  discrete|<device>` chooses the one that renders on a machine with two. `auto` (the
  default) changes nothing.
- An OpenGL ES 3.1 / 3.2 path for GPUs without desktop OpenGL 3.3 (`walpe-ctl set
  gl_api=auto|gl|gles`); effect shaders are translated to GLSL ES. On Mesa's software
  renderer every effect shader of the 91 test scenes compiles; pictures differ slightly
  from desktop OpenGL, because OpenGL ES has no texture LOD bias (minified textures are
  a little softer).
- On OpenGL ES an effect shader with more than 256 KiB of source is not compiled: a
  warning names it and its effect is not drawn. A source of that size is not translated
  to GLSL ES, and was handed to the driver as it was written. Desktop OpenGL still
  compiles such a source as written.
- `walpe-ctl gpu-stats`, and the same figures in `walpe-gpu-report` and in a profile: GPU
  busy, memory, power and temperature from the kernel's statistics on AMD and Intel.
- Driver hints of `gpu.conf` are handed to Mesa drivers where Mesa has a counterpart.
- Fallbacks for drivers without S3TC, with a smaller largest texture, or without float
  render targets.
- With `gpu=auto` the daemon does not read the PCI id list when it starts, and on
  NVIDIA's driver the GPU figures of `walpe-ctl status` read no file of `/proc` or
  `/sys`.

### Render resolution

- A render resolution per monitor: `native` (the default), 720p to 4K, or any size
  (`walpe-ctl resolution`, Engine settings > Resolution). The window asks once about new
  monitors; an existing installation is not asked and nothing changes size.
- Imported and converted videos are no longer tied to 1080p (`video_resolution`; a
  configuration from an older version keeps 1080p until you change it).
- Effects on colour-only layers of a scene are computed as wide as the longer side of
  a chosen render size where that is more than 2048 pixels (0.12: 2048 at most), and such
  a layer is sharper then. At a monitor's own resolution (`native`, the default) they
  stay at 2048 pixels as in 0.12, also on a monitor wider than that.

### Scenes

- Camera parallax, camera shake, bloom, sprite-sheet layers and their script control,
  and a script camera (zoom and position). Each has a switch (`walpe-ctl set bloom=off`,
  ...). **They are on by default and change the picture of the scenes that ask for
  them:** with default settings 16 of the 91 test scenes look different from the 0.12
  releases, because bloom, camera shake, sprite sheets and the script camera are on.
  `walpe-ctl set bloom=off camera_shake=off sprite_sheets=off script_camera=off` gives
  the earlier pictures back, each switch for its own feature. The look of the camera
  shake is an estimate; HDR bloom is not implemented. Limits: the sprite animations of
  one scene have at most 1048576 frames together (a layer beyond that shows its first
  frame), and a scene zoom that no float holds counts as zoom 1.
- The switch `script_texanim` (scripts drive sprite-sheet animations) is read when a
  wallpaper loads, as `sprite_sheets` is: changing it loads the wallpapers again.
  Switched off on a running scene it left the animations as the scripts had last set
  them, which is not the picture of a scene loaded with the switch off.
- A layer with a tint and effects keeps its effects. Such a layer could be drawn without
  them, depending on whether an effect target was new or taken back from the pool, so
  its picture was not always the same. Two scenes of the test library draw another
  picture now, whatever the feature switches say.
- A rope trail of a particle system is as long in seconds as its scene says at every
  frame rate. It was longer at low frame rates (a trail of 24 points over 0.2 seconds:
  twice as long at 60 Hz, four times at 30 Hz). Scenes with rope trails can look
  different at 60 Hz and below: the trails are shorter there.
- The Fluid Simulation effect runs on the grid its file declares, at a fixed 60 steps a
  second whatever the frame rate (`fluid_sim`, `fluid_quality`).
- Scene sound layers (WAV, MP3, FLAC, Ogg) through PulseAudio / PipeWire, **off by
  default**: `walpe-ctl set scene_sound=on volume=0.5`; `mute`; per wallpaper
  `walpe-ctl setprop walpe_sound=on|off`. The `volume` setting now has an effect.
  The sound layers of a scene open their streams one at a time, a few milliseconds
  apart: asked for in the same instant, some were refused by the sound server. A
  connection it still refuses is asked for again at once. A layer waits while the others
  get their streams; only behind a stream the server has not answered for a quarter of a
  second it plays into nothing meanwhile, and asks for its turn again a quarter of a
  second later. Measured against a null sink by recording it; nobody has heard it.

### Video

- Hardware decoding chooses its decoder by what the machine and its FFmpeg offer and by
  the GPU that renders (`walpe-ctl set hwdec=off|auto|<api>`: VA-API, NVDEC, VDPAU, Vulkan
  Video, V4L2); on NVIDIA's driver NVDEC stays first. Optional zero-copy
  (`video_zerocopy`). Both off by default.

### Pausing

- "Pause for fullscreen apps" (`pause_fullscreen`) pauses the monitor the focused
  fullscreen window is on; the wallpapers of the other monitors keep running. Up to 0.12
  a fullscreen window on one monitor paused every monitor. **This changes the default
  behaviour on a machine with more than one monitor**; `walpe-ctl set fs_all=on`
  (`pause_fullscreen_all`, also a switch in the settings window) brings the old behaviour
  back. On X11 the monitor is found from the window's rectangle: the monitor holding
  most of the window, whatever border or gap a tiling window manager keeps around it,
  and every monitor a window across several covers at least half of. On Wayland it is
  the output the compositor names for the window; where it names none, every monitor
  pauses as before and `walpe-ctl status` (`pause_rules`) says so. Status reports
  `fullscreen` and `fullscreen_pause` per monitor. Tested with two monitor rectangles
  on Xvfb and two outputs of a headless sway 1.9; not on real monitors.

### Other

- Pictures from outside go to fewer decoders. A picture or GIF file chosen as the
  wallpaper is shown only as PNG, JPEG, GIF, BMP, TIFF, WebP, TGA, AVIF or JPEG XL: a
  file of another kind (an SVG document, a Windows icon ...), which earlier versions
  handed to whatever loader gdk-pixbuf had for it, is refused now. Cover art of a wallpaper with
  media integration is decoded as JPEG, PNG, GIF, WebP or BMP only
  ([SECURITY.md](docs/SECURITY.md)).
- The source builds for 64-bit ARM (aarch64) as well as x86-64, with meson cross files:
  cross-compiled with 0 warnings, and 65 of the 68 tests that need neither a display nor
  an OpenGL context pass under emulation. Not run on ARM hardware. OpenGL ES 3.1 is the
  least an ARM GPU must offer.
- Test setups for AddressSanitizer / UndefinedBehaviorSanitizer and ThreadSanitizer,
  end-to-end tests on a headless Wayland compositor.
- Library window: the wallpaper grid takes 64 % of the window's width by default (6
  columns at 1920 pixels) and the details panel the rest, at least 460 pixels. The share
  follows the window's size until you move the handle between the two; from then on
  yours is kept. Before, the handle stood at a fixed 780 pixels, and in a window that a
  window manager tiles or maximizes after showing it the grid could end up with two
  columns. The position is not saved across starts.
- Build: `./build.sh` builds the installed programs and modules only (the new ninja
  target `programs`), which makes a fresh build much shorter. The test programs and the
  developer tools are built by `meson test`, or one by name (`ninja -C build
  sceneshot`); a plain `ninja -C build` builds them too. The installed binaries are the
  same as before.
- Two changes that are meant to change no picture: an effect takes memory for the passes
  it has (the pass table of an effect with one or two passes took 44 kB and takes 1.4 or
  2.8 kB now), and a control point's `flags` in a particle file is converted like the
  other numbers of a wallpaper file, so a value that no integer holds gives the same
  result on every machine.
- Tests: in a build without QuickJS (`-Dscenescript=disabled`) `fx-alpha` and `layers`
  leave out the checks that need a script instead of failing, and `sig-wl` no longer
  waits for ever where `TMPDIR` is too long for a socket path (it makes its directory in
  `/tmp` then).

### Known limitations

- See the state of testing at the top of this section. The full list is in
  [README.md](README.md#known-limitations).

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
