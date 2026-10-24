#!/bin/sh
# Configures OpenXcom/build as the release build: one Linux x86-64 game that runs as it comes on
# old and new distributions alike, with nothing to install but the original games' data:
# - glibc: glibc_compat.h binds the few calls that newer glibc versions renamed to their old
#   versions, so it runs on glibc 2.34 (2021) and every newer one;
# - SDL 1.2 (sdl12-compat on SDL2), SDL_image and SDL_mixer come with it in lib/, built by
#   build_libs.sh (run it first); the game finds them through its library path, $ORIGIN/../lib
#   (an RPATH, so it holds for the libraries they load too);
# - SDL_gfx and zlib are linked in, and the compiler's own C++ runtime (libstdc++, about 1.5 MB:
#   a dynamic one would need a system at least as new as this compiler). glibc_compat.c gives it
#   the functions it asks of a newer glibc. libgcc_s (exception unwinding) and OpenGL stay the
#   system's: every system has them.
# Source paths are made relative, so no home folder goes out with a release.
# tools/portable/check.sh checks the result; make_release.sh runs it.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
P="$ROOT/tools/portable"
B="$ROOT/OpenXcom/build"
PREFIX="$B/portable/prefix"
FLAGS="-ffile-prefix-map=$ROOT/OpenXcom/= -include $P/glibc_compat.h"
[ -f "$PREFIX/lib/libSDL2-2.0.so.0" ] || { echo "build the libraries first: tools/portable/build_libs.sh"; exit 1; }
mkdir -p "$B/portable"
# the functions the linked-in C++ runtime asks of a newer glibc (linked in as an object)
cc -O2 $FLAGS -c "$P/glibc_compat.c" -o "$B/portable/glibc_compat.o"
# the build's own game finds the same libraries as a release (bin/../lib)
ln -sfn portable/prefix/lib "$B/lib"
cd "$B"
# (-U: forget the system's SDL libraries found before; pkg-config sees only the bundled ones)
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" cmake .. \
	-U 'PKG_SDL*' -U 'pkgcfg_lib_PKG_*' -U '__pkg_config_checked_PKG_*' -U 'PKG_ZLIB*' \
	-DCMAKE_BUILD_TYPE=Release -DDEV_BUILD=OFF -DBUILD_PACKAGE=OFF \
	-DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
	-DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ $B/portable/glibc_compat.o -Wl,--disable-new-dtags -Wl,-rpath,'\$ORIGIN/../lib'" > /dev/null
grep -q "portable/prefix/lib.*-lSDL_gfx" CMakeCache.txt || { echo "the build didn't take the bundled libraries"; exit 1; }
echo "configured $B for the release (make -j8 there)"
