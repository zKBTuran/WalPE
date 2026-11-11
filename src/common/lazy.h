/* lazy.h - small values that several threads share without a lock: a
 * setting worked out on first use, and a counter that limits how often a
 * message is logged.
 *
 * Every monitor has its own render thread and every playing video its own
 * decode thread, and all of them run the same functions. A function that
 * keeps "have I read this environment switch yet" in a plain static is
 * therefore read and written by several threads at once. On x86-64 that
 * happened to work; by the C11 memory model it is a data race (undefined
 * behaviour, and ThreadSanitizer reports each one), and nothing but habit
 * keeps a compiler from assuming it cannot happen.
 *
 * WalpeLazy: an int that starts at WALPE_LAZY_UNSET (or another value the
 * result can never have) and is stored once it is known.
 *
 *     static WalpeLazy cell = WALPE_LAZY_UNSET;
 *     int on = walpe_lazy_get (&cell);
 *     if (on < 0)
 *         on = walpe_lazy_set (&cell, g_getenv ("WALPE_X") != NULL);
 *
 * Both accesses are atomic with memory_order_relaxed, which is the weakest
 * ordering there is and enough here, for three reasons that every user of
 * this type has to keep true:
 *  - the value is the whole message: nothing else is published through it
 *    (no pointer to memory another thread filled in, no second variable
 *    that has to be read together with it), so a reader needs no
 *    happens-before edge to the writer, only an untorn value;
 *  - the computation gives the same result in every thread (the
 *    environment is not changed once threads exist, there is one GL driver
 *    per process), so two threads that both find it unset store the same
 *    value and it does not matter whose store a third thread sees;
 *  - a thread that still reads "unset" after another has stored the result
 *    only computes it once more.
 * A relaxed load or store of an int is one plain move on x86-64 and one
 * ldr / str on arm64: the same instructions the plain static compiled to,
 * so the per-frame callers cost what they cost before.
 *
 * Anything that does not fit these rules - a pointer to something built on
 * first use, a double or 64-bit value (not lock-free everywhere), several
 * values set together, a computation with side effects such as a log line
 * that must appear once - uses g_once_init_enter () / g_once_init_leave ()
 * instead: one thread computes, the others wait and then read. Its fast
 * path is one atomic pointer load. */
#pragma once
#include <glib.h>
#include <stdatomic.h>

typedef atomic_int WalpeLazy;
#define WALPE_LAZY_UNSET (-1)

static inline int walpe_lazy_get (WalpeLazy *cell)
{
    return atomic_load_explicit (cell, memory_order_relaxed);
}

/* stores v and returns it */
static inline int walpe_lazy_set (WalpeLazy *cell, int v)
{
    atomic_store_explicit (cell, v, memory_order_relaxed);
    return v;
}

/* TRUE for the first `limit` calls, whichever threads make them: "log this
 * at most five times". The counter is only added to while it is below the
 * limit, so it stays within the number of threads of it and never wraps.
 * Relaxed: the count is the whole message, as above. */
static inline gboolean walpe_first_n (atomic_int *count, int limit)
{
    return atomic_load_explicit (count, memory_order_relaxed) < limit &&
           atomic_fetch_add_explicit (count, 1, memory_order_relaxed) < limit;
}
