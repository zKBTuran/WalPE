/* wl_session.h - the Wayland connection the pause providers share
 * (internal to src/signals; see wl_session.c). Main thread only. */
#pragma once
#include <glib.h>
#include <wayland-client.h>
#include "signals/wlplan.h"

typedef struct {
    struct wl_output *proxy;
    guint32 global;         /* its name in the registry */
    guint32 version;        /* the version bound */
    char *name;             /* wl_output.name ("DP-1"); NULL = the compositor gave none */
    gboolean done;          /* its first description is complete */
    guint32 id;             /* the daemon's id for this output, 0 = not one it shows on */
} WlSessOutput;

typedef struct {
    /* an output came, went, or its name / daemon id changed */
    void (*outputs_changed) (gpointer user);
    /* the connection ended (compositor gone, protocol error): every
     * proxy is dead; destroy them, report nothing as on */
    void (*lost) (gpointer user);
    gpointer user;
} WlSessUser;

typedef struct WlSession WlSession;

/* The session, connected on first use (one round trip for the globals,
 * one for what they announce). NULL: *why (g_free) says what failed. */
WlSession *walpe_wlsess_ref (char **why);
void walpe_wlsess_unref (WlSession *s);

gboolean walpe_wlsess_alive (WlSession *s);
const WalpeWlCaps *walpe_wlsess_caps (WlSession *s);
struct wl_seat *walpe_wlsess_seat (WlSession *s);       /* NULL = none */
/* Bind the global of this interface the registry announced (the first
 * one), at min (its version, max_version). NULL = not offered. */
void *walpe_wlsess_bind (WlSession *s, const struct wl_interface *iface, guint32 max_version,
                         guint32 *bound_version);
/* WlSessOutput *, owned by the session */
GPtrArray *walpe_wlsess_outputs (WlSession *s);
/* the session's record of one of its own wl_output proxies; NULL for a
 * proxy it does not know */
WlSessOutput *walpe_wlsess_output_of (WlSession *s, struct wl_output *proxy);
/* the struct must stay valid until removed */
void walpe_wlsess_add_user (WlSession *s, const WlSessUser *u);
void walpe_wlsess_remove_user (WlSession *s, const WlSessUser *u);
/* send what is queued (after requests made outside a callback) */
void walpe_wlsess_flush (WlSession *s);
/* wait until the compositor has answered everything sent so far */
gboolean walpe_wlsess_roundtrip (WlSession *s);
/* the daemon's output list changed: look every id up again */
void walpe_wlsess_relookup (WlSession *s);
