# Scene features and their switches

Things scene wallpapers can ask for that WalPE draws since 0.13, each with a switch. A
scene that asks for none of them is drawn as in 0.12.1 at render sizes up to 2048 pixels
on the longer side. Above that, effects on colour-only layers are computed at the render
size instead of at most 2048 pixels, on purpose, and are sharper than in 0.12.1
([RESOLUTION.md](RESOLUTION.md)).

> **State of testing.** Built and tested only on Mesa's software renderer. With every
> switch below off, all 91 scenes of the test library give the frames of 0.12.1 (compared
> at 960x540). Nothing
> was compared with Wallpaper Engine's own picture, and no cost was measured on a GPU:
> the cost statements are counts of pixels and passes.

## The switches

| Setting | Default | What it switches | Takes effect |
|---|---|---|---|
| `camera_parallax` | on | layers of scenes with camera parallax follow the pointer, each by its own depth | the wallpaper is loaded again |
| `parallax_offset` | **off** | such layers are also displaced while the pointer rests | next frame |
| `script_camera` | on | scripts move and zoom the view | next frame |
| `sprite_sheets` | on | animated (sprite sheet) pictures of image layers play | the wallpaper is loaded again |
| `script_texanim` | on | scripts drive those animations | next frame |
| `bloom` | on | scenes that ask for bloom get a glow around bright parts | at once |
| `camera_shake` | on | scenes that ask for camera shake sway | at once |
| `fluid_sim` | on | Wallpaper Engine's Fluid Simulation effect runs | at once |
| `fluid_quality` | `full` | `full`, `reduced`, `low` (below) | at once |

```sh
walpe-ctl set bloom=off camera_shake=off
walpe-ctl set sprite_sheets=off script_texanim=off
walpe-ctl status          # "features": "camera_parallax=on parallax_offset=off ..."
WALPE_FEATURES="bloom=off,camera_shake=off" walpe-daemon   # for one run; wins over the settings
```

The first seven are saved in `config.json` as `"features": { "<name>": true|false }` (only
the ones that were ever set) and shown by `walpe-ctl status` as the text `features` and the
object `feature_switches`; switches fixed by `WALPE_FEATURES` are named in
`features_fixed`. `fluid_sim` and `fluid_quality` are members of their own. There are no
rows for any of them in the settings window yet.

**Which default pictures change against 0.12.1.** With default settings 16 of the 91
scenes of the test library draw another picture than 0.12.1 did, each because it asks for a
feature that is now drawn: bloom 8, bloom and camera shake 2, camera shake 3, sprite sheets
2, a script that sets the view's zoom 1. With the switch named each of them is the picture
of 0.12.1 again. Scenes with camera parallax change only while the pointer is away from
the middle of their monitor.

## Camera parallax

A scene whose author switched on camera parallax moves its layers against the pointer, each
layer by its own depth, with the delay and the strength the scene sets (amount, delay,
mouse influence - also negative -, a depth per layer for x and y). Layers inside a group
and things attached to a puppet move as one piece. Wallpaper options bound to these values
work; scripts read and write a layer's `parallaxDepth`.

- The existing setting `parallax` still decides whether wallpapers get the pointer at all.
  With `parallax=off`, without a pointer, and while the pointer rests in the middle of the
  monitor, every layer stands where the scene file puts it.
- `parallax_offset=on` also displaces layers that sit away from the camera's centre while
  the pointer rests. Whether Wallpaper Engine does that is not verified, so it is off.
- Without a pointer the last known position holds; layers do not jump back when the
  pointer leaves (on Wayland the pointer is only known over the bare desktop).
- It costs nothing while the pointer rests: the scene asks for frames only while the
  layers are still catching up with the pointer.
- Layers move sideways; perspective does not change (scenes are flat here).

## Sprite-sheet layers and their script control

An image layer whose picture is an animation stored as a sheet of frames plays. Frame times
are the ones stored in the file (0.1 s for a frame without one); the animation loops unless
a script says otherwise; a paused wallpaper pauses it. The frame table is checked before
use; a sheet that fails is refused with a log line and the layer shows its first frame (or
the whole picture, as before the feature). The animations of one scene have at most
1048576 frames together (sixteen tables of the largest size); a layer beyond that shows its
first frame as a still. A playing sheet makes its layer change when the
frame changes, not with every screen frame; changing the frame uploads nothing.

Not covered: frames stored on further images of one file (left out, the log says how
many), blending between frames, puppets with an animated picture, animated pictures bound
to an effect.

Scripts: `layer.getTextureAnimation ()` (`frameCount`, `duration`, `rate`, `loop`, `play`,
`pause`, `stop`, `isPlaying`, `getFrame`, `setFrame`, `getCurrentTime`, `setCurrentTime`,
`join`); `addEndedCallback` is accepted and never called.

## Script camera

`thisScene.getCameraTransforms ()` returns `{ eye, center, up, zoom }`;
`setCameraTransforms` uses the zoom (0.01-100) and the eye's x and y; a script on the
scene's zoom sets the view's zoom. A script reaches the camera and the animations of its
own scene only; values that are not finite numbers are ignored. A zoom in the scene file
that no float holds (`1e39`) counts as no zoom: the scene is drawn at zoom 1.

## Bloom

Scenes that switch on bloom get it: bright parts of the finished picture shine softly into
their surroundings, with the threshold, strength and tint the wallpaper's author set (or
its own options set). It is drawn the way Wallpaper Engine's standard bloom is defined: a
bright pass, a 13-tap blur in both directions at an eighth of the frame, added to the
picture. The glow therefore reaches about 48 pixels whatever the monitor's resolution.

- Presets: `post_processing` (`post`) `full` draws it as it is, `reduced` (preset
  `balanced`) uses half-size inner buffers and 7 blur taps, `off` (preset `low`) leaves it
  out.
- Wallpaper Engine's **HDR bloom is not implemented**: scenes made for it get the standard
  bloom with their standard values.
- Per drawn frame: one copy of the frame at half size (none at a reduced render scale or
  resolution), three small passes and one pass over the whole frame. A bloom that cannot
  change a pixel is not run; a wallpaper in which nothing moves draws no frames.
- With "Skip hidden areas" on, a wallpaper with bloom draws its layers in full.
- `walpe-ctl profile` lists the steps as rows `bloom: ...` under "(after the layers)".

## Camera shake

Scenes that switch on camera shake sway: the whole view moves smoothly around its place of
rest (amplitude 1 = at most 1 % of the scene's width and height). The motion follows the
clock, not the frame count.

**The look is an estimate.** Wallpaper Engine does not publish how amplitude, roughness and
speed turn into motion. `WALPE_SHAKE_AMOUNT` and `WALPE_SHAKE_RATE` scale how far and how
fast the view sways, and `WALPE_SHAKE_COVER=1` zooms in by the reach of the sway so that
no edge of the canvas comes into view ([ENVIRONMENT.md](ENVIRONMENT.md)); reports of the
right factors are welcome.

Cost: no GPU work of its own, but the picture changes with every frame, so a wallpaper that
was still now draws every frame at its frame rate limit, and layers that do not move are
drawn every frame as well. With `camera_shake=off` such a wallpaper is as cheap as before.

## Fluid simulation

Wallpaper Engine's "Fluid Simulation" effect: emitters blow smoke into a layer, the smoke
flows and fades, and the pointer pushes it. WalPE runs the effect's own shaders from the
Wallpaper Engine installation, so the assets directory must be known (`we_assets`).

- `fluid_sim=off`: the effect is left out and its layer is shown without it.
- `fluid_quality`: `full` = as the effect file declares it, 60 steps a second; `reduced` =
  the smoke's picture at half its width and height; `low` = the same and 30 steps a second.
  The presets set it: `max` / `high` full, `balanced` reduced, `low` low.
  `WALPE_FLUID=0|1` and `WALPE_FLUID_QUALITY` override both for one run.
- The smoke is advanced in steps of the wallpaper's own time, so it moves at the same speed
  at every frame rate (Wallpaper Engine runs one step per frame). Below about 15 frames a
  second it slows down instead.
- The pointer pushes the smoke only while it moves and only on the monitor it is on;
  `parallax=off` switches the push off too.
- A driver that cannot draw into float buffers: the effect is left out and the log says so.
- An effect file gets a grid of at most 1024 cells on its longer side, and no buffer larger
  than the driver's largest texture.
- Not supported: the effect's lit variant (its "lighting" option) and the script calls
  `clearVelocity` / `clearDye`.
- If the smoke is pushed at the mirrored height (pointer near the top, push near the
  bottom), start the daemon once with `WALPE_FLUID_POINTER_Y=up` and report which is right.
- No wallpaper of the test library is known to use this effect; the tests run the
  simulation with shaders of their own.
