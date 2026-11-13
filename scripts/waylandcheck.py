#!/usr/bin/env python3
"""End-to-end checks of the daemon on Wayland: outputs that come, go and change.

  python3 -I scripts/waylandcheck.py --build build [--daemon FILE] [--only a,b] [--keep] [--tmp DIR]
                                     [--display sway|labwc|wayfire|river]

A private headless compositor (sway on wlroots' headless backend unless
--display or WALPE_CHECK_DISPLAY names another one, see
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
            process id stays. (How the daemon follows - by a restart in place,
            or in place with wayland_follow on - is the case `follow`; these
            cases look at what is on the output.)
  scale     output scale 2 and 1.5: the picture is right and is drawn in the
            output's own pixels (a sharp colour edge), not enlarged from
            the scaled size. Then the scale alone changes, at a moment at
            which the daemon cannot restart for it yet: the still picture
            fills the output all the same.
  noviewport  the same scales with the compositor's viewporter ignored
            (WALPE_WL_IGNORE=wp_viewporter): the picture is right, nothing
            the compositor objects to.
  rotate    a turned output (transform 90): status follows, the picture is
            the right way up.
  hotplug   an output appears (it gets the wallpaper for all), gets a
            wallpaper of its own, is disabled and enabled again (its own
            wallpaper is back), is unplugged; then every output is gone and
            one returns. The daemon lives through all of it: as it is by
            default, restarting in place for the changes, and once more
            with wayland_follow on, where nothing restarts and with no
            output left it has no monitor and its render threads wait.
  follow    two outputs; the second one moves, changes its scale and its
            mode, is switched off and on again: each change is followed
            without a restart in place (the log, the daemon's uptime), the
            first output's animated wallpaper is never loaded again and goes
            on drawing, the pictures are right, the pointer is followed on
            the output where it lies afterwards. This is the daemon with
            wayland_follow on; without the setting (the default) and with it
            off: the restart in place for a change, as before.
  closed    the compositor closes a wallpaper surface while its output
            stays (made up with WALPE_WL_CLOSE_AFTER_MS): the surface gets
            its role back, or with WALPE_WL_RECREATE=0 and wayland_follow on
            that output gets a new surface and its wallpaper again; nothing
            restarts, the other output's wallpaper goes on.
  nosurface the compositor does not answer the daemon's first wallpaper
            surfaces (made up with WALPE_WL_NO_CONFIGURE): the daemon starts
            without a monitor, says so, tries again on its schedule and
            shows the saved wallpaper once a surface is given; with no
            answer to any try it says that it gives up, runs on with no
            monitor (status, walpe-ctl diag), and tries again when the
            output changes (that last part with wayland_follow on).
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
            pausing (the compositor names the window's output). On a
            compositor other than sway: one output, the window made
            fullscreen, not fullscreen and fullscreen again through the
            compositor's window list (nothing there moves a window to
            another output or takes the focus from every window).
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
  cursor    the pointer's image over a wallpaper that takes pointer input:
            the compositor's own where it takes a shape by name
            (wp_cursor_shape_v1) - the same picture as over a wallpaper that
            takes no input, though XCURSOR_SIZE in the daemon's environment
            says 64 - and one from the theme where that protocol is treated
            as absent (WALPE_WL_IGNORE), as before.
  reconnect the compositor ends and is started again in the same place under
            a daemon with wayland_reconnect: told to end, then killed (its
            socket's file stays behind). The daemon waits, the same process
            is there again with its wallpaper shown. Then the compositor is
            started and killed ten times, between 0 and 0.9 seconds after
            its socket takes connections: at whatever moment of its own
            start that catches the daemon, it waits on (it ended when the
            display went while EGL was being set up), and shows its
            wallpaper when the compositor stays at last. Before all that: a
            daemon told to render on the CPU still does when the compositor
            is back.
  flap      a compositor that does not keep the daemon's connection, under a
            daemon with wayland_reconnect (a stand-in socket in front of the
            compositor's that hands every connection through and ends it
            three seconds after it was made): the daemon does not come back
            without end - the fifth stay in a row of less than 30 seconds
            ends it with status 1, the way wayland_reconnect = 0 does, with
            one sentence that names the setting; nothing of it is left. One
            stay that is not short and the count begins anew (with "short"
            made 8 seconds: WALPE_WL_SHORT_STAY_MS). A connection the
            compositor ends with a protocol error is not come back to at
            all, neither under a running daemon nor under one that waits.
  lost      the compositor goes away: the daemon ends with status 1 and
            leaves no socket behind; its render thread, which goes on for a
            moment (made long here: WALPE_LOST_LINGER_MS), presents nothing
            on the display that is gone - neither the frames of a wallpaper
            nor the black frame of one whose load fails in that moment.
  cycles    (only with --only cycles) an output is created and unplugged
            six times: the daemon's open files and threads do not grow from
            the second round to the last.

--display labwc|wayfire|river: that compositor in sway's place (it needs the
program wlr-randr besides grim; river: up to 0.3, see scripts/displayenv.py).
The cases are the same; what differs is said where it does (fullscreen above;
an output that "appears" was a disabled one).

Exit code 0 = all passed, 1 = failures, 77 = skipped (the compositor or grim is
not installed, or the build has no Wayland backend). WAYLANDCHECK_PATIENCE=N
multiplies every wait (a software renderer on a busy machine: 3 or more).
--tmp DIR: work in DIR (must not exist; removed afterwards); sockets live
below it and a socket path is limited to 107 bytes."""
import array
import select
import struct
import threading
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
GUI_APP_ID = "io.github.walpe.ui"            # the library window's application id
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
    # KWin's pause script is KWin's: no session-bus connection, no word of it, on another compositor
    rules = st.get("pause_rules") or {}
    check("pause_kwin_script" not in st and
          not any("kwin" in (r.get("sources") or "") for r in rules.values()),
          "start: the provider for KWin stays out on this compositor",
          (st.get("pause_kwin_script"), {k: r.get("sources") for k, r in rules.items()}))
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
    if extra is None:
        # The scale alone changes now, so the output's size in the layout does (320x180),
        # and right after the restart for the step before: the daemon does not restart
        # again for some seconds (restarts in place are 10 s apart). A still wallpaper
        # draws no frame meanwhile with which the new size could travel, and has to fill
        # the output nevertheless (it showed its top left quarter, enlarged).
        m = env.mark()
        if check(env.display.output(name, scale=2), f"{what}: scale 2 on the same mode (320x180 in the layout)"):
            bad, good, lk, t0 = 0, False, None, time.time()
            while time.time() - t0 < 8 * PATIENCE:
                restarted = "restarts in place" in env.since(m)
                lk = look(env, name)
                if restarted:
                    break
                if lk["middle"] == QUAD and lk["corners"] == QUAD and lk["size"] == (640, 360):
                    good = True
                    break
                bad += 1
                time.sleep(0.2)
            if good or bad:
                check(good, f"{what}: the still picture fills the output before the daemon restarts for "
                      "the new layout", lk)
            else:
                note(f"{what}: the daemon restarted before a picture could be taken: the time before the "
                     "restart was not looked at")
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
FOLLOW_ON = {"WALPE_WAYLAND_FOLLOW": "on"}     # (gpu.conf wayland_follow; off is the default)


def case_hotplug(env, W):
    """as the daemon is by default (it restarts in place for a change of the outputs), then
    with wayland_follow on (it follows in place)"""
    hotplug_one(env, W, False)
    hotplug_one(env, W, True)


def hotplug_one(env, W, in_place):
    hp = "hotplug, wayland_follow on" if in_place else "hotplug"
    if not start(env, [("A", 640, 360, 0, 0)], hp, extra=FOLLOW_ON if in_place else None):
        return
    mark = env.mark()
    a, pid = env.mons[0][0], env.daemon.pid
    env.open(W["red"])
    lc.loaded(env, W["red"])
    check(shows(env, a, "red") == "red", hp + ": red on the first output")

    b = env.display.add_output(mode=(480, 270), pos=(640, 0))
    if not check(bool(b), hp + ": the compositor makes a second output"):
        env.stop_daemon()
        return
    ok, bad = follow(env, lambda: rects(env) == {a: [0, 0, 640, 360], b: [640, 0, 480, 270]})
    check(ok, hp + ": status lists the new output", rects(env))
    check(bad == 0, hp + ": the control socket stayed connectable while it appeared", f"{bad} failed connects")
    check(shows(env, b, "red") == "red", hp + ": the new output gets the wallpaper for all")
    bi = index_of(env, b)
    env.open(W["blue"], bi)
    lc.loaded(env, W["blue"], bi)
    got = [shows(env, a, "red"), shows(env, b, "blue")]
    check(got == ["red", "blue"], hp + ": the new output takes a wallpaper of its own", got)

    # disabled and enabled again: the same output (same name) returns to its own wallpaper
    check(env.display.output(b, enable=False), hp + ": the compositor disables the second output")
    ok, bad = follow(env, lambda: rects(env) == {a: [0, 0, 640, 360]})
    check(ok, hp + ": status drops the disabled output", rects(env))
    check(shows(env, a, "red") == "red", hp + ": the first output is not disturbed by it")
    check(env.display.output(b, enable=True, mode=(480, 270), pos=(640, 0)), hp + ": ... and enables it again")
    ok, bad2 = follow(env, lambda: set(rects(env)) == {a, b})
    check(ok, hp + ": status lists it again", rects(env))
    got = [shows(env, a, "red"), shows(env, b, "blue")]
    check(got == ["red", "blue"], hp + ": its own wallpaper is back on it", got)
    check(bad + bad2 == 0, hp + ": the control socket stayed connectable through both", f"{bad + bad2} failed connects")

    # unplugged for good
    check(env.display.remove_output(b), hp + ": the compositor unplugs the second output")
    ok, _ = follow(env, lambda: rects(env) == {a: [0, 0, 640, 360]})
    check(ok and shows(env, a, "red") == "red", hp + ": one output left, still red", rects(env))
    cur, own = env.saved()
    check(cur == W["red"] and own.get(b) == W["blue"],
          hp + ": config.json keeps the unplugged output's own choice for its return", (cur, own))

    # every output gone, then one back
    check(env.display.output(a, enable=False), hp + ": the compositor disables the last output")
    if in_place:
        # (the daemon has no monitor then: the wallpaper is unloaded, its render thread
        # waits for an output)
        def none_shown():
            """the monitors on the outputs of this case, what else the daemon has a monitor
            on (a compositor may put a stand-in output in the place of the last one:
            Wayfire's NOOP-1), its surfaces, and whether a render thread waits"""
            st = env.state()
            names = [m.get("name") for m in st.get("monitors") or []]
            return {"monitors": [n for n in names if n in (a, b)], "elsewhere": [n for n in names if n not in (a, b)],
                    "surfaces": [(sf.get("output"), sf.get("role")) for sf in st.get("wl_surfaces") or []],
                    "waiting": (st.get("render_threads_waiting") or 0) > 0}

        def all_gone():
            ns = none_shown()
            return (not ns["monitors"] and ns["waiting"] and
                    sorted(o for o, _ in ns["surfaces"]) == sorted(ns["elsewhere"]))
        ok, _ = follow(env, all_gone)
        ns = none_shown()
        check(ok, hp + ": the daemon answers with no output left: no monitor and no surface on the outputs "
              "that went, a render thread waits", ns)
        if ns["elsewhere"]:
            note(hp + ": the compositor put another output in their place, which got a monitor: %s" % ns["elsewhere"])
    else:
        # (with no output there is nothing to restart for: the daemon keeps the monitors
        # it was started with until one is back; their surfaces are gone, nothing is drawn)
        def none_shown():
            st = env.state()
            return {"monitors": [bool(m.get("hidden")) for m in st.get("monitors") or []],
                    "surfaces": [sf.get("role") for sf in st.get("wl_surfaces") or []]}
        ok, _ = follow(env, lambda: none_shown() == {"monitors": [True], "surfaces": ["dead"]})
        check(ok, hp + ": the daemon answers with no output left: its monitor is not shown, the surface is gone",
              none_shown())
    time.sleep(1.0 * PATIENCE)
    check(env.daemon.poll() is None, hp + ": ... and keeps running")
    check(env.display.output(a, enable=True, mode=(640, 360), pos=(0, 0)), hp + ": the output returns")
    ok, _ = follow(env, lambda: rects(env) == {a: [0, 0, 640, 360]})
    check(ok and shows(env, a, "red") == "red", hp + ": the wallpaper is back on the returned output", rects(env))
    check(env.daemon.poll() is None and env.daemon.pid == pid, hp + ": same process throughout")
    log = env.since(mark)
    if in_place:
        # (one restart: for the second output when it first appeared, which was one more
        # output than the daemon had had)
        n = log.count("restarting to follow it")
        check("followed in place" in log and n <= 1,
              hp + ": the changes were followed in place: one restart at most, for the output that was new", n)
    else:
        check("followed in place" not in log and all(r in log for r in RESTART_LINES),
              hp + ": the changes were followed by restarts in place, as before; nothing in place",
              [r for r in RESTART_LINES if r in log])
    healthy(env, mark, hp)
    env.stop_daemon()


# ------------------------------------------------------------------- follow
def lives(env):
    """the `mem` report: per output how often its render thread loaded a wallpaper and how
    many frames it has presented since the daemon began, and how long that is ago. A
    restart in place begins all three again."""
    rep = (env.ipc({"cmd": "mem", "wait": 10}, timeout=15.0) or {}).get("mem") or {}
    return ({m.get("name"): (m.get("loads"), m.get("frames")) for m in rep.get("monitors") or []},
            rep.get("uptime_s") or 0.0)


RESTART_LINES = ("restarts in place", "restarting to follow it")


def case_follow(env, W):
    """Two outputs. The second one moves, changes its scale and its mode, is switched off
    and on again: the daemon follows each change without restarting, and the first
    output's wallpaper - an animated scene - is never loaded again and goes on drawing."""
    begun = env.mark()
    if not start(env, [("A", 640, 360, 0, 0), ("B", 480, 270, 640, 0)], "follow", extra=FOLLOW_ON):
        return
    mark = env.mark()
    (a, *_), (b, *_) = env.mons
    pid = env.daemon.pid
    env.open(W["anim"])
    lc.loaded(env, W["anim"])
    env.open(W["quad"], index_of(env, b))
    lc.loaded(env, W["quad"], index_of(env, b))
    ok, lk = shows_quad(env, b, size=(480, 270))
    check(ok, "follow: an animated scene on the first output, the four-colour scene on the second", lk)

    def mon(name):
        for m in env.state().get("monitors") or []:
            if m.get("name") == name:
                return m
        return {}

    def step(what, change, done, own=True, timeout=30):
        """one change of the second output. own: its own wallpaper is not loaded again
        either (a scene is, when the size it is rendered at changes). -> followed"""
        time.sleep(0.5 * PATIENCE)
        m, (before, up0) = env.mark(), lives(env)
        if not check(bool(change()), f"follow: {what}: the compositor takes it"):
            return False
        ok, bad = follow(env, done, timeout)
        check(ok, f"follow: {what}: status follows", rects(env))
        check(bad == 0, f"follow: {what}: the control socket stayed connectable", f"{bad} failed connects")
        time.sleep(1.0 * PATIENCE)
        after, up1 = lives(env)
        said = [ln for ln in env.since(m).splitlines() if any(r in ln for r in RESTART_LINES)]
        check(not said and up1 > up0 and env.daemon.poll() is None and env.daemon.pid == pid,
              f"follow: {what}: followed without a restart in place", (said[:2], up0, up1))
        la, lb = before.get(a) or (None, 0), after.get(a) or (None, -1)
        check(la[0] is not None and la[0] == lb[0] and (lb[1] or 0) > (la[1] or 0),
              f"follow: {what}: the first output's wallpaper was not loaded again and went on drawing",
              f"loads {la[0]} -> {lb[0]}, frames {la[1]} -> {lb[1]}")
        if own and b in after:
            check((before.get(b) or (None,))[0] == after[b][0],
                  f"follow: {what}: the second output's wallpaper was not loaded again either",
                  f"loads {(before.get(b) or (None,))[0]} -> {after[b][0]}")
        return ok

    # another place in the layout: below the first output
    # (both are placed: a compositor that arranges outputs by itself moves the other one)
    step("moved below the first", lambda: env.display.output(b, pos=(0, 360)) and env.display.output(a, pos=(0, 0)),
         lambda: rects(env) == {a: [0, 0, 640, 360], b: [0, 360, 480, 270]})
    ok, lk = shows_quad(env, b, size=(480, 270))
    check(ok, "follow: moved: the picture on the second output is as it was", lk)
    # another scale on the same mode: the same pixels, half the size in the layout
    step("scale 2", lambda: env.display.output(b, scale=2), lambda: mon(b).get("scale") == 2)
    ok, lk = shows_quad(env, b, size=(480, 270))
    check(ok and 0 <= lk["edge"] <= 1, "follow: scale 2: the picture fills the output, drawn in its own pixels", lk)
    step("scale 1 again", lambda: env.display.output(b, scale=1), lambda: mon(b).get("scale") in (None, 1))
    # another mode: the surface's buffers get the new size, the scene is rendered at it
    step("mode 800x450", lambda: env.display.output(b, mode=(800, 450)),
         lambda: rects(env).get(b, [])[2:] == [800, 450], own=False)
    check(lc.loaded(env, W["quad"], index_of(env, b), timeout=30), "follow: mode: the wallpaper is the one from before",
          rc.brief(env.monitors()))
    ok, lk = shows_quad(env, b, size=(800, 450))
    check(ok and 0 <= lk["edge"] <= 1, "follow: mode: the picture fills 800x450 and is drawn at that size "
          "(a sharp edge)", lk)
    sf = [x for x in env.state().get("wl_surfaces") or [] if x.get("output") == b]
    check(len(sf) == 1 and sf[0].get("buffer") == [800, 450] and sf[0].get("role") == "ready",
          "follow: mode: status wl_surfaces: the surface's buffer is 800x450", sf)
    # switched off: only the first output is left, and is not disturbed
    step("switched off", lambda: env.display.output(b, enable=False),
         lambda: rects(env) == {a: [0, 0, 640, 360]})
    st = env.state()
    check(len(st.get("wl_surfaces") or []) == 1 and st.get("render_threads_waiting") == 1,
          "follow: off: one wallpaper surface is left, the other output's render thread waits",
          (st.get("wl_surfaces"), st.get("render_threads_waiting")))
    # ... and on again: it has its own wallpaper back
    step("switched on again", lambda: env.display.output(b, enable=True, mode=(480, 270), pos=(640, 0)),
         lambda: rects(env) == {a: [0, 0, 640, 360], b: [640, 0, 480, 270]}, own=False)
    check(lc.loaded(env, W["quad"], index_of(env, b), timeout=30), "follow: on again: its own wallpaper is loaded",
          rc.brief(env.monitors()))
    ok, lk = shows_quad(env, b, size=(480, 270))
    check(ok, "follow: on again: the four-colour scene is on it", lk)
    if env.display.can_point():
        # the pointer is where the output is now: left of the first one
        step("moved to the left of the first", lambda: env.display.output(b, pos=(0, 0)) and
             env.display.output(a, pos=(480, 0)),
             lambda: rects(env) == {a: [480, 0, 640, 360], b: [0, 0, 480, 270]})
        env.open(W["ptr"], index_of(env, b))
        lc.loaded(env, W["ptr"], index_of(env, b))

        def colour(want, x, y):
            box, n = [None], [0]

            def ok():
                f = env.display.shot(b)
                box[0] = rc.name_of(displayenv.pixel_of(f, f[0] // 2, f[1] // 2)) if f else "none"
                if box[0] != want:
                    n[0] += 1
                    env.display.pointer(x + n[0] % 2, y)
                return box[0] == want
            env.display.pointer(x, y)
            wait_for(ok, 8, 0.2)
            return box[0]
        got = [colour("blue", 4, 100), colour("red", 476, 100)]
        check(got == ["blue", "red"], "follow: the pointer is followed on the output where it lies now "
              "(left edge blue, right edge red)", got)
    healthy(env, mark, "follow")
    env.stop_daemon()
    check("wayland_follow is on" in env.since(begun), "follow: the log says that the setting is on")
    env.stop_daemon()
    # without the setting, which is the default, and with it off: the restart in place for
    # every change, as before, and nothing of the outputs is followed in place
    for extra, what in ((None, "the setting is not set"), ({"WALPE_WAYLAND_FOLLOW": "off"}, "WALPE_WAYLAND_FOLLOW=off")):
        m = env.mark()
        if not start(env, [("A", 640, 360, 0, 0), ("B", 480, 270, 640, 0)], "follow, " + what, extra=extra):
            return
        (a, *_), (b, *_) = env.mons
        time.sleep(1.5 * PATIENCE)
        env.display.output(b, pos=(0, 360))
        env.display.output(a, pos=(0, 0))
        ok, _ = follow(env, lambda: rects(env) == {a: [0, 0, 640, 360], b: [0, 360, 480, 270]}, 40)
        log = env.since(m)
        said = [r for r in RESTART_LINES if r in log]
        check(ok and len(said) == len(RESTART_LINES) and "followed in place" not in log and
              "wayland_follow is on" not in log,
              f"follow: {what}: the daemon restarts in place for the change, as before", (rects(env), said))
        check(env.state().get("render_threads_waiting") == 0, f"follow: {what}: no render thread waits for an "
              "output", env.state().get("render_threads_waiting"))
        env.stop_daemon()


# ------------------------------------------------------------------- closed
def case_closed(env, W):
    """The compositor closes a wallpaper surface while its output stays (the daemon is
    told to act as if: WALPE_WL_CLOSE_AFTER_MS; no compositor here does it on request).
    The surface gets its role back in place; where that is not to be tried
    (WALPE_WL_RECREATE=0; with wayland_follow on) that output's wallpaper is loaded again
    on a new surface. In both cases nothing restarts and the other output's wallpaper
    goes on."""
    after = int(6000 * PATIENCE)
    for recreate, what in ((None, "closed"), ("0", "closed, WALPE_WL_RECREATE=0")):
        extra = {"WALPE_WL_CLOSE_AFTER_MS": str(after)}
        if recreate is not None:
            # (a new surface on a monitor that goes on: only where outputs are followed in
            # place; the daemon's default is the restart for it, as before)
            extra["WALPE_WL_RECREATE"] = recreate
            extra.update(FOLLOW_ON)
        t0 = time.time()
        if not start(env, [("A", 640, 360, 0, 0), ("B", 480, 270, 640, 0)], what, extra=extra):
            return
        mark = env.mark()
        (a, *_), (b, *_) = env.mons
        pid = env.daemon.pid
        # the surface that is closed is the first one the daemon made: the first output's
        env.open(W["quad"])
        lc.loaded(env, W["quad"])
        env.open(W["anim"], index_of(env, b))
        lc.loaded(env, W["anim"], index_of(env, b))
        ok, lk = shows_quad(env, a, size=(640, 360))
        check(ok and time.time() - t0 < after / 1000.0, f"{what}: the four-colour scene on the first output, an "
              "animated one on the second, before the surface is closed", (lk, time.time() - t0))
        before, up0 = lives(env)
        said = wait_for(lambda: "as if the compositor had closed the wallpaper surface" in env.since(mark),
                        after / 1000.0 / PATIENCE + 10, 0.2)
        check(bool(said), f"{what}: the surface of the first output is closed", env.since(mark)[-300:])

        def surface():
            sf = [x for x in env.state().get("wl_surfaces") or [] if x.get("output") == a]
            return sf[0] if len(sf) == 1 else {}
        if recreate is None:
            ok = wait_for(lambda: surface().get("role") == "ready" and surface().get("recreated") == 1, 20, 0.2)
            check(bool(ok) and "it has its role again" in env.since(mark),
                  f"{what}: the surface has its role again (status wl_surfaces: ready, recreated 1)", surface())
        else:
            ok = wait_for(lambda: "the window system closed its surface" in env.since(mark) and
                          surface().get("role") == "ready" and surface().get("recreated") == 0 and
                          env.state().get("render_threads_waiting") == 0, 30, 0.2)
            check(bool(ok), f"{what}: the first output's monitor let go of the closed surface and has a new one "
                  "(status wl_surfaces: ready, never re-created)", (surface(), env.since(mark)[-400:]))
            check(lc.loaded(env, W["quad"], index_of(env, a), timeout=30), f"{what}: its wallpaper is loaded again",
                  rc.brief(env.monitors()))
        ok, lk = shows_quad(env, a, size=(640, 360))
        check(ok, f"{what}: the four-colour scene is on the first output again", lk)
        time.sleep(1.0 * PATIENCE)
        now, up1 = lives(env)
        log = env.since(mark)
        check(not any(r in log for r in RESTART_LINES) and up1 > up0 and env.daemon.pid == pid,
              f"{what}: no restart in place", (up0, up1))
        lb, ln = before.get(b) or (None, 0), now.get(b) or (None, -1)
        check(lb[0] is not None and lb[0] == ln[0] and (ln[1] or 0) > (lb[1] or 0),
              f"{what}: the second output's wallpaper was not loaded again and went on drawing",
              f"loads {lb[0]} -> {ln[0]}, frames {lb[1]} -> {ln[1]}")
        healthy(env, mark, what)
        env.stop_daemon()


# ---------------------------------------------------------------- nosurface
def case_nosurface(env, W):
    """The compositor does not answer the daemon's first wallpaper surfaces (made up with
    WALPE_WL_NO_CONFIGURE=n: no compositor here stays silent on request; river 0.4 and
    later does until its window manager is connected). The daemon starts without a
    monitor and says so, tries again on its schedule and shows the wallpaper once a
    surface is given - with the daemon's default settings: this does not depend on
    wayland_follow. When the tries are used up it says that it gives up and runs on; with
    wayland_follow on it tries again when an output changes (without, such a change is a
    restart in place, which begins with new surfaces anyway)."""
    cfg = dict(CONFIG, current=W["quad"])

    def unanswered(mark):
        return env.since(mark).count("did not configure the wallpaper surface")

    # the start and the first try stay without an answer, the second try gets one
    begun = env.mark()
    if not start(env, [("A", 640, 360, 0, 0)], "nosurface", config=cfg,
                 extra={"WALPE_WL_NO_CONFIGURE": "2", "WALPE_SURFACE_RETRY_MS": "1000"}):
        return
    name, pid = env.mons[0][0], env.daemon.pid
    ok, _ = follow(env, lambda: rects(env) == {name: [0, 0, 640, 360]}, 30)
    log = env.since(begun)
    check(ok and unanswered(begun) == 2 and "NO WALLPAPER SURFACE could be made on it" in log and
          "tries again in 1 s" in log, "nosurface: no surface at the start: the daemon says so and that it "
          "tries again; the second try gets one", (rects(env), unanswered(begun), log[-600:]))
    check("has given it a wallpaper surface now (try 3" in log, "nosurface: the log says at which try", log[-400:])
    # (each try that got no surface was "eglCreateWindowSurface failed for monitor 0
    # (0x3000)": a call that was never made, and EGL's number for success)
    check(log.count("monitor 0: no surface to draw on (no EGL call failed") == 2 and
          "eglCreateWindowSurface failed" not in log and "(0x3000)" not in log,
          "nosurface: the daemon's own line about each try names no EGL call that did not fail",
          [ln for ln in log.splitlines() if "surface" in ln and "WARNING" in ln][:6])
    check(lc.loaded(env, W["quad"], timeout=30), "nosurface: the saved wallpaper is loaded on it",
          rc.brief(env.monitors()))
    ok, lk = shows_quad(env, name, size=(640, 360))
    check(ok, "nosurface: ... and shown", lk)
    st = env.state()
    check("monitors_without_surface" not in st and st.get("render_threads_waiting") == 0 and
          not any(r in env.since(begun) for r in RESTART_LINES) and env.daemon.pid == pid,
          "nosurface: status has no output without a surface any more, nothing restarted",
          {k: st.get(k) for k in ("monitors_without_surface", "render_threads_waiting")})
    check("wayland_follow is on" not in env.since(begun) and "followed in place" not in env.since(begun),
          "nosurface: all of that with wayland_follow off, the default")
    healthy(env, begun, "nosurface")
    env.stop_daemon()

    # no answer to the start nor to any of the eight tries; the first one after that gets one
    begun = env.mark()
    if not start(env, [("A", 640, 360, 0, 0)], "nosurface, given up", config=cfg,
                 extra=dict(FOLLOW_ON, WALPE_WL_NO_CONFIGURE="9", WALPE_SURFACE_RETRY_MS="100")):
        return
    name, pid = env.mons[0][0], env.daemon.pid
    gave = wait_for(lambda: "walpe-daemon gives up on it and runs on" in env.since(begun), 60, 0.3)
    st = env.state()
    check(bool(gave) and unanswered(begun) == 9, "nosurface: no answer to the start and to eight tries: the "
          "daemon says that it gives up", (unanswered(begun), env.since(begun)[-500:]))
    check(st.get("monitors") == [] and "given up after 9 tries" in (st.get("monitors_without_surface") or "") and
          name in (st.get("monitors_without_surface") or "") and st.get("render_threads_waiting") == 1,
          "nosurface: it runs on and answers: status has no monitor, names the output without a surface and "
          "says that it was given up",
          {k: st.get(k) for k in ("monitors", "monitors_without_surface", "render_threads_waiting")})
    diag = (env.ipc({"cmd": "diag"}) or {}).get("text") or ""
    check("NO WALLPAPER SURFACE on: " + name in diag and "given up" in diag,
          "nosurface: walpe-ctl diag says the same", diag[-500:])
    time.sleep(2.0 * PATIENCE)
    check(unanswered(begun) == 9 and env.daemon.poll() is None, "nosurface: no further try by itself")
    # an output changes: it is tried again
    m = env.mark()
    check(env.display.output(name, mode=(800, 450)), "nosurface: the output gets another mode")
    ok, _ = follow(env, lambda: rects(env) == {name: [0, 0, 800, 450]}, 30)
    check(ok and "has given it a wallpaper surface now" in env.since(m), "nosurface: the change of the output makes "
          "the daemon try again, and now it has a monitor", (rects(env), env.since(m)[-400:]))
    check(lc.loaded(env, W["quad"], timeout=30), "nosurface: the saved wallpaper is loaded on it",
          rc.brief(env.monitors()))
    ok, lk = shows_quad(env, name, size=(800, 450))
    check(ok and not any(r in env.since(begun) for r in RESTART_LINES) and env.daemon.pid == pid,
          "nosurface: ... and shown; nothing restarted", lk)
    healthy(env, begun, "nosurface, given up")
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

    def colour(name, want, at=None):
        """at: where the pointer was just put. A compositor tells a surface of the pointer
        when the pointer moves (docs/WAYLAND.md), and a movement that reaches it before
        the wallpaper's request for pointer input does is one the wallpaper never hears
        of (seen with river 0.3.17 on a busy machine): moved by one unit and back while
        waiting."""
        box, n = [None], [0]

        def ok():
            f = env.display.shot(name)
            box[0] = rc.name_of(displayenv.pixel_of(f, f[0] // 2, f[1] // 2)) if f else "none"
            if box[0] != want and at:
                n[0] += 1
                env.display.pointer(at[0] + n[0] % 2, at[1])
            return box[0] == want
        wait_for(ok, 6, 0.2)
        return box[0]
    note("pointer: moved by " + env.display.pointer_how)
    env.open(W["ptr"])
    lc.loaded(env, W["ptr"])
    check(env.display.pointer(4, 100), "pointer: the pointer moves to the left edge of the first output")
    got = colour(a, "blue", (4, 100))
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
        box, n = [(-1, -1, -1)], [0]

        def ok():
            f = env.display.shot(name)
            box[0] = displayenv.pixel_of(f, f[0] // 2, f[1] // 2) if f else displayenv.NO_PICTURE
            if abs(box[0][0] - want) > 16:       # (as in colour (): once more, a unit beside)
                n[0] += 1
                env.display.pointer(x + n[0] % 2, y)
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


# ------------------------------------------------------------------- cursor
def pointer_image(env, name):
    """where the pointer's image is in a picture of the output, over a wallpaper of one
    colour: (width, height, x, y) of what is not that colour; None = nothing is drawn (or
    no picture)"""
    f = env.display.shot(name, cursor=True)
    if not f:
        return None
    w, h, px = f
    bg = px[0:3]
    xs, ys = [], []
    for y in range(h):
        row = px[y * w * 3:(y + 1) * w * 3]
        if row != bg * w:
            ys.append(y)
            xs += [x for x in range(w) if row[x * 3:x * 3 + 3] != bg]
    return (max(xs) - min(xs) + 1, max(ys) - min(ys) + 1, min(xs), min(ys)) if ys else None


def case_cursor(env, W):
    """The pointer's image over a wallpaper that takes pointer input. Where the compositor
    takes a shape by name (wp_cursor_shape_v1) the daemon asks for the default arrow and
    the compositor draws its own: the same image, at the same place, as over a wallpaper
    that takes no input - although the daemon's environment names another size
    (XCURSOR_SIZE). With that protocol treated as absent the daemon sets an image from the
    theme, as it did before.

    It runs late, before the case that starts the compositor anew: once a picture was taken
    with the pointer in it, sway and labwc leave the pointer in the pictures taken without,
    and a case that compares pictures (`backend`) would see it."""
    if not env.display.can_point():
        print("skip cursor (the compositor gives no virtual pointer device)")
        return
    shape_global = "wp_cursor_shape_manager_v1"
    for ignore in (None, shape_global):
        what = "cursor, cursor-shape treated as absent" if ignore else "cursor"
        extra = {"XCURSOR_SIZE": "64"}
        if ignore:
            extra["WALPE_WL_IGNORE"] = ignore
        if not start(env, [("A", 640, 360, 0, 0)], what, extra=extra, config={"parallax": True}):
            return
        mark = env.mark()
        name = env.mons[0][0]
        st = env.state()
        offered = "cursor-shape" in (st.get("wl_compositor_globals") or "")
        if not ignore and not offered:
            note("cursor: the compositor has no cursor-shape-v1: the theme's image is what there is")
        want = "compositor" if offered else "theme"
        cur = st.get("wl_cursor") or {}
        check(cur.get("from") == want and cur.get("size") == 64,
              f"{what}: status wl_cursor says who draws the pointer's image ({want}) and the size the "
              "environment names", cur)
        # a wallpaper that takes no pointer input: the image is the compositor's business
        env.open(W["red"])
        lc.loaded(env, W["red"])
        env.display.pointer(100, 100)
        env.display.pointer(300, 200)
        time.sleep(0.5 * PATIENCE)
        own = pointer_image(env, name)
        cur = env.state().get("wl_cursor") or {}
        check(cur.get("set_by_shape") == 0 and cur.get("set_by_theme") == 0,
              f"{what}: over a wallpaper that takes no pointer input the daemon sets no image", cur)
        # one that does
        env.open(W["ptr"])
        lc.loaded(env, W["ptr"])

        def was_set():
            # (a compositor tells a surface of the pointer when the pointer moves)
            env.display.pointer(100, 100)
            env.display.pointer(300, 200)
            c = env.state().get("wl_cursor") or {}
            return (c.get("set_by_shape") or 0) + (c.get("set_by_theme") or 0) > 0
        wait_for(was_set, 10, 0.3)
        time.sleep(0.5 * PATIENCE)
        cur = env.state().get("wl_cursor") or {}
        by_shape, by_theme = cur.get("set_by_shape") or 0, cur.get("set_by_theme") or 0
        if offered:
            check(by_shape >= 1 and by_theme == 0, f"{what}: the pointer comes onto a wallpaper that takes "
                  "pointer input: the compositor is asked for its default arrow, no image is sent", cur)
        else:
            check(by_theme >= 1 and by_shape == 0, f"{what}: the pointer comes onto a wallpaper that takes "
                  "pointer input: an image from the theme is set", cur)
        over = pointer_image(env, name)
        note(f"{what}: the pointer's image in the picture: {own} over a wallpaper without pointer input, "
             f"{over} over one that takes it (width, height, x, y)")
        if own is None:
            note(f"{what}: this compositor's picture shows no pointer: what it looks like is not checked")
        elif offered:
            check(over == own, f"{what}: it is the compositor's own image there, the same and at the same "
                  "place, whatever size the daemon's environment names", (own, over))
        else:
            check(over is not None, f"{what}: the pointer has an image there", over)
        healthy(env, mark, what)
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
        if not wait_for(lambda: env.display.window_of(p.pid, GUI_APP_ID) or p.poll() is not None,
                        60, 0.5) or p.poll() is not None:
            print("skip covered (the library window did not come up on the compositor)")
            return
        env.display.win_fullscreen(env.display.window_of(p.pid, GUI_APP_ID), True)
        full = wait_for(lambda: (env.display.window_of(p.pid, GUI_APP_ID) or {}).get("fullscreen"), 15)
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
    """the last case: it ends the compositor. The daemon's main thread is made to stand
    still for a while when it has found the display gone (WALPE_LOST_LINGER_MS), so that
    the render thread of an animated wallpaper comes to its next frames meanwhile: it does
    not present them (a swap on a display that is gone is where Mesa's software path was
    seen to crash) and says so once. Before that the same for the one frame that is not a
    wallpaper's: the black one after a load that failed."""
    # A load that fails after the display went: its black frame is a frame like the
    # others. (The load is made to take a second, the compositor is killed inside it.)
    bad = os.path.join(env.tmp, "wallpapers", "broken")
    os.makedirs(bad, exist_ok=True)
    rc.jdump(bad + "/project.json", {"file": "scene.json", "title": "broken", "type": "scene"})
    with open(bad + "/scene.json", "w") as f:
        f.write("{ this is no scene")
    if not start(env, [("A", 640, 360, 0, 0)], "lost, a load that fails",
                 extra={"WALPE_LOST_LINGER_MS": "2500", "WALPE_LOAD_DELAY_MS": "1000"}):
        return
    time.sleep(1.0)
    mark = env.mark()
    check(bool((env.ipc({"cmd": "open", "path": bad}) or {}).get("ok")), "lost: a wallpaper that cannot be "
          "loaded is opened")
    time.sleep(0.25)
    env.display.end(hard=True)
    try:
        status = env.daemon.wait(timeout=30 * PATIENCE)
    except subprocess.TimeoutExpired:
        status = None
    log = env.since(mark)
    gone, failed = log.find("the display connection is gone: shutting down"), log.find("wallpaper load failed")
    if status is not None:
        env.daemon = None
    if check(status == 1 and 0 <= gone < failed, "lost: the compositor is killed, the load fails after that, the "
             "daemon ends with status 1", (status, log[-800:])):
        check(log.count("the display connection is gone: its frames are not presented any more") == 1,
              "lost: the black frame of the wallpaper that could not be loaded is not presented on the "
              "display that is gone", log[-800:])
    if not check(env.display.start(640, 360), "lost: the compositor is started again", env.display.log_tail()):
        return

    if not start(env, [("A", 640, 360, 0, 0)], "lost", extra={"WALPE_LOST_LINGER_MS": "700"}):
        return
    env.open(W["red"])
    lc.loaded(env, W["red"])
    name = env.mons[0][0]
    check(shows(env, name, "red") == "red", "lost: the wallpaper is shown")
    env.open(W["anim"])
    lc.loaded(env, W["anim"])
    mark = env.mark()
    env.display.stop()
    try:
        status = env.daemon.wait(timeout=30 * PATIENCE)
    except subprocess.TimeoutExpired:
        status = None
    check(status == 1, "lost: the compositor is gone, the daemon ends with status 1", status)
    log = env.since(mark)
    check(log.count("the display connection is gone: its frames are not presented any more") == 1 and
          "eglSwapBuffers failed" not in log,
          "lost: until the daemon had stopped its render thread, the thread presented no frame on the display "
          "that was gone, and said so once", log[-800:])
    if status is not None:
        env.daemon = None
    time.sleep(0.5)
    check(not os.path.exists(env.sock), "lost: no control socket is left behind", env.sock)
    check(not os.path.exists(env.sock + ".loading"), "lost: nor the marker of a start that did not finish")


# ---------------------------------------------------------------- reconnect
def reconnect_software(env, W, wait_s):
    """"Render on CPU" (here: WALPE_SOFTWARE=1) is still on when the compositor is back. The
    image that waits for its display asked its questions about the display - the test of
    Mesa's software renderer is one - before the display was there: "software rendering
    is switched on but not available: cannot connect to the Wayland compositor ...
    Rendering on the GPU", for as long as that daemon ran."""
    what = "reconnect, render on CPU"
    if not start(env, [("A", 640, 360, 0, 0)], what, config=dict(CONFIG, current=W["red"]),
                 extra={"WALPE_WAYLAND_RECONNECT": str(wait_s), "WALPE_SOFTWARE": "1"}):
        return
    try:
        lc.loaded(env, W["red"])
        st = env.state()
        if not check(st.get("software_active") is True, f"{what}: the daemon renders on the CPU because it was told to",
                     (st.get("software_active"), st.get("software_note"))):
            return
        pid, mark = env.daemon.pid, env.mark()
        env.display.end(hard=True)
        said = wait_for(lambda: "waiting up to %d s" % wait_s in env.since(mark), 15)
        time.sleep(2.0)
        check(bool(said) and env.display.start(640, 360), f"{what}: the compositor is killed, the daemon waits, "
              "the compositor is started again", env.since(mark)[-600:])
        back = wait_for(lambda: bool(env.state().get("monitors")), 40)
        st = env.state()
        check(bool(back) and env.daemon.poll() is None and env.daemon.pid == pid and lc.loaded(env, W["red"]),
              f"{what}: the daemon is there again, the same process, its wallpaper loaded", env.since(mark)[-800:])
        check(st.get("software_active") is True and not st.get("software_note") and
              "software rendering is switched on but not available" not in env.since(mark),
              f"{what}: it still renders on the CPU by its setting, and no line says otherwise",
              (st.get("software_active"), st.get("software_note"),
               [ln[-200:] for ln in env.since(mark).splitlines() if "software rendering" in ln][:3]))
        healthy(env, mark, what)
    finally:
        env.stop_daemon()


def case_reconnect(env, W):
    """The compositor goes away and comes back in the same place, under a daemon that is
    to wait for it (wayland_reconnect). It runs before `lost`, which ends the compositor
    for good."""
    wait_s = 40
    reconnect_software(env, W, wait_s)
    extra = {"WALPE_WAYLAND_RECONNECT": str(wait_s), "WALPE_WL_SHORT_STAY_MS": "0"}
    if not start(env, [("A", 640, 360, 0, 0)], "reconnect", extra=extra):
        return
    env.open(W["quad"])
    lc.loaded(env, W["quad"])
    name = env.mons[0][0]
    ok, lk = shows_quad(env, name)
    check(ok, "reconnect: the wallpaper is shown", lk)
    pid = env.daemon.pid

    def away(what, hard):
        mark = env.mark()
        env.display.end(hard=hard)
        said = wait_for(lambda: "waiting up to %d s" % wait_s in env.since(mark), 15 * PATIENCE)
        check(bool(said) and env.daemon.poll() is None, f"reconnect: the compositor is {what}: the daemon "
              "restarts in place and waits for it", env.since(mark)[-600:])
        return mark

    def back(what, mark):
        answered = wait_for(lambda: bool(env.state().get("monitors")), 40 * PATIENCE)
        check(bool(answered) and env.daemon.poll() is None and env.daemon.pid == pid,
              f"reconnect: {what}: the daemon is there again, the same process", env.since(mark)[-800:])
        check(lc.loaded(env, W["quad"]), f"reconnect: {what}: its wallpaper is loaded", rc.brief(env.monitors()))
        # (a compositor that starts anew numbers its outputs anew: after the cases before
        # this one the output need not have had the first name)
        now = [m.get("name") for m in env.state().get("monitors") or []]
        ok, lk = shows_quad(env, now[0] if now else name)
        check(ok, f"reconnect: {what}: the wallpaper is shown again", lk)
        healthy(env, mark, f"reconnect: {what}")

    for what, hard in (("told to end", False), ("killed", True)):
        mark = away(what, hard)
        time.sleep(2.0)
        check(env.daemon.poll() is None, f"reconnect: {what}: two seconds later it still waits")
        check(env.display.start(640, 360), f"reconnect: {what}: the compositor is started again",
              env.display.log_tail())
        back(what, mark)

    # The compositor that came back goes away again while the daemon starts on it: at
    # whatever moment of that start, the daemon waits again instead of ending.
    path = env.display.display

    def cut_after(delay):
        t0 = time.time()
        while time.time() - t0 < 20 * PATIENCE:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                s.close()
                break
            except OSError:
                time.sleep(0.005)
        time.sleep(delay)
        p = env.display.proc
        if p is not None:
            p.kill()

    # (Ten short stays in a row end a daemon since it counts them, the fifth does: the
    # case `flap`. What is looked at here is the moment of the daemon's start at which
    # the display goes, and ten of them are tried: this daemon is told that no stay is
    # short, WALPE_WL_SHORT_STAY_MS=0, and so counts none.)
    mark = away("killed", True)
    for delay in (0.0, 0.03, 0.06, 0.09, 0.12, 0.15, 0.2, 0.3, 0.5, 0.9):
        t = threading.Thread(target=cut_after, args=(delay,))
        t.start()
        env.display.start(640, 360)
        t.join()
        env.display.end(hard=True)
        time.sleep(1.0)
        alive = env.daemon.poll() is None
        check(alive, "reconnect: the compositor is killed again %.2f s after its socket was there: "
              "the daemon waits on" % delay, env.since(mark)[-1200:])
        if not alive:
            env.daemon = None
            return
        mark = env.mark()
    check(env.display.start(640, 360), "reconnect: after that the compositor is started and stays",
          env.display.log_tail())
    back("after ten short visits of the compositor", mark)


# --------------------------------------------------------------------- flap
class WlFront:
    """A Wayland socket in front of the compositor's own: every connection is handed
    through, descriptors too, and can be ended from here the two ways a compositor ends
    one. cut = seconds: each connection is closed that long after it was made (a
    compositor that dies and is started again, one that does not keep this client).
    refuse (): the wallpaper client's connection gets the sentence a compositor sends
    when it objects to a request (wl_display.error, here: about the client's registry,
    its object 2) and is closed after it; refuse_new = True: every connection made from
    now on gets that at its first request, about the display itself ("invalid method":
    libwayland reports the two kinds differently)."""

    class Conn:
        def __init__(self):
            self.t0, self.refuse, self.layer, self.live = time.time(), None, False, True
            self.hdr, self.left = b"", 0    # where the compositor's stream stands in a message

        def fed(self, data):
            i = 0
            while i < len(data):
                if self.left:
                    n = min(self.left, len(data) - i)
                    self.left -= n
                else:
                    n = min(8 - len(self.hdr), len(data) - i)
                    self.hdr += data[i:i + n]
                    if len(self.hdr) == 8:
                        self.left = max((struct.unpack("=II", self.hdr)[1] >> 16) - 8, 0)
                        self.hdr = b""
                i += n

        def between(self):
            return not self.left and not self.hdr

    def __init__(self, path, target):
        self.path, self.target, self.cut, self.refuse_new, self.n, self.on = path, target, None, False, 0, True
        self.conns = []
        try:
            os.unlink(path)
        except OSError:
            pass
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(8)
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while self.on:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            self.n += 1
            k = self.Conn()
            k.refuse = (1, 1) if self.refuse_new else None
            self.conns.append(k)
            threading.Thread(target=self._pump, args=(c, k), daemon=True).start()

    def refuse(self):
        """-> how many connections are told"""
        told = [k for k in self.conns if k.live and k.layer]
        for k in told:
            k.refuse = (2, 7)
        return len(told)

    @staticmethod
    def _error(obj, code, text):
        # wl_display@1.error(object, code, text): header (object 1, size << 16 | opcode 0), body
        raw = text.encode() + b"\0"
        raw += b"\0" * (-len(raw) % 4)
        body = struct.pack("=III", obj, code, len(text.encode()) + 1) + raw
        return struct.pack("=II", 1, (8 + len(body)) << 16) + body

    def _pump(self, c, k):
        u = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            u.connect(self.target)
            asked = False
            while True:
                if k.refuse and asked and k.between():
                    c.sendall(self._error(k.refuse[0], k.refuse[1], "the stand-in compositor objects"))
                    time.sleep(0.3)     # (its sentence is read before the socket's end)
                    break
                # (a connection that is to be refused is not cut before that)
                left = None if self.cut is None or k.refuse else self.cut - (time.time() - k.t0)
                if left is not None and left <= 0:
                    break
                r, _, _ = select.select([c, u], [], [], 0.1 if left is None else max(min(0.1, left), 0))
                for s in r:
                    o = u if s is c else c
                    data, anc, _, _ = s.recvmsg(65536, socket.CMSG_SPACE(28 * 4))
                    if not data and not anc:
                        raise OSError("closed")
                    fds = array.array("i")
                    for lvl, typ, cd in anc:
                        if lvl == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                            fds.frombytes(cd[:len(cd) - (len(cd) % fds.itemsize)])
                    if s is c:
                        asked = True
                        k.layer = k.layer or b"zwlr_layer_shell_v1" in data
                    else:
                        k.fed(data)
                    o.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds)] if len(fds) else [])
                    for fd in fds:
                        os.close(fd)
        except OSError:
            pass
        finally:
            k.live = False
            c.close()
            u.close()

    def stop(self):
        self.on = False
        self.srv.close()
        try:
            os.unlink(self.path)
        except OSError:
            pass


GAVE_UP = "(wayland_reconnect = %d gives up)"
REFUSED = "(wayland_reconnect = %d does not apply)"
RESTARTS = "restarting in place, waiting up to"


def flap_start(env, W, front, what, wait_s, extra=None):
    """a daemon with wayland_reconnect whose compositor is the stand-in; -> started"""
    front.cut, front.refuse_new = None, False
    cfg = dict(CONFIG, current=W["anim"])
    ex = {"WAYLAND_DISPLAY": front.path, "WALPE_WAYLAND_RECONNECT": str(wait_s)}
    ex.update(extra or {})
    return start(env, [("A", 640, 360, 0, 0)], what, extra=ex, config=cfg) and lc.loaded(env, W["anim"])


def flap_ended(env, timeout):
    """wait for the daemon to end by itself -> (status or None, its children seen meanwhile)"""
    pid, kids, t0 = env.daemon.pid, set(), time.time()
    while time.time() - t0 < timeout and env.daemon.poll() is None:
        kids.update(lc.children(pid))
        time.sleep(0.2)
    status = env.daemon.poll()
    if status is not None:
        env.daemon = None
    return status, kids


def flap_bound(env, W, front, wait_s):
    """every connection ends 3 s after it was made: the fifth short stay in a row ends the
    daemon. (Before: a restart in place every three seconds for as long as the compositor
    went on like this, 29 in 90 seconds.)"""
    what = "flap"
    t_start = time.time()
    if not flap_start(env, W, front, what, wait_s):
        return
    mark, n0 = env.mark(), front.n
    first_short = time.time() - t_start < 25     # (the first stay is one of the five)
    front.cut = 3.0
    t0 = time.time()
    status, kids = flap_ended(env, 60 + 40 * PATIENCE)
    took, log = time.time() - t0, env.since(mark)
    restarts = log.count(RESTARTS)
    if not check(status == 1, f"{what}: a compositor that ends each of the daemon's connections after 3 s: "
                 "the daemon ends by itself, with status 1", (status, restarts, log[-1500:])):
        return
    note(f"{what}: ended after {took:.1f} s, {restarts} restarts in place, {front.n - n0} more connections")
    check(restarts == 4 if first_short else restarts in (4, 5), f"{what}: after four restarts in place: the "
          "fifth stay of less than 30 s in a row is the last", (restarts, first_short, log[-1500:]))
    check(log.count(GAVE_UP % wait_s) == 1 and log.count("the display connection is gone: shutting down") == 1,
          f"{what}: it says once that wayland_reconnect gives up, and ends as it does without the setting",
          log[-1500:])
    check("short stay 4 in a row (at 5 the daemon ends instead of waiting again)" in log,
          f"{what}: each short stay was counted in the log before that")
    time.sleep(0.5)
    check(not os.path.exists(env.sock) and not os.path.exists(env.sock + ".loading"),
          f"{what}: no control socket and no marker of an unfinished start are left behind")
    left = [k for k in kids if os.path.exists(f"/proc/{k}")]
    check(not left, f"{what}: no child process of the daemon is left", left)
    bad = [ln for ln in log.splitlines() if TROUBLE.search(ln)]
    check(not bad, f"{what}: nothing else went wrong on the way", bad[:5])


def flap_long_stay(env, W, front, wait_s):
    """one stay that is not short and the count begins anew. "Short" is 8 s here."""
    what = "flap, a long stay"
    if not flap_start(env, W, front, what, wait_s, extra={"WALPE_WL_SHORT_STAY_MS": "8000"}):
        return
    pid = env.daemon.pid
    fourth = "short stay 4 in a row"

    def four_short(mark):
        front.cut = 3.0
        seen = wait_for(lambda: fourth in env.since(mark) or env.daemon.poll() is not None, 40)
        front.cut = None
        time.sleep(0.5)     # (the line of the restart follows the count's)
        return bool(seen) and env.daemon.poll() is None

    mark = env.mark()
    if not check(four_short(mark), f"{what}: four short stays in a row, the daemon restarts in place for each",
                 env.since(mark)[-1500:]):
        return
    back = wait_for(lambda: bool(env.state().get("monitors")), 40)
    check(bool(back) and lc.loaded(env, W["anim"]), f"{what}: the compositor keeps the connection now: the "
          "daemon is there with its wallpaper", env.since(mark)[-800:])
    time.sleep(9.0)
    mark2 = env.mark()
    ok = four_short(mark2)
    log = env.since(mark2)
    check(ok and env.daemon.pid == pid and (GAVE_UP % wait_s) not in env.since(mark),
          f"{what}: after a stay of more than 8 s four more short ones are waited through (eight short stays "
          "and the daemon is the same process)", log[-1500:])
    check(log.count(RESTARTS) == 5 and "short stay 1 in a row" in log,
          f"{what}: the loss after the long stay was not counted, the next one was the first again",
          (log.count(RESTARTS), log[-1500:]))
    mark3 = env.mark()
    front.cut = 3.0
    status, _ = flap_ended(env, 30 + 20 * PATIENCE)
    log = env.since(mark3)
    check(status == 1 and log.count(GAVE_UP % wait_s) == 1 and log.count(RESTARTS) == 0,
          f"{what}: the fifth in a row ends this daemon too", (status, log[-1500:]))


def flap_refused(env, W, front, wait_s):
    """the compositor objects to a request of the running daemon (a protocol error).
    (Before: the daemon restarted in place and came back to it.)"""
    what = "flap, a protocol error"
    if not flap_start(env, W, front, what, wait_s):
        return
    mark, n0 = env.mark(), front.n
    told = front.refuse()
    t0 = time.time()
    status, _ = flap_ended(env, 15 + 10 * PATIENCE)
    took, log = time.time() - t0, env.since(mark)
    check(told == 1 and status == 1 and "wayland: protocol error 7 on wl_registry@2" in log,
          f"{what}: the daemon's connection is ended with one: it ends with status 1 and says which",
          (told, status, log[-1500:]))
    check(log.count(RESTARTS) == 0 and log.count(REFUSED % wait_s) == 1 and front.n == n0,
          f"{what}: it does not restart in place and makes no new connection (wayland_reconnect does not "
          "apply, said once)", (front.n - n0, log[-1500:]))
    note(f"{what}: ended {took:.1f} s after the error")
    time.sleep(0.5)
    check(not os.path.exists(env.sock), f"{what}: no control socket is left behind")


def flap_refused_start(env, W, front, wait_s):
    """... and to the first request of a daemon that waits for it. (Before: the connection
    "closed during start-up" was made again five times a second for the whole of the wait.)"""
    what = "flap, a protocol error at the start"
    if not flap_start(env, W, front, what, wait_s):
        return
    mark, n0 = env.mark(), front.n
    front.refuse_new = True
    front.cut = 0.01
    waits = wait_for(lambda: RESTARTS in env.since(mark) or env.daemon.poll() is not None, 20)
    t0 = time.time()
    status, _ = flap_ended(env, wait_s + 10 * PATIENCE)
    took, log = time.time() - t0, env.since(mark)
    check(bool(waits) and status == 1 and "with a protocol error (1 on wl_display@1): it is not tried again "
          "(wayland_reconnect)" in log, f"{what}: the daemon that restarted in place to wait ends with status 1 "
          "and says why", (status, log[-1500:]))
    check(front.n - n0 == 1 and took < wait_s / 2, f"{what}: after one connection, not at the end of the "
          f"wait of {wait_s} s", (front.n - n0, round(took, 1)))
    time.sleep(0.5)
    check(not os.path.exists(env.sock), f"{what}: no control socket is left behind")


def case_flap(env, W):
    """A compositor that does not keep the daemon's connection, and one that objects to
    it: a daemon with wayland_reconnect does not come back to either without end. Runs
    after `reconnect` (the compositor that goes and comes back and is waited for)."""
    front = WlFront(os.path.join(env.display.rt, "wayland-front"), env.display.display)
    try:
        for part in (flap_bound, flap_long_stay, flap_refused, flap_refused_start):
            try:
                part(env, W, front, 20)
            finally:
                env.stop_daemon()
    finally:
        front.stop()


# ------------------------------------------------------------------- cycles
def case_cycles(env, W, rounds=6):
    """not in the default run (it is two restarts in place a round, ten seconds apart at
    least); then with wayland_follow on, where a render thread that waits is used again"""
    cycles_one(env, W, rounds, None)
    cycles_one(env, W, rounds, FOLLOW_ON)


def cycles_one(env, W, rounds, extra):
    if not start(env, [("A", 640, 360, 0, 0)], "cycles", extra=extra):
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

    def threads():
        try:
            return len(os.listdir(f"/proc/{pid}/task"))
        except OSError:
            return -1
    counts, tcounts = [], []
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
        tcounts.append(threads())
    check(len(counts) == rounds and counts[-1] <= counts[1],
          "cycles: no more open files after the last round than after the second", counts)
    check(len(tcounts) == rounds and tcounts[-1] <= tcounts[1],
          "cycles: no more threads after the last round than after the second (a render thread that waits "
          "is used again)", tcounts)
    check(env.daemon.poll() is None and env.daemon.pid == pid, "cycles: same process throughout")
    healthy(env, mark, "cycles")
    env.stop_daemon()


# --------------------------------------------------------------- fullscreen
def fullscreen_one(env, W, gui):
    """The fullscreen rule on one output, for a compositor that is driven through the
    window list (wlr-foreign-toplevel-management): that can make a window fullscreen and
    give it the focus, but cannot move it to an output (labwc 0.20 overlooks the output
    named in set_fullscreen) or take the focus from every window."""
    if not start(env, [("A", 640, 360, 0, 0)], "fullscreen"):
        return
    mark = env.mark()
    env.open(W["anim"])
    lc.loaded(env, W["anim"])

    def mon():
        return (env.state().get("monitors") or [{}])[0]

    def drawn(seconds=2.0):
        n = mon().get("frames") or 0
        time.sleep(seconds * PATIENCE)
        return (mon().get("frames") or 0) - n

    def is_state(full, what):
        ok = wait_for(lambda: bool(mon().get("fullscreen")) == full and bool(mon().get("paused")) == full, 20, 0.2)
        st = env.state()
        check(bool(ok), "fullscreen: %s: fullscreen %s, paused %s" % (what, full, full),
              {"fullscreen": mon().get("fullscreen"), "paused": mon().get("paused"),
               "monitors_state": st.get("monitors_state"), "windows": env.display.windows()})
        n = drawn()
        check(n <= 2 if full else n > 3, "fullscreen: %s: the scene %s" % (what, "stops" if full else "draws"),
              "%d frames in %.0f s" % (n, 2.0 * PATIENCE))
        return st

    wait_for(lambda: drawn(1.0) > 3, 20, 0.1)
    st = is_state(False, "no window")
    sig = st.get("pause_signals") or {}
    if not check((sig.get("fullscreen") or {}).get("from") == "wl-fullscreen",
                 "fullscreen: the compositor's window list reports fullscreen windows", sig.get("fullscreen")):
        env.stop_daemon()
        return
    genv = dict(env.run_env)
    genv.update({"GDK_BACKEND": "wayland", "GSK_RENDERER": "cairo", "GTK_A11Y": "none"})
    with open(os.path.join(env.tmp, "gui.log"), "ab") as log:
        p = subprocess.Popen([gui], env=genv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                             cwd=env.tmp)
    try:
        if not wait_for(lambda: env.display.window_of(p.pid, GUI_APP_ID) or p.poll() is not None,
                        60, 0.5) or p.poll() is not None:
            print("skip fullscreen (the library window did not come up on the compositor)")
            return

        def win():
            return env.display.window_of(p.pid, GUI_APP_ID) or {}
        env.display.win_focus(win())
        is_state(False, "a window that is not fullscreen")
        for again in ("", ", once more"):
            env.display.win_fullscreen(win(), True)
            env.display.win_focus(win())
            check(bool(wait_for(lambda: win().get("fullscreen"), 15)),
                  "fullscreen: the compositor shows the library window fullscreen" + again, win())
            st = is_state(True, "the window fullscreen with the focus" + again)
            check(st.get("fullscreen") is True and st.get("fullscreen_pause") is True,
                  "fullscreen: the status of all monitors: a fullscreen window has the focus, the rule pauses",
                  {k: st.get(k) for k in ("fullscreen", "fullscreen_pause")})
            if not again:
                env.display.win_fullscreen(win(), False)
                is_state(False, "the window not fullscreen any more")
    finally:
        p.terminate()
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    is_state(False, "the window is gone")
    healthy(env, mark, "fullscreen")
    env.stop_daemon()


def case_fullscreen(env, W):
    gui = os.path.join(env.build, "walpe")
    if not os.path.exists(gui):
        print("skip fullscreen (no library window program in the build)")
        return
    if env.display.kind != "sway":
        note("fullscreen: on %s the rule is looked at on one output (windows cannot be moved between "
             "outputs through the window list)" % env.display.kind)
        return fullscreen_one(env, W, gui)
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
        if not wait_for(lambda: env.display.window_of(p.pid, GUI_APP_ID) or p.poll() is not None,
                        60, 0.5) or p.poll() is not None:
            print("skip fullscreen (the library window did not come up on the compositor)")
            return

        def win():
            return env.display.window_of(p.pid, GUI_APP_ID) or {}
        # the focus is moved between outputs below, and sway would take the pointer along:
        # a pointer left at an output's edge would be in the pictures of the cases that
        # follow. The pointer stays where it is.
        env.display.pointer_follows_focus(False)

        def put(out):
            """the window fullscreen on this output, with the focus"""
            env.display.win_fullscreen(win(), True, out)
            env.display.win_focus(win())

        # a plain window with the focus pauses nothing
        env.display.win_focus(win(), a)
        is_state([False, False], [False, False], "a window that is not fullscreen")
        put(a)
        check(bool(wait_for(lambda: win().get("fullscreen"), 15)),
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
        env.display.focus_output(a)
        st = is_state([False, False], [False, False], "the focus is on the other output")
        check(st.get("fullscreen") is False, "fullscreen: ... and no fullscreen window has the focus",
              st.get("fullscreen"))
        env.display.win_focus(win())
        is_state([False, True], [False, True], "the focus is back on the fullscreen window")
    finally:
        p.terminate()
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        env.display.focus_output(a)                 # as the cases before left it
        env.display.pointer_follows_focus(True)
    is_state([False, False], [False, False], "the window is gone")
    healthy(env, mark, "fullscreen")
    env.stop_daemon()


OPT_IN = ("cycles",)
CASES = [("start", case_start), ("layer", case_layer), ("two", case_two), ("mode", case_mode), ("scale", case_scale),
         ("noviewport", case_noviewport), ("rotate", case_rotate), ("hotplug", case_hotplug),
         ("follow", case_follow), ("closed", case_closed), ("nosurface", case_nosurface),
         ("pointer", case_pointer), ("covered", case_covered), ("fullscreen", case_fullscreen),
         ("backend", case_backend),
         ("noshell", case_noshell), ("cycles", case_cycles), ("cursor", case_cursor),
         ("reconnect", case_reconnect), ("flap", case_flap),
         ("lost", case_lost)]


def main():
    args = sys.argv[1:]
    build, daemon, keep, tmp, only, display = "build", None, False, None, None, None
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
        elif a == "--display" and args:
            display = args.pop(0)
        else:
            sys.stderr.write(__doc__)
            return 2
    # the compositor: --display, else WALPE_CHECK_DISPLAY when it names one, else sway
    kind = displayenv.pick(display)
    if kind is None or (display and displayenv.WINSYS[kind] != "wayland"):
        sys.stderr.write("waylandcheck: --display takes a Wayland compositor: %s\n"
                         % ", ".join(sorted(k for k, w in displayenv.WINSYS.items() if w == "wayland")))
        return 2
    if displayenv.WINSYS[kind] != "wayland":
        kind = "sway"
    skip = displayenv.unusable(kind, build)
    if skip:
        print("waylandcheck: skipped (%s)" % skip[0])
        return skip[1]
    for b in (daemon or os.path.join(build, "walpe-daemon"), os.path.join(build, "walpe-ctl")):
        if not os.path.exists(b):
            print(f"waylandcheck: {b} not found")
            return 2
    env = lc.Env(build, daemon, keep, tmp, kind)
    try:
        if kind != "sway":
            print("waylandcheck: the compositor is %s" % kind)
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
