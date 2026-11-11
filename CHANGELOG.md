# Changelog

What changed in each released version of WalPE, newest first, written for people who use
it. Version numbers are `0.<minor>.<patch>`; every release is tagged `v<version>` in git.
Release names are album titles by the band Boris; a point release keeps the name of its
line.

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
