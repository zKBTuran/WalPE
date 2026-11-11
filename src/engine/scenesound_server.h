/* scenesound_server.h - tests only: stand-ins for the five functions of
 * libpulse-simple that scenesound.c calls, so that a test can stand in
 * for the sound server (one that refuses connections, one that does not
 * answer) without one. Only in a build with libpulse. */
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

/* These calls instead of the library's. Before the first stream is opened:
 * afterwards the library is loaded and the call changes nothing. */
void walpe_sound_set_server_calls (const WalpeSoundServerCalls *calls);

#endif
