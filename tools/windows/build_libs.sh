#!/bin/sh
# Builds what the Windows release (64-bit, Windows 10 and 11) needs besides the game, on Linux:
# - the compiler: llvm-mingw (Clang for Windows, with the MinGW-w64 headers and libraries),
#   downloaded once and checked against its SHA-256;
# - SDL2 and sdl12-compat (SDL 1.2 on top of SDL2, as in the Linux release): SDL2.dll and
#   SDL.dll, the two DLLs that go next to the game;
# - SDL_image 1.2 with PNG, SDL_mixer 1.2 with Ogg Vorbis and MIDI (Windows' own and TiMidity),
#   SDL_gfx, zlib, libpng, libogg and libvorbis: static libraries, linked into the game.
# The sources are the Linux release's (tools/portable/build_libs.sh), the same versions with the
# same SHA-256. Everything goes to OpenXcom/build-win64/deps: the compiler to toolchain/, the
# libraries to prefix/. configure.sh sets up the game's build with them.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$ROOT/OpenXcom/build-win64/deps"
SRC="$D/src"
PREFIX="$D/prefix"
TC="$D/toolchain"
JOBS=8
HOST=x86_64-w64-mingw32
# (the libraries from scratch every time: nothing left over from a build before)
rm -rf "$PREFIX"
mkdir -p "$SRC" "$D/work" "$PREFIX"

# The compiler: llvm-mingw 20260922 (LLVM 23.1.2), its build for Linux x86-64 with the UCRT
TC_FILE=llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64.tar.xz
TC_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/$TC_FILE
TC_SUM=bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21
if [ ! -x "$TC/bin/$HOST-clang" ]; then
	[ -f "$SRC/$TC_FILE" ] || curl -sSfL -o "$SRC/$TC_FILE" "$TC_URL"
	echo "$TC_SUM  $SRC/$TC_FILE" | sha256sum -c --quiet - || { echo "$TC_FILE: wrong SHA-256"; exit 1; }
	rm -rf "$TC"; mkdir -p "$TC"
	tar xJf "$SRC/$TC_FILE" -C "$TC" --strip-components=1
fi
export PATH="$TC/bin:$PATH"

# The libraries' sources (name, file, URL, SHA-256): the Linux release's (tools/portable/build_libs.sh),
# but sdl12-compat 1.2.74, the bugfix release of 1.2.72 for Windows: 1.2.72's SDL.dll crashes there
# when SDL_VIDEODRIVER isn't set (strcmp of NULL in SDL_InitSubSystem)
SOURCES="
zlib zlib-1.3.1.tar.gz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
libpng libpng-1.6.47.tar.gz https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.47.tar.gz 631a4c58ea6c10c81f160c4b21fa8495b715d251698ebc2552077e8450f30454
libogg libogg-1.3.5.tar.gz https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz 0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664
libvorbis libvorbis-1.3.7.tar.gz https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz 0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab
SDL2 SDL2-2.32.10.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz 5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
sdl12-compat sdl12-compat-1.2.74.tar.gz https://github.com/libsdl-org/sdl12-compat/archive/refs/tags/release-1.2.74.tar.gz 2588686c0972e1785829dc3bf436b543c317e6afa30a9b91d48013dd9c110e81
SDL_image SDL_image-1.2.12.tar.gz https://www.libsdl.org/projects/SDL_image/release/SDL_image-1.2.12.tar.gz 0b90722984561004de84847744d566809dbb9daf732a9e503b91a1b5a84e5699
SDL_mixer SDL_mixer-1.2.12.tar.gz https://www.libsdl.org/projects/SDL_mixer/release/SDL_mixer-1.2.12.tar.gz 1644308279a975799049e4826af2cfc787cad2abb11aa14562e402521f86992a
SDL_gfx SDL_gfx-2.0.26.tar.gz https://www.ferzkopp.net/Software/SDL_gfx-2.0/SDL_gfx-2.0.26.tar.gz 7ceb4ffb6fc63ffba5f1290572db43d74386cd0781c123bc912da50d34945446
"

# Unpacks one source (taking it from the Linux build's downloads, or downloading it, and
# checking it first), prints its folder.
unpack() {
	line=$(echo "$SOURCES" | awk -v n="$1" '$1 == n')
	file=$(echo "$line" | cut -d' ' -f2); url=$(echo "$line" | cut -d' ' -f3); sum=$(echo "$line" | cut -d' ' -f4)
	if [ ! -f "$SRC/$file" ]; then
		if [ -f "$ROOT/OpenXcom/build/portable/src/$file" ]; then cp "$ROOT/OpenXcom/build/portable/src/$file" "$SRC/$file"
		else curl -sSfL -o "$SRC/$file" "$url"; fi
	fi
	echo "$sum  $SRC/$file" | sha256sum -c --quiet - >&2 || { echo "$file: wrong SHA-256" >&2; exit 1; }
	rm -rf "$D/work/$1"; mkdir -p "$D/work/$1"
	tar xzf "$SRC/$file" -C "$D/work/$1" --strip-components=1
	echo "$D/work/$1"
}

# No build paths in what goes out, and for the 2012 sources C11 and warnings, not errors, for
# what newer compilers made errors (their function pointers differ from libpng's only by const).
FLAGS="-O2 -ffile-prefix-map=$D/work/="
OLD_C="-std=gnu11 -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=implicit-int"
export CC=$HOST-clang CXX=$HOST-clang++ AR=$HOST-ar RANLIB=$HOST-ranlib STRIP=$HOST-strip RC=$HOST-windres WINDRES=$HOST-windres
export CFLAGS="$FLAGS $OLD_C" CXXFLAGS="$FLAGS" CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
CMAKE="cmake -DCMAKE_TOOLCHAIN_FILE=$ROOT/tools/windows/toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_PREFIX_PATH=$PREFIX"
CONFIGURE="--host=$HOST --prefix=$PREFIX"

echo "zlib";       cd "$(unpack zlib)";      make -f win32/Makefile.gcc PREFIX=$HOST- CC=$CC CFLAGS="$FLAGS" libz.a > /dev/null
make -f win32/Makefile.gcc install PREFIX=$HOST- CC=$CC CFLAGS="$FLAGS" SHARED_MODE=0 BINARY_PATH="$PREFIX/bin" INCLUDE_PATH="$PREFIX/include" LIBRARY_PATH="$PREFIX/lib" > /dev/null
echo "libpng";     cd "$(unpack libpng)";    $CMAKE -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF \
	-DZLIB_LIBRARY="$PREFIX/lib/libz.a" -DZLIB_INCLUDE_DIR="$PREFIX/include" -B build > /dev/null; cmake --build build -j$JOBS > /dev/null; cmake --install build > /dev/null
echo "libogg";     cd "$(unpack libogg)";    ./configure $CONFIGURE --disable-shared --enable-static > /dev/null; make -j$JOBS > /dev/null; make install > /dev/null
echo "libvorbis";  cd "$(unpack libvorbis)"; ./configure $CONFIGURE --disable-shared --enable-static --with-ogg="$PREFIX" > /dev/null; make -j$JOBS > /dev/null; make install > /dev/null
echo "SDL2";       cd "$(unpack SDL2)";      $CMAKE -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF -B build > /dev/null
cmake --build build -j$JOBS > /dev/null; cmake --install build > /dev/null
# (sdl12-compat links SDL.dll with GCC's libgcc, for what the compiler calls by itself: Clang's
# own library of those, compiler-rt's builtins, stands in for it)
mkdir -p "$D/work/libgcc"; cp "$(ls "$TC"/lib/clang/*/lib/windows/libclang_rt.builtins-x86_64.a)" "$D/work/libgcc/libgcc.a"
echo "sdl12-compat"; cd "$(unpack sdl12-compat)"; $CMAKE -DSDL2_INCLUDE_DIR="$PREFIX/include/SDL2" -DSDL12TESTS=OFF -DSTATICDEVEL=OFF \
	-DCMAKE_SHARED_LINKER_FLAGS="-L$D/work/libgcc" -B build > /dev/null
cmake --build build -j$JOBS > /dev/null; cmake --install build > /dev/null
cp "$PREFIX/lib/pkgconfig/sdl12_compat.pc" "$PREFIX/lib/pkgconfig/sdl.pc"
# The game links its libraries statically (-static, see src/CMakeLists.txt), so it looks for
# libSDL.a: that's SDL.dll's import library under the name a static library would have.
cp "$PREFIX/lib/libSDL.dll.a" "$PREFIX/lib/libSDL.a"
echo "SDL_image";  cd "$(unpack SDL_image)"; ./configure $CONFIGURE --with-sdl-prefix="$PREFIX" --enable-static --disable-shared \
	--enable-png --disable-png-shared --disable-jpg --disable-tif --disable-webp > /dev/null; make -j$JOBS > /dev/null; make install > /dev/null
echo "SDL_mixer";  cd "$(unpack SDL_mixer)"; ./configure $CONFIGURE --with-sdl-prefix="$PREFIX" --enable-static --disable-shared \
	--enable-music-wave --enable-music-ogg --disable-music-ogg-shared --enable-music-midi --enable-music-timidity-midi --enable-music-native-midi \
	--disable-music-fluidsynth-midi --disable-music-mod --disable-music-mp3 --disable-music-flac LIBS="-lvorbis -logg" > /dev/null; make -j$JOBS > /dev/null; make install > /dev/null
echo "SDL_gfx";    cd "$(unpack SDL_gfx)";   ./configure $CONFIGURE --with-sdl-prefix="$PREFIX" --enable-static --disable-shared --disable-mmx > /dev/null
make -j$JOBS > /dev/null; make install > /dev/null
# What the static libraries need in their turn, for pkg-config --static (their .pc files from
# 2012 don't say)
sed -i '/^Libs.private:/d' "$PREFIX/lib/pkgconfig/SDL_image.pc" "$PREFIX/lib/pkgconfig/SDL_mixer.pc"
echo "Libs.private: -lpng16 -lz" >> "$PREFIX/lib/pkgconfig/SDL_image.pc"
echo "Libs.private: -lvorbisfile -lvorbis -logg -lwinmm" >> "$PREFIX/lib/pkgconfig/SDL_mixer.pc"
# rapidyaml (in the game's libs/) takes Clang for MinGW as no Windows compiler and asks for
# alloca.h, which MinGW doesn't have: its alloca is in malloc.h
printf '/* alloca, for rapidyaml compiled by Clang for MinGW (see build_libs.sh) */\n#include <malloc.h>\n' > "$PREFIX/include/alloca.h"
echo "libraries built in $PREFIX"
