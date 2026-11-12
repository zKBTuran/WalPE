/* fuzz_tex - the .tex texture container (src/common/tex.c): headers, mip
 * levels, LZ4, DXT1/3/5, R8 / RG88, embedded PNG / JPEG / GIF, the sprite
 * frame table. The input is the file; it is loaded with the flag sets and
 * levels the renderer asks for. What a caller relies on is checked: the
 * pixel buffer holds what width, height and format say (an upload reads
 * that many bytes), and every byte of it can be read. */
#include "fuzz_common.h"
#include "common/tex.h"

static void check (const WalpeTex *t, guint flags, int level)
{
    FUZZ_CHECK (t->width > 0 && t->height > 0 && t->width <= 16384 && t->height <= 16384);
    FUZZ_CHECK (t->data != NULL);
    gsize w = (gsize) t->width, h = (gsize) t->height, need;
    if (t->format == WALPE_TEX_RGBA8) {
        FUZZ_CHECK (t->channels == 0 || ((flags & WALPE_TEX_LOAD_NATIVE_RG) &&
                                         (t->channels == 1 || t->channels == 2)));
        need = w * h * (t->channels ? (gsize) t->channels : 4);
    } else {
        FUZZ_CHECK (flags & WALPE_TEX_LOAD_NATIVE_DXT);
        FUZZ_CHECK (t->format == WALPE_TEX_DXT1 || t->format == WALPE_TEX_DXT3 ||
                    t->format == WALPE_TEX_DXT5);
        need = ((w + 3) / 4) * ((h + 3) / 4) * (t->format == WALPE_TEX_DXT1 ? 8 : 16);
    }
    FUZZ_CHECK (t->len >= need);
    fuzz_touch (t->data, t->len);
    FUZZ_CHECK (t->full_width == t->width && t->full_height == t->height);
    FUZZ_CHECK (t->mip_level == level);
    FUZZ_CHECK (t->base_width >= t->width && t->base_height >= t->height);
    FUZZ_CHECK (t->n_levels >= 0 && t->n_levels <= WALPE_TEX_MAX_LEVELS);
    FUZZ_CHECK (t->n_levels == 0 || t->format != WALPE_TEX_RGBA8);
    for (int i = 0; i < t->n_levels; i++) {
        gsize lw = (gsize) t->level[i].w, lh = (gsize) t->level[i].h;
        FUZZ_CHECK (t->level[i].w > 0 && t->level[i].h > 0 && t->level[i].data);
        FUZZ_CHECK (t->level[i].len >=
                    ((lw + 3) / 4) * ((lh + 3) / 4) * (t->format == WALPE_TEX_DXT1 ? 8 : 16));
        fuzz_touch (t->level[i].data, t->level[i].len);
    }
    FUZZ_CHECK (t->nframes >= 0 && t->nframes <= 65536);
    FUZZ_CHECK ((t->nframes == 0) == (t->frames == NULL));
    if (t->nframes) {
        fuzz_touch (t->frames, (gsize) t->nframes * sizeof (WalpeTexFrame));
        FUZZ_CHECK (t->frame_w > 0 && t->frame_h > 0);
    }
}

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    static const struct { guint flags; int level; } how[] = {
        { 0, 0 },
        { WALPE_TEX_LOAD_NATIVE_RG | WALPE_TEX_LOAD_NATIVE_DXT, 0 },
        { WALPE_TEX_LOAD_WE_COLOR, 0 },
        { 0, 1 },
        { WALPE_TEX_LOAD_NATIVE_RG | WALPE_TEX_LOAD_LEVEL_FLOOR, 1 },
        { WALPE_TEX_LOAD_LEVEL_FLOOR, 3 },
    };
    const char *path = fuzz_file (0, data, size);
    WalpeTexProbe pr;
    gboolean probed = walpe_tex_probe (path, &pr);
    if (probed) {
        FUZZ_CHECK (pr.tex_width > 0 && pr.tex_height > 0 && pr.mips >= 1 && pr.mips <= 16);
        FUZZ_CHECK (pr.levels >= 0 && pr.levels <= pr.levels_floor &&
                    pr.levels_floor <= WALPE_TEX_MAX_LEVELS + 1);
    }
    int pw = 0, ph = 0;
    if (walpe_tex_picture_size (path, &pw, &ph))
        FUZZ_CHECK (pw > 0 && ph > 0 && pw <= 16384 && ph <= 16384);
    guint64 voff = 0, vlen = 0;
    int vw = 0, vh = 0;
    if (walpe_tex_video_range (path, &voff, &vlen, &vw, &vh))
        FUZZ_CHECK (voff + vlen <= size && vlen >= 64);
    for (guint i = 0; i < G_N_ELEMENTS (how); i++) {
        WalpeTex *t = walpe_tex_load_level (path, how[i].flags, how[i].level);
        if (t) {
            check (t, how[i].flags, how[i].level);
            walpe_tex_free (t);
        }
    }
    return 0;
}
