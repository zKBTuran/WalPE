#!/usr/bin/env python3
"""Display servers for the end-to-end checks of the daemon: one parameter picks the window system.

  xvfb   a private X server (Xvfb -displayfd, its framebuffer in a file: no screenshot tool
         needed). Monitors are rectangles of its one screen, given to the daemon with
         WALPE_MONITORS.
  sway   a private headless Wayland compositor with the layer shell: sway on wlroots'
         headless backend (WLR_BACKENDS=headless, WLR_LIBINPUT_NO_DEVICES=1, software
         rendering), with a runtime directory and a config of its own. Monitors are real
         outputs (HEADLESS-1, ...): created, given a mode / position / scale and removed
         over sway's IPC socket (what `swaymsg create_output` and `swaymsg output ...` do).
         Pictures are read with grim; the pointer is moved by a virtual pointer device.
  labwc, wayfire, river
         another wlroots compositor in sway's place, for scripts/waylandcheck.py (the other
         checks ask sway for things by name). No command socket: outputs are set with
         wlr-randr, windows and output power through the protocols these compositors
         share (WlDesk below). They start with three headless outputs, of which the
         unused ones are disabled: an output cannot be created while they run. river
         means river up to 0.3 (river-classic); from 0.4 on it needs a window manager
         program, named in WALPE_CHECK_RIVER_INIT. Wayfire does not run as root: started
         by root it is given the user nobody.

A check takes the kind from its --display option, else from WALPE_CHECK_DISPLAY, else xvfb
(`x11` and `wayland` are accepted as other names of xvfb and sway). Everything is started in a
directory the caller owns and stopped by process id; nothing of the user's session is used:
the compositor never sees the session's DISPLAY / WAYLAND_DISPLAY, and the clients get the
private server's address only.

Used as a module by scripts/lifecheck.py and scripts/waylandcheck.py (through the checks'
Env), and from the command line:

  python3 -I scripts/displayenv.py needs [--display KIND] [--build DIR]
        why that kind cannot be used here (programs that are missing; with --build also a
        build without the backend for it), and as exit status what a check ends with then
        (77 = skipped; 1 for missing programs with WALPE_CHECK_STRICT=1); 0 and no output = it can
  python3 -I scripts/displayenv.py run [--display KIND] [--outputs WxH+X+Y,...] -- COMMAND [ARG ...]
        COMMAND inside a private display (as xvfb-run does for X); its exit status is returned
  python3 -I scripts/displayenv.py serve [--display KIND] [--outputs WxH+X+Y,...] --ready FILE
        start the display, write the variables a client needs to FILE (one NAME=value per
        line: DISPLAY or WAYLAND_DISPLAY, WALPE_BACKEND) and stay until SIGTERM / SIGINT;
        for shell tests

WALPE_CHECK_STRICT=1 turns "cannot run here" (exit status 77, a skipped test) into a
failure. WALPE_CHECK_WL_RENDERER=gles2 runs the compositor on GLES 2 instead of pixman (it needs a
DRM render node; a container usually has none). The compositor's own messages go to
sway.log in the work directory."""
import hashlib, json, mmap, os, shutil, signal, socket, struct, subprocess, sys, tempfile, time

KINDS = {"xvfb": "xvfb", "x11": "xvfb", "sway": "sway", "wayland": "sway",
         "labwc": "labwc", "wayfire": "wayfire", "river": "river"}
WINSYS = {"xvfb": "x11", "sway": "wayland", "labwc": "wayland", "wayfire": "wayland", "river": "wayland"}
NEEDS = {"xvfb": ("Xvfb",), "sway": ("sway", "grim"), "labwc": ("labwc", "grim", "wlr-randr"),
         "wayfire": ("wayfire", "grim", "wlr-randr"), "river": ("river", "grim", "wlr-randr")}
# how the configured build header names the two window-system backends
BUILD_DEFINES = {"x11": "HAVE_X11", "wayland": "HAVE_WAYLAND"}
# exit status of a check that cannot run here: 77 (meson: skipped). WALPE_CHECK_STRICT=1
# makes a missing program or a display that does not start a failure instead: for a job
# that has installed everything, where neither may pass unnoticed as a skipped test. A
# build without the backend for the display stays a skip (NOT_BUILT) either way.
NOT_BUILT = 77
SKIP = 1 if os.environ.get("WALPE_CHECK_STRICT", "0") not in ("", "0") else NOT_BUILT


def pick(kind=None):
    """the display kind asked for (argument, else WALPE_CHECK_DISPLAY, else xvfb); None = unknown name"""
    k = (kind or os.environ.get("WALPE_CHECK_DISPLAY") or "xvfb").strip().lower()
    return KINDS.get(k)


def missing(kind):
    """programs this machine lacks for a display kind"""
    return [p for p in NEEDS[kind] if not shutil.which(p)]


def built_backends(build):
    """window systems the build in `build` was configured with, read from its
    walpe-build-config.h. A header that does not name a backend at all is a build from
    before the backends could be chosen: X11 is in it, Wayland is not."""
    try:
        with open(os.path.join(build, "walpe-build-config.h"), errors="replace") as f:
            lines = [ln.split() for ln in f.read().splitlines()]
    except OSError:
        return {"x11"}
    out = set()
    for ws, name in BUILD_DEFINES.items():
        named = [t for t in lines if name in t]
        if not named:
            if ws == "x11":
                out.add(ws)
        elif any(t[:2] == ["#define", name] and t[2:3] != ["0"] for t in named):
            out.add(ws)
    return out


# what a pixel reads as when no picture could be taken: no colour a check expects
NO_PICTURE = (256, 256, 256)


def unusable(kind, build):
    """why a check cannot use this display kind with the build in `build`, and the exit
    status to end with: (reason, status); None = it can"""
    if WINSYS[kind] not in built_backends(build):
        return "this build has no %s backend" % WINSYS[kind], NOT_BUILT
    lack = missing(kind)
    if lack:
        return "needs " + ", ".join(lack), SKIP
    return None


def frame_hash(frame):
    """16 hex digits for a picture (width, height, RGB bytes): the same for the same pixels
    whichever display they were read from; "none" for a picture that could not be taken"""
    if not frame:
        return "none"
    w, h, rgb = frame
    return hashlib.sha256(b"%d %d\n" % (w, h) + bytes(rgb)).hexdigest()[:16]


def parse_ppm(data):
    """binary PPM (P6, 8 bits per channel) -> (width, height, RGB bytes)"""
    if data[:2] != b"P6":
        raise ValueError("not a P6 picture")
    pos, vals = 2, []
    while len(vals) < 3:
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos < len(data) and data[pos] == 35:        # '#': a comment up to the line end
            while pos < len(data) and data[pos] != 10:
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n":
            pos += 1
        if start == pos:
            raise ValueError("cut-off P6 header")
        vals.append(int(data[start:pos]))
    pos += 1                                            # the one white-space byte after maxval
    w, h, top = vals
    if top != 255 or w < 1 or h < 1 or len(data) - pos < w * h * 3:
        raise ValueError("P6 picture of %dx%d, maxval %d, %d bytes" % (w, h, top, len(data) - pos))
    return w, h, data[pos:pos + w * h * 3]


def pixel_of(frame, x, y):
    if not frame:
        return NO_PICTURE
    w, h, rgb = frame
    x, y = min(max(int(x), 0), w - 1), min(max(int(y), 0), h - 1)
    o = (y * w + x) * 3
    return tuple(rgb[o:o + 3])


def parse_outputs(text):
    """ "WxH+X+Y,..." (an optional NAME= in front of each) -> [(name, w, h, x, y)] or None"""
    out = []
    for i, part in enumerate(p for p in text.split(",") if p.strip()):
        name, _, geo = part.strip().rpartition("=")
        try:
            size, x, y = geo.split("+")
            w, h = size.split("x")
            out.append((name or "mon%d" % i, int(w), int(h), int(x), int(y)))
        except ValueError:
            return None
    return out or None


class XvfbDisplay:
    kind, winsys, fake_monitors = "xvfb", "x11", True

    def __init__(self, tmp, patience=1.0):
        self.tmp, self.patience = tmp, patience
        self.proc = None
        self.fb = None
        self.display = None

    def start(self, width, height):
        """Xvfb on a display number it picks itself (-displayfd)"""
        fbdir = os.path.join(self.tmp, "fb")
        os.makedirs(fbdir)
        r, w = os.pipe()
        try:
            self.proc = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", f"{width}x{height}x24",
                                          "-nolisten", "tcp", "-s", "0", "-fbdir", fbdir],
                                         pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            os.close(r)
            os.close(w)
            return False
        os.close(w)
        num = b""
        t0 = time.time()
        os.set_blocking(r, False)
        while b"\n" not in num and time.time() - t0 < 20 * self.patience and self.proc.poll() is None:
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
        self.display = ":" + num.decode().strip()
        self.fb = os.path.join(fbdir, "Xvfb_screen0")
        t0 = time.time()
        while time.time() - t0 < 10 * self.patience:
            if os.path.exists(self.fb) and os.path.getsize(self.fb) > 100:
                return True
            time.sleep(0.2)
        return False

    def stop(self):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            self.proc = None
        shutil.rmtree(os.path.join(self.tmp, "fb"), ignore_errors=True)

    def client_env(self, env):
        """make env (a dict) that of a client of this display, and of no other; the daemon
        is told the backend by name (WALPE_BACKEND), so that a check does not depend on the
        rule that picks one"""
        env["DISPLAY"] = self.display
        env["WALPE_BACKEND"] = self.winsys
        for k in ("WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SWAYSOCK", "I3SOCK"):
            env.pop(k, None)

    def place(self, mons, hz=None):
        """the monitors are rectangles of the one screen: nothing to do here, the daemon is told"""
        return list(mons)

    def seen(self, mons, status_monitors):
        return list(mons)

    def _head(self, m):
        big = struct.unpack(">I", m[0:4])[0] < 65536
        hd = struct.unpack((">" if big else "<") + "25I", m[0:100])
        return {"data": hd[0] + hd[19] * 12, "bpp": hd[11], "bpl": hd[12], "msb": hd[7] == 1,
                "masks": hd[14:17], "width": hd[4], "height": hd[5]}

    def pixel(self, mon, x, y):
        """one pixel of a monitor (x, y from its top left corner), from Xvfb's framebuffer file (XWD)"""
        _, w, h, mx, my = mon
        with open(self.fb, "rb") as f:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            try:
                hd = self._head(m)
                n = hd["bpp"] // 8
                off = hd["data"] + (my + y) * hd["bpl"] + (mx + x) * n
                px = int.from_bytes(m[off:off + n], "big" if hd["msb"] else "little")
            finally:
                m.close()
        out = []
        for mask in hd["masks"]:
            sh = (mask & -mask).bit_length() - 1
            out.append((px & mask) >> sh)
        return tuple(out)

    def frame(self, mon):
        """the picture on a monitor -> (width, height, RGB bytes)"""
        _, w, h, mx, my = mon
        with open(self.fb, "rb") as f:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            try:
                hd = self._head(m)
                n = hd["bpp"] // 8
                raw = b"".join(m[hd["data"] + r * hd["bpl"] + mx * n:hd["data"] + r * hd["bpl"] + (mx + w) * n]
                               for r in range(my, my + h))
            finally:
                m.close()
        rgb = bytearray(w * h * 3)
        whole = all(mask and (mask >> ((mask & -mask).bit_length() - 1)) == 0xff and
                    ((mask & -mask).bit_length() - 1) % 8 == 0 for mask in hd["masks"])
        for c, mask in enumerate(hd["masks"]):
            sh = (mask & -mask).bit_length() - 1
            if whole:                       # a channel is a byte of the pixel: one slice each
                at = n - 1 - sh // 8 if hd["msb"] else sh // 8
                rgb[c::3] = raw[at::n]
            else:
                for i in range(w * h):
                    px = int.from_bytes(raw[i * n:i * n + n], "big" if hd["msb"] else "little")
                    rgb[i * 3 + c] = ((px & mask) >> sh) & 0xff
        return w, h, bytes(rgb)

    def can_point(self):
        return bool(shutil.which("xdotool"))

    def pointer(self, x, y, env=None):
        """put the pointer at a place of the screen; False = no tool for it here, or it
        failed. env: the environment the tool runs in (a check gives its own, with its
        private directories; the display's address is set here either way)"""
        if not self.can_point():
            return False
        env = dict(os.environ if env is None else env)
        self.client_env(env)
        try:
            return subprocess.run(["xdotool", "mousemove", str(int(x)), str(int(y))], env=env,
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  timeout=30 * self.patience).returncode == 0
        except (OSError, subprocess.TimeoutExpired):
            return False


class WlWire:
    """A small Wayland client: the wire protocol over the compositor's socket, nothing to
    install. A subclass says in _hello () what it binds and in _event () what it hears."""

    def __init__(self, path, timeout=5.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.buf = b""
        self.last = 1                       # object 1 is the display
        try:
            self.s.settimeout(timeout)
            self.s.connect(path)
            self._hello()
        except BaseException:               # (no half-made client keeps a connection open)
            self.close()
            raise

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

    def _hello(self):
        pass

    def _event(self, obj, op, body):
        """an event outside a roundtrip that has a listener of its own"""

    def _globals(self):
        """wl_display.get_registry -> (the registry, [(name, interface, version)])"""
        registry = self._new()
        self._send(1, 1, struct.pack("=I", registry))                     # wl_display.get_registry
        found = []

        def on_global(obj, op, body):
            if obj == registry and op == 0:                               # wl_registry.global
                name, n = struct.unpack_from("=II", body)
                iface = body[8:8 + n - 1].decode("ascii", "replace")
                found.append((name, iface, struct.unpack_from("=I", body, 8 + n + (-n % 4))[0]))
        self._roundtrip(on_global)
        return registry, found

    def _bind(self, registry, name, iface, version):
        obj = self._new()
        self._send(registry, 0, struct.pack("=I", name) + self._string(iface) +
                   struct.pack("=II", version, obj))                      # wl_registry.bind
        return obj

    @staticmethod
    def _text(body, at=0):
        """a string argument at an offset of an event's body"""
        n = struct.unpack_from("=I", body, at)[0]
        return body[at + 4:at + 4 + max(n - 1, 0)].decode("utf-8", "replace")

    def _new(self):
        self.last += 1
        return self.last

    @staticmethod
    def _string(text):
        b = text.encode() + b"\0"
        return struct.pack("=I", len(b)) + b + b"\0" * (-len(b) % 4)

    def _send(self, obj, opcode, body=b""):
        self.s.sendall(struct.pack("=II", obj, ((8 + len(body)) << 16) | opcode) + body)

    def _roundtrip(self, on_event=None):
        """wl_display.sync, then events until its callback is done: the compositor has
        handled everything sent before"""
        cb = self._new()
        self._send(1, 0, struct.pack("=I", cb))
        while True:
            while len(self.buf) < 8:
                d = self.s.recv(65536)
                if not d:
                    raise OSError("the compositor closed the connection")
                self.buf += d
            obj, word = struct.unpack_from("=II", self.buf)
            size, op = word >> 16, word & 0xffff
            if size < 8:
                raise OSError("malformed message from the compositor")
            while len(self.buf) < size:
                d = self.s.recv(65536)
                if not d:
                    raise OSError("the compositor closed the connection")
                self.buf += d
            body, self.buf = self.buf[8:size], self.buf[size:]
            if obj == 1 and op == 0:                                      # wl_display.error
                n = struct.unpack_from("=I", body, 8)[0]
                raise OSError("protocol error: " + body[12:12 + n - 1].decode("utf-8", "replace"))
            if obj == cb and op == 0:                                     # wl_callback.done
                return
            (on_event or self._event)(obj, op, body)


class WlPointer(WlWire):
    """A pointer device for the compositor's seat. Sway on the headless backend has no input
    device, so its seat offers no pointer and clients never hear of one. This client asks
    for a virtual pointer (wlr-virtual-pointer-unstable-v1) and keeps it for as long as it
    lives."""
    MANAGER = "zwlr_virtual_pointer_manager_v1"

    def _hello(self):
        """find the virtual pointer manager among the globals, bind it, make the device"""
        registry, found = self._globals()
        found = {iface: name for name, iface, _ in found}
        if self.MANAGER not in found:
            self.close()
            raise OSError("the compositor offers no " + self.MANAGER)
        manager = self._bind(registry, found[self.MANAGER], self.MANAGER, 1)
        self.device = self._new()
        self._send(manager, 0, struct.pack("=II", 0, self.device))        # create_virtual_pointer (default seat)
        self._roundtrip()

    def move(self, x, y, width, height):
        """the pointer to (x, y) of a layout of width x height"""
        ms = int(time.monotonic() * 1000) & 0xffffffff
        self._send(self.device, 1, struct.pack("=IIIII", ms, int(x), int(y), int(width), int(height)))   # motion_absolute
        self._send(self.device, 4)                                        # frame
        self._roundtrip()

    def button(self, code, pressed):
        """a button of the device goes down or up (code as in linux/input-event-codes.h:
        0x110 left, 0x111 right, 0x112 middle)"""
        ms = int(time.monotonic() * 1000) & 0xffffffff
        self._send(self.device, 2, struct.pack("=III", ms, int(code), 1 if pressed else 0))            # button
        self._send(self.device, 4)                                        # frame
        self._roundtrip()


class WlDesk(WlWire):
    """The compositor's window list and its outputs' power switches, for a compositor that
    has no command socket of its own for them: wlr-foreign-toplevel-management (which
    windows there are, whether they are fullscreen and have the focus; requests to make
    one fullscreen on an output, to give it the focus, to close it) and
    wlr-output-power-management (an output off and on)."""
    TOPLEVELS, POWER = "zwlr_foreign_toplevel_manager_v1", "zwlr_output_power_manager_v1"
    STATES = ("maximized", "minimized", "activated", "fullscreen")

    def _hello(self):
        self.manager = self.power_manager = self.seat = None
        self.outs, self.wins = {}, {}        # wl_output object: name; window handle: what is known of it
        self.out_globals = {}                # an output's name in the registry: its wl_output object
        self.registry, found = self._globals()
        for name, iface, version in found:
            self._global(name, iface, version)
        self._roundtrip()
        self._roundtrip()                   # (the windows the first answer announced)

    def _global(self, name, iface, version):
        if iface == self.TOPLEVELS and self.manager is None:
            self.manager = self._bind(self.registry, name, iface, min(version, 3))
        elif iface == self.POWER and self.power_manager is None:
            self.power_manager = self._bind(self.registry, name, iface, 1)
        elif iface == "wl_seat" and self.seat is None:
            self.seat = self._bind(self.registry, name, iface, 1)
        elif iface == "wl_output" and version >= 4:                       # (version 4: the output's name)
            self.out_globals[name] = self._bind(self.registry, name, iface, 4)
            self.outs[self.out_globals[name]] = None

    def _event(self, obj, op, body):
        if obj == self.registry:
            # outputs come and go while this client lives (an output that is disabled is
            # gone for clients, enabled again it is a new one)
            if op == 0:                                                   # global
                name, n = struct.unpack_from("=II", body)
                self._global(name, body[8:8 + n - 1].decode("ascii", "replace"),
                             struct.unpack_from("=I", body, 8 + n + (-n % 4))[0])
            elif op == 1:                                                 # global_remove
                self.outs.pop(self.out_globals.pop(struct.unpack_from("=I", body)[0], None), None)
        elif obj == self.manager:
            if op == 0:                                                   # toplevel (new handle)
                self.wins[struct.unpack_from("=I", body)[0]] = {"app_id": None, "title": None, "state": (),
                                                                "outputs": set()}
        elif obj in self.wins:
            w = self.wins[obj]
            if op in (0, 1):                                              # title, app_id
                w["title" if op == 0 else "app_id"] = self._text(body)
            elif op in (2, 3):                                            # output_enter, output_leave
                out = struct.unpack_from("=I", body)[0]
                (w["outputs"].add if op == 2 else w["outputs"].discard)(out)
            elif op == 4:                                                 # state (an array of numbers)
                n = struct.unpack_from("=I", body)[0] // 4
                w["state"] = tuple(self.STATES[v] if v < len(self.STATES) else str(v)
                                   for v in struct.unpack_from("=%dI" % n, body, 4))
            elif op == 6:                                                 # closed
                del self.wins[obj]
                self._send(obj, 7)                                        # destroy
        elif obj in self.outs and op == 4:                                # wl_output.name
            self.outs[obj] = self._text(body)

    def windows(self):
        """[{id, pid (None: the protocol does not tell), app_id, title, fullscreen, focused,
        maximized, outputs (names)}]"""
        self._roundtrip()
        return [{"id": h, "pid": None, "app_id": w["app_id"], "title": w["title"],
                 "fullscreen": "fullscreen" in w["state"], "focused": "activated" in w["state"],
                 "maximized": "maximized" in w["state"],
                 "outputs": sorted(self.outs.get(o) or "?" for o in w["outputs"])}
                for h, w in sorted(self.wins.items())]

    def _output(self, name):
        return next((o for o, n in self.outs.items() if n == name), 0)

    def fullscreen(self, handle, on=True, output=None):
        """ask for a window to be fullscreen (on that output), or not"""
        if handle not in self.wins:
            return False
        self._roundtrip()
        if on:
            self._send(handle, 8, struct.pack("=I", self._output(output) if output else 0))   # set_fullscreen
        else:
            self._send(handle, 9)                                         # unset_fullscreen
        self._roundtrip()
        return True

    def maximize(self, handle, on=True):
        if handle not in self.wins:
            return False
        self._send(handle, 0 if on else 1)                                # set_maximized, unset_maximized
        self._roundtrip()
        return True

    def focus(self, handle):
        if handle not in self.wins or self.seat is None:
            return False
        self._send(handle, 4, struct.pack("=I", self.seat))               # activate
        self._roundtrip()
        return True

    def power(self, name, on):
        """an output off or on (what `wlopm` does)"""
        self._roundtrip()
        self._roundtrip()                   # (an output that has just appeared: its name)
        out = self._output(name)
        if not out or self.power_manager is None:
            return False
        ctl = self._new()
        self._send(self.power_manager, 0, struct.pack("=II", ctl, out))   # get_output_power
        self._send(ctl, 0, struct.pack("=I", 1 if on else 0))             # set_mode
        self._roundtrip()
        self._send(ctl, 1)                                                # destroy
        self._roundtrip()
        return True


SWAY_CONFIG = """# written by scripts/displayenv.py: a headless session for the checks.
# No background, no bar, no X: the wallpaper under test is the only client that draws.
swaybg_command -
swaynag_command -
xwayland disable
focus_follows_mouse no
default_border none
"""


class SwayDisplay:
    kind, winsys, fake_monitors = "sway", "wayland", False

    def __init__(self, tmp, patience=1.0):
        self.tmp, self.patience = tmp, patience
        self.proc = None
        self.rt = os.path.join(tmp, "wl")           # the compositor's XDG_RUNTIME_DIR
        self.display = None                         # its Wayland socket (absolute path)
        self.ipc_path = None                        # its IPC socket
        self.log = os.path.join(tmp, "sway.log")
        self.ptr = None
        self.pointer_how = "nothing"

    # ------------------------------------------------------------- start / stop
    def start(self, width, height):
        # sway gives up when the path of its IPC socket (sway-ipc.<uid>.<pid>.sock in its
        # runtime directory) does not fit a socket address, 107 bytes; no client could
        # connect to a Wayland socket that long either. Said here: a compositor that
        # "did not start" for this reason is otherwise a riddle (a long TMPDIR is enough)
        longest = len(os.path.join(self.rt, "sway-ipc.4294967295.4194304.sock").encode())
        if longest > 107:
            sys.stderr.write("displayenv: %s is too long for the compositor's sockets (%d > 107 bytes): "
                             "use a shorter work directory (--tmp, or TMPDIR)\n" % (self.rt, longest))
            return False
        os.makedirs(self.rt, mode=0o700)
        home = os.path.join(self.tmp, "wl-home")
        os.makedirs(home, mode=0o700)
        conf = os.path.join(self.tmp, "sway.conf")
        with open(conf, "w") as f:
            f.write(SWAY_CONFIG)
        env = dict(os.environ)
        for k in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SWAYSOCK", "I3SOCK", "WLR_DRM_DEVICES",
                  "WLR_HEADLESS_OUTPUTS", "WLR_NO_HARDWARE_CURSORS"):
            env.pop(k, None)
        gles = os.environ.get("WALPE_CHECK_WL_RENDERER", "pixman") == "gles2"
        env.update({"XDG_RUNTIME_DIR": self.rt, "HOME": home, "XDG_CONFIG_HOME": os.path.join(home, "cfg"),
                    "WLR_BACKENDS": "headless", "WLR_LIBINPUT_NO_DEVICES": "1",
                    "WLR_RENDERER": "gles2" if gles else "pixman", "LIBGL_ALWAYS_SOFTWARE": "1"})
        if gles:
            env["WLR_RENDERER_ALLOW_SOFTWARE"] = "1"
        try:
            with open(self.log, "ab") as log:
                self.proc = subprocess.Popen(["sway", "-c", conf], env=env, stdin=subprocess.DEVNULL,
                                             stdout=log, stderr=subprocess.STDOUT, cwd=self.tmp)
        except OSError:
            return False
        t0 = time.time()
        outs = []
        while time.time() - t0 < 20 * self.patience and self.proc.poll() is None:
            names = os.listdir(self.rt)
            wl = sorted(n for n in names if n.startswith("wayland-") and not n.endswith(".lock"))
            ipc = sorted(n for n in names if n.startswith("sway-ipc.") and n.endswith(".sock"))
            if wl and ipc:
                self.display = os.path.join(self.rt, wl[0])
                self.ipc_path = os.path.join(self.rt, ipc[0])
                outs = self.outputs()
                if outs:
                    break
            time.sleep(0.1)
        else:
            return False
        if not self.display or self.proc.poll() is not None:
            return False
        # (the list that ended the wait: a second question could come back empty)
        return self.output(outs[0]["name"], mode=(width, height), pos=(0, 0), scale=1)

    def stop(self):
        if self.ptr:
            self.ptr.close()
            self.ptr = None
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            self.proc = None
        shutil.rmtree(self.rt, ignore_errors=True)

    def log_tail(self, lines=20):
        try:
            with open(self.log, errors="replace") as f:
                return "".join(f.readlines()[-lines:])
        except OSError:
            return ""

    def client_env(self, env):
        """make env (a dict) that of a client of this compositor, and of no other display.
        The client keeps its own XDG_RUNTIME_DIR: the socket is given as a full path. The
        daemon is told the backend by name (WALPE_BACKEND)."""
        env["WAYLAND_DISPLAY"] = self.display
        env["WALPE_BACKEND"] = self.winsys
        for k in ("DISPLAY", "WAYLAND_SOCKET", "SWAYSOCK", "I3SOCK", "WALPE_MONITORS"):
            env.pop(k, None)

    # ------------------------------------------------------------------ sway IPC
    def _ipc(self, kind, payload=b""):
        """one message of the i3 / sway IPC protocol -> the decoded reply, None on failure"""
        s = None
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(10 * self.patience)
            s.connect(self.ipc_path)
            s.sendall(b"i3-ipc" + struct.pack("=II", len(payload), kind) + payload)
            buf = b""
            while len(buf) < 14 or len(buf) < 14 + struct.unpack_from("=I", buf, 6)[0]:
                d = s.recv(65536)
                if not d:
                    break
                buf += d
            return json.loads(buf[14:].decode())
        except (OSError, ValueError, struct.error, TypeError):
            return None
        finally:
            if s is not None:
                s.close()

    def cmd(self, text):
        """a sway command (what `swaymsg -- <text>` sends) -> did every part succeed"""
        rep = self._ipc(0, text.encode())
        return isinstance(rep, list) and bool(rep) and all(r.get("success") for r in rep)

    def outputs(self, every=False):
        """the outputs as sway reports them (`swaymsg -t get_outputs`), left to right:
        [{name, x, y, width, height (place and size in the layout), scale, mode (w, h),
          refresh (Hz), transform, active}]; disabled ones only with every=True"""
        rep = self._ipc(3)
        out = []
        for o in rep if isinstance(rep, list) else []:
            if not (o.get("active") or every):
                continue
            rect, mode = o.get("rect") or {}, o.get("current_mode") or {}
            out.append({"name": o.get("name"), "x": rect.get("x", 0), "y": rect.get("y", 0),
                        "width": rect.get("width", 0), "height": rect.get("height", 0),
                        "scale": o.get("scale", 1.0), "mode": (mode.get("width", 0), mode.get("height", 0)),
                        "refresh": mode.get("refresh", 0) / 1000.0, "transform": o.get("transform", "normal"),
                        "active": bool(o.get("active"))})
        return sorted(out, key=lambda o: (not o["active"], o["x"], o["y"], o["name"] or ""))

    def output(self, name, mode=None, pos=None, scale=None, hz=60, transform=None, enable=None):
        """change one output: mode (w, h) in pixels, position in the layout, scale,
        transform ("normal", "90", "180", "270", "flipped", ...), enable True / False"""
        parts = []
        if enable is not None:
            parts.append("enable" if enable else "disable")
        if mode:
            parts.append("mode %dx%d@%gHz" % (mode[0], mode[1], hz))
        if pos:
            parts.append("position %d %d" % pos)
        if scale:
            parts.append("scale %g" % scale)
        if transform:
            parts.append("transform %s" % transform)
        return self.cmd("output %s %s" % (name, " ".join(parts))) if parts else True

    def add_output(self, mode=None, pos=None, scale=1, hz=60):
        """a new headless output (`swaymsg create_output`) -> its name, None on failure"""
        before = {o["name"] for o in self.outputs(every=True)}
        if not self.cmd("create_output"):
            return None
        t0 = time.time()
        new = []
        while not new and time.time() - t0 < 10 * self.patience:
            new = [o["name"] for o in self.outputs(every=True) if o["name"] not in before]
            if not new:
                time.sleep(0.1)
        if not new:
            return None
        if mode or pos:
            self.output(new[0], mode=mode, pos=pos, scale=scale, hz=hz)
        return new[0]

    def remove_output(self, name):
        """unplug a headless output (`swaymsg output NAME unplug`)"""
        return self.cmd("output %s unplug" % name)

    def layers(self, name):
        """the layer-shell surfaces on an output, as sway's tree lists them:
        [{namespace, layer ("background", "bottom", "top", "overlay"), width, height}];
        None when this sway does not say (1.9 lists no layer surfaces over its IPC socket
        any more, neither in get_outputs nor in get_tree): the caller needs another source"""
        tree, outs = self._ipc(4), self._ipc(3)
        nodes = ((tree.get("nodes") if isinstance(tree, dict) else None) or []) + \
                (outs if isinstance(outs, list) else [])
        for node in nodes:
            if node.get("name") == name and "layer_shell_surfaces" in node:
                return [{"namespace": s.get("namespace"), "layer": s.get("layer"),
                         "width": (s.get("extent") or {}).get("width"),
                         "height": (s.get("extent") or {}).get("height")}
                        for s in node.get("layer_shell_surfaces") or []]
        return None

    def power(self, name, on):
        """switch an output off or on (`output NAME power off|on`). A headless output that
        was off draws nothing after it is switched on again until something makes the
        compositor commit it once more (sway 1.9 / wlroots 0.17: a picture that changed
        while it was off is not shown and a screenshot waits for ever; a monitor on a
        real connector is lit with a new frame). Setting the scale it has does that, but
        only after the output's first frame: repeated until a screenshot can be taken."""
        if not self.cmd("output %s power %s" % (name, "on" if on else "off")):
            return False
        scale = [o["scale"] for o in self.outputs() if o["name"] == name] if on else []
        t0 = time.time()
        while scale and time.time() - t0 < 20 * self.patience:
            time.sleep(0.2)
            self.cmd("output %s scale %g" % (name, scale[0]))
            if self.shot(name, timeout=2.0):
                break
        return True

    def windows(self):
        """the application windows sway manages: [{pid, app_id, fullscreen}]"""
        out, todo = [], [self._ipc(4)]
        while todo:
            node = todo.pop()
            if not isinstance(node, dict):
                continue
            if node.get("pid") and node.get("type") in ("con", "floating_con"):
                out.append({"pid": node["pid"], "app_id": node.get("app_id"),
                            "fullscreen": bool(node.get("fullscreen_mode"))})
            todo += (node.get("nodes") or []) + (node.get("floating_nodes") or [])
        return out

    def window_of(self, pid, app_id=None):
        """the window of a process (a compositor that does not tell process ids: the one
        with this application id) -> its entry of windows (), None while there is none"""
        for w in self.windows():
            if w["pid"] == pid or (w["pid"] is None and app_id and w["app_id"] == app_id):
                return w
        return None

    def win_fullscreen(self, win, on=True, output=None):
        """a window fullscreen (on that output, moved there first), or not any more"""
        sel = "[pid=%d] " % win["pid"]
        if on and output:
            self.cmd(sel + "move container to output " + output)
        return self.cmd(sel + "fullscreen " + ("enable" if on else "disable"))

    def win_focus(self, win, output=None):
        """the focus to a window (moved to that output first)"""
        sel = "[pid=%d] " % win["pid"]
        if output:
            self.cmd(sel + "move container to output " + output)
        return self.cmd(sel + "focus")

    def focus_output(self, name):
        """the focus away from every window, to an output's bare desktop; False = this
        compositor has no way to ask for that"""
        return self.cmd("focus output " + name)

    def pointer_follows_focus(self, on):
        """sway takes the pointer along when the focus changes output (mouse_warping
        output, its default): off while a check moves the focus about, or the pointer
        ends up in the pictures of the cases that follow"""
        return self.cmd("mouse_warping " + ("output" if on else "none"))

    def layout_box(self):
        """the box around the enabled outputs in the layout: (x, y, width, height)"""
        outs = self.outputs()
        if not outs:
            return 0, 0, 0, 0
        x0, y0 = min(o["x"] for o in outs), min(o["y"] for o in outs)
        return (x0, y0, max(o["x"] + o["width"] for o in outs) - x0,
                max(o["y"] + o["height"] for o in outs) - y0)

    def layout_size(self):
        """the far corner of the layout (its width and height when it begins at 0, 0)"""
        x0, y0, w, h = self.layout_box()
        return x0 + w, y0 + h

    # ------------------------------------------------- the checks' monitor lists
    def place(self, mons, hz=None):
        """make the outputs these monitors [(name, w, h, x, y)]: as many outputs, each with
        its size as the mode, scale 1, at its place. -> the list with the outputs' names
        (the compositor names them; the names asked for cannot be given)"""
        have = [o["name"] for o in self.outputs(every=True)]
        while len(have) < len(mons):
            n = self.add_output()
            if not n:
                break
            have.append(n)
        for n in have[len(mons):]:
            self.remove_output(n)
        have = have[:len(mons)]
        out = []
        for i, (name, (_, w, h, x, y)) in enumerate(zip(have, mons)):
            self.output(name, enable=True, mode=(w, h), pos=(x, y), scale=1, transform="normal",
                        hz=(hz[i] if hz else 60))
            self.power(name, True)                      # (whatever a check before left behind)
            out.append((name, w, h, x, y))
        return out

    def seen(self, mons, status_monitors):
        """the monitor list in the daemon's order and with the daemon's names.
        status_monitors() gives the "monitors" of its status; a monitor is recognised by
        its rectangle. The list is returned as it is when the daemon never shows the same
        rectangles (the check that follows then fails on what status says)."""
        got = []
        t0 = time.time()
        while time.time() - t0 < 15 * self.patience:
            got = status_monitors() or []
            rects = sorted(tuple(m.get("geometry") or ()) for m in got)
            if rects == sorted((x, y, w, h) for _, w, h, x, y in mons):
                return [(m.get("name") or "", m["geometry"][2], m["geometry"][3], m["geometry"][0],
                         m["geometry"][1]) for m in got]
            time.sleep(0.3)
        return list(mons)

    # ------------------------------------------------------------------ pictures
    def shot(self, name, timeout=None):
        """one output's picture in its own pixels (grim) -> (width, height, RGB bytes), None on
        failure (grim waits for the output's next frame: at most `timeout` seconds)"""
        env = dict(os.environ)
        self.client_env(env)
        env["XDG_RUNTIME_DIR"] = self.rt
        try:
            p = subprocess.run(["grim", "-t", "ppm", "-o", name, "-"], env=env, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               timeout=(timeout or 30) * self.patience)
            return parse_ppm(p.stdout) if p.returncode == 0 else None
        except (OSError, ValueError, subprocess.TimeoutExpired):
            return None

    def frame(self, mon):
        """the picture on a monitor -> (width, height, RGB bytes), None when none could be taken"""
        return self.shot(mon[0])

    def pixel(self, mon, x, y):
        """one pixel of a monitor (x, y from its top left corner, in layout units);
        NO_PICTURE when the screenshot failed"""
        f = self.frame(mon)
        if not f:
            return NO_PICTURE
        _, w, h, _, _ = mon
        return pixel_of(f, x * f[0] // max(w, 1), y * f[1] // max(h, 1))

    # ------------------------------------------------------------------- pointer
    def can_point(self):
        """is there a way to move the pointer: a virtual pointer device of our own (then
        the seat has a pointer and clients are told so), else sway's `seat ... cursor set`
        (which moves the cursor, but a seat without any device offers clients no pointer)"""
        if self.ptr is None and self.display:
            try:
                self.ptr = WlPointer(self.display, 5 * self.patience)
                self.pointer_how = "a virtual pointer device"
            except (OSError, struct.error):
                self.ptr = False
                self.pointer_how = "sway's seat command (no pointer device: clients may not be told)"
        return bool(self.ptr) or (self.ptr is False and self.cmd("seat seat0 cursor move 0 0"))

    def click(self, button=1):
        """press and release a button (1 left, 2 middle, 3 right) where the pointer is"""
        if not self.can_point():
            return False
        if not self.ptr:
            return self.cmd("seat seat0 cursor press button%d" % button) and \
                self.cmd("seat seat0 cursor release button%d" % button)
        try:
            code = {1: 0x110, 2: 0x112, 3: 0x111}[button]
            self.ptr.button(code, True)
            self.ptr.button(code, False)
            return True
        except (OSError, struct.error, KeyError):
            return False

    def pointer(self, x, y, env=None):
        """put the pointer at a place of the layout; False = it could not be moved
        (env: taken for the other display kind's sake, no program is started here)"""
        if not self.can_point():
            return False
        if not self.ptr:
            return self.cmd("seat seat0 cursor set %d %d" % (int(x), int(y)))
        # the device's absolute position is a fraction of the box around all outputs,
        # which need not begin at 0, 0
        x0, y0, w, h = self.layout_box()
        if w < 1 or h < 1:      # (no output, or sway did not answer: a fraction of nothing)
            return False
        try:
            self.ptr.move(x - x0, y - y0, w, h)
            return True
        except (OSError, struct.error):
            return False


WAYFIRE_INI = """# written by scripts/displayenv.py: a headless session for the checks
[core]
plugins = foreign-toplevel place wm-actions
xwayland = false
"""
# The other compositors: {kind: (command line, files of its configuration: {path below the
# private XDG_CONFIG_HOME: text}, refuses to run as root)}. Each runs on wlroots' headless
# backend like sway and is driven through the protocols wlroots compositors share instead
# of a command socket: wlr-output-management (the program wlr-randr), wlr-screencopy (grim),
# wlr-virtual-pointer, wlr-foreign-toplevel-management and wlr-output-power-management.
COMPOSITORS = {
    "labwc": (["labwc"], {}, False),
    "wayfire": (["wayfire", "-c", "{cfg}/wayfire.ini"], {"wayfire.ini": WAYFIRE_INI}, True),
    # river up to 0.3 ("river-classic"), which manages windows itself. From 0.4 on river
    # shows nothing, a wallpaper included, until a window manager program is connected:
    # WALPE_CHECK_RIVER_INIT is the command line river starts (its -c), for that program
    "river": (["river", "-c", "{river_init}"], {}, False),
}
SPARE_OUTPUTS = 3       # a headless backend's outputs are made when it starts: the most a check uses


class WlrDisplay(SwayDisplay):
    """labwc, Wayfire or river in place of sway. What is different: outputs cannot be
    created or unplugged while the compositor runs, so it starts with SPARE_OUTPUTS and an
    output that "appears" is a disabled one enabled, one that is "unplugged" is disabled
    (the client sees the same: the wl_output global comes and goes); and windows are known
    by their application id, not by a process id."""
    fake_monitors = False

    def __init__(self, kind, tmp, patience=1.0):
        SwayDisplay.__init__(self, tmp, patience)
        self.kind = kind
        self.log = os.path.join(tmp, kind + ".log")
        self.desk = None

    def start(self, width, height):
        argv, files, no_root = COMPOSITORS[self.kind]
        os.makedirs(self.rt, mode=0o700)
        home = os.path.join(self.tmp, "wl-home")
        cfg = os.path.join(home, "cfg")
        os.makedirs(cfg, mode=0o700)
        for name, text in files.items():
            os.makedirs(os.path.dirname(os.path.join(cfg, name)), exist_ok=True)
            with open(os.path.join(cfg, name), "w") as f:
                f.write(text)
        env = dict(os.environ)
        for k in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SWAYSOCK", "I3SOCK", "WLR_DRM_DEVICES",
                  "WLR_NO_HARDWARE_CURSORS"):
            env.pop(k, None)
        gles = os.environ.get("WALPE_CHECK_WL_RENDERER", "pixman") == "gles2"
        env.update({"XDG_RUNTIME_DIR": self.rt, "HOME": home, "XDG_CONFIG_HOME": cfg,
                    "WLR_BACKENDS": "headless", "WLR_LIBINPUT_NO_DEVICES": "1",
                    "WLR_HEADLESS_OUTPUTS": str(SPARE_OUTPUTS),
                    "WLR_RENDERER": "gles2" if gles else "pixman", "LIBGL_ALWAYS_SOFTWARE": "1"})
        if gles:
            env["WLR_RENDERER_ALLOW_SOFTWARE"] = "1"
        argv = [a.format(cfg=cfg, river_init=os.environ.get("WALPE_CHECK_RIVER_INIT") or "true") for a in argv]
        if no_root and os.geteuid() == 0:
            # (Wayfire ends at once as root. A user of its own for the compositor; the
            # checks' clients stay what they are and reach its socket by its full path)
            uid = 65534
            for d in (self.rt, home, cfg):
                os.chown(d, uid, uid)
            os.chmod(self.tmp, os.stat(self.tmp).st_mode | 0o011)
            argv = ["setpriv", "--reuid", str(uid), "--regid", str(uid), "--clear-groups"] + argv
        try:
            with open(self.log, "ab") as log:
                self.proc = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL, stdout=log,
                                             stderr=subprocess.STDOUT, cwd=self.rt)
        except OSError:
            return False
        t0 = time.time()
        outs = []
        while time.time() - t0 < 20 * self.patience and self.proc.poll() is None:
            wl = sorted(n for n in os.listdir(self.rt) if n.startswith("wayland-") and not n.endswith(".lock"))
            if wl:
                self.display = os.path.join(self.rt, wl[0])
                outs = self.outputs(every=True)
                if outs:
                    break
            time.sleep(0.1)
        else:
            return False
        if not self.display or self.proc.poll() is not None:
            return False
        for o in outs[1:]:
            self.output(o["name"], enable=False)
        return self.output(outs[0]["name"], enable=True, mode=(width, height), pos=(0, 0), scale=1)

    def stop(self):
        if self.desk:
            self.desk.close()
            self.desk = None
        SwayDisplay.stop(self)

    # ------------------------------------------------- outputs: wlr-randr
    def _randr(self, args, timeout=10):
        env = dict(os.environ)
        self.client_env(env)
        env["XDG_RUNTIME_DIR"] = self.rt
        try:
            return subprocess.run(["wlr-randr"] + args, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, timeout=timeout * self.patience)
        except (OSError, subprocess.TimeoutExpired):
            return None

    def cmd(self, text):
        return False                        # (no command socket)

    def outputs(self, every=False):
        """the outputs as `wlr-randr --json` lists them, in the form of SwayDisplay.outputs ()"""
        p = self._randr(["--json"])
        try:
            rep = json.loads(p.stdout.decode()) if p and p.returncode == 0 else []
        except ValueError:
            rep = []
        out = []
        for o in rep if isinstance(rep, list) else []:
            on = bool(o.get("enabled"))
            if not (on or every):
                continue
            mode = next((m for m in o.get("modes") or [] if m.get("current")), {})
            pos, scale, turn = o.get("position") or {}, o.get("scale") or 1.0, o.get("transform") or "normal"
            w, h = mode.get("width", 0), mode.get("height", 0)
            if turn.replace("flipped-", "") in ("90", "270"):
                w, h = h, w
            out.append({"name": o.get("name"), "x": pos.get("x", 0), "y": pos.get("y", 0),
                        "width": int(round(w / scale)), "height": int(round(h / scale)), "scale": scale,
                        "mode": (mode.get("width", 0), mode.get("height", 0)),
                        "refresh": mode.get("refresh", 0), "transform": turn, "active": on})
        return sorted(out, key=lambda o: (not o["active"], o["x"], o["y"], o["name"] or ""))

    def output(self, name, mode=None, pos=None, scale=None, hz=60, transform=None, enable=None):
        args = ["--output", name]
        if enable is not None:
            args.append("--on" if enable else "--off")
        if mode:
            args += ["--custom-mode", "%dx%d@%gHz" % (mode[0], mode[1], hz)]
        if pos:
            args += ["--pos", "%d,%d" % pos]
        if scale:
            args += ["--scale", "%g" % scale]
        if transform:
            args += ["--transform", transform]
        if len(args) == 2:
            return True
        p = self._randr(args)
        return bool(p) and p.returncode == 0

    def add_output(self, mode=None, pos=None, scale=1, hz=60):
        """an output "appears": a disabled one is enabled -> its name, None when all are in use"""
        spare = [o["name"] for o in self.outputs(every=True) if not o["active"]]
        if not spare or not self.output(spare[0], enable=True, mode=mode, pos=pos, scale=scale, hz=hz):
            return None
        return spare[0]

    def remove_output(self, name):
        return self.output(name, enable=False)

    def layers(self, name):
        return None                         # (nothing lists them: the daemon's side of the wire does)

    # ---------------------------------- windows and power: the protocols, WlDesk
    def _desk(self):
        if self.desk is None and self.display:
            try:
                self.desk = WlDesk(self.display, 5 * self.patience)
            except (OSError, struct.error):
                self.desk = False
        return self.desk or None

    def _ask(self, fn, default):
        """a question to the window list; a connection that broke is made anew once"""
        for _ in range(2):
            d = self._desk()
            if not d:
                return default
            try:
                return fn(d)
            except (OSError, struct.error):
                d.close()
                self.desk = None
        return default

    def windows(self):
        return self._ask(lambda d: d.windows(), [])

    def win_fullscreen(self, win, on=True, output=None):
        return self._ask(lambda d: d.fullscreen(win["id"], on, output), False)

    def win_focus(self, win, output=None):
        return self._ask(lambda d: d.focus(win["id"]), False)

    def focus_output(self, name):
        return False

    def pointer_follows_focus(self, on):
        return True

    def power(self, name, on):
        """an output off or on (wlr-output-power-management); a headless output that was
        off shows nothing new until it is committed once more, as with sway"""
        if not self._ask(lambda d: d.power(name, on), False):
            return False
        scale = [o["scale"] for o in self.outputs() if o["name"] == name] if on else []
        t0 = time.time()
        while scale and time.time() - t0 < 20 * self.patience:
            time.sleep(0.2)
            self.output(name, scale=scale[0])
            if self.shot(name, timeout=2.0):
                break
        return True

    def can_point(self):
        if self.ptr is None and self.display:
            try:
                self.ptr = WlPointer(self.display, 5 * self.patience)
                self.pointer_how = "a virtual pointer device"
            except (OSError, struct.error):
                self.ptr = False
        return bool(self.ptr)


def make(kind, tmp, patience=1.0):
    if kind in COMPOSITORS:
        return WlrDisplay(kind, tmp, patience)
    return (SwayDisplay if kind == "sway" else XvfbDisplay)(tmp, patience)


# ---------------------------------------------------------------- command line
def _bounds(mons):
    return max(x + w for _, w, h, x, y in mons), max(y + h for _, w, h, x, y in mons)


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ("needs", "run", "serve"):
        sys.stderr.write(__doc__ + "\n")
        return 2
    what, args = args[0], args[1:]
    kind, mons, ready, command, build = None, [("mon0", 1280, 720, 0, 0)], None, [], None
    while args:
        a = args.pop(0)
        if a == "--display" and args:
            kind = args.pop(0)
        elif a == "--outputs" and args:
            mons = parse_outputs(args.pop(0))
        elif a == "--ready" and args:
            ready = args.pop(0)
        elif a == "--build" and args and what == "needs":
            build = args.pop(0)
        elif a == "--":
            command, args = args, []
        else:
            mons = None
            break
    kind = pick(kind)
    if not kind or not mons or (what == "run" and not command) or (what == "serve" and not ready):
        sys.stderr.write(__doc__ + "\n")
        return 2
    lack = missing(kind)
    if what == "needs":
        why = unusable(kind, build) if build else (("needs " + ", ".join(lack), SKIP) if lack else None)
        if why:
            print(why[0])
        return why[1] if why else 0
    if lack:
        print("displayenv: skipped (needs %s)" % ", ".join(lack))
        return SKIP

    def stop(signum, _frame):
        # serve: being told to stop is how it ends. run: the command did not finish, and
        # whoever started this must not read that as the command's success
        raise SystemExit(0 if what == "serve" else 128 + signum)
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, stop)
    try:
        patience = max(1.0, float(os.environ.get("RESTORECHECK_PATIENCE", "1") or 1))
    except ValueError:
        sys.stderr.write("displayenv: RESTORECHECK_PATIENCE is not a number\n")
        return 2
    tmp = tempfile.mkdtemp(prefix="walpe-display-")
    d = make(kind, tmp, patience)
    try:
        if not d.start(*_bounds(mons)):
            print("displayenv: the %s display did not start" % kind)
            return SKIP
        mons = d.place(mons)
        env = dict(os.environ)
        d.client_env(env)
        if d.fake_monitors and len(mons) > 1:
            env["WALPE_MONITORS"] = ",".join("%s=%dx%d+%d+%d" % m for m in mons)
        names = [k for k in ("DISPLAY", "WAYLAND_DISPLAY", "WALPE_BACKEND", "WALPE_MONITORS") if k in env]
        if what == "run":
            # (no time limit of its own: like xvfb-run, it lasts as long as the command;
            # a command ended by a signal is 128 + the signal, as a shell reports it)
            status = subprocess.run(command, env=env).returncode
            return status if status >= 0 else 128 - status
        with open(ready + ".part", "w") as f:
            f.write("".join("%s=%s\n" % (k, env[k]) for k in names))
        os.replace(ready + ".part", ready)
        while d.proc.poll() is None:
            time.sleep(0.2)
        return 1
    finally:
        for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(s, signal.SIG_IGN)    # (a second signal must not cut the clean-up short)
        d.stop()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
