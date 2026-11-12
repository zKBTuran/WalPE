/* plasma_below_test.js <below.js> - the KWin script of the add-on for KDE Plasma
 * (contrib/plasma/wallpaper/contents/code/below.js) between a stand-in KWin and a
 * stand-in Plasma shell. Run by node (meson: test "plasma-below", left out where there
 * is no node); nothing of KDE is needed.
 *
 * The stand-in KWin has what the script uses of KWin's scripting: workspace (windowList,
 * windowAdded, windowRemoved, currentActivityChanged, constrain, unconstrain), QTimer,
 * callDBus, print. The stand-in shell has what the script's question uses of Plasma's
 * desktop scripting: desktops (), desktopForScreen (), screenCount, screenGeometry (),
 * and per desktop wallpaperPlugin, screen, currentConfigGroup, readConfig, writeConfig -
 * with Plasma's habit of naming the screen also for the desktops of activities that are
 * not shown (seen on Plasma 6.8.2). The question the script sends over "D-Bus" is run as
 * the program text it is. The script must:
 *   put the desktop window above the daemon's surface exactly where the desktop shown
 *   on that screen is of the type "WalPE", and tell that desktop which process is under
 *   it; take both back when the surface goes;
 *   leave a screen alone whose desktop is of another type, also when the same screen
 *   has a desktop of the type in an activity that is not shown, and tell that hidden
 *   desktop that nothing is under it;
 *   follow the activity there and back;
 *   handle two screens apart;
 *   do nothing once the desktop window belongs to another process than the shell that
 *   loaded it;
 *   not use an answer that an earlier question got late. */
"use strict";
const fs = require("fs");
const TYPE = "io.github.walpe.plasma";
let fails = 0;

function check(what, ok, detail) {
    if (!ok) {
        fails++;
        console.log("FAIL: " + what + (detail !== undefined ? ": " + detail : ""));
    }
}

function signal() {
    const fns = [];
    return { connect: (f) => fns.push(f), emit: (...a) => fns.slice().forEach(f => f(...a)) };
}

/* one session: a KWin, a shell, the script loaded */
function session(src, screens) {
    const S = { windows: [], rules: new Set(), calls: [], timers: [], log: [], nextId: 1 };
    S.outputs = screens.map((g, i) => ({ name: "OUT-" + i, geometry: g }));
    /* the shell: activity -> one desktop per screen */
    S.shellPid = 500;
    S.activity = "A";
    S.desks = [];
    S.addDesk = (activity, screen, plugin) => {
        const d = {
            id: S.desks.length + 1, activity: activity, screen: screen, wallpaperPlugin: plugin,
            cfg: {}, currentConfigGroup: [],
            readConfig(k, dflt) { const v = this.cfg[this.currentConfigGroup.join("/") + "/" + k]; return v === undefined ? dflt : v; },
            writeConfig(k, v) { this.cfg[this.currentConfigGroup.join("/") + "/" + k] = v; },
            under() { return this.cfg["Wallpaper/" + TYPE + "/General/Under"] === true; },
            daemon() { return this.cfg["Wallpaper/" + TYPE + "/General/Daemon"] || 0; },
        };
        S.desks.push(d);
        return d;
    };
    S.evaluate = (text) => {
        let out = "";
        const run = new Function("desktops", "desktopForScreen", "screenCount", "screenGeometry", "print", text);
        run(() => S.desks.slice(),
            (s) => S.desks.find(d => d.activity === S.activity && d.screen === s),
            screens.length, (s) => screens[s], (t) => { out += t; });
        return out;
    };
    /* KWin */
    const workspace = {
        windowAdded: signal(), windowRemoved: signal(), currentActivityChanged: signal(),
        windowList: () => S.windows.slice(),
        constrain: (below, above) => S.rules.add(below.id + "<" + above.id),
        unconstrain: (below, above) => S.rules.delete(below.id + "<" + above.id),
    };
    function QTimer() {
        this.timeout = signal();
        this.running = false;
        this.start = () => { this.running = true; };
        this.stop = () => { this.running = false; };
        S.timers.push(this);
    }
    S.shellAnswers = true;
    const callDBus = (service, path, iface, method, arg, then) => {
        check("the script calls Plasma's shell only", service === "org.kde.plasmashell" && method === "evaluateScript", service + " " + method);
        S.calls.push({ text: arg, then: then });
    };
    /* deliver what is waiting on the "bus"; the shell answers the questions in order */
    S.flush = () => {
        while (S.calls.length) {
            const c = S.calls.shift();
            if (S.shellAnswers)
                c.then(S.evaluate(c.text));
        }
    };
    S.fireTimers = () => S.timers.filter(t => t.running).forEach(t => { t.running = false; t.timeout.emit(); });
    S.window = (cls, pid, output, desktop) => {
        const w = { id: S.nextId++, resourceClass: cls, pid: pid, output: output, desktopWindow: !!desktop, outputChanged: signal() };
        S.windows.push(w);
        workspace.windowAdded.emit(w);
        return w;
    };
    S.remove = (w) => {
        workspace.windowRemoved.emit(w);    /* KWin: still in the list while the signal runs */
        S.windows = S.windows.filter(x => x !== w);
        for (const r of Array.from(S.rules))
            if (r.split("<").indexOf(String(w.id)) >= 0)
                S.rules.delete(r);          /* KWin drops a rule with either of its windows */
    };
    S.setActivity = (a) => { S.activity = a; workspace.currentActivityChanged.emit(a); };
    S.workspace = workspace;
    new Function("workspace", "QTimer", "callDBus", "print", src)(workspace, QTimer, callDBus, (t) => S.log.push(t));
    return S;
}

const src = fs.readFileSync(process.argv[2], "utf8");
const one = [{ x: 0, y: 0, width: 1920, height: 1080 }];
const two = [{ x: 0, y: 0, width: 1920, height: 1080 }, { x: 1920, y: 0, width: 2560, height: 1440 }];

/* 1. one screen, the type chosen, the daemon comes and goes */
{
    const S = session(src, one);
    const d = S.addDesk("A", 0, TYPE);
    const desk = S.window("plasmashell", S.shellPid, S.outputs[0], true);
    S.flush();
    check("1: no surface of the daemon: nothing under the desktop", !d.under() && d.daemon() === 0 && S.rules.size === 0);
    const wp = S.window("walpe-daemon", 4242, S.outputs[0], false);
    S.flush();
    check("1: the desktop is put above the daemon's surface", S.rules.has(wp.id + "<" + desk.id), Array.from(S.rules).join(" "));
    check("1: the desktop is told so, and which process", d.under() && d.daemon() === 4242, d.under() + " " + d.daemon());
    S.remove(wp);
    S.flush();
    check("1: the surface gone: the desktop is told", !d.under() && d.daemon() === 0);
    check("1: no rule left", S.rules.size === 0);
}

/* 2. another type on the desktop: the wallpaper stays in front */
{
    const S = session(src, one);
    const d = S.addDesk("A", 0, "org.kde.image");
    const desk = S.window("plasmashell", S.shellPid, S.outputs[0], true);
    const wp = S.window("walpe-daemon", 4242, S.outputs[0], false);
    S.flush();
    check("2: a desktop of another type is not put above the wallpaper", S.rules.size === 0, Array.from(S.rules).join(" "));
    check("2: and is told nothing", !d.under());
    /* the type is chosen: Plasma makes the window anew */
    d.wallpaperPlugin = TYPE;
    S.remove(desk);
    const desk2 = S.window("plasmashell", S.shellPid, S.outputs[0], true);
    S.flush();
    check("2: chosen while the daemon runs", S.rules.has(wp.id + "<" + desk2.id) && d.under());
}

/* 3. two activities, the type on the first one's desktop only. Plasma shows both desktops
 * in the one window of the screen and names the screen for both. */
{
    const S = session(src, one);
    const a = S.addDesk("A", 0, TYPE);
    const b = S.addDesk("B", 0, "org.kde.image");
    let desk = S.window("plasmashell", S.shellPid, S.outputs[0], true);
    const wp = S.window("walpe-daemon", 4242, S.outputs[0], false);
    S.flush();
    check("3: in the first activity the desktop is above the wallpaper", S.rules.has(wp.id + "<" + desk.id) && a.under());
    S.setActivity("B");
    S.flush();
    check("3: in the second activity (another type) the opaque desktop is not above the wallpaper",
          S.rules.size === 0, Array.from(S.rules).join(" "));
    check("3: the hidden desktop of the type is told that nothing is under it", !a.under() && a.daemon() === 0);
    check("3: the shown desktop is told nothing", !b.under());
    /* the type's item hands the window back opaque: Plasma makes it anew */
    S.remove(desk);
    desk = S.window("plasmashell", S.shellPid, S.outputs[0], true);
    S.flush();
    S.fireTimers();
    S.flush();
    check("3: still not after the window was made anew", S.rules.size === 0, Array.from(S.rules).join(" "));
    S.setActivity("A");
    S.remove(desk);
    desk = S.window("plasmashell", S.shellPid, S.outputs[0], true);
    S.flush();
    check("3: back in the first activity", S.rules.has(wp.id + "<" + desk.id) && a.under() && a.daemon() === 4242);
}

/* 4. two screens, the type on the second only; the daemon on both */
{
    const S = session(src, two);
    const d0 = S.addDesk("A", 0, "org.kde.image"), d1 = S.addDesk("A", 1, TYPE);
    const k0 = S.window("plasmashell", S.shellPid, S.outputs[0], true);
    const k1 = S.window("plasmashell", S.shellPid, S.outputs[1], true);
    const w0 = S.window("walpe-daemon", 4242, S.outputs[0], false);
    const w1 = S.window("walpe-daemon", 4242, S.outputs[1], false);
    S.flush();
    check("4: the screen with the type", S.rules.has(w1.id + "<" + k1.id) && d1.under() && d1.daemon() === 4242);
    check("4: the other screen is left alone", !S.rules.has(w0.id + "<" + k0.id) && !d0.under() && S.rules.size === 1,
          Array.from(S.rules).join(" "));
    /* the daemon loses the second screen's surface only */
    S.remove(w1);
    S.flush();
    check("4: that screen's surface gone", !d1.under() && S.rules.size === 0);
}

/* 5. the shell that loaded the script is gone, another one has the desktop */
{
    const S = session(src, one);
    const d = S.addDesk("A", 0, TYPE);
    const desk = S.window("plasmashell", S.shellPid, S.outputs[0], true);
    S.flush();
    S.remove(desk);
    S.window("plasmashell", S.shellPid + 1, S.outputs[0], true);
    S.window("walpe-daemon", 4242, S.outputs[0], false);
    check("5: a script whose shell is gone asks nothing", S.calls.length === 0, S.calls.length);
    S.flush();
    check("5: and orders nothing", S.rules.size === 0 && !d.under());
}

/* 6. an answer that comes after a later question was asked is not used */
{
    const S = session(src, one);
    const d = S.addDesk("A", 0, TYPE);
    const desk = S.window("plasmashell", S.shellPid, S.outputs[0], true);
    S.flush();
    const wp = S.window("walpe-daemon", 4242, S.outputs[0], false);     /* question 1 */
    const first = S.calls.shift();
    const early = S.evaluate(first.text);                               /* the shell has answered it ... */
    d.wallpaperPlugin = "org.kde.image";                                /* ... then the type is taken away */
    S.remove(desk);
    const desk2 = S.window("plasmashell", S.shellPid, S.outputs[0], true);  /* question 2 */
    first.then(early);                                                  /* ... and answer 1 arrives now */
    check("6: a late answer orders nothing", S.rules.size === 0, Array.from(S.rules).join(" "));
    S.flush();
    check("6: the last answer stands", S.rules.size === 0 && !S.rules.has(wp.id + "<" + desk2.id));
}

if (fails) {
    console.log(fails + " check(s) failed");
    process.exit(1);
}
console.log("plasma-below: ok");
