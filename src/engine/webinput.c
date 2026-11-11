/* webinput.c - pointer input and navigation guard for an off-screen WebKit
 * page (walpe-web.so, main thread only).
 *
 * Why this is not simply "forward the X events": the page lives in an
 * off-screen override-redirect window that the real pointer never reaches.
 * What the user sees is the daemon's own EGL window, whose input shape is
 * empty so that clicks keep going to the root window (dwm and friends own
 * ButtonPress there). The daemon therefore watches the real pointer
 * (XQueryPointer poll for the position, XInput2 raw button events for
 * buttons and the wheel) and hands what happened on the empty desktop to
 * the page of the monitor it happened on (web.c: walpe_web_input).
 *
 * Delivery ("native"): WebKitGTK takes input only as GDK events on its
 * widget, and GTK 4 has no public constructor for them. So each event is
 * written as the XInput2 event the X server would have sent had the
 * pointer been over the page's window (XI_Enter / XI_Motion /
 * XI_ButtonPress / XI_ButtonRelease / XI_Leave, buttons 4-7 = wheel) and
 * put on GDK's Xlib event queue with XPutBackEvent (XSendEvent cannot carry
 * GenericEvents). GDK translates it like any other, GTK routes it to the
 * WebKitWebView, and the page gets TRUSTED DOM events: :hover, mouseenter /
 * mouseleave, mousedown / mouseup / click / dblclick, auxclick, contextmenu,
 * wheel (which also scrolls), pointer events, focus, default actions and
 * user activation (a click may start audio).
 *
 * Fallback ("js"): script-dispatched (untrusted) events through
 * window.__walpeIn. Used when XInput2 is not there, when WALPE_WEB_INPUT=js,
 * or when the page reports no trusted event after injected ones (the page's
 * user script posts 'input' on the first trusted pointer event it sees).
 * WALPE_WEB_INPUT=native keeps the native path whatever happens,
 * WALPE_WEB_INPUT=off delivers nothing.
 *
 * Because the page now gets real input, things WebKit would do with it on a
 * normal window are stopped here: no context menu, <select> popups, file /
 * colour choosers or print dialogs (they would open as real windows and
 * grab the pointer), no native drag-and-drop, no text selection outside
 * form fields (it would take the PRIMARY selection), no window fullscreen,
 * the wheel does not scroll the document as a whole, and the main frame
 * cannot navigate away from the wallpaper (see the navigation guard below). */
#include "walpe-build-config.h"
#include "webinput.h"
#include <gdk/x11/gdkx.h>
#include <libsoup/soup.h>
#include <X11/Xlib.h>
#ifdef HAVE_XI2
#include <X11/extensions/XInput2.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <math.h>

enum { WI_NATIVE, WI_JS, WI_OFF };

struct WebInput {
    GtkWidget *win;
    WebKitWebView *view;
    char *label;            /* wallpaper path (logs) */
    char *base;             /* URI prefix of the wallpaper's own files */
    int mode;
    gboolean forced;        /* WALPE_WEB_INPUT chose the mode: no fallback */
    gboolean acked;         /* the page saw a trusted pointer event */
    guint unacked;          /* native events sent before the first ack */
    gint64 unacked_since;
    gboolean inside;        /* the page has the pointer */
    double x, y;            /* last position (page px) */
    guint buttons;          /* bit n = X button n is held in the page */
    gboolean nav_blocked;   /* a main-frame load out of the wallpaper ended */
    char *click_uri;        /* last external link clicked (WALPE_WEB_OPEN_LINKS) */
    gint64 click_us;
};

/* ------------------------------------------------------------ page script
 * document start, every frame. 'input' ack; no native drag (GDK would grab
 * the real pointer for it) and no selection outside form fields; top frame
 * only: no wheel scrolling of the document itself, --mouse-x/--mouse-y
 * (0..1, as before) and the fallback dispatcher
 * __walpeIn(kind, x, y, button, buttons, dx, dy) with DOM button numbers;
 * kind = move / leave / down / up / cancel (a release without click) / wheel. */
static const char *INPUT_JS =
    "(function(){\n"
    " var sent=false;\n"
    " function ack(e){if(sent||!e.isTrusted)return;sent=true;\n"
    "  try{window.webkit.messageHandlers.walpe.postMessage('input');}catch(x){}}\n"
    " ['mousemove','mousedown','wheel'].forEach(function(t){window.addEventListener(t,ack,true);});\n"
    " window.addEventListener('dragstart',function(e){e.preventDefault();},true);\n"
    " window.addEventListener('selectstart',function(e){var t=e.target;if(t&&t.nodeType!==1)t=t.parentElement;\n"
    "  if(t&&t.closest&&t.closest('input,textarea,select,[contenteditable]'))return;e.preventDefault();},true);\n"
    " if(window.top!==window)return;\n"
    /* the wheel scrolls what is under the pointer, but never the page as a
     * whole: under WE the wheel does not reach a wallpaper, so pages a few
     * pixels (or screens) taller than the view were never meant to move */
    " window.addEventListener('wheel',function(e){if(!e.isTrusted||e.defaultPrevented)return;\n"
    "  var n=e.target,s,dy=e.deltaY,dx=e.deltaX;if(n&&n.nodeType!==1)n=n.parentElement;\n"
    "  for(;n&&n!==document.body&&n!==document.documentElement;n=n.parentElement){s=getComputedStyle(n);\n"
    "   if(dy&&n.scrollHeight>n.clientHeight&&/auto|scroll/.test(s.overflowY)&&\n"
    "    (dy<0?n.scrollTop>0:n.scrollTop+n.clientHeight<n.scrollHeight-1))return;\n"
    "   if(dx&&n.scrollWidth>n.clientWidth&&/auto|scroll/.test(s.overflowX)&&\n"
    "    (dx<0?n.scrollLeft>0:n.scrollLeft+n.clientWidth<n.scrollWidth-1))return;}\n"
    "  e.preventDefault();},{capture:true,passive:false});\n"
    " function mv(x,y){var r=document.documentElement;if(!r||!window.innerWidth||!window.innerHeight)return;\n"
    "  r.style.setProperty('--mouse-x',(x/window.innerWidth).toFixed(4));\n"
    "  r.style.setProperty('--mouse-y',(y/window.innerHeight).toFixed(4));}\n"
    " window.addEventListener('mousemove',function(e){if(e.isTrusted)mv(e.clientX,e.clientY);},true);\n"
    " var over=null,dn={},lc={t:0,x:0,y:0,n:0,b:-1};\n"
    " function at(x,y){return document.elementFromPoint(x,y)||document.documentElement;}\n"
    " function ev(t,type,x,y,b,bs,ex){if(!t)return true;\n"
    "  var o={clientX:x,clientY:y,screenX:x,screenY:y,button:b,buttons:bs,bubbles:true,cancelable:true,\n"
    "   composed:true,view:window,detail:0},k,C=MouseEvent;\n"
    "  if(ex)for(k in ex)o[k]=ex[k];\n"
    "  if(type==='wheel')C=WheelEvent;\n"
    "  else if(type.indexOf('pointer')===0){if(!window.PointerEvent)return true;\n"
    "   C=PointerEvent;o.pointerId=1;o.pointerType='mouse';o.isPrimary=true;}\n"
    "  try{return t.dispatchEvent(new C(type,o));}catch(e){return true;}}\n"
    " window.__walpeIn=function(k,x,y,b,bs,dx,dy){\n"
    "  var t=k==='leave'?null:at(x,y),c,n;\n"
    "  if(k==='cancel'){c=dn[b]||over||at(x,y);dn[b]=null;ev(c,'pointerup',x,y,b,bs);ev(c,'mouseup',x,y,b,bs);return;}\n"
    "  if(t!==over){if(over){ev(over,'pointerout',x,y,0,bs);ev(over,'mouseout',x,y,0,bs);}\n"
    "   if(t){ev(t,'pointerover',x,y,0,bs);ev(t,'mouseover',x,y,0,bs);}over=t;}\n"
    "  if(k==='move'){mv(x,y);ev(t,'pointermove',x,y,0,bs);ev(t,'mousemove',x,y,0,bs);}\n"
    "  else if(k==='down'){n=Date.now();\n"
    "   lc.n=(lc.b===b&&n-lc.t<500&&Math.abs(x-lc.x)<6&&Math.abs(y-lc.y)<6)?lc.n+1:1;\n"
    "   lc.t=n;lc.x=x;lc.y=y;lc.b=b;dn[b]=t;\n"
    "   ev(t,'pointerdown',x,y,b,bs);\n"
    "   if(ev(t,'mousedown',x,y,b,bs,{detail:lc.n})&&b===0){c=t.closest?t.closest('a[href],button,input,select,textarea,[tabindex]'):null;\n"
    "    try{if(c)c.focus();else if(document.activeElement&&document.activeElement.blur)document.activeElement.blur();}catch(e){}}\n"
    "   if(b===2)ev(t,'contextmenu',x,y,b,bs);}\n"
    "  else if(k==='up'){ev(t,'pointerup',x,y,b,bs);ev(t,'mouseup',x,y,b,bs,{detail:lc.n});\n"
    "   c=dn[b];dn[b]=null;\n"
    "   if(c&&t){c=c===t||c.contains(t)?c:(t.contains(c)?t:null);}\n"
    "   if(c&&t){if(b===0){ev(c,'click',x,y,b,bs,{detail:lc.n});if(lc.n===2)ev(c,'dblclick',x,y,b,bs,{detail:2});}\n"
    "    else ev(c,'auxclick',x,y,b,bs,{detail:lc.n});}}\n"
    "  else if(k==='wheel'){if(ev(t,'wheel',x,y,0,bs,{deltaX:dx*100,deltaY:dy*100,deltaMode:0})){\n"
    "   for(c=t;c&&c!==document.body&&c!==document.documentElement;c=c.parentElement){n=getComputedStyle(c);\n"
    "    if(dy&&c.scrollHeight>c.clientHeight&&/auto|scroll/.test(n.overflowY)){c.scrollTop+=dy*100;break;}\n"
    "    if(dx&&c.scrollWidth>c.clientWidth&&/auto|scroll/.test(n.overflowX)){c.scrollLeft+=dx*100;break;}}}}\n"
    " };\n"
    "})();\n";

/* ------------------------------------------------------- native delivery */

#ifdef HAVE_XI2
static gint64 time_delta;   /* X server time - monotonic ms, from real events */

/* server time for an injected event: the real event's, else the monotonic
 * clock shifted to the last real one (Xorg's clock is the monotonic one) */
static guint32 wi_time (guint32 t)
{
    gint64 now = g_get_monotonic_time () / 1000;
    if (t)
        time_delta = (gint64) t - now;
    return t ? t : (guint32) (now + time_delta);
}

typedef struct {
    WebInput *wi;
    int evtype;             /* XI_Enter / _Leave / _Motion / _ButtonPress / _ButtonRelease */
    int detail;             /* button */
    double x, y;            /* page px */
    guint buttons;          /* held before the event (X semantics) */
    guint32 time;
} Inj;

static int xi_opcode = -1;      /* XInput major opcode on GDK's connection */
static int xi_pointer = -1;     /* GDK's core pointer device */
static GdkDisplay *inj_display; /* "xevent" is connected on it */
static GQueue inj_q = G_QUEUE_INIT;   /* Inj*: waiting for the batch before it */
static guint inj_inflight;      /* put back, not yet read by GDK */
static gint64 inj_put_us;
static guint inj_watchdog_id;

static void inj_flush (void);

static void inj_put (const Inj *j)
{
    WebInput *wi = j->wi;
    GdkSurface *surf = gtk_native_get_surface (GTK_NATIVE (wi->win));
    if (!surf || !GDK_IS_X11_SURFACE (surf))
        return;
    Display *xd = gdk_x11_display_get_xdisplay (gdk_surface_get_display (surf));
    Window xid = gdk_x11_surface_get_xid (surf);
    /* GDK divides event coordinates by the surface scale (GDK_SCALE) */
    double s = gdk_surface_get_scale_factor (surf);
    unsigned char bmask[4] = { 0 }, vmask[4] = { 0 };
    double noval = 0;
    for (int b = 1; b <= 7; b++)
        if (j->buttons & (1u << b))
            XISetMask (bmask, b);
    union { XIDeviceEvent d; XIEnterEvent c; } u;
    memset (&u, 0, sizeof u);
    if (j->evtype == XI_Enter || j->evtype == XI_Leave) {
        XIEnterEvent *c = &u.c;
        c->type = GenericEvent;
        c->serial = LastKnownRequestProcessed (xd);
        c->send_event = True;
        c->display = xd;
        c->extension = xi_opcode;
        c->evtype = j->evtype;
        c->time = j->time;
        c->deviceid = c->sourceid = xi_pointer;
        c->detail = XINotifyNonlinear;
        c->root = DefaultRootWindow (xd);
        c->event = xid;
        c->root_x = c->event_x = j->x * s;
        c->root_y = c->event_y = j->y * s;
        c->mode = XINotifyNormal;
        c->focus = False;
        c->same_screen = True;
        c->buttons.mask_len = sizeof bmask;
        c->buttons.mask = bmask;
    } else {
        XIDeviceEvent *d = &u.d;
        d->type = GenericEvent;
        d->serial = LastKnownRequestProcessed (xd);
        d->send_event = True;
        d->display = xd;
        d->extension = xi_opcode;
        d->evtype = j->evtype;
        d->time = j->time;
        d->deviceid = d->sourceid = xi_pointer;
        d->detail = j->detail;
        d->root = DefaultRootWindow (xd);
        d->event = xid;
        d->root_x = d->event_x = j->x * s;
        d->root_y = d->event_y = j->y * s;
        d->buttons.mask_len = sizeof bmask;
        d->buttons.mask = bmask;
        d->valuators.mask_len = sizeof vmask;   /* no axes: x/y only */
        d->valuators.mask = vmask;
        d->valuators.values = &noval;
    }
    XEvent ev;
    memset (&ev, 0, sizeof ev);
    ev.xcookie.type = GenericEvent;
    ev.xcookie.serial = LastKnownRequestProcessed (xd);
    ev.xcookie.send_event = True;       /* our mark (on_gdk_xevent): the server never sets it on XI2 events */
    ev.xcookie.display = xd;
    ev.xcookie.extension = xi_opcode;
    ev.xcookie.evtype = j->evtype;
    ev.xcookie.data = &u;               /* Xlib copies a claimed cookie (libXi's copy function) */
    XPutBackEvent (xd, &ev);
    inj_inflight++;
}

/* GDK read an X event: count ours off, then the next batch may go */
static gboolean on_gdk_xevent (GdkDisplay *d, gpointer xevent, gpointer user)
{
    (void) d; (void) user;
    const XEvent *e = xevent;
    if (inj_inflight && e->type == GenericEvent && e->xcookie.extension == xi_opcode &&
        e->xcookie.send_event && --inj_inflight == 0)
        inj_flush ();
    return FALSE;
}

static gboolean inj_watchdog (gpointer user)
{
    (void) user;
    inj_watchdog_id = 0;
    inj_flush ();
    return G_SOURCE_REMOVE;
}

/* XPutBackEvent pushes on the HEAD of Xlib's queue, so a batch goes in
 * last-first and only once GDK has read the one before it (a release put
 * back while its press was still queued would overtake it) */
static void inj_flush (void)
{
    if (inj_inflight) {
        if (g_get_monotonic_time () - inj_put_us < 300 * G_TIME_SPAN_MILLISECOND) {
            if (!inj_watchdog_id && !g_queue_is_empty (&inj_q))
                inj_watchdog_id = g_timeout_add (320, inj_watchdog, NULL);
            return;
        }
        /* GDK did not report them read (events paused, no "xevent"?) */
        g_debug ("web input: %u injected events unaccounted for, continuing", inj_inflight);
        inj_inflight = 0;
    }
    if (g_queue_is_empty (&inj_q))
        return;
    for (GList *l = inj_q.tail; l; l = l->prev)
        inj_put (l->data);
    g_queue_clear_full (&inj_q, g_free);
    inj_put_us = g_get_monotonic_time ();
}

static void inj_add (WebInput *wi, int evtype, int detail, double x, double y, guint buttons,
                     guint32 time)
{
    Inj *last = g_queue_peek_tail (&inj_q);
    if (last && last->wi == wi && last->evtype == XI_Motion && evtype == XI_Motion) {
        last->x = x;            /* still waiting: only the newest position matters */
        last->y = y;
        last->time = time;
        return;
    }
    if (inj_q.length >= 256)    /* GDK is not reading: do not pile up */
        return;
    Inj *j = g_new0 (Inj, 1);
    j->wi = wi;
    j->evtype = evtype;
    j->detail = detail;
    j->x = x;
    j->y = y;
    j->buttons = buttons;
    j->time = time;
    g_queue_push_tail (&inj_q, j);
}

/* can this page take injected XInput2 events? */
static gboolean native_setup (WebInput *wi)
{
    GdkDisplay *dpy = gtk_widget_get_display (wi->win);
    if (!GDK_IS_X11_DISPLAY (dpy))
        return FALSE;
    if (inj_display == dpy)
        return xi_opcode >= 0 && xi_pointer >= 0;
    Display *xd = gdk_x11_display_get_xdisplay (dpy);
    int ev, err;
    GdkSeat *seat = gdk_display_get_default_seat (dpy);
    GdkDevice *ptr = seat ? gdk_seat_get_pointer (seat) : NULL;
    if (!XQueryExtension (xd, "XInputExtension", &xi_opcode, &ev, &err) ||
        !ptr || !GDK_IS_X11_DEVICE_XI2 (ptr)) {
        xi_opcode = -1;
        return FALSE;
    }
    xi_pointer = gdk_x11_device_get_id (ptr);
    inj_display = dpy;
    g_signal_connect (dpy, "xevent", G_CALLBACK (on_gdk_xevent), NULL);
    return TRUE;
}

static void native_forget (WebInput *wi)
{
    for (GList *l = inj_q.head; l;) {
        GList *n = l->next;
        if (((Inj *) l->data)->wi == wi) {
            g_free (l->data);
            g_queue_delete_link (&inj_q, l);
        }
        l = n;
    }
}
#else
static gboolean native_setup (WebInput *wi) { (void) wi; return FALSE; }
static void native_forget (WebInput *wi) { (void) wi; }
#endif

/* ----------------------------------------------------------- script path */

static void js_send (WebInput *wi, const char *kind, double x, double y, int xbutton, int dx, int dy)
{
    /* DOM numbering: button 0 left / 1 middle / 2 right, buttons 1 / 4 / 2 */
    int b = xbutton == 2 ? 1 : xbutton == 3 ? 2 : 0;
    int bs = (wi->buttons & 2 ? 1 : 0) | (wi->buttons & 4 ? 4 : 0) | (wi->buttons & 8 ? 2 : 0);
    char js[160];
    g_snprintf (js, sizeof js, "window.__walpeIn&&window.__walpeIn('%s',%d,%d,%d,%d,%d,%d);", kind,
                (int) lround (x), (int) lround (y), b, bs, dx, dy);
    webkit_web_view_evaluate_javascript (wi->view, js, -1, NULL, NULL, NULL, NULL, NULL);
}

/* ------------------------------------------------------------- the events */

/* the page holding a button (a drag), or NULL. While one does, no other
 * page gets pointer events: like the X server's implicit grab, and GDK
 * keeps one of its own from an injected press to its release - it rewrites
 * every other window's pointer events to the pressed one at "nowhere",
 * which ended the drag there */
static WebInput *wi_grab;

static gboolean wi_blocked (WebInput *wi)
{
    return !wi || wi->mode == WI_OFF || (wi_grab && wi_grab != wi);
}

static void wi_set_buttons (WebInput *wi, guint buttons)
{
    wi->buttons = buttons;
    if (buttons)
        wi_grab = wi;
    else if (wi_grab == wi)
        wi_grab = NULL;
}

#ifdef HAVE_XI2
/* native events went out and the page has not confirmed any: after 40 of
 * them over 3 s the script path takes over (an ack switches back) */
static void wi_done (WebInput *wi)
{
    inj_flush ();
    if (wi->acked || wi->forced)
        return;
    gint64 now = g_get_monotonic_time ();
    if (!wi->unacked++)
        wi->unacked_since = now;
    if (wi->unacked >= 40 && now - wi->unacked_since > 3 * G_TIME_SPAN_SECOND) {
        g_warning ("web: %s got no injected pointer event (%u sent through GDK/XInput2); "
                   "using script-dispatched events instead", wi->label, wi->unacked);
        wi->mode = WI_JS;
        wi->inside = FALSE;
        native_forget (wi);
    }
}

static void wi_emit (WebInput *wi, int evtype, int detail, guint buttons, guint32 time)
{
    /* the middle button exists for the script path only (web_input_button) */
    inj_add (wi, evtype, detail, wi->x, wi->y, buttons & ~(1u << 2), time);
}

/* put the pointer at x, y (enter first when it was outside) */
static void wi_goto (WebInput *wi, double x, double y, guint32 time)
{
    gboolean moved = !wi->inside || fabs (x - wi->x) >= 0.01 || fabs (y - wi->y) >= 0.01;
    wi->x = x;
    wi->y = y;
    if (!wi->inside) {
        wi->inside = TRUE;
        wi_emit (wi, XI_Enter, 0, wi->buttons, time);
    }
    if (moved)
        wi_emit (wi, XI_Motion, 0, wi->buttons, time);
}
#endif

void web_input_refresh (WebInput *wi)
{
    if (wi)
        wi->x = wi->y = -1e9;   /* differs from any real position */
}

void web_input_move (WebInput *wi, double x, double y, guint32 time)
{
    if (wi_blocked (wi))
        return;
    if (wi->inside && fabs (x - wi->x) < 0.01 && fabs (y - wi->y) < 0.01)
        return;
    if (wi->mode == WI_JS) {
        wi->inside = TRUE;
        wi->x = x;
        wi->y = y;
        js_send (wi, "move", x, y, 0, 0, 0);
        return;
    }
#ifdef HAVE_XI2
    wi_goto (wi, x, y, wi_time (time));
    wi_done (wi);
#else
    (void) time;
#endif
}

void web_input_button (WebInput *wi, int button, gboolean down, double x, double y, guint32 time)
{
    if (wi_blocked (wi) || button < 1 || button > 3)
        return;
    guint bit = 1u << button;
    if (down == !!(wi->buttons & bit))
        return;     /* a second press, or a release of what was never pressed here */
    if (wi->mode == WI_JS) {
        if (!wi->inside || fabs (x - wi->x) >= 0.01 || fabs (y - wi->y) >= 0.01)
            js_send (wi, "move", x, y, 0, 0, 0);
        wi->inside = TRUE;
        wi->x = x;
        wi->y = y;
        wi_set_buttons (wi, down ? wi->buttons | bit : wi->buttons & ~bit);
        js_send (wi, down ? "down" : "up", x, y, button, 0, 0);
        return;
    }
    (void) time;
#ifdef HAVE_XI2
    time = wi_time (time);
    if (button == 2) {
        /* The middle button never reaches the page as a real event:
         * WebKit's default action for a real middle press is "paste the X
         * PRIMARY selection", so a page that makes itself an editable
         * field got whatever the user last selected in any application
         * with one middle click on the desktop. Script-dispatched events
         * (mousedown / mouseup / auxclick) have no default action. */
        wi_goto (wi, x, y, time);
        wi_done (wi);
        wi_set_buttons (wi, down ? wi->buttons | bit : wi->buttons & ~bit);
        js_send (wi, down ? "down" : "up", x, y, button, 0, 0);
        return;
    }
    wi_goto (wi, x, y, time);
    wi_emit (wi, down ? XI_ButtonPress : XI_ButtonRelease, button, wi->buttons, time);
    wi_set_buttons (wi, down ? wi->buttons | bit : wi->buttons & ~bit);
    wi_done (wi);
#endif
}

void web_input_scroll (WebInput *wi, int dx, int dy, double x, double y, guint32 time)
{
    if (wi_blocked (wi) || (!dx && !dy))
        return;
    if (wi->mode == WI_JS) {
        wi->inside = TRUE;
        wi->x = x;
        wi->y = y;
        js_send (wi, "wheel", x, y, 0, dx, dy);
        return;
    }
    (void) time;
#ifdef HAVE_XI2
    time = wi_time (time);
    wi_goto (wi, x, y, time);
    /* X wheel buttons: 4 up, 5 down, 6 left, 7 right; GDK makes one
     * discrete scroll event of each press */
    for (int i = 0; i < MIN (abs (dy), 16); i++)
        wi_emit (wi, XI_ButtonPress, dy < 0 ? 4 : 5, wi->buttons, time);
    for (int i = 0; i < MIN (abs (dx), 16); i++)
        wi_emit (wi, XI_ButtonPress, dx < 0 ? 6 : 7, wi->buttons, time);
    wi_done (wi);
#endif
}

void web_input_leave (WebInput *wi, guint32 time)
{
    if (!wi || wi->mode == WI_OFF || (!wi->inside && !wi->buttons))
        return;
    if (wi->mode == WI_JS) {
        for (int b = 1; b <= 3; b++)
            if (wi->buttons & (1u << b)) {
                wi_set_buttons (wi, wi->buttons & ~(1u << b));
                js_send (wi, "cancel", wi->x, wi->y, b, 0, 0);  /* mouseup, no click */
            }
        js_send (wi, "leave", wi->x, wi->y, 0, 0, 0);
        wi->inside = FALSE;
        return;
    }
    (void) time;
#ifdef HAVE_XI2
    time = wi_time (time);
    for (int b = 1; b <= 3; b++)
        if (wi->buttons & (1u << b)) {
            if (b != 2)
                wi_emit (wi, XI_ButtonRelease, b, wi->buttons, time);
            wi_set_buttons (wi, wi->buttons & ~(1u << b));
            if (b == 2)     /* the middle button has no real events (web_input_button) */
                js_send (wi, "cancel", wi->x, wi->y, b, 0, 0);
        }
    if (wi->inside)
        wi_emit (wi, XI_Leave, 0, 0, time);
    wi->inside = FALSE;
    inj_flush ();
#endif
}

void web_input_cancel (WebInput *wi)
{
    if (!wi || wi->mode != WI_NATIVE)
        return;     /* the script path cancels in web_input_leave */
    guint held = wi->buttons;
    for (int b = 1; b <= 3; b++)
        if (b != 2 && (held & (1u << b))) {
            wi->buttons &= ~(1u << b);      /* the event's `buttons`: this one is up */
            js_send (wi, "cancel", wi->x, wi->y, b, 0, 0);
        }
    wi->buttons = held;     /* web_input_leave does the bookkeeping and the real releases */
}

void web_input_ack (WebInput *wi)
{
    if (!wi || wi->acked)
        return;
    wi->acked = TRUE;
    if (wi->mode == WI_JS && !wi->forced) {
        /* the injected events did arrive, only late */
        wi->mode = WI_NATIVE;
        wi->inside = FALSE;
        wi_set_buttons (wi, 0);
    }
    g_message ("web: pointer input reaches the page as real events (%s)", wi->label);
}

const char *web_input_mode_name (WebInput *wi)
{
    if (!wi || wi->mode == WI_OFF)
        return "off";
    if (wi->mode == WI_JS)
        return "js";
    return wi->acked ? "native" : "native (unconfirmed)";
}

/* ------------------------------------------------------ navigation guard
 * A click is real now, so a link in the page would navigate the wallpaper
 * away. The main frame may only load the wallpaper's own files; frames
 * (an embedded player, a weather widget) may load what they like. Which
 * frame a navigation is for only shows at the response
 * (webkit_response_policy_decision_is_main_frame_main_resource), so:
 *  - navigation action: schemes WebKit cannot load in the page (mailto:,
 *    steam:, ...) are refused outright (they would end in an error page);
 *  - response: a main-frame document from outside the wallpaper is
 *    refused; the page stays as it is;
 *  - new windows (target=_blank, window.open) are never created;
 *  - a failed main-frame load of a foreign URI shows no error page.
 * WALPE_WEB_OPEN_LINKS=1: an http(s) link the user clicked, and that was
 * refused, is handed to xdg-open instead (off by default: any desktop
 * click reaches the page, and a page can make a click follow any link). */

static gboolean uri_is_own (WebInput *wi, const char *uri)
{
    if (!uri || !*uri)
        return TRUE;
    if (g_str_has_prefix (uri, "about:"))
        return TRUE;
    return wi->base && g_str_has_prefix (uri, wi->base);
}

static gboolean uri_is_http (const char *uri)
{
    return uri && (!g_ascii_strncasecmp (uri, "http://", 7) || !g_ascii_strncasecmp (uri, "https://", 8));
}

static void open_external (WebInput *wi, const char *uri)
{
    static gint64 last_us;
    const char *e = g_getenv ("WALPE_WEB_OPEN_LINKS");
    gint64 now = g_get_monotonic_time ();
    if (!e || !*e || !strcmp (e, "0") || !uri_is_http (uri) || uri_is_own (wi, uri))
        return;
    if (last_us && now - last_us < 2 * G_TIME_SPAN_SECOND)
        return;
    last_us = now;
    const char *argv[] = { "xdg-open", uri, NULL };
    GError *err = NULL;
    if (g_spawn_async (NULL, (char **) argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                       NULL, NULL, NULL, &err)) {
        g_message ("web: opened %.300s with xdg-open (WALPE_WEB_OPEN_LINKS)", uri);
    } else {
        g_warning ("web: xdg-open failed: %s", err->message);
        g_error_free (err);
    }
}

/* the user clicked this external link (remembered until its response) */
static void note_click (WebInput *wi, WebKitNavigationAction *a, const char *uri)
{
    if (webkit_navigation_action_get_navigation_type (a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED &&
        webkit_navigation_action_is_user_gesture (a) && uri_is_http (uri)) {
        g_free (wi->click_uri);
        wi->click_uri = g_strdup (uri);
        wi->click_us = g_get_monotonic_time ();
    }
}

gboolean web_log_gate (int kind)
{
    static struct { gint64 since; guint n; } lim[WEB_LOG_KINDS];
    if (kind < 0 || kind >= WEB_LOG_KINDS)
        return TRUE;
    gint64 now = g_get_monotonic_time ();
    if (now - lim[kind].since > 60 * G_TIME_SPAN_SECOND) {
        if (lim[kind].n > WEB_LOG_BURST)
            g_message ("web: %u more messages of that kind in the last minute were not logged",
                       lim[kind].n - WEB_LOG_BURST);
        lim[kind].since = now;
        lim[kind].n = 0;
    }
    lim[kind].n++;
    if (lim[kind].n == WEB_LOG_BURST + 1)
        g_message ("web: the page keeps causing this message; further ones are counted, not logged");
    return lim[kind].n <= WEB_LOG_BURST;
}

char *web_log_text (const char *s, gsize max)
{
    char *c = g_strndup (s ? s : "", max);
    for (char *p = c; *p; p++)
        if ((guchar) *p < 0x20 || *p == 0x7f)
            *p = ' ';
    /* The page's own address holds its token, which is all that guards the
     * loopback file server (the folders the user picked for the wallpaper
     * included), and logs get read by others. The start-up line stopped
     * printing it in version 0.12; the lines for a refused download or frame
     * of the page's own files, and the console lines of
     * WALPE_WEB_CONSOLE, still did. A path segment of 32 hex digits is
     * starred out in everything that goes through here. */
    for (char *p = c; (p = strchr (p, '/')) != NULL; p++) {
        int n = 0;
        while (n < 33 && g_ascii_isxdigit (p[1 + n]))
            n++;
        if (n == 32 && (p[33] == '/' || p[33] == 0 || p[33] == ' ' || p[33] == '?' || p[33] == '#'))
            memset (p + 1, '*', 32);
    }
    char *v = g_utf8_make_valid (c, -1);
    g_free (c);
    return v;
}

static void refused (WebInput *wi, const char *what, const char *uri)
{
    if (web_log_gate (WEB_LOG_NAV)) {
        char *u = web_log_text (uri, 300);
        g_message ("web: %s refused, the wallpaper stays (%s): %s", what, wi->label, u);
        g_free (u);
    }
    if (wi->click_uri && uri && !strcmp (uri, wi->click_uri) &&
        g_get_monotonic_time () - wi->click_us < 10 * G_TIME_SPAN_SECOND)
        open_external (wi, uri);
    g_clear_pointer (&wi->click_uri, g_free);
}

static gboolean on_decide_policy (WebKitWebView *v, WebKitPolicyDecision *d,
                                  WebKitPolicyDecisionType type, gpointer user)
{
    (void) v;
    WebInput *wi = user;
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION (d);
        const char *uri = webkit_uri_request_get_uri (webkit_response_policy_decision_get_request (r));
        /* a response WebKit would turn into a download (an attachment, or
         * a type it cannot display), in any frame: dropped here, before a
         * download exists (web.c cancels the ones that start elsewhere) */
        WebKitURIResponse *resp = webkit_response_policy_decision_get_response (r);
        SoupMessageHeaders *hd = resp ? webkit_uri_response_get_http_headers (resp) : NULL;
        char *disp = NULL;
        gboolean attach = hd && soup_message_headers_get_content_disposition (hd, &disp, NULL) && disp &&
                          !g_ascii_strcasecmp (disp, "attachment");
        g_free (disp);
        if (attach || !webkit_response_policy_decision_is_mime_type_supported (r)) {
            webkit_policy_decision_ignore (d);
            if (web_log_gate (WEB_LOG_DOWNLOAD)) {  /* (a page can cause one per frame load) */
                char *u = web_log_text (uri, 300);
                g_message ("web: download refused (%s): %s", wi->label, u);
                g_free (u);
            }
            return TRUE;
        }
        if (!webkit_response_policy_decision_is_main_frame_main_resource (r) || uri_is_own (wi, uri))
            return FALSE;
        webkit_policy_decision_ignore (d);
        refused (wi, "navigation away from the wallpaper", uri);
        return TRUE;
    }
    WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action (
        WEBKIT_NAVIGATION_POLICY_DECISION (d));
    const char *uri = webkit_uri_request_get_uri (webkit_navigation_action_get_request (a));
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        webkit_policy_decision_ignore (d);
        note_click (wi, a, uri);
        refused (wi, "new window", uri);
        return TRUE;
    }
    if (uri_is_own (wi, uri))
        return FALSE;
    const char *sch = g_uri_peek_scheme (uri);
    if (uri_is_http (uri) || (sch && (!g_ascii_strcasecmp (sch, "blob") || !g_ascii_strcasecmp (sch, "data") ||
                                      !g_ascii_strcasecmp (sch, "walpe-web")))) {
        note_click (wi, a, uri);    /* a frame may; the main frame is stopped at the response */
        return FALSE;
    }
    webkit_policy_decision_ignore (d);
    refused (wi, "navigation", uri);
    return TRUE;
}

static GtkWidget *on_create (WebKitWebView *v, WebKitNavigationAction *a, gpointer user)
{
    (void) v;
    WebInput *wi = user;
    const char *uri = webkit_uri_request_get_uri (webkit_navigation_action_get_request (a));
    if (webkit_navigation_action_is_user_gesture (a) && uri_is_http (uri)) {
        g_free (wi->click_uri);
        wi->click_uri = g_strdup (uri);
        wi->click_us = g_get_monotonic_time ();
    }
    refused (wi, "new window", uri);
    return NULL;
}

static gboolean on_load_failed (WebKitWebView *v, WebKitLoadEvent e, const char *uri, GError *err,
                                gpointer user)
{
    (void) v; (void) e;
    WebInput *wi = user;
    if (uri_is_own (wi, uri))
        return FALSE;       /* the wallpaper itself: WebKit's error page says why */
    wi->nav_blocked = TRUE;
    if (!g_error_matches (err, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE))
        refused (wi, "navigation away from the wallpaper (it failed)", uri);
    return TRUE;            /* no error page over the wallpaper */
}

gboolean web_input_take_nav_blocked (WebInput *wi)
{
    gboolean b = wi && wi->nav_blocked;
    if (wi)
        wi->nav_blocked = FALSE;
    return b;
}

/* ------------------------------------------- no real windows for the page */

static gboolean on_context_menu (void)
{
    return TRUE;    /* the page had its contextmenu event; WebKit's menu is not shown */
}

/* a <select>: WebKit's popup would be a real window with a pointer grab.
 * A click steps to the next option instead (wrapping), like the wheel
 * does on a closed select */
static gboolean on_option_menu (WebKitWebView *v, WebKitOptionMenu *menu, GdkRectangle *r, gpointer user)
{
    (void) v; (void) r; (void) user;
    guint n = webkit_option_menu_get_n_items (menu), sel = n;
    for (guint i = 0; i < n && sel == n; i++)
        if (webkit_option_menu_item_is_selected (webkit_option_menu_get_item (menu, i)))
            sel = i;
    for (guint k = 1; k <= n; k++) {
        guint i = ((sel == n ? n - 1 : sel) + k) % n;
        WebKitOptionMenuItem *it = webkit_option_menu_get_item (menu, i);
        if (webkit_option_menu_item_is_enabled (it) && !webkit_option_menu_item_is_group_label (it)) {
            if (i != sel)
                webkit_option_menu_activate_item (menu, i);
            break;
        }
    }
    webkit_option_menu_close (menu);
    return TRUE;
}

static gboolean on_file_chooser (WebKitWebView *v, WebKitFileChooserRequest *req, gpointer user)
{
    (void) v; (void) user;
    webkit_file_chooser_request_cancel (req);
    return TRUE;
}

static gboolean on_color_chooser (WebKitWebView *v, WebKitColorChooserRequest *req, gpointer user)
{
    (void) v; (void) user;
    webkit_color_chooser_request_cancel (req);
    return TRUE;
}

static gboolean on_stop (void)
{
    return TRUE;    /* print dialog; the off-screen window going fullscreen */
}

/* A wallpaper gets no permission. WebKitGTK's default handler ALLOWS a
 * pointer-lock request: after one click on the desktop (a trusted user
 * gesture since desktop clicks reach the page) requestPointerLock() made
 * WebKit grab the real pointer on the daemon's X connection and warp it
 * back on every move - the mouse was stuck for every application until
 * the wallpaper was changed from the keyboard. Geolocation, camera /
 * microphone, notifications, clipboard, device info and the rest make no
 * sense for a desktop background either. */
static gboolean on_permission_request (WebKitWebView *v, WebKitPermissionRequest *rq, gpointer user)
{
    (void) v;
    WebInput *wi = user;
    if (web_log_gate (WEB_LOG_PERMISSION))  /* (a page can ask as often as it likes) */
        g_message ("web: %s refused (%s)", G_OBJECT_TYPE_NAME (rq), wi->label);
    webkit_permission_request_deny (rq);
    return TRUE;
}

/* --------------------------------------------------------------- lifecycle */

WebInput *web_input_new (GtkWidget *win, WebKitWebView *view, const char *label)
{
    WebInput *wi = g_new0 (WebInput, 1);
    wi->win = win;
    wi->view = view;
    wi->label = g_strdup (label ? label : "");
    const char *e = g_getenv ("WALPE_WEB_INPUT");
    gboolean native = native_setup (wi);
    if (e && (!strcmp (e, "off") || !strcmp (e, "0"))) {
        wi->mode = WI_OFF;
    } else if (e && !strcmp (e, "js")) {
        wi->mode = WI_JS;
        wi->forced = TRUE;
    } else {
        wi->mode = native ? WI_NATIVE : WI_JS;
        wi->forced = e && !strcmp (e, "native");
        if (!native)
            g_message ("web: no XInput2 on GDK's display: pointer input goes to the page as "
                       "script-dispatched events");
    }
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager (view);
    WebKitUserScript *us = webkit_user_script_new (INPUT_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                   WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                   NULL, NULL);
    webkit_user_content_manager_add_script (ucm, us);
    webkit_user_script_unref (us);
    g_signal_connect (view, "decide-policy", G_CALLBACK (on_decide_policy), wi);
    g_signal_connect (view, "create", G_CALLBACK (on_create), wi);
    g_signal_connect (view, "load-failed", G_CALLBACK (on_load_failed), wi);
    g_signal_connect (view, "context-menu", G_CALLBACK (on_context_menu), wi);
    g_signal_connect (view, "show-option-menu", G_CALLBACK (on_option_menu), wi);
    g_signal_connect (view, "run-file-chooser", G_CALLBACK (on_file_chooser), wi);
    g_signal_connect (view, "run-color-chooser", G_CALLBACK (on_color_chooser), wi);
    g_signal_connect (view, "print", G_CALLBACK (on_stop), wi);
    g_signal_connect (view, "enter-fullscreen", G_CALLBACK (on_stop), wi);
    g_signal_connect (view, "permission-request", G_CALLBACK (on_permission_request), wi);
    return wi;
}

void web_input_set_base (WebInput *wi, const char *base_uri)
{
    g_free (wi->base);
    wi->base = g_strdup (base_uri);
}

void web_input_free (WebInput *wi)
{
    if (!wi)
        return;
    native_forget (wi);
    wi_set_buttons (wi, 0);
    g_signal_handlers_disconnect_by_data (wi->view, wi);
    g_free (wi->label);
    g_free (wi->base);
    g_free (wi->click_uri);
    g_free (wi);
}
