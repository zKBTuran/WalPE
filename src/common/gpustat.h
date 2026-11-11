/* gpustat.h - GPU usage and GPU memory as the kernel and the drivers
 * account them, for any vendor.
 *
 * One layer with several sources, chosen at run time by what exists on the
 * machine. Every value says where it came from, or that it is not
 * available and why:
 *
 *   fdinfo       /proc/<pid>/fdinfo/<fd> of DRM file descriptors: the
 *                kernel's per-client statistics (drm-engine-<name> in ns,
 *                drm-cycles-<name> / drm-total-cycles-<name>, drm-memory-,
 *                drm-resident-, drm-total-, drm-shared-<region> ...).
 *                Published by i915, xe, amdgpu, v3d, panfrost, msm and
 *                others; not by the NVIDIA proprietary or open kernel
 *                modules. Two samples are needed for a busy share: engine
 *                time difference / wall time difference.
 *   amdgpu       device attributes in sysfs: gpu_busy_percent,
 *                mem_busy_percent, mem_info_vram_*, mem_info_gtt_*
 *   Intel        the frequency attributes of i915 and xe in sysfs (the
 *                drivers have no whole-GPU busy figure an ordinary user may
 *                read: it is the sum of the visible clients' engine time)
 *   hwmon        the device's hwmon node: power, temperature, clocks
 *   nvidia-smi   the NVIDIA tool, only when the caller allows running a
 *                program (walpe-ctl does, the daemon never does)
 *
 * What is read of other processes: only for processes of the calling user,
 * only /proc/<pid>/comm (the process name) and the fdinfo files; of those
 * only the documented drm-* keys (and amd-evicted-vram) are kept. Never a
 * command line, an environment or a file name. Processes of other users are
 * counted, not read.
 *
 * No libdrm, no ioctl, no root. /proc and /sys are parameters, so the tests
 * run against trees they write themselves (tests/fake-gpustat-tree.sh). */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ---- where a value came from ------------------------------------------- */
typedef enum {
    WALPE_GPU_SRC_NONE = 0,       /* not available */
    WALPE_GPU_SRC_FDINFO,         /* DRM per-client statistics */
    WALPE_GPU_SRC_AMD_SYSFS,      /* amdgpu device attributes */
    WALPE_GPU_SRC_INTEL_SYSFS,    /* i915 / xe frequency attributes */
    WALPE_GPU_SRC_HWMON,          /* the device's hwmon node */
    WALPE_GPU_SRC_NVIDIA_SMI,     /* nvidia-smi */
    WALPE_GPU_SRC_DERIVED,        /* computed from two other rows */
    WALPE_GPU_N_SRC
} WalpeGpuSrc;

/* short name for tables and JSON: "fdinfo", "amdgpu sysfs", ... */
const char *walpe_gpu_src_name (WalpeGpuSrc src);

/* ---- one fdinfo file (pure parser; the tests feed it text) ------------- */
#define WALPE_GPU_MAX_ENGINES 16
#define WALPE_GPU_MAX_REGIONS 12

typedef struct {
    char name[32];                /* render, gfx, rcs, video, ... (the driver's name) */
    gboolean has_ns;              /* drm-engine-<name>: <n> ns */
    gboolean has_cycles;          /* drm-cycles-<name> */
    gboolean has_total_cycles;    /* drm-total-cycles-<name> */
    guint64 ns, cycles, total_cycles;
    guint capacity;               /* drm-engine-capacity-<name>, 0 = not given (means 1) */
} WalpeGpuFdEngine;

typedef struct {
    char name[32];                /* vram, gtt, cpu, system0, local0, memory, ... */
    /* KiB, -1 = the key is not there. resident is drm-resident-<name> or its
     * older alias drm-memory-<name> */
    gint64 resident_kb, total_kb, shared_kb, active_kb, purgeable_kb;
} WalpeGpuFdRegion;

typedef struct {
    char driver[32];              /* drm-driver (required: without it the file is no DRM client) */
    char pdev[24];                /* drm-pdev: PCI address, "" for a platform device */
    gboolean has_client_id;
    guint64 client_id;            /* drm-client-id: one per open DRM file, the same through dup()ed fds */
    WalpeGpuFdEngine engines[WALPE_GPU_MAX_ENGINES];
    guint n_engines;
    WalpeGpuFdRegion regions[WALPE_GPU_MAX_REGIONS];
    guint n_regions;
    gint64 evicted_kb;            /* amd-evicted-vram, -1 = not there */
} WalpeGpuFdinfo;

/* text of one /proc/<pid>/fdinfo/<fd>. FALSE when it is not a DRM client
 * (no drm-driver key). Unknown keys, unknown units and values that are not
 * numbers are skipped; nothing in the text can make it fail otherwise. */
gboolean walpe_gpu_fdinfo_parse (const char *text, WalpeGpuFdinfo *out);

/* ---- sampling ----------------------------------------------------------- */
enum {
    /* also read the other processes of this user that have DRM clients (the
     * "programs on the GPU" list and the sum of all visible clients).
     * Without it only the target process is read. */
    WALPE_GPUSTAT_OTHERS = 1 << 0,
    /* nvidia-smi may be run when the GPU is driven by the NVIDIA driver.
     * walpe-ctl sets it; the daemon never starts a program for statistics. */
    WALPE_GPUSTAT_SPAWN = 1 << 1,
    /* only the processes' own figures: the device's attributes in sysfs
     * (busy, memory, power, temperature, clocks) are not read */
    WALPE_GPUSTAT_NO_DEVICE = 1 << 2,
};

typedef struct {
    const char *proc_root;        /* NULL = "/proc" */
    const char *sys_root;         /* NULL = "/sys" */
    int target_pid;               /* the process the report is about (walpe-daemon); 0 = none:
                                   * the GPU alone; -1 = the calling process */
    guint flags;                  /* WALPE_GPUSTAT_* */
    /* which GPU: "card1", "/dev/dri/card1", "renderD129", "/dev/dri/renderD129"
     * or a PCI address "0000:03:00.0". NULL = follow the target: the device
     * of its DRM clients (the one with the most engine time when it has
     * clients on two), else what walpe_gpustat_set_render_device () was told,
     * else the GL vendor below, else the only GPU, else the boot GPU. */
    const char *device;
    const char *gl_vendor;        /* the target's GL_VENDOR when the caller knows it, else NULL */
    const char *nvidia_smi;       /* program to run with WALPE_GPUSTAT_SPAWN; NULL = "nvidia-smi" */
    gint64 uid;                   /* whose processes may be read; < 0 = the calling user. (0 is a
                                   * valid uid: use -1 for "mine") */
    guint rescan_every;           /* walk all of /proc every N samples (0 = 5); in between only the
                                   * DRM descriptors found by the last walk are read again */
} WalpeGpuStatOpts;

typedef struct WalpeGpuSampler WalpeGpuSampler;

/* opts may be NULL (the calling process, real /proc and /sys, no other
 * processes, no programs). The strings in opts are copied. */
WalpeGpuSampler *walpe_gpu_sampler_new (const WalpeGpuStatOpts *opts);
void walpe_gpu_sampler_free (WalpeGpuSampler *s);
/* take one sample now. The first sample is the baseline: shares need two.
 * Not thread-safe: one thread owns a sampler. */
void walpe_gpu_sampler_tick (WalpeGpuSampler *s);
/* the same with the clock given (monotonic ns): the tests step time */
void walpe_gpu_sampler_tick_at (WalpeGpuSampler *s, gint64 mono_ns);
/* forget min / avg / max and the accumulated engine time, keep the
 * baselines: the next report covers the time from now on */
void walpe_gpu_sampler_restart (WalpeGpuSampler *s);

/* the daemon tells which device it renders on, once it knows (the same
 * spellings as WalpeGpuStatOpts.device; NULL = forget). Used by samplers
 * whose target is the calling process and that were given no device.
 * Thread-safe. */
void walpe_gpustat_set_render_device (const char *node_or_pci_slot);
/* GL_VENDOR of the calling process's contexts, once one exists: on a
 * machine with two GPUs it tells them apart when the process has no DRM
 * client with statistics (the NVIDIA driver). NULL = forget. Thread-safe. */
void walpe_gpustat_set_gl_vendor (const char *gl_vendor);

/* The kernel driver ("amdgpu", "i915", "nvidia", ...) of the device the
 * calling process renders on, as far as it can be told without looking at
 * any process: the device walpe_gpustat_set_render_device () named, else
 * the one the GL vendor points to, else the only / the boot / the first
 * GPU of sysfs. FALSE (driver = "") when no GPU is listed or it has no
 * driver. Reads the device list under sys_root (NULL = "/sys") and nothing
 * else: no /proc, no program. Thread-safe. */
gboolean walpe_gpustat_render_driver (const char *sys_root, char *driver, gsize size);

/* What a profile run inside the daemon samples of the kernel's statistics
 * (engine/gpuprof.c). */
typedef enum {
    WALPE_GPU_PROFILE_KERNEL_OFF = 0,   /* nothing: no thread, no section in the report */
    WALPE_GPU_PROFILE_KERNEL_SELF,      /* the calling process and the device */
    WALPE_GPU_PROFILE_KERNEL_ALL,       /* and the user's other programs on the GPU */
} WalpeGpuProfileKernel;

/* setting = the value of WALPE_GPU_PROFILE_KERNEL (NULL or "" = not set):
 *   0 | off | no -> OFF      self -> SELF      anything else (1, on, all) -> ALL
 *   not set      -> ALL; but OFF when walpe_gpustat_render_driver () says
 *                   "nvidia": that driver publishes no figure there, and a
 *                   profile on it is then exactly what it was without this
 *                   layer (no thread reading /proc, no extra section).
 * sys_root as above. */
WalpeGpuProfileKernel walpe_gpustat_profile_kernel (const char *setting, const char *sys_root);

/* ---- the result ---------------------------------------------------------- */
enum {
    WALPE_GPU_M_BUSY,             /* % of time the GPU was busy: a figure of the device itself */
    WALPE_GPU_M_BUSY_CLIENTS,     /* %: all visible clients together on the main engine (fdinfo) */
    WALPE_GPU_M_BUSY_SCALED,      /* %: busy x clock / highest clock, a rough "at full clock" figure */
    WALPE_GPU_M_MEM_BUSY,         /* % of time the GPU memory was read or written */
    WALPE_GPU_M_VRAM_USED,        /* MiB of dedicated memory in use, all programs */
    WALPE_GPU_M_VRAM_TOTAL,       /* MiB */
    WALPE_GPU_M_GTT_USED,         /* MiB of system memory mapped for the GPU, all programs */
    WALPE_GPU_M_GTT_TOTAL,        /* MiB */
    WALPE_GPU_M_POWER,            /* W */
    WALPE_GPU_M_TEMP,             /* degrees C */
    WALPE_GPU_M_CLOCK,            /* MHz, the core clock now */
    WALPE_GPU_M_CLOCK_MAX,        /* MHz, the highest it can run at */
    WALPE_GPU_M_MEM_CLOCK,        /* MHz */
    WALPE_GPU_M_PROC_BUSY,        /* %: the target's share of the main engine */
    WALPE_GPU_M_PROC_MEM,         /* MiB: the target's GPU memory */
    WALPE_GPU_M_OTHERS_BUSY,      /* %: the visible clients other than the target, main engine */
    WALPE_GPU_N_METRICS
};

typedef struct {
    WalpeGpuSrc src;              /* WALPE_GPU_SRC_NONE = not available: see why */
    guint n;                      /* samples that had a value */
    double min, avg, max, last;
    gboolean lower_bound;         /* the real value may be higher (clients that cannot be seen) */
    char why[128];                /* not available: the reason, in words for the user */
} WalpeGpuStatVal;

typedef struct {
    char name[32];
    guint capacity;               /* identical hardware engines behind the name */
    gboolean cycles;              /* measured as cycles / total cycles (xe), else ns / wall time */
    gboolean has_target;          /* the target had a client with this engine */
    double proc_pct;              /* the target over the whole run, % of the engine's capacity */
    double proc_max_pct;          /* its busiest interval */
    double all_pct;               /* every visible client */
    guint resets;                 /* counters that went backwards for good (see gpustat.c) */
} WalpeGpuEngineStat;

typedef struct {
    int pid;
    char name[32];                /* /proc/<pid>/comm */
    gboolean target;
    gboolean exited;              /* went away during the sampling */
    guint clients;                /* DRM clients on this device */
    gboolean has_busy;
    double busy_pct;              /* main engine, over the time the process was seen */
    gint64 mem_kb;                /* when last seen; -1 = the driver publishes none */
} WalpeGpuProcStat;

typedef struct {
    /* the device */
    char driver[32];              /* kernel driver: amdgpu, i915, xe, nvidia, nouveau, ...; "" = unknown */
    char pci_slot[24];            /* "0000:03:00.0", "" = not a PCI device or unknown */
    char card[16];                /* "card1", "" = unknown */
    char render_node[16];         /* "renderD129", "" = none */
    char vendor_name[16];         /* "AMD", "Intel", "NVIDIA", "" */
    guint vendor_id, device_id;   /* PCI ids, 0 = unknown */
    char name[96];                /* a product name when a source has one (nvidia-smi), else "" */
    char driver_version[32];      /* nvidia-smi, else "" */
    char how_found[64];           /* why this device: "the daemon's DRM client", "the only GPU", ... */
    guint n_devices;              /* GPUs the kernel lists */
    guint sources;                /* bit (1 << WalpeGpuSrc) of every source that has figures here */
    /* the run */
    double seconds;               /* first sample to last */
    guint samples;
    int target_pid;               /* 0 = no target */
    char target_name[32];
    gboolean target_seen;         /* the process existed at a sample */
    gboolean target_gone;         /* ... and went away during the sampling */
    guint target_clients;         /* its DRM clients on this device */
    gboolean others_read;         /* WALPE_GPUSTAT_OTHERS was set */
    gboolean fdinfo_engines;      /* some client of this device publishes engine time */
    gboolean fdinfo_memory;       /* ... memory */
    char main_engine[32];         /* the engine the busy rows are about, "" = none */
    WalpeGpuStatVal m[WALPE_GPU_N_METRICS];
    WalpeGpuEngineStat *engines;
    guint n_engines;
    WalpeGpuFdRegion *regions;    /* the target's memory by region, at the last sample */
    guint n_regions;
    gboolean mem_is_total;        /* no resident figure: PROC_MEM is drm-total-* (allocated) */
    gint64 proc_evicted_kb;       /* amdgpu: of the target's VRAM, moved out for lack of room; -1 = n/a */
    WalpeGpuProcStat *procs;      /* programs on this device, busiest first (the target included) */
    guint n_procs;
    /* what could not be seen or counted */
    guint procs_other_users;      /* processes of other users (never read) */
    guint procs_denied;           /* processes of this user whose fdinfo may not be read */
    guint clients_shared;         /* clients seen through several descriptors (counted once) */
    guint clients_no_id;          /* clients without drm-client-id (cannot be told apart from copies) */
    guint counter_resets;
} WalpeGpuStatReport;

/* statistics over the samples so far (g-allocated; never NULL) */
WalpeGpuStatReport *walpe_gpu_sampler_report (WalpeGpuSampler *s);
void walpe_gpu_stat_report_free (WalpeGpuStatReport *r);

/* ---- text and JSON (pure: the tests build reports by hand) -------------- */
/* one line that names the device and the driver, no newline:
 * "AMD [1002:73df] at 0000:03:00.0 (card1), driver amdgpu" */
void walpe_gpu_stat_format_device (const WalpeGpuStatReport *r, GString *out);
/* the sources that delivered values: "fdinfo, amdgpu sysfs, hwmon" or "none" */
void walpe_gpu_stat_format_sources (const WalpeGpuStatReport *r, GString *out);
/* rows of the table walpe-gpu-report prints (metric, unit, min, avg, max,
 * samples, then the source in brackets or "not available: why"). who =
 * what the target is called in the rows ("daemon"). */
void walpe_gpu_stat_format_rows (const WalpeGpuStatReport *r, const char *who, GString *out);
/* "programs on the GPU": up to max lines, then what is not in the list */
void walpe_gpu_stat_format_programs (const WalpeGpuStatReport *r, guint max, GString *out);
/* reading notes that apply to this report (walpe-gpu-report's last part) */
void walpe_gpu_stat_format_notes (const WalpeGpuStatReport *r, GString *out);
/* the section of `walpe-ctl profile`: a few lines, each starting with three
 * spaces, the heading without */
void walpe_gpu_stat_format_profile (const WalpeGpuStatReport *r, GString *out);
/* one JSON object (begins and ends it) */
void walpe_gpu_stat_to_json (const WalpeGpuStatReport *r, JsonBuilder *b);

/* ---- the calling process, for a status line ------------------------------ */
typedef struct {
    char driver[32], pci_slot[24], card[16], vendor_name[16];
    guint vendor_id, device_id;
    char sources[96];             /* "fdinfo, amdgpu sysfs" / "none" */
    gboolean has_busy;            /* FALSE on the first call and when the driver publishes none */
    double busy_pct;              /* this process's share of the main engine ... */
    double busy_window_s;         /* ... over this many seconds before the call */
    char engine[32];
    gint64 mem_kb;                /* this process's GPU memory, -1 = not available */
    char note[128];               /* why something is missing, "" */
} WalpeGpuSelfStat;

/* Reads this process's own fdinfo and its device's sysfs attributes, at
 * most once a second (calls in between return the last result). The busy
 * share is over the time since the reading before. Thread-safe; never
 * reads another process and never starts a program.
 * Nothing is read before the renderer's GL vendor is told
 * (walpe_gpustat_set_gl_vendor), and nothing at all - no file of /proc,
 * no file of /sys - when that vendor is NVIDIA: its kernel driver
 * publishes no figure there, and the answer (driver "nvidia", sources
 * "none", the note that names nvidia-smi) is made from the vendor alone.
 * Where the kernel lists no GPU for the renderer the second reading is the
 * last: later calls return it without touching a file, until the renderer
 * is told anew (walpe_gpustat_set_render_device / _set_gl_vendor). */
void walpe_gpustat_self (WalpeGpuSelfStat *out);
/* how often walpe_gpustat_self has read /proc so far (tests) */
guint walpe_gpustat_self_readings (void);
/* how often this process has read the device list of sysfs so far (tests) */
guint walpe_gpustat_device_scans (void);

G_END_DECLS
