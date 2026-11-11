#!/usr/bin/env python3
"""walpe-daemon against a Wayland socket that accepts the connection and
never answers (a compositor that hangs, or one that is stopped).

The daemon must not wait for it without end. Asked for by name
(WALPE_BACKEND=wayland) it ends with status 1 and a sentence that says the
compositor did not answer; with the automatic choice and no X display it
ends with status 1 and "no display to draw on". Both within the backend's
start-up limit (10 s) plus a margin.

No display, no compositor and no GPU are needed: the daemon never gets as
far as drawing. Exit status 0 = passed, 1 = failed, 77 = cannot run here.

    python3 -I scripts/wlsilentcheck.py --build BUILD_DIR
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

LIMIT_S = 40.0      # 10 s in the daemon, the rest for a loaded machine


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    a = ap.parse_args()
    daemon = os.path.abspath(os.path.join(a.build, "walpe-daemon"))
    if not os.access(daemon, os.X_OK):
        print("wlsilentcheck: %s is not there" % daemon)
        return 77
    top = tempfile.mkdtemp(prefix="walpe-wlsilent-")
    procs = []
    try:
        rt = os.path.join(top, "rt")
        os.mkdir(rt, 0o700)
        path = os.path.join(rt, "wayland-silent")
        if len(os.fsencode(path)) > 107:
            print("wlsilentcheck: the temporary directory's path is too long for a socket")
            return 77
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(path)
        srv.listen(8)       # connections are accepted by the kernel; nobody reads them

        cases = [
            ("wayland by name", {"WALPE_BACKEND": "wayland"}, "did not answer"),
            ("automatic choice, no X display", {}, "does not answer"),
        ]
        for n, (label, extra, want) in enumerate(cases):
            home = os.path.join(top, "home%d" % n)
            os.mkdir(home, 0o700)
            env = {k: v for k, v in os.environ.items()
                   if k not in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "WALPE_BACKEND")}
            env.update({
                "XDG_RUNTIME_DIR": rt,
                "WAYLAND_DISPLAY": "wayland-silent",
                "HOME": home,
                "XDG_CONFIG_HOME": os.path.join(home, "config"),
                "XDG_CACHE_HOME": os.path.join(home, "cache"),
                "XDG_DATA_HOME": os.path.join(home, "data"),
            })
            env.update(extra)
            log = open(os.path.join(top, "log%d" % n), "w+b")
            p = subprocess.Popen([daemon], env=env, cwd=home, stdin=subprocess.DEVNULL,
                                 stdout=log, stderr=subprocess.STDOUT)
            procs.append((label, want, p, log, time.monotonic()))

        failed = 0
        for label, want, p, log, t0 in procs:
            left = max(0.1, LIMIT_S - (time.monotonic() - t0))
            try:
                rc = p.wait(timeout=left)
            except subprocess.TimeoutExpired:
                print("FAIL %s: the daemon is still waiting after %.0f s" % (label, LIMIT_S))
                failed += 1
                continue
            took = time.monotonic() - t0
            log.seek(0)
            text = log.read().decode("utf-8", "replace")
            if rc != 1 or want not in text:
                print("FAIL %s: exit status %d after %.1f s, wanted 1 and \"%s\" in:\n%s"
                      % (label, rc, took, want, text[-2000:]))
                failed += 1
            else:
                print("ok   %s: exit status 1 after %.1f s" % (label, took))
        return 1 if failed else 0
    finally:
        for _label, _want, p, log, _t0 in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
            log.close()
        shutil.rmtree(top, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
