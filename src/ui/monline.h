/* monline.h - how the window names what each monitor shows: the choices of
 * the monitor chooser, its tool tip and the line "Showing  1 · A: title".
 * Pure text, no display.
 *
 * The tool tip and the line say "(paused)" for a monitor whose wallpaper
 * the engine paused. They were written again only when a monitor's place
 * or wallpaper changed: a pause that began or ended left them as they
 * were, until the next change of a wallpaper. walpe_mon_line_marks is what
 * else they are made of, to be compared as well. */
#pragma once
#include <glib.h>

typedef struct {
    int index;              /* from 0 */
    const char *name;       /* "" or NULL: "Monitor" */
    int w, h;
    const char *title;      /* of the wallpaper it shows */
    gboolean paused;
} WalpeMonLine;

/* the monitor in the chooser: "1 · HDMI-0 · 1920×1080" (g_free) */
static inline char *walpe_mon_line_choice (const WalpeMonLine *m)
{
    return g_strdup_printf ("%d · %s · %d×%d", m->index + 1, m->name && *m->name ? m->name : "Monitor",
                            m->w, m->h);
}

/* the chooser's tool tip (g_free) */
static inline char *walpe_mon_line_tip (const WalpeMonLine *m, guint n)
{
    GString *tip = g_string_new ("Currently showing:");
    for (guint i = 0; i < n; i++) {
        char *l = walpe_mon_line_choice (&m[i]);
        g_string_append_printf (tip, "\n%s: %s%s", l, m[i].title ? m[i].title : "",
                                m[i].paused ? " (paused)" : "");
        g_free (l);
    }
    return g_string_free (tip, FALSE);
}

/* which wallpaper each monitor shows, in one line; "" with fewer than two
 * monitors (g_free) */
static inline char *walpe_mon_line_now (const WalpeMonLine *m, guint n)
{
    GString *now = g_string_new (NULL);
    for (guint i = 0; i < n && n > 1; i++)
        g_string_append_printf (now, "%s%d · %s: %s%s", now->len ? "     " : "Showing  ", m[i].index + 1,
                                m[i].name && *m[i].name ? m[i].name : "Monitor",
                                m[i].title ? m[i].title : "", m[i].paused ? " (paused)" : "");
    return g_string_free (now, FALSE);
}

/* what the two texts say besides places, names and wallpapers: one letter
 * per monitor (g_free) */
static inline char *walpe_mon_line_marks (const WalpeMonLine *m, guint n)
{
    char *s = g_malloc (n + 1);
    for (guint i = 0; i < n; i++)
        s[i] = m[i].paused ? 'p' : '-';
    s[n] = 0;
    return s;
}
