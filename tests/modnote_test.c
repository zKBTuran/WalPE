/* modnote-test - `walpe-ctl mem` says what a module was loaded for
 * (engine/modload.c): a scene's video texture is opened through
 * walpe_video_tex_new_copy (), and that entry has to leave its name and
 * count its use like the others do. The clip does not exist: the module is
 * loaded, the open fails, the note stays. No GL, no display. */
#include "engine/modules.h"
#include "engine/videotex.h"
#include "common/util.h"
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

int main (void)
{
    GError *err = NULL;
    const char *clip = "/nonexistent/walpe-modnote-test/clip.tex";
    WalpeVideoTex *v = walpe_video_tex_new_copy (clip, 87, 1000, NULL, &err);
    if (v) {
        printf ("FAIL: a clip that does not exist was opened\n");
        return 1;
    }
    g_clear_error (&err);
    if (!walpe_video_module (FALSE)) {
        printf ("SKIP: walpe-video.so is not next to this program\n");
        return 77;
    }
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    walpe_modules_mem_json (b, g_get_monotonic_time ());
    json_builder_end_object (b);
    JsonNode *root = json_builder_get_root (b);
    JsonObject *mods = json_object_get_object_member (json_node_get_object (root), "modules");
    JsonObject *video = mods && json_object_has_member (mods, "video")
                            ? json_object_get_object_member (mods, "video") : NULL;
    const char *why = video && json_object_has_member (video, "loaded_for")
                          ? json_object_get_string_member (video, "loaded_for") : NULL;
    gint64 uses = video && json_object_has_member (video, "uses")
                      ? json_object_get_int_member (video, "uses") : -1;
    int fails = 0;
    if (!why || !g_str_has_prefix (why, "a scene with a video texture was opened") || !strstr (why, clip)) {
        printf ("FAIL: loaded for \"%s\"\n", why ? why : "(nothing)");
        fails++;
    }
    if (uses != 1) {
        printf ("FAIL: used %d time(s), 1 expected\n", (int) uses);
        fails++;
    }
    json_node_unref (root);
    g_object_unref (b);
    if (!fails)
        printf ("modnote-test: ok\n");
    return fails ? 1 : 0;
}
