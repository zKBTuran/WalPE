/* websec_selown.c - helper of tests/websec_test.sh: an X client that owns
 * PRIMARY and CLIPBOARD (as an application does in which the user has
 * selected and copied text), answers requests for them, and says when
 * either is taken away.
 *   websec-selown <seconds>
 * prints "OWN ...", "LOST <name> after <ms> ms" and at the end "KEPT <name>". */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
static double now_ms (void) { struct timeval t; gettimeofday (&t, 0); return t.tv_sec * 1e3 + t.tv_usec / 1e3; }
int main (int argc, char **argv)
{
    double secs = argc > 1 ? atof (argv[1]) : 10;
    Display *d = XOpenDisplay (NULL);
    if (!d) return 2;
    Window w = XCreateSimpleWindow (d, DefaultRootWindow (d), 0, 0, 1, 1, 0, 0, 0);
    Atom clip = XInternAtom (d, "CLIPBOARD", False), utf8 = XInternAtom (d, "UTF8_STRING", False);
    Atom targets = XInternAtom (d, "TARGETS", False);
    XSetSelectionOwner (d, XA_PRIMARY, w, CurrentTime);
    XSetSelectionOwner (d, clip, w, CurrentTime);
    XFlush (d);
    int have_p = XGetSelectionOwner (d, XA_PRIMARY) == w, have_c = XGetSelectionOwner (d, clip) == w;
    printf ("OWN primary=%d clipboard=%d\n", have_p, have_c); fflush (stdout);
    double t0 = now_ms ();
    const char *text = "USER-SELECTION";
    while (now_ms () - t0 < secs * 1e3) {
        while (XPending (d)) {
            XEvent e; XNextEvent (d, &e);
            if (e.type == SelectionClear) {
                const char *n = e.xselectionclear.selection == XA_PRIMARY ? "PRIMARY" : "CLIPBOARD";
                printf ("LOST %s after %.0f ms\n", n, now_ms () - t0); fflush (stdout);
                if (e.xselectionclear.selection == XA_PRIMARY) have_p = 0; else have_c = 0;
            } else if (e.type == SelectionRequest) {
                XSelectionRequestEvent *r = &e.xselectionrequest;
                XEvent s = {0};
                s.xselection.type = SelectionNotify; s.xselection.requestor = r->requestor;
                s.xselection.selection = r->selection; s.xselection.target = r->target;
                s.xselection.time = r->time; s.xselection.property = r->property ? r->property : r->target;
                if (r->target == targets) {
                    Atom t[2] = { utf8, XA_STRING };
                    XChangeProperty (d, r->requestor, s.xselection.property, XA_ATOM, 32, PropModeReplace, (unsigned char *) t, 2);
                } else if (r->target == utf8 || r->target == XA_STRING) {
                    XChangeProperty (d, r->requestor, s.xselection.property, r->target, 8, PropModeReplace, (const unsigned char *) text, (int) strlen (text));
                    printf ("SERVED %s to a requestor\n", r->selection == XA_PRIMARY ? "PRIMARY" : "CLIPBOARD"); fflush (stdout);
                } else s.xselection.property = None;
                XSendEvent (d, r->requestor, False, 0, &s);
            }
        }
        XFlush (d);
        struct timeval tv = { 0, 20000 }; select (0, 0, 0, 0, &tv);
    }
    if (have_p) printf ("KEPT PRIMARY\n");
    if (have_c) printf ("KEPT CLIPBOARD\n");
    printf ("owner now: primary=%s clipboard=%s\n", XGetSelectionOwner (d, XA_PRIMARY) == w ? "me" : XGetSelectionOwner (d, XA_PRIMARY) ? "other" : "none",
            XGetSelectionOwner (d, clip) == w ? "me" : XGetSelectionOwner (d, clip) ? "other" : "none");
    XCloseDisplay (d);
    return 0;
}
