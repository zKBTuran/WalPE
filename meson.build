project('walpe', 'c',
  version: '0.14.0',
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
# Every release has a name of its own. This is the one place that
# says it: the sources get it as WALPE_CODENAME, like WALPE_VERSION.
walpe_codename = 'Flood'

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
  '-fno-semantic-interposition',
  '-fomit-frame-pointer',
  '-fvisibility=hidden',
  '-ffp-contract=off',
  '-Wno-unused-parameter',
]), language: 'c')

add_project_arguments('-D_GNU_SOURCE', language: ['c'])

# ThreadSanitizer build (-Db_sanitize=thread; README.md "ThreadSanitizer").
# GLib's GMutex / GCond are futex based inside libglib and invisible to the
# sanitizer, so every program of such a build gets tests/tsan/gsync_shim.c,
# which puts them on pthread primitives (and exports them, so the modules
# and the other libraries use them too). Nothing of it is in any other
# build.
tsan = get_option('b_sanitize').split(',').contains('thread')
if tsan
  add_project_link_arguments(cc.get_supported_link_arguments(['-Wl,--export-dynamic']),
    language: 'c')
endif

# Hardening (-Dhardening=false turns it off). The daemon parses files from
# the Steam Workshop, so the usual protections are asked for here instead of
# being left to whatever the distribution's compiler enables by default:
# stack protector, stack clash protection, control-flow protection (x86),
# fortified libc calls, format-string errors, and at link time read-only
# relocations with immediate binding and a non-executable stack. Each flag
# is probed and dropped when the toolchain does not know it. They are
# ordinary compile/link flags: -O3 and LTO keep them.
if get_option('hardening')
  hardening_flags = [
    '-fstack-protector-strong',
    '-fstack-clash-protection',
    '-fcf-protection',
    '-Wformat',
    '-Werror=format-security',
  ]
  # aarch64 has no -fcf-protection (the probe drops it there); its
  # counterpart signs return addresses and marks branch targets (PAC + BTI,
  # no-ops on cores without them)
  if host_machine.cpu_family() == 'aarch64'
    hardening_flags += ['-mbranch-protection=standard']
  endif
  add_project_arguments(cc.get_supported_arguments(hardening_flags), language: 'c')
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

# Fuzz targets (-Dfuzz=enabled, clang; tests/fuzz). Every source gets the
# coverage instrumentation libFuzzer steers by; combine with
# -Db_sanitize=address,undefined -Db_lundef=false. Off by default: nothing
# of it is in any other build.
if get_option('fuzz').enabled()
  if not cc.has_argument('-fsanitize=fuzzer-no-link')
    error('fuzz=enabled needs a compiler with libFuzzer (clang): CC=clang meson setup ...')
  endif
  add_project_arguments('-fsanitize=fuzzer-no-link', language: 'c')
endif

# release builds: GObject type-cast macros (GTK_WINDOW (x), ...) are plain
# casts, as GLib recommends for production builds
if not get_option('debug')
  add_project_arguments('-DG_DISABLE_CAST_CHECKS', language: 'c')
endif

# Byte order. Wallpaper Engine's files are little-endian and so is every
# buffer handed to GL as it lies in memory (textures, 16-bit video planes,
# the premultiply loop; src/common/portable.h): a big-endian build would
# compile and then show wrong colours.
if host_machine.endian() != 'little'
  error('WalPE supports little-endian targets only (x86-64, aarch64); this build targets a ' +
        host_machine.endian() + '-endian ' + host_machine.cpu_family())
endif

# 32-bit targets: file offsets of 64 bits (a video is read with pread at
# the offsets its sample table gives). Nothing is added where off_t is
# 64 bits already (x86-64, aarch64).
if cc.sizeof('off_t', prefix: '#include <sys/types.h>') == 4
  add_project_arguments('-D_FILE_OFFSET_BITS=64', language: 'c')
endif

# CPU tuning (-Dnative_tuning=...). It only lets GCC/clang auto-vectorise
# and schedule for the target CPU. The hand-written kernels do not depend
# on it: on x86-64 the SSE2 ones (glutil.c, video_custom.c) are part of the
# baseline and the AVX2 ones (video_custom.c) are compiled per function and
# chosen at run time; every other architecture runs the plain C loops,
# which give the same bytes.
#   generic (default): no -march / -mtune / cache parameters: the binary
#             runs on any CPU of the architecture it is built for (portable
#             binaries, CI, other machines, cross builds). `off` is the same
#             (its earlier name, kept for existing scripts).
#   alderlake: x86-64 only (a configure error elsewhere): the maintainer's
#             Gentoo make.conf COMMON_FLAGS minus -O3 (= optimization=3
#             above); `./build.sh --tuning=alderlake`. The binary only runs
#             on Alder Lake-class Intel CPUs (AVX2/FMA/BMI/SHSTK...). This
#             was the default of earlier builds.
#   native:   the CPU of the machine that builds: -march=native on x86,
#             -mcpu=native on aarch64 (architecture level and tuning of the
#             core in one; clang knows -march=native there only from
#             release 16). Error if the compiler does not accept it, and in
#             a cross build (it would describe the wrong machine: give the
#             target's CPU with -Dc_args=-mcpu=... instead).
#   auto:     as native when the compiler accepts it, else nothing; nothing
#             in a cross build.
# Each flag is probed with get_supported_arguments(), so an older GCC or clang
# drops the ones it does not know (e.g. clang has no --param cache sizes)
# instead of failing. The same flags go on the link line so LTO code
# generation (which happens at link time) uses them too.
tuning = get_option('native_tuning')
host_cpu = host_machine.cpu_family()
tune_flags = []
if tuning == 'alderlake'
  if host_cpu != 'x86_64' and host_cpu != 'x86'
    error('native_tuning=alderlake is for x86-64 only and this build targets ' + host_cpu +
          ': use generic (the default), or native on the machine that will run the binary')
  endif
  tune_flags = [
    '-march=alderlake', '-mabm', '-mno-kl', '-mno-pconfig', '-mno-sgx',
    '-mno-widekl', '-mshstk',
    '--param=l1-cache-line-size=64', '--param=l1-cache-size=32',
    '--param=l2-cache-size=18432',
  ]
elif tuning == 'native' or tuning == 'auto'
  if meson.is_cross_build()
    if tuning == 'native'
      error('native_tuning=native in a cross build would tune for the build machine, not for ' +
            host_cpu + ': use generic, or name the target CPU with -Dc_args=-mcpu=<cpu>')
    endif
  elif host_cpu == 'aarch64' and cc.has_argument('-mcpu=native')
    tune_flags = ['-mcpu=native']
  else
    tune_flags = ['-march=native']
  endif
endif
tune_args = cc.get_supported_arguments(tune_flags)
if tuning == 'native' and tune_args.length() == 0
  error('native_tuning=native but the compiler rejects ' + ' '.join(tune_flags))
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

# ---- the window systems the daemon can draw on (src/platform): X11 and /
# or Wayland, -Dx11 / -Dwayland = auto | enabled | disabled. Both may be
# built in (the daemon picks one when it starts: common/backend.h); at
# least one has to be.
fs = import('fs')
x11_opt = get_option('x11')
wayland_opt = get_option('wayland')

# libX11 is looked for whatever -Dx11 says: the tray is an X11 client of its
# own (XEmbed) where the library is there. -Dx11=disabled leaves X out of
# walpe-daemon and of the web module (its X11 page host, see below).
x11       = dependency('x11', required: x11_opt.enabled())
xext      = dependency('xext', required: x11_opt.enabled())
xinerama  = dependency('xinerama', required: x11_opt.enabled())
have_x11  = not x11_opt.disabled() and x11.found() and xext.found() and xinerama.found()
lz4       = dependency('liblz4')  # WE .tex compressed mips
m_lib     = cc.find_library('m')

deps_core = [glib, gobject, gio, jsonglib, pixbuf, pangocairo, pangoft2, fontconfig, epoxy, egl]
if have_x11
  # (as before the two backends: everything that links the core library
  # is given the X libraries too)
  deps_core += [x11, xext, xinerama]
endif
deps_core += [lz4, m_lib]
# (ThreadSanitizer build only, see above: linked whole into every program)
tsan_deps = []
if tsan
  tsan_shim = static_library('walpe-tsan-shim', 'tests/tsan/gsync_shim.c',
    dependencies: [glib, dependency('threads'), cc.find_library('dl', required: false)],
    pic: true)
  tsan_deps = [declare_dependency(link_whole: tsan_shim)]
endif
# optional: XInput2 raw button events = desktop clicks for SceneScript cursor events
xi = dependency('xi', required: false)
if xi.found() and have_x11
  deps_core += xi
endif
# optional: RandR 1.5 monitor names (per-monitor wallpapers are saved by
# output name, e.g. HDMI-0; without it monitors are named mon0, mon1, ...)
xrandr = dependency('xrandr', required: false)
if xrandr.found() and have_x11
  deps_core += xrandr
endif

# Wayland: libwayland-client and -egl, and wayland-scanner to turn the
# protocol descriptions in protocols/ into C (the tree carries the ones
# it uses, so the build does not depend on which wayland-protocols release
# a distribution ships). The backend's source, src/platform/wayland.c, has
# to be in the tree.
have_wayland = false
deps_wayland = []
wayland_backend_src = []
wl_generated = []
if not wayland_opt.disabled()
  wayland_client = dependency('wayland-client', required: wayland_opt)
  wayland_egl = dependency('wayland-egl', required: wayland_opt)
  wl_scanner = find_program('wayland-scanner', native: true, required: wayland_opt)
  if wayland_client.found() and wayland_egl.found() and wl_scanner.found()
    if fs.exists('src/platform/wayland.c')
      have_wayland = true
    elif wayland_opt.enabled()
      error('wayland=enabled, but the Wayland backend (src/platform/wayland.c) is not in this source tree')
    else
      message('Wayland backend: src/platform/wayland.c is not in this source tree; not built')
    endif
  endif
endif
if not have_x11 and not have_wayland
  error('at least one display backend is needed (-Dx11 / -Dwayland). X11 needs libX11, libXext and ' +
        'libXinerama; Wayland needs wayland-client, wayland-egl and wayland-scanner')
endif
have_wl_cursor = false
if have_wayland
  deps_wayland = [wayland_client, wayland_egl]
  # optional: a pointer image over the wallpaper while it takes pointer input
  wayland_cursor = dependency('wayland-cursor', required: false)
  if wayland_cursor.found()
    deps_wayland += wayland_cursor
    have_wl_cursor = true
  endif
  # one name per protocol: protocols/<name>.xml becomes
  # <name>-client-protocol.h and <name>-protocol.c, compiled into the
  # daemon. The files are unchanged copies with their own licence notices:
  # six from wayland-protocols, wlr-layer-shell from wlr-protocols
  # (protocols/README says which release each one is from).
  wl_protocol_names = [
    'xdg-shell',
    'xdg-output-unstable-v1',
    'viewporter',
    'fractional-scale-v1',
    'presentation-time',
    'linux-dmabuf-v1',
    'wlr-layer-shell-unstable-v1',
  ]
  foreach wl_base : wl_protocol_names
    wl_xml = files('protocols' / wl_base + '.xml')
    wl_generated += custom_target(wl_base + '-client-protocol',
      input: wl_xml, output: wl_base + '-client-protocol.h',
      command: [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
    wl_generated += custom_target(wl_base + '-protocol',
      input: wl_xml, output: wl_base + '-protocol.c',
      command: [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  endforeach
  # (further source files of the Wayland backend are added to this list)
  wayland_backend_src = files('src/platform/wayland.c')
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
# Where the page of a web wallpaper lives while nobody sees its window
# (src/engine/webhostsel.h). Each page host is a compile-time part of the
# web module, built when what it needs is there:
#   x11      an off-screen X window: libX11 and GTK's X11 backend
#            (gtk4-x11). Part of -Dx11: -Dx11=disabled leaves it out, and
#            then neither walpe-daemon nor walpe-web.so names an X library.
#   private  the module's own Wayland display (src/engine/webhost_wl.c):
#            libwayland-server and -client 1.20 or newer, wayland-scanner
#            for the server side of xdg-shell, GTK's Wayland backend
#            (gtk4-wayland). Only together with the Wayland backend: a
#            daemon that can draw on X11 only always has an X display for
#            the page's window, and its web module then links no Wayland
#            library.
# The module needs one of the two; which one a page gets is decided when
# the first page is made (WALPE_WEB_HOST).
# (1.20: the code names wl_output.name / description (wl_output 4) in its
# listener and wl_surface.offset (wl_surface 5) in the page host's request
# table; with an older libwayland those files would not compile, so the
# Wayland parts are left out instead)
wl_client_dep = dependency('wayland-client', version: '>= 1.20', required: false)
wl_server_dep = dependency('wayland-server', version: '>= 1.20', required: false)
wl_scanner_prog = find_program('wayland-scanner', native: true, required: false)
have_wl_scan = wl_scanner_prog.found()
have_webhost_x11 = false
have_webhost_wl = false
if have_web
  gtk4_wayland_dep = dependency('gtk4-wayland', required: false)
  have_webhost_wl = (have_wayland and have_wl_scan and wl_server_dep.found() and
                     wl_client_dep.found() and gtk4_wayland_dep.found())
  if not x11_opt.disabled() and x11.found()
    have_webhost_x11 = dependency('gtk4-x11', required: false).found()
  endif
  if not have_webhost_x11 and not have_webhost_wl
    web_why = ('no page host can be built: the X11 one needs libX11 and GTK\'s X11 backend ' +
               '(gtk4-x11) and is left out by -Dx11=disabled; the private one needs the Wayland ' +
               'backend (-Dwayland), wayland-server and wayland-client 1.20 or newer, ' +
               'wayland-scanner and GTK\'s Wayland backend (gtk4-wayland)')
    if web_opt == 'enabled'
      error('web=enabled, but ' + web_why)
    endif
    message('web wallpapers: ' + web_why + '; the web module is not built')
    have_web = false
    deps_web = []
  endif
endif
if have_webhost_x11 and not have_x11
  # (with the X11 backend these come with deps_core)
  deps_web += x11
  if xi.found()
    deps_web += xi
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
quickjs_malloc_state = false
deps_js = []
if get_option('scenescript') != 'disabled'
  qjs_dirs = ['/usr/lib/quickjs', '/usr/lib64/quickjs', '/usr/local/lib/quickjs',
              '/usr/lib/x86_64-linux-gnu/quickjs']
  # the multiarch directory of the target as the compiler names it
  # (aarch64-linux-gnu, ...; empty on distributions without multiarch)
  qjs_ma = run_command(cc.cmd_array() + ['-print-multiarch'], check: false)
  if qjs_ma.returncode() == 0 and qjs_ma.stdout().strip() != ''
    qjs_ma_dir = '/usr/lib' / qjs_ma.stdout().strip() / 'quickjs'
    if not qjs_dirs.contains(qjs_ma_dir)
      qjs_dirs += [qjs_ma_dir]
    endif
  endif
  # a cross build's sysroot ([properties] sys_root in the cross file)
  qjs_sysroot = meson.get_external_property('sys_root', '')
  if qjs_sysroot != ''
    foreach qjs_d : ['usr/lib/quickjs', 'usr/lib64/quickjs', 'usr/local/lib/quickjs']
      qjs_dirs += [qjs_sysroot / qjs_d]
    endforeach
  endif
  # -Dquickjs_libdir=<dir>: a QuickJS built by hand, or one for the target
  # of a cross build; looked at first
  if get_option('quickjs_libdir') != ''
    qjs_dirs = [get_option('quickjs_libdir')] + qjs_dirs
  endif
  qjs = cc.find_library('quickjs', required: false, dirs: qjs_dirs)
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
    # scenescript.c gives each runtime an allocator that keeps track of the
    # runtime's blocks (an abandoned runtime is freed through it). That
    # needs the allocator interface with JSMallocState; a QuickJS with
    # another one runs on its own allocator as before.
    quickjs_malloc_state = cc.compiles('''
      #include <quickjs/quickjs.h>
      static void *m (JSMallocState *s, size_t n) { return s->malloc_size + n > s->malloc_limit ? 0 : s->opaque; }
      static void f (JSMallocState *s, void *p) { s->malloc_count--; (void) p; }
      static void *r (JSMallocState *s, void *p, size_t n) { (void) s; (void) n; return p; }
      static size_t u (const void *p) { (void) p; return 0; }
      static const JSMallocFunctions mf = { .js_malloc = m, .js_free = f, .js_realloc = r,
                                            .js_malloc_usable_size = u };
      int main (void) { return JS_NewRuntime2 (&mf, 0) != 0; }''',
      args: ['-Werror=incompatible-pointer-types'],
      name: 'QuickJS allocator interface with JSMallocState')
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
conf.set('HAVE_X11', have_x11)
conf.set('HAVE_WAYLAND', have_wayland)
conf.set('HAVE_WL_CURSOR', have_wl_cursor)
conf.set('WALPE_QJS_MALLOC_STATE', have_quickjs and quickjs_malloc_state)
conf.set('HAVE_XI2', xi.found())
conf.set('HAVE_XRANDR', xrandr.found())
conf.set('HAVE_PULSE', have_pulse)
conf.set('HAVE_FFMPEG', have_ffmpeg)
conf.set('HAVE_WEB', have_web)
# the web module's page hosts (src/engine/webhostsel.h)
conf.set('HAVE_WEBHOST_X11', have_webhost_x11)
conf.set('HAVE_WEBHOST_WL', have_webhost_wl)
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
  'src/common/resolution.c',
  'src/common/ipc.c',
  'src/common/ipcsub.c',
  'src/common/tex.c',
  'src/common/gifcheck.c',
  'src/common/region.c',
  'src/common/premult.c',
  'src/common/import.c',
  'src/common/scenevideo.c',
  'src/common/library.c',
  'src/common/steamsub.c',
  'src/common/audio.c',
  'src/common/media.c',
  'src/common/workshop.c',
  'src/common/workshop_query.c',
  'src/common/uitext.c',
  'src/common/weassets.c',
  'src/common/swrender.c',
  'src/common/gpusel.c',
  'src/common/gpucaps.c',
  'src/common/glapi.c',
  'src/common/gpustat.c',
  'src/engine/mp4demux.c',
  'src/engine/pkg.c',
  'src/engine/yuvconv.c',
  'src/engine/glutil.c',
  'src/engine/glcaps.c',
  'src/engine/glfeat.c',
  'src/engine/clip.c',
  'src/engine/gpuprof.c',
  'src/engine/memreport.c',
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
  'src/engine/features.c',
  'src/engine/bloom.c',
  'src/engine/camshake.c',
  'src/engine/scenesound.c',
  'src/engine/video.c',
  'src/engine/modload.c',
)

core = static_library('walpecore', core_srcs,
  include_directories: inc,
  c_args: ['-fvisibility=hidden'],
  dependencies: deps_core + deps_audio + deps_js + deps_media,
  pic: true)

core_dep = declare_dependency(link_with: core, include_directories: inc,
  dependencies: deps_core + deps_audio + deps_js + deps_media + tsan_deps +
                [cc.find_library('dl', required: false)])

# ------------------------------------------------- Wayland protocol code
# wayland-scanner output for the protocols used outside the drawing code:
# by the pause providers (src/signals, as a client), by the web module's
# private display for its page host (src/engine/webhost_wl.c, as the
# server) and by the stand-in compositor of tests/sig_wl_test.c. The XML
# files are in protocols/ (each with its own licence notice), so only
# wayland-scanner is needed, not the wayland-protocols package:
#   ext-idle-notify-v1, ext-foreign-toplevel-list-v1, xdg-shell   from wayland-protocols
#   wlr-foreign-toplevel-management-unstable-v1                    from wlr-protocols
# wl_gen[name] = [client header, server header, code]. The file names of
# the code (wl-<name>-protocol.c) and of the xdg-shell client header are
# chosen not to meet the ones the drawing backend generates from the same
# XML: a build directory holds each output name once.
# (wl_client_dep, wl_server_dep and wayland-scanner: looked for above, with
# the web module's page hosts)
wl_gen = {}
if have_wl_scan
  foreach name : ['ext-idle-notify-v1', 'ext-foreign-toplevel-list-v1',
                  'wlr-foreign-toplevel-management-unstable-v1', 'xdg-shell']
    xml = files('protocols' / name + '.xml')
    wl_gen += {name: [
      custom_target('wl-' + name + '-client-header', input: xml,
        output: (name == 'xdg-shell' ? 'xdg-shell-test' : name) + '-client-protocol.h',
        command: [wl_scanner_prog, 'client-header', '@INPUT@', '@OUTPUT@']),
      custom_target('wl-' + name + '-server-header', input: xml,
        output: name + '-server-protocol.h',
        command: [wl_scanner_prog, 'server-header', '@INPUT@', '@OUTPUT@']),
      custom_target('wl-' + name + '-code', input: xml,
        output: 'wl-' + name + '-protocol.c',
        command: [wl_scanner_prog, 'private-code', '@INPUT@', '@OUTPUT@']),
    ]}
  endforeach
endif

# ---------------------------------------------- pause providers (src/signals)
# What reports "nobody sees the wallpaper" besides the window system the
# daemon draws on (src/signals/providers.h): logind over GDBus (always
# built) and, with the Wayland backend and libwayland-client, the
# compositor's idle notification and window list on a connection of their
# own, and for KWin, which has no window list, a script loaded into it over
# the session bus (HAVE_WL_SIGNALS). The registry starts those only on the Wayland
# backend, so a build without that backend (-Dwayland=disabled) leaves
# them out: its daemon has no Wayland code and links no Wayland library.
# The daemon links signals_dep and calls walpe_sigprov_register ().
signals_srcs = ['src/signals/providers.c', 'src/signals/logind.c']
signals_args = []
signals_deps = [glib, gobject, gio, jsonglib]
have_wl_signals = have_wayland and have_wl_scan and wl_client_dep.found()
if have_wl_signals
  signals_srcs += ['src/signals/wl_session.c', 'src/signals/wl_idle.c', 'src/signals/wl_toplevel.c',
    'src/signals/kwin.c',
    wl_gen['ext-idle-notify-v1'][0], wl_gen['ext-idle-notify-v1'][2],
    wl_gen['wlr-foreign-toplevel-management-unstable-v1'][0],
    wl_gen['wlr-foreign-toplevel-management-unstable-v1'][2]]
  signals_args += ['-DHAVE_WL_SIGNALS']
  signals_deps += [wl_client_dep]
endif
signals_lib = static_library('walpe-signals', signals_srcs,
  include_directories: inc,
  c_args: ['-fvisibility=hidden'] + signals_args,
  dependencies: signals_deps,
  pic: true)
signals_dep = declare_dependency(link_with: signals_lib, include_directories: inc,
  compile_args: signals_args, dependencies: signals_deps)

# Tests and developer tools: only in a build directory set up with
# -Dtests=true (meson_options.txt; off by default). Without it this file
# defines no test, no test program and no developer tool, so `ninja`,
# `meson compile` and `meson install` build what is installed and nothing
# else. With it: `meson test -C build` builds what the tests it runs need,
# one tool is built by its name (`ninja -C build sceneshot`), and - meson
# puts every program a test() names into its default target, whatever
# build_by_default says - a plain `ninja` or `meson install` builds every
# test program too; ./build.sh builds the alias target `programs` (the
# installed programs, at the end of this file) in both cases.
# The test parts of this file stand between `if with_tests` and
# `endif  # with_tests`; their lines are not indented.
with_tests = get_option('tests')

if with_tests
# which provider can work where: capability lists made up after real
# compositors, the window arithmetic, WALPE_LOGIND, the status entry
# "pause_rules", and the real providers with nothing to talk to (each one
# declines with a sentence). No compositor, no bus. The registry is a
# stand-in (tests/sig_stub.h).
test('sig-select', executable('sig-select-test', 'tests/sig_select_test.c',
  include_directories: inc,
  dependencies: [signals_dep],
  install: false, build_by_default: false), timeout: 60)

# the logind provider against a stand-in login manager on a private bus
# (GTestDBus + a thread of the test owning org.freedesktop.login1): how the
# session is found, lock / suspend / inactive session and back, the state
# found at start, a login manager that leaves the bus, WALPE_LOGIND.
# Skipped (77) without dbus-daemon.
test('sig-logind', executable('sig-logind-test', 'tests/sig_logind_test.c',
  include_directories: inc,
  dependencies: [signals_dep],
  install: false, build_by_default: false), timeout: 120)

# the provider for KDE Plasma against a stand-in KWin on a private bus
# (GTestDBus + a thread of the test owning org.kde.KWin with the script
# interface): nothing kept while no rule is on, the script loaded with the
# daemon's unique name in it, reports turned into values per output, only
# KWin believed, outputs whose ids change, the see-through list, KWin
# leaving the bus and coming back, a KWin too old, the script unloaded when
# the rules go off and at stop; the pointer from the Plasma add-on (a name
# on the bus only while it is wanted, only Plasma's shell believed).
# Skipped (77) without dbus-daemon.
if have_wl_signals
  test('sig-kwin', executable('sig-kwin-test', 'tests/sig_kwin_test.c',
    include_directories: inc,
    dependencies: [signals_dep],
    install: false, build_by_default: false), timeout: 120)
endif

# the Wayland providers against a stand-in compositor (libwayland-server on
# a thread, private runtime directory): focus / covered per output,
# fullscreen and the output of the fullscreen window, see-through list,
# idle timeout, outputs that go or change their id, a dropped connection;
# a window list of version 1 and outputs without names (also: a fullscreen
# window whose output cannot be told); a compositor with only the
# stateless ext list.
if have_wl_signals and wl_server_dep.found()
  test('sig-wl', executable('sig-wl-test', 'tests/sig_wl_test.c',
    wl_gen['ext-idle-notify-v1'][1],
    wl_gen['wlr-foreign-toplevel-management-unstable-v1'][1],
    wl_gen['ext-foreign-toplevel-list-v1'][1], wl_gen['ext-foreign-toplevel-list-v1'][2],
    include_directories: inc,
    dependencies: [signals_dep, wl_server_dep],
    install: false, build_by_default: false), timeout: 120)
endif
endif  # with_tests

# ------------------------------------------------------- dlopen'd modules
# WebKitGTK (GTK 4, GStreamer, JavaScriptCore) and FFmpeg are loaded only
# when a web / video wallpaper (or a scene video texture) needs them. They
# resolve the core functions they call (WALPE_API) from the daemon, which
# exports exactly those (modules.dynlist).
if have_web
  web_srcs = ['src/engine/web.c', 'src/engine/webinput.c']
  web_args = ['-fvisibility=hidden']
  web_deps = deps_core + deps_web + [gtk4]
  # the page hosts found above (HAVE_WEBHOST_X11 / HAVE_WEBHOST_WL in
  # walpe-build-config.h): the X11 one is code in web.c and webinput.c, the
  # private one brings its display with it
  if have_webhost_wl
    web_srcs += ['src/engine/webhost_wl.c', wl_gen['xdg-shell'][1], wl_gen['xdg-shell'][2]]
    web_deps += [wl_server_dep, wl_client_dep, gtk4_wayland_dep]
  endif
  web_mod = shared_module('walpe-web', web_srcs,
    name_prefix: '',
    include_directories: inc,
    c_args: web_args,
    dependencies: web_deps,
    install: true,
    install_dir: moddir)
endif

if with_tests
# which page host a web wallpaper gets (src/engine/webhostsel.h): X11
# sessions unchanged, a Wayland backend through Xwayland too and on the
# private display only without an X display (elsewhere opt-in), with the
# fallback to Xwayland and the one from an X display that does not answer
# to the private display, forced names, one-sided builds. Pure.
test('webhost-select', executable('webhost-select-test', 'tests/webhost_select_test.c',
  include_directories: inc,
  dependencies: [glib],
  install: false, build_by_default: false))

# the private page host (src/engine/webhost_wl.c, compiled into the test)
# against a plain Wayland client doing what GTK does: one client only and
# no socket left behind, the globals GTK needs and none a selection could
# travel through, map / frame callbacks / unmap / map again, pointer and
# keyboard events as sent, selections and drags cancelled. No compositor,
# no display.
if have_wl_scan and wl_server_dep.found() and wl_client_dep.found()
  test('webhost-wl', executable('webhost-wl-test', 'tests/webhost_wl_test.c',
    'src/engine/webhost_wl.c',
    wl_gen['xdg-shell'][0], wl_gen['xdg-shell'][1], wl_gen['xdg-shell'][2],
    # (only `inc`: with src/engine itself on the include path the C library's
    # <features.h> would resolve to src/engine/features.h)
    include_directories: inc,
    dependencies: [glib, wl_server_dep, wl_client_dep],
    install: false, build_by_default: false), timeout: 60)
endif
endif  # with_tests
if have_ffmpeg
  video_mod = shared_module('walpe-video', 'src/engine/video_custom.c', 'src/engine/sounddec.c',
    name_prefix: '',
    include_directories: inc,
    c_args: ['-fvisibility=hidden'],
    dependencies: deps_core + deps_ffmpeg,
    install: true,
    install_dir: moddir)
endif

# ---------------------------------------------------------------- executables
dynlist = meson.current_source_dir() / 'src/daemon/modules.dynlist'
# the platform layer: the interface, the pause-signal registry and one
# source per backend that is built
daemon_src = files('src/daemon/main.c', 'src/platform/platform.c', 'src/platform/outputs.c',
  'src/platform/signals.c')
daemon_deps = [core_dep, signals_dep]   # signals_dep: the pause providers of src/signals
if have_x11
  daemon_src += files('src/platform/x11.c', 'src/daemon/vistrack.c')
else
  daemon_src += files('src/daemon/vistrack_none.c')   # "Skip hidden areas" reads the X window tree
endif
if have_wayland
  daemon_src += wayland_backend_src
  daemon_src += wl_generated
  daemon_deps += deps_wayland
endif
# the selection guard for the web module's X11 page host (it defines
# XSetSelectionOwner): with the X11 backend always, as before; without it
# only when that page host is built, which then brings libX11 into the
# process. A build with neither has no X call to guard.
if have_x11
  daemon_src += files('src/daemon/selguard.c')
elif have_webhost_x11
  daemon_src += files('src/daemon/selguard.c')
  daemon_deps += x11
endif
daemon = executable('walpe-daemon', daemon_src,
  include_directories: inc,
  dependencies: daemon_deps,
  link_args: ['-Wl,--dynamic-list=' + dynlist],
  link_depends: 'src/daemon/modules.dynlist',
  install: true)

if with_tests
# what walpe-daemon and the web module make the dynamic loader load: a
# build without the Wayland backend names no Wayland library and has no
# wl_* symbol left, a build without the X11 backend and the X11 page host
# (-Dx11=disabled, or no libX11 on the system) names no X library and has
# no X function left, and no build's daemon names libpulse-simple (scene
# sound loads it with its first stream). tests/linkage_test.sh reads the
# two files with readelf and nm; skipped without them. No display, no daemon.
linkage_deps = [daemon]
if have_web
  linkage_deps += web_mod
endif
test('linkage', find_program('sh'),
  args: [files('tests/linkage_test.sh'), meson.current_build_dir(),
         have_wayland ? 'wayland' : 'nowayland',
         have_x11 or have_webhost_x11 ? 'x11' : 'nox11'],
  depends: linkage_deps, timeout: 60)
endif  # with_tests

# GUI stylesheet compiled in (data/style.css; ~/.config/walpe/style.css overrides)
ui_res = import('gnome').compile_resources('walpe-ui-resources',
  'data/walpe-ui.gresource.xml', source_dir: 'data', c_name: 'walpe_ui')

# X11: the GUI sees its window manager hide the window (winstate.c)
ui_x11_dep = dependency('gtk4-x11', required: false)
ui_x11_args = ui_x11_dep.found() and x11.found() ? ['-DWALPE_UI_X11'] : []
# Wayland: every window of the GUI says the application's id (wlappid.c)
ui_wl_dep = dependency('gtk4-wayland', required: false)
ui_wl_args = ui_wl_dep.found() ? ['-DWALPE_UI_WAYLAND'] : []

# the Steam Workshop browser is a tab of the GUI (walpe_workshop_page_new)
ui = executable('walpe', 'src/ui/main.c', 'src/ui/store.c', 'src/ui/props.c', 'src/ui/layers.c',
  'src/ui/perf.c', 'src/ui/appearance.c', 'src/ui/winstate.c',
  'src/ui/resmodel.c', 'src/ui/resdialog.c',   # render resolution per monitor
  'src/ui/uiipc.c', 'src/ui/thumbs.c', 'src/ui/uistats.c', 'src/ui/workshop_page.c',
  'src/tray/trayconf.c', 'src/ui/delete.c', 'src/ui/wlappid.c', ui_res,
  include_directories: inc,
  c_args: ['-DWALPE_HAVE_WORKSHOP_PAGE'] + ui_x11_args + ui_wl_args,
  dependencies: [core_dep, gtk4] + (ui_x11_args.length() > 0 ? [ui_x11_dep, x11] : []) +
                (ui_wl_args.length() > 0 ? [ui_wl_dep] : []),
  install: true)

ctl = executable('walpe-ctl', 'src/ctl/main.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: true)

if with_tests
# Sanitizer builds (-Db_sanitize=address or address,undefined): the suites
# run with LeakSanitizer, which AddressSanitizer turns on by itself, and
# with the suppressions of tests/lsan.supp (library memory only; the file
# says why for each). `meson test` uses this setup without being asked.
if get_option('b_sanitize').contains('address')
  add_test_setup('sanitize', is_default: true,
    env: ['LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'tests' / 'lsan.supp' +
          ':print_suppressions=0'])
endif

# shader repair (fx_srcrepair.h): source inspection, the error-driven
# fallback under Mesa / NVIDIA / glslang / unknown log wordings, varying
# interface, transform effect (GL part: EGL device or surfaceless, skipped
# when unavailable). `meson test -C build`
test('fx-repair', executable('fx-repair-test', 'tests/fx_repair_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# OpenGL ES path. glapi: the gl_api preference, the context ladder walked
# with made-up EGL capability sets, the fallback table (no GL). fx-essl: the
# GLSL ES output of the effect shader builder - expected text per construct,
# the lexical fallback, and real compiles on an OpenGL ES context where EGL
# surfaceless makes one. gles-scene: generated scenes rendered by a desktop
# OpenGL child and an OpenGL ES child of the test, frames compared (skipped
# without both contexts); `gles-scene-test --scene DIR` does the same for
# any wallpaper.
test('glapi', executable('glapi-test', 'tests/glapi_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))
test('fx-essl', executable('fx-essl-test', 'tests/fx_essl_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))
test('gles-scene', executable('gles-scene-test', 'tests/gles_scene_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 180)

# `walpe-ctl mem` (engine/memreport.h): mappings by kind from a smaps text,
# the process part of the report (resident memory, descriptors, allocator),
# the counters, and the census of GL objects with their sizes (EGL device or
# Mesa surfaceless; that part skips itself without).
test('memreport', executable('memreport-test', 'tests/memreport_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# shared file-texture cache (engine/glutil.c): 1/2-channel storage of RGBA8
# data, one texture for identical pixels, stored mip levels, DXT kept
# compressed, the texture budget. Needs GL (EGL device or Mesa surfaceless);
# skips itself without.
test('texcache', executable('texcache-test', 'tests/texcache_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# how a layer picture gets to the GPU: reduced on the CPU before the upload
# (the texels of the GL blit, never a full-size texture), PNG / JPEG decoded
# on worker threads and picked up by the first frame's load, the reduced
# copy without a share group, a scene with PNG layers at half size. Needs
# GL with a pbuffer (Mesa surfaceless); skips itself without.
texload_test = executable('texload-test', 'tests/texload_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false)
test('texload', texload_test, timeout: 120)
# the same with the opt-in switch: odd-sized pictures reduced before the upload too
test('texload-odd', texload_test, env: ['WALPE_TEX_CPU_REDUCE=2'], timeout: 120)

# what the scene renderer draws per frame: unchanged frames are skipped,
# static layers come from one copy (bake), render_scale; generated scenes
# on EGL surfaceless (skipped when unavailable)
test('scene-frames', executable('scene-frames-test', 'tests/scene_frames_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# layer textures as the input of effects (stored reduced only where that
# gives the same picture; exact with tex_downscale off) and layers attached
# to a puppet's attachment point; generated scenes on EGL surfaceless
# (skipped when unavailable)
test('scene-fit', executable('scene-fit-test', 'tests/scene_fit_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# integration gate: with frame skipping and the static bake an idle scene
# (0 fps) must still repaint when something outside its layers changes the
# picture: "Adjust layers" move / scale / hide / show / highlight / reset,
# render_scale. EGL surfaceless (skipped when unavailable).
test('idle-repaint', executable('idle-repaint-test', 'tests/idle_repaint_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# scene bloom and camera shake. postfx: the parts without GL - the two
# switches (rows of engine/features.h), their place in config.json, the
# bright pass and the blur weights,
# the sizes of a bloom run from 720p to 4K and at odd sizes, the camera
# shake as a function of time (the same path at 30, 60 and 144 fps);
# postfx-env: the switches under WALPE_FEATURES. postfx-gl: generated
# scenes on EGL surfaceless (skipped when unavailable) - bloom that is off
# or cannot show leaves the frame byte-identical, one bright square, render
# sizes up to 4K, render scale, "Skip hidden areas", the shake at a fixed
# instant and with the clock running.
postfx_test = executable('postfx-test', 'tests/postfx_test.c',
  include_directories: inc,
  dependencies: [core_dep, m_lib],
  install: false, build_by_default: false)
test('postfx', postfx_test)
test('postfx-env', postfx_test, args: ['env'],
  env: ['WALPE_FEATURES=bloom=off,camera_shake=on,some_other_feature=off'])
postfx_gl_test = executable('postfx-gl-test', 'tests/postfx_gl_test.c',
  include_directories: inc,
  dependencies: [core_dep, m_lib],
  install: false, build_by_default: false)
test('postfx-gl', postfx_gl_test, timeout: 300)
# the same with the 1:1 copy of the frame that a multisampled window needs
test('postfx-gl-fullcopy', postfx_gl_test, env: ['WALPE_BLOOM_COPY=full'], timeout: 300)

# the scene clock around a pause: frames the daemon draws while a monitor is
# paused (an exposed window, a first frame under a cover) stand at the
# instant of the pause - engine.runtime never runs backwards, timers go on
# at the resume, the paused picture does not move. A generated scene with a
# script, the daemon's calls. EGL surfaceless, skipped when unavailable.
test('scene-clock', executable('scene-clock-test', 'tests/scene_clock_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# what a wallpaper script sees of a real scene (a generated one, through the
# content API): set-then-get of every layer / text / particle instance
# property a script can write, engine.userProperties with the type of each
# option (defaults and values the user stored), the order of init /
# applyUserProperties / update across layers, the shared object, thisLayer.
# EGL surfaceless, skipped when unavailable (and without QuickJS).
test('scene-props', executable('scene-props-test', 'tests/scene_props_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# a generated scene whose scripts drive layers and effects with hostile
# values (version 0.12.2 security pass): zero / huge / non-finite vectors on
# every kind of layer, megabytes of text, re-parenting in circles, a parent
# chain of 100 000 layers built at run time. EGL surfaceless (skipped
# without, and without QuickJS).
test('scene-hostile', executable('scene-hostile-test', 'tests/scene_hostile_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 900)

# the text files of a scene wallpaper and the entries of its folder
# (tests/scene_files_test.c): a FIFO or an outside link as scene.pkg, the
# options window's scan of the folder and links that leave it, the text
# layers' share of the texture budget (surfaceless GL, else skipped),
# particle files with many deaths under many followers.
# Private temp HOME.
test('scene-files', executable('scene-files-test', 'tests/scene_files_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 600)

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
  install: false, build_by_default: false))

# what the effect executor keeps over frames (static cache, held sets of the
# effect update rate, prefixes) against what fx_specialize / fx_quality=fast
# change about who draws a result: fused runs, skipped identity passes, the
# target pool; the comparison of specialised programs at fixed probe times
# (GL part: every EGL device or surfaceless, skipped when unavailable)
test('fx-hold', executable('fx-hold-test', 'tests/fx_hold_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# simulation effects (Wallpaper Engine's Fluid Simulation; src/engine/fx_sim.h):
# the fixed step at 30 / 60 / 144 frames per second, fbos[].fit, the
# pointer's path, the frame-to-layer matrix; with a small swap effect of the
# test's own: determinism, frame-rate independence, never cached / held /
# drawn in part, frames without a step, reset rules, switch and quality
# level (GL part: one EGL device or surfaceless, skipped when unavailable)
test('fx-sim', executable('fx-sim-test', 'tests/fx_sim_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false),
  env: ['WALPE_FLUID=', 'WALPE_FLUID_QUALITY='])

# a layer's opacity goes in before its effects (Wallpaper Engine's order:
# the layer's own material draw), WALPE_FX_ALPHA=after keeps the earlier build's order;
# layers without effects and opaque layers are byte for byte the same in
# both; a scripted opacity is followed in the frame it changes (static,
# animated and held effect sets). Generated scenes on EGL surfaceless
# (skipped when unavailable).
test('fx-alpha', executable('fx-alpha-test', 'tests/fx_alpha_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 180)

# effect passes restricted to what is read later (fx_region.h): which
# lookups count as local, the rectangle arithmetic, and - GL part, every
# EGL device or surfaceless, skipped when unavailable - restricted runs
# against whole runs byte for byte, also over poisoned targets; with the
# Wallpaper Engine assets the reach bounds of WE's shake / waterwaves /
# iris shaders (the check to run on an NVIDIA machine)
test('fx-region', executable('fx-region-test', 'tests/fx_region_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 180)

# the effect shader pipeline against hostile shader text and effect JSON
# (version 0.12.2 security pass): time scan and #include expansion stay bounded,
# seeded mutations of small effects through every text stage and, with EGL
# surfaceless (skipped without), through the executor in every program mode
test('fx-hostile', executable('fx-hostile-test', 'tests/fx_hostile_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 900)

# statements of stock effect shaders, recognised by key (fx_keys.h): the key
# functions against strstr (), the recognisers of fx_rewrite.h and of the
# vertex traits through statements written for the test (no GL). With
# WALPE_WE_ASSETS set, also the key table against that install's own
# shaders (skipped otherwise, and the output says so)
test('fx-rewrite-keys', executable('fx-keys-test', 'tests/fx_keys_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# workshop browsing: parsers on saved Steam replies, settings file mode,
# steamcmd job with a fake steamcmd. WALPE_WS_LIVE=1 adds live Steam calls.
test('workshop', executable('workshop-test', 'tests/workshop_test.c',
  include_directories: inc,
  c_args: ['-DSRCDIR="' + meson.current_source_dir() + '"'],
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# control socket (common/ipc.c): large replies, request limit, socket mode,
# single-instance lock, a client that does not read. No daemon, no display.
test('ipc', executable('ipc-test', 'tests/ipc_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 60)

# x86-64 and aarch64 give the same results (`meson test --suite portable`):
# byte readers at every alignment, the hashes whose values are kept (the
# effect-shader hash behind fx_region_models.h, the program cache key, the
# FNV loop) also over bytes >= 0x80, text with such bytes, numbers beyond
# the int range, no fused multiply-add, the JSON nesting limit on a 1 MiB
# stack, the page size. No display, no GL context.
test('portable', executable('portable-test', 'tests/portable_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), suite: 'portable', timeout: 120)

# the pixel loops of the CPU side (2x2 texture halving with its SSE2
# kernel on x86, YUV -> BGRA full and half size, premultiply) against a
# reference loop and against fixed hashes that are the same number on
# every architecture. No display, no GL context.
test('simd-equal', executable('simd-equal-test', 'tests/simd_equal_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), suite: 'portable', timeout: 120)

# what the daemon and its clients trust on
# the local machine - the socket's directory, config.json's mode and a
# hostile config.json, replies that are not UTF-8, NUL bytes in a request,
# connections from another account, which module files may be loaded.
# No daemon, no display; the cases that need a second account need root.
test('sec-local', executable('sec-local-test', 'tests/sec_local_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# the same review against a running daemon on an X server of its own
# (Xvfb -displayfd; exit 77 = skipped where Xvfb is not installed): a
# runtime directory others can write to, walpe-ctl's request building,
# numbers that are not numbers, descriptors across the layout restart.
# (WALPE_BACKEND=x11: the test brings its own X server; inside a Wayland
# session the daemon would otherwise go to the session's compositor)
if have_x11
  test('sec-daemon', find_program('sh'),
    args: [meson.current_source_dir() / 'tests/sec_daemon_test.sh', daemon, ctl],
    env: ['WALPE_BACKEND=x11', 'WALPE_CHECK_DISPLAY=xvfb'],
    timeout: 240)
endif

# audit input checks: project.json type casing and "file" confinement,
# built-in options per type, non-finite fit values, picture size limit,
# the MP4 demuxer refusing a FIFO. Private temp HOME, no display.
test('guard', executable('guard-test', 'tests/guard_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 60)

# opt-in resource rules in config.json, IPC state pushes
# ("subscribe" / broadcast) and the subscribing client. No daemon, no display.
test('respolicy', executable('respolicy-test', 'tests/respolicy_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 60)

# spectrum analysis: both channels from one FFT, band values, silence
test('audio', executable('audio-test', 'tests/audio_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# scene sound (engine/scenesound.c) on the null device, with WAV files the
# test writes: players, the switches (the setting, a wallpaper's own, volume,
# mute, pause), one voice per wallpaper shown twice, the limits and files
# that are no sound. Never opens a real output; no display.
test('scenesound', executable('scenesound-test', 'tests/scenesound_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), env: ['WALPE_SOUND_NULL=1'], timeout: 240)
# a packaged wallpaper's sound files and the package cache (engine/pkg.c,
# scenenode_sound.h): a copy made with sound off has none of them (files and
# bytes counted), they are extracted when sound is switched on - by a later
# load, by a player, by a scene that is shown at that moment - and the
# picture is the same in every case. Null device; GL part skipped without
# a context.
test('pkgsound', executable('pkgsound-test', 'tests/pkgsound_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), env: ['WALPE_SOUND_NULL=1'], timeout: 180)
# scene sound and the sound server (engine/scenesound.c, out_connect), with
# stand-ins for the five functions of libpulse-simple: players that start
# together open their streams one at a time, one that is refused by a
# server whose queue is full asks again in the same turn, a slow server
# loses nobody, and a server that stops answering holds one player only.
# Contacts no sound server; no display.
if have_pulse
  test('scenesound-server', executable('scenesound-server-test', 'tests/scenesound_server_test.c',
    include_directories: inc,
    dependencies: [core_dep],
    install: false, build_by_default: false), timeout: 120)
endif
# the decoders of compressed sound files (engine/sounddec.c, the part of
# walpe-video.so that scene sound uses, compiled into the test): hostile
# files, MP3 made of silent frames (gapless trimming), and - with FFmpeg's
# own encoders, skipped where the build has none - FLAC, Ogg Vorbis, Ogg Opus
if have_ffmpeg
  test('sounddec', executable('sounddec-test', 'tests/sounddec_test.c', 'src/engine/sounddec.c',
    include_directories: inc,
    dependencies: [core_dep] + deps_ffmpeg,
    install: false, build_by_default: false), env: ['WALPE_SOUND_NULL=1'], timeout: 240)
endif

# the size limits on wallpaper files in the loaders that work at reduced size: the
# shared GIF open, the still image decoded at shown size (limit on the
# declared size), MPRIS cover art and its two-entry cache. Refusals need no
# display; with a surfaceless GL context the good paths run too.
test('limits', executable('limits-test', 'tests/limits_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# video module (engine/video_custom.c, compiled into the test): the box
# filter kernels against a plain loop, the frame limit rules, and - with a
# clip that ffmpeg makes here (skipped without ffmpeg + libx264) - every
# queued frame against a plain libavcodec decode (full size, half size,
# limit off and on), the software fallback of hardware decoding with its
# status line, the attempts played through with stand-in "hardware
# decoders" (a refused stream moves to the next one, a failure in the
# middle of the clip to software, no picture lost or shown twice), and with
# a GL context the uploaded plane textures. Run a second time in the NV12
# layout that hardware decoders deliver.
if have_ffmpeg
  video_test = executable('video-test', 'tests/video_test.c', 'src/engine/sounddec.c',
    include_directories: inc,
    dependencies: [core_dep] + deps_ffmpeg,
    install: false, build_by_default: false)
  test('video', video_test, timeout: 300)
  test('video-nv12', video_test, env: ['WALPE_VIDEO_TEST_NV12=1'], timeout: 300)
  # what `walpe-ctl mem` says the video module was loaded for, when a
  # scene's video texture is the first to ask (the module itself is loaded)
  test('modnote', executable('modnote-test', 'tests/modnote_test.c',
    include_directories: inc,
    dependencies: [core_dep],
    install: false, build_by_default: false), depends: [video_mod])
endif

# version 0.12: config.json fps migration, shared-rendering leader, pacing, YUV matrices
test('pixfps', executable('pixfps-test', 'tests/pixfps_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# decoder threads by need (src/engine/video_threads.h): the count
# never exceeds version 0.12's, a clip that falls behind (also by a few percent)
# gets what its rate asks for in one step, one stall is not a reason. No display.
test('video-threads', executable('video-threads-test', 'tests/video_threads_test.c',
  include_directories: inc,
  dependencies: [glib, m_lib],
  install: false, build_by_default: false))

# hardware video decoding by capability (src/engine/video_hwsel.h): the
# setting's words, the order of the attempts for made-up machines and made-up
# FFmpeg builds, failures before and in the middle of a stream, the status
# line, the DMA-BUF layouts of the zero-copy path, the codec profile text.
# No FFmpeg, no GL, no hardware, no display.
test('video-hwsel', executable('video-hwsel-test', 'tests/video_hwsel_test.c',
  include_directories: inc,
  dependencies: [glib],
  install: false, build_by_default: false))

# walpe-ctl's command line (bad input is named and exits with 1,
# no daemon = 2, refused = 3), what it sends for `set` / `setprop` / `click`
# (a stand-in daemon on a private socket records it), and the daemon's check
# of a "set" request (common/setkeys.h; also read against ipc_set () and
# ipc_status () in the daemon's source and against the key tables of
# docs/SETTINGS.md). No daemon, no display.
test('ctl', executable('ctl-test', 'tests/ctl_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false),
  args: [ctl, files('src/daemon/main.c'), files('docs/SETTINGS.md')], timeout: 120)

# renderer feature switches (engine/features.h): defaults, the serial, their
# rows in common/setkeys.h, "features" in config.json (private
# XDG_CONFIG_HOME) and that the daemon's "set" takes them from the registry.
# WALPE_FEATURES is cleared: the switches are tested from their defaults.
test('features', executable('features-test', 'tests/features_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false),
  args: [files('src/daemon/main.c')], env: ['WALPE_FEATURES='])

# camera parallax, the parts without GL (scenenode.h): the move of a layer
# (direction, depth per axis, the offset term) and the lagging pointer (rest
# in the middle without a pointer, exact arrival, bad input).
test('parallax', executable('parallax-test', 'tests/parallax_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# sprite-sheet image layers, the parts without GL (scenenode.h): the frame
# table as it is played, its validation against the sheet (hostile tables,
# also through the container parser), the clock and its script control, and
# what scripts reach of the camera (QuickJS part skipped without it).
test('sprite-sheet', executable('sprite-sheet-test', 'tests/sprite_sheet_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), env: ['WALPE_FEATURES='], timeout: 120)

# camera parallax and sprite-sheet layers on generated scenes: positions of
# layers for pointer positions, the feature switches, frames of a sheet over
# time and behind an effect, refused sheets. EGL surfaceless (skipped when
# unavailable); the sprite part waits for frames (about 10 s). Not next to
# other tests: its sheet's frames last 2 s of wall-clock time, and the
# first frame of a scene (programs are built in it) must fit into one.
test('scene-feat', executable('scene-feat-test', 'tests/scene_feat_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), env: ['WALPE_FEATURES='], is_parallel: false, timeout: 300)

# config.json: defaults as documented, every key through save +
# load, members of other versions kept by a save, a version 0.12 file, files that
# are not a JSON object kept aside. Private XDG_CONFIG_HOME, no display.
test('config', executable('config-test', 'tests/config_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# the video importer: the 1080p cap and what is copied as it
# is (no ffmpeg needed); with ffmpeg / ffprobe in PATH real runs on clips made
# here: remux, encode, a file that is no video, cancel, nothing left behind.
test('import', executable('import-test', 'tests/import_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 300)

# the converted copy of a video inside a scene (common/scenevideo.c and the
# scene conversion of import.c): which clips get one and how large, the
# copy's name in the cache, the argument vector (no display, no ffmpeg);
# with ffmpeg / ffprobe in PATH real conversions of a clip made here that
# sits inside another file: size, picture count, tags, priority of the child,
# clips that are left alone, a stopped run.
test('scenevideo', executable('scenevideo-test', 'tests/scenevideo_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 600)

# the window system the daemon draws on (common/backend.h): the setting's
# names, --backend / WALPE_BACKEND / config precedence, the automatic rule
# with a made-up environment, builds with one backend, output name
# spellings. No display, no daemon.
test('backend', executable('backend-test', 'tests/backend_test.c',
  include_directories: inc,
  dependencies: [glib],
  install: false, build_by_default: false))

# monitors across changes (platform/outputs.c, the layout decision of
# platform.h): comparing, copying and finding outputs, and what the daemon
# decides when one is plugged in, pulled, changed or its surface is taken
# away. Made-up lists: no display, no daemon.
test('outputs', executable('outputs-test', 'tests/outputs_test.c', 'src/platform/outputs.c',
  include_directories: inc,
  dependencies: [glib, gobject, jsonglib, epoxy, egl, m_lib],
  install: false, build_by_default: false))

# the Wayland backend's arithmetic (sizes, positions, pointer mapping),
# where its source brings such a test along: no Wayland library, no display
if fs.exists('tests/wayland_logic_test.c')
  test('wayland_logic', executable('wayland-logic-test', 'tests/wayland_logic_test.c',
    include_directories: inc,
    dependencies: [glib, m_lib],
    install: false, build_by_default: false))
endif

# the registry of pause-signal providers (platform/signals.c): who starts
# on which backend, "not available" with its reason, values of several
# providers combined, per-output values, a value for every output,
# batches. Made-up providers.
test('signals', executable('signals-test', 'tests/signals_test.c', 'src/platform/signals.c',
  include_directories: inc,
  dependencies: [glib],
  install: false, build_by_default: false))

# which monitors a focused fullscreen window is on (platform/fsplace.h: the
# X11 side of "Pause for fullscreen apps" per monitor): a window on the
# second monitor, across several, with a tiling window manager's border or
# gap, monitors with offsets and turned ones, no monitor, a window without
# a rectangle, the focus on another window. Made-up rectangles: no display.
test('fsplace', executable('fsplace-test', 'tests/fsplace_test.c',
  include_directories: inc,
  dependencies: [glib],
  install: false, build_by_default: false))

# render resolution per output (common/resolution.c, config.json format 4):
# values read and refused, sizes by the output's shape, the texture limit of
# a wallpaper on several monitors, the video box (1080p = the limit that was
# built in), an upgraded config (same settings, outputs asked and native),
# "not asked yet" for an output that appears later. No display, no daemon.
test('resolution', executable('resolution-test', 'tests/resolution_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# presentation rules (present.h): present mode per monitor, swap interval
# with / without a compositing manager, gpu.conf, driver hints. No display.
test('present', executable('present-test', 'tests/present_test.c',
  include_directories: inc,
  dependencies: [glib],
  install: false, build_by_default: false))

# quality-for-load knobs (common/quality.c, engine/fxclass.c): presets,
# JSON, config.json round trip with per-wallpaper settings, the daemon's
# set / setquality / quality requests over a real socket, frame rate by
# state (the focus and battery rules as inputs), saving estimates, effect
# classification. No display, no daemon; private temp config dir.
test('quality', executable('quality-test', 'tests/quality_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 60)

# "Skip hidden areas": the rectangle-set arithmetic behind the visible
# region of a monitor (common/region.c) against a bitmap, on fixed window
# layouts and random ones. No display, no GL.
test('region', executable('region-test', 'tests/region_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# "Skip hidden areas" in the renderer: which lookups of an effect shader
# are local, what each pass has to fill, and (GL part: EGL device or
# surfaceless, skipped when unavailable) that a scene rendered clipped to
# the visible boxes of several window layouts has exactly the pixels of
# the unclipped rendering there. `clip-test <wallpaper dir> [frames] [WxH]`
# runs the comparison on a real scene and prints the fragments shaded.
test('clip', executable('clip-test', 'tests/clip_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 300)

# SceneScript limits and semantics: the time budget of a runaway script, a
# built-in that never returns (regular expression), zero-delay timers,
# promises, recovery after errors. No GL; skips itself without QuickJS.
test('scenescript', executable('scenescript-test', 'tests/scenescript_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# SceneScript against a hostile script (version 0.12.2 security pass): the
# module rewriter on cut-off and deeply nested sources, promise reactions
# that never return, NaN / Infinity / huge vectors at the host boundary,
# thrown values with hostile toString, a seeded driver that calls the whole
# API with wrong types. No GL; skips itself without QuickJS.
test('script-hostile', executable('script-hostile-test', 'tests/script_hostile_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 900)

# particle instance overrides "count" and "rate": values a script drives and
# values of the scene file (WALPE_WE_STATIC_OVERRIDES_AS_WE). No GL, no files.
test('weparticles', executable('weparticles-test', 'tests/weparticles_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# the same test with WALPE_PT_CAP=count in the environment
# (the opt-in switch: the cap of live particles follows the instance's count)
test('weparticles-cap-count', executable('weparticles-capcount-test', 'tests/weparticles_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), env: ['WALPE_PT_CAP=count'])

# the particle simulation without GL - child systems that
# follow events of their parent (one instance per parent particle: bursts of
# eventfollow children, followers carried by their particle, the instance
# count of the child reference), control points placed in the scene, a burst
# whose emitter duration is shorter than the first step. Its particle files
# are written to a private temp dir.
particle_sim_test = executable('particle-sim-test', 'tests/particle_sim_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false)
test('particle-sim', particle_sim_test)
# the same checks with the other reading of alphafade's fade-out time
test('particle-sim-fade-span', particle_sim_test, env: ['WALPE_PT_FADE=span'])

# the particle simulation as a function of scene time: the same system
# stepped at 30, 60, 144 and 200 Hz and with irregular steps holds the same
# particles at the same scene time (rate emitters, bursts, deaths, the cap,
# the instance's "rate", eventspawn children); frames of no length change
# nothing; two systems do not see each other. No GL.
test('particle-rate', executable('particle-rate-test', 'tests/particle_rate_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# particle objects of a scene made by the test, through
# the content API: the instance colour ("colorn" and the older "color" in
# 0..255), options that feed "count" (default, undeclared option, text),
# options that show / hide a particle object, and the same after the user
# changed them. GL part as layers: EGL device or surfaceless, else skipped.
test('particle-scene', executable('particle-scene-test', 'tests/particle_scene_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 180)

# version 0.12.1: sprite-sheet frames of a particle (sequence / once /
# randomframe, sequencemultiplier, frame blending and its flag); no GL
test('particle-sheet', executable('particle-sheet-test', 'tests/particle_sheet_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# version 0.12.1: "Render on CPU" - Mesa's EGL vendor file among others (fake
# vendor dirs), llvmpipe thread count, the software environment, the probe's
# report, the config keys, the control socket kept across a restart in place
test('swrender', executable('swrender-test', 'tests/swrender_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# which GPU renders (common/gpusel.h), without a GPU: the device list read
# from made-up sysfs trees (tests/fakegpu.h), integrated / discrete, the
# values of the gpu setting, the display device, the environment for
# rendering on another GPU, the probe's report, the config keys, the Mesa
# equivalents of gpu.conf's driver hints
test('gpusel', executable('gpusel-test', 'tests/gpusel_test.c',
  include_directories: [inc, include_directories('tests')],
  dependencies: [core_dep],
  install: false, build_by_default: false))

# the capability structure (common/gpucaps.h), without a GL context: driver,
# API and version, features and format fallbacks from the strings and
# extension lists of several drivers (illustrative strings, not captures)
test('gpucaps', executable('gpucaps-test', 'tests/gpucaps_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# "Adjust layers" (common/layeradj.h): layer keys and their matching,
# validation, config.json round trip, forgetting a wallpaper; and, with an
# EGL device or Mesa's surfaceless platform (skipped otherwise), pixel
# checks that a moved / scaled / hidden layer of a scene made by the test
# lands where it should (plain, parented, script-driven, text, particle)
test('layers', executable('layers-test', 'tests/layers_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 180)

# dev tool (not installed): one frame of a wallpaper, rendered off screen
# (EGL pbuffer; no X server, no daemon), as a PNG plus a hash of its pixels:
#   sceneshot <wallpaper dir> <out.png> [--size WxH] [--frames N]
executable('sceneshot', 'tests/sceneshot.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false)

# developer tool (not installed): what a wallpaper's frames cost, as counters
# (fragments shaded, draws, clears, copies, binds, GL calls per frame), and
# a hash of every frame - for before / after and for A/B of a switch:
#   scenecount <wallpaper dir> [--size WxH] [--warm N] [--frames M] [--tsv]
executable('scenecount', 'tests/scenecount.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false)

# a text layer whose string changes is rendered
# and uploaded only where the old or the new string has ink; the texture
# that results is the full render's, byte for byte (no GL)
test('text-part', executable('text-part-test', 'tests/text_part_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# a text layer's ink lies inside its texture and at the side the layer
# aligns it to: left / centre / right x no width limit / wrapped, for Latin,
# right-to-left (Hebrew, Arabic), mixed and CJK strings, with the fonts this
# machine has (no GL)
test('text-layout', executable('text-layout-test', 'tests/text_layout_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# dev tool (not installed): render-thread CPU time of a scene per frame by
# phase (scripts, transforms, text, particle simulation / vertex data / GL,
# layer draws), live particles and - for comparing two builds - hashes of
# the particle state, the particle vertex data and the picture:
#   scenecpu <wallpaper dir> [--size WxH] [--warm N] [--frames M] [--hash K]
executable('scenecpu', 'tests/scenecpu.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false)

# GPU round: particle submission - instanced quads against the vertex by
# vertex reference, off-screen culling, idle systems, the reduced-resolution
# layer for large soft sprites, the density setting. GL part as fx-repair:
# EGL device or surfaceless, skipped when unavailable.
test('particle-draw', executable('particle-draw-test', 'tests/particle_draw_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# wallpaper deletion: path validation against the library roots (symlinks,
# "..", roots, loose media), trash + permanent removal, config forgetting.
# Works in a private temp dir that is also its HOME; nothing else is touched.
test('library', executable('library-test', 'tests/library_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# hostile wallpaper files (version 0.12 audit): preset dependency cycles, pkg
# entries that share bytes / leave the target dir, oversized pictures and
# absurd sprite tables in .tex, asset names that leave the wallpaper; the
# library scan's wallpaper types (any letter case, presets). Private temp HOME.
test('formats', executable('formats-test', 'tests/formats_test.c', 'src/ui/store.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

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
  install: false, build_by_default: false), timeout: 240)

# puppet-warp models (.mdl): animation headers with an absurd length / fps
# must not index outside the frame table. puppet.c is compiled into the
# test (GL calls replaced), no display needed.
test('puppet', executable('puppet-test', 'tests/puppet_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# Steam subscriptions: "is Steam running" (a fake steam.pid/steam.pipe), the
# Workshop-page command with stub `steam` / `xdg-open`, the delete plan's
# Steam-client flag, and Subscribe/Unsubscribe against a loopback mock of
# the Steam Web API. Private temp HOME; no Steam, no network.
test('steamsub', executable('steamsub-test', 'tests/steamsub_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 60)

# what a reply can do to the Workshop client.
# A loopback stand-in for the Steam Web API sends a body without end, gzip
# that unpacks to 160 MiB, an over-long announced length, redirects, 100000
# nested brackets, a hanging and a trickling reply; parser limits on item
# counts and string lengths; the preview decoder allow-list. Private temp
# HOME; no Steam, no network.
test('wsnet', executable('wsnet-test', 'tests/wsnet_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 180)

# text of a wallpaper or a Workshop listing made fit for a label
# (src/common/uitext.c), the library scan's use of it and the import of a
# folder that holds links and a FIFO (src/ui/store.c)
test('uitext', executable('uitext-test', 'tests/uitext_test.c', 'src/ui/store.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# memory / disk reductions keep their results: partial LZ4 decode of padded
# .tex levels, stored mip levels (walpe_tex_load_level / walpe_tex_probe),
# the size-limited cache of extracted scene.pkg files. Private temp HOME.
test('resmem', executable('resmem-test', 'tests/resmem_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# a wallpaper that fails to load: which monitors return to their previous
# wallpaper and what the saved choice becomes (common/restore.h). No
# display: the rules only, not the daemon.
test('restore', executable('restore-test', 'tests/restore_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# the daemon end to end (scripts/lifecheck.py with its harness
# scripts/checkenv.py: a private Xvfb and a private walpe-daemon on
# generated scenes): the pointer after a load, settings
# that reload scenes, the restart in place, quality numbers of a still scene,
# "Pause for fullscreen apps" per monitor (two monitors, a window with the
# EWMH properties of a focused fullscreen one),
# a wallpaper window that another X client destroys, a video conversion
# that cannot start or is cancelled,
# buffer swaps that fail (a lost GL context: one restart in place, no more),
# a restart in place that cannot be begun (the daemon runs on),
# an exit asked for while a large scene loads (the load gives up),
# a video the decoder refuses (the clip is made with the ffmpeg program:
# that case is skipped where there is none).
# Skipped (77) without Xvfb. `meson test -C build daemon-life`
lifecheck_py = find_program('python3', required: false)
# lifecheck.py loads the modules the build has through the daemon (the
# clip of its video case, the web page of its restart case) and leaves
# those parts out where a module is missing. `meson test` builds what a
# test names here and nothing else, so the modules are named.
lifecheck_deps = [daemon, ctl]
if have_web
  lifecheck_deps += web_mod
endif
if have_ffmpeg
  lifecheck_deps += video_mod
endif
if lifecheck_py.found() and have_x11
  test('daemon-life', lifecheck_py,
    args: ['-I', meson.current_source_dir() / 'scripts/lifecheck.py',
           '--build', meson.current_build_dir(), '--display', 'xvfb'],
    env: ['WALPE_BACKEND=x11'],     # its own Xvfb, also inside a Wayland session
    depends: lifecheck_deps,
    is_parallel: false,
    timeout: 900)
  # the library window next to a real daemon (the case window of
  # scripts/lifecheck.py): the frames it draws for the engine's status lines;
  # and a wallpaper's quality panel with the engine's answers (the case
  # panel runs perf-panel-test on the check's display)
  perf_panel_test = executable('perf-panel-test', 'tests/perf_panel_test.c', 'src/ui/perf.c',
    include_directories: inc,
    dependencies: [core_dep, gtk4],
    install: false, build_by_default: false)
  test('ui-life', lifecheck_py,
    args: ['-I', meson.current_source_dir() / 'scripts/lifecheck.py',
           '--build', meson.current_build_dir(), '--display', 'xvfb', '--only', 'window,panel'],
    env: ['WALPE_BACKEND=x11'],
    depends: [daemon, ctl, ui, perf_panel_test],
    is_parallel: false,
    timeout: 300)
  # render resolution per monitor through a real daemon (own Xvfb, private
  # XDG dirs): a new installation, refused values, a render size that is
  # not the monitor's (status, config.json, the picture), monitors that
  # appear and go ("not asked yet"), a config.json of format 3, WALPE_RESOLUTION
  test('daemon-resolution', lifecheck_py,
    args: ['-I', meson.current_source_dir() / 'scripts/rescheck.py',
           '--build', meson.current_build_dir()],
    env: ['WALPE_BACKEND=x11'],     # its own Xvfb, also inside a Wayland session
    depends: [daemon, ctl],
    is_parallel: false,
    timeout: 900)
  # a video inside a scene that is larger than the scene shows it, through a
  # real daemon (own Xvfb, private XDG dirs; scenes and clips made there with
  # the ffmpeg program, skipped without one or without the video module): the
  # copy is made in the cache and played, status says what happens, the
  # picture with the copy against the picture with the original, the switch,
  # a start with the copy there, a conversion that is stopped when its
  # wallpaper goes or the daemon quits, exits by signal while a copy is made
  # (no unfinished file stays), scenes that need no copy, two monitors
  # of different size with the same clip
  if have_ffmpeg
    test('daemon-scene-video', lifecheck_py,
      args: ['-I', meson.current_source_dir() / 'scripts/scenevideocheck.py',
             '--build', meson.current_build_dir()],
      env: ['WALPE_BACKEND=x11'],
      depends: [daemon, ctl, video_mod],
      is_parallel: false,
      timeout: 900)
  endif
endif

# ------------------------------------------------- the daemon on Wayland
# The end-to-end checks again with a private headless Wayland compositor in
# place of Xvfb (scripts/displayenv.py: sway on wlroots' headless backend,
# software rendering, screenshots with grim), and scripts/waylandcheck.py
# for what only real outputs can do: a mode / scale / transform that changes
# under the running daemon, outputs that appear and go, a pointer device,
# the choice between the two backends. Each is skipped (77) without sway +
# grim and in a build without the Wayland backend; the X ones above are
# skipped in a build without the X11 backend. WALPE_CHECK_STRICT=1 in the
# environment turns "cannot run here" into a failure (for a CI job that has
# installed everything).
if lifecheck_py.found() and have_wayland
  # a Wayland socket that accepts the connection and never answers: the
  # daemon gives up after its start-up limit instead of waiting for ever.
  # Needs no display and no compositor.
  test('daemon-wayland-silent', lifecheck_py,
    args: ['-I', meson.current_source_dir() / 'scripts/wlsilentcheck.py',
           '--build', meson.current_build_dir()],
    depends: [daemon],
    timeout: 120)
  # a daemon restarted in place that waits for its compositor and gives
  # up: it removes the control socket it was handed, not the socket another
  # daemon has put there meanwhile. No display, no compositor.
  test('daemon-wayland-wait-socket', lifecheck_py,
    args: ['-I', meson.current_source_dir() / 'scripts/wlwaitcheck.py',
           '--build', meson.current_build_dir()],
    depends: [daemon],
    timeout: 180)
endif
if lifecheck_py.found()
  test('daemon-life-wayland', lifecheck_py,
    args: ['-I', meson.current_source_dir() / 'scripts/lifecheck.py',
           '--build', meson.current_build_dir(), '--display', 'sway'],
    depends: lifecheck_deps,
    is_parallel: false,
    timeout: 900)
  # (a dozen cases, and every change of an output is a restart in place of
  # the daemon, which it spaces ten seconds apart: the longest test here)
  test('daemon-wayland', lifecheck_py,
    args: ['-I', meson.current_source_dir() / 'scripts/waylandcheck.py',
           '--build', meson.current_build_dir()],
    depends: [daemon, ctl],
    is_parallel: false,
    timeout: 1800)
  test('sec-daemon-wayland', find_program('sh'),
    args: [meson.current_source_dir() / 'tests/sec_daemon_test.sh', daemon, ctl],
    env: ['WALPE_CHECK_DISPLAY=sway'],
    is_parallel: false,
    timeout: 240)
endif

# ------------------------------------------------------ sanitizer runs
# The sanitizers are meson's own option:
#   meson setup build-asan -Dtests=true -Db_sanitize=address,undefined -Db_lto=false -Db_lundef=false
#   meson test -C build-asan --setup=asan           reports are failures, leaks are not looked for
#   meson test -C build-asan --setup=asan_leaks     the same with leak detection
#   meson setup build-tsan -Dtests=true -Db_sanitize=thread -Db_lto=false -Db_lundef=false
#   meson test -C build-tsan --setup=tsan
#     (the thread-safety stress tests first, where the tree has them:
#      --suite race; its race-shim test says whether the sanitizer can follow
#      GLib's locks on this machine at all)
# (a setup's name may hold letters, digits and _ only: meson refuses the whole
# configuration for a name with a - in it)
# The setups add the run-time options only (a report ends the test as failed,
# the suppression files of the tree, longer timeouts); a plain `meson test`
# does not need them. tests/tsan.supp and tests/lsan.supp start empty: a
# line is added with the reason it is not ours to fix.
# Thread sanitizer: Mesa's software rasteriser hands pixels between worker
# threads with locks of its own that the sanitizer cannot see; with
# LP_NUM_THREADS=0 it rasterises in the calling thread and the reports are
# about this program's threads only.
san_src = meson.current_source_dir()
add_test_setup('asan',
  env: ['ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=0:strict_string_checks=1:detect_stack_use_after_return=1:print_summary=1',
        'UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:print_summary=1'],
  timeout_multiplier: 4)
add_test_setup('asan_leaks',
  env: ['ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=0:strict_string_checks=1:detect_stack_use_after_return=1:print_summary=1',
        'UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:print_summary=1',
        'LSAN_OPTIONS=suppressions=' + san_src / 'tests/lsan.supp' + ':print_suppressions=0'],
  timeout_multiplier: 4)
# There is ONE setup named tsan and ONE suppression file, tests/tsan.supp, for
# every thread-sanitizer run of the tree (meson keeps the last add_test_setup
# of a name without a word: a second one elsewhere in this file would replace
# this one, and lines added to the other's suppression file would never be
# read). It carries what the thread-safety stress tests ask for as well:
# history_size=5 (more of the other thread's accesses are remembered, so a
# report has both stacks) and timeouts x20 (their media test waits for
# several grace times).
add_test_setup('tsan',
  env: ['TSAN_OPTIONS=suppressions=' + san_src / 'tests/tsan.supp' + ':halt_on_error=0:exitcode=66:second_deadlock_stack=1:history_size=5:print_suppressions=0',
        'LP_NUM_THREADS=0'],
  timeout_multiplier: 20)

# web wallpapers: which part of a captured page frame changed (partial
# uploads): old frame + the reported rectangle of the new one == new frame
test('webdamage', executable('webdamage-test', 'tests/webdamage_test.c',
  include_directories: inc,
  dependencies: [glib],
  install: false, build_by_default: false))

# GPU profiler (gpuprof.h): report formatting (table order, top 15, totals,
# JSON, the no-timer fallback), driver-message de-duplication, pixel and
# upload-size helpers; then, with an EGL device or Mesa's surfaceless
# platform, the real thing on GL 3.3 and GLES 3: timer queries, the
# KHR_debug channel, the counting wrappers, the memory snapshot (skipped
# when no such context exists).
test('gpuprof', executable('gpuprof-test', 'tests/gpuprof_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)

# scripts/walpe-gpu-report.sh against a stand-in nvidia-smi: every field
# supported, fields a laptop GPU reports as [N/A], field names the tool
# does not know, no nvidia-smi at all, no walpe-daemon. Then with the built
# walpe-ctl on fake /proc and /sys trees (tests/fake-gpustat-tree.sh): the
# kernel's statistics of an AMD card, an Intel one, both vendors in one
# machine, the NVIDIA driver with and without its tool, no GPU.
test('gpu-report', find_program('sh'),
  args: [meson.current_source_dir() / 'tests/gpu_report_test.sh',
         meson.current_source_dir() / 'scripts/walpe-gpu-report.sh',
         meson.current_source_dir() / 'tests/fake-nvidia-smi',
         ctl.full_path(),
         meson.current_source_dir() / 'tests/fake-gpustat-tree.sh'],
  depends: [ctl],
  timeout: 240)

# GPU statistics from the kernel (common/gpustat.c): the fdinfo parser on
# hand-written texts (Intel i915 and xe key names, amdgpu, odd values), the
# sampler on fake /proc and /sys trees with the clock stepped by the test
# (a client behind several descriptors and in two processes, counters that
# go backwards, wrap or jump, processes that exit between samples,
# permission denied, other users' processes, two GPUs, a GPU that is
# powered down, a platform device, no GPU), nvidia-smi through the stand-in.
# No GPU and no DRM device needed.
test('gpustat', executable('gpustat-test', 'tests/gpustat_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false),
  args: [meson.current_source_dir() / 'tests/fake-gpustat-tree.sh',
         meson.current_source_dir() / 'tests/fake-nvidia-smi'],
  timeout: 180)

# option labels: WE property text (HTML/BBCode) -> plain readable labels;
# `proplabel-test <project.json>` prints a wallpaper's labels
test('proplabel', executable('proplabel-test', 'tests/proplabel_test.c',
  'src/ui/props.c', 'src/ui/uiipc.c',
  include_directories: inc,
  dependencies: [core_dep, gtk4],
  install: false, build_by_default: false))

# GUI previews (src/ui/thumbs.c): GIF block parser on cut / damaged files,
# animation frames at tile size and their limits. No display.
test('thumbs', executable('thumbs-test', 'tests/thumbs_test.c',
  include_directories: inc,
  dependencies: [core_dep, gtk4],
  install: false, build_by_default: false))

# render resolution per monitor, the GUI's side (src/ui/resmodel.c): the
# value text, the status reply and the listing read into one state, the
# choices offered for a monitor's mode, the check of a custom size, the
# requests, who is asked at start-up and when a new monitor is announced
# (not while a fullscreen application has the focus). No display, no
# daemon, no GTK.
test('resmodel', executable('resmodel-test', 'tests/resmodel_test.c', 'src/ui/resmodel.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false))

# hostile input for the web, video and import parts ------------
# hostile video files: the CPU YUV converter with odd picture sizes (it
# wrote a row past its buffer), the decoder's picture size limit (clips
# written by the test, no encoder needed), the decode thread count. No GL.
if have_ffmpeg
  test('videosec', executable('videosec-test', 'tests/videosec_test.c', 'src/engine/sounddec.c',
    include_directories: inc,
    dependencies: [core_dep] + deps_ffmpeg,
    install: false, build_by_default: false), timeout: 180)
endif
# the importer / converter with stand-in ffmpeg + ffprobe scripts: command
# line, output that is not UTF-8 (the job used to hang), size limit of a
# conversion on open. No ffmpeg, no display.
test('importsec', executable('importsec-test', 'tests/importsec_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 180)
# a hostile web wallpaper against the running daemon: the network switch
# and other loopback ports, the X selections of other clients, page-made
# log lines. Starts its own Xvfb; skips itself without Xvfb / python3 /
# WebKit.
if have_web and have_x11
  websec_selown = executable('websec-selown', 'tests/websec_selown.c',
    dependencies: [x11], install: false, build_by_default: false)
  if have_webhost_x11
    test('websec', find_program('sh'),
      args: [files('tests/websec_test.sh'), meson.current_build_dir()],
      env: ['WALPE_BACKEND=x11'],     # its own Xvfb, also inside a Wayland session
      depends: [daemon, ctl, web_mod, websec_selown],
      is_parallel: false, timeout: 300)
  endif
  # the same hostile page with its window on the private page host
  # (WALPE_WEB_HOST=private; the daemon itself still draws on the test's
  # Xvfb): the page loads and is captured, clicks reach it, and the X
  # selections of the other client survive - here because the page's
  # display has no way to them at all
  if have_webhost_wl
    test('websec-private', find_program('sh'),
      args: [files('tests/websec_test.sh'), meson.current_build_dir()],
      env: ['WALPE_WEB_HOST=private'],
      depends: [daemon, ctl, web_mod, websec_selown],
      is_parallel: false, timeout: 300)
  endif
endif

# real web wallpapers end to end in a session without X: a private headless
# Wayland compositor with Xwayland switched off (scripts/displayenv.py), no
# DISPLAY, generated pages in WebKitGTK on the private page host
# (scripts/webcheck.py): the picture on one and two outputs, animation, the
# Wallpaper Engine listeners, pointer input, outputs that come, change and
# go, pause and resume (CPU time of the page), a page opened while paused,
# restarts, a page process that dies, DISPLAY left over without an X server,
# what is left behind, what the page's sandbox can see, popups and choosers,
# the session's clipboard, network access off. Skipped (77) without sway +
# grim, and where WebKit does not start; not registered in a build without
# the private page host.
if have_web and have_webhost_wl and lifecheck_py.found()
  test('web-wayland', lifecheck_py,
    args: ['-I', meson.current_source_dir() / 'scripts/webcheck.py',
           '--build', meson.current_build_dir()],
    depends: [daemon, ctl, web_mod],
    is_parallel: false,
    timeout: 1800)
endif
# the same pages on the X11 page host: a private Xvfb, the daemon on X11
# (the cases that need outputs that come and go, or a Wayland session, are
# left out there). What the two page hosts have to agree on is checked by
# the same script on both.
if have_web and have_webhost_x11 and have_x11 and lifecheck_py.found()
  test('web-x11', lifecheck_py,
    args: ['-I', meson.current_source_dir() / 'scripts/webcheck.py',
           '--build', meson.current_build_dir(), '--display', 'xvfb'],
    depends: [daemon, ctl, web_mod],
    is_parallel: false,
    timeout: 1500)
endif

# dev tool (not installed): renders the "Wallpaper options" panel of one
# project.json into a PNG without the rest of the GUI (needs a display):
#   propshot <project.json> <out.png> data/style.css [width] [height]
executable('propshot', 'tests/propshot.c', 'src/ui/props.c', 'src/ui/uiipc.c',
  include_directories: inc,
  dependencies: [core_dep, gtk4],
  install: false, build_by_default: false)

# dev tool (not installed, needs a display): time to the first frames of a
# label set up like the GUI's, with N characters of text - the measurement
# behind src/common/uitext.h:
#   labelbench wsdesc 8000 W      Workshop description, unbroken   (9 s CPU)
#   labelbench wsdesc 8000 zw     the same with breaks every 64    (0.2 s)
#   labelbench tile 20000 W       library tile title, unbroken     (2.6 s)
executable('labelbench', 'tests/labelbench.c',
  dependencies: [gtk4],
  install: false, build_by_default: false)

# ---------------------------------------------------- concurrency ("race")
# State shared between the daemon's threads (one render thread per monitor,
# one decode thread per video, workers) on CPUs with weak memory ordering.
# The tests are ordinary stress tests in any build; their point is a
# ThreadSanitizer build, where they must run without a report:
#   meson setup build-tsan -Dtests=true -Db_sanitize=thread -Db_lto=false -Dbuildtype=debugoptimized
#   meson test -C build-tsan --setup=tsan --suite race      (then the whole suite)
# race-shim comes first: it tells whether the sanitizer can follow GLib on
# this machine at all (tests/tsan/gsync_shim.c).
# (the setup `tsan` itself is in the section "sanitizer runs" above: one
# setup and one suppression file, tests/tsan.supp, for the whole tree)

test('race-shim', executable('race-shim-test', 'tests/race_shim_test.c',
  dependencies: [glib, gio, dependency('threads')] + tsan_deps,
  install: false, build_by_default: false), suite: 'race', priority: 10, timeout: 120)

# the shapes that were made safe (common/lazy.h, g_once_init_enter, tests
# under the lock, an atomic signal note), hammered by four threads
test('race-patterns', executable('race-patterns-test', 'tests/race_patterns_test.c',
  include_directories: inc,
  dependencies: [glib, dependency('threads')] + tsan_deps,
  install: false, build_by_default: false), suite: 'race', timeout: 120)
if tsan
  # the same file with the shapes the tree had before: expected to fail,
  # i.e. the sanitizer reports them (shows that it can see each of them)
  test('race-patterns-old-shapes', executable('race-patterns-old', 'tests/race_patterns_test.c',
    include_directories: inc,
    c_args: ['-DRACE_OLD'],
    dependencies: [glib, dependency('threads')] + tsan_deps,
    install: false, build_by_default: false), suite: 'race', should_fail: true, timeout: 120)
endif

# config.c: options asked by render threads while the main thread stores
# the first one; configs loaded and freed on other threads; the save counter
test('race-config', executable('race-config-test', 'tests/race_config_test.c',
  include_directories: inc,
  dependencies: [core_dep, dependency('threads')],
  install: false, build_by_default: false), suite: 'race', timeout: 120)

# media.c: the media thread started, retired after the grace time and
# started again under several users (WALPE_NO_MEDIA: no session bus)
test('race-media', executable('race-media-test', 'tests/race_media_test.c',
  include_directories: inc,
  dependencies: [core_dep, dependency('threads')],
  install: false, build_by_default: false), suite: 'race', timeout: 120)

# media.c: a stand-in music player on a bus of the test's own - what its
# metadata can make the daemon hold (repeated keys, a title of a megabyte)
test('media-player', executable('media-player-test', 'tests/media_player_test.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)
endif  # with_tests

# GPU numbers for a bug report: the kernel's statistics (walpe-ctl gpu-stats) or
# nvidia-smi, /proc samples, then a profile
install_data('scripts/walpe-gpu-report.sh', rename: 'walpe-gpu-report',
  install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
install_data('data/walpe-autostart.desktop',
  install_dir: get_option('sysconfdir') / 'xdg/autostart')
install_data('data/style.css', install_dir: get_option('datadir') / 'walpe')
# licence texts next to the installed programs: WalPE's own (BSD 3-Clause),
# QuickJS's MIT notice (libquickjs is a static library on most systems, so
# the binaries contain it) and the notices of the Wayland protocol
# descriptions in protocols/ (code generated from them is compiled in)
install_data('LICENSE', 'licenses/QuickJS-MIT.txt', 'licenses/Wayland-protocols.txt',
  install_dir: get_option('datadir') / 'licenses' / 'walpe')

# The GUI's desktop entry, named after its application id: on Wayland a
# window gets its icon and its name in task bars from <app id>.desktop
# and from nowhere else (X11 never needed the file). The icon it names is
# installed here, with or without the tray.
install_data('data/io.github.walpe.ui.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('data/icons/walpe.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')

if with_tests
# GUI pieces that used to depend on the session type, without a display:
# when the dark / light preference is taken from the desktop portal (never
# on X11) and what it maps to; how a monitor is named while no engine runs.
# what the window is told about a daemon that gives no answer: no daemon,
# or one that listens and does not get to its socket (also with its queue
# of waiting connections full)
test('ui-ipc', executable('ui-ipc-test', 'tests/uiipc_test.c', 'src/ui/uiipc.c',
  include_directories: inc,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 60)
test('ui-session', executable('ui-session-test', 'tests/ui_session_test.c',
  include_directories: inc,
  dependencies: [gtk4],
  install: false, build_by_default: false))
endif  # with_tests

# ------------------------------------------------------------ walpe-tray
# System tray icon (src/tray/): a separate process, so a tray problem can
# never affect the daemon or the GUI. StatusNotifierItem needs only GDBus
# and is the protocol of every Wayland panel; XEmbed (X11 trays: dwm's
# systray, stalonetray, trayer, polybar) needs libX11 + libXrender (alpha
# icon over the tray background) and is built when they are there
# (WALPE_TRAY_XEMBED). Which one runs: src/tray/traysel.h.
tray_opt = get_option('tray')
have_tray = tray_opt != 'disabled'
if have_tray
  xrender = dependency('xrender', required: false)
  tray_srcs = ['src/tray/main.c', 'src/tray/icon.c', 'src/tray/trayipc.c', 'src/tray/trayconf.c',
               'src/tray/popup.c', 'src/tray/sni.c', 'src/ui/store.c',
               'src/ui/resmodel.c']   # which monitors are new (the tray announces them when the window is closed)
  tray_args = ['-DG_LOG_DOMAIN="walpe-tray"']
  tray_deps = [core_dep, gtk4]
  have_tray_xembed = x11.found() and xrender.found()
  if have_tray_xembed
    tray_srcs += ['src/tray/xembed.c']
    tray_args += ['-DWALPE_TRAY_XEMBED']
    tray_deps += [x11, xrender]
    if xi.found()
      tray_deps += xi   # menu grabs through XInput2 (what GDK listens to)
    endif
  endif
  tray = executable('walpe-tray', tray_srcs,
    include_directories: inc,
    c_args: tray_args,
    dependencies: tray_deps,
    install: true)
  install_data('data/walpe-tray.desktop',
    install_dir: get_option('datadir') / 'applications')
  # the same program under its application id (hidden from menus): desktops
  # look a program's notifications and its icon up by <app id>.desktop
  install_data('data/io.github.walpe.tray.desktop',
    install_dir: get_option('datadir') / 'applications')
  install_data('data/walpe-tray-autostart.desktop', rename: 'walpe-tray.desktop',
    install_dir: get_option('sysconfdir') / 'xdg/autostart')
  # (data/icons/walpe.svg is installed with the GUI's desktop entry)
  # PNG sizes, if a launcher needs them: walpe-tray --export-icon=walpe.png --icon-size=N
  if with_tests
  # dev/test helper (not installed): a StatusNotifierWatcher + host stub
  # that registers walpe-tray's item, dumps icon/tooltip/menu and clicks
  # menu items over dbusmenu
  sni_host_stub = executable('sni-host-stub', 'tests/tray/sni_host_stub.c',
    dependencies: [gio, glib, dependency('cairo')],
    install: false, build_by_default: false)
  if have_tray_xembed
    executable('xembed-host-stub', 'tests/tray/xembed_host_stub.c',
      dependencies: [x11, glib],
      install: false, build_by_default: false)
  endif
  # which protocol is used in which session (traysel.h): X11 with and
  # without a StatusNotifierWatcher / host, Wayland, forced names, a build
  # without XEmbed. No display, no bus.
  test('tray-select', executable('tray-select-test', 'tests/tray_select_test.c',
    include_directories: inc,
    dependencies: [glib],
    install: false, build_by_default: false))
  # the ids of the menu's items (trayids.h): across the menus of every
  # state an id is one item and never another kind of item, "Quit WalPE"
  # keeps its id. No display, no bus.
  test('tray-ids', executable('tray-ids-test', 'tests/tray_ids_test.c',
    include_directories: inc,
    dependencies: [glib, dependency('cairo')],
    install: false, build_by_default: false))
  # walpe-tray next to a real daemon (scripts/lifecheck.py, case tray: own
  # Xvfb, private XDG dirs): what it writes while the engine sends status
  # lines; and (case traymenu: a private session bus, sni-host-stub as the
  # panel) its menu as a panel keeps it that follows the tray from before
  # the engine ran, and the click on "Quit WalPE"; and (case trayreg) a
  # watcher that does not answer the first registration. Skipped (77)
  # without Xvfb; the two cases with the stub are left out without
  # dbus-daemon.
  if lifecheck_py.found() and have_x11
    test('tray-life', lifecheck_py,
      args: ['-I', meson.current_source_dir() / 'scripts/lifecheck.py',
             '--build', meson.current_build_dir(), '--display', 'xvfb', '--only', 'tray,traymenu,trayreg'],
      env: ['WALPE_BACKEND=x11'],
      depends: [daemon, ctl, tray, sni_host_stub],
      is_parallel: false,
      timeout: 300)
  endif
  # the menu again where the report came from: the tray and the daemon as
  # Wayland clients (a private headless sway). Skipped (77) without sway + grim.
  if lifecheck_py.found() and have_wayland
    test('tray-life-wayland', lifecheck_py,
      args: ['-I', meson.current_source_dir() / 'scripts/lifecheck.py',
             '--build', meson.current_build_dir(), '--display', 'sway', '--only', 'traymenu'],
      depends: [daemon, ctl, tray, sni_host_stub],
      is_parallel: false,
      timeout: 300)
  endif
  endif  # with_tests
endif

summary({'x11': have_x11, 'wayland': have_wayland},
  section: 'Display backends (walpe-daemon)', bool_yn: true)
if have_web
  summary({'x11': have_webhost_x11, 'private': have_webhost_wl},
    section: 'Page hosts of web wallpapers (walpe-web.so)', bool_yn: true)
endif

# What ./build.sh builds: the installed programs and modules, nothing else.
# (In a build directory with -Dtests=true a plain `ninja -C build` also
# builds every test program: meson adds the programs of all test()s to its
# default target whatever their build_by_default says.)
programs = [daemon, ui, ctl]
if have_tray
  programs += tray
endif
if have_web
  programs += web_mod
endif
if have_ffmpeg
  programs += video_mod
endif
alias_target('programs', programs)

# Fuzz harnesses for the parsers of wallpaper files (-Dfuzz=enabled), and
# the inputs that once broke one as tests of the suite (-Dtests=true):
# tests/fuzz/meson.build. Without either option nothing of it is defined.
if with_tests or get_option('fuzz').enabled()
  subdir('tests/fuzz')
endif  # with_tests or fuzz

# a text layer of an old-format scene whose texture OpenGL refuses: no
# texture name stays behind (src/engine/scene.c make_text_texture). OpenGL
# is the stand-in of the fuzz harnesses, so no display.
if with_tests
test('scene-text-glerr', executable('scene-text-glerr-test', 'tests/scene_text_glerr_test.c',
  include_directories: inc,
  link_with: fakegl,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 60)

# texture_limit and the texture quality reach a scene in which nothing
# moves: the frame that applies them is asked for (src/engine/scenenode.c),
# and no frame is asked for without them. The stand-in OpenGL again.
test('scene-texlimit-still', executable('scene-texlimit-still-test', 'tests/scene_texlimit_still_test.c',
  include_directories: inc,
  link_with: fakegl,
  dependencies: [core_dep],
  install: false, build_by_default: false), timeout: 120)
endif  # with_tests
