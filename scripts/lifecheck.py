#!/usr/bin/env python3
"""End-to-end checks of the daemon's lifecycle: loads, reloads, restarts.

  python3 -I scripts/lifecheck.py --build build [--daemon FILE] [--display KIND] [--only a,b] [--keep] [--tmp DIR]

Uses the harness scripts/checkenv.py (private Xvfb, private daemon, pixel
reading): generated scenes only, nothing of the user's is read or written. Each case starts its own daemon:

  pointer   a wallpaper that uses the pointer gets it after every load (the
            main thread hears of a finished load only when the new
            wallpaper's needs are in place). The daemon runs on one CPU
            here, which is where the old order failed most of the time.
  reload    a setting that reloads scenes (quality preset, effect shader
            switches) loads each scene once, leaves the wallpapers it does
            not concern alone (a picture is not loaded again), and reaches
            a scene that is still being loaded; preset Maximum reloads for
            "Downscale oversized textures" also when the texture slack was
            0 already. The scene feature switches that are read at load
            (script_texanim, sprite_sheets): one load when the value
            changes, none when it does not.
  restart   a restart in place (the monitor layout changed) keeps the
            control socket there the whole time, keeps the process id,
            leaves no child process of the old image behind, starts from
            the start environment, and a second daemon is refused after it;
            "Render on CPU" switched on and off again at once does not
            restart; a SIGTERM that arrives while the daemon restarts ends
            it instead of being lost.
  stats     `walpe-ctl quality` gets the cost numbers of a scene that stands
            still, also when asked long after its load.
  layout    the monitor layout changes while the daemon starts - after it
            read the layout, before it listens for changes, so no event
            tells it: it looks once more when it is up and follows.
  busy      a setting that restarts the daemon (web hardware acceleration)
            switched while a Workshop download runs: the restart waits for
            the download instead of ending it (a stand-in steamcmd that
            only sleeps is put in front of PATH). Skipped in a build
            without the web module.
  probe     the daemon ends (quit, and killed) while its WebKit start-up
            test runs in a child process: the child ends with it instead of
            staying behind as a second "walpe-daemon". Skipped in a build
            without the web module.
  fullscreen  "Pause for fullscreen apps" per monitor (X11): two monitors,
            a window that says it is fullscreen and has the focus - the
            check plays the window manager and sets the EWMH properties
            itself. The monitor the window is on pauses and the other one
            keeps drawing: with a border around the window (tiling window
            managers), inside a frame window (reparenting ones), moved to
            the other monitor, across both; nothing pauses while the focus
            is on another window; `set fs_all=on` pauses every monitor as
            earlier versions did; status says all of it per monitor. Then
            again with shared rendering (one monitor following the other's
            frames): each monitor still pauses on its own.
  video     a video's load ends with its first picture: a clip the decoder
            cannot do anything with (its container reads well) is a failed
            load with a reason in status, and the wallpaper from before
            comes back; a clip that plays is not held for one, on a paused
            monitor either. Needs an ffmpeg program that writes H.264
            (the clip is made here); skipped without one.

The display is Xvfb, or with --display sway (or WALPE_CHECK_DISPLAY=sway) a
headless Wayland compositor (scripts/displayenv.py); the cases are the same
on both, except `restart`, which changes the layout the way only the X11
backend is given one (WALPE_MONITORS=@FILE and an X event) and is skipped
under Wayland: outputs that change there are scripts/waylandcheck.py's.

Exit code 0 = all passed, 1 = failures, 77 = skipped (the display's programs
are not installed, or the build has no backend for it). --daemon FILE
runs another walpe-daemon binary with the tools of --build (a build without
the fixes: the cases fail there). LIFECHECK_PATIENCE=N multiplies every
wait (a software renderer on a busy machine: 3 or more). --tmp DIR: work in
DIR (must not exist; removed afterwards)."""
import json, os, re, shutil, signal, struct, subprocess, sys, threading, time

sys.dont_write_bytecode = True   # no __pycache__ next to the scripts
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("RESTORECHECK_PATIENCE", os.environ.get("LIFECHECK_PATIENCE", "1") or "1")
import checkenv as rc   # noqa: E402  (Env, the scene generators, the pixel reader)
import displayenv       # noqa: E402  (which display, and whether it can be used here)

PATIENCE = rc.PATIENCE
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


def put(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


VERT = ("uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
        "varying vec2 v_TexCoord;\nvoid main() {\n"
        "\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n\tv_TexCoord = a_TexCoord;\n}\n")
# red where the pointer is on the right, blue on the left, purple in the middle (or without the pointer)
POINTER_FRAG = ("varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform vec2 g_PointerPosition;\n"
                "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
                "\tc.rgb = vec3(g_PointerPosition.x, 0.0, 1.0 - g_PointerPosition.x);\n\tgl_FragColor = c;\n}\n")
# changes all the time
TIME_FRAG = ("varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
             "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
             "\tc.rgb = vec3(0.5 + 0.5 * sin(g_Time * 5.0), 0.2, 0.2);\n\tgl_FragColor = c;\n}\n")


def gen_fx_scene(out, frag, name="fx"):
    """one full-screen solid layer with a one-pass effect of its own"""
    rc.jdump(f"{out}/models/util/solidlayer.json", {"solidlayer": True})
    put(f"{out}/shaders/effects/{name}.vert", VERT)
    put(f"{out}/shaders/effects/{name}.frag", frag)
    rc.jdump(f"{out}/materials/effects/{name}.json",
             {"passes": [{"shader": f"effects/{name}", "blending": "normal", "depthtest": "disabled",
                          "depthwrite": "disabled", "cullmode": "nocull"}]})
    rc.jdump(f"{out}/effects/{name}/effect.json",
             {"name": name, "group": "t", "passes": [{"material": f"materials/effects/{name}.json"}]})
    rc.jdump(f"{out}/scene.json", {
        "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
        "general": {"ambientcolor": "0.3 0.3 0.3", "bloom": False, "cameraparallax": False,
                    "clearcolor": "0 0 0", "orthogonalprojection": {"height": 1080, "width": 1920},
                    "skylightcolor": "0.3 0.3 0.3"},
        "objects": [{"id": 40, "name": "fill", "image": "models/util/solidlayer.json",
                     "origin": "960 540 0", "size": "1920 1080", "scale": "1 1 1", "angles": "0 0 0",
                     "color": "1 1 1", "alpha": 1.0, "visible": True,
                     "effects": [{"file": f"effects/{name}/effect.json", "id": 9, "name": name,
                                  "visible": True, "passes": [{}]}]}]})
    rc.jdump(f"{out}/project.json", {"file": "scene.json", "title": os.path.basename(out), "type": "scene"})


def gen_bmp(path, rgb, w, h):
    """a picture wallpaper of one colour (24-bit BMP: no encoder needed)"""
    row = bytes((rgb[2], rgb[1], rgb[0])) * w
    row += b"\0" * ((4 - len(row) % 4) % 4)
    size = 54 + len(row) * h
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", size, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(row) * h, 2835, 2835, 0, 0))
        f.write(row * h)


def gen_big_scene(out, w=1280, h=720):
    """one picture layer whose texture (w x h, raw RGBA, no mip levels) is
    exactly twice the size of test monitor A: stored at half size with
    "Downscale oversized textures", in full without - the log's `scene
    summary:` line tells which"""
    px = bytes((40, 160, 60, 255)) * (w * h)
    tex = (b"TEXV0005\0" + b"TEXI0001\0" + struct.pack("<IIIIIII", 0, 0, w, h, w, h, 0) +
           b"TEXB0003\0" + struct.pack("<II", 1, 0xFFFFFFFF) + struct.pack("<I", 1) +
           struct.pack("<IIIII", w, h, 0, len(px), len(px)) + px)
    os.makedirs(f"{out}/materials", exist_ok=True)
    with open(f"{out}/materials/big.tex", "wb") as f:
        f.write(tex)
    rc.jdump(f"{out}/materials/big.json",
             {"passes": [{"blending": "translucent", "cullmode": "nocull", "depthtest": "disabled",
                          "depthwrite": "disabled", "shader": "genericimage2", "textures": ["big"]}]})
    rc.jdump(f"{out}/models/big.json", {"autosize": True, "material": "materials/big.json"})
    rc.jdump(f"{out}/scene.json", {
        "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
        "general": {"ambientcolor": "0.3 0.3 0.3", "bloom": False, "cameraparallax": False,
                    "clearcolor": "0 0 0", "orthogonalprojection": {"height": h, "width": w},
                    "skylightcolor": "0.3 0.3 0.3"},
        "objects": [{"id": 7, "name": "big", "image": "models/big.json", "origin": f"{w // 2} {h // 2} 0",
                     "size": f"{w} {h}", "scale": "1 1 1", "angles": "0 0 0", "color": "1 1 1",
                     "alpha": 1.0, "visible": True}]})
    rc.jdump(f"{out}/project.json", {"file": "scene.json", "title": "big", "type": "scene"})


class Env(rc.Env):
    """the harness's environment, plus: a daemon on chosen CPUs, extra
    environment for one start, the log from a mark on"""

    def start_daemon(self, mons, cpus=None, extra=None, hz=None):
        hz = hz or [60] * len(mons)
        self.mons = mons = self.display.place(mons, hz)
        env = dict(self.env)
        if self.display.fake_monitors:
            env["WALPE_MONITORS"] = ",".join(f"{n}={w}x{h}+{x}+{y}@{z}" for (n, w, h, x, y), z in zip(mons, hz))
        env.update(extra or {})
        self.run_env = env
        log = open(self.log, "a")
        pre = (lambda: os.sched_setaffinity(0, cpus)) if cpus else None
        self.daemon = subprocess.Popen([self.daemon_bin], env=env, stdout=log, stderr=subprocess.STDOUT,
                                       cwd=self.tmp, preexec_fn=pre)
        log.close()
        for _ in range(int(150 * PATIENCE)):
            time.sleep(0.1)
            if self.daemon.poll() is not None:
                return False
            if os.path.exists(self.sock) and self.ipc({"cmd": "status"}):
                self.mons = self.display.seen(mons, self.monitors)   # the daemon's order and names
                return True
        return False

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

    def state(self):
        rep = self.ipc({"cmd": "status"})
        return (rep or {}).get("state") or {}


def wait_for(fn, timeout=10.0, step=0.2):
    return rc.wait_for(fn, timeout, step)


def loaded(env, path, mon=None, timeout=30):
    """wait until the monitor(s) show path (status: current, a type, not "loading" any
    more, no load_error). "current" is the wallpaper a monitor was given and "type" what
    it shows until that is loaded: after a scene, the two alone are true of the next
    scene from the moment it is opened."""
    def ok():
        ms = env.monitors()
        sel = ms if mon is None else ms[mon:mon + 1]
        return bool(sel) and all(m.get("current") == path and m.get("type") not in (None, "none") and
                                 not m.get("loading") and not m.get("load_error") for m in sel)
    return bool(wait_for(ok, timeout, 0.2))


def colour_is(env, mon, pred, timeout=6.0):
    box = [None]

    def ok():
        box[0] = env.colour(mon)
        return pred(box[0])
    wait_for(ok, timeout, 0.1)
    return box[0]


# ------------------------------------------------------------------ pointer
def case_pointer(env, W):
    """After every load of a scene whose picture follows the pointer the
    daemon tracks the pointer. The pointer sits at the left edge the whole
    time: tracked = blue, not tracked = purple (the shader's centre value).
    Under Wayland a client is told of a pointer only when the compositor
    gives one of its surfaces the pointer focus, and a compositor does that
    when the pointer moves (sway 1.9: not when the surface under a resting
    pointer begins to take input). There the pointer is moved one step
    aside and back after each load; what is checked is the same: the new
    wallpaper's surface takes the pointer."""
    if not env.display.can_point():
        print("skip pointer (needs xdotool under X, a virtual pointer device under Wayland)")
        return
    mon = ("A", 640, 360, 0, 0)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False,
                          "parallax": True})
    cpus = {sorted(os.sched_getaffinity(0))[0]}
    if not check(env.start_daemon([mon], cpus=cpus), "pointer: daemon started (on one CPU)"):
        return
    # the monitor as the display knows it: ("A", ...) itself on X, where the name is this
    # check's to give; under Wayland the compositor's output name, by which a picture of
    # it is asked for (a screenshot of an output called "A" does not exist there)
    mon = env.mons[0]
    try:
        # (a client is connected now: the server keeps the position; the tool runs in the
        # check's own environment, as everything else the check starts)
        env.display.pointer(4, 100, env=env.env)
        tracked = []
        rounds = 8
        for i in range(rounds):
            env.open(W["red"])
            loaded(env, W["red"])
            colour_is(env, mon, lambda c: rc.name_of(c) == "red")
            env.open(W["ptr"])
            loaded(env, W["ptr"])

            def blue(c):
                if rc.name_of(c) != "blue" and env.display.winsys == "wayland":
                    env.display.pointer(5, 100, env=env.env)
                    env.display.pointer(4, 100, env=env.env)
                return rc.name_of(c) == "blue"
            c = colour_is(env, mon, blue, 4.0)
            tracked.append(rc.name_of(c))
        check(tracked == ["blue"] * rounds,
              f"pointer: the scene follows the pointer after each of {rounds} loads", tracked)
    finally:
        env.stop_daemon()


# ------------------------------------------------------------------- reload
SUMMARY = re.compile(r"scene summary: .*layer images (\d+) = ([0-9.]+)")


def loads_since(env, mark):
    """(scene loads, MiB of layer pictures of the last one, picture loads) in the log since mark"""
    txt = env.since(mark)
    sc = SUMMARY.findall(txt)
    return len(sc), (float(sc[-1][1]) if sc else None), txt.count("image: ")


def quiet(env, mark, want_scenes=1, timeout=30):
    """wait for the loads a setting caused, then a little longer for one too many"""
    wait_for(lambda: loads_since(env, mark)[0] >= want_scenes and
             not any(m.get("loading") for m in env.monitors()), timeout, 0.2)
    time.sleep(2.0 * PATIENCE)
    wait_for(lambda: not any(m.get("loading") for m in env.monitors()), timeout, 0.2)
    return loads_since(env, mark)


def case_reload(env, W):
    """Monitor A shows a scene with an oversized layer picture, monitor B (another
    size: no sharing) a picture wallpaper larger than B (each of its loads logs
    one "image:" line)."""
    A, B = ("A", 640, 360, 0, 0), ("B", 480, 270, 640, 0)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    if not check(env.start_daemon([A, B]), "reload: daemon started"):
        return
    FULL = 1280 * 720 * 4 / 1048576.0   # gen_big_scene's layer picture, MiB

    def setv(*kv):
        rcode, rep = env.ctl("set", *kv)
        return rcode == 0 and bool(rep and rep.get("ok"))

    env.open(W["pic"])       # (the first wallpaper is every monitor's)
    ok = loaded(env, W["pic"])
    mark = env.mark()
    env.open(W["big"], 0)
    ok = ok and loaded(env, W["big"], 0)
    wait_for(lambda: loads_since(env, mark)[0] >= 1, 20)   # (the summary comes with the first frame)
    time.sleep(1.0 * PATIENCE)
    ms = env.monitors()
    check(ok and ms[0].get("type") == "scene" and ms[1].get("type") == "image",
          "reload: scene on monitor 0, picture on monitor 1", rc.brief(ms))
    n, mib, _ = loads_since(env, mark)
    check(n == 1 and mib is not None and mib < FULL / 2,
          "reload: preset High stores the oversized layer picture reduced", (n, mib))

    # a preset that changes the effect shader switches only
    mark = env.mark()
    check(setv("preset=balanced"), "reload: set preset=balanced")
    n, mib, pics = quiet(env, mark)
    check(n == 1, "reload: Balanced loads the scene once", n)
    check(pics == 0, "reload: ... and leaves the picture wallpaper alone", pics)

    # a preset that changes texture slack, downscale and the effect shader switches
    mark = env.mark()
    check(setv("preset=max"), "reload: set preset=max")
    n, mib, pics = quiet(env, mark)
    check(n == 1, "reload: Maximum (texture slack, downscale and shaders change) loads the scene once", n)
    check(mib is not None and mib > FULL * 0.9, "reload: Maximum keeps the layer picture in full", mib)
    check(pics == 0, "reload: ... and leaves the picture wallpaper alone", pics)

    mark = env.mark()
    check(setv("preset=high"), "reload: set preset=high")
    n, mib, pics = quiet(env, mark)
    check(n == 1 and mib is not None and mib < FULL / 2, "reload: High again: one load, reduced", (n, mib))

    # Maximum when the texture slack is 0 already: only "Downscale oversized textures" changes
    mark = env.mark()
    check(setv("texture_slack=0"), "reload: set texture_slack=0")
    n, mib, pics = quiet(env, mark)
    check(n == 1 and mib is not None and mib < FULL / 2, "reload: slack 0: one load, still reduced", (n, mib))
    mark = env.mark()
    check(setv("preset=max"), "reload: set preset=max with the slack at 0 already")
    n, mib, pics = quiet(env, mark)
    check(n == 1 and mib is not None and mib > FULL * 0.9,
          "reload: Maximum applies at once (the scene is loaded again, layer picture in full)", (n, mib))

    # scene feature switches that are read when a scene loads
    # (docs/SCENE-FEATURES.md): switching one loads the scene once; the
    # value it has already, a switch scenes take up with their next frame
    # and a setting that concerns no scene load nothing
    for sw in ("script_texanim", "sprite_sheets"):
        mark = env.mark()
        check(setv(sw + "=off"), "reload: set %s=off" % sw)
        n, mib, pics = quiet(env, mark)
        check(n == 1 and pics == 0, "reload: %s=off loads the scene once, not the picture" % sw, (n, pics))
        mark = env.mark()
        check(setv(sw + "=off"), "reload: set %s=off once more" % sw)
        n, mib, pics = quiet(env, mark, 0, 5)
        check(n == 0 and pics == 0, "reload: ... which loads nothing", (n, pics))
    mark = env.mark()
    check(setv("script_texanim=on", "sprite_sheets=on", "bloom=off"),
          "reload: set script_texanim=on sprite_sheets=on bloom=off")
    n, mib, pics = quiet(env, mark)
    check(n == 1 and pics == 0, "reload: two such switches in one request: one load", (n, pics))
    mark = env.mark()
    check(setv("bloom=on", "camera_shake=off", "fps=30"), "reload: set bloom=on camera_shake=off fps=30")
    n, mib, pics = quiet(env, mark, 0, 5)
    check(n == 0 and pics == 0, "reload: next-frame switches and another setting load nothing", (n, pics))
    setv("camera_shake=on")

    env.stop_daemon()

    # a setting that arrives while the scene is still loading (each load
    # takes 1.5 s here): the monitor shows a picture, is given the scene,
    # and Maximum is picked half a second into that load
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    if not check(env.start_daemon([A, B], extra={"WALPE_LOAD_DELAY_MS": "1500"}),
                 "reload: daemon started (slow loads)"):
        return
    env.open(W["pic"], 0)
    loaded(env, W["pic"], 0)
    time.sleep(0.5)
    mark = env.mark()
    env.ipc({"cmd": "open", "path": W["big"], "monitor": 0})
    time.sleep(0.5)
    ms = env.monitors()
    check(bool(ms) and ms[0].get("loading") is True and ms[0].get("type") == "image",
          "reload: status says the monitor is loading (and still shows the picture)", rc.brief(ms) + [
              {"loading": m.get("loading")} for m in ms])
    st = env.ipc({"cmd": "set", "preset": "max"})
    n, mib, pics = quiet(env, mark, 2)
    check(bool(st and st.get("ok")) and mib is not None and mib > FULL * 0.9,
          "reload: a preset picked while the scene is loading reaches that scene", (n, mib))
    ms = env.monitors()
    check(bool(ms) and not any(m.get("loading") for m in ms) and ms[0].get("type") == "scene",
          "reload: status: loaded", rc.brief(ms))
    env.stop_daemon()


# ------------------------------------------------------------------ restart
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
    ev.xconfigure.width = ev.xconfigure.height = 1
    ok = x.XSendEvent(d, root, 0, 1 << 17, ctypes.byref(ev))   # StructureNotifyMask
    x.XFlush(d)
    x.XCloseDisplay(d)
    return bool(ok)


def children(pid):
    out = []
    for n in os.listdir("/proc"):
        if not n.isdigit():
            continue
        try:
            with open(f"/proc/{n}/stat") as f:
                st = f.read()
            if int(st[st.rindex(")") + 1:].split()[1]) == pid:
                out.append(int(n))
        except (OSError, ValueError, IndexError):
            pass
    return out


def can_connect(sock):
    """is the control socket there? False = no socket file, or nobody listens
    on it (what a client takes for "no daemon runs"). A listener that is
    busy (its queue is full while the daemon restarts) counts as there."""
    import socket
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2.0)
    try:
        s.connect(sock)
        return True
    except (FileNotFoundError, ConnectionRefusedError):
        return False
    except OSError:
        return True
    finally:
        s.close()


OURS = ("WALPE_WEB_ENV_OURS", "WALPE_GL_HINTS_SET", "WEBKIT_DISABLE_COMPOSITING_MODE",
        "WEBKIT_DISABLE_DMABUF_RENDERER", "GDK_DISABLE", "GSK_RENDERER", "LP_NUM_THREADS",
        "__EGL_VENDOR_LIBRARY_FILENAMES", "LIBGL_ALWAYS_SOFTWARE", "GALLIUM_DRIVER")


def env_names(pid):
    """names (never values) of the variables the process image was started with"""
    try:
        with open(f"/proc/{pid}/environ", "rb") as f:
            return {e.split(b"=", 1)[0].decode(errors="replace") for e in f.read().split(b"\0") if e}
    except OSError:
        return set()


def case_restart(env, W):
    """The monitor layout changes under a running daemon: it restarts in place."""
    A, B = ("A", 640, 360, 0, 0), ("B", 640, 360, 640, 0)
    mf = os.path.join(env.tmp, "monitors")
    if env.display.winsys != "x11":
        print("skip restart (the layout is changed through WALPE_MONITORS=@FILE and an X event here: "
              "X11 only; scripts/waylandcheck.py changes real outputs)")
        return
    if not poke_root(env.env["DISPLAY"]):
        print("skip restart (no libX11 for ctypes: cannot announce a layout change)")
        return

    def layout(bw):
        with open(mf, "w") as f:
            f.write(f"A=640x360+0+0@60,B={bw}x360+640+0@60")

    def restart_by_layout(bw, timeout=40):
        """change the layout and wait for the restart line; -> (seen, connect failures)"""
        mark = env.mark()
        layout(bw)
        poke_root(env.env["DISPLAY"])
        t0, bad = time.time(), 0
        while time.time() - t0 < timeout * PATIENCE:
            if "restarting to follow it" in env.since(mark):
                return True, bad
            bad += 0 if can_connect(env.sock) else 1
            time.sleep(0.01)
        return False, bad

    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    layout(640)
    if not check(env.start_daemon([A, B], extra={"WALPE_MONITORS": "@" + mf}), "restart: daemon started"):
        return
    pid = env.daemon.pid
    start_names = env_names(pid)
    env.open(W["red"])
    loaded(env, W["red"])
    web = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tests", "fixtures",
                       "web-audio")
    have_web = os.path.exists(os.path.join(env.build, "walpe-web.so")) and os.path.isdir(web)
    if have_web:
        # (the page's processes come up after the daemon has tried WebKit in a
        # child of its own, seconds after status says "web": wait for the page)
        wmark = env.mark()
        env.open(web, 1)
        have_web = bool(wait_for(lambda: "web: loaded " in env.since(wmark), 60, 0.5))
        time.sleep(1.0 * PATIENCE)
    kids = children(pid)

    seen, bad = restart_by_layout(600)
    check(seen, "restart: the daemon restarts for the new layout")
    # the new image answers; until then the socket must stay connectable
    t0, up = time.time(), False
    while time.time() - t0 < 40 * PATIENCE and not up:
        bad += 0 if can_connect(env.sock) else 1
        st = env.ipc({"cmd": "status"}, timeout=1.0)
        ms = ((st or {}).get("state") or {}).get("monitors") or []
        up = len(ms) == 2 and ms[1].get("geometry", [0, 0, 0, 0])[2] == 600
        time.sleep(0.01)
    check(up, "restart: the new image answers with the new layout")
    check(bad == 0, "restart: the control socket stayed connectable the whole time", f"{bad} failed connects")
    check(env.daemon.poll() is None and os.path.exists(f"/proc/{pid}"), "restart: same process id")
    if have_web and kids:
        left = wait_for(lambda: not [k for k in kids if k in children(pid)], 10, 0.5)
        check(bool(left), "restart: the child processes of the old image are gone",
              [k for k in kids if k in children(pid)])
    else:
        print("skip restart: child processes (no web wallpaper came up here)")
    names = env_names(pid)
    extra = sorted(n for n in names - start_names if n not in ("WALPE_LAYOUT_RESTARTED", "WALPE_IPC_FD"))
    check(not extra and not [n for n in OURS if n in names and n not in start_names],
          "restart: the new image has the start environment (nothing the old image set for itself)", extra)
    # the single-instance lock is held by the new image
    p = subprocess.run([env.daemon_bin], env=env.run_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=60 * PATIENCE)
    check(p.returncode != 0 and b"already running" in p.stdout,
          "restart: a second daemon is refused after the restart", p.stdout[-300:])
    ms = env.monitors()
    check(wait_for(lambda: all(m.get("current") and m.get("type") not in (None, "none")
                               for m in env.monitors() or [{}]), 40, 0.5),
          "restart: the wallpapers are back", rc.brief(env.monitors()))

    # "Render on CPU" switched on and off again before its restart came due
    # (200 ms after the request): what runs is what is wanted, no restart
    if "WALPE_SOFTWARE" not in env.run_env:
        mark = env.mark()
        reps = env.ipc_burst([{"cmd": "set", "software_render": True}, {"cmd": "set", "software_render": False}])
        time.sleep(2.0 * PATIENCE)
        check(len(reps) == 2 and reps[0].get("restarting") is True and not reps[1].get("restarting"),
              "restart: Render on CPU on, then off at once: the first asks for a restart, the second not",
              reps)
        check("restarting in place" not in env.since(mark) and bool(env.state()),
              "restart: ... and the daemon does not restart for it")
    env.stop_daemon()

    # told to stop while it restarts: it stops. (Loads take 1.5 s here and one
    # is always under way, so the old image is still joining its render
    # thread when the signal arrives.)
    layout(640)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    if not check(env.start_daemon([A, B], extra={"WALPE_MONITORS": "@" + mf, "WALPE_LOAD_DELAY_MS": "1500"}),
                 "restart: daemon started (slow loads)"):
        return
    pid = env.daemon.pid
    import threading
    stop = threading.Event()

    def keep_loading():
        i = 0
        while not stop.is_set():
            i += 1
            env.ipc({"cmd": "open", "path": W["red"] if i % 2 else W["blue"]}, timeout=1.0)
            stop.wait(1.0)

    th = threading.Thread(target=keep_loading, daemon=True)
    th.start()
    mark = env.mark()
    layout(600)
    poke_root(env.env["DISPLAY"])
    t0, seen = time.time(), False
    while time.time() - t0 < 40 * PATIENCE and not seen:
        seen = "restarting to follow it" in env.since(mark)
        time.sleep(0.002)
    stop.set()
    try:
        os.kill(pid, signal.SIGTERM)   # at once: the old image is taking itself apart
    except OSError:
        pass
    try:
        env.daemon.wait(timeout=20 * PATIENCE)
        gone = True
    except subprocess.TimeoutExpired:
        gone = False
    check(seen and gone, "restart: SIGTERM during the restart ends the daemon", (seen, gone))
    time.sleep(0.5)
    check(not env.ipc({"cmd": "status"}, timeout=2.0), "restart: no daemon answers afterwards")
    if gone:
        env.daemon = None
    env.stop_daemon()


# -------------------------------------------------------------------- stats
def case_stats(env, W):
    """`walpe-ctl quality` gets the cost numbers of a scene that stands still,
    also when it is asked long after the load (no frame comes by then)."""
    A = ("A", 640, 360, 0, 0)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    if not check(env.start_daemon([A]), "stats: daemon started"):
        return
    env.open(W["red"])
    loaded(env, W["red"])
    time.sleep(5.0)     # past the first frames and the memory hand-back that follows a load

    def has_stats():
        q = env.ipc({"cmd": "quality"})
        m = ((q or {}).get("quality") or {}).get("monitors") or [{}]
        return "stats" in m[0]
    has_stats()         # the first answer is given before the numbers can be there
    check(bool(wait_for(has_stats, 6, 0.5)), "stats: a still scene's numbers arrive after the first request")
    env.stop_daemon()


# -------------------------------------------------------------------- video
def gen_clip(path):
    """one second of ffmpeg's test picture as 320x180 H.264 in MP4; False when this machine cannot make it"""
    if not shutil.which("ffmpeg"):
        return False
    try:
        p = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=320x180:rate=30",
                            "-t", "1", "-c:v", "libx264", "-pix_fmt", "yuv420p", path], stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120 * PATIENCE)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return p.returncode == 0 and os.path.exists(path) and os.path.getsize(path) > 1000


def break_clip(src, dst):
    """src with every byte of its media data replaced: the container (sizes,
    times, the sample table) reads as before, the stream is noise"""
    d = bytearray(open(src, "rb").read())
    off, hit, seed = 0, False, 7
    while off + 8 <= len(d):
        size, kind = struct.unpack(">I4s", d[off:off + 8])
        hdr = 8
        if size == 1 and off + 16 <= len(d):
            size, hdr = struct.unpack(">Q", d[off + 8:off + 16])[0], 16
        if size == 0:
            size = len(d) - off
        if size < hdr:
            break
        if kind == b"mdat":
            for i in range(off + hdr, min(off + size, len(d))):
                seed = (seed * 1103515245 + 12345) & 0x7fffffff
                d[i] = (seed >> 16) & 0xff
            hit = True
        off += size
    with open(dst, "wb") as f:
        f.write(d)
    return hit


def case_video(env, W):
    """A video is loaded when it has shown a picture; one that never gives
    any has failed, and the wallpaper before it returns."""
    wait_s = 6
    if not gen_clip(W["clip"]) or not break_clip(W["clip"], W["badclip"]):
        print("skip  video: no ffmpeg program here that writes an H.264 clip", flush=True)
        return
    A = ("A", 640, 360, 0, 0)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    if not check(env.start_daemon([A], extra={"WALPE_FIRST_FRAME_S": str(wait_s)}), "video: daemon started"):
        return

    def mon():
        return (env.monitors() or [{}])[0]

    def over(path, kind="video"):
        m = mon()
        return m.get("current") == path and m.get("type") == kind and not m.get("loading")
    env.open(W["red"])
    loaded(env, W["red"])
    env.open(W["clip"])
    wait_for(lambda: over(W["clip"]) or mon().get("load_error"), wait_s + 20 * PATIENCE, 0.2)
    m = mon()
    if m.get("load_error") and "no picture" not in m["load_error"]:
        print("skip  video: this build does not play the clip (" + m["load_error"][-80:] + ")", flush=True)
        env.stop_daemon()
        return
    check(m.get("type") == "video" and m.get("current") == W["clip"] and not m.get("load_error") and
          not m.get("loading"), "video: a clip that plays is loaded", rc.brief([m]))

    # under a pause: the first picture comes there too, the wait must not end in a failure
    env.open(W["red"])
    loaded(env, W["red"])
    env.ctl("pause")
    env.open(W["clip"])
    wait_for(lambda: over(W["clip"]), wait_s + 20 * PATIENCE, 0.2)
    time.sleep(wait_s + 2)
    m = mon()
    check(m.get("type") == "video" and m.get("current") == W["clip"] and not m.get("load_error") and
          not m.get("loading"), "video: opened on a paused monitor it is loaded too (no failure after the wait)",
          rc.brief([m]))
    env.ctl("resume")

    # the clip whose stream is noise
    mark = env.mark()
    check(env.open(W["badclip"]), "video: open of the damaged clip is accepted (its container reads well)")
    seen = wait_for(lambda: mon().get("current") == W["badclip"] and mon().get("loading"), 3 * PATIENCE, 0.1)
    check(bool(seen), "video: status says \"loading\" while the clip has shown no picture", rc.brief([mon()]))
    wait_for(lambda: mon().get("load_error"), wait_s + 20 * PATIENCE, 0.2)
    wait_for(lambda: over(W["clip"]), 20 * PATIENCE, 0.2)
    m = mon()
    check("no picture" in (m.get("load_error") or ""), "video: a clip that gives no picture is a failed load "
          "(status load_error)", rc.brief([m]))
    check(m.get("current") == W["clip"] and m.get("load_restored") == W["clip"] and m.get("type") == "video",
          "video: the wallpaper from before is opened again", rc.brief([m]))
    check(env.saved()[0] == W["clip"], "video: the saved choice is the wallpaper that is shown", env.saved())
    check(env.since(mark).count("opening the previous wallpaper again") == 1, "video: one re-open")

    # another wallpaper while the video is waited for: the wait ends there, nothing fails later
    env.open(W["badclip"])
    wait_for(lambda: mon().get("current") == W["badclip"] and mon().get("loading"), 3 * PATIENCE, 0.1)
    env.open(W["red"])
    loaded(env, W["red"])
    time.sleep(wait_s + 2)
    m = mon()
    check(m.get("current") == W["red"] and m.get("type") == "scene" and not m.get("load_error") and
          not m.get("loading"), "video: another wallpaper opened during the wait: no failure follows",
          rc.brief([m]))
    check(env.daemon.poll() is None, "video: the daemon is running")
    env.stop_daemon()


# ------------------------------------------------------------------- layout
def case_layout(env, W):
    """A second monitor appears while the daemon starts. The daemon has read
    the layout by the time its socket file exists and asks for the X events
    of a change only later (after EGL, the windows, the first loads): the
    change is made in between and nothing announces it."""
    mf = os.path.join(env.tmp, "monitors-start")
    one, two = "A=640x360+0+0@60", "A=640x360+0+0@60,B=640x360+640+0@60"
    if env.display.winsys != "x11":
        print("skip layout (the layout is changed through WALPE_MONITORS=@FILE here: X11 only; "
              "scripts/waylandcheck.py changes real outputs)")
        return

    def layout(text):
        with open(mf + ".new", "w") as f:
            f.write(text)
        os.replace(mf + ".new", mf)

    layout(one)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    if os.path.exists(env.sock):
        os.unlink(env.sock)
    changed = []

    def change_when_read():
        t0 = time.time()
        while time.time() - t0 < 60 * PATIENCE and not changed:
            if os.path.exists(env.sock):
                layout(two)
                changed.append(True)
            else:
                time.sleep(0.002)

    th = threading.Thread(target=change_when_read, daemon=True)
    th.start()
    mark = env.mark()
    ok = env.start_daemon([("A", 640, 360, 0, 0)], extra={"WALPE_MONITORS": "@" + mf})
    th.join(timeout=60 * PATIENCE)
    if not check(ok and bool(changed), "layout: daemon started, the layout changed while it did"):
        return
    pid = env.daemon.pid
    env.open(W["red"])

    def followed():
        return "restarting to follow it" in env.since(mark) and len(env.monitors()) == 2
    got = bool(wait_for(followed, 12, 0.3))
    txt = env.since(mark)
    # (what the test rests on: this start read the layout before the change)
    check("monitor 1:" not in txt.split("restarting to follow it")[0], "layout: the daemon started on the old layout",
          txt[:300])
    check(got, "layout: the change made during start-up is followed without any event",
          rc.brief(env.monitors()))
    check(env.daemon.poll() is None and env.daemon.pid == pid, "layout: the same process went on")
    check(loaded(env, W["red"]), "layout: both monitors show the wallpaper", rc.brief(env.monitors()))
    env.stop_daemon()


# --------------------------------------------------------------------- busy
def case_busy(env, W):
    """Web hardware acceleration is switched while a Workshop download runs:
    the restart that applies it must not end the download."""
    A = ("A", 640, 360, 0, 0)
    bindir = os.path.join(env.tmp, "busy-bin")
    pidfile = os.path.join(env.tmp, "steamcmd.pid")
    os.makedirs(bindir, exist_ok=True)
    with open(os.path.join(bindir, "steamcmd"), "w") as f:
        f.write(f"#!/bin/sh\necho $$ > '{pidfile}'\nexec sleep 600\n")
    os.chmod(os.path.join(bindir, "steamcmd"), 0o755)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False,
                          "steam_user": "tester"})
    # WALPE_WEB_ACCEL=0: whatever the setting says, the restarted daemon does
    # not try WebKit's accelerated start-up (the switch restarts all the same)
    extra = {"PATH": bindir + os.pathsep + env.env.get("PATH", ""), "WALPE_WEB_ACCEL": "0"}
    if not check(env.start_daemon([A], extra=extra), "busy: daemon started"):
        return
    st = env.state()
    if "web_accel" not in st:
        print("skip busy (no web module in this build: no web_accel setting)")
        return
    pid = env.daemon.pid
    rep = env.ipc({"cmd": "install", "id": "123456789"})
    if not check(bool(rep and rep.get("ok")), "busy: the download started", rep):
        return
    check(bool(wait_for(lambda: env.state().get("install_active") and os.path.exists(pidfile) and
                        os.path.getsize(pidfile) > 0, 10)), "busy: the stand-in steamcmd runs")
    try:
        spid = int(open(pidfile).read())
    except (OSError, ValueError):
        spid = 0
    mark = env.mark()
    rep = env.ipc({"cmd": "set", "web_accel": not st.get("web_accel")})
    check(bool(rep and rep.get("ok")), "busy: web_accel switched", rep)
    time.sleep(3.0)     # the restart used to come 200 ms after that reply
    check("restarting to apply it" not in env.since(mark), "busy: no restart while the download runs",
          env.since(mark)[-400:])
    check(bool(env.state().get("install_active")), "busy: the download is still running")
    check(spid > 0 and os.path.exists(f"/proc/{spid}"), "busy: steamcmd was not ended")
    check("waits for the running Workshop download" in env.since(mark), "busy: the log says what the restart waits for")
    env.ipc({"cmd": "install_cancel"})

    def restarted():
        txt = env.since(mark)
        # (the line every start ends with: "... up (wm=..., backend=x11)" or "... up (<compositor>, ...)")
        return "restarting to apply it" in txt and '" up (' in txt and bool(env.state())
    check(bool(wait_for(restarted, 40, 0.3)), "busy: the restart comes once the download is over",
          env.since(mark)[-400:])
    check(env.daemon.poll() is None and env.daemon.pid == pid, "busy: the same process went on")
    check(env.state().get("web_accel") == (not st.get("web_accel")), "busy: the setting is the one asked for")
    check(not (spid > 0 and os.path.exists(f"/proc/{spid}")), "busy: no steamcmd left behind")
    env.stop_daemon()


# -------------------------------------------------------------------- probe
def running(pid):
    """is the process there and not a zombie waiting to be collected?"""
    try:
        with open(f"/proc/{pid}/stat") as f:
            st = f.read()
        return st[st.rindex(")") + 1:].split()[0] not in ("Z", "X")
    except (OSError, ValueError, IndexError):
        return False


def probe_children(pid):
    """the daemon's child processes that are its WebKit start-up test"""
    out = []
    for k in children(pid):
        try:
            with open(f"/proc/{k}/cmdline", "rb") as f:
                if b"--web-probe" in f.read().split(b"\0"):
                    out.append(k)
        except OSError:
            pass
    return out


def case_probe(env, W):
    """The first web wallpaper of a daemon starts WebKit in a child first
    (`walpe-daemon --web-probe`). A daemon that ends meanwhile must not leave
    that child behind."""
    A = ("A", 640, 360, 0, 0)
    web = os.path.join(env.tmp, "wallpapers", "web")
    os.makedirs(web, exist_ok=True)
    with open(os.path.join(web, "index.html"), "w") as f:
        f.write("<html><body style='background:#204060'>walpe</body></html>")
    rc.jdump(os.path.join(web, "project.json"), {"file": "index.html", "title": "web", "type": "web"})
    for how in ("quit", "killed"):
        rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
        if not check(env.start_daemon([A]), f"probe ({how}): daemon started"):
            return
        if "web_accel" not in env.state():
            print("skip probe (no web module in this build)")
            return
        pid = env.daemon.pid
        env.open(web)
        kids = wait_for(lambda: probe_children(pid), 15, 0.02)
        if not kids:
            print(f"skip probe ({how}): the WebKit start-up test was not seen running")
            env.stop_daemon()
            continue
        try:
            if how == "quit":
                env.ipc({"cmd": "quit"})
            else:
                env.daemon.kill()
            env.daemon.wait(timeout=30 * PATIENCE)
            env.daemon = None
            # The kernel ends the child when its parent has gone. One that was
            # only just started and had not asked for that yet ends itself
            # when it gets there, before it starts anything: in neither case
            # does it go on to run WebKit (processes of its own) without a
            # daemon. The moment after the daemon's end is left out: WebKit
            # processes it had started die with it a little later.
            t0, started = time.time(), []
            while any(running(k) for k in kids) and time.time() - t0 < 3 * PATIENCE:
                if time.time() - t0 > 0.3:
                    started += [c for k in kids if running(k) for c in children(k)]
                time.sleep(0.02)
            left = [k for k in kids if running(k)]
            check(not left and not started, f"probe ({how}): the WebKit start-up test ended with the daemon",
                  f"still running: {left}; ran on with {len(set(started))} process(es) of its own")
        finally:
            for k in kids:      # a build without the fix: do not leave them to the next case
                if running(k):
                    try:
                        os.kill(k, signal.SIGKILL)
                    except OSError:
                        pass
            if env.daemon is None and os.path.exists(env.sock):
                os.unlink(env.sock)
            for extra in (env.sock + ".loading", env.sock + ".lock"):
                if env.daemon is None and how == "killed" and os.path.exists(extra):
                    os.unlink(extra)
    env.stop_daemon()


# --------------------------------------------------------------- fullscreen
class XClient:
    """a second X client that plays window manager and application: plain windows, the
    EWMH properties a window manager would set (_NET_WM_STATE on a window,
    _NET_ACTIVE_WINDOW and _NET_CLIENT_LIST on the root). Xlib through ctypes."""

    XA_ATOM, XA_WINDOW = 4, 33

    def __init__(self, display):
        import ctypes
        import ctypes.util
        self.c = ctypes
        x = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        ul, i, u, vp = ctypes.c_ulong, ctypes.c_int, ctypes.c_uint, ctypes.c_void_p
        x.XOpenDisplay.restype, x.XOpenDisplay.argtypes = vp, [ctypes.c_char_p]
        x.XDefaultRootWindow.restype, x.XDefaultRootWindow.argtypes = ul, [vp]
        x.XCreateSimpleWindow.restype = ul
        x.XCreateSimpleWindow.argtypes = [vp, ul, i, i, u, u, u, ul, ul]
        x.XInternAtom.restype, x.XInternAtom.argtypes = ul, [vp, ctypes.c_char_p, i]
        x.XChangeProperty.argtypes = [vp, ul, ul, ul, i, i, ctypes.POINTER(ctypes.c_long), i]
        x.XDeleteProperty.argtypes = [vp, ul, ul]
        x.XMapWindow.argtypes = [vp, ul]
        x.XMoveResizeWindow.argtypes = [vp, ul, i, i, u, u]
        x.XDestroyWindow.argtypes = [vp, ul]
        x.XSync.argtypes = [vp, i]
        x.XCloseDisplay.argtypes = [vp]
        self.x = x
        self.d = x.XOpenDisplay(display.encode())
        if not self.d:
            raise OSError("cannot open the X display")
        self.root = x.XDefaultRootWindow(self.d)
        self.wins = []

    def atom(self, name):
        return self.x.XInternAtom(self.d, name.encode(), 0)

    def prop(self, win, name, kind, values):
        arr = (self.c.c_long * max(len(values), 1))(*values)
        self.x.XChangeProperty(self.d, win, self.atom(name), kind, 32, 0, arr, len(values))   # PropModeReplace
        self.x.XSync(self.d, 0)

    def window(self, x, y, w, h, parent=None):
        win = self.x.XCreateSimpleWindow(self.d, parent or self.root, x, y, w, h, 0, 0, 0)
        self.x.XMapWindow(self.d, win)
        self.x.XSync(self.d, 0)
        self.wins.append(win)
        return win

    def move(self, win, x, y, w, h):
        self.x.XMoveResizeWindow(self.d, win, x, y, w, h)
        self.x.XSync(self.d, 0)

    def fullscreen(self, win, on):
        self.prop(win, "_NET_WM_STATE", self.XA_ATOM, [self.atom("_NET_WM_STATE_FULLSCREEN")] if on else [])

    def focus(self, win):
        """win gets the focus as a window manager says it (0 = no window has it)"""
        self.prop(self.root, "_NET_ACTIVE_WINDOW", self.XA_WINDOW, [win])

    def clients(self, wins):
        self.prop(self.root, "_NET_CLIENT_LIST", self.XA_WINDOW, wins)

    def close(self):
        for name in ("_NET_ACTIVE_WINDOW", "_NET_CLIENT_LIST"):
            self.x.XDeleteProperty(self.d, self.root, self.atom(name))
        for win in reversed(self.wins):
            self.x.XDestroyWindow(self.d, win)
        self.x.XSync(self.d, 0)
        self.x.XCloseDisplay(self.d)


def case_fullscreen(env, W):
    """"Pause for fullscreen apps" pauses the monitor the focused fullscreen window is on,
    and only that one."""
    A, B = ("A", 640, 360, 0, 0), ("B", 640, 360, 640, 0)
    if env.display.winsys != "x11":
        print("skip fullscreen (the check sets a window manager's X properties itself: X11 only; "
              "scripts/waylandcheck.py has the case for a compositor)")
        return
    try:
        xc = XClient(env.env["DISPLAY"])
    except OSError:
        print("skip fullscreen (no libX11 for ctypes: cannot make a window)")
        return
    # (covered off: only the fullscreen rule may pause; each monitor draws its own scene)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False,
                          "render_shared": False})
    try:
        if not check(env.start_daemon([A, B]), "fullscreen: daemon started on two monitors"):
            return
        mark = env.mark()
        env.open(W["anim"])
        loaded(env, W["anim"])

        def mons():
            ms = (env.state().get("monitors") or [])
            return ms if len(ms) == 2 else [{}, {}]

        def flags(key):
            return [bool(m.get(key)) for m in mons()]

        def drawn(seconds=1.5):
            n = [m.get("frames") or 0 for m in mons()]
            time.sleep(seconds * PATIENCE)
            return [(m.get("frames") or 0) - k for m, k in zip(mons(), n)]

        def is_state(here, paused, what):
            """monitors' "fullscreen" and "paused" become what is wanted; then the frames follow"""
            ok = wait_for(lambda: flags("fullscreen") == here and flags("paused") == paused, 10, 0.1)
            st = env.state()
            check(bool(ok), "fullscreen: %s: fullscreen %s, paused %s" % (what, here, paused),
                  {"fullscreen": flags("fullscreen"), "paused": flags("paused"),
                   "monitors_state": st.get("monitors_state")})
            n = drawn()
            good = all((k <= 2) if p else (k > 3) for k, p in zip(n, paused))
            check(good, "fullscreen: %s: the paused monitor stops drawing, the other one draws" % what,
                  "frames in %.1f s: %s" % (1.5 * PATIENCE, n))
            return st

        wait_for(lambda: min(drawn(1.0)) > 3, 20, 0.1)
        st = is_state([False, False], [False, False], "no window")
        check(st.get("fullscreen") is False and st.get("fullscreen_pause") is False,
              "fullscreen: no window: the status of all monitors says so too",
              {k: st.get(k) for k in ("fullscreen", "fullscreen_pause")})
        check(st.get("pause_fullscreen_all") is False, "fullscreen: pause_fullscreen_all is off by default",
              st.get("pause_fullscreen_all"))
        src = ((st.get("pause_signals") or {}).get("fullscreen_on") or {}).get("from")
        check(src == "x11", "fullscreen: status names who tells the window's monitor", src)

        # a fullscreen window on A gets the focus
        w1 = xc.window(0, 0, 640, 360)
        xc.clients([w1])
        xc.fullscreen(w1, True)
        is_state([False, False], [False, False], "a fullscreen window that does not have the focus")
        xc.focus(w1)
        st = is_state([True, False], [True, False], "a focused fullscreen window on the first monitor")
        check(st.get("fullscreen") is True and st.get("fullscreen_pause") is True,
              "fullscreen: the status of all monitors: a fullscreen window has the focus, the rule pauses",
              {k: st.get(k) for k in ("fullscreen", "fullscreen_pause")})
        check([bool(m.get("fullscreen_pause")) for m in st.get("monitors") or []] == [True, False],
              "fullscreen: per monitor: the rule pauses the first one only",
              [m.get("fullscreen_pause") for m in st.get("monitors") or []])
        check("mon0:paused,fullscreen" in (st.get("monitors_state") or "") and
              "mon1:running" in (st.get("monitors_state") or ""),
              "fullscreen: monitors_state names the monitor and the reason", st.get("monitors_state"))

        # a tiling window manager's border and gap around it: still that monitor, and only it
        xc.move(w1, 12, 30, 616, 318)
        is_state([True, False], [True, False], "a border and a bar's gap around the window")
        # a frame that hangs a few pixels over into the neighbour
        xc.move(w1, 4, 0, 640, 360)
        is_state([True, False], [True, False], "four pixels of it on the neighbour")
        # moved to the second monitor (the window manager's "move to monitor")
        xc.move(w1, 640, 0, 640, 360)
        is_state([False, True], [False, True], "the window moved to the second monitor")
        # across both
        xc.move(w1, 0, 0, 1280, 360)
        is_state([True, True], [True, True], "the window across both monitors")
        xc.move(w1, 0, 0, 640, 360)
        is_state([True, False], [True, False], "back on the first monitor")

        # the focus goes to a plain window on the second monitor: the rule needs the focus
        w2 = xc.window(700, 40, 300, 200)
        xc.clients([w1, w2])
        xc.focus(w2)
        st = is_state([False, False], [False, False],
                      "the focus is on a plain window, the fullscreen one sits on the first monitor")
        check(st.get("fullscreen") is False, "fullscreen: ... and no fullscreen window has the focus",
              st.get("fullscreen"))
        xc.focus(w1)
        is_state([True, False], [True, False], "the focus is back on the fullscreen window")

        # every monitor, as before the rule looked at monitors
        code, rep = env.ctl("set", "fs_all=on")
        check(code == 0 and bool(rep and rep.get("ok")), "fullscreen: walpe-ctl set fs_all=on is taken", rep)
        st = is_state([True, False], [True, True], "pause_fullscreen_all on: every monitor pauses")
        check(st.get("pause_fullscreen_all") is True and
              [bool(m.get("fullscreen_pause")) for m in st.get("monitors") or []] == [True, True],
              "fullscreen: status: the setting, and the rule pauses both",
              (st.get("pause_fullscreen_all"), [m.get("fullscreen_pause") for m in st.get("monitors") or []]))
        try:
            saved = json.load(open(env.config)).get("pause_fullscreen_all")
        except (OSError, ValueError):
            saved = None
        check(saved is True, "fullscreen: the setting is saved in config.json", saved)
        code, rep = env.ctl("set", "pause_fullscreen_all=off")
        check(code == 0 and bool(rep and rep.get("ok")), "fullscreen: ... and off again under its full name", rep)
        is_state([True, False], [True, False], "pause_fullscreen_all off: the second monitor runs again")
        try:
            saved = "pause_fullscreen_all" in json.load(open(env.config))
        except (OSError, ValueError):
            saved = None
        check(saved is False, "fullscreen: switched off, the key leaves config.json again", saved)

        # the rule itself off: nothing pauses, status still says where the window is
        env.ctl("set", "fs=off")
        st = is_state([True, False], [False, False], "pause_fullscreen off")
        check(st.get("fullscreen") is True and st.get("fullscreen_pause") is False,
              "fullscreen: rule off: a fullscreen window has the focus, nothing is paused for it",
              {k: st.get(k) for k in ("fullscreen", "fullscreen_pause")})
        env.ctl("set", "fs=on")
        is_state([True, False], [True, False], "pause_fullscreen on again")

        # the window leaves fullscreen
        xc.fullscreen(w1, False)
        is_state([False, False], [False, False], "the window is no longer fullscreen")

        # under a reparenting window manager the client sits inside a frame: its own
        # position is the one in the frame (a few pixels), its place on the screen the frame's
        frame = xc.window(640, 0, 640, 360)
        w3 = xc.window(3, 20, 634, 337, parent=frame)
        xc.clients([w1, w2, w3])
        xc.fullscreen(w3, True)
        xc.focus(w3)
        is_state([False, True], [False, True], "a fullscreen client inside a frame window on the second monitor")
        xc.move(frame, 0, 0, 640, 360)
        is_state([True, False], [True, False], "its frame moved to the first monitor")
        xc.focus(0)
        is_state([False, False], [False, False], "no window has the focus")

        # a window that turns fullscreen right after it got the focus (an application
        # asks for fullscreen as soon as its window is mapped): the daemon must have
        # asked for the window's property events before it read the window's state, or
        # a change that falls between the two is told to nobody. A window the client
        # list does not name, so that nothing else watches it; many rounds, the state
        # change a little later each time
        w5 = xc.window(0, 0, 640, 360)
        missed = []
        for k in range(60):
            delay = (0, 50, 100, 200, 400, 800)[k % 6] / 1e6
            xc.focus(0)
            xc.fullscreen(w5, False)
            time.sleep(0.05)        # (the daemon is asleep again: it wakes for the focus change)
            xc.focus(w5)
            t0 = time.perf_counter()
            while time.perf_counter() - t0 < delay:
                pass
            xc.fullscreen(w5, True)
            time.sleep(0.05)
            if not wait_for(lambda: flags("fullscreen") == [True, False], 2, 0.05):
                missed.append(int(delay * 1e6))
        check(not missed, "fullscreen: a window that turns fullscreen right after it got the focus is "
              "seen every time", "missed in %d of 60 rounds (delays in us: %s)" % (len(missed), missed))
        xc.focus(0)
        xc.fullscreen(w5, False)
        is_state([False, False], [False, False], "after those rounds")
        lc_bad = [ln for ln in env.since(mark).splitlines()
                  if re.search(r"CRITICAL \*\*|AddressSanitizer|runtime error:|Segmentation fault", ln)]
        check(env.daemon.poll() is None and not lc_bad, "fullscreen: the daemon runs, its log is clean", lc_bad[:5])
        env.stop_daemon()

        # The same with shared rendering, the default: same-size monitors that show one
        # scene are rendered once, by a leader the others follow. Each still pauses on
        # its own: the leader's scene keeps running for a follower that is shown.
        xc.close()
        xc = XClient(env.env["DISPLAY"])
        rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
        if not check(env.start_daemon([A, B]), "fullscreen: daemon started again, with shared rendering"):
            return
        env.open(W["anim"])
        loaded(env, W["anim"])
        wait_for(lambda: min(drawn(1.0)) > 3, 20, 0.1)
        st = env.state()
        check(st.get("shared_followers") == 1, "fullscreen: one monitor follows the other's rendering",
              st.get("monitors_state"))
        w4 = xc.window(0, 0, 640, 360)
        xc.clients([w4])
        xc.fullscreen(w4, True)
        xc.focus(w4)
        is_state([True, False], [True, False], "shared rendering, the window on the first monitor")
        xc.move(w4, 640, 0, 640, 360)
        is_state([False, True], [False, True], "shared rendering, the window on the second monitor")
        xc.move(w4, 0, 0, 1280, 360)
        is_state([True, True], [True, True], "shared rendering, the window across both")
        xc.focus(0)
        is_state([False, False], [False, False], "shared rendering, no window has the focus")
    finally:
        xc.close()
    env.stop_daemon()


CASES = [("pointer", case_pointer), ("reload", case_reload), ("restart", case_restart),
         ("layout", case_layout), ("stats", case_stats), ("busy", case_busy), ("probe", case_probe),
         ("fullscreen", case_fullscreen), ("video", case_video)]



def main():
    args = sys.argv[1:]
    build, daemon, keep, tmp, only, display = "build", None, False, None, None, None
    while args:
        a = args.pop(0)
        if a == "--build" and args:
            build = args.pop(0)
        elif a == "--daemon" and args:
            daemon = args.pop(0)
        elif a == "--display" and args:
            display = args.pop(0)
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
    if not display:
        sys.stderr.write(__doc__)
        return 2
    skip = displayenv.unusable(display, build)
    if skip:
        print("lifecheck: skipped (%s)" % skip[0])
        return skip[1]
    for b in (daemon or os.path.join(build, "walpe-daemon"), os.path.join(build, "walpe-ctl")):
        if not os.path.exists(b):
            print(f"lifecheck: {b} not found")
            return 2
    env = Env(build, daemon, keep, tmp, display)
    try:
        if len(env.sock.encode()) > 107:
            print(f"lifecheck: {env.sock} is too long for a socket ({len(env.sock.encode())} > 107 bytes): "
                  "pass --tmp <a short directory>")
            return 2
        wp = os.path.join(env.tmp, "wallpapers")
        W = {k: os.path.join(wp, k) for k in ("red", "blue", "broken", "ptr", "anim", "big")}
        W["pic"] = os.path.join(wp, "pic.bmp")
        W["clip"] = os.path.join(wp, "clip.mp4")
        W["badclip"] = os.path.join(wp, "damaged.mp4")
        rc.gen_scene(W["red"], (1, 0, 0))
        rc.gen_scene(W["blue"], (0, 0, 1))
        rc.gen_broken(W["broken"])
        gen_fx_scene(W["ptr"], POINTER_FRAG)
        gen_fx_scene(W["anim"], TIME_FRAG)
        gen_big_scene(W["big"])
        gen_bmp(W["pic"], (0, 200, 0), 1024, 576)
        if not env.start_x(1280, 360):
            print("lifecheck: skipped (the %s display did not start)" % display)
            return displayenv.SKIP
        for name, fn in CASES:
            if only and name not in only:
                continue
            try:
                fn(env, W)
            finally:
                env.stop_daemon()
    finally:
        env.close()
    print(f"lifecheck: {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
