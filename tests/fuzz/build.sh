#!/bin/sh
# tests/fuzz/build.sh - build the fuzz harnesses without meson.
#
# For a machine that has clang with libFuzzer but not what the whole of
# WalPE needs (GTK 4, WebKitGTK, FFmpeg): the engine's core sources are
# compiled as they are, with the sanitizers, into one archive, and each
# harness is linked against it. Where meson can configure the project,
# `meson setup build -Dfuzz=enabled` does the same.
#
#   tests/fuzz/build.sh OUTDIR [target ...]      (no target: all of them)
#
# Environment:
#   CC           the compiler (clang)
#   FUZZ_SAN     instrumentation of every source
#                (-O1 -g -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
#   FUZZ_LINK    what links a harness (-fsanitize=fuzzer,address,undefined)
#   FUZZ_PKGS    pkg-config packages; EXTRA_CFLAGS / EXTRA_LIBS for libraries
#                that are not installed where pkg-config looks
#   FUZZ_FFMPEG_CFLAGS / FUZZ_FFMPEG_LIBS   libavcodec + libavutil for the
#                sound target (found through pkg-config when unset)
#   JOBS         compilers at once (2)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${1:?usage: build.sh OUTDIR [target ...]}
shift
mkdir -p "$out/obj"
out=$(cd "$out" && pwd)
CC=${CC:-clang}
FUZZ_SAN=${FUZZ_SAN:--O1 -g -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined}
FUZZ_LINK=${FUZZ_LINK:--fsanitize=fuzzer,address,undefined}
FUZZ_PKGS=${FUZZ_PKGS:-glib-2.0 gobject-2.0 gio-2.0 json-glib-1.0 gdk-pixbuf-2.0 pangocairo pangoft2 fontconfig epoxy egl liblz4}
JOBS=${JOBS:-2}
cflags="-std=gnu11 -D_GNU_SOURCE -ffp-contract=off -fno-omit-frame-pointer -I$out -I$top/src -I$here $(pkg-config --cflags $FUZZ_PKGS) ${EXTRA_CFLAGS:-}"
libs="$(pkg-config --libs $FUZZ_PKGS) ${EXTRA_LIBS:-} -lm -ldl"

# what meson's configure_file writes; no optional part is built
if [ ! -f "$out/walpe-build-config.h" ]; then
    ver=$(sed -n "s/^  version: '\(.*\)',/\1/p" "$top/meson.build" | head -n 1)
    cat > "$out/walpe-build-config.h" <<CONF
#pragma once
#define WALPE_VERSION "$ver"
#define WALPE_CODENAME "FUZZ"
#define WALPE_MODULEDIR "/nonexistent"
CONF
fi

srcs=$(sed -n '/^core_srcs = files(/,/^)/p' "$top/meson.build" | grep -o "src/[a-z_/0-9]*\.c")
echo "$srcs" | tr ' ' '\n' | while read -r s; do
    o="$out/obj/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
    if [ ! -f "$o" ] || [ "$top/$s" -nt "$o" ]; then
        printf '%s\0%s\0' "$top/$s" "$o"
    fi
done | xargs -0 -r -n 2 -P "$JOBS" sh -c 'echo "cc $1"; exec '"$CC $FUZZ_SAN $cflags"' -w -c "$1" -o "$2"' sh
# the stand-in OpenGL is called through pointers of other types on purpose
$CC $FUZZ_SAN -fno-sanitize=function $cflags -w -c "$here/fakegl.c" -o "$out/obj/fakegl.o"
rm -f "$out/libwalpecore.a"
ar rcs "$out/libwalpecore.a" "$out"/obj/src_*.o

[ $# -gt 0 ] || set -- $(cd "$here" && ls fuzz_*.c | sed 's/^fuzz_//; s/\.c$//')
# the sound target with FFmpeg's decoders where libavcodec is (else WAV only):
# FUZZ_FFMPEG_CFLAGS / FUZZ_FFMPEG_LIBS, or pkg-config
if [ -z "${FUZZ_FFMPEG_LIBS:-}" ] && pkg-config --exists libavcodec libavutil 2>/dev/null; then
    FUZZ_FFMPEG_CFLAGS=$(pkg-config --cflags libavcodec libavutil)
    FUZZ_FFMPEG_LIBS=$(pkg-config --libs libavcodec libavutil)
fi
for t in "$@"; do
    echo "link fuzz-$t"
    more=""
    if [ "$t" = sound ] && [ -n "${FUZZ_FFMPEG_LIBS:-}" ]; then
        more="-DFUZZ_HAVE_FFMPEG ${FUZZ_FFMPEG_CFLAGS:-} $top/src/engine/sounddec.c $FUZZ_FFMPEG_LIBS"
    fi
    [ "$t" = scenejson ] && more="-DFUZZ_ASSETS=\"$here/seeds/scene/everything.scene\""
    $CC $FUZZ_SAN $FUZZ_LINK $cflags -w "$here/fuzz_$t.c" $more "$out/obj/fakegl.o" "$out/libwalpecore.a" $libs -o "$out/fuzz-$t"
done
