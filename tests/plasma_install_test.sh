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
#   says that nothing is installed where nothing is;
#   a copy for every user, as a package installs it (in XDG_DATA_DIRS): status says whose
#   copy is in use and shows both versions, select and unselect work with it, install
#   makes or updates the user's own copy and uninstall removes only that one, and the
#   desktops keep their type because its files stay.
#   status outside Plasma, without KWin: no "?" and no line that ends in a comma.
# The stand-ins: FAKE_SHELL=up|down|deaf (org.kde.plasmashell on the bus, not on it, on
# it and not answering evaluateScript), FAKE_KWIN=<version>; what was sent to Plasma's
# shell is appended to $FAKE_LOG. The stand-in for kpackagetool6 finds a package in the
# system's data directories too and changes the user's only, like the real one.
# XDG_DATA_DIRS is the test's own in every run, so a WalPE package installed on the
# machine that runs the test is not seen.
script=$1
case "$script" in /*) ;; *) script=$PWD/$script ;; esac   # the test changes directory
fails=0
tmp=$(mktemp -d "${TMPDIR:-/tmp}/walpe-pit.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
home="$tmp/ho me"
mkdir -p "$tmp/bin" "$home/.config"
type_id=io.github.walpe.plasma
pkg="$home/.local/share/plasma/wallpapers/$type_id"
sysdir="$tmp/sy s/share"
syspkg="$sysdir/plasma/wallpapers/$type_id"
# what the installer gets as XDG_DATA_DIRS: an entry that does not exist, one that is no
# absolute path, the user's own data directory (some sessions list it), then the system's
sysdirs="$tmp/none:relative/share:$home/.local/share/:$sysdir/"
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
# found ID: in the user's directory or in one of the system's
found () {
    [ -f "$dest/$1/metadata.json" ] && return 0
    rest=${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:
    while [ -n "$rest" ]; do
        d=${rest%%:*}; rest=${rest#*:}
        [ -f "$d/plasma/wallpapers/$1/metadata.json" ] && return 0
    done
    return 1
}
case "$3" in
--show) found "$4" ;;
--install) [ ! -e "$dest/$id" ] || exit 4; mkdir -p "$dest" && cp -R "$4" "$dest/$id" ;;
--upgrade)
    found "$id" || exit 4
    # the real tool removes the old copy first, and only in the user's directory
    [ -e "$dest/$id" ] || { echo "Error: Uninstallation of $4 failed: package path was deleted manually" >&2; exit 4; }
    rm -rf "$dest/$id" && cp -R "$4" "$dest/$id" && echo upgraded >> "$FAKE_LOG" ;;
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
        XDG_DATA_DIRS="$sysdirs" FAKE_LOG="$tmp/log" $envs sh "$script" "$@" > "$tmp/out" 2> "$tmp/err"
    rc=$?
    if [ "$rc" != "$want_rc" ]; then
        fails=$((fails + 1))
        echo "FAIL: $name: exit status $rc, expected $want_rc"
        sed 's/^/    | /' "$tmp/out" "$tmp/err"
    fi
}
lacks () {  # file, fixed string, what
    if grep -F -q -- "$2" "$1"; then
        fails=$((fails + 1))
        echo "FAIL: $3: '$2' in:"
        sed 's/^/    | /' "$1"
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

# 9. the copy of a package, for every user (version 9.9.9 here), and none of the user's
src=$(dirname "$script")/wallpaper
mkdir -p "$sysdir/plasma/wallpapers"
cp -R "$src" "$syspkg"
sed 's/"Version": *"[^"]*"/"Version": "9.9.9"/' "$src/metadata.json" > "$syspkg/metadata.json"
ours=$(sed -n 's/.*"Version": *"\([^"]*\)".*/\1/p' "$src/metadata.json")
is "the tree's metadata.json has a version" -n "$ours"
rm -f "$rc_file"
run "status, package's copy" 0 -- status
want "$tmp/out" "installed, the system's copy (from a package), version 9.9.9 ($syspkg)" "status, package's copy"
lacks "$tmp/out" "not installed" "status, package's copy"
: > "$tmp/log"
run "select, package's copy" 0 -- select
want "$tmp/log" "d.wallpaperPlugin = '$type_id'" "select, package's copy: what Plasma's shell is told"
: > "$tmp/log"
run "unselect, package's copy" 0 -- unselect
want "$tmp/log" "readConfig('Previous'" "unselect, package's copy"
# uninstall has nothing of the user's to remove, with or without the shell, and the
# desktops keep their type: its files stay
printf '[Containments][1]\nwallpaperplugin=%s\n' "$type_id" > "$rc_file"
: > "$tmp/log"
run "uninstall, package's copy only" 0 -- uninstall
want "$tmp/out" "Nothing removed" "uninstall, package's copy only"
want "$tmp/out" "The package's copy stays: $syspkg (version 9.9.9). Remove the package to remove it." "uninstall, package's copy only"
lacks "$tmp/log" "d.wallpaperPlugin" "uninstall, package's copy only: the desktops are left alone"
is "uninstall, package's copy only: it is still there" -f "$syspkg/contents/code/below.js"
run "uninstall, package's copy only, shell not running" 0 FAKE_SHELL=down -- uninstall
is "uninstall without the shell: the package's copy is still there" -f "$syspkg/metadata.json"

# 10. install beside the package's copy: the user's own copy is made, not "updated"
: > "$tmp/log"
run "install beside the package's copy" 0 -- install
want "$tmp/out" "Installed" "install beside the package's copy"
want "$tmp/out" "The system has a copy too, from a package: $syspkg (version 9.9.9)." "install beside the package's copy"
want "$tmp/out" "Plasma takes yours, version $ours, before it" "install beside the package's copy"
lacks "$tmp/log" "upgraded" "install beside the package's copy"
lacks "$tmp/err" "Uninstallation" "install beside the package's copy"
is "install beside the package's copy: the user's copy is there" -f "$pkg/metadata.json"
run "install again beside the package's copy" 0 -- install
want "$tmp/out" "Updated" "install again beside the package's copy"
want "$tmp/log" "upgraded" "install again beside the package's copy"
# with the type in use, Plasma's shell has to be restarted to show the new copy
run "install beside the package's copy, type in use" 0 FAKE_SELECTED=1 -- install
want "$tmp/out" "until it is" "install beside the package's copy, type in use"

# 11. both copies: status shows both and which one Plasma uses
run "status, both copies" 0 FAKE_SHELL=down -- status
want "$tmp/out" "installed twice" "status, both copies"
want "$tmp/out" "yours, in use:  version $ours ($pkg)" "status, both copies"
want "$tmp/out" "the system's:   version 9.9.9 ($syspkg)" "status, both copies"
want "$tmp/out" "takes precedence" "status, both copies"

# 12. uninstall with both copies: only the user's goes and the desktops keep their type,
# in the session and without the shell (no desktop can be black: the type has its files)
: > "$tmp/log"
run "uninstall, both copies" 0 -- uninstall
want "$tmp/out" "Removed: your own copy" "uninstall, both copies"
want "$tmp/out" "The package's copy stays: $syspkg (version 9.9.9). Remove the package to remove it." "uninstall, both copies"
lacks "$tmp/log" "d.wallpaperPlugin" "uninstall, both copies: the desktops are left alone"
is "uninstall, both copies: the user's is gone" ! -e "$pkg"
is "uninstall, both copies: the package's is still there" -f "$syspkg/metadata.json"
run "install (3)" 0 -- install
run "uninstall, both copies, shell not running, type in use" 0 FAKE_SHELL=down -- uninstall
is "uninstall without the shell, both copies: the user's is gone" ! -e "$pkg"
is "uninstall without the shell, both copies: the package's is still there" -f "$syspkg/metadata.json"
run "status after it" 0 FAKE_SHELL=down -- status
want "$tmp/out" "the system's copy" "status after it"

# 13. the user's data directory in XDG_DATA_DIRS is not "the system's", and an entry
# that is no absolute path is not looked into
mv "$sysdir" "$tmp/sys-gone"
run "install (4)" 0 -- install
run "status, the user's copy only" 0 FAKE_SHELL=down -- status
want "$tmp/out" "installed, your own copy, version $ours ($pkg)" "status, the user's copy only"
mkdir -p "$tmp/cwd/relative/share/plasma/wallpapers"
cp -R "$src" "$tmp/cwd/relative/share/plasma/wallpapers/$type_id"
cd "$tmp/cwd" || exit 1
run "status from a directory with relative/share" 0 FAKE_SHELL=down -- status
cd / || exit 1
want "$tmp/out" "installed, your own copy" "a relative entry of XDG_DATA_DIRS"
printf '[Containments][1]\nwallpaperplugin=org.kde.image\n' > "$rc_file"
run "uninstall (4)" 0 FAKE_SHELL=down -- uninstall
want "$tmp/out" "Removed. walpe-daemon" "uninstall (4)"
is "uninstall (4): the package is gone" ! -e "$pkg"

# 14. the script without wallpaper/ next to it (a copy installed by itself): install
# says so and changes nothing; status works
mkdir -p "$tmp/alone"
cp "$script" "$tmp/alone/install.sh"
script_was=$script
script="$tmp/alone/install.sh"
run "install without wallpaper/" 1 -- install
want "$tmp/err" "no wallpaper/ next to this script" "install without wallpaper/"
is "install without wallpaper/: nothing installed" ! -e "$pkg"
run "status without wallpaper/" 0 FAKE_SHELL=down -- status
want "$tmp/out" "not installed" "status without wallpaper/"
script=$script_was

# 15. status outside Plasma, on a machine without KWin: every line says something (the
# session line was "session: ? on ?, ", ending in the comma)
mkdir -p "$tmp/bare"
for t in sh sed grep head dirname id; do ln -s "$(command -v $t)" "$tmp/bare/$t"; done
bare () {   # name, [VAR=value ...]: status with nothing of Plasma in PATH or the environment
    name=$1; shift
    # shellcheck disable=SC2086
    env -i PATH="$tmp/bare" HOME="$home" XDG_DATA_DIRS="$tmp/none" "$@" sh "$script" status > "$tmp/out" 2> "$tmp/err"
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
