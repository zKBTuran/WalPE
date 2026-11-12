#!/bin/sh
# websec_test.sh <build dir> - a hostile web wallpaper against the running
# daemon. Private HOME / runtime directory,
# its own X server (Xvfb -displayfd), a listener on a second loopback port
# that stands for "some other service on this machine".
#
#  1. network access off (WALPE_WEB_OFFLINE=1): the page's own files still
#     load, nothing reaches the other loopback port (fetch, localhost
#     spelling, WebSocket) - it used to;
#  2. the X selections of another client survive a page that selects text
#     on a timer (PRIMARY) and copies on every click (CLIPBOARD) - both
#     used to be taken and dropped;
#  3. log lines a page can cause at will (posting the timing script's
#     message, permission requests) are limited - they were not;
#  4. a page that posts megabytes to the daemon's message handler is
#     stopped and reported in status ("web_error") - the daemon used to
#     keep every message (2 GB in eight seconds);
#  5. the page's token (the secret part of its address, which guards the
#     loopback file server) does not appear in the log - it did, in the
#     line for a refused download of one of the page's own files and in
#     console lines;
#  6. the WebKit start-up test that the daemon runs in a child process
#     before its first page: a child that never finishes (stopped here) is
#     ended after 40 s and the reason is in the log and in status - the
#     pages used to wait for it for good, black, with nothing said;
#  7. a request with a large body sent to the page's file server (the
#     loopback port takes connections from any local program) is not kept
#     in the daemon's memory - it was, whole, whatever the method;
#  8. connections to that port that send nothing do not use up the
#     daemon's open files: 128 are kept, the longest idle one makes room -
#     every one was kept, for as long as its other end stayed.
#
# Skipped (exit 77) without Xvfb / python3 / the web module, or when WebKit
# does not come up here. Run: meson test -C build websec -v
B=${1:?build dir}
[ -x "$B/walpe-daemon" ] && [ -x "$B/walpe-ctl" ] && [ -f "$B/walpe-web.so" ] && [ -x "$B/websec-selown" ] || { echo "SKIP: daemon / web module / helper not built"; exit 77; }
command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no Xvfb"; exit 77; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: no python3"; exit 77; }

# (in the system's temporary directory, not below the build directory: the
# daemon's socket lives in XDG_RUNTIME_DIR and a socket path has 107 bytes)
T=$(mktemp -d "${TMPDIR:-/tmp}/walpe-websec-XXXXXX") || exit 77
export HOME="$T/home" XDG_RUNTIME_DIR="$T/run"
export XDG_CONFIG_HOME="$HOME/.config" XDG_DATA_HOME="$HOME/.local/share" XDG_CACHE_HOME="$HOME/.cache"
export NO_AT_BRIDGE=1 GTK_A11Y=none
unset WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS WALPE_WEB_SELECTIONS WALPE_WEB_TIMING WALPE_WEB_SCHEME WALPE_WEB_FILE_URLS
mkdir -p "$HOME/.config" "$XDG_RUNTIME_DIR" "$T/wp"
chmod 700 "$XDG_RUNTIME_DIR"
XPID= DPID= LPID=
cleanup () {
    [ -S "$XDG_RUNTIME_DIR/walpe.sock" ] && "$B/walpe-ctl" quit >/dev/null 2>&1
    sleep 0.3
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    [ -n "$LPID" ] && kill "$LPID" 2>/dev/null
    [ -n "$XPID" ] && kill "$XPID" 2>/dev/null
    sleep 0.3
    if [ -n "$WEBSEC_KEEP" ]; then echo "kept: $T"; return; fi   # (to look at daemon.log after a failure)
    case "$T" in */walpe-websec-??????) rm -rf "$T";; esac
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# --- X server of our own
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$T/dfd" >"$T/xvfb.log" 2>&1 &
XPID=$!
i=0; while [ ! -s "$T/dfd" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
[ -s "$T/dfd" ] || { echo "SKIP: Xvfb did not start"; exit 77; }
export DISPLAY=":$(cat "$T/dfd")"
# (the daemon is told its window system by name: it must use this X server whatever
# session the test is started from)
export WALPE_BACKEND=x11

# --- "another service on this machine"
python3 -I -c '
import socket, sys
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(16)
open(sys.argv[1], "w").write(str(s.getsockname()[1]))
while True:
    c, a = s.accept()
    try:
        c.settimeout(2); d = c.recv(200)
    except Exception:
        d = b""
    open(sys.argv[2], "a").write("CONN %r\n" % d.split(b"\r\n")[0])
    try:
        c.sendall(b"HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
    except Exception:
        pass
    c.close()
' "$T/port" "$T/listen.log" &
LPID=$!
i=0; while [ ! -s "$T/port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
[ -s "$T/port" ] || { echo "SKIP: no listener"; exit 77; }
PORT=$(cat "$T/port")
: > "$T/listen.log"

# --- the wallpaper
echo "own file" > "$T/wp/own.txt"
printf 'not a web type' > "$T/wp/own.bin"
printf '{ "type": "web", "title": "websec", "file": "index.html" }\n' > "$T/wp/project.json"
cat > "$T/wp/index.html" <<PAGE
<!doctype html><html><body style="background:#223"><p id="p">text of the page</p><input id="i" value="PAGE-TEXT">
<script>
function log(s) { console.log('NP ' + s); }
var O = 'http://127.0.0.1:$PORT', pending = 4;
function fin() { if (--pending == 0) { log('net done'); setTimeout(flood, 300); } }
fetch('own.txt').then(function (r) { return r.text(); }).then(function (t) { log('own: ' + (t.indexOf('own file') == 0 ? 'ok' : 'wrong')); fin(); },
                                                              function (e) { log('own: FAILED ' + e); fin(); });
// (the listener answers "ok" with CORS open: a readable "ok" = the request got there)
function other(name, url) { fetch(url).then(function (r) { return r.text(); }).then(
  function (t) { log(name + (t == 'ok' ? ': REACHED' : ': blocked (answer not from the service)')); fin(); },
  function () { log(name + ': blocked'); fin(); }); }
other('other-port', O + '/a');
other('other-port localhost', 'http://localhost:$PORT/b');
try { var w = new WebSocket('ws://127.0.0.1:$PORT/ws'); w.onopen = function () { log('websocket: OPEN'); };
      w.onerror = function () { log('websocket: error'); }; } catch (e) { log('websocket: ' + e); }
new Image().src = O + '/img';
// lines that carry the page's address: a frame that would be a download, the console
var fr = document.createElement('iframe'); fr.src = 'own.bin'; document.body.appendChild(fr);
log('my address is ' + location.href);
setTimeout(fin, 2500);
// selections: PRIMARY on a timer, CLIPBOARD on a click
setInterval(function () { var i = document.getElementById('i'); i.focus(); i.select();
  var r = document.createRange(); r.selectNodeContents(document.getElementById('p'));
  var s = getSelection(); s.removeAllRanges(); s.addRange(r); i.select(); }, 150);
document.addEventListener('mousedown', function (e) { var i = document.getElementById('i'); i.focus(); i.select();
  var c = false; try { c = document.execCommand('copy'); } catch (x) {}
  log('mousedown trusted=' + e.isTrusted + ' copy=' + c);
  if (navigator.clipboard) navigator.clipboard.writeText('PAGE-CLIP').catch(function () {}); });
// log lines at will (after the results above are logged: the daemon drops
// lines when more than 50 a second arrive)
function flood() { var H = window.webkit.messageHandlers.walpe;
  for (var k = 0; k < 3000; k++) H.postMessage('raf:1 2 3');
  for (var k = 0; k < 200; k++) navigator.geolocation.getCurrentPosition(function () {}, function () {}); }
</script></body></html>
PAGE

# --- the daemon
WALPE_WEB_CONSOLE=1 WALPE_WEB_OFFLINE=1 "$B/walpe-daemon" > "$T/daemon.log" 2>&1 &
DPID=$!
i=0; while [ ! -S "$XDG_RUNTIME_DIR/walpe.sock" ] && [ $i -lt 100 ]; do sleep 0.2; i=$((i+1)); done
[ -S "$XDG_RUNTIME_DIR/walpe.sock" ] || { echo "SKIP: the daemon did not start here"; tail -5 "$T/daemon.log"; exit 77; }
"$B/walpe-ctl" open "$T/wp" >/dev/null 2>&1
i=0; while ! grep -q 'NP net done' "$T/daemon.log" && [ $i -lt 240 ]; do sleep 0.5; i=$((i+1)); done
if ! grep -q 'NP net done' "$T/daemon.log"; then
    if grep -q 'web: loaded' "$T/daemon.log"; then echo "FAIL: the page loaded but never finished its requests"; exit 1; fi
    # a page host asked for by name (the websec-private run) that did not
    # come up is what that run is there to find: not a skip
    if [ "$WALPE_WEB_HOST" = private ] &&
       grep -q 'private page host cannot start\|could not use the private page host\|cannot make a directory for the page host' "$T/daemon.log"; then
        echo "FAIL: WALPE_WEB_HOST=private, and the private page host did not come up:"
        grep 'page host' "$T/daemon.log" | sed 's/^/    /' | head -5
        exit 1
    fi
    echo "SKIP: WebKit did not show the page here"; tail -5 "$T/daemon.log"; exit 77
fi

# --- a request with a body to the page's file server: any program on this
# machine can connect to that port, and the body was kept in the daemon's
# memory whole (192 MB here) before the request was looked at
HPORT=$(sed -n 's|.*web: serving wallpapers on http://127\.0\.0\.1:\([0-9]*\)/.*|\1|p' "$T/daemon.log" | head -1)
rss_kb () { sed -n 's/^VmRSS:[^0-9]*\([0-9]*\).*/\1/p' "/proc/$DPID/status" 2>/dev/null; }
BODY_KB=
if [ -n "$HPORT" ]; then
    R0=$(rss_kb)
    python3 -I -c '
import socket, sys
port = int(sys.argv[1]); mb = 192
s = socket.create_connection(("127.0.0.1", port), timeout=120)
s.sendall(b"POST /x HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Length: %d\r\n\r\n" % (port, mb << 20))
chunk = b"\0" * (1 << 20)
try:
    for i in range(mb):
        s.sendall(chunk)
    s.recv(64)      # the answer comes when the whole body was read
except Exception:
    pass
' "$HPORT"
    R1=$(rss_kb)
    [ -n "$R0" ] && [ -n "$R1" ] && BODY_KB=$((R1 - R0))
fi

# --- connections to that port that send nothing: each was a file descriptor
# of the daemon for as long as the other side cared (a thousand of them and
# the daemon was at its limit: the control socket accepted nobody)
CONN_HELD= CONN_GROW= CONN_STATUS=ok
if [ -n "$HPORT" ]; then
    F0=$(ls "/proc/$DPID/fd" 2>/dev/null | wc -l)
    python3 -I -c '
import resource, socket, sys, time
port = int(sys.argv[1]); want = 500
soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
try:
    need = want + 64
    if soft != resource.RLIM_INFINITY and soft < need:
        resource.setrlimit(resource.RLIMIT_NOFILE, (need if hard == resource.RLIM_INFINITY else min(need, hard), hard))
except Exception:
    pass
ss = []; t0 = time.time()
while len(ss) < want and time.time() - t0 < 40:
    try:
        s = socket.socket(); s.settimeout(3); s.connect(("127.0.0.1", port)); ss.append(s)
    except Exception:
        break
open(sys.argv[2], "w").write("%d\n" % len(ss))
time.sleep(8)
' "$HPORT" "$T/held" &
    HPID=$!
    i=0; while [ ! -s "$T/held" ] && [ $i -lt 120 ]; do sleep 0.5; i=$((i+1)); done
    sleep 1
    F1=$(ls "/proc/$DPID/fd" 2>/dev/null | wc -l)
    CONN_HELD=$(cat "$T/held" 2>/dev/null)
    CONN_GROW=$((F1 - F0))
    "$B/walpe-ctl" status >/dev/null 2>&1 || CONN_STATUS=none
    wait "$HPID"
fi

# --- another client's selections while the page selects, and two clicks
"$B/websec-selown" 6 > "$T/sel.log" 2>&1 &
SPID=$!
sleep 1.5
"$B/walpe-ctl" click 400 300 >/dev/null 2>&1
sleep 1.5
"$B/walpe-ctl" click 420 320 >/dev/null 2>&1
wait "$SPID"
sleep 1

# --- a second wallpaper that floods the message handler (at most 480 MB)
mkdir -p "$T/flood"
printf '{ "type": "web", "title": "websec flood", "file": "index.html" }\n' > "$T/flood/project.json"
cat > "$T/flood/index.html" <<'PAGE'
<!doctype html><html><body><script>
var H = window.webkit.messageHandlers.walpe, big = 'x'.repeat(4 * 1024 * 1024), n = 0;
var t = setInterval(function () { for (var i = 0; i < 4 && n < 120; i++, n++) H.postMessage(big); if (n >= 120) clearInterval(t); }, 50);
</script></body></html>
PAGE
"$B/walpe-ctl" open "$T/flood" >/dev/null 2>&1
i=0; while ! grep -q 'message handler: it is stopped' "$T/daemon.log" && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
"$B/walpe-ctl" status > "$T/status.json" 2>/dev/null

fail=0
bad () { echo "FAIL: $*"; fail=1; }
grep -q 'message handler: it is stopped' "$T/daemon.log" || bad "a page that posted 480 MB to the message handler was not stopped"
grep -q 'web_error.*was stopped' "$T/status.json" || bad "status does not report the stopped page (web_error)"
grep -q 'NP own: ok' "$T/daemon.log" || bad "the page's own file did not load with network access off"
if [ -z "$BODY_KB" ]; then
    bad "the port of the page's file server is not in the log, or the daemon's memory could not be read"
elif grep -q 'libasan\|libtsan' "/proc/$DPID/maps" 2>/dev/null; then
    echo "note: a sanitizer build keeps freed memory: the request body check is not made"
elif [ "$BODY_KB" -gt 98304 ]; then
    bad "a request body of 192 MB sent to the page's file server cost the daemon $((BODY_KB / 1024)) MB"
fi
# a run for one page host (WALPE_WEB_HOST=private | x11) has tested that
# host only if the page really lived there
case "$WALPE_WEB_HOST" in
private|x11)
    grep -Eq "\"web_host\" *: *\"$WALPE_WEB_HOST\"" "$T/status.json" ||
        bad "the page was not on the page host asked for (WALPE_WEB_HOST=$WALPE_WEB_HOST); status says: $(grep -Eo '"web_host" *: *"[a-z0-9]*"' "$T/status.json" | head -1)"
    ;;
esac
if [ -s "$T/listen.log" ]; then
    bad "with network access off the page reached another port of this machine:"; sed 's/^/    /' "$T/listen.log" | head -5
fi
grep -q 'NP other-port: REACHED\|NP other-port localhost: REACHED\|NP websocket: OPEN' "$T/daemon.log" && bad "the page says it got through to the other port"
grep -q '^OWN primary=1 clipboard=1' "$T/sel.log" || { echo "SKIP: the helper could not own the selections"; cat "$T/sel.log"; exit 77; }
if grep -q '^LOST' "$T/sel.log"; then bad "a selection of another client was taken by the wallpaper:"; sed 's/^/    /' "$T/sel.log"; fi
if [ "${CONN_HELD:-0}" -lt 300 ]; then
    echo "note: only ${CONN_HELD:-0} connections could be opened to the page's file server here (their limit not exercised)"
elif [ "$CONN_GROW" -gt 220 ]; then
    bad "$CONN_HELD connections that sent nothing were $CONN_GROW open files of the daemon"
fi
[ "$CONN_STATUS" = ok ] || bad "the daemon did not answer on its control socket while $CONN_HELD connections to the page's file server were open"
grep -q 'NP mousedown trusted=true' "$T/daemon.log" || echo "note: no trusted click reached the page here (CLIPBOARD part not exercised)"
grep -q 'NP my address is' "$T/daemon.log" || bad "the page's console line is missing from the log"
if grep -Eq '/[0-9a-f]{32}(/|$|[ ?#])' "$T/daemon.log"; then
    bad "the page token is in the daemon's log:"; grep -E '/[0-9a-f]{32}(/|$|[ ?#])' "$T/daemon.log" | sed -E 's|/[0-9a-f]{32}|/<32 hex digits>|g; s/^/    /' | cut -c1-160 | head -3
fi
n=$(grep -c 'web timing: page requestAnimationFrame' "$T/daemon.log")
[ "$n" -le 5 ] || bad "$n log lines from a message any page can post (raf:)"
n=$(grep -c 'PermissionRequest refused' "$T/daemon.log")
[ "$n" -le 10 ] || bad "$n log lines for permission requests of one page"

# --- a daemon of its own for the start-up test that does not finish
"$B/walpe-ctl" quit >/dev/null 2>&1
i=0; while kill -0 "$DPID" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.2; i=$((i+1)); done
kill "$DPID" 2>/dev/null
rm -f "$HOME/.config/walpe/config.json"
"$B/walpe-daemon" > "$T/daemon2.log" 2>&1 &
DPID=$!
i=0; while [ ! -S "$XDG_RUNTIME_DIR/walpe.sock" ] && [ $i -lt 100 ]; do sleep 0.2; i=$((i+1)); done
"$B/walpe-ctl" open "$T/wp" >/dev/null 2>&1
# the test process: a child of the daemon started with --web-probe
CH= i=0
while [ -z "$CH" ] && [ $i -lt 400 ] && ! grep -q 'WebKit start-up test' "$T/daemon2.log"; do
    for st in /proc/[0-9]*/stat; do
        p=${st#/proc/}; p=${p%/stat}
        pp=$(sed 's/.*) [A-Za-z] \([0-9]*\) .*/\1/' "$st" 2>/dev/null)
        if [ "$pp" = "$DPID" ] && tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q -- '--web-probe'; then
            CH=$p; break
        fi
    done
    [ -n "$CH" ] || sleep 0.05
    i=$((i+1))
done
[ -n "$CH" ] && kill -STOP "$CH" 2>/dev/null && sleep 1
if [ -n "$CH" ] && ! grep -q 'WebKit start-up test' "$T/daemon2.log"; then
    i=0; while ! grep -q 'did not finish within' "$T/daemon2.log" && [ $i -lt 140 ]; do sleep 0.5; i=$((i+1)); done
    grep -q 'did not finish within' "$T/daemon2.log" || bad "a WebKit start-up test that never finishes was waited for without end"
    "$B/walpe-ctl" status 2>/dev/null | grep -q 'web_error.*did not finish' || bad "status does not say that the WebKit start-up test did not finish (web_error)"
    kill -KILL "$CH" 2>/dev/null
else
    echo "note: the WebKit start-up test was over before it could be stopped (its time limit not exercised)"
fi
[ $fail -eq 0 ] && echo "websec: ok"
exit $fail
