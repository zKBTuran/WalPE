/* media.h - Wallpaper Engine "Media Integration" on Linux via MPRIS.
 *
 * WE reads the OS "now playing" session (title/artist/album, album art,
 * playback state, timeline) and hands it to scene scripts
 * (mediaStatusChanged / mediaPlaybackChanged / mediaPropertiesChanged /
 * mediaThumbnailChanged / mediaTimelineChanged, system textures
 * $mediaThumbnail / $mediaPreviousThumbnail) and to web wallpapers
 * (wallpaperRegisterMedia*Listener). On Linux the equivalent is MPRIS 2:
 * every player owns a well-known D-Bus name org.mpris.MediaPlayer2.<x> on
 * the session bus with an org.mpris.MediaPlayer2.Player object at
 * /org/mpris/MediaPlayer2.
 *
 * Like audio.h, this runs ONLY while a consumer holds a reference (a
 * scene that uses media integration, from its load to its end; a web page
 * with a media listener while it is visible and not paused). A paused
 * scene keeps its reference and rests (walpe_media_rest). The first
 * reference starts a "walpe-media" thread with its own main context, which
 * subscribes to NameOwnerChanged / PropertiesChanged / Seeked (no polling
 * except a 1 s timeline tick and a 5 s Position resync while something is
 * playing); the last unref drops the subscriptions after a short grace
 * period (wallpaper switches don't churn the bus). Consumers poll the
 * cheap walpe_media_serial() once per frame and copy the snapshot with
 * walpe_media_get() when it changed. Thread-safe.
 *
 * Active player: the most recently started Playing player; else the most
 * recently active Paused one; else none (state STOPPED, empty properties,
 * no thumbnail - WE's behaviour with no media session).
 * Album art: mpris:artUrl file:// (and http(s):// when built with
 * libsoup), decoded with gdk-pixbuf, scaled to <= 640 px; the five WE
 * thumbnail colours are derived from it. WALPE_NO_MEDIA=1 disables the
 * integration (status enabled = false). */
#pragma once
#include <glib.h>
#include "common/api.h"

enum { WALPE_MEDIA_STOPPED = 0, WALPE_MEDIA_PLAYING = 1, WALPE_MEDIA_PAUSED = 2 };

typedef struct {
    guint64 serial;              /* bumps on any change below */
    /* per-event change counters (compare with the last copy) */
    guint status_serial, props_serial, thumb_serial, playback_serial, timeline_serial;
    gboolean enabled;            /* media integration available */
    int state;                   /* WALPE_MEDIA_* */
    /* MediaPropertiesEvent (never NULL after walpe_media_get) */
    char *title, *artist, *subtitle, *album_title, *album_artist, *genres, *content_type;
    /* MediaThumbnailEvent: straight RGBA8, tightly packed (refcounted) */
    gboolean has_thumbnail;
    GBytes *thumb;
    int thumb_w, thumb_h;
    GBytes *prev_thumb;          /* $mediaPreviousThumbnail (may be NULL) */
    int prev_w, prev_h;
    char *thumb_data_url;        /* "data:image/...;base64,..." for web pages */
    float primary[3], secondary[3], tertiary[3], text[3], high_contrast[3];
    /* MediaTimelineEvent, seconds (position extrapolated at get time) */
    double position, duration;
    char *player;                /* bus name of the active player (diagnostics) */
} WalpeNowPlaying;

WALPE_API void walpe_media_ref (void);
WALPE_API void walpe_media_unref (void);
/* A holder of a reference draws nothing for now (TRUE: a paused scene) or
 * is drawn again (FALSE); calls pair up, and a holder that rests ends its
 * rest before it gives its reference back. While every holder rests the
 * once-a-second timeline tick and the Position request every 5 s stop -
 * the media thread then wakes only when a player says something, which is
 * still taken in, so that the first frame afterwards is right. */
void walpe_media_rest (gboolean rest);
/* any thread; 0 until the first snapshot */
WALPE_API guint64 walpe_media_serial (void);
/* copy the current snapshot (always fills strings); clear with walpe_media_now_clear */
WALPE_API void walpe_media_get (WalpeNowPlaying *out);
WALPE_API void walpe_media_now_clear (WalpeNowPlaying *info);
/* "off" | "idle (no player)" | "<player>: playing" | "disabled ..." */
char *walpe_media_status (void);

/* for tests (no session bus; not while media integration runs): treat url
 * as a player's cover art URL - the same read, limits, decode and cache -
 * and wait for the outcome. TRUE: a thumbnail of *w x *h was published.
 * *cached / *cached_bytes: the decoded covers the cache holds afterwards
 * and what they weigh. url NULL drops the cache. */
gboolean walpe_media_art_probe (const char *url, int *w, int *h, guint *cached, gsize *cached_bytes);
/* for tests: wait until the jobs of all decodes are gone. A job keeps its
 * own reference to the picture it decoded until its worker thread has let
 * go of it, which can be a moment after walpe_media_art_probe () returned.
 * FALSE: that took longer than max_ms. */
gboolean walpe_media_art_settled (guint max_ms);

/* pure helpers, exposed for tests: WE thumbnail colours from straight RGBA */
void walpe_media_colors (const guint8 *rgba, int w, int h, float primary[3], float secondary[3],
                         float tertiary[3], float text[3], float high_contrast[3]);
