#!/usr/bin/env python3
"""End-to-end check of the "Monitor resolution" dialog against a running engine (dev harness; it uses
guicheck.py's private X server, D-Bus session and engine and looks at windows from outside).

  python3 -I tools/dev/resuicheck.py --build build [--keep] [--tmp DIR] [--shots DIR]

(scripts/rescheck.py is the engine's check of the same feature: no window in it.)

The screen is 1280x720 on purpose: the dialog has to fit it. The engine gets fake monitors through
WALPE_MONITORS=@FILE, so the layout can change under it. What is checked:

  first start   a fresh installation (no config.json) with two monitors: the dialog opens by itself when the
                window starts; it fits the screen; Escape closes it and changes nothing (both monitors still
                count as not asked), and it returns when the window is started again
  keyboard      Alt+A in the dialog is "Apply": both monitors are asked and native, the dialog is gone and
                does not return at the next start. Opened again through the application action, the first
                row's choice is changed with the keyboard alone (space, Down, Return, Alt+A): the engine
                reports 720p for that monitor. (Return is "Apply" only from a size field or a check box:
                the focus starts on the first row's list, where Return opens the list.)
  upgrade       an existing installation (a config.json of the previous format) is never asked: no dialog
                at the start of the window, no monitor reported as not asked
  follow        the rows of an open dialog follow walpe-ctl: a resolution set from a terminal changes the
                open window's picture, set back it is as it was. (Engine settings shows the same rows,
                filled by the same function, but below what its window shows when it opens: that
                window is opened and kept open for the rest, its picture judged by nobody.)
  new monitor   a third monitor appears while the window runs: no dialog opens by itself, one desktop
                notification is sent (seen on the bus), and the action with that monitor's name opens the
                dialog for it alone (a smaller window than the one that lists all three)
  many          eight monitors: the dialog still fits the 720 pixel screen (its list scrolls)

Exit code 0 = all passed, 1 = failures, 77 = skipped (a tool is missing, or the engine does not know
{"cmd":"resolution"}: the GUI side cannot be checked without it). RESCHECK_PATIENCE=N multiplies every
wait."""
import importlib.util, json, os, shutil, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PATIENCE = max(1.0, float(os.environ.get("RESCHECK_PATIENCE", os.environ.get("GUICHECK_PATIENCE", "1")) or 1))
DIALOG = "Monitor resolution"


def load(name):
    """a sibling script as a module (python3 -I leaves the script's directory out of sys.path)"""
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gc = load("guicheck")
gc.PATIENCE = PATIENCE      # its waits (window, settled, start_daemon, ctl) follow RESCHECK_PATIENCE too
check = gc.check


def mon_list(n):
    """n fake monitors side by side, the first 1280x720, the others 640x360"""
    out, x = [], 0
    for i in range(n):
        w, h = (1280, 720) if i == 0 else (640, 360)
        out.append("%s=%dx%d+%d+0@60" % ("ABCDEFGH"[i], w, h, x))
        x += w
    return ",".join(out)


class Env(gc.Env):
    def __init__(self, build, keep, tmp=None):
        super().__init__(build, keep, tmp)
        self.monfile = os.path.join(self.tmp, "monitors")
        self.set_monitors(2)
        self.env["WALPE_MONITORS"] = "@" + self.monfile
        self.sock = os.path.join(self.env["XDG_RUNTIME_DIR"], "walpe.sock")
        self.busmon = None

    def set_monitors(self, n):
        with open(self.monfile, "w") as f:
            f.write(mon_list(n) + "\n")

    def start_x(self):
        """as guicheck's, with a 1280x720 screen: what GTK takes as the monitor the dialog must fit"""
        r, w = os.pipe()
        p = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", "1280x720x24", "-nolisten", "tcp",
                              "-s", "0"], pass_fds=[w], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(w)
        import select
        disp, end = b"", time.time() + 30 * PATIENCE
        while not disp.endswith(b"\n") and time.time() < end:
            if not select.select([r], [], [], 0.5)[0]:
                continue        # (a read alone would wait for ever for a server that says nothing)
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

    def ipc(self, req):
        """one request to the engine's socket, the reply as a dict (None: no answer)"""
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(10 * PATIENCE)
            s.connect(self.sock)
            s.sendall((json.dumps(req) + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                c = s.recv(65536)
                if not c:
                    break
                buf += c
            s.close()
            return json.loads(buf.decode())
        except (OSError, ValueError):
            return None

    def listing(self):
        return self.ipc({"cmd": "resolution"}) or {}

    def unasked(self):
        return [m.get("name") for m in self.listing().get("monitors", [])
                if m.get("connected", True) and m.get("asked") is False]

    def resolution(self, name):
        for m in self.listing().get("monitors", []):
            if m.get("name") == name:
                return m.get("resolution")
        return None

    def stop_gui(self):
        if self.gui and self.gui.poll() is None:
            self.gui.terminate()
            try:
                self.gui.wait(timeout=20 * PATIENCE)
            except subprocess.TimeoutExpired:
                self.gui.kill()
        self.gui = None

    def gone(self, title, secs):
        """True once no viewable window has this title"""
        for _ in range(int(secs * 5 * PATIENCE)):
            if not self.xdo("search", "--onlyvisible", "--name", "^" + title + "$"):
                return True
            time.sleep(0.2)
        return False

    def never(self, title, secs):
        """True when no window with this title shows up for secs seconds"""
        end = time.time() + secs * PATIENCE
        while time.time() < end:
            if self.xdo("search", "--onlyvisible", "--name", "^" + title + "$"):
                return False
            time.sleep(0.25)
        return True

    def xdo(self, *args):
        """guicheck's, with a limit: no call to xdotool waits for ever (an empty answer then)"""
        try:
            r = subprocess.run(["xdotool"] + list(args), env=self.env, capture_output=True, text=True,
                               timeout=30 * PATIENCE)
        except subprocess.TimeoutExpired:
            return []
        return r.stdout.split()

    def size(self, wid):
        w = h = 0
        try:
            out = subprocess.run(["xdotool", "getwindowgeometry", "--shell", wid], env=self.env,
                                 capture_output=True, text=True, timeout=30 * PATIENCE).stdout
        except subprocess.TimeoutExpired:
            out = ""
        for line in out.splitlines():
            if line.startswith("WIDTH="):
                w = int(line[6:])
            elif line.startswith("HEIGHT="):
                h = int(line[7:])
        return w, h

    def keys(self, wid, *keys):
        """keys to a window (there is no window manager: it is given the focus first)"""
        self.xdo("windowfocus", "--sync", wid)
        for k in keys:
            self.xdo("key", "--clearmodifiers", k)
            time.sleep(0.3 * PATIENCE)

    def dialog_action(self, names=()):
        arg = "[<[" + ", ".join("'%s'" % n for n in names) + "]>]" if names else "[<@as []>]"
        try:
            r = subprocess.run(["gdbus", "call", "--session", "--timeout", "20", "--dest", gc.APP_ID,
                                "--object-path", "/" + gc.APP_ID.replace(".", "/"), "--method",
                                "org.freedesktop.Application.ActivateAction", "monitor-resolution", arg, "{}"],
                               env=self.env, capture_output=True, text=True, timeout=40 * PATIENCE)
        except subprocess.TimeoutExpired:
            return False
        return r.returncode == 0

    def watch_notifications(self):
        self.busmon = subprocess.Popen(["dbus-monitor", "--session",
                                        "interface='org.freedesktop.Notifications',member='Notify'"],
                                       env=self.env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.procs.append(self.busmon)
        self.busseen = b""

    def notified(self, text, secs):
        """The number of Notify calls seen on the bus, once one of them mentions text; 0 when none
        did within secs. After the first it listens two seconds longer, for one that should not be.
        (The pipe is read with os.read: select() knows nothing of lines a buffered reader has
        already taken out of it - a call is several lines, and with readline() the one that holds
        the text stayed in that buffer until the time was up.)"""
        import select
        fd, want = self.busmon.stdout.fileno(), text.encode()
        end, found = time.time() + secs * PATIENCE, False
        while time.time() < end:
            if not select.select([fd], [], [], 0.5)[0]:
                continue
            c = os.read(fd, 65536)
            if not c:
                break
            self.busseen += c
            if not found and want in self.busseen:
                found = True
                end = time.time() + 2 * PATIENCE
        return self.busseen.count(b"member=Notify") if found else 0

    def poke_root(self):
        """a ConfigureNotify for the root window: the engine looks at its monitor list again"""
        import ctypes, ctypes.util
        try:
            x = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        except OSError:
            return False
        x.XOpenDisplay.restype = ctypes.c_void_p
        x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x.XDefaultRootWindow.restype = ctypes.c_ulong
        x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
        x.XFlush.argtypes = [ctypes.c_void_p]
        x.XCloseDisplay.argtypes = [ctypes.c_void_p]
        d = x.XOpenDisplay(self.env["DISPLAY"].encode())
        if not d:
            return False
        root = x.XDefaultRootWindow(d)
        # an XConfigureEvent laid out by hand (LP64): int type, long serial, int send_event,
        # Display *display, Window event, Window window, int x, y, width, height, ...
        ev = (ctypes.c_long * 24)()
        ints = ctypes.cast(ev, ctypes.POINTER(ctypes.c_int))
        longs = ctypes.cast(ev, ctypes.POINTER(ctypes.c_ulong))
        ints[0] = 22                            # ConfigureNotify
        longs[3], longs[4], longs[5] = d, root, root
        ints[14] = ints[15] = 1                 # width, height
        x.XSendEvent(d, root, 0, 1 << 17, ctypes.cast(ev, ctypes.c_void_p))     # StructureNotifyMask
        x.XFlush(d)
        x.XCloseDisplay(d)
        return True


def fresh_config(env, old_format):
    os.makedirs(os.path.dirname(env.config), exist_ok=True)
    if os.path.exists(env.config):
        os.remove(env.config)
    lib = os.path.join(env.tmp, "lib")
    os.makedirs(lib, exist_ok=True)
    if old_format:      # an installation of the previous release
        json.dump({"config_version": 3, "preview_anim": "off", "wallpapers_dir": lib}, open(env.config, "w"))


def main():
    build, keep, tmp, shots = "build", False, None, None
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
            shots = av.pop(0)
            os.makedirs(shots, exist_ok=True)
        else:
            print(__doc__)
            return 1
    for tool in ("Xvfb", "xdotool", "import", "dbus-daemon", "dbus-monitor", "gdbus"):
        if not shutil.which(tool):
            print("skipped: no " + tool)
            return 77
    if gc.Image is None:
        print("skipped: no Pillow")
        return 77
    gc.SHOTS = shots
    env = Env(build, keep, tmp)
    try:
        if len(env.sock) > 100:
            print("skipped: %s is too long for a socket path (use --tmp with a short directory)" % env.tmp)
            return 77
        if not env.start_x() or not env.start_bus():
            print("skipped: no private X server / D-Bus session")
            return 77

        # ---- first start
        fresh_config(env, False)
        if not check(env.start_daemon(), "the engine starts (fresh installation, two monitors)"):
            return 1
        if not env.listing().get("ok"):
            print("skipped: this engine does not answer {\"cmd\":\"resolution\"}")
            return 77
        check(sorted(env.unasked()) == ["A", "B"], "both monitors count as not asked")
        check(env.start_gui(), "the window opens")
        wid = env.window(DIALOG, 30)
        if not check(wid is not None, "the dialog opens by itself at the first start"):
            return 1
        gc.keep_shot(env.settled(wid), "1-first-start")
        w, h = env.size(wid)
        check(0 < w <= 1280 and 0 < h <= 720, "the dialog fits a 1280x720 screen (%dx%d)" % (w, h))
        h_two = h
        env.keys(wid, "Escape")
        check(env.gone(DIALOG, 10), "Escape closes the dialog")
        check(sorted(env.unasked()) == ["A", "B"], "closing it changed nothing: both still not asked")
        env.stop_gui()
        check(env.start_gui(), "the window starts again")
        wid = env.window(DIALOG, 30)
        check(wid is not None, "the dialog returns at the next start")

        # ---- keyboard
        if wid:
            # Alt+A, not Return: the focus starts on the first row's list, and Return on a list
            # (or on a button) is that list's own - it opens it. Return is the default button
            # only from a size field or a check box.
            env.keys(wid, "alt+a")
            check(env.gone(DIALOG, 20), "Alt+A applies and closes the dialog")
        check(env.unasked() == [], "every monitor counts as asked now")
        check(env.resolution("A") == "native" and env.resolution("B") == "native", "both are native")
        env.stop_gui()
        check(env.start_gui(), "the window starts a third time")
        check(env.never(DIALOG, 6), "no dialog once every monitor was asked about")
        check(env.dialog_action(), "the monitor-resolution action is taken")
        wid = env.window(DIALOG, 30)
        if check(wid is not None, "the action opens the dialog"):
            # focus starts in the first row's list: open it, one entry down (720p), take it, Alt+A = Apply
            env.keys(wid, "space", "Down", "Return", "alt+a")
            check(env.gone(DIALOG, 20), "Alt+A applies the changed row and closes the dialog")
            for _ in range(int(20 * PATIENCE)):
                if env.resolution("A") == "720p":
                    break
                time.sleep(0.25)
            check(env.resolution("A") == "720p", "monitor A is 720p, chosen with the keyboard alone (%s)"
                  % env.resolution("A"))
            check(env.resolution("B") == "native", "monitor B stayed native")

        # ---- open rows follow the command line. Looked at in the dialog: all of it is on the screen.
        # The Resolution group of Engine settings has the same rows (one panel_fill () for both), but
        # it lies below what that window shows when it opens - the eight rows of "Performance" and
        # the quality panel come first - so a picture of the settings window cannot tell whether
        # its rows changed.
        check(env.dialog_action(), "the action opens the dialog once more")
        wid = env.window(DIALOG, 30)
        if check(wid is not None, "the dialog is open"):
            base = env.settled(wid)
            gc.keep_shot(base, "2-dialog")
            rc, _ = env.ctl("set", "resolution=B:720p")
            check(rc == 0, "walpe-ctl set resolution=B:720p")
            other = gc.wait_for(env, wid, base, True)
            gc.keep_shot(other, "3-dialog-after-ctl")
            check(gc.changed(base, other) is not None, "the open dialog shows the change")
            rc, _ = env.ctl("set", "resolution=B:native")
            back = gc.wait_for(env, wid, base, False)
            check(rc == 0 and gc.changed(base, back) is None, "set back, the dialog is as it was")
            env.keys(wid, "Escape")
            check(env.gone(DIALOG, 10), "Escape closes it")
        # the settings window stays open from here on: its rows ask for the listing whenever the
        # status changes (the picture is kept with --shots; nothing is judged by it)
        check(env.action("settings"), "the Settings action is taken")
        swid = env.window("Engine settings", 60)
        if check(swid is not None, "the settings window opens"):
            gc.keep_shot(env.settled(swid), "4-settings")

        # ---- a new monitor while the window runs
        env.watch_notifications()
        env.set_monitors(3)
        check(env.poke_root(), "the engine is told to look at its monitors")
        for _ in range(int(120 * PATIENCE)):    # it restarts itself in place when the layout settled
            if "C" in env.unasked():
                break
            time.sleep(0.25)
        check(env.unasked() == ["C"], "the new monitor counts as not asked (%s)" % env.unasked())
        sent = env.notified("New monitor", 15)
        check(sent == 1, "one notification about the new monitor was sent (%d seen)" % sent)
        check(env.never(DIALOG, 4), "no dialog opened by itself for the new monitor")
        check(env.dialog_action(["C"]), "the action for monitor C is taken")
        wid = env.window(DIALOG, 30)
        if check(wid is not None, "it opens the dialog"):
            gc.keep_shot(env.settled(wid), "5-new-monitor")
            _, h_one = env.size(wid)
            check(0 < h_one < h_two, "the dialog lists the new monitor alone (%d px high, two rows were %d)"
                  % (h_one, h_two))
            env.keys(wid, "Escape")
            env.gone(DIALOG, 10)

        # ---- many monitors
        env.set_monitors(8)
        env.poke_root()
        for _ in range(int(120 * PATIENCE)):
            if len(env.listing().get("monitors", [])) >= 8:
                break
            time.sleep(0.25)
        env.dialog_action()
        wid = env.window(DIALOG, 30)
        if check(wid is not None, "the dialog opens with eight monitors"):
            gc.keep_shot(env.settled(wid), "6-eight-monitors")
            w, h = env.size(wid)
            check(0 < h <= 720, "with eight monitors it still fits the screen (%dx%d)" % (w, h))
            env.keys(wid, "Escape")
        env.stop_gui()
        env.stop_daemon()

        # ---- upgrade: an existing installation is never asked
        env.set_monitors(2)
        fresh_config(env, True)
        if check(env.start_daemon(), "the engine starts on a config.json of the previous format"):
            check(env.unasked() == [], "no monitor of an existing installation counts as not asked")
            check(env.start_gui(), "the window opens")
            check(env.never(DIALOG, 8), "no dialog after the upgrade")
    finally:
        env.close()
    print("resuicheck: %d checks, %d failed" % (gc.checks, gc.fails))
    return 1 if gc.fails else 0


if __name__ == "__main__":
    sys.exit(main())
