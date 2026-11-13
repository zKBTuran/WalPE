# Wayland

`walpe-daemon` draws its wallpapers either as an X11 client (one window per monitor below
all other windows, as in every earlier version) or as a Wayland client (one layer-shell
surface per output on the background layer; on KDE Plasma on the bottom layer, see
[KDE Plasma](#kde-plasma)). One daemon binary can hold both backends; it
picks one when it starts and keeps it until it ends. On an X11 session nothing is meant to
change.

> **State of testing.** The Wayland parts were built and tested only on Mesa's software
> renderer (llvmpipe). Wallpapers were shown on one compositor, a headless sway 1.9
> (wlroots 0.17, pixman renderer); a headless weston 13 served only as the compositor
> without layer shell that the daemon has to turn down. What ran on sway end to end:
> pictures, scenes, videos and web wallpapers on one and two outputs (web wallpapers also
> with no X server anywhere, see [Web wallpapers](#web-wallpapers)); outputs that appear,
> go and change their mode, scale or transform under the running daemon (with a restart
> in place, and with `wayland_follow = on` in place: that also on sway 1.12, labwc,
> Wayfire, river 0.3 and a headless KWin 6.8.2 with two virtual outputs); the restart in
> place, and the wait for a compositor that was killed and started again; the pause rules
> with stand-ins for logind and for the compositor's idle and window-list protocols.
> One more thing was looked at on one more compositor: where a video wallpaper lies
> relative to Plasma's desktop, without and with the add-on for Plasma, on a headless
> KWin 6.8.2 (without it also 5.27.11) with Plasma's shell ([KDE Plasma](#kde-plasma)).
> On 2026-11-12 six more compositors were run, headless and on the software renderer
> again, from Arch Linux's packages of that day: sway 1.12, labwc 0.20.2, Wayfire 0.11.0,
> river 0.3.17 and 0.5.0, niri 26.04 and COSMIC's compositor 1.11.0. What was run on
> each, and what could not be, is in [Compositors that were run](#compositors-that-were-run).
> Hyprland did not start without a graphics device and stays "expected".
> GNOME was run as well, headless again: GNOME Shell 46.0 and 51.1 with the WalPE
> extension for GNOME Shell, without which there is no wallpaper there ([GNOME](#gnome)).
> Nothing on this page has been
> run on a real Wayland desktop, on a real output or on a GPU.
> What is said below about Hyprland follows from the protocols it is
> documented to offer; it is marked "expected". Reports are welcome.

## What it needs

A compositor that offers the layer shell (protocol `zwlr_layer_shell_v1`), and an EGL with
`EGL_KHR_platform_wayland` (Mesa has it; NVIDIA's driver needs its `egl-wayland` library).

| Compositor | Layer shell | |
|---|---|---|
| sway | yes | tested headless (1.9 and 1.12) |
| KDE Plasma (KWin) | yes | shown on a headless Plasma 6.8 and 5.27; Plasma's desktop icons are behind the wallpaper unless the add-on for Plasma is used: see [KDE Plasma](#kde-plasma) |
| labwc | yes | tested headless (0.20.2) |
| Wayfire | yes | tested headless (0.11.0) |
| river | yes | tested headless (0.3.17, now "river-classic"; 0.5.0 with a stand-in for the window manager it needs: see [below](#compositors-that-were-run)) |
| niri | yes | tested headless (26.04, as a window on another compositor) |
| COSMIC | yes | tested headless (cosmic-comp 1.11.0, as a window on another compositor; nothing there reports a fullscreen window: see [Pause rules](#pause-rules)) |
| Hyprland | yes | expected to work, not tested (0.57.2 does not start without a graphics device) |
| GNOME (Mutter) | no | shown with the WalPE extension for GNOME Shell, which is not part of the build: see [GNOME](#gnome). Run on a headless GNOME Shell 46 and 51 |
| weston's desktop shell | no | not supported: use an X11 session |

Without the layer shell the daemon says so, names the protocol and ends with status 1,
also when `DISPLAY` is set: in such a session `DISPLAY` is Xwayland, and an X window is
not a desktop background there. The compositor shows the X11 backend's wallpaper window
above every other window instead of below, and clicks pass through it to windows nobody
can see (run on Mutter 46 and on KWin 5.27). Up to 0.14.0 `backend=auto` went on to X11
there by itself. X11 asked for by name (`--backend x11`, `walpe-ctl backend x11`) is
still taken and the log says what it means. On GNOME the answer is the extension
([GNOME](#gnome)): with it the daemon takes Wayland there and no layer shell is needed.
Its X11 session is the other answer where the distribution still offers one.

Used when the compositor offers them, not required: `xdg-output` (monitor names and
layout), `wp_viewporter` (pixel-exact rendering at fractional scales, render resolution
per monitor), `wp_fractional_scale_v1`, `wp_presentation`, `linux-dmabuf` (tells which GPU
the compositor uses). `walpe-ctl status` lists what was found (`wl_compositor_globals`).

Building the backend: see [BUILDING.md](BUILDING.md#build-options).

## Choosing the backend

Setting `backend`: `auto` (default), `x11` or `wayland`.

```sh
walpe-ctl backend                 # the backend running, why, the saved setting, what the
                                  # build holds, and who reports each pause condition
walpe-ctl backend wayland         # save the setting (= walpe-ctl set backend=wayland)
WALPE_BACKEND=x11 walpe-daemon    # for this start only; beats the saved setting
walpe-daemon --backend wayland    # for this start only; beats both
```

- `auto`: Wayland when `WAYLAND_DISPLAY` is set and the compositor offers the layer
  shell; X11 when `DISPLAY` is set and there is no Wayland compositor (or one that does
  not answer); otherwise the daemon ends with `walpe-daemon: no display to draw on: ...`
  and says what was missing. A Wayland compositor that answers and has no layer shell is
  such an end, whatever `DISPLAY` says (see above).
- `x11` / `wayland` by name: that backend or an error. No silent fallback to the other.
  `x11` inside a Wayland session draws through Xwayland, above the other windows.
- The setting is read when the daemon starts. Changing it does not move a running daemon:
  `walpe-ctl` answers `restart_needed` and the daemon keeps its backend until it is started
  again.
- A build with one backend says so (`walpe-ctl backend`: "built into this daemon: x11") and
  refuses the other with a sentence.

The first lines of the daemon's log say what was chosen and why (`display backend: wayland
(auto: WAYLAND_DISPLAY is set and the compositor offers the layer shell)`), and `walpe-ctl
status` carries the same as `display_backend`, `display_backend_why`,
`display_backend_setting`, `display_backend_source` and `display_backends_built`. (The
status member `backend` is older and means the video decoder backend.)

A compositor that accepts the connection and does not answer within 10 seconds counts as
absent: with `auto` the daemon goes on to X11 when `DISPLAY` is set and otherwise ends
saying that the compositor does not answer; with `wayland` by name it ends with status 1
and says so. A socket that refuses the connection is handled at once (error, or X11 with
`auto`).

## What is different from X11

| | X11 | Wayland |
|---|---|---|
| Pointer | followed everywhere on screen | known only while it is over the bare desktop of that monitor. When it moves onto a window, parallax keeps its last position, cursor scripts and web pages are told the pointer left |
| Clicks | reach the wallpaper on the bare desktop | a wallpaper that uses the pointer (parallax of a scene that reads it, cursor scripts, a web page) takes pointer input on its monitor, and then also the clicks: a compositor's own desktop menu does not get them while such a wallpaper is shown. Others take no input at all |
| Pointer image | the X server's, never set | set by the daemon while the pointer is over a wallpaper that takes pointer input (a surface has to; the image of the window the pointer came from would stay otherwise): the default arrow. The compositor draws its own where it takes a shape by name (`wp_cursor_shape_v1`: sway, KWin, labwc, Wayfire, river, niri, COSMIC do), so the arrow there is the one of the rest of the desktop. Elsewhere the daemon loads it from the theme and size in its environment (`XCURSOR_THEME`, `XCURSOR_SIZE`; the default theme at 24 without them, the output's whole-number scale), which matches the desktop only where the session exports the two to the daemon. `walpe-ctl status`, `wl_cursor`, says which. Over a wallpaper that takes no input the daemon sets nothing |
| Keyboard | never | never; no touch, no tablet |
| "Pause when covered" | from the X window tree: 85 % covered | the compositor stops asking a surface it does not show for frames (output off, lock screen, a fullscreen window in front where the compositor culls hidden surfaces); a window that covers only a part does not pause |
| "Skip hidden areas" | available | not available (no protocol gives window positions); the setting reads as off and the log says so |
| Scaling | - | rendered in the monitor's own pixels at every scale, also fractional ones (needs `wp_viewporter`; without it the next whole scale is rendered and the compositor scales down) |
| Rotation | - | an upright picture of the rotated size (1080 x 1920) |
| Frame pacing (`present` in `gpu.conf`) | as before | `vsync` (= `auto`): wait for the compositor's frame event after each frame; `limiter`: the frame clock alone. The frame rate limit per monitor works as on X11 |
| Monitor names | RandR output names (`HDMI-1`, NVIDIA: `DP-0`) | the compositor's (`HDMI-A-1`, `DP-1`) |
| See-through windows list | window classes (`WM_CLASS`) | application ids (`foot`, `org.wezfurlong.wezterm`) |
| X11 only | `WALPE_MONITORS`, `WALPE_ONE_XCONN` | |

`wayland_pointer=off` (`walpe-ctl set`) keeps the wallpaper from ever taking pointer input:
the desktop behaves as without WalPE, and wallpapers do not follow the pointer.

Seen on sway 1.9: a pointer is announced to a surface when it moves onto it, not when the
surface under a resting pointer begins to take input. A wallpaper that uses the pointer and
is loaded while the pointer rests on the bare desktop follows it from its first movement on.

Monitor names are the keys of per-monitor settings. Where X11 and Wayland only spell a
connector differently (`HDMI-1` / `HDMI-A-1`), status shows a `canon` name that is the same
for both. On Wayland an output without a stored setting of its own uses the one stored
under the other spelling of its connector (when that is the only one for the connector
and no connected output has that name); what is stored from then on is stored under the
output's own name. On X11 only the exact name counts, so a choice made on Wayland has to
be made once more there, and where the numbering differs (NVIDIA's X driver counts from
`DP-0`) a per-monitor wallpaper has to be chosen once more under the other backend. (The
name matching is tested with made-up names only: the headless outputs are called
`HEADLESS-n`.)

A monitor that is removed, comes back, moves in the layout or changes its mode, scale,
refresh rate or rotation makes the daemon restart itself in place, as on X11: two to ten
seconds, and every wallpaper on every monitor begins again.

**To try: `wayland_follow = on`** in `gpu.conf` ([GPU-PRESENT.md](GPU-PRESENT.md)), or
`WALPE_WAYLAND_FOLLOW=on` in the daemon's environment. It is off by default because it
has run on the software renderer only: on no GPU driver and no real monitor yet, and what
it does - new buffer sizes for a surface that is being drawn on, GL contexts kept for
monitors that are gone - is where drivers differ. With it the change is followed in
place, about two seconds after it has settled: nothing restarts, and the wallpapers of
the other monitors go on where they were:

- another place in the layout, another scale, refresh rate or a half turn: nothing is
  loaded or drawn anew, on any monitor;
- another size in pixels (a mode, a quarter turn, a scale where the compositor has no
  `wp_viewporter`): the surface stays and gets buffers of the new size. A video goes on
  playing; a scene on that monitor is loaded again, as it is when another render
  resolution is chosen for it;
- a monitor that is switched off, unplugged or put to sleep by a compositor that removes
  sleeping outputs: its wallpaper is unloaded, and its render thread waits. When it (or
  another one) appears, the wallpaper saved for it is loaded on a new surface. With no
  monitor left the daemon runs on with none; `walpe-ctl status` then lists no monitor and
  says how many render threads wait (`render_threads_waiting`).

Also with the setting on the daemon restarts in place when there are more monitors than
it has had at one time since it started (the first time an external monitor is plugged
into a laptop, say): a further monitor needs a further render thread with a GL context of
its own, and one is only made while the daemon starts. From then on that monitor comes
and goes in place. The log says at the start that the setting is on, and for each change
which of the two happened: `monitor layout changed (now: ...): followed in place (...)`
or `... restarting to follow it`. What to look at when trying it: a monitor switched off
and on, unplugged and plugged in, another resolution and scale chosen in the desktop's
settings, a rotation - each with a video on another monitor, which should play on.

### An output without a surface

The daemon asks the compositor for one wallpaper surface per monitor and waits three
seconds for each to be configured. A compositor that is not ready for that yet - river
0.4 and later before its window manager is connected, a session that is still starting -
leaves the monitor without one. The daemon then says `NO WALLPAPER SURFACE could be made`
in its log, runs on (its window and `walpe-ctl` work), and tries that monitor again:
after 2, 4, 8, 16, 32, 60, 60 and 60 seconds, each try waiting half a second for the
compositor, and at once whenever a monitor changes. The first try that is answered loads
the saved wallpaper. After the eighth (about four minutes) it says that it gives up on
that monitor and stays as it is until a monitor changes or it is started again.
`walpe-ctl status` (`monitors_without_surface`) and `walpe-ctl diag` say at any time
which monitor has no surface and where the tries stand. (Up to 0.14.0 there was no second
try.) This does not depend on `wayland_follow`: a try gives a surface to a render thread
that has waited for it since the start, and touches no monitor that shows something.
Without `wayland_follow` a monitor that changes meanwhile restarts the daemon, as any
change does, and the restart asks for every surface anew.

When the compositor goes away the daemon shuts down in order and exits with status 1, as it
does when an X server goes away. `wayland_reconnect = <seconds>` in `gpu.conf`
([GPU-PRESENT.md](GPU-PRESENT.md)) makes it restart in place instead and wait that long for
the compositor to come back: for its socket, and for its answer on a socket that is there
already (under `kwin_wayland_wrapper`, as a Plasma session runs KWin, the socket never goes
away: the wrapper holds it while `kwin_wayland` is started again). A compositor that comes
back and goes again while the daemon starts on it is waited for again, the whole time anew.
Not without end: a compositor that ends the daemon's connection every time it is made is
given up. A *stay* of the display is the time from the daemon's connection being answered
to its loss; the fifth stay in a row of less than 30 seconds ends the daemon with status 1
and one warning that names the setting, the way `wayland_reconnect = 0` ends it at the
first loss. That is four restarts in place in a row at most; one stay of 30 seconds or
more and the count begins anew, so a compositor that crashes three times within a minute
is waited for each time, and so is one that crashes every now and then for as long as the
session lasts. And a connection the compositor ended with a protocol error - it objected
to something the daemon sent - is not come back to at all: the same request would follow. (The compositor's sentence is in the log, in a
line of libwayland's and in the daemon's warning; please report it.) The two numbers are
fixed. The reason for them: under `kwin_wayland_wrapper` a Plasma session is over after
about ten crashes of KWin, and a daemon that came back each time to a KWin that dies of
something the daemon sends would use them up. Up to here the daemon came back every time
(the last rows of the table under "Compositors that were run").
The wallpapers begin anew then: the daemon is the same process, but nothing of what it
showed is kept. While it waits, `walpe-ctl` reports that the daemon
did not answer in time, and what was sent to it meanwhile is not carried out when it is back;
a signal that ends it (the session closes, `systemctl --user stop`) ends it at once.
The wait is for a compositor that is started again in the same session. At a logout the
daemon would wait as well, and can still be there, with the old session's environment, when
the next login starts its own: one of the two then ends ("another walpe-daemon is already
running") - keep the time short where the setting is used.
A session started with a one-time socket (`WAYLAND_SOCKET` instead of
`WAYLAND_DISPLAY`) cannot be reconnected to. Known gap: for the same reason the daemon
cannot restart itself in place there. Where it has to restart for a monitor (any change
of one; with `wayland_follow = on` only more monitors than it has had at one time) it ends with
status 1 and `no display to draw on` (with `backend=auto` and `DISPLAY` set it comes back
on X11 instead); start it again from the compositor.

## Pause rules

Each pause rule needs something in the session that reports its condition. `walpe-ctl
backend` and the status member `pause_rules` list every rule with its source; `"works":
false` means the setting has no effect in this session, whether it is on or off, and
`note` says why. The settings window shows the same reason under the row.

| Rule (config key) | Source on Wayland |
|---|---|
| Pause while the display sleeps (`pause_dpms`) | logind (systemd-logind or elogind, system bus): the machine is about to suspend, the session is locked (desktops and lockers that set the session's "locked hint"), or the session is not the active one |
| Pause for fullscreen applications (`pause_fullscreen`) | the compositor's window list (`zwlr_foreign_toplevel_manager_v1`, version 2 or later): the focused window is fullscreen. The monitor that pauses is the output the compositor names for that window, matched to the daemon's monitors by the output's name (`wl_output` version 4), or the only monitor there is. Where that cannot be told - the compositor names no output for the window, or its outputs have no names and there are several monitors - every monitor pauses, as up to 0.12, and `note` says so. A window that has just been mapped and has no output yet is on no monitor until the compositor names one (sway names it a moment after the window's state), unless the compositor has not named an output for any window so far |
| Pause a covered monitor (`pause_covered`) | (a) the compositor no longer shows the wallpaper on that monitor: every compositor; (b) the window list: the focused window on that monitor is fullscreen or maximized. There is no "85 % covered" as on X11 |
| While an application is focused (`focus_action`) | the window list |
| Pause after N seconds without input (`pause_idle`, new, off by default) | the compositor's idle notification (`ext-idle-notify-v1`). `walpe-ctl set idle=300`; 0 = off. On X11 nothing reports idle time in this version, and status says so |
| Pause on battery (`pause_battery`) | the kernel's power-supply state, as on X11 |

KWin and GNOME Shell offer other programs no window list. On GNOME the WalPE extension,
which the wallpaper needs there anyway, tells the daemon instead: fullscreen, focus, and
covered at 85 % as on X11, per monitor ([GNOME](#gnome); the source is named `gnome` in
`pause_rules`). COSMIC's compositor lists windows without their state
(`ext_foreign_toplevel_list_v1`; the state is in a protocol of its own,
`zcosmic_toplevel_info_v1`, which WalPE does not speak), so the fullscreen and focus
rules have no source there, and of the covered rule the "no longer shown" part is left;
`pause_rules` says so in the rule's `note`, and the settings window prints it under the
switch. A fullscreen window pauses the wallpaper on COSMIC all the same, through "pause
when covered": cosmic-comp 1.11.0 stops asking a surface behind a fullscreen window for
frames (run headless).

**On KDE Plasma the three rules work through a script in KWin.** KWin goes on asking a
wallpaper surface for frames while a window hides it (frame callbacks and presentation
feedback keep coming at full rate behind a fullscreen window: run on KWin 5.27 and 6.8,
and seen in a report from a real Plasma 6.8 session), so "no longer shown" never happens
there behind a window. What KWin does have is a script interface. While one of the rules
`pause_fullscreen`, `pause_covered` or `focus_action` is on, the daemon loads a small
script into KWin over the session bus (`org.kde.kwin.Scripting`; nothing is installed,
no permission is asked for) and the script calls the daemon back when an answer changes:

| | |
|---|---|
| `pause_fullscreen` | the active window is fullscreen; the monitor is the output KWin names for it (`pause_fullscreen_all`: every monitor) |
| `pause_covered` | as on X11: the application windows shown on the output (not minimized, on the current virtual desktop and activity, not in the see-through list) hide at least 85 % of it. A maximized window does, two windows side by side do |
| `focus_action` | the active window is an application's and is on that output |

What it costs and when: with all three rules off there is no connection to the session
bus and no script. With one on, the daemon holds one more connection (it owns no name on
the bus) and KWin runs the script when a window comes, goes, moves or changes state; the
daemon hears from it only when one of the answers changes. The script is taken out of
KWin when the last rule is switched off and when the daemon exits, and loaded again when
KWin was restarted. A daemon that is killed or crashes cannot take it out: the script
stays in KWin and goes on reporting to a connection that is gone (the bus answers that
nobody is there)
until the next daemon starts, which removes it before it loads its own, or until KWin
ends with the session. The status member `pause_kwin_script` says what the provider is
doing, and why when it cannot report (no session bus, KWin not on it, a KWin older than
Plasma 6: the rules then read `"works": false` with that reason). `WALPE_KWIN_SCRIPT=0`
keeps the daemon from using it.

Unlike X11, windows that are in none of task bar, pager and window switcher (a
drop-down terminal, panels of other programs) do not count as covering, and a window's
opacity is not looked at. Run on a headless Plasma 6.8.2 with one and two outputs
(fullscreen on one output pauses that one and, with `pause_fullscreen_all`, both;
maximized and nearly-covering windows; minimized; the focus rule; the see-through list;
rules switched off and on with a fullscreen window in place) and with a stand-in KWin
(test `sig-kwin`); not on Plasma 5.27, where the script only says that it is too old.
The test `daemon-kwin` repeats the fullscreen, maximized and half-covering windows and
the script's coming and going on a real KWin, where one is installed
([DEVELOPMENT.md](DEVELOPMENT.md#tests)).

`walpe-ctl pause` / `resume` and the tray's pause entry work everywhere.

Lockers that do not tell logind that the session is locked (seen with a lock client on
sway): the wallpaper pauses through the covered rule, because the compositor stops showing
it, not through the display-sleep rule.

The rules that ask the compositor (idle time, window list) use a connection of their own.
Its first answers are waited for 5 seconds at most when the daemon starts; a compositor
that stays silent leaves those rules without a source, and each of them says in
`pause_rules` that the compositor did not answer.

Tested with stand-ins only: logind (a stand-in login manager on a private bus; no system
bus was available), the idle and window-list protocols (a stand-in compositor in the test
`sig-wl`). KWin's script: a stand-in and the real KWin 6.8.2, as said above.

Switches in the daemon's environment: `WALPE_LOGIND`, `WALPE_WL_COVERED`,
`WALPE_KWIN_SCRIPT` ([ENVIRONMENT.md](ENVIRONMENT.md)).

## Web wallpapers

A web wallpaper is a page in WebKitGTK whose picture the engine captures and draws. WebKit
only runs a page in a mapped window that must not be seen; where that window lives is the
"page host". The web module holds the ones its build found the parts for:

- `x11`: an X window off screen. The default wherever an X display answers: on X11
  sessions as always, on Wayland sessions through Xwayland. Built when libX11 and GTK's
  X11 backend are found and the build was not given `-Dx11=disabled`.
- `private`: a Wayland display inside `walpe-daemon` to which only the page's toolkit is
  connected. It needs no X, neither when WalPE is built nor when it runs: a system
  without libX11, or a build with `-Dx11=disabled`, has web wallpapers through it. Used
  when there is no X display, when the X display does not answer, or when asked for with
  `WALPE_WEB_HOST=private`. Built with the Wayland backend when `wayland-server` and GTK's
  Wayland backend are found; not with `-Dwayland=disabled`.

`WALPE_WEB_HOST=auto|x11|private` in the daemon's environment; `walpe-ctl status` shows
`web_host` once a page was created.

With `auto` on a Wayland session a host that does not come up gives way to the other one,
where the build has both. `DISPLAY` set with no X server behind it (a variable left over
in a session without Xwayland) used to switch web wallpapers off; now the log says `web:
the X11 page host did not come up (...): using the private page host instead` and status
has `"web_host": "private (the X11 page host did not come up)"`. The same happens when
WebKit fails its start-up test on the X window, and the other way round (`x11 (the
private page host failed its start-up test)`). The second host is tested like the first;
when both fail, web wallpapers are off and `web_error` says why. A host asked for by name
is never replaced, and on an X11 session nothing of this applies: there the page host is
`x11` and the private one is not started.

> **State of testing of the private page host.** It was run with real pages, on one
> machine and one compositor: a headless sway 1.9 (wlroots 0.17, pixman renderer) with
> `xwayland disable`, no `DISPLAY`, in namespaces of its own without any X socket or X
> server process, Mesa's software renderer, WebKitGTK 2.52 on GTK 4.14, the build made
> without libX11. The test `web-wayland` (`scripts/webcheck.py`) does it again on any
> machine with sway and grim. What it showed there: generated pages with a
> `requestAnimationFrame` animation, a 2D canvas, a WebGL canvas, a CSS animation, the
> Wallpaper Engine property and audio listeners and pointer input, drawn right on one
> output and on two outputs of different sizes (one page for both, and a page of its own
> on one of them); an output that is added, changes its mode and its scale and is
> unplugged under the running daemon; pause and resume (while paused the page's processes
> and the daemon used 0 to 1 hundredths of a second of CPU time in 4 seconds, as with the
> X11 page host); the daemon ended and started again, and restarted in place; the page's
> process killed (loaded again three times, given up at the fourth, afresh when the
> wallpaper is opened again); `DISPLAY` left over without an X server; nothing left
> behind after `walpe-ctl quit` (no process, no socket, no directory); popups, choosers,
> dialogs, a new window and a link that leads away, none of which opens anything; the
> session's clipboard and primary selection, which a page that copies on every click
> neither reads nor replaces; network access off. The picture of a still page is the same
> as with the X11 page host, byte for byte (compared on Xvfb, through Xwayland on sway,
> and on sway without X). Web hardware acceleration was tried only with the software
> renderer standing in for a GPU. **Not run: a real desktop session, a real output, a
> GPU, any compositor but sway, a desktop's settings service (font scaling, dark
> preference).**

What differs from the X11 page host:

- **Frame rate above 60.** WebKit times a page's frames by its own 60 Hz timer unless it
  finds the monitor the page's window is on among the kernel's display connectors, and
  the private display's one output is none of them. A page there is expected to animate
  at 60 frames a second at most, whatever the frame rate limit and the monitor say (read
  from WebKit's source; not measured, the test machine has no display connector).
- **What it costs.** With a page on it the daemon has one more thread for the display
  (it sleeps while nothing happens: no wake-up in 10 seconds with a still page) and no
  file or directory of its own once the page is up. WebKit sets up OpenGL for the display
  its toolkit is on in either case; on the private display Mesa can only give it the
  software renderer, which is one idle thread per CPU and, measured on the software
  renderer, about 10 to 15 MB more memory in the daemon than with the X11 page host
  (204 to 209 MB against 194 MB, with one 1280 x 720 page).
- **The page's process and the session.** WebKit runs each page in a sandbox and hands it
  the socket of the display its toolkit is on: with the X11 page host the X server's.
  With the private page host that would have been the compositor's (WebKit looks for
  `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`), which no page needs; the daemon keeps WebKit from
  finding it, and the page's process has no display socket at all there
  ([SECURITY.md](SECURITY.md)).
- **If the private display fails**, GTK ends the process it runs in, and that is the
  daemon. This did not happen in any run; the display has no way of its own to drop its
  one client.
- The pointer and the wheel reach a page as real events on both; the keyboard never
  reaches a page on either.

A page cannot read, replace or clear what you copied or selected, on either host
([SECURITY.md](SECURITY.md)). The middle mouse button never reaches a page as a real click.

## Tray icon and window

- **Tray** (`walpe-tray`): a Wayland session always uses StatusNotifierItem (D-Bus), which
  is what Wayland panels speak; XEmbed is an X11 protocol. The icon appears as soon as a
  panel with a tray is there. Panels: waybar needs its `tray` module, swaybar a
  `tray_output` other than `none`, xfce4-panel its "Status Tray" plugin, GNOME Shell the
  extension "AppIndicator and KStatusNotifierItem Support"; KDE Plasma needs nothing
  (waybar 0.16 was run on sway, labwc, Wayfire, river and niri: the icon is shown, a right
  click opens the menu, "Pause all wallpapers" pauses; the others are expected). For a panel that only reports the click, and for
  `walpe-tray --menu`, the tray opens its own menu as a small window the compositor places.
  `walpe-tray --backend=sni|xembed`, `WALPE_TRAY_BACKEND` or `backend=` in `tray.ini` force
  a protocol.
- **Window** (`walpe`): uses no X11 call and behaves the same on both. Its icon and name in
  task bars come from the desktop entry `io.github.walpe.ui.desktop`. On GTK older than
  4.20 it follows the desktop's dark / light preference through the settings portal
  (`xdg-desktop-portal`); `GTK_THEME` still wins.

## Starting with the session

XDG autostart entries (the installed `walpe-autostart.desktop`, `walpe-tray.desktop`) are
run by desktops such as GNOME, KDE and Xfce. sway, Hyprland, river, niri, Wayfire and
labwc do not run them by themselves; start the engine from the compositor's configuration
(sway: `exec walpe-daemon`, `exec walpe-tray`; Hyprland: `exec-once = ...`; niri:
`spawn-at-startup "walpe-daemon"`; labwc: a line in `~/.config/labwc/autostart`;
Wayfire: its `autostart` plugin; river: its `init` script, from 0.4 on best after the
window manager, see [below](#compositors-that-were-run)). With the systemd user unit
(`data/walpe.service`) the user manager must know the display: compositors started outside
systemd need, once per session, `systemctl --user import-environment WAYLAND_DISPLAY
XDG_CURRENT_DESKTOP`.

## KDE Plasma

Plasma's desktop is not empty space behind the windows. It is a window of Plasma's shell
that fills each monitor and holds Plasma's own wallpaper, the desktop icons and the
widgets in one opaque picture. KWin, Plasma's compositor, keeps that window in the
stacking layer it also uses for every surface on the layer shell's background layer, and
orders the windows of that layer as it orders ordinary windows: the one clicked last is
in front.

WalPE 0.14.0 put its wallpapers on the background layer. On Plasma that gave two
pictures. Started while an application's window was the active one, the wallpaper lay in
front of Plasma's desktop and covered its icons until the first click on the desktop:
KWin then raised Plasma's desktop over the wallpaper, and the video was gone. Started
while Plasma's desktop itself was the active window, the wallpaper was behind it from the
start and never seen (Plasma 6). Either way the daemon went on running and drawing, with
nothing in its log or status: KWin goes on asking a surface it has covered for frames.

From this version on the daemon puts its wallpapers on the layer shell's **bottom** layer
when the compositor is KWin (told by the protocols `org_kde_plasma_shell` or
`org_kde_plasma_virtual_desktop_management`, which only KWin offers). KWin stacks that
layer above Plasma's desktop and below every window, so an opaque desktop can never be in
front of the wallpaper, whatever is clicked. There are two ways to live with that; the
daemon is the same in both and its surface stays where it is.

### Without the add-on: the wallpaper in front of Plasma's desktop

- **Plasma's desktop icons and widgets cannot be seen while WalPE shows a wallpaper.**
  They are part of the window the wallpaper lies on. They are back when the daemon is
  quit, and for as long as "Peek at Desktop" (Meta+D) lasts: KWin then hides the
  wallpaper with the windows and shows Plasma's own desktop.
- Clicks on the desktop go on reaching Plasma as long as the wallpaper does not use the
  pointer (a video and a picture never do, see "Clicks" in the table above): its
  right-click menu opens, and icons that cannot be seen still take clicks. If that is in
  the way, Plasma's desktop settings ("Desktop and Wallpaper" in the desktop's menu)
  have the layout "Desktop", which shows no icons, next to "Folder View".
- A wallpaper that uses the pointer gets it, clicks included, as on other compositors.
- The Overview shows Plasma's own desktop picture as the background of the desktop, not
  the WalPE wallpaper.

### With the add-on: the wallpaper behind Plasma's icons

[`contrib/plasma`](../contrib/plasma/README.md) holds an add-on for Plasma 6.5 or newer:
a wallpaper type "WalPE" for Plasma's "Desktop and Wallpaper" dialog. It is not part of
the build and not installed by `meson install`; a Plasma user installs it for himself, no
root needed:

    contrib/plasma/install.sh install     # from WalPE's source tree
    contrib/plasma/install.sh select      # or: Desktop and Wallpaper > Wallpaper type: WalPE

The type draws no wallpaper. It makes Plasma's desktop window see-through and loads a
small script into KWin that keeps this window right above the daemon's surface on the
same screen. The WalPE wallpaper is then seen behind Plasma's icons and widgets, which
take clicks as always. What a desktop of that type shows:

| | |
|---|---|
| the daemon shows a wallpaper on the screen | the wallpaper, Plasma's icons and widgets over it |
| the daemon is not running (not started yet, quit, crashed) or shows nothing there | the colour or picture set on the type's page of the dialog, icons over it |
| "Peek at Desktop" | the same colour or picture, icons over it (KWin hides the wallpaper) |
| the Overview | the desktop's icons on the Overview's own backdrop; the small pictures of the virtual desktops show the wallpaper |
| another wallpaper type is chosen for the desktop | as without the add-on, at once |
| the type on one of two screens | that screen: icons over the wallpaper; the other: the wallpaper in front |

The parts cannot leave a wallpaper that runs unseen or a black desktop: the daemon's
surface is in front of every desktop window except one the script has verified, with
Plasma's shell, to be of the type "WalPE"; and such a desktop stays filled until the
script has told it that a surface of the daemon lies under it. How:
[contrib/plasma/README.md](../contrib/plasma/README.md).

**The pointer with the add-on.** Plasma's desktop window is above the wallpaper and
KWin gives it every pointer event; the daemon's surface gets none there. The daemon
gets the pointer from KWin and from the wallpaper type instead, and only while a wallpaper of
the daemon uses the pointer (the daemon then has the name `io.github.walpe.Pointer` on
the session bus, and has no name there at any other time, and a script of its own in
KWin that tells it where the pointer is):

| | |
|---|---|
| the position | everywhere on the desktop, also over icons and widgets; at most once per frame of the wallpaper. It comes from a second script the daemon loads into KWin for as long as a wallpaper wants the pointer (KWin knows where the pointer is; asking Plasma's window for it took the hover from Plasma's icons). Scenes that follow the pointer and pages get it as from a pointer of their own |
| the left button, the wheel | over the bare desktop. Over an icon or a widget they are Plasma's alone |
| the right and the middle button | never: they are Plasma's (the desktop's menu, paste) |
| the pointer leaves the desktop (onto a window, the panel, a menu) | the wallpaper is told that it left |

Plasma goes on getting everything as before, the hover of its icons and widgets
included: a click on the bare desktop also takes the selection off Plasma's icons, and
a drag there also draws Plasma's selection frame over the wallpaper. A web wallpaper that needs the right button, or a drag without that
frame, wants a desktop with another wallpaper type (it is then in front and has the
pointer to itself). `wayland_pointer` = off switches this off with the rest of the
pointer. If the add-on is installed while a wallpaper that uses the pointer is shown,
the pointer arrives once that wallpaper is opened again. The type sends the pointer only
to the process whose surface KWin sees under the desktop, and the daemon takes it only
from the connection that has the name of Plasma's shell; what a name on the session bus
is and is not worth is in [contrib/plasma/README.md](../contrib/plasma/README.md#the-pointer).

### What the daemon says

The log has two lines starting with `wayland: KDE Plasma` when the daemon starts: the
layer, and, unless the add-on is in use, that Plasma's icons are hidden and which command
brings them back. `walpe-ctl status` has

| | |
|---|---|
| `wl_layer`, `wl_layer_why` | `bottom`, `the compositor is KWin (KDE Plasma)` |
| `wl_plasma_addon` | `not installed`, `installed, no desktop has the wallpaper type WalPE` or `in use: a desktop has the wallpaper type WalPE` |
| `wl_plasma_icons` | `hidden behind the wallpaper` or `shown over the wallpaper where the wallpaper type is WalPE` |
| `wl_plasma_hint` | only while the icons are hidden: the sentence of the log |
| `wl_plasma_pointer` | only while a wallpaper uses the pointer and the add-on is installed: that the pointer is taken from the add-on, or why not, and whether its position comes from the pointer script in KWin |

The daemon reads the first three from the installed package and from the settings file
of Plasma's desktops, which Plasma writes a few seconds after a change; the
`wl_plasma_*` members are there on KWin only. The pointer is the one thing the add-on
tells the daemon; the daemon tells the add-on nothing.

`WALPE_WL_LAYER=background` in the daemon's environment brings back the placement of
0.14.0, `WALPE_WL_LAYER=bottom` asks for the bottom layer on any compositor
([ENVIRONMENT.md](ENVIRONMENT.md)). On the background layer, a desktop without the
wallpaper type "WalPE" is in front of the wallpaper or behind it depending on which
window was active when the daemon started, and a click on the desktop hides the
wallpaper; the add-on works there as on the bottom layer.

> **State of testing on Plasma.** A headless Plasma 6.8.2 (KWin 6.8.2 on its virtual
> outputs, Qt 6.12; the bottom layer also on Plasma 5.27 with KWin 5.27.11), one and two
> 1280 x 800 outputs, Mesa's software renderer, a video wallpaper, clicks from a virtual
> pointer. Without the add-on: the wallpaper is in front of Plasma's desktop from the
> start and stays there through left clicks and a forced raise of the desktop window; a
> right click opens Plasma's desktop menu over it. With the add-on: the type chosen
> before the daemon starts, while it runs, and taken away while it runs; the daemon quit,
> killed and started again; Plasma's shell killed and started again; a forced raise of
> either window; the type on one of two screens; "Peek at Desktop"; the Overview; the
> type chosen in the "Desktop and Wallpaper" dialog (there the Apply button stayed grey
> after leaving the type "Image", for Plasma's own "Plain Color" as well; OK applies).
> In each of these the wallpaper was seen, or the colour of the type's page where the
> daemon showed nothing, and never a black desktop; the icons and the desktop menu took
> clicks. The pointer with the add-on: a web page that follows the pointer and shows
> presses, releases and the wheel got the position in page coordinates, the left button
> (press, drag, release) and the wheel over the bare desktop, and neither over an icon;
> the right click opened Plasma's menu and did not reach the page. Not run: a scene that
> uses the pointer with the add-on (it reads the same position), a touchpad, a touch
> screen, a real mouse wheel. In a second pass, with KWin's permission checks for
> clients on (the runs above had them off, for the virtual pointer): KWin is still told
> by its protocols and both scripts load; two activities with the type on one desktop
> (the wallpaper is in front in the other one, and the icons are over it again on the
> way back); two outputs at the scales 1.5 and 1.25 side by side (the page got the
> pointer at the right place on each); an output switched off and on; the type chosen
> and taken away a hundred times in a row (nothing grew in KWin or in Plasma's shell,
> no script left behind); the panel in each of its visibility modes (its window stays in
> a layer above the wallpaper's); the pointer's position from the daemon's script in KWin,
> on one output and on two (the frame around the icon under the pointer is there, the
> page has the pointer where it is, clicks and the wheel arrive from the wallpaper type,
> a popup of Plasma's over the desktop and the panel count as "left"). Not run at all: a real Plasma session, a GPU, NVIDIA's driver, a restart
> of KWin under the running shell, Plasma versions between 6.5 and 6.8. What can be
> repeated without a person - the stacking before and after a click in both starting
> orders, the add-on installed, selected, unselected and removed, the pointer through the
> add-on, the desktop after the daemon quit or was killed - is the test `daemon-kwin`
> ([DEVELOPMENT.md](DEVELOPMENT.md#tests)), which is skipped where no KWin of Plasma 6 is
> installed.

## GNOME

GNOME's compositor, Mutter, has no layer shell and nothing in its place: no protocol with
which a program can put a surface on the desktop. What Mutter has is a window type,
DESKTOP, that only GNOME Shell itself can give a window. WalPE therefore comes with an
extension for GNOME Shell, [contrib/gnome](../contrib/gnome/README.md). It is optional and
no part of the build: nothing of it is compiled or installed with WalPE, a GNOME user
installs it for himself, and on every other desktop the daemon does not ask for it.

    contrib/gnome/install.sh install      # then log out and in
    contrib/gnome/install.sh status

**With the extension** `walpe-daemon` starts on GNOME's Wayland session as anywhere else.
The start-up log says "GNOME Shell 46.0 with the WalPE extension ...", and `walpe-ctl
status` has `wl_gnome_extension` and `wl_gnome_connection`. Each wallpaper is an ordinary
window of the daemon, one per monitor, that the extension makes a desktop window:

- below every window, on all workspaces, not in the overview's window list, the window
  switcher or the dash; behind the windows while the workspace is changed;
- below the icons of "Desktop Icons NG" (Ubuntu's desktop icons), whichever starts first;
- behind the workspaces in the overview, with square corners (GNOME's own background has
  rounded ones there); on GNOME Shell 46 it stands still while the overview is open, on 51
  it runs;
- the pause rules for fullscreen windows, covered monitors and the focused application
  work, per monitor, from what the extension reports; a locked screen counts as covered.
  The lock screen itself shows GNOME's own picture;
- a wallpaper that uses the pointer (web wallpapers, scenes that follow it) gets motion
  and clicks like any window, where no other window is above it. "Desktop Icons NG" is
  such a window over the whole monitor: with it the pointer does not reach the wallpaper.
  A wallpaper that does not use the pointer lets clicks through to GNOME's desktop.

**Without it**, or with it switched off, the daemon ends with status 1 as on any compositor
without the layer shell, and the sentence names the extension and the command that
installs or enables it. An extension that is installed and not loaded yet (GNOME Shell
reads extensions when it starts) is waited for up to three seconds. Switching the
extension off while the daemon runs leaves the wallpaper where it is; the overview and
the pause rules for windows go without it until it is back. `WALPE_GNOME_EXT=0` keeps the
daemon from asking.

**GNOME Shell 46 to 48** can retype only the windows of a program the shell started
itself. There the daemon asks the extension for its connection to the compositor, and the
shell starts `walpe-daemon --wayland-socket-to 4`, a second process of the same program
that hands the connection over and then only waits (2 MB of its own) until the daemon
ends. From 49 on the shell retypes any window and there is no second process.

**Which GNOME needs this.** GNOME on X11 does not: there WalPE's X11 backend runs as it
always did. But the X11 session is going: Mutter 49 is built without its X11 backend by
default ("Disable X11 backend by default", Mutter's NEWS for 49.alpha.0) and Mutter 50
has none ("Drop the X11 backend", 50.alpha), so GNOME 49 has an X11 session only where a distribution builds it back in, and
from GNOME 50 on there is only the Wayland session. Xwayland is not affected by that, and
is no way out: an X11 window there lies above the other windows.

> Run on a headless GNOME Shell 46.0 (Mutter 46.2, Ubuntu 24.04; one and two monitors;
> with and without "Desktop Icons NG" 47.0.9) and 51.1 (Mutter 51.1, Arch Linux), software
> renderer. Not run: a real GNOME session with gnome-session, GDM and logind, a GPU,
> NVIDIA's driver, GNOME Shell 47 to 50 (the extension lists the versions it was run on,
> and GNOME Shell loads it on those only: [contrib/gnome](../contrib/gnome/README.md#versions)),
> a monitor plugged in or pulled while the daemon runs. On 46.0 two monitors changed
> places and one changed its scale to 2 and to 1.5 under the running daemon, which was
> not run on 51.1.

## Compositors that were run

All of this is from one day, 2026-11-12, on one machine without a graphics device: each
compositor headless, drawing with Mesa's software renderer or wlroots' pixman renderer,
from Arch Linux's packages (GTK 4.24, Mesa 26.2), with a picture, a generated video and
generated scenes. "The checks" are `scripts/waylandcheck.py --display <compositor>`
([DEVELOPMENT.md](DEVELOPMENT.md)): start, layer, two outputs, a mode / scale / transform
that changes, outputs that come and go, a pointer device, an output switched off, a
fullscreen window, a compositor without layer shell, a compositor that goes away (the
case that compares the Wayland backend with the X11 one was not run there: that
installation had no Xvfb). By
hand, where a row does not say otherwise: the tray on waybar, the library window, clicks
on the desktop, the daemon's CPU time with a still picture and with the output off, the
compositor's "reload configuration" where it has one, and the compositor killed and
started again with and without `wayland_reconnect`.

| Compositor | How it was run | What was seen |
|---|---|---|
| sway 1.12 | wlroots' headless backend | all checks pass, as on 1.9. `swaymsg reload` leaves the daemon alone |
| labwc 0.20.2 | wlroots' headless backend | all checks pass. A right click on the desktop opens labwc's menu over a wallpaper that does not use the pointer, and not over one that does (as the table above says). `SIGHUP` (reconfigure) leaves the daemon alone. labwc keeps a window on its output when the window list asks for fullscreen on another one |
| Wayfire 0.11.0 | wlroots' headless backend, as a user that is not root | all checks pass. A rewritten `wayfire.ini` (it reloads by itself) leaves the daemon alone |
| river 0.3.17 (river-classic) | wlroots' headless backend | all checks pass |
| river 0.5.0 | wlroots' headless backend, with a 150-line stand-in for a window manager | see "river" below; only the wallpaper, a window above it and a fullscreen window were looked at |
| niri 26.04 | as a window on a headless sway (its only way to run without a seat) | wallpaper shown, below windows; a fullscreen window pauses it (niri's window list names state and output); the pointer is followed; scale 1.5 and 2; tray on waybar, which niri's default configuration starts; configuration reload; restart with `wayland_reconnect`. Not possible that way: more than one output, an output that is off, rotation. The checks were not run (they need outputs that can be made) |
| cosmic-comp 1.11.0 | as a window on a headless sway | wallpaper shown, below windows; a fullscreen window pauses it through "pause when covered" only; the pointer is followed; scale 1.5 and 2; waybar shows the tray icon (its menu did not open on a simulated click and was not followed up); restart with `wayland_reconnect`. Not possible that way: more than one output, an output that is off. The checks were not run; COSMIC's own panel and its wallpaper program were not installed |
| Hyprland 0.57.2 | did not start | its backend library ends with "Cannot open backend: no allocator available": it needs a graphics device (DRM) also for a headless or nested session, and the test machine has none |

On sway, labwc, Wayfire, river 0.3, niri and cosmic-comp: the wallpaper is on the
background layer; with a still picture the daemon used no CPU time that could be measured
(0 hundredths of a second in 10 seconds), and the same with the output off where an
output can be switched off (sway, labwc, Wayfire, river); the daemon ends in order when
the compositor is killed, and comes back with its wallpaper when `wayland_reconnect` is
set and the compositor is started again on the same socket; nothing restarts the daemon
when the compositor reloads its configuration (sway, labwc, Wayfire, niri).

Three faults showed there and are repaired in this version: a still wallpaper kept its
old size for seconds when an output's scale changed (seen on niri, cosmic-comp and sway);
a daemon waiting with `wayland_reconnect` gave up at once when the old compositor took
one more connection on its way out (cosmic-comp); and the library window said another
application id than its dialogs in a session without a D-Bus session bus.

What a compositor that is started again does to the daemon, measured on KWin 6.8.2 under
`kwin_wayland_wrapper` (two virtual outputs) and on sway 1.9 and 1.12:

| what happened | `wayland_reconnect = 0` (default) | `wayland_reconnect = 60` |
|---|---|---|
| `kwin_wayland --replace` | the daemon ends 0.7 s later with status 1; KWin is back, the wallpaper is not, until something starts the daemon again (`walpe.service` does after 3 s) | the wallpaper is back half a second after the new KWin is up, from its beginning; same process id |
| `kwin_wayland` crashes (SIGSEGV), the wrapper starts it again | the same, 0.5 s | the same |
| the new `kwin_wayland` needs 13 s before it answers | the same | the wallpaper is back when it answers (the daemon used to give up after 10 s) |
| sway is told to end, or killed, and started again 2 s later | ends with status 1 | back with its wallpaper as soon as sway is up |
| the compositor comes back and is killed again within a second | - | the daemon waits again, at whatever moment of its start that catches it (it used to end when that caught it setting up EGL); the fifth time in a row it ends (next row) |
| a stand-in socket in front of sway that ends every connection 3 s after it was made | ends with status 1 at the first | four restarts in place, then the fifth short stay ends the daemon with status 1, 15 s after the first loss, nothing left behind (it used to restart every 3 s for as long as that went on: 29 times in 90 s) |
| the same socket ends the daemon's connection with a protocol error | ends with status 1 | ends with status 1 at once and makes no new connection (it used to restart in place and come back) |

## Notes per compositor

- **sway**: remove `output * bg ...` from its configuration (or stop swaybg): that is
  another background surface, and the one created last lies on top. The layer namespace of
  WalPE's surfaces is `walpe`.
- **river**: from 0.4 on river has no window management of its own; a separate program
  does it, and river shows a layer-shell surface only once a window manager that
  supports the layer shell (`river_layer_shell_v1`) is connected. Run on 0.5.0: with no
  window manager, or with one that does not bind that protocol, the wallpaper's surface is
  never configured; the daemon says `NO WALLPAPER SURFACE` after three seconds, keeps
  running and tries again (see [An output without a surface](#an-output-without-a-surface)):
  started 18 seconds before a stand-in window manager, it showed the wallpaper at its
  next try, 30 seconds after its start; up to 0.14.0 it never tried again. Starting
  `walpe-daemon` after the window manager (in river's `init`: the window manager first,
  then `walpe-daemon`) saves the wait.
  With a stand-in window manager that supports the layer shell the wallpaper was shown
  below windows, and a fullscreen window paused it through "pause when covered"; river's
  window list named neither the window's output nor its fullscreen state there, which may
  be the stand-in's doing. No real window manager for river 0.5 was run. river up to 0.3
  ("river-classic") needs none of this.
- **niri**: without further configuration the wallpaper belongs to each workspace and is
  scaled down with it in the overview, on niri's plain backdrop. niri's layer rule
  `place-within-backdrop true` for `namespace="^walpe$"` puts it on the backdrop instead;
  then the workspaces' own background hides it outside the overview unless that is made
  transparent in niri's configuration (seen that far; a transparent workspace background
  was not tried).
- **labwc**: its desktop menu (right click) goes on working over wallpapers that do not
  use the pointer; `wayland_pointer=off` keeps it for all wallpapers.
- **Hyprland** (expected, not run): do not run hyprpaper / swww at the same time.
- **KDE Plasma**: see [KDE Plasma](#kde-plasma) above.
- **GNOME**: see [GNOME](#gnome) above.
- **COSMIC**: its own wallpaper program is a background-layer surface too (expected: it
  was not installed in the run above).
- **Desktop-icon programs** on a layer above the background stay visible, and hide the
  wallpaper where they paint an opaque background.
- **NVIDIA's driver** needs its Wayland EGL library (`egl-wayland`).

## Limits

- Nothing above has been confirmed on a real compositor session, a real output or a GPU.
  That includes web wallpapers on the private page host, which were run on sway only.
- An output that gets no surface is tried again for about four minutes and then given
  up until an output changes ([An output without a surface](#an-output-without-a-surface)).
  Not seen at all: `linux-dmabuf`, a hardware EGL on Wayland, connector names of real
  outputs, DPMS on a real connector, presentation feedback with a real refresh period.
- 8 bits per channel, sRGB; no HDR, no colour management protocol.
- Four things the headless tests showed that are left as they are:
  - A hidden wallpaper with "pause when covered" switched off goes on being drawn at its
    frame rate limit, although the compositor discards every frame. It costs what a shown
    wallpaper costs.
  - A lock screen pauses the wallpaper through "pause when covered", not through "pause
    while the display sleeps", unless the locker tells logind that the session is locked:
    with `pause_covered` off and `pause_dpms` on the wallpaper is drawn on behind such a
    lock screen.
  - When the last output is gone, `walpe-ctl status` goes on listing the monitors the
    daemon had (`hidden: true`); the daemon restarts in place when one is back. With
    `wayland_follow = on` it has no monitor then, and a monitor that comes back gets its
    wallpaper from its beginning: it was unloaded when the monitor went (a compositor
    that removes an output while its monitor sleeps does this at every wake-up; the
    other monitors are not touched).
  - While the daemon waits for a compositor to come back (`wayland_reconnect`), its
    control socket is there and nobody answers on it: `walpe-ctl` says that the daemon
    did not answer in time, and what was sent in that time is not carried out when it is back.
- A session started with a one-time socket (`WAYLAND_SOCKET`) cannot be restarted in
  place: the daemon ends at the first change of a monitor it would follow that way (see
  "What is different from X11").
- A frame with translucent pixels: the surface is declared opaque; on a driver without
  `EGL_EXT_present_opaque` and without an alpha-free EGL config such pixels can show the
  compositor's background through them (the log says so at start).
- If the compositor closes a wallpaper surface while its monitor stays, the daemon gives
  the surface its role back in place, up to three times in ten seconds, else it restarts in
  place (`WALPE_WL_RECREATE=0`: always). With `wayland_follow = on` there is no restart
  for it: that monitor's wallpaper is unloaded and loaded again on a new surface, the
  other monitors go on. This path did not occur in the tests with a real compositor.
- `--windowed` (a normal window for trying things out) needs only `xdg-shell` and works
  where wallpapers cannot.

## For a bug report

`walpe-ctl backend`, `walpe-ctl status` (the `wl_*` members, among them `wl_layer`, and
`monitors`), the daemon's log
from its start (the lines `display backend:`, `compositor:`, `present: Wayland:` and every
line starting with `wayland:`), the compositor and its version. If the daemon ended with
`wayland: protocol error ...`, that whole line: it names the request the compositor
refused. `WAYLAND_DEBUG=1` in the daemon's environment prints the protocol exchange.

On KDE Plasma also: `pause_rules` and `pause_kwin_script` of the status, the three
`wl_plasma_*` members, the output of `contrib/plasma/install.sh status` when the add-on
is installed, and `presented_flags` of each entry of `wl_surfaces`. That last number is
what the compositor said about the last frame it showed (the kind bits of
`wp_presentation_feedback`: 1 vsync, 2 hardware clock, 4 hardware completion, 8 zero
copy). In a report from a real Plasma 6.8 session it was 15 while the wallpaper was the
only thing on the screen and 7 from the moment Plasma's desktop lay over it: the one
thing that changed for the daemon when the wallpaper disappeared. It is no rule to pause
by (a panel or any window over the wallpaper ends zero copy too), only a hint.
