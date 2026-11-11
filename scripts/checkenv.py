#!/usr/bin/env python3
"""Shared harness of the end-to-end checks of the daemon (scripts/lifecheck.py, a registered
test, and the developer tools on the `devtools` branch).

A private display (scripts/displayenv.py: Xvfb with its framebuffer in a file and fake
monitors, or a headless Wayland compositor with real outputs), a private walpe-daemon (own XDG dirs in a temp dir), generated one-colour scenes,
IPC helpers and a reader of the pixels in the middle of each monitor. Nothing of the
user's is read or written. RESTORECHECK_PATIENCE=N multiplies every wait (a software
renderer on a busy machine: 3 or more)."""
import json, os, shutil, socket, subprocess, sys, tempfile, time

sys.dont_write_bytecode = True   # no __pycache__ next to the scripts
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import displayenv   # noqa: E402  (the display: Xvfb or a headless Wayland compositor)

PATIENCE = max(1.0, float(os.environ.get("RESTORECHECK_PATIENCE", "1") or 1))


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


def gen_scene(out, rgb):
    """one flat colour: a solid layer over the whole scene"""
    jdump(f"{out}/models/util/solidlayer.json", {"solidlayer": True})
    col = " ".join(str(c) for c in rgb)
    jdump(f"{out}/scene.json", {
        "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
        "general": {"ambientcolor": "0.3 0.3 0.3", "bloom": False, "cameraparallax": False,
                    "clearcolor": " ".join(str(c * 0.5) for c in rgb),
                    "orthogonalprojection": {"height": 1080, "width": 1920},
                    "skylightcolor": "0.3 0.3 0.3"},
        "objects": [{"id": 40, "name": "fill", "image": "models/util/solidlayer.json",
                     "origin": "960 540 0", "size": "1920 1080", "scale": "1 1 1",
                     "angles": "0 0 0", "color": col, "alpha": 1.0, "visible": True}]})
    jdump(f"{out}/project.json", {"file": "scene.json", "title": os.path.basename(out), "type": "scene"})


def gen_broken(out):
    jdump(f"{out}/project.json", {"file": "scene.json", "title": "broken", "type": "scene"})
    with open(f"{out}/scene.json", "w") as f:
        f.write("{ \"camera\": this is not JSON\n")


class Env:
    def __init__(self, build, daemon, keep, tmp=None, display=None):
        self.build = os.path.abspath(build)
        self.daemon_bin = os.path.abspath(daemon) if daemon else os.path.join(self.build, "walpe-daemon")
        self.keep = keep
        if tmp:
            os.makedirs(tmp)            # must be new: it is removed afterwards
            self.tmp = os.path.abspath(tmp)
        else:
            self.tmp = tempfile.mkdtemp(prefix="walpe-check-")
        self.display = displayenv.make(displayenv.pick(display) or "xvfb", self.tmp, PATIENCE)
        self.daemon = None
        self.env = dict(os.environ)
        for k, d in (("XDG_RUNTIME_DIR", "rt"), ("XDG_CONFIG_HOME", "cfg"), ("XDG_CACHE_HOME", "cache"),
                     ("XDG_DATA_HOME", "data"), ("HOME", "home")):
            p = os.path.join(self.tmp, d)
            os.makedirs(p, mode=0o700)
            self.env[k] = p
        self.env.update({"LIBGL_ALWAYS_SOFTWARE": "1", "NO_AT_BRIDGE": "1"})
        for k in ("WALPE_PROP_PROBE", "WALPE_SKIP_IDS", "WALPE_NO_SCRIPTS", "WALPE_NO_SHARED_RENDER",
                  "WAYLAND_DISPLAY"):
            self.env.pop(k, None)
        self.sock = os.path.join(self.env["XDG_RUNTIME_DIR"], "walpe.sock")
        self.config = os.path.join(self.env["XDG_CONFIG_HOME"], "walpe", "config.json")
        self.log = os.path.join(self.tmp, "daemon.log")
        self.mons = []

    def start_x(self, width, height):
        """the display server (Xvfb, or the headless compositor) and the clients' variables for it"""
        if not self.display.start(width, height):
            return False
        self.display.client_env(self.env)
        return True

    def stop_x(self):
        self.display.stop()

    def start_daemon(self, mons):
        """mons: [(name, w, h, x, y)]"""
        # real outputs are made what is asked for and bring their own names; monitors
        # that are rectangles of one X screen are told to the daemon
        self.mons = mons = self.display.place(mons)
        if self.display.fake_monitors:
            self.env["WALPE_MONITORS"] = ",".join(f"{n}={w}x{h}+{x}+{y}@60" for n, w, h, x, y in mons)
        log = open(self.log, "a")
        self.daemon = subprocess.Popen([self.daemon_bin], env=self.env, stdout=log, stderr=subprocess.STDOUT,
                                       cwd=self.tmp)
        log.close()
        for _ in range(int(150 * PATIENCE)):
            time.sleep(0.1)
            if self.daemon.poll() is not None:
                return False
            if os.path.exists(self.sock) and self.ipc({"cmd": "status"}):
                self.mons = self.display.seen(mons, self.monitors)   # the daemon's order and names
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

    def ipc_burst(self, reqs):
        """several requests, all sent before the first reply is read -> replies"""
        socks, out = [], []
        try:
            for req in reqs:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(6.0 * PATIENCE)
                s.connect(self.sock)
                s.sendall(json.dumps(req).encode() + b"\n")
                socks.append(s)
            for s in socks:
                buf = b""
                while not buf.endswith(b"\n"):
                    d = s.recv(65536)
                    if not d:
                        break
                    buf += d
                out.append(json.loads(buf.decode()))
        except Exception:
            pass
        for s in socks:
            s.close()
        return out

    def ctl(self, *args):
        """walpe-ctl ... -> (exit status, reply dict or None); retried while the daemon is too busy to answer"""
        for _ in range(int(5 * PATIENCE)):
            p = subprocess.run([os.path.join(self.build, "walpe-ctl")] + [str(a) for a in args], env=self.env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60 * PATIENCE)
            if p.returncode != 2:
                break
            time.sleep(0.5)
        out = p.stdout.decode(errors="replace")
        try:
            return p.returncode, json.loads(out[out.index("{"):])
        except ValueError:
            return p.returncode, None

    def open(self, path, mon=None):
        rc, rep = self.ctl(*(["open", path] + (["--monitor", mon] if mon is not None else [])))
        return rc == 0 and bool(rep and rep.get("ok"))

    def monitors(self):
        """status "monitors" through walpe-ctl, [] when it gave none"""
        _, st = self.ctl("status")
        m = ((st or {}).get("state") or {}).get("monitors")
        return m if isinstance(m, list) else []

    def saved(self):
        try:
            c = json.load(open(self.config))
            return c.get("current", ""), c.get("monitor_current", {}) or {}
        except Exception:
            return None, None

    def count(self, text):
        try:
            return open(self.log, errors="replace").read().count(text)
        except OSError:
            return 0

    def colour(self, mon):
        """the pixel in the middle of a monitor, as the display shows it"""
        _, w, h, x, y = mon
        return self.display.pixel(mon, w // 2, h // 2)


def name_of(rgb):
    r, g, b = rgb
    if max(rgb) < 24:
        return "black"
    if r > 150 and r > b + 80:
        return "red"
    if b > 150 and b > r + 80:
        return "blue"
    return "rgb(%d,%d,%d)" % rgb


def brief(ms):
    return [{k: m.get(k) for k in ("index", "current", "type", "load_error", "load_restored", "follows")
             if m.get(k) is not None} for m in ms]
