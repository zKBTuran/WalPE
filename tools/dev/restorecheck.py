#!/usr/bin/env python3
"""End-to-end check: a wallpaper that fails to load brings the previous one back.

  python3 -I tools/dev/restorecheck.py --build build [--daemon FILE] [--display KIND] [--keep] [--tmp DIR]

Starts a private Xvfb (-displayfd, its framebuffer in a file: no screenshot
tool needed) with two fake monitors, a private walpe-daemon (own XDG dirs in
a temp dir) and three generated scenes - "red", "blue" and "broken" (a
project.json that says scene, a scene.json that is not JSON) - then drives
`walpe-ctl open` and reads back `walpe-ctl status`, config.json, the daemon
log and the pixels in the middle of each monitor:

  same-size monitors (one renders, the other follows it):
   1 no wallpaper before, broken opened      black, load_error, nothing re-opened
   2 red on both, broken opened              red is back on both monitors, load_error
                                             names broken, load_restored names red,
                                             config.json names red again; re-opened once
   3 red / blue per monitor, broken opened   each monitor has its own back, config too
   4 broken opened on one monitor            that monitor only
   5 blue deleted ("forget"), then broken    the monitor returns to what it showed last
     on that monitor                         (red), not to the deleted wallpaper
   6 everything deleted, then broken         black, load_error, nothing re-opened
   7 red shown, its files then damaged,      black on both, load_error names both, one
     broken opened (both fail)               re-open and nothing after it
   9 red deleted while broken loads          black on both, no stale picture
  monitors of different sizes (each loads on its own):
   8 red / blue per monitor, broken opened   each monitor has its own back, config too

The display is Xvfb, or with --display sway (or WALPE_CHECK_DISPLAY=sway) a headless
Wayland compositor with two outputs (scripts/displayenv.py of the source tree).

Exit code 0 = all passed, 1 = failures, 77 = skipped (the display's programs are
not installed, or the build has no backend for it). --daemon FILE
runs another walpe-daemon binary with the tools of --build (a build without
the change: cases 2-5, 7 and 8 fail there, the monitors stay black).
RESTORECHECK_PATIENCE=N multiplies every wait (a software renderer on a busy
machine: 3 or more). --tmp DIR: work in DIR (must not exist; removed
afterwards) - the daemon's socket lives below it and a socket path is
limited to 107 bytes.

The harness (private X server, private daemon, generated scenes, pixel reader) is
scripts/checkenv.py of the WalPE source tree: it is looked for in $WALPE_SRC/scripts, else
in scripts/ next to the directory given as --build."""
import os, shutil, sys, time

sys.dont_write_bytecode = True   # no __pycache__ in the source tree


def _harness_dir():
    src = os.environ.get("WALPE_SRC")
    if not src:
        build = "build"
        for i, a in enumerate(sys.argv):
            if a == "--build" and i + 1 < len(sys.argv):
                build = sys.argv[i + 1]
        src = os.path.dirname(os.path.abspath(build))
    return os.path.join(src, "scripts")


sys.path.insert(0, _harness_dir())
try:
    from checkenv import PATIENCE, wait_for, jdump, gen_scene, gen_broken, Env, name_of, brief   # noqa: E402,F401
    import displayenv   # noqa: E402  (which display, and whether it can be used here)
except ImportError:
    sys.stderr.write("restorecheck: scripts/checkenv.py of the WalPE source tree was not found in %s\n"
                     "(set WALPE_SRC to the source tree, or pass --build <source tree>/build)\n" % sys.path[0])
    sys.exit(2)

AGAIN = "opening the previous wallpaper again"
FAILED = "wallpaper load failed"

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


def shows(env, want):
    """wait until the monitors show these colours; -> what they show"""
    def look():
        return [name_of(env.colour(m)) for m in env.mons]
    wait_for(lambda: look() == list(want), 12)
    return look()


def settled(env, done, timeout=20):
    """wait until status satisfies done(monitors); -> monitors"""
    box = [[]]

    def ok():
        box[0] = env.monitors()
        return len(box[0]) == len(env.mons) and done(box[0])
    wait_for(ok, timeout, 0.3)
    if len(box[0]) != len(env.mons):
        check(False, "walpe-ctl status lists the monitors", box[0])
        return [{} for _ in env.mons]
    return box[0]


def base(p):
    return os.path.basename(p) if p else p


def run_same(env, red, blue, broken):
    m0, m1 = env.mons
    # 1: nothing before
    n_again, n_failed = env.count(AGAIN), env.count(FAILED)
    check(env.open(broken), "1 open broken with no wallpaper before: accepted (the load runs later)")
    ms = settled(env, lambda ms: any(m.get("load_error") for m in ms))
    time.sleep(1.0 * PATIENCE)
    ms = env.monitors() or ms
    check(any(broken in (m.get("load_error") or "") for m in ms) and not any(m.get("load_restored") for m in ms),
          "1 status: load_error names broken, nothing restored", brief(ms))
    check(shows(env, ["black", "black"]) == ["black", "black"], "1 both monitors black")
    check(env.count(AGAIN) == n_again, "1 nothing was re-opened")

    # 2: red on both, then broken
    check(env.open(red), "2 open red")
    ms = settled(env, lambda ms: all(m.get("type") == "scene" and not m.get("load_error") for m in ms))
    check(shows(env, ["red", "red"]) == ["red", "red"], "2 red on both monitors, no load_error", brief(ms))
    check(sum(1 for m in ms if m.get("follows") is not None) == 1, "2 one monitor follows the other", brief(ms))
    n_again, n_failed = env.count(AGAIN), env.count(FAILED)
    check(env.open(broken), "2 open broken: accepted")
    ms = settled(env, lambda ms: all(m.get("load_restored") for m in ms))
    check(all(m.get("current") == red and m.get("type") == "scene" for m in ms),
          "2 status: both monitors are on red again", brief(ms))
    check(all(broken in (m.get("load_error") or "") for m in ms),
          "2 status: load_error names broken on both monitors", brief(ms))
    check(all(m.get("load_restored") == red for m in ms), "2 status: load_restored names red", brief(ms))
    got = shows(env, ["red", "red"])
    check(got == ["red", "red"], "2 red is back on both monitors", got)
    check(sum(1 for m in ms if m.get("follows") is not None) == 1, "2 one monitor follows the other again",
          brief(ms))
    cur, own = env.saved()
    check(cur == red and not own, "2 config.json: red is the saved wallpaper again", (cur, own))
    time.sleep(1.5 * PATIENCE)
    check(env.count(AGAIN) == n_again + 1 and env.count(FAILED) == n_failed + 1,
          "2 one failed load, one re-open", (env.count(AGAIN) - n_again, env.count(FAILED) - n_failed))

    # 3: per-monitor wallpapers
    check(env.open(blue, 1), "3 open blue on monitor 1")
    ms = settled(env, lambda ms: ms[1].get("current") == blue and ms[1].get("type") == "scene" and
                 not ms[1].get("load_error"))
    got = shows(env, ["red", "blue"])
    check(got == ["red", "blue"], "3 red / blue", got)
    check(not ms[1].get("load_error"), "3 the next good load ends load_error on that monitor", brief(ms))
    n_again, n_failed = env.count(AGAIN), env.count(FAILED)
    check(env.open(broken), "3 open broken on every monitor: accepted")
    ms = settled(env, lambda ms: all(m.get("load_restored") for m in ms))
    check(ms[0].get("current") == red and ms[1].get("current") == blue and
          ms[0].get("load_restored") == red and ms[1].get("load_restored") == blue,
          "3 status: each monitor is on its own wallpaper again", brief(ms))
    check(all(broken in (m.get("load_error") or "") for m in ms), "3 status: load_error names broken", brief(ms))
    got = shows(env, ["red", "blue"])
    check(got == ["red", "blue"], "3 red / blue are back", got)
    cur, own = env.saved()
    check(cur == red and own == {m1[0]: blue}, "3 config.json: default red, monitor 1 blue, as before",
          (cur, own))
    time.sleep(1.5 * PATIENCE)
    check(env.count(AGAIN) == n_again + 1 and env.count(FAILED) == n_failed + 1,
          "3 one failed load, one re-open", (env.count(AGAIN) - n_again, env.count(FAILED) - n_failed))

    # 4: one monitor only
    check(env.open(red), "4 open red on every monitor")
    ms = settled(env, lambda ms: all(m.get("current") == red and m.get("type") == "scene" and
                                     not m.get("load_error") for m in ms))
    check(env.open(blue, 1), "4 open blue on monitor 1")
    ms = settled(env, lambda ms: ms[1].get("current") == blue and ms[1].get("type") == "scene")
    shows(env, ["red", "blue"])
    n_again = env.count(AGAIN)
    check(env.open(broken, 1), "4 open broken on monitor 1: accepted")
    ms = settled(env, lambda ms: ms[1].get("load_restored"))
    check(ms[1].get("current") == blue and ms[1].get("load_restored") == blue and
          broken in (ms[1].get("load_error") or ""), "4 status: monitor 1 is on blue again", brief(ms))
    check(ms[0].get("current") == red and not ms[0].get("load_error") and not ms[0].get("load_restored"),
          "4 status: monitor 0 is not concerned", brief(ms))
    got = shows(env, ["red", "blue"])
    check(got == ["red", "blue"], "4 red / blue", got)
    cur, own = env.saved()
    check(cur == red and own == {m1[0]: blue}, "4 config.json as before", (cur, own))
    check(env.count(AGAIN) == n_again + 1, "4 one re-open")

    # 5: the previous wallpaper was deleted meanwhile
    rep = env.ipc({"cmd": "forget", "path": blue})
    check(bool(rep and rep.get("ok")), "5 forget blue (the delete flow)", rep)
    ms = settled(env, lambda ms: ms[1].get("current") == red and not ms[1].get("load_error"))
    got = shows(env, ["red", "red"])
    check(got == ["red", "red"], "5 monitor 1 follows the default (red) again", got)
    n_again = env.count(AGAIN)
    check(env.open(broken, 1), "5 open broken on monitor 1: accepted")
    ms = settled(env, lambda ms: ms[1].get("load_restored"))
    check(ms[1].get("current") == red and ms[1].get("load_restored") == red,
          "5 status: monitor 1 returns to red, not to the deleted blue", brief(ms))
    got = shows(env, ["red", "red"])
    check(got == ["red", "red"], "5 red on both monitors", got)
    cur, own = env.saved()
    check(cur == red and not own, "5 config.json: red, no own choice", (cur, own))
    check(env.count(AGAIN) == n_again + 1, "5 one re-open")

    # 6: a deliberate unload leaves nothing to return to
    rep = env.ipc({"cmd": "forget", "path": red})
    check(bool(rep and rep.get("ok")), "6 forget red", rep)
    ms = settled(env, lambda ms: all(not m.get("current") and m.get("type") != "scene" for m in ms))
    got = shows(env, ["black", "black"])
    check(got == ["black", "black"] and not any(m.get("load_error") or m.get("load_restored") for m in ms),
          "6 both monitors black, no load_error: the unload is not answered with a re-open", (got, brief(ms)))
    n_again = env.count(AGAIN)
    check(env.open(broken), "6 open broken: accepted")
    ms = settled(env, lambda ms: any(m.get("load_error") for m in ms))
    time.sleep(1.5 * PATIENCE)
    ms = env.monitors() or ms
    check(any(broken in (m.get("load_error") or "") for m in ms) and not any(m.get("load_restored") for m in ms),
          "6 status: load_error, nothing restored", brief(ms))
    check(shows(env, ["black", "black"]) == ["black", "black"], "6 both monitors black")
    check(env.count(AGAIN) == n_again, "6 nothing was re-opened")

    # 7: the previous wallpaper fails too
    check(env.open(red), "7 open red")
    ms = settled(env, lambda ms: all(m.get("current") == red and m.get("type") == "scene" and
                                     not m.get("load_error") for m in ms))
    check(shows(env, ["red", "red"]) == ["red", "red"], "7 red on both monitors")
    scene = os.path.join(red, "scene.json")
    os.rename(scene, scene + ".good")
    with open(scene, "w") as f:
        f.write("damaged\n")
    try:
        n_again, n_failed = env.count(AGAIN), env.count(FAILED)
        check(env.open(broken), "7 red's files damaged, open broken: accepted")
        ms = settled(env, lambda ms: all("could not be opened again" in (m.get("load_error") or "") for m in ms))
        time.sleep(2.0 * PATIENCE)
        ms = env.monitors() or ms
        check(all(broken in (m.get("load_error") or "") and red in (m.get("load_error") or "") and
                  "could not be opened again" in (m.get("load_error") or "") for m in ms),
              "7 status: load_error names broken and the previous wallpaper", brief(ms))
        check(not any(m.get("load_restored") for m in ms) and all(m.get("type") != "scene" for m in ms),
              "7 status: nothing restored, no wallpaper", brief(ms))
        got = shows(env, ["black", "black"])
        check(got == ["black", "black"], "7 both monitors black", got)
        # broken once; red once by the monitor that renders for both, then once by
        # each monitor on its own (what the shared-rendering fallback does after
        # any failed load of a group) - and then nothing more
        seen = (env.count(AGAIN) - n_again, env.count(FAILED) - n_failed)
        check(seen[0] == 1 and 2 <= seen[1] <= 4, "7 one re-open, the previous wallpaper tried by it only", seen)
        time.sleep(3.0 * PATIENCE)
        later = (env.count(AGAIN) - n_again, env.count(FAILED) - n_failed)
        check(later == seen, "7 no further attempt", (seen, later))
    finally:
        os.replace(scene + ".good", scene)
    check(env.open(red), "7 red repaired and opened again")
    ms = settled(env, lambda ms: all(m.get("type") == "scene" and not m.get("load_error") for m in ms))
    check(shows(env, ["red", "red"]) == ["red", "red"] and not any(m.get("load_error") for m in ms),
          "7 red on both monitors, load_error gone", brief(ms))

    # 9: the previous wallpaper is deleted while the broken one loads (whichever
    # the daemon sees first: no wallpaper is left to show, and none may stay up)
    reps = env.ipc_burst([{"cmd": "open", "path": broken}, {"cmd": "forget", "path": red}])
    check(len(reps) == 2 and all(r.get("ok") for r in reps), "9 open broken and forget red at once", reps)
    time.sleep(2.0 * PATIENCE)
    got = shows(env, ["black", "black"])
    ms = env.monitors()
    check(got == ["black", "black"], "9 both monitors black: no stale picture of the deleted wallpaper", got)
    check(not any(m.get("type") == "scene" or m.get("load_restored") for m in ms),
          "9 status: no wallpaper, nothing restored", brief(ms))


def run_mixed(env, red, blue, broken):
    m0, m1 = env.mons
    check(env.open(red), "8 open red")
    settled(env, lambda ms: all(m.get("type") == "scene" and not m.get("load_error") for m in ms))
    check(env.open(blue, 1), "8 open blue on monitor 1")
    ms = settled(env, lambda ms: ms[1].get("current") == blue and ms[1].get("type") == "scene")
    got = shows(env, ["red", "blue"])
    check(got == ["red", "blue"] and not any(m.get("follows") is not None for m in ms),
          "8 red / blue, each monitor renders its own", (got, brief(ms)))
    n_again, n_failed = env.count(AGAIN), env.count(FAILED)
    check(env.open(broken), "8 open broken on every monitor: accepted")
    ms = settled(env, lambda ms: all(m.get("load_restored") for m in ms))
    check(ms[0].get("current") == red and ms[1].get("current") == blue and
          ms[0].get("load_restored") == red and ms[1].get("load_restored") == blue and
          all(broken in (m.get("load_error") or "") for m in ms),
          "8 status: each monitor is on its own wallpaper again, load_error names broken", brief(ms))
    got = shows(env, ["red", "blue"])
    check(got == ["red", "blue"], "8 red / blue are back", got)
    cur, own = env.saved()
    check(cur == red and own == {m1[0]: blue}, "8 config.json: default red, monitor 1 blue, as before",
          (cur, own))
    time.sleep(1.5 * PATIENCE)
    check(env.count(AGAIN) == n_again + 2 and env.count(FAILED) == n_failed + 2,
          "8 two failed loads (one per monitor), one re-open each",
          (env.count(AGAIN) - n_again, env.count(FAILED) - n_failed))


def main():
    args = sys.argv[1:]
    build, daemon, keep, tmp, display = "build", None, False, None, None
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
        else:
            sys.stderr.write(__doc__)
            return 2
    display = displayenv.pick(display)
    if not display:
        sys.stderr.write(__doc__)
        return 2
    skip = displayenv.unusable(display, build)
    if skip:
        print("restorecheck: skipped (%s)" % skip[0])
        return skip[1]
    for b in (daemon or os.path.join(build, "walpe-daemon"), os.path.join(build, "walpe-ctl")):
        if not os.path.exists(b):
            print(f"restorecheck: {b} not found")
            return 2
    env = Env(build, daemon, keep, tmp, display)
    try:
        if len(env.sock.encode()) > 107:
            print(f"restorecheck: {env.sock} is too long for a socket ({len(env.sock.encode())} > 107 bytes): "
                  "pass --tmp <a short directory>")
            return 2
        red = os.path.join(env.tmp, "wallpapers", "red")
        blue = os.path.join(env.tmp, "wallpapers", "blue")
        broken = os.path.join(env.tmp, "wallpapers", "broken")
        gen_scene(red, (1, 0, 0))
        gen_scene(blue, (0, 0, 1))
        gen_broken(broken)
        layouts = [(run_same, [("A", 640, 360, 0, 0), ("B", 640, 360, 640, 0)]),
                   (run_mixed, [("A", 640, 360, 0, 0), ("B", 480, 270, 640, 0)])]
        if not env.start_x(1280, 360):
            print("restorecheck: skipped (the %s display did not start)" % display)
            return displayenv.SKIP
        for fn, mons in layouts:
            jdump(env.config, {"config_version": 2, "fps": 30, "pause_covered": False, "pause_dpms": False})
            if not env.start_daemon(mons):
                print("restorecheck: the daemon did not start; log:")
                print(open(env.log, errors="replace").read()[-3000:])
                return 1
            try:
                fn(env, red, blue, broken)
            finally:
                env.stop_daemon()
    finally:
        env.close()
    print(f"restorecheck: {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
