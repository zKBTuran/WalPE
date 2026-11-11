#!/usr/bin/env python3
"""End-to-end check of option delivery to web wallpapers (dev harness, like layercheck.py).

  python3 -I tools/dev/webcheck.py --build build [--keep] [--tmp DIR]

Starts a private Xvfb (-displayfd) and a private walpe-daemon (own XDG dirs in a temp dir, one 320x180
fake monitor), writes two web wallpapers whose page paints itself in the colour its option "tint" names
(red = it was never told), and reads the desktop back from screenshots of the root window:

  - a page that sets wallpaperPropertyListener while it loads is told the default (blue), then a
    changed value (green) and another (blue) without being reloaded
  - a page that sets the listener 800 ms after its load event is told its saved value too (green; it
    stayed red in earlier builds: the first call was made at the load event and never again), and a
    later change (blue)
  - switching back and forth between the two while they load leaves the engine running, and the one
    opened last is shown and listens

Exit code 0 = all passed, 1 = failures, 77 = skipped (no Xvfb / ImageMagick `import` / Pillow, or the
build has no web module). WEBCHECK_PATIENCE=N multiplies every wait. --tmp DIR: work in DIR (must not
exist; removed afterwards); the engine's socket lives below it and a socket path is limited to 107
bytes (a longer one is refused). When run as root, the check switches WebKit's sandbox off for its
own daemon (WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1); run as any other user it leaves it on."""
import io, json, os, shutil, subprocess, sys, tempfile, time

try:
    from PIL import Image
except Exception:                       # pragma: no cover
    Image = None

PATIENCE = max(1.0, float(os.environ.get("WEBCHECK_PATIENCE", "1") or 1))
COLORS = {"red": (255, 0, 0), "green": (0, 255, 0), "blue": (0, 0, 255)}
fails = 0
checks = 0

PAGE = """<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;height:100%%;background:#ff0000}</style>
<script>
var names = {blue: '#0000ff', green: '#00ff00'};
function install() {
  window.wallpaperPropertyListener = {
    applyUserProperties: function (p) {
      if (p.tint && names[p.tint.value]) {
        document.documentElement.style.background = names[p.tint.value];
        document.body.style.background = names[p.tint.value];
      }
    }
  };
}
%s
</script></head><body></body></html>
"""


def check(ok, what):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    return ok


class Env:
    def __init__(self, build, keep, tmp=None):
        self.build = os.path.abspath(build)
        self.keep = keep
        if tmp:
            os.makedirs(tmp)
            self.tmp = os.path.abspath(tmp)
        else:
            self.tmp = tempfile.mkdtemp(prefix="walpe-webcheck-")
        self.env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8", "TZ": "UTC"}
        for k, d in (("XDG_RUNTIME_DIR", "rt"), ("XDG_CONFIG_HOME", "cfg"), ("XDG_CACHE_HOME", "cache"),
                     ("XDG_DATA_HOME", "data"), ("HOME", "home")):
            p = os.path.join(self.tmp, d)
            os.makedirs(p, mode=0o700)
            self.env[k] = p
        self.env.update({"LIBGL_ALWAYS_SOFTWARE": "1", "NO_AT_BRIDGE": "1", "WALPE_MODULE_DIR": self.build,
                         "WALPE_MONITORS": "A=320x180+0+0@60", "WALPE_NO_AUDIO": "1", "WALPE_NO_MEDIA": "1",
                         # (told by name: the X server started here, never a session's compositor)
                         "WALPE_BACKEND": "x11"})
        if os.geteuid() == 0:
            self.env["WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS"] = "1"
        self.config = os.path.join(self.env["XDG_CONFIG_HOME"], "walpe", "config.json")
        self.xvfb = None
        self.daemon = None

    def start_x(self):
        r, w = os.pipe()
        p = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", "320x180x24", "-nolisten", "tcp",
                              "-s", "0"], pass_fds=[w], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(w)
        disp = b""
        t0 = time.time()
        while not disp.endswith(b"\n") and time.time() - t0 < 30:
            c = os.read(r, 16)
            if not c:
                break
            disp += c
        os.close(r)
        if not disp.strip():
            p.kill()
            return False
        self.xvfb = p
        self.env["DISPLAY"] = ":" + disp.decode().strip()
        return True

    def ctl(self, *args):
        try:
            r = subprocess.run([os.path.join(self.build, "walpe-ctl")] + list(args), env=self.env,
                               capture_output=True, text=True, timeout=60 * PATIENCE)
            return r.returncode, r.stdout
        except subprocess.TimeoutExpired:
            return 124, ""

    def status(self):
        rc, out = self.ctl("status")
        try:
            return json.loads(out)["state"]
        except (ValueError, KeyError, TypeError):
            return None

    def start_daemon(self):
        log = open(os.path.join(self.tmp, "daemon.log"), "a")
        self.daemon = subprocess.Popen([os.path.join(self.build, "walpe-daemon")], env=self.env, stdout=log,
                                       stderr=subprocess.STDOUT, cwd=self.tmp)
        for _ in range(int(150 * PATIENCE)):
            time.sleep(0.2)
            if self.ctl("status")[0] == 0:
                return True
        return False

    def pixel(self):
        r = subprocess.run(["import", "-window", "root", "png:-"], env=self.env, capture_output=True)
        if r.returncode != 0 or not r.stdout:
            return None
        im = Image.open(io.BytesIO(r.stdout)).convert("RGB")
        return im.getpixel((im.size[0] // 2, im.size[1] // 2))

    def shows(self, name, secs=60):
        """the centre of the desktop becomes the named colour within secs; returns what it is at the end"""
        want = COLORS[name]
        px = None
        end = time.time() + secs * PATIENCE
        while time.time() < end:
            px = self.pixel()
            if px and all(abs(a - b) <= 12 for a, b in zip(px, want)):
                return True, px
            time.sleep(0.5)
        return False, px

    def close(self):
        if self.daemon and self.daemon.poll() is None:
            self.ctl("quit")
            try:
                self.daemon.wait(timeout=20 * PATIENCE)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
        if self.xvfb:
            self.xvfb.terminate()
        if self.keep:
            print("kept", self.tmp)
        else:
            shutil.rmtree(self.tmp, ignore_errors=True)


def main():
    build, keep, tmp = "build", False, None
    av = sys.argv[1:]
    while av:
        a = av.pop(0)
        if a == "--build" and av:
            build = av.pop(0)
        elif a == "--keep":
            keep = True
        elif a == "--tmp" and av:
            tmp = av.pop(0)
        else:
            print(__doc__)
            return 2
    for tool in ("Xvfb", "import"):
        if not shutil.which(tool):
            print("skipped: no " + tool)
            return 77
    if Image is None:
        print("skipped: no Pillow")
        return 77
    if not os.path.exists(os.path.join(build, "walpe-web.so")):
        print("skipped: this build has no walpe-web.so")
        return 77
    env = Env(build, keep, tmp)
    try:
        if len(os.path.join(env.env["XDG_RUNTIME_DIR"], "walpe.sock")) > 100:
            print("skipped: %s is too long for a socket path (use --tmp with a short directory)" % env.tmp)
            return 77
        if not env.start_x():
            print("skipped: no private X server")
            return 77
        lib = os.path.join(env.tmp, "lib")
        early, late = os.path.join(lib, "early"), os.path.join(lib, "late")
        for d, script in ((early, "install();"),
                          (late, "window.addEventListener('load', function () { setTimeout(install, 800); });")):
            os.makedirs(d)
            open(os.path.join(d, "index.html"), "w").write(PAGE % script)
            json.dump({"type": "web", "file": "index.html", "title": os.path.basename(d),
                       "general": {"properties": {"tint": {"type": "textinput", "text": "Tint", "value": "blue"}}}},
                      open(os.path.join(d, "project.json"), "w"))
        os.makedirs(os.path.dirname(env.config), exist_ok=True)
        # the late page's saved value is green (written the way the engine saves a wallpaper's options)
        json.dump({"config_version": 3, "wallpapers_dir": lib, "pause_covered": False, "pause_fullscreen": False,
                   "pause_dpms": False, "props": {late + "\x1ftint": "green"}}, open(env.config, "w"))
        if not check(env.start_daemon(), "the engine starts"):
            return 1

        # a page with its listener in place while it loads
        rc, out = env.ctl("open", early)
        ok, px = env.shows("blue", 120)
        if not check(rc == 0 and ok, "a page that listens from the start is told the default value (centre %s)" % (px,)):
            st = env.status() or {}
            if st.get("web_error") or "not loaded" in str(st.get("modules")):
                print("skipped: web wallpapers do not run here (%s)" % (st.get("web_error") or st.get("modules")))
                return 77
        rc, out = env.ctl("setprop", "tint=green")
        ok, px = env.shows("green")
        check(rc == 0 and ok, "a changed option reaches it (centre %s)" % (px,))
        rc, out = env.ctl("setprop", "tint=blue")
        ok, px = env.shows("blue")
        check(rc == 0 and ok, "and the next one (centre %s)" % (px,))

        # a page that starts to listen 800 ms after its load event; its saved value is green. The
        # desktop is blue until it paints, red while it has not been told anything.
        rc, out = env.ctl("open", late)
        ok, px = env.shows("green", 120)
        check(rc == 0 and ok, "a page that starts to listen after its load event is told its saved value (centre %s)" % (px,))
        rc, out = env.ctl("setprop", "tint=blue")
        ok, px = env.shows("blue")
        check(rc == 0 and ok, "and a later change (centre %s)" % (px,))

        # switching while pages load, then the page opened last must be there and listening
        for i in range(4):
            env.ctl("open", early if i % 2 == 0 else late)
            time.sleep(0.3)
        env.ctl("open", early)
        env.shows("blue", 120)
        rc, out = env.ctl("setprop", "tint=green")
        ok, px = env.shows("green", 120)
        st = env.status() or {}
        mons = st.get("monitors") or [{}]
        check(rc == 0 and ok and st.get("type") == "web" and "load_error" not in mons[0] and env.daemon.poll() is None,
              "after switching back and forth while pages load the last one is shown and listens (centre %s)" % (px,))
    finally:
        env.close()
    print("webcheck: %d checks, %d failed" % (checks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
