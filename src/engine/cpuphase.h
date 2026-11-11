/* cpuphase.h - where the render thread's CPU time of a scene frame goes.
 *
 * Diagnostics, off unless WALPE_CPU_PHASES is set in front of the process
 * (1 = times and counts; 2 = also a hash of every particle's state after
 * each step and of every byte of particle vertex data sent to the driver,
 * for comparing two builds under a fixed seed). When off, a phase switch
 * is one load and one predicted branch.
 *
 * Inside a scene frame the render thread is always "in" one phase;
 * walpe_cph_to () moves it to another and returns the one it left (to go
 * back to after a nested piece of work; WALPE_CPH_NONE = outside a frame,
 * not counted). Times are thread CPU time (CLOCK_THREAD_CPUTIME_ID),
 * so a loaded machine does not stretch them. Read with walpe_cph_get ()
 * on the same thread (tests/scenecpu.c). */
#pragma once
#include <glib.h>

typedef enum {
    WALPE_CPH_NONE = -1,
    WALPE_CPH_OTHER,      /* frame set-up, audio / media snapshot, layer sync, deferred loads */
    WALPE_CPH_SCRIPT,     /* SceneScript: the tick (QuickJS + the property bridge), cursor events */
    WALPE_CPH_VIDEO,      /* video frames into their textures */
    WALPE_CPH_WORLD,      /* layer transforms and visibility */
    WALPE_CPH_TEXT,       /* text layers rasterised and uploaded */
    WALPE_CPH_PT_SIM,     /* particle simulation */
    WALPE_CPH_SIG,        /* layer / frame signatures (frame skip, static bake) */
    WALPE_CPH_DRAW,       /* layer draws and effect passes: engine work + GL calls */
    WALPE_CPH_PT_BUILD,   /* particle vertex data built (engine only, no GL call) */
    WALPE_CPH_PT_GL,      /* particle state, upload and draw calls (GL driver) */
    WALPE_CPH_POST,       /* after the layers: target pool, heap trim, next deadline */
    WALPE_CPH_N
} WalpeCpuPhase;

typedef struct {
    double ms[WALPE_CPH_N];   /* thread CPU time per phase since the last reset */
    guint64 frames;           /* scene frames begun (walpe_cph_frame) */
    guint64 pt_steps;         /* particle objects stepped */
    guint64 pt_live;          /* live particles, summed over those steps */
    guint64 pt_sim_hash;      /* level 2: FNV-1a over every particle's state after each step */
    guint64 pt_draws;         /* particle draw calls */
    guint64 pt_bytes;         /* particle vertex data handed to the driver, bytes */
    guint64 pt_vtx_hash;      /* level 2: FNV-1a over those bytes, in draw order */
    guint64 text_renders;     /* text layers rasterised */
    guint64 world_updates;    /* layer world matrices recomputed */
    guint64 sig_layers;       /* layer signatures computed */
} WalpeCphStats;

extern gint walpe_cph_level;          /* -1 = WALPE_CPU_PHASES not read yet, 0 = off */
/* Every render thread tests the level many times per frame while the first
 * one to get there stores it (cpuphase.c): read through this, never by
 * name. A relaxed atomic load: the level is one self-contained value that
 * every thread works out alike (the reasoning of common/lazy.h), and the
 * load is the same single instruction the plain read was. */
static inline int walpe_cph_level_get (void)
{
    return __atomic_load_n (&walpe_cph_level, __ATOMIC_RELAXED);
}
int walpe_cph_switch (int phase);     /* (slow path of walpe_cph_to) */
WalpeCphStats *walpe_cph_stats (void);/* this thread's counters; NULL when off */
void walpe_cph_get (WalpeCphStats *out, gboolean reset);
guint64 walpe_cph_fnv (guint64 h, const void *p, gsize n);

static inline int walpe_cph_to (int phase)
{
    return G_UNLIKELY (walpe_cph_level_get () != 0) ? walpe_cph_switch (phase) : phase;
}

/* counters: `WALPE_CPH_COUNT (pt_draws, 1);` (nothing when off) */
#define WALPE_CPH_COUNT(field, n) G_STMT_START { \
    if (G_UNLIKELY (walpe_cph_level_get () != 0)) { \
        WalpeCphStats *cph_ = walpe_cph_stats (); \
        if (cph_) cph_->field += (guint64) (n); \
    } } G_STMT_END
#define WALPE_CPH_HASHING() (G_UNLIKELY (walpe_cph_level_get () != 0) && walpe_cph_stats () && \
                             walpe_cph_level_get () >= 2)
