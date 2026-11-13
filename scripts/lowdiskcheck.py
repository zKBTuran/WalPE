#!/usr/bin/env python3
"""End-to-end check of a video's sound track on a disk that runs out of room.

  python3 -I scripts/lowdiskcheck.py --build build [--daemon FILE] [--only a,b] [--keep] [--tmp DIR]

The first time a video with a sound track is heard, ffmpeg takes the track out into the
cache (~/.cache/walpe/video, FLAC). That is not begun, and is stopped, when less than
WALPE_CONVERT_FREE_MB (256) MiB are free where the cache is. Here the cache's video folder
is a small file system of its own (a tmpfs of 32 MiB), the clip is ten minutes of noise
(about 50 MiB of FLAC), and the daemon's `ffmpeg` is a script that hands the real one a
reading rate (-readrate), so that the track grows by a few MiB a second on any machine.
Uses the harness of scripts/lifecheck.py (private Xvfb, private daemon with its own XDG
dirs, sound into the null device): nothing of the user's is read or written.

  floor   a floor of 16 MiB is crossed while ffmpeg runs: ffmpeg ends, the ".part" is
          removed, there is no file under the track's name, the daemon says so in one
          line and stays, the video stays on the monitor (silent), and a pause and resume
          does not start ffmpeg again
  room    the next start, with room (the same file system made larger): the track is
          taken out, whole
  full    no floor, and the file system fills up: ffmpeg's own write error - one line
          that says what ffmpeg said, nothing left behind, the daemon stays
  begin   less than the floor is free before anything begins: nothing is started, one line

Needs a file system of a chosen size, that is: to mount one. As root the tmpfs is
mounted directly (and unmounted at the end); otherwise the check runs itself again in a
user namespace of its own (unshare --user --map-root-user --mount), where the mount is
allowed and ends with the check. Where neither can be had - and without Xvfb, the video
module, an ffmpeg program that knows -readrate and writes the clip - the check is
skipped (exit status 77) and says why in one line.
"""
import os, shutil, subprocess, sys, time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lifecheck as lc    # noqa: E402  (Env, check, loaded, wait_for, put)
import checkenv as rc     # noqa: E402
import displayenv         # noqa: E402

PATIENCE = rc.PATIENCE
SKIP = displayenv.SKIP
FS_MB, FLOOR_MB, ROOM_MB, FULL_MB = 32, 16, 128, 24
NS_MARK = "WALPE_LOWDISK_IN_NAMESPACE"


def say_skip(why):
    print("lowdiskcheck: skipped (%s)" % why, flush=True)
    return SKIP


def mount_tmpfs(where, mb):
    """mount (or resize) the tmpfs; the error text when that failed"""
    opt = "size=%dm,mode=700" % mb
    if os.path.ismount(where):
        opt = "remount," + opt
    try:
        p = subprocess.run(["mount", "-t", "tmpfs", "-o", opt, "walpe-lowdisk", where], stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as e:
        return str(e)
    return None if p.returncode == 0 else (p.stdout.decode(errors="replace").strip() or "mount failed")


def umount(where):
    for args in (["umount", where], ["umount", "-l", where]):
        if not os.path.ismount(where):
            return True
        try:
            subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            pass
    return not os.path.ismount(where)


def can_mount(tmp_parent):
    """None when this process may mount a tmpfs, else why not"""
    probe = os.path.join(tmp_parent, "walpe-lowdisk-probe-%d" % os.getpid())
    try:
        os.makedirs(probe)
    except OSError as e:
        return str(e)
    err = mount_tmpfs(probe, 1)
    if not err:
        umount(probe)
    try:
        os.rmdir(probe)
    except OSError:
        pass
    return err


def gen_noise_clip(path, seconds):
    """a small picture and `seconds` of white noise: a track that FLAC cannot make small"""
    try:
        p = subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                            "testsrc2=size=160x90:rate=5", "-f", "lavfi", "-i",
                            "anoisesrc=color=white:amplitude=0.5:sample_rate=48000:seed=7", "-t", str(seconds),
                            "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "25",
                            "-c:a", "aac", "-b:a", "192k", "-ac", "2", "-movflags", "+faststart", path],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=600 * PATIENCE)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return p.returncode == 0 and os.path.exists(path) and os.path.getsize(path) > 100000


def track_bytes(path):
    """the size of the FLAC track ffmpeg makes of the clip (the daemon's own arguments for it), 0 = unknown"""
    try:
        p = subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-vn", "-ac", "2",
                            "-ar", "48000", "-c:a", "flac", "-sample_fmt", "s16", "-f", "flac", "-"],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           timeout=600 * PATIENCE)
    except (OSError, subprocess.TimeoutExpired):
        return 0
    return len(p.stdout) if p.returncode == 0 else 0


class Run:
    """one daemon on the clip, and what is seen of ffmpeg and the cache while it runs"""

    def __init__(self, env, clip, cache, bindir):
        self.env, self.clip, self.cache, self.bindir = env, clip, cache, bindir
        self.peak = 0

    def start(self, floor_mb):
        rc.jdump(self.env.config, {"config_version": 3, "fps": 30, "pause_covered": False, "pause_dpms": False,
                                   "scene_sound": True})
        self.mark = self.env.mark()
        self.peak = 0
        return self.env.start_daemon([("A", 640, 360, 0, 0)], extra={
            "PATH": self.bindir + os.pathsep + os.environ.get("PATH", ""), "WALPE_SOUND_NULL": "1",
            "WALPE_CONVERT_FREE_MB": str(floor_mb)})

    def names(self):
        try:
            return sorted(os.listdir(self.cache))
        except OSError:
            return []

    def parts(self):
        return [n for n in self.names() if n.endswith(".part")]

    def tracks(self):
        return [n for n in self.names() if n.endswith("-sound.flac")]

    def part_bytes(self):
        n = 0
        for name in self.parts():
            try:
                n = max(n, os.path.getsize(os.path.join(self.cache, name)))
            except OSError:
                pass
        self.peak = max(self.peak, n)
        return n

    def ffmpegs(self):
        """processes below the daemon (ffmpeg, or the script that becomes it)"""
        pids, me = [], str(self.env.daemon.pid) if self.env.daemon else "-"
        for name in os.listdir("/proc"):
            if name.isdigit():
                try:
                    st = open(f"/proc/{name}/stat").read().rsplit(")", 1)[1].split()
                except (OSError, IndexError):
                    continue
                if st[1] != me or st[0] in "ZX":
                    continue
                try:
                    if b"ffmpeg" in open(f"/proc/{name}/cmdline", "rb").read():
                        pids.append(int(name))
                except OSError:
                    pass
        return pids

    def lines(self, text):
        return [ln for ln in self.env.since(self.mark).splitlines() if text in ln]

    def warnings(self):
        return [ln for ln in self.lines("WARNING") if "sound track" in ln]

    def shown(self):
        m = (self.env.monitors() or [{}])[0]
        return (m.get("current") == self.clip and m.get("type") == "video" and not m.get("load_error") and
                not m.get("loading"))

    def open_clip(self):
        self.env.open(self.clip)
        return lc.loaded(self.env, self.clip)

    def alive(self):
        return self.env.daemon is not None and self.env.daemon.poll() is None


def case_floor(r, total_mb):
    check = lc.check
    if not check(r.start(FLOOR_MB), "floor: daemon started"):
        return
    if not check(r.open_clip(), "floor: the video is shown", rc.brief(r.env.monitors() or [])):
        return
    # the track grows ...
    grew = rc.wait_for(lambda: r.part_bytes() > (1 << 20), 30, 0.05)
    check(bool(grew) and len(r.ffmpegs()) == 1, "floor: the track is being taken out (a \".part\" that grows, "
          "one ffmpeg)", (r.names(), r.ffmpegs()))
    # ... until the floor is reached
    rc.wait_for(lambda: (r.part_bytes(), r.warnings())[1], 60, 0.05)
    w = r.warnings()
    check(len(w) == 1 and "could not be taken out" in w[0] and "less than %d MB are left" % FLOOR_MB in w[0] and
          "stays silent" in w[0], "floor: one line says that the track was stopped, why, and that the video "
          "stays silent", w or r.env.since(r.mark)[-400:])
    lo, hi = (FS_MB - FLOOR_MB) * 0.5, FS_MB - 2
    check(lo * (1 << 20) <= r.peak <= hi * (1 << 20),
          f"floor: it was stopped part-way, by the floor and not by a full disk (the \".part\" reached "
          f"{r.peak / (1 << 20):.1f} MiB of about {total_mb:.0f}; wanted {lo:.0f} to {hi:.0f})")
    check(bool(rc.wait_for(lambda: not r.ffmpegs(), 5, 0.05)), "floor: ffmpeg has ended", r.ffmpegs())
    check(not r.parts() and not r.tracks(), "floor: no \".part\" and no file under the track's name", r.names())
    check(r.alive() and r.shown(), "floor: the daemon runs and the video is on the monitor",
          rc.brief(r.env.monitors() or []))
    # not tried again while the video stays: not by a pause and resume either
    r.env.ctl("pause")
    time.sleep(1.0)
    r.env.ctl("resume")
    time.sleep(4.0 * PATIENCE)
    check(len(r.warnings()) == 1 and not r.ffmpegs() and not r.names() and r.alive() and r.shown(),
          "floor: after a pause, a resume and 4 s: still one line, no ffmpeg, an empty cache, the video shown",
          (r.warnings(), r.ffmpegs(), r.names()))
    r.env.stop_daemon()


def case_room(r, total_mb):
    check = lc.check
    err = mount_tmpfs(r.cache, ROOM_MB)
    if not check(err is None, f"room: the file system is made {ROOM_MB} MiB", err):
        return
    if not check(r.start(FLOOR_MB), "room: daemon started again"):
        return
    if not check(r.open_clip(), "room: the video is shown", rc.brief(r.env.monitors() or [])):
        return
    done = rc.wait_for(lambda: (r.part_bytes(), r.tracks())[1], 120, 0.1)
    rc.wait_for(lambda: r.lines("taken out into"), 5, 0.1)
    size = os.path.getsize(os.path.join(r.cache, done[0])) if done else 0
    head = open(os.path.join(r.cache, done[0]), "rb").read(4) if done else b""
    check(bool(done) and head == b"fLaC" and abs(size / (1 << 20) - total_mb) < 1.0 and not r.parts(),
          f"room: with room the track is taken out, whole ({size / (1 << 20):.1f} MiB of {total_mb:.1f})",
          r.names())
    check(len(r.lines("taken out into")) == 1 and not r.warnings() and not r.ffmpegs() and r.alive() and r.shown(),
          "room: one line says so, no warning, no ffmpeg left, the video shown", (r.warnings(), r.ffmpegs()))
    r.env.stop_daemon()
    for n in r.names():
        os.unlink(os.path.join(r.cache, n))


def case_full(r, total_mb):
    check = lc.check
    err = mount_tmpfs(r.cache, FULL_MB)
    if not check(err is None, f"full: the file system is made {FULL_MB} MiB", err):
        return
    if not check(r.start(0), "full: daemon started without a floor (WALPE_CONVERT_FREE_MB=0)"):
        return
    if not check(r.open_clip(), "full: the video is shown", rc.brief(r.env.monitors() or [])):
        return
    rc.wait_for(lambda: (r.part_bytes(), r.warnings())[1], 90, 0.05)
    w = r.warnings()
    check(len(w) == 1 and "could not be taken out" in w[0] and "No space left on device" in w[0] and
          "stays silent" in w[0], "full: the disk fills up: one line with what ffmpeg said", w)
    check(r.peak >= (FULL_MB - 3) * (1 << 20), f"full: ... after the \".part\" had filled the file system "
          f"({r.peak / (1 << 20):.1f} of {FULL_MB} MiB)")
    check(bool(rc.wait_for(lambda: not r.ffmpegs(), 5, 0.05)) and not r.parts() and not r.tracks(),
          "full: ffmpeg has ended, no \".part\", no file under the track's name", (r.ffmpegs(), r.names()))
    time.sleep(2.0)
    check(len(r.warnings()) == 1 and r.alive() and r.shown() and not r.names(),
          "full: the daemon runs, the video is on the monitor, nothing is tried again", r.warnings())
    r.env.stop_daemon()


def case_begin(r, total_mb):
    check = lc.check
    if not check(r.start(FULL_MB + 8), f"begin: daemon started with a floor of {FULL_MB + 8} MiB on {FULL_MB}"):
        return
    if not check(r.open_clip(), "begin: the video is shown", rc.brief(r.env.monitors() or [])):
        return
    rc.wait_for(r.warnings, 30, 0.05)
    w = r.warnings()
    check(len(w) == 1 and "not taken out: less than %d MB are left" % (FULL_MB + 8) in w[0] and r.peak == 0 and
          not r.names() and not r.ffmpegs(), "begin: below the floor nothing is started, one line says why",
          (w, r.names(), r.ffmpegs()))
    check(r.alive() and r.shown(), "begin: the daemon runs and the video is on the monitor")
    r.env.stop_daemon()


CASES = [("floor", case_floor), ("room", case_room), ("full", case_full), ("begin", case_begin)]


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
        print("lowdiskcheck: skipped (%s)" % skip[0])
        return skip[1]
    for b in (daemon or os.path.join(build, "walpe-daemon"), os.path.join(build, "walpe-ctl")):
        if not os.path.exists(b):
            print(f"lowdiskcheck: {b} not found")
            return 2
    if not os.path.exists(os.path.join(build, "walpe-video.so")):
        return say_skip("this build has no video module")
    real = shutil.which("ffmpeg")
    if not real:
        return say_skip("needs the ffmpeg program")
    try:
        full_help = subprocess.run([real, "-hide_banner", "-h", "full"], stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        full_help = b""
    if b"-readrate" not in full_help:
        return say_skip("the ffmpeg program here has no -readrate (FFmpeg 5.0 or later), and without it the "
                        "track is written faster than the free disk is looked at")
    parent = os.path.dirname(os.path.abspath(tmp)) if tmp else (os.environ.get("TMPDIR") or "/tmp")
    why = can_mount(parent)
    if why:
        if os.environ.get(NS_MARK) or not shutil.which("unshare"):
            return say_skip("cannot mount a tmpfs for the cache: not root, and no user namespace that may "
                            "mount one here (%s)" % why.splitlines()[0][:120])
        # once more, as root of a user namespace of this check's own: its mounts end with it
        env = dict(os.environ)
        env[NS_MARK] = "1"
        try:
            p = subprocess.run(["unshare", "--user", "--map-root-user", "--mount", sys.executable, "-I",
                                os.path.abspath(__file__)] + sys.argv[1:], env=env)
        except OSError as e:
            return say_skip("cannot mount a tmpfs for the cache: not root, and unshare does not run (%s)" % e)
        return p.returncode

    env = lc.Env(build, daemon, keep, tmp, "xvfb")
    cache = os.path.join(env.env["XDG_CACHE_HOME"], "walpe", "video")
    mounted = False
    try:
        if len(env.sock.encode()) > 107:
            print(f"lowdiskcheck: {env.sock} is too long for a socket: pass --tmp <a short directory>")
            return 2
        wp = os.path.join(env.tmp, "wallpapers")
        clip = os.path.join(wp, "noise.mp4")
        os.makedirs(wp, exist_ok=True)
        if not gen_noise_clip(clip, 600):
            return say_skip("the ffmpeg program here writes no H.264 clip with an AAC track")
        total = track_bytes(clip)
        if total < (FS_MB + 8) << 20 or total > (ROOM_MB - FLOOR_MB - 8) << 20:
            return say_skip("the clip's track is %.1f MiB of FLAC here: not between %d and %d, which the sizes "
                            "of this check are made for" % (total / (1 << 20), FS_MB + 8, ROOM_MB - FLOOR_MB - 8))
        # the daemon's ffmpeg: the real one at 40 times the clip's pace (3 to 4 MiB of track a second)
        bindir = os.path.join(env.tmp, "paced")
        lc.put(os.path.join(bindir, "ffmpeg"), "#!/bin/sh\nexec '%s' -readrate 40 \"$@\"\n" % real)
        os.chmod(os.path.join(bindir, "ffmpeg"), 0o755)
        os.makedirs(cache, mode=0o700)
        err = mount_tmpfs(cache, FS_MB)
        if err:
            return say_skip("cannot mount a tmpfs for the cache (%s)" % err.splitlines()[0][:120])
        mounted = True
        print(f"lowdiskcheck: a tmpfs of {FS_MB} MiB is the cache's video folder ({cache}); the clip's track is "
              f"{total / (1 << 20):.1f} MiB of FLAC", flush=True)
        if not env.start_x(640, 360):
            return say_skip("the xvfb display did not start")
        r = Run(env, clip, cache, bindir)
        for name, fn in CASES:
            if only and name not in only:
                continue
            try:
                fn(r, total / (1 << 20))
            finally:
                env.stop_daemon()
    finally:
        env.stop_daemon()
        env.stop_x()
        if mounted and not umount(cache):
            print(f"lowdiskcheck: {cache} is still mounted (umount it by hand)", flush=True)
        env.close()
    print(f"lowdiskcheck: {lc.checks} checks, {lc.fails} failed")
    return 1 if lc.fails else 0


if __name__ == "__main__":
    sys.exit(main())
