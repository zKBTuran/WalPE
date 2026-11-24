# Fuzz harnesses: one LLVMFuzzerTestOneInput () per parser of bytes that a
# wallpaper brings, and of those that come over the control socket, from
# Steam's servers, from ffprobe and from config.json (fuzz_<target>.c;
# fuzz_common.h says how they are built).
#
#   fuzz-regress-<target>   tests of the normal suite: the harness as a plain
#       program (standalone.c) over tests/fuzz/seeds/<target> and
#       tests/fuzz/regress/<target>. Every input that once crashed a parser
#       is kept in regress/ and must come back; under a sanitizer build the
#       sanitizer is the judge. Any compiler; only with -Dtests=true, like
#       every test. An input that is a danger as a file (a JPEG no decoder
#       comes back from, in a folder a file manager makes thumbnails of) is
#       kept as its hex dump, <name>.hex, which standalone.c reads as the
#       bytes it stands for.
#   fuzz-<target>           the libFuzzer programs, only with -Dfuzz=enabled:
#       CC=clang meson setup bf -Dfuzz=enabled -Db_sanitize=address,undefined \
#           -Db_lundef=false -Dscenescript=disabled
#       ninja -C bf fuzz-tex && bf/tests/fuzz/fuzz-tex -dict=tests/fuzz/dict/tex.dict \
#           corpus/ tests/fuzz/seeds/tex
#   tests/fuzz/build.sh builds the same programs without meson (a machine
#   without GTK); tests/fuzz/mkseeds.py writes the seeds, tests/fuzz/mkdict.py
#   the dictionaries of the JSON targets (from the loaders' string literals).
#
# OpenGL in the harnesses is tests/fuzz/fakegl.c.
fuzz_targets = ['pkg', 'tex', 'mp4', 'gif', 'json', 'puppet', 'particle', 'shader', 'scene', 'scenejson',
                'scenefile', 'sound', 'notes', 'picture', 'steam', 'import', 'config', 'ipc', 'layers', 'thumbs', 'jpegstore', 'library']
fuzz_assets = ['-DFUZZ_ASSETS="' + meson.current_source_dir() / 'seeds' / 'scene' / 'everything.scene' + '"']
# the sound target reads MP3 / FLAC / Ogg only where FFmpeg is (else WAV alone)
fuzz_extra = {'sound': {'src': have_ffmpeg ? files('../../src/engine/sounddec.c') : [],
                        'args': have_ffmpeg ? ['-DFUZZ_HAVE_FFMPEG'] : [],
                        'deps': deps_ffmpeg},
              # the files around a fuzzed scene.json / the set a fuzzed file is one of
              # (a file of the GUI, included by the harness: GTK's headers; GTK brings the
              # real libepoxy, so this one target is linked without the stand-in)
              'thumbs': {'src': [], 'deps': [gtk4],
                         'args': cc.get_supported_arguments(['-Wno-unused-function', '-Wno-unused-variable'])},
              # (the library scan of the GUI: no GTK in it)
              'library': {'src': files('../../src/ui/store.c'), 'deps': [], 'args': []},
              'scenejson': {'src': [], 'deps': [], 'args': fuzz_assets},
              'scenefile': {'src': [], 'deps': [cc.find_library('dl', static: static_deps, required: false)],
                            'args': ['-DFUZZ_ASSETS="' + meson.current_source_dir() / 'seeds' /
                                     'scenefile.assets' + '"']}}

# (walpe-build-config.h lies in the build's root)
fuzz_inc = [inc, include_directories('../..')]

fakegl = static_library('walpe-fuzz-fakegl', 'fakegl.c',
  c_args: cc.get_supported_arguments(['-fno-sanitize=function']),
  dependencies: [epoxy, egl],
  build_by_default: false)

# the picture files of old-format layers and particle sprites through walpe_gl_tex_from_file
fuzz_targets += ['layerpic']
# a package's copy without its sound files, and the walk that completes it (src/engine/pkg.c)
fuzz_targets += ['pkgsound']
# a picture's format and size read from its own header, before any decoder (src/common/pichead.c)
fuzz_targets += ['pichead']

foreach t : fuzz_targets
  x = fuzz_extra.get(t, {'src': [], 'args': [], 'deps': []})
  if with_tests
    replay = executable('fuzz-replay-' + t, 'fuzz_' + t + '.c', 'standalone.c', x['src'],
      include_directories: fuzz_inc,
      c_args: x['args'],
      link_with: t == 'thumbs' ? [] : [fakegl],
      dependencies: [core_dep] + x['deps'],
      install: false, build_by_default: false)
    test('fuzz-regress-' + t, replay,
      args: [meson.current_source_dir() / 'seeds' / t, meson.current_source_dir() / 'regress' / t] +
            (t == 'steam' ? [meson.current_source_dir() / '..' / 'fixtures' / 'workshop'] : []),
      env: ['WALPE_SOUND_NULL=1'], timeout: 300)
  endif
  if get_option('fuzz').enabled()
    executable('fuzz-' + t, 'fuzz_' + t + '.c', x['src'],
      include_directories: fuzz_inc,
      c_args: x['args'],
      link_with: t == 'thumbs' ? [] : [fakegl],
      link_args: ['-fsanitize=fuzzer'],
      dependencies: [core_dep] + x['deps'],
      install: false, build_by_default: false)
  endif
endforeach
