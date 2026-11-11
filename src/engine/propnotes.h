/* propnotes.h - which options of a wallpaper cannot do anything, and why.
 *
 * Static: reads project.json and the scene's JSON (scene.json and the other
 * .json entries of scene.pkg; nothing is extracted or loaded), so the GUI
 * can say it for any wallpaper, running or not:
 *   "unused"           nothing in the scene refers to the option: no
 *                      {"user": ...} binding, no user texture slot, no
 *                      script mentions it and no other option's condition
 *                      depends on it. It does nothing in Wallpaper Engine
 *                      either (WE's scheme colour, checkboxes that only
 *                      carry a picture or a link).
 *   "needs:<feature>"  bound only to an engine feature WalPE does not
 *                      have (HDR bloom, camera fade, scene sound, ...).
 *   "full:<n>"         bound to nothing but the "count" of particle
 *                      systems that are full whatever the option says:
 *                      their emitters keep more than the file's maxcount
 *                      (n) alive even near the option's low end, and count
 *                      does not move that cap (weparticles.h). Judged from
 *                      the particle files inside the wallpaper only.
 * Options that are bound to something WalPE resolves, or that a script
 * names, get no note. (The probe of propprobe.h measures the same thing
 * dynamically: does the scene state change?) */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

/* {key: note} for the wallpaper at path (a scene, a preset of one, or a
 * video/GIF/image wallpaper, whose project properties WalPE never reads);
 * NULL when there is nothing to say (web pages are opaque). Unref. */
JsonObject *walpe_prop_notes (const char *path);
/* note -> a sentence for the user (g_free) */
char *walpe_prop_note_text (const char *note);
