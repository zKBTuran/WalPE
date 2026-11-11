/* video_hwsel_test.c - the rules of hardware video decoding
 * (src/engine/video_hwsel.h), without FFmpeg, GL or hardware: made-up
 * offers, made-up machines, made-up failures.
 *
 * 1. The setting's words: off | auto | <api>, the older switch values.
 * 2. The plan: which API on which device in which order, for a machine
 *    with one GPU, with two, with a render GPU that reports a compute
 *    device, with none that is known, for a software renderer, for an ARM
 *    board with decoder blocks, and for one API asked for by name.
 * 3. The attempts: a failure before the first picture moves on, a failure
 *    in the middle of the stream ends in software decoding, a closed
 *    decoder starts again with the attempt that worked, a new plan forgets.
 * 4. FFmpeg's words read as a kind of reason.
 * 5. The status line, letter for letter.
 * 6. The DMA-BUF layouts of the zero-copy path: what is taken, as what,
 *    and what is refused (the colour-format table); the sample comparison.
 * 7. The codec profile text from a sequence parameter set. */
#include <glib.h>
#include <stdio.h>
#include "engine/video_hwsel.h"

static int fails;
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fails++; \
            printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf (__VA_ARGS__); \
            printf ("\n"); \
        } \
    } while (0)

/* ------------------------------------------------------------ helpers */

static WalpeHwOffer offer (const char *api, const char *codec, WalpeHwKind kind, const char *dev)
{
    WalpeHwOffer o;
    memset (&o, 0, sizeof o);
    g_strlcpy (o.api, api, sizeof (o.api));
    g_strlcpy (o.codec, codec, sizeof (o.codec));
    g_strlcpy (o.dev_type, dev, sizeof (o.dev_type));
    o.kind = kind;
    return o;
}

/* what a desktop FFmpeg build has: the hardware accelerations of its own
 * decoder in FFmpeg's order, and decoders of their own */
static int offers_desktop (WalpeHwOffer *o)
{
    int n = 0;
    o[n++] = offer ("vdpau", "h264", WALPE_HW_DEVICE, "vdpau");
    o[n++] = offer ("cuda", "h264", WALPE_HW_DEVICE, "cuda");
    o[n++] = offer ("vaapi", "h264", WALPE_HW_DEVICE, "vaapi");
    o[n++] = offer ("vulkan", "h264", WALPE_HW_DEVICE, "vulkan");
    o[n++] = offer ("v4l2m2m", "h264_v4l2m2m", WALPE_HW_WRAPPER, "");
    o[n++] = offer ("cuvid", "h264_cuvid", WALPE_HW_WRAPPER_DEVICE, "cuda");
    o[n++] = offer ("qsv", "h264_qsv", WALPE_HW_WRAPPER_DEVICE, "qsv");
    for (int i = 0; i < n; i++)
        o[i].cookie = i;
    return n;
}

static WalpeHwGpu gpu_none (void)
{
    WalpeHwGpu g;
    memset (&g, 0, sizeof g);
    g.ordinal = -1;
    return g;
}

static void gpu_add_node (WalpeHwGpu *g, const char *node, const char *driver)
{
    g_strlcpy (g->nodes[g->n_nodes], node, sizeof (g->nodes[0]));
    g_strlcpy (g->node_driver[g->n_nodes], driver, sizeof (g->node_driver[0]));
    g->n_nodes++;
}

/* the plan as one line: "api@device/where ..." */
static char *plan_text (const WalpeHwTry *t, int n)
{
    static const char *const where[] = { "same", "unknown", "other", "own" };
    GString *s = g_string_new (NULL);
    for (int i = 0; i < n; i++)
        g_string_append_printf (s, "%s%s@%s/%s%s%s", i ? " " : "", t[i].api, t[i].device[0] ? t[i].device : "-",
                                where[t[i].where], t[i].derive ? "/derived" : "", t[i].dmabuf ? "/dmabuf" : "");
    return g_string_free (s, FALSE);
}

static void expect_plan (const char *what, const WalpeHwOffer *o, int no, const WalpeHwGpu *g, const char *setting,
                         const char *want_text)
{
    WalpeHwWant w;
    WalpeHwTry t[WALPE_HW_MAX_TRIES];
    CHECK (walpe_hwsel_parse (setting, &w), "%s: setting \"%s\" is not taken", what, setting);
    int n = walpe_hwsel_plan (o, no, g, &w, t);
    char *got = plan_text (t, n);
    CHECK (!strcmp (got, want_text), "%s:\n      got  %s\n      want %s", what, got, want_text);
    g_free (got);
}

/* ------------------------------------------------------- 1. the setting */

static void test_setting (void)
{
    WalpeHwWant w;
    static const char *const off[] = { "off", "OFF", "0", "no", "false", " off ", "none" };
    static const char *const autos[] = { "auto", "on", "1", "yes", "true", "Auto", "auto\n" };
    for (guint i = 0; i < G_N_ELEMENTS (off); i++)
        CHECK (walpe_hwsel_parse (off[i], &w) && !w.on && !w.api[0], "\"%s\" is off", off[i]);
    for (guint i = 0; i < G_N_ELEMENTS (autos); i++)
        CHECK (walpe_hwsel_parse (autos[i], &w) && w.on && !w.api[0], "\"%s\" is auto", autos[i]);
    CHECK (walpe_hwsel_parse ("vaapi", &w) && w.on && !strcmp (w.api, "vaapi"), "vaapi by name");
    CHECK (walpe_hwsel_parse ("VAAPI", &w) && !strcmp (w.api, "vaapi"), "names are not case sensitive");
    CHECK (walpe_hwsel_parse ("v4l2m2m", &w) && !strcmp (w.api, "v4l2m2m"), "v4l2m2m by name");
    CHECK (!walpe_hwsel_parse ("", &w) && !w.on, "an empty word is not a setting");
    CHECK (!walpe_hwsel_parse (NULL, &w), "NULL is not a setting");
    CHECK (!walpe_hwsel_parse ("va api", &w) && !walpe_hwsel_parse ("../x", &w) && !walpe_hwsel_parse ("a;b", &w),
           "only letters, digits and _ make a name");
    CHECK (!walpe_hwsel_parse ("abcdefghijklmnopqrstuvwxyz", &w), "a name of 26 letters is too long");
    CHECK (walpe_hwsel_parse ("off", NULL), "the parser works without a place for the result");
    walpe_hwsel_parse ("off", &w);
    CHECK (!strcmp (walpe_hwsel_want_name (&w), "off"), "the word for off");
    walpe_hwsel_parse ("on", &w);
    CHECK (!strcmp (walpe_hwsel_want_name (&w), "auto"), "the word for on is auto");
    walpe_hwsel_parse ("cuda", &w);
    CHECK (!strcmp (walpe_hwsel_want_name (&w), "cuda"), "the word for an API is its name");
    printf ("setting words: done\n");
}

/* ---------------------------------------------------------- 2. the plan */

static void test_plan (void)
{
    WalpeHwOffer o[16];
    int no = offers_desktop (o);

    /* one GPU whose EGL device names its render node and reports no compute device */
    WalpeHwGpu one = gpu_none ();
    one.known = one.ordinal_known = TRUE;
    g_strlcpy (one.render_node, "/dev/dri/renderD128", sizeof (one.render_node));
    g_strlcpy (one.driver, "amdgpu", sizeof (one.driver));
    gpu_add_node (&one, "/dev/dri/renderD128", "amdgpu");
    one.x86_64 = TRUE;      /* a desktop PC: FFmpeg's V4L2 stateful decoder is not part of auto there */
    /* (its only GPU: the API that is opened by a compute device number cannot be on another one) */
    expect_plan ("one GPU, no compute device", o, no, &one, "auto",
                 "vaapi@/dev/dri/renderD128/same/dmabuf cuda@-/unknown vdpau@-/unknown vulkan@-/same/derived");

    /* the same machine on another architecture: FFmpeg's V4L2 stateful decoder is the last attempt of auto
     * (ARM boards decode through it). On x86-64 auto leaves it out - opening it makes FFmpeg open every
     * /dev/video* node, webcams too - and it is planned only when it is asked for by name */
    WalpeHwGpu other_arch = one;
    other_arch.x86_64 = FALSE;
    expect_plan ("not x86-64: v4l2m2m is the last attempt of auto", o, no, &other_arch, "auto",
                 "vaapi@/dev/dri/renderD128/same/dmabuf cuda@-/unknown vdpau@-/unknown vulkan@-/same/derived "
                 "v4l2m2m@-/own/dmabuf");
    expect_plan ("x86-64: v4l2m2m by name", o, no, &one, "v4l2m2m", "v4l2m2m@-/own/dmabuf");

    /* the render GPU reports compute device 0: the API that is opened by that number comes first */
    WalpeHwGpu comp = one;
    comp.ordinal = 0;
    g_strlcpy (comp.driver, "gpudrv", sizeof (comp.driver));
    expect_plan ("render GPU with compute device 0", o, no, &comp, "auto",
                 "cuda@0/same vaapi@/dev/dri/renderD128/same/dmabuf vdpau@-/unknown vulkan@-/same/derived");
    comp.ordinal = 1;
    expect_plan ("compute device 1 is opened as 1", o, no, &comp, "cuda", "cuda@1/same");

    /* two GPUs, the one on renderD129 renders and reports no compute device:
     * its own decoder first, the other GPU's decoders after every API that picks its device, the late
     * API after all of them */
    WalpeHwGpu two = gpu_none ();
    two.known = two.ordinal_known = TRUE;
    g_strlcpy (two.render_node, "/dev/dri/renderD129", sizeof (two.render_node));
    gpu_add_node (&two, "/dev/dri/renderD128", "drv_a");
    gpu_add_node (&two, "/dev/dri/renderD129", "drv_b");
    two.x86_64 = TRUE;
    expect_plan ("two GPUs, renderD129 renders", o, no, &two, "auto",
                 "vaapi@/dev/dri/renderD129/same/dmabuf vdpau@-/unknown cuda@-/other "
                 "vaapi@/dev/dri/renderD128/other vulkan@-/same/derived vulkan@-/other/derived");
    {
        WalpeHwWant w;
        WalpeHwTry t[WALPE_HW_MAX_TRIES];
        walpe_hwsel_parse ("vaapi", &w);
        int n = walpe_hwsel_plan (o, no, &two, &w, t);
        CHECK (n == 2 && !strcmp (t[0].driver, "drv_b") && !strcmp (t[1].driver, "drv_a") &&
               !strcmp (t[1].node, "/dev/dri/renderD128"), "each attempt carries its node's driver for the message");
    }

    /* nothing is known about the renderer (no EGL device query): the order the module always had */
    WalpeHwGpu unk = gpu_none ();
    gpu_add_node (&unk, "/dev/dri/renderD128", "");
    unk.x86_64 = TRUE;
    expect_plan ("render GPU not known", o, no, &unk, "auto",
                 "cuda@-/unknown vaapi@-/unknown vdpau@-/unknown vulkan@-/unknown");

    /* the render node is known but the device could not be asked for a compute device (the node came
     * from the daemon's record, or the query failed): not asked is not "none". The API that is opened by
     * a compute device number keeps its place in front, as in the order the module always had; the
     * status says that the API chose the device */
    WalpeHwGpu half = one;
    half.ordinal_known = FALSE;
    expect_plan ("node known, compute device not asked", o, no, &half, "auto",
                 "cuda@-/unknown vaapi@/dev/dri/renderD128/same/dmabuf vdpau@-/unknown vulkan@-/same/derived");
    /* the same with two GPUs: still not suspected of being the other one */
    WalpeHwGpu half2 = two;
    half2.ordinal_known = FALSE;
    expect_plan ("two GPUs, compute device not asked", o, no, &half2, "auto",
                 "cuda@-/unknown vaapi@/dev/dri/renderD129/same/dmabuf vdpau@-/unknown "
                 "vaapi@/dev/dri/renderD128/other vulkan@-/same/derived vulkan@-/other/derived");

    /* NVIDIA's own driver renders (the one rule that goes by a name, WalpeHwGpu.nvidia_driver): cuda is the
     * first attempt also when the EGL device reports no compute device. One GPU: */
    WalpeHwGpu nv = one;
    g_strlcpy (nv.driver, "nvidia", sizeof (nv.driver));
    nv.nvidia_driver = TRUE;
    expect_plan ("NVIDIA's driver, one GPU, no compute device reported", o, no, &nv, "auto",
                 "cuda@-/unknown vaapi@/dev/dri/renderD128/same/dmabuf vdpau@-/unknown vulkan@-/same/derived");
    /* several GPUs: cuda is not "another GPU" there (without the flag this machine plans
     * vaapi, vdpau, cuda/other: "two GPUs, renderD129 renders" above) */
    WalpeHwGpu nv2 = two;
    nv2.nvidia_driver = TRUE;
    expect_plan ("NVIDIA's driver renders, several GPUs, no compute device reported", o, no, &nv2, "auto",
                 "cuda@-/unknown vaapi@/dev/dri/renderD129/same/dmabuf vdpau@-/unknown "
                 "vaapi@/dev/dri/renderD128/other vulkan@-/same/derived vulkan@-/other/derived");
    {
        /* what the status says then: the API chose the device (which of that driver's GPUs is its choice) */
        WalpeHwWant w;
        WalpeHwSel s;
        WalpeHwNote nt = { 3840, 2160, 60.0, NULL, 1, TRUE, FALSE, FALSE, NULL };
        memset (&s, 0, sizeof s);
        walpe_hwsel_parse ("auto", &w);
        walpe_hwsel_start (&s, o, no, &nv2, &w);
        const WalpeHwTry *t = walpe_hwsel_next (&s);
        walpe_hwsel_picture (&s);
        char *got = walpe_hwsel_note (&s, &nt);
        CHECK (t && !strcmp (t->api, "cuda") &&
               !strcmp (got, "3840x2160 60.00 fps: hardware (cuda, device chosen by the API; pictures are "
                             "downloaded to main memory)"), "NVIDIA's driver, status: %s", got);
        g_free (got);
        walpe_hwsel_clear (&s);
    }
    /* the compute device was not asked, or is reported: first either way, on the reported number when there is one */
    WalpeHwGpu nv3 = nv;
    nv3.ordinal_known = FALSE;
    expect_plan ("NVIDIA's driver, compute device not asked", o, no, &nv3, "auto",
                 "cuda@-/unknown vaapi@/dev/dri/renderD128/same/dmabuf vdpau@-/unknown vulkan@-/same/derived");
    nv3.ordinal_known = TRUE;
    nv3.ordinal = 0;
    expect_plan ("NVIDIA's driver, compute device 0 reported", o, no, &nv3, "auto",
                 "cuda@0/same vaapi@/dev/dri/renderD128/same/dmabuf vdpau@-/unknown vulkan@-/same/derived");
    /* nothing else known about the renderer than that driver (its EGL vendor string) */
    WalpeHwGpu nvu = unk;
    nvu.nvidia_driver = TRUE;
    expect_plan ("NVIDIA's driver, render GPU not identified", o, no, &nvu, "auto",
                 "cuda@-/unknown vaapi@-/unknown vdpau@-/unknown vulkan@-/unknown");
    /* the flag decides nothing else: one API by name is that API alone */
    expect_plan ("vaapi by name on NVIDIA's driver", o, no, &nv, "vaapi", "vaapi@/dev/dri/renderD128/same/dmabuf");
    expect_plan ("cuda by name on NVIDIA's driver", o, no, &nv, "cuda", "cuda@-/unknown");

    /* a software renderer: nothing */
    WalpeHwGpu sw = one;
    sw.software = TRUE;
    expect_plan ("software renderer", o, no, &sw, "auto", "");
    expect_plan ("setting off", o, no, &one, "off", "");

    /* by name: only that one, on the render GPU first; a decoder of its own that needs a device too */
    expect_plan ("vaapi by name", o, no, &one, "vaapi", "vaapi@/dev/dri/renderD128/same/dmabuf");
    expect_plan ("vdpau by name", o, no, &one, "vdpau", "vdpau@-/unknown");
    expect_plan ("cuvid by name takes the compute device", o, no, &comp, "cuvid", "cuvid@1/same");
    expect_plan ("an API this build does not have", o, no, &one, "d3d11va", "");

    /* an ARM board: no desktop API works there; FFmpeg has the stateless decoders as device type "drm"
     * and the stateful ones as a decoder of its own */
    WalpeHwOffer a[8];
    int na = 0;
    a[na++] = offer ("vaapi", "h264", WALPE_HW_DEVICE, "vaapi");
    a[na++] = offer ("drm", "h264", WALPE_HW_DEVICE, "drm");
    a[na++] = offer ("v4l2m2m", "h264_v4l2m2m", WALPE_HW_WRAPPER, "");
    a[na++] = offer ("rkmpp", "h264_rkmpp", WALPE_HW_WRAPPER, "");
    WalpeHwGpu arm = gpu_none ();
    arm.known = arm.ordinal_known = TRUE;
    g_strlcpy (arm.render_node, "/dev/dri/renderD128", sizeof (arm.render_node));
    gpu_add_node (&arm, "/dev/dri/renderD128", "panfrost");
    expect_plan ("ARM board", a, na, &arm, "auto",
                 "vaapi@/dev/dri/renderD128/same/dmabuf drm@/dev/dri/renderD128/own/dmabuf v4l2m2m@-/own/dmabuf "
                 "rkmpp@-/own/dmabuf");
    WalpeHwGpu bare = gpu_none ();
    expect_plan ("ARM board without any DRM node: the request API has nothing to be handed", a, na, &bare, "drm", "");

    /* a device type the table does not know is still tried, after the known ones */
    WalpeHwOffer x[4];
    x[0] = offer ("newapi", "h264", WALPE_HW_DEVICE, "newapi");
    x[1] = offer ("vaapi", "h264", WALPE_HW_DEVICE, "vaapi");
    x[2] = offer ("newdec", "h264_newdec", WALPE_HW_WRAPPER, "");
    expect_plan ("unknown names", x, 3, &unk, "auto", "vaapi@-/unknown newapi@-/unknown newdec@-/own/dmabuf");

    /* more attempts than the plan holds: cut, not overrun */
    WalpeHwGpu many = one;
    for (int i = 1; i < WALPE_HW_MAX_NODES; i++) {
        char node[32];
        g_snprintf (node, sizeof node, "/dev/dri/renderD%d", 128 + i);
        gpu_add_node (&many, node, "x");
    }
    WalpeHwOffer v[8];
    for (int i = 0; i < 8; i++)
        v[i] = offer ("vaapi", "h264", WALPE_HW_DEVICE, "vaapi");
    WalpeHwWant w;
    WalpeHwTry t[WALPE_HW_MAX_TRIES];
    walpe_hwsel_parse ("auto", &w);
    CHECK (walpe_hwsel_plan (v, 8, &many, &w, t) == WALPE_HW_MAX_TRIES, "the plan is cut at its size");
    printf ("plan: done\n");
}

/* ------------------------------------------------------ 3. the attempts */

static void test_attempts (void)
{
    WalpeHwOffer o[16];
    int no = offers_desktop (o);
    /* (a machine that is not x86-64: FFmpeg's V4L2 stateful decoder is the fifth attempt of auto) */
    WalpeHwGpu g = gpu_none ();
    g.known = g.ordinal_known = TRUE;
    g_strlcpy (g.render_node, "/dev/dri/renderD128", sizeof (g.render_node));
    gpu_add_node (&g, "/dev/dri/renderD128", "amdgpu");
    WalpeHwWant w;
    WalpeHwSel s;
    memset (&s, 0, sizeof s);
    walpe_hwsel_parse ("auto", &w);
    walpe_hwsel_start (&s, o, no, &g, &w);
    CHECK (s.n == 5 && s.cur == -1 && s.wanted, "five attempts planned, none current");

    /* the device of the first cannot be opened by this user; the second does not take the stream;
     * the third works and then fails in the middle of the clip */
    const WalpeHwTry *t = walpe_hwsel_next (&s);
    CHECK (t && !strcmp (t->api, "vaapi") && walpe_hwsel_current (&s) == t, "first attempt: vaapi");
    CHECK (walpe_hwsel_failed (&s, WALPE_HW_WHY_PERMISSION, "/dev/dri/renderD128 may not be opened"),
           "a failure before a picture: another attempt may be made");
    t = walpe_hwsel_next (&s);
    CHECK (t && !strcmp (t->api, "cuda") && t->where == WALPE_HW_UNKNOWN, "second attempt: cuda, device not known");
    CHECK (walpe_hwsel_failed (&s, WALPE_HW_WHY_NO_DEVICE, "Input/output error (libcuda.so.1 cannot be loaded)"),
           "still more to try");
    CHECK (!strcmp (s.tries[3].api, "vulkan") && s.tries[3].derive && s.tries[3].where == WALPE_HW_SAME,
           "the late API is the fourth attempt, derived from the render node");
    t = walpe_hwsel_next (&s);
    CHECK (t && !strcmp (t->api, "vdpau") && !s.seen, "third attempt: vdpau, no picture yet");
    walpe_hwsel_picture (&s);
    CHECK (s.seen, "it delivers pictures");
    /* the decoder is closed without a failure (parked): the same attempt is taken again */
    walpe_hwsel_closed (&s);
    CHECK (s.cur == -1 && !s.seen, "closed: nothing current");
    t = walpe_hwsel_next (&s);
    CHECK (t && !strcmp (t->api, "vdpau"), "reopened: the attempt that worked, not the failed ones");
    walpe_hwsel_picture (&s);
    /* a failure in the middle of the stream: software, and it stays software */
    CHECK (!walpe_hwsel_failed (&s, WALPE_HW_WHY_DECODE, "8 decode calls in a row failed"),
           "a failure after pictures: no other attempt, software decoding");
    CHECK (s.gave_up && walpe_hwsel_next (&s) == NULL && s.cur == -1, "software for the rest of the stream");
    CHECK (walpe_hwsel_next (&s) == NULL, "asked again: still software");
    char *why = walpe_hwsel_reasons (&s);
    CHECK (why && !strcmp (why,
           "vaapi on /dev/dri/renderD128 [amdgpu]: permission (/dev/dri/renderD128 may not be opened); "
           "cuda: no device (Input/output error (libcuda.so.1 cannot be loaded)); "
           "vdpau: decoding failed (8 decode calls in a row failed)"), "reasons: %s", why ? why : "(null)");
    g_free (why);

    /* the setting is switched: a new plan, every attempt is tried again */
    walpe_hwsel_start (&s, o, no, &g, &w);
    CHECK (!s.gave_up && s.why[0] == WALPE_HW_WHY_NONE && walpe_hwsel_reasons (&s) == NULL, "a new plan forgets");
    /* every attempt fails before a picture: the last failure says that nothing is left */
    int tried = 0;
    gboolean more = TRUE;
    while ((t = walpe_hwsel_next (&s))) {
        tried++;
        more = walpe_hwsel_failed (&s, WALPE_HW_WHY_NO_DEVICE, "Input/output error");
    }
    CHECK (tried == 5 && !more && !s.gave_up, "all five tried, the last one leaves nothing: %d", tried);
    /* failing with nothing current changes nothing */
    CHECK (!walpe_hwsel_failed (&s, WALPE_HW_WHY_DECODE, "x") && s.why[0] == WALPE_HW_WHY_NO_DEVICE,
           "a failure without a current attempt is not recorded");
    /* a detail longer than the status line can carry is cut, and is plain ASCII afterwards */
    walpe_hwsel_start (&s, o, no, &g, &w);
    walpe_hwsel_next (&s);
    char longtext[600];
    memset (longtext, 'x', sizeof longtext - 1);
    longtext[sizeof longtext - 1] = 0;
    longtext[10] = (char) 0xc3;
    longtext[11] = '\n';
    walpe_hwsel_failed (&s, WALPE_HW_WHY_NO_DEVICE, longtext);
    CHECK (s.text[0] && strlen (s.text[0]) == 240 && s.text[0][10] == '?' && s.text[0][11] == '?', "details are cut "
           "to 240 bytes of plain ASCII");
    /* a reason without a kind is still a failure */
    walpe_hwsel_next (&s);
    walpe_hwsel_failed (&s, WALPE_HW_WHY_NONE, NULL);
    CHECK (s.why[1] == WALPE_HW_WHY_REFUSED, "a failure without a kind counts as a refusal");

    /* nothing to try: the reason is general */
    walpe_hwsel_parse ("d3d11va", &w);
    walpe_hwsel_start (&s, o, no, &g, &w);
    why = walpe_hwsel_reasons (&s);
    CHECK (s.n == 0 && why && !strcmp (why, "codec unsupported: d3d11va: this FFmpeg build has no H.264 decoding "
           "with it"), "an API that is not built in: %s", why ? why : "(null)");
    g_free (why);
    walpe_hwsel_parse ("auto", &w);
    walpe_hwsel_start (&s, NULL, 0, &g, &w);
    why = walpe_hwsel_reasons (&s);
    CHECK (why && !strcmp (why, "codec unsupported: this FFmpeg build has no hardware decoding for H.264"),
           "a build without any: %s", why ? why : "(null)");
    g_free (why);
    {
        /* x86-64, and the build's only hardware decoder is the one auto leaves out there: the reason says
         * so and names the way to it; asked for by name it is planned */
        WalpeHwOffer only[1];
        only[0] = offer ("v4l2m2m", "h264_v4l2m2m", WALPE_HW_WRAPPER, "");
        WalpeHwGpu pc = g;
        pc.x86_64 = TRUE;
        walpe_hwsel_start (&s, only, 1, &pc, &w);
        why = walpe_hwsel_reasons (&s);
        CHECK (s.n == 0 && why && !strcmp (why, "codec unsupported: this FFmpeg build has no hardware decoding for "
               "H.264 that auto tries here (its V4L2 decoder is tried by name only on x86-64: hwdec=v4l2m2m)"),
               "x86-64, V4L2 decoder only, auto: %s", why ? why : "(null)");
        g_free (why);
        walpe_hwsel_start (&s, only, 1, &g, &w);
        CHECK (s.n == 1, "the same build on another architecture: one attempt in auto");
        walpe_hwsel_parse ("v4l2m2m", &w);
        walpe_hwsel_start (&s, only, 1, &pc, &w);
        CHECK (s.n == 1 && !strcmp (s.tries[0].api, "v4l2m2m"), "x86-64: by name it is planned");
        walpe_hwsel_parse ("auto", &w);
    }
    WalpeHwGpu sw = g;
    sw.software = TRUE;
    walpe_hwsel_start (&s, o, no, &sw, &w);
    why = walpe_hwsel_reasons (&s);
    CHECK (s.n == 0 && why && !strcmp (why, "the renderer is a software renderer"), "software renderer: %s",
           why ? why : "(null)");
    g_free (why);
    walpe_hwsel_parse ("off", &w);
    walpe_hwsel_start (&s, o, no, &g, &w);
    CHECK (!s.wanted && s.n == 0 && walpe_hwsel_reasons (&s) == NULL && walpe_hwsel_next (&s) == NULL,
           "off: nothing planned, no reason");
    walpe_hwsel_clear (&s);
    printf ("attempts: done\n");
}

/* ----------------------------------------------------- 4. FFmpeg's words */

static void test_classify (void)
{
    static const struct { const char *text; WalpeHwWhy want; } c[] = {
        { "No support for codec h264 profile 110 level 51.", WALPE_HW_WHY_PROFILE },
        { "Hardware does not support image size 8192x4320 (constraints: width 16-4096 height 16-4096).",
          WALPE_HW_WHY_SIZE },
        { "Video width 8192 not within range from 48 to 4096", WALPE_HW_WHY_SIZE },
        { "Video macroblock count 138240 exceeds maximum of 65536", WALPE_HW_WHY_SIZE },
        { "Unsupported bit depth", WALPE_HW_WHY_PROFILE },
        { "Hardware is lacking required capabilities", WALPE_HW_WHY_PROFILE },
        { "Failed to open /dev/dri/renderD128: Permission denied", WALPE_HW_WHY_PERMISSION },
        { "Failed setup for format vaapi: hwaccel initialisation returned error.", WALPE_HW_WHY_REFUSED },
        { "", WALPE_HW_WHY_REFUSED },
    };
    for (guint i = 0; i < G_N_ELEMENTS (c); i++)
        CHECK (walpe_hwsel_classify (c[i].text, WALPE_HW_WHY_REFUSED) == c[i].want, "\"%s\" is read as %s", c[i].text,
               walpe_hwsel_why_name (walpe_hwsel_classify (c[i].text, WALPE_HW_WHY_REFUSED)));
    CHECK (walpe_hwsel_classify (NULL, WALPE_HW_WHY_DECODE) == WALPE_HW_WHY_DECODE, "no text: the default");
    for (int w = WALPE_HW_WHY_NONE + 1; w < WALPE_HW_WHY_N; w++)
        CHECK (*walpe_hwsel_why_name ((WalpeHwWhy) w), "reason %d has a name", w);
    printf ("reasons from FFmpeg's log: done\n");
}

/* ------------------------------------------------------ 5. status line */

static void expect_note (const char *what, const WalpeHwSel *s, const WalpeHwNote *nt, const char *want)
{
    char *got = walpe_hwsel_note (s, nt);
    CHECK (!strcmp (got, want), "status, %s:\n      got  %s\n      want %s", what, got, want);
    g_free (got);
}

static void test_status (void)
{
    WalpeHwOffer o[16];
    int no = offers_desktop (o);
    WalpeHwGpu g = gpu_none ();
    g.known = g.ordinal_known = TRUE;
    g_strlcpy (g.render_node, "/dev/dri/renderD129", sizeof (g.render_node));
    gpu_add_node (&g, "/dev/dri/renderD128", "drv_a");
    gpu_add_node (&g, "/dev/dri/renderD129", "drv_b");
    WalpeHwWant w;
    WalpeHwSel s;
    memset (&s, 0, sizeof s);
    WalpeHwNote nt = { 1920, 1080, 60.0, "H.264 High@L4.2, 8-bit 4:2:0", 2, FALSE, FALSE, FALSE, NULL };

    walpe_hwsel_parse ("off", &w);
    walpe_hwsel_start (&s, o, no, &g, &w);
    expect_note ("off", &s, &nt, "1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: software, 2 threads");
    nt.threads = 1;
    nt.profile = NULL;
    expect_note ("off, one thread, profile not known", &s, &nt, "1920x1080 60.00 fps: software, 1 thread");
    nt.profile = "H.264 High@L4.2, 8-bit 4:2:0";

    walpe_hwsel_parse ("vaapi", &w);
    walpe_hwsel_start (&s, o, no, &g, &w);
    walpe_hwsel_next (&s);
    nt.hw = TRUE;
    expect_note ("hardware on the render GPU, opened", &s, &nt,
                 "1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: hardware (vaapi on /dev/dri/renderD129 [drv_b], "
                 "the GPU that renders; pictures are downloaded to main memory), no picture yet");
    walpe_hwsel_picture (&s);
    expect_note ("hardware on the render GPU", &s, &nt,
                 "1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: hardware (vaapi on /dev/dri/renderD129 [drv_b], "
                 "the GPU that renders; pictures are downloaded to main memory)");
    nt.zero_copy = TRUE;
    expect_note ("zero-copy", &s, &nt,
                 "1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: hardware (vaapi on /dev/dri/renderD129 [drv_b], "
                 "the GPU that renders; zero-copy: the pictures stay in GPU memory)");
    nt.zero_copy = FALSE;
    nt.zc_why = "the renderer's EGL has no EGL_EXT_image_dma_buf_import";
    expect_note ("zero-copy asked for and not in use", &s, &nt,
                 "1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: hardware (vaapi on /dev/dri/renderD129 [drv_b], "
                 "the GPU that renders; pictures are downloaded to main memory, zero-copy is not in use: the "
                 "renderer's EGL has no EGL_EXT_image_dma_buf_import)");
    nt.zc_why = NULL;
    /* it fails after pictures were shown: the other GPU is not tried, software decodes */
    walpe_hwsel_failed (&s, WALPE_HW_WHY_DECODE, "8 decode calls in a row failed: Input/output error");
    nt.hw = FALSE;
    nt.threads = 3;
    expect_note ("failed in the middle of the clip", &s, &nt,
                 "1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: software, 3 threads (hardware decoding was asked "
                 "for and is not in use: vaapi on /dev/dri/renderD129 [drv_b]: decoding failed (8 decode calls in a "
                 "row failed: Input/output error))");

    /* the render GPU's decoder is refused before a picture, the other GPU's takes over */
    walpe_hwsel_start (&s, o, no, &g, &w);
    walpe_hwsel_next (&s);
    walpe_hwsel_failed (&s, WALPE_HW_WHY_SIZE, "Hardware does not support image size 8192x4320");
    walpe_hwsel_next (&s);
    walpe_hwsel_picture (&s);
    nt.hw = TRUE;
    expect_note ("the other GPU decodes", &s, &nt,
                 "1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: hardware (vaapi on /dev/dri/renderD128 [drv_a], "
                 "NOT the GPU that renders: every picture is copied through main memory and that GPU is kept awake; "
                 "pictures are downloaded to main memory)");

    /* an API that picks its device, and a decoder block */
    walpe_hwsel_parse ("vdpau", &w);
    walpe_hwsel_start (&s, o, no, &g, &w);
    walpe_hwsel_next (&s);
    walpe_hwsel_picture (&s);
    expect_note ("device chosen by the API", &s, &nt,
                 "1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: hardware (vdpau, device chosen by the API; "
                 "pictures are downloaded to main memory)");
    walpe_hwsel_parse ("v4l2m2m", &w);
    walpe_hwsel_start (&s, o, no, &g, &w);
    walpe_hwsel_next (&s);
    walpe_hwsel_picture (&s);
    nt.in_memory = TRUE;
    expect_note ("decoder block", &s, &nt,
                 "1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: hardware (v4l2m2m, decoding hardware of its own; "
                 "the decoder hands its pictures out in main memory)");
    nt.in_memory = FALSE;

    /* the kinds of reason in one line: no device, permission, profile, resolution */
    walpe_hwsel_parse ("auto", &w);
    g.ordinal = 0;
    walpe_hwsel_start (&s, o, no, &g, &w);
    walpe_hwsel_next (&s);
    walpe_hwsel_failed (&s, WALPE_HW_WHY_NO_DEVICE, "Input/output error (libcuda.so.1 cannot be loaded)");
    walpe_hwsel_next (&s);
    walpe_hwsel_failed (&s, WALPE_HW_WHY_PERMISSION, "/dev/dri/renderD129 may not be opened by this user");
    walpe_hwsel_next (&s);
    walpe_hwsel_failed (&s, WALPE_HW_WHY_PROFILE, "FFmpeg has no hardware decoding for this stream's format "
                        "(yuv420p10le)");
    walpe_hwsel_next (&s);
    walpe_hwsel_failed (&s, WALPE_HW_WHY_SIZE, NULL);
    nt.hw = FALSE;
    nt.threads = 4;
    expect_note ("four attempts failed, the rest not tried yet", &s, &nt,
                 "1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: software, 4 threads (hardware decoding was asked "
                 "for and is not in use: cuda device 0 on /dev/dri/renderD129 [drv_b]: no device (Input/output error "
                 "(libcuda.so.1 cannot be loaded)); vaapi on /dev/dri/renderD129 [drv_b]: permission "
                 "(/dev/dri/renderD129 may not be opened by this user); vdpau: profile (FFmpeg has no hardware "
                 "decoding for this stream's format (yuv420p10le)); vaapi on /dev/dri/renderD128 [drv_a]: "
                 "resolution outside the decoder's limits)");
    walpe_hwsel_clear (&s);
    printf ("status line: done\n");
}

/* --------------------------------------- 6. DMA-BUF layouts (zero-copy) */

/* a picture exported as separate layers: R8 + GR88 in one buffer */
static WalpeDmaDesc desc_layers (guint32 f0, guint32 f1, gsize pitch, gsize off1, gsize size, guint64 mod)
{
    WalpeDmaDesc d;
    memset (&d, 0, sizeof d);
    d.nb_objects = 1;
    d.modifier[0] = mod;
    d.size[0] = size;
    d.nb_layers = 2;
    d.layers[0].format = f0;
    d.layers[0].nb_planes = 1;
    d.layers[0].planes[0].pitch = pitch;
    d.layers[1].format = f1;
    d.layers[1].nb_planes = 1;
    d.layers[1].planes[0].offset = off1;
    d.layers[1].planes[0].pitch = pitch;
    return d;
}

static WalpeDmaDesc desc_one (guint32 fmt, int nplanes, gsize pitch, gsize cpitch, int h, int ch)
{
    WalpeDmaDesc d;
    memset (&d, 0, sizeof d);
    d.nb_objects = 1;
    d.modifier[0] = WALPE_DRM_MOD_LINEAR;
    d.nb_layers = 1;
    d.layers[0].format = fmt;
    d.layers[0].nb_planes = nplanes;
    gsize off = 0;
    for (int p = 0; p < nplanes; p++) {
        d.layers[0].planes[p].offset = off;
        d.layers[0].planes[p].pitch = p ? cpitch : pitch;
        off += (p ? cpitch : pitch) * (gsize) (p ? ch : h);
    }
    d.size[0] = off;
    return d;
}

static void test_dma (void)
{
    WalpeDmaPlane pl[3];
    const char *why = NULL;
    WalpeDmaWant nv12 = { 2, 1, 1920, 1088, 1, 1 };
    WalpeDmaWant p010 = { 2, 2, 1920, 1088, 1, 1 };
    WalpeDmaWant i420 = { 3, 1, 1920, 1088, 1, 1 };

    /* NV12 as two layers (what VA-API exports): R8 + GR88 */
    WalpeDmaDesc d = desc_layers (WALPE_DRM_R8, WALPE_DRM_GR88, 1920, 1920 * 1088, 1920 * 1088 * 3 / 2,
                                  WALPE_DRM_MOD_LINEAR);
    CHECK (walpe_dma_planes (&d, &nv12, pl, &why) && !why, "NV12 as R8 + GR88 is taken (%s)", why ? why : "");
    CHECK (pl[0].fourcc == WALPE_DRM_R8 && pl[0].w == 1920 && pl[0].h == 1088 && pl[0].pitch == 1920 &&
           pl[0].offset == 0 && pl[1].fourcc == WALPE_DRM_GR88 && pl[1].w == 960 && pl[1].h == 544 &&
           pl[1].offset == 1920 * 1088 && pl[1].modifier == WALPE_DRM_MOD_LINEAR, "its planes: Y 1920x1088, UV 960x544");
    /* the same in one layer NV12 */
    d = desc_one (WALPE_DRM_NV12, 2, 1920, 1920, 1088, 544);
    CHECK (walpe_dma_planes (&d, &nv12, pl, &why) && pl[0].fourcc == WALPE_DRM_R8 && pl[1].fourcc == WALPE_DRM_GR88 &&
           pl[1].offset == 1920 * 1088, "NV12 in one layer is split into R8 + GR88");
    /* P010 both ways */
    d = desc_layers (WALPE_DRM_R16, WALPE_DRM_GR1616, 3840, 3840 * 1088, 3840 * 1088 * 3 / 2, WALPE_DRM_MOD_LINEAR);
    CHECK (walpe_dma_planes (&d, &p010, pl, &why) && pl[0].fourcc == WALPE_DRM_R16 && pl[1].fourcc == WALPE_DRM_GR1616,
           "P010 as R16 + GR1616 is taken");
    d = desc_one (WALPE_DRM_P010, 2, 3840, 3840, 1088, 544);
    CHECK (walpe_dma_planes (&d, &p010, pl, &why) && pl[0].fourcc == WALPE_DRM_R16 && pl[1].fourcc == WALPE_DRM_GR1616,
           "P010 in one layer is split into R16 + GR1616");
    d = desc_one (WALPE_DRM_P016, 2, 3840, 3840, 1088, 544);
    CHECK (walpe_dma_planes (&d, &p010, pl, &why), "P016 likewise");
    /* three planes, and the same with V before U */
    d = desc_one (WALPE_DRM_YUV420, 3, 1920, 960, 1088, 544);
    CHECK (walpe_dma_planes (&d, &i420, pl, &why) && pl[1].offset == 1920 * 1088 &&
           pl[2].offset == 1920 * 1088 + 960 * 544 && pl[2].fourcc == WALPE_DRM_R8, "YU12: three R8 planes");
    d = desc_one (WALPE_DRM_YVU420, 3, 1920, 960, 1088, 544);
    CHECK (walpe_dma_planes (&d, &i420, pl, &why) && pl[2].offset == 1920 * 1088 &&
           pl[1].offset == 1920 * 1088 + 960 * 544, "YV12: U and V change places");
    /* a tiled buffer: the modifier is handed on, the size check is the driver's */
    d = desc_layers (WALPE_DRM_R8, WALPE_DRM_GR88, 1920, 0, 100, G_GUINT64_CONSTANT (0x0100000000000002));
    d.nb_objects = 2;
    d.modifier[1] = d.modifier[0];
    d.layers[1].planes[0].object = 1;
    CHECK (walpe_dma_planes (&d, &nv12, pl, &why) && pl[1].object == 1 &&
           pl[0].modifier == G_GUINT64_CONSTANT (0x0100000000000002), "two buffers with a modifier are taken");

    /* refused: every one with a sentence */
    static const guint32 bad_formats[] = { WALPE_FOURCC ('Y', 'U', 'Y', 'V'), WALPE_FOURCC ('X', 'R', '2', '4'),
                                           WALPE_FOURCC ('N', 'V', '2', '1'), WALPE_FOURCC ('N', 'V', '1', '6'),
                                           WALPE_FOURCC ('Y', 'U', '2', '4'), 0 };
    for (guint i = 0; i < G_N_ELEMENTS (bad_formats); i++) {
        d = desc_one (bad_formats[i], 1, 4 * 1920, 0, 1088, 0);
        why = NULL;
        CHECK (!walpe_dma_planes (&d, &nv12, pl, &why) && why && *why, "format %08x is refused", bad_formats[i]);
    }
    d = desc_one (WALPE_DRM_NV12, 2, 1920, 1920, 1088, 544);
    CHECK (!walpe_dma_planes (&d, &p010, pl, &why) && why, "8-bit export of a 10-bit stream is refused");
    CHECK (!walpe_dma_planes (&d, &i420, pl, &why) && why, "two planes for a three-plane picture are refused");
    WalpeDmaWant nv16 = { 2, 1, 1920, 1088, 1, 0 };
    CHECK (!walpe_dma_planes (&d, &nv16, pl, &why) && why, "a 4:2:0 export of a 4:2:2 stream is refused");
    d = desc_layers (WALPE_DRM_R8, WALPE_DRM_GR88, 1900, 1900 * 1088, 1900 * 1088 * 3 / 2, WALPE_DRM_MOD_LINEAR);
    CHECK (!walpe_dma_planes (&d, &nv12, pl, &why) && why, "a line size smaller than a line is refused");
    d = desc_layers (WALPE_DRM_R8, WALPE_DRM_GR88, 1920, 1920 * 1088, 1920 * 1088 + 100, WALPE_DRM_MOD_LINEAR);
    CHECK (!walpe_dma_planes (&d, &nv12, pl, &why) && why, "planes beyond the end of a linear buffer are refused");
    d = desc_layers (WALPE_DRM_R8, WALPE_DRM_GR88, 1920, 1920 * 1088, 1920 * 1088 * 3 / 2, WALPE_DRM_MOD_LINEAR);
    d.layers[1].planes[0].object = 3;
    CHECK (!walpe_dma_planes (&d, &nv12, pl, &why) && why, "a buffer index that does not exist is refused");
    d = desc_layers (WALPE_DRM_R8, WALPE_DRM_R8, 1920, 1920 * 1088, 1920 * 1088 * 3 / 2, WALPE_DRM_MOD_LINEAR);
    CHECK (!walpe_dma_planes (&d, &nv12, pl, &why) && why, "R8 where the UV plane belongs is refused");
    d = desc_layers (WALPE_DRM_R8, WALPE_DRM_GR88, 1920, 1920 * 1088, 1920 * 1088 * 3 / 2, WALPE_DRM_MOD_LINEAR);
    d.nb_layers = 1;
    CHECK (!walpe_dma_planes (&d, &nv12, pl, &why) && why, "a picture with its UV layer missing is refused");
    d.nb_layers = 9;
    CHECK (!walpe_dma_planes (&d, &nv12, pl, &why) && why, "a layer count out of range is refused");
    d.nb_layers = 2;
    d.nb_objects = 0;
    CHECK (!walpe_dma_planes (&d, &nv12, pl, &why) && why, "no buffer at all is refused");
    WalpeDmaWant huge = { 2, 1, 70000, 1088, 1, 1 };
    d = desc_layers (WALPE_DRM_R8, WALPE_DRM_GR88, 70000, 0, 0, WALPE_DRM_MOD_INVALID);
    CHECK (!walpe_dma_planes (&d, &huge, pl, &why) && why, "a surface beyond 16384 is refused");
    CHECK (!walpe_dma_planes (NULL, &nv12, pl, &why) && !walpe_dma_planes (&d, NULL, pl, NULL), "nothing to describe");

    /* the comparison of imported planes (read back as RGBA) with the download */
    guint8 rgba[2 * 4 * 4], plane[2 * 6];
    for (int i = 0; i < 2 * 4; i++) {
        rgba[i * 4] = (guint8) (10 + i);
        rgba[i * 4 + 1] = (guint8) (100 + i);
        rgba[i * 4 + 2] = 0;
        rgba[i * 4 + 3] = 255;
    }
    memset (plane, 0xee, sizeof plane);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 4; x++)
            plane[y * 6 + x] = (guint8) (10 + y * 4 + x);
    CHECK (walpe_zc_rows_equal (rgba, 16, 4, plane, 6, 1, 1, 4, 2), "equal samples (one component, padded rows)");
    plane[6 + 3] ^= 1;
    CHECK (!walpe_zc_rows_equal (rgba, 16, 4, plane, 6, 1, 1, 4, 2), "one sample off by one is a difference");
    guint8 uv[2 * 8];
    for (int i = 0; i < 2 * 4; i++) {
        uv[i * 2] = (guint8) (10 + i);
        uv[i * 2 + 1] = (guint8) (100 + i);
    }
    CHECK (walpe_zc_rows_equal (rgba, 16, 4, uv, 8, 2, 1, 4, 2), "equal samples (two components)");
    uv[15] ^= 0x80;
    CHECK (!walpe_zc_rows_equal (rgba, 16, 4, uv, 8, 2, 1, 4, 2), "a difference in the second component is seen");
    CHECK (WALPE_ZC_TOLERANCE == 0 && WALPE_ZC_CHECK_FRAMES >= 1, "the tolerance is none");
    printf ("DMA-BUF layouts and the sample comparison: done\n");
}

/* ------------------------------------------------- 7. the codec profile */

static void test_profile (void)
{
    /* NAL header, profile_idc, constraint flags, level_idc, then Exp-Golomb codes:
     * sps id 0 ("1"), chroma_format_idc 1 ("010"), bit depths 0 ("1", "1") */
    static const guint8 high42[] = { 0x67, 100, 0x00, 42, 0xac };          /* 1 010 1 1 00 */
    static const guint8 main31[] = { 0x67, 77, 0x40, 31, 0xe0 };
    static const guint8 cbase30[] = { 0x67, 66, 0xc0, 30, 0xe0 };
    static const guint8 base1b[] = { 0x67, 66, 0x10, 11, 0xe0 };
    /* High 10: sps id 0, chroma 1, luma depth 2 ("011"), chroma depth 2: 1 010 011 0 | 11 ... */
    static const guint8 high10[] = { 0x67, 110, 0x00, 51, 0xa6, 0xc0 };
    /* High 4:4:4 Predictive, chroma 3 ("00100"), separate planes 0, depth 0: 1 00100 0 1 | 1 ... */
    static const guint8 high444[] = { 0x67, 244, 0x00, 52, 0x91, 0x80 };
    /* High 4:2:2 ("011"), depth 2: 1 011 011 0 | 11 */
    static const guint8 high422[] = { 0x67, 122, 0x00, 41, 0xb6, 0xc0 };
    static const struct { const guint8 *sps; gsize len; const char *want; } c[] = {
        { high42, sizeof high42, "H.264 High@L4.2, 8-bit 4:2:0" },
        { main31, sizeof main31, "H.264 Main@L3.1, 8-bit 4:2:0" },
        { cbase30, sizeof cbase30, "H.264 Constrained Baseline@L3.0, 8-bit 4:2:0" },
        { base1b, sizeof base1b, "H.264 Baseline@L1b, 8-bit 4:2:0" },
        { high10, sizeof high10, "H.264 High 10@L5.1, 10-bit 4:2:0" },
        { high444, sizeof high444, "H.264 High 4:4:4 Predictive@L5.2, 8-bit 4:4:4" },
        { high422, sizeof high422, "H.264 High 4:2:2@L4.1, 10-bit 4:2:2" },
        /* a high profile cut off after the level: the defaults, nothing read past the end */
        { high42, 4, "H.264 High@L4.2, 8-bit 4:2:0" },
    };
    for (guint i = 0; i < G_N_ELEMENTS (c); i++) {
        char *got = walpe_h264_profile_text (c[i].sps, c[i].len);
        CHECK (got && !strcmp (got, c[i].want), "profile %u: got \"%s\", want \"%s\"", i, got ? got : "(null)",
               c[i].want);
        g_free (got);
    }
    CHECK (walpe_h264_profile_text (high42, 3) == NULL && walpe_h264_profile_text (NULL, 10) == NULL,
           "a parameter set of three bytes says nothing");
    static const guint8 odd[] = { 0x67, 200, 0x00, 60 };
    char *got = walpe_h264_profile_text (odd, sizeof odd);
    CHECK (got && !strcmp (got, "H.264 profile 200@L6.0, 8-bit 4:2:0"), "an unknown profile is named by number: %s",
           got ? got : "(null)");
    g_free (got);
    /* only zero bits after the level: the Exp-Golomb reader gives up instead of running on */
    static const guint8 zeros[24] = { 0x67, 100, 0x00, 40 };
    got = walpe_h264_profile_text (zeros, sizeof zeros);
    CHECK (got && !strcmp (got, "H.264 High@L4.0, 8-bit 4:2:0"), "zero bits only: the defaults: %s", got ? got : "(null)");
    g_free (got);
    printf ("codec profile text: done\n");
}

int main (void)
{
    test_setting ();
    test_plan ();
    test_attempts ();
    test_classify ();
    test_status ();
    test_dma ();
    test_profile ();
    printf (fails ? "video-hwsel-test: %d FAILED\n" : "video-hwsel-test: all passed\n", fails);
    return fails ? 1 : 0;
}
