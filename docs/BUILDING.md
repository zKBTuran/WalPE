# Building and installing

How to build WalPE, what it needs, the build options, the build for 64-bit ARM, how to
install it, and the notices that have to accompany its binaries. Using it is in
[USAGE.md](USAGE.md); the tests are in [DEVELOPMENT.md](DEVELOPMENT.md#tests).

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
exists; [Tests](DEVELOPMENT.md#tests)). Without it `ninja -C build`, `meson compile` and `meson
install` build what is installed and nothing else.

The default is a release build (`-O3`, `NDEBUG`, LTO, hardening flags) with **generic CPU
tuning**: the binaries run on any CPU of the architecture they were built for.

## Dependencies

A C compiler (GCC or clang), meson, ninja and pkg-config, and GLib's `glib-compile-resources`
(the window's stylesheet is compiled into the program; Debian and Ubuntu have it in
`libglib2.0-dev-bin`, which `libglib2.0-dev` asks for, Arch in `glib2`). Package names are
Gentoo's; the pkg-config module is what `meson setup` looks for, to map them to another
distribution. The names of Debian and Ubuntu are listed [below](#64-bit-arm-aarch64), the
ones of Arch in [Installing on Arch-based systems](#installing-on-arch-based-systems).

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
| `wayland-cursor` (optional, found automatically) | `dev-libs/wayland` | Wayland: the pointer image over a wallpaper that takes pointer input, on a compositor that does not draw it itself on request (`wp_cursor_shape_v1`; see [WAYLAND.md](WAYLAND.md)) |
| `wayland-client` and `wayland-server` 1.20 or newer, `gtk4-wayland` (optional, found automatically) | `dev-libs/wayland`, `gui-libs/gtk` with its Wayland backend | Wayland: pause rules from the compositor (idle time, window list); the private page host for web wallpapers, which needs no X |
| `gtk4-x11` (optional, found automatically) | `gui-libs/gtk` with its X11 backend | the X11 page host for web wallpapers |

At least one of the two backends has to be built. The Wayland protocol descriptions the
build needs are in the source tree (`protocols/`, see [Licence](#licence)), so no
`wayland-protocols` package is needed. The GPU, OpenGL ES, resolution and hardware decoding
parts need no new library: graphics devices are read from sysfs, and VA-API, DRM and Vulkan
devices go through FFmpeg's own interface.

Three libraries are not linked: the daemon loads them by name when they are installed, and
without one nothing but its use is missing, with no message. `libpulse-simple.so.0` plays
wallpaper sound; it is part of the libpulse the `audio` option needs. `libXss.so.1` tells
the X11 backend that the X screen saver is on, which pauses like a sleeping display
(Debian and Ubuntu: `libxss1`, Arch: `libxss`). `libXfixes.so.3` reports the start and the
end of a compositing manager to "pause when covered" on X11, which otherwise asks when a
window comes or goes (`libxfixes3`, `libxfixes`; GTK 4 depends on it on both).

Optional parts (meson `auto`; each takes `-D<option>=enabled|disabled`):

| Option | pkg-config module | Gentoo package | Without it |
|---|---|---|---|
| `video` | `libavcodec`, `libavutil` | `media-video/ffmpeg` | video wallpapers and video textures do not load |
| `web` | `webkitgtk-6.0`, `libsoup-3.0`, and what one of the two page hosts needs: `x11` + `gtk4-x11`, or the Wayland backend + `wayland-server` + `gtk4-wayland`. `gstreamer-1.0` (the headers of the GStreamer that WebKitGTK plays media with) for `libgstwalpesound.so` | `net-libs/webkit-gtk:6`, `net-libs/libsoup:3.0`, `media-libs/gstreamer` | web wallpapers report an error. Without `gstreamer-1.0` alone: web wallpapers are built, `meson setup` warns, and the level and mute of their sound are kept by the sound server ([SOUND.md](SOUND.md#web-wallpapers)); `-Dweb=enabled` asks for it |
| `audio` | `libpulse-simple`, `libpulse` | `media-libs/libpulse` | audio spectra stay 0; wallpaper sound has no output |
| `scenescript` | none: `quickjs/quickjs.h` and `libquickjs` (searched in `<libdir>/quickjs`) | `dev-lang/quickjs` (no longer in the Gentoo tree, see below) | scripted values stay static |
| `tray` | `gtk4`; `x11` and `xrender` for XEmbed | `x11-libs/libXrender` | no `walpe-tray`. Without libX11 / libXrender the tray is built with StatusNotifierItem only |

`libsoup-3.0` is also used, when present, to fetch album art over http(s).

QuickJS: wallpaper scripts are untrusted and run inside QuickJS, so its age matters. `meson
setup` warns when the installed library is older than the 2024-01-13 release (the 2021-03-27
release lacks fixes for memory errors that plain script can reach); see
[SECURITY.md](SECURITY.md).

The build was made against Gentoo's `dev-lang/quickjs`. That package has left the Gentoo
tree: `-Dquickjs_libdir=<dir>` points the build at a QuickJS built by hand (`libquickjs.a`
in that directory; `-Dc_args=-I<dir>` when its `quickjs/quickjs.h` is not on the
compiler's include path). Whether `dev-libs/quickjs-ng`, which the Gentoo tree has today,
can take its place is not known.

At run time: `ffmpeg` and `ffprobe` for importing and converting videos and for the sound
track of video wallpapers, GStreamer elements for media inside web pages (below),
`steamcmd` for Workshop downloads.

### Web wallpaper sound

WebKitGTK plays a page's sound and video with GStreamer, and takes the elements from
GStreamer's plugin packages. Distributions do not all install them with WebKitGTK:

| What a page needs | Element | Gentoo | Arch Linux, CachyOS | Debian, Ubuntu |
|---|---|---|---|---|
| a sound output at all | `autoaudiosink` | `media-libs/gst-plugins-good` | `gst-plugins-good` | `gstreamer1.0-plugins-good` |
| PulseAudio / PipeWire output (what `libgstwalpesound.so` plays through) | `pulsesink` | `media-plugins/gst-plugins-pulse` | `gst-plugins-good` | `gstreamer1.0-plugins-good` (older releases: `gstreamer1.0-pulseaudio`) |
| WAV, MP4, WebM / Matroska files | `wavparse`, `qtdemux`, `matroskademux` | `media-libs/gst-plugins-good` | `gst-plugins-good` | `gstreamer1.0-plugins-good` |
| Ogg Vorbis, Opus | `oggdemux`, `vorbisdec`, `opusdec` | `media-libs/gst-plugins-base` (`ogg`, `vorbis`), `media-plugins/gst-plugins-opus` | `gst-plugins-base` | `gstreamer1.0-plugins-base` |
| MP3 | `mpg123audiodec` or `avdec_mp3` | `media-plugins/gst-plugins-mpg123` | `gst-plugins-good` | `gstreamer1.0-plugins-good` |
| AAC, H.264 | `avdec_aac`, `avdec_h264` | `media-plugins/gst-plugins-libav` | `gst-libav` | `gstreamer1.0-libav` |
| VP8, VP9 | `vp8dec`, `vp9dec` (or `avdec_vp8`, `avdec_vp9`) | `media-plugins/gst-plugins-vpx` | `gst-plugins-good` or `gst-libav` | `gstreamer1.0-plugins-good` or `gstreamer1.0-libav` |

Where this comes from: the Arch and Ubuntu 24.04 columns are read from the packages (which
package owns the plugin file; on Arch `webkitgtk-6.0` lists `gst-plugins-good`,
`gst-plugins-bad` and `gst-libav` as *optional*, and depends only on `gst-plugins-base-libs`
and `gst-plugins-bad-libs`, which have neither `autoaudiosink` nor `pulsesink`); the row
"a sound output at all" and the two after it were run on Arch (WebKitGTK 2.54.2,
GStreamer 1.28.9); the Gentoo names are the ones this file had and were not checked
against a Gentoo tree; older Debian releases were not looked at.

**Without `autoaudiosink` a page with sound is not silent, it cannot be shown**: WebKitGTK
prints "GStreamer element autoaudiosink not found. Please install it" and its page
process ends as soon as the page loads media; WalPE loads the page again three times and
then gives up ("web: page process crashed ... 4 times within a minute"). That is WebKitGTK's
doing and the same in 0.14.0. On Arch Linux and CachyOS: `pacman -S gst-plugins-good
gst-plugins-base gst-libav`.

When a web wallpaper's folder has media files the daemon asks `gst-inspect-1.0` (if it is
installed) for the elements those files and their sound need, once, in the background,
logs each missing one with the package to install, and names them in `walpe-ctl status`
as `web_missing_gst`. A page that makes its sound without a file in its folder (Web Audio,
a stream) is not looked at.

## Build options

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
| `-Dtests=true` | `false` | the test programs, the tests of `meson test` and the tools built with them (`sceneshot`, `scenecpu` ...): [Tests](DEVELOPMENT.md#tests). `false`: the build directory has none of them |
| `-Dfuzz=enabled` | `disabled` | the libFuzzer targets of `tests/fuzz`: needs clang (`CC=clang`), to be combined with `-Db_sanitize=address,undefined -Db_lundef=false`. A developer tool, never part of a normal build |

```sh
./build.sh -Dwayland=disabled                  # the X11-only build, as the 0.12 releases are
./build.sh -Dx11=disabled                      # Wayland only, no X in walpe-daemon and walpe-web.so
                                               # (what a system without libX11 builds by itself)
```

A daemon with both backends picks one when it starts: Wayland where the compositor has the
layer shell, X11 in an X session, and neither on a Wayland compositor without the layer
shell (it ends and says so; on GNOME it takes Wayland when the WalPE extension for GNOME
Shell, `contrib/gnome`, which no build installs, is switched on); `walpe-ctl backend` shows and sets it ([WAYLAND.md](WAYLAND.md)).
The web module holds the page hosts its build found the parts for (the configure summary
lists them): the off-screen X window, and a Wayland display of its own inside the daemon
that needs no X at all; which one a page gets is decided when the first page is made
([WAYLAND.md](WAYLAND.md#web-wallpapers)).
Which GPU renders (`walpe-ctl gpu`) and whether desktop OpenGL or OpenGL ES is used
(`walpe-ctl set gl_api=`) are run-time settings too ([GPU.md](GPU.md)); with the
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
by default; [SETTINGS.md](SETTINGS.md)): WebKitGTK and GTK then leave its memory, and
every monitor loads its wallpaper again as at a start. The video module stays loaded.

## 64-bit ARM (aarch64)

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
libgstreamer1.0-dev                     # web wallpapers: their sound output (libgstwalpesound.so)
libpulse-dev                            # audio spectra, scene sound
libquickjs                              # SceneScript
```

Ubuntu 24.04's `libquickjs` for arm64 is the 2021-03-27 release, the one `meson setup`
warns about ([SECURITY.md](SECURITY.md)).

The list is what the build needs, on x86-64 as on arm64: on Ubuntu 24.04 (x86-64) every
pkg-config module and program `meson.build` asks for is in one of these packages
(`wayland-scanner` and `glib-compile-resources` come with `libwayland-dev` and
`libglib2.0-dev`). At run time on Debian and Ubuntu: `ffmpeg` (importing and converting
videos, the sound track of video wallpapers), a sound server with the PulseAudio protocol
(`pipewire-pulse` or `pulseaudio`), and `gstreamer1.0-tools`: it has `gst-inspect-1.0`,
without which the daemon cannot tell which GStreamer element a web wallpaper's media lack
(`web_missing_gst` stays empty). Ubuntu 24.04's `libwebkitgtk-6.0-4` depends on
`gstreamer1.0-plugins-base` and `gstreamer1.0-plugins-good`, so the sound output of web
wallpapers is installed with it; `gstreamer1.0-libav` (H.264, AAC) is only suggested.

**What is different on ARM.**

- *Graphics.* Most ARM GPUs have OpenGL ES and no desktop OpenGL. With `gl_api=auto`
  (the default) the daemon asks for desktop OpenGL 3.3, then OpenGL ES 3.2, then 3.1,
  and `walpe-ctl status` says which it got. **OpenGL ES 3.1 is the minimum**: a GPU or
  driver with ES 3.0 or 2.0 only (Mali-400 with lima, for example) cannot run WalPE.
  What differs on the OpenGL ES path is in [GPU.md](GPU.md). Under emulation one
  scene with effects gave the same frames with OpenGL 4.5 and OpenGL ES 3.2.
- *Compressed textures.* Many scene textures are DXT (S3TC), which most ARM GPUs do not
  read. WalPE then decodes them on the CPU: longer loading, four to eight times the
  video memory for those textures, and minified layers that can look visibly different
  (their smaller levels are then made from the decoded picture).
- *Video.* Decoding is on the CPU by default, as everywhere. `hwdec` knows the V4L2
  decoders of ARM boards (`v4l2m2m`, the last attempt of `hwdec=auto` on ARM; the V4L2
  request API as `drm`; Rockchip's `rkmpp` by name), all untested on any board, and
  which of them exist depends on the FFmpeg of the distribution: Ubuntu 24.04's has the
  `v4l2m2m` ones only ([VIDEO.md](VIDEO.md)).
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

The two XDG autostart entries go to `<sysconfdir>/xdg/autostart`, and desktops read
`/etc/xdg/autostart` and `~/.config/autostart`. With meson's defaults only the prefix
`/usr` gives `/etc`: the default prefix puts them into `/usr/local/etc/xdg/autostart` and
`--prefix=$HOME/.local` into `~/.local/etc/xdg/autostart`, where no session looks. For an
installation for all users add `--sysconfdir=/etc` at setup time; for one in your home
directory copy `data/walpe-autostart.desktop` and `data/walpe-tray-autostart.desktop` (as
`walpe-tray.desktop`) to `~/.config/autostart/` if you want them.

`meson install` first runs a plain `ninja`. In a build directory without the tests that
builds nothing `build.sh` has not built. In one set up with `-Dtests=true` it builds
every test program as well: `meson install -C build --no-rebuild` installs without that.

Or run from the build directory, or copy by hand: `walpe-daemon`, `walpe`, `walpe-ctl`,
`walpe-tray` and the two modules `walpe-video.so` / `walpe-web.so` into one directory (the
daemon loads the modules from its own directory, and the GUI starts `walpe-tray` from
there). Without the modules video and web wallpapers do not load; `walpe-ctl status` then
shows `"modules": "... not loaded"`.

On a Wayland session the window of a WalPE that is not installed has the desktop's generic
icon, in its title bar and in the task bar: there a window gets its icon from the desktop
entry named after its application id, `io.github.walpe.ui.desktop`, and only `meson
install` puts that file in place. Two files of your own do the same without installing
(from the source directory; start the window again afterwards):

```sh
install -Dm644 data/icons/walpe.svg ~/.local/share/icons/hicolor/scalable/apps/walpe.svg
mkdir -p ~/.local/share/applications
sed "s|^Exec=walpe|Exec=$PWD/build/walpe|" data/io.github.walpe.ui.desktop \
  > ~/.local/share/applications/io.github.walpe.ui.desktop
```

This also puts WalPE into the desktop's application menu. Seen on KDE Plasma 5.27 (KWin);
X11 needs neither file.

Starting the daemon with your session: [USAGE.md](USAGE.md#starting-with-the-session).

## Installing on Arch-based systems

For Arch Linux and the distributions built on it (CachyOS, EndeavourOS, Manjaro) the tree
has a package recipe, [contrib/packaging/arch/PKGBUILD](../contrib/packaging/arch/PKGBUILD).
It builds the tree it lies in, a git checkout or an unpacked archive, and makes one
package, `walpe`:

```sh
cd contrib/packaging/arch
makepkg -si                  # pacman installs what is missing; build; install
makepkg -si _scenescript=0   # without SceneScript, when there is no QuickJS (below)
```

`pacman -R walpe` removes it again; settings, the library and caches in your home directory
stay. The package is the build of this file with the prefix `/usr`, and differs from
`./build.sh` and `meson install` in this:

- **Nothing is left out without a word.** Every optional part is asked for by name, and
  the parts meson finds by themselves (XInput2, RandR, the pointer image, both page hosts
  of web wallpapers, `libgstwalpesound.so`) are checked after the configure step: a
  missing development file stops the build. On Arch the headers are in the same packages
  as the libraries (`gstreamer` has GStreamer's), so `makepkg -s` installs all of them.
- **It starts with the session.** The autostart entries are in `/etc/xdg/autostart`:
  after the next login `walpe-daemon` and `walpe-tray` run on every desktop that runs XDG
  autostart (KDE Plasma, GNOME, Xfce), for every user. To stop that for yourself, put a
  file of the same name with the line `Hidden=true` under `[Desktop Entry]` into
  `~/.config/autostart/` (the window's tray switch does it for `walpe-tray.desktop`).
  For sway, Hyprland and the like, which run no autostart entries, the package has the
  systemd user unit, not switched on: `systemctl --user enable --now walpe.service`. Use
  one of the two: a second `walpe-daemon` ends with status 1, and the unit starts it again
  every three seconds.
- **The add-on for KDE Plasma is installed for all users**, as the wallpaper type "WalPE"
  in `/usr/share/plasma/wallpapers`. Nothing uses it until you choose it: right-click the
  desktop, "Desktop and Wallpaper", wallpaper type **WalPE**
  ([WAYLAND.md](WAYLAND.md#kde-plasma)). It is always the version of the engine; after an
  update of the package Plasma's shell keeps the version it has loaded until the next
  login. `contrib/plasma/install.sh` is not needed and does not know this copy (`select`
  and `status` look into your home directory only). A copy it has put into
  `~/.local/share/plasma/wallpapers` earlier is found first by Plasma and stays the old
  version: remove it with `contrib/plasma/install.sh uninstall`, then choose the type
  again.
- **The documents are in `/usr/share/doc/walpe`**, the licence texts in
  `/usr/share/licenses/walpe`.
- **The compiler flags are those of `/etc/makepkg.conf`**, after the project's own: Arch's
  file says `-O2` where `./build.sh` builds with `-O3`.

QuickJS, which runs the scripts of scene wallpapers and is linked statically, is in none
of Arch's repositories: `quickjs` is in the AUR and has to be installed before `makepkg`
(an AUR helper does both). `quickjs-ng` from `extra` is another library, with other file
names, and is not what the build looks for. `_scenescript=0` builds without: scripted
values of scenes (clocks, switches, values that follow the sound) then keep their start
values.

| Package | Installed with `walpe` | What for |
|---|---|---|
| `gtk4`, `glib2`, `json-glib`, `gdk-pixbuf2`, `pango`, `cairo`, `fontconfig`, `libepoxy`, `libglvnd`, `lz4`, `graphene` | yes | everything |
| `libx11`, `libxext`, `libxinerama`, `libxi`, `libxrandr`, `libxrender` | yes | the X11 backend, the tray on X11 |
| `wayland` | yes | the Wayland backend |
| `ffmpeg` | yes | video wallpapers and video textures; `ffmpeg` and `ffprobe` import and convert videos and take out the sound track of a video wallpaper |
| `webkitgtk-6.0`, `libsoup3`, `gstreamer` | yes | web wallpapers and the sound output of their pages |
| `gst-plugins-good` | yes | web wallpapers: Arch's `webkitgtk-6.0` names it as optional, and without its `autoaudiosink` the page of a web wallpaper ends at its first sound; also WAV, MP3, MP4, WebM |
| `gst-plugins-base`, `gst-libav` | yes | web wallpapers: Ogg Vorbis and Opus; H.264 and AAC |
| `libpulse` | yes | wallpaper sound, audio-reactive wallpapers |
| `pipewire-pulse` or `pulseaudio` | no | the sound server for both; without one WalPE is silent and spectra stay 0 |
| `quickjs` (AUR) | at build time | SceneScript |
| `plasma-workspace` | no | KDE Plasma 6.5 or newer on Wayland: the wallpaper type "WalPE" |
| `gnome-shell-extension-appindicator` | no | GNOME: the tray icon |
| `libxss` | no | X11: pause while the X screen saver is on |
| `xdg-utils` | no | open the Workshop page of a wallpaper in the browser |
| `steamcmd` (AUR) | no | download Workshop wallpapers |
| `nvidia-utils` | no | NVIDIA: `nvidia-smi` for `walpe-ctl gpu-stats` and `walpe-gpu-report`; NVDEC |

How far this was tried: the package was built with `makepkg` by an unprivileged user in an
Arch Linux root that held `base`, `base-devel` and the packages the recipe names, nothing
else, and `namcap` has no error for the recipe or the package. Installed with `pacman`
into a root with nothing but its dependencies, the daemon showed a scene, a scene with a
script and a video on X11 (Xvfb) and on Wayland (headless sway), loaded its modules from
`/usr/lib/walpe`, and `tests/websound_rig.sh` passed with a private PipeWire, the sound
plugin found in `/usr/lib/walpe/gst`; in a Plasma 6.8 session with a virtual display the
wallpaper type from `/usr/share` was chosen and KWin kept Plasma's desktop over the
daemon's surface. Not tried: CachyOS itself, a graphics card (the software renderer drew),
a real login with its autostart, the systemd unit under a running user manager, and the
AUR's `quickjs` (the build used QuickJS 2026-06-04 as its own Makefile installs it). With
GTK 4.24, gdk-pixbuf 2.44 and GLib 2.90 the compiler warns about functions those releases
have deprecated (31 lines); the build has no other warning.

## Licence

WalPE is released under the BSD 3-Clause licence: see [LICENSE](../LICENSE). Its relation to
Wallpaper Engine is stated at the end of the [README](../README.md#disclaimer).

- FFmpeg: `walpe-video.so` links libavcodec / libavutil. A binary linked against an FFmpeg
  that was built with GPL parts (`--enable-gpl`) is covered by the GPL as a whole.
- QuickJS: where `libquickjs` is linked statically (the usual case), the binaries contain
  it; its MIT notice is in [licenses/QuickJS-MIT.txt](../licenses/QuickJS-MIT.txt) and has to
  accompany them.
- Wayland protocol descriptions: the eleven XML files in `protocols/` are unchanged copies
  from the wayland-protocols and wlr-protocols projects (`protocols/README` names origin
  and checksum of each); they are not WalPE's own text. Each carries its own copyright
  and permission notice (MIT-style terms). The build generates C code from them that is
  compiled into `walpe-daemon` and `walpe-web.so`; the notices are collected in
  [licenses/Wayland-protocols.txt](../licenses/Wayland-protocols.txt) and have to accompany
  the binaries. `meson install` puts all three licence files into
  `<prefix>/share/licenses/walpe`.
