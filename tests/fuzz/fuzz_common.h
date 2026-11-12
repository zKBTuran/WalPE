/* fuzz_common.h - what the fuzz harnesses of this directory share.
 *
 * A harness is one LLVMFuzzerTestOneInput () per parser of wallpaper bytes.
 * It is built twice: with libFuzzer (-Dfuzz=enabled, clang), and with
 * standalone.c as a plain program that replays files (the "fuzz-regress"
 * tests of the normal suite: every input that once crashed a parser lies
 * under tests/fuzz/regress/<target>/ and is run again there).
 *
 * The parsers take a file's name: the input is put into a memory file
 * (memfd) and named through /proc/self/fd, so no harness writes to a disk. */
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <glib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size);
int LLVMFuzzerInitialize (int *argc, char ***argv);

static inline GLogWriterOutput fuzz_log_drop (GLogLevelFlags level, const GLogField *fields,
                                              gsize n_fields, gpointer user)
{
    (void) level; (void) fields; (void) n_fields; (void) user;
    return G_LOG_WRITER_HANDLED;
}

/* messages of the parsers are not what a run is about (and cost most of
 * its time); WALPE_FUZZ_LOG=1 keeps them. Once per process. */
static inline void fuzz_quiet (void)
{
    static gsize once;
    if (g_once_init_enter (&once)) {
        if (!g_getenv ("WALPE_FUZZ_LOG"))
            g_log_set_writer_func (fuzz_log_drop, NULL, NULL);
        g_once_init_leave (&once, 1);
    }
}

/* the input as a regular file: its name, valid until the next call with
 * the same slot (0..3) */
static inline const char *fuzz_file (int slot, const uint8_t *data, size_t size)
{
    static int fds[4] = { -1, -1, -1, -1 };
    static char names[4][40];
    if (fds[slot] < 0) {
        fds[slot] = memfd_create ("walpe-fuzz", MFD_CLOEXEC);
        if (fds[slot] < 0)
            abort ();
        g_snprintf (names[slot], sizeof names[slot], "/proc/self/fd/%d", fds[slot]);
    }
    if (ftruncate (fds[slot], 0) != 0)
        abort ();
    for (size_t off = 0; off < size;) {
        ssize_t w = pwrite (fds[slot], data + off, size - off, (off_t) off);
        if (w <= 0)
            abort ();
        off += (size_t) w;
    }
    return names[slot];
}

/* a directory for the few harnesses that need real files next to the input
 * (made once, never written to per input): $WALPE_FUZZ_TMP or the system's */
static inline const char *fuzz_dir (const char *name)
{
    const char *base = g_getenv ("WALPE_FUZZ_TMP");
    char *d = g_strdup_printf ("%s/walpe-fuzz-%s-%d", base && *base ? base : g_get_tmp_dir (), name,
                               (int) getpid ());
    if (g_mkdir_with_parents (d, 0700) != 0)
        abort ();
    return d;   /* kept for the life of the process */
}

/* every byte of a result is read: a length that says more than was
 * allocated is then a report of the address sanitizer */
static inline uint32_t fuzz_touch (const void *p, size_t n)
{
    const volatile uint8_t *b = p;
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++)
        s += b[i];
    return s;
}

/* a rule of the format that the harness checks itself */
#define FUZZ_CHECK(cond) \
    do { if (!(cond)) { g_printerr ("fuzz: check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); abort (); } } while (0)
