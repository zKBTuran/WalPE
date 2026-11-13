/* gpusel.c - the machine's graphics devices and the choice among them,
 * without EGL / X11: see gpusel.h */
#include "common/gpusel.h"
#include "common/util.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ------------------------------------------------------------- helpers */

const char *walpe_gpu_sysfs_root (void)
{
    const char *e = g_getenv ("WALPE_GPU_SYSFS_ROOT");
    return e && *e ? e : "/sys";
}

/* a small sysfs file, trimmed (g_free); NULL when it cannot be read */
static char *read_trim (const char *dir, const char *name)
{
    char *p = name ? g_build_filename (dir, name, NULL) : g_strdup (dir);
    char *txt = NULL;
    gsize len = 0;
    gboolean ok = g_file_get_contents (p, &txt, &len, NULL);
    g_free (p);
    if (!ok)
        return NULL;
    if (len > 65536) {          /* not a sysfs attribute */
        g_free (txt);
        return NULL;
    }
    return g_strstrip (txt);
}

/* Text that came from sysfs or from the PCI id list, for a log line, an
 * environment variable and a JSON reply (takes s; g_free): at most `max`
 * bytes, valid UTF-8, no control characters. The kernel's own names are
 * short ASCII words and pass unchanged; a file of another kind (a strange
 * mount, a broken list) must not make the status reply unreadable. */
static char *sys_text (char *s, gsize max)
{
    if (!s)
        return NULL;
    if (strlen (s) > max)
        s[max] = 0;
    if (!g_utf8_validate (s, -1, NULL)) {
        char *v = g_utf8_make_valid (s, -1);
        g_free (s);
        s = v;
    }
    for (char *p = s; *p; p++)
        if ((guchar) *p < 0x20 || *p == 0x7f)
            *p = '?';
    return s;
}

/* VALUE of the line "KEY=VALUE" in a uevent text (g_free) or NULL */
static char *uevent_get (const char *text, const char *key)
{
    if (!text)
        return NULL;
    gsize kl = strlen (key);
    for (const char *p = text; p && *p;) {
        const char *eol = strchr (p, '\n');
        gsize n = eol ? (gsize) (eol - p) : strlen (p);
        if (n > kl && !strncmp (p, key, kl) && p[kl] == '=')
            return g_strstrip (g_strndup (p + kl + 1, n - kl - 1));
        p = eol ? eol + 1 : NULL;
    }
    return NULL;
}

static gboolean contains_ci (const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle)
        return FALSE;
    char *h = g_ascii_strdown (hay, -1), *n = g_ascii_strdown (needle, -1);
    gboolean r = strstr (h, n) != NULL;
    g_free (h);
    g_free (n);
    return r;
}

static int cmp_int (gconstpointer a, gconstpointer b)
{
    return *(const int *) a - *(const int *) b;
}

static int cmp_strptr (gconstpointer a, gconstpointer b)
{
    return strcmp (*(const char *const *) a, *(const char *const *) b);
}

void walpe_gpu_dev_free (WalpeGpuDev *d)
{
    if (!d)
        return;
    g_free (d->card_node);
    g_free (d->render_node);
    g_free (d->pci);
    g_free (d->path_tag);
    g_free (d->sysfs);
    g_free (d->driver);
    g_free (d->name);
    g_strfreev (d->connectors);
    g_free (d);
}

/* ------------------------------------------------------ classification */

const char *walpe_gpu_class_name (WalpeGpuClass c)
{
    switch (c) {
    case WALPE_GPU_INTEGRATED: return "integrated";
    case WALPE_GPU_DISCRETE:   return "discrete";
    case WALPE_GPU_VIRTUAL:    return "virtual";
    default:                   return "unknown";
    }
}

const char *walpe_gpu_vendor_word (guint vendor)
{
    switch (vendor) {
    case WALPE_PCI_INTEL:  return "Intel";
    case WALPE_PCI_AMD:    return "AMD";
    case WALPE_PCI_NVIDIA: return "NVIDIA";
    case 0x1af4: return "virtio";
    case 0x15ad: return "VMware";
    case 0x1234: return "QEMU";
    case 0x1b36: return "QXL";
    case 0x80ee: return "VirtualBox";
    case 0x1414: return "Hyper-V";
    case 0x1a03: return "ASPEED";
    case 0x102b: return "Matrox";
    case 0x5143: return "Qualcomm";
    case 0x13b5: return "ARM";
    case 0x14e4: return "Broadcom";
    case 0x106b: return "Apple";
    default: return NULL;
    }
}

gboolean walpe_gpu_driver_is_nvidia (const char *driver)
{
    /* "nvidia" is the PCI driver of NVIDIA's own kernel module;
     * "nvidia-drm" is how its DRM side names itself */
    return driver && (!strcmp (driver, "nvidia") || !strcmp (driver, "nvidia-drm") ||
                      !strcmp (driver, "nvidia_drm"));
}

gboolean walpe_gpu_renderer_is_vendor (const char *renderer, guint vendor)
{
    const char *w = walpe_gpu_vendor_word (vendor);
    return renderer && w && contains_ci (renderer, w);
}

/* The PCI device ids of AMD's processors with graphics (APUs), as ranges.
 * From the public PCI id list; a processor newer than this table is
 * classified by the rule after it (a card, marked as guessed). */
static const struct { guint16 lo, hi; } amd_apu_ids[] = {
    { 0x1304, 0x131d },     /* Kaveri */
    { 0x9640, 0x964f },     /* Sumo, SuperSumo (Llano) */
    { 0x9802, 0x980a },     /* Wrestler */
    { 0x9830, 0x983f },     /* Kabini, Temash */
    { 0x9850, 0x985f },     /* Mullins */
    { 0x9870, 0x9877 },     /* Carrizo, Wani */
    { 0x98e4, 0x98e4 },     /* Stoney */
    { 0x9900, 0x991f },     /* Trinity, Richland */
    { 0x9990, 0x99a4 },     /* Trinity 2, Richland */
    { 0x15dd, 0x15dd },     /* Raven Ridge */
    { 0x15d8, 0x15d8 },     /* Picasso, Raven 2 */
    { 0x1636, 0x1636 },     /* Renoir */
    { 0x1638, 0x1638 },     /* Cezanne */
    { 0x164c, 0x164c },     /* Lucienne */
    { 0x15e7, 0x15e7 },     /* Barcelo */
    { 0x163f, 0x163f },     /* Van Gogh */
    { 0x1435, 0x1435 },     /* Van Gogh (second revision) */
    { 0x1681, 0x1681 },     /* Rembrandt */
    { 0x164d, 0x164d },     /* Rembrandt */
    { 0x1506, 0x1506 },     /* Mendocino */
    { 0x164e, 0x164e },     /* Raphael */
    { 0x164f, 0x164f },     /* Phoenix */
    { 0x15bf, 0x15bf },     /* Phoenix */
    { 0x15c8, 0x15c8 },     /* Phoenix 2 */
    { 0x1900, 0x1901 },     /* Hawk Point */
    { 0x150e, 0x150e },     /* Strix Point */
    { 0x1586, 0x1586 },     /* Strix Halo */
    { 0x1114, 0x1114 },     /* Krackan Point */
    { 0x13c0, 0x13c0 },     /* Granite Ridge */
};

static gboolean amd_is_apu (guint device)
{
    for (guint i = 0; i < G_N_ELEMENTS (amd_apu_ids); i++)
        if (device >= amd_apu_ids[i].lo && device <= amd_apu_ids[i].hi)
            return TRUE;
    return FALSE;
}

/* adapters that are not a GPU of this machine: virtual machines, the
 * firmware framebuffer, test drivers, server management chips */
static gboolean driver_is_virtual (const char *driver)
{
    static const char *const names[] = {
        "virtio-pci", "virtio_gpu", "virtio-gpu", "vmwgfx", "bochs-drm", "bochs", "qxl",
        "vboxvideo", "hyperv_drm", "cirrus", "cirrus-qemu", "simpledrm", "simple-framebuffer",
        "vkms", "vgem", "ast", "mgag200", "efi-framebuffer", "xen-drm-front", NULL
    };
    for (int i = 0; driver && names[i]; i++)
        if (!strcmp (driver, names[i]))
            return TRUE;
    return FALSE;
}

static gboolean vendor_is_virtual (guint vendor)
{
    return vendor == 0x1af4 || vendor == 0x15ad || vendor == 0x1234 || vendor == 0x1b36 ||
           vendor == 0x80ee || vendor == 0x1414 || vendor == 0x1a03 || vendor == 0x102b;
}

WalpeGpuClass walpe_gpu_classify (guint vendor, guint device, const char *driver,
                                  const char *pci, gboolean *guessed)
{
    if (guessed)
        *guessed = FALSE;
    if (driver_is_virtual (driver) || vendor_is_virtual (vendor))
        return WALPE_GPU_VIRTUAL;
    if (vendor == WALPE_PCI_INTEL) {
        /* the processor's graphics has been device 2 of bus 0 since the
         * first integrated chipsets; Intel's cards sit behind a bridge */
        if (pci) {
            const char *bdf = strchr (pci, ':');
            return bdf && !g_ascii_strcasecmp (bdf + 1, "00:02.0") ? WALPE_GPU_INTEGRATED
                                                                  : WALPE_GPU_DISCRETE;
        }
        return WALPE_GPU_INTEGRATED;
    }
    if (vendor == WALPE_PCI_NVIDIA) {
        /* (a Tegra's GPU is a platform device and has no PCI vendor id) */
        return WALPE_GPU_DISCRETE;
    }
    if (vendor == WALPE_PCI_AMD) {
        if (amd_is_apu (device))
            return WALPE_GPU_INTEGRATED;
        /* a card - or an APU newer than the table above */
        if (guessed)
            *guessed = TRUE;
        return WALPE_GPU_DISCRETE;
    }
    if (!pci || !*pci) {
        /* not on the PCI bus: the GPU of a system-on-chip */
        if (!driver || !*driver) {
            if (guessed)
                *guessed = TRUE;
            return WALPE_GPU_UNKNOWN;
        }
        return WALPE_GPU_INTEGRATED;
    }
    if (guessed)
        *guessed = TRUE;
    return WALPE_GPU_DISCRETE;
}

const char *walpe_gpu_class_preset (WalpeGpuClass klass, gboolean software)
{
    if (software)
        return "low";
    switch (klass) {
    case WALPE_GPU_DISCRETE:   return "high";
    case WALPE_GPU_INTEGRATED: return "balanced";
    case WALPE_GPU_VIRTUAL:    return "low";
    default:                   return NULL;
    }
}

char *walpe_gpu_pci_path_tag (const char *pci)
{
    if (!pci || !*pci)
        return NULL;
    char *t = g_strconcat ("pci-", pci, NULL);
    for (char *p = t + 4; *p; p++)
        if (*p == ':' || *p == '.')
            *p = '_';
        else
            *p = g_ascii_tolower (*p);
    return t;
}

/* ------------------------------------------------------------- names */

/* how often this process has looked for the PCI id list (tests) */
static gint pci_ids_reads;

guint walpe_gpu_pci_ids_reads (void)
{
    return (guint) g_atomic_int_get (&pci_ids_reads);
}

/* The text of the PCI id list (g_free) or NULL. The list is the one
 * pciutils / hwdata install; $WALPE_GPU_PCI_IDS names another file
 * (tests), or "none". */
static char *pci_ids_load (void)
{
    static const char *const places[] = { "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
                                          "/usr/share/pci.ids", NULL };
    const char *e = g_getenv ("WALPE_GPU_PCI_IDS");
    char *txt = NULL;
    gsize len = 0;
    if (e && !strcmp (e, "none"))
        return NULL;
    g_atomic_int_inc (&pci_ids_reads);
    if (e && *e) {
        if (!g_file_get_contents (e, &txt, &len, NULL))
            return NULL;
    } else {
        for (int i = 0; places[i] && !txt; i++)
            if (!g_file_get_contents (places[i], &txt, &len, NULL))
                txt = NULL;
    }
    if (!txt)
        return NULL;
    if (len > (8u << 20)) {     /* the real list is 1.5 MB */
        g_free (txt);
        return NULL;
    }
    return txt;
}

/* the device's text in the PCI id list `txt` (g_free) or NULL */
static char *pci_ids_find (const char *txt, guint vendor, guint device)
{
    char vkey[8], dkey[8];
    g_snprintf (vkey, sizeof vkey, "%04x", vendor & 0xffffu);
    g_snprintf (dkey, sizeof dkey, "%04x", device & 0xffffu);
    char *found = NULL;
    gboolean in_vendor = FALSE;
    for (const char *p = txt; p && *p && !found;) {
        const char *eol = strchr (p, '\n');
        gsize n = eol ? (gsize) (eol - p) : strlen (p);
        if (n >= 6 && p[0] != '#') {
            if (p[0] != '\t') {
                if (in_vendor)
                    break;      /* the next vendor: the device is not listed */
                in_vendor = !g_ascii_strncasecmp (p, vkey, 4) && p[4] == ' ';
            } else if (in_vendor && p[1] != '\t' && n >= 7 &&
                       !g_ascii_strncasecmp (p + 1, dkey, 4) && p[5] == ' ') {
                found = g_strstrip (g_strndup (p + 6, n - 6));
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    if (found && !*found) {
        g_free (found);
        found = NULL;
    }
    return found;
}

/* the name of a device. ids_text: its text in the PCI id list, NULL = not
 * looked up or not listed (the name is then made from the ids) */
static char *make_name (guint vendor, guint device, const char *driver, const char *of_compat,
                        const char *ids_text)
{
    const char *vw = walpe_gpu_vendor_word (vendor);
    if (vendor)
        return ids_text ? g_strdup_printf ("%s%s%s", vw ? vw : "", vw ? " " : "", ids_text)
                        : g_strdup_printf ("%s graphics device %04x:%04x", vw ? vw : "PCI", vendor,
                                           device);
    if (of_compat && *of_compat)
        return g_strdup (of_compat);
    if (driver && *driver)
        return g_strdup_printf ("%s device", driver);
    return g_strdup ("graphics device");
}

void walpe_gpu_name_devices (GPtrArray *devs)
{
    gboolean any = FALSE;
    for (guint i = 0; devs && i < devs->len; i++)
        any |= ((const WalpeGpuDev *) g_ptr_array_index (devs, i))->vendor != 0;
    if (!any)
        return;     /* no device the list could name: it is not read */
    char *txt = pci_ids_load ();
    if (!txt)
        return;
    for (guint i = 0; i < devs->len; i++) {
        WalpeGpuDev *d = g_ptr_array_index (devs, i);
        char *t = d->vendor ? pci_ids_find (txt, d->vendor, d->device) : NULL;
        if (t) {
            g_free (d->name);
            d->name = sys_text (make_name (d->vendor, d->device, d->driver, NULL, t), 127);
            g_free (t);
        }
    }
    g_free (txt);
}

/* ------------------------------------------------------- enumeration */

static guint hex_field (const char *s)
{
    return s ? (guint) g_ascii_strtoull (s, NULL, 16) : 0;
}

/* fills the identity of a device from its sysfs directory */
static void dev_identify (WalpeGpuDev *d, const char *devdir)
{
    char *ue = read_trim (devdir, "uevent");
    char *drv = uevent_get (ue, "DRIVER");
    char *slot = uevent_get (ue, "PCI_SLOT_NAME");
    char *id = uevent_get (ue, "PCI_ID");
    char *ofc = uevent_get (ue, "OF_COMPATIBLE_0");
    char *offn = uevent_get (ue, "OF_FULLNAME");
    if (id && strchr (id, ':')) {
        d->vendor = hex_field (id);
        d->device = hex_field (strchr (id, ':') + 1);
    } else {
        char *v = read_trim (devdir, "vendor"), *dv = read_trim (devdir, "device");
        d->vendor = hex_field (v);
        d->device = hex_field (dv);
        g_free (v);
        g_free (dv);
    }
    d->vendor &= 0xffffu;       /* PCI ids are 16 bits, whatever the text says */
    d->device &= 0xffffu;
    drv = sys_text (drv, 63);
    slot = sys_text (slot, 63);
    ofc = sys_text (ofc, 127);
    offn = sys_text (offn, 127);
    if (!drv) {
        /* no uevent line: the name the "driver" link points to */
        char *dl = g_build_filename (devdir, "driver", NULL);
        char *tgt = g_file_read_link (dl, NULL);
        if (tgt)
            drv = sys_text (g_path_get_basename (tgt), 63);
        g_free (tgt);
        g_free (dl);
    }
    d->driver = drv ? drv : g_strdup ("");
    if (slot && *slot) {
        d->pci = g_ascii_strdown (slot, -1);
        d->path_tag = walpe_gpu_pci_path_tag (d->pci);
    } else if (offn && *offn) {
        /* a device-tree node "/soc/gpu@ff9a0000" -> "platform-ff9a0000_gpu" */
        const char *base = strrchr (offn, '/');
        base = base ? base + 1 : offn;
        const char *at = strchr (base, '@');
        char *t = at ? g_strdup_printf ("platform-%s_%.*s", at + 1, (int) (at - base), base)
                     : g_strdup_printf ("platform-%s", base);
        for (char *p = t + 9; *p; p++)
            if (!g_ascii_isalnum (*p))
                *p = '_';
        d->path_tag = t;
    }
    g_free (slot);
    char *bv = read_trim (devdir, "boot_vga");
    d->boot_vga = bv && bv[0] == '1';
    g_free (bv);
    d->sysfs = g_strdup (devdir);
    d->klass = walpe_gpu_classify (d->vendor, d->device, d->driver, d->pci, &d->class_guessed);
    d->name = make_name (d->vendor, d->device, d->driver, ofc, NULL);
    g_free (id);
    g_free (ofc);
    g_free (offn);
    g_free (ue);
}

GPtrArray *walpe_gpu_enumerate (const char *sysroot)
{
    GPtrArray *devs = walpe_gpu_enumerate_plain (sysroot);
    walpe_gpu_name_devices (devs);
    return devs;
}

GPtrArray *walpe_gpu_enumerate_plain (const char *sysroot)
{
    if (!sysroot || !*sysroot)
        sysroot = walpe_gpu_sysfs_root ();
    GPtrArray *devs = g_ptr_array_new_with_free_func ((GDestroyNotify) walpe_gpu_dev_free);
    char *drm = g_build_filename (sysroot, "class", "drm", NULL);
    GArray *cards = g_array_new (FALSE, FALSE, sizeof (int));
    GPtrArray *outs = g_ptr_array_new_with_free_func (g_free);     /* "cardN-NAME" */
    GDir *dir = g_dir_open (drm, 0, NULL);
    const char *n;
    while (dir && (n = g_dir_read_name (dir))) {
        if (!g_str_has_prefix (n, "card") || !g_ascii_isdigit (n[4]))
            continue;
        char *end = NULL;
        long num = strtol (n + 4, &end, 10);
        if (num < 0 || num > 1023)
            continue;
        if (!*end) {
            int c = (int) num;
            g_array_append_val (cards, c);
        } else if (*end == '-' && end[1]) {
            g_ptr_array_add (outs, g_strdup (n));
        }
    }
    if (dir)
        g_dir_close (dir);
    g_array_sort (cards, cmp_int);
    g_ptr_array_sort (outs, cmp_strptr);

    for (guint i = 0; i < cards->len; i++) {
        int c = g_array_index (cards, int, i);
        char *cn = g_strdup_printf ("card%d", c);
        char *devdir = g_build_filename (drm, cn, "device", NULL);
        WalpeGpuDev *d = g_new0 (WalpeGpuDev, 1);
        d->card = c;
        d->card_node = g_strdup_printf ("/dev/dri/card%d", c);
        dev_identify (d, devdir);
        /* its render node: the renderD* entry next to the card */
        char *sub = g_build_filename (devdir, "drm", NULL);
        GDir *sd = g_dir_open (sub, 0, NULL);
        const char *sn;
        while (sd && (sn = g_dir_read_name (sd)))
            if (g_str_has_prefix (sn, "renderD") && g_ascii_isdigit (sn[7]) && !d->render_node)
                d->render_node = sys_text (g_strdup_printf ("/dev/dri/%s", sn), 63);
        if (sd)
            g_dir_close (sd);
        g_free (sub);
        /* an SoC's display controller: it shows pictures and renders
         * none (no render node); the GPU is another device */
        if (!d->pci && !d->render_node && d->klass == WALPE_GPU_INTEGRATED)
            d->klass = WALPE_GPU_VIRTUAL;
        /* the outputs with a display attached */
        GPtrArray *conn = g_ptr_array_new ();
        gsize pl = strlen (cn);
        for (guint k = 0; k < outs->len; k++) {
            const char *on = g_ptr_array_index (outs, k);
            if (strncmp (on, cn, pl) || on[pl] != '-')
                continue;
            char *od = g_build_filename (drm, on, NULL);
            char *st = read_trim (od, "status");
            if (st && !strcmp (st, "connected"))
                g_ptr_array_add (conn, sys_text (g_strdup (on + pl + 1), 63));
            g_free (st);
            g_free (od);
        }
        d->n_connected = (int) conn->len;
        g_ptr_array_add (conn, NULL);
        d->connectors = (char **) g_ptr_array_free (conn, FALSE);
        /* one device is listed once, whatever number of cards names it */
        gboolean dup = FALSE;
        for (guint k = 0; k < devs->len && d->pci; k++) {
            WalpeGpuDev *o = g_ptr_array_index (devs, k);
            dup |= o->pci && !strcmp (o->pci, d->pci);
        }
        if (dup)
            walpe_gpu_dev_free (d);
        else
            g_ptr_array_add (devs, d);
        g_free (devdir);
        g_free (cn);
    }
    g_array_free (cards, TRUE);
    g_ptr_array_unref (outs);
    g_free (drm);

    /* display devices on the PCI bus that no DRM driver has claimed (a
     * card whose driver is not loaded, NVIDIA's driver without its DRM
     * module): listed, so that the status can name them */
    char *pcidir = g_build_filename (sysroot, "bus", "pci", "devices", NULL);
    GPtrArray *slots = g_ptr_array_new_with_free_func (g_free);
    dir = g_dir_open (pcidir, 0, NULL);
    while (dir && (n = g_dir_read_name (dir)))
        g_ptr_array_add (slots, g_strdup (n));
    if (dir)
        g_dir_close (dir);
    g_ptr_array_sort (slots, cmp_strptr);
    for (guint i = 0; i < slots->len; i++) {
        const char *slot = g_ptr_array_index (slots, i);
        char *devdir = g_build_filename (pcidir, slot, NULL);
        char *cls = read_trim (devdir, "class");
        /* class 0x03xxxx: display controller (VGA, 3D, other) */
        gboolean display = cls && (hex_field (cls) >> 16) == 0x03;
        g_free (cls);
        gboolean known = FALSE;
        for (guint k = 0; k < devs->len && display; k++) {
            WalpeGpuDev *o = g_ptr_array_index (devs, k);
            known |= o->pci && !g_ascii_strcasecmp (o->pci, slot);
        }
        if (display && !known) {
            WalpeGpuDev *d = g_new0 (WalpeGpuDev, 1);
            d->card = -1;
            dev_identify (d, devdir);
            if (!d->pci) {
                d->pci = sys_text (g_ascii_strdown (slot, -1), 63);
                g_free (d->path_tag);   /* (one made from a device-tree node name) */
                d->path_tag = walpe_gpu_pci_path_tag (d->pci);
                d->klass = walpe_gpu_classify (d->vendor, d->device, d->driver, d->pci,
                                               &d->class_guessed);
            }
            d->connectors = g_new0 (char *, 1);
            g_ptr_array_add (devs, d);
        }
        g_free (devdir);
    }
    g_ptr_array_unref (slots);
    g_free (pcidir);
    return devs;
}

/* -------------------------------------------------------------- outputs */

/* an output name -> its connector family ("hdmi", "dp", "edp", ...) and
 * its last number (-1 = none). "HDMI-A-1", "HDMI1", "HDMI-1-2",
 * "DisplayPort-0", "eDP" all parse. */
static void output_parse (const char *name, char fam[16], int *num)
{
    fam[0] = 0;
    *num = -1;
    if (!name)
        return;
    gsize k = 0;
    const char *p = name;
    for (; *p && g_ascii_isalpha (*p) && k < 15; p++)
        fam[k++] = g_ascii_tolower (*p);
    fam[k] = 0;
    if (!strcmp (fam, "displayport"))
        strcpy (fam, "dp");
    /* the last run of digits is the number */
    const char *last = NULL;
    for (const char *q = p; *q; q++)
        if (g_ascii_isdigit (*q) && (q == name || !g_ascii_isdigit (q[-1])))
            last = q;
    if (last)
        *num = atoi (last);
}

/* 0 = different connector families; 1 = the same family; 2 = and the
 * numbers are one apart (drivers that count from 0); 3 = and equal */
static int output_score (const char *sysfs_name, const char *name)
{
    char fa[16], fb[16];
    int na, nb;
    output_parse (sysfs_name, fa, &na);
    output_parse (name, fb, &nb);
    if (!fa[0] || strcmp (fa, fb))
        return 0;
    if (na < 0 || nb < 0)
        return 1;
    if (na == nb)
        return 3;
    return abs (na - nb) == 1 ? 2 : 1;
}

gboolean walpe_gpu_output_matches (const char *sysfs_name, const char *name)
{
    return output_score (sysfs_name, name) > 0;
}

int walpe_gpu_index_of_node (GPtrArray *devs, const char *node)
{
    if (!devs || !node || !*node)
        return -1;
    const char *base = strrchr (node, '/');
    base = base ? base + 1 : node;
    for (guint i = 0; i < devs->len; i++) {
        WalpeGpuDev *d = g_ptr_array_index (devs, i);
        const char *cb = d->card_node ? strrchr (d->card_node, '/') : NULL;
        const char *rb = d->render_node ? strrchr (d->render_node, '/') : NULL;
        if ((cb && !strcmp (cb + 1, base)) || (rb && !strcmp (rb + 1, base)))
            return (int) i;
    }
    return -1;
}

int walpe_gpu_display_index (GPtrArray *devs, const char *const *outputs,
                             const char *display_node)
{
    if (!devs || !devs->len)
        return -1;
    int by_node = walpe_gpu_index_of_node (devs, display_node);
    if (by_node >= 0)
        return by_node;
    /* the device whose connected outputs carry the wallpapers' outputs */
    if (outputs && outputs[0]) {
        int best = -1, best_score = 0;
        gboolean tie = FALSE;
        for (guint i = 0; i < devs->len; i++) {
            WalpeGpuDev *d = g_ptr_array_index (devs, i);
            int score = 0;
            for (int o = 0; outputs[o]; o++) {
                int s = 0;
                for (int c = 0; d->connectors && d->connectors[c]; c++)
                    s = MAX (s, output_score (d->connectors[c], outputs[o]));
                score += s;
            }
            if (score > best_score) {
                best = (int) i;
                best_score = score;
                tie = FALSE;
            } else if (score == best_score && score > 0) {
                tie = TRUE;
            }
        }
        if (best >= 0 && !tie)
            return best;
    }
    int with_display = -1, n_with = 0, boot = -1, real = -1, n_real = 0;
    for (guint i = 0; i < devs->len; i++) {
        WalpeGpuDev *d = g_ptr_array_index (devs, i);
        if (d->n_connected > 0) {
            with_display = (int) i;
            n_with++;
        }
        if (d->boot_vga && boot < 0)
            boot = (int) i;
        if (d->card >= 0) {
            real = (int) i;
            n_real++;
        }
    }
    if (n_with == 1)
        return with_display;
    if (boot >= 0)
        return boot;
    if (devs->len == 1)
        return 0;
    if (n_real == 1)
        return real;
    return -1;
}

int walpe_gpu_default_renderer (GPtrArray *devs, int display)
{
    if (!devs || !devs->len)
        return -1;
    if (display >= 0 && (guint) display < devs->len) {
        const WalpeGpuDev *d = g_ptr_array_index (devs, display);
        if (d->render_node)
            return display;
    }
    int only = -1, n = 0;
    for (guint i = 0; i < devs->len; i++) {
        const WalpeGpuDev *d = g_ptr_array_index (devs, i);
        if (d->render_node) {
            only = (int) i;
            n++;
        }
    }
    if (n == 1)
        return only;
    return display >= 0 && (guint) display < devs->len ? display : -1;
}

/* ---------------------------------------------------------------- match */

gboolean walpe_gpu_value_is_word (const char *value)
{
    if (!value)
        return TRUE;
    char *v = g_ascii_strdown (value, -1);
    g_strstrip (v);
    gboolean w = !*v || !strcmp (v, "auto") || !strcmp (v, "display") ||
                 !strcmp (v, "integrated") || !strcmp (v, "discrete");
    g_free (v);
    return w;
}

gboolean walpe_gpu_value_is_display (const char *value)
{
    if (!value)
        return FALSE;
    char *v = g_ascii_strdown (value, -1);
    g_strstrip (v);
    gboolean d = !strcmp (v, "display");
    g_free (v);
    return d;
}

int walpe_gpu_auto_target (GPtrArray *devs, int display, char **why)
{
    if (why)
        *why = NULL;
    if (!devs || display < 0 || (guint) display >= devs->len)
        return -1;
    const WalpeGpuDev *dd = g_ptr_array_index (devs, display);
    if (dd->klass != WALPE_GPU_INTEGRATED)
        return -1;
    /* A device no kernel driver is bound to renders nothing. Where the list
     * has such a one as the displays' GPU - the processor's graphics of a
     * laptop, the firmware's primary device, left without a driver, while
     * the card's driver runs without its DRM module - the window system
     * does not draw with it, and which GPU it uses is not known from here:
     * nothing is tried and nothing is said. ("NVIDIA ... has a driver but
     * no DRM device: rendering stays on Intel ..." stood in the log and in
     * status there, about a machine on which the NVIDIA card renders.) */
    if (!dd->driver || !*dd->driver)
        return -1;
    int first = -1, boot = -1, unusable = -1, n_unusable = 0;
    for (guint i = 0; i < devs->len; i++) {
        const WalpeGpuDev *d = g_ptr_array_index (devs, i);
        if ((int) i == display || d->klass != WALPE_GPU_DISCRETE ||
            (d->class_guessed && d->vendor != WALPE_PCI_AMD))
            continue;
        if (!d->driver || !*d->driver || d->card < 0 || !d->render_node) {
            if (unusable < 0)
                unusable = (int) i;
            n_unusable++;
            continue;
        }
        if (first < 0)
            first = (int) i;
        if (d->boot_vga && boot < 0)
            boot = (int) i;
    }
    if (first < 0 && unusable >= 0 && why) {
        const WalpeGpuDev *u = g_ptr_array_index (devs, unusable);
        const char *what = !u->driver || !*u->driver ? "has no kernel driver loaded"
                           : u->card < 0             ? "has a driver but no DRM device"
                                                     : "has no render node";
        *why = g_strdup_printf ("%s %s%s: rendering stays on %s", u->name, what,
                                n_unusable > 1 ? " (and no other discrete GPU can be used)" : "",
                                dd->name);
    }
    return boot >= 0 ? boot : first;
}

char *walpe_gpu_value_normalize (const char *value)
{
    if (!value)
        return g_strdup ("auto");
    if (strlen (value) > 127)
        return NULL;
    /* the value is shown in `status` (JSON): bytes that are not UTF-8 would
     * make the whole reply unreadable for every client */
    if (!g_utf8_validate (value, -1, NULL))
        return NULL;
    for (const char *p = value; *p; p++)
        if ((guchar) *p < 0x20 || *p == 0x7f)
            return NULL;
    char *v = g_strstrip (g_strdup (value));
    if (!*v) {
        g_free (v);
        return g_strdup ("auto");
    }
    if (walpe_gpu_value_is_word (v)) {
        char *l = g_ascii_strdown (v, -1);
        g_free (v);
        return l;
    }
    return v;
}

static char *found_list (GPtrArray *devs)
{
    GString *s = g_string_new (NULL);
    for (guint i = 0; devs && i < devs->len; i++) {
        WalpeGpuDev *d = g_ptr_array_index (devs, i);
        g_string_append_printf (s, "%s%s (%s%s%s)", s->len ? "; " : "", d->name,
                                walpe_gpu_class_name (d->klass), d->pci ? ", " : "",
                                d->pci ? d->pci : "");
    }
    if (!s->len)
        g_string_append (s, "no graphics device in sysfs");
    return g_string_free (s, FALSE);
}

/* does `text` (lower case) name the PCI address of d? "0000:01:00.0",
 * "01:00.0", "pci-0000_01_00_0", "pci:0000:01:00.0" */
static gboolean names_pci (const WalpeGpuDev *d, const char *text)
{
    if (!d->pci)
        return FALSE;
    /* (strncmp, not GLib's g_str_has_prefix macro: its branch for a NULL
     * string made the link-time optimiser warn about every strcmp below) */
    if (!strncmp (text, "pci:", 4))
        text += 4;
    if (!strcmp (text, d->pci))
        return TRUE;
    if (d->path_tag && !strcmp (text, d->path_tag))
        return TRUE;
    const char *bdf = strchr (d->pci, ':');
    return bdf && !strcmp (text, bdf + 1);
}

static gboolean names_node (const WalpeGpuDev *d, const char *text)
{
    const char *base = strrchr (text, '/');
    base = base ? base + 1 : text;
    const char *cb = d->card_node ? strrchr (d->card_node, '/') : NULL;
    const char *rb = d->render_node ? strrchr (d->render_node, '/') : NULL;
    return (cb && !g_ascii_strcasecmp (cb + 1, base)) || (rb && !g_ascii_strcasecmp (rb + 1, base));
}

/* "10de:25a0" */
static gboolean names_ids (const WalpeGpuDev *d, const char *text)
{
    if (strlen (text) != 9 || text[4] != ':' || !d->vendor)
        return FALSE;
    for (int i = 0; i < 9; i++)
        if (i != 4 && !g_ascii_isxdigit (text[i]))
            return FALSE;
    return hex_field (text) == d->vendor && hex_field (text + 5) == d->device;
}

int walpe_gpu_match (GPtrArray *devs, const char *value, int prefer, char **why)
{
    if (why)
        *why = NULL;
    char *v = walpe_gpu_value_normalize (value);
    if (!v || !strcmp (v, "auto") || !strcmp (v, "display")) {
        g_free (v);
        return -1;
    }
    int found = -1;
    if (!strcmp (v, "integrated") || !strcmp (v, "discrete")) {
        WalpeGpuClass want = v[0] == 'i' ? WALPE_GPU_INTEGRATED : WALPE_GPU_DISCRETE;
        int first = -1, boot = -1;
        gboolean pref_ok = FALSE;
        for (guint i = 0; devs && i < devs->len; i++) {
            WalpeGpuDev *d = g_ptr_array_index (devs, i);
            if (d->klass != want)
                continue;
            if (first < 0)
                first = (int) i;
            if (d->boot_vga && boot < 0)
                boot = (int) i;
            pref_ok |= (int) i == prefer;
        }
        found = pref_ok ? prefer : boot >= 0 ? boot : first;
        if (found < 0 && why) {
            char *list = found_list (devs);
            *why = g_strdup_printf ("this machine has no %s GPU (found: %s)", v, list);
            g_free (list);
        }
        g_free (v);
        return found;
    }
    char *l = g_ascii_strdown (v, -1);
    /* an exact identity first, then a part of the name */
    for (guint i = 0; devs && i < devs->len && found < 0; i++) {
        WalpeGpuDev *d = g_ptr_array_index (devs, i);
        if (names_pci (d, l) || names_node (d, l) || names_ids (d, l))
            found = (int) i;
    }
    if (found < 0) {
        int first = -1;
        gboolean pref_ok = FALSE;
        for (guint i = 0; devs && i < devs->len; i++) {
            WalpeGpuDev *d = g_ptr_array_index (devs, i);
            const char *vw = walpe_gpu_vendor_word (d->vendor);
            gboolean hit = contains_ci (d->name, l) || (d->driver && !strcmp (d->driver, l)) ||
                           (vw && !g_ascii_strcasecmp (vw, l));
            if (!hit)
                continue;
            if (first < 0)
                first = (int) i;
            pref_ok |= (int) i == prefer;
        }
        /* a word that fits several devices ("intel" with the processor's
         * graphics and an Intel card): the display device when it is one
         * of them, else the first */
        found = pref_ok ? prefer : first;
    }
    if (found < 0 && why) {
        char *list = found_list (devs);
        *why = g_strdup_printf ("no graphics device matches \"%s\" (found: %s)", v, list);
        g_free (list);
    }
    g_free (l);
    g_free (v);
    return found;
}

/* ---------------------------------------------------------- environment */

static const char *const offload_names[] = {
    "DRI_PRIME", "__NV_PRIME_RENDER_OFFLOAD", "__NV_PRIME_RENDER_OFFLOAD_PROVIDER",
    "__GLX_VENDOR_LIBRARY_NAME", "__EGL_VENDOR_LIBRARY_FILENAMES", NULL
};

const char *const *walpe_gpu_offload_env_names (void)
{
    return offload_names;
}

char **walpe_gpu_offload_env (const WalpeGpuDev *display, const WalpeGpuDev *target,
                              WalpeGpuWinsys winsys, const char *mesa_vendor_json,
                              const char *nvidia_vendor_json, char **note)
{
    GPtrArray *a = g_ptr_array_new ();
    if (note)
        *note = NULL;
#define PAIR(k, v) (g_ptr_array_add (a, g_strdup (k)), g_ptr_array_add (a, g_strdup (v)))
    if (target && target != display) {
        gboolean t_nv = walpe_gpu_driver_is_nvidia (target->driver);
        gboolean d_nv = display && walpe_gpu_driver_is_nvidia (display->driver);
        if (!target->driver || !*target->driver) {
            if (note)
                *note = g_strdup_printf ("%s has no kernel driver loaded: it cannot render",
                                         target->name);
        } else if (target->card < 0) {
            /* a driver is bound, but no DRM device belongs to it (NVIDIA's
             * driver without its nvidia-drm module): it may well render,
             * only nothing here can send the rendering to it */
            if (note)
                *note = g_strdup_printf ("%s has a driver (%s) but no DRM device: rendering "
                                         "cannot be directed to it from here", target->name,
                                         target->driver);
        } else if (t_nv) {
            /* NVIDIA's driver renders for a screen another GPU drives */
            PAIR ("__NV_PRIME_RENDER_OFFLOAD", "1");
            PAIR ("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
            if (nvidia_vendor_json)
                PAIR ("__EGL_VENDOR_LIBRARY_FILENAMES", nvidia_vendor_json);
            if (winsys == WALPE_GPU_WS_WAYLAND && !nvidia_vendor_json && note)
                *note = g_strdup ("NVIDIA's EGL vendor file was not found: under Wayland the "
                                  "compositor's GPU may render all the same");
        } else {
            /* a Mesa driver: its loader opens the device with this tag
             * instead of the one the window system names */
            if (target->path_tag) {
                PAIR ("DRI_PRIME", target->path_tag);
            } else if (note) {
                *note = g_strdup_printf ("%s has no bus address Mesa could be pointed to",
                                         target->name);
            }
            if (d_nv && target->path_tag) {
                /* the default GL vendor of this desktop is NVIDIA's:
                 * Mesa has to be the one libglvnd loads */
                if (mesa_vendor_json) {
                    PAIR ("__EGL_VENDOR_LIBRARY_FILENAMES", mesa_vendor_json);
                    PAIR ("__GLX_VENDOR_LIBRARY_NAME", "mesa");
                } else if (note) {
                    *note = g_strdup ("Mesa's EGL vendor file was not found: NVIDIA's driver "
                                      "answers first and renders");
                }
            }
        }
    }
#undef PAIR
    g_ptr_array_add (a, NULL);
    return (char **) g_ptr_array_free (a, FALSE);
}

char *walpe_gpu_find_egl_vendor (const char *dirs, const char *word)
{
    if (!word || !*word)
        return NULL;
    if (!dirs || !*dirs)
        dirs = g_getenv ("__EGL_VENDOR_LIBRARY_DIRS");
    if (!dirs || !*dirs)
        dirs = "/etc/glvnd/egl_vendor.d:/usr/share/glvnd/egl_vendor.d";
    char **dv = g_strsplit (dirs, ":", -1);
    char *found = NULL;
    for (int i = 0; dv[i] && !found; i++) {
        if (!*dv[i])
            continue;
        GDir *d = g_dir_open (dv[i], 0, NULL);
        if (!d)
            continue;
        GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
        const char *n;
        while ((n = g_dir_read_name (d)))
            if (g_str_has_suffix (n, ".json"))
                g_ptr_array_add (names, g_strdup (n));
        g_dir_close (d);
        g_ptr_array_sort (names, cmp_strptr);
        for (guint k = 0; k < names->len && !found; k++) {
            char *p = g_build_filename (dv[i], names->pdata[k], NULL);
            JsonNode *root = wpj_parse_file (p);
            gboolean hit = FALSE;
            if (root && JSON_NODE_HOLDS_OBJECT (root)) {
                JsonObject *icd = wpj_obj (json_node_get_object (root), "ICD");
                const char *lib = icd ? wpj_str (icd, "library_path", NULL) : NULL;
                if (lib) {
                    char *base = g_path_get_basename (lib);
                    hit = contains_ci (base, word);
                    g_free (base);
                }
            }
            if (root)
                json_node_unref (root);
            if (hit)
                found = p;
            else
                g_free (p);
        }
        g_ptr_array_free (names, TRUE);
    }
    g_strfreev (dv);
    return found;
}

/* ------------------------------------------------- a program with a limit */

/* When the kernel started process pid (/proc/<pid>/stat, field 22), which
 * together with the number says which process it is: a number is given
 * out again, a start time with it is not. FALSE: there is no such process
 * any more, or only what is left of one that has ended (state Z). */
static gboolean proc_started (pid_t pid, guint64 *start)
{
    char path[48], *txt = NULL;
    g_snprintf (path, sizeof path, "/proc/%d/stat", (int) pid);
    if (!g_file_get_contents (path, &txt, NULL, NULL))
        return FALSE;
    /* "pid (comm) state ppid ...": comm may hold spaces and ')' */
    const char *rp = strrchr (txt, ')');
    char state = 0;
    unsigned long long st = 0;
    gboolean ok = rp && sscanf (rp + 1, " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u "
                                        "%*d %*d %*d %*d %*d %*d %llu", &state, &st) == 2 &&
                  state != 'Z' && state != 'X';
    g_free (txt);
    *start = st;
    return ok;
}

/* takes what is in the pipes now; a pipe at its end is closed (-1).
 * TRUE: one of them is still open */
static gboolean run_drain (int fd[2], GString *got[2])
{
    gboolean open_any = FALSE;
    for (int i = 0; i < 2; i++) {
        char buf[4096];
        while (fd[i] >= 0) {
            ssize_t n = read (fd[i], buf, sizeof buf);
            if (n > 0) {
                if (got[i]->len < 64 * 1024)
                    g_string_append_len (got[i], buf, MIN ((gsize) n, 64 * 1024 - got[i]->len));
            } else if (n == 0) {
                close (fd[i]);
                fd[i] = -1;
            } else if (errno != EINTR) {
                break;      /* EAGAIN: nothing now */
            }
        }
        open_any |= fd[i] >= 0;
    }
    return open_any;
}

gboolean walpe_gpu_run_limited (char **argv, char **envp, guint limit_ms, char **out,
                                char **errout, gboolean *timed_out, GError **error)
{
    GPid pid = 0;
    int fd[2] = { -1, -1 };
    *out = *errout = NULL;
    *timed_out = FALSE;
    const gint64 until = g_get_monotonic_time () + (gint64) limit_ms * 1000;
    /* (no G_SPAWN_DO_NOT_REAP_CHILD: GLib starts the program from a process
     * that ends at once and reaps that one itself - the program is nobody's
     * to wait for here) */
    if (!g_spawn_async_with_pipes (NULL, argv, envp, G_SPAWN_DEFAULT, NULL, NULL, &pid, NULL,
                                   &fd[0], &fd[1], error))
        return FALSE;
    /* which process that is, for as long as it lives: its number alone may
     * be another process's by the time a signal is sent */
    guint64 start = 0, now_start = 0;
    /* (without /proc nothing tells: then only the end of the streams and
     * the clock end the wait, and no signal is sent) */
    const gboolean have_proc = proc_started (getpid (), &now_start);
    const gboolean known = have_proc && proc_started (pid, &start);
    gboolean gone = have_proc && !known;    /* ended already */
    GString *got[2] = { g_string_new (NULL), g_string_new (NULL) };
    for (int i = 0; i < 2; i++)
        fcntl (fd[i], F_SETFL, fcntl (fd[i], F_GETFL) | O_NONBLOCK);
    gint64 looked = g_get_monotonic_time ();
    for (;;) {
        /* both streams at their end: all there is. Or the program is gone
         * and something else holds a stream: what the program wrote was in
         * the pipes before it ended, and this pass came after that */
        if (!run_drain (fd, got) || gone)
            break;
        gint64 now = g_get_monotonic_time ();
        if (now >= until) {
            *timed_out = TRUE;
            if (known && proc_started (pid, &now_start) && now_start == start)
                kill (pid, SIGKILL);
            break;      /* not waited for: it may not end for that either */
        }
        struct pollfd pfd[2] = { { fd[0], POLLIN, 0 }, { fd[1], POLLIN, 0 } };
        /* (a negative descriptor is skipped by poll) */
        int r = poll (pfd, 2, (int) MIN ((until - now) / 1000 + 1, 50));
        now = g_get_monotonic_time ();
        if (have_proc && (r == 0 || now - looked >= 50 * 1000)) {
            looked = now;
            gone = !(proc_started (pid, &now_start) && now_start == start);
        }
    }
    for (int i = 0; i < 2; i++)
        if (fd[i] >= 0)
            close (fd[i]);
    g_spawn_close_pid (pid);
    *out = g_string_free (got[0], FALSE);
    *errout = g_string_free (got[1], FALSE);
    return TRUE;
}

/* a GL_RENDERER of a CPU rasteriser (the words Mesa and others use) */
static gboolean renderer_is_software (const char *r)
{
    return contains_ci (r, "llvmpipe") || contains_ci (r, "softpipe") ||
           contains_ci (r, "swrast") || contains_ci (r, "software rasterizer") ||
           contains_ci (r, "swiftshader");
}

gboolean walpe_gpu_parse_probe (const char *out, char **device, char **render_node,
                                char **renderer, char **why)
{
    char *dev = NULL, *rn = NULL, *r = NULL, *fail = NULL;
    gboolean drew = FALSE;
    char **lines = g_strsplit (out ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *eq = strchr (lines[i], '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *k = lines[i], *val = g_strstrip (eq + 1);
        if (!strcmp (k, "device") && !dev)
            dev = g_strdup (val);
        else if (!strcmp (k, "render_node") && !rn)
            rn = g_strdup (val);
        else if (!strcmp (k, "renderer") && !r)
            r = g_strdup (val);
        else if (!strcmp (k, "fail") && !fail)
            fail = g_strdup (val);
        else if (!strcmp (k, "drew"))
            drew = !strcmp (val, "1");
    }
    g_strfreev (lines);
    char *w = NULL;
    gboolean ok = FALSE;
    if (fail)
        w = g_strdup (fail);
    else if (!r)
        w = g_strdup ("the test ended without a result (the driver ended the test process)");
    else if (renderer_is_software (r))
        w = g_strdup_printf ("the driver fell back to a software renderer (%s)", r);
    else if (!drew)
        w = g_strdup ("the GPU could not draw to a window of this display");
    else
        ok = TRUE;
    g_free (fail);
    if (device) *device = dev; else g_free (dev);
    if (render_node) *render_node = rn; else g_free (rn);
    if (renderer) *renderer = r; else g_free (r);
    if (why) *why = w; else g_free (w);
    return ok;
}

/* ----------------------------------------------------------------- JSON */

static void jb_s (JsonBuilder *b, const char *k, const char *v)
{
    if (!v)
        return;
    json_builder_set_member_name (b, k);
    json_builder_add_string_value (b, v);
}

static void jb_ids (JsonBuilder *b, guint vendor, guint device)
{
    if (!vendor)
        return;
    char buf[8];
    g_snprintf (buf, sizeof buf, "%04x", vendor & 0xffffu);
    jb_s (b, "vendor_id", buf);
    g_snprintf (buf, sizeof buf, "%04x", device & 0xffffu);
    jb_s (b, "device_id", buf);
}

void walpe_gpu_dev_to_json (JsonBuilder *b, const WalpeGpuDev *d)
{
    jb_s (b, "name", d->name);
    jb_s (b, "pci", d->pci);
    jb_s (b, "driver", d->driver ? d->driver : "");
    jb_s (b, "class", walpe_gpu_class_name (d->klass));
    if (d->class_guessed) {
        json_builder_set_member_name (b, "class_guessed");
        json_builder_add_boolean_value (b, TRUE);
    }
    jb_ids (b, d->vendor, d->device);
    jb_s (b, "card_node", d->card_node);
    jb_s (b, "render_node", d->render_node);
    json_builder_set_member_name (b, "boot_vga");
    json_builder_add_boolean_value (b, d->boot_vga);
    json_builder_set_member_name (b, "connectors");
    json_builder_begin_array (b);
    for (int i = 0; d->connectors && d->connectors[i]; i++)
        json_builder_add_string_value (b, d->connectors[i]);
    json_builder_end_array (b);
}

char *walpe_gpu_dev_describe (const WalpeGpuDev *d)
{
    GString *s = g_string_new (d->name ? d->name : "graphics device");
    if (d->vendor)
        g_string_append_printf (s, " [%04x:%04x]", d->vendor, d->device);
    if (d->pci)
        g_string_append_printf (s, " at %s", d->pci);
    g_string_append_printf (s, ", %s%s", walpe_gpu_class_name (d->klass),
                            d->class_guessed ? " (assumed)" : "");
    if (d->driver && *d->driver)
        g_string_append_printf (s, ", driver %s", d->driver);
    else
        g_string_append (s, ", no driver loaded");
    if (d->render_node)
        g_string_append_printf (s, ", %s", d->render_node);
    if (d->n_connected > 0) {
        char *c = g_strjoinv (" ", d->connectors);
        g_string_append_printf (s, ", outputs in use: %s", c);
        g_free (c);
    }
    if (d->boot_vga)
        g_string_append (s, ", primary");
    return g_string_free (s, FALSE);
}

/* ---------------------------------------------------- the GPU in use */

static GMutex in_use_lock;
static WalpeGpuInUse in_use;
static gboolean in_use_known;

void walpe_gpu_in_use_clear (WalpeGpuInUse *u)
{
    if (!u)
        return;
    g_free (u->render_node);
    g_free (u->card_node);
    g_free (u->pci);
    g_free (u->driver);
    g_free (u->sysfs);
    g_free (u->name);
    memset (u, 0, sizeof *u);
}

void walpe_gpu_in_use_set (const WalpeGpuDev *dev, gboolean software)
{
    g_mutex_lock (&in_use_lock);
    walpe_gpu_in_use_clear (&in_use);
    in_use_known = dev != NULL || software;
    if (software) {
        in_use.software = TRUE;
        in_use.driver = g_strdup ("");
        in_use.name = g_strdup ("software renderer (CPU)");
    } else if (dev) {
        in_use.render_node = g_strdup (dev->render_node);
        in_use.card_node = g_strdup (dev->card_node);
        in_use.pci = g_strdup (dev->pci);
        in_use.driver = g_strdup (dev->driver ? dev->driver : "");
        in_use.sysfs = g_strdup (dev->sysfs);
        in_use.vendor = dev->vendor;
        in_use.device = dev->device;
        in_use.klass = dev->klass;
        in_use.name = g_strdup (dev->name);
    }
    g_mutex_unlock (&in_use_lock);
}

gboolean walpe_gpu_in_use (WalpeGpuInUse *out)
{
    if (!out)
        return FALSE;
    memset (out, 0, sizeof *out);
    g_mutex_lock (&in_use_lock);
    gboolean known = in_use_known;
    if (known) {
        out->render_node = g_strdup (in_use.render_node);
        out->card_node = g_strdup (in_use.card_node);
        out->pci = g_strdup (in_use.pci);
        out->driver = g_strdup (in_use.driver ? in_use.driver : "");
        out->sysfs = g_strdup (in_use.sysfs);
        out->vendor = in_use.vendor;
        out->device = in_use.device;
        out->klass = in_use.klass;
        out->name = g_strdup (in_use.name);
        out->software = in_use.software;
    }
    g_mutex_unlock (&in_use_lock);
    return known;
}

void walpe_gpu_in_use_to_json (JsonBuilder *b, const WalpeGpuInUse *u)
{
    jb_s (b, "name", u->name);
    jb_s (b, "pci", u->pci);
    jb_s (b, "driver", u->driver ? u->driver : "");
    jb_s (b, "class", u->software ? "software" : walpe_gpu_class_name (u->klass));
    jb_ids (b, u->vendor, u->device);
    jb_s (b, "card_node", u->card_node);
    jb_s (b, "render_node", u->render_node);
    jb_s (b, "sysfs", u->sysfs);
    json_builder_set_member_name (b, "software");
    json_builder_add_boolean_value (b, u->software);
}
