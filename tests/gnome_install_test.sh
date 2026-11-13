#!/bin/sh
# gnome_install_test.sh <contrib/gnome/install.sh>
# `install.sh status` of the extension for GNOME Shell where there is no GNOME: nothing
# of GNOME in PATH or in the environment, then a stand-in gnome-shell. Every line says
# something (the session line was "session: ? on ?, GNOME Shell " with nothing after it),
# and in a GNOME session the line is the one it was. `uninstall` where nothing is
# installed says so. The home directory is the test's own; no setting is touched (there
# is no gsettings in the PATH the installer is given).
script=$1
fails=0
tmp=$(mktemp -d "${TMPDIR:-/tmp}/walpe-git.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bare" "$tmp/gnome" "$tmp/home"
for t in sh sed grep head tr dirname id; do ln -s "$(command -v $t)" "$tmp/bare/$t"; done
printf '#!/bin/sh\necho "GNOME Shell 46.0"\n' > "$tmp/gnome/gnome-shell"
chmod +x "$tmp/gnome/gnome-shell"

status () {   # name, PATH, [VAR=value ...]
    name=$1; path=$2; shift 2
    env -i PATH="$path" HOME="$tmp/home" "$@" sh "$script" status > "$tmp/out" 2> "$tmp/err"
    rc=$?
    if [ "$rc" != 0 ] || [ -s "$tmp/err" ] || grep -q -e '?' -e '[,:] *$' -e 'GNOME Shell *$' "$tmp/out"; then
        fails=$((fails + 1))
        echo "FAIL: $name: exit status $rc, or a line with '?', or one that ends in a comma, a colon or \"GNOME Shell\":"
        sed 's/^/    | /' "$tmp/out" "$tmp/err"
    fi
}
want () {   # fixed string, what
    if ! grep -F -q -- "$1" "$tmp/out"; then
        fails=$((fails + 1))
        echo "FAIL: $2: no '$1' in:"
        sed 's/^/    | /' "$tmp/out"
    fi
}

status "no session, no GNOME Shell" "$tmp/bare"
want "session: no desktop named (XDG_CURRENT_DESKTOP and XDG_SESSION_TYPE are not set), no GNOME Shell (gnome-shell was not found)" "no session, no GNOME Shell"
want "this is not a GNOME session" "no session, no GNOME Shell"
want "extension \"WalPE\": not installed" "no session, no GNOME Shell"
want "switched on: no" "no session, no GNOME Shell"
want "running: no" "no session, no GNOME Shell"

status "another desktop" "$tmp/bare" XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland
want "session: KDE on wayland, no GNOME Shell (gnome-shell was not found)" "another desktop"
want "this is not a GNOME session" "another desktop"

status "GNOME session" "$tmp/gnome:$tmp/bare" XDG_CURRENT_DESKTOP=ubuntu:GNOME XDG_SESSION_TYPE=wayland
want "session: ubuntu:GNOME on wayland, GNOME Shell 46.0" "GNOME session"
if grep -q "not a GNOME session" "$tmp/out"; then
    fails=$((fails + 1))
    echo "FAIL: GNOME session: called no GNOME session"
fi

# uninstall where nothing is installed: said, not "Removed"; where something is: removed
env -i PATH="$tmp/bare:$(dirname "$(command -v rm)")" HOME="$tmp/home" sh "$script" uninstall > "$tmp/out" 2> "$tmp/err"
rc=$?
want "is not installed" "uninstall, nothing installed"
if [ "$rc" != 0 ] || grep -q "Removed" "$tmp/out"; then
    fails=$((fails + 1))
    echo "FAIL: uninstall, nothing installed: exit status $rc, or it says \"Removed\""
fi
ext="$tmp/home/.local/share/gnome-shell/extensions/walpe@walpe.github.io"
mkdir -p "$ext"
echo '{}' > "$ext/metadata.json"
env -i PATH="$tmp/bare:$(dirname "$(command -v rm)")" HOME="$tmp/home" sh "$script" uninstall > "$tmp/out" 2> "$tmp/err"
want "Removed" "uninstall"
if [ -e "$ext" ]; then
    fails=$((fails + 1))
    echo "FAIL: uninstall: the extension is still there"
fi

if [ $fails != 0 ]; then
    echo "$fails check(s) failed"
    exit 1
fi
echo "gnome-install: ok"
