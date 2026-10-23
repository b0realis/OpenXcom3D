#!/bin/sh
# Configures OpenXcom/build-win64 as the Windows release build, on Linux: one 64-bit openxcom.exe
# for Windows 10 and 11 that runs as it comes, with SDL.dll and SDL2.dll next to it:
# - the compiler is llvm-mingw (tools/windows/toolchain.cmake), the libraries the ones
#   build_libs.sh made (run it first), found through their pkg-config files;
# - SDL_image, SDL_mixer, SDL_gfx and their codecs are linked in, and so is the compiler's C++
#   runtime (the game's CMakeLists.txt links statically for MinGW): no other DLLs;
# - source paths are made relative, so no home folder goes out with a release.
# tools/windows/check.sh checks the result; make_release_windows.sh runs it.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/OpenXcom/build-win64"
PREFIX="$B/deps/prefix"
# (the libraries' headers folder too: build_libs.sh puts an alloca.h for rapidyaml there)
FLAGS="-ffile-prefix-map=$ROOT/OpenXcom/= -I$PREFIX/include"
[ -f "$PREFIX/bin/SDL2.dll" ] || { echo "build the libraries first: tools/windows/build_libs.sh"; exit 1; }
cd "$B"
# (DEPS_DIR: no folder, so the game's CMakeLists.txt takes the libraries from pkg-config, as for
# Linux, not from the Visual Studio dependencies package; --static: with what the static
# libraries need in their turn)
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" cmake .. \
	-DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/windows/toolchain.cmake" -DDEPS_DIR=none -DPKG_CONFIG_ARGN=--static \
	-DCMAKE_BUILD_TYPE=Release -DDEV_BUILD=OFF -DBUILD_PACKAGE=OFF \
	-DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" > /dev/null
grep -q "build-win64/deps/prefix/lib.*-lSDL_gfx" CMakeCache.txt || { echo "the build didn't take the Windows libraries"; exit 1; }
echo "configured $B for the Windows release (make -j8 there)"
