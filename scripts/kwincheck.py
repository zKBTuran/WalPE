#!/usr/bin/env python3
"""End-to-end checks of the daemon on KDE Plasma: a real KWin, and Plasma's shell where it is installed.

  python3 -I scripts/kwincheck.py --build build [--daemon FILE] [--tray FILE] [--only a,b]
                                  [--outputs 1|2] [--no-shell] [--keep] [--tmp DIR]

A private session in a temporary directory: its own runtime directory, its own
session bus (dbus-daemon), `kwin_wayland --virtual` with software rendering on
two virtual outputs, Plasma's shell (plasmashell, with kded6 for the tray)
when it is installed, and a private walpe-daemon with generated scenes. Nothing
of the user's session is read, written or disturbed, no autostart program
runs, and everything started is stopped by process id.

What is checked is what KWin shows and does, not what the daemon believes:
the pictures are KWin's own screenshots (org.kde.KWin.ScreenShot2), the
stacking order and the windows' states are read by a script inside KWin
(org.kde.kwin.Scripting), the pointer is a device KWin makes for the check
(org_kde_kwin_fake_input) and the application windows are real Wayland
clients (xdg-shell, started by this script). Each case starts its own daemon:

  stack     where the wallpaper is among Plasma's windows. WalPE 0.14.0 put
            it on the layer KWin shares with Plasma's desktop window, which
            a click on the desktop raises: the wallpaper was gone, and the
            daemon went on drawing with nothing in its log or status. So:
            started while Plasma's desktop is the active window, and again
            while an application's window is (the order of the report), the
            wallpaper is in KWin's picture of every output and its surface
            is above Plasma's desktop window, before and after a click on
            the bare desktop; status says `wl_layer` "bottom" and why; the
            log has the two lines about Plasma; after `walpe-ctl quit`
            nothing of the wallpaper is left. Without Plasma's shell there
            is no desktop window: the layer, the picture and the quit only.
  pause     the pause rules through the script the daemon loads into KWin:
            status `pause_kwin_script` and KWin both say it is loaded; a
            fullscreen window pauses the output it is on and no other, on
            either output; closing it resumes; a maximized window pauses
            through "covered" (not "fullscreen"); a window of half the
            output pauses nothing. The script is out of KWin after `quit`
            and after SIGTERM, back at the next start (which finds the
            fullscreen window that is already there), replaced without a
            clash of names after a daemon that was killed and left its
            script behind, and after two starts in a row.
  addon     (Plasma's shell) the add-on for Plasma, contrib/plasma, into the
            private directories: `install.sh install` and `select`; the
            desktop shows the colour of the type's page while there is no
            daemon; with the daemon, status says the add-on is in use, the
            wallpaper is in KWin's picture and Plasma's desktop window is
            above the daemon's surface, also after a click; a scene that
            follows the pointer follows it over Plasma's desktop (the
            add-on hands it on); `unselect` gives the desktops their
            earlier wallpaper type and the wallpaper stays shown; with the
            type selected again the daemon quits, and the next one is
            killed: both times the desktop is the page's colour, not
            black; `uninstall` removes it.
  tray      (Plasma's shell and kded6, a build with walpe-tray) what a
            stand-in panel cannot say (tests `tray-life`, `tray-ids`): the
            tray registers its item with Plasma's own
            StatusNotifierWatcher while Plasma is the host. The menu it
            hands out there, read before the engine runs, with it, with a
            wallpaper, without it and with it again: no entry twice, an id
            is never another kind of entry, an entry that stays keeps its
            id, "Quit WalPE" has one id throughout and its click ends the
            engine and the tray.

Exit code 0 = all passed, 1 = failures, 77 = skipped: no kwin_wayland of
Plasma 6, no dbus-daemon, a build without the Wayland backend, or no device
KWin can draw on (it needs a DRM render node or /dev/udmabuf; the build's
kwincheck-udmabuf.so stands in for the latter where there is neither).
WALPE_CHECK_STRICT=1 turns "cannot run here" into a failure. The cases that
need Plasma's shell are left out, and say so, where it is not installed.
KWINCHECK_PATIENCE=N multiplies every wait (a software renderer on a busy
machine: 3 or more). --daemon / --tray: other programs than the build's (an
older release: the check must fail on what it was written for).
--tmp DIR: work in DIR (must not exist; removed afterwards); sockets live
below it and a socket path is limited to 107 bytes."""
import glob, json, os, re, shutil, signal, struct, subprocess, sys, time

sys.dont_write_bytecode = True   # no __pycache__ next to the scripts
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("LIFECHECK_PATIENCE", os.environ.get("KWINCHECK_PATIENCE", "1") or "1")
import displayenv        # noqa: E402
import lifecheck as lc   # noqa: E402  (its Env, scene generators and waits)

rc = lc.rc
PATIENCE = lc.PATIENCE
CONFIG = {"config_version": 3, "fps": 30, "pause_fullscreen": False, "pause_covered": False,
          "pause_dpms": False}
SIZE = (640, 400)                           # of each virtual output
SCRIPT = "io.github.walpe.pause"            # the daemon's script in KWin (src/signals/kwinscript.h)
SCRIPT_LOADED = "the pause script is loaded in KWin"
LAYER_WHY = "the compositor is KWin (KDE Plasma)"
ADDON = "io.github.walpe.plasma"            # the wallpaper type of contrib/plasma
STANDIN = (0, 192, 64)                      # the colour the check gives the type's page
SHIM = "kwincheck-udmabuf.so"
TROUBLE = re.compile(r"CRITICAL \*\*|@\d+: error \d+:|protocol error|Segmentation fault|AddressSanitizer|"
                     r"ThreadSanitizer|runtime error:")
fails = 0
checks = 0


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


def gv(text):
    """a string in GVariant's text form"""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


# ------------------------------------------------------------ Wayland clients
class Wl(displayenv.WlPointer):
    """displayenv's wire-protocol client with the compositor's globals kept by name"""
    MANAGER = None

    def _hello(self):
        self.registry = self._new()
        self._send(1, 1, struct.pack("=I", self.registry))               # wl_display.get_registry
        self.globals = {}

        def on_global(obj, op, body):
            if obj == self.registry and op == 0:                         # wl_registry.global
                name, n = struct.unpack_from("=II", body)
                iface = body[8:8 + n - 1].decode("ascii", "replace")
                self.globals[iface] = (name, struct.unpack_from("=I", body, 8 + ((n + 3) & ~3))[0])
        self._roundtrip(on_global)
        self._setup()

    def _bind(self, iface, version):
        if iface not in self.globals:
            raise OSError("the compositor offers no " + iface)
        obj = self._new()
        self._send(self.registry, 0, struct.pack("=I", self.globals[iface][0]) + self._string(iface) +
                   struct.pack("=II", min(version, self.globals[iface][1]), obj))
        return obj

    def _setup(self):
        pass


class FakeInput(Wl):
    """A pointer device for KWin's seat: its virtual backend has no input device, so the
    seat offers clients no pointer. org_kde_kwin_fake_input is KWin's own protocol for
    programs that make input (KDE Connect uses it); KWin gives it to everybody when it
    was started with KWIN_WAYLAND_NO_PERMISSION_CHECKS=1, as the check starts it. The
    device lasts as long as this client."""

    def _setup(self):
        if self.globals.get("org_kde_kwin_fake_input", (0, 0))[1] < 3:
            raise OSError("the compositor offers no org_kde_kwin_fake_input with absolute positions")
        self.device = self._bind("org_kde_kwin_fake_input", 4)
        self._send(self.device, 0, self._string("walpe kwincheck") + self._string("the pointer of a test"))
        self._roundtrip()

    def move(self, x, y):
        """the pointer to (x, y) of the layout"""
        self._send(self.device, 9, struct.pack("=ii", int(x * 256), int(y * 256)))   # pointer_motion_absolute
        self._roundtrip()

    def button(self, code, pressed):
        self._send(self.device, 2, struct.pack("=II", int(code), 1 if pressed else 0))
        self._roundtrip()


def window_main(title):
    """`kwincheck.py --window TITLE`: an application's window and nothing else. An xdg-shell
    toplevel of one colour (a single-pixel buffer stretched by the viewporter: no shared
    memory) that takes every size and state KWin gives it; it lasts until it is closed or
    this process ends. Prints "mapped" when it has its first picture."""
    w = Wl(os.path.join(os.environ["XDG_RUNTIME_DIR"], os.environ["WAYLAND_DISPLAY"]), 20)
    comp, wm = w._bind("wl_compositor", 1), w._bind("xdg_wm_base", 1)
    vp, spb = w._bind("wp_viewporter", 1), w._bind("wp_single_pixel_buffer_manager_v1", 1)
    surf, xs, top, view, buf = (w._new() for _ in range(5))
    w._send(comp, 0, struct.pack("=I", surf))                            # create_surface
    w._send(wm, 2, struct.pack("=II", xs, surf))                         # get_xdg_surface
    w._send(xs, 1, struct.pack("=I", top))                               # get_toplevel
    w._send(top, 2, w._string(title))                                    # set_title
    w._send(top, 3, w._string("io.github.walpe.kwincheck"))              # set_app_id
    w._send(vp, 1, struct.pack("=II", view, surf))                       # get_viewport
    w._send(spb, 1, struct.pack("=IIIII", buf, 0x70000000, 0x70000000, 0x70000000, 0xffffffff))
    w._send(surf, 6)                                                     # commit: the first configure
    size, mapped = [320, 240], False
    w.s.settimeout(None)
    while True:
        while len(w.buf) < 8 or len(w.buf) < struct.unpack_from("=II", w.buf)[1] >> 16:
            d = w.s.recv(65536)
            if not d:
                return 0
            w.buf += d
        obj, word = struct.unpack_from("=II", w.buf)
        n, op = word >> 16, word & 0xffff
        if n < 8:
            return 1
        body, w.buf = w.buf[8:n], w.buf[n:]
        if obj == 1 and op == 0:                                         # wl_display.error
            sys.stderr.write("window: protocol error: %r\n" % body[12:])
            return 1
        if obj == wm and op == 0:                                        # ping
            w._send(wm, 3, body[:4])
        elif obj == top and op == 0:                                     # configure (width, height, states)
            cw, ch = struct.unpack_from("=ii", body)
            if cw > 0 and ch > 0:
                size = [cw, ch]
        elif obj == top and op == 1:                                     # close
            return 0
        elif obj == xs and op == 0:                                      # configure (serial)
            w._send(xs, 4, body[:4])                                     # ack_configure
            w._send(view, 2, struct.pack("=ii", *size))                  # set_destination
            w._send(surf, 1, struct.pack("=Iii", buf, 0, 0))             # attach
            w._send(surf, 2, struct.pack("=iiii", 0, 0, *size))          # damage
            w._send(surf, 6)                                             # commit
            if not mapped:
                mapped = True
                print("mapped", flush=True)


# ------------------------------------------------------------- the session
JS_WRAP = """(function () {
    function out(x) { print("KWINCHECK @TAG@ " + JSON.stringify(x)); }
    try {
@BODY@
    } catch (e) { out({error: String(e)}); }
    print("KWINCHECK @TAG@ END");
})();
"""
JS_WINDOWS = """
        const order = workspace.stackingOrder;
        for (let i = 0; i < order.length; i++) {
            const w = order[i], g = w.frameGeometry;
            out({cls: String(w.resourceClass), title: String(w.caption), pid: w.pid, layer: w.layer,
                 desktop: !!w.desktopWindow, dock: !!w.dock, out: w.output ? w.output.name : "",
                 geo: [g.x, g.y, g.width, g.height], fs: !!w.fullScreen, min: !!w.minimized,
                 active: w === workspace.activeWindow});
        }
"""
JS_SCREENS = """
        for (const o of workspace.screens) {
            const g = o.geometry;
            out({name: o.name, geo: [g.x, g.y, g.width, g.height]});
        }
"""
JS_WINDOW = """
        const w = workspace.windowList().find(w => w.caption === @TITLE@);
        const g = (workspace.screens.find(o => o.name === @OUT@) || workspace.screens[0]).geometry;
        if (!w) out({missing: true}); else { @DO@ out({done: true}); }
"""


class KwinDisplay:
    """A private Plasma session for the checks, with the interface of displayenv's displays
    as far as the checks' Env uses it: dbus-daemon, kwin_wayland on virtual outputs, a
    pointer device, and (shell) kded6 and plasmashell. Everything lives in the checks'
    own XDG directories and is stopped by process id."""
    kind, winsys, fake_monitors = "kwin", "wayland", False
    seen = displayenv.SwayDisplay.seen

    def __init__(self, env, tmp, build, patience=1.0, outputs=2, shell=True):
        self.env, self.tmp, self.build, self.patience = env, tmp, build, patience
        self.rt = env["XDG_RUNTIME_DIR"]
        self.count, self.shell = outputs, shell
        self.address = "unix:path=" + os.path.join(self.rt, "bus")
        self.procs = []                 # (name, Popen), in the order they were started
        self.log = os.path.join(tmp, "kwin.log")
        self.ptr = None
        self.mons = []
        self.serial = 0
        self.pointer_how = "KWin's fake input device"
        self.why = ""                   # what did not start

    # ------------------------------------------------------------- start / stop
    def session_env(self):
        env = dict(self.env)
        for k in ("DISPLAY", "XAUTHORITY", "SESSION_MANAGER", "WAYLAND_SOCKET", "QT_QPA_PLATFORM",
                  "DESKTOP_STARTUP_ID", "XDG_ACTIVATION_TOKEN", "LD_PRELOAD", "WALPE_MONITORS"):
            env.pop(k, None)
        env.update({"XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": "KDE", "XDG_SESSION_DESKTOP": "KDE",
                    "KDE_FULL_SESSION": "true", "KDE_SESSION_VERSION": "6", "DESKTOP_SESSION": "plasma",
                    "XDG_STATE_HOME": os.path.join(self.tmp, "state"),
                    # (Qt writes to the journal where its stderr is no terminal)
                    "QT_FORCE_STDERR_LOGGING": "1", "LIBGL_ALWAYS_SOFTWARE": "1",
                    "WAYLAND_DISPLAY": "wayland-0", "DBUS_SESSION_BUS_ADDRESS": self.address})
        return env

    def spawn(self, name, argv, env):
        with open(os.path.join(self.tmp, name + ".log"), "ab") as log:
            p = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                 cwd=self.tmp)
        self.procs.append((name, p))
        return p

    def start(self, width, height):
        env = self.session_env()
        try:
            # (the bus has the session's environment: the services it starts for Plasma's
            # shell are clients of this KWin)
            self.spawn("dbus", ["dbus-daemon", "--session", "--nofork", "--nopidfile", "--address=" + self.address],
                       dict(env, QT_QPA_PLATFORM="wayland"))
            if not rc.wait_for(lambda: os.path.exists(os.path.join(self.rt, "bus")), 20, 0.05):
                self.why = "dbus-daemon made no bus socket"
                return False
            # KWIN_WAYLAND_NO_PERMISSION_CHECKS: every client may bind the globals KWin keeps
            # for its own programs (the pointer device). KWIN_SCREENSHOT_NO_PERMISSION_CHECKS:
            # every caller may use org.kde.KWin.ScreenShot2. The logging rules: what a script
            # prints goes to KWin's log, which is how the check reads its answers.
            kenv = dict(env, KWIN_WAYLAND_NO_PERMISSION_CHECKS="1", KWIN_SCREENSHOT_NO_PERMISSION_CHECKS="1",
                        QT_LOGGING_RULES="kwin_scripting.debug=true;js.debug=true")
            kenv.pop("WAYLAND_DISPLAY")
            if not render_device():
                kenv["LD_PRELOAD"] = os.path.join(self.build, SHIM)
            kwin = self.spawn("kwin", ["kwin_wayland", "--virtual", "--width", str(width), "--height", str(height),
                                       "--output-count", str(self.count), "--no-lockscreen",
                                       "--no-global-shortcuts", "--socket", "wayland-0"], kenv)
            sock = os.path.join(self.rt, "wayland-0")
            if not rc.wait_for(lambda: kwin.poll() is not None or
                               (os.path.exists(sock) and self.call("org.kde.KWin", "/Scripting",
                                                                   "org.kde.kwin.Scripting", "isScriptLoaded",
                                                                   "('none',)")[0]), 60, 0.3) or \
                    kwin.poll() is not None:
                self.why = "kwin_wayland did not come up"
                return False
            self.ptr = FakeInput(sock, 10 * self.patience)
            self.ptr.move(5, 5)             # (the first motion makes KWin create the device)
            rc.wait_for(lambda: len(self.js(JS_SCREENS) or []) == self.count, 20, 0.3)
            self.mons = [(o["name"], o["geo"][2], o["geo"][3], o["geo"][0], o["geo"][1])
                         for o in self.js(JS_SCREENS) or [] if "name" in o]
            if len(self.mons) != self.count:
                self.why = "KWin has %d outputs, not %d" % (len(self.mons), self.count)
                return False
            if not self.shot(self.mons[0][0]):
                self.why = "KWin gave no screenshot (org.kde.KWin.ScreenShot2)"
                return False
            if self.shell:
                penv = dict(env, QT_QPA_PLATFORM="wayland")
                try:
                    subprocess.run(["kbuildsycoca6"], env=penv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, timeout=120 * self.patience)
                except (OSError, subprocess.TimeoutExpired):
                    pass
                if shutil.which("kded6"):
                    self.spawn("kded", ["kded6"], penv)
                self.spawn("plasmashell", ["plasmashell", "--no-respawn"], penv)
                if not rc.wait_for(lambda: len(self.desktops()) == self.count, 120, 0.5):
                    self.why = "Plasma's shell made no desktop window on every output"
                    return False
        except OSError as e:
            self.why = str(e)
            return False
        return True

    def stop(self):
        if self.ptr:
            self.ptr.close()
            self.ptr = None
        for _, p in reversed(self.procs):
            if p.poll() is None:
                p.terminate()
        for _, p in reversed(self.procs):
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        self.procs = []
        # what the session's bus and Plasma's shell started themselves (services the bus
        # activates): every process that has this session's runtime directory, a path
        # nothing else has
        for sig in (signal.SIGTERM, signal.SIGKILL):
            left = strays(self.rt)
            for pid in left:
                try:
                    os.kill(pid, sig)
                except OSError:
                    pass
            t0 = time.time()
            while left and strays(self.rt) and time.time() - t0 < 5:
                time.sleep(0.1)
        # (the document portal the bus may have started mounts a FUSE file system in the
        # runtime directory and takes it away when it ends; one that was killed cannot)
        try:
            with open("/proc/self/mounts") as f:
                held = [ln.split()[1] for ln in f if len(ln.split()) > 1 and ln.split()[1].startswith(self.rt + "/")]
        except OSError:
            held = []
        for point in held:
            for tool in ("fusermount3", "fusermount"):
                if shutil.which(tool):
                    subprocess.run([tool, "-u", "-z", point], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
                    break

    def log_tail(self, lines=20):
        out = []
        for name, _ in self.procs:
            try:
                with open(os.path.join(self.tmp, name + ".log"), errors="replace") as f:
                    out += ["--- %s.log\n" % name] + f.readlines()[-lines:]
            except OSError:
                pass
        return "".join(out)

    def client_env(self, env):
        """make env (a dict) that of a program started in this session"""
        keep = self.session_env()
        for k in ("XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "KDE_FULL_SESSION",
                  "KDE_SESSION_VERSION", "DESKTOP_SESSION", "XDG_STATE_HOME", "WAYLAND_DISPLAY",
                  "DBUS_SESSION_BUS_ADDRESS"):
            env[k] = keep[k]
        for k in ("DISPLAY", "XAUTHORITY", "SESSION_MANAGER", "WAYLAND_SOCKET", "WALPE_MONITORS",
                  "DESKTOP_STARTUP_ID", "XDG_ACTIVATION_TOKEN"):
            env.pop(k, None)

    # ------------------------------------------------------------------- D-Bus
    def call(self, dest, path, iface, method, args=None, timeout=40):
        """one method call on the session's bus (the build's kwin-bus) -> (True, the reply
        as Python values) or (False, why)"""
        argv = [os.path.join(self.build, "kwin-bus"), "call", dest, path, iface, method] + ([args] if args else [])
        try:
            p = subprocess.run(argv, env=self.session_env(), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=timeout * self.patience)
            if p.returncode == 0:
                return True, json.loads(p.stdout.decode() or "null")
            return False, p.stderr.decode(errors="replace").strip()
        except (OSError, ValueError, subprocess.TimeoutExpired) as e:
            return False, str(e)

    def has_name(self, name):
        ok, rep = self.call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                            "NameHasOwner", "(%s,)" % gv(name))
        return ok and rep == [True]

    def script_loaded(self, name=SCRIPT):
        """does KWin have a script of this name: True, False, None = KWin did not say"""
        ok, rep = self.call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "isScriptLoaded",
                            "(%s,)" % gv(name))
        return rep[0] if ok and isinstance(rep, list) and rep else None

    def plasma_js(self, code):
        """run a script in Plasma's shell (its desktop scripting) -> what it printed, None = no answer"""
        ok, rep = self.call("org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell", "evaluateScript",
                            "(%s,)" % gv(code))
        return rep[0] if ok and isinstance(rep, list) and rep else None

    # ---------------------------------------------------------- scripts in KWin
    def js(self, body, timeout=15):
        """run JavaScript inside KWin (its script interface); the script calls out(value) and
        the values come back as a list. None = KWin did not run it. The answers travel
        through KWin's log: a script has print() and nothing else to say something with."""
        self.serial += 1
        tag = "%d-%d" % (os.getpid(), self.serial)
        name = "walpe-kwincheck-" + tag
        path = os.path.join(self.rt, name + ".js")
        with open(path, "w") as f:
            f.write(JS_WRAP.replace("@TAG@", tag).replace("@BODY@", body))
        try:
            mark = os.path.getsize(self.log)
        except OSError:
            mark = 0
        got = [None]

        def done():
            with open(self.log, errors="replace") as f:
                f.seek(mark)
                lines = [ln.split("KWINCHECK %s " % tag, 1)[1].strip() for ln in f if "KWINCHECK %s " % tag in ln]
            if "END" not in lines:
                return False
            got[0] = []
            for ln in lines:
                try:
                    got[0].append(json.loads(ln))
                except ValueError:
                    pass
            return True
        try:
            ok, rep = self.call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript",
                                "(%s, %s)" % (gv(path), gv(name)))
            if not ok or not isinstance(rep, list) or not rep or rep[0] < 0:
                return None
            # ("start" runs every loaded script that does not run yet. Not "run" on the
            # object of the id that came back: KWin's ids repeat after an unload.)
            self.call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "start")
            rc.wait_for(done, timeout, 0.05)
        finally:
            self.call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript", "(%s,)" % gv(name))
            try:
                os.unlink(path)
            except OSError:
                pass
        return got[0]

    def windows(self):
        """KWin's windows, bottom first: [{cls, title, pid, layer, desktop, dock, out, geo, fs, min, active}]"""
        return [w for w in self.js(JS_WINDOWS) or [] if "cls" in w]

    def desktops(self):
        return [w for w in self.windows() if w["desktop"] and w["cls"] == "plasmashell"]

    def window(self, title, do, out=""):
        """do something to the window of that title (JavaScript: `w` is the window, `g` the
        rectangle of the output named out); False = no such window"""
        got = self.js(JS_WINDOW.replace("@TITLE@", json.dumps(title)).replace("@OUT@", json.dumps(out))
                      .replace("@DO@", do))
        return bool(got) and {"done": True} in got

    # ------------------------------------------------- the checks' monitor lists
    def place(self, mons, hz=None):
        """the outputs are KWin's, made when it started: what is asked for is not looked at"""
        return list(self.mons)

    # ------------------------------------------------------------------ pictures
    def shot(self, name, timeout=None):
        """KWin's picture of one output -> (width, height, RGB bytes), None on failure"""
        try:
            p = subprocess.run([os.path.join(self.build, "kwin-bus"), "shot", name], env=self.session_env(),
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               timeout=(timeout or 40) * self.patience)
            return displayenv.parse_ppm(p.stdout) if p.returncode == 0 else None
        except (OSError, ValueError, subprocess.TimeoutExpired):
            return None

    def frame(self, mon):
        return self.shot(mon[0])

    def pixel(self, mon, x, y):
        f = self.frame(mon)
        if not f:
            return displayenv.NO_PICTURE
        _, w, h, _, _ = mon
        return displayenv.pixel_of(f, x * f[0] // max(w, 1), y * f[1] // max(h, 1))

    # ------------------------------------------------------------------- pointer
    def can_point(self):
        return bool(self.ptr)

    def pointer(self, x, y, env=None):
        try:
            self.ptr.move(x, y)
            return True
        except (OSError, struct.error, AttributeError):
            return False

    def click(self, x, y, button=1):
        """the pointer to (x, y) of the layout, a button down and up (1 left, 2 middle, 3 right)"""
        try:
            code = {1: 0x110, 2: 0x112, 3: 0x111}[button]
            self.ptr.move(x, y)
            time.sleep(0.15)
            self.ptr.button(code, True)
            time.sleep(0.1)
            self.ptr.button(code, False)
            return True
        except (OSError, struct.error, KeyError, AttributeError):
            return False


def strays(rt):
    """the processes (not this one) that have rt as their runtime directory"""
    want, out = b"XDG_RUNTIME_DIR=" + rt.encode(), []
    for d in os.listdir("/proc"):
        if d.isdigit() and int(d) != os.getpid():
            try:
                with open("/proc/%s/environ" % d, "rb") as f:
                    if want in f.read().split(b"\0"):
                        out.append(int(d))
            except OSError:
                pass
    return out


def render_device():
    """has this machine a device KWin 6.8 and newer can draw on"""
    return bool(glob.glob("/dev/dri/renderD*")) or os.path.exists("/dev/udmabuf")


def unusable(build):
    """why the check cannot run here, and the exit status to end with: (reason, status); None = it can"""
    if "wayland" not in displayenv.built_backends(build):
        return "this build has no wayland backend", displayenv.NOT_BUILT
    lack = [p for p in ("kwin_wayland", "dbus-daemon") if not shutil.which(p)]
    if lack:
        return "needs " + ", ".join(lack) + ": KDE Plasma's compositor is not installed here", displayenv.SKIP
    try:
        p = subprocess.run(["kwin_wayland", "--version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=60 * PATIENCE)
        said = p.stdout.decode(errors="replace").strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return "kwin_wayland cannot be run here (%s)" % e, displayenv.SKIP
    m = re.search(r"kwin (\d+)\.(\d+)", said)
    if not m or int(m.group(1)) < 6:
        return "needs the KWin of Plasma 6: this one says \"%s\"" % (said.splitlines() or ["nothing"])[-1], \
            displayenv.SKIP
    if not render_device() and not os.path.exists(os.path.join(build, SHIM)):
        return "KWin needs a DRM render node or /dev/udmabuf to draw on, and this machine has neither", \
            displayenv.SKIP
    return None


# -------------------------------------------------------------------- helpers
class Window:
    """an application's window in the session (this script again, as `--window TITLE`)"""
    serial = 0

    def __init__(self, env):
        Window.serial += 1
        self.title = "walpe-kwincheck-%d" % Window.serial
        self.env = env
        cenv = dict(env.env)
        self.proc = subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), "--window", self.title],
                                     env=cenv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL)
        self.mapped = bool(wait_for(lambda: self.info() is not None or self.proc.poll() is not None, 30, 0.2)) and \
            self.proc.poll() is None

    def info(self):
        for w in self.env.display.windows():
            if w["title"] == self.title:
                return w
        return None

    def do(self, what, out=""):
        return self.env.display.window(self.title, what, out)

    def put(self, out, fx=0.1, fy=0.1, fw=0.4, fh=0.4):
        """a plain window on that output, at fractions of the output's rectangle, with the focus"""
        self.do("w.fullScreen = false; w.setMaximize(false, false);")
        ok = self.do("w.frameGeometry = {x: g.x + g.width * %g, y: g.y + g.height * %g, width: g.width * %g, "
                     "height: g.height * %g}; workspace.activeWindow = w;" % (fx, fy, fw, fh), out)
        wait_for(lambda: (self.info() or {}).get("out") == out and not (self.info() or {}).get("fs"), 10, 0.2)
        return ok

    def fullscreen(self, out):
        self.put(out)
        self.do("w.fullScreen = true; workspace.activeWindow = w;")
        return bool(wait_for(lambda: (self.info() or {}).get("fs") and (self.info() or {}).get("out") == out, 15, 0.2))

    def maximize(self, out):
        self.put(out)
        return self.do("w.setMaximize(true, true); workspace.activeWindow = w;")

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc.stdout.close()
        wait_for(lambda: self.info() is None, 10, 0.2)


def start(env, what, config=None, extra=None):
    rc.jdump(env.config, dict(CONFIG, **(config or {})))
    return check(env.start_daemon(env.display.mons, extra=extra), what + ": daemon started on KWin's outputs",
                 env.since(0)[-1500:])


def healthy(env, mark, what):
    """the daemon runs and its log has no critical warning, protocol error or sanitizer report since mark"""
    bad = [ln for ln in env.since(mark).splitlines() if TROUBLE.search(ln)]
    return check(env.daemon is not None and env.daemon.poll() is None and not bad,
                 what + ": the daemon runs, its log is clean", bad[:5])


def colour(env, mon, want, timeout=10.0, at=(3, 3)):
    """the name of the colour KWin shows on a monitor (a third of the way in: away from
    the pointer and from Plasma's panel), waited for until it is `want`"""
    box = [None]

    def ok():
        box[0] = rc.name_of(env.display.pixel(mon, mon[1] // at[0], mon[2] // at[1]))
        return box[0] == want
    wait_for(ok, timeout, 0.2)
    return box[0]


def near(rgb, want, tol=12):
    return all(abs(a - b) <= tol for a, b in zip(rgb, want))


def stacking(env):
    """per output: is the daemon's surface above Plasma's desktop window, and the windows
    of that output bottom first, for the reader of a failure"""
    wins = env.display.windows()
    pid = env.daemon.pid if env.daemon else -1
    out = {}
    for name, *_ in env.display.mons:
        on = [w for w in wins if w["out"] == name]
        ours = [i for i, w in enumerate(on) if w["pid"] == pid]
        desk = [i for i, w in enumerate(on) if w["desktop"]]
        out[name] = {"above": bool(ours) and bool(desk) and min(ours) > max(desk),
                     "below": bool(ours) and bool(desk) and max(ours) < min(desk),
                     "order": ["%s%s" % (w["cls"], " (desktop)" if w["desktop"] else "") for w in on]}
    return out


def shown_everywhere(env, what, want="red", above=True):
    """the wallpaper is in KWin's picture of every output, and the daemon's surface is on
    the right side of Plasma's desktop window (above it; with the add-on below it)"""
    got = {m[0]: colour(env, m, want) for m in env.display.mons}
    check(all(c == want for c in got.values()), "%s: KWin shows the wallpaper on every output" % what, got)
    if env.display.shell:
        key = "above" if above else "below"
        wait_for(lambda: all(o[key] for o in stacking(env).values()), 5, 0.3)
        st = stacking(env)
        check(all(o[key] for o in st.values()),
              "%s: the daemon's surface is %s Plasma's desktop window on every output" % (what, key),
              {n: o["order"] for n, o in st.items()})


def click_desktops(env, what):
    """a left click on the bare desktop of every output, and time for KWin to act on it"""
    ok = True
    for name, w, h, x, y in env.display.mons:
        ok = env.display.click(x + w // 2, y + h // 2) and ok
        time.sleep(0.4)
    check(ok, "%s: a left click on the bare desktop of every output" % what)
    time.sleep(1.0 * PATIENCE)


def quit_daemon(env):
    """walpe-ctl quit -> the process's exit status (None: it had to be killed)"""
    p = env.daemon
    env.ctl("quit")
    try:
        p.wait(timeout=15 * PATIENCE)
    except subprocess.TimeoutExpired:
        pass
    code = p.poll()
    env.stop_daemon()
    return code


# ---------------------------------------------------------------------- cases
def case_stack(env, W):
    shell = env.display.shell
    orders = [("Plasma's desktop is the active window", False)] if shell else [("no shell", False)]
    if shell:
        orders.append(("an application's window is the active one", True))
    for n, (order, app) in enumerate(orders):
        what = "stack (%s)" % order
        win = None
        if app:
            win = Window(env)
            if not check(win.mapped and win.put(env.display.mons[0][0]),
                         "%s: an application's window, with the focus" % what, win.info()):
                win.close()
                continue
        elif shell:
            # (a click on the desktop makes it the active window, as in a session where
            # nothing else is open)
            env.display.click(env.display.mons[0][3] + 40, env.display.mons[0][4] + SIZE[1] // 2)
            wait_for(lambda: any(w["active"] for w in env.display.desktops()), 5, 0.3)
        begun = env.mark()
        try:
            if not start(env, what):
                continue
            mark = env.mark()
            env.open(W["red"])
            check(lc.loaded(env, W["red"]), "%s: the red scene is loaded" % what, rc.brief(env.monitors()))
            if n == 0:
                st = env.state()
                check((st.get("wl_layer"), st.get("wl_layer_why")) == ("bottom", LAYER_WHY),
                      "stack: status wl_layer is \"bottom\", because the compositor is KWin",
                      (st.get("wl_layer"), st.get("wl_layer_why")))
                log = env.since(begun)
                check("wayland: KDE Plasma" in log and "bottom" in log,
                      "stack: the log says which layer the wallpaper is on, on KDE Plasma",
                      [ln for ln in log.splitlines() if "wayland:" in ln][:6])
                check(st.get("wl_plasma_addon") == "not installed" and bool(st.get("wl_plasma_hint")),
                      "stack: status says the add-on for Plasma is not installed, and what that means",
                      {k: v for k, v in st.items() if k.startswith("wl_plasma")})
            if win:
                win.close()
                win = None
            shown_everywhere(env, what)
            if shell:
                click_desktops(env, what)
                shown_everywhere(env, what + ", after the click")
            healthy(env, mark, what)
            code = quit_daemon(env)
            wait_for(lambda: all(colour(env, m, "", 0) != "red" for m in env.display.mons), 10, 0.3)
            got = {m[0]: colour(env, m, "", 0) for m in env.display.mons}
            check(code == 0 and all(c != "red" for c in got.values()),
                  "%s: walpe-ctl quit ends the daemon and nothing of the wallpaper is left" % what, (code, got))
        finally:
            if win:
                win.close()
            env.stop_daemon()


def case_pause(env, W):
    mons = env.display.mons
    two = len(mons) > 1
    names = [m[0] for m in mons]
    config = {"render_shared": False, "pause_fullscreen": True, "pause_covered": True}
    if not start(env, "pause", config=config):
        return
    env.open(W["anim"])
    lc.loaded(env, W["anim"])

    def by_output():
        by = {m.get("name"): m for m in env.state().get("monitors") or []}
        return [by.get(n) or {} for n in names]

    def flags(key):
        return [bool(m.get(key)) for m in by_output()]

    def drawn(seconds=1.5):
        n = [m.get("frames") or 0 for m in by_output()]
        time.sleep(seconds * PATIENCE)
        return [(m.get("frames") or 0) - k for m, k in zip(by_output(), n)]

    def cut(values):
        return values[:len(names)]

    def is_state(what, paused, fullscreen=None, covered=None):
        """wait for the outputs' flags (a list per flag, the first output first); then:
        a paused output stops drawing, the others draw"""
        want = {"paused": cut(paused)}
        if fullscreen is not None:
            want["fullscreen"] = cut(fullscreen)
        if covered is not None:
            want["covered"] = cut(covered)
        ok = wait_for(lambda: all(flags(k) == v for k, v in want.items()), 20, 0.2)
        check(bool(ok), "pause: %s: %s" % (what, ", ".join("%s %s" % kv for kv in want.items())),
              {"status": {k: flags(k) for k in ("paused", "fullscreen", "covered", "focused_app")},
               "pause_kwin_script": env.state().get("pause_kwin_script"),
               "windows": [(w["cls"], w["out"], w["geo"], "fs" if w["fs"] else "", "active" if w["active"] else "")
                           for w in env.display.windows() if not w["desktop"] and not w["dock"]]})
        n = drawn()
        check(all(k <= 2 for k, p in zip(n, want["paused"]) if p) and
              all(k > 3 for k, p in zip(n, want["paused"]) if not p),
              "pause: %s: a paused output stops drawing, a free one draws" % what,
              "frames in %.1f s: %s" % (1.5 * PATIENCE, n))

    def script_state(what, want=True):
        """the script is (not) in KWin, and status says the same"""
        ok = wait_for(lambda: env.display.script_loaded() is want, 15, 0.2)
        good = check(bool(ok), "pause: %s: KWin %s the daemon's script (org.kde.kwin.Scripting.isScriptLoaded)"
                     % (what, "has" if want else "no longer has"), env.display.script_loaded())
        if want:
            ok = good and wait_for(lambda: env.state().get("pause_kwin_script") == SCRIPT_LOADED, 15, 0.2)
            return check(bool(ok), "pause: %s: status pause_kwin_script says it is loaded" % what,
                         env.state().get("pause_kwin_script")) and good
        left = [f for f in os.listdir(env.display.rt) if f.startswith("walpe-kwin-")]
        return check(not left, "pause: %s: no file of the script is left in the runtime directory" % what,
                     left) and good

    def again(what, config=config):
        """the next daemon, with the animated scene"""
        if not start(env, "pause: " + what, config=config):
            return False
        env.open(W["anim"])
        return lc.loaded(env, W["anim"])

    win = None
    try:
        mark = env.mark()
        there = script_state("at the start")
        rules = env.state().get("pause_rules") or {}
        check(all((rules.get(k) or {}).get("works") is True and "kwin" in ((rules.get(k) or {}).get("sources") or "")
                  for k in ("pause_fullscreen", "pause_covered")),
              "pause: status: the fullscreen and the covered rule work, and KWin's script is their source",
              {k: rules.get(k) for k in ("pause_fullscreen", "pause_covered")})
        if not there:
            return      # (nothing below can hold without the script, and each would wait for it)
        wait_for(lambda: min(drawn(1.0)) > 3, 20, 0.1)
        is_state("no window", [False, False], [False, False], [False, False])

        win = Window(env)
        if not check(win.mapped and win.put(names[0]), "pause: an application's window on the first output",
                     win.info()):
            return
        is_state("a small window with the focus", [False, False], [False, False], [False, False])
        check(win.fullscreen(names[0]), "pause: KWin shows the window fullscreen on the first output", win.info())
        is_state("the window fullscreen on the first output", [True, False], [True, False])
        check(env.state().get("fullscreen") is True,
              "pause: status of all monitors: a fullscreen window has the focus", env.state().get("fullscreen"))
        if two:
            check(win.fullscreen(names[1]), "pause: ... and fullscreen on the second output", win.info())
            is_state("the window fullscreen on the second output", [False, True], [False, True])
        win.close()
        win = None
        is_state("the fullscreen window is closed", [False, False], [False, False], [False, False])

        win = Window(env)
        if not check(win.mapped and win.maximize(names[0]), "pause: another window, maximized on the first output",
                     win.info()):
            return
        is_state("a maximized window", [True, False], [False, False], [True, False])
        win.put(names[0], 0, 0, 0.5, 1)
        is_state("the window is half the output", [False, False], [False, False], [False, False])
        healthy(env, mark, "pause")

        # the script leaves KWin with the daemon and comes back with the next one
        check(win.fullscreen(names[0]), "pause: the window fullscreen again", win.info())
        is_state("fullscreen again", [True, False], [True, False])
        code = quit_daemon(env)
        check(code == 0, "pause: walpe-ctl quit ends the daemon", code)
        script_state("after quit", False)
        if not again("a second daemon") or not script_state("the second daemon"):
            return
        is_state("the second daemon finds the fullscreen window", [True, False], [True, False])
        # a daemon that could not take its script out leaves it in KWin under its name
        dead = env.daemon
        dead.kill()
        dead.wait()
        env.daemon = None
        if os.path.exists(env.sock):
            os.unlink(env.sock)
        check(env.display.script_loaded() is True, "pause: a killed daemon leaves its script in KWin",
              env.display.script_loaded())
        mark = env.mark()
        if not again("a daemon after a killed one") or not script_state("after a killed daemon"):
            return
        is_state("the script of the new daemon reports, not the old one", [True, False], [True, False])
        log = env.since(mark)
        check("did not take the pause script" not in log and "did not run the pause script" not in log,
              "pause: KWin took the new script although one of the name was loaded",
              [ln for ln in log.splitlines() if "kwin:" in ln])
        # two starts in a row
        for n in ("first", "second"):
            code = quit_daemon(env)
            if not again("the %s of two starts in a row" % n) or \
                    not script_state("the %s of two starts in a row" % n):
                return
        is_state("after two starts in a row", [True, False], [True, False])
        env.daemon.terminate()
        try:
            env.daemon.wait(timeout=15 * PATIENCE)
        except subprocess.TimeoutExpired:
            pass
        check(env.daemon.poll() is not None, "pause: SIGTERM ends the daemon", env.daemon.poll())
        env.stop_daemon()
        script_state("after SIGTERM", False)
    finally:
        if win:
            win.close()
        env.stop_daemon()


def case_addon(env, W):
    d = env.display
    if not d.shell:
        print("skip addon (Plasma's shell is not running)")
        return
    lack = [p for p in ("kpackagetool6", "dbus-send") if not shutil.which(p)]
    if lack:
        print("skip addon (install.sh needs %s)" % ", ".join(lack))
        return
    tool = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "contrib", "plasma", "install.sh")
    if not os.path.exists(tool):
        print("skip addon (no contrib/plasma/install.sh next to the scripts)")
        return
    mons = d.mons

    def run(*args):
        try:
            p = subprocess.run(["sh", tool] + list(args), env=env.env, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120 * PATIENCE)
            return p.returncode, p.stdout.decode(errors="replace").strip()
        except (OSError, subprocess.TimeoutExpired) as e:
            return -1, str(e)

    def types():
        """the wallpaper type of every desktop of Plasma that is on a screen"""
        out = d.plasma_js("for (const d of desktops()) { if (d.screen >= 0) print(d.wallpaperPlugin + '\\n'); }")
        return sorted((out or "").split())

    def standin():
        return {m[0]: d.pixel(m, m[1] // 3, m[2] // 3) for m in mons}

    def addon_state(want):
        ok = wait_for(lambda: (env.state().get("wl_plasma_addon") or "").startswith(want), 40, 0.5)
        return bool(ok)

    before = types()
    try:
        code, out = run("install")
        check(code == 0 and "Installed" in out, "addon: install.sh install", (code, out))
        code, out = run("select")
        # (one desktop per output; Plasma's shell has been seen to keep a second one for
        # the only output of a session, so they are not counted)
        check(code == 0 and out.count("wallpaper type WalPE") >= len(mons),
              "addon: install.sh select: every desktop gets the wallpaper type WalPE", (code, out))
        check(len(types()) >= len(mons) and set(types()) == {ADDON}, "addon: Plasma's shell says the same", types())
        # the colour of the type's page: what such a desktop shows while no daemon runs
        d.plasma_js("for (const d of desktops()) { if (d.wallpaperPlugin !== '%s') continue; "
                    "d.currentConfigGroup = ['Wallpaper', '%s', 'General']; d.writeConfig('Color', '#%02x%02x%02x'); }"
                    % ((ADDON, ADDON) + STANDIN))
        wait_for(lambda: all(near(c, STANDIN) for c in standin().values()), 30, 0.5)
        check(all(near(c, STANDIN) for c in standin().values()),
              "addon: without a daemon the desktops show the colour of the type's page", standin())

        if not start(env, "addon", config={"parallax": True}):
            return
        mark = env.mark()
        env.open(W["red"])
        check(lc.loaded(env, W["red"]), "addon: the red scene is loaded", rc.brief(env.monitors()))
        if not check(addon_state("in use"), "addon: status wl_plasma_addon says the add-on is in use",
                     {k: v for k, v in env.state().items() if k.startswith("wl_plasma")}):
            return      # (a daemon that does not know the add-on: what follows is not about it)
        st = env.state()
        check((st.get("wl_plasma_icons") or "").startswith("shown over") and "wl_plasma_hint" not in st and
              st.get("wl_layer") == "bottom",
              "addon: ... the icons are shown over the wallpaper, no hint, the layer is still \"bottom\"",
              {k: v for k, v in st.items() if k.startswith("wl_plasma") or k.startswith("wl_layer")})
        shown_everywhere(env, "addon", above=False)
        click_desktops(env, "addon")
        shown_everywhere(env, "addon, after the click", above=False)

        # the pointer: Plasma's desktop window has it, the type hands it on
        env.open(W["ptr"])
        lc.loaded(env, W["ptr"])
        ok = wait_for(lambda: (env.state().get("wl_plasma_pointer") or "").startswith("taken from the Plasma add-on"),
                      20, 0.3)
        handed = check(bool(ok), "addon: status wl_plasma_pointer: the pointer is taken from the add-on",
                       env.state().get("wl_plasma_pointer"))
        for name, w, h, x, y in mons if handed else []:
            m = (name, w, h, x, y)
            # (the scene is blue with the pointer at the left edge and red at the right one;
            # looked at in the middle of the height, away from the pointer's row)
            d.pointer(x + w - 5, y + h * 3 // 4)
            d.pointer(x + 5, y + h * 3 // 4)
            got = colour(env, m, "blue", 15, at=(2, 3))
            check(got == "blue", "addon: on %s the scene follows the pointer over Plasma's desktop to the "
                  "left edge (blue)" % name, got)
            d.pointer(x + w - 5, y + h * 3 // 4)
            got = colour(env, m, "red", 15, at=(2, 3))
            check(got == "red", "addon: ... and to the right edge (red)", got)
        healthy(env, mark, "addon")

        # unselect: the desktops' earlier type, the wallpaper in front of them again
        env.open(W["red"])
        lc.loaded(env, W["red"])
        code, out = run("unselect")
        check(code == 0 and types() == before, "addon: install.sh unselect gives every desktop its earlier "
              "wallpaper type", (code, out, types(), before))
        shown_everywhere(env, "addon, unselected")
        check(addon_state("installed, no desktop"), "addon: status follows: installed, no desktop has the type",
              env.state().get("wl_plasma_addon"))

        # selected again; the daemon goes: the page's colour, not a black desktop
        code, out = run("select")
        check(code == 0, "addon: install.sh select again, while the daemon runs", (code, out))
        shown_everywhere(env, "addon, selected again", above=False)
        code = quit_daemon(env)
        wait_for(lambda: all(near(c, STANDIN) for c in standin().values()), 20, 0.5)
        got = standin()
        check(code == 0 and all(near(c, STANDIN) for c in got.values()),
              "addon: the daemon quits and the desktops show the page's colour again, not black", (code, got))
        # ... and a daemon that goes without a word
        if start(env, "addon: one more daemon"):
            env.open(W["red"])
            lc.loaded(env, W["red"])
            shown_everywhere(env, "addon, one more daemon", above=False)
            dead = env.daemon
            dead.kill()
            dead.wait()
            env.daemon = None
            if os.path.exists(env.sock):
                os.unlink(env.sock)
            wait_for(lambda: all(near(c, STANDIN) for c in standin().values()), 20, 0.5)
            got = standin()
            check(all(near(c, STANDIN) for c in got.values()),
                  "addon: that daemon is killed and the desktops show the page's colour, not black", got)
    finally:
        env.stop_daemon()
        code, out = run("uninstall")
        gone = not os.path.exists(os.path.join(env.env["XDG_DATA_HOME"], "plasma", "wallpapers", ADDON))
        check(code == 0 and gone and types() == before,
              "addon: install.sh uninstall removes the type and the desktops have their earlier one",
              (code, out, types()))


def flat_menu(node, parent=0, out=None):
    """dbusmenu's layout (id, properties, children) -> [{id, parent, label, kind}]"""
    out = [] if out is None else out
    ident, props, kids = node[0], node[1] or {}, node[2] or []
    kind = "separator" if props.get("type") == "separator" else \
        "submenu" if props.get("children-display") == "submenu" else props.get("toggle-type") or "entry"
    if ident:
        out.append({"id": ident, "parent": parent, "label": (props.get("label") or "").replace("_", ""),
                    "kind": kind})
    for k in kids:
        flat_menu(k, ident, out)
    return out


def case_tray(env, W):
    d = env.display
    tray_bin = env.tray_bin or os.path.join(env.build, "walpe-tray")
    why = "Plasma's shell is not running" if not d.shell else \
        "no walpe-tray in the build" if not os.path.exists(tray_bin) else ""
    if why:
        print("skip tray (%s)" % why)
        return
    watcher = ("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher")

    def prop(dest, path, iface, name):
        ok, rep = d.call(dest, path, "org.freedesktop.DBus.Properties", "Get", "(%s, %s)" % (gv(iface), gv(name)))
        return rep[0] if ok and isinstance(rep, list) and rep else None

    if not wait_for(lambda: d.has_name(watcher[0]), 30, 0.5):
        print("skip tray (nobody has org.kde.StatusNotifierWatcher on the session's bus: no kded6?)")
        return
    check(bool(wait_for(lambda: prop(*watcher, watcher[0], "IsStatusNotifierHostRegistered") is True, 60, 0.5)),
          "tray: Plasma's shell is a StatusNotifierHost at Plasma's own watcher")
    rc.jdump(env.config, dict(CONFIG))
    lc.put(os.path.join(os.path.dirname(env.config), "tray.ini"), "[tray]\nstart_with_gui=false\nconfirm_quit=false\n")
    tenv = dict(env.env)
    tenv["GTK_A11Y"] = "none"
    log = open(os.path.join(env.tmp, "tray.log"), "a")
    tray = subprocess.Popen([tray_bin], env=tenv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                            cwd=env.tmp)
    log.close()
    try:
        item = [None]

        def registered():
            for entry in prop(*watcher, watcher[0], "RegisteredStatusNotifierItems") or []:
                name, _, path = entry.partition("/")
                ok, rep = d.call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                 "GetConnectionUnixProcessID", "(%s,)" % gv(name))
                if ok and rep == [tray.pid]:
                    item[0] = (name, "/" + path if path else "/StatusNotifierItem")
                    return True
            return False
        if not check(bool(wait_for(registered, 40, 0.5)) and tray.poll() is None,
                     "tray: walpe-tray's item is registered with Plasma's StatusNotifierWatcher",
                     prop(*watcher, watcher[0], "RegisteredStatusNotifierItems")):
            return
        menu = prop(item[0][0], item[0][1], "org.kde.StatusNotifierItem", "Menu")
        if not check(isinstance(menu, str) and menu.startswith("/"), "tray: the item names its menu", menu):
            return
        seen = []                   # (what, the flat menu) of every state looked at

        def layout():
            ok, rep = d.call(item[0][0], menu, "com.canonical.dbusmenu", "GetLayout", "(0, -1, @as [])")
            return flat_menu(rep[1]) if ok and isinstance(rep, list) and len(rep) > 1 else []

        def look(what, want=(), gone=()):
            last = [[]]

            def there():
                last[0] = layout()
                labels = [e["label"] for e in last[0]]
                return all(w in labels for w in want) and not any(g in labels for g in gone)
            wait_for(there, 25, 0.3)
            flat = last[0]
            labels = [e["label"] for e in flat]
            if not check(all(w in labels for w in want) and not any(g in labels for g in gone),
                         "tray: %s: the menu follows" % what, "wanted %s, not %s: %s" % (list(want), list(gone), flat)):
                return
            seen.append((what, flat))
            ids = [e["id"] for e in flat]
            twice = sorted({(e["parent"], e["label"]) for e in flat if e["label"] and
                            [(f["parent"], f["label"]) for f in flat].count((e["parent"], e["label"])) > 1})
            check(len(set(ids)) == len(ids) and not twice and labels.count("Quit WalPE") == 1,
                  "tray: %s: every entry is there once, \"Quit WalPE\" among them" % what,
                  {"ids": ids, "twice": twice})

        look("no engine yet", want=("Start engine", "Settings…"), gone=("Restart engine",))
        if not check(env.start_daemon(d.mons), "tray: daemon started", env.since(0)[-1500:]):
            return
        look("the engine is there", want=("Restart engine",), gone=("Start engine",))
        env.open(W["red"])
        lc.loaded(env, W["red"])
        # (the list of recent wallpapers, and with two monitors a submenu for them)
        more = ("Recent (all monitors)", "Monitors") if len(d.mons) > 1 else ("Recent wallpapers",)
        look("a first wallpaper", want=more)
        env.stop_daemon()
        look("the engine is gone", want=("Start engine",), gone=("Restart engine",) + more[1:])
        if not check(env.start_daemon(d.mons), "tray: daemon started again", env.since(0)[-1500:]):
            return
        look("the engine is back", want=("Restart engine",) + more, gone=("Start engine",))
        # Across the states. A panel keeps the entry it made for an id (Plasma's decides once
        # whether that is a separator, has a check box or opens a submenu), so an id is one
        # kind of entry for good; an entry that goes and comes back may come back under a
        # new id, one that stays has to keep its own.
        kinds, quit_ids, moved = {}, set(), {}
        for n, (what, flat) in enumerate(seen):
            for e in flat:
                kinds.setdefault(e["id"], set()).add(e["kind"])
                if e["label"] == "Quit WalPE":
                    quit_ids.add(e["id"])
            if n:
                was = {e["label"]: e["id"] for e in seen[n - 1][1] if e["parent"] == 0 and e["label"]}
                for e in flat:
                    if e["parent"] == 0 and e["label"] in was and was[e["label"]] != e["id"]:
                        moved["%s (%s)" % (e["label"], what)] = (was[e["label"]], e["id"])
        mixed = {i: sorted(k) for i, k in kinds.items() if len(k) > 1}
        check(len(seen) == 5 and not mixed, "tray: across the five menus an id is one kind of entry (a "
              "separator, a submenu, a check box, a plain entry) and never another", mixed or [w for w, _ in seen])
        check(len(seen) == 5 and not moved and len(quit_ids) == 1,
              "tray: ... an entry of the top level that stays from one menu to the next keeps its id, "
              "\"Quit WalPE\" through all five", (moved, sorted(quit_ids)))
        if len(quit_ids) != 1:
            return
        daemon = env.daemon
        ok, rep = d.call(item[0][0], menu, "com.canonical.dbusmenu", "Event",
                         "(%d, 'clicked', <0>, uint32 0)" % min(quit_ids))
        gone = wait_for(lambda: daemon.poll() is not None and tray.poll() is not None, 20, 0.1)
        check(bool(gone) and daemon.returncode == 0 and tray.returncode == 0,
              "tray: the click on \"Quit WalPE\" ends the engine and the tray",
              "%s; engine %s, tray %s" % (rep, daemon.poll(), tray.poll()))
        if gone:
            env.daemon = None
            if os.path.exists(env.sock):
                os.unlink(env.sock)
    finally:
        if tray.poll() is None:
            tray.terminate()
            try:
                tray.wait(timeout=10 * PATIENCE)
            except subprocess.TimeoutExpired:
                tray.kill()
                tray.wait()
        env.stop_daemon()


CASES = [("stack", case_stack), ("pause", case_pause), ("addon", case_addon), ("tray", case_tray)]


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "--window":
        return window_main(args[1])
    build, daemon, tray, keep, tmp, only, outputs, shell = "build", None, None, False, None, None, 2, True
    while args:
        a = args.pop(0)
        if a == "--build" and args:
            build = args.pop(0)
        elif a == "--daemon" and args:
            daemon = args.pop(0)
        elif a == "--tray" and args:
            tray = os.path.abspath(args.pop(0))
        elif a == "--keep":
            keep = True
        elif a == "--tmp" and args:
            tmp = args.pop(0)
        elif a == "--only" and args:
            only = args.pop(0).split(",")
        elif a == "--outputs" and args and args[0] in ("1", "2"):
            outputs = int(args.pop(0))
        elif a == "--no-shell":
            shell = False
        else:
            sys.stderr.write(__doc__)
            return 2
    skip = unusable(build)
    if skip:
        print("kwincheck: skipped (%s)" % skip[0])
        return skip[1]
    for b in (daemon or os.path.join(build, "walpe-daemon"), os.path.join(build, "walpe-ctl"),
              os.path.join(build, "kwin-bus")):
        if not os.path.exists(b):
            print(f"kwincheck: {b} not found")
            return 2
    if shell and not shutil.which("plasmashell"):
        shell = False
        note("Plasma's shell (plasmashell) is not installed: the cases addon and tray are left out, and of "
             "stack the part with Plasma's desktop window")
    env = lc.Env(build, daemon, keep, tmp, "sway")
    env.tray_bin = tray
    env.display = KwinDisplay(env.env, env.tmp, os.path.abspath(build), PATIENCE, outputs, shell)
    try:
        longest = max(len(env.sock.encode()), len(os.path.join(env.display.rt, "wayland-0.lock").encode()))
        if longest > 107:
            print(f"kwincheck: {env.tmp} is too long for the sockets below it ({longest} > 107 bytes): "
                  "pass --tmp <a short directory>")
            return 2
        wp = os.path.join(env.tmp, "wallpapers")
        W = {k: os.path.join(wp, k) for k in ("red", "ptr", "anim")}
        rc.gen_scene(W["red"], (1, 0, 0))
        lc.gen_fx_scene(W["ptr"], lc.POINTER_FRAG)
        lc.gen_fx_scene(W["anim"], lc.TIME_FRAG)
        if not env.start_x(*SIZE):
            print("kwincheck: skipped (the session did not start: %s)" % env.display.why)
            print(env.display.log_tail())
            return displayenv.SKIP
        note("KWin on %s%s" % (", ".join("%s %dx%d+%d+%d" % m for m in env.display.mons),
                               ", with Plasma's shell" if shell else ", without Plasma's shell"))
        for name, fn in CASES:
            if only and name not in only:
                continue
            try:
                fn(env, W)
            finally:
                env.stop_daemon()
    finally:
        env.close()
    print(f"kwincheck: {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
