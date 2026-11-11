/* props.h - "Wallpaper options": editor for a wallpaper's user properties.
 *
 * Every project.json "general.properties" entry gets a row, ordered by
 * "order": bool -> switch, slider -> scale + spin button (min/max/step/
 * precision), combo -> dropdown, color -> colour button, textinput ->
 * entry, text/group -> section header or note; file/directory/
 * scenetexture -> a button opening a file / folder dialog (the value is
 * the absolute path). Labels are resolved through the project's
 * "localization" table (ui_* keys; WE's own ui_browse_properties_* keys
 * have built-in English), HTML/BBCode is stripped (first line = label,
 * further lines = subtitle, a picture-only label reads "(image)"); label
 * text is never interpreted as Pango markup. Rows honour
 * WE "condition" expressions (e.g. `a.value && b.value == 2`) live.
 *
 * Values shown = defaults <- overrides saved in config.json <- edits not
 * yet sent. Edits go to the daemon asynchronously, debounced 150 ms, as one
 * "setprops" (the daemon persists them per wallpaper in config.json and
 * reloads the wallpaper if it is the active one). Daemons that understand
 * "setprops"/"resetprops" with a "path" ("props_by_path" in status) also
 * persist edits of a wallpaper that is not active; with an older daemon
 * such edits are staged and sent right after the wallpaper is applied.
 * Each row has a reset-to-default button; "Reset all" clears them all. */
#pragma once
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

typedef void (*WalpePropsStatus) (const char *msg, gboolean error);

GtkWidget *walpe_props_panel_new (WalpePropsStatus status);
/* rebuild for a wallpaper. defs = general.properties (may be NULL),
 * loc = general.localization (may be NULL); both must outlive the panel
 * contents (they are ref'd). */
void walpe_props_panel_show (GtkWidget *panel, const char *path, JsonObject *defs,
                             JsonObject *loc, gboolean is_current);
/* daemon state changed (no rebuild): is the shown wallpaper the active
 * one, can the daemon persist edits by path */
void walpe_props_panel_set_state (GtkWidget *panel, gboolean is_current, gboolean by_path);
/* path was just applied: send edits staged for it (older daemons) */
void walpe_props_panel_applied (GtkWidget *panel, const char *path);
/* focus the options filter entry */
void walpe_props_panel_focus_filter (GtkWidget *panel);
/* WE markup (HTML / BBCode, as in descriptions) -> plain text lines */
char *walpe_props_plain_text (const char *markup);
/* tests: the label build_row gives a property, "title[\nsubtitle]"
 * ("# " prefix = a header/note row without a control); no widgets */
char *walpe_props_label_preview (JsonObject *def, const char *key, JsonObject *loc);
/* tests: is a row with this WE "condition" shown, given {key: value}? */
gboolean walpe_props_condition (const char *expr, JsonObject *values);
