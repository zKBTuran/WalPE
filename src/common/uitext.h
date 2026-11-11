/* uitext.h - text that was written by somebody else (a wallpaper's
 * project.json, a Workshop listing, a line of steamcmd) made fit for a
 * label, a tooltip or one of WalPE's own sentences.
 *
 * Why: such text is shown by the GUI and the tray as it is. A title of a
 * megabyte, 8000 characters without a space (the Workshop allows a
 * description of that length) or a U+202E in a name are all "valid":
 *   - Pango lays a wrapped paragraph out again for every measure, and a
 *     run without a break opportunity costs time with the square of its
 *     length: 8000 unbroken characters in the Workshop details pane were
 *     9 s of CPU for three frames (2000: 0.6 s), 20000 in a library tile
 *     2.6 s per tile;
 *   - control characters and line breaks in a one-line label change its
 *     height or its meaning;
 *   - the Unicode direction controls reorder the words WalPE puts around a
 *     name ("Delete <name>?", "Installed: <name>"). */
#pragma once
#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    /* one line (titles, names, tags, anything put into a sentence): line
     * breaks and tabs become spaces, control characters and the direction
     * controls (U+061C, U+200E/F, U+202A-202E, U+2066-2069) are dropped */
    WALPE_UITEXT_LINE = 0,
    /* free text shown as a paragraph of its own (descriptions): line
     * breaks and direction controls stay, other control characters go,
     * and a run of more than WALPE_UITEXT_RUN characters without a place
     * to break gets a zero-width space (U+200B) every that many */
    WALPE_UITEXT_PARAGRAPHS = 1,
} WalpeUiTextMode;

#define WALPE_UITEXT_RUN 240

/* The text for display: valid UTF-8, at most max_chars characters (a cut
 * ends in "…"), cleaned as the mode says. NULL gives "". g_free. */
char *walpe_ui_text (const char *s, glong max_chars, WalpeUiTextMode mode);

G_END_DECLS
