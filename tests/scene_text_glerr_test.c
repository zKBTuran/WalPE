/* scene_text_glerr_test - a text layer of an old-format scene (scene.json
 * with "layers", src/engine/scene.c) whose texture OpenGL refuses.
 *
 * make_text_texture () names a texture, uploads the rendered text and asks
 * glGetError (). When that reports an error the layer is not drawn - and
 * the name was neither kept nor deleted: one texture (with its storage,
 * when the upload itself had worked and the error was an older one) stayed
 * per such layer and load, for the life of the GL context.
 *
 * OpenGL here is the stand-in of the fuzz harnesses (tests/fuzz/fakegl.c):
 * no display. Its entries for glGenTextures, glDeleteTextures and
 * glGetError are wrapped to count the texture names that are alive and to
 * report an error on demand.
 *  1. without an error: both text layers get a texture, and the scene's
 *     gl_free deletes them;
 *  2. with glGetError () reporting GL_OUT_OF_MEMORY: no texture name is
 *     left behind after gl_init, none after gl_free;
 *  3. a load without the error works again afterwards.
 * Skipped (77) where Pango finds no font to measure the text with. */
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/gl.h>
#include "engine/scene.h"
#include "engine/glutil.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void (*real_gen) (GLsizei, GLuint *);
static void (*real_del) (GLsizei, const GLuint *);
static GHashTable *alive;
static gboolean refuse;
static int asked;

static void t_gen (GLsizei n, GLuint *ids)
{
    real_gen (n, ids);
    for (GLsizei i = 0; i < n; i++)
        g_hash_table_add (alive, GUINT_TO_POINTER (ids[i]));
}

static void t_del (GLsizei n, const GLuint *ids)
{
    for (GLsizei i = 0; i < n; i++)
        g_hash_table_remove (alive, GUINT_TO_POINTER (ids[i]));
    real_del (n, ids);
}

static GLenum t_err (void)
{
    asked++;
    return refuse ? GL_OUT_OF_MEMORY : GL_NO_ERROR;
}

static const char scene_json[] =
    "{\"object\":{\"width\":640,\"height\":360,\"layers\":["
    "{\"text\":{\"text\":\"12:34\",\"size\":48}},"
    "{\"compose\":{\"layers\":[{\"text\":{\"text\":\"Thursday\",\"size\":24,\"color\":[1.0,0.5,0.0,1.0]}}]}}"
    "]}}";

/* one load: the texture names alive after gl_init (-1 = it did not load) */
static int load_once (const char *dir, guint *after_free)
{
    GError *err = NULL;
    WalpeLoadOpts opts = { .fps = 30, .origin_dir = dir };
    WalpeContent *c = walpe_scene_new (dir, NULL, &opts, &err);
    g_clear_error (&err);
    if (!c)
        return -1;
    int n = -1;
    if (c->gl_init (c, &err))
        n = (int) g_hash_table_size (alive);
    g_clear_error (&err);
    c->gl_free (c);
    *after_free = g_hash_table_size (alive);
    c->free (c);
    return n;
}

int main (void)
{
    g_setenv ("WALPE_SOUND_NULL", "1", TRUE);
    char *dir = g_dir_make_tmp ("walpe-textglerr-XXXXXX", NULL);
    if (!dir) {
        g_printerr ("no temporary directory\n");
        return 1;
    }
    char *sj = g_build_filename (dir, "scene.json", NULL);
    g_file_set_contents (sj, scene_json, -1, NULL);

    /* what the engine makes once per thread is made before the count */
    walpe_glutil_init ();
    alive = g_hash_table_new (NULL, NULL);
    real_gen = epoxy_glGenTextures;
    real_del = epoxy_glDeleteTextures;
    epoxy_glGenTextures = t_gen;
    epoxy_glDeleteTextures = t_del;
    epoxy_glGetError = t_err;

    /* 1. no error */
    guint left = 99;
    int n = load_once (dir, &left);
    CHECK (n >= 0);
    int rc = 0;
    if (n == 0 && !fails) {
        printf ("skip (no text was rendered: no font here?)\n");
        rc = 77;
        goto out;
    }
    CHECK (n == 2);         /* the two text layers */
    CHECK (asked >= 2);     /* the test stands on that question */
    CHECK (left == 0);

    /* 2. the driver refuses */
    refuse = TRUE;
    asked = 0;
    left = 99;
    n = load_once (dir, &left);
    refuse = FALSE;
    CHECK (asked >= 2);
    CHECK (n == 0);         /* nothing kept ... */
    CHECK (left == 0);      /* ... and nothing that gl_free does not know of */

    /* 3. and again without */
    left = 99;
    n = load_once (dir, &left);
    CHECK (n == 2);
    CHECK (left == 0);

    if (!fails)
        printf ("scene text GL error: OK\n");
    rc = fails ? 1 : 0;
out:
    g_unlink (sj);
    g_rmdir (dir);
    g_free (sj);
    g_free (dir);
    return rc;
}
