/* video_hwsel.h - which hardware video decoder is tried, on which device, in
 * which order; what the status line says about it; which DMA-BUF layouts
 * the zero-copy path takes.
 *
 * Pure bookkeeping: glib only, no FFmpeg, no GL, no file access. The video
 * module (video_custom.c) asks FFmpeg what this build offers and the EGL
 * display which GPU renders, hands both in here, and does what the plan
 * says; tests/video_hwsel_test.c hands in made-up offers, made-up machines
 * and made-up failures.
 *
 * The rules. One of them, and only that one, looks at who made a GPU (the
 * project's rule: the support for NVIDIA's driver stays as it was):
 *  - Where the GPU that renders is driven by NVIDIA's own kernel driver
 *    ("nvidia"; WalpeHwGpu.nvidia_driver), "cuda" is the first hardware
 *    decoder, whatever the EGL device says or does not say about a compute
 *    device. Nothing else is decided by a name, and without that flag every
 *    plan is what the rules below give.
 * The others:
 *  - An API is tried on the GPU that renders the wallpaper first: a decoder
 *    on another GPU works, but every picture then crosses main memory and
 *    that GPU is kept awake for it.
 *  - Each API finds its device in its own way (walpe_hw_apis: a DRM node,
 *    the compute device number the EGL device reports, the X display, a
 *    device derived from a DRM node). Where the render GPU cannot be named
 *    in that way, the API's own default device is used and the status line
 *    says that the API chose.
 *  - Among attempts of the same standing the order is the table's rank.
 *  - An API marked "late" in the table (decoding through it is young in
 *    drivers and in FFmpeg) is tried after every other GPU API, whatever
 *    its standing; decoder blocks come last.
 *  - A machine with one GPU has no other GPU: an API whose device cannot
 *    be named there is not suspected of decoding elsewhere.
 *  - An API that is opened by a compute device number stands behind the
 *    render GPU's own APIs only when the render GPU ANSWERED that it is no
 *    such device. Where it could not be asked, the API keeps its rank among
 *    the first attempts (the order the module always had).
 *  - On x86-64 FFmpeg's V4L2 stateful decoder ("v4l2m2m") is not part of
 *    auto: to open it FFmpeg opens every /dev/video* node - webcams - to
 *    ask what it is, and no x86-64 machine decodes through it. By name
 *    (hwdec=v4l2m2m) it is taken there too; on every other architecture it
 *    is the last attempt of auto (ARM boards decode through it).
 *  - A failure before the first picture moves on to the next attempt; a
 *    failure after pictures were shown goes to software decoding at once
 *    and stays there (the wallpaper keeps playing; no second experiment in
 *    the middle of a stream).
 *  - Every failed attempt keeps its reason; the status line lists them. */
#pragma once
#include <glib.h>
#include <string.h>

/* ------------------------------------------------------------ the setting
 * "video_hwdec": off | auto | <api>. The older switch values keep their
 * meaning: on / true / 1 / yes = auto, false / 0 / no = off. An API name is
 * what FFmpeg calls the device type ("vaapi", "cuda", "vdpau", "vulkan",
 * "drm") or a decoder of its own ("v4l2m2m", "rkmpp", "cuvid", ...). */
typedef struct {
    gboolean on;        /* hardware decoding is asked for */
    char api[24];       /* "" = auto: whatever works, in the plan's order */
} WalpeHwWant;

static inline gboolean walpe_hwsel_parse (const char *s, WalpeHwWant *w)
{
    WalpeHwWant none = { FALSE, "" };
    if (w)
        *w = none;
    if (!s)
        return FALSE;
    while (*s == ' ' || *s == '\t')
        s++;
    gsize len = strlen (s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n'))
        len--;
    if (len == 0 || len >= sizeof (none.api))
        return FALSE;
    char low[sizeof (none.api)];
    for (gsize i = 0; i < len; i++) {
        char c = g_ascii_tolower (s[i]);
        if (!g_ascii_isalnum (c) && c != '_')
            return FALSE;
        low[i] = c;
    }
    low[len] = 0;
    if (!strcmp (low, "off") || !strcmp (low, "0") || !strcmp (low, "no") || !strcmp (low, "false") ||
        !strcmp (low, "none"))
        return TRUE;
    if (w)
        w->on = TRUE;
    if (!strcmp (low, "auto") || !strcmp (low, "on") || !strcmp (low, "1") || !strcmp (low, "yes") ||
        !strcmp (low, "true"))
        return TRUE;
    if (w)
        g_strlcpy (w->api, low, sizeof (w->api));
    return TRUE;
}

/* the setting as one word, for the configuration file and the status */
static inline const char *walpe_hwsel_want_name (const WalpeHwWant *w)
{
    return !w || !w->on ? "off" : w->api[0] ? w->api : "auto";
}

/* --------------------------------------------------- what FFmpeg can do */
typedef enum {
    WALPE_HW_DEVICE = 0,        /* FFmpeg's own H.264 decoder with a device of the API's type */
    WALPE_HW_WRAPPER_DEVICE,    /* a decoder of its own that is given such a device */
    WALPE_HW_WRAPPER,           /* a decoder of its own that finds its hardware itself */
} WalpeHwKind;

typedef struct {
    char api[24];       /* the name the setting takes */
    char codec[32];     /* the decoder's name, for the log */
    WalpeHwKind kind;
    char dev_type[24];  /* the device type it is given (kinds with a device) */
    int cookie;         /* the caller's own index */
} WalpeHwOffer;

/* ------------------------------------------------- the machine, as known */
#define WALPE_HW_MAX_NODES 8
typedef struct {
    gboolean known;         /* the GPU that renders was identified */
    gboolean software;      /* a CPU rasteriser renders: nothing to gain */
    char render_node[64];   /* its DRM render node, "" = it has none or is not known */
    char driver[32];        /* its kernel driver; for messages only */
    /* the compute device number the EGL device reports for itself: >= 0, or
     * -1 with ordinal_known = it reports that it has none */
    gboolean ordinal_known;
    int ordinal;
    /* every DRM render node of the machine, with its kernel driver */
    int n_nodes;
    char nodes[WALPE_HW_MAX_NODES][64];
    char node_driver[WALPE_HW_MAX_NODES][32];
    /* The GPU that renders is driven by NVIDIA's own kernel driver: "cuda"
     * is then the first attempt (the one rule that goes by a name, see the
     * top of this file). FALSE = it is not, or nobody could tell: the plan
     * is made by the other rules alone. */
    gboolean nvidia_driver;
    /* the program runs on x86-64: FFmpeg's V4L2 stateful decoder is tried
     * by name only there (walpe_hwsel_auto_skips) */
    gboolean x86_64;
} WalpeHwGpu;

/* ------------------------------------------------ how an API is opened */
typedef enum {
    WALPE_HW_BY_NODE = 0,   /* on a DRM node, and decodes on that GPU */
    WALPE_HW_BY_ORDINAL,    /* on a compute device number */
    WALPE_HW_BY_DISPLAY,    /* follows the X display; the device cannot be named */
    WALPE_HW_BY_DERIVE,     /* derived from a DRM device on a node */
    WALPE_HW_BY_FORMAL,     /* is handed a DRM node, decodes on hardware of its own */
    WALPE_HW_BY_ITSELF,     /* finds its hardware itself */
} WalpeHwTie;

typedef struct {
    const char *api;
    WalpeHwTie tie;
    int rank;           /* order among attempts of the same standing, lowest first */
    gboolean late;      /* tried after every other GPU API (see the rules above) */
    gboolean dmabuf;    /* its pictures can be handed to EGL as DMA-BUFs (zero-copy) */
    /* libraries FFmpeg loads by name at run time for it (the others are
     * linked): named in the message when no device can be made and one of
     * them is missing - the error code alone says "Input/output error" */
    const char *libs;
    const char *label;  /* for people */
} WalpeHwApi;

/* The order is the one the module had (cuda, vaapi, vdpau), continued. It
 * decides only between attempts of the same standing: on a machine whose
 * render GPU reports that it is no compute device, "cuda" stands behind
 * every API that can be opened on the render GPU (where the render GPU
 * could not be asked, or is driven by NVIDIA's own driver, it does not:
 * walpe_hwsel_plan). "vulkan" is the late one. */
static const WalpeHwApi walpe_hw_apis[] = {
    { "cuda",    WALPE_HW_BY_ORDINAL, 0,  FALSE, FALSE, "libcuda.so.1 libnvcuvid.so.1", "NVDEC" },
    { "vaapi",   WALPE_HW_BY_NODE,    1,  FALSE, TRUE,  "",                             "VA-API" },
    { "vdpau",   WALPE_HW_BY_DISPLAY, 2,  FALSE, FALSE, "",                             "VDPAU" },
    { "vulkan",  WALPE_HW_BY_DERIVE,  3,  TRUE,  FALSE, "",                             "Vulkan Video" },
    { "drm",     WALPE_HW_BY_FORMAL,  4,  FALSE, TRUE,  "",                             "V4L2 request API" },
    { "v4l2m2m", WALPE_HW_BY_ITSELF,  10, FALSE, TRUE,  "",                             "V4L2 stateful decoder" },
    { "rkmpp",   WALPE_HW_BY_ITSELF,  11, FALSE, TRUE,  "",                             "Rockchip MPP" },
};

static inline const WalpeHwApi *walpe_hwsel_api (const char *name)
{
    for (guint i = 0; name && i < G_N_ELEMENTS (walpe_hw_apis); i++)
        if (!strcmp (walpe_hw_apis[i].api, name))
            return &walpe_hw_apis[i];
    return NULL;
}

/* ------------------------------------------------------------ the plan */
typedef enum {
    WALPE_HW_SAME = 0,  /* the GPU that renders */
    WALPE_HW_UNKNOWN,   /* the API chose the device, or the render GPU is not known */
    WALPE_HW_OTHER,     /* another GPU than the one that renders */
    WALPE_HW_OWN,       /* decoding hardware of its own (a video codec block, not a GPU) */
} WalpeHwWhere;

typedef struct {
    int offer;          /* index into the offers */
    char api[24];
    char device[64];    /* the device string FFmpeg is given, "" = none (the API's default) */
    char node[64];      /* the DRM node behind it, "" = none: checked for access, named in the status */
    char driver[32];    /* that node's kernel driver; for messages only */
    WalpeHwWhere where;
    gboolean derive;    /* the device is derived from a DRM device on `node` */
    gboolean dmabuf;    /* the zero-copy path may be used with it */
    int rank;
    int order;          /* its place by standing: see walpe_hwsel_try_add */
} WalpeHwTry;

#define WALPE_HW_MAX_TRIES 24

static inline const char *walpe_hwsel_node_driver (const WalpeHwGpu *g, const char *node)
{
    if (!g || !node || !*node)
        return "";
    if (!strcmp (g->render_node, node) && g->driver[0])
        return g->driver;
    for (int i = 0; i < g->n_nodes && i < WALPE_HW_MAX_NODES; i++)
        if (!strcmp (g->nodes[i], node))
            return g->node_driver[i];
    return "";
}

static inline void walpe_hwsel_try_add (WalpeHwTry *out, int *n, int offer, const WalpeHwOffer *o,
                                        const WalpeHwGpu *g, const char *device, const char *node,
                                        WalpeHwWhere where, gboolean derive)
{
    if (*n >= WALPE_HW_MAX_TRIES)
        return;
    const WalpeHwApi *a = walpe_hwsel_api (o->api);
    WalpeHwTry *t = &out[*n];
    memset (t, 0, sizeof (*t));
    t->offer = offer;
    g_strlcpy (t->api, o->api, sizeof (t->api));
    g_strlcpy (t->device, device ? device : "", sizeof (t->device));
    g_strlcpy (t->node, node ? node : "", sizeof (t->node));
    g_strlcpy (t->driver, walpe_hwsel_node_driver (g, node), sizeof (t->driver));
    t->where = where;
    t->derive = derive;
    /* pictures as DMA-BUFs: from the render GPU's own decoder, or from a
     * decoder block that shares memory with it. A decoder of its own that
     * the table does not know may still deliver DMA-BUFs: the module looks
     * at the frames it gets. */
    t->dmabuf = a ? a->dmabuf && (where == WALPE_HW_SAME || where == WALPE_HW_OWN)
                  : where == WALPE_HW_OWN;
    t->rank = a ? a->rank : o->kind == WALPE_HW_DEVICE ? 50 : 60;
    /* the render GPU, devices the API chose, other GPUs; the same three
     * again for the late APIs; decoder blocks */
    t->order = where == WALPE_HW_OWN ? 6
             : (where == WALPE_HW_SAME ? 0 : where == WALPE_HW_UNKNOWN ? 1 : 2) + (a && a->late ? 3 : 0);
    (*n)++;
}

/* An offer that auto leaves out on this machine although the build has it:
 * FFmpeg's V4L2 stateful decoder on x86-64 (the rules at the top say why).
 * Asked for by name it is planned like any other. */
static inline gboolean walpe_hwsel_auto_skips (const WalpeHwOffer *o, const WalpeHwGpu *g)
{
    return o && g && g->x86_64 && o->kind == WALPE_HW_WRAPPER && !strcmp (o->api, "v4l2m2m");
}

/* The attempts for this machine and this setting, in order; returns how
 * many (0 = nothing to try: software). offers: what this FFmpeg build has
 * for H.264. In auto a decoder of its own that needs a device is left out
 * (FFmpeg's own decoder with the same device is the same hardware, read by
 * the parser every other path uses); by name it is taken. */
static inline int walpe_hwsel_plan (const WalpeHwOffer *offers, int n_offers, const WalpeHwGpu *g,
                                    const WalpeHwWant *want, WalpeHwTry out[WALPE_HW_MAX_TRIES])
{
    int n = 0;
    if (!want || !want->on || !g || g->software)
        return 0;
    for (int i = 0; i < n_offers; i++) {
        const WalpeHwOffer *o = &offers[i];
        if (want->api[0] ? strcmp (want->api, o->api) != 0 : o->kind == WALPE_HW_WRAPPER_DEVICE)
            continue;
        if (!want->api[0] && walpe_hwsel_auto_skips (o, g))
            continue;
        /* how the device is found is a property of the device type */
        const WalpeHwApi *a = walpe_hwsel_api (o->kind == WALPE_HW_WRAPPER_DEVICE ? o->dev_type : o->api);
        WalpeHwTie tie = o->kind == WALPE_HW_WRAPPER ? WALPE_HW_BY_ITSELF
                       : a ? a->tie : WALPE_HW_BY_DISPLAY;
        gboolean have_node = g->known && g->render_node[0];
        switch (tie) {
        case WALPE_HW_BY_NODE:
        case WALPE_HW_BY_DERIVE:
            if (have_node) {
                walpe_hwsel_try_add (out, &n, i, o, g, tie == WALPE_HW_BY_NODE ? g->render_node : "",
                                     g->render_node, WALPE_HW_SAME, tie == WALPE_HW_BY_DERIVE);
                for (int k = 0; k < g->n_nodes && k < WALPE_HW_MAX_NODES; k++)
                    if (g->nodes[k][0] && strcmp (g->nodes[k], g->render_node))
                        walpe_hwsel_try_add (out, &n, i, o, g,
                                             tie == WALPE_HW_BY_NODE ? g->nodes[k] : "", g->nodes[k],
                                             WALPE_HW_OTHER, tie == WALPE_HW_BY_DERIVE);
            } else {
                /* the render GPU has no node that is known: FFmpeg's own search */
                walpe_hwsel_try_add (out, &n, i, o, g, "", "", WALPE_HW_UNKNOWN, FALSE);
            }
            break;
        case WALPE_HW_BY_ORDINAL:
            if (g->known && g->ordinal_known && g->ordinal >= 0) {
                char num[16];
                g_snprintf (num, sizeof num, "%d", g->ordinal);
                walpe_hwsel_try_add (out, &n, i, o, g, num, g->render_node, WALPE_HW_SAME, FALSE);
            } else {
                /* the render GPU says it is no such device: the API's first
                 * device is another GPU - when the machine has another one.
                 * Not asked: nobody knows. */
                gboolean asked = g->known && g->ordinal_known;
                /* NVIDIA's own driver renders: its compute API decodes on
                 * that driver's GPU whatever the EGL device reported, so it
                 * is not "another GPU" and it is the first attempt (the
                 * project's rule; the one rule that goes by a name). Which
                 * of that driver's GPUs the API picks is still its own
                 * choice, and the status says so. */
                gboolean nv = g->nvidia_driver && a && !strcmp (a->api, "cuda");
                int before = n;
                walpe_hwsel_try_add (out, &n, i, o, g, "", "",
                                     asked && !nv && g->n_nodes > 1 ? WALPE_HW_OTHER : WALPE_HW_UNKNOWN,
                                     FALSE);
                /* Not asked is not an answer: the API is then not put behind
                 * the ones whose device could be named (the render node may
                 * be known from elsewhere while the EGL device could not be
                 * asked for its compute device). It keeps the place the
                 * table gives it, which is the order the module always had:
                 * on a driver whose compute API is the first row, that API
                 * stays the first attempt. The status still says that the
                 * API chose the device. */
                if ((!asked || nv) && n > before)
                    out[n - 1].order--;
            }
            break;
        case WALPE_HW_BY_FORMAL: {
            const char *node = have_node ? g->render_node : g->n_nodes > 0 ? g->nodes[0] : "";
            if (*node)
                walpe_hwsel_try_add (out, &n, i, o, g, node, node, WALPE_HW_OWN, FALSE);
            break;
        }
        case WALPE_HW_BY_ITSELF:
            walpe_hwsel_try_add (out, &n, i, o, g, "", "", WALPE_HW_OWN, FALSE);
            break;
        case WALPE_HW_BY_DISPLAY:
        default:
            walpe_hwsel_try_add (out, &n, i, o, g, "", "", WALPE_HW_UNKNOWN, FALSE);
            break;
        }
    }
    /* order: the render GPU, then devices the API chose, then other GPUs,
     * the late APIs in the same three steps, then decoder blocks; inside
     * each the table's rank; else as offered (insertion sort: stable, at
     * most WALPE_HW_MAX_TRIES entries) */
    for (int i = 1; i < n; i++) {
        WalpeHwTry t = out[i];
        int j = i;
        while (j > 0 && (out[j - 1].order > t.order ||
                         (out[j - 1].order == t.order && out[j - 1].rank > t.rank))) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = t;
    }
    return n;
}

/* ------------------------------------------------------------- reasons */
typedef enum {
    WALPE_HW_WHY_NONE = 0,
    WALPE_HW_WHY_NOT_BUILT,     /* this FFmpeg build has no such API (or no H.264 decoding for it) */
    WALPE_HW_WHY_NO_DEVICE,     /* no device of the API could be made */
    WALPE_HW_WHY_PERMISSION,    /* the device node may not be opened by this user */
    WALPE_HW_WHY_PROFILE,       /* profile, bit depth or chroma format the device does not decode */
    WALPE_HW_WHY_SIZE,          /* picture larger (or smaller) than the decoder's limits */
    WALPE_HW_WHY_REFUSED,       /* the device does not take the stream; it did not say which of the two */
    WALPE_HW_WHY_OPEN,          /* the decoder could not be opened with the device */
    WALPE_HW_WHY_DECODE,        /* decode calls failed */
    WALPE_HW_WHY_DOWNLOAD,      /* pictures could not be brought to main memory */
    WALPE_HW_WHY_SW_RENDERER,   /* a CPU rasteriser renders */
    WALPE_HW_WHY_N
} WalpeHwWhy;

static inline const char *walpe_hwsel_why_name (WalpeHwWhy w)
{
    switch (w) {
    case WALPE_HW_WHY_NOT_BUILT:   return "codec unsupported";
    case WALPE_HW_WHY_NO_DEVICE:   return "no device";
    case WALPE_HW_WHY_PERMISSION:  return "permission";
    case WALPE_HW_WHY_PROFILE:     return "profile";
    case WALPE_HW_WHY_SIZE:        return "resolution outside the decoder's limits";
    case WALPE_HW_WHY_REFUSED:     return "stream refused (profile, resolution or bit depth)";
    case WALPE_HW_WHY_OPEN:        return "decoder could not be opened";
    case WALPE_HW_WHY_DECODE:      return "decoding failed";
    case WALPE_HW_WHY_DOWNLOAD:    return "pictures could not be downloaded";
    case WALPE_HW_WHY_SW_RENDERER: return "the renderer is a software renderer";
    default:                       return "";
    }
}

/* FFmpeg says why a hardware decoder refuses a stream only in its log (the
 * call returns a software format or a general error code). The module
 * collects those lines; this reads the kind of reason out of them. Words,
 * not exact sentences: they differ between the APIs and between FFmpeg
 * versions. Nothing recognised = dflt, and the line itself is shown anyway. */
static inline WalpeHwWhy walpe_hwsel_classify (const char *text, WalpeHwWhy dflt)
{
    static const struct { const char *word; WalpeHwWhy why; } words[] = {
        { "permission denied", WALPE_HW_WHY_PERMISSION },
        { "image size", WALPE_HW_WHY_SIZE }, { "picture size", WALPE_HW_WHY_SIZE },
        { "not within range", WALPE_HW_WHY_SIZE }, { "macroblock count", WALPE_HW_WHY_SIZE },
        { "dimensions", WALPE_HW_WHY_SIZE }, { "resolution", WALPE_HW_WHY_SIZE },
        { "too large", WALPE_HW_WHY_SIZE }, { "too big", WALPE_HW_WHY_SIZE },
        { "profile", WALPE_HW_WHY_PROFILE }, { "bit depth", WALPE_HW_WHY_PROFILE },
        { "bitdepth", WALPE_HW_WHY_PROFILE }, { "chroma", WALPE_HW_WHY_PROFILE },
        { "no support for codec", WALPE_HW_WHY_PROFILE },
        { "unsupported codec", WALPE_HW_WHY_PROFILE },
        { "lacking required capabilities", WALPE_HW_WHY_PROFILE },
    };
    if (!text || !*text)
        return dflt;
    char *low = g_ascii_strdown (text, -1);
    WalpeHwWhy why = dflt;
    for (guint i = 0; i < G_N_ELEMENTS (words); i++)
        if (strstr (low, words[i].word)) {
            why = words[i].why;
            break;
        }
    g_free (low);
    return why;
}

/* ------------------------------------------------------ the attempts */
typedef struct {
    WalpeHwTry tries[WALPE_HW_MAX_TRIES];
    int n;
    int cur;                /* the attempt in use (or being opened), -1 = none */
    gboolean seen;          /* it has delivered a picture */
    gboolean gave_up;       /* software from now on (until the setting is switched) */
    WalpeHwWhy why[WALPE_HW_MAX_TRIES];     /* NONE = not failed */
    char *text[WALPE_HW_MAX_TRIES];         /* the detail of a failure */
    /* a reason that is no attempt's: a software renderer, nothing offered */
    WalpeHwWhy general;
    char *general_text;
    gboolean wanted;        /* the setting when the plan was made */
} WalpeHwSel;

static inline void walpe_hwsel_clear (WalpeHwSel *s)
{
    for (int i = 0; i < WALPE_HW_MAX_TRIES; i++)
        g_free (s->text[i]);
    g_free (s->general_text);
    memset (s, 0, sizeof (*s));
    s->cur = -1;
}

/* make the plan anew (a decoder is about to be opened for the first time,
 * or the setting changed): earlier failures are forgotten */
static inline void walpe_hwsel_start (WalpeHwSel *s, const WalpeHwOffer *offers, int n_offers,
                                      const WalpeHwGpu *g, const WalpeHwWant *want)
{
    walpe_hwsel_clear (s);
    s->wanted = want && want->on;
    if (!s->wanted)
        return;
    if (g && g->software) {
        s->general = WALPE_HW_WHY_SW_RENDERER;
        return;
    }
    s->n = walpe_hwsel_plan (offers, n_offers, g, want, s->tries);
    if (s->n == 0) {
        s->general = WALPE_HW_WHY_NOT_BUILT;
        if (want->api[0]) {
            /* offered and still nothing to try: an API that is handed a
             * DRM node on a machine without one */
            gboolean offered = FALSE;
            for (int i = 0; i < n_offers; i++)
                offered |= !strcmp (offers[i].api, want->api);
            s->general_text = offered
                ? g_strdup_printf ("%s: no DRM device node on this machine", want->api)
                : g_strdup_printf ("%s: this FFmpeg build has no H.264 decoding with it", want->api);
            if (offered)
                s->general = WALPE_HW_WHY_NO_DEVICE;
        } else {
            gboolean skipped = FALSE;
            for (int i = 0; i < n_offers; i++)
                skipped |= walpe_hwsel_auto_skips (&offers[i], g);
            s->general_text = g_strdup (skipped
                ? "this FFmpeg build has no hardware decoding for H.264 that auto tries here (its V4L2 "
                  "decoder is tried by name only on x86-64: hwdec=v4l2m2m)"
                : "this FFmpeg build has no hardware decoding for H.264");
        }
    }
}

/* the next attempt that has not failed, NULL = none left (software). It
 * becomes the current one. */
static inline const WalpeHwTry *walpe_hwsel_next (WalpeHwSel *s)
{
    s->seen = FALSE;
    if (s->gave_up) {
        s->cur = -1;
        return NULL;
    }
    for (int i = 0; i < s->n; i++)
        if (s->why[i] == WALPE_HW_WHY_NONE) {
            s->cur = i;
            return &s->tries[i];
        }
    s->cur = -1;
    return NULL;
}

static inline const WalpeHwTry *walpe_hwsel_current (const WalpeHwSel *s)
{
    return s->cur >= 0 && s->cur < s->n ? &s->tries[s->cur] : NULL;
}

/* the current attempt delivered a picture */
static inline void walpe_hwsel_picture (WalpeHwSel *s)
{
    if (s->cur >= 0)
        s->seen = TRUE;
}

/* The current attempt failed. TRUE = another attempt may be made
 * (walpe_hwsel_next); FALSE = software decoding: nothing is left, or
 * pictures of this decoder were already shown. */
static inline gboolean walpe_hwsel_failed (WalpeHwSel *s, WalpeHwWhy why, const char *detail)
{
    if (s->cur < 0 || s->cur >= s->n)
        return FALSE;
    int i = s->cur;
    s->why[i] = why == WALPE_HW_WHY_NONE ? WALPE_HW_WHY_REFUSED : why;
    g_free (s->text[i]);
    s->text[i] = detail && *detail ? g_strndup (detail, 240) : NULL;
    /* (it ends up in the status line and was cut at a byte: plain ASCII) */
    for (char *q = s->text[i]; q && *q; q++)
        if ((guchar) *q < 0x20 || (guchar) *q >= 0x7f)
            *q = '?';
    if (s->seen)
        s->gave_up = TRUE;
    s->cur = -1;
    s->seen = FALSE;
    if (s->gave_up)
        return FALSE;
    for (int k = 0; k < s->n; k++)
        if (s->why[k] == WALPE_HW_WHY_NONE)
            return TRUE;
    return FALSE;
}

/* the decoder is closed without a failure (parked, the thread count
 * changes): the next open starts with the first attempt that did not fail */
static inline void walpe_hwsel_closed (WalpeHwSel *s)
{
    s->cur = -1;
    s->seen = FALSE;
}

/* "vaapi on /dev/dri/renderD128 [amdgpu]", "cuda device 0 on
 * /dev/dri/renderD128 [gpudrv]", "vdpau": the API, the device it was given
 * when that is not the node itself, the DRM node behind it and that node's
 * kernel driver */
static inline void walpe_hwsel_try_name (const WalpeHwTry *t, GString *out)
{
    g_string_append (out, t->api);
    if (t->device[0] && strcmp (t->device, t->node))
        g_string_append_printf (out, " device %s", t->device);
    if (t->node[0])
        g_string_append_printf (out, " on %s", t->node);
    if (t->driver[0])
        g_string_append_printf (out, " [%s]", t->driver);
}

/* why hardware decoding is not in use: every failed attempt with its kind
 * of reason and its detail, or the general reason (g_free; NULL = none) */
static inline char *walpe_hwsel_reasons (const WalpeHwSel *s)
{
    GString *r = g_string_new (NULL);
    if (s->general != WALPE_HW_WHY_NONE) {
        g_string_append (r, walpe_hwsel_why_name (s->general));
        if (s->general_text)
            g_string_append_printf (r, ": %s", s->general_text);
    }
    for (int i = 0; i < s->n; i++) {
        if (s->why[i] == WALPE_HW_WHY_NONE)
            continue;
        if (r->len)
            g_string_append (r, "; ");
        walpe_hwsel_try_name (&s->tries[i], r);
        g_string_append_printf (r, ": %s", walpe_hwsel_why_name (s->why[i]));
        if (s->text[i])
            g_string_append_printf (r, " (%s)", s->text[i]);
    }
    if (!r->len) {
        g_string_free (r, TRUE);
        return NULL;
    }
    return g_string_free (r, FALSE);
}

/* ------------------------------------------------------ the status line
 * One line per open video (`walpe-ctl status`, "video_decoder"):
 *   <W>x<H> <fps> fps, <profile>: hardware (<api> on <device> [<driver>],
 *       <which GPU>; <how the pictures reach the renderer: downloaded to main
 *       memory | zero-copy | handed out in main memory by the decoder>)[, no
 *       picture yet]
 *   <W>x<H> <fps> fps, <profile>: software, N thread(s)[ (hardware decoding
 *       was asked for and is not in use: <api>[ on <device>]: <kind of
 *       reason>[ (<detail>)]; ...)]
 * The first words of both forms are the ones the line always had. */
typedef struct {
    int w, h;
    double fps;
    const char *profile;    /* "H.264 High@L4.2, 8-bit 4:2:0"; NULL = not known */
    int threads;            /* software decoding */
    gboolean hw;            /* a hardware decoder is open */
    gboolean in_memory;     /* it hands its pictures out in main memory itself: nothing to download */
    gboolean zero_copy;     /* its pictures stay in GPU memory */
    const char *zc_why;     /* zero-copy was asked for and is not in use: why (NULL = not asked) */
} WalpeHwNote;

static inline char *walpe_hwsel_note (const WalpeHwSel *s, const WalpeHwNote *nt)
{
    GString *o = g_string_new (NULL);
    g_string_append_printf (o, "%dx%d %.2f fps", nt->w, nt->h, nt->fps);
    if (nt->profile && *nt->profile)
        g_string_append_printf (o, ", %s", nt->profile);
    const WalpeHwTry *t = walpe_hwsel_current (s);
    if (nt->hw && t) {
        g_string_append (o, ": hardware (");
        walpe_hwsel_try_name (t, o);
        switch (t->where) {
        case WALPE_HW_SAME:
            g_string_append (o, ", the GPU that renders");
            break;
        case WALPE_HW_OTHER:
            g_string_append (o, ", NOT the GPU that renders: every picture is copied through main "
                                "memory and that GPU is kept awake");
            break;
        case WALPE_HW_OWN:
            g_string_append (o, ", decoding hardware of its own");
            break;
        default:
            g_string_append (o, ", device chosen by the API");
            break;
        }
        if (nt->in_memory)
            g_string_append (o, "; the decoder hands its pictures out in main memory");
        else if (nt->zero_copy)
            g_string_append (o, "; zero-copy: the pictures stay in GPU memory");
        else if (nt->zc_why && *nt->zc_why)
            g_string_append_printf (o, "; pictures are downloaded to main memory, zero-copy is not in "
                                       "use: %s", nt->zc_why);
        else
            g_string_append (o, "; pictures are downloaded to main memory");
        g_string_append_c (o, ')');
        if (!s->seen)
            g_string_append (o, ", no picture yet");
    } else {
        g_string_append_printf (o, ": software, %d thread%s", nt->threads, nt->threads == 1 ? "" : "s");
        if (s->wanted) {
            char *why = walpe_hwsel_reasons (s);
            g_string_append_printf (o, " (hardware decoding was asked for and is not in use: %s)",
                                    why ? why : "no reason was recorded");
            g_free (why);
        }
    }
    return g_string_free (o, FALSE);
}

/* ------------------------------------------- zero-copy: DMA-BUF layouts
 * A hardware picture exported as DMA-BUFs is described by layers of planes
 * (FFmpeg's AVDRMFrameDescriptor, which this mirrors so that the table can
 * be tested without FFmpeg). The zero-copy path imports every plane as a
 * texture of its own and draws with the shader of the upload path, so it
 * takes exactly the layouts the upload path has textures for:
 *
 *   picture (software layout)    exported as                 imported as
 *   NV12  8-bit 4:2:0            layers R8 + GR88            R8, GR88
 *                                one layer NV12 (2 planes)   R8, GR88
 *   P010 / P012 / P016           layers R16 + GR1616         R16, GR1616
 *   10..16-bit 4:2:0             one layer P010/P012/P016    R16, GR1616
 *   three planes 8-bit           one layer YU12 / YV12       R8, R8, R8
 *
 * Anything else (packed YUV, RGB, 4:4:4 in one plane, big-endian, a plane
 * count that does not fit the picture's layout) is refused: the picture is
 * downloaded instead. Range (limited / full) and matrix (BT.601 / BT.709 /
 * BT.2020 non-constant luminance) are not the exporter's business: the
 * shader applies them from the stream's tags exactly as for uploaded
 * pictures. */
#define WALPE_FOURCC(a, b, c, d) \
    ((guint32) (guint8) (a) | ((guint32) (guint8) (b) << 8) | ((guint32) (guint8) (c) << 16) | \
     ((guint32) (guint8) (d) << 24))
#define WALPE_DRM_R8      WALPE_FOURCC ('R', '8', ' ', ' ')
#define WALPE_DRM_GR88    WALPE_FOURCC ('G', 'R', '8', '8')
#define WALPE_DRM_R16     WALPE_FOURCC ('R', '1', '6', ' ')
#define WALPE_DRM_GR1616  WALPE_FOURCC ('G', 'R', '3', '2')
#define WALPE_DRM_NV12    WALPE_FOURCC ('N', 'V', '1', '2')
#define WALPE_DRM_P010    WALPE_FOURCC ('P', '0', '1', '0')
#define WALPE_DRM_P012    WALPE_FOURCC ('P', '0', '1', '2')
#define WALPE_DRM_P016    WALPE_FOURCC ('P', '0', '1', '6')
#define WALPE_DRM_YUV420  WALPE_FOURCC ('Y', 'U', '1', '2')
#define WALPE_DRM_YVU420  WALPE_FOURCC ('Y', 'V', '1', '2')
#define WALPE_DRM_MOD_INVALID G_GUINT64_CONSTANT (0x00ffffffffffffff)
#define WALPE_DRM_MOD_LINEAR  G_GUINT64_CONSTANT (0)

#define WALPE_DMA_MAX 4
typedef struct {
    int nb_objects;
    guint64 modifier[WALPE_DMA_MAX];
    gsize size[WALPE_DMA_MAX];          /* 0 = not told */
    int nb_layers;
    struct {
        guint32 format;
        int nb_planes;
        struct {
            int object;
            gsize offset, pitch;
        } planes[WALPE_DMA_MAX];
    } layers[WALPE_DMA_MAX];
} WalpeDmaDesc;

/* the picture as the upload path would get it */
typedef struct {
    int nplanes;            /* 2 = Y + interleaved UV, 3 = Y, U, V */
    int bytes;              /* per component: 1, or 2 (little-endian) */
    int surf_w, surf_h;     /* size of the decoder's surface (the picture is its top left part) */
    int log2_cw, log2_ch;   /* chroma planes are that much smaller */
} WalpeDmaWant;

typedef struct {
    guint32 fourcc;         /* R8, GR88, R16 or GR1616 */
    int object;
    guint32 offset, pitch;
    guint64 modifier;
    int w, h;               /* the plane's size on the surface, in samples */
} WalpeDmaPlane;

/* what one layer format holds: per plane the bytes per component and the
 * components per sample; uv_swapped: planes 1 and 2 are V, U */
typedef struct {
    guint32 format;
    int nplanes;
    int bytes;
    int comps[3];
    gboolean yuv420;        /* the format itself says 4:2:0 */
    gboolean uv_swapped;
} WalpeDmaFormat;

static const WalpeDmaFormat walpe_dma_formats[] = {
    { WALPE_DRM_R8,     1, 1, { 1, 0, 0 }, FALSE, FALSE },
    { WALPE_DRM_GR88,   1, 1, { 2, 0, 0 }, FALSE, FALSE },
    { WALPE_DRM_R16,    1, 2, { 1, 0, 0 }, FALSE, FALSE },
    { WALPE_DRM_GR1616, 1, 2, { 2, 0, 0 }, FALSE, FALSE },
    { WALPE_DRM_NV12,   2, 1, { 1, 2, 0 }, TRUE,  FALSE },
    { WALPE_DRM_P010,   2, 2, { 1, 2, 0 }, TRUE,  FALSE },
    { WALPE_DRM_P012,   2, 2, { 1, 2, 0 }, TRUE,  FALSE },
    { WALPE_DRM_P016,   2, 2, { 1, 2, 0 }, TRUE,  FALSE },
    { WALPE_DRM_YUV420, 3, 1, { 1, 1, 1 }, TRUE,  FALSE },
    { WALPE_DRM_YVU420, 3, 1, { 1, 1, 1 }, TRUE,  TRUE },
};

static inline const WalpeDmaFormat *walpe_dma_format (guint32 fourcc)
{
    for (guint i = 0; i < G_N_ELEMENTS (walpe_dma_formats); i++)
        if (walpe_dma_formats[i].format == fourcc)
            return &walpe_dma_formats[i];
    return NULL;
}

/* The planes to import for the exported picture d, which the upload path
 * would get as `want`: out[0] = Y, then UV or U and V. FALSE = this layout
 * is not taken (*why: a constant sentence). Every number of d is checked:
 * it comes from a driver. */
static inline gboolean walpe_dma_planes (const WalpeDmaDesc *d, const WalpeDmaWant *want,
                                         WalpeDmaPlane out[3], const char **why)
{
    const char *dummy;
    if (!why)
        why = &dummy;
    *why = NULL;
    if (!d || !want || d->nb_objects < 1 || d->nb_objects > WALPE_DMA_MAX || d->nb_layers < 1 ||
        d->nb_layers > WALPE_DMA_MAX) {
        *why = "the exported picture has no usable description";
        return FALSE;
    }
    if ((want->nplanes != 2 && want->nplanes != 3) || (want->bytes != 1 && want->bytes != 2) ||
        want->surf_w < 1 || want->surf_h < 1 || want->surf_w > 16384 || want->surf_h > 16384 ||
        want->log2_cw < 0 || want->log2_cw > 1 || want->log2_ch < 0 || want->log2_ch > 1) {
        *why = "the picture's layout is not one the plane textures can hold";
        return FALSE;
    }
    int n = 0;
    gboolean swapped = FALSE;
    for (int l = 0; l < d->nb_layers; l++) {
        const WalpeDmaFormat *f = walpe_dma_format (d->layers[l].format);
        if (!f) {
            *why = "the exported picture has a pixel format the plane textures cannot hold";
            return FALSE;
        }
        if (d->layers[l].nb_planes != f->nplanes) {
            *why = "the exported picture's plane count does not fit its format";
            return FALSE;
        }
        if (f->bytes != want->bytes) {
            *why = "the exported picture's sample size is not the stream's";
            return FALSE;
        }
        if (f->yuv420 && (want->log2_cw != 1 || want->log2_ch != 1)) {
            *why = "the exported picture is 4:2:0 and the stream is not";
            return FALSE;
        }
        swapped |= f->uv_swapped;
        for (int p = 0; p < f->nplanes; p++) {
            if (n >= want->nplanes) {
                *why = "the exported picture has more planes than the stream's layout";
                return FALSE;
            }
            /* plane n of the picture: Y alone, then UV together or U, V */
            int comps = n == 0 ? 1 : want->nplanes == 2 ? 2 : 1;
            if (f->comps[p] != comps) {
                *why = "the exported picture's planes are not laid out like the stream's";
                return FALSE;
            }
            int obj = d->layers[l].planes[p].object;
            gsize off = d->layers[l].planes[p].offset, pitch = d->layers[l].planes[p].pitch;
            if (obj < 0 || obj >= d->nb_objects) {
                *why = "the exported picture names a buffer it does not have";
                return FALSE;
            }
            int pw = n == 0 ? want->surf_w : (want->surf_w + (1 << want->log2_cw) - 1) >> want->log2_cw;
            int ph = n == 0 ? want->surf_h : (want->surf_h + (1 << want->log2_ch) - 1) >> want->log2_ch;
            gsize row = (gsize) pw * (gsize) (comps * want->bytes);
            if (pitch < row || pitch > G_MAXINT32 || off > G_MAXINT32) {
                *why = "the exported picture's line size does not hold a line of the picture";
                return FALSE;
            }
            /* a linear buffer must hold the plane; a tiled one is laid out
             * by its modifier, which only the driver can check */
            if (d->size[obj] > 0 && d->modifier[obj] == WALPE_DRM_MOD_LINEAR &&
                (off > d->size[obj] || pitch * (gsize) (ph - 1) + row > d->size[obj] - off)) {
                *why = "the exported picture's planes do not fit into its buffer";
                return FALSE;
            }
            out[n].fourcc = comps == 2 ? (want->bytes == 2 ? WALPE_DRM_GR1616 : WALPE_DRM_GR88)
                                       : (want->bytes == 2 ? WALPE_DRM_R16 : WALPE_DRM_R8);
            out[n].object = obj;
            out[n].offset = (guint32) off;
            out[n].pitch = (guint32) pitch;
            out[n].modifier = d->modifier[obj];
            out[n].w = pw;
            out[n].h = ph;
            n++;
        }
    }
    if (n != want->nplanes) {
        *why = "the exported picture has fewer planes than the stream's layout";
        return FALSE;
    }
    if (swapped && n == 3) {
        WalpeDmaPlane t = out[1];
        out[1] = out[2];
        out[2] = t;
    }
    return TRUE;
}

/* ------------------------------------------- zero-copy: the self-check
 * The zero-copy path draws with the shader of the upload path, so its
 * picture is the software path's as soon as the imported textures hold the
 * decoder's samples. That is checked, not assumed: the first pictures after
 * every decoder start are also downloaded, the imported planes are read
 * back from the GPU and compared sample by sample with the download.
 * Tolerance: WALPE_ZC_TOLERANCE code units per sample - none. One sample
 * off and the stream is refused: it is downloaded from then on. */
#define WALPE_ZC_TOLERANCE 0
#define WALPE_ZC_CHECK_FRAMES 2

/* rows a x b of n samples (bytes per sample; stride of b is bs, of a is as
 * bytes; a holds `step` components per sample of which the first `comps`
 * are compared - a read-back is RGBA) are equal */
static inline gboolean walpe_zc_rows_equal (const guint8 *a, gsize as, int step, const guint8 *b,
                                            gsize bs, int comps, int bytes, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const guint8 *ra = a + (gsize) y * as, *rb = b + (gsize) y * bs;
        for (int x = 0; x < w; x++)
            if (memcmp (ra + (gsize) x * (gsize) (step * bytes), rb + (gsize) x * (gsize) (comps * bytes),
                        (gsize) (comps * bytes)))
                return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------- the codec profile
 * "H.264 High@L4.2, 8-bit 4:2:0" from a sequence parameter set (the NAL
 * unit with its header byte, as an MP4 file stores it), for the status
 * line. NULL = too short to tell (g_free). Reads the first few bytes only
 * and never past len. */
typedef struct {
    const guint8 *p;
    gsize bits, pos;
    gboolean bad;
} WalpeBits;

static inline guint walpe_bits_u1 (WalpeBits *b)
{
    if (b->pos >= b->bits) {
        b->bad = TRUE;
        return 0;
    }
    guint v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}

/* an unsigned Exp-Golomb code */
static inline guint walpe_bits_ue (WalpeBits *b)
{
    int zeros = 0;
    while (!b->bad && walpe_bits_u1 (b) == 0)
        if (++zeros > 16) {
            b->bad = TRUE;
            break;
        }
    guint v = 0;
    for (int i = 0; i < zeros && !b->bad; i++)
        v = (v << 1) | walpe_bits_u1 (b);
    return b->bad ? 0 : (1u << zeros) - 1 + v;
}

static inline char *walpe_h264_profile_text (const guint8 *sps, gsize len)
{
    if (!sps || len < 4)
        return NULL;
    int profile = sps[1], flags = sps[2], level = sps[3];
    guint chroma = 1, depth = 8;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        /* after the level: seq_parameter_set_id, chroma_format_idc,
         * [separate_colour_plane_flag], bit_depth_luma_minus8 - read from
         * a copy without the emulation prevention bytes */
        guint8 rb[16];
        gsize n = 0;
        int zeros = 0;
        for (gsize i = 4; i < len && n < sizeof rb; i++) {
            if (zeros >= 2 && sps[i] == 3) {
                zeros = 0;
                continue;
            }
            zeros = sps[i] == 0 ? zeros + 1 : 0;
            rb[n++] = sps[i];
        }
        WalpeBits b = { rb, n * 8, 0, FALSE };
        walpe_bits_ue (&b);
        guint c = walpe_bits_ue (&b);
        if (!b.bad && c == 3)
            walpe_bits_u1 (&b);
        guint d = walpe_bits_ue (&b);
        if (!b.bad && c <= 3 && d <= 6) {
            chroma = c;
            depth = 8 + d;
        }
    }
    char pbuf[32], lbuf[16];
    const char *pn;
    switch (profile) {
    case 66:  pn = flags & 0x40 ? "Constrained Baseline" : "Baseline"; break;
    case 77:  pn = "Main"; break;
    case 88:  pn = "Extended"; break;
    case 100: pn = "High"; break;
    case 110: pn = flags & 0x10 ? "High 10 Intra" : "High 10"; break;
    case 122: pn = flags & 0x10 ? "High 4:2:2 Intra" : "High 4:2:2"; break;
    case 244: pn = flags & 0x10 ? "High 4:4:4 Intra" : "High 4:4:4 Predictive"; break;
    case 44:  pn = "CAVLC 4:4:4 Intra"; break;
    default:
        g_snprintf (pbuf, sizeof pbuf, "profile %d", profile);
        pn = pbuf;
        break;
    }
    if (level == 9 || (level == 11 && (flags & 0x10) && (profile == 66 || profile == 77 || profile == 88)))
        g_strlcpy (lbuf, "1b", sizeof lbuf);
    else
        g_snprintf (lbuf, sizeof lbuf, "%d.%d", level / 10, level % 10);
    static const char *const cn[4] = { "4:0:0", "4:2:0", "4:2:2", "4:4:4" };
    return g_strdup_printf ("H.264 %s@L%s, %u-bit %s", pn, lbuf, depth, cn[chroma]);
}
