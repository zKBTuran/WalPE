#!/usr/bin/env python3
"""Cross-build WalPE for arm64 on an x86-64 machine and run its tests under emulation.

  python3 -I arm64cross.py --src DIR --work DIR [--sysroot DIR] [--cross-file FILE ...]
        [--jobs N] [--nice N] [--min-free-mb N] [--timeout-multiplier N] [--tail N] [--suite NAME]
        [--with-scenescript] [--with-scripts] [--build-only] [--keep-build]
        [-- MESON SETUP ARGUMENT ...]

Needs aarch64-linux-gnu-gcc, qemu-aarch64 (package qemu-user; qemu-aarch64-static is
taken too) and the arm64 libraries tools/dev/arm64-sysroot.sh prepares:

  --sysroot DIR   a private sysroot (arm64-sysroot.sh private); the compiler, pkg-config
                  and the emulator are pointed at it
  without         arm64 as a foreign architecture of the system (arm64-sysroot.sh
                  multiarch): libraries in /usr/lib/aarch64-linux-gnu

The compiler and the machine description come from the tree's meson cross file: given
with --cross-file, else looked for in the tree (cross/aarch64-linux-gnu.ini, else a file
with aarch64 or arm64 in its name under cross/, meson/ or the top directory; a file that
only names an emulator is not taken), else a plain one for aarch64-linux-gnu-gcc is
written to the work directory. A second cross file with what belongs to this machine (the
sysroot, pkg-config's directories, the emulator as exe_wrapper) is written to
<work>/arm64-local.ini and given after it: it is the only place the emulator is named.

Tests: meson runs every test program that was cross-built through the emulator. What a
native script starts (the end-to-end checks: sh / python3 tests that start walpe-daemon),
and test programs that are handed another built program to start, cannot run that way:
they are left out and listed, unless --with-scripts is given and the kernel starts arm64
programs by itself (binfmt_misc with qemu, package qemu-user-binfmt). Tests that need a
GL context find none unless the sysroot has Mesa for arm64 (arm64-sysroot.sh --with-gl)
and then skip themselves, as they do on any machine without one.

Defaults that differ from a native build: -Dnative_tuning=generic; web wallpapers off
when the sysroot has no WebKitGTK; SceneScript on only when the sysroot has QuickJS for
arm64 (<sysroot>/usr/lib/quickjs/libquickjs.a, given to the build as -Dquickjs_libdir=)
or with --with-scenescript, else -Dscenescript=disabled: the library the build machine
has installed is for another CPU. Timeouts are multiplied by 10 under emulation
(--timeout-multiplier). --suite NAME runs only that meson test suite (the tree's
`portable` suite is the one to start with).

The build directory is <work>/build, removed at the end with the console output of the
steps in <work>/logs (--keep-build keeps both: logs/build.txt is where a compiler warning
of the arm64 build can be read). Exit
status 0 = built and every test that ran passed, 1 = something failed, 2 = wrong command
line or a missing program, 3 = not enough free disk space."""
import ast, configparser, glob, json, os, shutil, subprocess, sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import buildmatrix as bm   # noqa: E402  (steps with their console output in a file, result lines, re-runs)

TRIPLET = "aarch64-linux-gnu"
MARK = ".walpe-arm64cross"      # in <work>/build and <work>/logs: "made by this tool"
FALLBACK = """# written by tools/dev/arm64cross.py: no cross file was given or found in the tree
[binaries]
c = '%(t)s-gcc'
ar = '%(t)s-gcc-ar'
nm = '%(t)s-gcc-nm'
ranlib = '%(t)s-gcc-ranlib'
strip = '%(t)s-strip'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
""" % {"t": TRIPLET}


def say(text=""):
    print(text, flush=True)


def find_cross_file(src):
    for d in ("cross", "meson", "."):
        for pat in (TRIPLET + ".ini", "*aarch64*", "*arm64*"):
            for f in sorted(glob.glob(os.path.join(src, d, pat))):
                if os.path.isfile(f) and os.path.splitext(f)[1] in (".ini", ".txt", ".cross", "") and \
                        "qemu" not in os.path.basename(f):
                    return f
    return None


def machine_values(files):
    """what the given cross files already say for the keys the local file adds to:
    {(section, key): value}; a value that is not a plain literal is None"""
    out = {}
    for f in files:
        cp = configparser.ConfigParser(interpolation=None)
        try:
            cp.read(f)
        except configparser.Error:
            continue
        for sec, key in (("built-in options", "c_args"), ("built-in options", "c_link_args"),
                         ("binaries", "pkg-config"), ("binaries", "pkgconfig"), ("binaries", "exe_wrapper")):
            if cp.has_option(sec, key):
                try:
                    out[(sec, key)] = ast.literal_eval(cp.get(sec, key))
                except (ValueError, SyntaxError):
                    out[(sec, key)] = None
    return out


def as_list(v):
    return [v] if isinstance(v, str) else list(v or [])


def local_cross_file(path, sysroot, qemu, given):
    """the part of the cross setup that belongs to this machine"""
    have = machine_values(given)
    root = sysroot or "/"
    libdirs = [os.path.join(root, "usr/lib", TRIPLET, "pkgconfig"), os.path.join(root, "usr/share/pkgconfig")]
    if sysroot:     # (the system's own /usr/lib/pkgconfig may describe native libraries)
        libdirs.insert(1, os.path.join(root, "usr/lib/pkgconfig"))
    wrapper = [qemu] + (["-L", sysroot] if sysroot else [])
    lines = ["# written by tools/dev/arm64cross.py: what belongs to this machine", "[binaries]",
             "exe_wrapper = %r" % wrapper]
    if ("binaries", "pkg-config") not in have and ("binaries", "pkgconfig") not in have:
        lines.append("pkg-config = 'pkg-config'")
    lines += ["", "[properties]", "pkg_config_libdir = %r" % libdirs]
    notes = []
    if sysroot:
        lines.append("sys_root = %r" % sysroot)
        lines += ["", "[built-in options]"]
        for key in ("c_args", "c_link_args"):
            old = have.get(("built-in options", key), [])
            if old is None:
                notes.append("the cross file's %s is not a plain list: this run replaces it with --sysroot only" % key)
                old = []
            lines.append("%s = %r" % (key, as_list(old) + ["--sysroot=" + sysroot]))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return notes


def binfmt_arm64():
    """does the kernel start arm64 programs by itself (binfmt_misc with an emulator)"""
    d = "/proc/sys/fs/binfmt_misc"
    try:
        for n in os.listdir(d):
            if "aarch64" in n or "arm64" in n:
                with open(os.path.join(d, n)) as f:
                    if f.readline().strip() == "enabled":
                        return True
    except OSError:
        pass
    return False


def classify(build):
    """the build's tests -> (programs the emulator can run, {name: why not} for the others)"""
    try:
        tests = json.load(open(os.path.join(build, "meson-info", "intro-tests.json")))
    except (OSError, ValueError):
        return [], {}
    inside = os.path.abspath(build) + os.sep
    run, other = [], {}

    def built(arg, base):
        # (an argument like ./walpe-ctl is a path from the directory meson starts the test
        # in, not from the one this tool was started in)
        p = os.path.abspath(os.path.join(base, arg))
        return p.startswith(inside) and os.path.isfile(p) and os.access(p, os.X_OK)

    for t in tests:
        cmd = [str(c) for c in t.get("cmd") or []]
        base = os.path.abspath(t.get("workdir") or build)
        if not cmd or not os.path.abspath(cmd[0]).startswith(inside):
            other[t["name"]] = "a native script that starts the built programs"
        elif any(built(c, base) for c in cmd[1:]):
            other[t["name"]] = "is handed another built program to start"
        else:
            run.append(t["name"])
    return run, other


def pixbuf_cache(sysroot, qemu):
    """a private sysroot is unpacked, not installed: the list of gdk-pixbuf's picture
    loaders, which installing would have written, is made here (under the emulator)"""
    base = os.path.join(sysroot, "usr/lib", TRIPLET, "gdk-pixbuf-2.0")
    tool = os.path.join(base, "gdk-pixbuf-query-loaders")
    for d in glob.glob(os.path.join(base, "2.*")):
        cache = os.path.join(d, "loaders.cache")
        if os.path.exists(tool) and not os.path.exists(cache):
            try:
                p = subprocess.run([qemu, "-L", sysroot, tool], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                   stdin=subprocess.DEVNULL, timeout=300)
                if p.returncode == 0 and p.stdout:
                    with open(cache, "wb") as f:
                        f.write(p.stdout)
                    say("  wrote %s" % cache)
            except (OSError, subprocess.TimeoutExpired):
                pass


def main():
    a = {"src": None, "work": None, "sysroot": None, "jobs": 2, "nice": 10, "min_free": 3000, "mult": 10,
         "tail": 60, "scenescript": False, "scripts": False, "build_only": False, "keep_build": False,
         "suite": None}
    cross, extra = [], []
    flags = {"--with-scenescript": "scenescript", "--with-scripts": "scripts", "--build-only": "build_only",
             "--keep-build": "keep_build"}
    values = {"--src": "src", "--work": "work", "--sysroot": "sysroot", "--suite": "suite"}
    numbers = {"--jobs": "jobs", "--nice": "nice", "--min-free-mb": "min_free", "--timeout-multiplier": "mult",
               "--tail": "tail"}
    args = sys.argv[1:]
    try:
        while args:
            x = args.pop(0)
            if x == "--":
                extra, args = args, []
            elif x in flags:
                a[flags[x]] = True
            elif x in values:
                a[values[x]] = args.pop(0)
            elif x in numbers:
                a[numbers[x]] = int(args.pop(0))
            elif x == "--cross-file":
                cross.append(os.path.abspath(args.pop(0)))
            else:
                raise ValueError(x)
        if not a["src"] or not a["work"]:
            raise ValueError("--src and --work")
    except (ValueError, IndexError):
        sys.stderr.write(__doc__ + "\n")
        return 2
    src, work = os.path.abspath(a["src"]), os.path.abspath(a["work"])
    sysroot = os.path.abspath(a["sysroot"]) if a["sysroot"] else None
    qemu = shutil.which("qemu-aarch64") or shutil.which("qemu-aarch64-static")
    lack = [p for p in (TRIPLET + "-gcc", "meson", "ninja", "pkg-config") if not shutil.which(p)]
    if not qemu and not a["build_only"]:
        lack.append("qemu-aarch64")
    if lack:
        say("arm64cross: missing: " + ", ".join(lack))
        return 2
    libdir = os.path.join(sysroot or "/", "usr/lib", TRIPLET)
    if not os.path.isdir(os.path.join(libdir, "pkgconfig")):
        say("arm64cross: no arm64 libraries in %s (prepare them with arm64-sysroot.sh)" % libdir)
        return 2
    os.makedirs(work, exist_ok=True)
    free = shutil.disk_usage(work).free // (1024 * 1024)
    if free < a["min_free"]:
        say("arm64cross: %d MB free in %s, %d MB wanted (--min-free-mb)" % (free, work, a["min_free"]))
        return 3

    notes = []
    if not cross:
        found = find_cross_file(src)
        if found:
            cross = [found]
        else:
            fallback = os.path.join(work, "arm64-fallback.ini")
            with open(fallback, "w") as f:
                f.write(FALLBACK)
            cross = [fallback]
            notes.append("no cross file in the tree: a plain one for %s-gcc was used" % TRIPLET)
    local = os.path.join(work, "arm64-local.ini")
    notes += local_cross_file(local, sysroot, qemu or "qemu-aarch64", cross)
    opts = bm.read_options(src)
    setup = []
    for c in cross + [local]:
        setup += ["--cross-file", c]
    if "native_tuning" in opts:
        setup.append("-Dnative_tuning=generic")
    if "web" in opts and not os.path.exists(os.path.join(libdir, "pkgconfig", "webkitgtk-6.0.pc")):
        setup.append("-Dweb=" + bm.onoff(opts["web"], False))
        notes.append("web wallpapers are off: the sysroot has no WebKitGTK")
    qjs = os.path.join(sysroot or "/nonexistent", "usr/lib/quickjs")
    if "quickjs_libdir" in opts and os.path.exists(os.path.join(qjs, "libquickjs.a")):
        setup.append("-Dquickjs_libdir=" + qjs)
    elif "scenescript" in opts and not a["scenescript"]:
        setup.append("-Dscenescript=" + bm.onoff(opts["scenescript"], False))
        notes.append("SceneScript is off: no QuickJS for arm64 the build could be pointed at "
                     "(a private sysroot has it; the tree needs the quickjs_libdir option)")
    setup += extra

    # (only directories this tool made are ever removed: each carries its marker file)
    build = os.path.join(work, "build")
    logs = os.path.join(work, "logs")
    for d, fresh in ((logs, False), (build, True)):
        if not bm.own_dir(d, MARK, fresh):
            say("arm64cross: %s exists and was not made by this tool" % d)
            return 2
    row = {"cell": "arm64-cross"}
    failed, left_out = [], {}
    try:
        say("=== meson setup %s" % " ".join(setup))
        log = os.path.join(logs, "configure.txt")
        status, _ = bm.step(["meson", "setup", build, src] + setup, log, 1800, nice=a["nice"])
        bm.scrub(log, "from environment")
        row["configure"] = "passed" if status == 0 else "FAILED"
        if status != 0:
            bm.tail(log, 40)
            return 1
        say("=== build (-j%d)" % a["jobs"])
        log = os.path.join(logs, "build.txt")
        status, secs = bm.step(["ninja", "-C", build, "-j", str(a["jobs"])], log, 4 * 3600, nice=a["nice"])
        row["build"] = "passed (%d min)" % (secs // 60) if status == 0 else "FAILED"
        if status != 0:
            bm.tail(log, 40)
            return 1
        if a["build_only"]:
            row["tests"] = "not run (--build-only)"
            return 0
        names, left_out = classify(build)
        native = set(left_out)      # started by a native program (sh, python3) or handed a built one
        if a["suite"]:
            in_suite = bm.suite_tests(build, a["suite"])
            names = [n for n in names if n in in_suite]
            left_out = {n: why for n, why in left_out.items() if n in in_suite}
        if a["scripts"]:
            if binfmt_arm64():
                names, left_out = names + sorted(left_out), {}
            else:
                notes.append("--with-scripts: the kernel does not start arm64 programs by itself here "
                             "(no binfmt_misc entry): those tests stay left out")
        if sysroot:
            pixbuf_cache(sysroot, qemu)
        env = dict(os.environ)
        if sysroot:
            env["QEMU_LD_PREFIX"] = sysroot
        for k in ("RESTORECHECK_PATIENCE", "LIFECHECK_PATIENCE", "WAYLANDCHECK_PATIENCE"):
            env[k] = "10"
        gl = glob.glob(os.path.join(libdir, "libEGL_mesa.so*"))
        notes.append("GL: the sysroot has Mesa for arm64, tests with a GL part run it under emulation" if gl else
                     "GL: no arm64 GL driver in the sysroot, tests with a GL part skip that part themselves")
        if not names:
            row["tests"] = "FAILED (the build lists no test program)"
            return 1
        say("=== meson test: %d test programs through %s (timeouts x%d)" % (len(names), os.path.basename(qemu), a["mult"]))
        log = os.path.join(logs, "test.txt")
        cmd = ["meson", "test", "-C", build, "--no-rebuild", "--num-processes", str(a["jobs"]), "-t", str(a["mult"])]
        status, secs = bm.step(cmd + names, log, 12 * 3600, env=env, nice=a["nice"])
        res = bm.results(log)
        failed = sorted(n for n, r in res.items() if r in bm.BAD)
        row["failed"] = failed
        if not res:
            row["tests"] = "FAILED (no result lines; meson test ended with %s)" % status
            bm.tail(log, 20)
            return 1
        row["tests"] = ("passed" if not failed and status == 0 else "FAILED") + " (%s, %d min)" % (bm.counts(res), secs // 60)
        skipped = sorted(n for n, r in res.items() if r == "SKIP")
        if skipped:
            say("  skipped by themselves: " + ", ".join(skipped))
        wrapper = [qemu] + (["-L", sysroot] if sysroot else [])
        for name in failed:
            # (the emulator in front of the cross-built test programs only: in front of
            # /bin/sh or python3 it would refuse the native program)
            bm.rerun(build, name, env, a["mult"], a["tail"], wrapper if name not in native else [], a["nice"])
        if failed:
            notes.append("a test that starts a cross-built program itself fails with \"Exec format error\" "
                         "unless the kernel starts arm64 programs (binfmt_misc)")
        return 1 if failed or status != 0 else 0
    finally:
        if not a["keep_build"]:
            bm.drop_dir(build, MARK)
            bm.drop_dir(logs, MARK)
        else:
            say("kept: %s and the console output of the steps in %s" % (build, logs))
        say("\n" + "\n".join(bm.table([row])))
        if left_out:
            say("left out here (%d): they need the kernel to start arm64 programs" % len(left_out))
            for n in sorted(left_out):
                say("  %-24s %s" % (n, left_out[n]))
        for n in notes:
            say("note: " + n)


if __name__ == "__main__":
    sys.exit(main())
