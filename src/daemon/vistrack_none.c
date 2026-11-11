/* vistrack_none.c - "Skip hidden areas" in a build without the X11
 * backend: there is no tracker. The real one (vistrack.c) reads the X
 * window tree on a connection of its own; no other window system tells a
 * client where other programs' windows are. The daemon never asks for a
 * tracker off X11 (vis_apply_setting), so these only have to exist. */
#include "daemon/vistrack.h"
#include <string.h>

WalpeVisTrack *walpe_vistrack_new (const WalpeVisMon *mons, int n, WalpeSkipHidden mode,
                                   WalpeVisChanged cb, gpointer user)
{
    (void) mons; (void) n; (void) mode; (void) cb; (void) user;
    return NULL;
}

void walpe_vistrack_free (WalpeVisTrack *t)
{
    (void) t;
}

void walpe_vistrack_set_mode (WalpeVisTrack *t, WalpeSkipHidden mode)
{
    (void) t; (void) mode;
}

void walpe_vistrack_set_see_through (WalpeVisTrack *t, const char *classes)
{
    (void) t; (void) classes;
}

void walpe_vistrack_set_tuning (WalpeVisTrack *t, int inset_px, int settle_ms)
{
    (void) t; (void) inset_px; (void) settle_ms;
}

gboolean walpe_vistrack_compositor (WalpeVisTrack *t)
{
    (void) t;
    return FALSE;
}

gboolean walpe_vistrack_clipping (WalpeVisTrack *t)
{
    (void) t;
    return FALSE;
}

void walpe_vistrack_stats (WalpeVisTrack *t, WalpeVisStats *st)
{
    (void) t;
    memset (st, 0, sizeof (*st));
}
