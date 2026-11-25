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

Dependencies that are shared across the system (GLib/GObject/GIO, GTK, PulseAudio,
text/rendering, and the window-system stack) and module-private dependencies (FFmpeg, LZ4)
are linked from **shared system libraries by default (`-Dstatic_deps=false`)**. Static
`libquickjs.a` is still preferred when present because upstream `libquickjs.so` encodes its
release date in `DT_SONAME`. To link module-private/stateless dependencies (FFmpeg, LZ4,
QuickJS) from static archives instead, install your distribution's static-library development
packages (`static-libs` USE flags on Gentoo; PIC required in `.so` modules) and configure with
`-Dstatic_deps=true`. PulseAudio (`libpulse` / `libpulse-simple`) always stays shared so
private versioned DSOs (`libpulsecommon-X.Y.so`) are never leaked into `DT_NEEDED`.

The GLib/GObject/GIO, GTK, text/rendering, PulseAudio, and window-system stack remains shared on purpose.
WalPE loads `walpe-video.so` and `walpe-web.so` into the daemon and passes GLib/GObject objects
across that module ABI. Embedding independent copies of those libraries in the daemon and each
module can split process-wide type registries and other global state. The C runtime, dynamic
loader, graphics drivers and components deliberately loaded at run time are also system/runtime
dependencies. When `-Dstatic_deps=true` is used, static archives pulled into `.so` modules, notably
LZ4 and FFmpeg in `walpe-video.so`, must be position-independent code (PIC); the configure step checks
those archive sets and reports an actionable error when they cannot be linked into a module.

On systems with static FFmpeg/LZ4/QuickJS archives, you can enable static private linking with
`-Dstatic_deps=true`.

On a native Linux build, `build.sh` also discovers versioned *private* shared-library directories
automatically after the first link. It examines every installed WalPE executable and module, follows
their ELF `DT_NEEDED` graph, and searches nested library directories with version-agnostic
`lib*.so*` globs. A candidate is accepted only when its ELF `DT_SONAME` exactly matches the
requested dependency. Only matching directories are added to the binaries' inherited ELF
`DT_RPATH` (not `DT_RUNPATH`, so indirect transitive dependencies are covered too). This handles distro layouts such as
PulseAudio's private `libpulsecommon-<release>.so` without hardcoding a PulseAudio release or
adding every plugin directory to the search path; a wildcard never substitutes an
ABI-incompatible version. After a library update or downgrade, run
`./build.sh` again: it reruns discovery and relinks the installed executables/modules against the
current archives and shared-library SONAMEs without recompiling unchanged source files.
The build then checks all installed executable/module outputs for unresolved shared dependencies. Cross builds
skip the host scan; target library paths must come from the cross toolchain/sysroot.

## Dependencies

A C compiler (GCC or clang), meson, ninja, pkg-config, Python 3, `readelf` from binutils,
`ldd` (native Linux builds), and GLib's `glib-compile-resources` (the window's stylesheet is compiled into the program;
Debian and Ubuntu have it in
`libglib2.0-dev-bin`, which `libglib2.0-dev` asks for, Arch in `glib2`). Package names are
Gentoo's; the pkg-config module is what `meson setup` looks for, to map them to another
distribution. The names of Debian and Ubuntu are listed [below](#64-bit-arm-aarch64), the
ones of Arch in [Installing on Arch-based systems](#installing-on-arch-based-systems);
[Debian 13](#debian-13) and [Alpine Linux](#alpine-linux) say what was run there.

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

Two libraries are not linked: the daemon loads them by name when they are installed, and
without one nothing but its use is missing, with no message. (Wallpaper sound is played
with the libpulse the `audio` option needs; `libpulse-simple.so.0`, which was loaded by
name for it, is no longer used at run time.) `libXss.so.1` tells
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

### Debian 13

> **Run in a chroot, not on a machine.** Debian 13.7 (x86-64) from `debootstrap
> --variant=minbase`, packages installed with `--no-install-recommends`: GCC 14.2, glibc
> 2.41, GTK 4.18.6, GLib 2.84.4, gdk-pixbuf 2.42.12, json-glib 1.10.6, WebKitGTK 2.52.6,
> FFmpeg 7.1.5, Mesa 25.0.7 (its software renderer), QuickJS 2025-04-26. No session of a
> desktop, no GPU and no sound card has seen WalPE on Debian.

The packages are the ones listed under [64-bit ARM](#64-bit-arm-aarch64), which on Debian
13 are enough as they stand: `meson setup` finds every module and every optional part
(X11 and Wayland backend, video, web with both page hosts and its sound sink, audio,
SceneScript, tray), and the build with the default options compiles and links.

```sh
apt install build-essential meson ninja-build pkg-config python3 \
  libglib2.0-dev libjson-glib-dev libgdk-pixbuf-2.0-dev libpango1.0-dev libfontconfig-dev \
  libepoxy-dev libegl-dev liblz4-dev libgtk-4-dev \
  libx11-dev libxext-dev libxinerama-dev libxi-dev libxrandr-dev libxrender-dev \
  libwayland-dev libavcodec-dev libavutil-dev libwebkitgtk-6.0-dev libsoup-3.0-dev \
  libgstreamer1.0-dev libpulse-dev libquickjs
```

At run time, besides what that section names (`ffmpeg`, a sound server, the GStreamer
plugins): **`librsvg2-common`**, for the window's icon. It is gdk-pixbuf's SVG loader,
which `libgtk-4-1` only recommends, so a system set up without recommended packages lacks
it, and GTK 4.18 draws an SVG icon with nothing else. Without it the window `walpe` works,
and what it hands a task bar on X11 as its icon is GTK's stand-in for a picture it cannot
show instead of `walpe.svg` (tried with the icon where `meson install` puts it, in
`/usr/share/icons/hicolor/scalable/apps` and under `/usr/local/share`, in a second chroot
with only the libraries the window needs).

(The test `ui-life` did end with a segmentation fault of the window in the chroot above
before `librsvg2-common` was installed there. That was the test's doing: it gave the icon
theme a directory with a hicolor `index.theme` of its own, which hid every icon GTK brings
along, and GTK 4.18.6 looks up its stand-in by calling the same function again when it is
not found, without end. The test no longer writes that file.)

The tests that need a display were run on Xvfb and on a headless sway (`xvfb`, `sway`,
`dbus-daemon`, `x11-xserver-utils` for `xrandr`, `xdotool`, `grim`, `procps`): the
daemon's life on X11 and on Wayland, the window, the tray and web wallpapers on the X11
page host pass. `parsers` failed in its checks of a text with two JSON values (json-glib
1.10), and the private page host did not come up (GTK 4.18): both were repaired after
that run (see Alpine below).
Debian 13's sway is 1.10.1, and of the 400 checks of `daemon-wayland` 7 fail there, all
after a fullscreen window was moved to the second output: that sway goes on naming the
first output to the daemon (no `output_enter` for the new one in
wlr-foreign-toplevel-management, while its own tree has the window there; sway 1.12
sends it, and with sway 1.9 the same checks pass). "Pause behind a fullscreen window"
then pauses the monitor the window came from.

### Alpine Linux

> **Run in a chroot, not on a machine.** Alpine 3.24.4 (x86-64, the minirootfs): musl
> 1.2.6, GCC 15.2, GTK 4.22.4, GLib 2.88.1, gdk-pixbuf 2.44.7 (decodes through glycin),
> json-glib 1.10.8, WebKitGTK 2.48.1, FFmpeg 8.1.3, Mesa 26.1.6 (its software renderer),
> QuickJS 2025-09-13, busybox 1.37 as `sh` and for the usual tools. It is the only system
> with another C library than glibc that WalPE was built on. No session of a desktop, no
> GPU and no sound card has seen it there.

```sh
apk add build-base meson pkgconf \
  glib-dev json-glib-dev gdk-pixbuf-dev pango-dev fontconfig-dev libepoxy-dev mesa-dev \
  lz4-dev gtk4.0-dev \
  libx11-dev libxext-dev libxinerama-dev libxi-dev libxrandr-dev libxrender-dev \
  wayland-dev ffmpeg-dev webkit2gtk-6.0-dev libsoup3-dev gstreamer-dev pulseaudio-dev \
  quickjs-dev
```

(`meson` brings `samurai` as ninja and python3; `glib-compile-resources` and
`wayland-scanner` come with `glib-dev` and `wayland-dev`.) With these `meson setup` finds
every module and every optional part, as on Debian. At run time: `mesa-dri-gallium` (the
OpenGL drivers, the software renderer among them: `mesa-dev` does not ask for it),
`ffmpeg`, a font (`font-dejavu`), and for the sound of web wallpapers `gst-plugins-base
gst-plugins-good`.

What is different with musl:

- glibc's allocator is asked to give freed memory back (`malloc_trim`) and is set up when
  the daemon starts (`mallopt`); musl has neither call, and nothing takes their place.
  `WALPE_MALLOC_MMAP_MB` is not read.
- musl gives a thread 128 KiB of stack where glibc gives 8 MiB. The build asks for 8 MiB
  (`-Wl,-z,stack-size=8388608`, added by `meson.build` when the C library is not glibc):
  without it a wallpaper script that calls itself without end ended the daemon instead
  of getting an error.
- `scripts/walpe-collect.sh` asks `ps` who started the engine and for its CPU figures
  (`ps -U`, `ps -p`). busybox's `ps` has neither option, so a report made there lacks
  those lines (read from the script and tried with busybox's `ps`; the script was not
  run through on Alpine). `apk add procps-ng` gives them.

Not musl, but the WebKitGTK of this Alpine release: 2.48 has no display throttle of its
own (`WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS`), and the frame rate limit of a page's
`requestAnimationFrame` is kept by WalPE's script there.

The tests that need a display were run on Xvfb and on a headless sway (`xvfb`, `sway`,
`dbus`, `xrandr`, `xdotool`, `grim`, `python3`): the daemon's life on X11 and on Wayland,
the window, the tray and web wallpapers on the X11 page host pass. What did not pass on
Alpine 3.24 came with its library versions: checks of pictures decoded by gdk-pixbuf 2.44
and of a text with two JSON values under json-glib 1.10 (`limits`, `parsers`, `thumbs`,
`wsnet`), and the private page host for web wallpapers on a session without X, which did
not come up with GTK 4.18 or newer (on Debian 13 as well: `websec-private`). Both were
repaired after these runs (CHANGELOG.md, "Arch Linux and CachyOS" and, under Wayland,
"Web wallpapers in a session without an X display"), and the five tests pass on Arch
Linux of November 2026, which has gdk-pixbuf 2.44, json-glib 1.10 and GTK 4.24; they
were not run again on Alpine 3.24 or Debian 13. GNOME Shell and KWin were not installed
on either system.

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
cross-compiled, tested under emulation and built and run in an emulated arm64 machine,
not on ARM hardware: see [64-bit ARM](#64-bit-arm-aarch64) below.

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

> **Not run on ARM hardware.** State of 13 November 2026 (0.14.0 with the fixes since):
> the arm64 build was cross-compiled on an x86-64 machine, its tests were run under
> user-mode emulation (qemu-user), and it was built and run in an emulated arm64 machine
> (QEMU, software rendering) on X11 and on Wayland. In short: it builds with 0 warnings,
> the daemon starts, shows a picture, a video and scenes and quits on both window
> systems, a picture and a scene come out byte for byte as on x86-64, and every test
> failure that was looked into comes from the emulation's speed or from the emulator
> itself (three failed checks of one Wayland test were not seen). What was run, and what
> was not, is in [State of testing](#state-of-testing-on-arm64) below. No ARM
> board, no ARM GPU or its driver, no kernel with 16 or 64 KiB pages and no ARM CPU's
> memory ordering has seen WalPE, and there is no speed figure. Treat all of this section
> as expected to work, and report.

**Building on the ARM machine** is the same as on x86-64: `./build.sh -Dtests=true`, then
`meson test -C build`. `--tuning=native` tunes for the CPU that builds (`-mcpu=native`; the binary
may not run on another ARM CPU), `--tuning=alderlake` is refused. Packages by their
Debian 13 / Ubuntu 24.04 names (the dependency table above has the pkg-config modules).
The list was used as it stands on Ubuntu 24.04 for arm64 (in the emulated machine,
installed with `--no-install-recommends`): `./build.sh -Dtests=true` configured with
both backends and every optional part and built the programs with 0 warnings. Nothing
was checked for Fedora, Arch Linux ARM or Raspberry Pi OS, whose GTK 4 and WebKitGTK can
be older:

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
  frame, halving a texture) are plain C on ARM: the same bytes. GCC 13 at `-O3` turns
  both into NEON code by itself (seen in the disassembly of the arm64 build); how fast
  that is on an ARM CPU was not measured, emulation says nothing about speed. The video
  decoder's thread rules were measured on x86 desktop CPUs.
- *Pictures.* What WalPE computes itself gives the same numbers on both architectures
  (the tests compare them, and the build forbids fused multiply-add). What the GPU or
  the software renderer rounds is its own: frames from Mesa's software renderer on
  arm64 differ from the x86-64 ones by up to 5 of 255.
- *Hardening.* `-mbranch-protection=standard` takes the place of `-fcf-protection`
  (return-address signing and branch target identification, on CPUs that have them). On
  Ubuntu 24.04, in the cross build and in the build on arm64 itself, the programs came
  out without the BTI mark, because the C library's start files carry none: return
  addresses are signed there (and were checked by the emulated CPU, which has pointer
  authentication), branch targets are not enforced.

**Cross-building on x86-64.** The tree has three meson cross files:
`cross/aarch64-linux-gnu.ini` (the `aarch64-linux-gnu-gcc` toolchain; arm64 libraries
installed as multiarch packages), and two second layers to give after it:
`cross/aarch64-sysroot.ini` for arm64 libraries in a directory of their own (a sysroot;
it also names the emulator for the tests) and `cross/aarch64-qemu-user.ini` (the
emulator for the multiarch case). The sysroot way is the one that was run:

```sh
cp cross/aarch64-sysroot.ini my-sysroot.ini        # set `sysroot = '...'` in the copy
meson setup build-arm64 -Dtests=true --cross-file cross/aarch64-linux-gnu.ini --cross-file my-sysroot.ini
ninja -C build-arm64 programs                      # the installed programs
meson test -C build-arm64 --suite portable -t 10   # two tests, under qemu-aarch64 (builds what is missing first)
```

With multiarch packages it is `--cross-file cross/aarch64-linux-gnu.ini --cross-file
cross/aarch64-qemu-user.ini` (**not run**: it needs arm64 as a foreign architecture of
the build machine). Ubuntu 24.04 packages for the build machine: `gcc-aarch64-linux-gnu
libc6-dev-arm64-cross qemu-user meson ninja-build pkg-config`. QuickJS is looked for in
the sysroot (`<sysroot>/usr/lib/quickjs`), then in the target's multiarch directory; a
`libquickjs.a` that does not link for the target - the build machine's own, in
`/usr/lib/quickjs` - is not taken, and the configure step says so
(`-Dquickjs_libdir=<dir>` names another one). `--tuning=native` and
`-Dnative_tuning=alderlake` are refused in an arm64 cross build with a sentence that
says why, `auto` adds nothing there; name the target CPU with `-Dc_args=-mcpu=...` if
you know it.

Running the tests under emulation:

- The emulated CPU matters. The build signs return addresses, and QEMU 8.2 with its
  default CPU computes the signature in software on every call and return: the tests
  that limit CPU time (`formats`, `script-hostile`) then fail, `formats` took 4 min 51 s
  instead of 33 s. Both wrapper files start the emulator with `-cpu
  max,pauth-impdef=on` for that reason; for programs the kernel starts through
  `binfmt_misc` it is `QEMU_CPU=max,pauth-impdef=on` in the environment.
- Tests that start another program of the build, or themselves again (`sec-local`,
  `limits`, every test that is a shell or Python script), need the kernel to start arm64
  programs: `binfmt_misc` with qemu registered.
- What fails there for the emulation's sake, with qemu-user 8.2.2: `memreport` (the
  emulator makes up `/proc/self/smaps` without resident sizes), `limits` (the emulator
  itself ends with "QEMU internal SIGSEGV" when one thread reads `/proc/self/maps` while
  another maps memory), and tests that hold wall-clock or CPU time against a limit
  (`fx-hostile`, `video`, `video-nv12`, `scene-feat`). The first two pass on an arm64
  kernel (the emulated machine); the others need a CPU that is not emulated.

The developer tools have `arm64-sysroot.sh` (the arm64 libraries in a directory of their
own, without touching the system's packages) and `arm64cross.py` (configure, build and
run the tests under emulation). The suite `portable` (`meson test --suite
portable`) compares this machine's results with numbers fixed in the source, needs no
display and no OpenGL, and is the first thing to run on a real ARM machine: a failure
there is a portability bug.

### State of testing on arm64

All of it on 12 and 13 November 2026, on one x86-64 machine with Ubuntu 24.04; the tree
is 0.14.0 with the fixes since. Nothing here is a run on ARM hardware.

*Cross build* (gcc-aarch64-linux-gnu 13.3.0, the arm64 packages of Ubuntu 24.04 in a
sysroot): X11 and Wayland backends, video (FFmpeg 6.1), web (WebKitGTK 2.52), audio,
SceneScript (QuickJS 2021-03-27), tray, LTO, hardening: 457 build steps with every test
program, 0 warnings. Without LTO and with `native_tuning=auto`: 0 warnings, no `-mcpu`
on any command line.

*Tests under qemu-user 8.2.2* (`-cpu max,pauth-impdef=on`, arm64 Mesa 25.2.8 with the
software renderer, also emulated). The same tree passes 154 of 154 tests on x86-64.

| Tests | Run | Pass | Not passed |
|---|---|---|---|
| without a display or OpenGL | 102 | 102 | |
| with OpenGL (surfaceless, software renderer) | 33 | 27 | 6: `memreport`, `limits` (the emulator, see above); `fx-hostile`, `video`, `video-nv12`, `scene-feat` (time) |
| shell / Python tests that start the daemon | 4 of 19 | 4 (`linkage`, `sec-daemon`, `ctl`, `daemon-life`: 53 minutes) | the other 15 were not run under qemu-user (hours; the run was cut short) |
| the parsers and file readers built with UndefinedBehaviorSanitizer (alignment, shifts, conversions) | 53 | 52, no report | `video`, at the same wall-clock check |
| without a display or OpenGL, with 64 KiB pages (`qemu-aarch64 -p 65536`) | 102 | 100 | 2, both for what `-p` does: `scenescript` (the emulator answers "bad address" when `sigsetjmp` saves the signal mask, the time limit's signal then stays blocked), `videosec` (it multiplies the resident pages the host counts, of 4 KiB, by the emulated page size). Neither says anything about a kernel with 64 KiB pages |

*An emulated arm64 machine* (QEMU 8.2.2 `qemu-system-aarch64 -machine virt -cpu max`: a
CPU with pointer authentication, BTI and SVE; 2 CPUs, 3.5 GB; Ubuntu 24.04.5 for arm64,
kernel 6.8 with 4 KiB pages; Mesa 25.2.8, software renderer; no GPU):

- Built there from a source archive with the package list above: `./build.sh
  -Dtests=true`, the programs with LTO, 0 warnings (33 minutes). The test programs were
  not built there: each is linked with LTO, at 1.4 build steps a minute that is close to
  four hours. The tests that ran in the machine are the cross build's programs (the same
  compiler version and options), copied in.
- The daemon built there, on X11 (Xvfb) and on Wayland (headless sway, layer shell),
  with desktop OpenGL 4.5 and - on X11 - with OpenGL ES 3.2 (`WALPE_GL_API=gles`): it
  starts, answers `walpe-ctl status` and `diag`, shows a PNG picture, an H.264 video,
  two generated scenes (one that changes every frame), `walpe-tray` starts next to it,
  `walpe-ctl quit` ends it with status 0; no critical line in its log. The screen with
  the picture and the screen with the still scene are byte for byte the screens of the
  x86-64 build (640x360, read with `import` on X11 and `grim` on Wayland), on both
  window systems and with OpenGL ES. Video and the animated scene show moving pictures;
  their frames were not compared, they are taken at different moments.
- The test suite in that machine (the cross build's programs, `meson test -t 10`, while
  the host was busy with other work: load 15 to 40 on 4 cores): 142 of the 154 tests were
  started, 132 pass. Not passed: `fx-hostile`, `video`, `video-nv12`,
  `script-hostile`, `formats`, `sprite-sheet` (a limit on CPU or wall-clock time;
  `sprite-sheet` passes there when run alone, `formats` did once), `gpustat` (not finished
  in 30 minutes: it starts a great many small programs, what the emulated machine is
  slowest at; 81 s under qemu-user), `daemon-life` (two hours; 407 of its 412 checks
  pass - the tray and its menu, the library window, web wallpapers and the restarts among
  them - and the 5 others are one case, `swaplost`, that counts frames and restarts in a
  time window; the whole test passes under qemu-user), `daemon-life-wayland` (the same
  on headless sway, two hours: 210 of 220 checks; 7 of the others are `swaplost` again
  and one look for WebKit's processes, 3 are further up in an output of which only the
  end was kept) and `daemon-resolution` (its X server did not start in the time it
  gives it; run by hand with `RESCHECK_PATIENCE=20`: 111 of 112 checks, the other is a
  picture that was right a moment after the wait ended).
  `memreport`, `limits` and `sec-local` pass there. Two more were started by hand with
  the suite's command lines, on headless sway: `daemon-wayland` (the Wayland backend
  end to end: outputs that come and go, scale, pointer, pause rules; 230 checks, 0
  failed) and `tray-life-wayland` (the tray's menu up to "Quit WalPE"; 35 checks, 0
  failed). **Not run to an end** (10, all shell / Python tests; the machine needs hours
  for them): `tray-life` (its cases are part of `daemon-life`), `daemon-wayland-silent`,
  `daemon-wayland-wait-socket`, `sec-daemon-wayland`, `daemon-scene-video`, `gpu-report`,
  `websec`, `websec-private`, `web-wayland`, `web-x11`.

*Not run at all*: ARM hardware of any kind; an ARM GPU or its driver (every picture
above is Mesa's software renderer); hardware video decoding; a kernel with 16 or 64 KiB
pages; a real ARM CPU's memory ordering (the emulator runs threads in the order an x86
CPU would); sound output and capture with a sound server; Fedora, Arch Linux ARM,
Raspberry Pi OS; clang; a native build of the test programs; the multiarch cross files;
speed.

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
stay.

**Earlier copies first.** pacman knows nothing of a WalPE that was installed or run by
hand, and such a copy can be the one that starts:

- An earlier `meson install` with the default prefix put the programs into
  `/usr/local/bin`, which comes before `/usr/bin` in Arch's `PATH`: `walpe`, `walpe-ctl` and
  the autostart entries (`Exec=walpe-daemon`) would go on starting the old ones. Remove it
  with `sudo ninja -C <its build directory> uninstall`; the same, without `sudo`, for one
  in `~/.local` where `~/.local/bin` is in your `PATH`.
- The tray switch in the window's settings writes `~/.config/autostart/walpe-tray.desktop`
  with the full path of the `walpe-tray` beside the window it was pressed in, and that
  file wins over the package's entry of the same name. If you used the switch in a WalPE
  that ran from a build directory, the tray of that directory starts at login, and the
  engine and the window it starts are that directory's too: delete the file, or switch the
  tray off and on once in the installed window.
- A `walpe.service` copied to `~/.config/systemd/user/` names `~/.local/bin/walpe-daemon`
  and hides the package's unit of the same name: delete it.

`walpe-ctl diag` names in its first line the version of the engine that runs and what
started it, in its second the commit of the source it was built from;
`readlink /proc/$(pidof walpe-daemon)/exe` names its file.

That commit (`walpe-daemon --version`, `walpe-ctl status`: `source_id`) - its first seven
digits everywhere, whatever length git abbreviates to on a machine or a forge - is found
at build time by `scripts/source-id.sh`: a tree made by `git archive` - a release tarball,
the forge's download of a tag, a test build's zip - has the whole commit in the file
`SOURCE-ID` (`.gitattributes`: `export-subst`), and git is not asked; a checkout asks git, and marks
a tree with changed files by a `+`; a copy of the files without `.git` and with the
placeholder still in `SOURCE-ID` (an archive made by another tool) says `unknown`. The
step runs with every build and changes its header only when the id changed, so a build
of an unchanged tree compiles nothing and gives the same bytes; a new commit compiles
`src/common/sourceid.c` again and links the three programs that show the id.

The package is the build of this file with the prefix `/usr`, and differs from
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
  one of the two: a second `walpe-daemon` ends with status 1, which the unit takes for a
  failure and answers with a new start every three seconds (`Restart=on-failure`).
- **The add-on for KDE Plasma is installed for all users**, as the wallpaper type "WalPE"
  in `/usr/share/plasma/wallpapers`. Nothing uses it until you choose it: right-click the
  desktop, "Desktop and Wallpaper", wallpaper type **WalPE**
  ([WAYLAND.md](WAYLAND.md#kde-plasma)). It is always the version of the engine; after an
  update of the package Plasma's shell keeps the version it has loaded until the next
  login. `contrib/plasma/install.sh` of the source tree is not needed; it works with this
  copy too (`status` names it, `select` and `unselect` use it). A copy it has put into
  `~/.local/share/plasma/wallpapers` earlier is found first by Plasma and stays the old
  version, and `install.sh status` shows both versions: remove yours with
  `contrib/plasma/install.sh uninstall`. The desktops keep the wallpaper type and show
  the package's copy from the next login.
- **The documents are in `/usr/share/doc/walpe`**, the licence texts in
  `/usr/share/licenses/walpe`.
- **The compiler flags are those of `/etc/makepkg.conf`**, after the project's own: Arch's
  file says `-O2` where `./build.sh` builds with `-O3`.

QuickJS, which runs the scripts of scene wallpapers and is linked statically, is in none
of Arch's repositories: the AUR has `quickjs`, which has to be installed before `makepkg`
runs (it is needed for the build only). `quickjs-ng` from `extra` is another library, with
other file names, and is not what the build looks for. `_scenescript=0` builds without: scripted
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
GTK 4.24, gdk-pixbuf 2.44 and GLib 2.90 the build has no warning (before the changes for
these releases the compiler printed 31 lines about functions they have deprecated).

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
