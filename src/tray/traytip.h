/* traytip.h - the tooltip's text as a StatusNotifierItem host is given it
 * (sni.c and tests/tray_ids_test.c include it; no GTK, no bus).
 *
 * The text names the wallpaper: a title from a project.json, a file name -
 * somebody else's words (one clean line of at most 47 characters by then,
 * uitext.h). A host does not have to show it as plain text: the
 * specification lets a tooltip carry "a subset of the HTML markup
 * language", and KDE Plasma's system tray hands the text to a label with
 * Text.AutoText (plasmoids/org.kde.plasma.private.systemtray/contents/ui/
 * items/StatusNotifierItem.qml, 5.27 and 6): Qt then looks at it up to
 * its first line break, and a '<' that opens a tag it knows makes the
 * whole tooltip rich text. A wallpaper called
 *
 *     <img src="http://host/x.png">
 *
 * is 30 characters, and the label loads the picture when the pointer rests
 * on the tray icon - the host's process asks a server the wallpaper's
 * author chose - and the lines under it ("Paused: ...") lose their breaks.
 * The tray's own tooltip window (popup.c) escapes its text; a host cannot
 * be told to.
 *
 * So no '<' and no '>' goes out: they become the single angle quotation
 * marks, which no markup reader takes for anything. "&lt;" needs nothing:
 * it can make Qt read the text as rich text, where it is the character
 * '<' and not the start of a tag. */
#pragma once
#include <glib.h>

/* g_free */
static inline char *tray_tip_for_host (const char *text)
{
    GString *o = g_string_new (NULL);
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == '<')
            g_string_append (o, "\xe2\x80\xb9");     /* U+2039 */
        else if (*p == '>')
            g_string_append (o, "\xe2\x80\xba");     /* U+203A */
        else
            g_string_append_c (o, *p);
    }
    return g_string_free (o, FALSE);
}
