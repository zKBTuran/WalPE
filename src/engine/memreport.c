/* memreport.c - `walpe-ctl mem` (memreport.h): the process as the kernel and
 * the allocator see it, and a census of the GL objects of a context.
 * Everything is computed when asked for. */
#include "walpe-build-config.h"
#include "memreport.h"
#include "glutil.h"   /* walpe_gl_is_software */
#include <epoxy/egl.h>   /* eglGetProcAddress: the GL events */
#include <epoxy/gl.h>
#include <glib/gstdio.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------ counters */

gint walpe_memc_[WALPE_MEMC_N];

static const char *const memc_names[WALPE_MEMC_N] = {
    [WALPE_MEMC_IPC] = "ipc_requests",
    [WALPE_MEMC_STATUS] = "status_built",
    [WALPE_MEMC_STATE_PUSH] = "status_built_for_subscribers",
    [WALPE_MEMC_MEM] = "mem_reports",
    [WALPE_MEMC_PAUSE] = "pauses",
    [WALPE_MEMC_RESUME] = "resumes",
    [WALPE_MEMC_LOAD] = "loads",
    [WALPE_MEMC_LOAD_FAILED] = "loads_failed",
    [WALPE_MEMC_IDLE_RELEASE] = "idle_releases",
    [WALPE_MEMC_ROLE] = "shared_role_changes",
    [WALPE_MEMC_PAUSED_FRAME] = "frames_drawn_while_paused",
    [WALPE_MEMC_TARGET_DROP] = "scene_target_drops",
    [WALPE_MEMC_TEXT_RENDER] = "text_renders",
    [WALPE_MEMC_FX_COMPARE] = "fx_compares",
};

const char *walpe_memc_name (WalpeMemCounter c)
{
    return (guint) c < WALPE_MEMC_N ? memc_names[c] : NULL;
}

void walpe_mem_jint (JsonBuilder *b, const char *key, gint64 v)
{
    json_builder_set_member_name (b, key);
    json_builder_add_int_value (b, v);
}

void walpe_mem_jstr (JsonBuilder *b, const char *key, const char *v)
{
    json_builder_set_member_name (b, key);
    json_builder_add_string_value (b, v ? v : "");
}

void walpe_mem_counters_json (JsonBuilder *b)
{
    for (int i = 0; i < WALPE_MEMC_N; i++)
        walpe_mem_jint (b, memc_names[i], (guint) g_atomic_int_get (&walpe_memc_[i]));
}

/* ------------------------------------------------------------- process */

static const char *const map_names[WALPE_MAP_N] = {
    [WALPE_MAP_HEAP] = "heap",     [WALPE_MAP_ANON] = "anonymous", [WALPE_MAP_STACK] = "stack",
    [WALPE_MAP_NVIDIA] = "nvidia", [WALPE_MAP_DRI] = "dri",        [WALPE_MAP_MEMFD] = "memfd",
    [WALPE_MAP_SHM] = "shm",       [WALPE_MAP_LIB] = "libraries",  [WALPE_MAP_FILE] = "files",
    [WALPE_MAP_OTHER] = "other",
};

const char *walpe_mem_map_kind_name (int kind)
{
    return kind >= 0 && kind < WALPE_MAP_N ? map_names[kind] : "?";
}

int walpe_mem_map_kind (const char *name)
{
    if (!name || !*name)
        return WALPE_MAP_ANON;
    if (!strcmp (name, "[heap]"))
        return WALPE_MAP_HEAP;
    if (g_str_has_prefix (name, "[stack"))
        return WALPE_MAP_STACK;
    if (g_str_has_prefix (name, "[anon"))      /* named by its owner: "[anon: glibc: malloc]" */
        return WALPE_MAP_ANON;
    if (name[0] == '[')
        return WALPE_MAP_OTHER;
    if (g_str_has_prefix (name, "/dev/nvidia"))
        return WALPE_MAP_NVIDIA;
    /* (a kernel driver's buffer objects appear under several names: the
     * device node, "/drm mm object", "/i915", a dma-buf) */
    if (g_str_has_prefix (name, "/dev/dri/") || g_str_has_prefix (name, "/dmabuf") ||
        g_str_has_prefix (name, "anon_inode:dmabuf") || g_str_has_prefix (name, "anon_inode:i915") ||
        g_str_has_prefix (name, "anon_inode:drm") || g_str_has_prefix (name, "/drm mm object") ||
        g_str_has_prefix (name, "/i915"))
        return WALPE_MAP_DRI;
    if (g_str_has_prefix (name, "/memfd:"))
        return WALPE_MAP_MEMFD;
    /* "/dev/zero (deleted)" is what a shared anonymous mapping is called */
    if (g_str_has_prefix (name, "/dev/shm/") || g_str_has_prefix (name, "/SYSV") ||
        g_str_has_prefix (name, "/dev/zero"))
        return WALPE_MAP_SHM;
    if (name[0] != '/')
        return WALPE_MAP_OTHER;
    /* a shared library ("libfoo.so", "libfoo.so.1.2", "... (deleted)") or
     * the program: what the loader mapped. Other files are data. */
    const char *base = strrchr (name, '/');
    base = base ? base + 1 : name;
    const char *so = strstr (base, ".so");
    if (so && (so[3] == '\0' || so[3] == '.' || so[3] == ' '))
        return WALPE_MAP_LIB;
    return WALPE_MAP_FILE;
}

/* a line "7f00-7f10 rw-p 00000000 00:00 0   [name]" (and not "Rss:  4 kB") */
static gboolean smaps_is_header (const char *l)
{
    const char *p = l;
    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))
        p++;
    return p > l && *p == '-';
}

static void top_anon_add (WalpeMemMaps *m, gint64 size, gint64 rss, const char *perms)
{
    for (int i = 0; i < WALPE_MEM_TOP; i++) {
        if (rss <= m->top_anon[i].rss_kb)
            continue;
        memmove (&m->top_anon[i + 1], &m->top_anon[i],
                 sizeof (m->top_anon[0]) * (gsize) (WALPE_MEM_TOP - 1 - i));
        m->top_anon[i].size_kb = size;
        m->top_anon[i].rss_kb = rss;
        g_strlcpy (m->top_anon[i].perms, perms, sizeof (m->top_anon[i].perms));
        return;
    }
}

static void top_file_add (WalpeMemMaps *m, gint64 size, gint64 rss, const char *name)
{
    const char *base = strrchr (name, '/');
    base = base ? base + 1 : name;
    /* the parts of one file (text, data, relocations) count as one */
    for (int i = 0; i < WALPE_MEM_TOP; i++)
        if (m->top_file[i].rss_kb > 0 && !strncmp (m->top_file[i].name, base,
                                                   sizeof (m->top_file[i].name) - 1)) {
            size += m->top_file[i].size_kb;
            rss += m->top_file[i].rss_kb;
            memmove (&m->top_file[i], &m->top_file[i + 1],
                     sizeof (m->top_file[0]) * (gsize) (WALPE_MEM_TOP - 1 - i));
            memset (&m->top_file[WALPE_MEM_TOP - 1], 0, sizeof (m->top_file[0]));
            break;
        }
    for (int i = 0; i < WALPE_MEM_TOP; i++) {
        if (rss <= m->top_file[i].rss_kb)
            continue;
        memmove (&m->top_file[i + 1], &m->top_file[i],
                 sizeof (m->top_file[0]) * (gsize) (WALPE_MEM_TOP - 1 - i));
        m->top_file[i].size_kb = size;
        m->top_file[i].rss_kb = rss;
        g_strlcpy (m->top_file[i].name, base, sizeof (m->top_file[i].name));
        return;
    }
}

typedef struct {
    int kind;       /* -1 = no mapping open */
    gint64 size, rss, dirty, swap, anon, shared, huge[3];   /* huge: anon, shmem, file */
    char perms[5], name[512];
    GHashTable *by_name, *by_size;   /* NULL or row name -> WalpeMemMapRow */
} SmapsCur;

static void row_add (GHashTable *rows, const char *name, const SmapsCur *c)
{
    WalpeMemMapRow *r = g_hash_table_lookup (rows, name);
    if (!r) {
        r = g_new0 (WalpeMemMapRow, 1);
        g_strlcpy (r->name, name, sizeof (r->name));
        r->kind = c->kind;
        g_hash_table_insert (rows, r->name, r);
    }
    /* what is resident: the anonymous pages, and the pages of the object
     * behind the mapping - shared memory or a file */
    const gboolean no_object = c->kind == WALPE_MAP_HEAP || c->kind == WALPE_MAP_ANON ||
                               c->kind == WALPE_MAP_STACK;
    const gboolean shmem = c->kind == WALPE_MAP_SHM || c->kind == WALPE_MAP_MEMFD ||
                           g_str_has_prefix (c->name, "/drm mm object") ||
                           g_str_has_prefix (c->name, "/i915");
    const gint64 anon = no_object ? c->rss : MIN (c->anon, c->rss);
    r->n++;
    r->size_kb += c->size;
    r->rss_kb += c->rss;
    r->anon_kb += anon;
    if (shmem)
        r->shmem_kb += c->rss - anon;
    else
        r->file_kb += c->rss - anon;
    r->shared_kb += c->shared;
    r->swap_kb += c->swap;
    r->huge_kb += c->huge[0] + c->huge[1] + c->huge[2];
}

static void smaps_flush (WalpeMemMaps *out, SmapsCur *c)
{
    if (c->kind < 0)
        return;
    if (c->by_name || c->by_size) {
        char name[96];
        const char *base = strrchr (c->name, '/');
        if (c->kind == WALPE_MAP_HEAP || c->kind == WALPE_MAP_ANON)
            g_strlcpy (name, c->name[0] ? c->name : "[anon]", sizeof (name));
        else if (c->kind == WALPE_MAP_LIB || c->kind == WALPE_MAP_FILE)
            g_strlcpy (name, base ? base + 1 : c->name, sizeof (name));   /* no directories */
        else
            g_strlcpy (name, c->name, sizeof (name));
        if (c->by_name)
            row_add (c->by_name, name, c);
        if (c->by_size && (c->kind == WALPE_MAP_ANON || c->kind == WALPE_MAP_NVIDIA ||
                           c->kind == WALPE_MAP_DRI || c->kind == WALPE_MAP_MEMFD ||
                           c->kind == WALPE_MAP_SHM)) {
            char sized[96];
            name[sizeof (name) - 24] = '\0';
            g_snprintf (sized, sizeof (sized), "%s %" G_GINT64_FORMAT " kB", name, c->size);
            row_add (c->by_size, sized, c);
        }
    }
    WalpeMemKind *k = &out->kind[c->kind];
    k->n++;
    k->size_kb += c->size;
    k->rss_kb += c->rss;
    k->dirty_kb += c->dirty;
    k->swap_kb += c->swap;
    out->n_maps++;
    out->huge_anon_kb += c->huge[0];
    out->huge_shmem_kb += c->huge[1];
    out->huge_file_kb += c->huge[2];
    if (c->kind == WALPE_MAP_ANON || c->kind == WALPE_MAP_HEAP)
        top_anon_add (out, c->size, c->rss, c->perms);
    else if (c->kind == WALPE_MAP_FILE || c->kind == WALPE_MAP_LIB || c->kind == WALPE_MAP_MEMFD ||
             c->kind == WALPE_MAP_SHM)
        top_file_add (out, c->size, c->rss, c->name);
    c->kind = -1;
}

static gint map_row_cmp (gconstpointer a, gconstpointer b)
{
    const WalpeMemMapRow *x = a, *y = b;
    if (x->rss_kb != y->rss_kb)
        return x->rss_kb > y->rss_kb ? -1 : 1;
    if (x->size_kb != y->size_kb)
        return x->size_kb > y->size_kb ? -1 : 1;
    return strcmp (x->name, y->name);
}

static GArray *map_rows_take (GHashTable *rows)
{
    GArray *a = g_array_new (FALSE, TRUE, sizeof (WalpeMemMapRow));
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init (&it, rows);
    while (g_hash_table_iter_next (&it, NULL, &v))
        g_array_append_vals (a, v, 1);
    g_hash_table_destroy (rows);
    g_array_sort (a, map_row_cmp);
    return a;
}

void walpe_mem_smaps_parse (const char *text, const char *exe, WalpeMemMaps *out)
{
    walpe_mem_smaps_tables (text, exe, out, NULL, NULL);
}

void walpe_mem_smaps_tables (const char *text, const char *exe, WalpeMemMaps *out, GArray **by_name,
                             GArray **by_size)
{
    WalpeMemMaps scratch;
    if (!out)
        out = &scratch;
    memset (out, 0, sizeof (*out));
    SmapsCur c = { .kind = -1 };
    if (by_name)
        c.by_name = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
    if (by_size)
        c.by_size = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
    const char *p = text ? text : "";
    for (;;) {
        const char *e = strchr (p, '\n');
        gsize len = e ? (gsize) (e - p) : strlen (p);
        if (len > 0 && smaps_is_header (p)) {
            smaps_flush (out, &c);   /* the mapping before this line is complete */
            /* address perms offset dev inode [name] */
            char line[640];
            g_strlcpy (line, p, MIN (len + 1, sizeof (line)));
            char *f = line;
            int col = 0;
            c.name[0] = c.perms[0] = '\0';
            while (*f && col < 5) {
                char *sp = strchr (f, ' ');
                if (col == 1)
                    g_strlcpy (c.perms, f, MIN ((gsize) ((sp ? sp : f + strlen (f)) - f) + 1,
                                                sizeof (c.perms)));
                if (!sp)
                    break;
                f = sp;
                while (*f == ' ')
                    f++;
                col++;
            }
            if (col == 5)
                g_strlcpy (c.name, f, sizeof (c.name));
            g_strchomp (c.name);
            c.kind = walpe_mem_map_kind (c.name);
            if (c.kind == WALPE_MAP_FILE && exe && !strcmp (c.name, exe))
                c.kind = WALPE_MAP_LIB;   /* the program itself */
            c.size = c.rss = c.dirty = c.swap = c.anon = c.shared = 0;
            c.huge[0] = c.huge[1] = c.huge[2] = 0;
        } else if (c.kind >= 0 && len > 0) {
            static const struct { const char *key; int which; } keys[] = {
                { "Size:", 0 }, { "Rss:", 1 }, { "Private_Dirty:", 2 }, { "Swap:", 3 },
                { "Anonymous:", 4 }, { "Shared_Clean:", 5 }, { "Shared_Dirty:", 5 },
                { "AnonHugePages:", 6 }, { "ShmemPmdMapped:", 7 }, { "FilePmdMapped:", 8 },
            };
            for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
                gsize kl = strlen (keys[i].key);
                if (len <= kl || strncmp (p, keys[i].key, kl))
                    continue;
                gint64 v = g_ascii_strtoll (p + kl, NULL, 10);
                if (keys[i].which == 0)
                    c.size = v;
                else if (keys[i].which == 1)
                    c.rss = v;
                else if (keys[i].which == 2)
                    c.dirty = v;
                else if (keys[i].which == 3)
                    c.swap = v;
                else if (keys[i].which == 4)
                    c.anon = v;
                else if (keys[i].which == 5)
                    c.shared += v;
                else
                    c.huge[keys[i].which - 6] = v;
                break;
            }
        }
        if (!e)
            break;
        p = e + 1;
    }
    smaps_flush (out, &c);
    if (by_name)
        *by_name = map_rows_take (c.by_name);
    if (by_size)
        *by_size = map_rows_take (c.by_size);
}

static gint64 status_kb (const char *st, const char *key)
{
    /* at the start of a line: "RssAnon:" is not found inside another key */
    gsize kl = strlen (key);
    for (const char *p = st; p && *p; ) {
        if (!strncmp (p, key, kl))
            return g_ascii_strtoll (p + kl, NULL, 10);
        p = strchr (p, '\n');
        if (p)
            p++;
    }
    return -1;
}

static void fds_json (JsonBuilder *b)
{
    enum { F_SOCKET, F_PIPE, F_ANON, F_NVIDIA, F_DRI, F_MEMFD, F_SHM, F_FILE, F_OTHER, F_N };
    static const char *const names[F_N] = { "sockets", "pipes", "anon_inodes", "nvidia", "dri",
                                            "memfd", "shm", "files", "other" };
    int n[F_N] = { 0 }, total = 0;
    GHashTable *anon = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GDir *d = g_dir_open ("/proc/self/fd", 0, NULL);
    const char *e;
    while (d && (e = g_dir_read_name (d))) {
        char path[64], to[256];
        g_snprintf (path, sizeof (path), "/proc/self/fd/%s", e);
        ssize_t l = readlink (path, to, sizeof (to) - 1);
        if (l < 0)
            continue;   /* (the directory's own descriptor is gone by now) */
        to[l] = '\0';
        total++;
        int k = F_OTHER;
        if (g_str_has_prefix (to, "socket:"))
            k = F_SOCKET;
        else if (g_str_has_prefix (to, "pipe:"))
            k = F_PIPE;
        else if (g_str_has_prefix (to, "anon_inode:")) {
            k = F_ANON;
            /* "[eventfd]", "[eventpoll]", "[timerfd]", "sync_file", "dmabuf" ... */
            const char *what = to + strlen ("anon_inode:");
            gpointer c = g_hash_table_lookup (anon, what);
            g_hash_table_insert (anon, g_strdup (what), GINT_TO_POINTER (GPOINTER_TO_INT (c) + 1));
        } else if (g_str_has_prefix (to, "/dev/nvidia"))
            k = F_NVIDIA;
        else if (g_str_has_prefix (to, "/dev/dri/") || g_str_has_prefix (to, "/dmabuf"))
            k = F_DRI;
        else if (g_str_has_prefix (to, "/memfd:"))
            k = F_MEMFD;
        else if (g_str_has_prefix (to, "/dev/shm/"))
            k = F_SHM;
        else if (to[0] == '/' && !g_str_has_prefix (to, "/dev/") && !g_str_has_prefix (to, "/proc/"))
            k = F_FILE;
        n[k]++;
    }
    if (d) {
        g_dir_close (d);
        total--;        /* the descriptor g_dir_open used */
        n[F_OTHER] = MAX (0, n[F_OTHER] - 1);
    }
    json_builder_set_member_name (b, "fds");
    json_builder_begin_object (b);
    walpe_mem_jint (b, "count", MAX (total, 0));
    for (int i = 0; i < F_N; i++)
        walpe_mem_jint (b, names[i], n[i]);
    json_builder_set_member_name (b, "anon_inode_kinds");
    json_builder_begin_object (b);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, anon);
    while (g_hash_table_iter_next (&it, &k, &v))
        walpe_mem_jint (b, k, GPOINTER_TO_INT (v));
    json_builder_end_object (b);
    json_builder_end_object (b);
    g_hash_table_destroy (anon);
}

static void malloc_json (JsonBuilder *b)
{
    json_builder_set_member_name (b, "malloc");
    json_builder_begin_object (b);
#if defined (__GLIBC__)
#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33)
    struct mallinfo2 mi = mallinfo2 ();
#else
    struct mallinfo mi = mallinfo ();   /* (counts wrap at 2 GiB here) */
#endif
    /* arena: bytes the heaps took from the system (not mmapped blocks);
     * uordblks: bytes handed out from them; fordblks: free bytes they
     * keep; hblkhd: bytes in blocks that were mmapped one by one */
    walpe_mem_jint (b, "arena_kb", (gint64) ((guint64) mi.arena / 1024));
    walpe_mem_jint (b, "in_use_kb", (gint64) ((guint64) mi.uordblks / 1024));
    walpe_mem_jint (b, "free_kept_kb", (gint64) ((guint64) mi.fordblks / 1024));
    walpe_mem_jint (b, "mmapped_kb", (gint64) ((guint64) mi.hblkhd / 1024));
    walpe_mem_jint (b, "mmapped_blocks", (gint64) mi.hblks);
    walpe_mem_jint (b, "releasable_kb", (gint64) ((guint64) mi.keepcost / 1024));
    {
        /* the number of arenas: malloc_info () prints one <heap> each */
        char *xml = NULL;
        size_t xl = 0;
        FILE *f = open_memstream (&xml, &xl);
        int arenas = -1;
        if (f) {
            if (malloc_info (0, f) == 0) {
                fflush (f);
                arenas = 0;
                for (const char *p = xml; p && (p = strstr (p, "<heap nr=")); p++)
                    arenas++;
            }
            fclose (f);
        }
        free (xml);
        walpe_mem_jint (b, "arenas", arenas);
    }
#else
    walpe_mem_jstr (b, "note", "this C library has no mallinfo");
#endif
    json_builder_end_object (b);
}

/* "by_name", "by_size" and what the rows add up to (to hold against
 * RssAnon / RssFile / RssShmem of "process": where they differ, the rule
 * that sorts a mapping's pages into file or shared memory is wrong for
 * some mapping of this machine - the rows show which) */
static void map_rows_json (JsonBuilder *b, GArray *by_name, GArray *by_size)
{
    gint64 anon = 0, file = 0, shmem = 0;
    json_builder_set_member_name (b, "by_name");
    json_builder_begin_array (b);
    for (guint i = 0; i < by_name->len; i++) {
        const WalpeMemMapRow *r = &g_array_index (by_name, WalpeMemMapRow, i);
        anon += r->anon_kb;
        file += r->file_kb;
        shmem += r->shmem_kb;
        /* (a library or file nothing of which is resident says nothing) */
        if (r->rss_kb == 0 && r->swap_kb == 0 && (r->kind == WALPE_MAP_LIB || r->kind == WALPE_MAP_FILE))
            continue;
        json_builder_begin_object (b);
        walpe_mem_jstr (b, "name", r->name);
        walpe_mem_jstr (b, "kind", walpe_mem_map_kind_name (r->kind));
        walpe_mem_jint (b, "count", r->n);
        walpe_mem_jint (b, "size_kb", r->size_kb);
        walpe_mem_jint (b, "rss_kb", r->rss_kb);
        walpe_mem_jint (b, "anon_kb", r->anon_kb);
        walpe_mem_jint (b, "file_kb", r->file_kb);
        walpe_mem_jint (b, "shmem_kb", r->shmem_kb);
        walpe_mem_jint (b, "shared_kb", r->shared_kb);
        if (r->swap_kb)
            walpe_mem_jint (b, "swap_kb", r->swap_kb);
        if (r->huge_kb)
            walpe_mem_jint (b, "huge_kb", r->huge_kb);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    json_builder_set_member_name (b, "by_name_sum");
    json_builder_begin_object (b);
    walpe_mem_jint (b, "anon_kb", anon);
    walpe_mem_jint (b, "file_kb", file);
    walpe_mem_jint (b, "shmem_kb", shmem);
    json_builder_end_object (b);
    json_builder_set_member_name (b, "by_size");
    json_builder_begin_array (b);
    for (guint i = 0; i < by_size->len; i++) {
        const WalpeMemMapRow *r = &g_array_index (by_size, WalpeMemMapRow, i);
        json_builder_begin_object (b);
        walpe_mem_jstr (b, "name", r->name);
        walpe_mem_jint (b, "count", r->n);
        walpe_mem_jint (b, "rss_kb", r->rss_kb);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
}

/* the threads by name (/proc/self/task/<tid>/comm) */
static void threads_json (JsonBuilder *b)
{
    json_builder_set_member_name (b, "threads");
    json_builder_begin_object (b);
    GDir *d = g_dir_open ("/proc/self/task", 0, NULL);
    GHashTable *names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    guint n = 0;
    const char *e;
    while (d && (e = g_dir_read_name (d))) {
        char *path = g_strdup_printf ("/proc/self/task/%s/comm", e), *comm = NULL;
        if (g_file_get_contents (path, &comm, NULL, NULL)) {
            g_strchomp (comm);
            gpointer k = NULL, v = NULL;
            if (g_hash_table_lookup_extended (names, comm, &k, &v)) {
                g_hash_table_insert (names, comm, GUINT_TO_POINTER (GPOINTER_TO_UINT (v) + 1));
            } else {
                g_hash_table_insert (names, comm, GUINT_TO_POINTER (1));
            }
            n++;
        }
        g_free (path);
    }
    if (d)
        g_dir_close (d);
    walpe_mem_jint (b, "count", n);
    GList *keys = g_list_sort (g_hash_table_get_keys (names), (GCompareFunc) strcmp);
    json_builder_set_member_name (b, "by_name");
    json_builder_begin_array (b);
    for (GList *l = keys; l; l = l->next) {
        json_builder_begin_object (b);
        walpe_mem_jstr (b, "name", l->data);
        walpe_mem_jint (b, "count", GPOINTER_TO_UINT (g_hash_table_lookup (names, l->data)));
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    g_list_free (keys);
    g_hash_table_destroy (names);
    json_builder_end_object (b);
}

void walpe_mem_process_json (JsonBuilder *b)
{
    char *st = NULL;
    json_builder_set_member_name (b, "process");
    json_builder_begin_object (b);
    walpe_mem_jint (b, "pid", (gint64) getpid ());
    if (g_file_get_contents ("/proc/self/status", &st, NULL, NULL)) {
        static const struct { const char *key, *name; } keys[] = {
            { "VmRSS:", "rss_kb" },       { "RssAnon:", "rss_anon_kb" },
            { "RssFile:", "rss_file_kb" }, { "RssShmem:", "rss_shmem_kb" },
            { "VmHWM:", "rss_peak_kb" },   { "VmData:", "data_kb" },
            { "VmSize:", "virtual_kb" },   { "VmSwap:", "swap_kb" },
            { "VmPTE:", "page_tables_kb" }, { "Threads:", "threads" },
        };
        for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
            walpe_mem_jint (b, keys[i].name, status_kb (st, keys[i].key));
        g_free (st);
    }
    json_builder_end_object (b);

    fds_json (b);
    malloc_json (b);

    /* the mappings by kind. smaps makes the kernel walk the page tables of
     * every mapping: some milliseconds for a process of this size, which
     * is why nothing reads it unasked */
    char *sm = NULL;
    json_builder_set_member_name (b, "mappings");
    json_builder_begin_object (b);
    if (g_file_get_contents ("/proc/self/smaps", &sm, NULL, NULL)) {
        WalpeMemMaps *m = g_new0 (WalpeMemMaps, 1);
        GArray *by_name = NULL, *by_size = NULL;
        char *exe = g_file_read_link ("/proc/self/exe", NULL);
        walpe_mem_smaps_tables (sm, exe, m, &by_name, &by_size);
        g_free (exe);
        g_free (sm);
        walpe_mem_jint (b, "count", m->n_maps);
        for (int k = 0; k < WALPE_MAP_N; k++) {
            json_builder_set_member_name (b, map_names[k]);
            json_builder_begin_object (b);
            walpe_mem_jint (b, "count", m->kind[k].n);
            walpe_mem_jint (b, "size_kb", m->kind[k].size_kb);
            walpe_mem_jint (b, "rss_kb", m->kind[k].rss_kb);
            walpe_mem_jint (b, "private_dirty_kb", m->kind[k].dirty_kb);
            walpe_mem_jint (b, "swap_kb", m->kind[k].swap_kb);
            json_builder_end_object (b);
        }
        json_builder_set_member_name (b, "largest_anonymous");
        json_builder_begin_array (b);
        for (int i = 0; i < WALPE_MEM_TOP && m->top_anon[i].rss_kb > 0; i++) {
            json_builder_begin_object (b);
            walpe_mem_jint (b, "rss_kb", m->top_anon[i].rss_kb);
            walpe_mem_jint (b, "size_kb", m->top_anon[i].size_kb);
            walpe_mem_jstr (b, "perms", m->top_anon[i].perms);
            json_builder_end_object (b);
        }
        json_builder_end_array (b);
        json_builder_set_member_name (b, "largest_files");
        json_builder_begin_array (b);
        for (int i = 0; i < WALPE_MEM_TOP && m->top_file[i].rss_kb > 0; i++) {
            json_builder_begin_object (b);
            walpe_mem_jint (b, "rss_kb", m->top_file[i].rss_kb);
            walpe_mem_jint (b, "size_kb", m->top_file[i].size_kb);
            walpe_mem_jstr (b, "name", m->top_file[i].name);
            json_builder_end_object (b);
        }
        json_builder_end_array (b);
        {
            /* transparent huge pages: what is resident in 2 MiB units, and
             * the system's setting (the word in brackets is the one in
             * force: "always [madvise] never") */
            static const char *const files[] = {
                "/sys/kernel/mm/transparent_hugepage/enabled", "setting",
                "/sys/kernel/mm/transparent_hugepage/shmem_enabled", "shmem_setting",
            };
            json_builder_set_member_name (b, "huge_pages");
            json_builder_begin_object (b);
            walpe_mem_jint (b, "anon_kb", m->huge_anon_kb);
            walpe_mem_jint (b, "shmem_kb", m->huge_shmem_kb);
            walpe_mem_jint (b, "file_kb", m->huge_file_kb);
            for (guint i = 0; i < G_N_ELEMENTS (files); i += 2) {
                char *t = NULL;
                if (g_file_get_contents (files[i], &t, NULL, NULL)) {
                    g_strstrip (t);
                    if (strlen (t) < 120)
                        walpe_mem_jstr (b, files[i + 1], t);
                }
                g_free (t);
            }
            json_builder_end_object (b);
        }
        map_rows_json (b, by_name, by_size);
        g_array_unref (by_name);
        g_array_unref (by_size);
        g_free (m);
    } else {
        walpe_mem_jstr (b, "note", "/proc/self/smaps cannot be read");
    }
    json_builder_end_object (b);
    threads_json (b);
}

/* ----------------------------------------------------------- GL census */

static const struct { GLint f; const char *name; int bits; } gl_fmts[] = {
    { 0x8229, "R8", 8 },           { 0x822B, "RG8", 16 },         { 0x8051, "RGB8", 32 },
    { 0x8058, "RGBA8", 32 },       { 0x8C41, "SRGB8", 32 },       { 0x8C43, "SRGB8_A8", 32 },
    { 0x822D, "R16F", 16 },        { 0x822F, "RG16F", 32 },       { 0x881B, "RGB16F", 64 },
    { 0x881A, "RGBA16F", 64 },     { 0x822E, "R32F", 32 },        { 0x8230, "RG32F", 64 },
    { 0x8815, "RGB32F", 128 },     { 0x8814, "RGBA32F", 128 },    { 0x822A, "R16", 16 },
    { 0x822C, "RG16", 32 },        { 0x805B, "RGBA16", 64 },      { 0x8059, "RGB10_A2", 32 },
    { 0x8C3A, "R11F_G11F_B10F", 32 }, { 0x81A5, "DEPTH16", 16 },  { 0x81A6, "DEPTH24", 32 },
    { 0x88F0, "DEPTH24_STENCIL8", 32 }, { 0x8CAC, "DEPTH32F", 32 }, { 0x8D48, "STENCIL8", 8 },
    { 0x1903, "RED", 8 },          { 0x1907, "RGB", 32 },         { 0x1908, "RGBA", 32 },
    { 0x80E1, "BGRA", 32 },        { 0x1906, "ALPHA", 8 },        { 0x1909, "LUMINANCE", 8 },
    { 0x190A, "LUMINANCE_ALPHA", 16 },
    /* block compressed (their size is asked for, not computed) */
    { 0x83F0, "DXT1", 4 },         { 0x83F1, "DXT1A", 4 },        { 0x83F2, "DXT3", 8 },
    { 0x83F3, "DXT5", 8 },         { 0x8C4C, "DXT1_SRGB", 4 },    { 0x8C4D, "DXT1A_SRGB", 4 },
    { 0x8C4E, "DXT3_SRGB", 8 },    { 0x8C4F, "DXT5_SRGB", 8 },    { 0x8DBB, "RGTC1", 4 },
    { 0x8DBD, "RGTC2", 8 },        { 0x8E8C, "BC7", 8 },          { 0x8E8D, "BC7_SRGB", 8 },
    { 0x8E8E, "BC6H", 8 },         { 0x8E8F, "BC6H_U", 8 },
};

static const char *fmt_name (GLint f, char *buf, gsize len, int *bits)
{
    for (guint i = 0; i < G_N_ELEMENTS (gl_fmts); i++)
        if (gl_fmts[i].f == f) {
            if (bits)
                *bits = gl_fmts[i].bits;
            return gl_fmts[i].name;
        }
    if (bits)
        *bits = 32;
    g_snprintf (buf, len, "0x%04X", (guint) f);
    return buf;
}

static void gl_clear_errors (void)
{
    for (int i = 0; i < 64 && glGetError () != GL_NO_ERROR; i++)
        ;
}

enum { O_TEX, O_BUF, O_FBO, O_RBO, O_PROG, O_SHADER, O_VAO, O_QUERY, O_SAMPLER, O_N };

static gboolean gl_is (int type, GLuint name)
{
    switch (type) {
    case O_TEX:     return glIsTexture (name);
    case O_BUF:     return glIsBuffer (name);
    case O_FBO:     return glIsFramebuffer (name);
    case O_RBO:     return glIsRenderbuffer (name);
    case O_PROG:    return glIsProgram (name);
    case O_SHADER:  return glIsShader (name);
    case O_VAO:     return glIsVertexArray (name);
    case O_QUERY:   return glIsQuery (name);
    case O_SAMPLER: return glIsSampler (name);
    default:        return FALSE;
    }
}

/* the names of one object type that are in use. Drivers hand out the
 * lowest free name: the scan ends after 4096 unused names in a row.
 * *highest = the largest name found. */
static GArray *gl_names (int type, GLuint *highest)
{
    GArray *a = g_array_new (FALSE, FALSE, sizeof (GLuint));
    guint misses = 0;
    *highest = 0;
    for (GLuint name = 1; misses < 4096 && name < (1u << 22); name++) {
        if (!gl_is (type, name)) {
            misses++;
            continue;
        }
        misses = 0;
        *highest = name;
        g_array_append_val (a, name);
    }
    return a;
}

typedef struct {
    char *what;
    guint count;
    gint64 bytes;
} SizeRow;

static gint size_row_cmp (gconstpointer a, gconstpointer b)
{
    const SizeRow *x = a, *y = b;
    if (x->bytes != y->bytes)
        return x->bytes > y->bytes ? -1 : 1;
    return strcmp (x->what, y->what);
}

static void size_rows_add (GHashTable *rows, char *what /* taken */, gint64 bytes)
{
    SizeRow *r = g_hash_table_lookup (rows, what);
    if (!r) {
        r = g_new0 (SizeRow, 1);
        r->what = what;
        g_hash_table_insert (rows, r->what, r);
    } else {
        g_free (what);
    }
    r->count++;
    r->bytes += bytes;
}

/* the rows with the most bytes, and one row for the rest */
static void size_rows_json (JsonBuilder *b, const char *key, GHashTable *rows, guint max)
{
    GArray *a = g_array_new (FALSE, FALSE, sizeof (SizeRow));
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, rows);
    while (g_hash_table_iter_next (&it, &k, &v))
        g_array_append_val (a, *(SizeRow *) v);
    g_array_sort (a, size_row_cmp);
    json_builder_set_member_name (b, key);
    json_builder_begin_array (b);
    guint rest_n = 0;
    gint64 rest_b = 0;
    for (guint i = 0; i < a->len; i++) {
        SizeRow *r = &g_array_index (a, SizeRow, i);
        if (i >= max) {
            rest_n += r->count;
            rest_b += r->bytes;
            continue;
        }
        json_builder_begin_object (b);
        walpe_mem_jstr (b, "what", r->what);
        walpe_mem_jint (b, "count", r->count);
        walpe_mem_jint (b, "bytes", r->bytes);
        json_builder_end_object (b);
    }
    if (rest_n) {
        json_builder_begin_object (b);
        walpe_mem_jstr (b, "what", "(all other sizes)");
        walpe_mem_jint (b, "count", rest_n);
        walpe_mem_jint (b, "bytes", rest_b);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    g_array_free (a, TRUE);
}

static void size_row_free (gpointer p)
{
    SizeRow *r = p;
    g_free (r->what);
    g_free (r);
}

void walpe_mem_gl_json (JsonBuilder *b, gboolean deep)
{
    const gboolean desktop = epoxy_is_desktop_gl ();
    /* direct state access: sizes without binding anything */
    const gboolean dsa = desktop && (epoxy_gl_version () >= 45 ||
                                     epoxy_has_gl_extension ("GL_ARB_direct_state_access"));
    gint64 t0 = g_get_monotonic_time ();
    GArray *names[O_N];
    GLuint highest[O_N];
    gl_clear_errors ();
    for (int t = 0; t < O_N; t++)
        names[t] = gl_names (t, &highest[t]);

    json_builder_set_member_name (b, "gl");
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "sizes_known");
    json_builder_add_boolean_value (b, dsa);
    if (!dsa)
        walpe_mem_jstr (b, "sizes_note", desktop
                            ? "this driver has no GL_ARB_direct_state_access: objects are counted, "
                              "their sizes are not read"
                            : "OpenGL ES: objects are counted, their sizes cannot be read without "
                              "binding them");

    /* textures a framebuffer of this context renders into (framebuffers
     * belong to one context: only this thread's are seen) */
    GHashTable *targets = g_hash_table_new (NULL, NULL);
    guint fbo_attach = 0;
    if (dsa) {
        static const GLenum att[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                                      GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3,
                                      GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
        for (guint i = 0; i < names[O_FBO]->len; i++) {
            GLuint f = g_array_index (names[O_FBO], GLuint, i);
            for (guint k = 0; k < G_N_ELEMENTS (att); k++) {
                GLint type = GL_NONE, obj = 0;
                glGetNamedFramebufferAttachmentParameteriv (
                    f, att[k], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
                if (type != GL_TEXTURE)
                    continue;
                glGetNamedFramebufferAttachmentParameteriv (
                    f, att[k], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &obj);
                if (obj > 0) {
                    g_hash_table_add (targets, GUINT_TO_POINTER ((guint) obj));
                    fbo_attach++;
                }
            }
        }
        gl_clear_errors ();
    }

    /* ---- textures */
    {
        gint64 bytes = 0, comp_bytes = 0, target_bytes = 0;
        guint n_target = 0, n_comp = 0, n_unknown = 0, n_2d = 0, n_other = 0;
        GHashTable *rows = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, size_row_free);
        for (guint i = 0; dsa && i < names[O_TEX]->len; i++) {
            GLuint name = g_array_index (names[O_TEX], GLuint, i);
            GLint target = 0;
            glGetTextureParameteriv (name, GL_TEXTURE_TARGET, &target);
            if (glGetError () != GL_NO_ERROR || !target) {
                gl_clear_errors ();   /* deleted by its thread meanwhile */
                n_unknown++;
                continue;
            }
            if (target == GL_TEXTURE_2D)
                n_2d++;
            else
                n_other++;
            const int faces = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
            gint64 tb = 0;
            GLint w0 = 0, h0 = 0, d0 = 0, f0 = 0;
            int levels = 0;
            gboolean compressed = FALSE;
            for (GLint lv = 0; lv < 16 && target != GL_TEXTURE_BUFFER; lv++) {
                GLint w = 0, h = 0, d = 0, f = 0, c = 0, cs = 0;
                glGetTextureLevelParameteriv (name, lv, GL_TEXTURE_WIDTH, &w);
                glGetTextureLevelParameteriv (name, lv, GL_TEXTURE_HEIGHT, &h);
                if (w < 1 || h < 1)
                    break;
                glGetTextureLevelParameteriv (name, lv, GL_TEXTURE_DEPTH, &d);
                glGetTextureLevelParameteriv (name, lv, GL_TEXTURE_INTERNAL_FORMAT, &f);
                glGetTextureLevelParameteriv (name, lv, GL_TEXTURE_COMPRESSED, &c);
                d = MAX (d, 1);
                if (c) {
                    glGetTextureLevelParameteriv (name, lv, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &cs);
                    tb += (gint64) cs * faces;
                    compressed = TRUE;
                } else {
                    int bits = 32;
                    char fb[16];
                    fmt_name (f, fb, sizeof (fb), &bits);
                    tb += (gint64) w * h * d * faces * bits / 8;
                }
                if (lv == 0) {
                    w0 = w;
                    h0 = h;
                    d0 = d;
                    f0 = f;
                }
                levels++;
            }
            gl_clear_errors ();
            const gboolean is_target = g_hash_table_contains (targets, GUINT_TO_POINTER (name));
            bytes += tb;
            if (compressed) {
                comp_bytes += tb;
                n_comp++;
            }
            if (is_target) {
                target_bytes += tb;
                n_target++;
            }
            char fb[16], dim[32] = "";
            if (d0 > 1)
                g_snprintf (dim, sizeof (dim), "x%d", d0);
            const char *tn = target == GL_TEXTURE_2D ? "" : target == GL_TEXTURE_CUBE_MAP ? " cube"
                             : target == GL_TEXTURE_3D ? " 3D" : target == GL_TEXTURE_2D_ARRAY
                             ? " array" : target == GL_TEXTURE_BUFFER ? " buffer" : " other";
            size_rows_add (rows, levels ? g_strdup_printf ("%dx%d%s %s%s%s%s", w0, h0, dim,
                                                           fmt_name (f0, fb, sizeof (fb), NULL), tn,
                                                           levels > 1 ? " +mips" : "",
                                                           is_target ? " (render target)" : "")
                                        : g_strdup_printf ("no storage%s", tn), tb);
        }
        json_builder_set_member_name (b, "textures");
        json_builder_begin_object (b);
        walpe_mem_jint (b, "count", names[O_TEX]->len);
        walpe_mem_jint (b, "highest_name", highest[O_TEX]);
        if (dsa) {
            walpe_mem_jint (b, "bytes", bytes);
            walpe_mem_jint (b, "compressed", n_comp);
            walpe_mem_jint (b, "compressed_bytes", comp_bytes);
            /* attached to a framebuffer of THIS context */
            walpe_mem_jint (b, "render_targets", n_target);
            walpe_mem_jint (b, "render_target_bytes", target_bytes);
            walpe_mem_jint (b, "two_d", n_2d);
            walpe_mem_jint (b, "other_kinds", n_other);
            walpe_mem_jint (b, "vanished", n_unknown);
            size_rows_json (b, "by_size", rows, 40);
        }
        json_builder_end_object (b);
        g_hash_table_destroy (rows);
    }
    g_hash_table_destroy (targets);

    /* ---- buffers */
    {
        gint64 bytes = 0, largest = 0;
        guint mapped = 0;
        GHashTable *rows = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, size_row_free);
        for (guint i = 0; dsa && i < names[O_BUF]->len; i++) {
            GLuint name = g_array_index (names[O_BUF], GLuint, i);
            GLint size = 0, usage = 0, map = 0;
            glGetNamedBufferParameteriv (name, GL_BUFFER_SIZE, &size);
            glGetNamedBufferParameteriv (name, GL_BUFFER_USAGE, &usage);
            glGetNamedBufferParameteriv (name, GL_BUFFER_MAPPED, &map);
            if (glGetError () != GL_NO_ERROR) {
                gl_clear_errors ();
                continue;
            }
            bytes += size;
            largest = MAX (largest, (gint64) size);
            mapped += map != 0;
            const char *un = usage == GL_STATIC_DRAW ? "static" : usage == GL_DYNAMIC_DRAW
                             ? "dynamic" : usage == GL_STREAM_DRAW ? "stream"
                             : usage == GL_STREAM_READ || usage == GL_STATIC_READ ||
                               usage == GL_DYNAMIC_READ ? "read" : "other";
            /* by power-of-two size class and usage */
            gint64 cls = 1;
            while (cls < size)
                cls <<= 1;
            size_rows_add (rows, g_strdup_printf ("<= %" G_GINT64_FORMAT " bytes, %s%s", cls, un,
                                                  map ? ", mapped" : ""), size);
        }
        json_builder_set_member_name (b, "buffers");
        json_builder_begin_object (b);
        walpe_mem_jint (b, "count", names[O_BUF]->len);
        walpe_mem_jint (b, "highest_name", highest[O_BUF]);
        if (dsa) {
            walpe_mem_jint (b, "bytes", bytes);
            walpe_mem_jint (b, "largest_bytes", largest);
            walpe_mem_jint (b, "mapped", mapped);
            size_rows_json (b, "by_size", rows, 16);
        }
        json_builder_end_object (b);
        g_hash_table_destroy (rows);
    }

    /* ---- renderbuffers */
    {
        gint64 bytes = 0;
        for (guint i = 0; dsa && i < names[O_RBO]->len; i++) {
            GLuint name = g_array_index (names[O_RBO], GLuint, i);
            GLint w = 0, h = 0, f = 0, s = 0;
            glGetNamedRenderbufferParameteriv (name, GL_RENDERBUFFER_WIDTH, &w);
            glGetNamedRenderbufferParameteriv (name, GL_RENDERBUFFER_HEIGHT, &h);
            glGetNamedRenderbufferParameteriv (name, GL_RENDERBUFFER_INTERNAL_FORMAT, &f);
            glGetNamedRenderbufferParameteriv (name, GL_RENDERBUFFER_SAMPLES, &s);
            if (glGetError () != GL_NO_ERROR) {
                gl_clear_errors ();
                continue;
            }
            int bits = 32;
            char fb[16];
            fmt_name (f, fb, sizeof (fb), &bits);
            bytes += (gint64) w * h * MAX (s, 1) * bits / 8;
        }
        json_builder_set_member_name (b, "renderbuffers");
        json_builder_begin_object (b);
        walpe_mem_jint (b, "count", names[O_RBO]->len);
        if (dsa)
            walpe_mem_jint (b, "bytes", bytes);
        json_builder_end_object (b);
    }

    /* ---- programs and shaders */
    {
        gint64 binary = 0, attached = 0, source = 0;
        guint unlinked = 0;
        for (guint i = 0; i < names[O_PROG]->len; i++) {
            GLuint name = g_array_index (names[O_PROG], GLuint, i);
            GLint n = 0, ok = 0, len = 0;
            glGetProgramiv (name, GL_ATTACHED_SHADERS, &n);
            glGetProgramiv (name, GL_LINK_STATUS, &ok);
            attached += n;
            unlinked += !ok;
            if (deep) {
                glGetProgramiv (name, GL_PROGRAM_BINARY_LENGTH, &len);
                binary += MAX (len, 0);
            }
        }
        gl_clear_errors ();
        for (guint i = 0; i < names[O_SHADER]->len; i++) {
            GLint len = 0;
            glGetShaderiv (g_array_index (names[O_SHADER], GLuint, i), GL_SHADER_SOURCE_LENGTH,
                           &len);
            source += MAX (len, 0);
        }
        gl_clear_errors ();
        json_builder_set_member_name (b, "programs");
        json_builder_begin_object (b);
        walpe_mem_jint (b, "count", names[O_PROG]->len);
        walpe_mem_jint (b, "highest_name", MAX (highest[O_PROG], highest[O_SHADER]));
        walpe_mem_jint (b, "not_linked", unlinked);
        walpe_mem_jint (b, "shaders_still_attached", attached);
        if (deep)
            walpe_mem_jint (b, "binary_bytes", binary);
        json_builder_end_object (b);
        json_builder_set_member_name (b, "shaders");
        json_builder_begin_object (b);
        walpe_mem_jint (b, "count", names[O_SHADER]->len);
        walpe_mem_jint (b, "source_bytes", source);
        json_builder_end_object (b);
    }

    /* ---- what belongs to this context alone, and samplers */
    json_builder_set_member_name (b, "framebuffers");
    json_builder_begin_object (b);
    walpe_mem_jint (b, "count", names[O_FBO]->len);
    walpe_mem_jint (b, "highest_name", highest[O_FBO]);
    if (dsa)
        walpe_mem_jint (b, "texture_attachments", fbo_attach);
    json_builder_end_object (b);
    json_builder_set_member_name (b, "vertex_arrays");
    json_builder_begin_object (b);
    walpe_mem_jint (b, "count", names[O_VAO]->len);
    walpe_mem_jint (b, "highest_name", highest[O_VAO]);
    json_builder_end_object (b);
    json_builder_set_member_name (b, "queries");
    json_builder_begin_object (b);
    walpe_mem_jint (b, "count", names[O_QUERY]->len);
    walpe_mem_jint (b, "highest_name", highest[O_QUERY]);
    json_builder_end_object (b);
    json_builder_set_member_name (b, "samplers");
    json_builder_begin_object (b);
    walpe_mem_jint (b, "count", names[O_SAMPLER]->len);
    json_builder_end_object (b);

    /* the GPU's own memory where the driver tells (NVIDIA): what is free
     * of the dedicated memory, for every process together (a software
     * rasterizer answers with the machine's RAM: left out) */
    if (!walpe_gl_is_software () && epoxy_has_gl_extension ("GL_NVX_gpu_memory_info")) {
        GLint total = 0, avail = 0, evict = 0, evicted = 0;
        glGetIntegerv (0x9048, &total);     /* GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX */
        glGetIntegerv (0x9049, &avail);     /* ..._CURRENT_AVAILABLE_VIDMEM_NVX */
        glGetIntegerv (0x904A, &evict);     /* ..._EVICTION_COUNT_NVX */
        glGetIntegerv (0x904B, &evicted);   /* ..._EVICTED_MEMORY_NVX */
        gl_clear_errors ();
        json_builder_set_member_name (b, "video_memory");
        json_builder_begin_object (b);
        walpe_mem_jint (b, "total_kb", total);
        walpe_mem_jint (b, "free_kb", avail);
        walpe_mem_jint (b, "evictions", evict);
        walpe_mem_jint (b, "evicted_kb", evicted);
        json_builder_end_object (b);
    }
    for (int t = 0; t < O_N; t++)
        g_array_free (names[t], TRUE);
    gl_clear_errors ();
    walpe_mem_jint (b, "census_us", g_get_monotonic_time () - t0);
    json_builder_end_object (b);
}

/* ------------------------------------------------------- GL storage events
 * See memreport.h. The mechanism is the GPU profiler's (gpuprof.c,
 * "interposer"): every `glFoo (...)` of this process is a call through the
 * libepoxy pointer epoxy_glFoo; while counting is on, the pointers of the
 * entry points below hold a wrapper that adds to a counter and calls the
 * real entry point, taken from eglGetProcAddress. A wrapper is complete in
 * itself the moment its address can be seen (it looks its entry point up
 * when its pointer is still empty), and it only forwards while counting is
 * off - which is all that is left of it should another holder of the slot
 * (a GPU profile that started meanwhile) put it back later. */

static const gint64 size_class_kb[] = { 64, 256, 1024, 1536, 2560, 4608, 6656, 9216, 16384 };
static const char *const size_class_names[] = {
    "to 64 kB", "to 256 kB", "to 1 MB", "to 1.5 MB", "to 2.5 MB", "to 4.5 MB", "to 6.5 MB",
    "to 9 MB", "to 16 MB", "over 16 MB",
};
#define GLEV_CLASSES ((int) G_N_ELEMENTS (size_class_names))

const char *walpe_mem_size_class (gint64 bytes, int *cls)
{
    int c = 0;
    while (c < (int) G_N_ELEMENTS (size_class_kb) && bytes > size_class_kb[c] * 1024)
        c++;
    if (cls)
        *cls = c;
    return size_class_names[c];
}

enum {
    GLEV_TEX_EMPTY = 0,   /* storage without pixels: render targets, textures filled later */
    GLEV_TEX_PIXELS,      /* storage with pixels */
    GLEV_TEX_COMPRESSED,
    GLEV_BUF_STORAGE,     /* glBufferStorage: buffers that stay mapped (video staging) */
    GLEV_READBACK,        /* glReadPixels */
    GLEV_SIZED_N
};
static const char *const glev_sized_names[GLEV_SIZED_N] = {
    "texture_stores_without_pixels", "texture_stores_with_pixels", "texture_stores_compressed",
    "buffer_storages", "read_backs",
};
enum {
    GLEV_TEX_NAMES = 0, GLEV_TEX_DELETED, GLEV_FBO_MADE, GLEV_FBO_DELETED, GLEV_BUF_MADE,
    GLEV_BUF_DELETED, GLEV_PROG_LINKED, GLEV_PROG_DELETED, GLEV_TEX_READ, GLEV_PLAIN_N
};
static const char *const glev_plain_names[GLEV_PLAIN_N] = {
    "texture_names_made", "textures_deleted", "framebuffers_made", "framebuffers_deleted",
    "buffers_made", "buffers_deleted", "programs_linked", "programs_deleted", "texture_read_backs",
};

static struct {
    gint on;                                    /* atomic: the wrappers count */
    gint64 since_us;
    gint64 n[GLEV_SIZED_N][GLEV_CLASSES];       /* atomic: events by size class */
    gint64 kb[GLEV_SIZED_N];                    /* atomic: their sizes together */
    gint64 plain[GLEV_PLAIN_N];                 /* atomic */
} glev;

static inline void glev_sized (int kind, gint64 bytes)
{
    if (!g_atomic_int_get (&glev.on))
        return;
    int c = 0;
    walpe_mem_size_class (bytes, &c);
    __atomic_fetch_add (&glev.n[kind][c], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add (&glev.kb[kind], (bytes + 1023) / 1024, __ATOMIC_RELAXED);
}

static inline void glev_plain (int kind, gint64 n)
{
    if (g_atomic_int_get (&glev.on) && n > 0)
        __atomic_fetch_add (&glev.plain[kind], n, __ATOMIC_RELAXED);
}

static void (*glev_resolve (const char *name)) (void)
{
    void (*f) (void) = (void (*) (void)) eglGetProcAddress (name);
    if (G_UNLIKELY (!f))   /* (walpe_mem_gl_events found an address before it set the wrapper) */
        g_error ("mem: no address for %s", name);
    return f;
}

#define GLEV_REAL(fn) (__extension__ ({ \
    __typeof__ (r_##fn) real_ = __atomic_load_n (&r_##fn, __ATOMIC_RELAXED); \
    if (G_UNLIKELY (!real_)) { \
        real_ = (__typeof__ (r_##fn)) glev_resolve (#fn); \
        __atomic_store_n (&r_##fn, real_, __ATOMIC_RELAXED); \
    } \
    real_; }))

static void (*r_glTexImage2D) (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*r_glTexStorage2D) (GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (*r_glCompressedTexImage2D) (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
static void (*r_glGenTextures) (GLsizei, GLuint *);
static void (*r_glDeleteTextures) (GLsizei, const GLuint *);
static void (*r_glGenFramebuffers) (GLsizei, GLuint *);
static void (*r_glDeleteFramebuffers) (GLsizei, const GLuint *);
static void (*r_glGenBuffers) (GLsizei, GLuint *);
static void (*r_glDeleteBuffers) (GLsizei, const GLuint *);
static void (*r_glBufferStorage) (GLenum, GLsizeiptr, const void *, GLbitfield);
static void (*r_glReadPixels) (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*r_glGetTexImage) (GLenum, GLint, GLenum, GLenum, void *);
static void (*r_glLinkProgram) (GLuint);
static void (*r_glDeleteProgram) (GLuint);

static gint64 glev_tex_bytes (GLint internal, GLsizei w, GLsizei h)
{
    int bits = 32;
    char buf[16];
    fmt_name (internal, buf, sizeof (buf), &bits);
    return (gint64) MAX (w, 0) * (gint64) MAX (h, 0) * bits / 8;
}

static void w_glTexImage2D (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                            GLenum format, GLenum type, const void *px)
{
    if (level == 0)   /* (the further levels belong to the same texture) */
        glev_sized (px ? GLEV_TEX_PIXELS : GLEV_TEX_EMPTY, glev_tex_bytes (internal, w, h));
    GLEV_REAL (glTexImage2D) (target, level, internal, w, h, border, format, type, px);
}

static void w_glTexStorage2D (GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h)
{
    glev_sized (GLEV_TEX_EMPTY, glev_tex_bytes ((GLint) internal, w, h));
    GLEV_REAL (glTexStorage2D) (target, levels, internal, w, h);
}

static void w_glCompressedTexImage2D (GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h,
                                      GLint border, GLsizei size, const void *data)
{
    if (level == 0)
        glev_sized (GLEV_TEX_COMPRESSED, size);
    GLEV_REAL (glCompressedTexImage2D) (target, level, internal, w, h, border, size, data);
}

static void w_glGenTextures (GLsizei n, GLuint *names)
{
    glev_plain (GLEV_TEX_NAMES, n);
    GLEV_REAL (glGenTextures) (n, names);
}

static void w_glDeleteTextures (GLsizei n, const GLuint *names)
{
    glev_plain (GLEV_TEX_DELETED, n);
    GLEV_REAL (glDeleteTextures) (n, names);
}

static void w_glGenFramebuffers (GLsizei n, GLuint *names)
{
    glev_plain (GLEV_FBO_MADE, n);
    GLEV_REAL (glGenFramebuffers) (n, names);
}

static void w_glDeleteFramebuffers (GLsizei n, const GLuint *names)
{
    glev_plain (GLEV_FBO_DELETED, n);
    GLEV_REAL (glDeleteFramebuffers) (n, names);
}

static void w_glGenBuffers (GLsizei n, GLuint *names)
{
    glev_plain (GLEV_BUF_MADE, n);
    GLEV_REAL (glGenBuffers) (n, names);
}

static void w_glDeleteBuffers (GLsizei n, const GLuint *names)
{
    glev_plain (GLEV_BUF_DELETED, n);
    GLEV_REAL (glDeleteBuffers) (n, names);
}

static void w_glBufferStorage (GLenum target, GLsizeiptr size, const void *data, GLbitfield flags)
{
    glev_sized (GLEV_BUF_STORAGE, (gint64) size);
    GLEV_REAL (glBufferStorage) (target, size, data, flags);
}

static void w_glReadPixels (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *px)
{
    const int comps = format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB ? 3 :
                      format == GL_RG ? 2 : 1;
    const int each = type == GL_FLOAT || type == GL_UNSIGNED_INT || type == GL_INT ? 4 :
                     type == GL_HALF_FLOAT || type == GL_UNSIGNED_SHORT || type == GL_SHORT ? 2 : 1;
    glev_sized (GLEV_READBACK, (gint64) MAX (w, 0) * (gint64) MAX (h, 0) * comps * each);
    GLEV_REAL (glReadPixels) (x, y, w, h, format, type, px);
}

static void w_glGetTexImage (GLenum target, GLint level, GLenum format, GLenum type, void *px)
{
    glev_plain (GLEV_TEX_READ, 1);
    GLEV_REAL (glGetTexImage) (target, level, format, type, px);
}

static void w_glLinkProgram (GLuint program)
{
    glev_plain (GLEV_PROG_LINKED, 1);
    GLEV_REAL (glLinkProgram) (program);
}

static void w_glDeleteProgram (GLuint program)
{
    glev_plain (GLEV_PROG_DELETED, program ? 1 : 0);
    GLEV_REAL (glDeleteProgram) (program);
}

typedef void (*GlevFn) (void);
typedef struct {
    const char *name;
    void *slot;        /* &epoxy_glFoo */
    GlevFn wrap;
    GlevFn saved;      /* what the slot held when the wrapper went in */
    gboolean in;       /* the wrapper was put into the slot (and not taken out since) */
} GlevHook;
#define GLEV_HOOK(fn) { #fn, &epoxy_##fn, (GlevFn) w_##fn, NULL, FALSE }
static GlevHook glev_hooks[] = {
    GLEV_HOOK (glTexImage2D), GLEV_HOOK (glTexStorage2D), GLEV_HOOK (glCompressedTexImage2D),
    GLEV_HOOK (glGenTextures), GLEV_HOOK (glDeleteTextures), GLEV_HOOK (glGenFramebuffers),
    GLEV_HOOK (glDeleteFramebuffers), GLEV_HOOK (glGenBuffers), GLEV_HOOK (glDeleteBuffers),
    GLEV_HOOK (glBufferStorage), GLEV_HOOK (glReadPixels), GLEV_HOOK (glGetTexImage),
    GLEV_HOOK (glLinkProgram), GLEV_HOOK (glDeleteProgram),
};
static GMutex glev_lock;   /* the hook table (whoever asks; the wrappers never take it) */

/* (the slots are function pointers of different types inside libepoxy:
 * read and written as bytes, one aligned pointer store each) */
static inline GlevFn glev_slot_get (void *slot)
{
    GlevFn f;
    memcpy (&f, slot, sizeof (f));
    return f;
}

static inline void glev_slot_set (void *slot, GlevFn f)
{
    memcpy (slot, &f, sizeof (f));
}

int walpe_mem_gl_events (gboolean on)
{
    int n = 0;
    g_mutex_lock (&glev_lock);
    for (guint i = 0; i < G_N_ELEMENTS (glev_hooks); i++) {
        GlevHook *h = &glev_hooks[i];
        GlevFn cur = glev_slot_get (h->slot);
        if (on) {
            /* an entry point this driver does not have stays as it is */
            if (!h->in && !eglGetProcAddress (h->name))
                continue;
            if (cur != h->wrap) {
                /* first time, or libepoxy stored the entry point it had
                 * just looked up over the wrapper (it does that once, at
                 * the first call of an entry point), or a GPU profile's
                 * wrapper stands in front: what was saved the first time
                 * stays the value to put back */
                if (!h->in)
                    h->saved = cur;
                glev_slot_set (h->slot, h->wrap);
            }
            h->in = TRUE;
            n++;
        } else if (h->in) {
            /* (not ours any more: a profile's wrapper is in front and puts
             * ours back when it ends; it then only forwards) */
            if (cur == h->wrap)
                glev_slot_set (h->slot, h->saved);
            h->in = FALSE;
        }
    }
    if (on && !g_atomic_int_get (&glev.on)) {
        glev.since_us = g_get_monotonic_time ();
        g_atomic_int_set (&glev.on, 1);
    } else if (!on && g_atomic_int_get (&glev.on)) {
        g_atomic_int_set (&glev.on, 0);
        for (int k = 0; k < GLEV_SIZED_N; k++) {
            __atomic_store_n (&glev.kb[k], 0, __ATOMIC_RELAXED);
            for (int c = 0; c < GLEV_CLASSES; c++)
                __atomic_store_n (&glev.n[k][c], 0, __ATOMIC_RELAXED);
        }
        for (int k = 0; k < GLEV_PLAIN_N; k++)
            __atomic_store_n (&glev.plain[k], 0, __ATOMIC_RELAXED);
    }
    g_mutex_unlock (&glev_lock);
    return n;
}

void walpe_mem_gl_events_json (JsonBuilder *b)
{
    json_builder_set_member_name (b, "gl_events");
    json_builder_begin_object (b);
    const gboolean on = g_atomic_int_get (&glev.on);
    json_builder_set_member_name (b, "counting");
    json_builder_add_boolean_value (b, on);
    if (!on) {
        walpe_mem_jstr (b, "note", "not counted: `walpe-ctl mem --events on` starts it");
        json_builder_end_object (b);
        return;
    }
    /* a slot that lost its wrapper (see walpe_mem_gl_events) gets it again */
    walpe_mem_jint (b, "entry_points", walpe_mem_gl_events (TRUE));
    json_builder_set_member_name (b, "seconds");
    json_builder_add_double_value (b, (double) (g_get_monotonic_time () - glev.since_us) / 1e6);
    for (int k = 0; k < GLEV_PLAIN_N; k++)
        walpe_mem_jint (b, glev_plain_names[k], __atomic_load_n (&glev.plain[k], __ATOMIC_RELAXED));
    for (int k = 0; k < GLEV_SIZED_N; k++) {
        gint64 total = 0;
        json_builder_set_member_name (b, glev_sized_names[k]);
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "by_size");
        json_builder_begin_array (b);
        for (int c = 0; c < GLEV_CLASSES; c++) {
            const gint64 n = __atomic_load_n (&glev.n[k][c], __ATOMIC_RELAXED);
            total += n;
            if (!n)
                continue;
            json_builder_begin_object (b);
            walpe_mem_jstr (b, "what", size_class_names[c]);
            walpe_mem_jint (b, "count", n);
            json_builder_end_object (b);
        }
        json_builder_end_array (b);
        walpe_mem_jint (b, "count", total);
        walpe_mem_jint (b, "total_kb", __atomic_load_n (&glev.kb[k], __ATOMIC_RELAXED));
        json_builder_end_object (b);
    }
    json_builder_end_object (b);
}
