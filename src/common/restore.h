/* restore.h - what the saved wallpaper choice becomes when a wallpaper
 * failed to load (shared by the daemon and its test). No state of its own.
 *
 * A load runs on a monitor's render thread after `open` has answered and
 * saved the new choice. When it fails, the monitors that were to show the
 * wallpaper (a render_shared group: the leader and its followers) show
 * nothing: the wallpaper they had was freed to make room, a follower gave
 * up its own before the leader even started. They are given their previous
 * wallpapers back, once: this file decides which monitor returns to what
 * and puts the config (the default `current` and the monitors' own choices)
 * back to how it was for them. Loading, and what happens when the previous
 * wallpaper fails too, is the daemon's part. */
#pragma once
#include <glib.h>
#include "common/config.h"

/* the choice an `open` for every monitor replaced (it drops the monitors'
 * own choices, also those of outputs that are not connected) */
typedef struct {
    char *path;                  /* the wallpaper that open made the default */
    char *current;               /* the default before it */
    GHashTable *monitor_current; /* output -> path before it (copy; NULL = none) */
} WalpeOpenUndo;

static inline void walpe_open_undo_clear (WalpeOpenUndo *u)
{
    g_clear_pointer (&u->path, g_free);
    g_clear_pointer (&u->current, g_free);
    g_clear_pointer (&u->monitor_current, g_hash_table_unref);
}

/* before an open-for-all of `path` changes cfg */
static inline void walpe_open_undo_take (WalpeOpenUndo *u, const WalpeConfig *cfg,
                                         const char *path)
{
    walpe_open_undo_clear (u);
    u->path = g_strdup (path);
    u->current = g_strdup (cfg->current);
    if (cfg->monitor_current && g_hash_table_size (cfg->monitor_current)) {
        GHashTableIter it;
        gpointer k, v;
        u->monitor_current = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
        g_hash_table_iter_init (&it, cfg->monitor_current);
        while (g_hash_table_iter_next (&it, &k, &v))
            g_hash_table_insert (u->monitor_current, g_strdup (k), g_strdup (v));
    }
}

typedef struct {
    const char *name;   /* output name (the config key) */
    const char *good;   /* the last wallpaper it really showed (NULL = none) */
    gboolean member;    /* in: it was to show `failed` through the load that failed */
    gboolean restore;   /* out: it returns to `good` */
} WalpeRestoreMon;

/* `failed` did not load for the monitors marked `member` (all n connected
 * monitors are listed). Every member that showed another wallpaper before
 * returns to it; a member with nothing to return to (no wallpaper before,
 * or the one that just failed) stays on `failed`. cfg is changed, not
 * saved. Returns how many monitors return (0 = cfg untouched).
 *
 * The default: when `failed` is the default, no connected monitor is left
 * on it and undo is the open that made it the default, the default and the
 * own choices of outputs that are not connected are those from before that
 * open again. A monitor whose wallpaper is the default has no own choice
 * (as `open` keeps it). */
static inline int walpe_restore_plan (WalpeConfig *cfg, const WalpeOpenUndo *undo,
                                      const char *failed, WalpeRestoreMon *mons, int n)
{
    int back = 0;
    for (int i = 0; i < n; i++) {
        mons[i].restore = mons[i].member && mons[i].good && *mons[i].good &&
                          g_strcmp0 (mons[i].good, failed) != 0;
        back += mons[i].restore;
    }
    if (!back)
        return 0;
    for (int i = 0; i < n; i++)
        if (mons[i].restore)
            walpe_config_set_monitor_path (cfg, mons[i].name, mons[i].good);
    gboolean left = FALSE, undone = FALSE;
    for (int i = 0; i < n; i++)
        left |= !g_strcmp0 (walpe_config_monitor_path (cfg, mons[i].name), failed);
    if (!left && !g_strcmp0 (cfg->current, failed) && undo && undo->current && *undo->current &&
        !g_strcmp0 (undo->path, failed) && g_strcmp0 (undo->current, failed)) {
        undone = TRUE;
        g_free (cfg->current);
        cfg->current = g_strdup (undo->current);
        if (undo->monitor_current) {
            GHashTableIter it;
            gpointer k, v;
            g_hash_table_iter_init (&it, undo->monitor_current);
            while (g_hash_table_iter_next (&it, &k, &v)) {
                gboolean connected = FALSE;
                for (int i = 0; i < n; i++)
                    connected |= !g_strcmp0 (mons[i].name, k);
                if (!connected)
                    walpe_config_set_monitor_path (cfg, k, v);
            }
        }
    }
    /* (also the monitors that returned earlier, while the failed wallpaper
     * was still the default: their loads fail one after the other when
     * each monitor loads on its own) */
    for (int i = 0; i < n; i++)
        if ((mons[i].restore || undone) && cfg->monitor_current &&
            !g_strcmp0 (g_hash_table_lookup (cfg->monitor_current, mons[i].name), cfg->current))
            walpe_config_set_monitor_path (cfg, mons[i].name, NULL);
    return back;
}
