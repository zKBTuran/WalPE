/* gpusel_test.c - which GPU renders (src/common/gpusel.c) without a GPU,
 * a display or EGL: the device list read from made-up sysfs trees
 * (tests/fakegpu.h: laptops with two GPUs, desktops, a virtual machine,
 * an ARM board), the integrated / discrete classification, the values of
 * the `gpu` setting matched against the lists, what `auto` chooses on each
 * of the machines, the display device, the environment for rendering on
 * another GPU, the probe's report, the record of the GPU in use, the
 * config keys and the Mesa equivalents of gpu.conf's driver hints; and
 * that the wait for a test process ends when its time is up (stand-ins:
 * one that ignores signals and never ends, one that leaves a process
 * behind holding its pipes).
 * `gpusel-test --tree <machine> <dir>` writes one of the trees and nothing
 * else (tests/gpu_auto_test.sh starts a daemon on it).
 *
 * The trees and ids are illustrations written from the layout of sysfs
 * and public PCI ids, not captures of real machines. Nothing here says
 * that a real driver accepts the environment: that needs the hardware.
 *
 * Works in a private temp dir that is also the XDG config dir.
 * Run: meson test -C build gpusel -v */
#include "common/gpusel.h"
#include "common/config.h"
#include "common/present.h"
#include "fakegpu.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char *T;

static char *P (const char *rel)
{
    return g_build_filename (T, rel, NULL);
}

static WalpeGpuDev *dev (GPtrArray *a, guint i)
{
    return i < a->len ? g_ptr_array_index (a, i) : NULL;
}

/* value of `key` in a key, value, ..., NULL list */
static const char *pair (char **pairs, const char *key)
{
    for (int i = 0; pairs && pairs[i] && pairs[i + 1]; i += 2)
        if (!strcmp (pairs[i], key))
            return pairs[i + 1];
    return NULL;
}

static guint npairs (char **pairs)
{
    return pairs ? g_strv_length (pairs) / 2 : 0;
}

static void test_classify (void)
{
    gboolean g = TRUE;
    /* Intel: the processor's graphics is 00:02.0, anything else a card */
    CHECK (walpe_gpu_classify (0x8086, 0x9a49, "i915", "0000:00:02.0", &g) == WALPE_GPU_INTEGRATED && !g);
    CHECK (walpe_gpu_classify (0x8086, 0x56a0, "i915", "0000:03:00.0", &g) == WALPE_GPU_DISCRETE && !g);
    CHECK (walpe_gpu_classify (0x8086, 0x56a0, "xe", "0000:03:00.0", NULL) == WALPE_GPU_DISCRETE);
    /* NVIDIA: with its own driver, with nouveau, without any */
    CHECK (walpe_gpu_classify (0x10de, 0x28a1, "nvidia", "0000:01:00.0", &g) == WALPE_GPU_DISCRETE && !g);
    CHECK (walpe_gpu_classify (0x10de, 0x1c8c, "nouveau", "0000:01:00.0", &g) == WALPE_GPU_DISCRETE);
    CHECK (walpe_gpu_classify (0x10de, 0x28a1, "", "0000:01:00.0", &g) == WALPE_GPU_DISCRETE);
    /* AMD: processors with graphics by id, the rest are cards (assumed) */
    CHECK (walpe_gpu_classify (0x1002, 0x1638, "amdgpu", "0000:04:00.0", &g) == WALPE_GPU_INTEGRATED && !g);
    CHECK (walpe_gpu_classify (0x1002, 0x15d8, "amdgpu", "0000:05:00.0", &g) == WALPE_GPU_INTEGRATED && !g);
    CHECK (walpe_gpu_classify (0x1002, 0x9874, "amdgpu", "0000:00:01.0", &g) == WALPE_GPU_INTEGRATED);
    CHECK (walpe_gpu_classify (0x1002, 0x130f, "radeon", "0000:00:01.0", &g) == WALPE_GPU_INTEGRATED);
    CHECK (walpe_gpu_classify (0x1002, 0x73ff, "amdgpu", "0000:01:00.0", &g) == WALPE_GPU_DISCRETE && g);
    CHECK (walpe_gpu_classify (0x1002, 0x67df, "amdgpu", "0000:01:00.0", &g) == WALPE_GPU_DISCRETE && g);
    /* virtual adapters, management chips */
    CHECK (walpe_gpu_classify (0x1af4, 0x1050, "virtio-pci", "0000:00:01.0", &g) == WALPE_GPU_VIRTUAL && !g);
    CHECK (walpe_gpu_classify (0x15ad, 0x0405, "vmwgfx", "0000:00:0f.0", NULL) == WALPE_GPU_VIRTUAL);
    CHECK (walpe_gpu_classify (0x1a03, 0x2000, "ast", "0000:06:00.0", NULL) == WALPE_GPU_VIRTUAL);
    CHECK (walpe_gpu_classify (0, 0, "simpledrm", NULL, NULL) == WALPE_GPU_VIRTUAL);
    /* an SoC's GPU is integrated; no bus, no driver: nothing is known */
    CHECK (walpe_gpu_classify (0, 0, "panfrost", NULL, &g) == WALPE_GPU_INTEGRATED && !g);
    CHECK (walpe_gpu_classify (0, 0, "msm", "", &g) == WALPE_GPU_INTEGRATED);
    CHECK (walpe_gpu_classify (0, 0, "", NULL, &g) == WALPE_GPU_UNKNOWN && g);
    /* a PCI device of a vendor without a rule: a card, assumed */
    CHECK (walpe_gpu_classify (0x1ed5, 0x0100, "somedrv", "0000:01:00.0", &g) == WALPE_GPU_DISCRETE && g);

    CHECK (!strcmp (walpe_gpu_class_name (WALPE_GPU_INTEGRATED), "integrated"));
    CHECK (!strcmp (walpe_gpu_class_name (WALPE_GPU_DISCRETE), "discrete"));
    CHECK (!strcmp (walpe_gpu_class_name (WALPE_GPU_VIRTUAL), "virtual"));
    CHECK (!strcmp (walpe_gpu_class_name (WALPE_GPU_UNKNOWN), "unknown"));
    CHECK (walpe_gpu_driver_is_nvidia ("nvidia") && !walpe_gpu_driver_is_nvidia ("nouveau") &&
           !walpe_gpu_driver_is_nvidia ("") && !walpe_gpu_driver_is_nvidia (NULL));

    char *t = walpe_gpu_pci_path_tag ("0000:01:00.0");
    CHECK (!g_strcmp0 (t, "pci-0000_01_00_0"));
    g_free (t);
    t = walpe_gpu_pci_path_tag ("0000:0A:00.1");
    CHECK (!g_strcmp0 (t, "pci-0000_0a_00_1"));
    g_free (t);
    CHECK (walpe_gpu_pci_path_tag (NULL) == NULL);

    /* the preset that suits a kind of renderer (preset_follow_gpu) */
    CHECK (!g_strcmp0 (walpe_gpu_class_preset (WALPE_GPU_DISCRETE, FALSE), "high"));
    CHECK (!g_strcmp0 (walpe_gpu_class_preset (WALPE_GPU_INTEGRATED, FALSE), "balanced"));
    CHECK (!g_strcmp0 (walpe_gpu_class_preset (WALPE_GPU_DISCRETE, TRUE), "low"));
    CHECK (!g_strcmp0 (walpe_gpu_class_preset (WALPE_GPU_VIRTUAL, FALSE), "low"));
    CHECK (walpe_gpu_class_preset (WALPE_GPU_UNKNOWN, FALSE) == NULL);
}

static void test_laptop_intel_nvidia (void)
{
    char *root = P ("intel-nvidia");
    fakegpu_laptop_intel_nvidia (root);
    GPtrArray *a = walpe_gpu_enumerate (root);
    CHECK (a->len == 2);
    WalpeGpuDev *i = dev (a, 0), *n = dev (a, 1);
    if (i && n) {
        CHECK (i->card == 0 && !g_strcmp0 (i->card_node, "/dev/dri/card0") &&
               !g_strcmp0 (i->render_node, "/dev/dri/renderD128"));
        CHECK (!g_strcmp0 (i->pci, "0000:00:02.0") && !g_strcmp0 (i->path_tag, "pci-0000_00_02_0"));
        CHECK (i->vendor == 0x8086 && i->device == 0x9a49 && !g_strcmp0 (i->driver, "i915"));
        CHECK (i->klass == WALPE_GPU_INTEGRATED && !i->class_guessed && i->boot_vga);
        CHECK (i->n_connected == 1 && i->connectors && !g_strcmp0 (i->connectors[0], "eDP-1") &&
               i->connectors[1] == NULL);
        CHECK (i->sysfs && g_str_has_suffix (i->sysfs, "/class/drm/card0/device"));
        /* no id list in the test: the name is made from the ids */
        CHECK (i->name && strstr (i->name, "Intel") && strstr (i->name, "8086:9a49"));
        CHECK (n->card == 1 && !g_strcmp0 (n->render_node, "/dev/dri/renderD129"));
        CHECK (n->vendor == 0x10de && n->device == 0x28a1 && !g_strcmp0 (n->driver, "nvidia"));
        CHECK (n->klass == WALPE_GPU_DISCRETE && !n->boot_vga && n->n_connected == 0);
        CHECK (n->connectors && n->connectors[0] == NULL);
    }
    /* the panel hangs on the Intel device */
    int disp = walpe_gpu_display_index (a, NULL, NULL);
    CHECK (disp == 0);
    static const char *const outs[] = { "eDP-1", NULL };
    CHECK (walpe_gpu_display_index (a, outs, NULL) == 0);
    CHECK (walpe_gpu_display_index (a, NULL, "/dev/dri/card1") == 1);
    CHECK (walpe_gpu_display_index (a, NULL, "/dev/dri/renderD128") == 0);
    CHECK (walpe_gpu_default_renderer (a, 0) == 0 && walpe_gpu_default_renderer (a, 1) == 1);
    CHECK (walpe_gpu_default_renderer (a, -1) == -1);     /* two renderers: no guess */
    CHECK (walpe_gpu_index_of_node (a, "/dev/dri/renderD129") == 1);
    CHECK (walpe_gpu_index_of_node (a, "renderD129") == 1);
    CHECK (walpe_gpu_index_of_node (a, "/dev/dri/card7") == -1);
    CHECK (walpe_gpu_index_of_node (a, NULL) == -1);

    /* the setting */
    char *why = (char *) 1;
    CHECK (walpe_gpu_match (a, "auto", disp, &why) == -1 && why == NULL);
    CHECK (walpe_gpu_match (a, NULL, disp, NULL) == -1);
    CHECK (walpe_gpu_match (a, "", disp, NULL) == -1);
    CHECK (walpe_gpu_match (a, "integrated", disp, &why) == 0 && why == NULL);
    CHECK (walpe_gpu_match (a, "discrete", disp, &why) == 1 && why == NULL);
    CHECK (walpe_gpu_match (a, " Discrete ", disp, NULL) == 1);
    CHECK (walpe_gpu_match (a, "0000:01:00.0", disp, NULL) == 1);
    CHECK (walpe_gpu_match (a, "01:00.0", disp, NULL) == 1);
    CHECK (walpe_gpu_match (a, "00:02.0", disp, NULL) == 0);
    CHECK (walpe_gpu_match (a, "pci-0000_01_00_0", disp, NULL) == 1);
    CHECK (walpe_gpu_match (a, "pci:0000:01:00.0", disp, NULL) == 1);
    CHECK (walpe_gpu_match (a, "10de:28a1", disp, NULL) == 1);
    CHECK (walpe_gpu_match (a, "10DE:28A1", disp, NULL) == 1);
    CHECK (walpe_gpu_match (a, "card1", disp, NULL) == 1);
    CHECK (walpe_gpu_match (a, "/dev/dri/renderD128", disp, NULL) == 0);
    CHECK (walpe_gpu_match (a, "nvidia", disp, NULL) == 1);
    CHECK (walpe_gpu_match (a, "NVIDIA", disp, NULL) == 1);
    CHECK (walpe_gpu_match (a, "intel", disp, NULL) == 0);
    CHECK (walpe_gpu_match (a, "i915", disp, NULL) == 0);
    why = NULL;
    CHECK (walpe_gpu_match (a, "radeon", disp, &why) == -1);
    CHECK (why && strstr (why, "radeon") && strstr (why, "found:") && strstr (why, "0000:01:00.0"));
    g_free (why);
    why = NULL;
    CHECK (walpe_gpu_match (a, "02:00.0", disp, &why) == -1 && why);   /* the network card */
    g_free (why);

    /* rendering on the NVIDIA GPU while Intel drives the panel */
    char *note = (char *) 1;
    char **env = walpe_gpu_offload_env (i, n, WALPE_GPU_WS_X11, "/x/50_mesa.json",
                                        "/x/10_nvidia.json", &note);
    CHECK (npairs (env) == 3 && note == NULL);
    CHECK (!g_strcmp0 (pair (env, "__NV_PRIME_RENDER_OFFLOAD"), "1"));
    CHECK (!g_strcmp0 (pair (env, "__GLX_VENDOR_LIBRARY_NAME"), "nvidia"));
    CHECK (!g_strcmp0 (pair (env, "__EGL_VENDOR_LIBRARY_FILENAMES"), "/x/10_nvidia.json"));
    CHECK (pair (env, "DRI_PRIME") == NULL);
    g_strfreev (env);
    /* its vendor file not found: the two variables alone */
    env = walpe_gpu_offload_env (i, n, WALPE_GPU_WS_X11, "/x/50_mesa.json", NULL, &note);
    CHECK (npairs (env) == 2 && note == NULL && pair (env, "__EGL_VENDOR_LIBRARY_FILENAMES") == NULL);
    g_strfreev (env);
    env = walpe_gpu_offload_env (i, n, WALPE_GPU_WS_WAYLAND, NULL, NULL, &note);
    CHECK (npairs (env) == 2 && note != NULL);
    g_free (note);
    g_strfreev (env);
    /* the other way round (the X server on the NVIDIA GPU): Mesa has to
     * be the GL vendor, pointed at the Intel device */
    note = NULL;
    env = walpe_gpu_offload_env (n, i, WALPE_GPU_WS_X11, "/x/50_mesa.json", "/x/10_nvidia.json",
                                 &note);
    CHECK (npairs (env) == 3 && note == NULL);
    CHECK (!g_strcmp0 (pair (env, "DRI_PRIME"), "pci-0000_00_02_0"));
    CHECK (!g_strcmp0 (pair (env, "__EGL_VENDOR_LIBRARY_FILENAMES"), "/x/50_mesa.json"));
    CHECK (!g_strcmp0 (pair (env, "__GLX_VENDOR_LIBRARY_NAME"), "mesa"));
    CHECK (pair (env, "__NV_PRIME_RENDER_OFFLOAD") == NULL);
    g_strfreev (env);
    env = walpe_gpu_offload_env (n, i, WALPE_GPU_WS_X11, NULL, NULL, &note);
    CHECK (npairs (env) == 1 && note && strstr (note, "Mesa"));
    g_free (note);
    g_strfreev (env);
    /* the display device itself, or no target: nothing */
    note = NULL;
    env = walpe_gpu_offload_env (i, i, WALPE_GPU_WS_X11, "/x/m.json", "/x/n.json", &note);
    CHECK (npairs (env) == 0 && env && env[0] == NULL && note == NULL);
    g_strfreev (env);
    env = walpe_gpu_offload_env (i, NULL, WALPE_GPU_WS_X11, NULL, NULL, NULL);
    CHECK (npairs (env) == 0);
    g_strfreev (env);
    /* every variable it can set is in the list the daemon checks */
    const char *const *names = walpe_gpu_offload_env_names ();
    CHECK (g_strv_contains (names, "DRI_PRIME") && g_strv_contains (names, "__NV_PRIME_RENDER_OFFLOAD") &&
           g_strv_contains (names, "__GLX_VENDOR_LIBRARY_NAME") &&
           g_strv_contains (names, "__EGL_VENDOR_LIBRARY_FILENAMES"));

    /* descriptions */
    if (i) {
        char *d = walpe_gpu_dev_describe (i);
        CHECK (d && strstr (d, "0000:00:02.0") && strstr (d, "integrated") && strstr (d, "i915") &&
               strstr (d, "eDP-1") && strstr (d, "primary"));
        g_free (d);
        JsonBuilder *b = json_builder_new ();
        json_builder_begin_object (b);
        walpe_gpu_dev_to_json (b, i);
        json_builder_end_object (b);
        JsonNode *root_n = json_builder_get_root (b);
        JsonObject *o = json_node_get_object (root_n);
        CHECK (!g_strcmp0 (json_object_get_string_member (o, "pci"), "0000:00:02.0"));
        CHECK (!g_strcmp0 (json_object_get_string_member (o, "driver"), "i915"));
        CHECK (!g_strcmp0 (json_object_get_string_member (o, "class"), "integrated"));
        CHECK (!g_strcmp0 (json_object_get_string_member (o, "vendor_id"), "8086"));
        CHECK (!g_strcmp0 (json_object_get_string_member (o, "device_id"), "9a49"));
        CHECK (!g_strcmp0 (json_object_get_string_member (o, "render_node"), "/dev/dri/renderD128"));
        CHECK (json_object_get_boolean_member (o, "boot_vga"));
        CHECK (json_array_get_length (json_object_get_array_member (o, "connectors")) == 1);
        CHECK (!json_object_has_member (o, "class_guessed"));
        json_node_unref (root_n);
        g_object_unref (b);
    }
    g_ptr_array_unref (a);
    g_free (root);
}

static void test_other_machines (void)
{
    /* two amdgpu devices: the processor's graphics by its id */
    char *root = P ("amd-amd");
    fakegpu_laptop_amd_amd (root);
    GPtrArray *a = walpe_gpu_enumerate (root);
    CHECK (a->len == 2);
    if (a->len == 2) {
        WalpeGpuDev *apu = dev (a, 0), *card = dev (a, 1);
        CHECK (apu->klass == WALPE_GPU_INTEGRATED && !apu->class_guessed);
        CHECK (card->klass == WALPE_GPU_DISCRETE && card->class_guessed);
        int disp = walpe_gpu_display_index (a, NULL, NULL);
        CHECK (disp == 0);
        CHECK (walpe_gpu_match (a, "integrated", disp, NULL) == 0);
        CHECK (walpe_gpu_match (a, "discrete", disp, NULL) == 1);
        /* a word both fit: the display device */
        CHECK (walpe_gpu_match (a, "amd", disp, NULL) == 0);
        CHECK (walpe_gpu_match (a, "amdgpu", disp, NULL) == 0);
        CHECK (walpe_gpu_match (a, "amdgpu", 1, NULL) == 1);
        /* Mesa to Mesa: the device's tag, nothing else */
        char *note = NULL;
        char **env = walpe_gpu_offload_env (apu, card, WALPE_GPU_WS_X11, "/x/m.json", NULL, &note);
        CHECK (npairs (env) == 1 && !g_strcmp0 (pair (env, "DRI_PRIME"), "pci-0000_01_00_0") && !note);
        g_strfreev (env);
        env = walpe_gpu_offload_env (apu, card, WALPE_GPU_WS_WAYLAND, NULL, NULL, &note);
        CHECK (npairs (env) == 1 && !note);
        g_strfreev (env);
    }
    g_ptr_array_unref (a);
    g_free (root);

    /* one NVIDIA card: every value means that card */
    root = P ("nvidia");
    fakegpu_desktop_nvidia (root);
    a = walpe_gpu_enumerate (root);
    CHECK (a->len == 1);
    if (a->len == 1) {
        WalpeGpuDev *n = dev (a, 0);
        CHECK (n->klass == WALPE_GPU_DISCRETE && n->n_connected == 2);
        CHECK (!g_strcmp0 (n->connectors[0], "DP-1") && !g_strcmp0 (n->connectors[1], "HDMI-A-1"));
        int disp = walpe_gpu_display_index (a, NULL, NULL);
        CHECK (disp == 0);
        CHECK (walpe_gpu_match (a, "discrete", disp, NULL) == 0);
        char *why = NULL;
        CHECK (walpe_gpu_match (a, "integrated", disp, &why) == -1);
        CHECK (why && strstr (why, "no integrated GPU"));
        g_free (why);
        char **env = walpe_gpu_offload_env (n, n, WALPE_GPU_WS_X11, NULL, NULL, NULL);
        CHECK (npairs (env) == 0);
        g_strfreev (env);
    }
    g_ptr_array_unref (a);
    g_free (root);

    /* processor graphics unused, an Intel card with the monitor */
    root = P ("igpu-arc");
    fakegpu_desktop_igpu_arc (root);
    a = walpe_gpu_enumerate (root);
    CHECK (a->len == 2);
    if (a->len == 2) {
        CHECK (dev (a, 0)->klass == WALPE_GPU_INTEGRATED && dev (a, 1)->klass == WALPE_GPU_DISCRETE);
        int disp = walpe_gpu_display_index (a, NULL, NULL);
        CHECK (disp == 1);
        CHECK (walpe_gpu_match (a, "integrated", disp, NULL) == 0);
        CHECK (walpe_gpu_match (a, "discrete", disp, NULL) == 1);
        CHECK (walpe_gpu_match (a, "intel", disp, NULL) == 1);     /* both fit: the display device */
        CHECK (walpe_gpu_match (a, "intel", -1, NULL) == 0);
    }
    g_ptr_array_unref (a);
    g_free (root);

    /* two cards with a monitor each: the outputs tell which one */
    root = P ("two-cards");
    fakegpu_desktop_two_cards (root);
    a = walpe_gpu_enumerate (root);
    CHECK (a->len == 2);
    if (a->len == 2) {
        static const char *const dp[] = { "DisplayPort-0", NULL };
        static const char *const hdmi[] = { "HDMI-0", NULL };
        static const char *const both[] = { "DP-1", "HDMI-A-1", NULL };
        CHECK (walpe_gpu_display_index (a, dp, NULL) == 0);
        CHECK (walpe_gpu_display_index (a, hdmi, NULL) == 1);
        /* a wallpaper on both: no single answer from the outputs -> the primary adapter */
        CHECK (walpe_gpu_display_index (a, both, NULL) == 0);
        CHECK (walpe_gpu_display_index (a, NULL, NULL) == 0);
        /* two discrete devices: the display device when it is one, else the primary */
        CHECK (walpe_gpu_match (a, "discrete", 1, NULL) == 1);
        CHECK (walpe_gpu_match (a, "discrete", -1, NULL) == 0);
    }
    g_ptr_array_unref (a);
    g_free (root);

    /* a virtual machine: one adapter, neither integrated nor discrete */
    root = P ("vm");
    fakegpu_vm (root);
    a = walpe_gpu_enumerate (root);
    CHECK (a->len == 1 && dev (a, 0)->klass == WALPE_GPU_VIRTUAL);
    CHECK (walpe_gpu_display_index (a, NULL, NULL) == 0);
    CHECK (walpe_gpu_match (a, "discrete", 0, NULL) == -1);
    CHECK (walpe_gpu_match (a, "integrated", 0, NULL) == -1);
    g_ptr_array_unref (a);
    g_free (root);

    /* an ARM board: the display controller shows, the GPU renders */
    root = P ("arm");
    fakegpu_arm_board (root);
    a = walpe_gpu_enumerate (root);
    CHECK (a->len == 2);
    if (a->len == 2) {
        WalpeGpuDev *dc = dev (a, 0), *gpu = dev (a, 1);
        CHECK (dc->pci == NULL && dc->render_node == NULL && dc->klass == WALPE_GPU_VIRTUAL);
        CHECK (!g_strcmp0 (gpu->driver, "panfrost") && gpu->klass == WALPE_GPU_INTEGRATED);
        CHECK (!g_strcmp0 (gpu->render_node, "/dev/dri/renderD128"));
        CHECK (!g_strcmp0 (gpu->path_tag, "platform-ff9a0000_gpu"));
        CHECK (gpu->name && strstr (gpu->name, "mali"));
        CHECK (walpe_gpu_display_index (a, NULL, NULL) == 0);
        /* the controller renders nothing: the GPU beside it is what EGL
         * uses without any setting - not a second GPU to copy frames to */
        CHECK (walpe_gpu_default_renderer (a, 0) == 1);
        CHECK (walpe_gpu_default_renderer (a, -1) == 1);
        CHECK (walpe_gpu_match (a, "integrated", 0, NULL) == 1);
        CHECK (walpe_gpu_match (a, "panfrost", 0, NULL) == 1);
    }
    g_ptr_array_unref (a);
    g_free (root);

    /* an NVIDIA GPU without any driver: listed from the PCI bus, and the
     * setting cannot be honoured with it */
    root = P ("undriven");
    fakegpu_laptop_nvidia_undriven (root);
    a = walpe_gpu_enumerate (root);
    CHECK (a->len == 2);
    if (a->len == 2) {
        WalpeGpuDev *i = dev (a, 0), *n = dev (a, 1);
        CHECK (n->card == -1 && n->card_node == NULL && n->render_node == NULL);
        CHECK (n->vendor == 0x10de && !g_strcmp0 (n->pci, "0000:01:00.0") &&
               n->klass == WALPE_GPU_DISCRETE && !g_strcmp0 (n->driver, ""));
        CHECK (walpe_gpu_match (a, "discrete", 0, NULL) == 1);
        char *note = NULL;
        char **env = walpe_gpu_offload_env (i, n, WALPE_GPU_WS_X11, "/x/m.json", "/x/n.json", &note);
        CHECK (npairs (env) == 0 && note && strstr (note, "no kernel driver"));
        g_free (note);
        g_strfreev (env);
        char *d = walpe_gpu_dev_describe (n);
        CHECK (d && strstr (d, "no driver loaded"));
        g_free (d);
        /* the same device with NVIDIA's driver bound but without its DRM
         * module: nothing is set either, and the reason is not "no driver" */
        g_free (n->driver);
        n->driver = g_strdup ("nvidia");
        note = NULL;
        env = walpe_gpu_offload_env (i, n, WALPE_GPU_WS_X11, "/x/m.json", "/x/n.json", &note);
        CHECK (npairs (env) == 0 && note && strstr (note, "no DRM device") &&
               !strstr (note, "no kernel driver"));
        g_free (note);
        g_strfreev (env);
    }
    g_ptr_array_unref (a);
    g_free (root);

    /* no sysfs at all (a container): an empty list, nothing fails */
    root = P ("nothing");
    a = walpe_gpu_enumerate (root);
    CHECK (a && a->len == 0);
    CHECK (walpe_gpu_display_index (a, NULL, NULL) == -1);
    char *why = NULL;
    CHECK (walpe_gpu_match (a, "discrete", -1, &why) == -1 && why && strstr (why, "no graphics device"));
    g_free (why);
    g_ptr_array_unref (a);
    g_free (root);

    /* the root from the environment */
    root = P ("vm");
    g_setenv ("WALPE_GPU_SYSFS_ROOT", root, TRUE);
    CHECK (!g_strcmp0 (walpe_gpu_sysfs_root (), root));
    a = walpe_gpu_enumerate (NULL);
    CHECK (a->len == 1);
    g_ptr_array_unref (a);
    g_unsetenv ("WALPE_GPU_SYSFS_ROOT");
    CHECK (!g_strcmp0 (walpe_gpu_sysfs_root (), "/sys"));
    g_free (root);
}

static void test_outputs_and_values (void)
{
    /* kernel names against the names X drivers and compositors use */
    CHECK (walpe_gpu_output_matches ("HDMI-A-1", "HDMI-A-1"));
    CHECK (walpe_gpu_output_matches ("HDMI-A-1", "HDMI-1"));
    CHECK (walpe_gpu_output_matches ("HDMI-A-1", "HDMI1"));
    CHECK (walpe_gpu_output_matches ("HDMI-A-1", "HDMI-0"));
    CHECK (walpe_gpu_output_matches ("HDMI-A-1", "HDMI-1-1"));
    CHECK (walpe_gpu_output_matches ("DP-2", "DisplayPort-1"));
    CHECK (walpe_gpu_output_matches ("eDP-1", "eDP"));
    CHECK (walpe_gpu_output_matches ("eDP-1", "eDP1"));
    CHECK (!walpe_gpu_output_matches ("eDP-1", "DP-1"));
    CHECK (!walpe_gpu_output_matches ("HDMI-A-1", "DP-1"));
    CHECK (!walpe_gpu_output_matches ("DVI-D-1", "VGA-1"));
    CHECK (!walpe_gpu_output_matches (NULL, "DP-1") && !walpe_gpu_output_matches ("DP-1", NULL));

    char *v = walpe_gpu_value_normalize (NULL);
    CHECK (!g_strcmp0 (v, "auto"));
    g_free (v);
    v = walpe_gpu_value_normalize ("  ");
    CHECK (!g_strcmp0 (v, "auto"));
    g_free (v);
    v = walpe_gpu_value_normalize (" Integrated ");
    CHECK (!g_strcmp0 (v, "integrated"));
    g_free (v);
    v = walpe_gpu_value_normalize ("AUTO");
    CHECK (!g_strcmp0 (v, "auto"));
    g_free (v);
    v = walpe_gpu_value_normalize (" 0000:01:00.0 ");
    CHECK (!g_strcmp0 (v, "0000:01:00.0"));
    g_free (v);
    v = walpe_gpu_value_normalize ("GeForce RTX");
    CHECK (!g_strcmp0 (v, "GeForce RTX"));
    g_free (v);
    CHECK (walpe_gpu_value_normalize ("a\nb") == NULL);
    /* not UTF-8: it would go into the status reply as it is */
    CHECK (walpe_gpu_value_normalize ("nv\xff\xfe") == NULL);
    v = walpe_gpu_value_normalize ("Carte graphique \xc3\xa9");
    CHECK (!g_strcmp0 (v, "Carte graphique \xc3\xa9"));
    g_free (v);
    char big[200];
    memset (big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    CHECK (walpe_gpu_value_normalize (big) == NULL);
    CHECK (walpe_gpu_value_is_word ("auto") && walpe_gpu_value_is_word (" Discrete") &&
           walpe_gpu_value_is_word (NULL) && !walpe_gpu_value_is_word ("nvidia"));
    /* "display": a word, stored in lower case, and not a device's name */
    CHECK (walpe_gpu_value_is_word ("display") && walpe_gpu_value_is_word (" Display "));
    CHECK (walpe_gpu_value_is_display ("display") && walpe_gpu_value_is_display (" DISPLAY ") &&
           !walpe_gpu_value_is_display ("auto") && !walpe_gpu_value_is_display (NULL) &&
           !walpe_gpu_value_is_display ("") && !walpe_gpu_value_is_display ("displayport"));
    v = walpe_gpu_value_normalize (" Display ");
    CHECK (!g_strcmp0 (v, "display"));
    g_free (v);
}

/* the device gpu = auto renders on, for a machine of fakegpu.h: display =
 * what sysfs says drives the displays (-2), or the index given */
static int auto_on (const char *machine, int display, char **why)
{
    char *root = P (machine);
    CHECK (fakegpu_machine (machine, root));
    GPtrArray *a = walpe_gpu_enumerate (root);
    if (display == -2)
        display = walpe_gpu_default_renderer (a, walpe_gpu_display_index (a, NULL, NULL));
    int t = walpe_gpu_auto_target (a, display, why);
    /* `display` names no device on any machine, and never has a reason */
    char *w = (char *) 1;
    CHECK (walpe_gpu_match (a, "display", display, &w) == -1 && w == NULL);
    g_ptr_array_unref (a);
    g_free (root);
    return t;
}

/* What gpu = auto means, row by row. -1 = the window system's GPU renders
 * and nothing is set (what `display` means everywhere, and what auto meant
 * up to 0.14.0). */
static void test_auto (void)
{
    char *why = (char *) 1;
    /* one GPU: that one, whatever its kind; nothing to say */
    CHECK (auto_on ("desktop-nvidia", -2, &why) == -1 && why == NULL);
    CHECK (auto_on ("vm", -2, &why) == -1 && why == NULL);
    /* a card drives the displays, the processor's graphics sits unused:
     * the card renders already */
    CHECK (auto_on ("desktop-igpu-arc", -2, &why) == -1 && why == NULL);
    /* ... and auto never goes from a card to the integrated GPU or to
     * another card, whichever of two cards the window system uses */
    CHECK (auto_on ("desktop-two-cards", -2, &why) == -1 && why == NULL);
    CHECK (auto_on ("desktop-two-cards", 1, &why) == -1 && why == NULL);
    /* which device drives the displays is not known: nothing is guessed */
    CHECK (auto_on ("laptop-intel-nvidia", -1, &why) == -1 && why == NULL);
    CHECK (auto_on ("laptop-intel-nvidia", 7, &why) == -1 && why == NULL);
    CHECK (walpe_gpu_auto_target (NULL, 0, &why) == -1 && why == NULL);

    /* a laptop: the integrated GPU drives the panel, the second GPU renders */
    CHECK (auto_on ("laptop-intel-nvidia", -2, &why) == 1 && why == NULL);
    /* ... unless the window system turns out to render on that one itself
     * (EGL's answer, asked after sysfs's): then it is a card that drives
     * the displays */
    CHECK (auto_on ("laptop-intel-nvidia", 1, &why) == -1 && why == NULL);
    /* two Mesa devices; the card's class is "assumed" and it is AMD's */
    CHECK (auto_on ("laptop-amd-amd", -2, &why) == 1 && why == NULL);
    /* a device of a vendor no rule knows is not chosen on a guess */
    {
        char *root = P ("unknown-card");
        fakegpu_laptop_unknown_card (root);
        GPtrArray *a = walpe_gpu_enumerate (root);
        CHECK (a->len == 2 && dev (a, 1)->klass == WALPE_GPU_DISCRETE && dev (a, 1)->class_guessed);
        CHECK (walpe_gpu_auto_target (a, 0, &why) == -1 && why == NULL);
        /* (by name it can still be asked for) */
        CHECK (walpe_gpu_match (a, "discrete", 0, NULL) == 1);
        g_ptr_array_unref (a);
        g_free (root);
    }

    /* EGL draws and names no device: the renderer's name says whose GPU */
    CHECK (walpe_gpu_renderer_is_vendor ("NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2", 0x10de));
    CHECK (!walpe_gpu_renderer_is_vendor ("NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2", 0x8086));
    CHECK (walpe_gpu_renderer_is_vendor ("Mesa Intel(R) Graphics (ADL GT2)", 0x8086));
    CHECK (!walpe_gpu_renderer_is_vendor ("Mesa Intel(R) Graphics (ADL GT2)", 0x10de));
    CHECK (walpe_gpu_renderer_is_vendor ("AMD Radeon RX 7600 (radeonsi, navi33)", 0x1002));
    CHECK (!walpe_gpu_renderer_is_vendor ("llvmpipe (LLVM 20.1.2, 256 bits)", 0x10de));
    CHECK (!walpe_gpu_renderer_is_vendor (NULL, 0x10de) &&
           !walpe_gpu_renderer_is_vendor ("NVIDIA GeForce", 0) &&
           !walpe_gpu_renderer_is_vendor ("NVIDIA GeForce", 0x1ed5));

    {
        /* X11: the server's RandR providers say whose driver draws the
         * screen. "NVIDIA-0" is an X screen of NVIDIA's driver - the
         * window system renders on that card, whatever the list of
         * devices suggests; "NVIDIA-G0" is the card that only renders on
         * request beside another driver's screen (a laptop as auto means
         * it), and no other name says anything. */
        static const char *const nv[] = { "NVIDIA-0", NULL };
        static const char *const nv_sink[] = { "NVIDIA-0", "modesetting", NULL };
        static const char *const nv_second[] = { "modesetting", "NVIDIA-1", NULL };
        static const char *const offload[] = { "modesetting", "NVIDIA-G0", NULL };
        static const char *const mesa[] = { "modesetting", NULL };
        static const char *const odd[] = { "NVIDIA-", "NVIDIA-0x", "nvidia-0", "xNVIDIA-0", "", NULL };
        static const char *const none[] = { NULL };
        CHECK (walpe_gpu_x11_screen_is_nvidia (nv));
        CHECK (walpe_gpu_x11_screen_is_nvidia (nv_sink));
        CHECK (walpe_gpu_x11_screen_is_nvidia (nv_second));
        CHECK (!walpe_gpu_x11_screen_is_nvidia (offload));
        CHECK (!walpe_gpu_x11_screen_is_nvidia (mesa));
        CHECK (!walpe_gpu_x11_screen_is_nvidia (odd));
        CHECK (!walpe_gpu_x11_screen_is_nvidia (none));
        CHECK (!walpe_gpu_x11_screen_is_nvidia (NULL));
    }

    /* the second GPU has no driver: nothing is tried, and the reason is told */
    CHECK (auto_on ("laptop-nvidia-undriven", -2, &why) == -1);
    CHECK (why && strstr (why, "no kernel driver") && strstr (why, "rendering stays on"));
    g_free (why);
    why = NULL;
    {
        /* NVIDIA's driver bound without its DRM module; a device without a
         * render node */
        char *root = P ("laptop-intel-nvidia");
        GPtrArray *a = walpe_gpu_enumerate (root);
        WalpeGpuDev *n = dev (a, 1);
        int card = n->card;
        n->card = -1;
        CHECK (walpe_gpu_auto_target (a, 0, &why) == -1 && why && strstr (why, "no DRM device"));
        g_free (why);
        n->card = card;
        char *rn = n->render_node;
        n->render_node = NULL;
        CHECK (walpe_gpu_auto_target (a, 0, &why) == -1 && why && strstr (why, "no render node"));
        g_free (why);
        why = NULL;
        n->render_node = rn;
        CHECK (walpe_gpu_auto_target (a, 0, NULL) == 1);
        /* The processor's graphics has no driver at all (the firmware's
         * primary device of a laptop that runs on its card alone) and the
         * card's driver is there without its DRM module: the list names
         * the Intel device as the displays', and it renders nothing. Not
         * "rendering stays on" it: nothing is tried, nothing is said. */
        char *drv = dev (a, 0)->driver;
        dev (a, 0)->driver = g_strdup ("");
        n->card = -1;
        why = (char *) 1;
        CHECK (walpe_gpu_auto_target (a, 0, &why) == -1 && why == NULL);
        n->card = card;
        why = (char *) 1;
        CHECK (walpe_gpu_auto_target (a, 0, &why) == -1 && why == NULL);    /* (a card that could render, too) */
        why = NULL;
        g_free (dev (a, 0)->driver);
        dev (a, 0)->driver = drv;
        g_ptr_array_unref (a);
        g_free (root);
    }

    /* more than one discrete GPU beside the integrated one */
    {
        char *root = P ("laptop-two-discrete");
        fakegpu_laptop_two_discrete (root);
        GPtrArray *a = walpe_gpu_enumerate (root);
        CHECK (a->len == 3);
        if (a->len == 3) {
            /* the first by card number - the one `discrete` names too */
            CHECK (walpe_gpu_auto_target (a, 0, &why) == 1 && why == NULL);
            CHECK (walpe_gpu_match (a, "discrete", 0, NULL) == 1);
            /* the firmware's primary one before the others */
            dev (a, 2)->boot_vga = TRUE;
            CHECK (walpe_gpu_auto_target (a, 0, &why) == 2 && why == NULL);
            dev (a, 2)->boot_vga = FALSE;
            /* one that cannot be used is passed over, without a word */
            char *drv = dev (a, 1)->driver;
            dev (a, 1)->driver = g_strdup ("");
            CHECK (walpe_gpu_auto_target (a, 0, &why) == 2 && why == NULL);
            /* none can: the first is named, and that there is no other */
            dev (a, 2)->card = -1;
            CHECK (walpe_gpu_auto_target (a, 0, &why) == -1 && why &&
                   strstr (why, "NVIDIA") && strstr (why, "no other discrete GPU"));
            g_free (why);
            why = NULL;
            g_free (dev (a, 1)->driver);
            dev (a, 1)->driver = drv;
            /* what is set for the one chosen is what `discrete` sets */
            char **env = walpe_gpu_offload_env (dev (a, 0), dev (a, 1), WALPE_GPU_WS_WAYLAND,
                                                "/x/m.json", "/x/n.json", NULL);
            CHECK (!g_strcmp0 (pair (env, "__NV_PRIME_RENDER_OFFLOAD"), "1") &&
                   !g_strcmp0 (pair (env, "__EGL_VENDOR_LIBRARY_FILENAMES"), "/x/n.json") &&
                   !pair (env, "DRI_PRIME"));
            g_strfreev (env);
        }
        g_ptr_array_unref (a);
        g_free (root);
    }
}

/* is that process still there (and not only what is left of one that ended)? */
static gboolean pid_runs (int pid)
{
    char *p = g_strdup_printf ("/proc/%d/stat", pid), *txt = NULL;
    gboolean runs = FALSE;
    if (g_file_get_contents (p, &txt, NULL, NULL)) {
        const char *rp = strrchr (txt, ')');
        runs = rp && rp[1] == ' ' && rp[2] != 'Z' && rp[2] != 'X';
    }
    g_free (txt);
    g_free (p);
    return runs;
}

/* walpe_gpu_run_limited: the wait for a test process ends by this
 * process's clock, whatever the program does. Stand-ins written in sh. */
static void test_run_limited (void)
{
    char *out = NULL, *err = NULL;
    gboolean late = TRUE;
    GError *e = NULL;
#define SH(script) (char *[]) { (char *) "/bin/sh", (char *) "-c", (char *) (script), NULL }
#define MS_SINCE(t0) ((int) ((g_get_monotonic_time () - (t0)) / 1000))
    /* a program that answers and ends: both streams, at once */
    gint64 t0 = g_get_monotonic_time ();
    CHECK (walpe_gpu_run_limited (SH ("echo renderer=x; echo oops >&2; exit 3"), NULL, 5000, &out,
                                  &err, &late, &e));
    CHECK (!late && !e && !g_strcmp0 (out, "renderer=x\n") && !g_strcmp0 (err, "oops\n"));
    CHECK (MS_SINCE (t0) < 4000);
    g_free (out);
    g_free (err);
    /* its environment is the one given */
    {
        char **env = g_environ_setenv (g_get_environ (), "WALPE_T_RUN", "given", TRUE);
        CHECK (walpe_gpu_run_limited (SH ("echo $WALPE_T_RUN"), env, 5000, &out, &err, &late, &e));
        CHECK (!late && !g_strcmp0 (out, "given\n"));
        g_strfreev (env);
        g_free (out);
        g_free (err);
    }
    /* more than a pipe holds: read to the end (the program is not left
     * waiting for a reader), 64 KiB kept */
    t0 = g_get_monotonic_time ();
    CHECK (walpe_gpu_run_limited (SH ("head -c 300000 /dev/zero | tr '\\0' x; echo done >&2"), NULL,
                                  10000, &out, &err, &late, &e));
    CHECK (!late && out && strlen (out) == 64 * 1024 && !g_strcmp0 (err, "done\n"));
    CHECK (MS_SINCE (t0) < 8000);
    g_free (out);
    g_free (err);
    /* a program that cannot be started */
    CHECK (!walpe_gpu_run_limited ((char *[]) { (char *) "/nonexistent/walpe-no-such-program", NULL },
                                   NULL, 1000, &out, &err, &late, &e));
    CHECK (e != NULL && out == NULL && err == NULL);
    g_clear_error (&e);

    /* a program that never ends and ignores SIGTERM and its own alarm: the
     * wait ends when the time is up, what it wrote so far is there, and it
     * is ended from outside */
    t0 = g_get_monotonic_time ();
    CHECK (walpe_gpu_run_limited (SH ("trap '' TERM ALRM INT HUP; echo $$; "
                                      "while :; do sleep 0.05; done"),
                                  NULL, 700, &out, &err, &late, &e));
    int ms = MS_SINCE (t0);
    CHECK (late && ms >= 700 && ms < 700 + 3000);     /* (generous: a busy machine) */
    int pid = out ? atoi (out) : 0;
    CHECK (pid > 1);
    {
        /* (it was sent SIGKILL and not waited for: gone within moments) */
        int n = 0;
        while (pid > 1 && pid_runs (pid) && n++ < 100)
            g_usleep (20 * 1000);
        CHECK (pid > 1 && !pid_runs (pid));
    }
    g_free (out);
    g_free (err);

    /* a program that answers, ends and leaves a process behind that holds
     * both streams for 4 s (g_spawn_sync waits for that one): back with
     * the answer long before */
    t0 = g_get_monotonic_time ();
    CHECK (walpe_gpu_run_limited (SH ("sleep 4 & echo drew=1"), NULL, 10000, &out, &err, &late, &e));
    ms = MS_SINCE (t0);
    CHECK (!late && !g_strcmp0 (out, "drew=1\n") && ms < 3000);
    g_free (out);
    g_free (err);
    /* ... and one that holds them and goes on writing for 4 s and more */
    t0 = g_get_monotonic_time ();
    CHECK (walpe_gpu_run_limited (SH ("(trap '' TERM; n=0; while [ $n -lt 200 ]; do echo x; "
                                      "sleep 0.02; n=$((n + 1)); done) & echo drew=1"),
                                  NULL, 10000, &out, &err, &late, &e));
    ms = MS_SINCE (t0);
    /* (the other process may get its first line in before the answer) */
    CHECK (!late && out && strstr (out, "drew=1\n") && ms < 3000);
    g_free (out);
    g_free (err);

    /* none of them was this process's child: nothing is left to be reaped,
     * now or when the ones still running end */
    errno = 0;
    CHECK (waitpid (-1, NULL, WNOHANG) == -1 && errno == ECHILD);
#undef SH
#undef MS_SINCE
}

#define ICD(lib) "{ \"file_format_version\" : \"1.0.0\", \"ICD\" : { \"library_path\" : \"" lib "\" } }"

static void test_vendor_files_and_probe (void)
{
    fakegpu_put (T, "glvnd/10_nvidia.json", ICD ("libEGL_nvidia.so.0"));
    fakegpu_put (T, "glvnd/50_mesa.json", ICD ("libEGL_mesa.so.0"));
    fakegpu_put (T, "glvnd/notes.txt", "not a vendor file");
    char *d = P ("glvnd");
    char *f = walpe_gpu_find_egl_vendor (d, "egl_mesa");
    CHECK (f && g_str_has_suffix (f, "/glvnd/50_mesa.json"));
    g_free (f);
    f = walpe_gpu_find_egl_vendor (d, "egl_nvidia");
    CHECK (f && g_str_has_suffix (f, "/glvnd/10_nvidia.json"));
    g_free (f);
    CHECK (walpe_gpu_find_egl_vendor (d, "egl_other") == NULL);
    CHECK (walpe_gpu_find_egl_vendor (d, NULL) == NULL);
    g_free (d);
    d = P ("no-such-dir");
    CHECK (walpe_gpu_find_egl_vendor (d, "egl_mesa") == NULL);
    g_free (d);

    /* the probe child's report */
    char *dev_s = NULL, *rn = NULL, *r = NULL, *why = NULL;
    CHECK (walpe_gpu_parse_probe ("device=/dev/dri/card1\nrender_node=/dev/dri/renderD129\n"
                                  "egl=NVIDIA\nvendor=NVIDIA Corporation\n"
                                  "renderer=NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2\n"
                                  "version=4.6.0 NVIDIA 550.120\ndrew=1\n",
                                  &dev_s, &rn, &r, &why));
    CHECK (!g_strcmp0 (dev_s, "/dev/dri/card1") && !g_strcmp0 (rn, "/dev/dri/renderD129") &&
           r && strstr (r, "RTX 4050") && why == NULL);
    g_free (dev_s);
    g_free (rn);
    g_free (r);
    /* the driver fell back to the CPU: not a GPU choice that worked */
    CHECK (!walpe_gpu_parse_probe ("renderer=llvmpipe (LLVM 17.0.6, 256 bits)\ndrew=1\n", NULL, NULL,
                                   &r, &why));
    CHECK (why && strstr (why, "software renderer") && r && strstr (r, "llvmpipe"));
    g_free (r);
    g_free (why);
    CHECK (!walpe_gpu_parse_probe ("fail=EGL does not start on this display with these settings\n",
                                   NULL, NULL, NULL, &why));
    CHECK (why && strstr (why, "EGL does not start"));
    g_free (why);
    CHECK (!walpe_gpu_parse_probe ("renderer=Mesa Intel(R) Xe Graphics (TGL GT2)\ndrew=0\n", NULL,
                                   NULL, NULL, &why));
    CHECK (why && strstr (why, "could not draw"));
    g_free (why);
    CHECK (!walpe_gpu_parse_probe ("", NULL, NULL, NULL, &why) && why);
    g_free (why);
    CHECK (!walpe_gpu_parse_probe (NULL, NULL, NULL, NULL, NULL));
    /* a report without a device is still a success: not every EGL names it */
    CHECK (walpe_gpu_parse_probe ("renderer=AMD Radeon RX 6600 XT (radeonsi, navi23)\ndrew=1\n",
                                  &dev_s, &rn, NULL, NULL));
    CHECK (dev_s == NULL && rn == NULL);
}

static void test_in_use (void)
{
    WalpeGpuInUse u;
    walpe_gpu_in_use_set (NULL, FALSE);
    CHECK (!walpe_gpu_in_use (&u));
    char *root = P ("intel-nvidia");
    GPtrArray *a = walpe_gpu_enumerate (root);
    if (a->len == 2) {
        walpe_gpu_in_use_set (dev (a, 1), FALSE);
        CHECK (walpe_gpu_in_use (&u));
        CHECK (!u.software && !g_strcmp0 (u.driver, "nvidia") && !g_strcmp0 (u.pci, "0000:01:00.0") &&
               !g_strcmp0 (u.render_node, "/dev/dri/renderD129") &&
               !g_strcmp0 (u.card_node, "/dev/dri/card1") && u.vendor == 0x10de &&
               u.klass == WALPE_GPU_DISCRETE && u.sysfs && u.name);
        JsonBuilder *b = json_builder_new ();
        json_builder_begin_object (b);
        walpe_gpu_in_use_to_json (b, &u);
        json_builder_end_object (b);
        JsonNode *rn = json_builder_get_root (b);
        JsonObject *o = json_node_get_object (rn);
        CHECK (!g_strcmp0 (json_object_get_string_member (o, "driver"), "nvidia"));
        CHECK (!g_strcmp0 (json_object_get_string_member (o, "class"), "discrete"));
        CHECK (!json_object_get_boolean_member (o, "software"));
        CHECK (json_object_has_member (o, "sysfs"));
        json_node_unref (rn);
        g_object_unref (b);
        walpe_gpu_in_use_clear (&u);
        CHECK (u.driver == NULL && u.pci == NULL);
    }
    /* a CPU rasteriser: no device */
    walpe_gpu_in_use_set (NULL, TRUE);
    CHECK (walpe_gpu_in_use (&u) && u.software && u.pci == NULL && u.render_node == NULL &&
           !g_strcmp0 (u.driver, ""));
    walpe_gpu_in_use_clear (&u);
    walpe_gpu_in_use_set (NULL, FALSE);
    CHECK (!walpe_gpu_in_use (&u));
    g_ptr_array_unref (a);
    g_free (root);
}

static void test_config (void)
{
    /* a config that never used the setting does not gain a key */
    WalpeConfig *c = walpe_config_load ();
    CHECK (!g_strcmp0 (c->gpu, "auto") && !c->preset_follow_gpu && c->preset_gpu_class == NULL);
    walpe_config_save (c);
    char *path = g_build_filename (g_get_user_config_dir (), "walpe", "config.json", NULL);
    char *txt = NULL;
    CHECK (g_file_get_contents (path, &txt, NULL, NULL));
    CHECK (txt && !strstr (txt, "\"gpu\"") && !strstr (txt, "preset_follow_gpu") &&
           !strstr (txt, "preset_gpu_class"));
    g_free (txt);
    g_free (c->gpu);
    c->gpu = g_strdup ("discrete");
    c->preset_follow_gpu = TRUE;
    c->preset_gpu_class = g_strdup ("integrated");
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (!g_strcmp0 (c->gpu, "discrete") && c->preset_follow_gpu &&
           !g_strcmp0 (c->preset_gpu_class, "integrated"));
    /* back to the default: the key goes away again */
    g_free (c->gpu);
    c->gpu = g_strdup ("auto");
    c->preset_follow_gpu = FALSE;
    walpe_config_save (c);
    walpe_config_free (c);
    txt = NULL;
    CHECK (g_file_get_contents (path, &txt, NULL, NULL));
    CHECK (txt && !strstr (txt, "\"gpu\"") && !strstr (txt, "preset_follow_gpu"));
    g_free (txt);
    /* a value written by hand is normalised, a broken one keeps the default */
    CHECK (g_file_set_contents (path, "{ \"gpu\": \" Integrated \" }", -1, NULL));
    c = walpe_config_load ();
    CHECK (!g_strcmp0 (c->gpu, "integrated"));
    walpe_config_free (c);
    CHECK (g_file_set_contents (path, "{ \"gpu\": \"a\\nb\" }", -1, NULL));
    c = walpe_config_load ();
    CHECK (!g_strcmp0 (c->gpu, "auto"));
    walpe_config_free (c);
    g_unlink (path);
    g_free (path);
}

static gboolean strv_has (GPtrArray *a, const char *s)
{
    for (guint i = 0; i < a->len; i++)
        if (!strcmp (g_ptr_array_index (a, i), s))
            return TRUE;
    return FALSE;
}

static void test_mesa_hints (void)
{
    GPtrArray *ev = g_ptr_array_new_with_free_func (g_free);
    gboolean none = TRUE;
    CHECK (walpe_driver_hint_mesa ("threaded", "on", ev, &none) && !none);
    CHECK (ev->len == 1 && strv_has (ev, "mesa_glthread=true"));
    g_ptr_array_set_size (ev, 0);
    CHECK (walpe_driver_hint_mesa ("threaded", "off", ev, &none) && strv_has (ev, "mesa_glthread=false"));
    g_ptr_array_set_size (ev, 0);
    CHECK (walpe_driver_hint_mesa ("shader_cache", "/var/tmp/sc", ev, &none) && ev->len == 2 &&
           strv_has (ev, "MESA_SHADER_CACHE_DIR=/var/tmp/sc") &&
           strv_has (ev, "MESA_SHADER_CACHE_DISABLE=false"));
    g_ptr_array_set_size (ev, 0);
    CHECK (walpe_driver_hint_mesa ("shader_cache", "off", ev, &none) &&
           strv_has (ev, "MESA_SHADER_CACHE_DISABLE=true"));
    g_ptr_array_set_size (ev, 0);
    /* the hint is in bytes (the NVIDIA driver's unit), Mesa takes megabytes */
    CHECK (walpe_driver_hint_mesa ("shader_cache_size", "1073741824", ev, &none) &&
           strv_has (ev, "MESA_SHADER_CACHE_MAX_SIZE=1024M"));
    g_ptr_array_set_size (ev, 0);
    CHECK (walpe_driver_hint_mesa ("shader_cache_size", "1", ev, &none) &&
           strv_has (ev, "MESA_SHADER_CACHE_MAX_SIZE=1M"));
    g_ptr_array_set_size (ev, 0);
    /* settings only the NVIDIA driver has */
    CHECK (walpe_driver_hint_mesa ("yield", "usleep", ev, &none) && none && ev->len == 0);
    CHECK (walpe_driver_hint_mesa ("max_frames", "2", ev, &none) && none && ev->len == 0);
    CHECK (walpe_driver_hint_mesa ("sync_display", "DP-0", ev, &none) && none && ev->len == 0);
    /* unset values set nothing and are no complaint */
    CHECK (walpe_driver_hint_mesa ("threaded", "default", ev, &none) && !none && ev->len == 0);
    CHECK (walpe_driver_hint_mesa ("yield", "", ev, &none) && !none);
    /* not understood */
    CHECK (!walpe_driver_hint_mesa ("threaded", "maybe", ev, &none));
    CHECK (!walpe_driver_hint_mesa ("shader_cache_size", "lots", ev, &none));
    CHECK (!walpe_driver_hint_mesa ("present", "vsync", ev, &none));
    CHECK (ev->len == 0);
    /* the NVIDIA conversion is what it was */
    CHECK (walpe_driver_hint ("threaded", "on", ev) && strv_has (ev, "__GL_THREADED_OPTIMIZATIONS=1"));
    g_ptr_array_unref (ev);
}

/* The PCI id list serves the names and nothing else: the plain device list
 * does not read it, the naming call reads it once for all devices, and a
 * list of broken or hostile text names nothing and breaks nothing. */
static void test_pci_id_list (void)
{
    char *root = P ("ids-intel-nvidia");
    fakegpu_laptop_intel_nvidia (root);
    char *ids = P ("pci.ids");
    g_assert (g_file_set_contents (ids,
        "# a list for the test\n"
        "8086  Vendor One\n"
        "\t9a49  Panel Graphics [Test]\n"
        "\t\t8086 0001  A subsystem line\n"
        "10de  Vendor Two\n"
        "\t28a0  Not this one\n"
        "\t28a1  Second Card [Test]\n"
        "ffff  Illegal Vendor ID\n"
        "C 03  Display controller\n", -1, NULL));
    g_setenv ("WALPE_GPU_PCI_IDS", ids, TRUE);

    guint r0 = walpe_gpu_pci_ids_reads ();
    GPtrArray *a = walpe_gpu_enumerate_plain (root);
    CHECK (walpe_gpu_pci_ids_reads () == r0);            /* the list was not looked for */
    CHECK (a->len == 2);
    if (a->len == 2) {
        CHECK (!g_strcmp0 (dev (a, 0)->name, "Intel graphics device 8086:9a49"));
        CHECK (!g_strcmp0 (dev (a, 1)->name, "NVIDIA graphics device 10de:28a1"));
        /* everything a decision is made from is there without the names */
        CHECK (walpe_gpu_display_index (a, NULL, NULL) == 0);
        CHECK (walpe_gpu_match (a, "discrete", 0, NULL) == 1);
        CHECK (walpe_gpu_match (a, "10de:28a1", 0, NULL) == 1);
        CHECK (walpe_gpu_match (a, "nvidia", 0, NULL) == 1);
        CHECK (walpe_gpu_match (a, "second card", 0, NULL) == -1);   /* a name of the list: not yet */
        walpe_gpu_name_devices (a);
        CHECK (walpe_gpu_pci_ids_reads () == r0 + 1);    /* once for both devices */
        CHECK (!g_strcmp0 (dev (a, 0)->name, "Intel Panel Graphics [Test]"));
        CHECK (!g_strcmp0 (dev (a, 1)->name, "NVIDIA Second Card [Test]"));
        CHECK (walpe_gpu_match (a, "second card", 0, NULL) == 1);
    }
    g_ptr_array_unref (a);

    /* the full list: the same names, one more reading */
    a = walpe_gpu_enumerate (root);
    CHECK (walpe_gpu_pci_ids_reads () == r0 + 2);
    CHECK (a->len == 2 && !g_strcmp0 (dev (a, 1)->name, "NVIDIA Second Card [Test]"));
    g_ptr_array_unref (a);

    /* a machine without a PCI graphics device: nothing the list could name */
    char *arm = P ("ids-arm");
    fakegpu_arm_board (arm);
    guint r1 = walpe_gpu_pci_ids_reads ();
    a = walpe_gpu_enumerate (arm);
    CHECK (a->len == 2 && walpe_gpu_pci_ids_reads () == r1);
    g_ptr_array_unref (a);
    char *nowhere = P ("ids-no-such-tree");
    a = walpe_gpu_enumerate (nowhere);
    g_free (nowhere);
    CHECK (a->len == 0 && walpe_gpu_pci_ids_reads () == r1);
    walpe_gpu_name_devices (a);
    walpe_gpu_name_devices (NULL);
    CHECK (walpe_gpu_pci_ids_reads () == r1);
    g_ptr_array_unref (a);

    /* "none": no file is looked for; a file that is not there: looked for, no name */
    g_setenv ("WALPE_GPU_PCI_IDS", "none", TRUE);
    a = walpe_gpu_enumerate (root);
    CHECK (walpe_gpu_pci_ids_reads () == r1);
    CHECK (a->len == 2 && !g_strcmp0 (dev (a, 1)->name, "NVIDIA graphics device 10de:28a1"));
    g_ptr_array_unref (a);
    char *gone = P ("no-such-pci.ids");
    g_setenv ("WALPE_GPU_PCI_IDS", gone, TRUE);
    a = walpe_gpu_enumerate (root);
    CHECK (walpe_gpu_pci_ids_reads () == r1 + 1);
    CHECK (a->len == 2 && !g_strcmp0 (dev (a, 1)->name, "NVIDIA graphics device 10de:28a1"));
    g_ptr_array_unref (a);
    g_free (gone);

    /* broken lists: no newline at the end, a device line cut after the id,
     * a line of 100 000 characters, an empty file, the id without a text,
     * bytes that are no text */
    static const char *const broken[] = {
        "10de  Vendor Two\n\t28a1  Last Line Without Newline",
        "10de  Vendor Two\n\t28a1",
        "10de  Vendor Two\n\t28a1 \n\t28a1  \n",
        "",
        "\n\n\t\n\t\t\n10de\n\t28a1\n",
        "10de  V\n\t28a1  \xff\xfe\x01 not text\n",
    };
    for (guint i = 0; i < G_N_ELEMENTS (broken); i++) {
        g_assert (g_file_set_contents (ids, broken[i], -1, NULL));
        g_setenv ("WALPE_GPU_PCI_IDS", ids, TRUE);
        a = walpe_gpu_enumerate (root);
        CHECK (a->len == 2 && dev (a, 1)->name && *dev (a, 1)->name);
        if (a->len == 2) {
            if (i == 0)
                CHECK (!g_strcmp0 (dev (a, 1)->name, "NVIDIA Last Line Without Newline"));
            else if (i < 5)
                CHECK (!g_strcmp0 (dev (a, 1)->name, "NVIDIA graphics device 10de:28a1"));
            /* whatever the list holds, the name can go into a JSON reply */
            CHECK (g_utf8_validate (dev (a, 1)->name, -1, NULL) && !strchr (dev (a, 1)->name, 1));
        }
        g_ptr_array_unref (a);
    }
    {
        GString *big = g_string_new ("10de  Vendor Two\n\t28a0  ");
        for (int i = 0; i < 100000; i++)
            g_string_append_c (big, 'x');
        g_string_append (big, "\n\t28a1  After The Long Line\n");
        g_assert (g_file_set_contents (ids, big->str, -1, NULL));
        g_string_free (big, TRUE);
        a = walpe_gpu_enumerate (root);
        CHECK (a->len == 2 && !g_strcmp0 (dev (a, 1)->name, "NVIDIA After The Long Line"));
        g_ptr_array_unref (a);
    }

    g_setenv ("WALPE_GPU_PCI_IDS", "none", TRUE);
    g_free (ids);
    g_free (arm);
    g_free (root);
}

/* a PCI display device without a DRM driver whose uevent names a
 * device-tree node and no PCI slot: it is known by its directory's name,
 * and the tag made from the node is not left behind (the leak check of a
 * sanitizer build sees it) */
static void test_pci_device_tree_node (void)
{
    char *root = P ("pci-of-node");
    fakegpu_put (root, "bus/pci/devices/0000:09:00.0/class", "0x030000\n");
    fakegpu_put (root, "bus/pci/devices/0000:09:00.0/uevent",
                 "DRIVER=some-driver\nOF_FULLNAME=/soc/pcie@40000000/gpu@0\n");
    GPtrArray *a = walpe_gpu_enumerate (root);
    CHECK (a->len == 1);
    if (a->len == 1)
        CHECK (!g_strcmp0 (dev (a, 0)->pci, "0000:09:00.0") &&
               !g_strcmp0 (dev (a, 0)->path_tag, "pci-0000_09_00_0") && dev (a, 0)->card == -1);
    g_ptr_array_unref (a);
    g_free (root);
}

/* Device trees of broken text: what a strange kernel, a container or a
 * mount of another kind could show where sysfs should be. Whatever is in
 * there, the list is bounded, every text of a device is short valid UTF-8
 * without control characters (it goes into log lines, an environment
 * variable and the status reply), and the ids are 16 bits. */
static guint32 hx = 0x2545f491u;

static guint32 hrand (void)
{
    hx ^= hx << 13;
    hx ^= hx >> 17;
    hx ^= hx << 5;
    return hx;
}

/* a file of the tree, with plain stdio (many files, none worth a sync) */
static void hput (const char *root, const char *rel, const char *txt)
{
    char *p = g_build_filename (root, rel, NULL), *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    FILE *f = fopen (p, "wb");
    g_assert (f);
    fwrite (txt, 1, strlen (txt), f);
    fclose (f);
    g_free (d);
    g_free (p);
}

static char *hvalue (void)
{
    static const char *const words[] = {
        "", "\n", "=", "DRIVER", "DRIVER=", "DRIVER=i915", "DRIVER=nvidia\nPCI_ID=10DE:28A1",
        "PCI_ID=:", "PCI_ID=10de:", "PCI_ID=ffffffffffffffffffffffff:-1", "PCI_ID=zzzz:yyyy",
        "PCI_SLOT_NAME=", "PCI_SLOT_NAME=0000:01:00.0", "PCI_SLOT_NAME=\xff\xfe:01:\x01\x02.0",
        "OF_FULLNAME=/soc/gpu@ff9a0000", "OF_FULLNAME=@", "OF_FULLNAME=/", "OF_COMPATIBLE_0=\xc3\x28,mali",
        "DRIVER=\xe2\x82", "DRIVER=a\tb\x7f" "c", "0x030000", "0x03", "030000", "-1", "1", "0", "connected",
        "connected\n\n", "disconnected", "0x8086", "8086", "0xffffffffffffffffffff", "18446744073709551616",
    };
    GString *g = g_string_new (NULL);
    for (guint n = 1 + hrand () % 4; n; n--) {
        switch (hrand () % 10) {
        case 0:
            g_string_append (g, hrand () & 1 ? "DRIVER=" : "PCI_SLOT_NAME=");
            for (guint k = 1 + hrand () % 70000; k; k--)
                g_string_append_c (g, 'q');
            break;
        case 1:
            for (guint k = 1 + hrand () % 200; k; k--)
                g_string_append_c (g, (char) (1 + hrand () % 255));
            break;
        default:
            g_string_append (g, words[hrand () % G_N_ELEMENTS (words)]);
            break;
        }
        if (hrand () % 4)
            g_string_append_c (g, '\n');
    }
    return g_string_free (g, FALSE);
}

static void hattr (const char *root, const char *rel)
{
    if (hrand () % 5 == 0)
        return;
    char *v = hvalue ();
    hput (root, rel, v);
    g_free (v);
}

static gboolean text_ok (const char *s, gsize max)
{
    if (!s)
        return TRUE;
    if (strlen (s) > max || !g_utf8_validate (s, -1, NULL))
        return FALSE;
    for (; *s; s++)
        if ((guchar) *s < 0x20 || *s == 0x7f)
            return FALSE;
    return TRUE;
}

static void check_hostile_list (GPtrArray *a, const char *what)
{
    gboolean ok = a != NULL;
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_array (b);
    for (guint i = 0; ok && i < a->len; i++) {
        const WalpeGpuDev *d = g_ptr_array_index (a, i);
        /* (a made-up name from another vendor's ids is longer than a driver name: 127 for all) */
        ok = d->name && *d->name && text_ok (d->name, 255) && text_ok (d->driver, 127) &&
             text_ok (d->pci, 127) && text_ok (d->path_tag, 255) && text_ok (d->render_node, 127) &&
             text_ok (d->card_node, 127) && d->vendor <= 0xffff && d->device <= 0xffff && d->connectors;
        for (int c = 0; ok && d->connectors[c]; c++)
            ok = text_ok (d->connectors[c], 127);
        char *txt = walpe_gpu_dev_describe (d);
        ok = ok && g_utf8_validate (txt, -1, NULL);
        g_free (txt);
        json_builder_begin_object (b);
        walpe_gpu_dev_to_json (b, d);
        json_builder_end_object (b);
        if (i < 4) {
            walpe_gpu_in_use_set (d, FALSE);
            WalpeGpuInUse u;
            ok = ok && walpe_gpu_in_use (&u);
            walpe_gpu_in_use_clear (&u);
        }
    }
    json_builder_end_array (b);
    JsonNode *root = json_builder_get_root (b);
    char *js = json_to_string (root, FALSE);
    JsonParser *jp = json_parser_new ();
    ok = ok && json_parser_load_from_data (jp, js, -1, NULL);
    g_object_unref (jp);
    g_free (js);
    json_node_unref (root);
    g_object_unref (b);
    /* the choices on such a list end, with an index inside it or -1 */
    static const char *const outs[] = { "HDMI-A-1", "eDP-1", "DP-99999999999999999999", "", NULL };
    static const char *const values[] = { "auto", "integrated", "discrete", "nvidia", "0000:01:00.0",
                                          "card1", "renderD128", "10de:28a1", "q", "\xc3\xa9" };
    int disp = walpe_gpu_display_index (a, outs, NULL);
    int rend = walpe_gpu_default_renderer (a, disp);
    ok = ok && disp >= -1 && disp < (int) a->len && rend >= -1 && rend < (int) a->len;
    for (guint v = 0; v < G_N_ELEMENTS (values); v++) {
        char *why = NULL;
        int m = walpe_gpu_match (a, values[v], disp, &why);
        ok = ok && m >= -1 && m < (int) a->len && (!why || g_utf8_validate (why, -1, NULL));
        if (m >= 0) {
            char *note = NULL;
            char **env = walpe_gpu_offload_env (disp >= 0 ? g_ptr_array_index (a, disp) : NULL,
                                                g_ptr_array_index (a, m), WALPE_GPU_WS_X11, "/m.json",
                                                NULL, &note);
            for (int e = 0; env[e]; e++)
                ok = ok && text_ok (env[e], 255);
            g_strfreev (env);
            g_free (note);
        }
        g_free (why);
    }
    walpe_gpu_in_use_set (NULL, FALSE);
    if (!ok) {
        fails++;
        g_printerr ("FAIL: hostile device tree %s\n", what);
    }
}

static void test_hostile_sysfs (void)
{
    static const char *const attrs[] = { "uevent", "vendor", "device", "boot_vga", "class" };
    for (int t = 0; t < 50; t++) {
        char name[32], rel[200];
        g_snprintf (name, sizeof name, "hostile-%d", t);
        char *root = P (name);
        guint cards = hrand () % 4;
        for (guint c = 0; c < cards; c++) {
            for (guint i = 0; i < G_N_ELEMENTS (attrs); i++) {
                g_snprintf (rel, sizeof rel, "class/drm/card%u/device/%s", c, attrs[i]);
                hattr (root, rel);
            }
            g_snprintf (rel, sizeof rel, "class/drm/card%u/device/drm/renderD12%u\xff\x01/dev", c, c);
            hattr (root, rel);
            g_snprintf (rel, sizeof rel, "class/drm/card%u-HDMI-A-%u/status", c, hrand () % 3);
            hattr (root, rel);
            g_snprintf (rel, sizeof rel, "class/drm/card%u-\xfe\x02 odd\n/status", c);
            hput (root, rel, hrand () & 1 ? "connected\n" : "connected");
        }
        for (guint k = hrand () % 3; k; k--) {
            g_snprintf (rel, sizeof rel, "bus/pci/devices/%s/class",
                        hrand () & 1 ? "0000:0\xff:00.0" : "0000:09:00.0");
            hput (root, rel, hrand () & 1 ? "0x030000" : "0x030200\n");
            g_snprintf (rel, sizeof rel, "bus/pci/devices/0000:09:00.0/uevent");
            hattr (root, rel);
        }
        GPtrArray *a = walpe_gpu_enumerate (root);
        CHECK (a->len <= cards + 2);
        check_hostile_list (a, name);
        g_ptr_array_unref (a);
        g_free (root);
    }

    /* far more DRM cards than a kernel has, names that only look like
     * cards, and links that lead back into the tree */
    char *root = P ("hostile-many");
    char rel[200];
    for (int c = 0; c < 1100; c++) {
        if (c == 1000 || c == 1001)
            continue;       /* the two links below */
        g_snprintf (rel, sizeof rel, "class/drm/card%d/device/uevent", c);
        hput (root, rel, c % 3 ? "DRIVER=amdgpu\nPCI_ID=1002:73DF"
                                      : "DRIVER=i915\nPCI_ID=8086:9A49\nPCI_SLOT_NAME=0000:00:02.0\n");
        if (c < 40) {
            g_snprintf (rel, sizeof rel, "class/drm/card%d-DP-%d/status", c, c);
            hput (root, rel, "connected\n");
        }
    }
    hput (root, "class/drm/card99999999999999999999/device/uevent", "DRIVER=x\n");
    hput (root, "class/drm/card-1/device/uevent", "DRIVER=x\n");
    hput (root, "class/drm/card7x/device/uevent", "DRIVER=x\n");
    hput (root, "class/drm/card5-/status", "connected\n");
    {
        char *drm = g_build_filename (root, "class", "drm", NULL);
        char *l1 = g_build_filename (drm, "card2000", NULL);            /* outside the range */
        char *l2 = g_build_filename (drm, "card1000", "device", NULL);  /* a link to itself */
        char *l3 = g_build_filename (drm, "card1001", NULL);            /* back to the directory */
        fakegpu_dir (root, "class/drm/card1000");
        CHECK (symlink (".", l1) == 0);
        CHECK (symlink ("device", l2) == 0);
        CHECK (symlink ("..", l3) == 0);
        g_free (l1);
        g_free (l2);
        g_free (l3);
        g_free (drm);
    }
    GPtrArray *a = walpe_gpu_enumerate (root);
    CHECK (a->len >= 2 && a->len <= 1024);
    check_hostile_list (a, "hostile-many");
    g_ptr_array_unref (a);
    g_free (root);
}

int main (int argc, char **argv)
{
    if (argc == 4 && !strcmp (argv[1], "--tree")) {
        /* a made-up sysfs tree for a test that starts a daemon on it
         * (tests/gpu_auto_test.sh) */
        if (fakegpu_machine (argv[2], argv[3]))
            return 0;
        g_printerr ("gpusel-test: no machine \"%s\"\n", argv[2]);
        return 2;
    }
    T = g_dir_make_tmp ("walpe-gpusel-XXXXXX", NULL);
    g_assert (T);
    char *cfg = P ("cfg");
    g_setenv ("XDG_CONFIG_HOME", cfg, TRUE);
    g_free (cfg);
    /* names from the ids, whatever id list this machine has installed */
    g_setenv ("WALPE_GPU_PCI_IDS", "none", TRUE);
    g_unsetenv ("WALPE_GPU_SYSFS_ROOT");

    test_classify ();
    test_laptop_intel_nvidia ();
    test_other_machines ();
    test_outputs_and_values ();
    test_auto ();
    test_run_limited ();
    test_vendor_files_and_probe ();
    test_in_use ();
    test_config ();
    test_mesa_hints ();
    test_pci_id_list ();
    test_pci_device_tree_node ();
    test_hostile_sysfs ();

    char *cmd = g_strdup_printf ("rm -rf '%s'", T);
    if (system (cmd) != 0)
        g_printerr ("cleanup failed\n");
    g_free (cmd);
    if (fails)
        g_printerr ("gpusel-test: %d failure(s)\n", fails);
    else
        g_print ("gpusel-test: all checks passed\n");
    return fails ? 1 : 0;
}
