#!/bin/sh
# linkage_test.sh <build dir> <wayland|nowayland> [x11|nox11]
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
#   (scene sound is off by default; engine/scenesound.c loads the library
#   when the first stream is opened).
# Exit 77 (skipped) without readelf / nm. No display, no daemon.
B=$1; WL=$2; X=${3:-x11}
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
n=$(needed "$B/walpe-daemon" | grep -c '^libpulse-simple')
check $([ "$n" = 0 ] && echo 1 || echo 0) "walpe-daemon: libpulse-simple is not NEEDED (loaded with the first sound stream)"
echo "linkage: $checks checks, $fail failed"
[ $fail = 0 ]
