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
| picture inside a `.tex`, layer picture of the old scene format | PNG, JPEG, WebP (plain picture files also GIF, BMP, TIFF; old-format layers also TGA, AVIF, JXL), each read by its own decoder only |
| `.json` | 32 MiB, 512 levels of nesting |
| `scene.pkg` | entries stay inside the cache copy, at most 32 path components, 32768 files + directories, 4 x the package's size + 16 MiB written; the cache holds 1 GiB (`WALPE_PKG_CACHE_MB`) |
| puppet model `.mdl` | 256 MiB |
| video (`.mp4`, video inside a `.tex`) | 4194304 frames (19 hours at 60 fps), timescale below 2^31 |
| text layer | 16 KiB of text, 8192 px |
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

A header is included once per file, whatever the `#include` line calls it (`"x.h"`, `"./x.h"`, a link).
Tests: `meson test -C build script-hostile fx-hostile scene-hostile` (the last two need EGL
surfaceless; `fx-hostile-test gl 1500` and `fx-hostile-test text 6000` run more seeds).

## What is trusted on the local machine

- The control socket is for your own programs: mode 0600, a connection
  from another account is dropped (root is accepted). Whoever can reach it
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


## Workshop data

What Steam's servers answer and
what a wallpaper's `project.json` says is treated as data from strangers. Replies are read up
to a size limit and at most 120 s, redirects are followed only to Steam's https hosts (never
for calls that carry the Web API key), preview pictures go to a decoder only when they are
JPEG, PNG, GIF or WebP (library previews also BMP and AVIF), and titles, tags, names and
descriptions are cut to sane lengths and cleaned of control and text-direction characters
before they are shown. The optional Steam Web API key lives in `~/.config/walpe/workshop.json`
(mode 0600), is sent to api.steampowered.com only and is never logged; WalPE never handles a
Steam password. Tests: `meson test -C build wsnet uitext thumbs steamsub workshop`.

