#!/usr/bin/env python3
"""End-to-end checks of the daemon's lifecycle: loads, reloads, restarts.

  python3 -I scripts/lifecheck.py --build build [--daemon FILE] [--tray FILE] [--window FILE] [--display KIND] [--only a,b] [--keep] [--tmp DIR]

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
  push      the state lines of a subscriber (the tray, the window): a
            request that only reads (quality, layers, resolution without
            a change, profile_result, status, props, mem) sends none; a
            pause and a resume send one each.
  tray      walpe-tray next to two monitors that show two wallpapers: the
            status lines it is sent while nothing changes which wallpapers
            were shown last do not make it write its list of them again.
            Skipped in a build without the tray.
  window    the library window next to a running engine: open, idle and
            without the focus its main thread sleeps (no timer of a panel
            that shows nothing); status lines that say what the last one
            said draw no frame in it (counted by the window's own probe,
            WALPE_UI_STATS). Skipped in a build without the window.
  scan      the scan of the windows that decides "covered" and "an
            application has the focus" (X11): what it keeps of a window
            between two scans is dropped when the window's type or state
            changes without it moving (hidden, iconic, a dock type, back
            again), a window with a border and one inside a frame window
            are where the server says, and a window that is moved costs
            the X requests of a scan that asks each window for its
            attributes only.
  panel     a wallpaper's quality panel and the engine's answers (the
            program perf-panel-test of the build, on this check's
            display): an answer that says what the last one said leaves
            the panel's widgets alone. Skipped where it is not built.
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
  webunload the web module (WebKitGTK, GTK: it cannot be unloaded) leaves
            the daemon with its last web wallpaper: at the change of
            wallpapers that leaves no monitor with a web page the daemon
            restarts in place - same process, requests sent meanwhile are
            answered, no WebKit process stays behind, status says
            `web=not loaded`, every monitor shows its wallpaper again and
            the pause state is the one from before. Not while another
            monitor still shows a page, not twice within the time between
            two such restarts, not with `web_unload=off`; switched on
            again it happens at the next change of wallpapers. Skipped in
            a build without the web module.
  fullscreen  "Pause for fullscreen apps" per monitor (X11): two monitors,
            a window that says it is fullscreen and has the focus - the
            check plays the window manager and sets the EWMH properties
            itself. The monitor the window is on pauses and the other one
            keeps drawing: with a border around the window (tiling window
            managers), inside a frame window (reparenting ones), moved to
            the other monitor, across both; nothing pauses while the focus
            is on another window; `set fs_all=on` pauses every monitor as
            earlier versions did; status says all of it per monitor. Two
            fullscreen windows, one per monitor: the pause follows the
            focus; the focused one destroyed; with "pause while covered"
            on the monitor stays paused when the focus leaves and draws
            nothing in between; a window that turns fullscreen right after
            it got the focus; the daemon's own windows named as the focused
            one. A daemon on one of the two monitors (--monitor 1), with
            the window there before it starts. Then again with shared
            rendering (one monitor following the other's frames): each
            monitor still pauses on its own.
  winlost   another X client destroys one of the daemon's wallpaper windows
            (X11): the daemon says so and restarts in place - same process -
            and every monitor shows its wallpaper again, also the monitor
            that followed the destroyed window's frames (shared rendering).
  convert   a video wallpaper the player cannot read: one whose conversion
            cannot start is refused with the reason, one that is being
            converted can be cancelled (and started again); the wallpaper on
            screen stays through all of it, and a daemon built to look for
            leaks ends with status 0 afterwards. Needs the ffmpeg program;
            skipped without it.
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
the fixes: the cases fail there); --tray FILE and --window FILE do the same for walpe-tray and walpe. LIFECHECK_PATIENCE=N multiplies every
wait (a software renderer on a busy machine: 3 or more). --tmp DIR: work in
DIR (must not exist; removed afterwards)."""
import json, os, re, shutil, signal, socket, struct, subprocess, sys, threading, time

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

    def start_daemon(self, mons, cpus=None, extra=None, hz=None, args=None, log_append=True):
        hz = hz or [60] * len(mons)
        self.mons = mons = self.display.place(mons, hz)
        env = dict(self.env)
        if self.display.fake_monitors:
            env["WALPE_MONITORS"] = ",".join(f"{n}={w}x{h}+{x}+{y}@{z}" for (n, w, h, x, y), z in zip(mons, hz))
        env.update(extra or {})
        self.run_env = env
        if log_append:
            log = open(self.log, "a")
        else:
            # what a shell's "> file" is after a while: opened for writing,
            # not for appending, the offset at the end
            log = os.fdopen(os.open(self.log, os.O_WRONLY | os.O_CREAT, 0o600), "wb", buffering=0)
            log.seek(0, os.SEEK_END)
        pre = (lambda: os.sched_setaffinity(0, cpus)) if cpus else None
        self.daemon = subprocess.Popen([self.daemon_bin] + list(args or []), env=env, stdout=log,
                                       stderr=subprocess.STDOUT, cwd=self.tmp, preexec_fn=pre)
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


# --------------------------------------------------------------------- push
class Subscriber:
    """one connection that asked for the state lines, as the tray and the window do"""

    def __init__(self, sock):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(6.0 * PATIENCE)
        self.s.connect(sock)
        self.s.sendall(b'{"cmd":"subscribe"}\n')
        self.buf = b""
        first = self.line()
        self.ok = bool(first and first.get("subscribed"))

    def line(self, timeout=None):
        """the next line -> dict, None when none came in time"""
        self.s.settimeout(timeout if timeout is not None else 6.0 * PATIENCE)
        try:
            while b"\n" not in self.buf:
                d = self.s.recv(65536)
                if not d:
                    return None
                self.buf += d
        except (socket.timeout, OSError):
            return None
        ln, self.buf = self.buf.split(b"\n", 1)
        try:
            return json.loads(ln.decode())
        except ValueError:
            return None

    def lines(self, quiet=1.0):
        """every line that arrives until nothing came for `quiet` seconds"""
        out = []
        while True:
            ln = self.line(quiet * PATIENCE)
            if ln is None:
                return out
            out.append(ln)

    def close(self):
        self.s.close()


def case_push(env, W):
    """A subscriber is sent the state when something may have changed it. A
    request that only reads changes nothing: the line it used to cause (the
    whole status, which always differs by its frame counters) went to every
    subscriber - with the window's performance panel on screen, every 2 s."""
    A = ("A", 640, 360, 0, 0)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    if not check(env.start_daemon([A]), "push: daemon started"):
        return
    env.open(W["anim"])
    loaded(env, W["anim"])
    time.sleep(2.0)
    sub = Subscriber(env.sock)
    if not check(sub.ok, "push: subscribed"):
        return
    sub.lines(1.5)      # what the load still sends
    reads = [{"cmd": "quality"}, {"cmd": "quality", "describe": True}, {"cmd": "layers"},
             {"cmd": "resolution"}, {"cmd": "profile_result"}, {"cmd": "status"}, {"cmd": "props"},
             {"cmd": "mem"}, {"cmd": "steam_libraries"}]
    answered = 0
    for _ in range(3):
        for req in reads:
            answered += 1 if env.ipc(req) is not None else 0
    got = sub.lines(1.5)
    check(answered == 3 * len(reads), "push: the requests that only read were answered",
          f"{answered} of {3 * len(reads)}")
    check(not got, "push: requests that only read send no state line to the subscriber",
          f"{len(got)} lines, {sum(len(json.dumps(g)) for g in got)} bytes")
    # what does change the state is still told
    env.ipc({"cmd": "pause"})
    got = sub.lines(1.5)
    check(any((g.get("state") or {}).get("paused") is True for g in got),
          "push: a pause is told to the subscriber", f"{len(got)} lines")
    env.ipc({"cmd": "resume"})
    got = sub.lines(1.5)
    check(any((g.get("state") or {}).get("paused") is False for g in got),
          "push: a resume is told to the subscriber", f"{len(got)} lines")
    # a resolution request that changes something is one
    env.ipc({"cmd": "resolution", "prompt": False})
    got = sub.lines(1.5)
    check(any((g.get("state") or {}).get("resolution_prompt") is False for g in got),
          "push: a resolution request that changes a setting is told", f"{len(got)} lines")
    # An engine that leaves because it was asked to says so, as its last line:
    # the end of the connection alone is what a crash looks like too, and a
    # window that had not started this engine started a new one after
    # `walpe-ctl quit`. An engine that is killed says nothing.
    env.ipc({"cmd": "quit"})
    got = sub.lines(2.0)
    check(bool(got) and got[-1].get("event") == "quit" and "state" not in got[-1],
          "push: an engine told to quit says so to its subscribers, last of all",
          [g.get("event") for g in got])
    sub.close()
    env.stop_daemon()
    if not check(env.start_daemon([A]), "push: daemon started again"):
        return
    sub = Subscriber(env.sock)
    sub.lines(1.0)
    env.daemon.kill()
    env.daemon.wait()
    got = sub.lines(2.0)
    check(not any(g.get("event") == "quit" for g in got), "push: an engine that is killed says nothing of the kind",
          [g.get("event") for g in got])
    sub.close()
    env.daemon = None
    env.stop_daemon()


# --------------------------------------------------------------------- tray
def subscribers(env):
    rep = env.ipc({"cmd": "mem", "wait": 0.2}) or {}
    return ((rep.get("mem") or rep).get("counters") or {}).get("subscribers", 0)


def case_tray(env, W):
    """The tray keeps the wallpapers shown last in a file. With two monitors
    showing two wallpapers every status line moved the second one to the
    front and the first one back: the same list, and the file was written
    again (a new file, synced, renamed over the old one) for each line."""
    tray_bin = env.tray_bin or os.path.join(env.build, "walpe-tray")
    if not os.path.exists(tray_bin):
        print("lifecheck: tray skipped (no walpe-tray in the build)")
        return
    A, B = ("A", 640, 360, 0, 0), ("B", 640, 360, 640, 0)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    if not check(env.start_daemon([A, B]), "tray: daemon started"):
        return
    env.open(W["red"], 0)
    env.open(W["blue"], 1)
    loaded(env, W["red"], 0)
    loaded(env, W["blue"], 1)
    log = open(os.path.join(env.tmp, "tray.log"), "a")
    tray = subprocess.Popen([tray_bin], env=env.run_env, stdout=log, stderr=subprocess.STDOUT, cwd=env.tmp)
    log.close()
    try:
        recent = os.path.join(env.run_env["HOME"], ".local", "state", "walpe", "tray-recent")
        if "XDG_STATE_HOME" in env.run_env:
            recent = os.path.join(env.run_env["XDG_STATE_HOME"], "walpe", "tray-recent")
        up = wait_for(lambda: tray.poll() is None and subscribers(env) >= 1 and os.path.exists(recent), 30, 0.3)
        if not check(bool(up), "tray: it runs, listens to the engine and wrote its list",
                     f"exit status {tray.poll()}, subscribers {subscribers(env)}"):
            return
        time.sleep(1.5)

        def ident():
            st = os.stat(recent)
            return st.st_ino, st.st_mtime_ns

        first = ident()
        with open(recent) as f:
            text = f.read()
        check(W["red"] in text and W["blue"] in text, "tray: both wallpapers are in the list", text)
        sub = Subscriber(env.sock)
        sub.lines(1.0)
        told = 0
        for _ in range(4):
            env.ipc({"cmd": "pause"})
            told += len(sub.lines(0.6))
            env.ipc({"cmd": "resume"})
            told += len(sub.lines(0.6))
        sub.close()
        time.sleep(1.0)
        check(told >= 8, "tray: the engine sent a status line for every pause and resume", told)
        check(ident() == first, "tray: status lines that change no wallpaper leave the list file alone",
              f"(inode, mtime) {first} -> {ident()} after {told} lines")
        # a wallpaper that was not shown before is written
        env.open(W["anim"], 1)
        loaded(env, W["anim"], 1)

        def has_anim():
            with open(recent) as f:
                return W["anim"] in f.read()
        check(bool(wait_for(has_anim, 10, 0.3)), "tray: a new wallpaper is added to the list file")
        # A write that fails is not the end of it: with the list written only when it
        # changed, a failed write (a full disk; here a folder in the file's place) left the
        # file behind until the next change of wallpaper. The next status line tries again.
        os.unlink(recent)
        os.makedirs(os.path.join(recent, "in-the-way"))
        env.open(W["ptr"], 1)
        loaded(env, W["ptr"], 1)
        time.sleep(1.0 * PATIENCE)
        shutil.rmtree(recent)
        env.ipc({"cmd": "pause"})
        env.ipc({"cmd": "resume"})

        def has_ptr():
            try:
                with open(recent) as f:
                    return W["ptr"] in f.read()
            except OSError:
                return False
        check(bool(wait_for(has_ptr, 10, 0.3)),
              "tray: a list that could not be written is written with the next status line")
    finally:
        tray.terminate()
        try:
            tray.wait(timeout=10 * PATIENCE)
        except subprocess.TimeoutExpired:
            tray.kill()
            tray.wait()
    env.stop_daemon()


# ------------------------------------------------------------------- window
def case_window(env, W):
    """Every status line of the engine went through the window's engine
    indicator: its style class taken away and put back, its tooltip set to
    what it was. GTK restyles for the one and asks the X server where the
    pointer is for the other (under a server grab): a layout, a frame and
    some twenty round trips per line, whatever the line said."""
    win_bin = env.window_bin or os.path.join(env.build, "walpe")
    if not os.path.exists(win_bin):
        print("lifecheck: window skipped (no walpe in the build)")
        return
    A = ("A", 640, 360, 0, 0)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False,
                          "wallpapers_dir": os.path.dirname(W["red"]), "preview_anim": "off"})
    put(os.path.join(os.path.dirname(env.config), "tray.ini"), "[tray]\nstart_with_gui=false\n")
    if not check(env.start_daemon([A]), "window: daemon started"):
        return
    env.open(W["red"])
    loaded(env, W["red"])
    stats = os.path.join(env.tmp, "ui-stats.txt")
    genv = dict(env.run_env)
    genv.update({"GSK_RENDERER": "cairo", "GTK_A11Y": "none"})

    def start(extra=None):
        e = dict(genv)
        e.update(extra or {})
        log = open(os.path.join(env.tmp, "window.log"), "a")
        w = subprocess.Popen([win_bin], env=e, stdout=log, stderr=subprocess.STDOUT, cwd=env.tmp)
        log.close()
        up = wait_for(lambda: w.poll() is not None or subscribers(env) >= 1, 60, 0.5)
        return w, bool(up) and w.poll() is None

    def stop(w):
        w.terminate()
        try:
            w.wait(timeout=20 * PATIENCE)
        except subprocess.TimeoutExpired:
            w.kill()
            w.wait()
        wait_for(lambda: subscribers(env) == 0, 10, 0.3)

    def main_wakeups(pid):
        """how often the window's main thread has gone to sleep and been woken"""
        try:
            with open(f"/proc/{pid}/task/{pid}/status") as f:
                for ln in f:
                    if ln.startswith("voluntary_ctxt_switches"):
                        return int(ln.split()[1])
        except OSError:
            pass
        return None

    # The window open with nothing to do and without the focus (no status
    # request of its own then): its main thread sleeps. The timer of the
    # wallpaper's quality panel ran every 2 s while that panel was one closed
    # line, to do nothing.
    win, ok = start()
    try:
        if not check(ok, "window: it runs and listens to the engine", f"exit status {win.poll()}"):
            return
        xc = XClient(env.env["DISPLAY"])
        try:
            xc.input_focus(xc.window(1200, 300, 40, 40))     # the focus goes to another window
            time.sleep(8.0 * PATIENCE)      # the library is read, the first pictures are drawn
            woke = None
            for _ in range(6):
                a = main_wakeups(win.pid)
                time.sleep(16.0)
                b = main_wakeups(win.pid)
                woke = b - a if a is not None and b is not None else None
                if woke is None or woke <= 3:
                    break
            check(woke is not None and woke <= 3,
                  "window: open, idle and without the focus, its main thread sleeps",
                  f"{woke} wake-ups in 16 s")

            # `walpe-ctl quit` while the window is open. The window did not
            # start this engine, so the engine's going away was a crash to it
            # and a new engine ran a second later. It stays stopped now, and
            # the window does not go on asking for it either (it asked a
            # socket that was not there every 2 s for as long as it stayed
            # open).
            def answers():
                return env.ipc({"cmd": "status"}, timeout=1.5) is not None

            rcode, _ = env.ctl("quit")
            env.daemon.wait(timeout=20 * PATIENCE)
            env.daemon = None
            back = wait_for(answers, 8.0 * PATIENCE, 0.5)
            check(rcode == 0 and not back and win.poll() is None,
                  "window: an engine stopped with walpe-ctl quit stays stopped while the window is open")
            woke = None
            for _ in range(4):
                a = main_wakeups(win.pid)
                time.sleep(16.0)
                b = main_wakeups(win.pid)
                woke = b - a if a is not None and b is not None else None
                if woke is None or woke <= 3:
                    break
            check(not back and woke is not None and woke <= 3,
                  "window: its main thread sleeps while the engine is stopped", f"{woke} wake-ups in 16 s")
            # an engine somebody starts is found without asking, and one that
            # dies is still started again by the window
            if check(env.start_daemon([A]), "window: an engine started from outside runs"):
                check(bool(wait_for(lambda: subscribers(env) >= 1, 20, 0.5)),
                      "window: it listens to the engine that was started from outside")
                time.sleep(1.0)
                env.daemon.kill()
                env.daemon.wait()
                env.daemon = None
                check(bool(wait_for(answers, 40, 0.5)) and win.poll() is None,
                      "window: an engine that is killed is started again by the window")
                env.ipc({"cmd": "quit"})    # (now the window's own: quit, the window ends below)
                wait_for(lambda: not answers(), 15, 0.5)
        finally:
            xc.close()
    finally:
        stop(win)
    env.stop_daemon()
    if not check(env.start_daemon([A]), "window: daemon started again"):
        return
    loaded(env, W["red"])

    win, ok = start({"WALPE_UI_STATS": stats})

    def frames(seconds, during=None):
        """frame clock cycles of the window in the next `seconds`, None when it is gone"""
        if win.poll() is not None:
            return None
        mark = os.path.getsize(stats) if os.path.exists(stats) else 0
        win.send_signal(signal.SIGUSR1)     # counts from here
        time.sleep(0.3 * PATIENCE)
        if during:
            during()
        time.sleep(seconds * PATIENCE)
        if win.poll() is not None:
            return None
        win.send_signal(signal.SIGUSR2)     # writes them

        def written():
            try:
                with open(stats) as f:
                    f.seek(mark)
                    m = re.findall(r"^frame_work: n=(\d+)", f.read(), re.M)
                    return [int(m[-1])] if m else None
            except OSError:
                return None
        got = wait_for(written, 10, 0.2)
        return got[0] if got else None

    try:
        if not check(ok, "window: it runs again, with its frame probe", f"exit status {win.poll()}"):
            return
        time.sleep(3.0 * PATIENCE)   # the probe is attached with the window; the library is read
        quiet = None
        for _ in range(12):
            quiet = frames(2.0)
            if quiet == 0 or quiet is None:
                break
        if not check(quiet == 0, "window: nothing is drawn while nothing happens", quiet):
            return
        sub = Subscriber(env.sock)
        sub.lines(1.0)
        told = [0]

        def poke():
            # a request that changes nothing and is none that only reads: the
            # engine tells its subscribers, and the line says what the last did
            for _ in range(5):
                env.ipc({"cmd": "import_cancel"})
                told[0] += len(sub.lines(0.5))
        n = frames(1.5, poke)
        sub.close()
        check(told[0] >= 5, "window: the engine sent a status line per request", told[0])
        check(n is not None and n <= 1, "window: status lines that change nothing draw no frame",
              f"{n} frames for {told[0]} lines")
        # a line that does change something is shown
        n = frames(1.5, lambda: env.ipc({"cmd": "pause"}))
        check(n is not None and n >= 1, "window: a pause is drawn", n)
        env.ipc({"cmd": "resume"})
    finally:
        stop(win)
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
    # the value it has: answered like every other key set to its own value
    # (it was "nothing to set", an error), and nothing restarts for it
    mark = env.mark()
    rep = env.ipc({"cmd": "set", "web_accel": bool(st.get("web_accel"))})
    check(bool(rep and rep.get("ok")), "busy: web_accel set to the value it has is no error", rep)
    time.sleep(1.0)     # (a restart would be announced 200 ms after the reply)
    check("restarting to apply it" not in env.since(mark), "busy: and restarts nothing", env.since(mark)[-400:])
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


# ---------------------------------------------------------------- webunload
def gen_web(path):
    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, "index.html"), "w") as f:
        f.write("<html><body style='background:#204060'>walpe</body></html>")
    rc.jdump(os.path.join(path, "project.json"), {"file": "index.html", "title": os.path.basename(path),
                                                  "type": "web"})


def case_webunload(env, W):
    """The web module leaves the daemon with its last web wallpaper (a restart in place)."""
    A, B = ("A", 640, 360, 0, 0), ("B", 640, 360, 640, 0)
    web = os.path.join(env.tmp, "wallpapers", "page")
    gen_web(web)
    SAID = "restarting in place to unload the web module"

    def modules():
        return env.state().get("modules", "")

    def web_shown(mon=None, timeout=120):
        # (the first page of a daemon waits for the WebKit start-up test)
        return loaded(env, web, mon, timeout) and "web=loaded" in modules()

    def restarted(mark):
        txt = env.since(mark)
        return SAID in txt and '" up (' in txt[txt.index(SAID):] and bool(env.state())

    # ---- 1. two monitors; a long time between two restarts
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False,
                          "current": W["red"]})
    if not check(env.start_daemon([A, B], extra={"WALPE_WEB_UNLOAD_GAP_S": "3600"}), "webunload: daemon started"):
        return
    st = env.state()
    if "web_accel" not in st:
        print("skip webunload (no web module in this build)")
        return
    A, B = env.mons
    pid = env.daemon.pid
    check(st.get("web_unload") is True, "webunload: the setting is there and on", st.get("web_unload"))
    check(loaded(env, W["red"]) and "web=not loaded" in modules(), "webunload: two scenes, no web module",
          modules())
    env.open(web)
    if not check(web_shown(), "webunload: a web wallpaper on both monitors, the module is loaded",
                 (rc.brief(env.monitors()), modules())):
        return
    kids = children(pid)
    check(len(kids) > 0, "webunload: WebKit's processes run", kids)
    # one monitor leaves the page, the other still shows it: nothing restarts
    mark = env.mark()
    env.open(W["red"], 0)
    check(loaded(env, W["red"], 0), "webunload: a scene on the first monitor", rc.brief(env.monitors()))
    time.sleep(1.0)
    check(SAID not in env.since(mark) and "web=loaded" in modules() and env.daemon.pid == pid,
          "webunload: no restart while the other monitor shows a web wallpaper", env.since(mark)[-300:])
    # the last one leaves, with the wallpapers paused
    rep = env.ipc({"cmd": "pause"})
    check(bool(rep and rep.get("ok")), "webunload: paused", rep)
    mark = env.mark()
    env.open(W["blue"], 1)
    answers = []
    for _ in range(5):      # requests while the daemon is taking itself apart and starting again
        answers.append(bool(env.ipc({"cmd": "status"}, timeout=40)))
        time.sleep(0.1)
    check(bool(wait_for(lambda: restarted(mark), 60, 0.3)),
          "webunload: the daemon restarts in place when the last web wallpaper is replaced", env.since(mark)[-400:])
    check(all(answers), "webunload: requests sent during the restart were answered", answers)
    check(env.daemon.poll() is None and env.daemon.pid == pid, "webunload: the same process went on")
    check(bool(wait_for(lambda: "web=not loaded" in modules(), 20)), "webunload: status says web=not loaded",
          modules())
    ms = env.monitors()
    check([m.get("current") for m in ms] == [W["red"], W["blue"]], "webunload: every monitor has its wallpaper",
          rc.brief(ms))
    check(env.state().get("paused") is True, "webunload: still paused after the restart")
    env.ipc({"cmd": "resume"})
    check(loaded(env, W["red"], 0) and loaded(env, W["blue"], 1), "webunload: both wallpapers are loaded again",
          rc.brief(env.monitors()))
    c0, c1 = colour_is(env, A, lambda c: rc.name_of(c) == "red"), colour_is(env, B, lambda c: rc.name_of(c) == "blue")
    check(rc.name_of(c0) == "red" and rc.name_of(c1) == "blue", "webunload: and on screen", (c0, c1))
    wait_for(lambda: not [k for k in kids if running(k)] and not children(pid), 15)
    check(not [k for k in kids if running(k)] and not children(pid),
          "webunload: no WebKit process is left", ([k for k in kids if running(k)], children(pid)))
    # a second time within the time between two restarts: the module stays
    env.open(web, 0)
    check(web_shown(0), "webunload: a web wallpaper again", (rc.brief(env.monitors()), modules()))
    mark = env.mark()
    env.open(W["red"], 0)
    check(loaded(env, W["red"], 0), "webunload: replaced by a scene", rc.brief(env.monitors()))
    time.sleep(1.0)
    check(SAID not in env.since(mark) and "web=loaded" in modules() and
          "the web module stays loaded this time" in env.since(mark),
          "webunload: no second restart so soon after the first (the log says why)", env.since(mark)[-300:])
    env.stop_daemon()

    # ---- 2. the switch
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False,
                          "current": W["red"]})
    if not check(env.start_daemon([A], extra={"WALPE_WEB_UNLOAD_GAP_S": "0"}), "webunload (off): daemon started"):
        return
    pid = env.daemon.pid
    rep = env.ipc({"cmd": "set", "web_unload": False})
    check(bool(rep and rep.get("ok")) and env.state().get("web_unload") is False, "webunload (off): switched off", rep)
    env.open(web)
    check(web_shown(), "webunload (off): a web wallpaper", (rc.brief(env.monitors()), modules()))
    mark = env.mark()
    env.open(W["red"])
    check(loaded(env, W["red"]), "webunload (off): replaced by a scene", rc.brief(env.monitors()))
    time.sleep(1.0)
    check(SAID not in env.since(mark) and "web=loaded" in modules(),
          "webunload (off): no restart, the module stays loaded", env.since(mark)[-300:])
    rep = env.ipc({"cmd": "set", "web_unload": True})
    time.sleep(1.0)
    check(bool(rep and rep.get("ok")) and SAID not in env.since(mark),
          "webunload (off): switching it on restarts nothing by itself", env.since(mark)[-300:])
    env.open(W["blue"])
    check(bool(wait_for(lambda: restarted(mark), 60, 0.3)),
          "webunload (off): switched on again, the next change of wallpapers restarts", env.since(mark)[-400:])
    check(env.daemon.poll() is None and env.daemon.pid == pid and
          bool(wait_for(lambda: "web=not loaded" in modules(), 20)),
          "webunload (off): the same process, web=not loaded", modules())
    check(loaded(env, W["blue"]) and env.state().get("web_unload") is True,
          "webunload (off): the wallpaper of that change is shown, the setting was kept", rc.brief(env.monitors()))
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
        x.XQueryTree.argtypes = [vp, ul, ctypes.POINTER(ul), ctypes.POINTER(ul),
                                 ctypes.POINTER(ctypes.POINTER(ul)), ctypes.POINTER(u)]
        x.XFree.argtypes = [vp]
        x.XSync.argtypes = [vp, i]
        x.XSetInputFocus.argtypes = [vp, ul, i, ul]
        x.XSetWindowBorderWidth.argtypes = [vp, ul, u]
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

    def destroy(self, win):
        self.x.XDestroyWindow(self.d, win)
        self.x.XSync(self.d, 0)
        self.wins.remove(win)

    def lowest(self, n):
        """the n lowest windows of the screen (children of the root, bottom first)"""
        c = self.c
        root, parent, count = c.c_ulong(), c.c_ulong(), c.c_uint()
        kids = c.POINTER(c.c_ulong)()
        if not self.x.XQueryTree(self.d, self.root, c.byref(root), c.byref(parent), c.byref(kids),
                                 c.byref(count)):
            return []
        out = [kids[k] for k in range(min(n, count.value))]
        if count.value:
            self.x.XFree(kids)
        return out

    def fullscreen(self, win, on):
        self.prop(win, "_NET_WM_STATE", self.XA_ATOM, [self.atom("_NET_WM_STATE_FULLSCREEN")] if on else [])

    def focus(self, win):
        """win gets the focus as a window manager says it (0 = no window has it)"""
        self.prop(self.root, "_NET_ACTIVE_WINDOW", self.XA_WINDOW, [win])

    def unprop(self, win, name):
        self.x.XDeleteProperty(self.d, win, self.atom(name))
        self.x.XSync(self.d, 0)

    def border(self, win, width):
        self.x.XSetWindowBorderWidth(self.d, win, width)
        self.x.XSync(self.d, 0)

    def input_focus(self, win):
        """the keyboard focus itself (no window manager here to give it)"""
        self.x.XSetInputFocus(self.d, win, 0, 0)     # RevertToNone, CurrentTime
        self.x.XSync(self.d, 0)

    def clients(self, wins):
        self.prop(self.root, "_NET_CLIENT_LIST", self.XA_WINDOW, wins)

    def close(self):
        if not self.d:
            return      # closed already: its Display is freed memory
        for name in ("_NET_ACTIVE_WINDOW", "_NET_CLIENT_LIST"):
            self.x.XDeleteProperty(self.d, self.root, self.atom(name))
        for win in reversed(self.wins):
            self.x.XDestroyWindow(self.d, win)
        self.x.XSync(self.d, 0)
        self.x.XCloseDisplay(self.d)
        self.d = None


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

        # two fullscreen windows, one on each monitor: the monitor of the one that has
        # the focus pauses, and the pause moves with the focus
        w6, w7 = xc.window(0, 0, 640, 360), xc.window(640, 0, 640, 360)
        xc.clients([w1, w2, w3, w6, w7])
        xc.fullscreen(w6, True)
        xc.fullscreen(w7, True)
        xc.focus(w6)
        is_state([True, False], [True, False], "two fullscreen windows, the focus on the first monitor's")
        xc.focus(w7)
        is_state([False, True], [False, True], "two fullscreen windows, the focus on the second monitor's")
        # the focused one is destroyed; the window manager says a moment later where the
        # focus went (until then the property names a window that is gone)
        xc.destroy(w7)
        time.sleep(0.5 * PATIENCE)
        check(env.daemon.poll() is None, "fullscreen: the focused fullscreen window was destroyed: the daemon runs")
        xc.clients([w1, w2, w3, w6])
        xc.focus(w6)
        is_state([True, False], [True, False], "the focus went to the fullscreen window that is left")
        # "Pause while covered" on, as it is by default: when the focus leaves the
        # fullscreen window its monitor stays paused, as covered now, and draws nothing
        # while one rule takes over from the other
        env.ctl("set", "covered=on")
        is_state([True, False], [True, False], "pause_covered on, the focus on the fullscreen window")
        n0 = (mons()[0].get("frames") or 0)
        xc.focus(w2)
        st = is_state([False, False], [True, False], "the focus on a small window of the second monitor: "
                      "the first one stays paused, as covered")
        check("mon0:paused,covered" in (st.get("monitors_state") or ""),
              "fullscreen: monitors_state names the rule that pauses it now", st.get("monitors_state"))
        xc.focus(w6)
        is_state([True, False], [True, False], "the focus back on the fullscreen window")
        n1 = (mons()[0].get("frames") or 0)
        check(n1 - n0 <= 2, "fullscreen: the monitor drew nothing while fullscreen and covered took turns",
              "%d frames" % (n1 - n0))
        env.ctl("set", "covered=off")
        xc.destroy(w6)
        xc.clients([w1, w2, w3])
        xc.focus(0)
        is_state([False, False], [False, False], "both fullscreen windows are gone")

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

        # Another client names the daemon's own windows in _NET_ACTIVE_WINDOW (any
        # client can write the property) and gives them the fullscreen state: they are
        # not application windows. Nothing pauses for them, and the daemon keeps the
        # events it asked for on them: what a window covered is drawn again.
        walls = xc.lowest(2)
        for wp in walls:
            xc.fullscreen(wp, True)
            xc.focus(wp)
            time.sleep(0.3)
        is_state([False, False], [False, False], "a wallpaper window named as the focused fullscreen window")
        xc.focus(0)
        for wp in walls:
            xc.fullscreen(wp, False)
        for win in (w1, w2, frame, w5):         # out of the way: the wallpapers are looked at
            xc.move(win, 0, 0, 8, 8)
        env.open(W["red"])
        loaded(env, W["red"])
        seen = []
        for mon in (A, B):
            before = rc.name_of(colour_is(env, mon, lambda c: rc.name_of(c) == "red"))
            over = xc.window(mon[3] + 220, 100, 200, 160)       # over the middle of the monitor
            colour_is(env, mon, lambda c: rc.name_of(c) == "black")
            xc.destroy(over)
            seen.append((before, rc.name_of(colour_is(env, mon, lambda c: rc.name_of(c) == "red"))))
        check(seen == [("red", "red")] * 2, "fullscreen: after that the daemon still draws again what a "
              "window covered (a scene that stands still)", seen)
        lc_bad = [ln for ln in env.since(mark).splitlines()
                  if re.search(r"CRITICAL \*\*|AddressSanitizer|runtime error:|Segmentation fault", ln)]
        check(env.daemon.poll() is None and not lc_bad, "fullscreen: the daemon runs, its log is clean", lc_bad[:5])
        env.stop_daemon()

        # The same with shared rendering, the default: same-size monitors that show one
        # scene are rendered once, by a leader the others follow. Each still pauses on
        # its own: the leader's scene keeps running for a follower that is shown.
        # (the new client before the old one leaves: an X server whose last client has
        # left resets itself and may refuse the next connection for a moment - and the
        # `finally` below then closed the closed client a second time)
        old, xc = xc, XClient(env.env["DISPLAY"])
        old.close()

        # The daemon draws on the second monitor only (--monitor 1). The monitor that
        # holds most of the window is still found among all monitors of the screen: a
        # fullscreen window on the first one whose edge reaches a few pixels into the
        # second is not on the second. The window is there, with the focus, before the
        # daemon starts: its first look at the windows finds it.
        wm = xc.window(640, 0, 640, 360)
        xc.clients([wm])
        xc.fullscreen(wm, True)
        xc.focus(wm)
        if not check(env.start_daemon([A, B], args=["--monitor", "1"]),
                     "fullscreen: daemon started on the second monitor only"):
            return
        env.open(W["anim"])
        loaded(env, W["anim"])

        def only():
            ms = env.state().get("monitors") or []
            return ms[0] if len(ms) == 1 else {}

        def only_is(here, what):
            ok = wait_for(lambda: env.state().get("fullscreen") is True and
                          bool(only().get("fullscreen")) == here and bool(only().get("paused")) == here, 10, 0.1)
            time.sleep(0.5 * PATIENCE)      # (the scan that follows a move has run)
            m = only()
            check(bool(ok) and bool(m.get("fullscreen")) == here and bool(m.get("paused")) == here,
                  "fullscreen: one of two monitors: %s" % what,
                  {k: m.get(k) for k in ("index", "fullscreen", "paused")})
        check(only().get("index") == 1, "fullscreen: one of two monitors: the daemon shows on the second",
              rc.brief(env.state().get("monitors") or []))
        only_is(True, "a fullscreen window that had the focus before the daemon started pauses it")
        xc.move(wm, 0, 0, 640, 360)
        only_is(False, "the window on the other monitor pauses nothing")
        xc.move(wm, 4, 0, 640, 360)
        only_is(False, "four pixels of it on this monitor: still the other monitor's window")
        xc.move(wm, 400, 0, 640, 360)
        only_is(True, "most of it on this monitor: paused")
        xc.move(wm, 0, 0, 1280, 360)
        only_is(True, "across both: paused")
        xc.focus(0)
        xc.clients([])
        xc.destroy(wm)
        env.stop_daemon()

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
        # a paused follower sleeps: the frames its leader goes on publishing for its own
        # monitor do not wake the follower's thread (it presents none of them)
        fol = next((i for i, m in enumerate(mons()) if m.get("follows") is not None), None)
        if check(fol is not None, "fullscreen: shared rendering: status names the follower", rc.brief(mons())):
            xc.move(w4, 640 * fol, 0, 640, 360)
            want = [i == fol for i in range(2)]
            is_state(want, want, "shared rendering, the window on the follower's monitor")
            before = mons()
            time.sleep(1.5 * PATIENCE)
            after = mons()
            woke = (after[fol].get("wakeups") or 0) - (before[fol].get("wakeups") or 0)
            led = (after[1 - fol].get("frames") or 0) - (before[1 - fol].get("frames") or 0)
            check(led > 3 and woke <= 2,
                  "fullscreen: shared rendering: the paused follower's thread sleeps while its leader draws",
                  "in %.1f s: %d frames of the leader, %d wake-ups of the follower" % (1.5 * PATIENCE, led, woke))
        xc.focus(0)
        is_state([False, False], [False, False], "shared rendering, no window has the focus")
    finally:
        xc.close()
    env.stop_daemon()


# ------------------------------------------------------------------ convert
def case_convert(env, W):
    """A video the player cannot read is converted once with ffmpeg. The
    path of its media file used to be handed to the conversion as a copy
    that only the conversion's own report freed: one that could not start,
    or was cancelled, left the copy behind every time."""
    A = ("A", 640, 360, 0, 0)
    if not shutil.which("ffmpeg") or not os.path.exists(os.path.join(env.build, "walpe-video.so")):
        print("skip convert (needs the ffmpeg program and the video module)")
        return
    wp = os.path.dirname(W["red"])
    # "clip.mkv" is a directory: there is nothing to convert
    nothing = os.path.join(wp, "novideo")
    os.makedirs(os.path.join(nothing, "clip.mkv"), exist_ok=True)
    rc.jdump(os.path.join(nothing, "project.json"), {"file": "clip.mkv", "title": "novideo", "type": "video"})
    # MPEG-4 part 2 in Matroska: plays only as a converted copy, and the
    # conversion takes long enough to be cancelled
    odd = os.path.join(wp, "odd")
    os.makedirs(odd, exist_ok=True)
    try:
        p = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
                            "-t", "30", "-c:v", "mpeg4", "-q:v", "6", os.path.join(odd, "clip.mkv")],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=300 * PATIENCE)
        made = p.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        made = False
    if not made:
        print("skip convert (the ffmpeg program here writes no MPEG-4 clip)")
        return
    rc.jdump(os.path.join(odd, "project.json"), {"file": "clip.mkv", "title": "odd", "type": "video"})
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    if not check(env.start_daemon([A]), "convert: daemon started"):
        return
    env.open(W["red"])
    loaded(env, W["red"])

    def shown():
        m = (env.monitors() or [{}])[0]
        return m.get("current") == W["red"] and m.get("type") == "scene" and not m.get("load_error")

    reps = [env.ipc({"cmd": "open", "path": nothing}) or {} for _ in range(3)]
    check(all(r.get("ok") is False and r.get("error") for r in reps),
          "convert: a video whose conversion cannot start is refused with the reason", reps[-1])
    check(not env.state().get("import_active") and shown(),
          "convert: ... nothing runs and the wallpaper stays", rc.brief(env.monitors()))
    for n in range(2):
        rep = env.ipc({"cmd": "open", "path": odd}) or {}
        check(rep.get("ok") is True and rep.get("converting") is True,
              f"convert: round {n + 1}: a clip the player cannot read is converted", rep)
        check(bool(wait_for(lambda: env.state().get("import_active"), 10, 0.1)),
              f"convert: round {n + 1}: status shows the conversion")
        time.sleep(0.5 * PATIENCE)
        rep = env.ipc({"cmd": "import_cancel"}) or {}
        check(rep.get("ok") is True and bool(wait_for(lambda: not env.state().get("import_active"), 10, 0.1)),
              f"convert: round {n + 1}: it is cancelled", rep)
        check(shown(), f"convert: round {n + 1}: the wallpaper on screen stayed", rc.brief(env.monitors()))
    # (a build with the leak sanitizer ends with another status when something was left allocated)
    env.ipc({"cmd": "quit"})
    try:
        st = env.daemon.wait(timeout=20 * PATIENCE)
    except subprocess.TimeoutExpired:
        st = None
    if st is not None:
        env.daemon = None
    check(st == 0, "convert: the daemon ends with status 0", st)
    env.stop_daemon()


# ------------------------------------------------------------------ winlost
def case_winlost(env, W):
    """Another X client destroys a wallpaper window. The daemon drew into a
    drawable that no longer existed, without a word: that monitor showed the
    root window for the rest of the daemon's life, and with shared rendering
    so did the monitors that presented its frames."""
    A, B = ("A", 640, 360, 0, 0), ("B", 640, 360, 640, 0)
    if env.display.winsys != "x11":
        print("skip winlost (an X client destroys the daemon's window: X11 only; a compositor that "
              "closes a layer surface is scripts/waylandcheck.py's)")
        return
    try:
        xc = XClient(env.env["DISPLAY"])
    except OSError:
        print("skip winlost (no libX11 for ctypes: cannot destroy a window)")
        return

    def is_red(c):
        return rc.name_of(c) == "red"

    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    try:
        if not check(env.start_daemon([A, B]), "winlost: daemon started on two monitors"):
            return
        pid = env.daemon.pid
        env.open(W["red"])
        loaded(env, W["red"])
        check(is_red(colour_is(env, A, is_red)) and is_red(colour_is(env, B, is_red)),
              "winlost: the scene is on both monitors", (env.colour(A), env.colour(B)))
        wins = xc.lowest(2)     # the wallpaper windows are the lowest children of the root
        if not check(len(wins) == 2, "winlost: the daemon has two windows", wins):
            return
        for n in range(2):
            win = wins[n]
            if n:
                # (restarts for the layout are ten seconds apart; the second one waits for that)
                time.sleep(1.0 * PATIENCE)
            mark = env.mark()
            xc.x.XDestroyWindow(xc.d, win)
            xc.x.XSync(xc.d, 0)
            check(bool(wait_for(lambda: "took its surface away" in env.since(mark), 10, 0.1)),
                  f"winlost: window {n + 1} destroyed: the daemon says that it lost the surface",
                  env.since(mark)[-300:])
            check(bool(wait_for(lambda: "restarting to follow it" in env.since(mark), 40, 0.2)),
                  f"winlost: window {n + 1} destroyed: it restarts in place", env.since(mark)[-300:])
            check(bool(wait_for(lambda: loaded(env, W["red"], timeout=2), 60, 0.2)),
                  f"winlost: window {n + 1} destroyed: the wallpaper is loaded again", rc.brief(env.monitors()))
            check(is_red(colour_is(env, A, is_red, 20)) and is_red(colour_is(env, B, is_red, 20)),
                  f"winlost: window {n + 1} destroyed: both monitors show it again",
                  (rc.name_of(env.colour(A)), rc.name_of(env.colour(B))))
            # (the new image's connection may be given the same window ids again)
            now = xc.lowest(2)
            check(len(now) == 2, f"winlost: window {n + 1} destroyed: two windows are there again", now)
            wins = now
        check(env.daemon.poll() is None and os.path.exists(f"/proc/{pid}"), "winlost: same process")
    finally:
        xc.close()
    env.stop_daemon()


def case_logcap(env, W):
    """The daemon's log file does not grow without limit, whoever started the
    daemon: a file that is over the limit when the daemon starts is cut back
    at once (not after the 256th line of a run that may never write so
    many), and a file that was opened for writing instead of appending - a
    shell's "> file" - is capped like any other and stays the size it was
    cut to."""
    A = ("A", 320, 180, 0, 0)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False})
    limit = 1 << 20
    for append in (True, False):
        how = "opened for appending" if append else "opened for writing"
        with open(env.log, "w") as f:
            i = 0
            while f.tell() < limit * 3 // 2:
                f.write("old line %07d of an earlier run\n" % i)
                i += 1
        last_old = "old line %07d of an earlier run" % (i - 1)
        before = os.path.getsize(env.log)
        if not check(env.start_daemon([A], extra={"WALPE_LOG_MAX_MB": "1"}, log_append=append),
                     f"logcap: daemon started, log {how}"):
            return
        st = env.state()        # (answered: the start-up lines are written)
        time.sleep(0.5)
        text = open(env.log, errors="replace").read()
        size = os.path.getsize(env.log)
        check(size < limit + 65536 and "log passed its size limit" in text,
              f"logcap: {how}, {before} bytes at the start: cut back to the limit when the daemon starts",
              f"{size} bytes")
        check(last_old in text and "old line 0000000 " not in text,
              f"logcap: {how}: the newest old lines are kept, the oldest are gone")
        check("monitor 0" in text, f"logcap: {how}: this run's lines follow", text[-300:])
        # more lines of this run: they land at the end of the shorter file
        # (a descriptor that kept its offset would bring the old length back)
        for _ in range(3):
            env.ipc({"cmd": "set", "fps": 31})
            env.ipc({"cmd": "set", "fps": 30})
        time.sleep(0.3)
        stt = os.stat(env.log)
        check(stt.st_size < limit + 65536 and stt.st_blocks * 512 < limit + 131072,
              f"logcap: {how}: the file stays that size", f"{stt.st_size} bytes, {stt.st_blocks * 512} on disk")
        check(bool(st) and env.daemon.poll() is None, f"logcap: {how}: the daemon runs")
        env.stop_daemon()


# -------------------------------------------------------------------- panel
def case_panel(env, W):
    """tests/perf_panel_test.c needs a display: it gets this check's"""
    exe = os.path.join(env.build, "perf-panel-test")
    if not os.path.exists(exe):
        print("lifecheck: panel skipped (no perf-panel-test in the build)")
        return
    e = dict(env.env)
    e.update({"GSK_RENDERER": "cairo", "GTK_A11Y": "none"})
    try:
        p = subprocess.run([exe], env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=env.tmp,
                           timeout=120 * PATIENCE)
        out, status = p.stdout.decode(errors="replace"), p.returncode
    except subprocess.TimeoutExpired:
        out, status = "", "no end in time"
    lines = [ln for ln in out.splitlines() if ln.startswith(("ok  ", "FAIL"))]
    for ln in lines:
        check(ln.startswith("ok  "), "panel: " + ln[5:])
    check(status == 0 and bool(lines), "panel: perf-panel-test ended well", f"exit status {status}\n{out[-600:]}")


# --------------------------------------------------------------------- scan
def case_scan(env, W):
    """The scan of the client windows (X11): after every change of a window's
    place, of the focus or of a client's state it asked the server about
    every client anew - attributes, root position, type, state, WM_STATE:
    five round trips per window on a monitor. It now keeps the answer made
    of the three properties while the server reports no change of them and
    takes the root position of a client without a frame from its geometry."""
    if env.display.winsys != "x11":
        print("lifecheck: scan skipped (the scan of windows is the X11 backend's)")
        return
    A, B = ("A", 640, 360, 0, 0), ("B", 640, 360, 640, 0)
    rc.jdump(env.config, {"config_version": 3, "fps": 30, "pause_covered": True, "pause_dpms": False,
                          "pause_fullscreen": True})
    if not check(env.start_daemon([A, B]), "scan: daemon started"):
        return
    env.open(W["red"], 0)
    env.open(W["blue"], 1)
    loaded(env, W["red"], 0)
    loaded(env, W["blue"], 1)
    xc = XClient(env.env["DISPLAY"])

    def flags(key):
        return [bool(m.get(key)) for m in env.monitors()]

    def becomes(key, want, what):
        got = wait_for(lambda: flags(key) == want, 8, 0.2)
        return check(bool(got), what, f"{key} {flags(key)}, expected {want}")

    def xreq():
        return env.state().get("x_requests") or 0

    try:
        big = xc.window(0, 0, 640, 360)             # all of monitor A
        small = [xc.window(660 + 30 * k, 20, 20, 60) for k in range(7)]    # slivers on B
        xc.clients([big] + small)
        becomes("covered", [True, False], "scan: a window over all of monitor A covers it")
        # the scan runs a few times with nothing new about the big window: its answer is a kept one
        for k in range(4):
            xc.move(small[0], 660, 20 + k, 20, 60)
            time.sleep(0.3)
        check(flags("covered") == [True, False], "scan: still covered after other windows moved")
        hidden, normal = xc.atom("_NET_WM_STATE_HIDDEN"), xc.atom("_NET_WM_WINDOW_TYPE_NORMAL")
        xc.prop(big, "_NET_WM_STATE", xc.XA_ATOM, [hidden])
        becomes("covered", [False, False], "scan: the window says it is hidden: not covered")
        xc.prop(big, "_NET_WM_STATE", xc.XA_ATOM, [])
        becomes("covered", [True, False], "scan: no longer hidden: covered")
        xc.prop(big, "WM_STATE", xc.atom("WM_STATE"), [3, 0])        # IconicState
        becomes("covered", [False, False], "scan: the window is iconic (WM_STATE): not covered")
        xc.prop(big, "WM_STATE", xc.atom("WM_STATE"), [1, 0])        # NormalState
        becomes("covered", [True, False], "scan: normal again: covered")
        xc.prop(big, "_NET_WM_WINDOW_TYPE", xc.XA_ATOM, [xc.atom("_NET_WM_WINDOW_TYPE_DOCK")])
        becomes("covered", [False, False], "scan: the window becomes a dock: not covered")
        xc.prop(big, "_NET_WM_WINDOW_TYPE", xc.XA_ATOM, [normal])
        becomes("covered", [True, False], "scan: a normal window again: covered")
        xc.prop(big, "_NET_WM_WINDOW_TYPE", xc.XA_ATOM, [xc.atom("_NET_WM_WINDOW_TYPE_DOCK")])
        becomes("covered", [False, False], "scan: a dock once more: not covered")
        xc.unprop(big, "_NET_WM_WINDOW_TYPE")
        becomes("covered", [True, False], "scan: the type taken away: covered")
        xc.move(big, 640, 0, 640, 360)
        becomes("covered", [False, True], "scan: the window moved to monitor B covers that one")
        xc.move(big, 0, 0, 200, 100)
        becomes("covered", [False, False], "scan: made small: nothing covered")

        # a window with a border, its own origin exactly on monitor B's first column: the
        # focused application is on B and not on A (the border is on A)
        xc.border(small[1], 3)
        xc.move(small[1], 637, 100, 50, 50)
        xc.focus(small[1])
        becomes("focused_app", [False, True], "scan: a focused window whose border alone is on A counts for B")
        xc.move(small[1], 636, 100, 50, 50)
        becomes("focused_app", [True, True], "scan: one pixel to the left it is on both")
        xc.focus(0)
        becomes("focused_app", [False, False], "scan: no window focused")

        # a client inside a frame window (a reparenting window manager): its place is the
        # frame's plus its own
        frame = xc.window(640, 0, 640, 360)
        inner = xc.window(0, 0, 640, 360, parent=frame)
        xc.clients([big] + small + [inner])
        becomes("covered", [False, True], "scan: a client inside a frame on monitor B covers B")
        xc.move(frame, 0, 0, 640, 360)
        becomes("covered", [True, False], "scan: its frame moved to A: A is covered")
        xc.destroy(inner)
        xc.destroy(frame)
        xc.clients([big] + small)
        becomes("covered", [False, False], "scan: the framed client gone: nothing covered")

        # what a moved window costs: 8 clients on the monitors, 20 moves, one scan each
        time.sleep(0.5)
        nwin, moves = 1 + len(small), 20
        before = xreq()
        for k in range(moves):
            xc.move(small[0], 660, 30 + k, 20, 60)
            time.sleep(0.25)
        time.sleep(0.5)
        used = xreq() - before
        # attributes (two requests) per client and a handful per scan; it was 6 per client + 3
        limit = moves * (3 * nwin + 8)
        check(used <= limit, f"scan: {moves} moves of one of {nwin} windows cost at most {limit} X requests",
              f"{used} requests = {used / moves:.1f} per move")
        check(flags("covered") == [False, False] and env.daemon.poll() is None,
              "scan: the daemon runs, nothing covered")
    finally:
        xc.close()
    env.stop_daemon()


CASES = [("logcap", case_logcap), ("pointer", case_pointer), ("reload", case_reload), ("restart", case_restart),
         ("layout", case_layout), ("stats", case_stats), ("push", case_push), ("scan", case_scan), ("tray", case_tray), ("window", case_window), ("panel", case_panel), ("busy", case_busy), ("probe", case_probe),
         ("fullscreen", case_fullscreen), ("video", case_video), ("webunload", case_webunload),
         ("winlost", case_winlost), ("convert", case_convert)]



def main():
    args = sys.argv[1:]
    build, daemon, keep, tmp, only, display = "build", None, False, None, None, None
    tray = window = None
    while args:
        a = args.pop(0)
        if a == "--build" and args:
            build = args.pop(0)
        elif a == "--daemon" and args:
            daemon = args.pop(0)
        elif a == "--tray" and args:
            tray = os.path.abspath(args.pop(0))
        elif a == "--window" and args:
            window = os.path.abspath(args.pop(0))
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
    env.tray_bin = tray
    env.window_bin = window
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
