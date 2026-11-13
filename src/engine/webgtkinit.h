/* webgtkinit.h - GTK brought up on a Wayland display that is named, not
 * on the one the environment names. web.c (the private page host) and
 * tests/webgtkinit_test.c include it.
 *
 * The display of the private page host has to be GTK's default one
 * before anything of WebKit asks for "the" display, and it must not come
 * from the environment: WAYLAND_DISPLAY is the session's compositor, and
 * the environment cannot be changed once threads run. So it is opened by
 * name - and the order of the two steps is GTK's to say:
 *
 *   up to 4.16  a display opened before gtk_init became the default one,
 *               and gtk_init then opened no other. That is what WalPE
 *               did.
 *   from 4.18   GDK ends the process when a display is opened before
 *               gtk_init ("gdk_display_open() was called before
 *               gtk_init()", gdk_ensure_initialized): with the order
 *               above every web wallpaper on the private page host was
 *               off - WebKit's start-up test died of it (Arch, Fedora 42,
 *               Debian 13, Ubuntu 25.04 and later; a Wayland session
 *               without Xwayland).
 *
 * The order here works with both: GTK is initialised first, with no
 * backend allowed, so that it opens no display at all (gtk_init_check
 * says FALSE for that, and GTK is initialised all the same - as it is
 * after a gtk_init_check that found no X server); then the Wayland
 * backend is allowed and the display is opened by name. The first
 * display opened is the default one. */
#pragma once
#include <gtk/gtk.h>

/* -> the display, GTK's default from here on; NULL = not there, and
 * *gtk_failed says whether GTK itself did not come up (TRUE) or its
 * Wayland backend did not connect to `name` (FALSE). Main thread. */
static inline GdkDisplay *web_gtk_init_on (const char *name, gboolean *gtk_failed)
{
    *gtk_failed = FALSE;
    if (!gtk_is_initialized ()) {
        /* gtk_init would setlocale (LC_ALL, "") for the whole daemon (see
         * ensure_gtk in web.c) */
        gtk_disable_setlocale ();
        gdk_set_allowed_backends ("");
        (void) gtk_init_check ();
        if (!gtk_is_initialized ()) {
            *gtk_failed = TRUE;
            return NULL;
        }
    }
    gdk_set_allowed_backends ("wayland");
    /* (none, unless a GTK to come opens one with no backend allowed: that
     * would be the session's, and ours replaces it) */
    GdkDisplay *first = gdk_display_get_default ();
    GdkDisplay *d = gdk_display_open (name);
    if (!d)
        return NULL;
    if (gdk_display_get_default () != d)
        gdk_display_manager_set_default_display (gdk_display_manager_get (), d);
    if (first && first != d)
        gdk_display_close (first);
    return d;
}
