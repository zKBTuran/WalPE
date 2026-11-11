project('walpe', 'c',
  version: '0.12.1',
  meson_version: '>= 0.60.0',
  default_options: [
    'c_std=c11',
    'buildtype=release',
    'optimization=3',
    'b_lto=true',
    'b_ndebug=if-release',
    'b_pie=true',        # executables are position independent (ASLR)
    'warning_level=2',
  ])

# The release's name, next to the version above (CHANGELOG.md says where the
# names come from).
# A point release keeps the name of its line. This is the one place that
# says it: the sources get it as WALPE_CODENAME, like WALPE_VERSION.
walpe_codename = 'Absolutego'

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
  '-fno-semantic-interposition',
  '-fomit-frame-pointer',
  '-fvisibility=hidden',
  '-ffp-contract=off',
  '-Wno-unused-parameter',
]), language: 'c')

add_project_arguments('-D_GNU_SOURCE', language: ['c'])

# Hardening (-Dhardening=false turns it off). The daemon parses files from
# the Steam Workshop, so the usual protections are asked for here instead of
# being left to whatever the distribution's compiler enables by default:
# stack protector, stack clash protection, control-flow protection (x86),
# fortified libc calls, format-string errors, and at link time read-only
# relocations with immediate binding and a non-executable stack. Each flag
# is probed and dropped when the toolchain does not know it. They are
# ordinary compile/link flags: -O3 and LTO keep them.
if get_option('hardening')
  add_project_arguments(cc.get_supported_arguments([
    '-fstack-protector-strong',
    '-fstack-clash-protection',
    '-fcf-protection',
    '-Wformat',
    '-Werror=format-security',
  ]), language: 'c')
  # _FORTIFY_SOURCE needs optimisation and is left alone under sanitizers
  # (they do the same checks, better); a toolchain that already defines it
  # keeps its own level
  if get_option('optimization') != '0' and get_option('b_sanitize') == 'none'
    opt = get_option('optimization')
    fortify_probe = ['-O' + (opt == 'plain' ? '2' : opt)]
    if cc.get_define('_FORTIFY_SOURCE', args: fortify_probe) == ''
      add_project_arguments('-D_FORTIFY_SOURCE=2', language: 'c')
    endif
  endif
  add_project_link_arguments(cc.get_supported_link_arguments([
    '-Wl,-z,relro',
    '-Wl,-z,now',
    '-Wl,-z,noexecstack',
  ]), language: 'c')
endif

# release builds: GObject type-cast macros (GTK_WINDOW (x), ...) are plain
# casts, as GLib recommends for production builds
if not get_option('debug')
  add_project_arguments('-DG_DISABLE_CAST_CHECKS', language: 'c')
endif

# CPU tuning (-Dnative_tuning=...). No hand-written SIMD kernels exist; this
# only lets GCC/clang auto-vectorise and schedule for the target CPU.
#   generic (default): no -march / -mtune / cache parameters: the binary
#             runs on any CPU of the architecture it is built for (portable
#             binaries, CI, other machines). `off` is the same (its earlier name,
#             kept for existing scripts).
#   alderlake: the maintainer's Gentoo make.conf COMMON_FLAGS minus -O3
#             (= optimization=3 above); `./build.sh --tuning=alderlake`.
#             The binary only runs on Alder Lake-class Intel CPUs
#             (AVX2/FMA/BMI/SHSTK...). This was the default of earlier
#             builds.
#   native:   -march=native; error if the compiler does not accept it.
#   auto:     -march=native when the compiler accepts it, else nothing.
# Each flag is probed with get_supported_arguments(), so an older GCC or clang
# drops the ones it does not know (e.g. clang has no --param cache sizes)
# instead of failing. The same flags go on the link line so LTO code
# generation (which happens at link time) uses them too.
tuning = get_option('native_tuning')
tune_flags = []
if tuning == 'alderlake'
  tune_flags = [
    '-march=alderlake', '-mabm', '-mno-kl', '-mno-pconfig', '-mno-sgx',
    '-mno-widekl', '-mshstk',
    '--param=l1-cache-line-size=64', '--param=l1-cache-size=32',
    '--param=l2-cache-size=18432',
  ]
elif tuning == 'native' or tuning == 'auto'
  tune_flags = ['-march=native']
endif
tune_args = cc.get_supported_arguments(tune_flags)
if tuning == 'native' and tune_args.length() == 0
  error('native_tuning=native but the compiler rejects -march=native')
endif
if tuning == 'alderlake' and not tune_args.contains('-march=alderlake')
  warning('native_tuning=alderlake: compiler lacks -march=alderlake ' +
          '(GCC >= 11 / clang >= 12); building without it')
endif
tune_args += cc.get_supported_arguments(['-pipe'])
add_project_arguments(tune_args, language: 'c')
if get_option('b_lto')
  add_project_link_arguments(cc.get_supported_link_arguments(tune_args), language: 'c')
endif

web_opt = get_option('web')
audio_opt = get_option('audio')
ffmpeg_opt = get_option('video')

glib      = dependency('glib-2.0')
gobject   = dependency('gobject-2.0')
gio       = dependency('gio-2.0')
gtk4      = dependency('gtk4')
jsonglib  = dependency('json-glib-1.0')
pixbuf    = dependency('gdk-pixbuf-2.0')
pangocairo = dependency('pangocairo')  # scene text layers
pangoft2  = dependency('pangoft2')    # text: fontconfig font map (pkg / WE fonts)
fontconfig = dependency('fontconfig')
epoxy     = dependency('epoxy')
egl       = dependency('egl')
# mpv backend removed (resource decision): ffmpeg-only video

x11       = dependency('x11')
xext      = dependency('xext')
xinerama  = dependency('xinerama')
lz4       = dependency('liblz4')  # WE .tex compressed mips
m_lib     = cc.find_library('m')

deps_core = [glib, gobject, gio, jsonglib, pixbuf, pangocairo, pangoft2, fontconfig, epoxy, egl, x11, xext, xinerama, lz4, m_lib]
# optional: XInput2 raw button events = desktop clicks for SceneScript cursor events
xi = dependency('xi', required: false)
if xi.found()
  deps_core += xi
endif
# optional: RandR 1.5 monitor names (per-monitor wallpapers are saved by
# output name, e.g. HDMI-0; without it monitors are named mon0, mon1, ...)
xrandr = dependency('xrandr', required: false)
if xrandr.found()
  deps_core += xrandr
endif

have_ffmpeg = false
deps_ffmpeg = []
if ffmpeg_opt != 'disabled'
  foreach d : ['libavcodec', 'libavutil']
    dep = dependency(d, required: false)
    if dep.found()
      deps_ffmpeg += dep
    else
      have_ffmpeg = false
      break
    endif
    have_ffmpeg = true
  endforeach
  if not have_ffmpeg and ffmpeg_opt == 'enabled'
    error('video=enabled but FFmpeg/libva dev libs are missing')
  endif
endif

have_web = false
deps_web = []
if web_opt != 'disabled'
  w = dependency('webkitgtk-6.0', required: web_opt == 'enabled')
  if w.found()
    have_web = true
    # libsoup: loopback file server for page media (WebKit depends on it)
    deps_web = [w, dependency('libsoup-3.0')]
  endif
endif

# audio-reactive wallpapers: PulseAudio simple API (also PipeWire via
# pipewire-pulse). Optional: without it the spectrum stays silent.
have_pulse = false
deps_audio = []
if audio_opt != 'disabled'
  pa = dependency('libpulse-simple', required: audio_opt == 'enabled')
  if pa.found()
    have_pulse = true
    deps_audio = [pa, dependency('libpulse')]
  endif
endif

# SceneScript: QuickJS (no pkg-config file upstream; Gentoo dev-lang/quickjs
# and Debian/Ubuntu install libquickjs.a under <libdir>/quickjs)
have_quickjs = false
quickjs_old = false
deps_js = []
if get_option('scenescript') != 'disabled'
  qjs = cc.find_library('quickjs', required: false,
    dirs: ['/usr/lib/quickjs', '/usr/lib64/quickjs', '/usr/local/lib/quickjs',
           '/usr/lib/x86_64-linux-gnu/quickjs'])
  if qjs.found() and cc.has_header('quickjs/quickjs.h')
    have_quickjs = true
    deps_js = [qjs, cc.find_library('dl', required: false), dependency('threads')]
    # Wallpaper scripts are untrusted and the interpreter is their only
    # container. QuickJS has no version number in its header; releases
    # from 2024-01-13 on declare JS_EVAL_FLAG_ASYNC. 2021-03-27 (what
    # Debian / Ubuntu ship) lacks fixes for memory errors plain script can
    # reach (CVE-2023-48183, CVE-2023-48184, CVE-2023-31922): an old
    # library is named at configure time and once in the daemon's log.
    quickjs_old = not cc.has_header_symbol('quickjs/quickjs.h', 'JS_EVAL_FLAG_ASYNC')
    if quickjs_old
      warning('QuickJS: quickjs.h has no JS_EVAL_FLAG_ASYNC, so this libquickjs is older than ' +
              'the 2024-01-13 release. Wallpaper scripts run in it: install a current ' +
              'dev-lang/quickjs (or build with -Dscenescript=disabled).')
    endif
  elif get_option('scenescript') == 'enabled'
    error('scenescript=enabled but QuickJS (quickjs/quickjs.h + libquickjs) was not found')
  endif
endif

# media integration (MPRIS over GDBus, always built): libsoup fetches
# http(s) album art when available (WebKit already depends on it)
soup_dep = dependency('libsoup-3.0', required: false)
have_soup = soup_dep.found()
deps_media = have_soup ? [soup_dep] : []

conf = configuration_data()
conf.set('HAVE_SOUP', have_soup)
conf.set('HAVE_QUICKJS', have_quickjs)
conf.set('WALPE_QUICKJS_OLD', have_quickjs and quickjs_old)
conf.set('HAVE_XI2', xi.found())
conf.set('HAVE_XRANDR', xrandr.found())
conf.set('HAVE_PULSE', have_pulse)
conf.set('HAVE_FFMPEG', have_ffmpeg)
conf.set('HAVE_WEB', have_web)
conf.set_quoted('WALPE_VERSION', meson.project_version())
conf.set_quoted('WALPE_CODENAME', walpe_codename)
# optional heavy backends are dlopen'd modules (src/engine/modules.h)
moddir = get_option('prefix') / get_option('libdir') / 'walpe'
conf.set_quoted('WALPE_MODULEDIR', moddir)
# named apart from src/common/config.h (user config): which one a bare
# #include "config.h" resolved to depended on the including file's dir
configure_file(output: 'walpe-build-config.h', configuration: conf)

inc = include_directories('src')

# ---------------------------------------------------------------- core library
core_srcs = files(
  'src/common/util.c',
  'src/common/config.c',
  'src/common/layeradj.c',
  'src/common/quality.c',
  'src/common/ipc.c',
  'src/common/ipcsub.c',
  'src/common/tex.c',
  'src/common/region.c',
  'src/common/premult.c',
  'src/common/import.c',
  'src/common/library.c',
  'src/common/steamsub.c',
  'src/common/audio.c',
  'src/common/media.c',
  'src/common/workshop.c',
  'src/common/workshop_query.c',
  'src/common/uitext.c',
  'src/common/weassets.c',
  'src/common/swrender.c',
  'src/engine/mp4demux.c',
  'src/engine/pkg.c',
  'src/engine/yuvconv.c',
  'src/engine/glutil.c',
  'src/engine/clip.c',
  'src/engine/gpuprof.c',
  'src/engine/cpuphase.c',
  'src/engine/content.c',
  'src/engine/image.c',
  'src/engine/gif.c',
  'src/engine/scene.c',
  'src/engine/scenenode.c',
  'src/engine/scenetext.c',
  'src/engine/puppet.c',
  'src/engine/scenenode_effects.c',
  'src/engine/fxclass.c',
  'src/engine/particles.c',
  'src/engine/weparticles.c',
  'src/engine/scenescript.c',
  'src/engine/propprobe.c',
  'src/engine/propnotes.c',
  'src/engine/video.c',
  'src/engine/modload.c',
)

core = static_library('walpecore', core_srcs,
  include_directories: inc,
  c_args: ['-fvisibility=hidden'],
  dependencies: deps_core + deps_audio + deps_js + deps_media,
  pic: true)

core_dep = declare_dependency(link_with: core, include_directories: inc,
  dependencies: deps_core + deps_audio + deps_js + deps_media + [cc.find_library('dl', required: false)])

# ------------------------------------------------------- dlopen'd modules
# WebKitGTK (GTK 4, GStreamer, JavaScriptCore) and FFmpeg are loaded only
# when a web / video wallpaper (or a scene video texture) needs them. They
# resolve the core functions they call (WALPE_API) from the daemon, which
# exports exactly those (modules.dynlist).
if have_web
  web_mod = shared_module('walpe-web', 'src/engine/web.c', 'src/engine/webinput.c',
    name_prefix: '',
    include_directories: inc,
    c_args: ['-fvisibility=hidden'],
    dependencies: deps_core + deps_web + [gtk4],
    install: true,
    install_dir: moddir)
endif
if have_ffmpeg
  shared_module('walpe-video', 'src/engine/video_custom.c',
    name_prefix: '',
    include_directories: inc,
    c_args: ['-fvisibility=hidden'],
    dependencies: deps_core + deps_ffmpeg,
    install: true,
    install_dir: moddir)
endif

# ---------------------------------------------------------------- executables
dynlist = meson.current_source_dir() / 'src/daemon/modules.dynlist'
daemon = executable('walpe-daemon', 'src/daemon/main.c', 'src/daemon/selguard.c',
  'src/daemon/vistrack.c',
  include_directories: inc,
  dependencies: [core_dep],
  link_args: ['-Wl,--dynamic-list=' + dynlist],
  link_depends: 'src/daemon/modules.dynlist',
  install: true)

# GUI stylesheet compiled in (data/style.css; ~/.config/walpe/style.css overrides)
ui_res = import('gnome').compile_resources('walpe-ui-resources',
  'data/walpe-ui.gresource.xml', source_dir: 'data', c_name: 'walpe_ui')

# the Steam Workshop browser is a tab of the GUI (walpe_workshop_page_new)
ui = executable('walpe', 'src/ui/main.c', 'src/ui/store.c', 'src/ui/props.c', 'src/ui/layers.c',
  'src/ui/perf.c',
  'src/ui/uiipc.c', 'src/ui/thumbs.c', 'src/ui/uistats.c', 'src/ui/workshop_page.c',
  'src/tray/trayconf.c', 'src/ui/delete.c', ui_res,
  include_directories: inc,
  c_args: ['-DWALPE_HAVE_WORKSHOP_PAGE'],
  dependencies: [core_dep, gtk4],
  install: true)

ctl = executable('walpe-ctl', 'src/ctl/main.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: true)

# shader repair (fx_srcrepair.h): source inspection, the error-driven
# fallback under Mesa / NVIDIA / glslang / unknown log wordings, varying
# interface, transform effect (GL part: EGL device or surfaceless, skipped
# when unavailable). `meson test -C build`
test('fx-repair', executable('fx-repair-test', 'tests/fx_repair_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# shared file-texture cache (engine/glutil.c): 1/2-channel storage of RGBA8
# data, one texture for identical pixels, stored mip levels, DXT kept
# compressed, the texture budget. Needs GL (EGL device or Mesa surfaceless);
# skips itself without.
test('texcache', executable('texcache-test', 'tests/texcache_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# how a layer picture gets to the GPU: reduced on the CPU before the upload
# (the texels of the GL blit, never a full-size texture), PNG / JPEG decoded
# on worker threads and picked up by the first frame's load, the reduced
# copy without a share group, a scene with PNG layers at half size. Needs
# GL with a pbuffer (Mesa surfaceless); skips itself without.
texload_test = executable('texload-test', 'tests/texload_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false)
test('texload', texload_test, timeout: 120)
# the same with the opt-in switch: odd-sized pictures reduced before the upload too
test('texload-odd', texload_test, env: ['WALPE_TEX_CPU_REDUCE=2'], timeout: 120)

# what the scene renderer draws per frame: unchanged frames are skipped,
# static layers come from one copy (bake), render_scale; generated scenes
# on EGL surfaceless (skipped when unavailable)
test('scene-frames', executable('scene-frames-test', 'tests/scene_frames_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# layer textures as the input of effects (stored reduced only where that
# gives the same picture; exact with tex_downscale off) and layers attached
# to a puppet's attachment point; generated scenes on EGL surfaceless
# (skipped when unavailable)
test('scene-fit', executable('scene-fit-test', 'tests/scene_fit_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# integration gate: with frame skipping and the static bake an idle scene
# (0 fps) must still repaint when something outside its layers changes the
# picture: "Adjust layers" move / scale / hide / show / highlight / reset,
# render_scale. EGL surfaceless (skipped when unavailable).
test('idle-repaint', executable('idle-repaint-test', 'tests/idle_repaint_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# a generated scene whose scripts drive layers and effects with hostile
# values (version 0.12.2 security pass): zero / huge / non-finite vectors on
# every kind of layer, megabytes of text, re-parenting in circles, a parent
# chain of 100 000 layers built at run time. EGL surfaceless (skipped
# without, and without QuickJS).
test('scene-hostile', executable('scene-hostile-test', 'tests/scene_hostile_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 900)

# effect pass programs (fx_specialize.h, fx_verify.h, fx_rewrite.h,
# fx_fuse.h): program sharing byte-identical to one program per pass, baked
# constants, the comparison a specialised program has to pass before it is
# used, script writes, the vertex-shader moves, identity passes, fused
# runs; with the Wallpaper Engine assets also WE's own effects, generic vs
# specialised vs fx_quality=fast (GL part: every EGL device or surfaceless,
# skipped when unavailable)
test('fx-spec', executable('fx-spec-test', 'tests/fx_spec_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# what the effect executor keeps over frames (static cache, held sets of the
# effect update rate, prefixes) against what fx_specialize / fx_quality=fast
# change about who draws a result: fused runs, skipped identity passes, the
# target pool; the comparison of specialised programs at fixed probe times
# (GL part: every EGL device or surfaceless, skipped when unavailable)
test('fx-hold', executable('fx-hold-test', 'tests/fx_hold_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# a layer's opacity goes in before its effects (Wallpaper Engine's order:
# the layer's own material draw), WALPE_FX_ALPHA=after keeps the earlier build's order;
# layers without effects and opaque layers are byte for byte the same in
# both; a scripted opacity is followed in the frame it changes (static,
# animated and held effect sets). Generated scenes on EGL surfaceless
# (skipped when unavailable).
test('fx-alpha', executable('fx-alpha-test', 'tests/fx_alpha_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 180)

# effect passes restricted to what is read later (fx_region.h): which
# lookups count as local, the rectangle arithmetic, and - GL part, every
# EGL device or surfaceless, skipped when unavailable - restricted runs
# against whole runs byte for byte, also over poisoned targets; with the
# Wallpaper Engine assets the reach bounds of WE's shake / waterwaves /
# iris shaders (the check to run on an NVIDIA machine)
test('fx-region', executable('fx-region-test', 'tests/fx_region_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 180)

# the effect shader pipeline against hostile shader text and effect JSON
# (version 0.12.2 security pass): time scan and #include expansion stay bounded,
# seeded mutations of small effects through every text stage and, with EGL
# surfaceless (skipped without), through the executor in every program mode
test('fx-hostile', executable('fx-hostile-test', 'tests/fx_hostile_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 900)

# statements of stock effect shaders, recognised by key (fx_keys.h): the key
# functions against strstr (), the recognisers of fx_rewrite.h and of the
# vertex traits through statements written for the test (no GL). With
# WALPE_WE_ASSETS set, also the key table against that install's own
# shaders (skipped otherwise, and the output says so)
test('fx-rewrite-keys', executable('fx-keys-test', 'tests/fx_keys_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# workshop browsing: parsers on saved Steam replies, settings file mode,
# steamcmd job with a fake steamcmd. WALPE_WS_LIVE=1 adds live Steam calls.
test('workshop', executable('workshop-test', 'tests/workshop_test.c',
  include_directories: inc,
  c_args: ['-DSRCDIR="' + meson.current_source_dir() + '"'],
  dependencies: [core_dep],
  install: false), timeout: 120)

# control socket (common/ipc.c): large replies, request limit, socket mode,
# single-instance lock, a client that does not read. No daemon, no display.
test('ipc', executable('ipc-test', 'tests/ipc_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 60)

# what the daemon and its clients trust on
# the local machine - the socket's directory, config.json's mode and a
# hostile config.json, replies that are not UTF-8, NUL bytes in a request,
# connections from another account, which module files may be loaded.
# No daemon, no display; the cases that need a second account need root.
test('sec-local', executable('sec-local-test', 'tests/sec_local_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 120)

# the same review against a running daemon on an X server of its own
# (Xvfb -displayfd; exit 77 = skipped where Xvfb is not installed): a
# runtime directory others can write to, walpe-ctl's request building,
# numbers that are not numbers, descriptors across the layout restart.
test('sec-daemon', find_program('sh'),
  args: [meson.current_source_dir() / 'tests/sec_daemon_test.sh', daemon, ctl],
  timeout: 240)

# audit input checks: project.json type casing and "file" confinement,
# built-in options per type, non-finite fit values, picture size limit,
# the MP4 demuxer refusing a FIFO. Private temp HOME, no display.
test('guard', executable('guard-test', 'tests/guard_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 60)

# opt-in resource rules in config.json, IPC state pushes
# ("subscribe" / broadcast) and the subscribing client. No daemon, no display.
test('respolicy', executable('respolicy-test', 'tests/respolicy_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 60)

# spectrum analysis: both channels from one FFT, band values, silence
test('audio', executable('audio-test', 'tests/audio_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# the size limits on wallpaper files in the loaders that work at reduced size: the
# shared GIF open, the still image decoded at shown size (limit on the
# declared size), MPRIS cover art and its two-entry cache. Refusals need no
# display; with a surfaceless GL context the good paths run too.
test('limits', executable('limits-test', 'tests/limits_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 120)

# video module (engine/video_custom.c, compiled into the test): the box
# filter kernels against a plain loop, the frame limit rules, and - with a
# clip that ffmpeg makes here (skipped without ffmpeg + libx264) - every
# queued frame against a plain libavcodec decode (full size, half size,
# limit off and on), the software fallback of hardware decoding with its
# status line, and with a GL context the uploaded plane textures. Run a
# second time in the NV12 layout that hardware decoders deliver.
if have_ffmpeg
  video_test = executable('video-test', 'tests/video_test.c',
    include_directories: inc,
    dependencies: [core_dep] + deps_ffmpeg,
    install: false)
  test('video', video_test, timeout: 300)
  test('video-nv12', video_test, env: ['WALPE_VIDEO_TEST_NV12=1'], timeout: 300)
endif

# version 0.12: config.json fps migration, shared-rendering leader, pacing, YUV matrices
test('pixfps', executable('pixfps-test', 'tests/pixfps_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# decoder threads by need (src/engine/video_threads.h): the count
# never exceeds version 0.12's, a clip that falls behind (also by a few percent)
# gets what its rate asks for in one step, one stall is not a reason. No display.
test('video-threads', executable('video-threads-test', 'tests/video_threads_test.c',
  include_directories: inc,
  dependencies: [glib, m_lib],
  install: false))

# walpe-ctl's command line (bad input is named and exits with 1,
# no daemon = 2, refused = 3), what it sends for `set` / `setprop` / `click`
# (a stand-in daemon on a private socket records it), and the daemon's check
# of a "set" request (common/setkeys.h; also read against ipc_set () in the
# daemon's source). No daemon, no display.
test('ctl', executable('ctl-test', 'tests/ctl_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false),
  args: [ctl, files('src/daemon/main.c')], timeout: 120)

# config.json: defaults as documented, every key through save +
# load, members of other versions kept by a save, a version 0.12 file, files that
# are not a JSON object kept aside. Private XDG_CONFIG_HOME, no display.
test('config', executable('config-test', 'tests/config_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# the video importer: the 1080p cap and what is copied as it
# is (no ffmpeg needed); with ffmpeg / ffprobe in PATH real runs on clips made
# here: remux, encode, a file that is no video, cancel, nothing left behind.
test('import', executable('import-test', 'tests/import_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 300)

# presentation rules (present.h): present mode per monitor, swap interval
# with / without a compositing manager, gpu.conf, driver hints. No display.
test('present', executable('present-test', 'tests/present_test.c',
  include_directories: inc,
  dependencies: [glib],
  install: false))

# quality-for-load knobs (common/quality.c, engine/fxclass.c): presets,
# JSON, config.json round trip with per-wallpaper settings, the daemon's
# set / setquality / quality requests over a real socket, frame rate by
# state (the focus and battery rules as inputs), saving estimates, effect
# classification. No display, no daemon; private temp config dir.
test('quality', executable('quality-test', 'tests/quality_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 60)

# "Skip hidden areas": the rectangle-set arithmetic behind the visible
# region of a monitor (common/region.c) against a bitmap, on fixed window
# layouts and random ones. No display, no GL.
test('region', executable('region-test', 'tests/region_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# "Skip hidden areas" in the renderer: which lookups of an effect shader
# are local, what each pass has to fill, and (GL part: EGL device or
# surfaceless, skipped when unavailable) that a scene rendered clipped to
# the visible boxes of several window layouts has exactly the pixels of
# the unclipped rendering there. `clip-test <wallpaper dir> [frames] [WxH]`
# runs the comparison on a real scene and prints the fragments shaded.
test('clip', executable('clip-test', 'tests/clip_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 300)

# SceneScript limits and semantics: the time budget of a runaway script, a
# built-in that never returns (regular expression), zero-delay timers,
# promises, recovery after errors. No GL; skips itself without QuickJS.
test('scenescript', executable('scenescript-test', 'tests/scenescript_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 120)

# SceneScript against a hostile script (version 0.12.2 security pass): the
# module rewriter on cut-off and deeply nested sources, promise reactions
# that never return, NaN / Infinity / huge vectors at the host boundary,
# thrown values with hostile toString, a seeded driver that calls the whole
# API with wrong types. No GL; skips itself without QuickJS.
test('script-hostile', executable('script-hostile-test', 'tests/script_hostile_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 900)

# particle instance overrides "count" and "rate": values a script drives and
# values of the scene file (WALPE_WE_STATIC_OVERRIDES_AS_WE). No GL, no files.
test('weparticles', executable('weparticles-test', 'tests/weparticles_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# the same test with WALPE_PT_CAP=count in the environment
# (the opt-in switch: the cap of live particles follows the instance's count)
test('weparticles-cap-count', executable('weparticles-capcount-test', 'tests/weparticles_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), env: ['WALPE_PT_CAP=count'])

# the particle simulation without GL - child systems that
# follow events of their parent (one instance per parent particle: bursts of
# eventfollow children, followers carried by their particle, the instance
# count of the child reference), control points placed in the scene, a burst
# whose emitter duration is shorter than the first step. Its particle files
# are written to a private temp dir.
particle_sim_test = executable('particle-sim-test', 'tests/particle_sim_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false)
test('particle-sim', particle_sim_test)
# the same checks with the other reading of alphafade's fade-out time
test('particle-sim-fade-span', particle_sim_test, env: ['WALPE_PT_FADE=span'])

# particle objects of a scene made by the test, through
# the content API: the instance colour ("colorn" and the older "color" in
# 0..255), options that feed "count" (default, undeclared option, text),
# options that show / hide a particle object, and the same after the user
# changed them. GL part as layers: EGL device or surfaceless, else skipped.
test('particle-scene', executable('particle-scene-test', 'tests/particle_scene_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 180)

# version 0.12.1: sprite-sheet frames of a particle (sequence / once /
# randomframe, sequencemultiplier, frame blending and its flag); no GL
test('particle-sheet', executable('particle-sheet-test', 'tests/particle_sheet_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# version 0.12.1: "Render on CPU" - Mesa's EGL vendor file among others (fake
# vendor dirs), llvmpipe thread count, the software environment, the probe's
# report, the config keys, the control socket kept across a restart in place
test('swrender', executable('swrender-test', 'tests/swrender_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# "Adjust layers" (common/layeradj.h): layer keys and their matching,
# validation, config.json round trip, forgetting a wallpaper; and, with an
# EGL device or Mesa's surfaceless platform (skipped otherwise), pixel
# checks that a moved / scaled / hidden layer of a scene made by the test
# lands where it should (plain, parented, script-driven, text, particle)
test('layers', executable('layers-test', 'tests/layers_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 180)

# dev tool (not installed): one frame of a wallpaper, rendered off screen
# (EGL pbuffer; no X server, no daemon), as a PNG plus a hash of its pixels:
#   sceneshot <wallpaper dir> <out.png> [--size WxH] [--frames N]
executable('sceneshot', 'tests/sceneshot.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false)

# developer tool (not installed): what a wallpaper's frames cost, as counters
# (fragments shaded, draws, clears, copies, binds, GL calls per frame), and
# a hash of every frame - for before / after and for A/B of a switch:
#   scenecount <wallpaper dir> [--size WxH] [--warm N] [--frames M] [--tsv]
executable('scenecount', 'tests/scenecount.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false)

# a text layer whose string changes is rendered
# and uploaded only where the old or the new string has ink; the texture
# that results is the full render's, byte for byte (no GL)
test('text-part', executable('text-part-test', 'tests/text_part_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 120)

# dev tool (not installed): render-thread CPU time of a scene per frame by
# phase (scripts, transforms, text, particle simulation / vertex data / GL,
# layer draws), live particles and - for comparing two builds - hashes of
# the particle state, the particle vertex data and the picture:
#   scenecpu <wallpaper dir> [--size WxH] [--warm N] [--frames M] [--hash K]
executable('scenecpu', 'tests/scenecpu.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false)

# GPU round: particle submission - instanced quads against the vertex by
# vertex reference, off-screen culling, idle systems, the reduced-resolution
# layer for large soft sprites, the density setting. GL part as fx-repair:
# EGL device or surfaceless, skipped when unavailable.
test('particle-draw', executable('particle-draw-test', 'tests/particle_draw_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 120)

# wallpaper deletion: path validation against the library roots (symlinks,
# "..", roots, loose media), trash + permanent removal, config forgetting.
# Works in a private temp dir that is also its HOME; nothing else is touched.
test('library', executable('library-test', 'tests/library_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# hostile wallpaper files (version 0.12 audit): preset dependency cycles, pkg
# entries that share bytes / leave the target dir, oversized pictures and
# absurd sprite tables in .tex, asset names that leave the wallpaper; the
# library scan's wallpaper types (any letter case, presets). Private temp HOME.
test('formats', executable('formats-test', 'tests/formats_test.c', 'src/ui/store.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 120)

# hostile wallpaper files, second set: the json nesting limit
# against comments / single quotes, json file size, symbolic links that leave
# the wallpaper, texture files that are not pictures a wallpaper may wrap,
# package directory chains / entry counts / cache bookkeeping names, the
# video reader's sample count / timescale / empty tkhd box, and
# (surfaceless GL, else skipped) the old scene format's pictures and text.
# Private temp HOME.
test('parsers', executable('parsers-test', 'tests/parsers_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 240)

# puppet-warp models (.mdl): animation headers with an absurd length / fps
# must not index outside the frame table. puppet.c is compiled into the
# test (GL calls replaced), no display needed.
test('puppet', executable('puppet-test', 'tests/puppet_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# Steam subscriptions: "is Steam running" (a fake steam.pid/steam.pipe), the
# Workshop-page command with stub `steam` / `xdg-open`, the delete plan's
# Steam-client flag, and Subscribe/Unsubscribe against a loopback mock of
# the Steam Web API. Private temp HOME; no Steam, no network.
test('steamsub', executable('steamsub-test', 'tests/steamsub_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 60)

# what a reply can do to the Workshop client.
# A loopback stand-in for the Steam Web API sends a body without end, gzip
# that unpacks to 160 MiB, an over-long announced length, redirects, 100000
# nested brackets, a hanging and a trickling reply; parser limits on item
# counts and string lengths; the preview decoder allow-list. Private temp
# HOME; no Steam, no network.
test('wsnet', executable('wsnet-test', 'tests/wsnet_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 180)

# text of a wallpaper or a Workshop listing made fit for a label
# (src/common/uitext.c), the library scan's use of it and the import of a
# folder that holds links and a FIFO (src/ui/store.c)
test('uitext', executable('uitext-test', 'tests/uitext_test.c', 'src/ui/store.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# memory / disk reductions keep their results: partial LZ4 decode of padded
# .tex levels, stored mip levels (walpe_tex_load_level / walpe_tex_probe),
# the size-limited cache of extracted scene.pkg files. Private temp HOME.
test('resmem', executable('resmem-test', 'tests/resmem_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 120)

# a wallpaper that fails to load: which monitors return to their previous
# wallpaper and what the saved choice becomes (common/restore.h). No
# display: the rules only, not the daemon.
test('restore', executable('restore-test', 'tests/restore_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false))

# the daemon end to end (scripts/lifecheck.py with its harness
# scripts/checkenv.py: a private Xvfb and a private walpe-daemon on
# generated scenes): the pointer after a load, settings
# that reload scenes, the restart in place, quality numbers of a still scene,
# a video the decoder refuses (the clip is made with the ffmpeg program:
# that case is skipped where there is none).
# Skipped (77) without Xvfb. `meson test -C build daemon-life`
lifecheck_py = find_program('python3', required: false)
if lifecheck_py.found()
  test('daemon-life', lifecheck_py,
    args: ['-I', meson.current_source_dir() / 'scripts/lifecheck.py',
           '--build', meson.current_build_dir()],
    depends: [daemon, ctl],
    is_parallel: false,
    timeout: 900)
endif

# web wallpapers: which part of a captured page frame changed (partial
# uploads): old frame + the reported rectangle of the new one == new frame
test('webdamage', executable('webdamage-test', 'tests/webdamage_test.c',
  include_directories: inc,
  dependencies: [glib],
  install: false))

# GPU profiler (gpuprof.h): report formatting (table order, top 15, totals,
# JSON, the no-timer fallback), driver-message de-duplication, pixel and
# upload-size helpers; then, with an EGL device or Mesa's surfaceless
# platform, the real thing on GL 3.3 and GLES 3: timer queries, the
# KHR_debug channel, the counting wrappers, the memory snapshot (skipped
# when no such context exists).
test('gpuprof', executable('gpuprof-test', 'tests/gpuprof_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 120)

# scripts/walpe-gpu-report.sh against a stand-in nvidia-smi: every field
# supported, fields a laptop GPU reports as [N/A], field names the tool
# does not know, no nvidia-smi at all, no walpe-daemon.
test('gpu-report', find_program('sh'),
  args: [meson.current_source_dir() / 'tests/gpu_report_test.sh',
         meson.current_source_dir() / 'scripts/walpe-gpu-report.sh',
         meson.current_source_dir() / 'tests/fake-nvidia-smi'],
  timeout: 120)

# option labels: WE property text (HTML/BBCode) -> plain readable labels;
# `proplabel-test <project.json>` prints a wallpaper's labels
test('proplabel', executable('proplabel-test', 'tests/proplabel_test.c',
  'src/ui/props.c', 'src/ui/uiipc.c',
  include_directories: inc,
  dependencies: [core_dep, gtk4],
  install: false))

# GUI previews (src/ui/thumbs.c): GIF block parser on cut / damaged files,
# animation frames at tile size and their limits. No display.
test('thumbs', executable('thumbs-test', 'tests/thumbs_test.c',
  include_directories: inc,
  dependencies: [core_dep, gtk4],
  install: false))

# hostile input for the web, video and import parts ------------
# hostile video files: the CPU YUV converter with odd picture sizes (it
# wrote a row past its buffer), the decoder's picture size limit (clips
# written by the test, no encoder needed), the decode thread count. No GL.
if have_ffmpeg
  test('videosec', executable('videosec-test', 'tests/videosec_test.c',
    include_directories: inc,
    dependencies: [core_dep] + deps_ffmpeg,
    install: false), timeout: 180)
endif
# the importer / converter with stand-in ffmpeg + ffprobe scripts: command
# line, output that is not UTF-8 (the job used to hang), size limit of a
# conversion on open. No ffmpeg, no display.
test('importsec', executable('importsec-test', 'tests/importsec_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false), timeout: 180)
# a hostile web wallpaper against the running daemon: the network switch
# and other loopback ports, the X selections of other clients, page-made
# log lines. Starts its own Xvfb; skips itself without Xvfb / python3 /
# WebKit.
if have_web
  websec_selown = executable('websec-selown', 'tests/websec_selown.c',
    dependencies: [x11], install: false)
  test('websec', find_program('sh'),
    args: [files('tests/websec_test.sh'), meson.current_build_dir()],
    depends: [daemon, ctl, web_mod, websec_selown],
    is_parallel: false, timeout: 300)
endif

# dev tool (not installed): renders the "Wallpaper options" panel of one
# project.json into a PNG without the rest of the GUI (needs a display):
#   propshot <project.json> <out.png> data/style.css [width] [height]
executable('propshot', 'tests/propshot.c', 'src/ui/props.c', 'src/ui/uiipc.c',
  include_directories: inc,
  dependencies: [core_dep, gtk4],
  install: false)

# dev tool (not installed, needs a display): time to the first frames of a
# label set up like the GUI's, with N characters of text - the measurement
# behind src/common/uitext.h:
#   labelbench wsdesc 8000 W      Workshop description, unbroken   (9 s CPU)
#   labelbench wsdesc 8000 zw     the same with breaks every 64    (0.2 s)
#   labelbench tile 20000 W       library tile title, unbroken     (2.6 s)
executable('labelbench', 'tests/labelbench.c',
  dependencies: [gtk4],
  install: false)

# GPU numbers for a bug report: nvidia-smi + /proc samples, then a profile
install_data('scripts/walpe-gpu-report.sh', rename: 'walpe-gpu-report',
  install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
install_data('data/walpe-autostart.desktop',
  install_dir: get_option('sysconfdir') / 'xdg/autostart')
install_data('data/style.css', install_dir: get_option('datadir') / 'walpe')
# licence texts next to the installed programs: WalPE's own (BSD 3-Clause) and
# QuickJS's MIT notice (libquickjs is a static library on most systems, so
# the binaries contain it)
install_data('LICENSE', 'licenses/QuickJS-MIT.txt',
  install_dir: get_option('datadir') / 'licenses' / 'walpe')

# ------------------------------------------------------------ walpe-tray
# System tray icon (src/tray/): a separate process, so a tray problem can
# never affect the daemon or the GUI. XEmbed needs libXrender (alpha icon
# over the tray background); SNI needs only GDBus.
tray_opt = get_option('tray')
have_tray = false
if tray_opt != 'disabled'
  xrender = dependency('xrender', required: tray_opt == 'enabled')
  if xrender.found()
    have_tray = true
    tray_deps = [core_dep, gtk4, x11, xrender]
    if xi.found()
      tray_deps += xi   # menu grabs through XInput2 (what GDK listens to)
    endif
    executable('walpe-tray',
      'src/tray/main.c', 'src/tray/icon.c', 'src/tray/trayipc.c', 'src/tray/trayconf.c',
      'src/tray/popup.c', 'src/tray/xembed.c', 'src/tray/sni.c', 'src/ui/store.c',
      include_directories: inc,
      c_args: ['-DG_LOG_DOMAIN="walpe-tray"'],
      dependencies: tray_deps,
      install: true)
    install_data('data/walpe-tray.desktop',
      install_dir: get_option('datadir') / 'applications')
    install_data('data/walpe-tray-autostart.desktop', rename: 'walpe-tray.desktop',
      install_dir: get_option('sysconfdir') / 'xdg/autostart')
    install_data('data/icons/walpe.svg',
      install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
    # PNG sizes, if a launcher needs them: walpe-tray --export-icon=walpe.png --icon-size=N
    # dev/test helper (not installed): a StatusNotifierWatcher + host stub
    # that registers walpe-tray's item, dumps icon/tooltip/menu and clicks
    # menu items over dbusmenu
    executable('sni-host-stub', 'tests/tray/sni_host_stub.c',
      dependencies: [gio, glib, dependency('cairo')],
      install: false)
    executable('xembed-host-stub', 'tests/tray/xembed_host_stub.c',
      dependencies: [x11, glib],
      install: false)
  endif
endif
