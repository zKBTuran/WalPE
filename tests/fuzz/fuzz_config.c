/* fuzz_config - config.json as the daemon, the GUI and the tray read it
 * (src/common/config.c): every setting with its range and spellings, the
 * per-monitor and per-wallpaper tables, members of other versions; then
 * the settings are asked back and written out again (nothing reaches a
 * disk: the two calls that read and write the file are replaced here).
 * The file is the user's own - a hostile one is a second-order input, e.g.
 * a settings file from somewhere else. The input is the file's text.
 * config.c is included so that the file it opens is the input. */
#include "fuzz_common.h"
#include "common/util.h"
#include <glib/gstdio.h>

static const char *fuzz_cfg_file;
static gsize fuzz_cfg_written;

static JsonNode *fuzz_cfg_parse (const char *path)
{
    (void) path;
    return wpj_parse_file (fuzz_cfg_file);
}

static gboolean fuzz_cfg_write (const char *path, const char *data, gssize len, GError **err)
{
    (void) path; (void) err;
    fuzz_cfg_written = len < 0 ? strlen (data) : (gsize) len;
    fuzz_touch (data, fuzz_cfg_written);
    return TRUE;
}

static int fuzz_cfg_mkdir (const char *path, int mode)
{
    (void) path; (void) mode;
    return 0;
}

#define wpj_parse_file fuzz_cfg_parse
#define walpe_write_private fuzz_cfg_write
#define g_mkdir_with_parents fuzz_cfg_mkdir
#include "../../src/common/config.c"
#undef wpj_parse_file
#undef walpe_write_private
#undef g_mkdir_with_parents

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    /* (a directory with no config.json in it: nothing there is kept aside) */
    g_setenv ("XDG_CONFIG_HOME", fuzz_dir ("config"), TRUE);
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    static const char *const outs[] = { "HDMI-0", "DP-2", "", "eDP-1", "a/b\n" };
    fuzz_cfg_file = fuzz_file (0, data, size);
    WalpeConfig *c = walpe_config_load ();
    for (guint i = 0; i < G_N_ELEMENTS (outs); i++) {
        WalpeMonitorConf mc;
        walpe_config_monitor_path (c, outs[i]);
        walpe_config_monitor (c, outs[i]);
        walpe_config_monitor_get (c, outs[i], &mc);
    }
    walpe_config_set_outputs (c, outs, 2, TRUE);
    walpe_config_monitors_migrate (c, outs, 2);
    walpe_config_get_feature (c, "parallax");
    walpe_config_hwdec_word (c);
    if (c->current) {
        g_free (walpe_config_get_prop (c->current, "speed"));
        walpe_config_quality_ov (c, c->current);
    }
    walpe_config_save (c);
    FUZZ_CHECK (fuzz_cfg_written > 2);
    walpe_config_free (c);
    return 0;
}
