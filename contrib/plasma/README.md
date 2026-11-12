# WalPE under the desktop icons on KDE Plasma 6 (prototype)

Not part of the build: two small text packages, installed per user by `./install.sh`.
Nothing here is compiled, loaded or run unless a Plasma user installs it.

On Plasma the desktop icons and widgets are drawn by plasmashell in one window per screen,
together with the wallpaper. `walpe-daemon` draws in its own window (a layer-shell surface on
the background layer). KWin puts both in its desktop layer, in the order of their last raise:
whichever is on top hides the other.

| Part | What it does |
|---|---|
| `wallpaper/` (Plasma wallpaper type "WalPE", QML) | draws nothing and sets the desktop window's colour to transparent, so what is under that window shows through between the icons |
| `kwin-script/` (KWin script, JavaScript) | `workspace.constrain(walpe, desktop)` for each output: walpe-daemon's surface stays under plasmashell's desktop window. Needs KWin 6.5 or newer |

`walpe-daemon` is unchanged and keeps doing all the rendering; plasmashell repaints only when
its own icons or widgets change.

    ./install.sh            # install both for this user, enable the script
    ./install.sh select     # WalPE as the wallpaper type of every screen
    ./install.sh remove

Checked on Plasma 6.8.2 / KWin 6.8.2 / Qt 6.12 (software rendering, one virtual output).
Known limits: the pointer does not reach the wallpaper (plasmashell's window is above it and
takes all input); while walpe-daemon is not running the desktop background is black.
