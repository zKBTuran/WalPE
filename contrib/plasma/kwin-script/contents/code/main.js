// SPDX-License-Identifier: BSD-3-Clause
// KWin (Plasma 6.5 or newer: workspace.constrain) keeps windows of the same layer in the order
// of their last raise. walpe-daemon's background surface and plasmashell's desktop window are
// both in the desktop layer, so without this the newer or last-clicked one covers the other.
// One rule per pair on the same output: WalPE's surface stays below the desktop window.
// KWin drops a rule by itself when one of its two windows goes away.

function isWalpe(w) {
    return w.resourceClass === "walpe-daemon" && !w.desktopWindow;
}

function isDesktop(w) {
    return w.desktopWindow && w.resourceClass === "plasmashell";
}

function link() {
    const all = workspace.windowList();
    for (const below of all) {
        if (!isWalpe(below))
            continue;
        for (const above of all) {
            if (isDesktop(above) && above.output === below.output)
                workspace.constrain(below, above);
        }
    }
}

workspace.windowAdded.connect(function (w) {
    if (isWalpe(w) || isDesktop(w)) {
        link();
        w.outputChanged.connect(link);
    }
});
link();
