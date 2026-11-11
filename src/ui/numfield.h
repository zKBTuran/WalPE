/* numfield.h - the number fields (GtkSpinButton) of the window
 *
 * GTK converts the text of a number field with strtod, so an empty field
 * reads as 0 and the field's range turns that into its minimum. A field
 * emptied by mistake and left (select all, a letter the numeric field does
 * not take, Tab) was a change to the lowest value: 5 frames a second for
 * the frame rate limit, the smallest value of a wallpaper option.
 * walpe_num_field_keep_when_empty () makes an empty field keep the value
 * it has; GTK writes that value back into it. */
#pragma once
#include <gtk/gtk.h>

static inline int walpe_num_field_input (GtkSpinButton *sb, double *val, gpointer u)
{
    (void) u;
    const char *t = gtk_editable_get_text (GTK_EDITABLE (sb));
    while (t && g_ascii_isspace (*t))
        t++;
    if (t && *t)
        return FALSE;           /* something to read: GTK converts it */
    *val = gtk_spin_button_get_value (sb);
    return TRUE;
}

static inline void walpe_num_field_keep_when_empty (GtkWidget *spin)
{
    g_signal_connect (spin, "input", G_CALLBACK (walpe_num_field_input), NULL);
}
