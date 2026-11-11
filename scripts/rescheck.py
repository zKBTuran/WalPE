#!/usr/bin/env python3
"""End-to-end check of the render resolution per monitor (a harness of its own).

  python3 -I scripts/rescheck.py --build build [--daemon PATH] [--keep] [--tmp DIR]

Starts a private Xvfb (-displayfd, its framebuffer in a file: no screenshot
tool is needed) and a private walpe-daemon (own XDG dirs) whose monitor list
comes from a file (WALPE_MONITORS=@FILE), so monitors can be "plugged in" and
"removed" under the running daemon. Checks, through the control socket,
walpe-ctl, config.json and the pixels on screen:

  fresh     a new installation: every monitor at its own resolution, none
            asked about, nothing written; the listing and what it offers
  refuse    values that are not values, monitors that do not exist, a list
            with one bad element: refused with a reason, nothing changes
  set       a smaller render size on one monitor: status, config.json, the
            picture still fills the monitor with the same geometry; two
            same-size monitors stop sharing one rendering and share it again
            at the same size; scale_filter; texture_limit
  hotplug   a monitor appears: native, "not asked yet", the choices of the
            others kept across the daemon's restart; it goes and comes back:
            still not asked; an answer ends it; --forget brings it back
  upgrade   a config.json of format 3: nobody is asked, every connected
            monitor entered as native, videos stay within 1080p; a monitor
            that appears later is asked about
  env       WALPE_RESOLUTION decides for a run and saves nothing

Exit code 0 = all passed, 1 = failures, 77 = skipped (no Xvfb, or no libX11
for ctypes: a layout change cannot be announced). RESCHECK_PATIENCE=N
multiplies every wait (a software renderer on a busy machine: 3 or more).
--tmp DIR: work in DIR (must not exist; removed afterwards)."""
import json, mmap, os, shutil, socket, struct, subprocess, sys, tempfile, time

PATIENCE = max(1.0, float(os.environ.get("RESCHECK_PATIENCE", "1") or 1))
A = ("A", 800, 450, 0, 0)
B = ("B", 800, 450, 800, 0)
C = ("C", 400, 450, 1600, 0)
D = ("D", 400, 450, 2000, 0)
SCREEN = (2400, 450)

fails = 0
checks = 0


def check(ok, what, detail=None):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
    print(("ok   " if ok else "FAIL ") + what + ("" if ok or detail is None else f"   [{detail}]"), flush=True)
    return bool(ok)


def wait_for(fn, timeout=10.0, step=0.2):
    t0 = time.time()
    timeout *= PATIENCE
    while True:
        v = fn()
        if v or time.time() - t0 > timeout:
            return v
        time.sleep(step)


def jdump(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(obj, f, indent=1)


def gen_scene(out):
    """a 1920 x 1080 scene: blue all over, a red rectangle of half its size in the middle"""
    jdump(f"{out}/models/util/solidlayer.json", {"solidlayer": True})
    jdump(f"{out}/scene.json", {
        "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
        "general": {"ambientcolor": "0.3 0.3 0.3", "bloom": False, "cameraparallax": False,
                    "clearcolor": "0 0 0", "orthogonalprojection": {"height": 1080, "width": 1920},
                    "skylightcolor": "0.3 0.3 0.3"},
        "objects": [{"id": 40, "name": "fill", "image": "models/util/solidlayer.json",
                     "origin": "960 540 0", "size": "1920 1080", "scale": "1 1 1",
                     "angles": "0 0 0", "color": "0 0 1", "alpha": 1.0, "visible": True},
                    {"id": 41, "name": "mark", "image": "models/util/solidlayer.json",
                     "origin": "960 540 0", "size": "960 540", "scale": "1 1 1",
                     "angles": "0 0 0", "color": "1 0 0", "alpha": 1.0, "visible": True}]})
    jdump(f"{out}/project.json", {"file": "scene.json", "title": os.path.basename(out), "type": "scene"})


def poke_root(display):
    """a ConfigureNotify for the root window, as the X server sends when the
    screen is resized: the daemon looks at the monitor layout again
    (WALPE_MONITORS=@FILE is read anew). Xvfb's screen cannot be resized."""
    import ctypes
    import ctypes.util

    class XConfigureEvent(ctypes.Structure):
        _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                    ("display", ctypes.c_void_p), ("event", ctypes.c_ulong), ("window", ctypes.c_ulong),
                    ("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int),
                    ("height", ctypes.c_int), ("border_width", ctypes.c_int), ("above", ctypes.c_ulong),
                    ("override_redirect", ctypes.c_int)]

    class XEvent(ctypes.Union):
        _fields_ = [("xconfigure", XConfigureEvent), ("pad", ctypes.c_long * 24)]

    try:
        x = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
    except OSError:
        return False
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x.XDefaultRootWindow.restype = ctypes.c_ulong
    x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long,
                             ctypes.POINTER(XEvent)]
    x.XFlush.argtypes = [ctypes.c_void_p]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    d = x.XOpenDisplay(display.encode())
    if not d:
        return False
    root = x.XDefaultRootWindow(d)
    ev = XEvent()
    ev.xconfigure.type = 22            # ConfigureNotify
    ev.xconfigure.display = d
    ev.xconfigure.event = root
    ev.xconfigure.window = root
    ev.xconfigure.width = SCREEN[0]
    ev.xconfigure.height = SCREEN[1]
    x.XSendEvent(d, root, 0, 1 << 17, ctypes.byref(ev))     # StructureNotifyMask
    x.XFlush(d)
    x.XCloseDisplay(d)
    return True


class Env:
    def __init__(self, build, daemon, keep, tmp=None):
        self.build = os.path.abspath(build)
        self.daemon_bin = os.path.abspath(daemon) if daemon else os.path.join(self.build, "walpe-daemon")
        self.keep = keep
        if tmp:
            os.makedirs(tmp)            # must be new: it is removed afterwards
            self.tmp = os.path.realpath(tmp)
        else:
            # (the real path: the daemon saves wallpaper paths as it resolves them)
            self.tmp = os.path.realpath(tempfile.mkdtemp(prefix="walpe-rescheck-"))
        self.xvfb = None
        self.daemon = None
        self.fb = None
        self.env = dict(os.environ)
        for k, d in (("XDG_RUNTIME_DIR", "rt"), ("XDG_CONFIG_HOME", "cfg"), ("XDG_CACHE_HOME", "cache"),
                     ("XDG_DATA_HOME", "data"), ("HOME", "home")):
            p = os.path.join(self.tmp, d)
            os.makedirs(p, mode=0o700)
            self.env[k] = p
        # (this environment is the caller's: the daemon is told its window system by name, or
        # inside a Wayland session it would draw on the real desktop, not on the Xvfb started here)
        self.env.update({"LIBGL_ALWAYS_SOFTWARE": "1", "NO_AT_BRIDGE": "1", "WALPE_BACKEND": "x11"})
        for k in ("WALPE_PROP_PROBE", "WALPE_SKIP_IDS", "WALPE_NO_SCRIPTS", "WALPE_NO_SHARED_RENDER",
                  "WALPE_RESOLUTION", "WALPE_RENDER_SCALE", "WAYLAND_DISPLAY", "WAYLAND_SOCKET"):
            self.env.pop(k, None)
        self.sock = os.path.join(self.env["XDG_RUNTIME_DIR"], "walpe.sock")
        self.config = os.path.join(self.env["XDG_CONFIG_HOME"], "walpe", "config.json")
        self.log = os.path.join(self.tmp, "daemon.log")
        self.monfile = os.path.join(self.tmp, "monitors")
        self.mons = []

    def start_x(self):
        """Xvfb on a display number it picks itself (-displayfd)"""
        fbdir = os.path.join(self.tmp, "fb")
        os.makedirs(fbdir)
        r, w = os.pipe()
        try:
            self.xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0",
                                          f"{SCREEN[0]}x{SCREEN[1]}x24", "-nolisten", "tcp", "-s", "0",
                                          "-fbdir", fbdir],
                                         pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            os.close(r)
            os.close(w)
            return False
        os.close(w)
        num = b""
        t0 = time.time()
        os.set_blocking(r, False)
        while b"\n" not in num and time.time() - t0 < 20 * PATIENCE and self.xvfb.poll() is None:
            try:
                d = os.read(r, 32)
                if d:
                    num += d
                    continue
            except BlockingIOError:
                pass
            time.sleep(0.05)
        os.close(r)
        if b"\n" not in num:
            return False
        self.env["DISPLAY"] = ":" + num.decode().strip()
        self.fb = os.path.join(fbdir, "Xvfb_screen0")
        return bool(wait_for(lambda: os.path.exists(self.fb) and os.path.getsize(self.fb) > 100, 10))

    def stop_x(self):
        if self.xvfb:
            self.xvfb.terminate()
            try:
                self.xvfb.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.xvfb.kill()
            self.xvfb = None
        shutil.rmtree(os.path.join(self.tmp, "fb"), ignore_errors=True)

    def layout(self, mons):
        """the monitor list the daemon finds at its next look"""
        self.mons = list(mons)
        with open(self.monfile, "w") as f:
            f.write(",".join(f"{n}={w}x{h}+{x}+{y}@60" for n, w, h, x, y in mons))

    def start_daemon(self, mons, extra=None):
        self.layout(mons)
        env = dict(self.env)
        env["WALPE_MONITORS"] = "@" + self.monfile
        env.update(extra or {})
        log = open(self.log, "a")
        self.daemon = subprocess.Popen([self.daemon_bin], env=env, stdout=log, stderr=subprocess.STDOUT,
                                       cwd=self.tmp)
        log.close()
        for _ in range(int(150 * PATIENCE)):
            time.sleep(0.1)
            if self.daemon.poll() is not None:
                return False
            if os.path.exists(self.sock) and self.ipc({"cmd": "status"}):
                return True
        return False

    def stop_daemon(self):
        if self.daemon:
            self.ipc({"cmd": "quit"})
            try:
                self.daemon.wait(timeout=10 * PATIENCE)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
                self.daemon.wait()
            self.daemon = None
        if os.path.exists(self.sock):
            os.unlink(self.sock)

    def close(self):
        self.stop_daemon()
        self.stop_x()
        if not self.keep:
            shutil.rmtree(self.tmp, ignore_errors=True)
        else:
            print("kept", self.tmp)

    def ipc(self, req, timeout=6.0):
        """one request over the daemon socket -> reply dict or None"""
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(timeout * PATIENCE)
            s.connect(self.sock)
            s.sendall(json.dumps(req).encode() + b"\n")
            buf = b""
            while not buf.endswith(b"\n"):
                d = s.recv(65536)
                if not d:
                    break
                buf += d
            s.close()
            return json.loads(buf.decode())
        except Exception:
            return None

    def ctl(self, *args):
        """walpe-ctl ... -> (exit status, output text)"""
        p = None
        for _ in range(int(5 * PATIENCE)):
            p = subprocess.run([os.path.join(self.build, "walpe-ctl")] + [str(a) for a in args], env=self.env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60 * PATIENCE)
            if p.returncode != 2:
                break
            time.sleep(0.5)
        return p.returncode, p.stdout.decode(errors="replace")

    def state(self):
        return ((self.ipc({"cmd": "status"}) or {}).get("state")) or {}

    def mon(self, name, st=None):
        for m in (st or self.state()).get("monitors") or []:
            if m.get("name") == name:
                return m
        return {}

    def listing(self):
        return self.ipc({"cmd": "resolution"}) or {}

    def listed(self, name, ls=None):
        for m in (ls or self.listing()).get("monitors") or []:
            if m.get("name") == name:
                return m
        return {}

    def saved(self):
        """config.json as a dict ({} when it does not exist)"""
        try:
            return json.load(open(self.config))
        except Exception:
            return {}

    def mark(self):
        try:
            return os.path.getsize(self.log)
        except OSError:
            return 0

    def since(self, mark):
        try:
            with open(self.log, errors="replace") as f:
                f.seek(mark)
                return f.read()
        except OSError:
            return ""

    def pixel(self, x, y):
        """one pixel of the X screen, from Xvfb's framebuffer file (XWD) -> (r, g, b)"""
        with open(self.fb, "rb") as f:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            try:
                big = struct.unpack(">I", m[0:4])[0] < 65536
                e = ">" if big else "<"
                hd = struct.unpack(e + "25I", m[0:100])
                header_size, bpp, bpl, ncolors = hd[0], hd[11], hd[12], hd[19]
                msb, masks = hd[7] == 1, hd[14:17]
                off = header_size + ncolors * 12 + y * bpl + x * (bpp // 8)
                px = int.from_bytes(m[off:off + bpp // 8], "big" if msb else "little")
            finally:
                m.close()
        out = []
        for mask in masks:
            sh = (mask & -mask).bit_length() - 1
            out.append((px & mask) >> sh)
        return tuple(out)


def is_red(rgb):
    return rgb[0] > 150 and rgb[2] < 100


def is_blue(rgb):
    return rgb[2] > 150 and rgb[0] < 100


def mark_edges(env, mon):
    """where the red rectangle begins and ends on the monitor's middle row and
    middle column, monitor pixels: (left, right, top, bottom) or None"""
    _, w, h, x, y = mon
    row = [is_red(env.pixel(x + i, y + h // 2)) for i in range(w)]
    col = [is_red(env.pixel(x + w // 2, y + j)) for j in range(h)]
    if True not in row or True not in col:
        return None
    return (row.index(True), w - 1 - row[::-1].index(True), col.index(True), h - 1 - col[::-1].index(True))


def picture_ok(env, mon, tol, what):
    """the scene fills the monitor the way a scene does (its 1920 x 1080 cover
    the monitor, centred): blue at the corners, the red rectangle - the middle
    half of the scene - where that puts it, whatever size it was rendered at"""
    _, w, h, x, y = mon
    k = max(w / 1920.0, h / 1080.0)
    want = (max(0, round(w / 2 - 480 * k)), min(w, round(w / 2 + 480 * k)) - 1,
            max(0, round(h / 2 - 270 * k)), min(h, round(h / 2 + 270 * k)) - 1)

    def look():
        e = mark_edges(env, mon)
        corners = all(is_blue(env.pixel(x + cx, y + cy)) for cx, cy in
                      ((3, 3), (w - 4, 3), (3, h - 4), (w - 4, h - 4)))
        return bool(e) and corners and all(abs(a - b) <= tol for a, b in zip(e, want))
    ok = wait_for(look, 15, 0.3)
    return check(ok, what, (mark_edges(env, mon), want))


def loaded(env, names, path):
    """wait until these monitors show the wallpaper (their own load, or their leader's)"""
    def ok():
        st = env.state()
        ms = [env.mon(n, st) for n in names]
        return all(m.get("current") == path and m.get("type") == "scene" and not m.get("loading") for m in ms)
    return bool(wait_for(ok, 30, 0.3))


def restart_for(env, mons, n_expected):
    """change the layout, announce it, wait until the restarted daemon answers with it"""
    mark = env.mark()
    env.layout(mons)
    poke_root(env.env["DISPLAY"])
    seen = wait_for(lambda: "restarting to follow it" in env.since(mark), 40, 0.1)

    def up():
        ms = env.state().get("monitors") or []
        return len(ms) == n_expected and sorted(m.get("name") for m in ms) == sorted(m[0] for m in mons)
    return bool(seen) and bool(wait_for(up, 40, 0.2))


# ---------------------------------------------------------------------- fresh
def run_fresh(env, scene):
    st = env.state()
    check(st.get("resolution_unasked") == 2 and st.get("resolution_prompt") is True,
          "fresh: both monitors are not asked about yet, the question is allowed",
          (st.get("resolution_unasked"), st.get("resolution_prompt")))
    for m in (A, B):
        s = env.mon(m[0], st)
        check(s.get("mode") == [m[1], m[2]] and s.get("render_size") == [m[1], m[2]] and
              s.get("resolution") == "native" and s.get("resolution_asked") is False and
              s.get("texture_limit") == "auto", f"fresh: {m[0]} renders at its own mode, native, auto", s)
    check(st.get("scale_filter") == "bilinear" and st.get("video_resolution") == "auto" and
          "resolution_env" not in st, "fresh: bilinear, videos follow the monitors")
    check("monitors" not in env.saved(), "fresh: nothing is written for a monitor nobody answered for")
    ls = env.listing()
    check(ls.get("ok") is True and ls.get("unasked") == 2 and ls.get("prompt") is True, "listing: answers")
    lim = ls.get("limits") or {}
    check(lim.get("min_width") == 320 and lim.get("min_height") == 200 and 1024 <= lim.get("max_side", 0) <= 16384
          and lim.get("max_pixels") == 8192 * 8192, "listing: the limits of a custom size", lim)
    check([p.get("name") for p in ls.get("presets") or []] == ["720p", "1080p", "1440p", "4k"],
          "listing: the presets")
    la = env.listed("A", ls)
    check(la.get("connected") is True and la.get("index") == 0 and la.get("mode") == [800, 450] and
          la.get("render_percent") == 100 and la.get("asked") is False and la.get("texture_limit_px") == 0,
          "listing: monitor A", la)
    check([c.get("value") for c in la.get("choices") or []] == ["native"] and
          la["choices"][0].get("size") == [800, 450],
          "listing: a 450 line monitor is offered native only (no preset is that small)", la.get("choices"))
    check(ls.get("video_resolution") == "auto" and ls.get("video_box") == [800, 450],
          "listing: videos are limited to the largest render size", ls.get("video_box"))
    rc, out = env.ctl("resolution")
    check(rc == 0 and "not yet" in out and "2 monitor(s) not asked about yet" in out and "800x450" in out,
          "walpe-ctl resolution: the table and the hint", out[-300:])
    rc, out = env.ctl("resolution", "--json")
    check(rc == 0 and out.lstrip().startswith("{") and "\"choices\"" in out, "walpe-ctl resolution --json")

    rc, _ = env.ctl("open", scene)
    check(rc == 0 and loaded(env, ["A", "B"], scene), "the scene is on both monitors")
    check(wait_for(lambda: env.state().get("shared_followers") == 1, 15),
          "two monitors of one size at one render size share one rendering")
    picture_ok(env, A, 2, "picture: A at native")
    picture_ok(env, B, 2, "picture: B at native")


# --------------------------------------------------------------------- refuse
def run_refuse(env):
    before = json.dumps(env.listing(), sort_keys=True)
    cfg = json.dumps(env.saved(), sort_keys=True)
    bad = [
        ({"cmd": "resolution", "monitor": "A", "value": "big"}, "is not one of"),
        ({"cmd": "resolution", "monitor": "A", "value": "10x10"}, "too small"),
        ({"cmd": "resolution", "monitor": "A", "value": "99999x99999"}, "too large"),
        ({"cmd": "resolution", "monitor": "A", "value": 1080}, "\"value\" is"),
        ({"cmd": "resolution", "monitor": 9, "value": "native"}, "no such monitor"),
        ({"cmd": "resolution", "monitor": "", "value": "native"}, "not an output name"),
        ({"cmd": "resolution", "monitor": ["A"], "value": "native"}, "\"monitor\" is"),
        ({"cmd": "resolution", "monitor": "A"}, "nothing to set"),
        ({"cmd": "resolution", "monitor": "A", "value": "native", "forget": True}, "goes alone"),
        ({"cmd": "resolution", "monitor": "A", "texture_limit": "huge"}, "texture_limit"),
        ({"cmd": "resolution", "monitor": "A", "texture_limit": 7}, "out of range"),
        ({"cmd": "resolution", "monitor": "A", "asked": "yes"}, "\"asked\" is"),
        ({"cmd": "resolution", "prompt": "no"}, "\"prompt\" is"),
        ({"cmd": "resolution", "set": 5}, "\"set\" is a list"),
        ({"cmd": "resolution", "set": [5]}, "is an object"),
        ({"cmd": "resolution", "set": [{"monitor": "A", "value": "400x225"},
                                       {"monitor": "B", "value": "nope"}]}, "is not one of"),
        ({"cmd": "set", "resolution": "A:1081q"}, "is not one of"),
        ({"cmd": "set", "resolution": "A:1080p,720p"}, "<monitor>:<value>"),
        ({"cmd": "set", "resolution": 1080}, "needs a name or text"),
        ({"cmd": "set", "fps": 31, "resolution": "nope"}, "is not one of"),
        ({"cmd": "set", "texture_limit": "A:5"}, "out of range"),
        ({"cmd": "set", "scale_filter": "cubic"}, "bilinear or nearest"),
        ({"cmd": "set", "video_resolution": "gigantic"}, "video_resolution"),
        ({"cmd": "set", "resolution_prompt": "off"}, "is a switch"),
    ]
    for req, text in bad:
        r = env.ipc(req) or {}
        check(r.get("ok") is False and text in (r.get("error") or ""),
              "refused: " + json.dumps(req)[:86], r.get("error"))
    check(json.dumps(env.listing(), sort_keys=True) == before and
          json.dumps(env.saved(), sort_keys=True) == cfg and env.state().get("fps") != 31,
          "refused: nothing changed, also not the key in front of a bad one, nothing saved")
    rc, out = env.ctl("set", "resolution=A:1081q")
    check(rc == 3 and "is not one of" in out, "walpe-ctl set with a bad value: exit 3 and the reason", out[-200:])
    rc, out = env.ctl("resolution", "A", "1081q")
    check(rc == 1 and "is not one of" in out, "walpe-ctl resolution with a bad value: exit 1, nothing sent")


# ------------------------------------------------------------------------ set
def run_set(env, scene):
    mark = env.mark()
    r = env.ipc({"cmd": "resolution", "monitor": "A", "value": "400x225"}) or {}
    la = env.listed("A", r)
    check(r.get("ok") is True and la.get("resolution") == "400x225" and la.get("render_size") == [400, 225] and
          la.get("render_percent") == 25 and la.get("asked") is True and r.get("unasked") == 1,
          "set: A at 400x225 (the reply is the listing)", la)
    st = env.state()
    sa, sb = env.mon("A", st), env.mon("B", st)
    check(sa.get("render_size") == [400, 225] and sa.get("mode") == [800, 450] and
          sa.get("resolution_asked") is True and sb.get("render_size") == [800, 450] and
          sb.get("resolution_asked") is False and st.get("resolution_unasked") == 1,
          "set: status has the mode and the render size of each monitor")
    sv = env.saved()
    check(sv.get("config_version") == 4 and sv.get("monitors") ==
          {"A": {"resolution": "400x225", "texture_limit": "auto", "asked": True}},
          "set: config.json has A's choice and no entry for B", sv.get("monitors"))
    check(wait_for(lambda: env.state().get("shared_followers") == 0, 20),
          "set: monitors at different render sizes no longer share a rendering")
    check(loaded(env, ["A", "B"], scene), "set: both show the scene (A's was loaded again)")
    picture_ok(env, A, 4, "picture: A rendered at a quarter of its pixels still fills it, same geometry")
    picture_ok(env, B, 2, "picture: B is as it was")
    check("no 400x225 render target" not in env.since(mark) and "eglSwapBuffers failed" not in env.since(mark),
          "set: the render target of that size was made")

    rc, out = env.ctl("set", "scale_filter=nearest")
    check(rc == 0 and wait_for(lambda: env.state().get("scale_filter") == "nearest", 5),
          "scale_filter=nearest is taken")
    picture_ok(env, A, 4, "picture: A scaled with whole pixels, same geometry")
    check(env.saved().get("scale_filter") == "nearest", "... and saved")
    env.ctl("set", "scale_filter=bilinear")
    check("scale_filter" not in env.saved(), "scale_filter back at its default leaves config.json")

    # larger than the monitor (supersampling): accepted, with a note about the cost
    r = env.ipc({"cmd": "resolution", "monitor": 1, "value": "1600x900"}) or {}
    check(r.get("ok") is True and env.listed("B", r).get("render_size") == [1600, 900] and
          "4.0 times" in (r.get("note") or ""), "set: B at twice its mode, with a note about the cost",
          r.get("note"))
    check(loaded(env, ["A", "B"], scene), "... loaded")
    picture_ok(env, B, 3, "picture: B rendered at four times its pixels, scaled down, same geometry")

    # both back at native: one rendering again, the picture as at the start
    rc, out = env.ctl("resolution", "all", "native")
    check(rc == 0 and "native" in out and env.listing().get("unasked") == 0,
          "walpe-ctl resolution all native: both asked, at their own mode", out[-300:])
    check(wait_for(lambda: env.state().get("shared_followers") == 1, 20) and loaded(env, ["A", "B"], scene),
          "the two share one rendering again")
    picture_ok(env, A, 2, "picture: A at native again")
    picture_ok(env, B, 2, "picture: B at native again")

    # a preset on a monitor it is larger than: lines on the shorter side, the monitor's shape
    r = env.ipc({"cmd": "set", "resolution": "A:720p"}) or {}
    check(r.get("ok") is True and env.listed("A").get("render_size") == [1280, 720] and
          env.listed("A").get("resolution") == "720p", "set through `set`: A at 720p = 1280x720 on 16:9")

    # texture limit: a number is a cap, shown once the wallpaper is loaded with it
    rc, out = env.ctl("resolution", "all", "texture_limit=64")
    check(rc == 0 and loaded(env, ["A", "B"], scene) and
          wait_for(lambda: env.listed("A").get("texture_limit_px") == 64 and
                   env.listed("B").get("texture_limit") == "64", 15),
          "texture_limit=64 on both", out[-200:])
    picture_ok(env, B, 2, "picture: B with the cap (flat colours: nothing to lose)")
    r = env.ipc({"cmd": "resolution", "monitor": "B", "texture_limit": "none"}) or {}
    # one wallpaper on two monitors keeps one set of textures: the least strict limit holds
    check(r.get("ok") is True and wait_for(lambda: env.listed("A").get("texture_limit_px") == 0, 15) and
          env.listed("A").get("texture_limit") == "64",
          "A (64) shows the wallpaper B (none) shows too: no cap is in effect for it either",
          env.listed("A"))
    env.ctl("resolution", "all", "texture_limit=auto")
    env.ctl("resolution", "A", "400x225")
    check(env.listed("A").get("render_size") == [400, 225] and env.listed("A").get("texture_limit") == "auto",
          "A at 400x225 again for the next part")
    # the question can be switched off and on
    r = env.ipc({"cmd": "resolution", "prompt": False}) or {}
    check(r.get("prompt") is False and env.saved().get("resolution_prompt") is False and
          env.state().get("resolution_prompt") is False, "prompt off: in the listing, in status and saved")
    rc, _ = env.ctl("resolution", "--prompt", "on")
    check(rc == 0 and "resolution_prompt" not in env.saved(), "prompt on again")


# -------------------------------------------------------------------- hotplug
def run_hotplug(env, scene):
    if not check(restart_for(env, [A, B, C], 3), "hotplug: a third monitor appears, the daemon restarts for it"):
        return
    st = env.state()
    sc = env.mon("C", st)
    check(st.get("resolution_unasked") == 1 and sc.get("resolution_asked") is False and
          sc.get("resolution") == "native" and sc.get("render_size") == [400, 450],
          "hotplug: the new monitor is native and not asked about yet", sc)
    check(env.mon("A", st).get("render_size") == [400, 225] and env.mon("A", st).get("resolution_asked") is True
          and env.mon("B", st).get("resolution_asked") is True,
          "hotplug: the others keep their choices across the restart")
    check("C" not in (env.saved().get("monitors") or {}), "hotplug: no entry is written for it")
    check(loaded(env, ["A", "B", "C"], scene), "hotplug: the wallpaper is on all three")
    picture_ok(env, C, 2, "picture: C at native")
    picture_ok(env, A, 4, "picture: A at its chosen size after the restart")

    # gone and back, a restart each time: still not asked
    check(restart_for(env, [A, B], 2) and env.state().get("resolution_unasked") == 0,
          "hotplug: the monitor is removed; nobody is left to ask about")
    check(restart_for(env, [A, B, C], 3) and env.state().get("resolution_unasked") == 1 and
          env.mon("C").get("resolution_asked") is False, "hotplug: it is back, and still not asked about")
    # a daemon that nobody answers just renders: quit and start, same thing
    env.stop_daemon()
    check(env.start_daemon([A, B, C]) and env.state().get("resolution_unasked") == 1 and
          env.mon("C").get("render_size") == [400, 450],
          "a daemon started without anybody answering renders at native and keeps the mark")

    # an answer ends it: a preset on an upright monitor is its shorter side
    rc, out = env.ctl("resolution", "C", "720p")
    lc = env.listed("C")
    check(rc == 0 and lc.get("resolution") == "720p" and lc.get("render_size") == [720, 810] and
          lc.get("asked") is True and env.listing().get("unasked") == 0 and "times the pixels" in out,
          "answer: C at 720p (720 on its shorter side: 720x810), asked, with the cost note", out[-300:])
    check((env.saved().get("monitors") or {}).get("C") ==
          {"resolution": "720p", "texture_limit": "auto", "asked": True}, "answer: saved")
    check(loaded(env, ["A", "B", "C"], scene), "... loaded")
    picture_ok(env, C, 3, "picture: C rendered larger than itself, same geometry")
    # unplugged: its choice stays, listed as not connected
    check(restart_for(env, [A, B], 2), "C is removed")
    gone = env.listed("C")
    check(gone.get("connected") is False and gone.get("resolution") == "720p" and gone.get("asked") is True and
          "mode" not in gone, "a stored choice of a monitor that is not connected is listed as such", gone)
    r = env.ipc({"cmd": "resolution", "monitor": "Z-9", "value": "1080p"}) or {}
    check(r.get("ok") is True and "not connected" in (r.get("note") or "") and
          env.listed("Z-9", r).get("connected") is False, "a choice can be stored ahead for an output by name")
    r = env.ipc({"cmd": "resolution", "monitor": "Z-9", "forget": True}) or {}
    check(r.get("ok") is True and not env.listed("Z-9", r), "... and dropped again")
    check(restart_for(env, [A, B, C], 3) and env.mon("C").get("render_size") == [720, 810] and
          env.state().get("resolution_unasked") == 0, "C is back with its choice, nobody is asked")
    # ask again / forget
    rc, _ = env.ctl("resolution", "C", "--ask")
    check(rc == 0 and env.state().get("resolution_unasked") == 1 and env.mon("C").get("render_size") == [720, 810],
          "--ask: to be asked about again, the size stays")
    rc, _ = env.ctl("resolution", "--keep")
    check(rc == 0 and env.state().get("resolution_unasked") == 0 and env.mon("C").get("render_size") == [720, 810],
          "--keep: everything as it is, nobody to ask")
    rc, _ = env.ctl("resolution", "C", "--forget")
    check(rc == 0 and env.state().get("resolution_unasked") == 1 and env.mon("C").get("render_size") == [400, 450]
          and "C" not in (env.saved().get("monitors") or {}), "--forget: native, not asked, no entry")


# -------------------------------------------------------------------- upgrade
def run_upgrade(env, scene):
    env.stop_daemon()
    old = {"config_version": 3, "fps": 30, "current": scene, "pause_covered": False, "pause_dpms": False,
           "tex_downscale": True, "render_scale": 100, "video_limit": False}
    jdump(env.config, old)
    mark = env.mark()
    if not check(env.start_daemon([A, B, C]), "upgrade: the daemon starts on a config.json of format 3"):
        return
    st = env.state()
    check(st.get("resolution_unasked") == 0 and all(env.mon(n, st).get("resolution_asked") is True and
                                                    env.mon(n, st).get("resolution") == "native"
                                                    for n in "ABC"),
          "upgrade: nobody is asked; every connected monitor is native")
    check(all(env.mon(m[0], st).get("render_size") == [m[1], m[2]] for m in (A, B, C)),
          "upgrade: every monitor renders at its own mode")
    sv = env.saved()
    want = {n: {"resolution": "native", "texture_limit": "auto", "asked": True} for n in "ABC"}
    check(sv.get("config_version") == 4 and sv.get("monitors") == want and "resolution_migrate" not in sv,
          "upgrade: the monitors are entered once, the mark is gone", sv.get("monitors"))
    check(sv.get("video_resolution") == "1080p" and st.get("video_resolution") == "1080p" and
          env.listing().get("video_box") == [1920, 1080],
          "upgrade: videos stay within 1920x1080 as before")
    check(all(sv.get(k) == v for k, v in old.items() if k != "config_version"),
          "upgrade: every setting it had is unchanged")
    check("count as asked" in env.since(mark), "upgrade: the log says what was done")
    check(loaded(env, ["A", "B", "C"], scene), "upgrade: the saved wallpaper is shown")
    picture_ok(env, A, 2, "picture: A as before the upgrade")
    # a second start changes nothing more
    env.stop_daemon()
    txt = open(env.config).read()
    check(env.start_daemon([A, B, C]) and open(env.config).read() == txt and
          env.state().get("resolution_unasked") == 0, "upgrade: the next start leaves config.json alone")
    # a monitor that was never there is asked about, like on any installation
    check(restart_for(env, [A, B, C, D], 4) and env.state().get("resolution_unasked") == 1 and
          env.mon("D").get("resolution_asked") is False,
          "upgrade: a monitor that appears afterwards is not asked about yet")
    # the video limit can be lifted, and follows the monitors on request
    r = env.ipc({"cmd": "set", "video_resolution": "auto"}) or {}
    check(r.get("ok") is True and env.listing().get("video_box") == [800, 450] and
          env.saved().get("video_resolution") == "auto", "video_resolution=auto: the largest render size")
    r = env.ipc({"cmd": "set", "video_resolution": "none"}) or {}
    check(r.get("ok") is True and env.listing().get("video_box") == [0, 0] and
          env.saved().get("video_resolution") == "none", "video_resolution=none: no limit")
    r = env.ipc({"cmd": "set", "video_resolution": "720p"}) or {}
    check(r.get("ok") is True and env.listing().get("video_box") == [1280, 720], "video_resolution=720p")


# ------------------------------------------------------------------------ env
def run_env(env, scene):
    env.stop_daemon()
    txt = open(env.config).read()
    if not check(env.start_daemon([A, B], extra={"WALPE_RESOLUTION": "A:400x225"}),
                 "env: the daemon starts with WALPE_RESOLUTION"):
        return
    st = env.state()
    check(st.get("resolution_env") is True and env.mon("A", st).get("render_size") == [400, 225] and
          env.mon("B", st).get("render_size") == [800, 450] and env.listing().get("env") is True,
          "env: A at the size of the variable, B at its own")
    check(open(env.config).read() == txt, "env: nothing is saved")
    check(loaded(env, ["A", "B"], scene), "env: loaded")
    picture_ok(env, A, 4, "picture: A at the variable's size")
    env.stop_daemon()
    mark = env.mark()
    check(env.start_daemon([A, B], extra={"WALPE_RESOLUTION": "A:nonsense"}) and
          "WALPE_RESOLUTION is not used" in env.since(mark) and
          env.mon("A").get("render_size") == [800, 450] and "resolution_env" not in env.state(),
          "env: a value that is not one is not used, and the log says so")


def main():
    args = sys.argv[1:]
    build, daemon, keep, tmp = "build", None, False, None
    while args:
        a = args.pop(0)
        if a == "--build" and args:
            build = args.pop(0)
        elif a == "--daemon" and args:
            daemon = args.pop(0)
        elif a == "--keep":
            keep = True
        elif a == "--tmp" and args:
            tmp = args.pop(0)
        else:
            sys.stderr.write(__doc__)
            return 2
    if not shutil.which("Xvfb"):
        print("rescheck: skipped (needs Xvfb)")
        return 77
    for b in (daemon or os.path.join(build, "walpe-daemon"), os.path.join(build, "walpe-ctl")):
        if not os.path.exists(b):
            print(f"rescheck: {b} not found")
            return 2
    env = Env(build, daemon, keep, tmp)
    try:
        if len(env.sock.encode()) > 107:
            print(f"rescheck: {env.sock} is too long for a socket ({len(env.sock.encode())} > 107 bytes): "
                  "pass --tmp <a short directory>")
            return 2
        scene = os.path.join(env.tmp, "wallpapers", "marks")
        gen_scene(scene)
        if not env.start_x():
            print("rescheck: skipped (Xvfb did not start)")
            return 77
        if not poke_root(env.env["DISPLAY"]):
            print("rescheck: skipped (no libX11 for ctypes: a layout change cannot be announced)")
            return 77
        if not env.start_daemon([A, B]):
            print("rescheck: the daemon did not start; log:")
            print(open(env.log, errors="replace").read()[-3000:])
            return 1
        run_fresh(env, scene)
        run_refuse(env)
        run_set(env, scene)
        run_hotplug(env, scene)
        run_upgrade(env, scene)
        run_env(env, scene)
    finally:
        env.close()
    print(f"rescheck: {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
