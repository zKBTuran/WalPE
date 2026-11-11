/* videosec_test.c - hostile video files against the player.
 * No display, no GL context, no encoder: the clips are
 * written here, macroblock by macroblock.
 *
 *  1. walpe_yuv420_bgra_ex with odd sizes. H.264 4:2:2 / 4:4:4 crops by
 *     single rows, cv_normalize repacks such frames to 8-bit 4:2:0 and the
 *     CPU converter (software GL, "Render on CPU") is then handed an odd
 *     height: its 2x2 loop wrote one row past the BGRA buffer (and read a
 *     luma row / column that is not there). Guard rows around the output
 *     and a per-pixel reference check the conversion.
 *  2. The same through the player's own code: a 64 x 35 4:4:4 frame into
 *     cv_fill_frame, output buffer between guard pages.
 *  3. Picture size limit: a clip of 0.5 MB whose SPS declares 8192 x 5120
 *     (41.9 megapixels, more than any H.264 level allows) must not yield a
 *     frame - the decoder used to allocate ~63 MB per picture for it, and
 *     ~400 MB for 16384 x 16384. A 64 x 64 clip from the same writer must
 *     play (so the first result is not a broken test clip), and a clip
 *     whose container declares the absurd size is refused when opened.
 *  4. cv_threads_for with time stamps one tick apart (a load of 1e10
 *     "cores" was converted to int: undefined, INT_MIN on x86).
 *  5. A clip of predicted frames only (no keyframe): the decoder accepts
 *     every packet and never outputs a picture, and since the frame queue
 *     is the decode loop's only pacing, the loop ran through the clip as
 *     fast as a core could, for as long as the wallpaper was set. It now
 *     falls back to the clip's own pace.
 *
 * video_custom.c is included so the test reaches the decode queue.
 * Run: meson test -C build videosec -v */
#define walpe_video_custom_new test_video_custom_new
#define walpe_video_tex_new test_video_tex_new
#define walpe_video_tex_update test_video_tex_update
#define walpe_video_tex_set_view test_video_tex_set_view
#define walpe_video_tex_interval_us test_video_tex_interval_us
#define walpe_video_tex_set_paused test_video_tex_set_paused
#define walpe_video_tex_free test_video_tex_free
#define walpe_video_module_entry test_video_module_entry
#include "engine/video_custom.c"
#include <glib/gstdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                 failures++; } } while (0)

/* ------------------------------------------------------------- 1. yuvconv */

static guint8 ref_clamp (int t) { return (guint8) (t < 0 ? 0 : t > 255 ? 255 : t); }

static void yuv_case (int w, int h, guint mode)
{
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    int stride = (w * 4 + 63) & ~63;
    /* planes of exactly the size the format has (a sanitizer build sees
     * any read outside them) */
    guint8 *y = g_malloc ((gsize) w * h), *u = g_malloc ((gsize) cw * ch), *v = g_malloc ((gsize) cw * ch);
    for (int i = 0; i < w * h; i++)
        y[i] = (guint8) (16 + (i * 7) % 220);
    for (int i = 0; i < cw * ch; i++) {
        u[i] = (guint8) (30 + (i * 13) % 200);
        v[i] = (guint8) (220 - (i * 11) % 200);
    }
    const int guard = 3;    /* rows before and after the picture */
    gsize total = (gsize) stride * (h + 2 * guard);
    guint8 *buf = g_malloc (total);
    memset (buf, 0xA5, total);
    guint8 *dst = buf + (gsize) stride * guard;
    walpe_yuv420_bgra_ex (y, u, v, w, cw, dst, stride, w, h, mode);
    int bad_guard = 0, bad_pad = 0, bad_px = 0;
    for (gsize i = 0; i < (gsize) stride * guard; i++)
        bad_guard += buf[i] != 0xA5 || buf[total - 1 - i] != 0xA5;
    int k[6];
    walpe_yuv_coeffs (mode, k);
    for (int r = 0; r < h; r++) {
        for (int c = w * 4; c < stride; c++)
            bad_pad += dst[(gsize) r * stride + c] != 0xA5;
        for (int c = 0; c < w; c++) {
            int Y = y[r * w + c] - k[1];
            if (Y < 0)
                Y = 0;
            Y *= k[0];
            int U = u[(r / 2) * cw + c / 2] - 128, V = v[(r / 2) * cw + c / 2] - 128;
            const guint8 *d = dst + (gsize) r * stride + c * 4;
            bad_px += d[2] != ref_clamp ((Y + k[2] * V + 128) >> 8) ||
                      d[1] != ref_clamp ((Y - k[3] * U - k[4] * V + 128) >> 8) ||
                      d[0] != ref_clamp ((Y + k[5] * U + 128) >> 8) || d[3] != 255;
        }
    }
    if (bad_guard || bad_pad || bad_px) {
        g_printerr ("FAIL yuv %dx%d mode %u: %d bytes written outside the picture rows, %d in the "
                    "row padding, %d wrong pixels\n", w, h, mode, bad_guard, bad_pad, bad_px);
        failures++;
    }
    g_free (buf);
    g_free (y);
    g_free (u);
    g_free (v);
}

static void test_yuv (void)
{
    static const int sizes[][2] = { { 2, 2 }, { 64, 36 }, { 1, 1 }, { 3, 2 }, { 2, 3 }, { 3, 3 },
                                    { 64, 35 }, { 65, 36 }, { 65, 35 }, { 7, 5 }, { 1, 9 }, { 9, 1 } };
    for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
        for (guint mode = 0; mode < 4; mode++)
            yuv_case (sizes[i][0], sizes[i][1], mode);
}

/* ------------------------------------------- 2. cv_fill_frame, odd 4:4:4 */

static void fill_case (enum AVPixelFormat fmt, int w, int h)
{
    CvSource *src = g_new0 (CvSource, 1);
    src->path = g_strdup ("videosec-fill");
    src->gpu_yuv = FALSE;       /* the CPU path: software GL / WALPE_CPU_YUV */
    src->frame = av_frame_alloc ();
    src->frame->format = fmt;
    src->frame->width = w;
    src->frame->height = h;
    CHECK (av_frame_get_buffer (src->frame, 0) >= 0);
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get (fmt);
    for (int pl = 0; pl < 3; pl++) {
        int ph = pl ? AV_CEIL_RSHIFT (h, d->log2_chroma_h) : h;
        for (int r = 0; r < ph; r++)
            memset (src->frame->data[pl] + (gssize) r * src->frame->linesize[pl], 60 + 40 * pl + r,
                    (gsize) src->frame->linesize[pl]);
    }
    CvFrame f;
    memset (&f, 0, sizeof f);
    f.av = av_frame_alloc ();
    /* the output buffer the player would allocate (stride * h), handed in
     * with guard areas around it */
    int stride = (w * 4 + 63) & ~63;
    gsize need = (gsize) stride * h, guard = 4096;
    guint8 *buf = g_malloc (need + 2 * guard);
    memset (buf, 0xA5, need + 2 * guard);
    f.data = buf + guard;
    f.cap = need;
    double copied = 0;
    gboolean ok = cv_fill_frame (src, &f, &copied);
    CHECK (ok);
    CHECK (f.data == buf + guard);      /* (the test's buffer was used) */
    CHECK (f.kind == CV_BGRA && f.w == w && f.h == h);
    gsize outside = 0;
    for (gsize i = 0; i < guard; i++)
        outside += (buf[i] != 0xA5) + (buf[guard + need + i] != 0xA5);
    if (outside) {
        g_printerr ("FAIL %s %dx%d: %" G_GSIZE_FORMAT " bytes written outside the frame buffer\n",
                    av_get_pix_fmt_name (fmt), w, h, outside);
        failures++;
    }
    int alpha_bad = 0;
    for (int r = 0; ok && r < h; r++)
        for (int c = 0; c < w; c++)
            alpha_bad += f.pl[0][(gsize) r * f.ls[0] + c * 4 + 3] != 255;
    CHECK (alpha_bad == 0);     /* every pixel of the picture was converted */
    av_frame_free (&f.av);
    av_frame_free (&src->frame);
    g_free (src->norm_buf);
    g_free (src->path);
    g_free (src);
    g_free (buf);
}

static void test_fill (void)
{
    fill_case (AV_PIX_FMT_YUV444P, 64, 36);
    fill_case (AV_PIX_FMT_YUV444P, 64, 35);     /* what `ffmpeg -pix_fmt yuv444p -s 64x35` makes */
    fill_case (AV_PIX_FMT_YUV444P, 65, 35);
    fill_case (AV_PIX_FMT_YUV422P, 64, 35);
    fill_case (AV_PIX_FMT_YUV444P10LE, 63, 33);
}

/* ------------------------------------------------- 3. hand-written clips */

typedef struct { GByteArray *b; guint cur; int n; } BitW;

static void bw_put (BitW *w, guint32 v, int bits)
{
    for (int i = bits - 1; i >= 0; i--) {
        w->cur = (w->cur << 1) | ((v >> i) & 1);
        if (++w->n == 8) {
            guint8 c = (guint8) w->cur;
            g_byte_array_append (w->b, &c, 1);
            w->cur = 0;
            w->n = 0;
        }
    }
}

static void bw_ue (BitW *w, guint32 v)      /* Exp-Golomb */
{
    guint32 x = v + 1;
    int len = 0;
    for (guint32 t = x; t > 1; t >>= 1)
        len++;
    bw_put (w, 0, len);
    bw_put (w, x, len + 1);
}

/* stop bit + alignment, then the NAL unit: header byte + the payload with
 * emulation prevention bytes */
static GByteArray *bw_nal (BitW *w, guint8 header)
{
    bw_put (w, 1, 1);
    while (w->n)
        bw_put (w, 0, 1);
    GByteArray *o = g_byte_array_new ();
    g_byte_array_append (o, &header, 1);
    int zeros = 0;
    for (guint i = 0; i < w->b->len; i++) {
        guint8 c = w->b->data[i];
        if (zeros >= 2 && c <= 3) {
            guint8 e = 3;
            g_byte_array_append (o, &e, 1);
            zeros = 0;
        }
        g_byte_array_append (o, &c, 1);
        zeros = c ? 0 : zeros + 1;
    }
    g_byte_array_unref (w->b);
    return o;
}

/* Constrained Baseline, wmb x hmb macroblocks, frames only, no VUI */
static GByteArray *h264_sps (guint wmb, guint hmb)
{
    BitW w = { g_byte_array_new (), 0, 0 };
    bw_put (&w, 66, 8);         /* profile_idc */
    bw_put (&w, 0xC0, 8);       /* constraint_set0 + 1 */
    bw_put (&w, 62, 8);         /* level_idc */
    bw_ue (&w, 0);              /* seq_parameter_set_id */
    bw_ue (&w, 0);              /* log2_max_frame_num_minus4 */
    bw_ue (&w, 2);              /* pic_order_cnt_type */
    bw_ue (&w, 1);              /* max_num_ref_frames */
    bw_put (&w, 0, 1);          /* gaps_in_frame_num_value_allowed_flag */
    bw_ue (&w, wmb - 1);
    bw_ue (&w, hmb - 1);
    bw_put (&w, 1, 1);          /* frame_mbs_only_flag */
    bw_put (&w, 1, 1);          /* direct_8x8_inference_flag */
    bw_put (&w, 0, 1);          /* frame_cropping_flag */
    bw_put (&w, 0, 1);          /* vui_parameters_present_flag */
    return bw_nal (&w, 0x67);
}

static GByteArray *h264_pps (void)
{
    BitW w = { g_byte_array_new (), 0, 0 };
    bw_ue (&w, 0);              /* pic_parameter_set_id */
    bw_ue (&w, 0);              /* seq_parameter_set_id */
    bw_put (&w, 0, 1);          /* entropy_coding_mode_flag: CAVLC */
    bw_put (&w, 0, 1);          /* bottom_field_pic_order_in_frame_present_flag */
    bw_ue (&w, 0);              /* num_slice_groups_minus1 */
    bw_ue (&w, 0);              /* num_ref_idx_l0_default_active_minus1 */
    bw_ue (&w, 0);              /* num_ref_idx_l1_default_active_minus1 */
    bw_put (&w, 0, 1);          /* weighted_pred_flag */
    bw_put (&w, 0, 2);          /* weighted_bipred_idc */
    bw_ue (&w, 0);              /* pic_init_qp_minus26 (se) */
    bw_ue (&w, 0);              /* pic_init_qs_minus26 (se) */
    bw_ue (&w, 0);              /* chroma_qp_index_offset (se) */
    bw_put (&w, 1, 1);          /* deblocking_filter_control_present_flag */
    bw_put (&w, 0, 1);          /* constrained_intra_pred_flag */
    bw_put (&w, 0, 1);          /* redundant_pic_cnt_present_flag */
    return bw_nal (&w, 0x68);
}

/* one IDR picture: every macroblock Intra 16x16 with DC prediction and no
 * residual (one byte each), deblocking off: a flat grey picture */
static GByteArray *h264_idr (guint wmb, guint hmb, guint idr_pic_id)
{
    BitW w = { g_byte_array_new (), 0, 0 };
    bw_ue (&w, 0);              /* first_mb_in_slice */
    bw_ue (&w, 7);              /* slice_type: I */
    bw_ue (&w, 0);              /* pic_parameter_set_id */
    bw_put (&w, 0, 4);          /* frame_num */
    bw_ue (&w, idr_pic_id);
    bw_put (&w, 0, 1);          /* no_output_of_prior_pics_flag */
    bw_put (&w, 0, 1);          /* long_term_reference_flag */
    bw_ue (&w, 0);              /* slice_qp_delta (se) */
    bw_ue (&w, 1);              /* disable_deblocking_filter_idc */
    for (guint i = 0; i < wmb * hmb; i++) {
        bw_ue (&w, 3);          /* mb_type I_16x16_2_0_0 */
        bw_ue (&w, 0);          /* intra_chroma_pred_mode: DC */
        bw_ue (&w, 0);          /* mb_qp_delta (se) */
        bw_put (&w, 1, 1);      /* coeff_token: no coefficients */
    }
    return bw_nal (&w, 0x65);
}

/* one P picture, every macroblock skipped (a copy of the picture before
 * it - which a clip made only of these never has) */
static GByteArray *h264_pskip (guint wmb, guint hmb, guint frame_num)
{
    BitW w = { g_byte_array_new (), 0, 0 };
    bw_ue (&w, 0);              /* first_mb_in_slice */
    bw_ue (&w, 5);              /* slice_type: P */
    bw_ue (&w, 0);              /* pic_parameter_set_id */
    bw_put (&w, frame_num & 15, 4);
    bw_put (&w, 0, 1);          /* num_ref_idx_active_override_flag */
    bw_put (&w, 0, 1);          /* ref_pic_list_modification_flag_l0 */
    bw_put (&w, 0, 1);          /* adaptive_ref_pic_marking_mode_flag */
    bw_ue (&w, 0);              /* slice_qp_delta (se) */
    bw_ue (&w, 1);              /* disable_deblocking_filter_idc */
    bw_ue (&w, wmb * hmb);      /* mb_skip_run: all of them */
    return bw_nal (&w, 0x61);
}

static void put32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) (v >> 24), (guint8) (v >> 16), (guint8) (v >> 8), (guint8) v };
    g_byte_array_append (a, b, 4);
}

static void put16 (GByteArray *a, guint16 v)
{
    guint8 b[2] = { (guint8) (v >> 8), (guint8) v };
    g_byte_array_append (a, b, 2);
}

static void put0 (GByteArray *a, guint n)
{
    static const guint8 z[64];
    while (n) {
        guint k = MIN (n, (guint) sizeof z);
        g_byte_array_append (a, z, k);
        n -= k;
    }
}

/* box `type` holding body (taken); full: version / flags word first */
static GByteArray *mk_box (const char *type, GByteArray *body, gint64 full)
{
    GByteArray *o = g_byte_array_new ();
    put32 (o, 8 + (full >= 0 ? 4 : 0) + body->len);
    g_byte_array_append (o, (const guint8 *) type, 4);
    if (full >= 0)
        put32 (o, (guint32) full);
    g_byte_array_append (o, body->data, body->len);
    g_byte_array_unref (body);
    return o;
}

static void take (GByteArray *into, GByteArray *part)
{
    g_byte_array_append (into, part->data, part->len);
    g_byte_array_unref (part);
}

static void put_matrix (GByteArray *a)
{
    static const guint32 m[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 };
    for (int i = 0; i < 9; i++)
        put32 (a, m[i]);
}

/* an MP4 with n pictures of wmb x hmb macroblocks (IDR pictures, or with
 * p_only predicted ones); the sample entry declares decl_w x decl_h */
static gboolean p_only;
static char *write_clip (const char *dir, const char *name, guint wmb, guint hmb, guint n,
                         guint decl_w, guint decl_h)
{
    GByteArray *sps = h264_sps (wmb, hmb), *pps = h264_pps ();
    GByteArray *file = g_byte_array_new ();
    /* ftyp */
    put32 (file, 24);
    g_byte_array_append (file, (const guint8 *) "ftypisom\0\0\0\0isomavc1", 20);
    /* mdat first: its payload starts at byte 32 */
    GByteArray *mdat = g_byte_array_new (), *sizes = g_byte_array_new ();
    for (guint i = 0; i < n; i++) {
        GByteArray *s = p_only ? h264_pskip (wmb, hmb, i) : h264_idr (wmb, hmb, i & 1);
        put32 (mdat, s->len);
        g_byte_array_append (mdat, s->data, s->len);
        put32 (sizes, s->len + 4);
        g_byte_array_unref (s);
    }
    take (file, mk_box ("mdat", mdat, -1));

    GByteArray *avcc = g_byte_array_new ();
    static const guint8 avcc_head[6] = { 1, 66, 0xC0, 62, 0xFF, 0xE1 };
    g_byte_array_append (avcc, avcc_head, 6);
    put16 (avcc, (guint16) sps->len);
    g_byte_array_append (avcc, sps->data, sps->len);
    guint8 one = 1;
    g_byte_array_append (avcc, &one, 1);
    put16 (avcc, (guint16) pps->len);
    g_byte_array_append (avcc, pps->data, pps->len);
    g_byte_array_unref (sps);
    g_byte_array_unref (pps);

    GByteArray *e = g_byte_array_new ();    /* VisualSampleEntry */
    put0 (e, 6);
    put16 (e, 1);
    put0 (e, 16);
    put16 (e, (guint16) decl_w);
    put16 (e, (guint16) decl_h);
    put32 (e, 0x480000);
    put32 (e, 0x480000);
    put32 (e, 0);
    put16 (e, 1);
    put0 (e, 32);
    put16 (e, 24);
    put16 (e, 0xffff);
    take (e, mk_box ("avcC", avcc, -1));
    GByteArray *stsd = g_byte_array_new ();
    put32 (stsd, 1);
    take (stsd, mk_box ("avc1", e, -1));

    GByteArray *stts = g_byte_array_new (), *stsc = g_byte_array_new (), *stsz = g_byte_array_new (),
               *stco = g_byte_array_new ();
    put32 (stts, 1);
    put32 (stts, n);
    put32 (stts, 33);
    put32 (stsc, 1);
    put32 (stsc, 1);
    put32 (stsc, n);
    put32 (stsc, 1);
    put32 (stsz, 0);
    put32 (stsz, n);
    take (stsz, sizes);
    put32 (stco, 1);
    put32 (stco, 32);
    GByteArray *stbl = g_byte_array_new ();
    take (stbl, mk_box ("stsd", stsd, 0));
    take (stbl, mk_box ("stts", stts, 0));
    take (stbl, mk_box ("stsc", stsc, 0));
    take (stbl, mk_box ("stsz", stsz, 0));
    take (stbl, mk_box ("stco", stco, 0));

    GByteArray *vmhd = g_byte_array_new (), *dref = g_byte_array_new (), *dinf = g_byte_array_new (),
               *minf = g_byte_array_new ();
    put0 (vmhd, 8);
    put32 (dref, 1);
    take (dref, mk_box ("url ", g_byte_array_new (), 1));
    take (dinf, mk_box ("dref", dref, 0));
    take (minf, mk_box ("vmhd", vmhd, 1));
    take (minf, mk_box ("dinf", dinf, -1));
    take (minf, mk_box ("stbl", stbl, -1));

    GByteArray *mdhd = g_byte_array_new (), *hdlr = g_byte_array_new (), *mdia = g_byte_array_new ();
    put32 (mdhd, 0);
    put32 (mdhd, 0);
    put32 (mdhd, 1000);
    put32 (mdhd, 33 * n);
    put16 (mdhd, 0x55c4);
    put16 (mdhd, 0);
    put0 (hdlr, 4);
    g_byte_array_append (hdlr, (const guint8 *) "vide", 4);
    put0 (hdlr, 12);
    g_byte_array_append (hdlr, (const guint8 *) "v", 2);
    take (mdia, mk_box ("mdhd", mdhd, 0));
    take (mdia, mk_box ("hdlr", hdlr, 0));
    take (mdia, mk_box ("minf", minf, -1));

    GByteArray *tkhd = g_byte_array_new (), *trak = g_byte_array_new ();
    put32 (tkhd, 0);
    put32 (tkhd, 0);
    put32 (tkhd, 1);
    put32 (tkhd, 0);
    put32 (tkhd, 33 * n);
    put0 (tkhd, 16);
    put_matrix (tkhd);
    put32 (tkhd, decl_w << 16);
    put32 (tkhd, decl_h << 16);
    take (trak, mk_box ("tkhd", tkhd, 7));
    take (trak, mk_box ("mdia", mdia, -1));

    GByteArray *mvhd = g_byte_array_new (), *moov = g_byte_array_new ();
    put32 (mvhd, 0);
    put32 (mvhd, 0);
    put32 (mvhd, 1000);
    put32 (mvhd, 33 * n);
    put32 (mvhd, 0x10000);
    put16 (mvhd, 0x100);
    put0 (mvhd, 10);
    put_matrix (mvhd);
    put0 (mvhd, 24);
    put32 (mvhd, 2);
    take (moov, mk_box ("mvhd", mvhd, 0));
    take (moov, mk_box ("trak", trak, -1));
    take (file, mk_box ("moov", moov, -1));

    char *path = g_build_filename (dir, name, NULL);
    gboolean ok = g_file_set_contents (path, (const char *) file->data, (gssize) file->len, NULL);
    CHECK (ok);
    g_byte_array_unref (file);
    return path;
}

/* resident memory of this process, MiB */
static double rss_mb (void)
{
    char *s = NULL;
    double mb = 0;
    if (g_file_get_contents ("/proc/self/statm", &s, NULL, NULL)) {
        long pages = 0, res = 0;
        if (sscanf (s, "%ld %ld", &pages, &res) == 2)
            mb = (double) res * (double) sysconf (_SC_PAGESIZE) / 1048576.0;
        g_free (s);
    }
    return mb;
}

/* play path for up to `wait_s` seconds; *w x *h = the first frame's size.
 * Returns whether a frame was queued. */
static guint play_passes;       /* how often the clip was read through */
static gboolean play (const char *path, double wait_s, int *w, int *h, gint64 *dec_max_pixels)
{
    GError *err = NULL;
    WalpeVideo *v = test_video_custom_new (path, &err);
    CHECK (v != NULL);
    if (!v) {
        g_printerr ("  open failed: %s\n", err ? err->message : "?");
        g_clear_error (&err);
        return FALSE;
    }
    CvSource *src = ((CvViewer *) v)->src;
    gboolean got = FALSE;
    gint64 end = g_get_monotonic_time () + (gint64) (wait_s * 1e6);
    while (!got && g_get_monotonic_time () < end) {
        g_mutex_lock (&src->lock);
        got = src->has_frame && src->nq > 0;
        if (got) {
            *w = src->q[0]->w;
            *h = src->q[0]->h;
        }
        if (src->dec)
            *dec_max_pixels = src->dec->max_pixels;
        play_passes = (guint) g_atomic_int_get (&src->passes);  /* (the decode thread's counter) */
        g_mutex_unlock (&src->lock);
        if (!got)
            g_usleep (20000);
    }
    v->free (v);
    return got;
}

static void test_limit (const char *dir)
{
    int w = 0, h = 0;
    gint64 mp = 0;
    /* the writer makes clips the decoder plays */
    char *small = write_clip (dir, "small.mp4", 4, 4, 5, 64, 64);
    gboolean got = play (small, 20, &w, &h, &mp);
    CHECK (got);
    CHECK (w == 64 && h == 64);
    CHECK (mp > 0 && mp <= 40 * 1000 * 1000);   /* the decoder has a picture size limit */
    g_free (small);

    /* 8192 x 5120 in the stream, 64 x 64 in the container: no frame, no
     * frame-sized allocation */
    char *big = write_clip (dir, "big.mp4", 512, 320, 3, 64, 64);
    double before = rss_mb ();
    w = h = 0;
    got = play (big, 4, &w, &h, &mp);
    double grew = rss_mb () - before;
    if (got)
        g_printerr ("FAIL: a %d x %d picture was decoded and queued (%.0f MiB more resident "
                    "memory)\n", w, h, grew);
    CHECK (!got);
    CHECK (grew < 48);
    g_free (big);

    /* 5. predicted frames only: no picture ever, and no race through the
     * clip either. 8 pictures a pass; at the clip's 30 fps, 2.5 s are
     * ~10 passes after the 8 the loop needs to notice (the earlier build: tens of
     * thousands) */
    p_only = TRUE;
    char *ponly = write_clip (dir, "ponly.mp4", 8, 8, 8, 128, 128);
    p_only = FALSE;
    w = h = 0;
    got = play (ponly, 2.5, &w, &h, &mp);
    CHECK (!got);
    if (play_passes > 100)
        g_printerr ("FAIL: a clip that never yields a picture was read through %u times in "
                    "2.5 s (unpaced decode loop)\n", play_passes);
    CHECK (play_passes <= 100);
    g_free (ponly);

    /* an absurd size in the container is refused at once, with the reason */
    char *decl = write_clip (dir, "declared.mp4", 4, 4, 3, 16384, 16384);
    GError *err = NULL;
    WalpeVideo *v = test_video_custom_new (decl, &err);
    CHECK (v == NULL && err != NULL);
    if (v)
        v->free (v);
    else if (err)
        CHECK (strstr (err->message, "16384") != NULL);
    g_clear_error (&err);
    g_free (decl);
}

/* -------------------------------------------------------- 4. thread count */

static void test_threads (void)
{
    /* two large samples a tick apart at the largest timescale: 4.29e9
     * "fps", a bit rate of 3e18 - 3.5e10 "cores" of work */
    Mp4Sample smp[2] = { { .size = 100000000, .pts = 0 },
                         { .size = 100000000, .pts = 1.0 / 4294967295.0 } };
    Mp4Demux d = { .width = 3840, .height = 2160, .timescale = 4294967295u, .samples = smp,
                   .nsamples = 2 };
    int max = 0;
    /* (the merged tree's cv_threads_for takes the source: its demuxer and,
     * for what an earlier run learned, its path - none here) */
    CvSource cs = { .demux = &d };
    int n = cv_threads_for (&cs, &max);
    CHECK (max >= 1 && n >= 1 && n <= max);
    if (max > 1)
        CHECK (n == max);       /* the heaviest clip gets all it may have, not one thread */
    /* an ordinary clip is unchanged: 1080p30 at 8 Mbit/s decodes on one thread */
    Mp4Sample s2[2] = { { .size = 33333, .pts = 0 }, { .size = 33333, .pts = 1.0 / 30.0 } };
    Mp4Demux d2 = { .width = 1920, .height = 1080, .timescale = 30000, .samples = s2, .nsamples = 2 };
    CvSource cs2 = { .demux = &d2 };
    n = cv_threads_for (&cs2, &max);
    CHECK (n == 1);
}

int main (void)
{
    g_setenv ("WALPE_GPU_YUV", "1", TRUE);      /* planes as decoded: no GL is asked anything */
    g_unsetenv ("WALPE_VIDEO_THREADS");
    g_unsetenv ("WALPE_VIDEO_HWDEC");
    char *dir = g_dir_make_tmp ("walpe-videosec-XXXXXX", NULL);
    test_yuv ();
    test_fill ();
    test_threads ();
    test_limit (dir);
    /* tidy up (the files this test wrote, by name) */
    static const char *const names[] = { "small.mp4", "big.mp4", "declared.mp4", "ponly.mp4" };
    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        char *p = g_build_filename (dir, names[i], NULL);
        g_remove (p);
        g_free (p);
    }
    g_rmdir (dir);
    g_free (dir);
    if (failures)
        g_printerr ("videosec: %d check(s) failed\n", failures);
    else
        g_print ("videosec: ok\n");
    return failures ? 1 : 0;
}
