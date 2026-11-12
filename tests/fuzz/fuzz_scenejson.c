/* fuzz_scenejson - scene.json alone: the objects of a scene (image, text,
 * particle, sound and light layers, effect instances with their constant
 * values, combos and textures, instance overrides, parents, attachments,
 * animation layers, camera and general settings) through the loader and
 * three frames of the renderer (src/engine/scenenode*.c), over a fixed set
 * of the other files - models, materials, textures, effects, shaders,
 * particle files, a puppet - which is written once when the harness starts
 * (from FUZZ_ASSETS, an archive in the form of fuzz_archive.h; the seed
 * tests/fuzz/seeds/scene/everything.scene) and only read afterwards.
 * The input is the text of scene.json; no file is written per input, which
 * makes this the fast one of the two scene harnesses. OpenGL is the
 * stand-in of fakegl.c. Scripts: see fuzz_scene.c. */
#include "fuzz_archive.h"
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/scenenode.h"
#include "common/util.h"
#include <epoxy/gl.h>

#ifndef FUZZ_ASSETS
#error "FUZZ_ASSETS: the path of the archive with the scene's other files"
#endif

static const char *dir;

static void redraw (gpointer user) { (void) user; }

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    g_setenv ("WALPE_SOUND_NULL", "1", TRUE);
    g_unsetenv ("WALPE_WE_ASSETS");
    dir = fuzz_archive_dir = fuzz_dir ("scenejson");
    const char *assets = g_getenv ("WALPE_FUZZ_ASSETS");
    char *d = NULL;
    gsize n = 0;
    if (!g_file_get_contents (assets && *assets ? assets : FUZZ_ASSETS, &d, &n, NULL)) {
        g_printerr ("fuzz_scenejson: cannot read %s (set WALPE_FUZZ_ASSETS)\n", assets && *assets ? assets : FUZZ_ASSETS);
        exit (1);
    }
    fuzz_unpack ((const uint8_t *) d, n, "scene.json");
    g_free (d);
    walpe_glutil_init ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    /* read as the loader reads the file (walpe_scene_new) */
    JsonNode *root = wpj_parse_file (fuzz_file (0, data, size));
    if (!root)
        return 0;
    if (!json_object_has_member (json_node_get_object (root), "objects")) {
        json_node_unref (root);     /* the old layout: fuzz_scene reaches it */
        return 0;
    }
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    WalpeContent *c = walpe_scenenode_new_from_root (root, dir, &opts, &err);   /* takes root */
    g_clear_error (&err);
    if (!c)
        return 0;
    c->request_redraw = redraw;
    if (!c->gl_init || c->gl_init (c, &err)) {
        static const int sizes[3][2] = { { 64, 36 }, { 64, 36 }, { 31, 57 } };
        for (int f = 0; f < 3; f++) {
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
            glViewport (0, 0, sizes[f][0], sizes[f][1]);
            c->next_frame_us = 0;
            c->surface_lost = f == 2;
            if (c->pointer) {
                c->pointer (c, WALPE_PTR_MOVE, 0.25f * (float) f, 0.5f);
                c->pointer (c, f ? WALPE_PTR_UP : WALPE_PTR_DOWN, 0.5f, 0.5f);
            }
            if (c->gl_render)
                c->gl_render (c, sizes[f][0], sizes[f][1], f ? 0.5f : 0.0f, -0.25f);
            if (f == 1 && c->set_paused) {
                c->set_paused (c, TRUE);
                c->set_paused (c, FALSE);
            }
        }
    }
    g_clear_error (&err);
    if (c->gl_free)
        c->gl_free (c);
    c->free (c);
    return 0;
}
