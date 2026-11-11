# Security notes

A wallpaper from the Workshop is a program somebody else wrote: scene scripts, shaders, web
pages, and files in formats that are parsed by WalPE. This file says how to report a security
problem, what WalPE trusts, what it limits, and where the limits are.

## Reporting a security problem

Please report a security problem privately, not in a public issue: on the repository's
**Security** tab, use the **Report a vulnerability** button (GitHub's private vulnerability
reporting). Such a report is visible only to you and the maintainers.

What to include:

- the WalPE version or the commit: the line `walpe-daemon <version> "<name>" up` in the daemon's log,
  and `git rev-parse --short HEAD` for a build from git;
- what input triggers the problem (a wallpaper file, a script, a shader, a web page, a reply
  from a server, a request on the control socket) and what happens: a crash with its
  backtrace, a file read or written outside the wallpaper's folder, a limit of this document
  that does not hold;
- if one is needed to show it, the Workshop id of a wallpaper or a small file. Attach it to
  the private report, not to a public issue.

## Limits on wallpaper files

| File | Limit |
|---|---|
| texture / picture | 16384 px per side and 8192 x 8192 px; all textures of a scene 1024 MiB (`WALPE_TEX_BUDGET_MB=<MiB>`, 0 = none), the largest are stored smaller beyond that |
| picture inside a `.tex`, layer picture of the old scene format | PNG, JPEG, WebP (plain picture files also GIF, BMP, TIFF; old-format layers also TGA, AVIF, JXL), each read by its own decoder only, also when only its size is asked for |
| GIF (wallpaper, preview, cover art) | checked before any of it is decoded: the picture and every frame at least 1 x 1 pixel, every frame inside the picture; a GIF that fails is refused like a damaged picture |
| `.json` | 32 MiB, 512 levels of nesting |
| `scene.pkg` | a regular file inside the wallpaper's folder (no pipe, no link that leads out); entries stay inside the cache copy, at most 32 path components, 32768 files + directories, 4 x the package's size + 16 MiB written; the cache holds 1 GiB (`WALPE_PKG_CACHE_MB`) |
| puppet model `.mdl` | 256 MiB |
| video (`.mp4`, video inside a `.tex`) | 4194304 frames (19 hours at 60 fps), timescale below 2^31; a sample size table that is repeated in the file costs nothing more |
| font of a text layer | a TrueType / OpenType font file or a collection of them, at most 256 faces and named instances in all and 512 tables per face; anything else is not handed to fontconfig, and the layer is drawn with a system font |
| text layer | 16 KiB of text, 8192 px, a font size of at most 15000 points; the text layers of a scene together 1024 MiB of texture (the same `WALPE_TEX_BUDGET_MB`, counted apart from the pictures), a layer beyond that is not drawn |
| particles | 20000 per system, 128 systems per particle object, 400000 alive in all systems together; a system at a limit emits no more until particles end |
| names of files a wallpaper refers to | below the wallpaper's own folder (or the Wallpaper Engine `assets/`): no absolute path, no `..`, no symbolic link that leads out, regular files only |

A file beyond a limit is not loaded (the log says which); the rest of the wallpaper is.

## Scripts and shaders

QuickJS version: wallpaper scripts are untrusted and run inside QuickJS, so its age matters.
`meson setup` warns ("QuickJS: quickjs.h has no JS_EVAL_FLAG_ASYNC ...") when the installed
library is older than the 2024-01-13 release, and the daemon says so once in its log. The
2021-03-27 release (still what Debian / Ubuntu package) lacks fixes for memory errors that plain
script can reach (CVE-2023-48183, CVE-2023-48184, CVE-2023-31922). Install a current
`dev-lang/quickjs` and rebuild, or build with `-Dscenescript=disabled`. (The check looks for
`JS_EVAL_FLAG_ASYNC` in `quickjs.h`; QuickJS has no version number in its header.)

Limits on what a wallpaper's scripts and shaders may cost (fixed, no switch):

| what | limit | beyond it |
|---|---|---|
| CPU time of all scripts per frame / per load step | 250 ms | the script (or timer) that was running is stopped for good; three times in a row with nobody to name (a promise reaction): every script of the wallpaper stops |
| one built-in call that does not return (regular expression, `join` of a huge array, also inside an argument conversion) | 1.5 s CPU | every script of the wallpaper stops, the wallpaper keeps running; its script memory (64 MiB at most) is not given back until the daemon restarts |
| script memory | 64 MiB | allocation fails inside the script |
| one log line of a script | 1024 bytes | cut, marked `[...]` |
| a number a script hands to a layer (origin, scale, angles, ...) / to an effect constant | +-1e9 / +-1e30 | brought into range; a vector with a component that is not a number is not applied |
| text a script hands to a layer in one call | 64 KiB | cut between two characters (a text layer shows 16 KiB of it) |
| "parent" chain of layers, also when a script re-parents at run time | 256 | the layer becomes a root |
| one shader file (`.frag`, `.vert`, included header) | 1 MiB | not read: the effect fails like one whose shader is missing |
| one shader with all its `#include`s | 4 MiB | further includes stay unexpanded (the effect then fails to compile) |
| one statement of a shader, macros expanded | 8192 tokens | the shader is not handed to the driver (a driver's compiler can run out of stack on a longer statement); the effect fails like one that does not compile |
| a shader the repairs look at | 256 KiB of source, 8192 bytes per line | a larger source, or a longer line, is compiled as written, without the repairs for Wallpaper Engine's dialect |
| a combo name from a wallpaper's JSON or a `[COMBO]` annotation | an identifier of at most 128 characters | anything else is left out of the shader source |
| a time zone name a script asks for | a zone of the system's zone directory | other names (files there that are no zones, `posix/`, `right/`) give no zone |

A header is included once per file, whatever the `#include` line calls it (`"x.h"`, `"./x.h"`, a link).
Tests: `meson test -C build script-hostile fx-hostile scene-hostile` (the last two need EGL
surfaceless; `fx-hostile-test gl 1500` and `fx-hostile-test text 6000` run more seeds).

## What is trusted on the local machine

- The control socket is for your own programs: mode 0600, a connection
  from another account is dropped (root is accepted). A request line may
  have 1 MiB, 64 connections may be open at once and 8 of them may
  subscribe to state changes; a reply the client does not take at once is
  queued and sent when the socket takes more, and a client that reads
  nothing for 3 seconds is dropped, so one that never reads cannot hold up
  the daemon's other clients. Whoever can reach it
  can do what you can do with WalPE: open any wallpaper folder (scene
  scripts, web pages), point `we_assets` at other shaders, run an import
  through ffmpeg or a download through steamcmd. Do not hand the socket to
  a sandboxed program.
- The socket, its lock file (`walpe.sock.lock`) and the crash-loop marker
  (`walpe.sock.loading`) live in `$XDG_RUNTIME_DIR` (without it: `~/.cache`)
  only when that directory is yours and nobody else can write to it. If it
  is not (`XDG_RUNTIME_DIR=/tmp`, a directory someone else created, one
  that is gone) they go to `<tmp>/walpe-<uid>/` (0700) and the log says so;
  if that name is taken by someone else too, no daemon starts and no client
  connects ("no safe place for the control socket"). The fix is a runtime
  directory of your own with mode 0700.
- `config.json` is written 0600 (it names your Steam account, every
  wallpaper path and what you typed into wallpaper options); a file from an
  older version is narrowed when the daemon reads it. `workshop.json` (the
  Steam Web API key) and `daemon.log` were 0600 already. `~/.cache/walpe`
  (unpacked wallpapers, converted videos, reports) is made 0700 by the
  daemon at every start.
- `walpe-video.so` / `walpe-web.so` are code that runs inside the daemon:
  a file that belongs to another account, that everybody can write to, or
  that sits in such a directory is not loaded (log: "module: ... not
  loaded"), and `WALPE_MODULE_DIR` has to be an absolute path.
- The environment the daemon is started with is trusted like the daemon
  itself (`LD_*`, `GST_*`, `WEBKIT_*`, `LIBGL_*`, `__EGL_VENDOR_LIBRARY_*`,
  every `WALPE_*` switch): both restarts in place hand the start
  environment on unchanged, plus `WALPE_IPC_FD` / `WALPE_LAYOUT_RESTARTED`,
  and close every other descriptor.


## Cover art of media integration

A wallpaper with media integration is shown the cover picture the music player on the
session bus names (a file, a `data:` URL or an http(s) address; any address a player names
is fetched, also a local one). At most 16 MiB are read, counted after unpacking for a
compressed http body; a picture of more than 8192 pixels per side is refused before any
pixel exists, a JPEG that its decoder has to hold whole is not decoded beyond 256 MiB of
that store, and a GIF is checked as in the table above. A known limit: the cover is decoded
by whatever picture loader the system's gdk-pixbuf offers for it, not by a fixed list of
formats (such a list is prepared and not part of this release).

## Parts added in 0.13

The same rule - a wallpaper's files are data from strangers - as applied in the new parts.
None of them has been attacked by anyone but its own tests.

- **Sound files** ([SOUND.md](SOUND.md)): looked up inside the wallpaper's folder or
  Wallpaper Engine's assets only (no absolute paths, no `..`, no links that lead out, no
  addresses), regular files of at most 2 GiB; the file's first bytes decide the format, and
  only four FFmpeg decoders can be reached (MP3, FLAC, Vorbis, Opus; WAV is read by WalPE
  itself); 8000-192000 Hz, 1-8 channels, at most 6 hours, 64 files per layer, 32 sound
  layers per wallpaper and 16 sounding layers in the whole daemon. A project cannot declare
  an option named `walpe_sound`. Tests: `scenesound`, `sounddec`.
- **Sprite sheets**: the frame table comes from the wallpaper and is checked before use
  (every frame on the picture and with an area, times that are numbers); a sheet that
  fails is refused. A script reaches the animations and the camera of its own scene only,
  and what it sets is clamped. Tests: `sprite-sheet`, `scene-feat`.
- **Fluid simulation**: an effect file gets a grid of at most 1024 cells on its longer side
  and no buffer larger than the driver's largest texture. Test: `fx-sim`.
- **Effect shaders on OpenGL ES** go through one more rewriting stage ([GPU.md](GPU.md))
  with the limits of the table above. Tests: `fx-essl`, `fx-hostile`.
- **Web wallpapers on Wayland**: by default the page's hidden window is an X window under
  Xwayland, with the selection guard of X11 sessions. The private page host (experimental,
  [WAYLAND.md](WAYLAND.md)) is a Wayland display inside the daemon to which only the page's
  toolkit is connected: there is no clipboard of anyone else on it, and copy and drag
  requests are cancelled. Test: `websec-private`. The wallpaper surface itself never takes
  the keyboard.
- **GPU choice**: for a `gpu` value other than `auto` the daemon starts short child
  processes of itself (`--gpu-probe`) and sets driver variables in its own environment;
  a variable that is already set is never replaced. `walpe-ctl gpu-stats` and the kernel
  section of a profile read `/proc/<pid>/fdinfo` of your own processes and `/sys` of the
  GPU, nothing of another user's process, and start no program but `nvidia-smi` (the
  command and the report script only, never the daemon)
  ([GPU-PROFILING.md](GPU-PROFILING.md)).
- **logind** is asked over the system bus for three facts (suspend, locked hint, active
  session); `WALPE_LOGIND_BUS=session` (tests) looks for it on the session bus, where any
  program of yours can stand in for it.

## Workshop data

What Steam's servers answer and
what a wallpaper's `project.json` says is treated as data from strangers. Replies are read up
to a size limit and at most 120 s, redirects are followed only to Steam's https hosts (never
for calls that carry the Web API key), preview pictures go to a decoder only when their first
bytes are those of a JPEG, PNG, GIF or WebP file (library previews also BMP and AVIF) and then
to the decoder of that one format (gdk-pixbuf is never left to pick one of its other loaders -
SVG, XPM, PNM, TGA, ICO ... - for them), a JPEG that its decoder has to hold whole (progressive,
or a scan per component) is not decoded beyond 256 MiB of that store, and titles, tags, names
and descriptions are cut to sane lengths and cleaned of control and text-direction characters
before they are shown. `walpe-ctl` writes the texts it takes from a reply (a Workshop title, the
layer and effect names of a scene, a text layer's text) with their control characters made
visible (`\x1b`), so an escape sequence in them never reaches the terminal; a reply printed as
JSON has them as `\u001b`. The optional Steam Web API key lives in `~/.config/walpe/workshop.json`
(mode 0600), is sent to api.steampowered.com only and is never logged; WalPE never handles a
Steam password. Tests: `meson test -C build wsnet uitext thumbs steamsub workshop`.

