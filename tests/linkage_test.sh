#!/bin/sh
# linkage_test.sh <build dir> <wayland|nowayland> [x11|nox11] [modules.dynlist]
# What the built programs make the dynamic loader load, read from the files
# themselves (readelf -d, nm -D): a part that is not built must not leave a
# library behind in walpe-daemon or in the web module.
#   nowayland (-Dwayland=disabled): neither file names a libwayland-*
#   library as NEEDED or has an undefined wl_* symbol.
#   nox11 (no X11 backend and no X11 page host: -Dx11=disabled, or a system
#   without libX11): neither file names an X library (libX*, libxcb*) as
#   NEEDED, has an undefined Xlib / XCB function, or - the daemon - defines
#   XSetSelectionOwner (the selection guard of the X11 page host). What GTK
#   and WebKit load by themselves is their business and is not looked at.
#   every build: walpe-daemon does not name libpulse-simple as NEEDED
#   (engine/scenesound_pulse.c makes the calls of wallpaper sound with
#   libpulse itself; the build asks for libpulse-simple for its header).
#   every build, with the list as fourth argument: a function that
#   walpe-web.so or walpe-video.so takes from the program that loads it is
#   named in src/daemon/modules.dynlist, and what the list names of the
#   daemon's functions is exported. The daemon exports what the list names
#   and nothing else of its own - unless a library's pkg-config file puts
#   -Wl,--export-dynamic on the link line (libsoup-3.0 through gmodule-2.0
#   on Ubuntu 24.04; not on Debian 13, not on Alpine 3.24): there every
#   function marked WALPE_API is exported, a module loads without its
#   function in the list, and the same tree fails to load it elsewhere.
# Exit 77 (skipped) without readelf / nm. No display, no daemon.
B=$1; WL=$2; X=${3:-x11}; L=${4:-}
command -v readelf >/dev/null 2>&1 && command -v nm >/dev/null 2>&1 || { echo "linkage: no readelf / nm: skipped"; exit 77; }
[ -f "$B/walpe-daemon" ] || { echo "linkage: $B/walpe-daemon is not there"; exit 1; }
fail=0; checks=0
needed () { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
check () {   # <ok: 0|1> <text>
    checks=$((checks + 1))
    if [ "$1" = 1 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=$((fail + 1)); fi
}
for f in walpe-daemon walpe-web.so; do
    [ -f "$B/$f" ] || continue
    if [ "$WL" = nowayland ]; then
        n=$(needed "$B/$f" | grep -c '^libwayland-')
        check $([ "$n" = 0 ] && echo 1 || echo 0) "$f, no Wayland backend: no libwayland-* library is NEEDED ($(needed "$B/$f" | grep '^libwayland-' | tr '\n' ' '))"
        u=$(nm -D --undefined-only "$B/$f" | grep -c ' wl_')
        check $([ "$u" = 0 ] && echo 1 || echo 0) "$f, no Wayland backend: no undefined wl_* symbol ($u)"
    fi
    if [ "$X" = nox11 ]; then
        n=$(needed "$B/$f" | grep -c '^libX\|^libxcb')
        check $([ "$n" = 0 ] && echo 1 || echo 0) "$f, no X11 backend and no X11 page host: no X library is NEEDED ($(needed "$B/$f" | grep '^libX\|^libxcb' | tr '\n' ' '))"
        # (Xlib: XOpenDisplay, XInternAtom, Xutf8...; extensions: XI..., XRR..., Xinerama..., XShape..., DPMS...; XCB)
        u=$(nm -D --undefined-only "$B/$f" | awk '{ print $NF }' | grep -c '^X[A-Za-z]\|^xcb_\|^DPMS')
        check $([ "$u" = 0 ] && echo 1 || echo 0) "$f, no X11 backend and no X11 page host: no undefined X function ($u: $(nm -D --undefined-only "$B/$f" | awk '{ print $NF }' | grep '^X[A-Za-z]\|^xcb_\|^DPMS' | head -3 | tr '\n' ' '))"
    fi
done
if [ "$X" = nox11 ]; then
    d=$(nm -D --defined-only "$B/walpe-daemon" | grep -c ' XSetSelectionOwner$')
    check $([ "$d" = 0 ] && echo 1 || echo 0) "walpe-daemon, no X11 page host: the selection guard (XSetSelectionOwner) is not in it"
fi
if [ -n "$L" ]; then
    [ -f "$L" ] || { echo "linkage: $L is not there"; exit 1; }
    # the names of the list, its comments left out
    names=$(awk 'BEGIN { RS = "\\*/" } { sub(/\/\*.*/, ""); print }' "$L" | tr -c 'A-Za-z0-9_' '\n' | grep . | sort -u)
    # the daemon's functions: all of them (T, t; a stripped file has only the
    # exported ones), and the ones other files may see at all (T: WALPE_API)
    all=$(nm --defined-only "$B/walpe-daemon" 2>/dev/null | awk '$2 == "T" || $2 == "t" { print $3 }')
    [ -n "$all" ] || all=$(nm -D --defined-only "$B/walpe-daemon" | awk '$2 == "T" { print $3 }')
    open=$(nm --defined-only "$B/walpe-daemon" 2>/dev/null | awk '$2 == "T" { print $3 }')
    [ -n "$open" ] || open=$all
    exported=$(nm -D --defined-only "$B/walpe-daemon" | awk '{ print $NF }')
    for m in walpe-web.so walpe-video.so; do
        [ -f "$B/$m" ] || continue
        miss=""; took=0
        for s in $(nm -D --undefined-only "$B/$m" | awk '{ print $NF }' | sed 's/@.*//'); do
            echo "$open" | grep -q -x -F -- "$s" || continue    # a library's, not the daemon's
            took=$((took + 1))
            echo "$names" | grep -q -x -F -- "$s" || miss="$miss $s"
        done
        check $([ -z "$miss" ] && [ $took -gt 0 ] && echo 1 || echo 0) "$m: the $took functions it takes from the daemon are in modules.dynlist (missing:${miss:- none})"
    done
    miss=""
    for s in $names; do
        echo "$all" | grep -q -x -F -- "$s" || continue         # not in this build (the selection guard without X11)
        echo "$exported" | grep -q -x -F -- "$s" || miss="$miss $s"
    done
    check $([ -z "$miss" ] && echo 1 || echo 0) "walpe-daemon: what modules.dynlist names of its functions is exported (not exported:${miss:- none})"
fi
n=$(needed "$B/walpe-daemon" | grep -c '^libpulse-simple')
check $([ "$n" = 0 ] && echo 1 || echo 0) "walpe-daemon: libpulse-simple is not NEEDED (wallpaper sound calls libpulse itself)"
echo "linkage: $checks checks, $fail failed"
[ $fail = 0 ]
