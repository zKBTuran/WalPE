# WalPE developer tools

This branch (`devtools`) holds harnesses and generators that are not needed to build, test
or use WalPE. They are kept off `main` on purpose: a checkout of `main` is WalPE only. The
branch has no source of WalPE itself; the tools work against a build of `main`.

```sh
# in a clone of WalPE (branch main), with a build in ./build:
git fetch origin devtools:devtools && git worktree add ../walpe-devtools devtools
python3 -I ../walpe-devtools/tools/dev/restorecheck.py --build build
```

Every end-to-end tool takes the build directory as `--build DIR` and finds `walpe-daemon`,
`walpe-ctl` and `walpe` there; none of them needs to be started from a particular directory.
Each starts its own display (`Xvfb -displayfd`; `restorecheck.py --display sway`: a headless
sway) and its own daemon with private XDG directories in a temporary directory, so nothing
of yours is read or written and a running WalPE is not disturbed. Each tells its daemon
which backend to use (`WALPE_BACKEND=x11` under its own X server), so the checks run the
same inside an X11 and inside a Wayland session. Exit status: 0 passed, 1 failures, 2
wrong command line, 77 skipped (a program the check needs, such as Xvfb, or a part of the
build is missing).

One tool needs files of `main`: `restorecheck.py` imports the harness
`scripts/checkenv.py` (private display, private daemon, pixel reader) and
`scripts/displayenv.py` (which display, and whether it can be used here), which `main`
keeps because its registered tests use them too. They are looked for in
`$WALPE_SRC/scripts`, else in `scripts/` next to the `--build` directory - so with the
usual `<source>/build` they are found by themselves, and with a build directory elsewhere
set `WALPE_SRC=<source tree>`. `resuicheck.py` loads `guicheck.py` from its own directory.
`buildmatrix.py`, `arm64cross.py` and `arm64-sysroot.sh` take the source tree as `--src DIR`.

## The tools (`tools/dev/`)

| Tool | Invocation | Does |
|---|---|---|
| `restorecheck.py` | `python3 -I restorecheck.py --build DIR [--daemon FILE] [--display xvfb\|sway] [--keep] [--tmp DIR]` | a wallpaper that fails to load brings the previous one back: two fake monitors (with `--display sway`, or `WALPE_CHECK_DISPLAY=sway`: two outputs of a headless Wayland compositor), generated one-colour scenes, status / config / log / pixels read back. `--tmp` must be a short path that does not exist yet (a socket lives below it). `RESTORECHECK_PATIENCE=N` multiplies every wait |
| `ctlcheck.py` | `python3 -I ctlcheck.py --build DIR [--keep] [--tmp DIR]` | every `walpe-ctl` command against a running daemon: also that `status` shows every setting `set` takes with the value that was set, that mistyped values are refused, and that a change which cannot be saved (the configuration directory replaced by a file) is answered with its reason and shown in `status` until a save succeeds |
| `guicheck.py` | `python3 -I guicheck.py --build DIR [--keep] [--tmp DIR] [--shots DIR]` | the library window and its settings window against a running daemon, with its own D-Bus session: also that a wallpaper the engine cannot load shows its error in the window's status line, that an emptied frame rate field keeps its value, and that an install the engine refuses after its dialog was closed is still shown (that case is left out where `steamcmd` is installed: the request would start a download) |
| `resuicheck.py` | `python3 -I resuicheck.py --build DIR [--keep] [--tmp DIR] [--shots DIR]` | the "Monitor resolution" dialog against a running engine, on a 1280x720 screen: first start, keyboard, an upgraded configuration, following `walpe-ctl`, a new monitor and its one notification, eight monitors. `RESCHECK_PATIENCE=N` multiplies every wait |
| `webcheck.py` | `python3 -I webcheck.py --build DIR [--keep] [--tmp DIR]` | wallpaper options reaching web pages, also a page that starts listening late (needs a build with the web module) |
| `layercheck.py` | `python3 -I layercheck.py --build DIR [--keep] [--display :N] [--tmp DIR]` | "Adjust layers" end to end on the scene `genlayers.py` writes |
| `optcheck.py` | `WALPE_SCRATCH=DIR python3 -I optcheck.py --build DIR --out OUT [--ids a,b] [--jobs N]`; `optcheck.py --report OUT` | option coverage over a wallpaper library: changes every user property of every scene through the daemon and compares the engine's state digest (`WALPE_PROP_PROBE`). `WALPE_SCRATCH` is a directory with `wallpapers/items/<id>/` and, optionally, `we/install/assets` (your Wallpaper Engine assets); default: the current directory |
| `gensyn.py` | `python3 -I gensyn.py KIND OUTDIR` | synthetic scene wallpapers (textures, effects, particles) that need no Wallpaper Engine assets |
| `genlayers.py` | `python3 -I genlayers.py widget DIR` / `blocks DIR` | synthetic scenes for "Adjust layers" |
| `gencheck.py` | `python3 -I gencheck.py quad\|pointer\|anim\|big DIR [--size WxH]`; `ruler DIR --size WxH`; `clip FILE.mp4 [--codec h264\|hevc\|vp9\|av1] [--pixfmt ...]`; `videoscene DIR`; `layouts` | synthetic wallpapers for checks of the display backends, the pointer, video decoding and the render resolution: four coloured quarters, a layer whose colour is the pointer's place, a scene that draws every frame, one-pixel marks, a picture larger than 1080p, test clips, a scene with a video layer; `layouts` prints the monitor layouts the checks use |
| `gencontent.c` | `cc gencontent.c -o gencontent $(pkg-config --cflags --libs gdk-pixbuf-2.0 glib-2.0) -lm`, then `./gencontent OUTDIR` | a small scene project with a generated `.tex` and a GIF |
| `genworkshopfix.py` | `python3 -I genworkshopfix.py <source tree>/tests/fixtures/workshop` | writes the synthetic Steam Workshop replies the test `workshop` reads (invented items; nothing is fetched) |
| `fxkey.py` | `python3 -I fxkey.py 'statement;' ...` or `--file shader.frag` | prints the 64-bit key of a shader statement as the engine computes it (white space removed, FNV-1a), for maintaining the recogniser tables in `src/engine/fx_keys.h` |
| `mock-mpris-player.py` | `python3 mock-mpris-player.py NAME CONTROL_FIFO [--title T ...]` | a stand-in MPRIS player on the session bus (needs python-dbus and PyGObject) |
| `buildmatrix.py` | `python3 -I buildmatrix.py --src DIR --work DIR [--cells a,b] [--full] [--list] [--jobs N] [--min-free-mb N] [--strict]` | configures, builds and tests WalPE in every combination that has to keep working, one build directory at a time: backends `x11` / `wayland` / `both`, kinds `release` / `asan` (AddressSanitizer + UndefinedBehaviorSanitizer) / `tsan` (ThreadSanitizer, `tests/tsan.supp`), tuning `generic` / `alderlake`, and `-min` with every optional part off. A failing test is shown by running its program once more; meson's own log files are never read or kept. `--list` prints the cells |
| `arm64-sysroot.sh` | `sh arm64-sysroot.sh list --src DIR`; `private --src DIR --sysroot DIR [--with-web] [--with-gl]`; `multiarch --src DIR [--allow-upgrades] [--dry-run]`; `undo-multiarch` | the arm64 libraries and headers a cross build needs, on an x86-64 Ubuntu 24.04: as a private sysroot (no root, nothing of the system changes) or as a foreign architecture of the system (root; `undo-multiarch` takes back only what it added) |
| `arm64cross.py` | `python3 -I arm64cross.py --src DIR --work DIR [--sysroot DIR] [--cross-file FILE] [--suite portable] [--with-scripts] [--build-only]` | cross-builds for aarch64 with the tree's `cross/aarch64-linux-gnu.ini` and runs the test programs under `qemu-aarch64` |

Run the Python tools with `python3 -I` (isolated mode): they import nothing from the current
directory. The end-to-end checks render with Mesa's software renderer (they set
`LIBGL_ALWAYS_SOFTWARE=1`), so they need no graphics card and do not use yours; on a busy
machine give them more patience (`RESTORECHECK_PATIENCE=3`, `GUICHECK_PATIENCE=3`,
`WEBCHECK_PATIENCE=3`).

## What the tools need

All of them: Python 3. The generators `gensyn.py`, `genlayers.py`, `genworkshopfix.py` and
`fxkey.py` use the standard library only. Beyond that:

| Tool | Programs | Python packages |
|---|---|---|
| `restorecheck.py` | `Xvfb`, Mesa with llvmpipe | none (`scripts/checkenv.py` of `main`, see above) |
| `ctlcheck.py` | `Xvfb`, Mesa with llvmpipe, `ffmpeg` | none |
| `restorecheck.py --display sway` | `sway`, `grim`, Mesa with llvmpipe; a build with the Wayland backend | none |
| `guicheck.py` | `Xvfb`, Mesa with llvmpipe, `xdotool`, ImageMagick (`import`), `dbus-daemon`, `gdbus` (GLib); libX11 (loaded through `ctypes`) for the case that closes a dialog | Pillow |
| `resuicheck.py` | as `guicheck.py`, and `dbus-monitor` | Pillow |
| `webcheck.py` | `Xvfb`, Mesa with llvmpipe, ImageMagick (`import`); a build with `walpe-web.so` | Pillow |
| `layercheck.py` | `Xvfb`, Mesa with llvmpipe, ImageMagick (`import`) | Pillow |
| `optcheck.py` | `Xvfb`, Mesa with llvmpipe, ImageMagick (`convert`) | none |
| `gencontent.c` | a C compiler, `pkg-config`, the development files of GLib and gdk-pixbuf | |
| `gencheck.py` | `ffmpeg` for `clip` and `videoscene` (exit status 77 when it cannot write the codec) | none |
| `buildmatrix.py` | `meson`, `ninja`, a C compiler with the sanitizer run-time libraries, and what WalPE's build and tests need (it lists which of `Xvfb`, `xdotool`, `sway`, `grim`, `ffmpeg` it found); 3 GB of free disk by default | none |
| `arm64-sysroot.sh` | `apt-get`, `dpkg` (Ubuntu 24.04); network access to the Ubuntu ports archive | |
| `arm64cross.py` | `aarch64-linux-gnu-gcc`, `meson`, `ninja`, `pkg-config`, `qemu-aarch64` (package `qemu-user`), the sysroot `arm64-sysroot.sh` prepares | none |
| `mock-mpris-player.py` | a D-Bus session bus | dbus-python, PyGObject |

A check that misses one of its programs or packages says so and exits with 77;
`optcheck.py` and `mock-mpris-player.py` do not look first and stop with Python's error.
`buildmatrix.py` and `arm64cross.py` exit with 2 for a missing program and with 3 when the
disk is too full.

The arm64 tools were used once, on an x86-64 machine with Ubuntu 24.04.
`arm64-sysroot.sh private` made its library directory twice (396 packages, 704 MB; with
`--with-web` 448 packages, 877 MB). `arm64cross.py --suite portable` configured and built
the tree with both backends and every optional part (354 steps, 0 errors, 0 warnings)
and ran the suite `portable` under qemu-user (2 of 2 pass). In that build the 68 tests
that need neither a display nor an OpenGL context were then started by hand, one by one,
under qemu-user: 65 pass, 2 fail in a
timing check that means nothing under emulation, 1 starts itself again and cannot run
there. `arm64cross.py` got two corrections after that run (`--keep-build` keeps the
console logs; a test that is handed a program of the build is left out, not reported as
failed) and has not been run from start to end since. Nothing was run on ARM hardware
(see the main README, "64-bit ARM").

**`webcheck.py` as root.** WebKit runs its web processes in a sandbox. When `webcheck.py` is
run as root it switches that sandbox off for the daemon it starts
(`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` in that daemon's environment only). The pages
that daemon loads are the ones the check writes itself; run the check as an ordinary user
to keep the sandbox on. No other tool changes it, and WalPE itself never does.

Licence: BSD 3-Clause, as WalPE (`LICENSE`).
