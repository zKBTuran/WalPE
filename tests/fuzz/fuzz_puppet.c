/* fuzz_puppet - puppet models (src/engine/puppet.c): the MDLV mesh, the
 * MDLS bones, MDAT attachment points and MDLA animations of a .mdl file,
 * then what the renderer does with them - animation layers as a scene
 * names them, the pose at a few times, every attachment point, the texture
 * cover, a draw (skinning on the CPU; OpenGL is the stand-in of fakegl.c).
 * The input is the model file. puppet.c is included for the model's names. */
#include "fuzz_common.h"
#include "../../src/engine/puppet.c"

static JsonNode *layers_json;

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    layers_json = json_from_string (
        "[{\"animation\":1,\"rate\":1,\"blend\":1,\"visible\":true},"
        "{\"animation\":2,\"rate\":-0.5,\"blend\":0.5,\"additive\":true},"
        "{\"animation\":3,\"rate\":{\"user\":\"speed\",\"value\":7},\"blend\":2,\"visible\":{\"user\":\"on\",\"value\":true}},"
        "{\"animation\":1,\"rate\":1e30,\"blend\":1e-3},{\"animation\":99},7]", NULL);
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    static const double times[] = { 0, 0.016, 1.5, 1e9, -3, 1e300 };
    static const float mvp[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, tint[3] = { 1, 1, 1 };
    WalpePuppet *p = walpe_puppet_load (fuzz_file (0, data, size));
    if (!p)
        return 0;
    FUZZ_CHECK (p->nverts > 0 && p->nbones >= 0 && p->nbones < 256);
    for (int pass = 0; pass < 2; pass++) {
        if (pass)
            walpe_puppet_set_layers (p, json_node_get_array (layers_json), NULL);
        for (guint i = 0; i < G_N_ELEMENTS (times); i++) {
            float m[6];
            for (int a = 0; a < walpe_puppet_attachment_count (p); a++)
                walpe_puppet_attachment (p, p->att[a].name, times[i], m);
            walpe_puppet_attachment (p, "none of them", times[i], m);
            walpe_puppet_draw (p, times[i], 32.0f, 24.0f, 1, i & 1, 0.5f, mvp, (int) i % 3, tint);
        }
        float rects[8][4];
        int n = walpe_puppet_uv_cover (p, rects, 8);
        FUZZ_CHECK (n >= 0 && n <= 8);
        JsonBuilder *b = json_builder_new ();
        walpe_puppet_probe (p, b);
        g_object_unref (b);
    }
    walpe_puppet_bone_count (p);
    walpe_puppet_gl_free (p);
    walpe_puppet_free (p);
    return 0;
}
