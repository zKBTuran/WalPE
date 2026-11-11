/* platform.c - the functions of platform.h: each one goes to the backend
 * that is open (impl.h). (The output-list helpers, which need no backend,
 * are in outputs.c.) */
#include "walpe-build-config.h"
#include "platform/impl.h"
#include <string.h>

static const WalpePlatformOps *ops;     /* the open backend, NULL = none */

gboolean walpe_platform_built (WalpeBackend b)
{
#ifdef HAVE_X11
    if (b == WALPE_BACKEND_X11)
        return TRUE;
#endif
#ifdef HAVE_WAYLAND
    if (b == WALPE_BACKEND_WAYLAND)
        return TRUE;
#endif
    (void) b;
    return FALSE;
}

void walpe_platform_probe_wayland (WalpePlatformProbe *out)
{
    memset (out, 0, sizeof (*out));
#ifdef HAVE_WAYLAND
    walpe_wayland_probe (out);
#endif
}

gboolean walpe_platform_open (WalpeBackend backend, const WalpePlatformOpts *opts, char **err)
{
    const WalpePlatformOps *want = NULL;
    if (err)
        *err = NULL;
    g_return_val_if_fail (ops == NULL, FALSE);
#ifdef HAVE_X11
    if (backend == WALPE_BACKEND_X11)
        want = &walpe_x11_ops;
#endif
#ifdef HAVE_WAYLAND
    if (backend == WALPE_BACKEND_WAYLAND)
        want = &walpe_wayland_ops;
#endif
    if (!want) {
        if (err)
            *err = g_strdup_printf ("this build has no %s backend", walpe_backend_name (backend));
        return FALSE;
    }
    char *why = NULL;
    if (!want->open (opts, &why)) {
        if (err)
            *err = why ? why : g_strdup_printf ("the %s display could not be opened", want->name);
        else
            g_free (why);
        return FALSE;
    }
    g_free (why);
    ops = want;
#ifdef HAVE_WAYLAND
    /* a connection the probe kept for the Wayland backend, which was not
     * the one opened: nobody else would ever close it */
    if (backend != WALPE_BACKEND_WAYLAND)
        walpe_wayland_probe_release ();
#endif
    return TRUE;
}

WalpeBackend walpe_platform_backend (void)
{
    return ops ? ops->backend : WALPE_BACKEND_AUTO;
}

char *walpe_platform_describe (void)
{
    return ops && ops->describe ? ops->describe () : g_strdup ("");
}

void walpe_platform_start (gboolean layout_events, const WalpeSignalSink *sink, gpointer sink_user)
{
    g_return_if_fail (ops != NULL);
    ops->start (layout_events, sink, sink_user);
}

void walpe_platform_settle (void)
{
    if (ops && ops->settle)
        ops->settle ();
}

void walpe_platform_close (void)
{
    if (!ops)
        return;
    walpe_signals_stop ();
    ops->close ();
    ops = NULL;
}

void walpe_platform_sync (void)
{
    if (ops && ops->sync)
        ops->sync ();
}

/* What a backend lists is what the window system told it, and the daemon
 * adds these numbers up (is the pointer inside x .. x + width?), sizes
 * things by them and prints them. They are kept in a range in which that
 * is safe whatever came over the wire: positions within +-2^24, sizes
 * 0 .. 65535 (the largest an X screen can be: nothing X11 reports is
 * changed), a refresh rate of 0 .. 1000 Hz, a scale of 0.01 .. 64, a
 * transform of 0 .. 7; what is outside reads as "unknown" (0 Hz, scale 1,
 * not turned) or is brought to the nearest end of the range. */
#define OUT_POS_MAX  (1 << 24)
#define OUT_SIZE_MAX 65535

static void output_list_check (WalpeOutputList *l)
{
    if (l->n < 0 || (l->n > 0 && !l->o))
        l->n = 0;
    for (int i = 0; i < l->n; i++) {
        WalpeOutput *o = &l->o[i];
        o->x = CLAMP (o->x, -OUT_POS_MAX, OUT_POS_MAX);
        o->y = CLAMP (o->y, -OUT_POS_MAX, OUT_POS_MAX);
        o->logical_x = CLAMP (o->logical_x, -OUT_POS_MAX, OUT_POS_MAX);
        o->logical_y = CLAMP (o->logical_y, -OUT_POS_MAX, OUT_POS_MAX);
        o->width = CLAMP (o->width, 0, OUT_SIZE_MAX);
        o->height = CLAMP (o->height, 0, OUT_SIZE_MAX);
        o->logical_width = CLAMP (o->logical_width, 0, OUT_SIZE_MAX);
        o->logical_height = CLAMP (o->logical_height, 0, OUT_SIZE_MAX);
        /* (written so that a NaN fails the test) */
        if (!(o->refresh_hz >= 0.0 && o->refresh_hz <= 1000.0))
            o->refresh_hz = 0.0;
        if (!(o->scale >= 0.01 && o->scale <= 64.0))
            o->scale = 1.0;
        if (o->transform < WALPE_TRANSFORM_NORMAL || o->transform > WALPE_TRANSFORM_FLIPPED_270)
            o->transform = WALPE_TRANSFORM_NORMAL;
    }
}

void walpe_platform_outputs (WalpeOutputList *out)
{
    memset (out, 0, sizeof (*out));
    if (ops) {
        ops->outputs (out);
        output_list_check (out);
    }
}

EGLDisplay walpe_platform_egl_display (void)
{
    return ops ? ops->egl_display () : EGL_NO_DISPLAY;
}

const char *walpe_platform_display_node (void)
{
    return ops && ops->display_node ? ops->display_node () : NULL;
}

const char *walpe_platform_egl_note (void)
{
    return ops && ops->egl_note ? ops->egl_note () : "";
}

unsigned long walpe_platform_egl_visual (void)
{
    return ops && ops->egl_visual ? ops->egl_visual () : 0;
}

gboolean walpe_platform_egl_config_usable (EGLDisplay dpy, EGLConfig conf)
{
    return ops && ops->egl_config_usable ? ops->egl_config_usable (dpy, conf) : TRUE;
}

gboolean walpe_platform_paces (void)
{
    return ops && ops->paces;
}

WalpeSurface *walpe_surface_new (const WalpeSurfaceSpec *spec, EGLDisplay dpy, EGLConfig conf,
                                 EGLSurface *egl_surface, int *w, int *h)
{
    *egl_surface = EGL_NO_SURFACE;
    *w = *h = 0;
    return ops ? ops->surface_new (spec, dpy, conf, egl_surface, w, h) : NULL;
}

void walpe_surface_free (WalpeSurface *s)
{
    if (ops && s)
        ops->surface_free (s);
}

void walpe_surface_size (WalpeSurface *s, int *w, int *h)
{
    *w = *h = 0;
    if (ops && s)
        ops->surface_size (s, w, h);
}

gboolean walpe_surface_set_buffer_size (WalpeSurface *s, int w, int h)
{
    return ops && s && ops->surface_set_buffer_size ? ops->surface_set_buffer_size (s, w, h) : FALSE;
}

guint32 walpe_surface_output (WalpeSurface *s)
{
    return ops && s ? ops->surface_output (s) : 0;
}

unsigned long walpe_surface_native_id (WalpeSurface *s)
{
    return ops && s ? ops->surface_native_id (s) : 0;
}

void walpe_surface_want_pointer (WalpeSurface *s, gboolean want)
{
    if (ops && s && ops->surface_want_pointer)
        ops->surface_want_pointer (s, want);
}

/* The three hooks below run once per frame on the render threads. `ops`
 * is written by open / close only, on the main thread, before the render
 * threads start and after they ended. */
int walpe_surface_set_interval (WalpeSurface *s, int iv)
{
    return ops->surface_set_interval ? ops->surface_set_interval (s, iv) : iv;
}

void walpe_surface_pre_swap (WalpeSurface *s)
{
    if (ops->surface_pre_swap)
        ops->surface_pre_swap (s);
}

void walpe_surface_swapped (WalpeSurface *s)
{
    if (ops->surface_swapped)
        ops->surface_swapped (s);
}

gboolean walpe_surface_present_stats (WalpeSurface *s, WalpePresentStats *out)
{
    memset (out, 0, sizeof (*out));
    return ops && s && ops->surface_present_stats ? ops->surface_present_stats (s, out) : FALSE;
}

gboolean walpe_platform_pointer (WalpePointer *out)
{
    memset (out, 0, sizeof (*out));
    return ops ? ops->pointer (out) : FALSE;
}

gboolean walpe_platform_on_desktop (guintptr under)
{
    return ops && ops->on_desktop ? ops->on_desktop (under) : TRUE;
}

void walpe_platform_input_select (gboolean buttons, gboolean motion)
{
    if (ops && ops->input_select)
        ops->input_select (buttons, motion);
}

gboolean walpe_platform_has_input_events (void)
{
    return ops && ops->has_input_events ? ops->has_input_events () : ops != NULL;
}

gboolean walpe_platform_composited (void)
{
    return ops && ops->composited ? ops->composited () : FALSE;
}

void walpe_platform_present_log (void)
{
    if (ops && ops->present_log)
        ops->present_log ();
}

void walpe_platform_status (JsonBuilder *b)
{
    if (ops && ops->status)
        ops->status (b);
}
