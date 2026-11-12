#!/usr/bin/env python3
"""End-to-end check of "video_scene_convert": a video inside a scene that is larger than
the scene shows it is played from a converted copy.

  python3 -I scripts/scenevideocheck.py --build build [--daemon FILE] [--only a,b] [--keep] [--tmp DIR]

Uses the harness of scripts/lifecheck.py (private Xvfb, private daemon with its own XDG
dirs, pixel reading): nothing of the user's is read or written. The scenes are made here:
one picture layer whose texture is a WE .tex with an MP4 in it, as Wallpaper Engine packs
a video layer. The clips are made with the ffmpeg program (H.264; the whole check is
skipped where there is none): colour bars that stand still, so that the picture on the
monitor does not depend on the moment it is read.

  copy     a 1280x720 clip in a scene on a 640x360 monitor: the clip plays as it is, a
           640x360 copy is made in the cache (not in the wallpaper's folder), status
           "scene_videos" says "converting" and then "ready" with the played size, the
           copy has every picture of the clip, nothing unfinished is left, and the
           clip's decoder line in status is the copy's. The picture on the monitor with
           the copy against the picture with the original (the switch off): two
           different reductions of the same 1280x720 picture (the player's 2x2 average
           of the original, ffmpeg's bicubic scaling in the copy, encoded once more) -
           compared sample by sample, the mean difference and the share of samples that
           differ by more than 24 of 255 are bounded; a wrong colour matrix, a
           shifted or cropped picture or a wrong clip would be far outside.
  switch   the switch off: the original plays again and nothing is converted; on again:
           the copy that exists is used, no second conversion; video_resolution none:
           the original; saved in config.json.
  restart  a daemon that starts with the scene plays the copy from the first picture:
           the original's decoder is never opened.
  damaged  the copy in the cache is cut short (the machine lost power before the file
           had reached the disk): the daemon that starts with the scene removes it,
           plays the clip as it is, makes the copy again - once - and plays that.
  cancel   another wallpaper while the copy of a long clip is being made: the ffmpeg
           child (which runs at nice 19) ends, its partial file goes, status forgets
           the video; the same when the daemon quits.
  small    a scene whose clip is not larger than it is shown, and a scene without a
           video: nothing is converted, no file is written, status says "not needed" or
           has no "scene_videos" at all.
  monitors the scene on a 640x360 and on a 1280x720 monitor at once: both show the
           original (one decoder serves both, and the larger monitor needs every pixel);
           the larger monitor gets another wallpaper: the smaller one changes to the copy
           without being loaded again; the larger one comes back: the original again.

Exit code 0 = all passed, 1 = failures, 77 = skipped (no Xvfb, no ffmpeg that writes
H.264, or a build without the X11 backend or the video module). LIFECHECK_PATIENCE=N
multiplies every wait (a software renderer on a busy machine: 3 or more)."""
import glob, os, shutil, struct, subprocess, sys, time

sys.dont_write_bytecode = True   # no __pycache__ next to the scripts
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("RESTORECHECK_PATIENCE", os.environ.get("LIFECHECK_PATIENCE", "1") or "1")
import checkenv as rc   # noqa: E402
import displayenv       # noqa: E402
import lifecheck as lc  # noqa: E402  (its Env: extra environment, the log from a mark on)

PATIENCE = rc.PATIENCE
fails = 0
checks = 0
A = ("A", 640, 360, 0, 0)


def check(ok, what, detail=None):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok and detail is not None:
        print("       " + str(detail), flush=True)
    return ok


def gen_clip(path, size, seconds, rate=30):
    """colour bars that stand still, H.264 8-bit 4:2:0 in MP4; False when this machine cannot make it"""
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        p = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                            f"smptehdbars=size={size}:rate={rate}", "-t", str(seconds), "-c:v", "libx264",
                            "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "30", path],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=600 * PATIENCE)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return p.returncode == 0 and os.path.exists(path) and os.path.getsize(path) > 1000


def gen_video_scene(out, clip, w, h):
    """a scene of w x h with one picture layer over all of it whose texture is the clip:
    a TEXV container with the video flag (0x20) and the MP4 as its only level"""
    mp4 = open(clip, "rb").read()
    tex = (b"TEXV0005\0" + b"TEXI0001\0" + struct.pack("<IIIIIII", 0, 0x20, w, h, w, h, 0) +
           b"TEXB0003\0" + struct.pack("<II", 1, 0xFFFFFFFF) + struct.pack("<I", 1) +
           struct.pack("<IIIII", w, h, 0, len(mp4), len(mp4)) + mp4)
    os.makedirs(f"{out}/materials", exist_ok=True)
    with open(f"{out}/materials/v.tex", "wb") as f:
        f.write(tex)
    rc.jdump(f"{out}/materials/v.json",
             {"passes": [{"blending": "translucent", "cullmode": "nocull", "depthtest": "disabled",
                          "depthwrite": "disabled", "shader": "genericimage2", "textures": ["v"]}]})
    rc.jdump(f"{out}/models/v.json", {"autosize": True, "material": "materials/v.json"})
    rc.jdump(f"{out}/scene.json", {
        "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
        "general": {"ambientcolor": "0.3 0.3 0.3", "bloom": False, "cameraparallax": False,
                    "clearcolor": "0 0 0", "orthogonalprojection": {"height": h, "width": w},
                    "skylightcolor": "0.3 0.3 0.3"},
        "objects": [{"id": 7, "name": "video", "image": "models/v.json", "origin": f"{w // 2} {h // 2} 0",
                     "size": f"{w} {h}", "scale": "1 1 1", "angles": "0 0 0", "color": "1 1 1",
                     "alpha": 1.0, "visible": True}]})
    rc.jdump(f"{out}/project.json", {"file": "scene.json", "title": os.path.basename(out), "type": "scene"})


def scene_videos(env):
    return env.state().get("scene_videos") or ""


def cache_dir(env):
    return os.path.join(env.env["XDG_CACHE_HOME"], "walpe", "video")


def copies(env, suffix=".mp4"):
    return sorted(glob.glob(os.path.join(cache_dir(env), "*-scene-*" + suffix)))


def ffmpeg_children(pid, converting=False):
    """[(pid, nice)] of the ffmpeg processes whose parent is pid; converting: only one that
    reads a clip out of a file (not the short run that lists the encoders)"""
    out = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            st = open(f"/proc/{d}/stat").read()
            cmd = open(f"/proc/{d}/cmdline", "rb").read()
        except OSError:
            continue
        name = st[st.index("(") + 1:st.rindex(")")]
        f = st[st.rindex(")") + 2:].split()
        if name == "ffmpeg" and f[0] != "Z" and int(f[1]) == pid and (not converting or b"subfile," in cmd):
            out.append((int(d), int(f[16])))
    return out


def probe(path):
    """ "width,height,pix_fmt,pictures" of a video file"""
    try:
        p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries",
                            "stream=width,height,pix_fmt,nb_read_frames", "-of", "csv=p=0", path],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           timeout=120 * PATIENCE)
        return p.stdout.decode().strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def picture(env, mon=A):
    """the monitor's picture once it stands still: (w, h, RGB bytes), read twice the same"""
    last = None
    for _ in range(int(40 * PATIENCE)):
        f = env.display.frame(mon)
        if f and last and f[2] == last[2]:
            return f
        last = f
        time.sleep(0.25)
    return last


def compare(a, b):
    """two pictures of the same size -> (mean absolute difference per sample, share of
    samples that differ by more than 24, largest difference)"""
    if not a or not b or a[0] != b[0] or a[1] != b[1]:
        return (255.0, 1.0, 255)
    x, y = a[2], b[2]
    total = big = top = 0
    for i in range(len(x)):
        d = x[i] - y[i]
        if d < 0:
            d = -d
        total += d
        if d > 24:
            big += 1
        if d > top:
            top = d
    return (total / len(x), big / len(x), top)


BAR_X = (0.0625, 0.179, 0.286, 0.393, 0.5, 0.607, 0.714, 0.821, 0.9375)


def bars_ok(frame):
    """the picture is the colour bars: white, yellow and blue where the pattern has them (the
    upper part of smptehdbars: an eighth of grey at each side, between them seven bars of
    equal width: white, yellow, cyan, green, magenta, red, blue; BAR_X = their middles)"""
    if not frame:
        return False
    w, h, _ = frame

    def px(fx, fy):
        return displayenv.pixel_of(frame, int(w * fx), int(h * fy))
    yellow, blue, white = px(0.286, 0.3), px(0.821, 0.3), px(0.179, 0.3)
    return (yellow[0] > 150 and yellow[1] > 150 and yellow[2] < 80 and
            blue[2] > 150 and blue[0] < 80 and blue[1] < 80 and
            min(white) > 150 and max(white) - min(white) < 30)


def start(env, extra=None, cfg=None):
    conf = {"config_version": 4, "fps": 30, "pause_covered": False, "pause_dpms": False,
            "video_resolution": "auto"}
    conf.update(cfg or {})
    if cfg is not None or not os.path.exists(env.config):
        rc.jdump(env.config, conf)
    e = {"WALPE_CONVERT_BACKGROUND": "0"}   # the conversion at the daemon's priority: it has to end here
    e.update(extra or {})
    return env.start_daemon([A], extra=e)


# --------------------------------------------------------------------- copy
def case_copy(env, W):
    if not check(start(env, cfg={}), "copy: daemon started"):
        return
    mark = env.mark()
    env.open(W["vid"])
    check(lc.loaded(env, W["vid"]), "copy: the scene with the video is loaded", rc.brief(env.monitors()))
    first = lc.wait_for(lambda: scene_videos(env), 20, 0.1)
    check("v.tex: source 1280x720 30.00 fps, played 1280x720 30.00 fps, copy: " in first,
          "copy: status names the video, its size and frame rate, and that the clip itself plays", first)
    seen = lc.wait_for(lambda: "copy: converting" in scene_videos(env) or "copy: ready" in scene_videos(env) or
                       "copy: failed" in scene_videos(env), 60, 0.05)
    check(bool(seen), "copy: status says the copy is being made (or is ready already)", scene_videos(env))
    lc.wait_for(lambda: "played 640x360 30.00 fps, copy: ready" in scene_videos(env) or
                "failed" in scene_videos(env) or "not usable" in scene_videos(env), 300, 0.25)
    sv = scene_videos(env)
    if not check(sv == "v.tex: source 1280x720 30.00 fps, played 640x360 30.00 fps, copy: ready",
                 "copy: the copy is made and played (status: source 1280x720, played 640x360, copy: ready)", sv):
        print(env.since(mark)[-3000:])
        return
    log = env.since(mark)
    check(log.count("making a 640x360 copy once") == 1 and log.count("for its scene made") == 1,
          "copy: one conversion", [ln for ln in log.splitlines() if "video:" in ln][-8:])
    check("is played from" in log and "now: 640x360 30.00 fps" in log,
          "copy: the log says when the copy took over")
    cs = copies(env)
    check(len(cs) == 1 and cs[0].endswith("-scene-640x360.mp4") and not copies(env, ".part"),
          "copy: one copy in the cache, named by its size, nothing unfinished", os.listdir(cache_dir(env)))
    check(probe(cs[0]) == "640,360,yuv420p,60" if cs else False,
          "copy: 640x360, 8-bit 4:2:0, every one of the clip's 60 pictures", probe(cs[0]) if cs else None)
    check((os.stat(cache_dir(env)).st_mode & 0o077) == 0, "copy: the cache folder is private")
    extra = [p for p in glob.glob(os.path.join(W["vid"], "**"), recursive=True) if p.endswith((".mp4", ".part"))]
    check(not extra, "copy: nothing was written into the wallpaper's folder", extra)
    check(not ffmpeg_children(env.daemon.pid), "copy: no ffmpeg is left running")
    check("640x360 30.00 fps: software" in (env.state().get("video_decoder") or ""),
          "copy: \"video_decoder\" is the copy's decoder", env.state().get("video_decoder"))
    with_copy = picture(env)
    check(bars_ok(with_copy), "copy: the monitor shows the clip's picture (played from the copy)",
          [displayenv.pixel_of(with_copy, int(640 * f), 108) for f in BAR_X] if with_copy else None)

    # the same scene from the original: the switch off (the clip is 2 s long)
    env.ctl("set", "video_scene_convert=off")
    lc.wait_for(lambda: "played 1280x720 30.00 fps, copy: off" in scene_videos(env), 30, 0.2)
    check(scene_videos(env) == "v.tex: source 1280x720 30.00 fps, played 1280x720 30.00 fps, copy: off",
          "copy: the switch off: the original plays again (status: played 1280x720, copy: off)", scene_videos(env))
    time.sleep(0.5)
    original = picture(env)
    check(bars_ok(original), "copy: the monitor shows the clip's picture (played from the original)")
    mean, big, top = compare(with_copy, original)
    print("      picture with the copy against the picture with the original: mean difference %.3f of 255 per "
          "sample, %.2f %% of the samples differ by more than 24, largest difference %d" % (mean, big * 100, top),
          flush=True)
    check(mean <= 2.0 and big <= 0.03, "copy: the picture with the copy is the picture with the original within "
          "the scaling difference (mean <= 2.0 of 255, at most 3 % of the samples off by more than 24)",
          (mean, big, top))
    # away from the edges of the bars nothing but the second encoding differs: the colours
    # are the same (a wrong matrix or range would move every bar by 10 and more)
    flat = 0
    for fx in BAR_X:
        for fy in (0.2, 0.45):
            p, q = (displayenv.pixel_of(f, int(640 * fx), int(360 * fy)) for f in (with_copy, original))
            flat = max(flat, max(abs(p[i] - q[i]) for i in range(3)))
    print("      in the middle of the bars the two pictures differ by at most %d of 255" % flat, flush=True)
    check(flat <= 4, "copy: the colours of the bars are the same with the copy and with the original (within 4 "
          "of 255)", flat)
    env.stop_daemon()


# ------------------------------------------------------------------- switch
def case_switch(env, W):
    if not copies(env):
        print("skip  switch: the copy of case `copy` is not there", flush=True)
        return
    if not check(start(env, cfg={"video_scene_convert": False}), "switch: daemon started with the switch off"):
        return
    mark = env.mark()
    env.open(W["vid"])
    lc.loaded(env, W["vid"])
    lc.wait_for(lambda: "copy: off" in scene_videos(env), 30, 0.2)
    check(scene_videos(env) == "v.tex: source 1280x720 30.00 fps, played 1280x720 30.00 fps, copy: off" and
          env.state().get("video_scene_convert") is False,
          "switch: off in config.json: the original plays although a copy exists", scene_videos(env))
    env.ctl("set", "video_scene_convert=on")
    lc.wait_for(lambda: "played 640x360 30.00 fps, copy: ready" in scene_videos(env), 30, 0.2)
    check(scene_videos(env).endswith("played 640x360 30.00 fps, copy: ready") and
          env.state().get("video_scene_convert") is True,
          "switch: on again: the copy that exists is played", scene_videos(env))
    check("making a" not in env.since(mark) and len(copies(env)) == 1, "switch: nothing was converted again")
    try:
        import json
        saved = json.load(open(env.config)).get("video_scene_convert")
    except Exception:
        saved = None
    check(saved is True, "switch: the setting is saved in config.json", saved)
    # video_resolution none = videos keep their own size: the original
    env.ctl("set", "video_resolution=none")
    lc.wait_for(lambda: "played 1280x720" in scene_videos(env), 30, 0.2)
    check("played 1280x720 30.00 fps, copy: not needed (video_resolution is none)" in scene_videos(env),
          "switch: video_resolution none: the original plays", scene_videos(env))
    env.ctl("set", "video_resolution=auto")
    lc.wait_for(lambda: "played 640x360" in scene_videos(env), 30, 0.2)
    check(scene_videos(env).endswith("played 640x360 30.00 fps, copy: ready"),
          "switch: video_resolution auto again: the copy", scene_videos(env))
    # a box the clip fits into: no copy
    env.ctl("set", "video_resolution=1080p")
    lc.wait_for(lambda: "played 1280x720" in scene_videos(env), 30, 0.2)
    check("played 1280x720 30.00 fps, copy: not needed" in scene_videos(env) and
          "none" not in scene_videos(env), "switch: video_resolution 1080p holds a 1280x720 clip: the original",
          scene_videos(env))
    check("making a" not in env.since(mark) and len(copies(env)) == 1 and not copies(env, ".part"),
          "switch: no conversion in all of this")
    refused = env.ipc({"cmd": "set", "video_scene_convert": "maybe"})
    check(bool(refused) and not refused.get("ok"), "switch: a value that is no switch is refused", refused)
    env.stop_daemon()


# ------------------------------------------------------------------ restart
def case_restart(env, W):
    if not copies(env):
        print("skip  restart: the copy of case `copy` is not there", flush=True)
        return
    rc.jdump(env.config, {"config_version": 4, "fps": 30, "pause_covered": False, "pause_dpms": False,
                          "video_resolution": "auto", "current": W["vid"]})
    mark = env.mark()
    if not check(start(env), "restart: daemon started with the scene as its saved wallpaper"):
        return
    lc.loaded(env, W["vid"])
    lc.wait_for(lambda: "copy: ready" in scene_videos(env), 30, 0.2)
    log = env.since(mark)
    check(scene_videos(env) == "v.tex: source 1280x720 30.00 fps, played 640x360 30.00 fps, copy: ready",
          "restart: the copy plays", scene_videos(env))
    check("is played from its converted copy" in log and "video: 640x360 30.00 fps" in log and
          "video: 1280x720" not in log and "making a" not in log,
          "restart: from the first picture - the original's decoder was never opened, nothing converted",
          [ln for ln in log.splitlines() if "video:" in ln][:6])
    check(bars_ok(picture(env)), "restart: the monitor shows the clip's picture")
    env.stop_daemon()


# ------------------------------------------------------------------ damaged
def case_damaged(env, W):
    cs = copies(env)
    if not cs:
        print("skip  damaged: the copy of case `copy` is not there", flush=True)
        return
    whole = os.path.getsize(cs[0])
    with open(cs[0], "r+b") as f:
        f.truncate(whole // 2)
    rc.jdump(env.config, {"config_version": 4, "fps": 30, "pause_covered": False, "pause_dpms": False,
                          "video_resolution": "auto", "current": W["vid"]})
    mark = env.mark()
    if not check(start(env), "damaged: daemon started with the scene, its copy cut to half its length"):
        return
    lc.loaded(env, W["vid"])
    lc.wait_for(lambda: "played 640x360 30.00 fps, copy: ready" in scene_videos(env) or
                "failed" in scene_videos(env) or "not usable" in scene_videos(env), 300, 0.25)
    sv = scene_videos(env)
    log = env.since(mark)
    check(sv == "v.tex: source 1280x720 30.00 fps, played 640x360 30.00 fps, copy: ready",
          "damaged: the copy is made again and played (not \"not usable\" for ever)", sv)
    check("cannot be read (cut short or damaged): removed" in log, "damaged: the log says that the file was removed",
          [ln for ln in log.splitlines() if "scenenode:" in ln][:4])
    check(log.count("making a 640x360 copy once") == 1 and log.count("for its scene made") == 1,
          "damaged: one conversion", [ln for ln in log.splitlines() if "video:" in ln][-8:])
    cs2 = copies(env)
    check(cs2 == cs and probe(cs[0]) == "640,360,yuv420p,60" and not copies(env, ".part"),
          "damaged: the copy in the cache is whole again: 640x360, every one of the clip's 60 pictures",
          (cs2, probe(cs[0]) if cs2 else None))
    check(bars_ok(picture(env)), "damaged: the monitor shows the clip's picture")
    env.stop_daemon()
    # the daemon that starts next plays the copy from the first picture, like any other
    mark = env.mark()
    if check(start(env), "damaged: daemon started again"):
        lc.loaded(env, W["vid"])
        lc.wait_for(lambda: "copy: ready" in scene_videos(env), 30, 0.2)
        log = env.since(mark)
        check("is played from its converted copy" in log and "making a" not in log and "removed" not in log,
              "damaged: the next start plays the copy, nothing is removed or converted",
              [ln for ln in log.splitlines() if "video:" in ln][:6])
        env.stop_daemon()


# ------------------------------------------------------------------- cancel
def case_cancel(env, W):
    # at the lowest priority, as it runs for a user (nice 19 is checked here)
    if not check(start(env, extra={"WALPE_CONVERT_BACKGROUND": "1"}, cfg={}), "cancel: daemon started"):
        return
    env.open(W["red"])
    lc.loaded(env, W["red"])
    before = set(os.listdir(cache_dir(env))) if os.path.isdir(cache_dir(env)) else set()
    mark = env.mark()
    env.open(W["long"])
    lc.loaded(env, W["long"])
    kids = lc.wait_for(lambda: ffmpeg_children(env.daemon.pid, True), 60, 0.02)
    if not kids and "for its scene made" in env.since(mark):
        print("skip  cancel: the conversion of the long clip ended before it could be looked at", flush=True)
        env.stop_daemon()
        return
    check(len(kids or []) == 1, "cancel: one ffmpeg child converts the long clip", kids)
    check(bool(kids) and kids[0][1] == 19, "cancel: it runs at nice 19", kids)
    lc.wait_for(lambda: copies(env, ".part") or "copy: converting" in scene_videos(env), 60, 0.05)
    check("copy: converting" in scene_videos(env) and "played 1280x720" in scene_videos(env),
          "cancel: status says \"converting\" while the original plays", scene_videos(env))
    # the wallpaper goes
    env.open(W["red"])
    lc.loaded(env, W["red"])
    gone = lc.wait_for(lambda: not ffmpeg_children(env.daemon.pid) and not copies(env, ".part"), 20, 0.1)
    if "for its scene made" in env.since(mark):
        print("skip  cancel: the conversion ended before the wallpaper was replaced", flush=True)
    else:
        check(bool(gone), "cancel: the wallpaper is replaced: the ffmpeg child ends, its partial file goes",
              (ffmpeg_children(env.daemon.pid), os.listdir(cache_dir(env))))
        check("its conversion is stopped" in env.since(mark), "cancel: the log says so")
        after = set(os.listdir(cache_dir(env))) if os.path.isdir(cache_dir(env)) else set()
        check(after == before, "cancel: the cache holds what it held before", sorted(after - before))
    check(not scene_videos(env), "cancel: status has no \"scene_videos\" without such a scene", scene_videos(env))

    # the daemon ends while a copy is being made
    mark = env.mark()
    env.open(W["long"])
    lc.loaded(env, W["long"])
    kids = lc.wait_for(lambda: ffmpeg_children(env.daemon.pid, True), 60, 0.02)
    pid = env.daemon.pid
    if kids:
        env.stop_daemon()
        left = lc.wait_for(lambda: not [k for k in kids if os.path.exists(f"/proc/{k[0]}") and
                                        open(f"/proc/{k[0]}/stat").read().split(")")[-1].split()[0] != "Z"], 10, 0.1)
        check(bool(left), "cancel: the daemon quits: its ffmpeg child is gone", kids)
        check(not copies(env, ".part") and not ffmpeg_children(pid), "cancel: ... and no partial file is left",
              os.listdir(cache_dir(env)) if os.path.isdir(cache_dir(env)) else None)
    else:
        print("skip  cancel (quit): no conversion was running", flush=True)
        env.stop_daemon()


# -------------------------------------------------------------------- small
def case_small(env, W):
    if not check(start(env, cfg={}), "small: daemon started"):
        return
    before = sorted(os.listdir(cache_dir(env))) if os.path.isdir(cache_dir(env)) else []
    mark = env.mark()
    env.open(W["fit"])
    lc.loaded(env, W["fit"])
    lc.wait_for(lambda: "copy: not needed" in scene_videos(env), 30, 0.2)
    check(scene_videos(env) == "v.tex: source 640x360 30.00 fps, played 640x360 30.00 fps, copy: not needed",
          "small: a clip that is as large as it is shown: \"not needed\"", scene_videos(env))
    time.sleep(1.0)
    env.open(W["red"])
    lc.loaded(env, W["red"])
    lc.wait_for(lambda: not scene_videos(env), 10, 0.2)
    check("scene_videos" not in env.state(), "small: a scene without a video: no \"scene_videos\" in status",
          env.state().get("scene_videos"))
    log = env.since(mark)
    after = sorted(os.listdir(cache_dir(env))) if os.path.isdir(cache_dir(env)) else []
    check("making a" not in log and after == before and not ffmpeg_children(env.daemon.pid),
          "small: nothing was converted, no file written, no child started", after)
    env.stop_daemon()


# ----------------------------------------------------------------- monitors
def case_monitors(env, W):
    if not copies(env):
        print("skip  monitors: the copy of case `copy` is not there", flush=True)
        return
    B = ("B", 1280, 720, 640, 0)
    rc.jdump(env.config, {"config_version": 4, "fps": 30, "pause_covered": False, "pause_dpms": False,
                          "video_resolution": "auto", "render_shared": True})
    if not check(env.start_daemon([A, B], extra={"WALPE_CONVERT_BACKGROUND": "0"}),
                 "monitors: daemon started with a 640x360 and a 1280x720 monitor"):
        return
    mark = env.mark()
    env.open(W["vid"])
    lc.loaded(env, W["vid"])
    lc.wait_for(lambda: "copy: not needed" in scene_videos(env), 30, 0.2)
    check(scene_videos(env) == "v.tex: source 1280x720 30.00 fps, played 1280x720 30.00 fps, copy: not needed",
          "monitors: on both monitors the clip itself plays (the larger one shows every pixel)", scene_videos(env))
    loads = env.since(mark).count("scenenode: video texture")
    # the larger monitor shows something else: the box of the scene's video is the smaller monitor's
    env.open(W["red"], 1)
    lc.loaded(env, W["red"], 1)
    lc.wait_for(lambda: "played 640x360" in scene_videos(env), 30, 0.2)
    check(scene_videos(env) == "v.tex: source 1280x720 30.00 fps, played 640x360 30.00 fps, copy: ready",
          "monitors: the larger monitor gets another wallpaper: the smaller one plays the copy", scene_videos(env))
    check(env.since(mark).count("scenenode: video texture") == loads,
          "monitors: ... without loading its scene again")
    check(bars_ok(picture(env, env.mons[0])), "monitors: the smaller monitor shows the clip's picture")
    env.open(W["vid"], 1)
    lc.loaded(env, W["vid"], 1)
    lc.wait_for(lambda: "played 1280x720" in scene_videos(env) and "starts again" not in scene_videos(env), 30, 0.2)
    check(scene_videos(env) == "v.tex: source 1280x720 30.00 fps, played 1280x720 30.00 fps, copy: not needed",
          "monitors: the larger monitor shows the scene again: the clip itself on both", scene_videos(env))
    check(bars_ok(picture(env, env.mons[0])) and bars_ok(picture(env, env.mons[1])),
          "monitors: both monitors show the clip's picture")
    check("making a" not in env.since(mark) and len(copies(env)) == 1 and not copies(env, ".part"),
          "monitors: nothing was converted in all of this")
    env.stop_daemon()


# ---------------------------------------------------------------- webunload
def case_webunload(env, W):
    """the restart in place that unloads the web module (web_unload) waits while a copy is being made"""
    B = ("B", 640, 360, 640, 0)
    said = "restarting in place to unload the web module"
    web = os.path.join(env.tmp, "wallpapers", "page")
    lc.gen_web(web)
    rc.jdump(env.config, {"config_version": 4, "fps": 30, "pause_covered": False, "pause_dpms": False,
                          "video_resolution": "auto", "current": W["red"]})
    # (at the lowest priority: the conversion has to last)
    if not check(env.start_daemon([A, B], extra={"WALPE_CONVERT_BACKGROUND": "1"}), "webunload: daemon started"):
        return
    st = env.state()
    if "web_accel" not in st or "web_unload" not in st:
        print("skip  webunload (no web module in this build)", flush=True)
        return

    def modules():
        return env.state().get("modules", "")

    def page_on_b():
        env.open(web, 1)
        return lc.loaded(env, web, 1, 120) and "web=loaded" in modules()

    lc.loaded(env, W["red"])
    if not page_on_b():
        print("skip  webunload (no web wallpaper came up here)", flush=True)
        return
    pid = env.daemon.pid
    mark = env.mark()
    env.open(W["long"], 0)
    lc.loaded(env, W["long"], 0)
    kids = lc.wait_for(lambda: ffmpeg_children(pid, True), 60, 0.02)
    # the last web wallpaper leaves while the copy is being made
    env.open(W["red"], 1)
    lc.loaded(env, W["red"], 1)
    time.sleep(1.5)
    if not kids or "for its scene made" in env.since(mark):
        print("skip  webunload: the conversion of the long clip had ended", flush=True)
    else:
        check(said not in env.since(mark) and "web=loaded" in modules() and env.daemon.pid == pid,
              "webunload: the last web wallpaper leaves while a copy is being made: no restart",
              env.since(mark)[-300:])
        check([k[0] for k in ffmpeg_children(pid, True)] == [k[0] for k in kids],
              "webunload: ... and the same ffmpeg goes on converting", (kids, ffmpeg_children(pid, True)))
    # no copy is being made: the same change restarts the daemon
    env.open(W["red"], 0)
    lc.loaded(env, W["red"], 0)
    lc.wait_for(lambda: not ffmpeg_children(pid), 20, 0.1)
    if not page_on_b():
        print("skip  webunload: the web wallpaper did not come up again", flush=True)
        return
    mark = env.mark()
    env.open(W["red"], 1)
    ok = lc.wait_for(lambda: said in env.since(mark) and "web=not loaded" in modules(), 90, 0.3)
    check(bool(ok) and env.daemon.poll() is None and env.daemon.pid == pid,
          "webunload: with no copy being made the daemon restarts in place and the web module is gone",
          (env.since(mark)[-300:], modules()))
    env.stop_daemon()


CASES = [("copy", case_copy), ("switch", case_switch), ("restart", case_restart), ("damaged", case_damaged),
         ("cancel", case_cancel),
         ("small", case_small), ("monitors", case_monitors), ("webunload", case_webunload)]


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
    skip = displayenv.unusable("xvfb", build)
    if skip:
        print("scenevideocheck: skipped (%s)" % skip[0])
        return skip[1]
    for b in (daemon or os.path.join(build, "walpe-daemon"), os.path.join(build, "walpe-ctl")):
        if not os.path.exists(b):
            print(f"scenevideocheck: {b} not found")
            return 2
    if not os.path.exists(os.path.join(build, "walpe-video.so")):
        print("scenevideocheck: skipped (this build has no video module)")
        return displayenv.SKIP
    env = lc.Env(build, daemon, keep, tmp, "xvfb")
    try:
        if len(env.sock.encode()) > 107:
            print(f"scenevideocheck: {env.sock} is too long for a socket: pass --tmp <a short directory>")
            return 2
        wp = os.path.join(env.tmp, "wallpapers")
        W = {k: os.path.join(wp, k) for k in ("red", "vid", "long", "fit")}
        clips = os.path.join(env.tmp, "clips")
        if not (gen_clip(f"{clips}/big.mp4", "1280x720", 2) and gen_clip(f"{clips}/long.mp4", "1280x720", 90) and
                gen_clip(f"{clips}/fit.mp4", "640x360", 2)):
            print("scenevideocheck: skipped (no ffmpeg program here that writes an H.264 clip)")
            return displayenv.SKIP
        rc.gen_scene(W["red"], (1, 0, 0))
        gen_video_scene(W["vid"], f"{clips}/big.mp4", 1280, 720)
        gen_video_scene(W["long"], f"{clips}/long.mp4", 1280, 720)
        gen_video_scene(W["fit"], f"{clips}/fit.mp4", 640, 360)
        if not env.start_x(1920, 720):
            print("scenevideocheck: skipped (Xvfb did not start)")
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
    print(f"scenevideocheck: {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
