/* video_test.c - the video module (src/engine/video_custom.c), compiled into
 * the test so that its internals can be asked directly.
 *
 * 1. The 2x2 box filter of the half-size path: the SSE2 and AVX2 row kernels
 *    (one component and two interleaved components) give exactly the bytes
 *    of a plain reference loop, for odd sizes and every remainder.
 * 2. The frame limit ("video_limit", off by default): the grid rule, which
 *    samples may be left undecoded, the rate taken from the viewers.
 * 3. With a clip made here (ffmpeg + libx264: 1280x720, 60 fps, I BBB P with
 *    B pictures nothing is predicted from; skipped when ffmpeg cannot make
 *    it): every frame the module queues is byte-identical to a plain
 *    libavcodec decode of the same picture - at full size, through the
 *    half-size path, in the NV12 layout of hardware decoders
 *    (WALPE_VIDEO_TEST_NV12=1: meson runs the test a second time that way) -
 *    with the limit off (every picture, in order, pass after pass) and on
 *    (exactly the pictures of the grid, the others not decoded or not
 *    queued, the clip as long as before).
 * 4. Hardware decoding asked for where no device exists: software decoding
 *    takes over, the pictures are the same and the status line says why. On
 *    a machine with a working device the pictures of the hardware decoder
 *    are compared instead and the result is printed.
 *    Then the attempts are played through with two stand-in "hardware
 *    decoders" (the software decoder behind the module's hardware path,
 *    asked for by the name "fake"): the first refuses the stream before a
 *    picture - the second is tried; that one delivers 40 pictures and then
 *    fails for good - software decoding takes over in the middle of the
 *    clip. Every picture is queued exactly once, in order, identical to the
 *    plain decode, with unbroken presentation times, and the status line
 *    says "hardware" while the stand-in decodes and names both failures
 *    afterwards.
 * 5. With a GL context (EGL device or Mesa surfaceless; skipped without):
 *    the plane textures hold exactly the queued frame after an upload, and a
 *    scene-like viewer polled at 30 Hz is measured as such. */
#include "walpe-build-config.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <epoxy/egl.h>

/* the module's public names exist in the core too (modload.c forwards them) */
#define walpe_video_custom_new t_video_custom_new
#define walpe_video_tex_new t_video_tex_new
#define walpe_video_tex_update t_video_tex_update
#define walpe_video_tex_interval_us t_video_tex_interval_us
#define walpe_video_tex_set_view t_video_tex_set_view
#define walpe_video_tex_set_paused t_video_tex_set_paused
#define walpe_video_tex_free t_video_tex_free
#define walpe_video_module_entry t_video_module_entry

static void test_pushed (void *src, const void *frame);
#define CV_TEST_PUSHED(src, f) test_pushed (src, f)
/* two stand-in hardware decoders, tried by the name "fake" only, and what
 * they do with each picture (test_hw_fake) */
#define CV_TEST_HWFAKE 1
static int test_hw_receive (void *src, int rc);
#define CV_TEST_HW_RECEIVE(src, rc) test_hw_receive (src, rc)
#include "../src/engine/video_custom.c"

static int fails;
static int slow = 1;    /* VIDEO_TEST_SLOW=n: time limits n times as long (valgrind) */
static gboolean only_upload;    /* --upload: the clip and the GL upload paths only */
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fails++; \
            printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf (__VA_ARGS__); \
            printf ("\n"); \
        } \
    } while (0)

/* ------------------------------------------------------- 1. box filter */

static void ref_box (const guint8 *s, int ss, int sw, int sh, guint8 *d, int ds, int dw, int dh, int nc)
{
    for (int y = 0; y < dh; y++) {
        int y0 = MIN (2 * y, sh - 1), y1 = MIN (2 * y + 1, sh - 1);
        for (int x = 0; x < dw; x++) {
            int x0 = MIN (2 * x, sw - 1), x1 = MIN (2 * x + 1, sw - 1);
            for (int c = 0; c < nc; c++) {
                int sum = s[(gsize) y0 * ss + x0 * nc + c] + s[(gsize) y0 * ss + x1 * nc + c] +
                          s[(gsize) y1 * ss + x0 * nc + c] + s[(gsize) y1 * ss + x1 * nc + c];
                d[(gsize) y * ds + x * nc + c] = (guint8) ((sum + 2) / 4);
            }
        }
    }
}

static void test_box (void)
{
    static const int widths[] = { 1, 2, 3, 5, 16, 31, 32, 33, 47, 63, 64, 65, 66, 127, 129, 130, 258, 1280, 1921 };
    static const int heights[] = { 1, 2, 3, 7, 16 };
    GRand *r = g_rand_new_with_seed (7);
    int levels = 1, cases = 0;
#if defined(__SSE2__)
    levels = 2;
#endif
#ifdef CV_BOX_AVX2
    if (__builtin_cpu_supports ("avx2"))
        levels = 3;
#endif
    for (guint wi = 0; wi < G_N_ELEMENTS (widths); wi++)
        for (guint hi = 0; hi < G_N_ELEMENTS (heights); hi++)
            for (int nc = 1; nc <= 2; nc++) {
                int sw = widths[wi], sh = heights[hi];
                int ss = sw * nc + (int) g_rand_int_range (r, 0, 9);    /* a stride with padding */
                /* exactly up to the last sample of the last row: a kernel that
                 * reads past it is caught by valgrind / a sanitizer build
                 * (`valgrind ./build/video-test --box`) */
                gsize len = (gsize) ss * (sh - 1) + (gsize) sw * nc;
                guint8 *s = g_malloc (len);
                for (gsize i = 0; i < len; i++)
                    s[i] = (guint8) (g_rand_int (r) >> (g_rand_boolean (r) ? 24 : 31) | (i % 7 == 0 ? 0xfe : 0));
                /* both sizes the module asks for: floor (4:2:0 luma of an odd
                 * picture) and ceil (its chroma) */
                for (int up = 0; up <= 1; up++) {
                    int dw = up ? (sw + 1) / 2 : sw / 2, dh = up ? (sh + 1) / 2 : sh / 2;
                    if (dw < 1 || dh < 1)
                        continue;
                    int ds = dw * nc;
                    guint8 *want = g_malloc ((gsize) ds * dh), *got = g_malloc ((gsize) ds * dh);
                    ref_box (s, ss, sw, sh, want, ds, dw, dh, nc);
                    for (int lv = 0; lv < levels; lv++) {
                        memset (got, 0xa5, (gsize) ds * dh);
                        cv_box_forced = lv;
                        box2 (s, ss, sw, sh, got, ds, dw, dh, nc);
                        cases++;
                        CHECK (!memcmp (want, got, (gsize) ds * dh),
                               "box filter level %d, %d component(s), %dx%d -> %dx%d differs from the "
                               "reference", lv, nc, sw, sh, dw, dh);
                    }
                    g_free (want);
                    g_free (got);
                }
                g_free (s);
            }
    cv_box_forced = -1;
    g_rand_free (r);
    printf ("box filter: %d comparisons at %d kernel level(s) (0 plain C, 1 SSE2, 2 AVX2)\n", cases, levels);
}

/* ------------------------------------------------------ 2. frame limit */

static Mp4Sample mk_sample (GByteArray *b, int n, ...)
{
    /* n NAL units: (header byte, payload length) pairs */
    va_list ap;
    va_start (ap, n);
    g_byte_array_set_size (b, 0);
    for (int i = 0; i < n; i++) {
        int hdr = va_arg (ap, int), len = va_arg (ap, int);
        guint8 l4[4] = { 0, 0, (guint8) ((len + 1) >> 8), (guint8) (len + 1) };
        g_byte_array_append (b, l4, 4);
        guint8 h = (guint8) hdr;
        g_byte_array_append (b, &h, 1);
        for (int k = 0; k < len; k++)
            g_byte_array_append (b, (const guint8 *) "\x55", 1);
    }
    va_end (ap);
    Mp4Sample s = { .data = b->data, .size = b->len };
    return s;
}

/* a scene viewer that asked n times at `rate` a second (as cv_viewer_upload records it) */
static void set_ticks (CvViewer *vw, double rate, int n)
{
    vw->tick_n = 0;
    vw->tick_i = 5;     /* anywhere in the ring */
    vw->tick_rate = 0;
    for (int i = 0; i < n; i++)
        cv_viewer_tick (vw, 100.0 + i / rate);
}

static void test_limit_rules (void)
{
    /* the grid: 30 of 60 = every second picture, the first one always */
    double iv = 1.0 / 60.0;
    int kept = 0;
    for (int k = 0; k < 600; k++) {
        gboolean keep = cv_grid_keeps (k * iv, iv, 30.0);
        CHECK (keep == (k % 2 == 0), "30 of 60: picture %d %s", k, keep ? "kept" : "left out");
        kept += keep;
    }
    CHECK (kept == 300, "30 of 60 keeps %d of 600", kept);
    /* 60 of 144: 60 a second, never two left out next to a kept run of more than 3 */
    iv = 1.0 / 144.0;
    kept = 0;
    int gap = 0, maxgap = 0;
    for (int k = 0; k < 1440; k++) {
        if (cv_grid_keeps (k * iv, iv, 60.0)) {
            kept++;
            gap = 0;
        } else {
            gap++;
            maxgap = MAX (maxgap, gap);
        }
    }
    CHECK (kept == 600, "60 of 144 keeps %d of 1440 (10 s)", kept);
    CHECK (maxgap == 2, "60 of 144 leaves out up to %d pictures in a row", maxgap);
    /* 24 of 60, 1 of 60, time stamps with rounding noise */
    iv = 1.0 / 60.0;
    kept = 0;
    for (int k = 0; k < 600; k++)
        kept += cv_grid_keeps (k * iv + (k % 3 - 1) * 1e-7, iv, 24.0);
    CHECK (kept == 240, "24 of 60 keeps %d of 600", kept);
    kept = 0;
    for (int k = 0; k < 600; k++)
        kept += cv_grid_keeps (k * iv, iv, 1.0);
    CHECK (kept == 10, "1 of 60 keeps %d of 600", kept);
    CHECK (cv_grid_keeps (0.5, iv, 0) && cv_grid_keeps (0.5, 0, 30), "no rate / no interval keeps everything");

    /* which samples may be left undecoded */
    GByteArray *b = g_byte_array_new ();
    Mp4Sample s;
    s = mk_sample (b, 1, 0x01, 40);
    CHECK (cv_sample_disposable (&s), "a non-reference slice is disposable");
    s = mk_sample (b, 3, 0x09, 1, 0x06, 12, 0x01, 40);
    CHECK (cv_sample_disposable (&s), "delimiter + SEI + non-reference slice is disposable");
    s = mk_sample (b, 2, 0x01, 40, 0x01, 30);
    CHECK (cv_sample_disposable (&s), "two non-reference slices are disposable");
    s = mk_sample (b, 1, 0x41, 40);
    CHECK (!cv_sample_disposable (&s), "a reference slice (nal_ref_idc 2) must be decoded");
    s = mk_sample (b, 2, 0x01, 40, 0x21, 30);
    CHECK (!cv_sample_disposable (&s), "one reference slice among the slices: decode");
    s = mk_sample (b, 1, 0x65, 40);
    CHECK (!cv_sample_disposable (&s), "an IDR slice must be decoded");
    s = mk_sample (b, 2, 0x67, 20, 0x01, 40);
    CHECK (!cv_sample_disposable (&s), "a sample with a parameter set must be decoded");
    s = mk_sample (b, 1, 0x02, 40);
    CHECK (!cv_sample_disposable (&s), "a data partition must be decoded");
    s = mk_sample (b, 1, 0x06, 12);
    CHECK (!cv_sample_disposable (&s), "a sample without a slice is not dropped");
    s = mk_sample (b, 1, 0x01, 40);
    s.size -= 3;
    CHECK (!cv_sample_disposable (&s), "a sample with a broken NAL length is not dropped");
    s = mk_sample (b, 1, 0x01, 40);
    g_byte_array_append (b, (const guint8 *) "\0\0", 2);
    s.data = b->data;
    s.size = b->len;
    CHECK (!cv_sample_disposable (&s), "a sample with trailing bytes is not dropped");
    g_byte_array_free (b, TRUE);

    /* the rate taken from the viewers */
    CvSource src = { .clip_iv = 1.0 / 60.0 };
    CvViewer a = { 0 }, c = { 0 };
    src.viewers = g_ptr_array_new ();
    g_ptr_array_add (src.viewers, &a);
    walpe_video_set_limit (0);
    a.limit_fps = 30;
    CHECK (cv_keep_rate (&src) == 0, "setting off: every picture");
    walpe_video_set_limit (1);
    CHECK (fabs (cv_keep_rate (&src) - 30.0) < 1e-9, "limit 30 of 60: %.3f", cv_keep_rate (&src));
    a.limit_fps = 60;
    CHECK (cv_keep_rate (&src) == 0, "limit 60 of 60: every picture");
    a.limit_fps = 144;
    CHECK (cv_keep_rate (&src) == 0, "limit above the clip: every picture");
    a.limit_fps = 0;
    CHECK (cv_keep_rate (&src) == 0, "limit unknown: every picture");
    a.limit_fps = 24;
    CHECK (fabs (cv_keep_rate (&src) - 24.0) < 1e-9, "limit 24 of 60: %.3f", cv_keep_rate (&src));
    /* a second viewer: the faster one decides, a paused one does not count */
    g_ptr_array_add (src.viewers, &c);
    c.measured = TRUE;
    set_ticks (&c, 30.0, 3);
    CHECK (cv_keep_rate (&src) == 0, "a viewer whose rate is not measured yet: every picture");
    set_ticks (&c, 29.3, CV_TICKS);
    CHECK (fabs (cv_viewer_rate (&c) - 29.3) < 1e-6, "32 frames at 29.3 a second are measured as %.3f",
           cv_viewer_rate (&c));
    CHECK (fabs (cv_keep_rate (&src) - 30.0) < 1e-9, "measured 29.3 with a 60 fps clip is 30: %.3f",
           cv_keep_rate (&src));
    set_ticks (&c, 44.0, 20);
    CHECK (fabs (cv_keep_rate (&src) - 44.0) < 1e-6, "measured 44: %.3f", cv_keep_rate (&src));
    set_ticks (&c, 58.5, CV_TICKS);
    CHECK (cv_keep_rate (&src) == 0, "measured 58.5 of 60: every picture");
    /* one late frame (a hitch of 1.5 s) among frames at 60 a second: still 60, so nothing is left out */
    set_ticks (&c, 60.0, 20);
    cv_viewer_tick (&c, 100.0 + 19 / 60.0 + 1.5);
    for (int i = 1; i <= 10; i++)
        cv_viewer_tick (&c, 100.0 + 19 / 60.0 + 1.5 + i / 60.0);
    CHECK (fabs (cv_viewer_rate (&c) - 60.0) < 1e-6, "a hitch among 60 a second is measured as %.3f",
           cv_viewer_rate (&c));
    CHECK (cv_keep_rate (&src) == 0, "a hitch does not limit a scene that runs at the clip's rate");
    /* three seconds without a frame: measured anew, unknown until 9 frames came */
    cv_viewer_tick (&c, 200.0);
    CHECK (cv_viewer_rate (&c) == 0 && cv_keep_rate (&src) == 0, "after 3 s without a frame the rate is "
           "unknown: %.3f", cv_viewer_rate (&c));
    /* a scene that manages 1.4 frames a second is measured too (the decoder then leaves the rest out) */
    set_ticks (&c, 1.4, 12);
    a.paused = TRUE;
    /* (kept: the clip's rate divided by a whole number, here 60 / 43) */
    CHECK (fabs (cv_viewer_rate (&c) - 1.4) < 1e-6 && fabs (cv_keep_rate (&src) - 1.4) < 0.06,
           "a scene at 1.4 a second: measured %.3f, kept %.3f", cv_viewer_rate (&c), cv_keep_rate (&src));
    a.paused = FALSE;
    set_ticks (&c, 58.5, CV_TICKS);
    c.paused = TRUE;
    CHECK (fabs (cv_keep_rate (&src) - 24.0) < 1e-9, "paused viewer ignored: %.3f", cv_keep_rate (&src));
    a.paused = TRUE;
    CHECK (cv_keep_rate (&src) == 0, "all paused: nothing to limit");
    g_ptr_array_free (src.viewers, TRUE);
    walpe_video_set_limit (0);
    printf ("frame limit rules: done\n");

    /* the names of the upload paths */
    CHECK (walpe_video_upload_parse ("legacy") == WALPE_VUP_LEGACY && walpe_video_upload_parse ("") == 0,
           "upload path legacy");
    CHECK (walpe_video_upload_parse ("PBO") == WALPE_VUP_PBO && walpe_video_upload_parse ("pbodata") ==
           WALPE_VUP_PBODATA && walpe_video_upload_parse ("persist") == WALPE_VUP_PERSIST &&
           walpe_video_upload_parse ("direct") == WALPE_VUP_DIRECT, "upload path names");
    CHECK (walpe_video_upload_parse ("direct+pingpong") == (WALPE_VUP_DIRECT | WALPE_VUP_PINGPONG) &&
           walpe_video_upload_parse ("pingpong") == WALPE_VUP_PINGPONG &&
           walpe_video_upload_parse ("pingpong,pbo") == (WALPE_VUP_PBO | WALPE_VUP_PINGPONG),
           "upload path with +pingpong");
    CHECK (walpe_video_upload_parse ("bogus") < 0 && walpe_video_upload_parse (NULL) < 0 &&
           walpe_video_upload_parse ("direct+bogus") < 0, "unknown upload path names are refused");
    CHECK (!walpe_video_set_upload ("hold") && walpe_video_upload_mode () == WALPE_VUP_LEGACY,
           "hold is a measuring aid, not a setting");
    CHECK (walpe_video_set_upload ("persist+pingpong") && walpe_video_upload_mode () ==
           (WALPE_VUP_PERSIST | WALPE_VUP_PINGPONG) && walpe_video_set_upload ("legacy") &&
           walpe_video_upload_mode () == 0, "the setting is what the module is given");
}

/* ------------------------------------------------- 3. decoded pictures */

enum { V_FULL, V_HALF, V_NV_FULL, V_NV_HALF, V_N };
typedef struct {
    gint64 ticks;
    char *md5[V_N];
} Ref;
static GArray *refs;            /* Ref, display order */
static guint ref_timescale;
static int ref_w, ref_h, ref_nonref;

/* what the module queued */
typedef struct {
    gint64 ticks;
    char *md5;
    int w, h, kind;
    double play;
    guint64 seq;
} Rec;
static GMutex rec_lock;
static GArray *recs;

static void test_pushed (void *srcp, const void *frame)
{
    CvSource *src = srcp;
    const CvFrame *f = frame;
    Rec r = { .ticks = (gint64) llround (f->raw * src->demux->timescale), .md5 = cv_frame_md5_str (f),
              .w = f->w, .h = f->h, .kind = f->kind, .play = f->pts, .seq = f->seq };
    g_mutex_lock (&rec_lock);
    g_array_append_val (recs, r);
    g_mutex_unlock (&rec_lock);
}

static void recs_clear (void)
{
    g_mutex_lock (&rec_lock);
    for (guint i = 0; i < recs->len; i++)
        g_free (g_array_index (recs, Rec, i).md5);
    g_array_set_size (recs, 0);
    g_mutex_unlock (&rec_lock);
}

static guint recs_len (void)
{
    g_mutex_lock (&rec_lock);
    guint n = recs->len;
    g_mutex_unlock (&rec_lock);
    return n;
}

static char *md5_of (const guint8 *p, gsize n)
{
    return g_compute_checksum_for_data (G_CHECKSUM_MD5, p, n);
}

/* the four layouts of one decoded 8-bit 4:2:0 frame, as the module hands them on */
static void ref_add (const AVFrame *fr)
{
    int w = fr->width, h = fr->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    int dw = w / 2, dh = h / 2, dcw = (dw + 1) / 2, dch = (dh + 1) / 2;
    gsize ysz = (gsize) w * h, csz = (gsize) cw * ch, hy = (gsize) dw * dh, hc = (gsize) dcw * dch;
    guint8 *full = g_malloc (ysz + 2 * csz), *nv = g_malloc (ysz + 2 * csz);
    guint8 *half = g_malloc (hy + 2 * hc), *nvh = g_malloc (hy + 2 * hc);
    for (int y = 0; y < h; y++)
        memcpy (full + (gsize) y * w, fr->data[0] + (gssize) y * fr->linesize[0], (gsize) w);
    for (int p = 1; p <= 2; p++)
        for (int y = 0; y < ch; y++)
            memcpy (full + ysz + (gsize) (p - 1) * csz + (gsize) y * cw,
                    fr->data[p] + (gssize) y * fr->linesize[p], (gsize) cw);
    memcpy (nv, full, ysz);
    for (gsize i = 0; i < csz; i++) {
        nv[ysz + 2 * i] = full[ysz + i];
        nv[ysz + 2 * i + 1] = full[ysz + csz + i];
    }
    ref_box (full, w, w, h, half, dw, dw, dh, 1);
    ref_box (full + ysz, cw, cw, ch, half + hy, dcw, dcw, dch, 1);
    ref_box (full + ysz + csz, cw, cw, ch, half + hy + hc, dcw, dcw, dch, 1);
    ref_box (nv, w, w, h, nvh, dw, dw, dh, 1);
    ref_box (nv + ysz, 2 * cw, cw, ch, nvh + hy, 2 * dcw, dcw, dch, 2);
    Ref r = { .ticks = fr->pts };
    r.md5[V_FULL] = md5_of (full, ysz + 2 * csz);
    r.md5[V_HALF] = md5_of (half, hy + 2 * hc);
    r.md5[V_NV_FULL] = md5_of (nv, ysz + 2 * csz);
    r.md5[V_NV_HALF] = md5_of (nvh, hy + 2 * hc);
    g_array_append_val (refs, r);
    g_free (full);
    g_free (nv);
    g_free (half);
    g_free (nvh);
}

static gint ref_cmp (gconstpointer a, gconstpointer b)
{
    gint64 d = ((const Ref *) a)->ticks - ((const Ref *) b)->ticks;
    return d < 0 ? -1 : d > 0;
}

/* plain libavcodec decode of the clip, one thread, nothing of the module but the demuxer */
static gboolean ref_decode (const char *clip)
{
    GError *err = NULL;
    Mp4Demux *dm = mp4_demux_open_range (clip, 0, 0, &err);
    if (!dm) {
        printf ("the generated clip cannot be opened: %s\n", err ? err->message : "?");
        g_clear_error (&err);
        return FALSE;
    }
    const AVCodec *codec = avcodec_find_decoder (AV_CODEC_ID_H264);
    AVCodecContext *dec = avcodec_alloc_context3 (codec);
    dec->thread_count = 1;
    if (avcodec_open2 (dec, codec, NULL) < 0)
        return FALSE;
    AVPacket *pkt = av_packet_alloc ();
    AVFrame *fr = av_frame_alloc ();
    GString *ab = g_string_new (NULL);
    static const guint8 sc4[4] = { 0, 0, 0, 1 };
    ref_timescale = dm->timescale;
    ref_w = dm->width;
    ref_h = dm->height;
    for (guint i = 0; i <= dm->nsamples; i++) {
        const Mp4Sample *s = i < dm->nsamples ? mp4_demux_next (dm) : NULL;
        if (s) {
            ref_nonref += cv_sample_disposable (s);
            g_string_truncate (ab, 0);
            if (i == 0) {
                g_string_append_len (ab, (const char *) sc4, 4);
                g_string_append_len (ab, (const char *) dm->sps, (gssize) dm->sps_len);
                g_string_append_len (ab, (const char *) sc4, 4);
                g_string_append_len (ab, (const char *) dm->pps, (gssize) dm->pps_len);
            }
            gsize off = 0;
            while (off + 4 <= (gsize) s->size) {
                guint32 nl = ((guint32) s->data[off] << 24) | ((guint32) s->data[off + 1] << 16) |
                             ((guint32) s->data[off + 2] << 8) | (guint32) s->data[off + 3];
                off += 4;
                if (!nl || off + nl > (gsize) s->size)
                    break;
                g_string_append_len (ab, (const char *) sc4, 4);
                g_string_append_len (ab, (const char *) s->data + off, (gssize) nl);
                off += nl;
            }
            pkt->data = (guint8 *) ab->str;
            pkt->size = (int) ab->len;
            pkt->pts = (int64_t) (s->pts * dm->timescale) + s->cts_off;
            pkt->dts = pkt->pts;
            avcodec_send_packet (dec, pkt);
        } else {
            avcodec_send_packet (dec, NULL);    /* drain */
        }
        while (avcodec_receive_frame (dec, fr) >= 0) {
            if (fr->format == AV_PIX_FMT_YUV420P || fr->format == AV_PIX_FMT_YUVJ420P)
                ref_add (fr);
            av_frame_unref (fr);
        }
    }
    g_array_sort (refs, ref_cmp);
    guint n = dm->nsamples;
    av_frame_free (&fr);
    av_packet_free (&pkt);
    avcodec_free_context (&dec);
    g_string_free (ab, TRUE);
    mp4_demux_free (dm);
    CHECK (refs->len == n, "reference decode: %u frames of %u samples", refs->len, n);
    return refs->len == n && n > 0;
}

static const Ref *ref_find (gint64 ticks)
{
    for (guint i = 0; i < refs->len; i++)
        if (g_array_index (refs, Ref, i).ticks == ticks)
            return &g_array_index (refs, Ref, i);
    return NULL;
}

/* ffmpeg makes the clip: 1 s of 1280x720 at 60 fps, a keyframe every 30
 * pictures, three B pictures between the others, none of them a reference */
static char *make_clip (const char *dir)
{
    char *ff = g_find_program_in_path ("ffmpeg");
    if (!ff)
        return NULL;
    char *out = g_build_filename (dir, "clip.mp4", NULL);
    const char *argv[] = { ff, "-nostdin", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                           "testsrc2=size=1280x720:rate=60", "-t", "1", "-c:v", "libx264", "-preset",
                           "veryfast", "-pix_fmt", "yuv420p", "-g", "30", "-bf", "3", "-x264-params",
                           "b-pyramid=0:b-adapt=0:scenecut=0", "-an", "-y", out, NULL };
    int status = -1;
    gboolean ok = g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL, NULL, NULL, NULL, &status, NULL);
    g_free (ff);
    if (!ok || status != 0 || !g_file_test (out, G_FILE_TEST_IS_REGULAR)) {
        g_free (out);
        return NULL;
    }
    return out;
}

typedef struct {
    const char *name;
    gboolean half;      /* the viewer shows the clip at half size */
    int limit_fps;      /* a video wallpaper's frame limit, 0 = none */
    double measured;    /* a scene's measured frame rate, 0 = not a scene viewer */
    gboolean limit_on;
    double expect_rate; /* pictures a second expected in the queue, 0 = all */
} Case;

/* play the clip for three passes as one viewer and compare what was queued */
static void run_case (const char *clip, const Case *cs)
{
    GError *err = NULL;
    recs_clear ();
    walpe_video_set_limit (cs->limit_on);
    CvViewer *vw = cv_attach (clip, 0, 0, NULL, &err);
    if (!vw) {
        CHECK (FALSE, "%s: %s", cs->name, err ? err->message : "no viewer");
        g_clear_error (&err);
        return;
    }
    CvSource *src = vw->src;
    g_mutex_lock (&src->lock);
    vw->limit_fps = cs->limit_fps;
    if (cs->measured > 0) {
        vw->measured = TRUE;
        set_ticks (vw, cs->measured, CV_TICKS);
    }
    g_mutex_unlock (&src->lock);
    if (cs->half) {
        g_atomic_int_set (&vw->want_w, ref_w / 2);
        g_atomic_int_set (&vw->want_h, ref_h / 2);
    }
    double pass = (double) refs->len * src->clip_iv, t0 = src->clip_t0;
    guint want = cs->expect_rate > 0 ? (guint) (3.2 * pass * cs->expect_rate) : (guint) (3.2 * refs->len);
    gint64 until = g_get_monotonic_time () + 90 * G_USEC_PER_SEC;
    while (recs_len () < want && g_get_monotonic_time () < until)
        g_usleep (20000);
    g_mutex_lock (&src->lock);
    guint skipped = src->lim_skipped, unshown = src->lim_unshown, reused = src->dl_reused, fresh = src->dl_fresh;
    double iv = src->clip_iv;
    g_mutex_unlock (&src->lock);
    cvv_free (&vw->base);

    g_mutex_lock (&rec_lock);
    guint n = recs->len, same = 0, halfn = 0, bad = 0, off_grid = 0, in_pass2 = 0;
    CHECK (n >= want, "%s: %u frames queued, %u wanted", cs->name, n, want);
    double play0 = n ? g_array_index (recs, Rec, 0).play : 0;
    GString *seen2 = g_string_new (NULL), *want2 = g_string_new (NULL);
    double first_play[4] = { -1, -1, -1, -1 };
    for (guint i = 0; i < n; i++) {
        const Rec *r = &g_array_index (recs, Rec, i);
        const Ref *ref = ref_find (r->ticks);
        if (!ref) {
            bad++;
            continue;
        }
        gboolean half = r->w != ref_w;
        int v = r->kind == CV_NV ? (half ? V_NV_HALF : V_NV_FULL) : (half ? V_HALF : V_FULL);
        if (!strcmp (ref->md5[v], r->md5))
            same++;
        halfn += half;
        int p = (int) floor ((r->play - play0) / pass + 1e-6);
        double t = (double) r->ticks / ref_timescale - t0;
        if (p >= 0 && p < 4 && (first_play[p] < 0 || r->play < first_play[p]) && t < iv * 0.5)
            first_play[p] = r->play;
        if (p == 1) {
            in_pass2++;
            g_string_append_printf (seen2, "%" G_GINT64_FORMAT " ", r->ticks);
            if (cs->expect_rate > 0 && !cv_grid_keeps (t, iv, cs->expect_rate))
                off_grid++;
        }
        if (i > 0)
            CHECK (r->play > g_array_index (recs, Rec, i - 1).play, "%s: presentation time goes back at "
                   "frame %u", cs->name, i);
    }
    for (guint i = 0; i < refs->len; i++) {
        const Ref *ref = &g_array_index (refs, Ref, i);
        if (!(cs->expect_rate > 0) ||
            cv_grid_keeps ((double) ref->ticks / ref_timescale - t0, iv, cs->expect_rate))
            g_string_append_printf (want2, "%" G_GINT64_FORMAT " ", ref->ticks);
    }
    CHECK (bad == 0, "%s: %u queued frames carry a time stamp the clip does not have", cs->name, bad);
    CHECK (same == n, "%s: %u of %u queued frames are byte-identical to the plain decode", cs->name, same, n);
    CHECK (!strcmp (seen2->str, want2->str), "%s: the second pass queued other pictures than expected\n"
           "  queued:   %s\n  expected: %s", cs->name, seen2->str, want2->str);
    CHECK (off_grid == 0, "%s: %u pictures off the grid were queued", cs->name, off_grid);
    /* the clip is as long as before: its first picture comes back after exactly one pass */
    for (int p = 1; p < 3; p++)
        CHECK (first_play[p] >= 0 && first_play[p - 1] >= 0 &&
               fabs (first_play[p] - first_play[p - 1] - pass) < 1e-4,
               "%s: pass %d starts %.5f s after pass %d, the clip is %.5f s long", cs->name, p + 1,
               first_play[p] - first_play[p - 1], p, pass);
    if (cs->half)
        CHECK (halfn + 12 >= n, "%s: %u of %u frames at half size", cs->name, halfn, n);
    else
        CHECK (halfn == 0, "%s: %u frames at half size", cs->name, halfn);
    if (cs->expect_rate > 0) {
        CHECK (skipped > 0 || ref_nonref == 0, "%s: no picture was left undecoded", cs->name);
        CHECK (skipped + unshown > n / 4, "%s: %u + %u pictures left out for %u queued", cs->name,
               skipped, unshown, n);
    } else {
        CHECK (skipped == 0 && unshown == 0, "%s: %u + %u pictures left out without a limit", cs->name,
               skipped, unshown);
    }
    printf ("%s: %u frames queued, %u identical to the plain decode, %u at half size; second pass %u "
            "pictures; left undecoded %u, decoded and not queued %u; kept download buffer used %u times, "
            "new %u\n", cs->name, n, same, halfn, in_pass2, skipped, unshown, reused, fresh);
    g_string_free (seen2, TRUE);
    g_string_free (want2, TRUE);
    g_mutex_unlock (&rec_lock);
    walpe_video_set_limit (0);
}

/* ------------------------------------------- 4. hardware decoding asked */

static void test_hw (const char *clip)
{
    GError *err = NULL;
    recs_clear ();
    walpe_video_set_hwdec (1);
    CvViewer *vw = cv_attach (clip, 0, 0, NULL, &err);
    if (!vw) {
        CHECK (FALSE, "hardware asked: %s", err ? err->message : "no viewer");
        g_clear_error (&err);
        walpe_video_set_hwdec (0);
        return;
    }
    gint64 until = g_get_monotonic_time () + 60 * G_USEC_PER_SEC;
    while (recs_len () < 45 && g_get_monotonic_time () < until)
        g_usleep (20000);
    char *note = walpe_video_decoder_note ();
    cvv_free (&vw->base);
    walpe_video_set_hwdec (0);
    g_mutex_lock (&rec_lock);
    guint n = recs->len, same = 0;
    for (guint i = 0; i < n; i++) {
        const Rec *r = &g_array_index (recs, Rec, i);
        const Ref *ref = ref_find (r->ticks);
        if (ref && !strcmp (ref->md5[r->kind == CV_NV ? V_NV_FULL : V_FULL], r->md5))
            same++;
    }
    g_mutex_unlock (&rec_lock);
    CHECK (n >= 45, "hardware asked: %u frames queued", n);
    CHECK (note != NULL, "hardware asked: no status line");
    if (note && strstr (note, "hardware (")) {
        /* a device took the stream: its pictures against the software decoder's (H.264 decoding is
         * exact, so they should be the same). Reported, not failed: that is the driver's doing. */
        printf ("hardware decoding WORKS here: status \"%s\"; %u of %u pictures byte-identical to software "
                "decoding%s\n", note, same, n, same == n ? "" : " - PLEASE REPORT THIS LINE");
    } else {
        CHECK (note && strstr (note, "software") && strstr (note, "asked for and is not in use: "),
               "hardware asked, none there: status says \"%s\"", note ? note : "(null)");
        CHECK (note && !strstr (note, "not available or failed"), "the status gives no reason: \"%s\"",
               note ? note : "(null)");
        CHECK (same == n, "software fallback: %u of %u frames identical", same, n);
        printf ("hardware decoding asked for, no device here: software took over, %u of %u pictures "
                "identical; status \"%s\"\n", same, n, note ? note : "");
    }
    g_free (note);
}

/* ---------------------- 4b. the attempts, played through with stand-ins */

static gint fake_on;            /* the stand-ins act */
static gint fake_pictures;      /* pictures the second one has delivered */
static gint fake_fail_after;    /* ... before it fails for good */

/* decode thread, after every receive call of a hardware decoder */
static int test_hw_receive (void *srcp, int rc)
{
    CvSource *src = srcp;
    if (!g_atomic_int_get (&fake_on) || !src->hw_name || strcmp (src->hw_name, "fake"))
        return rc;
    if (src->hw.cur == 0) {
        /* the first stand-in does not take the stream: no picture comes
         * out, and the flag is set that a hardware acceleration sets when
         * its set-up for the stream fails */
        if (rc >= 0)
            av_frame_unref (src->frame);
        g_atomic_int_set (&src->hw_refused, 1);
        return AVERROR (EAGAIN);
    }
    if (rc >= 0 && g_atomic_int_add (&fake_pictures, 1) >= g_atomic_int_get (&fake_fail_after)) {
        /* the second one worked for a while: from here on every picture fails */
        av_frame_unref (src->frame);
        return AVERROR (EIO);
    }
    return rc;
}

static int ref_index (gint64 ticks)
{
    for (guint i = 0; i < refs->len; i++)
        if (g_array_index (refs, Ref, i).ticks == ticks)
            return (int) i;
    return -1;
}

static void test_hw_fake (const char *clip)
{
    const int hw_pictures = 40;
    GError *err = NULL;
    recs_clear ();
    g_atomic_int_set (&fake_pictures, 0);
    g_atomic_int_set (&fake_fail_after, hw_pictures);
    g_atomic_int_set (&fake_on, 1);
    CHECK (walpe_video_set_hwdec_name ("fake"), "the stand-in decoder is asked for by name");
    CvViewer *vw = cv_attach (clip, 0, 0, NULL, &err);
    if (!vw) {
        CHECK (FALSE, "stand-in hardware: %s", err ? err->message : "no viewer");
        g_clear_error (&err);
        g_atomic_int_set (&fake_on, 0);
        walpe_video_set_hwdec (0);
        return;
    }
    double iv = vw->src->clip_iv;
    /* long enough for the hardware part, the failure and a loop of the clip in software */
    guint want = (guint) hw_pictures + refs->len + 20;
    char *hw_note = NULL;
    gint64 until = g_get_monotonic_time () + (gint64) 90 * slow * G_USEC_PER_SEC;
    while (recs_len () < want && g_get_monotonic_time () < until) {
        char *now = walpe_video_decoder_note ();
        if (!hw_note && now && strstr (now, "hardware (fake") && !strstr (now, "no picture yet"))
            hw_note = g_strdup (now);
        g_free (now);
        g_usleep (5000);
    }
    char *note = walpe_video_decoder_note ();
    cvv_free (&vw->base);
    g_atomic_int_set (&fake_on, 0);
    walpe_video_set_hwdec (0);

    CHECK (hw_note != NULL, "stand-in hardware: the status never said that the second stand-in decodes");
    CHECK (!hw_note || (strstr (hw_note, "decoding hardware of its own") &&
                        strstr (hw_note, "hands its pictures out in main memory")),
           "stand-in hardware: status while it decoded: \"%s\"", hw_note ? hw_note : "(null)");
    CHECK (note && strstr (note, ": software, ") && strstr (note, "asked for and is not in use: ") &&
           strstr (note, "fake: stream refused") &&
           strstr (note, "fake: decoding failed (8 decode calls in a row failed"),
           "stand-in hardware: the status afterwards names both failures: \"%s\"", note ? note : "(null)");
    CHECK (g_atomic_int_get (&fake_pictures) >= hw_pictures, "stand-in hardware: %d pictures came from it",
           g_atomic_int_get (&fake_pictures));
    g_mutex_lock (&rec_lock);
    guint n = recs->len, same = 0, order_bad = 0, time_bad = 0;
    for (guint i = 0; i < n; i++) {
        const Rec *r = &g_array_index (recs, Rec, i);
        const Ref *ref = ref_find (r->ticks);
        if (ref && !strcmp (ref->md5[r->kind == CV_NV ? V_NV_FULL : V_FULL], r->md5))
            same++;
        if (i == 0)
            continue;
        const Rec *q = &g_array_index (recs, Rec, i - 1);
        int a = ref_index (q->ticks), b = ref_index (r->ticks);
        /* the next picture of the clip, also across the two decoder changes and the loop point */
        if (a < 0 || b < 0 || b != (a + 1) % (int) refs->len)
            order_bad++;
        if (fabs (r->play - q->play - iv) > 1e-4)
            time_bad++;
    }
    int first = n ? ref_index (g_array_index (recs, Rec, 0).ticks) : -1;
    g_mutex_unlock (&rec_lock);
    CHECK (n >= want, "stand-in hardware: %u frames queued, %u wanted", n, want);
    CHECK (first == 0, "stand-in hardware: the first picture queued is picture %d of the clip", first);
    CHECK (same == n, "stand-in hardware: %u of %u pictures identical to the plain decode", same, n);
    CHECK (order_bad == 0, "stand-in hardware: %u pictures missing, repeated or out of order", order_bad);
    CHECK (time_bad == 0, "stand-in hardware: %u presentation times not one frame interval after the one before",
           time_bad);
    printf ("stand-in hardware decoders: the first refused the stream, the second decoded %d pictures and failed, "
            "software took over: %u pictures queued, %u identical, %u out of order, %u with a gap in time; status "
            "while it decoded \"%s\"; afterwards \"%s\"\n", hw_pictures, n, same, order_bad, time_bad,
            hw_note ? hw_note : "", note ? note : "");
    g_free (hw_note);
    g_free (note);
}

/* ----------------------------------------------------------- 5. GL part */

static const char *gl_clip;

/* the plane textures the viewer draws from, read back: MD5 in the layout of cv_frame_md5_str */
static char *gl_planes_md5 (CvViewer *vw, guint8 *buf)
{
    GChecksum *ck = g_checksum_new (G_CHECKSUM_MD5);
    int np = vw->pkind == CV_NV ? 2 : 3;
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    for (int i = 0; i < np; i++) {
        gboolean two = vw->pkind == CV_NV && i == 1;
        glBindTexture (GL_TEXTURE_2D, vw->ptex[i]);
        glGetTexImage (GL_TEXTURE_2D, 0, two ? GL_RG : GL_RED, GL_UNSIGNED_BYTE, buf);
        g_checksum_update (ck, buf, (gssize) vw->pw[i] * vw->ph[i] * (two ? 2 : 1));
    }
    glPixelStorei (GL_PACK_ALIGNMENT, 4);
    char *md5 = g_strdup (g_checksum_get_string (ck));
    g_checksum_free (ck);
    return md5;
}

/* one upload attempt of vw; when it took another frame, compare the textures with that frame as queued.
 * Returns 1 = compared and identical, 0 = compared and different, -1 = no new frame. */
static int gl_step (CvViewer *vw, guint64 *last, guint8 *buf)
{
    int bw = 0, bh = 0, res = -1;
    gint64 next = 0;
    cv_viewer_upload (vw, &bw, &bh, &next);
    if (vw->drawn_seq == *last || !vw->pkind)
        return -1;
    *last = vw->drawn_seq;
    char *md5 = gl_planes_md5 (vw, buf);
    g_mutex_lock (&rec_lock);
    for (guint i = 0; i < recs->len; i++) {
        const Rec *r = &g_array_index (recs, Rec, i);
        if (r->seq == *last)
            res = !strcmp (r->md5, md5);
    }
    g_mutex_unlock (&rec_lock);
    g_free (md5);
    return res;
}

/* one upload path, at full or half size: every upload read back, and what it took per frame */
static void gl_path (const char *name, gboolean half)
{
    GError *err = NULL;
    CHECK (walpe_video_set_upload (name), "upload path %s is not accepted", name);
    int mode = walpe_video_upload_mode (), base = mode & WALPE_VUP_BASE;
    recs_clear ();
    WalpeVideo *v = walpe_video_custom_new (gl_clip, &err);
    if (!v) {
        CHECK (FALSE, "GL %s: %s", name, err ? err->message : "no viewer");
        g_clear_error (&err);
        return;
    }
    CvViewer *vw = (CvViewer *) v;
    v->gl_init (v, NULL);
    if (half) {
        g_atomic_int_set (&vw->want_w, ref_w / 2);
        g_atomic_int_set (&vw->want_h, ref_h / 2);
    }
    guint checked = 0, same = 0;
    guint64 last = 0;
    gint64 until = g_get_monotonic_time () + 40 * slow * G_USEC_PER_SEC;
    guint8 *buf = g_malloc ((gsize) ref_w * ref_h * 2);
    while (checked < 24 && g_get_monotonic_time () < until) {
        int r = gl_step (vw, &last, buf);
        if (r >= 0) {
            checked++;
            same += (guint) r;
        }
        g_usleep (3000);
    }
    g_free (buf);
    guint fr = vw->up_frames, np = vw->pkind == CV_NV ? 2 : 3;
    gboolean can = cv_gl_can_persist ();
    int real = vw->up_real;
    CHECK (checked >= 24, "GL %s: %u uploads could be compared", name, checked);
    CHECK (same == checked, "GL %s, %s size: %u of %u uploads left the queued frame's bytes in the plane "
           "textures", name, half ? "half" : "full", same, checked);
    CHECK (vw->up_tex_calls == np * fr, "GL %s: %u texture calls for %u frames of %u planes", name,
           vw->up_tex_calls, fr, np);
    {
        GLint bound = -1;
        glGetIntegerv (GL_PIXEL_UNPACK_BUFFER_BINDING, &bound);
        CHECK (bound == 0, "GL %s: a buffer object is left bound for unpacking (%d)", name, bound);
    }
    if (base == WALPE_VUP_LEGACY) {
        CHECK (real == WALPE_VUP_LEGACY && !vw->up_buf_calls && !vw->up_maps && !vw->up_fences &&
               !vw->up_staged && vw->up_copied == 0, "GL %s: buffer work on the legacy path", name);
    } else if (base == WALPE_VUP_PBO) {
        CHECK (real == WALPE_VUP_PBO && vw->up_buf_calls == fr && vw->up_maps == fr && vw->up_copied > 0,
               "GL %s: %u stores and %u maps for %u frames", name, vw->up_buf_calls, vw->up_maps, fr);
    } else if (base == WALPE_VUP_PBODATA) {
        CHECK (real == WALPE_VUP_PBODATA && vw->up_buf_calls >= fr && vw->up_copied > 0,
               "GL %s: %u buffer calls for %u frames", name, vw->up_buf_calls, fr);
    } else if (base == WALPE_VUP_PERSIST && can) {
        CHECK (real == WALPE_VUP_PERSIST && vw->up_fences + vw->up_waits == fr && vw->up_copied > 0 &&
               vw->up_buf_calls == vw->up_waits, "GL %s: %u fences, %u frames without an idle buffer, %u "
               "buffer calls for %u frames", name, vw->up_fences, vw->up_waits, vw->up_buf_calls, fr);
    } else if (base == WALPE_VUP_DIRECT && can) {
        /* the first frames are decoded before the viewer has made the buffers */
        CHECK (real == WALPE_VUP_DIRECT && vw->up_staged + 14 >= fr && vw->up_staged > 0 &&
               vw->up_fences == vw->up_staged && vw->up_copied == 0 && !vw->up_buf_calls,
               "GL %s: %u of %u frames from buffers the decoder wrote, %u fences, %.0f bytes copied by "
               "the render thread", name, vw->up_staged, fr, vw->up_fences, vw->up_copied);
    } else {
        CHECK (real == WALPE_VUP_PBO, "GL %s without persistent buffer storage is not the pbo path", name);
    }
    if (mode & WALPE_VUP_PINGPONG)
        CHECK (vw->palt[0] != 0 && vw->palt[0] != vw->ptex[0], "GL %s: no second set of textures", name);
    else
        CHECK (vw->palt[0] == 0, "GL %s: a second set of textures", name);
    char *note = walpe_video_upload_note ();
    printf ("GL upload %-16s %s size: %u of %u uploads read back identical; %u frames: %.2f texture calls, "
            "%.2f buffer calls, %.2f maps, %.2f fences per frame, %.2f MB per frame copied by the render "
            "thread, %u frames from buffers the decoder wrote; status \"%s\"\n", name, half ? "half" : "full",
            same, checked, fr, vw->up_tex_calls / (double) MAX (fr, 1), vw->up_buf_calls / (double) MAX (fr, 1),
            vw->up_maps / (double) MAX (fr, 1), vw->up_fences / (double) MAX (fr, 1),
            vw->up_copied / 1048576.0 / MAX (fr, 1), vw->up_staged, note ? note : "");
    g_free (note);
    v->gl_free (v);
    v->free (v);
    walpe_video_set_upload ("legacy");
}

/* the path is changed while the clip plays, and a second viewer shares the source: every upload of both
 * is still the queued frame; the viewer that made the decoder's buffers leaves first */
static void gl_switching (void)
{
    static const char *const order[] = { "legacy", "direct", "persist", "pbo", "direct+pingpong", "pbodata",
                                         "pingpong", "direct", "legacy" };
    GError *err = NULL;
    recs_clear ();
    walpe_video_set_upload ("direct");
    WalpeVideo *va = walpe_video_custom_new (gl_clip, &err), *vb = walpe_video_custom_new (gl_clip, &err);
    if (!va || !vb) {
        CHECK (FALSE, "GL switching: %s", err ? err->message : "no viewer");
        g_clear_error (&err);
        return;
    }
    CvViewer *a = (CvViewer *) va, *b = (CvViewer *) vb;
    CHECK (a->src == b->src, "GL switching: two viewers of one clip do not share the decoder");
    va->gl_init (va, NULL);
    vb->gl_init (vb, NULL);
    guint8 *buf = g_malloc ((gsize) ref_w * ref_h * 2);
    guint checked = 0, same = 0, staged_b = 0;
    guint64 la = 0, lb = 0;
    for (guint m = 0; m < G_N_ELEMENTS (order); m++) {
        walpe_video_set_upload (order[m]);
        guint got = 0;
        gint64 until = g_get_monotonic_time () + 20 * slow * G_USEC_PER_SEC;
        while (got < 8 && g_get_monotonic_time () < until) {
            int r = gl_step (a, &la, buf);
            if (r >= 0) {
                checked++;
                got++;
                same += (guint) r;
            }
            r = gl_step (b, &lb, buf);
            if (r >= 0) {
                checked++;
                same += (guint) r;
            }
            g_usleep (3000);
        }
        CHECK (got >= 8, "GL switching to %s: %u uploads", order[m], got);
    }
    /* the owner of the mapped buffers goes while frames in them are queued: the other viewer goes on,
     * and takes the buffers over */
    walpe_video_set_upload ("direct");
    for (int i = 0; i < 40; i++) {
        int r = gl_step (a, &la, buf);
        if (r >= 0) { checked++; same += (guint) r; }
        r = gl_step (b, &lb, buf);
        if (r >= 0) { checked++; same += (guint) r; }
        g_usleep (3000);
    }
    gboolean a_owned = a->src->stage_owner == a;
    va->gl_free (va);
    va->free (va);
    b->up_staged = 0;
    gint64 until = g_get_monotonic_time () + 20 * slow * G_USEC_PER_SEC;
    guint after = 0;
    while (after < 30 && g_get_monotonic_time () < until) {
        int r = gl_step (b, &lb, buf);
        if (r >= 0) {
            checked++;
            after++;
            same += (guint) r;
        }
        g_usleep (3000);
    }
    staged_b = b->up_staged;
    if (cv_gl_can_persist ()) {
        CHECK (a_owned, "GL switching: the first viewer did not make the decoder's buffers");
        CHECK (b->src->stage_owner == b && staged_b > 0, "GL switching: the second viewer did not take the "
               "buffers over (%u frames from them)", staged_b);
    }
    CHECK (same == checked && checked > 100, "GL switching: %u of %u uploads identical", same, checked);
    printf ("GL upload paths switched while playing, two viewers of one clip, the owner of the decoder's "
            "buffers leaving: %u of %u uploads read back identical; the other viewer then took %u of %u "
            "frames from buffers of its own\n", same, checked, staged_b, after);
    g_free (buf);
    vb->gl_free (vb);
    vb->free (vb);
    walpe_video_set_upload ("legacy");
}

static void gl_part (void)
{
    GError *err = NULL;
    walpe_video_set_limit (0);
    printf ("GL: %s, %s; persistent buffer storage %s\n", (const char *) glGetString (GL_RENDERER),
            (const char *) glGetString (GL_VERSION), cv_gl_can_persist () ? "yes" : "no");
    static const char *const paths[] = { "legacy", "pbo", "pbodata", "persist", "direct", "pingpong",
                                         "direct+pingpong" };
    for (guint i = 0; i < G_N_ELEMENTS (paths); i++) {
        gl_path (paths[i], FALSE);
        gl_path (paths[i], TRUE);
    }
    gl_switching ();
    if (only_upload)
        return;

    /* a scene's video layer, asked for a picture 30 times a second, with the limit on */
    walpe_video_set_limit (1);
    recs_clear ();
    WalpeVideoTex *t = walpe_video_tex_new (gl_clip, 0, 0, &err);
    if (!t) {
        CHECK (FALSE, "GL: %s", err ? err->message : "no video texture");
        g_clear_error (&err);
        return;
    }
    CvSource *src = t->vw->src;
    gint64 start = g_get_monotonic_time ();
    guint ticks = 0, tex = 0;
    while (g_get_monotonic_time () - start < 3 * G_USEC_PER_SEC) {
        /* a frame clock that never runs ahead: one frame, then the rest of its 33 ms */
        gint64 t0 = g_get_monotonic_time ();
        int w = 0, h = 0;
        tex = walpe_video_tex_update (t, &w, &h) ? tex + 1 : tex;
        ticks++;
        gint64 used = g_get_monotonic_time () - t0;
        if (used < 33333)
            g_usleep ((gulong) (33333 - used));
    }
    g_mutex_lock (&src->lock);
    double keep = src->keep_rate, measured = cv_viewer_rate (t->vw);
    guint skipped = src->lim_skipped, unshown = src->lim_unshown;
    g_mutex_unlock (&src->lock);
    walpe_video_tex_free (t);
    walpe_video_set_limit (0);
    CHECK (tex > 0, "GL: the video texture never had a picture");
    /* (30 a second where the machine is free; whatever it manages where it is not) */
    CHECK (measured > 1 && measured < 40, "GL: a viewer asked %u times in 3 s is measured at %.1f a second",
           ticks, measured);
    CHECK (keep > 0 && keep < 57 && fabs (keep - measured) < 0.35 * measured,
           "GL: the source keeps %.1f pictures a second for a viewer measured at %.1f", keep, measured);
    CHECK (skipped + unshown > 10, "GL: %u + %u pictures left out in 3 s", skipped, unshown);
    printf ("GL: video layer asked %u times in 3 s: measured %.1f a second, %.2f pictures a second kept, "
            "%u not decoded, %u decoded and not queued\n", ticks, measured, keep, skipped, unshown);
}

static gboolean gl_run (EGLDisplay dpy)
{
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1)
        return FALSE;
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext (dpy, ctx);
        eglTerminate (dpy);
        return FALSE;
    }
    gl_part ();
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return TRUE;
}

static void test_gl (const char *clip)
{
    gboolean ran = FALSE;
    gl_clip = clip;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd))
            for (EGLint i = 0; i < nd && !ran; i++)
                ran = gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL));
    }
    if (!ran && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
        ran = gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL));
    if (!ran)
        printf ("video-test: no EGL device / surfaceless GL 3.3 context, GL part skipped\n");
}

int main (int argc, char **argv)
{
    /* the path a hardware renderer takes (planes to the GPU), whatever renders here; no display needed */
    g_setenv ("WALPE_GPU_YUV", "1", TRUE);
    g_unsetenv ("WALPE_VIDEO_LIMIT");
    g_unsetenv ("WALPE_VIDEO_HWDEC");
    g_unsetenv ("WALPE_VIDEO_FULLRES");
    g_unsetenv ("WALPE_VIDEO_SIMD");
    g_unsetenv ("WALPE_VIDEO_UPLOAD");
    g_unsetenv ("WALPE_FPS_LOG");
    gboolean nv = g_getenv ("WALPE_VIDEO_TEST_NV12") != NULL;
    refs = g_array_new (FALSE, TRUE, sizeof (Ref));
    recs = g_array_new (FALSE, TRUE, sizeof (Rec));

    test_box ();
    test_limit_rules ();
    if (argc > 1 && !strcmp (argv[1], "--box")) {
        /* only the parts that need no clip */
        printf (fails ? "video-test: %d FAILED\n" : "video-test: all passed\n", fails);
        return fails ? 1 : 0;
    }

    /* an own clip can be given (the hardware decoding check on a real file):
     *   video-test <file.mp4>      (8-bit 4:2:0 H.264 in MP4) */
    char *dir = g_dir_make_tmp ("walpe-video-test-XXXXXX", NULL);
    only_upload = argc > 1 && !strcmp (argv[1], "--upload");
    if (g_getenv ("VIDEO_TEST_SLOW"))
        slow = CLAMP (atoi (g_getenv ("VIDEO_TEST_SLOW")), 1, 100);
    gboolean own = argc > 1 && !only_upload;
    char *clip = own ? g_strdup (argv[1]) : dir ? make_clip (dir) : NULL;
    if (!clip) {
        printf ("video-test: ffmpeg with libx264 is not here: the decode, frame limit, hardware and GL "
                "parts are SKIPPED\n");
    } else if (ref_decode (clip)) {
        printf ("clip: %dx%d, %u pictures, %d of them not a reference; layout %s\n", ref_w, ref_h, refs->len,
                ref_nonref, nv ? "NV12 (test layout of hardware decoders)" : "three planes");
        if (own) {
            test_hw (clip);
        } else if (only_upload) {
            test_gl (clip);
        } else {
            static const Case cases[] = {
                { "default, full size", FALSE, 0, 0, FALSE, 0 },
                { "default, half size", TRUE, 0, 0, FALSE, 0 },
                { "limit on, monitor limit 60 = the clip's rate", FALSE, 60, 0, TRUE, 0 },
                { "limit on, monitor limit 30", TRUE, 30, 0, TRUE, 30 },
                { "limit on, monitor limit 24", FALSE, 24, 0, TRUE, 24 },
                { "limit on, scene measured at 29.5", TRUE, 0, 29.5, TRUE, 30 },
                { "limit OFF, monitor limit 30 (nothing may change)", FALSE, 30, 0, FALSE, 0 },
            };
            for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
                run_case (clip, &cases[i]);
            test_hw (clip);
            test_hw_fake (clip);
            test_gl (clip);
        }
    }
    if (clip && !own)
        g_remove (clip);
    if (dir)
        g_rmdir (dir);
    g_free (clip);
    g_free (dir);
    printf (fails ? "video-test: %d FAILED\n" : "video-test: all passed\n", fails);
    return fails ? 1 : 0;
}
