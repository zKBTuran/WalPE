/* memreport_test.c - `walpe-ctl mem` (engine/memreport.h):
 *  1. the kind of a mapping by the name the kernel prints for it, and a
 *     /proc/<pid>/smaps text summed by kind (sizes, resident, private),
 *     with the largest anonymous mappings and files;
 *  2. the process part of the report as JSON: resident memory, the
 *     descriptors (one more after a file is opened), the allocator, the
 *     mappings of this very process;
 *  3. the counters: an increment shows in the JSON under its name;
 *  4. the GL census: textures, buffers, framebuffers, programs and vertex
 *     arrays made here are counted with their sizes and gone again after
 *     they are deleted; the census leaves the bindings and the error state
 *     alone.
 * Part 4 needs a GL 3.3 context (EGL device or Mesa surfaceless platform)
 * and skips itself without one. */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "engine/memreport.h"
#include "common/util.h"

static int fails;

static void check (gboolean ok, const char *what)
{
    printf ("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

static JsonObject *built (JsonBuilder *b, JsonNode **root)
{
    json_builder_end_object (b);
    *root = json_builder_get_root (b);
    g_object_unref (b);
    return json_node_get_object (*root);
}

static JsonBuilder *begin (void)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    return b;
}

static gint64 num (JsonObject *o, const char *path)
{
    char **parts = g_strsplit (path, ".", -1);
    int n = (int) g_strv_length (parts);
    for (int i = 0; o && i < n - 1; i++)
        o = wpj_obj (o, parts[i]);
    gint64 v = o && n ? (gint64) wpj_num (o, parts[n - 1], -1) : -1;
    g_strfreev (parts);
    return v;
}

static void test_maps (void)
{
    check (walpe_mem_map_kind ("") == WALPE_MAP_ANON && walpe_mem_map_kind (NULL) == WALPE_MAP_ANON,
           "a mapping without a name is anonymous memory");
    check (walpe_mem_map_kind ("[heap]") == WALPE_MAP_HEAP, "[heap]");
    check (walpe_mem_map_kind ("[stack]") == WALPE_MAP_STACK, "[stack]");
    check (walpe_mem_map_kind ("[anon: glibc: malloc]") == WALPE_MAP_ANON, "a named anonymous mapping");
    check (walpe_mem_map_kind ("[vdso]") == WALPE_MAP_OTHER, "[vdso]");
    check (walpe_mem_map_kind ("/dev/nvidiactl") == WALPE_MAP_NVIDIA &&
           walpe_mem_map_kind ("/dev/nvidia0") == WALPE_MAP_NVIDIA, "/dev/nvidia*");
    check (walpe_mem_map_kind ("/dev/dri/renderD128") == WALPE_MAP_DRI &&
           walpe_mem_map_kind ("anon_inode:dmabuf") == WALPE_MAP_DRI, "/dev/dri/*, dma-bufs");
    check (walpe_mem_map_kind ("/memfd:pulseaudio (deleted)") == WALPE_MAP_MEMFD, "/memfd:*");
    check (walpe_mem_map_kind ("/dev/shm/x") == WALPE_MAP_SHM &&
           walpe_mem_map_kind ("/SYSV00000000 (deleted)") == WALPE_MAP_SHM &&
           walpe_mem_map_kind ("/dev/zero (deleted)") == WALPE_MAP_SHM,
           "shared memory segments, shared anonymous mappings");
    check (walpe_mem_map_kind ("/drm mm object (deleted)") == WALPE_MAP_DRI &&
           walpe_mem_map_kind ("/i915 (deleted)") == WALPE_MAP_DRI &&
           walpe_mem_map_kind ("/dev/null") == WALPE_MAP_FILE,
           "a kernel driver's buffer objects under their other names");
    check (walpe_mem_map_kind ("/usr/lib/libGLX_nvidia.so.615.71.09") == WALPE_MAP_LIB &&
           walpe_mem_map_kind ("/usr/lib/libepoxy.so") == WALPE_MAP_LIB &&
           walpe_mem_map_kind ("/usr/lib/walpe/walpe-video.so (deleted)") == WALPE_MAP_LIB,
           "shared libraries");
    check (walpe_mem_map_kind ("/usr/share/fonts/a.sound.ttf") == WALPE_MAP_FILE &&
           walpe_mem_map_kind ("/home/u/.cache/walpe/x.sock") == WALPE_MAP_FILE,
           "a file whose name only contains \".so\" is a file");

    static const char smaps[] =
        "55d0a0000000-55d0a0100000 r-xp 00000000 08:01 11 /usr/bin/walpe-daemon\n"
        "Size:               1024 kB\nRss:                 800 kB\nPrivate_Dirty:         0 kB\n"
        "Swap:                  0 kB\nVmFlags: rd ex mr mw me\n"
        "55d0a1000000-55d0a3000000 rw-p 00000000 00:00 0 [heap]\n"
        "Size:              32768 kB\nRss:               30000 kB\nPrivate_Dirty:     30000 kB\n"
        "Swap:                100 kB\n"
        "7f0000000000-7f0004000000 rw-p 00000000 00:00 0 \n"
        "Size:              65536 kB\nRss:               40000 kB\nPrivate_Dirty:     39000 kB\n"
        "Swap:                  0 kB\n"
        "7f0010000000-7f0010800000 rw-p 00000000 00:00 0\n"
        "Size:               8192 kB\nRss:                8192 kB\nPrivate_Dirty:      8192 kB\n"
        "7f0020000000-7f0020400000 rw-s 00000000 00:05 99 /dev/nvidiactl\n"
        "Size:               4096 kB\nRss:                4096 kB\nPrivate_Dirty:         0 kB\n"
        "7f0030000000-7f0030200000 r--p 00000000 08:01 12 /usr/lib/libfoo.so.1\n"
        "Size:               2048 kB\nRss:                 500 kB\nPrivate_Dirty:         0 kB\n"
        "7f0030200000-7f0030300000 rw-p 00200000 08:01 12 /usr/lib/libfoo.so.1\n"
        "Size:               1024 kB\nRss:                 100 kB\nPrivate_Dirty:       100 kB\n"
        "7f0040000000-7f0040100000 r--s 00000000 08:01 13 /home/u/my wallpapers/scene one.pkg\n"
        "Size:               1024 kB\nRss:                 900 kB\nPrivate_Dirty:         0 kB\n"
        "7ffc00000000-7ffc00021000 rw-p 00000000 00:00 0 [stack]\n"
        "Size:                132 kB\nRss:                  60 kB\nPrivate_Dirty:        60 kB\n"
        "7ffc00100000-7ffc00102000 r-xp 00000000 00:00 0 [vdso]\n"
        "Size:                  8 kB\nRss:                   4 kB\n";
    WalpeMemMaps m;
    walpe_mem_smaps_parse (smaps, "/usr/bin/walpe-daemon", &m);
    check (m.n_maps == 10, "every mapping of the text is counted");
    check (m.kind[WALPE_MAP_HEAP].n == 1 && m.kind[WALPE_MAP_HEAP].rss_kb == 30000 &&
           m.kind[WALPE_MAP_HEAP].size_kb == 32768 && m.kind[WALPE_MAP_HEAP].swap_kb == 100,
           "the heap: size, resident, swapped");
    check (m.kind[WALPE_MAP_ANON].n == 2 && m.kind[WALPE_MAP_ANON].rss_kb == 48192 &&
           m.kind[WALPE_MAP_ANON].dirty_kb == 47192,
           "anonymous mappings are summed (a header line with and without a trailing blank)");
    check (m.kind[WALPE_MAP_NVIDIA].n == 1 && m.kind[WALPE_MAP_NVIDIA].rss_kb == 4096,
           "memory shared with the NVIDIA driver is its own kind");
    check (m.kind[WALPE_MAP_LIB].n == 3 && m.kind[WALPE_MAP_LIB].rss_kb == 1400,
           "the program and its libraries");
    check (m.kind[WALPE_MAP_FILE].n == 1 && m.kind[WALPE_MAP_FILE].rss_kb == 900,
           "a file with blanks in its name");
    check (m.kind[WALPE_MAP_STACK].n == 1 && m.kind[WALPE_MAP_OTHER].n == 1, "stack and vdso");
    check (m.top_anon[0].rss_kb == 40000 && m.top_anon[1].rss_kb == 30000 &&
           m.top_anon[2].rss_kb == 8192 && m.top_anon[3].rss_kb == 0 &&
           !strcmp (m.top_anon[0].perms, "rw-p"),
           "the largest anonymous mappings, largest first");
    check (!strcmp (m.top_file[0].name, "scene one.pkg") && m.top_file[0].rss_kb == 900 &&
           !strcmp (m.top_file[1].name, "walpe-daemon") &&
           !strcmp (m.top_file[2].name, "libfoo.so.1") && m.top_file[2].rss_kb == 600,
           "the largest files by base name, the parts of one file as one");
    walpe_mem_smaps_parse ("", NULL, &m);
    check (m.n_maps == 0, "an empty text has no mappings");
    walpe_mem_smaps_parse ("Rss: 5 kB\ngarbage\n\n7f00-7f10 rw-p 0 0:0 0", NULL, &m);
    check (m.n_maps == 1 && m.kind[WALPE_MAP_ANON].n == 1,
           "lines before the first mapping are skipped; a last line without a newline counts");
}

static void test_process (void)
{
    JsonNode *root = NULL;
    JsonBuilder *b = begin ();
    walpe_mem_process_json (b);
    JsonObject *o = built (b, &root);
    check (num (o, "process.pid") == (gint64) getpid (), "the report names this process");
    check (num (o, "process.rss_kb") > 500 && num (o, "process.rss_anon_kb") > 0 &&
           num (o, "process.rss_file_kb") > 0 && num (o, "process.threads") >= 1 &&
           num (o, "process.data_kb") > 0,
           "resident memory, its anonymous and file parts, data size and threads are read");
    check (num (o, "process.rss_kb") >= num (o, "process.rss_anon_kb"),
           "\"RssAnon:\" is not read where \"VmRSS:\" was asked for");
    const gint64 fds = num (o, "fds.count");
    check (fds >= 3 && fds < 4096, "open descriptors are counted");
    check (num (o, "mappings.count") > 10 && num (o, "mappings.libraries.count") > 0 &&
           num (o, "mappings.stack.count") == 1 && num (o, "mappings.heap.rss_kb") > 0,
           "the mappings of this process: libraries, one stack, a heap");
#ifdef __GLIBC__
    check (num (o, "malloc.arenas") >= 1 && num (o, "malloc.in_use_kb") > 0 &&
           num (o, "malloc.arena_kb") >= num (o, "malloc.in_use_kb"),
           "the allocator: arenas, bytes handed out, bytes its heaps hold");
#endif
    json_node_unref (root);

    /* 64 MiB that are allocated and touched show as resident anonymous
     * memory and in the allocator's own count; one more descriptor shows */
    const gsize big = 64u << 20;
    char *blk = g_malloc (big);
    memset (blk, 0x5a, big);
    int fd = open ("/dev/null", O_RDONLY | O_CLOEXEC);
    b = begin ();
    walpe_mem_process_json (b);
    JsonObject *o2 = built (b, &root);
    check (num (o2, "fds.count") == fds + 1, "an opened file is one more descriptor");
    check (num (o2, "process.rss_anon_kb") >= 60 * 1024,
           "64 MiB of touched heap are resident anonymous memory");
#ifdef __GLIBC__
    check (num (o2, "malloc.in_use_kb") + num (o2, "malloc.mmapped_kb") >= 64 * 1024,
           "... and are in the allocator's count of what it handed out");
#endif
    check (num (o2, "mappings.anonymous.rss_kb") + num (o2, "mappings.heap.rss_kb") >= 60 * 1024,
           "... and in the anonymous mappings");
    json_node_unref (root);
    if (fd >= 0)
        close (fd);
    g_free (blk);
}

static void test_counters (void)
{
    JsonNode *root = NULL;
    JsonBuilder *b = begin ();
    walpe_mem_counters_json (b);
    JsonObject *o = built (b, &root);
    const gint64 p0 = num (o, "pauses"), l0 = num (o, "loads");
    json_node_unref (root);
    walpe_memc_inc (WALPE_MEMC_PAUSE);
    walpe_memc_inc (WALPE_MEMC_PAUSE);
    walpe_memc_inc (WALPE_MEMC_LOAD);
    b = begin ();
    walpe_mem_counters_json (b);
    o = built (b, &root);
    check (p0 >= 0 && num (o, "pauses") == p0 + 2 && num (o, "loads") == l0 + 1 &&
           num (o, "resumes") == 0, "an increment shows under the counter's name, and only there");
    gboolean named = TRUE;
    for (int i = 0; i < WALPE_MEMC_N; i++)
        named = named && walpe_memc_name ((WalpeMemCounter) i) &&
                json_object_has_member (o, walpe_memc_name ((WalpeMemCounter) i));
    check (named, "every counter has a name and a member");
    json_node_unref (root);
}

static JsonObject *census (JsonNode **root, gboolean deep)
{
    JsonBuilder *b = begin ();
    walpe_mem_gl_json (b, deep);
    return wpj_obj (built (b, root), "gl");
}

static const char *row_of (JsonObject *gl, const char *kind, const char *needle, gint64 *count,
                           gint64 *bytes)
{
    JsonArray *a = wpj_arr (wpj_obj (gl, kind), "by_size");
    for (guint i = 0; i < wpj_arr_len (a); i++) {
        JsonObject *r = wpj_arr_obj (a, i);
        if (strstr (wpj_str (r, "what", ""), needle)) {
            *count = (gint64) wpj_num (r, "count", -1);
            *bytes = (gint64) wpj_num (r, "bytes", -1);
            return wpj_str (r, "what", "");
        }
    }
    *count = *bytes = -1;
    return NULL;
}

static void test_gl (void)
{
    JsonNode *r0 = NULL, *r1 = NULL, *r2 = NULL;
    JsonObject *g0 = census (&r0, FALSE);
    if (!wpj_bool (g0, "sizes_known", FALSE)) {
        /* counts still have to work */
        GLuint t = 0;
        glGenTextures (1, &t);
        glBindTexture (GL_TEXTURE_2D, t);
        JsonObject *g = census (&r1, FALSE);
        check (num (g, "textures.count") == num (g0, "textures.count") + 1,
               "without direct state access a new texture is still counted");
        glDeleteTextures (1, &t);
        printf ("note: this GL has no GL_ARB_direct_state_access: sizes not tested\n");
        json_node_unref (r0);
        json_node_unref (r1);
        return;
    }

    GLuint tex[5], fbo = 0, buf[2], vao = 0, rbo = 0;
    glGenTextures (5, tex);
    /* three 333 x 77 RGBA8 pictures, one of them a render target */
    for (int i = 0; i < 3; i++) {
        glBindTexture (GL_TEXTURE_2D, tex[i]);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 333, 77, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    /* 64 x 64 R8 with a full mip chain: 64^2 + 32^2 + ... + 1 = 5461 bytes */
    glBindTexture (GL_TEXTURE_2D, tex[3]);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_R8, 64, 64, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glGenerateMipmap (GL_TEXTURE_2D);
    /* 16 x 16 x 4 RGBA16F array */
    glBindTexture (GL_TEXTURE_2D_ARRAY, tex[4]);
    glTexImage3D (GL_TEXTURE_2D_ARRAY, 0, GL_RGBA16F, 16, 16, 4, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
    glGenRenderbuffers (1, &rbo);
    glBindRenderbuffer (GL_RENDERBUFFER, rbo);
    glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 100, 50);
    glGenBuffers (2, buf);
    glBindBuffer (GL_ARRAY_BUFFER, buf[0]);
    glBufferData (GL_ARRAY_BUFFER, 3000, NULL, GL_STREAM_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, buf[1]);
    glBufferData (GL_ARRAY_BUFFER, 70000, NULL, GL_STATIC_DRAW);
    glGenVertexArrays (1, &vao);
    glBindVertexArray (vao);
    GLuint prog = glCreateProgram (), sh = glCreateShader (GL_VERTEX_SHADER);
    const char *src = "#version 330\nvoid main(){ gl_Position = vec4(0.0); }\n";
    glShaderSource (sh, 1, &src, NULL);
    glCompileShader (sh);
    glAttachShader (prog, sh);
    glLinkProgram (prog);
    while (glGetError () != GL_NO_ERROR)
        ;

    /* what is bound when the census runs must be bound after it */
    glActiveTexture (GL_TEXTURE0 + 3);
    glBindTexture (GL_TEXTURE_2D, tex[2]);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
    glBindBuffer (GL_ARRAY_BUFFER, buf[1]);
    glUseProgram (prog);

    JsonObject *g1 = census (&r1, TRUE);
    check (num (g1, "textures.count") == num (g0, "textures.count") + 5, "five more textures");
    const gint64 want = 3 * 333 * 77 * 4 + 5461 + 16 * 16 * 4 * 8;
    check (num (g1, "textures.bytes") == num (g0, "textures.bytes") + want,
           "their bytes: three RGBA8 pictures, an R8 mip chain, an RGBA16F array");
    gint64 n = 0, by = 0;
    const char *w = row_of (g1, "textures", "333x77 RGBA8 (render target)", &n, &by);
    check (w && n == 1 && by == 333 * 77 * 4, "the picture a framebuffer renders into is a row of its own");
    w = row_of (g1, "textures", "64x64 R8 +mips", &n, &by);
    check (w && n == 1 && by == 5461, "a mip chain is counted with its levels");
    w = row_of (g1, "textures", "16x16x4 RGBA16F array", &n, &by);
    check (w && n == 1 && by == 16 * 16 * 4 * 8, "an array texture by its layers and format");
    check (num (g1, "textures.render_targets") == num (g0, "textures.render_targets") + 1 &&
           num (g1, "textures.render_target_bytes") ==
               num (g0, "textures.render_target_bytes") + 333 * 77 * 4,
           "render targets are summed");
    check (num (g1, "framebuffers.count") == num (g0, "framebuffers.count") + 1 &&
           num (g1, "vertex_arrays.count") == num (g0, "vertex_arrays.count") + 1 &&
           num (g1, "renderbuffers.count") == num (g0, "renderbuffers.count") + 1 &&
           num (g1, "renderbuffers.bytes") == num (g0, "renderbuffers.bytes") + 100 * 50 * 2,
           "framebuffer, vertex array, renderbuffer");
    check (num (g1, "buffers.count") == num (g0, "buffers.count") + 2 &&
           num (g1, "buffers.bytes") == num (g0, "buffers.bytes") + 73000 &&
           num (g1, "buffers.largest_bytes") >= 70000, "two buffers and their sizes");
    check (num (g1, "programs.count") == num (g0, "programs.count") + 1 &&
           num (g1, "shaders.count") == num (g0, "shaders.count") + 1 &&
           num (g1, "programs.shaders_still_attached") ==
               num (g0, "programs.shaders_still_attached") + 1 &&
           num (g1, "shaders.source_bytes") >= (gint64) strlen (src),
           "a program, the shader still attached to it and its source text");
    check (num (g1, "programs.binary_bytes") >= 0, "deep: the programs' binaries are asked for");

    GLint at = 0, t2 = 0, rf = 0, df = -1, ab = 0, up = 0, va = 0;
    glGetIntegerv (GL_ACTIVE_TEXTURE, &at);
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &t2);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &rf);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &df);
    glGetIntegerv (GL_ARRAY_BUFFER_BINDING, &ab);
    glGetIntegerv (GL_CURRENT_PROGRAM, &up);
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &va);
    check (at == GL_TEXTURE0 + 3 && (GLuint) t2 == tex[2] && (GLuint) rf == fbo && df == 0 &&
           (GLuint) ab == buf[1] && (GLuint) up == prog && (GLuint) va == vao,
           "the census leaves every binding as it was");
    check (glGetError () == GL_NO_ERROR, "... and no GL error behind");

    glUseProgram (0);
    glDeleteProgram (prog);
    glDeleteShader (sh);
    glBindVertexArray (0);
    glDeleteVertexArrays (1, &vao);
    glDeleteBuffers (2, buf);
    glDeleteRenderbuffers (1, &rbo);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers (1, &fbo);
    glDeleteTextures (5, tex);
    JsonObject *g2 = census (&r2, FALSE);
    check (num (g2, "textures.count") == num (g0, "textures.count") &&
           num (g2, "textures.bytes") == num (g0, "textures.bytes") &&
           num (g2, "buffers.count") == num (g0, "buffers.count") &&
           num (g2, "framebuffers.count") == num (g0, "framebuffers.count") &&
           num (g2, "programs.count") == num (g0, "programs.count") &&
           num (g2, "shaders.count") == num (g0, "shaders.count") &&
           num (g2, "vertex_arrays.count") == num (g0, "vertex_arrays.count") &&
           num (g2, "renderbuffers.count") == num (g0, "renderbuffers.count"),
           "after the deletes the census is what it was before");
    check (!json_object_has_member (wpj_obj (g2, "programs"), "binary_bytes"),
           "without deep no program is asked for its binary");
    json_node_unref (r0);
    json_node_unref (r1);
    json_node_unref (r2);
}

int main (void)
{
    test_maps ();
    test_process ();
    test_counters ();

    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd) && nd > 0)
            dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[0], NULL);
    }
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_NONE };
    gboolean ok = dpy != EGL_NO_DISPLAY && eglInitialize (dpy, &maj, &min) &&
                  eglBindAPI (EGL_OPENGL_API) && eglChooseConfig (dpy, attr, &cfg, 1, &nc) && nc > 0;
    if (!ok && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        ok = dpy != EGL_NO_DISPLAY && eglInitialize (dpy, &maj, &min) &&
             eglBindAPI (EGL_OPENGL_API) && eglChooseConfig (dpy, attr, &cfg, 1, &nc) && nc > 0;
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    EGLContext ctx = ok ? eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca) : EGL_NO_CONTEXT;
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf ("skip (GL census): no surfaceless GL 3.3 context\n");
    } else {
        printf ("GL: %s\n", (const char *) glGetString (GL_RENDERER));
        test_gl ();
        eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext (dpy, ctx);
        eglTerminate (dpy);
    }
    printf ("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
