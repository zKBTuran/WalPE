#!/bin/sh
# Installs (or with "remove": removes) the two Plasma 6 parts for the current user. No root, no build.
#   wallpaper/    Plasma wallpaper type "WalPE": draws nothing, makes the desktop window see-through
#   kwin-script/  KWin script: keeps walpe-daemon's surface under the desktop window (KWin 6.5+)
# Then: right-click the desktop > Desktop and Wallpaper > Wallpaper type: WalPE  (or "select" below).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
wp=io.github.walpe.plasma
ks=io.github.walpe.below
qd=$(command -v qdbus6 || command -v qdbus || true)
case "${1:-install}" in
install)
	kpackagetool6 --type Plasma/Wallpaper -u "$here/wallpaper" 2>/dev/null || kpackagetool6 --type Plasma/Wallpaper -i "$here/wallpaper"
	kpackagetool6 --type KWin/Script -u "$here/kwin-script" 2>/dev/null || kpackagetool6 --type KWin/Script -i "$here/kwin-script"
	kwriteconfig6 --file kwinrc --group Plugins --key "${ks}Enabled" true
	[ -z "$qd" ] || "$qd" org.kde.KWin /KWin org.kde.KWin.reconfigure >/dev/null 2>&1 || true
	echo "installed. Select it: $0 select   (or in the desktop's wallpaper settings)"
	;;
select)
	"$qd" org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.evaluateScript \
		"for (const d of desktops()) { d.wallpaperPlugin = '$wp'; }"
	;;
remove)
	kwriteconfig6 --file kwinrc --group Plugins --key "${ks}Enabled" --delete
	[ -z "$qd" ] || "$qd" org.kde.KWin /KWin org.kde.KWin.reconfigure >/dev/null 2>&1 || true
	kpackagetool6 --type KWin/Script -r "$ks" || true
	kpackagetool6 --type Plasma/Wallpaper -r "$wp" || true
	echo "removed. Choose another wallpaper type in the desktop's wallpaper settings."
	;;
*) echo "usage: $0 [install|select|remove]"; exit 2 ;;
esac
