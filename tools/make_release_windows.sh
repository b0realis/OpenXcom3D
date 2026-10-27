#!/bin/sh
# Packages release/OpenXcom3D-<version>-windows-x86_64/ and its .zip from the Windows build in
# OpenXcom/build-win64 (tools/windows: build_libs.sh, configure.sh, then make -j8 there), the two
# DLLs it comes with, the mod, the docs, play.bat and openxcom3d.patch. It runs as it is on 64-bit
# Windows 10 and 11: only the original games' data is needed.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# the version: the mod's (OpenXcom/mod/OpenXcom3D/metadata.yml, which the patch adds)
VERSION=$(sed -n 's/^version: *//p' "$ROOT/OpenXcom/mod/OpenXcom3D/metadata.yml")
[ -n "$VERSION" ] || { echo "no OpenXcom/mod/OpenXcom3D/metadata.yml: clone OXCE into OpenXcom and apply openxcom3d.patch first"; exit 1; }
NAME=OpenXcom3D-$VERSION-windows-x86_64
R="$ROOT/release/$NAME"
B="$ROOT/OpenXcom/build-win64/bin"
D="$ROOT/OpenXcom/build-win64/deps"
T="$D/toolchain/bin/x86_64-w64-mingw32"
[ -f "$B/openxcom.exe" ] || { echo "build OpenXcom/build-win64 first (see tools/windows/configure.sh)"; exit 1; }
# this script only packages the build: a source file newer than the program means it wasn't
# rebuilt (an old engine version refuses the new mod, and every game check under Wine dies)
NEWER=$(find "$ROOT/OpenXcom/src" -newer "$B/openxcom.exe" -type f \( -name '*.cpp' -o -name '*.h' \) | head -1)
[ -z "$NEWER" ] || { echo "OpenXcom/build-win64 is older than the source ($NEWER): run make -C OpenXcom/build-win64 -j8 first"; exit 1; }
[ -f "$ROOT/openxcom3d.patch" ] || { echo "run tools/make_patch.sh first"; exit 1; }
# a Windows program that needs nothing but Windows and the two DLLs, with no home paths
sh "$ROOT/tools/windows/check.sh" "$B/openxcom.exe" "$D/prefix/bin" || { echo "set up the Windows build with tools/windows/build_libs.sh and configure.sh"; exit 1; }
rm -rf "$R"
mkdir -p "$R/bin" "$R/data" "$R/user/mods" "$R/licenses"
# without their symbol tables (the crash log reads the exported names, which stay)
"$T-strip" -o "$R/bin/openxcom.exe" "$B/openxcom.exe"
for dll in SDL.dll SDL2.dll; do
	"$T-strip" --strip-unneeded -o "$R/bin/$dll" "$D/prefix/bin/$dll"
done
# the libraries' licences (all allow this; the game itself is GPL 3, see LICENSE in the source),
# and the compiler's runtime linked into the game (LLVM's libc++, libunwind and compiler-rt, and
# MinGW-w64's start-up code)
for part in SDL2/LICENSE.txt sdl12-compat/LICENSE.txt SDL_image/COPYING SDL_mixer/COPYING SDL_mixer/timidity/COPYING \
	SDL_gfx/COPYING zlib/LICENSE libpng/LICENSE libogg/COPYING libvorbis/COPYING; do
	cp "$D/work/$part" "$R/licenses/$(echo "$part" | tr '/' '-')"
done
cp "$D/toolchain/LICENSE.TXT" "$R/licenses/llvm-LICENSE.txt"
cp "$D/toolchain/x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt" "$R/licenses/mingw-w64-runtime-COPYING.txt"
cat > "$R/licenses/README.txt" <<'TXT'
The libraries in bin\ (SDL2.dll, SDL.dll) and linked into bin\openxcom.exe, built from their
unchanged sources by tools/windows/build_libs.sh in the OpenXcom3D repository:

  SDL2 2.32.10          zlib licence        https://github.com/libsdl-org/SDL
  sdl12-compat 1.2.74   zlib licence        https://github.com/libsdl-org/sdl12-compat
  SDL_image 1.2.12      zlib licence        https://github.com/libsdl-org/SDL_image (SDL-1.2 branch)
  SDL_mixer 1.2.12      zlib licence        https://github.com/libsdl-org/SDL_mixer (SDL-1.2 branch)
                        (its TiMidity: Perl Artistic Licence)
  SDL_gfx 2.0.26        zlib licence        https://www.ferzkopp.net/wordpress/2016/01/02/sdl_gfx-sdl2_gfx/
  zlib 1.3.1            zlib licence        https://zlib.net
  libpng 1.6.47         libpng licence      https://github.com/pnggroup/libpng
  libogg 1.3.5          BSD 3-clause        https://xiph.org/ogg/
  libvorbis 1.3.7       BSD 3-clause        https://xiph.org/vorbis/

The compiler's runtime, linked into bin\openxcom.exe (llvm-mingw 20260922, LLVM 23.1.2,
https://github.com/mstorsjo/llvm-mingw):

  libc++, libunwind, compiler-rt   Apache 2.0 with LLVM exceptions   llvm-LICENSE.txt
  MinGW-w64 runtime                 its own permissive licences       mingw-w64-runtime-COPYING.txt
TXT
cp -r "$B/common" "$B/standard" "$R/data/"
cp -r "$ROOT/OpenXcom/mod/OpenXcom3D" "$R/user/mods/OpenXcom3D"
# the starter and the short read-me, with Windows line ends (cmd misreads a batch file without
# them); play.bat writes the starting options the first time
sed 's/$/\r/' "$ROOT/tools/windows/play.bat" > "$R/play.bat"
sed 's/$/\r/' "$ROOT/tools/windows/README-Windows.txt" > "$R/README-Windows.txt"
cp "$ROOT/README.md" "$ROOT/OpenXcom3D_manual.md" "$ROOT/openxcom3d.patch" "$R/"
cp "$ROOT/OpenXcom/LICENSE.txt" "$R/LICENSE.txt"
# the zip, without this machine's user and group ids (-X)
rm -f "$ROOT/release/$NAME.zip"
(cd "$ROOT/release" && zip -r -X -q -9 "$NAME.zip" "$NAME")
echo "release: $R ($(du -sh "$R" | cut -f1)), zip: $ROOT/release/$NAME.zip ($(du -sh "$ROOT/release/$NAME.zip" | cut -f1))"
