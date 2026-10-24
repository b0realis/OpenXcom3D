#!/bin/sh
# Configures OpenXcom/build-macos: the game for the macOS app, for Apple silicon and Intel at
# once (universal) and macOS 11 and newer, against the libraries build_libs.sh made (run it
# first). OXCE's own app bundle is switched off: make_release_macos.sh puts the app together
# itself (OpenXcom3D's name, the starter that finds the games' data, the two SDL libraries
# inside), so the build is a plain bin/openxcom with OXCE's data next to it, as on Linux.
# Source paths are made relative, so no home folder goes out with it.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/OpenXcom/build-macos"
PREFIX="$B/deps/prefix"
FLAGS="-ffile-prefix-map=$ROOT/OpenXcom/="
[ -f "$PREFIX/lib/libSDL2-2.0.0.dylib" ] || { echo "build the libraries first: tools/macos/build_libs.sh"; exit 1; }
# (pkg-config sees only these libraries, not Homebrew's or MacPorts'; --static adds what the
# static ones need in their turn)
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" cmake -S "$ROOT/OpenXcom" -B "$B" \
	-DCMAKE_BUILD_TYPE=Release -DDEV_BUILD=OFF -DBUILD_PACKAGE=OFF -DCREATE_BUNDLE=OFF -DPKG_CONFIG_ARGN=--static \
	"-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
	-DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" > /dev/null
grep -q "build-macos/deps/prefix/lib" "$B/CMakeCache.txt" || { echo "the build didn't take the libraries build_libs.sh made"; exit 1; }
echo "configured $B for the macOS app (cmake --build there)"
