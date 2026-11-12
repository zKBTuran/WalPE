# X11 desktops

WalPE's X11 backend was written under dwm, a bare window manager: nothing but the root
window is below the windows, and the wallpaper is one more window at the very bottom.
A full desktop is different in two ways, and each of them hid the wallpaper or put it over
everything up to 0.14.0:

- **The desktop has a desktop window of its own.** Its file manager puts a window over the
  whole root - type `_NET_WM_WINDOW_TYPE_DESKTOP`, the desktop's picture and icons drawn
  into it - and the window manager keeps it below every other window. A wallpaper window at
  the bottom of the root is behind it: the daemon ran, said "running", and nothing was seen
  (XFCE, LXQt, LXDE, MATE).
- **The window manager does not show windows in the X server's order.** Mutter, its forks
  and KWin paint a window they do not manage above every window they do manage. The
  wallpaper window was such a window: lowest in the server's order, topmost on screen,
  over every window and the panel (Cinnamon, GNOME on X11, KDE Plasma on X11).

What the daemon does now is decided by what it finds, never by a setting a bare window
manager would notice: no window of the desktop type and a window manager of another name
mean exactly the window of 0.14.0, and not one request more to the X server after start-up.

| It finds | The wallpaper window is |
|---|---|
| no desktop window; any window manager but the two below (dwm, i3, openbox, bspwm, awesome, IceWM, Fluxbox, xfwm4, marco, none) | override-redirect, the lowest child of the root (as always) |
| a desktop window over the monitor that nothing is seen through: a 24-bit visual, or no compositing manager (xfdesktop, pcmanfm, pcmanfm-qt, caja) | a child of that desktop window, as large as the monitor: above the desktop's picture and icons, below every window and panel |
| a desktop window with a 32-bit visual under a compositing manager (nemo-desktop) | left where it is, below that window: its icons are seen above the wallpaper |
| a window manager named Mutter, Muffin or GNOME Shell | a managed window of the desktop type (no focus, no input, on every workspace), which Mutter keeps in its lowest layer |
| a window manager named KWin | a managed window of the desktop type; with Plasma's desktop window (32-bit, but painted all over) a child of that window |

`walpe-ctl diag` says which of these applies in its "window system" line, the log says it
when it happens, and `walpe-ctl status` has `x11_window`, `x11_window_manager`,
`x11_desktop_windows` and `x11_wallpaper_parent`.

## What was run

Everything below was run on **2026-11-12, headless, on a software renderer** (Mesa
llvmpipe in Xvfb, one 1280x800 screen, Ubuntu 24.04's packages), each desktop in a private
home directory, started by hand without a display manager. **No real session, no graphics
card and no real pair of monitors was run.** Per desktop: a picture and a video as
wallpaper, a right and a left click on the desktop, a terminal opened, maximized, made
fullscreen and closed, the window manager's "show desktop", the tray.

| Desktop | Wallpaper seen | The desktop's icons | Clicks on the desktop | Pause rules | Tray | What you have to do |
|---|---|---|---|---|---|---|
| **XFCE 4.18** (xfwm4 4.18.0 with its compositor on and off, xfdesktop 4.18.1, xfce4-panel 4.18.4) | yes, in xfdesktop's window | behind the wallpaper | xfdesktop's menu opens | covered (maximized under the panel), fullscreen | StatusNotifier, the panel draws the menu | nothing |
| **LXQt 1.4** (openbox 3.6.1, pcmanfm-qt 1.4.1, lxqt-panel 1.4.0; also with picom 10.2) | yes, in pcmanfm-qt's window | behind the wallpaper | pcmanfm-qt's menu opens | covered, fullscreen | StatusNotifier, the panel draws the menu | nothing |
| **LXDE** (openbox 3.6.1, pcmanfm 1.3.2, lxpanel 0.10.1) | yes, in pcmanfm's window | behind the wallpaper | pcmanfm's menu opens | covered, fullscreen | XEmbed: docked according to the tray and the window tree; not seen in a picture (lxpanel drew a third of its width in this session, with and without WalPE) | nothing |
| **MATE 1.26** (marco 1.26.2 with its compositor on and off, caja 1.26.3, mate-panel 1.27.1) | yes, in caja's window | behind the wallpaper | caja's menu opens | covered, fullscreen | StatusNotifier, the panel draws the menu | nothing |
| **Cinnamon 6.0.4** (Muffin 6.0.1, nemo-desktop 6.0.2; `cinnamon`, the settings daemons and nemo-desktop started one by one) | yes | **above the wallpaper** | nemo-desktop's menu opens | covered, fullscreen | XEmbed (Cinnamon's own tray), WalPE's menu | nothing |
| **GNOME 46 on X11** (GNOME Shell 46.0 as window manager, Mutter 46.2; no gnome-session) | yes | none in a plain GNOME; the "Desktop Icons NG" extension was not run | GNOME's background menu opens | covered, fullscreen | not run (GNOME has no tray without an extension) | nothing |
| **KDE Plasma 5.27 on X11** (kwin_x11 5.27.11 with compositing, plasmashell 5.27.12, started one by one) | yes, in plasmashell's desktop window | behind the wallpaper, widgets too | Plasma's desktop menu opens | covered, fullscreen | not run on X11 | nothing |
| **openbox 3.6.1, i3 4.23, IceWM 3.4.5, Fluxbox 1.3.7, bspwm 0.9.10, awesome 4.3, dwm 6.4** (the window manager alone) | yes, the lowest window of the root | - | the window manager's root menu opens where it has one | covered (tiled or maximized), fullscreen | not run again here | nothing; unchanged from 0.14.0 |

Also run: two monitors as two rectangles of one wide X screen under XFCE (xfdesktop has
one window for the whole screen: both wallpaper windows are children of it, a window over
the second one pauses the second one only); xfdesktop, caja, pcmanfm and pcmanfm-qt ended
and started again while the daemon ran; XDG autostart of daemon and tray in an XFCE
session.

Not run: Plasma 6's X11 session, Budgie, Pantheon, GNOME with desktop icons, a desktop
with one desktop window per monitor on two real monitors, any of them with a graphics
card. Mutter's forks are recognised by name ("Mutter(Budgie)", "Mutter(Gala)") and get
the window Cinnamon gets; that was not tried on them.

## Known limits

- **The desktop's icons are behind the wallpaper on XFCE, LXQt, LXDE, MATE and KDE
  Plasma.** Those desktops draw their icons into the same window as their own picture, and
  nothing is seen through that window; a wallpaper has to be above all of it or below all
  of it. (Cinnamon's icons are a window of their own that is see-through: there they stay.)
  The icons are still there: a double click where one lies opens it. To switch them off:

  | | |
  |---|---|
  | XFCE | `xfconf-query -c xfce4-desktop -p /desktop-icons/style -n -t int -s 0` (2 brings them back) |
  | MATE | `gsettings set org.mate.background show-desktop-icons false` (caja's desktop window goes, and its menu with it) |
  | LXQt | `pcmanfm-qt --desktop-off` (for good: the Desktop module in Session Settings; not tried here) |
  | LXDE | `pcmanfm --desktop-off` |
  | KDE Plasma | Configure Desktop and Wallpaper... > Layout: Desktop (not tried here) |

- The desktop's own wallpaper is behind WalPE's and its setting changes nothing you see.
- **The wallpaper window ends with the desktop window it is in.** When the file manager is
  closed or restarted (`xfdesktop --quit`, the commands above for MATE, LXQt and LXDE, a
  crash) the daemon makes its windows anew - a restart in place, about two seconds - and
  shows the wallpaper on the root, or in the new desktop window when one appears.
- "Skip hidden areas" (`skip_hidden`, off by default) counts everything as visible while a
  wallpaper window is inside a desktop window: nothing is skipped there.
- `meson install` with the default prefix puts the autostart entries into
  `/usr/local/etc/xdg/autostart`, where no session looks. Configure with
  `--sysconfdir=/etc`, or copy `walpe-autostart.desktop` and `walpe-tray.desktop` to
  `~/.config/autostart/`.

## Switches

Both are read from the daemon's environment at start ([ENVIRONMENT.md](ENVIRONMENT.md)).

- `WALPE_X11_DESKTOP=root` keeps the wallpaper window the lowest window of the root on
  every desktop, as up to 0.14.0. Behind an opaque desktop window it is not seen; the log
  says so once. Use it together with switching the file manager's desktop off.
  `WALPE_X11_DESKTOP=window` puts it into a see-through desktop window as well (over
  Cinnamon's icons).
- `WALPE_X11_WINDOW=override` makes the wallpaper window override-redirect under Mutter
  and KWin too; `WALPE_X11_WINDOW=managed` makes it a managed window of the desktop type
  under any window manager.

## For a bug report

`walpe-ctl diag`, and a picture of the screen. Under a compositing manager take the picture
with the desktop's own screenshot tool: `xwd -root` and `import -window root` ask the X
server for the root window's picture, which is not what the compositing manager shows (in
Xvfb it is black wherever a 32-bit window lies on top).
