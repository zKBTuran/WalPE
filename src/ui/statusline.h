/* statusline.h - the text of the window's status line.
 *
 * The line is cut with an ellipsis where it does not fit, so its tool tip
 * shows it whole. The two are written together: a line that was taken away
 * kept its tool tip - an error that was over could still be read there, and
 * a screen reader, which names a label without text by its tool tip, went
 * on announcing it. */
#pragma once
#include <gtk/gtk.h>

/* line: one line of text, "" for none */
static inline void walpe_status_line_show (GtkLabel *l, const char *line)
{
    gtk_label_set_text (l, line);
    gtk_widget_set_tooltip_text (GTK_WIDGET (l), *line ? line : NULL);
}
