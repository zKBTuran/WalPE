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
> go and change their mode, scale or transform under the running daemon; the restart in
> place, and the wait for a compositor that was killed and started again; the pause rules
> with stand-ins for logind and for the compositor's idle and window-list protocols.
> One more thing was looked at on one more compositor: where a video wallpaper lies
> relative to Plasma's desktop, on a headless KWin 6.8.2 and 5.27.11 with Plasma's shell
> ([KDE Plasma](#kde-plasma)).
> Nothing on this page has been
> run on a real Wayland desktop, on a real output, on a GPU, or with any other compositor.
> What is said below about Hyprland, KDE Plasma, GNOME, COSMIC, labwc, Wayfire, river and
> niri follows from the protocols they are documented to offer; it is marked "expected".
> Reports are welcome.

## What it needs

A compositor that offers the layer shell (protocol `zwlr_layer_shell_v1`), and an EGL with
`EGL_KHR_platform_wayland` (Mesa has it; NVIDIA's driver needs its `egl-wayland` library).

| Compositor | Layer shell | |
|---|---|---|
| sway | yes | tested headless (1.9) |
| KDE Plasma (KWin) | yes | shown on a headless Plasma 6.8 and 5.27; Plasma's desktop icons are behind the wallpaper: see [KDE Plasma](#kde-plasma) |
| Hyprland, river, labwc, Wayfire, niri, COSMIC | yes | expected to work, not tested |
| GNOME (Mutter), weston's desktop shell | no | not supported: use an X11 session |

Without the layer shell the daemon says so and names the protocol. With `backend=auto` it
then uses X11 if `DISPLAY` is set; under Xwayland an X window is not a desktop background
(it floats above the compositor's windows), so on GNOME the practical answer is its X11
session where the distribution still offers one.

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
  shell; otherwise X11 when `DISPLAY` is set; otherwise the daemon ends with
  `walpe-daemon: no display to draw on: ...` and says what was missing.
- `x11` / `wayland` by name: that backend or an error. No silent fallback to the other.
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

A monitor that is plugged in, removed or changes its mode, scale or rotation makes the
daemon restart itself in place about two seconds after the change has settled, as on X11.

When the compositor goes away the daemon shuts down in order and exits with status 1, as it
does when an X server goes away. `wayland_reconnect = <seconds>` in `gpu.conf`
([GPU-PRESENT.md](GPU-PRESENT.md)) makes it restart in place instead and wait that long for
the compositor's socket to come back. While it waits, `walpe-ctl` reports that the daemon
did not answer in time, and what was sent to it meanwhile is not carried out when it is back.
A session started with a one-time socket (`WAYLAND_SOCKET` instead of
`WAYLAND_DISPLAY`) cannot be reconnected to. Known gap: for the same reason the daemon
cannot restart itself in place there. At the first change of a monitor that it would
follow by restarting it ends with status 1 and `no display to draw on` (with `backend=auto`
and `DISPLAY` set it comes back on X11 instead); start it again from the compositor.

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

Expected, not tested: KWin and GNOME Shell offer other programs no window list, so the
fullscreen and focus rules have no source there; the "no longer shown" part of the covered
rule still works.

Lockers that do not tell logind that the session is locked (seen with a lock client on
sway): the wallpaper pauses through the covered rule, because the compositor stops showing
it, not through the display-sleep rule.

The rules that ask the compositor (idle time, window list) use a connection of their own.
Its first answers are waited for 5 seconds at most when the daemon starts; a compositor
that stays silent leaves those rules without a source, and each of them says in
`pause_rules` that the compositor did not answer.

Tested with stand-ins only: logind (a stand-in login manager on a private bus; no system
bus was available), the idle and window-list protocols (a stand-in compositor in the test
`sig-wl`).

Switches in the daemon's environment: `WALPE_LOGIND`, `WALPE_WL_COVERED`
([ENVIRONMENT.md](ENVIRONMENT.md)).

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
  (all expected; with waybar only the registration and the actions were tested, the icon's
  picture and menu were not looked at). For a panel that only reports the click, and for
  `walpe-tray --menu`, the tray opens its own menu as a small window the compositor places.
  `walpe-tray --backend=sni|xembed`, `WALPE_TRAY_BACKEND` or `backend=` in `tray.ini` force
  a protocol.
- **Window** (`walpe`): uses no X11 call and behaves the same on both. Its icon and name in
  task bars come from the desktop entry `io.github.walpe.ui.desktop`. On GTK older than
  4.20 it follows the desktop's dark / light preference through the settings portal
  (`xdg-desktop-portal`); `GTK_THEME` still wins.

## Starting with the session

XDG autostart entries (the installed `walpe-autostart.desktop`, `walpe-tray.desktop`) are
run by desktops such as GNOME, KDE and Xfce. sway, Hyprland, river and labwc do not run
them by themselves; start the engine from the compositor's configuration (sway: `exec
walpe-daemon`, `exec walpe-tray`; Hyprland: `exec-once = ...`). With the systemd user unit
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
pictures. Started while an application's window was the active one (the WalPE window, a
terminal), the wallpaper lay in front of Plasma's desktop and covered its icons until the
first click on the desktop: KWin then raised Plasma's desktop over the wallpaper, the
video was gone, and Plasma's own wallpaper and icons were back. Started while Plasma's
desktop itself was the active window, the wallpaper was behind it from the start and
never seen (Plasma 6). Either way the daemon went on running and drawing, with nothing in
its log or status: KWin goes on asking a surface it has covered for frames. A user
reported the first on Plasma 6.8; a headless Plasma 6.8.2 and a headless Plasma 5.27
showed the same.

From this version on the daemon puts its wallpapers on the layer shell's **bottom** layer
when the compositor is KWin (told by the protocols `org_kde_plasma_shell` or
`org_kde_plasma_virtual_desktop_management`, which only KWin offers). KWin stacks that
layer above Plasma's desktop and below every window, so the wallpaper stays in front of
Plasma's desktop whatever is clicked. The log says so when the daemon starts (a line
starting with `wayland: KDE Plasma (KWin):`), and `walpe-ctl status` has `wl_layer`
(`background` or `bottom`) and `wl_layer_why`.

What follows from it on Plasma:

- **Plasma's desktop icons and widgets cannot be seen while WalPE shows a wallpaper.**
  They are part of the window the wallpaper lies on. The layer shell has no place between
  Plasma's wallpaper and Plasma's icons, since both are one window, so no wallpaper
  program that is a Wayland client of its own can draw under the icons there; on Plasma
  only a wallpaper plugin that runs inside Plasma's shell can, and WalPE has none. The
  icons are back when the daemon is quit.
- Clicks on the desktop go on reaching Plasma as long as the wallpaper does not use the
  pointer (a video and a picture never do, see "Clicks" in the table above): its
  right-click menu opens, and icons that cannot be seen still take clicks. If that is in
  the way, Plasma's desktop settings ("Desktop and Wallpaper" in the desktop's menu)
  have the layout "Desktop", which shows no icons, next to "Folder View".
- `WALPE_WL_LAYER=background` in the daemon's environment brings back the placement of
  0.14.0, `WALPE_WL_LAYER=bottom` asks for the bottom layer on any compositor
  ([ENVIRONMENT.md](ENVIRONMENT.md)). On the background layer the wallpaper is in front
  of Plasma's desktop or behind it depending on which window was active when the daemon
  started, and a click on the desktop hides it.

> **State of testing on Plasma.** A headless Plasma 6.8.2 (KWin 6.8.2 on its virtual
> output, Qt 6.12) and a headless Plasma 5.27 (KWin 5.27.11), each with one 1280 x 800
> output, Mesa's software renderer, a video wallpaper and clicks from a virtual pointer.
> With 0.14.0: what is described above. With this version: the wallpaper is in KWin's
> stacking layer above Plasma's desktop from the start (on 6.8.2 both with Plasma's
> desktop and with the WalPE window as the active window), it stays there through left
> clicks on the desktop, and a right click opens Plasma's desktop menu over it. Not run: a
> real Plasma session, a GPU, more than one monitor, "Peek at Desktop", the Plasma
> versions in between.

## Notes per compositor (expected, not tested)

- **sway**: remove `output * bg ...` from its configuration (or stop swaybg): that is
  another background surface, and the one created last lies on top. The layer namespace of
  WalPE's surfaces is `walpe`.
- **Hyprland**: do not run hyprpaper / swww at the same time.
- **KDE Plasma**: see [KDE Plasma](#kde-plasma) above.
- **COSMIC**: its own wallpaper program is a background-layer surface too.
- **Desktop-icon programs** on a layer above the background stay visible, and hide the
  wallpaper where they paint an opaque background.
- **NVIDIA's driver** needs its Wayland EGL library (`egl-wayland`).

## Limits

- Nothing above has been confirmed on a real compositor session, a real output or a GPU.
  That includes web wallpapers on the private page host.
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
    daemon had (`hidden: true`); the daemon restarts in place when one is back.
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
  place (`WALPE_WL_RECREATE=0`: always restart). This path did not occur in the tests.
- `--windowed` (a normal window for trying things out) needs only `xdg-shell` and works
  where wallpapers cannot.

## For a bug report

`walpe-ctl backend`, `walpe-ctl status` (the `wl_*` members, among them `wl_layer`, and
`monitors`), the daemon's log
from its start (the lines `display backend:`, `compositor:`, `present: Wayland:` and every
line starting with `wayland:`), the compositor and its version. If the daemon ended with
`wayland: protocol error ...`, that whole line: it names the request the compositor
refused. `WAYLAND_DEBUG=1` in the daemon's environment prints the protocol exchange.
