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
    ./install.sh status      # what is installed, selected, loaded in KWin

Instead of `select`: right-click the desktop, "Desktop and Wallpaper", wallpaper type
**WalPE**, OK. The wallpaper itself is still chosen in WalPE (`walpe`, `walpe-ctl`, the
tray); the page in Plasma's dialog only has what the desktop shows while WalPE shows
nothing: a colour or a picture (`select` takes over the picture of Plasma's "Image"
type when that names a file).

    ./install.sh unselect    # every desktop back to the wallpaper type it had before
    ./install.sh uninstall   # unselect and remove

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
| `contents/ui/Script.qml` | loads that script into the running KWin over D-Bus while a desktop has the type, and takes it out with the last one. It is no KWin package and is not in KWin's list of scripts |
| `contents/ui/config.qml`, `contents/config/main.xml` | the type's page in Plasma's dialog and its settings |

`walpe-daemon` is not changed by the add-on and does not talk to it. It goes on doing
all the drawing in its own surface on the layer shell's bottom layer; Plasma's shell
repaints only when its icons or widgets change. Nothing in the add-on runs on a timer:
the script acts when a desktop window or a surface of the daemon comes or goes.

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

One case is left: somebody who takes the script out of KWin by hand
(`org.kde.kwin.Scripting.unloadScript` on D-Bus; no dialog does that) and then quits the
daemon has a black desktop behind the icons until Plasma's shell is restarted or the type
is chosen again.

## What it cannot do

- **The pointer does not reach the wallpaper.** Plasma's desktop window is above it and
  takes all pointer input, where there is an icon and where there is none. Scenes that
  follow the pointer and web wallpapers that are clicked get nothing on a desktop with the
  wallpaper type "WalPE". For those, use another wallpaper type on that screen (the icons
  are then hidden and the wallpaper has the pointer).
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
(`loadScript`, `unloadScript`) and the signal `org.kde.KWin.showingDesktopChanged`.

Run on a headless Plasma 6.8.2 (KWin 6.8.2, Qt 6.12) with Mesa's software renderer only,
with one and two outputs: no real session, no GPU, no NVIDIA driver, no restart of KWin
under the running shell, no activities.
