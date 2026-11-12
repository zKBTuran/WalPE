# The WalPE extension for GNOME Shell (Wayland)

Optional, and only for GNOME on Wayland. It is not part of the build: `meson` and
`build.sh` do not know this directory, nothing of it is compiled (an extension for GNOME
Shell is JavaScript), installed or loaded unless a GNOME user runs `./install.sh install`.
On X11 and on every other desktop WalPE needs none of it, and `walpe-daemon` does nothing
for it there: it asks for the extension in one situation only, a Wayland compositor without
the layer shell that looks like GNOME's.

**What it is for.** GNOME's compositor (Mutter) has no layer shell, the protocol with
which a program puts a surface on the desktop, and nothing else in its place. Without the
extension `walpe-daemon` ends on GNOME's Wayland session with a sentence
([docs/WAYLAND.md](../../docs/WAYLAND.md#gnome)). With it the wallpaper is shown: below
every window and below the desktop icons of "Desktop Icons NG", on all workspaces, behind
the workspaces in the overview, and the pause rules for fullscreen and covering windows
work.

    ./install.sh install     # for this user: copy to ~/.local/share/gnome-shell/extensions, switch on
    (log out and in: GNOME Shell reads extensions when it starts)
    ./install.sh status      # installed? switched on? running?

then start `walpe-daemon` (or WalPE's window, or log in with WalPE's autostart entry) as
on any desktop. The wallpaper itself is chosen in WalPE (`walpe`, `walpe-ctl`, the tray).

    ./install.sh disable     # switch off      (GNOME's "Extensions" has the same switch)
    ./install.sh enable
    ./install.sh uninstall   # switch off and remove

`install` may be run again (after an update of WalPE it replaces the files; GNOME Shell
keeps the version it has loaded until the next login). `install.sh` says what is missing
and changes nothing outside GNOME, on X11, and on a GNOME Shell older than 46.

## What it does

One directory, `walpe@walpe.github.io/` (`metadata.json`, `extension.js`), installed to
`~/.local/share/gnome-shell/extensions/`. While it is enabled, the extension

- owns the name `io.github.walpe.Gnome` on the session bus. That is how the daemon finds
  it: it asks there (`Hello`) and takes the Wayland backend when it gets an answer;
- looks at every new window. `walpe-daemon` opens one ordinary window per monitor with the
  application id `io.github.walpe.wallpaper` and the title `WalPE wallpaper: <connector>`;
  the extension gives such a window the window type DESKTOP, puts it on that monitor at
  the monitor's size, and keeps it below other programs' desktop windows. Everything else
  follows from the type inside GNOME Shell itself: below all windows, on all workspaces,
  not in the overview's window list, the window switcher or the dash, behind the windows
  while the workspace is changed;
- shows the wallpaper behind the workspaces in the overview: a clone of the window in each
  background the shell makes there;
- tells the daemon, per monitor and only when it changes, whether windows cover the
  monitor, whether the focused window is an application's and whether that is fullscreen
  (the pause rules; a locked screen counts as covered).

It starts no program by itself, keeps no timer running and draws nothing: `walpe-daemon`
does all the drawing in its own window, and GNOME Shell composites that window like any
other. The only method that starts something is `Socket`, see the next section.

## GNOME Shell 46 to 48, and 49 and newer

GNOME Shell 49 and newer can change the type of any window (`Meta.Window.set_type`).
46 to 48 can do it only for the windows of a Wayland client that the shell itself has
started (`Meta.WaylandClient`). There the daemon, which was started by you, a session's
autostart or WalPE's window as always, asks the extension for its connection to the
compositor: it hands over one end of a socket pair (method `Socket`), the shell starts
the daemon's own program a second time as `walpe-daemon --wayland-socket-to 4`, and that
process passes the connection it was started with through the socket pair and then only
waits (about 2 MB of memory of its own) until the daemon ends. The extension starts the
file behind `/proc/<pid>/exe` of the caller and nothing a caller could name, so the method
gives a program on the session bus nothing but a second copy of itself.

`walpe-ctl status` and the start-up log say which way it went ("wl_gnome_connection":
"handed over by the shell" or "the daemon's own").

## Switching it off, and what happens when

| What happens | What is seen |
|---|---|
| extension on, daemon started | the wallpaper, below windows and desktop icons |
| daemon quits or is killed | GNOME's own background again |
| daemon started without the extension (off, not installed, not loaded yet) | no wallpaper; the daemon ends with a sentence that names the extension and the command that installs or enables it |
| extension switched off while the daemon runs | the wallpaper stays where it is, a desktop window as before; the overview shows GNOME's own background; the pause rules for windows have no source (`walpe-ctl status` says so). The next start of the daemon ends with the sentence |
| extension switched on again while that daemon still runs | as before: overview and pause rules are back |
| another extension is switched on or off (GNOME Shell then disables and enables the others) | nothing changes |
| the screen is locked | the lock screen has GNOME's own picture; the extension stays loaded (`session-modes` in its `metadata.json` says so, for this reason: nothing is torn down and built again at every lock) and reports the monitors as covered |
| GNOME Shell ends | the daemon ends with it, as with any compositor |
| a monitor is plugged in or pulled | the daemon makes its windows anew (a restart in place); on GNOME Shell 46 to 48 it asks for a new connection |

## What it cannot do

- **Rounded corners in the overview.** GNOME's own background has rounded corners behind
  each workspace in the overview; the wallpaper there has square ones.
- **On GNOME Shell 46 the wallpaper stands still in the overview** (the compositor there
  asks a window that is not on the desktop for no frames) and runs again when the overview
  is left. On 51 it runs in the overview too.
- **The lock screen** shows GNOME's own background, never the WalPE wallpaper.
- **With "Desktop Icons NG" the pointer does not reach the wallpaper**: its window covers
  the monitor above the wallpaper and takes all pointer input, where there is an icon and
  where there is none. Without it a wallpaper that uses the pointer (web wallpapers,
  scenes that follow it) gets motion and clicks like any window, and a click on such a
  wallpaper gives its window the keyboard focus, as a click on a desktop does; GNOME's
  right-click menu of the desktop does not open on it. A wallpaper that does not use the
  pointer lets every click through to GNOME's desktop.
- GNOME on X11 (GNOME Shell 48 and older): not needed and not used; `install.sh` says so.

## Versions

`metadata.json` lists the versions of GNOME Shell the extension was run on: 46 (Ubuntu
24.04) and 51 (Arch Linux). GNOME Shell loads an extension only on the versions it lists;
on 47 to 50 the extension is expected to work (it uses the 46 way up to 48 and the 49 way
from 49 on, and both were run) but was not, so they are not listed. To try it there:
add the version to `shell-version` in the installed `metadata.json`, or switch GNOME's
check off (`gsettings set org.gnome.shell disable-extension-version-validation true`).

What it uses of GNOME Shell and Mutter, for whoever has to adapt it to a new release:
`Meta.Window.set_type` (49+) or `Meta.WaylandClient.new`, `spawnv`, `owns_window`,
`make_desktop` (46 to 48); `Meta.Window` `move_resize_frame`, `lower`, the signals `shown`,
`unmanaged`, `notify::title`; `Meta.Display` `list_all_windows`, `sort_windows_by_stacking`,
`window-created`, `restacked`; `Meta.MonitorManager.get_monitor_for_connector`;
`Clutter.Clone`; and one private method of the shell's JavaScript,
`BackgroundManager._createBackgroundActor` in `ui/background.js`, which it wraps to learn
of each background the shell makes (if that method is renamed, the wallpaper is missing in
the overview and everything else works).

## Where the code comes from

Written for WalPE, under WalPE's licence ([LICENSE](../../LICENSE), BSD-3-Clause). No code
of another extension is in it. Run on a headless GNOME Shell 46.0 (Mutter 46.2, Ubuntu
24.04, with and without Ubuntu's "Desktop Icons NG" 47.0.9, one and two monitors) and
51.1 (Mutter 51.1, Arch Linux, one monitor), with Mesa's software renderer only: no real
session, no GPU, no NVIDIA driver, no fractional scaling, no HDR.
