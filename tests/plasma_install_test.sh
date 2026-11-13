#!/bin/sh
# plasma_install_test.sh <contrib/plasma/install.sh>
# The installer of the add-on for KDE Plasma against stand-ins for the three tools it
# calls (kpackagetool6, dbus-send, kwin_wayland), in a home directory of its own - one
# with a space in its name. No Plasma is needed and none is touched.
#   refuses, and changes nothing: outside Plasma, on X11, on a KWin before 6.5, with a
#   home directory that is somebody else's (sudo -E);
#   install copies the package, a second install updates it;
#   select needs the package and Plasma's shell;
#   uninstall takes the type off the desktops first, and removes nothing where that
#   cannot be done while a desktop still has the type (Plasma's shell not running, or
#   not answering): a desktop whose wallpaper type has no files is black;
#   uninstall with no desktop of the type removes the package without the shell, and
#   says that nothing is installed where nothing is.
#   status outside Plasma, without KWin: no "?" and no line that ends in a comma.
# The stand-ins: FAKE_SHELL=up|down|deaf (org.kde.plasmashell on the bus, not on it, on
# it and not answering evaluateScript), FAKE_KWIN=<version>; what was sent to Plasma's
# shell is appended to $FAKE_LOG.
script=$1
fails=0
tmp=$(mktemp -d "${TMPDIR:-/tmp}/walpe-pit.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
home="$tmp/ho me"
mkdir -p "$tmp/bin" "$home/.config"
type_id=io.github.walpe.plasma
pkg="$home/.local/share/plasma/wallpapers/$type_id"
rc_file="$home/.config/plasma-org.kde.plasma.desktop-appletsrc"

cat > "$tmp/bin/kwin_wayland" <<'EOF'
#!/bin/sh
echo "kwin ${FAKE_KWIN:-6.8.2}"
EOF
cat > "$tmp/bin/kpackagetool6" <<'EOF'
#!/bin/sh
# kpackagetool6 --type Plasma/Wallpaper --show ID | --install DIR | --upgrade DIR | --remove ID
dest="${XDG_DATA_HOME:-$HOME/.local/share}/plasma/wallpapers"
id=io.github.walpe.plasma
case "$3" in
--show) [ -f "$dest/$4/metadata.json" ] ;;
--install) [ ! -e "$dest/$id" ] || exit 4; mkdir -p "$dest" && cp -R "$4" "$dest/$id" ;;
--upgrade) [ -e "$dest/$id" ] || exit 4; rm -rf "$dest/$id" && cp -R "$4" "$dest/$id" && echo upgraded >> "$FAKE_LOG" ;;
--remove) [ -e "$dest/$4" ] || exit 3; rm -rf "$dest/$4" ;;
*) exit 2 ;;
esac
EOF
cat > "$tmp/bin/dbus-send" <<'EOF'
#!/bin/sh
case "$*" in
*NameHasOwner*) [ "${FAKE_SHELL:-up}" = down ] && echo "   boolean false" || echo "   boolean true" ;;
*evaluateScript*)
    [ "${FAKE_SHELL:-up}" = up ] || { echo "Error org.freedesktop.DBus.Error.NoReply: no reply" >&2; exit 1; }
    for a in "$@"; do last=$a; done
    printf '%s\n----\n' "$last" >> "$FAKE_LOG"
    case "$last" in *"print('yes')"*) [ -n "${FAKE_SELECTED:-}" ] && echo "   yes" ;; esac ;;
*) exit 1 ;;
esac
exit 0
EOF
chmod +x "$tmp/bin/kwin_wayland" "$tmp/bin/kpackagetool6" "$tmp/bin/dbus-send"

# run NAME EXPECTED-STATUS [VAR=value ...] -- ARGS: the installer in a Plasma Wayland session
run () {
    name=$1; want_rc=$2; shift 2
    envs=
    while [ "$1" != -- ]; do envs="$envs $1"; shift; done
    shift
    # shellcheck disable=SC2086
    env -i PATH="$tmp/bin:$PATH" HOME="$home" XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland \
        FAKE_LOG="$tmp/log" $envs sh "$script" "$@" > "$tmp/out" 2> "$tmp/err"
    rc=$?
    if [ "$rc" != "$want_rc" ]; then
        fails=$((fails + 1))
        echo "FAIL: $name: exit status $rc, expected $want_rc"
        sed 's/^/    | /' "$tmp/out" "$tmp/err"
    fi
}
want () {   # file, fixed string, what
    if ! grep -F -q -- "$2" "$1"; then
        fails=$((fails + 1))
        echo "FAIL: $3: no '$2' in:"
        sed 's/^/    | /' "$1"
    fi
}
is () {     # what, test arguments...
    what=$1; shift
    if ! test "$@"; then
        fails=$((fails + 1))
        echo "FAIL: $what"
    fi
}

# 1. where the add-on has no use, nothing is changed
run "not Plasma" 1 XDG_CURRENT_DESKTOP=sway -- install
want "$tmp/err" "not a KDE Plasma session" "not Plasma"
is "not Plasma: nothing installed" ! -e "$pkg"
run "Plasma on X11" 1 XDG_SESSION_TYPE=x11 -- install
want "$tmp/err" "runs on X11" "Plasma on X11"
run "KWin 6.4" 1 FAKE_KWIN=6.4.5 -- install
want "$tmp/err" "too old" "KWin 6.4"
run "KWin 5.27" 1 FAKE_KWIN=5.27.11 -- install
is "old KWin: nothing installed" ! -e "$pkg"
run "select before install" 1 -- select
want "$tmp/err" "not installed yet" "select before install"

# 2. somebody else's home directory (sudo -E): nothing is written there
other="$tmp/other"
mkdir -p "$other"
if [ "$(id -u)" = 0 ]; then chown 54321 "$other"; else other=/; fi
if [ ! -O "$other" ]; then
    run "foreign home" 1 "HOME=$other" -- install
    want "$tmp/err" "without sudo" "foreign home"
    is "foreign home: nothing installed" ! -e "$other/.local/share/plasma"
fi

# 3. install, and again
run "install" 0 -- install
want "$tmp/out" "Installed" "install"
is "install: the package is there" -f "$pkg/metadata.json"
is "install: with the KWin script" -f "$pkg/contents/code/below.js"
run "install again" 0 -- install
want "$tmp/out" "Updated" "install again"
want "$tmp/log" "upgraded" "install again"

# 4. select
run "select without the shell" 1 FAKE_SHELL=down -- select
want "$tmp/err" "not running" "select without the shell"
: > "$tmp/log"
run "select" 0 -- select
want "$tmp/log" "d.wallpaperPlugin = '$type_id'" "select: what Plasma's shell is told"
want "$tmp/log" "d.writeConfig('Previous', before)" "select: the type before is kept"

# 5. uninstall while a desktop has the type and nobody can take it off: nothing goes
printf '[Containments][1]\nwallpaperplugin=%s\n' "$type_id" > "$rc_file"
run "uninstall, shell not running, type in use" 1 FAKE_SHELL=down -- uninstall
want "$tmp/err" "still has the wallpaper type" "uninstall, shell not running, type in use"
is "uninstall refused: the package is still there" -f "$pkg/metadata.json"
run "uninstall, shell not answering" 1 FAKE_SHELL=deaf -- uninstall
is "uninstall, shell not answering: the package is still there" -f "$pkg/metadata.json"

# 6. uninstall in the session: the desktops get their type back first
: > "$tmp/log"
run "uninstall" 0 -- uninstall
want "$tmp/log" "readConfig('Previous'" "uninstall: the desktops are given their type back"
want "$tmp/out" "Removed" "uninstall"
is "uninstall: the package is gone" ! -e "$pkg"

# 7. no desktop has the type: the shell is not needed to remove the files
run "install (2)" 0 -- install
printf '[Containments][1]\nwallpaperplugin=org.kde.image\n' > "$rc_file"
run "uninstall, shell not running, type not in use" 0 FAKE_SHELL=down -- uninstall
is "uninstall without the shell: the package is gone" ! -e "$pkg"

# 7b. uninstall once more, with nothing installed: not "Removed"
run "uninstall, nothing installed" 0 FAKE_SHELL=down -- uninstall
want "$tmp/out" "is not installed" "uninstall, nothing installed"
if grep -q "Removed" "$tmp/out"; then
    fails=$((fails + 1))
    echo "FAIL: uninstall, nothing installed: it says \"Removed\""
fi

# 8. status and the usage line change nothing and need nothing
run "status" 0 FAKE_SHELL=down -- status
want "$tmp/out" "not installed" "status"
want "$tmp/out" "session: KDE on wayland, kwin 6.8.2" "status: the session line in Plasma"
run "no command" 2 --
want "$tmp/out" "usage:" "no command"

# 9. status outside Plasma, on a machine without KWin: every line says something (the
# session line was "session: ? on ?, ", ending in the comma)
mkdir -p "$tmp/bare"
for t in sh sed grep head dirname id; do ln -s "$(command -v $t)" "$tmp/bare/$t"; done
bare () {   # name, [VAR=value ...]: status with nothing of Plasma in PATH or the environment
    name=$1; shift
    # shellcheck disable=SC2086
    env -i PATH="$tmp/bare" HOME="$home" "$@" sh "$script" status > "$tmp/out" 2> "$tmp/err"
    is "$name: exit status 0" $? = 0
    if grep -q -e '?' -e '[,:] *$' "$tmp/out" || [ -s "$tmp/err" ]; then
        fails=$((fails + 1))
        echo "FAIL: $name: a line with '?', or one that ends in a comma or colon, or an error:"
        sed 's/^/    | /' "$tmp/out" "$tmp/err"
    fi
}
bare "status, no session"
want "$tmp/out" "session: no desktop named (XDG_CURRENT_DESKTOP and XDG_SESSION_TYPE are not set), no KWin (kwin_wayland was not found)" "status, no session"
want "$tmp/out" "this is not a KDE Plasma session" "status, no session"
want "$tmp/out" "not installed" "status, no session"
bare "status, another desktop" XDG_CURRENT_DESKTOP=sway XDG_SESSION_TYPE=wayland
want "$tmp/out" "session: sway on wayland, no KWin (kwin_wayland was not found)" "status, another desktop"
want "$tmp/out" "this is not a KDE Plasma session" "status, another desktop"
bare "status, only the desktop named" XDG_CURRENT_DESKTOP=KDE
want "$tmp/out" "session: KDE (XDG_SESSION_TYPE is not set), no KWin" "status, only the desktop named"

if [ $fails != 0 ]; then
    echo "$fails check(s) failed"
    exit 1
fi
echo "plasma-install: ok"
