#!/bin/sh
# Configure (or reconfigure) and build WalPE.
#   ./build.sh                      the generic build: runs on any CPU of this architecture
#   ./build.sh --tuning=alderlake   the maintainer's make.conf flags (Alder Lake-class CPUs only)
#   --tuning=<generic|alderlake|native|auto> is passed on as -Dnative_tuning=<value>
#   WALPE_JOBS  parallel jobs   (ninja -j, default: the number of CPUs)
#   WALPE_LOAD  max load avg    (ninja -l, default: the number of CPUs + 2)
#               the maintainer's MAKEOPTS: WALPE_JOBS=16 WALPE_LOAD=18 ./build.sh --tuning=alderlake
#   WALPE_BUILDDIR  build dir   (default: build)
# Other arguments go to `meson setup`, e.g.
#   ./build.sh -Dweb=disabled
# An existing build dir is reconfigured and keeps the options it was set up
# with: give --tuning= once to change its tuning.
set -eu

cd "$(dirname "$0")"

ncpu=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
case "$ncpu" in '' | *[!0-9]*) ncpu=4 ;; esac
jobs=${WALPE_JOBS:-$ncpu}
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

exec ninja -C "$dir" -j "$jobs" -l "$load"
