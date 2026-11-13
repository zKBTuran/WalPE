# Changelog

What changed in each released version of WalPE, newest first, written for people who use
it. Version numbers are `0.<minor>.<patch>`; every release is tagged `v<version>` in git.
Release names are album titles by the band Boris, one per release.

## Unreleased

What has changed since 0.14.0 "Flood" and is not in a release yet: answers to the first
reports from other machines. On KDE Plasma (Wayland) the wallpaper was gone after a click
on the desktop, Plasma's desktop icons could not be had together with it, nothing paused
it behind a fullscreen window, and the tray's menu had no working "Quit WalPE"; wallpaper
sound could only be switched on with a command, and video and web wallpapers had none; on
a laptop with two graphics cards the discrete one sat idle; a log sent with a report
said too little; and, found on the way, the sound server could move the capture of a
wallpaper that reacts to sound onto a microphone (PulseAudio; under Audio-reactive
wallpapers). And what was looked for on the desktops nobody had reported from yet: on
X11 desktops whose file manager owns the desktop (XFCE, LXQt, LXDE, MATE) the wallpaper
ran unseen, under Cinnamon, GNOME and KDE Plasma on X11 it covered every window, and
GNOME on Wayland had no wallpaper at all (under X11 desktops, and GNOME under Wayland).
**State of testing: that of 0.14.0 - Mesa's software renderer, Xvfb, a
headless sway, a PulseAudio null sink - with these additions. The changes for KDE Plasma
were run on a headless Plasma 6.8.2 (the layer also on a headless Plasma 5.27): KWin on
one and two virtual outputs, software renderer, a virtual pointer; a real Plasma session,
a graphics card there and a restart of KWin have not been run. The sound of video and web
wallpapers and the audio capture were measured against private sound servers with null
sinks (PulseAudio 16.1; PipeWire 1.0.5 with WirePlumber 0.4.17; the sound of web
wallpapers also PipeWire 1.6.11 with WirePlumber 0.5.19): nobody has heard it, and no
sound card and no microphone was there. The new
choice of a laptop's discrete GPU has never run on a machine with two GPUs: what was run
is the decision, on made-up machines, and the way back when the GPU's test fails. The
X11 desktops, GNOME Shell 46 and 51 with the extension, and six more Wayland compositors
were each run headless on the software renderer, started by hand or by a test; none in
a real session.** Three things are different without being asked for: wallpaper sound is
on, at half volume (under Sound); `gpu=auto` renders on a laptop's discrete GPU where
there is one (under Graphics cards; `walpe-ctl set gpu=display` is what `auto` was); and
on an X11 desktop whose file manager owns the desktop the wallpaper is now seen, with
that desktop's own icons behind it (under X11 desktops; `WALPE_X11_DESKTOP=root` is what
it was). A bare window manager on X11 - dwm, i3, openbox - gets the window of 0.14.0.

### 64-bit ARM

- Nothing changes in what an arm64 build does; what changed is how it is built across
  and what is known about it. A cross build on an x86-64 machine that has QuickJS
  installed took that machine's `libquickjs.a` and failed at the link; it now takes the
  target's (from the sysroot of the cross file) or says which library does not fit.
  `cross/aarch64-sysroot.ini` is new: the cross build against arm64 libraries in a
  directory of their own, tests under `qemu-aarch64` included.
- State of testing ([BUILDING.md](docs/BUILDING.md#state-of-testing-on-arm64)): built
  and tested in an emulated aarch64 machine (QEMU, software rendering) on 13 November
  2026, on X11 and on Wayland; a picture and a scene come out byte for byte as on
  x86-64. Not run on real ARM hardware.

### Wayland

- KDE Plasma: the wallpaper stays where it belongs. With 0.14.0 it covered Plasma's
  desktop icons until the first click on the desktop and was then gone behind Plasma's
  own wallpaper - or was never seen at all - while the daemon ran on and neither its log
  nor `walpe-ctl status` knew. On KWin the wallpaper now goes on the layer shell's bottom
  layer, which KWin keeps above Plasma's desktop and below every window; clicks on the
  desktop still reach Plasma (its menu opens). Every other compositor gets the background
  layer as before. `WALPE_WL_LAYER=background|bottom|auto` in the daemon's environment
  overrides the choice, the log says at start which layer is used on KWin, and
  `walpe-ctl status` has `wl_layer` and `wl_layer_why`
  ([WAYLAND.md](docs/WAYLAND.md#kde-plasma)). Plasma's desktop icons and widgets are then
  behind the wallpaper; the add-on of the next entry shows them.
- KDE Plasma 6: an optional add-on shows Plasma's desktop icons and widgets over the
  wallpaper. Plasma's wallpaper, its icons and its widgets are one window, so a wallpaper
  program of its own can only be in front of all of it or behind all of it.
  `contrib/plasma` holds a wallpaper type "WalPE" for Plasma's "Desktop and Wallpaper"
  dialog that draws no wallpaper itself: it makes Plasma's desktop window see-through and
  has KWin keep that window right above WalPE's wallpaper. It is not part of the build
  and nothing installs it by itself; a Plasma user installs it for himself, without root:
  `contrib/plasma/install.sh install`, then `contrib/plasma/install.sh select` (or the
  type "WalPE" in the dialog); `unselect`, `uninstall` and `status` are the other words.
  The wallpaper is still chosen in WalPE. While the daemon is not running - not started
  yet, quit, crashed - such a desktop shows a colour or a picture of its own with the
  icons over it, not a black screen. Needs Plasma 6.5 or newer, on Wayland; the script
  says what is missing and changes nothing elsewhere. The daemon's log at start and
  `walpe-ctl status` (`wl_plasma_addon`, `wl_plasma_icons`, `wl_plasma_hint`) say whether
  the icons are hidden and which command brings them back
  ([WAYLAND.md](docs/WAYLAND.md#kde-plasma),
  [contrib/plasma/README.md](contrib/plasma/README.md)).
- KDE Plasma add-on that a package installed: `contrib/plasma/install.sh` works with a
  wallpaper type that a package put into `/usr/share/plasma/wallpapers` for every user.
  There it said "not installed", `select` refused and `install` failed inside Plasma's
  package tool. `status` now says whose copy is installed, with its version - and, with a
  copy of the user's own beside the package's, that Plasma takes the user's, which may be
  the older one; `select` and `unselect` work with the package's copy; `install` and
  `uninstall` are about the user's own copy, and `uninstall` leaves the desktops their
  wallpaper type while the package's copy stays. The daemon's line about the hidden icons
  names Plasma's "Desktop and Wallpaper" dialog first and `install.sh` as what the source
  tree has: an installed WalPE has no such script
  ([contrib/plasma/README.md](contrib/plasma/README.md)).
- KDE Plasma with the add-on: the wallpaper gets the pointer from the add-on. Plasma's
  desktop window is then above the wallpaper and KWin gives it every pointer event. The
  wallpaper type hands on the position, everywhere on the desktop, and the left button
  and the wheel over the bare desktop; over an icon or a widget they stay Plasma's alone,
  and the right and the middle button are never handed on (Plasma's menu, paste). Only
  while a wallpaper uses the pointer (a web wallpaper, a scene that follows it); Plasma
  goes on getting every event as before. `walpe-ctl status`: `wl_plasma_pointer`.
- KDE Plasma 6: "Pause for fullscreen apps", "Pause when covered" and the focus rule
  work. KWin offers other programs no window list and goes on asking a wallpaper that
  windows hide for frames: with 0.14.0 a wallpaper behind a fullscreen or maximized
  window, a game included, kept being drawn there. While one of the three rules is on
  ("Pause when covered" is, by default) the daemon now loads a small script into KWin
  over the session bus - nothing is installed, no permission is asked for - and the
  script reports, per output, whether application windows hide at least 85 % of it,
  whether the active window is on it and whether that window is fullscreen, and only
  when one of the answers changes. With the three rules off there is no script and no
  connection to the bus; the script is taken out of KWin when the daemon ends.
  `walpe-ctl status` names `kwin` among the sources of the three rules, and
  `pause_kwin_script` says what the script is doing or why it cannot report (no session
  bus, KWin not on it, a KWin older than Plasma 6). `WALPE_KWIN_SCRIPT=0` in the daemon's
  environment keeps the daemon from using it
  ([WAYLAND.md](docs/WAYLAND.md#pause-rules)).
- A Wayland compositor without the layer shell (GNOME): with `backend=auto` the daemon no
  longer goes on to X11 by itself when `DISPLAY` is set. That X11 is Xwayland, where the
  wallpaper window lies above every other window, the library window included, and clicks
  pass through it to windows nobody can see. The daemon now ends with status 1 and a
  sentence that names the layer shell and the X11 session, and on GNOME the extension of
  the next entry, with which the daemon stays and takes Wayland there. X11 asked for by
  name (`--backend x11`, `WALPE_BACKEND`, `walpe-ctl backend x11`) is still taken, and the
  log says what that means there. Unchanged: an X11 session, a compositor that has the
  layer shell, `--windowed`.
- GNOME on Wayland: an optional extension for GNOME Shell shows the wallpaper there.
  GNOME's compositor has no layer shell and nothing in its place; what it has is a window
  type, the desktop's, that only GNOME Shell itself can give a window.
  [contrib/gnome](contrib/gnome/README.md) holds an extension that gives it to the
  daemon's wallpaper windows. It is not part of the build and `meson install` does not
  install it; a GNOME user installs it for himself (`contrib/gnome/install.sh install`,
  then log out and in). With it `backend=auto` takes Wayland on GNOME and the wallpaper
  is below every window and below the icons of "Desktop Icons NG", on all workspaces and
  behind the workspaces in the overview; "Pause for fullscreen apps", "Pause when
  covered" and the focus rule work per monitor from what the extension reports (the
  source `gnome` in `pause_rules`; `walpe-ctl status`: `wl_gnome_extension`,
  `wl_gnome_connection`, `pause_gnome_extension`). On GNOME Shell 46 to 48 the shell
  starts a second, small process of the daemon for the connection (`walpe-daemon
  --wayland-socket-to 4`). On every other desktop the daemon does not ask for the
  extension, and `WALPE_GNOME_EXT=0` keeps it from asking on GNOME. Known: the lock
  screen keeps GNOME's own picture; in the overview the wallpaper has square corners
  (and stands still on GNOME Shell 46); with "Desktop Icons NG" the pointer does not
  reach the wallpaper; a daemon started at login waits up to three seconds for the
  extension. **State of testing: a headless GNOME Shell 46.0 and 51.1 on the software
  renderer, no real session and no graphics card. The extension lists the versions 46
  and 51, and GNOME Shell loads it on those only**; for another version,
  `python3 -I scripts/gnomecheck.py --build <build directory>` (the test `gnome-shell`)
  says what works there ([WAYLAND.md](docs/WAYLAND.md#gnome)).
- "Pause when covered" where nothing reports the windows (a compositor without a window
  list; KDE Plasma while the script above cannot report, Plasma 5.27 for one):
  `pause_rules` said that the rule works, and it pauses there only for a locked screen
  and a display that is off. `pause_rules.pause_covered` now has a `note` that says what
  it pauses for and that a window covering the wallpaper is not known in this session;
  the settings window prints it under the switch.
- For those who build and test: what was run by hand on the headless Plasma is a test of
  the suite now, `daemon-kwin`. It starts a private KWin and Plasma's shell and looks at
  KWin's own screenshots and stacking order: the wallpaper before and after a click on
  the desktop, the pause rules, the add-on, the tray's menu. With the daemon of 0.14.0 it
  fails where the reports did; it is skipped where no KWin of Plasma 6 is installed
  ([DEVELOPMENT.md](docs/DEVELOPMENT.md#tests)).
- To try, off by default: `wayland_follow = on` in `gpu.conf` (or
  `WALPE_WAYLAND_FOLLOW=on`), and a monitor that changes no longer restarts the daemon.
  Every change of an output - a monitor unplugged, back from sleep on a compositor that
  removes sleeping outputs, moved in the layout, another scale or mode chosen in the
  desktop's settings, a rotation - makes the daemon restart itself in place: two to ten
  seconds, and every wallpaper on every monitor begins again. With the setting the
  daemon follows the change in place:
  position, scale, refresh rate and a half turn change nothing that is shown; another
  size in pixels gives the surface new buffers (a video plays on, a scene on that
  monitor is loaded again); a monitor that goes has its wallpaper unloaded and gets it
  back on a new surface when it returns; the other monitors' wallpapers go on where they
  were throughout. Still a restart in place: more monitors than the daemon has had at
  one time since its start (once per session, typically). Without the setting nothing
  changes. It is off because it has only run headless on the software renderer (sway 1.9
  and 1.12, labwc 0.20.2, Wayfire 0.11.0, river 0.3.17, KWin 6.8.2) and on no GPU driver
  or real monitor: whoever tries it, please say how it went with a monitor switched off
  and on, unplugged, another resolution, scale and rotation
  ([WAYLAND.md](docs/WAYLAND.md)). X11 is unchanged: a build without the Wayland backend
  has none of this code. (Found in review and repaired before any release: with the
  setting on, a monitor that went and came back kept the names of its last one or two
  wallpapers in memory, some tens of bytes each time, and for the first moments on its
  new output it could have been taken for a monitor whose render thread does not come
  back from a load, which restarts the daemon. And an output that came back while its
  render thread was in a frame that does not end stayed without a wallpaper: the restart
  in place that was to follow after twenty seconds never came. It comes now.)
- A monitor that got no wallpaper surface when the daemon started is tried again. A
  compositor that was not ready to show wallpapers yet - river 0.4 and later before its
  window manager is connected, a session still starting - left the daemon running with an
  empty monitor for good: it said so once and never asked again. It now tries again
  after 2, 4, 8, 16, 32, 60, 60 and 60 seconds (this one is on, with or without
  `wayland_follow`), shows the
  saved wallpaper at the first try that is answered, and says in its log,
  `walpe-ctl diag` and `walpe-ctl status` where the tries stand and when it has given up
  (after about four minutes; it runs on then, as before). Run on river 0.5.0 with a
  daemon started 18 seconds before a stand-in window manager: the wallpaper was there at
  the next try. (The log line of the daemon about a surface it did not get was
  "eglCreateWindowSurface failed for monitor 0 (0x3000)" on Wayland also where no EGL
  call had been made, and 0x3000 is EGL's number for success: it says "no surface to draw
  on" there now and points to the line above it, which has the reason. On X11 and
  wherever an EGL call did fail the line is the old one.) A new try waits half a second
  for the compositor's answer, and the daemon answers nothing else meanwhile; on GNOME
  Shell with the extension, where wallpapers are windows, it waited the three seconds of
  the start, longer than `walpe-ctl` waits for an answer. Half a second there too now.
  And where the graphics driver gave the render thread that waits for such a monitor no
  context, the daemon took ten seconds to quit once the monitor had its surface, and
  left with "its render thread has not come back" (found in review, made up for the
  test; no driver was seen to do it).
- A still wallpaper (a picture, a scene in which nothing moves, anything paused) on an
  output whose scale or resolution changes: until the daemon had restarted for the new
  layout - two to ten seconds - it was shown at its old size: its top left quarter
  enlarged, the picture in a quarter of the screen, or nothing (sway). It fills the
  output at once now, a little soft until the restart draws it anew; an animated
  wallpaper always did. Seen on niri, COSMIC and sway.
- `wayland_reconnect`: a daemon that waits for its compositor to come back ended at once
  when the old compositor was still on its way out and took one more connection only to
  close it ("the Wayland compositor closed the connection during start-up"; COSMIC's
  compositor does that for a moment after it is told to end). It waits on now, for the
  time that was set, as it does for a socket that is not there yet.
- The pointer's image over a wallpaper that takes pointer input (parallax of a scene that
  reads the pointer, cursor scripts, a web page) is the compositor's own arrow now where
  the compositor takes a shape by name (`wp_cursor_shape_v1`: sway, KWin, labwc, Wayfire,
  river, niri, COSMIC). The daemon used to load the arrow itself, from `XCURSOR_THEME` and
  `XCURSOR_SIZE` in its own environment and at 24 without them: where the session does
  not hand these to the daemon (a start by `systemd --user`, a compositor whose cursor is
  set in its configuration only) the pointer changed its look and size when it came onto
  such a wallpaper (64 instead of 24 pixels in the test). On a compositor without that
  protocol nothing changes. `WALPE_WL_IGNORE=wp_cursor_shape_manager_v1` gives the old
  way; `walpe-ctl status` (`wl_cursor`) says which one is in use. For those who build: the
  Wayland backend still builds on a system without libwayland-cursor (the library is
  optional; the first version of this entry did not compile there, found in review).
- A compositor that goes away could take the daemon with it by a crash (SIGSEGV in
  `eglSwapBuffers`) instead of the orderly end, or the restart with `wayland_reconnect`:
  for the moment it takes the main thread to stop them, the render threads went on
  presenting frames on the connection that was gone, and Mesa's software renderer (26.2,
  llvmpipe) does not always survive that. Seen twice in some three hundred lost
  compositors (KWin 6.8.2, headless). A render thread now presents nothing once the
  connection is known to be gone - neither a wallpaper's frames nor the two frames that
  are not a wallpaper's: the black one after a wallpaper that could not be loaded, and
  the one drawn when a monitor's wallpaper is taken away. Not seen with a GPU driver; the
  code was the same in 0.14.0.
- `wayland_reconnect`, three faults of a daemon that waits for its compositor. Under
  `kwin_wayland_wrapper` the socket stays while `kwin_wayland` is started again, so the
  daemon was connected at once and gave up when no answer came within 10 seconds, whatever
  the setting said (KWin 6.8 held still for 13 seconds after a crash: the daemon was
  gone); it waits for the answer as long as for the socket now. A compositor that came
  back and went again while the daemon was setting up EGL ended the daemon ("EGL init
  failed; cannot render"); it waits again. A signal that ended the waiting daemon left its
  control socket's file behind; the file goes with it now. The default is unchanged (0:
  the daemon ends when the compositor goes); [docs/WAYLAND.md](docs/WAYLAND.md) has what
  was measured with KWin 6.8 (`kwin_wayland --replace`, a crash) and sway, with and
  without the setting.
- `wayland_reconnect`: a daemon that waits for its compositor came back to it without
  end. A compositor that ended the daemon's connection every time - in the test a socket
  in front of sway that closed each connection three seconds after it was made - had the
  daemon restart in place every three seconds for as long as that went on, 29 times in 90
  seconds. It gives up now: the fifth loss in a row that comes less than 30 seconds after
  the connection was made ends the daemon with status 1 and one warning that names the
  setting, the way it ends without the setting; after one stay of 30 seconds the count
  begins anew, so a compositor that crashes three times in a minute is still waited for
  each time. And it does not come back at all to a connection the compositor ended with a
  protocol error: the compositor objected to something the daemon sent and would again
  (it used to restart in place and connect anew; a daemon that was waiting connected five
  times a second until the wait was over). The reason for both: under
  `kwin_wayland_wrapper` a Plasma session is over after about ten crashes of KWin, and a
  daemon that KWin died of would have come back until they were used up. The two numbers
  are fixed, there is no new setting, and the default is unchanged (0: the daemon ends at
  the first loss).
- `wayland_reconnect` and "Render on CPU": a daemon that had waited for its compositor
  rendered on the GPU driver from then on, whatever the setting said. The daemon that
  waits tests Mesa's software renderer on the display as every start does, and did so
  before it waited: with the compositor still away the test failed ("software rendering
  is switched on but not available: cannot connect to the Wayland compositor ...
  Rendering on the GPU"), and that stayed until the daemon was started anew. It waits
  for its display first now and asks then. The same order holds for the test of hardware
  acceleration for web wallpapers, which asks the display too (not run: the test machine
  has no graphics card). In the code since 0.14.0; found in review.
- More compositors than sway and KWin have been run now, all headless on the software
  renderer: sway 1.12, labwc 0.20.2, Wayfire 0.11.0, river 0.3.17 and 0.5.0, niri 26.04
  and COSMIC's compositor 1.11.0. [WAYLAND.md](docs/WAYLAND.md#compositors-that-were-run)
  says per compositor what was run, what was seen and what could not be run; the three
  entries above this one and the application id under "Library window" come from there.
  Hyprland did not start on the test machine (it needs a graphics device) and is still
  "expected to work". One thing to know: on COSMIC nothing reports a fullscreen window,
  the wallpaper behind one pauses through "pause when covered" (on by default) only.
- Web wallpapers in a session without an X display work with GTK 4.18 and later (Arch,
  Fedora 42, Debian 13, Ubuntu 25.04 and newer). With 0.14.0 every web wallpaper stayed
  off there: the page's hidden window is then on the Wayland display inside the daemon
  (the "private page host"), the web module opened that display before it initialised
  GTK, and GTK from 4.18 on ends the process for that. The daemon lived - it tries
  WebKit in a process of its own first - and its log said `gdk_display_open() was called
  before gtk_init()` and `web wallpapers are disabled in this daemon`. GTK is initialised
  first now, without a display, and the page host's display is opened after it; the
  session's own display is still never opened. A session with Xwayland (`DISPLAY` set:
  KDE Plasma, GNOME, sway by default) was not affected unless `WALPE_WEB_HOST=private`
  was set; sway with `xwayland disable`, niri and labwc without Xwayland were, and so was
  a session with `DISPLAY` left over and no X server behind it. Found by running the
  checks on Arch (GTK 4.24.1, WebKitGTK 2.54.2); Ubuntu 24.04 has GTK 4.14, which allowed
  the old order.
- On the private page host every page after the first is laid out at its real size
  again where GTK has no font resolution to give. The page host's display said nothing
  about the size of its screen in millimetres, WebKit works out a resolution from that
  where the GTK setting for it is missing, and scales the page by the result: a second
  page - the next wallpaper, or a page of its own on another monitor - had a viewport of
  0 x 0 CSS pixels, `devicePixelRatio` Infinity and every pointer position at 0, 0; one
  that sizes things in fractions of the window still looked right, one that uses pixels
  did not. GTK from 4.22 on has that setting on Wayland only from the desktop's settings
  portal, so this is a session without an X display and without such a portal (or
  without a session bus); seen on Arch in the checks. The screen now has the size that
  says 96 pixels to the inch, which is a scale of 1, as with the setting at its default.

### X11 desktops

- XFCE, LXQt, LXDE, MATE: the wallpaper is seen. With 0.14.0 the daemon ran and said
  "running" while the wallpaper lay behind the file manager's desktop window (xfdesktop,
  pcmanfm-qt, pcmanfm, caja). The wallpaper window now becomes a child of that window:
  above the desktop's own picture, below every window and panel; the desktop's menu still
  opens on a right click. The desktop's icons are behind the wallpaper
  ([DESKTOPS.md](docs/DESKTOPS.md) says how to switch them off per desktop).
- Cinnamon, GNOME on X11, KDE Plasma on X11: the wallpaper no longer covers every window
  and the panel. Mutter, Muffin and KWin paint a window they do not manage above
  everything; there the wallpaper window is a managed window of the desktop type. On
  Cinnamon the desktop's icons are seen above the wallpaper; under KWin the wallpaper
  window goes into Plasma's desktop window, as on the four desktops above.
- A bare window manager (dwm, i3, openbox, bspwm ...) gets the window of 0.14.0, unchanged:
  what is done is chosen by the desktop window that is found and by the window manager's
  name. A window of the desktop type counts as the desktop's own only where the window
  manager lists that type in `_NET_SUPPORTED`; dwm and i3 do not, and there such a window -
  another program's fullscreen window - is left alone and pauses the wallpaper as before.
- Clicks on the desktop reach a wallpaper that takes them (web pages, cursor scripts)
  under window managers that put a frame around the desktop window (xfwm4, openbox); they
  counted as clicks on a window.
- `WALPE_X11_DESKTOP=auto|root|window` and `WALPE_X11_WINDOW=auto|override|managed` in the
  daemon's environment override both choices; `walpe-ctl diag` names the desktop window
  and the kind of the wallpaper windows, `walpe-ctl status` has `x11_window`,
  `x11_window_manager`, `x11_desktop_windows`, `x11_wallpaper_parent`.
- The library window has its icon in task bars that show only what the window carries
  (MATE, LXQt, LXDE).
- **State of testing: every desktop named here was run headless on a software renderer,
  started by hand in Xvfb (XFCE 4.18, LXQt 1.4, LXDE, MATE 1.26, Cinnamon 6.0, GNOME
  Shell 46, KDE Plasma 5.27); none in a real session, none with a graphics card.**

### Web wallpapers

- With WebKitGTK 2.54 (Arch, and what follows it; 2.53.1 to 2.54.2) an animated web
  wallpaper no longer goes black around what moves. There a page showed all of itself
  for its first three frames, and from then on only what it painted anew in each frame -
  the moving parts - on black: a still background behind an animation was gone a tenth
  of a second after the page came up. A page that never repaints, and one that redraws
  all of itself in every frame, looked right. WalPE gives the page's view a see-through
  background, and these WebKitGTK releases clear the whole frame of such a view before
  they paint what changed (WebKit's own repair is in its development branch). With them
  the view's background is opaque black now, which WebKit does not clear; the daemon
  draws pages over black anyway, so the picture is the one a page always had - except
  for a page without a background of its own that blends its elements with what is
  behind it (`mix-blend-mode`): they blend with black there. Other WebKitGTK versions
  keep the see-through background, nothing changes for them. `WALPE_WEB_OPAQUE=1` or `0`
  in the daemon's environment chooses by hand
  ([ENVIRONMENT.md](docs/ENVIRONMENT.md)); a daemon without this change shows right
  pictures there with `WALPE_WEB_SNAPSHOT=1`, at the price of processor time. With
  `WALPE_WEB_SNAPSHOT=1` the view stays see-through with these releases too: snapshots
  never lost anything, and of an opaque view they made a page's `requestAnimationFrame`
  run 42 to 56 times a second where the limit was 30. Found in
  the checks on Arch with the software renderer; not seen on a desktop.
- The frame rate limit holds with a WebKitGTK that has no display throttle of its own
  (2.48.1: Alpine Linux 3.24). For a limit that divides 60 the daemon leaves the rate of
  a page's `requestAnimationFrame` to WebKit's throttle; that WebKitGTK has none, and the
  page ran at 60 a second whatever the limit (52.6 a second measured where the limit was
  30: twice the frames anybody saw). With a WebKitGTK older than 2.52 the limit is now
  kept by WalPE's script, which has always kept the limits that do not divide 60 (24,
  25, 40, 50): 29.3 a second after. With 2.52 and newer nothing changes for 30, 20, 15.
- Under a limit that script keeps, a page's `requestAnimationFrame` callbacks are called
  by WebKit, each in a frame that is due, and no longer from a loop of the script. A
  page could tell: `this` in a callback was not the window, an error a callback threw
  reached `window.onerror` as "Script error." without message, file and line, and
  `requestAnimationFrame(null)` failed a frame later instead of throwing a TypeError
  where it is called. And `walpe-ctl set fps=N` while a page runs now reaches the frames
  inside the page (an `<iframe>` of the page's own origin kept the old limit until the
  page was loaded again; a frame of another origin has the limit the page was created
  with, as before).

### Sound

- Wallpaper sound is on by default, at half volume: a new configuration has `scene_sound`
  on and `volume` 0.5 (0.14.0: off and 0, so a wallpaper with sound was silent until two
  settings were found). A configuration written by 0.14.0 or older in which sound is not
  switched on gets it switched on at the first start, once, and the daemon says so in
  its log: at volume 0.5 when it holds volume 0, at its own volume when one was set
  there (the settings window's slider, or the tray's "Unmute", which set it to 1) - so a
  volume that was turned up in 0.14.0 without effect is heard now. Sound that was on with
  volume 0 is kept, and so is whatever is set from now on: switch it off in Engine
  settings ("Wallpaper sound") or with `walpe-ctl set scene_sound=off`; that is kept. A
  wallpaper without sound costs nothing for it: no stream, no thread, no connection to
  the sound server. A configuration that has no `scene_sound` at all (written before
  0.14.0, or by a 0.14.0 that never saved a setting) and a volume above 0 has nothing to
  switch on - sound is on by default, at that volume, from the first start: the daemon's
  log says that too now ("config: this file of a version up to 0.14.0 has no switch for
  wallpaper sound: it is on, ..."), at every start until the file is saved by this
  version, which the first change of a setting or of the wallpaper does. Nothing is
  changed by it and nothing saved.
- Engine settings, "Audio": a switch "Wallpaper sound" above the volume, with a line
  under it that says what is heard now and why not (off, muted, the volume is 0, no sound
  server, the wallpaper is paused), and a switch "Mute". Until now the only way to switch
  sound on was `walpe-ctl set scene_sound=on`, and the slider and the tray's Mute set a
  volume that nothing played at. Switched on while the volume is 0, the switch sets the
  volume to 0.5 as well.
- A scene that has sound layers gets two rows at the top of its options in the library
  window, under "Sound of this wallpaper", whether or not its author gave it a music
  option: "Sound" (As in Engine settings / On / Off) and "Volume" (0 to 1, times the
  volume of Engine settings). They act on the running wallpaper at once, without loading
  it again, and are saved per wallpaper. From the command line:
  `walpe-ctl setprop walpe_sound=on|off` and `walpe-ctl setprop walpe_volume=X`. A
  scene without sound layers has no such rows. A video wallpaper has the same two rows
  unless its file is known to have no sound track, and so has every web wallpaper (the
  entries on video and web wallpapers below).
- Tray: the sound entry reads "Switch wallpaper sound on" while sound is switched off and
  does that; "Unmute" while `mute` or a volume of 0 silences it, and brings the sound
  back in both cases; "Mute" otherwise. With 0.14.0 it read "Unmute", set the volume to
  1, and nothing was heard.
- `walpe-ctl status`, member `sound`: silence because the wallpaper is paused is said
  ("nothing is playing: 1 wallpaper(s) with sound paused - sound stops while its
  wallpaper is paused ..."). "Pause when covered" is on by default: a window that covers
  the monitor - under a tiling window manager one tiled window, WalPE's own included -
  pauses the wallpaper and its sound with it. `walpe-ctl set covered=off` keeps it heard
  behind windows ([SOUND.md](docs/SOUND.md)).
- A packaged scene (`scene.pkg`) whose package changed after its copy was made without
  the sound files stayed silent with "cannot be opened" in the log; the log now says
  that the package has changed and that loading the wallpaper again makes a new copy.
- **Video wallpapers play their sound track.** They were silent by design (the player
  decodes pictures only), which nobody expects of a video with music. The first sound
  track of the wallpaper's file is taken out once with ffmpeg into the cache (FLAC), in
  the background and only when it is about to be heard, and played by what plays scene
  sound, under the same settings: `scene_sound` (or the wallpaper's `walpe_sound`),
  `volume`, `mute`; silent while paused; the same video on two monitors sounds once. One
  pass of the track is one pass of the clip, to the sample, in one stream: where the clip
  starts again nothing is heard of it. With sound off nothing is started, opened or
  decoded. A place in the track - after a pause, an unmute, when sound is switched on in
  the middle - is found without decoding what lies in front of it, so a video of two
  hours is back as fast as a clip. A wallpaper that is replaced while its track is still
  being taken out gives that work up (ffmpeg is ended, nothing stays in the cache), and
  the next video's sound does not wait for it; the track is taken out from the start the
  next time that video is shown. While another monitor still shows the same video the
  work goes on to its end, for that monitor. The first time, a long video's sound comes only when
  its whole track has been taken out (two hours: about half a minute on an idle
  machine). [SOUND.md](docs/SOUND.md#video-wallpapers).
- **An imported video keeps its sound track** (AAC in the MP4 the import writes). Up to
  0.14.0 the import dropped it: videos imported before have no sound and must be imported
  again from the original file. Workshop videos need nothing.
- **Web wallpapers: sound starts, and follows the settings.** A page's media with sound
  never started by themselves - WebKit waited for a click on the page, as a browser does:
  no background music, `new Audio().play()` refused, Web Audio stuck, and a `<video
  autoplay>` with a sound track that was not muted showed its first picture and stood
  still. Pages may now start their media. Their sound follows `scene_sound`, `volume`
  and `mute` (it followed none of them: a page that had been clicked played at full
  volume with `mute=on`): silent while sound is off, scaled by the volume setting
  otherwise. While a wallpaper is paused, `Audio` objects outside the
  document and Web Audio are silent too (only elements in the document were paused).
  A web wallpaper has the two sound rows of a scene in its options - its own switch and
  its own volume (`walpe_sound`, `walpe_volume`) -, applied to the page that runs and
  kept apart for every page. Off and mute are enforced by WebKit's own mute of the page,
  which no page script can undo; the volume in between is scaled by a script inside the
  page, which a page that fights it could defeat. [SOUND.md](docs/SOUND.md#web-wallpapers).
- **`walpe-ctl soundcheck`: why a web wallpaper is silent.** The status line `sound` now
  names web wallpapers too (it said "nothing is playing" under a page that was heard),
  and `soundcheck` asks the sound server once whether the page's stream ("WebKit") is
  muted or at 0 % there - which the server remembers for every later start, and which
  no setting of WalPE changes - and prints the two commands that undo it.
  [SOUND.md](docs/SOUND.md#a-web-wallpaper-stays-silent-although-the-settings-say-on).
- **Web wallpapers: a looping Ogg Vorbis sound goes on after its first pass.** With
  WebKitGTK 2.52 and GStreamer 1.24 the jump back to the start of an `<audio loop>` or a
  looping `Audio` object with an Ogg Vorbis file mostly never ended: the sound played
  once and the page stayed silent (9 or 10 of 10 starts here; WAV, MP3 and Opus loops
  were not affected; the same in a plain WebKitGTK window, and in 0.13.0 and 0.14.0
  once a click had started the sound). A sound whose jump back has not ended after
  0.3 s is loaded again and from then on started again each time it ends. That costs
  one break of 0.6 to 1 s at the first repeat and about 0.1 s at each later one.
- **Web wallpapers: their level and mute stay out of the sound server.** WebKit keeps a
  page's volume and mute as the volume and mute of its stream in the sound server, which
  stores them for later streams of the same kind - other programs' too - and from where
  WebKit reads them back. With a level of WalPE's on every page that meant: on PipeWire a
  page that was muted and unmuted stayed muted; after a quit while muted, or at volume 0,
  the next start was silent whatever the settings said; and another music player's stream
  was silent or lowered with WalPE's (found before any release had it). The page process
  now plays through a GStreamer plugin of WalPE's, `libgstwalpesound.so`, which keeps
  both inside the page process: the stream in the server stays at 100 % and unmuted, and
  its slider in the system mixer is yours alone.
  [SOUND.md](docs/SOUND.md#web-wallpapers).
  For packagers: one more file, `<libdir>/walpe/gst/libgstwalpesound.so`, in a directory
  of its own; building it needs GStreamer's core headers (`gstreamer-1.0`:
  `libgstreamer1.0-dev`, `media-libs/gstreamer`), which `-Dweb=enabled` now asks for and
  `-Dweb=auto` only warns about ([BUILDING.md](docs/BUILDING.md)).
- **Sound that stayed away after a pause (PipeWire 1.0).** A wallpaper's sound could stay
  silent after anything that had made it fall silent for a moment: `walpe-ctl pause`, a
  window over the desktop ("pause when covered"), a fullscreen window, mute, volume 0,
  sound off and on. When a wallpaper falls silent WalPE lets the sound server play out
  what the stream holds and waits for its answer. `pipewire-pulse` 1.0.5 (Ubuntu 24.04)
  now and then does not answer, or takes no more sound into that stream afterwards -
  about one pause in twenty to thirty here -, and none of WalPE's calls into the sound
  server had a time limit: the wallpaper's sound thread stayed inside the call, the
  wallpaper was silent until it was opened again, and every such thread kept one of the
  16 streams. The sound layers of scenes have this since 0.14.0, where sound was off by
  default; a video's track with the changes above. Not seen with PipeWire 1.6.11
  (WirePlumber 0.5.19) or PulseAudio 16.1. Every call into the sound server now has a
  time limit, a stream that does not answer in time is given up, and a new one is opened
  with the next sound: with PipeWire 1.0.5 the sound is then back about half a second
  after the resume where it was gone before (no resume silent for more than three seconds
  in 800, 12 streams given up and replaced; 10 of 300 silent for good before). A
  sound server that is stopped, or a sink that is suspended, no longer holds a sound
  thread either: the wallpaper plays on without sound and asks again every 5 seconds.
  WalPE makes these calls with libpulse itself; `libpulse-simple.so.0` is no longer
  loaded. [SOUND.md](docs/SOUND.md#what-is-played).

### Audio-reactive wallpapers and media integration

Measured against PulseAudio 16.1 and PipeWire 1.0.5 with null sinks and test tones, and a
stand-in music player; no sound card and no microphone was there
([AUDIO-REACTIVE.md](docs/AUDIO-REACTIVE.md), new, says what a wallpaper gets, where it
comes from and what it costs).

- **A wallpaper that reacts to sound could end up listening to a microphone.** WalPE
  captures the monitor of the default output - what the computer plays. On PulseAudio,
  on a machine without a microphone, plugging one in (a headset, a camera) made the sound
  server move WalPE's capture onto it: the bars then followed the room, a web wallpaper's
  audio listener was handed the room's spectrum, and `walpe-ctl status` went on saying
  that the output was captured. The capture now stays on the output's monitor whatever the
  server's defaults do, checks which source it is on before it analyses anything, and
  refuses one that is not the monitor of an output. Status and log name the source. Only
  `WALPE_AUDIO_SOURCE=<source>` makes it read something else.
- After a switch of the default output (loudspeakers to headphones) with a microphone
  present, the bars stood still on PulseAudio until the wallpaper was paused and resumed:
  the capture stayed on the old output. It follows the default output now, and goes on
  with the next one when its output disappears.
- PipeWire: while an audio-reactive wallpaper was shown, the output device never
  suspended - the capture kept it running and woke 40 times a second for silence. The
  output now suspends when nothing plays, as without the wallpaper, and the capture does
  not wake. `walpe-ctl status` says "waiting for ...: the stream starts when the output
  plays" meanwhile.
- A scene in which only the audio spectrum moves anything (a picture with audio bars)
  no longer wakes 60 times a second while nothing plays: it rests until the first sound
  (`WALPE_AUDIO_REST=0` in the daemon's environment keeps the old behaviour). With music
  playing nothing changes.
- `WALPE_NO_AUDIO=1` now stops the capture for web wallpapers too; it was looked at by
  scenes only.
- Media integration: a paused scene with a "now playing" widget no longer wakes once a
  second, nor asks the music player for its position every 5 seconds, while something
  plays (behind a fullscreen film, for example). What the player announces meanwhile -
  another track, a pause - is still there when the wallpaper comes back.
- Media integration: `contentType` is `music` (it was `audio`, a word Wallpaper Engine
  never sends, so wallpapers that look at it took a song for neither music nor video).
- Web wallpapers: `window.wallpaperMediaIntegration.playback.PLAYING` / `.PAUSED` /
  `.STOPPED` exist next to `PLAYBACK_PLAYING` ...: Wallpaper Engine's documentation uses
  both spellings, and a page written with the first one threw an error in its playback
  listener.

### Graphics cards

- `gpu=auto`, the default, renders on a laptop's discrete GPU. Until now it set nothing
  on any machine: on a laptop whose integrated GPU drives the panel, the integrated GPU
  rendered and the discrete one sat idle beside it - what the first report from such a
  machine showed (Intel and NVIDIA). Where an integrated GPU drives the displays and a
  discrete GPU with a kernel driver sits beside it, the daemon now tests that GPU when
  it starts, the way `gpu=discrete` always has - a short process of its own that has to
  draw one frame on it - and renders on it only when the test passes, within 5 seconds.
  When it does not, the display's GPU renders as before, and the log (a line starting
  with `gpu = auto:`) and `gpu_note` in `walpe-ctl gpu --json` say why. **On every other
  machine nothing changes**: with one GPU, with a card that drives the displays, where it
  is not known which device drives them, with "Render on CPU", or with one of the
  drivers' own variables (`DRI_PRIME`, `__NV_PRIME_RENDER_OFFLOAD`, ...) in the daemon's
  environment, nothing is set and no test process is started. This has not run on a
  machine with two GPUs ([GPU.md](docs/GPU.md)).
- `gpu=auto` on X11 asks the X server before it tests a GPU. The list of devices is all
  `auto` has on X11 when it decides, and a machine whose X server runs on its NVIDIA card
  alone, with the kernel's driver for the unused processor graphics loaded, has the list
  of a laptop that renders on its integrated GPU: there `auto` would have started its
  test at every start of the daemon, to learn that nothing is to be set. Where a card
  run by NVIDIA's own driver is in the list, the daemon now asks the X server for the
  names of its RandR providers first, on one short connection of its own. NVIDIA's X
  driver names the provider of a screen it draws `NVIDIA-0`: with such a name in the
  answer `auto` is `display` - no test process, nothing set, no note, no line in the
  log. Any other answer, none, or a build without libXrandr: the test, as described
  above. Nothing is asked with one GPU, with `gpu=display` or a named device, or on
  Wayland. The names are those of NVIDIA's documentation; no X server with providers
  was at hand (`xrandr --listproviders` shows them).
- `gpu=auto` says nothing about a display GPU that has no driver. With NVIDIA's driver
  loaded without its DRM module and the processor's graphics left without a kernel
  driver, the log and `gpu_note` said that rendering "stays on" the Intel device, which
  renders nothing there. Nothing was tested or set; the sentence was wrong and is gone.
- `gpu=display` is new and is what `auto` was: nothing is set, and the GPU of the X
  server or the compositor renders, whatever the machine. It is the value for a laptop
  on which the second GPU should stay asleep: rendering on the discrete GPU copies every
  finished frame to the other one and keeps both awake while a wallpaper is animated.
  `walpe-ctl set gpu=display`; `integrated` and `discrete` mean what they did.
  `walpe-ctl gpu` lists `display` and says what `auto` picks on this machine, and a
  change between `auto`, `display` and `discrete` restarts the daemon only when another
  device would render.
- The test of a GPU cannot hold up the daemon's start for good. Its only limit was an
  alarm the test process set for itself: a process stuck in a driver while a powered-down
  GPU does not wake, or one that left something behind holding its output, kept the
  daemon waiting without a wallpaper and without an answer to `walpe-ctl` - until now
  only for somebody who had named a GPU (`gpu=discrete`, `gpu=<device>`). The wait now
  ends by the daemon's own clock - 10 seconds for a GPU that was named, 5 for the one
  `auto` found, per test process - whatever the process does; it is then ended, and the
  log and `gpu_note` say "the test did not finish within 5 s".

### Tray

- The menu on KDE Plasma, and on any panel that keeps the entries it has read: when the
  tray was there before the engine - the usual start - the menu had headings where
  entries belong, a check box in front of "Settings…", entries twice, and no "Quit
  WalPE", or one that did nothing. A menu item now keeps its id for as long as it is in
  the menu, whatever is added or removed above it. Not particular to Wayland; a tray
  that docks through XEmbed was not affected.
- "Recent wallpapers" and a monitor's list of wallpapers are read again each time they
  are opened; on such a panel they kept the entries of the first time.
- Wayland: the question before quitting ("Quit WalPE?") is a window of the tray's own
  that is given the panel's activation token, so that it can come to the front on a
  compositor that is strict about new windows. On X11 nothing changes.
- A panel that does not answer when the tray registers its icon (a session that is still
  starting) is asked again after 2, 5, 15 and 30 seconds and then once a minute. The
  tray used to stay without an icon.

### Library window

- Wayland: "Engine settings", "Monitor resolution", the Workshop install dialog and the
  delete dialog say the application's id (`io.github.walpe.ui`) like the library window,
  and so get the WalPE icon instead of the generic one where the desktop entry is
  installed. [BUILDING.md](docs/BUILDING.md) names the two files that give the icon to
  a WalPE that is run from the build directory.
- Wayland, a session without a D-Bus session bus (a compositor started from a console
  without `dbus-run-session`): the library window itself said the id `walpe`, for which
  there is no desktop entry, while its dialogs said the right one. It says
  `io.github.walpe.ui` now, bus or no bus (seen on labwc 0.20 with GTK 4.24).
- A video added with `walpe-ctl add` while the window is open is listed; it used to
  appear after F5 or the next start.
- An engine that does not start is quoted, not only counted: "The engine stopped: cannot
  open X display (exit status 1). Restarting it…". The engine's own sentence - no display
  to draw on, a compositor without the layer shell - was in its log and nowhere in the
  window.

### Arch Linux and CachyOS (gdk-pixbuf 2.44, json-glib 1.10)

Found by building and running the tests on current Arch, whose gdk-pixbuf decodes
pictures through glycin (decoder processes of its own) and whose json-glib reads a text
with two values differently. The first entry under Security below belongs here too.

- TGA pictures as layer pictures of the old scene format, as particle sprites and as
  picture wallpapers: a true-colour or grey TGA that carries a colour map it does not
  use, and one whose colour-map flag is not 0 or 1, were not loaded there (glycin does
  not take such a header for a TGA file). WalPE now reads the TGA header itself and
  hands the decoder the same picture without the unused map; on Ubuntu 24.04 these files
  loaded before and load the same now.
- A `project.json` or `scene.json` with a second value behind its first one (two objects
  in one file) was refused there as "no object" - the wallpaper did not load and was
  missing from the library - where json-glib 1.8 gave the first value. It is the first
  value on both now. A text with anything else behind its first value is refused on
  both, as before.

### Security

What another program of the session or a wallpaper's files could do, found by a review of
the changes above and of what they touch. None of these was seen being used. One more
is under "Audio-reactive wallpapers" above: the capture that could end up on a
microphone.

- Pictures, on systems whose gdk-pixbuf decodes through glycin (gdk-pixbuf 2.44: Arch,
  CachyOS; not Ubuntu 24.04): the two things WalPE asked of gdk-pixbuf for a wallpaper's
  pictures - "this file to the PNG decoder and no other", "tell me the size and stop if
  I refuse it" - are not what happens there. glycin picks the decoder from the file's
  bytes whatever loader it was asked through, and decodes the picture in full before the
  size is reported. So a layer picture of the old scene format named `.jpg`, `.gif`,
  `.bmp`, `.tga` ... that was an SVG document, a HEIC picture or one of a dozen other
  formats was decoded by that format's decoder (librsvg, libheif ...), and a picture
  WalPE refuses for its size - in a `.tex`, as a layer picture, as a preview in the
  library or the Workshop page, as cover art - was decoded anyway, in glycin's decoder
  process, and then dropped: 830 MiB and 4 to 11 seconds for 12000 x 12000 pixels, 4.3
  GiB and 17 seconds for a GIF of 43 bytes that declares 20617 x 18254, two gigabytes
  there and two in the daemon for an SVG document of 114 bytes. This is so in 0.14.0
  (measured there) and was not seen in use. WalPE now
  reads the format and the declared size from the file's own first bytes and hands a
  file to gdk-pixbuf only as one of the formats a wallpaper can have and within the size
  limit ([SECURITY.md](docs/SECURITY.md#which-decoder-a-picture-reaches-and-when-its-size-is-known),
  with what is still glycin's to decide). WalPE's readers were then held against the
  decoders behind glycin, and four ways to tell WalPE one size and the decoder another
  were closed (a JPEG with a restart marker or a second frame header in front of its
  scan, a TIFF with its size in another number type, a JPEG XL container cut behind its
  first two bytes, an "AVIF" whose box length is a TGA header to glycin). One way stays
  open, by decision: an AVIF image sequence (an animated AVIF) is still handed to the
  decoder, for its first frame, where gdk-pixbuf has an AVIF loader (Ubuntu 24.04 has
  none by default), and its frames are as large as its AV1 data says whatever the file
  declares (3.7 KB cost 1.4 GiB in the decoder process on Arch; under "Known limitations"
  below). On Ubuntu 24.04 the same checks run and
  change nothing that was shown: 193 pictures of every format, header version and
  name gave the same pixels through previews, `.tex`, layer pictures and as the
  wallpaper before and after. A picture or GIF wallpaper is no longer opened three times to learn
  its format and size (on Arch that was three full decodes before the picture was
  shown).

- Configuration: one request on the control socket with half of a surrogate pair in a
  string (`"a\ud800b"`, which is valid JSON) put bytes that are no UTF-8 into
  `config.json`. The next start refused the file, set it aside as
  `config.json.broken-<time>` and began with the defaults: the wallpaper gone, mute off,
  the volume back at one half. The daemon now refuses a request whose strings are not
  UTF-8 and changes nothing, and a save replaces such bytes and says so, whoever put
  them there.
- Configuration: a number too large to be one (`1e999`) in a member this version does
  not read - one of another version - was carried along and written back as `inf.0`,
  which is no JSON: the same loss of every setting, one start later. A save now writes
  finite numbers only, reads its own text back before it replaces the file, and leaves
  the file as it is when that text would not load; `walpe-ctl set` and the settings
  window show the reason.
- Tray: the tooltip of the tray icon begins with the wallpaper's title - somebody else's
  words - and a panel may show it as rich text; KDE Plasma's system tray does. A
  wallpaper called `<img src="http://host/x.png">` made Plasma's shell ask that server
  for the picture when the pointer rested on the icon. `<` and `>` now reach a panel as
  the single angle quotation marks. Read in Plasma's and Qt's sources, not run against
  a Plasma session.
- A wallpaper's `project.json` or `scene.json` whose string ends in half of a surrogate
  pair followed by another `\u` escape was refused, as it should be, and cost memory
  that was never given back: json-glib (1.8.0) keeps the string's buffer, up to 64 MiB
  for a file of 32 MiB, each time the file is read - by the window whenever it lists the
  library, by the engine at every load of the wallpaper, by the tray for the title. Such
  a text is now refused before json-glib sees it. 0.14.0 has this too.
- A wallpaper whose frame does not end could not be left. On the software renderer a
  wallpaper's effect shader that loops a thousand million times per pixel (three lines
  of a `.frag` file) keeps the monitor's render thread inside one frame for good:
  another wallpaper was accepted with "ok" and never shown, `walpe-ctl quit` closed the
  control socket and then waited for that thread for ever - a process nobody could
  reach, with a CPU busy until it was killed by hand - and a restart in place did the
  same with the socket open. Now a monitor that has not taken the wallpaper it was given
  20 seconds after it was asked for is restarted with the daemon, in place (same process,
  the log says which wallpaper held it), and an exit or a restart waits 10 seconds for a
  render thread and then goes on without it. Nothing is done while nobody asks for
  another wallpaper, how long a frame may take is not judged, and a wallpaper that is
  still loading is left to load (the 20 seconds count from the end of the load). With a graphics card
  such a frame is the driver's and the card's to end; that has not been run
  ([SECURITY.md](docs/SECURITY.md#scripts-and-shaders)). 0.14.0 has this too.
- A wallpaper's shader could get around the limit on its macros. A shader whose text
  comes to more than a million tokens once its macros are replaced is not handed to the
  driver (eight macros that each name the next eight times are a kilobyte of source and
  16.7 million tokens: gigabytes and a crash in the driver's compiler). That number was
  estimated line by line from the `#define` lines, and where the driver's preprocessor
  reads a text differently a few bytes went past it: lines that end in a carriage return,
  a call's argument behind a `;` or on the next lines, a call no compiler takes (another
  number of arguments than the macro has, no `)`), a second `#else`, a `#version` line
  that is not the first, an `#undef` of one of the driver's own names, and empty
  arguments, which the driver's preprocessor copies like tokens and which leave none.
  Shaders of 134 to 190 bytes that counted as a few dozen tokens took 1.9 to 2.5 GB and 6
  to 13 seconds in Mesa 25.2's compiler, others more than the 3 GiB the process was
  allowed. The text is now preprocessed here to be counted (`src/engine/fx_preproc.h`; it
  only reads, the driver gets the text as it was written), and what that pass may cost is
  bounded as well: 12 million tokens moved, 24 MiB of text - what `##` copies included -,
  calls nested 512 deep in each other's arguments. A condition only the driver can answer
  is counted both ways and the shader must fit in each; a shader with more than 6 of them
  is not compiled. A shader that is refused fails like one that does not compile: its
  effect is not drawn. Of 8625 shader sources built from real scenes and effects none was
  refused (measured while the changes were written, before the last two of them; not
  repeated since) ([SECURITY.md](docs/SECURITY.md#scripts-and-shaders)). 0.14.0 has this
  too.
- A scene with very many particle objects: every object that names a particle file with
  children could bring 128 particle systems, and nothing limited the objects. 2000 of
  them - a `scene.json` of 300 KB and one particle file of 4 KB - took 66 seconds and
  1088 MiB before the first frame, and a scene file may be a hundred times as long. All
  particle objects together now have 4096 systems (the same scene: 6 seconds, 161 MiB);
  an object beyond that gets no further system and the log says so once. 0.14.0 has
  this too.
- The sound track of a video wallpaper (new with the changes above) was taken out into
  the cache whatever the disk had left: a video file of 25 MB with an hour of sound
  becomes 274 MB of FLAC in half a minute, six hours up to 2 GB. It now has the floor
  every conversion has - not begun, and stopped, when less than 256 MB are free
  (`WALPE_CONVERT_FREE_MB`, new, for both).
- Where `ffmpeg` in `PATH` is a wrapper script that starts the real program without
  `exec`: an import that was cancelled, a conversion that reached one of its limits and a
  sound track that was given up ended the script only, and the real ffmpeg ran on to the
  end of its source as nobody's child. Every ffmpeg and ffprobe the daemon starts now
  leads a process group of its own, and the group is what is ended. A daemon that quits
  or restarts itself now ends the ffmpeg of a sound track itself and removes its
  unfinished file, instead of leaving both to the kernel and to the next start. (Behind
  such a wrapper the real ffmpeg still outlives a daemon that is killed or crashes; its
  unfinished file is removed at the next start.) Imports and conversions of 0.14.0 have
  the first of these too.
- The cache of converted videos and sound tracks (`~/.cache/walpe/video`) can be given a
  size: `WALPE_VIDEO_CACHE_MB=<MiB>` in the daemon's environment removes, when the daemon
  starts, what was not played for the longest time until the rest fits. Without it
  nothing changes: no limit on the sum, 90 days without use.
- New with the changes above, and closed before anybody had them: a web page could undo
  mute and "off" for its own sound (now WebKit's own mute, under Sound), and with the
  Plasma add-on another program of the session could take the pointer's bus name and be
  sent the pointer's position (the add-on now sends only to the process whose wallpaper
  KWin sees under the desktop). What a name on the session bus is worth in the other
  direction is written down in
  [contrib/plasma/README.md](contrib/plasma/README.md#the-pointer).

### Building

- No compiler warnings on a current distribution. Arch Linux of November 2026 (GCC 16,
  GTK 4.24, gdk-pixbuf 2.44, GLib 2.90) built WalPE with 31 warnings, each a note about
  a call one of these libraries has marked as deprecated since the versions of Ubuntu
  24.04, which builds without any. Where the library has a successor that does the same,
  it is used (`g_sort_array` from GLib 2.82 on; a texture over a pixbuf's own pixels
  instead of `gdk_texture_new_for_pixbuf`). gdk-pixbuf's animation interface (animated
  GIFs) and GDK's X11 calls have none; they stay as they are, and the note is switched off
  around each such call and nowhere else. No picture, no frame timing and no particle
  changes, and no library has to be newer than before. The default, the X11-only and the
  Wayland-only build are warning-free on both systems, with `-Dtests=true` too.
- `walpe-collect --help` says how the installed command is called (it printed `sh
  walpe-collect.sh`), and the two messages of the video import about a missing ffmpeg or
  H.264 encoder no longer answer with a Gentoo command alone.
- WalPE builds and runs with musl as the C library (Alpine Linux 3.24, in a chroot). The
  build stopped at `malloc_trim` and `mallopt`, which are glibc's (`WALPE_MALLOC_MMAP_MB`
  is not read in a build for another C library), and the programs are linked so that a
  thread has the 8 MiB of stack it has with glibc: with musl's 128 KiB a wallpaper
  script that calls itself without end ended the daemon with a segmentation fault where
  it gets "stack overflow" as an error. Nothing changes in a build for glibc.
  [BUILDING.md](docs/BUILDING.md) has sections for Debian 13 and for Alpine Linux: the
  packages, and what was run there - each in a chroot on the software renderer, neither
  on a machine.

### Log and bug reports

- `walpe-ctl diag` prints the daemon's state in a dozen lines, made to be posted with a
  report: version, session, the window system and why it was chosen, what the compositor
  offers and lacks, the OpenGL driver, modules, sound, the pause rules and who reports
  them, and per monitor what it shows and which rule pauses it. Wallpapers are named by
  the last part of their path, the home directory is written `~`, and of `DISPLAY` and
  `WAYLAND_DISPLAY` only whether they are set. The daemon writes the same summary into
  its log once per start.
- The daemon's log (`~/.cache/walpe/daemon.log` for an engine the window started) now
  says: when a monitor pauses, by which rule, and when it runs again; who asked the
  daemon to quit (the program and its process id for a request over the control socket,
  the signal and who sent it); in its last line why the daemon ends, so that a start
  without that line before it is a daemon that was killed or crashed; when the first
  sound output opens and when the sound settings silence the wallpapers or let them be
  heard again; that a video is decoded on the CPU and how hardware decoding is switched
  on; and, loudly, an output for which no wallpaper surface could be made (`walpe-ctl
  status`: `monitors_without_surface`). The start-up line about "swap with damage" is
  worded so that it can be read. No timer and no thread were added for any of it.
- `scripts/walpe-collect.sh` (installed as `walpe-collect`) collects the facts for a
  report about sound, the wallpaper window on Wayland or the tray: it runs the daemon with
  its debug output going to files, asks for the few things only the user can do, puts
  everything back and packs `~/walpe-report`. `--redact` replaces the home path, the user
  name and the host name; nothing is uploaded. What the files hold and what to look at
  before sending: [BUG-REPORTS.md](docs/BUG-REPORTS.md). The issue forms ask for
  `walpe-ctl diag` and for that archive.
- Wayland: every entry of `wl_surfaces` in `walpe-ctl status` has `presented_flags`, the
  flags the compositor gave with the last frame it showed (-1 before the first). In a
  report from a real Plasma session they were the only thing that changed when Plasma's
  desktop came to lie over the wallpaper. A hint for a report; the daemon does not act
  on it.
- `walpe-ctl diag` no longer names the folders of a wallpaper that lies outside the home
  directory. A monitor's line quotes the reason a load failed, and with a library on
  another disk, or a file opened from `/tmp` or a stick, that sentence held the whole
  path. It now reads `LOAD ERROR: noscene: scene.json not found in noscene`: the folders
  the daemon knows are taken out, and any other absolute path is cut down to
  `.../<last part>`. Below the home directory it is `~/...` as before, also where GLib
  puts a file name between its own quotation marks (“...”). `walpe-ctl status` is
  unchanged.
- `walpe-ctl diag` has a line `sound server:` under `sound:`, the sentence of
  `walpe-ctl soundcheck` (under Sound): whether the sound server holds a web
  wallpaper's stream muted or at 0 %. Only the text that is asked for has it; the
  summary the daemon writes into its log at start does not ask the sound server.
- The daemon says which source it was built from. Every test build since 0.14.0 and
  0.14.0 itself answer "0.14.0", so nothing told a tester which one was running. The
  commit of the source (`077a0dd`, its first seven digits; with a `+` when the build was made in a checkout
  with changed files; `unknown` where neither git nor the files say) is the member
  `source_id` of `walpe-ctl status`, the line `source:` of `walpe-ctl diag`, in the
  daemon's first line in its log, and in what `walpe-collect` prints and writes down.
  `walpe-daemon --version`, `walpe-ctl --version`, `walpe-tray --version` and `walpe
  --version` print it of their own file without a running daemon (`walpe-daemon
  --version` was no option: it started a daemon; the window `walpe` answered "Unknown
  option --version"). It is found when the program is built: in a tree made by `git
  archive` (a release tarball, a test build's zip) from the file `SOURCE-ID`, which git
  fills in there, in a checkout from git. The version stays the released one, nothing of
  the build's time or machine is in the id - the same tree builds the same programs - and
  a new commit compiles one small file again, not the tree.

### Settings and walpe-ctl

- `walpe-ctl set` with a value that is not allowed now changes nothing. `walpe-ctl set
  fps=31 focus=sideways` was refused (exit status 3) and the frame rate limit was 31 all
  the same: the pairs in front of the bad one had been applied and saved, and the reply
  did not say so. The whole request is checked first, as it already was for an unknown
  key, and every refusal ends with "nothing was changed". A request that is accepted is
  applied as before.
- `wallpapers_dir` in `config.json` stays as you wrote it where it begins with `~` or is
  an absolute path. The first change of any setting after a start had turned
  `~/.local/share/walpe/workshop` into `/home/<name>/.local/share/walpe/workshop`. A
  relative path (`walls`) is still saved as the folder it named when it was first read,
  as before: kept as written, the daemon and the library window would each look below
  the directory they were started in.
- `walpe-ctl status`: `backend` is the video backend that plays, `ffmpeg`, also after
  `walpe-ctl set video_backend=mpv` (that word is still stored for configurations of
  earlier versions and means the same; the reply now says so). It showed `mpv`.
- `walpe-ctl pause`, `resume`, `status`, `quit`, `cancel-import` and `steam-libraries`
  with an argument, and `add` with a second file, end with status 1 and a sentence. The
  argument was dropped and the command carried out (`walpe-ctl pause 1` paused every
  monitor). `walpe-ctl status --json` is still the status: JSON is what status prints,
  and six other commands take `--json`. `walpe-ctl soundcheck` takes no argument either.
- A restart in place no longer waits without limit for a child process that does not end
  (WebKit's processes): one second after asking, two more after killing it, then the
  restart goes on and the log names the process.
- A start that cannot render ("EGL init failed; cannot render": no usable graphics
  driver, or the compositor went away during the start) ends with status 1 as before and
  no longer leaves the control socket's file (`$XDG_RUNTIME_DIR/walpe.sock`) behind with
  nobody listening. Nothing depended on it: `walpe-ctl` said that no daemon runs, and the
  next start removed the file itself.

### Known limitations

- Arch, CachyOS (gdk-pixbuf 2.44 with glycin): pictures are decoded by glycin's decoders
  there, not by the ones Ubuntu's gdk-pixbuf has. PNG, GIF, BMP, TIFF and TGA pictures
  of 8, 24 and 32 bits gave the same pixels on both; a JPEG differs by up to 3 steps of
  255 in a third of its values (another JPEG decoder) and a 16-bit true-colour TGA by
  one step; a grey TGA with an alpha channel (16 bits) is not decoded at all, and a
  64-bit TIFF is not recognised. Every picture costs a temporary file and a round trip
  to a decoder process (one process for all pictures of a kind, kept for 30 seconds),
  and asking a picture for its size alone is a full decode there: a picture inside a
  `.tex` that is asked for its size and then loaded is decoded twice.
- KDE Plasma: without the add-on (`contrib/plasma`, under Wayland above) the desktop
  icons and widgets cannot be seen while WalPE shows a wallpaper; they are part of the
  window of Plasma's shell that the wallpaper lies on. With the add-on they are shown
  over the wallpaper, and the wallpaper then gets the left button and the wheel over the
  bare desktop only, never the right or the middle button; the Overview shows the icons
  without the wallpaper. The add-on needs Plasma 6.5 or newer.
- KDE Plasma: the pause behind a fullscreen or maximized window needs Plasma 6 and the
  session bus. On Plasma 5.27 nothing pauses the wallpaper behind a window, a game
  included; it pauses there for a locked screen and a display that is off.
- GNOME on Wayland: only with the extension for GNOME Shell (`contrib/gnome`, under
  Wayland above), which GNOME Shell loads on the two versions it was run on, 46 and 51;
  on 47 to 50 the version has to be added to its `metadata.json` by hand.
- Sound on PipeWire 1.0 (seen on 1.0.5, Ubuntu 24.04's; `pipewire --version` says which):
  where the sound server leaves a stream standing after a pause, the stream is given up
  and replaced (under Sound above), and from then on a video's sound is 0.13 to 0.29
  seconds behind its picture and stays behind - still 75, 176 and 215 ms after a minute
  in the three cases that were watched; what puts it back in step was not measured. By
  its review the sound also begins at full level there, without the short fade-in. Not
  repaired.
- An animated AVIF (an AVIF image sequence) is shown by its first frame, as a wallpaper,
  a layer picture or a preview, where gdk-pixbuf has an AVIF loader - and nothing in such
  a file bounds what the decoder takes for it: its frames are as large as its AV1 data
  says whatever the file declares, and 3.7 KB cost 1.4 GiB and 15 seconds in the decoder
  process on Arch ([SECURITY.md](docs/SECURITY.md#which-decoder-a-picture-reaches-and-when-its-size-is-known)).
  A still AVIF is held to the size it declares.
- Video sound: videos imported with 0.14.0 or earlier have no sound track - the import
  dropped it - and have to be imported again from the original file. A video layer
  inside a scene is silent. A video's track is kept as a FLAC file in
  `~/.cache/walpe/video`, a few MB per minute, removed after 90 days without use; it
  needs `ffmpeg` in `PATH`.
- Web wallpapers: a looping Ogg Vorbis sound has a break of 0.6 to 1 s at its first
  repeat and of about 0.1 s at each later one (the way round WebKit's fault, under
  Sound). The sound of a `<video>` element in a page ends about 0.1 s after a mute, not
  at once.
- Web wallpapers in a build without GStreamer's core headers (`gstreamer-1.0`; the
  configure step warns): the plugin that keeps a page's level and mute out of the sound
  server is not built. Web wallpapers play, but muting WalPE or a volume of 0 can then
  silence other programs' music as well, also after WalPE has quit, and on PipeWire an
  unmute may not bring a page's sound back.
- `gpu=auto` on a laptop with two GPUs has not run on one. If the wallpaper is slower,
  stutters or drains the battery there: `walpe-ctl set gpu=display`.
- The full list is in [LIMITATIONS.md](docs/LIMITATIONS.md#known-limitations).

## 0.14.0 "Flood"

This release contains everything of 0.13.0 "PINK" below, which was not
released on its own. On top of it: memory and CPU that the daemon kept or spent for
things that were not in use, corrections to the fullscreen pause per monitor, a fix for
scene sound, a report of where the daemon's memory is, and what a review of the things
the daemon takes from outside found (under Security). **The state of testing is that
of 0.13.0: Mesa's software renderer (llvmpipe), Xvfb, a headless Wayland compositor (sway
1.9) and a PulseAudio null sink, with one addition: the maintainer ran the X11 build on
his own NVIDIA card before the release. The Wayland backend, AMD and Intel graphics, the
OpenGL ES path and the build for 64-bit ARM have still not been run on real hardware or
on a real Wayland desktop.**
Two settings are new and on by default: `video_scene_convert` (under Video) and
`web_unload` (under Other). The fixes are meant to change no picture; the one place where
the picture is not the same pixel for pixel is a scene's video played from its converted
copy.

### Wayland

- Web wallpapers on a Wayland session need no X any more. They run through Xwayland
  where an X display answers, as before; where none does, the page's hidden window lives
  on a Wayland display inside the daemon (the "private page host"; `walpe-ctl status`:
  `web_host`). `DISPLAY` left over in a session without an X server no longer switches
  web wallpapers off: the log and `web_host` say that the private page host is used
  instead. With that page host the page's process is not handed the compositor's socket
  by WebKit's sandbox ([SECURITY.md](docs/SECURITY.md)). Run with real pages on the
  headless sway, without any X; a page there is expected to animate at 60 frames a
  second at most ([WAYLAND.md](docs/WAYLAND.md#web-wallpapers)).
- The web module builds without libX11 (`-Dx11=disabled`, or a system that has none):
  `./build.sh -Dx11=disabled` gives a build in which the daemon and the web module use no
  X library. The module needs one of the two page hosts, and `-Dweb=enabled` fails at the
  configure step only when neither can be built. On an X11 session nothing changes, and
  the X11-only build (`-Dwayland=disabled`) has nothing of the private page host in it.

### Scenes

- A scene with a video layer that was paused before the video had given its first
  picture - a daemon that starts with the pause saved or the displays asleep, a restart in
  place while paused - rested on a frame without that picture: a scene whose background
  is a video was black until the pause ended. The paused monitor is now drawn until the
  picture is in the frame (three seconds at most for a clip that gives none), then rests
  as before.
- A layer with effects that changes its size from frame to frame - one that crosses a
  screen edge while the camera parallax follows the pointer, a camera shake sways the view
  or a script moves it - no longer fills the pool of effect targets. The pool kept one set
  of targets for every size of the last five seconds: on a 640x360 output with two such
  layers 137 to 262 targets (31 to 60 MiB) against 28 (2.8 MiB) with the pointer at rest;
  now 37 to 43 (5 to 9 MiB). A scene whose layers keep their sizes allocates as before.
- Particles: the reduced-resolution layer for large soft sprites keeps one buffer for
  each of its two sizes (1/2 and 1/4 of the view) instead of one buffer that was deleted
  and made again whenever the systems of a scene changed between the two. One test scene
  did that four times within a few seconds, several times an hour, for as long as it ran:
  23 buffers made in an hour on a 1920x1080 monitor, 3 now. The smaller buffer (0.5 MiB at
  1920x1080) now stays while the layer is in use.
- Particles: a particle system takes its memory when it makes its first particle, not
  when the wallpaper is loaded. A system that never emits - its object is not visible, it
  waits for an event, a script keeps its count at 0 - held room for up to 256 particles
  for nothing.
- Text layers: the glyphs of a wallpaper's fonts leave the daemon with the wallpaper.
  They stayed until the daemon was stopped: 0.3 to 0.7 MB for every wallpaper with a clock
  and a date that had been shown, more with large text.
- Text layers, with a text wallpaper on one monitor while others come and go on the next:
  the font files of a wallpaper that left stayed registered (about 6 kB per file), **and a
  later wallpaper could be drawn with the wrong font**: when its font had the same family
  and style name as a font of a wallpaper that had left, the file of that earlier
  wallpaper was used. A wallpaper's text is drawn from its own font file now.
- A program keeps no shader object alive after it was linked. Each program held its two
  shader objects with their source text for as long as it lived: 170 shader objects and
  743 kB of source for the 85 programs of one test scene, and whatever the driver keeps
  for a shader on top of that.
- The table that remembers which effect pass last used a program forgets a program when
  it is deleted. On a driver that gives every new program a new name it grew by a few
  dozen bytes per program with every change of wallpaper.
- With `fx_specialize` on or `fx_quality=fast`: an effect pass that draws one single
  colour (the blur-size pass of a bokeh effect, the same value all over the picture) was
  compared with its faster program again and again for as long as the wallpaper ran -
  behind draw 8192, 16384, 32768 and so on, at 60 frames a second after 2, 5, 9, 18, 36
  and 73 minutes - each time with a scratch target, a read-back and a buffer of 2 MiB
  each on a 1920x1080 monitor. Such a pass now keeps the program it draws with after
  draw 4096 (about a minute) and is not compared again. The picture is the one it drew
  before.
- Scene sound (`scene_sound`, off by default): a scene with many sound layers lost some
  of them for five seconds after a start or a resume on a busy machine, although the
  sound server had answered every stream. The layers open their streams one at a time,
  and a layer gave up a quarter of a second after it began to wait, whoever was being
  served. The quarter second now belongs to the layer whose turn it is: the others wait
  as long as the turns go round. Only behind a stream the server has not answered in that
  time do they play into nothing, and they ask again a quarter of a second later instead
  of five seconds later. With 16 layers against PulseAudio 16.1 on a loaded machine 3 of
  30 starts lost 2 to 15 layers before, none of 30 now. Measured against a null sink;
  nobody has heard it.
- OpenGL ES: with `fx_specialize` on or `fx_quality=fast`, an effect shader above the
  256 KiB limit was named in two warnings instead of one.

### Video

- A video inside a scene that is larger than the scene shows it (a 4K clip on a 1080p
  monitor) used to be decoded at its own size for as long as the scene ran: four times the
  decoding of what is seen. It is now converted once, in the background and at the lowest
  priority, into a copy of the size shown (`video_resolution`) in `~/.cache/walpe/video`,
  and the scene plays the copy from where the clip next starts again; the original plays
  until then. `walpe-ctl set video_scene_convert=on|off` (on by default; Engine settings,
  "Smaller copies of videos in scenes"), `walpe-ctl status`: `scene_videos`. Only 8-bit
  4:2:0 clips are converted, the frame rate is kept, the picture differs from the original
  by the scaling and one more encoding. A copy in the cache that cannot be read any more
  (cut short when the machine lost power) is removed and made again. A clip that is
  stored rotated gets no copy (it would never be played: `not made (the clip is stored
  rotated)`), and a copy that turns out not to be the clip (`not usable`) is no longer
  kept for ever. Measured with a 3840x2160 60 fps clip on a 1920x1080 monitor, software
  decoding: 16.1 - 16.8 ms of CPU per picture before, 4.9 ms from the copy (3.3 to 3.5
  times less), about 110 MB less memory; the conversion took 64 CPU seconds once
  (docs/VIDEO.md).

### Pausing

Corrections to "Pause for fullscreen apps" per monitor, which is new in 0.13.0:

- X11: a window that turns fullscreen within a moment of getting the focus - a game or a
  player that asks for fullscreen as soon as its window is there - pauses its monitor.
  The daemon could miss that change (35 of 300 such starts on a busy machine) and left
  the monitor running until the focus moved again.
- X11: the root window and the daemon's own wallpaper windows never count as the focused
  application. When another program named one of them as the active window, the daemon
  stopped hearing that a part of that wallpaper had to be drawn again: without a
  compositor, what a window had covered on a paused or still wallpaper stayed black until
  the daemon was restarted. **You can see this change with `walpe-daemon --windowed`:**
  the preview window no longer pauses itself when it is made fullscreen and has the
  focus.
- X11, `walpe-daemon --monitor N`: a fullscreen window on another monitor that reaches a
  few pixels into the daemon's monitor no longer pauses it. The monitor holding most of
  the window is looked for among all monitors of the screen.
- Wayland: with two connectors whose names differ in one letter only (`DVI-D-1` and
  `DVI-I-1`, `HDMI-A-1` and `HDMI-B-1`) a window on the second monitor paused the first
  one. This also concerned "pause when covered" and the pause for a focused application.
- Wayland: a window that starts in fullscreen pauses its own monitor only. For the moment
  between its appearing and the compositor naming its output (18 to 122 ms on sway 1.9)
  it paused every monitor. The first window a session ever shows still does.
- Shared rendering (two monitors of one size showing the same scene are rendered once):
  a paused monitor that follows the other one sleeps. It was woken for every frame of the
  monitor that draws, 52 times a second in the test, to find that it had nothing to show.
- Web wallpapers: a page opened while everything is paused (`walpe-ctl pause`, a monitor
  that is covered or has a fullscreen window) is paused like one that was running. It
  was not: the page kept running for the whole pause - about 30 animation callbacks a
  second in the test, and it was never told `setPaused` - behind a picture that stayed
  black, because the one frame a paused monitor took was the one from the moment loading
  ended, before the page had drawn. A new page now has one second after it finished
  loading for its first picture, a paused monitor shows what it draws in that second,
  and then the page is paused: in the test 1.8 seconds after it was opened, after 18 to
  26 animation callbacks, and from there neither its processes nor the daemon used CPU
  time worth counting. The same on both page hosts. Test: `web-wayland` / `web-x11`,
  case `pausedopen`.

### Security

What a wallpaper's files, a web page, a music player or another program on the machine
could do to the daemon. None of these was seen being used.

- Web wallpapers, the file server that hands a page its files (it listens on 127.0.0.1,
  where every program on the machine can connect):
  - a request for a part of a file - what a page sends to play or to seek in a video or a
    sound - made the daemon copy the whole file into its memory for as long as the answer
    was on its way: 200 MB for a 200 MB clip, per request. A part is now answered without
    a copy. The copy was also where a file that shrank meanwhile (a wallpaper being
    updated) ended the daemon;
  - the body of a request was read whole into memory before anything was checked: one
    request of 192 MB took the daemon's memory up by 388 MB for good. A body is read and
    dropped piece by piece now;
  - connections that sent nothing stayed open for ever: a thousand of them used up the
    daemon's open files, and `walpe-ctl`, the tray and the window then said the daemon
    was not running. At most 128 are open; for one more, the one that has waited longest
    is closed.
- Web wallpapers: a server that answers "login required" (401, or a proxy with 407) made
  WebKit put its own name-and-password dialog over the wallpaper, which nobody can
  answer; a page behind such a proxy was never shown. The request now goes on without a
  login.
- Pictures:
  - BMP: a file of 60 bytes that calls itself a BMP could make gdk-pixbuf's decoder
    reserve up to 4 GiB of memory, because the decoder takes three sizes from the file's
    first bytes before it checks anything. Those three fields are checked first now,
    wherever a BMP can come from: a texture, a layer picture, a picture wallpaper, a
    preview, cover art. A BMP that fails is not shown; a BMP as the format defines it
    passes. Found by fuzzing;
  - JPEG: a file of 106 bytes - an arithmetic-coded JPEG that ends inside its first
    scan - kept the system's JPEG library (seen with libjpeg-turbo 2.1.5) decoding for
    ever: the thread that loaded the picture never came back and kept one CPU core
    busy. An arithmetic-coded JPEG is not given to the decoder any more, wherever a
    JPEG can come from and whatever the file is called: a texture, a layer picture, a
    picture wallpaper, a preview, cover art. It is not shown, and the log says "an
    arithmetic-coded JPEG is not supported". Browsers, Windows and most other programs
    cannot open such files, so pictures as they are made everywhere (Huffman-coded,
    baseline or progressive) are not affected. A complete arithmetic-coded JPEG, which
    did load here, is refused as well. Found by fuzzing;
  - layer pictures of the old scene format are read by the decoder of their own format
    only: a file of another kind under a picture's name (SVG, XPM, PNM, a Windows icon
    ...) no longer reaches that kind's loader. Damaged files are taken as before, with
    one exception: a progressive JPEG that ends early is refused there now, as scenes
    of the current format always did; the part that had been decoded used to be shown
    when the cut lay in the picture's data;
  - a JPEG layer picture of the old scene format that is larger than a texture and has
    to be held whole by its decoder (progressive) is refused beyond 256 MiB of that store:
    136 bytes of file could take gigabytes.
- Library window: the preview picture a wallpaper names is one of its own files. A name
  with `..`, or a link in the folder, could put any picture you can read on the
  wallpaper's tile.
- Deleting a Workshop item no longer waits for ever, or reads gigabytes, when another
  item's `project.json` is a pipe, a device or a huge file.
- Music players (media integration): a cover file that was cut short while it was being
  decoded ended the daemon; it is read into memory first now. What a player says about
  its track is bounded: a repeated entry is freed (a program with a player name could
  grow the daemon by megabytes per message), and title, artist, album and the other
  texts are kept up to 4096 bytes.
- Video import and conversion: the `ffmpeg` of an import or of a conversion ends with
  the daemon. It used to go on after the daemon was killed or had crashed, without
  anything watching its limits of time, size and disk space.
- Two files that name wallpaper paths are written for your account alone (mode 0600),
  like `config.json`: the marker of wallpapers being loaded next to the control socket,
  and the tray's list of recent wallpapers.
- Log lines: control characters in names and texts of a wallpaper, and in what a scene
  script logs or throws, are shown as `\xNN`. A name with a line break in it could
  start a log line of the wallpaper's own wording, and an escape sequence in it reached
  the terminal the log was read in. Not covered: shader compile logs, the error texts
  of libraries, the daemon's own lines and the video and web modules.
- "Adjust layers": a layer's name is shown as one line of at most 200 characters,
  without control or direction characters; a name of any length with line breaks went
  into the list as it was.
- A layer key with ten digits (`walpe-ctl layer`, `config.json`) overflowed a number
  before it was refused; nothing came of it with the compilers in use.
- [SECURITY.md](docs/SECURITY.md) has a new section on web wallpapers: what a page gets
  (the file server, network access - on by default, also to the local network -, where it
  may navigate, which permissions and dialogs it does not get) and what WebKit's sandbox
  keeps from a page process.

### Other

- `walpe-ctl mem` says where the daemon's memory is: resident memory by kind of mapping
  and by name, the allocator's numbers, descriptors, threads, per monitor the OpenGL
  objects of its context (textures by size, buffers, programs) and what its wallpaper
  holds, and counters since the start. `mem --json` saves a report and `mem --diff A B`
  shows what differs between two. `mem --events on` starts counting how often textures,
  framebuffers, buffers and programs are made and deleted and pixels are read back;
  `--events off` stops it. The report is computed when it is asked for and at no other
  time: no timer, no thread, nothing per frame; nothing counts the OpenGL events before
  they are switched on. File names in the report are printed with their control
  characters made visible.
- A daemon that has shown a web wallpaper gives WebKit back. The web module (WebKitGTK,
  JavaScriptCore, GTK) cannot be unloaded, so after the last web wallpaper had been
  replaced it stayed in the daemon until the daemon was stopped: about 50 MB of its
  resident memory on the machine this was measured on, three threads (one of them wakes
  every five seconds) and GTK's connection to the display. Now the daemon restarts
  itself in place at the wallpaper change that leaves no monitor with a web wallpaper,
  the way it does when the monitor layout changes: same process, the control socket
  stays open, wallpapers and settings come back from the configuration. Every monitor
  loads its wallpaper again and shows nothing until it has, as at a start (one second
  without a picture on both monitors where it was measured); a wallpaper that then fails
  to load does not bring the web wallpaper back. At most once a minute, never on a
  timer, not while an import or a Workshop download runs or the copy of a scene's video
  is being made, and not when the settings cannot be saved (the restart would bring back
  the wallpapers that are saved). A restart that was put off is carried out at the next
  change of wallpapers, or earlier when the displays sleep or the session is locked
  (`pause_dpms`): nobody sees a wallpaper then. `walpe-ctl set web_unload=off` keeps the
  module loaded as before;
  `walpe-ctl status` shows `modules`. The video module (FFmpeg) is small and stays
  loaded.
- The daemon, `walpe-video.so` and `walpe-web.so` have to come from the same build, as
  before: the number of the module interface changed, and a module of 0.13.0 next to a
  daemon of 0.14.0 is refused.
- README: "One scene's picture depends on reused effect targets" is no known limitation
  any more; 0.13.0 fixed it (a layer with a tint and effects keeps its effects).
- Less work while nothing happens:
  - the daemon no longer sends its whole status to the library window and the tray for
    requests that only read (the performance panel asked every 2 seconds: 5 kB each time
    to both);
  - the tray no longer rewrites its list of recent wallpapers for every status line when
    two monitors show different wallpapers (a new file, two `fsync` and a rename each
    time, for every pause, resume or cover change);
  - the library window no longer lays itself out, draws and queries the X server for
    every status line of the daemon that says what the last one said;
  - the library window's main thread no longer wakes every 2 seconds for a performance
    panel that is closed;
  - the animated previews of a library window that is not on screen - on another
    workspace or tag, minimised - stop playing and give their frames back (up to 48 MiB,
    and a timer 20 times a second);
  - an open performance panel no longer builds its line anew for every answer of the
    daemon that says what the last one said;
  - X11: the look at the windows behind "covered" and the focus rule asks each window
    for its attributes only: 22 requests to the X server per focus change with eight
    windows where it was 53. What it finds is the same;
  - Wayland: a window's new title no longer makes the daemon go through the window
    list (a player or a browser changes its title all the time);
  - scenes: a layer that puts no pixel on the frame - parked outside the view, or scaled
    to nothing - no longer keeps the whole scene drawing at the full frame rate for an
    effect on it that follows the time. The picture is the same: such a layer drew
    nothing before either.
- X11: a wallpaper window that another program destroys (a script that tidies up
  windows, `xdotool windowclose` on the wrong id) is made again. The daemon did not
  notice: that monitor showed the root window for as long as the daemon ran. It now
  restarts itself in place, at most once in ten seconds.
- A video conversion that cannot start, or is cancelled, leaves no memory behind.
- Scenes: `texture_limit` and the texture quality reach a scene that stands still from
  its load on. They are applied in a scene's third frame, and a scene without anything
  that moves drew two and waited: its textures stayed as large as without the setting
  (921600 bytes of layer texture instead of 57600 with `texture_limit=256` in the test)
  until a pause and resume, an exposure or a moved layer brought that frame. With a
  number as the limit or the quality preset Low a still wallpaper now looks as the
  setting says at once. Nothing changes with the defaults (limit `auto`, full quality).
- Old-format scenes: a text texture that OpenGL refuses is deleted; it was kept.
- A lost GL context (suspend and resume, a GPU reset) no longer leaves a black monitor
  and a log line per frame: `walpe-ctl status` names it per monitor (`swap_error`), and
  the daemon restarts in place once, at most once a minute. Tried by making swaps fail
  on purpose; no driver here loses a context.
- A restart in place that cannot be begun (the daemon's binary can no longer be
  executed, as after an update that removed it) leaves the daemon running instead of
  ending it; `walpe-ctl status` has `restart_failed`.
- The unfinished copy of a scene's video no longer stays in the cache when the daemon
  ends while the copy is being started.
- `walpe-ctl quit`, a signal or a restart in place no longer wait until a scene that is
  being loaded is complete: the load gives up at its next layer. An exit during the
  load of a large scene took up to four to six seconds in the test and takes under one
  now. A layer or texture that is being read at that moment, and a frame that is being
  drawn, are still finished first.
- Less on disk: the copy of a packaged scene in `~/.cache/walpe/pkg` is made without the
  scene's sound files (MP3, Ogg, WAV, FLAC - by their names and by how they begin) while
  sound is off for that wallpaper, which is the default: 29 % of the bytes of 86
  Workshop packages. They are extracted when sound is switched on (`scene_sound`, or the
  wallpaper's own switch), also while the scene is shown. With sound on the copy is
  complete, as before; the cache's limit and what it removes are unchanged. A package
  with more than 256 sound files is extracted whole even while sound is off, and so is
  a copy that has to be written over while a wallpaper still shows it.
  `WALPE_PKG_WHOLE=1` in the daemon's environment extracts every package whole, as
  before.
- A package in which one name is both a file and a folder, or with a name too long to
  write, is refused before anything is extracted - with sound off as well. A package
  with a folder named `project.json` is refused.
- The daemon's log file is capped from the start. The cap (16 MiB, `WALPE_LOG_MAX_MB`) was
  looked at only with every 256th line of a run and never for a file a shell had opened
  with `> file`: a daemon started from an autostart entry at every login, saying little
  each time, let the file grow for ever.
- "Adjust layers": two layers with the same name whose ids in `scene.json` are negative
  (other than -1) got the same key, so an adjustment saved for the second one moved the
  first; and a layer with an id of ten digits could not be adjusted at all. Every layer
  has a key of its own now. Keys of layers with ordinary ids are what they were: saved
  adjustments keep their meaning. Found by fuzzing.
- Library window:
  - the performance panel asks the daemon nothing while its window is minimised or on a
    workspace that is not shown, and the animated previews stop there too. On X11 this
    now also holds under tiling window managers that never mark a hidden window (i3,
    bspwm, awesome, xmonad, dwm): 486 wake-ups of the window and 222 of the daemon per
    minute on a hidden i3 workspace before, none now;
  - previews kept in memory are given back while the window is not on screen;
  - with the daemon stopped the window no longer asks for it every 2 seconds (the
    performance panel: every 10 seconds);
  - a wallpaper that is copied into the library slowly is shown complete without Rescan;
  - the search ignores accents (é = e, ü = u) and treats i, ı, İ and I as one letter;
  - screen readers get names for the choices and the search field;
  - an engine that is running but does not answer (stopped, hung, its socket's queue
    full) is called "Engine not responding" for as long as that lasts. It was taken for
    stopped after 44 seconds and for "keeps crashing" after 66, and a restart was tried;
  - the error of a wallpaper that could not be loaded no longer stays as the tool tip
    of the status line after the line itself is gone;
  - "(paused)" in the line that says what the monitors show appears and goes with the
    pause (within a second); it used to change only with a wallpaper or the layout.
- `walpe-ctl quit` (and "Quit" in the tray) stops the daemon for good while the library
  window is open: the window used to start it again at once. A daemon that crashed or
  was killed is still started again by an open window.
- `walpe-ctl` says "running but did not answer in time" for a daemon that is busy, where
  it said "not running"; and it ends with status 1 when its output cannot be written
  (a full disk, a closed pipe). Wayland: the same answer comes while the daemon waits
  for a compositor to come back (the known limitation under 0.13.0 below still says
  "not running"); what it was sent meanwhile is not carried out later, as before.
- `walpe-ctl` writes titles, paths and layer names as the daemon sent them, in every
  locale: every character outside ASCII came out as `?` in `status`, `layers` and `mem`,
  also in the JSON that scripts read. Control characters are still made harmless, now
  also in the names of outputs that `resolution`, `gpu`, `gpu-stats` and `backend`
  print. Columns are padded by bytes, as before.
- "Adjust layers": a layer whose name is longer than 480 bytes has a key that works; it
  got one that was refused. Keys of shorter names are what they were.
- Import: a video with `%d` in its name (`take %03d.mp4`) got its thumbnail under another
  name, replaced a picture of that name next to it and was shown without a preview.
- Import: a width, height or rotation in what `ffprobe` says about a video that no
  whole number can hold (`1e30`) was converted in a way C leaves undefined; on x86-64 it
  happened to come out as "no size". Every build now takes it as that. Found by fuzzing.
- Build: **the tests and the developer tools are no part of a build unless asked for.**
  The new build option `tests` is off by default, and a build directory set up without it
  has no test, no test program and no developer tool: `ninja`, `meson compile` and `meson
  install` build the installed programs and nothing else. In 0.13.0 only `./build.sh`
  left them out; `meson install` and a plain `ninja -C build` built every test program
  (about a hundred) first. To run the tests: `./build.sh -Dtests=true` (or `meson setup
  build -Dtests=true`; for a directory that exists `meson configure build -Dtests=true`),
  then `meson test -C build`. `./build.sh` builds the installed programs only, as before,
  with the option or without. The developer tools (`sceneshot` ...) were never built by
  `meson test`, whatever the section of 0.13.0 says: one is built by its name (`ninja -C
  build sceneshot`), in a directory with `-Dtests=true`.
- `build.sh`: `WALPE_JOBS=0` meant `ninja -j 0`, which is no limit on parallel jobs; 0
  now means the default (the number of CPUs). A relative `WALPE_BUILDDIR` is taken from
  where the script is called; it was taken from the source directory.
- `walpe-ctl set web_accel=<the value it has>` answers "ok" like every other key; it was
  an error ("nothing to set", exit status 3).
- Library window: an arrow, Home, End or Page key pressed anywhere in the window (in the
  search field, for one) ended the rule that gives the wallpaper grid 64 % of the width;
  from then on the grid kept its width in pixels when the window changed size. Only moving
  the handle itself ends the rule now.
- `walpe-ctl help` marks `script_texanim` as a switch that loads the wallpaper again.
- Issue forms: they said that WalPE draws on X11 only; and the bug form now says that the
  output of `walpe-ctl status`, which it asks for, carries the name of the Steam account
  (`steam_user`) besides the home path.
- Documents corrected against the code: the two picture-changing fixes of 0.13.0 in the
  README's summary, the OpenGL ES shader size limit in SECURITY.md, two rows of
  SETTINGS.md (where `status` shows the window system; which monitors a fullscreen window
  across several pauses).
- README: shortened to one page. What it held is in `docs/` now: building, dependencies,
  the arm64 build and installing in [BUILDING.md](docs/BUILDING.md); what is played,
  `walpe-ctl`, pausing, Wallpaper Engine assets, the Workshop and the tray in
  [USAGE.md](docs/USAGE.md); the known limitations in
  [LIMITATIONS.md](docs/LIMITATIONS.md); architecture, tests and developer tools in
  [DEVELOPMENT.md](docs/DEVELOPMENT.md). Two statements were corrected on the way:
  Gentoo's `dev-lang/quickjs` has left the Gentoo tree (`-Dquickjs_libdir=` takes a
  QuickJS built by hand), and `data/walpe.service` is in the source tree and is not
  installed by `meson install`.
- Tests: a test that cannot run at all - no OpenGL context, or nothing but script checks
  in a build without QuickJS - ends with exit status 77, so `meson test` counts it as
  skipped; 13 such tests ended with 0 and were counted as passed. `meson test daemon-life`
  builds the web and video modules it uses (in a fresh build directory its video case was
  left out without a word). New or extended: scene sound against a slow and a refusing
  sound server; files of another kind under a picture's name; an effect with 1 to 33
  passes; rope trails at 10 to 1000 frames a second; the working size of effects on a
  colour-only layer; scene switches that reload once; more of the fullscreen rule on X11,
  and its case no longer ends in a crash of the test script when the X server refuses a
  connection for a moment; the converted copies of scene videos (`scenevideo`,
  `daemon-scene-video`); the memory report (`memreport`); real pages in WebKitGTK end to
  end on both page hosts (`web-wayland`, `web-x11`, `scripts/webcheck.py`). Fuzz harnesses
  for the parsers of wallpaper files (`tests/fuzz`: packages, textures, MP4, GIF and JPEG
  checks, JSON, puppets, particles, shader text, sound files, scenes) for libFuzzer
  (`-Dfuzz=enabled`, clang); their seeds and every input that once broke a parser run in
  the suite as `fuzz-regress-<target>`; 24 targets in all, with the control socket,
  `config.json`, Steam's replies, previews, the library scan and layer keys. From the
  review: the web file server (request bodies, idle connections, parts of files), the
  login answer, a stand-in music player (`media-player`), import (`%d`, a child without
  its daemon), old-format layer pictures, the tray and the library window next to a real
  daemon (`tray-life`, `ui-life`).

### Known limitations

- See the state of testing at the top of this section. The full list is in
  [LIMITATIONS.md](docs/LIMITATIONS.md#known-limitations).

## 0.13.0 "PINK" (not released)

A development version that was not released on its own: all of it is part of 0.14.0, and
the state of testing as it is now stands at the top of that section. It contains every
fix of 0.12.2. **Everything listed here was
built and tested only on Mesa's software renderer (llvmpipe), a headless Wayland
compositor (sway 1.9), a PulseAudio null sink and - the build for 64-bit ARM - under
emulation. Nothing of it has been run on real AMD, Intel, NVIDIA or ARM hardware or on a
real Wayland desktop.** On NVIDIA's driver under X11 with default settings it is meant
to behave as 0.12.2 does, apart from the scene features, two fixes that change the
picture of some scenes (layers with a tint and effects, rope trails; under Scenes) and -
with more than one monitor - the fullscreen pause rule below. What was compared, on the
software renderer: the development
tree before the fixes of 0.12.2 were merged in with
release 0.12.1, call by call (four scenes: the same OpenGL calls) and picture by picture
(the 91 test scenes: the same frames with the new features off); then the pictures of
the final tree with those of that earlier tree.

### Wayland

- A Wayland backend next to the X11 one: one layer-shell surface per output on
  compositors that offer the layer shell (sway, Hyprland, KDE Plasma and others; not
  GNOME's Wayland session). The daemon picks the backend when it starts: `walpe-ctl
  backend`, `walpe-ctl set backend=auto|x11|wayland`, `walpe-daemon --backend`.
- Pause rules get their information from the compositor and from logind where the
  session offers it; `walpe-ctl status` (`pause_rules`) says which rule can work. New:
  pause after a time without input (`walpe-ctl set idle=N`).
- The tray icon uses StatusNotifierItem on Wayland sessions; the window follows the
  desktop's dark / light preference there.
- Web wallpapers on a Wayland session run through Xwayland by default.
- Build options `-Dx11` and `-Dwayland`. `./build.sh -Dwayland=disabled` gives the
  X11-only build, in which the daemon and the web module link no Wayland library.
- A compositor that accepts the connection and does not answer within 10 seconds counts
  as absent: the daemon takes X11 when there is an X display, else it ends and says so.
  The connection of the pause rules gives up after 5 seconds.
- A per-monitor wallpaper or render resolution that was stored under the X11 name of an
  output (`HDMI-1`) is found on Wayland, where the same connector is `HDMI-A-1`.
- Wallpapers were shown on a headless sway 1.9 only. Known and left as they are: a
  wallpaper that is not shown keeps being drawn when "pause when covered" is off; a lock
  screen pauses through "pause when covered" only, unless the locker tells logind; with
  no monitor left `walpe-ctl status` still lists the monitors; while the daemon waits for
  a compositor to come back, `walpe-ctl` says it is not running, and what it was sent is
  not carried out later; a session started with a one-time socket (`WAYLAND_SOCKET`)
  cannot be restarted in place.

### Graphics cards

- `walpe-ctl gpu` lists the graphics devices; `walpe-ctl set gpu=auto|integrated|
  discrete|<device>` chooses the one that renders on a machine with two. `auto` (the
  default) changes nothing.
- An OpenGL ES 3.1 / 3.2 path for GPUs without desktop OpenGL 3.3 (`walpe-ctl set
  gl_api=auto|gl|gles`); effect shaders are translated to GLSL ES. On Mesa's software
  renderer every effect shader of the 91 test scenes compiles; pictures differ slightly
  from desktop OpenGL, because OpenGL ES has no texture LOD bias (minified textures are
  a little softer).
- On OpenGL ES an effect shader with more than 256 KiB of source is not compiled: a
  warning names it and its effect is not drawn. A source of that size is not translated
  to GLSL ES, and was handed to the driver as it was written. Desktop OpenGL still
  compiles such a source as written.
- `walpe-ctl gpu-stats`, and the same figures in `walpe-gpu-report` and in a profile: GPU
  busy, memory, power and temperature from the kernel's statistics on AMD and Intel.
- Driver hints of `gpu.conf` are handed to Mesa drivers where Mesa has a counterpart.
- Fallbacks for drivers without S3TC, with a smaller largest texture, or without float
  render targets.
- With `gpu=auto` the daemon does not read the PCI id list when it starts, and on
  NVIDIA's driver the GPU figures of `walpe-ctl status` read no file of `/proc` or
  `/sys`.

### Render resolution

- A render resolution per monitor: `native` (the default), 720p to 4K, or any size
  (`walpe-ctl resolution`, Engine settings > Resolution). The window asks once about new
  monitors; an existing installation is not asked and nothing changes size.
- Imported and converted videos are no longer tied to 1080p (`video_resolution`; a
  configuration from an older version keeps 1080p until you change it).
- Effects on colour-only layers of a scene are computed as wide as the longer side of
  a chosen render size where that is more than 2048 pixels (0.12: 2048 at most), and such
  a layer is sharper then. At a monitor's own resolution (`native`, the default) they
  stay at 2048 pixels as in 0.12, also on a monitor wider than that.

### Scenes

- Camera parallax, camera shake, bloom, sprite-sheet layers and their script control,
  and a script camera (zoom and position). Each has a switch (`walpe-ctl set bloom=off`,
  ...). **They are on by default and change the picture of the scenes that ask for
  them:** with default settings 16 of the 91 test scenes look different from the 0.12
  releases, because bloom, camera shake, sprite sheets and the script camera are on.
  `walpe-ctl set bloom=off camera_shake=off sprite_sheets=off script_camera=off` gives
  the earlier pictures back, each switch for its own feature. The look of the camera
  shake is an estimate; HDR bloom is not implemented. Limits: the sprite animations of
  one scene have at most 1048576 frames together (a layer beyond that shows its first
  frame), and a scene zoom that no float holds counts as zoom 1.
- The switch `script_texanim` (scripts drive sprite-sheet animations) is read when a
  wallpaper loads, as `sprite_sheets` is: changing it loads the wallpapers again.
  Switched off on a running scene it left the animations as the scripts had last set
  them, which is not the picture of a scene loaded with the switch off.
- A layer with a tint and effects keeps its effects. Such a layer could be drawn without
  them, depending on whether an effect target was new or taken back from the pool, so
  its picture was not always the same. Two scenes of the test library draw another
  picture now, whatever the feature switches say.
- A rope trail of a particle system is as long in seconds as its scene says at every
  frame rate. It was longer at low frame rates (a trail of 24 points over 0.2 seconds:
  twice as long at 60 Hz, four times at 30 Hz). Scenes with rope trails can look
  different at 60 Hz and below: the trails are shorter there.
- The Fluid Simulation effect runs on the grid its file declares, at a fixed 60 steps a
  second whatever the frame rate (`fluid_sim`, `fluid_quality`).
- Scene sound layers (WAV, MP3, FLAC, Ogg) through PulseAudio / PipeWire, **off by
  default**: `walpe-ctl set scene_sound=on volume=0.5`; `mute`; per wallpaper
  `walpe-ctl setprop walpe_sound=on|off`. The `volume` setting now has an effect.
  The sound layers of a scene open their streams one at a time, a few milliseconds
  apart: asked for in the same instant, some were refused by the sound server. A
  connection it still refuses is asked for again at once. A layer waits for its turn for
  a quarter of a second at most; after that it plays into nothing and asks again 5
  seconds later. Measured against a null sink by recording it; nobody has heard it.

### Video

- Hardware decoding chooses its decoder by what the machine and its FFmpeg offer and by
  the GPU that renders (`walpe-ctl set hwdec=off|auto|<api>`: VA-API, NVDEC, VDPAU, Vulkan
  Video, V4L2); on NVIDIA's driver NVDEC stays first. Optional zero-copy
  (`video_zerocopy`). Both off by default.

### Pausing

- "Pause for fullscreen apps" (`pause_fullscreen`) pauses the monitor the focused
  fullscreen window is on; the wallpapers of the other monitors keep running. Up to 0.12
  a fullscreen window on one monitor paused every monitor. **This changes the default
  behaviour on a machine with more than one monitor**; `walpe-ctl set fs_all=on`
  (`pause_fullscreen_all`, also a switch in the settings window) brings the old behaviour
  back. On X11 the monitor is found from the window's rectangle: the monitor holding
  most of the window, whatever border or gap a tiling window manager keeps around it,
  and every monitor a window across several covers at least half of. On Wayland it is
  the output the compositor names for the window; where it names none, every monitor
  pauses as before and `walpe-ctl status` (`pause_rules`) says so. Status reports
  `fullscreen` and `fullscreen_pause` per monitor. Tested with two monitor rectangles
  on Xvfb and two outputs of a headless sway 1.9; not on real monitors.

### Other

- Pictures from outside go to fewer decoders. A picture or GIF file chosen as the
  wallpaper is shown only as PNG, JPEG, GIF, BMP, TIFF, WebP, TGA, AVIF or JPEG XL: a
  file of another kind (an SVG document, a Windows icon ...), which earlier versions
  handed to whatever loader gdk-pixbuf had for it, is refused now. Cover art of a wallpaper with
  media integration is decoded as JPEG, PNG, GIF, WebP or BMP only
  ([SECURITY.md](docs/SECURITY.md)).
- The source builds for 64-bit ARM (aarch64) as well as x86-64, with meson cross files:
  cross-compiled with 0 warnings, and 65 of the 68 tests that need neither a display nor
  an OpenGL context pass under emulation. Not run on ARM hardware. OpenGL ES 3.1 is the
  least an ARM GPU must offer.
- Test setups for AddressSanitizer / UndefinedBehaviorSanitizer and ThreadSanitizer,
  end-to-end tests on a headless Wayland compositor.
- Library window: the wallpaper grid takes 64 % of the window's width by default (6
  columns at 1920 pixels) and the details panel the rest, at least 460 pixels. The share
  follows the window's size until you move the handle between the two; from then on
  yours is kept. Before, the handle stood at a fixed 780 pixels, and in a window that a
  window manager tiles or maximizes after showing it the grid could end up with two
  columns. The position is not saved across starts.
- Build: `./build.sh` builds the installed programs and modules only (the new ninja
  target `programs`), which makes a fresh build much shorter. The test programs and the
  developer tools are built by `meson test`, or one by name (`ninja -C build
  sceneshot`); a plain `ninja -C build` builds them too. The installed binaries are the
  same as before.
- Two changes that are meant to change no picture: an effect takes memory for the passes
  it has (the pass table of an effect with one or two passes took 44 kB and takes 1.4 or
  2.8 kB now), and a control point's `flags` in a particle file is converted like the
  other numbers of a wallpaper file, so a value that no integer holds gives the same
  result on every machine.
- Tests: in a build without QuickJS (`-Dscenescript=disabled`) `fx-alpha` and `layers`
  leave out the checks that need a script instead of failing, and `sig-wl` no longer
  waits for ever where `TMPDIR` is too long for a socket path (it makes its directory in
  `/tmp` then).

### Known limitations

- See the state of testing at the top of this section. The full list is in
  [LIMITATIONS.md](docs/LIMITATIONS.md#known-limitations).

## 0.12.2 "Amplifier Worship"

Security fixes and bug fixes for 0.12.1; no new features. Updating is recommended for
everyone who opens wallpapers, previews or Workshop pages that other people made.

### Security

What a hostile file or answer could do in 0.12.1, most serious first. All of these are
closed; the limits are listed in [docs/SECURITY.md](docs/SECURITY.md).

- GIF files (as a wallpaper, a preview or cover art) whose frames do not lie inside their
  picture, as the format requires, were given to the decoder unchecked. They are now
  refused before anything of them is decoded.
- A shader file with one extremely long statement crashed the graphics driver's shader
  compiler, and the daemon with it.
- A video file could keep the daemon busy for minutes to hours while it was opened; the
  daemon answered nothing meanwhile.
- A font file named by a text layer could take hundreds of megabytes and seconds of
  processor time, and could be of any format FreeType reads. Only TrueType / OpenType
  fonts with a bounded number of faces are loaded now.
- A texture file could hand its bytes to picture decoders that were meant to be out of
  reach (Windows cursor and icon formats), and cost seconds of processor time.
- A preview picture could reach decoders outside the allowed formats (SVG, XPM and
  others) in the window, and a small JPEG preview or cover picture could take half a
  gigabyte of memory while it was decoded.
- Cover art fetched over http(s) for a wallpaper with media integration could unpack to
  gigabytes inside the daemon.
- An answer to a Workshop request could crash the window (deeply nested data behind a
  comment or an unusual string).
- A scene of a few hundred bytes could take gigabytes of texture memory through text
  layers, hold millions of particles, or keep the render thread busy for minutes with
  particle systems. Text layers and particles now have totals.
- Shader files of megabytes could stall the render thread for minutes, and names from a
  wallpaper's JSON could push a shader past its size limit.
- A scene script could run its own code at a moment when the engine held a pointer into
  one of the script's audio arrays (no memory error was reached with the QuickJS that
  was tested).
- With `fx_quality=fast` (not the default) a crafted shader crashed the daemon.
- A wallpaper with thousands of options, or with very long option labels, froze the
  window for seconds to minutes.
- `walpe-ctl` printed titles and names from wallpapers and Workshop items with their
  control characters, so an escape sequence in them was obeyed by the terminal.
- A named pipe called `scene.pkg` in a wallpaper folder blocked the load for good; a
  symbolic link there was followed to a package anywhere on the disk, and the options
  window read linked files outside the wallpaper's folder.
- A program of your own account that sent requests to the control socket and never read
  the answers stalled the daemon's other clients and web wallpapers.
- Smaller ones: time zone names from a script that are no time zones, a text size beyond
  what the text library can hold, the script log limit after weeks of logging, reserved
  names in a package.

### Fixes

- A monitor that was plugged in or removed while the daemon was starting was never
  noticed: the wallpapers kept the old layout.
- A paused scene that had to draw a frame (a window moved away, a layer adjusted) moved
  on in time: script clocks jumped and fell back, timers stalled, the paused picture
  changed.
- Right-to-left text (Hebrew, Arabic) in a wrapped text layer was placed on the wrong
  side, or not drawn at all.
- A script on a property that also has a keyframe animation made the value run away
  once the animation stood still.
- Particles: an object that a script stops and plays again fires its burst again; a
  mirrored property animation fires its events at both ends.
- Window: a wallpaper the engine cannot load is no longer reported as set, the reason is
  shown; "Add folder" works on a new installation and says which folders could not be
  added; an import no longer copies files the library never lists; a number field that
  is emptied keeps its value (the frame rate limit used to fall to 5); a refused Workshop
  install is shown also after its dialog was closed.
- A setting that cannot be saved (full disk, unwritable home) is reported instead of
  answered with "ok": `walpe-ctl` prints the reason, `walpe-ctl status` has
  `config_save_error`.
- `walpe-ctl set`: mistyped values of `skip_hidden` and negative margins are refused
  instead of ignored; `walpe-ctl status` shows every saved setting; `walpe-ctl setprop`
  stores text options as typed.
- Switching web acceleration while an import or a Workshop download runs no longer ends
  it.
- Web wallpapers: a WebKit start-up test that never finishes is ended after 40 seconds
  and reported, instead of leaving every web wallpaper black; its child process is no
  longer left behind when the daemon ends.
- Memory that was never given back: the album cover of media integration, wallpaper
  files with a second top-level JSON value, the script memory of a wallpaper whose
  script got stuck (up to 64 MiB per load), two frame-sized textures of a wallpaper that
  is no longer shown.

### Performance

Measured on Mesa's software renderer only; the pictures are byte-identical before and
after. What it gives on a graphics card has not been measured.

- Loading a scene: the render thread's processor time up to the first frame fell from
  1122 to 948 ms, 1874 to 1302 ms and 1736 to 1348 ms for three scenes of the test
  library (sprite analysis, repeated shader text work, sprites decoded twice).
- Refracting particle systems copy only the part of the frame they can read, and not
  again what the system before them just copied: in two scenes at 1920x1080 the pixels
  copied per frame fell from 13.7 to 7.9 and from 22.2 to 9.1 megapixels.
- Puppet layers are no longer sent to the graphics card again while their pose stands
  still: 9 to 4 and 3 to 1 uploads per frame in two scenes.

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
  full list is in [LIMITATIONS.md](docs/LIMITATIONS.md#known-limitations).
