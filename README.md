# WalPE

Animated desktop wallpapers for Linux, on X11 and on Wayland compositors with the layer
shell. WalPE plays Wallpaper Engine scene, video and web projects, GIFs and still images.

The current release is 0.14.0 "Flood". Changes per version: [CHANGELOG.md](CHANGELOG.md).

## Features

- Scene wallpapers: effects, particle systems, puppet warp, text layers, video textures.
- Video, web (`type: web`), GIF and still-image wallpapers.
- X11 and Wayland (layer shell); a wallpaper and a render resolution per monitor.
- x86-64 and AArch64 (arm64) support ([arm64 status](docs/BUILDING.md#64-bit-arm-aarch64)).
- Pauses under a fullscreen window, while covered and while the display sleeps.
- Audio-reactive wallpapers, "now playing" data from any MPRIS player, a SceneScript subset.
- Wallpaper options, quality presets and "Adjust layers" per wallpaper.
- Steam Workshop through steamcmd: browse, search, download; WalPE never sees a Steam password.
- A library window (`walpe`), a tray icon (`walpe-tray`) and a command line (`walpe-ctl`).

Not supported: 3D model scenes and `application` wallpapers. The full table is in
[docs/USAGE.md](docs/USAGE.md#what-is-played).

## Status

Everything that is new since 0.12.2 (the Wayland backend, GPU choice, OpenGL ES, the
arm64 build, scene sound and more) was built and tested only on Mesa's software renderer,
a headless compositor, a null sound sink and - the arm64 build - under emulation, with one
exception: the maintainer ran the X11 build of 0.14.0 on his own NVIDIA card before the
release. Nothing else has been run on real AMD, Intel, NVIDIA or ARM hardware or on a real
Wayland desktop.
The details and the known limitations: [docs/LIMITATIONS.md](docs/LIMITATIONS.md).

## Requirements

- GCC or clang, meson and ninja.
- GLib, json-glib, gdk-pixbuf, Pango, fontconfig, libepoxy, EGL, LZ4 and GTK 4.
- The X11 libraries (libX11, libXext, libXinerama) or the Wayland ones (wayland-client,
  wayland-egl, wayland-scanner). At least one of the two backends has to be built.
- Optional: FFmpeg (video), WebKitGTK 6 and libsoup 3 (web), libpulse (audio), QuickJS
  (SceneScript). At run time: `ffmpeg` / `ffprobe` (video import), `steamcmd` (Workshop).
- For the builtin effects of scenes: your own Wallpaper Engine install
  ([what works without one](docs/USAGE.md#wallpaper-engine-assets)).

Package names and what each part is needed for:
[docs/BUILDING.md](docs/BUILDING.md#dependencies).

## Build and install

```sh
./build.sh --prefix=$HOME/.local     # meson setup build, then ninja
meson install -C build               # binaries to <prefix>/bin, modules to <prefix>/<libdir>/walpe
```

Build options, CPU tuning, the X11-only and Wayland-only builds, cross-building for arm64
and running from the build directory: [docs/BUILDING.md](docs/BUILDING.md).

## First steps

```sh
walpe-daemon &          # one desktop window (X11) or layer surface (Wayland) per monitor
walpe-tray &            # the tray icon
walpe                   # the library window (starts the daemon if needed)
walpe-ctl open <project dir | media file>
walpe-ctl status        # the daemon's state as JSON
walpe-ctl set fps=60 preset=balanced
walpe-ctl help          # every command
```

Start `walpe-daemon` from your session: `~/.xinitrc`, the compositor's configuration or
an XDG autostart entry ([how](docs/USAGE.md#starting-with-the-session)). The library is
`~/.local/share/walpe/workshop/`, the settings are in `~/.config/walpe/config.json`.
More: [docs/USAGE.md](docs/USAGE.md).

## Documentation

- [USAGE.md](docs/USAGE.md): what is played, `walpe-ctl`, pausing, Workshop, tray
- [BUILDING.md](docs/BUILDING.md): dependencies, build options, arm64, install
- [SETTINGS.md](docs/SETTINGS.md): every `config.json` key, quality presets, "Adjust layers"
- [ENVIRONMENT.md](docs/ENVIRONMENT.md): every environment switch
- [LIMITATIONS.md](docs/LIMITATIONS.md): known limitations, the state of testing
- [WAYLAND.md](docs/WAYLAND.md): the Wayland backend, what differs from X11
- [GPU.md](docs/GPU.md): drivers, choosing the GPU, OpenGL ES
- [GPU-PRESENT.md](docs/GPU-PRESENT.md): `gpu.conf`, swap interval, driver hints
- [GPU-PROFILING.md](docs/GPU-PROFILING.md): measuring WalPE's GPU use
- [RESOLUTION.md](docs/RESOLUTION.md): render resolution per monitor
- [SCENE-FEATURES.md](docs/SCENE-FEATURES.md): parallax, bloom, camera shake, sprite sheets
- [SOUND.md](docs/SOUND.md): wallpaper sound
- [VIDEO.md](docs/VIDEO.md): playback, import, hardware decoding
- [SECURITY.md](docs/SECURITY.md): limits on wallpaper files, reporting a problem
- [DEVELOPMENT.md](docs/DEVELOPMENT.md): architecture, tests, developer tools

The developer tools (harnesses and generators that are not needed to build, test or use
WalPE) are kept outside this branch: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#developer-tools).

## Licence

BSD 3-Clause, see [LICENSE](LICENSE). The notices that have to accompany binaries (FFmpeg,
QuickJS, the Wayland protocol files): [docs/BUILDING.md](docs/BUILDING.md#licence).

## Disclaimer

WalPE is an independent project. It is not affiliated with or endorsed by Wallpaper
Engine, its developers or Valve, and it ships none of Wallpaper Engine's files. The
wallpapers are yours (your own files, or Workshop items downloaded with your own Steam
account), and Wallpaper Engine's own assets are read from your own Steam copy of
Wallpaper Engine.
