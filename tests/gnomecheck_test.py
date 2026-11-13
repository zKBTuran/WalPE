#!/usr/bin/env python3
"""gnomecheck_test.py <scripts/gnomecheck.py>

What scripts/gnomecheck.py decides before it starts anything, without a GNOME Shell:
  - its case "socket-bound" in a build directory without gnome-caller-test: skipped
    with a reason where the build knows no such target (a build without the tests,
    the default of ./build.sh; the case FAILED there and the script ended with 1),
    failed where build.ninja has the target and the program is not there;
  - the directory its private one is made in: TMPDIR, /tmp where that is not set,
    is no directory or is too long for the sockets (it was /tmp whatever TMPDIR said).
"""
import importlib.util
import os
import sys
import tempfile

sys.dont_write_bytecode = True
script = os.path.abspath(sys.argv[1])
sys.path.insert(0, os.path.dirname(script))
spec = importlib.util.spec_from_file_location("gnomecheck", script)
gc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gc)

fails = []


def check(what, ok, got=None):
    if not ok:
        fails.append(what)
        print("FAIL: %s%s" % (what, "" if got is None else ": %r" % (got,)))


with tempfile.TemporaryDirectory(prefix="walpe-gct.") as top:
    # a build without the tests: no build.ninja at all, and one without the target
    plain = os.path.join(top, "plain")
    os.mkdir(plain)
    r = gc.case_socket_bound(None, plain)
    check("no build.ninja: socket-bound is skipped with a reason", isinstance(r, str) and "tests" in r, r)
    with open(os.path.join(plain, "build.ninja"), "w") as f:
        f.write("build walpe-daemon: c_LINKER walpe-daemon.p/src_daemon_main.c.o\n"
                "build all: phony walpe-daemon\n")
    r = gc.case_socket_bound(None, plain)
    check("a build without the tests: socket-bound is skipped with a reason", isinstance(r, str) and "tests" in r, r)
    # a build with the tests whose program is not there: that is a failure
    tests = os.path.join(top, "tests")
    os.mkdir(tests)
    with open(os.path.join(tests, "build.ninja"), "w") as f:
        f.write("build walpe-daemon: c_LINKER walpe-daemon.p/src_daemon_main.c.o\n"
                "build gnome-caller-test: c_LINKER gnome-caller-test.p/tests_gnome_caller.c.o\n")
    r = gc.case_socket_bound(None, tests)
    check("a build with the tests and no gnome-caller-test: socket-bound fails",
          isinstance(r, list) and len(r) == 1 and "is not built" in r[0], r)

    # the directory for the private one
    tmp_dir = getattr(gc, "tmp_dir", None)
    check("gnomecheck has tmp_dir()", callable(tmp_dir))
    if callable(tmp_dir):
        short = os.path.realpath(top)
        ok_len = len(os.fsencode(short)) <= gc.TMP_MAX
        r = tmp_dir({"TMPDIR": short})
        check("TMPDIR is taken", r == (short if ok_len else "/tmp"), r)
        check("no TMPDIR: /tmp", tmp_dir({}) == "/tmp", tmp_dir({}))
        r = tmp_dir({"TMPDIR": os.path.join(top, "not-there")})
        check("a TMPDIR that is no directory: /tmp", r == "/tmp", r)
        long = os.path.join(top, "l" * 60)
        os.mkdir(long)
        r = tmp_dir({"TMPDIR": long})
        check("a TMPDIR too long for the sockets: /tmp", r == "/tmp", r)

if fails:
    print("gnomecheck_test: %d failed" % len(fails))
    sys.exit(1)
print("gnomecheck_test: all passed")
