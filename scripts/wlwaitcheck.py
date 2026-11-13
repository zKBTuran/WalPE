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
  closing   the compositor is still there when the wait begins and is on
            its way out: it accepts the connection and closes it (seen
            with cosmic-comp 1.11 for a moment after it was told to end).
            The daemon waits on as for a socket that is not there, for
            the whole of wayland_reconnect; it used to end at once
  silent    the compositor's socket is there and nobody answers on it yet:
            under kwin_wayland_wrapper the socket never goes away, the
            wrapper holds it while kwin_wayland is started again, and a
            connection made meanwhile is answered when the new one is up.
            The daemon waits for the answer as long as wayland_reconnect
            says; it used to give up after 10 seconds whatever that said
  signal    the waiting daemon is told to end (SIGTERM: the session closes,
            systemctl --user stop). It ends by that signal and the socket
            file is removed; the file used to stay, with nobody listening
  signal-replaced   the same after the file was replaced by another daemon:
            that one's file stays

The restarted image is made by hand: a listening socket at the control
socket's path, handed over the way a restart in place does it (WALPE_IPC_FD,
WALPE_DISPLAY_LOST), and no compositor. No display is needed.
Exit status 0 = passed, 1 = failed, 77 = cannot run here.

    python3 -I scripts/wlwaitcheck.py --build BUILD_DIR
"""
import argparse
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

WAIT_S = 4          # wayland_reconnect of the daemon under test
LIMIT_S = 60.0


def listener(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)
    s.listen(8)
    return s


def closing_compositor(path, stop):
    """a Wayland socket that takes every connection and closes it, until told to stop"""
    s = listener(path)
    s.settimeout(0.1)

    def serve():
        while not stop.is_set():
            try:
                s.accept()[0].close()
            except OSError:
                pass
        s.close()
    t = threading.Thread(target=serve, daemon=True)
    t.start()
    return t


def run_case(daemon, top, name, replace, absent=False, closing=False):
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
    stop = threading.Event()
    if closing:
        env["WAYLAND_DISPLAY"] = "wayland-closing"
        closing_compositor(os.path.join(rt, "wayland-closing"), stop)
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
            waited = time.monotonic() - t0
            stop.set()
            if p.poll() is None:
                p.kill()
                p.wait()
        log.seek(0)
        text = log.read().decode("utf-8", "replace")
    old.close()
    if closing and waited < WAIT_S - 0.5:
        print("FAIL %s: the daemon ended after %.1f s of a wait of %d s:\n%s" % (name, waited, WAIT_S, text[-1500:]))
        return 1
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
        print("ok   %s: the handed-over socket file is removed%s"
              % (name, " after the whole wait (%.1f s)" % waited if closing else ""))
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


def waiting_daemon(daemon, top, name, wait_s, display=None):
    """a daemon as run_case makes it, waiting -> (process, the old listener, socket path, log
    file name, the listener of `display` when one was asked for: a Wayland socket in the
    runtime directory that takes connections into its queue and never answers)"""
    d = os.path.join(top, name)
    rt = os.path.join(d, "rt")
    home = os.path.join(d, "home")
    os.makedirs(rt, 0o700)
    os.makedirs(home, 0o700)
    os.chmod(rt, 0o700)
    path = os.path.join(rt, "walpe.sock")
    if len(os.fsencode(path)) > 107:
        print("wlwaitcheck: the temporary directory's path is too long for a socket")
        return None
    old = listener(path)
    held = listener(os.path.join(rt, display)) if display else None
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_SOCKET", "WALPE_BACKEND")}
    env.update({
        "XDG_RUNTIME_DIR": rt,
        "WAYLAND_DISPLAY": display or "wayland-none",
        "WALPE_IPC_FD": str(old.fileno()),
        "WALPE_DISPLAY_LOST": "wayland",
        "WALPE_WAYLAND_RECONNECT": str(wait_s),
        "HOME": home,
        "XDG_CONFIG_HOME": os.path.join(home, "config"),
        "XDG_CACHE_HOME": os.path.join(home, "cache"),
        "XDG_DATA_HOME": os.path.join(home, "data"),
    })
    logname = os.path.join(d, "log")
    with open(logname, "wb") as log:
        p = subprocess.Popen([daemon], env=env, cwd=home, stdin=subprocess.DEVNULL,
                             stdout=log, stderr=subprocess.STDOUT, pass_fds=(old.fileno(),))
    t0 = time.monotonic()
    while time.monotonic() - t0 < LIMIT_S and p.poll() is None:
        with open(logname, "rb") as f:
            if b"display backend" in f.read():
                break
        time.sleep(0.05)
    return p, old, path, logname, held


def text_of(logname):
    with open(logname, "rb") as f:
        return f.read().decode("utf-8", "replace")


def end_of(p):
    if p.poll() is None:
        p.kill()
        p.wait()


def silent_case(daemon, top):
    """the socket is there, connections are taken into its queue and nobody answers"""
    name, wait_s = "silent", 13      # (longer than the 10 s the daemon gave an answer before)
    t0 = time.monotonic()
    got = waiting_daemon(daemon, top, name, wait_s, "wayland-held")
    if got is None:
        return 77
    p, old, path, logname, held = got
    try:
        rc = p.wait(timeout=LIMIT_S)
    except subprocess.TimeoutExpired:
        rc = None
    waited = time.monotonic() - t0
    end_of(p)
    held.close()
    old.close()
    text = text_of(logname)
    if rc != 1 or waited < wait_s - 0.5:
        print("FAIL %s: the daemon ended with status %s after %.1f s; wanted: status 1 after the whole "
              "wait of %d s:\n%s" % (name, rc, waited, wait_s, text[-1500:]))
        return 1
    if os.path.exists(path):
        print("FAIL %s: the socket file was left behind" % name)
        return 1
    print("ok   %s: the daemon waited the whole %d s for an answer (%.1f s), then ended; the socket file "
          "is removed" % (name, wait_s, waited))
    return 0


def signal_case(daemon, top, replaced):
    name = "signal-replaced" if replaced else "signal"
    got = waiting_daemon(daemon, top, name, 30)
    if got is None:
        return 77
    p, old, path, logname, _ = got
    new = None
    try:
        if p.poll() is not None:
            print("FAIL %s: the daemon ended before it waited:\n%s" % (name, text_of(logname)[-1500:]))
            return 1
        time.sleep(0.5)     # it waits now
        if replaced:
            os.unlink(path)
            new = listener(path)
        p.send_signal(signal.SIGTERM)
        try:
            rc = p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            print("FAIL %s: the daemon is still there 10 s after SIGTERM" % name)
            return 1
    finally:
        end_of(p)
        old.close()
    there = os.path.exists(path)
    if new:
        new.close()
    if rc != -signal.SIGTERM:
        print("FAIL %s: ended with status %d, wanted: by SIGTERM:\n%s" % (name, rc, text_of(logname)[-1500:]))
        return 1
    if replaced != there:
        print("FAIL %s: the socket file %s" % (name, "of the other daemon was removed" if replaced
                                               else "was left behind"))
        return 1
    print("ok   %s: SIGTERM ends the waiting daemon; %s" % (name, "the other daemon's socket is left in place"
                                                             if replaced else "the socket file is removed"))
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
        for name, replace, absent, closing in (("alone", False, False, False), ("replaced", True, False, False),
                                               ("absent", True, True, False), ("closing", False, False, True)):
            rc = run_case(daemon, top, name, replace, absent, closing)
            if rc == 77:
                return 77
            worst = max(worst, rc)
        for rc in (signal_case(daemon, top, False), signal_case(daemon, top, True), silent_case(daemon, top)):
            if rc == 77:
                return 77
            worst = max(worst, rc)
        return worst
    finally:
        shutil.rmtree(top, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
