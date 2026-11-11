/* webhost_wl.h - a private Wayland display for the web module's page
 * windows (part of walpe-web.so; see webhost_wl.c). */
#pragma once
#include <glib.h>

/* Bring the display up (once): a compositor thread and a socket only this
 * process can reach. TRUE: *socket_path (g_free) is what
 * gdk_display_open () takes; the display waits a while for GTK to connect
 * and then accepts nobody else. FALSE: *why (g_free) says what failed. */
gboolean walpe_webhost_start (char **socket_path, char **why);
/* GTK did not connect after all: stop waiting, drop the socket */
void walpe_webhost_abandon (void);
/* the display runs and GTK is connected to it */
gboolean walpe_webhost_running (void);

/* The size in pixels of the display's one output: GTK's monitor, and so
 * what a page reads as window.screen. Before walpe_webhost_start: the
 * size the output is announced with; while the display runs: a mode
 * change (GTK learns it on its next turn of the main loop). Never called:
 * 16384 x 16384, larger than any page. Main thread. */
void walpe_webhost_output_size (int w, int h);

/* ---- input for a page window. Main thread; nothing blocks. surface =
 * the object id of the window's wl_surface on GTK's connection
 * (wl_proxy_get_id), x / y in surface pixels, time in milliseconds on any
 * steady clock. A pointer is on at most one window at a time. */
void walpe_webhost_pointer_enter (guint32 surface, double x, double y);
void walpe_webhost_pointer_motion (guint32 time, double x, double y);
/* button: 1 left, 2 middle, 3 right */
void walpe_webhost_pointer_button (guint32 time, int button, gboolean down);
/* wheel clicks: dy > 0 = down, dx > 0 = right */
void walpe_webhost_pointer_wheel (guint32 time, int dx, int dy);
void walpe_webhost_pointer_leave (void);

/* ---- keyboard, as wl_keyboard carries it. keymap: an xkb keymap (text
 * format v1) in a file descriptor the callee takes over (-1 = none: GTK
 * keeps its built-in US layout). focus: the window that gets the keys,
 * 0 = none; a focused window is also told that it is active. */
void walpe_webhost_keymap (int fd, guint32 size);
void walpe_webhost_keyboard_focus (guint32 surface);
void walpe_webhost_key (guint32 time, guint32 evdev_key, gboolean down);
void walpe_webhost_modifiers (guint32 depressed, guint32 latched, guint32 locked, guint32 group);

/* numbers for status and tests (any thread) */
typedef struct {
    gboolean running;
    guint surfaces;         /* wl_surfaces alive */
    guint toplevels;        /* mapped page windows */
    guint64 commits;        /* wl_surface.commit seen */
    guint64 frames;         /* frame callbacks answered */
    guint64 selections;     /* clipboard / drag requests dropped */
    guint64 input_events;   /* pointer + keyboard events sent */
} WalpeWebhostStats;
void walpe_webhost_stats (WalpeWebhostStats *out);
