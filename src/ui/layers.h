/* layers.h - "Adjust layers": move, resize or hide parts of a scene
 * wallpaper (a clock, a music widget, a logo) from the options pane.
 *
 * A collapsed section under the wallpaper's options. Opened, it lists the
 * layers of the running scene (searchable, several can be selected) with
 * offset fields and nudge buttons, a size factor, a visibility choice,
 * "Reset selected" and "Reset all". Every change goes to the daemon at once
 * ("layeradj", src/daemon/layers_ipc.h), which applies it to the running
 * wallpaper without a reload and saves it for that wallpaper in
 * config.json. While the section is open the selected layers are outlined
 * on the wallpaper itself (layer names in Workshop scenes rarely say what
 * a layer is); the outline goes away when the section closes, another
 * wallpaper is picked or the window closes, and by itself a few seconds
 * after the GUI stops asking for it.
 *
 * Scenes only: the section is hidden for video, web, GIF and image
 * wallpapers, and when the daemon is too old to know "layers". */
#pragma once
#include <gtk/gtk.h>

typedef void (*WalpeLayersStatus) (const char *msg, gboolean error);

GtkWidget *walpe_layers_panel_new (WalpeLayersStatus status);
/* the wallpaper the details pane shows (path NULL = none) and its type as
 * the library knows it ("scene", "video", ...) */
void walpe_layers_panel_show (GtkWidget *panel, const char *path, const char *type);
/* from the status poll: is that wallpaper on a monitor right now; does the
 * daemon know the "layers" requests (status "layer_adjust") */
void walpe_layers_panel_set_state (GtkWidget *panel, gboolean is_current, gboolean supported);
/* tests / screenshots: open the section */
void walpe_layers_panel_expand (GtkWidget *panel, gboolean open);
