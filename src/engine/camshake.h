/* camshake.h - camera shake of a scene (general.camerashake), the pure part.
 *
 * A scene that switches the shake on names three numbers: amplitude (how
 * far), speed (how fast) and roughness (how jittery). The view centre then
 * sways around its place of rest; every layer that the camera shows moves
 * with it as one rigid picture.
 *
 * Nothing here is frame based: the offset is a function of the scene's
 * clock alone, so the sway looks the same at 30, 60 or 144 frames per
 * second, a paused scene resumes where it stopped, and a given instant
 * always gives the same offset (the noise has fixed seeds; a test or an
 * A/B picture names the instant and gets the same view on every run).
 *
 * How the three numbers turn into motion is NOT known from Wallpaper
 * Engine (it publishes no formula): the shape below is an estimate of the
 * look, sized after what scene authors do about the shake - scenes with
 * amplitude 0.5 zoom in by about 1 %, a scene with amplitude 1 by 4 %, to
 * hide what the sway would uncover at the edges.
 *
 *   speed      the view changes direction about 0.6 times a second at
 *              speed 1 (smooth value noise, one lattice point per 1 / 0.6 s)
 *   roughness  mixes in a second noise 4.3 times as fast with weight
 *              roughness / 2 (the sum is scaled back, the reach stays)
 *   amplitude  1 reaches at most 1 % of the scene's width horizontally and
 *              1 % of its height vertically
 *
 * No GL, no globals: tests call it directly. */
#pragma once
#include <glib.h>

G_BEGIN_DECLS

/* The offset of the view centre at scene time t (seconds), in scene units,
 * x to the right and y up. (0, 0) when nothing moves: t < 0 or not a
 * number, amplitude 0, speed 0, or any argument that is not finite. */
void walpe_camera_shake (double t, float amplitude, float roughness, float speed, float scene_w,
                         float scene_h, float out[2]);

/* The farthest the view centre gets from rest, as a share of the scene's
 * size (0.01 at amplitude 1; the same clamp as walpe_camera_shake). */
float walpe_camera_shake_reach (float amplitude);

/* largest values taken (anything beyond is clamped) */
#define WALPE_CAMSHAKE_MAX_AMPLITUDE 100.0f
#define WALPE_CAMSHAKE_MAX_ROUGHNESS 10.0f
#define WALPE_CAMSHAKE_MAX_SPEED     100.0f

G_END_DECLS
