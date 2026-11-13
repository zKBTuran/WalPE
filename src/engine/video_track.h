/* video_track.h - the sound track of a video wallpaper follows its picture
 *
 * The part of video.c that decides when the track's player (scenesound.h)
 * is told a place, apart from the video module so that it can be run
 * without one: video.c applies it with every frame it draws,
 * tests/scenesound_test.c runs it against the null device.
 *
 * The picture leads. Every frame that is drawn says where in the clip its
 * picture is; the track is told that place (walpe_sound_restart_at: a
 * short fade, the same stream) when it begins to be heard - the first
 * picture, sound switched on, the volume raised, the other monitor's
 * instance fell silent - and when the two are more than
 * WALPE_VTRACK_SLACK_S apart: a resume after another monitor played on, a
 * decoder that restarted somewhere else, or, after hours, the difference
 * between the monotonic clock and the sound device's. A clip whose length
 * is not known has its track started again with every pass.
 *
 * And the track plays from no other place. While it is not heard its
 * player is held (walpe_sound_hold), until the frame that tells it the
 * place: a player that is only silent goes on by itself, from where it
 * stopped, the moment its group gets the word - and for a track that is
 * the wrong place. That was heard: the same video on two monitors, both
 * paused (walpe-ctl pause, the display asleep): the monitor whose instance
 * spoke is paused first, the other instance has the word for the
 * milliseconds until its own pause arrives, and its player started a
 * thread, opened a stream and wrote 63 ms of the track from the place it
 * had last stopped at, at the track's level, over the end of the first -
 * in 6 of 12 pauses here, the stream closed again two seconds later. The
 * same at the end of a mute or when sound is switched on: up to a frame of
 * sound from the old place before the picture's place was told. (An
 * instance that is paused and resumes with the word goes on where it
 * stopped, as before: its picture stopped there too.) */
#pragma once
#include <glib.h>
#include <math.h>
#include "scenesound.h"

#define WALPE_VTRACK_SLACK_S 0.30
#define WALPE_VTRACK_AGAIN_US (G_USEC_PER_SEC)   /* not told a place more often (the player's
                                                  * answer to the last one is not in its place yet) */
/* the player's place is what it handed to its stream, which holds up to a
 * fifth of a second: on average it is this far ahead of what is heard */
#define WALPE_VTRACK_AHEAD_S 0.10

typedef struct {
    WalpeSoundGroup *group; /* NULL = no track: a program that plays no sound, or a
                             * video known to have none */
    WalpeSound *snd;
    double pos;             /* the picture's place when the track was last looked at, -1 = none */
    gboolean heard;         /* the track was heard then */
    gint64 set_us;          /* when the track was last told a place */
    double period;          /* the length of a pass the track loops with, 0 = none (started with every pass) */
} WalpeVideoTrack;

/* after a frame was drawn: pos = the place of its picture in the clip,
 * seconds (< 0: not known, nothing happens) */
static inline void walpe_video_track_follow (WalpeVideoTrack *t, double pos)
{
    if (!t->snd || pos < 0)
        return;
    const gboolean heard = walpe_sound_group_speaking (t->group);
    const gint64 now = g_get_monotonic_time ();
    const gboolean first = t->pos < 0;
    gboolean set = FALSE;
    if (first || (t->period <= 0 && pos < t->pos - WALPE_VTRACK_SLACK_S)) {
        set = TRUE;                         /* the first picture (no length known: every pass) */
    } else if (heard && !t->heard) {
        set = TRUE;                         /* it begins to be heard: from where the picture is */
    } else if (heard && now - t->set_us > WALPE_VTRACK_AGAIN_US && walpe_sound_is_playing (t->snd)) {
        WalpeSoundStats st;
        walpe_sound_stats (t->snd, &st);
        if (st.running && st.rate > 0) {
            double d = (double) st.pos / st.rate - WALPE_VTRACK_AHEAD_S - pos;
            if (t->period > 0)              /* (both are places in a pass: the nearer way round) */
                d -= t->period * round (d / t->period);
            set = fabs (d) > WALPE_VTRACK_SLACK_S;  /* apart: a resume, a decoder that started over */
        }
    }
    if (set) {
        walpe_sound_restart_at (t->snd, pos);
        t->set_us = now;
    }
    /* held while it is not heard (see above); let go after it was told the
     * place it begins to be heard at, so that it starts there */
    if (first || heard != t->heard)
        walpe_sound_hold (t->snd, !heard);
    t->pos = pos;
    t->heard = heard;
}
