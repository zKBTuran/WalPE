#!/bin/sh
# tests/swsim/run.sh - "Render on CPU" against a stand-in GPU vendor.
#
# What cannot be tried without an NVIDIA machine is which GL implementation
# libglvnd picks when a GPU driver is installed next to Mesa. This script
# builds a tiny EGL vendor library (fakegpu_egl_vendor.c) that behaves like
# the driver in the one respect that matters: it sorts before Mesa and
# claims the X display. Then it starts the real daemon four times with a
# private vendor directory, config and runtime dir:
#
#   1  switch off             libglvnd hands the display to the fake GPU
#                             vendor (which cannot render: the daemon stops)
#   2  WALPE_SOFTWARE=1       the daemon runs on Mesa llvmpipe, the fake
#                             vendor is never loaded
#   3  walpe-ctl set software=on / off on a daemon that runs on a working
#                             "GPU" vendor (Mesa under another name): it
#                             restarts in place, same pid, socket answers
#   4  switch on, Mesa's EGL library missing
#                             one clear line, status software_note, and
#                             the daemon keeps rendering on the "GPU"
#
# Needs: an X display (e.g. Xvfb :99 & DISPLAY=:99), a C compiler,
# libglvnd's headers, Mesa with llvmpipe, python3. Usage:
#   tests/swsim/run.sh <build dir>        (default: build)
set -u
here=$(cd "$(dirname "$0")" && pwd)
B=$(cd "${1:-build}" && pwd) || exit 2
[ -x "$B/walpe-daemon" ] && [ -x "$B/walpe-ctl" ] || { echo "no walpe-daemon in $B"; exit 2; }
[ -n "${DISPLAY:-}" ] || { echo "DISPLAY is not set"; exit 2; }
mesa_json=$(grep -l -i 'egl_mesa' /usr/share/glvnd/egl_vendor.d/*.json /etc/glvnd/egl_vendor.d/*.json 2>/dev/null | head -1)
[ -n "$mesa_json" ] || { echo "Mesa's EGL vendor file not found: cannot simulate"; exit 2; }
mesa_lib=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["ICD"]["library_path"])' "$mesa_json")

T=$(mktemp -d /tmp/walpe-swsim.XXXXXX)
dpid=0
trap '[ "$dpid" != 0 ] && kill $dpid 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/v-gpu" "$T/v-work" "$T/v-nomesa" "$T/cfg" "$T/rt" "$T/lib"
chmod 700 "$T/rt"
${CC:-cc} -shared -fPIC -O1 -o "$T/lib/libEGL_fakegpu.so.0" "$here/fakegpu_egl_vendor.c" || exit 2
icd () { printf '{ "file_format_version" : "1.0.0", "ICD" : { "library_path" : "%s" } }\n' "$1"; }
# v-gpu: a GPU driver that claims the display + Mesa
icd "$T/lib/libEGL_fakegpu.so.0" > "$T/v-gpu/10_fakegpu.json"
cp "$mesa_json" "$T/v-gpu/50_mesa.json"
# v-work: a "GPU driver" that really renders (Mesa under another name) + Mesa
real=$(ldconfig -p | awk -v l="$mesa_lib" '$1 == l { print $NF; exit }')
[ -n "$real" ] || real=$mesa_lib
cp "$real" "$T/lib/libEGL_gpudrv.so.0"   # a copy: its own name in /proc/<pid>/maps
icd "$T/lib/libEGL_gpudrv.so.0" > "$T/v-work/10_gpudrv.json"
cp "$mesa_json" "$T/v-work/50_mesa.json"
# v-nomesa: the same "GPU driver", Mesa's vendor file names a library that is not there
icd "$T/lib/libEGL_gpudrv.so.0" > "$T/v-nomesa/10_gpudrv.json"
icd "libEGL_mesa_not_installed.so.0" > "$T/v-nomesa/50_mesa.json"

export XDG_CONFIG_HOME="$T/cfg" XDG_RUNTIME_DIR="$T/rt" XDG_CACHE_HOME="$T/cache" XDG_DATA_HOME="$T/data"
# (the X display asked for above is the one to draw on, also when the terminal this is
# started from belongs to a Wayland session)
export WALPE_BACKEND=x11
unset WALPE_SOFTWARE __EGL_VENDOR_LIBRARY_FILENAMES LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER LP_NUM_THREADS
fails=0
ok ()  { echo "  ok    $1"; }
bad () { echo "  FAIL  $1"; fails=$((fails + 1)); }
st ()  { "$B/walpe-ctl" status 2>/dev/null | python3 -c '
import json, sys
try: s = json.load(sys.stdin)["state"]
except Exception: sys.exit(1)
print(s.get(sys.argv[1], ""))' "$1"; }
start () {   # start <vendor dir> <log> [VAR=value]
    v=$1; log=$2; shift 2
    env __EGL_VENDOR_LIBRARY_DIRS="$v" "$@" "$B/walpe-daemon" > "$log" 2>&1 &
    dpid=$!
    i=0
    while [ $i -lt 40 ]; do
        kill -0 $dpid 2>/dev/null || return 1
        [ -n "$(st gl_renderer)" ] && return 0
        sleep 0.25; i=$((i + 1))
    done
    return 1
}
stop () { "$B/walpe-ctl" quit >/dev/null 2>&1; [ "$dpid" != 0 ] && wait $dpid 2>/dev/null; dpid=0; }

echo "1. switch off, a GPU vendor sorts before Mesa"
if start "$T/v-gpu" "$T/1.log"; then bad "the daemon came up (expected: the fake GPU vendor owns the display)"; stop
else ok "the daemon did not get a GL context from Mesa"; fi
grep -q "FAKEGPU: eglInitialize called" "$T/1.log" && ok "libglvnd gave the X display to the GPU vendor" \
    || bad "the GPU vendor was not asked"
[ "$dpid" != 0 ] && wait $dpid 2>/dev/null; dpid=0

echo "2. WALPE_SOFTWARE=1, same vendor directory"
if start "$T/v-gpu" "$T/2.log" WALPE_SOFTWARE=1; then
    r=$(st gl_renderer)
    case "$r" in *llvmpipe*) ok "renders with: $r";; *) bad "renderer: $r";; esac
    [ "$(st software_active)" = True ] && [ "$(st software_render)" = True ] && [ -z "$(st software_note)" ] \
        && ok "status: software_render and software_active are on" || bad "status flags"
    [ "$(st software_threads)" -ge 1 ] 2>/dev/null && ok "llvmpipe threads: $(st software_threads)" || bad "thread count"
    grep -q FAKEGPU "$T/2.log" && bad "the GPU vendor library was loaded" || ok "the GPU vendor library was never loaded"
    stop
else bad "the daemon did not come up"; cat "$T/2.log" | tail -5; fi

echo "3. walpe-ctl set software=on / off on a running daemon (working GPU vendor)"
if start "$T/v-work" "$T/3.log"; then
    pid0=$dpid
    [ "$(st software_render)" = False ] && grep -q libEGL_gpudrv /proc/$pid0/maps \
        && ok "starts with the switch off, on the GPU vendor library" || bad "state at start"
    out=$("$B/walpe-ctl" set software=on)
    case "$out" in *restarting*) ok "set software=on: $out";; *) bad "reply: $out";; esac
    sleep 2
    [ "$(st software_render)" = True ] && [ "$(st software_active)" = True ] && ok "after the restart: on, $(st gl_renderer)" \
        || bad "not on after the restart"
    kill -0 $pid0 2>/dev/null && ok "same process id ($pid0), control socket answered" || bad "the daemon is gone"
    grep -q '"software_render" : true' "$T/cfg/walpe/config.json" && ok "saved in config.json" || bad "not saved"
    [ "$(cat /proc/$pid0/comm 2>/dev/null)" = walpe-daemon ] && ok "still called walpe-daemon" || bad "process name: $(cat /proc/$pid0/comm 2>/dev/null)"
    grep -q libEGL_gpudrv /proc/$pid0/maps && bad "the GPU vendor library is loaded" \
        || ok "the GPU vendor library is not loaded in the restarted daemon"
    out=$("$B/walpe-ctl" set software=off)
    sleep 2
    [ "$(st software_render)" = False ] && kill -0 $pid0 2>/dev/null && ok "set software=off: restarted again, switch off" || bad "switching off"
    grep -q libEGL_gpudrv /proc/$pid0/maps && ok "back on the GPU vendor library (the start environment was restored)" \
        || bad "still not on the GPU vendor library"
    "$B/walpe-ctl" set software_threads=3 >/dev/null; "$B/walpe-ctl" set software=on >/dev/null; sleep 2
    [ "$(st software_threads)" = 3 ] && ok "software_threads=3 is used" || bad "threads: $(st software_threads)"
    "$B/walpe-ctl" set software=off >/dev/null; "$B/walpe-ctl" set software_threads=auto >/dev/null; sleep 2
    stop
else bad "the daemon did not come up"; tail -5 "$T/3.log"; fi

echo "4. switch on, but Mesa's EGL library is not installed"
if start "$T/v-nomesa" "$T/4.log" WALPE_SOFTWARE=1; then
    ok "the daemon came up and renders ($(st gl_renderer) through the GPU vendor)"
    n=$(st software_note)
    [ -n "$n" ] && ok "status software_note: $n" || bad "no software_note"
    [ "$(grep -c 'software rendering is switched on but not available' "$T/4.log")" = 1 ] \
        && ok "one line in the log" || bad "log lines: $(grep -c 'not available' "$T/4.log")"
    grep -q libEGL_gpudrv /proc/$dpid/maps && ok "the daemon is on the GPU vendor library" || bad "not on the GPU vendor library"
    stop
else bad "the daemon did not come up"; tail -5 "$T/4.log"; fi

[ $fails = 0 ] && echo "swsim: all scenarios passed" || echo "swsim: $fails failure(s)"
exit $fails
