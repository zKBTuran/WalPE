/* scenesound.h - sound layers of Wallpaper Engine scenes.
 *
 * A scene object with "sound": [files] plays them: "playbackmode" loop (the
 * files one after the other, for ever), random (a random file, then a pause
 * of mintime..maxtime seconds, again) or single (once); "volume";
 * "startsilent" (waits for a script's play ()). Scripts: play / pause /
 * stop / isPlaying and the layer's volume.
 *
 * WHEN SOMETHING IS HEARD. All of these have to hold:
 *  - the process allowed sound output (walpe_sound_allow: the daemon does,
 *    the settings window, the tools and the tests never do);
 *  - sound is switched on for the wallpaper: the setting "scene_sound"
 *    (walpe_sound_set_enabled; on in a new configuration), over which the wallpaper's
 *    own switch wins (the group's override);
 *  - the volume setting is above 0 and "mute" is off, and the wallpaper's
 *    own volume (walpe_sound_group_set_volume) is above 0;
 *  - the scene is shown and not paused (walpe_sound_group_set_active), and
 *    no other scene instance of the same wallpaper is already heard: a
 *    wallpaper that two monitors render separately sounds once;
 *  - the layer is visible and its player was told to play.
 * While one of them does not hold, the layer's player keeps its place in
 * the file and has no output stream and no file open that it does not
 * need: a silent wallpaper costs a parked thread at most, and one that
 * never played costs nothing.
 *
 * OUTPUT. PulseAudio's simple API (also PipeWire through pipewire-pulse),
 * one playback stream per sounding layer at the file's own sample rate:
 * the sound server mixes and resamples. A stream is written in blocks by
 * the layer's own thread, which the server's buffer paces: sound runs on
 * the audio device's clock and never on the frame clock, so a loop cannot
 * drift against anything. A stream is a connection of its own to the sound
 * server, which takes a limited number of them from all programs together:
 * at most WALPE_SOUND_MAX_STREAMS players of the process have an output at
 * one time; a layer that would be one more stays silent and gets its
 * output when another player has closed its own (a silent player closes
 * it after two seconds). WALPE_SOUND_SINK names a sink. The null device
 * - WALPE_SOUND_NULL in the environment, walpe_sound_use_null (), a build
 * without libpulse, or no server (asked again every few seconds) - plays
 * into nothing at the pace of real output: tests never make a sound.
 *
 * FILES are data from a wallpaper: WAV (PCM 8 / 16 / 24 / 32 bit, 32-bit
 * float) is read here; MP3, FLAC and Ogg (Vorbis, Opus) are decoded by
 * walpe-video.so (sounddec.c: FFmpeg's decoders for exactly these four
 * codecs, nothing else is ever opened), loaded on first use. Nothing is
 * read from a network. The limits below are checked where a file is
 * opened; a file that breaks one, or that is no sound at all, is refused
 * with a log line and the wallpaper is shown as before.
 *
 * THREADS. Every function here may be called from any thread. A player's
 * thread is detached: walpe_sound_group_free () never waits for the sound
 * server. */
#pragma once
#include <glib.h>
#include <stdio.h>

/* ---- limits ---- */
#define WALPE_SOUND_MAX_FILES     64          /* files of one layer; more are ignored */
#define WALPE_SOUND_MAX_LAYERS    32          /* players of one scene; more layers stay silent */
#define WALPE_SOUND_MAX_STREAMS   16          /* players of the whole process (all wallpapers, all
                                               * monitors) that have an output open at one time;
                                               * a layer beyond that waits, silent, for a free one */
#define WALPE_SOUND_MIN_RATE      8000        /* Hz */
#define WALPE_SOUND_MAX_RATE      192000
#define WALPE_SOUND_MAX_CHANNELS  8           /* in the file; the first two are played */
#define WALPE_SOUND_MAX_SECONDS   (6 * 3600)  /* of one file: it ends there */
#define WALPE_SOUND_MAX_FILE_BYTES (G_GINT64_CONSTANT (2) << 30)   /* 2 GiB */
#define WALPE_SOUND_BLOCK         2048        /* frames decoded and written at a time: all
                                               * that is ever held of a file's sound */

typedef struct WalpeSound WalpeSound;             /* the player of one sound layer */
typedef struct WalpeSoundGroup WalpeSoundGroup;   /* the players of one scene instance */

enum { WALPE_SOUND_LOOP = 0, WALPE_SOUND_RANDOM = 1, WALPE_SOUND_SINGLE = 2 };

typedef struct {
    const char *const *files;   /* absolute paths (the caller checked that they lie
                                 * inside the wallpaper) */
    int nfiles;
    float volume;               /* the layer's volume, 0..1 (more counts as 1) */
    int mode;                   /* WALPE_SOUND_* */
    float mintime, maxtime;     /* random mode: seconds between two plays */
    const char *name;           /* the layer's name, for the log and the stream */
} WalpeSoundSpec;

/* ---- process-wide settings (atomic; the daemon's main thread sets them) ---- */

/* Sound output is refused until a process allows it: groups and players
 * are not even created before (both constructors return NULL). */
void walpe_sound_allow (gboolean allow);
gboolean walpe_sound_allowed (void);
/* the setting "scene_sound": wallpapers without a switch of their own play
 * their sound layers. Off in this library until a program sets it (the
 * daemon does, from its configuration, where it is on by default). Takes
 * effect at once. */
void walpe_sound_set_enabled (gboolean on);
gboolean walpe_sound_enabled (void);
/* the user's volume for every wallpaper (0..1; 1 until set) and "mute" */
void walpe_sound_set_master (double volume);
void walpe_sound_set_muted (gboolean muted);
/* every stream opened from now on is the null device; fast = as fast as it
 * decodes (tests) instead of at the pace of real output */
void walpe_sound_use_null (gboolean fast);
/* one line for `walpe-ctl status`: why nothing may be heard, or what plays
 * (g_free) */
char *walpe_sound_status (void);
/* Wallpaper sound that is not this player's and obeys the same settings:
 * the pages of web wallpapers, whose sound WebKit plays (web.h
 * walpe_web_sound). The player has no stream for them, so its line alone
 * said "nothing is playing (0 wallpaper(s) with sound shown)" while a page
 * was heard. Counted by who knows the pages (the daemon), one call of
 * walpe_sound_pages_add for each page that is shown:
 *   sw, own_volume  the wallpaper's own switch (-1 none, 0 off, 1 on) and
 *                   volume (0..1), as walpe_sound_switch_parse and
 *                   walpe_sound_volume_parse give them
 *   paused          every monitor that shows the page is paused
 *   setting_on, mute, volume   the three settings
 * A page that the settings alone silence (sound off without a switch of its
 * own that is on, mute, volume 0) is not counted: the line says that of all
 * wallpaper sound. Whether a page makes any sound nobody knows without
 * running it: "heard" is "its level is above 0 and it runs". */
typedef struct {
    int heard;      /* pages that run at a level above 0 */
    int paused;     /* pages with a level above 0 that are paused: silent until they go on */
    int silent;     /* pages silenced by their own switch or their own volume of 0 */
    int own_on;     /* of the heard and paused ones: the wallpaper's own switch is on */
    int percent;    /* the level of the heard pages in percent (1..100), 0 = none yet, -1 = they differ */
} WalpeSoundPages;
void walpe_sound_pages_add (WalpeSoundPages *w, int sw, double own_volume, gboolean paused,
                            gboolean setting_on, gboolean mute, double volume);
/* walpe_sound_status with those pages in it (NULL, or all 0: the same line) */
char *walpe_sound_status_with (const WalpeSoundPages *web);
/* player threads alive right now (they end a moment after their group is
 * freed; tests wait for 0) */
int walpe_sound_threads (void);

/* ---- one scene instance ---- */

/* key: what identifies the wallpaper (the path it was opened as): of the
 * active groups with the same key one is heard. wallpaper_switch: the
 * wallpaper's own switch, see walpe_sound_switch_parse. NULL when the
 * process did not allow sound. */
WalpeSoundGroup *walpe_sound_group_new (const char *key, int wallpaper_switch);
/* the scene is shown and not paused / is not */
void walpe_sound_group_set_active (WalpeSoundGroup *g, gboolean active);
/* the wallpaper's own volume (its option "walpe_volume"), 0..1: a factor on
 * the volume setting, 1 until set; at 0 the wallpaper is silent like a
 * muted one (nothing runs for it) */
void walpe_sound_group_set_volume (WalpeSoundGroup *g, double volume);
/* The wallpaper's own switch and volume were changed while it is loaded:
 * every scene instance of the wallpaper `key` (as given to
 * walpe_sound_group_new) takes them at once - nothing is loaded again, a
 * player keeps its place. Returns how many instances there are (0: the
 * wallpaper is not loaded, or has no sound layers). */
guint walpe_sound_wallpaper_set (const char *key, int wallpaper_switch, double volume);
/* "0.35" -> 0.35; anything that is no number from 0 to 1 (also NULL) -> 1 */
double walpe_sound_volume_parse (const char *value);
/* sound is switched on for this wallpaper (its own switch, else the setting) */
gboolean walpe_sound_group_enabled (WalpeSoundGroup *g);
/* its players are the ones that are heard right now */
gboolean walpe_sound_group_speaking (WalpeSoundGroup *g);
/* stops and frees the group's players; does not wait for their threads */
void walpe_sound_group_free (WalpeSoundGroup *g);

/* "on" / "true" / "1" / "yes" = 1, "off" / "false" / "0" / "no" = 0,
 * anything else (also NULL) = -1: no switch of its own */
int walpe_sound_switch_parse (const char *value);
/* a sound file name a scene may use: relative, no "..", no drive or URL
 * scheme (it is looked up inside the wallpaper, never anywhere else) */
gboolean walpe_sound_name_ok (const char *file);

/* ---- one sound layer ---- */

/* created stopped, owned by the group. NULL without a group or a file, or
 * when the group already has WALPE_SOUND_MAX_LAYERS players. */
WalpeSound *walpe_sound_new (WalpeSoundGroup *g, const WalpeSoundSpec *spec);
/* Some of the layer's files are not on disk yet (a packaged wallpaper whose
 * copy was made while its sound was off, pkg.h): the player's own thread
 * calls prepare (arg) before it opens its first file - that is, not before
 * the layer is really about to be heard. To be set before the first play;
 * arg is copied. */
void walpe_sound_set_prepare (WalpeSound *s, void (*prepare) (const char *arg), const char *arg);
void walpe_sound_play (WalpeSound *s);    /* start, or go on after pause */
void walpe_sound_pause (WalpeSound *s);
void walpe_sound_stop (WalpeSound *s);    /* stop; the next play starts the file again */
/* Play the layer's first file from `seconds` into it (0 = its beginning),
 * whatever the player did before: what is heard now is faded out and the
 * new place follows in the same output stream, without the silence of a
 * stop and a play (which close the stream and open another). Behind the
 * end of the file there is nothing to play: the file counts as played.
 * While the layer is not heard the place is kept for when it is. For the
 * sound track of a video (video.c), which has to follow the picture. */
void walpe_sound_restart_at (WalpeSound *s, double seconds);
/* A looping player (WALPE_SOUND_LOOP, one file) whose pass is to take
 * exactly `seconds` of output, to the sample: the loop of a video's sound
 * track, which has to be as long as the clip's. The file is played from
 * its beginning for that long, again and again, in one stream and without
 * a seam of the player's making - sample n of a pass is followed by sample
 * 0 of the next, as the file has them. A file that is shorter is followed
 * by silence until the pass is over (the clip is still running); one that
 * is longer is cut there - as it is when it is longer by no more than
 * WALPE_SOUND_PERIOD_PAD_S (what an encoder pads a track's last frame
 * with), faded out over 5 ms when it is longer than that (FLAC files, whose
 * header says their length; any other file is cut as it is).
 * WalpeSoundStats.pos is then the place in the pass, and
 * walpe_sound_restart_at a place in it. To be set before the first play;
 * 0 = no such length (the default). */
#define WALPE_SOUND_PERIOD_PAD_S 0.05
void walpe_sound_set_period (WalpeSound *s, double seconds);
/* told to play and not ended (a "single" sound ends by itself) - also
 * while nothing is heard because the scene is paused or sound is off */
gboolean walpe_sound_is_playing (WalpeSound *s);
void walpe_sound_set_volume (WalpeSound *s, float volume);
float walpe_sound_get_volume (WalpeSound *s);
/* the layer is invisible: silent, its play state is kept */
void walpe_sound_hold (WalpeSound *s, gboolean hold);

typedef struct {
    gint64 frames;       /* sample frames handed to the output so far */
    int rate, channels;  /* of the file being played (0 = none yet) */
    int plays;           /* files played to their end */
    int opens;           /* files opened */
    int peak;            /* largest sample handed to the output (0..32767) */
    gboolean null_out;   /* plays into the null device */
    gboolean running;    /* has a thread that produces output right now */
    char error[160];     /* last failure ("" = none) */
    gint64 pos;          /* where in its file the player is: sample frames from the file's
                          * start to the last one handed to the output (-1 = no file) */
} WalpeSoundStats;
void walpe_sound_stats (WalpeSound *s, WalpeSoundStats *out);

/* ---- decoders for compressed files (walpe-video.so, modules.h) ---- */
typedef struct {
    /* *rate and *channels (1 or 2) describe what read () delivers; NULL and
     * *err (g_free) when the file is refused */
    gpointer (*open) (const char *path, int *rate, int *channels, char **err);
    /* interleaved 16-bit frames; 0 = end, < 0 = error */
    int (*read) (gpointer dec, gint16 *out, int max_frames);
    void (*close) (gpointer dec);
    /* The next `frames` frames are passed over as if they had been read;
     * returns how many (fewer: the file ended). May be NULL: the player
     * then reads and drops them. With it a decoder can go to a place
     * without decoding all that lies in front of it (sounddec.c: FLAC). */
    gint64 (*skip) (gpointer dec, gint64 frames);
} WalpeSoundDecoder;

/* sounddec.c (part of walpe-video.so; tests compile it in) */
gpointer walpe_sounddec_open (const char *path, int *rate, int *channels, char **err);
int walpe_sounddec_read (gpointer dec, gint16 *out, int max_frames);
gint64 walpe_sounddec_skip (gpointer dec, gint64 frames);
void walpe_sounddec_close (gpointer dec);

/* tests: this decoder instead of the module's (NULL = the module's again) */
void walpe_sound_set_decoder (const WalpeSoundDecoder *dec);

/* A wallpaper's sound file, opened for reading: a regular file of at most
 * WALPE_SOUND_MAX_FILE_BYTES (a FIFO or a device under that name would
 * block or never end). NULL and *err (static text) otherwise. Used by the
 * WAV reader here and by the module's decoders. */
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static inline FILE *walpe_sound_fopen (const char *path, const char **err)
{
    int fd = path ? open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY) : -1;
    struct stat st;
    if (fd < 0) {
        *err = "cannot be opened";
        return NULL;
    }
    if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode)) {
        *err = "not a regular file";
        close (fd);
        return NULL;
    }
    if ((gint64) st.st_size > WALPE_SOUND_MAX_FILE_BYTES) {
        *err = "larger than the size limit for sound files";
        close (fd);
        return NULL;
    }
    FILE *f = fdopen (fd, "rb");
    if (!f) {
        *err = "cannot be opened";
        close (fd);
    }
    return f;
}
