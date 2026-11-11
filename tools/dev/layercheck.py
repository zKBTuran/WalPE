#!/usr/bin/env python3
"""End-to-end check of "Adjust layers" through the daemon (dev harness, like optcheck.py).

  python3 -I tools/dev/layercheck.py --build build [--keep] [--display :N] [--tmp DIR]

Starts a private Xvfb with two fake monitors of different sizes (1280x720 and
960x540, so each has its own scene instance and its own pixels per scene unit),
a private walpe-daemon (own XDG dirs in a temp dir) and the synthetic "blocks"
scene of genlayers.py, then drives `walpe-ctl layers ...` and the raw
IPC and reads the result back from screenshots of the root window:

  - the layer list (keys, types, parents, positions, a script-driven origin)
  - move / group move / scale / hide / show / reset, live, on both monitors
  - selectors (key, id, name), refused input (nothing changes)
  - the outline of selected layers and its time limit
  - config.json: saved, survives a daemon restart, "resetprops" leaves it,
    "forget" (the delete flow) removes it
  - a second wallpaper on one monitor has its own adjustments
  - a wallpaper that is not shown, a wallpaper that is not a scene
  - changes while paused are drawn

Exit code 0 = all passed, 1 = failures, 77 = skipped (no Xvfb / ImageMagick
`import` / Pillow). LAYERCHECK_PATIENCE=N multiplies every wait (a software
renderer on a busy machine: 3 or more). --tmp DIR: work in DIR (must not
exist; removed afterwards) instead of a fresh directory under $TMPDIR - the
daemon's socket lives below it and a socket path is limited to 107 bytes."""
import json, os, shutil, socket, subprocess, sys, tempfile, time

try:
    from PIL import Image, ImageChops
except Exception:                       # pragma: no cover
    Image = None

HERE = os.path.dirname(os.path.abspath(__file__))
MONS = [("A", 1280, 720, 0, 0), ("B", 960, 540, 1280, 0)]
SCREEN = (2240, 720)
RED, GREEN, BLUE, YELLOW = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)
OUTLINE = (255, 204, 26)
PATIENCE = max(1.0, float(os.environ.get("LAYERCHECK_PATIENCE", "1") or 1))

fails = 0
checks = 0


def check(ok, what):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    return ok


class Env:
    def __init__(self, build, display, keep, tmp=None):
        self.build = os.path.abspath(build)
        self.keep = keep
        if tmp:
            os.makedirs(tmp)            # must be new: it is removed afterwards
            self.tmp = os.path.abspath(tmp)
        else:
            self.tmp = tempfile.mkdtemp(prefix="walpe-layercheck-")
        self.display = display
        self.xvfb = None
        self.daemon = None
        self.env = dict(os.environ)
        for k, d in (("XDG_RUNTIME_DIR", "rt"), ("XDG_CONFIG_HOME", "cfg"), ("XDG_CACHE_HOME", "cache"),
                     ("XDG_DATA_HOME", "data"), ("HOME", "home")):
            p = os.path.join(self.tmp, d)
            os.makedirs(p, mode=0o700)
            self.env[k] = p
        # (this environment is the caller's: inside a Wayland session the daemon, left to
        # choose, would put the test wallpapers on the real desktop. It is told to use X11,
        # and the session's compositor is taken out of its sight)
        self.env.update({"LIBGL_ALWAYS_SOFTWARE": "1", "NO_AT_BRIDGE": "1", "WALPE_BACKEND": "x11",
                         "WALPE_MONITORS": ",".join(f"{n}={w}x{h}+{x}+{y}@60" for n, w, h, x, y in MONS)})
        for k in ("WALPE_PROP_PROBE", "WALPE_SKIP_IDS", "WALPE_NO_SCRIPTS", "WAYLAND_DISPLAY", "WAYLAND_SOCKET"):
            self.env.pop(k, None)
        self.sock = os.path.join(self.env["XDG_RUNTIME_DIR"], "walpe.sock")
        self.config = os.path.join(self.env["XDG_CONFIG_HOME"], "walpe", "config.json")

    def start_x(self):
        nums = [self.display] if self.display else [f":{n}" for n in range(141, 160)]
        for d in nums:
            if os.path.exists(f"/tmp/.X11-unix/X{d[1:]}"):
                continue
            # -s 0: no screen saver (the daemon pauses while the screen is blanked)
            p = subprocess.Popen(["Xvfb", d, "-screen", "0", f"{SCREEN[0]}x{SCREEN[1]}x24", "-nolisten", "tcp",
                                  "-s", "0"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(50):
                time.sleep(0.1)
                if p.poll() is not None or os.path.exists(f"/tmp/.X11-unix/X{d[1:]}"):
                    break
            if p.poll() is None:
                self.xvfb = p
                self.env["DISPLAY"] = d
                return True
        return False

    def start_daemon(self):
        log = open(os.path.join(self.tmp, "daemon.log"), "a")
        self.daemon = subprocess.Popen([os.path.join(self.build, "walpe-daemon")], env=self.env,
                                       stdout=log, stderr=subprocess.STDOUT, cwd=self.tmp)
        for _ in range(int(100 * PATIENCE)):
            time.sleep(0.1)
            if os.path.exists(self.sock) and self.ipc({"cmd": "status"}):
                return True
        return False

    def stop_daemon(self):
        if self.daemon:
            self.ipc({"cmd": "quit"})
            try:
                self.daemon.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
            self.daemon = None
        if os.path.exists(self.sock):
            os.unlink(self.sock)

    def close(self):
        self.stop_daemon()
        if self.xvfb:
            self.xvfb.terminate()
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
            s.sendall((req if isinstance(req, str) else json.dumps(req)).encode() + b"\n")
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
        p = subprocess.run([os.path.join(self.build, "walpe-ctl")] + [str(a) for a in args], env=self.env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20 * PATIENCE)
        return p.returncode, p.stdout.decode(errors="replace")

    def shot(self):
        f = os.path.join(self.tmp, "shot.png")
        subprocess.run(["import", "-window", "root", "-silent", f], env=self.env, check=True, timeout=30)
        return Image.open(f).convert("RGB")

    def saved(self):
        try:
            return json.load(open(self.config)).get("layers", {})
        except Exception:
            return {}


def box(img, mon, rgb, tol=10):
    """bounding box (x0, y0, x1, y1), monitor pixels, of the colour on one monitor, or None"""
    _, w, h, x, y = mon
    part = img.crop((x, y, x + w, y + h))
    mask = None
    for band, c in zip(part.split(), rgb):
        m = band.point(lambda v, c=c: 255 if abs(v - c) <= tol else 0)
        mask = m if mask is None else ImageChops.darker(mask, m)
    return mask.getbbox()


def scene_box(mon, cx, cy, w, h):
    """where a scene rectangle (centre cx,cy, y up) lands on a monitor"""
    _, mw, mh, _, _ = mon
    k = max(mw / 1920.0, mh / 1080.0)
    x0 = (cx - w / 2 - 960) * k + mw / 2
    x1 = (cx + w / 2 - 960) * k + mw / 2
    y0 = mh / 2 - (cy + h / 2 - 540) * k
    y1 = mh / 2 - (cy - h / 2 - 540) * k
    return (x0, y0, x1, y1)


def near(b, want, tol=1.6):
    return b is not None and all(abs(a - c) <= tol for a, c in zip(b, want))


def wait_for(fn, timeout=6.0, step=0.15):
    t0 = time.time()
    timeout *= PATIENCE
    while True:
        v = fn()
        if v or time.time() - t0 > timeout:
            return v
        time.sleep(step)


def seen(env, rgb, cx, cy, w, h, what, mons=MONS):
    """the colour sits where the scene rectangle maps to, on every monitor"""
    def ok():
        img = env.shot()
        return all(near(box(img, m, rgb), scene_box(m, cx, cy, w, h)) for m in mons)
    good = wait_for(ok)
    if not good:
        img = env.shot()
        for m in mons:
            print("     ", m[0], "found", box(img, m, rgb), "expected",
                  tuple(round(v, 1) for v in scene_box(m, cx, cy, w, h)))
    return check(good, what)


def gone(env, rgb, what, mons=MONS):
    return check(wait_for(lambda: all(box(env.shot(), m, rgb) is None for m in mons)), what)


def adj_of(reply, key):
    for a in (reply or {}).get("adjustments", []):
        if a.get("key") == key:
            return a
    return None


def main():
    args = sys.argv[1:]
    build, keep, display, tmp = "build", False, None, None
    while args:
        a = args.pop(0)
        if a == "--build" and args:
            build = args.pop(0)
        elif a == "--keep":
            keep = True
        elif a == "--display" and args:
            display = args.pop(0)
        elif a == "--tmp" and args:
            tmp = args.pop(0)
        else:
            sys.stderr.write(__doc__)
            return 2
    if Image is None or not shutil.which("Xvfb") or not shutil.which("import"):
        print("layercheck: skipped (needs Xvfb, ImageMagick `import` and Pillow)")
        return 77
    for b in ("walpe-daemon", "walpe-ctl"):
        if not os.path.exists(os.path.join(build, b)):
            print(f"layercheck: {b} not found in {build}")
            return 2
    env = Env(build, display, keep, tmp)
    try:
        if len(env.sock.encode()) > 107:
            print(f"layercheck: {env.sock} is too long for a socket ({len(env.sock.encode())} > 107 bytes): "
                  "pass --tmp <a short directory>")
            return 2
        if not env.start_x():
            print("layercheck: skipped (Xvfb did not start)")
            return 77
        scene = os.path.join(env.tmp, "wallpapers", "blocks")
        other = os.path.join(env.tmp, "wallpapers", "blocks-two")
        for d in (scene, other):
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "genlayers.py"), "blocks", d],
                           check=True, stdout=subprocess.DEVNULL)
        image = os.path.join(env.tmp, "wallpapers", "plain.png")
        Image.new("RGB", (64, 64), (40, 90, 160)).save(image)
        os.makedirs(os.path.dirname(env.config), exist_ok=True)
        json.dump({"config_version": 2, "fps": 30, "pause_covered": False, "pause_dpms": False},
                  open(env.config, "w"))
        if not env.start_daemon():
            print("layercheck: the daemon did not start; log:")
            print(open(os.path.join(env.tmp, "daemon.log")).read()[-3000:])
            return 1
        run(env, scene, other, image)
    finally:
        env.close()
    print(f"layercheck: {checks} checks, {fails} failed")
    return 1 if fails else 0


def run(env, scene, other, image):
    st = env.ipc({"cmd": "status"})
    check(st and st["state"].get("layer_adjust") is True, "status announces layer_adjust")
    check(env.ipc({"cmd": "layers"}) == {"ok": False, "error": "no active wallpaper (or no such monitor)"},
          "layers without a wallpaper is refused")
    check(env.ipc({"cmd": "open", "path": scene})["ok"], "open the blocks scene on both monitors")

    # ---- the list -------------------------------------------------------
    def listed():
        r = env.ipc({"cmd": "layers"})
        return r if r and r.get("ok") and r.get("layers") and r.get("fresh") else None
    r = wait_for(listed, 15)
    check(r is not None, "layers: a list from the running scene")
    if not r:
        return
    L = {l["key"]: l for l in r["layers"]}
    check(r["scene"] and r["running"] and r["size"] == [1920, 1080] and r["path"] == scene,
          "layers: scene, running, size 1920x1080")
    check(list(L) == ["40:red", "41:green", "42:blue", "43:label", "44:yellow"], "layers: keys in scene order")
    check([L[k]["type"] for k in L] == ["solid", "solid", "solid", "text", "solid"], "layers: types")
    check(L["41:green"]["parent"] == 0 and L["40:red"]["parent"] == -1, "layers: green's parent is red")
    check(L["40:red"]["pos"] == [400, 300] and L["40:red"]["size"] == [200, 100] and
          L["40:red"]["bounds"] == [300, 250, 500, 350], "layers: red position, size, bounds")
    check(L["41:green"]["pos"] == [600, 450], "layers: the child's position is in the scene (parent applied)")
    check(L["42:blue"]["pos"] == [1000, 700], "layers: blue is where its script puts it, not where the file says")
    check(L["43:label"].get("text") == "MOVE", "layers: a text layer tells its text")
    check(L["44:yellow"]["visible"] is False and L["44:yellow"]["shown"] is False and L["40:red"]["shown"] is True,
          "layers: visible / shown")
    check(r["adjustments"] == [] and not any("adjust" in l for l in r["layers"]), "layers: nothing adjusted yet")
    rc, out = env.ctl("layers")
    check(rc == 0 and "40:red" in out and "solid" in out and 'text "MOVE"' in out, "walpe-ctl layers prints the table")
    rc, out = env.ctl("layers", "--monitor", "1", "--json")
    check(rc == 0 and json.loads(out)["path"] == scene, "walpe-ctl layers --monitor 1 --json")
    seen(env, RED, 400, 300, 200, 100, "picture: red where the scene has it, on both monitors")

    # ---- move -----------------------------------------------------------
    rc, out = env.ctl("layers", "move", 300, 200, "40:red")
    check(rc == 0 and "1 layer(s) changed" in out and "dx=+300 dy=+200" in out, "ctl move 300 200 40:red")
    seen(env, RED, 700, 500, 200, 100, "picture: red moved by exactly +300,+200 scene px on both monitor sizes")
    seen(env, GREEN, 900, 650, 100, 100, "picture: its child followed")
    check(env.saved().get(scene) == [{"key": "40:red", "dx": 300.0, "dy": 200.0, "scale": 1.0,
                                       "visible": "default"}], "config.json holds the adjustment under the wallpaper")
    # group: parent and child named together -> the child is not moved twice
    r = env.ipc({"cmd": "layeradj", "op": "move", "keys": ["40:red", "41:green"], "dx": -100, "dy": 0})
    check(r["ok"] and r["changed"] == 1 and adj_of(r, "40:red")["dx"] == 200 and adj_of(r, "41:green") is None,
          "group move of a parent and its child changes the parent only")
    seen(env, GREEN, 800, 650, 100, 100, "picture: the child moved once")
    # "children": true selects what hangs below
    r = env.ipc({"cmd": "layeradj", "op": "highlight", "keys": ["40:red"], "children": True, "seconds": 1})
    check(r["ok"] and r["changed"] == 2, "children: true adds the layers below (highlight of 2)")
    # the script-driven layer, by bare id; a text layer, by name
    rc, out = env.ctl("layers", "move", -200, -100, 42, "label")
    check(rc == 0 and "2 layer(s) changed" in out, "ctl move by bare id and by name")
    seen(env, BLUE, 800, 600, 80, 80, "picture: the script-driven layer is offset from where its script puts it")
    r = env.ipc({"cmd": "layers"})
    check(next(l for l in r["layers"] if l["key"] == "42:blue").get("adjust", {}).get("dx") == -200,
          "layers: the list shows the adjustment on its layer")

    # ---- refused input: nothing changes ----------------------------------
    before = env.saved()
    bad = [
        ({"cmd": "layeradj", "op": "move", "keys": ["40:red"], "dx": "12", "dy": 0}, "a string for dx"),
        ({"cmd": "layeradj", "op": "move", "keys": ["40:red"], "dx": 1e9, "dy": 0}, "dx out of range"),
        ({"cmd": "layeradj", "op": "move", "keys": ["40:red"], "dx": 99900, "dy": 0}, "dx that adds up out of range"),
        ({"cmd": "layeradj", "op": "move", "keys": ["40:red"]}, "move without dx / dy"),
        ({"cmd": "layeradj", "op": "move", "keys": [], "dx": 1, "dy": 1}, "move without keys"),
        ({"cmd": "layeradj", "op": "move", "keys": "40:red", "dx": 1, "dy": 1}, "keys that are not a list"),
        ({"cmd": "layeradj", "op": "move", "keys": [40], "dx": 1, "dy": 1}, "a key that is not a string"),
        ({"cmd": "layeradj", "op": "move", "keys": ["nothing"], "dx": 1, "dy": 1}, "an unknown layer"),
        ({"cmd": "layeradj", "op": "move", "keys": ["40:red", "nothing"], "dx": 1, "dy": 1},
         "one unknown layer among good ones"),
        ({"cmd": "layeradj", "op": "scale", "keys": ["40:red"], "factor": 0}, "factor 0"),
        ({"cmd": "layeradj", "op": "scale", "keys": ["40:red"], "factor": -2}, "a negative factor"),
        ({"cmd": "layeradj", "op": "scale", "keys": ["40:red"], "factor": 1000}, "factor 1000"),
        ({"cmd": "layeradj", "op": "scale", "keys": ["40:red"]}, "scale without factor"),
        ({"cmd": "layeradj", "op": "set", "layers": [{"key": "40:red", "scale": 0.001}]}, "scale 0.001"),
        ({"cmd": "layeradj", "op": "set", "layers": [{"key": "40:red", "dy": True}]}, "a boolean for dy"),
        ({"cmd": "layeradj", "op": "set", "layers": [{"key": "40:red", "visible": "sometimes"}]}, "an unknown visibility"),
        ({"cmd": "layeradj", "op": "set", "layers": [{"dx": 5}]}, "set without a key"),
        ({"cmd": "layeradj", "op": "set", "layers": []}, "set without layers"),
        ({"cmd": "layeradj", "op": "set", "layers": [{"key": "41:green", "dx": 5}, {"key": "40:red", "dx": 2e5}]},
         "one bad entry among good ones"),
        ({"cmd": "layeradj", "op": "explode"}, "an unknown op"),
        ({"cmd": "layeradj", "op": "highlight", "keys": ["40:red"], "seconds": 1e6}, "a highlight for 11 days"),
        ({"cmd": "layeradj", "op": "move", "monitor": 7, "keys": ["40:red"], "dx": 1, "dy": 1}, "a monitor that does not exist"),
        ('{"cmd":"layeradj","op":"move","keys":["40:red"],"dx":NaN,"dy":0}', "NaN (not JSON)"),
        ('{"cmd":"layeradj","op":"move","keys":["40:red"],"dx":1e999,"dy":0}', "1e999 (infinity)"),
    ]
    for req, what in bad:
        r = env.ipc(req)
        check(r is not None and r.get("ok") is False and r.get("error"), "refused: " + what)
    check(env.saved() == before, "refused requests changed nothing in config.json")
    rc, out = env.ctl("layers", "move", "nan", 0, "40:red")
    check(rc == 1, "walpe-ctl refuses a non-number")
    rc, out = env.ctl("layers", "move", 5, 5, "nothing")
    check(rc == 3 and "no layer" in out, "walpe-ctl reports the daemon's refusal (exit 3)")
    check(env.ipc({"cmd": "status"})["ok"], "the daemon is still answering")

    # ---- set / scale / hide / show --------------------------------------
    rc, out = env.ctl("layers", "set", "40:red", "dx=0", "dy=0", "scale=2")
    check(rc == 0, "ctl set 40:red dx=0 dy=0 scale=2")
    seen(env, RED, 400, 300, 400, 200, "picture: red twice as large about its own origin")
    seen(env, GREEN, 800, 600, 200, 200, "picture: the child scaled with it")
    rc, out = env.ctl("layers", "reset", "40:red")
    check(rc == 0 and "1 layer(s) changed" in out, "ctl reset 40:red")
    seen(env, RED, 400, 300, 200, 100, "picture: red back where the scene has it")
    # a group of two unrelated layers scaled about the centre of their bounds
    env.ctl("layers", "reset", "42:blue")
    r = env.ipc({"cmd": "layeradj", "op": "scale", "keys": ["40:red", "42:blue"], "factor": 0.5})
    # bounds: red 300..500 x 250..350, blue 960..1040 x 660..740 -> centre 670, 495
    a, b2 = adj_of(r, "40:red"), adj_of(r, "42:blue")
    check(r["ok"] and a and b2 and a["scale"] == 0.5 and b2["scale"] == 0.5 and
          abs(a["dx"] - 135) < 0.01 and abs(a["dy"] - 97.5) < 0.01 and
          abs(b2["dx"] + 165) < 0.01 and abs(b2["dy"] + 102.5) < 0.01,
          "group scale 0.5: each layer halves and moves half way to the group's centre")
    seen(env, RED, 535, 397.5, 100, 50, "picture: group scale, red")
    seen(env, BLUE, 835, 597.5, 40, 40, "picture: group scale, blue")
    env.ctl("layers", "reset", "40:red", "42:blue")
    rc, out = env.ctl("layers", "hide", "40:red")
    check(rc == 0, "ctl hide 40:red")
    gone(env, RED, "picture: red hidden")
    gone(env, GREEN, "picture: its child hidden with it")
    rc, out = env.ctl("layers", "show", "44:yellow")
    check(rc == 0, "ctl show 44:yellow (a layer the scene hides)")
    seen(env, YELLOW, 1600, 800, 120, 60, "picture: yellow shown")
    rc, out = env.ctl("layers", "set", "40:red", "visible=default")
    seen(env, RED, 400, 300, 200, 100, "picture: red visible again (visible=default)")
    env.ctl("layers", "reset", "44:yellow")
    gone(env, YELLOW, "picture: yellow hidden again after its reset")

    # ---- outline ---------------------------------------------------------
    rc, out = env.ctl("layers", "highlight", "40:red", "--seconds", str(1.5 * PATIENCE))
    check(rc == 0, "ctl highlight 40:red --seconds 1.5")
    def outlined():
        img = env.shot()
        return all(near(box(img, m, OUTLINE, 24), scene_box(m, 400, 300, 200, 100), 3.5) for m in MONS)
    check(wait_for(outlined, 1.2, 0.1), "picture: a thin outline on the red layer's bounds")
    check(wait_for(lambda: all(box(env.shot(), m, OUTLINE, 24) is None for m in MONS), 5),
          "picture: the outline goes away by itself")
    check(env.saved().get(scene, []) == [e for e in env.saved().get(scene, []) if e["key"] != "x"] and
          "highlight" not in json.dumps(env.saved()), "an outline is not saved")

    # ---- paused: a change is still drawn ---------------------------------
    check(env.ipc({"cmd": "pause"})["ok"], "pause")
    time.sleep(0.5)
    env.ctl("layers", "move", 300, 0, "40:red")
    seen(env, RED, 700, 300, 200, 100, "picture: a move while paused shows")
    env.ipc({"cmd": "resume"})

    # ---- config: restart, resetprops, a saved key without a layer --------
    env.ctl("layers", "move", 0, 100, "43:label")
    cfg = json.load(open(env.config))
    cfg["layers"][scene].append({"key": "999:gone", "dx": 5, "dy": 5, "scale": 1, "visible": "hidden"})
    env.stop_daemon()
    json.dump(cfg, open(env.config, "w"))
    check(env.start_daemon(), "daemon restarted")
    seen(env, RED, 700, 300, 200, 100, "picture: the adjustment is applied after a restart")
    r = wait_for(listed, 15)
    m = adj_of(r, "999:gone")
    check(r and m and m["missing"] is True and adj_of(r, "40:red")["missing"] is False,
          "a saved key that matches no layer is reported as missing")
    check(any(e["key"] == "999:gone" for e in env.saved()[scene]), "... and kept in config.json")
    log = open(os.path.join(env.tmp, "daemon.log")).read()
    check(log.count('"999:gone"') >= 1 and log.count("match no layer") <= 2,
          "... and logged once per scene instance (two monitors: at most twice)")
    r = env.ipc({"cmd": "resetprops"})
    check(r["ok"] and len(env.saved().get(scene, [])) == 3, "resetprops (options) leaves the layer adjustments")
    r = env.ipc({"cmd": "layeradj", "op": "reset", "keys": ["999:gone"]})
    check(r["ok"] and r["changed"] == 1 and adj_of(r, "999:gone") is None, "reset removes a missing key by its key")

    # ---- another wallpaper on monitor 1 only ----------------------------
    check(env.ipc({"cmd": "open", "path": other, "monitor": 1})["ok"], "open a second scene on monitor 1")
    r = wait_for(lambda: (lambda x: x if x and x.get("layers") and x.get("path") == other else None)(
        env.ipc({"cmd": "layers", "monitor": 1})), 15)
    check(r is not None and r["adjustments"] == [], "monitor 1: its own wallpaper, no adjustments")
    r = env.ipc({"cmd": "layeradj", "op": "move", "monitor": 1, "keys": ["40:red"], "dx": 0, "dy": 300})
    check(r["ok"] and r["path"] == other, "move on monitor 1's wallpaper")
    seen(env, RED, 400, 600, 200, 100, "picture: monitor 1 has its own offset", [MONS[1]])
    seen(env, RED, 700, 300, 200, 100, "picture: monitor 0 keeps the first wallpaper's", [MONS[0]])
    check(set(env.saved()) == {scene, other}, "config.json: one list per wallpaper")

    # ---- not shown / not a scene ----------------------------------------
    check(env.ipc({"cmd": "open", "path": scene})["ok"], "first scene on both monitors again")
    r = env.ipc({"cmd": "layers", "path": other})
    check(r["ok"] and r["scene"] and not r["running"] and r["layers"] == [] and len(r["adjustments"]) == 1,
          "a scene that is not shown: no list, its saved adjustments")
    r = env.ipc({"cmd": "layeradj", "op": "move", "path": other, "keys": ["red"], "dx": 1, "dy": 1})
    check(r["ok"] is False, "not shown: a bare name is refused")
    r = env.ipc({"cmd": "layeradj", "op": "set", "path": other, "layers": [{"key": "42:blue", "dx": 7}]})
    check(r["ok"] and not r["running"] and adj_of(r, "42:blue")["dx"] == 7, "not shown: a key is saved")
    r = env.ipc({"cmd": "layers", "path": image})
    check(r["ok"] and r["scene"] is False and r["layers"] == [], "an image wallpaper is not a scene")
    rc, out = env.ctl("layers", "--path", image)
    check(rc == 0 and "not a scene" in out, "walpe-ctl says so")

    # ---- reset all, forget ----------------------------------------------
    rc, out = env.ctl("layers", "reset")
    check(rc == 0 and "no layer of this wallpaper is adjusted" in out, "ctl reset (all)")
    seen(env, RED, 400, 300, 200, 100, "picture: everything back")
    check(scene not in env.saved() and other in env.saved(), "config.json: the first wallpaper's list is gone")
    r = env.ipc({"cmd": "forget", "path": other})
    check(r["ok"] and r["forgotten"] >= 2 and env.saved() == {} and
          "layers" not in json.load(open(env.config)),
          "forget (delete flow) removes the other wallpaper's adjustments; no empty \"layers\" left")


if __name__ == "__main__":
    sys.exit(main())
