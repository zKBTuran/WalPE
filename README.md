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
Each starts its own X server (`Xvfb -displayfd`) and its own daemon with private XDG
directories in a temporary directory, so nothing of yours is read or written and a running
WalPE is not disturbed. Exit status: 0 passed, 1 failures, 2 wrong command line, 77 skipped
(a program the check needs, such as Xvfb, or a part of the build is missing).

One tool needs a file of `main`: `restorecheck.py` imports the harness
`scripts/checkenv.py` (private X server, private daemon, pixel reader), which `main` keeps
because its registered test `daemon-life` uses it too. It is looked for in
`$WALPE_SRC/scripts`, else in `scripts/` next to the `--build` directory - so with the
usual `<source>/build` it is found by itself, and with a build directory elsewhere set
`WALPE_SRC=<source tree>`.

## The tools (`tools/dev/`)

| Tool | Invocation | Does |
|---|---|---|
| `restorecheck.py` | `python3 -I restorecheck.py --build DIR [--daemon FILE] [--keep] [--tmp DIR]` | a wallpaper that fails to load brings the previous one back: two fake monitors, generated one-colour scenes, status / config / log / pixels read back. `--tmp` must be a short path that does not exist yet (a socket lives below it). `RESTORECHECK_PATIENCE=N` multiplies every wait |
| `ctlcheck.py` | `python3 -I ctlcheck.py --build DIR [--keep] [--tmp DIR]` | every `walpe-ctl` command against a running daemon: also that `status` shows every setting `set` takes with the value that was set, that mistyped values are refused, and that a change which cannot be saved (the configuration directory replaced by a file) is answered with its reason and shown in `status` until a save succeeds |
| `guicheck.py` | `python3 -I guicheck.py --build DIR [--keep] [--tmp DIR] [--shots DIR]` | the library window and its settings window against a running daemon, with its own D-Bus session: also that a wallpaper the engine cannot load shows its error in the window's status line, that an emptied frame rate field keeps its value, and that an install the engine refuses after its dialog was closed is still shown (that case is left out where `steamcmd` is installed: the request would start a download) |
| `webcheck.py` | `python3 -I webcheck.py --build DIR [--keep] [--tmp DIR]` | wallpaper options reaching web pages, also a page that starts listening late (needs a build with the web module) |
| `layercheck.py` | `python3 -I layercheck.py --build DIR [--keep] [--display :N] [--tmp DIR]` | "Adjust layers" end to end on the scene `genlayers.py` writes |
| `optcheck.py` | `WALPE_SCRATCH=DIR python3 -I optcheck.py --build DIR --out OUT [--ids a,b] [--jobs N]`; `optcheck.py --report OUT` | option coverage over a wallpaper library: changes every user property of every scene through the daemon and compares the engine's state digest (`WALPE_PROP_PROBE`). `WALPE_SCRATCH` is a directory with `wallpapers/items/<id>/` and, optionally, `we/install/assets` (your Wallpaper Engine assets); default: the current directory |
| `gensyn.py` | `python3 -I gensyn.py KIND OUTDIR` | synthetic scene wallpapers (textures, effects, particles) that need no Wallpaper Engine assets |
| `genlayers.py` | `python3 -I genlayers.py widget DIR` / `blocks DIR` | synthetic scenes for "Adjust layers" |
| `gencontent.c` | `cc gencontent.c -o gencontent $(pkg-config --cflags --libs gdk-pixbuf-2.0 glib-2.0) -lm`, then `./gencontent OUTDIR` | a small scene project with a generated `.tex` and a GIF |
| `genworkshopfix.py` | `python3 -I genworkshopfix.py <source tree>/tests/fixtures/workshop` | writes the synthetic Steam Workshop replies the test `workshop` reads (invented items; nothing is fetched) |
| `fxkey.py` | `python3 -I fxkey.py 'statement;' ...` or `--file shader.frag` | prints the 64-bit key of a shader statement as the engine computes it (white space removed, FNV-1a), for maintaining the recogniser tables in `src/engine/fx_keys.h` |
| `mock-mpris-player.py` | `python3 mock-mpris-player.py NAME CONTROL_FIFO [--title T ...]` | a stand-in MPRIS player on the session bus (needs python-dbus and PyGObject) |

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
| `guicheck.py` | `Xvfb`, Mesa with llvmpipe, `xdotool`, ImageMagick (`import`), `dbus-daemon`, `gdbus` (GLib); libX11 (loaded through `ctypes`) for the case that closes a dialog | Pillow |
| `webcheck.py` | `Xvfb`, Mesa with llvmpipe, ImageMagick (`import`); a build with `walpe-web.so` | Pillow |
| `layercheck.py` | `Xvfb`, Mesa with llvmpipe, ImageMagick (`import`) | Pillow |
| `optcheck.py` | `Xvfb`, Mesa with llvmpipe, ImageMagick (`convert`) | none |
| `gencontent.c` | a C compiler, `pkg-config`, the development files of GLib and gdk-pixbuf | |
| `mock-mpris-player.py` | a D-Bus session bus | dbus-python, PyGObject |

A check that misses one of its programs or packages says so and exits with 77;
`optcheck.py` and `mock-mpris-player.py` do not look first and stop with Python's error.

**`webcheck.py` as root.** WebKit runs its web processes in a sandbox. When `webcheck.py` is
run as root it switches that sandbox off for the daemon it starts
(`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` in that daemon's environment only). The pages
that daemon loads are the ones the check writes itself; run the check as an ordinary user
to keep the sandbox on. No other tool changes it, and WalPE itself never does.

Licence: BSD 3-Clause, as WalPE (`LICENSE`).
