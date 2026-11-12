/* fakegl.c - a stand-in for OpenGL in the fuzz harnesses.
 *
 * The scene loaders create textures, programs and framebuffers while they
 * read a wallpaper's files. A harness has no display and must not spend
 * its time in a software rasterizer, so the entries of libepoxy that the
 * engine calls are defined here: most do nothing, some keep the little
 * state the engine asks back (object names, texture sizes, pixel store,
 * the uniforms a program's sources declare), and the ones that take a
 * buffer from the engine read or fill exactly the bytes OpenGL would - an
 * upload or a read-back with a buffer that is too small is then a report
 * of the address sanitizer, as it would be memory corruption in a driver.
 *
 * libepoxy's entries are pointers to functions; a program that defines
 * them itself gets its own (the shared library is still linked, for the
 * entries that are not listed here: calling one of those ends the run with
 * libepoxy's "no context" message, which says what to add).
 *
 * Built without -fsanitize=function: the entries that do nothing share one
 * function. */
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static intptr_t nop (void) { return 0; }
/* (through void (*) (void): the cast compilers do not warn about) */
#define NOP(name) __typeof__ (epoxy_##name) epoxy_##name = (__typeof__ (epoxy_##name)) (void (*) (void)) nop;
#define DEF(name, fn) __typeof__ (epoxy_##name) epoxy_##name = fn;

/* ------------------------------------------------------------- state */

#define NTEX 8192
typedef struct { GLuint id; int w, h, ifmt, csize; } Tex;
static Tex texs[NTEX];
static GLuint next_name = 1, bound_tex, bound_fbo_draw, bound_fbo_read;
static int unpack_align = 4, unpack_rowlen, pack_align = 4, pack_rowlen;
static int viewport[4];

#define NPROG 256
#define NUNI 64
typedef struct { char name[64]; GLenum type; int size; } Uni;
typedef struct { GLuint id; char *src; } Shader;
typedef struct { GLuint id; GLuint sh[4]; int nsh; Uni uni[NUNI]; int nuni; } Prog;
static Shader shaders[NPROG * 2];
static Prog progs[NPROG];

static Tex *tex_slot (GLuint id, int make)
{
    if (!id)
        return NULL;
    for (unsigned k = 0; k < 16; k++) {
        Tex *t = &texs[(id + k) % NTEX];
        if (t->id == id)
            return t;
        if (make && !t->id) {
            memset (t, 0, sizeof *t);
            t->id = id;
            return t;
        }
    }
    return NULL;
}

static volatile unsigned sink;
static void touch (const void *p, size_t n)
{
    const unsigned char *b = p;
    unsigned s = 0;
    for (size_t i = 0; i < n; i++)
        s += b[i];
    sink += s;
}

static size_t px_bytes (GLenum format, GLenum type)
{
    size_t ch, sz;
    switch (format) {
    case GL_RED: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: case GL_RED_INTEGER: ch = 1; break;
    case GL_RG: case GL_LUMINANCE_ALPHA: case GL_RG_INTEGER: ch = 2; break;
    case GL_RGB: case GL_BGR: ch = 3; break;
    case GL_RGBA: case GL_BGRA: ch = 4; break;
    default: return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: sz = 1; break;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: sz = 2; break;
    case GL_FLOAT: case GL_UNSIGNED_INT: case GL_INT: sz = 4; break;
    default: return 0;
    }
    return ch * sz;
}

/* the bytes a w x h transfer covers under the pixel store rules */
static size_t rect_bytes (int w, int h, GLenum format, GLenum type, int align, int rowlen)
{
    size_t bpp = px_bytes (format, type);
    if (!bpp || w <= 0 || h <= 0)
        return 0;
    size_t row = (size_t) (rowlen > 0 ? rowlen : w) * bpp;
    size_t a = align > 0 ? (size_t) align : 1;
    row = (row + a - 1) / a * a;
    return row * (size_t) (h - 1) + (size_t) w * bpp;
}

/* ----------------------------------------------------------- objects */

static void f_Gen (GLsizei n, GLuint *out)
{
    for (GLsizei i = 0; i < n; i++)
        out[i] = next_name++;
}

static void f_DeleteTextures (GLsizei n, const GLuint *ids)
{
    touch (ids, (size_t) (n > 0 ? n : 0) * sizeof *ids);
    for (GLsizei i = 0; i < n; i++) {
        Tex *t = tex_slot (ids[i], 0);
        if (t) {
            t->w = t->h = 0;   /* the slot stays taken: names are never reused */
        }
        if (ids[i] == bound_tex)
            bound_tex = 0;
    }
}

static void f_DeleteN (GLsizei n, const GLuint *ids)
{
    touch (ids, (size_t) (n > 0 ? n : 0) * sizeof *ids);
}

static void f_BindTexture (GLenum target, GLuint id)
{
    if (target == GL_TEXTURE_2D)
        bound_tex = id;
}

static void f_BindFramebuffer (GLenum target, GLuint id)
{
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
        bound_fbo_draw = id;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
        bound_fbo_read = id;
}

static GLenum f_CheckFramebufferStatus (GLenum target)
{
    (void) target;
    return GL_FRAMEBUFFER_COMPLETE;
}

static void f_PixelStorei (GLenum pname, GLint v)
{
    switch (pname) {
    case GL_UNPACK_ALIGNMENT: unpack_align = v; break;
    case GL_UNPACK_ROW_LENGTH: unpack_rowlen = v; break;
    case GL_PACK_ALIGNMENT: pack_align = v; break;
    case GL_PACK_ROW_LENGTH: pack_rowlen = v; break;
    default: break;
    }
}

static void f_Viewport (GLint x, GLint y, GLsizei w, GLsizei h)
{
    viewport[0] = x; viewport[1] = y; viewport[2] = w; viewport[3] = h;
}

/* ---------------------------------------------------------- textures */

static void f_TexImage2D (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                          GLenum format, GLenum type, const void *pixels)
{
    (void) border;
    if (pixels)
        touch (pixels, rect_bytes (w, h, format, type, unpack_align, unpack_rowlen));
    Tex *t = target == GL_TEXTURE_2D && level == 0 ? tex_slot (bound_tex, 1) : NULL;
    if (t) {
        t->w = w; t->h = h; t->ifmt = ifmt; t->csize = 0;
    }
}

static void f_TexSubImage2D (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                             GLenum format, GLenum type, const void *pixels)
{
    (void) target; (void) level; (void) x; (void) y;
    if (pixels)
        touch (pixels, rect_bytes (w, h, format, type, unpack_align, unpack_rowlen));
}

static void f_CompressedTexImage2D (GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                    GLint border, GLsizei size, const void *data)
{
    (void) border;
    if (data && size > 0)
        touch (data, (size_t) size);
    /* S3TC: what the driver reads is the blocks of w x h, whatever size says */
    if (data && w > 0 && h > 0 && (ifmt == 0x83F0 || ifmt == 0x83F1 || ifmt == 0x83F2 || ifmt == 0x83F3 ||
                                   ifmt == 0x8C4C || ifmt == 0x8C4D || ifmt == 0x8C4E || ifmt == 0x8C4F)) {
        size_t bs = ifmt == 0x83F0 || ifmt == 0x83F1 || ifmt == 0x8C4C || ifmt == 0x8C4D ? 8 : 16;
        touch (data, ((size_t) w + 3) / 4 * (((size_t) h + 3) / 4) * bs);
    }
    Tex *t = target == GL_TEXTURE_2D && level == 0 ? tex_slot (bound_tex, 1) : NULL;
    if (t) {
        t->w = w; t->h = h; t->ifmt = (int) ifmt; t->csize = size;
    }
}

static void f_GetTexLevelParameteriv (GLenum target, GLint level, GLenum pname, GLint *out)
{
    Tex *t = target == GL_TEXTURE_2D ? tex_slot (bound_tex, 0) : NULL;
    int w = t ? t->w >> level : 0, h = t ? t->h >> level : 0;
    if (t && t->w && level >= 0 && level < 31) {
        w = w ? w : 1;
        h = h ? h : 1;
    }
    switch (pname) {
    case GL_TEXTURE_WIDTH: *out = w; break;
    case GL_TEXTURE_HEIGHT: *out = h; break;
    case GL_TEXTURE_INTERNAL_FORMAT: *out = t ? t->ifmt : 0; break;
    case GL_TEXTURE_COMPRESSED: *out = t && t->csize > 0; break;
    case GL_TEXTURE_COMPRESSED_IMAGE_SIZE: *out = t ? t->csize : 0; break;
    default: *out = 0; break;
    }
}

static void f_GetTexParameteriv (GLenum target, GLenum pname, GLint *out)
{
    (void) target;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: case GL_TEXTURE_MAG_FILTER: *out = GL_LINEAR; break;
    case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T: *out = GL_CLAMP_TO_EDGE; break;
    case GL_TEXTURE_SWIZZLE_R: *out = GL_RED; break;
    case GL_TEXTURE_SWIZZLE_G: *out = GL_GREEN; break;
    case GL_TEXTURE_SWIZZLE_B: *out = GL_BLUE; break;
    case GL_TEXTURE_SWIZZLE_A: *out = GL_ALPHA; break;
    default: *out = 0; break;
    }
}

static void f_GetTexParameterfv (GLenum target, GLenum pname, GLfloat *out)
{
    (void) target; (void) pname;
    *out = 0;
}

static void f_GetTexImage (GLenum target, GLint level, GLenum format, GLenum type, void *pixels)
{
    Tex *t = target == GL_TEXTURE_2D ? tex_slot (bound_tex, 0) : NULL;
    if (t && pixels && level == 0)
        memset (pixels, 0, rect_bytes (t->w, t->h, format, type, pack_align, pack_rowlen));
}

static void f_ReadPixels (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels)
{
    (void) x; (void) y;
    if (pixels)
        memset (pixels, 0, rect_bytes (w, h, format, type, pack_align, pack_rowlen));
}

/* ----------------------------------------------------------- buffers */

static void f_BufferData (GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    (void) target; (void) usage;
    if (data && size > 0)
        touch (data, (size_t) size);
}

static void f_BufferSubData (GLenum target, GLintptr off, GLsizeiptr size, const void *data)
{
    (void) target; (void) off;
    if (data && size > 0)
        touch (data, (size_t) size);
}

/* ---------------------------------------------------------- programs */

static GLuint f_CreateShader (GLenum type)
{
    (void) type;
    return next_name++;
}

static GLuint f_CreateProgram (void)
{
    GLuint id = next_name++;
    Prog *p = &progs[id % NPROG];
    memset (p, 0, sizeof *p);
    p->id = id;
    return id;
}

static void f_ShaderSource (GLuint sh, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    Shader *s = &shaders[sh % (NPROG * 2)];
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++)
        total += lengths && lengths[i] >= 0 ? (size_t) lengths[i] : strlen (strings[i]);
    free (s->src);
    s->id = sh;
    s->src = malloc (total + 1);
    size_t at = 0;
    for (GLsizei i = 0; i < count; i++) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t) lengths[i] : strlen (strings[i]);
        memcpy (s->src + at, strings[i], n);
        at += n;
    }
    s->src[at] = 0;
}

static void f_DeleteShader (GLuint sh)
{
    Shader *s = &shaders[sh % (NPROG * 2)];
    if (s->id == sh) {
        free (s->src);
        s->src = NULL;
        s->id = 0;
    }
}

static void f_AttachShader (GLuint prog, GLuint sh)
{
    Prog *p = &progs[prog % NPROG];
    if (p->id == prog && p->nsh < 4)
        p->sh[p->nsh++] = sh;
}

static int is_ident (int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* "uniform <type> <name>[n];" at the start of a line: what a compiler
 * would list as active (every one of them: nothing is optimised away) */
static void f_LinkProgram (GLuint prog)
{
    static const struct { const char *word; GLenum type; } types[] = {
        { "float", GL_FLOAT }, { "vec2", GL_FLOAT_VEC2 }, { "vec3", GL_FLOAT_VEC3 },
        { "vec4", GL_FLOAT_VEC4 }, { "mat3", GL_FLOAT_MAT3 }, { "mat4", GL_FLOAT_MAT4 },
        { "int", GL_INT }, { "bool", GL_BOOL }, { "sampler2D", GL_SAMPLER_2D },
    };
    Prog *p = &progs[prog % NPROG];
    if (p->id != prog)
        return;
    p->nuni = 0;
    for (int k = 0; k < p->nsh; k++) {
        Shader *s = &shaders[p->sh[k] % (NPROG * 2)];
        if (s->id != p->sh[k] || !s->src)
            continue;
        for (const char *l = s->src; l && *l; l = strchr (l, '\n'), l = l ? l + 1 : NULL) {
            if (strncmp (l, "uniform ", 8))
                continue;
            const char *q = l + 8;
            GLenum type = 0;
            for (unsigned t = 0; t < sizeof types / sizeof *types; t++) {
                size_t n = strlen (types[t].word);
                if (!strncmp (q, types[t].word, n) && q[n] == ' ') {
                    type = types[t].type;
                    q += n + 1;
                    break;
                }
            }
            if (!type || p->nuni >= NUNI)
                continue;
            Uni *u = &p->uni[p->nuni];
            size_t n = 0;
            while (is_ident ((unsigned char) q[n]) && n < sizeof u->name - 4)
                n++;
            if (!n)
                continue;
            memcpy (u->name, q, n);
            u->name[n] = 0;
            u->type = type;
            u->size = q[n] == '[' ? atoi (q + n + 1) : 1;
            if (u->size < 1 || u->size > 64)
                u->size = 1;
            int dup = 0;
            for (int i = 0; i < p->nuni; i++)
                dup |= !strcmp (p->uni[i].name, u->name);
            if (!dup)
                p->nuni++;
        }
    }
}

static void f_GetShaderiv (GLuint sh, GLenum pname, GLint *out)
{
    (void) sh;
    *out = pname == GL_COMPILE_STATUS ? 1 : 0;
}

static void f_GetProgramiv (GLuint prog, GLenum pname, GLint *out)
{
    Prog *p = &progs[prog % NPROG];
    switch (pname) {
    case GL_LINK_STATUS: case GL_VALIDATE_STATUS: *out = 1; break;
    case GL_ACTIVE_UNIFORMS: *out = p->id == prog ? p->nuni : 0; break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: *out = 64; break;
    default: *out = 0; break;
    }
}

static void f_GetInfoLog (GLuint obj, GLsizei size, GLsizei *len, GLchar *log)
{
    (void) obj;
    if (size > 0 && log)
        log[0] = 0;
    if (len)
        *len = 0;
}

static void f_GetActiveUniform (GLuint prog, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size,
                                GLenum *type, GLchar *name)
{
    Prog *p = &progs[prog % NPROG];
    if (p->id != prog || (int) index >= p->nuni) {
        if (bufsize > 0)
            name[0] = 0;
        if (length) *length = 0;
        if (size) *size = 0;
        if (type) *type = 0;
        return;
    }
    Uni *u = &p->uni[index];
    char full[80];
    strcpy (full, u->name);
    if (u->size > 1)
        strcat (full, "[0]");
    size_t n = strlen (full);
    if (bufsize > 0) {
        if (n > (size_t) bufsize - 1)
            n = (size_t) bufsize - 1;
        memcpy (name, full, n);
        name[n] = 0;
    }
    if (length) *length = (GLsizei) n;
    if (size) *size = u->size;
    if (type) *type = u->type;
}

static GLint f_GetUniformLocation (GLuint prog, const GLchar *name)
{
    Prog *p = &progs[prog % NPROG];
    size_t n = strcspn (name, "[");
    if (p->id != prog)
        return -1;
    for (int i = 0; i < p->nuni; i++)
        if (strlen (p->uni[i].name) == n && !strncmp (p->uni[i].name, name, n))
            return i * 64;
    return -1;
}

/* a uniform's value read back: the caller's buffer holds one value of the
 * uniform's type (16 floats at most) */
static void f_GetUniformfv (GLuint prog, GLint loc, GLfloat *out)
{
    Prog *p = &progs[prog % NPROG];
    int i = loc / 64, n = 1;
    if (p->id == prog && loc >= 0 && i < p->nuni)
        switch (p->uni[i].type) {
        case GL_FLOAT_VEC2: n = 2; break;
        case GL_FLOAT_VEC3: n = 3; break;
        case GL_FLOAT_VEC4: n = 4; break;
        case GL_FLOAT_MAT3: n = 9; break;
        case GL_FLOAT_MAT4: n = 16; break;
        default: break;
        }
    for (int k = 0; k < n; k++)
        out[k] = 0;
}

static void f_GetUniformiv (GLuint prog, GLint loc, GLint *out)
{
    (void) prog; (void) loc;
    *out = 0;
}

static void f_Uniform1fv (GLint l, GLsizei n, const GLfloat *v) { (void) l; touch (v, (size_t) (n > 0 ? n : 0) * 4); }
static void f_Uniform2fv (GLint l, GLsizei n, const GLfloat *v) { (void) l; touch (v, (size_t) (n > 0 ? n : 0) * 8); }
static void f_Uniform3fv (GLint l, GLsizei n, const GLfloat *v) { (void) l; touch (v, (size_t) (n > 0 ? n : 0) * 12); }
static void f_Uniform4fv (GLint l, GLsizei n, const GLfloat *v) { (void) l; touch (v, (size_t) (n > 0 ? n : 0) * 16); }
static void f_UniformMatrix3fv (GLint l, GLsizei n, GLboolean t, const GLfloat *v) { (void) l; (void) t; touch (v, (size_t) (n > 0 ? n : 0) * 36); }
static void f_UniformMatrix4fv (GLint l, GLsizei n, GLboolean t, const GLfloat *v) { (void) l; (void) t; touch (v, (size_t) (n > 0 ? n : 0) * 64); }

/* ----------------------------------------------------------- queries */

static const GLubyte *f_GetString (GLenum name)
{
    switch (name) {
    case GL_VENDOR: return (const GLubyte *) "walpe";
    case GL_RENDERER: return (const GLubyte *) "fuzz stand-in";
    case GL_VERSION: return (const GLubyte *) "3.3 (Core Profile) fuzz";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *) "3.30";
    default: return (const GLubyte *) "";
    }
}

/* a desktop driver's usual two extensions (compressed textures go to GL as
 * they are stored: the upload path of most machines);
 * WALPE_FUZZ_GL_PLAIN=1: none */
static const char *const exts[] = { "GL_EXT_texture_compression_s3tc", "GL_EXT_texture_filter_anisotropic" };

static int n_exts (void)
{
    static int n = -1;
    if (n < 0)
        n = getenv ("WALPE_FUZZ_GL_PLAIN") ? 0 : (int) (sizeof exts / sizeof *exts);
    return n;
}

static const GLubyte *f_GetStringi (GLenum name, GLuint i)
{
    return (const GLubyte *) (name == GL_EXTENSIONS && (int) i < n_exts () ? exts[i] : "");
}

static void f_GetIntegerv (GLenum pname, GLint *out)
{
    switch (pname) {
    case GL_VIEWPORT: case GL_SCISSOR_BOX: memcpy (out, viewport, sizeof viewport); break;
    case GL_TEXTURE_BINDING_2D: *out = (GLint) bound_tex; break;
    case GL_DRAW_FRAMEBUFFER_BINDING: *out = (GLint) bound_fbo_draw; break;
    case GL_READ_FRAMEBUFFER_BINDING: *out = (GLint) bound_fbo_read; break;
    case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: *out = 16384; break;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *out = 32; break;
    case GL_PACK_ALIGNMENT: *out = pack_align; break;
    case GL_UNPACK_ROW_LENGTH: *out = unpack_rowlen; break;
    case GL_NUM_EXTENSIONS: *out = n_exts (); break;
    case GL_MAJOR_VERSION: *out = 3; break;
    case GL_MINOR_VERSION: *out = 3; break;
    default: *out = 0; break;
    }
}

static void f_GetFloatv (GLenum pname, GLfloat *out)
{
    int n = pname == GL_COLOR_CLEAR_VALUE || pname == GL_BLEND_COLOR ? 4 : 1;
    for (int i = 0; i < n; i++)
        out[i] = pname == GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT ? 16.0f : 0.0f;
}

static void f_Get1i (GLuint a, GLenum b, GLint *out) { (void) a; (void) b; *out = 0; }
static void f_Get1ui (GLuint a, GLenum pname, GLuint *out) { (void) a; *out = pname == GL_QUERY_RESULT_AVAILABLE; }
static void f_Get1ui64 (GLuint a, GLenum b, GLuint64 *out) { (void) a; (void) b; *out = 0; }
static void f_Get1e (GLenum a, GLenum b, GLint *out) { (void) a; (void) b; *out = 0; }
static void f_GetFbAtt (GLenum a, GLenum b, GLenum c, GLint *out) { (void) a; (void) b; (void) c; *out = 0; }

/* ------------------------------------------------------------ tables */

DEF (glGenTextures, f_Gen) DEF (glGenFramebuffers, f_Gen) DEF (glGenBuffers, f_Gen)
DEF (glGenVertexArrays, f_Gen) DEF (glGenQueries, f_Gen) DEF (glGenSamplers, f_Gen)
DEF (glDeleteTextures, f_DeleteTextures) DEF (glDeleteFramebuffers, f_DeleteN)
DEF (glDeleteBuffers, f_DeleteN) DEF (glDeleteVertexArrays, f_DeleteN)
DEF (glDeleteQueries, f_DeleteN) DEF (glDeleteSamplers, f_DeleteN)
DEF (glBindTexture, f_BindTexture) DEF (glBindFramebuffer, f_BindFramebuffer)
DEF (glCheckFramebufferStatus, f_CheckFramebufferStatus)
DEF (glPixelStorei, f_PixelStorei) DEF (glViewport, f_Viewport)
DEF (glTexImage2D, f_TexImage2D) DEF (glTexSubImage2D, f_TexSubImage2D)
DEF (glCompressedTexImage2D, f_CompressedTexImage2D)
DEF (glGetTexLevelParameteriv, f_GetTexLevelParameteriv)
DEF (glGetTexParameteriv, f_GetTexParameteriv) DEF (glGetTexParameterfv, f_GetTexParameterfv)
DEF (glGetTexImage, f_GetTexImage) DEF (glReadPixels, f_ReadPixels)
DEF (glBufferData, f_BufferData) DEF (glBufferSubData, f_BufferSubData)
DEF (glCreateShader, f_CreateShader) DEF (glCreateProgram, f_CreateProgram)
DEF (glShaderSource, f_ShaderSource) DEF (glDeleteShader, f_DeleteShader)
DEF (glAttachShader, f_AttachShader) DEF (glLinkProgram, f_LinkProgram)
DEF (glGetShaderiv, f_GetShaderiv) DEF (glGetProgramiv, f_GetProgramiv)
DEF (glGetShaderInfoLog, f_GetInfoLog) DEF (glGetProgramInfoLog, f_GetInfoLog)
DEF (glGetActiveUniform, f_GetActiveUniform) DEF (glGetUniformLocation, f_GetUniformLocation)
DEF (glGetUniformfv, f_GetUniformfv) DEF (glGetUniformiv, f_GetUniformiv)
DEF (glUniform1fv, f_Uniform1fv) DEF (glUniform2fv, f_Uniform2fv) DEF (glUniform3fv, f_Uniform3fv)
DEF (glUniform4fv, f_Uniform4fv) DEF (glUniformMatrix3fv, f_UniformMatrix3fv)
DEF (glUniformMatrix4fv, f_UniformMatrix4fv)
DEF (glGetString, f_GetString) DEF (glGetStringi, f_GetStringi)
DEF (glGetIntegerv, f_GetIntegerv) DEF (glGetFloatv, f_GetFloatv)
DEF (glGetQueryObjectuiv, f_Get1ui) DEF (glGetQueryObjectiv, f_Get1i)
DEF (glGetQueryObjectui64v, f_Get1ui64) DEF (glGetQueryObjectui64vEXT, f_Get1ui64)
DEF (glGetQueryiv, f_Get1e) DEF (glGetBufferParameteriv, f_Get1e)
DEF (glGetFramebufferAttachmentParameteriv, f_GetFbAtt)

NOP (glTexParameteri) NOP (glTexParameterf) NOP (glBindVertexArray) NOP (glBindBuffer)
NOP (glUniform1i) NOP (glUniform2i) NOP (glUniform1f) NOP (glUniform2f) NOP (glUniform3f)
NOP (glUniform4f) NOP (glBindAttribLocation) NOP (glGetError) NOP (glUseProgram)
NOP (glActiveTexture) NOP (glDrawArrays) NOP (glDrawElements) NOP (glDrawArraysInstanced)
NOP (glVertexAttribPointer) NOP (glEnableVertexAttribArray) NOP (glVertexAttribDivisor)
NOP (glVertexAttrib3f) NOP (glVertexAttrib4f) NOP (glBlendFunc) NOP (glBlendFuncSeparate)
NOP (glBlendEquation) NOP (glFramebufferTexture2D) NOP (glEnable) NOP (glDisable)
NOP (glIsEnabled) NOP (glClearColor) NOP (glClear) NOP (glDeleteProgram) NOP (glCompileShader)
NOP (glEndQuery) NOP (glBeginQuery) NOP (glFinish) NOP (glFlush) NOP (glBlitFramebuffer)
NOP (glSamplerParameteri) NOP (glBindSampler) NOP (glScissor) NOP (glIsTexture)
NOP (glIsFramebuffer) NOP (glIsBuffer) NOP (glGenerateMipmap) NOP (glDeleteSync)
NOP (glFenceSync) NOP (glClientWaitSync) NOP (glUnmapBuffer) NOP (glMapBufferRange)
NOP (glInvalidateFramebuffer) NOP (glColorMask) NOP (glReadBuffer) NOP (glCopyTexSubImage2D)
NOP (glBufferStorage) NOP (glDebugMessageCallback) NOP (glDebugMessageCallbackKHR)
NOP (glDebugMessageControl) NOP (glDebugMessageControlKHR)
NOP (glEGLImageTargetTexture2DOES) NOP (glEGLImageTargetTexStorageEXT)
NOP (eglGetCurrentDisplay) NOP (eglGetError) NOP (eglQueryString) NOP (eglGetProcAddress)
NOP (eglQueryDeviceStringEXT) NOP (eglQueryDisplayAttribEXT) NOP (eglQueryDeviceAttribEXT)
NOP (eglDestroyImageKHR) NOP (eglCreateImageKHR) NOP (eglSwapBuffers)

bool epoxy_is_desktop_gl (void) { return true; }
int epoxy_gl_version (void) { return 33; }
bool epoxy_has_gl_extension (const char *ext)
{
    for (int i = 0; i < n_exts (); i++)
        if (!strcmp (ext, exts[i]))
            return true;
    return false;
}
bool epoxy_has_egl_extension (EGLDisplay dpy, const char *ext) { (void) dpy; (void) ext; return false; }
int epoxy_egl_version (EGLDisplay dpy) { (void) dpy; return 15; }
