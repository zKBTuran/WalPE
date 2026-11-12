/* a11yname.h - the name of a control for assistive technology
 *
 * The rows of the settings window, of the quality panel and of a
 * wallpaper's options have their title in a label next to the control, so
 * the control is given that title as its accessible name. For a choice
 * (GtkDropDown) the label property does not arrive: GTK names the control
 * after the value it shows, and a screen reader said "Automatic, combo
 * box" for five different settings. A relation to the title's label does
 * arrive and stays when the selection changes (GTK 4.14: checked on the
 * accessibility bus); the value is still read from the control itself. */
#pragma once
#include <gtk/gtk.h>

/* title_label: the visible label that holds `title`, or NULL */
static inline void walpe_a11y_name (GtkWidget *ctl, const char *title, GtkWidget *title_label)
{
    if (title_label && GTK_IS_DROP_DOWN (ctl))
        gtk_accessible_update_relation (GTK_ACCESSIBLE (ctl), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                        title_label, NULL, -1);
    else
        gtk_accessible_update_property (GTK_ACCESSIBLE (ctl), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
}
