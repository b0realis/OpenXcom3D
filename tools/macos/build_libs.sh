#!/bin/sh
# Builds, on a Mac, the libraries the macOS app comes with, for Apple silicon and Intel at once
# (universal) and macOS 11 (2020) and newer, from the same sources as the Linux and Windows
# releases, each checked against its SHA-256:
# - SDL2 and sdl12-compat (SDL 1.2 on top of SDL2): dynamic libraries the app carries;
# - SDL_image 1.2 with PNG, SDL_mixer 1.2 with Ogg Vorbis and MIDI (TiMidity), SDL_gfx, libpng,
#   libogg and libvorbis: static libraries linked into the game itself;
# - zlib is the system's (every macOS has it).
# The result goes to OpenXcom/build-macos/deps/prefix; configure.sh builds the game against it,
# make_release_macos.sh makes the app. The Xcode command line tools and CMake are all it needs
# (the automatic builds run it on GitHub's Macs).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$ROOT/OpenXcom/build-macos/deps"
SRC="$D/src"
PREFIX="$D/prefix"
JOBS=$(sysctl -n hw.ncpu)
# (a fresh prefix every time: nothing from an earlier build is taken by mistake)
rm -rf "$PREFIX" "$D/work"
mkdir -p "$SRC" "$D/work" "$PREFIX"

# name, file, URL, SHA-256 (the Linux and Windows releases' sources; sdl12-compat as Windows', 1.2.74)
SOURCES="
libpng libpng-1.6.47.tar.gz https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.47.tar.gz 631a4c58ea6c10c81f160c4b21fa8495b715d251698ebc2552077e8450f30454
libogg libogg-1.3.5.tar.gz https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz 0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664
libvorbis libvorbis-1.3.7.tar.gz https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz 0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab
SDL2 SDL2-2.32.10.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz 5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
sdl12-compat sdl12-compat-1.2.74.tar.gz https://github.com/libsdl-org/sdl12-compat/archive/refs/tags/release-1.2.74.tar.gz 2588686c0972e1785829dc3bf436b543c317e6afa30a9b91d48013dd9c110e81
SDL_image SDL_image-1.2.12.tar.gz https://www.libsdl.org/projects/SDL_image/release/SDL_image-1.2.12.tar.gz 0b90722984561004de84847744d566809dbb9daf732a9e503b91a1b5a84e5699
SDL_mixer SDL_mixer-1.2.12.tar.gz https://www.libsdl.org/projects/SDL_mixer/release/SDL_mixer-1.2.12.tar.gz 1644308279a975799049e4826af2cfc787cad2abb11aa14562e402521f86992a
SDL_gfx SDL_gfx-2.0.26.tar.gz https://www.ferzkopp.net/Software/SDL_gfx-2.0/SDL_gfx-2.0.26.tar.gz 7ceb4ffb6fc63ffba5f1290572db43d74386cd0781c123bc912da50d34945446
"

# Downloads one file, trying again after a network hiccup (a name that didn't resolve, a server
# that didn't answer): into a .part file first, so half a file is never taken for the whole.
download() {
	curl -sSfL --retry 5 --retry-all-errors --retry-delay 10 -o "$2.part" "$1" || { echo "$(basename "$2"): download failed ($1)" >&2; exit 1; }
	mv "$2.part" "$2"
}

# Unpacks one source (downloading and checking it first) and goes into its folder. It runs in
# this shell, not in $(...), so a failed download or a wrong checksum stops the whole build.
unpack() {
	line=$(echo "$SOURCES" | awk -v n="$1" '$1 == n')
	file=$(echo "$line" | cut -d' ' -f2); url=$(echo "$line" | cut -d' ' -f3); sum=$(echo "$line" | cut -d' ' -f4)
	[ -f "$SRC/$file" ] || download "$url" "$SRC/$file"
	echo "$sum  $SRC/$file" | shasum -a 256 -c --quiet - >&2 || { echo "$file: wrong SHA-256" >&2; exit 1; }
	rm -rf "$D/work/$1"; mkdir -p "$D/work/$1"
	tar xzf "$SRC/$file" -C "$D/work/$1" --strip-components=1
	cd "$D/work/$1"
}

# Both kinds of Mac in one go, the oldest macOS supported, no build paths in what goes out, and
# for the 2012 sources C11 and warnings, not errors, for what newer compilers made errors (their
# function pointers differ from libpng's only by const).
export MACOSX_DEPLOYMENT_TARGET=11.0
ARCHS="-arch arm64 -arch x86_64"
FLAGS="-mmacosx-version-min=11.0 -O2 -ffile-prefix-map=$D/work/="
OLD_C="-std=gnu11 -Wno-error=incompatible-function-pointer-types -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=implicit-int"
export CPPFLAGS="-I$PREFIX/include"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
# The autoconf builds get both -arch flags with the compiler's flags (autoconf's preprocessor
# checks don't read them, and its dependency tracking can't follow two -arch flags)...
export CFLAGS="$ARCHS $FLAGS $OLD_C" CXXFLAGS="$ARCHS $FLAGS" OBJCFLAGS="$ARCHS $FLAGS" LDFLAGS="$ARCHS -mmacosx-version-min=11.0 -L$PREFIX/lib"
CONFIGURE="--prefix=$PREFIX --disable-dependency-tracking"
# ... the CMake builds get them from CMAKE_OSX_ARCHITECTURES instead (libpng runs the preprocessor
# with the compiler's flags and one architecture of its own, and two -arch flags stop it)
cmake_configure() {
	CFLAGS="$FLAGS $OLD_C" CXXFLAGS="$FLAGS" OBJCFLAGS="$FLAGS" LDFLAGS="-mmacosx-version-min=11.0 -L$PREFIX/lib" \
		cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_PREFIX_PATH="$PREFIX" \
		"-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_FIND_FRAMEWORK=LAST "$@" > /dev/null
}
# Runs one autoconf build's configure quietly; when it fails, the end of its config.log says why
run_configure() {
	./configure $CONFIGURE "$@" > /dev/null || { tail -n 40 config.log; exit 1; }
}

# (libpng's ARM and Intel speedups are chosen for the machine it's built on: plain C serves both)
echo "libpng";     unpack libpng;            cmake_configure -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
	-DPNG_FRAMEWORK=OFF -DPNG_HARDWARE_OPTIMIZATIONS=OFF -B build; cmake --build build -j$JOBS > /dev/null; cmake --install build > /dev/null
echo "libogg";     unpack libogg;            run_configure --disable-shared --enable-static; make -j$JOBS > /dev/null; make install > /dev/null
# (libvorbis's configure asks Apple's linker for a flag it dropped long ago)
echo "libvorbis";  unpack libvorbis;         sed -i '' 's/-force_cpusubtype_ALL//g' configure
run_configure --disable-shared --enable-static --with-ogg="$PREFIX"; make -j$JOBS > /dev/null; make install > /dev/null
echo "SDL2";       unpack SDL2;              cmake_configure -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF -B build
cmake --build build -j$JOBS > /dev/null; cmake --install build > /dev/null
echo "sdl12-compat"; unpack sdl12-compat;         cmake_configure -DSDL2_INCLUDE_DIR="$PREFIX/include/SDL2" -DSDL12TESTS=OFF -DSTATICDEVEL=OFF -B build
cmake --build build -j$JOBS > /dev/null; cmake --install build > /dev/null
cp "$PREFIX/lib/pkgconfig/sdl12_compat.pc" "$PREFIX/lib/pkgconfig/sdl.pc"
# (SDL 1.2's add-ons: their configure would also run a test program built with SDL, which a
# build folder can't start on macOS: sdl12-compat is found through the app's path, and its SDLmain
# starts a Cocoa app; --disable-sdltest keeps the version check without running it)
echo "SDL_image";  unpack SDL_image;         run_configure --with-sdl-prefix="$PREFIX" --disable-sdltest --enable-static --disable-shared \
	--enable-png --disable-png-shared --disable-jpg --disable-tif --disable-webp --disable-imageio LIBS="-lz"; make -j$JOBS > /dev/null; make install > /dev/null
echo "SDL_mixer";  unpack SDL_mixer;         run_configure --with-sdl-prefix="$PREFIX" --disable-sdltest --enable-static --disable-shared \
	--enable-music-wave --enable-music-ogg --disable-music-ogg-shared --enable-music-midi --enable-music-timidity-midi --disable-music-native-midi \
	--disable-music-fluidsynth-midi --disable-music-mod --disable-music-mp3 --disable-music-flac LIBS="-lvorbis -logg"; make -j$JOBS > /dev/null; make install > /dev/null
echo "SDL_gfx";    unpack SDL_gfx;           run_configure --with-sdl-prefix="$PREFIX" --disable-sdltest --enable-static --disable-shared --disable-mmx
make -j$JOBS > /dev/null; make install > /dev/null
# macOS's own zlib, for pkg-config (the game asks it for zlib; macOS has no .pc file for it)
cat > "$PREFIX/lib/pkgconfig/zlib.pc" <<'PC'
Name: zlib
Description: macOS's own zlib
Version: 1.2.12
Libs: -lz
Cflags:
PC
# What the static libraries need in their turn, for pkg-config --static (their .pc files from
# 2012 don't say)
sed -i '' '/^Libs.private:/d' "$PREFIX/lib/pkgconfig/SDL_image.pc" "$PREFIX/lib/pkgconfig/SDL_mixer.pc"
echo "Libs.private: -lpng16 -lz" >> "$PREFIX/lib/pkgconfig/SDL_image.pc"
echo "Libs.private: -lvorbisfile -lvorbis -logg" >> "$PREFIX/lib/pkgconfig/SDL_mixer.pc"
# Every library holds both kinds of Mac
for lib in "$PREFIX"/lib/*.a "$PREFIX"/lib/*.dylib; do
	[ -L "$lib" ] && continue
	archs=$(lipo -archs "$lib")
	case "$archs" in *arm64*x86_64*|*x86_64*arm64*) ;; *) echo "$(basename "$lib") is only for $archs"; exit 1 ;; esac
done
echo "libraries built in $PREFIX"
