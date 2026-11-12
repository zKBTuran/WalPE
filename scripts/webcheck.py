#!/usr/bin/env python3
"""End-to-end checks of web wallpapers: real pages in WebKitGTK, shown by a private daemon.

  python3 -I scripts/webcheck.py --build build [--display sway|xvfb] [--host auto|private|x11]
          [--only a,b] [--hashes FILE] [--keep] [--tmp DIR]

The display is a private headless Wayland compositor without Xwayland (sway on wlroots'
headless backend, `xwayland disable`, see scripts/displayenv.py) and the daemon's
environment has no DISPLAY: the page's window can only live on the web module's private
page host (src/engine/webhost_wl.c). Compositor and daemon share one runtime directory and
WAYLAND_DISPLAY is a name in it, as in a session. The pictures are screenshots of the
compositor's outputs (grim): what is checked is what the compositor shows. The pages are
generated here and say what happened in them on their console (WALPE_WEB_CONSOLE=1 puts
that into the daemon's log). Nothing of the user's is read or written.

  start     the daemon comes up without DISPLAY, the page is on the private page host
            (status web_host), is visible to itself, has the output's size as its
            viewport and screen, and the output shows its picture: four coloured
            quarters, a canvas, the colour of a wallpaper option.
  live      requestAnimationFrame runs at about the frame rate limit, the canvas and a
            CSS animation move on the output.
  props     the Wallpaper Engine listener: applyUserProperties with the option values
            on load, with the changed one after `walpe-ctl setprop` (and the picture
            follows), applyGeneralProperties with the frame rate limit.
  audio     wallpaperRegisterAudioListener: the listener is called with 128 numbers.
  pointer   a pointer device over the wallpaper reaches the page as trusted events at
            the right place; a click of the device, `walpe-ctl click` and `walpe-ctl
            scroll` too; status says the input is native.
  two       two outputs of different sizes show the same page (one page, the larger
            size), the pointer on the second output is mapped to the page; then a page
            of its own on one output: two page windows on the page host.
  outputs   an output is added (the page appears there), changes its mode (the page
            follows the larger size) and is unplugged; the page stays right on the
            output that is left.
  pause     `walpe-ctl pause`: the page is told (setPaused), is hidden to itself, its
            requestAnimationFrame stops and its processes use (next to) no CPU time;
            `resume` brings all of it back.
  restart   the daemon is ended and started again (the page is back), and restarts in
            place (web hardware acceleration switched): the page is back, the page
            processes of the old image are gone.
  crash     the page's process is killed: the daemon says so and loads the page
            again; killed again and again it is given up with a note in status, and
            opening the wallpaper again starts it afresh.
  stale     DISPLAY names an X server that is not there: the page ends up on the
            private page host all the same, the log and status say why. (A build
            without the X11 page host has nothing to fall back from: the page is on
            the private host and nothing is said.)
  leftover  while a page runs the page host has no socket in the file system and no
            directory of its own; after `walpe-ctl quit` no process of the daemon is
            left and the runtime directory holds nothing of it.
  sandbox   the page's process runs in WebKit's sandbox (its own namespaces, a seccomp
            filter) and sees neither the daemon's control socket, nor a socket of the
            page host, nor - on the private page host - the compositor's socket or an
            X server's.
  widgets   a page that asks for what a wallpaper must not get - an option menu, a file
            and a colour chooser, a new window, fullscreen, dialogs, a context menu, a
            link that leads away, a drag - is clicked on all of it: nothing opens, the
            page stays the page it was, the daemon lives.
  clipboard (needs wl-copy / wl-paste) the session's clipboard and primary selection,
            set by another program, are still that program's after a page selected
            text on a timer, copied on every click and asked to read the clipboard;
            the page read nothing of them.
  offline   network access off (WALPE_WEB_OFFLINE=1): the page's own file loads, and
            nothing reaches another port of this machine.

--display xvfb runs the cases that need no real output on a private X server instead
(the daemon draws on X11); with --host x11 the pages are then on the X11 page host, with
--host private on the private one. --hashes FILE appends one line per picture of the
still page ("<display> <host> <size> <hash> <hash without the text line>"), to compare
the two page hosts.

Exit code 0 = all passed, 1 = failures, 77 = skipped (sway or grim is not installed, the
build has no Wayland backend, no web module or no private page host, or WebKit does not
start here). WEBCHECK_PATIENCE=N multiplies every wait (a software renderer on a busy
machine: 3 or more). --tmp DIR: work in DIR (must not exist; removed afterwards)."""
import json, os, re, signal, subprocess, sys, time

sys.dont_write_bytecode = True   # no __pycache__ next to the scripts
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("LIFECHECK_PATIENCE", os.environ.get("WEBCHECK_PATIENCE", "1") or "1")
import displayenv        # noqa: E402
import lifecheck as lc   # noqa: E402  (its Env and waits)

rc = lc.rc
PATIENCE = lc.PATIENCE
CONFIG = {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False}
fails = 0
checks = 0
HOST = "auto"           # --host
HASHES = None           # --hashes


def check(ok, what, detail=None):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok and detail is not None:
        print("       " + str(detail), flush=True)
    return ok


def note(text):
    print("note " + text, flush=True)


def wait_for(fn, timeout=10.0, step=0.2):
    return rc.wait_for(fn, timeout, step)


# ------------------------------------------------------------------ the pages
# Sizes are fractions of the viewport, so that the same page is right at any size:
#   four quarters (red, green, blue, light grey), in the middle a box with the colour of
#   the option "tint", bottom right a canvas (left half magenta, right half cyan; in the
#   live page a white bar walks over it), bottom left one line of text; the live page
#   also has a yellow box that a CSS animation moves along the top.
# The page keeps a record S and writes it to the console as one line "WC<tag> {json}".
PAGE = """<!doctype html><html><head><meta charset="utf-8"><style>
html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#101820}
.q{position:absolute;width:50%;height:50%}
#q0{left:0;top:0;background:#c02020}#q1{left:50%;top:0;background:#20c020}
#q2{left:0;top:50%;background:#2020c0}#q3{left:50%;top:50%;background:#e0e0e0}
#prop{position:absolute;left:45%;top:45%;width:10%;height:10%;background:#000}
#cv{position:absolute;left:62%;top:62%;width:26%;height:26%}
#txt{position:absolute;left:4%;top:82%;height:10%;font:28px sans-serif;color:#fff;white-space:nowrap}
@keyframes mv{from{left:4%}to{left:40%}}
#css{position:absolute;top:5%;width:8%;height:8%;left:4%;background:#f0f000;%CSSANIM%}
</style></head><body>
<div class="q" id="q0"></div><div class="q" id="q1"></div><div class="q" id="q2"></div><div class="q" id="q3"></div>
<div id="prop"></div><div id="css"></div><canvas id="cv" width="260" height="130"></canvas>
<canvas id="gl" width="64" height="64" style="position:absolute;left:88%;top:4%;width:8%;height:8%"></canvas>
<div id="txt">WalPE web check 0123456789</div>
<script>
var LIVE = %LIVE%, TAG = '%TAG%';
var S = {raf: 0, audio: 0, alen: 0, anum: true, user: 0, tint: null, speed: null, keys: [], gen: null, paused: null,
         npause: 0, moves: 0, mx: -1, my: -1, down: 0, up: 0, click: 0, cx: -1, cy: -1, wheel: 0, wy: 0,
         trusted: 0, untrusted: 0, born: Date.now()};
var cv = document.getElementById('cv'), g = cv.getContext('2d');
function paint() {
  g.fillStyle = '#c020c0'; g.fillRect(0, 0, 130, 130);
  g.fillStyle = '#20c0c0'; g.fillRect(130, 0, 130, 130);
  if (LIVE) { g.fillStyle = '#ffffff'; g.fillRect((S.raf * 2) % 250, 50, 10, 30); }
}
paint();
try { var w3 = document.getElementById('gl').getContext('webgl');
  if (w3) { w3.clearColor(1.0, 0.5, 0.0, 1.0); w3.clear(w3.COLOR_BUFFER_BIT); S.gl = true; } else S.gl = false;
} catch (e) { S.gl = false; }
function frame() { S.raf++; paint(); requestAnimationFrame(frame); }
if (LIVE) requestAnimationFrame(frame);
function say() {
  S.vis = document.visibilityState; S.w = innerWidth; S.h = innerHeight; S.sw = screen.width; S.sh = screen.height;
  S.dpr = devicePixelRatio; S.t = Math.round(performance.now());
  console.log('WC' + TAG + ' ' + JSON.stringify(S));
}
window.wallpaperPropertyListener = {
  applyUserProperties: function (p) {
    S.user++; S.keys = Object.keys(p).sort();
    if (p.tint) { S.tint = p.tint.value;
      var c = p.tint.value.split(' ').map(function (x) { return Math.round(parseFloat(x) * 255); });
      document.getElementById('prop').style.background = 'rgb(' + c.join(',') + ')'; }
    if (p.speed) S.speed = p.speed.value;
    say();
  },
  applyGeneralProperties: function (p) { if (p.fps !== undefined) S.gen = p.fps; },
  setPaused: function (b) { S.paused = b; S.npause++; say(); }
};
if (window.wallpaperRegisterAudioListener)
  window.wallpaperRegisterAudioListener(function (a) { S.audio++; S.alen = a.length;
    for (var i = 0; i < a.length; i++) if (typeof a[i] !== 'number') S.anum = false; });
function tr(e) { if (e.isTrusted) S.trusted++; else S.untrusted++; }
window.addEventListener('mousemove', function (e) { S.moves++; S.mx = e.clientX; S.my = e.clientY; tr(e); });
window.addEventListener('mousedown', function (e) { S.down++; tr(e); });
window.addEventListener('mouseup', function (e) { S.up++; tr(e); });
window.addEventListener('click', function (e) { S.click++; S.cx = e.clientX; S.cy = e.clientY; tr(e); say(); });
window.addEventListener('wheel', function (e) { S.wheel++; S.wy += e.deltaY > 0 ? 1 : e.deltaY < 0 ? -1 : 0; tr(e); say(); });
document.addEventListener('visibilitychange', say);
setInterval(say, 500);
window.addEventListener('load', say);
</script></body></html>
"""
RED, GREEN, BLUE, LIGHT = (0xc0, 0x20, 0x20), (0x20, 0xc0, 0x20), (0x20, 0x20, 0xc0), (0xe0, 0xe0, 0xe0)
MAGENTA, CYAN, YELLOW = (0xc0, 0x20, 0xc0), (0x20, 0xc0, 0xc0), (0xf0, 0xf0, 0x00)
# where each part is looked at, as fractions of the picture
SPOTS = {"red": (0.20, 0.35, RED), "green": (0.80, 0.20, GREEN), "blue": (0.20, 0.65, BLUE),
         "light": (0.57, 0.93, LIGHT), "magenta": (0.66, 0.70, MAGENTA), "cyan": (0.84, 0.70, CYAN)}
TINT0, TINT1 = "1 0.5 0", "0 0.5 1"
TINT_RGB = {TINT0: (255, 128, 0), TINT1: (0, 128, 255)}


def gen_page(out, tag, live):
    os.makedirs(out)
    rc.jdump(os.path.join(out, "project.json"), {
        "type": "web", "title": "webcheck " + tag, "file": "index.html",
        "general": {"properties": {
            "tint": {"order": 1, "text": "Tint", "type": "color", "value": TINT0},
            "speed": {"order": 2, "text": "Speed", "type": "slider", "min": 0, "max": 10, "value": 3}}}})
    html = (PAGE.replace("%TAG%", tag).replace("%LIVE%", "true" if live else "false")
            .replace("%CSSANIM%", "animation:mv 1.5s linear infinite alternate" if live else ""))
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write(html)


def near(a, b, tol=10):
    return a != displayenv.NO_PICTURE and all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def states(env, tag, mark=0):
    """the records the page with this tag wrote since mark, oldest first"""
    out = []
    key = "web console: log WC%s {" % tag
    for ln in env.since(mark).splitlines():
        i = ln.find(key)
        if i >= 0:
            try:
                out.append(json.loads(ln[i + len(key) - 1:]))
            except ValueError:
                pass
    return out


def state(env, tag, mark=0, fresh=False, timeout=15):
    """the page's newest record; fresh: one written after this call"""
    if fresh:
        mark = env.mark()
    box = [None]

    def got():
        s = states(env, tag, mark)
        box[0] = s[-1] if s else None
        return box[0]
    wait_for(got, timeout, 0.1)
    return box[0] or {}


def wait_state(env, tag, pred, timeout=20, mark=0):
    """wait until the page's newest record satisfies pred; -> that record (or the last seen)"""
    box = [{}]

    def ok():
        s = states(env, tag, mark)
        if s:
            box[0] = s[-1]
        return bool(s) and pred(s[-1])
    wait_for(ok, timeout, 0.2)
    return box[0]


def cover(page, mon_size, fx, fy):
    """where the point (fx, fy) of a page (fractions of it) is on a monitor that shows the
    page "cover" (scaled about the middle until it fills the monitor): monitor pixels"""
    pw, ph = page
    mw, mh = mon_size
    k = max(mw / pw, mh / ph)
    return mw / 2 + (fx * pw - pw / 2) * k, mh / 2 + (fy * ph - ph / 2) * k


def look(env, mon, page=None):
    """a picture of a monitor read as the check page: {part: its colour}, the tint box,
    the size, and the picture itself. page: the page's size when it is not the monitor's"""
    f = env.display.frame(mon)
    if not f:
        return {"ok": False, "size": (0, 0), "frame": None, "bad": ["no picture"], "tint": None}
    w, h, _ = f
    page = page or (w, h)
    px = displayenv.pixel_of
    got, bad = {}, []
    for k, (fx, fy, want) in SPOTS.items():
        x, y = cover(page, (w, h), fx, fy)
        if not (2 <= x < w - 2 and 2 <= y < h - 2):
            continue                    # (cut off on this monitor)
        got[k] = px(f, int(x), int(y))
        if not near(got[k], want):
            bad.append("%s is rgb%s" % (k, got[k]))
    return {"ok": not bad and len(got) >= 4, "bad": bad, "size": (w, h), "frame": f, "tint": px(f, w // 2, h // 2)}


def shows_page(env, mon, tint=None, timeout=30, page=None):
    """wait until the monitor shows the check page (with this tint box); -> (it does, the last look)"""
    box = [None]

    def ok():
        box[0] = lk = look(env, mon, page)
        return lk["ok"] and (tint is None or near(lk["tint"], tint, 6))
    return bool(wait_for(ok, timeout, 0.4)), box[0]


def yellow_at(frame):
    """where the animated box is: the x of the first yellow pixel in the row through it, -1 = none"""
    if not frame:
        return -1
    w, h, _ = frame
    y = int(h * 0.09)
    for x in range(0, w, 2):
        if near(displayenv.pixel_of(frame, x, y), YELLOW, 24):
            return x
    return -1


def bar_at(frame):
    """where the canvas's white bar is: the x of the first white pixel in the row through it"""
    if not frame:
        return -1
    w, h, _ = frame
    y = int(h * 0.75)
    for x in range(int(w * 0.62), int(w * 0.88)):
        if near(displayenv.pixel_of(frame, x, y), (255, 255, 255), 12):
            return x
    return -1


def still_hashes(frame):
    """(hash of the picture, hash of it with the rows of the text line left out)"""
    w, h, rgb = frame
    y0, y1 = int(h * 0.80), int(h * 0.94)
    cut = bytes(rgb[:y0 * w * 3]) + bytes(rgb[y1 * w * 3:])
    return displayenv.frame_hash(frame), displayenv.frame_hash((w, h - (y1 - y0), cut))


TROUBLE = re.compile(r"CRITICAL \*\*|protocol error|Segmentation fault|AddressSanitizer|LeakSanitizer|"
                     r"ThreadSanitizer|runtime error:|lost its client")


def healthy(env, mark, what):
    bad = [ln for ln in env.since(mark).splitlines() if TROUBLE.search(ln)]
    return check(env.daemon is not None and env.daemon.poll() is None and not bad,
                 what + ": the daemon runs, its log is clean", bad[:5])


def daemon_env(env):
    """what every daemon of this check gets on top of the display's variables"""
    ex = {"WALPE_WEB_CONSOLE": "1"}
    if HOST != "auto":
        ex["WALPE_WEB_HOST"] = HOST
    if env.display.kind == "xvfb":
        # the X11 backend waits for raw motion events while the pointer rests, and the
        # tool that moves it here (xdotool) warps it without any: ask for the position
        # all the time instead, as versions before the events did
        ex["WALPE_POINTER_POLL"] = "1"
    return ex


def start(env, mons, what, extra=None, config=None):
    rc.jdump(env.config, dict(CONFIG, **(config or {})))
    ex = daemon_env(env)
    ex.update(extra or {})
    return check(env.start_daemon(mons, extra=ex), what + ": daemon started", env.since(0)[-1500:])


def web_status(env):
    return {k: v for k, v in env.state().items() if k.startswith("web")}


def want_host(env):
    """the page host this run's pages must be on"""
    if HOST != "auto":
        return HOST
    return "private" if env.display.kind == "sway" else "x11"


def open_page(env, path, tag, what, mon=None, timeout=90):
    """open a check page and wait for its first record; -> the record ({} = it never came)"""
    mark = env.mark()
    env.open(path, mon)
    st = wait_state(env, tag, lambda s: s.get("user", 0) >= 1, timeout, mark)
    if not st:
        check(False, what + ": the page loaded and said so", web_status(env))
    return st


def procs():
    """every process: {pid: (ppid, comm)}"""
    out = {}
    for n in os.listdir("/proc"):
        if not n.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % n) as f:
                txt = f.read()
        except OSError:
            continue
        rp = txt.rfind(")")
        rest = txt[rp + 2:].split()
        out[int(n)] = (int(rest[1]), txt[txt.find("(") + 1:rp])
    return out


def descendants(pid, table=None):
    table = table or procs()
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        for q, (pp, _) in table.items():
            if pp == p:
                out.append(q)
                todo.append(q)
    return out


def page_procs(env, kind="WebKitWebProc"):
    """the daemon's page processes of one kind (WebKitWebProc, WebKitNetwork): [pid]"""
    if not env.daemon:
        return []
    table = procs()
    return sorted(p for p in descendants(env.daemon.pid, table) if table[p][1].startswith(kind))


def cpu_ticks(pids):
    """CPU time (utime + stime, clock ticks) these processes have used so far"""
    n = 0
    for p in pids:
        try:
            with open("/proc/%d/stat" % p) as f:
                txt = f.read()
            rest = txt[txt.rfind(")") + 2:].split()
            n += int(rest[11]) + int(rest[12])
        except (OSError, ValueError, IndexError):
            pass
    return n


def thread_names(pid):
    out = []
    try:
        for t in os.listdir("/proc/%d/task" % pid):
            try:
                with open("/proc/%d/task/%s/comm" % (pid, t)) as f:
                    out.append(f.read().strip())
            except OSError:
                pass
    except OSError:
        pass
    return out


def env_names(pid):
    try:
        with open("/proc/%d/environ" % pid, "rb") as f:
            return {e.split(b"=", 1)[0].decode(errors="replace") for e in f.read().split(b"\0") if e}
    except OSError:
        return None


# -------------------------------------------------------------------- start
def case_start(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "start"):
        return
    mark = env.mark()
    mon = env.mons[0]
    if env.display.kind == "sway":
        names = env_names(env.daemon.pid)
        check(names is not None and "DISPLAY" not in names and "WAYLAND_DISPLAY" in names,
              "start: the daemon's environment has a Wayland display and no DISPLAY",
              sorted(n for n in names or [] if "DISPLAY" in n))
    st = open_page(env, W["still"], "still", "start")
    if not st:
        return
    ws = web_status(env)
    check(str(ws.get("web_host", "")) == want_host(env), "start: status says the page is on the %s page host"
          % want_host(env), ws)
    check(not ws.get("web_error"), "start: status has no web_error", ws.get("web_error"))
    threads = thread_names(env.daemon.pid)
    if want_host(env) == "private":
        check("walpe-web-host" in threads, "start: the page host's display runs on a thread of the daemon")
    else:
        check("walpe-web-host" not in threads, "start: with the X11 page host the private one is not started "
              "(no thread of it)", sorted(set(threads)))
    check(st.get("vis") == "visible", "start: the page is visible to itself", st.get("vis"))
    check((st.get("w"), st.get("h")) == (mon[1], mon[2]), "start: the page's viewport is the monitor's size",
          (st.get("w"), st.get("h"), mon[1:3]))
    check((st.get("sw"), st.get("sh")) == (mon[1], mon[2]) and st.get("dpr") == 1,
          "start: window.screen is the monitor's size, one page pixel is one wallpaper pixel",
          (st.get("sw"), st.get("sh"), st.get("dpr")))
    ok, lk = shows_page(env, mon, TINT_RGB[TINT0])
    check(ok, "start: the output shows the page (quarters, canvas, the option's colour)",
          (lk["bad"], lk.get("tint"), lk["size"]))
    check(lk["size"] == (mon[1], mon[2]), "start: the picture has the output's size", lk["size"])
    if st.get("gl") and lk["frame"]:
        got = displayenv.pixel_of(lk["frame"], int(mon[1] * 0.92), int(mon[2] * 0.08))
        check(near(got, (255, 128, 0), 6), "start: a WebGL canvas of the page is in the picture", got)
    else:
        note("start: the page got no WebGL context here (status web_webgl: %s)" % ws.get("web_webgl"))
    if ok:
        # a still page stays still: the same picture twice
        time.sleep(1.0)
        again = look(env, mon)
        a, b = still_hashes(lk["frame"]), still_hashes(again["frame"]) if again["frame"] else ("none", "none")
        check(a == b, "start: a still page is the same picture a second later", (a, b))
        if HASHES:
            with open(HASHES, "a") as f:
                f.write("%s %s %dx%d %s %s\n" % (env.display.kind, ws.get("web_host"), lk["size"][0], lk["size"][1],
                                              a[0], a[1]))
    healthy(env, mark, "start")


# --------------------------------------------------------------------- live
def case_live(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "live"):
        return
    mark = env.mark()
    mon = env.mons[0]
    if not open_page(env, W["live"], "live", "live"):
        return
    ok, lk = shows_page(env, mon)
    check(ok, "live: the output shows the page", lk["bad"])
    a = state(env, "live", fresh=True)
    time.sleep(3.0)
    b = state(env, "live", fresh=True)
    dt = (b.get("t", 0) - a.get("t", 0)) / 1000.0
    rate = (b.get("raf", 0) - a.get("raf", 0)) / dt if dt > 0 else 0
    # (the limit is 30 a second; a busy machine runs it slower, nothing runs it faster)
    check(1 <= rate <= 36, "live: requestAnimationFrame runs, and not faster than the frame rate limit (30)",
          "%.1f a second over %.1f s" % (rate, dt))
    note("live: requestAnimationFrame %.1f times a second (limit 30)" % rate)
    xs, bars = set(), set()
    for _ in range(8):
        f = env.display.frame(mon)
        xs.add(yellow_at(f))
        bars.add(bar_at(f))
        time.sleep(0.25)
    xs.discard(-1)
    bars.discard(-1)
    check(len(xs) >= 3, "live: the CSS animation moves on the output", sorted(xs))
    check(len(bars) >= 3, "live: the canvas animation moves on the output", sorted(bars))
    healthy(env, mark, "live")


# -------------------------------------------------------------------- props
def case_props(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "props"):
        return
    mark = env.mark()
    mon = env.mons[0]
    st = open_page(env, W["still"], "still", "props")
    if not st:
        return
    check(st.get("keys") == ["speed", "tint"] and st.get("tint") == TINT0 and st.get("speed") == 3,
          "props: applyUserProperties was called with the wallpaper's options on load",
          {k: st.get(k) for k in ("keys", "tint", "speed", "user")})
    st = wait_state(env, "still", lambda s: s.get("gen") is not None, 10, mark)
    check(st.get("gen") == 30, "props: applyGeneralProperties was called with the frame rate limit", st.get("gen"))
    ok, lk = shows_page(env, mon, TINT_RGB[TINT0])
    check(ok, "props: the option's colour is on the output", lk.get("tint"))
    m2 = env.mark()
    rcode, rep = env.ctl("setprop", "tint=" + TINT1)
    check(rcode == 0, "props: walpe-ctl setprop is accepted", rep)
    st = wait_state(env, "still", lambda s: s.get("tint") == TINT1, 30, m2)
    check(st.get("tint") == TINT1 and st.get("keys") == ["tint"],
          "props: the page is told the changed option, and only that one",
          {k: st.get(k) for k in ("keys", "tint", "user")})
    ok, lk = shows_page(env, mon, TINT_RGB[TINT1])
    check(ok, "props: the output shows the new colour", lk.get("tint"))
    first = states(env, "still", mark)
    check(bool(first) and all(s.get("born") == first[0].get("born") for s in first),
          "props: the page was not loaded again for it")
    healthy(env, mark, "props")


# -------------------------------------------------------------------- audio
def case_audio(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "audio"):
        return
    mark = env.mark()
    if not open_page(env, W["live"], "live", "audio"):
        return
    check(bool(wait_for(lambda: "registered an audio listener" in env.since(mark), 20)),
          "audio: the daemon saw the page register its audio listener")
    st = wait_state(env, "live", lambda s: s.get("audio", 0) >= 3, 30, mark)
    check(st.get("audio", 0) >= 3 and st.get("alen") == 128 and st.get("anum") is True,
          "audio: the listener is called with 128 numbers", {k: st.get(k) for k in ("audio", "alen", "anum")})
    healthy(env, mark, "audio")


# ------------------------------------------------------------------ pointer
def case_pointer(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "pointer"):
        return
    mark = env.mark()
    mon = env.mons[0]
    if not open_page(env, W["still"], "still", "pointer"):
        return
    shows_page(env, mon)
    if not env.display.can_point():
        note("pointer: no way to move the pointer here; only walpe-ctl click / scroll are checked")
    else:
        for x, y in ((300, 200), (640, 360), (900, 500)):
            env.display.pointer(mon[3] + x, mon[4] + y, env.env)
            time.sleep(0.4)
        st = wait_state(env, "still", lambda s: (s.get("mx"), s.get("my")) == (900, 500), 20, mark)
        check((st.get("mx"), st.get("my")) == (900, 500) and st.get("moves", 0) >= 2,
              "pointer: the page follows the pointer device, at the right place",
              {k: st.get(k) for k in ("moves", "mx", "my", "trusted", "untrusted")})
        check(st.get("trusted", 0) >= 2 and st.get("untrusted", 0) == 0,
              "pointer: its events are trusted ones", {k: st.get(k) for k in ("trusted", "untrusted")})
        if env.display.kind == "sway":
            m2 = env.mark()
            env.display.click(1)
            st = wait_state(env, "still", lambda s: s.get("click", 0) >= 1, 20, m2)
            check(st.get("click") == 1 and (st.get("cx"), st.get("cy")) == (900, 500) and st.get("down") == 1 and
                  st.get("up") == 1, "pointer: a click of the device is one mousedown, mouseup and click there",
                  {k: st.get(k) for k in ("down", "up", "click", "cx", "cy")})
    before = state(env, "still", fresh=True)
    m3 = env.mark()
    rcode, rep = env.ctl("click", mon[3] + 400, mon[4] + 300)
    st = wait_state(env, "still", lambda s: s.get("click", 0) > before.get("click", 0), 20, m3)
    check(rcode == 0 and st.get("click", 0) == before.get("click", 0) + 1 and (st.get("cx"), st.get("cy")) == (400, 300),
          "pointer: walpe-ctl click reaches the page at that pixel", (rep, {k: st.get(k) for k in ("click", "cx", "cy")}))
    m4 = env.mark()
    rcode, rep = env.ctl("scroll", mon[3] + 400, mon[4] + 300, 2)
    st = wait_state(env, "still", lambda s: s.get("wheel", 0) >= 1, 20, m4)
    check(rcode == 0 and st.get("wheel", 0) >= 1 and st.get("wy", 0) > 0,
          "pointer: walpe-ctl scroll turns the wheel over the page (downwards)",
          (rep, {k: st.get(k) for k in ("wheel", "wy")}))
    check(st.get("untrusted", 0) == 0, "pointer: none of it came as a script-made event", st.get("untrusted"))
    ws = web_status(env)
    check(ws.get("web_input") == "native", "pointer: status says input reaches the page as real events", ws)
    healthy(env, mark, "pointer")


# ---------------------------------------------------------------------- two
def index_of(env, name):
    for m in env.monitors():
        if m.get("name") == name:
            return m.get("index")
    return None


def case_two(env, W):
    if not start(env, [("A", 1280, 720, 0, 0), ("B", 800, 600, 1280, 0)], "two"):
        return
    mark = env.mark()
    a, b = env.mons[0], env.mons[1]
    st = open_page(env, W["still"], "still", "two")
    if not st:
        return
    # one page for both monitors, laid out at the larger size
    st = wait_state(env, "still", lambda s: (s.get("w"), s.get("h")) == (1280, 720), 20, mark)
    check((st.get("w"), st.get("h")) == (1280, 720), "two: one page, laid out at the larger monitor's size",
          (st.get("w"), st.get("h")))
    check(len(page_procs(env)) == 1, "two: one page process for both monitors", page_procs(env))
    ok, lk = shows_page(env, a)
    check(ok and lk["size"] == (1280, 720), "two: the larger output shows the page", (lk["bad"], lk["size"]))
    ok, lk = shows_page(env, b, page=(1280, 720))
    check(ok and lk["size"] == (800, 600), "two: the smaller output shows it too, filled to its edges",
          (lk["bad"], lk["size"]))
    if env.display.can_point():
        # the middle of B is the middle of the page; (200, 150) on B is 240 / 180 monitor
        # pixels from it, and the page is drawn 600 / 720 as large there
        for bx, by, want in ((400, 300, (640, 360)), (200, 150, (400, 180))):
            m2 = env.mark()
            env.display.pointer(b[3] + bx, b[4] + by, env.env)
            st = wait_state(env, "still", lambda s: abs(s.get("mx", -9) - want[0]) <= 1 and
                            abs(s.get("my", -9) - want[1]) <= 1, 15, m2)
            check(abs(st.get("mx", -9) - want[0]) <= 1 and abs(st.get("my", -9) - want[1]) <= 1,
                  "two: the pointer at %d, %d of the smaller output is at %d, %d of the page" % (bx, by, *want),
                  (st.get("mx"), st.get("my")))
    # a page of its own on B: a second page window on the page host
    m3 = env.mark()
    idx = index_of(env, b[0])
    env.open(W["other"], idx)
    st = wait_state(env, "other", lambda s: s.get("user", 0) >= 1 and (s.get("w"), s.get("h")) == (800, 600), 90, m3)
    check((st.get("w"), st.get("h")) == (800, 600), "two: a page of its own on the smaller output has that size",
          (st.get("w"), st.get("h"), st.get("user")))
    check(st.get("vis") == "visible", "two: the second page is visible to itself", st.get("vis"))
    ok, lk = shows_page(env, b)
    check(ok, "two: the smaller output shows its own page", lk["bad"])
    ok, lk = shows_page(env, a)
    check(ok, "two: the larger output still shows the first page", lk["bad"])
    check(bool(wait_for(lambda: len(page_procs(env)) == 2, 10)), "two: two pages, two page processes",
          page_procs(env))
    if env.display.can_point():
        m4 = env.mark()
        env.display.pointer(b[3] + 100, b[4] + 100, env.env)
        st = wait_state(env, "other", lambda s: (s.get("mx"), s.get("my")) == (100, 100), 15, m4)
        check((st.get("mx"), st.get("my")) == (100, 100), "two: the pointer over the second page reaches that page",
              (st.get("mx"), st.get("my")))
        m5 = env.mark()
        env.display.pointer(a[3] + 300, a[4] + 200, env.env)
        st = wait_state(env, "still", lambda s: (s.get("mx"), s.get("my")) == (300, 200), 15, m5)
        check((st.get("mx"), st.get("my")) == (300, 200), "two: and back over the first page that one",
              (st.get("mx"), st.get("my")))
    ws = web_status(env)
    check(str(ws.get("web_host", "")) == want_host(env), "two: both on the %s page host" % want_host(env), ws)
    healthy(env, mark, "two")


# ------------------------------------------------------------------ outputs
def case_outputs(env, W):
    if not start(env, [("A", 1024, 576, 0, 0)], "outputs"):
        return
    mark = env.mark()
    pid = env.daemon.pid
    a = env.mons[0]
    st = open_page(env, W["still"], "still", "outputs")
    if not st:
        return
    ok, lk = shows_page(env, a)
    check(ok and (st.get("w"), st.get("h")) == (1024, 576), "outputs: the page on the first output",
          (lk["bad"], st.get("w"), st.get("h")))

    def page_is(size, since, timeout=90):
        return wait_state(env, "still", lambda s: (s.get("w"), s.get("h")) == size and s.get("user", 0) >= 1,
                          timeout, since)

    def monitor(name):
        for m in env.monitors():
            if m.get("name") == name:
                return (name, m["geometry"][2], m["geometry"][3], m["geometry"][0], m["geometry"][1])
        return None
    # --- a second, larger output appears
    m1 = env.mark()
    name = env.display.add_output(mode=(1280, 720), pos=(1024, 0))
    if not check(bool(name), "outputs: a second output was made"):
        return
    b = wait_for(lambda: monitor(name), 60, 0.5)
    if not check(bool(b), "outputs: the daemon lists the new output", [m.get("name") for m in env.monitors()]):
        return
    st = page_is((1280, 720), m1)
    check((st.get("w"), st.get("h")) == (1280, 720), "outputs: the page is laid out for the larger output now",
          (st.get("w"), st.get("h")))
    ok, lk = shows_page(env, b)
    check(ok and lk["size"] == (1280, 720), "outputs: the new output shows the page", (lk["bad"], lk["size"]))
    ok, lk = shows_page(env, a, page=(1280, 720))
    check(ok, "outputs: the first output still shows it", lk["bad"])
    check(env.daemon.poll() is None and env.daemon.pid == pid, "outputs: the daemon is the same process")
    # --- the first output changes its mode to the largest of all
    m2 = env.mark()
    env.display.output(a[0], mode=(1600, 900))
    a2 = wait_for(lambda: (lambda m: m if m and m[1:3] == (1600, 900) else None)(monitor(a[0])), 60, 0.5)
    check(bool(a2), "outputs: status follows the first output's new mode", monitor(a[0]))
    st = page_is((1600, 900), m2)
    check((st.get("w"), st.get("h")) == (1600, 900), "outputs: the page follows the larger mode",
          (st.get("w"), st.get("h")))
    if a2:
        ok, lk = shows_page(env, a2)
        check(ok and lk["size"] == (1600, 900), "outputs: the page fills the output's new mode", (lk["bad"], lk["size"]))
    # --- the second output is unplugged
    m3 = env.mark()
    env.display.remove_output(name)
    gone = wait_for(lambda: monitor(name) is None and len(env.monitors()) == 1, 60, 0.5)
    check(bool(gone), "outputs: the unplugged output is gone from status", [m.get("name") for m in env.monitors()])
    st = wait_state(env, "still", lambda s: s.get("user", 0) >= 1, 90, m3)
    a3 = monitor(a[0])
    if a3:
        ok, lk = shows_page(env, a3, timeout=60)
        check(ok, "outputs: the output that is left shows the page", lk["bad"])
    check(len(page_procs(env)) == 1, "outputs: one page process is left", page_procs(env))
    # --- output scale 2: the page is laid out in the output's own pixels, as before
    # (status lists a monitor with the pixels rendered for it; the surface's logical size
    # is what the scale changes)
    m4 = env.mark()
    env.display.output(a[0], scale=2)

    def logical():
        return [sf.get("logical") for sf in env.state().get("wl_surfaces") or [] if sf.get("output") == a[0]]
    scaled = wait_for(lambda: logical() == [[800, 450]], 60, 0.5)
    if check(bool(scaled), "outputs: the wallpaper's surface follows the output's scale 2 (800 x 450 in the layout)",
             logical()):
        st = wait_state(env, "still", lambda s: s.get("user", 0) >= 1 and (s.get("w"), s.get("h")) == (1600, 900),
                        90, m4)
        check((st.get("w"), st.get("h"), st.get("dpr")) == (1600, 900, 1),
              "outputs: at scale 2 the page still has the output's pixels as its size, one page pixel is one of them",
              (st.get("w"), st.get("h"), st.get("dpr")))
        a4 = monitor(a[0])
        if a4:
            ok, lk = shows_page(env, a4, timeout=60)
            check(ok and lk["size"] == (1600, 900), "outputs: and the output shows it in its own pixels",
                  (lk["bad"], lk["size"]))
    check(env.daemon.poll() is None and env.daemon.pid == pid, "outputs: still the same daemon process")
    left = host_leftovers(env)
    check(not left, "outputs: nothing of the page host is in the runtime directory", left)
    healthy(env, mark, "outputs")


# -------------------------------------------------------------------- pause
def busy(env, seconds):
    """CPU time (clock ticks) the daemon's page processes, and the daemon itself, use in a while"""
    pages = page_procs(env) + page_procs(env, "WebKitNetwork")
    p0, d0 = cpu_ticks(pages), cpu_ticks([env.daemon.pid])
    time.sleep(seconds)
    return cpu_ticks(pages) - p0, cpu_ticks([env.daemon.pid]) - d0


def case_pause(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "pause"):
        return
    mark = env.mark()
    mon = env.mons[0]
    if not open_page(env, W["live"], "live", "pause"):
        return
    shows_page(env, mon)
    time.sleep(1.0)
    run_pages, run_daemon = busy(env, 4.0)
    m1 = env.mark()
    env.ctl("pause")
    st = wait_state(env, "live", lambda s: s.get("paused") is True and s.get("vis") == "hidden", 30, m1)
    check(st.get("paused") is True, "pause: the page is told (setPaused(true))", st.get("paused"))
    check(st.get("vis") == "hidden", "pause: the page is hidden to itself", st.get("vis"))
    time.sleep(1.5)
    a = state(env, "live", fresh=True, timeout=5)
    idle_pages, idle_daemon = busy(env, 4.0)
    b = state(env, "live", fresh=True, timeout=5)
    check(bool(a) and bool(b) and a.get("raf") == b.get("raf"),
          "pause: requestAnimationFrame does not run while paused", (a.get("raf"), b.get("raf")))
    # (clock ticks of 10 ms over 4 s: 400 would be one CPU all the time)
    check(idle_pages <= max(8, run_pages // 8),
          "pause: the page's processes use next to no CPU time while paused",
          "%d ticks in 4 s paused, %d running" % (idle_pages, run_pages))
    check(idle_daemon <= max(8, run_daemon // 4),
          "pause: neither does the daemon", "%d ticks in 4 s paused, %d running" % (idle_daemon, run_daemon))
    note("pause: page processes %d ticks / 4 s running, %d paused; daemon %d running, %d paused (10 ms ticks)"
         % (run_pages, idle_pages, run_daemon, idle_daemon))
    f0 = env.display.frame(mon)
    m2 = env.mark()
    env.ctl("resume")
    st = wait_state(env, "live", lambda s: s.get("paused") is False and s.get("vis") == "visible" and
                    s.get("raf", 0) > b.get("raf", 0) + 10, 30, m2)
    check(st.get("paused") is False and st.get("vis") == "visible", "pause: resume is told and the page is visible again",
          (st.get("paused"), st.get("vis")))
    check(st.get("raf", 0) > b.get("raf", 0) + 10, "pause: requestAnimationFrame runs again",
          (b.get("raf"), st.get("raf")))
    check(st.get("born") == b.get("born"), "pause: it is the same page (not loaded again)")
    bars = set()
    for _ in range(6):
        bars.add(bar_at(env.display.frame(mon)))
        time.sleep(0.25)
    bars.discard(-1)
    check(len(bars) >= 2 and f0 is not None, "pause: the picture on the output moves again", sorted(bars))
    if env.display.can_point():
        # pointer input after a pause: the window was unmapped and mapped again
        m3 = env.mark()
        env.display.pointer(mon[3] + 500, mon[4] + 400, env.env)
        time.sleep(0.3)
        env.display.pointer(mon[3] + 520, mon[4] + 410, env.env)
        st = wait_state(env, "live", lambda s: (s.get("mx"), s.get("my")) == (520, 410), 15, m3)
        check((st.get("mx"), st.get("my")) == (520, 410), "pause: the pointer reaches the page after the resume",
              (st.get("mx"), st.get("my")))
    healthy(env, mark, "pause")


# ------------------------------------------------------------------ restart
def host_leftovers(env):
    """what the private page host could leave in the runtime directory: its socket directories"""
    rt = env.env["XDG_RUNTIME_DIR"]
    try:
        return sorted(n for n in os.listdir(rt) if n.startswith("walpe-web-"))
    except OSError:
        return []


def alive(pids):
    table = procs()
    return [p for p in pids if p in table and table[p][1] != "defunct"]


def case_restart(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "restart"):
        return
    mark = env.mark()
    mon = env.mons[0]
    st = open_page(env, W["still"], "still", "restart")
    if not st:
        return
    shows_page(env, mon)
    old = descendants(env.daemon.pid)
    check(len(page_procs(env)) == 1, "restart: one page process", page_procs(env))
    # --- ended and started again: the saved wallpaper is back
    env.stop_daemon()
    # (WebKit's helper processes end by themselves when the daemon's end of their
    # connection is closed: a moment later)
    t0 = time.time()
    check(bool(wait_for(lambda: not alive(old), 15, 0.1)), "restart: no process of the ended daemon is left",
          [procs().get(p) for p in alive(old)])
    note("restart: the last page helper process was gone %.1f s after the daemon" % (time.time() - t0))
    check(not host_leftovers(env), "restart: nothing of the page host is left in the runtime directory",
          host_leftovers(env))
    m1 = env.mark()
    if not check(env.start_daemon([("A", 1280, 720, 0, 0)], extra=daemon_env(env)), "restart: the daemon starts again"):
        return
    st2 = wait_state(env, "still", lambda s: s.get("user", 0) >= 1, 90, m1)
    check(bool(st2) and st2.get("born") != st.get("born"), "restart: the page is loaded again by the new daemon",
          (st.get("born"), st2.get("born")))
    ok, lk = shows_page(env, env.mons[0])
    check(ok, "restart: the output shows the page again", lk["bad"])
    # --- restart in place: web hardware acceleration is switched (the new image runs the
    # accelerated start-up test first; on a software renderer it then stays in software mode)
    pid = env.daemon.pid
    old = descendants(pid)
    m2 = env.mark()
    rcode, rep = env.ctl("set", "web_accel=on")
    check(rcode == 0, "restart: web_accel=on is accepted", rep)
    check(bool(wait_for(lambda: "restarting to apply it" in env.since(m2), 30)),
          "restart: the daemon restarts in place for it")
    st3 = wait_state(env, "still", lambda s: s.get("user", 0) >= 1 and s.get("born") != st2.get("born"), 150, m2)
    check(bool(st3) and st3.get("born") != st2.get("born"), "restart: the page is loaded again after the restart in place",
          web_status(env))
    check(env.daemon.poll() is None and env.daemon.pid == pid, "restart: it is the same process")
    ok, lk = shows_page(env, env.mons[0])
    check(ok, "restart: the output shows the page again", lk["bad"])
    check(bool(wait_for(lambda: not alive(old), 15)), "restart: the page processes of the old image are gone",
          [procs().get(p) for p in alive(old)])
    check(bool(wait_for(lambda: len(page_procs(env)) == 1, 15)), "restart: one page process again", page_procs(env))
    ws = web_status(env)
    check(str(ws.get("web_host", "")) == want_host(env), "restart: the page is on the %s page host again"
          % want_host(env), ws)
    check(not host_leftovers(env), "restart: nothing of the page host is in the runtime directory", host_leftovers(env))
    env.ctl("set", "web_accel=off")     # (the saved setting; this restarts once more)
    wait_for(lambda: env.since(m2).count("restarting to apply it") >= 2, 30)
    wait_state(env, "still", lambda s: s.get("born") != st3.get("born"), 150, m2)
    healthy(env, mark, "restart")


# -------------------------------------------------------------------- crash
def case_crash(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "crash"):
        return
    mark = env.mark()
    mon = env.mons[0]
    st = open_page(env, W["still"], "still", "crash")
    if not st:
        return
    shows_page(env, mon)
    born = st.get("born")
    for n in (1, 2, 3):
        pids = page_procs(env)
        if not check(len(pids) == 1, "crash: one page process to kill (%d)" % n, pids):
            return
        m1 = env.mark()
        os.kill(pids[0], signal.SIGKILL)
        said = wait_for(lambda: "reloading it" in env.since(m1), 30)
        check(bool(said), "crash: the daemon says that the page process died and that it loads the page again (%d)" % n,
              [ln for ln in env.since(m1).splitlines() if "web:" in ln][-3:])
        st = wait_state(env, "still", lambda s: s.get("user", 0) >= 1 and s.get("born") != born, 60, m1)
        check(bool(st) and st.get("born") != born, "crash: the page is back (%d)" % n, web_status(env))
        born = st.get("born", born)
        check(bool(wait_for(lambda: page_procs(env) and page_procs(env) != pids, 15)),
              "crash: with a new page process (%d)" % n, page_procs(env))
    ok, lk = shows_page(env, mon)
    check(ok, "crash: the output shows the page after three crashes", lk["bad"])
    # the fourth within a minute: given up, with a note
    pids = page_procs(env)
    m2 = env.mark()
    if pids:
        os.kill(pids[0], signal.SIGKILL)
    check(bool(wait_for(lambda: "not reloading it" in env.since(m2), 30)),
          "crash: the fourth crash within a minute is not followed by another reload",
          [ln for ln in env.since(m2).splitlines() if "web:" in ln][-3:])
    ws = web_status(env)
    check("keeps crashing" in str(ws.get("web_error", "")), "crash: status says so (web_error)", ws.get("web_error"))
    check(env.daemon.poll() is None, "crash: the daemon lives")
    # opened again: a fresh start
    m3 = env.mark()
    env.open(W["still"])
    st = wait_state(env, "still", lambda s: s.get("user", 0) >= 1 and s.get("born") != born, 60, m3)
    check(bool(st) and st.get("born") != born, "crash: opening the wallpaper again loads the page afresh",
          web_status(env))
    check(not web_status(env).get("web_error"), "crash: and the note in status is gone", web_status(env).get("web_error"))
    ok, lk = shows_page(env, mon)
    check(ok, "crash: the output shows the page", lk["bad"])
    healthy(env, mark, "crash")


# -------------------------------------------------------------------- stale
def built_hosts(build):
    """the page hosts of the web module in this build, from its walpe-build-config.h"""
    try:
        with open(os.path.join(build, "walpe-build-config.h"), errors="replace") as f:
            lines = [ln.split() for ln in f.read().splitlines()]
    except OSError:
        return set()
    return {host for host, name in (("x11", "HAVE_WEBHOST_X11"), ("private", "HAVE_WEBHOST_WL"))
            if any(t[:2] == ["#define", name] and t[2:3] != ["0"] for t in lines)}


def free_x_display():
    """an X display number nobody listens on (neither the socket file nor the abstract name)"""
    try:
        with open("/proc/net/unix") as f:
            used = f.read()
    except OSError:
        used = ""
    for n in range(4200 + os.getpid() % 500, 4200 + os.getpid() % 500 + 200):
        if not os.path.exists("/tmp/.X11-unix/X%d" % n) and ("/tmp/.X11-unix/X%d\n" % n) not in used:
            return ":%d" % n
    return ":4199"


def case_stale(env, W):
    if env.display.kind != "sway" or HOST != "auto":
        print("skip stale (the choice of the page host on a Wayland backend: --display sway, --host auto)")
        return
    stale = free_x_display()
    if not start(env, [("A", 1280, 720, 0, 0)], "stale", extra={"DISPLAY": stale}):
        return
    mark = env.mark()
    mon = env.mons[0]
    names = env_names(env.daemon.pid)
    check(names is not None and "DISPLAY" in names, "stale: the daemon's environment has DISPLAY (%s, no X server there)"
          % stale)
    st = open_page(env, W["still"], "still", "stale", timeout=150)
    if not st:
        print(env.since(mark)[-2000:])
        return
    ws = web_status(env)
    host = str(ws.get("web_host", ""))
    log = env.since(0)
    if "x11" in built_hosts(env.build):
        check(host.startswith("private (") and "X11 page host" in host,
              "stale: status says the page is on the private page host, and why", ws)
        check("the X11 page host did not come up" in log and "using the private page host instead" in log,
              "stale: the log says that the X11 page host did not come up and what is used instead",
              [ln for ln in log.splitlines() if "page host" in ln][-4:])
    else:
        check(host == "private", "stale: (a build without the X11 page host) the page is on the private page host", ws)
        check("X11 page host" not in log, "stale: and nothing is said about an X11 page host")
    check(not ws.get("web_error"), "stale: web wallpapers are not switched off (no web_error)", ws.get("web_error"))
    ok, lk = shows_page(env, mon, TINT_RGB[TINT0])
    check(ok, "stale: the output shows the page", lk["bad"])
    healthy(env, mark, "stale")


# ----------------------------------------------------------------- leftover
def case_leftover(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "leftover"):
        return
    mark = env.mark()
    rt = env.env["XDG_RUNTIME_DIR"]
    before = set(os.listdir(rt))
    if not open_page(env, W["still"], "still", "leftover"):
        return
    pid = env.daemon.pid
    check(not host_leftovers(env), "leftover: while the page runs the page host has no socket or directory left in "
          "the runtime directory", host_leftovers(env))
    if want_host(env) == "private":
        # the listening socket is closed once the toolkit is connected: nothing to connect to
        # (/proc/net/unix: "Num RefCount Protocol Flags Type St Inode Path"; a listening
        # socket has the flag 00010000. The one connection that was accepted keeps the
        # path's name without being reachable through it.)
        listening = []
        try:
            with open("/proc/net/unix") as f:
                for ln in f.read().splitlines():
                    t = ln.split()
                    if len(t) >= 8 and "walpe-web-" in t[7] and t[3] == "00010000":
                        listening.append(t[7])
        except OSError:
            pass
        check(not listening, "leftover: no socket of the page host is listening", listening)
    kids = descendants(pid)
    check(bool(page_procs(env)), "leftover: (a page process runs)")
    env.ctl("quit")
    gone = wait_for(lambda: env.daemon.poll() is not None, 20, 0.1)
    check(bool(gone), "leftover: walpe-ctl quit ends the daemon", None)
    if gone:
        check(env.daemon.returncode == 0, "leftover: with exit status 0", env.daemon.returncode)
        env.daemon = None
    check(bool(wait_for(lambda: not alive(kids), 15)), "leftover: none of its processes is left",
          [procs().get(p) for p in alive(kids)])
    after = set(os.listdir(rt))
    # (WebKit and GLib keep files of their own there: dconf, .flatpak)
    mine = sorted(n for n in after - before if n.startswith("walpe"))
    check(not mine, "leftover: the runtime directory holds nothing of the daemon or the page host", mine)
    bad = [ln for ln in env.since(mark).splitlines() if TROUBLE.search(ln)]
    check(not bad, "leftover: the log is clean", bad[:5])


# ------------------------------------------------------------------ sandbox
def case_sandbox(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "sandbox"):
        return
    mark = env.mark()
    if not open_page(env, W["still"], "still", "sandbox"):
        return
    pids = page_procs(env)
    if not check(len(pids) == 1, "sandbox: one page process", pids):
        return
    pid = pids[0]
    status = {}
    try:
        with open("/proc/%d/status" % pid) as f:
            for ln in f:
                k, _, v = ln.partition(":")
                status[k] = v.split()
    except OSError:
        pass
    if len(status.get("NSpid", [])) < 2:
        # (WebKit's sandbox needs bubblewrap and user namespaces; where they are not
        # there WebKit runs its page processes without one, whatever the page host)
        note("sandbox: the page process is not in a namespace of its own here (no bubblewrap sandbox on this "
             "machine): nothing to check")
        healthy(env, mark, "sandbox")
        return
    check(status.get("Seccomp") == ["2"], "sandbox: the page process runs under a seccomp filter", status.get("Seccomp"))
    rt = env.env["XDG_RUNTIME_DIR"]
    root = "/proc/%d/root" % pid
    try:
        seen = sorted(os.listdir(root + rt))
    except OSError:
        seen = None
    if seen is None:
        note("sandbox: the page process's files cannot be looked at from here")
    else:
        check("walpe.sock" not in seen, "sandbox: the page process does not see the daemon's control socket", seen)
        check(not [n for n in seen if n.startswith("walpe-web-")],
              "sandbox: nor anything of the page host", seen)
        try:
            xs = sorted(os.listdir(root + "/tmp/.X11-unix"))
        except OSError:
            xs = []
        if env.display.kind == "sway" and want_host(env) == "private":
            # WebKit would bind $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY into the sandbox; the
            # daemon keeps it from finding the session's compositor there (main.c,
            # web_env_wayland_socket)
            wl = os.path.basename(env.display.display)
            check(wl not in seen, "sandbox: the page process does not see the compositor's socket (%s)" % wl, seen)
            check(not xs, "sandbox: and no X server's socket", xs)
        else:
            note("sandbox: X server sockets the page process sees (WebKit binds the one of the display its "
                 "toolkit is on): %s" % (xs or "none"))
        note("sandbox: of the runtime directory the page process sees %s" % seen)
    healthy(env, mark, "sandbox")


# ------------------------------------------------------------------ widgets
# everything a click could open as a window of its own, each in its own cell of a grid;
# the page counts the clicks it saw and says what its requests came to
WIDGETS = ["sel", "file", "color", "open", "full", "dialog", "link", "drag", "text"]
WIDGET_PAGE = """<!doctype html><html><head><meta charset="utf-8"><style>
html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#182028}
.c{position:absolute;top:20%;height:20%;width:9%;box-sizing:border-box;font:16px sans-serif}
</style></head><body>
<select class="c" id="sel" style="left:1%"><option>one</option><option>two</option><option>three</option></select>
<input class="c" id="file" type="file" style="left:11%">
<input class="c" id="color" type="color" style="left:21%">
<button class="c" id="open" style="left:31%">open</button>
<button class="c" id="full" style="left:41%">full</button>
<button class="c" id="dialog" style="left:51%">dialog</button>
<a class="c" id="link" href="https://walpe-check.invalid/away" style="left:61%;background:#446">link</a>
<div class="c" id="drag" draggable="true" style="left:71%;background:#644">drag</div>
<input class="c" id="text" value="PAGE-TEXT" style="left:81%">
<script>
var S = {clicks: {}, n: 0, ctx: 0, opened: null, full: null, dialog: 0, drag: 0, sel: 0, born: Date.now(),
         href: location.pathname.split('/').pop()};
function say() { S.fs = !!document.fullscreenElement; S.vis = document.visibilityState; S.len = history.length;
  S.w = innerWidth; S.h = innerHeight;
  console.log('WCwidgets ' + JSON.stringify(S)); }
document.addEventListener('click', function (e) { var id = e.target.id || '?'; S.clicks[id] = (S.clicks[id] || 0) + 1; S.n++; say(); }, true);
document.addEventListener('contextmenu', function (e) { S.ctx++; say(); }, true);
document.addEventListener('dragstart', function (e) { S.drag++; }, true);
document.getElementById('sel').addEventListener('change', function () { S.sel++; });
document.getElementById('open').addEventListener('click', function () {
  try { var w = window.open('about:blank', '_blank', 'width=300,height=200'); S.opened = !!w; } catch (e) { S.opened = 'error'; } });
document.getElementById('full').addEventListener('click', function () {
  try { document.documentElement.requestFullscreen().then(function () { S.full = 'granted'; say(); },
                                                         function () { S.full = 'refused'; say(); }); }
  catch (e) { S.full = 'error'; } });
document.getElementById('dialog').addEventListener('click', function () {
  alert('a dialog'); var c = confirm('another'); var p = prompt('a third', 'dflt'); S.dialog++; S.answers = [c, p]; });
window.addEventListener('load', function () { S.user = 1; say(); });
setInterval(say, 500);
</script></body></html>
"""


def gen_widgets(out):
    os.makedirs(out)
    rc.jdump(os.path.join(out, "project.json"), {"type": "web", "title": "webcheck widgets", "file": "index.html"})
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write(WIDGET_PAGE)


def case_widgets(env, W):
    if not start(env, [("A", 1280, 720, 0, 0)], "widgets"):
        return
    mark = env.mark()
    mon = env.mons[0]
    env.open(W["widgets"])
    st = wait_state(env, "widgets", lambda s: s.get("user") == 1, 90, mark)
    if not check(bool(st), "widgets: the page loaded", web_status(env)):
        return
    born = st.get("born")
    time.sleep(1.0)
    windows0 = env.display.windows() if env.display.kind == "sway" else []
    for i, wid in enumerate(WIDGETS):
        x, y = int(1280 * (0.01 + 0.10 * i + 0.045)), int(720 * 0.30)
        m1 = env.mark()
        env.ctl("click", mon[3] + x, mon[4] + y)
        st = wait_state(env, "widgets", lambda s: s.get("clicks", {}).get(wid, 0) >= 1, 20, m1)
        check(st.get("clicks", {}).get(wid, 0) >= 1, "widgets: the click on '%s' reached the page" % wid,
              st.get("clicks"))
        time.sleep(0.5)
        check(env.daemon.poll() is None, "widgets: the daemon lives after it")
        if env.daemon.poll() is not None:
            print(env.since(m1)[-1500:])
            return
    # a right click: the page's own event, no menu of the toolkit
    m2 = env.mark()
    env.ctl("click", mon[3] + 640, mon[4] + 500, 3)
    st = wait_state(env, "widgets", lambda s: s.get("ctx", 0) >= 1, 20, m2)
    check(st.get("ctx", 0) >= 1, "widgets: a right click reaches the page as contextmenu", st.get("ctx"))
    # a press, a walk and a release over the draggable box
    if env.display.kind == "sway" and env.display.can_point():
        bx, by = int(1280 * 0.755), int(720 * 0.30)
        env.display.pointer(mon[3] + bx, mon[4] + by, env.env)
        try:
            env.display.ptr.button(0x110, True)
            for d in (10, 40, 90, 160):
                env.display.pointer(mon[3] + bx - d, mon[4] + by + d, env.env)
                time.sleep(0.1)
            env.display.ptr.button(0x110, False)
        except (OSError, AttributeError):
            pass
    time.sleep(1.5)
    st = state(env, "widgets", fresh=True)
    check(st.get("born") == born and st.get("href") == "index.html",
          "widgets: the page is still the page it was (no navigation, no reload)", (st.get("href"), st.get("len")))
    check(st.get("opened") in (False, None, "error"), "widgets: window.open gave the page no window", st.get("opened"))
    # (WebKit lets the element count as fullscreen for the page; what is kept from it is
    # the window going fullscreen, so nothing may have changed its size)
    check((st.get("w"), st.get("h")) == (mon[1], mon[2]), "widgets: a fullscreen request changed nothing of the "
          "page's size", (st.get("w"), st.get("h"), st.get("full")))
    check(st.get("dialog", 0) >= 1, "widgets: alert / confirm / prompt came back at once", st.get("dialog"))
    check(st.get("vis") == "visible", "widgets: the page is still visible to itself", st.get("vis"))
    if env.display.kind == "sway":
        now = env.display.windows()
        check(len(now) == len(windows0), "widgets: no window appeared in the compositor", now)
    log = env.since(mark)
    check("Gtk-CRITICAL" not in log and "Gdk-CRITICAL" not in log, "widgets: no critical warning of the toolkit",
          [ln for ln in log.splitlines() if "CRITICAL" in ln][:3])
    # still answering: one more click is counted
    m3 = env.mark()
    n0 = st.get("n", 0)
    env.ctl("click", mon[3] + 640, mon[4] + 600)
    st = wait_state(env, "widgets", lambda s: s.get("n", 0) > n0, 20, m3)
    check(st.get("n", 0) > n0, "widgets: the page still takes clicks afterwards", (n0, st.get("n")))
    healthy(env, mark, "widgets")


# ---------------------------------------------------------------- clipboard
CLIP_PAGE = """<!doctype html><html><body style="background:#223"><p id="p">text of the page</p>
<input id="i" value="PAGE-TEXT" style="position:absolute;left:10%;top:40%;width:30%;height:10%">
<script>
var S = {down: 0, copy: null, wrote: null, read: null, paste: null, sel: 0, born: Date.now()};
function say() { console.log('WCclip ' + JSON.stringify(S)); }
setInterval(function () { var i = document.getElementById('i'); i.focus(); i.select();
  var r = document.createRange(); r.selectNodeContents(document.getElementById('p'));
  var s = getSelection(); s.removeAllRanges(); s.addRange(r); i.select(); S.sel++; }, 150);
document.addEventListener('mousedown', function (e) { var i = document.getElementById('i'); i.focus(); i.select();
  S.down++;
  try { S.copy = document.execCommand('copy'); } catch (x) { S.copy = 'error'; }
  try { i.value = ''; S.paste = document.execCommand('paste') ? i.value : false; i.value = 'PAGE-TEXT'; } catch (x) { S.paste = 'error'; }
  if (navigator.clipboard) {
    navigator.clipboard.writeText('PAGE-CLIP').then(function () { S.wrote = true; say(); }, function () { S.wrote = false; say(); });
    navigator.clipboard.readText().then(function (t) { S.read = t; say(); }, function () { S.read = false; say(); });
  }
  say(); });
window.addEventListener('load', function () { S.user = 1; say(); });
setInterval(say, 500);
</script></body></html>
"""


def gen_clip(out):
    os.makedirs(out)
    rc.jdump(os.path.join(out, "project.json"), {"type": "web", "title": "webcheck clipboard", "file": "index.html"})
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write(CLIP_PAGE)


def case_clipboard(env, W):
    import shutil
    if env.display.kind != "sway":
        print("skip clipboard (the session's Wayland clipboard: --display sway; X selections are tests/websec_test.sh's)")
        return
    if not shutil.which("wl-copy") or not shutil.which("wl-paste"):
        print("skip clipboard (needs wl-copy and wl-paste)")
        return
    cenv = dict(env.env)

    def paste(*opt):
        try:
            p = subprocess.run(["wl-paste", "-n"] + list(opt), env=cenv, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=10 * PATIENCE)
            return p.stdout.decode(errors="replace") if p.returncode == 0 else None
        except (OSError, subprocess.TimeoutExpired):
            return None
    owners = []
    try:
        # another program of the session owns both (wl-copy stays to serve them)
        for opt, text in (([], "USER-CLIP"), (["--primary"], "USER-PRIM")):
            owners.append(subprocess.Popen(["wl-copy", "--foreground"] + opt + [text], env=cenv,
                                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                           stderr=subprocess.DEVNULL))
        if not wait_for(lambda: paste() == "USER-CLIP" and paste("--primary") == "USER-PRIM", 15):
            print("skip clipboard (the session's clipboard could not be set here: %r, %r)" % (paste(), paste("--primary")))
            return
        if not start(env, [("A", 1280, 720, 0, 0)], "clipboard"):
            return
        mark = env.mark()
        mon = env.mons[0]
        env.open(W["clip"])
        st = wait_state(env, "clip", lambda s: s.get("user") == 1 and s.get("sel", 0) >= 5, 90, mark)
        if not check(bool(st), "clipboard: the page loaded and selects text on a timer", web_status(env)):
            return
        for x, y in ((400, 300), (420, 320)):
            env.ctl("click", mon[3] + x, mon[4] + y)
            time.sleep(1.0)
        st = wait_state(env, "clip", lambda s: s.get("down", 0) >= 2 and s.get("read") is not None, 20, mark)
        check(st.get("down", 0) >= 2, "clipboard: the clicks reached the page (it copies in each)", st.get("down"))
        time.sleep(1.0)
        check(paste() == "USER-CLIP", "clipboard: the session's clipboard is still the other program's", paste())
        check(paste("--primary") == "USER-PRIM", "clipboard: so is the primary selection", paste("--primary"))
        check(all(o.poll() is None for o in owners), "clipboard: neither owner was told that it lost its selection",
              [o.poll() for o in owners])
        st = state(env, "clip", fresh=True)
        seen = [v for v in (st.get("read"), st.get("paste")) if isinstance(v, str)]
        check(not any("USER-" in v for v in seen), "clipboard: the page read nothing of either", seen)
        note("clipboard: the page's own account: copy %r, writeText %r, readText %r, paste %r"
             % (st.get("copy"), st.get("wrote"), st.get("read"), st.get("paste")))
        healthy(env, mark, "clipboard")
    finally:
        for o in owners:
            o.terminate()
            try:
                o.wait(timeout=5)
            except subprocess.TimeoutExpired:
                o.kill()


# ------------------------------------------------------------------ offline
OFFLINE_PAGE = """<!doctype html><html><body style="background:#232"><script>
var S = {own: null, other: null, local: null, ws: null, born: Date.now()}, O = 'http://127.0.0.1:%PORT%';
function say() { console.log('WCoffline ' + JSON.stringify(S)); }
fetch('own.txt').then(function (r) { return r.text(); }).then(
  function (t) { S.own = t.indexOf('own file') == 0 ? 'ok' : 'wrong'; say(); }, function (e) { S.own = 'failed'; say(); });
function other(key, url) { fetch(url).then(function (r) { return r.text(); }).then(
  function (t) { S[key] = t == 'ok' ? 'REACHED' : 'blocked'; say(); }, function () { S[key] = 'blocked'; say(); }); }
other('other', O + '/a'); other('local', 'http://localhost:%PORT%/b');
try { var w = new WebSocket('ws://127.0.0.1:%PORT%/ws'); w.onopen = function () { S.ws = 'OPEN'; say(); };
      w.onerror = function () { S.ws = 'blocked'; say(); }; } catch (e) { S.ws = 'blocked'; }
new Image().src = O + '/img';
window.addEventListener('load', function () { S.user = 1; say(); });
setInterval(say, 500);
</script></body></html>
"""


def case_offline(env, W):
    import socket, threading
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(16)
    srv.settimeout(0.5)
    port = srv.getsockname()[1]
    got, stop = [], threading.Event()

    def serve():
        while not stop.is_set():
            try:
                c, _ = srv.accept()
            except OSError:
                continue
            try:
                c.settimeout(2)
                got.append(c.recv(200).split(b"\r\n")[0])
                c.sendall(b"HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 2\r\n"
                          b"Connection: close\r\n\r\nok")
            except OSError:
                got.append(b"(connected)")
            c.close()
    th = threading.Thread(target=serve, daemon=True)
    th.start()
    out = W["offline"]
    try:
        if not os.path.isdir(out):
            os.makedirs(out)
            rc.jdump(os.path.join(out, "project.json"), {"type": "web", "title": "webcheck offline", "file": "index.html"})
            with open(os.path.join(out, "own.txt"), "w") as f:
                f.write("own file\n")
        with open(os.path.join(out, "index.html"), "w") as f:
            f.write(OFFLINE_PAGE.replace("%PORT%", str(port)))
        if not start(env, [("A", 1280, 720, 0, 0)], "offline", extra={"WALPE_WEB_OFFLINE": "1"}):
            return
        mark = env.mark()
        env.open(out)
        st = wait_state(env, "offline", lambda s: s.get("user") == 1 and all(s.get(k) for k in ("own", "other", "local", "ws")),
                        90, mark)
        if not check(bool(st), "offline: the page loaded and its requests ended", web_status(env)):
            return
        check(st.get("own") == "ok", "offline: the page's own file loads with network access off", st.get("own"))
        check(st.get("other") == "blocked" and st.get("local") == "blocked" and st.get("ws") == "blocked",
              "offline: the page says it got through to no other port of this machine",
              {k: st.get(k) for k in ("other", "local", "ws")})
        time.sleep(1.0)
        check(not got, "offline: and nothing connected to the other port", got[:4])
        check("network access is off" in env.since(mark) or "network access is off" in env.since(0),
              "offline: the log says that network access is off")
        healthy(env, mark, "offline")
    finally:
        stop.set()
        th.join(timeout=3)
        srv.close()


CASES = [("start", case_start), ("live", case_live), ("props", case_props), ("audio", case_audio),
         ("pointer", case_pointer), ("two", case_two), ("outputs", case_outputs), ("pause", case_pause),
         ("restart", case_restart), ("crash", case_crash), ("stale", case_stale), ("leftover", case_leftover),
         ("sandbox", case_sandbox), ("widgets", case_widgets), ("clipboard", case_clipboard),
         ("offline", case_offline)]
# cases that need outputs that can be made, changed and removed
NEED_OUTPUTS = ("two", "outputs")


def main():
    global HOST, HASHES
    args = sys.argv[1:]
    build, daemon, keep, tmp, only, display = "build", None, False, None, None, "sway"
    while args:
        a = args.pop(0)
        if a == "--build" and args:
            build = args.pop(0)
        elif a == "--daemon" and args:
            daemon = args.pop(0)
        elif a == "--display" and args:
            display = args.pop(0)
        elif a == "--host" and args:
            HOST = args.pop(0)
        elif a == "--hashes" and args:
            HASHES = os.path.abspath(args.pop(0))
        elif a == "--keep":
            keep = True
        elif a == "--tmp" and args:
            tmp = args.pop(0)
        elif a == "--only" and args:
            only = args.pop(0).split(",")
        else:
            sys.stderr.write(__doc__)
            return 2
    display = displayenv.pick(display)
    if not display or HOST not in ("auto", "private", "x11"):
        sys.stderr.write(__doc__)
        return 2
    skip = displayenv.unusable(display, build)
    if skip:
        print("webcheck: skipped (%s)" % skip[0])
        return skip[1]
    for b in (daemon or os.path.join(build, "walpe-daemon"), os.path.join(build, "walpe-ctl")):
        if not os.path.exists(b):
            print(f"webcheck: {b} not found")
            return 2
    if not os.path.exists(os.path.join(build, "walpe-web.so")):
        print("webcheck: skipped (this build has no web module)")
        return displayenv.NOT_BUILT
    env = lc.Env(build, daemon, keep, tmp, display)
    try:
        if display == "sway":
            # as in a session: one runtime directory for the compositor and its clients,
            # and WAYLAND_DISPLAY a name in it (set after the compositor is up)
            os.rmdir(env.env["XDG_RUNTIME_DIR"])
            env.env["XDG_RUNTIME_DIR"] = env.display.rt
            env.sock = os.path.join(env.display.rt, "walpe.sock")
        longest = max(len(env.sock.encode()), len(os.path.join(env.display.rt, "sway-ipc.4294967295.4194304.sock"))
                      if display == "sway" else 0)
        if longest > 107:
            print(f"webcheck: {env.tmp} is too long for the sockets below it ({longest} > 107 bytes): "
                  "pass --tmp <a short directory>")
            return 2
        wp = os.path.join(env.tmp, "wallpapers")
        W = {k: os.path.join(wp, k) for k in ("still", "live", "other", "widgets", "clip", "offline")}
        gen_page(W["still"], "still", False)
        gen_page(W["live"], "live", True)
        gen_page(W["other"], "other", False)
        gen_widgets(W["widgets"])
        gen_clip(W["clip"])
        if not env.start_x(1280, 720):
            print("webcheck: skipped (the %s display did not start)" % display)
            if display == "sway":
                print(env.display.log_tail())
            return displayenv.SKIP
        if display == "sway":
            env.env["WAYLAND_DISPLAY"] = os.path.basename(env.display.display)
            env.env.pop("DISPLAY", None)
        for name, fn in CASES:
            if only and name not in only:
                continue
            if name in NEED_OUTPUTS and display != "sway":
                print("skip %s (needs outputs that can be made and removed: --display sway)" % name)
                continue
            try:
                fn(env, W)
            finally:
                env.stop_daemon()
    finally:
        env.close()
    print(f"webcheck: {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
