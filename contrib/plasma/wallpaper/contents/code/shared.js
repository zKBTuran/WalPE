/* shared.js - what the wallpaper items of the type "WalPE" in one Plasma shell share (one
 * item per desktop: per screen and activity): how many there are, and the name under which
 * this shell has its copy of the KWin script in KWin. The name is this shell's alone, so an
 * item that finds it taken knows that a sibling has loaded the script, and nothing of an
 * earlier shell can be in the way. */
.pragma library

var users = 0;
var name = fresh();

function fresh() {
    return "io.github.walpe.below." + Date.now().toString(36) + Math.floor(Math.random() * 1e6).toString(36);
}
