/* walpe-daemon: renders the active wallpaper on one "desktop" window per
 * monitor. X11-only, no GTK in the render path: each monitor gets an
 * override-redirect window (invisible to window managers — dwm cannot tile
 * or raise it) that is directly an EGL surface.
 *
 * Threading model: every monitor owns a dedicated EGL context and a render
 * thread that keeps that context current for the thread's entire lifetime —
 * EGL contexts must not migrate between threads (NVIDIA enforces this with
 * EGL_BAD_ACCESS, which silently broke every earlier design here). mpv/
 * ffmpeg push frames via request_redraw; scenes render at a capped fps;
 * static images render once. */
#include "walpe-build-config.h"
#include <glib.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <signal.h>
#include <json-glib/json-glib.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h> /* XGetClassHint */
#include <X11/extensions/shape.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/dpms.h>
#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>   /* output names of the monitors */
#endif
#ifdef HAVE_XI2
#include <X11/extensions/XInput2.h>
#endif
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/wait.h>
#include <poll.h>
#include <locale.h>
#include <math.h>
#include <malloc.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <sys/eventfd.h>

#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/scenenode_effects.h" /* fx_set_cache_enabled */
#include "engine/gpuprof.h"           /* GPUPROF: walpe-ctl profile */
#include "engine/weparticles.h"       /* particle settings */
#include "engine/modules.h"           /* walpe_modules_status */
#include "engine/mp4demux.h"          /* open: can this video be played? */
#include "common/config.h"
#include "common/ipc.h"
#include "common/util.h"
#include "common/import.h"
#include "common/library.h"   /* walpe_config_forget_path */
#include "common/audio.h"
#include "common/media.h"
#include "common/workshop.h"
#include "common/weassets.h"
#include "common/swrender.h"
#include "common/pace.h"
#include "common/restore.h"
#include "common/present.h"
#include "common/region.h"
#include "engine/clip.h"
#include "daemon/vistrack.h"
#ifdef HAVE_WEB
#include "engine/web.h"
#endif


/* pointer position, one XQueryPointer per tick, only while a visible
 * wallpaper follows the pointer AND the pointer moves: at the frame rate
 * (30..144 Hz). At the old fixed 30 Hz, parallax and cursor effects moved
 * in 30 steps a second however fast the scene rendered. */
#define MOUSE_POLL_MS mouse_poll_ms ()
static guint mouse_poll_ms (void);

typedef struct { int x, y, width, height; } WalpeRect;

typedef struct Mon {
    int index;
    Window xid;              /* override-redirect window = render target */
    EGLSurface surf;
    EGLContext ctx;          /* owned by the render thread */
    WalpeContent *content;   /* touched ONLY by the render thread */
    gint content_type;       /* WalpeType of content (atomic; main reads it) */
    char *load_path;         /* render-thread: pending wallpaper to load */
    gboolean render_req;     /* render-thread: present a frame */
    gboolean quitting;
    gboolean pause_wanted;   /* main -> render thread (under lock) */
    gboolean pause_applied;  /* render thread: last state given to content */
    GMutex lock;
    GCond cond;
    GThread *thread;
    WalpeRect geom;          /* monitor geometry (read-only after init) */
    float mx, my;            /* parallax mouse, written by main, read by thread */
    guint render_count;      /* rendered frames (diagnostics) */
    int last_w, last_h;      /* last rendered size (diagnostics) */
    gboolean covered;        /* main thread: client windows hide this monitor */
    gboolean focus_app;      /* main: the focused window is an application's, on this monitor */
    int fps_cap;             /* main: the frame rate limit last handed to the thread */
    gint64 last_swap_us;     /* WALPE_CADENCE: previous swap timestamp */
    /* presentation (present.h) */
    gint present_mode;       /* main: WalpePresentMode of this monitor, fixed after init */
    gint present_iv;         /* swap interval wanted (atomic: main sets it, the
                              * render thread applies it to its surface) */
    int present_iv_applied;  /* render thread: the interval its surface has (-1 = none yet) */
    gint64 paused_since_us;  /* render thread: paused since (0 = running) */
    gint idle_released;      /* idle_release: the paused wallpaper's GL objects were
                              * freed, it is loaded again on resume (atomic; main reads) */
    gboolean needs_first_frame; /* paint once even when covered/paused */
    /* render thread: a video's load is reported when its first picture is
     * there, or as failed when none has come in time (mon_first_frame_tick).
     * first_path: the video that is waited for (NULL = none); tag and keep:
     * that load's; ticks: quarter seconds waited so far */
    char *first_path;
    guint first_tag;
    gboolean first_keep;
    int first_ticks;
    gint64 first_tick_us;
    /* render thread: when to hand freed memory back to the system after a
     * load, an unload or a role change (0 = nothing pending). What a switch
     * frees is partly freed late: the driver runs texture deletes when it
     * next flushes, a scene frees its full-size textures when its first
     * frame has made the smaller copies, decoder threads end on their own.
     * A trim at the end of the load came before all of that, and the pages
     * then stayed resident until the next switch. */
    gint64 settle_us;
    int settle_waits;
    int fps;                 /* fps cap snapshot for the pending load */
    gboolean fps_changed;    /* main -> render thread: hand fps to the content */
    GArray *ptr_q;           /* PtrMsg: pointer events for the content (lock) */
    gint wants_pointer;      /* content has SceneScript cursor events (atomic) */
    gint uses_ptr_pos;       /* content draws with the pointer position (atomic) */
    gboolean ptr_pressed;    /* main: a desktop press went to this monitor */
    float ptr_lx, ptr_ly;    /* main: last move sent (-2 = none) */
    /* web wallpapers (main): pointer input handed to this monitor's page */
    guint web_btns;          /* bit n: X button n went down on its desktop */
    gboolean web_in;         /* the page has the pointer */
    int web_lx, web_ly;      /* last position sent (root px) */
    /* shared rendering (render_shared): one leader renders the scene and
     * publishes each frame; followers only present it */
    gboolean own_paused;     /* main: this monitor's own pause state */
    gboolean self_paused;    /* main -> leader thread (lock): own pause (the
                              * leader's pause_wanted = whole group paused) */
    struct Mon *leader;      /* main: following this monitor (NULL = none) */
    int nfollowers;          /* main: monitors following this one */
    gboolean role_change;    /* main -> thread (lock): adopt role_sf/role_idx */
    struct SharedFrame *role_sf;
    int role_idx;            /* -1 = lead role_sf, >= 0 = follower slot */
    guint role_gen;          /* D.wall_gen of the pending role */
    guint lead_gen;          /* render thread: wall_gen of the lead role */
    char *content_path;      /* render thread: the path its own content was loaded from */
    /* per-monitor wallpapers (version 0.11.1) */
    char *name;              /* output name (RandR monitor, e.g. "HDMI-0"; else "monN") */
    char *want_path;         /* main: the wallpaper this monitor shows ("" = none) */
    char *loaded_path;       /* main: what its thread loaded as its own content
                              * (NULL while following a leader or empty) */
    struct SharedFrame *lead_sf;   /* render thread: leading */
    struct SharedFrame *follow_sf; /* render thread: following */
    int follow_idx;
    GLuint sf_fbo[8];        /* render thread: FBOs on the shared slot textures */
    GLuint sf_fbo_tex[8];
    /* pacing (version 0.12) */
    double hz;               /* refresh rate (RandR; 0 = unknown), fixed after init */
    gint64 present_us;       /* render thread, following: last presentation */
    gint64 follow_wait_us;   /* render thread, following: a newer frame waits
                              * for this monitor's next refresh slot (0 = none) */
    gboolean ptr_dirty;      /* main -> thread (lock): parallax pointer moved */
    /* frame skipping (content->frame_unchanged), render thread: who holds
     * the frame the content drew last. A content that finds its picture
     * unchanged draws nothing; that is only right while the window (and,
     * for a leader, the shared slot) really show that frame. */
    gboolean win_has_frame;  /* this window presented it */
    gboolean win_exposed;    /* ... and lost it to an Expose (not yet drawn again) */
    gboolean sf_has_frame;   /* leading: it was published to the followers */
    gint exposed;            /* main -> thread (atomic): the X server exposed a
                              * part of the window (painted with the background
                              * where no compositor keeps the pixels): the frame
                              * has to be drawn again */
    guint expose_timer_id;   /* main: Expose events seen, expose_fire pending */
    char *load_error;        /* main: why its last load failed (NULL = it loaded) */
    /* a failed load brings the previous wallpaper back, once (load_ended) */
    char *good_path;         /* main: the last wallpaper it really showed: its own
                              * load, or its leader's, ended well (NULL = none) */
    char *reopen_for;        /* main: it was given good_path back after a failed
                              * load: that load's reason, which load_error goes
                              * on showing until it is given another wallpaper
                              * (NULL = no re-open; one is never answered with
                              * another) */
    char *restored;          /* main: that re-open ended well: its path (status
                              * "load_restored"; NULL = none) */
    gboolean load_keep;      /* main -> thread (lock), with load_path: if this load
                              * fails, leave the windows as they are - main answers
                              * with the previous wallpaper, or with blank_req */
    guint load_tag;          /* main -> thread (lock), with load_path: which load
                              * that is; its note carries it back (0 = none) */
    guint load_wait;         /* main: the tag of the load it waits for */
    gboolean loading;        /* main: that load has not reported yet (status
                              * "loading"; a setting that reloads scenes counts
                              * this monitor in: its type is not known yet) */
    gboolean reload_wanted;  /* main: load its wallpaper again at the next
                              * reload_flush () (scenes_reload_mark) */
    gboolean blank_req;      /* main -> thread (lock): that failure is final: black */
    gboolean prof_sync;      /* GPUPROF main -> thread (lock): call mon_prof_sync */
    guint fpslog_count;      /* main: render_count at the last WALPE_FPS_LOG line */
    /* "Adjust layers" (layers_ipc.h), main -> thread (lock) */
    gboolean force_frame;    /* draw one frame now, also while paused / covered */
    gint64 force_until_us;   /* following: present the leader's frames until then,
                              * also while paused */
    /* quality knobs (common/quality.h) */
    WalpeQuality quality;    /* main -> thread (lock): what its wallpaper runs with */
    int rt_render_scale;     /* render thread: quality.render_scale as last taken */
    gboolean quality_changed; /* main -> thread (lock): hand `quality` to the content */
    gint64 stats_until_us;   /* main -> thread (lock): somebody looks at the numbers
                              * until then (the content may count pixels) */
    gboolean describe_req;   /* main -> thread (lock): refresh `qdescribe` */
    gboolean stats_req;      /* main -> thread (lock): there are no numbers yet:
                              * hand them over now, not with the next frame */
    WalpeQualityStats qstats; /* thread -> main (lock): the content's numbers */
    gboolean qstats_ok;
    char *qdescribe;         /* thread -> main (lock): how the knobs apply */
    /* pacing between the threads, and numbers for `status` */
    gint wakeups;            /* render thread: times it came back from a wait (atomic) */
    gboolean clock_wait;     /* lock: the render thread sleeps until a time at which
                              * it draws / presents whatever happens - its content's
                              * next frame (which reads the pointer position
                              * anyway), a follower's next slot (which takes the
                              * newest frame anyway): a pointer move or a frame
                              * published meanwhile needs no wake-up */
    gint follow_free;        /* main -> follower thread (atomic): its leader never
                              * publishes more frames than this monitor refreshes:
                              * each one is presented when it arrives */
    gint follow_cap;         /* main -> follower thread (atomic): this monitor's own
                              * frame rate limit while it is below its leader's
                              * (the focus rule on this monitor); 0 = none */
    /* "Skip hidden areas" (daemon/vistrack.h, engine/clip.h); opt-in */
    WalpeRegion vis;         /* main: what other windows leave visible of this
                              * monitor (its own coordinates) */
    gboolean vis_known;      /* main: vis is tracked (else: everything) */
    gboolean vis_none;       /* main: nothing of it can be seen */
    int clip_n;              /* main -> thread (lock): boxes to render, -1 = all */
    WalpeBox clip[WALPE_CLIP_MAX];
    guint64 clip_serial;     /* lock: changes with clip[] */
    gboolean clip_grew;      /* main -> thread (lock): something became visible */
    guint64 follow_need;     /* main -> follower thread (lock): the oldest clip
                              * of its leader whose frames hold everything this
                              * monitor shows now */
    gboolean clip_owed;      /* render thread: a frame is owed for what became
                              * visible ... */
    gint64 clip_due_us;      /* ... and this is when it is drawn (0 = with the
                              * next frame that comes anyway) */
    gint64 last_draw_us;     /* render thread: when the last frame was started */
    gint64 grew_us;          /* main -> thread (lock): when the oldest part not
                              * drawn yet became visible (0 = none) */
    gint64 lat_last_us, lat_max_us, lat_sum_us;  /* lock: from there to the swap */
    guint lat_n;             /* ... of the frame that shows it */
    guint64 frames_done;     /* lock: frames this monitor has shown */
    guint64 grew_frames;     /* lock: frames_done when grew_us was set */
    guint lat_frames_last, lat_frames_max;   /* lock: frames shown from there up to
                              * and including the one with the uncovered pixels
                              * (1 = the very next one; 2 = one was being drawn
                              * when the window moved) */
} Mon;

typedef struct { int kind; float x, y; } PtrMsg;

static struct {
    WalpeConfig *cfg;
    GList *mons;
    Display *xd;             /* main thread: events, queries, the windows */
    Display *egl_xd;         /* EGL / the render threads (= xd with WALPE_ONE_XCONN) */
    Window root;
    EGLDisplay egl_dpy;
    EGLConfig egl_conf;
    gboolean fs_active;      /* fullscreen window has focus */
    gboolean asleep;         /* DPMS power-save or screensaver active */
    gboolean on_battery;     /* running on battery (followed only while a battery rule is on) */
    gboolean has_wm;         /* EWMH manager present (checked at startup) */
    gint egl_is_es;          /* set by monitor threads after ctx creation */
    Window watched_win;      /* active window we subscribed to */
    guint cover_timer_id;    /* debounced coverage recompute (main thread) */
    gboolean stack_dirty;    /* a window may have got below a wallpaper window */
    GHashTable *watched_clients; /* client windows with PropertyChangeMask selected */
    gboolean probe;          /* WALPE_PROBE (read once at startup) */
    gboolean cadence;        /* WALPE_CADENCE */
    GMainLoop *loop;
    guint wall_gen;          /* main: bumped by every apply_wallpapers */
    gint share_failed;       /* a monitor context is outside the share group */
    char *shared_note;       /* main: why the last scene was not shared */
    GHashTable *noshare;     /* main: wallpaper paths that cannot be shared (fallback) */
    double max_hz;           /* fastest monitor (0 = no rate known), fixed after init */
    gboolean no_refresh_cap; /* WALPE_NO_REFRESH_CAP: pace by the fps setting only */
    gboolean leader_first;   /* WALPE_LEADER=first: the old rule (A/B) */
    gboolean fake_vsync;     /* WALPE_FAKE_VSYNC: emulate each monitor's vblank */
    gulong load_delay_us;    /* WALPE_LOAD_DELAY_MS (tests): every load takes at least this */
    int first_frame_ticks;   /* WALPE_FIRST_FRAME_S in quarter seconds: how long a video
                              * may take for its first picture (0 = not waited for) */
    gboolean restart;        /* main: the monitor layout changed, re-exec after shutdown */
    double fpslog_s;         /* WALPE_FPS_LOG interval, 0 = off */
    gint64 fpslog_us;        /* main: time of the last line */
    WalpeVisTrack *vis;      /* main: "Skip hidden areas" tracker (NULL = off) */
    Mon **vis_mons;          /* main: the monitors it was given, in its order */
    gboolean windowed;       /* --windowed: a normal window, nothing is tracked */
} D;

/* "Render on CPU" (common/swrender.h): the daemon's GL comes from Mesa's
 * llvmpipe instead of the GPU driver. Decided once at start-up, before
 * the first EGL call; a change of the setting restarts the daemon in
 * place. The renderer itself knows nothing about it. */
static struct {
    gboolean wanted;        /* the setting: WALPE_SOFTWARE, else config software_render */
    gboolean env_forced;    /* WALPE_SOFTWARE decides, not the config */
    gboolean active;        /* this process runs on llvmpipe because of it */
    int threads;            /* llvmpipe rasteriser threads when active */
    char *note;             /* wanted but not available: why (shown by the GUI) */
    GMutex lock;            /* the three strings: set once by the first monitor thread */
    char *gl_vendor, *gl_renderer, *gl_version;
    char **orig_env;        /* environment at start, before anything was set */
    char **argv;
    char *exe;              /* this binary's path (the test child, the restart) */
    gboolean restart;       /* main loop left to restart in place */
    guint restart_id;
} Sw;
static void sw_note_gl (void);
static void sw_schedule_restart (void);

/* forward decls */
static void request_redraw (gpointer user);
#ifdef HAVE_WEB
static char *web_render_state;      /* status "web_render" (web_env_setup) */
static gboolean web_accel_restart (gpointer user);
#endif
static void pointer_update (void);
static void pointer_repace (void);
static void follow_pace_update (void);
static void vis_push (void);          /* "Skip hidden areas" */
static void state_touch (void);
/* XInput2 raw events on the root, selected only while something needs
 * them: desktop clicks (cursor scripts, web pages), pointer motion (a
 * wallpaper that follows the pointer, pointer at rest), any input (display
 * asleep by DPMS on a server without DPMS events: the wake-up) */
static gboolean xi_want_buttons, xi_want_motion, xi_want_wake;
static int xi2_opcode = -1;
static void xi2_apply_mask (void);

/* ---- log guard
 * The daemon runs for weeks and its output usually ends in a file
 * (~/.cache/walpe/daemon.log when the GUI started it, whatever the user
 * redirected to otherwise). Two things keep that file from eating the
 * disk: (1) a rate limit - messages, warnings and criticals pass at up to
 * LOG_RATE a second after a burst of LOG_BURST (a broken socket or a
 * driver error repeating every frame wrote 30 000 lines a second before);
 * what was dropped is counted and reported once it calms down; debug
 * output (G_MESSAGES_DEBUG, asked for) is never limited; (2) a size cap -
 * when stderr is a regular file opened for appending and has grown past
 * 16 MiB (WALPE_LOG_MAX_MB) it is cut back to its last 1 MiB
 * (log_cap_now), checked every LOG_CHECK_EVERY lines (no timer). */
#define LOG_BURST 2000
#define LOG_RATE 50
#define LOG_MAX_BYTES (16 << 20)
#define LOG_CHECK_EVERY 256

static gboolean log_cap_now (void);

static void log_guard (const gchar *domain, GLogLevelFlags level, const gchar *msg, gpointer user)
{
    (void) user;
    static GMutex lock;
    static double tokens = LOG_BURST;
    static gint64 last_us;
    static guint dropped, lines;
    gboolean pass = TRUE, cut = FALSE;
    guint report = 0;
    if (!(level & G_LOG_LEVEL_DEBUG) && !(level & (G_LOG_FLAG_FATAL | G_LOG_LEVEL_ERROR))) {
        g_mutex_lock (&lock);
        gint64 now = g_get_monotonic_time ();
        if (last_us)
            tokens = MIN ((double) LOG_BURST, tokens + (now - last_us) * (LOG_RATE / 1e6));
        last_us = now;
        if (tokens >= 1.0) {
            tokens -= 1.0;
            report = dropped;
            dropped = 0;
        } else {
            dropped++;
            pass = FALSE;
        }
        /* the size cap (log_cap_now: the last 1 MiB is kept, WALPE_LOG_MAX_MB) */
        if (pass && ++lines % LOG_CHECK_EVERY == 0)
            cut = log_cap_now ();
        g_mutex_unlock (&lock);
    }
    if (!pass)
        return;
    (void) cut;   /* log_cap_now wrote its own line into the file */
    if (report) {
        char *note = g_strdup_printf ("%u log lines dropped (more than %d a second)", report, LOG_RATE);
        g_log_default_handler ("walpe", G_LOG_LEVEL_WARNING, note, NULL);
        g_free (note);
    }
    g_log_default_handler (domain, level, msg, NULL);
}

/* quality knobs: the wallpaper a monitor's settings come from - its own,
 * or, following a leader, the leader's (the same wallpaper, rendered once) */
static const char *mon_quality_path (Mon *m)
{
    Mon *src = m->leader ? m->leader : m;
    return src->want_path ? src->want_path : walpe_config_monitor_path (D.cfg, src->name);
}

/* what the frame rate rules see for monitor m (walpe_quality_fps): the
 * focus and battery signals and the two opt-in limits */
static void mon_fps_state (Mon *m, WalpeFpsState *st)
{
    memset (st, 0, sizeof (*st));
    st->focus_app = m->focus_app;
    st->focus_fps = D.cfg->focus_action == WALPE_FOCUS_LIMIT ? D.cfg->focus_fps : 0;
    st->on_battery = D.on_battery;
    st->battery_fps = D.cfg->battery_fps;
}

/* the frame rate limit of one monitor: the fps setting, lowered by the
 * opt-in rules (focus_action "limit" while an application on that monitor
 * has the focus; battery_fps while on battery) and by its wallpaper's
 * fps_idle */
static int mon_fps_cap (Mon *m)
{
    /* quality knobs (common/quality.h): the wallpaper's own limit
     * (fps_idle, global or its own) and the two rules in one pure
     * function; with fps_idle 0 it is min (fps, focus rule, battery rule)
     * as before */
    WalpeQuality q;
    WalpeFpsState st;
    walpe_config_quality_for (D.cfg, mon_quality_path (m), &q);
    mon_fps_state (m, &st);
    return walpe_quality_fps (&q, &st, D.cfg->fps);
}
static gboolean want_pause_mon (Mon *m);

/* -------------------------------------------------------- presentation
 * (present.h) Which swap interval each monitor's window gets, what the GL
 * driver is told before EGL starts, and what this machine's EGL and X
 * server can do (logged once at start-up, so a log from the user's
 * machine describes the path its frames take). Settings: gpu.conf or
 * WALPE_<KEY> in the environment, read once before any thread exists.
 * Render threads only read m->present_iv and apply it to their own
 * surface: eglSwapInterval acts on the draw surface of the context
 * current on the calling thread. */
static struct {
    GHashTable *conf;        /* gpu.conf: key -> value (never NULL after load) */
    char *spec;              /* present = "MODE[,NAME=MODE]..." (NULL = vsync) */
    char *sync_device;       /* __GL_SYNC_DISPLAY_DEVICE as the driver sees it */
    gboolean composited;     /* main: a compositing manager owns _NET_WM_CM_Sn */
    Atom cm_atom;
    int min_iv, max_iv;      /* EGL_MIN/MAX_SWAP_INTERVAL of the config */
    gboolean has_damage;     /* EGL_KHR/EXT_swap_buffers_with_damage */
    gboolean damage_khr;
    gboolean swap_damage;    /* swap_damage=on: swaps carry their damage */
    gint64 idle_release_us;  /* idle_release (seconds), 0 = off */
    gboolean own_display;    /* egl_display=own: what this build does by default
                              * (D.egl_xd); kept as a known key */
    gint warned_iv;          /* render threads: eglSwapInterval failure logged once */
} P = { .max_iv = 1 };

/* a setting: WALPE_<KEY> from the environment, else gpu.conf; NULL = unset */
static const char *present_setting (const char *key)
{
    char *up = g_ascii_strup (key, -1);
    char *env = g_strconcat ("WALPE_", up, NULL);
    const char *v = g_getenv (env);
    g_free (up);
    g_free (env);
    if (v && *v)
        return v;
    v = P.conf ? g_hash_table_lookup (P.conf, key) : NULL;
    return v && *v ? v : NULL;
}

/* main, before any thread exists and before EGL is loaded (setenv is not
 * thread-safe; the driver reads its variables when it starts): read the
 * settings and hand the driver hints on. A variable the user already set
 * in the environment is never replaced. */
static void present_settings_load (void)
{
    char *path = g_build_filename (g_get_user_config_dir (), "walpe", "gpu.conf", NULL);
    char *txt = NULL;
    if (g_file_get_contents (path, &txt, NULL, NULL))
        g_message ("present: settings from %s", path);
    P.conf = walpe_gpuconf_parse (txt);
    g_free (txt);
    g_free (path);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, P.conf);
    while (g_hash_table_iter_next (&it, &k, &v))
        if (!g_strv_contains (walpe_gpuconf_keys (), k))
            g_warning ("gpu.conf: unknown key \"%s\" (ignored)", (char *) k);

    P.spec = g_strdup (present_setting ("present"));
    const char *s = present_setting ("idle_release");
    if (s) {
        double sec = g_ascii_strtod (s, NULL);
        P.idle_release_us = sec >= 1.0 ? (gint64) (MIN (sec, 86400.0) * 1e6) : 0;
    }
    gboolean b = FALSE;
    s = present_setting ("swap_damage");
    P.swap_damage = s && walpe_conf_bool (s, &b) && b;
    s = present_setting ("egl_display");
    P.own_display = s && !g_ascii_strcasecmp (s, "own");
    /* frame_align: all monitors' frame clocks on one grid */
    s = present_setting ("frame_align");
    if (s && walpe_conf_bool (s, &b) && b) {
        walpe_frame_align_set (TRUE);
        g_message ("present: frame clocks of all monitors aligned (frame_align)");
    }

    /* a daemon that restarted itself in place (monitor layout change)
     * inherits the hints its previous run set: drop those first, so they
     * follow the settings of now and are not taken for the user's own */
    const char *mine = g_getenv ("WALPE_GL_HINTS_SET");
    if (mine) {
        char **names = g_strsplit (mine, ",", -1);
        for (int i = 0; names[i]; i++)
            if (g_str_has_prefix (names[i], "__GL_"))
                g_unsetenv (names[i]);
        g_strfreev (names);
        g_unsetenv ("WALPE_GL_HINTS_SET");
    }
    GString *set = g_string_new (NULL);
    static const char *const hints[] = { "yield", "threaded", "max_frames", "sync_display",
                                         "shader_cache", "shader_cache_size" };
    for (guint i = 0; i < G_N_ELEMENTS (hints); i++) {
        const char *val = present_setting (hints[i]);
        if (!val)
            continue;
        GPtrArray *ev = g_ptr_array_new_with_free_func (g_free);
        if (!walpe_driver_hint (hints[i], val, ev))
            g_warning ("present: %s = %s is not understood (ignored)", hints[i], val);
        for (guint j = 0; j < ev->len; j++) {
            char *nv = g_ptr_array_index (ev, j), *eq = strchr (nv, '=');
            *eq = 0;
            if (g_getenv (nv)) {
                g_message ("present: %s is set in the environment: kept (%s ignored)", nv, hints[i]);
            } else {
                g_setenv (nv, eq + 1, FALSE);
                g_string_append_printf (set, "%s%s", set->len ? "," : "", nv);
                g_message ("present: driver hint %s=%s (%s)", nv, eq + 1, hints[i]);
            }
        }
        g_ptr_array_unref (ev);
    }
    if (set->len)
        g_setenv ("WALPE_GL_HINTS_SET", set->str, TRUE);
    g_string_free (set, TRUE);
    P.sync_device = g_strdup (g_getenv ("__GL_SYNC_DISPLAY_DEVICE"));
}

/* main, EGL initialised and the config chosen: what the swap can do here */
static void present_egl_probe (void)
{
    EGLint lo = 0, hi = 1;
    eglGetConfigAttrib (D.egl_dpy, D.egl_conf, EGL_MIN_SWAP_INTERVAL, &lo);
    eglGetConfigAttrib (D.egl_dpy, D.egl_conf, EGL_MAX_SWAP_INTERVAL, &hi);
    if (D.fake_vsync) {
        /* testing on a display without vsync (Xvfb: Mesa offers 1..1 and
         * never waits): mon_swapped emulates the wait of interval 1, so
         * both intervals exist as far as the pacing is concerned */
        lo = 0;
        hi = MAX (hi, 1);
    }
    P.min_iv = lo;
    P.max_iv = hi;
    P.damage_khr = epoxy_has_egl_extension (D.egl_dpy, "EGL_KHR_swap_buffers_with_damage");
    P.has_damage = P.damage_khr ||
                   epoxy_has_egl_extension (D.egl_dpy, "EGL_EXT_swap_buffers_with_damage");
    const char *vendor = eglQueryString (D.egl_dpy, EGL_VENDOR);
    const char *version = eglQueryString (D.egl_dpy, EGL_VERSION);
    g_message ("present: EGL %s (%s)%s, swap interval %d..%d%s, swap with damage: %s, buffer age: %s",
               version ? version : "?", vendor ? vendor : "?",
               D.egl_xd != D.xd ? ", own X connection" : "", (int) lo, (int) hi,
               D.fake_vsync ? " (WALPE_FAKE_VSYNC: emulated)" : "",
               P.has_damage ? (P.swap_damage ? "yes (used)" : "yes (not used: swap_damage=on)")
                            : "no",
               epoxy_has_egl_extension (D.egl_dpy, "EGL_EXT_buffer_age") ? "yes" : "no");
    if (vendor && strstr (vendor, "NVIDIA")) {
        /* the driver's own disk cache of compiled shaders is what makes
         * the second load of a wallpaper cheap: say so when it cannot work */
        const char *blk = walpe_shader_cache_blocker (
            geteuid () == 0, !g_strcmp0 (g_getenv ("__GL_SHADER_DISK_CACHE"), "1"),
            g_getenv ("__GL_SHADER_DISK_CACHE_PATH") != NULL, g_getenv ("XDG_CACHE_HOME") != NULL,
            g_getenv ("HOME") != NULL);
        if (blk)
            g_warning ("present: %s: every start compiles every shader again (gpu.conf: "
                       "shader_cache = /a/directory)", blk);
    }
}

static gboolean present_composited (void)
{
    return P.cm_atom != None && XGetSelectionOwner (D.xd, P.cm_atom) != None;
}

/* main: the swap interval of every monitor from its mode and what is
 * known now; all = log every monitor (start-up), else only changes */
static void present_update (gboolean all)
{
    WalpePresentEnv e = { .composited = P.composited, .sync_device = P.sync_device,
                          .min_interval = P.min_iv, .max_interval = P.max_iv };
    for (GList *l = D.mons; l && e.sync_device; l = l->next)
        if (!g_ascii_strcasecmp (e.sync_device, ((Mon *) l->data)->name))
            e.sync_is_ours = TRUE;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        const char *why = NULL;
        int iv = walpe_present_interval (m->present_mode, m->name, &e, &why);
        if (all || iv != g_atomic_int_get (&m->present_iv))
            g_message ("present: mon%d %s: swap interval %d (%s)", m->index, m->name, iv, why);
        g_atomic_int_set (&m->present_iv, iv);   /* applied before its next swap */
    }
}

static gboolean present_tick (gpointer user);
static guint present_timer;

/* main: (re)read the present setting "MODE[,NAME=MODE]..." into every
 * monitor and work out the swap intervals; the render threads pick them
 * up before their next swap, so this also works on a running wallpaper
 * (`walpe-ctl set present=...`). Returns the first token that was not
 * understood (g_free), NULL when all of it was. */
static char *present_apply_spec (const char *spec)
{
    char *first_bad = NULL;
    gboolean any_auto = FALSE;
    if (spec != P.spec) {
        g_free (P.spec);
        P.spec = g_strdup (spec);
    }
    P.composited = present_composited ();
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        char *bad = NULL;
        m->present_mode = walpe_present_spec_mode (P.spec, m->name, m->index, &bad);
        any_auto |= m->present_mode == WALPE_PRESENT_AUTO;
        if (bad && !first_bad)
            first_bad = bad;
        else
            g_free (bad);
    }
    present_update (TRUE);
    /* auto follows the compositing manager: one X request every 2 s */
    if (any_auto && !present_timer) {
        present_timer = g_timeout_add_seconds (2, present_tick, NULL);
    } else if (!any_auto && present_timer) {
        g_source_remove (present_timer);
        present_timer = 0;
    }
    return first_bad;
}

/* main, monitors exist, no render thread yet: the compositing manager,
 * the X server's presentation-related extensions, each monitor's mode */
static void present_init (void)
{
    char an[32];
    g_snprintf (an, sizeof an, "_NET_WM_CM_S%d", DefaultScreen (D.xd));
    P.cm_atom = XInternAtom (D.xd, an, False);
    static const char *const xext[] = { "NV-GLX", "NV-CONTROL", "DRI3", "Present", "Composite" };
    GString *s = g_string_new (NULL);
    for (guint i = 0; i < G_N_ELEMENTS (xext); i++) {
        int op = 0, ev = 0, er = 0;
        g_string_append_printf (s, "%s%s %s", i ? ", " : "", xext[i],
                                XQueryExtension (D.xd, xext[i], &op, &ev, &er) ? "yes" : "no");
    }
    g_message ("present: X server: %s; compositing manager: %s", s->str,
               present_composited () ? "running" : "none");
    g_string_free (s, TRUE);
    char *bad = present_apply_spec (P.spec);
    if (bad)
        g_warning ("present: \"%s\" in the present setting is not understood (vsync, limiter, "
                   "auto, NAME=mode)", bad);
    g_free (bad);
    if (P.idle_release_us)
        g_message ("present: idle release after %.0f s paused", P.idle_release_us / 1e6);
}

/* main, every 2 s while a monitor is in auto mode: a compositing manager
 * was started or stopped (one request to the X server) */
static gboolean present_tick (gpointer user)
{
    (void) user;
    gboolean c = present_composited ();
    if (c != P.composited) {
        P.composited = c;
        g_message ("present: compositing manager %s", c ? "started" : "gone");
        present_update (FALSE);
    }
    return G_SOURCE_CONTINUE;
}

/* render thread: give its surface the swap interval the monitor wants,
 * once and again whenever that changes */
static void mon_present_apply (Mon *m)
{
    int iv = g_atomic_int_get (&m->present_iv);
    if (iv == m->present_iv_applied)
        return;
    if (!eglSwapInterval (D.egl_dpy, iv) && g_atomic_int_compare_and_exchange (&P.warned_iv, 0, 1))
        g_warning ("eglSwapInterval (%d) failed on monitor %d: 0x%x", iv, m->index, eglGetError ());
    m->present_iv_applied = iv;
}

/* render thread: present the back buffer of its window. The swap flushes
 * the context itself (EGL: "eglSwapBuffers performs an implicit flush
 * operation on the context"), so no glFlush / glFinish goes before it.
 * swap_damage: the damage goes with the swap where the EGL can take it;
 * it is the whole surface, because no content reports less yet. */
static EGLBoolean mon_swap (Mon *m)
{
    mon_present_apply (m);
    if (P.swap_damage && P.has_damage) {
        EGLint r[4] = { 0, 0, m->geom.width, m->geom.height };
        return P.damage_khr ? eglSwapBuffersWithDamageKHR (D.egl_dpy, m->surf, r, 1)
                            : eglSwapBuffersWithDamageEXT (D.egl_dpy, m->surf, r, 1);
    }
    return eglSwapBuffers (D.egl_dpy, m->surf);
}

/* ------------------------------------------------- shared rendering
 * Monitors of the same pixel size showing the same scene form a group: the
 * leader's thread loads, ticks and renders the scene once, copies each
 * frame into a slot texture of the EGL share group and signals the
 * followers; each follower thread blits the newest slot to its own window
 * and swaps at its own refresh rate (200 Hz and 144 Hz monitors each
 * present the latest frame). Contexts never change threads: only textures
 * (share group) and GLsync fences cross between them. Slots: the newest
 * frame + one being written + one per follower being read, so the leader
 * never waits for a follower's CPU, only (on the GPU) for its blit. */
#define SF_MAX_FOLLOWERS 4
#define SF_MAX_SLOTS (SF_MAX_FOLLOWERS + 2)

typedef struct SharedFrame {
    gint refs;                       /* atomic: main's pending roles + threads */
    GMutex lock;
    GCond cond;                      /* busy[] reached 0 */
    int nfollowers, nslots;
    Mon *fol[SF_MAX_FOLLOWERS];      /* immutable after creation */
    int w, h;                        /* slot size (leader thread) */
    GLuint tex[SF_MAX_SLOTS];        /* leader-created, shared; fixed for life */
    GLsync written[SF_MAX_SLOTS];    /* leader: slot content complete */
    GLsync readf[SF_MAX_SLOTS][SF_MAX_FOLLOWERS]; /* follower blit done */
    guint64 clip_gen[SF_MAX_SLOTS];  /* leader's clip_serial the slot was drawn with */
    int busy[SF_MAX_SLOTS];          /* followers reading the slot right now */
    int latest;                      /* newest complete slot, -1 = none */
    guint64 serial;
    gboolean dead;                   /* leader retired it */
} SharedFrame;

static gint follow_always_wake;   /* WALPE_FOLLOW_GATE=1 (atomic: main sets it once,
                                    * the leaders' threads read it) */

static SharedFrame *sf_new (int nfollowers)
{
    SharedFrame *sf = g_new0 (SharedFrame, 1);
    g_mutex_init (&sf->lock);
    g_cond_init (&sf->cond);
    sf->nfollowers = nfollowers;
    sf->nslots = nfollowers + 2;
    sf->latest = -1;
    return sf;
}

static SharedFrame *sf_ref (SharedFrame *sf)
{
    if (sf)
        g_atomic_int_inc (&sf->refs);
    return sf;
}

static void sf_unref (SharedFrame *sf)
{
    if (!sf || !g_atomic_int_dec_and_test (&sf->refs))
        return;
    g_mutex_clear (&sf->lock);
    g_cond_clear (&sf->cond);
    g_free (sf);
}

static void sf_drop_fbos (Mon *m)
{
    for (int i = 0; i < 8; i++)
        if (m->sf_fbo[i])
            glDeleteFramebuffers (1, &m->sf_fbo[i]);
    memset (m->sf_fbo, 0, sizeof (m->sf_fbo));
    memset (m->sf_fbo_tex, 0, sizeof (m->sf_fbo_tex));
}

/* leader thread: no follower reads any more; free every GL object */
static void sf_retire (Mon *m)
{
    SharedFrame *sf = m->lead_sf;
    if (!sf)
        return;
    g_mutex_lock (&sf->lock);
    sf->dead = TRUE;
    for (;;) {
        gboolean busy = FALSE;
        for (int i = 0; i < sf->nslots; i++)
            busy |= sf->busy[i] > 0;
        if (!busy)
            break;
        g_cond_wait (&sf->cond, &sf->lock);
    }
    g_mutex_unlock (&sf->lock);
    glFinish ();
    for (int i = 0; i < sf->nslots; i++) {
        if (sf->written[i])
            glDeleteSync (sf->written[i]);
        for (int f = 0; f < SF_MAX_FOLLOWERS; f++)
            if (sf->readf[i][f])
                glDeleteSync (sf->readf[i][f]);
        if (sf->tex[i])
            glDeleteTextures (1, &sf->tex[i]);
    }
    sf_drop_fbos (m);
    m->lead_sf = NULL;
    sf_unref (sf);
}

/* XCheckIfEvent predicate: any event (mon_swapped throws them all away) */
static Bool egl_any_event (Display *d, XEvent *e, XPointer arg)
{
    (void) d; (void) e; (void) arg;
    return True;
}

/* render thread, after a swap. WALPE_FAKE_VSYNC=1 (testing): block until
 * the monitor's next vertical blank, as a swap does on a real display with
 * vsync; Xvfb has none and returns at once, which hides what a slow
 * monitor does to the pacing. Monitors of unknown rate do not block. */
static void mon_swapped (Mon *m)
{
    /* the EGL connection has no event loop: whatever the server sent it
     * anyway (MappingNotify, ...) would pile up in Xlib's queue. No
     * request, no system call: the queue is only looked at */
    if (D.egl_xd != D.xd && XEventsQueued (D.egl_xd, QueuedAlready) > 0) {
        /* Every monitor's thread comes by here on the one EGL connection.
         * Looking at the queue and taking an event were two calls
         * (XEventsQueued, XNextEvent): two threads that both saw the last
         * event both asked for it, and the one that came second waited
         * inside XNextEvent for the next event of a connection that hardly
         * ever gets one - its monitor stood still, and the daemon's quit
         * waited for the thread. XCheckIfEvent takes an event if there is
         * one and returns if there is none, in one call under Xlib's lock.
         * The look above is what a swap cost before; only when something
         * is queued does the last call here read the connection once
         * (without waiting). */
        XEvent e;
        while (XCheckIfEvent (D.egl_xd, &e, egl_any_event, NULL)) {
        }
    }
    if (!D.fake_vsync || m->hz < 1.0 || m->present_iv_applied < 1)
        return;   /* (swap interval 0: the swap does not wait) */
    gint64 period = (gint64) (1e6 / m->hz), now = g_get_monotonic_time ();
    g_usleep ((gulong) (period - now % period));
}

/* leader thread, after the scene was drawn to the back buffer (w x h) and
 * before the swap: copy it into a free slot and hand it to the followers.
 * FALSE = no slot was free, the followers did not get this frame. */
static gboolean sf_publish (Mon *m, int w, int h, guint64 clip_gen)
{
    SharedFrame *sf = m->lead_sf;
    GLsync rf[SF_MAX_FOLLOWERS], old_w;
    int nrf = 0, s = -1;
    g_mutex_lock (&sf->lock);
    for (int i = 0; i < sf->nslots && s < 0; i++)
        if (i != sf->latest && sf->busy[i] == 0)
            s = i;
    if (s < 0) {
        g_mutex_unlock (&sf->lock);
        return FALSE;
    }
    old_w = sf->written[s];
    sf->written[s] = NULL;
    for (int f = 0; f < SF_MAX_FOLLOWERS; f++)
        if (sf->readf[s][f]) {
            rf[nrf++] = sf->readf[s][f];
            sf->readf[s][f] = NULL;
        }
    g_mutex_unlock (&sf->lock);
    /* the followers' last blits from this slot finish before we overwrite
     * it (a GPU-side wait: this thread does not block) */
    for (int i = 0; i < nrf; i++) {
        glWaitSync (rf[i], 0, GL_TIMEOUT_IGNORED);
        glDeleteSync (rf[i]);
    }
    if (old_w)
        glDeleteSync (old_w);
    if (!sf->w) {
        sf->w = w;
        sf->h = h;
    }
    if (!sf->tex[s]) {
        glGenTextures (1, &sf->tex[s]);
        glBindTexture (GL_TEXTURE_2D, sf->tex[s]);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, sf->w, sf->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers (1, &m->sf_fbo[s]);
        glBindFramebuffer (GL_FRAMEBUFFER, m->sf_fbo[s]);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sf->tex[s], 0);
    }
    glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, m->sf_fbo[s]);
    {
        /* "Skip hidden areas": only the boxes that were rendered (what
         * any monitor of the group shows) are worth copying; -1 = no
         * clip: the whole frame, as ever */
        WalpeBox cb[WALPE_CLIP_MAX];
        int cn = sf->w == w && sf->h == h ? walpe_clip_boxes (cb) : -1;
        if (cn < 0)
            glBlitFramebuffer (0, 0, w, h, 0, 0, sf->w, sf->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        for (int i = 0; i < cn; i++)
            glBlitFramebuffer (cb[i].x0, cb[i].y0, cb[i].x1, cb[i].y1, cb[i].x0, cb[i].y0,
                               cb[i].x1, cb[i].y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    GLsync f = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush ();   /* the fence must reach the GPU before others wait on it */
    g_mutex_lock (&sf->lock);
    sf->written[s] = f;
    sf->clip_gen[s] = clip_gen;
    sf->latest = s;
    sf->serial++;
    g_mutex_unlock (&sf->lock);
    for (int i = 0; i < sf->nfollowers; i++) {
        /* request_redraw, but a follower that waits for its next
         * slot (its refresh rate or its own frame rate limit is below the
         * leader's rate) presents the newest frame when the slot comes: it
         * was woken for every frame published in between only to go back
         * to sleep. WALPE_FOLLOW_GATE=1: always woken, as before (A/B). */
        Mon *f = sf->fol[i];
        g_mutex_lock (&f->lock);
        f->render_req = TRUE;
        if (!f->clock_wait || g_atomic_int_get (&follow_always_wake))
            g_cond_signal (&f->cond);
        g_mutex_unlock (&f->lock);
    }
    return TRUE;
}

/* follower thread: present the newest published frame; FALSE = none yet */
static gboolean sf_present (Mon *m, guint64 need)
{
    SharedFrame *sf = m->follow_sf;
    g_mutex_lock (&sf->lock);
    /* need ("Skip hidden areas", else 0): the newest frame was drawn before
     * the leader knew what this monitor shows now (a window moved away
     * here): it lacks those pixels. The leader is drawing one that has
     * them and will wake us. */
    if (sf->dead || sf->latest < 0 || sf->clip_gen[sf->latest] < need) {
        g_mutex_unlock (&sf->lock);
        return FALSE;
    }
    int s = sf->latest, fi = m->follow_idx;
    sf->busy[s]++;
    GLsync f = sf->written[s];
    GLuint tex = sf->tex[s];
    int w = sf->w, h = sf->h;
    g_mutex_unlock (&sf->lock);

    if (f)
        glWaitSync (f, 0, GL_TIMEOUT_IGNORED);
    /* GPUPROF: the follower's frame = this blit + its swap */
    gboolean prof = WALPE_PROF_ON ();
    if (prof) {
        walpe_prof_frame_begin (m->index, m->geom.width, m->geom.height);
        walpe_prof_stage (WALPE_PROF_ST_PRESENT);
    }
    if (m->sf_fbo_tex[s] != tex) {
        if (!m->sf_fbo[s])
            glGenFramebuffers (1, &m->sf_fbo[s]);
        glBindFramebuffer (GL_FRAMEBUFFER, m->sf_fbo[s]);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        m->sf_fbo_tex[s] = tex;
    }
    glBindFramebuffer (GL_READ_FRAMEBUFFER, m->sf_fbo[s]);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
    glViewport (0, 0, m->geom.width, m->geom.height);
    {
        /* this monitor's own visible boxes ("Skip hidden areas"; -1 = no clip) */
        WalpeBox cb[WALPE_CLIP_MAX];
        int cn = w == m->geom.width && h == m->geom.height ? walpe_clip_boxes (cb) : -1;
        if (cn < 0)
            glBlitFramebuffer (0, 0, w, h, 0, 0, m->geom.width, m->geom.height,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
        for (int i = 0; i < cn; i++)
            glBlitFramebuffer (cb[i].x0, cb[i].y0, cb[i].x1, cb[i].y1, cb[i].x0, cb[i].y0,
                               cb[i].x1, cb[i].y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    GLsync rf = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    g_mutex_lock (&sf->lock);
    GLsync old = sf->readf[s][fi];
    sf->readf[s][fi] = rf;
    if (--sf->busy[s] == 0)
        g_cond_broadcast (&sf->cond);
    g_mutex_unlock (&sf->lock);
    if (old)
        glDeleteSync (old);
    if (prof)
        walpe_prof_stage (WALPE_PROF_ST_SWAP);   /* GPUPROF */
    /* no glFlush before the swap: it flushes this context itself (the
     * blit and the fence rf; the leader waits on rf at the earliest with
     * its next frame). One flush per presented frame. */
    if (!mon_swap (m)) {
        g_warning ("eglSwapBuffers failed: 0x%x", eglGetError ());
        glFlush ();   /* the fence must still reach the GPU */
    }
    mon_swapped (m);
    if (prof)
        walpe_prof_frame_end (TRUE);             /* GPUPROF */
    g_atomic_int_inc (&m->render_count);
    g_atomic_int_set (&m->last_w, m->geom.width);
    g_atomic_int_set (&m->last_h, m->geom.height);
    return TRUE;
}

/* render thread: leave the current role (leader or follower) */
static void mon_drop_role (Mon *m)
{
    if (m->lead_sf)
        sf_retire (m);
    if (m->follow_sf) {
        sf_drop_fbos (m);
        sf_unref (m->follow_sf);
        m->follow_sf = NULL;
    }
}

/* ----------------------------------------------------- render thread
 * The render thread is the ONLY thread that touches m->content: it loads,
 * renders, pauses and frees it. The main thread communicates through
 * fields under m->lock (load_path, render_req, pause_wanted) and reads
 * m->content_type atomically. Animated content asks for its next frame via
 * content->next_frame_us; the thread sleeps on its condition variable
 * until then (no main-loop timers reference content, so content can be
 * freed synchronously here — no timer use-after-free on swaps). */

static void mon_free_content (Mon *m)
{
    /* a video still waited for (first_path) goes unreported: whoever frees
     * it reports for what comes instead (a newer load, the failure) or has
     * told the main thread already (a monitor that follows another now).
     * (An idle release does not come by here with one: mon_thread.) */
    g_clear_pointer (&m->first_path, g_free);
    if (!m->content)
        return;
    if (m->content->gl_free)
        m->content->gl_free (m->content);
    m->content->free (m->content);
    m->content = NULL;
    g_atomic_int_set (&m->content_type, WALPE_TYPE_NONE);
    g_atomic_int_set (&m->wants_pointer, 0);
    g_atomic_int_set (&m->uses_ptr_pos, 0);
}

/* render thread: hand the wanted pause state to the content */
static void mon_apply_pause (Mon *m, gboolean p)
{
    if (m->content && m->content->set_paused && p != m->pause_applied)
        m->content->set_paused (m->content, p);
    m->pause_applied = p;
    if (p && m->content)
        m->content->next_frame_us = 0; /* paused: no frame clock */
}

/* render thread, idle_release: a wallpaper whose monitor (shared
 * rendering: whole group) has been paused this long gives back what it
 * holds on the GPU - textures, render targets, programs, the decoder -
 * like Wallpaper Engine's "Stop (free memory)". The thread loads it again
 * (m->content_path) when the monitor resumes; a video starts over. */
static gboolean content_note_cb (gpointer user);

static void mon_idle_release (Mon *m)
{
    g_message ("idle release: mon%d (%s) has been paused for %.0f s: %s unloaded, its video "
               "memory is free; it loads again when the monitor resumes", m->index, m->name,
               (g_get_monotonic_time () - m->paused_since_us) / 1e6,
               m->content_path ? m->content_path : "?");
    mon_free_content (m);
    g_atomic_int_set (&m->idle_released, 1);
    malloc_trim (0);
    g_idle_add (content_note_cb, NULL);   /* status changed without a command: subscribers */
}

/* the frame rate handed to a monitor's content: the fps setting, limited
 * to what is shown. A scene belongs to its monitor (shared rendering: to
 * the group's fastest, which leads), so its own refresh rate; a web page
 * is captured once for every monitor showing it, so the fastest
 * monitor's. Videos and GIFs keep their own timing. */
static int mon_content_fps (Mon *m, WalpeType type, int cap)
{
    if (D.no_refresh_cap)
        return cap;
    return walpe_paced_fps (cap, type == WALPE_TYPE_SCENE ? m->hz : D.max_hz);
}

/* render thread -> main: how the load of `path` ended (msg taken; NULL =
 * fine). A load runs after `open` has answered, so a failure used to reach
 * the log only: status now shows it per monitor ("load_error"). kept: it
 * failed and the windows were left as they were (load_keep). What follows
 * from it is decided on the main thread: load_ended (). */
typedef struct { Mon *m; char *path; char *msg; gboolean kept; guint tag; } LoadNote;

static void load_ended (Mon *m, const char *path, const char *msg, gboolean kept);

static gboolean load_note_cb (gpointer user)
{
    LoadNote *n = user;
    if (n->tag && n->tag == n->m->load_wait)
        n->m->loading = FALSE;   /* (an older load's note leaves the newer one pending) */
    load_ended (n->m, n->path, n->msg, n->kept);
    g_free (n->path);
    g_free (n->msg);
    g_free (n);
    /* the new content's type and pointer needs are set by now (the 1 s
     * recheck timer that used to pick them up is gone) */
    pointer_update ();
    state_touch ();
    return G_SOURCE_REMOVE;
}

/* render thread -> main: this monitor's content went away without a load
 * (it follows another monitor now) */
static gboolean content_note_cb (gpointer user)
{
    (void) user;
    pointer_update ();
    state_touch ();
    return G_SOURCE_REMOVE;
}

static void mon_load_note (Mon *m, const char *path, char *msg, gboolean kept, guint tag)
{
    LoadNote *n = g_new0 (LoadNote, 1);
    n->m = m;
    n->path = g_strdup (path);
    n->msg = msg;
    n->kept = kept;
    n->tag = tag;
    g_idle_add (load_note_cb, n);
}

/* render thread: the load failed - no content, a black frame on this
 * monitor and (shared rendering) on the monitors following it. keep: no
 * frame; the windows go on showing the last frame of what they had (also a
 * follower's, which presents nothing until its leader publishes): the main
 * thread opens the previous wallpapers again, and asks for the black frame
 * (blank_req) should there be none after all. */
static void mon_load_failed_frame (Mon *m, gboolean keep)
{
    int w = MAX (1, m->geom.width), h = MAX (1, m->geom.height);
    mon_free_content (m);
    g_atomic_int_set (&m->content_type, WALPE_TYPE_NONE);   /* a follower had no content */
    /* nor does anything want the pointer: a scene freed early to make room
     * (mon_do_load_ex) left what the main thread saw as it was */
    g_atomic_int_set (&m->wants_pointer, 0);
    g_atomic_int_set (&m->uses_ptr_pos, 0);
    if (keep) {
        m->win_has_frame = m->sf_has_frame = FALSE;   /* as below: no content's frame */
        return;
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glDisable (GL_SCISSOR_TEST);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport (0, 0, w, h);
    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    if (m->lead_sf)
        (void) sf_publish (m, w, h, G_MAXUINT64);   /* drawn in full: it serves any clip */
    eglSwapBuffers (D.egl_dpy, m->surf);
    /* neither the window nor the followers' slot holds a frame of a
     * content any more (frame skipping) */
    m->win_has_frame = m->sf_has_frame = FALSE;
}

/* A video's load ends with its first picture.
 *
 * The player opens a clip when its container reads well; whether the
 * decoder can do anything with the stream shows on its thread, later. A
 * clip it refuses (a damaged stream, a profile or size it does not take)
 * used to count as loaded: `open` and status named it, nothing said why
 * the monitor went on showing the last frame of the wallpaper before, and
 * the wallpaper that had worked was not opened again.
 *
 * So the note of a video load is held back (first_path) until gl_render
 * has drawn a picture of it (mon_do_render_frame) - status says "loading"
 * until then - and when none has come after WALPE_FIRST_FRAME_S seconds
 * (20; 0 = as before: a video counts as loaded when it was opened, with or
 * without a picture) the load has failed like any other:
 * the content is freed, the reason goes to status ("load_error") and the
 * main thread opens the previous wallpaper again, once (load_ended).
 *
 * The time is counted in quarter seconds this thread saw pass: a daemon
 * that was stopped meanwhile (SIGSTOP, a suspended machine, a debugger)
 * has not waited. A paused monitor waits too: a video paints its first
 * frame under a pause. */
#define FIRST_TICK_US 250000

static void mon_first_note_send (Mon *m)
{
    if (!m->first_path)
        return;
    char *path = g_steal_pointer (&m->first_path);
    mon_load_note (m, path, NULL, FALSE, m->first_tag);
    g_free (path);
}

/* render thread, once per loop turn while a video is waited for */
static void mon_first_frame_tick (Mon *m)
{
    gint64 now = g_get_monotonic_time ();
    if (now - m->first_tick_us < FIRST_TICK_US)
        return;
    m->first_tick_us = now;
    if (++m->first_ticks < D.first_frame_ticks)
        return;
    char *path = g_steal_pointer (&m->first_path);
    char *dec = walpe_video_decoder_note ();
    char *why = g_strdup_printf ("%s: the video gave no picture in %d s%s%s%s", path,
                                 (D.first_frame_ticks + 3) / 4, dec && *dec ? " (decoder: " : "",
                                 dec && *dec ? dec : "", dec && *dec ? ")" : "");
    g_free (dec);
    g_warning ("wallpaper load failed: %s", why);
    gboolean keep = m->first_keep;
    mon_load_failed_frame (m, keep);
    g_clear_pointer (&m->content_path, g_free);
    m->needs_first_frame = FALSE;
    malloc_trim (0);
    mon_load_note (m, path, why, keep, m->first_tag);
    g_free (path);
}

/* load + init a wallpaper. render thread only. keep: what a failure
 * leaves on the windows (mon_load_failed_frame). */
static void mon_do_load_ex (Mon *m, const char *path, gboolean paused, int fps,
                            const WalpeQuality *quality, gboolean keep, guint tag)
{
    GError *err = NULL;
    if (!*path) {
        /* "" = no wallpaper any more (its folder is being deleted: IPC
         * "forget"): free the content and leave the window black */
        mon_free_content (m);
        g_atomic_int_set (&m->content_type, WALPE_TYPE_NONE);   /* a follower had no content */
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glDisable (GL_SCISSOR_TEST);
        glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport (0, 0, MAX (1, m->geom.width), MAX (1, m->geom.height));
        glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        eglSwapBuffers (D.egl_dpy, m->surf);
        malloc_trim (0);
        mon_load_note (m, path, NULL, FALSE, tag);
        return;
    }
    WalpeType ntype = walpe_detect_type (path, NULL);
    WalpeLoadOpts opts = {
        .video_backend = "ffmpeg",
        .audio = FALSE,
        .fps = mon_content_fps (m, ntype, fps),
        .origin_dir = path,
        .no_tex_downscale = !g_atomic_int_get (&D.cfg->tex_downscale),
        .no_tex_slack = quality->tex_slack == 0,
        .render_scale = quality->render_scale,
        .quality = quality,
    };
    /* WALPE_LOAD_DELAY_MS (tests, scripts/lifecheck.py): a load that takes a
     * while whatever the machine, for what has to hold while one runs (a
     * setting that arrives meanwhile, a restart, a signal). Here: the
     * settings the load works with have been read. */
    if (D.load_delay_us)
        g_usleep (D.load_delay_us);
    /* A scene decodes and uploads all its textures inside its constructor.
     * Built next to the wallpaper it replaces, the old and the new
     * wallpaper's textures, render targets and decode buffers were alive
     * together during every switch (old + new instead of the larger of
     * the two, in RAM and in VRAM). Nothing is drawn while this thread
     * loads and the window keeps the last presented frame until the next
     * swap, so the old wallpaper can go first - except when the same
     * wallpaper is loaded again (an option change): its textures then come
     * from the shared cache, which only holds them while the old scene
     * does. The other content types build their GL side in gl_init, after
     * the old wallpaper is freed below. If the new scene then fails to
     * load, the main thread opens the old wallpaper again (load_ended). */
    char *restore = NULL;
    if (m->content && ntype == WALPE_TYPE_SCENE && m->content_path &&
        strcmp (m->content_path, path) != 0) {
        int ctype = g_atomic_int_get (&m->content_type);
        int wp = g_atomic_int_get (&m->wants_pointer), up = g_atomic_int_get (&m->uses_ptr_pos);
        restore = g_strdup (m->content_path);
        mon_free_content (m);
        /* what the main thread sees stays as it was until the load ends */
        g_atomic_int_set (&m->content_type, ctype);
        g_atomic_int_set (&m->wants_pointer, wp);
        g_atomic_int_set (&m->uses_ptr_pos, up);
        malloc_trim (0);
    }
    WalpeContent *c = walpe_content_new (path, &opts, &err);
    if (!c) {
        g_warning ("wallpaper load failed: %s", err ? err->message : "?");
        char *why = g_strdup_printf ("%s: %s", path, err ? err->message : "load failed");
        g_clear_error (&err);
        /* The monitor shows nothing now, like after a failed GL init
         * below. It used to keep the previous wallpaper running while
         * status and the config named the new one, and a monitor that had
         * just become its follower (its own content already dropped) kept
         * a stale picture: every monitor of the group gets the same black
         * frame instead, and status carries the reason ("load_error").
         * A scene that was freed above to make room is not loaded again
         * from here either (that brought back the leader's wallpaper only):
         * the main thread, which knows the group and what each of its
         * monitors showed before, opens the previous wallpapers again -
         * once, and until then (keep) the windows are left alone. */
        g_free (restore);
        mon_load_failed_frame (m, keep);
        /* the note goes last (as below): the main thread acts on it at once */
        mon_load_note (m, path, why, keep, tag);
        return;
    }
    g_free (restore);
    c->request_redraw = request_redraw;
    c->request_user = m;

    /* free the old wallpaper with our context current */
    mon_free_content (m);
    if (!c->gl_init (c, &err)) {
        g_warning ("wallpaper GL init failed: %s", err ? err->message : "?");
        char *why = g_strdup_printf ("%s: %s", path, err ? err->message : "GL init failed");
        g_clear_error (&err);
        if (c->gl_free)
            c->gl_free (c);
        c->free (c);
        mon_load_failed_frame (m, keep);
        mon_load_note (m, path, why, keep, tag);
        return;
    }
    m->content = c;
    m->win_has_frame = m->sf_has_frame = m->win_exposed = FALSE;
    g_atomic_int_set (&m->content_type, c->type);
    g_atomic_int_set (&m->wants_pointer, c->wants_pointer && c->pointer ? 1 : 0);
    g_atomic_int_set (&m->uses_ptr_pos, c->uses_pointer_pos ? 1 : 0);
    /* The note is queued only now. load_note_cb runs on the main thread as
     * soon as it is queued and works out there what the new wallpaper
     * needs of the pointer (pointer_update: position tracking, desktop
     * clicks, input for a web page) from the three values above - once,
     * there is no recheck. Queued before they were stored (an earlier build), the main
     * thread could run first and see the wallpaper that was replaced: a
     * scene with mouse effects or click scripts, or a web page, then got
     * no pointer until something else changed the pause state. */
    if (c->type == WALPE_TYPE_VIDEO && D.first_frame_ticks) {
        /* reported with its first picture (mon_first_note_send); what the
         * main thread takes from the three values above it gets now */
        m->first_path = g_strdup (path);
        m->first_tag = tag;
        m->first_keep = keep;
        m->first_ticks = 0;
        m->first_tick_us = g_get_monotonic_time ();
        g_idle_add (content_note_cb, NULL);
    } else {
        mon_load_note (m, path, NULL, FALSE, tag);
    }
    m->pause_applied = FALSE;
    mon_apply_pause (m, paused);
    /* (under the lock like every other writer: another monitor's thread
     * publishing to this one, or a decoder thread, sets it at any time) */
    g_mutex_lock (&m->lock);
    m->render_req = TRUE;
    g_mutex_unlock (&m->lock);
    m->needs_first_frame = TRUE; /* paint once even under covers */
    /* the old wallpaper and the new one's decode buffers (a 4700x3098 PNG
     * is 58 MB of RGBA) are freed now; glibc keeps freed heap pages
     * resident unless trimmed, so a light wallpaper after a heavy one kept
     * the heavy one's RSS */
    malloc_trim (0);
}

static void mon_do_load (Mon *m, const char *path, gboolean paused, int fps,
                         const WalpeQuality *quality, gboolean keep, guint tag)
{
    mon_do_load_ex (m, path, paused, fps, quality, keep, tag);
}

static void mon_do_render_frame (Mon *m, gboolean paused, gboolean force, gboolean self_paused,
                                 float ptr_x, float ptr_y, guint64 clip_gen)
{
    /* force = first frame after a load: paints even under covers so a
     * freshly loaded wallpaper is actually ON the window before the
     * covered-pause kicks in (otherwise it stays black until exposed) */
    if (!m->content || (paused && !force))
        return;
    int w = m->geom.width;
    int h = m->geom.height;
    if (w < 1 || h < 1)
        return;
    glViewport (0, 0, w, h);
    /* settings the main thread may change meanwhile: read as atomics */
    gboolean parallax = g_atomic_int_get (&D.cfg->parallax);
    float px = parallax ? ptr_x : 0.0f;
    float py = parallax ? ptr_y : 0.0f;
    m->content->next_frame_us = 0; /* gl_render re-arms it while animating */
    if (m->content->set_render_scale)   /* live setting: the global one or the wallpaper's own */
        m->content->set_render_scale (m->content, m->rt_render_scale > 0
                                          ? m->rt_render_scale
                                          : g_atomic_int_get (&D.cfg->render_scale));
    /* the frame must be drawn whatever the content thinks of it when the
     * window that is to show it, or the followers' slot, lacks its last one */
    if (g_atomic_int_compare_and_exchange (&m->exposed, 1, 0)) {
        m->win_has_frame = FALSE;
        m->win_exposed = TRUE;
    }
    m->content->surface_lost = force || (!self_paused && !m->win_has_frame) ||
                               (m->lead_sf && !m->sf_has_frame);
    m->content->surface_exposed = m->win_exposed;
    m->win_exposed = FALSE;
    m->content->frame_unchanged = FALSE;
    walpe_pixstat_begin ();     /* WALPE_PIX_STATS (engine/clip.h); nothing when unset */
    gboolean drew = m->content->gl_render (m->content, w, h, px, py);
    if (m->first_path && !m->content->first_frame_pending)
        mon_first_note_send (m);   /* the video has a picture: its load is over */
    walpe_pixstat_end (m->name, w, h);
    walpe_clip_retarget (0, w, h);   /* a scene at a reduced render scale placed the
                                      * boxes on its own target: back on the window
                                      * (nothing without a clip) */
    if (!drew || m->content->frame_unchanged)
        WALPE_PROF_FRAME_SKIPPED (m->index);   /* GPUPROF: nothing new was drawn */
    if (paused)
        m->content->next_frame_us = 0;
    /* a video that has no frame yet drew nothing: keep what is on screen
     * (swapping now showed an undefined buffer); its first frame comes
     * with a redraw request and is painted even on a paused monitor */
    if (m->content->first_frame_pending && !m->lead_sf)
        return;
    /* content.h: gl_render returns whether a change was drawn. Nothing
     * drawn = nothing to show: the back buffer is undefined after a swap
     * (EGL_BUFFER_DESTROYED), and presenting it again costs a full-screen
     * copy (and a recomposition under a compositing manager) for the same
     * or a stale picture. (A scene that found its picture unchanged says so
     * in frame_unchanged and returns FALSE: window and followers keep
     * their frame. A forced frame is always drawn: surface_lost.) */
    if ((!drew || m->content->frame_unchanged) && !force)
        return;
    static int gpumem = -1;
    if (gpumem < 0)
        gpumem = g_getenv ("WALPE_GPU_MEM") != NULL;
    if (gpumem && (m->render_count % 300) == 10) {
        char tag[16];
        g_snprintf (tag, sizeof tag, "mon%d", m->index);
        walpe_gl_mem_report (tag);
    }
    if (D.probe) {
        GLubyte p[4], q[4];
        glReadPixels (w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        glReadPixels (w / 4, h / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
        g_debug ("probe mon%d: center=%d,%d,%d,%d quarter=%d,%d,%d,%d",
                 m->index, p[0], p[1], p[2], p[3], q[0], q[1], q[2], q[3]);
    }
    if (m->lead_sf) {
        /* shared rendering: the followers get this frame; our own window
         * is presented only while this monitor itself is visible */
        WALPE_PROF_STAGE (WALPE_PROF_ST_PUBLISH);   /* GPUPROF */
        m->sf_has_frame = sf_publish (m, w, h, clip_gen);
        if (self_paused && !force) {
            m->win_has_frame = FALSE;
            return;
        }
    }
    WALPE_PROF_STAGE (WALPE_PROF_ST_SWAP);   /* GPUPROF: no GPU timer over the vsync wait */
    if (!mon_swap (m)) {
        g_warning ("eglSwapBuffers failed: 0x%x", eglGetError ());
        m->win_has_frame = FALSE;
        return;
    }
    m->win_has_frame = TRUE;
    mon_swapped (m);
    if (m->content->swapped)
        m->content->swapped (m->content);
    if (D.cadence) {
        gint64 now = g_get_monotonic_time ();
        if (m->last_swap_us)
            g_print ("CADENCE mon%d %ld\n", m->index, (long) (now - m->last_swap_us));
        m->last_swap_us = now;
    }
    g_atomic_int_inc (&m->render_count);
    g_atomic_int_set (&m->last_w, w);
    g_atomic_int_set (&m->last_h, h);
}

/* GPUPROF: one frame of a monitor's own content, as a profiler frame while
 * a profile runs (the flag is read once: a frame that began profiled also
 * ends profiled) */
static void mon_do_render (Mon *m, gboolean paused, gboolean force, gboolean self_paused,
                           float ptr_x, float ptr_y, guint64 clip_gen)
{
    gboolean prof = WALPE_PROF_ON () && m->content && !(paused && !force);
    guint presented = (guint) g_atomic_int_get (&m->render_count);
    if (prof)
        walpe_prof_frame_begin (m->index, m->geom.width, m->geom.height);
    mon_do_render_frame (m, paused, force, self_paused, ptr_x, ptr_y, clip_gen);
    if (prof)
        walpe_prof_frame_end ((guint) g_atomic_int_get (&m->render_count) != presented);
}

/* GPUPROF, render thread, between frames: attach to a starting profile /
 * hand in this thread's part of an ending one (gpuprof.h) */
static void mon_prof_sync (Mon *m)
{
    char role[24] = "";
    if (m->follow_sf)
        g_strlcpy (role, "follows a shared frame", sizeof (role));
    else if (m->lead_sf)
        g_strlcpy (role, "leads (shared)", sizeof (role));
    WalpeProfThreadInfo info = {
        .monitor = m->index,
        .name = m->name,
        .w = m->geom.width,
        .h = m->geom.height,
        .type = walpe_type_name (g_atomic_int_get (&m->content_type)),
        .path = m->content_path,
        .role = role,
        .hz = m->hz,
    };
    fx_targets_stats (&info.fx_bytes, &info.fx_count);
    fx_pass_stats (&info.fx_run, &info.fx_cached, FALSE);
    walpe_prof_thread_sync (&info);
}

/* GPUPROF, main thread: every render thread calls mon_prof_sync next */
static void prof_wake_all (gpointer user)
{
    (void) user;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        g_mutex_lock (&m->lock);
        m->prof_sync = TRUE;
        g_cond_signal (&m->cond);
        g_mutex_unlock (&m->lock);
    }
}

/* GL version ladder per thread: desktop 3.3, then GLES 3.2/3.0 */
static const struct { int maj, min; gboolean es; } ctx_ladder[] = {
    { 3, 3, FALSE }, { 3, 2, TRUE }, { 3, 0, TRUE },
};
/* share-group root: monitor contexts share its textures, so the
 * wallpaper's images exist once, not once per monitor. The first monitor
 * thread renders with the root itself (it used to be an extra context
 * that was never current: with N monitors N + 1 contexts were kept, each
 * with its own driver state - 28 MiB apiece on llvmpipe). */
static EGLContext share_root = EGL_NO_CONTEXT;
static int share_rung = -1;
static gint share_root_taken;
/* no context of the share group is current while another one is created:
 * every monitor thread waits here until all of them have theirs */
static GMutex ctx_lock;
static GCond ctx_cond;
static int ctx_expected, ctx_created;

static EGLContext ladder_context (guint i, EGLContext share)
{
    if (!eglBindAPI (ctx_ladder[i].es ? EGL_OPENGL_ES_API : EGL_OPENGL_API))
        return EGL_NO_CONTEXT;
    EGLint ca[] = {
        EGL_CONTEXT_MAJOR_VERSION, ctx_ladder[i].maj,
        EGL_CONTEXT_MINOR_VERSION, ctx_ladder[i].min,
        EGL_NONE, EGL_NONE,
        EGL_NONE
    };
    /* GPUPROF: WALPE_GL_DEBUG=1 (or a profile asked for at start-up with
     * WALPE_GPU_PROFILE) = debug contexts, so that the driver reports
     * everything it has on the KHR_debug channel; a context without the
     * flag is tried when the driver refuses it */
    static int dbg = -1;
    if (dbg < 0) {
        const char *e = g_getenv ("WALPE_GL_DEBUG");
        dbg = (e && *e && *e != '0') || g_getenv ("WALPE_GPU_PROFILE") != NULL;
    }
    if (dbg) {
        ca[4] = EGL_CONTEXT_OPENGL_DEBUG;
        ca[5] = EGL_TRUE;
        EGLContext c = eglCreateContext (D.egl_dpy, D.egl_conf, share, ca);
        if (c != EGL_NO_CONTEXT)
            return c;
        ca[4] = ca[5] = EGL_NONE;
    }
    return eglCreateContext (D.egl_dpy, D.egl_conf, share, ca);
}

/* main thread, before any monitor thread: WALPE_NO_SHARE=1 disables */
static void create_share_root (void)
{
    if (g_getenv ("WALPE_NO_SHARE"))
        return;
    for (guint i = 0; i < G_N_ELEMENTS (ctx_ladder); i++) {
        share_root = ladder_context (i, EGL_NO_CONTEXT);
        if (share_root != EGL_NO_CONTEXT) {
            share_rung = (int) i;
            return;
        }
    }
}

static EGLContext create_thread_context (void)
{
    if (share_root != EGL_NO_CONTEXT) {
        gboolean first = g_atomic_int_compare_and_exchange (&share_root_taken, 0, 1);
        /* the root was made on the main thread: the API binding is per thread */
        EGLContext ctx = !first ? ladder_context ((guint) share_rung, share_root)
                       : eglBindAPI (ctx_ladder[share_rung].es ? EGL_OPENGL_ES_API : EGL_OPENGL_API)
                             ? share_root : EGL_NO_CONTEXT;
        if (ctx) {
            walpe_gl_set_shared (TRUE);
            g_message ("monitor context: %s %d.%d (textures shared across monitors)",
                       ctx_ladder[share_rung].es ? "GLES" : "GL", ctx_ladder[share_rung].maj,
                       ctx_ladder[share_rung].min);
            return ctx;
        }
        /* all monitors must agree: without a share group no cache */
        g_warning ("shared GL context failed (0x%x); textures are per monitor", eglGetError ());
        walpe_gl_set_shared (FALSE);
        g_atomic_int_set (&D.share_failed, 1);
    }
    for (guint i = 0; i < G_N_ELEMENTS (ctx_ladder); i++) {
        EGLContext ctx = ladder_context (i, EGL_NO_CONTEXT);
        if (ctx) {
            g_message ("monitor context: %s %d.%d",
                       ctx_ladder[i].es ? "GLES" : "GL", ctx_ladder[i].maj, ctx_ladder[i].min);
            return ctx;
        }
    }
    return EGL_NO_CONTEXT;
}

typedef struct { guint gen; Mon *leader; char *path; const char *why; } SharedFallback;
static gboolean shared_fallback_cb (gpointer user);

/* leader thread, after loading: can one rendering serve every monitor of
 * the group? Not when the picture follows the pointer position (mouse
 * parallax on, or cursor scripts that read it): each monitor sees the
 * pointer at a different place. Clicks/hover alone are fine: they are
 * routed to the one scene from whichever monitor they happen on. */
/* pointer_share (config, `walpe-ctl set pointer_share=on`, or
 * WALPE_POINTER_SHARE=1 for one run): a scene that follows the pointer is
 * shared like any other scene; see pointer_sample. Off by default. */
static gboolean pointer_share_on (void)
{
    static int env = -1;
    if (env < 0)
        env = g_getenv ("WALPE_POINTER_SHARE") != NULL;
    return env || g_atomic_int_get (&D.cfg->pointer_share);
}

static void mon_check_shareable (Mon *m, const char *path)
{
    const char *why = NULL;
    if (!m->content)
        why = "load failed";
    else if (m->content->type != WALPE_TYPE_SCENE)
        why = "not a scene";
    else if (m->content->uses_pointer_pos && !pointer_share_on () &&
             (g_atomic_int_get (&D.cfg->parallax) || m->content->wants_pointer))
        why = "the scene follows the pointer position (parallax effects, cursor scripts or "
              "mouse-linked particles)";
    if (!why)
        return;
    SharedFallback *fb = g_new0 (SharedFallback, 1);
    fb->gen = m->lead_gen;
    fb->leader = m;
    fb->path = g_strdup (path);
    fb->why = why;
    g_idle_add (shared_fallback_cb, fb);
}

#define MON_SETTLE_US (3 * G_USEC_PER_SEC)

static gpointer mon_thread (gpointer data)
{
    Mon *m = data;
    g_mutex_lock (&ctx_lock);
    m->ctx = create_thread_context ();
    ctx_created++;
    g_cond_broadcast (&ctx_cond);
    while (ctx_created < ctx_expected)
        g_cond_wait (&ctx_cond, &ctx_lock);
    g_mutex_unlock (&ctx_lock);
    if (m->ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent (D.egl_dpy, m->surf, m->surf, m->ctx)) {
        g_warning ("monitor %d: EGL context unusable; thread exits", m->index);
        return NULL;
    }
    mon_present_apply (m);   /* the swap interval of this monitor's window */
    {
        /* one monitor thread at a time: what glutil builds once for all of
         * them on first use (the shader header text) is built by the first
         * and only read by the others, instead of by two at once */
        static GMutex glutil_init_lock;
        g_mutex_lock (&glutil_init_lock);
        walpe_glutil_init (); /* thread-local program/VAO state */
        g_mutex_unlock (&glutil_init_lock);
    }
    sw_note_gl ();        /* GL_VENDOR / GL_RENDERER: logged once, in `status` */
    g_atomic_int_set (&D.egl_is_es, epoxy_is_desktop_gl () ? 0 : 1);

    g_mutex_lock (&m->lock);
    while (!m->quitting) {
        char *load = m->load_path;
        m->load_path = NULL;
        gboolean keep = m->load_keep;
        m->load_keep = FALSE;
        guint load_tag = load ? m->load_tag : 0;   /* (its own reload after an idle release: 0) */
        gboolean blank = m->blank_req;
        m->blank_req = FALSE;
        gboolean render = m->render_req;
        m->render_req = FALSE;
        gboolean paused = m->pause_wanted;
        gboolean self_paused = m->self_paused;
        int fps = m->fps;
        gboolean fps_changed = m->fps_changed;
        m->fps_changed = FALSE;
        WalpeQuality quality = m->quality;
        m->rt_render_scale = quality.render_scale;
        gboolean quality_changed = m->quality_changed;
        m->quality_changed = FALSE;
        gboolean stats_wanted = m->stats_until_us > g_get_monotonic_time ();
        gboolean describe_req = m->describe_req;
        m->describe_req = FALSE;
        gboolean stats_req = m->stats_req;
        m->stats_req = FALSE;
        gboolean have_stats = m->qstats_ok;
        GArray *ptr = NULL;
        gboolean role = m->role_change;
        SharedFrame *rsf = m->role_sf;
        int ridx = m->role_idx;
        guint rgen = m->role_gen;
        m->role_change = FALSE;
        m->role_sf = NULL;
        gboolean ptr_dirty = m->ptr_dirty;
        m->ptr_dirty = FALSE;
        gboolean prof_sync = m->prof_sync;   /* GPUPROF */
        m->prof_sync = FALSE;
        float ptr_x = m->mx, ptr_y = m->my;
        /* "Adjust layers": the user changed a layer (or asked for the layer
         * list) - one frame now, whatever the pause state */
        gboolean force = m->force_frame;
        m->force_frame = FALSE;
        gint64 force_until = m->force_until_us;
        gboolean idle_release = FALSE;
        if (!paused)
            m->paused_since_us = 0;
        else if (!m->paused_since_us)
            m->paused_since_us = g_get_monotonic_time ();
        /* a parallax pointer move: an animating scene shows it with its
         * next frame (m->mx/my are read then); only a scene with no frame
         * clock running needs a frame for it. (The pointer is polled at up
         * to the monitor rate now: requesting a frame per move would
         * double the frame rate while the mouse moves.) */
        if (ptr_dirty && !paused && m->content && !m->content->next_frame_us)
            render = TRUE;
        /* the X server exposed a part of the window (Expose, main thread):
         * the frame it shows is gone there. A content with a frame clock
         * draws its next frame for real (surface_lost) when that is due;
         * one without a clock, or a follower of a shared scene, needs a
         * frame for it now. */
        if (g_atomic_int_compare_and_exchange (&m->exposed, 1, 0)) {
            m->win_has_frame = FALSE;
            m->win_exposed = TRUE;
            /* also while paused: a paused or static
             * wallpaper kept black holes until the next load. One forced
             * frame; an animating wallpaper does it with its next one. */
            m->needs_first_frame = TRUE;
            if (paused || m->follow_sf || !m->content || !m->content->next_frame_us)
                render = TRUE;
        }
        /* "Skip hidden areas": the boxes this frame is rendered into
         * (clip_n -1 = everything: always, unless the setting is on) */
        int clip_n = m->clip_n;
        WalpeBox clip[WALPE_CLIP_MAX];
        if (clip_n > 0)
            memcpy (clip, m->clip, sizeof (WalpeBox) * (gsize) clip_n);
        guint64 clip_gen = m->clip_serial, follow_need = m->follow_need;
        if (m->clip_grew) {
            /* Something became visible: there the screen shows the window
             * background (no compositor) or what was drawn long ago (a
             * compositor) until a frame draws it - also while paused, and
             * for content that has no frame clock. One frame at once; not
             * more often than the monitor refreshes (a window dragged
             * about uncovers pixels with every pointer motion). */
            m->clip_grew = FALSE;
            m->clip_owed = TRUE;
            gint64 civ = (gint64) (1e6 / (m->hz >= 20.0 ? m->hz : 60.0));
            gint64 at = MAX (g_get_monotonic_time (), m->last_draw_us + civ);
            if (!m->clip_due_us || at < m->clip_due_us)
                m->clip_due_us = at;
        }
        gboolean settle = m->settle_us && g_get_monotonic_time () >= m->settle_us;
        /* a video whose first picture is waited for: a turn every quarter
         * second, whatever else happens (mon_first_frame_tick). Until
         * then the sleep below ends at first_due at the latest - also on a
         * paused monitor, which is not unloaded (idle_release) meanwhile */
        const gint64 first_due = m->first_path ? m->first_tick_us + FIRST_TICK_US : 0;
        const gboolean first_tick = first_due && g_get_monotonic_time () >= first_due;
        if (!load && !render && !role && !fps_changed && !settle && !prof_sync && !blank &&
            !quality_changed && !describe_req && !stats_req && !first_tick) {
            /* sleep until woken, or until the content's next frame is due */
            gint64 due = (m->content && !paused && m->pause_applied == paused)
                             ? m->content->next_frame_us : 0;
            if (m->follow_sf && !paused)
                due = m->follow_wait_us;   /* following: its next refresh slot */
            if (first_due && (!due || first_due < due))
                due = first_due;           /* (not yet: first_tick is clear) */
            if (m->clip_due_us && (!due || m->clip_due_us < due))
                due = m->clip_due_us;      /* "Skip hidden areas": what became visible */
            if (m->content && paused != m->pause_applied) {
                /* pause transition only: apply it, no frame */
            } else if (!due) {
                /* nothing to draw: sleep until woken - or, idle_release,
                 * until the paused wallpaper is to be unloaded; or until the
                 * memory freed by a switch is to be handed back (settle) */
                gint64 idle = P.idle_release_us && paused && m->content && m->content_path &&
                              !m->follow_sf   /* (a follower has no wallpaper of its own) */
                                  ? m->paused_since_us + P.idle_release_us : 0;
                if (!idle || g_get_monotonic_time () < idle) {
                    gint64 until = idle;
                    if (m->settle_us && (!until || m->settle_us < until))
                        until = m->settle_us;
                    m->clock_wait = FALSE;
                    if (until)
                        g_cond_wait_until (&m->cond, &m->lock, until);
                    else
                        g_cond_wait (&m->cond, &m->lock);
                    g_atomic_int_inc (&m->wakeups);
                    continue;
                }
                idle_release = TRUE;
            } else if (g_get_monotonic_time () < due) {
                /* at `due` it renders (its content's frame clock)
                 * or presents the newest frame (a follower's slot) by
                 * itself: pointer_sample / sf_publish do not wake it */
                m->clock_wait = TRUE;
                g_cond_wait_until (&m->cond, &m->lock,
                                   m->settle_us ? MIN (due, m->settle_us) : due);
                m->clock_wait = FALSE;
                g_atomic_int_inc (&m->wakeups);
                continue;
            } else {
                render = TRUE;
            }
        }
        /* pointer events ride along with the next frame (a press/release
         * also requests one: mon_queue_pointer) */
        if (m->ptr_q && m->ptr_q->len) {
            ptr = g_array_copy (m->ptr_q);
            g_array_set_size (m->ptr_q, 0);
        }
        g_mutex_unlock (&m->lock);
        if (prof_sync)
            mon_prof_sync (m);   /* GPUPROF */
        if (idle_release)
            mon_idle_release (m);
        if (role) {
            mon_drop_role (m);
            if (rsf && ridx >= 0) {
                /* follower: no scene of its own, presents the leader's */
                g_atomic_int_set (&m->idle_released, 0);
                mon_free_content (m);
                g_clear_pointer (&m->content_path, g_free);
                m->follow_sf = rsf;
                m->follow_idx = ridx;
                m->needs_first_frame = TRUE;
                /* the leader may have published already (it took its role
                 * first and, leading with the scene it had, drew at once):
                 * its wake-up then came before this role and is used up.
                 * Look now; a scene that does not animate sends no other. */
                render = TRUE;
                g_atomic_int_set (&m->content_type, WALPE_TYPE_SCENE);
                malloc_trim (0);
                m->settle_us = g_get_monotonic_time () + MON_SETTLE_US;
                g_idle_add (content_note_cb, NULL);
                if (load) {
                    /* taken in this same turn, queued before the role
                     * (mon_assign drops the ones not taken yet) */
                    g_free (load);
                    load = NULL;
                }
            } else if (rsf) {
                m->lead_sf = rsf;
                m->lead_gen = rgen;
                m->sf_has_frame = FALSE;   /* a new group: nothing published yet */
            }
        }
        /* idle_release: the monitor resumed - load its wallpaper again
         * (unless it was told to load something, or follows a leader now) */
        if (!load && !paused && !m->follow_sf && m->content_path &&
            g_atomic_int_get (&m->idle_released))
            load = g_strdup (m->content_path);
        const gboolean did_load = load != NULL;
        if (load) {
            mon_do_load (m, load, paused, fps, &quality, keep, load_tag);
            g_free (m->content_path);
            m->content_path = m->content ? g_strdup (load) : NULL;
            if (m->lead_sf)
                mon_check_shareable (m, load);
            g_free (load);
            render = TRUE;
            m->settle_us = g_get_monotonic_time () + MON_SETTLE_US;
            g_atomic_int_set (&m->idle_released, 0);
            if (paused)   /* loaded while paused: the idle time starts over */
                m->paused_since_us = g_get_monotonic_time ();
        } else if (role && m->lead_sf && !g_atomic_int_get (&m->idle_released)) {
            /* leading with the content it already had (regroup, parallax
             * change): check again */
            mon_check_shareable (m, m->content_path ? m->content_path : "");
            /* and the monitors that follow it now have no frame of it: a
             * scene that does not animate has no frame clock and never
             * gave them one (a monitor that returned to the wallpaper its
             * neighbour shows - load_ended, `open` on one monitor - kept
             * its old picture). One frame now, also while paused. */
            m->needs_first_frame = TRUE;
            render = TRUE;
        }
        if (m->follow_sf && m->content) {
            /* the invariant behind the two fixes above: a follower owns no
             * wallpaper. Should a path be left that breaks it, say so and
             * give the memory / the decoder back instead of carrying it */
            g_warning ("monitor %d follows another monitor but had a wallpaper of its own "
                       "(%s): freed", m->index, m->content_path ? m->content_path : "?");
            mon_free_content (m);
            g_clear_pointer (&m->content_path, g_free);
            g_atomic_int_set (&m->content_type, WALPE_TYPE_SCENE);
        }
        /* a failed load left the windows as they were and the main thread
         * found no wallpaper to open instead: the black frame after all
         * (unless a newer load, or a leader, has taken over since) */
        if (blank && !load && !m->content && !m->follow_sf)
            mon_load_failed_frame (m, FALSE);
        if (paused != m->pause_applied)
            mon_apply_pause (m, paused);
        /* live fps cap (a load above already used the new value; shared
         * followers have no content: their leader paces them) */
        if (fps_changed && !load && m->content && m->content->set_fps)
            m->content->set_fps (m->content, mon_content_fps (m, m->content->type, fps));
        /* quality knobs changed for a running wallpaper (a load above
         * already got them): effect rate / post-processing, render scale.
         * A scene with no frame clock shows the change with one frame. */
        if (quality_changed && !load && m->content) {
            if (m->content->set_quality)
                m->content->set_quality (m->content, &quality);
            if (m->content->set_render_scale)
                m->content->set_render_scale (m->content, quality.render_scale);
            if (m->content->set_quality || m->content->set_render_scale)
                render = TRUE;
        }
        if (m->content)
            m->content->stats_wanted = stats_wanted;
        if (ptr) {
            for (guint i = 0; m->content && m->content->pointer && i < ptr->len; i++) {
                PtrMsg *pm = &g_array_index (ptr, PtrMsg, i);
                m->content->pointer (m->content, pm->kind, pm->x, pm->y);
            }
            g_array_unref (ptr);
        }
        /* "Skip hidden areas": a frame is owed for what became visible,
         * paused or not; the clip of this frame goes to the renderers */
        gboolean clip_owed = m->clip_owed, shown = FALSE;
        if (render) {
            m->clip_due_us = 0;
            if (clip_n >= 0)
                walpe_clip_set (clip, clip_n, m->geom.width, m->geom.height);
            else
                walpe_clip_off ();
            m->last_draw_us = g_get_monotonic_time ();
        }
        if (render && m->follow_sf) {
            /* the leader (the group's fastest monitor) publishes more
             * frames than a slower monitor shows: take the newest once per
             * refresh. Where the swap blocks on vsync that already
             * happens; where it does not (a compositor, Xvfb) this keeps a
             * 60 Hz follower from presenting 144 times a second. */
            gint64 now = g_get_monotonic_time ();
            gint64 iv = m->hz >= 20.0 && !D.no_refresh_cap ? (gint64) (1e6 / ceil (m->hz - 0.005)) : 0;
            /* ... and only then: a leader paced at or below this
             * monitor's refresh rate (limit 60 on a 144 Hz + 60 Hz pair)
             * cannot publish too many. Waiting for "its next refresh slot"
             * there woke this thread twice per frame whenever the leader's
             * frames arrived a little before the slot, and showed them up to
             * one refresh late. WALPE_FOLLOW_GATE=1 = the old rule (A/B). */
            if (g_atomic_int_get (&m->follow_free))
                iv = 0;
            /* its own frame rate limit, where that is lower than
             * the leader's (focus_action "limit" while an application on
             * THIS monitor has the focus): the newest frame at that rate.
             * A follower used to present every frame of its leader. */
            int fcap = g_atomic_int_get (&m->follow_cap);
            if (fcap > 0)
                iv = MAX (iv, (gint64) (1000000 / fcap));
            gboolean fforce = now < force_until;
            if (iv && !m->needs_first_frame && !fforce && now < m->present_us + iv - 500) {
                m->follow_wait_us = m->present_us + iv;
            } else {
                m->follow_wait_us = 0;
                /* (a frame that lacks what this monitor shows now is not
                 * presented: clip_owed stays until the leader's next one) */
                if ((!paused || m->needs_first_frame || fforce || clip_owed) &&
                    sf_present (m, follow_need)) {
                    m->clip_owed = FALSE;
                    shown = TRUE;
                    m->needs_first_frame = FALSE;
                    /* phase-locked while frames keep coming */
                    m->present_us = iv && now < m->present_us + 2 * iv ? m->present_us + iv : now;
                }
            }
        } else if (render) {
            mon_do_render (m, paused, m->needs_first_frame || force || clip_owed, self_paused,
                           ptr_x, ptr_y, clip_gen);
            m->needs_first_frame = m->content && m->content->first_frame_pending;
            m->clip_owed = FALSE;
            shown = TRUE;
        }
        if (m->first_path)
            mon_first_frame_tick (m);   /* the video still has no picture */
        if (render)
            walpe_clip_off ();
        if (m->settle_us && g_get_monotonic_time () >= m->settle_us) {
            if (m->needs_first_frame && m->settle_waits++ < 20) {
                /* the new wallpaper has not drawn yet: after its first frame */
                m->settle_us = g_get_monotonic_time () + MON_SETTLE_US / 2;
            } else {
                m->settle_us = 0;
                m->settle_waits = 0;
                glFinish ();   /* deletes are only queued until the driver has run them */
                malloc_trim (0);
            }
        }
        /* quality numbers for `walpe-ctl quality` / the GUI, while asked for */
        WalpeQualityStats qs;
        gboolean qs_ok = (stats_wanted || describe_req) && m->content &&
                         m->content->quality_stats &&
                         (describe_req || !have_stats || m->render_count % 8 == 0) &&
                         m->content->quality_stats (m->content, &qs);
        char *qd = describe_req && m->content && m->content->quality_describe
                       ? m->content->quality_describe (m->content) : NULL;
        g_mutex_lock (&m->lock);
        if (shown)
            m->frames_done++;
        if (clip_owed && !m->clip_owed && m->grew_us && !m->clip_grew) {
            /* "Skip hidden areas": what became visible is on screen: how
             * long it took */
            gint64 lat = g_get_monotonic_time () - m->grew_us;
            m->grew_us = 0;
            m->lat_frames_last = (guint) (m->frames_done - m->grew_frames);
            m->lat_frames_max = MAX (m->lat_frames_max, m->lat_frames_last);
            m->lat_last_us = lat;
            m->lat_max_us = MAX (m->lat_max_us, lat);
            m->lat_sum_us += lat;
            m->lat_n++;
        }
        if (qs_ok) {
            m->qstats = qs;
            m->qstats_ok = TRUE;
        }
        if (describe_req) {
            g_free (m->qdescribe);
            m->qdescribe = qd;
        }
        if (did_load)
            m->qstats_ok = qs_ok;   /* another wallpaper: the old numbers are not its */
    }
    if (m->role_sf)
        sf_unref (m->role_sf);
    m->role_sf = NULL;
    g_mutex_unlock (&m->lock);
    /* release everything owned by this thread */
    walpe_prof_thread_exit ();   /* GPUPROF */
    mon_drop_role (m);
    mon_free_content (m);
    g_clear_pointer (&m->content_path, g_free);
    eglMakeCurrent (D.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (D.egl_dpy, m->ctx);
    m->ctx = EGL_NO_CONTEXT;
    eglReleaseThread ();
    return NULL;
}

/* ----------------------------------------------------- main-thread API */

/* main thread, one frame interval after the first Expose event of a burst:
 * tell the monitor's render thread that its window lost pixels. Whether
 * that takes a frame of its own is the thread's call (a content with a
 * frame clock draws its next frame anyway). */
static gboolean expose_fire (gpointer user)
{
    Mon *m = user;
    m->expose_timer_id = 0;
    g_atomic_int_set (&m->exposed, 1);
    g_mutex_lock (&m->lock);
    g_cond_signal (&m->cond);
    g_mutex_unlock (&m->lock);
    return G_SOURCE_REMOVE;
}

/* thread-safe: called from decode threads, the web snapshotter, main */
static void request_redraw (gpointer user)
{
    Mon *m = user;
    g_mutex_lock (&m->lock);
    m->render_req = TRUE;
    g_cond_signal (&m->cond);
    g_mutex_unlock (&m->lock);
}

G_GNUC_UNUSED static void invoke_queue_render (Mon *m)
{
    request_redraw (m);
}

/* main thread: queue a pointer event (x, y in [0,1] of the monitor) for
 * the content's SceneScript cursor events; the render thread delivers it
 * before its next frame */
static void mon_queue_pointer (Mon *m, int kind, float x, float y)
{
    g_mutex_lock (&m->lock);
    if (!m->ptr_q)
        m->ptr_q = g_array_new (FALSE, FALSE, sizeof (PtrMsg));
    if (m->ptr_q->len < 64) {
        PtrMsg pm = { kind, x, y };
        g_array_append_val (m->ptr_q, pm);
    }
    if (kind != WALPE_PTR_MOVE) {
        m->render_req = TRUE;
        g_cond_signal (&m->cond);
    }
    g_mutex_unlock (&m->lock);
}

static Mon *mon_at (int rx, int ry)
{
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        if (rx >= m->geom.x && rx < m->geom.x + m->geom.width &&
            ry >= m->geom.y && ry < m->geom.y + m->geom.height)
            return m;
    }
    return NULL;
}

static void apply_pause_state (void);

/* main thread: give a monitor thread its next wallpaper (path, or NULL =
 * keep the content) and role: sf + idx -1 = lead sf, idx >= 0 = follow
 * sf, sf NULL = render on its own. keep: should that load fail, a monitor
 * of its group has a previous wallpaper to return to (load_keep). */
static void mon_assign (Mon *m, const char *path, SharedFrame *sf, int idx, gboolean keep)
{
    g_mutex_lock (&m->lock);
    if (path) {
        static guint tags;
        g_free (m->load_path);
        m->load_path = g_strdup (path);
        m->load_keep = keep;
        if (!++tags)
            tags = 1;
        m->load_tag = m->load_wait = tags;
        m->loading = TRUE;   /* until this load's note (load_note_cb) */
        /* the knobs this wallpaper runs with (global + its own) */
        walpe_config_quality_for (D.cfg, path, &m->quality);
        m->fps = m->fps_cap = mon_fps_cap (m);
    } else if (sf && idx >= 0) {
        /* it follows another monitor from now on: a load queued by an
         * earlier apply_wallpapers that its thread has not taken yet (it
         * was still creating its context, or loading) must not run. It
         * did: the monitor presented the leader's frames AND kept a
         * wallpaper of its own nobody saw - a video went on decoding at
         * full rate, a scene held its textures (`open` right after
         * start-up, two quick opens, the shared-rendering fallback). */
        g_clear_pointer (&m->load_path, g_free);
        m->loading = FALSE;   /* its leader loads */
    }
    if (m->role_sf)
        sf_unref (m->role_sf);   /* a pending role nobody adopted yet */
    m->role_sf = sf_ref (sf);
    m->role_idx = idx;
    m->role_gen = D.wall_gen;
    m->role_change = TRUE;
    g_cond_signal (&m->cond);
    g_mutex_unlock (&m->lock);
}

/* main thread: the failed load of m is final. Its thread left the windows
 * as they were, expecting another wallpaper (load_keep): the black frame. */
static void mon_blank (Mon *m)
{
    g_mutex_lock (&m->lock);
    m->blank_req = TRUE;
    g_cond_signal (&m->cond);
    g_mutex_unlock (&m->lock);
}

static gboolean shared_render_possible (void)
{
    return D.cfg->render_shared && !g_getenv ("WALPE_NO_SHARED_RENDER") &&
           share_root != EGL_NO_CONTEXT && !g_atomic_int_get (&D.share_failed) &&
           D.mons && D.mons->next;
}

/* main thread: bring every monitor to its wallpaper (config: the monitor's
 * own entry in monitor_current, else `current`). A monitor reloads only
 * when its wallpaper changed or a reload is forced: force_path = every
 * monitor showing that path (re-open, property change), force_mon = that
 * monitor, force_all = all. Monitors showing the SAME wallpaper at the SAME
 * size, if it is a scene, form a render_shared group: the first renders,
 * the others follow it. regroup = rebuild every group (parallax change:
 * leaders re-check whether their scene can be shared). */
/* crash-loop guard, per monitor. "<socket>.loading" lists "<output>\t<path>"
 * for every monitor that started loading (or following) a wallpaper in the
 * last LOADMARK_SECS; it is removed then, and on a clean quit. A daemon
 * that finds the file at start-up died while those loads ran (a WebKit
 * abort took every monitor with it, and the GUI respawned the daemon into
 * the same wallpaper every ~3 s): each listed monitor whose wallpaper is
 * still that path starts without it (other monitors load normally), the
 * reason is logged and status lists it under "load_skipped". The skip is
 * in memory only: opening a wallpaper on that monitor (or on all) clears
 * it, and the next daemon start tries the wallpaper again.
 *
 * Two strikes: the file only says "a load started less than LOADMARK_SECS
 * ago", so ONE death in that window proves nothing about the wallpaper -
 * logging out of X, closing the terminal, kill -9, the OOM killer or a
 * power cut within 20 s of a start or a wallpaper switch all left the next
 * session without wallpaper. Each line carries a count ("<output>\t<path>
 * \t<n>"): a start that finds n = 1 loads the wallpaper again and writes
 * n = 2; only a second death in a row while loading the same wallpaper on
 * the same monitor skips it. A real crash loop costs one more round. */
#define LOADMARK_SECS 20
#define LOADMARK_STRIKES 2
static GHashTable *loadmark;    /* output -> "<path>\t<n>", loads in the current window */
static guint loadmark_timer;
static GHashTable *load_skip;   /* output -> path not loaded at start-up */
static GHashTable *load_struck; /* output -> path the previous daemon died on once */

static char *loadmark_file (void)
{
    char *sock = walpe_socket_path ();
    char *p = g_strconcat (sock, ".loading", NULL);
    g_free (sock);
    return p;
}

static gboolean loadmark_clear (gpointer user)
{
    (void) user;
    char *p = loadmark_file ();
    unlink (p);
    g_free (p);
    if (loadmark)
        g_hash_table_remove_all (loadmark);
    if (load_struck)    /* these loads survived their window: no strike left */
        g_hash_table_remove_all (load_struck);
    loadmark_timer = 0;
    return G_SOURCE_REMOVE;
}

static void loadmark_add (const char *mon, const char *path)
{
    if (!loadmark)
        loadmark = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    /* second strike when the previous daemon died on this very load */
    const char *struck = load_struck ? g_hash_table_lookup (load_struck, mon) : NULL;
    int n = struck && !g_strcmp0 (struck, path) ? 2 : 1;
    g_hash_table_replace (loadmark, g_strdup (mon), g_strdup_printf ("%s\t%d", path, n));
    GString *s = g_string_new (NULL);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, loadmark);
    while (g_hash_table_iter_next (&it, &k, &v))
        g_string_append_printf (s, "%s\t%s\n", (char *) k, (char *) v);
    char *p = loadmark_file ();
    g_file_set_contents (p, s->str, (gssize) s->len, NULL);
    g_free (p);
    g_string_free (s, TRUE);
    if (loadmark_timer)
        g_source_remove (loadmark_timer);
    loadmark_timer = g_timeout_add_seconds (LOADMARK_SECS, loadmark_clear, NULL);
}

/* start-up, before the first apply_wallpapers */
static void loadmark_check (void)
{
    char *p = loadmark_file ();
    char *txt = NULL;
    if (g_file_get_contents (p, &txt, NULL, NULL)) {
        char **lines = g_strsplit (txt, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *tab = strchr (lines[i], '\t');
            if (!tab)
                continue;
            *tab = 0;
            const char *mon = lines[i], *path = tab + 1;
            /* "<path>\t<n>"; a file from before the count has none: 1 */
            char *t2 = strrchr (tab + 1, '\t');
            int n = 1;
            if (t2 && t2[1] && strspn (t2 + 1, "0123456789") == strlen (t2 + 1)) {
                n = atoi (t2 + 1);
                *t2 = 0;
            }
            if (g_strcmp0 (walpe_config_monitor_path (D.cfg, mon), path))
                continue;   /* that monitor was given something else since */
            if (n < LOADMARK_STRIKES) {
                /* once is no proof (X exit, kill, power cut): load it
                 * again, remember the strike */
                g_message ("%s on %s: the previous daemon ended within %d s of loading it; "
                           "loading it again (not loaded next time if that repeats)", path, mon,
                           LOADMARK_SECS);
                if (!load_struck)
                    load_struck = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
                g_hash_table_replace (load_struck, g_strdup (mon), g_strdup (path));
                continue;
            }
            g_warning ("not loading %s on %s: the daemon died twice in a row while loading it "
                       "(open it again to retry)", path, mon);
            if (!load_skip)
                load_skip = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
            g_hash_table_replace (load_skip, g_strdup (mon), g_strdup (path));
        }
        g_strfreev (lines);
        g_free (txt);
        unlink (p);
    }
    g_free (p);
}

/* main thread: running wallpapers pick up a new fps cap now (scenes and
 * web capture; video pacing does not use it) instead of at the next load */
/* hand every monitor thread its frame rate limit when it changed (the fps
 * setting, the focus / battery rules, the wallpaper's own fps_idle). A
 * leader renders for its followers too: it gets the highest limit among
 * the group's visible monitors. */
static void fps_refresh (void)
{
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        int cap = mon_fps_cap (m);
        if (m->nfollowers) {
            int best = m->own_paused ? 0 : cap;
            for (GList *l2 = D.mons; l2; l2 = l2->next) {
                Mon *o = l2->data;
                if (o->leader == m && !o->own_paused)
                    best = MAX (best, mon_fps_cap (o));
            }
            if (best)
                cap = best;
        }
        if (cap == m->fps_cap)
            continue;
        m->fps_cap = cap;
        g_mutex_lock (&m->lock);
        m->fps = cap;
        m->fps_changed = TRUE;
        g_cond_signal (&m->cond);
        g_mutex_unlock (&m->lock);
    }
    pointer_repace ();
    follow_pace_update ();
}

/* main: which followers may present every frame of their leader
 * as it arrives (Mon.follow_free): the leader's frame clock - the fps
 * setting, at most its own refresh rate - is not faster than the
 * follower's display. Roles, the fps setting. */
static void follow_pace_update (void)
{
    static int gate = -1;
    if (gate < 0) {
        gate = g_getenv ("WALPE_FOLLOW_GATE") != NULL;
        g_atomic_int_set (&follow_always_wake, gate);
    }
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        gboolean fr = !gate && m->leader && !D.no_refresh_cap &&
                      walpe_follow_free (D.cfg->fps, m->leader->hz, m->hz);
        g_atomic_int_set (&m->follow_free, fr);
        g_atomic_int_set (&m->follow_cap,
                          m->leader ? walpe_follow_cap (mon_fps_cap (m), m->leader->fps_cap) : 0);
    }
}

/* main: the apply_wallpapers running is load_ended's re-open of previous
 * wallpapers (what is known about the failed load stays) */
static gboolean restore_apply;
/* main: the choice the last `open` for every monitor replaced */
static WalpeOpenUndo open_undo;

static void apply_wallpapers (const char *force_path, Mon *force_mon, gboolean force_all,
                              gboolean regroup);

/* ---- settings that reload scenes --------------------------------------
 * Texture resolution, texture slack, "Downscale oversized textures" and
 * the effect shader switches are read while a scene loads: a change loads
 * the scenes on screen again. One request can carry several of them (a
 * quality preset sets all at once), so the settings only mark the monitors
 * (scenes_reload_mark) and the request ends with one reload_flush ():
 *  - one load per scene, started when every value of the request is in
 *    place. (the earlier build reloaded from inside each setting: a preset that changes
 *    the texture slack and the effect shaders queued two reloads, and the
 *    first one could start with half of the new values.)
 *  - only monitors that show a scene of their own. (the earlier build reloaded every
 *    monitor: a video next to the scene started over, a web page was loaded
 *    again.) A monitor that is still loading counts as one: what it loads
 *    is not known here yet, and the load under way began with the old
 *    values - the earlier build asked for content_type == scene and so left a scene
 *    alone that was still loading when the setting arrived.
 * A monitor that follows another one has no wallpaper of its own (its
 * leader is marked), one whose wallpaper was unloaded while paused
 * (idle_release) loads with the values of then. */
static gboolean mon_shows_scene (Mon *m)
{
    return !m->leader && m->want_path && *m->want_path &&
           (g_atomic_int_get (&m->content_type) == WALPE_TYPE_SCENE || m->loading);
}

/* main: the scenes on screen (path: only that wallpaper's) load again at
 * the next reload_flush () */
static void scenes_reload_mark (const char *path)
{
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        if (mon_shows_scene (m) && (!path || !g_strcmp0 (m->want_path, path)))
            m->reload_wanted = TRUE;
    }
}

/* main, at the end of a request that may have marked monitors */
static void reload_flush (void)
{
    for (GList *l = D.mons; l; l = l->next)
        if (((Mon *) l->data)->reload_wanted) {
            apply_wallpapers (NULL, NULL, FALSE, FALSE);   /* takes every mark */
            return;
        }
}

/* ---- quality knobs (common/quality.h) ----------------------------------
 * main thread: knobs changed (`changed`: WALPE_Q_* bits; path = one
 * wallpaper's own settings, NULL = the global ones). Every monitor gets
 * what its wallpaper runs with now: the frame rate limit and the live knobs
 * (effect rate and resolution, post-processing, render scale) at once; a
 * different texture resolution or texture slack reloads the wallpapers it
 * applies to. */
static void quality_apply (guint changed, const char *path, gboolean reload_all)
{
    (void) path;
    /* reload_all: a setting that scenes read when they load changed with
     * the knobs (a preset's effect shader keys) - the one load asked for
     * here serves both */
    if (!changed && !reload_all)
        return;
    /* the global render scale and texture slack have config fields of
     * their own (status, older requests): they follow the knobs */
    walpe_config_quality_sync (D.cfg);
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        WalpeQuality q;
        walpe_config_quality_for (D.cfg, mon_quality_path (m), &q);
        g_mutex_lock (&m->lock);
        gboolean live = q.fx_rate != m->quality.fx_rate || q.fx_rate_all != m->quality.fx_rate_all ||
                        q.post != m->quality.post || q.render_scale != m->quality.render_scale ||
                        q.fx_res != m->quality.fx_res;
        /* texture resolution and texture slack are decided when a scene's
         * textures are loaded: the callers end with reload_flush () */
        if ((q.tex_res != m->quality.tex_res || q.tex_slack != m->quality.tex_slack) &&
            mon_shows_scene (m))
            m->reload_wanted = TRUE;
        m->quality = q;
        if (live) {
            m->quality_changed = TRUE;
            g_cond_signal (&m->cond);
        }
        g_mutex_unlock (&m->lock);
    }
    fps_refresh ();
    if (reload_all)
        scenes_reload_mark (NULL);   /* (the caller flushes: reload_flush) */
}

static void apply_wallpapers (const char *force_path, Mon *force_mon, gboolean force_all,
                              gboolean regroup)
{
    /* own copy: callers pass a monitor's want_path (ipc_target_path),
     * which the loop below frees and replaces (heap-use-after-free with
     * setprop/setprops/resetprops {"monitor": N}) */
    char *force_own = g_strdup (force_path);
    force_path = force_own;
    D.wall_gen++;
    guint n = g_list_length (D.mons);
    Mon **ms = g_new0 (Mon *, n);
    Mon **lead = g_new0 (Mon *, n);   /* new leader of monitor i (NULL = leads/solo) */
    gboolean *load = g_new0 (gboolean, n);
    guint i = 0;
    for (GList *l = D.mons; l; l = l->next, i++) {
        Mon *m = ms[i] = l->data;
        char *was = m->want_path;
        m->want_path = g_strdup (walpe_config_monitor_path (D.cfg, m->name));
        const char *skip = load_skip ? g_hash_table_lookup (load_skip, m->name) : NULL;
        if (skip && !g_strcmp0 (skip, m->want_path)) {
            g_free (m->want_path);
            m->want_path = g_strdup ("");   /* crash-loop guard: stays empty */
        }
        if (g_strcmp0 (was, m->want_path) && !restore_apply) {
            /* it was given another wallpaper (or none): how the load of the
             * one before ended is history, and so is a re-open */
            g_clear_pointer (&m->reopen_for, g_free);
            g_clear_pointer (&m->load_error, g_free);
            g_clear_pointer (&m->restored, g_free);
        }
        g_free (was);
    }
    gboolean can = shared_render_possible ();
    gboolean *grouped = g_new0 (gboolean, n);   /* already in a group (its leader too) */
    GHashTable *is_scene = g_hash_table_new (g_str_hash, g_str_equal);
    for (i = 0; i < n; i++) {
        Mon *m = ms[i];
        if (lead[i] || grouped[i] || !can || !*m->want_path ||
            (D.noshare && g_hash_table_contains (D.noshare, m->want_path)))
            continue;
        gpointer sc;
        if (!g_hash_table_lookup_extended (is_scene, m->want_path, NULL, &sc)) {
            sc = GINT_TO_POINTER (walpe_detect_type (m->want_path, NULL) == WALPE_TYPE_SCENE);
            g_hash_table_insert (is_scene, m->want_path, sc);
        }
        if (!sc)
            continue;
        /* the group: this monitor and every later one showing the same
         * scene at the same size. Its fastest monitor renders (pace.h). */
        guint grp[SF_MAX_FOLLOWERS + 1];
        double ghz[SF_MAX_FOLLOWERS + 1];
        int ng = 0;
        grp[ng] = i;
        ghz[ng++] = m->hz;
        for (guint j = i + 1; j < n && ng <= SF_MAX_FOLLOWERS; j++) {
            Mon *o = ms[j];
            if (!lead[j] && !grouped[j] && !g_strcmp0 (o->want_path, m->want_path) &&
                o->geom.width == m->geom.width && o->geom.height == m->geom.height) {
                grp[ng] = j;
                ghz[ng++] = o->hz;
            }
        }
        Mon *ld = ms[grp[D.leader_first ? 0 : walpe_pick_leader (ghz, ng)]];
        for (int k = 0; ng > 1 && k < ng; k++) {
            grouped[grp[k]] = TRUE;
            if (ms[grp[k]] != ld)
                lead[grp[k]] = ld;
        }
    }
    g_free (grouped);
    g_hash_table_unref (is_scene);
    for (i = 0; i < n; i++) {
        Mon *m = ms[i];
        load[i] = *m->want_path && !lead[i] &&
                  (force_all || m == force_mon || !g_strcmp0 (m->want_path, force_path) ||
                   m->reload_wanted || g_strcmp0 (m->loaded_path, m->want_path));
        m->reload_wanted = FALSE;   /* (scenes_reload_mark) taken, or void: it follows now */
        /* nothing wanted any more (the wallpaper was forgotten: it is
         * being deleted): "load" the empty path = unload, window black */
        if (!*m->want_path && ((m->loaded_path && *m->loaded_path) || m->leader))
            load[i] = TRUE;
    }
    /* crash-loop guard: every monitor about to load a wallpaper, and the
     * followers of a leader that loads one, show it if the load kills us */
    for (i = 0; i < n; i++) {
        Mon *src = lead[i] ? lead[i] : ms[i];
        guint si = 0;
        while (si < n && ms[si] != src)
            si++;
        if (si < n && load[si] && *ms[i]->want_path)
            loadmark_add (ms[i]->name, ms[i]->want_path);
    }
    /* apply per leader/solo monitor, comparing with the current roles */
    for (i = 0; i < n; i++) {
        Mon *m = ms[i];
        if (lead[i])
            continue;
        Mon *fol[SF_MAX_FOLLOWERS];
        int nf = 0;
        gboolean same = !load[i] && !regroup && !m->leader;
        for (guint j = 0; j < n; j++)
            if (lead[j] == m) {
                fol[nf++] = ms[j];
                same = same && ms[j]->leader == m;
            }
        same = same && m->nfollowers == nf;
        const char *path = load[i] ? m->want_path : NULL;
        /* should this load fail: does a monitor it is for (this one, its
         * followers) have another wallpaper to return to? (load_ended) */
        gboolean keep = path && *path && m->good_path && g_strcmp0 (m->good_path, path);
        for (int k = 0; path && *path && k < nf; k++)
            keep = keep || (fol[k]->good_path && g_strcmp0 (fol[k]->good_path, path));
        if (nf > 0 && !same) {
            SharedFrame *sf = sf_new (nf);
            memcpy (sf->fol, fol, sizeof (Mon *) * nf);
            mon_assign (m, path, sf, -1, keep);
            for (int k = 0; k < nf; k++)
                mon_assign (fol[k], NULL, sf, k, FALSE);
            double slowest = m->hz;
            for (int k = 0; k < nf; k++)
                slowest = MIN (slowest, fol[k]->hz);
            if (m->hz > slowest + 0.5 && !D.leader_first)
                g_message ("shared rendering: mon%d (%s, %.0f Hz: the fastest of the group) "
                           "renders %s, %d monitor(s) follow", m->index, m->name, m->hz,
                           m->want_path, nf);
            else
                g_message ("shared rendering: mon%d renders %s, %d monitor(s) follow", m->index,
                           m->want_path, nf);
        } else if (nf == 0 && (m->leader || m->nfollowers || load[i])) {
            mon_assign (m, path, NULL, -1, keep);   /* leaves its group, and/or loads */
        }
    }
    /* commit the new roles (main-thread view) */
    for (i = 0; i < n; i++) {
        Mon *m = ms[i];
        m->nfollowers = 0;
    }
    for (i = 0; i < n; i++) {
        Mon *m = ms[i];
        m->leader = lead[i];
        if (lead[i]) {
            lead[i]->nfollowers++;
            g_clear_pointer (&m->loaded_path, g_free);
            /* it joins a monitor that shows the wallpaper already and does
             * not load it again: no load will report for it. It shows the
             * wallpaper from now on, and a re-open it waited for is done. */
            guint li = 0;
            while (li < n && ms[li] != lead[i])
                li++;
            if (li < n && !load[li] && !g_strcmp0 (lead[i]->good_path, m->want_path)) {
                g_free (m->good_path);
                m->good_path = g_strdup (m->want_path);
                if (m->reopen_for) {
                    g_free (m->restored);
                    m->restored = g_strdup (m->want_path);
                }
            }
        } else if (load[i]) {
            g_free (m->loaded_path);
            m->loaded_path = g_strdup (m->want_path);
        }
    }
    g_free (ms);
    g_free (lead);
    g_free (load);
    g_free (force_own);
    follow_pace_update ();   /* the roles changed */
    vis_push ();    /* "Skip hidden areas": a leader draws what its followers show */
    apply_pause_state ();
}

static gboolean shared_fallback_cb (gpointer user)
{
    SharedFallback *fb = user;
    if (fb->leader->nfollowers > 0 && !g_strcmp0 (fb->leader->want_path, fb->path)) {
        g_message ("shared rendering off for %s: %s", fb->path, fb->why);
        g_free (D.shared_note);
        D.shared_note = g_strdup (fb->why);
        if (!D.noshare)
            D.noshare = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_add (D.noshare, g_strdup (fb->path));
        apply_wallpapers (NULL, NULL, FALSE, FALSE);   /* followers load their own */
    }
    g_free (fb->path);
    g_free (fb);
    return G_SOURCE_REMOVE;
}

/* sharing may be possible again (parallax / render_shared changed) */
static void noshare_reset (void)
{
    if (D.noshare)
        g_hash_table_remove_all (D.noshare);
    g_clear_pointer (&D.shared_note, g_free);
}

/* main: the load of `path` on m's render thread ended (msg NULL = well).
 * The result counts for m and for the monitors following it, as far as
 * they are still to show that wallpaper: one that arrives after they were
 * given another is history (that load reports for itself).
 *
 * Loaded: these monitors really show `path` now (good_path) and load_error
 * is over - unless `path` is the re-open after a failure: then load_error
 * goes on naming the wallpaper that failed and status adds "load_restored",
 * until the monitor is given another wallpaper.
 *
 * Failed: every monitor of the group carries the reason and shows nothing:
 * the leader freed its wallpaper to make room for the new one, a follower
 * gave up its own to follow. Each of them that showed another wallpaper
 * before is given that one back - leader and followers alike, each its own
 * (per-monitor wallpapers), loaded like any wallpaper, so with the options
 * saved for it - and the saved choice returns with them (common/restore.h):
 * the config and status name what is shown, the next start loads it. Once:
 * a monitor on a re-opened wallpaper (reopen_for) is never given another
 * one; when the re-open fails too it stays black, as after any failed load,
 * with both reasons in load_error. Nothing comes back where no wallpaper
 * was shown before, after "forget" (a wallpaper being deleted is none to
 * return to) and when the wallpaper that failed is the previous one itself
 * (its reload for an option change). A monitor of the group that has
 * nothing to return to stays on the failed wallpaper, black. */
static void load_ended (Mon *m, const char *path, const char *msg, gboolean kept)
{
    if (g_strcmp0 (m->want_path, path))
        return;
    guint n = g_list_length (D.mons), i = 0;
    Mon **ms = g_new0 (Mon *, n);
    WalpeRestoreMon *rm = g_new0 (WalpeRestoreMon, n);
    for (GList *l = D.mons; l; l = l->next, i++) {
        Mon *x = ms[i] = l->data;
        rm[i].name = x->name;
        rm[i].member = (x == m || x->leader == m) && !g_strcmp0 (x->want_path, path);
    }
    if (!msg) {
        for (i = 0; i < n; i++) {
            Mon *x = ms[i];
            if (!rm[i].member)
                continue;
            g_free (x->good_path);
            x->good_path = *path ? g_strdup (path) : NULL;
            g_free (x->load_error);
            x->load_error = g_strdup (x->reopen_for);   /* NULL = it loaded */
            g_free (x->restored);
            x->restored = x->reopen_for ? g_strdup (path) : NULL;
        }
    } else {
        /* nothing is loaded: opening the same wallpaper again must try again */
        g_clear_pointer (&m->loaded_path, g_free);
        for (i = 0; i < n; i++) {
            Mon *x = ms[i];
            if (!rm[i].member)
                continue;
            g_free (x->load_error);
            g_clear_pointer (&x->restored, g_free);
            if (x->reopen_for) {
                /* the re-open itself (or a later attempt at that wallpaper:
                 * each monitor of a group tries a failed one on its own,
                 * shared_fallback_cb): nothing else is opened for it */
                x->load_error = g_strdup_printf ("%s; the previous wallpaper could not be opened "
                                                 "again (%s)", x->reopen_for, msg);
                g_clear_pointer (&x->good_path, g_free);
                continue;
            }
            x->load_error = g_strdup (msg);
            if (!g_strcmp0 (x->good_path, path))
                g_clear_pointer (&x->good_path, g_free);
            rm[i].good = x->good_path;
        }
        if (walpe_restore_plan (D.cfg, &open_undo, path, rm, (int) n)) {
            GString *who = g_string_new (NULL);
            for (i = 0; i < n; i++)
                if (rm[i].restore) {
                    ms[i]->reopen_for = g_strdup (msg);
                    g_string_append_printf (who, "%s%s: %s", who->len ? ", " : "", ms[i]->name,
                                            ms[i]->good_path);
                }
            g_message ("%s did not load: opening the previous wallpaper again (%s)", path,
                       who->str);
            g_string_free (who, TRUE);
            walpe_config_save (D.cfg);
            restore_apply = TRUE;
            apply_wallpapers (NULL, NULL, FALSE, FALSE);
            restore_apply = FALSE;
        } else if (kept) {
            mon_blank (m);
        }
    }
    g_free (rm);
    g_free (ms);
}

static Mon *mon_by_index (int idx)
{
    for (GList *l = D.mons; l; l = l->next)
        if (((Mon *) l->data)->index == idx)
            return l->data;
    return NULL;
}

/* the monitor whose scene handles input on m (its leader when following) */
static Mon *mon_route (Mon *m)
{
    return m && m->leader ? m->leader : m;
}

/* --------------------------------------------------- display sleep checks */

/* Display sleep is noticed by events where the server has them, not by
 * asking it every second:
 *  - screensaver (blank / lock): MIT-SCREEN-SAVER ScreenSaverNotify;
 *  - DPMS: DPMSInfoNotify (DPMS 1.2: newer servers + libXext >= 1.3.6,
 *    looked up at run time). Older servers have no DPMS event at all:
 *    there the state is asked every 10 s (`xset dpms force off` and the
 *    idle timeout are both noticed within that), and while the display is
 *    off raw input events are selected (XInput2), so the wake-up - which
 *    only input causes - is seen at once instead of at the next poll.
 * WALPE_FAKE_DPMS=<file> (tests; Xvfb has no DPMS): the DPMS state is the
 * file's content ("off..." = asleep), handled like a server without DPMS
 * events. */
static int ss_event_base = -1;      /* ScreenSaverNotify selected: first event code */
static int dpms_opcode = -1;        /* DPMSInfoNotify selected: the extension's opcode */
static gboolean dpms_poll;          /* DPMS without events: slow poll + wake watch */
static guint sleep_poll_id, sleep_poll_secs;
static guint sleep_kick_id;
static int sleep_kick_n;
static const char *fake_dpms;

/* DPMS (libXext, already linked): power-save states standby/suspend/off */
static gboolean dpms_asleep (void)
{
    static int probed = FALSE, ok = FALSE, evb = 0, errb = 0;
    if (fake_dpms) {
        char *s = NULL;
        gboolean off = g_file_get_contents (fake_dpms, &s, NULL, NULL) && s &&
                       g_str_has_prefix (s, "off");
        g_free (s);
        return off;
    }
    if (!probed) {
        probed = TRUE;
        ok = DPMSQueryExtension (D.xd, &evb, &errb);
        if (ok)
            g_message ("DPMS extension present");
    }
    if (!ok)
        return FALSE;
    CARD16 level = DPMSModeOn;
    BOOL enabled = False;
    DPMSInfo (D.xd, &level, &enabled);
    return enabled && level != DPMSModeOn;
}

/* screensaver active = screen locked/blanked. libXss is optional here:
 * dlopened at runtime (no build dep); silently unavailable if missing. */
typedef struct {
    Window window;
    int state, kind;
    unsigned long til_or_since, idle, eventMask;
} XScreenSaverInfo; /* layout-identical to <X11/extensions/scrnsaver.h> */
#define SCREENSAVER_ON 1 /* ScreenSaverOn in scrnsaver.h */
#define SCREENSAVER_NOTIFY_MASK 0x00000001L /* ScreenSaverNotifyMask */

static struct {
    int probed, ok, evb, errb;
    Bool (*qext) (Display *, int *, int *);
    XScreenSaverInfo *(*alloc_info) (void);
    Bool (*query_info) (Display *, Drawable, XScreenSaverInfo *);
    void (*select_input) (Display *, Drawable, unsigned long);
    XScreenSaverInfo *info;
} Xss;

static gboolean screensaver_probe (void)
{
    if (!Xss.probed) {
        Xss.probed = TRUE;
        void *lib = dlopen ("libXss.so.1", RTLD_LAZY);
        if (lib) {
            Xss.qext = (Bool (*)(Display *, int *, int *))
                dlsym (lib, "XScreenSaverQueryExtension");
            Xss.alloc_info = (XScreenSaverInfo * (*) (void))
                dlsym (lib, "XScreenSaverAllocInfo");
            Xss.query_info = (Bool (*) (Display *, Drawable, XScreenSaverInfo *))
                dlsym (lib, "XScreenSaverQueryInfo");
            Xss.select_input = (void (*) (Display *, Drawable, unsigned long))
                dlsym (lib, "XScreenSaverSelectInput");
            Xss.ok = Xss.qext && Xss.alloc_info && Xss.query_info &&
                     Xss.qext (D.xd, &Xss.evb, &Xss.errb);
        }
        if (Xss.ok)
            g_message ("XScreenSaver extension present (libXss dlopened)");
    }
    return Xss.ok;
}

static gboolean screensaver_active (void)
{
    if (!screensaver_probe ())
        return FALSE;
    if (!Xss.info)
        Xss.info = Xss.alloc_info ();
    if (!Xss.info || !Xss.query_info (D.xd, D.root, Xss.info))
        return FALSE;
    return Xss.info->state == SCREENSAVER_ON;
}

/* once at start-up: ask for the events */
static void sleep_watch_init (void)
{
    fake_dpms = g_getenv ("WALPE_FAKE_DPMS");
    if (screensaver_probe () && Xss.select_input) {
        Xss.select_input (D.xd, D.root, SCREENSAVER_NOTIFY_MASK);
        ss_event_base = Xss.evb;
    }
    int evb = 0, errb = 0;
    if (fake_dpms) {
        dpms_poll = TRUE;
    } else if (DPMSQueryExtension (D.xd, &evb, &errb)) {
        int maj = 0, min = 0, op = 0, e1 = 0, e2 = 0;
        void (*sel) (Display *, unsigned long) = NULL;
        if (DPMSGetVersion (D.xd, &maj, &min) && (maj > 1 || (maj == 1 && min >= 2)))
            sel = (void (*) (Display *, unsigned long)) dlsym (RTLD_DEFAULT, "DPMSSelectInput");
        if (sel && XQueryExtension (D.xd, "DPMS", &op, &e1, &e2)) {
            sel (D.xd, 1UL /* DPMSInfoNotifyMask */);
            dpms_opcode = op;
        } else {
            dpms_poll = TRUE;
        }
    }
    g_message ("display sleep: screensaver %s, DPMS %s",
               ss_event_base >= 0 ? "events" : Xss.ok ? "present, no events (not followed)" : "absent",
               dpms_opcode >= 0 ? "events" : dpms_poll ? "polled every 10 s (no DPMS events here)"
                                                       : "absent");
}

/* one central place deciding whether a monitor's wallpaper should be
 * paused (decode + render). Per-monitor: each monitor owns its own
 * content/decoder instance, so covered monitors stop while others play. */
static gboolean want_pause_mon (Mon *m)
{
    return D.cfg->paused ||
           (D.cfg->pause_fullscreen && D.fs_active) ||
           (D.cfg->pause_dpms && D.asleep) ||
           (D.cfg->pause_covered && m->covered) ||
           /* "Skip hidden areas" (opt-in): not one pixel of it can be seen
            * (also under windows the cover scan does not count: a screen
            * locker, an override-redirect full-screen window) */
           (D.cfg->pause_covered && m->vis_none) ||
           /* opt-in rules (off by default) */
           (D.cfg->focus_action == WALPE_FOCUS_PAUSE && m->focus_app) ||
           (D.cfg->pause_battery && D.on_battery);
}

static void sleep_arm (void);

/* GPUPROF: why a monitor is paused, for the profile report (the
 * first rule that applies, in want_pause_mon's order) */
static const char *pause_reason_mon (Mon *m)
{
    if (D.cfg->paused)
        return "walpe-ctl pause";
    if (D.cfg->pause_fullscreen && D.fs_active)
        return "a fullscreen window has the focus";
    if (D.cfg->pause_dpms && D.asleep)
        return "display asleep";
    if (D.cfg->pause_covered && m->covered)
        return "covered by windows";
    if (D.cfg->focus_action == WALPE_FOCUS_PAUSE && m->focus_app)
        return "focus rule";
    if (D.cfg->pause_battery && D.on_battery)
        return "on battery";
    return "";
}

/* GPUPROF, main thread: the profiler follows the monitors' pauses
 * (a run during which a monitor was paused says so) */
static void prof_note_pauses (void)
{
    if (!WALPE_PROF_ON ())
        return;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        walpe_prof_note_pause (m->index, m->own_paused, pause_reason_mon (m));
    }
}

static void apply_pause_state (void)
{
    for (GList *l = D.mons; l; l = l->next)
        ((Mon *) l->data)->own_paused = want_pause_mon (l->data);
    prof_note_pauses ();
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        gboolean self = m->own_paused, p = self;
        /* a leader's scene runs while any monitor of its group shows it */
        for (GList *l2 = D.mons; m->nfollowers && l2; l2 = l2->next)
            if (((Mon *) l2->data)->leader == m)
                p = p && ((Mon *) l2->data)->own_paused;
        g_mutex_lock (&m->lock);
        if (p != m->pause_wanted || self != m->self_paused) {
            if ((!p && p != m->pause_wanted) || (!self && self != m->self_paused))
                m->render_req = TRUE; /* resume: present the current state */
            m->pause_wanted = p;
            m->self_paused = self;
            g_cond_signal (&m->cond); /* render thread applies it */
        }
        g_mutex_unlock (&m->lock);
    }
    fps_refresh ();
    pointer_update ();
    state_touch ();
}

static void sleep_recheck (void)
{
    gboolean asleep = dpms_asleep () || screensaver_active ();
    if (asleep != D.asleep) {
        D.asleep = asleep;
        if (asleep) {
            g_message ("display asleep (dpms/screensaver): pausing render + decode");
            malloc_trim (0);
        } else {
            g_message ("display awake: resuming");
        }
        apply_pause_state ();
    }
    sleep_arm ();
}

static int walpe_xio_error (Display *d);

/* The GUI starts the daemon with stdout / stderr appended to
 * ~/.cache/walpe/daemon.log and rotates that file only when it starts a
 * daemon: one that runs for weeks (or meets something that logs every
 * frame) grew it without limit. Every 256 log lines (log_guard; no
 * timer): a log that is a regular
 * file opened for appending and has passed 16 MiB keeps its last 1 MiB
 * (WALPE_LOG_MAX_MB=<n> changes the limit, 0 switches this off). A
 * terminal, a pipe, the journal or a file opened without O_APPEND is left
 * alone. */
static gboolean log_cap_now (void)   /* TRUE = the file was cut back */
{
    static gint64 limit = -1;
    if (limit < 0) {
        const char *e = g_getenv ("WALPE_LOG_MAX_MB");
        limit = (e && *e ? CLAMP (g_ascii_strtoll (e, NULL, 10), 0, 4096) : 16) << 20;
    }
    if (!limit)
        return FALSE;
    enum { KEEP = 1 << 20 };
    struct stat st;
    int fl = fcntl (STDERR_FILENO, F_GETFL);
    if (fl < 0 || !(fl & O_APPEND) || fstat (STDERR_FILENO, &st) != 0 || !S_ISREG (st.st_mode) ||
        st.st_size <= limit)
        return FALSE;
    /* the descriptor is write-only: read the tail through a second one */
    int rfd = open ("/proc/self/fd/2", O_RDONLY | O_CLOEXEC);
    char *tail = rfd >= 0 ? g_try_malloc (KEEP) : NULL;
    ssize_t n = tail ? pread (rfd, tail, KEEP, st.st_size - KEEP) : -1;
    if (rfd >= 0)
        close (rfd);
    fflush (NULL);
    if (ftruncate (STDERR_FILENO, 0) == 0 && n > 0) {
        /* O_APPEND: every writer continues at the new end. Start the kept
         * part at a line. */
        const char *nl = memchr (tail, '\n', (size_t) n);
        const char *from = nl ? nl + 1 : tail;
        static const char note[] = "walpe-daemon: log passed its size limit, older lines dropped\n";
        ssize_t r = write (STDERR_FILENO, note, sizeof note - 1);
        r = write (STDERR_FILENO, from, (size_t) (tail + n - from));
        (void) r;
    }
    g_free (tail);
    return TRUE;
}

static gboolean sleep_tick (gpointer user)
{
    (void) user;
    sleep_recheck ();
    return G_SOURCE_CONTINUE;
}

/* the poll (servers without DPMS events only) and the wake watch follow
 * the state */
static void sleep_arm (void)
{
    gboolean poll = dpms_poll && D.cfg->pause_dpms;
    /* asleep without XInput2: nothing announces the wake-up */
    guint secs = !poll ? 0 : (D.asleep && xi2_opcode < 0) ? 2 : 10;
    if (secs != sleep_poll_secs) {
        if (sleep_poll_id)
            g_source_remove (sleep_poll_id);
        sleep_poll_secs = secs;
        sleep_poll_id = secs ? g_timeout_add_seconds (secs, sleep_tick, NULL) : 0;
    }
    xi_want_wake = poll && D.asleep && !sleep_kick_id;
    xi2_apply_mask ();
}

static gboolean sleep_kick_cb (gpointer user)
{
    (void) user;
    sleep_kick_id = 0;
    /* the server may report the display on a moment after the input that
     * woke it: look again, then wait for the next input */
    if (D.asleep && ++sleep_kick_n < 3)
        sleep_kick_id = g_timeout_add (1000, sleep_kick_cb, NULL);
    sleep_recheck ();
    return G_SOURCE_REMOVE;
}

/* raw input arrived while the display was off */
static void sleep_wake_kick (void)
{
    if (sleep_kick_id)
        return;
    sleep_kick_n = 0;
    sleep_kick_id = g_timeout_add (150, sleep_kick_cb, NULL);
    sleep_recheck ();   /* also drops the raw input selection (sleep_arm) */
}

/* ------------------------------------------------------ power source
 * Followed only while a battery rule is set (pause_battery / battery_fps):
 * /sys/class/power_supply, re-read when the kernel announces a change of
 * a power supply (uevent netlink socket: no polling; where that socket
 * cannot be opened, every 30 s). "On battery" = a system battery is
 * discharging and no mains / USB supply is online.
 * WALPE_FAKE_POWER=<file> (tests): the file says "battery" or "ac", read
 * every second. */
static int power_fd = -1;
static guint power_src_id, power_poll_id, power_debounce_id;
static const char *fake_power;

static char *power_attr (const char *dev, const char *attr)
{
    char *p = g_build_filename ("/sys/class/power_supply", dev, attr, NULL), *s = NULL;
    if (!g_file_get_contents (p, &s, NULL, NULL))
        s = NULL;
    g_free (p);
    return s ? g_strstrip (s) : NULL;
}

static gboolean power_on_battery (void)
{
    if (fake_power) {
        char *s = NULL;
        gboolean b = g_file_get_contents (fake_power, &s, NULL, NULL) && s &&
                     g_str_has_prefix (s, "bat");
        g_free (s);
        return b;
    }
    GDir *d = g_dir_open ("/sys/class/power_supply", 0, NULL);
    if (!d)
        return FALSE;
    gboolean supply_online = FALSE, discharging = FALSE;
    const char *name;
    while ((name = g_dir_read_name (d))) {
        char *type = power_attr (name, "type");
        if (!g_strcmp0 (type, "Battery")) {
            /* not the battery of a mouse / headset (scope "Device") */
            char *scope = power_attr (name, "scope"), *st = power_attr (name, "status");
            if (g_strcmp0 (scope, "Device") && !g_strcmp0 (st, "Discharging"))
                discharging = TRUE;
            g_free (scope);
            g_free (st);
        } else if (type) {
            char *on = power_attr (name, "online");
            if (on && atoi (on) > 0)
                supply_online = TRUE;
            g_free (on);
        }
        g_free (type);
    }
    g_dir_close (d);
    return discharging && !supply_online;
}

static gboolean power_recheck (gpointer user)
{
    if (user)                   /* the debounce after an uevent */
        power_debounce_id = 0;
    gboolean b = power_on_battery ();
    if (b != D.on_battery) {
        D.on_battery = b;
        g_message ("power: %s", b ? "on battery" : "on mains");
        apply_pause_state ();
    }
    return user ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static gboolean power_ready (gint fd, GIOCondition cond, gpointer user)
{
    (void) cond; (void) user;
    char buf[8192];
    gboolean hit = FALSE;
    ssize_t n;
    while ((n = recv (fd, buf, sizeof (buf) - 1, MSG_DONTWAIT)) > 0) {
        buf[n] = 0;
        for (char *p = buf; p < buf + n; p += strlen (p) + 1)
            hit |= !strcmp (p, "SUBSYSTEM=power_supply");
    }
    if (hit && !power_debounce_id)
        power_debounce_id = g_timeout_add (300, power_recheck, GINT_TO_POINTER (1));
    return G_SOURCE_CONTINUE;
}

/* start / stop following the power source as the settings ask */
static void power_watch_update (void)
{
    gboolean want = D.cfg->pause_battery || D.cfg->battery_fps > 0;
    gboolean have = power_src_id || power_poll_id;
    if (want && !have) {
        fake_power = g_getenv ("WALPE_FAKE_POWER");
        if (!fake_power) {
            power_fd = socket (AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                               NETLINK_KOBJECT_UEVENT);
            struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = 1 };
            if (power_fd >= 0 && bind (power_fd, (struct sockaddr *) &a, sizeof (a)) != 0) {
                close (power_fd);
                power_fd = -1;
            }
        }
        if (power_fd >= 0)
            power_src_id = g_unix_fd_add (power_fd, G_IO_IN, power_ready, NULL);
        else
            power_poll_id = g_timeout_add_seconds (fake_power ? 1 : 30, power_recheck, NULL);
        g_message ("power: following the power source (%s)",
                   power_fd >= 0 ? "kernel events" : fake_power ? "test file" : "every 30 s");
        power_recheck (NULL);
    } else if (!want && have) {
        if (power_src_id)
            g_source_remove (power_src_id);
        if (power_poll_id)
            g_source_remove (power_poll_id);
        if (power_debounce_id)
            g_source_remove (power_debounce_id);
        power_src_id = power_poll_id = power_debounce_id = 0;
        if (power_fd >= 0)
            close (power_fd);
        power_fd = -1;
        if (D.on_battery) {
            D.on_battery = FALSE;
            apply_pause_state ();
        }
    } else if (want) {
        apply_pause_state ();   /* a rule changed while already following */
    }
}

/* -------------------------------------------------- covered detection
 * dwm has no MAXIMIZED EWMH atoms and tiled layouts cover a monitor even
 * when the active window is only half of it, so "covered" is decided by
 * GEOMETRY: union of all visible client windows rasterized on a coarse
 * grid per monitor (exact enough, no rectangle-union algebra). Event
 * driven only — recomputed after map/unmap/configure/property changes,
 * debounced. */

#define COVER_GRID 24           /* raster cells per axis per monitor */
#define COVER_THRESHOLD 0.85f   /* covered area fraction that pauses */

static gboolean window_is_wallpaper (Window w)
{
    for (GList *l = D.mons; l; l = l->next)
        if (((Mon *) l->data)->xid == w)
            return TRUE;
    return FALSE;
}

/* windows that never count as covering: the wallpaper is visible through
 * them (WM_CLASS matched against cfg->transparent_windows, e.g. "st" —
 * suckless terminal's alpha patch shows the wallpaper behind it) */
static gboolean window_is_translucent (Window w)
{
    if (!D.cfg->transparent_windows || !*D.cfg->transparent_windows)
        return FALSE;
    XClassHint ch = { NULL, NULL };
    if (!XGetClassHint (D.xd, w, &ch))
        return FALSE;
    gboolean hit = FALSE;
    char **tokens = g_strsplit (D.cfg->transparent_windows, ",", -1);
    for (guint i = 0; tokens && tokens[i]; i++) {
        const char *t = g_strstrip (tokens[i]);
        if (*t && ((ch.res_name && !g_ascii_strcasecmp (t, ch.res_name)) ||
                   (ch.res_class && !g_ascii_strcasecmp (t, ch.res_class)))) {
            hit = TRUE;
            break;
        }
    }
    g_strfreev (tokens);
    if (ch.res_name)
        XFree (ch.res_name);
    if (ch.res_class)
        XFree (ch.res_class);
    return hit;
}

/* property helper: up to max 32-bit items of type `type` (XFree *out).
 * Every property the daemon reads as an array of longs goes through
 * here: any X client can set e.g. _NET_CLIENT_LIST with format 8, Xlib
 * then hands back nitems BYTES, and indexing that as Window[] read far
 * past the buffer (ASan: heap-buffer-overflow in the cover scan). */
static gulong window_prop32 (Window w, Atom prop, Atom type, gulong max, guchar **out)
{
    Atom rt = None;
    int fmt = 0;
    gulong n = 0, left = 0;
    *out = NULL;
    if (prop == None ||
        XGetWindowProperty (D.xd, w, prop, 0, (long) max, False, type, &rt, &fmt, &n, &left,
                            out) != Success || !*out)
        return 0;
    if (fmt != 32 || rt != type) {
        XFree (*out);
        *out = NULL;
        return 0;
    }
    return MIN (n, max);
}

/* a window that can hide the wallpaper: a normal (or dialog / untyped)
 * client that is not minimized. Docks, desktop icon windows, menus,
 * tooltips, notifications and splash screens never count; neither do
 * iconic (WM_STATE) or _NET_WM_STATE_HIDDEN windows. dwm keeps the windows
 * of other tags mapped but moves them off-screen, so the geometry test
 * already ignores them. */
static gboolean window_is_normal_visible (Window w)
{
    static Atom a_type, a_normal, a_dialog, a_state, a_hidden, a_wmstate;
    if (!a_type) {
        a_type = XInternAtom (D.xd, "_NET_WM_WINDOW_TYPE", False);
        a_normal = XInternAtom (D.xd, "_NET_WM_WINDOW_TYPE_NORMAL", False);
        a_dialog = XInternAtom (D.xd, "_NET_WM_WINDOW_TYPE_DIALOG", False);
        a_state = XInternAtom (D.xd, "_NET_WM_STATE", False);
        a_hidden = XInternAtom (D.xd, "_NET_WM_STATE_HIDDEN", False);
        a_wmstate = XInternAtom (D.xd, "WM_STATE", False);
    }
    guchar *d = NULL;
    gulong n = window_prop32 (w, a_type, XA_ATOM, 16, &d);
    if (d) {
        gboolean normal = FALSE;
        for (gulong i = 0; i < n; i++)
            normal |= ((Atom *) d)[i] == a_normal || ((Atom *) d)[i] == a_dialog;
        XFree (d);
        if (n > 0 && !normal)
            return FALSE;
    }
    n = window_prop32 (w, a_state, XA_ATOM, 32, &d);
    if (d) {
        gboolean hidden = FALSE;
        for (gulong i = 0; i < n; i++)
            hidden |= ((Atom *) d)[i] == a_hidden;
        XFree (d);
        if (hidden)
            return FALSE;
    }
    n = window_prop32 (w, a_wmstate, a_wmstate, 2, &d);
    if (d) {
        gboolean iconic = n >= 1 && ((long *) d)[0] == IconicState;
        XFree (d);
        if (iconic)
            return FALSE;
    }
    return TRUE;
}

/* is a compositing manager running? (_NET_WM_CM_Sn selection owner) */
static gboolean compositor_running (void)
{
    static Atom a_cm;
    if (!a_cm) {
        char name[32];
        g_snprintf (name, sizeof name, "_NET_WM_CM_S%d", DefaultScreen (D.xd));
        a_cm = XInternAtom (D.xd, name, False);
    }
    return XGetSelectionOwner (D.xd, a_cm) != None;
}

/* see-through: listed in transparent_windows, a compositor opacity below
 * 100 % (_NET_WM_WINDOW_OPACITY, set by transset / some picom set-ups), or
 * - under a compositing manager - a 32-bit (ARGB) window that does not
 * declare itself opaque: toolkits set _NET_WM_OPAQUE_REGION on windows
 * they paint solid, and leave it off (or small) on translucent ones (a
 * terminal with background alpha, a GTK window with a translucent theme).
 * picom's own opacity rules leave no trace on the window: such windows
 * need their WM_CLASS in transparent_windows.
 * depth / w / h: the window's (already asked for by the scan); cm: a
 * compositing manager runs. */
static gboolean window_is_see_through (Window w, int depth, int ww, int wh, gboolean cm)
{
    if (window_is_translucent (w))
        return TRUE;
    static Atom a_op, a_opaque;
    if (!a_op) {
        a_op = XInternAtom (D.xd, "_NET_WM_WINDOW_OPACITY", False);
        a_opaque = XInternAtom (D.xd, "_NET_WM_OPAQUE_REGION", False);
    }
    guchar *d = NULL;
    gulong n = window_prop32 (w, a_op, XA_CARDINAL, 1, &d);
    gboolean see = d && n == 1 && (((unsigned long *) d)[0] & 0xffffffffUL) < 0xffffffffUL;
    if (d)
        XFree (d);
    if (see || !cm || depth != 32 || ww < 1 || wh < 1)
        return see;
    /* opaque part of the window (rectangles x, y, w, h; overlap between
     * them is rare and only errs towards "opaque") */
    n = window_prop32 (w, a_opaque, XA_CARDINAL, 4 * 64, &d);
    double opaque = 0.0;
    for (gulong i = 0; d && i + 3 < n; i += 4) {
        long x = (long) ((unsigned long *) d)[i], y = (long) ((unsigned long *) d)[i + 1];
        long rw = (long) ((unsigned long *) d)[i + 2], rh = (long) ((unsigned long *) d)[i + 3];
        long x1 = CLAMP (x, 0, ww), y1 = CLAMP (y, 0, wh);
        long x2 = CLAMP (x + rw, 0, ww), y2 = CLAMP (y + rh, 0, wh);
        if (x2 > x1 && y2 > y1)
            opaque += (double) (x2 - x1) * (double) (y2 - y1);
    }
    if (d)
        XFree (d);
    return opaque < 0.85 * (double) ww * (double) wh;
}

/* fraction of monitor g hidden by the given window rectangles (root px) */
static float monitor_coverage (const WalpeRect *g, const WalpeRect *wins, guint n)
{
    guchar grid[COVER_GRID * COVER_GRID] = { 0 };
    float cw = (float) g->width / COVER_GRID;
    float ch = (float) g->height / COVER_GRID;
    guint marked = 0;

    for (guint i = 0; i < n && marked < COVER_GRID * COVER_GRID; i++) {
        const WalpeRect *r = &wins[i];
        if (r->x >= g->x + g->width || r->y >= g->y + g->height || r->x + r->width <= g->x ||
            r->y + r->height <= g->y)
            continue;   /* no overlap with this monitor */
        for (int cy = 0; cy < COVER_GRID; cy++)
            for (int cx = 0; cx < COVER_GRID; cx++) {
                if (grid[cy * COVER_GRID + cx])
                    continue;
                float px = g->x + (cx + 0.5f) * cw;
                float py = g->y + (cy + 0.5f) * ch;
                if (px >= r->x && px < r->x + r->width && py >= r->y && py < r->y + r->height) {
                    grid[cy * COVER_GRID + cx] = 1;
                    marked++;
                }
            }
    }
    return (float) marked / (COVER_GRID * COVER_GRID);
}

/* Property changes of the clients (state: hidden / iconic / fullscreen,
 * opacity, type) decide whether they cover: the daemon asks for them on
 * every client of _NET_CLIENT_LIST once, when it first sees it, instead
 * of re-scanning all windows every 2 s "in case" one changed. A window
 * that is gone needs no clean-up (its event selection went with it). */
static void clients_watch (const Window *w, guint n)
{
    GHashTable *now = g_hash_table_new (NULL, NULL);
    for (guint i = 0; i < n; i++) {
        if (w[i] == None || w[i] == D.root || window_is_wallpaper (w[i]))
            continue;
        g_hash_table_add (now, GSIZE_TO_POINTER (w[i]));
        if (!D.watched_clients || !g_hash_table_contains (D.watched_clients, GSIZE_TO_POINTER (w[i])))
            XSelectInput (D.xd, w[i], PropertyChangeMask);
    }
    if (D.watched_clients)
        g_hash_table_unref (D.watched_clients);
    D.watched_clients = now;
}

static gboolean client_is_watched (Window w)
{
    return D.watched_clients && g_hash_table_contains (D.watched_clients, GSIZE_TO_POINTER (w));
}

static void recompute_covered (void)
{
    /* client list: dwm maintains _NET_CLIENT_LIST; XQueryTree fallback */
    Window *clients = NULL;
    guint n = 0;
    Atom prop = XInternAtom (D.xd, "_NET_CLIENT_LIST", True);
    guchar *data = NULL;
    if (prop != None) {
        gulong cnt = window_prop32 (D.root, prop, XA_WINDOW, 1024, &data);
        if (data && cnt > 0) {
            clients = (Window *) data;
            n = (guint) cnt;
        }
    }
    clients_watch (clients, n);
    Window *tree_clients = NULL;
    if (!clients) {
        Window r2, parent;
        guint tn = 0;
        if (XQueryTree (D.xd, D.root, &r2, &parent, &tree_clients, &tn) && tree_clients) {
            clients = tree_clients;
            n = tn;
        }
    }

    /* each client is asked about once per scan (attributes, root position,
     * type / state / opacity), not once per monitor: the windows that can
     * hide a wallpaper, as rectangles in root coordinates */
    WalpeRect *wins = g_new (WalpeRect, n ? n : 1);
    guint nw = 0;
    WalpeRect focus = { 0, 0, 0, 0 };   /* the focused application window */
    gboolean have_focus = FALSE;
    gboolean cm = !D.cfg->cover_transparent && compositor_running ();
    for (guint i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes (D.xd, clients[i], &wa))
            continue;
        if (wa.map_state != IsViewable || wa.class == InputOnly ||
            wa.override_redirect || window_is_wallpaper (clients[i]))
            continue;
        Window junk0;
        int ox, oy;     /* root coords (attrs x/y are parent-relative) */
        if (!XTranslateCoordinates (D.xd, clients[i], D.root, 0, 0, &ox, &oy, &junk0))
            continue;
        /* cheap rejection first: on no monitor at all (dwm parks the
         * windows of other tags off-screen) */
        gboolean on_any = FALSE;
        for (GList *l = D.mons; l && !on_any; l = l->next) {
            const WalpeRect *g = &((Mon *) l->data)->geom;
            on_any = !(ox >= g->x + g->width || oy >= g->y + g->height || ox + wa.width <= g->x ||
                       oy + wa.height <= g->y);
        }
        if (!on_any)
            continue;
        if (!window_is_normal_visible (clients[i]))
            continue;
        if (clients[i] == D.watched_win) {
            focus = (WalpeRect) { ox, oy, wa.width, wa.height };
            have_focus = TRUE;
        }
        if (!D.cfg->cover_transparent &&
            window_is_see_through (clients[i], wa.depth, wa.width, wa.height, cm))
            continue;
        wins[nw++] = (WalpeRect) { ox, oy, wa.width, wa.height };
    }

    gboolean changed = FALSE;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        float cov = nw > 0 ? monitor_coverage (&m->geom, wins, nw) : 0.0f;
        gboolean now = cov >= COVER_THRESHOLD;
        if (now != m->covered) {
            m->covered = now;
            changed = TRUE;
            g_message ("mon%d %s by windows (coverage %.2f) -> %s",
                       m->index, now ? "covered" : "exposed", cov,
                       now ? "pausing" : "resuming");
        }
        /* an application's window on this monitor has the focus (the
         * focus_action rule; the desktop itself focused = none) */
        const WalpeRect *g = &m->geom;
        gboolean fa = have_focus &&
                      !(focus.x >= g->x + g->width || focus.y >= g->y + g->height ||
                        focus.x + focus.width <= g->x || focus.y + focus.height <= g->y);
        if (fa != m->focus_app) {
            m->focus_app = fa;
            changed = TRUE;
            if (D.cfg->focus_action != WALPE_FOCUS_RUN)
                g_debug ("mon%d: %s", m->index, fa ? "an application has the focus"
                                                   : "no application focused");
        }
    }
    g_free (wins);
    if (data)
        XFree (data);
    if (tree_clients)
        XFree (tree_clients);

    if (changed)
        apply_pause_state ();
}

static void restack_check (void);
static void pointer_rest_sample (void);

static gboolean cover_recompute_tick (gpointer user)
{
    (void) user;
    D.cover_timer_id = 0;
    restack_check ();
    recompute_covered ();
    pointer_rest_sample ();   /* another window may be under a resting pointer now */
    return G_SOURCE_REMOVE;
}

/* coalesce bursts (window drags fire ConfigureNotify continuously). There
 * is no periodic re-scan any more: geometry / visibility changes arrive as
 * SubstructureNotify events of the root, state / opacity / type changes as
 * PropertyNotify of the watched clients (clients_watch). */
static void cover_schedule (void)
{
    if (!D.cover_timer_id)
        D.cover_timer_id = g_timeout_add (80, cover_recompute_tick, NULL);
}

/* diagnostics: visible with G_MESSAGES_DEBUG=all */
static gboolean stats_tick (gpointer user)
{
    (void) user;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        g_debug ("stats: mon%d renders=%u content=%s last=%dx%d win=0x%lx",
                 m->index, g_atomic_int_get (&m->render_count),
                 walpe_type_name (g_atomic_int_get (&m->content_type)),
                 g_atomic_int_get (&m->last_w), g_atomic_int_get (&m->last_h),
                 (unsigned long) m->xid);
        g_debug ("stats:   geom=%dx%d at %d,%d fs=%d covered=%d paused_cfg=%d",
                 m->geom.width, m->geom.height, m->geom.x, m->geom.y,
                 D.fs_active, m->covered, D.cfg->paused);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------- "Skip hidden areas"
 * Opt-in (config skip_hidden, default "off": no tracker exists, no monitor
 * is ever given a clip and every line below is dead).
 * daemon/vistrack.h says which pixels of each monitor other windows leave
 * visible; engine/clip.h makes the renderers leave the rest alone. Here:
 * the setting, and what each render thread is told to draw. */

static guint64 vis_serial_src;      /* main: clip serials, never reused */

/* main thread: give every monitor the boxes it renders into (-1 = all).
 * Called when a visible region, the setting or the monitor groups change. */
static void vis_push (void)
{
    int margin = D.vis && walpe_vistrack_compositor (D.vis) ? D.cfg->skip_hidden_margin : 0;
    gboolean pause_changed = FALSE;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        const WalpeBox full = { 0, 0, m->geom.width, m->geom.height };
        WalpeBox boxes[WALPE_CLIP_MAX];
        int n = -1;
        gboolean none = FALSE, all = !D.vis || !m->vis_known;
        WalpeRegion r;
        walpe_region_init (&r);
        if (!all) {
            none = m->vis.n == 0;
            walpe_region_copy (&r, &m->vis);
            /* a leader renders what its followers show as well (same size:
             * the same coordinates) */
            for (GList *l2 = D.mons; m->nfollowers && l2 && !all; l2 = l2->next) {
                Mon *f = l2->data;
                if (f->leader != m)
                    continue;
                if (f->vis_known)
                    walpe_region_add_boxes (&r, f->vis.b, f->vis.n);
                else
                    all = TRUE;
            }
            walpe_region_grow (&r, margin, full);
            all = all || (r.n == 1 && !memcmp (&r.b[0], &full, sizeof (full)));
        }
        if (!all)
            n = walpe_region_simplify (&r, boxes, WALPE_CLIP_MAX,
                                       (gint64) m->geom.width * m->geom.height / 256);
        walpe_region_clear (&r);
        if (none != m->vis_none) {
            m->vis_none = none;
            pause_changed = TRUE;
        }
        g_mutex_lock (&m->lock);
        if (n != m->clip_n || (n > 0 && memcmp (boxes, m->clip, sizeof (WalpeBox) * (gsize) n))) {
            /* did anything become visible that the old boxes did not hold? */
            gboolean grew = n < 0 && m->clip_n >= 0;
            if (n >= 0 && m->clip_n >= 0) {
                WalpeRegion was, now;
                walpe_region_init (&was);
                walpe_region_init (&now);
                walpe_region_set_boxes (&was, m->clip, m->clip_n);
                walpe_region_set_boxes (&now, boxes, n);
                grew = !walpe_region_contains (&was, &now);
                walpe_region_clear (&was);
                walpe_region_clear (&now);
            }
            m->clip_n = n;
            if (n > 0)
                memcpy (m->clip, boxes, sizeof (WalpeBox) * (gsize) n);
            m->clip_serial = ++vis_serial_src;
            if (grew) {
                m->clip_grew = TRUE;
                if (!m->grew_us) {
                    m->grew_us = g_get_monotonic_time ();
                    m->grew_frames = m->frames_done;
                }
                g_cond_signal (&m->cond);   /* draw it now, not at the next frame */
            }
        }
        g_mutex_unlock (&m->lock);
    }
    /* a follower shows frames of its leader: none older than the leader's
     * current boxes once its own have grown */
    for (GList *l = D.mons; l; l = l->next) {
        Mon *f = l->data;
        if (!f->leader)
            continue;
        g_mutex_lock (&f->leader->lock);
        guint64 lead = f->leader->clip_serial;
        g_mutex_unlock (&f->leader->lock);
        g_mutex_lock (&f->lock);
        if (f->clip_grew)
            f->follow_need = lead;
        g_mutex_unlock (&f->lock);
    }
    if (pause_changed)
        apply_pause_state ();
}

/* vistrack: monitor idx (D.vis_mons) shows another part of itself */
static void vis_changed (int idx, const WalpeRegion *vis, gpointer user)
{
    (void) user;
    Mon *m = D.vis_mons[idx];
    walpe_region_copy (&m->vis, vis);
    m->vis_known = TRUE;
    if (D.vis)      /* (not yet while walpe_vistrack_new reports the first regions) */
        vis_push ();
}

/* main thread: the tracker follows the setting. WALPE_SKIP_HIDDEN=off|
 * auto|always overrides the config for this run (tests, A/B). */
static void vis_apply_setting (void)
{
    int mode = walpe_skip_hidden_parse (g_getenv ("WALPE_SKIP_HIDDEN"), D.cfg->skip_hidden);
    if (D.windowed || !D.mons)
        mode = WALPE_SKIP_OFF;
    if (mode == WALPE_SKIP_OFF) {
        if (D.vis) {
            walpe_vistrack_free (D.vis);
            D.vis = NULL;
            g_message ("skip hidden areas: off");
        }
        for (GList *l = D.mons; l; l = l->next)
            ((Mon *) l->data)->vis_known = FALSE;
        vis_push ();
        return;
    }
    /* settle time of a moved window under a compositor (ms) */
    const char *st = g_getenv ("WALPE_SKIP_SETTLE_MS");
    int settle = st && *st ? atoi (st) : 500;
    if (!D.vis) {
        int n = (int) g_list_length (D.mons), i = 0;
        WalpeVisMon *vm = g_new0 (WalpeVisMon, n);
        g_free (D.vis_mons);
        D.vis_mons = g_new0 (Mon *, n);
        for (GList *l = D.mons; l; l = l->next, i++) {
            Mon *m = l->data;
            D.vis_mons[i] = m;
            vm[i] = (WalpeVisMon) { m->geom.x, m->geom.y, m->geom.width, m->geom.height, m->xid };
        }
        XSync (D.xd, False);    /* the tracker's connection must see our windows */
        D.vis = walpe_vistrack_new (vm, n, mode, vis_changed, NULL);
        g_free (vm);
        if (!D.vis) {
            g_warning ("skip hidden areas: no second X connection; everything is rendered");
            return;
        }
        g_message ("skip hidden areas: %s (compositing manager %s)",
                   walpe_skip_hidden_name (mode),
                   walpe_vistrack_compositor (D.vis) ? "running" : "not running");
    } else {
        walpe_vistrack_set_mode (D.vis, mode);
    }
    walpe_vistrack_set_see_through (D.vis, D.cfg->transparent_windows);
    walpe_vistrack_set_tuning (D.vis, D.cfg->skip_hidden_inset, settle);
    vis_push ();
}

/* percent of the monitor that can be seen (100 when nothing is tracked) */
static int vis_percent (Mon *m)
{
    gint64 a = (gint64) m->geom.width * m->geom.height;
    if (!D.vis || !m->vis_known || a <= 0)
        return 100;
    return (int) ((walpe_region_area (&m->vis) * 100 + a / 2) / a);
}

/* ------------------------------------------------------------- X11 */

#ifdef HAVE_XI2
static void xi2_event (XEvent *e);
#endif

static gboolean xdata_ready (gint fd, GIOCondition cond, gpointer user);
static void layout_schedule (void);
#ifdef HAVE_XRANDR
static int rr_event_base;
#endif

/* the X server went away (logging out, X killed): Xlib calls this and
 * exits the process. That end is not the wallpaper's fault: remove the
 * crash-loop marker and the socket first, or the next session would start
 * without wallpaper / find a dead socket. Only async-signal-safe calls
 * (this may run on any thread, inside Xlib); the paths are prepared at
 * start-up. GDK installs its own handler when the web module starts:
 * the X event source puts this one back (xsrc_prepare). */
static char *xio_marker_path, *xio_sock_path;

static int walpe_xio_error (Display *d)
{
    (void) d;
    static const char msg[] = "walpe-daemon: lost the X server connection; exit\n";
    if (xio_marker_path)
        unlink (xio_marker_path);
    if (xio_sock_path)
        unlink (xio_sock_path);
    if (write (2, msg, sizeof (msg) - 1) < 0) { /* nothing to do about it */ }
    _exit (1);
    return 0;
}

/* non-fatal X error handler: BadWindow races (selecting input on a window
 * that just died) must never take the daemon down */
static int walpe_xerror (Display *d, XErrorEvent *e)
{
    g_debug ("X error %d on resource 0x%lx (request %lu)",
             e->error_code, (unsigned long) e->resourceid, (unsigned long) e->serial);
    return 0;
}

static gboolean have_shape = FALSE; /* probed at startup */

static void shape_away_input (Window w)
{
    if (have_shape)
        XShapeCombineRectangles (D.xd, w, ShapeInput, 0, 0, NULL, 0, ShapeSet, 0);
}

/* the wallpaper window: override-redirect so no WM (dwm tiles root children,
 * desktops restack) can manage, tile or raise it; input shaped away so
 * clicks/keys reach whatever is below. This window IS the EGL surface. */
static Window create_desktop_window (const WalpeRect *g, gboolean or_mode)
{
    XSetWindowAttributes swa = {
        .override_redirect = or_mode ? True : False,
        .background_pixel = 0x000000, /* black until the first frame lands */
        /* Expose: without a compositor the server paints a part that
         * comes into view with the background; a scene that skips
         * unchanged frames (or has no frame clock) must draw it again */
        .event_mask = ExposureMask,
        .save_under = False,
        .backing_store = NotUseful,
    };
    Window w = XCreateWindow (D.xd, D.root, g->x, g->y, g->width, g->height, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWOverrideRedirect | CWBackPixel | CWEventMask |
                              CWSaveUnder | CWBackingStore, &swa);
    if (or_mode) {
        shape_away_input (w);
        XMapWindow (D.xd, w);
        XLowerWindow (D.xd, w);
    } else {
        XStoreName (D.xd, w, "walpe (preview)");
        XMapWindow (D.xd, w);
    }
    XFlush (D.xd);
    return w;
}

/* keep the wallpaper window at the bottom of the stack; re-assert often */
static void sink_desktop_window (Mon *m)
{
    if (m->xid == None)
        return;
    shape_away_input (m->xid);
    XLowerWindow (D.xd, m->xid);
}

/* are the wallpaper windows the lowest children of the root already?
 * (XQueryTree lists them bottom first) */
static gboolean wallpapers_at_bottom (void)
{
    Window r, p, *kids = NULL;
    guint n = 0, nm = g_list_length (D.mons);
    if (!XQueryTree (D.xd, D.root, &r, &p, &kids, &n))
        return FALSE;
    gboolean ok = n >= nm;
    for (guint i = 0; ok && i < nm; i++)
        ok = window_is_wallpaper (kids[i]);
    if (kids)
        XFree (kids);
    return ok;
}

/* Lowering unconditionally restacked for nothing: with two monitors each
 * XLowerWindow puts its window below the other one, so every tick both
 * monitor-sized windows changed places, the WM / a compositor / our own
 * cover scan were woken by the ConfigureNotify events, and the input
 * shape was re-sent. Now only when something got below a wallpaper - and
 * that is looked at (one XQueryTree) only after an event that can mean it
 * (stack_event), not every 5 s. */
static void restack_check (void)
{
    if (!D.stack_dirty)
        return;
    D.stack_dirty = FALSE;
    if (wallpapers_at_bottom ())
        return;
    g_debug ("a window got below the wallpaper: lowering the wallpaper windows");
    for (GList *l = D.mons; l; l = l->next)
        sink_desktop_window (l->data);
}

/* a root child was restacked: can a window be below a wallpaper window
 * now? `above` = the sibling directly below the window (None: lowest).
 * Our own lowering (wallpaper above None / above a wallpaper) is not. */
static void stack_event (Window w, Window above)
{
    gboolean wp = window_is_wallpaper (w);
    if (wp ? (above != None && !window_is_wallpaper (above))
           : (above == None || window_is_wallpaper (above)))
        D.stack_dirty = TRUE;
}

static gboolean window_is_fullscreen (Window w)
{
    if (w == None)
        return FALSE;
    Atom type = XInternAtom (D.xd, "_NET_WM_STATE", True);
    if (type == None)
        return FALSE;
    Atom fs = XInternAtom (D.xd, "_NET_WM_STATE_FULLSCREEN", True);
    guchar *data = NULL;
    gboolean found = FALSE;
    gulong n = window_prop32 (w, type, XA_ATOM, 64, &data);
    if (data) {
        Atom *atoms = (Atom *) data;
        for (gulong i = 0; i < n; i++)
            if (atoms[i] == fs)
                found = TRUE;
        XFree (data);
    }
    return found;
}

static void update_active_window (void)
{
    Atom prop = XInternAtom (D.xd, "_NET_ACTIVE_WINDOW", True);
    if (prop == None)
        return;
    guchar *data = NULL;
    Window active = None;
    gulong n = window_prop32 (D.root, prop, XA_WINDOW, 1, &data);
    if (data) {
        if (n == 1)
            active = *(Window *) data;
        XFree (data);
    }
    gboolean fs = window_is_fullscreen (active);
    gboolean changed = active != D.watched_win;
    if (fs != D.fs_active) {
        D.fs_active = fs;
        changed = TRUE;
        g_debug ("fullscreen %s", fs ? "engaged -> pausing" : "left -> resuming");
        apply_pause_state ();
    }
    if (active != D.watched_win) {
        /* clients of _NET_CLIENT_LIST are watched anyway (clients_watch) */
        if (D.watched_win != None && !client_is_watched (D.watched_win))
            XSelectInput (D.xd, D.watched_win, 0);
        if (active != None && active != D.root && !client_is_watched (active))
            XSelectInput (D.xd, active, PropertyChangeMask);
        D.watched_win = active;
    }
    /* focus changes usually accompany stack/geometry churn (a property of
     * the same window that changed nothing here does not). The scan also
     * looks at a resting pointer again: window managers that warp the
     * pointer to the focused window send no raw motion for it */
    if (changed)
        cover_schedule ();
}

static gboolean xdata_ready (gint fd, GIOCondition cond, gpointer user)
{
    (void) fd; (void) cond; (void) user;
    while (XPending (D.xd)) {
        XEvent e;
        XNextEvent (D.xd, &e);
        if (e.type == PropertyNotify) {
            static Atom a_active, a_clients, a_state, a_wmstate, a_opacity, a_wtype, a_opaque;
            if (!a_active) {
                a_active = XInternAtom (D.xd, "_NET_ACTIVE_WINDOW", False);
                a_clients = XInternAtom (D.xd, "_NET_CLIENT_LIST", False);
                a_state = XInternAtom (D.xd, "_NET_WM_STATE", False);
                a_wmstate = XInternAtom (D.xd, "WM_STATE", False);
                a_opacity = XInternAtom (D.xd, "_NET_WM_WINDOW_OPACITY", False);
                a_wtype = XInternAtom (D.xd, "_NET_WM_WINDOW_TYPE", False);
                a_opaque = XInternAtom (D.xd, "_NET_WM_OPAQUE_REGION", False);
            }
            Atom at = e.xproperty.atom;
            if (e.xproperty.window == D.root) {
                if (at == a_active)
                    update_active_window ();
                else if (at == a_clients)
                    cover_schedule (); /* clients appeared/vanished */
                /* anything else on the root (a status bar text set with
                 * xsetroot -name every second, ...) costs this wake-up and
                 * nothing more */
            } else if (at == a_state || at == a_wmstate) {
                /* a client's state: fullscreen (the focused window), hidden,
                 * iconic. Titles, _NET_WM_USER_TIME (set on every key press
                 * / click), icons ... are ignored: no round trip, no scan */
                if (e.xproperty.window == D.watched_win)
                    update_active_window ();
                cover_schedule ();
            } else if (at == a_opacity || at == a_wtype || at == a_opaque) {
                cover_schedule ();
            }
        }
        else if (ss_event_base >= 0 && e.type == ss_event_base) {
            sleep_recheck ();       /* ScreenSaverNotify: blanked / unblanked */
        }
        else if (e.type == GenericEvent) {
            if (dpms_opcode >= 0 && e.xcookie.extension == dpms_opcode)
                sleep_recheck ();   /* DPMSInfoNotify */
#ifdef HAVE_XI2
            else
                xi2_event (&e);
#endif
        }
#ifdef HAVE_XRANDR
        else if (rr_event_base >= 0 && e.type == rr_event_base + RRScreenChangeNotify) {
            XRRUpdateConfiguration (&e);
            layout_schedule ();     /* monitors changed? */
        }
#endif
        else if (e.type == Expose) {
            /* a part of a wallpaper window lost its pixels: its next frame
             * is drawn whatever the content thinks of it (a follower of a
             * shared scene presents the leader's newest frame again). A
             * window dragged across the wallpaper sends these at the
             * pointer's rate: one answer per frame interval of the fps
             * setting (expose_fire). */
            for (GList *l = D.mons; l; l = l->next) {
                Mon *m = l->data;
                if (m->xid == e.xexpose.window && !m->expose_timer_id)
                    m->expose_timer_id = g_timeout_add ((guint) MAX (1, 1000 / CLAMP (D.cfg->fps, 5, 200)),
                                                        expose_fire, m);
            }
        }
        else if (e.type == MapNotify || e.type == UnmapNotify ||
                   e.type == ConfigureNotify || e.type == CirculateNotify ||
                   e.type == DestroyNotify || e.type == ReparentNotify) {
            if (e.type == ConfigureNotify && e.xconfigure.window == D.root)
                layout_schedule (); /* the screen was resized */
            if (e.type == ConfigureNotify && e.xconfigure.event == D.root)
                stack_event (e.xconfigure.window, e.xconfigure.above);
            else if (e.type == CirculateNotify &&
                     (e.xcirculate.place == PlaceOnBottom ||
                      window_is_wallpaper (e.xcirculate.window)))
                D.stack_dirty = TRUE;
            cover_schedule (); /* window geometry/visibility changed */
        }
    }
    return G_SOURCE_CONTINUE;
}

/* The X connection as a main-loop source that also looks at Xlib's own
 * queue. A plain fd watch is not enough: every round trip on the Display
 * (XQueryPointer in the pointer poll, the cover queries, a render thread's
 * eglSwapBuffers) reads whatever the server sent and parks the events
 * that came before its reply in Xlib's memory. The socket is empty then,
 * an fd watch never fires, and those events - desktop clicks above all -
 * waited for the next unrelated X event: releases seconds late, double
 * clicks lost, drags stuck. prepare / check ask Xlib (as GDK's event
 * source does), so a parked event is dispatched in the same main-loop
 * iteration when the main thread's own round trip parked it. (Render
 * threads no longer use this connection: EGL has its own, D.egl_xd.) */
typedef struct {
    GSource src;
    GPollFD pfd;
} XEventSource;

static gboolean xsrc_prepare (GSource *src, gint *timeout)
{
    (void) src;
    *timeout = -1;
    /* GDK (the web module) installs its own I/O error handler when it
     * opens its display. Putting ours back here - every main-loop
     * iteration, i.e. right after GDK did it - costs no request and no
     * timer (it used to ride on a 1 s tick) */
    XSetIOErrorHandler (walpe_xio_error);
    return XEventsQueued (D.xd, QueuedAlready) > 0;
}

static gboolean xsrc_check (GSource *src)
{
    XEventSource *x = (XEventSource *) src;
    return (x->pfd.revents & (G_IO_IN | G_IO_HUP | G_IO_ERR)) != 0 ||
           XEventsQueued (D.xd, QueuedAlready) > 0;
}

static gboolean xsrc_dispatch (GSource *src, GSourceFunc cb, gpointer user)
{
    (void) src; (void) cb; (void) user;
    return xdata_ready (0, 0, NULL);
}

static GSourceFuncs xsrc_funcs = { xsrc_prepare, xsrc_check, xsrc_dispatch, NULL, NULL, NULL };

static void x_event_source_add (void)
{
    XEventSource *x = (XEventSource *) g_source_new (&xsrc_funcs, sizeof (XEventSource));
    x->pfd.fd = ConnectionNumber (D.xd);
    x->pfd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
    g_source_add_poll (&x->src, &x->pfd);
    g_source_set_name (&x->src, "walpe X events");
    g_source_attach (&x->src, NULL);
}

/* ------------------------------------------------------------- mouse */

/* is the pointer on the desktop, i.e. over no client window? child = the
 * root child XQueryPointer reports (input shapes respected: our wallpaper
 * windows are input-shaped away, so the bare desktop gives None); desktop
 * icon windows (_NET_WM_WINDOW_TYPE_DESKTOP: pcmanfm, xfdesktop, ...)
 * count as desktop. Like WE: clicks on the desktop reach the wallpaper,
 * clicks on windows (and on bars/panels) do not. */
static gboolean pointer_on_desktop (Window child)
{
    if (child == None || child == D.root)
        return TRUE;
    for (GList *l = D.mons; l; l = l->next)
        if (((Mon *) l->data)->xid == child)
            return TRUE;
    static Window last_w = None;
    static gboolean last_r = FALSE;
    if (child == last_w)
        return last_r;
    gboolean desk = FALSE;
    Atom type = XInternAtom (D.xd, "_NET_WM_WINDOW_TYPE", True);
    Atom dt = XInternAtom (D.xd, "_NET_WM_WINDOW_TYPE_DESKTOP", True);
    if (type != None && dt != None) {
        guchar *data = NULL;
        gulong n = window_prop32 (child, type, XA_ATOM, 16, &data);
        if (data) {
            for (gulong i = 0; i < n; i++)
                if (((Atom *) data)[i] == dt)
                    desk = TRUE;
            XFree (data);
        }
    }
    last_w = child;
    last_r = desk;
    return desk;
}

/* a left press/release somewhere on screen (XI2 raw event or IPC): goes to
 * the monitor under the pointer when that spot is desktop */
static void pointer_button (gboolean down, int rx, int ry, gboolean on_desktop)
{
    Mon *m = mon_at (rx, ry);       /* the monitor clicked on */
    Mon *r = mon_route (m);         /* the one whose scene gets it */
    if (down) {
        for (GList *l = D.mons; l; l = l->next)
            ((Mon *) l->data)->ptr_pressed = FALSE;
        if (!m || !on_desktop || !g_atomic_int_get (&r->wants_pointer) || m->own_paused)
            return;
        r->ptr_pressed = TRUE;
    } else {
        /* the release goes where the press went (it may land on a window) */
        Mon *pm = NULL;
        for (GList *l = D.mons; l; l = l->next)
            if (((Mon *) l->data)->ptr_pressed)
                pm = l->data;
        if (!pm)
            return;
        pm->ptr_pressed = FALSE;
        if (r != pm || !on_desktop) {
            mon_queue_pointer (pm, WALPE_PTR_UP, -1.0f, -1.0f);
            return;
        }
    }
    /* same-size groups: coordinates on the clicked monitor = on the scene */
    float nx = (float) (rx - m->geom.x) / m->geom.width;
    float ny = (float) (ry - m->geom.y) / m->geom.height;
    mon_queue_pointer (r, down ? WALPE_PTR_DOWN : WALPE_PTR_UP, nx, ny);
}

/* ---- web wallpapers: real pointer input for the page on a monitor.
 * Position: pointer_sample (frame rate while it moves), wherever the pointer is on that monitor
 * (pages follow the cursor like in WE). Buttons and wheel: only on the
 * empty desktop, like scene clicks; a release always follows its press. */
#ifdef HAVE_WEB
static gboolean mon_web_input (Mon *m)
{
    return m && !m->pause_wanted && g_atomic_int_get (&m->content_type) == WALPE_TYPE_WEB;
}

static void web_send (Mon *m, int kind, int button, int rx, int ry, int dx, int dy, guint32 time)
{
    WalpeWebInput in = {
        .kind = kind, .button = button,
        .x = rx - m->geom.x, .y = ry - m->geom.y,
        .mon_w = m->geom.width, .mon_h = m->geom.height,
        .dx = dx, .dy = dy, .time = time,
    };
    walpe_web_input (m, &in);
}

/* may m's page have the pointer? Not while a button pressed on another
 * monitor's page is still held (that page has it until the release) */
static gboolean web_drag_ok (Mon *m)
{
    for (GList *l = D.mons; l; l = l->next)
        if (l->data != m && ((Mon *) l->data)->web_btns)
            return FALSE;
    return TRUE;
}
#endif

G_GNUC_UNUSED static gboolean any_web_input (void)
{
#ifdef HAVE_WEB
    for (GList *l = D.mons; l; l = l->next)
        if (mon_web_input (l->data))
            return TRUE;
#endif
    return FALSE;
}

/* button 1-3 press/release or a wheel click (4 up, 5 down, 6 left, 7
 * right) at root position rx, ry (XI2 raw event or IPC) */
G_GNUC_UNUSED static void web_button (int button, gboolean down, int rx, int ry, gboolean on_desktop,
                                        guint32 time)
{
#ifdef HAVE_WEB
    Mon *m = mon_at (rx, ry);
    g_debug ("web input: button %d %s at %d,%d (%s, monitor %d)", button, down ? "down" : "up", rx, ry,
             on_desktop ? "desktop" : "over a window", m ? m->index : -1);
    if (button >= 4) {
        if (down && on_desktop && mon_web_input (m) && web_drag_ok (m))
            web_send (m, WALPE_WEB_IN_SCROLL, 0, rx, ry, button == 6 ? -1 : button == 7 ? 1 : 0,
                      button == 4 ? -1 : button == 5 ? 1 : 0, time);
        return;
    }
    guint bit = 1u << button;
    /* the release goes to the page that got the press, wherever the
     * pointer is by then (also a press whose release was never seen) */
    for (GList *l = D.mons; l; l = l->next) {
        Mon *o = l->data;
        if (o->web_btns & bit) {
            o->web_btns &= ~bit;
            web_send (o, WALPE_WEB_IN_UP, button, rx, ry, 0, 0, time);
        }
    }
    if (down && on_desktop && mon_web_input (m) && web_drag_ok (m)) {
        m->web_btns |= bit;
        m->web_in = TRUE;
        m->web_lx = rx;
        m->web_ly = ry;
        web_send (m, WALPE_WEB_IN_DOWN, button, rx, ry, 0, 0, time);
    }
#else
    (void) button; (void) down; (void) rx; (void) ry; (void) on_desktop; (void) time;
#endif
}

static gboolean any_wants_pointer (void)
{
    for (GList *l = D.mons; l; l = l->next)
        if (g_atomic_int_get (&((Mon *) l->data)->wants_pointer))
            return TRUE;
    return FALSE;
}

static void pointer_motion (void);

#ifdef HAVE_XI2
/* raw button events on the root: any client may select them (a plain
 * ButtonPress on the root is owned by the window manager).
 * XInput 2.1, not 2.0: a 2.0 client gets no raw events while another
 * client holds a grab, and a window manager that selects ButtonPress on
 * the root (dwm does, to focus the monitor clicked on) holds the implicit
 * grab from every desktop press to its release - the daemon saw the press
 * and never the release, so no click ever completed (cursorClick fires on
 * the release). From 2.1 on raw events are sent whatever the grab state. */
static void xi2_init (void)
{
    int ev, err, maj = 2, min = 1;
    if (!XQueryExtension (D.xd, "XInputExtension", &xi2_opcode, &ev, &err) ||
        XIQueryVersion (D.xd, &maj, &min) != Success) {
        xi2_opcode = -1;
        g_message ("pointer: no XInput2, desktop clicks do not reach wallpapers");
        return;
    }
}

/* Raw events are selected only while something needs them (each one wakes
 * the daemon): buttons while a wallpaper takes clicks (SceneScript cursor
 * events, or a web page), motion while a wallpaper follows a pointer that
 * is at rest (pointer_set_mode), any input while the display is off on a
 * server without DPMS events (sleep_arm). One request per change. */
static void xi2_apply_mask (void)
{
    static int cur = -1;
    if (xi2_opcode < 0)
        return;
    int want = (xi_want_buttons ? 1 : 0) | (xi_want_motion ? 2 : 0) | (xi_want_wake ? 4 : 0);
    if (want == cur)
        return;
    gboolean clicks_changed = cur < 0 || ((cur ^ want) & 1);
    cur = want;
    unsigned char mask[XIMaskLen (XI_LASTEVENT)] = { 0 };
    if (want & 1) {
        XISetMask (mask, XI_RawButtonPress);
        XISetMask (mask, XI_RawButtonRelease);
    }
    if (want & 2)
        XISetMask (mask, XI_RawMotion);
    if (want & 4) {
        XISetMask (mask, XI_RawMotion);
        XISetMask (mask, XI_RawKeyPress);
        XISetMask (mask, XI_RawButtonPress);
    }
    XIEventMask em = { XIAllMasterDevices, sizeof (mask), mask };
    XISelectEvents (D.xd, D.root, &em, 1);
    XFlush (D.xd);
    if (clicks_changed)
        g_message ("pointer: desktop clicks %s",
                   (want & 1) ? "selected (cursor events / web input)" : "not selected");
}

static void xi2_event (XEvent *e)
{
    if (e->xcookie.extension != xi2_opcode)
        return;
    int t = e->xcookie.evtype;
    if (xi_want_wake && (t == XI_RawMotion || t == XI_RawKeyPress || t == XI_RawButtonPress))
        sleep_wake_kick ();     /* input while the display was off */
    if (t == XI_RawMotion) {
        pointer_motion ();      /* the pointer left its resting place */
        return;                 /* the event's data is not needed */
    }
    if (t != XI_RawButtonPress && t != XI_RawButtonRelease)
        return;
    if (!xi_want_buttons || !XGetEventData (D.xd, &e->xcookie))
        return;
    XIRawEvent *re = e->xcookie.data;
    /* scenes: left button; web pages: left/middle/right and the wheel (4-7) */
    gboolean scene = re->detail == 1 && any_wants_pointer ();
    gboolean web = re->detail >= 1 && re->detail <= 7 && any_web_input ();
    if (scene || web) {
        Window r, c;
        int rx, ry, wx, wy;
        unsigned mask = 0;
        if (XQueryPointer (D.xd, D.root, &r, &c, &rx, &ry, &wx, &wy, &mask)) {
            gboolean desk = pointer_on_desktop (c);
            if (scene)
                pointer_button (t == XI_RawButtonPress, rx, ry, desk);
            if (web)
                web_button (re->detail, t == XI_RawButtonPress, rx, ry, desk,
                            (guint32) re->time);
        }
    }
    XFreeEventData (D.xd, &e->xcookie);
}
#else
static void xi2_apply_mask (void)
{
}
#endif

/* ---- pointer position
 * OFF:   nothing shown uses the pointer: no events, no timer.
 * TRACK: one XQueryPointer per frame interval (30..144 Hz, as before)
 *        while the pointer moves.
 * ARMED: the pointer has not moved for PTR_REST_MS: the timer is gone and
 *        XI_RawMotion is selected on the root. The first motion event goes
 *        back to TRACK and deselects motion again (a 1000 Hz mouse would
 *        otherwise wake the daemon a thousand times a second).
 * So a pointer at rest costs nothing; it used to cost one round trip per
 * frame interval for as long as such a wallpaper was visible. Without
 * XInput2 the rest state polls every PTR_SLOW_MS instead. */
#define PTR_REST_MS 400
#define PTR_SLOW_MS 100
enum { PTR_OFF, PTR_ARMED, PTR_TRACK };
static int ptr_mode = PTR_OFF;
static guint ptr_timer;
static gint64 ptr_moved_us;
static int ptr_seen_x = G_MININT, ptr_seen_y = G_MININT;
static Window ptr_seen_child = None;
static guint ptr_poll_ms;            /* interval of the running TRACK timer */
static gboolean ptr_always_wake;     /* WALPE_PTR_WAKE=1: wake the render thread for
                                      * every pointer move, as before (A/B) */

static gboolean pointer_tick (gpointer user);
static gboolean pointer_sample (void);

/* scenes use the pointer for parallax, web pages get real mousemove
 * events: some unpaused monitor showing one of those */
static gboolean mouse_needed (void)
{
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        int t = g_atomic_int_get (&m->content_type);
        /* a scene only when it draws with the pointer position (parallax
         * effects, cursor-linked particles, scripts reading the cursor):
         * every visible scene used to cost one XQueryPointer round trip per
         * frame interval, also static ones and those that never look at
         * the pointer */
        if (!m->pause_wanted && ((t == WALPE_TYPE_SCENE && D.cfg->parallax &&
                                  g_atomic_int_get (&m->uses_ptr_pos)) ||
                                 t == WALPE_TYPE_WEB || g_atomic_int_get (&m->wants_pointer)))
            return TRUE;
    }
    return FALSE;
}

/* the tracking rate: the frame rate, 30..144 Hz */
static guint mouse_poll_ms (void)
{
    int rate = walpe_paced_fps (D.cfg->fps, D.no_refresh_cap ? 0.0 : D.max_hz);
    /* ... the rate the wallpapers that use it are drawn at: with a
     * focus / battery limit or fps_idle of 15, the pointer was still asked
     * for 60 times a second while it moved. Web pages get input events of
     * their own timing: the full rate. Below 30 only for such a limit. */
    int need = 0;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        int t = g_atomic_int_get (&m->content_type);
        if (m->pause_wanted)
            continue;
        if (t == WALPE_TYPE_WEB)
            need = rate;
        else if (g_atomic_int_get (&m->wants_pointer) ||
                 (t == WALPE_TYPE_SCENE && D.cfg->parallax && g_atomic_int_get (&m->uses_ptr_pos)))
            need = MAX (need, m->fps_cap);
    }
    return walpe_pointer_poll_ms (rate, need);
}

static void pointer_set_mode (int mode);
/* a frame rate limit changed while the pointer is tracked: the
 * poll takes its new interval now, not after the next rest */
static void pointer_repace (void)
{
    if (ptr_mode == PTR_TRACK && ptr_poll_ms != MOUSE_POLL_MS)
        pointer_set_mode (PTR_TRACK);
}

static void pointer_set_mode (int mode)
{
    static const char *const names[] = { "off", "at rest (waiting for motion)", "tracking" };
    if (ptr_timer) {
        g_source_remove (ptr_timer);
        ptr_timer = 0;
    }
    if (mode != ptr_mode)
        g_debug ("pointer position: %s", names[mode]);
    ptr_mode = mode;
    if (mode == PTR_TRACK)
        ptr_timer = g_timeout_add ((ptr_poll_ms = MOUSE_POLL_MS), pointer_tick, NULL);
    else if (mode == PTR_ARMED && xi2_opcode < 0)
        ptr_timer = g_timeout_add (PTR_SLOW_MS, pointer_tick, NULL);
    xi_want_motion = mode == PTR_ARMED && xi2_opcode >= 0;
    xi2_apply_mask ();
}

static gboolean pointer_tick (gpointer user)
{
    (void) user;
    if (!mouse_needed ()) {
        ptr_timer = 0;          /* this source ends */
        pointer_set_mode (PTR_OFF);
        return G_SOURCE_REMOVE;
    }
    gboolean moved = pointer_sample ();
    gint64 now = g_get_monotonic_time ();
    if (moved)
        ptr_moved_us = now;
    /* WALPE_POINTER_POLL=1: the old behaviour, one query per frame
     * interval for as long as a wallpaper uses the pointer (off switch for
     * the event-driven rest state, should raw motion events not arrive) */
    static int always = -1;
    if (always < 0)
        always = g_getenv ("WALPE_POINTER_POLL") != NULL;
    if (!always && ptr_mode == PTR_TRACK && now - ptr_moved_us > PTR_REST_MS * 1000) {
        ptr_timer = 0;
        pointer_set_mode (PTR_ARMED);
        return G_SOURCE_REMOVE;
    }
    if (ptr_mode == PTR_ARMED && moved) {   /* no XInput2: the slow poll saw it move */
        ptr_timer = 0;
        pointer_set_mode (PTR_TRACK);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* XI_RawMotion while at rest */
static void pointer_motion (void)
{
    if (ptr_mode != PTR_ARMED)
        return;
    ptr_moved_us = g_get_monotonic_time ();
    pointer_set_mode (PTR_TRACK);
    pointer_sample ();
}

/* windows changed while the pointer rests: what is under it may be
 * another window now (hover of cursor scripts / web pages) */
static void pointer_rest_sample (void)
{
    if (ptr_mode == PTR_ARMED && xi2_opcode >= 0)
        pointer_sample ();
}

/* what the wallpapers need from the pointer changed (pause state, a load,
 * the parallax setting): event driven, there is no recheck timer */
static void pointer_update (void)
{
    static gboolean env_read;
    if (!env_read) {
        env_read = TRUE;
        ptr_always_wake = g_getenv ("WALPE_PTR_WAKE") != NULL;
    }
    xi_want_buttons = any_wants_pointer () || any_web_input ();
#ifdef HAVE_WEB
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        if (!mon_web_input (m) && (m->web_in || m->web_btns)) {
            /* paused, or another wallpaper now: the page lost the pointer
             * (it gets the position again when it is back) */
            m->web_in = FALSE;
            m->web_btns = 0;
            web_send (m, WALPE_WEB_IN_LEAVE, 0, 0, 0, 0, 0, 0);
        }
    }
#endif
    if (!mouse_needed ()) {
        if (ptr_mode != PTR_OFF)
            pointer_set_mode (PTR_OFF);
    } else if (ptr_mode == PTR_OFF) {
        ptr_moved_us = g_get_monotonic_time ();
        pointer_set_mode (PTR_TRACK);   /* the current position first, then rest */
    }
    xi2_apply_mask ();
}

/* one XQueryPointer, handed to every monitor that uses it; TRUE = the
 * pointer (or the window under it) is not where it was last time */
static gboolean pointer_sample (void)
{
    Window r, c;
    int rx, ry, wx, wy;
    unsigned mask = 0;
    if (!XQueryPointer (D.xd, D.root, &r, &c, &rx, &ry, &wx, &wy, &mask))
        return FALSE;
    gboolean moved = rx != ptr_seen_x || ry != ptr_seen_y || c != ptr_seen_child;
    ptr_seen_x = rx;
    ptr_seen_y = ry;
    ptr_seen_child = c;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        int t = g_atomic_int_get (&m->content_type);
        if (m->pause_wanted)
            continue;
#ifdef HAVE_WEB
        if (t == WALPE_TYPE_WEB) {
            /* each page gets the pointer only while it is on its monitor
             * (or a button pressed there is still held: a drag) */
            gboolean in = rx >= m->geom.x && rx < m->geom.x + m->geom.width &&
                          ry >= m->geom.y && ry < m->geom.y + m->geom.height &&
                          web_drag_ok (m);
            if (in || m->web_btns) {
                if (!m->web_in || rx != m->web_lx || ry != m->web_ly) {
                    m->web_in = TRUE;
                    m->web_lx = rx;
                    m->web_ly = ry;
                    web_send (m, WALPE_WEB_IN_MOVE, 0, rx, ry, 0, 0, 0);
                }
            } else if (m->web_in) {
                m->web_in = FALSE;
                web_send (m, WALPE_WEB_IN_LEAVE, 0, rx, ry, 0, 0, 0);
            }
            continue;
        }
#endif
        if (g_atomic_int_get (&m->wants_pointer) && !m->leader) {
            /* hover/move for SceneScript cursor events: on the desktop of
             * this monitor (or of a monitor following it), else "left" once */
            gboolean desk = pointer_on_desktop (c) || m->ptr_pressed;
            Mon *on = NULL;
            for (GList *l2 = D.mons; desk && l2 && !on; l2 = l2->next) {
                Mon *o = l2->data;
                if ((o == m || o->leader == m) && !o->own_paused &&
                    rx >= o->geom.x && rx < o->geom.x + o->geom.width &&
                    ry >= o->geom.y && ry < o->geom.y + o->geom.height)
                    on = o;
            }
            gboolean in = on != NULL;
            float nx = in ? (float) (rx - on->geom.x) / on->geom.width : -1.0f;
            float ny = in ? (float) (ry - on->geom.y) / on->geom.height : -1.0f;
            if (fabsf (nx - m->ptr_lx) > 1e-4f || fabsf (ny - m->ptr_ly) > 1e-4f) {
                m->ptr_lx = nx;
                m->ptr_ly = ny;
                mon_queue_pointer (m, WALPE_PTR_MOVE, nx, ny);
            }
        }
        if (t != WALPE_TYPE_SCENE || !D.cfg->parallax || m->leader ||
            !g_atomic_int_get (&m->uses_ptr_pos))
            continue;
        /* (pointer_share) a scene rendered once for a group of
         * monitors follows the pointer on the monitor the pointer is on:
         * its position there, as that monitor's own rendering would get it */
        const WalpeRect *pg = &m->geom;
        for (GList *l2 = D.mons; m->nfollowers && l2; l2 = l2->next) {
            Mon *o = l2->data;
            if (o->leader == m && !o->own_paused &&
                rx >= o->geom.x && rx < o->geom.x + o->geom.width &&
                ry >= o->geom.y && ry < o->geom.y + o->geom.height)
                pg = &o->geom;
        }
        float nx, ny;
        walpe_pointer_norm (rx, ry, pg->x, pg->y, pg->width, pg->height, &nx, &ny);
        if (fabsf (nx - m->mx) > 0.001f || fabsf (ny - m->my) > 0.001f) {
            g_mutex_lock (&m->lock);
            m->mx = nx;            /* read by the render thread under the lock */
            m->my = ny;
            m->ptr_dirty = TRUE;   /* mon_thread: a frame only if none is coming */
            /* a thread asleep until its content's next frame is
             * due draws that frame with m->mx / m->my whatever happens:
             * waking it now only sent it back to sleep (one wake-up per
             * pointer sample and monitor, 60 a second while the mouse moves) */
            if (!m->clock_wait || ptr_always_wake)
                g_cond_signal (&m->cond);
            g_mutex_unlock (&m->lock);
        }
    }
    return moved;
}

/* ------------------------------------------------------------- import
 * `walpe-ctl add` / GUI import: common/import.c runs ffmpeg as a child
 * process (async on this main loop) and normalizes any video into H.264
 * High yuv420p <=1080p MP4 (+faststart, no audio: the renderer is silent)
 * in <wallpapers_dir>/imported/, plus a <stem>.jpg grid thumbnail. One
 * import runs at a time; status reports progress. */

static struct {
    WalpeImport *job;        /* running (or finished, kept for status) */
    gboolean active;
    char *label;
    char *mode;
    char *error;
    int percent;
    const char *purpose;     /* "add" (library import) | "convert" (see Conv) */
} Imp;

static void import_done (WalpeImport *job, gboolean ok, const char *dst,
                         const char *error, gpointer user)
{
    (void) user;
    if (job != Imp.job)
        return;
    Imp.active = FALSE;
    Imp.percent = ok ? 100 : -1;
    g_free (Imp.mode);
    Imp.mode = g_strdup (walpe_import_mode (job));
    g_free (Imp.error);
    Imp.error = ok ? NULL : g_strdup (error ? error : "import failed");
    if (ok)
        g_message ("import done: %s", dst);
    else
        g_warning ("import failed: %s", Imp.error);
    state_touch ();
}

static GError *start_import (const char *abs)
{
    char *dir = walpe_expand_path (D.cfg->wallpapers_dir ? D.cfg->wallpapers_dir
                                                         : walpe_default_dir ());
    char *impdir = g_build_filename (dir, "imported", NULL);
    g_free (dir);
    GError *err = NULL;
    WalpeImport *job = walpe_import_start (abs, impdir, NULL, import_done, NULL, &err);
    g_free (impdir);
    if (!job)
        return err;
    if (Imp.job)
        walpe_import_free (Imp.job);
    Imp.job = job;
    Imp.active = TRUE;
    Imp.percent = -1;
    Imp.purpose = "add";
    g_free (Imp.label);
    Imp.label = g_strdup (walpe_import_label (job));
    g_clear_pointer (&Imp.error, g_free);
    g_clear_pointer (&Imp.mode, g_free);
    g_message ("import started: %s", abs);
    return NULL;
}

/* ------------------------------------------- video conversion on open
 * The player reads H.264 in MP4 / MOV. A video wallpaper in anything else
 * (WebM, HEVC, AV1, MKV, fragmented MP4: Workshop items of type video
 * come in those too) used to fail to load. Such a file now goes through
 * the import pipeline (ffprobe -> ffmpeg, the same rules as `walpe-ctl
 * add`: H.264 High yuv420p, at most 1920x1080, no audio) once, into
 * ~/.cache/walpe/video/<hash of path + size + mtime>.mp4, and the player
 * shows that copy (engine/video.c) while the wallpaper keeps its own path
 * in the config, the options and status. Asynchronous on the main loop:
 * the wallpaper on screen stays until the copy is ready; status shows the
 * progress through the import fields ("import_purpose": "convert"). One
 * import or conversion runs at a time. */
static struct {
    char *wallpaper;    /* shown when the copy is ready (as opened), NULL = nothing waits */
    char *mon;          /* output it was opened on, NULL = every monitor */
    gboolean commit;    /* an `open` waits (config is set then); FALSE = reload only */
} Conv;
static GHashTable *conv_failed;     /* media files whose conversion failed in this run */

static void open_commit (const char *abs, Mon *tm);
static void convert_scan (void);

/* can the player show wallpaper abs now, the way it is meant to look?
 * Non-video content: yes. *src (g_free) = the video file when it cannot;
 * *why (g_free) = the reason; *soft = the file itself plays, but sideways
 * or stretched (display matrix / pixel aspect ratio, which the player does
 * not apply): worth converting, yet usable as it is when that fails. */
static gboolean video_playable (const char *abs, char **src, char **why, gboolean *soft)
{
    char *media = NULL;
    if (src)
        *src = NULL;
    if (why)
        *why = NULL;
    if (soft)
        *soft = FALSE;
    if (walpe_detect_type (abs, &media) != WALPE_TYPE_VIDEO) {
        g_free (media);
        return TRUE;
    }
    char *file = media ? media : g_strdup (abs);
    GError *e = NULL;
    Mp4Demux *dm = mp4_demux_open (file, &e);
    gboolean odd = dm && (dm->rotation != 0 || dm->sar_num != dm->sar_den);
    if (odd) {
        g_set_error (&e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "%s is stored %s", file, dm->rotation ? "rotated" : "with non-square pixels");
        g_clear_pointer (&dm, mp4_demux_free);
    }
    if (!dm) {
        char *cf = walpe_video_cache_file (file);
        if (cf && g_file_test (cf, G_FILE_TEST_IS_REGULAR))
            dm = mp4_demux_open (cf, NULL);     /* converted earlier */
        g_free (cf);
    }
    gboolean ok = dm != NULL;
    if (dm)
        mp4_demux_free (dm);
    if (!ok && soft)
        *soft = odd;
    if (!ok && why)
        *why = g_strdup (e ? e->message : "cannot read the video");
    g_clear_error (&e);
    if (!ok && src)
        *src = file;
    else
        g_free (file);
    return ok;
}

static void convert_done (WalpeImport *job, gboolean ok, const char *dst, const char *error,
                          gpointer user)
{
    if (job != Imp.job)
        return;
    char *src = user;       /* owned: the media file */
    import_done (job, ok, dst, error, NULL);
    char *wp = g_steal_pointer (&Conv.wallpaper), *mon = g_steal_pointer (&Conv.mon);
    if (!ok) {
        if (!conv_failed)
            conv_failed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_add (conv_failed, g_strdup (src));
    } else if (wp) {
        Mon *tm = NULL;
        for (GList *l = D.mons; mon && l; l = l->next)
            if (!g_strcmp0 (((Mon *) l->data)->name, mon))
                tm = l->data;
        if (Conv.commit && (!mon || tm))
            open_commit (wp, tm);
        else
            apply_wallpapers (wp, NULL, FALSE, FALSE);  /* reload where it is wanted */
    }
    g_free (wp);
    g_free (mon);
    g_free (src);
    convert_scan ();    /* another saved wallpaper may be waiting for its copy */
}

/* start converting video file src for wallpaper wp (shown on monitor mon,
 * NULL = all, when done). NULL = started, else why not. */
static GError *convert_start (const char *src, const char *wp, const char *mon, gboolean commit)
{
    if (Imp.active)
        return g_error_new (G_IO_ERROR, G_IO_ERROR_BUSY, "%s of %s is still running; open the "
                            "wallpaper again when it is done",
                            !g_strcmp0 (Imp.purpose, "convert") ? "the conversion" : "the import",
                            Imp.label ? Imp.label : "a video");
    char *dst = walpe_video_cache_file (src);
    GError *err = NULL;
    WalpeImport *job = dst ? walpe_import_start_to (src, dst, NULL, convert_done, g_strdup (src), &err)
                           : NULL;
    g_free (dst);
    if (!job)
        return err ? err : g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no such file: %s", src);
    if (Imp.job)
        walpe_import_free (Imp.job);
    Imp.job = job;
    Imp.active = TRUE;
    Imp.percent = -1;
    Imp.purpose = "convert";
    g_free (Imp.label);
    Imp.label = g_strdup (walpe_import_label (job));
    g_clear_pointer (&Imp.error, g_free);
    g_clear_pointer (&Imp.mode, g_free);
    g_free (Conv.wallpaper);
    g_free (Conv.mon);
    Conv.wallpaper = g_strdup (wp);
    Conv.mon = g_strdup (mon);
    Conv.commit = commit;
    if (conv_failed)
        g_hash_table_remove (conv_failed, src);
    g_message ("video: %s cannot be shown as it is (not H.264 in MP4 / MOV, or stored rotated): "
               "converting it once (ffmpeg, at most 1920x1080), the wallpaper changes when the "
               "copy is ready", src);
    return NULL;
}

/* start-up (this daemon holds the instance lock, so nothing is being
 * converted): remove the partial outputs a killed daemon left behind, and
 * copies no wallpaper has played for CONV_CACHE_DAYS (the player touches
 * the copy it plays: engine/video.c) so the cache does not grow for ever */
#define CONV_CACHE_DAYS 90
static void conv_cache_tidy (void)
{
    char *dir = g_build_filename (g_get_user_cache_dir (), "walpe", "video", NULL);
    GDir *d = g_dir_open (dir, 0, NULL);
    gint64 now = g_get_real_time () / G_USEC_PER_SEC;
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        gboolean part = g_str_has_suffix (n, ".mp4.part");
        if (!part && !g_str_has_suffix (n, ".mp4"))
            continue;
        char *p = g_build_filename (dir, n, NULL);
        GStatBuf st;
        if (g_lstat (p, &st) == 0 && S_ISREG (st.st_mode) &&
            (part || now - (gint64) st.st_mtime > (gint64) CONV_CACHE_DAYS * 86400)) {
            g_message ("video: removing %s (%s)", p, part ? "unfinished conversion"
                                                          : "converted copy not used for a long time");
            g_remove (p);
        }
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    g_free (dir);
}

/* a wallpaper some monitor is set to (saved config, a forgotten cache)
 * whose video still has no playable copy: convert it, then reload it */
static void convert_scan (void)
{
    for (GList *l = D.mons; l && !Imp.active; l = l->next) {
        Mon *m = l->data;
        char *src = NULL;
        if (m->want_path && *m->want_path && !video_playable (m->want_path, &src, NULL, NULL) && src &&
            !(conv_failed && g_hash_table_contains (conv_failed, src))) {
            GError *e = convert_start (src, m->want_path, NULL, FALSE);
            if (e) {
                g_warning ("video: %s cannot be converted: %s", src, e->message);
                if (!conv_failed)
                    conv_failed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
                g_hash_table_add (conv_failed, g_strdup (src));
                g_error_free (e);
            }
        }
        g_free (src);
    }
}

/* --------------------------------------------- workshop install (steamcmd) */

static struct {
    WalpeWorkshopJob *job;
    gboolean active;
    char *id;
    char *error;
    char *dir;       /* installed dir of the last success */
    char *title;
} Ws;

static void ws_done (WalpeWorkshopJob *job, gboolean ok, const char *dir, const char *error,
                     gpointer user)
{
    (void) user;
    if (job != Ws.job)
        return;
    Ws.active = FALSE;
    g_free (Ws.error);
    Ws.error = ok ? NULL : g_strdup (error ? error : "install failed");
    g_free (Ws.dir);
    Ws.dir = ok ? g_strdup (dir) : NULL;
    g_free (Ws.title);
    Ws.title = g_strdup (walpe_workshop_title (job));
    if (ok)
        g_message ("workshop install done: %s (%s)", dir, Ws.title ? Ws.title : "");
    else
        g_warning ("workshop install failed: %s", Ws.error);
    state_touch ();
}

static GError *start_install (const char *id_or_url)
{
    if (Ws.active) {
        return g_error_new (G_IO_ERROR, G_IO_ERROR_BUSY, "an install of %s is running",
                            Ws.id ? Ws.id : "?");
    }
    char *lib = walpe_expand_path (D.cfg->wallpapers_dir ? D.cfg->wallpapers_dir
                                                         : walpe_default_dir ());
    GError *err = NULL;
    WalpeWorkshopJob *job = walpe_workshop_install (id_or_url, D.cfg->steam_user, lib,
                                                    ws_done, NULL, &err);
    g_free (lib);
    if (!job)
        return err;
    if (Ws.job)
        walpe_workshop_free (Ws.job);
    Ws.job = job;
    Ws.active = TRUE;
    g_free (Ws.id);
    Ws.id = walpe_workshop_parse_id (id_or_url);
    g_clear_pointer (&Ws.error, g_free);
    g_clear_pointer (&Ws.dir, g_free);
    g_clear_pointer (&Ws.title, g_free);
    g_message ("workshop install started: %s", Ws.id);
    return NULL;
}

/* ------------------------------------------------------------- IPC
 * Replies are built with JsonBuilder (never printf): every reply is valid
 * JSON whatever the paths, labels or property values contain. */

static void jb_str (JsonBuilder *b, const char *k, const char *v)
{
    json_builder_set_member_name (b, k);
    json_builder_add_string_value (b, v ? v : "");
}

static void jb_bool (JsonBuilder *b, const char *k, gboolean v)
{
    json_builder_set_member_name (b, k);
    json_builder_add_boolean_value (b, v);
}

static void jb_int (JsonBuilder *b, const char *k, gint64 v)
{
    json_builder_set_member_name (b, k);
    json_builder_add_int_value (b, v);
}

static void jb_fail (JsonBuilder *b, const char *error)
{
    jb_bool (b, "ok", FALSE);
    jb_str (b, "error", error);
}

/* an integer from a request: the double is limited before the cast (a
 * double outside the int range, e.g. 1e300, is undefined behaviour in a
 * plain (int) cast) */
static int ipc_int (JsonObject *o, const char *k, int dflt)
{
    double v = wpj_num (o, k, dflt);
    if (!(v >= -1e9))       /* also NaN */
        return v < 0 ? -1000000000 : dflt;
    return (int) MIN (v, 1e9);
}

/* request member k holds a number / a string (a member of the wrong type
 * is ignored: it used to reset the setting to its default) */
static gboolean ipc_has_num (JsonObject *o, const char *k)
{
    JsonNode *n = json_object_get_member (o, k);
    if (!n || !JSON_NODE_HOLDS_VALUE (n))
        return FALSE;
    GType t = json_node_get_value_type (n);
    return t == G_TYPE_DOUBLE || t == G_TYPE_INT64;
}

static gboolean ipc_has_str (JsonObject *o, const char *k)
{
    JsonNode *n = json_object_get_member (o, k);
    return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING;
}

/* a scalar JSON value as the override string stored in config "props";
 * NULL for non-scalars. json-glib reports integers as INT64. */
static char *json_value_to_prop (JsonNode *vn)
{
    if (!vn || json_node_get_node_type (vn) != JSON_NODE_VALUE)
        return NULL;
    GType t = json_node_get_value_type (vn);
    if (t == G_TYPE_STRING)
        return g_strdup (json_node_get_string (vn));
    if (t == G_TYPE_BOOLEAN)
        return g_strdup (json_node_get_boolean (vn) ? "true" : "false");
    if (t == G_TYPE_INT64)
        return g_strdup_printf ("%" G_GINT64_FORMAT, json_node_get_int (vn));
    if (t == G_TYPE_DOUBLE) {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        return g_strdup (g_ascii_dtostr (buf, sizeof (buf), json_node_get_double (vn)));
    }
    return NULL;
}

/* add an override string back as a typed JSON value (by declared type) */
static void jb_prop_value (JsonBuilder *b, const char *type, const char *cur)
{
    if (!g_strcmp0 (type, "bool") || !g_strcmp0 (type, "checkbox")) {
        json_builder_add_boolean_value (b, !g_ascii_strcasecmp (cur, "true") ||
                                           !g_strcmp0 (cur, "1"));
        return;
    }
    if (!g_strcmp0 (type, "slider") || !g_strcmp0 (type, "combo")) {
        char *end = NULL;
        double d = g_ascii_strtod (cur, &end);
        if (end && end != cur && *end == 0 && isfinite (d)) {
            if (d == (double) (gint64) d && fabs (d) < 9e15)
                json_builder_add_int_value (b, (gint64) d);
            else
                json_builder_add_double_value (b, d);
            return;
        }
    }
    json_builder_add_string_value (b, cur); /* combos may be strings */
}

/* the wallpaper a props/setprop request is about: {"monitor": N} = what
 * monitor N shows, else the default (`current`); NULL = none / no such
 * monitor */
static const char *ipc_target_path (JsonObject *o)
{
    /* "path": any wallpaper (the GUI edits options of one that is not
     * shown; nothing reloads unless a monitor shows it) */
    static char *by_path;
    const char *pp = wpj_str (o, "path", NULL);
    if (pp && *pp) {
        g_free (by_path);
        by_path = walpe_expand_path (pp);
        return by_path;
    }
    const char *p = D.cfg->current;
    if (json_object_has_member (o, "monitor")) {
        Mon *m = mon_by_index (ipc_int (o, "monitor", -1));
        p = m ? m->want_path : NULL;
    }
    return p && *p ? p : NULL;
}

#include "layers_ipc.h"   /* "layers" / "layeradj": ipc_layers, ipc_layeradj */
#include "common/setkeys.h"   /* "set": the members it may carry (walpe_set_check) */

/* setprop / setprops: the keys of the request that wallpaper wp does not
 * declare (project.json, a preset's base item, WalPE's built-in options),
 * as a "note" in the reply. The values are stored all the same - a scene
 * may bind an option its project.json does not list - but a mistyped key
 * used to be answered with a bare ok and nothing changed on screen. */
static void ipc_props_note (JsonBuilder *b, const char *wp, GPtrArray *keys)
{
    JsonNode *root = wp ? walpe_content_property_defs (wp, NULL) : NULL;
    JsonObject *defs = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
    GString *unknown = g_string_new (NULL);
    for (guint i = 0; defs && i < keys->len; i++)
        if (!json_object_has_member (defs, keys->pdata[i]))
            g_string_append_printf (unknown, "%s%s", unknown->len ? ", " : "",
                                    (const char *) keys->pdata[i]);
    if (unknown->len) {
        char *msg = g_strdup_printf ("saved, but this wallpaper declares no option named %s "
                                     "(walpe-ctl props lists its options)", unknown->str);
        jb_str (b, "note", msg);
        g_free (msg);
    }
    g_string_free (unknown, TRUE);
    if (root)
        json_node_unref (root);
}

static void ipc_props (JsonBuilder *b, const char *wp)
{
    /* the wallpaper's own properties (a preset: its base item's, with the
     * preset's values as defaults) and WalPE's built-in ones for its type
     * (alignment of video/GIF/image wallpapers: fit.h) */
    JsonNode *root2 = wp ? walpe_content_property_defs (wp, NULL) : NULL;
    JsonObject *defs = root2 ? json_node_get_object (root2) : NULL;
    jb_bool (b, "ok", TRUE);
    json_builder_set_member_name (b, "props");
    json_builder_begin_object (b);
    GList *ks = defs ? json_object_get_members (defs) : NULL;
    for (GList *k = ks; k; k = k->next) {
        JsonObject *d2 = wpj_obj (defs, k->data);
        if (!d2)
            continue;
        const char *ty = wpj_str (d2, "type", "bool");
        json_builder_set_member_name (b, k->data);
        json_builder_begin_object (b);
        jb_str (b, "type", ty);
        jb_str (b, "text", wpj_str (d2, "text", ""));
        json_builder_set_member_name (b, "default");
        JsonNode *dv = json_object_get_member (d2, "value");
        if (dv && json_node_get_node_type (dv) == JSON_NODE_VALUE)
            json_builder_add_value (b, json_node_copy (dv)); /* keeps int/double/str/bool */
        else
            json_builder_add_null_value (b);
        json_builder_set_member_name (b, "value");
        char *cur = walpe_config_get_prop (wp, k->data);
        if (cur)
            jb_prop_value (b, ty, cur);
        else
            json_builder_add_null_value (b);
        g_free (cur);
        json_builder_end_object (b);
    }
    g_list_free (ks);
    json_builder_end_object (b);
    if (root2)
        json_node_unref (root2);
}

static void ipc_status (JsonBuilder *b)
{
    Mon *m0 = D.mons ? D.mons->data : NULL;
    jb_bool (b, "ok", TRUE);
    json_builder_set_member_name (b, "state");
    json_builder_begin_object (b);
    jb_str (b, "path", D.cfg->current);
    jb_str (b, "type", m0 ? walpe_type_name (g_atomic_int_get (&m0->content_type)) : "none");
    jb_bool (b, "paused", D.cfg->paused);
    jb_bool (b, "fullscreen_pause", D.cfg->pause_fullscreen && D.fs_active);
    jb_bool (b, "dpms_pause", D.cfg->pause_dpms && D.asleep);
    jb_bool (b, "covered_pause", D.cfg->pause_covered && m0 && m0->covered);
    /* the opt-in rules */
    jb_str (b, "focus_action", walpe_focus_action_name (D.cfg->focus_action));
    jb_bool (b, "on_battery", D.on_battery);
    jb_bool (b, "battery_pause", D.cfg->pause_battery && D.on_battery);
    {
        /* per monitor: covered, paused, following (render_shared) */
        GString *mons = g_string_new (NULL);
        int shared = 0;
        for (GList *l = D.mons; l; l = l->next) {
            Mon *m = l->data;
            g_string_append_printf (mons, "%smon%d:%s%s%s", mons->len ? " " : "", m->index,
                                    m->own_paused ? "paused" : "running",
                                    m->covered ? ",covered" : "",
                                    m->leader ? ",follows" : m->nfollowers ? ",leads" : "");
            if (D.vis && m->vis_known && vis_percent (m) < 100)
                g_string_append_printf (mons, ",visible %d%%", vis_percent (m));
            shared += m->leader != NULL;
        }
        jb_str (b, "monitors_state", mons->str);
        g_string_free (mons, TRUE);
        jb_int (b, "shared_followers", shared);
        if (D.shared_note)
            jb_str (b, "shared_off", D.shared_note);
        jb_bool (b, "pointer_share", pointer_share_on ());
        /* "Skip hidden areas": the setting, and whether it is at work
         * (auto: not under a compositing manager) */
        jb_str (b, "skip_hidden", walpe_skip_hidden_name (D.cfg->skip_hidden));
        if (D.vis) {
            WalpeVisStats vs;
            walpe_vistrack_stats (D.vis, &vs);
            jb_int (b, "skip_hidden_margin", D.cfg->skip_hidden_margin);
            jb_int (b, "skip_hidden_inset", D.cfg->skip_hidden_inset);
            jb_bool (b, "skip_hidden_active", walpe_vistrack_clipping (D.vis));
            char *t = g_strdup_printf ("%u windows, %" G_GUINT64_FORMAT " events, %"
                                       G_GUINT64_FORMAT " region passes, %" G_GUINT64_FORMAT
                                       " changes, %" G_GUINT64_FORMAT " round trips, %"
                                       G_GINT64_FORMAT " us", vs.windows, vs.events,
                                       vs.recomputes, vs.changes, vs.roundtrips, vs.cpu_us);
            jb_str (b, "skip_hidden_tracker", t);
            g_free (t);
            /* uncovered pixels: time from the X event to the swap of the
             * frame that shows them, per monitor "last/avg/max (n)", and
             * how many frames that was (last/max; 1 = the very next) */
            GString *ls = g_string_new (NULL);
            for (GList *l = D.mons; l; l = l->next) {
                Mon *m = l->data;
                g_mutex_lock (&m->lock);
                g_string_append_printf (ls, "%smon%d %.1f/%.1f/%.1f ms (%u) frames %u/%u",
                                        ls->len ? " " : "", m->index, m->lat_last_us / 1e3,
                                        m->lat_n ? m->lat_sum_us / 1e3 / m->lat_n : 0.0,
                                        m->lat_max_us / 1e3, m->lat_n, m->lat_frames_last,
                                        m->lat_frames_max);
                g_mutex_unlock (&m->lock);
            }
            jb_str (b, "skip_hidden_latency", ls->str);
            g_string_free (ls, TRUE);
        }
        jb_bool (b, "fx_cache", D.cfg->fx_cache);
        {
            /* effect pass programs (fx_specialize.h): the two settings, and
             * the specialised programs since the daemon started that drew
             * what the generic program draws (in use) / did not (the pass
             * keeps the generic program) */
            int fv = 0, fr = 0;
            fx_verify_stats (&fv, &fr);
            jb_bool (b, "fx_specialize", D.cfg->fx_specialize);
            jb_str (b, "fx_quality", D.cfg->fx_quality ? D.cfg->fx_quality : "exact");
            jb_int (b, "fx_verified", fv);
            jb_int (b, "fx_rejected", fr);
        }
        jb_bool (b, "video_hwdec", D.cfg->video_hwdec);
        jb_bool (b, "video_limit", D.cfg->video_limit);
        jb_str (b, "video_upload", D.cfg->video_upload ? D.cfg->video_upload : "legacy");
        {
            /* the path the video module really took (a path the GL context
             * cannot do falls back; WALPE_VIDEO_UPLOAD overrides the setting) */
            char *vu = walpe_video_upload_note ();
            if (vu)
                jb_str (b, "video_upload_now", vu);
            g_free (vu);
        }
        {
            /* what the decoder opened last really does (hardware decoding
             * falls back to software on its own) */
            char *vd = walpe_video_decoder_note ();
            if (vd)
                jb_str (b, "video_decoder", vd);
            g_free (vd);
        }
        jb_str (b, "particle_lowres", walpe_particle_lowres_name (D.cfg->particle_lowres));
        jb_int (b, "particle_density", D.cfg->particle_density);
        jb_bool (b, "particle_instancing", D.cfg->particle_instancing);
        char *ms = walpe_modules_status ();
        jb_str (b, "modules", ms);
        g_free (ms);
    }
    {
        /* "Render on CPU": the setting, what really renders, and why not
         * when the setting could not be honoured */
        g_mutex_lock (&Sw.lock);
        /* the saved setting (or WALPE_SOFTWARE while it is set): a change
         * shows here at once, in software_active after the restart */
        jb_bool (b, "software_render", Sw.env_forced ? Sw.wanted : D.cfg->software_render);
        jb_bool (b, "software_env", Sw.env_forced);
        jb_bool (b, "software_active", Sw.gl_renderer
                                           ? walpe_sw_renderer_is_software (Sw.gl_renderer)
                                           : Sw.active);
        jb_int (b, "software_threads", Sw.active ? Sw.threads : 0);
        jb_str (b, "software_note", Sw.note);
        jb_str (b, "gl_vendor", Sw.gl_vendor);
        jb_str (b, "gl_renderer", Sw.gl_renderer);
        jb_str (b, "gl_version", Sw.gl_version);
        g_mutex_unlock (&Sw.lock);
    }
    jb_int (b, "fps", D.cfg->fps);
    jb_int (b, "render_scale", g_atomic_int_get (&D.cfg->render_scale));
    /* bumped by every settings save: a subscriber (tray, GUI) sees that
     * config.json changed even when nothing else in the status did */
    jb_int (b, "config_serial", walpe_config_save_count ());
    /* quality knobs: the preset in use and the frame rate limit that
     * results ({"cmd":"quality"} has the rest) */
    jb_str (b, "quality_preset", walpe_preset_name (walpe_quality_preset_of (&D.cfg->quality)));
    jb_int (b, "fps_now", m0 ? m0->fps : D.cfg->fps);
    json_builder_set_member_name (b, "volume");
    json_builder_add_double_value (b, D.cfg->volume);
    jb_str (b, "backend", D.cfg->video_backend);
    /* per monitor (version 0.11.1): index (for open/props "monitor"), output
     * name (config key), geometry, the wallpaper it shows, own pause */
    json_builder_set_member_name (b, "monitors");
    json_builder_begin_array (b);
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        json_builder_begin_object (b);
        jb_int (b, "index", m->index);
        jb_str (b, "name", m->name);
        json_builder_set_member_name (b, "geometry");
        json_builder_begin_array (b);
        json_builder_add_int_value (b, m->geom.x);
        json_builder_add_int_value (b, m->geom.y);
        json_builder_add_int_value (b, m->geom.width);
        json_builder_add_int_value (b, m->geom.height);
        json_builder_end_array (b);
        jb_str (b, "current", m->want_path ? m->want_path
                                           : walpe_config_monitor_path (D.cfg, m->name));
        jb_bool (b, "paused", m->own_paused);
        if (D.vis)
            jb_int (b, "visible_percent", vis_percent (m));   /* "Skip hidden areas" */
        jb_bool (b, "covered", m->covered);
        jb_bool (b, "focused_app", m->focus_app);      /* an application on it has the focus */
        jb_int (b, "fps_limit", m->fps_cap);            /* what its render thread was given */
        json_builder_set_member_name (b, "refresh");   /* Hz, 0 = unknown */
        json_builder_add_double_value (b, m->hz);
        jb_str (b, "type", walpe_type_name (g_atomic_int_get (&m->content_type)));
        if (m->loading || (m->leader && m->leader->loading))
            /* the wallpaper it was given is still being loaded (by its own
             * thread, or by the monitor it follows): "type" is what the
             * window shows until then */
            jb_bool (b, "loading", TRUE);
        if (m->load_error)   /* the wallpaper it was given did not load */
            jb_str (b, "load_error", m->load_error);
        if (m->load_error && m->restored)   /* and the one it showed before is back */
            jb_str (b, "load_restored", m->restored);
        if (m->leader)
            jb_int (b, "follows", m->leader->index);
        /* presentation: its mode and the swap interval in effect */
        jb_str (b, "present", walpe_present_mode_name (m->present_mode));
        jb_int (b, "swap_interval", g_atomic_int_get (&m->present_iv));
        /* counters since start: frames this monitor presented, and
         * how often its render thread woke up (two status calls a few
         * seconds apart give both per second) */
        jb_int (b, "frames", (guint) g_atomic_int_get (&m->render_count));
        jb_int (b, "wakeups", (guint) g_atomic_int_get (&m->wakeups));
        if (g_atomic_int_get (&m->idle_released))
            jb_bool (b, "idle_released", TRUE);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    jb_int (b, "monitor_count", g_list_length (D.mons));
    jb_bool (b, "compositor", present_composited ());   /* asked now: one X request */
    jb_bool (b, "frame_align", walpe_frame_align_get ());
    /* requests the main thread has sent to the X server since
     * start (its own connection; this status call adds a few) */
    jb_int (b, "x_requests", (gint64) NextRequest (D.xd) - 1);
    /* props/setprop/setprops/resetprops take an optional "path" (any
     * wallpaper, shown or not) besides "monitor" */
    jb_bool (b, "props_by_path", TRUE);
    /* "layers" / "layeradj" requests exist ("Adjust layers") */
    jb_bool (b, "layer_adjust", TRUE);
    jb_str (b, "gl", g_atomic_int_get (&D.egl_is_es) ? "gles" : "gl");
#ifdef HAVE_WEB
    char *mm = walpe_web_media_missing ();
    if (*mm)
        jb_str (b, "web_missing_gst", mm);
    g_free (mm);
    char *we = walpe_web_error ();
    if (*we)
        jb_str (b, "web_error", we);
    g_free (we);
    /* how pointer input reaches web pages: "native" = real input events */
    char *wi = walpe_web_input_status ();
    if (*wi)
        jb_str (b, "web_input", wi);
    g_free (wi);
    /* how pages are rendered, and what WebGL runs on inside them */
    jb_bool (b, "web_accel", D.cfg->web_accel);
    if (web_render_state)
        jb_str (b, "web_render", web_render_state);
    char *wg = walpe_web_info ();
    if (*wg)
        jb_str (b, "web_webgl", wg);
    g_free (wg);
#endif
    if (load_skip && g_hash_table_size (load_skip)) {
        /* crash-loop guard: {"<output>": "<path not loaded>"} */
        json_builder_set_member_name (b, "load_skipped");
        json_builder_begin_object (b);
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init (&it, load_skip);
        while (g_hash_table_iter_next (&it, &k, &v))
            jb_str (b, k, v);
        json_builder_end_object (b);
    }
    if (Ws.job) {
        jb_bool (b, "install_active", Ws.active);
        jb_str (b, "install_id", Ws.id);
        jb_str (b, "install_phase", walpe_workshop_phase (Ws.job));
        json_builder_set_member_name (b, "install_progress");   /* 0..1, -1 unknown */
        json_builder_add_double_value (b, walpe_workshop_progress (Ws.job));
        if (Ws.error)
            jb_str (b, "install_error", Ws.error);
        if (Ws.dir)
            jb_str (b, "install_dir", Ws.dir);
        if (Ws.title)
            jb_str (b, "install_title", Ws.title);
    }
    if (D.cfg->broken_copy)   /* config.json was unreadable at start-up: defaults in use */
        jb_str (b, "config_broken_copy", D.cfg->broken_copy);
#if defined (__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    {
        /* WALPE_MEM_STATS=1: the allocator's own numbers next to what the
         * system reports, to tell live heap from memory the allocator or a
         * GL driver holds (`walpe-ctl status`). Off by default: status is
         * polled every 2 s and mallinfo2 walks every arena. */
        static int mem_stats = -1;
        if (mem_stats < 0)
            mem_stats = g_getenv ("WALPE_MEM_STATS") != NULL;
        if (mem_stats) {
            struct mallinfo2 mi = mallinfo2 ();
            gint64 rss = 0, anon = 0, file = 0, shm = 0, hwm = 0;
            char *st = NULL;
            if (g_file_get_contents ("/proc/self/status", &st, NULL, NULL)) {
                static const struct { const char *key; int idx; } keys[] = {
                    { "VmRSS:", 0 }, { "RssAnon:", 1 }, { "RssFile:", 2 }, { "RssShmem:", 3 },
                    { "VmHWM:", 4 },
                };
                gint64 *out[] = { &rss, &anon, &file, &shm, &hwm };
                for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
                    const char *p = strstr (st, keys[i].key);
                    if (p)
                        *out[keys[i].idx] = g_ascii_strtoll (p + strlen (keys[i].key), NULL, 10);
                }
                g_free (st);
            }
            json_builder_set_member_name (b, "memory_kb");
            json_builder_begin_object (b);
            jb_int (b, "rss", rss);
            jb_int (b, "rss_anon", anon);
            jb_int (b, "rss_file", file);
            jb_int (b, "rss_shmem", shm);
            jb_int (b, "rss_peak", hwm);
            /* malloc: bytes handed out (heap + mmapped blocks), bytes the
             * heaps hold in total, free bytes kept in the heaps */
            jb_int (b, "heap_in_use", (gint64) ((mi.uordblks + mi.hblkhd) / 1024));
            jb_int (b, "heap_from_system", (gint64) ((mi.arena + mi.hblkhd) / 1024));
            jb_int (b, "heap_free_kept", (gint64) (mi.fordblks / 1024));
            jb_int (b, "heap_mmapped_blocks", (gint64) (mi.hblkhd / 1024));
            int thr = 0;
            GDir *td = g_dir_open ("/proc/self/task", 0, NULL);
            while (td && g_dir_read_name (td))
                thr++;
            if (td)
                g_dir_close (td);
            jb_int (b, "threads", thr);
            json_builder_end_object (b);
        }
    }
#endif
    jb_str (b, "steam_user", D.cfg->steam_user ? D.cfg->steam_user : "");
    {
        const char *wa = walpe_we_assets_dir ();
        jb_str (b, "we_assets", wa ? wa : "");
        jb_str (b, "we_assets_source", walpe_we_assets_source ());
    }
    char *au = walpe_audio_status ();
    jb_str (b, "audio", au);
    char *me = walpe_media_status ();
    jb_str (b, "media", me);
    g_free (me);
    g_free (au);
    if (Imp.job && Imp.active)
        Imp.percent = walpe_import_percent (Imp.job);
    jb_bool (b, "import_active", Imp.active);
    if (Imp.job)    /* "add" = library import, "convert" = a video wallpaper being made playable */
        jb_str (b, "import_purpose", Imp.purpose ? Imp.purpose : "add");
    if (Conv.wallpaper)     /* the wallpaper shown once the conversion is done */
        jb_str (b, "convert_for", Conv.wallpaper);
    jb_int (b, "import_percent", Imp.percent);
    jb_str (b, "import_file", Imp.label);
    jb_str (b, "import_mode", Imp.job ? (Imp.active ? walpe_import_mode (Imp.job) : Imp.mode) : "");
    if (Imp.error)
        jb_str (b, "import_error", Imp.error);
    json_builder_end_object (b);
}

/* {"cmd":"quality"}: the knobs, the presets, each wallpaper's own
 * settings, the desktop's state, and per monitor what its wallpaper runs
 * with, what a frame costs and what each preset would save (the numbers
 * arrive a moment after the first request: the renderer only counts while
 * somebody asks). {"describe": true} adds how the knobs apply to each
 * effect of the scenes on screen (from the next request on). */
static void ipc_quality (JsonBuilder *b, JsonObject *o)
{
    gboolean describe = wpj_bool (o, "describe", FALSE);
    jb_bool (b, "ok", TRUE);
    json_builder_set_member_name (b, "quality");
    json_builder_begin_object (b);
    walpe_quality_ipc_describe (D.cfg, b);
    jb_str (b, "picture_note", walpe_quality_picture_note ());
    jb_int (b, "fps_cap", D.cfg->fps);
    json_builder_set_member_name (b, "monitors");
    json_builder_begin_array (b);
    gint64 until = g_get_monotonic_time () + 12 * G_USEC_PER_SEC;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        Mon *src = m->leader ? m->leader : m;
        const char *path = mon_quality_path (m);
        WalpeQuality q;
        walpe_config_quality_for (D.cfg, path, &q);
        g_mutex_lock (&src->lock);
        WalpeQualityStats st = src->qstats;
        gboolean st_ok = src->qstats_ok;
        char *desc = describe && src->qdescribe ? g_strdup (src->qdescribe) : NULL;
        gboolean wake = src->stats_until_us < g_get_monotonic_time () || describe;
        src->stats_until_us = until;
        if (!st_ok) {
            /* no numbers yet. The thread hands them over when it next
             * passes a frame - which a scene that stands still never does
             * (it was woken, found nothing to do and slept on: asked later
             * than a few seconds after its load, `walpe-ctl quality` and
             * the window's panel never got any). One pass now. */
            src->stats_req = TRUE;
            wake = TRUE;
        }
        if (describe)
            src->describe_req = TRUE;
        if (wake)
            g_cond_signal (&src->cond);
        int fps = src->fps;
        g_mutex_unlock (&src->lock);
        json_builder_begin_object (b);
        jb_int (b, "index", m->index);
        jb_str (b, "name", m->name);
        jb_str (b, "path", path);
        if (m->leader)
            jb_int (b, "follows", m->leader->index);
        jb_str (b, "type", walpe_type_name (g_atomic_int_get (&src->content_type)));
        jb_bool (b, "own_settings", walpe_config_quality_ov (D.cfg, path) != NULL);
        walpe_quality_to_json (b, &q, WALPE_Q_ALL, TRUE);
        WalpeFpsState fst;
        mon_fps_state (src, &fst);
        jb_str (b, "fps_state", walpe_fps_state_name (&fst));
        jb_int (b, "fps_now", walpe_paced_fps (fps, D.no_refresh_cap ? 0.0 : src->hz));
        jb_bool (b, "render_scale_applies",
                 g_atomic_int_get (&src->content_type) == WALPE_TYPE_SCENE);
        if (st_ok && !m->leader) {
            json_builder_set_member_name (b, "stats");
            json_builder_begin_object (b);
            jb_int (b, "width", st.w);
            jb_int (b, "height", st.h);
            jb_bool (b, "measured", st.measured);
            jb_int (b, "px_base", (gint64) st.px_base);
            jb_int (b, "px_fx", (gint64) st.px_fx);
            jb_int (b, "px_fx_now", (gint64) st.px_now);
            json_builder_set_member_name (b, "passes");
            json_builder_add_double_value (b, st.passes);
            json_builder_set_member_name (b, "passes_now");
            json_builder_add_double_value (b, st.passes_now);
            jb_int (b, "px_hold_slow2", (gint64) st.px_hold[0]);
            jb_int (b, "px_hold_slow3", (gint64) st.px_hold[1]);
            jb_int (b, "px_hold_all2", (gint64) st.px_hold[2]);
            jb_int (b, "px_hold_all3", (gint64) st.px_hold[3]);
            jb_int (b, "px_fxres_screen", (gint64) st.px_fxres[0]);
            jb_int (b, "px_fxres_reduced", (gint64) st.px_fxres[1]);
            jb_int (b, "px_fxres_half", (gint64) st.px_fxres[2]);
            jb_int (b, "px_post_reduced", (gint64) st.px_post_red);
            jb_int (b, "px_post_off", (gint64) st.px_post_off);
            jb_int (b, "target_bytes", (gint64) st.target_bytes);
            json_builder_end_object (b);
            /* pixels shaded per frame and per second (at the frame rate
             * of the monitor's state now) with each preset and with the
             * settings in use; "saving" = 1 - that / High, per second */
            double hz = D.no_refresh_cap ? 0.0 : src->hz;
            WalpeQuality hi;
            walpe_quality_preset (WALPE_PRESET_HIGH, &hi);
            const double h = walpe_quality_estimate (&st, &hi, &fst, D.cfg->fps, hz);
            const double hf = walpe_quality_estimate_frame (&st, &hi);
            json_builder_set_member_name (b, "estimate");
            json_builder_begin_object (b);
            for (int p = WALPE_PRESET_MAX; p <= WALPE_PRESET_CUSTOM; p++) {
                WalpeQuality v = q;
                if (p != WALPE_PRESET_CUSTOM)
                    walpe_quality_preset ((WalpePreset) p, &v);
                json_builder_set_member_name (b, p == WALPE_PRESET_CUSTOM ? "current"
                                                 : walpe_preset_name ((WalpePreset) p));
                json_builder_begin_object (b);
                jb_int (b, "px_frame", (gint64) walpe_quality_estimate_frame (&st, &v));
                json_builder_set_member_name (b, "passes_frame");
                json_builder_add_double_value (b, walpe_quality_estimate_passes (&st, &v));
                const double e = walpe_quality_estimate (&st, &v, &fst, D.cfg->fps, hz);
                const double ef = walpe_quality_estimate_frame (&st, &v);
                jb_int (b, "fps", walpe_paced_fps (walpe_quality_fps (&v, &fst, D.cfg->fps), hz));
                jb_int (b, "px_s", (gint64) e);
                json_builder_set_member_name (b, "saving_frame");
                json_builder_add_double_value (b, hf > 0 ? 1.0 - ef / hf : 0.0);
                json_builder_set_member_name (b, "saving");
                json_builder_add_double_value (b, h > 0 ? 1.0 - e / h : 0.0);
                json_builder_end_object (b);
            }
            json_builder_end_object (b);
        }
        if (desc && !m->leader)
            jb_str (b, "effects", desc);
        g_free (desc);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    json_builder_end_object (b);
}

/* ---- state pushes ("subscribe"): tray and GUI keep one connection open
 * and are told when the state changed. state_touch() is called wherever
 * it may have (pause / cover / sleep, loads, IPC commands, import and
 * install ends); the status line is built a moment later and sent only
 * when it differs from the last one. Costs nothing without subscribers. */
static guint state_push_id;
static char *state_pushed;

static char *state_line (gboolean event)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    if (event)
        jb_str (b, "event", "state");
    ipc_status (b);
    json_builder_end_object (b);
    JsonNode *rn = json_builder_get_root (b);
    JsonGenerator *g = json_generator_new ();
    json_generator_set_root (g, rn);
    char *out = json_generator_to_data (g, NULL);
    g_object_unref (g);
    json_node_unref (rn);
    g_object_unref (b);
    return out;
}

static gboolean state_push_cb (gpointer user)
{
    (void) user;
    state_push_id = 0;
    if (!walpe_ipc_subscribers ())
        return G_SOURCE_REMOVE;
    char *line = state_line (TRUE);
    if (g_strcmp0 (line, state_pushed)) {
        walpe_ipc_broadcast (line);
        g_free (state_pushed);
        state_pushed = line;
    } else {
        g_free (line);
    }
    return G_SOURCE_REMOVE;
}

static void state_touch (void)
{
    if (!state_push_id && walpe_ipc_subscribers ())
        state_push_id = g_timeout_add (60, state_push_cb, NULL);
}

/* The particle settings (config at start, walpe-ctl set particle_*) reach
 * the engine through here and nowhere else. They apply to running scenes
 * from the next frame, so every monitor is asked for one: a scene that is
 * otherwise idle repaints (the engine counts the change for a renderer
 * that skips unchanged frames: walpe_weparticles_settings_serial). One
 * line says which submission is active - the instanced one is the part of
 * the GPU work most likely to differ between drivers. */
static void particles_apply (void)
{
    walpe_weparticles_set_lowres (D.cfg->particle_lowres);
    walpe_weparticles_set_density (D.cfg->particle_density);
    walpe_weparticles_set_instanced (D.cfg->particle_instancing);
    int lowres = 0, density = 100;
    gboolean inst = TRUE;
    walpe_weparticles_settings_get (&lowres, &density, &inst);
    gboolean env = lowres != D.cfg->particle_lowres || density != D.cfg->particle_density ||
                   !inst != !D.cfg->particle_instancing;
    g_message ("particles: %s; reduced-resolution layer %s; density %d %%%s",
               inst ? "instanced quads (one record a sprite)"
                    : "six vertices a sprite (the submission before instancing)",
               walpe_particle_lowres_name (lowres), density,
               env ? " (WALPE_PT_* in the environment overrides the config)" : "");
    for (GList *l = D.mons; l; l = l->next)
        request_redraw (l->data);
}

/* The effect shader keys of a `set` request (fx_specialize, fx_quality;
 * "fx_fast" is the GUI switch's spelling of fx_quality = fast / exact):
 * config and the engine's process-wide settings. TRUE = a value changed,
 * and since effect pass programs are built when a scene loads, what is
 * shown has to load again. *given = a key was there; *err = a value that
 * is not allowed (nothing of fx_quality is changed then). */
static gboolean ipc_fx_keys (JsonObject *o, gboolean *given, const char **err)
{
    gboolean reload = FALSE;
    if (json_object_has_member (o, "fx_specialize")) {
        gboolean v = wpj_bool (o, "fx_specialize", D.cfg->fx_specialize);
        reload |= v != D.cfg->fx_specialize;
        D.cfg->fx_specialize = v;
        fx_set_specialize (v);
        *given = TRUE;
    }
    const char *q = ipc_has_str (o, "fx_quality") ? wpj_str (o, "fx_quality", "") : NULL;
    if (json_object_has_member (o, "fx_fast"))
        q = wpj_bool (o, "fx_fast", FALSE) ? "fast" : "exact";
    if (q && strcmp (q, "exact") && strcmp (q, "fast")) {
        *err = "fx_quality: exact or fast";
        return reload;
    }
    if (q) {
        reload |= g_strcmp0 (q, D.cfg->fx_quality) != 0;
        g_free (D.cfg->fx_quality);
        D.cfg->fx_quality = g_strdup (q);
        fx_set_shader_quality (q);
        *given = TRUE;
    }
    return reload;
}

static void ipc_set_keys (JsonBuilder *b, JsonObject *o);

static void ipc_set (JsonBuilder *b, JsonObject *o)
{
    ipc_set_keys (b, o);
    /* one load per scene for everything this request changed, also when it
     * ended early at a key that was not understood */
    reload_flush ();
}

static void ipc_set_keys (JsonBuilder *b, JsonObject *o)
{
    gboolean changed = FALSE;
    {
        /* a member this build does not know (a typo, a newer client) or a
         * value of the wrong type for its key: refused as a whole, before
         * anything is changed (common/setkeys.h; it used to be dropped
         * without a word while the rest was applied) */
        char *bad = walpe_set_check (o);
        if (bad) {
            jb_fail (b, bad);
            g_free (bad);
            return;
        }
    }
    {
        /* quality knobs (quality_preset, fx_rate, fx_rate_mode, fx_res,
         * fps_idle, post_processing, render_scale, tex_res, texture_slack):
         * checked first, so a value that is not allowed changes nothing */
        guint given = 0, diff = 0;
        char *qerr = NULL;
        if (!walpe_quality_ipc_set (D.cfg, o, &given, &diff, &qerr)) {
            jb_fail (b, qerr ? qerr : "quality setting not allowed");
            g_free (qerr);
            return;
        }
        if (given) {
            changed = TRUE;
            /* a preset also sets what other parts offer (particles, effect
             * shaders, tex_downscale: the rows of preset_extras[] in
             * quality.c): as members of this request, taken by the
             * branches below where this build has them, ignored where it
             * has not. This block has to stay in front of those branches. */
            WalpePreset pp;
            const char *pn = wpj_str (o, "quality_preset", NULL);
            if (!pn)
                pn = wpj_str (o, "preset", NULL);
            gboolean fx_keys = FALSE;
            if (pn && walpe_preset_parse (pn, &pp)) {
                walpe_preset_extras_into (pp, o);
                /* tex_downscale is read by the render thread when a scene
                 * loads (mon_do_load_ex). A preset that changes it reloads
                 * the scenes for it: Maximum is the exact picture now, not
                 * from the next wallpaper on - also when nothing else of
                 * the preset asks for a reload (the earlier build: when the texture slack
                 * was 0 already, the scenes kept their reduced layer
                 * pictures). `set tex_downscale=` alone stays "next load". */
                if (json_object_has_member (o, "tex_downscale")) {
                    gboolean was = g_atomic_int_get (&D.cfg->tex_downscale) != 0;
                    gboolean now = wpj_bool (o, "tex_downscale", was);
                    g_atomic_int_set (&D.cfg->tex_downscale, now);
                    if (was != now)
                        scenes_reload_mark (NULL);
                }
                /* The effect shader keys likewise (ipc_fx_keys): a scene's pass
                 * programs are built from them when it loads, so they are in
                 * place before the monitors are marked; the block below finds
                 * them unchanged. */
                const char *fx_err = NULL;
                gboolean fx_given = FALSE;
                fx_keys = ipc_fx_keys (o, &fx_given, &fx_err);
            }
            quality_apply (diff, NULL, fx_keys);   /* (marks; ipc_set flushes) */
        }
    }
    /* presentation, for an A/B on the running wallpaper: `walpe-ctl set
     * present=limiter` (vsync | limiter | auto, NAME=mode per monitor)
     * applies at the next swap and is not saved - gpu.conf holds the
     * lasting value */
    gboolean live = FALSE;
    if (ipc_has_str (o, "present")) {
        const char *spec = wpj_str (o, "present", "");
        char *bad = NULL;
        walpe_present_spec_mode (spec, NULL, -1, &bad);   /* checked before anything changes */
        if (bad) {
            char *msg = g_strdup_printf ("present: \"%s\" is not understood (vsync, limiter, auto, "
                                         "NAME=mode)", bad);
            jb_fail (b, msg);
            g_free (msg);
            g_free (bad);
            return;
        }
        g_free (present_apply_spec (spec));
        live = TRUE;
    }
    /* `walpe-ctl set frame_align=on|off`: for an A/B on the running
     * wallpapers, not saved (gpu.conf holds the lasting value) */
    if (json_object_has_member (o, "frame_align")) {
        gboolean on = wpj_bool (o, "frame_align", FALSE);
        if (on != walpe_frame_align_get ()) {
            walpe_frame_align_set (on);
            g_message ("present: frame clocks %s (frame_align)", on ? "aligned" : "free-running");
        }
        live = TRUE;
    }
    gboolean restart = FALSE;
    if (json_object_has_member (o, "software_render") ||
        json_object_has_member (o, "software_threads")) {
        /* "Render on CPU" and its thread count: saved, and the daemon
         * restarts in place when the running renderer has to change
         * (libglvnd picks the GL implementation once per process) */
        gboolean sr = wpj_bool (o, "software_render", D.cfg->software_render);
        int st = D.cfg->software_threads;
        if (json_object_has_member (o, "software_threads")) {
            JsonNode *n = json_object_get_member (o, "software_threads");
            double v = st;
            if (JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING) {
                const char *t = json_node_get_string (n);   /* walpe-ctl sends text */
                v = !g_strcmp0 (t, "auto") ? 0.0 : g_ascii_strtod (t ? t : "", NULL);
            } else {
                v = wpj_num (o, "software_threads", st);
            }
            /* "nan" is a number to g_ascii_strtod and passes CLAMP as it
             * is: the cast was undefined (-2147483648 went into the config) */
            st = v >= 0.0 ? (int) MIN (v, 32.0) : 0;
        }
        gboolean need = (!Sw.env_forced && sr != Sw.wanted) ||
                        (Sw.active && st != D.cfg->software_threads &&
                         !g_getenv ("LP_NUM_THREADS"));
        if (need && ((Imp.job && Imp.active) || (Ws.job && Ws.active))) {
            jb_fail (b, "software_render: the engine has to restart for this and an import / "
                        "Workshop download is running; change it when that has finished");
            return;
        }
        D.cfg->software_render = sr;
        D.cfg->software_threads = st;
        changed = TRUE;
        restart = need;
        /* switched back before the restart it had asked for came due (the
         * switch flipped twice): what runs is what is wanted again. the earlier build
         * restarted all the same, for nothing - every wallpaper loaded
         * anew. (Asked of the running process, not of `need`, which
         * compares with the saved values of a moment ago.) */
        gboolean differs = (!Sw.env_forced && sr != Sw.wanted) ||
                           (Sw.active && !g_getenv ("LP_NUM_THREADS") &&
                            walpe_sw_threads (st, (int) g_get_num_processors ()) != Sw.threads);
        if (!differs && Sw.restart_id) {
            g_source_remove (Sw.restart_id);
            Sw.restart_id = 0;
            g_message ("software rendering setting is back to what runs: no restart");
        }
    }
    if (ipc_has_num (o, "fps")) {
        D.cfg->fps = (int) CLAMP (wpj_num (o, "fps", D.cfg->fps), 5.0, 200.0);
        fps_refresh ();
        if (ptr_mode == PTR_TRACK)   /* the pointer tracking rate follows it */
            pointer_set_mode (PTR_TRACK);
        changed = TRUE;
    }
    if (ipc_has_num (o, "volume")) {
        /* persisted only: the renderer has no audio output path */
        D.cfg->volume = CLAMP (wpj_num (o, "volume", D.cfg->volume), 0.0, 1.0);
        changed = TRUE;
    }
    if (json_object_has_member (o, "pause_fullscreen")) {
        D.cfg->pause_fullscreen = wpj_bool (o, "pause_fullscreen", D.cfg->pause_fullscreen);
        changed = TRUE;
        update_active_window ();
        apply_pause_state ();
    }
    if (json_object_has_member (o, "pause_dpms")) {
        D.cfg->pause_dpms = wpj_bool (o, "pause_dpms", D.cfg->pause_dpms);
        changed = TRUE;
        sleep_recheck ();
        apply_pause_state ();
    }
    if (json_object_has_member (o, "pause_covered")) {
        D.cfg->pause_covered = wpj_bool (o, "pause_covered", D.cfg->pause_covered);
        changed = TRUE;
        recompute_covered ();
        apply_pause_state ();
    }
    if (json_object_has_member (o, "cover_transparent")) {
        D.cfg->cover_transparent = wpj_bool (o, "cover_transparent", D.cfg->cover_transparent);
        changed = TRUE;
        cover_schedule ();
    }
    /* ---- opt-in resource rules */
    if (ipc_has_str (o, "focus_action")) {
        int fa = walpe_focus_action_parse (wpj_str (o, "focus_action", ""), -1);
        if (fa < 0) {
            if (changed)
                walpe_config_save (D.cfg);
            jb_fail (b, "focus_action: run, limit or pause");
            return;
        }
        D.cfg->focus_action = fa;
        changed = TRUE;
        apply_pause_state ();
    }
    if (ipc_has_num (o, "focus_fps")) {
        D.cfg->focus_fps = (int) CLAMP (wpj_num (o, "focus_fps", D.cfg->focus_fps), 1.0, 200.0);
        changed = TRUE;
        fps_refresh ();
    }
    if (json_object_has_member (o, "pause_battery")) {
        D.cfg->pause_battery = wpj_bool (o, "pause_battery", D.cfg->pause_battery);
        changed = TRUE;
        power_watch_update ();
    }
    if (ipc_has_num (o, "battery_fps")) {
        D.cfg->battery_fps = (int) CLAMP (wpj_num (o, "battery_fps", D.cfg->battery_fps), 0.0, 200.0);
        changed = TRUE;
        power_watch_update ();
    }
    if (json_object_has_member (o, "preview_anim")) {
        /* a GUI setting, kept here because the daemon owns config.json;
         * "off" may arrive as false (walpe-ctl turns on/off into booleans) */
        JsonNode *pn = json_object_get_member (o, "preview_anim");
        int pa = -1;
        if (JSON_NODE_HOLDS_VALUE (pn) && json_node_get_value_type (pn) == G_TYPE_BOOLEAN)
            pa = json_node_get_boolean (pn) ? WALPE_PREVIEW_ALWAYS : WALPE_PREVIEW_OFF;
        else if (ipc_has_str (o, "preview_anim"))
            pa = walpe_preview_anim_parse (wpj_str (o, "preview_anim", ""), -1);
        if (pa < 0) {
            if (changed)
                walpe_config_save (D.cfg);
            jb_fail (b, "preview_anim: always, hover or off");
            return;
        }
        D.cfg->preview_anim = pa;
        changed = TRUE;
    }
    if (json_object_has_member (o, "fx_cache")) {
        D.cfg->fx_cache = wpj_bool (o, "fx_cache", D.cfg->fx_cache);
        fx_set_cache_enabled (D.cfg->fx_cache);   /* applies from the next frame */
        changed = TRUE;
    }
#ifdef HAVE_WEB
    if (json_object_has_member (o, "web_accel")) {
        gboolean v = wpj_bool (o, "web_accel", D.cfg->web_accel);
        if (v != D.cfg->web_accel) {
            /* WebKit and GDK read their switches once per process: the
             * daemon starts again (same PID, saved wallpapers), after the
             * reply to this request went out */
            D.cfg->web_accel = v;
            changed = TRUE;
            g_timeout_add (200, web_accel_restart, NULL);
        }
    }
#endif
    if (json_object_has_member (o, "particle_lowres")) {
        /* off | auto | all (walpe-ctl sends on / off as booleans) */
        JsonNode *pn = json_object_get_member (o, "particle_lowres");
        int mode = -1;
        if (JSON_NODE_HOLDS_VALUE (pn) && json_node_get_value_type (pn) == G_TYPE_BOOLEAN)
            mode = json_node_get_boolean (pn) ? 1 : 0;
        else
            mode = walpe_particle_lowres_from_name (wpj_str (o, "particle_lowres", NULL), -1);
        if (mode < 0) {
            if (changed)    /* the keys before this one were applied: keep them */
                walpe_config_save (D.cfg);
            jb_fail (b, "particle_lowres: off, auto or all");
            return;
        }
        D.cfg->particle_lowres = mode;
        particles_apply ();   /* applies from the next frame */
        changed = TRUE;
    }
    if (json_object_has_member (o, "particle_density")) {
        /* a number or a numeric string, percent */
        double pd = wpj_num (o, "particle_density", -1.0);
        const char *ps = wpj_str (o, "particle_density", NULL);
        if (pd < 0 && ps && *ps)
            pd = g_ascii_strtod (ps, NULL);
        if (!(pd >= 5.0 && pd <= 100.0)) {
            if (changed)
                walpe_config_save (D.cfg);
            jb_fail (b, "particle_density: a percentage from 5 to 100");
            return;
        }
        D.cfg->particle_density = (int) pd;
        particles_apply ();   /* running systems thin out */
        changed = TRUE;
    }
    if (json_object_has_member (o, "particle_instancing")) {
        D.cfg->particle_instancing = wpj_bool (o, "particle_instancing", D.cfg->particle_instancing);
        particles_apply ();   /* the next draw of every system */
        changed = TRUE;
    }
    if (json_object_has_member (o, "tex_downscale")) {
        g_atomic_int_set (&D.cfg->tex_downscale, wpj_bool (o, "tex_downscale", D.cfg->tex_downscale));
        changed = TRUE;   /* next wallpaper load */
    }
    if (json_object_has_member (o, "texture_slack")) {
        /* 0 / off = exact sizes, anything else = the 2 % tolerance
         * (walpe-ctl set texture_slack=0|2|on|off) */
        JsonNode *tn = json_object_get_member (o, "texture_slack");
        gboolean on = TRUE;
        if (JSON_NODE_HOLDS_VALUE (tn)) {
            GType vt = json_node_get_value_type (tn);
            if (vt == G_TYPE_BOOLEAN)
                on = json_node_get_boolean (tn);
            else if (vt == G_TYPE_STRING)
                on = g_ascii_strtod (json_node_get_string (tn), NULL) > 0;
            else
                on = json_node_get_double (tn) > 0;
        }
        g_atomic_int_set (&D.cfg->texture_slack, on ? 2 : 0);
        changed = TRUE;   /* next wallpaper load */
    }
    if (ipc_has_num (o, "render_scale")) {
        /* scenes: percent of the monitor's resolution they are rendered at;
         * running scenes take it with their next frame (asked for here) */
        g_atomic_int_set (&D.cfg->render_scale,
                          (int) CLAMP (wpj_num (o, "render_scale", D.cfg->render_scale), 25.0, 100.0));
        for (GList *l = D.mons; l; l = l->next)
            request_redraw (l->data);
        changed = TRUE;
    }
    if (json_object_has_member (o, "video_hwdec")) {
        D.cfg->video_hwdec = wpj_bool (o, "video_hwdec", D.cfg->video_hwdec);
        walpe_video_set_hwdec (D.cfg->video_hwdec);   /* the next video that is opened */
        changed = TRUE;
    }
    if (ipc_has_str (o, "video_upload")) {
        /* videos that are playing change over with their next frame */
        const char *u = wpj_str (o, "video_upload", "");
        if (!walpe_video_set_upload (u)) {
            walpe_video_set_upload (D.cfg->video_upload);
            if (changed)
                walpe_config_save (D.cfg);
            jb_fail (b, "video_upload: legacy, pbo, pbodata, persist or direct, each also with "
                        "+pingpong");
            return;
        }
        g_free (D.cfg->video_upload);
        D.cfg->video_upload = g_strdup (u);
        for (GList *l = D.mons; l; l = l->next)
            request_redraw (l->data);
        changed = TRUE;
    }
    if (json_object_has_member (o, "video_limit")) {
        /* videos that are playing follow it with their next picture */
        D.cfg->video_limit = wpj_bool (o, "video_limit", D.cfg->video_limit);
        walpe_video_set_limit (D.cfg->video_limit);
        for (GList *l = D.mons; l; l = l->next)
            request_redraw (l->data);
        changed = TRUE;
    }
    {
        /* effect pass programs are built when a scene loads: a change
         * reloads what is shown (ipc_fx_keys; a preset's values were
         * taken at the top of this function, with its one reload) */
        const char *fx_err = NULL;
        gboolean fx_reload = ipc_fx_keys (o, &changed, &fx_err);
        if (fx_reload)
            scenes_reload_mark (NULL);   /* (ipc_set flushes) */
        if (fx_err) {
            if (changed)
                walpe_config_save (D.cfg);
            jb_fail (b, fx_err);
            return;
        }
    }
    if (json_object_has_member (o, "skip_hidden") || json_object_has_member (o, "skip_hidden_margin") ||
        json_object_has_member (o, "skip_hidden_inset")) {
        /* "Skip hidden areas": "off" / "auto" / "always" (also false / true = auto, 0..2) */
        JsonNode *sn = json_object_get_member (o, "skip_hidden");
        if (sn && JSON_NODE_HOLDS_VALUE (sn)) {
            GType st = json_node_get_value_type (sn);
            if (st == G_TYPE_STRING)
                D.cfg->skip_hidden = walpe_skip_hidden_parse (json_node_get_string (sn),
                                                              D.cfg->skip_hidden);
            else if (st == G_TYPE_BOOLEAN)
                D.cfg->skip_hidden = json_node_get_boolean (sn) ? WALPE_SKIP_AUTO : WALPE_SKIP_OFF;
            else if (st == G_TYPE_INT64)
                D.cfg->skip_hidden = (int) CLAMP (json_node_get_int (sn), 0, 2);
        }
        static const char *const keys[2] = { "skip_hidden_margin", "skip_hidden_inset" };
        for (int k = 0; k < 2; k++) {
            JsonNode *vn = json_object_get_member (o, keys[k]);
            if (!vn || !JSON_NODE_HOLDS_VALUE (vn))
                continue;
            /* walpe-ctl sends every value but fps / volume as a string */
            double v = ipc_has_num (o, keys[k]) ? wpj_num (o, keys[k], 0)
                     : ipc_has_str (o, keys[k]) ? g_ascii_strtod (json_node_get_string (vn), NULL)
                     : -1.0;
            if (!(v >= 0.0))
                continue;
            if (k == 0)
                D.cfg->skip_hidden_margin = (int) MIN (v, 512.0);
            else
                D.cfg->skip_hidden_inset = (int) MIN (v, 256.0);
        }
        changed = TRUE;
        vis_apply_setting ();
    }
    if (json_object_has_member (o, "render_shared")) {
        D.cfg->render_shared = wpj_bool (o, "render_shared", D.cfg->render_shared);
        changed = TRUE;
        noshare_reset ();
        apply_wallpapers (NULL, NULL, FALSE, TRUE);   /* regroup now */
    }
    if (json_object_has_member (o, "pointer_share")) {
        g_atomic_int_set (&D.cfg->pointer_share,
                          wpj_bool (o, "pointer_share", D.cfg->pointer_share));
        changed = TRUE;
        noshare_reset ();
        apply_wallpapers (NULL, NULL, FALSE, TRUE);   /* regroup: leaders check again */
        pointer_update ();
    }
    if (ipc_has_str (o, "transparent_windows")) {
        const char *tw = wpj_str (o, "transparent_windows", "");
        g_free (D.cfg->transparent_windows);
        D.cfg->transparent_windows = g_strdup (tw);
        changed = TRUE;
        cover_schedule (); /* re-evaluate coverage with the new list */
        if (D.vis)
            walpe_vistrack_set_see_through (D.vis, tw);
    }
    if (ipc_has_str (o, "steam_user")) {
        const char *su = wpj_str (o, "steam_user", "");
        if (*su && !walpe_workshop_valid_user (su)) {
            if (changed)    /* the keys before this one were applied: keep them */
                walpe_config_save (D.cfg);
            jb_fail (b, "steam_user: Steam account names are letters, digits and _");
            return;
        }
        g_free (D.cfg->steam_user);
        D.cfg->steam_user = *su ? g_strdup (su) : NULL;
        changed = TRUE;
    }
    if (ipc_has_str (o, "we_assets")) {
        /* "" = auto-detect; a dir must be a WE assets dir or install root */
        const char *wa = wpj_str (o, "we_assets", "");
        char *chk = *wa ? walpe_we_assets_check (wa) : NULL;
        if (*wa && !chk) {
            if (changed)
                walpe_config_save (D.cfg);
            jb_fail (b, "we_assets: not a Wallpaper Engine assets dir (expected shaders/, "
                        "effects/ or materials/ in it, or an install root with assets/)");
            return;
        }
        g_free (D.cfg->we_assets);
        D.cfg->we_assets = chk;
        walpe_we_assets_set_configured (chk);
        changed = TRUE; /* applies to the next wallpaper load */
    }
    if (json_object_has_member (o, "parallax")) {
        gboolean was = D.cfg->parallax;
        g_atomic_int_set (&D.cfg->parallax, wpj_bool (o, "parallax", D.cfg->parallax));
        pointer_update ();
        changed = TRUE;
        /* sharing depends on it (pointer-following scenes): regroup, the
         * leaders check their scene again */
        if (was != D.cfg->parallax && shared_render_possible ()) {
            noshare_reset ();
            apply_wallpapers (NULL, NULL, FALSE, TRUE);
        }
    }
    if (ipc_has_str (o, "video_backend")) {
        const char *bk = wpj_str (o, "video_backend", "ffmpeg");
        if (!g_strcmp0 (bk, "mpv") || !g_strcmp0 (bk, "ffmpeg")) {
            /* stored for old configs / GUIs only: FFmpeg is the one video
             * backend (mpv was removed), so nothing reloads for it */
            g_free (D.cfg->video_backend);
            D.cfg->video_backend = g_strdup (bk);
            changed = TRUE;
        }
    }
    if (changed) {
        walpe_config_save (D.cfg);
        jb_bool (b, "ok", TRUE);
        if (restart) {
            jb_bool (b, "restarting", TRUE);   /* after this reply is out */
            sw_schedule_restart ();
        } else if (Sw.env_forced && json_object_has_member (o, "software_render")) {
            jb_str (b, "note", "saved; WALPE_SOFTWARE in the daemon's environment decides "
                               "while it is set");
        }
    } else if (live) {
        jb_bool (b, "ok", TRUE);
    } else {
        jb_fail (b, "nothing to set");
    }
}

/* {"cmd":"forget","path":P}: P is about to be deleted (GUI "Delete"). No
 * monitor keeps showing it (one with its own wallpaper follows the default
 * again; when P is the default, the monitors following it show nothing)
 * and nothing saved points at it any more: `current`, monitor_current, the
 * saved options, the crash-loop skip list. The files are not touched here. */
static void ipc_forget (JsonBuilder *b, JsonObject *o)
{
    const char *path = wpj_str (o, "path", NULL);
    if (!path || !*path) {
        jb_fail (b, "missing path");
        return;
    }
    char *abs = walpe_expand_path (path);
    guint shown = 0;
    for (GList *l = D.mons; l; l = l->next)
        shown += walpe_path_same (((Mon *) l->data)->want_path, abs);
    /* nor is it a wallpaper to return to when a later load fails (load_ended) */
    for (GList *l = D.mons; l; l = l->next) {
        Mon *x = l->data;
        if (walpe_path_same (x->good_path, abs))
            g_clear_pointer (&x->good_path, g_free);
    }
    if (walpe_path_same (open_undo.current, abs)) {
        walpe_open_undo_clear (&open_undo);
    } else if (open_undo.monitor_current) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init (&it, open_undo.monitor_current);
        while (g_hash_table_iter_next (&it, &k, &v))
            if (walpe_path_same (v, abs))
                g_hash_table_iter_remove (&it);
    }
    guint n = walpe_config_forget_path (D.cfg, abs);
    GHashTable *tabs[] = { load_skip, D.noshare };
    for (guint i = 0; i < G_N_ELEMENTS (tabs); i++) {
        GHashTableIter it;
        gpointer k, v;
        if (!tabs[i])
            continue;
        g_hash_table_iter_init (&it, tabs[i]);
        while (g_hash_table_iter_next (&it, &k, &v))   /* load_skip: value, noshare: key */
            if (walpe_path_same (tabs[i] == load_skip ? v : k, abs))
                g_hash_table_iter_remove (&it);
    }
    if (n)
        walpe_config_save (D.cfg);
    if (n || shown)
        apply_wallpapers (NULL, NULL, FALSE, FALSE);
    g_message ("forget %s: %u config entr%s, shown on %u monitor(s)", abs, n,
               n == 1 ? "y" : "ies", shown);
    jb_bool (b, "ok", TRUE);
    jb_int (b, "forgotten", n);
    jb_int (b, "monitors", shown);
    g_free (abs);
}

/* make abs the wallpaper of monitor tm (NULL = of every monitor: the
 * default `current`, per-monitor choices dropped), save, load */
static void open_commit (const char *abs, Mon *tm)
{
    /* the user picks: what these monitors show from now on is no re-open
     * of a previous wallpaper (load_ended) any more, also when it is that
     * same wallpaper */
    for (GList *l = D.mons; l; l = l->next)
        if (!tm || l->data == tm)
            g_clear_pointer (&((Mon *) l->data)->reopen_for, g_free);
    if (tm) {
        if (load_skip)   /* the user picks again: retry */
            g_hash_table_remove (load_skip, tm->name);
        /* same as the default: follow it again */
        walpe_config_set_monitor_path (D.cfg, tm->name,
                                       g_strcmp0 (abs, D.cfg->current) ? abs : NULL);
        if (!D.cfg->current || !*D.cfg->current) {
            g_free (D.cfg->current);   /* first wallpaper ever: also the default */
            D.cfg->current = g_strdup (abs);
            walpe_config_set_monitor_path (D.cfg, tm->name, NULL);
        }
        walpe_config_save (D.cfg);
        apply_wallpapers (NULL, tm, FALSE, FALSE);
    } else {
        /* what this open replaces, should abs fail to load (load_ended).
         * When the default it replaces is the previous open's and no
         * monitor has shown that one, what was before that one stays. */
        gboolean shown = !(open_undo.path && !g_strcmp0 (open_undo.path, D.cfg->current));
        for (GList *l = D.mons; l && !shown; l = l->next)
            shown = !g_strcmp0 (((Mon *) l->data)->good_path, D.cfg->current);
        if (shown) {
            walpe_open_undo_take (&open_undo, D.cfg, abs);
        } else {
            char *np = g_strdup (abs);
            g_free (open_undo.path);
            open_undo.path = np;
        }
        char *own = g_strdup (abs);
        g_free (D.cfg->current);
        D.cfg->current = own;
        if (load_skip)
            g_hash_table_remove_all (load_skip);
        if (D.cfg->monitor_current)
            g_hash_table_remove_all (D.cfg->monitor_current);
        walpe_config_save (D.cfg);
        noshare_reset ();
        apply_wallpapers (own, NULL, FALSE, FALSE);
    }
}

/* GPUPROF: {"cmd":"profile","seconds":N[,"sync":true]} starts a GPU profile of every
 * monitor's render thread (gpuprof.h); the answer comes at once, the report
 * N seconds later: {"cmd":"profile_result"} says how far it is and, when
 * done, carries the text and where text and JSON were written
 * (~/.cache/walpe/gpu-profile-<time>.txt / .json) */
static gboolean prof_start (double seconds, gboolean sync, const char **why)
{
    walpe_prof_set_waker (prof_wake_all, NULL);
    walpe_prof_set_expected ((int) g_list_length (D.mons));
    gboolean ok = walpe_prof_start (seconds, share_root != EGL_NO_CONTEXT &&
                                                 !g_atomic_int_get (&D.share_failed),
                                    sync ? WALPE_PROF_SYNC : 0, why);
    if (ok)
        prof_note_pauses ();   /* monitors that are paused already */
    return ok;
}

static void ipc_profile_start (JsonBuilder *b, double seconds, gboolean sync)
{
    const char *why = NULL;
    if (!prof_start (seconds, sync, &why)) {
        jb_fail (b, why ? why : "the profile could not be started");
        return;
    }
    jb_bool (b, "ok", TRUE);
    json_builder_set_member_name (b, "seconds");
    json_builder_add_double_value (b, walpe_prof_remaining ());
}

static void ipc_profile_result (JsonBuilder *b)
{
    static const char *names[] = { "idle", "running", "collecting", "ready" };
    int st = walpe_prof_state ();
    jb_bool (b, "ok", TRUE);
    jb_str (b, "state", names[CLAMP (st, 0, 3)]);
    json_builder_set_member_name (b, "remaining");
    json_builder_add_double_value (b, walpe_prof_remaining ());
    if (st == 3) {
        char *text = walpe_prof_last_text ();
        jb_str (b, "text", text ? text : "");
        g_free (text);
        if (walpe_prof_last_text_path ())
            jb_str (b, "text_file", walpe_prof_last_text_path ());
        if (walpe_prof_last_json_path ())
            jb_str (b, "json_file", walpe_prof_last_json_path ());
    }
}

/* GPUPROF: WALPE_GPU_PROFILE=<seconds> = one profile shortly after start-up
 * (WALPE_GPU_PROFILE_DELAY seconds, default 5: the wallpapers load first) */
static gboolean prof_env_start (gpointer user)
{
    const char *why = NULL;
    if (!prof_start (GPOINTER_TO_INT (user) / 1000.0, FALSE, &why))
        g_warning ("WALPE_GPU_PROFILE: %s", why ? why : "not started");
    return G_SOURCE_REMOVE;
}

static char *ipc_handler (const char *req, gpointer user)
{
    (void) user;
    JsonNode *root = wpj_parse_data (req, -1);
    if (!root || json_node_get_node_type (root) != JSON_NODE_OBJECT) {
        if (root)
            json_node_unref (root);
        return g_strdup ("{\"ok\":false,\"error\":\"bad json\"}");
    }
    JsonObject *o = json_node_get_object (root);
    const char *cmd = wpj_str (o, "cmd", "");
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    gboolean quit = FALSE;

    if (!g_strcmp0 (cmd, "add")) {
        const char *path = wpj_str (o, "path", NULL);
        if (Imp.active) {
            jb_fail (b, "import already running");
        } else if (!path || !*path) {
            jb_fail (b, "missing path");
        } else {
            char *abs = walpe_expand_path (path);
            GError *ierr = start_import (abs);
            if (ierr) {
                jb_fail (b, ierr->message);
                g_error_free (ierr);
            } else {
                jb_bool (b, "ok", TRUE);
            }
            g_free (abs);
        }
    } else if (!g_strcmp0 (cmd, "open")) {
        /* {"path": P} = every monitor (the default `current`, per-monitor
         * choices dropped); {"path": P, "monitor": N} = monitor index N only
         * (persisted by its output name) */
        const char *path = wpj_str (o, "path", NULL);
        gboolean one = json_object_has_member (o, "monitor");
        Mon *tm = one ? mon_by_index (ipc_int (o, "monitor", -1)) : NULL;
        if (!path || !*path) {
            jb_fail (b, "missing path");
        } else if (one && !tm) {
            jb_fail (b, "no such monitor (see status \"monitors\")");
        } else {
            char *abs = walpe_expand_path (path);
            WalpeType t = walpe_detect_type (abs, NULL);
            char *vsrc = NULL, *vwhy = NULL;
            gboolean vsoft = FALSE, now = TRUE;
            if (t == WALPE_TYPE_NONE || t == WALPE_TYPE_UNSUPPORTED) {
                char *msg = g_strdup_printf ("unsupported content at %s", abs);
                jb_fail (b, msg);
                g_free (msg);
                now = FALSE;
            } else if (t == WALPE_TYPE_VIDEO && !video_playable (abs, &vsrc, &vwhy, &vsoft)) {
                /* a video the player cannot read (WebM, HEVC / AV1, MKV,
                 * fragmented MP4 ...) or would show sideways / stretched:
                 * converted once, shown when ready. (The load runs later
                 * on the render thread: version 0.12 answered ok, kept the old
                 * wallpaper and saved a path that gave a black desktop at
                 * the next start.) */
                GError *ce = vsoft && conv_failed && g_hash_table_contains (conv_failed, vsrc)
                    ? g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "its conversion failed before")
                    : convert_start (vsrc, abs, tm ? tm->name : NULL, TRUE);
                if (ce && vsoft) {
                    /* the file itself plays: shown as stored, like before */
                    g_message ("video: %s, not converted (%s): shown as stored",
                               vwhy ? vwhy : abs, ce->message);
                } else if (ce) {
                    char *msg = g_strdup_printf ("%s; WalPE plays H.264 video in MP4 / MOV and "
                                                 "converts other videos with ffmpeg, which did not "
                                                 "start: %s", vwhy ? vwhy : "cannot read the video",
                                                 ce->message);
                    jb_fail (b, msg);
                    g_free (msg);
                    now = FALSE;
                } else {
                    jb_bool (b, "ok", TRUE);
                    jb_bool (b, "converting", TRUE);
                    jb_str (b, "message", "converting the video to H.264 (once); the wallpaper "
                                          "changes when it is ready (status: import_percent)");
                    now = FALSE;
                }
                g_clear_error (&ce);
            }
            if (now) {
                /* a newer choice: an older one still waiting for its copy
                 * must not replace it later */
                g_clear_pointer (&Conv.wallpaper, g_free);
                g_clear_pointer (&Conv.mon, g_free);
                open_commit (abs, tm);
                jb_bool (b, "ok", TRUE);
            }
            g_free (vsrc);
            g_free (vwhy);
            g_free (abs);
        }
    } else if (!g_strcmp0 (cmd, "pause") || !g_strcmp0 (cmd, "resume")) {
        D.cfg->paused = !g_strcmp0 (cmd, "pause");
        walpe_config_save (D.cfg);
        apply_pause_state ();
        jb_bool (b, "ok", TRUE);
    } else if (!g_strcmp0 (cmd, "subscribe")) {
        /* status now, and a line {"event":"state", ...status...} on this
         * connection whenever the state changes from here on */
        if (walpe_ipc_subscribe_current ()) {
            ipc_status (b);
            jb_bool (b, "subscribed", TRUE);
            if (!state_pushed)
                state_pushed = state_line (TRUE);
        } else {
            jb_fail (b, "too many subscribers");
        }
    } else if (!g_strcmp0 (cmd, "status")) {
        ipc_status (b);
    } else if (!g_strcmp0 (cmd, "pointer")) {
        /* {"action": "click" | "down" | "up" | "move", "x", "y"}: screen
         * pixels; treated as a click on the desktop at that spot. Web
         * wallpapers also take "button" (1 left, 2 middle, 3 right),
         * "count" (2 = double click) and "action": "scroll" with "dy"/"dx"
         * wheel clicks (dy > 0 = down) */
        const char *act = wpj_str (o, "action", "click");
        int rx = ipc_int (o, "x", -1), ry = ipc_int (o, "y", -1);
        Mon *m = mon_at (rx, ry);
        if (!m) {
            jb_fail (b, "x,y is on no monitor");
#ifdef HAVE_WEB
        } else if (g_atomic_int_get (&m->content_type) == WALPE_TYPE_WEB && !mon_web_input (m)) {
            jb_fail (b, "the wallpaper there is paused");
        } else if (mon_web_input (m)) {
            int button = ipc_int (o, "button", 1), count = ipc_int (o, "count", 1);
            int dx = ipc_int (o, "dx", 0), dy = ipc_int (o, "dy", 0);
            button = CLAMP (button, 1, 3);
            count = CLAMP (count, 1, 3);
            if (!g_strcmp0 (act, "move")) {
                m->web_in = TRUE;
                m->web_lx = rx;
                m->web_ly = ry;
                web_send (m, WALPE_WEB_IN_MOVE, 0, rx, ry, 0, 0, 0);
            } else if (!g_strcmp0 (act, "scroll")) {
                for (int i = 0; i < MIN (abs (dy), 32); i++)
                    web_button (dy < 0 ? 4 : 5, TRUE, rx, ry, TRUE, 0);
                for (int i = 0; i < MIN (abs (dx), 32); i++)
                    web_button (dx < 0 ? 6 : 7, TRUE, rx, ry, TRUE, 0);
            } else {
                gboolean click = !g_strcmp0 (act, "click");
                for (int i = 0; i < (click ? count : 1); i++) {
                    if (click || !g_strcmp0 (act, "down"))
                        web_button (button, TRUE, rx, ry, TRUE, 0);
                    if (click || !g_strcmp0 (act, "up"))
                        web_button (button, FALSE, rx, ry, TRUE, 0);
                }
            }
            jb_bool (b, "ok", TRUE);
#endif
        } else if (!g_atomic_int_get (&mon_route (m)->wants_pointer)) {
            jb_fail (b, "the wallpaper there has no cursor events");
        } else {
            if (!g_strcmp0 (act, "move"))
                mon_queue_pointer (mon_route (m), WALPE_PTR_MOVE,
                                   (float) (rx - m->geom.x) / m->geom.width,
                                   (float) (ry - m->geom.y) / m->geom.height);
            if (!g_strcmp0 (act, "click") || !g_strcmp0 (act, "down"))
                pointer_button (TRUE, rx, ry, TRUE);
            if (!g_strcmp0 (act, "click") || !g_strcmp0 (act, "up"))
                pointer_button (FALSE, rx, ry, TRUE);
            jb_bool (b, "ok", TRUE);
        }
    } else if (!g_strcmp0 (cmd, "set")) {
        ipc_set (b, o);
    } else if (!g_strcmp0 (cmd, "setprop")) {
        /* Tier 4: set a user property on the CURRENT wallpaper and reload
         * it so scenes pick the value up */
        const char *key = wpj_str (o, "key", NULL);
        const char *tp = NULL;
        JsonNode *vn = json_object_get_member (o, "value");
        char *val = json_value_to_prop (vn);
        if (!key || !*key || !val) {
            jb_fail (b, "need key + scalar value");
        } else if (!(tp = ipc_target_path (o))) {
            jb_fail (b, "no active wallpaper (or no such monitor)");
        } else {
            char *tpc = g_strdup (tp);   /* apply_wallpapers may free a monitor's path */
            walpe_config_set_prop (D.cfg, tpc, key, val);
            walpe_config_save (D.cfg);
            apply_wallpapers (tpc, NULL, FALSE, FALSE);
            jb_bool (b, "ok", TRUE);
            GPtrArray *ks = g_ptr_array_new ();
            g_ptr_array_add (ks, (gpointer) key);
            ipc_props_note (b, tpc, ks);
            g_ptr_array_free (ks, TRUE);
            g_free (tpc);
        }
        g_free (val);
    } else if (!g_strcmp0 (cmd, "install")) {
        GError *ie = start_install (wpj_str (o, "id", NULL));
        if (ie) {
            jb_fail (b, ie->message);
            g_error_free (ie);
        } else {
            /* optional "size": the item's file_size, for download progress */
            double sz = wpj_num (o, "size", 0);
            walpe_workshop_set_expected_size (Ws.job, sz > 0 ? (guint64) MIN (sz, 1e18) : 0);
            jb_bool (b, "ok", TRUE);
            jb_str (b, "id", Ws.id);
        }
    } else if (!g_strcmp0 (cmd, "install_cancel")) {
        if (Ws.job && Ws.active) {
            walpe_workshop_cancel (Ws.job);
            Ws.active = FALSE;
            g_free (Ws.error);
            Ws.error = g_strdup ("cancelled");
        }
        jb_bool (b, "ok", TRUE);
    } else if (!g_strcmp0 (cmd, "import_cancel")) {
        /* stop the running import / video conversion: ffmpeg is killed,
         * its partial output removed, nothing changes on screen */
        if (Imp.job && Imp.active) {
            walpe_import_cancel (Imp.job);
            Imp.active = FALSE;
            g_free (Imp.error);
            Imp.error = g_strdup ("cancelled");
            g_clear_pointer (&Conv.wallpaper, g_free);
            g_clear_pointer (&Conv.mon, g_free);
        }
        jb_bool (b, "ok", TRUE);
    } else if (!g_strcmp0 (cmd, "forget")) {
        ipc_forget (b, o);
    } else if (!g_strcmp0 (cmd, "steam_libraries")) {
        char **libs = walpe_workshop_steam_libraries ();
        jb_bool (b, "ok", TRUE);
        json_builder_set_member_name (b, "libraries");
        json_builder_begin_array (b);
        for (int i = 0; libs[i]; i++)
            json_builder_add_string_value (b, libs[i]);
        json_builder_end_array (b);
        g_strfreev (libs);
    } else if (!g_strcmp0 (cmd, "setprops")) {
        /* GUI: several properties in one request = one reload */
        JsonObject *po = wpj_obj (o, "props");
        const char *tp = ipc_target_path (o);
        if (!tp) {
            jb_fail (b, "no active wallpaper (or no such monitor)");
        } else if (!po) {
            jb_fail (b, "need props object");
        } else {
            char *tpc = g_strdup (tp);   /* apply_wallpapers may free a monitor's path */
            GList *ks = json_object_get_members (po);
            GPtrArray *set = g_ptr_array_new ();
            guint n = 0;
            for (GList *k = ks; k; k = k->next) {
                char *val = json_value_to_prop (json_object_get_member (po, k->data));
                if (val && *(const char *) k->data) {
                    walpe_config_set_prop (D.cfg, tpc, k->data, val);
                    g_ptr_array_add (set, k->data);
                    n++;
                }
                g_free (val);
            }
            if (n) {
                walpe_config_save (D.cfg);
                apply_wallpapers (tpc, NULL, FALSE, FALSE);
            }
            jb_bool (b, "ok", TRUE);
            jb_int (b, "changed", n);
            ipc_props_note (b, tpc, set);
            g_ptr_array_free (set, TRUE);
            g_list_free (ks);
            g_free (tpc);
        }
    } else if (!g_strcmp0 (cmd, "resetprops")) {
        const char *tp = ipc_target_path (o);
        if (!tp) {
            jb_fail (b, "no active wallpaper (or no such monitor)");
        } else {
            guint n = walpe_config_clear_props (D.cfg, tp);
            if (n) {
                walpe_config_save (D.cfg);
                apply_wallpapers (tp, NULL, FALSE, FALSE);
            }
            jb_bool (b, "ok", TRUE);
            jb_int (b, "removed", n);
        }
    } else if (!g_strcmp0 (cmd, "props")) {
        ipc_props (b, ipc_target_path (o));
    } else if (!g_strcmp0 (cmd, "quality")) {
        ipc_quality (b, o);
    } else if (!g_strcmp0 (cmd, "setquality")) {
        /* one wallpaper's own knobs: {"path": P | "monitor": N, "values":
         * {key: value | null}, "reset": true}; null / "default" = follow
         * the global setting again */
        const char *tp = ipc_target_path (o);
        JsonObject *vals = wpj_obj (o, "values");
        gboolean reset = wpj_bool (o, "reset", FALSE);
        if (!tp) {
            jb_fail (b, "no active wallpaper (or no such monitor)");
        } else if (!vals && !reset) {
            jb_fail (b, "need values object (or reset)");
        } else {
            char *tpc = g_strdup (tp);   /* apply_wallpapers may free a monitor's path */
            guint diff = 0;
            char *qerr = NULL;
            if (!walpe_quality_ipc_set_wallpaper (D.cfg, tpc, vals, reset, &diff, &qerr)) {
                jb_fail (b, qerr ? qerr : "quality setting not allowed");
            } else {
                walpe_config_save (D.cfg);
                quality_apply (diff, tpc, FALSE);
                reload_flush ();
                jb_bool (b, "ok", TRUE);
                jb_int (b, "changed", diff != 0);
            }
            g_free (qerr);
            g_free (tpc);
        }
    } else if (!g_strcmp0 (cmd, "layers")) {
        ipc_layers (b, o);
    } else if (!g_strcmp0 (cmd, "layeradj")) {
        ipc_layeradj (b, o);
    } else if (!g_strcmp0 (cmd, "profile")) {
        ipc_profile_start (b, wpj_num (o, "seconds", 10.0),     /* GPUPROF */
                           wpj_bool (o, "sync", FALSE));
    } else if (!g_strcmp0 (cmd, "profile_result")) {
        ipc_profile_result (b);                                 /* GPUPROF */
    } else if (!g_strcmp0 (cmd, "quit")) {
        jb_bool (b, "ok", TRUE);
        quit = TRUE;
    } else {
        jb_fail (b, "unknown cmd");
    }

    json_builder_end_object (b);
    JsonNode *rn = json_builder_get_root (b);
    JsonGenerator *g = json_generator_new ();
    json_generator_set_root (g, rn);
    char *out = json_generator_to_data (g, NULL);
    g_object_unref (g);
    json_node_unref (rn);
    g_object_unref (b);
    /* cmd lives in root: decide before it goes */
    gboolean touch = g_strcmp0 (cmd, "status") && g_strcmp0 (cmd, "props") &&
                     g_strcmp0 (cmd, "subscribe") && g_strcmp0 (cmd, "steam_libraries");
    json_node_unref (root);
    if (quit)
        g_main_loop_quit (D.loop);
    else if (touch)
        state_touch ();     /* subscribers hear about what this changed */
    return out;
}

/* ------------------------------------------------------------- EGL init */

/* The window surface a 2D compositor needs: 8 bits per colour channel, no
 * depth buffer, no stencil buffer, no multisampling, no alpha (the desktop
 * window is opaque), and the visual the windows are created with (the root
 * window's). eglChooseConfig returns every config that has AT LEAST what
 * was asked for and the first one was taken on trust: EGL sorts deeper
 * colour formats first and a driver may list a config with a depth /
 * stencil buffer or another visual before the plain one - a depth 24 /
 * stencil 8 buffer for a 1920x1080 window is 7.9 MiB of memory per monitor
 * that nothing ever uses. Now every match is looked at and the cheapest
 * one is taken (lowest cost below; ties keep EGL's order), provided a
 * window surface can really be made with it (tried on a 1x1 window);
 * otherwise the first match as before.
 * WALPE_EGL_CONFIG=first: the first match as before (A/B);
 * WALPE_EGL_CONFIG=0x<id>: the config with that EGL_CONFIG_ID. */
static int egl_config_cost (EGLConfig c, VisualID want_visual)
{
    EGLint r = 0, g = 0, b = 0, a = 0, d = 0, st = 0, sb = 0, sm = 0, vid = 0, cav = EGL_NONE;
    eglGetConfigAttrib (D.egl_dpy, c, EGL_RED_SIZE, &r);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_BLUE_SIZE, &b);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_ALPHA_SIZE, &a);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_DEPTH_SIZE, &d);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_STENCIL_SIZE, &st);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_SAMPLE_BUFFERS, &sb);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_SAMPLES, &sm);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_NATIVE_VISUAL_ID, &vid);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_CONFIG_CAVEAT, &cav);
    int cost = 0;
    if ((VisualID) vid != want_visual)
        cost += 100000;                     /* the surface may be refused */
    if (cav != EGL_NONE)
        cost += 50000;                      /* slow or non-conformant */
    if (r != 8 || g != 8 || b != 8)
        cost += 10000;                      /* the scene is 8 bits per channel */
    if (sb > 0 || sm > 1)
        cost += 1000 * MAX (sm, 2);         /* samples x the colour buffer */
    cost += 10 * (d + st);                  /* bits per pixel nothing uses */
    cost += a;
    return cost;
}

static void egl_config_describe (EGLConfig c, char *buf, gsize n)
{
    EGLint r = 0, g = 0, b = 0, a = 0, d = 0, st = 0, sm = 0, vid = 0, id = 0;
    eglGetConfigAttrib (D.egl_dpy, c, EGL_RED_SIZE, &r);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_BLUE_SIZE, &b);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_ALPHA_SIZE, &a);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_DEPTH_SIZE, &d);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_STENCIL_SIZE, &st);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_SAMPLES, &sm);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_NATIVE_VISUAL_ID, &vid);
    eglGetConfigAttrib (D.egl_dpy, c, EGL_CONFIG_ID, &id);
    g_snprintf (buf, n, "R%d G%d B%d A%d, depth %d, stencil %d, samples %d (visual 0x%x, config 0x%x)",
                r, g, b, a, d, st, sm, (unsigned) vid, (unsigned) id);
}

/* can a window of the root visual get a surface with this config? */
static gboolean egl_config_usable (EGLConfig c)
{
    XSetWindowAttributes swa = { .override_redirect = True };
    Window w = XCreateWindow (D.xd, D.root, 0, 0, 1, 1, 0, CopyFromParent, InputOutput,
                              CopyFromParent, CWOverrideRedirect, &swa);
    if (w == None)
        return FALSE;
    EGLSurface sf = eglCreateWindowSurface (D.egl_dpy, c, (EGLNativeWindowType) w, NULL);
    gboolean ok = sf != EGL_NO_SURFACE;
    if (ok)
        eglDestroySurface (D.egl_dpy, sf);
    else
        (void) eglGetError ();
    XDestroyWindow (D.xd, w);
    XSync (D.xd, False);
    return ok;
}

static EGLConfig egl_pick_config (EGLConfig *all, EGLint n)
{
    EGLConfig first = all[0];
    const char *e = g_getenv ("WALPE_EGL_CONFIG");
    if (e && !strcmp (e, "first"))
        return first;
    if (e && *e) {
        EGLint want = (EGLint) g_ascii_strtoll (e, NULL, 0);
        for (EGLint i = 0; i < n; i++) {
            EGLint id = 0;
            eglGetConfigAttrib (D.egl_dpy, all[i], EGL_CONFIG_ID, &id);
            if (id == want && id != 0)
                return all[i];
        }
        g_warning ("WALPE_EGL_CONFIG=%s: no such window config; choosing one", e);
    }
    XWindowAttributes ra;
    VisualID vis = XGetWindowAttributes (D.xd, D.root, &ra) && ra.visual
                 ? XVisualIDFromVisual (ra.visual) : 0;
    EGLint best = 0;
    int best_cost = egl_config_cost (all[0], vis);
    for (EGLint i = 1; i < n; i++) {
        int c = egl_config_cost (all[i], vis);
        if (c < best_cost) {
            best = i;
            best_cost = c;
        }
    }
    /* only a config of the windows' own visual replaces the first match
     * (anything else may be refused, or raise an X error, when the surface
     * is made) */
    if (best == 0 || best_cost >= 100000)
        return first;
    if (!egl_config_usable (all[best])) {
        g_message ("EGL: the cheapest window config cannot make a surface here; "
                   "using the first match");
        return first;
    }
    char was[160];
    egl_config_describe (first, was, sizeof was);
    g_message ("EGL: %d window configs; the first match was %s", (int) n, was);
    return all[best];
}

static gboolean init_egl (void)
{
    /* (the gpu.conf key egl_display=own and the daemon's own EGL connection are
     * one thing here: D.egl_xd, on by default, WALPE_ONE_XCONN=1 = shared) */
    D.egl_dpy = eglGetDisplay ((EGLNativeDisplayType) D.egl_xd);
    if (D.egl_dpy == EGL_NO_DISPLAY) {
        g_warning ("eglGetDisplay failed");
        return FALSE;
    }
    if (!eglInitialize (D.egl_dpy, NULL, NULL)) {
        g_warning ("eglInitialize failed");
        return FALSE;
    }
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, EGL_DONT_CARE,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    EGLConfig conf = NULL;
    EGLConfig all[512];
    EGLint n = 0;
    if (!eglChooseConfig (D.egl_dpy, cfg_attrs, all, G_N_ELEMENTS (all), &n) || n < 1) {
        g_warning ("eglChooseConfig failed");
        return FALSE;
    }
    conf = egl_pick_config (all, n);
    D.egl_conf = conf;
    {
        /* banding / memory diagnostics: what is actually rendered to */
        char desc[160];
        egl_config_describe (conf, desc, sizeof desc);
        g_message ("EGL config: %s", desc);
    }
    present_egl_probe ();
    create_share_root ();
    return TRUE;
}

/* ------------------------------------------------ "Render on CPU" set-up */

/* first monitor thread, its context current: what really renders */
static void sw_note_gl (void)
{
    g_mutex_lock (&Sw.lock);
    if (!Sw.gl_renderer) {
        const char *v = (const char *) glGetString (GL_VENDOR);
        const char *r = (const char *) glGetString (GL_RENDERER);
        const char *ver = (const char *) glGetString (GL_VERSION);
        Sw.gl_vendor = g_strdup (v ? v : "?");
        Sw.gl_renderer = g_strdup (r ? r : "?");
        Sw.gl_version = g_strdup (ver ? ver : "?");
        gboolean cpu = walpe_sw_renderer_is_software (Sw.gl_renderer);
        g_message ("renderer: %s | %s | %s - rendering on the %s%s", Sw.gl_vendor, Sw.gl_renderer,
                   Sw.gl_version, cpu ? "CPU" : "GPU",
                   Sw.active ? " (software rendering is switched on)"
                   : cpu ? " (no GPU driver answered; the software rendering switch is off)" : "");
        if (Sw.active && !cpu)
            g_warning ("software rendering is switched on but the GL context is not Mesa's "
                       "software renderer");
    }
    g_mutex_unlock (&Sw.lock);
}

/* `walpe-daemon --sw-probe`, run by sw_setup() in a child that has the
 * software environment: the daemon's own path once - EGL on the X display,
 * its config, a context from its ladder, a window with the root's visual
 * as create_desktop_window() makes them (never mapped), one frame. Reports
 * key=value lines on stdout (walpe_sw_parse_probe). An X error ends the
 * child through Xlib's default handler: no "drew=1". */
static int sw_probe_run (void)
{
    alarm (10);   /* never hold up the daemon's start */
    D.xd = XOpenDisplay (NULL);
    if (!D.xd) {
        g_print ("fail=cannot open the X display\n");
        return 1;
    }
    D.root = DefaultRootWindow (D.xd);
    if (eglGetDisplay ((EGLNativeDisplayType) D.xd) == EGL_NO_DISPLAY) {
        /* libglvnd had nothing to ask: the library named by the vendor
         * file did not load */
        const char *vf = g_getenv ("__EGL_VENDOR_LIBRARY_FILENAMES");
        g_print ("fail=Mesa's EGL library (libEGL_mesa, named in %s) could not be loaded\n",
                 vf ? vf : "its vendor file");
        return 1;
    }
    if (!init_egl ()) {
        g_print ("fail=Mesa's EGL does not start on this X server\n");
        return 1;
    }
    const char *ev = eglQueryString (D.egl_dpy, EGL_VENDOR);
    g_print ("egl=%s\n", ev ? ev : "?");
    XSetWindowAttributes swa = { .override_redirect = True };
    Window w = XCreateWindow (D.xd, D.root, 0, 0, 64, 64, 0, CopyFromParent, InputOutput,
                              CopyFromParent, CWOverrideRedirect, &swa);
    XSync (D.xd, False);
    EGLSurface surf = eglCreateWindowSurface (D.egl_dpy, D.egl_conf, (EGLNativeWindowType) w, NULL);
    EGLContext ctx = surf != EGL_NO_SURFACE ? create_thread_context () : EGL_NO_CONTEXT;
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent (D.egl_dpy, surf, surf, ctx)) {
        g_print ("fail=Mesa's EGL cannot make a window surface or a GL 3.3 / GLES 3 context "
                 "on this X server (EGL error 0x%x)\n", eglGetError ());
        return 1;
    }
    const char *v = (const char *) glGetString (GL_VENDOR);
    const char *r = (const char *) glGetString (GL_RENDERER);
    const char *ver = (const char *) glGetString (GL_VERSION);
    g_print ("vendor=%s\nrenderer=%s\nversion=%s\n", v ? v : "?", r ? r : "?", ver ? ver : "?");
    glViewport (0, 0, 64, 64);
    glClearColor (0, 0, 0, 1);
    glClear (GL_COLOR_BUFFER_BIT);
    gboolean drew = eglSwapBuffers (D.egl_dpy, surf) && glGetError () == GL_NO_ERROR;
    XSync (D.xd, False);   /* an X error of the presentation arrives here */
    g_print ("drew=%d\n", drew ? 1 : 0);
    return 0;
}

/* main thread, after the config is loaded, before the first EGL call and
 * before any thread exists (setenv): decide the mode of this process */
static void sw_setup (void)
{
    int ov = walpe_sw_env_override (g_getenv ("WALPE_SOFTWARE"));
    Sw.env_forced = ov >= 0;
    Sw.wanted = ov >= 0 ? ov : D.cfg->software_render;
    const char *lp = g_getenv ("LP_NUM_THREADS");
    Sw.threads = lp && *lp ? atoi (lp)
                           : walpe_sw_threads (D.cfg->software_threads,
                                               (int) g_get_num_processors ());
    if (!Sw.wanted)
        return;
    char *searched = NULL, *why = NULL;
    char *json = walpe_sw_find_mesa_vendor (NULL, &searched);
    if (!json) {
        why = g_strdup_printf ("Mesa's EGL vendor file (*mesa.json) is not in %s - is Mesa "
                               "(media-libs/mesa) installed?", searched);
    } else {
        char **pairs = walpe_sw_env_pairs (json, Sw.threads, lp && *lp);
        char **envp = walpe_sw_environ (g_get_environ (), pairs);
        char *pargv[] = { Sw.exe, (char *) "--sw-probe", NULL };
        char *out = NULL, *errout = NULL;
        GError *e = NULL;
        if (!g_spawn_sync (NULL, pargv, envp, G_SPAWN_DEFAULT, NULL, NULL, &out, &errout, NULL,
                           &e)) {
            why = g_strdup_printf ("the test of Mesa's EGL could not be run (%s)",
                                   e ? e->message : "?");
            g_clear_error (&e);
        } else {
            char *r = NULL, *ver = NULL;
            if (walpe_sw_parse_probe (out, NULL, &r, &ver, &why)) {
                for (int i = 0; pairs[i] && pairs[i + 1]; i += 2)
                    g_setenv (pairs[i], pairs[i + 1], TRUE);
                Sw.active = TRUE;
                g_message ("software rendering: on - %s, %d rasteriser thread%s (vendor file %s)",
                           r, Sw.threads, Sw.threads == 1 ? "" : "s", json);
            } else if (errout && *errout) {
                g_debug ("software rendering test, stderr: %s", errout);
            }
            g_free (r);
            g_free (ver);
        }
        g_free (out);
        g_free (errout);
        g_strfreev (envp);
        g_strfreev (pairs);
    }
    if (why) {
        Sw.note = why;
        g_warning ("software rendering is switched on but not available: %s. Rendering on "
                   "the GPU.", why);
    }
    g_free (json);
    g_free (searched);
}

static gboolean sw_restart_cb (gpointer user)
{
    (void) user;
    Sw.restart_id = 0;
    Sw.restart = TRUE;
    g_message ("software rendering setting changed: restarting in place");
    g_main_loop_quit (D.loop);
    return G_SOURCE_REMOVE;
}

/* a little later, so that the reply to `set` is out first */
static void sw_schedule_restart (void)
{
    if (!Sw.restart_id)
        Sw.restart_id = g_timeout_add (200, sw_restart_cb, NULL);
}

/* Child processes still alive at the end of main() (WebKit's network and
 * page processes: they normally die with their parent, which an exec is
 * not): end them, or every restart would leave a set behind. */
static void sw_end_children (void)
{
    GArray *kids = g_array_new (FALSE, FALSE, sizeof (pid_t));
    pid_t self = getpid ();
    GDir *d = g_dir_open ("/proc", 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        if (!g_ascii_isdigit (*n))
            continue;
        char *sp = g_strdup_printf ("/proc/%s/stat", n), *txt = NULL;
        if (g_file_get_contents (sp, &txt, NULL, NULL)) {
            /* "pid (comm) state ppid ...": comm may hold spaces and ')' */
            const char *rp = strrchr (txt, ')');
            int ppid = 0;
            if (rp && sscanf (rp + 1, " %*c %d", &ppid) == 1 && ppid == (int) self) {
                pid_t k = (pid_t) atoi (n);
                g_array_append_val (kids, k);
            }
            g_free (txt);
        }
        g_free (sp);
    }
    if (d)
        g_dir_close (d);
    for (guint i = 0; i < kids->len; i++)
        kill (g_array_index (kids, pid_t, i), SIGTERM);
    for (int round = 0; round < 20 && kids->len; round++) {   /* up to 1 s, then SIGKILL */
        for (guint i = 0; i < kids->len;) {
            pid_t k = g_array_index (kids, pid_t, i);
            pid_t r = waitpid (k, NULL, WNOHANG);
            if (r == k || (r < 0 && errno != EINTR))
                g_array_remove_index_fast (kids, i);
            else
                i++;
        }
        if (kids->len)
            g_usleep (50 * 1000);
    }
    for (guint i = 0; i < kids->len; i++) {
        kill (g_array_index (kids, pid_t, i), SIGKILL);
        waitpid (g_array_index (kids, pid_t, i), NULL, 0);
    }
    g_array_free (kids, TRUE);
}

/* Right before an exec of the daemon itself (both restarts): nothing but
 * stdin / stdout / stderr and `keep` (-1 = nothing) may reach the new
 * image. Libraries leave descriptors open without close-on-exec (WebKit's
 * memory monitor: /proc/meminfo, the cgroup files, /dev/urandom ...); the
 * new image knows nothing of them and never closes them. Every open
 * descriptor is looked at (/proc/self/fd), not only the first 4096: the
 * limit can be far higher. */
static void exec_close_others (int keep)
{
    GArray *fds = g_array_new (FALSE, FALSE, sizeof (int));
    GDir *d = g_dir_open ("/proc/self/fd", 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        int fd = atoi (n);
        if (g_ascii_isdigit (*n) && fd >= 3)
            g_array_append_val (fds, fd);
    }
    if (d)
        g_dir_close (d);
    else   /* no /proc: the range that is always there */
        for (int fd = 3; fd < 4096; fd++)
            g_array_append_val (fds, fd);
    for (guint i = 0; i < fds->len; i++) {
        int fd = g_array_index (fds, int, i);
        int fl = fd != keep ? fcntl (fd, F_GETFD) : -1;
        if (fl >= 0)
            fcntl (fd, F_SETFD, fl | FD_CLOEXEC);
    }
    g_array_free (fds, TRUE);
}

/* end of main(), everything torn down: the same binary, arguments and
 * start environment again; the control socket stays open across it
 * (ipc.h). Returns only when the exec failed.
 * Every restart in place goes through here: "Render on CPU" changed
 * (Sw.restart), the monitor layout changed or web hardware acceleration
 * was switched (D.restart; layout_stamp: the time of this restart for the
 * next image's LAYOUT_RESTART_GAP_S). The second kind used to have an exec
 * of its own that released the control socket and the lock first and left
 * the child processes alone: for a moment no daemon was there for the GUI
 * and walpe-ctl ("not running"; a supervisor could start a second one that
 * then won the lock), and each restart with a web wallpaper on screen left
 * its WebKit network process and sandbox helper behind as children of the
 * new image, for good. The START environment in both cases: the variables
 * sw_setup() and the web / driver hint setup put into this process must
 * not look like the user's own to the next image (it could then never
 * leave software rendering again). */
static void restart_exec (gboolean layout_stamp)
{
    int lfd = walpe_ipc_listen_fd ();
    char **env = g_strdupv (Sw.orig_env);
    if (layout_stamp) {
        char now[32];
        g_snprintf (now, sizeof now, "%" G_GINT64_FORMAT, g_get_monotonic_time ());
        env = g_environ_setenv (env, "WALPE_LAYOUT_RESTARTED", now, TRUE);
    }
    sw_end_children ();
    /* nothing else may leak into the new image */
    exec_close_others (lfd);
    if (lfd >= 0) {
        char n[16];
        g_snprintf (n, sizeof n, "%d", lfd);
        fcntl (lfd, F_SETFD, 0);
        env = g_environ_setenv (env, "WALPE_IPC_FD", n, TRUE);
    }
    fflush (NULL);
    execve (g_file_test (Sw.exe, G_FILE_TEST_IS_EXECUTABLE) ? Sw.exe : "/proc/self/exe", Sw.argv,
            env);
    g_warning ("restart in place failed (%s): walpe-daemon stops; start it again",
               g_strerror (errno));
    {
        /* the socket (kept open for the new image) and the single-instance
         * lock: this process ends now */
        char *sp = walpe_socket_path ();
        walpe_ipc_release (lfd >= 0 ? sp : NULL);
        g_free (sp);
    }
}

/* ------------------------------------------------------------- monitors */

/* SIGTERM / SIGINT / SIGHUP: the clean shutdown of `quit`. The handler
 * only notes the signal and wakes the main loop through an eventfd. The
 * note is what g_unix_signal_add () could not give: it is still there when
 * the main loop has ended. A restart in place leaves the loop, tears
 * everything down (joining a render thread in the middle of a load takes
 * seconds) and execs; a signal in that time was caught, never dispatched
 * and gone with the exec - `pkill walpe-daemon` or a session ending while
 * the monitors change left a daemon running that had been told to stop.
 * main() looks at term_signal before the exec (term_pending). */
static volatile sig_atomic_t term_signal;   /* the signal that arrived (0 = none) */
static int term_fd = -1;

static void term_handler (int sig)
{
    int e = errno;
    term_signal = sig;
    if (term_fd >= 0) {
        guint64 one = 1;
        if (write (term_fd, &one, sizeof one) < 0) { /* the flag is set: enough */ }
    }
    errno = e;
}

static gboolean quit_on_signal (gint fd, GIOCondition cond, gpointer user)
{
    (void) cond; (void) user;
    guint64 n;
    if (fd >= 0 && read (fd, &n, sizeof n) < 0) { /* only the wake-up mattered */ }
    g_message ("signal: shutting down");
    g_main_loop_quit (D.loop);
    return G_SOURCE_CONTINUE;
}

static gboolean quit_on_signal_glib (gpointer user)
{
    (void) user;
    return quit_on_signal (-1, 0, NULL);
}

static void term_signals_init (void)
{
    static const int sigs[] = { SIGTERM, SIGINT, SIGHUP };
    term_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (term_fd < 0) {
        /* no descriptor to wake the loop with: GLib's signal sources, as
         * before (a signal during a restart is lost again) */
        for (guint i = 0; i < G_N_ELEMENTS (sigs); i++)
            g_unix_signal_add (sigs[i], quit_on_signal_glib, NULL);
        return;
    }
    g_unix_fd_add (term_fd, G_IO_IN, quit_on_signal, NULL);
    struct sigaction sa;
    memset (&sa, 0, sizeof sa);
    sa.sa_handler = term_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset (&sa.sa_mask);
    for (guint i = 0; i < G_N_ELEMENTS (sigs); i++)
        sigaction (sigs[i], &sa, NULL);
}

/* main, torn down, about to exec for a restart in place: was the daemon
 * told to stop meanwhile? The three signals get their default action back
 * first, so one that arrives after this look ends the process at once
 * (before the exec, or in the new image before it has its own handler):
 * no window in which it is lost. */
static gboolean term_pending (void)
{
    static const int sigs[] = { SIGTERM, SIGINT, SIGHUP };
    for (guint i = 0; i < G_N_ELEMENTS (sigs); i++)
        signal (sigs[i], SIG_DFL);
    return term_signal != 0;
}

/* WALPE_FPS_LOG=<seconds>: one line every <seconds> with the frames each
 * monitor really presented (buffer swaps), next to its refresh rate, the
 * fps setting and its role. "walpe-ctl set fps=N" and pauses show up in the
 * next line. Video wallpapers add their own line (frames decoded / shown,
 * video_custom.c). */
static gboolean fpslog_tick (gpointer user)
{
    (void) user;
    gint64 now = g_get_monotonic_time ();
    double dt = (now - D.fpslog_us) / 1e6;
    D.fpslog_us = now;
    if (dt <= 0)
        return G_SOURCE_CONTINUE;
    GString *ln = g_string_new ("fps:");
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        guint c = (guint) g_atomic_int_get (&m->render_count);
        double fps = (c - m->fpslog_count) / dt;
        m->fpslog_count = c;
        g_string_append_printf (ln, "%s mon%d %s %.1f fps (", l == D.mons ? "" : " |", m->index,
                                m->name, fps);
        if (m->hz > 0)
            g_string_append_printf (ln, "%.1f Hz, ", m->hz);
        else
            g_string_append (ln, "refresh unknown, ");
        g_string_append (ln, walpe_type_name (g_atomic_int_get (&m->content_type)));
        if (m->leader)
            g_string_append_printf (ln, ", follows mon%d", m->leader->index);
        else if (m->nfollowers)
            g_string_append (ln, ", leads");
        if (m->own_paused)
            g_string_append_printf (ln, ", paused%s", m->covered ? ": covered" : "");
        g_string_append_c (ln, ')');
    }
    g_string_append_printf (ln, " [fps setting %d%s]", D.cfg->fps,
                            D.no_refresh_cap ? ", no refresh cap" : "");
    g_string_append (ln, " [swap interval");
    for (GList *l = D.mons; l; l = l->next)
        g_string_append_printf (ln, " mon%d %d", ((Mon *) l->data)->index,
                                g_atomic_int_get (&((Mon *) l->data)->present_iv));
    g_string_append_printf (ln, "%s]", present_composited () ? ", compositor" : "");
    g_message ("%s", ln->str);
    g_string_free (ln, TRUE);
    return G_SOURCE_CONTINUE;
}

#ifdef HAVE_XRANDR
/* refresh rate of the output(s) lit at a monitor's rectangle: the CRTC
 * whose area is that rectangle (else the one containing its centre); the
 * fastest when several show it (mirrored outputs). 0 = none found. */
static double randr_refresh (const WalpeRect *g)
{
    int ev = 0, er = 0;
    if (!XRRQueryExtension (D.xd, &ev, &er))
        return 0.0;
    XRRScreenResources *res = XRRGetScreenResourcesCurrent (D.xd, D.root);
    if (!res)
        return 0.0;
    double exact = 0.0, inside = 0.0;
    int cx = g->x + g->width / 2, cy = g->y + g->height / 2;
    for (int c = 0; c < res->ncrtc; c++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo (D.xd, res, res->crtcs[c]);
        if (!ci)
            continue;
        if (ci->mode != None && ci->noutput > 0) {
            double hz = 0.0;
            for (int k = 0; k < res->nmode; k++)
                if (res->modes[k].id == ci->mode)
                    hz = walpe_mode_refresh (res->modes[k].dotClock, res->modes[k].hTotal,
                                             res->modes[k].vTotal,
                                             (res->modes[k].modeFlags & RR_Interlace) != 0,
                                             (res->modes[k].modeFlags & RR_DoubleScan) != 0);
            if (ci->x == g->x && ci->y == g->y && (int) ci->width == g->width &&
                (int) ci->height == g->height)
                exact = MAX (exact, hz);
            else if (cx >= ci->x && cx < ci->x + (int) ci->width && cy >= ci->y &&
                     cy < ci->y + (int) ci->height)
                inside = MAX (inside, hz);
        }
        XRRFreeCrtcInfo (ci);
    }
    XRRFreeScreenResources (res);
    return exact > 0.0 ? exact : inside;
}
#endif

static Mon *make_monitor (const WalpeRect *g, int idx, gboolean or_mode, const char *name)
{
    Mon *m = g_new0 (Mon, 1);
    m->name = name && *name ? g_strdup (name) : g_strdup_printf ("mon%d", idx);
    m->ptr_lx = m->ptr_ly = -2.0f;
    m->index = idx;
    m->geom = *g;
    m->xid = create_desktop_window (g, or_mode);
    m->present_iv_applied = -1;
    m->present_iv = 1;
    if (D.egl_xd != D.xd)
        XSync (D.xd, False);    /* EGL names the window on its own connection */
    m->surf = eglCreateWindowSurface (D.egl_dpy, D.egl_conf,
                                      (EGLNativeWindowType) m->xid, NULL);
    if (m->surf == EGL_NO_SURFACE) {
        g_warning ("eglCreateWindowSurface failed for monitor %d (0x%x)",
                   idx, eglGetError ());
        XDestroyWindow (D.xd, m->xid);
        m->xid = None;
        g_free (m);
        return NULL;
    }
    {
        /* every frame is drawn in full before it is swapped: the back
         * buffer's old content is never needed. A surface that preserves
         * it (EGL_BUFFER_PRESERVED) makes the driver copy the whole frame
         * back on every swap; ask for EGL_BUFFER_DESTROYED where that is
         * not the surface's state already. WALPE_EGL_SWAP=keep leaves the
         * driver's default (A/B). */
        const char *e = g_getenv ("WALPE_EGL_SWAP");
        EGLint sb = 0;
        gboolean have = eglQuerySurface (D.egl_dpy, m->surf, EGL_SWAP_BEHAVIOR, &sb);
        if (!have)
            (void) eglGetError ();
        gboolean set = FALSE;
        if (have && sb == EGL_BUFFER_PRESERVED && !(e && !strcmp (e, "keep"))) {
            set = eglSurfaceAttrib (D.egl_dpy, m->surf, EGL_SWAP_BEHAVIOR, EGL_BUFFER_DESTROYED);
            if (!set)
                (void) eglGetError ();
        }
        if (idx == 0)
            g_message ("EGL surface: swap behaviour %s%s",
                       !have ? "unknown" : sb == EGL_BUFFER_PRESERVED ? "preserved" : "destroyed",
                       set ? " -> destroyed" : "");
    }
    g_mutex_init (&m->lock);
    g_cond_init (&m->cond);
    m->clip_n = -1;     /* everything, until "Skip hidden areas" says otherwise */
    D.mons = g_list_append (D.mons, m);
    return m;
}

/* ------------------------------------------------------ monitor layout
 * Rectangles (Xinerama, else the whole screen; WALPE_MONITORS overrides),
 * output names (RandR 1.5 monitors matched by geometry, else "monN") and
 * refresh rates (WALPE_MONITORS "@HZ", else the RandR mode). Read at
 * start-up and again whenever the X screen changes. */
typedef struct {
    int n;
    WalpeRect *geom;
    char **name;        /* n entries, never NULL */
    double *hz;
} Layout;

static Layout layout_now;       /* the layout this daemon runs with */
static gboolean layout_watch;   /* follow changes (not in --windowed) */
static guint layout_timer;
#ifdef HAVE_XRANDR
static int rr_event_base = -1;
#endif

static void layout_free (Layout *l)
{
    for (int i = 0; l->name && i < l->n; i++)
        g_free (l->name[i]);
    g_free (l->name);
    g_free (l->geom);
    g_free (l->hz);
    memset (l, 0, sizeof (*l));
}

static void layout_detect (Layout *out)
{
    WalpeRect *geoms = NULL;
    int nmon = 0;
    int evb = 0, errb = 0;
    if (XineramaQueryExtension (D.xd, &evb, &errb)) {
        int nscr = 0;
        XineramaScreenInfo *scr = XineramaQueryScreens (D.xd, &nscr);
        if (scr && nscr > 0) {
            nmon = nscr;
            geoms = g_new0 (WalpeRect, nmon);
            for (int i = 0; i < nmon; i++) {
                geoms[i].x = scr[i].x_org;
                geoms[i].y = scr[i].y_org;
                geoms[i].width = scr[i].width;
                geoms[i].height = scr[i].height;
            }
        }
        if (scr)
            XFree (scr);
    }
    /* WALPE_MONITORS="WxH+X+Y,...": override the monitor layout (testing on
     * a server without Xinerama, e.g. Xvfb with one wide screen) */
    const char *mon_env = g_getenv ("WALPE_MONITORS");
    char *mon_file = NULL;
    if (mon_env && mon_env[0] == '@') {
        /* WALPE_MONITORS=@FILE: the list is read from FILE at every
         * detection, so a test can change the layout under a running
         * daemon (then any RandR / root resize event makes it look) */
        if (g_file_get_contents (mon_env + 1, &mon_file, NULL, NULL))
            mon_env = g_strstrip (mon_file);
        else
            mon_env = NULL;
    }
    char **env_names = NULL;   /* "NAME=WxH+X+Y" entries name their monitor */
    double *env_hz = NULL;     /* "...@HZ": its refresh rate (testing the pacing) */
    if (mon_env && *mon_env) {
        char **parts = g_strsplit (mon_env, ",", -1);
        int n = 0;
        WalpeRect *g = g_new0 (WalpeRect, g_strv_length (parts) + 1);
        env_names = g_new0 (char *, g_strv_length (parts) + 1);
        env_hz = g_new0 (double, g_strv_length (parts) + 1);
        for (int i = 0; parts[i]; i++) {
            const char *eq = strchr (parts[i], '=');
            const char *geo = eq ? eq + 1 : parts[i];
            if (sscanf (geo, "%dx%d+%d+%d", &g[n].width, &g[n].height, &g[n].x,
                        &g[n].y) == 4 && g[n].width > 0 && g[n].height > 0) {
                env_names[n] = eq ? g_strndup (parts[i], (gsize) (eq - parts[i])) : NULL;
                env_hz[n] = walpe_monitor_spec_hz (geo);
                n++;
            }
        }
        g_strfreev (parts);
        if (n > 0) {
            g_free (geoms);
            geoms = g;
            nmon = n;
        } else {
            g_free (g);
            g_clear_pointer (&env_names, g_free);
            g_clear_pointer (&env_hz, g_free);
        }
    }
    if (nmon < 1) {
        nmon = 1;
        g_free (geoms);
        geoms = g_new0 (WalpeRect, 1);
        Window rw;
        int wx, wy;
        unsigned ww = 0, wh = 0, wb, wd;
        XGetGeometry (D.xd, D.root, &rw, &wx, &wy, &ww, &wh, &wb, &wd);
        geoms[0].width = (int) ww;
        geoms[0].height = (int) wh;
    }

    /* output names (config keys for per-monitor wallpapers): RandR 1.5
     * monitors matched by geometry ("HDMI-0", "DP-2"), else "monN" */
    char **names = g_new0 (char *, nmon + 1);
    for (int i = 0; i < nmon; i++)
        names[i] = env_names ? env_names[i] : NULL;   /* ownership moves */
    g_free (env_names);     /* the array only: unnamed entries are NULL holes */
#ifdef HAVE_XRANDR
    {
        int rev = 0, rerr = 0, nm = 0;
        XRRMonitorInfo *mi = XRRQueryExtension (D.xd, &rev, &rerr)
                                 ? XRRGetMonitors (D.xd, D.root, True, &nm) : NULL;
        for (int i = 0; i < nmon; i++)
            for (int k = 0; mi && k < nm && !names[i]; k++)
                if (mi[k].x == geoms[i].x && mi[k].y == geoms[i].y &&
                    mi[k].width == geoms[i].width && mi[k].height == geoms[i].height) {
                    char *an = XGetAtomName (D.xd, mi[k].name);
                    if (an) {
                        names[i] = g_strdup (an);
                        XFree (an);
                    }
                }
        if (mi)
            XRRFreeMonitors (mi);
    }
#endif
    for (int i = 0; i < nmon; i++)
        if (!names[i])
            names[i] = g_strdup_printf ("mon%d", i);
    /* refresh rates (pacing, shared-rendering leader): WALPE_MONITORS
     * "@HZ", else the RandR mode of the output */
    double *hzs = g_new0 (double, nmon + 1);
    for (int i = 0; i < nmon; i++) {
        hzs[i] = env_hz ? env_hz[i] : 0.0;
#ifdef HAVE_XRANDR
        if (hzs[i] <= 0.0 && !env_hz)
            hzs[i] = randr_refresh (&geoms[i]);
#endif
    }
    g_free (env_hz);
    g_free (mon_file);
    out->n = nmon;
    out->geom = geoms;
    out->name = names;
    out->hz = hzs;
}

static gboolean layout_equal (const Layout *a, const Layout *b)
{
    if (a->n != b->n)
        return FALSE;
    for (int i = 0; i < a->n; i++)
        if (memcmp (&a->geom[i], &b->geom[i], sizeof (WalpeRect)) ||
            g_strcmp0 (a->name[i], b->name[i]) || fabs (a->hz[i] - b->hz[i]) > 0.5)
            return FALSE;
    return TRUE;
}

/* The X screen changed (monitor connected / removed / switched off with
 * xrandr, another resolution, position or refresh rate). Monitor
 * rectangles, windows, EGL surfaces, render threads, shared-rendering
 * groups and the pacing are all set up for the layout found at start-up
 * ("read-only after init"), and nothing followed a change: windows stayed
 * where the old monitors were until the daemon was restarted by hand. The
 * daemon now does that restart itself: when the layout really differs
 * (debounced; RandR sends several events per change) it shuts down the
 * normal way and execs itself, which comes back with the saved wallpapers
 * on the new layout. A running import or workshop download is not cut
 * off: the restart waits for it. A changed layout must be seen twice, a
 * second apart (xrandr goes through intermediate states: outputs off,
 * screen resized, outputs on), and restarts are at least
 * LAYOUT_RESTART_GAP_S apart (WALPE_LAYOUT_RESTARTED carries the time of
 * the last one across the exec). */
#define LAYOUT_RESTART_GAP_S 10
static Layout layout_pending;   /* a different layout, seen once */

static gboolean layout_check (gpointer user)
{
    (void) user;
    layout_timer = 0;
    Layout l = { 0 };
    layout_detect (&l);
    gboolean same = layout_equal (&l, &layout_now);
    if (same) {
        layout_free (&layout_pending);      /* it was a passing state */
        layout_free (&l);
        return G_SOURCE_REMOVE;
    }
    gint64 last = g_ascii_strtoll (g_getenv ("WALPE_LAYOUT_RESTARTED") ? g_getenv ("WALPE_LAYOUT_RESTARTED") : "0",
                                   NULL, 10);
    gboolean settled = layout_pending.n && layout_equal (&l, &layout_pending);
    gboolean too_soon = last > 0 && g_get_monotonic_time () - last < LAYOUT_RESTART_GAP_S * G_USEC_PER_SEC &&
                        g_get_monotonic_time () >= last;
    if (!settled || too_soon || Imp.active || Ws.active) {
        layout_free (&layout_pending);
        layout_pending = l;                 /* look again: it has to hold */
        layout_timer = g_timeout_add (settled ? 2000 : 1000, layout_check, NULL);
        return G_SOURCE_REMOVE;
    }
    {
        GString *d = g_string_new (NULL);
        for (int i = 0; i < l.n; i++)
            g_string_append_printf (d, "%s%s %dx%d+%d+%d", i ? ", " : "", l.name[i],
                                    l.geom[i].width, l.geom[i].height, l.geom[i].x, l.geom[i].y);
        g_message ("monitor layout changed (now: %s): restarting to follow it", d->str);
        g_string_free (d, TRUE);
        D.restart = TRUE;
        g_main_loop_quit (D.loop);
    }
    layout_free (&l);
    return G_SOURCE_REMOVE;
}

static void layout_schedule (void)
{
    if (!layout_watch || D.restart)
        return;
    if (layout_timer)
        g_source_remove (layout_timer);
    layout_timer = g_timeout_add (800, layout_check, NULL);
}

#ifdef HAVE_WEB
/* ------------------------------------------- how WebKit renders web pages
 * Decided once, here, before any thread exists (setenv is not
 * thread-safe, and WebKit / GDK read these variables when they start).
 *
 * Software (the default since version 0.11): three switches, each of which
 * alone keeps WebKit off the GPU:
 *   WEBKIT_DISABLE_COMPOSITING_MODE=1  no accelerated compositing
 *   WEBKIT_DISABLE_DMABUF_RENDERER=1   no renderer to composite with
 *   GDK_DISABLE=gl / GDK_DEBUG=gl-disable   GTK has no GL, and WebKit
 *                                      refuses acceleration without it
 * They are there because on an NVIDIA card WebKit abort()ed the
 * daemon ("Could not create GBM EGL display"). The price: CSS animations,
 * 2D canvas and video are painted by one CPU thread of the page process.
 *
 * "web hardware acceleration" (config "web_accel", walpe-ctl set
 * web_accel=on|off, the GUI switch, WALPE_WEB_ACCEL=0|1|dmabuf|force)
 * leaves the three switches unset, so pages are composited (and their
 * canvases, CSS animations and WebGL drawn) with the GPU in the page
 * process. Frames come back
 *   on / 1:  through shared memory (WEBKIT_DMABUF_RENDERER_FORCE_SHM=1,
 *            WEBKIT_DMABUF_RENDERER_DISABLE_GBM=1, WEBKIT_WEBGL_DISABLE_GBM=1):
 *            no GBM and no DMA-BUF anywhere, which is what aborted on NVIDIA;
 *   dmabuf:  as DMA-BUF textures, WebKit's own default (needs a driver
 *            whose GBM and DMA-BUF import work).
 * GTK itself still draws nothing with GL (GSK_RENDERER=cairo).
 * Fallback: the accelerated start-up is run in a child first
 * (`--web-probe`, with a WebGL page). If the child dies, hangs, or
 * reports a software GL renderer (then WebKit's own CPU painting is the
 * faster software path), this daemon runs in software mode exactly as if
 * the setting were off, and the log and `status` ("web_render") say why.
 * WALPE_WEB_ACCEL=force skips the software-renderer check (tests). */
static void web_env_set (GString *ours, const char *name, const char *value)
{
    if (g_getenv (name))
        return;         /* an explicit user value is kept */
    g_setenv (name, value, TRUE);
    g_string_append_printf (ours, "%s%s", ours->len ? "," : "", name);
}

static void web_env_undo (const char *list)
{
    char **names = g_strsplit (list ? list : "", ",", -1);
    for (int i = 0; names[i]; i++)
        if (*names[i])
            g_unsetenv (names[i]);
    g_strfreev (names);
}

static void web_env_software (GString *ours)
{
    web_env_set (ours, "WEBKIT_DISABLE_COMPOSITING_MODE", "1");
    web_env_set (ours, "WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    web_env_set (ours, "GDK_DISABLE", "gl");       /* GTK >= 4.16 */
    web_env_set (ours, "GDK_DEBUG", "gl-disable"); /* GTK < 4.16 */
}

/* the accelerated start-up in a child: TRUE = it worked; *renderer = what
 * WebGL runs on there (g_free), *why = the failure (g_free) */
#define WEB_PROBE_MAX_S 40
static gboolean web_accel_probe (char **renderer, char **why)
{
    *renderer = NULL;
    *why = NULL;
    char *exe = g_file_read_link ("/proc/self/exe", NULL);
    if (!exe) {
        *why = g_strdup ("cannot find the daemon's own executable");
        return FALSE;
    }
    char *argv[] = { exe, "--web-probe", NULL };
    char **envp = g_environ_setenv (g_get_environ (), "WALPE_WEB_PROBE_ACCEL", "1", TRUE);
    GString *out = g_string_new (NULL);
    GPid pid = 0;
    int ofd = -1, status = 0;
    GError *err = NULL;
    gboolean ok = g_spawn_async_with_pipes (NULL, argv, envp, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                                            &pid, NULL, &ofd, NULL, &err);
    if (!ok) {
        *why = g_strdup (err->message);
    } else {
        /* The child gives up by itself (20 s without a page, 8 s without
         * WebGL), but not when it is stuck inside a driver: this daemon
         * must come up whatever happens there, so it is killed after
         * WEB_PROBE_MAX_S. Its output is read while waiting; WebKit's
         * helper processes may keep the pipe open after it has gone, so
         * the end of the child, not of the pipe, ends the wait. */
        gint64 end = g_get_monotonic_time () + WEB_PROBE_MAX_S * G_USEC_PER_SEC, grace = 0;
        gboolean eof = FALSE, gone = FALSE, killed = FALSE;
        for (;;) {
            if (!eof) {
                struct pollfd pf = { .fd = ofd, .events = POLLIN };
                if (poll (&pf, 1, 100) > 0) {
                    char buf[1024];
                    ssize_t n = read (ofd, buf, sizeof buf);
                    if (n > 0)
                        g_string_append_len (out, buf, n);
                    else if (n == 0 || errno != EINTR)
                        eof = TRUE;
                }
            } else {
                g_usleep (20 * 1000);
            }
            gint64 now = g_get_monotonic_time ();
            if (!gone) {
                pid_t w = waitpid (pid, &status, WNOHANG);
                if (w == pid || (w < 0 && errno != EINTR)) {
                    gone = TRUE;
                    if (w < 0)
                        status = -1;        /* not ours to wait for: judged by its output */
                    grace = now + 300 * 1000;   /* what it wrote last */
                } else if (now > end) {
                    killed = TRUE;
                    kill (pid, SIGKILL);
                    waitpid (pid, &status, 0);
                    break;
                }
            }
            if (gone && (eof || now > grace))
                break;
        }
        close (ofd);
        g_spawn_close_pid (pid);
        if (killed) {
            ok = FALSE;
            *why = g_strdup_printf ("the test process did not finish within %d s", WEB_PROBE_MAX_S);
        } else if (status == -1) {
            ok = strstr (out->str, "WALPE-PROBE webgl=") != NULL;
            if (!ok)
                *why = g_strdup ("the test process ended without a result");
        } else if (!g_spawn_check_wait_status (status, NULL)) {
            ok = FALSE;
            *why = WIFSIGNALED (status)
                ? g_strdup_printf ("the test process was killed by signal %d (%s)", WTERMSIG (status),
                                   g_strsignal (WTERMSIG (status)))
                : g_strdup_printf ("the test process exited with status %d", WEXITSTATUS (status));
        }
    }
    const char *l = strstr (out->str, "WALPE-PROBE webgl=");
    if (l) {
        l += strlen ("WALPE-PROBE webgl=");
        *renderer = g_strstrip (g_strndup (l, strcspn (l, "\r\n")));
    }
    g_clear_error (&err);
    g_string_free (out, TRUE);
    g_strfreev (envp);
    g_free (exe);
    return ok;
}

static gboolean web_gl_is_software (const char *r)
{
    char *l = g_ascii_strdown (r ? r : "", -1);
    gboolean sw = !*l || strstr (l, "llvmpipe") || strstr (l, "softpipe") || strstr (l, "swiftshader") ||
                  strstr (l, "swrast") || strstr (l, "software") || g_str_has_prefix (l, "none") ||
                  g_str_has_prefix (l, "unavailable") || g_str_has_prefix (l, "error");
    g_free (l);
    return sw;
}

static void web_env_setup (gboolean want_accel, int fps)
{
    /* what an earlier image of this process set (it restarts itself after
     * a monitor layout or web_accel change) is not the user's choice */
    web_env_undo (g_getenv ("WALPE_WEB_ENV_OURS"));
    g_unsetenv ("WALPE_WEB_ENV_OURS");
    g_unsetenv ("WALPE_WEB_PROBED");
    GString *ours = g_string_new (NULL);
    const char *e = g_getenv ("WALPE_WEB_ACCEL");
    gboolean force = FALSE, dmabuf = FALSE;
    if (e && *e) {
        want_accel = strcmp (e, "0") && g_ascii_strcasecmp (e, "off") && g_ascii_strcasecmp (e, "no");
        dmabuf = !g_ascii_strcasecmp (e, "dmabuf");
        force = !g_ascii_strcasecmp (e, "force");
    }
    /* GTK (web pages only) renders nothing through GSK: keep it off the
     * GPU in either mode */
    web_env_set (ours, "GSK_RENDERER", "cairo");
    if (!want_accel) {
        web_env_software (ours);
        web_render_state = g_strdup ("software (web hardware acceleration is off)");
    } else {
        GString *acc = g_string_new (NULL);
        if (!dmabuf) {
            web_env_set (acc, "WEBKIT_DMABUF_RENDERER_FORCE_SHM", "1");
            web_env_set (acc, "WEBKIT_DMABUF_RENDERER_DISABLE_GBM", "1");
            web_env_set (acc, "WEBKIT_WEBGL_DISABLE_GBM", "1");
        }
        char *renderer = NULL, *why = NULL;
        gboolean ok = web_accel_probe (&renderer, &why);
        if (ok && !force && web_gl_is_software (renderer)) {
            ok = FALSE;
            why = g_strdup_printf ("WebKit would render with a software GL (%s), which is slower than "
                                   "its own CPU painting", renderer && *renderer ? renderer : "no WebGL");
        }
        if (ok) {
            g_message ("web: hardware acceleration on (%s; renderer: %s)",
                       dmabuf ? "frames as DMA-BUF textures" : "frames through shared memory",
                       renderer && *renderer ? renderer : "unknown");
            web_render_state = g_strdup_printf ("hardware (%s; %s)", dmabuf ? "DMA-BUF" : "shared memory",
                                                renderer && *renderer ? renderer : "renderer unknown");
            if (acc->len)
                g_string_append_printf (ours, ",%s", acc->str);
            g_setenv ("WALPE_WEB_PROBED", renderer ? renderer : "", TRUE);
            g_string_append (ours, ",WALPE_WEB_PROBED");
        } else {
            g_warning ("web: hardware acceleration was asked for but failed its start-up test (%s): "
                       "web wallpapers run in software mode", why ? why : "unknown reason");
            web_env_undo (acc->str);
            web_env_software (ours);
            web_render_state = g_strdup_printf ("software (hardware acceleration failed its start-up "
                                                "test: %s)", why ? why : "unknown reason");
        }
        g_string_free (acc, TRUE);
        g_free (renderer);
        g_free (why);
    }
    /* How often pages produce frames. WebKit steps requestAnimationFrame,
     * CSS animations and video frames from the refresh of the monitor it
     * thinks the (off-screen) page is on: on a 144 Hz monitor a page
     * rendered 144 frames a second for at most <fps limit> captures. Up
     * to a limit of 60 WebKit's own 60 Hz timer is used instead, and a
     * limit that divides 60 (30, 20, 15, ...) is handed to WebKit as its
     * display throttle, so the page renders exactly the frames that are
     * shown. Read once per process: a limit changed while the daemon runs
     * applies to requestAnimationFrame at once (web.c) and to everything
     * else at the next start. Above 60 pages follow the monitor as before. */
    if (fps <= 60) {
        web_env_set (ours, "WEBKIT_FORCE_VBLANK_TIMER", "1");
        if (fps >= 1 && fps < 60 && 60 % fps == 0) {
            char num[16];
            g_snprintf (num, sizeof num, "%d", fps);
            web_env_set (ours, "WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS", num);
        }
    }
    g_setenv ("WALPE_WEB_ENV_OURS", ours->str, TRUE);
    g_string_free (ours, TRUE);
}

static gboolean web_accel_restart (gpointer user)
{
    (void) user;
    g_message ("web hardware acceleration switched %s: restarting to apply it",
               D.cfg->web_accel ? "on" : "off");
    D.restart = TRUE;
    g_main_loop_quit (D.loop);
    return G_SOURCE_REMOVE;
}
#endif

int main (int argc, char **argv)
{
    gboolean monitor_all = TRUE;
    int monitor_idx = 0;
    gboolean windowed = FALSE;
    g_mutex_init (&Sw.lock);
    {
        /* restarted in place: the control socket of the previous image.
         * Before any thread exists (unsetenv) and before orig_env is taken */
        const char *lf = g_getenv ("WALPE_IPC_FD");
        if (lf && *lf)
            walpe_ipc_adopt_fd (atoi (lf));
        g_unsetenv ("WALPE_IPC_FD");
        Sw.orig_env = g_get_environ ();
        Sw.argv = g_strdupv (argv);
        /* by its real path, so that the restarted process is still called
         * walpe-daemon (pkill, ps); /proc/self/exe itself when that path
         * is gone (binary replaced while running) */
        Sw.exe = g_file_read_link ("/proc/self/exe", NULL);
        if (!Sw.exe || !g_file_test (Sw.exe, G_FILE_TEST_IS_EXECUTABLE)) {
            g_free (Sw.exe);
            Sw.exe = g_strdup ("/proc/self/exe");
        }
    }
    for (int i = 1; i < argc; i++)
        if (!g_strcmp0 (argv[i], "--sw-probe")) {
            /* the software rendering test, run by sw_setup() in a child */
            int rc = sw_probe_run ();
            fflush (NULL);
            _exit (rc);
        }
    for (int i = 1; i < argc; i++) {
        if (!g_strcmp0 (argv[i], "--windowed"))
            windowed = TRUE;
        else if (!g_strcmp0 (argv[i], "--monitor") && i + 1 < argc) {
            monitor_all = FALSE;
            monitor_idx = atoi (argv[++i]);
        } else if (!g_strcmp0 (argv[i], "--fps") && i + 1 < argc)
            ; /* read after config load */
        else if (!g_strcmp0 (argv[i], "-h") || !g_strcmp0 (argv[i], "--help")) {
            g_print ("usage: walpe-daemon [--windowed] [--monitor N] [--fps N]\n"
                     "  WALPE_SOFTWARE=1|0 in the environment: render on the CPU (Mesa llvmpipe)\n"
                     "  or on the GPU, whatever the saved setting says "
                     "(walpe-ctl set software=on|off)\n");
            return 0;
        }
    }

    g_log_set_default_handler (log_guard, NULL);   /* rate limit + file size cap */
    /* libmpv refuses to initialize with a non-C LC_NUMERIC */
    setlocale (LC_NUMERIC, "C");
    /* threads + libmpv fragment glibc arenas (one 64M arena per thread);
     * capping them keeps RSS flat for a long-running daemon */
    mallopt (M_ARENA_MAX, 2);
    /* Large buffers get their own mapping and go back to the system when
     * they are freed. glibc raises its mapping threshold to the size of
     * every large block that is freed (up to 32 MiB): after the first
     * decoded picture, pictures, decoder buffers and - on a software GL -
     * textures came from the heap, where freeing them leaves holes that
     * stay resident (3163060610 on llvmpipe: 36 MiB of the steady RSS and
     * 40 MiB of the load-time peak). A fixed threshold switches that
     * adaptation off. WALPE_MALLOC_MMAP_MB=<MiB>: another threshold (at most
     * 32); 0 = glibc's own behaviour, as before. */
    {
        const char *e = g_getenv ("WALPE_MALLOC_MMAP_MB");
        double mb = e && *e ? g_ascii_strtod (e, NULL) : 4.0;
        if (mb > 0)
            mallopt (M_MMAP_THRESHOLD, (int) (CLAMP (mb, 0.125, 32.0) * 1048576.0));
    }

    D.cfg = walpe_config_load ();
    {
        /* ~/.cache/walpe holds the unpacked wallpapers (by Workshop id),
         * converted videos, profile reports and the log: the user's own
         * business. Whoever created it first decided its mode (the GUI
         * 0700 for the log, the package cache 0755 with all its parents):
         * made here, private, before anything else can. */
        char *cd = g_build_filename (g_get_user_cache_dir (), "walpe", NULL);
        GStatBuf st;
        g_mkdir_with_parents (cd, 0700);
        if (g_lstat (cd, &st) == 0 && S_ISDIR (st.st_mode) && st.st_uid == getuid () &&
            (st.st_mode & 077))
            g_chmod (cd, 0700);
        g_free (cd);
    }
    walpe_video_set_hwdec (D.cfg->video_hwdec);
    walpe_video_set_limit (D.cfg->video_limit);
    if (D.cfg->video_upload && !walpe_video_set_upload (D.cfg->video_upload))
        g_warning ("config: video_upload \"%s\" is not a known upload path: legacy is used",
                   D.cfg->video_upload);
    if (D.cfg->fps_migrated) {
        /* config.h walpe_config_migrate_fps: a frame rate limit left over
         * from an older version's default (30, then 200 = "the monitor's
         * rate"). Saved now (before --fps), so it happens once */
        g_message ("config: frame rate limit %d was the default of an older version, not a "
                   "choice: now %d (walpe-ctl set fps=N or the GUI to change it; a value set "
                   "from now on is kept)", D.cfg->fps_migrated_from, D.cfg->fps);
        walpe_config_save (D.cfg);
    }
    present_settings_load ();   /* gpu.conf / WALPE_*: before any thread, before EGL */
    D.no_refresh_cap = g_getenv ("WALPE_NO_REFRESH_CAP") != NULL;
    D.leader_first = !g_strcmp0 (g_getenv ("WALPE_LEADER"), "first");
    D.fake_vsync = g_getenv ("WALPE_FAKE_VSYNC") != NULL;
    {
        const char *ld = g_getenv ("WALPE_LOAD_DELAY_MS");
        D.load_delay_us = ld && *ld ? (gulong) CLAMP (g_ascii_strtoll (ld, NULL, 10), 0, 60000) * 1000 : 0;
        const char *ff = g_getenv ("WALPE_FIRST_FRAME_S");
        D.first_frame_ticks = 4 * (ff && *ff ? (int) CLAMP (g_ascii_strtoll (ff, NULL, 10), 0, 3600) : 20);
    }
    {
        const char *fl = g_getenv ("WALPE_FPS_LOG");
        D.fpslog_s = fl && *fl ? CLAMP (g_ascii_strtod (fl, NULL), 0.0, 3600.0) : 0.0;
    }
    walpe_we_assets_set_configured (D.cfg->we_assets);
    Imp.percent = -1;
    /* diagnostics switches: read once here, never getenv per frame from
     * render threads (getenv/setenv race) */
    D.probe = g_getenv ("WALPE_PROBE") != NULL;
    D.cadence = g_getenv ("WALPE_CADENCE") != NULL;
    for (int i = 1; i < argc; i++)
        if (!g_strcmp0 (argv[i], "--fps") && i + 1 < argc) {
            /* the value in a local first: CLAMP() evaluates its argument
             * more than once, and with argv[++i] inside it that walked past
             * argc (atoi (NULL): `walpe-daemon --fps N` died at start-up) */
            int fps = atoi (argv[++i]);
            D.cfg->fps = CLAMP (fps, 5, 200);
        }

    /* "Render on CPU": must be decided before the first EGL call and
     * before any thread exists (it sets environment variables). The
     * WebKit start-up test child inherits the result. */
    {
        gboolean web_probe = FALSE;
        for (int i = 1; i < argc; i++)
            web_probe |= !g_strcmp0 (argv[i], "--web-probe");
        if (!web_probe)
            sw_setup ();
    }

#ifdef HAVE_WEB
    /* `walpe-daemon --web-probe`: the WebKit start-up test run in a child
     * (by web.c before WebKit is started inside the daemon, by
     * web_env_setup to try hardware acceleration). It keeps the
     * environment its parent gave it. */
    for (int i = 1; i < argc; i++)
        if (!g_strcmp0 (argv[i], "--web-probe")) {
            int rc = walpe_web_probe_run ();
            fflush (NULL);
            _exit (rc);
        }
    /* before any thread exists: software or hardware rendering of pages */
    web_env_setup (D.cfg->web_accel, D.cfg->fps);
#endif
    /* a peer that closes a socket/pipe mid-write must not kill the daemon */
    signal (SIGPIPE, SIG_IGN);
    /* render threads present through EGL on the same Display the main
     * thread polls (XPending/XQueryPointer): Xlib must be thread-safe */
    XInitThreads ();
    XSetErrorHandler (walpe_xerror); /* never let a BadWindow kill us */
    xio_sock_path = walpe_socket_path ();
    xio_marker_path = loadmark_file ();
    XSetIOErrorHandler (walpe_xio_error);
    D.xd = XOpenDisplay (NULL);
    /* the software rendering test was an X client of its own. Where it was
     * the only one (nothing else running yet, Xvfb in tests), a server
     * without -noreset resets itself when that client leaves and refuses
     * connections for a moment */
    for (int i = 0; !D.xd && Sw.wanted && i < 30; i++) {
        g_usleep (100 * 1000);
        D.xd = XOpenDisplay (NULL);
    }
    if (!D.xd) {
        g_printerr ("cannot open X display\n");
        return 1;
    }
    D.root = DefaultRootWindow (D.xd);
    /* EGL and the render threads get an X connection of their own. On a
     * shared one every buffer swap's traffic (replies, events of the GL
     * driver) made the socket readable and woke the main loop, which then
     * found nothing of its own: ~200 wake-ups a second with one scene at
     * 60 fps on two monitors, and the swaps' round trips parked main-loop
     * events in Xlib's queue (late clicks). WALPE_ONE_XCONN=1 = one
     * connection for everything, as before. */
    D.egl_xd = g_getenv ("WALPE_ONE_XCONN") ? NULL : XOpenDisplay (NULL);
    if (!D.egl_xd)
        D.egl_xd = D.xd;
    D.loop = g_main_loop_new (NULL, FALSE);
    /* systemctl --user stop / Ctrl+C: the same clean shutdown as `quit`
     * (kills a running ffmpeg import / steamcmd and their partial output,
     * removes the socket) instead of dying with them orphaned. Installed
     * before the socket and the first load exist: a signal during start-up
     * is acted on as soon as the main loop runs, instead of killing the
     * process with the socket and the crash-loop marker left behind. */
    /* (SIGHUP: the terminal the daemon was started from was closed) */
    term_signals_init ();
    int shape_ev, shape_err;
    have_shape = XShapeQueryExtension (D.xd, &shape_ev, &shape_err);
    Atom wmcheck = XInternAtom (D.xd, "_NET_SUPPORTING_WM_CHECK", True);
    if (wmcheck != None) {
        guchar *data = NULL;
        gulong n = window_prop32 (D.root, wmcheck, XA_WINDOW, 1, &data);
        if (data) {
            Window w = n == 1 ? *(Window *) data : None;
            XFree (data);
            XWindowAttributes wa;
            D.has_wm = (w != None && XGetWindowAttributes (D.xd, w, &wa));
        }
    }
    g_message ("walpe-daemon %s \"%s\" up (wm=%s, backend=%s)", WALPE_VERSION, WALPE_CODENAME,
               D.has_wm ? "yes" : "no", D.cfg->video_backend);

    /* monitor layout: rectangles, output names, refresh rates */
    layout_detect (&layout_now);
    int nmon = layout_now.n;
    WalpeRect *geoms = layout_now.geom;
    char **names = layout_now.name;
    double *hzs = layout_now.hz;
    for (int i = 0; i < nmon; i++) {
        if (hzs[i] > 0.0)
            g_message ("monitor %d: %s %dx%d+%d+%d, %.2f Hz", i, names[i], geoms[i].width,
                       geoms[i].height, geoms[i].x, geoms[i].y, hzs[i]);
        else
            g_message ("monitor %d: %s %dx%d+%d+%d, refresh rate unknown", i, names[i],
                       geoms[i].width, geoms[i].height, geoms[i].x, geoms[i].y);
    }

    char *sock = walpe_socket_path ();
    if (!walpe_ipc_serve (sock, ipc_handler, NULL)) {
        g_printerr ("the control socket could not be set up (another daemon is running, or "
                    "see the warning above); exit\n");
        g_free (sock);
        return 1;
    }
    g_free (sock);

    if (!init_egl ()) {
        g_printerr ("EGL init failed; cannot render\n");
        return 1;
    }

    if (windowed) {
        WalpeRect g = { .width = 960, .height = 540 };
        make_monitor (&g, 0, FALSE, "windowed"); /* normal window: WM tiles it, on top */
    } else {
        for (int i = 0; i < nmon; i++)
            if (monitor_all || i == monitor_idx) {
                Mon *nm = make_monitor (&geoms[i], i, TRUE, names[i]);
                if (nm) {
                    nm->hz = hzs[i];
                    D.max_hz = MAX (D.max_hz, hzs[i]);
                }
            }
    }

    present_init ();   /* swap interval per monitor, before its thread starts */
    /* one render thread per monitor: owns its EGL context for life */
    ctx_expected = (int) g_list_length (D.mons);
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        m->pause_wanted = m->self_paused = m->own_paused = want_pause_mon (m);
        m->fps = m->fps_cap = mon_fps_cap (m);
        walpe_config_quality_for (D.cfg, mon_quality_path (m), &m->quality);
        /* GPUPROF: a name per render thread, so that top / the GPU report
         * script can tell them from the main and the decode threads */
        char tname[16];
        g_snprintf (tname, sizeof (tname), "walpe-mon%d", m->index);
        m->thread = g_thread_new (tname, mon_thread, m);
    }
    fx_set_cache_enabled (D.cfg->fx_cache);
    fx_set_specialize (D.cfg->fx_specialize);       /* effect pass programs (fx_specialize.h) */
    fx_set_shader_quality (D.cfg->fx_quality);
    particles_apply ();
    loadmark_check ();   /* crash-loop guard: skip what killed the last daemon */
    apply_wallpapers (NULL, NULL, FALSE, FALSE);   /* each monitor's saved wallpaper */
    conv_cache_tidy ();
    {
        /* the same for `walpe-ctl add` / the window's Import: an import
         * that died with its daemon left <library>/imported/<name>.mp4.part
         * behind for good (nothing ever looked there) */
        char *lib = walpe_expand_path (D.cfg->wallpapers_dir ? D.cfg->wallpapers_dir
                                                             : walpe_default_dir ());
        char *impdir = lib ? g_build_filename (lib, "imported", NULL) : NULL;
        guint n = walpe_import_tidy_parts (impdir);
        if (n)
            g_message ("import: removed %u unfinished import(s) in %s", n, impdir);
        g_free (impdir);
        g_free (lib);
    }
    convert_scan ();     /* a saved video wallpaper whose converted copy is gone */

    /* StructureNotify: the root's own ConfigureNotify (screen resized) */
    XSelectInput (D.xd, D.root, PropertyChangeMask | SubstructureNotifyMask | StructureNotifyMask);
#ifdef HAVE_XRANDR
    {
        int rerr = 0;
        if (!windowed && XRRQueryExtension (D.xd, &rr_event_base, &rerr))
            XRRSelectInput (D.xd, D.root, RRScreenChangeNotifyMask);
        else
            rr_event_base = -1;
    }
#endif
    layout_watch = !windowed;
#ifdef HAVE_XI2
    xi2_init ();
#endif
    x_event_source_add ();   /* socket + Xlib's queue (not a bare fd watch) */
    update_active_window ();
    recompute_covered ();
    D.windowed = windowed;
    vis_apply_setting ();          /* "Skip hidden areas": the tracker, when switched on */
    /* No periodic timer runs from here on (there were four: pointer
     * recheck 1 s, display sleep 1 s, cover re-scan 2 s, re-lower 5 s). An
     * idle daemon - static wallpaper, paused, covered, display off - sleeps
     * in poll() until an X event, an IPC request or a signal arrives. */
    D.stack_dirty = !windowed;     /* one look at the stacking order now */
    restack_check ();
    pointer_update ();             /* tracks once content needs it */
    if (g_getenv ("G_MESSAGES_DEBUG"))   /* debug-only output */
        g_timeout_add_seconds (2, stats_tick, NULL);
    sleep_watch_init ();
    power_watch_update ();         /* only when a battery rule is set */
    {
        /* GPUPROF */
        const char *gp = g_getenv ("WALPE_GPU_PROFILE");
        if (gp && *gp) {
            /* not CLAMP: "nan" passes it and the casts below are undefined */
            double secs = g_ascii_strtod (gp, NULL);
            secs = secs >= 1.0 ? MIN (secs, 600.0) : 1.0;
            const char *gd = g_getenv ("WALPE_GPU_PROFILE_DELAY");
            double delay = gd && *gd ? g_ascii_strtod (gd, NULL) : 5.0;
            delay = delay >= 0.0 ? MIN (delay, 600.0) : 0.0;
            g_timeout_add ((guint) (delay * 1000.0), prof_env_start,
                           GINT_TO_POINTER ((int) (secs * 1000.0)));
        }
    }
    if (D.fpslog_s > 0) {
        D.fpslog_us = g_get_monotonic_time ();
        g_timeout_add ((guint) MAX (200.0, D.fpslog_s * 1000.0), fpslog_tick, NULL);
    }
    sleep_recheck ();

    g_main_loop_run (D.loop);

    /* release the socket BEFORE the (slow) EGL/X teardown: if a new daemon
     * starts during shutdown, our late unlink would otherwise delete its
     * freshly bound socket and leave it deaf */
    const gboolean restarting = (Sw.restart || D.restart) && !term_signal;
    const gboolean sock_kept = restarting && walpe_ipc_listen_fd () >= 0;
    if (!sock_kept) {
        char *sp = walpe_socket_path ();
        walpe_ipc_release (sp);   /* socket + single-instance lock */
        g_free (sp);
    }   /* restarting in place: the socket is handed to the new image */
    loadmark_clear (NULL);   /* clean exit: nothing died while loading */
    if (D.vis) {   /* "Skip hidden areas" */
        walpe_vistrack_free (D.vis);
        D.vis = NULL;
    }
    if (Imp.job) /* kills a running ffmpeg + removes its partial output */
        walpe_import_free (Imp.job);
    if (Ws.job)  /* kills steamcmd + removes a partial copy */
        walpe_workshop_free (Ws.job);
    /* two phases: every render thread is stopped and joined before any
     * monitor is freed. One by one, a shared-rendering leader that was
     * still running called request_redraw on a follower that had already
     * been joined and freed (heap-use-after-free when the follower comes
     * first in the list: the faster monitor leads). */
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        g_mutex_lock (&m->lock);
        m->quitting = TRUE;
        g_cond_broadcast (&m->cond);
        g_mutex_unlock (&m->lock);
    }
    for (GList *l = D.mons; l; l = l->next)
        g_thread_join (((Mon *) l->data)->thread); /* thread frees its content + context */
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        if (m->expose_timer_id)
            g_source_remove (m->expose_timer_id);
        eglDestroySurface (D.egl_dpy, m->surf);
        XDestroyWindow (D.xd, m->xid);
        g_mutex_clear (&m->lock);
        g_cond_clear (&m->cond);
        g_free (m->name);
        g_free (m->want_path);
        g_free (m->loaded_path);
        g_free (m->load_error);
        walpe_region_clear (&m->vis);   /* "Skip hidden areas" */
        g_free (m->qdescribe);
        g_free (m->good_path);
        g_free (m->reopen_for);
        g_free (m->restored);
        g_free (m->load_path);       /* a load its thread never took */
        if (m->ptr_q)                /* (LeakSanitizer: one array per monitor that saw the pointer) */
            g_array_unref (m->ptr_q);
        g_free (m);
    }
    walpe_open_undo_clear (&open_undo);
    g_list_free (D.mons);
    eglTerminate (D.egl_dpy);
    if (D.egl_xd != D.xd)
        XCloseDisplay (D.egl_xd);
    XCloseDisplay (D.xd);
    if (restarting) {
        /* "Render on CPU" changed, the monitor layout changed or web
         * hardware acceleration was switched: the same daemon again (same
         * process id), with the saved wallpapers; the socket was kept, the
         * new image adopts it. Unless a signal told the daemon to stop
         * while it was taking itself apart: then it stops. */
        if (term_pending ()) {
            g_message ("signal %d during the restart: not restarting", (int) term_signal);
            if (sock_kept) {
                char *sp = walpe_socket_path ();
                walpe_ipc_release (sp);
                g_free (sp);
            }
            return 0;
        }
        restart_exec (D.restart);   /* returns only when it failed */
        return 1;
    }
    return 0;
}
