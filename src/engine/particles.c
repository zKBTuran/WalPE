#include "particles.h"
#include "glutil.h"
#include "glfeat.h"
#include "clip.h"
#include "common/util.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define MAX_PARTICLES 4096
#define MAX_SYSTEMS 16

typedef struct {
    double origin[2];
    double rate;       /* particles per second */
    double lifetime;   /* seconds */
    double size;       /* point size in scene px */
    double velocity[2];
    double gravity[2];
    double drag;
    char *tex_path;    /* resolved texture path or NULL -> soft circle */
} PtCfg;

typedef struct {
    GLuint tex;
    gboolean premult;
    gboolean loaded;
} PtSys;

struct WalpeParticles {
    PtCfg cfg[MAX_SYSTEMS];
    PtSys sys[MAX_SYSTEMS];
    int n_sys;

    float px[MAX_PARTICLES], py[MAX_PARTICLES];
    float vx[MAX_PARTICLES], vy[MAX_PARTICLES];
    float life[MAX_PARTICLES], maxlife[MAX_PARTICLES];
    float size[MAX_PARTICLES];
    int   sys_of[MAX_PARTICLES];
    int   count;
    double emit_accum[MAX_SYSTEMS];
    double last_t;

    GLuint prog, vao, vbo, white_tex;
    gpointer verts;    /* PtVertex[MAX_PARTICLES]: this object's vertex scratch
                        * (was one function-static array written by every
                        * monitor's render thread) */
    GLint loc_mvp, loc_tex, loc_premult;
};

typedef struct {
    float x, y, size, alpha;
} PtVertex;

static const char *PT_VS =
    "in vec2 a_pos;\n"
    "in float a_size;\n"
    "in float a_alpha;\n"
    "uniform mat3 u_mvp;\n"
    "out float v_alpha;\n"
    "void main(){ v_alpha = a_alpha;\n"
    "  vec2 p = (u_mvp * vec3(a_pos, 1.0)).xy;\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "  gl_PointSize = a_size; }\n";

static const char *PT_FS =
    "in float v_alpha;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_premult;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  vec2 uv = gl_PointCoord; uv.y = 1.0 - uv.y;\n"
    "  vec4 c = texture(u_tex, uv) * v_alpha;\n"
    "  c.rgb *= mix(c.a, 1.0, u_premult);\n"
    "  frag = c; }\n";

/* resolve a possibly-relative asset path inside a WE project */
static char *resolve_asset (const char *dir, const char *file)
{
    if (!file || !*file)
        return NULL;
    /* the name comes from the scene json: below dir, a regular file */
    char *a = walpe_asset_path (dir, file);
    if (a)
        return a;
    char *b = g_build_filename ("assets", file, NULL);
    a = walpe_asset_path (dir, b);
    g_free (b);
    if (a)
        return a;
    /* missing: keep the name so the loader's message says which file */
    a = g_build_filename (dir, file, NULL);
    if (walpe_path_is_inside (a, dir) && !g_file_test (a, G_FILE_TEST_EXISTS))
        return a;
    g_free (a);
    return NULL;
}

static void parse_system (PtCfg *cfg, JsonObject *s, const char *dir)
{
    memset (cfg, 0, sizeof (*cfg));
    JsonObject *o = wpj_obj (s, "objectconfig");
    if (!o)
        return;
    wpj_num2 (o, "origin", &cfg->origin[0], &cfg->origin[1]);
    cfg->rate = wpj_num (o, "emissionrate", wpj_num (o, "particlerate", 30));
    if (cfg->rate < 0)
        cfg->rate = 0;
    if (cfg->rate > 500)
        cfg->rate = 500;
    cfg->lifetime = wpj_num (o, "lifetime", 3.0);
    if (cfg->lifetime <= 0)
        cfg->lifetime = 3.0;
    cfg->size = wpj_num (o, "size", 16.0);
    if (cfg->size < 1)
        cfg->size = 1;
    if (cfg->size > 512)
        cfg->size = 512;
    wpj_num2 (o, "velocity", &cfg->velocity[0], &cfg->velocity[1]);
    wpj_num2 (o, "gravity", &cfg->gravity[0], &cfg->gravity[1]);
    cfg->drag = wpj_num (o, "damping", 0);
    const char *img = wpj_str (o, "imagepath", wpj_str (o, "image", NULL));
    if (img)
        cfg->tex_path = resolve_asset (dir, img);
}

WalpeParticles *walpe_particles_new (JsonArray *systems, const char *project_dir)
{
    if (!systems || json_array_get_length (systems) == 0)
        return NULL;
    WalpeParticles *p = g_new0 (WalpeParticles, 1);
    p->last_t = g_get_monotonic_time () / 1000000.0;
    guint n = json_array_get_length (systems);
    for (guint i = 0; i < n && p->n_sys < MAX_SYSTEMS; i++) {
        JsonObject *s = wpj_arr_obj (systems, i);
        if (!s)
            continue;
        if (!wpj_obj (s, "objectconfig"))
            continue;
        parse_system (&p->cfg[p->n_sys], s, project_dir);
        p->n_sys++;
    }
    if (p->n_sys == 0) {
        g_free (p);
        return NULL;
    }
    return p;
}

gboolean walpe_particles_usable (WalpeParticles *p)
{
    return p && p->n_sys > 0;
}

static GLuint make_white_tex (void)
{
    GLuint t = 0;
    glGenTextures (1, &t);
    glBindTexture (GL_TEXTURE_2D, t);
    guint8 px[4 * 4 * 4];
    for (int i = 0; i < 16; i++) {
        px[i * 4 + 0] = 255;
        px[i * 4 + 1] = 255;
        px[i * 4 + 2] = 255;
        px[i * 4 + 3] = 255;
    }
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}

void walpe_particles_gl_init (WalpeParticles *p)
{
    char *vs_src = g_strconcat (walpe_gl_shader_header (), PT_VS, NULL);
    char *fs_src = g_strconcat (walpe_gl_shader_fs_header (), PT_FS, NULL);
    GLuint vs = glCreateShader (GL_VERTEX_SHADER);
    glShaderSource (vs, 1, (const char **) &vs_src, NULL);
    glCompileShader (vs);
    GLuint fs = glCreateShader (GL_FRAGMENT_SHADER);
    glShaderSource (fs, 1, (const char **) &fs_src, NULL);
    glCompileShader (fs);
    g_free (vs_src);
    g_free (fs_src);
    p->prog = glCreateProgram ();
    glAttachShader (p->prog, vs);
    glAttachShader (p->prog, fs);
    /* explicit locations must match the VAO below (a_pos=0, a_size=1, a_alpha=2) */
    glBindAttribLocation (p->prog, 0, "a_pos");
    glBindAttribLocation (p->prog, 1, "a_size");
    glBindAttribLocation (p->prog, 2, "a_alpha");
    glLinkProgram (p->prog);
    glDetachShader (p->prog, vs);
    glDetachShader (p->prog, fs);
    glDeleteShader (vs);
    glDeleteShader (fs);
    p->loc_mvp = glGetUniformLocation (p->prog, "u_mvp");
    p->loc_tex = glGetUniformLocation (p->prog, "u_tex");
    p->loc_premult = glGetUniformLocation (p->prog, "u_premult");

    glGenVertexArrays (1, &p->vao);
    glBindVertexArray (p->vao);
    glGenBuffers (1, &p->vbo);
    glBindBuffer (GL_ARRAY_BUFFER, p->vbo);
    glBufferData (GL_ARRAY_BUFFER, sizeof (PtVertex) * MAX_PARTICLES, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, sizeof (PtVertex), (void *) 0);
    glEnableVertexAttribArray (1);
    glVertexAttribPointer (1, 1, GL_FLOAT, GL_FALSE, sizeof (PtVertex), (void *) 8);
    glEnableVertexAttribArray (2);
    glVertexAttribPointer (2, 1, GL_FLOAT, GL_FALSE, sizeof (PtVertex), (void *) 12);
    glBindVertexArray (0);

    p->white_tex = make_white_tex ();
    for (int i = 0; i < p->n_sys; i++) {
        if (p->cfg[i].tex_path) {
            WalpeGlTex t = walpe_gl_tex_from_file (p->cfg[i].tex_path);
            p->sys[i].tex = t.tex;
            p->sys[i].premult = t.premult;
        }
        p->sys[i].loaded = TRUE;
    }
}

void walpe_particles_render (WalpeParticles *p, float scene_w, float scene_h,
                             int vw, int vh, float px, float py)
{
    if (p->n_sys == 0)
        return;
    double now = g_get_monotonic_time () / 1000000.0;
    double dt = now - p->last_t;
    if (dt > 0.25)
        dt = 0.25;
    p->last_t = now;

    for (int i = 0; i < p->n_sys; i++) {
        PtCfg *cfg = &p->cfg[i];
        p->emit_accum[i] += cfg->rate * dt;
        while (p->emit_accum[i] >= 1.0 && p->count < MAX_PARTICLES) {
            p->emit_accum[i] -= 1.0;
            int n = p->count++;
            p->sys_of[n] = i;
            p->px[n] = (float) cfg->origin[0];
            p->py[n] = (float) cfg->origin[1];
            p->vx[n] = (float) cfg->velocity[0];
            p->vy[n] = (float) cfg->velocity[1];
            p->maxlife[n] = (float) cfg->lifetime;
            p->life[n] = (float) cfg->lifetime;
            p->size[n] = (float) cfg->size;
        }
        (void) cfg;
    }

    /* integrate */
    for (int n = 0; n < p->count; n++) {
        PtCfg *cfg = &p->cfg[p->sys_of[n]];
        p->life[n] -= (float) dt;
        if (p->life[n] <= 0) {
            int last = --p->count;
            p->px[n] = p->px[last]; p->py[n] = p->py[last];
            p->vx[n] = p->vx[last]; p->vy[n] = p->vy[last];
            p->life[n] = p->life[last]; p->maxlife[n] = p->maxlife[last];
            p->size[n] = p->size[last]; p->sys_of[n] = p->sys_of[last];
            n--;
            continue;
        }
        p->vx[n] += (float) (cfg->gravity[0] * dt);
        p->vy[n] += (float) (cfg->gravity[1] * dt);
        if (cfg->drag > 0) {
            float d = (float) (1.0 - cfg->drag * dt);
            if (d < 0)
                d = 0;
            p->vx[n] *= d;
            p->vy[n] *= d;
        }
        p->px[n] += p->vx[n] * (float) dt;
        p->py[n] += p->vy[n] * (float) dt;
    }

    if (p->count == 0)
        return;

    if (!p->verts)
        p->verts = g_new (PtVertex, MAX_PARTICLES);
    PtVertex *verts = p->verts;
    for (int n = 0; n < p->count; n++) {
        verts[n].x = p->px[n] + px * 20.0f;
        verts[n].y = p->py[n] + py * 20.0f;
        verts[n].size = p->size[n];
        verts[n].alpha = p->life[n] / (p->maxlife[n] > 0 ? p->maxlife[n] : 1.0f);
    }
    glBindBuffer (GL_ARRAY_BUFFER, p->vbo);
    glBufferSubData (GL_ARRAY_BUFFER, 0, sizeof (PtVertex) * (gsize) p->count, verts);

    float k = fmaxf ((float) vw / scene_w, (float) vh / scene_h);
    WalpeMat3 c;
    walpe_mat3_identity (&c);
    walpe_mat3_translate (&c, -scene_w * k / (float) vw, scene_h * k / (float) vh);
    walpe_mat3_scale (&c, 2 * k / (float) vw, -2 * k / (float) vh);

    glUseProgram (p->prog);
    glUniformMatrix3fv (p->loc_mvp, 1, GL_FALSE, c.m);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, p->sys[0].tex ? p->sys[0].tex : p->white_tex);
    glUniform1i (p->loc_tex, 0);
    glUniform1f (p->loc_premult, p->sys[0].tex ? (p->sys[0].premult ? 1.0f : 0.0f) : 1.0f);
    glBindVertexArray (p->vao);
    /* OpenGL ES has no such switch (gl_PointSize always counts there);
     * the enum would be GL_INVALID_ENUM on every draw */
    const gboolean point_switch = !walpe_gl_feat_es ();
    if (point_switch)
        glEnable (GL_PROGRAM_POINT_SIZE);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (int it = 0; walpe_clip_next (&it, NULL);)   /* per visible box (clip.h) */
        glDrawArrays (GL_POINTS, 0, p->count);
    if (point_switch)
        glDisable (GL_PROGRAM_POINT_SIZE);
    glBindVertexArray (0);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void walpe_particles_free (WalpeParticles *p)
{
    if (!p)
        return;
    for (int i = 0; i < p->n_sys; i++) {
        if (p->sys[i].tex)
            glDeleteTextures (1, &p->sys[i].tex);
        g_free (p->cfg[i].tex_path);
    }
    /* what walpe_particles_gl_init made (nothing when it never ran): one
     * program, VAO, 64 KiB buffer and texture stayed behind per load, in
     * the monitor's context after every wallpaper switch */
    if (p->prog)
        glDeleteProgram (p->prog);
    if (p->vao)
        glDeleteVertexArrays (1, &p->vao);
    if (p->vbo)
        glDeleteBuffers (1, &p->vbo);
    if (p->white_tex)
        glDeleteTextures (1, &p->white_tex);
    g_free (p->verts);
    g_free (p);
}
