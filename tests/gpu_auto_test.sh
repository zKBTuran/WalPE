#!/bin/sh
# gpu_auto_test.sh <walpe-daemon> <walpe-ctl> <gpusel-test>
# What gpu = auto does in a running daemon, on machines that exist only as
# made-up sysfs trees (WALPE_GPU_SYSFS_ROOT, written by `gpusel-test
# --tree`). A display of its own (Xvfb -displayfd; WALPE_CHECK_DISPLAY=sway:
# a headless Wayland compositor, scripts/displayenv.py), private HOME /
# config / cache / runtime directories, everything removed at the end.
#
# There is no GPU here: Mesa's software renderer draws. So the one thing
# these runs cannot show is a second GPU that passes its test. They show
# everything around it:
#  1. a machine with one GPU, a card that drives the displays itself, and
#     gpu = display on a laptop with two: no test process is started, no
#     variable is set, there is no note - the start of every version before;
#  2. a laptop whose integrated GPU drives the panel, with a discrete GPU
#     beside it: auto tests that one (two test processes), the test fails
#     (a software renderer answered), the window system's GPU renders, the
#     log and status say why, and no variable is left in the daemon's
#     environment;
#  3. the same laptop with a GPU variable of the user's own: no test;
#  4. the discrete GPU has no driver: no test, and the note says so;
#  5. `walpe-ctl set gpu=display` on the laptop of 2: taken without a
#     restart, and the next start tests nothing; `set gpu=auto`: the daemon
#     restarts in place and tests again.
# exit 77: skipped (the display's programs are not installed, or the build has
# no backend for it); WALPE_CHECK_STRICT=1 makes that a failure.
daemon=$1
ctl=$2
gpusel=$3
fails=0
skip=77
[ "${WALPE_CHECK_STRICT:-0}" = 0 ] || skip=1
displayenv=$(dirname "$0")/../scripts/displayenv.py
case "${WALPE_CHECK_DISPLAY:-xvfb}" in
    xvfb|x11) kind=xvfb ;;
    sway|wayland) kind=sway ;;
    *) echo "gpu-auto: WALPE_CHECK_DISPLAY is not a display kind (xvfb, sway)"; exit 2 ;;
esac
if [ "$kind" = xvfb ] && ! command -v python3 >/dev/null 2>&1; then
    command -v Xvfb >/dev/null 2>&1 || { echo "gpu-auto: Xvfb not installed, skipped"; exit $skip; }
else
    command -v python3 >/dev/null 2>&1 || { echo "gpu-auto: python3 not installed, skipped"; exit $skip; }
    why=$(python3 -I "$displayenv" needs --display "$kind" --build "$(dirname "$daemon")")
    rc=$?
    [ $rc = 0 ] || { echo "gpu-auto: skipped: $why"; exit $rc; }
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/walpe-gpua.XXXXXX") || exit 1
xpid=
dpid=
stop_daemon () {
    [ -n "$dpid" ] || return 0
    "$ctl" quit >/dev/null 2>&1
    n=0
    while kill -0 "$dpid" 2>/dev/null && [ $n -lt 40 ]; do sleep 0.25; n=$((n + 1)); done
    kill -9 "$dpid" 2>/dev/null     # one that hangs in its start-up does not act on a request
    wait "$dpid" 2>/dev/null
    dpid=
}
cleanup () {
    stop_daemon
    [ -n "$xpid" ] && kill "$xpid" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP
fail () { fails=$((fails + 1)); echo "FAIL: $*"; }

mkdir "$tmp/home" "$tmp/cfg" "$tmp/cache" "$tmp/rt" "$tmp/tmp" "$tmp/log" "$tmp/sys"
chmod 700 "$tmp/rt"
export HOME="$tmp/home" XDG_CONFIG_HOME="$tmp/cfg" XDG_CACHE_HOME="$tmp/cache"
export XDG_RUNTIME_DIR="$tmp/rt" TMPDIR="$tmp/tmp"
unset DBUS_SESSION_BUS_ADDRESS WALPE_SOFTWARE WALPE_MODULE_DIR DISPLAY WAYLAND_DISPLAY WALPE_MONITORS
unset WALPE_GPU DRI_PRIME __NV_PRIME_RENDER_OFFLOAD __NV_PRIME_RENDER_OFFLOAD_PROVIDER
unset __GLX_VENDOR_LIBRARY_NAME __EGL_VENDOR_LIBRARY_FILENAMES
# (the devices' names from their ids, whatever id list this machine has)
export WALPE_GPU_PCI_IDS=none

for m in desktop-nvidia desktop-igpu-arc laptop-intel-nvidia laptop-nvidia-undriven; do
    "$gpusel" --tree "$m" "$tmp/sys/$m" || { echo "gpu-auto: no tree for $m"; exit 1; }
done

if [ "$kind" = xvfb ]; then
    Xvfb -displayfd 3 -screen 0 640x480x24 -nolisten tcp 3>"$tmp/disp" >"$tmp/log/xvfb" 2>&1 &
    xpid=$!
    i=0
    while [ ! -s "$tmp/disp" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
    [ -s "$tmp/disp" ] || { echo "gpu-auto: Xvfb did not start, skipped"; exit $skip; }
    DISPLAY=:$(cat "$tmp/disp")
    WALPE_BACKEND=x11
    export DISPLAY WALPE_BACKEND
else
    python3 -I "$displayenv" serve --display sway --outputs 640x480+0+0 --ready "$tmp/disp" \
        >"$tmp/log/xvfb" 2>&1 &
    xpid=$!
    i=0
    while [ ! -s "$tmp/disp" ] && [ $i -lt 150 ] && kill -0 "$xpid" 2>/dev/null; do sleep 0.2; i=$((i + 1)); done
    [ -s "$tmp/disp" ] || { echo "gpu-auto: the headless compositor did not start, skipped"; exit $skip; }
    WAYLAND_DISPLAY=$(sed -n 's/^WAYLAND_DISPLAY=//p' "$tmp/disp")
    WALPE_BACKEND=wayland
    export WAYLAND_DISPLAY WALPE_BACKEND
fi

status () { "$ctl" status 2>/dev/null; }
# start <machine> [NAME=value ...]: a daemon on that machine's tree; its log is $log
start () {
    stop_daemon
    log="$tmp/log/daemon"
    m=$1
    shift
    env WALPE_GPU_SYSFS_ROOT="$tmp/sys/$m" "$@" "$daemon" >"$log" 2>&1 &
    dpid=$!
    end=$(($(date +%s) + 60))
    while [ "$(date +%s)" -lt "$end" ]; do
        status | grep -q '"ok":true' && return 0
        kill -0 "$dpid" 2>/dev/null || break
        sleep 0.25
    done
    fail "$m: the daemon did not come up"
    sed 's/^/    | /' "$log" | tail -20
    return 1
}
# the variables a GPU choice sets must not be in the daemon's environment
clean_env () {
    if tr '\0' '\n' < "/proc/$dpid/environ" 2>/dev/null |
        grep -E -q '^(DRI_PRIME|__NV_PRIME_RENDER_OFFLOAD|__GLX_VENDOR_LIBRARY_NAME|__EGL_VENDOR_LIBRARY_FILENAMES)='; then
        fail "$1: a GPU variable is set in the daemon's environment"
    fi
}
# nothing was tested, nothing is noted: the start of every version before
untested () {
    grep -q "gpu: device 0: " "$log" || fail "$1: the device list is not in the log"
    grep -E -q "tested in|could not be made the renderer|gpu = (auto|display)" "$log" &&
        fail "$1: the log speaks of a choice or a test: $(grep -E 'gpu = |tested in' "$log" | head -3)"
    st=$(status)
    echo "$st" | grep -q '"gpu_offload":false' || fail "$1: gpu_offload is not false"
    echo "$st" | grep -q '"gpu_note"' && fail "$1: status has a gpu_note"
    echo "$st" | grep -q '"gpu_environment"' && fail "$1: status has gpu_environment"
    clean_env "$1"
}

# 1. machines on which auto is `display`
if start desktop-nvidia; then
    untested "one GPU, auto"
    status | grep -q '"gpu":"auto"' || fail "one GPU: the setting is not auto"
fi
start desktop-igpu-arc && untested "a card drives the displays, auto"
start laptop-intel-nvidia WALPE_GPU=display && untested "laptop, gpu = display"

# 2. the laptop: auto tries the discrete GPU, and here the test has to fail
if start laptop-intel-nvidia; then
    grep -q "gpu = auto: .*10de:28a1.* could not be made the renderer .*rendering on .*8086:9a49" "$log" ||
        fail "laptop, auto: no line about the failed test: $(grep 'gpu' "$log" | head -5)"
    grep -q "gpu = auto: .*(tested in [0-9]* ms)" "$log" || fail "laptop, auto: the log does not say how long the test took"
    grep -q "WARNING.*gpu" "$log" && fail "laptop, auto: a failed test of auto's own choice is logged as a warning"
    st=$(status)
    echo "$st" | grep -q '"gpu":"auto"' || fail "laptop, auto: the setting is not auto"
    echo "$st" | grep -q '"gpu_offload":false' || fail "laptop, auto: gpu_offload is not false"
    echo "$st" | grep -q '"gpu_note":"[^"]*could not be made the renderer[^"]*gpu = display does not try' ||
        fail "laptop, auto: gpu_note does not say what happened"
    echo "$st" | grep -q '"gpu_environment"' && fail "laptop, auto: gpu_environment after a failed test"
    n=0     # (the renderer's name is known when the first frame was drawn)
    while [ $n -lt 120 ] && ! status | grep -q '"gl_renderer":"[^"]'; do sleep 0.25; n=$((n + 1)); done
    [ $n -lt 120 ] || fail "laptop, auto: nothing renders after the failed test"
    clean_env "laptop, auto, failed test"

    # 5. display: the device that renders now - no restart; auto: tested again
    out=$("$ctl" set gpu=display 2>&1)
    echo "$out" | grep -q '"ok":true' || fail "set gpu=display was refused: $out"
    sleep 1
    grep -q "restarting in place" "$log" && fail "set gpu=display restarted a daemon that renders on that GPU already"
    status | grep -q '"gpu":"display"' || fail "set gpu=display: status does not show it"
    grep -q '"gpu" *: *"display"' "$tmp/cfg/walpe/config.json" || fail "set gpu=display was not saved"
    out=$("$ctl" set gpu=auto 2>&1)
    echo "$out" | grep -q '"ok":true' || fail "set gpu=auto was refused: $out"
    n=0
    while [ $n -lt 120 ] && [ "$(grep -c 'could not be made the renderer' "$log")" -lt 2 ]; do sleep 0.25; n=$((n + 1)); done
    grep -q "gpu setting changed: restarting in place" "$log" || fail "set gpu=auto did not restart the daemon"
    [ "$(grep -c 'could not be made the renderer' "$log")" -ge 2 ] || fail "set gpu=auto: the discrete GPU was not tested again"
    grep -q '"gpu"' "$tmp/cfg/walpe/config.json" && fail "gpu=auto is the default and was written to the config"
    n=0
    while [ $n -lt 120 ] && ! status | grep -q '"ok":true'; do sleep 0.25; n=$((n + 1)); done
    out=$("$ctl" set gpu=window-system 2>&1)
    echo "$out" | grep -q '"ok":true' && fail "a value that names no device was accepted: $out"
fi
# ... and a saved gpu = display tests nothing at the next start
mkdir -p "$tmp/cfg/walpe"
echo '{ "gpu": "display" }' > "$tmp/cfg/walpe/config.json"
start laptop-intel-nvidia && untested "laptop, saved gpu = display"
rm -f "$tmp/cfg/walpe/config.json"

# 3. the user's own variable decides: no test, and a note that is no warning
if start laptop-intel-nvidia DRI_PRIME=1; then
    grep -q "tested in\|could not be made" "$log" && fail "DRI_PRIME set: auto tested a GPU all the same"
    grep -q "gpu: DRI_PRIME is set in the environment: gpu = auto is not applied" "$log" ||
        fail "DRI_PRIME set: the log does not say that it decides"
    grep -q "WARNING.*gpu" "$log" && fail "DRI_PRIME set: logged as a warning"
    status | grep -q '"gpu_note":"DRI_PRIME is set' || fail "DRI_PRIME set: no gpu_note"
fi

# 4. a discrete GPU nothing drives: no test, the reason in log and status
if start laptop-nvidia-undriven; then
    grep -q "tested in\|could not be made" "$log" && fail "no driver: auto tested the GPU all the same"
    grep -q "gpu = auto: .*no kernel driver loaded: rendering stays on" "$log" ||
        fail "no driver: the log does not say why auto stays"
    status | grep -q '"gpu_note":"[^"]*no kernel driver loaded' || fail "no driver: no gpu_note"
    clean_env "no driver"
fi

stop_daemon
if [ $fails = 0 ]; then
    echo "gpu-auto ($kind): all checks passed"
    exit 0
fi
echo "gpu-auto ($kind): $fails check(s) failed"
exit 1
