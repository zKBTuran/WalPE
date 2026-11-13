/* walpe-daemon: renders the active wallpaper on one "desktop" surface per
 * monitor, with EGL, no GTK in the render path. The window system is
 * behind platform/platform.h: on X11 each monitor gets an override-redirect
 * window (invisible to window managers — dwm cannot tile or raise it) that
 * is directly an EGL surface; on Wayland a layer-shell surface of the
 * background layer. This file holds no window-system call.
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

#include "common/lazy.h"
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/scenenode_effects.h" /* fx_set_cache_enabled */
#include "common/fluidsim.h"           /* fluid_quality: the level names */
#include "engine/gpuprof.h"           /* GPUPROF: walpe-ctl profile */
#include "engine/weparticles.h"       /* particle settings */
#include "engine/modules.h"           /* walpe_modules_status */
#include "engine/memreport.h"         /* walpe-ctl mem */
#include "engine/features.h"          /* renderer feature switches */
#include "engine/scenesound.h"        /* wallpaper sound: allowed in this program only */
#include "engine/mp4demux.h"          /* open: can this video be played? */
#include "common/config.h"
#include "common/ipc.h"
#include "signals/providers.h"        /* the idle rule: walpe_signal_get, its timeout */
#include "common/util.h"
#include "common/sourceid.h"   /* which tree this daemon was built from: status, diag, the log */
#include "common/import.h"
#include "common/scenevideo.h" /* converted copies of videos inside scenes */
#include "common/library.h"   /* walpe_config_forget_path */
#include "common/audio.h"
#include "common/media.h"
#include "common/workshop.h"
#include "common/weassets.h"
#include "common/swrender.h"
#include "common/gpusel.h"    /* which GPU renders */
#include "engine/glcaps.h"    /* what its driver offers */
#include "common/glapi.h"
#include "engine/glfeat.h"
#include "common/pace.h"
#include "common/restore.h"
#include "common/present.h"
#include "common/region.h"
#include "engine/clip.h"
#include "daemon/vistrack.h"
#include "platform/platform.h"
#include "signals/providers.h"        /* logind and the compositor's idle / window-list providers */
#ifdef HAVE_WAYLAND
#include "platform/wl_gnome.h"        /* the process GNOME Shell 46 to 48 starts for the connection */
#include "platform/wayland_logic.h"   /* wl_lost_decide: a compositor that does not stay */
#endif
#ifdef HAVE_WEB
#include "engine/web.h"
#endif


/* pointer position, one query of the window system per tick, only while a visible
 * wallpaper follows the pointer AND the pointer moves: at the frame rate
 * (30..144 Hz). At the old fixed 30 Hz, parallax and cursor effects moved
 * in 30 steps a second however fast the scene rendered. */
#define MOUSE_POLL_MS mouse_poll_ms ()
static guint mouse_poll_ms (void);

typedef struct { int x, y, width, height; } WalpeRect;

/* Following a change of the outputs in place (layout_follow, further down)
 * needs a window system whose surfaces last while their output changes:
 * Wayland (platform.h). A build without that backend holds none of the
 * code for it: no field of Mon, nothing on a render thread's path. */
#ifdef HAVE_WAYLAND
#define WALPE_FOLLOW 1
/* render thread: the size of its surface, which is what it draws at */
#define MON_W(m) ((m)->rt_w)
#define MON_H(m) ((m)->rt_h)
#else
#define MON_W(m) ((m)->geom.width)
#define MON_H(m) ((m)->geom.height)
#endif

typedef struct Mon {
    int index;
    WalpeSurface *ps;        /* the platform's surface: the native window `surf` is
                              * on (set once, before the render thread starts) */
    guint32 out_id;          /* WalpeOutput.id of its output: per-output pause
                              * signals are reported under it */
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
    WalpeRect geom;          /* monitor geometry (read-only after init; where an output
                              * is followed in place, layout_follow: the main thread's,
                              * and the render thread reads MON_W / MON_H) */
    float mx, my;            /* parallax mouse, written by main, read by thread */
    guint render_count;      /* rendered frames (diagnostics) */
    /* swaps that fail (mon_swap_failed): render thread, except swap_lost */
    guint swap_fails;        /* failed swaps in a row (0 = the last one went through) */
    gint64 swap_warn_us;     /* when the last "eglSwapBuffers failed" line was written */
    EGLint swap_err;         /* a failure made by WALPE_SWAP_FAIL: the error it stands for */
    gint swap_lost;          /* atomic, main reads it: the EGL error of SWAP_LOST_AFTER
                              * failed swaps in a row that say the context or the
                              * surface is gone (0 = none): status "swap_error" */
    int last_w, last_h;      /* last rendered size (diagnostics) */
    gboolean covered;        /* main thread: client windows hide this monitor */
    gboolean focus_app;      /* main: the focused window is an application's, on this monitor */
    gboolean fs_here;        /* main: the focused fullscreen window (D.fs_active) is on
                              * this monitor, or nobody can tell which one it is on */
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
    gint thread_ended;       /* atomic: its render thread has left its loop and freed
                              * what it owned (mons_wait_end, load_held_check) */
    char *given_path;        /* main: the wallpaper it was handed last (mon_assign) */
    char *busy_path;         /* main: ... and the one before, while its thread has not
                              * taken that one: what the thread is in (mon_thread_in) */
    gboolean in_load;        /* thread -> main (lock): its thread is still setting up, or is
                              * inside the load of a wallpaper (mon_do_load): however long
                              * that takes, it is not a frame that does not end */
    gint64 load_end_us;      /* thread -> main (lock): when it left that (monotonic) */
    gboolean reload_wanted;  /* main: load its wallpaper again at the next
                              * reload_flush () (scenes_reload_mark) */
    gboolean blank_req;      /* main -> thread (lock): that failure is final: black */
    gboolean prof_sync;      /* GPUPROF main -> thread (lock): call mon_prof_sync */
    guint fpslog_count;      /* main: render_count at the last WALPE_FPS_LOG line */
    /* "Adjust layers" (layers_ipc.h), main -> thread (lock) */
    gboolean force_frame;    /* draw one frame now, also while paused / covered */
    gint64 force_until_us;   /* following: present the leader's frames until then,
                              * also while paused */
    gboolean present_req;    /* leader's thread -> follower (lock): present the leader's
                              * newest frame once, also while paused (sf_show_followers) */
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
    gboolean follow_idle;    /* lock: the render thread follows a leader, is paused
                              * and owes its window no frame: it presents nothing
                              * until something else wakes it (the resume, an
                              * Expose, a forced frame), so a frame published
                              * meanwhile needs no wake-up either */
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
    /* render resolution (common/resolution.h): the size this output's
     * wallpaper is rendered at, the texture limit it is loaded with and
     * the box of its video's converted copy */
    int res_w, res_h;        /* main -> thread (lock): the render size (the output's
                              * own mode at "native") */
    int tex_limit;           /* main -> thread (lock): WALPE_TEXLIMIT_* / pixels, as
                              * it holds for the wallpaper it shows (res_texlimit_for) */
    gboolean res_changed;    /* main -> thread (lock): one of them changed for the
                              * running wallpaper: a frame now */
    WalpeVideoBox load_box;  /* main -> thread (lock), with load_path */
    WalpeVideoBox vbox;      /* main -> thread (lock): the same box as it is now, for
                              * the videos inside a wallpaper that is running
                              * (video_boxes_update; WalpeContent.video_box_w / _h) */
    int rt_res_w, rt_res_h;  /* render thread: as last taken */
    int rt_tex_limit;
    WalpeVideoBox rt_box;
    WalpeVideoBox rt_vbox;
    char *res_note;          /* main: why the chosen size is not used (NULL = it is) */
    /* `walpe-ctl mem` (engine/memreport.h). Asked for by the main thread,
     * written by the render thread between two frames (its GL objects are
     * its context's, its content is its own): mem_req = the number of the
     * report wanted (0 = none), mem_done = the number mem_json answers */
    guint mem_req, mem_done; /* lock */
    gboolean mem_deep;       /* lock */
    char *mem_json;          /* lock: thread -> main */
    GCond mem_cond;          /* mem_done changed */
    /* render thread: what happened to this monitor since the daemon started */
    guint n_loads, n_pauses, n_resumes, n_paused_frames, n_idle_releases, n_roles;
#ifdef WALPE_FOLLOW
    /* an output followed in place (layout_follow) */
    int rt_w, rt_h;          /* render thread: the size of its surface (MON_W, MON_H):
                              * geom.width / .height as it last took them */
    gboolean fit_req;        /* main -> thread (lock), with res_changed: its output has
                              * another size or scale: the surface is fitted to it
                              * and rt_w / rt_h are taken anew */
    gboolean parked;         /* lock: it has no output: its thread holds no wallpaper,
                              * its context is current on nothing, and it waits */
    gboolean park_told;      /* main: ... and has said so (mon_parked_cb) */
    gboolean gone_said;      /* render thread: it has said that it presents nothing
                              * any more, the display being gone (mon_swap_failed) */
    guint life;              /* main: the outputs it has had; a load's note carries it,
                              * and one of an earlier output is not acted on */
#endif
} Mon;

typedef struct { int kind; float x, y; } PtrMsg;

static struct {
    WalpeConfig *cfg;
    GList *mons;
    int backend;             /* WalpeBackend: the window system in use (fixed at start-up) */
    EGLDisplay egl_dpy;
    EGLConfig egl_conf;
    gboolean fs_active;      /* fullscreen window has focus (on whichever monitor:
                              * Mon.fs_here) */
    gboolean asleep;         /* DPMS power-save or screensaver active */
    gboolean idle;           /* main: no user input for a while (a provider's
                              * report; no rule of this file reads it) */
    gboolean display_lost;   /* main: the connection to the window system is gone */
    gboolean on_battery;     /* running on battery (followed only while a battery rule is on) */
    gint egl_is_es;          /* set by monitor threads after ctx creation */
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
    guint swap_fail_at;      /* WALPE_SWAP_FAIL (tests): every monitor's swaps fail from
                              * this one on (0 = off) ... */
    EGLint swap_fail_err;    /* ... with this EGL error */
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
/* the GPU choice (gpu_glue.h, included below init_egl) */
static void gpu_note_egl (EGLDisplay dpy);
static void gpu_note_software_renderer (void);
static void gpu_status_json (JsonBuilder *b);
static gboolean gpu_ipc_set (JsonObject *o, gboolean busy, gboolean *restart, char **note,
                             char **err);
static void gpu_schedule_restart (void);
/* (the --gpu-probe child opens its display as the --sw-probe child does) */
static gboolean probe_open (int argc, char **argv, char **err);
static void res_note_gl (void);     /* render resolution: what the GL can be asked for */
static gboolean res_gl_known_cb (gpointer user);

/* forward decls */
static void request_redraw (gpointer user);
#ifdef HAVE_WEB
static char *web_render_state;      /* status "web_render" (web_env_setup) */
static gboolean web_accel_restart (gpointer user);
#endif
/* "web_unload": the web module leaves again (web_unload_due) */
static gboolean web_unload_due (gboolean changed);
static void web_unload_hidden (void);   /* ... at a moment nobody sees, when it was put off */
static gboolean web_unload_restart;     /* main: the restart under way is that one */
static gint64 web_unloaded_us;          /* monotonic time of the last one (main (), 0 = none) */
static gboolean swap_restart;           /* main: the restart under way follows swaps that fail */
static gint64 swap_restarted_us;        /* monotonic time of the last such restart (main (), 0 = none) */
static void pointer_update (void);
static void pointer_repace (void);
static char *restart_failed;            /* main: a restart in place was wanted and could not be
                                         * begun: why (status "restart_failed"; NULL = none) */
static gboolean restart_begin (const char *what);
static void follow_pace_update (void);
static void vis_push (void);          /* "Skip hidden areas" */
static void state_touch (void);
/* What the daemon asks the window system to tell it about, each only
 * while something needs it (every event wakes the daemon): button events
 * while a wallpaper takes clicks (cursor scripts, web pages), and one
 * motion event when a pointer that was at rest moves again (a wallpaper
 * that follows it). How is the backend's business (X11: XInput2 raw
 * events on the root window). */
static gboolean in_want_buttons, in_want_motion;
static void input_apply (void)
{
    walpe_platform_input_select (in_want_buttons, in_want_motion);
}

/* which window system this process draws on (common/backend.h): decided
 * once at start-up, before anything connects (backend_decide) */
static struct {
    const char *source;     /* who asked: "--backend", "WALPE_BACKEND", "config", "default" */
    char *why;              /* the sentence of the log line and of `status` */
    char *lost;             /* this image was started in place after the display
                             * went away: the backend the image before ran on
                             * (NULL = any other start) */
    char *display_node;     /* Wayland, when the compositor told: the DRM device
                             * it composes with (NULL = unknown) */
    int ipc_fd;             /* the control socket handed over by the image before, -1 = none */
    /* the file of that socket as it was when this image began (start_failed) */
    gboolean ipc_file_known;
    dev_t ipc_dev;
    ino_t ipc_ino;
    struct timespec ipc_ctim;
#ifdef HAVE_WAYLAND
    WlStays stays;          /* Wayland: since when the display is there, and how many
                             * times in a row it was not for long (lost_decide) */
#endif
} Bk = { .ipc_fd = -1 };

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
    int min_iv, max_iv;      /* EGL_MIN/MAX_SWAP_INTERVAL of the config */
    gboolean has_damage;     /* EGL_KHR/EXT_swap_buffers_with_damage */
    gboolean damage_khr;
    gboolean swap_damage;    /* swap_damage=on: swaps carry their damage */
    gint64 idle_release_us;  /* idle_release (seconds), 0 = off */
    gboolean own_display;    /* egl_display=own: what this build does by default
                              * (the X11 backend's EGL connection); kept as a known key */
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
    if (walpe_platform_paces ()) {
        /* the backend paces frames itself (Wayland: frame callbacks), so
         * both intervals exist whatever the EGL config offers */
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
               walpe_platform_egl_note (), (int) lo, (int) hi,
               D.fake_vsync ? " (WALPE_FAKE_VSYNC: emulated)" : "",
               P.has_damage ? (P.swap_damage ? "available, used (swap_damage = on)"
                                               : "available, not used (gpu.conf: swap_damage = on "
                                                 "would use it)")
                            : "not available",
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
    return walpe_platform_composited ();
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
        if (walpe_platform_paces ())
            why = iv ? "the next frame waits until the compositor took this one (frame callback)"
                     : "the frame clock paces, frame callbacks are not waited for";
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
    if (any_auto && !present_timer && D.backend == WALPE_BACKEND_X11) {
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
    walpe_platform_present_log ();
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
    /* what an interval means is the backend's: on X11 it is EGL's own swap
     * interval; a backend that paces itself has EGL never wait and does
     * the wait of interval 1 in walpe_surface_swapped */
    int egl_iv = walpe_surface_set_interval (m->ps, iv);
    if (!eglSwapInterval (D.egl_dpy, egl_iv) && g_atomic_int_compare_and_exchange (&P.warned_iv, 0, 1))
        g_warning ("eglSwapInterval (%d) failed on monitor %d: 0x%x", egl_iv, m->index, eglGetError ());
    m->present_iv_applied = iv;
}

/* render thread: present the back buffer of its window. The swap flushes
 * the context itself (EGL: "eglSwapBuffers performs an implicit flush
 * operation on the context"), so no glFlush / glFinish goes before it.
 * swap_damage: the damage goes with the swap where the EGL can take it;
 * it is the whole surface, because no content reports less yet. */
/* (not an EGL error: mon_swap asked nothing of EGL, the display is gone) */
#define SWAP_ERR_NO_DISPLAY ((EGLint) -1)

static EGLBoolean mon_swap (Mon *m)
{
    mon_present_apply (m);
    if (G_UNLIKELY (D.swap_fail_at) && m->render_count + 1 >= D.swap_fail_at) {
        m->swap_err = D.swap_fail_err;      /* WALPE_SWAP_FAIL (tests): this swap "fails" */
        return EGL_FALSE;
    }
#ifdef HAVE_WAYLAND
    /* The compositor is gone and the main thread is on its way to end the
     * daemon or to restart it; until it has told this thread, the thread
     * goes on with its frames. A swap on a display that is gone is not for
     * EGL to sort out: Mesa's software path (26.2, llvmpipe) reads through
     * a back buffer it could not get any more and the daemon dies of
     * SIGSEGV in eglSwapBuffers - seen once in some hundred lost
     * compositors, each time in the frame after the one whose wait for
     * the compositor had found the connection gone. What is known to be
     * gone is not swapped on. (What goes in the middle of a swap is still
     * EGL's.) */
    if (G_UNLIKELY (walpe_platform_display_gone ())) {
        m->swap_err = SWAP_ERR_NO_DISPLAY;
        return EGL_FALSE;
    }
#endif
    walpe_surface_pre_swap (m->ps);
    if (P.swap_damage && P.has_damage) {
        EGLint r[4] = { 0, 0, MON_W (m), MON_H (m) };
        return P.damage_khr ? eglSwapBuffersWithDamageKHR (D.egl_dpy, m->surf, r, 1)
                            : eglSwapBuffersWithDamageEXT (D.egl_dpy, m->surf, r, 1);
    }
    return eglSwapBuffers (D.egl_dpy, m->surf);
}

/* A swap that fails.
 *
 * eglSwapBuffers fails once in a while for reasons that pass. It also fails
 * for good: after a suspend / resume or a reset of the GPU the driver may
 * take the GL context away (EGL_CONTEXT_LOST), and a window or surface that
 * is gone gives EGL_BAD_NATIVE_WINDOW / EGL_BAD_SURFACE. Every frame then
 * wrote "eglSwapBuffers failed" (as fast as the log's rate limit lets
 * through), the monitor kept its last picture or went black, and status
 * said nothing.
 *
 * Now: the line of a failure that may pass is written at most once in
 * SWAP_WARN_S seconds per monitor.
 * When SWAP_LOST_AFTER swaps in a row have failed with one of the three
 * errors above, the monitor is "lost": status carries "swap_error" for it
 * (until a swap goes through again), ONE line says so, and the daemon
 * restarts in place - the way it follows a surface the window system took
 * away (layout_surface_lost: not during an import or a download, restarts
 * ten seconds apart) - and makes its contexts and surfaces anew. Not when
 * it has restarted for this less than SWAP_RESTART_GAP_S ago: a loss the
 * restart does not cure must not make the daemon restart for ever. The
 * line then says that it stays as it is.
 *
 * Nothing of this is on the path of a swap that goes through, save one
 * test of swap_fails. WALPE_SWAP_FAIL=N[,always][,0xERROR] (tests): every
 * monitor's swaps fail from its N-th on, with EGL_CONTEXT_LOST or the error
 * given; without "always" only in an image that has not been restarted for
 * it (a loss the restart cures). */
#define SWAP_WARN_S 5
#define SWAP_LOST_AFTER 3
#define SWAP_RESTART_GAP_S 60

static gboolean swap_lost_cb (gpointer user);
static gboolean content_note_cb (gpointer user);

static const char *swap_err_name (EGLint e)
{
    return e == EGL_CONTEXT_LOST ? "EGL_CONTEXT_LOST"
         : e == EGL_BAD_SURFACE ? "EGL_BAD_SURFACE"
         : e == EGL_BAD_NATIVE_WINDOW ? "EGL_BAD_NATIVE_WINDOW" : "an EGL error";
}

/* render thread: mon_swap () returned EGL_FALSE */
static void mon_swap_failed (Mon *m)
{
    EGLint err = m->swap_err ? m->swap_err : eglGetError ();
    m->swap_err = 0;
#ifdef HAVE_WAYLAND
    if (err == SWAP_ERR_NO_DISPLAY) {
        /* nothing failed and nothing is to be made anew: the main thread
         * has the lost display in hand (plat_connection_lost) */
        if (!m->gone_said) {
            m->gone_said = TRUE;
            g_message ("monitor %d (%s): the display connection is gone: its frames are not "
                       "presented any more", m->index, m->name);
        }
        return;
    }
#endif
    const gboolean gone = err == EGL_CONTEXT_LOST || err == EGL_BAD_SURFACE ||
                          err == EGL_BAD_NATIVE_WINDOW;
    const gint64 now = g_get_monotonic_time ();
    if (m->swap_fails < G_MAXUINT)
        m->swap_fails++;
    if (g_atomic_int_get (&m->swap_lost))
        return;     /* said, and in status */
    if (gone && m->swap_fails >= SWAP_LOST_AFTER) {
        g_atomic_int_set (&m->swap_lost, (gint) err);
        g_idle_add (swap_lost_cb, m);   /* the one line, and the restart: main */
        return;
    }
    if (gone) {
        /* is it gone for good? A wallpaper that stands still would not swap
         * again for hours: the next frames now, drawn in full, paused or
         * not. (No line yet: the one about the loss follows at once, or the
         * swaps work again.) */
        m->needs_first_frame = TRUE;
        g_mutex_lock (&m->lock);
        m->render_req = TRUE;
        g_mutex_unlock (&m->lock);
    } else if (!m->swap_warn_us || now - m->swap_warn_us >= SWAP_WARN_S * G_USEC_PER_SEC) {
        m->swap_warn_us = now;
        g_warning ("monitor %d (%s): eglSwapBuffers failed: 0x%x", m->index, m->name, (guint) err);
    }
}

/* render thread: present a frame that is no content's - the black one of
 * a monitor whose wallpaper is gone or could not be loaded. Through the
 * platform's hooks like every other frame (a backend that paces itself
 * keeps its own commits apart from this one), and like every other frame
 * not on a display that is gone (mon_swap; said once, mon_swap_failed):
 * a load that fails while the daemon is on its way out came here with
 * the same swap on a dead connection. */
static void mon_swap_plain (Mon *m)
{
#ifdef HAVE_WAYLAND
    if (G_UNLIKELY (walpe_platform_display_gone ())) {
        m->swap_err = SWAP_ERR_NO_DISPLAY;
        mon_swap_failed (m);
        return;
    }
#endif
    walpe_surface_pre_swap (m->ps);
    if (eglSwapBuffers (D.egl_dpy, m->surf))
        walpe_surface_swapped (m->ps);
}

/* render thread: a swap went through after one that had not */
static void mon_swap_recovered (Mon *m)
{
    m->swap_fails = 0;
    if (g_atomic_int_get (&m->swap_lost)) {
        g_atomic_int_set (&m->swap_lost, 0);
        g_message ("monitor %d (%s): eglSwapBuffers works again", m->index, m->name);
        g_idle_add (content_note_cb, NULL);     /* status changed: subscribers */
    }
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

/* render thread, after a swap. WALPE_FAKE_VSYNC=1 (testing): block until
 * the monitor's next vertical blank, as a swap does on a real display with
 * vsync; Xvfb has none and returns at once, which hides what a slow
 * monitor does to the pacing. Monitors of unknown rate do not block. */
static void mon_swapped (Mon *m)
{
    /* the backend's part: on X11 the EGL connection's event queue is
     * emptied; a backend that paces itself waits here for interval 1 */
    walpe_surface_swapped (m->ps);
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
         * to sleep. Nor is a paused follower that owes its window nothing
         * (follow_idle): it was woken for every frame of a leader that
         * went on rendering for another monitor, up to the frame rate, to
         * present none of them. WALPE_FOLLOW_GATE=1: always woken, as
         * before (A/B). */
        Mon *f = sf->fol[i];
        g_mutex_lock (&f->lock);
        f->render_req = TRUE;
        if ((!f->clock_wait && !f->follow_idle) || g_atomic_int_get (&follow_always_wake))
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
        walpe_prof_frame_begin (m->index, MON_W (m), MON_H (m));
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
    glViewport (0, 0, MON_W (m), MON_H (m));
    {
        /* this monitor's own visible boxes ("Skip hidden areas"; -1 = no clip) */
        WalpeBox cb[WALPE_CLIP_MAX];
        int cn = w == MON_W (m) && h == MON_H (m) ? walpe_clip_boxes (cb) : -1;
        if (cn < 0)
            glBlitFramebuffer (0, 0, w, h, 0, 0, MON_W (m), MON_H (m),
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
        mon_swap_failed (m);
        glFlush ();   /* the fence must still reach the GPU */
        if (prof)
            walpe_prof_frame_end (FALSE);        /* GPUPROF */
        return FALSE;   /* nothing was presented: the frame is still owed */
    }
    if (G_UNLIKELY (m->swap_fails))
        mon_swap_recovered (m);
    mon_swapped (m);   /* after a swap that went through only: a backend that paces
                        * itself would wait here for a frame it was never given */
    if (prof)
        walpe_prof_frame_end (TRUE);             /* GPUPROF */
    g_atomic_int_inc (&m->render_count);
    g_atomic_int_set (&m->last_w, MON_W (m));
    g_atomic_int_set (&m->last_h, MON_H (m));
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
/* A scene that is paused before a video layer of it has given its first
 * picture - a daemon that starts paused, a wallpaper loaded on a paused
 * monitor - used to rest on the one frame a paused monitor draws, and in
 * that frame the layer has no picture: a scene whose background is a video
 * was black for as long as the pause lasted. Such a scene now says so
 * (content.h first_frame_pending, set by its set_paused and its gl_render;
 * the video module decodes a paused clip's first picture as it always
 * did) and the paused monitor draws a frame every
 * SCENE_FIRST_US until the picture is in it, or the scene gives up on the
 * clip (scenenode.c SCENE_VIDEO_FIRST_US). Then it rests as before: no
 * frame clock, no wake-up. A scene without a video, and one that is not
 * paused, never set the flag. Render thread. */
#define SCENE_FIRST_US 50000

static gboolean mon_scene_first (Mon *m)
{
    return m->needs_first_frame && m->content && m->content->type == WALPE_TYPE_SCENE &&
           m->content->first_frame_pending;
}

/* render thread of a leader whose paused scene has just drawn the frame
 * with that picture: its followers are paused too and present a frame of
 * their leader only when one is owed to their window - this one is */
static void sf_show_followers (Mon *m)
{
    SharedFrame *sf = m->lead_sf;
    for (int i = 0; i < sf->nfollowers; i++) {
        Mon *f = sf->fol[i];
        g_mutex_lock (&f->lock);
        f->present_req = TRUE;
        f->render_req = TRUE;
        g_cond_signal (&f->cond);
        g_mutex_unlock (&f->lock);
    }
}

static void mon_apply_pause (Mon *m, gboolean p)
{
    if (m->content && m->content->set_paused && p != m->pause_applied)
        m->content->set_paused (m->content, p);
    if (p != m->pause_applied) {
        if (p)
            m->n_pauses++;
        else
            m->n_resumes++;
    }
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
    m->n_idle_releases++;
    walpe_memc_inc (WALPE_MEMC_IDLE_RELEASE);
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
typedef struct {
    Mon *m;
    char *path, *msg;
    gboolean kept;
    guint tag;
#ifdef WALPE_FOLLOW
    guint life;     /* Mon.life when the load was made */
#endif
} LoadNote;

static void load_ended (Mon *m, const char *path, const char *msg, gboolean kept);
static gboolean diag_say (gpointer first_gl);   /* diag.h: the summary in the log, once */

static gboolean load_note_cb (gpointer user)
{
    LoadNote *n = user;
    if (n->tag && n->tag == n->m->load_wait)
        n->m->loading = FALSE;   /* (an older load's note leaves the newer one pending) */
#ifdef WALPE_FOLLOW
    /* (a load made for an output the monitor no longer has says nothing
     * about the one it has now: layout_follow) */
    if (n->life == n->m->life)
#endif
    load_ended (n->m, n->path, n->msg, n->kept);
    g_free (n->path);
    g_free (n->msg);
    g_free (n);
    /* the new content's type and pointer needs are set by now (the 1 s
     * recheck timer that used to pick them up is gone) */
    pointer_update ();
    state_touch ();
    diag_say (NULL);    /* the first wallpaper is there, or its load has failed */
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
#ifdef WALPE_FOLLOW
    n->life = m->life;      /* (written by the main thread only while this thread is parked) */
#endif
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
    int w = MAX (1, MON_W (m)), h = MAX (1, MON_H (m));
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
    mon_swap_plain (m);
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
    m->n_loads++;
    walpe_memc_inc (WALPE_MEMC_LOAD);
    if (!*path) {
        /* "" = no wallpaper any more (its folder is being deleted: IPC
         * "forget"): free the content and leave the window black */
        mon_free_content (m);
        g_atomic_int_set (&m->content_type, WALPE_TYPE_NONE);   /* a follower had no content */
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glDisable (GL_SCISSOR_TEST);
        glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport (0, 0, MAX (1, MON_W (m)), MAX (1, MON_H (m)));
        glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        mon_swap_plain (m);
        malloc_trim (0);
        mon_load_note (m, path, NULL, FALSE, tag);
        return;
    }
    WalpeType ntype = walpe_detect_type (path, NULL);
    WalpeLoadOpts opts = {
        .video_backend = "ffmpeg",
        .fps = mon_content_fps (m, ntype, fps),
        .origin_dir = path,
        .no_tex_downscale = !g_atomic_int_get (&D.cfg->tex_downscale),
        .no_tex_slack = quality->tex_slack == 0,
        .render_scale = quality->render_scale,
        .quality = quality,
        .video_box_w = m->rt_box.w,
        .video_box_h = m->rt_box.h,
    };
    /* texture_limit (resolution.h): "none" keeps file sizes like
     * tex_downscale off, a number is a cap the content applies */
    int tex_cap = 0;
    {
        gboolean none = FALSE;
        walpe_texlimit_effective (m->rt_tex_limit, !opts.no_tex_downscale, &none, &tex_cap);
        opts.no_tex_downscale = none;
    }
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
    if (walpe_load_stopping ()) {
        /* the daemon is ending (main () said so when it left its loop) and
         * the load gave up where it was: what there is of the wallpaper is
         * freed here, with this thread's context current, like one whose
         * GL init failed. Nothing is drawn and nobody is told: the main
         * thread is past its loop and waits for this thread to end. */
        g_clear_error (&err);
        g_free (restore);
        if (c) {
            if (c->gl_free)
                c->gl_free (c);
            c->free (c);
        }
        return;
    }
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
        walpe_memc_inc (WALPE_MEMC_LOAD_FAILED);
        mon_load_failed_frame (m, keep);
        /* the note goes last (as below): the main thread acts on it at once */
        mon_load_note (m, path, why, keep, tag);
        return;
    }
    g_free (restore);
    c->request_redraw = request_redraw;
    c->request_user = m;
    /* the render size chosen for this output (0 = its own mode), the way a
     * frame of another size is scaled and the texture cap: before gl_init,
     * and again before every frame (mon_do_render_frame) */
    const gboolean own_size = m->rt_res_w > 0 && m->rt_res_h > 0 &&
                              (m->rt_res_w != MON_W (m) || m->rt_res_h != MON_H (m));
    c->render_w = own_size ? m->rt_res_w : 0;
    c->render_h = own_size ? m->rt_res_h : 0;
    c->scale_filter = g_atomic_int_get (&D.cfg->scale_filter);
    c->tex_limit = tex_cap;

    /* free the old wallpaper with our context current */
    mon_free_content (m);
    const gboolean gl_ok = c->gl_init (c, &err);
    if (walpe_load_stopping ()) {
        /* (as above: it gave up in there, or the daemon began to end while
         * it finished; either way nobody will see this wallpaper) */
        g_clear_error (&err);
        if (c->gl_free)
            c->gl_free (c);
        c->free (c);
        return;
    }
    if (!gl_ok) {
        g_warning ("wallpaper GL init failed: %s", err ? err->message : "?");
        char *why = g_strdup_printf ("%s: %s", path, err ? err->message : "GL init failed");
        g_clear_error (&err);
        if (c->gl_free)
            c->gl_free (c);
        c->free (c);
        walpe_memc_inc (WALPE_MEMC_LOAD_FAILED);
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
    /* a forced frame is a wallpaper's first one, the most expensive it
     * ever draws (textures read, shaders used for the first time): not
     * begun when the daemon is ending */
    if (G_UNLIKELY (force) && walpe_load_stopping ())
        return;
    int w = MON_W (m);
    int h = MON_H (m);
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
    {
        /* render resolution: the size chosen for this output when it is
         * not the window's own (0, 0: everything is as without the setting),
         * the scaling filter and the texture cap. The content scales its
         * frame into the window itself, which works on every window system.
         * Where the window system can give the surface buffers of the
         * render size and scale them (a compositor with a viewport), this
         * is the place to ask for that instead: w and h are then the
         * render size and these fields stay 0. */
        const gboolean own_size = m->rt_res_w > 0 && m->rt_res_h > 0 &&
                                  (m->rt_res_w != w || m->rt_res_h != h);
        int cap = 0;
        walpe_texlimit_effective (m->rt_tex_limit, TRUE, NULL, &cap);
        m->content->render_w = own_size ? m->rt_res_w : 0;
        m->content->render_h = own_size ? m->rt_res_h : 0;
        m->content->scale_filter = g_atomic_int_get (&D.cfg->scale_filter);
        m->content->tex_limit = cap;
        /* the box of "video_resolution" for this wallpaper as it is now (a
         * scene's video layers: common/scenevideo.h) */
        m->content->video_box_w = m->rt_vbox.w;
        m->content->video_box_h = m->rt_vbox.h;
        m->content->video_box_live = TRUE;
    }
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
    if (paused) {   /* a forced frame (first frame, Expose, uncovered pixels) */
        m->n_paused_frames++;
        walpe_memc_inc (WALPE_MEMC_PAUSED_FRAME);
    }
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
    if (m->content->first_frame_pending && !m->lead_sf && m->content->type != WALPE_TYPE_SCENE)
        return;     /* (a scene drew: what it waits for is one layer's picture) */
    /* content.h: gl_render returns whether a change was drawn. Nothing
     * drawn = nothing to show: the back buffer is undefined after a swap
     * (EGL_BUFFER_DESTROYED), and presenting it again costs a full-screen
     * copy (and a recomposition under a compositing manager) for the same
     * or a stale picture. (A scene that found its picture unchanged says so
     * in frame_unchanged and returns FALSE: window and followers keep
     * their frame. A forced frame is always drawn: surface_lost.) */
    if (!drew || m->content->frame_unchanged) {
        /* (a first frame that gave up because the daemon is ending drew
         * nothing either, and is forced: not presented) */
        if (!force || walpe_load_stopping ())
            return;
    }
    static WalpeLazy gpumem_cell = WALPE_LAZY_UNSET;   /* every monitor's thread: common/lazy.h */
    int gpumem = walpe_lazy_get (&gpumem_cell);
    if (gpumem < 0)
        gpumem = walpe_lazy_set (&gpumem_cell, g_getenv ("WALPE_GPU_MEM") != NULL);
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
        mon_swap_failed (m);
        m->win_has_frame = FALSE;
        return;
    }
    if (G_UNLIKELY (m->swap_fails))
        mon_swap_recovered (m);
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
        walpe_prof_frame_begin (m->index, MON_W (m), MON_H (m));
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
        .w = MON_W (m),
        .h = MON_H (m),
        .type = walpe_type_name (g_atomic_int_get (&m->content_type)),
        .path = m->content_path,
        .role = role,
        .hz = m->hz,
        /* the size its wallpaper is rendered at (the output's own unless
         * another was chosen); a scene of its own keeps one more picture
         * of that size, the target it is scaled from */
        .render_w = m->rt_res_w > 0 ? m->rt_res_w : MON_W (m),
        .render_h = m->rt_res_h > 0 ? m->rt_res_h : MON_H (m),
        .render_target = m->content && m->content->type == WALPE_TYPE_SCENE && m->rt_res_w > 0 &&
                         (m->rt_res_w != MON_W (m) || m->rt_res_h != MON_H (m)),
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

/* Desktop OpenGL or OpenGL ES (common/glapi.h): the setting gl_api (auto |
 * gl | gles; WALPE_GL_API overrides it for one run), the rung of the
 * ladder OpenGL 3.3 -> OpenGL ES 3.2 -> OpenGL ES 3.1 that gave a context,
 * and why. Decided once in init_egl (), before a window or surface
 * exists; every context of the process is of that rung. With `auto` on a
 * driver that has desktop OpenGL 3.3 the first context asked for is the
 * one that was always asked for, with the same attributes. A change of
 * the setting restarts the daemon in place, as "Render on CPU" does. */
static struct {
    WalpeGlApiPref pref;        /* WALPE_GL_API, else the config */
    gboolean env_forced;        /* WALPE_GL_API decides */
    gboolean decided;           /* choice.got is the rung in use */
    WalpeGlApiChoice choice;
    EGLContext first;           /* the context the walk made */
    EGLConfig conf0;            /* the window config as init_egl () chose it, */
    int iv0[2];                 /* its swap intervals (P.min_iv, P.max_iv) */
} Ga;
/* share-group root: monitor contexts share its textures, so the
 * wallpaper's images exist once, not once per monitor. The first monitor
 * thread renders with the root itself (it used to be an extra context
 * that was never current: with N monitors N + 1 contexts were kept, each
 * with its own driver state - 28 MiB apiece on llvmpipe). */
static EGLContext share_root = EGL_NO_CONTEXT;
static gint share_root_taken;
/* no context of the share group is current while another one is created:
 * every monitor thread waits here until all of them have theirs */
static GMutex ctx_lock;
static GCond ctx_cond;
static int ctx_expected, ctx_created;

static EGLContext ladder_context (const WalpeGlRung *rung, EGLContext share)
{
    if (!eglBindAPI (rung->es ? EGL_OPENGL_ES_API : EGL_OPENGL_API))
        return EGL_NO_CONTEXT;
    EGLint ca[] = {
        EGL_CONTEXT_MAJOR_VERSION, rung->major,
        EGL_CONTEXT_MINOR_VERSION, rung->minor,
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

static gboolean egl_config_for_es (void);

/* WalpeGlTryFn: a context of this rung, kept in Ga.first */
static gboolean gl_api_try (const WalpeGlRung *rung, gpointer user)
{
    (void) user;
    /* (the OpenGL rung comes first and uses the window config as chosen;
     * an ES rung may need another one) */
    if (rung->es && !egl_config_for_es ())
        return FALSE;
    if (!rung->es && D.egl_conf != Ga.conf0) {
        /* only after a saved gl_api = gles found no context and its rungs
         * had taken another config: the OpenGL rung gets the window config
         * it always got, and the swap intervals read from it */
        D.egl_conf = Ga.conf0;
        P.min_iv = Ga.iv0[0];
        P.max_iv = Ga.iv0[1];
        g_message ("EGL config: the first one again, for desktop OpenGL");
    }
    Ga.first = ladder_context (rung, EGL_NO_CONTEXT);
    if (Ga.first == EGL_NO_CONTEXT) {
        (void) eglGetError ();
        return FALSE;
    }
    return TRUE;
}

/* main thread, in init_egl (), before any monitor thread: which API
 * renders. The context the walk made becomes the share-group root
 * (WALPE_NO_SHARE=1: it is given back, every monitor makes its own of the
 * same rung). FALSE: the driver has no context WalPE can use. */
static gboolean gl_api_decide (void)
{
    int ov = walpe_gl_api_env_override (g_getenv ("WALPE_GL_API"));
    Ga.env_forced = ov >= 0;
    Ga.pref = ov >= 0 ? (WalpeGlApiPref) ov
            : D.cfg ? (WalpeGlApiPref) D.cfg->gl_api : WALPE_GL_API_AUTO;
    if (g_getenv ("WALPE_GL_API") && ov < 0)
        g_warning ("WALPE_GL_API: auto, gl or gles expected; the gl_api setting decides");
    WalpeGlEglFacts facts = { eglQueryString (D.egl_dpy, EGL_CLIENT_APIS), FALSE, FALSE, FALSE };
    Ga.first = EGL_NO_CONTEXT;
    Ga.conf0 = D.egl_conf;
    Ga.iv0[0] = P.min_iv;
    Ga.iv0[1] = P.max_iv;
    /* WALPE_GL_API is strict: that API or nothing (a diagnostic for one
     * run). A saved gl / gles this machine has no context for runs as auto
     * would: the daemon must start, or walpe-ctl - which talks to it -
     * could never set another value. Ga.pref and the config keep the saved
     * value; the log and "gl_api_note" say what runs instead. */
    Ga.decided = walpe_gl_api_choose_for (Ga.pref, Ga.env_forced, &facts, gl_api_try, NULL,
                                          &Ga.choice);
    if (!Ga.decided) {
        g_warning ("%s [%s]", Ga.choice.why, Ga.choice.tried);
        return FALSE;
    }
    if (Ga.choice.setting_fallback)
        g_warning ("gl_api: %s [%s]", Ga.choice.why, Ga.choice.tried);
    else
        g_message ("gl_api: %s%s [%s]", Ga.choice.why,
                   Ga.env_forced ? " (WALPE_GL_API)" : "", Ga.choice.tried);
    if (g_getenv ("WALPE_NO_SHARE")) {
        eglDestroyContext (D.egl_dpy, Ga.first);
        Ga.first = EGL_NO_CONTEXT;
    } else {
        share_root = Ga.first;
    }
    return TRUE;
}

static EGLContext create_thread_context (void)
{
    if (share_root != EGL_NO_CONTEXT) {
        gboolean first = g_atomic_int_compare_and_exchange (&share_root_taken, 0, 1);
        /* the root was made on the main thread: the API binding is per thread */
        const WalpeGlRung *rung = &Ga.choice.got;
        EGLContext ctx = !first ? ladder_context (rung, share_root)
                       : eglBindAPI (rung->es ? EGL_OPENGL_ES_API : EGL_OPENGL_API)
                             ? share_root : EGL_NO_CONTEXT;
        if (ctx) {
            walpe_gl_set_shared (TRUE);
            g_message ("monitor context: %s %d.%d (textures shared across monitors)",
                       rung->es ? "GLES" : "GL", rung->major, rung->minor);
            return ctx;
        }
        /* all monitors must agree: without a share group no cache */
        g_warning ("shared GL context failed (0x%x); textures are per monitor", eglGetError ());
        walpe_gl_set_shared (FALSE);
        g_atomic_int_set (&D.share_failed, 1);
    }
    if (Ga.decided) {
        /* no share group: a context of its own, of the rung decided */
        const WalpeGlRung *rung = &Ga.choice.got;
        EGLContext ctx = ladder_context (rung, EGL_NO_CONTEXT);
        if (ctx) {
            g_message ("monitor context: %s %d.%d", rung->es ? "GLES" : "GL", rung->major,
                       rung->minor);
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
    static WalpeLazy env_cell = WALPE_LAZY_UNSET;   /* leaders' threads and main: common/lazy.h */
    int env = walpe_lazy_get (&env_cell);
    if (env < 0)
        env = walpe_lazy_set (&env_cell, g_getenv ("WALPE_POINTER_SHARE") != NULL);
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

/* `walpe-ctl mem` (engine/memreport.h), render thread, between two frames
 * and only when the main thread asked: this monitor's part of the report
 * as JSON text (g_free) - what happened to it, the GL objects its context
 * can see and what its wallpaper says it holds. Nothing is drawn and no
 * GL state is changed. */
static char *mon_mem_report (Mon *m, gboolean deep)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    walpe_mem_jint (b, "index", m->index);
    walpe_mem_jstr (b, "name", m->name);
    walpe_mem_jstr (b, "type", walpe_type_name (m->content ? m->content->type
                                                : (WalpeType) g_atomic_int_get (&m->content_type)));
    walpe_mem_jstr (b, "path", m->content_path);
    json_builder_set_member_name (b, "paused");
    json_builder_add_boolean_value (b, m->pause_applied);
    walpe_mem_jstr (b, "role", m->follow_sf ? "follows" : m->lead_sf ? "leads" : "own");
    walpe_mem_jint (b, "width", MON_W (m));
    walpe_mem_jint (b, "height", MON_H (m));
    walpe_mem_jint (b, "frames", (guint) g_atomic_int_get (&m->render_count));
    walpe_mem_jint (b, "wakeups", (guint) g_atomic_int_get (&m->wakeups));
    walpe_mem_jint (b, "loads", m->n_loads);
    walpe_mem_jint (b, "pauses", m->n_pauses);
    walpe_mem_jint (b, "resumes", m->n_resumes);
    walpe_mem_jint (b, "frames_drawn_while_paused", m->n_paused_frames);
    walpe_mem_jint (b, "idle_releases", m->n_idle_releases);
    walpe_mem_jint (b, "shared_role_changes", m->n_roles);
    if (g_atomic_int_get (&m->idle_released))
        walpe_mem_jstr (b, "note", "unloaded while paused (idle_release)");
    {
        /* shared rendering: the slot textures a leader made, the
         * framebuffers this thread keeps on the slots */
        int slots = 0, fbos = 0;
        gint64 bytes = 0;
        if (m->lead_sf)
            for (int i = 0; i < m->lead_sf->nslots; i++)
                if (m->lead_sf->tex[i]) {
                    slots++;
                    bytes += (gint64) m->lead_sf->w * m->lead_sf->h * 4;
                }
        for (int i = 0; i < 8; i++)
            fbos += m->sf_fbo[i] != 0;
        json_builder_set_member_name (b, "shared_frames");
        json_builder_begin_object (b);
        walpe_mem_jint (b, "slot_textures", slots);
        walpe_mem_jint (b, "slot_bytes", bytes);
        walpe_mem_jint (b, "framebuffers", fbos);
        json_builder_end_object (b);
    }
    walpe_mem_gl_json (b, deep);
    if (m->content && m->content->mem_stats) {
        json_builder_set_member_name (b, "content");
        json_builder_begin_object (b);
        m->content->mem_stats (m->content, b);
        json_builder_end_object (b);
    }
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

/* render thread, its context current for the first time: what the thread
 * and the daemon take from it once */
static void mon_thread_begin (Mon *m)
{
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
    res_note_gl ();       /* the largest render size this GL can be asked for */
    g_atomic_int_set (&D.egl_is_es, epoxy_is_desktop_gl () ? 0 : 1);
}

#ifdef WALPE_FOLLOW
/* A monitor without an output (layout_follow): its render thread holds no
 * wallpaper and no role, its context is current on nothing, and it waits
 * here - m->lock held, m->parked set - until the main thread has given
 * the monitor an output again (a new surface, every field of Mon set
 * anew, m->parked cleared) or the daemon ends. Then: its context on the
 * new surface. The main thread is told that the wait has begun
 * (mon_parked_cb): from then on it may free the old surface.
 * *begun: this thread has had its context current before. Returns with
 * m->lock held; the caller looks at m->quitting. */
static gboolean mon_parked_cb (gpointer user);

static void mon_park_wait (Mon *m, gboolean *begun)
{
    g_idle_add (mon_parked_cb, m);
    while (m->parked && !m->quitting)
        g_cond_wait (&m->cond, &m->lock);
    if (m->quitting)
        return;
    g_mutex_unlock (&m->lock);
    gboolean ok = eglMakeCurrent (D.egl_dpy, m->surf, m->surf, m->ctx);
    const EGLint err = ok ? EGL_SUCCESS : eglGetError ();
    if (ok && *begun) {
        mon_present_apply (m);
    } else if (ok) {
        mon_thread_begin (m);
        *begun = TRUE;
    }
    g_mutex_lock (&m->lock);
    /* (load_held_check: the setting-up of this output is over) */
    m->in_load = FALSE;
    m->load_end_us = g_get_monotonic_time ();
    if (ok)
        return;
    /* nothing can be drawn: said like swaps that fail for good (status
     * "swap_error"; the main thread restarts in place to make contexts and
     * surfaces anew, at most once a minute), and this thread waits for
     * the end */
    g_warning ("monitor %d (%s): its GL context could not be put on the new surface (0x%x)",
               m->index, m->name, (guint) err);
    g_atomic_int_set (&m->swap_lost, (gint) (err == EGL_CONTEXT_LOST ? err : EGL_BAD_SURFACE));
    g_idle_add (swap_lost_cb, m);
    while (!m->quitting)
        g_cond_wait (&m->cond, &m->lock);
}

/* Mon.role_idx: no role and no output any more (mon_park) */
#define MON_ROLE_PARK (-2)
#endif

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
#ifdef WALPE_FOLLOW
    /* (a monitor that starts without an output has no surface yet: its
     * context is made with the others' and current on nothing) */
    gboolean begun = m->surf != EGL_NO_SURFACE;
#else
    const gboolean begun = TRUE;
#endif
#ifdef WALPE_FOLLOW
    /* WALPE_WL_NO_CONTEXT (tests): the thread of a monitor that starts
     * without an output gets no context, as from a driver that refuses
     * one more. (Read here, once per such thread, before any frame.) */
    if (!begun && m->ctx != EGL_NO_CONTEXT && g_getenv ("WALPE_WL_NO_CONTEXT")) {
        eglDestroyContext (D.egl_dpy, m->ctx);
        m->ctx = EGL_NO_CONTEXT;
    }
#endif
    if (m->ctx == EGL_NO_CONTEXT ||
        (begun && !eglMakeCurrent (D.egl_dpy, m->surf, m->surf, m->ctx))) {
        g_warning ("monitor %d: EGL context unusable; thread exits", m->index);
        g_atomic_int_set (&m->thread_ended, 1);
        return NULL;
    }
    if (begun)
        mon_thread_begin (m);

    g_mutex_lock (&m->lock);
#ifdef WALPE_FOLLOW
    if (!begun)
        mon_park_wait (m, &begun);
#endif
    m->in_load = FALSE;     /* (set when the monitor was made: context, first GL calls) */
    m->load_end_us = g_get_monotonic_time ();
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
        /* render resolution: what the next load and the next frame use */
        m->rt_res_w = m->res_w;
        m->rt_res_h = m->res_h;
        m->rt_tex_limit = m->tex_limit;
        if (load)
            m->rt_box = m->load_box;
        m->rt_vbox = m->vbox;
        gboolean res_changed = m->res_changed;
        m->res_changed = FALSE;
#ifdef WALPE_FOLLOW
        /* its output has another size or scale (layout_follow): the
         * surface is fitted to it below */
        const gboolean fit = res_changed && m->fit_req;
        if (fit)
            m->fit_req = FALSE;
#endif
        gboolean stats_wanted = m->stats_until_us > g_get_monotonic_time ();
        gboolean describe_req = m->describe_req;
        m->describe_req = FALSE;
        gboolean stats_req = m->stats_req;
        m->stats_req = FALSE;
        gboolean have_stats = m->qstats_ok;
        const guint mem_req = m->mem_req;   /* walpe-ctl mem: mon_mem_report below */
        const gboolean mem_deep = m->mem_deep;
        m->mem_req = 0;
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
        if (m->present_req) {
            /* (the frame its leader has just published is owed to this
             * window, like a first frame) */
            m->present_req = FALSE;
            m->needs_first_frame = TRUE;
        }
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
            !quality_changed && !describe_req && !stats_req && !first_tick && !res_changed &&
            !mem_req) {
            /* sleep until woken, or until the content's next frame is due */
            gint64 due = (m->content && !paused && m->pause_applied == paused)
                             ? m->content->next_frame_us : 0;
            if (m->follow_sf && !paused)
                due = m->follow_wait_us;   /* following: its next refresh slot */
            if (first_due && (!due || first_due < due))
                due = first_due;           /* (not yet: first_tick is clear) */
            if (m->clip_due_us && (!due || m->clip_due_us < due))
                due = m->clip_due_us;      /* "Skip hidden areas": what became visible */
            /* a paused scene one of whose video layers has no picture yet
             * (mon_scene_first): it looks again shortly, until it has it */
            if (paused && m->pause_applied && mon_scene_first (m) &&
                (!due || m->last_draw_us + SCENE_FIRST_US < due))
                due = m->last_draw_us + SCENE_FIRST_US;
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
                    /* a paused follower presents a frame of its leader only
                     * for an Expose, a clip that grew, its first frame or a
                     * forced one: with none of them owed, the frames its
                     * leader publishes need not wake it (sf_publish) */
                    m->follow_idle = m->follow_sf && paused && !m->needs_first_frame &&
                                     !m->clip_owed && g_get_monotonic_time () >= force_until;
                    if (until)
                        g_cond_wait_until (&m->cond, &m->lock, until);
                    else
                        g_cond_wait (&m->cond, &m->lock);
                    m->follow_idle = FALSE;
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
#ifdef WALPE_FOLLOW
        if (role && ridx == MON_ROLE_PARK) {
            /* its output is gone (mon_park): the wallpaper and the role go,
             * the context is taken off the surface, which the main thread
             * frees once this thread waits. The thread and its context
             * stay for the next output: a monitor that comes back. */
            mon_drop_role (m);
            g_atomic_int_set (&m->idle_released, 0);
            mon_free_content (m);
            g_clear_pointer (&m->content_path, g_free);
            g_clear_pointer (&load, g_free);
            if (ptr)
                g_array_unref (ptr);
            glFinish ();   /* deletes are only queued until the driver has run them */
            eglMakeCurrent (D.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            malloc_trim (0);
            g_mutex_lock (&m->lock);
            m->parked = TRUE;
            mon_park_wait (m, &begun);
            continue;
        }
        if (fit) {
            /* between two frames: the buffers of the next one have the
             * output's new size, and it is drawn whatever the content
             * thinks of it (the window shows the old size stretched) */
            int fw = m->rt_w, fh = m->rt_h;
            walpe_surface_fit (m->ps, &fw, &fh);
            if (fw > 0 && fh > 0) {
                m->rt_w = fw;
                m->rt_h = fh;
            }
            m->win_has_frame = FALSE;
            m->needs_first_frame = TRUE;
            render = TRUE;
        }
#endif
        if (role) {
            m->n_roles++;
            walpe_memc_inc (WALPE_MEMC_ROLE);
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
            /* (load_held_check: a load is not a frame that does not end) */
            g_mutex_lock (&m->lock);
            m->in_load = TRUE;
            g_mutex_unlock (&m->lock);
            mon_do_load (m, load, paused, fps, &quality, keep, load_tag);
            g_mutex_lock (&m->lock);
            m->in_load = FALSE;
            m->load_end_us = g_get_monotonic_time ();
            g_mutex_unlock (&m->lock);
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
        if (paused != m->pause_applied) {
            mon_apply_pause (m, paused);
            if (paused && mon_scene_first (m))
                m->needs_first_frame = TRUE;    /* (the sleep above ends for it) */
        }
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
        /* another render size or texture limit for the running wallpaper:
         * shown with one frame (a paused monitor takes it when it resumes;
         * a scene is loaded again by the main thread besides) */
        if (res_changed && !load && m->content)
            render = TRUE;
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
                walpe_clip_set (clip, clip_n, MON_W (m), MON_H (m));
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
            const gboolean first_was = mon_scene_first (m);
            mon_do_render (m, paused, m->needs_first_frame || force || clip_owed, self_paused,
                           ptr_x, ptr_y, clip_gen);
            m->needs_first_frame = m->content && m->content->first_frame_pending;
            if (first_was && !m->needs_first_frame && m->lead_sf)
                sf_show_followers (m);  /* (paused, they hold the frame without that picture) */
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
        char *mem = mem_req ? mon_mem_report (m, mem_deep) : NULL;
        g_mutex_lock (&m->lock);
        if (mem_req) {
            g_free (m->mem_json);
            m->mem_json = mem;
            m->mem_done = mem_req;
            g_cond_broadcast (&m->mem_cond);
        }
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
    g_atomic_int_set (&m->thread_ended, 1);
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

/* ---- render resolution (common/resolution.h) ---------------------------
 * Every output has a choice of its own in config.json "monitors": the size
 * its wallpaper is rendered at (native = the output's mode, the default)
 * and the largest texture kept for it. The main thread turns the choices
 * into numbers (res_refresh, res_texlimit_for, video_box_for) and hands
 * them to the render threads through Mon, under its lock; a render thread
 * gives them to its content before every frame (WalpeContent.render_w /
 * render_h / tex_limit). An output without an entry runs at native and is
 * "not asked yet": the daemon never waits for an answer, the GUI asks. */
static gint gl_max_side;        /* atomic: the largest texture side the render
                                 * threads' GL takes (0 = not known yet) */
static GPtrArray *res_env;      /* WALPE_RESOLUTION (WalpeResAssign): choices that
                                 * replace the stored ones for this run, not saved */
static gboolean res_no_ask;     /* --windowed: its one window is not an output */

/* render thread, context current */
static void res_note_gl (void)
{
    GLint mx = 0;
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &mx);
    while (mx > 0) {
        int have = g_atomic_int_get (&gl_max_side);
        if (have > 0 && have <= mx)
            break;
        if (g_atomic_int_compare_and_exchange (&gl_max_side, have, mx)) {
            /* The render sizes were worked out before any context existed
             * (main (), res_refresh: the limit was not known, every stored
             * size passed). Now it is known, or smaller than thought: the
             * main thread looks at the sizes again. */
            g_idle_add (res_gl_known_cb, NULL);
            break;
        }
    }
}

/* main: the choice that holds for output `name`: WALPE_RESOLUTION when it
 * names the output (or all), else its entry, else the defaults.
 * The entry is found by the exact name the output list gives (the key of
 * "monitor_current" too). This is the one place a lookup under another
 * spelling of the same connector belongs, should the output list offer
 * one (the same output under another window system). */
static void res_conf_for (const char *name, WalpeMonitorConf *mc)
{
    walpe_config_monitor_get (D.cfg, name, mc);
    for (guint i = 0; res_env && i < res_env->len; i++) {
        const WalpeResAssign *a = g_ptr_array_index (res_env, i);
        if (!a->monitor || !g_strcmp0 (a->monitor, name))
            walpe_res_parse (a->value, &mc->res, NULL);
    }
}

/* main: the size the wallpaper of m is rendered at. *note (g_free; may be
 * NULL): why it is not the chosen one, when it is not. */
static void mon_res_size (Mon *m, int *w, int *h, char **note)
{
    WalpeMonitorConf mc;
    res_conf_for (m->name, &mc);
    int rw = m->geom.width, rh = m->geom.height;
    walpe_res_size (&mc.res, m->geom.width, m->geom.height, &rw, &rh);
    const int mx = g_atomic_int_get (&gl_max_side);
    if (note)
        *note = NULL;
    if (rw < 1 || rh < 1) {
        rw = m->geom.width;
        rh = m->geom.height;
    } else if (mx > 0 && (rw > mx || rh > mx)) {
        if (note)
            *note = g_strdup_printf ("%d x %d is more than the largest picture this GPU "
                                     "renders to (%d pixels a side): rendered at the output's "
                                     "own %d x %d", rw, rh, mx, m->geom.width, m->geom.height);
        rw = m->geom.width;
        rh = m->geom.height;
    }
    *w = rw;
    *h = rh;
}

/* main: the texture limit a wallpaper is loaded with on m - the least
 * strict of the monitors that show `path` (resolution.h
 * walpe_texlimit_merge: they share its textures) */
static int res_texlimit_for (Mon *m, const char *path)
{
    WalpeMonitorConf mc;
    res_conf_for (m->name, &mc);
    int lim = mc.tex_limit;
    for (GList *l = D.mons; path && *path && l; l = l->next) {
        Mon *o = l->data;
        if (o == m || g_strcmp0 (o->want_path, path))
            continue;
        res_conf_for (o->name, &mc);
        lim = walpe_texlimit_merge (lim, mc.tex_limit);
    }
    return lim;
}

/* main: has anybody been asked about m's output (mc: its stored entry, or
 * what res_conf_for gave). Also TRUE where the question could never be
 * answered: the --windowed window, and an output whose name cannot be a key
 * of "monitors" (walpe_monitor_name_ok: longer than 64 bytes, control
 * characters, not UTF-8). Output names are whatever the X server has, and
 * `xrandr --setmonitor` takes any text. No choice can be stored for such an
 * output, it runs at native; reported as "not asked" it stayed so after
 * every answer, and the window asked again at every start. */
static gboolean mon_res_asked (const Mon *m, const WalpeMonitorConf *mc)
{
    return res_no_ask || !walpe_monitor_name_ok (m->name) || (mc && mc->asked);
}

/* main: connected outputs nobody has been asked about yet */
static int res_unasked (void)
{
    int n = 0;
    for (GList *l = D.mons; !res_no_ask && l; l = l->next) {
        const Mon *m = l->data;
        n += !mon_res_asked (m, walpe_config_monitor (D.cfg, m->name));
    }
    return n;
}

/* main: the box the converted copy of video wallpaper `path` is made for
 * (config video_resolution; resolution.h walpe_video_box). auto looks at
 * the render sizes of the monitors that show the video: those whose
 * wallpaper it is, plus `also` (an `open` on that monitor is under way),
 * or all of them (everyone: an `open` for every monitor; and when nobody
 * shows it: an import into the library). */
static void video_box_for (const char *path, Mon *also, gboolean everyone, WalpeVideoBox *box)
{
    enum { MAXN = 32 };
    int sizes[2 * MAXN];
    guint n = 0;
    for (int pass = 0; pass < 2 && !n; pass++)
        for (GList *l = D.mons; l && n < MAXN; l = l->next) {
            Mon *m = l->data;
            if (!pass && !everyone && m != also && (!path || g_strcmp0 (m->want_path, path)))
                continue;
            sizes[2 * n] = m->res_w > 0 ? m->res_w : m->geom.width;
            sizes[2 * n + 1] = m->res_h > 0 ? m->res_h : m->geom.height;
            n++;
        }
    walpe_video_box (&D.cfg->video_res, sizes, n, box);
}

/* main: the box of every monitor's wallpaper as it is now, handed to its
 * render thread when it changed (Mon.vbox). The box depends on which
 * monitors show the wallpaper and on their render sizes, so it changes for
 * a wallpaper that stays where it is when another monitor gets it too, or
 * loses it, or is given another render size, and with "video_resolution".
 * A scene with a video layer looks at it with its next frame
 * (common/scenevideo.h); nothing else reads it. */
static void video_boxes_update (void)
{
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        WalpeVideoBox box;
        video_box_for (m->want_path, NULL, FALSE, &box);
        if (box.w == m->vbox.w && box.h == m->vbox.h)
            continue;
        g_mutex_lock (&m->lock);
        m->vbox = box;
        /* (a frame only where a scene runs: nothing else has such videos) */
        if (g_atomic_int_get (&m->content_type) == WALPE_TYPE_SCENE) {
            m->render_req = TRUE;
            g_cond_signal (&m->cond);
        }
        g_mutex_unlock (&m->lock);
    }
}

/* main thread: give a monitor thread its next wallpaper (path, or NULL =
 * keep the content) and role: sf + idx -1 = lead sf, idx >= 0 = follow
 * sf, sf NULL = render on its own. keep: should that load fail, a monitor
 * of its group has a previous wallpaper to return to (load_keep). */
static void mon_assign (Mon *m, const char *path, SharedFrame *sf, int idx, gboolean keep)
{
    g_mutex_lock (&m->lock);
    if (path) {
        static guint tags;
        if (!m->load_path) {
            /* the thread took the one before: that is what it draws or loads */
            g_free (m->busy_path);
            m->busy_path = m->given_path;
            m->given_path = NULL;
        }
        g_free (m->given_path);
        m->given_path = g_strdup (path);
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
        /* its texture limit (shared with the other monitors that show it)
         * and, a video, the box of its converted copy */
        m->tex_limit = res_texlimit_for (m, path);
        video_box_for (path, NULL, FALSE, &m->load_box);
        m->vbox = m->load_box;
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

static gboolean loadmark_due (gpointer user);

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
    /* for the owner alone, like config.json: it names wallpaper paths, and
     * without XDG_RUNTIME_DIR it lies in ~/.cache, which is not private
     * everywhere (it was written with whatever the umask left: 0644) */
    walpe_write_private (p, s->str, (gssize) s->len, NULL);
    g_free (p);
    g_string_free (s, TRUE);
    if (loadmark_timer)
        g_source_remove (loadmark_timer);
    loadmark_timer = g_timeout_add_seconds (LOADMARK_SECS, loadmark_due, NULL);
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

/* ---- render resolution: the choices become numbers ----------------------
 * main thread. res_refresh: every monitor's render size from its choice. A
 * monitor whose size changed gets it at once (its thread draws a frame at
 * it); a scene it shows is marked to load again - what a scene keeps of
 * each texture follows the size it is rendered at, and only a load makes
 * that smaller (scenes_reload_mark's convention: the caller ends with
 * res_commit ()). Videos, web pages and pictures follow with their next
 * frame. Returns TRUE when a size changed. */
static gboolean res_refresh (void)
{
    gboolean any = FALSE;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        int w = 0, h = 0;
        char *note = NULL;
        mon_res_size (m, &w, &h, &note);
        if (note && g_strcmp0 (note, m->res_note))
            g_warning ("monitor %s: %s", m->name, note);
        g_free (m->res_note);
        m->res_note = note;
        if (w == m->res_w && h == m->res_h)
            continue;
        any = TRUE;
        g_mutex_lock (&m->lock);
        m->res_w = w;
        m->res_h = h;
        m->res_changed = TRUE;
        g_cond_signal (&m->cond);
        g_mutex_unlock (&m->lock);
        if (mon_shows_scene (m))
            m->reload_wanted = TRUE;
    }
    return any;
}

/* main: the texture limit of every monitor's wallpaper again (a monitor's
 * choice changed). Where it differs from what the wallpaper was loaded
 * with: handed over, and a scene loads again. TRUE when one changed. */
static gboolean res_texlimits_refresh (void)
{
    gboolean any = FALSE;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        if (m->leader)
            continue;   /* shows its leader's frames: no textures of its own */
        int lim = res_texlimit_for (m, m->want_path);
        if (lim == m->tex_limit)
            continue;
        any = TRUE;
        g_mutex_lock (&m->lock);
        m->tex_limit = lim;
        m->res_changed = TRUE;
        g_cond_signal (&m->cond);
        g_mutex_unlock (&m->lock);
        if (mon_shows_scene (m))
            m->reload_wanted = TRUE;
    }
    return any;
}

static void video_boxes_refresh (void);

/* main, at the end of a request that changed a render size, a texture
 * limit or video_resolution: the groups of monitors that share one
 * rendering depend on the render size (formed again), the marked scenes
 * load, and a video whose converted copy is for another box now gets the
 * one for this box. */
static void res_commit (gboolean sizes_changed)
{
    gboolean marked = FALSE;
    for (GList *l = D.mons; l; l = l->next)
        marked = marked || ((Mon *) l->data)->reload_wanted;
    if (sizes_changed)
        apply_wallpapers (NULL, NULL, FALSE, TRUE);    /* regroup; takes every mark */
    else if (marked)
        apply_wallpapers (NULL, NULL, FALSE, FALSE);
    video_boxes_refresh ();
    fps_refresh ();
    state_touch ();
}

/* main (idle, queued by res_note_gl): the largest picture the GL renders to
 * is known now. A stored size beyond it was handed to the render threads at
 * start as it was; here it falls back to the output's own (mon_res_size)
 * and status says why ("resolution_note"). Nothing happens when every size
 * fits - the usual case. */
static gboolean res_gl_known_cb (gpointer user)
{
    (void) user;
    if (res_refresh ())
        res_commit (TRUE);
    return G_SOURCE_REMOVE;
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
    gboolean changed = FALSE;   /* a monitor was given another wallpaper */
    for (GList *l = D.mons; l; l = l->next, i++) {
        Mon *m = ms[i] = l->data;
        char *was = m->want_path;
        m->want_path = g_strdup (walpe_config_monitor_path (D.cfg, m->name));
        const char *skip = load_skip ? g_hash_table_lookup (load_skip, m->name) : NULL;
        if (skip && !g_strcmp0 (skip, m->want_path)) {
            g_free (m->want_path);
            m->want_path = g_strdup ("");   /* crash-loop guard: stays empty */
        }
        changed = changed || g_strcmp0 (was, m->want_path) != 0;
        if (g_strcmp0 (was, m->want_path) && !restore_apply) {
            /* it was given another wallpaper (or none): how the load of the
             * one before ended is history, and so is a re-open */
            g_clear_pointer (&m->reopen_for, g_free);
            g_clear_pointer (&m->load_error, g_free);
            g_clear_pointer (&m->restored, g_free);
        }
        g_free (was);
    }
    if (web_unload_due (changed)) {
        /* the last web wallpaper is being replaced: the daemon starts again
         * in place, and that image loads what the monitors are set to now
         * (nothing is loaded here first: it would be loaded twice) */
        g_free (ms);
        g_free (lead);
        g_free (load);
        g_free (force_own);
        return;
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
            /* (and rendered at the same size: one frame cannot be two) */
            if (!lead[j] && !grouped[j] && !g_strcmp0 (o->want_path, m->want_path) &&
                o->geom.width == m->geom.width && o->geom.height == m->geom.height &&
                o->res_w == m->res_w && o->res_h == m->res_h) {
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
    video_boxes_update ();   /* who shows what changed: so may the box of a video in a scene */
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

/* The folders of the wallpapers whose load failed, for the summary of
 * diag.h: the reason a monitor's line quotes names the failed wallpaper by
 * its path, and the monitor itself is back on the one it showed before, so
 * nothing else the daemon holds says which folder that was. The last 32
 * different ones; touched when a load fails, read when a summary is made. */
static GPtrArray *load_failed_dirs;

static void load_failed_note (const char *path)
{
    if (!path || path[0] != '/')
        return;
    char *dir = g_path_get_dirname (path);
    if (!load_failed_dirs)
        load_failed_dirs = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; i < load_failed_dirs->len; i++)
        if (!strcmp (g_ptr_array_index (load_failed_dirs, i), dir)) {
            g_free (dir);
            return;
        }
    if (load_failed_dirs->len >= 32)
        g_ptr_array_remove_index (load_failed_dirs, 0);
    g_ptr_array_add (load_failed_dirs, dir);
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
    if (msg)
        load_failed_note (path);
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

static const WalpeOutput *mon_output (Mon *m);

/* A fullscreen window that has the focus is on this monitor. (The
 * --windowed preview is on no monitor: for it, any such window counts,
 * as it always did.) */
static gboolean mon_fullscreen (Mon *m)
{
    return D.fs_active && (m->fs_here || !mon_output (m));
}

/* Which of the rules that need other programs' windows are switched on:
 * a provider that costs something to keep (KWin's: a connection to the
 * session bus and a script in KWin) is kept only for a rule that is. */
static void sig_window_rules (void)
{
    walpe_sigprov_set_window_rules (
        (D.cfg->pause_fullscreen ? WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN) : 0) |
        (D.cfg->pause_covered ? WALPE_SIG_BIT (WALPE_SIG_COVERED) : 0) |
        (D.cfg->focus_action != WALPE_FOCUS_RUN ? WALPE_SIG_BIT (WALPE_SIG_FOCUS_APP) : 0));
}

/* "Pause for fullscreen apps" (pause_fullscreen) applies to this monitor:
 * the focused fullscreen window is on it. A film or a game in full screen
 * on one monitor leaves the wallpaper of the monitor next to it running;
 * pause_fullscreen_all brings back what the rule did before it looked at
 * monitors (every monitor pauses, which frees the GPU for the game). */
static gboolean mon_fullscreen_pause (Mon *m)
{
    return D.cfg->pause_fullscreen && D.fs_active &&
           (D.cfg->pause_fullscreen_all || mon_fullscreen (m));
}

/* ... and to any monitor (status "fullscreen_pause") */
static gboolean any_fullscreen_pause (void)
{
    for (GList *l = D.mons; l; l = l->next)
        if (mon_fullscreen_pause (l->data))
            return TRUE;
    return FALSE;
}

/* one central place deciding whether a monitor's wallpaper should be
 * paused (decode + render). Per-monitor: each monitor owns its own
 * content/decoder instance, so covered monitors stop while others play. */
static gboolean want_pause_mon (Mon *m)
{
    return D.cfg->paused ||
           mon_fullscreen_pause (m) ||
           (D.cfg->pause_dpms && D.asleep) ||
           (D.cfg->pause_covered && m->covered) ||
           /* "Skip hidden areas" (opt-in): not one pixel of it can be seen
            * (also under windows the cover scan does not count: a screen
            * locker, an override-redirect full-screen window) */
           (D.cfg->pause_covered && m->vis_none) ||
           /* opt-in rules (off by default) */
           (D.cfg->focus_action == WALPE_FOCUS_PAUSE && m->focus_app) ||
           (D.cfg->pause_battery && D.on_battery) ||
           /* nobody has touched keyboard or pointer for pause_idle seconds
            * (off by default; reported where the session can: status
            * "pause_rules") */
           (D.cfg->pause_idle > 0 && walpe_signal_get (WALPE_SIG_IDLE, 0));
}

/* GPUPROF: why a monitor is paused, for the profile report (the
 * first rule that applies, in want_pause_mon's order) */
static const char *pause_reason_mon (Mon *m)
{
    if (D.cfg->paused)
        return "walpe-ctl pause";
    if (mon_fullscreen_pause (m))
        return "a fullscreen window has the focus";
    if (D.cfg->pause_dpms && D.asleep)
        return "display asleep";
    if (D.cfg->pause_covered && m->covered)
        return "covered by windows";
    if (D.cfg->focus_action == WALPE_FOCUS_PAUSE && m->focus_app)
        return "focus rule";
    if (D.cfg->pause_battery && D.on_battery)
        return "on battery";
    if (D.cfg->pause_idle > 0 && walpe_signal_get (WALPE_SIG_IDLE, 0))
        return "no input for a while";
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

/* The log says when a monitor's wallpaper stops and when it runs again, and
 * which rule stopped it (the first that applies, as `status` tells it): a
 * wallpaper that "froze" or "went silent" after a click was a pause rule at
 * work more often than a fault, and the log had no word of it. One line per
 * change of a monitor's own state, written by the main thread where the
 * change is decided: a window that covers the desktop and uncovers it makes
 * two. */
static void pause_said (Mon *m, gboolean paused)
{
    const char *why = paused ? pause_reason_mon (m) : "";
    if (paused)
        g_message ("monitor %d (%s): paused: %s", m->index, m->name ? m->name : "?",
                   *why ? why : "nothing of it can be seen");
    else
        g_message ("monitor %d (%s): running again", m->index, m->name ? m->name : "?");
}

static void apply_pause_state (void)
{
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        const gboolean was = m->own_paused;
        m->own_paused = want_pause_mon (m);
        if (m->own_paused != was)
            pause_said (m, m->own_paused);
    }
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
            if (p != m->pause_wanted)
                walpe_memc_inc (p ? WALPE_MEMC_PAUSE : WALPE_MEMC_RESUME);
            m->pause_wanted = p;
            m->self_paused = self;
            g_cond_signal (&m->cond); /* render thread applies it */
        }
        g_mutex_unlock (&m->lock);
    }
    fps_refresh ();
    pointer_update ();
    state_touch ();
    web_unload_hidden ();   /* (a restart that was put off: now, if nobody sees a wallpaper) */
}

/* The GUI starts the daemon with stdout / stderr appended to
 * ~/.cache/walpe/daemon.log and rotates that file only when it starts a
 * daemon: one that runs for weeks (or meets something that logs every
 * frame) grew it without limit. Every 256 log lines (log_guard; no
 * timer): a log that is a regular
 * file opened for appending and has passed 16 MiB keeps its last 1 MiB
 * (WALPE_LOG_MAX_MB=<n> changes the limit, 0 switches this off). A
 * terminal, a pipe or the journal is left alone; a file that was opened
 * for writing only is appended to from the start on (log_file_setup),
 * so it is capped like the others. */
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

/* main (), before the first log line. Two things the cap above did not
 * reach when the daemon was not started by the GUI:
 *  - a log opened for writing, not for appending (a shell's "> file", an
 *    autostart entry that redirects): never capped, for good reason - cut
 *    back under a descriptor that keeps its old offset, the file is as
 *    long as before with the next line. Such a descriptor, when it stands
 *    at the end of a regular file, is switched to appending: the same
 *    place to write to, now and after every line, and after a cut;
 *  - a file that is over the limit already: the check came with the 256th
 *    line of a run, so a daemon that is started often and says little
 *    each time (an autostart at every login) added to its log for ever.
 *    The cap is applied once now. */
static void log_file_setup (void)
{
    struct stat st;
    int fl = fcntl (STDERR_FILENO, F_GETFL);
    if (fl < 0 || fstat (STDERR_FILENO, &st) != 0 || !S_ISREG (st.st_mode))
        return;
    if (!(fl & O_APPEND) && lseek (STDERR_FILENO, 0, SEEK_CUR) == st.st_size &&
        fcntl (STDERR_FILENO, F_SETFL, fl | O_APPEND) != 0)
        return;
    log_cap_now ();
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

/* -------------------------------------------------------- pause signals
 * Whether a fullscreen window has the focus (and on which monitor), the
 * displays sleep, windows cover a monitor or an application has the focus
 * on it is found out by
 * the providers of the platform layer (platform/signals.h: on X11 from
 * EWMH properties, DPMS and the screensaver extension, as ever). Their
 * combined values land here, in the fields the pause rules read; the
 * registry calls this once per change (once per scan of the windows). */
static void signals_changed (guint mask, gpointer user)
{
    (void) user;
    if (mask & WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN))
        D.fs_active = walpe_signal_get (WALPE_SIG_FULLSCREEN, 0);
    if (mask & WALPE_SIG_BIT (WALPE_SIG_ASLEEP))
        D.asleep = walpe_signal_get (WALPE_SIG_ASLEEP, 0);
    if (mask & WALPE_SIG_BIT (WALPE_SIG_IDLE))
        D.idle = walpe_signal_get (WALPE_SIG_IDLE, 0);
    if (mask & WALPE_SIG_PER_OUTPUT)
        for (GList *l = D.mons; l; l = l->next) {
            Mon *m = l->data;
            m->covered = walpe_signal_get (WALPE_SIG_COVERED, m->out_id);
            /* an application's window on this monitor has the focus (the
             * focus_action rule; the desktop itself focused = none) */
            m->focus_app = walpe_signal_get (WALPE_SIG_FOCUS_APP, m->out_id);
            /* the focused fullscreen window is on this monitor (or on one
             * nobody can name: reported for every output then) */
            m->fs_here = walpe_signal_get (WALPE_SIG_FULLSCREEN_ON, m->out_id);
        }
    apply_pause_state ();
}

static const WalpeSignalSink signal_sink = { signals_changed };

static void pointer_rest_sample (void);

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
                 walpe_surface_native_id (m->ps));
        g_debug ("stats:   geom=%dx%d at %d,%d fs=%d covered=%d paused_cfg=%d",
                 m->geom.width, m->geom.height, m->geom.x, m->geom.y,
                 mon_fullscreen (m), m->covered, D.cfg->paused);
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
    if (mode != WALPE_SKIP_OFF && D.backend != WALPE_BACKEND_X11) {
        /* the tracker reads the X window tree (daemon/vistrack.h); other
         * window systems do not tell a client where other windows are */
        static gboolean said;
        if (!said)
            g_message ("skip hidden areas: only on X11; everything is rendered");
        said = TRUE;
        mode = WALPE_SKIP_OFF;
    }
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
            vm[i] = (WalpeVisMon) { m->geom.x, m->geom.y, m->geom.width, m->geom.height,
                                       walpe_surface_native_id (m->ps) };
        }
        walpe_platform_sync (); /* the tracker's connection must see our windows */
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

/* ------------------------------------------------------------- mouse */

/* a left press/release somewhere on screen (a button event or IPC): goes to
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
 * right) at position rx, ry (a button event or IPC) */
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

/* ---- pointer position
 * OFF:   nothing shown uses the pointer: no events, no timer.
 * TRACK: one query of the pointer per frame interval (30..144 Hz, as
 *        before) while the pointer moves (X11: XQueryPointer).
 * ARMED: the pointer has not moved for PTR_REST_MS: the timer is gone and
 *        motion events are asked for (X11: XI_RawMotion on the root). The
 *        first one goes back to TRACK and deselects motion again (a
 *        1000 Hz mouse would otherwise wake the daemon a thousand times a
 *        second).
 * So a pointer at rest costs nothing; it used to cost one round trip per
 * frame interval for as long as such a wallpaper was visible. Where the
 * window system has no input events (X11 without XInput2) the rest state
 * polls every PTR_SLOW_MS instead. */
#define PTR_REST_MS 400
#define PTR_SLOW_MS 100
enum { PTR_OFF, PTR_ARMED, PTR_TRACK };
static int ptr_mode = PTR_OFF;
static guint ptr_timer;
static gint64 ptr_moved_us;
static int ptr_seen_x = G_MININT, ptr_seen_y = G_MININT;
static guintptr ptr_seen_child;
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

/* does the wallpaper shown on m (its own, or its leader's) use the pointer
 * right now - its position or its clicks? wayland_pointer = off (config):
 * never on Wayland, so that the wallpaper surface takes no input at all and
 * every click on the desktop stays with the compositor. */
static gboolean mon_uses_pointer (Mon *m)
{
    Mon *r = mon_route (m);
    int t = g_atomic_int_get (&r->content_type);
    if (m->own_paused || (D.backend == WALPE_BACKEND_WAYLAND && D.cfg->wayland_pointer))
        return FALSE;
    return (t == WALPE_TYPE_SCENE && D.cfg->parallax && g_atomic_int_get (&r->uses_ptr_pos)) ||
           t == WALPE_TYPE_WEB || g_atomic_int_get (&r->wants_pointer);
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
    else if (mode == PTR_ARMED && !walpe_platform_has_input_events ())
        ptr_timer = g_timeout_add (PTR_SLOW_MS, pointer_tick, NULL);
    in_want_motion = mode == PTR_ARMED && walpe_platform_has_input_events ();
    input_apply ();
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
    if (ptr_mode == PTR_ARMED && moved) {   /* no input events: the slow poll saw it move */
        ptr_timer = 0;
        pointer_set_mode (PTR_TRACK);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* a motion event while at rest */
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
    if (ptr_mode == PTR_ARMED && walpe_platform_has_input_events ())
        pointer_sample ();
}

/* one pointer event from the Plasma add-on (signals/kwin.c) */
static void plasma_pointer_event (const char *output, guint what, double x, double y, double a, double b,
                                  gpointer user)
{
    (void) user;
    if (what <= WALPE_FOREIGN_LEAVE)
        walpe_platform_pointer_foreign (output, (WalpeForeignPointer) what, x, y, a, b);
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
    in_want_buttons = any_wants_pointer () || any_web_input ();
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
    input_apply ();
    /* which monitors' wallpapers use the pointer at all: a window system
     * that shows the pointer to a surface only while it takes input
     * (Wayland) is told per surface; X11 reads the pointer from the root
     * window and needs nothing */
    for (GList *l = D.mons; l; l = l->next)
        walpe_surface_want_pointer (((Mon *) l->data)->ps, mon_uses_pointer (l->data));
    /* KDE Plasma with the add-on: Plasma's desktop window is over the
     * wallpaper and has the pointer; its wallpaper type hands it on while
     * a wallpaper here wants it (and only then is anything listening) */
    char *ptr_outputs = NULL;
    gboolean ptr_helper = walpe_platform_pointer_helper (&ptr_outputs);
    walpe_sigprov_set_plasma_pointer (ptr_helper, ptr_outputs, mouse_poll_ms (), plasma_pointer_event, NULL);
    g_free (ptr_outputs);
}

/* one query of the pointer, handed to every monitor that uses it; TRUE = the
 * pointer (or the window under it) is not where it was last time */
static gboolean pointer_sample (void)
{
    WalpePointer ptr;
    if (!walpe_platform_pointer (&ptr))
        return FALSE;
    /* inside = FALSE: the pointer is where the window system does not show
     * it to this program (Wayland: over another program's window). It is
     * then on no monitor as far as the wallpapers are concerned: pages and
     * cursor scripts are told that it left. (X11 always knows.) */
    const int rx = ptr.inside ? ptr.x : G_MININT / 2, ry = ptr.inside ? ptr.y : G_MININT / 2;
    const guintptr c = ptr.under;
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
            gboolean desk = walpe_platform_on_desktop (c) || m->ptr_pressed;
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
        /* a pointer that cannot be seen leaves parallax where it was */
        if (!ptr.inside)
            continue;
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

/* ------------------------------------------------------ platform events
 * What the window system tells the daemon (platform.h,
 * WalpePlatformEvents); all on the main thread. */
static void layout_schedule (void);
static void layout_surface_lost (void);

static void plat_outputs_changed (gpointer user)
{
    (void) user;
    layout_schedule ();     /* monitors changed? */
}

/* A part of a wallpaper window lost its pixels: its next frame is drawn
 * whatever the content thinks of it (a follower of a shared scene presents
 * the leader's newest frame again). A window dragged across the wallpaper
 * sends these at the pointer's rate: one answer per frame interval of the
 * fps setting (expose_fire). */
static void plat_surface_exposed (gpointer surface_user, gpointer user)
{
    (void) user;
    Mon *m = surface_user;
    if (m && !m->expose_timer_id)
        m->expose_timer_id = g_timeout_add ((guint) MAX (1, 1000 / CLAMP (D.cfg->fps, 5, 200)),
                                            expose_fire, m);
}

/* The window system took a surface away (Wayland: its output is gone, or
 * the compositor closed the layer surface). Nothing can be shown on it any
 * more; a restart in place makes the surfaces again for the outputs there
 * are then. A closed --windowed window ends the daemon, as closing a
 * program's only window does. */
static void plat_surface_closed (gpointer surface_user, gpointer user)
{
    (void) user;
    Mon *m = surface_user;
    if (D.windowed) {
        g_message ("the window was closed: shutting down");
        g_main_loop_quit (D.loop);
        return;
    }
    g_message ("monitor %d (%s): the window system took its surface away", m ? m->index : -1,
               m && m->name ? m->name : "?");
    layout_surface_lost ();
}

static void plat_pointer_motion (gpointer user)
{
    (void) user;
    pointer_motion ();      /* the pointer left its resting place */
}

static void plat_windows_changed (gpointer user)
{
    (void) user;
    pointer_rest_sample (); /* another window may be under a resting pointer now */
}

/* scenes: left button; web pages: left/middle/right and the wheel (4-7) */
static gboolean plat_button_wanted (int button, gpointer user)
{
    (void) user;
    return (button == 1 && any_wants_pointer ()) ||
           (button >= 1 && button <= 7 && any_web_input ());
}

static void plat_button (int button, gboolean down, int x, int y, gboolean on_desktop,
                         guint32 time_ms, gpointer user)
{
    (void) user;
    gboolean scene = button == 1 && any_wants_pointer ();
    gboolean web = button >= 1 && button <= 7 && any_web_input ();
    if (scene)
        pointer_button (down, x, y, on_desktop);
    if (web)
        web_button (button, down, x, y, on_desktop, time_ms);
}

static int reconnect_wait_s (void);
#ifdef HAVE_WAYLAND
static int lost_decide (int wait);
#endif

/* The connection to the window system is gone for good (Wayland: the
 * compositor went away; on X11 Xlib ends the process before anything
 * could be told). The daemon stops the normal way - socket, crash-loop
 * marker, running imports - and exits with status 1, as it does when the
 * X server goes away. With wayland_reconnect = <seconds> (gpu.conf /
 * WALPE_WAYLAND_RECONNECT; default 0) it restarts in place instead and
 * the new image waits that long for the display to come back - unless
 * the compositor is one that does not stay (lost_decide). */
static void plat_connection_lost (gpointer user)
{
    (void) user;
    if (D.display_lost)
        return;
    D.display_lost = TRUE;
    int wait = D.windowed ? 0 : reconnect_wait_s ();
#ifdef HAVE_WAYLAND
    if (wait > 0)
        wait = lost_decide (wait);
#endif
    if (wait > 0) {
        g_message ("the display connection is gone: restarting in place, waiting up to %d s "
                   "for it to come back (wayland_reconnect)", wait);
        D.restart = TRUE;
    } else {
        g_message ("the display connection is gone: shutting down");
    }
#ifdef HAVE_WAYLAND
    {
        /* WALPE_LOST_LINGER_MS (tests): the main thread stands still that
         * long before it leaves its loop, so that the render threads come
         * to their next frames with the display gone - which they do
         * anyway, for the moment the tear-down takes to reach them */
        const char *e = g_getenv ("WALPE_LOST_LINGER_MS");
        if (e && *e)
            g_usleep ((gulong) CLAMP (g_ascii_strtoll (e, NULL, 10), 0, 10000) * 1000);
    }
#endif
    if (D.loop)
        g_main_loop_quit (D.loop);
}

static const WalpePlatformEvents plat_events = {
    .outputs_changed = plat_outputs_changed,
    .surface_exposed = plat_surface_exposed,
    .surface_closed = plat_surface_closed,
    .pointer_motion = plat_pointer_motion,
    .windows_changed = plat_windows_changed,
    .button_wanted = plat_button_wanted,
    .button = plat_button,
    .connection_lost = plat_connection_lost,
};

/* ------------------------------------------------------------- import
 * `walpe-ctl add` / GUI import: common/import.c runs ffmpeg as a child
 * process (async on this main loop) and normalizes any video into H.264
 * High yuv420p MP4 (+faststart, no audio: the renderer is silent) in
 * <wallpapers_dir>/imported/, plus a <stem>.jpg grid thumbnail. Its size:
 * the source's, fitted into the box of the setting video_resolution
 * (video_box_for: by default the largest render size of the monitors).
 * One import runs at a time; status reports progress. */

static struct {
    WalpeImport *job;        /* running (or finished, kept for status) */
    gboolean active;
    char *label;
    char *mode;
    char *error;
    int percent;
    const char *purpose;     /* "add" (library import) | "convert" (see Conv) */
} Imp;

/* copies of videos inside scenes (below): they give way to an import or a
 * conversion somebody asked for, and go on when it has ended */
static void sv_yield (void);
static void sv_start_next (void);

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
    sv_start_next ();
}

static GError *start_import (const char *abs)
{
    char *dir = walpe_expand_path (D.cfg->wallpapers_dir ? D.cfg->wallpapers_dir
                                                         : walpe_default_dir ());
    char *impdir = g_build_filename (dir, "imported", NULL);
    g_free (dir);
    GError *err = NULL;
    /* nobody shows it yet: as large as any monitor could */
    WalpeVideoBox box;
    video_box_for (NULL, NULL, TRUE, &box);
    WalpeImport *job = walpe_import_start (abs, impdir, NULL, &box, import_done, NULL, &err);
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
    sv_yield ();
    return NULL;
}

/* ------------------------------------------- video conversion on open
 * The player reads H.264 in MP4 / MOV. A video wallpaper in anything else
 * (WebM, HEVC, AV1, MKV, fragmented MP4: Workshop items of type video
 * come in those too) used to fail to load. Such a file now goes through
 * the import pipeline (ffprobe -> ffmpeg, the same rules as `walpe-ctl
 * add`: H.264 High yuv420p, no audio, fitted into the box of the monitors
 * that show it: video_box_for) once, into ~/.cache/walpe/video/<hash of
 * path + size + mtime>[-<box>].mp4, and the player shows that copy
 * (engine/video.c) while the wallpaper keeps its own path
 * in the config, the options and status. Asynchronous on the main loop:
 * the wallpaper on screen stays until the copy is ready; status shows the
 * progress through the import fields ("import_purpose": "convert"). One
 * import or conversion runs at a time. */
static struct {
    char *wallpaper;    /* shown when the copy is ready (as opened), NULL = nothing waits */
    char *mon;          /* output it was opened on, NULL = every monitor */
    gboolean commit;    /* an `open` waits (config is set then); FALSE = reload only */
    char *src;          /* the media file of the conversion that runs (NULL = none). It
                         * used to travel as the job's callback pointer, a copy only
                         * the callback freed: a conversion that could not start, or
                         * was cancelled (the callback is not called then), left it
                         * behind every time */
} Conv;
static GHashTable *conv_failed;     /* media files whose conversion failed in this run */

static void open_commit (const char *abs, Mon *tm);
static void convert_scan (void);

/* can the player show wallpaper abs now, the way it is meant to look?
 * Non-video content: yes. *src (g_free) = the video file when it cannot;
 * *why (g_free) = the reason; *soft = the file itself plays, but sideways
 * or stretched (display matrix / pixel aspect ratio, which the player does
 * not apply): worth converting, yet usable as it is when that fails.
 * box: the box a converted copy has to be made for to count
 * (video_box_for; a copy for another box is another file). */
static gboolean video_playable (const char *abs, const WalpeVideoBox *box, char **src, char **why,
                                gboolean *soft)
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
        char *cf = walpe_video_cache_file (file, box);
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
    (void) user;
    if (job != Imp.job)
        return;
    char *src = g_steal_pointer (&Conv.src);    /* the media file */
    import_done (job, ok, dst, error, NULL);
    char *wp = g_steal_pointer (&Conv.wallpaper), *mon = g_steal_pointer (&Conv.mon);
    if (!ok) {
        if (!conv_failed)
            conv_failed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        if (src)
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
 * NULL = all, when done) into the copy for box. NULL = started, else why
 * not. */
static GError *convert_start (const char *src, const char *wp, const char *mon, gboolean commit,
                              const WalpeVideoBox *box)
{
    if (Imp.active)
        return g_error_new (G_IO_ERROR, G_IO_ERROR_BUSY, "%s of %s is still running; open the "
                            "wallpaper again when it is done",
                            !g_strcmp0 (Imp.purpose, "convert") ? "the conversion" : "the import",
                            Imp.label ? Imp.label : "a video");
    char *dst = walpe_video_cache_file (src, box);
    GError *err = NULL;
    WalpeImport *job = dst ? walpe_import_start_to (src, dst, NULL, box, convert_done, NULL, &err)
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
    g_free (Conv.src);
    Conv.src = g_strdup (src);
    sv_yield ();
    if (conv_failed)
        g_hash_table_remove (conv_failed, src);
    {
        char *lim = box && box->w > 0 && box->h > 0 ? g_strdup_printf ("at most %dx%d", box->w, box->h)
                                                    : g_strdup ("at its own size");
        g_message ("video: %s cannot be shown as it is (not H.264 in MP4 / MOV, or stored "
                   "rotated): converting it once (ffmpeg, %s), the wallpaper changes when the "
                   "copy is ready", src, lim);
        g_free (lim);
    }
    return NULL;
}

/* start-up (this daemon holds the instance lock, so nothing is being
 * converted): remove the partial outputs a killed daemon left behind, and
 * copies no wallpaper has played for CONV_CACHE_DAYS (the player touches
 * the copy it plays: engine/video.c) so the cache does not grow for ever;
 * with WALPE_VIDEO_CACHE_MB also the longest unused ones beyond that size
 * (common/import.h walpe_video_cache_tidy) */
#define CONV_CACHE_DAYS 90
static void conv_cache_note (const char *path, const char *why, gpointer user)
{
    (void) user;
    g_message ("video: removing %s (%s)", path, why);
}

static void conv_cache_tidy (void)
{
    walpe_video_cache_tidy (CONV_CACHE_DAYS, conv_cache_note, NULL);
}

/* a wallpaper some monitor is set to (saved config, a forgotten cache)
 * whose video still has no playable copy: convert it, then reload it */
static void convert_scan (void)
{
    for (GList *l = D.mons; l && !Imp.active; l = l->next) {
        Mon *m = l->data;
        char *src = NULL;
        WalpeVideoBox box;
        video_box_for (m->want_path, NULL, FALSE, &box);
        if (m->want_path && *m->want_path &&
            !video_playable (m->want_path, &box, &src, NULL, NULL) && src &&
            !(conv_failed && g_hash_table_contains (conv_failed, src))) {
            GError *e = convert_start (src, m->want_path, NULL, FALSE, &box);
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

/* main: a render size or video_resolution changed. A monitor showing a
 * video from a converted copy was given the copy for the box of then
 * (Mon.load_box); when the box is another one now, the wallpaper is loaded
 * again with the copy for it - at once when that copy exists, else when
 * convert_scan () has made it (the picture on screen stays until then). A
 * video the player reads itself has no copy and is left alone. */
static void video_boxes_refresh (void)
{
    gboolean reload = FALSE;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        if (m->leader || !m->want_path || !*m->want_path ||
            (g_atomic_int_get (&m->content_type) != WALPE_TYPE_VIDEO && !m->loading))
            continue;
        WalpeVideoBox box;
        video_box_for (m->want_path, NULL, FALSE, &box);
        if (box.w == m->load_box.w && box.h == m->load_box.h)
            continue;
        char *media = NULL;
        if (walpe_detect_type (m->want_path, &media) == WALPE_TYPE_VIDEO) {
            const char *file = media ? media : m->want_path;
            char *was = walpe_video_cache_file (file, &m->load_box);
            char *now = walpe_video_cache_file (file, &box);
            /* it plays from a copy (the one of the old box exists) and
             * the one for the new box is there too */
            if (was && now && g_file_test (was, G_FILE_TEST_IS_REGULAR) &&
                g_file_test (now, G_FILE_TEST_IS_REGULAR)) {
                m->reload_wanted = TRUE;
                reload = TRUE;
            }
            g_free (was);
            g_free (now);
        }
        g_free (media);
    }
    if (reload)
        apply_wallpapers (NULL, NULL, FALSE, FALSE);
    video_boxes_update ();  /* videos inside scenes: their scenes look again */
    convert_scan ();    /* copies that do not exist for their box yet */
}

/* ---------------------------------------- copies of videos inside scenes
 * A scene's video layer that is larger than the scene shows it (a 4K clip
 * on a 1080p monitor) is decoded at its own size, four times the work of
 * what is seen. With "video_scene_convert" (on by default) the scene asks
 * for a copy of the size it needs (common/scenevideo.h: the decision and
 * the copy's name; engine/scenenode.c scene_video_plan asks) and plays the
 * clip itself until the copy exists; the video module then changes over
 * where the clip starts again (engine/videotex.h).
 *
 * Here: what the scenes on screen wait for, and the one ffmpeg that makes
 * it (common/import.h walpe_import_start_scene: the import pipeline at the
 * lowest priority, with the limits of a conversion on open). One clip at a
 * time, never beside an import or a conversion somebody asked for (those
 * come first: a copy that is being made is started again afterwards). A
 * copy no scene waits for any more - its wallpaper was replaced - is not
 * made: the ffmpeg that runs for it is stopped and its partial file
 * removed. Nothing here exists while no scene waits for a copy: no table,
 * no timer, no child. The copies share the folder and the rules of the
 * converted video wallpapers (conv_cache_tidy: unfinished files go at
 * start-up, copies nothing played for 90 days too). */
typedef struct {
    char *dst;          /* the copy (what it is known by) */
    char *src;          /* the file with the clip, and the clip in it */
    guint64 off, len;
    int w, h, matrix;
    int refs;           /* scenes that wait for it */
    char *error;        /* its conversion failed in this run: not tried again */
} SvWant;

static struct {
    GPtrArray *wants;       /* SvWant *; NULL while nothing is waited for */
    WalpeImport *job;       /* the conversion that runs, or has just ended */
    SvWant *running;        /* what it makes */
    gboolean ended;         /* job has reported: sv_after_cb takes it away */
    GHashTable *failed;     /* dst -> why, for copies no scene waits for any more */
} Sv;

static void sv_want_free (gpointer p)
{
    SvWant *w = p;
    g_free (w->dst);
    g_free (w->src);
    g_free (w->error);
    g_free (w);
}

static SvWant *sv_find (const char *dst)
{
    for (guint i = 0; Sv.wants && dst && i < Sv.wants->len; i++) {
        SvWant *w = g_ptr_array_index (Sv.wants, i);
        if (!strcmp (w->dst, dst))
            return w;
    }
    return NULL;
}

/* main: a copy is being made right now (work that a restart of the daemon
 * would throw away) */
static gboolean sv_converting (void)
{
    return Sv.job && !Sv.ended;
}

/* the job is not needed any more (done, failed, stopped) */
static void sv_job_clear (void)
{
    if (Sv.job)
        walpe_import_free (Sv.job);     /* a running ffmpeg is killed, its partial file removed */
    Sv.job = NULL;
    Sv.running = NULL;
    Sv.ended = FALSE;
}

static gboolean sv_after_cb (gpointer user)
{
    (void) user;
    if (Sv.job && Sv.ended)
        sv_job_clear ();
    sv_start_next ();
    return G_SOURCE_REMOVE;
}

static void sv_done (WalpeImport *job, gboolean ok, const char *dst, const char *error,
                     gpointer user)
{
    (void) job;
    (void) user;
    /* (called from inside the job, also before walpe_import_start_scene has
     * returned: the result is noted here, the job goes from the main loop.
     * A job that was stopped does not report.) */
    SvWant *w = Sv.running;
    if (w && !ok) {
        g_free (w->error);
        w->error = g_strdup (error && *error ? error : "the conversion failed");
        g_warning ("video: no copy of %s for its scene (%s): the clip is played as it is",
                   w->src, w->error);
    } else if (w) {
        g_message ("video: copy of %s for its scene made: %s (%dx%d)", w->src, dst, w->w, w->h);
    }
    Sv.ended = TRUE;
    walpe_scene_video_touch ();     /* the scenes look again: they play it, or say why not */
    for (GList *l = D.mons; l; l = l->next)
        if (g_atomic_int_get (&((Mon *) l->data)->content_type) == WALPE_TYPE_SCENE)
            request_redraw (l->data);
    g_idle_add (sv_after_cb, NULL);
}

/* main: make the next copy that is waited for, unless something runs */
static void sv_start_copy (void);
static void sv_start_next (void)
{
    sv_start_copy ();
    /* (called when a copy, an import or a conversion has ended or was
     * stopped: what kept a restart waiting may be gone) */
    web_unload_hidden ();
}

static void sv_start_copy (void)
{
    if (Sv.job && Sv.ended)
        sv_job_clear ();
    if (Sv.job || Imp.active || !Sv.wants)
        return;
    for (guint i = 0; i < Sv.wants->len; i++) {
        SvWant *w = g_ptr_array_index (Sv.wants, i);
        if (w->refs <= 0 || w->error)
            continue;
        if (g_file_test (w->dst, G_FILE_TEST_IS_REGULAR)) {
            walpe_scene_video_touch ();     /* it is there already: the scene finds it */
            continue;
        }
        /* (which encoding: the copy's name says it, scenevideo.h) */
        const gboolean fast = walpe_scene_video_copy_is_fast (w->dst);
        const WalpeImportScene sc = { w->off, w->len, w->w, w->h, w->matrix, fast };
        GError *err = NULL;
        Sv.ended = FALSE;
        Sv.running = w;
        Sv.job = walpe_import_start_scene (w->src, &sc, w->dst, sv_done, NULL, &err);
        if (Sv.job) {
            g_message ("video: %s is larger than its scene shows it: making a %dx%d copy%s once, "
                       "in the background (%s); the clip plays as it is until then", w->src,
                       w->w, w->h, fast ? " that is cheap to decode" : "", w->dst);
            return;
        }
        Sv.running = NULL;
        w->error = g_strdup (err ? err->message : "the conversion could not be started");
        g_warning ("video: no copy of %s for its scene: %s", w->src, w->error);
        g_clear_error (&err);
        walpe_scene_video_touch ();
    }
}

/* main: an import or a conversion somebody asked for starts: the copy that
 * is being made waits (it is started again when that has ended) */
static void sv_yield (void)
{
    if (sv_converting ()) {
        g_message ("video: the copy of %s for its scene waits for the %s", Sv.running->src,
                   !g_strcmp0 (Imp.purpose, "convert") ? "conversion" : "import");
        sv_job_clear ();
    }
}

typedef struct {
    WalpeSceneVideoWant w;
    gboolean wanted;
} SvMsg;

/* main (idle, queued by sv_hook): a scene waits for a copy, or no longer */
static gboolean sv_want_cb (gpointer user)
{
    SvMsg *msg = user;
    SvWant *w = sv_find (msg->w.dst);
    if (msg->wanted) {
        if (!w) {
            w = g_new0 (SvWant, 1);
            w->dst = g_strdup (msg->w.dst);
            w->src = g_strdup (msg->w.src);
            w->off = msg->w.off;
            w->len = msg->w.len;
            w->w = msg->w.w;
            w->h = msg->w.h;
            w->matrix = msg->w.matrix;
            const char *why = Sv.failed ? g_hash_table_lookup (Sv.failed, w->dst) : NULL;
            w->error = g_strdup (why);
            if (!Sv.wants)
                Sv.wants = g_ptr_array_new_with_free_func (sv_want_free);
            g_ptr_array_add (Sv.wants, w);
        }
        w->refs++;
        sv_start_next ();
    } else if (w && --w->refs <= 0) {
        if (Sv.running == w) {
            if (!Sv.ended)
                g_message ("video: no scene waits for the copy of %s any more: its conversion "
                           "is stopped", w->src);
            sv_job_clear ();
        }
        if (w->error) {     /* (not tried again when its scene is loaded again) */
            if (!Sv.failed)
                Sv.failed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
            g_hash_table_replace (Sv.failed, g_strdup (w->dst), g_strdup (w->error));
        }
        g_ptr_array_remove_fast (Sv.wants, w);
        if (!Sv.wants->len)
            g_clear_pointer (&Sv.wants, g_ptr_array_unref);
        sv_start_next ();
    }
    g_free ((char *) msg->w.src);
    g_free ((char *) msg->w.dst);
    g_free (msg);
    return G_SOURCE_REMOVE;
}

/* a render thread (common/scenevideo.h walpe_scene_video_want) */
static void sv_hook (const WalpeSceneVideoWant *want, gboolean wanted)
{
    SvMsg *msg = g_new0 (SvMsg, 1);
    msg->w = *want;
    msg->w.src = g_strdup (want->src);
    msg->w.dst = g_strdup (want->dst);
    msg->wanted = wanted;
    g_idle_add (sv_want_cb, msg);
}

/* status "scene_videos": what the daemon does about the copy a scene waits
 * for (videotex.h WalpeVideoJobText; called with the video module's locks
 * held, so nothing here calls into the module) */
static char *sv_job_text (const char *copy, gpointer user)
{
    (void) user;
    const SvWant *w = sv_find (copy);
    const char *why = w && w->error ? w->error
                    : Sv.failed ? g_hash_table_lookup (Sv.failed, copy) : NULL;
    if (why)
        return g_strdup_printf ("failed (%s)", why);
    if (w && Sv.running == w && sv_converting ()) {
        int pc = walpe_import_percent (Sv.job);
        /* (the kind that is not the default says so, here and when it plays) */
        const char *kind = walpe_scene_video_copy_is_fast (copy) ? ", encoded for cheap decoding" : "";
        return pc >= 0 ? g_strdup_printf ("converting %d %%%s", pc, kind)
                       : g_strdup_printf ("converting%s", kind);
    }
    return g_strdup (Imp.active ? "waiting for the running import" : "waiting");
}

/* main: the setting changed. Scenes look at their videos with their next
 * frame (the copy is played from where the clip starts again, or the clip
 * itself is); off: nothing more is converted. */
static void sv_setting (gboolean on)
{
    walpe_scene_video_set_on (on);
    for (GList *l = D.mons; l; l = l->next)
        if (g_atomic_int_get (&((Mon *) l->data)->content_type) == WALPE_TYPE_SCENE)
            request_redraw (l->data);
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
    web_unload_hidden ();
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

/* "k": [w, h] */
static void jb_size (JsonBuilder *b, const char *k, int w, int h)
{
    json_builder_set_member_name (b, k);
    json_builder_begin_array (b);
    json_builder_add_int_value (b, w);
    json_builder_add_int_value (b, h);
    json_builder_end_array (b);
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

/* setprop / setprops: WalPE's own two sound options of a wallpaper, its
 * switch "walpe_sound" (on | off | anything else: as the setting says) and
 * its volume "walpe_volume" (0..1, a factor on the volume setting). They
 * change nothing a scene draws, so the wallpaper is not loaded again for
 * them - its sound would start from the beginning and the picture would
 * blink for a volume slider: the players of the loaded wallpaper take both
 * at once. FALSE: the wallpaper has no players (not loaded, no sound
 * layers): the caller loads it again as for any option. */
static gboolean prop_is_sound (const char *key)
{
    return !strcmp (key, "walpe_sound") || !strcmp (key, "walpe_volume");
}

static gboolean sound_props_apply (const char *wp)
{
    char *sw = walpe_config_get_prop (wp, "walpe_sound"), *vol = walpe_config_get_prop (wp, "walpe_volume");
    guint n = walpe_sound_wallpaper_set (wp, walpe_sound_switch_parse (sw), walpe_sound_volume_parse (vol));
#ifdef HAVE_WEB
    /* a web wallpaper's page takes them too, without being loaded again */
    n += walpe_web_sound_wallpaper (wp, walpe_sound_switch_parse (sw), walpe_sound_volume_parse (vol));
#endif
    g_free (sw);
    g_free (vol);
    return n > 0;
}

#include "engine/soundserver.h"   /* "soundcheck": the page's stream in the sound server */
#include "layers_ipc.h"   /* "layers" / "layeradj": ipc_layers, ipc_layeradj */
#include "common/setkeys.h"   /* "set": the members it may carry (walpe_set_check) */
#include "common/setvalues.h"   /* walpe_set_values_check: the values of a "set" request */

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
        if (!json_object_has_member (defs, keys->pdata[i]) &&
            strcmp (keys->pdata[i], "walpe_sound") != 0 &&   /* WalPE's own: a scene's sound switch */
            strcmp (keys->pdata[i], "walpe_volume") != 0)    /* ... and its own volume */
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

/* status: the window system in use, how it was chosen, what this build
 * holds, and for every pause signal who reports it in this session ("" =
 * nobody: the rule that reads it cannot fire) */
static WalpeOutputList layout_now;   /* (the layout code further down fills it) */
static char *mons_no_surface;        /* outputs that got no wallpaper surface at start, by name
                                      * (NULL = every one got its own; diag.h) */

#ifdef WALPE_FOLLOW
static int mons_waiting (void);
#endif

static void status_backend (JsonBuilder *b)
{
    jb_str (b, "display_backend", walpe_backend_name (D.backend));
    jb_str (b, "display_backend_setting", walpe_backend_name (D.cfg->backend));
    jb_str (b, "display_backend_source", Bk.source);
    jb_str (b, "display_backend_why", Bk.why);
    jb_str (b, "display_backends_built",
            walpe_backends_built (walpe_platform_built (WALPE_BACKEND_X11),
                                  walpe_platform_built (WALPE_BACKEND_WAYLAND)));
    jb_str (b, "wayland_pointer", D.cfg->wayland_pointer ? "off" : "auto");
    if (mons_no_surface)    /* outputs the daemon shows nothing on: no surface at start */
        jb_str (b, "monitors_without_surface", mons_no_surface);
#ifdef WALPE_FOLLOW
    /* render threads that wait for an output (their monitor's output went:
     * layout_follow); absent on a window system where none ever does */
    if (walpe_platform_follows_outputs ())
        jb_int (b, "render_threads_waiting", mons_waiting ());
#endif
    json_builder_set_member_name (b, "pause_signals");
    json_builder_begin_object (b);
    for (int s = 0; s < WALPE_SIG_N; s++) {
        json_builder_set_member_name (b, walpe_signal_name (s));
        json_builder_begin_object (b);
        jb_str (b, "from", walpe_signal_sources (s));
        jb_str (b, "missing", walpe_signal_missing (s));
        json_builder_end_object (b);
    }
    json_builder_end_object (b);
    jb_bool (b, "idle", D.idle);
    /* per pause setting: does anything in this session drive it, and if
     * not, why (signals/rules.h; the GUI prints the note under the switch) */
    walpe_sigprov_status_json (b, D.cfg);
}

/* the output a monitor is on, NULL = none (--windowed) */
static const WalpeOutput *mon_output (Mon *m)
{
    for (int i = 0; i < layout_now.n; i++)
        if (layout_now.o[i].id == m->out_id && !g_strcmp0 (layout_now.o[i].name, m->name))
            return &layout_now.o[i];
    return NULL;
}

/* status, per monitor: what the window system says about its output beyond
 * name and rectangle, and (where the backend counts them) what became of
 * the frames it was given */
static void status_monitor_output (JsonBuilder *b, Mon *m)
{
    const WalpeOutput *o = mon_output (m);
    if (o) {
        jb_str (b, "canon", o->canon);
        if (o->make)
            jb_str (b, "make", o->make);
        if (o->model)
            jb_str (b, "model", o->model);
        if (o->serial)
            jb_str (b, "serial", o->serial);
        if (o->description)
            jb_str (b, "description", o->description);
        if (o->scale < 0.999 || o->scale > 1.001) {
            json_builder_set_member_name (b, "scale");
            json_builder_add_double_value (b, o->scale);
        }
        if (o->transform)
            jb_int (b, "transform", o->transform);
        if (o->logical_x != o->x || o->logical_y != o->y || o->logical_width != o->width ||
            o->logical_height != o->height) {
            json_builder_set_member_name (b, "logical");
            json_builder_begin_array (b);
            json_builder_add_int_value (b, o->logical_x);
            json_builder_add_int_value (b, o->logical_y);
            json_builder_add_int_value (b, o->logical_width);
            json_builder_add_int_value (b, o->logical_height);
            json_builder_end_array (b);
        }
    }
    WalpePresentStats ps;
    if (walpe_surface_present_stats (m->ps, &ps)) {
        jb_int (b, "presented", (gint64) ps.presented);
        jb_int (b, "discarded", (gint64) ps.discarded);
        jb_int (b, "wait_timeouts", (gint64) ps.wait_timeouts);
        jb_bool (b, "hidden", ps.hidden);
        if (ps.refresh_hz > 0.0) {
            json_builder_set_member_name (b, "refresh_feedback");
            json_builder_add_double_value (b, ps.refresh_hz);
        }
    }
}

/* status "sound": the player's line (scenes, videos) with the pages of web
 * wallpapers in it (scenesound.h walpe_sound_status_with) - a page's sound is
 * WebKit's, the player has no stream for it, and its line alone said
 * "nothing is playing" under a page that was heard. A page is the wallpaper
 * of one or more monitors (shared); it is paused when all of them are. Main
 * thread, only when somebody asks. */
static void sound_pages_count (WalpeSoundPages *out)
{
    WalpeSoundPages pages = { 0 };
#ifdef HAVE_WEB
    GHashTable *seen = NULL;    /* wallpaper path -> one of its monitors runs */
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        const char *wp = m->want_path ? m->want_path : walpe_config_monitor_path (D.cfg, m->name);
        if (g_atomic_int_get (&m->content_type) != WALPE_TYPE_WEB || !wp || !*wp)
            continue;
        g_mutex_lock (&m->lock);
        const gboolean runs = !m->pause_wanted;
        g_mutex_unlock (&m->lock);
        if (!seen)
            seen = g_hash_table_new (g_str_hash, g_str_equal);
        g_hash_table_insert (seen, (gpointer) wp,
                             GINT_TO_POINTER (runs || GPOINTER_TO_INT (g_hash_table_lookup (seen, wp))));
    }
    if (seen) {
        GHashTableIter it;
        gpointer wp, runs;
        g_hash_table_iter_init (&it, seen);
        while (g_hash_table_iter_next (&it, &wp, &runs)) {
            char *sw = walpe_config_get_prop (wp, "walpe_sound"), *vol = walpe_config_get_prop (wp, "walpe_volume");
            walpe_sound_pages_add (&pages, walpe_sound_switch_parse (sw), walpe_sound_volume_parse (vol),
                                   !GPOINTER_TO_INT (runs), D.cfg->scene_sound, D.cfg->mute, D.cfg->volume);
            g_free (sw);
            g_free (vol);
        }
        g_hash_table_destroy (seen);
    }
#endif
    *out = pages;
}

static char *sound_status_line (void)
{
    WalpeSoundPages pages;
    sound_pages_count (&pages);
    return walpe_sound_status_with (&pages);
}

/* {"cmd":"soundcheck"} (`walpe-ctl soundcheck`): the status line "sound",
 * and - only while a web wallpaper is shown at a level above 0, the one
 * case in which WalPE's settings cannot say why nothing is heard - a look
 * at the sound server: is the page's stream muted or at 0 % there
 * (soundserver.h). One child process for this request; nothing is asked
 * of the server otherwise, by anything. */
static char *sound_server_line (void)
{
    WalpeSoundPages pages;
    sound_pages_count (&pages);
    if (pages.heard <= 0)
        return g_strdup ("not asked: no web wallpaper is shown at a level above 0 (the sound of scenes and "
                         "videos is WalPE's own stream \"walpe\", which the line above counts)");
    gboolean asked = FALSE;
    char *note = walpe_sound_server_note (&asked);
    if (note)
        return note;
    return g_strdup (asked ? "no stream of WebKit is muted or at 0 % in the sound server (a page that is silent "
                             "now plays nothing, or its stream is not there yet)"
                           : "could not be asked: `pactl -f json list sink-inputs` did not answer (no pactl, "
                             "one older than 16, or no sound server)");
}

static void ipc_soundcheck (JsonBuilder *b)
{
    char *snd = sound_status_line (), *srv = sound_server_line ();
    jb_bool (b, "ok", TRUE);
    jb_str (b, "sound", snd);
    jb_str (b, "sound_server", srv);
    g_free (snd);
    g_free (srv);
}

static void ipc_status (JsonBuilder *b)
{
    Mon *m0 = D.mons ? D.mons->data : NULL;
    walpe_memc_inc (WALPE_MEMC_STATUS);
    jb_bool (b, "ok", TRUE);
    json_builder_set_member_name (b, "state");
    json_builder_begin_object (b);
    jb_str (b, "path", D.cfg->current);
    jb_str (b, "type", m0 ? walpe_type_name (g_atomic_int_get (&m0->content_type)) : "none");
    /* the tree this daemon was built from (common/sourceid.h): test builds
     * and the release they follow answer the same version */
    jb_str (b, "source_id", walpe_source_id ());
    jb_bool (b, "paused", D.cfg->paused);
    /* the fullscreen rule pauses a monitor (which one: "monitors" below) */
    jb_bool (b, "fullscreen_pause", any_fullscreen_pause ());
    /* a fullscreen window has the focus, whatever the pause setting says
     * and whichever monitor it is on (a client that wants to show
     * something waits until it has not) */
    jb_bool (b, "fullscreen", D.fs_active);
    jb_bool (b, "dpms_pause", D.cfg->pause_dpms && D.asleep);
    jb_bool (b, "covered_pause", D.cfg->pause_covered && m0 && m0->covered);
    /* the opt-in rules */
    jb_str (b, "focus_action", walpe_focus_action_name (D.cfg->focus_action));
    jb_bool (b, "on_battery", D.on_battery);
    jb_bool (b, "battery_pause", D.cfg->pause_battery && D.on_battery);
    jb_bool (b, "idle_pause", D.cfg->pause_idle > 0 && walpe_signal_get (WALPE_SIG_IDLE, 0));
    /* The saved settings themselves, under their config.json names. Status
     * had what results from them at the moment ("fullscreen_pause" = the
     * setting AND a fullscreen window) but not the settings: after
     * `walpe-ctl set covered=off` nothing in status said so. */
    jb_bool (b, "pause_fullscreen", D.cfg->pause_fullscreen);
    jb_bool (b, "pause_fullscreen_all", D.cfg->pause_fullscreen_all);
    jb_bool (b, "pause_dpms", D.cfg->pause_dpms);
    jb_bool (b, "pause_covered", D.cfg->pause_covered);
    jb_int (b, "pause_idle", D.cfg->pause_idle);     /* seconds; "idle_pause" above = it applies now */
    jb_bool (b, "cover_transparent", D.cfg->cover_transparent);
    jb_str (b, "transparent_windows", D.cfg->transparent_windows ? D.cfg->transparent_windows : "");
    jb_int (b, "focus_fps", D.cfg->focus_fps);
    jb_bool (b, "pause_battery", D.cfg->pause_battery);
    jb_int (b, "battery_fps", D.cfg->battery_fps);
    jb_str (b, "preview_anim", walpe_preview_anim_name (D.cfg->preview_anim));
    jb_bool (b, "render_shared", D.cfg->render_shared);
    jb_bool (b, "parallax", g_atomic_int_get (&D.cfg->parallax) != 0);
    jb_bool (b, "tex_downscale", g_atomic_int_get (&D.cfg->tex_downscale) != 0);
    /* ("software_threads" below is the count in use, 0 while a GPU renders) */
    jb_int (b, "software_threads_saved", D.cfg->software_threads);
    {
        /* per monitor: paused, fullscreen (the rule pauses it), covered,
         * following (render_shared) */
        GString *mons = g_string_new (NULL);
        int shared = 0;
        for (GList *l = D.mons; l; l = l->next) {
            Mon *m = l->data;
            g_string_append_printf (mons, "%smon%d:%s%s%s%s", mons->len ? " " : "", m->index,
                                    m->own_paused ? "paused" : "running",
                                    mon_fullscreen_pause (m) ? ",fullscreen" : "",
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
        /* (also while the setting is off: they are saved settings) */
        jb_int (b, "skip_hidden_margin", D.cfg->skip_hidden_margin);
        jb_int (b, "skip_hidden_inset", D.cfg->skip_hidden_inset);
        if (D.vis) {
            WalpeVisStats vs;
            walpe_vistrack_stats (D.vis, &vs);
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
        /* the same setting as its word (off | auto | the one API asked for);
         * what really decodes is "video_decoder" below */
        jb_str (b, "video_hwdec_api", walpe_config_hwdec_word (D.cfg));
        jb_bool (b, "video_zerocopy", D.cfg->video_zerocopy);
        /* simulation effects (fx_sim.h): the saved settings; WALPE_FLUID /
         * WALPE_FLUID_QUALITY in the daemon's environment override them */
        jb_bool (b, "fluid_sim", D.cfg->fluid_sim);
        jb_str (b, "fluid_quality", walpe_fluid_quality_name ((WalpeFluidQuality) D.cfg->fluid_quality));
        jb_bool (b, "video_limit", D.cfg->video_limit);
        jb_bool (b, "video_scene_convert", D.cfg->video_scene_convert);
        {
            /* the videos inside the scenes on screen: the clip's size and
             * frame rate, what is decoded, and the state of its converted
             * copy (ready, converting NN %, off, not needed, ...) */
            char *sn = walpe_video_scene_note (sv_job_text, NULL);
            if (sn)
                jb_str (b, "scene_videos", sn);
            g_free (sn);
        }
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
        {
            /* renderer features (engine/features.h) as they are now: one
             * line for people, one object for programs (a settings window
             * shows the switches from it); "features_fixed" names the ones
             * WALPE_FEATURES in the daemon's environment decides */
            GString *fs = g_string_new (NULL), *fx = g_string_new (NULL);
            for (int f = 0; walpe_feature_name (f); f++) {
                g_string_append_printf (fs, "%s%s=%s", f ? " " : "", walpe_feature_name (f),
                                        walpe_feature ((WalpeFeature) f) ? "on" : "off");
                if (walpe_feature_fixed (f))
                    g_string_append_printf (fx, "%s%s", fx->len ? " " : "", walpe_feature_name (f));
            }
            jb_str (b, "features", fs->str);
            if (fx->len)
                jb_str (b, "features_fixed", fx->str);
            g_string_free (fs, TRUE);
            g_string_free (fx, TRUE);
            json_builder_set_member_name (b, "feature_switches");
            json_builder_begin_object (b);
            for (int f = 0; walpe_feature_name (f); f++)
                jb_bool (b, walpe_feature_name (f), walpe_feature ((WalpeFeature) f));
            json_builder_end_object (b);
        }
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
        walpe_gpustat_set_gl_vendor (Sw.gl_vendor);
        g_mutex_unlock (&Sw.lock);
    }
    /* which GPU: the setting, the device in use, the candidates */
    gpu_status_json (b);
    {
        /* the daemon on the GPU as the kernel accounts it (common/gpustat.h):
         * device and driver, where the figures come from, the daemon's GPU
         * memory and its share of the GPU since the status request before.
         * This process's own fdinfo only, read at most once a second; a
         * figure the driver does not publish is left out and "note" says
         * why (the NVIDIA driver publishes none: walpe-ctl gpu-stats asks
         * nvidia-smi). */
        WalpeGpuSelfStat gs;
        walpe_gpustat_self (&gs);
        json_builder_set_member_name (b, "gpu_stats");
        json_builder_begin_object (b);
        jb_str (b, "driver", gs.driver);
        jb_str (b, "vendor", gs.vendor_name);
        jb_str (b, "pci_slot", gs.pci_slot);
        jb_str (b, "card", gs.card);
        jb_str (b, "sources", gs.sources);
        if (gs.has_busy) {
            json_builder_set_member_name (b, "daemon_busy_percent");
            json_builder_add_double_value (b, gs.busy_pct);
            json_builder_set_member_name (b, "busy_seconds");
            json_builder_add_double_value (b, gs.busy_window_s);
            jb_str (b, "engine", gs.engine);
        }
        if (gs.mem_kb >= 0)
            jb_int (b, "daemon_memory_kib", gs.mem_kb);
        if (gs.note[0])
            jb_str (b, "note", gs.note);
        json_builder_end_object (b);
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
    /* wallpaper sound: the two switches, and in one line what is heard or
     * why nothing is (engine/scenesound.h) */
    jb_bool (b, "scene_sound", D.cfg->scene_sound);
    jb_bool (b, "mute", D.cfg->mute);
    {
        char *snd = sound_status_line ();
        jb_str (b, "sound", snd);
        g_free (snd);
    }
    /* the video backend that plays: FFmpeg, the one there is. (The stored
     * word was shown here, and after `set video_backend=mpv` - still taken,
     * for files and clients of earlier versions - status said "mpv" while
     * the refusal of any other word says that FFmpeg is the one backend.) */
    jb_str (b, "backend", WALPE_VIDEO_BACKEND);
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
        /* a focused fullscreen window is on it (whatever the settings
         * say), and: the fullscreen rule pauses it (pause_fullscreen; with
         * pause_fullscreen_all also for a window on another monitor) */
        jb_bool (b, "fullscreen", mon_fullscreen (m));
        jb_bool (b, "fullscreen_pause", mon_fullscreen_pause (m));
        jb_bool (b, "covered", m->covered);
        jb_bool (b, "focused_app", m->focus_app);      /* an application on it has the focus */
        jb_int (b, "fps_limit", m->fps_cap);            /* what its render thread was given */
        json_builder_set_member_name (b, "refresh");   /* Hz, 0 = unknown */
        json_builder_add_double_value (b, m->hz);
        {
            /* render resolution: the output's mode, the choice for it, the
             * size its wallpaper is rendered at, and whether anybody was
             * asked ({"cmd":"resolution"} has the rest) */
            WalpeMonitorConf mc;
            res_conf_for (m->name, &mc);
            char *rs = walpe_res_format (&mc.res), *tl = walpe_texlimit_format (mc.tex_limit);
            jb_size (b, "mode", m->geom.width, m->geom.height);
            jb_str (b, "resolution", rs);
            jb_size (b, "render_size", m->res_w, m->res_h);
            jb_bool (b, "resolution_asked", mon_res_asked (m, &mc));
            jb_str (b, "texture_limit", tl);
            if (m->res_note)
                jb_str (b, "resolution_note", m->res_note);
            g_free (rs);
            g_free (tl);
        }
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
        {
            /* its swaps fail and say that the GL context or the surface is
             * gone (mon_swap_failed); absent while it presents */
            const EGLint se = (EGLint) g_atomic_int_get (&m->swap_lost);
            if (se) {
                char *t = g_strdup_printf ("%s (0x%x): the GL context or the window's surface is "
                                           "gone, nothing is presented", swap_err_name (se), (guint) se);
                jb_str (b, "swap_error", t);
                g_free (t);
            }
        }
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
        status_monitor_output (b, m);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    jb_int (b, "monitor_count", g_list_length (D.mons));
    /* render resolution: outputs nobody was asked about yet (the GUI asks
     * while resolution_prompt is on), and the global settings */
    jb_int (b, "resolution_unasked", res_unasked ());
    jb_bool (b, "resolution_prompt", D.cfg->resolution_prompt);
    jb_str (b, "scale_filter", walpe_scale_filter_name (g_atomic_int_get (&D.cfg->scale_filter)));
    {
        char *vr = walpe_vidres_format (&D.cfg->video_res);
        jb_str (b, "video_resolution", vr);
        g_free (vr);
    }
    if (res_env)    /* WALPE_RESOLUTION decides the sizes while it is set */
        jb_bool (b, "resolution_env", TRUE);
    /* asked now (X11: one request; a Wayland compositor always composes) */
    jb_bool (b, "compositor", D.backend == WALPE_BACKEND_WAYLAND || present_composited ());
    jb_bool (b, "frame_align", walpe_frame_align_get ());
    /* the backend's own keys (X11: "x_requests", the requests the main
     * thread has sent to the X server since start; this status call adds
     * a few) */
    walpe_platform_status (b);
    status_backend (b);     /* which window system, who reports the pause signals */
    /* props/setprop/setprops/resetprops take an optional "path" (any
     * wallpaper, shown or not) besides "monitor" */
    jb_bool (b, "props_by_path", TRUE);
    /* "layers" / "layeradj" requests exist ("Adjust layers") */
    jb_bool (b, "layer_adjust", TRUE);
    jb_str (b, "gl", g_atomic_int_get (&D.egl_is_es) ? "gles" : "gl");
    {
        /* which OpenGL API renders and why; on OpenGL ES, what the path
         * does differently: every item of the table (common/glapi.h) that
         * is replaced or off, and the off ones by name */
        char ver[32] = "";
        if (Ga.decided)
            walpe_gl_rung_name (&Ga.choice.got, ver, sizeof ver);
        /* the saved setting (or WALPE_GL_API while it is set): a change
         * shows here at once, in "gl" after the restart */
        jb_str (b, "gl_api", walpe_gl_api_name (Ga.env_forced ? Ga.pref
                                                              : (WalpeGlApiPref) D.cfg->gl_api));
        jb_bool (b, "gl_api_env", Ga.env_forced);
        jb_str (b, "gl_context", ver);     /* the version asked for; "gl_version" is the driver's string */
        jb_str (b, "gl_api_note", Ga.choice.why);
        jb_str (b, "gl_api_tried", Ga.choice.tried);
        jb_int (b, "glsl_es", walpe_gl_feat_essl ());
        const WalpeGlfItem *plan = walpe_gl_feat_plan ();
        json_builder_set_member_name (b, "gl_features");
        json_builder_begin_array (b);
        for (int i = 0; i < WALPE_GLF_N; i++) {
            if (plan[i].how != WALPE_GLF_REPLACED && plan[i].how != WALPE_GLF_OFF)
                continue;
            json_builder_begin_object (b);
            jb_str (b, "name", plan[i].name);
            jb_str (b, "state", walpe_glf_how_name (plan[i].how));
            jb_str (b, "via", plan[i].via);
            jb_str (b, "effect", plan[i].effect);
            json_builder_end_object (b);
        }
        json_builder_end_array (b);
        json_builder_set_member_name (b, "gl_features_off");
        json_builder_begin_array (b);
        for (int i = 0; i < WALPE_GLF_N; i++)
            if (plan[i].how == WALPE_GLF_OFF)
                json_builder_add_string_value (b, plan[i].name);
        json_builder_end_array (b);
    }
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
    /* where the pages' windows live: "x11" or "private" (engine/webhostsel.h) */
    char *wh = walpe_web_host ();
    if (*wh)
        jb_str (b, "web_host", wh);
    g_free (wh);
    /* how pages are rendered, and what WebGL runs on inside them */
    jb_bool (b, "web_accel", D.cfg->web_accel);
    /* the daemon restarts in place when the last web wallpaper is replaced */
    jb_bool (b, "web_unload", D.cfg->web_unload);
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
    if (walpe_config_save_error ())   /* the last save of config.json failed: what runs is not saved */
        jb_str (b, "config_save_error", walpe_config_save_error ());
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
    if (restart_failed)     /* a restart in place that could not be begun (restart_begin) */
        jb_str (b, "restart_failed", restart_failed);
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
    walpe_memc_inc (WALPE_MEMC_STATE_PUSH);
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

/* ---- {"cmd":"resolution"}: the render resolution per output ------------
 * Without other members: the listing (res_describe). With "monitor" (an
 * index, an output name, "all") and "value" / "texture_limit" / "asked" /
 * "forget", or with "set": [ such objects ]: every element is checked
 * first, then all are applied, saved once and followed by one reload per
 * scene; one element that is not right and nothing is changed. "prompt":
 * may the GUI ask. The reply to a change is the listing again. */
typedef struct {
    char *name;         /* output name (connected or not) */
    gboolean has_res, has_tex, forget;
    WalpeRes res;
    int tex;
    int asked;          /* -1 = as it follows from the rest */
} ResChange;

static void res_change_clear (gpointer p)
{
    g_free (((ResChange *) p)->name);
}

static Mon *mon_by_name (const char *name)
{
    for (GList *l = D.mons; name && l; l = l->next)
        if (!g_strcmp0 (((Mon *) l->data)->name, name))
            return l->data;
    return NULL;
}

/* a "texture_limit" member: text, a number of pixels, or a switch (off =
 * none, on = auto) */
static gboolean res_texlimit_node (JsonNode *n, int *out, char **err)
{
    if (!n || !JSON_NODE_HOLDS_VALUE (n)) {
        *err = g_strdup ("texture_limit needs auto, none or a number of pixels");
        return FALSE;
    }
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_BOOLEAN) {
        *out = json_node_get_boolean (n) ? WALPE_TEXLIMIT_AUTO : WALPE_TEXLIMIT_NONE;
        return TRUE;
    }
    if (t == G_TYPE_STRING)
        return walpe_texlimit_parse (json_node_get_string (n), out, err);
    char num[32];
    double v = json_node_get_double (n);
    g_snprintf (num, sizeof (num), "%.0f", v >= 0.0 && v < 1e9 ? v : 1e9);
    return walpe_texlimit_parse (num, out, err);
}

/* one element of a request into changes (one per output it names).
 * FALSE + *err: nothing was added that the caller keeps. notes: sentences
 * for the reply. */
static gboolean res_parse_element (JsonObject *e, GArray *out, GString *notes, char **err)
{
    /* which outputs */
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    JsonNode *mn = e ? json_object_get_member (e, "monitor") : NULL;
    gboolean all = !mn || JSON_NODE_HOLDS_NULL (mn);
    if (mn && JSON_NODE_HOLDS_VALUE (mn)) {
        GType t = json_node_get_value_type (mn);
        if (t == G_TYPE_STRING) {
            const char *nm = json_node_get_string (mn);
            if (!strcmp (nm, "*") || !g_ascii_strcasecmp (nm, "all"))
                all = TRUE;
            else if (walpe_monitor_name_ok (nm))
                g_ptr_array_add (names, g_strdup (nm));
            else
                *err = g_strdup ("resolution: \"monitor\" is not an output name (1 to 64 "
                                 "characters; see walpe-ctl resolution)");
        } else if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE) {
            Mon *m = mon_by_index ((int) CLAMP (json_node_get_double (mn), -1.0, 100000.0));
            if (m)
                g_ptr_array_add (names, g_strdup (m->name));
            else
                *err = g_strdup ("resolution: no such monitor (walpe-ctl resolution lists them)");
        } else {
            *err = g_strdup ("resolution: \"monitor\" is an index, an output name or \"all\"");
        }
    } else if (mn && !all) {
        *err = g_strdup ("resolution: \"monitor\" is an index, an output name or \"all\"");
    }
    for (GList *l = D.mons; all && !*err && l; l = l->next)
        g_ptr_array_add (names, g_strdup (((Mon *) l->data)->name));
    /* what to set */
    ResChange c = { NULL, FALSE, FALSE, FALSE, { WALPE_RES_NATIVE, 0, 0, 0 }, 0, -1 };
    if (!*err && e && json_object_has_member (e, "value")) {
        JsonNode *vn = json_object_get_member (e, "value");
        if (JSON_NODE_HOLDS_VALUE (vn) && json_node_get_value_type (vn) == G_TYPE_STRING)
            c.has_res = walpe_res_parse (json_node_get_string (vn), &c.res, err);
        else
            *err = g_strdup ("resolution: \"value\" is native, 720p, 1080p, 1440p, 4k or "
                             "\"<width>x<height>\"");
    }
    if (!*err && e && json_object_has_member (e, "texture_limit"))
        c.has_tex = res_texlimit_node (json_object_get_member (e, "texture_limit"), &c.tex, err);
    if (!*err && e && json_object_has_member (e, "asked")) {
        JsonNode *an = json_object_get_member (e, "asked");
        if (JSON_NODE_HOLDS_VALUE (an) && json_node_get_value_type (an) == G_TYPE_BOOLEAN)
            c.asked = json_node_get_boolean (an) ? 1 : 0;
        else
            *err = g_strdup ("resolution: \"asked\" is true or false");
    }
    c.forget = !*err && e && wpj_bool (e, "forget", FALSE);
    if (!*err && !c.has_res && !c.has_tex && c.asked < 0 && !c.forget)
        *err = g_strdup ("resolution: nothing to set (\"value\", \"texture_limit\", "
                         "\"asked\" or \"forget\")");
    if (!*err && c.forget && (c.has_res || c.has_tex || c.asked >= 0))
        *err = g_strdup ("resolution: \"forget\" goes alone (it drops the stored choice)");
    for (guint i = 0; !*err && i < names->len; i++) {
        const char *nm = g_ptr_array_index (names, i);
        Mon *m = mon_by_name (nm);
        if (!walpe_monitor_name_ok (nm)) {
            /* (only through "all" or an index: a name in the request was
             * checked above.) Nothing can be stored under such a name
             * (walpe_config_set_monitor): say so instead of answering as
             * if the choice had been taken. */
            g_string_append_printf (notes, "%smonitor %d: its output name cannot be stored "
                                    "(longer than 64 bytes or not plain text): it stays at "
                                    "its own resolution", notes->len ? "; " : "",
                                    m ? m->index : -1);
            continue;
        }
        if (c.has_res && m) {
            /* the size on this output: one the GL can make a picture of */
            int w = 0, h = 0;
            const int mx = g_atomic_int_get (&gl_max_side);
            walpe_res_size (&c.res, m->geom.width, m->geom.height, &w, &h);
            if (mx > 0 && (w > mx || h > mx)) {
                *err = g_strdup_printf ("resolution: %d x %d on %s is more than the largest "
                                        "picture this GPU renders to (%d pixels a side); nothing "
                                        "was changed", w, h, nm, mx);
                break;
            }
            if ((gint64) w * h > 2 * (gint64) m->geom.width * m->geom.height)
                g_string_append_printf (notes, "%s%s: %d x %d is %.1f times the pixels of its "
                                        "%d x %d mode - scenes cost about that much more GPU "
                                        "work and video memory", notes->len ? "; " : "", nm, w, h,
                                        walpe_res_percent (w, h, m->geom.width, m->geom.height) /
                                            100.0, m->geom.width, m->geom.height);
        } else if (!m && !c.forget) {
            /* stored ahead for an output that is not there: these entries
             * are the ones that can pile up (any name will do), and each is
             * in every listing and every save */
            guint stored = D.cfg->monitors ? g_hash_table_size (D.cfg->monitors) : 0;
            for (guint q = 0; q < out->len; q++) {
                const ResChange *p = &g_array_index (out, ResChange, q);
                stored += !p->forget && !mon_by_name (p->name) &&
                          !walpe_config_monitor (D.cfg, p->name);
            }
            if (!walpe_config_monitor (D.cfg, nm) && stored >= WALPE_MONITORS_MAX) {
                *err = g_strdup_printf ("resolution: %d choices are stored already, no more for "
                                        "an output that is not connected (%s); `walpe-ctl "
                                        "resolution <output> --forget` drops one; nothing was "
                                        "changed", WALPE_MONITORS_MAX, nm);
                break;
            }
            g_string_append_printf (notes, "%s%s is not connected now: stored for when it is",
                                    notes->len ? "; " : "", nm);
        }
        ResChange one = c;
        one.name = g_strdup (nm);
        g_array_append_val (out, one);
    }
    g_ptr_array_unref (names);
    return *err == NULL;
}

/* main: the checked changes go into the config; sizes and limits reach
 * the monitors. Saves. */
static void res_changes_apply (GArray *ch)
{
    for (guint i = 0; ch && i < ch->len; i++) {
        const ResChange *c = &g_array_index (ch, ResChange, i);
        if (c->forget) {
            walpe_config_set_monitor (D.cfg, c->name, NULL);
            continue;
        }
        WalpeMonitorConf mc;
        walpe_config_monitor_get (D.cfg, c->name, &mc);
        if (c->has_res)
            mc.res = c->res;
        if (c->has_tex)
            mc.tex_limit = c->tex;
        /* a choice was made (or confirmed): the output has been asked about */
        mc.asked = c->asked >= 0 ? c->asked != 0 : TRUE;
        walpe_config_set_monitor (D.cfg, c->name, &mc);
    }
    gboolean sizes = res_refresh ();
    res_texlimits_refresh ();
    walpe_config_save (D.cfg);
    res_commit (sizes);
}

/* the members of the listing ({"cmd":"resolution"}) */
static void res_describe (JsonBuilder *b)
{
    const int mx = g_atomic_int_get (&gl_max_side);
    guint np = 0;
    const WalpeResPreset *pr = walpe_res_presets (&np);
    jb_bool (b, "ok", TRUE);
    jb_bool (b, "prompt", D.cfg->resolution_prompt);
    jb_int (b, "unasked", res_unasked ());
    json_builder_set_member_name (b, "limits");
    json_builder_begin_object (b);
    jb_int (b, "min_width", WALPE_RES_MIN_W);
    jb_int (b, "min_height", WALPE_RES_MIN_H);
    jb_int (b, "max_side", mx > 0 ? MIN (mx, WALPE_RES_MAX_SIDE) : WALPE_RES_MAX_SIDE);
    jb_int (b, "max_pixels", WALPE_RES_MAX_PIXELS);
    json_builder_end_object (b);
    json_builder_set_member_name (b, "presets");
    json_builder_begin_array (b);
    for (guint i = 0; i < np; i++) {
        json_builder_begin_object (b);
        jb_str (b, "name", pr[i].name);
        jb_int (b, "lines", pr[i].lines);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    jb_str (b, "scale_filter", walpe_scale_filter_name (g_atomic_int_get (&D.cfg->scale_filter)));
    {
        char *vr = walpe_vidres_format (&D.cfg->video_res);
        WalpeVideoBox box;
        video_box_for (NULL, NULL, TRUE, &box);
        jb_str (b, "video_resolution", vr);
        jb_size (b, "video_box", box.w, box.h);
        g_free (vr);
    }
    if (res_env)
        jb_bool (b, "env", TRUE);   /* WALPE_RESOLUTION decides the sizes while it is set */
    json_builder_set_member_name (b, "monitors");
    json_builder_begin_array (b);
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        const int mw = m->geom.width, mh = m->geom.height;
        WalpeMonitorConf mc;
        res_conf_for (m->name, &mc);
        char *rs = walpe_res_format (&mc.res), *tl = walpe_texlimit_format (mc.tex_limit);
        int cap = 0;
        walpe_texlimit_effective (m->tex_limit, TRUE, NULL, &cap);
        json_builder_begin_object (b);
        jb_int (b, "index", m->index);
        jb_str (b, "name", m->name);    /* (what else the output list knows of the
                                         * output - a description, make, model -
                                         * goes next to it, here and in status) */
        jb_bool (b, "connected", TRUE);
        jb_size (b, "mode", mw, mh);
        json_builder_set_member_name (b, "refresh");
        json_builder_add_double_value (b, m->hz);
        jb_str (b, "resolution", rs);
        jb_size (b, "render_size", m->res_w, m->res_h);
        jb_int (b, "render_percent", walpe_res_percent (m->res_w, m->res_h, mw, mh));
        jb_bool (b, "asked", mon_res_asked (m, &mc));
        jb_str (b, "texture_limit", tl);
        jb_int (b, "texture_limit_px", cap);
        if (m->res_note)
            jb_str (b, "note", m->res_note);
        /* what to offer for this output: native, then every preset that
         * is not larger than its mode */
        json_builder_set_member_name (b, "choices");
        json_builder_begin_array (b);
        json_builder_begin_object (b);
        jb_str (b, "value", "native");
        jb_size (b, "size", mw, mh);
        jb_int (b, "percent", 100);
        json_builder_end_object (b);
        for (guint i = 0; i < np; i++) {
            if (pr[i].lines > MIN (mw, mh))
                continue;
            WalpeRes r = { WALPE_RES_LINES, pr[i].lines, 0, 0 };
            int w = 0, h = 0;
            walpe_res_size (&r, mw, mh, &w, &h);
            json_builder_begin_object (b);
            jb_str (b, "value", pr[i].name);
            jb_size (b, "size", w, h);
            jb_int (b, "percent", walpe_res_percent (w, h, mw, mh));
            json_builder_end_object (b);
        }
        json_builder_end_array (b);
        json_builder_end_object (b);
        g_free (rs);
        g_free (tl);
    }
    /* stored choices of outputs that are not connected now */
    GList *ks = D.cfg->monitors ? g_list_sort (g_hash_table_get_keys (D.cfg->monitors),
                                               (GCompareFunc) strcmp) : NULL;
    for (GList *k = ks; k; k = k->next) {
        if (mon_by_name (k->data))
            continue;
        const WalpeMonitorConf *mc = g_hash_table_lookup (D.cfg->monitors, k->data);
        char *rs = walpe_res_format (&mc->res), *tl = walpe_texlimit_format (mc->tex_limit);
        json_builder_begin_object (b);
        jb_str (b, "name", k->data);
        jb_bool (b, "connected", FALSE);
        jb_str (b, "resolution", rs);
        jb_str (b, "texture_limit", tl);
        jb_bool (b, "asked", mc->asked);
        json_builder_end_object (b);
        g_free (rs);
        g_free (tl);
    }
    g_list_free (ks);
    json_builder_end_array (b);
}

static void ipc_resolution (JsonBuilder *b, JsonObject *o)
{
    GArray *ch = g_array_new (FALSE, TRUE, sizeof (ResChange));
    g_array_set_clear_func (ch, res_change_clear);
    GString *notes = g_string_new (NULL);
    char *err = NULL;
    gboolean prompt_given = FALSE, prompt = TRUE;
    JsonNode *sn = json_object_get_member (o, "set");
    if (sn && !JSON_NODE_HOLDS_ARRAY (sn)) {
        err = g_strdup ("resolution: \"set\" is a list of {\"monitor\", \"value\", ...} objects");
    } else if (sn) {
        JsonArray *arr = json_node_get_array (sn);
        const guint n = json_array_get_length (arr);
        if (n > 256)
            err = g_strdup ("resolution: too many elements in \"set\"");
        for (guint i = 0; !err && i < n; i++) {
            JsonNode *en = json_array_get_element (arr, i);
            if (!JSON_NODE_HOLDS_OBJECT (en))
                err = g_strdup ("resolution: every element of \"set\" is an object");
            else
                res_parse_element (json_node_get_object (en), ch, notes, &err);
        }
    }
    if (!err && (json_object_has_member (o, "monitor") || json_object_has_member (o, "value") ||
                 json_object_has_member (o, "texture_limit") ||
                 json_object_has_member (o, "asked") || json_object_has_member (o, "forget")))
        res_parse_element (o, ch, notes, &err);
    if (!err && json_object_has_member (o, "prompt")) {
        JsonNode *pn = json_object_get_member (o, "prompt");
        if (JSON_NODE_HOLDS_VALUE (pn) && json_node_get_value_type (pn) == G_TYPE_BOOLEAN) {
            prompt_given = TRUE;
            prompt = json_node_get_boolean (pn);
        } else {
            err = g_strdup ("resolution: \"prompt\" is true or false");
        }
    }
    if (err) {
        jb_fail (b, err);
    } else {
        if (prompt_given && prompt != D.cfg->resolution_prompt) {
            D.cfg->resolution_prompt = prompt;
            if (!ch->len)
                walpe_config_save (D.cfg);
        }
        if (ch->len)
            res_changes_apply (ch);     /* saves */
        res_describe (b);
        if (notes->len)
            jb_str (b, "note", notes->str);
    }
    g_free (err);
    g_string_free (notes, TRUE);
    g_array_unref (ch);
}

/* the "resolution" / "texture_limit" members of a "set" request ("VALUE"
 * for every connected output or "NAME:VALUE,NAME:VALUE"; texture_limit also
 * a number or a switch) into changes, and the other render resolution keys
 * checked: FALSE + *err when one is not right (nothing was changed). */
static gboolean res_set_parse (JsonObject *o, GArray *ch, GString *notes, char **err)
{
    static const char *const keys[2] = { "resolution", "texture_limit" };
    for (int k = 0; k < 2 && !*err; k++) {
        JsonNode *n = json_object_get_member (o, keys[k]);
        if (!n)
            continue;
        if (!JSON_NODE_HOLDS_VALUE (n) || json_node_get_value_type (n) != G_TYPE_STRING) {
            /* (texture_limit as a number or a switch: every output) */
            JsonObject *e = json_object_new ();
            json_object_set_member (e, k ? "texture_limit" : "value", json_node_copy (n));
            res_parse_element (e, ch, notes, err);
            json_object_unref (e);
            continue;
        }
        GPtrArray *as = walpe_res_assign_parse (json_node_get_string (n), err);
        if (!as && *err) {
            char *full = g_strdup_printf ("%s: %s", keys[k], *err);
            g_free (*err);
            *err = full;
        }
        for (guint i = 0; as && !*err && i < as->len; i++) {
            const WalpeResAssign *a = g_ptr_array_index (as, i);
            JsonObject *e = json_object_new ();
            if (a->monitor)
                json_object_set_string_member (e, "monitor", a->monitor);
            json_object_set_string_member (e, k ? "texture_limit" : "value", a->value);
            res_parse_element (e, ch, notes, err);
            json_object_unref (e);
        }
        if (as)
            g_ptr_array_unref (as);
    }
    if (!*err && json_object_has_member (o, "scale_filter") &&
        walpe_scale_filter_parse (wpj_str (o, "scale_filter", NULL)) < 0)
        *err = g_strdup ("scale_filter: bilinear or nearest");
    if (!*err && json_object_has_member (o, "video_resolution")) {
        WalpeVideoRes vr;
        walpe_vidres_parse (wpj_str (o, "video_resolution", ""), &vr, err);
    }
    return *err == NULL;
}

static void ipc_set_keys (JsonBuilder *b, JsonObject *o);

/* The sound of web pages follows the three settings of wallpaper sound
 * (web.h walpe_web_sound): as loud as "volume" says while "scene_sound" is
 * on and "mute" is off, silent otherwise (a wallpaper's own switch and
 * volume come on top, sound_props_apply). At start, before a wallpaper is
 * opened, and after every `set` request: costs nothing while the level is
 * what it was, and never loads the web module. */
static void web_sound_apply (void)
{
#ifdef HAVE_WEB
    walpe_web_sound (D.cfg->mute ? 0.0 : D.cfg->volume, D.cfg->scene_sound);
#endif
}

static void ipc_set (JsonBuilder *b, JsonObject *o)
{
    ipc_set_keys (b, o);
    web_sound_apply ();
    /* one load per scene for everything this request changed, also when it
     * ended early at a key that was not understood */
    reload_flush ();
}

/* "software_render" / "software_threads" of a set request: the values it
 * asks for (those in force where it names none), and whether the renderer
 * that runs has to change for them - a restart in place. */
static gboolean sw_set_wanted (JsonObject *o, gboolean *render, int *threads)
{
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
    *render = sr;
    *threads = st;
    return (!Sw.env_forced && sr != Sw.wanted) ||
           (Sw.active && st != D.cfg->software_threads && !g_getenv ("LP_NUM_THREADS"));
}

/* What only the running daemon can say about a set request, asked before
 * anything is changed like the checks of common/setkeys.h and
 * common/setvalues.h: the quality knobs against the values in force, a
 * window system this build does not have, a folder that is no Wallpaper
 * Engine assets folder (*we_assets: the folder as it will be stored, NULL
 * for "" = find it), a restart in place that cannot be made while an
 * import or a download runs. NULL = nothing against it, else the sentence
 * for the reply (g_free). ("gpu" is checked by gpu_ipc_set, which
 * ipc_set_keys calls in front of every other change.) */
static char *ipc_set_refused (JsonObject *o, gboolean busy, char **we_assets)
{
    {
        WalpeQuality q = D.cfg->quality;
        guint given = 0;
        char *qerr = NULL;
        if (!walpe_quality_from_json (o, &q, &given, NULL, &qerr)) {
            char *msg = g_strdup_printf ("%s" WALPE_SET_UNCHANGED, qerr ? qerr : "quality setting not allowed");
            g_free (qerr);
            return msg;
        }
    }
    if (ipc_has_str (o, "backend")) {
        int bk = walpe_backend_parse (wpj_str (o, "backend", ""), -1);
        if (bk >= 0 && bk != WALPE_BACKEND_AUTO && !walpe_platform_built (bk))
            return g_strdup_printf (
                "backend: this build has no %s backend (built: %s)" WALPE_SET_UNCHANGED, walpe_backend_name (bk),
                walpe_backends_built (walpe_platform_built (WALPE_BACKEND_X11),
                                      walpe_platform_built (WALPE_BACKEND_WAYLAND)));
    }
    if (ipc_has_str (o, "we_assets")) {
        /* "" = auto-detect; a dir must be a WE assets dir or install root */
        const char *wa = wpj_str (o, "we_assets", "");
        *we_assets = *wa ? walpe_we_assets_check (wa) : NULL;
        if (*wa && !*we_assets)
            return g_strdup ("we_assets: not a Wallpaper Engine assets dir (expected shaders/, "
                             "effects/ or materials/ in it, or an install root with assets/)"
                             WALPE_SET_UNCHANGED);
    }
    if (busy) {
        gboolean sr;
        int st;
        WalpeGlApiPref ga;
        if ((json_object_has_member (o, "software_render") || json_object_has_member (o, "software_threads")) &&
            sw_set_wanted (o, &sr, &st))
            return g_strdup ("software_render: the engine has to restart for this and an import / "
                             "Workshop download is running; change it when that has finished"
                             WALPE_SET_UNCHANGED);
        if (ipc_has_str (o, "gl_api") && walpe_gl_api_parse (wpj_str (o, "gl_api", ""), &ga) &&
            !Ga.env_forced && ga != Ga.pref)
            return g_strdup ("gl_api: the engine has to restart for this and an import / "
                             "Workshop download is running; change it when that has finished"
                             WALPE_SET_UNCHANGED);
    }
    return NULL;
}

static void ipc_set_keys (JsonBuilder *b, JsonObject *o)
{
    gboolean changed = FALSE;
    gboolean backend_saved = FALSE;     /* "backend" changed: in force at the next start */
    const gboolean busy = (Imp.job && Imp.active) || (Ws.job && Ws.active);
    g_autofree char *we_assets = NULL;  /* "we_assets" as checked (ipc_set_refused) */
    {
        /* a member this build does not know (a typo, a newer client), a
         * value of the wrong type for its key or a value its key does not
         * allow: refused as a whole, before anything is changed
         * (common/setkeys.h, common/setvalues.h, ipc_set_refused). An
         * unknown member used to be dropped without a word while the rest
         * was applied; a value that was not allowed ended the request at
         * its key, with the keys in front of it applied and saved and
         * nothing in the reply to say so (`set fps=31 focus=sideways`:
         * refused, and the frame rate limit was 31). */
        char *bad = walpe_set_check (o);
        if (!bad)
            bad = walpe_set_values_check (o);
        if (!bad)
            bad = ipc_set_refused (o, busy, &we_assets);
        if (bad) {
            jb_fail (b, bad);
            g_free (bad);
            return;
        }
    }
    /* render resolution (resolution, texture_limit, scale_filter,
     * video_resolution): checked here, before anything is changed, applied
     * below once the quality knobs are in place */
    GArray *res_ch = g_array_new (FALSE, TRUE, sizeof (ResChange));
    g_array_set_clear_func (res_ch, res_change_clear);
    GString *res_notes = g_string_new (NULL);
    {
        char *rerr = NULL;
        if (!res_set_parse (o, res_ch, res_notes, &rerr)) {
            char *msg = g_strdup_printf ("%s" WALPE_SET_UNCHANGED, rerr ? rerr : "resolution setting not allowed");
            jb_fail (b, msg);
            g_free (msg);
            g_free (rerr);
            g_array_unref (res_ch);
            g_string_free (res_notes, TRUE);
            return;
        }
    }
    gboolean gpu_restart = FALSE;
    g_autofree char *gpu_note = NULL;   /* a sentence for the reply */
    if (json_object_has_member (o, "gpu") || json_object_has_member (o, "preset_follow_gpu")) {
        /* which GPU renders: saved, and the daemon restarts in place when
         * another device has to render (the drivers read their variables
         * when EGL starts). In front of every other change: gpu_ipc_set
         * checks its value against the list of devices and changes nothing
         * when it refuses - behind the other keys, a device that does not
         * exist left those applied. */
        char *gerr = NULL;
        if (!gpu_ipc_set (o, busy, &gpu_restart, &gpu_note, &gerr)) {
            jb_fail (b, gerr ? gerr : "gpu: not allowed");
            g_free (gerr);
            g_array_unref (res_ch);
            g_string_free (res_notes, TRUE);
            return;
        }
        changed = TRUE;
    }
    {
        /* quality knobs (quality_preset, fx_rate, fx_rate_mode, fx_res,
         * fps_idle, post_processing, render_scale, tex_res, texture_slack):
         * checked first, so a value that is not allowed changes nothing */
        guint given = 0, diff = 0;
        char *qerr = NULL;
        if (!walpe_quality_ipc_set (D.cfg, o, &given, &diff, &qerr)) {
            /* (a guard: ipc_set_refused has asked the same before anything changed) */
            jb_fail (b, qerr ? qerr : "quality setting not allowed");
            g_free (qerr);
            g_array_unref (res_ch);
            g_string_free (res_notes, TRUE);
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
    gboolean changed_res = FALSE;   /* a render resolution key was given and applied */
    /* (freed wherever this function ends: the keys below still refuse and return) */
    g_autofree char *res_note = res_notes->len ? g_strdup (res_notes->str) : NULL;
    {
        /* render resolution: the choices per output (they were checked
         * above; saved here), then the three global keys */
        gboolean sizes = FALSE, commit = FALSE;
        if (res_ch->len) {
            res_changes_apply (res_ch);     /* sizes and limits to the monitors; saves */
            changed_res = TRUE;
        }
        if (json_object_has_member (o, "scale_filter")) {
            /* a scene whose frame is scaled shows it with its next frame */
            int f = walpe_scale_filter_parse (wpj_str (o, "scale_filter", NULL));
            if (f >= 0) {
                g_atomic_int_set (&D.cfg->scale_filter, f);
                for (GList *l = D.mons; l; l = l->next)
                    request_redraw (l->data);
                changed_res = TRUE;
            }
        }
        if (json_object_has_member (o, "video_resolution")) {
            /* imports and conversions from now on; a video on screen whose
             * converted copy is for another box is given the one for this */
            WalpeVideoRes vr;
            if (walpe_vidres_parse (wpj_str (o, "video_resolution", ""), &vr, NULL)) {
                commit = !walpe_vidres_equal (&vr, &D.cfg->video_res);
                D.cfg->video_res = vr;
                changed_res = TRUE;
            }
        }
        if (json_object_has_member (o, "resolution_prompt")) {
            D.cfg->resolution_prompt = wpj_bool (o, "resolution_prompt", D.cfg->resolution_prompt);
            changed_res = TRUE;
        }
        if (commit)
            res_commit (sizes);
    }
    g_array_unref (res_ch);
    g_string_free (res_notes, TRUE);
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
        gboolean sr;
        int st;
        gboolean need = sw_set_wanted (o, &sr, &st);
        if (need && busy) {     /* (a guard: ipc_set_refused has said so before anything changed) */
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
        /* (a restart that a change of gl_api asked for stays due) */
        if (!differs && Sw.restart_id &&
            (Ga.env_forced || (WalpeGlApiPref) D.cfg->gl_api == Ga.pref)) {
            g_source_remove (Sw.restart_id);
            Sw.restart_id = 0;
            g_message ("software rendering setting is back to what runs: no restart");
        }
    }
    if (json_object_has_member (o, "gl_api")) {
        /* desktop OpenGL or OpenGL ES: saved, and the daemon restarts in
         * place when what runs was decided by another value (the API of
         * the contexts is fixed when the first one is made) */
        WalpeGlApiPref ga;
        if (!walpe_gl_api_parse (wpj_str (o, "gl_api", ""), &ga)) {
            jb_fail (b, "gl_api: auto, gl or gles");
            return;
        }
        gboolean need = !Ga.env_forced && ga != Ga.pref;
        if (need && busy) {
            jb_fail (b, "gl_api: the engine has to restart for this and an import / "
                        "Workshop download is running; change it when that has finished");
            return;
        }
        if ((int) ga != D.cfg->gl_api) {
            D.cfg->gl_api = (int) ga;
            changed = TRUE;
        } else {
            live = TRUE;        /* nothing to save: not "nothing to set" */
        }
        restart |= need;
    }
    if (ipc_has_num (o, "fps")) {
        D.cfg->fps = (int) CLAMP (wpj_num (o, "fps", D.cfg->fps), 5.0, 200.0);
        fps_refresh ();
        if (ptr_mode == PTR_TRACK)   /* the pointer tracking rate follows it */
            pointer_set_mode (PTR_TRACK);
        changed = TRUE;
    }
    if (ipc_has_num (o, "volume")) {
        /* the volume of wallpaper sound (scene sound layers; nothing else
         * makes a sound): heard at once */
        D.cfg->volume = CLAMP (wpj_num (o, "volume", D.cfg->volume), 0.0, 1.0);
        walpe_sound_set_master (D.cfg->volume);
        changed = TRUE;
    }
    if (json_object_has_member (o, "scene_sound")) {
        /* scenes play their sound layers; takes effect at once, nothing is
         * loaded again. A wallpaper's own switch (setprop walpe_sound) wins. */
        D.cfg->scene_sound = wpj_bool (o, "scene_sound", D.cfg->scene_sound);
        walpe_sound_set_enabled (D.cfg->scene_sound);
        changed = TRUE;
    }
    if (json_object_has_member (o, "mute")) {
        D.cfg->mute = wpj_bool (o, "mute", D.cfg->mute);
        walpe_sound_set_muted (D.cfg->mute);
        changed = TRUE;
    }
    if (json_object_has_member (o, "pause_fullscreen")) {
        D.cfg->pause_fullscreen = wpj_bool (o, "pause_fullscreen", D.cfg->pause_fullscreen);
        changed = TRUE;
        sig_window_rules ();
        walpe_signals_recheck (WALPE_SIG_FULLSCREEN, TRUE);
        apply_pause_state ();
    }
    if (json_object_has_member (o, "pause_fullscreen_all")) {
        /* the fullscreen rule for every monitor (as before 0.13) or only
         * for the one the window is on; at once */
        D.cfg->pause_fullscreen_all = wpj_bool (o, "pause_fullscreen_all",
                                                D.cfg->pause_fullscreen_all);
        changed = TRUE;
        apply_pause_state ();
    }
    if (json_object_has_member (o, "pause_dpms")) {
        D.cfg->pause_dpms = wpj_bool (o, "pause_dpms", D.cfg->pause_dpms);
        changed = TRUE;
        walpe_signals_recheck (WALPE_SIG_ASLEEP, TRUE);
        apply_pause_state ();
    }
    if (json_object_has_member (o, "pause_covered")) {
        D.cfg->pause_covered = wpj_bool (o, "pause_covered", D.cfg->pause_covered);
        changed = TRUE;
        sig_window_rules ();
        walpe_signals_recheck (WALPE_SIG_COVERED, TRUE);
        apply_pause_state ();
    }
    if (json_object_has_member (o, "cover_transparent")) {
        D.cfg->cover_transparent = wpj_bool (o, "cover_transparent", D.cfg->cover_transparent);
        changed = TRUE;
        walpe_signals_recheck (WALPE_SIG_COVERED, FALSE);
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
        sig_window_rules ();
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
    if (ipc_has_num (o, "pause_idle")) {
        /* seconds without input after which every wallpaper pauses; 0 = off */
        D.cfg->pause_idle = (int) CLAMP (wpj_num (o, "pause_idle", D.cfg->pause_idle), 0.0, 86400.0);
        walpe_sigprov_set_idle_timeout ((guint) D.cfg->pause_idle);
        walpe_signals_recheck (WALPE_SIG_IDLE, TRUE);
        changed = TRUE;
        apply_pause_state ();   /* switched off while idle: run again */
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
        } else {
            live = TRUE;        /* the value it has: nothing to save, but not "nothing to set" */
        }
    }
#endif
    if (json_object_has_member (o, "web_unload")) {
        /* (read at the next change of wallpapers: web_unload_due) */
        D.cfg->web_unload = wpj_bool (o, "web_unload", D.cfg->web_unload);
        changed = TRUE;
    }
    if (json_object_has_member (o, "particle_lowres")) {
        /* off | auto | all (walpe-ctl sends on / off as booleans) */
        JsonNode *pn = json_object_get_member (o, "particle_lowres");
        int mode = -1;
        if (JSON_NODE_HOLDS_VALUE (pn) && json_node_get_value_type (pn) == G_TYPE_BOOLEAN)
            mode = json_node_get_boolean (pn) ? 1 : 0;
        else
            mode = walpe_particle_lowres_from_name (wpj_str (o, "particle_lowres", NULL), -1);
        if (mode < 0) {
            /* (a guard, here and at the keys below that refuse a value:
             * walpe_set_values_check has refused it before anything changed) */
            if (changed)
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
    {
        /* renderer features (engine/features.h): `walpe-ctl set
         * <name>=on|off` for every feature that common/setkeys.h lists as
         * a setting. Running scenes take a switch up with their next frame
         * (asked for here: a still scene shows it now); a feature that a
         * scene decides when it loads gets what is shown loaded again. */
        gboolean feat_frame = FALSE, feat_reload = FALSE;
        const char *feat_env = NULL;
        for (int f = 0; walpe_feature_name (f); f++) {
            const char *fname = walpe_feature_name (f);
            if (!walpe_set_key_find (fname) || !json_object_has_member (o, fname))
                continue;
            gboolean was = walpe_feature ((WalpeFeature) f);
            gboolean on = wpj_bool (o, fname, was);
            walpe_feature_set (fname, on);
            walpe_config_set_feature (D.cfg, fname, on);   /* saved also when the environment decides now */
            changed = TRUE;
            if (walpe_feature_fixed (f))
                feat_env = fname;
            if (walpe_feature ((WalpeFeature) f) != was) {
                if (walpe_feature_reloads (f))
                    feat_reload = TRUE;
                else
                    feat_frame = TRUE;
                g_message ("feature %s: %s", fname, on ? "on" : "off");
            }
        }
        if (feat_reload)
            scenes_reload_mark (NULL);   /* (ipc_set flushes) */
        if (feat_frame)
            for (GList *l = D.mons; l; l = l->next)
                request_redraw (l->data);
        if (feat_env)
            g_message ("feature %s: saved; WALPE_FEATURES in the daemon's environment decides while "
                       "it is set", feat_env);
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
        /* a switch (on = with the API that is stored), or off | auto | the
         * name of one decoding API; videos that are playing follow */
        JsonNode *hn = json_object_get_member (o, "video_hwdec");
        const char *hs = wpj_str (o, "video_hwdec", NULL);
        gboolean ok = TRUE;
        if (hs)
            ok = walpe_config_set_hwdec (D.cfg, hs);
        else if (JSON_NODE_HOLDS_VALUE (hn))
            D.cfg->video_hwdec = wpj_bool (o, "video_hwdec", D.cfg->video_hwdec);
        else
            ok = FALSE;
        if (!ok) {
            if (changed)
                walpe_config_save (D.cfg);
            jb_fail (b, "video_hwdec: off, auto, or the name of one decoding API (vaapi, cuda, "
                        "vdpau, vulkan, drm, v4l2m2m, ...)");
            return;
        }
        walpe_video_set_hwdec_name (walpe_config_hwdec_word (D.cfg));
        changed = TRUE;
    }
    if (json_object_has_member (o, "video_zerocopy")) {
        /* read by the video module with the next picture it hands over */
        D.cfg->video_zerocopy = wpj_bool (o, "video_zerocopy", D.cfg->video_zerocopy);
        walpe_video_set_zerocopy (D.cfg->video_zerocopy);
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
    if (json_object_has_member (o, "video_scene_convert")) {
        /* scenes that are running follow it where their clip starts again */
        D.cfg->video_scene_convert = wpj_bool (o, "video_scene_convert", D.cfg->video_scene_convert);
        sv_setting (D.cfg->video_scene_convert);
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
    if (json_object_has_member (o, "fluid_sim") || json_object_has_member (o, "fluid_quality")) {
        /* simulation effects (engine/fx_sim.h): the switch and the quality
         * level. Both reach the running wallpapers with their next frame;
         * nothing is loaded again. */
        WalpeFluidQuality fq = (WalpeFluidQuality) D.cfg->fluid_quality;
        if (json_object_has_member (o, "fluid_quality") &&
            !walpe_fluid_quality_parse (wpj_str (o, "fluid_quality", ""), &fq)) {
            if (changed)
                walpe_config_save (D.cfg);
            jb_fail (b, "fluid_quality: full, reduced or low");
            return;
        }
        D.cfg->fluid_quality = (int) fq;
        fx_sim_set_quality (walpe_fluid_quality_name (fq));
        if (json_object_has_member (o, "fluid_sim")) {
            D.cfg->fluid_sim = wpj_bool (o, "fluid_sim", D.cfg->fluid_sim);
            fx_sim_set_enabled (D.cfg->fluid_sim);
        }
        changed = TRUE;
        for (GList *l = D.mons; l; l = l->next)
            request_redraw (l->data);
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
        walpe_sigprov_set_see_through (tw);   /* Wayland: the same list, by app id */
        walpe_signals_recheck (WALPE_SIG_COVERED, FALSE); /* coverage with the new list */
        if (D.vis)
            walpe_vistrack_set_see_through (D.vis, tw);
    }
    if (ipc_has_str (o, "steam_user")) {
        const char *su = wpj_str (o, "steam_user", "");
        if (*su && !walpe_workshop_valid_user (su)) {
            if (changed)
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
        /* (checked by ipc_set_refused before anything changed) */
        char *chk = g_steal_pointer (&we_assets);
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
    /* the window system (common/backend.h): saved; the running daemon keeps
     * the one it started on, the next start reads the setting */
    if (ipc_has_str (o, "backend")) {
        int bk = walpe_backend_parse (wpj_str (o, "backend", ""), -1);
        if (bk < 0) {
            if (changed)
                walpe_config_save (D.cfg);
            jb_fail (b, "backend: auto, x11 or wayland");
            return;
        }
        if (bk != WALPE_BACKEND_AUTO && !walpe_platform_built (bk)) {
            if (changed)
                walpe_config_save (D.cfg);
            char *msg = g_strdup_printf (
                "backend: this build has no %s backend (built: %s)", walpe_backend_name (bk),
                walpe_backends_built (walpe_platform_built (WALPE_BACKEND_X11),
                                      walpe_platform_built (WALPE_BACKEND_WAYLAND)));
            jb_fail (b, msg);
            g_free (msg);
            return;
        }
        backend_saved = bk != D.cfg->backend;
        D.cfg->backend = bk;
        changed = TRUE;
    }
    /* Wayland: may the wallpaper surface take pointer input while a
     * wallpaper uses the pointer ("auto"), or never ("off")? */
    if (ipc_has_str (o, "wayland_pointer")) {
        const char *wp = wpj_str (o, "wayland_pointer", "");
        if (g_ascii_strcasecmp (wp, "auto") && g_ascii_strcasecmp (wp, "off")) {
            if (changed)
                walpe_config_save (D.cfg);
            jb_fail (b, "wayland_pointer: auto or off");
            return;
        }
        D.cfg->wayland_pointer = !g_ascii_strcasecmp (wp, "off");
        changed = TRUE;
        pointer_update ();
    }
    gboolean mpv_word = FALSE;      /* "mpv" was stored: the reply says what that means */
    if (ipc_has_str (o, "video_backend")) {
        const char *bk = wpj_str (o, "video_backend", "ffmpeg");
        if (!g_strcmp0 (bk, "mpv") || !g_strcmp0 (bk, "ffmpeg")) {
            /* stored for old configs / GUIs only: FFmpeg is the one video
             * backend (mpv was removed), so nothing reloads for it */
            g_free (D.cfg->video_backend);
            D.cfg->video_backend = g_strdup (bk);
            changed = TRUE;
            mpv_word = !strcmp (bk, "mpv");
        }
    }
    changed = changed || changed_res;
    if (changed) {
        walpe_config_save (D.cfg);
        jb_bool (b, "ok", TRUE);
        if (backend_saved) {
            /* nothing changes under the running daemon */
            jb_bool (b, "restart_needed", TRUE);
            jb_str (b, "backend_now", walpe_backend_name (D.backend));
        }
        if (res_note)
            jb_str (b, "note", res_note);
        if (gpu_note)
            jb_str (b, "gpu_note", gpu_note);
        if (mpv_word)
            jb_str (b, "video_backend_note", "stored; mpv means the same as " WALPE_VIDEO_BACKEND
                                             ": FFmpeg is the one video backend, and status shows "
                                             "it as \"backend\"");
        if (restart) {
            jb_bool (b, "restarting", TRUE);   /* after this reply is out */
            sw_schedule_restart ();
        } else if (gpu_restart) {
            jb_bool (b, "restarting", TRUE);
            gpu_schedule_restart ();
        } else if (Sw.env_forced && json_object_has_member (o, "software_render")) {
            jb_str (b, "note", "saved; WALPE_SOFTWARE in the daemon's environment decides "
                               "while it is set");
        } else if (Ga.env_forced && json_object_has_member (o, "gl_api")) {
            jb_str (b, "note", "saved; WALPE_GL_API in the daemon's environment decides "
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

/* {"cmd":"mem"} (`walpe-ctl mem`, engine/memreport.h): where the daemon's
 * memory is. Computed now, for this request: the process as the kernel
 * and the allocator see it, what happened how often since the start, and
 * per monitor the GL objects of its context and what its wallpaper holds.
 * Each monitor's part is written by its render thread between two frames
 * (also a paused one's, which draws nothing for it); a thread that does
 * not answer within three seconds - it is loading a wallpaper, or a
 * software renderer is busy with one frame for that long - is reported as
 * busy ({"wait": seconds} waits longer, up to a minute; nothing else is
 * answered meanwhile). {"deep": true} also asks the driver for the size
 * of every program binary. {"events": "on"} starts the counting of GL
 * storage events (memreport.h: nothing counts them before), "off" ends it;
 * the report says what was counted since. */
static gint64 daemon_start_us;

static void ipc_mem (JsonBuilder *b, JsonObject *o)
{
    static guint serial;
    const gboolean deep = wpj_bool (o, "deep", FALSE);
    const double wait_s = CLAMP (wpj_num (o, "wait", 3.0), 0.1, 60.0);
    const char *events = wpj_str (o, "events", NULL);
    const gint64 t0 = g_get_monotonic_time ();
    if (events && (!strcmp (events, "on") || !strcmp (events, "off")))
        walpe_mem_gl_events (events[1] == 'n');
    if (!++serial)
        serial = 1;
    walpe_memc_inc (WALPE_MEMC_MEM);
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        g_mutex_lock (&m->lock);
        m->mem_req = serial;
        m->mem_deep = deep;
        g_cond_signal (&m->cond);
        g_mutex_unlock (&m->lock);
    }
    jb_bool (b, "ok", TRUE);
    json_builder_set_member_name (b, "mem");
    json_builder_begin_object (b);
    jb_int (b, "report_format", 1);
    jb_str (b, "version", WALPE_VERSION);
    {
        GDateTime *now = g_date_time_new_now_utc ();
        char *ts = g_date_time_format_iso8601 (now);
        jb_str (b, "time", ts);
        g_free (ts);
        g_date_time_unref (now);
    }
    json_builder_set_member_name (b, "uptime_s");
    json_builder_add_double_value (b, (double) (t0 - daemon_start_us) / 1e6);
    walpe_mem_process_json (b);   /* "process", "fds", "malloc", "mappings" */
    json_builder_set_member_name (b, "counters");
    json_builder_begin_object (b);
    walpe_mem_counters_json (b);
    jb_int (b, "config_saves", walpe_config_save_count ());
    jb_int (b, "subscribers", walpe_ipc_subscribers ());
    {
        /* GLib numbers the sources of a main context in the order they
         * are attached: the number a new one gets is how many the main
         * loop has been given so far (timers, idle calls, watches). The
         * probe is destroyed before the loop sees it. */
        GSource *probe = g_idle_source_new ();
        guint id = g_source_attach (probe, NULL);
        g_source_destroy (probe);
        g_source_unref (probe);
        jb_int (b, "main_loop_sources_created", id);
    }
    {
        /* effect pass programs since the start (fx_specialize.h): built,
         * found equal to the plain program / not equal */
        int fc = 0, fv = 0, fr = 0;
        fx_program_stats (&fc, NULL, NULL, NULL, NULL, NULL, FALSE);
        fx_verify_stats (&fv, &fr);
        jb_int (b, "fx_programs_compiled", fc);
        jb_int (b, "fx_verified", fv);
        jb_int (b, "fx_rejected", fr);
    }
    json_builder_end_object (b);
    walpe_mem_gl_events_json (b);                  /* "gl_events" */
    walpe_modules_mem_json (b, daemon_start_us);   /* "modules", "video_module" */
    {
        guint snaps = 0;
#ifdef HAVE_WEB
        walpe_web_stats (&snaps);
#endif
        jb_int (b, "web_snapshots", snaps);
        char *snd = sound_status_line ();
        jb_str (b, "sound", snd);
        g_free (snd);
    }
    g_mutex_lock (&Sw.lock);
    jb_str (b, "gl_vendor", Sw.gl_vendor);
    jb_str (b, "gl_renderer", Sw.gl_renderer);
    jb_str (b, "gl_version", Sw.gl_version);
    g_mutex_unlock (&Sw.lock);
    /* one EGL share group: textures, buffers, programs, shaders,
     * renderbuffers and samplers are the same objects in every monitor's
     * census (framebuffers, vertex arrays and queries are each context's
     * own) */
    jb_bool (b, "gl_shared", walpe_gl_get_shared () && !g_atomic_int_get (&D.share_failed));
    json_builder_set_member_name (b, "monitors");
    json_builder_begin_array (b);
    const gint64 deadline = g_get_monotonic_time () + (gint64) (wait_s * G_USEC_PER_SEC);
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        char *js = NULL;
        g_mutex_lock (&m->lock);
        while (m->mem_done != serial && !m->quitting)
            if (!g_cond_wait_until (&m->mem_cond, &m->lock, deadline))
                break;
        if (m->mem_done == serial)
            js = g_steal_pointer (&m->mem_json);
        g_mutex_unlock (&m->lock);
        JsonNode *node = js ? wpj_parse_data (js, -1) : NULL;
        g_free (js);
        if (node) {
            json_builder_add_value (b, node);   /* (takes the node) */
        } else {
            json_builder_begin_object (b);
            jb_int (b, "index", m->index);
            jb_str (b, "name", m->name);
            jb_bool (b, "busy", TRUE);
            jb_str (b, "note", "its render thread did not answer in time (it is loading a "
                               "wallpaper, or busy with a slow frame): ask again, or with "
                               "--wait SECONDS");
            json_builder_end_object (b);
        }
    }
    json_builder_end_array (b);
    jb_int (b, "report_us", g_get_monotonic_time () - t0);
    json_builder_end_object (b);
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

#include "diag.h"         /* the summary for a bug report: diag_say, diag_ipc */

/* a request that only reads: it changes nothing a subscriber is told
 * about. "resolution" lists when it names nothing to change. */
static gboolean ipc_request_reads (const char *cmd, JsonObject *o)
{
    static const char *const reads[] = { "status", "props", "subscribe", "steam_libraries", "mem",
                                         "quality", "layers", "profile_result", "diag", NULL };
    static const char *const res_changes[] = { "set", "monitor", "value", "texture_limit", "asked",
                                               "forget", "prompt", NULL };
    for (int i = 0; reads[i]; i++)
        if (!g_strcmp0 (cmd, reads[i]))
            return TRUE;
    if (g_strcmp0 (cmd, "resolution"))
        return FALSE;
    for (int i = 0; res_changes[i]; i++)
        if (json_object_has_member (o, res_changes[i]))
            return FALSE;
    return TRUE;
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
    /* A request line is UTF-8 (ipc.c) and its strings can still be none:
     * json-glib makes three bytes that are no UTF-8 of "\ud800", the escape
     * of half a surrogate pair. What a request sets is kept and saved - a
     * wallpaper's option, a path, a list of window classes - and one such
     * string made config.json a file the next start could not read: it was
     * set aside as damaged and every setting was back at its default.
     * Nothing a request may say needs such a string. */
    if (!wpj_text_is_utf8 (root)) {
        json_node_unref (root);
        return g_strdup ("{\"ok\":false,\"error\":\"the request holds text that is not valid UTF-8 "
                         "(the escape of half a surrogate pair, \\\\ud800 to \\\\udfff); nothing was "
                         "changed\"}");
    }
    JsonObject *o = json_node_get_object (root);
    const char *cmd = wpj_str (o, "cmd", "");
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    gboolean quit = FALSE;
    guint saves_before = walpe_config_save_count ();
    walpe_memc_inc (WALPE_MEMC_IPC);

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
            /* the box of a video's converted copy: by the monitors that
             * will show it once this request is carried out (open_commit:
             * that monitor besides those that have it already; all of them
             * without --monitor, and when it becomes the default) */
            WalpeVideoBox vbox = { 0, 0 };
            if (t == WALPE_TYPE_VIDEO)
                video_box_for (abs, tm, !tm || !D.cfg->current || !*D.cfg->current, &vbox);
            if (t == WALPE_TYPE_NONE || t == WALPE_TYPE_UNSUPPORTED) {
                char *msg = g_strdup_printf ("unsupported content at %s", abs);
                jb_fail (b, msg);
                g_free (msg);
                now = FALSE;
            } else if (t == WALPE_TYPE_VIDEO && !video_playable (abs, &vbox, &vsrc, &vwhy, &vsoft)) {
                /* a video the player cannot read (WebM, HEVC / AV1, MKV,
                 * fragmented MP4 ...) or would show sideways / stretched:
                 * converted once, shown when ready. (The load runs later
                 * on the render thread: version 0.12 answered ok, kept the old
                 * wallpaper and saved a path that gave a black desktop at
                 * the next start.) */
                GError *ce = vsoft && conv_failed && g_hash_table_contains (conv_failed, vsrc)
                    ? g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "its conversion failed before")
                    : convert_start (vsrc, abs, tm ? tm->name : NULL, TRUE, &vbox);
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
    } else if (!g_strcmp0 (cmd, "soundcheck")) {
        ipc_soundcheck (b);
    } else if (!g_strcmp0 (cmd, "mem")) {
        ipc_mem (b, o);
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
            if (!(prop_is_sound (key) && sound_props_apply (tpc)))
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
            web_unload_hidden ();
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
            g_clear_pointer (&Conv.src, g_free);
            sv_start_next ();   /* (a copy for a scene that gave way to it) */
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
            gboolean sound_only = TRUE;   /* nothing but WalPE's own sound options */
            for (GList *k = ks; k; k = k->next) {
                char *val = json_value_to_prop (json_object_get_member (po, k->data));
                if (val && *(const char *) k->data) {
                    walpe_config_set_prop (D.cfg, tpc, k->data, val);
                    g_ptr_array_add (set, k->data);
                    sound_only &= prop_is_sound (k->data);
                    n++;
                }
                g_free (val);
            }
            if (n) {
                walpe_config_save (D.cfg);
                if (!(sound_only && sound_props_apply (tpc)))
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
    } else if (!g_strcmp0 (cmd, "resolution")) {
        ipc_resolution (b, o);
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
    } else if (!g_strcmp0 (cmd, "diag")) {
        diag_ipc (b);                                           /* walpe-ctl diag */
    } else if (!g_strcmp0 (cmd, "quit")) {
        jb_bool (b, "ok", TRUE);
        quit = TRUE;
        diag_quit_ipc ();   /* the log names the program that asked */
        /* The subscribers (the window, the tray) are told that the engine
         * leaves because it was asked to. To them the end of the connection
         * was all there was, the same as after a crash - so a window that
         * had not started this engine itself started a new one a second
         * after `walpe-ctl quit`. One line, the last on the connection;
         * an engine that dies sends none and is restarted as before. */
        walpe_ipc_broadcast ("{\"event\":\"quit\"}");
    } else {
        jb_fail (b, "unknown cmd");
    }

    /* What this request changed is in effect, but config.json could not be
     * written (a full disk, a home that cannot be written to): the answer
     * stays what it is and says so too. It said "ok" and nothing more; the
     * log alone had the warning, and the change was gone at the next start. */
    if (walpe_config_save_count () != saves_before && walpe_config_save_error ())
        jb_str (b, "save_error", walpe_config_save_error ());

    json_builder_end_object (b);
    JsonNode *rn = json_builder_get_root (b);
    JsonGenerator *g = json_generator_new ();
    json_generator_set_root (g, rn);
    char *out = json_generator_to_data (g, NULL);
    g_object_unref (g);
    json_node_unref (rn);
    g_object_unref (b);
    /* cmd lives in root: decide before it goes. A request that only reads
     * is no reason to send the state to the subscribers: the line carries
     * the frame counters, so it always differs from the last one and is
     * always sent - the window's performance panel asks for "quality"
     * every 2 s while it shows, and each time the whole status went to the
     * window and to the tray, which read their files and went through
     * their menus for it. */
    gboolean touch = !ipc_request_reads (cmd, o);
    json_node_unref (root);
    if (touch)
        diag_sound_line ();     /* the log: sound was switched off, muted, set to 0 - or back */
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
static int egl_config_cost (EGLConfig c, unsigned long want_visual)
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
    /* (a backend that names no visual - 0 - has no such preference; X11
     * always names the root window's) */
    if ((want_visual || D.backend == WALPE_BACKEND_X11) && (unsigned long) vid != want_visual)
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
    unsigned long vis = walpe_platform_egl_visual ();
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
    if (!walpe_platform_egl_config_usable (D.egl_dpy, all[best])) {
        g_message ("EGL: the cheapest window config cannot make a surface here; "
                   "using the first match");
        return first;
    }
    char was[160];
    egl_config_describe (first, was, sizeof was);
    g_message ("EGL: %d window configs; the first match was %s", (int) n, was);
    return all[best];
}

/* The window config for OpenGL ES contexts. The config chosen in
 * init_egl () is asked for without EGL_RENDERABLE_TYPE (as it always was:
 * the desktop OpenGL path must get the config it always got). When that
 * one is an OpenGL ES 3 config it serves; else the choice is made again
 * among the configs that are, by the same rule. FALSE: there is none. */
static gboolean egl_config_for_es (void)
{
    EGLint rt = 0;
    if (eglGetConfigAttrib (D.egl_dpy, D.egl_conf, EGL_RENDERABLE_TYPE, &rt) &&
        (rt & EGL_OPENGL_ES3_BIT))
        return TRUE;
    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, EGL_DONT_CARE,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    EGLConfig all[512];
    EGLint n = 0;
    if (!eglChooseConfig (D.egl_dpy, attrs, all, G_N_ELEMENTS (all), &n) || n < 1) {
        (void) eglGetError ();
        return FALSE;
    }
    D.egl_conf = egl_pick_config (all, n);
    char desc[160];
    egl_config_describe (D.egl_conf, desc, sizeof desc);
    g_message ("EGL config for OpenGL ES: %s", desc);
    {
        /* present_egl_probe () has read the swap intervals of the config
         * chosen first: the pacing has to know those of the one in use */
        EGLint lo = 0, hi = 1;
        eglGetConfigAttrib (D.egl_dpy, D.egl_conf, EGL_MIN_SWAP_INTERVAL, &lo);
        eglGetConfigAttrib (D.egl_dpy, D.egl_conf, EGL_MAX_SWAP_INTERVAL, &hi);
        if (D.fake_vsync) {
            lo = 0;
            hi = MAX (hi, 1);
        }
        if (lo != P.min_iv || hi != P.max_iv)
            g_message ("present: swap interval %d..%d with this config", (int) lo, (int) hi);
        P.min_iv = lo;
        P.max_iv = hi;
    }
    return TRUE;
}

static gboolean init_egl (void)
{
    /* the display of the window-system connection (X11: on a connection
     * of EGL's own by default, WALPE_ONE_XCONN=1 = the main thread's) */
    D.egl_dpy = walpe_platform_egl_display ();
    if (D.egl_dpy == EGL_NO_DISPLAY) {
        g_warning ("eglGetDisplay failed");
        return FALSE;
    }
    if (!eglInitialize (D.egl_dpy, NULL, NULL)) {
        g_warning ("eglInitialize failed");
        return FALSE;
    }
    /* GPU choice: hand-over point for the window-system layer (call 2 of
     * 2, gpu_glue.h; the display is initialised): which device this
     * display renders on */
    gpu_note_egl (D.egl_dpy);
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
    return gl_api_decide ();
}

#include "gpu_glue.h"     /* gpu_setup, gpu_note_egl, --gpu-probe, status, set */

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
        /* which driver renders, for common/gpustat.h (a profile asks it
         * before any render thread has attached to the profiler) */
        walpe_gpustat_set_gl_vendor (v);
        gboolean cpu = walpe_sw_renderer_is_software (Sw.gl_renderer);
        g_message ("renderer: %s | %s | %s - rendering on the %s%s", Sw.gl_vendor, Sw.gl_renderer,
                   Sw.gl_version, cpu ? "CPU" : "GPU",
                   Sw.active ? " (software rendering is switched on)"
                   : cpu ? " (no GPU driver answered; the software rendering switch is off)" : "");
        if (Sw.active && !cpu)
            g_warning ("software rendering is switched on but the GL context is not Mesa's "
                       "software renderer");
        if (cpu && !Sw.active)
            gpu_note_software_renderer ();   /* no device renders: say so in `status` */
        g_idle_add (diag_say, &Sw);   /* the summary (diag.h), unless a wallpaper is loading */
    }
    g_mutex_unlock (&Sw.lock);
}

/* Test children (`--sw-probe`): the daemon's own way to its display once,
 * without events or pause signals. The parent hands its choice of window
 * system down with --backend; a child started by hand decides as the
 * daemon would (the saved setting is not read: no config is loaded here). */
static gboolean backend_decide (const char *cli, gboolean windowed, gboolean quiet, char **err);

static gboolean probe_open (int argc, char **argv, char **err)
{
    const char *cli = NULL;
    for (int i = 1; i + 1 < argc; i++)
        if (!g_strcmp0 (argv[i], "--backend"))
            cli = argv[i + 1];
    if (!backend_decide (cli, FALSE, TRUE, err))
        return FALSE;
    WalpePlatformOpts po = { .probe = TRUE };
    return walpe_platform_open (D.backend, &po, err);
}

/* `walpe-daemon --sw-probe`, run by sw_setup() in a child that has the
 * software environment: the daemon's own path once - EGL on the display,
 * its config, a context from its ladder, a surface as the wallpaper
 * windows have them (never shown), one frame. Reports key=value lines on
 * stdout (walpe_sw_parse_probe). On X11 an X error ends the child through
 * Xlib's default handler: no "drew=1". */
static int sw_probe_run (int argc, char **argv)
{
    alarm (10);   /* never hold up the daemon's start */
    char *err = NULL;
    if (!probe_open (argc, argv, &err)) {
        g_print ("fail=%s\n", err ? err : "cannot open the display");
        return 1;
    }
    const char *where = D.backend == WALPE_BACKEND_WAYLAND ? "Wayland compositor" : "X server";
    if (walpe_platform_egl_display () == EGL_NO_DISPLAY) {
        /* libglvnd had nothing to ask: the library named by the vendor
         * file did not load */
        const char *vf = g_getenv ("__EGL_VENDOR_LIBRARY_FILENAMES");
        g_print ("fail=Mesa's EGL library (libEGL_mesa, named in %s) could not be loaded\n",
                 vf ? vf : "its vendor file");
        return 1;
    }
    if (!init_egl ()) {
        g_print ("fail=Mesa's EGL does not start on this %s\n", where);
        return 1;
    }
    const char *ev = eglQueryString (D.egl_dpy, EGL_VENDOR);
    g_print ("egl=%s\n", ev ? ev : "?");
    WalpeSurfaceSpec spec = { .kind = WALPE_SURFACE_PROBE, .width = 64, .height = 64 };
    EGLSurface surf = EGL_NO_SURFACE;
    int pw = 0, ph = 0;
    WalpeSurface *ps = walpe_surface_new (&spec, D.egl_dpy, D.egl_conf, &surf, &pw, &ph);
    EGLContext ctx = ps ? create_thread_context () : EGL_NO_CONTEXT;
    if (!ps || ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent (D.egl_dpy, surf, surf, ctx)) {
        g_print ("fail=Mesa's EGL cannot make a window surface or a GL 3.3 / GLES 3 context "
                 "on this %s (EGL error 0x%x)\n", where, eglGetError ());
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
    walpe_platform_sync ();   /* an X error of the presentation arrives here */
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
        /* the child draws on the window system this daemon chose */
        char *pargv[] = { Sw.exe, (char *) "--sw-probe", (char *) "--backend",
                          (char *) walpe_backend_name (D.backend), NULL };
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

/* May a restart in place begin?
 *
 * The restart leaves the main loop, takes the daemon apart - render
 * threads, wallpapers, GL, the display - and execs the same binary. When
 * that exec fails there is nothing left to go back to: the daemon ends, and
 * the desktop has no wallpaper until somebody starts it again. So whoever
 * wants a restart asks here first, while everything still stands: can the
 * binary be executed (by its path, or through /proc/self/exe)? If not, the
 * restart does not happen: the daemon runs on as it was, the log says so
 * once, and status carries "restart_failed" with the reason. What the
 * restart was for (another monitor layout, a setting that needs a new
 * process, the web module that was to be left behind) is not done.
 * WALPE_RESTART_EXEC=fail (tests): as if the binary could not be executed.
 * An exec that fails although this said yes (no memory, a file system that
 * went away in between) still ends the daemon: restart_exec. */
static gboolean restart_begin (const char *what)
{
    const char *t = g_getenv ("WALPE_RESTART_EXEC");
    const gboolean can = g_strcmp0 (t, "fail") != 0 &&
                         ((Sw.exe && g_file_test (Sw.exe, G_FILE_TEST_IS_EXECUTABLE)) ||
                          access ("/proc/self/exe", X_OK) == 0);
    if (can)
        return TRUE;
    if (!restart_failed)
        g_warning ("%s: walpe-daemon cannot be restarted in place (%s can no longer be executed): it "
                   "runs on as it is; start it again to apply this (walpe-ctl status: restart_failed)",
                   what, Sw.exe ? Sw.exe : "its binary");
    g_free (restart_failed);
    restart_failed = g_strdup_printf ("%s: the daemon's binary can no longer be executed, the restart "
                                      "in place was not begun", what);
    state_touch ();
    return FALSE;
}

static gboolean sw_restart_cb (gpointer user)
{
    (void) user;
    Sw.restart_id = 0;
    if (!restart_begin ("software rendering setting changed"))
        return G_SOURCE_REMOVE;
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

#include "childend.h"     /* walpe_children_end: SIGTERM, SIGKILL, both waits with a limit */

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
    /* SIGTERM, up to 1 s, SIGKILL, up to 2 s more (daemon/childend.h): the
     * wait after the kill had no limit, and one child that the kernel does
     * not let go of held the restart for as long */
    if (walpe_children_end (kids, WALPE_CHILD_TERM_MS, WALPE_CHILD_KILL_MS, NULL))
        for (guint i = 0; i < kids->len; i++) {
            const int k = (int) g_array_index (kids, pid_t, i);
            char *comm = diag_comm (k);
            g_warning ("restart: child process %d (%s) has not ended %.0f s after SIGKILL (it does "
                       "not come back from a call into the kernel): the restart goes on without "
                       "waiting for it; it stays a child of this process", k,
                       comm && *comm ? walpe_log_strn (comm, 32) : "?", WALPE_CHILD_KILL_MS / 1000.0);
            g_free (comm);
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
    if (swap_restart) {
        /* swaps failed for good: when (swap_lost_cb) */
        char now[32];
        g_snprintf (now, sizeof now, "%" G_GINT64_FORMAT, g_get_monotonic_time ());
        env = g_environ_setenv (env, "WALPE_SWAP_RESTARTED", now, TRUE);
    }
    if (web_unload_restart) {
        /* the web module was left behind: when (web_unload_due) */
        char now[32];
        g_snprintf (now, sizeof now, "%" G_GINT64_FORMAT, g_get_monotonic_time ());
        env = g_environ_setenv (env, "WALPE_WEB_UNLOADED", now, TRUE);
    }
    /* the display went away and this restart waits for it: the new image
     * goes back to the same window system instead of choosing anew while
     * nothing answers */
    if (D.display_lost)
        env = g_environ_setenv (env, "WALPE_DISPLAY_LOST", walpe_backend_name (D.backend), TRUE);
#ifdef HAVE_WAYLAND
    /* ... and knows how long the display has been there, or how many
     * times in a row it was not for long (lost_decide). Only where the
     * daemon would wait for its display at all. */
    if (D.backend == WALPE_BACKEND_WAYLAND && (Bk.stays.shorts || Bk.stays.since_us) &&
        reconnect_wait_s () > 0) {
        char rec[48];
        g_snprintf (rec, sizeof rec, "%d,%" G_GINT64_FORMAT, Bk.stays.shorts, Bk.stays.since_us);
        env = g_environ_setenv (env, "WALPE_DISPLAY_STAYS", rec, TRUE);
    }
#endif
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
    {
        /* by its path; should that fail, the image this process runs (the
         * same file unless it was replaced meanwhile) */
        const gboolean by_path = g_file_test (Sw.exe, G_FILE_TEST_IS_EXECUTABLE);
        execve (by_path ? Sw.exe : "/proc/self/exe", Sw.argv, env);
        const int e1 = errno;
        if (by_path && strcmp (Sw.exe, "/proc/self/exe") != 0)
            execve ("/proc/self/exe", Sw.argv, env);
        errno = e1;
    }
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

/* A render thread that does not come back. A wallpaper is handed to a
 * monitor's render thread (mon_assign: load_path) and taken by it at the top
 * of its loop - after the frame or the load it is in. A frame that does not
 * end (on a software renderer: a wallpaper's shader that loops a thousand
 * million times per pixel, or more to fill than the CPU manages in minutes)
 * therefore kept the wallpaper on that monitor for good: `open` was answered
 * with "ok" and nothing changed, and the daemon could not be told to quit
 * either (mons_wait_end). The crash-loop window is the moment to look: it
 * closes LOADMARK_SECS after the last wallpaper was handed out, on the main
 * thread, and costs nothing new. A monitor whose thread has still not taken
 * what it was given then is restarted with the daemon, in place: the new
 * image loads the wallpapers of the saved configuration, the one that was
 * asked for among them, and the thread that did not come back ends with the
 * old image. Nothing is done while nobody asks for another wallpaper: there
 * is no judgement here of how long a frame may take. */
/* the name of the wallpaper a monitor's render thread draws or loads (the
 * folder's or the file's own name, g_free), NULL = none */
static char *mon_thread_in (Mon *m)
{
    g_mutex_lock (&m->lock);
    const char *p = m->load_path ? m->busy_path : m->given_path;
    char *n = p && *p ? g_path_get_basename (p) : NULL;
    g_mutex_unlock (&m->lock);
    return n;
}

static gboolean load_held_retry (gpointer user);
static gboolean load_held;      /* main: the restart under way is that one */
static guint load_held_timer;   /* main: the look that was put off (load_held_later) */
static gboolean term_pending (void);

/* look again in secs seconds; one such look is pending at most */
static void load_held_later (guint secs)
{
    if (!load_held_timer)
        load_held_timer = g_timeout_add_seconds (secs, load_held_retry, NULL);
}

static void load_held_check (void)
{
    if (D.restart || Sw.restart || !D.loop || D.windowed || term_pending ())
        return;
    const gint64 now = g_get_monotonic_time ();
    gint64 later_us = 0;    /* a monitor that waits for a load to end: when to look again */
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        g_mutex_lock (&m->lock);
        const gboolean held = m->load_path != NULL && !g_atomic_int_get (&m->thread_ended);
        /* A thread that is loading has not taken what it was given either,
         * and a load may take as long as it takes: a large scene from a
         * cold disk or a network share, the first compile of its shaders,
         * a machine that is busy. That is no frame that does not end, and
         * restarting the daemon for it throws the load away - with
         * WALPE_LOAD_DELAY_MS=30000 a wallpaper that was opened a second
         * time 3 s into its load was on screen after 55 s instead of 31,
         * and every other monitor loaded its wallpaper again. So: only a
         * thread that has been out of every load for the whole window. */
        const gint64 quiet_us = m->in_load ? 0 : now - m->load_end_us;
        g_mutex_unlock (&m->lock);
        if (!held)
            continue;
        if (quiet_us < (gint64) LOADMARK_SECS * G_USEC_PER_SEC) {
            /* (the first of them that is due: another monitor may be held) */
            const gint64 due_us = (gint64) LOADMARK_SECS * G_USEC_PER_SEC - quiet_us;
            later_us = later_us ? MIN (later_us, due_us) : due_us;
            continue;
        }
        if (Imp.active || Ws.active) {
            /* (as every restart in place: not under a running import or
             * Workshop download, which it would end) */
            load_held_later (LOADMARK_SECS);
            return;
        }
        if (!restart_begin ("a wallpaper that was asked for is not taken by its monitor"))
            return;
        if (!walpe_config_save (D.cfg))
            return;     /* (the new image would read other wallpapers than were asked for) */
        char *was = mon_thread_in (m);
        g_warning ("monitor %d (%s): its render thread has been in one frame%s%s for more "
                   "than %d s and has not taken the wallpaper it was given since: restarting in "
                   "place to show it (a frame of that wallpaper does not end on this renderer)",
                   m->index, m->name ? m->name : "?", was ? " of " : "",
                   was ? walpe_log_str (was) : "", LOADMARK_SECS);
        g_free (was);
        load_held = TRUE;
        D.restart = TRUE;
        g_main_loop_quit (D.loop);
        return;
    }
    if (later_us > 0)
        load_held_later ((guint) (later_us / G_USEC_PER_SEC) + 1);
}

static gboolean load_held_retry (gpointer user)
{
    (void) user;
    load_held_timer = 0;
    load_held_check ();
    return G_SOURCE_REMOVE;
}

static gboolean loadmark_due (gpointer user)
{
    loadmark_clear (user);
    load_held_check ();
    return G_SOURCE_REMOVE;
}

/* The daemon ends or restarts in place: every render thread was told
 * (quitting) and is waited for RENDER_END_WAIT_S in all. NULL = they have
 * all ended; else a monitor whose thread is still inside a frame or a load
 * - it cannot be joined, and what it owns cannot be freed under it. */
#define RENDER_END_WAIT_S 10
static Mon *mons_wait_end (void)
{
    /* (a thread this restart is for has been waited for already: one more
     * second, for the others) */
    const gint64 until = g_get_monotonic_time () +
                         (gint64) (load_held ? 1 : RENDER_END_WAIT_S) * G_USEC_PER_SEC;
    for (;;) {
        Mon *left = NULL;
        for (GList *l = D.mons; l && !left; l = l->next)
            if (!g_atomic_int_get (&((Mon *) l->data)->thread_ended))
                left = l->data;
        if (!left || g_get_monotonic_time () >= until)
            return left;
        g_usleep (1000);
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
 * main() looks at term_signal before the exec (term_pending).
 *
 * The note is an atomic: a signal sent to the process is handled by
 * whichever thread does not block it - a render thread, a decoder, one of
 * GLib's or the GL driver's - while the main thread reads the note.
 * `volatile sig_atomic_t` only covers a handler that interrupts the thread
 * that reads. A lock-free atomic may be stored from a signal handler
 * (C11 7.14.1.1); the default ordering (sequentially consistent) because
 * the one reader that is not woken through term_fd, term_pending (), must
 * see a store that came before its look. Nothing here is on a frame's path. */
#include <stdatomic.h>
_Static_assert (ATOMIC_INT_LOCK_FREE == 2, "term_signal is stored from a signal handler");
static atomic_int term_signal;   /* the signal that arrived (0 = none) */
static atomic_int term_sender;   /* the process that sent it, as the kernel tells (0 = the
                                  * kernel itself, or not told): for the log line only */
static int term_fd = -1;         /* set before the handler is installed */

static void term_handler (int sig, siginfo_t *si, void *uctx)
{
    (void) uctx;
    int e = errno;
    atomic_store (&term_sender, si && si->si_code <= 0 ? (int) si->si_pid : 0);
    atomic_store (&term_signal, sig);
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
    diag_quit_signal (atomic_load (&term_signal), atomic_load (&term_sender));
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
    sa.sa_sigaction = term_handler;     /* (SA_SIGINFO: who sent it, for the log) */
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
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
    return atomic_load (&term_signal) != 0;
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
            g_string_append_printf (ln, ", paused%s", mon_fullscreen_pause (m) ? ": fullscreen"
                                                      : m->covered ? ": covered" : "");
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

/* The surface a monitor is rendered to: the platform makes the native
 * window and the EGL surface on it. out: the output it covers (NULL: the
 * --windowed window of g's size). *sw, *sh: the surface's size. NULL = none
 * could be made (said). say: the line about the swap behaviour. */
static WalpeSurface *mon_surface_make (Mon *m, const WalpeOutput *out, const WalpeRect *g,
                                       gboolean or_mode, int idx, gboolean say, EGLSurface *surf,
                                       int *sw, int *sh)
{
    WalpeSurfaceSpec spec = {
        .kind = or_mode ? WALPE_SURFACE_WALLPAPER : WALPE_SURFACE_WINDOW,
        .output = or_mode ? out : NULL,
        .width = g->width,
        .height = g->height,
        .title = "walpe (preview)",
        .user = m,
    };
    WalpeSurface *ps = walpe_surface_new (&spec, D.egl_dpy, D.egl_conf, surf, sw, sh);
    if (!ps) {
        EGLint err = eglGetError ();
#ifdef HAVE_WAYLAND
        /* On Wayland a surface mostly fails before EGL is asked for
         * anything: the compositor did not answer it, its output is gone
         * (the backend has said which, in the line before this one). The
         * line below then named a call that was never made, with
         * "0x3000", which is EGL's word for success. */
        if (err == EGL_SUCCESS && D.backend == WALPE_BACKEND_WAYLAND) {
            g_warning ("monitor %d: no surface to draw on (no EGL call failed: the reason is in "
                       "the line above)", idx);
            return NULL;
        }
#endif
        g_warning ("eglCreateWindowSurface failed for monitor %d (0x%x)", idx, err);
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
        gboolean have = eglQuerySurface (D.egl_dpy, *surf, EGL_SWAP_BEHAVIOR, &sb);
        if (!have)
            (void) eglGetError ();
        gboolean set = FALSE;
        if (have && sb == EGL_BUFFER_PRESERVED && !(e && !strcmp (e, "keep"))) {
            set = eglSurfaceAttrib (D.egl_dpy, *surf, EGL_SWAP_BEHAVIOR, EGL_BUFFER_DESTROYED);
            if (!set)
                (void) eglGetError ();
        }
        if (say)
            g_message ("EGL surface: swap behaviour %s%s",
                       !have ? "unknown" : sb == EGL_BUFFER_PRESERVED ? "preserved" : "destroyed",
                       set ? " -> destroyed" : "");
    }
    return ps;
}

/* One monitor: its record and the surface it is rendered to. out: the
 * output it covers (NULL: the --windowed window of g's size). */
static Mon *make_monitor (const WalpeOutput *out, const WalpeRect *g, int idx, gboolean or_mode,
                          const char *name)
{
    Mon *m = g_new0 (Mon, 1);
    m->name = name && *name ? g_strdup (name) : g_strdup_printf ("mon%d", idx);
    m->ptr_lx = m->ptr_ly = -2.0f;
    m->index = idx;
    m->geom = *g;
    m->present_iv_applied = -1;
    m->present_iv = 1;
    int sw = 0, sh = 0;
    m->ps = mon_surface_make (m, out, g, or_mode, idx, idx == 0, &m->surf, &sw, &sh);
    if (!m->ps) {
        g_free (m->name);
        g_free (m);
        return NULL;
    }
    m->out_id = walpe_surface_output (m->ps);
    /* what is rendered and presented is the surface: on X11 it always has
     * the monitor's size; a window system that sizes surfaces itself may
     * have made it another */
    if (sw > 0 && sh > 0) {
        m->geom.width = sw;
        m->geom.height = sh;
    }
#ifdef WALPE_FOLLOW
    m->rt_w = m->geom.width;
    m->rt_h = m->geom.height;
#endif
    g_mutex_init (&m->lock);
    g_cond_init (&m->cond);
    g_cond_init (&m->mem_cond);
    m->clip_n = -1;     /* everything, until "Skip hidden areas" says otherwise */
    m->in_load = TRUE;  /* until its thread is in its loop (mon_thread) */
    D.mons = g_list_append (D.mons, m);
    return m;
}

/* ------------------------------------------------------ monitor layout
 * The outputs as the platform lists them (platform.h, WalpeOutput):
 * rectangles, output names and refresh rates. On X11: Xinerama, else the
 * whole screen, WALPE_MONITORS overrides; RandR 1.5 monitor names matched
 * by geometry, else "monN"; WALPE_MONITORS "@HZ", else the RandR mode.
 * Read at start-up and again whenever the window system says they
 * changed. */
/* layout_now (declared above, with the status code): the layout this
 * daemon runs with */
static gboolean layout_watch;   /* follow changes (not in --windowed) */
static guint layout_timer;
static gboolean layout_force;   /* a surface was taken away: restart even when the
                                 * list reads the same as at start-up */

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
static WalpeOutputList layout_pending;   /* a different layout, seen once */

#ifdef WALPE_FOLLOW
/* ------------------------------------------- outputs followed in place
 * A window system whose surfaces last while their output changes
 * (walpe_platform_follows_outputs: Wayland) needs no restart for a change
 * of the outputs. The restart costs every monitor its picture for two to
 * ten seconds and every wallpaper its place: a video starts over, a scene
 * begins again - on the monitor nobody touched too - for a monitor that
 * went to sleep, a laptop that was docked, a scale chosen in the desktop's
 * settings. layout_check () therefore asks layout_follow () first, with
 * the layout it has seen twice, and restarts only where that says no.
 *
 * What is followed, per output of the new list (walpe_follow_plan):
 *  - the same output at another place, scale, refresh rate or transform:
 *    its monitor gets the new rectangle and rate. The surface needs
 *    nothing (the compositor configured its new logical size, the backend
 *    showed the buffer at it); neither its wallpaper nor anybody else's is
 *    touched.
 *  - the same output with another size in pixels (a mode, a scale without
 *    viewporter, a quarter turn): the surface stays, its render thread
 *    gives the buffers the new size between two frames (Mon.fit_req,
 *    walpe_surface_fit) and draws at it. What depends on the size follows
 *    as it does for a render resolution chosen with `walpe-ctl resolution`
 *    (res_refresh, res_commit): a scene on that monitor is loaded again,
 *    groups that share a rendering are formed again. The other monitors
 *    go on.
 *  - an output that is gone: its monitor is PARKED (mon_park). The render
 *    thread frees the wallpaper, takes its context off the surface and
 *    waits (mon_park_wait); the main thread then frees the surface. Thread
 *    and context stay.
 *  - a new output: a parked monitor is given to it (mon_attach): a new
 *    surface, every field of the record anew, its thread goes on with the
 *    context it has, and the wallpaper saved for the output is loaded.
 *    A monitor that is switched off and on again is this case (the
 *    compositor makes a new output of the old name).
 *  - a monitor whose surface the compositor closed for good while its
 *    output stays is parked and given the output again: a new surface for
 *    that output alone.
 * Not followed, the restart in place as before: an output for which no
 * render thread waits - more outputs than the daemon has had at once.
 * A new thread would have to make a GL context in the share group while
 * the other threads' contexts are current, which the start-up takes care
 * never to do (ctx_lock), and no driver was at hand to try it on.
 * Likewise --monitor N and swaps that fail for good (swap_lost_cb: the
 * contexts are made anew).
 *
 * All of this only with `wayland_follow = on` in gpu.conf
 * (WALPE_WAYLAND_FOLLOW=on). Off is the default and the behaviour up to
 * 0.14.0: the restart in place for every change. The one thing done in
 * place either way is the next try for an output that got no surface
 * (below).
 *
 * A record is never freed while the daemon runs: a shared frame names its
 * followers, queued callbacks name their monitor. Its thread waits on its
 * own lock and condition, so a record is used again for the next output
 * with everything else in it wiped (mon_wipe). Mon.life tells a load's
 * note that it was made for the output before. */
static GList *mons_parked;      /* Mon: monitors without an output (their threads wait,
                                 * or are on their way there: Mon.park_told) */
static int layout_only = -1;    /* --monitor N: only that output has a monitor */
static gboolean layout_owed;    /* an output of layout_now waits for a render thread
                                 * that is not parked yet: layout_check looks again */
static int layout_owed_looks;   /* ... and has done so this often */
/* half-second looks an output waits for a render thread that is still busy
 * with the output it had (it loads a wallpaper, or draws a slow frame)
 * before the daemon restarts in place for it after all */
#define LAYOUT_OWED_LOOKS 40
static gboolean layout_pending_set;     /* layout_pending holds a layout (one without
                                         * any output is one too) */

/* ---- an output no surface could be made on
 * walpe_surface_new waits three seconds for the compositor to configure a
 * wallpaper surface. Where no answer came the daemon said so once and
 * left that output empty for good: a daemon started before the compositor
 * was ready to show layer surfaces - river 0.4 and later until its window
 * manager is connected, a session that is still starting - ran, answered
 * and showed nothing until somebody started it again.
 * Such an output is tried again: NOSURF_TRIES times, the first try two
 * seconds after the failure and each one twice as long after the one
 * before, a minute at most (2, 4, 8, 16, 32, 60, 60, 60 s: four minutes
 * in all), each waiting half a second for the answer, and at once
 * whenever the outputs change. A render thread waits for it meanwhile
 * (mon_make_waiting: at the start one is made for an output without a
 * surface, with its context, so that the surface can be given to it
 * later). When the tries are used up the log, `walpe-ctl diag` and status
 * say that it was given up; the next change of an output begins anew. The
 * daemon runs on either way: its window and walpe-ctl go on working.
 * Also with `wayland_follow = off`: a try gives a surface to a render
 * thread that has waited since the start and touches no monitor that
 * draws; a change of an output is then followed by the restart, which
 * makes every surface anew and begins the schedule again when one is
 * still missing. Not on X11 (it has no such failure) and not with
 * --monitor N (said once, never tried again). */
#define NOSURF_TRIES 8
#define NOSURF_WAIT_MS 500
static GPtrArray *nosurf;       /* char *: names of the outputs without a surface */
static int nosurf_tries;        /* tries made since the first failure, or since the outputs
                                 * changed last */
static int nosurf_total;        /* tries since the first failure, the failure included */
static gint64 nosurf_since_us;  /* when that was */
static guint nosurf_timer;      /* the next try */
static gboolean nosurf_due;     /* ... is now (layout_check) */
static gboolean nosurf_gave_up;

static gboolean nosurf_has (const char *name)
{
    for (guint i = 0; nosurf && i < nosurf->len; i++)
        if (!g_strcmp0 (g_ptr_array_index (nosurf, i), name))
            return TRUE;
    return FALSE;
}

/* WALPE_SURFACE_RETRY_MS (tests): the time before the first try, 2000 */
static guint nosurf_delay_ms (int tries)
{
    static guint base;
    if (!base) {
        const char *e = g_getenv ("WALPE_SURFACE_RETRY_MS");
        base = e && *e ? (guint) CLAMP (g_ascii_strtoll (e, NULL, 10), 10, 60000) : 2000;
    }
    return MIN (base << MIN (tries, 8), base * 30);
}

static gboolean layout_check (gpointer user);

static gboolean nosurf_retry_cb (gpointer user)
{
    (void) user;
    nosurf_timer = 0;
    nosurf_due = TRUE;
    if (layout_timer)
        g_source_remove (layout_timer);
    layout_timer = 0;
    layout_check (NULL);
    return G_SOURCE_REMOVE;
}

/* main: the names in `nosurf` changed, or a round of tries is over: what
 * status and the summary say, and when the next try is. tried: this was a
 * try (not the failure that begins a round). */
static void nosurf_update (gboolean tried)
{
    g_clear_pointer (&mons_no_surface, g_free);
    /* (the time of the next try stands while nothing was tried: a look at
     * the outputs for another reason must not put it off) */
    if (nosurf_timer && (tried || !nosurf || !nosurf->len)) {
        g_source_remove (nosurf_timer);
        nosurf_timer = 0;
    }
    if (!nosurf || !nosurf->len) {
        nosurf_tries = nosurf_total = 0;
        nosurf_gave_up = FALSE;
        return;
    }
    GString *names = g_string_new (NULL);
    for (guint i = 0; i < nosurf->len; i++)
        g_string_append_printf (names, "%s%s", i ? ", " : "", (char *) g_ptr_array_index (nosurf, i));
    if (tried)
        nosurf_tries++;
    if (!nosurf_total)
        nosurf_since_us = g_get_monotonic_time ();
    if (tried || !nosurf_total)
        nosurf_total++;
    const int secs = (int) ((g_get_monotonic_time () - nosurf_since_us) / G_USEC_PER_SEC);
    if (nosurf_tries >= NOSURF_TRIES) {
        if (!nosurf_gave_up)
            g_warning ("NO WALLPAPER SURFACE on %s: the window system gave none in %d tries over "
                       "%d s. walpe-daemon gives up on it and runs on; it begins to try again when "
                       "an output changes, or start it again (walpe-ctl quit, then walpe-daemon) "
                       "once the desktop is up", names->str, nosurf_total, secs);
        nosurf_gave_up = TRUE;
        mons_no_surface = g_strdup_printf ("%s [given up after %d tries over %d s; tried again when "
                                           "an output changes]", names->str, nosurf_total, secs);
    } else {
        const guint ms = nosurf_delay_ms (nosurf_tries);
        nosurf_gave_up = FALSE;
        if (!nosurf_timer)
            nosurf_timer = g_timeout_add (ms, nosurf_retry_cb, NULL);
        mons_no_surface = g_strdup_printf ("%s [%d tries without an answer; the next one follows "
                                           "%g s after the last, %d more at most]", names->str,
                                           nosurf_total, ms / 1000.0, NOSURF_TRIES - nosurf_tries);
    }
    g_string_free (names, TRUE);
}

/* main, at the start, before the render threads exist: a monitor without
 * an output, for the output that got no surface. Its thread makes its
 * context with the others' and waits (mon_thread). */
static void mon_make_waiting (void)
{
    Mon *m = g_new0 (Mon, 1);
    m->name = g_strdup ("-");
    m->index = -1;
    m->ptr_lx = m->ptr_ly = -2.0f;
    m->surf = EGL_NO_SURFACE;
    m->present_iv_applied = -1;
    m->present_iv = 1;
    m->clip_n = -1;
    g_mutex_init (&m->lock);
    g_cond_init (&m->cond);
    g_cond_init (&m->mem_cond);
    m->parked = TRUE;
    m->park_told = TRUE;
    mons_parked = g_list_append (mons_parked, m);
}

/* wayland_follow (gpu.conf, WALPE_WAYLAND_FOLLOW): off unless it says on.
 * Following in place resizes a surface's buffers under a running render
 * thread and keeps GL contexts for outputs that are gone; that has run on
 * the software renderer only, on no GPU driver yet. */
static gboolean layout_follow_on (void)
{
    static int on = -1;
    if (on < 0) {
        gboolean b = FALSE;
        const char *v = present_setting ("wayland_follow");
        on = v && walpe_conf_bool (v, &b) && b;
        if (v && !walpe_conf_bool (v, &b))
            g_warning ("wayland_follow = %s is not understood (on, off): off", v);
        if (on)
            g_message ("wayland_follow is on: a change of an output is followed in place, without "
                       "the restart (off, the default: the restart in place for every change)");
    }
    return on;
}

/* main, queued by a render thread that has begun to wait (mon_park_wait):
 * nothing uses the surface of its monitor any more */
static gboolean mon_parked_cb (gpointer user)
{
    Mon *m = user;
    g_mutex_lock (&m->lock);
    const gboolean parked = m->parked;
    g_mutex_unlock (&m->lock);
    if (!parked || m->park_told)
        return G_SOURCE_REMOVE;
    m->park_told = TRUE;
    if (m->ps) {
        walpe_surface_free (m->ps);   /* its EGL surface, then its window */
        m->ps = NULL;
        m->surf = EGL_NO_SURFACE;
    }
    if (layout_owed)
        layout_schedule ();     /* an output waits for this thread */
    return G_SOURCE_REMOVE;
}

/* main: m has no output any more (why: for the log). It leaves the list of
 * monitors at once; its thread lets go of the wallpaper and the surface
 * when it gets there. The caller forms the groups again (apply_wallpapers):
 * monitors that followed m still name it as their leader until then. */
static void mon_park (Mon *m, const char *why)
{
    g_message ("monitor %d (%s): %s: its wallpaper is unloaded, its render thread waits for an "
               "output", m->index, m->name ? m->name : "?", why);
    D.mons = g_list_remove (D.mons, m);
    mons_parked = g_list_append (mons_parked, m);
    if (m->expose_timer_id) {
        g_source_remove (m->expose_timer_id);
        m->expose_timer_id = 0;
    }
    m->park_told = FALSE;
    g_mutex_lock (&m->lock);
    g_clear_pointer (&m->load_path, g_free);    /* a load its thread has not taken */
    m->loading = FALSE;
    if (m->role_sf)
        sf_unref (m->role_sf);                  /* a pending role nobody adopted */
    m->role_sf = NULL;
    m->role_idx = MON_ROLE_PARK;
    m->role_change = TRUE;
    g_cond_signal (&m->cond);
    g_mutex_unlock (&m->lock);
}

/* main, m->lock held, m's thread waits in mon_park_wait (or has ended
 * without a context): the record as g_new0 gave it, but for what the
 * waiting thread stands on (the lock, the conditions, the thread, its
 * context), its count of outputs, and whether the thread has ended */
static void mon_wipe (Mon *m)
{
    g_free (m->load_path);
    g_free (m->first_path);
    g_free (m->content_path);
    g_free (m->name);
    g_free (m->want_path);
    g_free (m->loaded_path);
    g_free (m->load_error);
    g_free (m->good_path);
    g_free (m->reopen_for);
    g_free (m->restored);
    g_free (m->qdescribe);
    g_free (m->res_note);
    g_free (m->mem_json);
    g_free (m->given_path);
    g_free (m->busy_path);
    walpe_region_clear (&m->vis);
    if (m->ptr_q)
        g_array_unref (m->ptr_q);
    if (m->role_sf)
        sf_unref (m->role_sf);
    GThread *thread = m->thread;
    const EGLContext ctx = m->ctx;
    const guint life = m->life;
    const gboolean quitting = m->quitting;
    /* (a thread that got no context has left for good, mon_thread: the
     * new record must not say that it is still there - the daemon's end
     * waits ten seconds for every thread that has not ended, and then
     * leaves without taking itself apart) */
    const gint ended = g_atomic_int_get (&m->thread_ended);
    char *base = (char *) m;
    /* [start, lock) [thread, mem_cond) (mem_cond, end): lock and cond lie
     * side by side */
    G_STATIC_ASSERT (G_STRUCT_OFFSET (Mon, cond) == G_STRUCT_OFFSET (Mon, lock) + sizeof (GMutex));
    G_STATIC_ASSERT (G_STRUCT_OFFSET (Mon, thread) == G_STRUCT_OFFSET (Mon, cond) + sizeof (GCond));
    memset (base, 0, G_STRUCT_OFFSET (Mon, lock));
    memset (base + G_STRUCT_OFFSET (Mon, thread), 0,
            G_STRUCT_OFFSET (Mon, mem_cond) - G_STRUCT_OFFSET (Mon, thread));
    memset (base + G_STRUCT_OFFSET (Mon, mem_cond) + sizeof (GCond), 0,
            sizeof (Mon) - G_STRUCT_OFFSET (Mon, mem_cond) - sizeof (GCond));
    m->thread = thread;
    m->ctx = ctx;
    m->life = life + 1;
    m->quitting = quitting;
    g_atomic_int_set (&m->thread_ended, ended);
    m->parked = TRUE;
    m->park_told = TRUE;
    /* as make_monitor: until its thread has its context on the new surface
     * and is in its loop again (mon_park_wait), it is setting up - not in a
     * frame that does not end (load_held_check) */
    m->in_load = !ended;
}

static int mons_waiting (void)
{
    return (int) g_list_length (mons_parked);
}

/* main: a parked monitor whose thread waits, NULL = none */
static Mon *mon_parked_ready (void)
{
    for (GList *l = mons_parked; l; l = l->next)
        if (((Mon *) l->data)->park_told)
            return l->data;
    return NULL;
}

/* main: output o (entry idx of the layout) gets the parked monitor m: a
 * surface on it and a record set up as make_monitor and the start-up set
 * up a new one. Its thread goes on waiting until mon_unpark. NULL = no
 * surface could be made on the output (said); m stays parked. */
static Mon *mon_attach (Mon *m, const WalpeOutput *o, int idx)
{
    WalpeRect g = { o->x, o->y, o->width, o->height };
    EGLSurface surf = EGL_NO_SURFACE;
    int sw = 0, sh = 0;
    WalpeSurface *ps = mon_surface_make (m, o, &g, TRUE, idx, FALSE, &surf, &sw, &sh);
    if (!ps)
        return NULL;
    g_mutex_lock (&m->lock);
    mon_wipe (m);
    m->name = o->name && *o->name ? g_strdup (o->name) : g_strdup_printf ("mon%d", idx);
    m->ptr_lx = m->ptr_ly = -2.0f;
    m->index = idx;
    m->geom = g;
    if (sw > 0 && sh > 0) {
        m->geom.width = sw;
        m->geom.height = sh;
    }
    m->rt_w = m->geom.width;
    m->rt_h = m->geom.height;
    m->present_iv_applied = -1;
    m->present_iv = 1;
    m->ps = ps;
    m->surf = surf;
    m->out_id = walpe_surface_output (ps);
    m->hz = o->refresh_hz;
    m->clip_n = -1;
    m->needs_first_frame = TRUE;
    g_mutex_unlock (&m->lock);
    mons_parked = g_list_remove (mons_parked, m);
    D.mons = g_list_append (D.mons, m);     /* (layout_follow puts the list in order) */
    return m;
}

/* main: everything of m is set: its thread puts its context on the new
 * surface and goes on */
static void mon_unpark (Mon *m)
{
    g_mutex_lock (&m->lock);
    m->pause_wanted = m->self_paused = m->own_paused = want_pause_mon (m);
    m->fps = m->fps_cap = mon_fps_cap (m);
    walpe_config_quality_for (D.cfg, mon_quality_path (m), &m->quality);
    m->parked = FALSE;
    m->park_told = FALSE;
    g_cond_signal (&m->cond);
    g_mutex_unlock (&m->lock);
}

static gint mon_index_cmp (gconstpointer a, gconstpointer b)
{
    return ((const Mon *) a)->index - ((const Mon *) b)->index;
}

typedef enum { FOLLOW_NO, FOLLOW_DONE, FOLLOW_WAIT } FollowResult;

/* main: the outputs are `l` now, and that has held for two looks. DONE:
 * the monitors are on it. WAIT: also, but for an output whose render
 * thread is still letting go of the output it had: look again shortly.
 * NO: nothing was changed (or nothing that a restart does not make anew):
 * the caller restarts in place. */
/* Can an output that got no surface be tried again here? That needs a
 * window system whose surfaces are made per output while the daemon runs,
 * and is not a matter of wayland_follow: such an output was empty for
 * good, and the try touches no monitor that draws. */
static gboolean layout_retries (void)
{
    return walpe_platform_follows_outputs () && !D.windowed && layout_only < 0;
}

/* ... and are changes of the outputs followed in place? */
static gboolean layout_follows (void)
{
    return layout_retries () && layout_follow_on ();
}

static FollowResult layout_follow (const WalpeOutputList *l, gboolean changed)
{
    if (!layout_retries () || swap_restart)
        return FOLLOW_NO;
    /* wayland_follow off (the default): the one thing done in place is the
     * next try for an output that got no surface, on the layout the
     * daemon runs with. A change of an output and a surface that was
     * taken away are followed by the restart, as before. */
    const gboolean full = layout_follows ();
    if (!full && (changed || layout_force || !(nosurf_due || layout_owed) || !nosurf || !nosurf->len))
        return FOLLOW_NO;
    /* outputs that got no surface are tried again when their time has
     * come, and whenever the outputs changed */
    const gboolean retry = changed || nosurf_due;
    nosurf_due = FALSE;
    if (changed) {
        /* (and what was given up is tried from the start of the schedule) */
        nosurf_tries = 0;
        nosurf_gave_up = FALSE;
    }
    guint n = g_list_length (D.mons), i = 0;
    Mon **ms = g_new0 (Mon *, n + 1);
    gboolean *dead = g_new0 (gboolean, n + 1);
    WalpeOutputList run = { (int) n, g_new0 (WalpeOutput, n + 1) };
    gboolean known = TRUE;
    for (GList *it = D.mons; it; it = it->next, i++) {
        ms[i] = it->data;
        const WalpeOutput *o = mon_output (ms[i]);
        if (o)
            run.o[i] = *o;      /* (its strings stay layout_now's) */
        else
            known = FALSE;
    }
    WalpeFollowPlan p = { 0 };
    FollowResult res = FOLLOW_NO;
    if (!known)
        goto out;
    walpe_follow_plan (&run, l, &p);
    /* the surfaces that stay take their output's rectangle; one the window
     * system has closed for good says so here */
    int n_dead = 0;
    int *fit_w = g_new0 (int, n + 1), *fit_h = g_new0 (int, n + 1);
    for (i = 0; i < n; i++) {
        if (p.running[i] == WALPE_FOLLOW_GONE)
            continue;
        const WalpeOutput *o = &l->o[p.running_to[i]];
        fit_w[i] = o->width;
        fit_h[i] = o->height;
        dead[i] = !walpe_surface_follow_output (ms[i]->ps, o, &fit_w[i], &fit_h[i]);
        n_dead += dead[i];
    }
    if (!full && (n_dead || p.gone || p.placed || p.resized)) {
        /* (more than a try for an output without a surface after all: the
         * restart; a surface found closed here is one already) */
        if (n_dead)
            layout_force = TRUE;
        g_free (fit_w);
        g_free (fit_h);
        goto out;
    }
    /* is there a render thread for every output that will be without a
     * monitor? (the ones parked here are: each frees one) */
    if (p.added > (int) g_list_length (mons_parked) + p.gone) {
        g_message ("monitor layout: %d new output(s), %d render thread(s) without an output: "
                   "not followed in place", p.added, (int) g_list_length (mons_parked) + p.gone);
        g_free (fit_w);
        g_free (fit_h);
        goto out;
    }
    gboolean structure = p.gone || p.resized || n_dead;
    int attached = 0;
    /* 1. the monitors whose output is gone, or whose surface is */
    for (i = 0; i < n; i++)
        if (p.running[i] == WALPE_FOLLOW_GONE)
            mon_park (ms[i], "its output is gone");
        else if (dead[i])
            mon_park (ms[i], "the window system closed its surface");
    /* 2. the monitors that stay */
    for (i = 0; i < n; i++) {
        Mon *m = ms[i];
        if (p.running[i] == WALPE_FOLLOW_GONE || dead[i] || !full)
            continue;       /* (!full: nothing of theirs changed, and nothing is touched) */
        const WalpeOutput *o = &l->o[p.running_to[i]];
        const WalpeOutput *was = &run.o[i];
        const gboolean fit = fit_w[i] != m->geom.width || fit_h[i] != m->geom.height ||
                             fabs (o->scale - was->scale) > 0.001;
        m->index = p.running_to[i];
        m->geom.x = o->x;
        m->geom.y = o->y;
        if (fabs (o->refresh_hz - m->hz) > 0.5)
            structure = TRUE;       /* who leads a group follows from the rates */
        g_mutex_lock (&m->lock);
        m->hz = o->refresh_hz;
        if (fit) {
            m->geom.width = fit_w[i];
            m->geom.height = fit_h[i];
            m->fit_req = TRUE;
            m->res_changed = TRUE;
            g_cond_signal (&m->cond);
        }
        g_mutex_unlock (&m->lock);
    }
    g_free (fit_w);
    g_free (fit_h);
    /* 3. the outputs without a monitor */
    GList *fresh = NULL;
    GPtrArray *none = g_ptr_array_new_with_free_func (g_free);
    gboolean owed = FALSE, tried = FALSE;
    for (int j = 0; j < l->n; j++) {
        gboolean have = p.seen_from[j] >= 0 && !dead[p.seen_from[j]];
        if (have)
            continue;
        const gboolean before = nosurf_has (l->o[j].name);
        if (before && !retry) {
            g_ptr_array_add (none, g_strdup (l->o[j].name));    /* (its next try has a time) */
            continue;
        }
        Mon *m = mon_parked_ready ();
        if (!m) {
            owed = TRUE;        /* (its thread is on its way: mon_parked_cb) */
            if (before)
                g_ptr_array_add (none, g_strdup (l->o[j].name));
            continue;
        }
        if (before) {
            tried = TRUE;
            walpe_platform_surface_wait (NOSURF_WAIT_MS);
        }
        Mon *got = mon_attach (m, &l->o[j], j);
        walpe_platform_surface_wait (0);
        if (got) {
            fresh = g_list_prepend (fresh, m);
            attached++;
            if (before)
                g_message ("monitor %d (%s): the window system has given it a wallpaper surface now "
                           "(try %d, %.0f s after the first)", j, l->o[j].name, nosurf_total + 1,
                           (g_get_monotonic_time () - nosurf_since_us) / 1e6);
        } else {
            g_ptr_array_add (none, g_strdup (l->o[j].name));
            if (!before)
                g_warning ("monitor %d (%s): NO WALLPAPER SURFACE could be made on it (the line above "
                           "says why): nothing is shown there. walpe-daemon tries again in %g s, "
                           "%d times in all", j, l->o[j].name, nosurf_delay_ms (0) / 1000.0,
                           NOSURF_TRIES);
        }
    }
    if (nosurf)
        g_ptr_array_unref (nosurf);
    nosurf = none;
    nosurf_update (tried);
    D.mons = g_list_sort (D.mons, mon_index_cmp);
    /* 4. the layout the daemon runs with, and what is worked out from it */
    walpe_output_list_free (&layout_now);
    walpe_output_list_copy (&layout_now, l);
    if (full && (p.placed || p.resized || p.gone || n_dead || attached)) {
        /* (not for a look that only found an output still waiting, and not
         * for a try that gave an output its surface: that has its own line) */
        GString *d = g_string_new (NULL);
        for (int j = 0; j < l->n; j++)
            g_string_append_printf (d, "%s%s %dx%d+%d+%d", j ? ", " : "", l->o[j].name, l->o[j].width,
                                    l->o[j].height, l->o[j].x, l->o[j].y);
        g_message ("monitor layout changed (now: %s): followed in place (%d moved or scaled, %d "
                   "resized, %d gone, %d new%s%s)", l->n ? d->str : "no output", p.placed, p.resized,
                   p.gone, attached, n_dead ? ", a closed surface is made again" : "",
                   owed ? "; an output waits for a render thread that is still busy" : "");
        g_string_free (d, TRUE);
    }
    D.max_hz = 0.0;
    for (GList *it = D.mons; it; it = it->next)
        D.max_hz = MAX (D.max_hz, ((Mon *) it->data)->hz);
    {
        const char **names = g_new0 (const char *, (gsize) l->n + 1);
        for (int j = 0; j < l->n; j++)
            names[j] = l->o[j].name;
        walpe_config_set_outputs (D.cfg, names, (guint) l->n, D.backend == WALPE_BACKEND_WAYLAND);
        g_free (names);
    }
    const gboolean sizes = res_refresh ();
    if (structure || fresh) {
        char *bad = present_apply_spec (P.spec);   /* the present mode goes by name and index */
        g_free (bad);
    }
    for (GList *it = fresh; it; it = it->next)
        mon_unpark (it->data);
    g_list_free (fresh);
    if (structure || attached || sizes)
        res_commit (TRUE);      /* the groups again, each monitor its wallpaper */
    /* who reports what for which output: the ids are looked up again, the
     * values read again (which also brings the pause states up to date) */
    walpe_sigprov_outputs_changed ();
    signals_changed (WALPE_SIG_PER_OUTPUT, NULL);
    layout_force = FALSE;
    layout_owed_looks = owed ? layout_owed_looks + 1 : 0;
    layout_owed = owed;
    res = owed ? FOLLOW_WAIT : FOLLOW_DONE;
    if (owed && layout_owed_looks > LAYOUT_OWED_LOOKS) {
        g_message ("monitor layout: an output has waited %d s for a render thread: not followed "
                   "in place", LAYOUT_OWED_LOOKS / 2);
        layout_owed_looks = 0;
        res = FOLLOW_NO;        /* (layout_check: the restart in place makes its monitor) */
    }
out:
    walpe_follow_plan_clear (&p);
    g_free (run.o);
    g_free (ms);
    g_free (dead);
    return res;
}
#endif  /* WALPE_FOLLOW */

static gboolean layout_check (gpointer user)
{
    (void) user;
    layout_timer = 0;
    WalpeOutputList l = { 0 };
    walpe_platform_outputs (&l);
#ifdef WALPE_FOLLOW
    {
        /* a window system whose surfaces last: the change is followed in
         * place where it can be, once it has held for two looks (or is the
         * layout the daemon is on already, with something still owed) */
        const gboolean same = walpe_output_list_equal (&l, &layout_now);
        const gboolean held = layout_pending_set && walpe_output_list_equal (&l, &layout_pending);
        if (same ? (layout_force || layout_owed || nosurf_due) : held) {
            FollowResult fr = layout_follow (&l, !same);
            if (fr != FOLLOW_NO) {
                walpe_output_list_free (&layout_pending);
                layout_pending_set = FALSE;
                walpe_output_list_free (&l);
                if (fr == FOLLOW_WAIT)
                    layout_timer = g_timeout_add (500, layout_check, NULL);
                return G_SOURCE_REMOVE;
            }
            if (layout_owed) {
                /* an output is still without a monitor and cannot get one
                 * in place any more: the restart makes one for it */
                layout_owed = FALSE;
                layout_force = TRUE;
            }
        }
    }
#endif
    gint64 last = g_ascii_strtoll (g_getenv ("WALPE_LAYOUT_RESTARTED") ? g_getenv ("WALPE_LAYOUT_RESTARTED") : "0",
                                   NULL, 10);
    gboolean too_soon = last > 0 && g_get_monotonic_time () - last < LAYOUT_RESTART_GAP_S * G_USEC_PER_SEC &&
                        g_get_monotonic_time () >= last;
    gboolean settled = FALSE;
    /* (layout_force: a surface is gone - the layout this daemon runs with
     * is not on screen any more, whatever the list says) */
    switch (walpe_layout_verdict (&layout_now, &l, &layout_pending, layout_force, too_soon,
                                  Imp.active || Ws.active, &settled)) {
    case WALPE_LAYOUT_SAME:
        walpe_output_list_free (&layout_pending);      /* it was a passing state */
#ifdef WALPE_FOLLOW
        layout_pending_set = FALSE;
#endif
        walpe_output_list_free (&l);
        return G_SOURCE_REMOVE;
    case WALPE_LAYOUT_WAIT:
        walpe_output_list_free (&layout_pending);
        layout_pending = l;                 /* look again: it has to hold */
#ifdef WALPE_FOLLOW
        layout_pending_set = TRUE;
#endif
        layout_timer = g_timeout_add (settled ? 2000 : 1000, layout_check, NULL);
        return G_SOURCE_REMOVE;
    case WALPE_LAYOUT_RESTART:
        break;
    }
    if (!restart_begin ("the monitor layout changed")) {
        /* (the next change of the outputs looks again) */
        walpe_output_list_free (&layout_pending);
#ifdef WALPE_FOLLOW
        layout_pending_set = FALSE;
#endif
        layout_force = FALSE;
        swap_restart = FALSE;
    } else {
        GString *d = g_string_new (NULL);
        for (int i = 0; i < l.n; i++)
            g_string_append_printf (d, "%s%s %dx%d+%d+%d", i ? ", " : "", l.o[i].name,
                                    l.o[i].width, l.o[i].height, l.o[i].x, l.o[i].y);
        g_message ("monitor layout changed (now: %s): restarting to follow it", d->str);
        g_string_free (d, TRUE);
        D.restart = TRUE;
        g_main_loop_quit (D.loop);
    }
    walpe_output_list_free (&l);
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

/* a surface was taken away by the window system (plat_surface_closed) */
static void layout_surface_lost (void)
{
    layout_force = TRUE;
    layout_schedule ();
}

/* main, from a render thread whose swaps fail and say that its GL context
 * or surface is gone (mon_swap_failed): the one line about it, and the
 * restart in place that makes them anew - unless the daemon restarted for
 * this a moment ago (then the restart did not cure it and another one would
 * not either), or runs in a window (nothing follows a surface there). */
static gboolean swap_lost_cb (gpointer user)
{
    Mon *m = user;
    const EGLint e = (EGLint) g_atomic_int_get (&m->swap_lost);
    if (!e)
        return G_SOURCE_REMOVE;     /* (it presents again) */
    const gint64 now = g_get_monotonic_time ();
    const gboolean again = swap_restarted_us > 0 && now >= swap_restarted_us &&
                           now - swap_restarted_us < (gint64) SWAP_RESTART_GAP_S * G_USEC_PER_SEC;
    if (swap_restart || D.restart) {
        /* (another monitor got there first, or another restart is on its way) */
    } else if (again) {
        g_warning ("monitor %d (%s): eglSwapBuffers fails with %s (0x%x): its GL context or surface "
                   "is gone. walpe-daemon restarted for this %d s ago and does not do so again (at "
                   "most once in %d s): the monitor stays as it is until the daemon is started again "
                   "(walpe-ctl status: swap_error)", m->index, m->name, swap_err_name (e), (guint) e,
                   (int) ((now - swap_restarted_us) / G_USEC_PER_SEC), SWAP_RESTART_GAP_S);
    } else if (!layout_watch) {
        g_warning ("monitor %d (%s): eglSwapBuffers fails with %s (0x%x): its GL context or surface "
                   "is gone (walpe-ctl status: swap_error); start walpe-daemon again", m->index,
                   m->name, swap_err_name (e), (guint) e);
    } else {
        g_warning ("monitor %d (%s): eglSwapBuffers fails with %s (0x%x): its GL context or surface "
                   "is gone; restarting in place to make them again", m->index, m->name,
                   swap_err_name (e), (guint) e);
        swap_restart = TRUE;
        layout_surface_lost ();
    }
    state_touch ();     /* status has "swap_error" now: subscribers */
    return G_SOURCE_REMOVE;
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
    /* The child opens no display backend (it only starts the page engine):
     * it is told the one this daemon draws on, in the variable the daemon
     * itself reads, so that a web module that keeps its pages on another
     * host per window system tests the host it will be given here - in a
     * Wayland session whose compositor has no layer shell the daemon is on
     * X11 through Xwayland while WAYLAND_DISPLAY is still set. Only in the
     * child's environment, never in this process's own. */
    if (D.backend == WALPE_BACKEND_X11 || D.backend == WALPE_BACKEND_WAYLAND)
        envp = g_environ_setenv (envp, "WALPE_BACKEND", walpe_backend_name (D.backend), TRUE);
    {
        /* the test ends with this process (web.c, walpe_web_probe_run) */
        char self[24];
        g_snprintf (self, sizeof self, "%ld", (long) getpid ());
        envp = g_environ_setenv (envp, "WALPE_WEB_PROBE_PARENT", self, TRUE);
    }
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

#ifdef HAVE_WEBHOST_WL
/* WebKit starts every page process in a sandbox of its own and binds the
 * socket of "the" display into it: with the X11 page host the X
 * server's, and as soon as its toolkit is on a Wayland display the file
 * $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY. With the private page host that is
 * not the display the page's window lives on (GTK is given that one by
 * path, and its socket is gone by then) but the session's compositor: a
 * page process that was taken over could connect to it like any program
 * of the session, and on compositors that offer their clipboard, screen
 * capture and input protocols to every client it could use those. A page
 * has no business there.
 * WebKit joins the two names without looking whether the second one is a
 * path by itself, and libwayland (1.15 on) takes a full path in
 * WAYLAND_DISPLAY. So on a Wayland backend the variable is replaced by
 * the full path of the same socket, here, before any thread exists:
 * everything that connects through libwayland (this daemon, the pause
 * providers, a program started from it) reaches the compositor as before,
 * and WebKit finds no file of that name to hand to a page process. A
 * restart in place starts from the environment the daemon was given
 * (restart_exec), so the next image does the same again. Nothing is done
 * for a session with a one-time socket (WAYLAND_SOCKET, no name), and
 * nothing on an X11 backend. scripts/webcheck.py (sandbox) looks into a
 * page process for the result: a WebKit that binds differently shows
 * there. */
static void web_env_wayland_socket (void)
{
    const char *name = g_getenv ("WAYLAND_DISPLAY"), *rt = g_getenv ("XDG_RUNTIME_DIR");
    if (D.backend != WALPE_BACKEND_WAYLAND || !name || !*name || g_path_is_absolute (name) ||
        !rt || !g_path_is_absolute (rt))
        return;
    char *full = g_build_filename (rt, name, NULL);
    g_setenv ("WAYLAND_DISPLAY", full, TRUE);
    g_free (full);
}
#endif

/* The sound of pages. WebKit's GStreamer would keep the level and the mute
 * of a page's sound in the sound server, as the volume and the mute of the
 * stream, where they are stored and handed to other programs' streams
 * (gst/walpesound.c says how). The plugin there keeps both in the page
 * process; GStreamer finds it through GST_PLUGIN_PATH, which WebKit's
 * page processes inherit (and bind into their sandbox), and loads it with
 * the first sound of a page, not before. GStreamer reads
 * GST_PLUGIN_PATH_1_0 instead when that is set. A value the user has is
 * kept in front; an image of this process that restarts itself finds the
 * directory already there. Nothing else in this process reads the
 * variable: the daemon itself does not use GStreamer. */
static void web_env_sound_sink (void)
{
    char *dir = walpe_web_sound_sink_dir ();
    if (!dir)
        return;
    const char *var = g_getenv ("GST_PLUGIN_PATH_1_0") ? "GST_PLUGIN_PATH_1_0" : "GST_PLUGIN_PATH";
    const char *old = g_getenv (var);
    char **have = g_strsplit (old ? old : "", ":", -1);
    if (!g_strv_contains ((const char *const *) have, dir)) {
        char *all = old && *old ? g_strconcat (old, ":", dir, NULL) : g_strdup (dir);
        g_setenv (var, all, TRUE);
        g_free (all);
    }
    g_strfreev (have);
    g_free (dir);
}

/* GStreamer's list of plugins (its "registry") for the page processes: a
 * file of WalPE's own, in a directory that holds nothing else.
 *
 * WebKit's sandbox gives a page process the directory of that file to write
 * in. With nothing set that is not ~/.cache/gstreamer-1.0 but the whole of
 * the user's cache directory (seen in the mount table of the page process,
 * WebKitGTK 2.52.6 and 2.54.2, and in WalPE 0.14.0 as well): every program's
 * cache - a browser's, a compiler's, ~/.cache/walpe with the converted
 * videos and sound tracks this daemon decodes outside any sandbox - can be
 * read and rewritten by a page that has broken into its own process, which
 * is the one thing the sandbox is there to contain. With GST_REGISTRY set
 * the sandbox binds the directory of the named file instead (the same two
 * versions; GST_REGISTRY_1_0 is read by GStreamer but not by that code).
 * It also ends a quarrel over the shared file: GStreamer rewrites it (1 MB)
 * whenever a program with another plugin path than the last one starts,
 * and WalPE's pages have a path of their own (web_env_sound_sink).
 * A value the user set, under either name, is the user's.
 *
 * The variable is set here, at start, before any thread exists: later a
 * setenv would race every getenv of the render, sound and media threads and
 * of the GL driver (web.c says what that did). The directory is not made
 * here (it was, at every start, also for a user who never shows a web
 * wallpaper): the module loader makes it when the web module has been
 * loaded, before WebKit starts its first process (walpe_web_registry_dir) -
 * WebKit's sandbox binds it only if it exists by then. A variable that
 * names a file in a directory that is not there yet costs nothing: nothing
 * but a page process reads it in WalPE (ffmpeg, pactl and steamcmd do not
 * use GStreamer), and a GStreamer outside a sandbox makes the directory of
 * its registry file itself. The file is written by the first page that
 * plays media.
 *
 * The WebKit start-up test (`walpe-daemon --web-probe`) is a child that
 * keeps its parent's environment: there the variable is the daemon's own
 * already, and only the directory is handed to the loader. */
static void web_env_gst_registry (void)
{
    char *dir = g_build_filename (g_get_user_cache_dir (), "walpe", "gstreamer", NULL);
    char *file = g_build_filename (dir, "registry.bin", NULL);
    const char *have = g_getenv ("GST_REGISTRY");
    if (!have && !g_getenv ("GST_REGISTRY_1_0")) {
        g_setenv ("GST_REGISTRY", file, TRUE);
        walpe_web_registry_dir (dir);
    } else if (have && !strcmp (have, file)) {
        walpe_web_registry_dir (dir);   /* set by the daemon this process was started by */
    }
    g_free (file);
    g_free (dir);
}

static void web_env_setup (gboolean want_accel, int fps)
{
#ifdef HAVE_WEBHOST_WL
    web_env_wayland_socket ();
#endif
    web_env_sound_sink ();
    web_env_gst_registry ();
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
    /* The restart ends a running import and a running Workshop download
     * with everything else (main: walpe_import_free, walpe_workshop_free
     * kill the helper and remove what it had written so far). The switch
     * used to do that to a download of several gigabytes without a word;
     * the restart now waits for both, as the one for a new monitor layout
     * does (layout_check). The setting is saved already. */
    if (Imp.active || Ws.active) {
        if (!GPOINTER_TO_INT (user))
            g_message ("web hardware acceleration switched %s: the restart that applies it waits "
                       "for the running %s", D.cfg->web_accel ? "on" : "off",
                       Ws.active ? "Workshop download" : "import");
        g_timeout_add_seconds (1, web_accel_restart, GINT_TO_POINTER (1));
        return G_SOURCE_REMOVE;
    }
    if (D.restart)      /* (the switch was flipped more than once while it waited) */
        return G_SOURCE_REMOVE;
    if (!restart_begin ("web hardware acceleration switched"))
        return G_SOURCE_REMOVE;
    g_message ("web hardware acceleration switched %s: restarting to apply it",
               D.cfg->web_accel ? "on" : "off");
    D.restart = TRUE;
    g_main_loop_quit (D.loop);
    return G_SOURCE_REMOVE;
}
#endif

/* ------------------------------------------- the web module leaves again
 * walpe-web.so brings WebKitGTK, JavaScriptCore and GTK into the daemon and
 * none of them can be unloaded (engine/modules.h). After the last web
 * wallpaper had been replaced they stayed for the rest of the daemon's
 * life: the pages of those libraries that were touched (about 50 MB of the
 * resident size on the machine this was measured on), three threads - one
 * of them WebKit's memory monitor, which wakes every five seconds -, GDK's
 * connection to the display and its events, sixteen descriptors. A daemon
 * that had shown one web wallpaper once paid for it until it was stopped.
 * What gives all of it back is a new process image, and the daemon has
 * one: the restart in place that follows a changed monitor layout
 * (restart_exec: the same process id, the control socket stays open, the
 * saved wallpapers and settings come back).
 *
 * When: at the change of wallpapers that leaves no monitor with a web
 * wallpaper - none shown, none wanted, none being loaded -, in place of the
 * loads of that change: the new image loads what the monitors are set to.
 * Never on a timer. Not
 *  - with the setting off ("web_unload", on by default);
 *  - when the restart before this one, for the same reason, is less than
 *    WEB_UNLOAD_GAP_S ago (wallpapers tried one after the other, a script
 *    that alternates between a page and a scene): the module then stays
 *    until a later change of wallpapers finds the time over;
 *  - when the settings cannot be saved (the new image would read the
 *    wallpapers of before the change);
 *  - while an import, a video conversion or a Workshop download runs (the
 *    restart would end it), or the copy of a scene's video is being made
 *    (sv_converting: it would be started again from nothing);
 *  - when the wallpapers of this change are previous ones opened again
 *    after a failed load (what failed is to stay in the status), or while a
 *    wallpaper is held back by the crash-loop guard (only this image knows);
 *  - in a window (--windowed).
 * It costs what a start costs: every monitor loads its wallpaper again and
 * shows nothing until it has, and should a wallpaper of this change fail to
 * load, the web wallpaper it replaced is not opened again in its place.
 * WALPE_WEB_UNLOADED carries the time of the restart across the exec, as
 * WALPE_LAYOUT_RESTARTED does for the layout's.
 *
 * A restart that was put off - the time between two was not over, something
 * was being converted or downloaded, the setting was off - used to wait for
 * the next change of wallpapers, whatever that change was: a scene replaced
 * by another scene hours later, and every monitor went black for a second.
 * It still does when nothing else comes first (web_unload_due is asked at
 * every change). But while it waits (web_unload_put_off) it is also carried
 * out at the first moment at which what kept it waiting is gone and nobody
 * can see a wallpaper: the displays sleep or the session is locked, and
 * pause_dpms is on (wallpapers_unseen). No other pause is taken for such a
 * moment: `walpe-ctl pause`, the battery, idle, focus and cover rules leave
 * the picture or a part of it on screen, and under a fullscreen window the
 * restart would work beside the application that has the machine. No timer and
 * no polling: web_unload_hidden is called where those states change
 * (apply_pause_state) and where a conversion, import or download ends. The
 * time between two restarts ends without an event: a restart it kept
 * waiting is looked at again at the next of those changes. */
#define WEB_UNLOAD_GAP_S 60

#ifdef HAVE_WEB
static gboolean web_unload_pending;     /* main: the restart was decided, its idle has not run */
static gboolean web_unload_put_off;     /* main: the module is loaded, no monitor has a web
                                         * wallpaper, and web_unload_due did not restart */

static gboolean web_unload_cb (gpointer user)
{
    (void) user;
    web_unload_pending = FALSE;
    if (D.restart)
        return G_SOURCE_REMOVE;     /* another restart got there first: it loads them */
    if (Imp.active || Ws.active || sv_converting ()) {
        /* started by the same request, after the decision (a conversion
         * that followed the one just finished): no restart then, and the
         * wallpapers that were not loaded for it are loaded here */
        apply_wallpapers (NULL, NULL, FALSE, FALSE);
        return G_SOURCE_REMOVE;
    }
    if (!restart_begin ("no web wallpaper is shown any more")) {
        /* the module stays; the wallpapers of this change are loaded here
         * (web_unload_due does not decide for a restart again) */
        apply_wallpapers (NULL, NULL, FALSE, FALSE);
        return G_SOURCE_REMOVE;
    }
    web_unload_restart = TRUE;
    D.restart = TRUE;
    g_main_loop_quit (D.loop);
    return G_SOURCE_REMOVE;
}
#endif

#ifdef HAVE_WEB
/* is the last restart that unloaded the module long enough ago for another
 * one? *since, *gap: seconds since it, and the least time between two */
static gboolean web_unload_gap_over (gint64 *since, gint64 *gap)
{
    const char *ge = g_getenv ("WALPE_WEB_UNLOAD_GAP_S");   /* (tests) */
    const gint64 g = (ge && *ge ? CLAMP (g_ascii_strtoll (ge, NULL, 10), 0, 86400) : WEB_UNLOAD_GAP_S) *
                     G_USEC_PER_SEC;
    const gint64 now = g_get_monotonic_time ();
    *since = web_unloaded_us > 0 ? (now - web_unloaded_us) / G_USEC_PER_SEC : 0;
    *gap = g / G_USEC_PER_SEC;
    return !(web_unloaded_us > 0 && now >= web_unloaded_us && now - web_unloaded_us < g);
}
#endif

/* main, apply_wallpapers: changed = a monitor was given another wallpaper.
 * TRUE: the restart is on its way and the caller loads nothing */
static gboolean web_unload_due (gboolean changed)
{
#ifdef HAVE_WEB
    static gboolean said;
    if (web_unload_pending)
        return TRUE;        /* (a second change before the restart's idle has run) */
    if (!changed || restore_apply || D.windowed || D.restart || !D.loop ||
        restart_failed ||   /* (it was tried: restart_begin) */
        !walpe_web_module (FALSE) ||
        (load_skip && g_hash_table_size (load_skip) > 0))
        return FALSE;
    for (GList *l = D.mons; l; l = l->next) {
        const Mon *m = l->data;
        if (m->want_path && *m->want_path && walpe_detect_type (m->want_path, NULL) == WALPE_TYPE_WEB) {
            web_unload_put_off = FALSE;     /* the module is in use (again) */
            return FALSE;
        }
    }
    /* the module is loaded and no monitor is to show a web wallpaper */
    if (!D.cfg->web_unload || Imp.active || Ws.active || sv_converting ()) {
        web_unload_put_off = TRUE;      /* (web_unload_hidden asks all of it again) */
        return FALSE;
    }
    gint64 since = 0, gap = 0;
    if (!web_unload_gap_over (&since, &gap)) {
        if (!said)
            g_message ("no web wallpaper is shown any more; the web module stays loaded this time "
                       "(the daemon restarted to unload it %d s ago: at most once in %d s)",
                       (int) since, (int) gap);
        said = TRUE;
        web_unload_put_off = TRUE;
        return FALSE;
    }
    /* the new image reads the wallpapers from the saved settings. When
     * those cannot be written (a full disk, a home that is read-only) it
     * would come back with the wallpapers of before this change - the web
     * wallpaper that was just replaced - and the one that was chosen would
     * be gone: no restart then, this image carries the change out, as with
     * the setting off */
    if (!walpe_config_save (D.cfg)) {
        static gboolean said_unsaved;
        if (!said_unsaved)
            g_message ("no web wallpaper is shown any more; the web module stays loaded this time "
                       "(the settings could not be saved, and a restart would bring back the "
                       "wallpapers that are saved)");
        said_unsaved = TRUE;
        return FALSE;
    }
    g_message ("no web wallpaper is shown any more: restarting in place to unload the web module "
               "(WebKitGTK, GTK); `walpe-ctl set web_unload=off` keeps it loaded instead");
    web_unload_pending = TRUE;
    /* after the reply to the request that changed the wallpaper went out */
    g_idle_add (web_unload_cb, NULL);
    return TRUE;
#else
    (void) changed;
    return FALSE;
#endif
}

#ifdef HAVE_WEB
/* Nobody can see a wallpaper: the displays sleep or the session is locked,
 * and the rule that pauses for it is on (want_pause_mon). That is the only
 * state taken for it. A focused fullscreen window hides its monitor too,
 * but a restart there would load every wallpaper and make its GL contexts
 * beside a game that has the machine, and a fullscreen window can be
 * translucent; a monitor that windows cover is paused at 85 % covered. */
static gboolean wallpapers_unseen (void)
{
    return D.cfg->pause_dpms && D.asleep;
}

/* the restart that was put off can be carried out now, unseen */
static gboolean web_unload_hidden_now (void)
{
    gint64 since, gap;
    if (!web_unload_put_off || web_unload_pending || D.restart || !D.loop || D.windowed || restore_apply ||
        restart_failed ||   /* (it was tried: restart_begin) */
        !D.cfg->web_unload || !D.mons || !walpe_web_module (FALSE) || Imp.active || Ws.active ||
        sv_converting () || (load_skip && g_hash_table_size (load_skip) > 0) ||
        !wallpapers_unseen () || !web_unload_gap_over (&since, &gap))
        return FALSE;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        /* (a load that runs or has failed: what the status says about it
         * is this image's to keep) */
        if (m->loading || m->load_error || m->reopen_for || m->restored ||
            g_atomic_int_get (&m->content_type) == WALPE_TYPE_WEB)
            return FALSE;
    }
    return TRUE;
}

static guint web_unload_hidden_id;      /* main: the idle of web_unload_hidden */

static gboolean web_unload_hidden_cb (gpointer user)
{
    (void) user;
    web_unload_hidden_id = 0;
    if (!web_unload_hidden_now ())
        return G_SOURCE_REMOVE;     /* (seen again, or busy again, meanwhile) */
    if (!restart_begin ("no web wallpaper is shown any more"))
        return G_SOURCE_REMOVE;     /* (said once; restart_failed keeps it from being asked again) */
    if (!walpe_config_save (D.cfg)) {
        /* the new image would read other wallpapers than are shown: left
         * to the next change of wallpapers (web_unload_due) */
        web_unload_put_off = FALSE;
        return G_SOURCE_REMOVE;
    }
    g_message ("no web wallpaper is shown and nobody can see a wallpaper now: restarting in place "
               "to unload the web module (WebKitGTK, GTK), which was put off at the change that "
               "replaced the last web wallpaper");
    web_unload_restart = TRUE;
    D.restart = TRUE;
    g_main_loop_quit (D.loop);
    return G_SOURCE_REMOVE;
}
#endif

/* main: a pause state changed, or a conversion, an import or a download
 * ended. One comparison while no restart waits. */
static void web_unload_hidden (void)
{
#ifdef HAVE_WEB
    if (web_unload_put_off && !web_unload_hidden_id && web_unload_hidden_now ())
        web_unload_hidden_id = g_idle_add (web_unload_hidden_cb, NULL);
#endif
}

/* ------------------------------------------------------ display backend
 * Which window system the daemon draws on (common/backend.h has the
 * rules and the wording): --backend, else WALPE_BACKEND, else the saved
 * setting, else automatic - Wayland when WAYLAND_DISPLAY is set and the
 * compositor offers the layer shell, else X11 when DISPLAY is set (not in
 * the session of a compositor without the layer shell: Xwayland). Decided
 * once, before anything connects; the answer is logged and shown by
 * `status`. An image restarted because the display went away goes back
 * to the window system it had. */
static void backend_env_fill (WalpeBackendEnv *e, const char *cli, gboolean windowed)
{
    memset (e, 0, sizeof (*e));
    e->built_x11 = walpe_platform_built (WALPE_BACKEND_X11);
    e->built_wayland = walpe_platform_built (WALPE_BACKEND_WAYLAND);
    e->cli = cli;
    e->env = g_getenv ("WALPE_BACKEND");
    e->cfg = D.cfg ? D.cfg->backend : WALPE_BACKEND_AUTO;
    const char *wd = g_getenv ("WAYLAND_DISPLAY"), *ws = g_getenv ("WAYLAND_SOCKET");
    const char *xd = g_getenv ("DISPLAY");
    e->wayland_display = (wd && *wd) || (ws && *ws);
    e->x_display = xd && *xd;
    e->layer_shell = -1;
    e->windowed = windowed;
}

/* quiet: no log lines (the test children). FALSE: *err says what is
 * missing (g_free). */
static gboolean backend_decide (const char *cli, gboolean windowed, gboolean quiet, char **err)
{
    WalpeBackendEnv e;
    WalpeBackendChoice ch;
    backend_env_fill (&e, Bk.lost ? Bk.lost : cli, windowed);
    if (walpe_backend_wants_probe (&e)) {
        /* automatic, in a Wayland session: can the compositor show a
         * wallpaper at all? A short connection of its own, no EGL */
        WalpePlatformProbe pr;
        walpe_platform_probe_wayland (&pr);
        e.layer_shell = !pr.connected ? -1 : pr.layer_shell ? 1 : 0;
        e.windowed = windowed && pr.xdg_shell;
        e.desk_windows = pr.desk_windows;       /* GNOME Shell with the WalPE extension */
        e.no_shell_hint = pr.no_shell_hint;
        g_free (Bk.display_node);
        Bk.display_node = pr.display_node;
    }
    walpe_backend_choose (&e, &ch);
    if (ch.warn && !quiet)
        g_warning ("%s", ch.warn);
    if (ch.backend == WALPE_BACKEND_AUTO) {
        if (err)
            *err = g_steal_pointer (&ch.why);
        walpe_backend_choice_clear (&ch);
        return FALSE;
    }
    D.backend = ch.backend;
    Bk.source = Bk.lost ? "restart after the display went away" : ch.source;
    g_free (Bk.why);
    Bk.why = g_steal_pointer (&ch.why);
    if (Bk.lost) {
        /* (the backend of the image before was passed in the command
         * line's place: nobody typed --backend) */
        g_free (Bk.why);
        Bk.why = g_strdup_printf ("%s, as before the display went away (restart in place)",
                                  walpe_backend_name (D.backend));
    }
    if (!quiet)
        g_message ("display backend: %s", Bk.why);
    /* X11 asked for by name inside a Wayland session: that X server is
     * Xwayland, and an X window is not a desktop background there - the
     * compositor shows the wallpaper window above the session's windows
     * (Mutter 46, KWin 5.27), which is why the automatic choice never ends
     * here (common/backend.h). Asked for, it is done, and said. Not after
     * a restart in place: the image before has said it. */
    if (!quiet && !Bk.lost && ch.asked == WALPE_BACKEND_X11 && ch.backend == WALPE_BACKEND_X11 &&
        e.wayland_display)
        g_warning ("WAYLAND_DISPLAY is set: if this is a Wayland session, the X11 backend draws "
                   "through Xwayland, where the wallpaper window lies above the other windows "
                   "instead of below them (clicks pass through it). Stop it with `walpe-ctl "
                   "quit`; a compositor with the layer shell is drawn on with backend = "
                   "wayland or auto");
    walpe_backend_choice_clear (&ch);
    return TRUE;
}

/* wayland_reconnect (gpu.conf, WALPE_WAYLAND_RECONNECT): seconds a daemon
 * that lost its display waits for it to come back; 0 (default) = it ends */
static int reconnect_wait_s (void)
{
    const char *s = present_setting ("wayland_reconnect");
    double v = s ? g_ascii_strtod (s, NULL) : 0.0;
    return v >= 1.0 ? (int) MIN (v, 3600.0) : 0;
}

/* The start ends before the control socket is served. An image that was
 * restarted in place holds the socket of the image before: without this
 * its file would stay behind with nobody listening. Only that file is
 * removed: the single-instance lock is not held while such an image waits
 * for its display (wayland_reconnect: up to an hour), and a daemon that
 * was started meanwhile has put its own socket in the same place. Returns
 * main's exit status. */
static int start_failed (void)
{
    if (Bk.ipc_fd >= 0) {
        char *sp = walpe_socket_path ();
        /* (any other restart in place fails within the moment of its
         * start: the file is the handed-over one) */
        gboolean ours = TRUE;
        if (Bk.lost) {
            struct stat st;
            gboolean there = stat (sp, &st) == 0;
            /* (no file when this image began: then none of its own can be left) */
            ours = Bk.ipc_file_known && there && st.st_dev == Bk.ipc_dev &&
                   st.st_ino == Bk.ipc_ino && st.st_ctim.tv_sec == Bk.ipc_ctim.tv_sec &&
                   st.st_ctim.tv_nsec == Bk.ipc_ctim.tv_nsec;
            if (!ours && there)
                g_message ("the control socket %s is no longer this daemon's (another "
                           "walpe-daemon was started meanwhile): left in place", sp);
        }
        walpe_ipc_release (ours ? sp : NULL);
        g_free (sp);
    }
    return 1;
}

#ifdef HAVE_WAYLAND
/* The display is gone and the daemon is to wait for it (wayland_reconnect
 * = wait seconds): does it? Not without end. A compositor that ends the
 * daemon's connection every time it is made had the daemon restart in
 * place every few seconds for as long as it ran, and a compositor that
 * dies of something the daemon sends dies once per comeback
 * (kwin_wayland_wrapper gives the session up after about ten). The rule
 * and its two numbers: platform/wayland_logic.h (wl_lost_decide) - the
 * fifth stay in a row of less than 30 seconds ends the daemon, and so
 * does a protocol error at once. Returns wait, or 0 with the reason said:
 * the daemon then ends as it does with wayland_reconnect = 0.
 * WALPE_WL_SHORT_STAY_MS (tests): what counts as a short stay; 0 =
 * nothing does. */
static int lost_decide (int wait)
{
    gint64 short_us = (gint64) WL_RECONNECT_SHORT_S * G_USEC_PER_SEC, stay = -1;
    const char *e = g_getenv ("WALPE_WL_SHORT_STAY_MS");
    if (e && *e)
        short_us = CLAMP (g_ascii_strtoll (e, NULL, 10), 0, 3600 * 1000) * 1000;
    WlLostAct act = wl_lost_decide (&Bk.stays, wait, walpe_platform_display_refused (),
                                    g_get_monotonic_time (), short_us, &stay);
    stay = MAX (stay, 0);
    switch (act) {
    case WL_LOST_WAIT:
        if (Bk.stays.shorts)
            g_message ("the display was there for %.1f s: short stay %d in a row (at %d the "
                       "daemon ends instead of waiting again)", stay / 1e6, Bk.stays.shorts,
                       WL_RECONNECT_SHORT_MAX);
        return wait;
    case WL_LOST_END_REFUSED:
        g_warning ("the compositor closed the display connection with a protocol error: it "
                   "objected to a request of this daemon and would again, so the daemon does not "
                   "come back to it (wayland_reconnect = %d does not apply). Please report the "
                   "line above this one with the compositor's name and version", wait);
        return 0;
    case WL_LOST_END_SHORT:
        g_warning ("the display went away %d times in a row less than %.0f s after the daemon "
                   "had connected to it (this time after %.1f s): the compositor does not keep "
                   "this daemon's connection, and the daemon does not come back to it once "
                   "more (wayland_reconnect = %d gives up). Start walpe-daemon again when the "
                   "session is steady", Bk.stays.shorts, short_us / 1e6, stay / 1e6, wait);
        return 0;
    default:
        return 0;
    }
}

/* The compositor went away again while the daemon was starting on it: one
 * that came back and crashed once more, kwin_wayland replaced twice in a
 * row. A start-up step that needs the display has just failed (EGL could
 * not be initialised) and the daemon is about to end. Where it is to wait
 * for its display (wayland_reconnect) it waits again instead, the whole
 * time anew, as it does when the display goes while it runs
 * (plat_connection_lost; the main loop that would report that is not
 * running yet). Returns when this is not the case, when the daemon was
 * told to stop meanwhile, or when the exec failed: the start then fails
 * the way it did. */
static void start_display_gone (gboolean windowed)
{
    int wait = windowed ? 0 : reconnect_wait_s ();
    if (wait <= 0 || !walpe_platform_display_gone ())
        return;
    /* (not a fifth time in a row, and not after a protocol error) */
    if (lost_decide (wait) <= 0)
        return;
    g_message ("the display connection went away during the start: restarting in place, waiting "
               "up to %d s for it to come back (wayland_reconnect)", wait);
    D.display_lost = TRUE;
    if (term_pending ()) {
        g_message ("signal %d during the start: not restarting", atomic_load (&term_signal));
        return;
    }
    diag_end_line (TRUE);
    restart_exec (TRUE);
}

/* An image that waits for its display to come back holds the control
 * socket of the image before it and has no main loop yet. A signal that
 * ended it there (the session closes, systemctl --user stop) left the
 * socket's file behind with nobody listening. Until the daemon's own
 * handlers are in place (term_signals_init) the three signals remove the
 * file first - when it is still this daemon's, as in start_failed - and
 * then end the process the way they would have. stat, unlink, signal and
 * raise may all be called from a signal handler. */
static const char *wait_sock_path;

static void wait_term_handler (int sig)
{
    int e = errno;
    struct stat st;
    if (wait_sock_path && stat (wait_sock_path, &st) == 0 && st.st_dev == Bk.ipc_dev &&
        st.st_ino == Bk.ipc_ino && st.st_ctim.tv_sec == Bk.ipc_ctim.tv_sec &&
        st.st_ctim.tv_nsec == Bk.ipc_ctim.tv_nsec)
        unlink (wait_sock_path);
    errno = e;
    signal (sig, SIG_DFL);
    raise (sig);    /* (delivered when this handler returns) */
}

static void wait_signals_init (void)
{
    static const int sigs[] = { SIGTERM, SIGINT, SIGHUP };
    if (!Bk.lost || Bk.ipc_fd < 0 || !Bk.ipc_file_known)
        return;
    wait_sock_path = walpe_socket_path ();  /* (kept: the handler reads it) */
    for (guint i = 0; i < G_N_ELEMENTS (sigs); i++) {
        struct sigaction old, sa;
        /* (a signal the daemon was started to ignore stays ignored) */
        if (sigaction (sigs[i], NULL, &old) != 0 || old.sa_handler != SIG_DFL)
            continue;
        memset (&sa, 0, sizeof sa);
        sa.sa_handler = wait_term_handler;
        sigemptyset (&sa.sa_mask);
        sigaction (sigs[i], &sa, NULL);
    }
}
#endif

/* For pause-signal providers that know outputs by their connector name
 * (a provider on a connection of its own): the id per-output signals are
 * reported under, 0 = no monitor of this daemon. name = NULL: the provider
 * was told no name (a compositor whose wl_output is older than version 4)
 * and means "the output": the one this layout has when it has exactly one
 * and a wallpaper of this daemon is on it; with several outputs nobody can
 * tell which one is meant, and the answer is 0. */
G_GNUC_UNUSED static guint32 sig_output_lookup (const char *name, gpointer user)
{
    (void) user;
    if (!name) {
        guint32 only = walpe_output_list_only_id (&layout_now);
        for (GList *l = D.mons; only && l; l = l->next) {
            Mon *m = l->data;
            if (m->out_id == only && mon_output (m))    /* (not the --windowed window) */
                return only;
        }
        return 0;
    }
    /* the monitor spelled like it before one that only shares its
     * canonical form: "DVI-D-1" and "DVI-I-1" (or "HDMI-A-1" and
     * "HDMI-B-1") of one machine are two monitors, and a window on the
     * second was reported for the first */
    guint n = g_list_length (D.mons), k = 0;
    const char **names = g_new0 (const char *, n + 1);
    guint32 *ids = g_new0 (guint32, n + 1);
    for (GList *l = D.mons; l; l = l->next, k++) {
        Mon *m = l->data;
        names[k] = m->name;
        ids[k] = m->out_id;
    }
    int at = walpe_output_name_find (names, (int) n, name);
    guint32 id = at >= 0 ? ids[at] : 0;
    g_free (names);
    g_free (ids);
    return id;
}

/* for the modules (walpe-web.so): WALPE_BACKEND_X11 / WALPE_BACKEND_WAYLAND,
 * 0 before the choice was made */
WALPE_API int walpe_backend_in_use (void);
int walpe_backend_in_use (void)
{
    return D.backend;
}

int main (int argc, char **argv)
{
    gboolean monitor_all = TRUE;
    int monitor_idx = 0;
    gboolean windowed = FALSE;
    const char *cli_backend = NULL;     /* --backend NAME */
#ifdef HAVE_WAYLAND
    gint64 waited_t0 = 0;   /* an image that waits for its display opened it early: since when */
    /* GNOME Shell 46 to 48 started this process for a daemon that asked
     * its extension for a connection to the compositor (platform/
     * wl_gnome.h): hand that connection on and wait. Nothing of the daemon
     * is set up in such a process. */
    if (argc == 3 && !strcmp (argv[1], WL_GNOME_DONOR_OPTION))
        return wl_gnome_donor_run (getenv ("WAYLAND_SOCKET"), argv[2]);
#endif
    g_mutex_init (&Sw.lock);
    {
        /* restarted in place: the control socket of the previous image.
         * Before any thread exists (unsetenv) and before orig_env is taken */
        const char *lf = g_getenv ("WALPE_IPC_FD");
        if (lf && *lf) {
            Bk.ipc_fd = atoi (lf);
            walpe_ipc_adopt_fd (Bk.ipc_fd);
        }
        g_unsetenv ("WALPE_IPC_FD");
        /* ... and, when that image lost its display and this one is to
         * wait for it: the window system it ran on (restart_exec) */
        const char *dl = g_getenv ("WALPE_DISPLAY_LOST");
        if (dl && *dl)
            Bk.lost = g_strdup (dl);
        g_unsetenv ("WALPE_DISPLAY_LOST");
#ifdef HAVE_WAYLAND
        /* ... with how long the display has been there, or how many times
         * in a row it was not for long (restart_exec, lost_decide) */
        wl_stays_parse (g_getenv ("WALPE_DISPLAY_STAYS"), g_get_monotonic_time (), &Bk.stays);
        g_unsetenv ("WALPE_DISPLAY_STAYS");
#endif
        /* ... and when an image before this one restarted to unload the
         * web module (web_unload_due; it stays in the environment the next
         * restart hands on) */
        const char *wu = g_getenv ("WALPE_WEB_UNLOADED");
        web_unloaded_us = wu && *wu ? g_ascii_strtoll (wu, NULL, 10) : 0;
        /* ... or because swaps failed for good (swap_lost_cb; likewise) */
        const char *sr = g_getenv ("WALPE_SWAP_RESTARTED");
        swap_restarted_us = sr && *sr ? g_ascii_strtoll (sr, NULL, 10) : 0;
        if (Bk.lost && Bk.ipc_fd >= 0) {
            /* this image may wait a long time for its display: which file
             * the handed-over socket is now, for a start that fails later */
            char *sp = walpe_socket_path ();
            struct stat st;
            if (stat (sp, &st) == 0 && S_ISSOCK (st.st_mode)) {
                Bk.ipc_file_known = TRUE;
                Bk.ipc_dev = st.st_dev;
                Bk.ipc_ino = st.st_ino;
                Bk.ipc_ctim = st.st_ctim;
            }
            g_free (sp);
        }
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
            int rc = sw_probe_run (argc, argv);
            fflush (NULL);
            _exit (rc);
        }
    for (int i = 1; i < argc; i++)
        if (!g_strcmp0 (argv[i], "--gpu-probe")) {
            /* the test of a GPU choice, run by gpu_setup() in a child */
            int rc = gpu_probe_run (argc, argv);
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
        else if (!g_strcmp0 (argv[i], "--backend") && i + 1 < argc)
            cli_backend = argv[++i];
        else if (!g_strcmp0 (argv[i], "--version")) {
            /* (it was no option: `walpe-daemon --version` started a daemon) */
            g_print ("walpe-daemon %s \"%s\" (source %s)\n", WALPE_VERSION, WALPE_CODENAME, walpe_source_id ());
            return 0;
        } else if (!g_strcmp0 (argv[i], "-h") || !g_strcmp0 (argv[i], "--help")) {
            g_print ("usage: walpe-daemon [--windowed] [--monitor N] [--fps N] "
                     "[--backend auto|x11|wayland]\n"
                     "  --version: the version, and the commit of the source it was built from\n"
                     "  --backend / WALPE_BACKEND / the saved setting (walpe-ctl set backend=...):\n"
                     "  the window system to draw on; auto = Wayland where the compositor has\n"
                     "  the layer shell, X11 on an X session (never Xwayland by itself)\n"
                     "  WALPE_SOFTWARE=1|0 in the environment: render on the CPU (Mesa llvmpipe)\n"
                     "  or on the GPU, whatever the saved setting says "
                     "(walpe-ctl set software=on|off)\n"
                     "  WALPE_GL_API=auto|gl|gles in the environment: desktop OpenGL 3.3, or\n"
                     "  OpenGL ES 3.2 / 3.1, whatever the saved setting says "
                     "(walpe-ctl set gl_api=...)\n");
            return 0;
        }
    }

    daemon_start_us = g_get_monotonic_time ();     /* walpe-ctl mem: "uptime_s" */
    log_file_setup ();                             /* a log file: appended to, capped from the start */
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
    walpe_video_set_hwdec_name (walpe_config_hwdec_word (D.cfg));
    walpe_video_set_zerocopy (D.cfg->video_zerocopy);
    walpe_video_set_limit (D.cfg->video_limit);
    walpe_scene_video_set_on (D.cfg->video_scene_convert);
    walpe_scene_video_set_hook (sv_hook);   /* before any render thread loads a scene */
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
    if (D.cfg->sound_migrated) {
        /* config.h walpe_config_migrate_sound: "off" was the default up to
         * 0.14.0, written into every config.json, and only walpe-ctl could
         * switch it on. Saved now, so it happens once and a choice made
         * from now on is kept. */
        char vol[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd (vol, sizeof vol, "%.2f", D.cfg->volume);
        g_message ("config: wallpaper sound was off%s, the default of versions up to 0.14.0, not a choice: "
                   "it is on now, at volume %s%s (the GUI's Engine settings, \"Wallpaper sound\", or "
                   "walpe-ctl set scene_sound=off, switches it off; that is kept)",
                   D.cfg->sound_migrated == WALPE_SOUND_MIGRATE_DEFAULTS ? " at volume 0" : "", vol,
                   D.cfg->sound_migrated == WALPE_SOUND_MIGRATE_ON ? ", the volume that was set" : "");
        walpe_config_save (D.cfg);
    } else if (D.cfg->sound_unswitched && D.cfg->scene_sound) {
        /* config.h walpe_config_sound_unswitched: the file of a version up
         * to 0.14.0 that has no switch at all and a volume above 0. Nothing
         * is migrated - sound is on by default - and so nothing said why a
         * wallpaper is heard at the first start of this version. Only said:
         * nothing is changed and nothing is saved here, so the line is in
         * the log of every start until the file is saved by this version
         * (the first change of a setting or of the wallpaper). */
        char vol[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd (vol, sizeof vol, "%.2f", D.cfg->volume);
        g_message ("config: this file of a version up to 0.14.0 has no switch for wallpaper sound: it is on, the "
                   "default of this version, at volume %s%s (the GUI's Engine settings, \"Wallpaper sound\", or "
                   "walpe-ctl set scene_sound=off, switches it off; that is kept)", vol,
                   D.cfg->sound_unswitched == WALPE_SOUND_UNSWITCHED_VOLUME ? ", the volume the file has"
                                                                            : ", this version's default too");
    }
    present_settings_load ();   /* gpu.conf / WALPE_*: before any thread, before EGL */
    D.no_refresh_cap = g_getenv ("WALPE_NO_REFRESH_CAP") != NULL;
    D.leader_first = !g_strcmp0 (g_getenv ("WALPE_LEADER"), "first");
    D.fake_vsync = g_getenv ("WALPE_FAKE_VSYNC") != NULL;
    {
        const char *ld = g_getenv ("WALPE_LOAD_DELAY_MS");
        D.load_delay_us = ld && *ld ? (gulong) CLAMP (g_ascii_strtoll (ld, NULL, 10), 0, 60000) * 1000 : 0;
        /* WALPE_SWAP_FAIL=N[,always][,0xERROR] (tests): mon_swap_failed () */
        const char *sf = g_getenv ("WALPE_SWAP_FAIL");
        if (sf && *sf) {
            char **p = g_strsplit (sf, ",", -1);
            gboolean always = FALSE;
            D.swap_fail_err = EGL_CONTEXT_LOST;
            for (int i = 1; p[i]; i++) {
                if (!strcmp (p[i], "always"))
                    always = TRUE;
                else if (g_str_has_prefix (p[i], "0x"))
                    D.swap_fail_err = (EGLint) g_ascii_strtoll (p[i], NULL, 16);
            }
            if (always || swap_restarted_us <= 0)
                D.swap_fail_at = (guint) CLAMP (g_ascii_strtoll (p[0], NULL, 10), 0, 1000000);
            g_strfreev (p);
        }
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
        if (!web_probe) {
            /* which window system: decided here, before the software
             * rendering test (its child draws on the same one) and before
             * anything connects to a display */
            char *berr = NULL;
            if (!backend_decide (cli_backend, windowed, FALSE, &berr)) {
                g_printerr ("walpe-daemon: %s\n", berr ? berr : "no display to draw on");
                return start_failed ();
            }
#ifdef HAVE_WAYLAND
            /* An image that waits for its display to come back
             * (wayland_reconnect) waits here, before the questions that
             * need a display to be answered: the test of the software
             * renderer just below draws on it, the test of hardware
             * acceleration for pages too. They were asked while the
             * compositor was still away: "software rendering is switched
             * on but not available: cannot connect to the Wayland
             * compositor ... Rendering on the GPU", and a daemon told to
             * render on the CPU was on the GPU driver from the first
             * compositor that came back, until it was started anew. So
             * this image opens its display now, the way it is opened
             * further down for every other start; nothing below minds:
             * the backend makes no EGL call and no thread when it opens. */
            if (Bk.lost && D.backend == WALPE_BACKEND_WAYLAND) {
                char *xs = walpe_socket_path (), *xm = loadmark_file (), *perr = NULL;
                WalpePlatformOpts po = {
                    .cfg = D.cfg,
                    .windowed = windowed,
                    .reconnect_wait_s = reconnect_wait_s (),
                    .crash_marker_path = xm,
                    .socket_path = xs,
                    .events = &plat_events,
                };
                waited_t0 = g_get_monotonic_time ();
                wait_signals_init ();   /* a signal while this image waits for its display */
                gboolean opened = walpe_platform_open (D.backend, &po, &perr);
                g_free (xs);
                g_free (xm);
                if (!opened) {
                    g_printerr ("%s\n", perr ? perr : "cannot open the display");
                    return start_failed ();
                }
            }
#endif
            sw_setup ();
            /* GPU choice: hand-over point for the window-system layer
             * (call 1 of 2, gpu_glue.h). After the software decision
             * (which wins), before the first EGL call: D.backend is known,
             * nothing has connected to it yet, no thread exists; the
             * outputs are not known yet here; Bk.display_node names the
             * compositor's device when a Wayland compositor told it */
            gpu_setup (D.backend == WALPE_BACKEND_WAYLAND ? WALPE_GPU_WS_WAYLAND : WALPE_GPU_WS_X11,
                       NULL, Bk.display_node);
        }
    }

#ifdef HAVE_WEB
    /* `walpe-daemon --web-probe`: the WebKit start-up test run in a child
     * (by web.c before WebKit is started inside the daemon, by
     * web_env_setup to try hardware acceleration). It keeps the
     * environment its parent gave it. */
    for (int i = 1; i < argc; i++)
        if (!g_strcmp0 (argv[i], "--web-probe")) {
            /* (no thread yet. The daemon's GST_REGISTRY is in the environment
             * already; this hands its directory to the loader, which makes
             * it before the test's page process is started - the daemon
             * itself has not made it if no web wallpaper was loaded yet) */
            web_env_gst_registry ();
            int rc = walpe_web_probe_run ();
            fflush (NULL);
            _exit (rc);
        }
    /* before any thread exists: software or hardware rendering of pages */
    web_env_setup (D.cfg->web_accel, D.cfg->fps);
#endif
    /* a peer that closes a socket/pipe mid-write must not kill the daemon */
    signal (SIGPIPE, SIG_IGN);
    {
        /* The display: its connections and error handling, and what the
         * window system offers (platform/). Should the connection die
         * under the process (X11: the server went away), the control
         * socket and the crash-loop marker are removed first. After a
         * software rendering test the X server may refuse connections for
         * a moment; an image restarted because the display went away
         * waits for it to come back. */
        char *xs = walpe_socket_path (), *xm = loadmark_file (), *perr = NULL;
        WalpePlatformOpts po = {
            .cfg = D.cfg,
            .windowed = windowed,
            .wait_for_display = Sw.wanted,
            .reconnect_wait_s = Bk.lost ? reconnect_wait_s () : 0,
            .crash_marker_path = xm,
            .socket_path = xs,
            .events = &plat_events,
        };
        gint64 open_t0 = Bk.lost ? g_get_monotonic_time () : 0;
#ifdef HAVE_WAYLAND
        /* (an image that waited for its display has it open already: above) */
        if (waited_t0)
            open_t0 = waited_t0;
        else
            wait_signals_init ();   /* a signal while this image waits for its display */
        gboolean opened = waited_t0 || walpe_platform_open (D.backend, &po, &perr);
#else
        gboolean opened = walpe_platform_open (D.backend, &po, &perr);
#endif
        g_free (xs);
        g_free (xm);
        if (!opened) {
            g_printerr ("%s\n", perr ? perr : "cannot open the display");
            return start_failed ();
        }
#ifdef HAVE_WAYLAND
        /* the display is there from now (lost_decide), unless an image
         * before this one, restarted for another reason, knew it longer */
        if (D.backend == WALPE_BACKEND_WAYLAND && !Bk.stays.since_us)
            Bk.stays.since_us = g_get_monotonic_time ();
#endif
        /* An image that waited for its display to come back did not answer
         * the control socket in that time: walpe-ctl gives up after two
         * seconds and says that the daemon did not answer in time. What was
         * sent then (a quit, another wallpaper) is not carried out now. */
        if (Bk.lost && Bk.ipc_fd >= 0 &&
            g_get_monotonic_time () - open_t0 >= 2 * G_USEC_PER_SEC) {
            int dropped = walpe_ipc_drop_queued ();
            if (dropped)
                g_message ("%d request(s) sent while this daemon waited for its display were "
                           "not answered and are not carried out", dropped);
        }
    }
    D.loop = g_main_loop_new (NULL, FALSE);
    /* systemctl --user stop / Ctrl+C: the same clean shutdown as `quit`
     * (kills a running ffmpeg import / steamcmd and their partial output,
     * removes the socket) instead of dying with them orphaned. Installed
     * before the socket and the first load exist: a signal during start-up
     * is acted on as soon as the main loop runs, instead of killing the
     * process with the socket and the crash-loop marker left behind. */
    /* (SIGHUP: the terminal the daemon was started from was closed) */
    term_signals_init ();
    {
        char *desc = walpe_platform_describe ();
        g_message ("walpe-daemon %s \"%s\" up (%s, backend=%s, source %s)", WALPE_VERSION, WALPE_CODENAME,
                   desc, WALPE_VIDEO_BACKEND, walpe_source_id ());
        g_free (desc);
    }

    /* monitor layout: rectangles, output names, refresh rates */
    walpe_platform_outputs (&layout_now);
    int nmon = layout_now.n;
    for (int i = 0; i < nmon; i++) {
        const WalpeOutput *o = &layout_now.o[i];
        if (o->refresh_hz > 0.0)
            g_message ("monitor %d: %s %dx%d+%d+%d, %.2f Hz", i, o->name, o->width,
                       o->height, o->x, o->y, o->refresh_hz);
        else
            g_message ("monitor %d: %s %dx%d+%d+%d, refresh rate unknown", i, o->name,
                       o->width, o->height, o->x, o->y);
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
#ifdef HAVE_WAYLAND
        start_display_gone (windowed);  /* (returns when that is not why) */
#endif
        g_printerr ("EGL init failed; cannot render\n");
        return 1;
    }

#ifdef WALPE_FOLLOW
    layout_only = monitor_all ? -1 : monitor_idx;
#endif
    if (windowed) {
        WalpeRect g = { .width = 960, .height = 540 };
        make_monitor (NULL, &g, 0, FALSE, "windowed"); /* normal window: WM tiles it, on top */
    } else {
        for (int i = 0; i < nmon; i++)
            if (monitor_all || i == monitor_idx) {
                const WalpeOutput *o = &layout_now.o[i];
                WalpeRect g = { o->x, o->y, o->width, o->height };
                Mon *nm = make_monitor (o, &g, i, TRUE, o->name);
                if (nm) {
                    nm->hz = o->refresh_hz;
                    D.max_hz = MAX (D.max_hz, o->refresh_hz);
                } else {
#ifdef WALPE_FOLLOW
                    if (layout_retries ()) {
                        /* tried again later: a render thread waits for it */
                        if (!nosurf)
                            nosurf = g_ptr_array_new_with_free_func (g_free);
                        g_ptr_array_add (nosurf, g_strdup (o->name));
                        mon_make_waiting ();
                        g_warning ("monitor %d (%s): NO WALLPAPER SURFACE could be made on it (the "
                                   "line above says why): nothing is shown there. walpe-daemon tries "
                                   "again in %g s, %d times in all (each try twice as long after "
                                   "the one before, a minute at most), and whenever an output "
                                   "changes", i, o->name, nosurf_delay_ms (0) / 1000.0, NOSURF_TRIES);
                    } else
#endif
                    diag_mon_failed (i, o->name);   /* said, and kept for status */
                }
            }
        diag_mons_checked (nmon);
#ifdef WALPE_FOLLOW
        if (nosurf)
            nosurf_update (FALSE);  /* what status says of them, and the first try */
#endif
    }

    {
        /* render resolution (common/resolution.h), before any thread and
         * any load. A config.json written by a version that had no choice
         * per output: the outputs connected now are entered as "asked,
         * native" - the user of that version is not asked about the
         * screens that were there, and they keep their picture. Then every
         * monitor's render size from its choice; an output without one
         * runs at native and stays "not asked yet" until the GUI or
         * walpe-ctl says otherwise (nothing here waits for an answer). */
        res_no_ask = windowed;
        const char *re = g_getenv ("WALPE_RESOLUTION");
        if (re && *re) {
            char *rerr = NULL;
            res_env = walpe_res_assign_parse (re, &rerr);
            for (guint i = 0; res_env && !rerr && i < res_env->len; i++) {
                WalpeRes r;
                walpe_res_parse (((WalpeResAssign *) g_ptr_array_index (res_env, i))->value, &r,
                                 &rerr);
            }
            if (rerr) {
                g_warning ("WALPE_RESOLUTION is not used: %s", rerr);
                g_clear_pointer (&res_env, g_ptr_array_unref);
                g_free (rerr);
            } else {
                g_message ("WALPE_RESOLUTION decides the render sizes of this run; the saved "
                           "choices are not changed");
            }
        }
        if (!windowed) {
            /* the names of ALL detected outputs (also with --monitor N), as
             * the output list of the platform layer spells them */
            const char **names = g_new0 (const char *, (gsize) nmon + 1);
            for (int i = 0; i < nmon; i++)
                names[i] = layout_now.o[i].name;
            /* on Wayland every entry an earlier version stored carries an
             * X11 name: an output without one of its own uses the entry of
             * the same connector under its other spelling */
            walpe_config_set_outputs (D.cfg, names, (guint) nmon,
                                      D.backend == WALPE_BACKEND_WAYLAND);
            if (walpe_config_monitors_migrate (D.cfg, names, (guint) nmon)) {
                g_message ("config: written before a render resolution could be chosen per "
                           "monitor: the %d connected monitor(s) stay at their own resolution and "
                           "count as asked (walpe-ctl resolution, or the settings window, to "
                           "choose another)", nmon);
                walpe_config_save (D.cfg);
            }
            g_free (names);
        }
        res_refresh ();
        const int un = res_unasked ();
        if (un)
            g_message ("resolution: %d monitor(s) not asked about yet: rendered at their own "
                       "resolution (walpe-ctl resolution lists them; the window asks once)", un);
    }
    present_init ();   /* swap interval per monitor, before its thread starts */
    /* one render thread per monitor: owns its EGL context for life */
    ctx_expected = (int) g_list_length (D.mons);
#ifdef WALPE_FOLLOW
    /* (and one per output that got no surface: mon_make_waiting) */
    ctx_expected += (int) g_list_length (mons_parked);
    for (GList *l = mons_parked; l; l = l->next)
        ((Mon *) l->data)->thread = g_thread_new ("walpe-mon-w", mon_thread, l->data);
#endif
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
    if (D.cfg->features) {
        /* renderer feature switches of config.json (WALPE_FEATURES wins; a
         * name this build has no feature for stays in the file, unused) */
        GHashTableIter fit;
        gpointer fk, fv;
        g_hash_table_iter_init (&fit, D.cfg->features);
        while (g_hash_table_iter_next (&fit, &fk, &fv))
            walpe_feature_set (fk, GPOINTER_TO_INT (fv) == 1);
    }
    fx_set_specialize (D.cfg->fx_specialize);       /* effect pass programs (fx_specialize.h) */
    fx_set_shader_quality (D.cfg->fx_quality);
    fx_sim_set_enabled (D.cfg->fluid_sim);          /* simulation effects (fx_sim.h) */
    fx_sim_set_quality (walpe_fluid_quality_name ((WalpeFluidQuality) D.cfg->fluid_quality));
    particles_apply ();
    web_sound_apply ();     /* (web pages; the scenes' sound follows) */

    /* wallpaper sound: only this program may play (the settings window,
     * the tools and the tests never do); what is heard follows the three
     * settings (scene_sound is on unless the user switched it off; a
     * wallpaper without sound layers makes no player, thread or stream) */
    walpe_sound_allow (TRUE);
    walpe_sound_set_master (D.cfg->volume);
    walpe_sound_set_muted (D.cfg->mute);
    walpe_sound_set_enabled (D.cfg->scene_sound);
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

    /* From here on the window system's events arrive: output changes
     * (not in --windowed), what the pause signals need, input. The
     * backend takes its first look at the windows before this returns
     * (X11: the focused window, then what covers the monitors). */
    layout_watch = !windowed;
    /* pause providers: hand-over point (further providers register here,
     * before the backend starts the registry; sig_output_lookup names an
     * output for them) */
    walpe_sigprov_set_idle_timeout ((guint) MAX (D.cfg->pause_idle, 0));
    walpe_sigprov_set_see_through (D.cfg->transparent_windows);
    walpe_sigprov_set_output_lookup (sig_output_lookup, NULL);
    sig_window_rules ();
    walpe_sigprov_register (D.backend);
    walpe_platform_start (!windowed, &signal_sink, NULL);
    /* The layout this daemon runs with was read at the top of start-up
     * (layout_detect), the events that announce a change were asked for
     * only just now (walpe_platform_start) - after EGL, the windows, the
     * render threads and the first loads. A change in between reached
     * nobody: an xrandr call of
     * the session's start-up running next to the daemon's, a monitor that
     * came back while the daemon was restarting for its going (the events
     * of that time went to the old image, which no longer read them). The
     * daemon then kept the layout of that moment - a wallpaper window of
     * the old size, none on a new monitor - until some later event. So one
     * look now, through the usual check (seen twice, restarts apart). */
    layout_schedule ();
    D.windowed = windowed;
    vis_apply_setting ();          /* "Skip hidden areas": the tracker, when switched on */
    /* No periodic timer runs from here on (there were four: pointer
     * recheck 1 s, display sleep 1 s, cover re-scan 2 s, re-lower 5 s). An
     * idle daemon - static wallpaper, paused, covered, display off - sleeps
     * in poll() until a window-system event, an IPC request or a signal
     * arrives. */
    walpe_platform_settle ();      /* one look at the stacking order now */
    pointer_update ();             /* tracks once content needs it */
    if (g_getenv ("G_MESSAGES_DEBUG"))   /* debug-only output */
        g_timeout_add_seconds (2, stats_tick, NULL);
    /* display sleep: its watch is set up now, the state is asked for at
     * the end of the start-up */
    walpe_signals_recheck (WALPE_SIG_ASLEEP, FALSE);
    power_watch_update ();         /* only when a battery rule is set */
    /* the idle rule's time, for whoever reports idle in this session */
    walpe_sigprov_set_idle_timeout ((guint) MAX (D.cfg->pause_idle, 0));
    walpe_signals_recheck (WALPE_SIG_IDLE, TRUE);
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
    walpe_signals_recheck (WALPE_SIG_ASLEEP, TRUE);
    walpe_signals_recheck (WALPE_SIG_IDLE, TRUE);

    if (!D.display_lost)    /* (it can go while the daemon is still starting) */
        g_main_loop_run (D.loop);
    diag_say (NULL);        /* a daemon that never drew: its summary now */
    diag_end_line ((Sw.restart || D.restart) && !atomic_load (&term_signal));
    /* The daemon ends, or starts itself again. A render thread that is in
     * the middle of loading a scene is not waited for until the scene is
     * complete: the load gives up at its next layer or texture (content.h)
     * and the thread frees what it has. (The exit waited for the whole
     * load: seconds for a large scene, with the socket already closed.) */
    walpe_load_stop ();

    /* release the socket BEFORE the (slow) EGL/X teardown: if a new daemon
     * starts during shutdown, our late unlink would otherwise delete its
     * freshly bound socket and leave it deaf */
    const gboolean restarting = (Sw.restart || D.restart) && !atomic_load (&term_signal);
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
    sv_job_clear ();    /* the same for the copy of a scene's video that is being made */
    if (Ws.job)  /* kills steamcmd + removes a partial copy */
        walpe_workshop_free (Ws.job);
    /* two phases: every render thread is stopped and joined before any
     * monitor is freed. One by one, a shared-rendering leader that was
     * still running called request_redraw on a follower that had already
     * been joined and freed (heap-use-after-free when the follower comes
     * first in the list: the faster monitor leads). */
#ifdef WALPE_FOLLOW
    /* the monitors without an output end like the others: their threads
     * wait, or are on their way there (their surfaces are freed below) */
    D.mons = g_list_concat (D.mons, mons_parked);
    mons_parked = NULL;
#endif
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        g_mutex_lock (&m->lock);
        m->quitting = TRUE;
        g_cond_broadcast (&m->cond);
        g_mutex_unlock (&m->lock);
    }
    {
        /* A thread that does not come back from a frame or a load (see
         * load_held_check) is not joined: the daemon waited for it for
         * ever - after `quit` with the socket closed already, a process
         * nobody could reach that kept a CPU busy until it was killed by
         * hand, and before a restart in place with the socket open and
         * nobody answering. The orderly tear-down below needs the threads
         * gone, so it is left out: a restart goes straight to the new
         * image, an exit to the end of the process (which closes the
         * display connection and ends the helper processes with it). */
        Mon *held = mons_wait_end ();
        if (held) {
            char *was = mon_thread_in (held);
            const gboolean again = restarting && !term_pending ();
            g_warning ("monitor %d (%s): its render thread has not come back from a frame or a "
                       "load%s%s within %d s: the daemon %s without waiting for it",
                       held->index, held->name ? held->name : "?", was ? " of " : "",
                       was ? walpe_log_str (was) : "", load_held ? 1 : RENDER_END_WAIT_S,
                       again ? "restarts" : "ends");
            g_free (was);
            walpe_import_settle (400);
            if (again) {
                restart_exec (D.restart);   /* returns only when it failed (socket released) */
            } else if (sock_kept) {
                char *sp = walpe_socket_path ();
                walpe_ipc_release (sp);
                g_free (sp);
            }
            fflush (NULL);
            _exit (again || D.display_lost ? 1 : 0);
        }
    }
    for (GList *l = D.mons; l; l = l->next)
        g_thread_join (((Mon *) l->data)->thread); /* thread frees its content + context */
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        if (m->expose_timer_id)
            g_source_remove (m->expose_timer_id);
        walpe_surface_free (m->ps);   /* its EGL surface, then its window */
        g_mutex_clear (&m->lock);
        g_cond_clear (&m->cond);
        g_cond_clear (&m->mem_cond);
        g_free (m->mem_json);
        g_free (m->name);
        g_free (m->want_path);
        g_free (m->loaded_path);
        g_free (m->load_error);
        g_free (m->res_note);
        walpe_region_clear (&m->vis);   /* "Skip hidden areas" */
        g_free (m->qdescribe);
        g_free (m->good_path);
        g_free (m->reopen_for);
        g_free (m->restored);
        g_free (m->load_path);       /* a load its thread never took */
        g_free (m->given_path);
        g_free (m->busy_path);
        if (m->ptr_q)                /* (LeakSanitizer: one array per monitor that saw the pointer) */
            g_array_unref (m->ptr_q);
        g_free (m);
    }
    walpe_open_undo_clear (&open_undo);
    g_list_free (D.mons);
    eglTerminate (D.egl_dpy);
    walpe_platform_close ();
    /* the encoders that were stopped above have had the time of the tear-down
     * to end: what one of them still created after it was told to stop (a
     * ".part" file in the cache) goes now. Bounded: a child that is still
     * not gone leaves its file to the next start (conv_cache_tidy). */
    walpe_import_settle (400);
    if (restarting) {
        /* "Render on CPU" changed, the monitor layout changed or web
         * hardware acceleration was switched: the same daemon again (same
         * process id), with the saved wallpapers; the socket was kept, the
         * new image adopts it. Unless a signal told the daemon to stop
         * while it was taking itself apart: then it stops. */
        if (term_pending ()) {
            g_message ("signal %d during the restart: not restarting", atomic_load (&term_signal));
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
    /* without its display the daemon did not end by choice (as on X11,
     * where the X library ends the process with status 1) */
    return D.display_lost ? 1 : 0;
}
