/* rescanpace.h - when the library is scanned again after a change in it.
 *
 * A scan waits 0.6 s for the change to be over; a change that comes while
 * it waits starts the 0.6 s again. For the library folder itself that is
 * the whole rule. A folder that is arriving (main.c: watched until it is a
 * complete wallpaper) reports every file written into it, and files that
 * keep coming - an unpacking, a copy of many files - would start the wait
 * again for as long as they come: nothing else that changed in the library
 * in that time showed before the last file was there. What an arriving
 * folder reports therefore moves a waiting scan for 3 s at most. */
#pragma once
#include <glib.h>

#define WALPE_RESCAN_QUIET_MS 600
#define WALPE_RESCAN_ARRIVAL_MAX_MS 3000

/* A file event from an arriving folder at now_us. waiting: the scan after
 * the change has not been made yet (its 0.6 s run); asked_us: when it was
 * first asked for. TRUE: start the 0.6 s again, as for any change. FALSE:
 * leave the timer that runs alone - it has been moved for long enough. */
static inline gboolean walpe_rescan_arrival_moves (gboolean waiting, gint64 asked_us, gint64 now_us)
{
    return !waiting || now_us - asked_us < (gint64) WALPE_RESCAN_ARRIVAL_MAX_MS * 1000;
}
