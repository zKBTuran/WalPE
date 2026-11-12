#!/bin/sh
# Configure (or reconfigure) and build WalPE.
#   ./build.sh                      the generic build: runs on any CPU of this architecture
#                                   (x86-64 or aarch64, whichever this machine is)
#   ./build.sh --tuning=alderlake   the maintainer's make.conf flags (Alder Lake-class CPUs only;
#                                   x86-64: on another architecture the configure step refuses it)
#   ./build.sh --tuning=native      the CPU of this machine (-march=native, on aarch64 -mcpu=native)
#   --tuning=<generic|alderlake|native|auto> is passed on as -Dnative_tuning=<value>
#   Building for arm64 on an x86-64 machine: see cross/aarch64-linux-gnu.ini (meson setup
#   --cross-file ...; this script configures native builds only).
#   WALPE_JOBS  parallel jobs   (ninja -j, default: the number of CPUs; 0 = the default)
#   WALPE_LOAD  max load avg    (ninja -l, default: the number of CPUs + 2)
#               the maintainer's MAKEOPTS: WALPE_JOBS=16 WALPE_LOAD=18 ./build.sh --tuning=alderlake
#   WALPE_BUILDDIR  build dir   (default: build in the source directory; a
#               relative path is taken from where the script is called)
# Other arguments go to `meson setup`, e.g.
#   ./build.sh -Dweb=disabled
# An existing build dir is reconfigured and keeps the options it was set up
# with: give --tuning= once to change its tuning.
set -eu

# a relative WALPE_BUILDDIR is meant from where the script was called, not
# from the source directory the script changes to
case "${WALPE_BUILDDIR:-}" in
	'' | /*) ;;
	*) WALPE_BUILDDIR=$PWD/$WALPE_BUILDDIR ;;
esac

cd "$(dirname "$0")"

ncpu=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
case "$ncpu" in '' | *[!0-9]*) ncpu=4 ;; esac
jobs=${WALPE_JOBS:-$ncpu}
# 0 would be `ninja -j 0`, which is no limit at all: 0 and anything that is
# no number mean the default
case "$jobs" in '' | *[!0-9]*) jobs=$ncpu ;; esac
[ "$jobs" -gt 0 ] || jobs=$ncpu
load=${WALPE_LOAD:-$((ncpu + 2))}
dir=${WALPE_BUILDDIR:-build}

# --tuning=<value> becomes -Dnative_tuning=<value>; everything else as given
n=$#
while [ "$n" -gt 0 ]; do
	a=$1
	shift
	n=$((n - 1))
	case "$a" in
		--tuning=*) set -- "$@" "-Dnative_tuning=${a#--tuning=}" ;;
		*) set -- "$@" "$a" ;;
	esac
done

if [ -f "$dir/build.ninja" ]; then
	meson setup --reconfigure "$dir" "$@"
else
	meson setup "$dir" "$@"
fi

# the installed programs only (meson.build: alias_target programs). Tests and
# developer tools exist only in a build directory set up with -Dtests=true
# (./build.sh -Dtests=true); there `meson test` or `ninja -C <dir> <name>`
# builds them, this script still does not
exec ninja -C "$dir" -j "$jobs" -l "$load" programs
