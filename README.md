# WalPE

Animated desktop wallpapers for Linux/X11: **Wallpaper Engine scene, video and web
projects, GIFs and still images**, rendered by a small GTK-free daemon (one EGL render
thread per monitor), with a GTK4 library window, a tray icon and a command line client.
Written in C; version 0.12.1.

> **WalPE is an independent project. It is not affiliated with or endorsed by Wallpaper
> Engine, its developers or Valve, and it ships none of Wallpaper Engine's files.** The
> wallpapers are yours (your own files, or Workshop items downloaded with your own Steam
> account), and Wallpaper Engine's own assets - its builtin effects, shader headers,
> models, particle files and fonts - are read from your own Steam copy of Wallpaper
> Engine. Without a Wallpaper Engine installation, video, web, GIF and picture wallpapers
> work in full and a scene shows everything that is in the wallpaper's own files; what
> the scene takes from Wallpaper Engine's `assets/` - for most scenes that includes their
> effects - is missing ([Wallpaper Engine assets](#wallpaper-engine-assets)).

Licence: BSD 3-Clause, see [Licence](#licence). Changes per version:
[CHANGELOG.md](CHANGELOG.md).

Contents: [Features](#features) · [Build](#build) · [Install](#install) · [Run](#run) ·
[walpe-ctl](#walpe-ctl) · [Settings](#settings) · [Pausing](#pausing) ·
[Wallpaper Engine assets](#wallpaper-engine-assets) · [Video](#video) ·
[Steam Workshop](#steam-workshop) · [Tray icon](#tray-icon-walpe-tray) ·
[Known limitations](#known-limitations) · [Architecture](#architecture) ·
[Tests](#tests) · [Developer tools](#developer-tools) · [Licence](#licence)

More detail lives in `docs/`: [SETTINGS.md](docs/SETTINGS.md) (every `config.json` key,
quality presets, "Adjust layers"), [ENVIRONMENT.md](docs/ENVIRONMENT.md) (every environment
switch), [VIDEO.md](docs/VIDEO.md), [GPU-PRESENT.md](docs/GPU-PRESENT.md) (`gpu.conf`),
[GPU-PROFILING.md](docs/GPU-PROFILING.md), [SECURITY.md](docs/SECURITY.md).

## Features

| Content | Status | Notes |
|---|---|---|
| **Scene** (node graph, `objects[]`: the common format) | yes | `scene.pkg` extraction, `.tex` textures (LZ4 mips; DXT1/3/5 and R8/RG88 kept GPU-native; embedded PNG/JPEG; stored mip chains), parent transforms, camera, text layers |
| Scene, legacy format (`object` / `layers`) | yes | |
| Effects | yes | Wallpaper Engine's GLSL dialect compiled for GL 3.3 (`#include`, combos, HLSL helpers, implicit conversions repaired from the shader source before the first compile); multi-pass chains with named targets, `copy` passes, masks, compose / fullscreen layers. Builtin effects need your Wallpaper Engine install's `assets/` ([below](#wallpaper-engine-assets)) |
| Particle systems | yes | emitters, initializers, operators, children, instance overrides, control points; sprites, sprite trails and rope ribbons; perspective systems; animated sprite sheets; refracting materials |
| Puppet warp layers (`.mdl`) | yes | skinned 2D mesh, bones, keyframed bone animations |
| Video textures (MP4 inside a `.tex`) | yes | one shared decoder per unique texture |
| SceneScript | subset | QuickJS: scripts on layer `text` / `visible` / `alpha` / `origin` / `scale` / `angles` / `color`, on effect visibility and material constants, media events, property animations, cursor events |
| Media integration | yes | the "now playing" session of any MPRIS player: title, artist, album art with its colours, playback state, timeline; for scene scripts and web pages |
| Audio-reactive wallpapers | yes | captures the default output monitor (PulseAudio or PipeWire's pipewire-pulse) while such a wallpaper is visible |
| **Video** | yes | in-house MP4 demuxer + libavcodec H.264; other codecs and containers are converted once with ffmpeg ([Video](#video)) |
| **Web** (`type: web`) | yes | WebKitGTK 6 off screen, pages served from a token-protected loopback server; `wallpaperPropertyListener`, media and audio listeners; real mouse input on the empty desktop |
| GIF / still images | yes | images render once (no idle cost) |
| User properties | yes | options panel in the GUI and `walpe-ctl props` / `setprop` / `resetprops`; for video, GIF and image wallpapers the built-in `walpe_fit` (cover, contain, stretch, center), `walpe_pos_x` / `walpe_pos_y`, `walpe_flip` |
| Presets (`project.json` with `dependency` + `preset`) | yes | the base item is found next to the preset, in the Steam libraries or the WalPE library |
| Steam Workshop | yes | browse, search and download through steamcmd; WalPE never handles a Steam password ([Steam Workshop](#steam-workshop)) |
| Sound | no | the renderer is silent (web pages play their own media) |
| 3D model scenes, `application` wallpapers | no | |

## Build

```sh
./build.sh                     # generic build: meson setup (or --reconfigure) build, then ninja
./build.sh --tuning=alderlake  # the same with -march=alderlake and friends (see below)
# or by hand:
meson setup build
ninja -C build
```

`build.sh` (POSIX sh) runs ninja with as many jobs as the machine has CPUs (`-j <CPUs> -l
<CPUs + 2>`); `WALPE_JOBS` / `WALPE_LOAD` override `-j` / `-l`, `WALPE_BUILDDIR` the build
directory (default `build`). `--tuning=<value>` is passed on as `-Dnative_tuning=<value>`,
every other argument goes to `meson setup` (`./build.sh -Dweb=disabled`). An existing build
directory is reconfigured, not wiped, and keeps the options it was set up with.

The default is a release build (`-O3`, `NDEBUG`, LTO, hardening flags) with **generic CPU
tuning**: the binaries run on any CPU of the architecture they were built for.

### Dependencies

A C compiler (GCC or clang), meson and ninja. Package names are Gentoo's; the pkg-config
module is what `meson setup` looks for, to map them to another distribution.

| pkg-config module | Gentoo package | Needed for |
|---|---|---|
| `glib-2.0`, `gobject-2.0`, `gio-2.0` | `dev-libs/glib` | everything |
| `json-glib-1.0` | `dev-libs/json-glib` | everything |
| `gdk-pixbuf-2.0` | `x11-libs/gdk-pixbuf` | pictures |
| `pangocairo`, `pangoft2` | `x11-libs/pango` | text layers |
| `fontconfig` | `media-libs/fontconfig` | fonts of text layers |
| `epoxy` | `media-libs/libepoxy` | OpenGL |
| `egl` | `media-libs/libglvnd` with `media-libs/mesa` or the NVIDIA driver | OpenGL contexts |
| `x11`, `xext`, `xinerama` | `x11-libs/libX11`, `libXext`, `libXinerama` | desktop windows |
| `liblz4` | `app-arch/lz4` | `.tex` textures |
| `gtk4` | `gui-libs/gtk` | the library window `walpe` |
| `xi` (optional, found automatically) | `x11-libs/libXi` | clicks on the empty desktop |
| `xrandr` (optional, found automatically) | `x11-libs/libXrandr` | monitor names for per-monitor wallpapers |

Optional parts (meson `auto`; each takes `-D<option>=enabled|disabled`):

| Option | pkg-config module | Gentoo package | Without it |
|---|---|---|---|
| `video` | `libavcodec`, `libavutil` | `media-video/ffmpeg` | video wallpapers and video textures do not load |
| `web` | `webkitgtk-6.0`, `libsoup-3.0` | `net-libs/webkit-gtk:6`, `net-libs/libsoup:3.0` | web wallpapers report an error |
| `audio` | `libpulse-simple`, `libpulse` | `media-libs/libpulse` | audio spectra stay 0 |
| `scenescript` | none: `quickjs/quickjs.h` and `libquickjs` (searched in `<libdir>/quickjs`) | `dev-lang/quickjs` | scripted values stay static |
| `tray` | `xrender` (and `gtk4`) | `x11-libs/libXrender` | no `walpe-tray` |

`libsoup-3.0` is also used, when present, to fetch album art over http(s).

QuickJS: wallpaper scripts are untrusted and run inside QuickJS, so its age matters. `meson
setup` warns when the installed library is older than the 2024-01-13 release (the 2021-03-27
release lacks fixes for memory errors that plain script can reach); see
[SECURITY.md](docs/SECURITY.md).

At run time: `ffmpeg` and `ffprobe` for importing and converting videos, GStreamer elements
for video inside web pages (`media-libs/gst-plugins-good`, `media-plugins/gst-plugins-vpx`
or `media-plugins/gst-plugins-libav`; missing ones are named in `walpe-ctl status` as
`web_missing_gst`), `steamcmd` for Workshop downloads.

### Build options

| Option | Default | Meaning |
|---|---|---|
| `-Dnative_tuning=generic` | default | no `-march`, `-mtune` or cache parameters, `-pipe` only. `off` is another name for it |
| `-Dnative_tuning=alderlake` | | `./build.sh --tuning=alderlake`: `-march=alderlake -mabm -mno-kl -mno-pconfig -mno-sgx -mno-widekl -mshstk --param=l1-cache-line-size=64 --param=l1-cache-size=32 --param=l2-cache-size=18432 -pipe`. **Such a build only runs on Alder Lake-class Intel CPUs** (12th generation Core and newer) and may die with "Illegal instruction" elsewhere |
| `-Dnative_tuning=native` | | `-march=native -pipe`; an error if the compiler rejects it |
| `-Dnative_tuning=auto` | | `-march=native -pipe` if the compiler accepts it, else `-pipe` only |
| `-Dhardening=true` | default | stack protector, stack clash and control-flow protection, `_FORTIFY_SOURCE`, format-security errors, full RELRO and immediate binding |
| `-Dvideo`, `-Dweb`, `-Daudio`, `-Dscenescript`, `-Dtray` | `auto` | the optional parts above |

The code is plain C without hand-written SIMD for a particular `-march`; tuning only steers
the compiler's auto-vectoriser and scheduling. Each tuning flag is probed first, so a
compiler that does not know one drops it. With LTO the flags also go on the link line.

The web and video backends are separate modules, `walpe-web.so` and `walpe-video.so`, loaded
the first time a wallpaper needs them: from `$WALPE_MODULE_DIR`, the daemon's own directory
(build tree) or `<prefix>/<libdir>/walpe` (where `meson install` puts them). The daemon
itself links neither WebKit, GTK, GStreamer nor FFmpeg.

## Install

```sh
./build.sh --prefix=$HOME/.local     # choose the prefix at setup time
meson install -C build               # binaries to <prefix>/bin, modules to <prefix>/<libdir>/walpe
```

Or run from the build directory, or copy by hand: `walpe-daemon`, `walpe`, `walpe-ctl`,
`walpe-tray` and the two modules `walpe-video.so` / `walpe-web.so` into one directory (the
daemon loads the modules from its own directory, and the GUI starts `walpe-tray` from
there). Without the modules video and web wallpapers do not load; `walpe-ctl status` then
shows `"modules": "... not loaded"`.

Start the daemon from your session, for example in `~/.xinitrc` before `exec dwm`:

```sh
walpe-daemon &
walpe-tray &
```

A systemd user unit (`data/walpe.service`) and XDG autostart entries
(`data/walpe-autostart.desktop`, `data/walpe-tray-autostart.desktop`) are provided for
desktops that use them.

## Run

```sh
walpe-daemon            # one desktop window per monitor
walpe-daemon --windowed # a 960x540 preview window instead
walpe                   # the library window (starts the daemon if needed)
walpe-ctl status
walpe-ctl open <project dir | media file>
```

| Path | What |
|---|---|
| `~/.local/share/walpe/workshop/` | the library: project folders or loose media files; imported videos in `imported/` |
| `~/.config/walpe/config.json` | settings ([SETTINGS.md](docs/SETTINGS.md)) |
| `~/.config/walpe/gpu.conf` | presentation and driver hints ([GPU-PRESENT.md](docs/GPU-PRESENT.md)) |
| `~/.cache/walpe/` | unpacked packages, converted videos, thumbnails, `daemon.log`, profiles |
| `$XDG_RUNTIME_DIR/walpe.sock` | the control socket (newline-delimited JSON) |

Scene objects are drawn in `objects[]` order with parent transforms, Y-up coordinates and
angles in radians, as in Wallpaper Engine.

## walpe-ctl

`walpe-ctl help` prints the full list. Exit status: 0 done, 1 bad input, 2 no daemon runs,
3 the daemon refused (the reason is in the reply). A wallpaper that cannot be loaded leaves
the one before it on screen; `walpe-ctl status` says why (`load_error`, `load_restored`).

| Command | Does |
|---|---|
| `open <path> [--monitor N]` | set the wallpaper on every monitor, or on monitor N (index from `status` "monitors"). A video the player cannot read is converted first; the wallpaper changes when it is ready |
| `add <video>` | import a video into the library ([Video](#video)) |
| `cancel-import` | stop a running import or conversion |
| `pause` / `resume` | |
| `status` | the daemon's state as JSON: monitors, what they show, why one is paused, import progress, renderer, counters |
| `set key=value ...` | change settings ([SETTINGS.md](docs/SETTINGS.md)); refused as a whole when one key is unknown or one value has the wrong kind |
| `quality [--json] [--effects]` | the quality knobs, what the wallpapers on screen cost and what each preset would save |
| `quality --monitor N \| --path P key=value ... \| reset` | quality knobs for one wallpaper only |
| `props [--monitor N \| --path P]` | the options (user properties) of a wallpaper |
| `setprop key=value ...` | set wallpaper options (saved; one reload) |
| `resetprops` | drop all option overrides of the wallpaper |
| `layers ...` | "Adjust layers": list, move, resize, hide the layers of the running scene (`walpe-ctl layers --help`) |
| `click X Y [BUTTON [COUNT]]` | click the wallpaper at a screen pixel, as a click on the empty desktop there does |
| `scroll X Y DY [DX]` | turn the wheel over a web wallpaper |
| `profile [SECONDS] [--json] [--sync]` | GPU profile of what every monitor shows ([GPU-PROFILING.md](docs/GPU-PROFILING.md)) |
| `install <id \| url>` | download a Workshop item with steamcmd into the library |
| `steam-libraries` | list the Steam Wallpaper Engine workshop folders that were found |
| `quit` | stop the daemon |

```sh
walpe-ctl set fps=60 parallax=on fs=off covered=on transparent_windows=st,foot
walpe-ctl set preset=balanced
walpe-ctl setprop speed=2 clock=off title="a b"
walpe-ctl setprop walpe_fit=contain          # video / GIF / image: cover | contain | stretch | center
walpe-ctl open ~/wallpapers/123 --monitor 1
```

## Settings

All settings are in `~/.config/walpe/config.json`, written by the daemon; change them in
the settings window, the tray menu or with `walpe-ctl set`. The complete list with defaults
is [docs/SETTINGS.md](docs/SETTINGS.md); the most used:

| `walpe-ctl set` | Default | |
|---|---|---|
| `fps=N` | 60 | frame rate limit of scenes and web pages (5-200); never above the monitor's refresh rate |
| `preset=max\|high\|balanced\|low` | high | quality for GPU load, all knobs at once (below) |
| `fs=on\|off`, `covered=on\|off`, `dpms=on\|off` | on | pause under a fullscreen window / while covered / while the display sleeps |
| `focus=run\|limit\|pause`, `focus_fps=N` | run, 15 | what a wallpaper does while an application has the focus on its monitor |
| `battery=on\|off`, `battery_fps=N` | off, 0 | pause / limit on battery power |
| `shared=on\|off` | on | render a scene once for same-size monitors |
| `software=on\|off` | off | render on the CPU (Mesa llvmpipe) instead of the GPU |
| `hwdec=on\|off` | off | decode videos on the GPU |
| `web_accel=on\|off` | off | web pages on the GPU |
| `we_assets=<dir>` | auto-detect | your Wallpaper Engine install |

**Quality presets.** `max` is the exact picture; `high` (the default) is the same except
that a reduced texture or effect may be drawn up to 2 % enlarged; `balanced` redraws slow
effects every second frame and computes effects at the size they have on screen; `low` also
halves texture sizes, renders at 75 % of the monitor's resolution, leaves cosmetic
full-screen passes out and limits the wallpaper to 30 frames a second. Every knob can be set
on its own, globally or per wallpaper; the table of what each preset sets is in
[SETTINGS.md](docs/SETTINGS.md#quality-presets).

**Adjust layers.** For a scene: move, resize or hide single layers, saved per wallpaper
(GUI: below the wallpaper's options; command line: `walpe-ctl layers`). See
[SETTINGS.md](docs/SETTINGS.md#adjust-layers).

**Presentation and driver hints** (swap interval, frame limiter, NVIDIA's `__GL_*` hints,
releasing GPU memory while paused, frame clocks on one grid) are read from
`~/.config/walpe/gpu.conf` at start: [GPU-PRESENT.md](docs/GPU-PRESENT.md).

**Environment switches** for diagnosis and for comparing two behaviours on your hardware:
[ENVIRONMENT.md](docs/ENVIRONMENT.md).

## Pausing

Per monitor, a wallpaper pauses while: you paused it; a focused fullscreen window is on the
monitor (`pause_fullscreen`); the display is in power save or the screensaver runs
(`pause_dpms`); or visible normal windows cover 85 % of the monitor or more
(`pause_covered`; docks, desktop windows, menus, minimized windows and the windows of other
dwm tags never count). See-through windows (WM_CLASSes in `transparent_windows`, or
`_NET_WM_WINDOW_OPACITY` below 100 %) count as covering while `cover_transparent` is on.

A paused scene's clock stops (no jump on resume). A paused monitor does no decoding,
rendering or capturing; a video decoder sleeps once every monitor showing it is paused, a
web page is suspended once no monitor shows it, audio capture closes, and the daemon idles
at about two wake-ups a second.

## Wallpaper Engine assets

Most scenes use files that belong to Wallpaper Engine itself, not to the wallpaper: its
**builtin** effects, the shader headers (`common*.h`) that effect shaders include, util
models, particle files and sprites, fonts. WalPE ships none of them: it reads them from
your own Wallpaper Engine install (the `assets/` folder next to `wallpaper64.exe`).

| | Without a Wallpaper Engine install |
|---|---|
| video, web, GIF and picture wallpapers | work in full: nothing of the install is read |
| a scene's layers, particle systems and effects whose files are all in the wallpaper | work |
| builtin effects (`effects/<name>/`: effect file, materials, shaders, textures) | not built; the log names each (`fx: ... not found (not in the project; WE assets not found)`). A layer with a picture, text or video is shown without the effect; a solid or compose layer, which is nothing but its effects, is not drawn |
| a wallpaper's own effects whose shaders include Wallpaper Engine's headers | fail to compile and are left out like builtin ones, unless the wallpaper carries the headers |
| layers on builtin models (`models/util/`) | cannot be built (`scenenode: model ... not found`) |
| builtin particle systems; builtin particle sprites | left out; drawn as a soft dot |
| builtin fonts of text layers | a system font is used |

The Workshop tab and `walpe-ctl install` read nothing of the install; downloads run through
`steamcmd` with your own Steam login ([Steam Workshop](#steam-workshop)).

WalPE looks, in this order (first hit wins):

1. `WALPE_WE_ASSETS=/path/to/wallpaper_engine/assets` (or the install root)
2. the config key `we_assets` (`walpe-ctl set we_assets=<dir>`; an empty value goes back to
   auto-detection)
3. the opened wallpaper's own path: `.../steamapps/workshop/content/431960/<id>` ->
   `.../steamapps/common/wallpaper_engine/assets`
4. every Steam library: `~/.local/share/Steam`, `~/.steam/steam`, `~/.steam/root`, Flatpak
   Steam, and each `"path"` in their `libraryfolders.vdf`

A directory counts when it has `shaders/`, `effects/` or `materials/` (or is an install
root with such an `assets/`). `walpe-ctl status` shows the result as `we_assets` and
`we_assets_source`. Proton / Wine installs work the same way.

## Video

The player reads H.264 in MP4. `walpe-ctl add <file>` (or **Import** in the GUI) turns
anything ffmpeg can read into that: H.264 High, yuv420p, scaled to fit within 1920x1080
(never upscaled), source frame rate kept, audio dropped; a file that already conforms is
remuxed. `walpe-ctl open` on another format converts it once into `~/.cache/walpe/video`.
ffmpeg runs as a child process with an argument vector (no shell).

Hardware decoding (`walpe-ctl set hwdec=on`; NVDEC, VA-API or VDPAU through FFmpeg) is off
by default; software decoding takes over by itself when it fails, and `walpe-ctl status`
shows what the decoder really does (`video_decoder`). Details, the cost of each step and
the switches for it: [VIDEO.md](docs/VIDEO.md).

## Steam Workshop

The Workshop tab of the GUI browses, searches and downloads; `walpe-ctl install <id|url>`
downloads from the command line. Downloads go through `steamcmd` with its cached login:
run `steamcmd +login <account> +quit` once in a terminal, then `walpe-ctl set
steam_user=<account>`. WalPE never sees or stores a Steam password. Existing Steam
Wallpaper Engine libraries are found and listed as they are.

Optional and off by default, with your own Steam Web API key (kept in
`~/.config/walpe/workshop.json`, mode 0600, sent to api.steampowered.com only): "Subscribe
on Steam when downloading" and "Also unsubscribe on Steam" when deleting. Deleting a
wallpaper in the GUI moves every copy of the item to the trash.

What Steam's servers answer and what a wallpaper's files say is treated as data from
strangers: see [SECURITY.md](docs/SECURITY.md).

## Tray icon (walpe-tray)

Left click opens (or raises) the WalPE window, middle click pauses / resumes, right click
opens the menu (open, pause / resume all, mute, next / random wallpaper, per-monitor
wallpaper and recent ones, pause switches, settings, restart engine, quit). The icon dims with a
pause badge while paused and turns grey with a red badge when the engine is not running.

It is a separate small process. It docks into classic XEmbed trays (dwm systray patch,
stalonetray, trayer, polybar, xfce4-panel) and uses StatusNotifierItem + dbusmenu when an
SNI host is running (KDE, waybar, GNOME AppIndicator extension). The GUI's "Tray icon
(start with WalPE)" switch starts and stops it and remembers the choice in
`~/.config/walpe/tray.ini` (`start_with_gui`, `confirm_quit`, `backend=auto|sni|xembed`).
`walpe-tray --quit` closes it, `walpe-tray --menu` pops its menu at the pointer (bind it to
a key), `--action=next|random|pause-toggle` runs a menu entry.

## Known limitations

- **Run on one graphics card.** The maintainer runs WalPE on one NVIDIA laptop card
  (RTX 4050, proprietary driver, X11). A figure in the documents comes from that machine
  only where the text says so; all other sizes, counts and timings are counts or
  software-rendering numbers, not the performance of a particular card. Version 0.12.1 was
  confirmed on that card as it stood before its last set of changes; that last set was
  built and tested only on Mesa's software renderer (llvmpipe). Nothing has been run on AMD
  or Intel graphics. Measure on your own card with `walpe-ctl profile`.
- **X11 only.** The daemon draws into override-redirect X11 windows; there is no Wayland
  backend.
- **No sound.** Scene sound objects are not played and the `volume` setting is stored only;
  a web page's own sound is not muted by it. Audio bars and the music title / cover block
  were written and tested without sound.
- **Builtin effects need a Wallpaper Engine install** (see above); WalPE has no shaders of
  its own to replace them.
- **SceneScript is a subset.** Layers cannot be created or destroyed by script
  (`thisScene.createLayer()` returns a stand-in that is not drawn); scripts on properties
  the renderer does not drive (`parallaxDepth`, a script-driven camera or zoom, among
  others) have no effect. Workshop item 3397287008 logs `'thisProject' is not defined` from
  its media handler: a gap in the script interface; the wallpaper runs.
- **Scenes are composed in 2D.** The x / y tilt of a layer is ignored; the scene settings
  for mouse parallax of the camera, camera shake, camera fade and bloom are not applied (an
  option bound to one of them gets a note in the options panel); 3D model scenes do not
  load. Not read at all: `perspectiveoverridefov`, rope `uvscale` / `uvscrolling`,
  `copybackground`. The Fluid Simulation effect runs but does not move as in Wallpaper
  Engine. Picture layers that are sprite sheets are not animated; smoke made from a video
  plays at the rate of its clip.
- **Particles.** How many particles a system shows follows Wallpaper Engine's documentation
  (the cap is the particle file's `maxcount`). Where that leaves room for another reading
  there is a switch for it (`WALPE_PT_CAP=count`, `WALPE_PT_FADE=span`,
  `WALPE_PT_SIZE=full`, `WALPE_PT_WORLD=scaled`, `WALPE_PT_CAMERA=fixed`,
  [ENVIRONMENT.md](docs/ENVIRONMENT.md)), for comparing with Wallpaper Engine on a real
  screen.
- **Quality presets.** At `low`, Workshop item 1251123383 is about 18 of 255 darker than at
  `high` (the particle part of `low`), and six other scenes of the test library differ by 2
  to 5 of 255 in mean level. A mix no preset uses (`fx_rate=2 fx_rate_mode=slow` with
  `fx_quality=fast`) was only run for crashes. `preset=max` for one wallpaper only does not
  switch `tex_downscale` off (that switch belongs to the whole daemon).
- **Video is H.264 in MP4 only** without a conversion; other codecs and containers go
  through ffmpeg once, and imported or converted videos are limited to 1080p. A video
  wallpaper on two monitors is uploaded once per monitor.
- **Off by default and not confirmed on a graphics card:** `hwdec` (hardware video decoding),
  `web_accel`, `skip_hidden`, `pointer_share`, `video_limit`, `frame_align` and the
  `video_upload` alternatives. Hardware decoding and web acceleration fall back by
  themselves.
- **"Skip hidden areas" under a compositing manager** (`skip_hidden=always`) cannot know
  which windows the compositor itself makes see-through: behind those the wallpaper
  stands still; see SETTINGS.md.
- **One scene's picture depends on reused effect targets.** In Workshop item 3465215190 a
  small area (about 66 x 40 pixels of a 960 x 540 frame, by up to 18 of 255) looks
  different depending on whether an effect target was just made or taken back from the
  pool: one pass of a layer with an opacity below 100 % reads a target that was not written
  in that frame, and which pass is not found yet. Unused targets are freed after 5 s
  (`WALPE_FX_POOL_IDLE_MS`), so the other picture shows after the wallpaper was paused or
  hidden that long. `WALPE_FX_ALPHA=after` does not have it. It is the only one of the 91
  scenes of the test library that behaves so.
- **Two monitors.** The leading monitor of a shared scene does not apply its own
  "application focused" limit to its own window; a scene that only follows the pointer
  still ticks (drawing nothing) while the pointer rests; a pointer moved by a program while
  it rests is noticed at the next real movement.
- **Library window.** It does not show why a wallpaper could not be loaded
  (`walpe-ctl status` does), and it still waits for the daemon in a few places ("Set as
  wallpaper", Workshop downloads).
- **Album art** is fetched from any address a music player names, including local ones.
- `application` wallpapers are Windows programs and are not supported.

## Architecture

```
walpe-daemon (src/daemon/)   no GTK. One override-redirect X11 window per monitor that is
                             the EGL surface, and one render thread per monitor that owns
                             its EGL context for life. Main thread: X events, pause policy,
                             the control socket, WebKit's main loop.
   +- engine (src/engine/)   content.c        type detection + factory
                             scenenode*.c     node-graph scenes: layers, effects (pass graph
                                              executor, shader translation and repair), text
                             scene.c          the legacy scene format
                             weparticles.c    particle systems; puppet.c puppet warp
                             scenescript.c    SceneScript on QuickJS
                             video*.c, mp4demux.c   H.264 player (module walpe-video.so)
                             web.c, webinput.c      WebKitGTK backend (module walpe-web.so)
                             gif.c, image.c, glutil.c, gpuprof.c (profiler)
   +- common (src/common/)   config, IPC, .tex parser, quality presets, pacing rules,
                             video import (ffmpeg), audio capture, media (MPRIS),
                             Workshop (steamcmd, Steam Web API), library, layer adjustments
walpe      (src/ui/)         GTK4 library window, options panel, settings, Workshop tab
walpe-ctl  (src/ctl/)        command line client of the control socket
walpe-tray (src/tray/)       tray icon (XEmbed + StatusNotifierItem), own process
scripts/                     walpe-gpu-report.sh (installed), lifecheck.py + checkenv.py (a test)
data/                        systemd user unit, autostart entries, style.css, icon
tests/                       unit and end-to-end tests, test fixtures
```

Things the code relies on:

- An EGL context never changes threads; GL state helpers are thread-local.
- Same-size monitors that show one scene share one rendering: the fastest monitor leads,
  the others present its frames.
- A wallpaper's files are untrusted input: every parser has size limits, file names stay
  below the wallpaper's folder, scripts and shaders have CPU, memory and size limits.
- Effects are Wallpaper Engine's own shader files, translated at load. Some stock shaders
  are recognised (by a hash of their statements, never by stored shader text) so that the
  engine can skip work whose result it can prove: a pass nobody reads, a blur computed at
  the size it is shown at.
- The daemon restarts itself in place (same environment, the control socket handed on)
  when the monitor layout or a setting that needs a new GL context changes.

## Tests

```sh
meson test -C build            # everything
meson test -C build config ctl # single suites
```

Tests that need a display start their own (`Xvfb`), tests that need OpenGL use EGL's
surfaceless platform or a software device and skip (exit 77) without one. `fx-spec` and
`fx-region` have an extra part on Wallpaper Engine's own effect shaders: it runs when an
install is found the way the engine finds one ("Wallpaper Engine assets" above) and is
skipped otherwise. `fx-rewrite-keys` checks its table of statement keys against an install
only when `WALPE_WE_ASSETS` is set (`build/fx-keys-test --strict` turns its notes about
another version of a shader into failures). Each of them says what it skipped; none writes
to the install.

## Developer tools

Harnesses and generators that are not needed to build, test or use WalPE live on the
separate branch **`devtools`** (directory `tools/dev/`), so a checkout of `main` holds
WalPE only:

| Tool | Does |
|---|---|
| `restorecheck.py`, `ctlcheck.py`, `guicheck.py`, `webcheck.py`, `layercheck.py` | end-to-end checks against a built tree, each with its own X server and daemon in a temporary directory |
| `optcheck.py` | option coverage: which wallpaper options reach the engine, over a wallpaper library |
| `gensyn.py`, `genlayers.py`, `gencontent.c` | generators of synthetic wallpapers |
| `genworkshopfix.py` | regenerates the synthetic Workshop replies in `tests/fixtures/workshop/` |
| `fxkey.py` | prints the hash key of a shader statement, for the recogniser tables in `src/engine/fx_keys.h` |
| `mock-mpris-player.py` | a stand-in media player on the session bus |

```sh
git fetch origin devtools:devtools && git worktree add ../walpe-devtools devtools
python3 -I ../walpe-devtools/tools/dev/restorecheck.py --build build
```

The tools take the build directory as `--build`; `../walpe-devtools/README.md` has the
invocation of each.

## Licence

WalPE is released under the BSD 3-Clause licence: see [LICENSE](LICENSE). Its relation to
Wallpaper Engine is stated at the top of this file.

- FFmpeg: `walpe-video.so` links libavcodec / libavutil. A binary linked against an FFmpeg
  that was built with GPL parts (`--enable-gpl`) is covered by the GPL as a whole.
- QuickJS: where `libquickjs` is linked statically (the usual case), the binaries contain
  it; its MIT notice is in [licenses/QuickJS-MIT.txt](licenses/QuickJS-MIT.txt) and has to
  accompany them.
