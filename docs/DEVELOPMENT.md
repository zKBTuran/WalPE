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
                                              (its page process's sound output is a GStreamer
                                              plugin of its own: gst/walpesound.c)
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
scripts/                     walpe-gpu-report.sh, walpe-collect.sh (installed); the end-to-end tests' harness:
                             displayenv.py, checkenv.py, lifecheck.py, waylandcheck.py, rescheck.py,
                             scenevideocheck.py, webcheck.py, wlsilentcheck.py, wlwaitcheck.py,
                             kwincheck.py, gnomecheck.py (GNOME Shell)
contrib/gnome/               the extension for GNOME Shell (JavaScript; no build installs it)
protocols/                   Wayland protocol descriptions (third-party, see Licence)
cross/                       meson cross files for aarch64 (compiler, sysroot, emulator)
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
| `daemon-wayland-wait-socket` | none | a daemon restarted in place that waits for its compositor and gives up: which control socket file it removes, and what its log says; that it waits as long for the answer of a socket that is there and silent; that a signal ends it and takes the socket file along (`scripts/wlwaitcheck.py`) |
| `daemon-wayland` | headless sway | what only outputs can do (`scripts/waylandcheck.py`): two outputs, a mode / scale / transform that changes under the running daemon, outputs that appear and go - by default with a restart in place, and with `wayland_follow` on without one while another output's wallpaper goes on (`follow`, `hotplug`; `busy`: an output that returns while its render thread is in a frame that does not end, which is a restart after twenty seconds) -, a pointer device, the choice between the two backends, the pointer's image over a wallpaper that takes pointer input (`cursor`), a compositor that ends and is started again under a daemon with `wayland_reconnect` (`reconnect`), and one that does not keep the daemon's connection or ends it with a protocol error (`flap`: a stand-in socket in front of the compositor's) |
| `daemon-kwin` | a private KDE Plasma session: KWin on two virtual outputs, Plasma's shell | what only KWin and Plasma can show (`scripts/kwincheck.py`): where the wallpaper is among Plasma's windows before and after a click on the desktop, the layer and what status says about it, the pause rules through the script in KWin, the add-on of `contrib/plasma` installed, selected and taken away, the tray's menu at Plasma's own StatusNotifierWatcher |
| `gnome-shell` | headless `gnome-shell` (also `gdbus`, `dbus-daemon`; `gjs` for two cases) | the daemon and the extension of `contrib/gnome` on a private GNOME Shell with the software renderer (`scripts/gnomecheck.py`): the wallpaper's window becomes a desktop window on its monitor; nothing of it stays in the shell when the daemon is quit, killed, or ends after the process GNOME Shell 46 to 48 starts for its connection; another program with the wallpaper's application id and title keeps an ordinary window; what the extension reports about covering and focused windows; the screen locked and unlocked (a stand-in for the display manager gives the shell a lock screen); the bounds on what it takes from the session bus; a wallpaper window the shell does not answer is tried again without holding `walpe-ctl` (`retry`). `WALPE_CHECK_GNOME_SHELL` runs the shell of another root (a chroot with another GNOME version) |
| `web-wayland` | headless sway without Xwayland, no `DISPLAY` | the same pages on the private page host (`scripts/webcheck.py`), and what only a Wayland session has: two outputs, outputs that come, change and go, `DISPLAY` left over without an X server, what the page's sandbox sees of the session, the session's clipboard (needs `wl-copy` / `wl-paste`) |

The X ones need `Xvfb` (and use `xdotool`, `xrandr` when present), the Wayland ones `sway`
and `grim` (`weston` adds a compositor without layer shell); each is skipped where its
programs are missing or the build has no backend for it. The headless compositor is sway
on wlroots' headless backend with the pixman renderer and a configuration of its own;
`scripts/displayenv.py` holds all of it and can be used by hand (`python3 -I
scripts/displayenv.py run --display sway -- build/walpe-daemon`). `daemon-wayland` can be
run against another wlroots compositor by hand, which the test suite does not do:
`python3 -I scripts/waylandcheck.py --build build --display labwc` (or `wayfire`, or
`river` up to 0.3; it needs `wlr-randr` as well). The other Wayland tests are written
for sway. `WALPE_CHECK_STRICT=1`
turns "cannot run here" into a failure.

`daemon-kwin` (its own suite, `kwin`) is the one test that runs the daemon on KDE Plasma
itself. Every other Wayland test uses sway or a stand-in, and release 0.14.0 lost its
wallpaper at the first click on Plasma's desktop with all of them passing: sway has no
desktop window to raise, and the daemon's own status knew nothing. So this test looks at
what KWin shows and does: the pictures are KWin's screenshots, the stacking order and the
windows' states are read by a script inside KWin, the pointer is a device KWin makes for
the test, the application windows are real Wayland clients.

```sh
meson test -C build daemon-kwin                      # or: --suite kwin
python3 -I scripts/kwincheck.py --build build        # by hand: one line per check
python3 -I scripts/kwincheck.py --build build --only stack,pause    # cases: stack, pause, addon, tray
python3 -I scripts/kwincheck.py --build build --outputs 1           # one output instead of two
python3 -I scripts/kwincheck.py --build build --no-shell            # KWin alone
python3 -I scripts/kwincheck.py --build build --daemon old/walpe-daemon --tray old/walpe-tray
```

- **Needs** `kwin_wayland` of Plasma 6 and `dbus-daemon`. Without them, or with the KWin
  of Plasma 5, the test is skipped (exit 77) with one sentence that says what is missing.
  With `plasmashell` (and what comes with it: `kded6`, `kpackagetool6`, `dbus-send`) it
  also runs the parts that need Plasma's desktop window, the add-on and the tray; where
  the shell is missing those are left out, and the output says so.
- **Starts** a session of its own in a temporary directory: runtime directory, session
  bus, `kwin_wayland --virtual` on two outputs of 640 x 400, Plasma's shell with `kded6`,
  a daemon per case. Nothing of your session is used, no autostart program runs, the
  add-on is installed into the temporary directories only, and what was started is
  stopped by process id (the services the private bus started for Plasma's shell by
  their runtime directory, which nothing else has).
- **KWin needs a device to draw on**: from 6.8 on a DRM render node or `/dev/udmabuf`.
  On a machine with neither (a container) the test preloads a stand-in for the latter
  into KWin (`tests/kwin/udmabuf_shim.c`, built with the tests, loaded into nothing
  else); KWin's window management and script interface are untouched by it.
- **It fails on what it was written for.** With the daemon and the tray of 0.14.0
  (`--daemon`, `--tray`) the case `stack` fails in both orders of the report - the
  wallpaper behind Plasma's desktop from the start, and gone after the click - and so do
  `pause` (no script) and the ids of the tray's menu; with `WALPE_WL_LAYER=background`
  in the environment today's daemon fails `stack` the same way.
- **Time**: about a minute; a run in which much fails waits for each thing that does
  not come (against 0.14.0: two and a half minutes). `KWINCHECK_PATIENCE=N` multiplies
  the waits.

What it does not cover: a graphics card (KWin and the daemon draw with Mesa's software
rasteriser; it has not been run on a machine with a render node, where KWin would use
that); a real session (no login, no session manager, no lock screen, no Xwayland, no
PipeWire, no input device but KWin's own fake one); KWin or Plasma's shell restarted
under the running daemon; activities and virtual desktops, "Peek at Desktop", the
Overview; outputs that come, go, change or have a scale (KWin's virtual outputs are
fixed when it starts: `daemon-wayland` does that on sway); video and web wallpapers
(the wallpapers are generated one-colour scenes); the menu as Plasma draws it (the
tray's menu is read over D-Bus, as a panel reads it); Plasma 5. It has run on Plasma
6.8.2 only.

One test talks to a sound server: `audio-server` starts a PulseAudio of its own (`pulseaudio`
and `pactl` in a temporary directory, null sinks only, no session bus) and plays tones into
it: which source the capture of audio-reactive wallpapers reads, that it is not moved to a
microphone, that it follows the default output
([AUDIO-REACTIVE.md](AUDIO-REACTIVE.md#where-the-sound-comes-from)). Nothing is heard and
the session's sound server is not contacted; it is skipped where the two programs are
missing. Every other sound test uses stand-ins or the null device.

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
| `arm64-sysroot.sh`, `arm64cross.py`, `binfmt-ns.py` | the arm64 libraries for a cross build, the cross build with its tests under emulation, and a namespace in which the kernel starts arm64 programs through qemu (for the tests that start the daemon) |
| `genworkshopfix.py` | regenerates the synthetic Workshop replies in `tests/fixtures/workshop/` |
| `fxkey.py` | prints the hash key of a shader statement, for the recogniser tables in `src/engine/fx_keys.h` |
| `mock-mpris-player.py` | a stand-in media player on the session bus |

```sh
git fetch origin devtools:devtools && git worktree add ../walpe-devtools devtools
python3 -I ../walpe-devtools/tools/dev/restorecheck.py --build build
```

The tools take the build directory as `--build`; `../walpe-devtools/README.md` has the
invocation of each.
