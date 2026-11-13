#!/bin/sh
# install.sh - the WalPE extension for GNOME Shell (Wayland), for the current user.
# No root, nothing is compiled; everything it does `uninstall` takes back.
#
#   ./install.sh install     copy the extension to ~/.local/share/gnome-shell/extensions and
#                            switch it on (again: update it)
#   ./install.sh enable      switch it on        (the same switch as in GNOME's "Extensions")
#   ./install.sh disable     switch it off
#   ./install.sh uninstall   switch it off and remove it
#   ./install.sh status      what is installed, switched on and running
#
# Needs GNOME Shell 46 or newer (run on 46 and 51) in a Wayland session, and of GNOME's own tools gsettings
# and (for status) gnome-extensions and gdbus. GNOME Shell reads a new or changed
# extension when it starts: after `install`, log out and in.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
uuid=walpe@walpe.github.io
data=${XDG_DATA_HOME:-$HOME/.local/share}
dest=$data/gnome-shell/extensions/$uuid
bus_name=io.github.walpe.Gnome

say() { printf '%s\n' "$*"; }
refuse() {
	say "install.sh: $*" >&2
	say "Nothing was changed." >&2
	exit 1
}

shell_version() { gnome-shell --version 2>/dev/null | sed -n 's/^GNOME Shell \([0-9][0-9.a-z]*\).*/\1/p' | head -n 1; }

# the extension has a use only where GNOME Shell (46 or newer) is the Wayland compositor
check_session() {
	case ":${XDG_CURRENT_DESKTOP:-}:" in
	*:GNOME:* | *:GNOME-Classic:* | *:ubuntu:* | *:Unity:*) ;;
	*) refuse "this is not a GNOME session (XDG_CURRENT_DESKTOP is \"${XDG_CURRENT_DESKTOP:-}\")." \
		"The extension is for GNOME Shell only; elsewhere WalPE needs none." ;;
	esac
	if [ "${XDG_SESSION_TYPE:-}" != wayland ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
		refuse "this GNOME session runs on X11. The extension is for GNOME on Wayland; on X11 WalPE needs none."
	fi
	command -v gsettings >/dev/null 2>&1 || refuse "gsettings was not found: it comes with GNOME (glib)."
	v=$(shell_version)
	[ -n "$v" ] || refuse "gnome-shell --version gave no version: is GNOME Shell installed?"
	major=${v%%.*}
	case "$major" in '' | *[!0-9]*) refuse "GNOME Shell's version \"$v\" was not understood." ;; esac
	if [ "$major" -lt 46 ]; then
		refuse "GNOME Shell $v is too old: the extension needs 46 or newer." \
			"On older versions use the session \"GNOME on Xorg\", where WalPE needs no extension."
	fi
}

# the session as its environment names it, "GNOME on wayland"; what is not named is said so
session_words() {
	d=${XDG_CURRENT_DESKTOP:-}
	t=${XDG_SESSION_TYPE:-}
	if [ -n "$d" ] && [ -n "$t" ]; then
		printf '%s\n' "$d on $t"
	elif [ -n "$d" ]; then
		printf '%s\n' "$d (XDG_SESSION_TYPE is not set)"
	elif [ -n "$t" ]; then
		printf '%s\n' "no desktop named (XDG_CURRENT_DESKTOP is not set) on $t"
	else
		printf '%s\n' "no desktop named (XDG_CURRENT_DESKTOP and XDG_SESSION_TYPE are not set)"
	fi
}

# the versions of GNOME Shell the extension lists (GNOME Shell loads it on those only)
listed() { sed -n 's/.*"shell-version"[^[]*\[\([^]]*\)\].*/\1/p' "$1/metadata.json" | tr -d '" '; }
lists_this_shell() {
	major=$(shell_version)
	major=${major%%.*}
	case ",$(listed "$1")," in *",$major,"*) return 0 ;; esac
	return 1
}

enabled_list() { gsettings get org.gnome.shell enabled-extensions 2>/dev/null || true; }
is_enabled() { enabled_list | grep -q "'$uuid'"; }
# the shell's own switch where the shell knows the extension, else the setting it reads at its start
switch() {
	if [ "$1" = on ]; then
		if command -v gnome-extensions >/dev/null 2>&1 && gnome-extensions enable "$uuid" 2>/dev/null; then
			return 0
		fi
		is_enabled && return 0
		cur=$(enabled_list)
		case "$cur" in
		'' | '@as []' | '[]') new="['$uuid']" ;;
		*) new="${cur%]}, '$uuid']" ;;
		esac
		gsettings set org.gnome.shell enabled-extensions "$new"
	else
		if command -v gnome-extensions >/dev/null 2>&1 && gnome-extensions disable "$uuid" 2>/dev/null; then
			return 0
		fi
		is_enabled || return 0
		new=$(enabled_list | sed "s/, '$uuid'//; s/'$uuid', //; s/'$uuid'//")
		[ "$new" = '[]' ] && new='@as []'
		gsettings set org.gnome.shell enabled-extensions "$new"
	fi
}
installed() { [ -f "$dest/metadata.json" ]; }
on_bus() {
	command -v gdbus >/dev/null 2>&1 || return 1
	gdbus call --session --timeout 3 --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
		--method org.freedesktop.DBus.NameHasOwner "$bus_name" 2>/dev/null | grep -q true
}

case "${1:-}" in
install)
	check_session
	was=no
	installed && was=yes
	mkdir -p "$dest"
	cp "$here/$uuid/metadata.json" "$here/$uuid/extension.js" "$dest/"
	switch on
	if [ "$was" = yes ]; then
		say "Updated: the extension \"WalPE\" ($dest)."
	else
		say "Installed and switched on: the extension \"WalPE\" ($dest)."
	fi
	if ! lists_this_shell "$dest"; then
		say "It lists GNOME Shell $(listed "$dest" | sed 's/,/, /g'), the versions it was run on, and this is"
		say "$(shell_version): GNOME Shell does not load it here unless its check of that list is off"
		say "(gsettings set org.gnome.shell disable-extension-version-validation true)."
	fi
	say "GNOME Shell reads it when it starts: log out and in. Then start walpe-daemon (or open"
	say "WalPE): the wallpaper is shown on the desktop."
	;;
enable)
	check_session
	installed || refuse "the extension is not installed yet: $0 install"
	switch on
	say "Switched on. Start walpe-daemon again if it is not showing a wallpaper."
	;;
disable)
	installed || refuse "the extension is not installed."
	switch off
	say "Switched off. A walpe-daemon that is running keeps its windows on the desktop; started"
	say "again it ends with a sentence about the missing extension (walpe-ctl quit stops it now)."
	;;
uninstall)
	# (where nothing was installed it said "Removed." all the same)
	if [ ! -e "$dest" ]; then
		say "The extension \"WalPE\" is not installed ($dest): nothing to remove."
		exit 0
	fi
	if installed; then switch off || true; fi
	rm -rf "$dest"
	say "Removed. On GNOME's Wayland session walpe-daemon shows no wallpaper again (walpe-ctl quit"
	say "stops one that still runs)."
	;;
status)
	# (without GNOME Shell and outside a session the line read "session: ? on ?, GNOME Shell ")
	v=$(shell_version)
	if [ -n "$v" ]; then
		v="GNOME Shell $v"
	else
		command -v gnome-shell >/dev/null 2>&1 && v="gnome-shell gave no version" || v="no GNOME Shell (gnome-shell was not found)"
	fi
	say "session: $(session_words), $v"
	case ":${XDG_CURRENT_DESKTOP:-}:" in
	*:GNOME:* | *:GNOME-Classic:* | *:ubuntu:* | *:Unity:*) ;;
	*) say "  this is not a GNOME session: the extension is for GNOME Shell on Wayland, elsewhere WalPE needs none" ;;
	esac
	if installed; then
		say "extension \"WalPE\": installed ($dest), lists GNOME Shell $(listed "$dest" | sed 's/,/, /g')"
		lists_this_shell "$dest" || say "  this version is not in that list: GNOME Shell does not load it"
	else
		say "extension \"WalPE\": not installed"
	fi
	is_enabled && say "switched on: yes" || say "switched on: no"
	if on_bus; then
		say "running: yes (it answers on the session bus as $bus_name)"
	else
		say "running: no (nothing answers on the session bus as $bus_name)"
		if installed && is_enabled; then
			say "  installed and switched on, not running: log out and in; \`gnome-extensions info $uuid\` shows an error it had"
		fi
	fi
	;;
*)
	say "usage: $0 install | enable | disable | uninstall | status"
	exit 2
	;;
esac
