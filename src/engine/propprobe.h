/* propprobe.h - user-property coverage probe (diagnostics, off by default).
 *
 * WALPE_PROP_PROBE=<dir> makes a scene record
 *  - every user property VALUE the engine consumed while loading and
 *    running it, with the place that consumed it ("visible", "fx.const",
 *    "script.read", ...), and
 *  - a digest of the resolved scene state (layers, effects, particles,
 *    text, textures, clear colour, camera) at a fixed frame, with a
 *    virtual 30 fps clock so two loads with the same values match (frame
 *    6 by default; WALPE_PROP_PROBE_FRAME=n or a number in <dir>/frame,
 *    read at each load, picks another).
 * One JSON file per load: <dir>/<wallpaper dir name>.<n>.json. The harness
 * changes one property at a time with walpe-ctl and
 * diffs the digests: an option is effective when the digest changes. */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

gboolean walpe_probe_on (void);
/* frame (1-based) at which a scene writes its digest */
int walpe_probe_frame (void);
/* a wallpaper starts loading on this thread: forget recorded uses */
void walpe_probe_begin (void);
/* the value of user property prop was consumed at site */
void walpe_probe_use (const char *prop, const char *site);
/* b: builder positioned inside the root object (the caller added the
 * state members); adds "uses" and writes the file. Consumes nothing. */
void walpe_probe_write (JsonBuilder *b, const char *origin);
