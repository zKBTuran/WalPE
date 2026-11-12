#!/usr/bin/env python3
"""End-to-end checks of the daemon on Wayland: outputs that come, go and change.

  python3 -I scripts/waylandcheck.py --build build [--daemon FILE] [--only a,b] [--keep] [--tmp DIR]

A private headless compositor (sway on wlroots' headless backend, see
scripts/displayenv.py), a private walpe-daemon and generated scenes; nothing of
the user's is read or written. The pictures are screenshots of the
compositor's outputs (grim), so what is checked is what the compositor shows,
not what the daemon believes it drew. Each case starts its own daemon:

  start     the daemon comes up with only a Wayland display in its
            environment (no DISPLAY), lists the compositor's outputs by their
            names and rectangles, shows a four-colour scene the right way up
            and out to the output's corners, keeps a still picture still,
            refuses a second daemon, and leaves nothing on the output after
            `walpe-ctl quit`.
  layer     WALPE_WL_LAYER=bottom (where the daemon puts its wallpapers by
            itself on KDE Plasma's compositor): the surface is on the
            bottom layer, status says so and why, the wallpaper is shown;
            a value that is no layer is taken as "auto" and said so.
  two       two outputs of different sizes: a wallpaper for all, another for
            one output (saved under that output's name), each still on its
            own output after the outputs swap places.
  mode      the output's mode changes under the running daemon (640x360 ->
            800x450 -> 1280x720 -> 640x360): status follows, the picture
            fills the new size, the control socket stays connectable and the
            process id stays.
  scale     output scale 2 and 1.5: the picture is right and is drawn in the
            output's own pixels (a sharp colour edge), not enlarged from
            the scaled size.
  noviewport  the same scales with the compositor's viewporter ignored
            (WALPE_WL_IGNORE=wp_viewporter): the picture is right, nothing
            the compositor objects to.
  rotate    a turned output (transform 90): status follows, the picture is
            the right way up.
  hotplug   an output appears (it gets the wallpaper for all), gets a
            wallpaper of its own, is disabled and enabled again (its own
            wallpaper is back), is unplugged; then every output is gone and
            one returns. The daemon lives through all of it.
  pointer   a scene whose colour follows the pointer follows a pointer
            device that moves over the wallpaper, on the output it is on,
            at the right place also on an output with scale 1.5.
  covered   the output is switched off (`output ... power off`), then a
            window fills it (the library window `walpe`, made fullscreen):
            status says the wallpaper is not shown, an animated scene stops
            drawing, the daemon still answers, loads another wallpaper and
            quits at once; all comes back when the output is on again or
            the window is gone.
  fullscreen  "Pause for fullscreen apps" per monitor: two outputs, the
            library window `walpe` made fullscreen on one of them. The
            wallpaper of that output pauses and the other one keeps
            drawing; moved to the other output it is the other way round;
            nothing pauses for it while the focus is elsewhere; `set
            fs_all=on` pauses both, as earlier versions did. Status names
            the output per monitor and has no note about every monitor
            pausing (the compositor names the window's output).
  backend   (builds with both backends) with an X server and a Wayland
            compositor both in the environment: without a setting Wayland
            is taken, config.json "backend" picks the other, WALPE_BACKEND
            beats the config, status names the choice and where it came
            from, and the same scene is the same picture, byte for byte,
            on both.
  noshell   a compositor without the layer shell (weston when installed,
            else sway with the layer shell ignored through
            WALPE_WL_IGNORE): the Wayland backend asked for by name ends
            with status 1 and says why; without a setting the daemon ends
            the same way also with an X server there (in a session that
            is Xwayland, where the wallpaper would lie above every
            window) and says so; `--backend x11` takes X11 there.
  lost      the compositor goes away: the daemon ends with status 1 and
            leaves no socket behind.
  cycles    (only with --only cycles) an output is created and unplugged
            six times: the daemon's open files do not grow from the second
            round to the last.

Exit code 0 = all passed, 1 = failures, 77 = skipped (sway or grim is not
installed, or the build has no Wayland backend). WAYLANDCHECK_PATIENCE=N
multiplies every wait (a software renderer on a busy machine: 3 or more).
--tmp DIR: work in DIR (must not exist; removed afterwards); sockets live
below it and a socket path is limited to 107 bytes."""
import os, re, shutil, socket, subprocess, sys, time

sys.dont_write_bytecode = True   # no __pycache__ next to the scripts
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("LIFECHECK_PATIENCE", os.environ.get("WAYLANDCHECK_PATIENCE", "1") or "1")
import displayenv        # noqa: E402
import lifecheck as lc   # noqa: E402  (its Env, scene generators and waits)

rc = lc.rc
PATIENCE = lc.PATIENCE
CONFIG = {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False}
# how a backend is asked for: a config.json member, and a variable that beats it
BACKEND_KEY, BACKEND_ENV = "backend", "WALPE_BACKEND"
QUAD = ["red", "green", "blue", "white"]     # top left, top right, bottom left, bottom right
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


def gen_quad(out):
    """four flat quarters, each its own colour (scene coordinates: y up): a picture that
    is wrong when it is mirrored, turned, shifted or drawn at another size"""
    rc.jdump(f"{out}/models/util/solidlayer.json", {"solidlayer": True})
    objs = []
    for i, (x, y, col) in enumerate(((480, 810, "1 0 0"), (1440, 810, "0 1 0"),
                                     (480, 270, "0 0 1"), (1440, 270, "1 1 1"))):
        objs.append({"id": 50 + i, "name": "q%d" % i, "image": "models/util/solidlayer.json",
                     "origin": f"{x} {y} 0", "size": "960 540", "scale": "1 1 1", "angles": "0 0 0",
                     "color": col, "alpha": 1.0, "visible": True})
    rc.jdump(f"{out}/scene.json", {
        "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
        "general": {"ambientcolor": "0.3 0.3 0.3", "bloom": False, "cameraparallax": False,
                    "clearcolor": "0 0 0", "orthogonalprojection": {"height": 1080, "width": 1920},
                    "skylightcolor": "0.3 0.3 0.3"},
        "objects": objs})
    rc.jdump(f"{out}/project.json", {"file": "scene.json", "title": "quad", "type": "scene"})


def tone(rgb):
    if rgb == displayenv.NO_PICTURE:
        return "none"
    hi, lo = [c > 150 for c in rgb], [c < 90 for c in rgb]
    for name, want in (("black", (0, 0, 0)), ("white", (1, 1, 1)), ("red", (1, 0, 0)),
                       ("green", (0, 1, 0)), ("blue", (0, 0, 1))):
        if all(hi[i] if want[i] else lo[i] for i in range(3)):
            return name
    return "rgb(%d,%d,%d)" % tuple(rgb)


def look(env, name):
    """a screenshot of an output read as the four-colour scene: the tones around the
    middle, the tones in the corners, the picture's size and how many pixels the
    red/green edge of the top half takes"""
    f = env.display.shot(name)
    if not f:
        return {"middle": ["none"] * 4, "corners": ["none"] * 4, "size": (0, 0), "edge": -1, "hash": "none"}
    w, h, _ = f
    d = max(4, min(w, h) // 10)
    px = displayenv.pixel_of
    mid = [tone(px(f, w // 2 + sx * d, h // 2 + sy * d)) for sy in (-1, 1) for sx in (-1, 1)]
    cor = [tone(px(f, x, y)) for y in (3, h - 4) for x in (3, w - 4)]
    row = [tone(px(f, x, h // 4)) for x in range(max(0, w // 2 - 40), min(w, w // 2 + 40))]
    edge = sum(1 for t in row if t not in ("red", "green"))
    return {"middle": mid, "corners": cor, "size": (w, h), "edge": edge, "hash": displayenv.frame_hash(f)}


def shows_quad(env, name, corners=True, size=None, timeout=20):
    """wait until the output shows the four-colour scene (also in its corners, also at
    this size in pixels); -> (it does, the last look)"""
    box = [None]

    def ok():
        box[0] = lk = look(env, name)
        return (lk["middle"] == QUAD and (not corners or lk["corners"] == QUAD) and
                (size is None or lk["size"] == tuple(size)))
    return bool(wait_for(ok, timeout, 0.3)), box[0]


def shows(env, name, want, timeout=15):
    """wait until the middle of an output has this tone; -> the tone it has"""
    def mid():
        f = env.display.shot(name)
        return tone(displayenv.pixel_of(f, f[0] // 2, f[1] // 2)) if f else "none"
    wait_for(lambda: mid() == want, timeout, 0.3)
    return mid()


def rects(env):
    """status: {name: [x, y, w, h]} of the daemon's monitors ({} while it does not answer)"""
    return {m.get("name"): list(m.get("geometry") or []) for m in env.state().get("monitors") or []}


def index_of(env, name):
    for m in env.monitors():
        if m.get("name") == name:
            return m.get("index")
    return None


def follow(env, done, timeout=30):
    """wait for done() while watching the control socket; -> (done, failed connects)"""
    t0, bad, ok = time.time(), 0, False
    while time.time() - t0 < timeout * PATIENCE and not ok:
        bad += 0 if lc.can_connect(env.sock) else 1
        ok = bool(done())
        time.sleep(0.05)
    return ok, bad


TROUBLE = re.compile(r"CRITICAL \*\*|@\d+: error \d+:|protocol error|Segmentation fault|AddressSanitizer|"
                     r"ThreadSanitizer|runtime error:")


def healthy(env, mark, what):
    """the daemon runs and its log has no critical warning, protocol error or sanitizer report since mark"""
    bad = [ln for ln in env.since(mark).splitlines() if TROUBLE.search(ln)]
    return check(env.daemon is not None and env.daemon.poll() is None and not bad,
                 what + ": the daemon runs, its log is clean", bad[:5])


def start(env, mons, what, extra=None, config=None):
    rc.jdump(env.config, dict(CONFIG, **(config or {})))
    return check(env.start_daemon(mons, extra=extra), what + ": daemon started on the compositor's outputs",
                 env.since(0)[-1500:])


# (libwayland writes object@id up to 1.22 and object#id from 1.23 on)
WIRE_LAYER = re.compile(r'-> zwlr_layer_shell_v1[@#]\d+\.get_layer_surface\(new id zwlr_layer_surface_v1[@#](\d+), '
                        r'wl_surface[@#]\d+, (?:wl_output[@#]\d+|nil), (\d+), "([^"]*)"\)')
WIRE_CONFIGURE = re.compile(r'zwlr_layer_surface_v1[@#](\d+)\.configure\(\d+, (\d+), (\d+)\)')
WIRE_DESTROY = re.compile(r'-> zwlr_layer_surface_v1[@#](\d+)\.destroy\(\)')
LAYER_NAMES = ("background", "bottom", "top", "overlay")


def wire_layers(log):
    """the layer-shell surfaces a client has, from its side of the wire (the lines
    libwayland prints with WAYLAND_DEBUG=1): what it asked for and the size the
    compositor configured last -> [{namespace, layer, width, height}], in the form of
    displayenv's layers (). For a compositor that does not list them itself."""
    have = {}
    for ln in log.splitlines():
        m = WIRE_LAYER.search(ln)
        if m:
            layer = int(m.group(2))
            have[m.group(1)] = {"namespace": m.group(3), "width": None, "height": None,
                                "layer": LAYER_NAMES[layer] if layer < len(LAYER_NAMES) else str(layer)}
            continue
        m = WIRE_CONFIGURE.search(ln)
        if m and m.group(1) in have:
            have[m.group(1)].update(width=int(m.group(2)), height=int(m.group(3)))
            continue
        m = WIRE_DESTROY.search(ln)
        if m:
            have.pop(m.group(1), None)
    return list(have.values())


# -------------------------------------------------------------------- start
def case_start(env, W):
    # who can say which layer surfaces are on an output: the compositor (sway up to 1.8),
    # else the daemon's side of the wire, which libwayland then prints into its log
    listed = env.display.layers(env.display.outputs()[0]["name"]) is not None
    if not start(env, [("A", 640, 360, 0, 0)], "start", extra=None if listed else {"WAYLAND_DEBUG": "1"}):
        return
    mark = env.mark()
    name = env.display.outputs()[0]["name"]
    check("DISPLAY" not in env.run_env and bool(env.run_env.get("WAYLAND_DISPLAY")),
          "start: the daemon was given a Wayland display and no X display")
    check(rects(env) == {name: [0, 0, 640, 360]}, "start: status lists the output by its name and rectangle",
          (rects(env), name))
    st = env.state()
    check(st.get("display_backend") == "wayland", "start: status says the display backend is Wayland",
          {k: v for k, v in st.items() if k.startswith("display_backend")})
    check("display backend: wayland" in env.since(0), "start: the log's first lines name the backend")
    ls = env.display.layers(name) if listed else wire_layers(env.since(0))
    check(len(ls) == 1 and ls[0]["layer"] == "background" and (ls[0]["width"], ls[0]["height"]) == (640, 360),
          "start: one layer-shell surface on the output: background layer, the output's size", ls)
    check(bool(ls) and ls[0]["namespace"] == "walpe", "start: its namespace is walpe", ls)
    check((st.get("wl_layer"), st.get("wl_layer_why")) == ("background", "default"),
          "start: status wl_layer: background, by default (this compositor is not KWin)",
          (st.get("wl_layer"), st.get("wl_layer_why")))
    sf = st.get("wl_surfaces") or [{}]
    check(len(sf) == 1 and sf[0].get("role") == "ready" and sf[0].get("buffer") == [640, 360],
          "start: status wl_surfaces: one surface, ready, buffer 640x360", sf)
    # what this compositor lets the daemon know (for the reader; no rule here says which must work)
    sig = st.get("pause_signals") or {}
    note("start: pause signals and who reports them: " +
         (", ".join("%s <- %s" % (k, (v or {}).get("from") or "nobody") for k, v in sorted(sig.items())) or "none"))
    note("start: compositor globals: %s" % st.get("wl_compositor_globals"))
    env.open(W["quad"])
    check(lc.loaded(env, W["quad"]), "start: the four-colour scene is loaded", rc.brief(env.monitors()))
    ok, lk = shows_quad(env, name, size=(640, 360))
    check(ok, "start: the picture is the right way up and reaches the output's corners", lk)
    time.sleep(1.0 * PATIENCE)
    again = look(env, name)
    check(again["hash"] == lk["hash"] and lk["hash"] != "none", "start: a still scene is the same picture a second later",
          (lk["hash"], again["hash"]))
    p = subprocess.run([env.daemon_bin], env=env.run_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=60 * PATIENCE)
    check(p.returncode != 0 and b"already running" in p.stdout, "start: a second daemon is refused",
          p.stdout[-300:])
    healthy(env, mark, "start")
    pid = env.daemon.pid
    env.ctl("quit")
    gone = wait_for(lambda: env.daemon.poll() is not None, 10, 0.1)
    check(bool(gone), "start: walpe-ctl quit ends the daemon", pid)
    if gone:
        env.daemon = None
    time.sleep(0.5 * PATIENCE)
    after = look(env, name)
    check(after["middle"] != QUAD and after["size"] == (640, 360),
          "start: after quit the output no longer shows the wallpaper (the compositor is still there)", after)


# -------------------------------------------------------------------- layer
def case_layer(env, W):
    listed = env.display.layers(env.display.outputs()[0]["name"]) is not None
    extra = {} if listed else {"WAYLAND_DEBUG": "1"}
    begun = env.mark()      # the log goes on from the cases before
    if not start(env, [("A", 640, 360, 0, 0)], "layer", extra=dict(extra, WALPE_WL_LAYER="bottom")):
        return
    mark = env.mark()
    name = env.display.outputs()[0]["name"]
    st = env.state()
    ls = env.display.layers(name) if listed else wire_layers(env.since(begun))
    check(len(ls) == 1 and ls[0]["layer"] == "bottom" and ls[0]["namespace"] == "walpe" and
          (ls[0]["width"], ls[0]["height"]) == (640, 360),
          "layer: WALPE_WL_LAYER=bottom: one layer-shell surface, on the bottom layer, the output's size", ls)
    check((st.get("wl_layer"), st.get("wl_layer_why")) == ("bottom", "WALPE_WL_LAYER"),
          "layer: status wl_layer says bottom and where it came from", (st.get("wl_layer"), st.get("wl_layer_why")))
    check('WALPE_WL_LAYER: the wallpaper surfaces go on the layer "bottom"' in env.since(begun),
          "layer: the log says it")
    env.open(W["quad"])
    check(lc.loaded(env, W["quad"]), "layer: the four-colour scene is loaded", rc.brief(env.monitors()))
    ok, lk = shows_quad(env, name, size=(640, 360))
    check(ok, "layer: the wallpaper is shown on the bottom layer", lk)
    healthy(env, mark, "layer")
    env.stop_daemon()
    begun = env.mark()
    if not start(env, [("A", 640, 360, 0, 0)], "layer", extra=dict(extra, WALPE_WL_LAYER="overlay")):
        return
    st = env.state()
    ls = env.display.layers(name) if listed else wire_layers(env.since(begun))
    check(len(ls) == 1 and ls[0]["layer"] == "background" and
          (st.get("wl_layer"), st.get("wl_layer_why")) == ("background", "default"),
          "layer: WALPE_WL_LAYER=overlay is no layer for a wallpaper: background, as without it",
          (ls, st.get("wl_layer"), st.get("wl_layer_why")))
    check("WALPE_WL_LAYER=overlay: not a layer this program uses" in env.since(begun), "layer: and the log says so")
    env.stop_daemon()
    # The add-on for KDE Plasma (contrib/plasma) is the business of KWin and of Plasma's shell.
    # Its files in this user's directories, saying "installed and selected", change nothing
    # where the compositor is not KWin: the background layer, and no word about Plasma.
    data = env.run_env.get("XDG_DATA_HOME") or os.path.join(env.run_env["HOME"], ".local", "share")
    conf = env.run_env.get("XDG_CONFIG_HOME") or os.path.join(env.run_env["HOME"], ".config")
    pkg = os.path.join(data, "plasma", "wallpapers", "io.github.walpe.plasma")
    applets = os.path.join(conf, "plasma-org.kde.plasma.desktop-appletsrc")
    os.makedirs(pkg, exist_ok=True)
    os.makedirs(conf, exist_ok=True)
    with open(os.path.join(pkg, "metadata.json"), "w") as f:
        f.write('{"KPlugin": {"Id": "io.github.walpe.plasma"}}\n')
    with open(applets, "w") as f:
        f.write("[Containments][1]\nwallpaperplugin=io.github.walpe.plasma\n")
    begun = env.mark()
    try:
        if not start(env, [("A", 640, 360, 0, 0)], "layer", extra=extra):
            return
        st = env.state()
        ls = env.display.layers(name) if listed else wire_layers(env.since(begun))
        check(len(ls) == 1 and ls[0]["layer"] == "background" and
              (st.get("wl_layer"), st.get("wl_layer_why")) == ("background", "default"),
              "layer: the Plasma add-on's files in the user's directories: still the background layer here",
              (ls, st.get("wl_layer"), st.get("wl_layer_why")))
        plasma = sorted(k for k in st if k.startswith("wl_plasma"))
        check(not plasma and "KDE Plasma" not in env.since(begun),
              "layer: and neither status nor the log speaks of Plasma on another compositor", plasma)
        env.stop_daemon()
    finally:
        os.remove(applets)
        shutil.rmtree(os.path.join(data, "plasma"), ignore_errors=True)


# ---------------------------------------------------------------------- two
def case_two(env, W):
    if not start(env, [("A", 640, 360, 0, 0), ("B", 480, 270, 640, 0)], "two"):
        return
    mark = env.mark()
    (a, *_), (b, *_) = env.mons
    outs = {o["name"]: o for o in env.display.outputs()}
    check(set(rects(env)) == set(outs) and len(outs) == 2, "two: the daemon's monitor names are the outputs' names",
          (rects(env), sorted(outs)))
    env.open(W["red"])
    lc.loaded(env, W["red"])
    got = [shows(env, a, "red"), shows(env, b, "red")]
    check(got == ["red", "red"], "two: the wallpaper for all is on both outputs", got)
    env.open(W["blue"], 1)
    lc.loaded(env, W["blue"], 1)
    got = [shows(env, a, "red"), shows(env, b, "blue")]
    check(got == ["red", "blue"], "two: the second output has its own wallpaper", got)
    cur, own = env.saved()
    check(cur == W["red"] and own == {b: W["blue"]}, "two: config.json saves it under the output's name", (cur, own))
    # the outputs swap places: B to the left, A to its right
    env.display.output(b, pos=(0, 0))
    env.display.output(a, pos=(480, 0))
    ok, bad = follow(env, lambda: rects(env) == {a: [480, 0, 640, 360], b: [0, 0, 480, 270]})
    check(ok, "two: status follows the outputs to their new places", rects(env))
    check(bad == 0, "two: the control socket stayed connectable meanwhile", f"{bad} failed connects")
    got = [shows(env, a, "red"), shows(env, b, "blue")]
    check(got == ["red", "blue"], "two: each output still shows its own wallpaper", got)
    healthy(env, mark, "two")
    env.stop_daemon()


# --------------------------------------------------------------------- mode
def case_mode(env, W):
    if not start(env, [("A", 640, 360, 0, 0)], "mode"):
        return
    mark = env.mark()
    name, pid = env.mons[0][0], env.daemon.pid
    env.open(W["quad"])
    lc.loaded(env, W["quad"])
    ok, lk = shows_quad(env, name, size=(640, 360))
    check(ok, "mode: the scene at 640x360", lk)
    for w, h in ((800, 450), (1280, 720), (640, 360)):
        check(env.display.output(name, mode=(w, h)), f"mode: the compositor takes mode {w}x{h}")
        ok, bad = follow(env, lambda: rects(env) == {name: [0, 0, w, h]})
        check(ok, f"mode: status follows to {w}x{h}", rects(env))
        check(bad == 0, f"mode: the control socket stayed connectable during the change to {w}x{h}",
              f"{bad} failed connects")
        ok, lk = shows_quad(env, name, size=(w, h))
        check(ok, f"mode: the picture fills {w}x{h} (corners and middle)", lk)
        check(0 <= lk["edge"] <= 1, f"mode: drawn at {w}x{h}, not enlarged from the size before (a sharp edge)",
              f"the red/green edge takes {lk['edge']} pixels")
        check(env.daemon.poll() is None and env.daemon.pid == pid and os.path.exists(f"/proc/{pid}"),
              f"mode: same process after the change to {w}x{h}")
        check(lc.loaded(env, W["quad"], timeout=20), "mode: the wallpaper is the one from before",
              rc.brief(env.monitors()))
    healthy(env, mark, "mode")
    env.stop_daemon()


# -------------------------------------------------------------------- scale
def scales(env, W, what, extra, edge):
    if not start(env, [("A", 640, 360, 0, 0)], what, extra=extra):
        return
    mark = env.mark()
    name = env.mons[0][0]
    env.open(W["quad"])
    lc.loaded(env, W["quad"])
    shows_quad(env, name, size=(640, 360))
    for scale, (w, h) in ((2, (1280, 720)), (1.5, (960, 540)), (1, (640, 360))):
        check(env.display.output(name, mode=(w, h), scale=scale),
              f"{what}: the compositor takes {w}x{h} at scale {scale}")
        # (the output is 640x360 in the layout each time; its picture has w x h pixels)
        # What the daemon lists is what it renders: the mode with a viewport; without one a
        # buffer is a whole multiple of the surface, the next whole scale (the compositor
        # reduces it to the mode)
        up = -(-scale // 1)
        lw, lh = (w, h) if extra is None else (int(640 * up), int(360 * up))
        ok, _ = follow(env, lambda: rects(env).get(name, [])[2:] == [lw, lh], 30)
        check(ok, f"{what} {scale}: status lists the output with " +
              (f"its {w}x{h} pixels" if (lw, lh) == (w, h) else
               f"the {lw}x{lh} pixels rendered for it (640x360 at the next whole scale)"), rects(env))
        ok, lk = shows_quad(env, name, size=(w, h))
        check(ok, f"{what} {scale}: the picture is right on all {w}x{h} pixels", lk)
        check(0 <= lk["edge"] <= edge(scale), f"{what} {scale}: the colour edge is sharp enough "
              f"(at most {edge(scale)} pixels)", f"the red/green edge takes {lk['edge']} pixels")
        sf = (env.state().get("wl_surfaces") or [{}])[0]
        note(f"{what} {scale}: wl_surfaces[0] buffer {sf.get('buffer')} logical {sf.get('logical')} "
             f"viewport {sf.get('viewport')} scale120 {sf.get('scale120')}")
        if extra is None:
            check(sf.get("buffer") == [w, h] and sf.get("logical") == [640, 360],
                  f"{what} {scale}: the surface's buffer is the mode, its logical size 640x360", sf)
        else:
            check(sf.get("viewport") is False, f"{what} {scale}: no viewport is used", sf)
    healthy(env, mark, what)
    env.stop_daemon()


def case_scale(env, W):
    """drawn in the output's own pixels: the edge between two colours is one pixel"""
    scales(env, W, "scale", None, lambda s: 1 if s == int(s) else 2)


def case_noviewport(env, W):
    """without wp_viewporter a fractional scale is drawn larger and reduced by the
    compositor: the edge may be a few pixels of mixed colour; nothing else may differ"""
    scales(env, W, "noviewport", {"WALPE_WL_IGNORE": "wp_viewporter"}, lambda s: 1 if s == int(s) else 4)


# ------------------------------------------------------------------- rotate
def case_rotate(env, W):
    if not start(env, [("A", 640, 360, 0, 0)], "rotate"):
        return
    mark = env.mark()
    name = env.mons[0][0]
    env.open(W["quad"])
    lc.loaded(env, W["quad"])
    shows_quad(env, name, size=(640, 360))
    check(env.display.output(name, transform="90"), "rotate: the compositor turns the output")
    ok, bad = follow(env, lambda: rects(env) == {name: [0, 0, 360, 640]})
    check(ok, "rotate: status follows: the output is 360x640 now", rects(env))
    # the screenshot is taken the way the output is looked at; the scene (16:9) does not fill a
    # 9:16 output edge to edge the same way under every fit, so only the middle is read
    ok, lk = shows_quad(env, name, corners=False)
    check(ok, "rotate: the picture is the right way up on the turned output", lk)
    check(env.display.output(name, transform="normal"), "rotate: the compositor turns it back")
    ok, _ = follow(env, lambda: rects(env) == {name: [0, 0, 640, 360]})
    ok2, lk = shows_quad(env, name, size=(640, 360))
    check(ok and ok2, "rotate: back to 640x360, picture right", (rects(env), lk))
    healthy(env, mark, "rotate")
    env.stop_daemon()


# ------------------------------------------------------------------ hotplug
def case_hotplug(env, W):
    if not start(env, [("A", 640, 360, 0, 0)], "hotplug"):
        return
    mark = env.mark()
    a, pid = env.mons[0][0], env.daemon.pid
    env.open(W["red"])
    lc.loaded(env, W["red"])
    check(shows(env, a, "red") == "red", "hotplug: red on the first output")

    b = env.display.add_output(mode=(480, 270), pos=(640, 0))
    if not check(bool(b), "hotplug: the compositor makes a second output"):
        env.stop_daemon()
        return
    ok, bad = follow(env, lambda: rects(env) == {a: [0, 0, 640, 360], b: [640, 0, 480, 270]})
    check(ok, "hotplug: status lists the new output", rects(env))
    check(bad == 0, "hotplug: the control socket stayed connectable while it appeared", f"{bad} failed connects")
    check(shows(env, b, "red") == "red", "hotplug: the new output gets the wallpaper for all")
    bi = index_of(env, b)
    env.open(W["blue"], bi)
    lc.loaded(env, W["blue"], bi)
    got = [shows(env, a, "red"), shows(env, b, "blue")]
    check(got == ["red", "blue"], "hotplug: the new output takes a wallpaper of its own", got)

    # disabled and enabled again: the same output (same name) returns to its own wallpaper
    check(env.display.output(b, enable=False), "hotplug: the compositor disables the second output")
    ok, bad = follow(env, lambda: rects(env) == {a: [0, 0, 640, 360]})
    check(ok, "hotplug: status drops the disabled output", rects(env))
    check(shows(env, a, "red") == "red", "hotplug: the first output is not disturbed by it")
    check(env.display.output(b, enable=True, mode=(480, 270), pos=(640, 0)), "hotplug: ... and enables it again")
    ok, bad2 = follow(env, lambda: set(rects(env)) == {a, b})
    check(ok, "hotplug: status lists it again", rects(env))
    got = [shows(env, a, "red"), shows(env, b, "blue")]
    check(got == ["red", "blue"], "hotplug: its own wallpaper is back on it", got)
    check(bad + bad2 == 0, "hotplug: the control socket stayed connectable through both", f"{bad + bad2} failed connects")

    # unplugged for good
    check(env.display.remove_output(b), "hotplug: the compositor unplugs the second output")
    ok, _ = follow(env, lambda: rects(env) == {a: [0, 0, 640, 360]})
    check(ok and shows(env, a, "red") == "red", "hotplug: one output left, still red", rects(env))
    cur, own = env.saved()
    check(cur == W["red"] and own.get(b) == W["blue"],
          "hotplug: config.json keeps the unplugged output's own choice for its return", (cur, own))

    # every output gone, then one back
    # (with no output there is nothing to restart for: the daemon keeps the monitors it
    # was started with until one is back; their surfaces are gone, nothing is drawn)
    check(env.display.output(a, enable=False), "hotplug: the compositor disables the last output")

    def none_shown():
        st = env.state()
        return {"monitors": [bool(m.get("hidden")) for m in st.get("monitors") or []],
                "surfaces": [sf.get("role") for sf in st.get("wl_surfaces") or []]}
    ok, _ = follow(env, lambda: none_shown() == {"monitors": [True], "surfaces": ["dead"]})
    check(ok, "hotplug: the daemon answers with no output left: its monitor is not shown, the surface is gone",
          none_shown())
    time.sleep(1.0 * PATIENCE)
    check(env.daemon.poll() is None, "hotplug: ... and keeps running")
    check(env.display.output(a, enable=True, mode=(640, 360), pos=(0, 0)), "hotplug: the output returns")
    ok, _ = follow(env, lambda: rects(env) == {a: [0, 0, 640, 360]})
    check(ok and shows(env, a, "red") == "red", "hotplug: the wallpaper is back on the returned output", rects(env))
    check(env.daemon.poll() is None and env.daemon.pid == pid, "hotplug: same process throughout")
    healthy(env, mark, "hotplug")
    env.stop_daemon()


# ------------------------------------------------------------------ pointer
def case_pointer(env, W):
    """The scene is blue where the pointer is at the left edge, red at the right edge
    and purple in between or without a pointer."""
    if not env.display.can_point():
        print("skip pointer (the compositor gives no virtual pointer device)")
        return
    if not start(env, [("A", 640, 360, 0, 0), ("B", 640, 360, 640, 0)], "pointer", config={"parallax": True}):
        return
    mark = env.mark()
    (a, *_), (b, *_) = env.mons

    def colour(name, want):
        box = [None]

        def ok():
            f = env.display.shot(name)
            box[0] = rc.name_of(displayenv.pixel_of(f, f[0] // 2, f[1] // 2)) if f else "none"
            return box[0] == want
        wait_for(ok, 6, 0.2)
        return box[0]
    note("pointer: moved by " + env.display.pointer_how)
    env.open(W["ptr"])
    lc.loaded(env, W["ptr"])
    check(env.display.pointer(4, 100), "pointer: the pointer moves to the left edge of the first output")
    got = colour(a, "blue")
    check(got == "blue", "pointer: the scene on that output follows it there (blue)", got)
    check(env.display.pointer(636, 100), "pointer: ... and to its right edge")
    got = colour(a, "red")
    check(got == "red", "pointer: the scene follows (red)", got)
    check(env.display.pointer(640 + 4, 100), "pointer: the pointer moves to the left edge of the second output")
    got = colour(b, "blue")
    check(got == "blue", "pointer: the scene on the second output follows it (blue)", got)
    # after a load the scene still knows where the pointer is (it has not moved since)
    env.open(W["red"])
    lc.loaded(env, W["red"])
    env.open(W["ptr"])
    lc.loaded(env, W["ptr"])
    got = colour(b, "blue")
    check(got == "blue", "pointer: after another load the scene still has the pointer's place", got)
    note("pointer: status wl_seat_pointer = %s" % env.state().get("wl_seat_pointer"))

    def red_at(name, x, y, want):
        """the pointer to (x, y) of the layout; -> the red the scene on `name` shows (0..255)"""
        env.display.pointer(x, y)
        box = [(-1, -1, -1)]

        def ok():
            f = env.display.shot(name)
            box[0] = displayenv.pixel_of(f, f[0] // 2, f[1] // 2) if f else displayenv.NO_PICTURE
            return abs(box[0][0] - want) <= 16
        wait_for(ok, 6, 0.2)
        return box[0][0]
    # the scene's red is the pointer's x as a fraction of the monitor: a quarter and three quarters
    for frac in (0.25, 0.75):
        got = red_at(a, 640 * frac, 180, 255 * frac)
        check(abs(got - 255 * frac) <= 16, f"pointer: at {frac} of the output's width the scene has that place",
              f"red {got}, expected about {255 * frac:.0f}")
    # the same on an output with scale 1.5 (960x540 pixels shown as 640x360)
    if check(env.display.output(a, mode=(960, 540), scale=1.5), "pointer: the first output gets scale 1.5"):
        ok, _ = follow(env, lambda: rects(env).get(a, [])[2:] == [960, 540], 40)
        check(ok, "pointer: status follows", rects(env))
        lc.loaded(env, W["ptr"], timeout=30)
        for frac in (0.25, 0.75):
            got = red_at(a, 640 * frac, 180, 255 * frac)
            check(abs(got - 255 * frac) <= 16, f"pointer: at scale 1.5, {frac} of the width is {frac} for the scene",
                  f"red {got}, expected about {255 * frac:.0f}")
    healthy(env, mark, "pointer")
    env.stop_daemon()


# ------------------------------------------------------------------ covered
def case_covered(env, W):
    gui = os.path.join(env.build, "walpe")
    if not os.path.exists(gui):
        print("skip covered (no library window program in the build)")
        return
    if not start(env, [("A", 640, 360, 0, 0)], "covered", config={"pause_covered": True}):
        return
    mark = env.mark()
    a = env.mons[0][0]

    def mon():
        return (env.state().get("monitors") or [{}])[0]

    def hidden():
        return bool(mon().get("hidden") or mon().get("covered"))

    def drawn(seconds=2.0):
        n = mon().get("frames") or 0
        time.sleep(seconds * PATIENCE)
        return (mon().get("frames") or 0) - n
    env.open(W["anim"])
    lc.loaded(env, W["anim"])
    wait_for(lambda: drawn(1.0) > 3, 20, 0.1)
    check(drawn() > 3 and not hidden(), "covered: an animated scene draws while nothing is over it",
          {k: mon().get(k) for k in ("frames", "hidden", "covered", "paused")})

    # the output switched off: no frame is shown, the compositor asks for none
    if check(env.display.power(a, False), "covered: the compositor switches the output off"):
        check(bool(wait_for(hidden, 20, 0.2)), "covered: status says the wallpaper is not shown",
              {k: mon().get(k) for k in ("hidden", "covered", "paused", "wait_timeouts")})
        note("covered: reported by: %s" % ((env.state().get("pause_signals") or {}).get("covered") or {}).get("from"))
        n = drawn(3.0)
        check(n <= 2, "covered: the animated scene stops drawing while the output is off", f"{n} frames in 3 s")
        t0 = time.time()
        st = env.ipc({"cmd": "status"}, timeout=5.0)
        check(bool(st) and time.time() - t0 < 1.0 * PATIENCE, "covered: the daemon answers at once meanwhile",
              f"{time.time() - t0:.2f} s")
        env.open(W["red"])
        check(lc.loaded(env, W["red"], timeout=20), "covered: another wallpaper loads while the output is off",
              rc.brief(env.monitors()))
        check(env.display.power(a, True), "covered: the output is switched on again")
        check(bool(wait_for(lambda: not hidden(), 20, 0.2)), "covered: shown again",
              {k: mon().get(k) for k in ("hidden", "covered", "paused")})
        check(shows(env, a, "red") == "red", "covered: the wallpaper loaded meanwhile is on the output")
        env.open(W["anim"])
        lc.loaded(env, W["anim"])
        wait_for(lambda: drawn(1.0) > 3, 20, 0.1)
    genv = dict(env.run_env)
    genv.update({"GDK_BACKEND": "wayland", "GSK_RENDERER": "cairo", "GTK_A11Y": "none"})
    with open(os.path.join(env.tmp, "gui.log"), "ab") as log:
        p = subprocess.Popen([gui], env=genv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                             cwd=env.tmp)
    try:
        if not wait_for(lambda: [w for w in env.display.windows() if w["pid"] == p.pid] or p.poll() is not None,
                        60, 0.5) or p.poll() is not None:
            print("skip covered (the library window did not come up on the compositor)")
            return
        env.display.cmd("[pid=%d] fullscreen enable" % p.pid)
        full = wait_for(lambda: any(w["fullscreen"] for w in env.display.windows() if w["pid"] == p.pid), 15)
        check(bool(full), "covered: the compositor shows the library window fullscreen")
        check(bool(wait_for(hidden, 20, 0.3)), "covered: status says the monitor is covered",
              {k: mon().get(k) for k in ("hidden", "covered", "paused", "wait_timeouts")})
        n = drawn(3.0)
        check(n <= 2, "covered: the animated scene stops drawing behind the window", f"{n} frames in 3 s")
    finally:
        p.terminate()
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    check(bool(wait_for(lambda: not hidden(), 20, 0.3)), "covered: not covered any more when the window is gone",
          {k: mon().get(k) for k in ("hidden", "covered", "paused")})
    n = drawn()
    check(n > 3, "covered: the scene draws again", f"{n} frames in 2 s")
    ls = env.display.layers(a)
    if ls is None:      # (a compositor that does not list them: the daemon's own account)
        ls = [sf for sf in env.state().get("wl_surfaces") or [] if sf.get("output") == a]
        check(len(ls) == 1 and ls[0].get("role") == "ready" and ls[0].get("recreated") == 0,
              "covered: the wallpaper's surface is still the one it was made with (status wl_surfaces)", ls)
    else:
        check(len(ls) == 1, "covered: the wallpaper's surface is still the one on the output", ls)
    healthy(env, mark, "covered")
    # told to quit while nothing is shown: no thread may be waiting for a frame that never comes
    env.display.power(a, False)
    wait_for(hidden, 20, 0.2)
    t0 = time.time()
    env.ctl("quit")
    gone = wait_for(lambda: env.daemon.poll() is not None, 5, 0.05)
    check(bool(gone), "covered: quit while the output is off ends the daemon at once", f"{time.time() - t0:.1f} s")
    if gone:
        env.daemon = None
    env.display.power(a, True)
    env.stop_daemon()


# ------------------------------------------------------------------ backend
def case_backend(env, W):
    if "x11" not in displayenv.built_backends(env.build):
        print("skip backend (this build has the Wayland backend only)")
        return
    lack = displayenv.missing("xvfb")
    if lack:
        print("skip backend (needs %s)" % ", ".join(lack))
        return
    xtmp = os.path.join(env.tmp, "x")
    os.makedirs(xtmp)
    x = displayenv.make("xvfb", xtmp, PATIENCE)
    if not check(x.start(640, 360), "backend: an X server next to the compositor"):
        x.stop()        # (one that came up without ever showing its framebuffer still runs)
        return
    xmon = ("X", 640, 360, 0, 0)
    named = env.env.pop(BACKEND_ENV, None)      # this case decides what the daemon is told
    hashes = {}
    try:
        # (config.json "backend", WALPE_BACKEND) -> the backend that must run, where status says it came from
        for conf, var, want, source in ((None, None, "wayland", "default"), ("x11", None, "x11", "config"),
                                        ("wayland", None, "wayland", "config"),
                                        ("x11", "wayland", "wayland", "WALPE_BACKEND"),
                                        ("wayland", "x11", "x11", "WALPE_BACKEND")):
            what = f"backend (config {conf or '-'}, {BACKEND_ENV} {var or '-'})"
            extra = {"DISPLAY": x.display}          # (the X screen is one monitor of 640x360 as it is)
            if var:
                extra[BACKEND_ENV] = var
            if not start(env, [("A", 640, 360, 0, 0)], what, extra=extra, config={BACKEND_KEY: conf} if conf else {}):
                continue
            name = env.display.outputs()[0]["name"]
            mark = env.mark()
            st = env.state()
            check(st.get("display_backend") == want, f"{what}: status says {want}",
                  {k: v for k, v in st.items() if k.startswith("display_backend")})
            check(st.get("display_backend_source") == source, f"{what}: ... chosen by: {source}",
                  st.get("display_backend_source"))
            env.open(W["quad"])
            lc.loaded(env, W["quad"])
            if want == "wayland":
                ok, lk = shows_quad(env, name, size=(640, 360))
                check(ok, f"{what}: the picture is on the Wayland output", lk)
                check(tone(x.pixel(xmon, 160, 90)) != "red", f"{what}: ... and not on the X screen",
                      x.pixel(xmon, 160, 90))
                hashes["wayland"] = lk["hash"]
            else:
                def on_x():
                    return [tone(x.pixel(xmon, px, py)) for py in (90, 270) for px in (160, 480)]
                wait_for(lambda: on_x() == QUAD, 20, 0.3)
                check(on_x() == QUAD, f"{what}: the picture is on the X screen", on_x())
                check(look(env, name)["middle"] != QUAD, f"{what}: ... and not on the Wayland output")
                hashes["x11"] = displayenv.frame_hash(x.frame(xmon))
            healthy(env, mark, what)
            env.stop_daemon()
        if len(hashes) == 2:
            check(hashes["x11"] == hashes["wayland"],
                  "backend: the same scene is the same picture on X11 and on Wayland, byte for byte", hashes)
    finally:
        env.stop_daemon()
        x.stop()
        if named is not None:
            env.env[BACKEND_ENV] = named


# ------------------------------------------------------------------ noshell
def case_noshell(env, W):
    """weston has no wlr-layer-shell: a wallpaper cannot be placed there. Without weston
    the daemon is told to overlook sway's layer shell, which reaches the same code."""
    w, x = None, None
    denv = dict(env.env)
    for k in ("DISPLAY", BACKEND_ENV):
        denv.pop(k, None)
    if shutil.which("weston"):
        rt = os.path.join(env.tmp, "weston")
        os.makedirs(rt, mode=0o700)
        wenv = dict(os.environ)
        for k in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET"):
            wenv.pop(k, None)
        wenv["XDG_RUNTIME_DIR"] = rt
        with open(os.path.join(env.tmp, "weston.log"), "ab") as log:
            w = subprocess.Popen(["weston", "--backend=headless", "--socket=noshell", "--no-config",
                                  "--idle-time=0"], env=wenv, stdin=subprocess.DEVNULL, stdout=log,
                                 stderr=subprocess.STDOUT, cwd=env.tmp)
        sock = os.path.join(rt, "noshell")
        if not wait_for(lambda: os.path.exists(sock) or w.poll() is not None, 20, 0.1) or w.poll() is not None:
            print("note noshell: weston did not start here; sway with the layer shell ignored is used", flush=True)
            if w.poll() is None:
                w.terminate()
                try:
                    w.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    w.kill()
                    w.wait()
            w = None
        else:
            denv["WAYLAND_DISPLAY"] = sock
    if w is None:
        denv["WALPE_WL_IGNORE"] = "zwlr_layer_shell_v1"
    try:
        rc.jdump(env.config, CONFIG)
        p = subprocess.run([env.daemon_bin, "--backend", "wayland"], env=denv, stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60 * PATIENCE, cwd=env.tmp)
        check(p.returncode == 1 and b"layer shell" in p.stdout,
              "noshell: asked for by name, the Wayland backend ends with status 1 and names the layer shell",
              (p.returncode, p.stdout[-400:]))
        check(not os.path.exists(env.sock), "noshell: ... and leaves no socket", env.sock)
        p = subprocess.run([env.daemon_bin], env=denv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=60 * PATIENCE, cwd=env.tmp)
        check(p.returncode == 1 and b"no display to draw on" in p.stdout,
              "noshell: without a setting and without an X server the daemon ends with status 1 and says why",
              (p.returncode, p.stdout[-400:]))
        if "x11" in displayenv.built_backends(env.build) and not displayenv.missing("xvfb"):
            xtmp = os.path.join(env.tmp, "x-noshell")
            os.makedirs(xtmp)
            x = displayenv.make("xvfb", xtmp, PATIENCE)
            if check(x.start(640, 360), "noshell: an X server next to that compositor"):
                # In a real session that X server is Xwayland, where the X11 backend's
                # window lies above every other window: the automatic choice does not
                # go there. (Here it is an Xvfb; the daemon cannot and need not tell.)
                denv["DISPLAY"] = x.display
                p = subprocess.run([env.daemon_bin], env=denv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=60 * PATIENCE, cwd=env.tmp)
                check(p.returncode == 1 and b"layer shell" in p.stdout and b"Xwayland" in p.stdout and
                      b"--backend x11" in p.stdout,
                      "noshell: without a setting X11 is not taken although DISPLAY is set: status 1, and the "
                      "sentence names the layer shell, Xwayland and --backend x11", (p.returncode, p.stdout[-500:]))
                check(not os.path.exists(env.sock), "noshell: ... and leaves no socket", env.sock)
                # The same with the compositor's socket handed over as a descriptor
                # (WAYLAND_SOCKET, which can be connected to once)
                sk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    sk.connect(denv["WAYLAND_DISPLAY"])
                    henv = dict(denv, WAYLAND_SOCKET=str(sk.fileno()))
                    del henv["WAYLAND_DISPLAY"]
                    p = subprocess.run([env.daemon_bin], env=henv, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60 * PATIENCE,
                                       cwd=env.tmp, pass_fds=(sk.fileno(),))
                    check(p.returncode == 1 and b"Xwayland" in p.stdout,
                          "noshell: the same with the compositor's socket handed over (WAYLAND_SOCKET)",
                          (p.returncode, p.stdout[-500:]))
                finally:
                    sk.close()
                # asked for by name, X11 is taken there, and the log says what that means
                mark = env.mark()
                with open(env.log, "ab") as log:
                    env.daemon = subprocess.Popen([env.daemon_bin, "--backend", "x11"], env=denv, stdout=log,
                                                  stderr=subprocess.STDOUT, cwd=env.tmp)
                up = wait_for(lambda: env.daemon.poll() is not None or bool(env.state()), 30, 0.2)
                st = env.state()
                check(bool(up) and st.get("display_backend") == "x11",
                      "noshell: asked for by name (--backend x11), X11 is taken next to that compositor",
                      {k: v for k, v in st.items() if k.startswith("display_backend")})
                check("lies above the other windows" in env.since(mark),
                      "noshell: ... and the log says that the window lies above the others under Xwayland",
                      env.since(mark)[-400:])
                env.stop_daemon()
    finally:
        env.stop_daemon()
        if x:
            x.stop()
        if w:
            w.terminate()
            try:
                w.wait(timeout=10)
            except subprocess.TimeoutExpired:
                w.kill()
                w.wait()


# --------------------------------------------------------------------- lost
def case_lost(env, W):
    """the last case: it ends the compositor"""
    if not start(env, [("A", 640, 360, 0, 0)], "lost"):
        return
    env.open(W["red"])
    lc.loaded(env, W["red"])
    name = env.mons[0][0]
    check(shows(env, name, "red") == "red", "lost: the wallpaper is shown")
    env.display.stop()
    try:
        status = env.daemon.wait(timeout=30 * PATIENCE)
    except subprocess.TimeoutExpired:
        status = None
    check(status == 1, "lost: the compositor is gone, the daemon ends with status 1", status)
    if status is not None:
        env.daemon = None
    time.sleep(0.5)
    check(not os.path.exists(env.sock), "lost: no control socket is left behind", env.sock)
    check(not os.path.exists(env.sock + ".loading"), "lost: nor the marker of a start that did not finish")


# ------------------------------------------------------------------- cycles
def case_cycles(env, W, rounds=6):
    """not in the default run (each round is two restarts in place, ten seconds apart at least)"""
    if not start(env, [("A", 640, 360, 0, 0)], "cycles"):
        return
    mark = env.mark()
    a, pid = env.mons[0][0], env.daemon.pid
    env.open(W["red"])
    lc.loaded(env, W["red"])

    def open_files():
        try:
            return len(os.listdir(f"/proc/{pid}/fd"))
        except OSError:
            return -1
    counts = []
    for i in range(rounds):
        b = env.display.add_output(mode=(480, 270), pos=(640, 0))
        ok, _ = follow(env, lambda: len(rects(env)) == 2, 40)
        ok2 = bool(b) and env.display.remove_output(b)
        ok3, _ = follow(env, lambda: rects(env) == {a: [0, 0, 640, 360]}, 40)
        if not check(ok and ok2 and ok3, f"cycles: round {i + 1}: the output came and went, status followed",
                     rects(env)):
            break
        lc.loaded(env, W["red"], timeout=30)
        time.sleep(2.0 * PATIENCE)
        counts.append(open_files())
    check(len(counts) == rounds and counts[-1] <= counts[1],
          "cycles: no more open files after the last round than after the second", counts)
    check(env.daemon.poll() is None and env.daemon.pid == pid, "cycles: same process throughout")
    healthy(env, mark, "cycles")
    env.stop_daemon()


# --------------------------------------------------------------- fullscreen
def case_fullscreen(env, W):
    gui = os.path.join(env.build, "walpe")
    if not os.path.exists(gui):
        print("skip fullscreen (no library window program in the build)")
        return
    # (covered stays off, as in CONFIG: only the fullscreen rule may pause; each output
    # draws its own scene)
    if not start(env, [("A", 640, 360, 0, 0), ("B", 480, 270, 640, 0)], "fullscreen",
                 config={"render_shared": False}):
        return
    mark = env.mark()
    (a, *_), (b, *_) = env.mons
    env.open(W["anim"])
    lc.loaded(env, W["anim"])

    def mons():
        """status of the two outputs, A first (the daemon's own order is its business)"""
        by = {m.get("name"): m for m in env.state().get("monitors") or []}
        return [by.get(a) or {}, by.get(b) or {}]

    def flags(key):
        return [bool(m.get(key)) for m in mons()]

    def drawn(seconds=2.0):
        n = [m.get("frames") or 0 for m in mons()]
        time.sleep(seconds * PATIENCE)
        return [(m.get("frames") or 0) - k for m, k in zip(mons(), n)]

    def is_state(here, paused, what):
        ok = wait_for(lambda: flags("fullscreen") == here and flags("paused") == paused, 20, 0.2)
        st = env.state()
        check(bool(ok), "fullscreen: %s: fullscreen %s, paused %s" % (what, here, paused),
              {"fullscreen": flags("fullscreen"), "paused": flags("paused"),
               "monitors_state": st.get("monitors_state"), "windows": env.display.windows()})
        n = drawn()
        # (a paused monitor draws nothing; one that runs behind a fullscreen window is
        # not asked for frames by the compositor, so only an uncovered one is counted)
        good = all(k <= 2 for k, p in zip(n, paused) if p) and \
            all(k > 3 for k, p, h in zip(n, paused, here) if not p and not h)
        check(good, "fullscreen: %s: a paused output stops drawing, a free one draws" % what,
              "frames in %.0f s: %s" % (2.0 * PATIENCE, n))
        return st

    wait_for(lambda: min(drawn(1.0)) > 3, 20, 0.1)
    st = is_state([False, False], [False, False], "no window")
    sig = st.get("pause_signals") or {}
    src = (sig.get("fullscreen_on") or {}).get("from")
    if not check(src == "wl-fullscreen", "fullscreen: the compositor's window list tells the window's output",
                 sig.get("fullscreen_on")):
        env.stop_daemon()
        return
    rule = (st.get("pause_rules") or {}).get("pause_fullscreen") or {}
    check(rule.get("works") is True and "note" not in rule,
          "fullscreen: status: the rule works and has nothing to explain (outputs have names)", rule)

    genv = dict(env.run_env)
    genv.update({"GDK_BACKEND": "wayland", "GSK_RENDERER": "cairo", "GTK_A11Y": "none"})
    with open(os.path.join(env.tmp, "gui.log"), "ab") as log:
        p = subprocess.Popen([gui], env=genv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                             cwd=env.tmp)
    try:
        if not wait_for(lambda: [w for w in env.display.windows() if w["pid"] == p.pid] or p.poll() is not None,
                        60, 0.5) or p.poll() is not None:
            print("skip fullscreen (the library window did not come up on the compositor)")
            return
        sel = "[pid=%d] " % p.pid
        # the focus is moved between outputs below, and sway would take the pointer along
        # (mouse_warping output, its default): a pointer left at an output's edge would be
        # in the pictures of the cases that follow. The pointer stays where it is.
        env.display.cmd("mouse_warping none")

        def put(out):
            """the window fullscreen on this output, with the focus"""
            env.display.cmd(sel + "move container to output " + out)
            env.display.cmd(sel + "fullscreen enable")
            env.display.cmd(sel + "focus")

        # a plain window with the focus pauses nothing
        env.display.cmd(sel + "move container to output " + a)
        env.display.cmd(sel + "focus")
        is_state([False, False], [False, False], "a window that is not fullscreen")
        put(a)
        check(bool(wait_for(lambda: any(w["fullscreen"] for w in env.display.windows() if w["pid"] == p.pid), 15)),
              "fullscreen: the compositor shows the library window fullscreen")
        st = is_state([True, False], [True, False], "the window fullscreen on the first output")
        check(st.get("fullscreen") is True and st.get("fullscreen_pause") is True,
              "fullscreen: the status of all monitors: a fullscreen window has the focus, the rule pauses",
              {k: st.get(k) for k in ("fullscreen", "fullscreen_pause")})
        rule = (st.get("pause_rules") or {}).get("pause_fullscreen") or {}
        check("note" not in rule, "fullscreen: no note about every monitor pausing", rule)
        put(b)
        is_state([False, True], [False, True], "the window moved to the second output")
        # every monitor, as before the rule looked at outputs
        code, rep = env.ctl("set", "fs_all=on")
        check(code == 0 and bool(rep and rep.get("ok")), "fullscreen: walpe-ctl set fs_all=on is taken", rep)
        is_state([False, True], [True, True], "pause_fullscreen_all on: both outputs pause")
        env.ctl("set", "fs_all=off")
        is_state([False, True], [False, True], "pause_fullscreen_all off: the first output runs again")
        # the focus leaves the window (to the empty first output): the rule needs the focus
        env.display.cmd("focus output " + a)
        st = is_state([False, False], [False, False], "the focus is on the other output")
        check(st.get("fullscreen") is False, "fullscreen: ... and no fullscreen window has the focus",
              st.get("fullscreen"))
        env.display.cmd(sel + "focus")
        is_state([False, True], [False, True], "the focus is back on the fullscreen window")
    finally:
        p.terminate()
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        env.display.cmd("focus output " + a)        # as the cases before left it
        env.display.cmd("mouse_warping output")
    is_state([False, False], [False, False], "the window is gone")
    healthy(env, mark, "fullscreen")
    env.stop_daemon()


OPT_IN = ("cycles",)
CASES = [("start", case_start), ("layer", case_layer), ("two", case_two), ("mode", case_mode), ("scale", case_scale),
         ("noviewport", case_noviewport), ("rotate", case_rotate), ("hotplug", case_hotplug),
         ("pointer", case_pointer), ("covered", case_covered), ("fullscreen", case_fullscreen),
         ("backend", case_backend),
         ("noshell", case_noshell), ("cycles", case_cycles), ("lost", case_lost)]


def main():
    args = sys.argv[1:]
    build, daemon, keep, tmp, only = "build", None, False, None, None
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
        elif a == "--only" and args:
            only = args.pop(0).split(",")
        else:
            sys.stderr.write(__doc__)
            return 2
    skip = displayenv.unusable("sway", build)
    if skip:
        print("waylandcheck: skipped (%s)" % skip[0])
        return skip[1]
    for b in (daemon or os.path.join(build, "walpe-daemon"), os.path.join(build, "walpe-ctl")):
        if not os.path.exists(b):
            print(f"waylandcheck: {b} not found")
            return 2
    env = lc.Env(build, daemon, keep, tmp, "sway")
    try:
        longest = max(len(env.sock.encode()), len(os.path.join(env.display.rt, "sway-ipc.4294967295.4194304.sock")))
        if longest > 107:
            print(f"waylandcheck: {env.tmp} is too long for the sockets below it ({longest} > 107 bytes): "
                  "pass --tmp <a short directory>")
            return 2
        wp = os.path.join(env.tmp, "wallpapers")
        W = {k: os.path.join(wp, k) for k in ("red", "blue", "quad", "ptr", "anim")}
        rc.gen_scene(W["red"], (1, 0, 0))
        rc.gen_scene(W["blue"], (0, 0, 1))
        gen_quad(W["quad"])
        lc.gen_fx_scene(W["ptr"], lc.POINTER_FRAG)
        lc.gen_fx_scene(W["anim"], lc.TIME_FRAG)
        if not env.start_x(640, 360):
            print("waylandcheck: skipped (the compositor did not start)")
            print(env.display.log_tail())
            return displayenv.SKIP
        for name, fn in CASES:
            if (only and name not in only) or (not only and name in OPT_IN):
                continue
            try:
                fn(env, W)
            finally:
                env.stop_daemon()
    finally:
        env.close()
    print(f"waylandcheck: {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
