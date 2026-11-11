#!/usr/bin/env python3
"""The control socket of a walpe-daemon that was restarted in place and waits
for its Wayland compositor (wayland_reconnect), when the wait ends without one.

Such an image holds the listening socket of the image before it and removes
the socket's file when its start fails. While it waits it does not hold the
single-instance lock, so another walpe-daemon can be started and puts its
own socket in the same place. Two cases:

  alone     nobody took the place: the file is removed (nothing stale stays)
  replaced  the file was replaced meanwhile, as a second daemon does it
            (unlink, bind, listen): it must still be there afterwards and
            still reach its new listener
  absent    the file was gone already when the image began (the runtime
            directory was cleared) and another daemon made its own later:
            the same

The restarted image is made by hand: a listening socket at the control
socket's path, handed over the way a restart in place does it (WALPE_IPC_FD,
WALPE_DISPLAY_LOST), and no compositor. No display is needed.
Exit status 0 = passed, 1 = failed, 77 = cannot run here.

    python3 -I scripts/wlwaitcheck.py --build BUILD_DIR
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

WAIT_S = 4          # wayland_reconnect of the daemon under test
LIMIT_S = 60.0


def listener(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)
    s.listen(8)
    return s


def run_case(daemon, top, name, replace, absent=False):
    d = os.path.join(top, name)
    rt = os.path.join(d, "rt")
    home = os.path.join(d, "home")
    os.makedirs(rt, 0o700)
    os.makedirs(home, 0o700)
    os.chmod(rt, 0o700)
    path = os.path.join(rt, "walpe.sock")
    if len(os.fsencode(path)) > 107:
        print("wlwaitcheck: the temporary directory's path is too long for a socket")
        return 77
    old = listener(path)
    if absent:
        os.unlink(path)
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_SOCKET", "WALPE_BACKEND")}
    env.update({
        "XDG_RUNTIME_DIR": rt,
        "WAYLAND_DISPLAY": "wayland-none",      # no such socket
        "WALPE_IPC_FD": str(old.fileno()),
        "WALPE_DISPLAY_LOST": "wayland",
        "WALPE_WAYLAND_RECONNECT": str(WAIT_S),
        "HOME": home,
        "XDG_CONFIG_HOME": os.path.join(home, "config"),
        "XDG_CACHE_HOME": os.path.join(home, "cache"),
        "XDG_DATA_HOME": os.path.join(home, "data"),
    })
    logname = os.path.join(d, "log")
    new = None
    with open(logname, "w+b") as log:
        p = subprocess.Popen([daemon], env=env, cwd=home, stdin=subprocess.DEVNULL,
                             stdout=log, stderr=subprocess.STDOUT, pass_fds=(old.fileno(),))
        try:
            t0 = time.monotonic()
            if replace:
                # once the daemon has chosen its backend it is about to wait
                while time.monotonic() - t0 < LIMIT_S and p.poll() is None:
                    with open(logname, "rb") as f:
                        if b"display backend" in f.read():
                            break
                    time.sleep(0.05)
                if p.poll() is not None:
                    print("FAIL %s: the daemon ended before it waited" % name)
                    return 1
                if not absent:
                    os.unlink(path)
                new = listener(path)
            try:
                rc = p.wait(timeout=LIMIT_S)
            except subprocess.TimeoutExpired:
                print("FAIL %s: the daemon is still there after %.0f s" % (name, LIMIT_S))
                return 1
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
        log.seek(0)
        text = log.read().decode("utf-8", "replace")
    old.close()
    if rc != 1:
        print("FAIL %s: exit status %d, wanted 1:\n%s" % (name, rc, text[-1500:]))
        return 1
    # the log says why this backend: the restart, not an option nobody typed
    line = next((l for l in text.splitlines() if "display backend:" in l), "")
    if "--backend" in line or "display went away" not in line:
        print("FAIL %s: the reason given for the backend: %s" % (name, line))
        return 1
    there = os.path.exists(path)
    if not replace:
        if there:
            print("FAIL %s: the socket file was left behind" % name)
            return 1
        print("ok   %s: the handed-over socket file is removed" % name)
        return 0
    reached = False
    if there:
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            c.connect(path)
            new.settimeout(5)
            new.accept()[0].close()
            reached = True
        except OSError:
            pass
        c.close()
    new.close()
    if not reached:
        print("FAIL %s: the other daemon's socket file %s:\n%s"
              % (name, "does not reach it" if there else "was removed", text[-1500:]))
        return 1
    print("ok   %s: the other daemon's socket is left in place" % name)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    a = ap.parse_args()
    daemon = os.path.abspath(os.path.join(a.build, "walpe-daemon"))
    if not os.access(daemon, os.X_OK):
        print("wlwaitcheck: %s is not there" % daemon)
        return 77
    top = tempfile.mkdtemp(prefix="walpe-wlwait-")
    try:
        worst = 0
        for name, replace, absent in (("alone", False, False), ("replaced", True, False),
                                      ("absent", True, True)):
            rc = run_case(daemon, top, name, replace, absent)
            if rc == 77:
                return 77
            worst = max(worst, rc)
        return worst
    finally:
        shutil.rmtree(top, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
