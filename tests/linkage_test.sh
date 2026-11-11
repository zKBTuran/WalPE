#!/bin/sh
# linkage_test.sh <build dir> <wayland|nowayland>
# What the built programs make the dynamic loader load, read from the files
# themselves (readelf -d, nm -D): a part that is not built must not leave a
# library behind in walpe-daemon or in the web module.
#   nowayland (-Dwayland=disabled): neither file names a libwayland-*
#   library as NEEDED or has an undefined wl_* symbol.
#   every build: walpe-daemon does not name libpulse-simple as NEEDED
#   (scene sound is off by default; engine/scenesound.c loads the library
#   when the first stream is opened).
# Exit 77 (skipped) without readelf / nm. No display, no daemon.
B=$1; WL=$2
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
done
n=$(needed "$B/walpe-daemon" | grep -c '^libpulse-simple')
check $([ "$n" = 0 ] && echo 1 || echo 0) "walpe-daemon: libpulse-simple is not NEEDED (loaded with the first sound stream)"
echo "linkage: $checks checks, $fail failed"
[ $fail = 0 ]
