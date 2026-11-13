/* web.c - Tier 3: WebKitGTK web wallpaper backend.
 *
 * Threading (the daemon is GTK-free on its render threads):
 *  - WebShared lives on the MAIN thread (the thread running the default
 *    GMainContext): gtk_init (once), a 1 x 1 GtkWindow that nobody sees
 *    and whose child (WalpeWebHost) gives the WebKitWebView the
 *    page's size, the capture, JS pushes. One per wallpaper path, shared
 *    by every monitor showing it (one web process, one capture stream).
 *    Where that window lives is the "page host" (webhostsel.h): an
 *    override-redirect X window off screen (X11, and Wayland through
 *    Xwayland), or a Wayland display of the module's own
 *    (webhost_wl.c; taken where there is no X display or none that
 *    works, or by name: WALPE_WEB_HOST=private). Each is in the module
 *    only when the build found what it needs (HAVE_WEBHOST_X11,
 *    HAVE_WEBHOST_WL): a module without the X11 one includes no X header
 *    and calls no X function. GTK talks to exactly
 *    one of them for the life of the process. Nothing else in this
 *    file depends on which: the capture reads GTK's render node, never
 *    the window system.
 *  - WebViewer is the WalpeContent on each monitor's render thread: it
 *    only uploads what changed of the latest frame (glTexSubImage2D) and
 *    draws. Render threads never touch GTK/WebKit objects; they tell the
 *    main thread about pauses, resumes and sizes through idles
 *    (sh_state_main).
 *  - Hand-off: a capture (the view's own render node, drawn straight into
 *    a reused B8G8R8A8_PREMULTIPLIED buffer: sh_on_paint / sh_capture_node;
 *    WALPE_WEB_SNAPSHOT=1 = WebKit snapshots) is compared with the frame
 *    before it (sh_publish), swapped in under sh->lock with its damage,
 *    seq bumped and the viewers woken (request_redraw). A repaint that
 *    changed nothing is not handed on.
 *  - Captured when the view repainted (no timer), at most at the fps
 *    setting (up to WEB_MAX_FPS), and only while some viewer is unpaused.
 *    "walpe-ctl set fps=N" applies to a running page (web_set_fps).
 *
 * Wallpaper Engine web API: on load the page gets
 * window.wallpaperPropertyListener.applyUserProperties({...}) with the
 * project.json properties merged with the user's overrides, and
 * applyGeneralProperties({fps}); setPaused(bool) on pause transitions;
 * pointer input (move/hover, left/middle/right buttons, wheel) happening
 * on the empty desktop of a monitor reaches that monitor's page as real
 * input events (webinput.c; position throttled to the daemon's 30 Hz
 * pointer poll), plus --mouse-x/--mouse-y CSS variables.
 * A document-start user script defines the wallpaperRegister*Listener
 * functions (pages calling an undefined one used to throw during init);
 * wallpaperRegisterAudioListener(cb) posts to the "walpe" message handler,
 * after which the page gets cb(128 floats: 64 left + 64 right) about 30
 * times a second from the shared audio capture (audio.h) while visible
 * (sh_audio_tick: one call at a time; in silence once a second once the
 * picture stands still).
 *
 * Files: the wallpaper dir is served through a private "walpe-web://"
 * URI scheme that only resolves paths inside that dir; file:// access is
 * not granted (a workshop page could otherwise fetch() any local file).
 * WALPE_WEB_FILE_URLS=1 falls back to file:// + same-dir file access.
 *
 * Requires webkitgtk-6.0 (HAVE_WEB). */
#include "walpe-build-config.h"
#include "content.h"
#include "web.h"
#include "webinput.h"
#include "webdamage.h"
#include "modules.h"
#include "glutil.h"
#include "common/util.h"
#include "common/lazy.h"
#include "common/config.h"
#include "common/audio.h"
#include "common/media.h"
#include "webhostsel.h"
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <libsoup/soup.h>
/* the X11 page host: GTK's X11 backend and Xlib in this build */
#if defined (GDK_WINDOWING_X11) && defined (HAVE_WEBHOST_X11)
#define WEB_HAVE_X11 1
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#endif
/* the private page host */
#if defined (GDK_WINDOWING_WAYLAND) && defined (HAVE_WEBHOST_WL)
#define WEB_HAVE_PRIVATE 1
#include "webhost_wl.h"
#endif
#include <dlfcn.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <sys/random.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <time.h>
#include <unistd.h>

/* capture rate limit: the fps setting decides below it. The daemon passes
 * min(fps setting, fastest monitor's refresh rate). (30 until version 0.11:
 * web wallpapers ran at 30 fps whatever the setting and the monitor.) */
#define WEB_MAX_FPS 144
/* audio listeners: WE calls them "roughly 30 times per second" whatever
 * the frame rate (docs: web/audio/visualizer). Pages draw their visualizer
 * inside the listener, so the call rate is a rendering load of its own. */
#define WEB_AUDIO_HZ 30
/* silence: the listener is called until the picture has stood still this
 * long (visualizers let their bars fall over several calls), then once a
 * second until there is sound or the page is told something else */
#define WEB_AUDIO_SETTLE_US (1500 * G_TIME_SPAN_MILLISECOND)
/* ... and for twice as long as its last two changes were apart, up to
 * this: a page that is slow, or moves every few seconds, is not still */
#define WEB_AUDIO_SETTLE_MAX_US (10 * G_TIME_SPAN_SECOND)
#define WEB_AUDIO_IDLE_MS 100   /* silence: look for sound this often */
/* silence and a still picture: the listener is still called this often (a
 * clock that is drawn in the listener must not stop) */
#define WEB_AUDIO_KEEPALIVE_US (1000 * G_TIME_SPAN_MILLISECOND)
#define WEB_SCHEME "walpe-web"

typedef struct {
    void (*cb) (gpointer);
    gpointer user;
    gboolean paused;
} WebViewerRef;

/* a captured frame. Render threads take a reference under sh->lock and
 * upload it AFTER unlocking (a full-frame glTexSubImage2D under the lock
 * stalled the main thread's next hand-off); the main thread
 * draws into a buffer only when no render thread still holds it. Buffers
 * are kept and reused (sh_buf_take): a capture allocates nothing. */
typedef struct WebBuf {
    gint refs;
    guint8 *data;             /* BGRA premultiplied, stride w * 4 */
    gsize cap;
    int w, h;
    cairo_surface_t *surf;    /* image surface on data (main thread only) */
} WebBuf;

static WebBuf *webbuf_new (int w, int h)
{
    WebBuf *b = g_new0 (WebBuf, 1);
    b->refs = 1;
    b->cap = (gsize) w * h * 4;
    b->data = g_malloc (b->cap);
    b->w = w;
    b->h = h;
    return b;
}

static WebBuf *webbuf_ref (WebBuf *b)
{
    g_atomic_int_inc (&b->refs);
    return b;
}

static void webbuf_unref (WebBuf *b)
{
    if (b && g_atomic_int_dec_and_test (&b->refs)) {
        /* (the surface only wraps data: destroying it on a render thread
         * touches nothing shared) */
        if (b->surf)
            cairo_surface_destroy (b->surf);
        g_free (b->data);
        g_free (b);
    }
}

/* what a frame changed against the one before it (render threads upload
 * only that): x0 >= x1 = nothing known, upload everything */
#define WEB_DMG_N 8
typedef struct {
    guint64 seq;
    int x0, y0, x1, y1;
} WebDmg;

typedef struct {
    /* --- any thread, under lock */
    GMutex lock;
    int refs;                 /* viewers attached */
    GArray *viewers;          /* WebViewerRef, keyed by user */
    struct WebBuf *px;        /* front frame: BGRA premultiplied, tight */
    int pw, ph;
    guint64 seq;
    WebDmg dmg[WEB_DMG_N];    /* [seq % WEB_DMG_N]: frame seq against seq - 1 */
    int want_w, want_h;       /* largest monitor size reported by viewers */
    int active;               /* unpaused viewers */
    /* --- immutable after creation */
    char *path;               /* wallpaper dir */
    char *origin;             /* as opened: override key (a preset's dir) */
    char *preset_dir;         /* WE preset: second file root ("files/..."), or NULL */
    JsonObject *preset;       /* WE preset values (owned), or NULL */
    char *html;               /* entry file (absolute) */
    char *host;               /* walpe-web://<host>/ id */
    int fps;                  /* capture rate: main thread after creation */
    gint fps_want;            /* atomic: render thread -> main (web_set_fps) */
    /* --- main thread only */
    GtkWidget *win;           /* 1 x 1 off-screen toplevel */
    GtkWidget *box;           /* WalpeWebHost: gives the view the page's size */
    WebKitWebView *view;
    GCancellable *cancel;
    gboolean active_main;     /* some viewer is unpaused (as of sh_state_main) */
    guint snap_id;            /* WALPE_WEB_SNAPSHOT: the snapshot timer */
    gboolean in_flight;       /* one snapshot at a time */
    /* capture from the view's own rendering (see sh_on_paint) */
    gint64 cap_due_us;        /* the fps limit: earliest next capture */
    guint cap_id;             /* a repaint came too early: capture when due */
    gint64 change_us;         /* when the picture last changed */
    gint64 change_iv_us;      /* how far its last two changes were apart (capped) */
    struct WebBuf *spare[2];  /* buffers to draw the next frame into */
    guint spare_idle;         /* captures in a row that left spare[1] unused */
    guint audio_id;           /* audio pushes (WEB_AUDIO_HZ; slow in silence) */
    guint audio_ms;           /* its interval */
    guint media_id;           /* media integration pushes */
    guint replay_id;          /* pointer replay after a resume */
    guint report_id;          /* WALPE_WEB_TIMING */
    gboolean dead;            /* released; free when the snapshot returns */
    gboolean loaded;
    int crashes;              /* page process deaths in the current run (60 s) */
    gint64 crash_us;          /* time of the last one */
    gboolean gave_up;         /* too many: not reloaded until opened again */
    gboolean webgl_off;       /* WebGL was switched off after repeated crashes */
    gboolean js_paused;
    gboolean suspended;       /* page paused + window unmapped (sh_suspend) */
    WebKitUserScript *snd_script; /* the sound script with the level it starts with */
    int snd_sw;               /* the wallpaper's own sound switch: -1 none, 0 off, 1 on */
    int snd_vol_q16;          /* ... and its own volume, 0..65536 */
    char snd_key[24];         /* what its copies in the page's frames know each other by */
    int snd_q16;              /* the level the page was last given (-1 = none yet) */
    int view_w, view_h;
    WebInput *input;          /* pointer input + navigation guard (webinput.c) */
    gpointer in_owner;        /* the viewer (monitor) the page's pointer is on */
    WalpeWebInput in_last;    /* where it is there (kind MOVE), if in_have */
    gboolean in_have;
    guint snapshots;          /* diagnostics */
    gboolean audio_wanted;    /* page registered an audio listener */
    gboolean audio_ref;       /* holds a capture reference */
    gint64 audio_due_us;      /* next spectrum push (WEB_AUDIO_HZ) */
    gint64 audio_sent_us;     /* when the last one went out */
    struct WebGate *audio_gate; /* a spectrum push the page has not run yet */
    gint64 audio_quiet_us;    /* silence since (0 = sound) */
    gint64 audio_wake_us;     /* the page was told something else: keep calling */
    gboolean audio_idle;      /* silence and a still picture: no pushes */
    gboolean media_wanted;    /* page registered a media listener */
    gboolean media_ref;       /* holds a media integration reference */
    gboolean media_first;     /* push the full state on the next tick */
    guint64 media_serial;
    guint media_seen[5];
    /* --- main thread: absolute paths of file/directory property values
     * the page may load through file:/// (see WE_API_JS) */
    GPtrArray *file_allow;
    /* --- main thread: what the page was last told (applyUserProperties
     * is called with every property on load and with the CHANGED ones
     * afterwards, like WE): key -> {"type","value"} as JSON text */
    GHashTable *pushed;
    /* directory properties: key -> the file list last handed to
     * userDirectoryFilesAddedOrChanged (JSON array text) */
    GHashTable *pushed_dirs;
    /* main thread: network switch as applied to the page's session */
    gboolean net_known, net_off;
    /* the page's first picture (sh_first_frame_wanted): first_done is
     * written by the main thread under sh->lock, first_id is its timer */
    gboolean first_done;
    guint first_id;
    guint n_load_logs, n_media_logs;  /* "loaded" / "media state pushed" lines so far */
    /* main thread: what the page posted to the message handler (on_script_message) */
    gint64 msg_win_us;        /* start of the current counting window */
    guint msg_n;              /* messages in it */
    guint64 msg_big;          /* characters of over-long messages so far */
} WebShared;

/* A page opened while every viewer is paused (walpe-ctl pause, a covered
 * monitor) was suspended before it had drawn anything: the monitor stayed
 * black until resume. So a new page counts as active for its first
 * picture, whatever its viewers say: until WEB_FIRST_SETTLE_MS after it
 * finished loading (what it draws in that time - with its option values,
 * from its first animation frames - is the picture a paused monitor
 * keeps), and for at most WEB_FIRST_FRAME_S when it never finishes. Then
 * sh_first_end brings it in line with its viewers: paused when they all
 * are. (The first frame alone was not enough of a rule: that is the one
 * taken the moment loading ended, often before the page had drawn at
 * all, and nothing looked at the viewers again after it - the page of a
 * paused monitor kept running, 30 animation frames a second, behind a
 * black or half-drawn picture.) Call with sh->lock held. */
#define WEB_FIRST_FRAME_S 30
#define WEB_FIRST_SETTLE_MS 1000
/* the size a page is made at when no viewer has drawn yet (sh_create_real);
 * it is resized to what its monitors ask for with their first frame */
#define WEB_SIZE_UNKNOWN_W 1920
#define WEB_SIZE_UNKNOWN_H 1080
static gboolean sh_first_frame_wanted (WebShared *sh)
{
    return !sh->first_done;
}

typedef struct {
    WalpeContent base;
    WebShared *sh;
    GLuint tex;
    int tex_w, tex_h;
    guint64 drawn_seq;
    gboolean paused;
    int last_w, last_h;
} WebViewer;

/* ------------------------------------------------------------ page host
 * The page's GtkWindow must be mapped (WebKit only runs a page it believes
 * is visible), but nothing in it is ever seen. Until version 0.12 the window
 * had the page's size, so for every page frame GTK painted the whole
 * window with its cairo renderer, copied it into a new SysV SHM segment
 * and sent it to the X server, which copied it into a window that is
 * entirely off screen: 12-19 ms of main-thread CPU per 1080p frame and
 * 73-77 ms at 4K, 3-5 times what the capture itself cost.
 * WalpeWebHost is the window's only child: it asks for 1 x 1 pixel, so
 * the toplevel (and what GTK paints and ships) is 1 x 1, and gives the
 * web view the page's real size. The view keeps its full allocation:
 * layout, input coordinates and the capture are unchanged. Events aimed
 * at page coordinates outside the 1 x 1 window still reach the view: GTK
 * picks through widgets whose overflow is "visible" (the window's is set
 * so in sh_create_real). WALPE_WEB_BIGWIN=1 = the old full-size window. */
#define WALPE_TYPE_WEB_HOST (walpe_web_host_get_type ())
G_DECLARE_FINAL_TYPE (WalpeWebHost, walpe_web_host, WALPE, WEB_HOST, GtkWidget)

struct _WalpeWebHost {
    GtkWidget parent_instance;
    GtkWidget *child;
    int cw, ch;               /* the child's size */
    gboolean big;             /* WALPE_WEB_BIGWIN: ask for the child's size */
    /* GTK paints the window: the child repainted, or sh_kick asked */
    void (*paint) (struct _WalpeWebHost *h, GtkSnapshot *snapshot, gpointer user);
    gpointer user;
};

G_DEFINE_FINAL_TYPE (WalpeWebHost, walpe_web_host, GTK_TYPE_WIDGET)

static void walpe_web_host_measure (GtkWidget *w, GtkOrientation o, int for_size, int *min, int *nat,
                                    int *min_base, int *nat_base)
{
    (void) for_size;
    WalpeWebHost *h = WALPE_WEB_HOST (w);
    int v = !h->big ? 1 : o == GTK_ORIENTATION_HORIZONTAL ? h->cw : h->ch;
    *min = *nat = MAX (v, 1);
    *min_base = *nat_base = -1;
}

static void walpe_web_host_size_allocate (GtkWidget *w, int width, int height, int baseline)
{
    (void) width; (void) height; (void) baseline;
    WalpeWebHost *h = WALPE_WEB_HOST (w);
    if (!h->child)
        return;
    /* (GTK wants a widget measured before it is allocated) */
    int mw = 0, mh = 0;
    gtk_widget_measure (h->child, GTK_ORIENTATION_HORIZONTAL, -1, &mw, NULL, NULL, NULL);
    gtk_widget_measure (h->child, GTK_ORIENTATION_VERTICAL, -1, &mh, NULL, NULL, NULL);
    gtk_widget_size_allocate (h->child, &(GtkAllocation) { 0, 0, MAX (h->cw, mw), MAX (h->ch, mh) }, -1);
}

/* the child's render node for this frame (NULL = it drew nothing). Only
 * from the paint callback. It is NOT put into the window's own snapshot:
 * a node of the page's size in there made GTK allocate and clear a
 * page-sized image for every frame of the 1 x 1 window (its redraw region
 * is not clipped to the surface), and nothing of it is ever seen. With an
 * empty snapshot GTK renders nothing and sends the X server nothing. */
static GskRenderNode *walpe_web_host_child_node (WalpeWebHost *h)
{
    GtkSnapshot *own = gtk_snapshot_new ();
    gtk_widget_snapshot_child (GTK_WIDGET (h), h->child, own);
    return gtk_snapshot_free_to_node (own);
}

static void walpe_web_host_snapshot (GtkWidget *w, GtkSnapshot *snapshot)
{
    WalpeWebHost *h = WALPE_WEB_HOST (w);
    if (!h->child)
        return;
    if (h->paint)
        h->paint (h, snapshot, h->user);
    else
        gtk_widget_snapshot_child (w, h->child, snapshot);
}

static void walpe_web_host_dispose (GObject *o)
{
    WalpeWebHost *h = WALPE_WEB_HOST (o);
    g_clear_pointer (&h->child, gtk_widget_unparent);
    G_OBJECT_CLASS (walpe_web_host_parent_class)->dispose (o);
}

static void walpe_web_host_class_init (WalpeWebHostClass *k)
{
    G_OBJECT_CLASS (k)->dispose = walpe_web_host_dispose;
    GTK_WIDGET_CLASS (k)->measure = walpe_web_host_measure;
    GTK_WIDGET_CLASS (k)->size_allocate = walpe_web_host_size_allocate;
    GTK_WIDGET_CLASS (k)->snapshot = walpe_web_host_snapshot;
}

static void walpe_web_host_init (WalpeWebHost *h)
{
    (void) h;
}

static void walpe_web_host_set_size (WalpeWebHost *h, int w, int hh)
{
    h->cw = w;
    h->ch = hh;
    if (h->child)
        gtk_widget_set_size_request (h->child, w, hh);
    if (h->big)
        gtk_widget_queue_resize (GTK_WIDGET (h));
    else
        gtk_widget_queue_allocate (GTK_WIDGET (h));
}

static GtkWidget *walpe_web_host_new (GtkWidget *child, int w, int hh, gboolean big)
{
    WalpeWebHost *h = g_object_new (WALPE_TYPE_WEB_HOST, NULL);
    h->big = big;
    h->child = child;
    gtk_widget_set_parent (child, GTK_WIDGET (h));
    walpe_web_host_set_size (h, w, hh);
    return GTK_WIDGET (h);
}

/* registry: path -> WebShared (any thread) */
static GMutex reg_lock;
static GHashTable *registry;

static void sh_input_replay (WebShared *sh);
static void sh_timers_update (WebShared *sh);
static void sh_kick (WebShared *sh);
static void sh_audio_wake (WebShared *sh);
static int audio_silence_mode (void);
static gboolean sh_state_main (gpointer user);
static gboolean sh_first_end (gpointer user);
static void timer_set (guint *id, gboolean want, guint ms, GSourceFunc fn, gpointer user);

/* ------------------------------------------------------------ helpers */

/* "one in flight" marker shared by the page object and the script
 * evaluation it waits for (main thread only; outlives the page) */
typedef struct WebGate { int ref; gboolean busy; } WebGate;

static void webgate_unref (WebGate *g)
{
    if (g && --g->ref == 0)
        g_free (g);
}

static void sh_free (WebShared *sh)
{
    webgate_unref (sh->audio_gate);
    g_clear_object (&sh->cancel);
    webbuf_unref (sh->px);
    webbuf_unref (sh->spare[0]);
    webbuf_unref (sh->spare[1]);
    g_array_free (sh->viewers, TRUE);
    g_free (sh->path);
    g_free (sh->origin);
    g_free (sh->preset_dir);
    if (sh->preset)
        json_object_unref (sh->preset);
    g_free (sh->html);
    g_free (sh->host);
    if (sh->file_allow)
        g_ptr_array_unref (sh->file_allow);
    g_clear_pointer (&sh->pushed, g_hash_table_unref);
    g_clear_pointer (&sh->pushed_dirs, g_hash_table_unref);
    g_mutex_clear (&sh->lock);
    g_free (sh);
}

/* 128-bit page tokens from the kernel CSPRNG (g_random_int is a Mersenne
 * Twister seeded from the clock: predictable) */
static void web_random_bytes (void *buf, gsize n)
{
    gsize got = 0;
    while (got < n) {
        ssize_t r = getrandom ((guint8 *) buf + got, n - got, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            /* no getrandom (very old kernel): fall back, still unique */
            for (; got < n; got++)
                ((guint8 *) buf)[got] = (guint8) g_random_int ();
            return;
        }
        got += (gsize) r;
    }
}

/* constant-time compare of two 32-char tokens */
static gboolean token_equal (const char *a, const char *b)
{
    if (!a || !b || strlen (a) != 32 || strlen (b) != 32)
        return FALSE;
    unsigned d = 0;
    for (int i = 0; i < 32; i++)
        d |= (unsigned) (a[i] ^ b[i]);
    return d == 0;
}

/* host -> shared lookup for the URI scheme handler (main thread) */
static WebShared *sh_by_host (const char *host)
{
    WebShared *found = NULL;
    g_mutex_lock (&reg_lock);
    if (registry) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init (&it, registry);
        while (g_hash_table_iter_next (&it, &k, &v))
            if (token_equal (((WebShared *) v)->host, host))
                found = v;
    }
    g_mutex_unlock (&reg_lock);
    return found;
}

/* escaped URI path inside the wallpaper dir -> real file path, or NULL
 * when it does not resolve to a regular file strictly inside the dir
 * (symlinks resolved, ".." cannot escape) */
static char *sh_resolve_in (const char *rootdir, const char *escaped_path);

static char *sh_resolve_file (WebShared *sh, const char *escaped_path)
{
    char *f = sh_resolve_in (sh->path, escaped_path);
    /* a preset's own files ("files/x.png" property values) */
    if (!f && sh->preset_dir)
        f = sh_resolve_in (sh->preset_dir, escaped_path);
    return f;
}

static char *sh_resolve_in (const char *rootdir, const char *escaped_path)
{
    char *file = NULL;
    {
        const char *p = escaped_path;
        char *rel = g_uri_unescape_string (p && *p ? p : "/", NULL);
        char *joined = rel ? g_build_filename (rootdir, rel, NULL) : NULL;
        char *canon = joined ? g_canonicalize_filename (joined, NULL) : NULL;
        char *root = g_canonicalize_filename (rootdir, NULL);
        char *real = canon ? realpath (canon, NULL) : NULL;     /* resolves symlinks */
        char *rroot = realpath (root, NULL);
        gsize rl = rroot ? strlen (rroot) : 0;
        if (real && rroot && !strncmp (real, rroot, rl) && (real[rl] == '/' || real[rl] == 0) &&
            g_file_test (real, G_FILE_TEST_IS_REGULAR))
            file = g_strdup (real);
        free (real);
        free (rroot);
        g_free (rel);
        g_free (joined);
        g_free (canon);
        g_free (root);
    }
    return file;
}

/* WE pages load property files as 'file:///' + value (WE runs them from
 * file://). Served over http, WebKit refuses file:// subresources, so the
 * WE_API_JS shim rewrites such URLs to /<token>/__walpe_file/<abs path>,
 * answered here ONLY for files inside the wallpaper or preset dir, or a
 * current file/directory property value (sh->file_allow). */
#define WALPE_FILE_ROUTE "__walpe_file"

static gboolean path_under (const char *real, const char *root)
{
    char *rr = root ? realpath (root, NULL) : NULL;
    gsize rl = rr ? strlen (rr) : 0;
    gboolean ok = rr && !strncmp (real, rr, rl) && (real[rl] == '/' || real[rl] == 0);
    free (rr);
    return ok;
}

/* the file kinds a "directory" option hands to a page (dir_files) */
static const char *const dir_img_exts[] = { "png", "jpg", "jpeg", "gif", "bmp", "webp", "svg", "avif", NULL };
static const char *const dir_vid_exts[] = { "mp4", "webm", "ogv", "ogg", "mkv", "mov", "m4v", NULL };

static gboolean dir_media_name (const char *name, gboolean video)
{
    const char *dot = strrchr (name, '.');
    if (!dot)
        return FALSE;
    for (int i = 0; dir_img_exts[i]; i++)
        if (!g_ascii_strcasecmp (dot + 1, dir_img_exts[i]))
            return TRUE;
    for (int i = 0; video && dir_vid_exts[i]; i++)
        if (!g_ascii_strcasecmp (dot + 1, dir_vid_exts[i]))
            return TRUE;
    return FALSE;
}

/* is `real` a file the page may read through a folder the user picked for
 * a "directory" option (rdir, resolved)? Only what dir_files() can list
 * for it: pictures and videos, at most one sub-folder down, nothing
 * hidden. Serving every file below the folder let a page with network
 * access read e.g. ~/.ssh/id_rsa once the user chose "~" (or documents
 * next to the pictures) for a slideshow. */
static gboolean dir_grant_covers (const char *real, const char *rdir)
{
    gsize rl = strlen (rdir);
    while (rl > 1 && rdir[rl - 1] == '/')
        rl--;
    if (rl == 1 && rdir[0] == '/')
        rl = 0;         /* the root itself: everything is "/name" below it */
    if (strncmp (real, rdir, rl) || real[rl] != '/')
        return FALSE;
    const char *rel = real + rl + 1, *name = rel;
    int depth = 0;
    for (const char *c = rel;;) {
        if (*c == '.' || !*c)
            return FALSE;       /* hidden file or folder */
        name = c;
        const char *sl = strchr (c, '/');
        if (!sl)
            break;
        if (++depth > 1)
            return FALSE;
        c = sl + 1;
    }
    return dir_media_name (name, TRUE);
}

static char *sh_resolve_abs (WebShared *sh, const char *escaped_abs)
{
    char *p = g_uri_unescape_string (escaped_abs, NULL);
    char *real = p && g_path_is_absolute (p) ? realpath (p, NULL) : NULL;
    g_free (p);
    if (!real || !g_file_test (real, G_FILE_TEST_IS_REGULAR)) {
        free (real);
        return NULL;
    }
    gboolean ok = path_under (real, sh->path) || (sh->preset_dir && path_under (real, sh->preset_dir));
    for (guint i = 0; !ok && sh->file_allow && i < sh->file_allow->len; i++) {
        const char *a = g_ptr_array_index (sh->file_allow, i);
        char *ra = realpath (a, NULL);
        ok = ra && (!strcmp (ra, real) ||
                    (g_file_test (ra, G_FILE_TEST_IS_DIR) && dir_grant_covers (real, ra)));
        free (ra);
    }
    char *out = ok ? g_strdup (real) : NULL;
    if (!ok && web_log_gate (WEB_LOG_FILE)) {
        char *r = web_log_text (real, 600);
        g_message ("web: refused file:// load of %s (not a wallpaper/property file)", r);
        g_free (r);
    }
    free (real);
    return out;
}

/* walpe-web://<host>/<path>: serve files strictly inside the wallpaper dir */
static void scheme_cb (WebKitURISchemeRequest *req, gpointer user)
{
    (void) user;
    GError *err = NULL;
    GUri *uri = g_uri_parse (webkit_uri_scheme_request_get_uri (req), G_URI_FLAGS_NONE, NULL);
    WebShared *sh = uri ? sh_by_host (g_uri_get_host (uri)) : NULL;
    char *file = sh ? sh_resolve_file (sh, g_uri_get_path (uri)) : NULL;
    if (uri)
        g_uri_unref (uri);
    if (!file) {
        err = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not found");
        webkit_uri_scheme_request_finish_error (req, err);
        g_error_free (err);
        return;
    }
    GFile *gf = g_file_new_for_path (file);
    GFileInputStream *in = g_file_read (gf, NULL, &err);
    GFileInfo *info = in ? g_file_query_info (gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                              G_FILE_QUERY_INFO_NONE, NULL, NULL) : NULL;
    if (!in) {
        webkit_uri_scheme_request_finish_error (req, err);
        g_error_free (err);
    } else {
        gboolean uncertain = FALSE;
        char *ct = g_content_type_guess (file, NULL, 0, &uncertain);
        char *mime = g_content_type_get_mime_type (ct);
        webkit_uri_scheme_request_finish (req, G_INPUT_STREAM (in),
                                          info ? g_file_info_get_size (info) : -1, mime);
        g_free (ct);
        g_free (mime);
        g_object_unref (in);
    }
    g_clear_object (&info);
    g_object_unref (gf);
    g_free (file);
}

/* ------------------------------------------- loopback HTTP file server
 * WebKit's media player (GStreamer webkitwebsrc) only fetches http(s)/blob
 * URLs: <video src="x.webm"> on the walpe-web:// scheme fails with
 * MEDIA_ERR_SRC_NOT_SUPPORTED, and file:// media fails too. Pages are
 * therefore served from http://127.0.0.1:<port>/<token>/ by a libsoup
 * server on the main thread: IPv4 loopback only, a random 128-bit token
 * per wallpaper as the first path segment, the same confinement as the
 * scheme (files strictly inside the wallpaper dir), GET/HEAD only,
 * memory-mapped bodies (libsoup answers Range requests from them, which
 * GStreamer uses to seek in webm). WALPE_WEB_SCHEME=1 keeps the scheme. */
static SoupServer *http_srv;
static guint http_port;

static void http_cb (SoupServer *srv, SoupServerMessage *msg, const char *path,
                     GHashTable *query, gpointer user)
{
    (void) srv; (void) query; (void) user;
    const char *method = soup_server_message_get_method (msg);
    if (strcmp (method, "GET") && strcmp (method, "HEAD")) {
        soup_server_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED, NULL);
        return;
    }
    /* DNS rebinding: a browser page on evil.example resolving to 127.0.0.1
     * would send "Host: evil.example"; only our own names are served */
    const char *hh = soup_message_headers_get_one (soup_server_message_get_request_headers (msg),
                                                   "Host");
    char ok1[48], ok2[48];
    g_snprintf (ok1, sizeof ok1, "127.0.0.1:%u", http_port);
    g_snprintf (ok2, sizeof ok2, "localhost:%u", http_port);
    if (!hh || (g_ascii_strcasecmp (hh, ok1) && g_ascii_strcasecmp (hh, ok2))) {
        soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN, NULL);
        return;
    }
    /* With network access off this server is also the page's proxy
     * (sh_apply_network): every request of the page arrives here, in
     * proxy form ("GET http://host:port/path"). Only a request whose
     * target is this server is answered; one for any other host or port -
     * another service on this machine included - ends here. (CONNECT, for
     * https and wss, is no GET: refused above.) */
    GUri *ru = soup_server_message_get_uri (msg);
    const char *rh = ru ? g_uri_get_host (ru) : NULL;
    if (!rh || (g_ascii_strcasecmp (rh, "127.0.0.1") && g_ascii_strcasecmp (rh, "localhost")) ||
        g_uri_get_port (ru) != (int) http_port) {
        soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN, NULL);
        return;
    }
    /* /<token>/<rest> */
    const char *p = path && path[0] == '/' ? path + 1 : NULL;
    const char *slash = p ? strchr (p, '/') : NULL;
    char *token = slash ? g_strndup (p, (gsize) (slash - p)) : NULL;
    WebShared *sh = token && strlen (token) == 32 ? sh_by_host (token) : NULL;
    char *file = NULL;
    if (sh && slash && g_str_has_prefix (slash, "/" WALPE_FILE_ROUTE "/"))
        file = sh_resolve_abs (sh, slash + strlen ("/" WALPE_FILE_ROUTE));
    else if (sh)
        file = sh_resolve_file (sh, slash);
    g_free (token);
    GMappedFile *mf = file ? g_mapped_file_new (file, FALSE, NULL) : NULL;
    if (!mf) {
        soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
        g_free (file);
        return;
    }
    gboolean uncertain = FALSE;
    char *ct = g_content_type_guess (file, NULL, 0, &uncertain);
    char *mime = g_content_type_get_mime_type (ct);
    SoupMessageHeaders *h = soup_server_message_get_response_headers (msg);
    soup_message_headers_set_content_type (h, mime ? mime : "application/octet-stream", NULL);
    soup_message_headers_replace (h, "Cache-Control", "no-store");
    GBytes *b = g_mapped_file_get_bytes (mf);
    guint status = SOUP_STATUS_OK;
    {
        /* A request for a part of the file (WebKit's media player asks for
         * "bytes=N-" to play and to seek). Left to libsoup, the part is cut
         * out of a copy of the WHOLE file made in memory for it: every such
         * request cost the daemon the file's size for as long as its answer
         * was on its way - a page with a 200 MB clip, 200 MB a request; a
         * page that keeps a few such answers waiting, that many times its
         * largest file - and the copy is the one place where this process
         * itself reads the mapped file: a file that shrinks under it (an
         * update of the wallpaper) ends the daemon with SIGBUS. One range
         * is answered here, from the mapping itself: the kernel reads it
         * when it sends, and a file cut short then is a failed send. */
        SoupMessageHeaders *rq = soup_server_message_get_request_headers (msg);
        const goffset total = (goffset) g_bytes_get_size (b);
        SoupRange *ranges = NULL;
        int n_ranges = 0;
        /* (a GET only: a HEAD is answered as without the header, as it
         * was - with the whole file's length, which is what a page asks a
         * HEAD for) */
        if (!strcmp (method, "GET") && soup_message_headers_get_ranges (rq, total, &ranges, &n_ranges)) {
            if (n_ranges == 1 && ranges[0].start < total && ranges[0].end >= ranges[0].start) {
                const goffset start = MAX (ranges[0].start, 0), end = MIN (ranges[0].end, total - 1);
                GBytes *part = g_bytes_new_from_bytes (b, (gsize) start, (gsize) (end - start + 1));
                g_bytes_unref (b);
                b = part;
                soup_message_headers_set_content_range (h, start, end, total);
                status = SOUP_STATUS_PARTIAL_CONTENT;
            } else if (n_ranges != 1) {
                /* several parts in one answer (no player asks): the whole
                 * file, which a server may always send instead */
                soup_message_headers_remove (rq, "Range");
            }
            /* (one range that begins behind the end: libsoup answers 416
             * without looking at the body) */
            soup_message_headers_free_ranges (rq, ranges);
        }
    }
    soup_message_body_append_bytes (soup_server_message_get_response_body (msg), b);
    g_bytes_unref (b);
    g_mapped_file_unref (mf);
    soup_server_message_set_status (msg, status, NULL);
    g_free (ct);
    g_free (mime);
    g_free (file);
}

/* The server answers GET and HEAD: the body of a request is never looked
 * at. libsoup collects a body whole, in memory, before it calls the
 * handler, whatever the method and before any token is checked: one POST
 * to the port cost the daemon as much memory as the sender cared to send
 * (the page itself, whose every request arrives here while its network
 * access is off, or any other program on this machine: the port takes
 * connections from every local account). A body is read and dropped piece
 * by piece instead. Set when the request starts, before its first byte is
 * read. */
static void http_conn_begin (SoupServerMessage *msg);

static void http_request_started (SoupServer *srv, SoupServerMessage *msg, gpointer user)
{
    (void) srv; (void) user;
    soup_message_body_set_accumulate (soup_server_message_get_request_body (msg), FALSE);
    http_conn_begin (msg);
}

/* Connections to the file server. libsoup keeps a connection for as long
 * as its other end does, also one that never sends a request, and every
 * one is a file descriptor of the daemon: a program that opened a thousand
 * and sent nothing (the port takes connections from every local account)
 * had the daemon at its limit of open files - the control socket accepted
 * nobody ("walpe-daemon is not running" in walpe-ctl, the tray and the
 * window), no wallpaper could be loaded, and the main loop turned on the
 * failing accept. At most HTTP_MAX_CONNS are open at once; for one more,
 * the connection that has waited longest for a request is closed (a
 * client that keeps connections for later requests opens a new one, as
 * after any server's idle timeout; a request that is being answered is
 * never the one). A page has a handful: WebKit opens six per host. */
#define HTTP_MAX_CONNS 128
typedef struct {
    gint64 since_us;    /* waiting for a request since then */
    gboolean busy;      /* a request is being answered, or the connection is being closed */
} HttpConn;
static GHashTable *http_conns;      /* GSocket * (not referenced) -> HttpConn * */

static void http_conn_gone (gpointer user, GObject *sock)
{
    (void) user;
    g_hash_table_remove (http_conns, sock);
}

static HttpConn *http_conn_of (SoupServerMessage *msg)
{
    GSocket *sk = soup_server_message_get_socket (msg);
    return sk && http_conns ? g_hash_table_lookup (http_conns, sk) : NULL;
}

/* a request starts: on a new connection, or the next one on a kept one */
static void http_conn_begin (SoupServerMessage *msg)
{
    GSocket *sk = soup_server_message_get_socket (msg);
    if (!sk)
        return;
    if (!http_conns)
        http_conns = g_hash_table_new_full (NULL, NULL, NULL, g_free);
    HttpConn *c = g_hash_table_lookup (http_conns, sk);
    if (!c) {
        if (g_hash_table_size (http_conns) >= HTTP_MAX_CONNS) {
            GHashTableIter it;
            gpointer k, v;
            GSocket *old = NULL;
            HttpConn *oc = NULL;
            g_hash_table_iter_init (&it, http_conns);
            while (g_hash_table_iter_next (&it, &k, &v)) {
                HttpConn *x = v;
                if (!x->busy && (!oc || x->since_us < oc->since_us)) {
                    old = k;
                    oc = x;
                }
            }
            static gint64 said_us;
            gint64 now = g_get_monotonic_time ();
            if (!said_us || now - said_us > 60 * G_USEC_PER_SEC) {
                said_us = now;
                g_message ("web: more than %d connections to the wallpapers' file server: the ones "
                           "that send no request are closed", HTTP_MAX_CONNS);
            }
            /* both directions: libsoup reads the end of the stream and
             * drops the connection itself (http_conn_gone) */
            if (!oc) {
                g_socket_shutdown (sk, TRUE, TRUE, NULL);
                return;
            }
            oc->busy = TRUE;
            g_socket_shutdown (old, TRUE, TRUE, NULL);
        }
        c = g_new0 (HttpConn, 1);
        g_hash_table_insert (http_conns, sk, c);
        g_object_weak_ref (G_OBJECT (sk), http_conn_gone, NULL);
    }
    c->busy = FALSE;
    c->since_us = g_get_monotonic_time ();
}

static void http_request_read (SoupServer *srv, SoupServerMessage *msg, gpointer user)
{
    (void) srv; (void) user;
    HttpConn *c = http_conn_of (msg);
    if (c)
        c->busy = TRUE;
}

static void http_request_finished (SoupServer *srv, SoupServerMessage *msg, gpointer user)
{
    (void) srv; (void) user;
    HttpConn *c = http_conn_of (msg);
    if (c) {
        c->busy = FALSE;
        c->since_us = g_get_monotonic_time ();
    }
}

static gboolean http_start (void)
{
    if (http_srv)
        return http_port != 0;
    GError *err = NULL;
    http_srv = soup_server_new ("server-header", "walpe", NULL);
    soup_server_add_handler (http_srv, NULL, http_cb, NULL, NULL);
    g_signal_connect (http_srv, "request-started", G_CALLBACK (http_request_started), NULL);
    g_signal_connect (http_srv, "request-read", G_CALLBACK (http_request_read), NULL);
    g_signal_connect (http_srv, "request-finished", G_CALLBACK (http_request_finished), NULL);
    if (!soup_server_listen_local (http_srv, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &err)) {
        g_warning ("web: loopback file server failed (%s); media in pages may not play",
                   err->message);
        g_error_free (err);
        return FALSE;
    }
    GSList *uris = soup_server_get_uris (http_srv);
    if (uris)
        http_port = (guint) g_uri_get_port (uris->data);
    g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
    g_message ("web: serving wallpapers on http://127.0.0.1:%u/<token>/", http_port);
    return http_port != 0;
}

/* ------------------------------------------- GStreamer media check
 * Pages play <video>/<audio> through WebKit's GStreamer backend in the
 * WebProcess. A missing demuxer/decoder only shows up as a silent
 * MEDIA_ERR_SRC_NOT_SUPPORTED inside the page, so when a wallpaper ships
 * media files the daemon checks the elements once (gst-inspect-1.0
 * --exists in a worker thread: GStreamer itself is never loaded into the
 * daemon) and logs exactly what is missing and which package has it. */
#define MEDIA_ANY "any media file"
typedef struct { const char *ext; const char *alts[3]; const char *what; const char *pkg; } MediaNeed;
static const MediaNeed media_needs[] = {
    /* The output WebKit asks GStreamer for when a page makes a media
     * element or an AudioContext. Without it WebKitGTK does not go silent:
     * its page process ends ("GStreamer element autoaudiosink not found.
     * Please install it", then a crash; WebKitGTK 2.54.2, also in WalPE
     * 0.14.0), the page is loaded again three times and then given up -
     * the wallpaper cannot be shown. Arch Linux has the element in a
     * package WebKitGTK only lists as optional. */
    { MEDIA_ANY, { "autoaudiosink", NULL }, "the sound output WebKit asks for - without it WebKit's page process "
      "ends when the page loads media, and the wallpaper cannot be shown;",
      "GStreamer's \"good\" plugins (Gentoo media-libs/gst-plugins-good, Arch gst-plugins-good, Debian / Ubuntu "
      "gstreamer1.0-plugins-good)" },
    { MEDIA_ANY, { "pulsesink", NULL }, "the PulseAudio / PipeWire output, which libgstwalpesound.so plays through;",
      "Gentoo media-plugins/gst-plugins-pulse, Arch gst-plugins-good, Debian / Ubuntu gstreamer1.0-plugins-good "
      "(older releases: gstreamer1.0-pulseaudio)" },
    { "wav", { "wavparse", NULL }, "WAV reader", "media-libs/gst-plugins-good" },
    { "webm", { "matroskademux", NULL }, "WebM/Matroska demuxer", "media-libs/gst-plugins-good" },
    { "webm", { "vp8dec", "avdec_vp8", NULL }, "VP8 decoder", "media-plugins/gst-plugins-vpx (or gst-plugins-libav)" },
    { "webm", { "vp9dec", "avdec_vp9", NULL }, "VP9 decoder", "media-plugins/gst-plugins-vpx (or gst-plugins-libav)" },
    { "mp4", { "qtdemux", NULL }, "MP4 demuxer", "media-libs/gst-plugins-good" },
    { "mp4", { "avdec_h264", "openh264dec", NULL }, "H.264 decoder", "media-plugins/gst-plugins-libav" },
    { "ogg", { "oggdemux", NULL }, "Ogg demuxer", "media-libs/gst-plugins-base[ogg]" },
    { "ogg", { "vorbisdec", NULL }, "Vorbis decoder", "media-libs/gst-plugins-base[vorbis]" },
    { "mp3", { "mpg123audiodec", "avdec_mp3", NULL }, "MP3 decoder", "media-plugins/gst-plugins-mpg123 (or gst-plugins-libav)" },
};

static void media_exts_scan (const char *dir, int depth, GHashTable *exts)
{
    GDir *d = depth <= 3 ? g_dir_open (dir, 0, NULL) : NULL;
    if (!d)
        return;
    const char *n;
    while ((n = g_dir_read_name (d))) {
        char *p = g_build_filename (dir, n, NULL);
        if (g_file_test (p, G_FILE_TEST_IS_DIR) && !g_file_test (p, G_FILE_TEST_IS_SYMLINK)) {
            media_exts_scan (p, depth + 1, exts);
        } else {
            const char *dot = strrchr (n, '.');
            char *e = dot ? g_ascii_strdown (dot + 1, -1) : NULL;
            const char *kind = NULL;
            if (e && (!strcmp (e, "mkv") || !strcmp (e, "webm")))
                kind = "webm";
            else if (e && (!strcmp (e, "mp4") || !strcmp (e, "m4v") || !strcmp (e, "mov")))
                kind = "mp4";
            else if (e && (!strcmp (e, "ogg") || !strcmp (e, "ogv") || !strcmp (e, "oga")))
                kind = "ogg";
            else if (e && !strcmp (e, "mp3"))
                kind = "mp3";
            else if (e && !strcmp (e, "wav"))
                kind = "wav";
            else if (e && (!strcmp (e, "flac") || !strcmp (e, "opus") || !strcmp (e, "m4a") || !strcmp (e, "aac")))
                kind = MEDIA_ANY;
            if (kind) {
                g_hash_table_add (exts, g_strdup (kind));
                /* (whatever it is: the page will ask for a sound output) */
                g_hash_table_add (exts, g_strdup (MEDIA_ANY));
            }
            g_free (e);
        }
        g_free (p);
    }
    g_dir_close (d);
}

static gboolean gst_has (const char *element)
{
    const char *argv[] = { "gst-inspect-1.0", "--exists", element, NULL };
    int status = 0;
    if (!g_spawn_sync (NULL, (char **) argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                       NULL, NULL, NULL, NULL, &status, NULL))
        return TRUE;  /* no gst-inspect: cannot tell, do not cry wolf */
    return g_spawn_check_wait_status (status, NULL);
}

static char *media_missing;     /* last check result (status), under reg_lock */

static void media_check_thread (GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    (void) t; (void) src; (void) c;
    char *dir = data;
    GHashTable *exts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    media_exts_scan (dir, 0, exts);
    GString *miss = g_string_new (NULL);
    for (guint i = 0; i < G_N_ELEMENTS (media_needs); i++) {
        const MediaNeed *m = &media_needs[i];
        if (!g_hash_table_contains (exts, m->ext))
            continue;
        gboolean ok = FALSE;
        for (int k = 0; m->alts[k] && !ok; k++)
            ok = gst_has (m->alts[k]);
        if (!ok) {
            if (!strcmp (m->ext, MEDIA_ANY))
                g_warning ("web: GStreamer element %s missing (%s %s has media files): install %s",
                           m->alts[0], m->what, dir, m->pkg);
            else
                g_warning ("web: GStreamer element %s missing (%s for .%s media in %s): install %s",
                           m->alts[0], m->what, m->ext, dir, m->pkg);
            g_string_append_printf (miss, "%s%s", miss->len ? "," : "", m->alts[0]);
        }
    }
    if (g_hash_table_size (exts) && !miss->len)
        g_message ("web: GStreamer media elements OK for %s", dir);
    g_mutex_lock (&reg_lock);
    g_free (media_missing);
    media_missing = g_string_free (miss, FALSE);
    g_mutex_unlock (&reg_lock);
    g_hash_table_unref (exts);
}

static void media_check_start (const char *dir)
{
    GTask *t = g_task_new (NULL, NULL, NULL, NULL);
    g_task_set_task_data (t, g_strdup (dir), g_free);
    g_task_run_in_thread (t, media_check_thread);
    g_object_unref (t);
}

char *walpe_web_media_missing (void)
{
    g_mutex_lock (&reg_lock);
    char *r = g_strdup (media_missing ? media_missing : "");
    g_mutex_unlock (&reg_lock);
    return r;
}

static gboolean use_scheme (void)
{
    const char *e = g_getenv ("WALPE_WEB_SCHEME");
    return e && *e && strcmp (e, "0");
}

static gboolean use_file_urls (void)
{
    const char *e = g_getenv ("WALPE_WEB_FILE_URLS");
    return e && *e && *e != '0';
}

/* ----------------------------------------------------------- JS pushes */

static void sh_eval (WebShared *sh, const char *js)
{
    if (sh->view && !sh->dead)
        webkit_web_view_evaluate_javascript (sh->view, js, -1, NULL, NULL, NULL, NULL, NULL);
}

/* WALPE_PROP_PROBE=<dir> (propprobe.h; the web module logs on its own):
 * every applyUserProperties payload and the page's acknowledgement go to
 * <dir>/web.<wallpaper dir name>.log, one JSON object per line */
static void web_probe_log (WebShared *sh, const char *kind, const char *json)
{
    const char *dir = g_getenv ("WALPE_PROP_PROBE");
    if (!dir || !*dir)
        return;
    char *bn = g_path_get_basename (sh->origin);
    char *f = g_strdup_printf ("%s/web.%s.log", dir, bn);
    g_mkdir_with_parents (dir, 0700);
    FILE *fp = fopen (f, "a");
    if (fp) {
        fprintf (fp, "{\"kind\":\"%s\",\"data\":%s}\n", kind, json);
        fclose (fp);
    }
    g_free (f);
    g_free (bn);
}

/* the files of a directory property, as WE lists them for the page:
 * regular files of the kinds a wallpaper shows (pictures; videos too for
 * "fileType": "video"), sorted, absolute paths. Sub-folders one level
 * deep are included. Capped: a page gets them all in one call. */
static void dir_files (const char *dir, gboolean video, int depth, GPtrArray *out)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d)
        return;
    const char *n;
    while ((n = g_dir_read_name (d)) && out->len < 4000) {
        if (n[0] == '.')
            continue;
        /* a name that is not UTF-8 cannot be put into the script that
         * hands the page its files: with one such file in the folder the
         * whole call was not valid text, WebKit ran nothing of it, and the
         * page got neither the folder nor the other options of that push */
        if (!g_utf8_validate (n, -1, NULL))
            continue;
        char *p = g_build_filename (dir, n, NULL);
        if (g_file_test (p, G_FILE_TEST_IS_DIR)) {
            if (depth < 1)
                dir_files (p, video, depth + 1, out);
            g_free (p);
            continue;
        }
        if (dir_media_name (n, video) && g_file_test (p, G_FILE_TEST_IS_REGULAR))
            g_ptr_array_add (out, p);
        else
            g_free (p);
    }
    g_dir_close (d);
}

static int path_cmp (gconstpointer a, gconstpointer b)
{
    return strcmp (*(const char *const *) a, *(const char *const *) b);
}

/* one property as the page sees it: {"type": t, "value": v}. Value
 * shapes follow WE: bool -> boolean, slider -> number, color -> "r g b"
 * (floats 0..1), combo -> the chosen option's own value (string or
 * number, as the project declares it), text -> string, file/directory ->
 * a path the page appends to 'file:///' ("" when unset). */
static JsonNode *sh_prop_node (WebShared *sh, const char *key, JsonObject *d)
{
    const char *ty = wpj_str (d, "type", "");
    JsonNode *dv = json_object_get_member (d, "value");
    JsonNode *pv = sh->preset ? json_object_get_member (sh->preset, key) : NULL;
    if (pv && json_node_get_node_type (pv) == JSON_NODE_VALUE)
        dv = pv; /* a WE preset's value replaces the default */
    else
        pv = NULL;
    if (dv && json_node_get_node_type (dv) != JSON_NODE_VALUE)
        dv = NULL;
    char *ov = walpe_config_get_prop (sh->origin, key);
    gboolean is_file = !strcmp (ty, "file") || !strcmp (ty, "directory");
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "type");
    json_builder_add_string_value (b, ty);
    json_builder_set_member_name (b, "value");
    if (is_file) {
        /* an absolute path is the USER's choice only (an override): a
         * default or preset value comes from the workshop item, and
         * "value": "/" there opened every local file to the page (fetch
         * /<token>/__walpe_file/home/...). A preset's "files/x.png" is
         * relative to the preset item: the page gets the absolute path
         * (pages build 'file:///' + value; the WE_API_JS shim maps that to
         * the loopback server's file route). */
        const char *fv = ov ? ov : dv && json_node_get_value_type (dv) == G_TYPE_STRING
                                       ? json_node_get_string (dv) : NULL;
        char *abs = NULL, *own = NULL;
        if (fv && g_str_has_prefix (fv, "file://")) {
            own = g_filename_from_uri (fv, NULL, NULL);
            fv = own;
        }
        if (fv && *fv && g_path_is_absolute (fv)) {
            if (ov)
                abs = g_strdup (fv);
        } else if (fv && *fv && !strstr (fv, "..")) {
            const char *roots[2] = { pv && !ov ? sh->preset_dir : NULL, sh->path };
            for (int r = 0; r < 2 && !abs; r++) {
                if (!roots[r])
                    continue;
                /* a default / preset value names something INSIDE the
                 * item: resolved, so a symlink shipped in the folder
                 * ("leak" -> /etc, -> $HOME) cannot put its target on the
                 * allow list (only the user's own choice may point
                 * outside) */
                char *c = g_build_filename (roots[r], fv, NULL);
                char *rc = realpath (c, NULL);
                if (rc && path_under (rc, roots[r]))
                    abs = c;
                else
                    g_free (c);
                free (rc);
            }
        }
        if (abs)
            g_ptr_array_add (sh->file_allow, g_strdup (abs));
        json_builder_add_string_value (b, abs ? abs : "");
        g_free (abs);
        g_free (own);
    } else if (ov) {
        /* overrides are stored as strings: back to the declared type */
        GType t = dv ? json_node_get_value_type (dv) : G_TYPE_INVALID;
        JsonNode *opt = NULL;
        if (!strcmp (ty, "combo")) {
            /* the option the user picked, typed as the project wrote it */
            /* json-glib directly: the module may only call the core
             * functions listed in modules.dynlist */
            JsonNode *oan = json_object_get_member (d, "options");
            JsonArray *oa = oan && JSON_NODE_HOLDS_ARRAY (oan) ? json_node_get_array (oan) : NULL;
            for (guint i = 0; oa && i < json_array_get_length (oa) && !opt; i++) {
                JsonNode *oon = json_array_get_element (oa, i);
                JsonObject *oo = oon && JSON_NODE_HOLDS_OBJECT (oon) ? json_node_get_object (oon) : NULL;
                JsonNode *on = oo ? json_object_get_member (oo, "value") : NULL;
                if (!on || json_node_get_node_type (on) != JSON_NODE_VALUE)
                    continue;
                char buf[G_ASCII_DTOSTR_BUF_SIZE];
                const char *os = NULL;
                GType ot = json_node_get_value_type (on);
                if (ot == G_TYPE_STRING)
                    os = json_node_get_string (on);
                else if (ot == G_TYPE_BOOLEAN)
                    os = json_node_get_boolean (on) ? "true" : "false";
                else
                    os = g_ascii_dtostr (buf, sizeof (buf), json_node_get_double (on));
                if (!g_strcmp0 (os, ov))
                    opt = on;
                else if (ot != G_TYPE_STRING && ot != G_TYPE_BOOLEAN) {
                    char *e = NULL;
                    double x = g_ascii_strtod (ov, &e);
                    if (e && e != ov && !*e && x == json_node_get_double (on))
                        opt = on;
                }
            }
        }
        char *end = NULL;
        double num = g_ascii_strtod (ov, &end);
        gboolean numeric = end && end != ov && !*end && isfinite (num);
        if (opt)
            json_builder_add_value (b, json_node_copy (opt));
        else if (t == G_TYPE_BOOLEAN || !strcmp (ty, "bool") || !strcmp (ty, "checkbox"))
            json_builder_add_boolean_value (b, !g_ascii_strcasecmp (ov, "true") || !g_strcmp0 (ov, "1"));
        else if ((t == G_TYPE_INT64 || t == G_TYPE_DOUBLE || !strcmp (ty, "slider")) && numeric) {
            if (num == (double) (gint64) num && fabs (num) < 9e15)
                json_builder_add_int_value (b, (gint64) num);
            else
                json_builder_add_double_value (b, num);
        } else
            json_builder_add_string_value (b, ov);
    } else if (dv) {
        json_builder_add_value (b, json_node_copy (dv));
    } else if (!strcmp (ty, "bool") || !strcmp (ty, "checkbox")) {
        json_builder_add_boolean_value (b, FALSE);
    } else if (!strcmp (ty, "textinput") || !strcmp (ty, "text")) {
        json_builder_add_string_value (b, "");
    } else {
        json_builder_add_null_value (b);
    }
    json_builder_end_object (b);
    JsonNode *root = json_builder_get_root (b);
    g_object_unref (b);
    g_free (ov);
    return root;
}

/* applyUserProperties / applyGeneralProperties / directory callbacks.
 * all = the page just loaded: every property, like WE's first call.
 * Otherwise only what changed since the last push (a settings change):
 * pages restart their video / slideshow / visualizer on every property
 * they are handed, so re-sending all of them on each change reset the
 * page. Directory properties additionally get their file lists
 * (userDirectoryFilesAddedOrChanged / ...Removed) and
 * wallpaperRequestRandomFileForProperty answers from them. The lists go
 * out before applyUserProperties: slideshow pages start (or restart)
 * from "if (properties.customdirectory) changeBackground()" and would
 * otherwise show nothing new until their next switch, minutes later.
 * (That order is inferred from those pages, not checked against WE.) */
static void sh_push_properties (WebShared *sh, gboolean all)
{
    char *pjp = g_build_filename (sh->path, "project.json", NULL);
    JsonNode *pj = wpj_parse_file (pjp);
    g_free (pjp);
    JsonObject *defs = pj && json_node_get_node_type (pj) == JSON_NODE_OBJECT
        ? wpj_obj (wpj_obj (json_node_get_object (pj), "general"), "properties") : NULL;
    if (sh->file_allow)
        g_ptr_array_unref (sh->file_allow);
    sh->file_allow = g_ptr_array_new_with_free_func (g_free);
    if (!sh->pushed) {
        sh->pushed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
        sh->pushed_dirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    }
    if (all) {
        g_hash_table_remove_all (sh->pushed);
        g_hash_table_remove_all (sh->pushed_dirs);
    }
    GString *props = g_string_new ("{");
    GString *dirs = g_string_new (NULL);   /* JS statements */
    GString *dirlog = g_string_new ("{");  /* WALPE_PROP_PROBE: key -> files */
    guint nsent = 0;
    GList *ks = defs ? json_object_get_members (defs) : NULL;
    for (GList *k = ks; k; k = k->next) {
        JsonObject *d = wpj_obj (defs, k->data);
        if (!d)
            continue;
        JsonNode *pn = sh_prop_node (sh, k->data, d);
        char *txt = json_to_string (pn, FALSE);
        JsonNode *kn = json_node_new (JSON_NODE_VALUE);
        json_node_set_string (kn, k->data);
        char *kq = json_to_string (kn, FALSE);
        json_node_unref (kn);
        const char *old = g_hash_table_lookup (sh->pushed, k->data);
        if (!old || strcmp (old, txt)) {
            g_string_append_printf (props, "%s%s:%s", nsent ? "," : "", kq, txt);
            nsent++;
            g_hash_table_insert (sh->pushed, g_strdup (k->data), g_strdup (txt));
        }
        if (!g_strcmp0 (wpj_str (d, "type", ""), "directory")) {
            const char *dir = wpj_str (json_node_get_object (pn), "value", "");
            GPtrArray *fl = g_ptr_array_new_with_free_func (g_free);
            if (*dir)
                dir_files (dir, !g_strcmp0 (wpj_str (d, "fileType", ""), "video"), 0, fl);
            g_ptr_array_sort (fl, path_cmp);
            JsonBuilder *fb = json_builder_new ();
            json_builder_begin_array (fb);
            for (guint i = 0; i < fl->len; i++)
                json_builder_add_string_value (fb, g_ptr_array_index (fl, i));
            json_builder_end_array (fb);
            JsonNode *fr = json_builder_get_root (fb);
            char *ftxt = json_to_string (fr, FALSE);
            guint nfiles = fl->len;
            json_node_unref (fr);
            g_object_unref (fb);
            g_ptr_array_unref (fl);
            const char *fold = g_hash_table_lookup (sh->pushed_dirs, k->data);
            if (g_strcmp0 (fold, ftxt) && (fold || strcmp (ftxt, "[]"))) {
                g_string_append_printf (dirs, "D(%s,%s,%s);", kq, fold ? fold : "[]", ftxt);
                g_string_append_printf (dirlog, "%s%s:%u", dirlog->len > 1 ? "," : "", kq, nfiles);
                g_hash_table_insert (sh->pushed_dirs, g_strdup (k->data), g_strdup (ftxt));
            }
            g_free (ftxt);
        }
        g_free (kq);
        g_free (txt);
        json_node_unref (pn);
    }
    g_list_free (ks);
    g_string_append_c (props, '}');
    if (pj)
        json_node_unref (pj);
    if (nsent || dirs->len || all) {
        char *gen = all ? g_strdup_printf ("if(L&&L.applyGeneralProperties)L.applyGeneralProperties({fps:%d});",
                                           sh->fps) : g_strdup ("");
        /* scheme color also as a CSS variable (some pages style from it) */
        char *js = g_strdup_printf (
            "(function(){var p=%s;var L=window.wallpaperPropertyListener;var er='';"
            "var F=window.__walpeDirFiles=window.__walpeDirFiles||{};"
            "window.wallpaperRequestRandomFileForProperty=function(n,cb){var a=F[n]||[];"
            "var f=a.length?a[Math.floor(Math.random()*a.length)]:null;"
            "setTimeout(function(){try{if(cb)cb(n,f);}catch(e){}},0);};"
            "function D(n,o,a){var gone=o.filter(function(x){return a.indexOf(x)<0;});"
            "var add=a.filter(function(x){return o.indexOf(x)<0;});F[n]=a;"
            "try{if(gone.length&&L&&L.userDirectoryFilesRemoved)L.userDirectoryFilesRemoved(n,gone);}"
            "catch(e){er+=' dir-:'+e;}"
            "try{if(add.length&&L&&L.userDirectoryFilesAddedOrChanged)L.userDirectoryFilesAddedOrChanged(n,add);}"
            "catch(e){er+=' dir+:'+e;}}"
            "try{if(p.schemecolor&&typeof p.schemecolor.value==='string'){"
            "var c=p.schemecolor.value.split(' ').map(function(x){return Math.round(parseFloat(x)*255)});"
            "document.documentElement.style.setProperty('--scheme-color','rgb('+c.join(',')+')');}}catch(e){}"
            "%s"
            "try{if(%d&&L&&L.applyUserProperties)L.applyUserProperties(p);}catch(e){er+=' props:'+e;}"
            "%s"
            /* a page that sets its listener after the load event (from a
             * timer, a script loaded later, a fetch): the first call with
             * every option waits for it - looked for after 50 ms, 100 ms,
             * ... for about a minute - instead of never being made. A
             * change that arrives meanwhile goes into the waiting set. */
            "try{if(%d){if(L&&L.applyUserProperties)window.__walpeLate=null;"
            "else{window.__walpeLate=p;(function w(d){setTimeout(function(){"
            "var q=window.__walpeLate,l=window.wallpaperPropertyListener;if(!q)return;"
            "if(l&&l.applyUserProperties){window.__walpeLate=null;"
            "try{if(l.applyGeneralProperties)l.applyGeneralProperties({fps:%d});}catch(e){}"
            "try{l.applyUserProperties(q);}catch(e){}"
            "try{if(%d)window.webkit.messageHandlers.walpe.postMessage('probe:'+JSON.stringify({"
            "listener:true,late:true,keys:Object.keys(q).length,error:''}));}catch(e){}"
            "}else if(d<30000)w(d*2);else window.__walpeLate=null;},d);})(50);}}"
            "else if(window.__walpeLate){for(var k in p)window.__walpeLate[k]=p[k];}}catch(e){}"
            "try{if(%d)window.webkit.messageHandlers.walpe.postMessage('probe:'+JSON.stringify({"
            "listener:!!(L&&L.applyUserProperties),keys:Object.keys(p).length,error:er}));}catch(e){}"
            "})();",
            props->str, dirs->str, nsent > 0, gen, all != FALSE, sh->fps,
            g_getenv ("WALPE_PROP_PROBE") != NULL, g_getenv ("WALPE_PROP_PROBE") != NULL);
        sh_eval (sh, js);
        sh_audio_wake (sh);     /* a listener may draw with the new values */
        g_free (js);
        g_free (gen);
        web_probe_log (sh, all ? "load" : "change", props->str);
        if (dirlog->len > 1) {
            g_string_append_c (dirlog, '}');
            web_probe_log (sh, "dirs", dirlog->str);
        }
        g_debug ("web: %s: %u propert%s sent%s", all ? "load" : "change", nsent,
                 nsent == 1 ? "y" : "ies", dirs->len ? " + directory lists" : "");
    }
    g_string_free (props, TRUE);
    g_string_free (dirs, TRUE);
    g_string_free (dirlog, TRUE);
}

static void hide_window_offscreen (GtkWidget *win, int w, int h);
#ifdef WEB_HAVE_PRIVATE
static void web_private_screen (int w, int h);
#endif

/* ------------------------------------------------------ the page's sound
 * A wallpaper page plays its sound by itself: <audio> and <video> elements,
 * Audio objects made by its script, Web Audio. Three things are WalPE's:
 *
 * It may start. WebKit's default lets media with a sound track start only
 * after the user has clicked the page, as in a browser: a wallpaper's
 * background music never began, `new Audio (...).play ()` was refused
 * (NotAllowedError), an AudioContext stayed "interrupted", and a <video
 * autoplay> that has a sound track and is not muted showed its first
 * picture for ever. The page gets WEBKIT_AUTOPLAY_ALLOW (sh_create_real).
 *
 * How loud it is: a level for each page - the volume setting (0 while
 * "mute" is on) times the wallpaper's own volume while sound is on for the
 * wallpaper (its own switch, else the setting "scene_sound"), 0 otherwise,
 * and 0 in every program that never said (walpe_web_sound,
 * walpe_web_sound_wallpaper in web.h). WebKitGTK
 * has no volume for a page, only a mute. The mute is what makes "off" and
 * "mute" hold: at level 0 the view is muted, and no script of the page can
 * undo that. What is in between is the work of a script in every frame of
 * the page (SOUND_JS), which runs among the page's own scripts - no other
 * place reaches an Audio object or an AudioContext - and is therefore an
 * agreement, not a wall: a page that sets out to defeat it can be louder
 * than the volume says, never louder than off. (It has no global the page
 * could call, and is told what to do by events whose names the page does
 * not know, sent from a script world of WalPE's own: sh_sound_tell.) The
 * script scales what the page itself sets: the volume of each
 * media element - the page reads back the value it wrote - and a gain in
 * front of each AudioContext's output (an element routed into an
 * AudioContext is left alone: the gain has it). At level 0 that is
 * silence: a video goes on playing its pictures, and an element that is
 * sound alone, or an AudioContext, is not played into nothing - it is
 * held (paused / suspended; a play () of the page is taken and kept) and
 * starts when there is a level, as a scene's sound layer does. That is
 * also what lets sound come back after the mute: an Audio object that is
 * started in a muted view never plays, not after the view is unmuted
 * either (WebKitGTK 2.52), so nothing is started while it is muted - the
 * view is unmuted first, then the script lets go. (A page that gets round
 * the script and starts its sound in the muted view has silenced itself
 * until it is loaded again.) The script starts with the level of the
 * moment it was made with, so a page that loads again, and a frame that
 * is made later, begin right; a change replaces it and tells the running
 * page (sh_sound_apply), which hands it on to its frames of the same site.
 * A frame from another site keeps scripts out: it has the level of the
 * moment it was made, and the mute.
 *
 * Where level and mute end up. WebKit hands an element's volume and the
 * view's mute to its audio sink, and GStreamer's pulsesink makes them the
 * volume and mute of the stream in the sound server - stored there for
 * later streams of the same media.role, other programs' too, and read
 * back by WebKit into the element. A level per page does not belong
 * there: the page process's sink is gst/walpesound.c (found through
 * GST_PLUGIN_PATH: main.c web_env_sound_sink), which keeps both in the
 * page process. Nothing in this file depends on which sink plays.
 *
 * Silence while paused: sh_suspend has the script pause every element it
 * knows and those in the document, and suspend the AudioContexts that
 * have an output; then the view is muted (what the script did not reach).
 * Resume unmutes first and starts them again after it. */
static gint web_sound_q16;          /* atomic: the master level, 0..65536 */
static gint web_sound_on;           /* atomic: wallpaper sound is switched on */
static GPtrArray *sound_pages;      /* main thread: the pages that have a view */

#define SOUND_WORLD "walpe-sound"
#define SOUND_JS_HEAD "(function(){var W=window,K='"
#define SOUND_JS_MID "',L="
#define SOUND_JS_TAIL \
    ",S=0,D=document,H=W.HTMLMediaElement,M=H&&H.prototype,d=M&&Object.getOwnPropertyDescriptor(M,'volume');\n" \
    "var own=new WeakMap(),routed=new WeakSet(),els=[],outs=[],V=W.HTMLVideoElement,pl=M&&M.play,pa=M&&M.pause;\n" \
    "var ET=W.EventTarget.prototype,on=ET.addEventListener,fire=ET.dispatchEvent,Ev=W.Event,call=Reflect.apply;\n" \
    "function each(a,f){for(var i=a.length-1;i>=0;i--){var x=a[i].deref();if(x)f(x);else a.splice(i,1);}}\n" \
    "function ap(m){try{d.set.call(m,own.get(m)*(routed.has(m)?1:L));}catch(e){}}\n" \
    "var watch=null;\n" \
    "function reg(m){if(!d||!(m instanceof H)||own.has(m))return;own.set(m,d.get.call(m));\n" \
    " if(els.length>64)each(els,function(){});els.push(new WeakRef(m));ap(m);if(watch)watch(m);}\n" \
    /* (sound alone, at level 0: not played into nothing, held until there is a level) */ \
    "function held(m){return L<=0&&!(V&&m instanceof V)&&!routed.has(m);}\n" \
    "function go(m){try{var p=pl.call(m);if(p&&p.catch)p.catch(function(){});}catch(e){}}\n" \
    "if(d&&d.set&&d.configurable&&pl&&pa){\n" \
    " Object.defineProperty(M,'volume',{configurable:true,enumerable:d.enumerable,\n" \
    "  get:function(){return own.has(this)?own.get(this):d.get.call(this);},\n" \
    "  set:function(v){v=Number(v);if(!(v>=0&&v<=1)||!(this instanceof H)){d.set.call(this,v);return;}\n" \
    "   reg(this);own.set(this,v);ap(this);}});\n" \
    " M.play=function(){reg(this);if(this instanceof H&&held(this)){this.__walpeHeld=1;return Promise.resolve();}\n" \
    "  return pl.apply(this,arguments);};\n" \
    " M.pause=function(){if(this instanceof H)this.__walpeHeld=this.__walpeResume=0;return pa.apply(this,arguments);};\n" \
    " if(M.load){var ld=M.load;M.load=function(){reg(this);return ld.apply(this,arguments);};}\n" \
    " ['src','srcObject','autoplay'].forEach(function(n){var p=Object.getOwnPropertyDescriptor(M,n);\n" \
    "  if(p&&p.set&&p.configurable)Object.defineProperty(M,n,{configurable:true,enumerable:p.enumerable,get:p.get,\n" \
    "   set:function(v){reg(this);p.set.call(this,v);}});});\n" \
    /* A looping Ogg Vorbis sound: the jump back to its start mostly never ends in \
     * WebKitGTK 2.52 with GStreamer 1.24 - the element says "seeking" for ever, is \
     * silent and asks the server for nothing; the same in a plain WebKitGTK window, \
     * whatever a server answers (WAV, MP3, Opus loops are not affected). A sound \
     * (not a video) whose jump to the start has not ended after 0.3 s is loaded \
     * again, and from then on this script starts it again each time it ends, which \
     * does not stick; the page still reads loop as true and sees no "ended". A \
     * start that does not get going is loaded again as well, 0.6 s, 1.2 s ... 4.8 s \
     * later. Nothing is started while the level is 0 or the wallpaper is paused: \
     * such an element is left held, as play () of the page is. */ \
    " var lo=Object.getOwnPropertyDescriptor(M,'loop'),st=W.setTimeout,ct=W.clearTimeout;\n" \
    " if(ld&&lo&&lo.get&&lo.set&&lo.configurable&&st&&ct){\n" \
    "  Object.defineProperty(M,'loop',{configurable:true,enumerable:lo.enumerable,\n" \
    "   get:function(){return this.__walpeLoop?true:lo.get.call(this);},\n" \
    "   set:function(v){if(this.__walpeLoop){if(!v)this.__walpeLoop=0;}else lo.set.call(this,v);}});\n" \
    "  watch=function(m){if(V&&m instanceof V)return;var t=0;\n" \
    "   function kick(n){call(ct,W,[t]);t=call(st,W,[function(){\n" \
    "    if(S||L<=0||m.paused||m.currentTime>0.05)return;\n" \
    "    if(!m.__walpeLoop){if(!m.seeking||!lo.get.call(m))return;lo.set.call(m,false);m.__walpeLoop=1;}\n" \
    "    ld.call(m);go(m);kick(n<4?n+1:n);},300<<n]);}\n" \
    "   call(on,m,['seeking',function(){if(!m.__walpeLoop)kick(0);}]);\n" \
    "   call(on,m,['seeked',function(){if(!m.__walpeLoop)call(ct,W,[t]);}]);\n" \
    "   call(on,m,['ended',function(e){if(!m.__walpeLoop)return;e.stopImmediatePropagation();\n" \
    "    try{m.currentTime=0;}catch(x){}\n" \
    "    if(S)m.__walpeResume=1;else if(held(m))m.__walpeHeld=1;else{go(m);kick(1);}},true]);};\n" \
    " }\n" \
    " call(on,D,['loadstart',function(e){reg(e.target);},true]);\n" \
    /* (an element that starts by its autoplay attribute) */ \
    " call(on,D,['play',function(e){var m=e.target;reg(m);if(m instanceof H&&held(m)){m.__walpeHeld=1;pa.call(m);}},true]);\n" \
    "}\n" \
    /* (outs holds the contexts and their gains for good: the page may drop \
     * every reference of its own while the context goes on sounding; at \
     * level 0 a context that gets an output is suspended until there is one) */ \
    "var A=W.AudioNode&&W.AudioNode.prototype,T=W.AudioDestinationNode,O=W.OfflineAudioContext;\n" \
    "function hold(c){try{if(c.state==='running'){c.__walpeHeld=1;c.suspend();}}catch(e){}}\n" \
    "if(A&&T){var oc=A.connect,od=A.disconnect;\n" \
    " var mg=function(t,make){var c=t.context;if(O&&c instanceof O)return null;\n" \
    "  for(var i=0;i<outs.length;i++)if(outs[i].c===c)return outs[i].g;\n" \
    "  if(!make)return null;var g=c.createGain();g.gain.value=L;oc.call(g,t);outs.push({c:c,g:g});\n" \
    "  if(L<=0)hold(c);return g;};\n" \
    " A.connect=function(t){var g=null;try{g=t instanceof T?mg(t,1):null;}catch(e){}if(!g)return oc.apply(this,arguments);\n" \
    "  var a=Array.prototype.slice.call(arguments);a[0]=g;oc.apply(this,a);return t;};\n" \
    " A.disconnect=function(t){var g=null;try{g=t instanceof T?mg(t,0):null;}catch(e){}if(!g)return od.apply(this,arguments);\n" \
    "  var a=Array.prototype.slice.call(arguments);a[0]=g;return od.apply(this,a);};\n" \
    " var C=W.AudioContext&&W.AudioContext.prototype,cs=C&&C.createMediaElementSource;\n" \
    " if(cs)C.createMediaElementSource=function(m){var r=cs.apply(this,arguments);\n" \
    "  try{reg(m);if(own.has(m)){routed.add(m);ap(m);}}catch(e){}return r;};\n" \
    "}\n" \
    "function vol(v){L=v;each(els,function(m){ap(m);\n" \
    "  if(held(m)){if(!m.paused&&!m.ended){m.__walpeHeld=1;pa.call(m);}}\n" \
    "  else if(m.__walpeHeld){m.__walpeHeld=0;if(S)m.__walpeResume=1;else go(m);}});\n" \
    " outs.forEach(function(o){var c=o.c;o.g.gain.value=v;try{if(v<=0)hold(c);\n" \
    "  else if(c.__walpeHeld){c.__walpeHeld=0;if(S)c.__walpeResume=1;else c.resume();}}catch(e){}});}\n" \
    "function snd(s){S=s;var seen=new Set();\n" \
    " function one(m){if(seen.has(m))return;seen.add(m);\n" \
    "  if(s){if(!m.paused&&!m.ended){m.__walpeResume=1;if(pa)pa.call(m);else m.pause();}}\n" \
    "  else if(m.__walpeResume){m.__walpeResume=0;if(held(m))m.__walpeHeld=1;else if(pl)go(m);else m.play();}}\n" \
    " D.querySelectorAll('video,audio').forEach(one);each(els,one);\n" \
    " outs.forEach(function(o){var c=o.c;try{if(s){if(c.state==='running'){c.__walpeResume=1;c.suspend();}}\n" \
    "  else if(c.__walpeResume){c.__walpeResume=0;if(L<=0)c.__walpeHeld=1;else c.resume();}}catch(e){}});}\n" \
    /* What the daemon says arrives as events on the document whose names \
     * begin with K, sent from the script world SOUND_WORLD (sh_sound_tell): \
     * 'b', the level's digits in ten-thousandths, 'v'; 's1' / 's0'. A frame \
     * of the same site gets them handed on; nothing here is a global. */ \
    "var acc='';\n" \
    "function hear(n,f){call(on,D,[K+n,function(e){try{e.stopImmediatePropagation();}catch(x){}f();\n" \
    " for(var i=0;i<W.frames.length;i++){try{var k=W.frames[i].document;if(k)call(fire,k,[new Ev(K+n)]);}catch(x){}}},true]);}\n" \
    "hear('b',function(){acc='';});\n" \
    "'0123456789'.split('').forEach(function(c){hear(c,function(){if(acc.length<6)acc+=c;});});\n" \
    "hear('v',function(){var v=parseInt(acc,10)/10000;acc='';if(v>=0&&v<=1)vol(v);});\n" \
    "hear('s1',function(){snd(1);});hear('s0',function(){snd(0);});\n" \
    "})();"

static void web_random_bytes (void *buf, gsize n);

/* main thread: `what` to the page's copy of the sound script - "s1" / "s0"
 * (the wallpaper is paused / goes on), or NULL for the level q (0..65536).
 * Sent as events on the document from a script world of WalPE's own: the
 * page's scripts see neither the names nor who sent them. */
static void sh_sound_tell (WebShared *sh, const char *what, int q)
{
    if (!sh->view || sh->dead || !sh->snd_key[0])
        return;
    GString *js = g_string_new ("(function(){var d=document,E=Event;function f(n){d.dispatchEvent(new E('");
    g_string_append (js, sh->snd_key);
    g_string_append (js, "'+n));}");
    if (what) {
        g_string_append_printf (js, "f('%s');", what);
    } else {
        char num[16];
        g_snprintf (num, sizeof num, "%05d", (int) (((gint64) CLAMP (q, 0, 65536) * 10000 + 32768) >> 16));
        g_string_append (js, "f('b');");
        for (const char *c = num; *c; c++)
            g_string_append_printf (js, "f('%c');", *c);
        g_string_append (js, "f('v');");
    }
    g_string_append (js, "})();");
    webkit_web_view_evaluate_javascript (sh->view, js->str, -1, SOUND_WORLD, NULL, NULL, NULL, NULL);
    g_string_free (js, TRUE);
}


/* main thread: the page gets the level of now - at its creation (before it
 * loads anything) and when the level changed */
static void sh_sound_apply (WebShared *sh)
{
    if (!sh->view || sh->dead)
        return;
    /* the page's level: the master's times the wallpaper's own volume while
     * sound is on for the wallpaper (its own switch, else the setting) */
    const gboolean on = sh->snd_sw > 0 || (sh->snd_sw < 0 && g_atomic_int_get (&web_sound_on));
    const int q = on ? (int) ((gint64) g_atomic_int_get (&web_sound_q16) * sh->snd_vol_q16 >> 16) : 0;
    if (q == sh->snd_q16)
        return;
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd (num, sizeof num, "%.4f", q / 65536.0);
    if (!sh->snd_key[0]) {
        guint32 r[2];
        web_random_bytes (r, sizeof r);
        g_snprintf (sh->snd_key, sizeof sh->snd_key, "walpe%08x%08x", r[0], r[1]);
    }
    /* up from 0: unmuted before the script lets anything start */
    if (q > 0 && !sh->suspended)
        webkit_web_view_set_is_muted (sh->view, FALSE);
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager (sh->view);
    if (sh->snd_script) {
        webkit_user_content_manager_remove_script (ucm, sh->snd_script);
        webkit_user_script_unref (sh->snd_script);
    }
    char *src = g_strconcat (SOUND_JS_HEAD, sh->snd_key, SOUND_JS_MID, num, SOUND_JS_TAIL, NULL);
    sh->snd_script = webkit_user_script_new (src, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                             WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    g_free (src);
    webkit_user_content_manager_add_script (ucm, sh->snd_script);
    if (sh->snd_q16 >= 0)       /* a page that runs */
        sh_sound_tell (sh, NULL, q);
    else
        g_message ("web: the page's sound is at %d %%%s", (q * 100 + 32768) / 65536,
                   q ? "" : " (walpe-ctl set scene_sound=on volume=0.5 mute=off)");
    /* down to 0: what makes off and mute hold, whatever the page does */
    if (q <= 0)
        webkit_web_view_set_is_muted (sh->view, TRUE);
    sh->snd_q16 = q;
}

static gboolean web_sound_main (gpointer user)
{
    (void) user;
    for (guint i = 0; sound_pages && i < sound_pages->len; i++)
        sh_sound_apply (sound_pages->pdata[i]);
    return G_SOURCE_REMOVE;
}

/* any thread (modules.h web_sound): the settings changed, or the module
 * was loaded and is told what they are */
static void walpe_web_sound_level (double master, gboolean enabled)
{
    const int q = (int) (CLAMP (master, 0.0, 1.0) * 65536.0);
    if (g_atomic_int_get (&web_sound_q16) == q && g_atomic_int_get (&web_sound_on) == (enabled ? 1 : 0))
        return;
    g_atomic_int_set (&web_sound_q16, q);
    g_atomic_int_set (&web_sound_on, enabled ? 1 : 0);
    g_idle_add (web_sound_main, NULL);
}

/* the wallpaper's own sound options as the page keeps them */
static void sh_sound_own (WebShared *sh, int sw, double vol)
{
    sh->snd_sw = sw > 0 ? 1 : sw == 0 ? 0 : -1;
    sh->snd_vol_q16 = (int) (CLAMP (isfinite (vol) ? vol : 1.0, 0.0, 1.0) * 65536.0);
}

/* ... read from the configuration (the options "walpe_sound", "walpe_volume"
 * of the wallpaper the page was opened as; neither set: the setting decides,
 * at full volume) */
static void sh_sound_own_load (WebShared *sh)
{
    char *sw = walpe_config_get_prop (sh->origin, "walpe_sound");
    char *vol = walpe_config_get_prop (sh->origin, "walpe_volume");
    char *end = NULL;
    const double v = vol && *vol ? g_ascii_strtod (vol, &end) : 1.0;
    const gboolean yes = sw && (!g_ascii_strcasecmp (sw, "on") || !g_ascii_strcasecmp (sw, "true") ||
                                !strcmp (sw, "1") || !g_ascii_strcasecmp (sw, "yes"));
    const gboolean no = sw && (!g_ascii_strcasecmp (sw, "off") || !g_ascii_strcasecmp (sw, "false") ||
                               !strcmp (sw, "0") || !g_ascii_strcasecmp (sw, "no"));
    sh_sound_own (sh, yes ? 1 : no ? 0 : -1, vol && *vol && end && !*end && v >= 0.0 && v <= 1.0 ? v : 1.0);
    g_free (sw);
    g_free (vol);
}

typedef struct { char *key; int sw; double vol; } SoundOwn;

static gboolean web_sound_page_main (gpointer user)
{
    SoundOwn *o = user;
    for (guint i = 0; sound_pages && i < sound_pages->len; i++) {
        WebShared *sh = sound_pages->pdata[i];
        if (!g_strcmp0 (sh->origin, o->key)) {
            sh_sound_own (sh, o->sw, o->vol);
            sh_sound_apply (sh);
        }
    }
    g_free (o->key);
    g_free (o);
    return G_SOURCE_REMOVE;
}

/* any thread (modules.h web_sound_page): the wallpaper `key` has another
 * switch or volume of its own; -> the pages that show it */
static guint walpe_web_sound_page (const char *key, int sw, double vol)
{
    guint n = 0;
    g_mutex_lock (&reg_lock);
    n = registry && g_hash_table_lookup (registry, key) ? 1 : 0;
    g_mutex_unlock (&reg_lock);
    if (n) {
        SoundOwn *o = g_new0 (SoundOwn, 1);
        o->key = g_strdup (key);
        o->sw = sw;
        o->vol = vol;
        g_idle_add (web_sound_page_main, o);
    }
    return n;
}

/* Paused = no viewer wants frames: the page itself must stop too (a
 * playing <video> or a requestAnimationFrame loop kept the web process at
 * full CPU while nothing was snapshotted). Media elements that were
 * playing are paused and remembered, then the window is unmapped, which
 * makes the page hidden for WebKit (rAF stops, timers throttle). Resume
 * maps it again and restarts exactly those elements.
 * WALPE_WEB_KEEP_RUNNING=1 keeps the old behaviour (page runs paused). */
static gboolean sh_replay_cb (gpointer user)
{
    WebShared *sh = user;
    sh->replay_id = 0;
    sh_input_replay (sh);
    return G_SOURCE_REMOVE;
}

static void sh_suspend (WebShared *sh, gboolean suspend)
{
    if (!sh->win || suspend == sh->suspended)
        return;
    if (suspend && g_getenv ("WALPE_WEB_KEEP_RUNNING"))
        return;
    sh->suspended = suspend;
    if (suspend) {  /* no input while hidden: end hover, release held buttons */
        web_input_cancel (sh->input);
        web_input_leave (sh->input, 0);
    }
    /* (in this order: an Audio object that is started while the view is
     * muted never plays, see "the page's sound") */
    if (!suspend && sh->snd_q16 > 0)
        webkit_web_view_set_is_muted (sh->view, FALSE);
    if (sh->loaded)
        sh_sound_tell (sh, suspend ? "s1" : "s0", 0);
    if (suspend)
        webkit_web_view_set_is_muted (sh->view, TRUE);
    if (suspend) {
        gtk_widget_set_visible (sh->win, FALSE);
    } else {
        gtk_widget_set_visible (sh->win, TRUE);
        hide_window_offscreen (sh->win, sh->view_w, sh->view_h);
        /* the pointer may rest on it: tell the page once GTK has laid the
         * window out again (before that no widget is picked) */
        if (!sh->replay_id)
            sh->replay_id = g_timeout_add (50, sh_replay_cb, sh);
    }
    g_debug ("web: %s %s", suspend ? "suspended" : "resumed", sh->path);
}

static void sh_push_paused (WebShared *sh, gboolean paused)
{
    if (!paused)
        sh_suspend (sh, FALSE);
    if (sh->loaded && paused != sh->js_paused) {
        sh->js_paused = paused;
        sh_eval (sh, paused
            ? "(function(){var L=window.wallpaperPropertyListener;if(L&&L.setPaused)L.setPaused(true);})();"
            : "(function(){var L=window.wallpaperPropertyListener;if(L&&L.setPaused)L.setPaused(false);})();");
    }
    if (paused)
        sh_suspend (sh, TRUE);
}

static void on_load_changed (WebKitWebView *v, WebKitLoadEvent e, gpointer user)
{
    (void) v;
    WebShared *sh = user;
    if (e == WEBKIT_LOAD_FINISHED) {
        /* a link out of the wallpaper was refused: still the same page */
        if (web_input_take_nav_blocked (sh->input))
            return;
        sh->loaded = TRUE;
        sh->js_paused = FALSE;
        sh_push_properties (sh, TRUE);
        if (!sh->first_done) {  /* its first picture: from now it has WEB_FIRST_SETTLE_MS */
            timer_set (&sh->first_id, FALSE, 0, NULL, NULL);
            timer_set (&sh->first_id, TRUE, WEB_FIRST_SETTLE_MS, sh_first_end, sh);
        }
        g_mutex_lock (&sh->lock);
        gboolean active = sh->active > 0 || sh_first_frame_wanted (sh);
        g_mutex_unlock (&sh->lock);
        sh_push_paused (sh, !active);
        if (sh->suspended)   /* paused before the page existed */
            sh_sound_tell (sh, "s1", 0);
        sh_input_replay (sh);   /* the pointer may rest on it */
        {   /* (the limiter script carries the limit the page was created with) */
            char *js = g_strdup_printf ("window.__walpeRaf&&window.__walpeRaf(%d);", sh->fps);
            sh_eval (sh, js);
            g_free (js);
        }
        sh_audio_wake (sh);
        sh_timers_update (sh);
        sh_kick (sh);           /* what it painted while loading was not captured */
        /* (a page can reload itself in a loop: the first loads of a page
         * are always logged, further ones at the page-message rate) */
        if (sh->n_load_logs++ < 5 || web_log_gate (WEB_LOG_LOAD))
            g_message ("web: loaded %s", sh->html);
    }
}

/* alert()/confirm()/prompt()/beforeunload: nobody can answer a dialog on
 * an off-screen wallpaper, and WebKit's default handler shows one and
 * blocks the page's script until it is closed - the page never finished
 * loading (black, no properties). 1271777873 alert()s "WebGL not
 * supported" when WebGL is unavailable. Answer at once (alert = OK,
 * confirm = cancel, prompt = default text). */
static gboolean on_script_dialog (WebKitWebView *v, WebKitScriptDialog *d, gpointer user)
{
    (void) v;
    WebShared *sh = user;
    WebKitScriptDialogType t = webkit_script_dialog_get_dialog_type (d);
    if (t == WEBKIT_SCRIPT_DIALOG_CONFIRM || t == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM)
        webkit_script_dialog_confirm_set_confirmed (d, t == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM);
    else if (t == WEBKIT_SCRIPT_DIALOG_PROMPT)
        webkit_script_dialog_prompt_set_text (d, webkit_script_dialog_prompt_get_default_text (d));
    if (web_log_gate (WEB_LOG_DIALOG)) {
        char *msg = web_log_text (webkit_script_dialog_get_message (d), 200);
        g_message ("web: page dialog answered automatically (%s): %s", sh->path, msg);
        g_free (msg);
    }
    return TRUE;   /* handled; not ref'ed, so it closes when this returns */
}

/* a page whose process keeps dying, for status "web_error" (reg_lock) */
static char *page_error;

static void page_error_set (char *msg /* taken, NULL = clear */)
{
    g_mutex_lock (&reg_lock);
    g_free (page_error);
    page_error = msg;
    g_mutex_unlock (&reg_lock);
}

/* the WebProcess died (a GL/WebGL driver abort in the page's process, the
 * OOM killer, WebKit's memory limit, a WebKit bug): log why and load the
 * page again, whatever the reason - it used to stay a frozen picture that
 * even opening the wallpaper again did not revive. Up to WEB_CRASH_MAX
 * deaths within a minute are retried; the second one in a row with WebGL
 * on is retried without WebGL (pages like 1271777873 then fall back to
 * 2D; a single unrelated crash no longer costs the page its WebGL). After
 * that the page is left alone, status says so ("web_error"), and opening
 * the wallpaper again starts over (sh_reprops_main). */
#define WEB_CRASH_MAX 3

static void on_web_process_terminated (WebKitWebView *v, WebKitWebProcessTerminationReason r,
                                       gpointer user)
{
    WebShared *sh = user;
    sh->loaded = FALSE;
    sh->js_paused = FALSE;
    sh_timers_update (sh);
    if (sh->gave_up && !sh->dead)
        return;     /* stopped on purpose (sh_stop_page): not a crash, no reload */
    WebKitSettings *st = webkit_web_view_get_settings (v);
    gboolean webgl = webkit_settings_get_enable_webgl (st);
    const char *why = r == WEBKIT_WEB_PROCESS_CRASHED ? "crashed"
                      : r == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "exceeded its memory limit"
                      : "was terminated";
    gint64 now = g_get_monotonic_time ();
    if (now - sh->crash_us > 60 * G_USEC_PER_SEC)
        sh->crashes = 0;
    sh->crash_us = now;
    sh->crashes++;
    if (sh->dead) {
        g_warning ("web: page process %s (%s)", why, sh->path);
        return;
    }
    if (sh->crashes > WEB_CRASH_MAX) {
        sh->gave_up = TRUE;
        g_warning ("web: page process %s (%s) %d times within a minute: not reloading it "
                   "again (open the wallpaper again to retry)", why, sh->path, sh->crashes);
        /* (WALPE_WEB_PROBED: set by the daemon when pages run with
         * hardware acceleration; its start-up test passed, a real page
         * may still be too much for the driver) */
        page_error_set (g_strdup_printf ("the page of %s keeps crashing (its process %s %d times "
                                         "in a row); open the wallpaper again to retry%s",
                                         sh->path, why, sh->crashes,
                                         g_getenv ("WALPE_WEB_PROBED")
                                             ? ". Web hardware acceleration is on: if this began "
                                               "with it, switch it off (walpe-ctl set web_accel=off)"
                                             : ""));
        return;
    }
    gboolean drop_webgl = r == WEBKIT_WEB_PROCESS_CRASHED && webgl && sh->crashes == 2;
    g_warning ("web: page process %s (%s); reloading it%s (attempt %d of %d)", why, sh->path,
               drop_webgl ? " without WebGL" : "", sh->crashes, WEB_CRASH_MAX);
    if (drop_webgl) {
        webkit_settings_set_enable_webgl (st, FALSE);
        sh->webgl_off = TRUE;
    }
    webkit_web_view_reload (v);
}

/* ----------------------------------------------------------- snapshots */

/* all: the paused monitors too (sh_first_end) */
static void sh_notify_viewers (WebShared *sh, gboolean all)
{
    WebViewerRef refs[8];
    guint n = 0;
    g_mutex_lock (&sh->lock);
    for (guint i = 0; i < sh->viewers->len && n < G_N_ELEMENTS (refs); i++)
        refs[n++] = g_array_index (sh->viewers, WebViewerRef, i);
    /* the page's first picture: paused monitors paint it too, every frame
     * of it (web_gl_render keeps first_frame_pending up for that long) */
    gboolean first = all || !sh->first_done;
    g_mutex_unlock (&sh->lock);
    for (guint i = 0; i < n; i++)
        if (refs[i].cb && (!refs[i].paused || first))
            refs[i].cb (refs[i].user);
}

/* WALPE_WEB_TIMING=1: every 2 s, what the frame path cost. All "ms CPU"
 * figures are CPU time of the main thread (CLOCK_THREAD_CPUTIME_ID), not
 * wall-clock: they stay meaningful on a busy machine. upload is wall time
 * on the render threads (they add atomically; glFinish makes it visible).
 *   web timing: main thread N ms CPU/s | page repaints/s | GTK frames/s x
 *   ms | captures/s x ms (copied bytes, allocations) | uploads x ms (bytes) */
/* (-1 = not read yet. The main thread reads the switch, the render threads
 * look at the result for their upload times: common/lazy.h) */
static WalpeLazy web_timing = WALPE_LAZY_UNSET;
static struct {
    guint frames, idle_ticks, invalidates, gtk_frames, allocs, same;
    gint64 grab_cpu, dl_cpu, diff_cpu, gtk_cpu, fc_cpu0, main_cpu0, t0;
    guint64 copied, alloc_bytes;
    gboolean fc_open;
} wt;                         /* main thread */
static struct {
    gint uploads, up_us;
    gint up_kb;               /* kB handed to glTexSubImage2D */
} wtu;                        /* atomic: the render threads add */

static gboolean timing_on (void)
{
    int on = walpe_lazy_get (&web_timing);
    if (on < 0)
        on = walpe_lazy_set (&web_timing, g_getenv ("WALPE_WEB_TIMING") != NULL);
    return on;
}

/* CPU time of the calling thread, us */
static gint64 web_cpu_us (void)
{
    struct timespec ts;
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return (gint64) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void timing_report (void)
{
    gint64 now = g_get_monotonic_time ();
    if (!wt.t0) {
        wt.t0 = now;
        wt.main_cpu0 = web_cpu_us ();
    }
    if (now - wt.t0 < 2 * G_USEC_PER_SEC)
        return;
    double sec = (now - wt.t0) / 1e6;
    gint64 mc = web_cpu_us ();
    guint ups = (guint) g_atomic_int_and (&wtu.uploads, 0);
    gint upus = g_atomic_int_and (&wtu.up_us, 0);
    gint upkb = g_atomic_int_and (&wtu.up_kb, 0);
    guint f = wt.frames ? wt.frames : 1;
    g_message ("web timing: main thread %.0f ms CPU/s | page repaints %.1f/s | GTK frames %.1f/s x %.2f ms | "
               "captures %.1f/s x %.2f ms (grab %.2f + copy %.2f + diff %.2f; %.0f kB copied, %.2f allocs "
               "of %.0f kB per capture; %u unchanged) | uploads %u x %.2f ms (%.0f kB each) | %u idle ticks",
               (mc - wt.main_cpu0) / 1e3 / sec, wt.invalidates / sec, wt.gtk_frames / sec,
               wt.gtk_frames ? wt.gtk_cpu / 1e3 / wt.gtk_frames : 0.0, wt.frames / sec,
               (wt.grab_cpu + wt.dl_cpu + wt.diff_cpu) / 1e3 / f, wt.grab_cpu / 1e3 / f, wt.dl_cpu / 1e3 / f,
               wt.diff_cpu / 1e3 / f, wt.copied / 1e3 / f, (double) wt.allocs / f,
               wt.allocs ? wt.alloc_bytes / 1e3 / wt.allocs : 0.0, wt.same,
               ups, ups ? upus / 1e3 / ups : 0.0, ups ? (double) upkb / ups : 0.0, wt.idle_ticks);
    memset (&wt, 0, sizeof wt);
    wt.t0 = now;
    wt.main_cpu0 = mc;
}

/* the cost of GTK's own frame for the page's (off-screen) window */
static void timing_fc_before (GdkFrameClock *fc, gpointer user)
{
    (void) fc; (void) user;
    wt.fc_cpu0 = web_cpu_us ();
    wt.fc_open = TRUE;
}

static void timing_fc_after (GdkFrameClock *fc, gpointer user)
{
    (void) fc; (void) user;
    if (!wt.fc_open)
        return;
    wt.fc_open = FALSE;
    wt.gtk_frames++;
    wt.gtk_cpu += web_cpu_us () - wt.fc_cpu0;
}

static gboolean web_bigwin (void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = g_getenv ("WALPE_WEB_BIGWIN");
        v = e && *e && strcmp (e, "0");
    }
    return v;
}

/* WALPE_WEB_SNAPSHOT=1 = a WebKit snapshot per frame (the path of versions
 * 0.1 - 0.10: the WebProcess paints the whole page again into a new bitmap for
 * every frame and sends it over IPC) */
static gboolean use_snapshots (void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = g_getenv ("WALPE_WEB_SNAPSHOT");
        v = e && *e && strcmp (e, "0");
    }
    return v;
}

/* a buffer to draw the next frame into: one nobody else holds. Two are
 * kept besides the front frame; a third only exists while a render thread
 * is slow enough to still upload the frame before the front one, and is
 * dropped again when that stops. */
static WebBuf *sh_buf_take (WebShared *sh, int w, int h)
{
    WebBuf *b = NULL;
    for (int i = 0; i < 2 && !b; i++) {
        WebBuf *c = sh->spare[i];
        if (c && c->w == w && c->h == h && g_atomic_int_get (&c->refs) == 1) {
            b = c;
            sh->spare[i] = NULL;
        }
    }
    if (b && sh->spare[1] && g_atomic_int_get (&sh->spare[1]->refs) == 1) {
        if (++sh->spare_idle > 240) {    /* not needed for a while: 4 x w x h bytes back */
            webbuf_unref (sh->spare[1]);
            sh->spare[1] = NULL;
            sh->spare_idle = 0;
        }
    } else {
        sh->spare_idle = 0;
    }
    if (!b) {
        b = webbuf_new (w, h);
        if (timing_on ()) {
            wt.allocs++;
            wt.alloc_bytes += b->cap;
        }
    }
    if (!b->surf)
        b->surf = cairo_image_surface_create_for_data (b->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    return b;
}

static void sh_buf_put (WebShared *sh, WebBuf *b)
{
    if (!b)
        return;
    for (int i = 0; i < 2; i++)
        if (!sh->spare[i]) {
            sh->spare[i] = b;
            return;
        }
    /* both taken: replace one that is of no use (still being uploaded
     * from, or of an old size) */
    for (int i = 0; i < 2; i++) {
        WebBuf *c = sh->spare[i];
        if (g_atomic_int_get (&c->refs) > 1 || c->w != b->w || c->h != b->h) {
            webbuf_unref (c);
            sh->spare[i] = b;
            return;
        }
    }
    webbuf_unref (b);
}

/* What did this frame change? (webdamage.h) FALSE = the frames are
 * identical: a page that repaints the same picture (a visualizer in
 * silence, a canvas cleared and redrawn) costs no upload and no redraw at
 * all. */
static gboolean frame_damage (const WebBuf *n, const WebBuf *o, WebDmg *d)
{
    d->x0 = 0;
    d->y0 = 0;
    d->x1 = n->w;
    d->y1 = n->h;
    if (!o || o->w != n->w || o->h != n->h)
        return TRUE;
    WalpeWebDamage r;
    if (!walpe_web_frame_damage (n->data, o->data, n->w, n->h, &r))
        return FALSE;
    d->x0 = r.x0;
    d->y0 = r.y0;
    d->x1 = r.x1;
    d->y1 = r.y1;
    return TRUE;
}

/* main thread: frame b (drawn into a spare buffer) becomes the front frame
 * the render threads upload - unless it shows what the front frame shows */
static void sh_publish (WebShared *sh, WebBuf *b)
{
    gint64 c0 = timing_on () ? web_cpu_us () : 0;
    WebDmg d;
    gboolean changed = frame_damage (b, sh->px, &d);
    if (c0)
        wt.diff_cpu += web_cpu_us () - c0;
    sh->snapshots++;
    if (!changed) {
        if (c0)
            wt.same++;
        sh_buf_put (sh, b);
        return;
    }
    gint64 nowc = g_get_monotonic_time ();
    sh->change_iv_us = sh->change_us ? MIN (nowc - sh->change_us, WEB_AUDIO_SETTLE_MAX_US / 2) : 0;
    sh->change_us = nowc;
    if (sh->audio_idle && audio_silence_mode () == 0)
        sh_audio_wake (sh);     /* the picture moves again: the listener at its full rate */
    g_mutex_lock (&sh->lock);
    WebBuf *old = sh->px;
    sh->px = b;
    sh->pw = b->w;
    sh->ph = b->h;
    sh->seq++;
    d.seq = sh->seq;
    sh->dmg[sh->seq % WEB_DMG_N] = d;
    g_mutex_unlock (&sh->lock);
    sh_buf_put (sh, old);
    sh_notify_viewers (sh, FALSE);
}

/* WALPE_WEB_SNAPSHOT: a WebKit snapshot -> a frame */
static void sh_take_texture (WebShared *sh, GdkTexture *tex)
{
    gint64 c0 = timing_on () ? web_cpu_us () : 0;
    int w = gdk_texture_get_width (tex), h = gdk_texture_get_height (tex);
    if (w < 1 || h < 1)
        return;
    WebBuf *b = sh_buf_take (sh, w, h);
    /* GL_BGRA + premultiplied: uploads as-is, drawn with premult=TRUE */
    GdkTextureDownloader *dl = gdk_texture_downloader_new (tex);
    gdk_texture_downloader_set_format (dl, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED);
    gdk_texture_downloader_download_into (dl, b->data, (gsize) w * 4);
    gdk_texture_downloader_free (dl);
    if (c0) {
        wt.frames++;
        wt.dl_cpu += web_cpu_us () - c0;
        wt.copied += b->cap;
    }
    sh_publish (sh, b);
}

static void snapshot_cb (GObject *src, GAsyncResult *res, gpointer user)
{
    WebShared *sh = user;
    GError *err = NULL;
    GdkTexture *tex = webkit_web_view_get_snapshot_finish (WEBKIT_WEB_VIEW (src), res, &err);
    sh->in_flight = FALSE;
    if (sh->dead) {
        g_clear_object (&tex);
        g_clear_error (&err);
        sh_free (sh);
        return;
    }
    if (!tex) {
        static int nerr;
        if (err && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) && nerr++ < 5)
            g_debug ("web: snapshot failed: %s", err->message);
        g_clear_error (&err);
        return;
    }
    sh_take_texture (sh, tex);
    g_object_unref (tex);
}

static gboolean sh_snapshot_tick (gpointer user)
{
    WebShared *sh = user;
    if (!sh->in_flight && sh->loaded) {
        sh->in_flight = TRUE;
        webkit_web_view_get_snapshot (sh->view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                      WEBKIT_SNAPSHOT_OPTIONS_TRANSPARENT_BACKGROUND,
                                      sh->cancel, snapshot_cb, sh);
    }
    if (timing_on ())
        timing_report ();
    return G_SOURCE_CONTINUE;
}

/* Capture = copy the view's current frame. WebKit paints the page into
 * its backing store anyway (the off-screen window is mapped, so the page
 * is visible and animating) and says when it did: the view asks GTK for a
 * redraw, GTK runs a frame for the window, WalpeWebHost is asked to paint
 * (sh_on_paint) and takes the view's render node - WebKit's backing store
 * in software mode, a texture with hardware acceleration. The node is
 * drawn straight into a frame buffer: one pass over the pixels on this
 * thread, no allocation. (Version 0.12: the node went through a cairo
 * renderer into a new image, that was copied into the frame buffer, and
 * GTK painted the window besides: three to four passes and two
 * allocations of the frame's size per repaint. Versions 0.1 - 0.10: a WebKit
 * snapshot per frame.) A page that does not repaint costs nothing: no
 * GTK frame, no timer. */
static void sh_capture_node (WebShared *sh, GskRenderNode *node)
{
    gint64 c0 = timing_on () ? web_cpu_us () : 0;
    int w = sh->view_w, h = sh->view_h;
    if (w < 1 || h < 1)
        return;
    WebBuf *b = sh_buf_take (sh, w, h);
    GskRenderNodeType nt = gsk_render_node_get_node_type (node);
    graphene_rect_t nb;
    gsk_render_node_get_bounds (node, &nb);
    gboolean covers = nb.origin.x <= 0 && nb.origin.y <= 0 && nb.origin.x + nb.size.width >= w &&
                      nb.origin.y + nb.size.height >= h;
    GdkTexture *tex = nt == GSK_TEXTURE_NODE ? gsk_texture_node_get_texture (node) : NULL;
    if (tex && covers && nb.origin.x == 0 && nb.origin.y == 0 && gdk_texture_get_width (tex) == w &&
        gdk_texture_get_height (tex) == h) {
        /* hardware acceleration: the frame is a texture (DMA-BUF or GL):
         * read it back in the format the render threads upload */
        GdkTextureDownloader *dl = gdk_texture_downloader_new (tex);
        gdk_texture_downloader_set_format (dl, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED);
        gdk_texture_downloader_download_into (dl, b->data, (gsize) w * 4);
        gdk_texture_downloader_free (dl);
    } else {
        cairo_t *cr = cairo_create (b->surf);
        if (nt == GSK_CAIRO_NODE && covers) {
            /* one opaque-or-not picture covering everything: "source"
             * lets cairo replay WebKit's drawing straight into the buffer
             * (anything else goes through a temporary image) */
            cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
        } else {
            cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
            cairo_paint (cr);
            cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
        }
        gsk_render_node_draw (node, cr);
        cairo_destroy (cr);
        cairo_surface_flush (b->surf);
    }
    if (c0) {
        wt.frames++;
        wt.grab_cpu += web_cpu_us () - c0;
        wt.copied += b->cap;
    }
    sh_publish (sh, b);
}

static gboolean sh_capture_due (gpointer user)
{
    WebShared *sh = user;
    sh->cap_id = 0;
    sh_kick (sh);
    return G_SOURCE_REMOVE;
}

/* ask GTK for a frame of the page's window: sh_on_paint runs in it */
static void sh_kick (WebShared *sh)
{
    if (sh->box && !sh->dead && !sh->suspended)
        gtk_widget_queue_draw (sh->box);
}

/* GTK paints the page's window: the view repainted (or sh_kick asked).
 * Frames beyond the fps limit are not taken: the view is left unpainted
 * ("dirty" for GTK, so further repaints of the page start no GTK frame
 * and WebKit's backing store is not snapshotted in between) and painted
 * when the next capture is due. */
static void sh_on_paint (WalpeWebHost *h, GtkSnapshot *snapshot, gpointer user)
{
    WebShared *sh = user;
    if (timing_on ())
        wt.invalidates++;
    gboolean can = !sh->dead && sh->loaded && sh->active_main && !use_snapshots ();
    gint64 now = g_get_monotonic_time ();
    /* (1 ms: a repaint that is a hair early is this period's frame) */
    if (can && now < sh->cap_due_us - 1000) {
        if (!sh->cap_id) {
            guint ms = (guint) ((sh->cap_due_us - now + 999) / 1000);
            sh->cap_id = g_timeout_add (MAX (ms, 1), sh_capture_due, sh);
        }
        if (!h->big)
            return;
        can = FALSE;
    }
    GskRenderNode *node = walpe_web_host_child_node (h);
    if (!node)
        return;
    if (h->big)     /* WALPE_WEB_BIGWIN: GTK paints the page as well */
        gtk_snapshot_append_node (snapshot, node);
    if (can) {
        /* the limit as an average rate: periods are counted from when a
         * frame was due, not from when it came (unless that is long ago) */
        gint64 iv = G_USEC_PER_SEC / MAX (sh->fps, 1);
        sh->cap_due_us = now - sh->cap_due_us > iv ? now + iv : sh->cap_due_us + iv;
        sh_capture_node (sh, node);
    }
    gsk_render_node_unref (node);
    if (timing_on ())
        timing_report ();
}

/* main thread: capture only while the page listens AND is visible */
static void sh_audio_update (WebShared *sh, gboolean active)
{
    gboolean want = sh->audio_wanted && active && !sh->dead;
    if (want == sh->audio_ref)
        return;
    sh->audio_ref = want;
    if (want)
        walpe_audio_ref ();
    else
        walpe_audio_unref ();
}

static void audio_eval_done (GObject *src, GAsyncResult *res, gpointer user)
{
    WebGate *g = user;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (src), res, NULL);
    if (v)
        g_object_unref (v);
    g->busy = FALSE;
    webgate_unref (g);
}

/* WE web audio array: 64 left + 64 right, 0..~1 (three decimals, written
 * by hand: 128 printf-style conversions per call were most of this
 * function) */
static void sh_push_audio (WebShared *sh, const WalpeAudioSpectrum *a)
{
    char js[64 + 128 * 8], *p = js;
    static const char head[] = "window.__walpeAudio&&window.__walpeAudio([";
    memcpy (p, head, sizeof head - 1);
    p += sizeof head - 1;
    for (int i = 0; i < 128; i++) {
        float v = i < 64 ? a->left[i] : a->right[i - 64];
        guint m = !(v > 0) ? 0 : v >= 99.999f ? 99999 : (guint) (v * 1000.0f + 0.5f);
        guint ip = m / 1000, fp = m % 1000;
        if (ip >= 10)
            *p++ = (char) ('0' + ip / 10);
        *p++ = (char) ('0' + ip % 10);
        if (fp) {
            *p++ = '.';
            *p++ = (char) ('0' + fp / 100);
            *p++ = (char) ('0' + fp / 10 % 10);
            *p++ = (char) ('0' + fp % 10);
        }
        if (i < 127)
            *p++ = ',';
    }
    memcpy (p, "]);", 4);
    if (sh->view && !sh->dead) {
        if (!sh->audio_gate) {
            sh->audio_gate = g_new0 (WebGate, 1);
            sh->audio_gate->ref = 1;
        }
        sh->audio_gate->busy = TRUE;
        sh->audio_gate->ref++;
        webkit_web_view_evaluate_javascript (sh->view, js, -1, NULL, NULL, NULL,
                                             audio_eval_done, sh->audio_gate);
    }
}

/* WALPE_WEB_AUDIO_SILENCE: "always" = stop calling the listener after
 * WEB_AUDIO_SETTLE_US of silence whatever the page does (no call at all
 * until there is sound), "off" = never slow down (version 0.12), unset = once
 * a second once the picture stands still */
static int audio_silence_mode (void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = g_getenv ("WALPE_WEB_AUDIO_SILENCE");
        v = !e || !*e ? 0 : !strcmp (e, "always") ? 1 : !strcmp (e, "off") || !strcmp (e, "0") ? 2 : 0;
    }
    return v;
}

/* the page was told something a listener may draw with (new property
 * values, a click, it is shown again): call it again for a while even in
 * silence */
static void sh_audio_wake (WebShared *sh)
{
    sh->audio_wake_us = g_get_monotonic_time ();
    if (sh->audio_idle) {
        sh->audio_idle = FALSE;
        sh_timers_update (sh);
    }
}

/* main thread, WEB_AUDIO_HZ timer: hand the page the spectrum at WE's
 * rate, not at the capture rate. Version 0.12 raised the tick from 30 Hz to
 * the fps setting (up to 144) and the listener was called on every tick:
 * a page that draws its bars in the listener spent all its time there
 * (measured with a 14 ms listener: 68 calls/s, requestAnimationFrame at
 * 1-4/s, the wallpaper at 0-3 fps; at 30 calls/s: 27 fps). A push is also
 * skipped while the page has not run the previous one: calls must not
 * queue up behind a page that is slower than the rate.
 * Silence (no sound, or nothing can be captured): the listener still got
 * an all-zero array at the full rate, and a visualizer redrew its canvas
 * for it for ever. Now it is called until the picture has stood still for
 * WEB_AUDIO_SETTLE_US (longer for a picture that changed slowly) -
 * falling bars finish, and a page that animates on its own, or inside
 * the listener, keeps being called as before - and
 * then once a second (WEB_AUDIO_KEEPALIVE_US: a clock drawn in the
 * listener keeps going) until there is sound, the picture changes again
 * (sh_publish) or the page is told something else (sh_audio_wake).
 * Meanwhile the timer only looks for sound. */
static void sh_audio_tick (WebShared *sh)
{
    gint64 now = g_get_monotonic_time ();
    WalpeAudioSpectrum a;
    walpe_audio_get (&a);
    gboolean quiet = TRUE;
    for (int i = 0; i < 64 && quiet; i++)
        quiet = a.left[i] < 0.0005f && a.right[i] < 0.0005f;
    if (!quiet) {
        sh->audio_quiet_us = 0;
        if (sh->audio_idle) {
            sh->audio_idle = FALSE;
            sh->audio_due_us = 0;
            sh_timers_update (sh);
        }
    } else {
        if (!sh->audio_quiet_us)
            sh->audio_quiet_us = now;
        int mode = audio_silence_mode ();
        gint64 still = MAX (WEB_AUDIO_SETTLE_US, 2 * sh->change_iv_us);
        if (!sh->audio_idle && mode != 2 && now - sh->audio_quiet_us > WEB_AUDIO_SETTLE_US &&
            now - sh->audio_wake_us > WEB_AUDIO_SETTLE_US &&
            (mode == 1 || now - sh->change_us > still)) {
            sh->audio_idle = TRUE;
            g_debug ("web: silence and a still picture: audio listener called once a second until there is sound");
            sh_timers_update (sh);
        }
        if (sh->audio_idle) {
            if (mode == 1 || now - sh->audio_sent_us < WEB_AUDIO_KEEPALIVE_US)
                return;
            sh->audio_due_us = 0;
        }
    }
    if (now < sh->audio_due_us - 2000)      /* (2 ms: the timer is not exact) */
        return;
    /* (a result that never arrives - page reloaded, process gone - must
     * not silence the listener: give up waiting after a second) */
    if (sh->audio_gate && sh->audio_gate->busy && now - sh->audio_sent_us < G_USEC_PER_SEC)
        return;
    const gint64 iv = G_USEC_PER_SEC / WEB_AUDIO_HZ;
    /* ticks land up to one tick late: keep the average rate, never burst */
    sh->audio_due_us = sh->audio_due_us + iv > now ? sh->audio_due_us + iv : now + iv;
    sh->audio_sent_us = now;
    sh_push_audio (sh, &a);
}

static gboolean sh_audio_timer (gpointer user)
{
    sh_audio_tick (user);
    return G_SOURCE_CONTINUE;
}

/* main thread: media integration only while a listener is registered
 * AND the page is visible */
static void sh_media_update (WebShared *sh, gboolean active)
{
    gboolean want = sh->media_wanted && active && !sh->dead;
    if (want == sh->media_ref)
        return;
    sh->media_ref = want;
    if (want) {
        sh->media_first = TRUE;
        walpe_media_ref ();
    } else {
        walpe_media_unref ();
    }
}

static void css_hex (GString *js, const char *name, const float c[3])
{
    g_string_append_printf (js, "\"%s\":\"#%02X%02X%02X\",", name,
                            (int) lroundf (CLAMP (c[0], 0, 1) * 255),
                            (int) lroundf (CLAMP (c[1], 0, 1) * 255),
                            (int) lroundf (CLAMP (c[2], 0, 1) * 255));
}

static void json_member (GString *js, const char *name, const char *v)
{
    JsonNode *n = json_node_new (JSON_NODE_VALUE);
    json_node_set_string (n, v ? v : "");
    char *q = json_to_string (n, FALSE);
    json_node_unref (n);
    g_string_append_printf (js, "\"%s\":%s,", name, q);
    g_free (q);
}

static void media_push (WebShared *sh, const char *kind, GString *body)
{
    if (body->len && body->str[body->len - 1] == ',')
        g_string_truncate (body, body->len - 1);
    char *js = g_strdup_printf ("window.__walpeMedia&&window.__walpeMedia('%s',{%s});", kind,
                                body->str);
    sh_eval (sh, js);
    g_free (js);
    g_string_truncate (body, 0);
}

/* WE web media events: thumbnail = data URL ("" = none), colours as CSS
 * hex, state 0/1/2 (window.wallpaperMediaIntegration.PLAYBACK_*) */
static void sh_push_media (WebShared *sh)
{
    guint64 ser = walpe_media_serial ();
    if (ser == sh->media_serial && !sh->media_first)
        return;
    WalpeNowPlaying m;
    walpe_media_get (&m);
    if (sh->media_first && !m.enabled && sh->media_first++ < 30) {
        walpe_media_now_clear (&m);   /* give the media thread time to connect */
        return;
    }
    gboolean first = sh->media_first != 0;
    sh->media_first = FALSE;
    sh->media_serial = ser;
    guint cur[5] = { m.status_serial, m.props_serial, m.thumb_serial, m.playback_serial,
                     m.timeline_serial };
    gboolean ch[5];
    for (int k = 0; k < 5; k++)
        ch[k] = first || cur[k] != sh->media_seen[k];
    memcpy (sh->media_seen, cur, sizeof (cur));
    gboolean playing = m.player && *m.player;
    GString *b = g_string_new (NULL);
    if (ch[0]) {
        g_string_append_printf (b, "\"enabled\":%s", m.enabled ? "true" : "false");
        media_push (sh, "status", b);
    }
    if (ch[3]) {
        g_string_append_printf (b, "\"state\":%d", m.state);
        media_push (sh, "playback", b);
    }
    if (ch[1] && (!first || playing)) {
        json_member (b, "title", m.title);
        json_member (b, "artist", m.artist);
        json_member (b, "subTitle", m.subtitle);
        json_member (b, "albumTitle", m.album_title);
        json_member (b, "albumArtist", m.album_artist);
        json_member (b, "genres", m.genres);
        json_member (b, "contentType", m.content_type);
        media_push (sh, "properties", b);
    }
    if (ch[2] && (!first || m.has_thumbnail)) {
        json_member (b, "thumbnail", m.has_thumbnail ? m.thumb_data_url : "");
        css_hex (b, "primaryColor", m.primary);
        css_hex (b, "secondaryColor", m.secondary);
        css_hex (b, "tertiaryColor", m.tertiary);
        css_hex (b, "textColor", m.text);
        css_hex (b, "highContrastColor", m.high_contrast);
        media_push (sh, "thumbnail", b);
    }
    if (ch[4] && (!first || playing)) {
        char p1[G_ASCII_DTOSTR_BUF_SIZE], p2[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd (p1, sizeof (p1), "%.3f", m.position);
        g_ascii_formatd (p2, sizeof (p2), "%.3f", m.duration);
        g_string_append_printf (b, "\"position\":%s,\"duration\":%s", p1, p2);
        media_push (sh, "timeline", b);
    }
    g_string_free (b, TRUE);
    /* (once per listener registration, which a page can repeat at will) */
    if (first && (sh->n_media_logs++ < 5 || web_log_gate (WEB_LOG_LOAD)))
        g_message ("web: media integration state pushed (%s)", playing ? m.player : "no player");
    walpe_media_now_clear (&m);
}

/* The message handler is the one thing a page can call in this process.
 * Everything WalPE's own scripts post is a few bytes (the console lines of
 * WALPE_WEB_CONSOLE: a line of text) and rare. A page can post anything,
 * as fast as it likes, and every message lands in the daemon as a value
 * of JavaScriptCore's heap here, freed only when that heap is collected:
 * a page posting 8 MB strings took the daemon from 200 MB to 2.3 GB in
 * eight seconds and its replies to walpe-ctl to two seconds each. A
 * message longer than WEB_MSG_MAX is not read at all; a page that sent
 * WEB_MSG_BIG_TOTAL of them, or more than WEB_MSG_RATE_N messages within
 * WEB_MSG_RATE_US, is stopped: its process is ended and not started
 * again until the wallpaper is opened again (status: "web_error"). */
#define WEB_MSG_MAX (64 * 1024)
#define WEB_MSG_BIG_TOTAL (8 * 1024 * 1024)
#define WEB_MSG_RATE_N 20000
#define WEB_MSG_RATE_US (10 * G_USEC_PER_SEC)

static void sh_stop_page (WebShared *sh, const char *why)
{
    if (sh->gave_up || sh->dead || !sh->view)
        return;
    sh->gave_up = TRUE;     /* on_web_process_terminated: no reload */
    g_warning ("web: the page of %s %s: it is stopped (open the wallpaper again to retry)",
               sh->path, why);
    page_error_set (g_strdup_printf ("the page of %s was stopped: it %s; open the wallpaper "
                                     "again to retry", sh->path, why));
    webkit_web_view_terminate_web_process (sh->view);
}

static void on_script_message (WebKitUserContentManager *m, JSCValue *v, gpointer user)
{
    (void) m;
    WebShared *sh = user;
    if (sh->gave_up)
        return;     /* stopped: what it had still queued is dropped unread */
    gint64 mnow = g_get_monotonic_time ();
    if (mnow - sh->msg_win_us > WEB_MSG_RATE_US) {
        sh->msg_win_us = mnow;
        sh->msg_n = 0;
    }
    if (++sh->msg_n > WEB_MSG_RATE_N) {
        sh_stop_page (sh, "sent the daemon more than 20000 messages within ten seconds");
        return;
    }
    if (jsc_value_is_string (v)) {
        /* (the length without copying the text out of the script heap) */
        JSCValue *lv = jsc_value_object_get_property (v, "length");
        gint32 len = lv ? jsc_value_to_int32 (lv) : 0;
        g_clear_object (&lv);
        if (len > WEB_MSG_MAX) {
            sh->msg_big += (guint64) len;
            if (sh->msg_big > WEB_MSG_BIG_TOTAL)
                sh_stop_page (sh, "sent megabytes of data to the daemon's message handler");
            return;
        }
    }
    char *msg = jsc_value_is_string (v) ? jsc_value_to_string (v) : NULL;
    if (msg && g_str_has_prefix (msg, "raf:")) {
        /* the answer of the WALPE_WEB_TIMING script (sh_create_real). Any
         * page can post it: without the switch it is not logged at all (a
         * page posting it in a loop wrote log lines without limit), with
         * it at most one a second (the script posts every two). */
        static gint64 raf_logged_us;
        gint64 now = g_get_monotonic_time ();
        if (timing_on () && now - raf_logged_us >= G_USEC_PER_SEC) {
            raf_logged_us = now;
            double raf = 0, aud = 0, audms = 0;
            char *e = NULL;
            raf = g_ascii_strtod (msg + 4, &e);
            aud = e ? g_ascii_strtod (e, &e) : 0;
            audms = e ? g_ascii_strtod (e, NULL) : 0;
            g_message ("web timing: page requestAnimationFrame %.1f/s, audio listener %.1f calls/s x %.2f ms",
                       raf, aud, audms);
        }
        g_free (msg);
        return;
    }
    if (msg && g_str_has_prefix (msg, "probe:")) {
        web_probe_log (sh, "ack", msg + 6);   /* WALPE_PROP_PROBE */
        g_free (msg);
        return;
    }
    if (msg && g_str_has_prefix (msg, "log:")) {
        if (g_getenv ("WALPE_WEB_CONSOLE")) {
            /* (one line each: a page's text with line breaks in it could
             * write lines that look like the daemon's own) */
            char *t = web_log_text (msg + 4, 2000);
            g_message ("web console: %s", t);
            g_free (t);
        }
    } else if (msg && !strcmp (msg, "input")) {
        web_input_ack (sh->input);   /* a trusted pointer event arrived */
    } else if (msg && !strcmp (msg, "audio") && !sh->audio_wanted) {
        sh->audio_wanted = TRUE;
        g_message ("web: page registered an audio listener (%s)", sh->path);
        g_mutex_lock (&sh->lock);
        gboolean active = sh->active > 0;
        g_mutex_unlock (&sh->lock);
        sh_audio_update (sh, active);
        sh_audio_wake (sh);
        sh_timers_update (sh);
    } else if (msg && !strcmp (msg, "media")) {
        /* a listener (re-)registered: it gets the current state */
        if (!sh->media_wanted)
            g_message ("web: page registered a media listener (%s)", sh->path);
        sh->media_wanted = TRUE;
        sh->media_first = TRUE;
        g_mutex_lock (&sh->lock);
        gboolean active = sh->active > 0;
        g_mutex_unlock (&sh->lock);
        sh_media_update (sh, active);
        sh_timers_update (sh);
    }
    g_free (msg);
}

/* the WE web API surface pages expect before their own scripts run */
static const char *WE_API_JS =
    "(function(){\n"
    " var noop=function(){};\n"
    " window.__walpeAudioCb=null;\n"
    " window.__walpeAudio=function(a){var c=window.__walpeAudioCb;if(c){try{c(a);}catch(e){}}};\n"
    " window.wallpaperRegisterAudioListener=function(cb){window.__walpeAudioCb=cb;\n"
    "  try{window.webkit.messageHandlers.walpe.postMessage('audio');}catch(e){}};\n"
    /* media integration: one listener per kind (the last registration
     * wins); registering asks the daemon for the current state */
    /* (both spellings of Wallpaper Engine's documentation: its list of
     * the states names PLAYBACK_PLAYING ..., its example compares with
     * wallpaperMediaIntegration.playback.PLAYING - a page written after
     * the example threw a TypeError in its playback listener here) */
    " window.wallpaperMediaIntegration={PLAYBACK_STOPPED:0,PLAYBACK_PLAYING:1,PLAYBACK_PAUSED:2,\n"
    "  playback:{STOPPED:0,PLAYING:1,PAUSED:2}};\n"
    " var mcb={};\n"
    " window.__walpeMedia=function(k,e){var c=mcb[k];if(c){try{c(e);}catch(x){console.error(x);}}};\n"
    " [['Status','status'],['Properties','properties'],['Thumbnail','thumbnail'],['Playback','playback'],\n"
    "  ['Timeline','timeline']].forEach(function(p){window['wallpaperRegisterMedia'+p[0]+'Listener']=function(cb){\n"
    "  mcb[p[1]]=cb;try{window.webkit.messageHandlers.walpe.postMessage('media');}catch(e){}};});\n"
    /* 'file:///' + property value (WE pages run from file://): mapped to
     * the loopback server's file route on src/href/style/url() writes */
    " var fb=null;function fbase(){if(fb!==null)return fb;var m=location.pathname.match(/^\\/[0-9a-f]{32}\\//);\n"
    "  fb=(location.protocol.indexOf('http')===0&&m)?location.origin+m[0]+'__walpe_file':'';return fb;}\n"
    " function fix(u){if(typeof u!=='string'||u.indexOf('file:')<0)return u;var b=fbase();if(!b)return u;\n"
    "  return u.replace(/file:\\/\\/(?:localhost)?\\/*([^'\"\\)\\s]*)/gi,function(m,p){try{p=decodeURI(p);}catch(e){}\n"
    "   return b+encodeURI('/'+p).replace(/\\?/g,'%3F').replace(/#/g,'%23');});}\n"
    " window.__walpeFixUrl=fix;\n"
    " function hook(proto,prop){if(!proto)return;var d=Object.getOwnPropertyDescriptor(proto,prop);\n"
    "  if(!d||!d.set||!d.configurable)return;Object.defineProperty(proto,prop,{configurable:true,enumerable:d.enumerable,\n"
    "   get:d.get,set:function(v){d.set.call(this,fix(v));}});}\n"
    " [[window.HTMLImageElement,'src'],[window.HTMLMediaElement,'src'],[window.HTMLSourceElement,'src'],\n"
    "  [window.HTMLVideoElement,'poster'],[window.HTMLLinkElement,'href'],[window.HTMLElement,'style']].forEach(function(h){\n"
    "  if(h[0])hook(h[0].prototype,h[1]);});\n"
    " [window.CSSStyleDeclaration,window.CSSStyleProperties,window.CSS2Properties].forEach(function(C){if(!C)return;var P=C.prototype;\n"
    "  ['background','backgroundImage','cssText','borderImage','borderImageSource','listStyleImage','maskImage',\n"
    "   'webkitMaskImage','content'].forEach(function(p){hook(P,p);});\n"
    "  if(P.setProperty&&!P.setProperty.__walpe){var sp=P.setProperty;P.setProperty=function(n,v,pr){return sp.call(this,n,fix(v),pr);};P.setProperty.__walpe=1;}});\n"
    " var sa=Element.prototype.setAttribute;Element.prototype.setAttribute=function(n,v){\n"
    "  return sa.call(this,n,/^(src|href|style|poster)$/i.test(n)?fix(v):v);};\n"
    " if(window.Audio){var OA=window.Audio;window.Audio=function(s){return s===undefined?new OA():new OA(fix(s));};\n"
    "  window.Audio.prototype=OA.prototype;}\n"
    " if(window.fetch){var of=window.fetch;window.fetch=function(i,o){return of.call(this,typeof i==='string'?fix(i):i,o);};}\n"
    " var xo=XMLHttpRequest.prototype.open;XMLHttpRequest.prototype.open=function(m,u){var a=Array.prototype.slice.call(arguments);\n"
    "  a[1]=fix(u);return xo.apply(this,a);};\n"
    " if(!window.wallpaperRequestRandomFileForProperty)window.wallpaperRequestRandomFileForProperty=noop;\n"
    "})();\n";

/* requestAnimationFrame at the fps limit. Wallpaper Engine hands pages
 * frames at its fps setting; WebKit runs requestAnimationFrame at its
 * display rate (60 Hz timer, or the monitor's refresh: main.c), so with a
 * limit below that a page drew frames nobody saw. lim = the limit, nat =
 * the rate WebKit itself runs at when the daemon knows it (0 = not known).
 * While lim >= nat the page gets WebKit's own requestAnimationFrame,
 * untouched. Below it callbacks are collected and run from a native frame
 * when a period of 1/lim has passed (periods counted from when a frame
 * was due, so the average rate is the limit): same order, same time
 * stamp argument, cancelAnimationFrame works, a throwing callback does not
 * stop the others. __walpeRaf(n) sets the limit (walpe-ctl set fps=N). */
static char *raf_limit_js (int lim)
{
    int nat = 0;
    const char *thr = g_getenv ("WEBKIT_DISPLAY_REFRESH_THROTTLE_FPS");
    const char *tim = g_getenv ("WEBKIT_FORCE_VBLANK_TIMER");
    if (tim && *tim && strcmp (tim, "0")) {
        nat = 60;
        int t = thr ? atoi (thr) : 0;
        if (t > 0 && 60 % t == 0)
            nat = t;
    }
    return g_strdup_printf (
        "(function(){var lim=%d,nat=%d,nr=window.requestAnimationFrame,nc=window.cancelAnimationFrame;"
        "if(!nr||!nc)return;nr=nr.bind(window);nc=nc.bind(window);"
        "try{if(window.top!==window&&window.top.__walpeRaf)lim=window.top.__walpeRaf();}catch(e){}"
        "var q=[],cur=null,pend=0,due=0,nid=1e9;"
        "function pump(t){pend=0;var iv=1000/lim;"
        "if(!(lim>0)||t>=due-1.5){due=(t-due>iv)?t+iv:due+iv;cur=q;q=[];"
        "for(var i=0;i<cur.length;i++){var c=cur[i];if(!c)continue;cur[i]=null;"
        "try{c.f(t);}catch(e){(function(x){setTimeout(function(){throw x;},0);})(e);}}cur=null;}"
        "if(q.length&&!pend)pend=nr(pump);}"
        "function raf(f){if(!(lim>0)||(nat>0&&lim>=nat))return nr(f);"
        "var id=++nid;q.push({id:id,f:f});if(!pend)pend=nr(pump);return id;}"
        "function caf(id){if(id>1e9){var i;for(i=0;i<q.length;i++)if(q[i]&&q[i].id===id)q[i]=null;"
        "if(cur)for(i=0;i<cur.length;i++)if(cur[i]&&cur[i].id===id)cur[i]=null;return;}return nc(id);}"
        "window.requestAnimationFrame=raf;window.cancelAnimationFrame=caf;"
        "if(window.webkitRequestAnimationFrame)window.webkitRequestAnimationFrame=raf;"
        "if(window.webkitCancelAnimationFrame)window.webkitCancelAnimationFrame=caf;"
        "if(window.webkitCancelRequestAnimationFrame)window.webkitCancelRequestAnimationFrame=caf;"
        "window.__walpeRaf=function(n){if(n!==undefined)lim=n;return lim;};})();",
        lim, nat);
}

static gboolean sh_media_timer (gpointer user)
{
    WebShared *sh = user;
    if (sh->media_ref && sh->loaded)
        sh_push_media (sh);
    return G_SOURCE_CONTINUE;
}

static gboolean sh_report_timer (gpointer user)
{
    (void) user;
    timing_report ();
    return G_SOURCE_CONTINUE;
}

static void timer_set (guint *id, gboolean want, guint ms, GSourceFunc fn, gpointer user)
{
    if (!want && *id) {
        g_source_remove (*id);
        *id = 0;
    } else if (want && !*id) {
        *id = g_timeout_add (ms, fn, user);
    }
}

/* main thread: the timers a page needs right now. None at all for a page
 * without audio / media listener (captures follow the page's repaints);
 * version 0.12 woke the main loop 60-166 times a second per page to look for
 * a repaint, push the spectrum and check for a pause. */
static void sh_timers_update (WebShared *sh)
{
    gboolean run = !sh->dead && sh->view && sh->active_main && sh->loaded;
    guint ams = sh->audio_idle ? WEB_AUDIO_IDLE_MS : 1000 / WEB_AUDIO_HZ;
    if (sh->audio_id && sh->audio_ms != ams)
        timer_set (&sh->audio_id, FALSE, 0, NULL, NULL);
    sh->audio_ms = ams;
    timer_set (&sh->audio_id, run && sh->audio_ref, ams, sh_audio_timer, sh);
    /* (the media thread bumps a serial when something changed: 4 looks a
     * second are plenty for titles, covers and the timeline) */
    timer_set (&sh->media_id, run && sh->media_ref, 250, sh_media_timer, sh);
    timer_set (&sh->snap_id, run && use_snapshots (), (guint) (1000 / MAX (sh->fps, 1)),
               sh_snapshot_tick, sh);
    timer_set (&sh->report_id, !sh->dead && sh->view && sh->active_main && timing_on (), 2000,
               sh_report_timer, sh);
    if (!run)
        timer_set (&sh->cap_id, FALSE, 0, NULL, NULL);
}

static void sh_timers_stop (WebShared *sh)
{
    timer_set (&sh->audio_id, FALSE, 0, NULL, NULL);
    timer_set (&sh->media_id, FALSE, 0, NULL, NULL);
    timer_set (&sh->snap_id, FALSE, 0, NULL, NULL);
    timer_set (&sh->report_id, FALSE, 0, NULL, NULL);
    timer_set (&sh->cap_id, FALSE, 0, NULL, NULL);
    timer_set (&sh->replay_id, FALSE, 0, NULL, NULL);
    timer_set (&sh->first_id, FALSE, 0, NULL, NULL);
}

/* main thread: something about the viewers changed (one attached, was
 * paused or resumed, left, or its monitor has another size): bring the
 * page in line. Queued by the render threads; idles run FIFO, so one
 * queued before the page's teardown runs before it. */
static gboolean sh_state_main (gpointer user)
{
    WebShared *sh = user;
    if (sh->dead || !sh->view)
        return G_SOURCE_REMOVE;
    g_mutex_lock (&sh->lock);
    gboolean active = sh->active > 0 || sh_first_frame_wanted (sh);
    int ww = sh->want_w, wh = sh->want_h;
    g_mutex_unlock (&sh->lock);
    gboolean was = sh->active_main;
    sh->active_main = active;
    sh_audio_update (sh, active);
    sh_media_update (sh, active);
    if (!active) {
        sh_timers_update (sh);    /* everyone paused: nothing runs until a resume */
        sh_push_paused (sh, TRUE);
        return G_SOURCE_REMOVE;
    }
    sh_push_paused (sh, FALSE);
    if (ww > 0 && wh > 0 && (ww != sh->view_w || wh != sh->view_h)) {
#ifdef WEB_HAVE_PRIVATE
        web_private_screen (ww, wh);
#endif
        if (web_bigwin ())
            gtk_window_set_default_size (GTK_WINDOW (sh->win), ww, wh);
        walpe_web_host_set_size (WALPE_WEB_HOST (sh->box), ww, wh);
        sh->view_w = ww;
        sh->view_h = wh;
        /* grown: keep all of it off-screen (a window that reaches onto
         * the screen takes the real pointer there, and GDK then reads the
         * pointer position from it) */
        hide_window_offscreen (sh->win, ww, wh);
    }
    if (!was)
        sh_audio_wake (sh);
    sh_timers_update (sh);
    if (!was)
        sh_kick (sh);     /* shown again: the frame it has now */
    return G_SOURCE_REMOVE;
}

/* main thread: the page has had the time for its first picture
 * (sh_first_frame_wanted). From here on it is as active as its viewers:
 * when they are all paused it is told so and suspended now. */
static gboolean sh_first_end (gpointer user)
{
    WebShared *sh = user;
    sh->first_id = 0;
    g_mutex_lock (&sh->lock);
    sh->first_done = TRUE;
    g_mutex_unlock (&sh->lock);
    /* every monitor once more, the paused ones too: each takes the newest
     * frame if it has not yet and stops asking for forced frames */
    sh_notify_viewers (sh, TRUE);
    sh_state_main (sh);
    return G_SOURCE_REMOVE;
}

/* main thread: a new fps setting for a running page (queued by
 * web_set_fps before any teardown of the same page: idles run FIFO) */
static gboolean sh_fps_main (gpointer user)
{
    WebShared *sh = user;
    int fps = g_atomic_int_get (&sh->fps_want);
    if (sh->dead || fps < 1 || fps == sh->fps)
        return G_SOURCE_REMOVE;
    sh->fps = fps;
    sh->cap_due_us = 0;
    timer_set (&sh->snap_id, FALSE, 0, NULL, NULL);
    sh_timers_update (sh);
    if (sh->loaded) {
        char *js = g_strdup_printf ("(function(){if(window.__walpeRaf)window.__walpeRaf(%d);"
                                    "var L=window.wallpaperPropertyListener;"
                                    "if(L&&L.applyGeneralProperties)L.applyGeneralProperties({fps:%d});})();",
                                    fps, fps);
        sh_eval (sh, js);
        g_free (js);
    }
    g_message ("web: captures <= %d/s now", fps);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------ main-thread side */

static gboolean gtk_ready;
static WebKitWebContext *web_ctx;   /* the context pages are created in */

static void on_clipboard_changed (GdkClipboard *cb, gpointer user)
{
    (void) user;
    if (gdk_clipboard_is_local (cb) && gdk_clipboard_get_content (cb))
        gdk_clipboard_set_content (cb, NULL);
}

/* ---- the page host (webhostsel.h) */
static WebHost web_host = WEB_HOST_NONE;    /* in use once GTK is up; main thread */
static gboolean web_private_failed;         /* its start-up test failed: not tried again */
static gint web_host_shown = WEB_HOST_NONE; /* for status (any thread) */
#ifdef WEB_HAVE_PRIVATE
/* the X11 page host could not open its display or failed its start-up
 * test: not tried again (only a module with a second host keeps track) */
static gboolean web_x11_failed;
/* the host in use is not the first choice: why, for status (static text;
 * set once on the main thread, read by any) */
static const char *web_host_note;
#endif

static WebHost web_host_pick (const char **why)
{
    WebHostEnv e = { 0 };
    const char *xd = g_getenv ("DISPLAY"), *wd = g_getenv ("WAYLAND_DISPLAY");
    e.setting = g_getenv ("WALPE_WEB_HOST");
    /* the daemon says which backend it runs on (0 = none open yet: the
     * start-up test's process, which goes by the environment). A daemon
     * without that function has no backend but X11: it draws on X, also
     * under Xwayland in a session that sets WAYLAND_DISPLAY, and its pages
     * stay on the X11 page host as before. */
    int (*in_use) (void) = (int (*) (void)) dlsym (RTLD_DEFAULT, "walpe_backend_in_use");
    e.backend = in_use ? in_use () : 1;
    e.backend_env = g_getenv ("WALPE_BACKEND");
    e.x_display = xd && *xd;
    e.wayland_display = wd && *wd;
#ifdef WEB_HAVE_X11
    e.built_x11 = TRUE;
#endif
#ifdef WEB_HAVE_PRIVATE
    e.built_private = TRUE;
#endif
    e.private_failed = web_private_failed;
#ifdef WEB_HAVE_PRIVATE
    e.x11_failed = web_x11_failed;
#endif
    return web_host_choose (&e, why);
}

#ifdef WEB_HAVE_PRIVATE
/* What a page reads as window.screen is GTK's monitor, and on the private
 * page host that is the host's one made-up output. It is given the size
 * of the largest page (a page fills its monitor): left at the host's own
 * 16384 x 16384, a page that sizes a canvas by screen.width / .height
 * would ask for a quarter of a billion pixels. The first page's size is
 * there before GTK connects; a later, larger page changes the mode. */
static void web_private_screen (int w, int h)
{
    static int max_w, max_h;
    if (w <= max_w && h <= max_h)
        return;
    max_w = MAX (max_w, w);
    max_h = MAX (max_h, h);
    walpe_webhost_output_size (max_w, max_h);
}

/* GTK on the module's own Wayland display. The display has to be GTK's
 * default one before anything of WebKit asks for "the" display, and it
 * must not come from the environment (WAYLAND_DISPLAY is the session's
 * compositor, and the environment cannot be changed once threads run):
 * it is opened by name. */
static gboolean gtk_init_private (void)
{
    char *sock = NULL, *why = NULL;
    if (!walpe_webhost_start (&sock, &why)) {
        g_warning ("web: %s", why ? why : "the private page host cannot start");
        g_free (why);
        return FALSE;
    }
    gdk_set_allowed_backends ("wayland");
    if (!gtk_is_initialized ())
        gtk_disable_setlocale ();
    /* opened before gtk_init: GTK then takes it as the default display
     * instead of opening the session's */
    GdkDisplay *d = gdk_display_open (sock);
    gboolean ok = d != NULL && gtk_init_check ();
    if (!d && gtk_init_check ()) {
        /* a GTK that wants to be initialised first: that opened the
         * session's display; ours replaces it as the default */
        GdkDisplay *first = gdk_display_get_default ();
        d = gdk_display_open (sock);
        ok = d != NULL;
        if (d && first && first != d) {
            gdk_display_manager_set_default_display (gdk_display_manager_get (), d);
            gdk_display_close (first);
        }
    }
    g_free (sock);
    if (!ok || !d) {
        walpe_webhost_abandon ();
        g_warning ("web: GTK could not use the private page host%s",
                   d ? " (gtk_init failed)" : " (its Wayland backend did not connect; GDK_BACKEND "
                                              "set to something else?)");
        return FALSE;
    }
    if (gdk_display_get_default () != d)
        gdk_display_manager_set_default_display (gdk_display_manager_get (), d);
    g_message ("web: pages live on a private Wayland display inside the daemon (no window on the "
               "desktop, no X; the session's clipboard and selections are out of their reach)");
    return TRUE;
}
#endif

static gboolean ensure_gtk (void)
{
    if (gtk_ready)
        return TRUE;
    const char *host_why = NULL;
    WebHost host = web_host_pick (&host_why);
    if (host == WEB_HOST_NONE) {
        g_warning ("web: %s", host_why ? host_why : "no page host");
        return FALSE;
    }
    if (host_why)
        g_message ("web: %s", host_why);
    /* GSK_RENDERER=cairo / GDK GL off are set in main() before any thread
     * exists: g_setenv here raced getenv on the render/audio/media threads
     * (and in the GL driver) - glibc reallocates environ on a new name */
#ifdef WEB_HAVE_PRIVATE
    if (host == WEB_HOST_PRIVATE && !gtk_init_private ())
        return FALSE;
#endif
    if (host == WEB_HOST_X11) {
#ifdef WEB_HAVE_X11
        gdk_set_allowed_backends ("x11");
        /* gtk_init would setlocale (LC_ALL, "") for the whole daemon: in a
         * comma-decimal locale every scene loaded afterwards was mis-parsed
         * ("0.5" read as 0) and Turkish case rules reached strcasecmp. The
         * daemon stays in the "C" locale it started in; WebKit takes the
         * page language from the environment, not from setlocale. */
        if (!gtk_is_initialized ())
            gtk_disable_setlocale ();
        if (!gtk_init_check ()) {
#ifdef WEB_HAVE_PRIVATE
            /* DISPLAY is set and no X server answers there (a variable
             * left over in a session without Xwayland): GTK has opened
             * nothing, so where the choice was this module's (a Wayland
             * backend, WALPE_WEB_HOST not x11) the private page host is
             * taken instead. The start-up test's process comes this way
             * too, and so tests the host this daemon ends up with. */
            web_x11_failed = TRUE;
            host = web_host_pick (&host_why);
            if (host == WEB_HOST_PRIVATE) {
                g_warning ("web: %s", host_why);
                if (!gtk_init_private ())
                    return FALSE;
            } else
#endif
            {
                g_warning ("web: gtk_init failed (no display?)");
                return FALSE;
            }
        }
        /* gtk_init replaces the process-wide Xlib error handler (the daemon's
         * walpe_xerror) with GDK's, which _exit(1)s on any X error on its
         * display that no GDK trap covers - the daemon vanished without a
         * trace. A trap that is never popped covers them all; GDK's own nested
         * traps still get their errors (innermost first). */
        if (host == WEB_HOST_X11) {
            /* GTK 4.18 marks GDK's X11 interface as deprecated without a successor */
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            gdk_x11_display_error_trap_push (gdk_display_get_default ());
            G_GNUC_END_IGNORE_DEPRECATIONS
        }
#else
        return FALSE;   /* web_host_pick does not choose a host that is not built */
#endif
    }
    web_host = host;
#ifdef WEB_HAVE_PRIVATE
    if (host == WEB_HOST_PRIVATE && web_x11_failed)
        g_atomic_pointer_set (&web_host_note, "the X11 page host did not come up");
    else if (host == WEB_HOST_X11 && web_private_failed)
        g_atomic_pointer_set (&web_host_note, "the private page host failed its start-up test");
#endif
    g_atomic_int_set (&web_host_shown, (gint) host);
    /* a page can select text in a form field by script: WebKit then made
     * the daemon the owner of the X PRIMARY selection with page-chosen
     * text, replacing what the user had selected (the next middle-click
     * paste into a terminal inserted the wallpaper's text). A wallpaper
     * has no business owning a selection: give it up as soon as it is ours.
     * (X11 host: the claim itself is stopped in the daemon, selguard.c,
     * and this drops what GDK keeps locally. Private host: the display
     * cancels every claim, and what is dropped here is the copy pages of
     * this process could otherwise read from each other.) */
    g_signal_connect (gdk_display_get_primary_clipboard (gdk_display_get_default ()), "changed",
                      G_CALLBACK (on_clipboard_changed), NULL);
    g_signal_connect (gdk_display_get_clipboard (gdk_display_get_default ()), "changed",
                      G_CALLBACK (on_clipboard_changed), NULL);
    /* WALPE_WEB_MEMORY_MB=N (experimental, off by default): page processes
     * start giving memory back (caches, a full garbage collection) from
     * half of N MB of their own footprint, looked at every 10 s. WebKit's
     * default limit is derived from the machine's RAM, which a wallpaper
     * page never reaches, so nothing is ever released. Never kills a page
     * for its size. */
    {
        const char *ml = g_getenv ("WALPE_WEB_MEMORY_MB");
        int mb = ml ? atoi (ml) : 0;
        if (mb >= 64) {
            WebKitMemoryPressureSettings *mps = webkit_memory_pressure_settings_new ();
            webkit_memory_pressure_settings_set_memory_limit (mps, (guint) mb);
            webkit_memory_pressure_settings_set_conservative_threshold (mps, 0.5);
            webkit_memory_pressure_settings_set_strict_threshold (mps, 0.75);
            webkit_memory_pressure_settings_set_kill_threshold (mps, 0);
            webkit_memory_pressure_settings_set_poll_interval (mps, 10);
            web_ctx = g_object_new (WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", mps, NULL);
            webkit_memory_pressure_settings_free (mps);
            g_message ("web: page processes release memory from %d MB (WALPE_WEB_MEMORY_MB=%d)",
                       mb / 2, mb);
        }
    }
    if (!web_ctx)
        web_ctx = g_object_ref (webkit_web_context_get_default ());
    WebKitWebContext *ctx = web_ctx;
    webkit_web_context_register_uri_scheme (ctx, WEB_SCHEME, scheme_cb, NULL, NULL);
    WebKitSecurityManager *sm = webkit_web_context_get_security_manager (ctx);
    webkit_security_manager_register_uri_scheme_as_secure (sm, WEB_SCHEME);
    webkit_security_manager_register_uri_scheme_as_cors_enabled (sm, WEB_SCHEME);
    webkit_web_context_set_cache_model (ctx, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    gtk_ready = TRUE;
    return TRUE;
}

/* off-screen, override-redirect: the WM never manages, tiles or focuses it,
 * but it is mapped, so WebKit keeps the page "visible" and animating.
 * (X11 page host only.) */
static void hide_window_offscreen (GtkWidget *win, int w, int h)
{
#ifdef WEB_HAVE_X11
    GdkSurface *surf = gtk_native_get_surface (GTK_NATIVE (win));
    if (!surf || !GDK_IS_X11_SURFACE (surf))
        return;     /* the private page host: its display shows nothing anyway */
    /* GTK 4.18 marks GDK's X11 interface as deprecated without a successor: the
     * page's window is put off screen through X itself, so the two calls stay */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    Display *xd = gdk_x11_display_get_xdisplay (gdk_surface_get_display (surf));
    Window xid = gdk_x11_surface_get_xid (surf);
    G_GNUC_END_IGNORE_DEPRECATIONS
    XSetWindowAttributes a = { .override_redirect = True };
    XChangeWindowAttributes (xd, xid, CWOverrideRedirect, &a);
    XMoveWindow (xd, xid, -w - 64, -h - 64);
    XFlush (xd);
#else
    (void) win; (void) w; (void) h;
#endif
}

/* ------------------------------------------------ WebKit start-up probe
 * WebKit can abort() the process that hosts it while it initialises -
 * on NVIDIA its DMA-BUF renderer died with "Could not create GBM EGL
 * display: EGL_NOT_INITIALIZED. Aborting..." (fixed by
 * WEBKIT_DISABLE_DMABUF_RENDERER=1, set in main()). That process is the
 * daemon: every monitor went with it, and the GUI respawned it into the
 * same web wallpaper every 3 s. So before the first page is created in
 * this process, `walpe-daemon --web-probe` does the same start-up (GTK,
 * an off-screen view, one page, one snapshot) in a child; only if the
 * child succeeds does WebKit run in here. Otherwise web wallpapers stay
 * black with the reason in the log and in `status` ("web_error"). */
enum { PROBE_UNKNOWN, PROBE_RUNNING, PROBE_OK, PROBE_FAILED };
static int probe_state;
static GPtrArray *probe_waiting;  /* WebShared waiting for the result (main thread) */
static char *probe_error;         /* under reg_lock */

static gboolean sh_create_real (WebShared *sh);
static gboolean probe_spawn (void);
/* the page host the running test uses (the child is told by
 * WALPE_WEB_HOST, so it tests what this process will then use) */
static WebHost probe_host = WEB_HOST_NONE;

/* what the start-up test said about WebGL in pages (under reg_lock):
 * the renderer string, "" = not known */
static char *probe_webgl;

static gboolean gl_is_software (const char *r)
{
    char *l = g_ascii_strdown (r ? r : "", -1);
    gboolean sw = strstr (l, "llvmpipe") || strstr (l, "softpipe") || strstr (l, "swiftshader") ||
                  strstr (l, "swrast") || strstr (l, "software");
    g_free (l);
    return sw;
}

/* "WALPE-PROBE webgl=<renderer>" in the test process's output. WebKit
 * runs WebGL in the page's process with whatever EGL display it finds
 * there; with its compositing switched off that display is not the one
 * of the X server, and on a machine whose GPU driver does not offer it
 * (EGL "surfaceless") the page's WebGL silently runs on Mesa's software
 * rasteriser: a full-screen shader effect is then a few frames a second
 * on the CPU however fast the graphics card is. Nothing in the page or
 * in the daemon shows that, so the test asks and the log / status say. */
static void probe_note_webgl (const char *out)
{
    const char *l = out ? strstr (out, "WALPE-PROBE webgl=") : NULL;
    if (!l)
        return;
    l += strlen ("WALPE-PROBE webgl=");
    char *r = g_strndup (l, strcspn (l, "\r\n"));
    g_strstrip (r);
    if (gl_is_software (r))
        g_warning ("web: WebGL in web wallpapers runs on the CPU here (%s): pages with WebGL "
                   "effects will be slow whatever the graphics card. See docs/SETTINGS.md, "
                   "\"Rendering\" (web_accel; \"web_webgl\" in walpe-ctl status)", r);
    else
        g_message ("web: WebGL in web wallpapers: %s", r);
    g_mutex_lock (&reg_lock);
    g_free (probe_webgl);
    probe_webgl = r;
    g_mutex_unlock (&reg_lock);
}

/* The test process gives up by itself (20 s without a page, 8 s without
 * WebGL), but not when it is stuck - inside a driver, or stopped. Then
 * nothing ever answered here: the pages waiting for the result stayed
 * black for good, with no reason in the log or in `status`. After
 * WEB_PROBE_MAX_S it is killed and the test has failed. The wait for its
 * output is given up as well: WebKit's helper processes may keep that pipe
 * open after the process itself has gone. */
#define WEB_PROBE_MAX_S 40
static GCancellable *probe_cancel;
static guint probe_child_timer;
static gboolean probe_timed_out;

static gboolean probe_child_overdue (gpointer user)
{
    probe_child_timer = 0;
    probe_timed_out = TRUE;
    g_subprocess_force_exit (G_SUBPROCESS (user));
    g_cancellable_cancel (probe_cancel);
    return G_SOURCE_REMOVE;
}

static void probe_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    GSubprocess *p = G_SUBPROCESS (src);
    GError *err = NULL;
    char *out = NULL;
    if (probe_child_timer) {
        g_source_remove (probe_child_timer);
        probe_child_timer = 0;
    }
    gboolean ok = g_subprocess_communicate_utf8_finish (p, res, &out, NULL, &err) &&
                  g_subprocess_get_successful (p);
    g_clear_object (&probe_cancel);
    char *why = NULL;
    if (!ok)
        why = probe_timed_out
            ? g_strdup_printf ("the WebKit start-up test did not finish within %d s", WEB_PROBE_MAX_S)
            : err ? g_strdup (err->message)
            : g_subprocess_get_if_signaled (p)
            ? g_strdup_printf ("WebKit start-up killed the test process with signal %d (%s)",
                               g_subprocess_get_term_sig (p), g_strsignal (g_subprocess_get_term_sig (p)))
            : g_strdup_printf ("WebKit start-up test exited with status %d",
                               g_subprocess_get_exit_status (p));
    g_clear_error (&err);
    if (!ok && probe_host == WEB_HOST_PRIVATE && !web_private_failed) {
        /* the private page host did not get through: with an X display
         * (Xwayland) the off-screen X window is tried before giving up */
        web_private_failed = TRUE;
        const char *hw = NULL;
        WebHost next = web_host_pick (&hw);
        if (next == WEB_HOST_X11) {
            g_warning ("web: %s (%s)", hw ? hw : "falling back to the X11 page host", why);
            g_free (why);
            g_free (out);
            g_object_unref (p);
            if (probe_spawn ())
                return;
            /* could not even start the second test: as after a failed one */
            ok = FALSE;
            why = g_strdup ("the start-up test for the X11 page host could not be run");
            out = NULL;
            p = NULL;
        }
    }
#ifdef WEB_HAVE_PRIVATE
    else if (!ok && probe_host == WEB_HOST_X11 && !web_x11_failed) {
        /* the other way round: the X11 page host did not get through (no
         * X server behind DISPLAY, or WebKit failed on it). Where it was
         * this module's choice on a Wayland backend, the private page
         * host is tried before web wallpapers are switched off. */
        web_x11_failed = TRUE;
        const char *hw = NULL;
        if (web_host_pick (&hw) == WEB_HOST_PRIVATE) {
            g_warning ("web: %s (%s)", hw ? hw : "falling back to the private page host", why);
            g_free (why);
            g_free (out);
            g_object_unref (p);
            if (probe_spawn ())
                return;
            ok = FALSE;
            why = g_strdup ("the start-up test for the private page host could not be run");
            out = NULL;
            p = NULL;
        }
    }
#endif
    probe_state = ok ? PROBE_OK : PROBE_FAILED;
    if (ok) {
        g_message ("web: WebKit start-up test passed");
        probe_note_webgl (out);
    } else {
        g_warning ("web: %s - web wallpapers are disabled in this daemon (it would have aborted); "
                   "see the lines above for WebKit's reason", why);
        g_mutex_lock (&reg_lock);
        g_free (probe_error);
        probe_error = g_strdup (why);
        g_mutex_unlock (&reg_lock);
    }
    g_free (why);
    g_free (out);
    GPtrArray *w = probe_waiting;
    probe_waiting = NULL;
    for (guint i = 0; w && i < w->len; i++)
        if (ok)
            sh_create_real (w->pdata[i]);
    if (w)
        g_ptr_array_unref (w);
    if (p)
        g_object_unref (p);
}

/* run `walpe-daemon --web-probe` for the page host that would be used
 * now; FALSE when the child could not be started */
static gboolean probe_spawn (void)
{
    char *exe = g_file_read_link ("/proc/self/exe", NULL);
    GError *err = NULL;
    GSubprocess *p = NULL;
    const char *hw = NULL;
    probe_host = web_host_pick (&hw);
    if (exe) {
        /* stderr inherited: WebKit's own abort message lands in our log;
         * stdout is the test's answer (probe_note_webgl) */
        GSubprocessLauncher *l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
        if (probe_host != WEB_HOST_NONE)
            g_subprocess_launcher_setenv (l, "WALPE_WEB_HOST", web_host_name (probe_host), TRUE);
        char self[24];
        g_snprintf (self, sizeof self, "%ld", (long) getpid ());
        g_subprocess_launcher_setenv (l, "WALPE_WEB_PROBE_PARENT", self, TRUE);   /* walpe_web_probe_run */
        p = g_subprocess_launcher_spawn (l, &err, exe, "--web-probe", NULL);
        g_object_unref (l);
    }
    g_free (exe);
    if (!p) {
        g_warning ("web: cannot run the WebKit start-up test (%s)", err ? err->message : "no exe");
        g_clear_error (&err);
        return FALSE;
    }
    g_message ("web: testing WebKit start-up in a child process (page host: %s)",
               web_host_name (probe_host));
    probe_timed_out = FALSE;   /* (a second test, for the other page host, starts from here too) */
    probe_cancel = g_cancellable_new ();
    probe_child_timer = g_timeout_add_seconds (WEB_PROBE_MAX_S, probe_child_overdue, p);
    g_subprocess_communicate_utf8_async (p, NULL, probe_cancel, probe_done, NULL);
    return TRUE;
}

static gboolean sh_create_main (gpointer user)
{
    WebShared *sh = user;
    if (probe_state == PROBE_UNKNOWN) {
        /* the daemon already ran the test at start-up (hardware
         * acceleration on: main.c decides by it before any thread exists) */
        const char *pre = g_getenv ("WALPE_WEB_PROBED");
        if (pre) {
            probe_state = PROBE_OK;
            if (*pre) {
                char *line = g_strconcat ("WALPE-PROBE webgl=", pre, NULL);
                probe_note_webgl (line);
                g_free (line);
            }
        }
    }
    if (probe_state == PROBE_OK)
        return sh_create_real (sh);
    if (probe_state == PROBE_FAILED)
        return G_SOURCE_REMOVE;
    if (!probe_waiting)
        probe_waiting = g_ptr_array_new ();
    g_ptr_array_add (probe_waiting, sh);
    if (probe_state == PROBE_RUNNING)
        return G_SOURCE_REMOVE;
    probe_state = PROBE_RUNNING;
    if (!probe_spawn ()) {
        /* cannot test (no /proc?): keep the old in-process behaviour */
        probe_state = PROBE_OK;
        GPtrArray *w = probe_waiting;
        probe_waiting = NULL;
        for (guint i = 0; i < w->len; i++)
            sh_create_real (w->pdata[i]);
        g_ptr_array_unref (w);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_REMOVE;
}

char *walpe_web_error (void)
{
    g_mutex_lock (&reg_lock);
    char *r = g_strdup (probe_error ? probe_error : page_error ? page_error : "");
    g_mutex_unlock (&reg_lock);
    return r;
}

/* how pages are rendered, for status: {"web_webgl": ...} (g_free; "" =
 * not known yet) */
char *walpe_web_info (void)
{
    g_mutex_lock (&reg_lock);
    char *r = g_strdup (probe_webgl ? probe_webgl : "");
    g_mutex_unlock (&reg_lock);
    return r;
}

/* `walpe-daemon --web-probe`: WebKit start-up in this throw-away process.
 * Step 1 (what decides): GTK, an off-screen view, one page, one snapshot.
 * Step 2: a page with a WebGL canvas reports the renderer WebGL uses in
 * the page process ("WALPE-PROBE webgl=..." on stdout). A page process
 * that dies in step 2 only fails the test when hardware acceleration is
 * being tried (WALPE_WEB_PROBE_ACCEL, set by main.c for that run): in
 * software mode pages without WebGL still work. */
static int probe_rc = 3;
static GMainLoop *probe_loop;
static int probe_step;
static gboolean probe_accel;

static const char PROBE_GL_HTML[] =
    "<html><body style='background:#123'><canvas id='c' width='64' height='64' "
    "style='will-change:transform'></canvas><script>"
    "var r='none';try{var c=document.getElementById('c');"
    "var g=c.getContext('webgl')||c.getContext('experimental-webgl');"
    "if(g){var d=g.getExtension('WEBGL_debug_renderer_info');"
    "r=String(d?g.getParameter(d.UNMASKED_RENDERER_WEBGL):g.getParameter(g.RENDERER));"
    "g.clearColor(0,1,0,1);g.clear(g.COLOR_BUFFER_BIT);}}catch(e){r='error: '+e;}"
    "window.webkit.messageHandlers.walpe.postMessage('webgl:'+r);</script></body></html>";

/* is `pid` a descendant of this process? */
static gboolean pid_is_ours (int pid)
{
    int self = (int) getpid ();
    for (int hops = 0; pid > 1 && hops < 8; hops++) {
        char path[64], *txt = NULL;
        g_snprintf (path, sizeof path, "/proc/%d/stat", pid);
        if (!g_file_get_contents (path, &txt, NULL, NULL))
            return FALSE;
        const char *rp = strrchr (txt, ')');    /* pid (comm) state ppid */
        int ppid = 0;
        char st = 0;
        gboolean ok = rp && sscanf (rp + 1, " %c %d", &st, &ppid) == 2;
        g_free (txt);
        if (!ok)
            return FALSE;
        if (ppid == self)
            return TRUE;
        pid = ppid;
    }
    return FALSE;
}

/* What does WebGL really run on in the page process? WebKit does not tell
 * a page (the WEBGL_debug_renderer_info strings are masked: "Apple GPU"),
 * so the test looks at its own page process: Mesa's software rasteriser
 * shows as threads named "llvmpipe-N", a hardware driver as its mapped
 * libraries. g_free. */
static char *probe_gl_backend (void)
{
    char *found = NULL;
    GDir *d = g_dir_open ("/proc", 0, NULL);
    const char *n;
    while (d && !found && (n = g_dir_read_name (d))) {
        int pid = atoi (n);
        char path[96], *comm = NULL;
        if (pid <= 1)
            continue;
        g_snprintf (path, sizeof path, "/proc/%d/comm", pid);
        if (!g_file_get_contents (path, &comm, NULL, NULL))
            continue;
        gboolean web = g_str_has_prefix (comm, "WebKitWebProc");
        g_free (comm);
        if (!web || !pid_is_ours (pid))
            continue;
        gboolean llvmpipe = FALSE;
        g_snprintf (path, sizeof path, "/proc/%d/task", pid);
        GDir *td = g_dir_open (path, 0, NULL);
        const char *t;
        while (td && !llvmpipe && (t = g_dir_read_name (td))) {
            char tp[128], *tc = NULL;
            g_snprintf (tp, sizeof tp, "/proc/%d/task/%s/comm", pid, t);
            if (g_file_get_contents (tp, &tc, NULL, NULL))
                llvmpipe = g_str_has_prefix (tc, "llvmpipe") || g_str_has_prefix (tc, "swrast");
            g_free (tc);
        }
        if (td)
            g_dir_close (td);
        char *maps = NULL;
        g_snprintf (path, sizeof path, "/proc/%d/maps", pid);
        g_file_get_contents (path, &maps, NULL, NULL);
        const char *m = maps ? maps : "";
        if (llvmpipe)
            found = g_strdup ("llvmpipe (Mesa software rasteriser)");
        else if (strstr (m, "libnvidia-eglcore") || strstr (m, "libnvidia-glcore") || strstr (m, "libEGL_nvidia"))
            found = g_strdup ("NVIDIA driver");
        else if (strstr (m, "libgallium") || strstr (m, "_dri.so"))
            found = g_strdup ("Mesa hardware driver");
        else if (strstr (m, "libEGL"))
            found = g_strdup ("GL driver not recognised");
        g_free (maps);
    }
    if (d)
        g_dir_close (d);
    return found ? found : g_strdup ("");
}

static void probe_snapshot2_cb (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    GdkTexture *t = webkit_web_view_get_snapshot_finish (WEBKIT_WEB_VIEW (src), res, NULL);
    g_clear_object (&t);
    g_main_loop_quit (probe_loop);
}

static void probe_message (WebKitUserContentManager *m, JSCValue *v, gpointer user)
{
    (void) m;
    WebKitWebView *view = user;
    char *msg = jsc_value_is_string (v) ? jsc_value_to_string (v) : NULL;
    if (msg && g_str_has_prefix (msg, "webgl:") && probe_step == 2) {
        /* the page only says whether it got a context */
        gboolean none = g_str_has_prefix (msg + 6, "none") || g_str_has_prefix (msg + 6, "error");
        char *be = none ? g_strdup ("none (pages get no WebGL context)") : probe_gl_backend ();
        g_print ("WALPE-PROBE webgl=%s\n", be);
        g_free (be);
        probe_step = 3;
        if (probe_accel)    /* one composited frame must make it back as well */
            webkit_web_view_get_snapshot (view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                          WEBKIT_SNAPSHOT_OPTIONS_TRANSPARENT_BACKGROUND, NULL,
                                          probe_snapshot2_cb, NULL);
        else
            g_main_loop_quit (probe_loop);
    }
    g_free (msg);
}

static gboolean probe_gl_timeout (gpointer user)
{
    (void) user;
    if (probe_step == 2)
        g_print ("WALPE-PROBE webgl=\n");   /* no answer: not known */
    g_main_loop_quit (probe_loop);
    return G_SOURCE_REMOVE;
}

static void probe_snapshot_cb (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    GdkTexture *t = webkit_web_view_get_snapshot_finish (WEBKIT_WEB_VIEW (src), res, NULL);
    probe_rc = 0;   /* got this far without an abort; a failed snapshot is not fatal */
    g_clear_object (&t);
    probe_step = 2;
    webkit_web_view_load_html (WEBKIT_WEB_VIEW (src), PROBE_GL_HTML, NULL);
    g_timeout_add_seconds (8, probe_gl_timeout, NULL);
}

static void probe_load_changed (WebKitWebView *v, WebKitLoadEvent e, gpointer user)
{
    (void) user;
    if (e == WEBKIT_LOAD_FINISHED && probe_step == 1)
        webkit_web_view_get_snapshot (v, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                      WEBKIT_SNAPSHOT_OPTIONS_TRANSPARENT_BACKGROUND, NULL,
                                      probe_snapshot_cb, NULL);
}

static void probe_process_terminated (WebKitWebView *v, WebKitWebProcessTerminationReason r,
                                      gpointer user)
{
    (void) v; (void) r; (void) user;
    if (probe_step >= 2) {
        g_print ("WALPE-PROBE webgl=unavailable (the page process died when WebGL was used)\n");
        if (probe_accel)
            probe_rc = 5;
    } else {
        g_printerr ("walpe web probe: the page process died\n");
        probe_rc = 4;
    }
    g_main_loop_quit (probe_loop);
}

static gboolean probe_timeout (gpointer user)
{
    (void) user;
    if (probe_step < 2)
        g_printerr ("walpe web probe: no page after 20 s\n");
    g_main_loop_quit (probe_loop);
    return G_SOURCE_REMOVE;
}

int walpe_web_probe_run (void)
{
#ifdef __linux__
    /* This process is of use to the daemon that started it and to nobody
     * else. A daemon that quit (or was killed, or crashed) while the test
     * ran left it behind: a second "walpe-daemon" with WebKit's processes
     * under it, for up to half a minute, writing into the log of a daemon
     * that was gone. The kernel ends it with its parent now. (A restart in
     * place is an exec of the same thread: no parent dies there, and that
     * path ends the daemon's children itself.) */
    prctl (PR_SET_PDEATHSIG, SIGKILL);
    {
        /* ... unless the parent was gone before the line above was reached
         * (this process had only just been started): the daemon says who
         * it is, and a different parent now means it went away */
        const char *pp = g_getenv ("WALPE_WEB_PROBE_PARENT");
        if (pp && *pp && g_ascii_strtoll (pp, NULL, 10) != (gint64) getppid ())
            return 2;
    }
#endif
    if (!ensure_gtk ())
        return 2;
    probe_accel = g_getenv ("WALPE_WEB_PROBE_ACCEL") != NULL;
    WebKitUserContentManager *ucm = webkit_user_content_manager_new ();
    webkit_user_content_manager_register_script_message_handler (ucm, "walpe", NULL);
    WebKitWebView *view = WEBKIT_WEB_VIEW (g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                                         "user-content-manager", ucm, NULL));
    g_signal_connect (ucm, "script-message-received::walpe", G_CALLBACK (probe_message), view);
    g_object_unref (ucm);
    webkit_settings_set_enable_webgl (webkit_web_view_get_settings (view), TRUE);
    GtkWidget *win = gtk_window_new ();
    gtk_window_set_decorated (GTK_WINDOW (win), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (win), 320, 240);
    gtk_window_set_child (GTK_WINDOW (win), GTK_WIDGET (view));
    gtk_widget_realize (win);
    hide_window_offscreen (win, 320, 240);
    gtk_window_present (GTK_WINDOW (win));
    probe_loop = g_main_loop_new (NULL, FALSE);
    probe_step = 1;
    g_signal_connect (view, "load-changed", G_CALLBACK (probe_load_changed), NULL);
    g_signal_connect (view, "web-process-terminated", G_CALLBACK (probe_process_terminated), NULL);
    webkit_web_view_load_html (view, "<html><body style='background:#123'>walpe</body></html>", NULL);
    g_timeout_add_seconds (20, probe_timeout, NULL);
    g_main_loop_run (probe_loop);
    fflush (stdout);
    return probe_rc;   /* the caller _exit()s: no teardown of WebKit */
}

/* A wallpaper never downloads anything. WebKit's default saves an
 * attachment response, a response it cannot display or an
 * <a download="name"> click under ~/Downloads - $HOME where no XDG user
 * dirs are set up - with a file name the page chose. Every download is
 * cancelled as it starts, and never gets a destination. */
static gboolean on_download_destination (WebKitDownload *dl, const char *suggested, gpointer user)
{
    (void) suggested; (void) user;
    webkit_download_cancel (dl);
    return TRUE;    /* handled: WebKit's default handler would pick a path */
}

static void on_download_started (WebKitNetworkSession *ns, WebKitDownload *dl, gpointer user)
{
    (void) ns; (void) user;
    WebKitURIRequest *rq = webkit_download_get_request (dl);
    if (web_log_gate (WEB_LOG_DOWNLOAD)) {
        char *u = web_log_text (rq ? webkit_uri_request_get_uri (rq) : NULL, 300);
        g_message ("web: download refused (wallpapers do not save files): %s", u);
        g_free (u);
    }
    g_signal_connect (dl, "decide-destination", G_CALLBACK (on_download_destination), NULL);
    webkit_download_cancel (dl);
}

/* Network access of a page: on by default (pages fetch weather, images,
 * fonts - as under Wallpaper Engine). Off per wallpaper with its built-in
 * option "Allow network access" (walpe_network, engine/content.c; GUI
 * option row or `walpe-ctl setprop walpe_network=false`), off for every
 * page with WALPE_WEB_OFFLINE=1. Off = every request of the page goes to
 * a proxy that serves nothing but the wallpaper's own files: the loopback
 * file server itself (http_cb refuses every other target). Until version
 * 0.12.1 the proxy was a dead port with 127.0.0.1 and localhost exempt, so
 * that the page's own files still loaded - and with them every other
 * port of this machine: a page with network access "off" could still
 * talk to local services (a printer or router page forwarded to
 * localhost, a development server, a media server's API) over http and
 * WebSocket. With the walpe-web:// scheme or file:// (WALPE_WEB_SCHEME,
 * WALPE_WEB_FILE_URLS) the page's files are not fetched over http: there
 * the proxy is a dead port and nothing is exempt. Returns whether the
 * state changed. */
static gboolean sh_apply_network (WebShared *sh, WebKitNetworkSession *ns)
{
    const char *env = g_getenv ("WALPE_WEB_OFFLINE");
    gboolean env_off = env && *env && strcmp (env, "0");
    char *v = walpe_config_get_prop (sh->origin, "walpe_network");
    gboolean opt_off = v && (!g_ascii_strcasecmp (v, "false") || !strcmp (v, "0"));
    g_free (v);
    /* WALPE_WEB_FILE_URLS=1 (the page runs from file:// with file access,
     * a diagnostic fallback): WebKit then lets the page read EVERY local
     * file the user can read, not only its own folder (fetch
     * ('file:///home/...'): there is no narrower setting). What it reads
     * must not leave the machine, so such a page never has network access. */
    gboolean file_off = use_file_urls ();
    gboolean off = env_off || opt_off || file_off;
    if (sh->net_known && off == sh->net_off)
        return FALSE;
    if (off) {
        char *proxy = !use_scheme () && !use_file_urls () && http_start ()
            ? g_strdup_printf ("http://127.0.0.1:%u", http_port) : g_strdup ("http://127.0.0.1:9");
        WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new (proxy, NULL);
        webkit_network_session_set_proxy_settings (ns, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
        webkit_network_proxy_settings_free (ps);
        g_free (proxy);
        if (file_off)
            g_warning ("web: WALPE_WEB_FILE_URLS=1: the page of %s can read every local file "
                       "you can read (WebKit has no narrower setting); its network access is "
                       "switched off for that reason. Use it for diagnosis only", sh->path);
        g_message ("web: network access is off for %s (%s): external requests are refused",
                   sh->path, env_off ? "WALPE_WEB_OFFLINE"
                                     : opt_off ? "the wallpaper's \"Allow network access\" option"
                                               : "WALPE_WEB_FILE_URLS");
    } else if (sh->net_known) {
        webkit_network_session_set_proxy_settings (ns, WEBKIT_NETWORK_PROXY_MODE_DEFAULT, NULL);
        g_message ("web: network access is on again for %s", sh->path);
    }
    gboolean changed = sh->net_known;
    sh->net_known = TRUE;
    sh->net_off = off;
    return changed;
}

static gboolean sh_create_real (WebShared *sh)
{
    if (sh->dead)
        return G_SOURCE_REMOVE;
    /* the page's size: the largest its viewers asked for (their monitors'
     * render sizes). None has yet when the page is made before the first
     * frame of any viewer: a placeholder until the first one says
     * (web_gl_render -> sh_state_main resizes the page) */
    g_mutex_lock (&sh->lock);
    int w = sh->want_w > 0 ? sh->want_w : WEB_SIZE_UNKNOWN_W;
    int h = sh->want_h > 0 ? sh->want_h : WEB_SIZE_UNKNOWN_H;
    g_mutex_unlock (&sh->lock);
#ifdef WEB_HAVE_PRIVATE
    /* before GTK is brought up: the private page host announces its
     * output with this size (nothing happens with the X11 page host) */
    web_private_screen (w, h);
#endif
    if (!ensure_gtk ())
        return G_SOURCE_REMOVE;

    WebKitNetworkSession *ns = webkit_network_session_new_ephemeral ();
    g_signal_connect (ns, "download-started", G_CALLBACK (on_download_started), NULL);
    sh_apply_network (sh, ns);
    WebKitUserContentManager *ucm = webkit_user_content_manager_new ();
    WebKitUserScript *us = webkit_user_script_new (WE_API_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                   WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                   NULL, NULL);
    webkit_user_content_manager_add_script (ucm, us);
    webkit_user_script_unref (us);
    if (!g_getenv ("WALPE_WEB_NO_RAF_LIMIT")) {
        char *rl = raf_limit_js (sh->fps);
        us = webkit_user_script_new (rl, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                     WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
        webkit_user_content_manager_add_script (ucm, us);
        webkit_user_script_unref (us);
        g_free (rl);
    }
    if (timing_on ()) {
        /* WALPE_WEB_TIMING: the page's own requestAnimationFrame rate */
        us = webkit_user_script_new (
            "(function(){var n=0,a=0,am=0,t0=performance.now();function f(){n++;requestAnimationFrame(f);}requestAnimationFrame(f);"
            "var o=window.__walpeAudio;if(o)window.__walpeAudio=function(x){var t=performance.now();o(x);am+=performance.now()-t;a++;};"
            "setInterval(function(){var t=performance.now(),d=(t-t0)/1000;t0=t;"
            "try{window.webkit.messageHandlers.walpe.postMessage('raf:'+(n/d).toFixed(1)+' '+(a/d).toFixed(1)+' '+(a?am/a:0).toFixed(2));}catch(e){}"
            "n=0;a=0;am=0;},2000);})();",
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
        webkit_user_content_manager_add_script (ucm, us);
        webkit_user_script_unref (us);
    }
    if (g_getenv ("WALPE_WEB_CONSOLE")) {
        /* WALPE_WEB_CONSOLE=1: the page's console output, script errors and
         * the button / wheel events it receives, in the
         * daemon log ("web console: ...") */
        us = webkit_user_script_new (
            "(function(){function p(k,a){try{window.webkit.messageHandlers.walpe.postMessage('log:'+k+' '+"
            "Array.prototype.map.call(a,function(x){try{return typeof x==='string'?x:JSON.stringify(x);}"
            "catch(e){return String(x);}}).join(' '));}catch(e){}}"
            "['log','info','warn','error'].forEach(function(k){var o=console[k];"
            "console[k]=function(){p(k,arguments);return o.apply(console,arguments);};});"
            "window.addEventListener('error',function(e){p('error',[e.message+' ('+(e.filename||'')+':'+e.lineno+')']);});"
            "['mousedown','mouseup','click','dblclick','contextmenu','auxclick','wheel'].forEach(function(t){"
            "window.addEventListener(t,function(e){var g=e.target,c;"
            "c=g&&g.nodeName?g.nodeName.toLowerCase()+(g.id?'#'+g.id:'')+(typeof g.className==='string'&&g.className?'.'+g.className.split(' ')[0]:''):'?';"
            "p('input',[t,e.clientX+','+e.clientY,'button='+e.button,'detail='+e.detail,t==='wheel'?'deltaY='+e.deltaY:'',"
            "e.isTrusted?'trusted':'script','on',c]);},true);});})();",
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
        webkit_user_content_manager_add_script (ucm, us);
        webkit_user_script_unref (us);
    }
    /* jquery.backstretch (the slideshow of 884307090, 1271777873,
     * 1509243786) puts an <img> with z-index -999999 into a fixed wrapper
     * with z-index -999999. WebKitGTK 2.52 composites a stacking context
     * that is a negative z-order child and has negative z-order children
     * even with accelerated compositing off (RenderLayerCompositor's
     * speculative overlap container), the page enters compositing mode,
     * and the UI side - this process - dereferences the accelerated
     * backing store it does not have (main() disables WebKit's DMA-BUF
     * renderer because it aborts on NVIDIA): the daemon died as soon as a
     * slideshow had a picture to show. Equal z-index siblings paint in
     * document order either way, so "auto" inside the wrapper looks the
     * same. Other pages that nest negative z-index are not covered. */
    {
        WebKitUserStyleSheet *css = webkit_user_style_sheet_new (
            ".backstretch img,.backstretch video,.backstretch iframe,.backstretch .backstretch-item"
            "{z-index:auto !important}",
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
        webkit_user_content_manager_add_style_sheet (ucm, css);
        webkit_user_style_sheet_unref (css);
    }
    webkit_user_content_manager_register_script_message_handler (ucm, "walpe", NULL);
    g_signal_connect (ucm, "script-message-received::walpe", G_CALLBACK (on_script_message), sh);
    /* a wallpaper's media start by themselves, with their sound (see "the
     * page's sound" above): nobody clicks a wallpaper to start its music */
    WebKitWebsitePolicies *pol = webkit_website_policies_new_with_policies ("autoplay", WEBKIT_AUTOPLAY_ALLOW, NULL);
    sh->view = WEBKIT_WEB_VIEW (g_object_new (WEBKIT_TYPE_WEB_VIEW, "web-context", web_ctx,
                                              "network-session", ns,
                                              "user-content-manager", ucm,
                                              "website-policies", pol, NULL));
    g_object_unref (pol);
    g_object_unref (ns);
    g_object_unref (ucm);
    /* how loud, before the page loads anything */
    sh->snd_q16 = -1;
    sh_sound_own_load (sh);
    sh_sound_apply (sh);
    if (!sound_pages)
        sound_pages = g_ptr_array_new ();
    g_ptr_array_add (sound_pages, sh);
    webkit_web_view_set_background_color (sh->view, &(GdkRGBA) { 0, 0, 0, 0 });
    WebKitSettings *st = webkit_web_view_get_settings (sh->view);
    gboolean files = use_file_urls ();
    g_object_set (st,
                  "enable-javascript", TRUE,
                  "enable-webgl", TRUE,
                  "enable-developer-extras", FALSE,
                  /* (WebKit's default; stated because the network switch
                   * relies on it: peer connections are UDP, no proxy sees
                   * them) */
                  "enable-webrtc", FALSE,
                  /* no universal access: a page must not read arbitrary
                   * local files; file access only in the opt-in fallback */
                  "allow-universal-access-from-file-urls", FALSE,
                  "allow-file-access-from-file-urls", files,
                  NULL);
    g_signal_connect (sh->view, "load-changed", G_CALLBACK (on_load_changed), sh);
    g_signal_connect (sh->view, "script-dialog", G_CALLBACK (on_script_dialog), sh);
    g_signal_connect (sh->view, "web-process-terminated", G_CALLBACK (on_web_process_terminated), sh);

    gboolean bigwin = web_bigwin ();
    sh->win = gtk_window_new ();
    gtk_window_set_decorated (GTK_WINDOW (sh->win), FALSE);
    gtk_window_set_title (GTK_WINDOW (sh->win), "walpe web (offscreen)");
    sh->box = walpe_web_host_new (GTK_WIDGET (sh->view), w, h, bigwin);
    WALPE_WEB_HOST (sh->box)->paint = sh_on_paint;
    WALPE_WEB_HOST (sh->box)->user = sh;
    gtk_window_set_child (GTK_WINDOW (sh->win), sh->box);
    if (bigwin) {
        gtk_window_set_default_size (GTK_WINDOW (sh->win), w, h);
    } else {
        gtk_window_set_default_size (GTK_WINDOW (sh->win), 1, 1);
        /* GtkWindow clips picking to its own box: page coordinates are
         * all outside a 1 x 1 window, and every injected pointer event
         * went to the window instead of the view */
        gtk_widget_set_overflow (sh->win, GTK_OVERFLOW_VISIBLE);
    }
    gtk_widget_realize (sh->win);
    hide_window_offscreen (sh->win, w, h);
    gtk_window_present (GTK_WINDOW (sh->win));
    sh->view_w = w;
    sh->view_h = h;
    if (timing_on ()) {
        GdkFrameClock *fc = gtk_widget_get_frame_clock (sh->win);
        if (fc) {
            g_signal_connect (fc, "before-paint", G_CALLBACK (timing_fc_before), NULL);
            g_signal_connect (fc, "after-paint", G_CALLBACK (timing_fc_after), NULL);
        }
    }
    sh->input = web_input_new (sh->win, sh->view, sh->path);

    /* base = the wallpaper's own files: all the main frame may navigate to */
    char *uri, *base;
    if (files) {
        uri = g_filename_to_uri (sh->html, NULL, NULL);
        char *dir = g_filename_to_uri (sh->path, NULL, NULL);
        base = g_strconcat (dir ? dir : "file://", "/", NULL);
        g_free (dir);
    } else {
        char *rel = g_path_get_basename (sh->html);
        char *esc = g_uri_escape_string (rel, NULL, FALSE);
        if (!use_scheme () && http_start ())
            base = g_strdup_printf ("http://127.0.0.1:%u/%s/", http_port, sh->host);
        else
            base = g_strdup_printf (WEB_SCHEME "://%s/", sh->host);
        uri = g_strconcat (base, esc, NULL);
        g_free (rel);
        g_free (esc);
    }
    web_input_set_base (sh->input, base);
    g_free (base);
    media_check_start (sh->path);
    webkit_web_view_load_uri (sh->view, uri);
    /* not the URI: its token is all that guards the loopback file server
     * (user-chosen folders included) and logs get read by others */
    g_message ("web: %s (%dx%d, %s <= %d/s)", sh->html ? sh->html : sh->path, w, h,
               use_snapshots () ? "snapshots" : "captures on repaint", sh->fps);
    g_free (uri);
    /* (replaced by the shorter one when the page has finished loading) */
    if (!sh->first_done)
        timer_set (&sh->first_id, TRUE, WEB_FIRST_FRAME_S * 1000, sh_first_end, sh);
    sh_state_main (sh);
    return G_SOURCE_REMOVE;
}

static gboolean sh_destroy_main (gpointer user)
{
    WebShared *sh = user;
    sh->dead = TRUE;            /* main thread only */
    if (sh->gave_up)            /* its "keeps crashing" note goes with it */
        page_error_set (NULL);
    if (probe_waiting)          /* still waiting for the start-up test */
        g_ptr_array_remove (probe_waiting, sh);
    sh_timers_stop (sh);
    g_cancellable_cancel (sh->cancel);
    sh_audio_update (sh, FALSE);
    sh_media_update (sh, FALSE);
    if (sh->win) {
        WALPE_WEB_HOST (sh->box)->paint = NULL;
        web_input_free (sh->input);
        sh->input = NULL;
        g_signal_handlers_disconnect_by_data (webkit_web_view_get_user_content_manager (sh->view), sh);
        g_signal_handlers_disconnect_by_data (sh->view, sh);
        /* destroying the view left its WebProcess running (WebKit does not
         * reuse it: every later web wallpaper started another one, ~150 MB
         * here, ~370 MB on an NVIDIA machine). No monitor shows this page
         * any more (a re-open of the same wallpaper keeps the page: the new
         * viewer attaches before the old one is freed), so end it now */
        webkit_web_view_terminate_web_process (sh->view);
        g_ptr_array_remove_fast (sound_pages, sh);
        g_clear_pointer (&sh->snd_script, webkit_user_script_unref);
        gtk_window_destroy (GTK_WINDOW (sh->win)); /* owns the host and the view */
        sh->win = NULL;
        sh->box = NULL;
        sh->view = NULL;
    }
    if (!sh->in_flight)
        sh_free (sh);           /* else snapshot_cb frees it */
    return G_SOURCE_REMOVE;
}

/* the page is kept when the same wallpaper is set again (property
 * changes reopen it): hand it the current values like WE's
 * applyUserProperties on a settings change (the old viewer's page never
 * saw `walpe-ctl setprop` values) */
static gboolean sh_reprops_main (gpointer user)
{
    WebShared *sh = user;
    if (!sh->dead && sh->view) {
        /* its own sound switch and volume are options too (`resetprops`
         * takes them away without saying so to the page) */
        sh_sound_own_load (sh);
        sh_sound_apply (sh);
    }
    if (!sh->dead && sh->view &&
        sh_apply_network (sh, webkit_web_view_get_network_session (sh->view)) && !sh->gave_up) {
        /* the network option changed: the page starts over under the new
         * rule (open connections and what it already fetched go with it) */
        webkit_web_view_reload (sh->view);
        return G_SOURCE_REMOVE;
    }
    if (!sh->dead && sh->loaded)
        sh_push_properties (sh, FALSE);   /* only what changed */
    else if (!sh->dead && sh->gave_up && sh->view) {
        /* the page was given up after repeated crashes and the user opens
         * the wallpaper again: a fresh start, WebGL included */
        g_message ("web: %s opened again after its page crashed: reloading it", sh->path);
        sh->gave_up = FALSE;
        sh->crashes = 0;
        sh->msg_n = 0;
        sh->msg_big = 0;
        page_error_set (NULL);
        if (sh->webgl_off) {
            webkit_settings_set_enable_webgl (webkit_web_view_get_settings (sh->view), TRUE);
            sh->webgl_off = FALSE;
        }
        webkit_web_view_reload (sh->view);
    }
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------- pointer (main thread) */

/* the page shown by this viewer (a monitor's request_user), or NULL */
static WebShared *sh_by_viewer (gpointer viewer)
{
    WebShared *found = NULL;
    g_mutex_lock (&reg_lock);
    if (registry) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init (&it, registry);
        while (!found && g_hash_table_iter_next (&it, &k, &v)) {
            WebShared *sh = v;
            g_mutex_lock (&sh->lock);
            for (guint i = 0; i < sh->viewers->len; i++)
                if (g_array_index (sh->viewers, WebViewerRef, i).user == viewer)
                    found = sh;
            g_mutex_unlock (&sh->lock);
        }
    }
    g_mutex_unlock (&reg_lock);
    return found;   /* pages are only freed on this (the main) thread */
}

static gboolean sh_takes_input (WebShared *sh)
{
    return sh->loaded && !sh->dead && sh->view && sh->input && !sh->suspended;
}

static void sh_input_deliver (WebShared *sh, const WalpeWebInput *in)
{
    if (in->mon_w < 1 || in->mon_h < 1 || sh->view_w < 1 || sh->view_h < 1)
        return;
    /* monitor px -> page px: the page is drawn "cover" (web_gl_render),
     * i.e. scaled by k about the centre; 1:1 on the monitor it was sized
     * for */
    double k = MAX ((double) in->mon_w / sh->view_w, (double) in->mon_h / sh->view_h);
    double x = sh->view_w / 2.0 + (in->x - in->mon_w / 2.0) / k;
    double y = sh->view_h / 2.0 + (in->y - in->mon_h / 2.0) / k;
    switch (in->kind) {
    case WALPE_WEB_IN_MOVE:
        web_input_move (sh->input, x, y, in->time);
        break;
    case WALPE_WEB_IN_DOWN:
    case WALPE_WEB_IN_UP:
        web_input_button (sh->input, in->button, in->kind == WALPE_WEB_IN_DOWN, x, y, in->time);
        break;
    case WALPE_WEB_IN_SCROLL:
        web_input_scroll (sh->input, in->dx, in->dy, x, y, in->time);
        break;
    default:
        break;
    }
}

/* a new document, or the page is shown again: the daemon only reports the
 * pointer when it moves, so tell the page where it already is */
static void sh_input_replay (WebShared *sh)
{
    if (!sh->in_have || !sh_takes_input (sh))
        return;
    web_input_refresh (sh->input);
    sh_input_deliver (sh, &sh->in_last);
}

/* pointer input on one monitor, for the page that monitor shows: each page
 * only gets what happened on a monitor showing it. A page on several
 * monitors follows the monitor the pointer is on. */
void walpe_web_input (gpointer viewer, const WalpeWebInput *in)
{
    /* the keyboard's layout and modifier state belong to no page */
    if (in->kind == WALPE_WEB_IN_KEYMAP) {
        int fd = in->keymap_fd >= 0 ? dup (in->keymap_fd) : -1;
        web_input_keymap (fd, in->keymap_size);
        return;
    }
    if (in->kind == WALPE_WEB_IN_MODIFIERS) {
        web_input_modifiers (in->mods);
        return;
    }
    WebShared *sh = sh_by_viewer (viewer);
    if (!sh || sh->dead)
        return;
    if (in->kind == WALPE_WEB_IN_KBD_FOCUS || in->kind == WALPE_WEB_IN_KEY) {
        if (!sh_takes_input (sh))
            return;
        if (in->kind == WALPE_WEB_IN_KBD_FOCUS)
            web_input_keyboard_focus (sh->input, in->pressed);
        else
            web_input_key (sh->input, in->key, in->pressed, in->time);
        return;
    }
    if (in->kind == WALPE_WEB_IN_LEAVE) {
        if (sh->in_owner == viewer) {   /* not when another monitor took it over */
            sh->in_owner = NULL;
            sh->in_have = FALSE;
            if (sh_takes_input (sh))
                web_input_leave (sh->input, in->time);
        }
        return;
    }
    if (in->kind != WALPE_WEB_IN_UP)
        sh->in_owner = viewer;
    if (sh->in_owner == viewer) {
        sh->in_last = *in;
        sh->in_last.kind = WALPE_WEB_IN_MOVE;
        sh->in_last.time = 0;
        sh->in_have = TRUE;
    }
    if (sh_takes_input (sh)) {
        sh_input_deliver (sh, in);
        /* an audio listener may draw with the pointer (visualizers that
         * follow it): keep calling it for a while, silence or not */
        if (sh->audio_ref)
            sh_audio_wake (sh);
    }
}

/* how pointer input reaches the live pages: "native", "native
 * (unconfirmed)", "js", "off"; "" = no page; g_free */
/* where pages live, for status: "x11", "private", "" = no page yet. A
 * host that is not the first choice has the reason after its name:
 * "private (the X11 page host did not come up)" */
char *walpe_web_host (void)
{
    WebHost h = (WebHost) g_atomic_int_get (&web_host_shown);
#ifdef WEB_HAVE_PRIVATE
    const char *note = g_atomic_pointer_get (&web_host_note);
    if (h != WEB_HOST_NONE && note)
        return g_strdup_printf ("%s (%s)", web_host_name (h), note);
#endif
    return g_strdup (h == WEB_HOST_NONE ? "" : web_host_name (h));
}

char *walpe_web_input_status (void)
{
    GString *s = g_string_new (NULL);
    g_mutex_lock (&reg_lock);
    if (registry) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init (&it, registry);
        while (g_hash_table_iter_next (&it, &k, &v)) {
            WebShared *sh = v;
            const char *m = sh->input ? web_input_mode_name (sh->input) : NULL;
            if (m && !strstr (s->str, m))
                g_string_append_printf (s, "%s%s", s->len ? ", " : "", m);
        }
    }
    g_mutex_unlock (&reg_lock);
    return g_string_free (s, FALSE);
}

/* --------------------------------------------------- render-thread side */

static gboolean web_gl_init (WalpeContent *c, GError **err)
{
    (void) err;
    WebViewer *v = (WebViewer *) c;
    walpe_glutil_init ();
    glGenTextures (1, &v->tex);
    glBindTexture (GL_TEXTURE_2D, v->tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* attach for wake-ups now that request_redraw is set */
    WebShared *sh = v->sh;
    g_mutex_lock (&sh->lock);
    WebViewerRef r = { c->request_redraw, c->request_user, FALSE };
    g_array_append_val (sh->viewers, r);
    sh->active++;
    g_mutex_unlock (&sh->lock);
    g_idle_add (sh_state_main, sh);
    return TRUE;
}

static gboolean web_gl_render (WalpeContent *c, int w, int h, float px, float py)
{
    (void) px; (void) py;
    WebViewer *v = (WebViewer *) c;
    WebShared *sh = v->sh;
    /* the size the page is laid out and painted at for this monitor: the
     * one chosen for its picture (content.h render_w / render_h), else the
     * surface's. The page's texture is drawn over the whole surface either
     * way, and pointer positions are mapped from the monitor to the page
     * (walpe_web_input). */
    const int pw = c->render_w > 0 && c->render_h > 0 ? c->render_w : w;
    const int ph = c->render_w > 0 && c->render_h > 0 ? c->render_h : h;
    if (pw != v->last_w || ph != v->last_h) {
        v->last_w = pw;
        v->last_h = ph;
        g_mutex_lock (&sh->lock);
        gboolean grew = pw > sh->want_w || ph > sh->want_h;
        sh->want_w = MAX (sh->want_w, pw);
        sh->want_h = MAX (sh->want_h, ph);
        g_mutex_unlock (&sh->lock);
        if (grew)
            g_idle_add (sh_state_main, sh);   /* the page gets that size */
    }
    int bw = 0, bh = 0;
    /* take the newest frame under the lock, upload it outside. Only what
     * changed since the frame this texture holds is uploaded (the damage
     * of every frame in between, sh_publish): a clock's digits or a small
     * animation no longer cost a full-frame upload on every monitor. */
    WebBuf *fr = NULL;
    int fw = 0, fh = 0;
    guint64 fseq = 0;
    int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
    gboolean full = TRUE;
    g_mutex_lock (&sh->lock);
    const gboolean first_done = sh->first_done;
    if (sh->px && sh->seq != v->drawn_seq) {
        fr = webbuf_ref (sh->px);
        fw = sh->pw;
        fh = sh->ph;
        fseq = sh->seq;
        full = v->tex_w != fw || v->tex_h != fh || !v->drawn_seq || fseq < v->drawn_seq ||
               fseq - v->drawn_seq > WEB_DMG_N;
        for (guint64 q = v->drawn_seq + 1; !full && q <= fseq; q++) {
            const WebDmg *d = &sh->dmg[q % WEB_DMG_N];
            if (d->seq != q || d->x0 >= d->x1 || d->y0 >= d->y1) {
                full = TRUE;
            } else if (dx0 >= dx1) {
                dx0 = d->x0, dy0 = d->y0, dx1 = d->x1, dy1 = d->y1;
            } else {
                dx0 = MIN (dx0, d->x0), dy0 = MIN (dy0, d->y0);
                dx1 = MAX (dx1, d->x1), dy1 = MAX (dy1, d->y1);
            }
        }
    }
    g_mutex_unlock (&sh->lock);
    if (fr) {
        glBindTexture (GL_TEXTURE_2D, v->tex);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
        if (v->tex_w != fw || v->tex_h != fh) {
            walpe_gl_tex_image_bgra (fw, fh, NULL);
            v->tex_w = fw;
            v->tex_h = fh;
        }
        if (full || dx0 < 0 || dy0 < 0 || dx1 > fw || dy1 > fh)
            dx0 = 0, dy0 = 0, dx1 = fw, dy1 = fh;
        gint64 ut0 = walpe_lazy_get (&web_timing) > 0 ? g_get_monotonic_time () : 0;
        glPixelStorei (GL_UNPACK_ROW_LENGTH, fw);
        walpe_gl_tex_sub_image_bgra (dx0, dy0, dx1 - dx0, dy1 - dy0,
                                     fr->data + ((gsize) dy0 * fw + dx0) * 4);
        glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
        if (ut0) {
            glFinish ();   /* timing only: make the upload's cost visible */
            g_atomic_int_add (&wtu.up_us, (gint) (g_get_monotonic_time () - ut0));
            g_atomic_int_add (&wtu.up_kb, (gint) ((gint64) (dx1 - dx0) * (dy1 - dy0) * 4 / 1000));
            g_atomic_int_inc (&wtu.uploads);
        }
        webbuf_unref (fr);
        v->drawn_seq = fseq;
    }
    bw = v->tex_w;
    bh = v->tex_h;

    glClearColor (0, 0, 0, 1);
    glClear (GL_COLOR_BUFFER_BIT);
    if (v->drawn_seq && bw > 0 && bh > 0) {
        WalpeMat3 m;
        walpe_mat3_cover (&m, (float) bw, (float) bh, (float) w, (float) h);
        /* snapshot pixels are already premultiplied */
        walpe_glutil_draw_quad (v->tex, TRUE, 1.0f, m.m, WALPE_BLEND_NORMAL);
    }
    /* no frame of the page yet: the daemon keeps what is on screen and
     * paints the first one when it arrives, also on a paused monitor. And
     * the ones after it while the page draws its first picture
     * (sh_first_frame_wanted): the frame taken when loading ended is
     * often an empty page.
     * That second part on a paused monitor only. While the flag is up the
     * daemon presents nothing (content.h), so a monitor that runs was kept
     * on the wallpaper before for that time too: every page came a second
     * later than it used to, and what it drew in its first second - the
     * start of a fade, an intro - never reached the screen. A monitor that
     * runs gets every frame anyway; it needs no first picture kept for it. */
    c->first_frame_pending = !v->drawn_seq || (!first_done && v->paused);
    return TRUE;
}

static void web_gl_free (WalpeContent *c)
{
    WebViewer *v = (WebViewer *) c;
    if (v->tex) {
        glDeleteTextures (1, &v->tex);
        v->tex = 0;
    }
}

static void web_set_paused (WalpeContent *c, gboolean p)
{
    WebViewer *v = (WebViewer *) c;
    if (p == v->paused)
        return;
    v->paused = p;
    WebShared *sh = v->sh;
    gboolean edge = FALSE;
    g_mutex_lock (&sh->lock);
    for (guint i = 0; i < sh->viewers->len; i++) {
        WebViewerRef *r = &g_array_index (sh->viewers, WebViewerRef, i);
        if (r->user == c->request_user) {
            r->paused = p;
            sh->active += p ? -1 : 1;
            edge = sh->active == (p ? 0 : 1);
        }
    }
    g_mutex_unlock (&sh->lock);
    if (edge)   /* the last one paused / the first one resumed */
        g_idle_add (sh_state_main, sh);
}

/* render thread: live fps setting (walpe-ctl set fps=N / the GUI) */
static void web_set_fps (WalpeContent *c, int fps)
{
    WebShared *sh = ((WebViewer *) c)->sh;
    g_atomic_int_set (&sh->fps_want, CLAMP (fps, 1, WEB_MAX_FPS));
    g_idle_add (sh_fps_main, sh);
}

/* render thread: detach; the last viewer posts the WebKit teardown to main */
static void web_free (WalpeContent *c)
{
    WebViewer *v = (WebViewer *) c;
    WebShared *sh = v->sh;
    gboolean last;
    g_mutex_lock (&reg_lock);
    g_mutex_lock (&sh->lock);
    for (guint i = 0; i < sh->viewers->len; i++) {
        WebViewerRef *r = &g_array_index (sh->viewers, WebViewerRef, i);
        if (r->user == c->request_user) {
            if (!r->paused)
                sh->active--;
            g_array_remove_index_fast (sh->viewers, i);
            break;
        }
    }
    last = --sh->refs == 0;
    if (last && registry && g_hash_table_lookup (registry, sh->origin) == sh)
        g_hash_table_remove (registry, sh->origin);
    g_mutex_unlock (&sh->lock);
    g_mutex_unlock (&reg_lock);
    if (last) /* idles run FIFO: after sh_create_main / pending state changes */
        g_idle_add (sh_destroy_main, sh);
    else      /* the viewers left may all be paused */
        g_idle_add (sh_state_main, sh);
    g_free (c->path);
    g_free (c->title);
    g_free (v);
}

/* ------------------------------------------------------------ constructor */

WalpeContent *walpe_web_new (const char *path, const WalpeLoadOpts *opts, GError **err)
{
    if (!g_file_test (path, G_FILE_TEST_IS_DIR)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "web wallpaper dir not found: %s", path);
        return NULL;
    }
    char *html = NULL;
    char *pjp = g_build_filename (path, "project.json", NULL);
    JsonNode *pj = wpj_parse_file (pjp);
    g_free (pjp);
    const char *file = pj && json_node_get_node_type (pj) == JSON_NODE_OBJECT
                           ? wpj_str (json_node_get_object (pj), "file", NULL) : NULL;
    if (file && !strchr (file, '/') && strcmp (file, ".."))
        html = g_build_filename (path, file, NULL);
    if (pj)
        json_node_unref (pj);
    const char *cands[] = { "index.html", "index.htm", NULL };
    for (int i = 0; cands[i] && (!html || !g_file_test (html, G_FILE_TEST_IS_REGULAR)); i++) {
        g_free (html);
        html = g_build_filename (path, cands[i], NULL);
    }
    if (!g_file_test (html, G_FILE_TEST_IS_REGULAR)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "index.html not found in %s", path);
        g_free (html);
        return NULL;
    }

    /* one page per wallpaper AS OPENED: a preset of this page is another
     * page (other property values) */
    const char *origin = opts && opts->origin_dir ? opts->origin_dir : path;
    g_mutex_lock (&reg_lock);
    if (!registry)
        registry = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    WebShared *sh = g_hash_table_lookup (registry, origin);
    gboolean created = FALSE;
    if (!sh) {
        sh = g_new0 (WebShared, 1);
        g_mutex_init (&sh->lock);
        sh->viewers = g_array_new (FALSE, FALSE, sizeof (WebViewerRef));
        sh->path = g_strdup (path);
        sh->origin = g_strdup (origin);
        if (opts && opts->preset) {
            JsonNode *pn = json_node_new (JSON_NODE_OBJECT);
            json_node_set_object (pn, opts->preset);
            JsonNode *cp = json_node_copy (pn);
            sh->preset = json_object_ref (json_node_get_object (cp));
            json_node_unref (cp);
            json_node_unref (pn);
            sh->preset_dir = g_strdup (opts->preset_dir);
        }
        sh->html = g_steal_pointer (&html);
        /* unguessable: also the loopback server's path token */
        guint32 r[4];
        web_random_bytes (r, sizeof r);   /* kernel CSPRNG */
        sh->host = g_strdup_printf ("%08x%08x%08x%08x", r[0], r[1], r[2], r[3]);
        int fps = opts && opts->fps > 0 ? opts->fps : 30;
        sh->fps = CLAMP (fps, 1, WEB_MAX_FPS);
        sh->cancel = g_cancellable_new ();
        g_hash_table_insert (registry, g_strdup (origin), sh);
        created = TRUE;
    }
    g_mutex_lock (&sh->lock);
    sh->refs++;
    g_mutex_unlock (&sh->lock);
    g_mutex_unlock (&reg_lock);
    g_free (html);
    if (created)
        g_idle_add (sh_create_main, sh); /* GTK/WebKit only on the main thread */
    else
        g_idle_add (sh_reprops_main, sh); /* reopened (setprop): new values */

    WebViewer *v = g_new0 (WebViewer, 1);
    v->sh = sh;
    v->base.type = WALPE_TYPE_WEB;
    v->base.path = g_strdup (path);
    v->base.title = g_path_get_basename (path);
    v->base.gl_init = web_gl_init;
    v->base.gl_render = web_gl_render;
    v->base.gl_free = web_gl_free;
    v->base.set_paused = web_set_paused;
    v->base.set_fps = web_set_fps;
    v->base.free = web_free;
    return &v->base;
}

void walpe_web_stats (guint *snapshots)
{
    guint n = 0;
    g_mutex_lock (&reg_lock);
    if (registry) {
        GHashTableIter it;
        gpointer k, val;
        g_hash_table_iter_init (&it, registry);
        while (g_hash_table_iter_next (&it, &k, &val))
            n += ((WebShared *) val)->snapshots;
    }
    g_mutex_unlock (&reg_lock);
    *snapshots = n;
}

/* walpe-web.so entry point (modules.h): everything above is module-local */
WALPE_API const WalpeWebModule *walpe_web_module_entry (void)
{
    static const WalpeWebModule m = {
        WALPE_MODULE_ABI, walpe_web_new, walpe_web_input, walpe_web_stats,
        walpe_web_media_missing, walpe_web_error, walpe_web_probe_run,
        walpe_web_input_status, walpe_web_info, walpe_web_host,
        walpe_web_sound_level, walpe_web_sound_page,
    };
    return &m;
}
