/* gpustat.c - GPU usage and memory from the kernel's own accounting (see
 * gpustat.h for what is read and what is not).
 *
 * Layout:
 *   1. small helpers: files, numbers, running statistics
 *   2. the fdinfo parser (pure)
 *   3. devices: /sys/class/drm, PCI display devices without a DRM node
 *   4. device-level sources: amdgpu, Intel clocks, hwmon, nvidia-smi
 *   5. the sampler: processes, clients, engines, one tick
 *   6. the report
 *   7. text and JSON
 *   8. the calling process (status line)
 *
 * The fdinfo format is the kernel's "DRM client usage stats"
 * (Documentation/gpu/drm-usage-stats.rst). The rules taken from it:
 *   - a file is a DRM client when it has a drm-driver key;
 *   - drm-client-id names the open DRM file: descriptors that are copies of
 *     one another (dup, passed over a socket) show the same id and must be
 *     counted once. The id is unique per device (drm-pdev) or globally;
 *   - engine time is in ns of the engine being busy for this client; a
 *     value may be seen lower than before: the reader keeps the larger
 *     value until the counter has caught up;
 *   - drm-engine-capacity-<name> = number of identical engines behind the
 *     name (1 when absent);
 *   - a driver without per-client ns (xe) gives drm-cycles-<name> and
 *     drm-total-cycles-<name>: share = cycle difference / total difference;
 *   - memory: drm-resident-<region> (older alias drm-memory-<region>),
 *     drm-total-, drm-shared-, drm-active-, drm-purgeable-<region>; bytes
 *     unless a KiB / MiB unit follows.
 * None of this has run on a GPU yet: the code was written against the
 * documented format and tested with hand-written trees only. */
#include "gpustat.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ===================================================== 1. small helpers */

#define FDINFO_MAX 16384          /* one fdinfo file; a DRM one is 1-3 KiB */
#define MAX_PROCS 4096            /* processes followed in one sampler */
#define MAX_CLIENTS 16384         /* DRM clients remembered in one sampler */
#define MAX_FDS_PER_PROC 8192     /* descriptors looked at per process and walk */
#define HOLD_SAMPLES 3            /* samples a counter may stay below its old value */

/* a name from /proc or /sys into a report: one line, no control characters,
 * valid UTF-8 or plain ASCII */
static void copy_name (char *dst, gsize size, const char *src)
{
    if (!size)
        return;
    gsize n = 0;
    for (; src && *src && n + 1 < size; src++) {
        guchar c = (guchar) *src;
        if (c == '\n' || c == '\r')
            break;
        dst[n++] = (c < 0x20 || c == 0x7f) ? '?' : (char) c;
    }
    dst[n] = 0;
    if (!g_utf8_validate (dst, -1, NULL))
        for (char *p = dst; *p; p++)
            if ((guchar) *p >= 0x80)
                *p = '?';
    g_strchomp (dst);
}

/* a small text file into buf, NUL-terminated. Returns its length or -1 with
 * errno set. dirfd may be AT_FDCWD. */
static gssize read_small_at (int dirfd, const char *path, char *buf, gsize size)
{
    int fd = openat (dirfd, path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    gsize n = 0;
    while (n < size - 1) {
        gssize r = read (fd, buf + n, size - 1 - n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            close (fd);
            errno = e;
            return -1;
        }
        if (r == 0)
            break;
        n += (gsize) r;
    }
    close (fd);
    buf[n] = 0;
    return (gssize) n;
}

static gssize read_small (const char *path, char *buf, gsize size)
{
    return read_small_at (AT_FDCWD, path, buf, size);
}

/* "dir/name" as one unsigned number (decimal, or hex with 0x). FALSE when
 * the file is missing, unreadable or does not start with a number. */
static gboolean read_u64 (const char *dir, const char *name, guint64 *out)
{
    char buf[64];
    char *path = g_build_filename (dir, name, NULL);
    gssize n = read_small (path, buf, sizeof (buf));
    g_free (path);
    if (n <= 0)
        return FALSE;
    const char *p = buf;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!g_ascii_isdigit (*p))
        return FALSE;
    char *end = NULL;
    errno = 0;
    guint64 v = g_ascii_strtoull (p, &end, 0);
    if (end == p || errno == ERANGE)
        return FALSE;
    *out = v;
    return TRUE;
}

/* the last element of what a symbolic link points to ("dir/name" ->
 * ".../drivers/amdgpu" gives "amdgpu"), "" when it is no link */
static void link_base (const char *dir, const char *name, char *out, gsize size)
{
    char target[PATH_MAX];
    out[0] = 0;
    char *path = g_build_filename (dir, name, NULL);
    gssize n = readlink (path, target, sizeof (target) - 1);
    g_free (path);
    if (n <= 0)
        return;
    target[n] = 0;
    const char *b = strrchr (target, '/');
    copy_name (out, size, b ? b + 1 : target);
}

/* running minimum / average / maximum */
typedef struct {
    guint n;
    double min, max, sum, last;
} Acc;

static void acc_add (Acc *a, double v)
{
    if (!isfinite (v))
        return;
    if (!a->n || v < a->min)
        a->min = v;
    if (!a->n || v > a->max)
        a->max = v;
    a->sum += v;
    a->last = v;
    a->n++;
}

static gboolean is_pci_slot (const char *s)
{
    /* hhhh:hh:hh.h */
    if (!s || strlen (s) != 12)
        return FALSE;
    for (int i = 0; i < 12; i++) {
        char want = (i == 4 || i == 7) ? ':' : (i == 10) ? '.' : 'x';
        if (want == 'x' ? !g_ascii_isxdigit (s[i]) : s[i] != want)
            return FALSE;
    }
    return TRUE;
}

static const char *vendor_name (guint id)
{
    switch (id) {
    case 0x1002: case 0x1022: return "AMD";
    case 0x8086: return "Intel";
    case 0x10de: return "NVIDIA";
    case 0x14e4: return "Broadcom";
    case 0x5143: return "Qualcomm";
    case 0x13b5: return "Arm";
    case 0x1af4: return "virtio";
    case 0x15ad: return "VMware";
    default: return "";
    }
}

const char *walpe_gpu_src_name (WalpeGpuSrc src)
{
    switch (src) {
    case WALPE_GPU_SRC_FDINFO: return "fdinfo";
    case WALPE_GPU_SRC_AMD_SYSFS: return "amdgpu sysfs";
    case WALPE_GPU_SRC_INTEL_SYSFS: return "Intel sysfs";
    case WALPE_GPU_SRC_HWMON: return "hwmon";
    case WALPE_GPU_SRC_NVIDIA_SMI: return "nvidia-smi";
    case WALPE_GPU_SRC_DERIVED: return "computed";
    default: return "none";
    }
}

/* ============================================== 2. the fdinfo parser */

/* "<digits>[ unit]" -> number and unit; FALSE when it does not start with
 * a digit or does not fit */
static gboolean parse_uint_unit (const char *v, guint64 *num, const char **unit)
{
    while (*v == ' ' || *v == '\t')
        v++;
    if (!g_ascii_isdigit (*v))
        return FALSE;
    char *end = NULL;
    errno = 0;
    guint64 n = g_ascii_strtoull (v, &end, 10);
    if (end == v || errno == ERANGE)
        return FALSE;
    while (*end == ' ' || *end == '\t')
        end++;
    *num = n;
    *unit = end;
    return TRUE;
}

/* the largest memory value taken from one key: 2^40 KiB (1 PiB). The values
 * are added up per client (WALPE_GPU_MAX_REGIONS of them) and per device (up
 * to MAX_CLIENTS clients) in gint64: with this limit no such sum can
 * overflow, whatever the text says */
#define MEM_KB_MAX ((guint64) 1 << 40)

/* a memory value in KiB; -1 when it is not a number or has an unknown unit */
static gint64 parse_mem_kb (const char *v)
{
    guint64 n;
    const char *unit;
    if (!parse_uint_unit (v, &n, &unit))
        return -1;
    if (!*unit)
        n /= 1024;                       /* bytes */
    else if (!strcmp (unit, "KiB"))
        ;
    else if (!strcmp (unit, "MiB"))
        n = n > MEM_KB_MAX / 1024 ? MEM_KB_MAX : n * 1024;
    else
        return -1;
    return (gint64) MIN (n, MEM_KB_MAX);
}

static WalpeGpuFdEngine *fd_engine (WalpeGpuFdinfo *f, const char *name)
{
    if (!*name || strlen (name) >= sizeof (f->engines[0].name))
        return NULL;
    for (guint i = 0; i < f->n_engines; i++)
        if (!strcmp (f->engines[i].name, name))
            return &f->engines[i];
    if (f->n_engines >= WALPE_GPU_MAX_ENGINES)
        return NULL;
    WalpeGpuFdEngine *e = &f->engines[f->n_engines++];
    memset (e, 0, sizeof (*e));
    copy_name (e->name, sizeof (e->name), name);
    return e;
}

static WalpeGpuFdRegion *fd_region (WalpeGpuFdinfo *f, const char *name)
{
    if (!*name || strlen (name) >= sizeof (f->regions[0].name))
        return NULL;
    for (guint i = 0; i < f->n_regions; i++)
        if (!strcmp (f->regions[i].name, name))
            return &f->regions[i];
    if (f->n_regions >= WALPE_GPU_MAX_REGIONS)
        return NULL;
    WalpeGpuFdRegion *r = &f->regions[f->n_regions++];
    copy_name (r->name, sizeof (r->name), name);
    r->resident_kb = r->total_kb = r->shared_kb = r->active_kb = r->purgeable_kb = -1;
    return r;
}

gboolean walpe_gpu_fdinfo_parse (const char *text, WalpeGpuFdinfo *out)
{
    memset (out, 0, sizeof (*out));
    out->evicted_kb = -1;
    if (!text)
        return FALSE;
    const char *line = text;
    while (*line) {
        const char *eol = strchr (line, '\n');
        gsize len = eol ? (gsize) (eol - line) : strlen (line);
        char buf[160];
        if (len < sizeof (buf)) {
            memcpy (buf, line, len);
            buf[len] = 0;
            char *colon = strchr (buf, ':');
            if (colon) {
                *colon = 0;
                char *key = g_strstrip (buf);
                char *val = g_strstrip (colon + 1);
                guint64 n;
                const char *unit;
                WalpeGpuFdEngine *e;
                WalpeGpuFdRegion *r;
                if (!strcmp (key, "drm-driver")) {
                    copy_name (out->driver, sizeof (out->driver), val);
                } else if (!strcmp (key, "drm-pdev")) {
                    if (is_pci_slot (val))
                        g_strlcpy (out->pdev, val, sizeof (out->pdev));
                } else if (!strcmp (key, "drm-client-id")) {
                    if (parse_uint_unit (val, &n, &unit) && !*unit) {
                        out->client_id = n;
                        out->has_client_id = TRUE;
                    }
                } else if (g_str_has_prefix (key, "drm-engine-capacity-")) {
                    if (parse_uint_unit (val, &n, &unit) && !*unit && n >= 1 && n <= 4096 &&
                        (e = fd_engine (out, key + 20)))
                        e->capacity = (guint) n;
                } else if (g_str_has_prefix (key, "drm-engine-")) {
                    /* the unit is ns; a value without a unit is taken as ns too */
                    if (parse_uint_unit (val, &n, &unit) && (!*unit || !strcmp (unit, "ns")) &&
                        (e = fd_engine (out, key + 11))) {
                        e->ns = n;
                        e->has_ns = TRUE;
                    }
                } else if (g_str_has_prefix (key, "drm-total-cycles-")) {
                    if (parse_uint_unit (val, &n, &unit) && !*unit && (e = fd_engine (out, key + 17))) {
                        e->total_cycles = n;
                        e->has_total_cycles = TRUE;
                    }
                } else if (g_str_has_prefix (key, "drm-cycles-")) {
                    if (parse_uint_unit (val, &n, &unit) && !*unit && (e = fd_engine (out, key + 11))) {
                        e->cycles = n;
                        e->has_cycles = TRUE;
                    }
                } else if (g_str_has_prefix (key, "drm-maxfreq-") ||
                           g_str_has_prefix (key, "drm-curfreq-")) {
                    /* documented, not used: the clock rows come from sysfs */
                } else if (g_str_has_prefix (key, "drm-memory-") ||
                           g_str_has_prefix (key, "drm-resident-")) {
                    gint64 kb = parse_mem_kb (val);
                    const char *name = key + (key[4] == 'm' ? 11 : 13);
                    if (kb >= 0 && (r = fd_region (out, name)))
                        r->resident_kb = kb;
                } else if (g_str_has_prefix (key, "drm-total-")) {
                    gint64 kb = parse_mem_kb (val);
                    if (kb >= 0 && (r = fd_region (out, key + 10)))
                        r->total_kb = kb;
                } else if (g_str_has_prefix (key, "drm-shared-")) {
                    gint64 kb = parse_mem_kb (val);
                    if (kb >= 0 && (r = fd_region (out, key + 11)))
                        r->shared_kb = kb;
                } else if (g_str_has_prefix (key, "drm-active-")) {
                    gint64 kb = parse_mem_kb (val);
                    if (kb >= 0 && (r = fd_region (out, key + 11)))
                        r->active_kb = kb;
                } else if (g_str_has_prefix (key, "drm-purgeable-")) {
                    gint64 kb = parse_mem_kb (val);
                    if (kb >= 0 && (r = fd_region (out, key + 14)))
                        r->purgeable_kb = kb;
                } else if (!strcmp (key, "amd-evicted-vram")) {
                    out->evicted_kb = parse_mem_kb (val);
                }
            }
        }
        if (!eol)
            break;
        line = eol + 1;
    }
    return out->driver[0] != 0;
}

/* the client's memory in one number: resident where the driver says it,
 * else what it allocated; -1 = no memory keys at all */
static gint64 fdinfo_mem_kb (const WalpeGpuFdinfo *f, gboolean *is_total)
{
    gint64 res = 0, tot = 0;
    gboolean have_res = FALSE, have_tot = FALSE;
    for (guint i = 0; i < f->n_regions; i++) {
        if (f->regions[i].resident_kb >= 0) {
            res += f->regions[i].resident_kb;
            have_res = TRUE;
        }
        if (f->regions[i].total_kb >= 0) {
            tot += f->regions[i].total_kb;
            have_tot = TRUE;
        }
    }
    if (is_total)
        *is_total = !have_res && have_tot;
    return have_res ? res : have_tot ? tot : -1;
}

/* ======================================================= 3. devices */

typedef struct {
    char card[16], render[16], slot[24], driver[32];
    guint vendor, device;
    gboolean boot_vga;
    gboolean pseudo;              /* known from fdinfo only: nothing in sysfs matched */
    char *dev_path;               /* the device's sysfs directory, NULL = none */
    char *card_path;              /* <sys>/class/drm/cardN, NULL = no DRM node */
    char *hwmon_path;             /* its hwmon node, NULL = none / not looked for yet */
    gboolean hwmon_looked;
    /* device-level values over the run */
    Acc m[WALPE_GPU_N_METRICS];
    WalpeGpuSrc src[WALPE_GPU_N_METRICS];
    gboolean mem_is_total;
    guint64 energy_last;          /* hwmon energy counter (uJ) and when it was read */
    gint64 energy_t;
    gboolean energy_ok;
    guint suspended_ticks;        /* samples at which the device was powered down */
    /* nvidia-smi */
    char name[96], drv_version[32];
    char *nv_fields;              /* the --query-gpu fields this nvidia-smi knows, NULL = not probed */
    int nv_index;                 /* its index in nvidia-smi, -1 = unknown */
    gboolean nv_dead;             /* the tool does not answer: do not try again */
} Dev;

static void dev_free (Dev *d)
{
    if (!d)
        return;
    g_free (d->dev_path);
    g_free (d->card_path);
    g_free (d->hwmon_path);
    g_free (d->nv_fields);
    g_free (d);
}

/* fill slot / driver / ids from a device directory in sysfs */
static void dev_identity (Dev *d)
{
    char buf[256];
    guint64 v;
    const char *b = strrchr (d->dev_path, '/');
    b = b ? b + 1 : d->dev_path;
    if (is_pci_slot (b))
        g_strlcpy (d->slot, b, sizeof (d->slot));
    link_base (d->dev_path, "driver", d->driver, sizeof (d->driver));
    if (read_u64 (d->dev_path, "vendor", &v))
        d->vendor = (guint) (v & 0xffff);
    if (read_u64 (d->dev_path, "device", &v))
        d->device = (guint) (v & 0xffff);
    if (!d->driver[0] || !d->slot[0] || !d->vendor) {
        /* what the links and attributes did not tell (a tree of plain files,
         * a platform device) is in the uevent file:
         *   DRIVER=amdgpu   PCI_ID=1002:73DF   PCI_SLOT_NAME=0000:03:00.0 */
        char *path = g_build_filename (d->dev_path, "uevent", NULL);
        if (read_small (path, buf, sizeof (buf)) > 0) {
            char **lines = g_strsplit (buf, "\n", -1);
            for (char **l = lines; *l; l++) {
                g_strchomp (*l);
                if (g_str_has_prefix (*l, "DRIVER=") && !d->driver[0])
                    copy_name (d->driver, sizeof (d->driver), *l + 7);
                else if (g_str_has_prefix (*l, "PCI_SLOT_NAME=") && !d->slot[0] &&
                         is_pci_slot (*l + 14))
                    g_strlcpy (d->slot, *l + 14, sizeof (d->slot));
                else if (g_str_has_prefix (*l, "PCI_ID=") && !d->vendor) {
                    char *colon = strchr (*l + 7, ':');
                    if (colon) {
                        d->vendor = (guint) (g_ascii_strtoull (*l + 7, NULL, 16) & 0xffff);
                        d->device = (guint) (g_ascii_strtoull (colon + 1, NULL, 16) & 0xffff);
                    }
                }
            }
            g_strfreev (lines);
        }
        g_free (path);
    }
    for (char *c = d->slot; *c; c++)
        *c = g_ascii_tolower (*c);
    if (read_u64 (d->dev_path, "boot_vga", &v))
        d->boot_vga = v != 0;
    d->nv_index = -1;
}

static int dev_cmp (gconstpointer a, gconstpointer b)
{
    const Dev *x = *(Dev *const *) a, *y = *(Dev *const *) b;
    return g_strcmp0 (x->card[0] ? x->card : "~", y->card[0] ? y->card : "~");
}

/* every GPU the kernel lists: DRM cards first, then PCI display devices
 * that have no DRM node (an NVIDIA card whose driver registers none) */
static GPtrArray *devs_scan (const char *sys_root)
{
    GPtrArray *devs = g_ptr_array_new_with_free_func ((GDestroyNotify) dev_free);
    char *drm = g_build_filename (sys_root, "class", "drm", NULL);
    GDir *dir = g_dir_open (drm, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name (dir))) {
        /* "card0", not "card0-DP-1" (a connector) */
        if (!g_str_has_prefix (name, "card") || !g_ascii_isdigit (name[4]) ||
            strlen (name) >= sizeof (((Dev *) 0)->card))
            continue;
        const char *p = name + 4;
        while (g_ascii_isdigit (*p))
            p++;
        if (*p)
            continue;
        Dev *d = g_new0 (Dev, 1);
        g_strlcpy (d->card, name, sizeof (d->card));
        d->card_path = g_build_filename (drm, name, NULL);
        char *link = g_build_filename (d->card_path, "device", NULL);
        d->dev_path = realpath (link, NULL);   /* malloc: g_free is free in GLib >= 2.46 */
        if (!d->dev_path)
            d->dev_path = g_strdup (link);
        g_free (link);
        dev_identity (d);
        /* its render node: <device>/drm/renderD* */
        char *sub = g_build_filename (d->dev_path, "drm", NULL);
        GDir *sd = g_dir_open (sub, 0, NULL);
        const char *n2;
        while (sd && (n2 = g_dir_read_name (sd)))
            if (g_str_has_prefix (n2, "renderD") && strlen (n2) < sizeof (d->render)) {
                g_strlcpy (d->render, n2, sizeof (d->render));
                break;
            }
        if (sd)
            g_dir_close (sd);
        g_free (sub);
        g_ptr_array_add (devs, d);
    }
    if (dir)
        g_dir_close (dir);
    g_free (drm);
    g_ptr_array_sort (devs, dev_cmp);

    char *pci = g_build_filename (sys_root, "bus", "pci", "devices", NULL);
    dir = g_dir_open (pci, 0, NULL);
    while (dir && (name = g_dir_read_name (dir))) {
        if (!is_pci_slot (name))
            continue;
        gboolean known = FALSE;
        for (guint i = 0; i < devs->len && !known; i++)
            known = !strcmp (((Dev *) g_ptr_array_index (devs, i))->slot, name);
        if (known)
            continue;
        char *path = g_build_filename (pci, name, NULL);
        guint64 cls = 0;
        if (read_u64 (path, "class", &cls) && (cls >> 16) == 0x03) {
            Dev *d = g_new0 (Dev, 1);
            d->dev_path = realpath (path, NULL);
            if (!d->dev_path)
                d->dev_path = g_strdup (path);
            dev_identity (d);
            if (!d->slot[0])
                g_strlcpy (d->slot, name, sizeof (d->slot));
            g_ptr_array_add (devs, d);
        }
        g_free (path);
    }
    if (dir)
        g_dir_close (dir);
    g_free (pci);
    return devs;
}

/* "card1", "/dev/dri/card1", "renderD129", "/dev/dri/renderD129" or a PCI
 * address -> the device, NULL when nothing matches */
static Dev *devs_find_named (GPtrArray *devs, const char *spec)
{
    if (!spec || !*spec)
        return NULL;
    const char *b = strrchr (spec, '/');
    b = b ? b + 1 : spec;
    for (guint i = 0; i < devs->len; i++) {
        Dev *d = g_ptr_array_index (devs, i);
        if ((d->card[0] && !strcmp (d->card, b)) || (d->render[0] && !strcmp (d->render, b)) ||
            (d->slot[0] && !g_ascii_strcasecmp (d->slot, b)))
            return d;
    }
    return NULL;
}

/* the device a client key belongs to: its PCI address, or - a platform
 * device has none - the first device with that driver */
static Dev *devs_find_key (GPtrArray *devs, const char *pdev, const char *driver)
{
    for (guint i = 0; i < devs->len; i++) {
        Dev *d = g_ptr_array_index (devs, i);
        if (pdev[0] ? !g_ascii_strcasecmp (d->slot, pdev)
                    : (!d->slot[0] && d->driver[0] && !strcmp (d->driver, driver)))
            return d;
    }
    if (!pdev[0])
        for (guint i = 0; i < devs->len; i++) {
            Dev *d = g_ptr_array_index (devs, i);
            if (d->driver[0] && !strcmp (d->driver, driver))
                return d;
        }
    return NULL;
}

/* ====================================== 4. device-level sources */

/* A device that is powered down (runtime suspended: the second GPU of a
 * laptop) must not be woken by a statistics reader: some of its attributes
 * resume it when read. Its power state can be read without that. */
static gboolean dev_is_suspended (const Dev *d)
{
    char buf[32];
    if (!d->dev_path)
        return FALSE;
    char *path = g_build_filename (d->dev_path, "power", "runtime_status", NULL);
    gssize n = read_small (path, buf, sizeof (buf));
    g_free (path);
    return n > 0 && g_str_has_prefix (buf, "suspend");   /* suspended, suspending */
}

static void dev_value (Dev *d, int metric, WalpeGpuSrc src, double v)
{
    acc_add (&d->m[metric], v);
    d->src[metric] = src;
}

/* amdgpu: pp_dpm_sclk lists the core clock levels, the one in use marked
 * with "*":   0: 500Mhz        1: 2575Mhz *  */
static void amd_dpm_clocks (const char *dev_path, double *max_mhz)
{
    char buf[1024];
    *max_mhz = 0;
    char *path = g_build_filename (dev_path, "pp_dpm_sclk", NULL);
    gssize n = read_small (path, buf, sizeof (buf));
    g_free (path);
    if (n <= 0)
        return;
    char **lines = g_strsplit (buf, "\n", -1);
    for (char **l = lines; *l; l++) {
        const char *colon = strchr (*l, ':');
        if (!colon)
            continue;
        double mhz = g_ascii_strtod (colon + 1, NULL);
        if (mhz > *max_mhz && mhz < 100000)
            *max_mhz = mhz;
    }
    g_strfreev (lines);
}

static void dev_find_hwmon (Dev *d)
{
    if (d->hwmon_looked || !d->dev_path)
        return;
    d->hwmon_looked = TRUE;
    char *dir = g_build_filename (d->dev_path, "hwmon", NULL);
    GDir *g = g_dir_open (dir, 0, NULL);
    const char *name;
    while (g && (name = g_dir_read_name (g)))
        if (g_str_has_prefix (name, "hwmon")) {
            d->hwmon_path = g_build_filename (dir, name, NULL);
            break;
        }
    if (g)
        g_dir_close (g);
    g_free (dir);
}

/* one reading of the sysfs attributes of the device. now_ns: the sample's time. */
static void dev_sample_sysfs (Dev *d, gint64 now_ns)
{
    guint64 v;
    if (!d->dev_path || d->pseudo)
        return;
    if (dev_is_suspended (d)) {
        d->suspended_ticks++;
        d->energy_ok = FALSE;
        return;
    }
    gboolean amd = !strcmp (d->driver, "amdgpu") || !strcmp (d->driver, "radeon");
    gboolean intel = !strcmp (d->driver, "i915") || !strcmp (d->driver, "xe");
    if (amd) {
        if (read_u64 (d->dev_path, "gpu_busy_percent", &v) && v <= 100)
            dev_value (d, WALPE_GPU_M_BUSY, WALPE_GPU_SRC_AMD_SYSFS, (double) v);
        if (read_u64 (d->dev_path, "mem_busy_percent", &v) && v <= 100)
            dev_value (d, WALPE_GPU_M_MEM_BUSY, WALPE_GPU_SRC_AMD_SYSFS, (double) v);
        if (read_u64 (d->dev_path, "mem_info_vram_used", &v))
            dev_value (d, WALPE_GPU_M_VRAM_USED, WALPE_GPU_SRC_AMD_SYSFS, v / 1048576.0);
        if (read_u64 (d->dev_path, "mem_info_vram_total", &v))
            dev_value (d, WALPE_GPU_M_VRAM_TOTAL, WALPE_GPU_SRC_AMD_SYSFS, v / 1048576.0);
        if (read_u64 (d->dev_path, "mem_info_gtt_used", &v))
            dev_value (d, WALPE_GPU_M_GTT_USED, WALPE_GPU_SRC_AMD_SYSFS, v / 1048576.0);
        if (read_u64 (d->dev_path, "mem_info_gtt_total", &v))
            dev_value (d, WALPE_GPU_M_GTT_TOTAL, WALPE_GPU_SRC_AMD_SYSFS, v / 1048576.0);
        double max_mhz;
        amd_dpm_clocks (d->dev_path, &max_mhz);
        if (max_mhz > 0)
            dev_value (d, WALPE_GPU_M_CLOCK_MAX, WALPE_GPU_SRC_AMD_SYSFS, max_mhz);
    }
    if (intel) {
        /* MHz. The clock reads 0 while the GPU sleeps (RC6). i915 has the
         * attributes on the card, xe below the device. */
        gboolean got = FALSE;
        if (d->card_path && read_u64 (d->card_path, "gt_act_freq_mhz", &v)) {
            dev_value (d, WALPE_GPU_M_CLOCK, WALPE_GPU_SRC_INTEL_SYSFS, (double) v);
            got = TRUE;
            if (read_u64 (d->card_path, "gt_RP0_freq_mhz", &v) ||
                read_u64 (d->card_path, "gt_max_freq_mhz", &v))
                dev_value (d, WALPE_GPU_M_CLOCK_MAX, WALPE_GPU_SRC_INTEL_SYSFS, (double) v);
        }
        if (!got) {
            char *fq = g_build_filename (d->dev_path, "tile0", "gt0", "freq0", NULL);
            if (read_u64 (fq, "act_freq", &v)) {
                dev_value (d, WALPE_GPU_M_CLOCK, WALPE_GPU_SRC_INTEL_SYSFS, (double) v);
                if (read_u64 (fq, "rp0_freq", &v) || read_u64 (fq, "max_freq", &v))
                    dev_value (d, WALPE_GPU_M_CLOCK_MAX, WALPE_GPU_SRC_INTEL_SYSFS, (double) v);
            }
            g_free (fq);
        }
    }
    /* hwmon, whatever the driver: power in uW (an average or a reading), or
     * an energy counter in uJ; temperature in millidegrees; clocks in Hz */
    dev_find_hwmon (d);
    if (d->hwmon_path) {
        if (read_u64 (d->hwmon_path, "power1_average", &v) ||
            read_u64 (d->hwmon_path, "power1_input", &v)) {
            dev_value (d, WALPE_GPU_M_POWER, WALPE_GPU_SRC_HWMON, v / 1e6);
        } else if (read_u64 (d->hwmon_path, "energy1_input", &v)) {
            if (d->energy_ok && v >= d->energy_last && now_ns > d->energy_t)
                dev_value (d, WALPE_GPU_M_POWER, WALPE_GPU_SRC_HWMON,
                           (double) (v - d->energy_last) / 1e6 / ((now_ns - d->energy_t) / 1e9));
            d->energy_last = v;
            d->energy_t = now_ns;
            d->energy_ok = TRUE;
        }
        if (read_u64 (d->hwmon_path, "temp1_input", &v) ||
            read_u64 (d->hwmon_path, "temp2_input", &v))
            dev_value (d, WALPE_GPU_M_TEMP, WALPE_GPU_SRC_HWMON, v / 1000.0);
        if (read_u64 (d->hwmon_path, "freq1_input", &v) && d->src[WALPE_GPU_M_CLOCK] != WALPE_GPU_SRC_INTEL_SYSFS)
            dev_value (d, WALPE_GPU_M_CLOCK, WALPE_GPU_SRC_HWMON, v / 1e6);
        if (read_u64 (d->hwmon_path, "freq2_input", &v))
            dev_value (d, WALPE_GPU_M_MEM_CLOCK, WALPE_GPU_SRC_HWMON, v / 1e6);
    }
}

/* ---- nvidia-smi (only with WALPE_GPUSTAT_SPAWN) ---- */

/* run a program (argv, PATH is searched), give back its standard output or
 * NULL: it could not be started, ended with an error, or did not end
 * within timeout_ms (it is killed then). Its standard error is dropped. */
static char *run_tool (const char *const *argv, int timeout_ms)
{
    GPid pid = 0;
    int out_fd = -1;
    if (!g_spawn_async_with_pipes (NULL, (char **) argv, NULL,
                                   G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                   G_SPAWN_STDERR_TO_DEV_NULL,
                                   NULL, NULL, &pid, NULL, &out_fd, NULL, NULL))
        return NULL;
    GString *out = g_string_new (NULL);
    gint64 deadline = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
    gboolean timed_out = FALSE;
    for (;;) {
        gint64 left = (deadline - g_get_monotonic_time ()) / 1000;
        if (left <= 0) {
            timed_out = TRUE;
            break;
        }
        struct pollfd pfd = { .fd = out_fd, .events = POLLIN };
        int pr = poll (&pfd, 1, (int) MIN (left, 1000));
        if (pr < 0 && errno != EINTR)
            break;
        if (pr <= 0)
            continue;
        char buf[4096];
        gssize n = read (out_fd, buf, sizeof (buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        g_string_append_len (out, buf, n);
        if (out->len > (1u << 20))
            break;   /* not a statistics tool's answer */
    }
    close (out_fd);
    /* The pipe is closed: the output ended, was too much, or the time is
     * up. A program that goes on without its output (it closed it itself,
     * or does not mind the closed pipe) is waited for until the deadline
     * and no longer. */
    int status = 0;
    while (!timed_out) {
        pid_t w = waitpid (pid, &status, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR))
            break;
        if (g_get_monotonic_time () >= deadline)
            timed_out = TRUE;
        else
            g_usleep (2000);
    }
    if (timed_out) {
        kill (pid, SIGKILL);
        while (waitpid (pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    g_spawn_close_pid (pid);
    if (timed_out || !WIFEXITED (status) || WEXITSTATUS (status) != 0) {
        g_string_free (out, TRUE);
        return NULL;
    }
    return g_string_free (out, FALSE);
}

static const char *const nv_all_fields[] = {
    "utilization.gpu", "utilization.memory", "memory.used", "memory.total", "power.draw",
    "temperature.gpu", "clocks.gr", "clocks.mem", "clocks.max.gr", NULL
};

static char *nv_query (const char *tool, const char *fields)
{
    char *q = g_strconcat ("--query-gpu=", fields, NULL);
    const char *argv[] = { tool, q, "--format=csv,noheader,nounits", NULL };
    char *out = run_tool (argv, 4000);
    g_free (q);
    return out;
}

/* "00000000:01:00.0" (nvidia-smi) against "0000:01:00.0" (sysfs) */
static gboolean nv_same_slot (const char *bus_id, const char *slot)
{
    gsize a = strlen (bus_id), b = strlen (slot);
    if (a < 7 || b < 7)
        return FALSE;
    return !g_ascii_strcasecmp (bus_id + a - 7, slot + b - 7);   /* bb:dd.f */
}

/* first use: which GPU of nvidia-smi this device is, its name, and which
 * fields this version of the tool knows */
static void nv_probe (Dev *d, const char *tool)
{
    char *out = nv_query (tool, "index,pci.bus_id,name,driver_version");
    if (!out) {
        /* an older tool without pci.bus_id: its first GPU */
        out = nv_query (tool, "index,name,driver_version");
        if (!out) {
            d->nv_dead = TRUE;
            return;
        }
    }
    char **lines = g_strsplit (out, "\n", -1);
    for (char **l = lines; *l; l++) {
        char **f = g_strsplit (*l, ",", -1);
        guint nf = g_strv_length (f);
        for (guint i = 0; i < nf; i++)
            g_strstrip (f[i]);
        gboolean with_bus = nf >= 4;
        if (nf >= 3 && g_ascii_isdigit (f[0][0]) &&
            (d->nv_index < 0 || (with_bus && d->slot[0] && nv_same_slot (f[1], d->slot)))) {
            d->nv_index = atoi (f[0]);
            copy_name (d->name, sizeof (d->name), f[with_bus ? 2 : 1]);
            copy_name (d->drv_version, sizeof (d->drv_version), f[with_bus ? 3 : 2]);
        }
        g_strfreev (f);
    }
    g_strfreev (lines);
    g_free (out);
    if (d->nv_index < 0) {
        d->nv_dead = TRUE;
        return;
    }
    GString *ok = g_string_new (NULL);
    for (int i = 0; nv_all_fields[i]; i++) {
        char *o = nv_query (tool, nv_all_fields[i]);
        if (o) {
            g_string_append_printf (ok, "%s%s", ok->len ? "," : "", nv_all_fields[i]);
            g_free (o);
        }
    }
    d->nv_fields = g_string_free (ok, FALSE);
}

/* the n-th non-empty line of a text (0-based), g_free */
static char *nth_line (const char *text, int n)
{
    char **lines = g_strsplit (text, "\n", -1);
    char *found = NULL;
    for (char **l = lines; *l; l++) {
        if (!*g_strstrip (*l))
            continue;
        if (n-- == 0) {
            found = g_strdup (*l);
            break;
        }
    }
    g_strfreev (lines);
    return found;
}

typedef struct {
    int pid;
    char name[32];
    double sm;          /* GPU share %, < 0 = the driver has no figure */
    double fb_mb;       /* GPU memory MB, < 0 = no figure */
} NvProc;

/* one sample of the whole GPU and, when procs is given, of its programs
 * (nvidia-smi pmon: columns are found by the header line) */
static void dev_sample_nvidia (Dev *d, const char *tool, GArray *procs)
{
    if (d->nv_dead)
        return;
    if (!d->nv_fields)
        nv_probe (d, tool);
    if (d->nv_dead || !d->nv_fields || !*d->nv_fields)
        return;
    char *out = nv_query (tool, d->nv_fields);
    char *line = out ? nth_line (out, d->nv_index) : NULL;
    g_free (out);
    if (line) {
        char **names = g_strsplit (d->nv_fields, ",", -1);
        char **vals = g_strsplit (line, ",", -1);
        for (guint i = 0; names[i] && vals[i]; i++) {
            const char *v = g_strstrip (vals[i]);
            if (!g_ascii_isdigit (*v))
                continue;   /* "[N/A]", "[Not Supported]" */
            double x = g_ascii_strtod (v, NULL);
            int metric = -1;
            if (!strcmp (names[i], "utilization.gpu")) metric = WALPE_GPU_M_BUSY;
            else if (!strcmp (names[i], "utilization.memory")) metric = WALPE_GPU_M_MEM_BUSY;
            else if (!strcmp (names[i], "memory.used")) metric = WALPE_GPU_M_VRAM_USED;
            else if (!strcmp (names[i], "memory.total")) metric = WALPE_GPU_M_VRAM_TOTAL;
            else if (!strcmp (names[i], "power.draw")) metric = WALPE_GPU_M_POWER;
            else if (!strcmp (names[i], "temperature.gpu")) metric = WALPE_GPU_M_TEMP;
            else if (!strcmp (names[i], "clocks.gr")) metric = WALPE_GPU_M_CLOCK;
            else if (!strcmp (names[i], "clocks.mem")) metric = WALPE_GPU_M_MEM_CLOCK;
            else if (!strcmp (names[i], "clocks.max.gr")) metric = WALPE_GPU_M_CLOCK_MAX;
            if (metric >= 0)
                dev_value (d, metric, WALPE_GPU_SRC_NVIDIA_SMI, x);
        }
        g_strfreev (names);
        g_strfreev (vals);
        g_free (line);
    }
    if (!procs)
        return;
    const char *argv[] = { tool, "pmon", "-c", "1", "-s", "um", NULL };
    out = run_tool (argv, 5000);
    if (!out)
        return;
    int c_gpu = -1, c_pid = -1, c_sm = -1, c_fb = -1, c_cmd = -1;
    char **lines = g_strsplit (out, "\n", -1);
    for (char **l = lines; *l; l++) {
        char **f = g_strsplit_set (*l, " \t", -1);
        GPtrArray *tok = g_ptr_array_new ();
        for (char **t = f; *t; t++)
            if (**t)
                g_ptr_array_add (tok, *t);
        if (tok->len >= 2 && !strcmp (g_ptr_array_index (tok, 0), "#") &&
            !strcmp (g_ptr_array_index (tok, 1), "gpu")) {
            for (guint i = 1; i < tok->len; i++) {
                const char *t = g_ptr_array_index (tok, i);
                int col = (int) i - 1;
                if (!strcmp (t, "gpu")) c_gpu = col;
                else if (!strcmp (t, "pid")) c_pid = col;
                else if (!strcmp (t, "sm")) c_sm = col;
                else if (!strcmp (t, "fb")) c_fb = col;
                else if (!strcmp (t, "command")) c_cmd = col;
            }
        } else if (tok->len && ((const char *) g_ptr_array_index (tok, 0))[0] != '#' && c_pid >= 0 &&
                   (guint) c_pid < tok->len) {
            const char *sp = g_ptr_array_index (tok, c_pid);
            const char *sg = c_gpu >= 0 && (guint) c_gpu < tok->len ? g_ptr_array_index (tok, c_gpu) : "0";
            if (g_ascii_isdigit (*sp) && atoi (sg) == d->nv_index) {
                NvProc p = { .pid = atoi (sp), .sm = -1, .fb_mb = -1 };
                if (c_sm >= 0 && (guint) c_sm < tok->len &&
                    g_ascii_isdigit (*(const char *) g_ptr_array_index (tok, c_sm)))
                    p.sm = g_ascii_strtod (g_ptr_array_index (tok, c_sm), NULL);
                if (c_fb >= 0 && (guint) c_fb < tok->len &&
                    g_ascii_isdigit (*(const char *) g_ptr_array_index (tok, c_fb)))
                    p.fb_mb = g_ascii_strtod (g_ptr_array_index (tok, c_fb), NULL);
                if (c_cmd >= 0 && (guint) c_cmd < tok->len)
                    copy_name (p.name, sizeof (p.name), g_ptr_array_index (tok, c_cmd));
                g_array_append_val (procs, p);
            }
        }
        g_ptr_array_free (tok, TRUE);
        g_strfreev (f);
    }
    g_strfreev (lines);
    g_free (out);
}

/* ======================================================= 5. the sampler */

typedef struct {
    char name[32];
    gboolean cycles;              /* cycles against total cycles, else ns against the clock */
    guint capacity;
    gboolean have_last;
    guint64 last;                 /* the largest value seen (ns or cycles): the baseline */
    guint64 last_total;           /* cycles mode: total cycles at that reading */
    guint below;                  /* samples in a row with a value under the baseline */
    gint64 held_ns;               /* wall time of those samples */
    guint64 acc;                  /* accounted since the (re)start */
    guint64 acc_den;              /* wall ns, or total cycles, of the counted intervals */
    double max_pct;               /* its busiest interval */
    guint resets;
    /* this sample */
    guint64 d, d_den;
    gboolean d_valid;
} CEngine;

typedef struct {
    char devkey[40];              /* PCI address, or "drv:<driver>" for a platform device */
    char pdev[24], driver[32];
    int pid;                      /* the process it is accounted to */
    gboolean has_id;
    GArray *engines;              /* CEngine */
    WalpeGpuFdRegion regions[WALPE_GPU_MAX_REGIONS];
    guint n_regions;
    gint64 mem_kb;                /* -1 = no memory keys */
    gboolean mem_is_total;
    gint64 evicted_kb;
    guint first_tick, seen_tick;
    gint64 seen_ns;
    gboolean shared_noted;
} Client;

typedef struct {
    int pid;
    char name[32];
    gboolean target;
    gboolean exited;              /* it was there and is gone */
    gboolean denied;              /* its fdinfo may not be read */
    guint seen_tick;
    GArray *drm_fds;              /* int: descriptors that were DRM clients at the last full listing */
} Proc;

typedef struct {                  /* one engine of one device: all clients, and the target's part */
    char devkey[40];
    char name[32];
    guint capacity;
    gboolean cycles;
    gboolean has_target;
    guint64 acc_t, acc_all, acc_den;
    guint64 acc_den_t;            /* acc_den of the samples at which the target was measured */
    double t_max;
    guint resets;
    /* this sample */
    guint64 d_t, d_all, d_den;
    gboolean d_any;               /* some client had a counted interval */
    gboolean d_t_any;             /* ... a client of the target */
    double p_t, p_all;            /* % */
} Agg;

typedef struct {                  /* one client as read at this sample, through one or more descriptors */
    WalpeGpuFdinfo fi;
    int pid;
    gboolean target;
    guint fds;
} Sight;

typedef struct {                  /* a program as nvidia-smi lists it */
    int pid;
    char name[32];
    Acc sm, fb;
    guint rows;
} NvAcc;

struct WalpeGpuSampler {
    char *proc_root, *sys_root, *device, *gl_vendor, *nvidia_smi;
    int target_pid;
    gboolean target_self;
    gboolean real_proc;           /* proc_root is the machine's /proc */
    guint flags;
    gint64 uid;
    guint rescan_every;
    GPtrArray *devs;              /* Dev */
    GHashTable *clients;          /* key -> Client */
    GHashTable *procs;            /* pid -> Proc */
    GArray *aggs;                 /* Agg */
    GHashTable *nv;               /* pid -> NvAcc */
    Dev *cur;                     /* the device the report is about, NULL = none known */
    char cur_how[64];
    guint tick;
    gint64 t_first, t_last;
    gboolean target_seen, target_gone, target_other_user;
    guint other_users, shared, no_id, resets;
    GHashTable *sight;            /* key -> Sight, during one sample */
};

static GMutex hint_lock;
static char *hint_device;         /* walpe_gpustat_set_render_device */
static char *hint_gl_vendor;      /* walpe_gpustat_set_gl_vendor */
static gint hint_gen;             /* atomic: counts the changes of the two (walpe_gpustat_self) */

void walpe_gpustat_set_gl_vendor (const char *gl_vendor)
{
    if (gl_vendor && !*gl_vendor)
        gl_vendor = NULL;
    g_mutex_lock (&hint_lock);
    if (g_strcmp0 (hint_gl_vendor, gl_vendor)) {
        g_free (hint_gl_vendor);
        hint_gl_vendor = g_strdup (gl_vendor);
        g_atomic_int_inc (&hint_gen);
    }
    g_mutex_unlock (&hint_lock);
}

void walpe_gpustat_set_render_device (const char *node_or_pci_slot)
{
    if (node_or_pci_slot && !*node_or_pci_slot)
        node_or_pci_slot = NULL;
    g_mutex_lock (&hint_lock);
    if (g_strcmp0 (hint_device, node_or_pci_slot)) {
        g_free (hint_device);
        hint_device = g_strdup (node_or_pci_slot);
        g_atomic_int_inc (&hint_gen);
    }
    g_mutex_unlock (&hint_lock);
}

static Dev *devs_guess (GPtrArray *devs, const char *glv, const char **how);

gboolean walpe_gpustat_render_driver (const char *sys_root, char *driver, gsize size)
{
    if (!driver || !size)
        return FALSE;
    driver[0] = 0;
    g_mutex_lock (&hint_lock);
    char *hint = g_strdup (hint_device), *glv = g_strdup (hint_gl_vendor);
    g_mutex_unlock (&hint_lock);
    GPtrArray *devs = devs_scan (sys_root && *sys_root ? sys_root : "/sys");
    const char *how = "";
    Dev *d = devs_find_named (devs, hint);
    if (!d)
        d = devs_guess (devs, glv, &how);
    if (d && d->driver[0])
        g_strlcpy (driver, d->driver, size);
    else if (glv && strstr (glv, "NVIDIA") && (!d || d->vendor == 0x10de))
        /* NVIDIA's OpenGL draws on NVIDIA's kernel driver and no other,
         * also where sysfs does not show the device or its driver */
        g_strlcpy (driver, "nvidia", size);
    g_ptr_array_unref (devs);
    g_free (hint);
    g_free (glv);
    return driver[0] != 0;
}

WalpeGpuProfileKernel walpe_gpustat_profile_kernel (const char *setting, const char *sys_root)
{
    if (setting && *setting) {
        if (*setting == '0' || !g_ascii_strcasecmp (setting, "off") ||
            !g_ascii_strcasecmp (setting, "no"))
            return WALPE_GPU_PROFILE_KERNEL_OFF;
        return !g_ascii_strcasecmp (setting, "self") ? WALPE_GPU_PROFILE_KERNEL_SELF
                                                     : WALPE_GPU_PROFILE_KERNEL_ALL;
    }
    /* Not set. NVIDIA's kernel driver gives these interfaces no figure, and
     * a profile on it stays what it was before this layer existed: nothing
     * is read and the report has no section of ours. */
    char drv[32];
    if (walpe_gpustat_render_driver (sys_root, drv, sizeof (drv)) && !strcmp (drv, "nvidia"))
        return WALPE_GPU_PROFILE_KERNEL_OFF;
    return WALPE_GPU_PROFILE_KERNEL_ALL;
}

static void client_free (gpointer p)
{
    Client *c = p;
    g_array_unref (c->engines);
    g_free (c);
}

static void proc_free (gpointer p)
{
    Proc *x = p;
    g_array_unref (x->drm_fds);
    g_free (x);
}

WalpeGpuSampler *walpe_gpu_sampler_new (const WalpeGpuStatOpts *opts)
{
    static const WalpeGpuStatOpts none = { .target_pid = -1, .uid = -1 };
    if (!opts)
        opts = &none;
    WalpeGpuSampler *s = g_new0 (WalpeGpuSampler, 1);
    s->proc_root = g_strdup (opts->proc_root && *opts->proc_root ? opts->proc_root : "/proc");
    s->sys_root = g_strdup (opts->sys_root && *opts->sys_root ? opts->sys_root : "/sys");
    s->real_proc = !strcmp (s->proc_root, "/proc");
    s->device = g_strdup (opts->device && *opts->device ? opts->device : NULL);
    s->gl_vendor = g_strdup (opts->gl_vendor && *opts->gl_vendor ? opts->gl_vendor : NULL);
    s->nvidia_smi = g_strdup (opts->nvidia_smi && *opts->nvidia_smi ? opts->nvidia_smi : "nvidia-smi");
    s->target_self = opts->target_pid < 0;
    s->target_pid = s->target_self ? (int) getpid () : opts->target_pid;
    s->flags = opts->flags;
    s->uid = opts->uid < 0 ? (gint64) getuid () : opts->uid;
    s->rescan_every = opts->rescan_every ? opts->rescan_every : 5;
    s->devs = devs_scan (s->sys_root);
    s->clients = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, client_free);
    s->procs = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, proc_free);
    s->aggs = g_array_new (FALSE, TRUE, sizeof (Agg));
    s->nv = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
    return s;
}

void walpe_gpu_sampler_free (WalpeGpuSampler *s)
{
    if (!s)
        return;
    g_hash_table_destroy (s->clients);
    g_hash_table_destroy (s->procs);
    g_hash_table_destroy (s->nv);
    g_array_unref (s->aggs);
    g_ptr_array_unref (s->devs);
    g_free (s->proc_root);
    g_free (s->sys_root);
    g_free (s->device);
    g_free (s->gl_vendor);
    g_free (s->nvidia_smi);
    g_free (s);
}

void walpe_gpu_sampler_restart (WalpeGpuSampler *s)
{
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init (&it, s->clients);
    while (g_hash_table_iter_next (&it, NULL, &v)) {
        Client *c = v;
        /* a client that is gone has nothing more to say */
        if (c->seen_tick != s->tick) {
            g_hash_table_iter_remove (&it);
            continue;
        }
        for (guint i = 0; i < c->engines->len; i++) {
            CEngine *e = &g_array_index (c->engines, CEngine, i);
            e->acc = e->acc_den = 0;
            e->max_pct = 0;
            e->resets = 0;
        }
    }
    g_hash_table_iter_init (&it, s->procs);
    while (g_hash_table_iter_next (&it, NULL, &v))
        if (((Proc *) v)->exited)
            g_hash_table_iter_remove (&it);
    g_array_set_size (s->aggs, 0);
    g_hash_table_remove_all (s->nv);
    for (guint i = 0; i < s->devs->len; i++) {
        Dev *d = g_ptr_array_index (s->devs, i);
        memset (d->m, 0, sizeof (d->m));
        d->suspended_ticks = 0;
    }
    s->shared = s->no_id = s->resets = 0;
    s->other_users = 0;
    s->t_first = s->t_last;
    s->tick = s->tick ? 1 : 0;
    /* the clients that stay were seen at "sample 1" of the new run */
    g_hash_table_iter_init (&it, s->clients);
    while (g_hash_table_iter_next (&it, NULL, &v)) {
        ((Client *) v)->seen_tick = s->tick;
        ((Client *) v)->first_tick = s->tick;
    }
    g_hash_table_iter_init (&it, s->procs);
    while (g_hash_table_iter_next (&it, NULL, &v))
        ((Proc *) v)->seen_tick = s->tick;
}

static Proc *proc_get (WalpeGpuSampler *s, int pid, gboolean create)
{
    Proc *p = g_hash_table_lookup (s->procs, GINT_TO_POINTER (pid));
    if (p || !create || g_hash_table_size (s->procs) >= MAX_PROCS)
        return p;
    p = g_new0 (Proc, 1);
    p->pid = pid;
    p->target = pid == s->target_pid;
    p->drm_fds = g_array_new (FALSE, FALSE, sizeof (int));
    g_hash_table_insert (s->procs, GINT_TO_POINTER (pid), p);
    return p;
}

/* whose process is it? -1 = it is not there (any more) */
static gint64 proc_uid (WalpeGpuSampler *s, int pid)
{
    char path[PATH_MAX];
    struct stat st;
    g_snprintf (path, sizeof (path), "%s/%d", s->proc_root, pid);
    if (stat (path, &st) != 0 || !S_ISDIR (st.st_mode))
        return -1;
    return (gint64) st.st_uid;
}

/* all of /proc: the processes of this user become known, the others are
 * only counted (their uid comes from the directory, nothing in it is read) */
static void walk_procs (WalpeGpuSampler *s)
{
    GDir *dir = g_dir_open (s->proc_root, 0, NULL);
    if (!dir)
        return;
    guint others = 0;
    int self = s->real_proc ? (int) getpid () : -1;
    const char *name;
    while ((name = g_dir_read_name (dir))) {
        if (!g_ascii_isdigit (name[0]))
            continue;
        char *end = NULL;
        long pid = strtol (name, &end, 10);
        if (*end || pid <= 0 || pid > G_MAXINT)
            continue;
        if (pid == s->target_pid || pid == self)
            continue;
        if (g_hash_table_contains (s->procs, GINT_TO_POINTER ((int) pid)))
            continue;
        gint64 uid = proc_uid (s, (int) pid);
        if (uid < 0)
            continue;
        if (uid != s->uid) {
            others++;
            continue;
        }
        proc_get (s, (int) pid, TRUE);
    }
    g_dir_close (dir);
    s->other_users = MAX (s->other_users, others);
}

/* one descriptor's fdinfo: when it is a DRM client it joins this sample's
 * sightings. Returns whether it was one. */
static gboolean read_fd (WalpeGpuSampler *s, Proc *p, int dfd, const char *fdname)
{
    char buf[FDINFO_MAX];
    gssize len = read_small_at (dfd, fdname, buf, sizeof (buf));
    if (len <= 0)
        return FALSE;
    if ((gsize) len >= sizeof (buf) - 1) {
        /* longer than the buffer: the text ends somewhere inside a line,
         * and a number cut short is a smaller number. That line goes. */
        char *nl = strrchr (buf, '\n');
        if (nl)
            nl[1] = 0;
        else
            return FALSE;
    }
    if (!strstr (buf, "drm-driver:"))
        return FALSE;
    WalpeGpuFdinfo fi;
    if (!walpe_gpu_fdinfo_parse (buf, &fi))
        return FALSE;
    char key[128], devkey[40];
    if (fi.pdev[0]) {
        g_strlcpy (devkey, fi.pdev, sizeof (devkey));
        for (char *c = devkey; *c; c++)
            *c = g_ascii_tolower (*c);
    } else {
        g_snprintf (devkey, sizeof (devkey), "drv:%s", fi.driver);
    }
    if (fi.has_client_id)
        g_snprintf (key, sizeof (key), "%s#%" G_GUINT64_FORMAT, devkey, fi.client_id);
    else   /* no id: every descriptor is taken as a client of its own */
        g_snprintf (key, sizeof (key), "%s#p%d.%s", devkey, p->pid, fdname);
    Sight *g = g_hash_table_lookup (s->sight, key);
    if (!g) {
        g = g_new0 (Sight, 1);
        g->fi = fi;
        g->pid = p->pid;
        g->target = p->target;
        g->fds = 1;
        g_hash_table_insert (s->sight, g_strdup (key), g);
        return TRUE;
    }
    /* the same client through another descriptor (a copy in this process,
     * or one passed to another process): counted once. The counters were
     * read a moment apart: the later, larger reading is kept. */
    g->fds++;
    if (p->target && !g->target) {
        g->target = TRUE;
        g->pid = p->pid;
    } else if (!g->target && p->pid < g->pid) {
        g->pid = p->pid;
    }
    for (guint i = 0; i < fi.n_engines; i++) {
        WalpeGpuFdEngine *n = &fi.engines[i], *o = fd_engine (&g->fi, n->name);
        if (!o)
            continue;
        if (n->has_ns && (!o->has_ns || n->ns > o->ns)) {
            o->ns = n->ns;
            o->has_ns = TRUE;
        }
        if (n->has_cycles && n->has_total_cycles &&
            (!o->has_total_cycles || n->total_cycles > o->total_cycles)) {
            o->cycles = n->cycles;
            o->total_cycles = n->total_cycles;
            o->has_cycles = o->has_total_cycles = TRUE;
        }
        if (n->capacity > o->capacity)
            o->capacity = n->capacity;
    }
    return TRUE;
}

/* read one process: its name and its DRM descriptors. list_all = look at
 * every descriptor (finds new clients); otherwise only at those that were
 * DRM clients at the last full listing. */
static void read_proc (WalpeGpuSampler *s, Proc *p, gboolean list_all)
{
    char path[PATH_MAX], buf[64];
    if (p->exited || p->denied)
        return;
    if (!p->name[0] || list_all) {
        g_snprintf (path, sizeof (path), "%s/%d/comm", s->proc_root, p->pid);
        if (read_small (path, buf, sizeof (buf)) > 0)
            copy_name (p->name, sizeof (p->name), buf);
    }
    g_snprintf (path, sizeof (path), "%s/%d/fdinfo", s->proc_root, p->pid);
    int dfd = open (path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        if (errno == EACCES || errno == EPERM)
            p->denied = TRUE;
        else if (p->seen_tick)   /* it was there before */
            p->exited = TRUE;
        return;
    }
    p->seen_tick = s->tick;
    if (!list_all) {
        for (guint i = 0; i < p->drm_fds->len; i++) {
            g_snprintf (buf, sizeof (buf), "%d", g_array_index (p->drm_fds, int, i));
            read_fd (s, p, dfd, buf);
        }
        close (dfd);
        return;
    }
    g_array_set_size (p->drm_fds, 0);
    DIR *dir = fdopendir (dfd);
    if (!dir) {
        close (dfd);
        return;
    }
    struct dirent *de;
    guint looked = 0;
    while ((de = readdir (dir)) && looked < MAX_FDS_PER_PROC) {
        if (!g_ascii_isdigit (de->d_name[0]))
            continue;
        looked++;
        if (read_fd (s, p, dirfd (dir), de->d_name)) {
            int fd = atoi (de->d_name);
            g_array_append_val (p->drm_fds, fd);
        }
    }
    closedir (dir);
}

static CEngine *client_engine (Client *c, const char *name)
{
    for (guint i = 0; i < c->engines->len; i++) {
        CEngine *e = &g_array_index (c->engines, CEngine, i);
        if (!strcmp (e->name, name))
            return e;
    }
    CEngine e = { 0 };
    g_strlcpy (e.name, name, sizeof (e.name));
    g_array_append_val (c->engines, e);
    return &g_array_index (c->engines, CEngine, c->engines->len - 1);
}

static Agg *agg_get (WalpeGpuSampler *s, const char *devkey, const char *name)
{
    for (guint i = 0; i < s->aggs->len; i++) {
        Agg *a = &g_array_index (s->aggs, Agg, i);
        if (!strcmp (a->devkey, devkey) && !strcmp (a->name, name))
            return a;
    }
    Agg a = { 0 };
    g_strlcpy (a.devkey, devkey, sizeof (a.devkey));
    g_strlcpy (a.name, name, sizeof (a.name));
    a.capacity = 1;
    g_array_append_val (s->aggs, a);
    return &g_array_index (s->aggs, Agg, s->aggs->len - 1);
}

/* One engine counter of one client at this sample: what it adds, by the
 * kernel's rule for readers - a value lower than the one before is not a
 * step back in time, the reader stays with the larger value until the
 * counter has caught up. A counter that stays below for HOLD_SAMPLES
 * samples, or jumps by more than the engine can have worked in the time,
 * has restarted or wrapped: the baseline moves to the new value and the
 * interval is not counted ("reset"). */
static void engine_step (WalpeGpuSampler *s, CEngine *e, const WalpeGpuFdEngine *f, gint64 interval_ns)
{
    e->d = e->d_den = 0;
    e->d_valid = FALSE;
    gboolean cycles = !f->has_ns && f->has_cycles && f->has_total_cycles;
    if (!f->has_ns && !cycles)
        return;   /* nothing a share can be computed from */
    guint64 v = cycles ? f->cycles : f->ns;
    if (f->capacity > e->capacity)
        e->capacity = f->capacity;
    if (!e->capacity)
        e->capacity = 1;
    if (!e->have_last || e->cycles != cycles) {
        e->cycles = cycles;
        e->last = v;
        e->last_total = f->total_cycles;
        e->have_last = TRUE;
        e->below = 0;
        e->held_ns = 0;
        return;   /* the baseline: a share needs two readings */
    }
    if (interval_ns <= 0)
        return;
    gboolean reset = FALSE;
    if (cycles) {
        if (v >= e->last && f->total_cycles >= e->last_total) {
            guint64 dc = v - e->last, dt = f->total_cycles - e->last_total;
            if (!dt)
                return;   /* the engine's clock did not move: nothing to say */
            if ((long double) dc > (long double) dt * e->capacity * 1.25L) {
                reset = TRUE;
            } else {
                e->d = dc;
                e->d_den = dt;
                e->d_valid = TRUE;
                e->last = v;
                e->last_total = f->total_cycles;
                e->below = 0;
            }
        } else if (++e->below >= HOLD_SAMPLES) {
            reset = TRUE;
        }
    } else {
        if (v >= e->last) {
            guint64 d = v - e->last;
            long double limit = ((long double) interval_ns + e->held_ns) * e->capacity * 1.25L + 1e6L;
            if ((long double) d > limit) {
                reset = TRUE;
            } else {
                e->d = d;
                e->d_den = (guint64) interval_ns;
                e->d_valid = TRUE;
                e->last = v;
                e->below = 0;
                e->held_ns = 0;
            }
        } else if (++e->below >= HOLD_SAMPLES) {
            reset = TRUE;
        } else {
            /* stay with the larger value: no time is added, the interval counts */
            e->held_ns += interval_ns;
            e->d_den = (guint64) interval_ns;
            e->d_valid = TRUE;
        }
    }
    if (reset) {
        /* the samples it was held for said "no time": they said nothing */
        if (!cycles && e->held_ns > 0)
            e->acc_den -= MIN (e->acc_den, (guint64) e->held_ns);
        e->resets++;
        s->resets++;
        e->last = v;
        e->last_total = f->total_cycles;
        e->below = 0;
        e->held_ns = 0;
        return;
    }
    if (e->d_valid) {
        e->acc += e->d;
        e->acc_den += e->d_den;
        double pct = 100.0 * (double) e->d / (double) e->d_den / e->capacity;
        if (pct > e->max_pct)
            e->max_pct = MIN (pct, 100.0);
    }
}

/* this sample's sightings into the clients and the per-engine sums */
static void apply_sightings (WalpeGpuSampler *s, gint64 now)
{
    for (guint i = 0; i < s->aggs->len; i++) {
        Agg *a = &g_array_index (s->aggs, Agg, i);
        a->d_t = a->d_all = a->d_den = 0;
        a->d_any = a->d_t_any = FALSE;
        a->p_t = a->p_all = 0;
    }
    GHashTableIter it;
    gpointer k, v;
    if (g_hash_table_size (s->clients) >= MAX_CLIENTS) {
        /* far more clients than a desktop has (a program that opens the
         * device over and over during a long run): those that are gone are
         * forgotten, so that the ones that are there can be followed */
        g_hash_table_iter_init (&it, s->clients);
        while (g_hash_table_iter_next (&it, NULL, &v))
            if (((Client *) v)->seen_tick + 1 < s->tick)
                g_hash_table_iter_remove (&it);
    }
    g_hash_table_iter_init (&it, s->sight);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        const char *key = k;
        Sight *g = v;
        Client *c = g_hash_table_lookup (s->clients, key);
        if (!c) {
            if (g_hash_table_size (s->clients) >= MAX_CLIENTS)
                continue;   /* all of them alive: no room, not followed */
            c = g_new0 (Client, 1);
            const char *hash = strchr (key, '#');
            g_strlcpy (c->devkey, key, MIN (sizeof (c->devkey), (gsize) (hash - key) + 1));
            g_strlcpy (c->pdev, g->fi.pdev, sizeof (c->pdev));
            g_strlcpy (c->driver, g->fi.driver, sizeof (c->driver));
            c->has_id = g->fi.has_client_id;
            c->engines = g_array_new (FALSE, TRUE, sizeof (CEngine));
            c->first_tick = s->tick;
            c->pid = g->pid;
            if (!c->has_id)
                s->no_id++;
            g_hash_table_insert (s->clients, g_strdup (key), c);
        }
        /* whose it is at this sample: the target's when the target holds it
         * (also when another process holds a copy), else the lowest pid's */
        c->pid = g->pid;
        if (g->fds > 1 && !c->shared_noted) {
            c->shared_noted = TRUE;
            s->shared++;
        }
        gint64 interval = c->first_tick != s->tick ? now - c->seen_ns : 0;   /* 0: just met */
        gboolean is_target = c->pid == s->target_pid && s->target_pid > 0;
        for (guint i = 0; i < g->fi.n_engines; i++) {
            const WalpeGpuFdEngine *f = &g->fi.engines[i];
            if (!f->has_ns && !(f->has_cycles && f->has_total_cycles))
                continue;
            CEngine *e = client_engine (c, f->name);
            guint resets0 = e->resets;
            engine_step (s, e, f, interval);
            Agg *a = agg_get (s, c->devkey, f->name);
            a->cycles = e->cycles;
            if (e->capacity > a->capacity)
                a->capacity = e->capacity;
            if (is_target)
                a->has_target = TRUE;
            a->resets += e->resets - resets0;
            if (e->d_valid) {
                a->d_all += e->d;
                if (is_target) {
                    a->d_t += e->d;
                    a->d_t_any = TRUE;
                }
                if (e->d_den > a->d_den)
                    a->d_den = e->d_den;
                a->d_any = TRUE;
            }
        }
        memcpy (c->regions, g->fi.regions, sizeof (c->regions));
        c->n_regions = g->fi.n_regions;
        c->mem_kb = fdinfo_mem_kb (&g->fi, &c->mem_is_total);
        c->evicted_kb = g->fi.evicted_kb;
        c->seen_tick = s->tick;
        c->seen_ns = now;
    }
    for (guint i = 0; i < s->aggs->len; i++) {
        Agg *a = &g_array_index (s->aggs, Agg, i);
        if (!a->d_any || !a->d_den)
            continue;
        a->p_t = MIN (100.0, 100.0 * (double) a->d_t / (double) a->d_den / a->capacity);
        a->p_all = MIN (100.0, 100.0 * (double) a->d_all / (double) a->d_den / a->capacity);
        a->acc_t += a->d_t;
        a->acc_all += a->d_all;
        a->acc_den += a->d_den;
        if (a->d_t_any)
            a->acc_den_t += a->d_den;
        if (a->p_t > a->t_max)
            a->t_max = a->p_t;
    }
}

static void dev_key (const Dev *d, char *out, gsize size)
{
    if (d->slot[0]) {
        g_strlcpy (out, d->slot, size);
        for (char *c = out; *c; c++)
            *c = g_ascii_tolower (*c);
    } else {
        g_snprintf (out, size, "drv:%s", d->driver);
    }
}

/* the engine the busy rows are about: the one that draws (its name differs
 * by driver), else the one the visible clients used most */
static Agg *main_agg (WalpeGpuSampler *s, const char *devkey)
{
    static const char *const render[] = { "render", "rcs", "gfx", "gpu", "fragment", NULL };
    Agg *best = NULL;
    for (guint i = 0; i < s->aggs->len; i++) {
        Agg *a = &g_array_index (s->aggs, Agg, i);
        if (strcmp (a->devkey, devkey))
            continue;
        for (int k = 0; render[k]; k++)
            if (!strcmp (a->name, render[k]))
                return a;
        if (!best || a->acc_all > best->acc_all)
            best = a;
    }
    return best;
}

static Dev *dev_pseudo (WalpeGpuSampler *s, const char *pdev, const char *driver)
{
    Dev *d = g_new0 (Dev, 1);
    d->pseudo = TRUE;
    d->nv_index = -1;
    g_strlcpy (d->slot, pdev, sizeof (d->slot));
    g_strlcpy (d->driver, driver, sizeof (d->driver));
    if (!strcmp (driver, "amdgpu") || !strcmp (driver, "radeon"))
        d->vendor = 0x1002;
    else if (!strcmp (driver, "i915") || !strcmp (driver, "xe"))
        d->vendor = 0x8086;
    else if (!strcmp (driver, "nvidia") || !strcmp (driver, "nouveau"))
        d->vendor = 0x10de;
    g_ptr_array_add (s->devs, d);
    return d;
}

/* The device a process most likely renders on when its DRM clients do not
 * tell: the one its GL vendor names (asked only with two or more GPUs),
 * else the only GPU, the boot GPU, the first GPU. NULL = no GPU is listed.
 * *how is set when a device is returned. */
static Dev *devs_guess (GPtrArray *devs, const char *glv, const char **how)
{
    Dev *d = NULL;
    guint real = 0;
    for (guint i = 0; i < devs->len; i++)
        real += !((Dev *) g_ptr_array_index (devs, i))->pseudo;
    if (glv && real > 1) {
        guint want = strstr (glv, "NVIDIA") ? 0x10de
                     : strstr (glv, "Intel") ? 0x8086
                     : (strstr (glv, "AMD") || strstr (glv, "ATI")) ? 0x1002 : 0;
        for (guint i = 0; want && i < devs->len && !d; i++) {
            Dev *c = g_ptr_array_index (devs, i);
            if (c->vendor == want && !c->pseudo) {
                d = c;
                *how = "the GL vendor of the process";
            }
        }
    }
    for (guint i = 0; !d && real == 1 && i < devs->len; i++) {
        Dev *c = g_ptr_array_index (devs, i);
        if (!c->pseudo) {
            d = c;
            *how = "the only GPU";
        }
    }
    for (guint i = 0; !d && i < devs->len; i++) {
        Dev *c = g_ptr_array_index (devs, i);
        if (c->boot_vga && !c->pseudo) {
            d = c;
            *how = "the boot GPU";
        }
    }
    for (guint i = 0; !d && i < devs->len; i++) {
        Dev *c = g_ptr_array_index (devs, i);
        if (!c->pseudo) {
            d = c;
            *how = "the first GPU";
        }
    }
    return d;
}

/* which device the report is about, with what is known at this sample */
static void pick_device (WalpeGpuSampler *s)
{
    Dev *d = NULL;
    const char *how = "";
    if (s->device) {
        d = devs_find_named (s->devs, s->device);
        how = "named by the caller";
    }
    if (!d && s->target_self) {
        g_mutex_lock (&hint_lock);
        char *hint = g_strdup (hint_device);
        g_mutex_unlock (&hint_lock);
        if (hint && (d = devs_find_named (s->devs, hint)))
            how = "the device the daemon renders on";
        g_free (hint);
    }
    if (!d && s->target_pid > 0) {
        /* the device of the target's clients. With clients on two devices
         * (one renders, one only shows the picture) the one it works on
         * more; before any work is known, the one it holds more memory on. */
        Client *best = NULL;
        double best_score = -1;
        GHashTableIter it, it2;
        gpointer v, v2;
        g_hash_table_iter_init (&it, s->clients);
        while (g_hash_table_iter_next (&it, NULL, &v)) {
            Client *c = v;
            if (c->pid != s->target_pid || c->seen_tick != s->tick)
                continue;
            double score = 1;
            g_hash_table_iter_init (&it2, s->clients);
            while (g_hash_table_iter_next (&it2, NULL, &v2)) {
                Client *o = v2;
                if (o->pid != s->target_pid || strcmp (o->devkey, c->devkey))
                    continue;
                for (guint i = 0; i < o->engines->len; i++) {
                    CEngine *e = &g_array_index (o->engines, CEngine, i);
                    if (e->acc_den)
                        score += 1e6 * (double) e->acc / (double) e->acc_den;
                }
                if (o->mem_kb > 0)
                    score += o->mem_kb / 1048576.0;
            }
            if (score > best_score || (score == best_score && best && strcmp (c->devkey, best->devkey) < 0)) {
                best_score = score;
                best = c;
            }
        }
        if (best) {
            d = devs_find_key (s->devs, best->pdev, best->driver);
            if (!d)
                d = dev_pseudo (s, best->pdev, best->driver);
            how = "the process's DRM client";
        }
    }
    guint real = 0;
    for (guint i = 0; i < s->devs->len; i++)
        real += !((Dev *) g_ptr_array_index (s->devs, i))->pseudo;
    char *glv = g_strdup (s->gl_vendor);
    if (!glv && s->target_self) {
        g_mutex_lock (&hint_lock);
        glv = g_strdup (hint_gl_vendor);
        g_mutex_unlock (&hint_lock);
    }
    if (!d)
        d = devs_guess (s->devs, glv, &how);
    g_free (glv);
    if (!d && real == 0 && (s->flags & WALPE_GPUSTAT_SPAWN) && s->tick == 1) {
        /* the kernel lists no GPU (a driver without a DRM node, a container):
         * nvidia-smi may still know one */
        Dev *c = dev_pseudo (s, "", "nvidia");
        nv_probe (c, s->nvidia_smi);
        if (c->nv_dead) {
            g_ptr_array_remove (s->devs, c);
        } else {
            d = c;
            how = "the GPU nvidia-smi lists";
        }
    }
    if (!d && s->cur && s->cur->pseudo && s->cur->nv_fields)
        return;   /* the nvidia-smi device of an earlier sample stays */
    s->cur = d;
    g_strlcpy (s->cur_how, d ? how : "", sizeof (s->cur_how));
}

static gboolean dev_is_nvidia (const Dev *d)
{
    return !strcmp (d->driver, "nvidia") || (d->vendor == 0x10de && !d->driver[0]);
}

void walpe_gpu_sampler_tick_at (WalpeGpuSampler *s, gint64 now)
{
    s->tick++;
    if (s->tick == 1)
        s->t_first = now;
    gboolean others = (s->flags & WALPE_GPUSTAT_OTHERS) != 0;
    gboolean full = others && (s->tick == 1 || (s->tick - 1) % s->rescan_every == 0);
    s->sight = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    if (full) {
        if (g_hash_table_size (s->procs) >= MAX_PROCS / 2) {
            /* a long run on a busy machine: processes that ended and never
             * had a DRM client make room for the ones that start now */
            GHashTableIter pit;
            gpointer pv;
            g_hash_table_iter_init (&pit, s->procs);
            while (g_hash_table_iter_next (&pit, NULL, &pv)) {
                Proc *p = pv;
                if (p->exited && !p->drm_fds->len && !p->target)
                    g_hash_table_iter_remove (&pit);
            }
        }
        walk_procs (s);
    }
    if (s->target_pid > 0 && !s->target_gone && !s->target_other_user) {
        gint64 uid = s->target_self ? s->uid : proc_uid (s, s->target_pid);
        if (uid < 0) {
            if (s->target_seen)
                s->target_gone = TRUE;
        } else if (uid != s->uid) {
            s->target_other_user = TRUE;   /* not ours: not read */
        } else {
            Proc *p = proc_get (s, s->target_pid, TRUE);
            if (p) {
                read_proc (s, p, TRUE);   /* every descriptor, every sample: new clients show at once */
                if (p->seen_tick == s->tick)
                    s->target_seen = TRUE;
                if (p->exited)
                    s->target_gone = TRUE;
            }
        }
    }
    if (others) {
        GHashTableIter it;
        gpointer v;
        g_hash_table_iter_init (&it, s->procs);
        while (g_hash_table_iter_next (&it, NULL, &v)) {
            Proc *p = v;
            if (p->pid == s->target_pid)
                continue;
            if (full || p->drm_fds->len)
                read_proc (s, p, full);
        }
    }
    apply_sightings (s, now);
    g_hash_table_destroy (s->sight);
    s->sight = NULL;

    pick_device (s);
    Dev *d = s->cur;
    if (d) {
        guint busy_n = d->m[WALPE_GPU_M_BUSY].n, clk_n = d->m[WALPE_GPU_M_CLOCK].n;
        if (!(s->flags & WALPE_GPUSTAT_NO_DEVICE))
            dev_sample_sysfs (d, now);
        gboolean nv = (s->flags & WALPE_GPUSTAT_SPAWN) && dev_is_nvidia (d);
        if (nv) {
            GArray *np = g_array_new (FALSE, TRUE, sizeof (NvProc));
            dev_sample_nvidia (d, s->nvidia_smi, np);
            for (guint i = 0; i < np->len; i++) {
                NvProc *x = &g_array_index (np, NvProc, i);
                NvAcc *a = g_hash_table_lookup (s->nv, GINT_TO_POINTER (x->pid));
                if (!a) {
                    a = g_new0 (NvAcc, 1);
                    a->pid = x->pid;
                    g_hash_table_insert (s->nv, GINT_TO_POINTER (x->pid), a);
                }
                g_strlcpy (a->name, x->name, sizeof (a->name));
                a->rows++;
                if (x->sm >= 0)
                    acc_add (&a->sm, x->sm);
                if (x->fb_mb >= 0)
                    acc_add (&a->fb, x->fb_mb);
                if (x->pid == s->target_pid) {
                    if (x->sm >= 0)
                        dev_value (d, WALPE_GPU_M_PROC_BUSY, WALPE_GPU_SRC_NVIDIA_SMI, x->sm);
                    if (x->fb_mb >= 0)
                        dev_value (d, WALPE_GPU_M_PROC_MEM, WALPE_GPU_SRC_NVIDIA_SMI, x->fb_mb);
                }
            }
            g_array_unref (np);
        }
        /* the kernel's per-client figures for this device */
        char key[40];
        dev_key (d, key, sizeof (key));
        Agg *a = main_agg (s, key);
        double busy_now = -1;
        if (a && a->d_any && a->d_den) {
            if (others) {
                dev_value (d, WALPE_GPU_M_BUSY_CLIENTS, WALPE_GPU_SRC_FDINFO, a->p_all);
                dev_value (d, WALPE_GPU_M_OTHERS_BUSY, WALPE_GPU_SRC_FDINFO, MAX (0.0, a->p_all - a->p_t));
                busy_now = a->p_all;
            }
            if (a->d_t_any)   /* not after the target has gone */
                dev_value (d, WALPE_GPU_M_PROC_BUSY, WALPE_GPU_SRC_FDINFO, a->p_t);
        }
        if (s->target_pid > 0 && !nv) {
            gint64 kb = 0;
            gboolean any = FALSE, total = FALSE;
            GHashTableIter it;
            gpointer v;
            g_hash_table_iter_init (&it, s->clients);
            while (g_hash_table_iter_next (&it, NULL, &v)) {
                Client *c = v;
                if (c->pid != s->target_pid || c->seen_tick != s->tick || strcmp (c->devkey, key) ||
                    c->mem_kb < 0)
                    continue;
                kb += c->mem_kb;
                any = TRUE;
                total |= c->mem_is_total;
            }
            if (any) {
                dev_value (d, WALPE_GPU_M_PROC_MEM, WALPE_GPU_SRC_FDINFO, kb / 1024.0);
                d->mem_is_total = total;
            }
        }
        /* a rough "at full clock" figure, as the NVIDIA table has it */
        if (d->m[WALPE_GPU_M_BUSY].n > busy_n)
            busy_now = d->m[WALPE_GPU_M_BUSY].last;
        if (busy_now >= 0 && d->m[WALPE_GPU_M_CLOCK].n > clk_n && d->m[WALPE_GPU_M_CLOCK_MAX].n &&
            d->m[WALPE_GPU_M_CLOCK_MAX].last > 0)
            dev_value (d, WALPE_GPU_M_BUSY_SCALED, WALPE_GPU_SRC_DERIVED,
                       MIN (100.0, busy_now * d->m[WALPE_GPU_M_CLOCK].last /
                                   d->m[WALPE_GPU_M_CLOCK_MAX].last));
    }
    s->t_last = now;
}

void walpe_gpu_sampler_tick (WalpeGpuSampler *s)
{
    walpe_gpu_sampler_tick_at (s, g_get_monotonic_time () * 1000);
}

/* ========================================================= 6. the report */

static void val_set (WalpeGpuStatVal *o, const Acc *a, WalpeGpuSrc src)
{
    if (!a->n || src == WALPE_GPU_SRC_NONE)
        return;
    o->src = src;
    o->n = a->n;
    o->min = a->min;
    o->max = a->max;
    o->avg = a->sum / a->n;
    o->last = a->last;
}

static void why (WalpeGpuStatReport *r, int metric, const char *fmt, ...) G_GNUC_PRINTF (3, 4);
static void why (WalpeGpuStatReport *r, int metric, const char *fmt, ...)
{
    if (r->m[metric].src != WALPE_GPU_SRC_NONE || r->m[metric].why[0])
        return;
    va_list ap;
    va_start (ap, fmt);
    g_vsnprintf (r->m[metric].why, sizeof (r->m[metric].why), fmt, ap);
    va_end (ap);
}

/* why each value that is missing is missing, in words for the user */
static void report_reasons (WalpeGpuSampler *s, WalpeGpuStatReport *r, const Dev *d)
{
    const char *drv = r->driver[0] ? r->driver : "this";
    gboolean nvidia = d && dev_is_nvidia (d);
    gboolean intel = !strcmp (r->driver, "i915") || !strcmp (r->driver, "xe");
    gboolean amd = !strcmp (r->driver, "amdgpu");
    if (!d) {
        for (int i = 0; i < WALPE_GPU_N_METRICS; i++)
            why (r, i, "the kernel lists no GPU (nothing in /sys/class/drm)");
        return;
    }
    if (d->suspended_ticks && d->suspended_ticks >= s->tick)
        for (int i = WALPE_GPU_M_BUSY; i <= WALPE_GPU_M_MEM_CLOCK; i++)
            if (i != WALPE_GPU_M_BUSY_CLIENTS)
                why (r, i, "the GPU was powered down (runtime suspended) at every sample; it is "
                     "not woken for statistics");
    if (nvidia) {
        const char *t = !(s->flags & WALPE_GPUSTAT_SPAWN)
                        ? "the NVIDIA driver gives the kernel interfaces no statistics; nvidia-smi "
                          "has them (walpe-gpu-report, walpe-ctl gpu-stats)"
                        : "nvidia-smi is not installed, does not answer or has no figure";
        for (int i = 0; i < WALPE_GPU_N_METRICS; i++)
            if (i != WALPE_GPU_M_BUSY_CLIENTS && i != WALPE_GPU_M_OTHERS_BUSY &&
                i != WALPE_GPU_M_GTT_USED && i != WALPE_GPU_M_GTT_TOTAL)
                why (r, i, "%s", t);
    }
    if (intel)
        why (r, WALPE_GPU_M_BUSY, "the %s driver has no whole-GPU figure a user may read; the "
             "visible clients' sum is the nearest", drv);
    else if (amd)
        why (r, WALPE_GPU_M_BUSY, "this amdgpu device has no gpu_busy_percent");
    else
        why (r, WALPE_GPU_M_BUSY, "the %s driver has no whole-GPU busy figure in sysfs", drv);
    if (!r->others_read)
        why (r, WALPE_GPU_M_BUSY_CLIENTS, "other processes were not read");
    else if (!r->fdinfo_engines)
        why (r, WALPE_GPU_M_BUSY_CLIENTS, "no client of the %s driver publishes engine time in "
             "fdinfo", drv);
    else
        why (r, WALPE_GPU_M_BUSY_CLIENTS, "a share needs two samples of the same client");
    g_strlcpy (r->m[WALPE_GPU_M_OTHERS_BUSY].why, r->m[WALPE_GPU_M_BUSY_CLIENTS].why,
               sizeof (r->m[0].why));
    why (r, WALPE_GPU_M_BUSY_SCALED, "needs a busy figure, the clock and the highest clock");
    why (r, WALPE_GPU_M_MEM_BUSY, "only amdgpu and nvidia-smi report it");
    if (intel) {
        why (r, WALPE_GPU_M_VRAM_USED, "the %s driver has no whole-GPU memory figure a user may "
             "read (an integrated GPU uses system memory)", drv);
        why (r, WALPE_GPU_M_POWER, "no power sensor a user may read (an integrated GPU's power "
             "is part of the CPU package's)");
        why (r, WALPE_GPU_M_TEMP, "no temperature sensor of its own (an integrated GPU shares "
             "the CPU's)");
    }
    why (r, WALPE_GPU_M_VRAM_USED, "the %s driver has no whole-GPU memory figure in sysfs", drv);
    g_strlcpy (r->m[WALPE_GPU_M_VRAM_TOTAL].why, r->m[WALPE_GPU_M_VRAM_USED].why, sizeof (r->m[0].why));
    why (r, WALPE_GPU_M_GTT_USED, "only amdgpu reports it");
    why (r, WALPE_GPU_M_GTT_TOTAL, "only amdgpu reports it");
    why (r, WALPE_GPU_M_POWER, "the device has no hwmon power sensor");
    why (r, WALPE_GPU_M_TEMP, "the device has no hwmon temperature sensor");
    why (r, WALPE_GPU_M_CLOCK, "the %s driver does not publish its clock in sysfs", drv);
    why (r, WALPE_GPU_M_CLOCK_MAX, "the %s driver does not publish its clock in sysfs", drv);
    why (r, WALPE_GPU_M_MEM_CLOCK, "the device's hwmon node has no memory clock");
    /* the target's rows */
    const char *t = NULL;
    if (r->target_pid <= 0)
        t = "no process was given";
    else if (s->target_other_user)
        t = "the process belongs to another user: not read";
    else if (!r->target_seen)
        t = "the process was not found";
    else {
        Proc *p = g_hash_table_lookup (s->procs, GINT_TO_POINTER (r->target_pid));
        if (p && p->denied)
            t = "the process does not let its fdinfo be read";
    }
    if (t) {
        why (r, WALPE_GPU_M_PROC_BUSY, "%s", t);
        why (r, WALPE_GPU_M_PROC_MEM, "%s", t);
    } else if (!r->target_clients) {
        why (r, WALPE_GPU_M_PROC_BUSY, "the process has no DRM client with statistics on this "
             "device (it renders elsewhere, or in software)");
        why (r, WALPE_GPU_M_PROC_MEM, "the process has no DRM client with statistics on this "
             "device (it renders elsewhere, or in software)");
    } else {
        gboolean has_eng = FALSE;
        for (guint i = 0; i < r->n_engines; i++)
            has_eng |= r->engines[i].has_target;
        if (!has_eng)
            why (r, WALPE_GPU_M_PROC_BUSY, "the %s driver publishes no engine time in fdinfo", drv);
        else
            why (r, WALPE_GPU_M_PROC_BUSY, "a share needs two samples of the same client");
        why (r, WALPE_GPU_M_PROC_MEM, "the %s driver (or this kernel) publishes no memory in "
             "fdinfo", drv);
    }
}

static int proc_cmp (gconstpointer a, gconstpointer b)
{
    const WalpeGpuProcStat *x = a, *y = b;
    double bx = x->has_busy ? x->busy_pct : -1, by = y->has_busy ? y->busy_pct : -1;
    if (bx != by)
        return bx > by ? -1 : 1;
    if (x->mem_kb != y->mem_kb)
        return x->mem_kb > y->mem_kb ? -1 : 1;
    return x->pid - y->pid;
}

WalpeGpuStatReport *walpe_gpu_sampler_report (WalpeGpuSampler *s)
{
    WalpeGpuStatReport *r = g_new0 (WalpeGpuStatReport, 1);
    r->proc_evicted_kb = -1;
    if (!s->cur)
        pick_device (s);
    Dev *d = s->cur;
    for (guint i = 0; i < s->devs->len; i++)
        r->n_devices += !((Dev *) g_ptr_array_index (s->devs, i))->pseudo;
    if (!r->n_devices && d)
        r->n_devices = 1;
    r->samples = s->tick;
    r->seconds = s->tick > 1 ? (s->t_last - s->t_first) / 1e9 : 0.0;
    r->target_pid = s->target_pid > 0 ? s->target_pid : 0;
    r->target_seen = s->target_seen;
    r->target_gone = s->target_gone;
    r->others_read = (s->flags & WALPE_GPUSTAT_OTHERS) != 0;
    r->procs_other_users = s->other_users;
    r->clients_shared = s->shared;
    r->clients_no_id = s->no_id;
    r->counter_resets = s->resets;
    Proc *tp = r->target_pid ? g_hash_table_lookup (s->procs, GINT_TO_POINTER (r->target_pid)) : NULL;
    if (tp)
        g_strlcpy (r->target_name, tp->name, sizeof (r->target_name));
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init (&it, s->procs);
    while (g_hash_table_iter_next (&it, NULL, &v))
        r->procs_denied += ((Proc *) v)->denied && !((Proc *) v)->target;

    char key[40] = "";
    if (d) {
        g_strlcpy (r->driver, d->driver, sizeof (r->driver));
        g_strlcpy (r->pci_slot, d->slot, sizeof (r->pci_slot));
        g_strlcpy (r->card, d->card, sizeof (r->card));
        g_strlcpy (r->render_node, d->render, sizeof (r->render_node));
        g_strlcpy (r->vendor_name, vendor_name (d->vendor), sizeof (r->vendor_name));
        r->vendor_id = d->vendor;
        r->device_id = d->device;
        g_strlcpy (r->name, d->name, sizeof (r->name));
        g_strlcpy (r->driver_version, d->drv_version, sizeof (r->driver_version));
        g_strlcpy (r->how_found, s->cur_how, sizeof (r->how_found));
        r->mem_is_total = d->mem_is_total;
        for (int i = 0; i < WALPE_GPU_N_METRICS; i++) {
            val_set (&r->m[i], &d->m[i], d->src[i]);
            if (r->m[i].src != WALPE_GPU_SRC_NONE)
                r->sources |= 1u << r->m[i].src;
        }
        r->sources &= ~(1u << WALPE_GPU_SRC_DERIVED);
        dev_key (d, key, sizeof (key));
    }

    /* engines of this device */
    Agg *main = d ? main_agg (s, key) : NULL;
    if (main)
        g_strlcpy (r->main_engine, main->name, sizeof (r->main_engine));
    r->engines = g_new0 (WalpeGpuEngineStat, s->aggs->len + 1);
    for (guint i = 0; d && i < s->aggs->len; i++) {
        Agg *a = &g_array_index (s->aggs, Agg, i);
        if (strcmp (a->devkey, key))
            continue;
        WalpeGpuEngineStat *e = &r->engines[r->n_engines++];
        g_strlcpy (e->name, a->name, sizeof (e->name));
        e->capacity = a->capacity;
        e->cycles = a->cycles;
        e->has_target = a->has_target;
        e->resets = a->resets;
        /* the target's share is over the time the target was measured (it
         * may have started late or gone early) */
        if (a->acc_den_t)
            e->proc_pct = MIN (100.0, 100.0 * (double) a->acc_t / (double) a->acc_den_t / a->capacity);
        if (a->acc_den)
            e->all_pct = MIN (100.0, 100.0 * (double) a->acc_all / (double) a->acc_den / a->capacity);
        e->proc_max_pct = a->t_max;
        r->fdinfo_engines = TRUE;
    }
    /* the averages of the share rows are the whole run's (engine time over
     * the run / its length), not the mean of the per-sample shares: a
     * counter that was held back and caught up later must not count twice */
    if (main && main->acc_den) {
        double cap = main->capacity;
        if (r->m[WALPE_GPU_M_PROC_BUSY].src == WALPE_GPU_SRC_FDINFO && main->acc_den_t)
            r->m[WALPE_GPU_M_PROC_BUSY].avg = MIN (100.0, 100.0 * (double) main->acc_t / (double) main->acc_den_t / cap);
        if (r->m[WALPE_GPU_M_BUSY_CLIENTS].src == WALPE_GPU_SRC_FDINFO)
            r->m[WALPE_GPU_M_BUSY_CLIENTS].avg = MIN (100.0, 100.0 * (double) main->acc_all / (double) main->acc_den / cap);
        if (r->m[WALPE_GPU_M_OTHERS_BUSY].src == WALPE_GPU_SRC_FDINFO)
            r->m[WALPE_GPU_M_OTHERS_BUSY].avg = MIN (100.0, 100.0 * (double) (main->acc_all - main->acc_t) /
                                                              (double) main->acc_den / cap);
    }

    /* clients of this device: the target's memory by region, the programs */
    r->regions = g_new0 (WalpeGpuFdRegion, WALPE_GPU_MAX_REGIONS + 1);
    GHashTable *by_pid = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
    g_hash_table_iter_init (&it, s->clients);
    while (d && g_hash_table_iter_next (&it, NULL, &v)) {
        Client *c = v;
        if (strcmp (c->devkey, key))
            continue;
        if (c->mem_kb >= 0)
            r->fdinfo_memory = TRUE;
        if (c->engines->len)
            r->fdinfo_engines = TRUE;
        Proc *p = g_hash_table_lookup (s->procs, GINT_TO_POINTER (c->pid));
        gboolean current = p && c->seen_tick == p->seen_tick;   /* open when the process was last seen */
        if (c->pid == r->target_pid && c->seen_tick == s->tick) {
            r->target_clients++;
            if (c->evicted_kb >= 0)
                r->proc_evicted_kb = MAX (r->proc_evicted_kb, 0) + c->evicted_kb;
            for (guint i = 0; i < c->n_regions; i++) {
                WalpeGpuFdRegion *o = NULL;
                for (guint k = 0; k < r->n_regions && !o; k++)
                    if (!strcmp (r->regions[k].name, c->regions[i].name))
                        o = &r->regions[k];
                if (!o && r->n_regions < WALPE_GPU_MAX_REGIONS) {
                    o = &r->regions[r->n_regions++];
                    g_strlcpy (o->name, c->regions[i].name, sizeof (o->name));
                    o->resident_kb = o->total_kb = o->shared_kb = o->active_kb = o->purgeable_kb = -1;
                }
                if (!o)
                    continue;
#define ADD_KB(f) if (c->regions[i].f >= 0) o->f = MAX (o->f, 0) + c->regions[i].f
                ADD_KB (resident_kb);
                ADD_KB (total_kb);
                ADD_KB (shared_kb);
                ADD_KB (active_kb);
                ADD_KB (purgeable_kb);
#undef ADD_KB
            }
        }
        WalpeGpuProcStat *ps = g_hash_table_lookup (by_pid, GINT_TO_POINTER (c->pid));
        if (!ps) {
            ps = g_new0 (WalpeGpuProcStat, 1);
            ps->pid = c->pid;
            ps->mem_kb = -1;
            ps->target = c->pid == r->target_pid;
            if (p) {
                g_strlcpy (ps->name, p->name, sizeof (ps->name));
                ps->exited = p->exited || (p->seen_tick != s->tick && p->seen_tick != 0 &&
                                           (p->drm_fds->len || p->target));
            }
            g_hash_table_insert (by_pid, GINT_TO_POINTER (c->pid), ps);
        }
        if (current) {
            ps->clients++;
            if (c->mem_kb >= 0)
                ps->mem_kb = MAX (ps->mem_kb, 0) + c->mem_kb;
        }
        if (main)
            for (guint i = 0; i < c->engines->len; i++) {
                CEngine *e = &g_array_index (c->engines, CEngine, i);
                if (strcmp (e->name, main->name) || !e->acc_den)
                    continue;
                /* busy_pct collects the share of each of its clients: they
                 * were all measured over the time the process was seen */
                ps->busy_pct += 100.0 * (double) e->acc / (double) e->acc_den / MAX (e->capacity, 1u);
                ps->has_busy = TRUE;
            }
    }
    /* programs as nvidia-smi lists them (it names other users' programs itself) */
    if (d && g_hash_table_size (s->nv)) {
        g_hash_table_iter_init (&it, s->nv);
        while (g_hash_table_iter_next (&it, NULL, &v)) {
            NvAcc *a = v;
            WalpeGpuProcStat *ps = g_new0 (WalpeGpuProcStat, 1);
            ps->pid = a->pid;
            g_strlcpy (ps->name, a->name, sizeof (ps->name));
            ps->target = a->pid == r->target_pid;
            ps->clients = 1;
            ps->has_busy = a->sm.n > 0;
            ps->busy_pct = a->sm.n ? a->sm.sum / a->sm.n : 0;
            ps->mem_kb = a->fb.n ? (gint64) (a->fb.sum / a->fb.n * 1024.0) : -1;
            if (ps->target) {
                r->target_clients = 1;
                r->target_seen = TRUE;
                g_strlcpy (r->target_name, a->name, sizeof (r->target_name));
            }
            g_hash_table_insert (by_pid, GINT_TO_POINTER (a->pid), ps);
        }
    }
    /* a driver whose clients publish statistics is a source from the first
     * sample on, before a share (which needs two) exists */
    if (r->fdinfo_engines || r->fdinfo_memory)
        r->sources |= 1u << WALPE_GPU_SRC_FDINFO;
    r->procs = g_new0 (WalpeGpuProcStat, g_hash_table_size (by_pid) + 1);
    g_hash_table_iter_init (&it, by_pid);
    while (g_hash_table_iter_next (&it, NULL, &v)) {
        WalpeGpuProcStat *ps = v;
        ps->busy_pct = MIN (ps->busy_pct, 100.0);
        r->procs[r->n_procs++] = *ps;
    }
    g_hash_table_destroy (by_pid);
    if (r->n_procs > 1)
        qsort (r->procs, r->n_procs, sizeof (r->procs[0]), proc_cmp);

    /* the sum of the clients a user may read is a lower bound of the whole
     * GPU's load whenever some clients cannot be read */
    gboolean hidden = r->procs_other_users || r->procs_denied;
    r->m[WALPE_GPU_M_BUSY_CLIENTS].lower_bound = hidden;
    if (r->m[WALPE_GPU_M_BUSY_SCALED].src != WALPE_GPU_SRC_NONE &&
        r->m[WALPE_GPU_M_BUSY].src == WALPE_GPU_SRC_NONE)
        r->m[WALPE_GPU_M_BUSY_SCALED].lower_bound = hidden;
    report_reasons (s, r, d);
    return r;
}

void walpe_gpu_stat_report_free (WalpeGpuStatReport *r)
{
    if (!r)
        return;
    g_free (r->engines);
    g_free (r->regions);
    g_free (r->procs);
    g_free (r);
}

/* ====================================================== 7. text and JSON */

void walpe_gpu_stat_format_device (const WalpeGpuStatReport *r, GString *out)
{
    if (!r->driver[0] && !r->pci_slot[0] && !r->card[0] && !r->name[0]) {
        g_string_append (out, "none found (the kernel lists no GPU)");
        return;
    }
    if (r->name[0])
        g_string_append (out, r->name);
    else if (r->vendor_name[0])
        g_string_append (out, r->vendor_name);
    else
        g_string_append (out, "GPU");
    if (r->vendor_id)
        g_string_append_printf (out, " [%04x:%04x]", r->vendor_id, r->device_id);
    if (r->pci_slot[0])
        g_string_append_printf (out, " at %s", r->pci_slot);
    if (r->card[0])
        g_string_append_printf (out, " (%s%s%s)", r->card, r->render_node[0] ? ", " : "", r->render_node);
    g_string_append_printf (out, ", driver %s", r->driver[0] ? r->driver : "unknown");
    if (r->driver_version[0])
        g_string_append_printf (out, " %s", r->driver_version);
}

void walpe_gpu_stat_format_sources (const WalpeGpuStatReport *r, GString *out)
{
    gboolean any = FALSE;
    for (int i = 1; i < WALPE_GPU_N_SRC; i++)
        if (i != WALPE_GPU_SRC_DERIVED && (r->sources & (1u << i))) {
            g_string_append_printf (out, "%s%s", any ? ", " : "", walpe_gpu_src_name (i));
            any = TRUE;
        }
    if (!any)
        g_string_append (out, "none");
}

/* one table row. den = how many samples could have had a value. */
static void row (GString *out, const char *name, const char *unit, const WalpeGpuStatVal *v,
                 guint den, const char *remark)
{
    if (v->src == WALPE_GPU_SRC_NONE) {
        g_string_append_printf (out, "%-26s %-7s %9s %9s %9s  -  (not available: %s)\n", name, unit,
                                "n/a", "n/a", "n/a", v->why[0] ? v->why : "not published");
        return;
    }
    g_string_append_printf (out, "%-26s %-7s %9.1f %9.1f %9.1f  %u/%u  (%s%s%s%s)\n", name, unit,
                            v->min, v->avg, v->max, v->n, MAX (den, v->n), walpe_gpu_src_name (v->src),
                            remark && *remark ? ": " : "", remark ? remark : "",
                            v->lower_bound ? "; a lower bound: some clients cannot be read" : "");
}

void walpe_gpu_stat_format_rows (const WalpeGpuStatReport *r, const char *who, GString *out)
{
    char name[64], rem[256];
    guint n = r->samples, nd = r->samples > 1 ? r->samples - 1 : 0;   /* readings / differences */
    const WalpeGpuStatVal *m = r->m;
    const char *eng = r->main_engine[0] ? r->main_engine : "?";
    if (!who || !*who)
        who = "process";

    row (out, "GPU busy", "%", &m[WALPE_GPU_M_BUSY], n,
         m[WALPE_GPU_M_BUSY].src == WALPE_GPU_SRC_AMD_SYSFS ? "gpu_busy_percent"
         : m[WALPE_GPU_M_BUSY].src == WALPE_GPU_SRC_NVIDIA_SMI ? "utilization.gpu" : "");
    if (r->others_read) {
        g_snprintf (rem, sizeof (rem), "engine %s, every client this user may read", eng);
        row (out, "GPU busy, visible clients", "%", &m[WALPE_GPU_M_BUSY_CLIENTS], nd, rem);
    }
    if (m[WALPE_GPU_M_BUSY_SCALED].src != WALPE_GPU_SRC_NONE)
        row (out, "GPU busy at full clock", "%", &m[WALPE_GPU_M_BUSY_SCALED], n,
             "busy x clock / highest clock: rough");
    if (m[WALPE_GPU_M_MEM_BUSY].src != WALPE_GPU_SRC_NONE)
        row (out, "GPU memory busy", "%", &m[WALPE_GPU_M_MEM_BUSY], n,
             "share of time memory was read or written, not how full it is");
    if (m[WALPE_GPU_M_VRAM_TOTAL].src != WALPE_GPU_SRC_NONE)
        g_snprintf (rem, sizeof (rem), "all programs, of %.0f MiB", m[WALPE_GPU_M_VRAM_TOTAL].last);
    else
        g_snprintf (rem, sizeof (rem), "all programs");
    row (out, "VRAM used", "MiB", &m[WALPE_GPU_M_VRAM_USED], n, rem);
    if (m[WALPE_GPU_M_GTT_USED].src != WALPE_GPU_SRC_NONE) {
        if (m[WALPE_GPU_M_GTT_TOTAL].src != WALPE_GPU_SRC_NONE)
            g_snprintf (rem, sizeof (rem), "system memory mapped for the GPU, of %.0f MiB",
                        m[WALPE_GPU_M_GTT_TOTAL].last);
        else
            g_snprintf (rem, sizeof (rem), "system memory mapped for the GPU");
        row (out, "GTT used", "MiB", &m[WALPE_GPU_M_GTT_USED], n, rem);
    }
    row (out, "power", "W", &m[WALPE_GPU_M_POWER], n, "");
    row (out, "temperature", "C", &m[WALPE_GPU_M_TEMP], n, "");
    if (m[WALPE_GPU_M_CLOCK_MAX].src != WALPE_GPU_SRC_NONE)
        g_snprintf (rem, sizeof (rem), "highest %.0f MHz", m[WALPE_GPU_M_CLOCK_MAX].last);
    else
        rem[0] = 0;
    row (out, "clock", "MHz", &m[WALPE_GPU_M_CLOCK], n, rem);
    if (m[WALPE_GPU_M_MEM_CLOCK].src != WALPE_GPU_SRC_NONE)
        row (out, "memory clock", "MHz", &m[WALPE_GPU_M_MEM_CLOCK], n, "");

    if (r->target_pid <= 0)
        return;
    g_snprintf (name, sizeof (name), "%s GPU share", who);
    if (m[WALPE_GPU_M_PROC_BUSY].src == WALPE_GPU_SRC_NVIDIA_SMI)
        g_snprintf (rem, sizeof (rem), "pmon sm");
    else
        g_snprintf (rem, sizeof (rem), "engine %s: its time on the engine / wall time", eng);
    row (out, name, "%", &m[WALPE_GPU_M_PROC_BUSY], nd, rem);
    /* its other engines (video decode, compute, copy ...), when it used them */
    for (guint i = 0; i < r->n_engines; i++) {
        const WalpeGpuEngineStat *e = &r->engines[i];
        if (!e->has_target || !strcmp (e->name, r->main_engine) || e->proc_max_pct <= 0)
            continue;
        g_snprintf (name, sizeof (name), "%s engine %s", who, e->name);
        g_string_append_printf (out, "%-26s %-7s %9s %9.1f %9.1f  %u/%u  (fdinfo%s)\n", name, "%", "-",
                                e->proc_pct, e->proc_max_pct, nd, nd,
                                e->capacity > 1 ? ": share of all engines of this kind" : "");
    }
    GString *reg = g_string_new (r->mem_is_total ? "allocated (the driver gives no resident figure)"
                                                 : "resident");
    gboolean first = TRUE;
    for (guint i = 0; i < r->n_regions; i++) {
        gint64 kb = r->mem_is_total ? r->regions[i].total_kb : r->regions[i].resident_kb;
        if (kb <= 0)
            continue;
        g_string_append_printf (reg, "%s%s %.1f", first ? ": " : " + ", r->regions[i].name, kb / 1024.0);
        first = FALSE;
    }
    if (r->proc_evicted_kb > 0)
        g_string_append_printf (reg, "; %.1f MiB of its VRAM moved out for lack of room",
                                r->proc_evicted_kb / 1024.0);
    g_snprintf (name, sizeof (name), "%s GPU memory", who);
    row (out, name, "MiB", &m[WALPE_GPU_M_PROC_MEM], n,
         m[WALPE_GPU_M_PROC_MEM].src == WALPE_GPU_SRC_NVIDIA_SMI ? "pmon fb" : reg->str);
    g_string_free (reg, TRUE);
    if (r->others_read) {
        g_snprintf (rem, sizeof (rem), "engine %s, the visible clients without the %s", eng, who);
        row (out, "other programs' GPU share", "%", &m[WALPE_GPU_M_OTHERS_BUSY], nd, rem);
    }
    if (r->target_gone)
        g_string_append_printf (out, "(the %s went away during the sampling: its rows cover the "
                                "time before)\n", who);
}

static void append_hidden (const WalpeGpuStatReport *r, GString *out, const char *indent)
{
    if (!r->procs_other_users && !r->procs_denied)
        return;
    g_string_append_printf (out, "%snot in this list:", indent);
    if (r->procs_other_users)
        g_string_append_printf (out, " %u process%s of other users (never read; an X server or a "
                                "display manager that runs as root is among them)",
                                r->procs_other_users, r->procs_other_users == 1 ? "" : "es");
    if (r->procs_denied)
        g_string_append_printf (out, "%s %u of your processes that do not allow reading their "
                                "fdinfo", r->procs_other_users ? ";" : "", r->procs_denied);
    g_string_append_c (out, '\n');
}

void walpe_gpu_stat_format_programs (const WalpeGpuStatReport *r, guint max, GString *out)
{
    gboolean nv = r->m[WALPE_GPU_M_PROC_BUSY].src == WALPE_GPU_SRC_NVIDIA_SMI ||
                  (r->sources & (1u << WALPE_GPU_SRC_NVIDIA_SMI) && !r->fdinfo_engines);
    if (!r->n_procs) {
        if (r->others_read && !nv)
            g_string_append_printf (out, "programs on the GPU: none of your processes has a DRM "
                                    "client with statistics on this device%s\n",
                                    r->driver[0] && !r->fdinfo_engines && !r->fdinfo_memory
                                    ? " (the driver publishes none)" : "");
        append_hidden (r, out, "  ");
        return;
    }
    if (nv)
        g_string_append (out, "programs on the GPU (nvidia-smi pmon; share = sm)\n");
    else
        g_string_append_printf (out, "programs on the GPU (kernel fdinfo; share = time on engine %s "
                                "/ wall time; memory = %s)\n",
                                r->main_engine[0] ? r->main_engine : "?",
                                r->mem_is_total ? "allocated" : "resident");
    guint shown = 0;
    for (guint i = 0; i < r->n_procs && shown < max; i++, shown++) {
        const WalpeGpuProcStat *p = &r->procs[i];
        char mem[24], busy[24];
        if (p->mem_kb >= 0)
            g_snprintf (mem, sizeof (mem), "%.1f", p->mem_kb / 1024.0);
        else
            g_strlcpy (mem, "n/a", sizeof (mem));
        if (p->has_busy)
            g_snprintf (busy, sizeof (busy), "%.1f", p->busy_pct);
        else
            g_strlcpy (busy, "n/a", sizeof (busy));
        g_string_append_printf (out, "  %-20s %8s MiB   %5s %%%s%s\n", p->name[0] ? p->name : "?", mem,
                                busy, p->target ? "   <- measured" : "",
                                p->exited ? "   (exited during the sampling)" : "");
    }
    if (r->n_procs > shown)
        g_string_append_printf (out, "  (+%u more)\n", r->n_procs - shown);
    if (!nv)
        append_hidden (r, out, "  ");
}

void walpe_gpu_stat_format_notes (const WalpeGpuStatReport *r, GString *out)
{
    gboolean fd = (r->sources & (1u << WALPE_GPU_SRC_FDINFO)) != 0;
    if (fd)
        g_string_append (out,
            " - \"GPU share\" from fdinfo is the time the kernel accounted to a program on one engine\n"
            "   of the GPU (the one that draws: render / rcs / gfx / gpu), divided by the wall time\n"
            "   between two samples. Like every busy percentage it is a share of TIME at the clock\n"
            "   the GPU ran at, not of its capacity: compare runs by the clock and the watts too.\n"
            " - \"GPU memory\" from fdinfo is what the kernel holds for the program's buffers. A\n"
            "   buffer shared by two programs (a window's picture: the program and the compositor)\n"
            "   is counted in both, so the programs' figures do not add up to the GPU's.\n");
    if (r->m[WALPE_GPU_M_BUSY_CLIENTS].src != WALPE_GPU_SRC_NONE)
        g_string_append_printf (out,
            " - \"GPU busy, visible clients\" adds up the programs this user may read%s.\n",
            r->m[WALPE_GPU_M_BUSY_CLIENTS].lower_bound
            ? ": programs of other users\n   (an X server running as root) are missing, the real "
              "load is higher by their part" : "");
    if (r->m[WALPE_GPU_M_BUSY].src == WALPE_GPU_SRC_AMD_SYSFS)
        g_string_append (out,
            " - \"GPU busy\" (amdgpu) is the driver's own reading at the moment of each sample.\n");
    if (r->m[WALPE_GPU_M_GTT_USED].src != WALPE_GPU_SRC_NONE)
        g_string_append (out,
            " - VRAM is the GPU's own memory; on an integrated AMD GPU it is only the part of system\n"
            "   memory the firmware set aside, and GTT - ordinary system memory the GPU uses beyond\n"
            "   that - is where most of it lives. A small, full \"VRAM\" there is not a full card.\n");
    if (r->m[WALPE_GPU_M_CLOCK].src == WALPE_GPU_SRC_INTEL_SYSFS)
        g_string_append (out,
            " - Intel: a clock of 0 MHz is a GPU that sleeps between frames, not an error.\n");
    if (r->counter_resets)
        g_string_append_printf (out,
            " - %u time%s a kernel counter went backwards and did not catch up, or jumped: the\n"
            "   interval was left out, the shares are slightly low.\n",
            r->counter_resets, r->counter_resets == 1 ? "" : "s");
    if (r->clients_no_id)
        g_string_append_printf (out,
            " - %u client%s without drm-client-id: copies of one descriptor could not be told\n"
            "   apart and may be counted twice.\n", r->clients_no_id, r->clients_no_id == 1 ? "" : "s");
    g_string_append (out,
        " - \"not available\" names what this driver or kernel does not publish; nothing is wrong.\n"
        " - None of the kernel-side figures needs root. Nothing is read of other users'\n"
        "   processes; of yours only the process name and the DRM statistics.\n");
}

/* "12.3 % (9.0 .. 31.0)" */
static void append_range (GString *out, const WalpeGpuStatVal *v, const char *unit)
{
    g_string_append_printf (out, "%.1f%s", v->avg, unit);
    if (v->n > 1 && v->max - v->min >= 0.05)
        g_string_append_printf (out, " (%.1f .. %.1f)", v->min, v->max);
}

void walpe_gpu_stat_format_profile (const WalpeGpuStatReport *r, GString *out)
{
    const WalpeGpuStatVal *m = r->m;
    g_string_append (out, "GPU as the kernel accounts it: ");
    walpe_gpu_stat_format_device (r, out);
    g_string_append (out, "\n   sources: ");
    walpe_gpu_stat_format_sources (r, out);
    g_string_append_printf (out, " | %u samples over %.1f s", r->samples, r->seconds);
    if (r->how_found[0])
        g_string_append_printf (out, " | device chosen as: %s", r->how_found);
    if (r->n_devices > 1)
        g_string_append_printf (out, " (%u GPUs in this machine)", r->n_devices);
    g_string_append_c (out, '\n');

    /* this process */
    g_string_append (out, "   walpe-daemon: ");
    if (m[WALPE_GPU_M_PROC_BUSY].src != WALPE_GPU_SRC_NONE) {
        g_string_append_printf (out, "engine %s busy for it ", r->main_engine);
        append_range (out, &m[WALPE_GPU_M_PROC_BUSY], " %");
        g_string_append (out, " of the time");
        for (guint i = 0; i < r->n_engines; i++) {
            const WalpeGpuEngineStat *e = &r->engines[i];
            if (e->has_target && strcmp (e->name, r->main_engine) && e->proc_max_pct > 0)
                g_string_append_printf (out, ", %s %.1f %%", e->name, e->proc_pct);
        }
    } else {
        g_string_append_printf (out, "GPU share not available (%s)", m[WALPE_GPU_M_PROC_BUSY].why);
    }
    g_string_append (out, " | ");
    if (m[WALPE_GPU_M_PROC_MEM].src != WALPE_GPU_SRC_NONE) {
        g_string_append_printf (out, "GPU memory %.1f MiB %s", m[WALPE_GPU_M_PROC_MEM].last,
                                r->mem_is_total ? "allocated" : "resident");
        gboolean first = TRUE;
        for (guint i = 0; i < r->n_regions; i++) {
            gint64 kb = r->mem_is_total ? r->regions[i].total_kb : r->regions[i].resident_kb;
            if (kb <= 0)
                continue;
            g_string_append_printf (out, "%s%s %.1f", first ? " (" : ", ", r->regions[i].name, kb / 1024.0);
            first = FALSE;
        }
        if (!first)
            g_string_append_c (out, ')');
        if (r->proc_evicted_kb > 0)
            g_string_append_printf (out, ", %.1f MiB moved out of VRAM for lack of room",
                                    r->proc_evicted_kb / 1024.0);
    } else {
        g_string_append_printf (out, "GPU memory not available (%s)", m[WALPE_GPU_M_PROC_MEM].why);
    }
    g_string_append_c (out, '\n');

    /* the whole GPU: what there is, then what there is not */
    g_string_append (out, "   whole GPU:");
    gboolean any = FALSE;
    GString *na = g_string_new (NULL);
#define PART(metric, label, unit) \
    if (m[metric].src != WALPE_GPU_SRC_NONE) { \
        g_string_append_printf (out, "%s %s ", any ? " |" : "", label); \
        append_range (out, &m[metric], unit); \
        g_string_append_printf (out, " [%s]", walpe_gpu_src_name (m[metric].src)); \
        any = TRUE; \
    } else { \
        g_string_append_printf (na, "%s%s (%s)", na->len ? "; " : "", label, m[metric].why); \
    }
    PART (WALPE_GPU_M_BUSY, "busy", " %")
    if (r->others_read) {
        PART (WALPE_GPU_M_BUSY_CLIENTS, r->m[WALPE_GPU_M_BUSY_CLIENTS].lower_bound
              ? "visible clients busy (a lower bound)" : "visible clients busy", " %")
    }
    PART (WALPE_GPU_M_VRAM_USED, "VRAM used", " MiB")
    PART (WALPE_GPU_M_POWER, "power", " W")
    PART (WALPE_GPU_M_TEMP, "temperature", " C")
    PART (WALPE_GPU_M_CLOCK, "clock", " MHz")
#undef PART
    if (!any)
        g_string_append (out, " no figure");
    g_string_append_c (out, '\n');
    if (m[WALPE_GPU_M_VRAM_TOTAL].src != WALPE_GPU_SRC_NONE || m[WALPE_GPU_M_CLOCK_MAX].src != WALPE_GPU_SRC_NONE) {
        g_string_append (out, "   limits:");
        if (m[WALPE_GPU_M_VRAM_TOTAL].src != WALPE_GPU_SRC_NONE)
            g_string_append_printf (out, " VRAM %.0f MiB", m[WALPE_GPU_M_VRAM_TOTAL].last);
        if (m[WALPE_GPU_M_CLOCK_MAX].src != WALPE_GPU_SRC_NONE)
            g_string_append_printf (out, "%s highest clock %.0f MHz",
                                    m[WALPE_GPU_M_VRAM_TOTAL].src != WALPE_GPU_SRC_NONE ? " |" : "",
                                    m[WALPE_GPU_M_CLOCK_MAX].last);
        g_string_append_c (out, '\n');
    }
    if (na->len)
        g_string_append_printf (out, "   not available: %s\n", na->str);
    g_string_free (na, TRUE);

    /* the other programs, by name */
    if (r->others_read) {
        guint listed = 0;
        for (guint i = 0; i < r->n_procs && listed < 6; i++) {
            const WalpeGpuProcStat *p = &r->procs[i];
            if (p->target)
                continue;
            g_string_append_printf (out, "%s%s", listed ? ", " : "   other programs: ",
                                    p->name[0] ? p->name : "?");
            if (p->has_busy)
                g_string_append_printf (out, " %.1f %%", p->busy_pct);
            if (p->mem_kb >= 0)
                g_string_append_printf (out, " %.0f MiB", p->mem_kb / 1024.0);
            listed++;
        }
        if (listed)
            g_string_append_c (out, '\n');
        append_hidden (r, out, "   ");
    }
    g_string_append (out, "   (the rows above are GL timer queries, the same on every driver; these "
                     "figures are the kernel's\n    and cover everything the process does on the GPU, "
                     "video decoding included)\n");
}

static void jv (JsonBuilder *b, const char *k, const WalpeGpuStatVal *v)
{
    json_builder_set_member_name (b, k);
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "source");
    json_builder_add_string_value (b, walpe_gpu_src_name (v->src));
    if (v->src == WALPE_GPU_SRC_NONE) {
        json_builder_set_member_name (b, "why");
        json_builder_add_string_value (b, v->why);
    } else {
        json_builder_set_member_name (b, "min");
        json_builder_add_double_value (b, v->min);
        json_builder_set_member_name (b, "avg");
        json_builder_add_double_value (b, v->avg);
        json_builder_set_member_name (b, "max");
        json_builder_add_double_value (b, v->max);
        json_builder_set_member_name (b, "last");
        json_builder_add_double_value (b, v->last);
        json_builder_set_member_name (b, "samples");
        json_builder_add_int_value (b, v->n);
        if (v->lower_bound) {
            json_builder_set_member_name (b, "lower_bound");
            json_builder_add_boolean_value (b, TRUE);
        }
    }
    json_builder_end_object (b);
}

static void js (JsonBuilder *b, const char *k, const char *v)
{
    json_builder_set_member_name (b, k);
    json_builder_add_string_value (b, v ? v : "");
}

static void ji (JsonBuilder *b, const char *k, gint64 v)
{
    json_builder_set_member_name (b, k);
    json_builder_add_int_value (b, v);
}

void walpe_gpu_stat_to_json (const WalpeGpuStatReport *r, JsonBuilder *b)
{
    static const char *const names[WALPE_GPU_N_METRICS] = {
        "gpu_busy_percent", "visible_clients_busy_percent", "gpu_busy_at_full_clock_percent",
        "memory_busy_percent", "vram_used_mib", "vram_total_mib", "gtt_used_mib", "gtt_total_mib",
        "power_w", "temperature_c", "clock_mhz", "clock_max_mhz", "memory_clock_mhz",
        "process_busy_percent", "process_memory_mib", "other_programs_busy_percent",
    };
    char hex[16];
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "device");
    json_builder_begin_object (b);
    js (b, "driver", r->driver);
    js (b, "driver_version", r->driver_version);
    js (b, "pci_slot", r->pci_slot);
    js (b, "card", r->card);
    js (b, "render_node", r->render_node);
    js (b, "vendor", r->vendor_name);
    g_snprintf (hex, sizeof (hex), "%04x:%04x", r->vendor_id, r->device_id);
    js (b, "pci_id", r->vendor_id ? hex : "");
    js (b, "name", r->name);
    js (b, "chosen_as", r->how_found);
    ji (b, "gpus_in_machine", r->n_devices);
    json_builder_end_object (b);
    json_builder_set_member_name (b, "sources");
    json_builder_begin_array (b);
    for (int i = 1; i < WALPE_GPU_N_SRC; i++)
        if (i != WALPE_GPU_SRC_DERIVED && (r->sources & (1u << i)))
            json_builder_add_string_value (b, walpe_gpu_src_name (i));
    json_builder_end_array (b);
    json_builder_set_member_name (b, "seconds");
    json_builder_add_double_value (b, r->seconds);
    ji (b, "samples", r->samples);
    ji (b, "process", r->target_pid);
    js (b, "process_name", r->target_name);
    js (b, "main_engine", r->main_engine);
    json_builder_set_member_name (b, "values");
    json_builder_begin_object (b);
    for (int i = 0; i < WALPE_GPU_N_METRICS; i++)
        jv (b, names[i], &r->m[i]);
    json_builder_end_object (b);
    json_builder_set_member_name (b, "engines");
    json_builder_begin_array (b);
    for (guint i = 0; i < r->n_engines; i++) {
        const WalpeGpuEngineStat *e = &r->engines[i];
        json_builder_begin_object (b);
        js (b, "name", e->name);
        ji (b, "capacity", e->capacity);
        js (b, "unit", e->cycles ? "cycles" : "ns");
        json_builder_set_member_name (b, "process_uses_it");
        json_builder_add_boolean_value (b, e->has_target);
        json_builder_set_member_name (b, "process_percent");
        json_builder_add_double_value (b, e->proc_pct);
        json_builder_set_member_name (b, "process_percent_max");
        json_builder_add_double_value (b, e->proc_max_pct);
        json_builder_set_member_name (b, "visible_clients_percent");
        json_builder_add_double_value (b, e->all_pct);
        ji (b, "counter_resets", e->resets);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    json_builder_set_member_name (b, "process_memory_regions");
    json_builder_begin_array (b);
    for (guint i = 0; i < r->n_regions; i++) {
        const WalpeGpuFdRegion *g = &r->regions[i];
        json_builder_begin_object (b);
        js (b, "name", g->name);
        ji (b, "resident_kib", g->resident_kb);
        ji (b, "total_kib", g->total_kb);
        ji (b, "shared_kib", g->shared_kb);
        ji (b, "active_kib", g->active_kb);
        ji (b, "purgeable_kib", g->purgeable_kb);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    ji (b, "process_vram_evicted_kib", r->proc_evicted_kb);
    json_builder_set_member_name (b, "programs");
    json_builder_begin_array (b);
    for (guint i = 0; i < r->n_procs; i++) {
        const WalpeGpuProcStat *p = &r->procs[i];
        json_builder_begin_object (b);
        js (b, "name", p->name);
        json_builder_set_member_name (b, "measured");
        json_builder_add_boolean_value (b, p->target);
        json_builder_set_member_name (b, "busy_percent");
        json_builder_add_double_value (b, p->has_busy ? p->busy_pct : -1.0);
        ji (b, "memory_kib", p->mem_kb);
        ji (b, "clients", p->clients);
        json_builder_set_member_name (b, "exited");
        json_builder_add_boolean_value (b, p->exited);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    json_builder_set_member_name (b, "not_visible");
    json_builder_begin_object (b);
    ji (b, "processes_of_other_users", r->procs_other_users);
    ji (b, "own_processes_not_readable", r->procs_denied);
    json_builder_end_object (b);
    ji (b, "clients_seen_through_several_descriptors", r->clients_shared);
    ji (b, "clients_without_id", r->clients_no_id);
    ji (b, "counter_resets", r->counter_resets);
    json_builder_end_object (b);
}

/* ============================== 8. the calling process (status line) */

static gint self_readings;        /* atomic: walpe_gpustat_self_readings */

guint walpe_gpustat_self_readings (void)
{
    return (guint) g_atomic_int_get (&self_readings);
}

void walpe_gpustat_self (WalpeGpuSelfStat *out)
{
    static GMutex lock;
    static WalpeGpuSampler *self;
    static WalpeGpuSelfStat last;
    static gint64 last_us;
    static gboolean settled;      /* the answer is final: nothing is read any more */
    static gint gen_seen;

    g_mutex_lock (&lock);
    /* told another renderer since (the daemon learns it after EGL is up):
     * what was settled for the old one does not hold */
    gint gen = g_atomic_int_get (&hint_gen);
    if (gen != gen_seen) {
        gen_seen = gen;
        settled = FALSE;
    }
    if (settled) {
        *out = last;
        g_mutex_unlock (&lock);
        return;
    }
    gint64 now = g_get_monotonic_time ();
    if (!self) {
        /* this process alone, and none of the device's attributes: a status
         * request must stay cheap */
        WalpeGpuStatOpts o = { .target_pid = -1, .uid = -1, .flags = WALPE_GPUSTAT_NO_DEVICE };
        self = walpe_gpu_sampler_new (&o);
        walpe_gpu_sampler_tick (self);   /* the baseline */
        g_atomic_int_inc (&self_readings);
        last_us = now;
        memset (&last, 0, sizeof (last));
        last.mem_kb = -1;
    }
    if (now - last_us >= G_USEC_PER_SEC || !last.sources[0]) {
        if (now - last_us >= G_USEC_PER_SEC) {
            walpe_gpu_sampler_tick (self);
            g_atomic_int_inc (&self_readings);
        }
        WalpeGpuStatReport *r = walpe_gpu_sampler_report (self);
        const gboolean two = r->samples > 1;
        memset (&last, 0, sizeof (last));
        g_strlcpy (last.driver, r->driver, sizeof (last.driver));
        g_strlcpy (last.pci_slot, r->pci_slot, sizeof (last.pci_slot));
        g_strlcpy (last.card, r->card, sizeof (last.card));
        g_strlcpy (last.vendor_name, r->vendor_name, sizeof (last.vendor_name));
        last.vendor_id = r->vendor_id;
        last.device_id = r->device_id;
        GString *src = g_string_new (NULL);
        walpe_gpu_stat_format_sources (r, src);
        g_strlcpy (last.sources, src->str, sizeof (last.sources));
        g_string_free (src, TRUE);
        const WalpeGpuStatVal *b = &r->m[WALPE_GPU_M_PROC_BUSY], *m = &r->m[WALPE_GPU_M_PROC_MEM];
        last.has_busy = b->src != WALPE_GPU_SRC_NONE;
        last.busy_pct = last.has_busy ? b->avg : 0.0;
        last.busy_window_s = r->seconds;
        g_strlcpy (last.engine, r->main_engine, sizeof (last.engine));
        last.mem_kb = m->src != WALPE_GPU_SRC_NONE ? (gint64) (m->last * 1024.0) : -1;
        if (!last.has_busy && r->samples > 1)
            g_strlcpy (last.note, b->why, sizeof (last.note));
        else if (last.mem_kb < 0)
            g_strlcpy (last.note, m->why, sizeof (last.note));
        walpe_gpu_stat_report_free (r);
        if (now - last_us >= G_USEC_PER_SEC) {
            /* the next reading covers the time from here */
            walpe_gpu_sampler_restart (self);
            last_us = now;
        }
        /* Nothing more can come where the kernel lists no GPU for the
         * renderer, or lists it under NVIDIA's driver, which puts no figure
         * into fdinfo (the profile section is off there for the same
         * reason: walpe_gpustat_profile_kernel): the answer of the second
         * reading - device, "sources: none", the note that says why - is
         * the answer from then on, and a status request reads no file. */
        if (two && !last.has_busy && last.mem_kb < 0 &&
            (!last.driver[0] || !strcmp (last.driver, "nvidia"))) {
            settled = TRUE;
            walpe_gpu_sampler_free (self);
            self = NULL;
        }
    }
    *out = last;
    g_mutex_unlock (&lock);
}
