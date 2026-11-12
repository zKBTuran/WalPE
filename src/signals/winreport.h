/* winreport.h - how a desktop shell tells the daemon about the windows of
 * other programs, where the compositor has no window list for clients:
 * one string, a line per output,
 *
 *     NAME<tab>FLAGS
 *
 * NAME the output's name as the compositor gives it to clients
 * (wl_output.name), FLAGS a subset of
 *     c   windows of applications cover the output
 *     a   the focused window is an application's and is on the output
 *     f   that window is fullscreen
 * An output the daemon shows nothing on may be in the report under any
 * name: its "f" still counts for "a fullscreen window has the focus".
 * The WalPE extension for GNOME Shell sends this (contrib/gnome,
 * src/signals/gnome.c). The script the daemon loads into KWin sends the
 * same lines; where both are in one tree, this is the one reader. */
#pragma once
#include <glib.h>
#include <string.h>

enum {
    WALPE_WINREPORT_COVERED = 1u << 0,
    WALPE_WINREPORT_FOCUS = 1u << 1,
    WALPE_WINREPORT_FULLSCREEN = 1u << 2,
};

/* One line of a report: FALSE for a line that is none. *name points into
 * line (not terminated: *name_len bytes). */
static inline gboolean walpe_winreport_line (const char *line, gsize len, const char **name,
                                             gsize *name_len, guint *flags)
{
    const char *tab = memchr (line, '\t', len);
    if (!tab || tab == line)
        return FALSE;
    *name = line;
    *name_len = (gsize) (tab - line);
    *flags = 0;
    for (const char *p = tab + 1; p < line + len; p++) {
        if (*p == 'c')
            *flags |= WALPE_WINREPORT_COVERED;
        else if (*p == 'a')
            *flags |= WALPE_WINREPORT_FOCUS;
        else if (*p == 'f')
            *flags |= WALPE_WINREPORT_FULLSCREEN;
        else
            return FALSE;
    }
    return TRUE;
}
