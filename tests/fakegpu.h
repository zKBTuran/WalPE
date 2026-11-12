/* fakegpu.h - made-up sysfs trees of machines with one, two or no GPUs,
 * for tests of common/gpusel.c (WALPE_GPU_SYSFS_ROOT or the sysroot
 * argument of walpe_gpu_enumerate).
 *
 * The trees are ILLUSTRATIONS written from the documented layout of
 * sysfs (class/drm, bus/pci) and public PCI ids. None of them was
 * captured from a real machine.
 *
 * Regular files and directories only (no symbolic links):
 *   <root>/class/drm/cardN/device/uevent      DRIVER= PCI_ID= PCI_SLOT_NAME=
 *   <root>/class/drm/cardN/device/boot_vga
 *   <root>/class/drm/cardN/device/drm/renderDM/
 *   <root>/class/drm/cardN-<output>/status    connected | disconnected
 *   <root>/bus/pci/devices/<slot>/{class,vendor,device,uevent}
 * Other tests may add their own files below the same root (usage figures
 * of a device live below cardN/device/). */
#pragma once
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static inline void fakegpu_put (const char *root, const char *rel, const char *txt)
{
    char *p = g_build_filename (root, rel, NULL), *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_assert (g_file_set_contents (p, txt, -1, NULL));
    g_free (d);
    g_free (p);
}

static inline void fakegpu_dir (const char *root, const char *rel)
{
    char *p = g_build_filename (root, rel, NULL);
    g_mkdir_with_parents (p, 0700);
    g_free (p);
}

/* a PCI graphics device with a DRM driver. pci_id "8086:9A49" as the
 * kernel prints it; render < 0 = no render node; outputs: "name=status"
 * strings, NULL-terminated, may be NULL */
static inline void fakegpu_card (const char *root, int card, int render, const char *slot,
                                 const char *pci_id, const char *driver, gboolean boot_vga,
                                 const char *const *outputs)
{
    char *rel = g_strdup_printf ("class/drm/card%d/device/uevent", card);
    char *ue = g_strdup_printf ("DRIVER=%s\nPCI_CLASS=30000\nPCI_ID=%s\nPCI_SUBSYS_ID=0000:0000\n"
                                "PCI_SLOT_NAME=%s\nMODALIAS=pci:fake\n", driver, pci_id, slot);
    fakegpu_put (root, rel, ue);
    g_free (ue);
    g_free (rel);
    rel = g_strdup_printf ("class/drm/card%d/device/boot_vga", card);
    fakegpu_put (root, rel, boot_vga ? "1\n" : "0\n");
    g_free (rel);
    rel = g_strdup_printf ("class/drm/card%d/device/drm/card%d", card, card);
    fakegpu_dir (root, rel);
    g_free (rel);
    if (render >= 0) {
        rel = g_strdup_printf ("class/drm/card%d/device/drm/renderD%d", card, render);
        fakegpu_dir (root, rel);
        g_free (rel);
    }
    for (int i = 0; outputs && outputs[i]; i++) {
        char **kv = g_strsplit (outputs[i], "=", 2);
        rel = g_strdup_printf ("class/drm/card%d-%s/status", card, kv[0]);
        char *st = g_strdup_printf ("%s\n", kv[1] ? kv[1] : "connected");
        fakegpu_put (root, rel, st);
        g_free (st);
        g_free (rel);
        g_strfreev (kv);
    }
    /* the same device as the PCI bus lists it */
    char **ids = g_strsplit (pci_id, ":", 2);
    char *v = g_strdup_printf ("0x%s\n", ids[0]), *d = g_strdup_printf ("0x%s\n", ids[1]);
    rel = g_strdup_printf ("bus/pci/devices/%s/class", slot);
    fakegpu_put (root, rel, "0x030000\n");
    g_free (rel);
    rel = g_strdup_printf ("bus/pci/devices/%s/vendor", slot);
    fakegpu_put (root, rel, v);
    g_free (rel);
    rel = g_strdup_printf ("bus/pci/devices/%s/device", slot);
    fakegpu_put (root, rel, d);
    g_free (rel);
    g_free (v);
    g_free (d);
    g_strfreev (ids);
}

/* a PCI display device no DRM driver has claimed (klass "0x030200") */
static inline void fakegpu_pci_only (const char *root, const char *slot, const char *klass,
                                     const char *vendor, const char *device)
{
    char *rel = g_strdup_printf ("bus/pci/devices/%s/class", slot);
    char *t = g_strdup_printf ("%s\n", klass);
    fakegpu_put (root, rel, t);
    g_free (t);
    g_free (rel);
    rel = g_strdup_printf ("bus/pci/devices/%s/vendor", slot);
    t = g_strdup_printf ("0x%s\n", vendor);
    fakegpu_put (root, rel, t);
    g_free (t);
    g_free (rel);
    rel = g_strdup_printf ("bus/pci/devices/%s/device", slot);
    t = g_strdup_printf ("0x%s\n", device);
    fakegpu_put (root, rel, t);
    g_free (t);
    g_free (rel);
    rel = g_strdup_printf ("bus/pci/devices/%s/uevent", slot);
    t = g_strdup_printf ("PCI_CLASS=%s\nPCI_ID=%s:%s\nPCI_SLOT_NAME=%s\n", klass + 2, vendor, device,
                         slot);
    fakegpu_put (root, rel, t);
    g_free (t);
    g_free (rel);
}

/* something on the PCI bus that is not a display device (a network card) */
static inline void fakegpu_pci_other (const char *root, const char *slot)
{
    char *rel = g_strdup_printf ("bus/pci/devices/%s/class", slot);
    fakegpu_put (root, rel, "0x020000\n");
    g_free (rel);
}

/* a device-tree (SoC) graphics device */
static inline void fakegpu_platform (const char *root, int card, int render, const char *driver,
                                     const char *compatible, const char *fullname,
                                     const char *const *outputs)
{
    char *rel = g_strdup_printf ("class/drm/card%d/device/uevent", card);
    char *ue = g_strdup_printf ("DRIVER=%s\nOF_NAME=gpu\nOF_FULLNAME=%s\nOF_COMPATIBLE_0=%s\n"
                                "OF_COMPATIBLE_N=1\nMODALIAS=of:fake\n", driver, fullname, compatible);
    fakegpu_put (root, rel, ue);
    g_free (ue);
    g_free (rel);
    if (render >= 0) {
        rel = g_strdup_printf ("class/drm/card%d/device/drm/renderD%d", card, render);
        fakegpu_dir (root, rel);
        g_free (rel);
    }
    for (int i = 0; outputs && outputs[i]; i++) {
        char **kv = g_strsplit (outputs[i], "=", 2);
        rel = g_strdup_printf ("class/drm/card%d-%s/status", card, kv[0]);
        char *st = g_strdup_printf ("%s\n", kv[1] ? kv[1] : "connected");
        fakegpu_put (root, rel, st);
        g_free (st);
        g_free (rel);
        g_strfreev (kv);
    }
}

/* ---- machines ---------------------------------------------------------- */

/* a laptop: Intel graphics drives the panel, an NVIDIA GPU (NVIDIA's
 * driver, with its DRM module) beside it; an external HDMI port wired to
 * the NVIDIA GPU with nothing plugged in */
static inline void fakegpu_laptop_intel_nvidia (const char *root)
{
    static const char *const o0[] = { "eDP-1=connected", "DP-1=disconnected", NULL };
    static const char *const o1[] = { "HDMI-A-1=disconnected", NULL };
    fakegpu_card (root, 0, 128, "0000:00:02.0", "8086:9A49", "i915", TRUE, o0);
    fakegpu_card (root, 1, 129, "0000:01:00.0", "10DE:28A1", "nvidia", FALSE, o1);
    fakegpu_pci_other (root, "0000:02:00.0");
}

/* a laptop with an AMD processor's graphics and an AMD card, both amdgpu */
static inline void fakegpu_laptop_amd_amd (const char *root)
{
    static const char *const o0[] = { "eDP-1=connected", "HDMI-A-1=disconnected", NULL };
    fakegpu_card (root, 0, 128, "0000:04:00.0", "1002:1638", "amdgpu", TRUE, o0);
    fakegpu_card (root, 1, 129, "0000:01:00.0", "1002:73FF", "amdgpu", FALSE, NULL);
}

/* a desktop with one NVIDIA card and two monitors */
static inline void fakegpu_desktop_nvidia (const char *root)
{
    static const char *const o0[] = { "DP-1=connected", "HDMI-A-1=connected", "DP-2=disconnected",
                                      NULL };
    fakegpu_card (root, 0, 128, "0000:01:00.0", "10DE:2503", "nvidia", TRUE, o0);
}

/* a desktop: the processor's Intel graphics enabled but unused, an Intel
 * Arc card with the monitor */
static inline void fakegpu_desktop_igpu_arc (const char *root)
{
    static const char *const o1[] = { "DP-1=connected", "HDMI-A-1=disconnected", NULL };
    static const char *const o0[] = { "HDMI-A-2=disconnected", NULL };
    fakegpu_card (root, 0, 128, "0000:00:02.0", "8086:4680", "i915", FALSE, o0);
    fakegpu_card (root, 1, 129, "0000:03:00.0", "8086:56A0", "i915", TRUE, o1);
}

/* a desktop with two cards, a monitor on each: an AMD card (primary) and
 * an NVIDIA card */
static inline void fakegpu_desktop_two_cards (const char *root)
{
    static const char *const o0[] = { "DP-1=connected", NULL };
    static const char *const o1[] = { "HDMI-A-1=connected", NULL };
    fakegpu_card (root, 0, 128, "0000:03:00.0", "1002:747E", "amdgpu", TRUE, o0);
    fakegpu_card (root, 1, 129, "0000:04:00.0", "10DE:2503", "nvidia", FALSE, o1);
}

/* a virtual machine */
static inline void fakegpu_vm (const char *root)
{
    static const char *const o0[] = { "Virtual-1=connected", NULL };
    fakegpu_card (root, 0, 128, "0000:00:01.0", "1AF4:1050", "virtio-pci", TRUE, o0);
}

/* an ARM board: the display controller and the GPU are two devices */
static inline void fakegpu_arm_board (const char *root)
{
    static const char *const o0[] = { "HDMI-A-1=connected", NULL };
    fakegpu_platform (root, 0, -1, "rockchip-drm", "rockchip,display-subsystem",
                      "/display-subsystem", o0);
    fakegpu_platform (root, 1, 128, "panfrost", "arm,mali-t860", "/gpu@ff9a0000", NULL);
}

/* Intel graphics with its driver, and an NVIDIA GPU nothing drives (no
 * driver installed): only the PCI bus lists the second one */
static inline void fakegpu_laptop_nvidia_undriven (const char *root)
{
    static const char *const o0[] = { "eDP-1=connected", NULL };
    fakegpu_card (root, 0, 128, "0000:00:02.0", "8086:9A49", "i915", TRUE, o0);
    fakegpu_pci_only (root, "0000:01:00.0", "0x030200", "10de", "28a1");
}

/* a laptop with three: Intel graphics drives the panel, an NVIDIA GPU
 * inside (NVIDIA's driver), and an AMD card in an external enclosure */
static inline void fakegpu_laptop_two_discrete (const char *root)
{
    static const char *const o0[] = { "eDP-1=connected", NULL };
    static const char *const o2[] = { "DP-3=disconnected", NULL };
    fakegpu_card (root, 0, 128, "0000:00:02.0", "8086:A7A0", "i915", TRUE, o0);
    fakegpu_card (root, 1, 129, "0000:01:00.0", "10DE:28A1", "nvidia", FALSE, NULL);
    fakegpu_card (root, 2, 130, "0000:3c:00.0", "1002:7480", "amdgpu", FALSE, o2);
}

/* Intel graphics drives the panel; beside it a PCI display device of a
 * vendor no rule knows, with a driver and a render node */
static inline void fakegpu_laptop_unknown_card (const char *root)
{
    static const char *const o0[] = { "eDP-1=connected", NULL };
    fakegpu_card (root, 0, 128, "0000:00:02.0", "8086:9A49", "i915", TRUE, o0);
    fakegpu_card (root, 1, 129, "0000:01:00.0", "1ED5:0201", "somegpu", FALSE, NULL);
}

/* a machine by name, for a test that starts a daemon on such a tree
 * (gpusel-test --tree <machine> <dir>); FALSE = no such machine */
static inline gboolean fakegpu_machine (const char *name, const char *root)
{
    static const struct { const char *name; void (*make) (const char *); } m[] = {
        { "laptop-intel-nvidia", fakegpu_laptop_intel_nvidia },
        { "laptop-amd-amd", fakegpu_laptop_amd_amd },
        { "laptop-nvidia-undriven", fakegpu_laptop_nvidia_undriven },
        { "laptop-two-discrete", fakegpu_laptop_two_discrete },
        { "desktop-nvidia", fakegpu_desktop_nvidia },
        { "desktop-igpu-arc", fakegpu_desktop_igpu_arc },
        { "desktop-two-cards", fakegpu_desktop_two_cards },
        { "vm", fakegpu_vm },
    };
    for (guint i = 0; i < G_N_ELEMENTS (m); i++)
        if (!strcmp (m[i].name, name)) {
            m[i].make (root);
            return TRUE;
        }
    return FALSE;
}
