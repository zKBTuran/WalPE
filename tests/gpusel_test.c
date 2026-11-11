/* gpusel_test.c - which GPU renders (src/common/gpusel.c) without a GPU,
 * a display or EGL: the device list read from made-up sysfs trees
 * (tests/fakegpu.h: laptops with two GPUs, desktops, a virtual machine,
 * an ARM board), the integrated / discrete classification, the values of
 * the `gpu` setting matched against the lists, the display device, the
 * environment for rendering on another GPU, the probe's report, the
 * record of the GPU in use, the config keys and the Mesa equivalents of
 * gpu.conf's driver hints.
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
#include <stdlib.h>
#include <string.h>

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

int main (void)
{
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
    test_vendor_files_and_probe ();
    test_in_use ();
    test_config ();
    test_mesa_hints ();

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
