# Wayland

`walpe-daemon` draws its wallpapers either as an X11 client (one window per monitor below
all other windows, as in every earlier version) or as a Wayland client (one layer-shell
surface per output on the background layer). One daemon binary can hold both backends; it
picks one when it starts and keeps it until it ends. On an X11 session nothing is meant to
change.

> **State of testing.** The Wayland parts were built and tested only on Mesa's software
> renderer (llvmpipe). Wallpapers were shown on one compositor, a headless sway 1.9
> (wlroots 0.17, pixman renderer); a headless weston 13 served only as the compositor
> without layer shell that the daemon has to turn down. What ran on sway end to end:
> pictures, scenes, videos and web wallpapers on one and two outputs; outputs that appear,
> go and change their mode, scale or transform under the running daemon; the restart in
> place, and the wait for a compositor that was killed and started again; the pause rules
> with stand-ins for logind and for the compositor's idle and window-list protocols.
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
| Hyprland, river, labwc, Wayfire, niri, KDE Plasma (KWin), COSMIC | yes | expected to work, not tested |
| GNOME (Mutter), weston's desktop shell | no | not supported: use an X11 session |

Without the layer shell the daemon says so and names the protocol. With `backend=auto` it
then uses X11 if `DISPLAY` is set; under Xwayland an X window is not a desktop background
(it floats above the compositor's windows), so on GNOME the practical answer is its X11
session where the distribution still offers one.

Used when the compositor offers them, not required: `xdg-output` (monitor names and
layout), `wp_viewporter` (pixel-exact rendering at fractional scales, render resolution
per monitor), `wp_fractional_scale_v1`, `wp_presentation`, `linux-dmabuf` (tells which GPU
the compositor uses). `walpe-ctl status` lists what was found (`wl_compositor_globals`).

Building the backend: see "Build options" in the [README](../README.md).

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
is not running, and what was sent to it in that time is not carried out when it is back.
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
"page host":

- `x11`: an X window off screen. The default wherever an X display is there (`DISPLAY`
  set): on X11 sessions as always, on Wayland sessions through Xwayland.
- `private`: a Wayland display inside `walpe-daemon` to which only the page's toolkit is
  connected; no X is needed. Used when there is no X display at all, or when asked for with
  `WALPE_WEB_HOST=private`. **Experimental**: it has run only in the tests (a stand-in
  client, and a hostile page in `websec-private`), never in a session. Not built with
  `-Dwayland=disabled`.

`WALPE_WEB_HOST=auto|x11|private` in the daemon's environment; `walpe-ctl status` shows
`web_host` once a page was created. Known gap: `DISPLAY` set with no X server behind it (a
stale variable in a session without Xwayland) makes `auto` pick `x11`, the start-up test
fails and web wallpapers are off; set `WALPE_WEB_HOST=private` or unset `DISPLAY` for the
daemon.

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

## Notes per compositor (expected, not tested)

- **sway**: remove `output * bg ...` from its configuration (or stop swaybg): that is
  another background surface, and the one created last lies on top. The layer namespace of
  WalPE's surfaces is `walpe`.
- **Hyprland**: do not run hyprpaper / swww at the same time.
- **KDE Plasma**: Plasma's own desktop (wallpaper, icons, right-click menu) is itself a
  surface on the background layer. Which of the two lies on top is KWin's decision; there
  is no protocol to order them and no setting on WalPE's side.
- **COSMIC**: its own wallpaper program is a background-layer surface too.
- **Desktop-icon programs** on a layer above the background stay visible, and hide the
  wallpaper where they paint an opaque background.
- **NVIDIA's driver** needs its Wayland EGL library (`egl-wayland`).

## Limits

- Nothing above has been confirmed on a real compositor session, a real output or a GPU.
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
    control socket is there and nobody answers on it: `walpe-ctl` says that the daemon is
    not running, and what was sent in that time is not carried out when it is back.
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

`walpe-ctl backend`, `walpe-ctl status` (the `wl_*` members, `monitors`), the daemon's log
from its start (the lines `display backend:`, `compositor:`, `present: Wayland:` and every
line starting with `wayland:`), the compositor and its version. If the daemon ended with
`wayland: protocol error ...`, that whole line: it names the request the compositor
refused. `WAYLAND_DEBUG=1` in the daemon's environment prints the protocol exchange.
