/* fuzz_scene - a whole scene wallpaper through the loader and three frames
 * of the renderer: project.json, scene.json, the model / material / effect
 * / particle files and shaders it names, textures, puppet models
 * (src/engine/content.c, scene.c, scenenode*.c, weparticles.c, ...), then
 * the scan of the folder that the options window makes (propnotes.c).
 * OpenGL is the stand-in of fakegl.c: nothing is drawn, but every buffer
 * the engine hands to GL is read and every read-back is written.
 *
 * The input is a small archive of the wallpaper's files:
 *     === scene.json\n<bytes>\n=== materials/a.json\n<bytes> ...
 * (a line "=== <relative name>" starts a file; at most 48 files). They are
 * written to a directory of the harness - $WALPE_FUZZ_TMP, best a tmpfs -
 * which is emptied before every input.
 * Scripts need QuickJS, which is not what is tested here: build the fuzz
 * targets with -Dscenescript=disabled. */
#include "fuzz_archive.h"
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/propnotes.h"
#include <epoxy/gl.h>

static const char *dir;

static void redraw (gpointer user) { (void) user; }

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    g_setenv ("WALPE_SOUND_NULL", "1", TRUE);   /* no sound server is asked */
    g_unsetenv ("WALPE_WE_ASSETS");
    dir = fuzz_archive_dir = fuzz_dir ("scene");
    walpe_glutil_init ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    fuzz_unpack (data, size, NULL);
    if (!n_written)
        return 0;
    GError *err = NULL;
    WalpeLoadOpts opts = { .video_backend = "ffmpeg", .fps = 60, .origin_dir = dir };
    WalpeContent *c = walpe_content_new (dir, &opts, &err);
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
    /* what the options window reads of the same folder */
    JsonNode *defs = walpe_content_property_defs (dir, NULL);
    if (defs)
        json_node_unref (defs);
    JsonObject *notes = walpe_prop_notes (dir);
    if (notes)
        json_object_unref (notes);
    return 0;
}
