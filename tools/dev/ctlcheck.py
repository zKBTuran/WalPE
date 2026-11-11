#!/usr/bin/env python3
"""End-to-end check of walpe-ctl against a running engine (dev harness, like layercheck.py).

  python3 -I tools/dev/ctlcheck.py --build build [--keep] [--tmp DIR]

Starts a private Xvfb (-displayfd) and a private walpe-daemon (own XDG dirs in a temp dir, two fake
640x360 monitors), generates a GIF and a picture with ffmpeg (loose media files, opened as wallpapers
by their path), and runs the commands README.md documents:

  - status: one JSON object with the documented fields, two monitors
  - every `set` key of the usage text: answered ok, the value is in config.json afterwards (and in
    status where status has it), several keys in one call, the short names
  - what the engine refuses: an unknown key (nothing else of that call is applied), a value of the
    wrong kind, a number out of the stated set - exit status 3 and a reason
  - open / open --monitor N / a path that does not exist / a monitor that does not exist
  - props, setprop (one and several), resetprops, with --monitor and --path
  - pause / resume, quality, quality --path, cancel-import with nothing running, steam-libraries
  - status names every saved setting `set` takes; a change that cannot be saved (config.json's directory
    is not there to write into) is answered ok with the reason, shows in status, and is gone again after
    the next save that works
  - config.json keeps a member it does not know through all of that
  - quit: the engine ends; afterwards every command exits with 2

Exit code 0 = all passed, 1 = failures, 77 = skipped (no Xvfb / ffmpeg). CTLCHECK_PATIENCE=N multiplies
every wait. --tmp DIR: work in DIR (must not exist; removed afterwards); the engine's socket lives
below it and a socket path is limited to 107 bytes (a longer one is refused: the engine would fall back
to a socket shared with other sessions)."""
import json, os, shutil, subprocess, sys, tempfile, time

PATIENCE = max(1.0, float(os.environ.get("CTLCHECK_PATIENCE", "1") or 1))
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
            os.makedirs(tmp)
            self.tmp = os.path.abspath(tmp)
        else:
            self.tmp = tempfile.mkdtemp(prefix="walpe-ctlcheck-")
        self.env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8", "TZ": "UTC"}
        for k, d in (("XDG_RUNTIME_DIR", "rt"), ("XDG_CONFIG_HOME", "cfg"), ("XDG_CACHE_HOME", "cache"),
                     ("XDG_DATA_HOME", "data"), ("HOME", "home")):
            p = os.path.join(self.tmp, d)
            os.makedirs(p, mode=0o700)
            self.env[k] = p
        # (the daemon is told its window system by name: left to choose, it would take a
        # Wayland session's compositor when it finds one, not the X server started here)
        self.env.update({"LIBGL_ALWAYS_SOFTWARE": "1", "NO_AT_BRIDGE": "1", "WALPE_MODULE_DIR": self.build,
                         "WALPE_MONITORS": "A=640x360+0+0@60,B=640x360+640+0@60", "WALPE_BACKEND": "x11"})
        self.config = os.path.join(self.env["XDG_CONFIG_HOME"], "walpe", "config.json")
        self.xvfb = None
        self.daemon = None

    def start_x(self):
        r, w = os.pipe()
        p = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", "1280x360x24", "-nolisten", "tcp",
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
        self.xvfb = p
        self.env["DISPLAY"] = ":" + disp.decode().strip()
        return True

    def ctl(self, *args):
        """(exit status, stdout, stderr)"""
        try:
            r = subprocess.run([os.path.join(self.build, "walpe-ctl")] + list(args), env=self.env,
                               capture_output=True, text=True, timeout=60 * PATIENCE)
            return r.returncode, r.stdout, r.stderr
        except subprocess.TimeoutExpired:
            return 124, "", ""

    def reply(self, *args):
        rc, out, _ = self.ctl(*args)
        try:
            return rc, json.loads(out)
        except ValueError:
            return rc, None

    def status(self):
        rc, r = self.reply("status")
        return r.get("state") if rc == 0 and isinstance(r, dict) else None

    def cfg(self):
        try:
            return json.load(open(self.config))
        except (OSError, ValueError):
            return {}

    def start_daemon(self):
        log = open(os.path.join(self.tmp, "daemon.log"), "a")
        self.daemon = subprocess.Popen([os.path.join(self.build, "walpe-daemon")], env=self.env, stdout=log,
                                       stderr=subprocess.STDOUT, cwd=self.tmp)
        for _ in range(int(150 * PATIENCE)):
            time.sleep(0.2)
            if self.ctl("status")[0] == 0:
                return True
        return False

    def close(self):
        if self.daemon and self.daemon.poll() is None:
            self.ctl("quit")
            try:
                self.daemon.wait(timeout=15 * PATIENCE)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
        if self.xvfb:
            self.xvfb.terminate()
        if self.keep:
            print("kept", self.tmp)
        else:
            shutil.rmtree(self.tmp, ignore_errors=True)


def main():
    build, keep, tmp = "build", False, None
    av = sys.argv[1:]
    while av:
        a = av.pop(0)
        if a == "--build" and av:
            build = av.pop(0)
        elif a == "--keep":
            keep = True
        elif a == "--tmp" and av:
            tmp = av.pop(0)
        else:
            print(__doc__)
            return 2
    for tool in ("Xvfb", "ffmpeg"):
        if not shutil.which(tool):
            print("skipped: no " + tool)
            return 77
    env = Env(build, keep, tmp)
    try:
        if len(os.path.join(env.env["XDG_RUNTIME_DIR"], "walpe.sock")) > 100:
            print("skipped: %s is too long for a socket path (use --tmp with a short directory)" % env.tmp)
            return 77
        if not env.start_x():
            print("skipped: no private X server")
            return 77
        # two wallpapers of kinds that need no Wallpaper Engine assets
        lib = os.path.join(env.tmp, "lib")
        os.makedirs(lib)
        gif = os.path.join(lib, "a.gif")
        pic = os.path.join(lib, "b.png")
        mk = lambda *a: subprocess.run(["ffmpeg", "-v", "error", "-y"] + list(a), env=env.env).returncode == 0
        if not mk("-f", "lavfi", "-i", "testsrc2=size=320x180:rate=5", "-t", "1", gif) or \
           not mk("-f", "lavfi", "-i", "testsrc2=size=320x180:rate=1", "-frames:v", "1", pic):
            print("skipped: ffmpeg cannot make the test pictures")
            return 77
        os.makedirs(os.path.dirname(env.config), exist_ok=True)
        json.dump({"config_version": 3, "wallpapers_dir": lib, "pause_covered": False, "pause_fullscreen": False,
                   "pause_dpms": False, "kept_by_hand": {"a": [1, 2]}}, open(env.config, "w"))
        if not check(env.start_daemon(), "the engine starts"):
            return 1

        # ---- status
        rc, out, _ = env.ctl("status")
        st = None
        try:
            st = json.loads(out)["state"]
        except (ValueError, KeyError, TypeError):
            pass
        check(rc == 0 and isinstance(st, dict), "status is one JSON object with a state")
        st = st or {}
        want = ["path", "type", "paused", "monitors", "monitor_count", "fps", "volume", "render_scale",
                "quality_preset", "software_render", "software_active", "gl_renderer", "fx_cache",
                "fx_specialize", "fx_quality", "video_hwdec", "particle_lowres", "particle_density",
                "particle_instancing", "focus_action", "we_assets", "we_assets_source", "audio", "media",
                "import_active", "import_percent", "config_serial", "modules",
                # the saved settings under their config.json names
                "pause_fullscreen", "pause_dpms", "pause_covered", "cover_transparent", "transparent_windows",
                "focus_fps", "pause_battery", "battery_fps", "preview_anim", "render_shared", "parallax",
                "tex_downscale", "skip_hidden", "skip_hidden_margin", "skip_hidden_inset", "software_threads_saved"]
        missing = [k for k in want if k not in st]
        check(not missing, "status has the documented fields (missing: %s)" % missing)
        mons = st.get("monitors") or []
        check(len(mons) == 2 and st.get("monitor_count") == 2 and
              all(set(("index", "name", "geometry", "current", "paused", "type")) <= set(m) for m in mons),
              "status lists two monitors with index, name, geometry, current, paused, type")

        # ---- set: every key of the usage text, the value in config.json afterwards
        table = [
            ("preset=low", "quality_preset", "low"),    # first: a preset sets several of the keys below
            ("fps=48", "fps", 48), ("volume=0.25", "volume", 0.25), ("parallax=off", "parallax", False),
            ("fs=on", "pause_fullscreen", True), ("covered=on", "pause_covered", True),
            ("dpms=on", "pause_dpms", True), ("transparent=off", "cover_transparent", False),
            ("transparent_windows=st,foot", "transparent_windows", "st,foot"),
            ("focus_action=limit", "focus_action", "limit"), ("focus=pause", "focus_action", "pause"),
            ("focus_fps=20", "focus_fps", 20), ("battery=on", "pause_battery", True),
            ("battery_fps=24", "battery_fps", 24), ("previews=hover", "preview_anim", "hover"),
            ("previews=off", "preview_anim", "off"), ("shared=off", "render_shared", False),
            ("fx_cache=off", "fx_cache", False), ("tex_downscale=off", "tex_downscale", False),
            ("fx_specialize=on", "fx_specialize", True), ("fx_quality=fast", "fx_quality", "fast"),
            ("texture_slack=0", "texture_slack", 0), ("render_scale=50", "render_scale", 50),
            ("hwdec=on", "video_hwdec", True), ("particle_lowres=all", "particle_lowres", "all"),
            ("particle_density=60", "particle_density", 60), ("particle_instancing=off", "particle_instancing", False),
            ("fx_rate=2", "fx_rate", 2),
            ("fx_rate_mode=all", "fx_rate_mode", "all"), ("fx_res=half", "fx_res", "half"),
            ("fps_idle=30", "fps_idle", 30), ("post=off", "post_processing", "off"),
            ("tex_res=half", "tex_res", "half"), ("steam_user=some_account", "steam_user", "some_account"),
            ("software_threads=3", "software_threads", 3), ("software_threads=auto", "software_threads", 0),
            ("we_assets=", "we_assets", None),
        ]
        bad = []
        for arg, key, val in table:
            rc, r = env.reply("set", arg)
            got = env.cfg().get(key)
            if rc != 0 or not (isinstance(r, dict) and r.get("ok")) or got != val:
                bad.append("%s -> exit %d, config %s=%r" % (arg, rc, key, got))
        check(not bad, "every `set` key is taken and saved (%d keys; wrong: %s)" % (len(table), bad))
        st = env.status() or {}
        check(st.get("fps") == 48 and st.get("video_hwdec") is True and st.get("particle_density") == 60 and
              st.get("fx_quality") == "fast" and st.get("focus_action") == "pause" and
              st.get("render_scale") == 50, "status shows the values that were set")
        shown = {"parallax": False, "pause_fullscreen": True, "pause_covered": True, "pause_dpms": True,
                 "cover_transparent": False, "transparent_windows": "st,foot", "focus_fps": 20, "pause_battery": True,
                 "battery_fps": 24, "preview_anim": "off", "render_shared": False, "tex_downscale": False,
                 "software_threads_saved": 0}
        wrong = {k: st.get(k, "(missing)") for k, v in shown.items() if st.get(k, "(missing)") != v}
        check(not wrong, "status shows every saved setting that was set, under its config.json name (wrong: %s)" % wrong)
        check("video_decoder" not in st or "software" in str(st.get("video_decoder")),
              "hardware decoding on with nothing to decode on: no decoder claimed in hardware")
        rc, r = env.reply("set", "fps=60", "volume=0", "covered=off", "fs=off", "dpms=off", "preset=high",
                          "hwdec=off", "shared=on", "render_scale=100")
        c = env.cfg()
        check(rc == 0 and c.get("fps") == 60 and c.get("pause_covered") is False and
              c.get("quality_preset") == "high" and c.get("render_shared") is True and c.get("render_scale") == 100,
              "several keys in one call are all applied")

        # ---- set: what is refused
        before = env.cfg()
        for args, text in ((["coverd=off", "fps=30"], "unknown setting"), (["fps=on"], "number"),
                           (["covered=maybe"], "switch"), (["focus=sideways"], "run, limit or pause"),
                           (["particle_density=3"], "5 to 100"), (["fx_quality=best"], "exact or fast"),
                           (["previews=sometimes"], "always, hover or off"),
                           (["steam_user=no spaces allowed"], "steam_user"),
                           (["skip_hidden=alwys"], "off, auto or always"),
                           (["skip_hidden_margin=-5"], "from 0 on"),
                           (["video_backend=vlc", "fps=30"], "video_backend")):
            rc, out, err = env.ctl("set", *args)
            check(rc == 3 and text in out, "set %s is refused with a reason (exit %d: %s)" %
                  (" ".join(args), rc, out.strip()[:90]))
        check(env.cfg() == before, "nothing of the refused calls was applied")
        check(env.cfg().get("kept_by_hand") == {"a": [1, 2]}, "config.json still holds the member it does not know")

        # ---- a change that cannot be saved: config.json's directory is not there to write into
        cdir = os.path.dirname(env.config)
        os.rename(cdir, cdir + ".aside")
        open(cdir, "w").close()
        rc, out, err = env.ctl("set", "fps=31")
        try:
            r = json.loads(out)
        except ValueError:
            r = {}
        st = env.status() or {}
        os.unlink(cdir)
        os.rename(cdir + ".aside", cdir)
        check(rc == 0 and r.get("ok") is True and r.get("save_error") and "could not save" in err,
              "a change that could not be saved is answered ok with the reason (save_error) and a sentence on stderr")
        check(st.get("fps") == 31 and st.get("config_save_error") and env.cfg().get("fps") == 60,
              "it is in effect, status shows config_save_error, config.json is as it was")
        rc, r = env.reply("set", "fps=60")
        st = env.status() or {}
        check(rc == 0 and "save_error" not in (r or {}) and "config_save_error" not in st and env.cfg().get("fps") == 60,
              "the next save works and the reason is gone")

        # ---- open
        rc, r = env.reply("open", gif)
        time.sleep(2 * PATIENCE)
        st = env.status() or {}
        check(rc == 0 and st.get("path") == gif and all(m.get("current") == gif for m in st.get("monitors", [])),
              "open <path> sets every monitor")
        rc, r = env.reply("open", pic, "--monitor", "1")
        time.sleep(2 * PATIENCE)
        st = env.status() or {}
        mons = st.get("monitors") or [{}, {}]
        check(rc == 0 and mons[0].get("current") == gif and mons[1].get("current") == pic,
              "open <path> --monitor 1 sets only that monitor")
        for _ in range(int(40 * PATIENCE)):
            st = env.status() or {}
            mons = st.get("monitors") or [{}, {}]
            if mons[0].get("type") == "gif" and mons[1].get("type") == "image":
                break
            time.sleep(0.5)
        check(mons[0].get("type") == "gif" and mons[1].get("type") == "image" and
              not any("load_error" in m for m in mons), "both wallpapers are shown (gif, image), no load error")
        rc, out, _ = env.ctl("open", os.path.join(lib, "nothing-here"))
        check(rc == 3 and "unsupported content" in out, "open of a path that does not exist is refused")
        rc, out, _ = env.ctl("open", gif, "--monitor", "7")
        check(rc == 3 and "no such monitor" in out, "open on a monitor that does not exist is refused")

        # ---- props / setprop / resetprops
        rc, r = env.reply("props")
        check(rc == 0 and "walpe_fit" in (r or {}).get("props", {}), "props lists the built-in options of the default wallpaper")
        rc, r = env.reply("setprop", "walpe_fit=contain")
        rc_b, r2 = env.reply("props")
        check(rc == 0 and (r2 or {}).get("props", {}).get("walpe_fit", {}).get("value") == "contain",
              "setprop walpe_fit=contain shows in props")
        rc, r = env.reply("setprop", "--monitor", "1", "walpe_fit=stretch", "walpe_pos_x=10", "walpe_flip=on")
        rc_b, r2 = env.reply("props", "--monitor", "1")
        p1 = (r2 or {}).get("props", {})
        rc3, r3 = env.reply("props")
        check(rc == 0 and (r or {}).get("changed") == 3 and p1.get("walpe_fit", {}).get("value") == "stretch" and
              p1.get("walpe_pos_x", {}).get("value") == 10 and
              (r3 or {}).get("props", {}).get("walpe_fit", {}).get("value") == "contain",
              "setprop --monitor 1 with three options reaches that monitor's wallpaper only")
        rc, r = env.reply("resetprops", "--path", pic)
        rc_b, r2 = env.reply("props", "--path", pic)
        check(rc == 0 and (r or {}).get("removed") == 3 and
              (r2 or {}).get("props", {}).get("walpe_fit", {}).get("value") is None,
              "resetprops --path drops the three options")
        rc, r = env.reply("resetprops")
        check(rc == 0 and (r or {}).get("removed") == 1, "resetprops drops the default wallpaper's option")

        # ---- pause / resume, quality, the rest
        rc, r = env.reply("pause")
        check(rc == 0 and (env.status() or {}).get("paused") is True, "pause")
        rc, r = env.reply("resume")
        check(rc == 0 and (env.status() or {}).get("paused") is False, "resume")
        rc, out, _ = env.ctl("quality", "--json")
        try:
            q = json.loads(out).get("quality")
        except ValueError:
            q = None
        check(rc == 0 and isinstance(q, dict) and q.get("quality_preset") == "high", "quality --json")
        rc, out, _ = env.ctl("quality")
        check(rc == 0 and "preset: high" in out and "what the viewer gives up" in out, "quality prints the presets")
        rc, r = env.reply("quality", "--path", gif, "fps_idle=30")
        rc_b, out2, _ = env.ctl("quality", "--json")
        try:
            own = json.loads(out2)["quality"]["wallpapers"].get(gif)
        except (ValueError, KeyError, TypeError):
            own = None
        check(rc == 0 and own and own.get("fps_idle") == 30, "quality --path P fps_idle=30 is that wallpaper's own setting")
        rc, r = env.reply("quality", "--path", gif, "reset")
        check(rc == 0, "quality --path P reset")
        rc, out, _ = env.ctl("quality", "--path", gif, "fx_rate=9")
        check(rc == 1, "quality with a value that is not allowed exits with 1")
        rc, r = env.reply("cancel-import")
        check(rc == 0 and (r or {}).get("ok") is True, "cancel-import with nothing running")
        rc, r = env.reply("steam-libraries")
        check(rc == 0 and isinstance((r or {}).get("libraries"), list), "steam-libraries")
        rc, out, _ = env.ctl("click", "100", "100")
        check(rc == 3 and "no cursor events" in out, "click on a wallpaper without cursor events is refused with the reason")
        rc, out, _ = env.ctl("click", "5000", "5000")
        check(rc == 3 and "no monitor" in out, "click outside every monitor is refused")
        check(env.cfg().get("kept_by_hand") == {"a": [1, 2]}, "config.json holds the member it does not know to the end")

        # ---- quit
        rc, r = env.reply("quit")
        try:
            env.daemon.wait(timeout=20 * PATIENCE)
            ended = True
        except subprocess.TimeoutExpired:
            ended = False
        check(rc == 0 and ended and env.daemon.returncode == 0, "quit ends the engine (exit status 0)")
        rc, out, err = env.ctl("status")
        check(rc == 2 and "not running" in err, "without an engine: exit status 2 and a sentence")
        rc, out, err = env.ctl("set", "fps=30")
        check(rc == 2, "set without an engine: exit status 2")
    finally:
        env.close()
    print("ctlcheck: %d checks, %d failed" % (checks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
