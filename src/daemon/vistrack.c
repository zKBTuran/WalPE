/* vistrack.c - see vistrack.h */
#include <X11/Xlib.h>
#include "daemon/vistrack.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <dlfcn.h>
#include <string.h>
#include <time.h>

#define VT_SHAPE_MAX 64         /* rectangles kept of a window's shape */
#define VT_CLIENT_SEARCH 24     /* windows looked at to find a frame's client */

typedef struct {
    Window w;
    int x, y, width, height, bw;    /* as the server has it (root coordinates) */
    gboolean mapped;
    gboolean known;                 /* class and depth read */
    gboolean input_only;
    gboolean wallpaper;             /* one of ours */
    int depth;
    gboolean selected;              /* shape + property events asked for */
    /* shape (window coordinates: origin inside the border) */
    gboolean shape_dirty, shaped;
    WalpeBox *shape;
    int n_shape;
    /* what a compositing manager lets through (read only under one) */
    gboolean props_dirty;
    Window client;                  /* the window carrying WM_STATE (itself, or
                                     * below it under a reparenting manager) */
    gboolean argb;                  /* it or its client has a 32-bit visual */
    gboolean see_through;           /* opacity property < 100 %, or listed class */
    WalpeBox *opaque;               /* _NET_WM_OPAQUE_REGION, window coordinates */
    int n_opaque;
    gint64 still_us;                /* last map / move / resize */
} VtWin;

struct WalpeVisTrack {
    Display *xd;
    Window root;
    GSource *src;
    GPtrArray *stack;               /* VtWin *, lowest first */
    GHashTable *by_xid;             /* root child -> VtWin */
    GHashTable *by_client;          /* client window -> VtWin */
    WalpeVisMon *mons;
    WalpeRegion *vis;
    gboolean *reported;
    int nmon;
    WalpeSkipHidden mode;
    char **see_through;
    int inset, settle_ms;
    gboolean have_shape;
    int shape_event;
    int xfixes_event;               /* first XFixes event code, -1 = not used */
    Atom a_cm, a_opacity, a_opaque, a_wm_state;
    Window cm_owner;
    gboolean cm_check;              /* ask who owns the selection */
    guint cm_timer, settle_timer;
    gboolean dirty;
    gboolean in_dispatch;
    WalpeVisChanged cb;
    gpointer user;
    WalpeVisStats st;
};

typedef struct {
    GSource src;
    GPollFD pfd;
    WalpeVisTrack *t;
} VtSource;

/* CPU time of this thread (us): what the tracker itself costs, without
 * the time spent waiting for the X server's answers */
static gint64 vt_cpu_us (void)
{
    struct timespec ts;
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return (gint64) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void vt_recompute (WalpeVisTrack *t, gboolean force);

/* ------------------------------------------------------------ windows */

static void vt_win_free (gpointer p)
{
    VtWin *v = p;
    g_free (v->shape);
    g_free (v->opaque);
    g_free (v);
}

static VtWin *vt_find (WalpeVisTrack *t, Window w)
{
    return g_hash_table_lookup (t->by_xid, GSIZE_TO_POINTER (w));
}

static int vt_index (WalpeVisTrack *t, const VtWin *v)
{
    for (guint i = 0; i < t->stack->len; i++)
        if (g_ptr_array_index (t->stack, i) == v)
            return (int) i;
    return -1;
}

static void vt_set_client (WalpeVisTrack *t, VtWin *v, Window client)
{
    if (v->client == client)
        return;
    if (v->client != None && v->client != v->w)
        g_hash_table_remove (t->by_client, GSIZE_TO_POINTER (v->client));
    v->client = client;
    if (client != None && client != v->w) {
        g_hash_table_insert (t->by_client, GSIZE_TO_POINTER (client), v);
        XSelectInput (t->xd, client, PropertyChangeMask);
    }
}

/* a child of the root appeared (on top of the others) */
static VtWin *vt_add (WalpeVisTrack *t, Window w)
{
    VtWin *v = vt_find (t, w);
    if (v)
        return v;
    v = g_new0 (VtWin, 1);
    v->w = w;
    v->shape_dirty = v->props_dirty = TRUE;
    for (int i = 0; i < t->nmon; i++)
        v->wallpaper |= t->mons[i].xid == w;
    g_ptr_array_add (t->stack, v);
    g_hash_table_insert (t->by_xid, GSIZE_TO_POINTER (w), v);
    return v;
}

static void vt_remove (WalpeVisTrack *t, Window w)
{
    VtWin *v = vt_find (t, w);
    if (!v)
        return;
    if (v->client != None && v->client != v->w)
        g_hash_table_remove (t->by_client, GSIZE_TO_POINTER (v->client));
    g_hash_table_remove (t->by_xid, GSIZE_TO_POINTER (w));
    g_ptr_array_remove (t->stack, v);       /* frees it (the array owns) */
}

/* v now sits directly above sibling `above` (None = lowest) */
static void vt_restack (WalpeVisTrack *t, VtWin *v, Window above)
{
    int from = vt_index (t, v);
    if (from < 0)
        return;
    VtWin *a = above != None ? vt_find (t, above) : NULL;
    if (above != None && !a)
        return;                             /* a sibling we never heard of */
    int below = a ? vt_index (t, a) : -1;   /* index it must follow */
    if (below == from - 1)
        return;
    g_ptr_array_steal_index (t->stack, (guint) from);
    if (below > from)
        below--;
    g_ptr_array_insert (t->stack, below + 1, v);
}

/* class, depth, geometry and map state from the server (one request) */
static void vt_read_attrs (WalpeVisTrack *t, VtWin *v)
{
    XWindowAttributes wa;
    t->st.roundtrips++;
    if (!XGetWindowAttributes (t->xd, v->w, &wa))
        return;                             /* gone: its DestroyNotify follows */
    v->known = TRUE;
    v->input_only = wa.class == InputOnly;
    v->depth = wa.depth;
    v->x = wa.x;
    v->y = wa.y;
    v->width = wa.width;
    v->height = wa.height;
    v->bw = wa.border_width;
    v->mapped = wa.map_state != IsUnmapped;
}

static void vt_select (WalpeVisTrack *t, VtWin *v)
{
    if (v->selected || v->wallpaper)
        return;
    v->selected = TRUE;
    XSelectInput (t->xd, v->w, PropertyChangeMask);
    if (t->have_shape)
        XShapeSelectInput (t->xd, v->w, ShapeNotifyMask);
}

static void vt_read_shape (WalpeVisTrack *t, VtWin *v)
{
    v->shape_dirty = FALSE;
    g_clear_pointer (&v->shape, g_free);
    v->n_shape = 0;
    v->shaped = FALSE;
    if (!t->have_shape)
        return;
    Bool bs = False, cs = False;
    int xb, yb, xc, yc;
    unsigned wb, hb, wc, hc;
    t->st.roundtrips++;
    if (!XShapeQueryExtents (t->xd, v->w, &bs, &xb, &yb, &wb, &hb, &cs, &xc, &yc, &wc, &hc) || !bs)
        return;
    int n = 0, order = 0;
    t->st.roundtrips++;
    XRectangle *r = XShapeGetRectangles (t->xd, v->w, ShapeBounding, &n, &order);
    v->shaped = TRUE;                       /* no rectangle = nothing of it is drawn */
    if (!r)
        return;
    /* a ragged shape (a round clock: one rectangle per row): the largest
     * ones only - hiding less than the window does is always safe */
    int keep = MIN (n, VT_SHAPE_MAX);
    v->shape = g_new (WalpeBox, (gsize) MAX (1, keep));
    for (int i = 0; i < n; i++) {
        WalpeBox b = { r[i].x, r[i].y, r[i].x + r[i].width, r[i].y + r[i].height };
        if (v->n_shape < keep) {
            v->shape[v->n_shape++] = b;
            continue;
        }
        int small = 0;
        for (int k = 1; k < keep; k++)
            if (walpe_box_area (v->shape[k]) < walpe_box_area (v->shape[small]))
                small = k;
        if (walpe_box_area (b) > walpe_box_area (v->shape[small]))
            v->shape[small] = b;
    }
    XFree (r);
}

/* up to max 32-bit items of a property; XFree *out */
static gulong vt_prop32 (WalpeVisTrack *t, Window w, Atom prop, Atom type, long max, guchar **out)
{
    Atom rt = None;
    int fmt = 0;
    gulong n = 0, left = 0;
    *out = NULL;
    t->st.roundtrips++;
    if (XGetWindowProperty (t->xd, w, prop, 0, max, False, type, &rt, &fmt, &n, &left, out) !=
            Success || !*out)
        return 0;
    if (fmt != 32 || (type != AnyPropertyType && rt != type)) {
        XFree (*out);
        *out = NULL;
        return 0;
    }
    return n;
}

static gboolean vt_has_prop (WalpeVisTrack *t, Window w, Atom prop)
{
    Atom rt = None;
    int fmt = 0;
    gulong n = 0, left = 0;
    guchar *d = NULL;
    t->st.roundtrips++;
    if (XGetWindowProperty (t->xd, w, prop, 0, 0, False, AnyPropertyType, &rt, &fmt, &n, &left,
                            &d) != Success)
        return FALSE;
    if (d)
        XFree (d);
    return rt != None;
}

/* the window below w (or w) that the window manager manages: the one
 * with WM_STATE. None = there is none (an override-redirect window). */
static Window vt_client_of (WalpeVisTrack *t, Window w, int *budget, int depth)
{
    if ((*budget)-- <= 0)
        return None;
    if (vt_has_prop (t, w, t->a_wm_state))
        return w;
    if (depth >= 3)
        return None;
    Window r, p, *kids = NULL, found = None;
    guint n = 0;
    t->st.roundtrips++;
    if (!XQueryTree (t->xd, w, &r, &p, &kids, &n) || !kids)
        return None;
    for (guint i = n; i-- > 0 && found == None && *budget > 0;)
        found = vt_client_of (t, kids[i], budget, depth + 1);
    XFree (kids);
    return found;
}

static gboolean vt_opacity_set (WalpeVisTrack *t, Window w)
{
    guchar *d = NULL;
    gulong n = vt_prop32 (t, w, t->a_opacity, XA_CARDINAL, 1, &d);
    gboolean see = d && n == 1 && (((unsigned long *) d)[0] & 0xffffffffUL) < 0xffffffffUL;
    if (d)
        XFree (d);
    return see;
}

static gboolean vt_class_listed (WalpeVisTrack *t, Window w)
{
    if (!t->see_through || !t->see_through[0])
        return FALSE;
    XClassHint ch = { NULL, NULL };
    t->st.roundtrips++;
    if (!XGetClassHint (t->xd, w, &ch))
        return FALSE;
    gboolean hit = FALSE;
    for (char **c = t->see_through; *c && !hit; c++)
        hit = (ch.res_name && !g_ascii_strcasecmp (*c, ch.res_name)) ||
              (ch.res_class && !g_ascii_strcasecmp (*c, ch.res_class));
    if (ch.res_name)
        XFree (ch.res_name);
    if (ch.res_class)
        XFree (ch.res_class);
    return hit;
}

/* what a compositing manager is told about the window */
static void vt_read_props (WalpeVisTrack *t, VtWin *v)
{
    v->props_dirty = FALSE;
    int budget = VT_CLIENT_SEARCH;
    Window c = vt_client_of (t, v->w, &budget, 0);
    vt_set_client (t, v, c != None ? c : v->w);
    c = v->client;
    v->argb = v->depth == 32;
    int cdepth = v->depth;
    if (c != v->w) {
        XWindowAttributes wa;
        t->st.roundtrips++;
        if (XGetWindowAttributes (t->xd, c, &wa)) {
            cdepth = wa.depth;
            v->argb |= wa.depth == 32;
        }
    }
    (void) cdepth;
    v->see_through = vt_opacity_set (t, v->w) || (c != v->w && vt_opacity_set (t, c)) ||
                     vt_class_listed (t, c);
    g_clear_pointer (&v->opaque, g_free);
    v->n_opaque = 0;
    if (!v->argb || v->see_through)
        return;
    /* an ARGB window says itself where it is opaque (GTK, Qt, browsers
     * and terminals without transparency do): x, y, width, height in the
     * client window's coordinates */
    guchar *d = NULL;
    gulong n = vt_prop32 (t, c, t->a_opaque, XA_CARDINAL, 4 * VT_SHAPE_MAX, &d);
    if (!d)
        return;
    int ox = 0, oy = 0;
    if (c != v->w) {
        Window child;
        t->st.roundtrips++;
        if (!XTranslateCoordinates (t->xd, c, v->w, 0, 0, &ox, &oy, &child))
            n = 0;
    }
    const unsigned long *q = (const unsigned long *) d;
    v->opaque = g_new (WalpeBox, (gsize) MAX (1, n / 4));
    for (gulong i = 0; i + 3 < n; i += 4) {
        long x = (long) (q[i] & 0xffffffffUL), y = (long) (q[i + 1] & 0xffffffffUL);
        long w = (long) (q[i + 2] & 0xffffffffUL), h = (long) (q[i + 3] & 0xffffffffUL);
        if (x > 32767 || y > 32767 || w > 32767 || h > 32767)
            continue;                       /* negative / absurd values */
        v->opaque[v->n_opaque++] = (WalpeBox) { ox + (int) x, oy + (int) y, ox + (int) (x + w),
                                                oy + (int) (y + h) };
    }
    XFree (d);
}

/* the window's outer rectangle (borders included) in root coordinates */
static WalpeBox vt_outer (const VtWin *v)
{
    return (WalpeBox) { v->x, v->y, v->x + v->width + 2 * v->bw, v->y + v->height + 2 * v->bw };
}

/* the boxes (root coordinates) window v hides. *wait_us: under a
 * compositing manager, when a window that is not still long enough will
 * start to hide (0 = no such window). */
static void vt_cover (WalpeVisTrack *t, VtWin *v, gboolean compositor, gint64 now, GArray *out,
                      gint64 *wait_us)
{
    if (!v->mapped || v->wallpaper)
        return;
    if (!v->known) {
        vt_read_attrs (t, v);
        if (!v->known || !v->mapped)
            return;
    }
    if (v->input_only)
        return;
    const WalpeBox outer = vt_outer (v);
    gboolean on = FALSE;
    for (int i = 0; i < t->nmon && !on; i++) {
        const WalpeVisMon *m = &t->mons[i];
        on = walpe_box_overlaps (outer, (WalpeBox) { m->x, m->y, m->x + m->width, m->y + m->height });
    }
    if (!on)
        return;         /* off every monitor (dwm parks hidden tags there): nothing is asked */
    vt_select (t, v);
    if (v->shape_dirty)
        vt_read_shape (t, v);
    const int ox = v->x + v->bw, oy = v->y + v->bw;   /* window origin in root coordinates */
    if (!compositor) {
        /* what the X server clips: the bounding shape, borders included */
        if (!v->shaped) {
            g_array_append_val (out, outer);
            return;
        }
        for (int i = 0; i < v->n_shape; i++) {
            WalpeBox b = { ox + v->shape[i].x0, oy + v->shape[i].y0, ox + v->shape[i].x1,
                           oy + v->shape[i].y1 };
            g_array_append_val (out, b);
        }
        return;
    }
    if (v->props_dirty)
        vt_read_props (t, v);
    if (v->see_through)
        return;
    gint64 settled = v->still_us + (gint64) t->settle_ms * 1000;
    if (now < settled) {
        if (!*wait_us || settled < *wait_us)
            *wait_us = settled;
        return;
    }
    const WalpeBox *src = &outer;
    int n = 1, dx = 0, dy = 0;
    if (v->argb) {
        src = v->opaque;
        n = v->n_opaque;
        dx = ox;
        dy = oy;
    } else if (v->shaped) {
        src = v->shape;
        n = v->n_shape;
        dx = ox;
        dy = oy;
    }
    for (int i = 0; i < n; i++) {
        WalpeBox b = { src[i].x0 + dx + t->inset, src[i].y0 + dy + t->inset,
                       src[i].x1 + dx - t->inset, src[i].y1 + dy - t->inset };
        b = walpe_box_intersect (b, outer);
        if (!walpe_box_empty (b))
            g_array_append_val (out, b);
    }
}

/* ----------------------------------------------------------- regions */

static gboolean vt_settle_cb (gpointer user)
{
    WalpeVisTrack *t = user;
    t->settle_timer = 0;
    gint64 t0 = vt_cpu_us ();
    vt_recompute (t, FALSE);
    XFlush (t->xd);
    t->st.cpu_us += vt_cpu_us () - t0;
    return G_SOURCE_REMOVE;
}

static gboolean vt_cm_cb (gpointer user)
{
    WalpeVisTrack *t = user;
    t->cm_timer = 0;
    t->cm_check = TRUE;
    gint64 t0 = vt_cpu_us ();
    vt_recompute (t, FALSE);
    XFlush (t->xd);
    t->st.cpu_us += vt_cpu_us () - t0;
    return G_SOURCE_REMOVE;
}

gboolean walpe_vistrack_compositor (WalpeVisTrack *t)
{
    return t->cm_owner != None;
}

gboolean walpe_vistrack_clipping (WalpeVisTrack *t)
{
    return t->mode == WALPE_SKIP_ALWAYS || (t->mode == WALPE_SKIP_AUTO && t->cm_owner == None);
}

static void vt_recompute (WalpeVisTrack *t, gboolean force)
{
    t->dirty = FALSE;
    t->st.recomputes++;
    if (t->cm_check) {
        t->cm_check = FALSE;
        t->st.roundtrips++;
        Window o = XGetSelectionOwner (t->xd, t->a_cm);
        if (o != t->cm_owner) {
            t->cm_owner = o;
            if (o != None)      /* its DestroyNotify, wherever in the tree it is */
                XSelectInput (t->xd, o, StructureNotifyMask | PropertyChangeMask);
            g_message ("skip hidden areas: compositing manager %s%s",
                       o != None ? "running" : "not running",
                       walpe_vistrack_clipping (t) ? "" : " (everything counts as visible)");
        }
    }
    gboolean compositor = t->cm_owner != None;
    gboolean clipping = walpe_vistrack_clipping (t);
    gint64 now = g_get_monotonic_time (), wait_us = 0;
    GArray *boxes = g_array_new (FALSE, FALSE, sizeof (WalpeBox));
    for (int i = 0; i < t->nmon; i++) {
        const WalpeVisMon *m = &t->mons[i];
        const WalpeBox full = { 0, 0, m->width, m->height };
        WalpeRegion r;
        walpe_region_init (&r);
        walpe_region_set_boxes (&r, &full, 1);
        VtWin *own = clipping ? vt_find (t, m->xid) : NULL;
        int at = own ? vt_index (t, own) : -1;
        if (at >= 0) {
            g_array_set_size (boxes, 0);
            for (guint k = (guint) at + 1; k < t->stack->len; k++)
                vt_cover (t, g_ptr_array_index (t->stack, k), compositor, now, boxes, &wait_us);
            for (guint k = 0; k < boxes->len; k++) {
                WalpeBox *b = &g_array_index (boxes, WalpeBox, k);
                b->x0 -= m->x;
                b->x1 -= m->x;
                b->y0 -= m->y;
                b->y1 -= m->y;
                *b = walpe_box_intersect (*b, full);
            }
            walpe_region_subtract_boxes (&r, (const WalpeBox *) boxes->data, (int) boxes->len);
        }
        if (force || !t->reported[i] || !walpe_region_equal (&r, &t->vis[i])) {
            t->reported[i] = TRUE;
            walpe_region_copy (&t->vis[i], &r);
            t->st.changes++;
            if (t->cb)
                t->cb (i, &t->vis[i], t->user);
        }
        walpe_region_clear (&r);
    }
    g_array_unref (boxes);
    if (t->settle_timer) {
        g_source_remove (t->settle_timer);
        t->settle_timer = 0;
    }
    if (wait_us)
        t->settle_timer = g_timeout_add ((guint) MAX (1, (wait_us - now) / 1000 + 1), vt_settle_cb, t);
}

/* ------------------------------------------------------------- events */

static void vt_cm_maybe (WalpeVisTrack *t)
{
    /* without XFixes nothing announces a compositing manager: it creates
     * (and on exit destroys) the window that owns the selection, so ask
     * when a child of the root comes or goes, and once more a moment
     * later (the window exists before it owns the selection) */
    if (t->xfixes_event >= 0)
        return;
    t->cm_check = TRUE;
    if (!t->cm_timer)
        t->cm_timer = g_timeout_add (400, vt_cm_cb, t);
}

static void vt_event (WalpeVisTrack *t, XEvent *e)
{
    gint64 now = g_get_monotonic_time ();
    t->st.events++;
    /* what a client sent with XSendEvent says nothing about the server's
     * state (ICCCM has clients send UnmapNotify to the root; the real one
     * arrives as well) */
    if (e->xany.send_event)
        return;
    switch (e->type) {
    case CreateNotify:
        if (e->xcreatewindow.parent == t->root) {
            VtWin *v = vt_add (t, e->xcreatewindow.window);
            v->x = e->xcreatewindow.x;
            v->y = e->xcreatewindow.y;
            v->width = e->xcreatewindow.width;
            v->height = e->xcreatewindow.height;
            v->bw = e->xcreatewindow.border_width;
            vt_cm_maybe (t);
        }
        return;
    case DestroyNotify: {
        Window w = e->xdestroywindow.window;
        if (w == t->cm_owner)
            t->cm_check = t->dirty = TRUE;
        VtWin *c = g_hash_table_lookup (t->by_client, GSIZE_TO_POINTER (w));
        if (c) {
            g_hash_table_remove (t->by_client, GSIZE_TO_POINTER (w));
            c->client = None;
            c->props_dirty = TRUE;
        }
        if (vt_find (t, w)) {
            vt_remove (t, w);
            t->dirty = TRUE;
            vt_cm_maybe (t);
        }
        return;
    }
    case ReparentNotify:
        if (e->xreparent.parent == t->root) {
            VtWin *v = vt_add (t, e->xreparent.window);
            v->known = FALSE;               /* size and map state: ask */
            v->mapped = TRUE;               /* (vt_read_attrs corrects it) */
            v->still_us = now;
        } else {
            vt_remove (t, e->xreparent.window);
        }
        t->dirty = TRUE;
        return;
    case MapNotify:
    case UnmapNotify: {
        Window w = e->type == MapNotify ? e->xmap.window : e->xunmap.window;
        Window ev = e->type == MapNotify ? e->xmap.event : e->xunmap.event;
        VtWin *v = ev == t->root ? vt_find (t, w) : NULL;
        if (!v)
            return;
        v->mapped = e->type == MapNotify;
        if (v->mapped) {
            v->props_dirty = v->shape_dirty = TRUE;
            v->still_us = now;
        }
        t->dirty = TRUE;
        return;
    }
    case ConfigureNotify: {
        VtWin *v = e->xconfigure.event == t->root ? vt_find (t, e->xconfigure.window) : NULL;
        if (!v)
            return;
        if (v->x != e->xconfigure.x || v->y != e->xconfigure.y ||
            v->width != e->xconfigure.width || v->height != e->xconfigure.height ||
            v->bw != e->xconfigure.border_width) {
            v->x = e->xconfigure.x;
            v->y = e->xconfigure.y;
            v->width = e->xconfigure.width;
            v->height = e->xconfigure.height;
            v->bw = e->xconfigure.border_width;
            v->still_us = now;
        }
        vt_restack (t, v, e->xconfigure.above);
        t->dirty = TRUE;
        return;
    }
    case GravityNotify: {
        VtWin *v = e->xgravity.event == t->root ? vt_find (t, e->xgravity.window) : NULL;
        if (!v)
            return;
        v->x = e->xgravity.x;
        v->y = e->xgravity.y;
        v->still_us = now;
        t->dirty = TRUE;
        return;
    }
    case CirculateNotify: {
        VtWin *v = e->xcirculate.event == t->root ? vt_find (t, e->xcirculate.window) : NULL;
        if (!v)
            return;
        g_ptr_array_steal_index (t->stack, (guint) vt_index (t, v));
        if (e->xcirculate.place == PlaceOnTop)
            g_ptr_array_add (t->stack, v);
        else
            g_ptr_array_insert (t->stack, 0, v);
        t->dirty = TRUE;
        return;
    }
    case PropertyNotify: {
        Atom a = e->xproperty.atom;
        if (a != t->a_opacity && a != t->a_opaque && a != XA_WM_CLASS && a != t->a_wm_state)
            return;     /* titles, icons, user time ...: nothing to do */
        VtWin *v = vt_find (t, e->xproperty.window);
        if (!v)
            v = g_hash_table_lookup (t->by_client, GSIZE_TO_POINTER (e->xproperty.window));
        if (v && !v->props_dirty) {
            v->props_dirty = TRUE;
            if (t->cm_owner != None)            /* matters under a compositor only */
                t->dirty = TRUE;
        }
        return;
    }
    default:
        break;
    }
    if (t->have_shape && e->type == t->shape_event + ShapeNotify) {
        XShapeEvent *se = (XShapeEvent *) e;
        VtWin *v = se->kind == ShapeBounding ? vt_find (t, se->window) : NULL;
        if (v) {
            v->shape_dirty = TRUE;
            t->dirty = TRUE;
        }
    } else if (t->xfixes_event >= 0 && e->type == t->xfixes_event) {
        t->cm_check = t->dirty = TRUE;          /* XFixesSelectionNotify */
    }
}

static gboolean vt_prepare (GSource *src, gint *timeout)
{
    *timeout = -1;
    return XEventsQueued (((VtSource *) src)->t->xd, QueuedAlready) > 0;
}

static gboolean vt_check (GSource *src)
{
    VtSource *s = (VtSource *) src;
    return (s->pfd.revents & (G_IO_IN | G_IO_HUP | G_IO_ERR)) != 0 ||
           XEventsQueued (s->t->xd, QueuedAlready) > 0;
}

/* every event that has arrived is taken first, the regions are worked out
 * once after them: a window dragged across the screen sends a
 * ConfigureNotify per pointer motion, and however many of them wait here
 * they cost one pass over the window list */
static gboolean vt_dispatch (GSource *src, GSourceFunc cb, gpointer user)
{
    (void) cb; (void) user;
    WalpeVisTrack *t = ((VtSource *) src)->t;
    gint64 t0 = vt_cpu_us ();
    t->in_dispatch = TRUE;
    while (XPending (t->xd)) {
        XEvent e;
        XNextEvent (t->xd, &e);
        vt_event (t, &e);
    }
    if (t->dirty)
        vt_recompute (t, FALSE);
    XFlush (t->xd);
    t->in_dispatch = FALSE;
    t->st.cpu_us += vt_cpu_us () - t0;
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs vt_funcs = { vt_prepare, vt_check, vt_dispatch, NULL, NULL, NULL };

/* ------------------------------------------------------------- public */

static void vt_xfixes_init (WalpeVisTrack *t)
{
    t->xfixes_event = -1;
    void *lib = dlopen ("libXfixes.so.3", RTLD_LAZY);
    if (!lib)
        return;
    Bool (*query) (Display *, int *, int *) = (Bool (*) (Display *, int *, int *))
        dlsym (lib, "XFixesQueryExtension");
    Status (*version) (Display *, int *, int *) = (Status (*) (Display *, int *, int *))
        dlsym (lib, "XFixesQueryVersion");
    void (*select_sel) (Display *, Window, Atom, unsigned long) =
        (void (*) (Display *, Window, Atom, unsigned long))
        dlsym (lib, "XFixesSelectSelectionInput");
    int ev = 0, err = 0, maj = 0, min = 0;
    if (!query || !version || !select_sel || !query (t->xd, &ev, &err) ||
        !version (t->xd, &maj, &min) || maj < 1)
        return;
    /* owner set / owner window destroyed / owner's client closed */
    select_sel (t->xd, t->root, t->a_cm, (1UL << 0) | (1UL << 1) | (1UL << 2));
    t->xfixes_event = ev;       /* XFixesSelectionNotify = base + 0 */
}

WalpeVisTrack *walpe_vistrack_new (const WalpeVisMon *mons, int n, WalpeSkipHidden mode,
                                   WalpeVisChanged cb, gpointer user)
{
    Display *xd = XOpenDisplay (NULL);
    if (!xd)
        return NULL;
    WalpeVisTrack *t = g_new0 (WalpeVisTrack, 1);
    t->xd = xd;
    t->root = DefaultRootWindow (xd);
    t->mode = mode;
    t->cb = cb;
    t->user = user;
    t->nmon = n;
    t->mons = g_memdup2 (mons, sizeof (WalpeVisMon) * (gsize) MAX (1, n));
    t->vis = g_new0 (WalpeRegion, (gsize) MAX (1, n));
    t->reported = g_new0 (gboolean, (gsize) MAX (1, n));
    t->settle_ms = 500;
    t->stack = g_ptr_array_new_with_free_func (vt_win_free);
    t->by_xid = g_hash_table_new (NULL, NULL);
    t->by_client = g_hash_table_new (NULL, NULL);
    char sel[32];
    g_snprintf (sel, sizeof sel, "_NET_WM_CM_S%d", DefaultScreen (xd));
    t->a_cm = XInternAtom (xd, sel, False);
    t->a_opacity = XInternAtom (xd, "_NET_WM_WINDOW_OPACITY", False);
    t->a_opaque = XInternAtom (xd, "_NET_WM_OPAQUE_REGION", False);
    t->a_wm_state = XInternAtom (xd, "WM_STATE", False);
    int serr = 0;
    t->have_shape = XShapeQueryExtension (xd, &t->shape_event, &serr);
    vt_xfixes_init (t);

    /* events first, then the tree: a window created in between shows up
     * in both and is only added once */
    XSelectInput (xd, t->root, SubstructureNotifyMask);
    Window r, p, *kids = NULL;
    guint nk = 0;
    t->st.roundtrips++;
    if (XQueryTree (xd, t->root, &r, &p, &kids, &nk) && kids) {
        for (guint i = 0; i < nk; i++)      /* lowest first */
            vt_read_attrs (t, vt_add (t, kids[i]));
        XFree (kids);
    }
    t->cm_check = TRUE;

    VtSource *s = (VtSource *) g_source_new (&vt_funcs, sizeof (VtSource));
    s->t = t;
    s->pfd.fd = ConnectionNumber (xd);
    s->pfd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
    g_source_add_poll (&s->src, &s->pfd);
    g_source_set_name (&s->src, "walpe visible-region tracker");
    g_source_attach (&s->src, NULL);
    t->src = &s->src;

    vt_recompute (t, TRUE);
    XFlush (xd);
    return t;
}

void walpe_vistrack_free (WalpeVisTrack *t)
{
    if (!t)
        return;
    if (t->settle_timer)
        g_source_remove (t->settle_timer);
    if (t->cm_timer)
        g_source_remove (t->cm_timer);
    g_source_destroy (t->src);
    g_source_unref (t->src);
    g_ptr_array_unref (t->stack);
    g_hash_table_unref (t->by_xid);
    g_hash_table_unref (t->by_client);
    for (int i = 0; i < t->nmon; i++)
        walpe_region_clear (&t->vis[i]);
    g_free (t->vis);
    g_free (t->reported);
    g_free (t->mons);
    g_strfreev (t->see_through);
    XCloseDisplay (t->xd);      /* drops every event selection it made */
    g_free (t);
}

void walpe_vistrack_set_mode (WalpeVisTrack *t, WalpeSkipHidden mode)
{
    if (t->mode == mode)
        return;
    t->mode = mode;
    vt_recompute (t, FALSE);
    XFlush (t->xd);
}

void walpe_vistrack_set_see_through (WalpeVisTrack *t, const char *classes)
{
    g_strfreev (t->see_through);
    t->see_through = NULL;
    if (classes && *classes) {
        t->see_through = g_strsplit (classes, ",", -1);
        for (char **c = t->see_through; *c; c++)
            g_strstrip (*c);
    }
    for (guint i = 0; i < t->stack->len; i++)
        ((VtWin *) g_ptr_array_index (t->stack, i))->props_dirty = TRUE;
    if (t->reported[0]) {
        vt_recompute (t, FALSE);
        XFlush (t->xd);
    }
}

void walpe_vistrack_set_tuning (WalpeVisTrack *t, int inset_px, int settle_ms)
{
    inset_px = CLAMP (inset_px, 0, 256);
    settle_ms = CLAMP (settle_ms, 0, 10000);
    if (t->inset == inset_px && t->settle_ms == settle_ms)
        return;
    t->inset = inset_px;
    t->settle_ms = settle_ms;
    if (t->reported[0]) {
        vt_recompute (t, FALSE);
        XFlush (t->xd);
    }
}

void walpe_vistrack_stats (WalpeVisTrack *t, WalpeVisStats *st)
{
    *st = t->st;
    st->windows = t->stack->len;
}
