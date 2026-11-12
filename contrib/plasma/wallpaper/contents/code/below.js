/* below.js - the KWin script of the Plasma wallpaper type "WalPE" (KWin 6.5 or newer:
 * workspace.constrain). The type loads it into KWin (../ui/Script.qml); it is no KWin package.
 *
 * walpe-daemon draws the wallpaper in a surface of its own, on KWin on the layer shell's
 * "bottom" layer: above Plasma's desktop window (the icons and widgets), which is opaque.
 * Where the desktop's wallpaper type is "WalPE" (io.github.walpe.plasma) the desktop window
 * is see-through instead, and this script tells KWin to keep it right above the daemon's
 * surface on the same screen: the icons are then seen over the wallpaper. KWin applies such
 * a rule after it has sorted the windows by layer, so it holds across the two layers (and
 * within one, should the daemon be told to use "background").
 *
 * Which desktops are of that type only Plasma's shell knows, so it is asked (its scripting
 * interface on D-Bus) whenever a window of either kind comes or goes. The same question tells
 * the wallpaper type whether a surface of the daemon is on its screen (the key Under of its
 * settings): without one it fills the desktop itself, or the desktop would be black.
 * An opaque desktop is never put above the wallpaper, and KWin drops a rule by itself when
 * one of its two windows goes away. A script whose shell is gone stays quiet (the next shell
 * brings its own). Nothing here runs unless such a window comes or goes;
 * with print() the script says what it asked and was told (KWin's log, category
 * kwin_scripting, debug). */

const TYPE = "io.github.walpe.plasma";
let shell = 0;         // the process of the Plasma shell that loaded this script
let serial = 0;        // of the last question; an older answer is not used
let unanswered = 0;    // questions in a row that Plasma's shell did not answer
const again = new QTimer();
again.singleShot = true;
again.timeout.connect(ask);

function isWalpe(w) {
    return w.resourceClass === "walpe-daemon" && !w.desktopWindow;
}

function isDesktop(w) {
    return w.desktopWindow && w.resourceClass === "plasmashell";
}

// a screen, named so that Plasma's shell and KWin mean the same one
function screenKey(g) {
    return Math.round(g.x) + "," + Math.round(g.y) + "," + Math.round(g.width) + "," + Math.round(g.height);
}

// what Plasma's shell runs: one line "walpe X,Y,W,H" per screen whose desktop is of the type,
// after telling that desktop whether the daemon has a surface on its screen, and which process
// that is (covered: screen -> process id; the type hands the pointer to that process only).
// The desktop of a screen is the one of the current activity (desktopForScreen): Plasma has
// one desktop per screen and activity, shows them all in the one window of the screen, and
// names the screen also for those of the other activities. A desktop of the type that is
// not shown is told that nothing is under it: what it was told last is from another time.
function question(covered) {
    return "const covered = " + JSON.stringify(covered) + ";" +
        "const shown = [];" +
        "for (let s = 0; s < screenCount; s++) {" +
        "  const d = desktopForScreen(s);" +
        "  if (!d) continue;" +
        "  shown.push(d.id);" +
        "  if (d.wallpaperPlugin !== '" + TYPE + "') continue;" +
        "  const g = screenGeometry(s);" +
        "  const key = [g.x, g.y, g.width, g.height].map(Math.round).join(',');" +
        "  const under = key in covered;" +
        "  d.currentConfigGroup = ['Wallpaper', '" + TYPE + "', 'General'];" +
        "  d.writeConfig('Daemon', under ? covered[key] : 0);" +
        "  d.writeConfig('Under', under);" +
        "  print('walpe ' + key + '\\n');" +
        "}" +
        "for (const d of desktops()) {" +
        "  if (shown.indexOf(d.id) >= 0 || d.wallpaperPlugin !== '" + TYPE + "') continue;" +
        "  d.currentConfigGroup = ['Wallpaper', '" + TYPE + "', 'General'];" +
        "  if (String(d.readConfig('Under', false)) === 'true') {" +
        "    d.writeConfig('Daemon', 0);" +
        "    d.writeConfig('Under', false);" +
        "  }" +
        "}";
}

function ask(gone) {
    if (typeof workspace.constrain !== "function")
        return;                       // KWin before 6.5
    const all = workspace.windowList().filter(w => w !== gone);
    again.stop();
    const desktop = all.find(isDesktop);
    if (!desktop)
        return;                       // no desktop window: nobody to ask, nothing to order
    // A shell that ends without taking its script out (it crashed) leaves it here; the next
    // shell loads its own. The one whose shell is gone does nothing any more.
    if (!shell)
        shell = desktop.pid;
    if (shell !== desktop.pid)
        return;
    const covered = {};
    for (const w of all.filter(isWalpe))
        covered[screenKey(w.output.geometry)] = Number(w.pid) || 0;
    const mine = ++serial;
    print("walpe: question " + mine + ", surfaces of the daemon on [" + Object.keys(covered).join(" ") + "]");
    again.interval = 3000;            // Plasma's shell may still be starting: ask again, a few times
    if (unanswered++ < 5)
        again.start();
    callDBus("org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell", "evaluateScript",
             question(covered), function (answer) {
        if (mine !== serial)
            return;
        again.stop();
        unanswered = 0;
        const ofType = String(answer).split("\n").filter(l => l.startsWith("walpe ")).map(l => l.slice(6).trim());
        print("walpe: answer " + mine + ", desktops of the type WalPE on [" + ofType.join(" ") + "]");
        const now = workspace.windowList();
        for (const desktop of now) {
            if (!isDesktop(desktop))
                continue;
            const seeThrough = ofType.indexOf(screenKey(desktop.output.geometry)) >= 0;
            for (const walpe of now) {
                if (!isWalpe(walpe) || walpe.output !== desktop.output)
                    continue;
                if (seeThrough)
                    workspace.constrain(walpe, desktop);
                else
                    workspace.unconstrain(walpe, desktop);
            }
        }
    });
}

function watch(w) {
    if (isWalpe(w) || isDesktop(w)) {
        w.outputChanged.connect(() => ask());
        ask();
    }
}

workspace.windowAdded.connect(watch);
workspace.windowRemoved.connect(function (w) {
    if (isWalpe(w) || isDesktop(w))
        ask(w);
});
workspace.currentActivityChanged.connect(function () {
    ask();
    again.interval = 1000;            // the shell changes its desktops a moment after KWin knows
    again.start();
});
workspace.windowList().forEach(w => { if (isWalpe(w) || isDesktop(w)) w.outputChanged.connect(() => ask()); });
ask();
