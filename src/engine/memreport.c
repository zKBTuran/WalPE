/* memreport.c - `walpe-ctl mem` (memreport.h): the process as the kernel and
 * the allocator see it, and a census of the GL objects of a context.
 * Everything is computed when asked for. */
#include "walpe-build-config.h"
#include "memreport.h"
#include "glutil.h"   /* walpe_gl_is_software */
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
    gint64 size, rss, dirty, swap;
    char perms[5], name[512];
} SmapsCur;

static void smaps_flush (WalpeMemMaps *out, SmapsCur *c)
{
    if (c->kind < 0)
        return;
    WalpeMemKind *k = &out->kind[c->kind];
    k->n++;
    k->size_kb += c->size;
    k->rss_kb += c->rss;
    k->dirty_kb += c->dirty;
    k->swap_kb += c->swap;
    out->n_maps++;
    if (c->kind == WALPE_MAP_ANON || c->kind == WALPE_MAP_HEAP)
        top_anon_add (out, c->size, c->rss, c->perms);
    else if (c->kind == WALPE_MAP_FILE || c->kind == WALPE_MAP_LIB || c->kind == WALPE_MAP_MEMFD ||
             c->kind == WALPE_MAP_SHM)
        top_file_add (out, c->size, c->rss, c->name);
    c->kind = -1;
}

void walpe_mem_smaps_parse (const char *text, const char *exe, WalpeMemMaps *out)
{
    memset (out, 0, sizeof (*out));
    SmapsCur c = { .kind = -1 };
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
            c.size = c.rss = c.dirty = c.swap = 0;
        } else if (c.kind >= 0 && len > 0) {
            static const struct { const char *key; int which; } keys[] = {
                { "Size:", 0 }, { "Rss:", 1 }, { "Private_Dirty:", 2 }, { "Swap:", 3 },
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
                else
                    c.swap = v;
                break;
            }
        }
        if (!e)
            break;
        p = e + 1;
    }
    smaps_flush (out, &c);
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
        char *exe = g_file_read_link ("/proc/self/exe", NULL);
        walpe_mem_smaps_parse (sm, exe, m);
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
        g_free (m);
    } else {
        walpe_mem_jstr (b, "note", "/proc/self/smaps cannot be read");
    }
    json_builder_end_object (b);
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
