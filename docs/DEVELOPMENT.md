# Architecture, tests and developer tools

How the source is laid out, what the code relies on, how to run the tests, and where the
developer tools are. Building is in [BUILDING.md](BUILDING.md).

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
| `web-x11` | `Xvfb` | real pages in WebKitGTK on the X11 page host (`scripts/webcheck.py`): the picture, animation, the Wallpaper Engine listeners, pointer input, pause and resume with the page's CPU time, a page opened while everything is paused, restarts, a page process that dies, popups and choosers, network access off |
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
install is found the way the engine finds one
([USAGE.md](USAGE.md#wallpaper-engine-assets)) and is skipped otherwise. `fx-rewrite-keys` checks its table of statement keys against an install
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
