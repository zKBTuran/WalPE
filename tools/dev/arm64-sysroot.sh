#!/bin/sh
# arm64-sysroot.sh - the arm64 libraries and headers a cross build of WalPE needs, on an
# x86-64 Ubuntu 24.04 (noble). The package list is made from the dependency() names in
# the tree's meson.build; arm64 packages come from the ports archive.
#
#   sh arm64-sysroot.sh list     --src DIR [--with-web] [--with-gl]
#        print the arm64 packages that would be fetched, and the dependency() names of
#        meson.build this script knows no package for; changes nothing, fetches nothing
#   sh arm64-sysroot.sh private  --src DIR --sysroot DIR [--with-web] [--with-gl] [--min-free-mb N]
#        a sysroot in DIR: the packages and everything they depend on are downloaded with
#        a package state of their own (below DIR/.apt) and unpacked into DIR. Nothing of
#        the system changes: no foreign architecture, no package source, no installed
#        package. Needs no root. Remove it by removing DIR. (apt reads its configuration
#        as it is: what an administrator made it run after every `apt-get update` - a
#        cache clean-up, a notification - runs after this one too.)
#        Nothing is unpacked outside DIR: a link in a package that would lead out of it
#        ends the run, and no unpacked file keeps a set-user-ID or set-group-ID bit.
#   sh arm64-sysroot.sh multiarch --src DIR [--with-web] [--with-gl] [--allow-upgrades] [--dry-run]
#        arm64 as a foreign architecture of the system itself (root): dpkg
#        --add-architecture arm64, the Ubuntu package sources limited to amd64, the ports
#        archive added for arm64, the packages installed as <name>:arm64 next to the
#        native ones. The "sysroot" is then / . Nothing is ever removed for it (apt-get
#        --no-remove), and the run stops before installing if native packages would have
#        to be upgraded to the archive's version (libraries installed for two
#        architectures must have the same version) unless --allow-upgrades is given.
#   sh arm64-sysroot.sh undo-multiarch
#        take the above back: the arm64 packages installed since removed, the ports
#        source removed, the sources files restored from the copies made, the foreign
#        architecture dropped. Only what multiarch added: arm64 packages that were
#        installed before its first run stay, and so does the architecture when it was
#        a foreign one already (multiarch writes both down in /var/lib/walpe-arm64-multiarch;
#        without that record undo-multiarch changes nothing). Native packages are never
#        removed: when taking an arm64 package away would take one along, it stops.
#
# --with-web adds WebKitGTK (large; the cross build is otherwise configured without web
# wallpapers). --with-gl adds Mesa's software renderer for arm64, so that tests which
# need a GL context can run under emulation instead of skipping themselves.
#
# The cross compiler and the emulator are native packages and are not installed here:
# gcc-aarch64-linux-gnu, qemu-user (see the package list in the documentation).
# tools/dev/arm64cross.py builds and tests against either kind of sysroot.
#
# The script prints what it does. It never prints the environment, and it leaves apt's
# own configuration (proxy settings and the like) to apt.
set -eu

SUITE=noble
PORTS=http://ports.ubuntu.com/ubuntu-ports/
KEYRING=/usr/share/keyrings/ubuntu-archive-keyring.gpg
PORTS_FILE=/etc/apt/sources.list.d/walpe-arm64-ports.sources
BACKUP_SUFFIX=.before-walpe-arm64
# what multiarch found before its first run (for undo-multiarch)
STATE_DIR=/var/lib/walpe-arm64-multiarch

die () { echo "arm64-sysroot: $*" >&2; exit 1; }
say () { echo "arm64-sysroot: $*"; }

usage () { sed -n '2,/^set -eu/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 2; }

# pkg-config module (as meson.build names it in dependency()) -> Ubuntu package.
# One "module package" pair per line; "-" = nothing to install for the host architecture
# (part of libc, or a tool of the build machine).
MODULES='
glib-2.0 libglib2.0-dev
gobject-2.0 libglib2.0-dev
gio-2.0 libglib2.0-dev
gio-unix-2.0 libglib2.0-dev
gmodule-2.0 libglib2.0-dev
gthread-2.0 libglib2.0-dev
gtk4 libgtk-4-dev
gtk4-x11 libgtk-4-dev
gtk4-wayland libgtk-4-dev
json-glib-1.0 libjson-glib-dev
gdk-pixbuf-2.0 libgdk-pixbuf-2.0-dev
pango libpango1.0-dev
pangocairo libpango1.0-dev
pangoft2 libpango1.0-dev
cairo libcairo2-dev
fontconfig libfontconfig-dev
freetype2 libfreetype-dev
harfbuzz libharfbuzz-dev
epoxy libepoxy-dev
egl libegl-dev
glesv2 libgles-dev
gl libgl-dev
gbm libgbm-dev
libdrm libdrm-dev
x11 libx11-dev
xext libxext-dev
xinerama libxinerama-dev
xi libxi-dev
xrandr libxrandr-dev
xrender libxrender-dev
xfixes libxfixes-dev
xcomposite libxcomposite-dev
xdamage libxdamage-dev
xcb libxcb1-dev
x11-xcb libx11-xcb-dev
wayland-client libwayland-dev
wayland-cursor libwayland-dev
wayland-egl libwayland-dev
wayland-server libwayland-dev
wayland-scanner -
wayland-protocols wayland-protocols
xkbcommon libxkbcommon-dev
liblz4 liblz4-dev
zlib zlib1g-dev
libpng libpng-dev
libavcodec libavcodec-dev
libavutil libavutil-dev
libavformat libavformat-dev
libswscale libswscale-dev
libva libva-dev
libva-drm libva-dev
libva-x11 libva-dev
libva-wayland libva-dev
vdpau libvdpau-dev
libpulse libpulse-dev
libpulse-simple libpulse-dev
libpipewire-0.3 libpipewire-0.3-dev
libsoup-3.0 libsoup-3.0-dev
webkitgtk-6.0 libwebkitgtk-6.0-dev
dbus-1 libdbus-1-dev
libudev libudev-dev
threads -
dl -
'

# the packages for the tree in $1, one per line (web left out unless $2 = yes)
packages_for () {
    src=$1 web=$2 gl=$3
    [ -f "$src/meson.build" ] || die "no meson.build in $src (give the source tree with --src)"
    # every quoted name in the build files: dependency('x') and the lists dependency() is called over
    names=$(find "$src" -name meson.build -not -path "$src/build*/*" -exec cat {} + |
            tr -c "A-Za-z0-9_.+'-" '\n' | sed -n "s/^'\\([A-Za-z0-9_.+-]*\\)'\$/\\1/p" | sort -u)
    {
        echo libc6-dev
        echo linux-libc-dev
        for n in $names; do
            p=$(echo "$MODULES" | awk -v n="$n" '$1 == n { print $2; exit }')
            case "$p" in ''|-) continue ;; esac
            [ "$p" = libwebkitgtk-6.0-dev ] && [ "$web" != yes ] && continue
            echo "$p"
        done
        # QuickJS has no pkg-config file: meson.build looks for the library itself
        if find "$src" -name meson.build -not -path "$src/build*/*" -exec grep -q "find_library *( *'quickjs'" {} +; then
            echo libquickjs
        fi
        if [ "$gl" = yes ]; then
            echo libgl1-mesa-dri; echo libegl-mesa0; echo libegl1; echo libgles2; echo libgbm1
        fi
    } | sort -u
}

# dependency('x') names without a line in MODULES
unknown_modules () {
    find "$1" -name meson.build -not -path "$1/build*/*" -exec cat {} + |
        grep -o "dependency *( *'[A-Za-z0-9_.+-]*'" | sed "s/.*'\\(.*\\)'/\\1/" | sort -u |
        while read -r n; do
            echo "$MODULES" | awk -v n="$n" '$1 == n { f = 1 } END { exit !f }' || echo "$n"
        done
}

free_mb () { df -Pm "$1" | awk 'NR == 2 { print $4 }'; }

# the arm64 packages dpkg has installed, one name:arm64 per line
arm64_installed () {
    dpkg-query -W -f '${Package}:${Architecture} ${db:Status-Abbrev}\n' 2>/dev/null |
        awk '$1 ~ /:arm64$/ && $2 ~ /^i/ { print $1 }' | sort -u
}

mode=${1:-}
[ -n "$mode" ] || usage
shift
src= sysroot= web=no gl=no upgrades=no dry=no minfree=4000
while [ $# -gt 0 ]; do
    case "$1" in
        --src|--sysroot|--min-free-mb) [ $# -ge 2 ] || usage ;;
    esac
    case "$1" in
        --src) src=$2; shift 2 ;;
        --sysroot) sysroot=$2; shift 2 ;;
        --min-free-mb) minfree=$2; shift 2 ;;
        --with-web) web=yes; shift ;;
        --with-gl) gl=yes; shift ;;
        --allow-upgrades) upgrades=yes; shift ;;
        --dry-run) dry=yes; shift ;;
        *) usage ;;
    esac
done

case "$mode" in
list)
    [ -n "$src" ] || usage
    # (asked here too: in a pipe the exit of packages_for would be lost)
    [ -f "$src/meson.build" ] || die "no meson.build in $src (give the source tree with --src)"
    packages_for "$src" "$web" "$gl" | sed 's/$/:arm64/'
    u=$(unknown_modules "$src")
    [ -z "$u" ] || { echo; echo "dependency() names without a package in this script's table:"; echo "$u" | sed 's/^/  /'; }
    ;;

private)
    [ -n "$src" ] && [ -n "$sysroot" ] || usage
    [ "$(dpkg --print-architecture)" = amd64 ] || die "written for an amd64 machine (this is $(dpkg --print-architecture))"
    [ -r "$KEYRING" ] || die "$KEYRING is missing (package ubuntu-keyring)"
    pkgs=$(packages_for "$src" "$web" "$gl")
    case "$minfree" in ''|*[!0-9]*) die "--min-free-mb takes a number" ;; esac
    mkdir -p "$sysroot"
    # (the real path: the links inside are compared with it further down)
    sysroot=$(cd "$sysroot" && pwd -P)
    if [ -n "$(ls -A "$sysroot")" ] && [ ! -f "$sysroot/.walpe-arm64-sysroot" ]; then
        die "$sysroot is not empty and was not made by this script"
    fi
    f=$(free_mb "$sysroot")
    [ "$f" -ge "$minfree" ] || die "$f MB free in $sysroot, $minfree MB wanted (--min-free-mb)"
    : > "$sysroot/.walpe-arm64-sysroot"
    apt=$sysroot/.apt
    mkdir -p "$apt/state/lists/partial" "$apt/cache/archives/partial" "$apt/etc"
    : > "$apt/status"
    cat > "$apt/etc/sources.list" <<EOF
deb [arch=arm64 signed-by=$KEYRING] $PORTS $SUITE main universe
deb [arch=arm64 signed-by=$KEYRING] $PORTS $SUITE-updates main universe
deb [arch=arm64 signed-by=$KEYRING] $PORTS $SUITE-security main universe
EOF
    # apt with a package state of its own: arm64 is its only architecture, nothing is
    # installed as far as it knows, the system's dpkg is not asked and not locked
    papt () {
        apt-get -o Dir::Etc::sourcelist="$apt/etc/sources.list" -o Dir::Etc::sourceparts=/dev/null \
                -o Dir::State="$apt/state" -o Dir::State::status="$apt/status" -o Dir::Cache="$apt/cache" \
                -o APT::Architecture=arm64 -o APT::Architectures=arm64 \
                -o APT::Install-Recommends=false -o Debug::NoLocking=true "$@"
    }
    say "package lists for arm64 from $PORTS ($SUITE, -updates, -security)"
    papt update
    say "downloading $(echo "$pkgs" | wc -l) packages and what they depend on"
    # shellcheck disable=SC2086
    papt install --download-only --yes $pkgs
    n=$(find "$apt/cache/archives" -maxdepth 1 -name '*.deb' | wc -l)
    [ "$n" -gt 0 ] || die "nothing was downloaded"
    say "unpacking $n packages into $sysroot"
    # merged /usr, as base-files would set it up
    mkdir -p "$sysroot/usr/bin" "$sysroot/usr/sbin" "$sysroot/usr/lib" "$sysroot/usr/lib64"
    for d in bin sbin lib lib64; do
        [ -e "$sysroot/$d" ] || ln -s "usr/$d" "$sysroot/$d"
    done
    # links that name a place from the root must name it from inside the sysroot
    relink () {
        find "$sysroot" -type l -lname '/*' | while IFS= read -r link; do
            target=$(readlink "$link")
            rel=$(dirname "${link#"$sysroot"}" | sed 's|/[^/][^/]*|/..|g; s|^/||')
            ln -sfn "${rel:-.}$target" "$link"
        done
    }
    # the links that end outside the sysroot (the first few; nothing = none does)
    leading_out () {
        # (one realpath for all links says whether any does; only then are they looked
        # at one by one for their names)
        find "$sysroot" -type l -exec realpath -m -- {} + 2>/dev/null |
            ROOT=$sysroot awk '$0 != ENVIRON["ROOT"] && index($0, ENVIRON["ROOT"] "/") != 1 { f = 1 }
                               END { exit !f }' || return 0
        find "$sysroot" -type l | while IFS= read -r link; do
            dest=$(realpath -m -- "$link" 2>/dev/null) || continue
            case "$dest" in
                "$sysroot"|"$sysroot"/*) ;;
                *) echo "${link#"$sysroot"/} -> $(readlink "$link")" ;;
            esac
        done | head -n 5
    }
    for deb in "$apt/cache/archives"/*.deb; do
        # (a package that still ships /lib or /bin as a directory goes through the links:
        # tar follows a link to a directory here)
        dpkg-deb --fsys-tarfile "$deb" | tar -x -C "$sysroot" --keep-directory-symlink --no-same-owner
        # ... which is why no link may lead out of the sysroot when the next package is
        # unpacked: /abs/dir of one package and dir/file of the next would otherwise be
        # written to /abs/dir of this machine. Links from the root are turned inwards
        # after every package, and a link that still leaves the sysroot ends the run.
        relink
        out=$(leading_out)
        [ -z "$out" ] ||
            die "$(basename "$deb"): a link leads out of the sysroot ($(echo "$out" | tr '\n' ';')) - stopped, the rest is not unpacked"
    done
    rm -f "$apt/cache/archives"/*.deb
    # nothing in a sysroot has to run with another account's rights (and an emulator
    # registered with the kernel would run such a file with them)
    find "$sysroot" -type f -perm /6000 -exec chmod ug-s {} +
    # what a build does not read
    rm -rf "$sysroot/usr/share/doc" "$sysroot/usr/share/man" "$sysroot/usr/share/locale" \
           "$sysroot/usr/share/icons" "$sysroot/usr/share/help" "$sysroot/usr/share/info"
    say "done: $(du -sm "$sysroot" | awk '{ print $1 }') MB in $sysroot"
    u=$(unknown_modules "$src")
    [ -z "$u" ] || { say "dependency() names without a package in this script's table (not fetched):"; echo "$u" | sed 's/^/  /'; }
    ;;

multiarch)
    [ -n "$src" ] || usage
    [ "$(id -u)" = 0 ] || die "multiarch changes the system's package setup: run it as root"
    [ "$(dpkg --print-architecture)" = amd64 ] || die "written for an amd64 machine"
    pkgs=$(packages_for "$src" "$web" "$gl")
    # a library and its header at paths without the architecture in them cannot be
    # installed for two architectures: the native one stays
    if dpkg -s libquickjs >/dev/null 2>&1; then
        pkgs=$(echo "$pkgs" | grep -v '^libquickjs$')
        say "libquickjs is installed for amd64 and cannot be installed for arm64 next to it:"
        say "  the cross build gets -Dscenescript=disabled (a private sysroot has no such limit)"
    fi
    list=$(echo "$pkgs" | sed 's/$/:arm64/' | tr '\n' ' ')
    if [ "$dry" = yes ]; then
        say "would: dpkg --add-architecture arm64; limit the Ubuntu sources to amd64; write $PORTS_FILE;"
        say "       apt-get update; apt-get install --no-remove --no-install-recommends $list"
        exit 0
    fi
    # written down once, before anything changes: undo-multiarch takes back only the rest
    if [ ! -f "$STATE_DIR/arm64-before" ]; then
        mkdir -p "$STATE_DIR"
        if dpkg --print-foreign-architectures | grep -q -x arm64; then echo yes; else echo no; fi \
            > "$STATE_DIR/foreign-before"
        arm64_installed > "$STATE_DIR/arm64-before"
    fi
    dpkg --add-architecture arm64
    # sources that name no architecture would be asked for arm64 too, and have none
    for f in /etc/apt/sources.list.d/ubuntu.sources; do
        [ -f "$f" ] || continue
        if ! grep -q '^Architectures:' "$f"; then
            cp -p "$f" "$f$BACKUP_SUFFIX"
            sed -i 's/^Types: deb.*/&\nArchitectures: amd64/' "$f"
            say "$f: limited to amd64 (copy: $f$BACKUP_SUFFIX)"
        fi
    done
    f=/etc/apt/sources.list
    if [ -f "$f" ] && grep -q '^deb  *[a-z]' "$f"; then
        cp -p "$f" "$f$BACKUP_SUFFIX"
        sed -i 's/^deb  *\([a-z]\)/deb [arch=amd64] \1/' "$f"
        say "$f: limited to amd64 (copy: $f$BACKUP_SUFFIX)"
    fi
    for f in /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
        [ -f "$f" ] && [ "$f" != "$PORTS_FILE" ] || continue
        grep -q 'arch=\|^Architectures:' "$f" ||
            say "note: $f names no architecture; apt-get update may report that it has nothing for arm64"
    done
    cat > "$PORTS_FILE" <<EOF
# arm64 packages for cross builds (written by tools/dev/arm64-sysroot.sh)
Types: deb
URIs: $PORTS
Suites: $SUITE $SUITE-updates $SUITE-security
Components: main universe
Architectures: arm64
Signed-By: $KEYRING
EOF
    say "wrote $PORTS_FILE"
    apt-get update
    # shellcheck disable=SC2086
    plan=$(apt-get -s install --no-remove --no-install-recommends $list) ||
        die "apt cannot install the arm64 packages without removing something; nothing was installed (undo: $0 undo-multiarch)"
    if echo "$plan" | grep -q '^Remv '; then
        die "the plan removes packages; nothing was installed (undo: $0 undo-multiarch)"
    fi
    up=$(echo "$plan" | grep '^Inst [^ ]* \[' | grep -v '^Inst [^ ]*:arm64 ' || true)
    if [ -n "$up" ] && [ "$upgrades" != yes ]; then
        say "installing these for arm64 needs the same version for amd64, so these native packages would be upgraded:"
        echo "$up" | awk '{ print "  " $2, $3, "->", $4 }' | tr -d '()'
        die "nothing was installed. Run again with --allow-upgrades to accept, or use the private mode"
    fi
    # shellcheck disable=SC2086
    apt-get install --yes --no-remove --no-install-recommends $list
    say "done: arm64 libraries are in /usr/lib/aarch64-linux-gnu"
    u=$(unknown_modules "$src")
    [ -z "$u" ] || { say "dependency() names without a package in this script's table (not installed):"; echo "$u" | sed 's/^/  /'; }
    ;;

undo-multiarch)
    [ "$(id -u)" = 0 ] || die "run it as root"
    [ -f "$STATE_DIR/arm64-before" ] && [ -f "$STATE_DIR/foreign-before" ] ||
        die "no record of a multiarch run of this script in $STATE_DIR: nothing is taken back"
    # the arm64 packages installed since the first multiarch run; one that was there before stays
    gone=$(arm64_installed | grep -v -x -F -f "$STATE_DIR/arm64-before" | tr '\n' ' ')
    if [ -n "$(echo "$gone" | tr -d ' ')" ]; then
        # shellcheck disable=SC2086
        plan=$(apt-get -s remove --purge $gone) ||
            die "apt cannot remove the arm64 packages; nothing was changed"
        other=$(echo "$plan" | awk '($1 == "Remv" || $1 == "Purg") && $2 !~ /:arm64$/ { print $2 }' |
                sort -u | tr '\n' ' ')
        [ -z "$other" ] ||
            die "removing the arm64 packages would also remove native ones ($other); nothing was changed"
        say "removing the arm64 packages installed since"
        # shellcheck disable=SC2086
        apt-get remove --yes --purge $gone
    fi
    rm -f "$PORTS_FILE"
    for f in /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list; do
        [ -f "$f$BACKUP_SUFFIX" ] && { mv "$f$BACKUP_SUFFIX" "$f"; say "$f restored"; }
    done
    if [ "$(cat "$STATE_DIR/foreign-before")" = no ]; then
        dpkg --remove-architecture arm64 ||
            say "dpkg keeps arm64 as an architecture (packages of it are still installed)"
    else
        say "arm64 was a foreign architecture before: it stays one"
    fi
    rm -f "$STATE_DIR/arm64-before" "$STATE_DIR/foreign-before"
    rmdir "$STATE_DIR" 2>/dev/null || true
    apt-get update
    say "done"
    ;;

*) usage ;;
esac
