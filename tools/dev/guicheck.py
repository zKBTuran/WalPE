#!/usr/bin/env python3
"""End-to-end check of the library window against a running engine (dev harness, like layercheck.py).

  python3 -I tools/dev/guicheck.py --build build [--keep] [--tmp DIR] [--shots DIR]

Starts a private Xvfb (-displayfd), a private D-Bus session, a private walpe-daemon (own XDG dirs in a
temp dir, one 640x360 fake monitor) and the window, opens "Engine settings" through the window's own
application action, and looks at the settings window from outside (screenshots, no test hook in the GUI):

  - the window starts and the settings window opens while the engine runs
  - a wallpaper that fails to load (walpe-ctl open on a scene whose scene.json is not JSON) is reported in
    the window's status line, in the error colour
  - the install dialog closed before the engine has answered its request (the engine is stopped for a
    moment): the window keeps running and the refusal shows in its status line (not tried where
    steamcmd is installed: the request would start a download)
  - the frame rate field of the settings window keeps its value when it is emptied and left, and takes a
    number typed into it
  - settings changed elsewhere while the settings window is open (walpe-ctl set shared=off fx_cache=off
    tex_downscale=off: what a terminal or the tray's menu do; three switches at the top of the window)
    show in the window: its picture changes in the column of switches, and it changes back when they
    are set back
  - the same values again change nothing
  - the window survives the engine going away and coming back (walpe-ctl quit, a new daemon) and follows
    the new engine
  - closing the window with the engine running leaves the engine running

Exit code 0 = all passed, 1 = failures, 77 = skipped (no Xvfb / xdotool / ImageMagick `import` / Pillow /
dbus-daemon). GUICHECK_PATIENCE=N multiplies every wait (a busy machine: 3 or more). --tmp DIR: work in
DIR (must not exist; removed afterwards) instead of a fresh directory under $TMPDIR - the engine's and the
bus's sockets live below it and a socket path is limited to 107 bytes (the harness refuses a longer one:
the engine would fall back to a socket shared with other sessions). --shots DIR keeps its looks at the
settings window as PNG files."""
import hashlib, io, json, os, shutil, signal, subprocess, sys, tempfile, time

try:
    from PIL import Image, ImageChops
except Exception:                       # pragma: no cover
    Image = None

PATIENCE = max(1.0, float(os.environ.get("GUICHECK_PATIENCE", "1") or 1))
APP_ID = "io.github.walpe.ui"
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
    def __init__(self, build, keep, tmp=None):
        self.build = os.path.abspath(build)
        self.keep = keep
        if tmp:
            os.makedirs(tmp)            # must be new: it is removed afterwards
            self.tmp = os.path.abspath(tmp)
        else:
            self.tmp = tempfile.mkdtemp(prefix="walpe-guicheck-")
        self.procs = []
        self.env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8", "TZ": "UTC"}
        for k, d in (("XDG_RUNTIME_DIR", "rt"), ("XDG_CONFIG_HOME", "cfg"), ("XDG_CACHE_HOME", "cache"),
                     ("XDG_DATA_HOME", "data"), ("HOME", "home")):
            p = os.path.join(self.tmp, d)
            os.makedirs(p, mode=0o700)
            self.env[k] = p
        self.env.update({"LIBGL_ALWAYS_SOFTWARE": "1", "NO_AT_BRIDGE": "1", "GDK_BACKEND": "x11",
                         "GSK_RENDERER": "cairo", "GTK_A11Y": "none", "WALPE_MONITORS": "A=640x360+0+0@60",
                         "WALPE_MODULE_DIR": self.build})
        self.config = os.path.join(self.env["XDG_CONFIG_HOME"], "walpe", "config.json")
        self.daemon = None
        self.gui = None

    def start_x(self):
        r, w = os.pipe()
        p = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", "1600x1000x24", "-nolisten", "tcp",
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
        self.procs.append(p)
        self.env["DISPLAY"] = ":" + disp.decode().strip()
        return True

    def start_bus(self):
        p = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1",
                              "--address=unix:path=" + os.path.join(self.env["XDG_RUNTIME_DIR"], "bus")],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=self.env)
        addr = p.stdout.readline().decode().strip()
        if not addr:
            p.kill()
            return False
        self.procs.append(p)
        self.env["DBUS_SESSION_BUS_ADDRESS"] = addr
        return True

    def ctl(self, *args):
        try:
            r = subprocess.run([os.path.join(self.build, "walpe-ctl")] + list(args), env=self.env,
                               capture_output=True, text=True, timeout=30 * PATIENCE)
            return r.returncode, r.stdout
        except subprocess.TimeoutExpired:
            return 124, ""

    def start_daemon(self):
        log = open(os.path.join(self.tmp, "daemon.log"), "a")
        self.daemon = subprocess.Popen([os.path.join(self.build, "walpe-daemon")], env=self.env, stdout=log,
                                       stderr=subprocess.STDOUT, cwd=self.tmp)
        for _ in range(int(150 * PATIENCE)):
            time.sleep(0.2)
            if self.ctl("status")[0] == 0:
                return True
        return False

    def stop_daemon(self):
        if self.daemon:
            self.ctl("quit")
            try:
                self.daemon.wait(timeout=15 * PATIENCE)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
            self.daemon = None

    def start_gui(self):
        log = open(os.path.join(self.tmp, "gui.log"), "a")
        self.gui = subprocess.Popen([os.path.join(self.build, "walpe")], env=self.env, stdout=log,
                                    stderr=subprocess.STDOUT, cwd=self.tmp)
        return self.window("WalPE", 60) is not None

    def xdo(self, *args):
        r = subprocess.run(["xdotool"] + list(args), env=self.env, capture_output=True, text=True)
        return r.stdout.split()

    def window(self, title, secs):
        """X id of the first viewable window with exactly this title, or None"""
        for _ in range(int(secs * 5 * PATIENCE)):
            for wid in self.xdo("search", "--onlyvisible", "--name", "^" + title + "$"):
                return wid
            time.sleep(0.2)
        return None

    def action(self, name):
        r = subprocess.run(["gdbus", "call", "--session", "--dest", APP_ID, "--object-path",
                            "/" + APP_ID.replace(".", "/"), "--method", "org.freedesktop.Application.ActivateAction",
                            name, "[]", "{}"], env=self.env, capture_output=True, text=True)
        return r.returncode == 0

    def shot(self, wid):
        r = subprocess.run(["import", "-window", wid, "png:-"], env=self.env, capture_output=True)
        if r.returncode != 0 or not r.stdout:
            return None
        return Image.open(io.BytesIO(r.stdout)).convert("RGB")

    def settled(self, wid, secs=30):
        """the window's picture once four shots in a row are the same (the quality panel asks the
        engine every two seconds while it is shown: its lines arrive a moment after the window)"""
        last, same = None, 0
        for _ in range(int(secs * 2 * PATIENCE)):
            time.sleep(0.5)
            im = self.shot(wid)
            if im is not None and last is not None and im.size == last.size and \
                    ImageChops.difference(im, last).getbbox() is None:
                same += 1
                if same >= 3:
                    return im
            else:
                same = 0
            last = im
        return last

    def close(self):
        if self.gui and self.gui.poll() is None:
            self.gui.terminate()
            try:
                self.gui.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.gui.kill()
        self.stop_daemon()
        for p in reversed(self.procs):
            p.terminate()
        if self.keep:
            print("kept", self.tmp)
        else:
            shutil.rmtree(self.tmp, ignore_errors=True)


SHOTS = None


def keep_shot(im, name):
    if SHOTS and im is not None:
        im.save(os.path.join(SHOTS, name + ".png"))


def wait_for(env, wid, base, differs, secs=40):
    """look at the window until it differs from base (differs=True) or equals it again, or secs passed"""
    end = time.time() + secs * PATIENCE
    im = None
    while True:
        im = env.settled(wid, 6)
        if (changed(base, im) is not None) == differs or time.time() > end:
            return im


def ask_to_close(display, wid):
    """what the close button of a window manager's title bar does: WM_DELETE_WINDOW sent to the window (on a
    display without a window manager a dialog has no title bar of its own). False without libX11."""
    import ctypes, ctypes.util
    try:
        x = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
    except OSError:
        return False

    class ClientMessage(ctypes.Structure):
        _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                    ("display", ctypes.c_void_p), ("window", ctypes.c_ulong), ("message_type", ctypes.c_ulong),
                    ("format", ctypes.c_int), ("data", ctypes.c_long * 5)]

    class Event(ctypes.Union):
        _fields_ = [("xclient", ClientMessage), ("pad", ctypes.c_long * 24)]

    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x.XInternAtom.restype = ctypes.c_ulong
    x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
    x.XFlush.argtypes = [ctypes.c_void_p]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    dpy = x.XOpenDisplay(display.encode())
    if not dpy:
        return False
    ev = Event()
    ev.xclient.type = 33                # ClientMessage
    ev.xclient.window = int(wid)
    ev.xclient.message_type = x.XInternAtom(dpy, b"WM_PROTOCOLS", 0)
    ev.xclient.format = 32
    ev.xclient.data[0] = x.XInternAtom(dpy, b"WM_DELETE_WINDOW", 0)
    x.XSendEvent(dpy, int(wid), 0, 0, ctypes.byref(ev))
    x.XFlush(dpy)
    x.XCloseDisplay(dpy)
    return True


def status_line(im):
    """the lowest 40 rows of the window's picture: its status line (None without a picture)"""
    if im is None:
        return None
    w, h = im.size
    return im.crop((0, max(0, h - 40), w, h))


def red_in_status_line(im):
    """pixels of the error colour (red text) in the window's status line"""
    if im is None:
        return 0
    n = 0
    for r, g, b in status_line(im).getdata():
        if r > 150 and g < 90 and b < 90:
            n += 1
    return n


def changed(a, b):
    """bounding box of the pixels that differ, None = the same picture"""
    if a is None or b is None or a.size != b.size:
        return (0, 0, 0, 0)
    return ImageChops.difference(a, b).getbbox()


def main():
    build, keep, shots, tmp = "build", False, None, None
    av = sys.argv[1:]
    while av:
        a = av.pop(0)
        if a == "--build" and av:
            build = av.pop(0)
        elif a == "--keep":
            keep = True
        elif a == "--tmp" and av:
            tmp = av.pop(0)
        elif a == "--shots" and av:
            shots = av.pop(0)           # the looks at the settings window as PNG files, for a failure
            os.makedirs(shots, exist_ok=True)
        else:
            print(__doc__)
            return 2
    for tool in ("Xvfb", "xdotool", "import", "dbus-daemon", "gdbus"):
        if not shutil.which(tool):
            print("skipped: no " + tool)
            return 77
    if Image is None:
        print("skipped: no Pillow")
        return 77
    global SHOTS
    SHOTS = shots
    env = Env(build, keep, tmp)
    try:
        if len(os.path.join(env.env["XDG_RUNTIME_DIR"], "walpe.sock")) > 100:
            print("skipped: %s is too long for a socket path (use --tmp with a short directory)" % env.tmp)
            return 77
        if not env.start_x() or not env.start_bus():
            print("skipped: no private X server / D-Bus session")
            return 77
        os.makedirs(os.path.dirname(env.config), exist_ok=True)
        # (the library folder is written out: the daemon's first save spells it out anyway, and the
        # row that shows it would then change the window's layout between two looks)
        lib = os.path.join(env.tmp, "lib")
        os.makedirs(lib)
        json.dump({"config_version": 3, "render_shared": True, "fx_cache": True, "tex_downscale": True,
                   "preview_anim": "off", "wallpapers_dir": lib}, open(env.config, "w"))
        if not check(env.start_daemon(), "the engine starts"):
            return 1
        if not check(env.start_gui(), "the window opens"):
            return 1
        # a wallpaper that does not load: the window says so (the engine answers "open" at once and
        # reports the failure in its status; the status line stayed empty)
        main_wid = env.window("WalPE", 10)
        bad = os.path.join(lib, "broken")
        os.makedirs(bad)
        json.dump({"file": "scene.json", "title": "broken", "type": "scene"}, open(os.path.join(bad, "project.json"), "w"))
        open(os.path.join(bad, "scene.json"), "w").write("{ \"camera\": this is not JSON\n")
        before = red_in_status_line(env.shot(main_wid))
        rc, _ = env.ctl("open", bad)
        seen = 0
        for _ in range(int(40 * PATIENCE)):
            time.sleep(0.5)
            seen = red_in_status_line(env.shot(main_wid))
            if seen > before + 40:
                break
        keep_shot(env.shot(main_wid), "0-load-error")
        check(rc == 0 and before < 20 and seen > before + 40,
              "a wallpaper that fails to load is reported in the window's status line as an error "
              "(error-coloured pixels in the line: %d before, %d after)" % (before, seen))

        # the install dialog closed while its request is on the way (the engine has to be started first,
        # or is busy): the engine's refusal was written into the closed dialog and shown nowhere. The
        # engine is stopped for a moment, so that the answer arrives after the dialog is gone; without
        # steamcmd the answer is a refusal, which now shows in the window's status line.
        # (first a wallpaper that loads: the error line above goes away with it, and stays away)
        good = os.path.join(lib, "plain.png")
        Image.new("RGB", (64, 64), (40, 90, 160)).save(good)
        env.ctl("open", good)
        for _ in range(int(40 * PATIENCE)):
            time.sleep(0.5)
            if red_in_status_line(env.shot(main_wid)) < 20:
                break
        if shutil.which("steamcmd"):
            print("     (the install dialog is not tried: steamcmd is installed and the request would start a download)")
        elif check(env.action("install"), "the Install action is taken"):
            title = "Install from Steam Workshop"
            dlg = env.window(title, 30)
            if check(dlg is not None, "the install dialog opens"):
                env.xdo("mousemove", "--window", dlg, "200", "120")     # keys go to the window under the pointer
                time.sleep(0.5 * PATIENCE)
                env.xdo("type", "123456")
                env.xdo("key", "Tab")
                env.xdo("type", "nobody")
                env.xdo("key", "shift+Tab")
                time.sleep(0.5 * PATIENCE)
                keep_shot(env.shot(dlg), "0-install-dialog")
                line0 = status_line(env.shot(main_wid))
                gone = False
                # (kept short whatever the patience: the dialog's first request waits 2.5 s for an answer)
                os.kill(env.daemon.pid, signal.SIGSTOP)
                try:
                    env.xdo("key", "Return")                            # Install (the button takes it 250 ms later)
                    time.sleep(1.0)
                    ask_to_close(env.env["DISPLAY"], dlg)               # what a title bar's close button does
                    for _ in range(8):
                        time.sleep(0.2)
                        if not env.xdo("search", "--onlyvisible", "--name", "^" + title + "$"):
                            gone = True
                            break
                finally:
                    os.kill(env.daemon.pid, signal.SIGCONT)
                check(gone, "the dialog is closed while the engine has not answered")
                # the first answer ("Steam account applied.") is followed by the refusal, which stays
                seen, new = 0, False
                for _ in range(int(40 * PATIENCE)):
                    time.sleep(0.5)
                    if env.gui.poll() is not None:
                        break
                    im = env.shot(main_wid)
                    seen, new = red_in_status_line(im), changed(line0, status_line(im)) is not None
                    if seen > 40 and new:
                        break
                alive = env.gui.poll() is None
                keep_shot(env.shot(main_wid) if alive else None, "0-install-closed")
                check(alive and gone and seen > 40 and new,
                      "the answer to an install request whose dialog was closed leaves the window running and "
                      "shows in its status line (window running: %s, error-coloured pixels in a new line: %d)"
                      % (alive, seen if new else 0))

        check(env.action("settings"), "the Settings action is taken")
        wid = env.window("Engine settings", 60)
        if not check(wid is not None, "the settings window opens"):
            return 1
        # the number field that has the focus when the window opens is the frame rate limit: emptied and
        # left, it keeps its value (GTK reads an empty field as 0, the range made that the minimum: 5
        # frames a second); a number typed into it is taken
        fps0 = json.load(open(env.config)).get("fps", 60)
        env.xdo("mousemove", "--window", wid, "12", "12")
        env.xdo("key", "ctrl+a", "BackSpace", "Tab")
        time.sleep(2.0 * PATIENCE)
        fps1 = json.load(open(env.config)).get("fps", 60)
        check(fps1 == fps0, "an emptied frame rate field keeps the frame rate (%s, was %s)" % (fps1, fps0))
        env.xdo("key", "shift+Tab")
        env.xdo("key", "ctrl+a")
        env.xdo("type", "48")
        env.xdo("key", "Tab")
        for _ in range(int(20 * PATIENCE)):
            time.sleep(0.5)
            if json.load(open(env.config)).get("fps") == 48:
                break
        check(json.load(open(env.config)).get("fps") == 48, "a number typed into it is taken (config.json fps %s)"
              % json.load(open(env.config)).get("fps"))
        env.ctl("set", "fps=%d" % fps0)
        time.sleep(1.0 * PATIENCE)

        base = env.settled(wid)
        check(base is not None, "the settings window can be looked at")
        keep_shot(base, "1-base")

        # the same values again: nothing moves
        rc, _ = env.ctl("set", "shared=on", "fx_cache=on", "tex_downscale=on")
        again = env.settled(wid)
        keep_shot(again, "2-same-values")
        check(rc == 0 and changed(base, again) is None, "the same values again leave the window as it is")

        # changed from a terminal while the window is open
        rc, _ = env.ctl("set", "shared=off", "fx_cache=off", "tex_downscale=off")
        check(rc == 0, "walpe-ctl set shared=off fx_cache=off tex_downscale=off")
        off = wait_for(env, wid, base, True)
        box = changed(base, off)
        keep_shot(off, "3-switched-off")
        check(box is not None, "the open settings window shows the switches walpe-ctl turned off (changed area %s)" % (box,))
        if box:
            # three switches in the right-hand column: narrow and several rows high
            check(box[0] > base.size[0] // 2 and box[3] - box[1] > 60,
                  "what changed is the column of switches (%d px wide, %d px high)" % (box[2] - box[0], box[3] - box[1]))
        rc, _ = env.ctl("set", "shared=on", "fx_cache=on", "tex_downscale=on")
        back = wait_for(env, wid, base, False)
        check(rc == 0 and changed(base, back) is None, "set back, the window is as it was")

        # the engine goes away and comes back with other settings
        env.stop_daemon()
        cfg = json.load(open(env.config))
        cfg.update({"render_shared": False, "tex_downscale": False})
        json.dump(cfg, open(env.config, "w"))
        time.sleep(1.0 * PATIENCE)
        check(env.gui.poll() is None, "the window survives the engine going away")
        if env.ctl("status")[0] != 0:       # (the window restarts the engine by itself)
            env.start_daemon()
        rc, out = env.ctl("status")
        check(rc == 0, "an engine runs again")
        env.ctl("set", "fx_cache=off")      # one change: the window reads the settings again
        again = wait_for(env, wid, base, True)
        check(changed(base, again) is not None, "the window follows the engine that came back")

        # closing the window leaves the engine alone
        env.gui.terminate()
        try:
            env.gui.wait(timeout=20 * PATIENCE)
        except subprocess.TimeoutExpired:
            env.gui.kill()
        time.sleep(1.0)
        check(env.ctl("status")[0] == 0, "the engine still runs after the window was closed")
    finally:
        env.close()
    print("guicheck: %d checks, %d failed" % (checks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
