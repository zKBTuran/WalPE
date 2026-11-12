/* memreport.h - `walpe-ctl mem`: where the daemon's memory is.
 *
 * Nothing here runs unless somebody asks ({"cmd":"mem"}): no timer, no
 * sampling, no thread. What a report needs that cannot be found by looking
 * (how often something happened) is counted with walpe_memc_inc (), one
 * integer increment on a path that runs anyway and never once per frame.
 *
 * The report has three kinds of numbers:
 *   - what the kernel says about the process (/proc/self/status, the
 *     mappings by kind from /proc/self/smaps, the open descriptors);
 *   - what the allocator says (mallinfo2, the number of arenas);
 *   - what the GL says: OpenGL cannot list its objects, so the small,
 *     sequential name space of each object type is probed (glIsTexture
 *     ...), as walpe_gl_mem_report and the GPU profiler do. Sizes are read
 *     with the direct-state-access queries, which neither bind nor create
 *     anything: the census changes no GL state and can run next to the
 *     other monitors' threads. A driver without them (OpenGL ES, old
 *     desktop drivers) gets counts without sizes.
 * A content adds what it knows about itself (content.h mem_stats). */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

/* ---- lifetime counters (any thread) */
typedef enum {
    WALPE_MEMC_IPC = 0,        /* requests answered on the control socket */
    WALPE_MEMC_STATUS,         /* status objects built (requests and pushed lines) */
    WALPE_MEMC_STATE_PUSH,     /* ... of them: built for the subscribers */
    WALPE_MEMC_MEM,            /* memory reports */
    WALPE_MEMC_PAUSE,          /* a monitor's thread was told to pause */
    WALPE_MEMC_RESUME,         /* ... to resume */
    WALPE_MEMC_LOAD,           /* wallpaper loads begun by a render thread */
    WALPE_MEMC_LOAD_FAILED,
    WALPE_MEMC_IDLE_RELEASE,   /* paused wallpapers unloaded (idle_release) */
    WALPE_MEMC_ROLE,           /* shared rendering: a thread changed its role */
    WALPE_MEMC_PAUSED_FRAME,   /* frames drawn on a paused monitor (forced) */
    WALPE_MEMC_TARGET_DROP,    /* a scene gave back all its render targets */
    WALPE_MEMC_TEXT_RENDER,    /* text layers rendered into their texture */
    WALPE_MEMC_FX_COMPARE,     /* a specialised pass program was compared with the plain one
                                * (a scratch texture, a framebuffer and one read-back each) */
    WALPE_MEMC_N
} WalpeMemCounter;

extern gint walpe_memc_[WALPE_MEMC_N];
static inline void walpe_memc_inc (WalpeMemCounter c)
{
    g_atomic_int_inc (&walpe_memc_[c]);
}
const char *walpe_memc_name (WalpeMemCounter c);
/* {"<name>": n, ...} as members of the object being built */
void walpe_mem_counters_json (JsonBuilder *b);

/* ---- the process (any thread; reads /proc/self) */
enum {
    WALPE_MAP_HEAP = 0,   /* [heap]: the main malloc arena */
    WALPE_MAP_ANON,       /* no name: other arenas, large malloc blocks, thread stacks,
                           * and whatever a library maps for itself (a GL driver's
                           * own allocator and its copies of textures and buffers) */
    WALPE_MAP_STACK,      /* [stack] */
    WALPE_MAP_NVIDIA,     /* the /dev/nvidia... nodes: memory shared with the NVIDIA driver */
    WALPE_MAP_DRI,        /* the /dev/dri nodes, dma-bufs: memory of a kernel graphics driver */
    WALPE_MAP_MEMFD,      /* memfd files */
    WALPE_MAP_SHM,        /* files in /dev/shm, SysV segments (MIT-SHM pictures), shared
                           * anonymous mappings ("/dev/zero (deleted)") */
    WALPE_MAP_LIB,        /* shared libraries and the program itself */
    WALPE_MAP_FILE,       /* other files (fonts, caches, wallpaper files) */
    WALPE_MAP_OTHER,      /* [vdso] and the like */
    WALPE_MAP_N
};
const char *walpe_mem_map_kind_name (int kind);
/* the kind of a mapping by the name the kernel prints for it (the 6th
 * column of /proc/<pid>/maps; "" or NULL = none) */
int walpe_mem_map_kind (const char *name);

typedef struct {
    guint n;
    gint64 size_kb, rss_kb, dirty_kb, swap_kb;   /* dirty = Private_Dirty: nobody else's */
} WalpeMemKind;

#define WALPE_MEM_TOP 8
typedef struct {
    WalpeMemKind kind[WALPE_MAP_N];
    guint n_maps;
    /* the mappings with the most resident memory: without a name, and of
     * files (the file's base name only) */
    struct { gint64 size_kb, rss_kb; char perms[5]; } top_anon[WALPE_MEM_TOP];
    struct { gint64 size_kb, rss_kb; char name[48]; } top_file[WALPE_MEM_TOP];
    /* resident memory the kernel maps in 2 MiB units (transparent huge
     * pages): AnonHugePages, ShmemPmdMapped, FilePmdMapped of all mappings */
    gint64 huge_anon_kb, huge_shmem_kb, huge_file_kb;
} WalpeMemMaps;
/* text = the contents of /proc/<pid>/smaps; exe = the program's own file
 * (counted with the libraries; NULL = not known) */
void walpe_mem_smaps_parse (const char *text, const char *exe, WalpeMemMaps *out);

/* The same mappings as a table by name: one row per file (its base name),
 * per special name as the kernel prints it ("/dev/nvidiactl", "/memfd:x
 * (deleted)", "/SYSV00000000 (deleted)", "[stack]"), "[heap]", and one row
 * "[anon]" for the mappings without a name. What is resident of a row is
 * split the way /proc/<pid>/status splits it: anonymous pages (RssAnon),
 * pages of shared memory (RssShmem: /dev/shm, SysV segments, memfd, shared
 * anonymous mappings, a kernel graphics driver's buffer objects) and the
 * rest (RssFile: files, and what a device node lets a process map). shared
 * = resident pages another process maps too; huge = the part of rss that
 * is mapped in 2 MiB units (it comes and goes 2 MiB at a time). */
typedef struct {
    char name[96];
    int kind;
    guint n;
    gint64 size_kb, rss_kb, anon_kb, file_kb, shmem_kb, shared_kb, swap_kb, huge_kb;
} WalpeMemMapRow;
/* by_name / by_size: NULL or where a GArray of WalpeMemMapRow goes, the
 * most resident first (g_array_unref). by_size has one row per name AND
 * size ("/dev/nvidiactl 4096 kB", "[anon] 65536 kB") for the kinds a driver
 * or an allocator makes at run time (anonymous, nvidia, dri, memfd, shm):
 * a mapping that appears, goes or changes its size between two reports is
 * a row whose count changes. */
void walpe_mem_smaps_tables (const char *text, const char *exe, WalpeMemMaps *out, GArray **by_name,
                             GArray **by_size);

/* "process": {...}, "mappings": {...}, "fds": {...}, "malloc": {...},
 * "threads": {...} as members of the object being built */
void walpe_mem_process_json (JsonBuilder *b);

/* ---- GL storage events (any thread)
 * How often storage for a texture was made (with or without pixels, by
 * size class), textures, framebuffers, buffers and programs were made and
 * deleted, and pixels were read back - wherever in the process the call
 * came from. Nothing of this exists until it is asked for: on = TRUE puts
 * counting wrappers into the libepoxy slots of a dozen entry points that
 * make or delete objects (the way the GPU profiler does for its runs; no
 * draw call, nothing that runs per frame in a still scene), FALSE takes
 * them out again and forgets the counts. While a GPU profile runs, its
 * wrappers stand in front of five of the slots and those events are not
 * counted here. Returns the number of entry points being counted. */
int walpe_mem_gl_events (gboolean on);
/* "gl_events": {...} as a member of the object being built */
void walpe_mem_gl_events_json (JsonBuilder *b);
/* the size class of an event of that many bytes (its name; index in *cls) */
const char *walpe_mem_size_class (gint64 bytes, int *cls);

/* ---- the GL objects of the current context (a render thread)
 * "gl": {...} as a member of the object being built. deep: also ask every
 * program for the size of its binary (a driver may have to serialise it:
 * slower). */
void walpe_mem_gl_json (JsonBuilder *b, gboolean deep);

/* helpers for whoever writes a part of the report */
void walpe_mem_jint (JsonBuilder *b, const char *key, gint64 v);
void walpe_mem_jstr (JsonBuilder *b, const char *key, const char *v);
