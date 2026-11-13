#!/bin/sh
# sec_daemon_test.sh <walpe-daemon> <walpe-ctl>
# Security checks against a running daemon. An X server of
# its own (Xvfb -displayfd, never a fixed display number), private HOME /
# config / cache / runtime directories, everything removed at the end.
# WALPE_CHECK_DISPLAY=sway runs the same against a headless Wayland compositor
# of its own (scripts/displayenv.py; part 5 needs the X server and is skipped).
#  1. a runtime directory other accounts can write to is not used: the
#     socket goes to a private directory, and a FIFO planted as the
#     crash-loop marker does not hang the start;
#  2. config.json is written for the owner alone, and ~/.cache/walpe (made
#     0755 here beforehand, as the package cache used to) is private;
#  3. `walpe-ctl set` cannot be made to send another command through a key;
#  4. software_threads=nan does not put a number outside 0..32 in the config;
#  5. the layout restart does not hand its open descriptors to the new image
#     and leaves no zombie children;
#  6. the crash-loop marker ("walpe.sock.loading": the paths of the
#     wallpapers being loaded) is written for the owner alone.
# exit 77: skipped (the display's programs are not installed, or the build has
# no backend for it); WALPE_CHECK_STRICT=1 makes that a failure.
daemon=$1
ctl=$2
fails=0
skip=77
[ "${WALPE_CHECK_STRICT:-0}" = 0 ] || skip=1
displayenv=$(dirname "$0")/../scripts/displayenv.py
case "${WALPE_CHECK_DISPLAY:-xvfb}" in
    xvfb|x11) kind=xvfb ;;
    sway|wayland) kind=sway ;;
    *) echo "sec-daemon: WALPE_CHECK_DISPLAY is not a display kind (xvfb, sway)"; exit 2 ;;
esac
if [ "$kind" = xvfb ] && ! command -v python3 >/dev/null 2>&1; then
    command -v Xvfb >/dev/null 2>&1 || { echo "sec-daemon: Xvfb not installed, skipped"; exit $skip; }
else
    # (asked of the harness: the display's programs, and whether this build has the backend)
    command -v python3 >/dev/null 2>&1 || { echo "sec-daemon: python3 not installed, skipped"; exit $skip; }
    why=$(python3 -I "$displayenv" needs --display "$kind" --build "$(dirname "$daemon")")
    rc=$?
    [ $rc = 0 ] || { echo "sec-daemon: skipped: $why"; exit $rc; }
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/walpe-secd.XXXXXX") || exit 1
xpid=
dpid=
cleanup () {
    if [ -n "$dpid" ]; then
        kill "$dpid" 2>/dev/null
        n=0
        while kill -0 "$dpid" 2>/dev/null && [ $n -lt 20 ]; do sleep 0.25; n=$((n + 1)); done
        kill -9 "$dpid" 2>/dev/null     # one that hangs in its start-up does not act on SIGTERM
    fi
    [ -n "$xpid" ] && kill "$xpid" 2>/dev/null
    wait 2>/dev/null
    chmod -R u+rwx "$tmp" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP
fail () { fails=$((fails + 1)); echo "FAIL: $*"; }

mkdir "$tmp/home" "$tmp/cfg" "$tmp/cache" "$tmp/shared" "$tmp/tmp" "$tmp/log"
mkdir "$tmp/cache/walpe"
chmod 755 "$tmp/cache" "$tmp/cache/walpe"
chmod 1777 "$tmp/shared"            # a runtime "directory" like /tmp
mkfifo "$tmp/shared/walpe.sock.loading"   # what another account could put there
export HOME="$tmp/home" XDG_CONFIG_HOME="$tmp/cfg" XDG_CACHE_HOME="$tmp/cache"
export XDG_RUNTIME_DIR="$tmp/shared" TMPDIR="$tmp/tmp"
unset DBUS_SESSION_BUS_ADDRESS WALPE_SOFTWARE WALPE_MODULE_DIR DISPLAY WAYLAND_DISPLAY WALPE_MONITORS
umask 022

echo "640x720+0+0,640x720+640+0" > "$tmp/mons"
if [ "$kind" = xvfb ]; then
    Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$tmp/disp" >"$tmp/log/xvfb" 2>&1 &
    xpid=$!
    i=0
    while [ ! -s "$tmp/disp" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
    [ -s "$tmp/disp" ] || { echo "sec-daemon: Xvfb did not start, skipped"; exit $skip; }
    DISPLAY=:$(cat "$tmp/disp")
    export DISPLAY
    # the two monitors are halves of the one X screen
    WALPE_MONITORS="@$tmp/mons"
    WALPE_BACKEND=x11
    export WALPE_MONITORS WALPE_BACKEND
else
    # two outputs of the same sizes; the compositor lives in a runtime directory of its
    # own, the directory others can write to is the daemon's alone
    python3 -I "$displayenv" serve --display sway --outputs "$(cat "$tmp/mons")" --ready "$tmp/disp" \
        >"$tmp/log/xvfb" 2>&1 &
    xpid=$!
    i=0
    while [ ! -s "$tmp/disp" ] && [ $i -lt 150 ] && kill -0 "$xpid" 2>/dev/null; do sleep 0.2; i=$((i + 1)); done
    [ -s "$tmp/disp" ] || { echo "sec-daemon: the headless compositor did not start, skipped"; exit $skip; }
    WAYLAND_DISPLAY=$(sed -n 's/^WAYLAND_DISPLAY=//p' "$tmp/disp")
    WALPE_BACKEND=wayland
    export WAYLAND_DISPLAY WALPE_BACKEND
fi

: > "$tmp/leak"
# fd 9: a descriptor some library left open without close-on-exec
"$daemon" 9<"$tmp/leak" >"$tmp/log/daemon" 2>&1 &
dpid=$!

sock="$tmp/tmp/walpe-$(id -u)/walpe.sock"
status () { "$ctl" status 2>/dev/null; }
wait_up () {    # seconds (by the clock: a request to a daemon that hangs takes 2 s itself)
    end=$(($(date +%s) + $1))
    while [ "$(date +%s)" -lt "$end" ]; do
        status | grep -q '"ok":true' && return 0
        kill -0 "$dpid" 2>/dev/null || return 1
        sleep 0.25
    done
    return 1
}

# 1. the shared runtime directory
if wait_up 30; then
    [ -S "$sock" ] || fail "the socket is not in the private directory ($sock)"
    [ -e "$tmp/shared/walpe.sock" ] && fail "a socket was made in the directory others can write to"
    [ -e "$tmp/shared/walpe.sock.lock" ] && fail "the lock file was made in the directory others can write to"
    m=$(stat -c %a "$tmp/tmp/walpe-$(id -u)")
    [ "$m" = 700 ] || fail "the private socket directory has mode $m"
else
    fail "the daemon did not come up with a runtime directory others can write to (a FIFO as its crash-loop marker)"
    sed 's/^/    | /' "$tmp/log/daemon" | tail -20
    echo "sec-daemon: $fails check(s) failed"
    exit 1
fi

# 2. config.json: saved by the first change, for the owner alone
# (asked again while a busy machine keeps the answer away)
cfg="$tmp/cfg/walpe/config.json"
n=0
while [ $n -lt 10 ]; do
    "$ctl" set fps=47 2>/dev/null | grep -q '"ok":true' && break
    sleep 1
    n=$((n + 1))
done
if [ -f "$cfg" ]; then
    m=$(stat -c %a "$cfg")
    [ "$m" = 600 ] || fail "config.json has mode $m, not 600"
    m=$(stat -c %a "$tmp/cfg/walpe")
    [ "$m" = 700 ] || fail "the config directory was created with mode $m, not 700"
else
    fail "config.json was not written"
fi
m=$(stat -c %a "$tmp/cache/walpe")
[ "$m" = 700 ] || fail "the cache directory has mode $m, not 700"

# 3. a key with a quote in it stays a key
"$ctl" set 'zz":1,"cmd":"quit","y=on' >/dev/null 2>&1
sleep 1
if ! wait_up 20; then   # ends at once when the process is gone
    fail "walpe-ctl set with a crafted key made the daemon quit"
    echo "sec-daemon: $fails check(s) failed"
    exit 1
fi

# 4. not a number
"$ctl" set software_threads=nan >/dev/null 2>&1
t=$(sed -n 's/.*"software_threads" *: *\(-\{0,1\}[0-9]*\).*/\1/p' "$cfg")
case "$t" in
    ''|-*) fail "software_threads=nan saved \"$t\"" ;;
    *) [ "$t" -le 32 ] || fail "software_threads=nan saved $t" ;;
esac

# 5. the layout restart: the file behind fd 9 must not be open in the new
# image (under whatever number)
leaked () { ls -l "/proc/$dpid/fd" 2>/dev/null | grep -q -F -- "$tmp/leak"; }
# the children of process $1 that have ended and were not collected. Read
# from /proc (a stat line is "pid (name) state parent ...", and the name may
# hold blanks and brackets): `ps --ppid` is procps's own, busybox's ps has
# no way to ask for a parent and said nothing but its usage, which counted
# as "no zombie".
zombies_of () {
    cat /proc/[0-9]*/stat 2>/dev/null | sed 's/^.*) //' | grep -c "^Z $1 "
}
if [ "$kind" = xvfb ] && leaked && command -v xrandr >/dev/null 2>&1; then
    echo "1280x720+0+0" > "$tmp/mons"
    n=0
    restarted=0
    while [ $n -lt 80 ]; do
        # any change of the root window makes the daemon look at the layout
        [ $((n % 12)) -eq 0 ] && xrandr --fb 1024x700 >/dev/null 2>&1
        if grep -q "restarting to follow" "$tmp/log/daemon" &&
           status | grep -q '"monitor_count":1'; then
            restarted=1
            break
        fi
        sleep 0.25
        n=$((n + 1))
    done
    if [ $restarted = 1 ]; then
        kill -0 "$dpid" 2>/dev/null || fail "the daemon is gone after the layout restart"
        leaked && fail "a descriptor of the old image is open in the daemon after the layout restart"
        z=$(zombies_of "$dpid")
        [ "$z" = 0 ] || fail "$z zombie child(ren) after the layout restart"
    else
        echo "sec-daemon: no layout restart could be triggered on this X server; that part is skipped"
    fi
else
    echo "sec-daemon: no X server with xrandr (or no /proc/<pid>/fd); the layout restart part is skipped"
fi

# 6. the crash-loop marker next to the socket names the wallpaper that is
# being loaded: for the owner alone, like config.json (a 1 x 1 BMP picture
# is the wallpaper; the marker stays for 20 s after a load began)
printf 'BM:\0\0\0\0\0\0\0\066\0\0\0\050\0\0\0\001\0\0\0\001\0\0\0\001\0\030\0\0\0\0\0\004\0\0\0\023\013\0\0\023\013\0\0\0\0\0\0\0\0\0\0\100\040\020\0' > "$tmp/pic.bmp"
if "$ctl" open "$tmp/pic.bmp" 2>/dev/null | grep -q '"ok":true'; then
    n=0
    while [ ! -e "$sock.loading" ] && [ $n -lt 40 ]; do sleep 0.25; n=$((n + 1)); done
    if [ -f "$sock.loading" ]; then
        m=$(stat -c %a "$sock.loading")
        [ "$m" = 600 ] || fail "the crash-loop marker (it names the wallpaper's path) has mode $m, not 600"
        grep -q -F "$tmp/pic.bmp" "$sock.loading" || fail "the crash-loop marker does not name the wallpaper that was opened"
    else
        fail "no crash-loop marker next to the socket after a wallpaper was opened"
    fi
else
    fail "the test picture could not be opened as a wallpaper"
fi

"$ctl" quit >/dev/null 2>&1
n=0
while kill -0 "$dpid" 2>/dev/null && [ $n -lt 80 ]; do sleep 0.25; n=$((n + 1)); done
kill -0 "$dpid" 2>/dev/null && fail "the daemon did not stop on quit"
[ -e "$sock" ] && fail "the socket was left behind"

if [ $fails -gt 0 ]; then
    echo "sec-daemon: $fails check(s) failed"
    sed 's/^/    | /' "$tmp/log/daemon" | tail -15
    exit 1
fi
echo "sec-daemon: ok"
exit 0
