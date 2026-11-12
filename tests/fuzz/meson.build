# Fuzz harnesses: one LLVMFuzzerTestOneInput () per parser of bytes that a
# wallpaper brings (fuzz_<target>.c; fuzz_common.h says how they are built).
#
#   fuzz-regress-<target>   tests of the normal suite: the harness as a plain
#       program (standalone.c) over tests/fuzz/seeds/<target> and
#       tests/fuzz/regress/<target>. Every input that once crashed a parser
#       is kept in regress/ and must come back; under a sanitizer build the
#       sanitizer is the judge. Any compiler; only with -Dtests=true, like
#       every test.
#   fuzz-<target>           the libFuzzer programs, only with -Dfuzz=enabled:
#       CC=clang meson setup bf -Dfuzz=enabled -Db_sanitize=address,undefined \
#           -Db_lundef=false -Dscenescript=disabled
#       ninja -C bf fuzz-tex && bf/tests/fuzz/fuzz-tex -dict=tests/fuzz/dict/tex.dict \
#           corpus/ tests/fuzz/seeds/tex
#   tests/fuzz/build.sh builds the same programs without meson (a machine
#   without GTK); tests/fuzz/mkseeds.py writes the seeds.
#
# OpenGL in the harnesses is tests/fuzz/fakegl.c.
fuzz_targets = ['pkg', 'tex', 'mp4', 'gif', 'json', 'puppet', 'particle', 'shader', 'scene', 'scenejson',
                'sound']
# the sound target reads MP3 / FLAC / Ogg only where FFmpeg is (else WAV alone)
fuzz_extra = {'sound': {'src': have_ffmpeg ? files('../../src/engine/sounddec.c') : [],
                        'args': have_ffmpeg ? ['-DFUZZ_HAVE_FFMPEG'] : [],
                        'deps': deps_ffmpeg},
              # the files around a fuzzed scene.json
              'scenejson': {'src': [], 'deps': [],
                            'args': ['-DFUZZ_ASSETS="' + meson.current_source_dir() /
                                     'seeds' / 'scene' / 'everything.scene' + '"']}}

# (walpe-build-config.h lies in the build's root)
fuzz_inc = [inc, include_directories('../..')]

fakegl = static_library('walpe-fuzz-fakegl', 'fakegl.c',
  c_args: cc.get_supported_arguments(['-fno-sanitize=function']),
  dependencies: [epoxy, egl],
  build_by_default: false)

foreach t : fuzz_targets
  x = fuzz_extra.get(t, {'src': [], 'args': [], 'deps': []})
  if with_tests
    replay = executable('fuzz-replay-' + t, 'fuzz_' + t + '.c', 'standalone.c', x['src'],
      include_directories: fuzz_inc,
      c_args: x['args'],
      link_with: fakegl,
      dependencies: [core_dep] + x['deps'],
      install: false, build_by_default: false)
    test('fuzz-regress-' + t, replay,
      args: [meson.current_source_dir() / 'seeds' / t, meson.current_source_dir() / 'regress' / t],
      env: ['WALPE_SOUND_NULL=1'], timeout: 300)
  endif
  if get_option('fuzz').enabled()
    executable('fuzz-' + t, 'fuzz_' + t + '.c', x['src'],
      include_directories: fuzz_inc,
      c_args: x['args'],
      link_with: fakegl,
      link_args: ['-fsanitize=fuzzer'],
      dependencies: [core_dep] + x['deps'],
      install: false, build_by_default: false)
  endif
endforeach
