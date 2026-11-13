#!/usr/bin/env python3
"""walpe-daemon and the WalPE extension for GNOME Shell (contrib/gnome) on a
private headless GNOME Shell.

The extension is JavaScript that runs inside GNOME Shell, so what it does
can only be checked there. This starts a shell of its own: `gnome-shell
--headless` on Mesa's software renderer with one virtual monitor, a private
HOME and runtime directory, a private session bus and an empty stand-in for
the system bus (the shell wants one to exist; nothing owns a name on it).
No gnome-session and no login manager. GNOME Shell has a lock screen only
where a display manager answers on the system bus: with gjs installed a
stand-in of a dozen lines does that (it has a version and nothing else),
without gjs the case "lock" is skipped. The daemon shows a scene of one colour (scripts/checkenv.py);
without a wallpaper it shows no window at all. The extension is copied from the source tree into that HOME
(with the shell's version added to the list in its metadata.json, so that
the check runs on a version the extension does not list yet) and the shell
is asked about its windows through org.gnome.Shell.Eval, which it offers
with --unsafe-mode.

Cases (--only NAME runs one):

  desktop        the daemon takes the Wayland backend, its window gets the
                 type DESKTOP at the monitor's place and size, and is gone
                 with the daemon
  helper-killed  GNOME Shell 46 to 48 (the shell starts a second walpe-daemon
                 process that hands its connection over): that process ends
                 while the daemon runs, the shell collects garbage: the
                 daemon and its window stay
  pkill          the same versions: SIGTERM to both processes at once (what
                 `pkill walpe-daemon` does), the shell collects garbage while
                 the daemon is still shutting down: no picture of the window
                 stays in the shell and the extension keeps no window
  socket-bound   the same versions: a program that is not walpe-daemon
                 (tests/gnome_caller.c) asks for a dozen connections and
                 keeps them: it gets eight, the rest is refused with a
                 sentence, and when it has ended a daemon gets its own
                 (skipped in a build without the tests: the program is
                 one of them)
  stranger       a window of another program (a GTK 4 window made with gjs)
                 that uses the application id and the title of a wallpaper
                 window stays an ordinary window; skipped without gjs
  covered        what the extension reports about other programs' windows
                 (method Windows): a small window of an application has the
                 focus and does not cover the monitor, a maximized one
                 covers it, and its application id in the "see-through
                 windows" list takes the covering away; skipped without gjs
  lock           the screen is locked and unlocked: the extension stays
                 enabled, reports the monitor as covered while it is locked
                 (the daemon pauses) and as free again afterwards (it runs),
                 and the window is a desktop window throughout; skipped
                 without gjs
  crash          the daemon is killed (SIGKILL): its window, the process
                 the shell started for it and what the extension kept of
                 both are gone
  bus-bounds     a "see-through windows" list of 100 000 characters sent to
                 the extension's method Windows: it keeps 64 names at most
                 and answers

Exit status 0 = passed, 1 = failed, 77 = cannot run here (no gnome-shell,
gdbus or dbus-daemon, a build without the Wayland backend, or a shell that
does not come up headless); WALPE_CHECK_STRICT=1 makes that a failure.

    python3 -I scripts/gnomecheck.py --build BUILD_DIR [--only NAME] [--keep] [--tmp DIR]

The private directory (home, sockets, the logs a failure leaves) is made in
--tmp, else in TMPDIR, else in /tmp: walpe-gn.XXXXXXXX.

Another version of GNOME Shell than the installed one, from a chroot or a
container: WALPE_CHECK_GNOME_SHELL is the command that runs its gnome-shell
("chroot /srv/arch gnome-shell"), and --tmp a directory that is the same
path inside and outside (a bind mount); the daemon, the buses and the
questions stay outside.
"""
import argparse
import ast
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True   # no __pycache__ next to the scripts
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checkenv   # noqa: E402  (the one-colour scene)

UUID = "walpe@walpe.github.io"
BUS_NAME = "io.github.walpe.Gnome"
TITLE = "WalPE wallpaper: "
SKIP = 1 if os.environ.get("WALPE_CHECK_STRICT", "0") not in ("", "0") else 77
SHELL = shlex.split(os.environ.get("WALPE_CHECK_GNOME_SHELL", "") or "gnome-shell")
START_S = 90.0      # for the shell to come up (software renderer, loaded machine)
WAIT_S = 30.0       # for anything else

SYSBUS = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=%s</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""

# the stand-in display manager (gjs -m): GNOME Shell makes a lock screen only
# when org.gnome.DisplayManager answers with a version
GDM_JS = """import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
const xml = `<node><interface name="org.gnome.DisplayManager.Manager">
  <property name="Version" type="s" access="read"/>
  <method name="RegisterDisplay"><arg type="a{ss}" direction="in"/></method>
  <method name="RegisterSession"><arg type="a{sv}" direction="in"/></method>
</interface></node>`;
const obj = Gio.DBusExportedObject.wrapJSObject(xml, {Version: '46.0', RegisterDisplay() {}, RegisterSession() {}});
obj.export(Gio.DBus.system, '/org/gnome/DisplayManager/Manager');
Gio.bus_own_name_on_connection(Gio.DBus.system, 'org.gnome.DisplayManager', Gio.BusNameOwnerFlags.NONE, null, null);
new GLib.MainLoop(null, false).run();
"""

# what the shell is asked: every window, lowest first, and the pictures of
# windows (window actors) in the shell's window group, whether or not a
# window still belongs to them
JS_WINDOWS = """JSON.stringify({
  windows: global.display.sort_windows_by_stacking(global.display.list_all_windows()).map(w => {
    const r = w.get_frame_rect();
    return {title: w.get_title(), app: w.get_wm_class(), type: w.get_window_type(),
            rect: [r.x, r.y, r.width, r.height], layer: w.get_layer(), focus: w.has_focus(),
            skip: w.is_skip_taskbar(), sticky: w.is_on_all_workspaces()};
  }),
  actors: global.window_group.get_children().filter(a => a.meta_window).map(a => a.meta_window.get_title()),
  monitor: (() => { const g = global.display.get_monitor_geometry(0); return [g.x, g.y, g.width, g.height]; })(),
})"""
JS_EXT = """(() => { const x = Main.extensionManager.lookup('%s'); const o = x?.stateObj;
  return JSON.stringify({state: x ? x.state : -1, error: x?.error ?? '',
                         wallpapers: o?._wallpapers ? o._wallpapers.size : -1,
                         backgrounds: o?._backgrounds ? o._backgrounds.size : -1,
                         look: o?._lookId ?? 0}); })()""" % UUID
JS_GC = "imports.system.gc(); 1"
DESKTOP = 1         # Meta.WindowType.DESKTOP


class CannotRun(Exception):
    pass


class Shell:
    def __init__(self, top, source):
        self.top = top
        self.source = source
        self.home = os.path.join(top, "home")
        self.rt = os.path.join(top, "rt")
        self.procs = []
        self.version = ""
        self.env = None

    def _spawn(self, name, argv, env):
        log = open(os.path.join(self.top, name + ".log"), "wb")
        p = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                             start_new_session=True)
        log.close()
        self.procs.append(p)
        return p

    def log_tail(self, name, lines=15):
        try:
            with open(os.path.join(self.top, name + ".log"), "rb") as f:
                return b"\n".join(f.read().splitlines()[-lines:]).decode("utf-8", "replace")
        except OSError:
            return ""

    def start(self):
        out = subprocess.run(SHELL + ["--version"], capture_output=True, text=True).stdout
        m = re.search(r"GNOME Shell (\d+)", out)
        if not m:
            raise CannotRun("gnome-shell --version gave no version")
        self.version = m.group(1)
        os.makedirs(self.rt, 0o700)
        os.chmod(self.rt, 0o700)
        ext = os.path.join(self.home, ".local", "share", "gnome-shell", "extensions", UUID)
        os.makedirs(ext)
        src = os.path.join(self.source, "contrib", "gnome", UUID)
        shutil.copy(os.path.join(src, "extension.js"), ext)
        with open(os.path.join(src, "metadata.json")) as f:
            meta = json.load(f)
        if self.version not in meta["shell-version"]:
            meta["shell-version"].append(self.version)
        with open(os.path.join(ext, "metadata.json"), "w") as f:
            json.dump(meta, f)
        # the setting the shell reads its enabled extensions from, without dconf
        keyfile = os.path.join(self.home, ".config", "glib-2.0", "settings")
        os.makedirs(keyfile)
        with open(os.path.join(keyfile, "keyfile"), "w") as f:
            f.write("[org/gnome/shell]\nenabled-extensions=['%s']\n" % UUID)
        env = {k: v for k, v in os.environ.items()
               if k not in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DBUS_SESSION_BUS_ADDRESS",
                            "WALPE_BACKEND", "WALPE_GNOME_EXT", "XDG_DATA_DIRS")}
        env.update({
            "HOME": self.home, "XDG_RUNTIME_DIR": self.rt,
            "XDG_CONFIG_HOME": os.path.join(self.home, ".config"),
            "XDG_DATA_HOME": os.path.join(self.home, ".local", "share"),
            "XDG_CACHE_HOME": os.path.join(self.home, ".cache"),
            "XDG_STATE_HOME": os.path.join(self.home, ".local", "state"),
            "XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": "GNOME", "XDG_SESSION_DESKTOP": "gnome",
            "LIBGL_ALWAYS_SOFTWARE": "1", "GSETTINGS_BACKEND": "keyfile", "WALPE_SOFTWARE": "1",
            "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=" + os.path.join(self.rt, "sysbus"),
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=" + os.path.join(self.rt, "bus"),
        })
        conf = os.path.join(self.top, "sysbus.conf")
        with open(conf, "w") as f:
            f.write(SYSBUS % os.path.join(self.rt, "sysbus"))
        self._spawn("sysbus", ["dbus-daemon", "--config-file=" + conf, "--nofork", "--nopidfile"], env)
        self._spawn("bus", ["dbus-daemon", "--session", "--address=" + env["DBUS_SESSION_BUS_ADDRESS"],
                            "--nofork", "--nopidfile"], env)
        end = time.monotonic() + WAIT_S
        while not (os.path.exists(os.path.join(self.rt, "bus")) and os.path.exists(os.path.join(self.rt, "sysbus"))):
            if time.monotonic() > end:
                raise CannotRun("dbus-daemon made no socket")
            time.sleep(0.05)
        if shutil.which("gjs"):
            gdm = os.path.join(self.top, "gdm.js")
            with open(gdm, "w") as f:
                f.write(GDM_JS)
            self._spawn("gdm", ["gjs", "-m", gdm], env)
            end = time.monotonic() + WAIT_S
            while time.monotonic() < end:
                r = subprocess.run(["gdbus", "call", "--address", env["DBUS_SYSTEM_BUS_ADDRESS"], "--dest",
                                    "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus", "--method",
                                    "org.freedesktop.DBus.NameHasOwner", "org.gnome.DisplayManager"],
                                   env=env, capture_output=True, text=True)
                if "true" in r.stdout:
                    break
                time.sleep(0.1)
        shell = self._spawn("shell", SHELL + ["--headless", "--wayland", "--no-x11", "--unsafe-mode",
                                      "--virtual-monitor", "800x600", "--wayland-display", "walpe-check"], env)
        self.env = dict(env, WAYLAND_DISPLAY="walpe-check")
        end = time.monotonic() + START_S
        while True:
            if shell.poll() is not None:
                raise CannotRun("gnome-shell %s ended while starting headless:\n%s"
                                % (self.version, self.log_tail("shell")))
            try:
                if self.eval("Main.layoutManager._startingUp", timeout=5) is False:
                    break
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
            if time.monotonic() > end:
                raise CannotRun("gnome-shell %s did not come up headless in %d s:\n%s"
                                % (self.version, START_S, self.log_tail("shell")))
            time.sleep(0.2)
        self.eval("Main.overview.hide(); 1")
        self.shell = shell

    def eval(self, js, timeout=WAIT_S):
        r = subprocess.run(["gdbus", "call", "--session", "--dest", "org.gnome.Shell", "--object-path",
                            "/org/gnome/Shell", "--method", "org.gnome.Shell.Eval", " ".join(js.split())],
                           env=self.env, capture_output=True, text=True, timeout=timeout)
        m = re.match(r"\((true|false), (.*)\)\s*$", r.stdout, re.S)
        if r.returncode != 0 or not m:
            raise RuntimeError("org.gnome.Shell.Eval: %s" % (r.stderr.strip() or r.stdout.strip()))
        text = ast.literal_eval(m.group(2))
        if m.group(1) != "true":
            raise RuntimeError("org.gnome.Shell.Eval: %s" % text)
        v = json.loads(text) if text else None
        return json.loads(v) if isinstance(v, str) and v[:1] in "{[" else v

    def windows(self):
        return self.eval(JS_WINDOWS)

    def extension(self):
        return self.eval(JS_EXT)

    def gc(self):
        self.eval(JS_GC)

    def hello(self):
        r = subprocess.run(["gdbus", "call", "--session", "--dest", BUS_NAME, "--object-path",
                            "/io/github/walpe/Gnome", "--method", BUS_NAME + ".Hello"],
                           env=self.env, capture_output=True, text=True, timeout=WAIT_S)
        return r.stdout if r.returncode == 0 else ""

    def stop(self):
        for p in reversed(self.procs):
            if p.poll() is None:
                p.terminate()
        end = time.monotonic() + 10
        for p in self.procs:
            try:
                p.wait(max(0.1, end - time.monotonic()))
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()


class Daemon:
    """One walpe-daemon in the shell's session, showing a scene of one colour."""

    def __init__(self, shell, build, name):
        self.shell = shell
        self.exe = os.path.join(build, "walpe-daemon")
        self.ctl = os.path.join(build, "walpe-ctl")
        self.logname = os.path.join(shell.top, name + ".log")
        self.p = None

    def start(self):
        log = open(self.logname, "wb")
        self.p = subprocess.Popen([self.exe], env=self.shell.env, cwd=self.shell.home, stdin=subprocess.DEVNULL,
                                  stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        log.close()
        scene = os.path.join(self.shell.top, "scene")
        if not os.path.isdir(scene):
            checkenv.gen_scene(scene, (0.1, 0.6, 0.3))
        end = time.monotonic() + WAIT_S
        while self.p.poll() is None and time.monotonic() < end:
            r = subprocess.run([self.ctl, "open", scene], env=self.shell.env, capture_output=True, text=True)
            if '"ok":true' in r.stdout:
                break
            time.sleep(0.25)

    def log(self):
        with open(self.logname, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def helper(self):
        """the process the shell started for this daemon's connection (GNOME Shell 46 to 48), or None"""
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open("/proc/%s/cmdline" % pid, "rb") as f:
                    argv = f.read().split(b"\0")
                if len(argv) >= 3 and argv[1] == b"--wayland-socket-to" and \
                        os.stat("/proc/%s" % pid).st_uid == os.getuid() and \
                        os.path.realpath("/proc/%s/exe" % pid) == os.path.realpath(self.exe) and \
                        ("XDG_RUNTIME_DIR=" + self.shell.rt).encode() in \
                        open("/proc/%s/environ" % pid, "rb").read().split(b"\0"):
                    return int(pid)
            except OSError:
                continue
        return None

    def wait_window(self):
        """the daemon's window as the shell sees it, once it is a desktop window on the monitor"""
        end = time.monotonic() + WAIT_S
        last = None
        while time.monotonic() < end:
            if self.p.poll() is not None:
                return None, "the daemon ended with status %d:\n%s" % (self.p.returncode, self.log()[-1500:])
            w = self.shell.windows()
            mine = [x for x in w["windows"] if (x["title"] or "").startswith(TITLE)]
            if mine:
                last = mine[0]
                if last["type"] == DESKTOP and last["rect"] == w["monitor"]:
                    return last, None
            time.sleep(0.25)
        return None, "no desktop window of the daemon on the monitor after %d s (last seen: %s)" % (WAIT_S, last)

    def wait_gone(self, limit=WAIT_S):
        try:
            self.p.wait(limit)
            return True
        except subprocess.TimeoutExpired:
            return False

    def stop(self):
        if self.p and self.p.poll() is None:
            self.p.terminate()
            if not self.wait_gone(10):
                self.p.kill()
                self.p.wait()


def leftovers(shell):
    """what must not be there once no daemon runs; [] = nothing"""
    bad = []
    end = time.monotonic() + WAIT_S
    while True:
        w = shell.windows()
        x = shell.extension()
        bad = []
        if any((t or "").startswith(TITLE) for t in w["actors"]):
            bad.append("the picture of a wallpaper window is still in the shell's window group: %s" % w["actors"])
        if any((t["title"] or "").startswith(TITLE) for t in w["windows"]):
            bad.append("a wallpaper window is still there")
        if x["wallpapers"] != 0:
            bad.append("the extension still keeps %d wallpaper window(s)" % x["wallpapers"])
        if not bad or time.monotonic() > end:
            return bad
        time.sleep(0.25)


def case_desktop(shell, build):
    d = Daemon(shell, build, "daemon-desktop")
    d.start()
    try:
        w, why = d.wait_window()
        if not w:
            return [why]
        bad = []
        if w["app"] != "io.github.walpe.wallpaper":
            bad.append("application id %r" % w["app"])
        if w["layer"] != 0 or not w["skip"] or not w["sticky"] or w["focus"]:
            bad.append("not a desktop window in every respect: %s" % w)
        if shell.windows()["windows"][0]["title"] != w["title"]:
            bad.append("not the lowest window")
        r = subprocess.run([d.ctl, "quit"], env=shell.env, capture_output=True, text=True, timeout=WAIT_S)
        if not d.wait_gone():
            bad.append("the daemon did not end after walpe-ctl quit (%s)" % r.stdout.strip())
        return bad + leftovers(shell)
    finally:
        d.stop()


def case_helper_killed(shell, build):
    d = Daemon(shell, build, "daemon-helper-killed")
    d.start()
    try:
        w, why = d.wait_window()
        if not w:
            return [why]
        helper = d.helper()
        if not helper:
            return ["no process `walpe-daemon --wayland-socket-to` of this daemon, on a shell that says it starts one"]
        os.kill(helper, signal.SIGTERM)
        end = time.monotonic() + WAIT_S
        while os.path.exists("/proc/%d" % helper) and time.monotonic() < end:
            time.sleep(0.05)
        bad = []
        for _ in range(3):
            time.sleep(0.5)
            shell.gc()
        if d.p.poll() is not None:
            bad.append("the daemon ended when the process the shell had started for it was gone and the "
                       "shell collected garbage:\n%s" % d.log()[-600:])
        elif not any((x["title"] or "").startswith(TITLE) and x["type"] == DESKTOP
                     for x in shell.windows()["windows"]):
            bad.append("the daemon's desktop window is gone")
        d.stop()
        return bad + leftovers(shell)
    finally:
        d.stop()


def case_pkill(shell, build):
    d = Daemon(shell, build, "daemon-pkill")
    d.start()
    try:
        w, why = d.wait_window()
        if not w:
            return [why]
        helper = d.helper()
        if not helper:
            return ["no process `walpe-daemon --wayland-socket-to` of this daemon, on a shell that says it starts one"]
        os.kill(helper, signal.SIGTERM)
        os.kill(d.p.pid, signal.SIGTERM)
        # (the daemon needs a moment to shut down; the collector runs meanwhile)
        while d.p.poll() is None:
            shell.gc()
        return leftovers(shell)
    finally:
        d.stop()


def caller_missing(build):
    """The case "socket-bound" needs gnome-caller-test, which only a build with the tests
    (-Dtests=true) has, and only once it was built. A build that knows no such target - the
    default one of ./build.sh - skips the case: a string, the reason. A build that knows the
    target and has no program fails it (a list): there the program should be."""
    target = False
    try:
        with open(os.path.join(build, "build.ninja"), "rb") as f:
            target = any(line.startswith(b"build gnome-caller-test:") for line in f)
    except OSError:
        pass
    if target:
        return ["%s is not built (ninja -C %s gnome-caller-test)" % (os.path.join(build, "gnome-caller-test"), build)]
    return ("this build is without the tests, and gnome-caller-test is one of them "
            "(meson setup -Dtests=true, then ninja gnome-caller-test)")


def case_socket_bound(shell, build):
    caller = os.path.join(build, "gnome-caller-test")
    if not os.access(caller, os.X_OK):
        return caller_missing(build)
    p = subprocess.Popen([caller, "12"], env=shell.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    lines = []
    try:
        for line in p.stdout:
            lines.append(line.strip())
            if line.strip() == "done":
                break
    finally:
        p.stdin.close()
        p.wait(WAIT_S)
    bad = []
    handed = lines.count("handed")
    refused = [x for x in lines if x.startswith("refused")]
    if handed != 8 or len(refused) != 4:
        bad.append("a program asked for 12 connections and kept them: %d handed, %d refused (8 and 4 "
                   "are right): %s" % (handed, len(refused), lines))
    elif not all("handed out already" in x for x in refused):
        bad.append("refused without the sentence: %s" % refused)
    # the eight are let go when their program is gone: a daemon gets its connection
    d = Daemon(shell, build, "daemon-socket-bound")
    end = time.monotonic() + WAIT_S
    try:
        while True:
            d.start()
            w, why = d.wait_window()
            if w or time.monotonic() > end:
                break
            d.stop()        # (the shell had not seen the eight connections close yet)
            time.sleep(0.5)
        if not w:
            bad.append("after the program had ended a daemon got no desktop window: %s" % why)
        d.stop()
        return bad + leftovers(shell)
    finally:
        d.stop()


WINDOW_JS = """import Gtk from 'gi://Gtk?version=4.0';
import Gio from 'gi://Gio';
const app = new Gtk.Application({application_id: '%s', flags: Gio.ApplicationFlags.NON_UNIQUE});
app.connect('activate', () => {
    const w = new Gtk.ApplicationWindow({application: app, title: '%s', default_width: 300, default_height: 200});
    if (%s)
        w.maximize();
    w.present();
});
app.run([]);
"""
CHECK_APP = "io.github.walpe.check"


def gtk_window(shell, name, app_id, title, maximized=False):
    """a GTK 4 window of a program of its own (gjs); the process"""
    js = os.path.join(shell.top, name + ".js")
    with open(js, "w") as f:
        f.write(WINDOW_JS % (app_id, title, "true" if maximized else "false"))
    log = open(os.path.join(shell.top, name + ".log"), "wb")
    p = subprocess.Popen(["gjs", "-m", js], env=dict(shell.env, GTK_A11Y="none"), stdin=subprocess.DEVNULL,
                         stdout=log, stderr=subprocess.STDOUT)
    log.close()
    return p


def end_window(shell, p, title):
    p.terminate()
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
    end = time.monotonic() + WAIT_S
    while time.monotonic() < end and any((x["title"] or "").startswith(title) for x in shell.windows()["windows"]):
        time.sleep(0.25)


def report(shell, see_through=""):
    """the flags of the one monitor in the answer of Windows, or None"""
    r = subprocess.run(["gdbus", "call", "--session", "--dest", BUS_NAME, "--object-path",
                        "/io/github/walpe/Gnome", "--method", BUS_NAME + ".Windows", see_through],
                       env=shell.env, capture_output=True, text=True, timeout=WAIT_S)
    m = re.match(r"\('[^'\\]+\\t([caf]*)',\)", r.stdout.strip())
    return m.group(1) if r.returncode == 0 and m else None


def wait_report(shell, want, see_through=""):
    end = time.monotonic() + WAIT_S
    while True:
        got = report(shell, see_through)
        if got == want or time.monotonic() > end:
            return got
        time.sleep(0.25)


def case_covered(shell, build):
    if not shutil.which("gjs"):
        return None
    bad = []
    got = wait_report(shell, "")
    if got != "":
        return ["with no window the monitor's flags are %r, not none" % got]
    p = gtk_window(shell, "small", CHECK_APP, "walpe-check small")
    try:
        got = wait_report(shell, "a")
        if got == "" and p.poll() is not None:
            return None     # (no GTK 4 for gjs: nothing was shown)
        if got != "a":
            bad.append("a small window of an application with the focus: flags %r, not 'a'" % got)
    finally:
        end_window(shell, p, "walpe-check")
    p = gtk_window(shell, "big", CHECK_APP, "walpe-check maximized", maximized=True)
    try:
        got = wait_report(shell, "ca")
        if got != "ca":
            bad.append("a maximized window of an application with the focus: flags %r, not 'ca'" % got)
        got = wait_report(shell, "a", " Other.App , %s" % CHECK_APP.upper())
        if got != "a":
            bad.append("the same with its application id in the see-through list: flags %r, not 'a'" % got)
    finally:
        end_window(shell, p, "walpe-check")
        report(shell, "")
    return bad


def case_lock(shell, build):
    if shell.eval("Main.screenShield != null") is not True:
        return None         # (no stand-in display manager: no gjs)
    d = Daemon(shell, build, "daemon-lock")
    d.start()

    def paused():
        r = subprocess.run([d.ctl, "status"], env=shell.env, capture_output=True, text=True, timeout=WAIT_S)
        try:
            return json.loads(r.stdout)["state"]["monitors"][0]["paused"]
        except (ValueError, KeyError, IndexError):
            return None

    def wait(fn, want):
        end = time.monotonic() + WAIT_S
        while True:
            got = fn()
            if got == want or time.monotonic() > end:
                return got
            time.sleep(0.25)

    try:
        w, why = d.wait_window()
        if not w:
            return [why]
        bad = []
        if wait(paused, False) is not False:
            bad.append("the wallpaper is paused before the screen is locked")
        shell.eval("Main.screenShield.lock(false); 1")
        mode = wait(lambda: shell.eval("Main.sessionMode.currentMode"), "unlock-dialog")
        if mode != "unlock-dialog":
            return bad + ["the screen did not lock (session mode %r)" % mode]
        x = shell.extension()
        if x["state"] != 1:
            bad.append("the extension is not enabled while the screen is locked (state %s)" % x["state"])
        got = wait_report(shell, "c", "")
        if got != "c":
            bad.append("locked: the monitor's flags are %r, not 'c'" % got)
        if wait(paused, True) is not True:
            bad.append("locked: the daemon does not pause the wallpaper")
        shell.eval("Main.screenShield.deactivate(false); 1")
        mode = wait(lambda: shell.eval("Main.sessionMode.currentMode"), "user")
        if mode != "user":
            bad.append("the screen did not unlock (session mode %r)" % mode)
        got = wait_report(shell, "", "")
        if got != "":
            bad.append("unlocked: the monitor's flags are %r, not none" % got)
        if wait(paused, False) is not False:
            bad.append("unlocked: the wallpaper stays paused")
        if not any((t["title"] or "").startswith(TITLE) and t["type"] == DESKTOP
                   for t in shell.windows()["windows"]):
            bad.append("after the lock the daemon's window is no desktop window")
        d.stop()
        return bad + leftovers(shell)
    finally:
        d.stop()


def case_crash(shell, build):
    d = Daemon(shell, build, "daemon-crash")
    d.start()
    try:
        w, why = d.wait_window()
        if not w:
            return [why]
        helper = d.helper()
        d.p.kill()
        d.p.wait()
        bad = leftovers(shell)
        end = time.monotonic() + WAIT_S
        while helper and os.path.exists("/proc/%d" % helper) and time.monotonic() < end:
            time.sleep(0.1)
        if helper and os.path.exists("/proc/%d" % helper):
            bad.append("the process the shell started for the daemon's connection (%d) outlives the daemon" % helper)
        shell.gc()
        return bad + leftovers(shell)
    finally:
        d.stop()


def case_stranger(shell, build):
    if not shutil.which("gjs"):
        return None
    p = gtk_window(shell, "stranger", "io.github.walpe.wallpaper", TITLE + "Meta-0")
    try:
        end = time.monotonic() + WAIT_S
        seen = None
        while time.monotonic() < end and p.poll() is None:
            mine = [x for x in shell.windows()["windows"] if (x["title"] or "").startswith(TITLE)]
            if mine and seen is None:
                seen = time.monotonic()
            if mine and mine[0]["type"] == DESKTOP:
                return ["a window of another program with the wallpaper's application id and title was "
                        "made a desktop window: %s" % mine[0]]
            if seen is not None and time.monotonic() - seen > 3:
                return []       # (it has been an ordinary window for three seconds)
            time.sleep(0.25)
        if seen is None:
            # (no GTK 4 for gjs, or it cannot reach the compositor: nothing was shown)
            return None
        return []
    finally:
        end_window(shell, p, TITLE)


def case_bus_bounds(shell, build):
    names = ",".join("name%05d" % i for i in range(10000))        # 100 000 characters
    r = subprocess.run(["gdbus", "call", "--session", "--dest", BUS_NAME, "--object-path",
                        "/io/github/walpe/Gnome", "--method", BUS_NAME + ".Windows", names],
                       env=shell.env, capture_output=True, text=True, timeout=WAIT_S)
    bad = []
    if r.returncode != 0 or "monitor-0" not in r.stdout:
        bad.append("Windows with a long list did not answer with the monitor's line: %s %s"
                   % (r.stdout.strip(), r.stderr.strip()))
    kept = shell.eval("Main.extensionManager.lookup('%s').stateObj._seeThrough.length" % UUID)
    if not isinstance(kept, int) or kept < 1 or kept > 64:
        bad.append("the extension keeps %s names of a list of 10000 (64 at most)" % kept)
    shell.eval("Main.extensionManager.lookup('%s').stateObj.Windows(''); 1" % UUID)
    return bad


CASES = [("desktop", case_desktop, False), ("helper-killed", case_helper_killed, True),
         ("pkill", case_pkill, True), ("socket-bound", case_socket_bound, True),
         ("stranger", case_stranger, False), ("covered", case_covered, False),
         ("lock", case_lock, False), ("crash", case_crash, False),
         ("bus-bounds", case_bus_bounds, False)]


TMP_MAX = 45    # bytes of a directory for the private one: below it are "walpe-gn.XXXXXXXX/rt/" and the
                # sockets of the buses, the shell and the daemon, and a socket's path has 107


def tmp_dir(environ=None):
    """Where the private directory is made when --tmp does not say: TMPDIR (the logs went to
    /tmp whatever it said), /tmp where it is not set. A TMPDIR that is no directory, or one
    whose path leaves no room for the sockets, is said in a line and /tmp is taken."""
    t = (os.environ if environ is None else environ).get("TMPDIR", "")
    if not t:
        return "/tmp"
    t = os.path.abspath(t)
    if not os.path.isdir(t):
        print("gnomecheck: TMPDIR (%s) is not a directory: /tmp is used" % t)
        return "/tmp"
    if len(os.fsencode(t)) > TMP_MAX:
        print("gnomecheck: TMPDIR (%s) is too long for the sockets of the session (%d bytes at most): "
              "/tmp is used, or --tmp" % (t, TMP_MAX))
        return "/tmp"
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--only")
    ap.add_argument("--keep", action="store_true", help="keep the directory with the logs")
    ap.add_argument("--tmp", help="where the private directory is made (a short path); "
                                  "without it TMPDIR, and /tmp where that is not set")
    a = ap.parse_args()
    build = os.path.abspath(a.build)
    source = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for prog in (SHELL[0], "gdbus", "dbus-daemon"):
        if not shutil.which(prog):
            print("gnomecheck: %s is not installed, skipped" % prog)
            return SKIP
    daemon = os.path.join(build, "walpe-daemon")
    if not os.access(daemon, os.X_OK):
        print("gnomecheck: no walpe-daemon in %s" % build)
        return 1
    with open(daemon, "rb") as f:
        if b"--wayland-socket-to" not in f.read():
            print("gnomecheck: this walpe-daemon is built without the Wayland backend, skipped")
            return SKIP
    top = tempfile.mkdtemp(prefix="walpe-gn.", dir=a.tmp or tmp_dir())      # (short: socket paths)
    shell = Shell(top, source)
    failed = 0
    try:
        try:
            shell.start()
        except CannotRun as e:
            print("gnomecheck: cannot run here: %s" % e)
            return SKIP
        x = shell.extension()
        hello = shell.hello()
        print("gnomecheck: GNOME Shell %s, extension state %s, Hello: %s" % (shell.version, x["state"], hello.strip()))
        if x["state"] != 1 or not hello:
            print("FAIL: the extension is not enabled in the shell (state %s; 1 = enabled): %s\n%s"
                  % (x["state"], x["error"], shell.log_tail("shell", 30)))
            return 1
        socket = "'socket': <true>" in hello
        for name, fn, needs_socket in CASES:
            if a.only and a.only != name:
                continue
            if needs_socket and not socket:
                print("%-14s not for this shell (it starts no process for the connection)" % name)
                continue
            if shell.shell.poll() is not None:
                print("FAIL: gnome-shell ended (status %s) before case %s:\n%s"
                      % (shell.shell.returncode, name, shell.log_tail("shell", 30)))
                failed += 1
                break
            try:
                bad = fn(shell, build)
            except (RuntimeError, subprocess.TimeoutExpired) as e:
                bad = ["%s" % e]
            if bad is None:
                print("%-14s skipped (what it needs is not installed)" % name)
                continue
            if isinstance(bad, str):
                print("%-14s skipped (%s)" % (name, bad))
                continue
            for b in bad:
                print("FAIL: %s: %s" % (name, b))
            print("%-14s %s" % (name, "FAILED" if bad else "ok"))
            failed += 1 if bad else 0
        return 1 if failed else 0
    finally:
        shell.stop()
        if a.keep or failed:
            print("gnomecheck: logs in %s" % top)
        else:
            shutil.rmtree(top, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
