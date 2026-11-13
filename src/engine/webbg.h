/* webbg.h - is the page's view see-through or black where the page
 * paints nothing? Pure; web.c and tests/webbg_test.c include it.
 *
 * The view of a web wallpaper has a see-through background: where a page
 * paints nothing its frame is transparent, and the daemon draws frames
 * over black. That is the picture of every version so far.
 *
 * WebKitGTK 2.53.1 to 2.54.2 - every release of 2.54 there is - cannot
 * do that in software mode (the default here, "web_accel" off): for a
 * view that is not opaque it clears the whole frame before it paints,
 * and then paints only what the page changed since that buffer was last
 * used (NonCompositedFrameRenderer: m_surface->clear, then the damage
 * rectangles; 2.52 painted without that clear, and WebKit's main branch
 * limits the clear to the rectangles: "otherwise a non-opaque clear
 * color wipes the rest of the target"). An animated page showed all of
 * itself for three frames - one per buffer - and from then on only what
 * moved, on black. With an opaque background WebKit clears nothing and
 * every frame is the whole page. So with those versions the view's
 * background is opaque black: the same picture wherever the page paints
 * its own background or nothing blends with what is behind the page,
 * since black is what the daemon had behind it anyway. (Not the same:
 * a page without a background of its own whose elements blend with the
 * backdrop, mix-blend-mode on the root - there the backdrop is black
 * now instead of nothing.)
 *
 * Not with WALPE_WEB_SNAPSHOT=1: a WebKit snapshot is the whole page
 * painted anew, nothing is wiped from it, and it is asked for with a
 * transparent background - of an opaque view that costs: the page's
 * requestAnimationFrame then ran 42 to 56 times a second where the limit
 * was 30 (WebKitGTK 2.54.2; 28 with the see-through view), and its
 * process took a third more processor time. There the view stays
 * see-through, as in every version before.
 *
 * WALPE_WEB_OPAQUE=1 / 0 in the daemon's environment makes the
 * background opaque black / see-through whatever the version is and
 * however frames are taken. */
#pragma once
#include <glib.h>
#include <string.h>

/* does this WebKitGTK wipe what a page did not repaint from the frame of
 * a see-through view? */
static inline gboolean web_bg_wiped_by (guint major, guint minor, guint micro)
{
    (void) micro;       /* 2.53.1 - 2.53.91 and 2.54.0 - 2.54.2 all do */
    return major == 2 && (minor == 53 || minor == 54);
}

/* setting: WALPE_WEB_OPAQUE (NULL / "" / anything else = by version);
 * snapshots: frames are WebKit snapshots (WALPE_WEB_SNAPSHOT=1) */
static inline gboolean web_bg_opaque (const char *setting, gboolean snapshots, guint major, guint minor,
                                      guint micro)
{
    if (setting && (!strcmp (setting, "1") || !g_ascii_strcasecmp (setting, "on")))
        return TRUE;
    if (setting && (!strcmp (setting, "0") || !g_ascii_strcasecmp (setting, "off")))
        return FALSE;
    return !snapshots && web_bg_wiped_by (major, minor, micro);
}
