/* selguard.c - the daemon never becomes the owner of an X selection.
 *
 * Web wallpapers run in WebKitGTK inside this process, and WebKit puts
 * text on the X selections whenever a page has some selected: PRIMARY as
 * soon as a script selects text in a form field or in the document (no
 * click needed), CLIPBOARD with document.execCommand ('copy') or
 * navigator.clipboard.writeText () during any click on the desktop (the
 * daemon hands desktop clicks to the page as real input). Taking a
 * selection takes it away from the application that had it: what the user
 * had selected or copied elsewhere is gone, and a page doing this on a
 * timer leaves middle-click paste dead for as long as the wallpaper is
 * shown. Version 0.12 gave the selection up again as soon as it was the
 * daemon's (web.c, on_clipboard_changed) - which stops a page from
 * putting its own text on the clipboard, but the previous owner has lost
 * it by then, and X cannot give it back.
 *
 * WebKitGTK has no setting for this and GDK no hook, so the request is
 * stopped where it leaves the process: XSetSelectionOwner is defined here
 * (the daemon exports it, modules.dynlist, so GTK's calls resolve to this
 * definition before libX11's) and passes on only "no owner" requests. GDK
 * asks the server who owns the selection right after claiming it, sees
 * that it is not GDK, and reports the claim as failed; WebKit carries on
 * without it. Nothing in the daemon owns a selection for a purpose of its
 * own (the tray and the window are other programs), so this covers every
 * selection: PRIMARY, CLIPBOARD and the drag-and-drop one (a drag started
 * in a page cannot hand page-chosen data to another window either).
 *
 * WALPE_WEB_SELECTIONS=1 switches the guard off (then pages can set, and
 * thereby clear, the selections as before; web.c still drops them). */
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "common/api.h"

WALPE_API int XSetSelectionOwner (Display *dpy, Atom selection, Window owner, Time time);

static gint blocked;        /* claims refused so far */

int XSetSelectionOwner (Display *dpy, Atom selection, Window owner, Time time)
{
    /* (plain statics: every caller computes the same values) */
    static int (*real) (Display *, Atom, Window, Time);
    static int allow = -1;
    if (!real)
        real = (int (*) (Display *, Atom, Window, Time)) dlsym (RTLD_NEXT, "XSetSelectionOwner");
    if (allow < 0) {
        const char *e = getenv ("WALPE_WEB_SELECTIONS");
        allow = e && *e && strcmp (e, "0") != 0;
    }
    if (owner != None && !allow) {
        if (g_atomic_int_add (&blocked, 1) == 0)    /* the first time only */
            g_message ("web: a page tried to put text on an X selection (clipboard / middle-click "
                       "paste); wallpapers do not get the selections, they stay with the "
                       "program that has them (WALPE_WEB_SELECTIONS=1 allows it)");
        return 1;       /* Xlib's "request sent"; the selection keeps its owner */
    }
    return real ? real (dpy, selection, owner, time) : 0;
}
