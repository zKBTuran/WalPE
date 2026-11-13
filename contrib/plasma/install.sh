#!/bin/sh
# install.sh - the WalPE add-on for KDE Plasma 6 on Wayland, for the current user.
# No root, nothing is compiled; everything it does `uninstall` takes back.
#
#   ./install.sh install     copy the wallpaper type "WalPE" to ~/.local/share (again: update it)
#   ./install.sh select      "WalPE" as the wallpaper type of every desktop; the same as
#                            choosing it in Plasma's "Desktop and Wallpaper" dialog
#   ./install.sh unselect    every desktop back to the wallpaper type it had before
#   ./install.sh uninstall   unselect and remove (in the Plasma session: Plasma's shell has
#                            to be there to give the desktops their wallpaper type back)
#   ./install.sh status      what is installed and selected
#
# Needs Plasma 6.5 or newer (KWin's workspace.constrain) in a Wayland session, and of
# Plasma's own tools kpackagetool6 and dbus-send.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
type_id=io.github.walpe.plasma
data=${XDG_DATA_HOME:-$HOME/.local/share}
config=${XDG_CONFIG_HOME:-$HOME/.config}

say() { printf '%s\n' "$*"; }
refuse() {
	say "install.sh: $*" >&2
	say "Nothing was changed." >&2
	exit 1
}

# the add-on has a use only where KWin (6.5 or newer) is the Wayland compositor
check_session() {
	case ":${XDG_CURRENT_DESKTOP:-}:" in
	*:KDE:*) ;;
	*) refuse "this is not a KDE Plasma session (XDG_CURRENT_DESKTOP is \"${XDG_CURRENT_DESKTOP:-}\")." \
		"The add-on is for Plasma only; elsewhere WalPE needs none." ;;
	esac
	if [ "${XDG_SESSION_TYPE:-}" != wayland ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
		refuse "this Plasma session runs on X11. The add-on is for Plasma on Wayland; on X11 WalPE needs none."
	fi
	for tool in kpackagetool6 dbus-send; do
		command -v "$tool" >/dev/null 2>&1 || refuse "$tool was not found: it comes with Plasma 6 (dbus-send: with D-Bus)."
	done
	v=$(kwin_wayland --version 2>/dev/null | sed -n 's/^kwin \([0-9][0-9.]*\).*/\1/p' | head -n 1)
	[ -n "$v" ] || refuse "kwin_wayland --version gave no version: is this Plasma 6?"
	major=${v%%.*}
	rest=${v#*.}
	minor=${rest%%.*}
	case "$major$minor" in *[!0-9]*) refuse "KWin's version \"$v\" was not understood." ;; esac
	if [ "$major" -lt 6 ] || { [ "$major" -eq 6 ] && [ "$minor" -lt 5 ]; }; then
		refuse "KWin $v is too old: the add-on needs Plasma 6.5 or newer." \
			"On older versions WalPE shows the wallpaper over Plasma's desktop icons (docs/WAYLAND.md, \"KDE Plasma\")."
	fi
}

# the session as its environment names it, "KDE on wayland"; what is not named is said so
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

# plasma_js SCRIPT: run SCRIPT in Plasma's shell (its desktop scripting), print what it prints
plasma_js() {
	dbus-send --session --print-reply=literal --dest=org.kde.plasmashell /PlasmaShell \
		org.kde.PlasmaShell.evaluateScript "string:$1"
}
plasma_runs() {
	dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
		org.freedesktop.DBus.NameHasOwner string:org.kde.plasmashell 2>/dev/null | grep -q 'boolean true'
}
installed() { [ -f "$data/plasma/wallpapers/$type_id/metadata.json" ]; }
# what Plasma's shell wrote down: a desktop (of any activity) has the type. The file is a
# few seconds behind the shell, and it is all there is to go by while the shell is not running.
written_selected() { grep -qx "wallpaperplugin=$type_id" "$config/plasma-org.kde.plasma.desktop-appletsrc" 2>/dev/null; }
# sudo with the user's environment (sudo -E) would leave root's files in the user's home,
# which the user can then neither update nor remove
own_home() {
	[ -O "$HOME" ] || refuse "$HOME is not the home directory of the user this runs as ($(id -un))." \
		"Run it as the user of the Plasma session, without sudo: it needs no root."
}
selected() { plasma_js "for (const d of desktops()) { if (d.wallpaperPlugin === '$type_id') print('yes'); }" 2>/dev/null | grep -q yes; }

do_unselect() {
	plasma_js "for (const d of desktops()) {
		if (d.wallpaperPlugin !== '$type_id') continue;
		d.currentConfigGroup = ['Wallpaper', '$type_id', 'General'];
		const before = String(d.readConfig('Previous', ''));
		d.wallpaperPlugin = before && before !== '$type_id' ? before : 'org.kde.image';
		print('screen ' + d.screen + ': wallpaper type ' + d.wallpaperPlugin + '\\n');
	}"
}

case "${1:-}" in
install)
	check_session
	own_home
	if kpackagetool6 --type Plasma/Wallpaper --show "$type_id" >/dev/null 2>&1; then
		kpackagetool6 --type Plasma/Wallpaper --upgrade "$here/wallpaper" >/dev/null
		say "Updated: the wallpaper type \"WalPE\"."
		if plasma_runs && selected; then
			say "It is in use: Plasma's shell goes on with the version it has loaded until it is"
			say "restarted (log out and in, or: plasmashell --replace)."
		fi
	else
		kpackagetool6 --type Plasma/Wallpaper --install "$here/wallpaper" >/dev/null
		say "Installed: the wallpaper type \"WalPE\"."
	fi
	say "Choose it: right-click the desktop > Desktop and Wallpaper > Wallpaper type: WalPE,"
	say "or: $0 select"
	;;
select)
	check_session
	installed || refuse "the add-on is not installed yet: $0 install"
	plasma_runs || refuse "Plasma's shell (plasmashell) is not running."
	# each desktop remembers its type for `unselect`, and the picture of Plasma's "Image"
	# type becomes what the desktop shows while walpe-daemon shows nothing
	plasma_js "for (const d of desktops()) {
		const before = d.wallpaperPlugin;
		if (before === '$type_id') { print('screen ' + d.screen + ': WalPE already\\n'); continue; }
		d.currentConfigGroup = ['Wallpaper', 'org.kde.image', 'General'];
		const picture = String(d.readConfig('Image', ''));
		d.currentConfigGroup = ['Wallpaper', '$type_id', 'General'];
		d.writeConfig('Previous', before);
		if (picture && !String(d.readConfig('Image', ''))) d.writeConfig('Image', picture);
		d.wallpaperPlugin = '$type_id';
		print('screen ' + d.screen + ': wallpaper type WalPE (was ' + before + ')\\n');
	}"
	say "Start walpe-daemon and open a wallpaper: it shows behind the icons."
	;;
unselect)
	plasma_runs || refuse "Plasma's shell (plasmashell) is not running."
	do_unselect
	;;
uninstall)
	# A desktop that keeps the type after its files are gone has no wallpaper at all: Plasma
	# shows it black behind the icons. So the type is taken off the desktops first, and
	# where that cannot be done nothing is removed.
	if plasma_runs; then
		do_unselect || refuse "Plasma's shell did not take the wallpaper type off its desktops (see above)."
	elif written_selected; then
		refuse "a desktop still has the wallpaper type \"WalPE\", and Plasma's shell (plasmashell) is not" \
			"running here to give it another one: without the type's files that desktop would be black." \
			"Run this in the Plasma session, or choose another wallpaper type there first."
	fi
	if installed && command -v kpackagetool6 >/dev/null 2>&1; then
		kpackagetool6 --type Plasma/Wallpaper --remove "$type_id" >/dev/null || true
	fi
	rm -rf "$data/plasma/wallpapers/$type_id"
	say "Removed. walpe-daemon shows its wallpaper over Plasma's desktop again (the icons are hidden while it runs)."
	;;
status)
	# (outside a Plasma session the line read "session: ? on ?, " and ended in the comma)
	kwin=$(kwin_wayland --version 2>/dev/null | head -n 1 || true)
	if [ -z "$kwin" ]; then
		command -v kwin_wayland >/dev/null 2>&1 && kwin="kwin_wayland gave no version" || kwin="no KWin (kwin_wayland was not found)"
	fi
	say "session: $(session_words), $kwin"
	case ":${XDG_CURRENT_DESKTOP:-}:" in
	*:KDE:*) ;;
	*) say "  this is not a KDE Plasma session: the add-on is for Plasma on Wayland, elsewhere WalPE needs none" ;;
	esac
	installed && say "wallpaper type \"WalPE\": installed ($data/plasma/wallpapers/$type_id)" ||
		say "wallpaper type \"WalPE\": not installed"
	if plasma_runs; then
		plasma_js "for (const d of desktops()) {
			if (d.screen < 0) continue;
			print('screen ' + d.screen + ': wallpaper type ' + (d.wallpaperPlugin === '$type_id' ? 'WalPE' : d.wallpaperPlugin) + '\\n');
		}" || say "Plasma's shell did not answer on D-Bus"
	else
		say "Plasma's shell (plasmashell) is not running"
	fi
	;;
*)
	say "usage: $0 install | select | unselect | uninstall | status"
	exit 2
	;;
esac
