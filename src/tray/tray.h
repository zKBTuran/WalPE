/* tray.h - walpe-tray: shared types of the tray process
 *
 * walpe-tray is a separate process (a tray bug can never take the daemon
 * or the GUI down). main.c owns the daemon state and builds an abstract
 * menu tree (TrayItem); a backend shows the icon and the menu:
 *   sni.c     StatusNotifierItem + com.canonical.dbusmenu (KDE, waybar, ...)
 *   xembed.c  freedesktop XEmbed system tray (_NET_SYSTEM_TRAY_S<n>: dwm
 *             systray patch, stalonetray, trayer, xfce4-panel, ...); its
 *             menu is drawn by popup.c (GTK4 override-redirect window)
 */
#pragma once
#include <glib.h>
#include <cairo.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------ icon state */
typedef enum {
    TRAY_ICON_RUNNING,
    TRAY_ICON_PAUSED,      /* user pause: dimmed + pause badge */
    TRAY_ICON_AUTOPAUSED,  /* fullscreen / covered pause: small badge only */
    TRAY_ICON_DEAD,        /* daemon not reachable: grey + red badge */
} TrayIconState;

/* icon.c: the WalPE icon drawn at size x size into a cairo ARGB32 surface
 * (premultiplied, native endian). Vector-drawn, so every tray height gets
 * a sharp icon. Caller cairo_surface_destroy(). */
cairo_surface_t *tray_icon_render (int size, TrayIconState st);
/* the icon centred in a w x h transparent surface (tray slots are not
 * always square); icon side = MIN (w, h) minus a small margin */
cairo_surface_t *tray_icon_render_in (int w, int h, TrayIconState st);
/* write a PNG (walpe-tray --export-icon) */
gboolean tray_icon_export_png (const char *path, int size, TrayIconState st);

/* ------------------------------------------------------------- menu tree */
typedef enum {
    TRAY_ITEM_NORMAL,
    TRAY_ITEM_CHECK,
    TRAY_ITEM_RADIO,
    TRAY_ITEM_SEPARATOR,
    TRAY_ITEM_SUBMENU,
    TRAY_ITEM_INFO,        /* insensitive informational line */
} TrayItemKind;

typedef struct TrayItem TrayItem;
struct TrayItem {
    int id;                /* unique in one tree (dbusmenu item id; 0 = root) */
    TrayItemKind kind;
    char *label;           /* plain text, no mnemonics */
    char *action;          /* dispatched to tray_dispatch () */
    char *arg;             /* action argument (a wallpaper path) or NULL */
    gboolean enabled;
    gboolean visible;
    gboolean checked;      /* CHECK / RADIO */
    gboolean is_default;   /* drawn bold (the left-click action) */
    GPtrArray *children;   /* TrayItem* (SUBMENU and the root) */
    TrayItem *parent;
};

TrayItem *tray_item_new (TrayItemKind kind, const char *label, const char *action);
void      tray_item_append (TrayItem *parent, TrayItem *child);
void      tray_item_free (TrayItem *it);
TrayItem *tray_item_find (TrayItem *root, int id);

/* main.c: the current menu (rebuilt on state change; backends re-read it
 * when tray_menu_serial () changes) and the action dispatcher */
TrayItem   *tray_menu_get (void);
guint       tray_menu_serial (void);
void        tray_dispatch (const char *action, const char *arg);
const char *tray_tooltip_title (void);
const char *tray_tooltip_text (void);
TrayIconState tray_icon_state (void);

/* "Quit" asks first (tray.ini confirm_quit); the popup menu asks in
 * place, other hosts get a dialog. "quit-now" skips the question. */
gboolean tray_confirm_quit (void);
gboolean tray_engine_alive (void);

/* left click / activate: open or raise the GUI */
void tray_open_gui (void);

/* ------------------------------------------------------------- backends */
typedef struct {
    const char *name;
    /* start: TRUE when the backend could start (an XEmbed backend with no
     * tray yet still starts: it docks when one appears) */
    gboolean (*start) (void);
    void     (*stop) (void);
    /* state, tooltip or menu changed */
    void     (*update) (void);
} TrayBackend;

extern const TrayBackend tray_backend_sni;
extern const TrayBackend tray_backend_xembed;

/* popup.c: our own menu for trays without a menu protocol (XEmbed, and
 * SNI hosts that call ContextMenu instead of reading dbusmenu). X11 only;
 * x, y = root (screen) pixel of the pointer. */
gboolean tray_popup_available (void);
void     tray_popup_show (int x, int y);
void     tray_popup_hide (void);
gboolean tray_popup_visible (void);
/* the menu tree changed while it is open */
void     tray_popup_refresh (void);
/* tooltip for XEmbed (trays draw none): anchor = icon rectangle on screen */
void     tray_tooltip_show (int ax, int ay, int aw, int ah);
void     tray_tooltip_hide (void);

G_END_DECLS
