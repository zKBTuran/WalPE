# Changelog

What changed in each released version of WalPE, newest first, written for people who use
it. Version numbers are `0.<minor>.<patch>`; every release is tagged `v<version>` in git.
Release names are album titles by the band Boris, one per release.

## 0.14.0 "Flood" (not released)

The development version. It contains everything of 0.13.0 "PINK" below, which was not
released on its own. On top of it: memory and CPU that the daemon kept or spent for
things that were not in use, corrections to the fullscreen pause per monitor, a fix for
scene sound, a report of where the daemon's memory is, and what a review of the things
the daemon takes from outside found (under Security). **The state of testing is that
of 0.13.0: Mesa's software renderer (llvmpipe), Xvfb, a headless Wayland compositor (sway
1.9) and a PulseAudio null sink. Nothing of it has been run on real graphics hardware.**
Two settings are new and on by default: `video_scene_convert` (under Video) and
`web_unload` (under Other). The fixes are meant to change no picture; the one place where
the picture is not the same pixel for pixel is a scene's video played from its converted
copy.

### Wayland

- Web wallpapers on a Wayland session need no X any more. They run through Xwayland
  where an X display answers, as before; where none does, the page's hidden window lives
  on a Wayland display inside the daemon (the "private page host"; `walpe-ctl status`:
  `web_host`). `DISPLAY` left over in a session without an X server no longer switches
  web wallpapers off: the log and `web_host` say that the private page host is used
  instead. With that page host the page's process is not handed the compositor's socket
  by WebKit's sandbox ([SECURITY.md](docs/SECURITY.md)). Run with real pages on the
  headless sway, without any X; a page there is expected to animate at 60 frames a
  second at most ([WAYLAND.md](docs/WAYLAND.md#web-wallpapers)).
- The web module builds without libX11 (`-Dx11=disabled`, or a system that has none):
  `./build.sh -Dx11=disabled` gives a build in which the daemon and the web module use no
  X library. The module needs one of the two page hosts, and `-Dweb=enabled` fails at the
  configure step only when neither can be built. On an X11 session nothing changes, and
  the X11-only build (`-Dwayland=disabled`) has nothing of the private page host in it.

### Scenes

- A layer with effects that changes its size from frame to frame - one that crosses a
  screen edge while the camera parallax follows the pointer, a camera shake sways the view
  or a script moves it - no longer fills the pool of effect targets. The pool kept one set
  of targets for every size of the last five seconds: on a 640x360 output with two such
  layers 137 to 262 targets (31 to 60 MiB) against 28 (2.8 MiB) with the pointer at rest;
  now 37 to 43 (5 to 9 MiB). A scene whose layers keep their sizes allocates as before.
- Particles: the reduced-resolution layer for large soft sprites keeps one buffer for
  each of its two sizes (1/2 and 1/4 of the view) instead of one buffer that was deleted
  and made again whenever the systems of a scene changed between the two. One test scene
  did that four times within a few seconds, several times an hour, for as long as it ran:
  23 buffers made in an hour on a 1920x1080 monitor, 3 now. The smaller buffer (0.5 MiB at
  1920x1080) now stays while the layer is in use.
- Particles: a particle system takes its memory when it makes its first particle, not
  when the wallpaper is loaded. A system that never emits - its object is not visible, it
  waits for an event, a script keeps its count at 0 - held room for up to 256 particles
  for nothing.
- Text layers: the glyphs of a wallpaper's fonts leave the daemon with the wallpaper.
  They stayed until the daemon was stopped: 0.3 to 0.7 MB for every wallpaper with a clock
  and a date that had been shown, more with large text.
- Text layers, with a text wallpaper on one monitor while others come and go on the next:
  the font files of a wallpaper that left stayed registered (about 6 kB per file), **and a
  later wallpaper could be drawn with the wrong font**: when its font had the same family
  and style name as a font of a wallpaper that had left, the file of that earlier
  wallpaper was used. A wallpaper's text is drawn from its own font file now.
- A program keeps no shader object alive after it was linked. Each program held its two
  shader objects with their source text for as long as it lived: 170 shader objects and
  743 kB of source for the 85 programs of one test scene, and whatever the driver keeps
  for a shader on top of that.
- The table that remembers which effect pass last used a program forgets a program when
  it is deleted. On a driver that gives every new program a new name it grew by a few
  dozen bytes per program with every change of wallpaper.
- With `fx_specialize` on or `fx_quality=fast`: an effect pass that draws one single
  colour (the blur-size pass of a bokeh effect, the same value all over the picture) was
  compared with its faster program again and again for as long as the wallpaper ran -
  behind draw 8192, 16384, 32768 and so on, at 60 frames a second after 2, 5, 9, 18, 36
  and 73 minutes - each time with a scratch target, a read-back and a buffer of 2 MiB
  each on a 1920x1080 monitor. Such a pass now keeps the program it draws with after
  draw 4096 (about a minute) and is not compared again. The picture is the one it drew
  before.
- Scene sound (`scene_sound`, off by default): a scene with many sound layers lost some
  of them for five seconds after a start or a resume on a busy machine, although the
  sound server had answered every stream. The layers open their streams one at a time,
  and a layer gave up a quarter of a second after it began to wait, whoever was being
  served. The quarter second now belongs to the layer whose turn it is: the others wait
  as long as the turns go round. Only behind a stream the server has not answered in that
  time do they play into nothing, and they ask again a quarter of a second later instead
  of five seconds later. With 16 layers against PulseAudio 16.1 on a loaded machine 3 of
  30 starts lost 2 to 15 layers before, none of 30 now. Measured against a null sink;
  nobody has heard it.
- OpenGL ES: with `fx_specialize` on or `fx_quality=fast`, an effect shader above the
  256 KiB limit was named in two warnings instead of one.

### Video

- A video inside a scene that is larger than the scene shows it (a 4K clip on a 1080p
  monitor) used to be decoded at its own size for as long as the scene ran: four times the
  decoding of what is seen. It is now converted once, in the background and at the lowest
  priority, into a copy of the size shown (`video_resolution`) in `~/.cache/walpe/video`,
  and the scene plays the copy from where the clip next starts again; the original plays
  until then. `walpe-ctl set video_scene_convert=on|off` (on by default; Engine settings,
  "Smaller copies of videos in scenes"), `walpe-ctl status`: `scene_videos`. Only 8-bit
  4:2:0 clips are converted, the frame rate is kept, the picture differs from the original
  by the scaling and one more encoding. A copy in the cache that cannot be read any more
  (cut short when the machine lost power) is removed and made again. A clip that is
  stored rotated gets no copy (it would never be played: `not made (the clip is stored
  rotated)`), and a copy that turns out not to be the clip (`not usable`) is no longer
  kept for ever. Measured with a 3840x2160 60 fps clip on a 1920x1080 monitor, software
  decoding: 16.1 - 16.8 ms of CPU per picture before, 4.9 ms from the copy (3.3 to 3.5
  times less), about 110 MB less memory; the conversion took 64 CPU seconds once
  (docs/VIDEO.md).

### Pausing

Corrections to "Pause for fullscreen apps" per monitor, which is new in 0.13.0:

- X11: a window that turns fullscreen within a moment of getting the focus - a game or a
  player that asks for fullscreen as soon as its window is there - pauses its monitor.
  The daemon could miss that change (35 of 300 such starts on a busy machine) and left
  the monitor running until the focus moved again.
- X11: the root window and the daemon's own wallpaper windows never count as the focused
  application. When another program named one of them as the active window, the daemon
  stopped hearing that a part of that wallpaper had to be drawn again: without a
  compositor, what a window had covered on a paused or still wallpaper stayed black until
  the daemon was restarted. **You can see this change with `walpe-daemon --windowed`:**
  the preview window no longer pauses itself when it is made fullscreen and has the
  focus.
- X11, `walpe-daemon --monitor N`: a fullscreen window on another monitor that reaches a
  few pixels into the daemon's monitor no longer pauses it. The monitor holding most of
  the window is looked for among all monitors of the screen.
- Wayland: with two connectors whose names differ in one letter only (`DVI-D-1` and
  `DVI-I-1`, `HDMI-A-1` and `HDMI-B-1`) a window on the second monitor paused the first
  one. This also concerned "pause when covered" and the pause for a focused application.
- Wayland: a window that starts in fullscreen pauses its own monitor only. For the moment
  between its appearing and the compositor naming its output (18 to 122 ms on sway 1.9)
  it paused every monitor. The first window a session ever shows still does.
- Shared rendering (two monitors of one size showing the same scene are rendered once):
  a paused monitor that follows the other one sleeps. It was woken for every frame of the
  monitor that draws, 52 times a second in the test, to find that it had nothing to show.
- Web wallpapers: a page opened while everything is paused (`walpe-ctl pause`, a monitor
  that is covered or has a fullscreen window) is paused like one that was running. It
  was not: the page kept running for the whole pause - about 30 animation callbacks a
  second in the test, and it was never told `setPaused` - behind a picture that stayed
  black, because the one frame a paused monitor took was the one from the moment loading
  ended, before the page had drawn. A new page now has one second after it finished
  loading for its first picture, a paused monitor shows what it draws in that second,
  and then the page is paused: in the test 1.8 seconds after it was opened, after 18 to
  26 animation callbacks, and from there neither its processes nor the daemon used CPU
  time worth counting. The same on both page hosts. Test: `web-wayland` / `web-x11`,
  case `pausedopen`.

### Security

What a wallpaper's files, a web page, a music player or another program on the machine
could do to the daemon. None of these was seen being used.

- Web wallpapers, the file server that hands a page its files (it listens on 127.0.0.1,
  where every program on the machine can connect):
  - a request for a part of a file - what a page sends to play or to seek in a video or a
    sound - made the daemon copy the whole file into its memory for as long as the answer
    was on its way: 200 MB for a 200 MB clip, per request. A part is now answered without
    a copy. The copy was also where a file that shrank meanwhile (a wallpaper being
    updated) ended the daemon;
  - the body of a request was read whole into memory before anything was checked: one
    request of 192 MB took the daemon's memory up by 388 MB for good. A body is read and
    dropped piece by piece now;
  - connections that sent nothing stayed open for ever: a thousand of them used up the
    daemon's open files, and `walpe-ctl`, the tray and the window then said the daemon
    was not running. At most 128 are open; for one more, the one that has waited longest
    is closed.
- Web wallpapers: a server that answers "login required" (401, or a proxy with 407) made
  WebKit put its own name-and-password dialog over the wallpaper, which nobody can
  answer; a page behind such a proxy was never shown. The request now goes on without a
  login.
- Pictures:
  - BMP: a file of 60 bytes that calls itself a BMP could make gdk-pixbuf's decoder
    reserve up to 4 GiB of memory, because the decoder takes three sizes from the file's
    first bytes before it checks anything. Those three fields are checked first now,
    wherever a BMP can come from: a texture, a layer picture, a picture wallpaper, a
    preview, cover art. A BMP that fails is not shown; a BMP as the format defines it
    passes. Found by fuzzing;
  - layer pictures of the old scene format are read by the decoder of their own format
    only: a file of another kind under a picture's name (SVG, XPM, PNM, a Windows icon
    ...) no longer reaches that kind's loader. Damaged files are taken as before, with
    one exception: a progressive JPEG that ends early is refused there now, as scenes
    of the current format always did; the part that had been decoded used to be shown
    when the cut lay in the picture's data;
  - a JPEG layer picture of the old scene format that is larger than a texture and has
    to be held whole by its decoder (progressive) is refused beyond 256 MiB of that store:
    136 bytes of file could take gigabytes.
- Library window: the preview picture a wallpaper names is one of its own files. A name
  with `..`, or a link in the folder, could put any picture you can read on the
  wallpaper's tile.
- Deleting a Workshop item no longer waits for ever, or reads gigabytes, when another
  item's `project.json` is a pipe, a device or a huge file.
- Music players (media integration): a cover file that was cut short while it was being
  decoded ended the daemon; it is read into memory first now. What a player says about
  its track is bounded: a repeated entry is freed (a program with a player name could
  grow the daemon by megabytes per message), and title, artist, album and the other
  texts are kept up to 4096 bytes.
- Video import and conversion: the `ffmpeg` of an import or of a conversion ends with
  the daemon. It used to go on after the daemon was killed or had crashed, without
  anything watching its limits of time, size and disk space.
- Two files that name wallpaper paths are written for your account alone (mode 0600),
  like `config.json`: the marker of wallpapers being loaded next to the control socket,
  and the tray's list of recent wallpapers.
- Log lines: control characters in names and texts of a wallpaper, and in what a scene
  script logs or throws, are shown as `\xNN`. A name with a line break in it could
  start a log line of the wallpaper's own wording, and an escape sequence in it reached
  the terminal the log was read in. Not covered: shader compile logs, the error texts
  of libraries, the daemon's own lines and the video and web modules.
- "Adjust layers": a layer's name is shown as one line of at most 200 characters,
  without control or direction characters; a name of any length with line breaks went
  into the list as it was.
- A layer key with ten digits (`walpe-ctl layer`, `config.json`) overflowed a number
  before it was refused; nothing came of it with the compilers in use.
- [SECURITY.md](docs/SECURITY.md) has a new section on web wallpapers: what a page gets
  (the file server, network access - on by default, also to the local network -, where it
  may navigate, which permissions and dialogs it does not get) and what WebKit's sandbox
  keeps from a page process.

### Other

- `walpe-ctl mem` says where the daemon's memory is: resident memory by kind of mapping
  and by name, the allocator's numbers, descriptors, threads, per monitor the OpenGL
  objects of its context (textures by size, buffers, programs) and what its wallpaper
  holds, and counters since the start. `mem --json` saves a report and `mem --diff A B`
  shows what differs between two. `mem --events on` starts counting how often textures,
  framebuffers, buffers and programs are made and deleted and pixels are read back;
  `--events off` stops it. The report is computed when it is asked for and at no other
  time: no timer, no thread, nothing per frame; nothing counts the OpenGL events before
  they are switched on. File names in the report are printed with their control
  characters made visible.
- A daemon that has shown a web wallpaper gives WebKit back. The web module (WebKitGTK,
  JavaScriptCore, GTK) cannot be unloaded, so after the last web wallpaper had been
  replaced it stayed in the daemon until the daemon was stopped: about 50 MB of its
  resident memory on the machine this was measured on, three threads (one of them wakes
  every five seconds) and GTK's connection to the display. Now the daemon restarts
  itself in place at the wallpaper change that leaves no monitor with a web wallpaper,
  the way it does when the monitor layout changes: same process, the control socket
  stays open, wallpapers and settings come back from the configuration. Every monitor
  loads its wallpaper again and shows nothing until it has, as at a start (one second
  without a picture on both monitors where it was measured); a wallpaper that then fails
  to load does not bring the web wallpaper back. At most once a minute, never on a
  timer, not while an import or a Workshop download runs or the copy of a scene's video
  is being made, and not when the settings cannot be saved (the restart would bring back
  the wallpapers that are saved). `walpe-ctl set web_unload=off` keeps the module loaded
  as before;
  `walpe-ctl status` shows `modules`. The video module (FFmpeg) is small and stays
  loaded.
- The daemon, `walpe-video.so` and `walpe-web.so` have to come from the same build, as
  before: the number of the module interface changed, and a module of 0.13.0 next to a
  daemon of 0.14.0 is refused.
- README: "One scene's picture depends on reused effect targets" is no known limitation
  any more; 0.13.0 fixed it (a layer with a tint and effects keeps its effects).
- Less work while nothing happens:
  - the daemon no longer sends its whole status to the library window and the tray for
    requests that only read (the performance panel asked every 2 seconds: 5 kB each time
    to both);
  - the tray no longer rewrites its list of recent wallpapers for every status line when
    two monitors show different wallpapers (a new file, two `fsync` and a rename each
    time, for every pause, resume or cover change);
  - the library window no longer lays itself out, draws and queries the X server for
    every status line of the daemon that says what the last one said;
  - the library window's main thread no longer wakes every 2 seconds for a performance
    panel that is closed;
  - the animated previews of a library window that is not on screen - on another
    workspace or tag, minimised - stop playing and give their frames back (up to 48 MiB,
    and a timer 20 times a second);
  - an open performance panel no longer builds its line anew for every answer of the
    daemon that says what the last one said;
  - X11: the look at the windows behind "covered" and the focus rule asks each window
    for its attributes only: 22 requests to the X server per focus change with eight
    windows where it was 53. What it finds is the same;
  - Wayland: a window's new title no longer makes the daemon go through the window
    list (a player or a browser changes its title all the time);
  - scenes: a layer that puts no pixel on the frame - parked outside the view, or scaled
    to nothing - no longer keeps the whole scene drawing at the full frame rate for an
    effect on it that follows the time. The picture is the same: such a layer drew
    nothing before either.
- X11: a wallpaper window that another program destroys (a script that tidies up
  windows, `xdotool windowclose` on the wrong id) is made again. The daemon did not
  notice: that monitor showed the root window for as long as the daemon ran. It now
  restarts itself in place, at most once in ten seconds.
- A video conversion that cannot start, or is cancelled, leaves no memory behind.
- A lost GL context (suspend and resume, a GPU reset) no longer leaves a black monitor
  and a log line per frame: `walpe-ctl status` names it per monitor (`swap_error`), and
  the daemon restarts in place once, at most once a minute. Tried by making swaps fail
  on purpose; no driver here loses a context.
- A restart in place that cannot be begun (the daemon's binary can no longer be
  executed, as after an update that removed it) leaves the daemon running instead of
  ending it; `walpe-ctl status` has `restart_failed`.
- The unfinished copy of a scene's video no longer stays in the cache when the daemon
  ends while the copy is being started.
- The daemon's log file is capped from the start. The cap (16 MiB, `WALPE_LOG_MAX_MB`) was
  looked at only with every 256th line of a run and never for a file a shell had opened
  with `> file`: a daemon started from an autostart entry at every login, saying little
  each time, let the file grow for ever.
- "Adjust layers": two layers with the same name whose ids in `scene.json` are negative
  (other than -1) got the same key, so an adjustment saved for the second one moved the
  first; and a layer with an id of ten digits could not be adjusted at all. Every layer
  has a key of its own now. Keys of layers with ordinary ids are what they were: saved
  adjustments keep their meaning. Found by fuzzing.
- Library window:
  - the performance panel asks the daemon nothing while its window is minimised or on a
    workspace that is not shown, and the animated previews stop there too. On X11 this
    now also holds under tiling window managers that never mark a hidden window (i3,
    bspwm, awesome, xmonad, dwm): 486 wake-ups of the window and 222 of the daemon per
    minute on a hidden i3 workspace before, none now;
  - previews kept in memory are given back while the window is not on screen;
  - with the daemon stopped the window no longer asks for it every 2 seconds (the
    performance panel: every 10 seconds);
  - a wallpaper that is copied into the library slowly is shown complete without Rescan;
  - the search ignores accents (é = e, ü = u) and treats i, ı, İ and I as one letter;
  - screen readers get names for the choices and the search field;
  - an engine that is running but does not answer (stopped, hung, its socket's queue
    full) is called "Engine not responding" for as long as that lasts. It was taken for
    stopped after 44 seconds and for "keeps crashing" after 66, and a restart was tried;
  - the error of a wallpaper that could not be loaded no longer stays as the tool tip
    of the status line after the line itself is gone;
  - "(paused)" in the line that says what the monitors show appears and goes with the
    pause (within a second); it used to change only with a wallpaper or the layout.
- `walpe-ctl quit` (and "Quit" in the tray) stops the daemon for good while the library
  window is open: the window used to start it again at once. A daemon that crashed or
  was killed is still started again by an open window.
- `walpe-ctl` says "running but did not answer in time" for a daemon that is busy, where
  it said "not running"; and it ends with status 1 when its output cannot be written
  (a full disk, a closed pipe). Wayland: the same answer comes while the daemon waits
  for a compositor to come back (the known limitation under 0.13.0 below still says
  "not running"); what it was sent meanwhile is not carried out later, as before.
- Import: a video with `%d` in its name (`take %03d.mp4`) got its thumbnail under another
  name, replaced a picture of that name next to it and was shown without a preview.
- Import: a width, height or rotation in what `ffprobe` says about a video that no
  whole number can hold (`1e30`) was converted in a way C leaves undefined; on x86-64 it
  happened to come out as "no size". Every build now takes it as that. Found by fuzzing.
- Build: **the tests and the developer tools are no part of a build unless asked for.**
  The new build option `tests` is off by default, and a build directory set up without it
  has no test, no test program and no developer tool: `ninja`, `meson compile` and `meson
  install` build the installed programs and nothing else. In 0.13.0 only `./build.sh`
  left them out; `meson install` and a plain `ninja -C build` built every test program
  (about a hundred) first. To run the tests: `./build.sh -Dtests=true` (or `meson setup
  build -Dtests=true`; for a directory that exists `meson configure build -Dtests=true`),
  then `meson test -C build`. `./build.sh` builds the installed programs only, as before,
  with the option or without. The developer tools (`sceneshot` ...) were never built by
  `meson test`, whatever the section of 0.13.0 says: one is built by its name (`ninja -C
  build sceneshot`), in a directory with `-Dtests=true`.
- `build.sh`: `WALPE_JOBS=0` meant `ninja -j 0`, which is no limit on parallel jobs; 0
  now means the default (the number of CPUs). A relative `WALPE_BUILDDIR` is taken from
  where the script is called; it was taken from the source directory.
- `walpe-ctl set web_accel=<the value it has>` answers "ok" like every other key; it was
  an error ("nothing to set", exit status 3).
- Library window: an arrow, Home, End or Page key pressed anywhere in the window (in the
  search field, for one) ended the rule that gives the wallpaper grid 64 % of the width;
  from then on the grid kept its width in pixels when the window changed size. Only moving
  the handle itself ends the rule now.
- `walpe-ctl help` marks `script_texanim` as a switch that loads the wallpaper again.
- Issue forms: they said that WalPE draws on X11 only; and the bug form now says that the
  output of `walpe-ctl status`, which it asks for, carries the name of the Steam account
  (`steam_user`) besides the home path.
- Documents corrected against the code: the two picture-changing fixes of 0.13.0 in the
  README's summary, the OpenGL ES shader size limit in SECURITY.md, two rows of
  SETTINGS.md (where `status` shows the window system; which monitors a fullscreen window
  across several pauses).
- Tests: a test that cannot run at all - no OpenGL context, or nothing but script checks
  in a build without QuickJS - ends with exit status 77, so `meson test` counts it as
  skipped; 13 such tests ended with 0 and were counted as passed. `meson test daemon-life`
  builds the web and video modules it uses (in a fresh build directory its video case was
  left out without a word). New or extended: scene sound against a slow and a refusing
  sound server; files of another kind under a picture's name; an effect with 1 to 33
  passes; rope trails at 10 to 1000 frames a second; the working size of effects on a
  colour-only layer; scene switches that reload once; more of the fullscreen rule on X11,
  and its case no longer ends in a crash of the test script when the X server refuses a
  connection for a moment; the converted copies of scene videos (`scenevideo`,
  `daemon-scene-video`); the memory report (`memreport`); real pages in WebKitGTK end to
  end on both page hosts (`web-wayland`, `web-x11`, `scripts/webcheck.py`). Fuzz harnesses
  for the parsers of wallpaper files (`tests/fuzz`: packages, textures, MP4, GIF and JPEG
  checks, JSON, puppets, particles, shader text, sound files, scenes) for libFuzzer
  (`-Dfuzz=enabled`, clang); their seeds and every input that once broke a parser run in
  the suite as `fuzz-regress-<target>`; 22 targets in all, with the control socket,
  `config.json`, Steam's replies, previews, the library scan and layer keys. From the
  review: the web file server (request bodies, idle connections, parts of files), the
  login answer, a stand-in music player (`media-player`), import (`%d`, a child without
  its daemon), old-format layer pictures, the tray and the library window next to a real
  daemon (`tray-life`, `ui-life`).

### Known limitations

- See the state of testing at the top of this section. The full list is in
  [README.md](README.md#known-limitations).

## 0.13.0 "PINK" (not released)

The development version. It contains every fix of 0.12.2. **Everything listed here was
built and tested only on Mesa's software renderer (llvmpipe), a headless Wayland
compositor (sway 1.9), a PulseAudio null sink and - the build for 64-bit ARM - under
emulation. Nothing of it has been run on real AMD, Intel, NVIDIA or ARM hardware or on a
real Wayland desktop.** On NVIDIA's driver under X11 with default settings it is meant
to behave as 0.12.2 does, apart from the scene features, two fixes that change the
picture of some scenes (layers with a tint and effects, rope trails; under Scenes) and -
with more than one monitor - the fullscreen pause rule below. What was compared, on the
software renderer: the development
tree before the fixes of 0.12.2 were merged in with
release 0.12.1, call by call (four scenes: the same OpenGL calls) and picture by picture
(the 91 test scenes: the same frames with the new features off); then the pictures of
the final tree with those of that earlier tree.

### Wayland

- A Wayland backend next to the X11 one: one layer-shell surface per output on
  compositors that offer the layer shell (sway, Hyprland, KDE Plasma and others; not
  GNOME's Wayland session). The daemon picks the backend when it starts: `walpe-ctl
  backend`, `walpe-ctl set backend=auto|x11|wayland`, `walpe-daemon --backend`.
- Pause rules get their information from the compositor and from logind where the
  session offers it; `walpe-ctl status` (`pause_rules`) says which rule can work. New:
  pause after a time without input (`walpe-ctl set idle=N`).
- The tray icon uses StatusNotifierItem on Wayland sessions; the window follows the
  desktop's dark / light preference there.
- Web wallpapers on a Wayland session run through Xwayland by default.
- Build options `-Dx11` and `-Dwayland`. `./build.sh -Dwayland=disabled` gives the
  X11-only build, in which the daemon and the web module link no Wayland library.
- A compositor that accepts the connection and does not answer within 10 seconds counts
  as absent: the daemon takes X11 when there is an X display, else it ends and says so.
  The connection of the pause rules gives up after 5 seconds.
- A per-monitor wallpaper or render resolution that was stored under the X11 name of an
  output (`HDMI-1`) is found on Wayland, where the same connector is `HDMI-A-1`.
- Wallpapers were shown on a headless sway 1.9 only. Known and left as they are: a
  wallpaper that is not shown keeps being drawn when "pause when covered" is off; a lock
  screen pauses through "pause when covered" only, unless the locker tells logind; with
  no monitor left `walpe-ctl status` still lists the monitors; while the daemon waits for
  a compositor to come back, `walpe-ctl` says it is not running, and what it was sent is
  not carried out later; a session started with a one-time socket (`WAYLAND_SOCKET`)
  cannot be restarted in place.

### Graphics cards

- `walpe-ctl gpu` lists the graphics devices; `walpe-ctl set gpu=auto|integrated|
  discrete|<device>` chooses the one that renders on a machine with two. `auto` (the
  default) changes nothing.
- An OpenGL ES 3.1 / 3.2 path for GPUs without desktop OpenGL 3.3 (`walpe-ctl set
  gl_api=auto|gl|gles`); effect shaders are translated to GLSL ES. On Mesa's software
  renderer every effect shader of the 91 test scenes compiles; pictures differ slightly
  from desktop OpenGL, because OpenGL ES has no texture LOD bias (minified textures are
  a little softer).
- On OpenGL ES an effect shader with more than 256 KiB of source is not compiled: a
  warning names it and its effect is not drawn. A source of that size is not translated
  to GLSL ES, and was handed to the driver as it was written. Desktop OpenGL still
  compiles such a source as written.
- `walpe-ctl gpu-stats`, and the same figures in `walpe-gpu-report` and in a profile: GPU
  busy, memory, power and temperature from the kernel's statistics on AMD and Intel.
- Driver hints of `gpu.conf` are handed to Mesa drivers where Mesa has a counterpart.
- Fallbacks for drivers without S3TC, with a smaller largest texture, or without float
  render targets.
- With `gpu=auto` the daemon does not read the PCI id list when it starts, and on
  NVIDIA's driver the GPU figures of `walpe-ctl status` read no file of `/proc` or
  `/sys`.

### Render resolution

- A render resolution per monitor: `native` (the default), 720p to 4K, or any size
  (`walpe-ctl resolution`, Engine settings > Resolution). The window asks once about new
  monitors; an existing installation is not asked and nothing changes size.
- Imported and converted videos are no longer tied to 1080p (`video_resolution`; a
  configuration from an older version keeps 1080p until you change it).
- Effects on colour-only layers of a scene are computed as wide as the longer side of
  a chosen render size where that is more than 2048 pixels (0.12: 2048 at most), and such
  a layer is sharper then. At a monitor's own resolution (`native`, the default) they
  stay at 2048 pixels as in 0.12, also on a monitor wider than that.

### Scenes

- Camera parallax, camera shake, bloom, sprite-sheet layers and their script control,
  and a script camera (zoom and position). Each has a switch (`walpe-ctl set bloom=off`,
  ...). **They are on by default and change the picture of the scenes that ask for
  them:** with default settings 16 of the 91 test scenes look different from the 0.12
  releases, because bloom, camera shake, sprite sheets and the script camera are on.
  `walpe-ctl set bloom=off camera_shake=off sprite_sheets=off script_camera=off` gives
  the earlier pictures back, each switch for its own feature. The look of the camera
  shake is an estimate; HDR bloom is not implemented. Limits: the sprite animations of
  one scene have at most 1048576 frames together (a layer beyond that shows its first
  frame), and a scene zoom that no float holds counts as zoom 1.
- The switch `script_texanim` (scripts drive sprite-sheet animations) is read when a
  wallpaper loads, as `sprite_sheets` is: changing it loads the wallpapers again.
  Switched off on a running scene it left the animations as the scripts had last set
  them, which is not the picture of a scene loaded with the switch off.
- A layer with a tint and effects keeps its effects. Such a layer could be drawn without
  them, depending on whether an effect target was new or taken back from the pool, so
  its picture was not always the same. Two scenes of the test library draw another
  picture now, whatever the feature switches say.
- A rope trail of a particle system is as long in seconds as its scene says at every
  frame rate. It was longer at low frame rates (a trail of 24 points over 0.2 seconds:
  twice as long at 60 Hz, four times at 30 Hz). Scenes with rope trails can look
  different at 60 Hz and below: the trails are shorter there.
- The Fluid Simulation effect runs on the grid its file declares, at a fixed 60 steps a
  second whatever the frame rate (`fluid_sim`, `fluid_quality`).
- Scene sound layers (WAV, MP3, FLAC, Ogg) through PulseAudio / PipeWire, **off by
  default**: `walpe-ctl set scene_sound=on volume=0.5`; `mute`; per wallpaper
  `walpe-ctl setprop walpe_sound=on|off`. The `volume` setting now has an effect.
  The sound layers of a scene open their streams one at a time, a few milliseconds
  apart: asked for in the same instant, some were refused by the sound server. A
  connection it still refuses is asked for again at once. A layer waits for its turn for
  a quarter of a second at most; after that it plays into nothing and asks again 5
  seconds later. Measured against a null sink by recording it; nobody has heard it.

### Video

- Hardware decoding chooses its decoder by what the machine and its FFmpeg offer and by
  the GPU that renders (`walpe-ctl set hwdec=off|auto|<api>`: VA-API, NVDEC, VDPAU, Vulkan
  Video, V4L2); on NVIDIA's driver NVDEC stays first. Optional zero-copy
  (`video_zerocopy`). Both off by default.

### Pausing

- "Pause for fullscreen apps" (`pause_fullscreen`) pauses the monitor the focused
  fullscreen window is on; the wallpapers of the other monitors keep running. Up to 0.12
  a fullscreen window on one monitor paused every monitor. **This changes the default
  behaviour on a machine with more than one monitor**; `walpe-ctl set fs_all=on`
  (`pause_fullscreen_all`, also a switch in the settings window) brings the old behaviour
  back. On X11 the monitor is found from the window's rectangle: the monitor holding
  most of the window, whatever border or gap a tiling window manager keeps around it,
  and every monitor a window across several covers at least half of. On Wayland it is
  the output the compositor names for the window; where it names none, every monitor
  pauses as before and `walpe-ctl status` (`pause_rules`) says so. Status reports
  `fullscreen` and `fullscreen_pause` per monitor. Tested with two monitor rectangles
  on Xvfb and two outputs of a headless sway 1.9; not on real monitors.

### Other

- Pictures from outside go to fewer decoders. A picture or GIF file chosen as the
  wallpaper is shown only as PNG, JPEG, GIF, BMP, TIFF, WebP, TGA, AVIF or JPEG XL: a
  file of another kind (an SVG document, a Windows icon ...), which earlier versions
  handed to whatever loader gdk-pixbuf had for it, is refused now. Cover art of a wallpaper with
  media integration is decoded as JPEG, PNG, GIF, WebP or BMP only
  ([SECURITY.md](docs/SECURITY.md)).
- The source builds for 64-bit ARM (aarch64) as well as x86-64, with meson cross files:
  cross-compiled with 0 warnings, and 65 of the 68 tests that need neither a display nor
  an OpenGL context pass under emulation. Not run on ARM hardware. OpenGL ES 3.1 is the
  least an ARM GPU must offer.
- Test setups for AddressSanitizer / UndefinedBehaviorSanitizer and ThreadSanitizer,
  end-to-end tests on a headless Wayland compositor.
- Library window: the wallpaper grid takes 64 % of the window's width by default (6
  columns at 1920 pixels) and the details panel the rest, at least 460 pixels. The share
  follows the window's size until you move the handle between the two; from then on
  yours is kept. Before, the handle stood at a fixed 780 pixels, and in a window that a
  window manager tiles or maximizes after showing it the grid could end up with two
  columns. The position is not saved across starts.
- Build: `./build.sh` builds the installed programs and modules only (the new ninja
  target `programs`), which makes a fresh build much shorter. The test programs and the
  developer tools are built by `meson test`, or one by name (`ninja -C build
  sceneshot`); a plain `ninja -C build` builds them too. The installed binaries are the
  same as before.
- Two changes that are meant to change no picture: an effect takes memory for the passes
  it has (the pass table of an effect with one or two passes took 44 kB and takes 1.4 or
  2.8 kB now), and a control point's `flags` in a particle file is converted like the
  other numbers of a wallpaper file, so a value that no integer holds gives the same
  result on every machine.
- Tests: in a build without QuickJS (`-Dscenescript=disabled`) `fx-alpha` and `layers`
  leave out the checks that need a script instead of failing, and `sig-wl` no longer
  waits for ever where `TMPDIR` is too long for a socket path (it makes its directory in
  `/tmp` then).

### Known limitations

- See the state of testing at the top of this section. The full list is in
  [README.md](README.md#known-limitations).

## 0.12.2 "Amplifier Worship"

Security fixes and bug fixes for 0.12.1; no new features. Updating is recommended for
everyone who opens wallpapers, previews or Workshop pages that other people made.

### Security

What a hostile file or answer could do in 0.12.1, most serious first. All of these are
closed; the limits are listed in [docs/SECURITY.md](docs/SECURITY.md).

- GIF files (as a wallpaper, a preview or cover art) whose frames do not lie inside their
  picture, as the format requires, were given to the decoder unchecked. They are now
  refused before anything of them is decoded.
- A shader file with one extremely long statement crashed the graphics driver's shader
  compiler, and the daemon with it.
- A video file could keep the daemon busy for minutes to hours while it was opened; the
  daemon answered nothing meanwhile.
- A font file named by a text layer could take hundreds of megabytes and seconds of
  processor time, and could be of any format FreeType reads. Only TrueType / OpenType
  fonts with a bounded number of faces are loaded now.
- A texture file could hand its bytes to picture decoders that were meant to be out of
  reach (Windows cursor and icon formats), and cost seconds of processor time.
- A preview picture could reach decoders outside the allowed formats (SVG, XPM and
  others) in the window, and a small JPEG preview or cover picture could take half a
  gigabyte of memory while it was decoded.
- Cover art fetched over http(s) for a wallpaper with media integration could unpack to
  gigabytes inside the daemon.
- An answer to a Workshop request could crash the window (deeply nested data behind a
  comment or an unusual string).
- A scene of a few hundred bytes could take gigabytes of texture memory through text
  layers, hold millions of particles, or keep the render thread busy for minutes with
  particle systems. Text layers and particles now have totals.
- Shader files of megabytes could stall the render thread for minutes, and names from a
  wallpaper's JSON could push a shader past its size limit.
- A scene script could run its own code at a moment when the engine held a pointer into
  one of the script's audio arrays (no memory error was reached with the QuickJS that
  was tested).
- With `fx_quality=fast` (not the default) a crafted shader crashed the daemon.
- A wallpaper with thousands of options, or with very long option labels, froze the
  window for seconds to minutes.
- `walpe-ctl` printed titles and names from wallpapers and Workshop items with their
  control characters, so an escape sequence in them was obeyed by the terminal.
- A named pipe called `scene.pkg` in a wallpaper folder blocked the load for good; a
  symbolic link there was followed to a package anywhere on the disk, and the options
  window read linked files outside the wallpaper's folder.
- A program of your own account that sent requests to the control socket and never read
  the answers stalled the daemon's other clients and web wallpapers.
- Smaller ones: time zone names from a script that are no time zones, a text size beyond
  what the text library can hold, the script log limit after weeks of logging, reserved
  names in a package.

### Fixes

- A monitor that was plugged in or removed while the daemon was starting was never
  noticed: the wallpapers kept the old layout.
- A paused scene that had to draw a frame (a window moved away, a layer adjusted) moved
  on in time: script clocks jumped and fell back, timers stalled, the paused picture
  changed.
- Right-to-left text (Hebrew, Arabic) in a wrapped text layer was placed on the wrong
  side, or not drawn at all.
- A script on a property that also has a keyframe animation made the value run away
  once the animation stood still.
- Particles: an object that a script stops and plays again fires its burst again; a
  mirrored property animation fires its events at both ends.
- Window: a wallpaper the engine cannot load is no longer reported as set, the reason is
  shown; "Add folder" works on a new installation and says which folders could not be
  added; an import no longer copies files the library never lists; a number field that
  is emptied keeps its value (the frame rate limit used to fall to 5); a refused Workshop
  install is shown also after its dialog was closed.
- A setting that cannot be saved (full disk, unwritable home) is reported instead of
  answered with "ok": `walpe-ctl` prints the reason, `walpe-ctl status` has
  `config_save_error`.
- `walpe-ctl set`: mistyped values of `skip_hidden` and negative margins are refused
  instead of ignored; `walpe-ctl status` shows every saved setting; `walpe-ctl setprop`
  stores text options as typed.
- Switching web acceleration while an import or a Workshop download runs no longer ends
  it.
- Web wallpapers: a WebKit start-up test that never finishes is ended after 40 seconds
  and reported, instead of leaving every web wallpaper black; its child process is no
  longer left behind when the daemon ends.
- Memory that was never given back: the album cover of media integration, wallpaper
  files with a second top-level JSON value, the script memory of a wallpaper whose
  script got stuck (up to 64 MiB per load), two frame-sized textures of a wallpaper that
  is no longer shown.

### Performance

Measured on Mesa's software renderer only; the pictures are byte-identical before and
after. What it gives on a graphics card has not been measured.

- Loading a scene: the render thread's processor time up to the first frame fell from
  1122 to 948 ms, 1874 to 1302 ms and 1736 to 1348 ms for three scenes of the test
  library (sprite analysis, repeated shader text work, sprites decoded twice).
- Refracting particle systems copy only the part of the frame they can read, and not
  again what the system before them just copied: in two scenes at 1920x1080 the pixels
  copied per frame fell from 13.7 to 7.9 and from 22.2 to 9.1 megapixels.
- Puppet layers are no longer sent to the graphics card again while their pose stands
  still: 9 to 4 and 3 to 1 uploads per frame in two scenes.

## 0.12.1 "Absolutego"

The first public version.

### Wallpapers

- Plays Wallpaper Engine scene, video and web projects, GIFs and still images on
  Linux / X11.
- Scenes: layers with parent transforms, effects (Wallpaper Engine's shader dialect
  translated for OpenGL 3.3), particle systems, puppet warp layers, video textures, text
  layers, and a subset of SceneScript on QuickJS. Both scene formats are read.
- Wallpaper options (user properties) in the library window and with `walpe-ctl props`,
  `setprop` and `resetprops`; "Adjust layers" moves, resizes or hides single layers of a
  scene, saved per wallpaper.
- Media integration (title, artist, album art and playback state of any MPRIS player) and
  audio-reactive wallpapers (PulseAudio or pipewire-pulse).
- Wallpaper Engine's builtin effects, shader headers, models and particle sprites are read
  from your own Wallpaper Engine installation; WalPE ships none of them. Video, web, GIF
  and picture wallpapers do not need it.

### Engine

- One render thread with its own EGL context per monitor; monitors of the same size that
  show one scene share one rendering.
- Quality presets `max`, `high` (the default), `balanced` and `low`; every knob can also
  be set on its own, for all wallpapers or for one.
- Work that would not change the picture is left out: unchanged frames are neither drawn
  nor presented, still layers and still effects are drawn once and kept, textures are
  stored at the size they are shown at.
- A wallpaper pauses under a fullscreen window, while its monitor is covered, while the
  display sleeps, and optionally while an application has the focus or on battery power.
- Off by default: rendering on the CPU (Mesa llvmpipe), hardware video decoding, web pages
  on the GPU.
- `walpe-ctl profile` measures GPU time per layer and effect pass; `walpe-gpu-report`
  collects numbers for a report.

### Programs

- `walpe-daemon` (the engine, no GTK), `walpe` (GTK4 library window with wallpaper
  options, settings and a Workshop tab), `walpe-ctl` (command line client), `walpe-tray`
  (tray icon).
- Steam Workshop: browse, search and download through `steamcmd`; existing Steam
  libraries of Wallpaper Engine are found and listed. WalPE never handles a Steam
  password.
- Video import: whatever ffmpeg can read becomes H.264 in MP4, at most 1080p.

### Safety

- A wallpaper's files are treated as untrusted input: every parser has size limits, file
  names stay below the wallpaper's folder, scripts and shaders have CPU, memory and size
  limits, web pages are served from a token-protected loopback server. See
  [docs/SECURITY.md](docs/SECURITY.md).

### Known limitations

- X11 only, no sound, and run on one graphics card only (an NVIDIA laptop card; the last
  set of changes on Mesa's software renderer only; nothing on AMD or Intel graphics). The
  full list is in [README.md](README.md#known-limitations).
