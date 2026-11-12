/* gpusel.h - which GPU renders: the graphics devices of this machine, what
 * kind each one is, and what has to be set so that the wallpapers are
 * drawn by the one the user asked for.
 *
 * Setting `gpu` (config.json "gpu", `walpe-ctl set gpu=...`, WALPE_GPU in
 * the environment, which wins):
 *
 *   auto        (default) a discrete GPU when the machine has one. Where
 *               the window system's GPU is the only one or is discrete
 *               already - every desktop with one card - that is `display`:
 *               nothing is set, nothing is tested. Where an integrated GPU
 *               drives the displays and a discrete one with a driver sits
 *               beside it (a laptop with two GPUs) that one renders, as
 *               with `discrete` - after the same test, and with `display`
 *               as what is done when the test fails
 *               (walpe_gpu_auto_target).
 *   display     nothing is set. EGL is started on the connection to the
 *               window system, and the driver that answers there renders
 *               on the GPU the X server / the compositor itself uses - the
 *               one that drives the displays. Frames go to the screen
 *               without a copy between GPUs. What `auto` meant up to
 *               0.14.0.
 *   integrated  the GPU that is part of the processor (Intel graphics, AMD
 *               APU graphics, an ARM SoC's GPU): lowest power draw; shares
 *               the machine's memory.
 *   discrete    the separate graphics card / the laptop's second GPU.
 *   <text>      one device by PCI address ("0000:01:00.0", "01:00.0"), by
 *               DRM node ("card1", "/dev/dri/renderD129"), by vendor:device
 *               ("10de:25a0") or by a part of its name or driver ("nvidia",
 *               "radeon", "arc").
 *
 * When the device asked for is not the one that drives the displays, the
 * driver renders there and copies every finished frame to the display GPU
 * (PRIME render offload): one full-screen copy per monitor and frame
 * through system memory or a shared buffer, and the second GPU stays
 * powered. That is the cost of every value except `display` on a laptop
 * with two GPUs; on a machine with one GPU every value means the same
 * device.
 *
 * How a device is reached (nothing but environment variables, which the
 * drivers read when EGL starts - so they are set before the first EGL
 * call and a change of the setting restarts the daemon in place):
 *   a Mesa driver (i915 / xe / amdgpu / radeon / nouveau / ...):
 *       DRI_PRIME=pci-0000_01_00_0 (the device's path tag), and when the
 *       default GL vendor is not Mesa also __EGL_VENDOR_LIBRARY_FILENAMES
 *       = Mesa's vendor file and __GLX_VENDOR_LIBRARY_NAME=mesa;
 *   the NVIDIA driver:
 *       __NV_PRIME_RENDER_OFFLOAD=1, __GLX_VENDOR_LIBRARY_NAME=nvidia and,
 *       when its vendor file is found, __EGL_VENDOR_LIBRARY_FILENAMES.
 * A variable of these that the user has set is never replaced: the
 * setting is then not applied and the status note says so.
 *
 * This file is the part without EGL / X11: the device list (read from
 * sysfs), the classification, the choice and the environment. The daemon
 * verifies a plan in a child process before it adopts it
 * (daemon/gpu_glue.h). */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include "common/api.h"

G_BEGIN_DECLS

typedef enum {
    WALPE_GPU_UNKNOWN = 0,
    WALPE_GPU_INTEGRATED,
    WALPE_GPU_DISCRETE,
    WALPE_GPU_VIRTUAL,       /* not a GPU of this machine to render on: a virtual
                              * machine's adapter, a server's management chip, an
                              * SoC's display controller */
} WalpeGpuClass;

typedef enum {
    WALPE_GPU_WS_X11 = 0,
    WALPE_GPU_WS_WAYLAND,
} WalpeGpuWinsys;

#define WALPE_PCI_INTEL  0x8086
#define WALPE_PCI_AMD    0x1002
#define WALPE_PCI_NVIDIA 0x10de

typedef struct {
    int card;                /* N of /sys/class/drm/cardN, -1 = no DRM device */
    char *card_node;         /* "/dev/dri/cardN" or NULL */
    char *render_node;       /* "/dev/dri/renderDN" or NULL */
    char *pci;               /* "0000:01:00.0", NULL for a platform device */
    char *path_tag;          /* "pci-0000_01_00_0" / "platform-..." (DRI_PRIME form) or NULL */
    char *sysfs;             /* the device's sysfs directory ("/sys/class/drm/card0/device") */
    guint vendor, device;    /* PCI ids, 0 = none */
    char *driver;            /* kernel driver ("i915", "amdgpu", "nvidia", ...), "" unknown */
    char *name;              /* for people: "Intel TigerLake-LP GT2 [Iris Xe Graphics]" */
    WalpeGpuClass klass;
    gboolean class_guessed;  /* no rule knew this device: the class is an assumption */
    gboolean boot_vga;       /* the firmware's primary display adapter */
    char **connectors;       /* names of its outputs with a display attached, NULL-terminated */
    int n_connected;
} WalpeGpuDev;

void walpe_gpu_dev_free (WalpeGpuDev *d);

/* Where sysfs is: $WALPE_GPU_SYSFS_ROOT (tests: a made-up tree), else "/sys" */
const char *walpe_gpu_sysfs_root (void);

/* The graphics devices of the machine, in the order of their DRM card
 * numbers, then PCI display devices no DRM driver has claimed (card = -1).
 * sysroot NULL = walpe_gpu_sysfs_root (). Reads class/drm/cardN/device/
 * uevent, vendor, device, boot_vga, class/drm/cardN/device/drm/renderD*,
 * class/drm/cardN-<output>/status and bus/pci/devices/<slot>/. Never fails:
 * an empty array on a machine where none of it exists. Free with
 * g_ptr_array_unref (element free func set). */
GPtrArray *walpe_gpu_enumerate (const char *sysroot);
/* The same list without the PCI id list (1.5 MB of text that serves the
 * names and nothing else): a PCI device is named by its vendor and ids
 * ("NVIDIA graphics device 10de:25a0"). For a process that decides and
 * shows no name - the daemon when it starts with gpu = auto. */
GPtrArray *walpe_gpu_enumerate_plain (const char *sysroot);
/* Gives the PCI devices of a list their names from the PCI id list, which
 * is read once for all of them; a device it does not have keeps its name.
 * Reads nothing when the list holds no PCI device. */
void walpe_gpu_name_devices (GPtrArray *devs);
/* how often this process has looked for the PCI id list so far (tests) */
guint walpe_gpu_pci_ids_reads (void);

/* integrated / discrete / virtual from the ids, the driver and the bus
 * position. *guessed (optional): no rule applied. The rules:
 *   virtual adapters and display-only chips by driver or vendor;
 *   Intel: the device at 00:02.0 is the processor's, any other is a card;
 *   NVIDIA: discrete;
 *   AMD: the APU device ids are integrated, every other id is a card;
 *   a device that is not on the PCI bus (an SoC's GPU) is integrated;
 *   anything else: discrete, guessed. */
WalpeGpuClass walpe_gpu_classify (guint vendor, guint device, const char *driver,
                                  const char *pci, gboolean *guessed);
const char *walpe_gpu_class_name (WalpeGpuClass c);   /* "integrated", ... */
/* "Intel", "AMD", "NVIDIA", ... or NULL for an id this file has no word for */
const char *walpe_gpu_vendor_word (guint vendor);
/* the driver is NVIDIA's own (not nouveau) */
gboolean walpe_gpu_driver_is_nvidia (const char *driver);
/* a GL_RENDERER string carries the word of that PCI vendor ("NVIDIA GeForce
 * RTX 4050/PCIe/SSE2": NVIDIA; "Mesa Intel(R) Graphics (ADL GT2)": Intel).
 * FALSE for a vendor without a word and for NULL. For the one case in
 * which EGL draws on a GPU and does not say on which device. */
gboolean walpe_gpu_renderer_is_vendor (const char *renderer, guint vendor);

/* "0000:01:00.0" -> "pci-0000_01_00_0" (g_free); NULL in -> NULL */
char *walpe_gpu_pci_path_tag (const char *pci);

/* The device a setting value names: its index in devs, -1 = none.
 * "auto", "display", "" and NULL name none (for "auto" see
 * walpe_gpu_auto_target). For "integrated" / "discrete": the
 * device of that class; among several the one at index `prefer` (the
 * display device) when it is of the class, else the firmware's primary
 * one, else the first. *why (optional, g_free): the sentence when the
 * value names nothing here. */
int walpe_gpu_match (GPtrArray *devs, const char *value, int prefer, char **why);
/* the value is one of the words (auto / display / integrated / discrete) */
gboolean walpe_gpu_value_is_word (const char *value);
/* the value names no device and none is looked for: "display" (NULL, ""
 * and "auto" are not that: `auto` may choose) */
gboolean walpe_gpu_value_is_display (const char *value);
/* The device `auto` renders on when that is not the window system's own
 * GPU: its index in devs, -1 = the window system's GPU renders and nothing
 * is set. `display`: the device that renders for the window system when
 * nothing is set (walpe_gpu_default_renderer; -1 = not known).
 *   not known, or not an integrated GPU (a card, a virtual adapter): -1;
 *   an integrated GPU, and no discrete device beside it: -1;
 *   an integrated GPU and discrete devices: the first of them that can be
 *   rendered on from here - a kernel driver is bound, it has a DRM device
 *   and a render node - the firmware's primary one before the others,
 *   then by DRM card number. A device whose class is a guess counts only
 *   when it is AMD's (a Radeon card is "discrete (assumed)"; beside an
 *   integrated GPU that is what it is).
 * *why (optional, g_free): set only when discrete devices are there and
 * none of them can be used - the sentence that says which and why. NULL in
 * every case in which there is nothing to say. Reads nothing. */
int walpe_gpu_auto_target (GPtrArray *devs, int display, char **why);
/* the setting as it is stored: lower case and trimmed for the words,
 * the text as given otherwise; "" / NULL -> "auto". NULL = not acceptable
 * (longer than 127 bytes, control characters, not UTF-8). g_free */
char *walpe_gpu_value_normalize (const char *value);

/* The device that drives the displays, as far as it can be told without
 * asking the window system: the device `display_node` names (a /dev/dri
 * path) when given; else the one whose connected outputs match `outputs`
 * (RandR / wl_output names, NULL-terminated, may be NULL); else the only
 * device with a display attached; else the firmware's primary adapter;
 * else the only device. -1 = cannot be told. */
int walpe_gpu_display_index (GPtrArray *devs, const char *const *outputs,
                             const char *display_node);
/* the device a /dev/dri/... path (card or render node) belongs to, -1 */
int walpe_gpu_index_of_node (GPtrArray *devs, const char *node);
/* The device that renders when nothing is set, given the one that drives
 * the displays (`display`, -1 = not known): that device itself when it
 * can render; when it is a display controller without a renderer (a
 * system-on-chip: display controller and GPU are two devices, and that is
 * no second GPU to copy frames to) or not known, the only device that has
 * a render node. -1 = cannot be told. */
int walpe_gpu_default_renderer (GPtrArray *devs, int display);

/* does a sysfs output name ("HDMI-A-1") mean the window system's output
 * `name` ("HDMI-A-1", "HDMI-1", "HDMI1", "HDMI-A-1-1" of a second
 * provider)? Kernel and X drivers number and spell outputs differently:
 * the connector type and the last number are compared. */
gboolean walpe_gpu_output_matches (const char *sysfs_name, const char *name);

/* What has to be in the environment so that EGL renders on `target`
 * while `display` (may be NULL = unknown) drives the screen: key, value,
 * key, value, ..., NULL (g_strfreev); an empty list when target is the
 * display device. mesa_vendor_json / nvidia_vendor_json: the EGL vendor
 * files (walpe_gpu_find_egl_vendor), NULL when not found. *note (optional,
 * g_free): what could not be arranged and why. */
char **walpe_gpu_offload_env (const WalpeGpuDev *display, const WalpeGpuDev *target,
                              WalpeGpuWinsys winsys, const char *mesa_vendor_json,
                              const char *nvidia_vendor_json, char **note);
/* the variables walpe_gpu_offload_env can set, NULL-terminated: when the
 * user has set one of them the setting is not applied */
const char *const *walpe_gpu_offload_env_names (void);

/* The EGL vendor file (libglvnd) whose ICD.library_path contains `word`
 * ("mesa", "nvidia"), searched like libglvnd does (dirs: ':'-separated,
 * NULL = __EGL_VENDOR_LIBRARY_DIRS or the two standard directories).
 * g_free; NULL = none. */
char *walpe_gpu_find_egl_vendor (const char *dirs, const char *word);

/* Runs argv with envp (NULL = this process's environment) and collects
 * what it writes to stdout and stderr - for at most limit_ms by this
 * process's own clock, whatever the program does. For the test process of
 * a GPU choice: it talks to a driver, and a start that waits for it must
 * not depend on the driver letting it end.
 *   The program ends, or closes both streams: back at once, as from
 *   g_spawn_sync.
 *   It has ended and something it started still holds a stream: back
 *   within a few hundredths of a second, with what it wrote
 *   (g_spawn_sync waits for that other process).
 *   The time is up: *timed_out, the program is sent SIGKILL and not
 *   waited for - also when the signal does not end it (a task that
 *   sleeps in a driver and cannot be interrupted).
 * The program is never a child of this process (it is started through a
 * process that ends at once): nothing is left to be reaped, whenever and
 * however it ends, and code that ends "all children" never waits for it.
 * So there is no exit status; what it wrote is the result.
 * FALSE + *error: it could not be started. *out / *errout: g_free; at
 * most 64 KiB of each are kept (the rest is read and dropped). */
gboolean walpe_gpu_run_limited (char **argv, char **envp, guint limit_ms, char **out,
                                char **errout, gboolean *timed_out, GError **error);

/* The report of `walpe-daemon --gpu-probe` (lines "key=value": device,
 * render_node, vendor, renderer, version, drew, fail). TRUE when a frame
 * was drawn by a hardware renderer. The strings are set when the report
 * has them (g_free), NULL otherwise; *why (g_free) on FALSE. */
gboolean walpe_gpu_parse_probe (const char *out, char **device, char **render_node,
                                char **renderer, char **why);

/* one device as members of the object being built: name, pci, driver,
 * class, class_guessed, vendor_id, device_id, card, render_node, boot_vga,
 * connectors */
void walpe_gpu_dev_to_json (JsonBuilder *b, const WalpeGpuDev *d);
/* one line: "Intel ... [8086:9a49] at 0000:00:02.0, integrated, driver i915, outputs eDP-1" */
char *walpe_gpu_dev_describe (const WalpeGpuDev *d);

/* ---- the GPU this process renders on
 * One record for everything that has to name it (status, the video
 * decoder's device, usage figures). Set once by the daemon when EGL is
 * up; read from any thread. "driver" is the kernel driver, spelled as in
 * sysfs (uevent DRIVER=) and in /proc/<pid>/fdinfo (drm-driver); "pci" is
 * spelled as in sysfs and fdinfo's drm-pdev. */
typedef struct {
    char *render_node;       /* "/dev/dri/renderD128" or NULL */
    char *card_node;         /* "/dev/dri/card0" or NULL */
    char *pci;               /* "0000:00:02.0" or NULL */
    char *driver;            /* "i915", "amdgpu", "nvidia", ...; "" = not known */
    char *sysfs;             /* the device's sysfs directory or NULL */
    guint vendor, device;
    WalpeGpuClass klass;
    char *name;
    gboolean software;       /* a CPU rasteriser renders: no device */
} WalpeGpuInUse;

/* dev NULL + software FALSE: not known (the record is emptied) */
void walpe_gpu_in_use_set (const WalpeGpuDev *dev, gboolean software);
/* a copy (walpe_gpu_in_use_clear); FALSE = nothing was set */
WALPE_API gboolean walpe_gpu_in_use (WalpeGpuInUse *out);
WALPE_API void walpe_gpu_in_use_clear (WalpeGpuInUse *u);
/* the record as members of the object being built (the same members as
 * walpe_gpu_dev_to_json plus "sysfs" and "software") */
void walpe_gpu_in_use_to_json (JsonBuilder *b, const WalpeGpuInUse *u);

/* The quality preset that suits a class of renderer, for the opt-in
 * `preset_follow_gpu`: "high" for a discrete GPU, "balanced" for an
 * integrated one, "low" for a software renderer (klass ignored then) and
 * for a virtual adapter; NULL = leave the preset alone (unknown class). */
const char *walpe_gpu_class_preset (WalpeGpuClass klass, gboolean software);

G_END_DECLS
