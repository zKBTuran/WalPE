#!/usr/bin/env python3
"""Build matrix: configure, build and test WalPE in every combination that has to keep working.

  python3 -I buildmatrix.py --src DIR --work DIR [--cells a,b,...] [--full] [--list]
        [--jobs N] [--nice N] [--min-free-mb N] [--patience N] [--timeout-multiplier N]
        [--cc NAME] [--no-leaks] [--strict] [--keep-logs] [--tail N]

A cell is <backend>-<kind>-<tuning>[-min]:

  backend  x11 | wayland | both      which window-system backends are built
                                     (-Dx11= / -Dwayland=, read from meson_options.txt; a tree
                                     without these options has the x11 cells only)
  kind     release                   the build as shipped (buildtype release, -O3, LTO)
           asan                      AddressSanitizer + UndefinedBehaviorSanitizer, no LTO.
                                     Passed = no report with leak detection off; the tests
                                     are then run again with leak detection on and what
                                     fails only there is listed, not counted
           tsan                      ThreadSanitizer, no LTO, tests/tsan.supp as suppressions
  tuning   generic | alderlake       -Dnative_tuning=; alderlake is compiled everywhere and
                                     tested only where the CPU has what the flags ask for
  -min                               every optional part off (web, video, audio,
                                     scenescript, tray: those meson_options.txt has)

Without --cells: both / x11 / wayland as release and as asan with generic tuning, both as
release with alderlake, and both-release-generic-min. --full: every backend with every kind
and tuning, plus the -min cell of each backend. --list prints the cells and their options.

One build directory at a time (<work>/build, made and removed by this tool; the steps'
console output in <work>/logs while it runs, the table also in <work>/summary.txt). The
tool removes only directories it made itself (each carries a file .walpe-build-matrix):
a <work>/build, <work>/logs or <work>/reports/<cell> that was there before is left as it
is and the cell is not run. Before each cell the free space of <work> is
looked at: below --min-free-mb (default 3000) the run stops there with what it has. Every
command runs under nice, builds and tests with --jobs processes (default 2).

A failing test is shown by running its program once more (command line from the build's
meson-info/intro-tests.json) and printing the end of its output; meson's own log files are
never read, printed or kept. Sanitizer reports go to <work>/reports/<cell>/ and their
summary lines are printed.

--patience N (default 3) is given to the end-to-end checks as their wait multiplier,
--timeout-multiplier N (default: 3, asan cells 10, tsan cells 20) to meson test. --strict sets
WALPE_CHECK_STRICT=1: a check that cannot run here (no Xvfb, no sway ...) fails instead of
being skipped. --cc NAME: the C compiler (gcc, clang).

The table at the end has one row per cell: configure / build / tests as passed or failed
with the counts, and the names of the failing tests. Exit status 0 = every cell that ran
passed, 1 = something failed, 2 = wrong command line, 3 = stopped for lack of disk space."""
import json, os, platform, re, shutil, signal, subprocess, sys, time

BACKENDS = ("both", "x11", "wayland")
KINDS = ("release", "asan", "tsan")
TUNINGS = ("generic", "alderlake")
OPTIONAL = ("web", "video", "audio", "scenescript", "tray")
DEFAULT = ["both-release-generic", "x11-release-generic", "wayland-release-generic",
           "both-asan-generic", "x11-asan-generic", "wayland-asan-generic",
           "both-release-alderlake", "both-release-generic-min"]
# /proc/cpuinfo flags a binary built with -march=alderlake may use
ALDERLAKE = ("avx2", "fma", "bmi1", "bmi2", "movbe", "f16c", "adx", "rdseed", "clflushopt", "clwb",
             "sha_ni", "gfni", "vaes", "vpclmulqdq", "avx_vnni", "pku", "waitpkg", "movdiri",
             "movdir64b", "serialize")
RESULT = re.compile(r"^\s*\d+/\d+\s+(.*?)\s+(OK|EXPECTEDFAIL|UNEXPECTEDPASS|FAIL|SKIP|TIMEOUT|INTERRUPT|ERROR)"
                    r"\s+\d+\.\d+s")
BAD = ("FAIL", "TIMEOUT", "UNEXPECTEDPASS", "INTERRUPT", "ERROR")
# the file that says "this tool made this directory": nothing without it is ever removed
MARK = ".walpe-build-matrix"
running = set()


def say(text=""):
    print(text, flush=True)


def read_options(src):
    """meson_options.txt -> {name: type}"""
    try:
        text = open(os.path.join(src, "meson_options.txt")).read()
    except OSError:
        return {}
    out = {}
    parts = re.split(r"\boption\s*\(", text)[1:]
    for p in parts:
        name = re.match(r"\s*'([^']+)'", p)
        kind = re.search(r"\btype\s*:\s*'(\w+)'", p)
        if name and kind:
            out[name.group(1)] = kind.group(1)
    return out


def onoff(kind, on):
    if kind == "boolean":
        return "true" if on else "false"
    return "enabled" if on else "disabled"      # feature, and the tree's auto/enabled/disabled combos


def parse_cell(cell):
    parts = cell.split("-")
    minimal = parts[-1] == "min"
    if minimal:
        parts = parts[:-1]
    if len(parts) != 3 or parts[0] not in BACKENDS or parts[1] not in KINDS or parts[2] not in TUNINGS:
        return None
    return parts[0], parts[1], parts[2], minimal


def cell_options(cell, opts):
    """-> (meson setup arguments, why the cell cannot exist in this tree or None)"""
    backend, kind, tuning, minimal = parse_cell(cell)
    args, na = [], None
    if "tests" in opts:
        args.append("-Dtests=true")     # a tree with this option has no tests unless asked
    for ws in ("x11", "wayland"):
        want = backend in (ws, "both")
        if ws in opts:
            args.append("-D%s=%s" % (ws, onoff(opts[ws], want)))
        elif want != (ws == "x11"):
            na = "meson_options.txt has no '%s' option" % ws
    if kind in ("asan", "tsan"):
        args += ["-Db_sanitize=" + ("address,undefined" if kind == "asan" else "thread"),
                 "-Db_lto=false", "-Db_lundef=false", "-Dc_args=-fno-omit-frame-pointer -g1"]
    if "native_tuning" in opts:
        args.append("-Dnative_tuning=" + tuning)
    elif tuning != "generic":
        na = "meson_options.txt has no 'native_tuning' option"
    if tuning == "alderlake" and platform.machine() not in ("x86_64", "AMD64"):
        na = "alderlake tuning is for x86-64"
    if minimal:
        args += ["-D%s=%s" % (o, onoff(opts[o], False)) for o in OPTIONAL if o in opts]
    return args, na


def cpu_lacks():
    """alderlake instruction sets this CPU does not have (so the binaries cannot be run)"""
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("flags"):
                have = set(line.split(":", 1)[1].split())
                return [f for f in ALDERLAKE if f not in have]
    except OSError:
        pass
    return list(ALDERLAKE)


def own_dir(path, mark=MARK, fresh=True):
    """a directory of this tool's own: made new with the marker file in it, or, when the
    path exists, taken only if it is a directory that carries the marker (and emptied
    first when fresh). False = the path exists and was not made by this tool: it is not
    touched. A wrong --work can therefore cost nothing that was there before."""
    if os.path.lexists(path):
        if os.path.islink(path) or not os.path.isfile(os.path.join(path, mark)):
            return False
        if fresh:
            shutil.rmtree(path)
    os.makedirs(path, exist_ok=True)
    open(os.path.join(path, mark), "w").close()
    return True


def drop_dir(path, mark=MARK):
    """remove a directory, but only one this tool made (it carries the marker)"""
    if not os.path.islink(path) and os.path.isfile(os.path.join(path, mark)):
        shutil.rmtree(path, ignore_errors=True)


def drop_unused(path, mark=MARK):
    """remove a directory of this tool's own that holds nothing but the marker"""
    try:
        if os.listdir(path) == [mark]:
            drop_dir(path, mark)
    except OSError:
        pass


def kill_group(p):
    """end a step we started, with everything it started (it leads a process group of its own)"""
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(p.pid, sig)
        except OSError:
            return
        try:
            p.wait(timeout=15)
            return
        except subprocess.TimeoutExpired:
            pass


def step(cmd, log, timeout, env=None, cwd=None, nice=10):
    """one command with its console output in a file -> (exit status or "timeout", seconds)"""
    t0 = time.time()
    with open(log, "wb") as f:
        try:
            p = subprocess.Popen(["nice", "-n", str(nice)] + cmd, stdout=f, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, env=env, cwd=cwd, start_new_session=True)
        except OSError as e:
            f.write(("cannot run %s: %s\n" % (cmd[0], e.strerror)).encode())
            return 127, 0.0
        running.add(p)
        try:
            status = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            kill_group(p)
            status = "timeout"
        except BaseException:       # interrupted: the step runs in a session of its own
            kill_group(p)
            raise
        finally:
            running.discard(p)
    return status, time.time() - t0


def tail(path, n, drop=None):
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError:
        return
    if drop:
        lines = [ln for ln in lines if drop not in ln]
    for ln in lines[-n:]:
        say("    | " + ln)


def scrub(path, drop):
    """remove from a console log the lines in which meson quotes variables of the environment"""
    try:
        lines = open(path, errors="replace").read().splitlines(True)
        with open(path, "w") as f:
            f.writelines(ln for ln in lines if drop not in ln)
    except OSError:
        pass


def results(log):
    """meson test's console output -> {test name: result}"""
    out = {}
    try:
        for ln in open(log, errors="replace"):
            m = RESULT.match(ln)
            if m:
                out[m.group(1).split(" / ")[-1].strip()] = m.group(2)
    except OSError:
        pass
    return out


def test_entry(build, name):
    try:
        for t in json.load(open(os.path.join(build, "meson-info", "intro-tests.json"))):
            if t.get("name") == name:
                return t
    except (OSError, ValueError):
        pass
    return None


def suite_tests(build, suite):
    """names of the build's tests that belong to a suite (meson lists it as "project:suite")"""
    try:
        tests = json.load(open(os.path.join(build, "meson-info", "intro-tests.json")))
    except (OSError, ValueError):
        return set()
    return {t["name"] for t in tests if any(s.split(":")[-1] == suite for s in t.get("suite") or [])}


def rerun(build, name, env, mult, lines, wrapper, nice):
    """show a failing test: its program once more, the end of what it prints"""
    t = test_entry(build, name)
    if not t:
        say("    (no entry for %s in the build's test list)" % name)
        return
    e = dict(env)
    e.update(t.get("env") or {})
    cmd = ["nice", "-n", str(nice)] + wrapper + list(t["cmd"])
    say("  %s once more: %s" % (name, " ".join(os.path.basename(c) if i == 0 else c
                                                 for i, c in enumerate(t["cmd"]))))
    try:
        p = subprocess.Popen(cmd, env=e, cwd=t.get("workdir") or build, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    except OSError as err:
        say("    cannot run it: %s" % err.strerror)
        return
    running.add(p)
    try:
        out, _ = p.communicate(timeout=max(30, (t.get("timeout") or 30) * mult))
        status = p.returncode
    except subprocess.TimeoutExpired:
        kill_group(p)
        # what it printed until then; not for ever: a child that left the process group
        # can still hold the pipe open
        try:
            out = p.communicate(timeout=10)[0] or b""
        except (subprocess.TimeoutExpired, OSError, ValueError):
            out = b""
        status = "timeout"
    except BaseException:
        kill_group(p)
        raise
    finally:
        running.discard(p)
    for ln in out.decode(errors="replace").splitlines()[-lines:]:
        say("    | " + ln)
    say("    exit status: %s" % status)


def reports(directory, lines):
    """sanitizer report files of a run: how many, their summary lines, the start of the first ones"""
    try:
        names = sorted(n for n in os.listdir(directory) if n != MARK)
    except OSError:
        return 0
    if not names:
        return 0
    say("  sanitizer reports: %d file(s) in %s" % (len(names), directory))
    seen = set()
    for n in names:
        try:
            text = open(os.path.join(directory, n), errors="replace").read().splitlines()
        except OSError:
            continue
        for ln in text:
            if ln.startswith("SUMMARY:") and ln not in seen:
                seen.add(ln)
                if len(seen) <= 40:
                    say("    " + ln)
    for n in names[:3]:
        say("  %s:" % n)
        for ln in open(os.path.join(directory, n), errors="replace").read().splitlines()[:lines]:
            say("    | " + ln)
    return len(names)


def sanitizer_env(kind, src, rep, leaks=False):
    os.makedirs(rep, exist_ok=True)
    if kind == "asan":
        env = {"ASAN_OPTIONS": "detect_leaks=%d:halt_on_error=1:abort_on_error=0:strict_string_checks=1:"
                               "detect_stack_use_after_return=1:print_summary=1:log_path=%s/asan" % (leaks, rep),
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1:print_summary=1:log_path=%s/ubsan" % rep}
        supp = os.path.join(src, "tests", "lsan.supp")
        if leaks and os.path.exists(supp):
            env["LSAN_OPTIONS"] = "suppressions=%s:print_suppressions=0" % supp
        return env
    if kind == "tsan":
        supp = os.path.join(src, "tests", "tsan.supp")
        # (the options of meson.build's test setup `tsan`, plus the report files)
        opts = "halt_on_error=0:exitcode=66:second_deadlock_stack=1:history_size=5:print_suppressions=0:" \
               "log_path=%s/tsan" % rep
        if os.path.exists(supp):
            opts = "suppressions=%s:" % supp + opts
        # Mesa's software rasteriser in the calling thread: its worker threads lock with
        # primitives the sanitizer cannot see
        return {"TSAN_OPTIONS": opts, "LP_NUM_THREADS": "0"}
    return {}


def tsan_wrapper(build, env):
    """ThreadSanitizer cannot map its shadow memory when the kernel randomises addresses
    with more bits than its layout allows ("unexpected memory mapping"). Try one small test
    program: as it is, then with address randomisation off for it (setarch -R).
    -> (wrapper argv for meson test, None) or (None, why the tests cannot run)"""
    t = test_entry(build, "region") or test_entry(build, "present")
    if not t:
        return [], None
    e = dict(env)
    e.pop("TSAN_OPTIONS", None)
    for wrapper in ([], ["setarch", platform.machine(), "-R"]):
        try:
            p = subprocess.run(wrapper + list(t["cmd"]), env=e, cwd=build, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if b"unexpected memory mapping" not in p.stdout and b"FATAL: ThreadSanitizer" not in p.stdout:
            return wrapper, None
    return None, ("ThreadSanitizer cannot start here (address-space layout); as root: "
                  "sysctl vm.mmap_rnd_bits=28, or allow setarch -R")


def run_tests(build, log, env, jobs, mult, wrapper, nice, limit):
    cmd = ["meson", "test", "-C", build, "--no-rebuild", "--num-processes", str(jobs), "-t", str(mult)]
    if wrapper:
        cmd += ["--wrapper", " ".join(wrapper)]
    status, secs = step(cmd, log, limit, env=env, nice=nice)
    return status, secs, results(log)


def counts(res):
    n = {"ok": 0, "fail": 0, "skip": 0}
    for r in res.values():
        n["fail" if r in BAD else "skip" if r == "SKIP" else "ok"] += 1
    return "%d ok, %d failed, %d skipped" % (n["ok"], n["fail"], n["skip"])


def do_cell(cell, a, opts, row):
    backend, kind, tuning, minimal = parse_cell(cell)
    setup_args, na = cell_options(cell, opts)
    if na:
        row.update(configure="n/a", note=na)
        return
    build = os.path.join(a["work"], "build")
    logs = os.path.join(a["work"], "logs")
    rep = os.path.join(a["work"], "reports", cell)
    # (logs and reports first: a cell that cannot have them must not leave a build behind)
    for d, fresh in ((logs, False), (rep, True), (rep + "-leaks", True), (build, True)):
        if not own_dir(d, fresh=fresh):
            row.update(configure="not run", note="%s exists and was not made by this tool" % d)
            drop_unused(rep)
            drop_unused(rep + "-leaks")
            return
    env = dict(os.environ)
    if a["cc"]:
        env["CC"] = a["cc"]
    try:
        say("\n=== %s: meson setup %s" % (cell, " ".join(setup_args)))
        log = os.path.join(logs, cell + ".configure.txt")
        status, secs = step(["meson", "setup", build, a["src"]] + setup_args, log, 1800, env=env, nice=a["nice"])
        scrub(log, "from environment")
        row["configure"] = "passed" if status == 0 else "FAILED"
        if status != 0:
            tail(log, 30)
            return
        say("=== %s: build (-j%d)" % (cell, a["jobs"]))
        log = os.path.join(logs, cell + ".build.txt")
        status, secs = step(["ninja", "-C", build, "-j", str(a["jobs"])], log, 4 * 3600, env=env, nice=a["nice"])
        row["build"] = "passed (%d min)" % (secs // 60) if status == 0 else "FAILED"
        if status != 0:
            tail(log, 40)
            return
        warned = sum(1 for ln in open(log, errors="replace") if "warning:" in ln)
        if warned:
            row["note"] = "%d compiler warning line(s)" % warned
        if tuning == "alderlake":
            lacks = cpu_lacks()
            if lacks:
                row["tests"] = "not run"
                row["note"] = "compiled only: this CPU lacks " + " ".join(lacks)
                return
        tenv = dict(os.environ)
        for k in ("RESTORECHECK_PATIENCE", "LIFECHECK_PATIENCE", "WAYLANDCHECK_PATIENCE", "CTLCHECK_PATIENCE"):
            tenv[k] = str(a["patience"])
        if a["strict"]:
            tenv["WALPE_CHECK_STRICT"] = "1"
        tenv.update(sanitizer_env(kind, a["src"], rep))
        mult = a["mult"] or {"release": 3, "tsan": 20}.get(kind, 10)     # (tsan: as the test setup `tsan`)
        wrapper = []
        if kind == "tsan":
            wrapper, why = tsan_wrapper(build, tenv)
            if wrapper is None:
                row.update(tests="not run", note=why)
                return
        say("=== %s: meson test (%d at a time, timeouts x%d)" % (cell, a["jobs"], mult))
        log = os.path.join(logs, cell + ".test.txt")
        status, secs, res = run_tests(build, log, tenv, a["jobs"], mult, wrapper, a["nice"], 8 * 3600)
        failed = sorted(n for n, r in res.items() if r in BAD)
        row["failed"] = failed
        if not res:
            row["tests"] = "FAILED (no result lines; meson test ended with %s)" % status
            tail(log, 20)
            return
        row["tests"] = ("passed" if not failed and status == 0 else "FAILED") + " (%s, %d min)" % (counts(res), secs // 60)
        skipped = sorted(n for n, r in res.items() if r == "SKIP")
        if skipped:
            say("  skipped: " + ", ".join(skipped))
        for name in failed:
            rerun(build, name, tenv, mult, a["tail"], wrapper, a["nice"])
        reports(rep, 30)
        if kind == "asan" and not a["no_leaks"]:
            say("=== %s: meson test again with leak detection (a listing, not counted)" % cell)
            rep2 = rep + "-leaks"
            lenv = dict(tenv)
            lenv.update(sanitizer_env(kind, a["src"], rep2, leaks=True))
            log = os.path.join(logs, cell + ".leaks.txt")
            _, _, res2 = run_tests(build, log, lenv, a["jobs"], mult, wrapper, a["nice"], 8 * 3600)
            only = sorted(n for n, r in res2.items() if r in BAD and n not in failed)
            row["leaks"] = only
            n = reports(rep2, 30)
            if res2 and len(only) > 0.8 * len(res2):
                say("  nearly every test fails with leak detection on: it probably cannot run here "
                    "(LeakSanitizer needs ptrace); the listing says nothing then")
                row["leaks"] = ["(leak detection cannot run here)"]
            elif not n and not only:
                say("  no leak reports")
    finally:
        drop_dir(build)
        drop_unused(rep)                # (a report directory nothing was written to says nothing)
        drop_unused(rep + "-leaks")


def table(rows):
    out = ["=" * 100, "%-28s %-10s %-18s %s" % ("cell", "configure", "build", "tests"), "-" * 100]
    for r in rows:
        out.append("%-28s %-10s %-18s %s" % (r["cell"], r.get("configure", "-"), r.get("build", "-"),
                                             r.get("tests", "-")))
        if r.get("failed"):
            out.append("%-28s   failing tests: %s" % ("", ", ".join(r["failed"])))
        if r.get("leaks"):
            out.append("%-28s   failing only with leak detection: %s" % ("", ", ".join(r["leaks"])))
        if r.get("note"):
            out.append("%-28s   %s" % ("", r["note"]))
    out.append("=" * 100)
    return out


def bad_row(r):
    return "FAILED" in (r.get("configure", "") + r.get("build", "") + r.get("tests", "")) or \
        r.get("configure") == "not run"


def main():
    a = {"src": None, "work": None, "cells": None, "full": False, "list": False, "jobs": 2, "nice": 10,
         "min_free": 3000, "patience": 3, "mult": 0, "cc": None, "no_leaks": False, "strict": False,
         "keep_logs": False, "tail": 60}
    flags = {"--full": "full", "--list": "list", "--no-leaks": "no_leaks", "--strict": "strict",
             "--keep-logs": "keep_logs"}
    values = {"--src": "src", "--work": "work", "--cells": "cells", "--cc": "cc"}
    numbers = {"--jobs": "jobs", "--nice": "nice", "--min-free-mb": "min_free", "--patience": "patience",
               "--timeout-multiplier": "mult", "--tail": "tail"}
    args = sys.argv[1:]
    try:
        while args:
            x = args.pop(0)
            if x in flags:
                a[flags[x]] = True
            elif x in values:
                a[values[x]] = args.pop(0)
            elif x in numbers:
                a[numbers[x]] = int(args.pop(0))
            else:
                raise ValueError(x)
    except (ValueError, IndexError):
        sys.stderr.write(__doc__ + "\n")
        return 2
    if not a["src"]:
        here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        a["src"] = here if os.path.exists(os.path.join(here, "meson.build")) else os.getcwd()
    a["src"] = os.path.abspath(a["src"])
    if not os.path.exists(os.path.join(a["src"], "meson.build")):
        sys.stderr.write("buildmatrix: no meson.build in %s (give the source tree with --src)\n" % a["src"])
        return 2
    opts = read_options(a["src"])
    if a["cells"]:
        cells = [c for c in a["cells"].split(",") if c]
    elif a["full"]:
        cells = ["%s-%s-%s" % (b, k, t) for b in BACKENDS for k in KINDS for t in TUNINGS] + \
                ["%s-release-generic-min" % b for b in BACKENDS]
    else:
        cells = list(DEFAULT)
    wrong = [c for c in cells if not parse_cell(c)]
    if wrong:
        sys.stderr.write("buildmatrix: not a cell: %s\n" % ", ".join(wrong))
        return 2
    if a["list"]:
        for c in cells:
            setup_args, na = cell_options(c, opts)
            say("%-28s %s" % (c, "n/a: " + na if na else " ".join(setup_args)))
        return 0
    if not a["work"]:
        sys.stderr.write("buildmatrix: --work DIR is needed (the build directory and the logs go there)\n")
        return 2
    a["work"] = os.path.abspath(a["work"])
    os.makedirs(a["work"], exist_ok=True)

    def stop(signum, frame):
        for p in list(running):
            kill_group(p)
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGHUP, stop)

    tools = ("meson", "ninja", "Xvfb", "xdotool", "sway", "grim", "ffmpeg", "python3")
    say("programs: " + ", ".join("%s %s" % (t, "yes" if shutil.which(t) else "NO") for t in tools))
    rows, stopped = [], None
    try:
        for cell in cells:
            free = shutil.disk_usage(a["work"]).free // (1024 * 1024)
            if free < a["min_free"]:
                stopped = "stopped before %s: %d MB free in %s, %d MB wanted (--min-free-mb)" % (
                    cell, free, a["work"], a["min_free"])
                break
            row = {"cell": cell}
            rows.append(row)
            do_cell(cell, a, opts, row)
    except KeyboardInterrupt:
        for p in list(running):
            kill_group(p)
        stopped = "interrupted"
        drop_dir(os.path.join(a["work"], "build"))
    lines = table(rows) + ([stopped] if stopped else [])
    say("\n" + "\n".join(lines))
    with open(os.path.join(a["work"], "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    if not a["keep_logs"]:
        drop_dir(os.path.join(a["work"], "logs"))
    if stopped:
        say("cells not run: " + ", ".join(c for c in cells if c not in [r["cell"] for r in rows]))
        return 3 if stopped != "interrupted" else 1
    return 1 if any(bad_row(r) for r in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
