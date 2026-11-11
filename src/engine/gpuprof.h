/* gpuprof.h - opt-in GPU profiler of the daemon's render threads.
 *
 *   walpe-ctl profile [seconds]        (IPC "profile" / "profile_result")
 *   WALPE_GPU_PROFILE=<seconds>        one run right after start-up
 *
 * Off by default. While off, every hook below is one relaxed load of
 * walpe_prof_enabled_ and a branch that is never taken; no GL call is made
 * or wrapped and no memory is allocated. While a run is active it measures,
 * for each monitor's render thread (= its GL context):
 *   - GPU time per layer and per effect pass (GL_TIME_ELAPSED timer queries:
 *     desktop GL >= 3.3 / ARB_timer_query, GLES 3 + EXT_disjoint_timer_query;
 *     without them the report falls back to CPU time and says so; "sync"
 *     mode measures with glFinish instead, see walpe_prof_start);
 *   - passes, draw calls, pixels shaded (target area x passes), uploads
 *     (bytes per frame), blits, render-target switches, readbacks;
 *   - frames rendered / presented / skipped as unchanged;
 *   - render-thread time by stage (wall and thread CPU);
 *   - texture and render-target memory by category;
 *   - driver messages of the KHR_debug channel (PERFORMANCE first),
 *     de-duplicated with counts.
 * The report (text + JSON) goes to ~/.cache/walpe/gpu-profile.{txt,json}.
 * It holds layer and effect names and nothing else of the wallpaper.
 *
 * Renderer hooks are the WALPE_PROF_* macros (search for "GPUPROF"): begin /
 * end pairs around existing layer and pass draw sites. Draw calls, uploads,
 * blits and readbacks need no hook at their call sites: while a run is
 * active the profiler puts counting wrappers into libepoxy's dispatch
 * pointers (gpuprof.c "interposer") and takes them out again at the end. */
#pragma once
#include <glib.h>
#include "common/api.h"

G_BEGIN_DECLS

/* 0 = no run active. Read by every hook; written by start/stop only. */
extern int walpe_prof_enabled_;
#define WALPE_PROF_ON() \
    (__builtin_expect (__atomic_load_n (&walpe_prof_enabled_, __ATOMIC_RELAXED) != 0, 0))

/* render-thread stages (CPU time by stage) */
enum {
    WALPE_PROF_ST_RENDER = 0, /* content render without finer stages (video, GIF, image, web) */
    WALPE_PROF_ST_UPDATE,     /* scene: scripts, audio / media, video frames, transforms */
    WALPE_PROF_ST_LAYERS,     /* scene: effect passes and layer draws (GL submission) */
    WALPE_PROF_ST_POST,       /* scene: after the layers (target pool, heap trim) */
    WALPE_PROF_ST_PUBLISH,    /* shared rendering: copy of the frame for the followers */
    WALPE_PROF_ST_SWAP,       /* eglSwapBuffers (waits for vsync / the compositor) */
    WALPE_PROF_ST_PRESENT,    /* follower monitor: blit of the shared frame */
    WALPE_PROF_ST_PROFILER,   /* this profiler reading back its timer queries */
    WALPE_PROF_N_STAGES
};

/* ---- hooks (render thread, its GL context current). Call through the
 * macros: they cost nothing while no run is active. ---------------------- */
void walpe_prof_frame_begin (int monitor, int w, int h);
void walpe_prof_frame_end (gboolean presented);
void walpe_prof_stage (int stage);
/* a wake-up that drew nothing because nothing changed. Inside a frame
 * (between frame_begin and frame_end) that is then not presented, the
 * frame is counted as skipped instead of rendered; outside a frame it
 * only counts. A renderer that learns to skip unchanged frames calls this
 * where it decides to (the daemon already does when gl_render () returns
 * FALSE). */
void walpe_prof_frame_skipped (int monitor);
/* one scene layer: everything between begin and end that is not inside a
 * pass is the layer's own row (its draw into the frame, particles,
 * captures). kind: "image", "text", "particles", "compose", "video" */
void walpe_prof_layer_begin (int index, const char *name, const char *kind);
void walpe_prof_layer_end (void);
/* one effect pass into a target of tw x th. effect = short effect name
 * (walpe_prof_effect_label), inst = its position in the layer's effects,
 * pass = pass number in the effect (-1 = not a numbered pass) */
void walpe_prof_pass_begin (const char *effect, int inst, int pass, int tw, int th);
void walpe_prof_pass_end (void);
/* the pass in progress fills only drawn_px of the full_px (tw x th) its
 * begin counted: a pass restricted to a part of its target */
void walpe_prof_pass_region (guint64 full_px, guint64 drawn_px);
/* a quad drawn into the frame with this 3x3 column-major transform of the
 * +-1 quad: adds its on-screen pixel area to the current row */
void walpe_prof_quad (const float *mvp);

/* GPUPROF hook macros */
#define WALPE_PROF_STAGE(st) \
    do { if (WALPE_PROF_ON ()) walpe_prof_stage (st); } while (0)
#define WALPE_PROF_FRAME_SKIPPED(mon) \
    do { if (WALPE_PROF_ON ()) walpe_prof_frame_skipped (mon); } while (0)
#define WALPE_PROF_LAYER_BEGIN(index, name, kind) \
    do { if (WALPE_PROF_ON ()) walpe_prof_layer_begin ((index), (name), (kind)); } while (0)
#define WALPE_PROF_LAYER_END() \
    do { if (WALPE_PROF_ON ()) walpe_prof_layer_end (); } while (0)
#define WALPE_PROF_PASS_BEGIN(effect, inst, pass, tw, th) \
    do { if (WALPE_PROF_ON ()) walpe_prof_pass_begin ((effect), (inst), (pass), (tw), (th)); } while (0)
#define WALPE_PROF_PASS_END() \
    do { if (WALPE_PROF_ON ()) walpe_prof_pass_end (); } while (0)
#define WALPE_PROF_QUAD(mvp) \
    do { if (WALPE_PROF_ON ()) walpe_prof_quad (mvp); } while (0)

/* "effects/waterripple/effect.json" -> "waterripple" (the effect's folder;
 * a workshop effect keeps its folder name too). Always NUL-terminated. */
void walpe_prof_effect_label (char *out, gsize size, const char *effect_file);

/* ---- render thread <-> profiler ---------------------------------------- */
typedef struct {
    int monitor;             /* index */
    const char *name;        /* output name */
    int w, h;                /* monitor pixels */
    const char *type;        /* content type name */
    const char *path;        /* wallpaper path (only an all-digit folder name, the
                              * workshop id, is kept), NULL = none */
    const char *role;        /* "", "leads", "follows monN" */
    double hz;               /* refresh rate, 0 = unknown */
    gint64 fx_bytes;         /* effect render targets alive in this thread */
    int fx_count;
    guint fx_run, fx_cached; /* effect passes drawn / served from the static cache
                              * (running totals of this thread; deltas are taken) */
} WalpeProfThreadInfo;

/* render thread, between frames, its context current: at the start of a
 * run it attaches the thread (context facts, debug channel), at the end it
 * finishes it (outstanding timer queries, memory snapshot). Cheap no-op
 * otherwise. The daemon calls it when walpe_prof_thread_wanted () or after
 * the waker ran. */
void walpe_prof_thread_sync (const WalpeProfThreadInfo *info);
/* the thread ends (its context is still current) */
void walpe_prof_thread_exit (void);

/* ---- control (main thread, default main context) ------------------------ */
/* wake every render thread so that it calls walpe_prof_thread_sync () */
typedef void (*WalpeProfWaker) (gpointer user);
void walpe_prof_set_waker (WalpeProfWaker wake, gpointer user);
/* start a run of `seconds` (clamped to 1..600). shared = the monitor
 * contexts are one share group (textures are counted once). FALSE + *why
 * (static string) when a run is active already.
 * flags: WALPE_PROF_SYNC = no timer queries: glFinish at every row boundary
 * and the wall time in between as the row's GPU time. Slows rendering
 * (frame rate and totals are not those of a normal run) but attributes
 * exactly on any driver - on software rasterizers such as llvmpipe, whose
 * timer queries do not follow the draw order, it is the mode to use. Also
 * WALPE_GPU_PROFILE_SYNC=1 in the daemon's environment. */
enum { WALPE_PROF_SYNC = 1 };
gboolean walpe_prof_start (double seconds, gboolean shared, guint flags, const char **why);
/* how many render threads the daemon has (a report says when some are missing) */
void walpe_prof_set_expected (int n);
/* main thread: monitor `monitor` is paused / runs again, and why
 * (a short static phrase such as "fullscreen window focused"). The daemon
 * calls it for every monitor right after walpe_prof_start () and whenever a
 * monitor's pause state changes; a no-op while no run is active. The
 * report then says how long each monitor was paused and why. */
void walpe_prof_note_pause (int monitor, gboolean paused, const char *reason);
/* 0 = idle (no run yet), 1 = running, 2 = collecting, 3 = a report is ready */
int walpe_prof_state (void);
/* seconds until the running profile ends (0 when none runs) */
double walpe_prof_remaining (void);
/* the last report: text / JSON (g_free) and where they were written
 * (static storage, NULL before the first report) */
char *walpe_prof_last_text (void);
char *walpe_prof_last_json (void);
const char *walpe_prof_last_text_path (void);
const char *walpe_prof_last_json_path (void);

/* ---- report model + formatting (pure: no GL, unit-tested) --------------- */
typedef struct {
    char layer[64];          /* layer name, or "(frame)" etc. */
    char effect[64];         /* effect name, "" = the layer's own draw */
    char kind[16];
    int layer_index;         /* -1 = not a layer */
    int inst, pass;          /* -1 = n/a */
    int tw, th;              /* target size */
    guint64 gpu_ns, cpu_ns;  /* totals over the run */
    guint64 runs;            /* times the row was entered */
    guint64 draws, pixels, upload_bytes;
} WalpeProfRow;

enum {
    WALPE_PROF_C_PASSES, WALPE_PROF_C_DRAWS, WALPE_PROF_C_PIXELS, WALPE_PROF_C_CLEARS,
    WALPE_PROF_C_BLITS, WALPE_PROF_C_BLIT_PIXELS, WALPE_PROF_C_FBO_BINDS,
    WALPE_PROF_C_TEX_UPLOADS, WALPE_PROF_C_TEX_UPLOAD_BYTES,
    WALPE_PROF_C_BUF_UPLOADS, WALPE_PROF_C_BUF_UPLOAD_BYTES,
    WALPE_PROF_C_READBACKS, WALPE_PROF_C_FINISHES, WALPE_PROF_C_FLUSHES,
    WALPE_PROF_C_MIPGENS, WALPE_PROF_C_TEX_COPIES, WALPE_PROF_C_PROGRAM_LINKS,
    WALPE_PROF_N_COUNTERS
};

enum {
    WALPE_PROF_MEM_TEX_COMPRESSED,   /* textures in a compressed format (DXT / BC) */
    WALPE_PROF_MEM_TEX_PLAIN,        /* other textures that are no render target */
    WALPE_PROF_MEM_FX_TARGETS,       /* effect render targets (pool + owned) */
    WALPE_PROF_MEM_OTHER_TARGETS,    /* other render targets: captures, blend copies, shared frames */
    WALPE_PROF_MEM_BUFFERS,          /* vertex / index buffers */
    WALPE_PROF_MEM_SURFACES,         /* window back + front buffer (estimate) */
    WALPE_PROF_N_MEM
};

typedef struct {
    int monitor;
    char name[32];
    int w, h;
    double hz;
    char type[16], wallpaper[32], role[24];
    /* context */
    char gl_vendor[64], gl_renderer[96], gl_version[96];
    gboolean gl_es;
    int gl_ver;                 /* 33, 46, 32 (ES) ... */
    char timer[96];             /* how GPU time was measured, or why not */
    gboolean timer_ok;
    char debug[96];             /* state of the KHR_debug channel */
    gboolean hooks_ok;          /* draw / upload counters available (interposer) */
    gboolean attached;          /* the thread reported in time */
    /* frames */
    double seconds;             /* length of the run on this thread */
    guint frames;               /* frames rendered */
    guint presented;            /* of them, swapped to the window */
    guint skipped;              /* wake-ups that drew nothing: unchanged */
    guint gpu_frames;           /* frames whose timer queries were read back */
    guint disjoint;             /* frames dropped: GPU_DISJOINT (ES) or unread queries */
    guint64 gpu_ns;             /* sum over gpu_frames */
    guint64 gpu_max_ns;         /* the most expensive of them */
    guint64 stage_wall_ns[WALPE_PROF_N_STAGES], stage_cpu_ns[WALPE_PROF_N_STAGES];
    guint64 counters[WALPE_PROF_N_COUNTERS];   /* inside frames */
    guint64 load_upload_bytes;  /* uploads outside frames (wallpaper loads) */
    guint fx_run, fx_cached;    /* effect passes drawn / served from the static cache */
    WalpeProfRow *rows;
    guint n_rows;
    /* memory at the end of the run */
    gint64 mem_bytes[WALPE_PROF_N_MEM];
    int mem_count[WALPE_PROF_N_MEM];
    gboolean mem_shared;        /* textures counted on this monitor are the share group's */
    gint64 vram_total_kb, vram_avail_kb;   /* GL_NVX_gpu_memory_info, 0 = n/a */
    gint64 vram_evictions;      /* evictions during the run, -1 = n/a */
    /* the part of the run in which nothing was drawn */
    double idle_seconds;        /* gaps of more than WALPE_PROF_IDLE_GAP_S without a drawn frame */
    guint draw_runs;            /* runs of frames drawn without such a gap in between */
    double paused_seconds;      /* of the run, as the daemon reported it (walpe_prof_note_pause) */
    char pause_reason[64];      /* why (the reasons seen, comma separated) */
    gboolean wall_timer;        /* a row's "GPU time" is wall time by construction (software GL
                                 * draws on the render thread; --sync times glFinish to glFinish):
                                 * no row is flagged as CPU bound */
} WalpeProfMon;

typedef struct {
    guint source, type, severity, id;   /* GL_DEBUG_* */
    guint count;
    guint monitors;                     /* bit n = seen on monitor n */
    guint key_hash;                     /* of its de-duplication key (walpe_prof_msg_add) */
    char text[256];
} WalpeProfMsg;

typedef struct {
    char when[40];              /* ISO 8601 local time of the end of the run */
    char version[24];
    double seconds;             /* requested length */
    gboolean shared;
    WalpeProfMon *mons;
    guint n_mons;
    WalpeProfMsg *msgs;
    guint n_msgs;
    guint msgs_dropped;         /* distinct messages beyond the table size */
} WalpeProfReport;

#define WALPE_PROF_TOP_ROWS 15

/* honest numbers when a monitor did not draw for part of a run, and
 * for rows whose GPU timer brackets CPU work. */
/* a pause between two drawn frames longer than this is time not drawing */
#define WALPE_PROF_IDLE_GAP_S 0.25
/* frames per second over the time the monitor was drawing (= over the run
 * when it drew all the time): the frame intervals inside its runs of
 * frames, so that a scene which draws one frame a second does not read as
 * "60 a second while drawing" */
double walpe_prof_mon_fps_drawing (const WalpeProfMon *m);
/* A row's GPU time comes from GL_TIME_ELAPSED, which runs from "everything
 * before the row has completed" to "everything in the row has completed":
 * the time the render thread spends inside the row (the driver copying an
 * upload out of client memory, a script) is in it as GPU waiting time. A
 * row is flagged when that share is large; its GPU work is then somewhere
 * between gpu - cpu and gpu. Returns the possible waiting time per frame in
 * ms (0 = the row is not flagged). */
double walpe_prof_row_cpu_wait_ms (const WalpeProfMon *m, const WalpeProfRow *w);
/* lower bound of a monitor's GPU ms per frame: the total minus the possible
 * waiting time of its flagged rows */
double walpe_prof_mon_gpu_ms_min (const WalpeProfMon *m);

/* newly allocated text / JSON of a report (g_free) */
char *walpe_prof_format_text (const WalpeProfReport *r);
char *walpe_prof_format_json (const WalpeProfReport *r);
/* the "one-line total" of the text report (g_free; no newline) */
char *walpe_prof_format_total (const WalpeProfReport *r);

/* helpers shared with the tests */
/* pixel area of the +-1 quad under mvp (3x3 column-major), clipped to a
 * w x h target (bounding box of the transformed corners) */
double walpe_prof_quad_pixels (const float *mvp, int w, int h);
/* bytes of a w x h client-memory image in a GL format / type pair */
gsize walpe_prof_image_bytes (guint format, guint type, int w, int h);
/* de-duplication key of a driver message: digits collapsed, so the same
 * message about buffer 3 and buffer 17 is one entry (g_free) */
char *walpe_prof_msg_key (guint source, guint type, guint id, const char *text);
/* add one message to a table of up to max entries; returns its index or -1
 * when the table is full (*dropped is then incremented) */
int walpe_prof_msg_add (WalpeProfMsg *tab, guint *n, guint max, guint *dropped,
                        guint source, guint type, guint severity, guint id,
                        const char *text, int monitor);
const char *walpe_prof_stage_name (int stage);

G_END_DECLS
