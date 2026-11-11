/* propshot.c - render WalPE's "Wallpaper options" panel (src/ui/props.c of
 * the tree it is compiled against) for one project.json into a PNG, without
 * the rest of the GUI: evidence for option labels / controls.
 *   propshot <project.json> <out.png> <style.css> [width] [height] [key=value ...]
 * key=value pairs fill the config the panel reads (saved overrides).
 * Needs an X display (Xvfb). Test tool, not installed. */
#include <gtk/gtk.h>
#include "ui/props.h"
#include "common/util.h"

static const char *out_png;
static GtkWidget *win, *panel;
static GMainLoop *loop;

static void status (const char *m, gboolean e) { (void) m; (void) e; }

static gboolean shoot (gpointer u)
{
    (void) u;
    GdkPaintable *p = gtk_widget_paintable_new (win);
    int w = gtk_widget_get_width (win), h = gtk_widget_get_height (win);
    GtkSnapshot *sn = gtk_snapshot_new ();
    gdk_paintable_snapshot (p, sn, w, h);
    GskRenderNode *node = gtk_snapshot_free_to_node (sn);
    if (node) {
        GskRenderer *r = gtk_native_get_renderer (GTK_NATIVE (win));
        GdkTexture *t = gsk_renderer_render_texture (r, node, &GRAPHENE_RECT_INIT (0, 0, w, h));
        gdk_texture_save_to_png (t, out_png);
        g_object_unref (t);
        gsk_render_node_unref (node);
    }
    g_object_unref (p);
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

int main (int argc, char **argv)
{
    if (argc < 4)
        return 2;
    gtk_init ();
    out_png = argv[2];
    int w = argc > 4 ? atoi (argv[4]) : 760, h = argc > 5 ? atoi (argv[5]) : 2400;
    GtkCssProvider *css = gtk_css_provider_new ();
    gtk_css_provider_load_from_path (css, argv[3]);
    gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    JsonNode *root = wpj_parse_file (argv[1]);
    JsonObject *gen = root ? wpj_obj (json_node_get_object (root), "general") : NULL;
    win = gtk_window_new ();
    gtk_window_set_default_size (GTK_WINDOW (win), w, h);
    gtk_window_set_decorated (GTK_WINDOW (win), FALSE);
    panel = walpe_props_panel_new (status);
    gtk_window_set_child (GTK_WINDOW (win), panel);
    char *dir = g_path_get_dirname (argv[1]);
    walpe_props_panel_show (panel, dir, wpj_obj (gen, "properties"), wpj_obj (gen, "localization"), TRUE);
    walpe_props_panel_set_state (panel, TRUE, TRUE);
    gtk_window_present (GTK_WINDOW (win));
    loop = g_main_loop_new (NULL, FALSE);
    g_timeout_add (2500, shoot, NULL);
    g_main_loop_run (loop);
    return 0;
}
