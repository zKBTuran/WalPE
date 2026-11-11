/* popup.c - the tray menu and tooltip drawn by walpe-tray itself
 *
 * XEmbed trays have no menu protocol: like Windows tray icons, the app
 * shows its own menu. This is a small GTK4 window made override-redirect
 * (no window manager involvement: dwm never tiles it) placed at the
 * pointer, with a pointer + keyboard grab like every X11 menu:
 *   - Up/Down/Tab move, Enter/Space activate, Right/Left enter/leave a
 *     submenu, Escape leaves a submenu or closes
 *   - a click outside, focus loss or Escape closes it
 * Submenus slide in place (like GTK4's own popover menus). Colours come
 * from the GTK theme's public named colours, so a dark theme (settings.ini
 * gtk-application-prefer-dark-theme, GTK_THEME=...:dark or the desktop
 * portal's color-scheme) gives a dark menu.
 *
 * Off X11 (a Wayland session) a client can neither place a window nor
 * grab the pointer, and no XEmbed tray exists. The menu is still needed
 * there for StatusNotifierItem hosts that call ContextMenu instead of
 * reading the menu over D-Bus (swaybar before 1.10) and for
 * `walpe-tray --menu`: it then opens as an ordinary small window that the
 * compositor places (fixed size, so tiling compositors float it), with the
 * same keys, and closes when it loses the focus. The tooltip window is
 * X11 only (StatusNotifierItem hosts draw the tooltip themselves).
 */
#include <gtk/gtk.h>
#include <string.h>
#include "tray.h"

/* the X11 parts need GTK's X11 backend and, for Xlib itself, a build with
 * the XEmbed tray (meson defines WALPE_TRAY_XEMBED when libX11 / libXrender
 * are there) */
#if defined (GDK_WINDOWING_X11) && defined (WALPE_TRAY_XEMBED)
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include "walpe-build-config.h"
#ifdef HAVE_XI2
#include <X11/extensions/XInput2.h>
#endif
#define HAVE_GDK_X11 1
#endif

G_GNUC_BEGIN_IGNORE_DEPRECATIONS  /* gdk_x11_* are deprecated in GTK >= 4.18 */

#define CONFIRM_PAGE (-1)   /* "Quit WalPE?" page id */

static struct {
    GtkWidget *win;
    GtkWidget *stack;
    int ax, ay;            /* anchor: pointer position */
    GHashTable *pages;     /* submenu id -> page widget */
    int page_id;           /* shown submenu (0 = root) */
    gboolean grabbed;
    gboolean was_active;
    guint grab_timer;
    int grab_tries;
    gint64 shown_at;
    GtkWidget *tip;
    GtkWidget *tip_label;
    gboolean css_done;
} P;

static const char *css =
    "window.walpe-menu, window.walpe-tip { background: none; }\n"
    ".walpe-menu-frame {\n"
    "  background-color: @theme_bg_color; color: @theme_fg_color;\n"
    "  border: 1px solid alpha(@theme_fg_color, 0.18); padding: 5px;\n"
    "}\n"
    ".walpe-composited .walpe-menu-frame { border-radius: 9px; }\n"
    ".walpe-menu-item {\n"
    "  min-height: 26px; padding: 2px 10px 2px 6px; border-radius: 5px;\n"
    "  background: none; border: none; box-shadow: none; outline: none;\n"
    "  color: @theme_fg_color; font-weight: normal;\n"
    "}\n"
    ".walpe-menu-item:hover, .walpe-menu-item:focus-visible {\n"
    "  background-color: alpha(@theme_fg_color, 0.10);\n"
    "}\n"
    ".walpe-menu-item:active { background-color: alpha(@theme_fg_color, 0.18); }\n"
    ".walpe-menu-item:disabled { color: alpha(@theme_fg_color, 0.45); }\n"
    ".walpe-menu-item.default label.walpe-text { font-weight: bold; }\n"
    ".walpe-menu-item.back label.walpe-text { font-weight: bold; }\n"
    ".walpe-menu-info { padding: 3px 10px 3px 30px; font-size: smaller;\n"
    "  color: alpha(@theme_fg_color, 0.6); }\n"
    ".walpe-menu-sep { margin: 4px 6px; background-color: alpha(@theme_fg_color, 0.15);\n"
    "  min-height: 1px; }\n"
    ".walpe-indicator { min-width: 18px; color: @theme_selected_bg_color; font-weight: bold; }\n"
    ".walpe-arrow { color: alpha(@theme_fg_color, 0.6); }\n"
    ".walpe-tip-frame { background-color: rgba(20,20,22,0.94); color: #f2f2f2;\n"
    "  padding: 5px 9px; border: 1px solid rgba(255,255,255,0.12); }\n"
    ".walpe-composited .walpe-tip-frame { border-radius: 6px; }\n";

/* the toolkit runs on X11: override-redirect, placement and grabs exist */
static gboolean on_x11 (void)
{
#ifdef HAVE_GDK_X11
    GdkDisplay *d = gdk_display_get_default ();
    return d && GDK_IS_X11_DISPLAY (d);
#else
    return FALSE;
#endif
}

gboolean tray_popup_available (void)
{
    return gdk_display_get_default () != NULL;
}

static void dark_from_portal (void)
{
    if (g_getenv ("GTK_THEME"))
        return; /* the user's explicit theme wins */
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return;
    GVariant *r = g_dbus_connection_call_sync (
        bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings", "ReadOne",
        g_variant_new ("(ss)", "org.freedesktop.appearance", "color-scheme"), NULL,
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 300, NULL, NULL);
    if (r) {
        GVariant *v = NULL;
        g_variant_get (r, "(v)", &v);
        while (v && g_variant_is_of_type (v, G_VARIANT_TYPE_VARIANT)) {
            GVariant *in = g_variant_get_variant (v);
            g_variant_unref (v);
            v = in;
        }
        if (v && g_variant_is_of_type (v, G_VARIANT_TYPE_UINT32) && g_variant_get_uint32 (v) == 1)
            g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", TRUE, NULL);
        if (v)
            g_variant_unref (v);
        g_variant_unref (r);
    }
    g_object_unref (bus);
}

static void ensure_css (void)
{
    if (P.css_done)
        return;
    P.css_done = TRUE;
    dark_from_portal ();
    GtkCssProvider *p = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (p, css);
    gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (p),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (p);
}

#ifdef HAVE_GDK_X11
static Display *xdpy (void)
{
    return gdk_x11_display_get_xdisplay (gdk_display_get_default ());
}

static Window xid_of (GtkWidget *w)
{
    GdkSurface *s = gtk_native_get_surface (GTK_NATIVE (w));
    return s ? gdk_x11_surface_get_xid (s) : None;
}

/* realized (X window exists, unmapped): no WM, popup-menu type for the
 * compositor (picom: wintypes popup_menu / tooltip) */
static void make_override_redirect (GtkWidget *w, const char *type)
{
    Display *d = xdpy ();
    Window x = xid_of (w);
    if (!x)
        return;
    XSetWindowAttributes a = { 0 };
    a.override_redirect = True;
    XChangeWindowAttributes (d, x, CWOverrideRedirect, &a);
    Atom wt = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    Atom v = XInternAtom (d, type, False);
    XChangeProperty (d, x, wt, XA_ATOM, 32, PropModeReplace, (unsigned char *) &v, 1);
}

gboolean tray_x11_raise_class (const char *res_name)
{
    if (!on_x11 ())
        return FALSE;
    Display *d = xdpy ();
    Window root = DefaultRootWindow (d);
    Atom cl = XInternAtom (d, "_NET_CLIENT_LIST", False);
    Atom type;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    Window *wins = NULL;
    gboolean own = FALSE;
    if (XGetWindowProperty (d, root, cl, 0, 4096, False, XA_WINDOW, &type, &fmt, &n, &after, &data) ==
            Success && data && fmt == 32) {
        wins = (Window *) data;
    } else {
        if (data)
            XFree (data);
        data = NULL;
        Window r, p;
        unsigned int nn = 0;
        if (!XQueryTree (d, root, &r, &p, &wins, &nn))
            return FALSE;
        n = nn;
        own = TRUE;
    }
    gboolean found = FALSE;
    gdk_x11_display_error_trap_push (gdk_display_get_default ());
    for (unsigned long i = 0; i < n && !found; i++) {
        XClassHint ch = { 0 };
        if (!XGetClassHint (d, wins[i], &ch))
            continue;
        if (ch.res_name && !strcmp (ch.res_name, res_name)) {
            XEvent e = { 0 };
            e.xclient.type = ClientMessage;
            e.xclient.window = wins[i];
            e.xclient.message_type = XInternAtom (d, "_NET_ACTIVE_WINDOW", False);
            e.xclient.format = 32;
            e.xclient.data.l[0] = 2; /* source: pager-like, a user request */
            e.xclient.data.l[1] = CurrentTime;
            XSendEvent (d, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
            XMapRaised (d, wins[i]);
            found = TRUE;
        }
        if (ch.res_name)
            XFree (ch.res_name);
        if (ch.res_class)
            XFree (ch.res_class);
    }
    XFlush (d);
    gdk_x11_display_error_trap_pop_ignored (gdk_display_get_default ());
    if (own && wins)
        XFree (wins);
    if (data)
        XFree (data);
    return found;
}
#else
gboolean tray_x11_raise_class (const char *res_name) { (void) res_name; return FALSE; }
#endif

/* monitor rectangle containing a point (application pixels) */
static GdkRectangle monitor_at (int x, int y)
{
    GdkRectangle best = { 0, 0, 1920, 1080 };
    GListModel *ms = gdk_display_get_monitors (gdk_display_get_default ());
    for (guint i = 0; i < g_list_model_get_n_items (ms); i++) {
        GdkMonitor *m = g_list_model_get_item (ms, i);
        GdkRectangle g;
        gdk_monitor_get_geometry (m, &g);
        if (i == 0)
            best = g;
        g_object_unref (m);
        if (x >= g.x && y >= g.y && x < g.x + g.width && y < g.y + g.height)
            return g;
    }
    return best;
}

/* ================================================================ menu */

static void popup_place (void);

static void page_show (int id)
{
    GtkWidget *pg = g_hash_table_lookup (P.pages, GINT_TO_POINTER (id));
    if (!pg)
        return;
    P.page_id = id;
    gtk_stack_set_visible_child (GTK_STACK (P.stack), pg);
    popup_place ();
    /* keyboard: first sensitive row */
    for (GtkWidget *c = gtk_widget_get_first_child (pg); c; c = gtk_widget_get_next_sibling (c))
        if (GTK_IS_BUTTON (c) && gtk_widget_get_sensitive (c) && gtk_widget_get_visible (c)) {
            gtk_widget_grab_focus (c);
            break;
        }
}

static void on_item_clicked (GtkButton *b, gpointer user)
{
    (void) user;
    int id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "walpe-item"));
    if (g_object_get_data (G_OBJECT (b), "walpe-back")) {
        TrayItem *it = tray_item_find (tray_menu_get (), P.page_id);
        page_show (it && it->parent ? it->parent->id : 0);
        return;
    }
    if (g_object_get_data (G_OBJECT (b), "walpe-quit-now")) {
        tray_popup_hide ();
        tray_dispatch ("quit-now", NULL);
        return;
    }
    TrayItem *it = tray_item_find (tray_menu_get (), id);
    if (!it)
        return;
    if (it->kind == TRAY_ITEM_SUBMENU) {
        page_show (it->id);
        return;
    }
    if (!g_strcmp0 (it->action, "quit") && tray_confirm_quit ()) {
        /* asked in place: a dialog would land at 0,0 under tiling WMs */
        page_show (CONFIRM_PAGE);
        return;
    }
    /* the tree is rebuilt by the action: copy first */
    char *action = g_strdup (it->action), *arg = g_strdup (it->arg);
    tray_popup_hide ();
    tray_dispatch (action, arg);
    g_free (action);
    g_free (arg);
}

static GtkWidget *item_row (TrayItem *it, gboolean back)
{
    GtkWidget *b = gtk_button_new ();
    gtk_widget_add_css_class (b, "walpe-menu-item");
    gtk_widget_set_focus_on_click (b, FALSE);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *ind = gtk_label_new (back ? "‹" : "");
    gtk_widget_add_css_class (ind, "walpe-indicator");
    if (!back && it->kind == TRAY_ITEM_CHECK && it->checked)
        gtk_label_set_text (GTK_LABEL (ind), "✓");
    else if (!back && it->kind == TRAY_ITEM_RADIO && it->checked)
        gtk_label_set_text (GTK_LABEL (ind), "●");
    gtk_box_append (GTK_BOX (box), ind);
    GtkWidget *l = gtk_label_new (it->label);
    gtk_widget_add_css_class (l, "walpe-text");
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_widget_set_hexpand (l, TRUE);
    gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (l), 52);
    gtk_box_append (GTK_BOX (box), l);
    if (!back && it->kind == TRAY_ITEM_SUBMENU) {
        GtkWidget *a = gtk_label_new ("›");
        gtk_widget_add_css_class (a, "walpe-arrow");
        gtk_widget_set_margin_start (a, 16);
        gtk_box_append (GTK_BOX (box), a);
        g_object_set_data (G_OBJECT (b), "walpe-sub", GINT_TO_POINTER (1));
    }
    gtk_button_set_child (GTK_BUTTON (b), box);
    if (back) {
        gtk_widget_add_css_class (b, "back");
        g_object_set_data (G_OBJECT (b), "walpe-back", GINT_TO_POINTER (1));
    }
    if (it->is_default)
        gtk_widget_add_css_class (b, "default");
    gtk_widget_set_sensitive (b, it->enabled);
    g_object_set_data (G_OBJECT (b), "walpe-item", GINT_TO_POINTER (it->id));
    g_signal_connect (b, "clicked", G_CALLBACK (on_item_clicked), NULL);
    return b;
}

static void build_pages (TrayItem *menu)
{
    GtkWidget *pg = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    if (menu->id != 0) {
        gtk_box_append (GTK_BOX (pg), item_row (menu, TRUE));
        GtkWidget *s = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_add_css_class (s, "walpe-menu-sep");
        gtk_box_append (GTK_BOX (pg), s);
    }
    for (guint i = 0; menu->children && i < menu->children->len; i++) {
        TrayItem *it = menu->children->pdata[i];
        if (!it->visible)
            continue;
        if (it->kind == TRAY_ITEM_SEPARATOR) {
            GtkWidget *s = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_add_css_class (s, "walpe-menu-sep");
            gtk_box_append (GTK_BOX (pg), s);
        } else if (it->kind == TRAY_ITEM_INFO) {
            GtkWidget *l = gtk_label_new (it->label);
            gtk_label_set_xalign (GTK_LABEL (l), 0);
            gtk_widget_add_css_class (l, "walpe-menu-info");
            gtk_box_append (GTK_BOX (pg), l);
        } else {
            gtk_box_append (GTK_BOX (pg), item_row (it, FALSE));
            if (it->kind == TRAY_ITEM_SUBMENU)
                build_pages (it);
        }
    }
    g_hash_table_insert (P.pages, GINT_TO_POINTER (menu->id), pg);
    gtk_stack_add_child (GTK_STACK (P.stack), pg);
}

static void build_confirm_page (void)
{
    GtkWidget *pg = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    TrayItem back = { .id = CONFIRM_PAGE, .kind = TRAY_ITEM_SUBMENU, .label = (char *) "Quit WalPE?",
                      .enabled = TRUE, .visible = TRUE };
    gtk_box_append (GTK_BOX (pg), item_row (&back, TRUE));
    GtkWidget *l = gtk_label_new (tray_engine_alive ()
                                      ? "Stops the wallpaper engine (the desktop\n"
                                        "goes back to its plain background)\nand removes the tray icon."
                                      : "Removes the tray icon.");
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_widget_add_css_class (l, "walpe-menu-info");
    gtk_box_append (GTK_BOX (pg), l);
    GtkWidget *s = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class (s, "walpe-menu-sep");
    gtk_box_append (GTK_BOX (pg), s);
    TrayItem q = { .id = CONFIRM_PAGE, .kind = TRAY_ITEM_NORMAL, .label = (char *) "Quit", .enabled = TRUE,
                   .visible = TRUE, .is_default = TRUE };
    GtkWidget *qb = item_row (&q, FALSE);
    g_object_set_data (G_OBJECT (qb), "walpe-quit-now", GINT_TO_POINTER (1));
    gtk_box_append (GTK_BOX (pg), qb);
    TrayItem c = { .id = CONFIRM_PAGE, .kind = TRAY_ITEM_NORMAL, .label = (char *) "Cancel", .enabled = TRUE,
                   .visible = TRUE };
    GtkWidget *cb = item_row (&c, FALSE);
    g_object_set_data (G_OBJECT (cb), "walpe-back", GINT_TO_POINTER (1));
    gtk_box_append (GTK_BOX (pg), cb);
    g_hash_table_insert (P.pages, GINT_TO_POINTER (CONFIRM_PAGE), pg);
    gtk_stack_add_child (GTK_STACK (P.stack), pg);
}

static void fill (void)
{
    GtkWidget *frame = gtk_window_get_child (GTK_WINDOW (P.win));
    if (P.stack)
        gtk_box_remove (GTK_BOX (frame), P.stack);
    if (P.pages)
        g_hash_table_remove_all (P.pages);
    else
        P.pages = g_hash_table_new (NULL, NULL);
    P.stack = gtk_stack_new ();
    gtk_stack_set_hhomogeneous (GTK_STACK (P.stack), FALSE);
    gtk_stack_set_vhomogeneous (GTK_STACK (P.stack), FALSE);
    gtk_stack_set_transition_type (GTK_STACK (P.stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_box_append (GTK_BOX (frame), P.stack);
    TrayItem *m = tray_menu_get ();
    if (m)
        build_pages (m);
    build_confirm_page ();
    /* submenu pages were added before the root page: show the root */
    GtkWidget *root = g_hash_table_lookup (P.pages, GINT_TO_POINTER (0));
    if (root)
        gtk_stack_set_visible_child (GTK_STACK (P.stack), root);
}

/* size = natural size of the visible page; position = at the pointer,
 * flipped left / up where the monitor ends (bar at the bottom, tray at
 * the right edge) */
static void popup_place (void)
{
    if (!P.win)
        return;
    GtkWidget *frame = gtk_window_get_child (GTK_WINDOW (P.win));
    int w, h;
    gtk_widget_measure (frame, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
    gtk_widget_measure (frame, GTK_ORIENTATION_VERTICAL, w, NULL, &h, NULL, NULL);
    GdkRectangle mon = monitor_at (P.ax, P.ay);
    int x = P.ax, y = P.ay;
    if (x + w > mon.x + mon.width)
        x = MAX (mon.x, x - w);
    if (y + h > mon.y + mon.height)
        y = MAX (mon.y, y - h);
    g_debug ("menu: %dx%d at %d,%d", w, h, x, y);
    gtk_window_set_default_size (GTK_WINDOW (P.win), w, h);
#ifdef HAVE_GDK_X11
    if (on_x11 ()) {
        Window xw = xid_of (P.win);
        if (xw) {
            XMoveResizeWindow (xdpy (), xw, x, y, w, h);
            XRaiseWindow (xdpy (), xw);
            XFlush (xdpy ());
        }
    }
#else
    (void) x; (void) y;
#endif
}

#ifdef HAVE_GDK_X11
/* The grab: GDK reads pointer and keyboard input through XInput2, and
 * under a core grab the server reports events outside our windows as core
 * events only, which GDK ignores (a click outside would never close the
 * menu). So grab GDK's own XI2 master devices; core grabs remain the
 * fallback without libXi. */
static int grab_pointer (Display *d, Window w, gboolean on)
{
#ifdef HAVE_XI2
    GdkSeat *seat = gdk_display_get_default_seat (gdk_display_get_default ());
    GdkDevice *ptr = seat ? gdk_seat_get_pointer (seat) : NULL;
    if (ptr && GDK_IS_X11_DEVICE_XI2 (ptr)) {
        int id = gdk_x11_device_get_id (ptr);
        if (!on)
            return XIUngrabDevice (d, id, CurrentTime);
        unsigned char bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
        XISetMask (bits, XI_ButtonPress);
        XISetMask (bits, XI_ButtonRelease);
        XISetMask (bits, XI_Motion);
        XISetMask (bits, XI_Enter);
        XISetMask (bits, XI_Leave);
        XIEventMask m = { id, sizeof bits, bits };
        return XIGrabDevice (d, id, w, CurrentTime, None, XIGrabModeAsync, XIGrabModeAsync, True, &m);
    }
#endif
    if (!on)
        return XUngrabPointer (d, CurrentTime);
    return XGrabPointer (d, w, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                                         EnterWindowMask | LeaveWindowMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}

static int grab_keyboard (Display *d, Window w, gboolean on)
{
#ifdef HAVE_XI2
    GdkSeat *seat = gdk_display_get_default_seat (gdk_display_get_default ());
    GdkDevice *kbd = seat ? gdk_seat_get_keyboard (seat) : NULL;
    if (kbd && GDK_IS_X11_DEVICE_XI2 (kbd)) {
        int id = gdk_x11_device_get_id (kbd);
        if (!on)
            return XIUngrabDevice (d, id, CurrentTime);
        unsigned char bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
        XISetMask (bits, XI_KeyPress);
        XISetMask (bits, XI_KeyRelease);
        XISetMask (bits, XI_FocusIn);
        XISetMask (bits, XI_FocusOut);
        XIEventMask m = { id, sizeof bits, bits };
        return XIGrabDevice (d, id, w, CurrentTime, None, XIGrabModeAsync, XIGrabModeAsync, True, &m);
    }
#endif
    if (!on)
        return XUngrabKeyboard (d, CurrentTime);
    return XGrabKeyboard (d, w, True, GrabModeAsync, GrabModeAsync, CurrentTime);
}

static void ungrab (void)
{
    if (P.grab_timer) {
        g_source_remove (P.grab_timer);
        P.grab_timer = 0;
    }
    if (P.grabbed) {
        Display *d = xdpy ();
        gdk_x11_display_error_trap_push (gdk_display_get_default ());
        grab_pointer (d, None, FALSE);
        grab_keyboard (d, None, FALSE);
        XSync (d, False);
        gdk_x11_display_error_trap_pop_ignored (gdk_display_get_default ());
        P.grabbed = FALSE;
    }
}

static void nudge_pointer (gpointer u)
{
    (void) u;
    if (!P.win)
        return;
    XWarpPointer (xdpy (), None, None, 0, 0, 0, 0, 1, 1);
    XWarpPointer (xdpy (), None, None, 0, 0, 0, 0, -1, -1);
    XFlush (xdpy ());
}

static void focus_popup (Display *d, Window w)
{
    gdk_x11_display_error_trap_push (gdk_display_get_default ());
    XSetInputFocus (d, w, RevertToPointerRoot, CurrentTime);
    XSync (d, False);
    gdk_x11_display_error_trap_pop_ignored (gdk_display_get_default ());
}

/* grab after the map: retried while another client (the tray's own
 * implicit button grab, a WM) still holds the pointer */
static gboolean try_grab (gpointer u)
{
    (void) u;
    if (!P.win) {
        P.grab_timer = 0;
        return G_SOURCE_REMOVE;
    }
    Display *d = xdpy ();
    Window w = xid_of (P.win);
    int pg = grab_pointer (d, w, TRUE);
    int kg = pg == GrabSuccess ? grab_keyboard (d, w, TRUE) : -1;
    if (pg == GrabSuccess && kg == GrabSuccess) {
        P.grabbed = TRUE;
        /* GTK picked the hovered row while the window still had its
         * first, tiny allocation (every row at 0,0: the last one won):
         * once laid out, a zero-sum pointer nudge makes it pick again */
        g_timeout_add_once (80, nudge_pointer, NULL);
        focus_popup (d, w);
        P.grab_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (pg == GrabSuccess)
        grab_pointer (d, None, FALSE);
    if (++P.grab_tries > 50) {
        /* no grab: still usable with the mouse; focus loss closes it */
        g_message ("menu: pointer/keyboard grab failed (%d/%d)", pg, kg);
        focus_popup (d, w);
        P.grab_timer = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}
#endif

static void on_map (GtkWidget *w, gpointer u)
{
    (void) w; (void) u;
    popup_place ();
#ifdef HAVE_GDK_X11
    if (on_x11 ()) {
        P.grab_tries = 0;
        if (!P.grab_timer && try_grab (NULL) == G_SOURCE_CONTINUE)
            P.grab_timer = g_timeout_add (20, try_grab, NULL);
    }
#endif
    page_show (P.page_id);
}

static gboolean on_key (GtkEventControllerKey *k, guint keyval, guint code, GdkModifierType st, gpointer u)
{
    (void) k; (void) code; (void) st; (void) u;
    GtkWidget *f = gtk_root_get_focus (GTK_ROOT (P.win));
    switch (keyval) {
    case GDK_KEY_Escape:
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
    case GDK_KEY_BackSpace:
        if (P.page_id != 0) {
            TrayItem *it = tray_item_find (tray_menu_get (), P.page_id);
            page_show (it && it->parent ? it->parent->id : 0);
        } else if (keyval == GDK_KEY_Escape) {
            tray_popup_hide ();
        }
        return TRUE;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
        if (f && GTK_IS_BUTTON (f) && g_object_get_data (G_OBJECT (f), "walpe-sub"))
            g_signal_emit_by_name (f, "clicked");
        return TRUE;
    default:
        return FALSE;
    }
}

/* clicks outside: with the pointer grab they are reported to this window
 * at coordinates outside it */
static gboolean on_event (GtkEventControllerLegacy *c, GdkEvent *ev, gpointer u)
{
    (void) c; (void) u;
    if (gdk_event_get_event_type (ev) != GDK_BUTTON_PRESS)
        return FALSE;
    double x = 0, y = 0;
    gdk_event_get_position (ev, &x, &y);
    int w = gtk_widget_get_width (P.win), h = gtk_widget_get_height (P.win);
    if (x < 0 || y < 0 || x >= w || y >= h) {
        tray_popup_hide ();
        return TRUE;
    }
    return FALSE;
}

static void on_active (GObject *o, GParamSpec *ps, gpointer u)
{
    (void) ps; (void) u;
    gboolean act = gtk_window_is_active (GTK_WINDOW (o));
    if (act)
        P.was_active = TRUE;
    else if (P.was_active && g_get_monotonic_time () - P.shown_at > 150000)
        g_idle_add_once ((GSourceOnceFunc) tray_popup_hide, NULL);
}

/* the compositor's close button / shortcut on the plain-window menu */
static gboolean on_close_request (GtkWindow *w, gpointer u)
{
    (void) w; (void) u;
    g_idle_add_once ((GSourceOnceFunc) tray_popup_hide, NULL);
    return TRUE;    /* tray_popup_hide destroys it */
}

void tray_popup_show (int x, int y)
{
    if (!tray_popup_available ())
        return;
    ensure_css ();
    if (P.win) {
        /* second right click: toggle like a real menu */
        tray_popup_hide ();
        return;
    }
    tray_tooltip_hide ();
#ifdef HAVE_GDK_X11
    if (on_x11 () && (x < 0 || y < 0)) {
        Window r, c;
        int wx, wy;
        unsigned int mask;
        XQueryPointer (xdpy (), DefaultRootWindow (xdpy ()), &r, &c, &x, &y, &wx, &wy, &mask);
    }
#endif
    P.ax = x;
    P.ay = y;
    P.page_id = 0;
    P.was_active = FALSE;
    P.shown_at = g_get_monotonic_time ();
    P.win = gtk_window_new ();
    gtk_window_set_decorated (GTK_WINDOW (P.win), FALSE);
    gtk_window_set_resizable (GTK_WINDOW (P.win), FALSE);
    gtk_window_set_title (GTK_WINDOW (P.win), "WalPE menu");
    if (!on_x11 ())     /* a window like any other there: let Alt+F4 and friends work */
        g_signal_connect (P.win, "close-request", G_CALLBACK (on_close_request), NULL);
    gtk_widget_add_css_class (P.win, "walpe-menu");
    if (gdk_display_is_composited (gdk_display_get_default ()))
        gtk_widget_add_css_class (P.win, "walpe-composited");
    GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (frame, "walpe-menu-frame");
    gtk_window_set_child (GTK_WINDOW (P.win), frame);
    fill ();

    GtkEventController *k = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (k, GTK_PHASE_CAPTURE);
    g_signal_connect (k, "key-pressed", G_CALLBACK (on_key), NULL);
    gtk_widget_add_controller (P.win, k);
    GtkEventController *lg = gtk_event_controller_legacy_new ();
    gtk_event_controller_set_propagation_phase (lg, GTK_PHASE_CAPTURE);
    g_signal_connect (lg, "event", G_CALLBACK (on_event), NULL);
    gtk_widget_add_controller (P.win, lg);
    g_signal_connect (P.win, "notify::is-active", G_CALLBACK (on_active), NULL);
    g_signal_connect (P.win, "map", G_CALLBACK (on_map), NULL);

    gtk_widget_realize (P.win);
#ifdef HAVE_GDK_X11
    if (on_x11 ())
        make_override_redirect (P.win, "_NET_WM_WINDOW_TYPE_POPUP_MENU");
#endif
    popup_place ();
    gtk_window_present (GTK_WINDOW (P.win));
}

void tray_popup_hide (void)
{
    if (!P.win)
        return;
#ifdef HAVE_GDK_X11
    if (on_x11 ())
        ungrab ();
#endif
    GtkWidget *w = P.win;
    P.win = NULL;
    P.stack = NULL;
    if (P.pages)
        g_hash_table_remove_all (P.pages);
    gtk_window_destroy (GTK_WINDOW (w));
}

gboolean tray_popup_visible (void)
{
    return P.win != NULL;
}

void tray_popup_refresh (void)
{
    if (!P.win)
        return;
    int keep = P.page_id;
    fill ();
    TrayItem *it = tray_item_find (tray_menu_get (), keep);
    page_show (keep == CONFIRM_PAGE || (it && it->kind == TRAY_ITEM_SUBMENU) ? keep : 0);
}

/* ================================================================ tooltip */

void tray_tooltip_show (int ax, int ay, int aw, int ah)
{
    /* XEmbed trays only (they draw no tooltip); it has to be placed */
    if (!on_x11 () || P.win)
        return;
    ensure_css ();
    const char *title = tray_tooltip_title (), *text = tray_tooltip_text ();
    char *t = g_markup_printf_escaped ("<b>%s</b>%s%s", title, *text ? "\n" : "", text);
    if (!P.tip) {
        P.tip = gtk_window_new ();
        gtk_window_set_decorated (GTK_WINDOW (P.tip), FALSE);
        gtk_window_set_resizable (GTK_WINDOW (P.tip), FALSE);
        gtk_widget_add_css_class (P.tip, "walpe-tip");
        if (gdk_display_is_composited (gdk_display_get_default ()))
            gtk_widget_add_css_class (P.tip, "walpe-composited");
        P.tip_label = gtk_label_new (NULL);
        gtk_widget_add_css_class (P.tip_label, "walpe-tip-frame");
        gtk_label_set_xalign (GTK_LABEL (P.tip_label), 0);
        gtk_window_set_child (GTK_WINDOW (P.tip), P.tip_label);
        gtk_widget_realize (P.tip);
#ifdef HAVE_GDK_X11
        make_override_redirect (P.tip, "_NET_WM_WINDOW_TYPE_TOOLTIP");
        /* never takes input */
        Display *d = xdpy ();
        XWMHints h = { .flags = InputHint, .input = False };
        XSetWMHints (d, xid_of (P.tip), &h);
#endif
    }
    gtk_label_set_markup (GTK_LABEL (P.tip_label), t);
    g_free (t);
    int w, h;
    gtk_widget_measure (P.tip_label, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
    gtk_widget_measure (P.tip_label, GTK_ORIENTATION_VERTICAL, w, NULL, &h, NULL, NULL);
    GdkRectangle mon = monitor_at (ax + aw / 2, ay + ah / 2);
    int x = ax + aw / 2 - w / 2;
    int y = ay + ah + 4;
    if (y + h > mon.y + mon.height)
        y = ay - h - 4; /* bar at the bottom: above the icon */
    x = CLAMP (x, mon.x + 2, MAX (mon.x + 2, mon.x + mon.width - w - 2));
    gtk_window_set_default_size (GTK_WINDOW (P.tip), w, h);
#ifdef HAVE_GDK_X11
    XMoveResizeWindow (xdpy (), xid_of (P.tip), x, y, w, h);
#endif
    gtk_window_present (GTK_WINDOW (P.tip));
#ifdef HAVE_GDK_X11
    XMoveResizeWindow (xdpy (), xid_of (P.tip), x, y, w, h);
    XRaiseWindow (xdpy (), xid_of (P.tip));
    XFlush (xdpy ());
#endif
}

void tray_tooltip_hide (void)
{
    if (P.tip)
        gtk_widget_set_visible (P.tip, FALSE);
}

G_GNUC_END_IGNORE_DEPRECATIONS
