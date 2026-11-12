/* wl_gnome.h - GNOME Shell on Wayland: what the daemon and the WalPE
 * extension for GNOME Shell (contrib/gnome) agree on, and the parts of it
 * that need no connection (tests/wl_gnome_test.c).
 *
 * GNOME's compositor (Mutter) has no layer shell, and no other protocol
 * with which a client could put a surface on the desktop. What it has is
 * the window type DESKTOP, which only the shell can give a window: such a
 * window lies below every other one, is on all workspaces and is left out
 * of the overview, the window switcher and the dash. The extension gives
 * that type to the windows the daemon marks as wallpapers, and nothing of
 * it is built, loaded or asked on any other desktop.
 *
 * How the two find each other: the extension owns the name
 * io.github.walpe.Gnome on the session bus while it is enabled. The
 * daemon asks there (method Hello) in one situation only: a Wayland
 * compositor that answers, has no layer shell and looks like GNOME's -
 * the situation that ended with "no layer shell" before. Nothing is
 * installed for the daemon, nothing is started by the extension, no
 * setting has to be kept in step.
 *
 * What the daemon then does differently (platform/wayland.c):
 *   - each wallpaper is an xdg_toplevel instead of a layer surface, with
 *     the application id and a title naming its output, see below; the
 *     extension places it on that monitor at the monitor's size, and the
 *     daemon follows the size as with any configure;
 *   - GNOME Shell 49 and newer can retype any window. 46 to 48 can do it
 *     only for windows of a Wayland client the shell has started itself.
 *     There (Hello says "socket") the daemon asks the extension for its
 *     connection to the compositor: it hands over one end of a socket
 *     pair (method Socket), the shell starts the daemon's own program a
 *     second time with the option below, and that process sends the
 *     connection it was started with (WAYLAND_SOCKET) through the socket
 *     pair and then only waits for the pair to be closed - which is when
 *     the daemon ends or restarts in place. The compositor takes a client
 *     for the shell's own for as long as the process it started lives.
 *
 * The windows of other programs (the pause rules) are reported by the
 * extension to the provider "gnome", src/signals/gnome.c. */
#pragma once
#include <glib.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WL_GNOME_EXT_UUID "walpe@walpe.github.io"
#define WL_GNOME_BUS_NAME "io.github.walpe.Gnome"
#define WL_GNOME_OBJECT "/io/github/walpe/Gnome"
#define WL_GNOME_IFACE "io.github.walpe.Gnome"
/* the version of the three above this daemon speaks */
#define WL_GNOME_PROTOCOL 1u
/* a wallpaper window: xdg_toplevel.set_app_id and the start of its title;
 * the rest of the title is the output's name (wl_output.name, "DP-1") */
#define WL_GNOME_APP_ID "io.github.walpe.wallpaper"
#define WL_GNOME_TITLE_PREFIX "WalPE wallpaper: "
/* walpe-daemon --wayland-socket-to N: the process the shell starts */
#define WL_GNOME_DONOR_OPTION "--wayland-socket-to"

/* does this look like GNOME's compositor? gtk_shell: a global Mutter
 * announces for GTK (and nobody else does); desktop: XDG_CURRENT_DESKTOP */
static inline gboolean wl_gnome_session (gboolean gtk_shell, const char *desktop)
{
    return gtk_shell || (desktop && (strstr (desktop, "GNOME") || strstr (desktop, "gnome")));
}

/* the title of the wallpaper window on an output (g_free) */
static inline char *wl_gnome_title (const char *output_name)
{
    return g_strconcat (WL_GNOME_TITLE_PREFIX, output_name && *output_name ? output_name : "?", NULL);
}

/* ---- the extension, as far as the daemon can tell */
typedef enum {
    WL_GNOME_EXT_NONE = 0,      /* no file of it in the data directories */
    WL_GNOME_EXT_SILENT,        /* installed; nobody owns the name on the bus */
    WL_GNOME_EXT_OTHER,         /* it answers, with another version of the interface */
    WL_GNOME_EXT_ACTIVE,
} WlGnomeExt;

static inline const char *wl_gnome_ext_name (WlGnomeExt e)
{
    return e == WL_GNOME_EXT_ACTIVE   ? "active"
           : e == WL_GNOME_EXT_OTHER  ? "active, but another version than this walpe-daemon's"
           : e == WL_GNOME_EXT_SILENT ? "installed, not active"
                                      : "not installed";
}

/* The sentence that follows "no layer shell" on GNOME (static text). */
static inline const char *wl_gnome_ext_hint (WlGnomeExt e)
{
    switch (e) {
    case WL_GNOME_EXT_ACTIVE:
        return NULL;
    case WL_GNOME_EXT_OTHER:
        return "The WalPE extension for GNOME Shell (" WL_GNOME_EXT_UUID ") answers, but it is "
               "another version than this walpe-daemon's: install the one that came with this "
               "WalPE (contrib/gnome/install.sh install) and log out and in";
    case WL_GNOME_EXT_SILENT:
        return "The WalPE extension for GNOME Shell (" WL_GNOME_EXT_UUID ") is installed but not "
               "active: it is switched off, GNOME Shell has not been restarted since it was "
               "installed (log out and in), or it does not list this version of GNOME Shell. "
               "`gnome-extensions enable " WL_GNOME_EXT_UUID "` switches it on, "
               "`contrib/gnome/install.sh status` says which of the three it is; then start "
               "walpe-daemon again";
    default:
        return "With the WalPE extension for GNOME Shell (46 or newer) the wallpaper is shown on "
               "GNOME's Wayland session: `contrib/gnome/install.sh install` in WalPE's source "
               "tree, log out and in, start walpe-daemon again (docs/WAYLAND.md, \"GNOME\")";
    }
}

/* ---- Hello */
typedef struct {
    guint protocol;
    gboolean socket;            /* the connection has to be asked for (GNOME Shell 46 to 48) */
    char shell[24];             /* "46.0" */
    char extension[24];         /* its version as its metadata says, "" = not told */
} WlGnomeHello;

static inline void wl_gnome_copy (char *dst, gsize n, const char *src)
{
    gsize i = 0;
    /* (for the log and status: printable ASCII only, whatever the bus sent) */
    for (; src && src[i] && i + 1 < n; i++)
        dst[i] = src[i] >= 0x20 && src[i] < 0x7f ? src[i] : '?';
    dst[i] = 0;
}

/* about: the a{sv} of the answer. FALSE: not an answer of the extension. */
static inline gboolean wl_gnome_hello_parse (GVariant *about, WlGnomeHello *out)
{
    memset (out, 0, sizeof *out);
    if (!about || !g_variant_is_of_type (about, G_VARIANT_TYPE_VARDICT))
        return FALSE;
    const char *s = NULL;
    if (!g_variant_lookup (about, "protocol", "u", &out->protocol) || out->protocol == 0)
        return FALSE;
    g_variant_lookup (about, "socket", "b", &out->socket);
    if (g_variant_lookup (about, "shell", "&s", &s))
        wl_gnome_copy (out->shell, sizeof out->shell, s);
    if (g_variant_lookup (about, "extension", "&s", &s))
        wl_gnome_copy (out->extension, sizeof out->extension, s);
    return TRUE;
}

/* what the start-up log and `status` say about the mode (g_free) */
static inline char *wl_gnome_mode_text (const WlGnomeHello *h, gboolean handed)
{
    return g_strdup_printf (
        "GNOME Shell %s with the WalPE extension%s%s: each wallpaper is an ordinary window that "
        "the extension makes a desktop window (below every other window, on all workspaces, not "
        "in the overview's window list)%s",
        *h->shell ? h->shell : "?", *h->extension ? " " : "", h->extension,
        handed ? "; the connection to the compositor was handed over by the shell, which can do "
                 "that only for a client it started (GNOME Shell 46 to 48)"
               : "");
}

/* ---- one descriptor through a socket pair
 * Sent with one byte of data; the receiver's copy is close-on-exec. */
static inline gboolean wl_gnome_send_fd (int sock, int fd)
{
    char byte = 'w';
    struct iovec io = { .iov_base = &byte, .iov_len = 1 };
    union {
        char buf[CMSG_SPACE (sizeof (int))];
        struct cmsghdr align;
    } u;
    memset (&u, 0, sizeof u);
    struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.buf,
                          .msg_controllen = sizeof u.buf };
    struct cmsghdr *c = CMSG_FIRSTHDR (&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN (sizeof (int));
    memcpy (CMSG_DATA (c), &fd, sizeof (int));
    ssize_t r;
    do
        r = sendmsg (sock, &msg, MSG_NOSIGNAL);
    while (r < 0 && errno == EINTR);
    return r == 1;
}

/* -1: nothing came within timeout_ms, the other end closed, or what came
 * carried no descriptor */
static inline int wl_gnome_recv_fd (int sock, int timeout_ms)
{
    gint64 end = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
    for (;;) {
        gint64 left = (end - g_get_monotonic_time ()) / 1000;
        struct pollfd p = { .fd = sock, .events = POLLIN };
        int n = poll (&p, 1, (int) MAX (left, 0));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        break;
    }
    char byte;
    struct iovec io = { .iov_base = &byte, .iov_len = 1 };
    union {
        char buf[CMSG_SPACE (sizeof (int))];
        struct cmsghdr align;
    } u;
    memset (&u, 0, sizeof u);
    struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.buf,
                          .msg_controllen = sizeof u.buf };
    ssize_t r;
    do
        r = recvmsg (sock, &msg, MSG_CMSG_CLOEXEC);
    while (r < 0 && errno == EINTR);
    if (r != 1)
        return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR (&msg); c; c = CMSG_NXTHDR (&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
            c->cmsg_len >= CMSG_LEN (sizeof (int))) {
            int fd;
            memcpy (&fd, CMSG_DATA (c), sizeof (int));
            return fd;
        }
    return -1;
}

/* The process the shell starts (walpe-daemon --wayland-socket-to N): send
 * the connection named by WAYLAND_SOCKET through descriptor `to`, let go
 * of it, and stay until the other end of `to` is closed. Uses nothing of
 * the daemon and nothing that allocates; returns the exit status. */
static inline int wl_gnome_donor_run (const char *wayland_socket, const char *to)
{
    char *end = NULL;
    long wl = wayland_socket && *wayland_socket ? strtol (wayland_socket, &end, 10) : -1;
    if (wl < 0 || wl > 1 << 20 || (end && *end))
        return 2;
    end = NULL;
    long back = to && *to ? strtol (to, &end, 10) : -1;
    if (back < 0 || back > 1 << 20 || (end && *end) || back == wl)
        return 2;
    if (!wl_gnome_send_fd ((int) back, (int) wl))
        return 1;
    close ((int) wl);
    for (;;) {
        char buf[16];
        ssize_t r = read ((int) back, buf, sizeof buf);
        if (r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN))
            return 0;
        if (r < 0 && errno == EAGAIN) {
            struct pollfd p = { .fd = (int) back, .events = POLLIN };
            poll (&p, 1, -1);
        }
    }
}

/* ---- wl_gnome.c: the session bus. Main thread, synchronous, at start-up
 * only; the connection is let go again with walpe_wl_gnome_release (). */
/* Is the extension there? Looks for its files and asks on the bus; the
 * answer is kept for the rest of the process (walpe_wl_gnome_release
 * forgets it). *hello is filled for _ACTIVE and _OTHER. WALPE_GNOME_EXT=0
 * in the environment: not asked, _NONE. */
WlGnomeExt walpe_wl_gnome_find (WlGnomeHello *hello);
/* GNOME Shell 46 to 48: have the shell start the process that hands its
 * connection over, and take it. The descriptor of a connection to the
 * compositor (for wl_display_connect_to_fd), -1 and *why (g_free) when
 * that failed. The daemon's end of the socket pair stays open until the
 * process ends or executes another image. */
int walpe_wl_gnome_socket (char **why);
void walpe_wl_gnome_release (void);
/* tests: the bus to ask instead of the session bus (an address), and the
 * directory to look for the extension's files in instead of the data
 * directories; NULL = as usual */
void walpe_wl_gnome_use (const char *address, const char *data_dir);
