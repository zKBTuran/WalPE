/* scenesound_server.h - the five calls scenesound.c makes into the sound
 * server (they have the shape of libpulse-simple's functions, which they
 * once were): the real ones, and the way for a test to put stand-ins in
 * their place (how many streams are being opened at a time, a server that
 * does not answer) without a server. Only in a build with libpulse. */
#ifndef WALPE_SCENESOUND_SERVER_H
#define WALPE_SCENESOUND_SERVER_H
#include <pulse/simple.h>

typedef struct {
    pa_simple *(*new_stream) (const char *server, const char *app, pa_stream_direction_t dir, const char *device,
                              const char *stream, const pa_sample_spec *spec, const pa_channel_map *map,
                              const pa_buffer_attr *attr, int *error);
    void (*free_stream) (pa_simple *s);
    int (*write) (pa_simple *s, const void *data, size_t bytes, int *error);
    int (*drain) (pa_simple *s, int *error);
    int (*flush) (pa_simple *s, int *error);
} WalpeSoundServerCalls;

/* These calls instead of the real ones. Before the first stream is opened:
 * afterwards the call changes nothing. */
void walpe_sound_set_server_calls (const WalpeSoundServerCalls *calls);
/* The real ones (scenesound_pulse.c): libpulse, each call with a time
 * limit. A stream is the opaque pa_simple pointer they hand out. For
 * scenesound.c, and for a test that runs them against a server. */
const WalpeSoundServerCalls *walpe_sound_pulse_calls (void);

#endif
