/* labelbench - dev tool (needs a display): how long GTK takes to show a
 * label that is set up like one of WalPE's, holding N characters of text.
 * The measurement behind src/common/uitext.h (a limit on the
 * text a label is given): a wrapped paragraph without break opportunities costs time with
 * the square of its length.
 *   labelbench <mode> <n> <kind>
 *   mode  wsdesc | wstitle  Workshop details pane (wrapping, selectable)
 *         tile              library tile title (2 lines, ellipsized)
 *         wstile | status   one line, ellipsized
 *         tag               a chip in the tags flow box
 *   kind  W      n x "W", no place to break
 *         words  five letters and a space
 *         cjk    n CJK characters (break anywhere)
 *         zw     "W" with U+200B after every 64th
 *         w200   "W" with a space after every 200th
 * Prints wall-clock and CPU seconds for the first three frames. */
#include <gtk/gtk.h>
#include <sys/resource.h>
#include <string.h>
static int frames;
static double cpu (void) { struct rusage r; getrusage (RUSAGE_SELF, &r);
  return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6; }
static gint64 t0; static double c0;
static gboolean tick (GtkWidget *w, GdkFrameClock *c, gpointer u)
{
    (void) c; (void) u;
    if (++frames == 3) {
        printf ("frames=3 wall=%.2fs cpu=%.2fs\n", (g_get_monotonic_time () - t0) / 1e6, cpu () - c0);
        fflush (stdout);
        g_application_quit (g_application_get_default ());
        (void) w;
    }
    return G_SOURCE_CONTINUE;
}
static const char *mode, *kind; static long n;
static void activate (GtkApplication *app, gpointer u)
{
    (void) u;
    GString *s = g_string_new (NULL);
    for (long i = 0; i < n; i++) {
        if (!strcmp (kind, "W")) g_string_append_c (s, 'W');
        else if (!strcmp (kind, "words")) g_string_append (s, i % 6 == 5 ? " " : "a");
        else if (!strcmp (kind, "zw")) { g_string_append_c (s, (char) 87); if (i % 64 == 63) g_string_append (s, "\xe2\x80\x8b"); }
        else if (!strcmp (kind, "w200")) g_string_append (s, i % 200 == 199 ? " " : "W");
        else g_string_append (s, "\xe6\xbc\xa2");
    }
    GtkWidget *win = gtk_application_window_new (app);
    gtk_window_set_default_size (GTK_WINDOW (win), 1100, 700);
    GtkWidget *l = gtk_label_new (NULL);
    GtkWidget *child = l;
    if (!strcmp (mode, "wsdesc") || !strcmp (mode, "wstitle")) {
        gtk_label_set_xalign (GTK_LABEL (l), 0); gtk_label_set_wrap (GTK_LABEL (l), TRUE);
        gtk_label_set_wrap_mode (GTK_LABEL (l), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_selectable (GTK_LABEL (l), TRUE);
        GtkWidget *sw = gtk_scrolled_window_new ();
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request (sw, 360, -1);
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append (GTK_BOX (box), l);
        gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), box);
        child = sw;
    } else if (!strcmp (mode, "tile")) {
        gtk_label_set_wrap (GTK_LABEL (l), TRUE);
        gtk_label_set_wrap_mode (GTK_LABEL (l), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_lines (GTK_LABEL (l), 2);
        gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars (GTK_LABEL (l), 10);
        gtk_label_set_max_width_chars (GTK_LABEL (l), 18);
        gtk_widget_set_size_request (l, 160, -1);
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_append (GTK_BOX (box), l); child = box;
    } else if (!strcmp (mode, "wstile") || !strcmp (mode, "status")) {
        gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars (GTK_LABEL (l), 22);
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_append (GTK_BOX (box), l); child = box;
    } else if (!strcmp (mode, "tag")) {
        GtkWidget *fb = gtk_flow_box_new ();
        gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (fb), 12);
        gtk_flow_box_append (GTK_FLOW_BOX (fb), l);
        GtkWidget *sw = gtk_scrolled_window_new ();
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), fb);
        child = sw;
    }
    gtk_label_set_text (GTK_LABEL (l), s->str);
    gtk_window_set_child (GTK_WINDOW (win), child);
    t0 = g_get_monotonic_time (); c0 = cpu ();
    gtk_widget_add_tick_callback (win, tick, NULL, NULL);
    gtk_window_present (GTK_WINDOW (win));
}
int main (int argc, char **argv)
{
    if (argc < 4) return 2;
    mode = argv[1]; n = atol (argv[2]); kind = argv[3];
    GtkApplication *app = gtk_application_new ("org.walpe.s5.labelbench", G_APPLICATION_NON_UNIQUE);
    g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
    return g_application_run (G_APPLICATION (app), 1, argv);
}
