/* shared.js - what the wallpaper items of the type "WalPE" in one Plasma shell share (one
 * item per desktop: per screen and activity): how many there are, and whether this shell
 * has already put its own copy of the KWin script into the running KWin. */
.pragma library

var users = 0;
var replaced = false;
