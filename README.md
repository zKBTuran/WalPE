# WalPE

Animated desktop wallpapers for Linux, on X11 and on Wayland compositors with the layer
shell: **Wallpaper Engine scene, video and web projects, GIFs and still images**, rendered
by a small GTK-free daemon (one EGL render thread per monitor), with a GTK4 library window,
a tray icon and a command line client. Written in C. This is the development tree of
version 0.14.0, which is not released; the last release is 0.12.2.

> **What is new since 0.12.2, and how far it was tested.** A Wayland backend next to the
> X11 one, with web wallpapers that need no X there; choice of the GPU on machines with
> two, and support written for AMD and Intel
> graphics; an OpenGL ES 3.1+ path for GPUs without desktop OpenGL; a render resolution
> per monitor; hardware video decoding chosen by what the machine offers; scene sound
> (off by default); camera parallax, camera shake, bloom, sprite-sheet layers and script
> camera in scenes; builds for 64-bit ARM. **All of it was built and tested only on Mesa's
> software renderer, a headless compositor, a null sound sink and - the ARM build - under
> emulation. Nothing new has been run on real AMD, Intel, NVIDIA or ARM hardware or on a
> real Wayland desktop.** The NVIDIA / X11 path is meant to be unchanged from 0.12.2,
> except that scenes which ask for bloom, camera shake, sprite sheets or a script camera
> now get them, that two fixes change the picture of some scenes (layers with a tint and
> effects, rope trails of particle systems) and that a fullscreen window pauses the
> wallpaper of its own monitor only ([Pausing](#pausing)); that too was checked on the
> software renderer only ([Known limitations](#known-limitations)).

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
switch), [WAYLAND.md](docs/WAYLAND.md) (the Wayland backend, what differs from X11),
[GPU.md](docs/GPU.md) (drivers, choosing the GPU, OpenGL ES),
[RESOLUTION.md](docs/RESOLUTION.md) (render resolution per monitor),
[SCENE-FEATURES.md](docs/SCENE-FEATURES.md) (parallax, bloom, camera shake, sprite sheets,
fluid simulation and their switches), [SOUND.md](docs/SOUND.md), [VIDEO.md](docs/VIDEO.md),
[GPU-PRESENT.md](docs/GPU-PRESENT.md) (`gpu.conf`), [GPU-PROFILING.md](docs/GPU-PROFILING.md),
[SECURITY.md](docs/SECURITY.md).

## Features

| Content | Status | Notes |
|---|---|---|
| **Scene** (node graph, `objects[]`: the common format) | yes | `scene.pkg` extraction, `.tex` textures (LZ4 mips; DXT1/3/5 and R8/RG88 kept GPU-native; embedded PNG/JPEG; stored mip chains), parent transforms, camera, text layers |
| Scene, legacy format (`object` / `layers`) | yes | |
| Effects | yes | Wallpaper Engine's GLSL dialect compiled for GL 3.3 (`#include`, combos, HLSL helpers, implicit conversions repaired from the shader source before the first compile); multi-pass chains with named targets, `copy` passes, masks, compose / fullscreen layers. Builtin effects need your Wallpaper Engine install's `assets/` ([below](#wallpaper-engine-assets)) |
| Particle systems | yes | emitters, initializers, operators, children, instance overrides, control points; sprites, sprite trails and rope ribbons; perspective systems; animated sprite sheets; refracting materials |
| Puppet warp layers (`.mdl`) | yes | skinned 2D mesh, bones, keyframed bone animations |
| Video textures (MP4 inside a `.tex`) | yes | one shared decoder per unique texture; a clip larger than it is shown is played from a converted copy of the size shown ([Video](#video)) |
| SceneScript | subset | QuickJS: scripts on layer `text` / `visible` / `alpha` / `origin` / `scale` / `angles` / `color`, on effect visibility and material constants, media events, property animations, cursor events |
| Media integration | yes | the "now playing" session of any MPRIS player: title, artist, album art with its colours, playback state, timeline; for scene scripts and web pages |
| Audio-reactive wallpapers | yes | captures the default output monitor (PulseAudio or PipeWire's pipewire-pulse) while such a wallpaper is visible |
| **Video** | yes | in-house MP4 demuxer + libavcodec H.264; other codecs and containers are converted once with ffmpeg ([Video](#video)) |
| **Web** (`type: web`) | yes | WebKitGTK 6 off screen, pages served from a token-protected loopback server; `wallpaperPropertyListener`, media and audio listeners; real mouse input on the empty desktop |
| GIF / still images | yes | images render once (no idle cost) |
| User properties | yes | options panel in the GUI and `walpe-ctl props` / `setprop` / `resetprops`; for video, GIF and image wallpapers the built-in `walpe_fit` (cover, contain, stretch, center), `walpe_pos_x` / `walpe_pos_y`, `walpe_flip` |
| Presets (`project.json` with `dependency` + `preset`) | yes | the base item is found next to the preset, in the Steam libraries or the WalPE library |
| Steam Workshop | yes | browse, search and download through steamcmd; WalPE never handles a Steam password ([Steam Workshop](#steam-workshop)) |
| Camera parallax, camera shake, bloom, sprite-sheet layers, script camera | yes | each with a switch; they change the picture of scenes that ask for them ([SCENE-FEATURES.md](docs/SCENE-FEATURES.md)). HDR bloom is not implemented; the look of the camera shake is an estimate |
| Sound | off by default | scene sound layers (WAV, MP3, FLAC, Ogg) through PulseAudio / PipeWire: `walpe-ctl set scene_sound=on volume=0.5` ([SOUND.md](docs/SOUND.md)). Video wallpapers are silent; web pages play their own media |
| 3D model scenes, `application` wallpapers | no | |

## Build

```sh
./build.sh                     # generic build: meson setup (or --reconfigure) build, then ninja
./build.sh --tuning=alderlake  # the same with -march=alderlake and friends (see below)
# or by hand:
meson setup build
ninja -C build programs        # the installed programs (all a plain `ninja -C build` builds, unless the tests are on)
```

`build.sh` (POSIX sh) runs ninja with as many jobs as the machine has CPUs (`-j <CPUs> -l
<CPUs + 2>`); `WALPE_JOBS` / `WALPE_LOAD` override `-j` / `-l`, `WALPE_BUILDDIR` the build
directory (default `build` in the source directory; a relative path counts from where the
script is called). `--tuning=<value>` is passed on as `-Dnative_tuning=<value>`,
every other argument goes to `meson setup` (`./build.sh -Dweb=disabled`). An existing build
directory is reconfigured, not wiped, and keeps the options it was set up with.
`build.sh` builds the installed programs only (the ninja target `programs`). **The tests
and the developer tools are not part of a build unless you ask for them**: a build
directory has no test program, no test and no tool until it is set up with `-Dtests=true`
(`./build.sh -Dtests=true`, or `meson configure build -Dtests=true` for a directory that
exists; [Tests](#tests)). Without it `ninja -C build`, `meson compile` and `meson
install` build what is installed and nothing else.

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
| `x11`, `xext`, `xinerama` | `x11-libs/libX11`, `libXext`, `libXinerama` | the X11 backend (`-Dx11`); `x11` also for the web module's X11 page host and for the XEmbed tray. A system without them builds the Wayland backend, web wallpapers included |
| `wayland-client`, `wayland-egl`, the program `wayland-scanner` | `dev-libs/wayland`, `dev-util/wayland-scanner` | the Wayland backend (`-Dwayland`) |
| `liblz4` | `app-arch/lz4` | `.tex` textures |
| `gtk4` | `gui-libs/gtk` | the library window `walpe` |
| `xi` (optional, found automatically) | `x11-libs/libXi` | X11: clicks on the empty desktop |
| `xrandr` (optional, found automatically) | `x11-libs/libXrandr` | X11: monitor names for per-monitor wallpapers |
| `wayland-cursor` (optional, found automatically) | `dev-libs/wayland` | Wayland: the pointer image over a wallpaper that takes pointer input |
| `wayland-client` and `wayland-server` 1.20 or newer, `gtk4-wayland` (optional, found automatically) | `dev-libs/wayland`, `gui-libs/gtk` with its Wayland backend | Wayland: pause rules from the compositor (idle time, window list); the private page host for web wallpapers, which needs no X |
| `gtk4-x11` (optional, found automatically) | `gui-libs/gtk` with its X11 backend | the X11 page host for web wallpapers |

At least one of the two backends has to be built. The Wayland protocol descriptions the
build needs are in the source tree (`protocols/`, see [Licence](#licence)), so no
`wayland-protocols` package is needed. The GPU, OpenGL ES, resolution and hardware decoding
parts need no new library: graphics devices are read from sysfs, and VA-API, DRM and Vulkan
devices go through FFmpeg's own interface.

Optional parts (meson `auto`; each takes `-D<option>=enabled|disabled`):

| Option | pkg-config module | Gentoo package | Without it |
|---|---|---|---|
| `video` | `libavcodec`, `libavutil` | `media-video/ffmpeg` | video wallpapers and video textures do not load |
| `web` | `webkitgtk-6.0`, `libsoup-3.0`, and what one of the two page hosts needs: `x11` + `gtk4-x11`, or the Wayland backend + `wayland-server` + `gtk4-wayland` | `net-libs/webkit-gtk:6`, `net-libs/libsoup:3.0` | web wallpapers report an error |
| `audio` | `libpulse-simple`, `libpulse` | `media-libs/libpulse` | audio spectra stay 0; wallpaper sound has no output |
| `scenescript` | none: `quickjs/quickjs.h` and `libquickjs` (searched in `<libdir>/quickjs`) | `dev-lang/quickjs` | scripted values stay static |
| `tray` | `gtk4`; `x11` and `xrender` for XEmbed | `x11-libs/libXrender` | no `walpe-tray`. Without libX11 / libXrender the tray is built with StatusNotifierItem only |

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
| `-Dx11=auto\|enabled\|disabled` | `auto` | the daemon's X11 backend: built when its libraries are found / configure fails when they are not / not built. `disabled` also leaves the X11 page host out of the web module: `walpe-daemon` and `walpe-web.so` then use no X library |
| `-Dwayland=auto\|enabled\|disabled` | `auto` | the daemon's Wayland backend (layer shell), likewise. `disabled` also leaves out the Wayland pause providers and the private page host |
| `-Dnative_tuning=generic` | default | no `-march`, `-mtune` or cache parameters, `-pipe` only. `off` is another name for it |
| `-Dnative_tuning=alderlake` | | `./build.sh --tuning=alderlake`: `-march=alderlake -mabm -mno-kl -mno-pconfig -mno-sgx -mno-widekl -mshstk --param=l1-cache-line-size=64 --param=l1-cache-size=32 --param=l2-cache-size=18432 -pipe`. **Such a build only runs on Alder Lake-class Intel CPUs** (12th generation Core and newer) and may die with "Illegal instruction" elsewhere |
| `-Dnative_tuning=native` | | `-march=native -pipe` (`-mcpu=native` on aarch64); an error if the compiler rejects it, and refused in a cross build |
| `-Dnative_tuning=auto` | | `-march=native -pipe` if the compiler accepts it, else `-pipe` only |
| `-Dhardening=true` | default | stack protector, stack clash and control-flow protection, `_FORTIFY_SOURCE`, format-security errors, full RELRO and immediate binding |
| `-Dvideo`, `-Dweb`, `-Daudio`, `-Dscenescript`, `-Dtray` | `auto` | the optional parts above. `-Dweb=enabled` fails at the configure step only when neither page host can be built |
| `-Dquickjs_libdir=/abs/dir` | | one more directory searched first for `libquickjs.a` (a QuickJS built by hand, or the target's in a cross build) |

```sh
./build.sh -Dwayland=disabled                  # the X11-only build, as the 0.12 releases are
./build.sh -Dx11=disabled                      # Wayland only, no X in walpe-daemon and walpe-web.so
                                               # (what a system without libX11 builds by itself)
```

A daemon with both backends picks one when it starts: Wayland where the compositor has the
layer shell, else X11; `walpe-ctl backend` shows and sets it ([WAYLAND.md](docs/WAYLAND.md)).
The web module holds the page hosts its build found the parts for (the configure summary
lists them): the off-screen X window, and a Wayland display of its own inside the daemon
that needs no X at all; which one a page gets is decided when the first page is made
([WAYLAND.md](docs/WAYLAND.md#web-wallpapers)).
Which GPU renders (`walpe-ctl gpu`) and whether desktop OpenGL or OpenGL ES is used
(`walpe-ctl set gl_api=`) are run-time settings too ([GPU.md](docs/GPU.md)); with the
defaults nothing is chosen differently from 0.12.2 on a machine with one GPU and desktop
OpenGL.

What the second backend costs: in the X11-only build the daemon and the web module link
no Wayland library and hold none of the Wayland code (the test `linkage` reads that from
the binaries). The other way round, a build without the X11 backend and the X11 page host
(`-Dx11=disabled`, or no libX11 on the system) names no X library and calls no X function
in `walpe-daemon` and `walpe-web.so` (the same test; GTK and WebKit load libX11 by
themselves where they were built with X support, and the tray has XEmbed where libX11 and
libXrender are found, whatever `-Dx11` says). A daemon with both backends needs
`libwayland-client`, `-cursor` and `-egl`, and the loader maps them in an X11 session too;
measured on the software renderer that was two libraries and about 20 kB of memory more
than the X11-only build (Mesa maps `libwayland-client` in either), and no thread,
descriptor, connection or wake-up of Wayland in an X11 session. The private page host is
started only when a page is put on it: with a web wallpaper on an X11 session the daemon
of a build with both backends has the same threads and descriptors as the X11-only build.

**Other architectures.** The source is written for x86-64 and aarch64 (little-endian, 64
bit; a big-endian target stops at the configure step). The aarch64 side has been
cross-compiled and run under emulation, not on ARM hardware: see
[64-bit ARM](#64-bit-arm-aarch64) below.

The code is plain C without hand-written SIMD for a particular `-march`; tuning only steers
the compiler's auto-vectoriser and scheduling. Each tuning flag is probed first, so a
compiler that does not know one drops it. With LTO the flags also go on the link line.

The web and video backends are separate modules, `walpe-web.so` and `walpe-video.so`, loaded
the first time a wallpaper needs them: from `$WALPE_MODULE_DIR`, the daemon's own directory
(build tree) or `<prefix>/<libdir>/walpe` (where `meson install` puts them). The daemon
itself links neither WebKit, GTK, GStreamer nor FFmpeg. A loaded module cannot be unloaded
again. For the web module, which is the large one, the daemon therefore restarts itself in
place at the wallpaper change that leaves no monitor with a web wallpaper (`web_unload`, on
by default; [SETTINGS.md](docs/SETTINGS.md)): WebKitGTK and GTK then leave its memory, and
every monitor loads its wallpaper again as at a start. The video module stays loaded.

### 64-bit ARM (aarch64)

> **Not run on ARM hardware.** The arm64 build was cross-compiled on an x86-64 machine
> (both backends and every optional part, 0 warnings) and its tests were run under
> emulation (qemu-user): of the 68 tests that need neither a display nor an OpenGL
> context, 65 pass, 2 fail in a
> timing check that means nothing under emulation (`formats`, `scenescript`) and 1 cannot
> run there (`sec-local` starts itself again). Seven of the tests that draw were run on
> the emulated software renderer and pass; the tests with a display and a daemon, the
> window, the tray and the web module were only compiled. No ARM board, no ARM GPU or its
> driver, no kernel with 16 or 64 KiB pages and no ARM CPU's memory ordering has seen
> WalPE, and there is no speed figure. Treat all of this section as expected to work, and
> report.

**Building on the ARM machine** is the same as on x86-64: `./build.sh -Dtests=true`, then
`meson test -C build`. `--tuning=native` tunes for the CPU that builds (`-mcpu=native`; the binary
may not run on another ARM CPU), `--tuning=alderlake` is refused. Packages by their
Debian 13 / Ubuntu 24.04 names (the dependency table above has the pkg-config modules).
**The list is untested on an ARM machine**: it follows the package names of an x86-64
system and the library set of the cross build. Nothing was checked for Fedora, Arch
Linux ARM or Raspberry Pi OS, whose GTK 4 and WebKitGTK can be older:

```
build-essential meson ninja-build pkg-config python3
libglib2.0-dev libjson-glib-dev libgdk-pixbuf-2.0-dev libpango1.0-dev libfontconfig-dev
libepoxy-dev libegl-dev liblz4-dev libgtk-4-dev
libx11-dev libxext-dev libxinerama-dev libxi-dev libxrandr-dev libxrender-dev   # X11
libwayland-dev                                                                  # Wayland
libavcodec-dev libavutil-dev            # video wallpapers
libwebkitgtk-6.0-dev libsoup-3.0-dev    # web wallpapers
libpulse-dev                            # audio spectra, scene sound
libquickjs                              # SceneScript
```

Ubuntu 24.04's `libquickjs` for arm64 is the 2021-03-27 release, the one `meson setup`
warns about ([SECURITY.md](docs/SECURITY.md)).

**What is different on ARM.**

- *Graphics.* Most ARM GPUs have OpenGL ES and no desktop OpenGL. With `gl_api=auto`
  (the default) the daemon asks for desktop OpenGL 3.3, then OpenGL ES 3.2, then 3.1,
  and `walpe-ctl status` says which it got. **OpenGL ES 3.1 is the minimum**: a GPU or
  driver with ES 3.0 or 2.0 only (Mali-400 with lima, for example) cannot run WalPE.
  What differs on the OpenGL ES path is in [GPU.md](docs/GPU.md). Under emulation one
  scene with effects gave the same frames with OpenGL 4.5 and OpenGL ES 3.2.
- *Compressed textures.* Many scene textures are DXT (S3TC), which most ARM GPUs do not
  read. WalPE then decodes them on the CPU: longer loading, four to eight times the
  video memory for those textures, and minified layers that can look visibly different
  (their smaller levels are then made from the decoded picture).
- *Video.* Decoding is on the CPU by default, as everywhere. `hwdec` knows the V4L2
  decoders of ARM boards (`v4l2m2m`, the last attempt of `hwdec=auto` on ARM; the V4L2
  request API as `drm`; Rockchip's `rkmpp` by name), all untested on any board, and
  which of them exist depends on the FFmpeg of the distribution: Ubuntu 24.04's has the
  `v4l2m2m` ones only ([VIDEO.md](docs/VIDEO.md)).
- *Speed.* The two pixel loops that have SSE2 / AVX2 versions on x86 (halving a video
  frame, halving a texture) run as plain C on ARM: the same bytes, slower. The video
  decoder's thread rules were measured on x86 desktop CPUs.
- *Pictures.* What WalPE computes itself gives the same numbers on both architectures
  (the tests compare them, and the build forbids fused multiply-add). What the GPU or
  the software renderer rounds is its own: frames from Mesa's software renderer on
  arm64 differ from the x86-64 ones by up to 5 of 255.
- *Hardening.* `-mbranch-protection=standard` takes the place of `-fcf-protection`
  (return-address signing and branch target identification, on CPUs that have them). In
  the Ubuntu 24.04 cross build the programs came out without the BTI mark, because the C
  library's start files carry none: return addresses are signed there, branch targets
  are not enforced.

**Cross-building on x86-64.** The tree has two meson cross files:
`cross/aarch64-linux-gnu.ini` (the `aarch64-linux-gnu-gcc` toolchain and arm64 libraries)
and `cross/aarch64-qemu-user.ini` (runs the test programs under `qemu-aarch64`; not
tried yet).

```sh
meson setup build-arm64 --cross-file cross/aarch64-linux-gnu.ini   # add -Dquickjs_libdir=... or -Dscenescript=disabled
ninja -C build-arm64 programs      # (-Dtests=true at setup for the test programs)
```

`--tuning=native` is refused in a cross build; name the target CPU with
`-Dc_args=-mcpu=...` if you know it. The developer tools have `arm64-sysroot.sh` (the
arm64 libraries in a directory of their own, without touching the system's packages) and
`arm64cross.py` (configure, build and run tests under emulation). The figures above were
made with them: `arm64cross.py --suite portable` made the build and ran the suite
`portable`; the 68 tests were then started by hand, one by one, under qemu-user in that
build. Tests that start another program of the build, or
themselves again, need `binfmt_misc` with qemu registered; timing checks are not valid
under emulation. The suite `portable` (`meson test --suite portable`) compares this
machine's results with numbers fixed in the source, needs no display and no OpenGL, and
is the first thing to run on a real ARM machine: a failure there is a portability bug.

## Install

```sh
./build.sh --prefix=$HOME/.local     # choose the prefix at setup time
meson install -C build               # binaries to <prefix>/bin, modules to <prefix>/<libdir>/walpe
```

`meson install` first runs a plain `ninja`. In a build directory without the tests that
builds nothing `build.sh` has not built. In one set up with `-Dtests=true` it builds
every test program as well: `meson install -C build --no-rebuild` installs without that.

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
desktops that use them. Wayland compositors such as sway do not run autostart entries:
start both from the compositor's configuration ([WAYLAND.md](docs/WAYLAND.md)).

## Run

```sh
walpe-daemon            # one desktop window (X11) or layer surface (Wayland) per monitor
walpe-daemon --backend x11|wayland   # this backend for this start, or an error
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
| `backend [auto\|x11\|wayland]` | show the window system the daemon draws on, how it was chosen, what the build holds and who reports each pause condition; or save the setting for its next start ([WAYLAND.md](docs/WAYLAND.md)) |
| `gpu [--json]` | the graphics devices of this machine, the one that renders, and what `set gpu=auto\|integrated\|discrete\|<device>` would pick ([GPU.md](docs/GPU.md)) |
| `gpu-stats [--sample SECONDS] [--json] ...` | GPU busy, memory, power and temperature as the kernel and the driver account them, for the whole GPU and for walpe-daemon; works without a running daemon ([GPU-PROFILING.md](docs/GPU-PROFILING.md)) |
| `mem [--json] [--deep] [--wait S] [--events on\|off]`, `mem --diff A B` | where the daemon's memory is, computed when asked and at no other time: resident memory by kind of mapping and by name, the allocator, descriptors, threads by name, counters since the start, and per monitor the GL objects of its context (textures by size, buffers, programs) and what its wallpaper holds. Save two reports with `--json` and `--diff` shows what grew. `--events on` starts counting how often textures, framebuffers, buffers and programs are made and deleted and pixels are read back (nothing counts them before; `off` stops it) |
| `resolution [<monitor> <value>] ...` | the render resolution of every monitor, or set one: `native`, `720p` ... `4k`, `WxH` ([RESOLUTION.md](docs/RESOLUTION.md)) |
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
| `fs=on\|off`, `covered=on\|off`, `dpms=on\|off` | on | pause the monitor a focused fullscreen window is on / while covered / while the display sleeps |
| `fs_all=on\|off` | off | a focused fullscreen window pauses every monitor, not only the one it is on |
| `focus=run\|limit\|pause`, `focus_fps=N` | run, 15 | what a wallpaper does while an application has the focus on its monitor |
| `battery=on\|off`, `battery_fps=N` | off, 0 | pause / limit on battery power |
| `shared=on\|off` | on | render a scene once for same-size monitors |
| `idle=N` | 0 | pause after N seconds without input (needs a session that reports idle time: Wayland compositors with `ext-idle-notify-v1`) |
| `software=on\|off` | off | render on the CPU (Mesa llvmpipe) instead of the GPU |
| `backend=auto\|x11\|wayland` | auto | the window system the daemon draws on; read at its next start |
| `gpu=auto\|integrated\|discrete\|<device>` | auto | which GPU renders; `auto` sets nothing |
| `gl_api=auto\|gl\|gles` | auto | desktop OpenGL 3.3 where the driver has it, else OpenGL ES 3.2 / 3.1 |
| `resolution=native\|720p\|1080p\|1440p\|4k\|WxH` | native | render resolution, for every monitor or `NAME:value,...` |
| `scene_sound=on\|off`, `volume=0..1`, `mute=on\|off` | off, 0, off | wallpaper sound |
| `bloom`, `camera_shake`, `camera_parallax`, `sprite_sheets`, `script_camera`, `script_texanim`, `fluid_sim` `=on\|off` | on | scene features; `parallax_offset` is off |
| `hwdec=off\|auto\|<api>`, `video_zerocopy=on\|off` | off | decode videos with a hardware decoder; keep its pictures in GPU memory |
| `video_scene_convert=on\|off` | on | a video inside a scene that is larger than it is shown is played from a converted copy of the size shown |
| `web_accel=on\|off` | off | web pages on the GPU |
| `web_unload=on\|off` | on | restart in place when the last web wallpaper is replaced, so WebKit leaves the daemon's memory |
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

Per monitor, a wallpaper pauses while: you paused it; a focused fullscreen window is on
that monitor (`pause_fullscreen`); the display is in power save or the screensaver runs
(`pause_dpms`); or visible normal windows cover 85 % of the monitor or more
(`pause_covered`; docks, desktop windows, menus, minimized windows and the windows of other
dwm tags never count). See-through windows (WM_CLASSes in `transparent_windows`, or
`_NET_WM_WINDOW_OPACITY` below 100 %) count as covering while `cover_transparent` is on.

A fullscreen window on one monitor leaves the wallpapers of the other monitors running
(up to 0.12 it paused all of them; `walpe-ctl set fs_all=on`, `pause_fullscreen_all`,
brings that back, for example to leave the whole GPU to a game). The monitor is the one
that holds most of the window, so a border or gap a tiling window manager keeps around a
fullscreen window does not matter; a window across several monitors pauses each one it
covers at least half of. The rule follows the focus: while another window has the focus,
the monitor under the fullscreen window is paused by `pause_covered`, not by this rule.
`walpe-ctl status` says it per monitor (`monitors`: `fullscreen`, `fullscreen_pause`,
`paused`).

Those paragraphs describe X11. On Wayland each rule needs something in the session that
reports its condition (the compositor's frame events, its window list, its idle
notification, logind); `walpe-ctl backend` and the status member `pause_rules` say for the
running session which rule has a source and which cannot trigger, and why
([WAYLAND.md](docs/WAYLAND.md#pause-rules)). New on both: `pause_idle` (`walpe-ctl set
idle=N`), pausing after N seconds without input where the session reports idle time.

A paused scene's clock stops (no jump on resume). A paused monitor does no decoding,
rendering or capturing; a video decoder sleeps once every monitor showing it is paused, a
web page is suspended once no monitor shows it, audio capture closes, and the daemon idles:
no periodic wake-up was measured while every monitor is paused or covered, and about one
every three seconds while a web page is loaded (measured on X11; not measured on
Wayland).

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
anything ffmpeg can read into that: H.264 High, yuv420p, scaled to fit within
`video_resolution` (by default the largest render resolution among your monitors; 1080p
for a configuration from an older version; never upscaled), source frame rate kept, audio
dropped; a file that already conforms is
remuxed. `walpe-ctl open` on another format converts it once into `~/.cache/walpe/video`.
ffmpeg runs as a child process with an argument vector (no shell).

A video inside a scene that is larger than the scene shows it - a 4K clip on a 1080p
monitor - is decoded at its own size, four times the work of what is seen. With
`video_scene_convert` (on by default) it is converted once, in the background at the lowest
priority, into a copy of the size shown (`video_resolution`) in the same cache, and the
scene plays the copy from where the clip next starts again; the original plays until then
and whenever the switch is off. `walpe-ctl status` says what each such video does
(`scene_videos`: source size, played size, `copy: converting 42 %` / `ready` / `off` / `not
needed`).

Hardware decoding (`walpe-ctl set hwdec=auto`, or one API by name: VA-API, NVDEC, VDPAU,
Vulkan Video, V4L2 through FFmpeg) is off by default and not confirmed on a graphics
card. With `auto` the decoder follows the GPU that renders (`cuda` first on NVIDIA's
driver, as before); software decoding takes over by itself when it fails, and `walpe-ctl
status` shows what the decoder really does (`video_decoder`). Details, the cost of each
step and the switches for it: [VIDEO.md](docs/VIDEO.md).

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
SNI host is running (KDE, waybar, GNOME AppIndicator extension); a Wayland session always
uses StatusNotifierItem. The tray also announces a newly connected monitor once, for its
render resolution ([RESOLUTION.md](docs/RESOLUTION.md)). The GUI's "Tray icon
(start with WalPE)" switch starts and stops it and remembers the choice in
`~/.config/walpe/tray.ini` (`start_with_gui`, `confirm_quit`, `backend=auto|sni|xembed`).
`walpe-tray --quit` closes it, `walpe-tray --menu` pops its menu at the pointer (bind it to
a key), `--action=next|random|pause-toggle` runs a menu entry.

## Known limitations

- **Run on one graphics card, and that was 0.12.1.** The maintainer runs WalPE on one
  NVIDIA laptop card (RTX 4050, proprietary driver, X11). A figure in the documents comes
  from that machine only where the text says so; all other sizes, counts and timings are
  counts or software-rendering numbers, not the performance of a particular card. Version
  0.12.1 was confirmed on that card as it stood before its last set of changes. The
  fixes of 0.12.2 were built and tested only on Mesa's software renderer (llvmpipe);
  nothing of 0.12.2 has been run on that card yet.
  **Everything that is new since 0.12.2 was built and tested only on Mesa's software
  renderer (llvmpipe), a headless Wayland compositor, a null sound sink and - the build
  for 64-bit ARM - under emulation. Nothing new has been run on real AMD, Intel, NVIDIA
  or ARM hardware or on a real Wayland desktop**; the NVIDIA / X11 path is meant to be
  unchanged from 0.12.2 with default settings, except for the scenes of the next item
  and for the fullscreen pause rule with more than one monitor ([Pausing](#pausing): the
  monitor of the fullscreen window pauses, the others keep running; `fs_all=on` is the
  old rule), and that too is so far shown only on the software renderer, in two steps. The development tree as it
  stood before the fixes of 0.12.2 were merged in was compared with release 0.12.1: the
  same frames of the 91 test scenes at 960x540 (16 of them with the switch of their new
  feature off), the same threads,
  descriptors and idle cost, and for four scenes the same OpenGL calls in the same order
  over 30 frames (calls that only ask the driver something left aside). The frames of the
  final tree were then compared with those of that earlier tree. Measure on your own
  card with `walpe-ctl profile`.
- **Default pictures that changed.** Scenes that ask for bloom, camera shake, a sprite
  sheet or a script camera now get them: 16 of the 91 scenes of the test library draw
  another picture than in the 0.12 releases (bloom in 10 of them, camera shake in 5,
  sprite sheets in 2, the script camera in 1). `walpe-ctl set bloom=off camera_shake=off
  sprite_sheets=off script_camera=off` gives the earlier pictures back
  ([SCENE-FEATURES.md](docs/SCENE-FEATURES.md)). Wallpaper Engine's HDR bloom is not
  implemented (such scenes get the standard bloom), the look of the camera shake is an
  estimate, and none of these was compared with Wallpaper Engine's own picture. Two
  fixes change pictures whatever the switches say: a layer with a tint and effects keeps
  its effects (two scenes of the test library), and a rope trail of a particle system is
  as long as its scene says at every frame rate (shorter than in 0.12 at 60 Hz and
  below); [CHANGELOG.md](CHANGELOG.md) has both. Effects
  on colour-only layers are as in 0.12 at a monitor's own resolution; a render size
  chosen above 2048 pixels makes them sharper ([RESOLUTION.md](docs/RESOLUTION.md)).
- **Wayland: tested headless only.** The Wayland backend needs a compositor with the
  layer shell (`zwlr_layer_shell_v1`); GNOME's Wayland session and weston's desktop shell
  have none and are not supported. Wallpapers were shown on a headless sway 1.9 with the
  software renderer and on nothing else: no real output, no GPU, no other compositor (a
  headless weston 13 served only as the compositor without layer shell that the daemon
  has to turn down). On Wayland
  the pointer is known only over the bare desktop, "Skip hidden areas" does not exist,
  the fullscreen and focus rules need the compositor's window list, and web wallpapers go
  through Xwayland where an X display answers and through the private page host (a
  Wayland display inside the daemon, no X needed) where none does. That page host ran
  real pages on the headless sway only; a page is expected to animate at 60 frames a
  second at most there ([WAYLAND.md](docs/WAYLAND.md#web-wallpapers)). A compositor that
  accepts the connection and does not answer within 10 seconds counts as absent (X11 is
  taken when there is an X display). Known and left as they are: a wallpaper that is not shown keeps being
  drawn when "pause when covered" is off; a lock screen pauses through "pause when
  covered" only, unless the locker tells logind; with no monitor left `walpe-ctl status`
  still lists the monitors; while the daemon waits for a compositor to come back,
  `walpe-ctl` says it is not running, and what it was sent is not carried out later; and
  a session started with a one-time socket (`WAYLAND_SOCKET`) cannot be restarted in
  place, so the daemon ends at the first change of a monitor there.
  [WAYLAND.md](docs/WAYLAND.md#limits).
- **AMD and Intel graphics, GPU choice, OpenGL ES: written, not run on hardware.** The
  list of graphics devices, render offload to another GPU (`gpu=integrated|discrete`),
  the kernel's GPU statistics and the Mesa counterparts of the driver hints were tested
  against made-up machines only. The OpenGL ES path (OpenGL ES 3.1 at least) rendered
  every scene of the test library on Mesa's llvmpipe, and all 1934 effect programs of
  those scenes compile there; no ES-only driver and no vendor shader compiler has seen
  it. Pictures differ slightly from desktop OpenGL: OpenGL ES has no texture LOD bias,
  so minified textures are a little softer (65 of the 91 scenes; in 52 of them at most
  1 % of the pixels differ by more than 2 of 255; each is exactly the desktop picture
  without the bias).
  [GPU.md](docs/GPU.md).
- **64-bit ARM: cross-compiled and tested under emulation, never run on ARM hardware**
  (65 of the 68 tests that need neither a display nor an OpenGL context pass there; no
  ARM GPU, no board):
  [64-bit ARM](#64-bit-arm-aarch64).
- **Sound is off by default and nobody has heard it.** Scene sound layers were measured
  against a PulseAudio null sink by recording it; no loudspeaker, no sound card, no
  `pipewire-pulse`. Quitting or restarting the daemon cuts a playing sound without a
  fade. Video wallpapers are silent, and a web page's own sound is not affected by the
  volume setting. Audio-reactive wallpapers react to wallpaper sound too.
  [SOUND.md](docs/SOUND.md).
- **Builtin effects need a Wallpaper Engine install** (see above); WalPE has no shaders of
  its own to replace them.
- **SceneScript is a subset.** Layers cannot be created or destroyed by script
  (`thisScene.createLayer()` returns a stand-in that is not drawn); scripts on properties
  the renderer does not drive have no effect. Of a script camera only the zoom and the
  eye's x and y are used; an animation's `addEndedCallback` is accepted and never called.
  Workshop item 3397287008 logs `'thisProject' is not defined` from its media handler: a
  gap in the script interface; the wallpaper runs.
- **Scenes are composed in 2D.** The x / y tilt of a layer is ignored; camera parallax
  moves layers sideways and does not change perspective; the scene settings for camera
  fade and HDR bloom are not applied (an option bound to one of them gets a note in the
  options panel); 3D model scenes do not load. Not read at all: `perspectiveoverridefov`,
  rope `uvscale` / `uvscrolling`, `copybackground`. The Fluid Simulation effect runs on
  the grid its file declares at a fixed 60 steps a second; whether it moves as in
  Wallpaper Engine has not been compared, its lit variant is not supported, and no scene
  of the test library is known to use it. Of sprite-sheet layers, frames stored on further
  images of one file and blending between frames are not covered; smoke made from a video
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
  through ffmpeg once, and imported or converted videos are limited to `video_resolution`
  (1080p for a configuration that comes from an older version). A video wallpaper on two
  monitors is uploaded once per monitor.
- **Off by default and not confirmed on a graphics card:** `hwdec` (hardware video decoding,
  every API), `video_zerocopy`, `web_accel`, `skip_hidden`, `pointer_share`, `video_limit`,
  `frame_align`, the `video_upload` alternatives, `gpu` other than `auto`, `gl_api=gles`,
  `preset_follow_gpu`, `parallax_offset` and a render resolution other than `native`.
  Hardware decoding and web acceleration fall back by themselves.
- **"Skip hidden areas" under a compositing manager** (`skip_hidden=always`) cannot know
  which windows the compositor itself makes see-through: behind those the wallpaper
  stands still; see SETTINGS.md.
- **Two monitors.** The leading monitor of a shared scene does not apply its own
  "application focused" limit to its own window; a scene that only follows the pointer
  still ticks (drawing nothing) while the pointer rests; a pointer moved by a program while
  it rests is noticed at the next real movement.
- **Library window.** It still waits for the daemon in a few places ("Set as wallpaper",
  Workshop downloads).
- **Album art** is fetched from any address a music player names, including local ones.
- `application` wallpapers are Windows programs and are not supported.

## Architecture

```
walpe-daemon (src/daemon/)   no GTK. Per monitor one EGL surface - an override-redirect X11
                             window or a Wayland layer surface - and one render thread that
                             owns its EGL context for life. Main thread: display events,
                             pause policy, the control socket, WebKit's main loop.
   +- platform (src/platform/)  the display backends behind one interface: x11.c,
                             wayland.c; output lists and the layout rule; pause signals
   +- signals (src/signals/) who reports a pause condition in a Wayland session: logind,
                             the compositor's idle notification and window list
   +- engine (src/engine/)   content.c        type detection + factory
                             scenenode*.c     node-graph scenes: layers, effects (pass graph
                                              executor, shader translation and repair), text
                             scene.c          the legacy scene format
                             weparticles.c    particle systems; puppet.c puppet warp
                             scenescript.c    SceneScript on QuickJS
                             video*.c, mp4demux.c   H.264 player (module walpe-video.so)
                             web.c, webinput.c      WebKitGTK backend (module walpe-web.so)
                             gif.c, image.c, glutil.c, gpuprof.c (profiler)
                             fx_essl.h (effect shaders as GLSL ES), bloom.c, fx_sim.h
                             (simulation effects), scenesound.c, sounddec.c (sound)
   +- common (src/common/)   config, IPC, .tex parser, quality presets, pacing rules,
                             video import (ffmpeg), audio capture, media (MPRIS),
                             Workshop (steamcmd, Steam Web API), library, layer adjustments,
                             backend and GPU choice (backend.h, gpusel.c, glapi.c),
                             GPU statistics (gpustat.c), render resolution (resolution.c)
walpe      (src/ui/)         GTK4 library window, options panel, settings, Workshop tab
walpe-ctl  (src/ctl/)        command line client of the control socket
walpe-tray (src/tray/)       tray icon (XEmbed + StatusNotifierItem), own process
scripts/                     walpe-gpu-report.sh (installed); the end-to-end tests' harness:
                             displayenv.py, checkenv.py, lifecheck.py, waylandcheck.py, rescheck.py
protocols/                   Wayland protocol descriptions (third-party, see Licence)
cross/                       meson cross files for aarch64
data/                        systemd user unit, autostart and desktop entries, style.css, icon
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
  when the monitor layout or a setting that needs a new GL context changes (`software`,
  `gpu`, `gl_api`, `web_accel`).
- The display backend is chosen once per start; nothing of the other backend runs then.

## Tests

```sh
./build.sh -Dtests=true        # or: meson setup build -Dtests=true; an existing directory:
                               #     meson configure build -Dtests=true
meson test -C build            # everything (builds the test programs first)
meson test -C build config ctl # single tests (builds only what they need)
```

The tests are off by default (`-Dtests=false`): such a build directory has no test and
no test program, and `meson test` says "No tests defined." there. `-Dtests=true` adds
the tests, their programs and the developer tools (`ninja -C build sceneshot`) to the
build directory; `./build.sh` still builds the installed programs only, and `meson
configure build -Dtests=false` takes them out again.

Tests that need a display start their own, tests that need OpenGL use EGL's surfaceless
platform or a software device and skip (exit 77) without one; a test that also has checks
without OpenGL runs those and says what it left out. The end-to-end tests start a
private display and a private `walpe-daemon` with its own XDG directories in a temporary
directory; nothing of your session is read, written or disturbed:

| Test | Display | Checks |
|---|---|---|
| `daemon-life`, `sec-daemon`, `daemon-resolution`, `daemon-scene-video`, `websec`, `websec-private` | `Xvfb` | loads, reloads, the restart in place; what the daemon trusts on the local machine; render resolution; a scene's video played from a converted copy; a hostile web wallpaper, on the X11 page host and on the private one |
| `web-x11` | `Xvfb` | real pages in WebKitGTK on the X11 page host (`scripts/webcheck.py`): the picture, animation, the Wallpaper Engine listeners, pointer input, pause and resume with the page's CPU time, restarts, a page process that dies, popups and choosers, network access off |
| `daemon-life-wayland`, `sec-daemon-wayland` | headless sway | the same cases on Wayland; the two of `daemon-life` that change the monitor layout are skipped there (the next row does that with real outputs) |
| `daemon-wayland-silent` | none | a compositor socket that accepts the connection and never answers: the daemon gives up after ten seconds (`scripts/wlsilentcheck.py`) |
| `daemon-wayland-wait-socket` | none | a daemon restarted in place that waits for its compositor and gives up: which control socket file it removes, and what its log says (`scripts/wlwaitcheck.py`) |
| `daemon-wayland` | headless sway | what only outputs can do (`scripts/waylandcheck.py`): two outputs, a mode / scale / transform that changes under the running daemon, outputs that appear and go, a pointer device, the choice between the two backends |
| `web-wayland` | headless sway without Xwayland, no `DISPLAY` | the same pages on the private page host (`scripts/webcheck.py`), and what only a Wayland session has: two outputs, outputs that come, change and go, `DISPLAY` left over without an X server, what the page's sandbox sees of the session, the session's clipboard (needs `wl-copy` / `wl-paste`) |

The X ones need `Xvfb` (and use `xdotool`, `xrandr` when present), the Wayland ones `sway`
and `grim` (`weston` adds a compositor without layer shell); each is skipped where its
programs are missing or the build has no backend for it. The headless compositor is sway
on wlroots' headless backend with the pixman renderer and a configuration of its own;
`scripts/displayenv.py` holds all of it and can be used by hand (`python3 -I
scripts/displayenv.py run --display sway -- build/walpe-daemon`). `WALPE_CHECK_STRICT=1`
turns "cannot run here" into a failure.

Sanitizer runs have test setups of their own:

```sh
meson setup build-asan -Dtests=true -Db_sanitize=address,undefined -Db_lto=false -Db_lundef=false
meson test -C build-asan                        # no setup named: leak detection on, suppressions of tests/lsan.supp
meson test -C build-asan --setup=asan           # stricter options, a report fails the test; leaks are not looked for
meson test -C build-asan --setup=asan_leaks     # the stricter options with leak detection and the same suppressions
meson setup build-tsan -Dtests=true -Db_sanitize=thread -Db_lto=false -Db_lundef=false
meson test -C build-tsan --setup=tsan --suite race   # then the rest
```

A ThreadSanitizer build links `tests/tsan/gsync_shim.c` into every program, because GLib's
locks are invisible to the sanitizer otherwise; the test `race-shim` says whether the
sanitizer can follow them on this machine at all, `race-patterns` (and its
expected-to-fail twin with the old shapes), `race-config` and `race-media` stress state
shared between threads. `tests/tsan.supp` is the one suppression file and is empty on
purpose; its header has the rule for adding a line. The setup runs Mesa's software
rasteriser in the calling thread. The suite `portable` (tests `portable`, `simd-equal`)
compares this machine's results with numbers fixed in the source and is the part worth
running on another architecture.

`fx-spec` and
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
| `restorecheck.py`, `ctlcheck.py`, `guicheck.py`, `resuicheck.py`, `webcheck.py`, `layercheck.py` | end-to-end checks against a built tree, each with its own X server and daemon in a temporary directory |
| `optcheck.py` | option coverage: which wallpaper options reach the engine, over a wallpaper library |
| `gensyn.py`, `genlayers.py`, `gencheck.py`, `gencontent.c` | generators of synthetic wallpapers |
| `buildmatrix.py` | configures, builds and tests every combination of backends, sanitizers and tuning that has to keep working |
| `arm64-sysroot.sh`, `arm64cross.py` | the arm64 libraries for a cross build, and the cross build with its tests under emulation |
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
- Wayland protocol descriptions: the ten XML files in `protocols/` are unchanged copies
  from the wayland-protocols and wlr-protocols projects (`protocols/README` names origin
  and checksum of each); they are not WalPE's own text. Each carries its own copyright
  and permission notice (MIT-style terms). The build generates C code from them that is
  compiled into `walpe-daemon` and `walpe-web.so`; the notices are collected in
  [licenses/Wayland-protocols.txt](licenses/Wayland-protocols.txt) and have to accompany
  the binaries. `meson install` puts all three licence files into
  `<prefix>/share/licenses/walpe`.
