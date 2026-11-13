/* websinkdir_test.c - which directory the daemon names to GStreamer as the
 * place of libgstwalpesound.so (engine/modload.c walpe_web_sound_sink_dir).
 * GStreamer loads every plugin it finds in a directory of its path - into
 * the page process, and into gst-inspect-1.0 when the web module asks it
 * for elements - so the directory has to be as trustworthy as the one file:
 * nothing of another account or writable by everybody in it, and no ':' in
 * its name (the path is a list). No GStreamer, no display. */
#include "engine/modules.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); fails++; } } while (0)

/* <base>/<name>/gst/libgstwalpesound.so, an empty file: the module directory */
static char *make_dir (const char *base, const char *name)
{
    char *mod = g_build_filename (base, name, NULL);
    char *gst = g_build_filename (mod, "gst", NULL);
    char *so = g_build_filename (gst, WALPE_WEB_SOUND_SINK_FILE, NULL);
    g_mkdir_with_parents (gst, 0755);
    g_file_set_contents (so, "", 0, NULL);
    g_chmod (so, 0755);
    g_free (so);
    g_free (gst);
    return mod;
}

static char *ask (const char *mod)
{
    g_setenv ("WALPE_MODULE_DIR", mod, TRUE);
    return walpe_web_sound_sink_dir ();
}

int main (void)
{
    char *base = g_dir_make_tmp ("walpe-sinkdir-XXXXXX", NULL);
    if (!base) {
        printf ("FAIL: no temp dir\n");
        return 1;
    }
    g_unsetenv ("WALPE_WEB_SOUND_SINK");
    /* the plain case: found, and it is the directory gst/ */
    char *mod = make_dir (base, "plain");
    char *want = g_build_filename (mod, "gst", NULL);
    char *got = ask (mod);
    CHECK (!g_strcmp0 (got, want), "a directory with the plugin alone: %s", got ? got : "(not used)");
    g_free (got);
    /* switched off */
    g_setenv ("WALPE_WEB_SOUND_SINK", "0", TRUE);
    got = ask (mod);
    CHECK (got == NULL || !g_str_has_prefix (got, base), "WALPE_WEB_SOUND_SINK=0: %s", got ? got : "(not used)");
    g_free (got);
    g_unsetenv ("WALPE_WEB_SOUND_SINK");
    /* what a build leaves beside it (meson's directory of object files): fine */
    char *objs = g_build_filename (want, "libgstwalpesound.so.p", NULL);
    g_mkdir (objs, 0755);
    got = ask (mod);
    CHECK (!g_strcmp0 (got, want), "with a directory of the user's own beside the plugin: %s", got ? got : "(not used)");
    g_free (got);
    /* a second file that everybody can write to: GStreamer would load it too */
    char *other = g_build_filename (want, "libgstother.so", NULL);
    g_file_set_contents (other, "", 0, NULL);
    g_chmod (other, 0666);
    got = ask (mod);
    CHECK (got == NULL || !g_str_has_prefix (got, base), "a file beside the plugin that everybody can write to: %s",
           got ? got : "(not used)");
    g_free (got);
    g_chmod (other, 0644);
    got = ask (mod);
    CHECK (!g_strcmp0 (got, want), "the same file, writable by its owner only: %s", got ? got : "(not used)");
    g_free (got);
    if (getuid () == 0) {
        /* (only root can give a file away) another account's file */
        if (chown (other, 65534, 65534) == 0) {
            got = ask (mod);
            CHECK (got == NULL || !g_str_has_prefix (got, base), "a file of another account beside the plugin: %s",
                   got ? got : "(not used)");
            g_free (got);
        }
    }
    g_remove (other);
    /* a sub-directory everybody can write to (GStreamer looks into it) */
    g_chmod (objs, 0777);
    got = ask (mod);
    CHECK (got == NULL || !g_str_has_prefix (got, base), "a directory below it that everybody can write to: %s",
           got ? got : "(not used)");
    g_free (got);
    g_chmod (objs, 0755);
    g_rmdir (objs);
    /* the plugin path is a list: a ':' in the directory's name */
    char *colon = make_dir (base, "a:b");
    got = ask (colon);
    CHECK (got == NULL || !g_str_has_prefix (got, base), "a directory with ':' in its name: %s", got ? got : "(not used)");
    g_free (got);
    /* left as it was found */
    char *cmd[] = { (char *) "rm", (char *) "-r", base, NULL };
    g_spawn_sync (NULL, cmd, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free (colon);
    g_free (other);
    g_free (objs);
    g_free (want);
    g_free (mod);
    g_free (base);
    if (fails) {
        printf ("websinkdir: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("websinkdir: ok\n");
    return 0;
}
