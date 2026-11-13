# The WalPE add-on for KDE Plasma 6 (Wayland)

Optional, and only for Plasma on Wayland. It is not part of the build: `meson` and
`build.sh` do not know this directory, nothing of it is compiled, installed or loaded
unless a Plasma user runs `./install.sh install`. On X11 and on every other desktop WalPE
needs none of it.

**What it is for.** On Plasma the desktop icons and widgets are one window of Plasma's
shell, opaque, together with Plasma's own wallpaper. `walpe-daemon` draws in a window of
its own and keeps it in front of that window, so while WalPE shows a wallpaper Plasma's
icons and widgets cannot be seen ([docs/WAYLAND.md](../../docs/WAYLAND.md#kde-plasma)).
With the add-on they are shown over the WalPE wallpaper, and they take clicks as usual.

    ./install.sh install     # for this user: copy the wallpaper type to ~/.local/share
    ./install.sh select      # the wallpaper type "WalPE" on every desktop
    ./install.sh status      # what is installed and selected

Instead of `select`: right-click the desktop, "Desktop and Wallpaper", wallpaper type
**WalPE**, OK. The wallpaper itself is still chosen in WalPE (`walpe`, `walpe-ctl`, the
tray); the page in Plasma's dialog only has what the desktop shows while WalPE shows
nothing: a colour or a picture (`select` takes over the picture of Plasma's "Image"
type when that names a file).

    ./install.sh unselect    # every desktop back to the wallpaper type it had before
    ./install.sh uninstall   # unselect and remove

`unselect` knows the type a desktop had before from `select`; a desktop that got "WalPE"
in Plasma's dialog goes back to Plasma's "Image" with the picture it had. `select` and
`unselect` go through the desktops of every activity. `uninstall` has to be run in the
Plasma session: it removes nothing while a desktop still has the type and Plasma's
shell is not there to give it another one (a desktop whose wallpaper type has no files
is black behind its icons).

`install` may be run again (after an update of WalPE it replaces the type; Plasma's shell
keeps the version it has loaded until it is restarted, e.g. by logging out and in).
Needs Plasma 6.5 or newer and the tools `kpackagetool6` and `dbus-send`: `install.sh`
says what is missing and changes nothing on an older Plasma, on X11 and outside Plasma.

## What is in it

One Plasma package, `wallpaper/` (`io.github.walpe.plasma`), installed to
`~/.local/share/plasma/wallpapers/`:

| File | What it does |
|---|---|
| `contents/ui/main.qml` | the wallpaper type: draws no wallpaper, makes Plasma's desktop window see-through, and fills it with a colour or a picture for as long as no surface of `walpe-daemon` lies under it |
| `contents/code/below.js` | a KWin script: asks Plasma's shell which desktops have the type, tells KWin to keep those desktop windows right above the daemon's surface on the same screen (`workspace.constrain`, KWin 6.5), and tells the type whether such a surface is there |
| `contents/ui/Script.qml`, `contents/code/shared.js` | load that script into the running KWin over D-Bus while a desktop has the type, and take it out with the last one. It is no KWin package and is not in KWin's list of scripts; each Plasma shell loads it under a name of its own |
| `contents/ui/Pointer.qml` | hands the left button and the wheel on to `walpe-daemon`; loaded only while a wallpaper there uses the pointer (the position comes from a script the daemon itself loads into KWin) |
| `contents/ui/config.qml`, `contents/config/main.xml` | the type's page in Plasma's dialog and its settings |

`walpe-daemon` goes on doing all the drawing in its own surface on the layer shell's
bottom layer, and tells the add-on nothing; Plasma's shell repaints only when its icons
or widgets change. Nothing in the add-on wakes up by itself: the script acts when a
desktop window or a surface of the daemon comes or goes. The one thing that goes from the add-on
to the daemon is the pointer's buttons and wheel (below), and only while the daemon asks for them.

## How the parts keep each other safe

Two things must not happen: a wallpaper that runs and cannot be seen (an opaque desktop
in front of it), and a black desktop (a see-through desktop with nothing behind it).

- The daemon's surface is in front of Plasma's desktop unless the KWin script puts one
  desktop window above it, and the script does that only for a desktop whose wallpaper
  type Plasma's shell reports as "WalPE". Without the script, with another wallpaper
  type, with an older KWin: the wallpaper is in front, as without the add-on.
- The wallpaper type keeps the desktop filled (the colour or picture of its settings)
  until the script has told it that a surface of the daemon lies under this desktop. The
  script takes that back when the surface goes away (the daemon quits, crashes, loses the
  monitor). The type believes the script only while its own copy is loaded in KWin, and
  not while KWin shows the desktop ("Peek at Desktop" hides the daemon's surface).
- When another wallpaper type is chosen, the WalPE type hands the desktop window back
  opaque and has Plasma make it anew; KWin forgets its rule with the old window.

| What happens | What is seen |
|---|---|
| the type chosen, daemon running | the WalPE wallpaper, icons and widgets over it |
| daemon not running yet, quit or killed | the colour or picture of the type's settings, icons over it |
| daemon started later | the wallpaper appears under the icons |
| another wallpaper type chosen while the daemon runs | the WalPE wallpaper in front, icons hidden (as without the add-on) |
| Plasma's shell is killed and started again | the wallpaper alone, then the icons over it again |
| "Peek at Desktop" | the colour or picture, icons over it |
| the type on one screen only | that screen: icons over the wallpaper; the others: the wallpaper in front |
| the type on the desktop of one activity only | in that activity: icons over the wallpaper; in the others: the wallpaper in front |

One case is left: somebody who takes the script out of KWin by hand
(`org.kde.kwin.Scripting.unloadScript` on D-Bus with the name the shell gave it; no
dialog does that) and then quits the daemon has a black desktop behind the icons until
Plasma's shell is restarted or the type is chosen again.

A shell that is killed leaves its copy of the script in KWin. That copy does nothing any
more (it sees that the desktop is another process's); the next shell loads its own. It is
gone when KWin is restarted, with the next login at the latest.

## The pointer

Plasma's desktop window is above the wallpaper, and KWin gives it every pointer event.
While a wallpaper of the daemon uses the pointer (a web wallpaper, a scene that follows
the pointer), and only then, the daemon gets it from two sides:

- **Where the pointer is: from a script of the daemon's own in KWin.** The daemon loads
  it into KWin over the session bus (`org.kde.kwin.Scripting`, as it does for its pause
  rules) and takes it out when no wallpaper wants the pointer. The script calls the
  daemon while the pointer is over Plasma's desktop window (icons and widgets included)
  on an output whose wallpaper uses the pointer, at most once per frame of the
  wallpaper however fast the mouse reports, and once more when the pointer leaves the
  desktop (for a window, the panel, a menu). Over windows and panels, on other outputs
  and while the pointer rests, nothing is sent. Positions are in the output's logical
  pixels.
- **The left button and the wheel: from `Pointer.qml`**, an item on top of everything in
  the desktop window that only looks (a `PointHandler` with a passive grab, a
  `WheelHandler` that does not block): the icons, the widgets and the desktop's menu get
  every event as before. Where the pointer is over the bare desktop it sends the press,
  the release and the wheel, each with its place, to the daemon
  (`io.github.walpe.Pointer.Event` on the daemon's connection). Over an icon or a widget
  they are Plasma's alone. What counts as one: anything visible in the window that is
  smaller than nine tenths of it both in width and in height (the desktop's own layers
  are as wide or as high as the window).

The right and the middle button are never sent: they open Plasma's menu and paste.

Why two sides. `Pointer.qml` used to read the position as well, with a hover handler on
its item. An item that asks for the hover above Plasma's own takes it from them: while
a wallpaper used the pointer Plasma's icons had no frame under the pointer and its
widgets no tool tips (Plasma 6.8.2, Qt 6.12; a `MouseArea` in its place did the same,
and a handler lower down was not told the position over the bare desktop). KWin knows
where the pointer is without anybody asking Plasma's window for it.

It costs nothing while no wallpaper wants the pointer: no script in KWin, no name on the
bus, `Pointer.qml` not loaded. The daemon takes the name `io.github.walpe.Pointer` and
loads the script only on KWin, with the add-on installed, while a wallpaper it shows uses
the pointer; the type watches for the name and loads `Pointer.qml` only while somebody
has it and the daemon's surface is under the desktop. The daemon treats what comes like
a pointer of its own: the same coordinates per output, the same button and wheel events.
Measured on a headless Plasma 6.8.2 (software rendering, a page that follows the
pointer): 55 to 59 calls a second from the script with the pointer moving at 1000 and at
125 positions a second, 2 in ten seconds with the pointer nearly at rest; see the commit
that brought the script for the CPU time beside the QML path.

The daemon and the add-on have to be of the same version: this `Pointer.qml` with a
daemon that has no pointer script gives the wallpaper clicks without a position.

Who is believed. Any program of the session can take a name on the session bus, so:

- The type does not send to the name. It asks the bus which connection has it and which
  process that is, and sends to that connection only if it is the process whose surface
  KWin sees under the desktop (the KWin script writes that process id next to `Under`).
  A program that takes the name before the daemon gets nothing; the daemon then cannot
  have the name, the wallpaper gets no buttons, and `walpe-ctl status` says in
  `wl_plasma_pointer` that the name is not the daemon's.
- The daemon takes buttons and the wheel from the connection that has the name
  `org.kde.plasmashell` at the time, and positions, like the pause script's reports,
  from the one that has `org.kde.KWin` - there only at the object it made for the
  script it loaded last. While Plasma's shell is not on the bus (it crashed, it is
  being replaced), the first program to take its name is believed in its place: it can
  click in a wallpaper that uses the pointer, with the left, middle or right button,
  and nowhere else. The session bus has no means to tell such a program from the
  shell, and a program that can do this can already do more to a Plasma session (load
  a script into KWin, for one).

## What it cannot do

- **The right and the middle button do not reach the wallpaper, and the left one only
  over the bare desktop.** See "The pointer" above. A press on the bare desktop also
  takes the selection off Plasma's icons and a drag there draws Plasma's selection frame
  over the wallpaper: Plasma cannot be told that the wallpaper has used the event. A web
  wallpaper that needs more wants a desktop with another wallpaper type (the icons are
  then hidden and the wallpaper has the pointer to itself).
- If the add-on is installed while a wallpaper that uses the pointer is already shown,
  the daemon notices when that wallpaper is opened again.
- The Overview shows the desktop's icons without the wallpaper behind them (the small
  pictures of the virtual desktops show it).
- The lock screen and the preview in Plasma's dialog show the colour or picture, never
  the WalPE wallpaper.

## Where the code comes from

Written for WalPE, under WalPE's licence ([LICENSE](../../LICENSE), BSD-3-Clause; the
package names it in its `metadata.json`). No code of another wallpaper plugin is in it.
It uses these interfaces of Plasma 6 and KWin 6: `WallpaperItem` (`org.kde.plasma.plasmoid`),
the D-Bus types of `org.kde.plasma.workspace.dbus`, KWin's scripting API (`workspace`,
`callDBus`, `QTimer`), `org.kde.PlasmaShell.evaluateScript`, `org.kde.kwin.Scripting`
(`loadScript`, `start`, `unloadScript`), the signal `org.kde.KWin.showingDesktopChanged`
and Qt Quick's pointer handlers.

The script is started with `org.kde.kwin.Scripting.start`, not with `run` on the object
`/Scripting/Script<id>` of the id that `loadScript` answers: that id is the number of
scripts KWin has loaded, so after another script was unloaded it is the id, and the
object, of a script that is still there, and `run` reaches that one (seen on KWin 6.8.2).

Run on a headless Plasma 6.8.2 (KWin 6.8.2, Qt 6.12) with Mesa's software renderer only,
with one and two outputs (also at the scales 1.5 and 1.25 side by side, and with one of
them switched off and on), two activities, KWin's permission checks for clients on and
off, and a virtual pointer: no real session, no GPU, no NVIDIA driver, no restart of KWin
under the running shell, no touchpad or touch screen, and of the wallpapers that use the
pointer a web page only.
