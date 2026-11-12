/* fuzz_particle - particle systems (src/engine/weparticles.c): a particle
 * file's emitters, initializers, operators, control points and renderer
 * are read and the system is simulated for a few steps without OpenGL (the
 * path of tests/particle_sim_test.c). The input is the particle JSON. */
#include "fuzz_common.h"
#include "engine/weparticles.h"
#include "common/util.h"

static const char *dir;

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    dir = fuzz_dir ("particle");   /* empty: materials and children are not found */
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    JsonNode *n = wpj_parse_data ((const char *) data, (gssize) size);
    if (!n)
        return 0;
    WalpeWeParticleOverride ov;
    walpe_we_particle_override_default (&ov);
    if (size > 2 && (data[size - 2] & 1)) {   /* an instance override, as a scene sets one */
        ov.count = 3.0f;
        ov.rate = 0.5f;
        ov.cp[1][0] = 5.0f;
        ov.cp_set = 2;
    }
    WalpeWeParticles *p = walpe_weparticles_new (json_node_get_object (n), dir, NULL, &ov);
    json_node_unref (n);
    if (!p)
        return 0;
    static const float world[9] = { 1, 0, 0, 0, 1, 0, 3, 4, 1 };
    walpe_weparticles_set_world (p, world);
    walpe_weparticles_set_cursor (p, 0.25f, 0.75f);
    walpe_weparticles_set_audio_level (p, 0.5f);
    for (int k = 0; k < 24; k++) {
        walpe_weparticles_step (p, k == 7 ? 2.5 : k == 13 ? 0.0 : 1.0 / 30.0);
        if (k == 10)
            walpe_weparticles_set_playing (p, 0);
        if (k == 12)
            walpe_weparticles_set_playing (p, 1);
        if (k == 16) {
            ov.count = 0.25f;
            walpe_weparticles_set_override (p, &ov, WALPE_WE_OV_COUNT | WALPE_WE_OV_RATE);
        }
    }
    int systems = walpe_weparticles_systems (p);
    FUZZ_CHECK (systems >= 1);
    for (int s = 0; s < systems && s < 64; s++) {
        int cnt = walpe_weparticles_sys_count (p, s);
        float pos[3], vel[3], age, old[3];
        for (int i = 0; i < cnt && i < 64; i++) {
            walpe_weparticles_sys_peek (p, s, i, pos, vel, &age);
            walpe_weparticles_sys_alpha (p, s, i);
            walpe_weparticles_sys_trail (p, s, i, old);
        }
    }
    walpe_weparticles_count (p);
    walpe_weparticles_uses_cursor (p);
    walpe_weparticles_uses_audio (p);
    walpe_weparticles_free (p);
    return 0;
}
