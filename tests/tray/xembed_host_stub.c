/* xembed_host_stub - a tiny XEmbed system tray for testing walpe-tray
 *
 * Behaves like dwm's systray patch: a bar window at the top right that
 * owns _NET_SYSTEM_TRAY_S0, sets _NET_SYSTEM_TRAY_ORIENTATION (and with
 * --argb a 32-bit _NET_SYSTEM_TRAY_VISUAL), reparents docked icons, sets
 * their background pixel to the bar colour and sizes them to the bar
 * height. Exits after --seconds (a "tray restart" when run again).
 *
 *   xembed-host-stub [--size N] [--argb] [--vertical] [--bg RRGGBB] [--x X]
 *                    [--seconds N]
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>

int main (int argc, char **argv)
{
    int size = 24, argb = 0, vertical = 0, seconds = 10, bx = -1;
    unsigned long bg = 0x222222;
    for (int i = 1; i < argc; i++) {
        if (!strcmp (argv[i], "--size") && i + 1 < argc) size = atoi (argv[++i]);
        else if (!strcmp (argv[i], "--argb")) argb = 1;
        else if (!strcmp (argv[i], "--vertical")) vertical = 1;
        else if (!strcmp (argv[i], "--bg") && i + 1 < argc) bg = strtoul (argv[++i], NULL, 16);
        else if (!strcmp (argv[i], "--x") && i + 1 < argc) bx = atoi (argv[++i]);
        else if (!strcmp (argv[i], "--seconds") && i + 1 < argc) seconds = atoi (argv[++i]);
    }
    Display *d = XOpenDisplay (NULL);
    if (!d)
        return 2;
    int scr = DefaultScreen (d);
    Window root = RootWindow (d, scr);
    int sw = DisplayWidth (d, scr);
    int bw = vertical ? size + 8 : 200, bh = vertical ? 200 : size + 8;
    if (bx < 0)
        bx = sw - bw;
    XSetWindowAttributes a = { 0 };
    a.background_pixel = bg;
    a.override_redirect = True;
    a.event_mask = SubstructureNotifyMask;
    Window bar = XCreateWindow (d, root, bx, 0, bw, bh, 0, CopyFromParent, InputOutput, CopyFromParent,
                                CWBackPixel | CWOverrideRedirect | CWEventMask, &a);
    XMapRaised (d, bar);

    Atom sel = XInternAtom (d, "_NET_SYSTEM_TRAY_S0", False);
    Atom opcode = XInternAtom (d, "_NET_SYSTEM_TRAY_OPCODE", False);
    Atom manager = XInternAtom (d, "MANAGER", False);
    Atom orient = XInternAtom (d, "_NET_SYSTEM_TRAY_ORIENTATION", False);
    Atom visual = XInternAtom (d, "_NET_SYSTEM_TRAY_VISUAL", False);
    long o = vertical ? 1 : 0;
    XChangeProperty (d, bar, orient, XA_CARDINAL, 32, PropModeReplace, (unsigned char *) &o, 1);
    if (argb) {
        XVisualInfo vi;
        if (XMatchVisualInfo (d, scr, 32, TrueColor, &vi)) {
            long v = vi.visualid;
            XChangeProperty (d, bar, visual, XA_VISUALID, 32, PropModeReplace, (unsigned char *) &v, 1);
            printf ("host: ARGB visual 0x%lx\n", v);
        } else {
            printf ("host: no 32-bit visual\n");
        }
    }
    XSetSelectionOwner (d, sel, bar, CurrentTime);
    if (XGetSelectionOwner (d, sel) != bar) {
        printf ("host: selection busy\n");
        return 3;
    }
    XEvent m = { 0 };
    m.xclient.type = ClientMessage;
    m.xclient.window = root;
    m.xclient.message_type = manager;
    m.xclient.format = 32;
    m.xclient.data.l[0] = CurrentTime;
    m.xclient.data.l[1] = sel;
    m.xclient.data.l[2] = bar;
    XSendEvent (d, root, False, StructureNotifyMask, &m);
    XFlush (d);
    printf ("host: tray 0x%lx at %d,0 %dx%d, icons %dpx\n", bar, bx, bw, bh, size);
    fflush (stdout);

    int n = 0;
    time_t end = time (NULL) + seconds;
    while (time (NULL) < end) {
        while (XPending (d)) {
            XEvent e;
            XNextEvent (d, &e);
            if (e.type == ClientMessage && e.xclient.message_type == opcode && e.xclient.data.l[1] == 0) {
                Window icon = (Window) e.xclient.data.l[2];
                XWindowAttributes wa;
                XGetWindowAttributes (d, icon, &wa);
                printf ("host: dock request 0x%lx (depth %d)\n", icon, wa.depth);
                if (!argb || wa.depth != 32) {
                    XSetWindowAttributes ia = { .background_pixel = bg };
                    XChangeWindowAttributes (d, icon, CWBackPixel, &ia);
                }
                XAddToSaveSet (d, icon);
                XReparentWindow (d, icon, bar, vertical ? 4 : 4 + n * (size + 4), vertical ? 4 + n * (size + 4) : 4);
                XResizeWindow (d, icon, size, size);
                XMapRaised (d, icon);
                n++;
                fflush (stdout);
            }
        }
        fd_set f;
        FD_ZERO (&f);
        FD_SET (ConnectionNumber (d), &f);
        struct timeval tv = { 0, 100000 };
        select (ConnectionNumber (d) + 1, &f, NULL, NULL, &tv);
    }
    printf ("host: exiting\n");
    XCloseDisplay (d);
    return 0;
}
