#!/bin/sh
# Builds the libraries the self-contained release carries in lib/, so that it runs with nothing
# installed but glibc 2.34 or newer and the system's own display, sound and graphics drivers
# (Ubuntu 22.04 and newer, Debian 12, Fedora, Arch, the Steam Deck ...):
# - SDL2, built to open X11, Wayland, PulseAudio (PipeWire's too) and ALSA from the system when
#   the game starts, so it needs none of them to load;
# - sdl12-compat: SDL 1.2 on top of SDL2, as current distributions ship it;
# - SDL_image 1.2 with PNG, SDL_mixer 1.2 with Ogg Vorbis and MIDI, their codecs linked in;
# - SDL_gfx, a static library linked into the game itself.
# Each source is checked against its SHA-256 and built with the same glibc_compat.h as the game.
# The result goes to OpenXcom/build/portable/prefix; configure.sh links the game against it,
# make_release.sh copies lib/ from it. Sources are downloaded once into OpenXcom/build/portable/src.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
P="$ROOT/tools/portable"
W="$ROOT/OpenXcom/build/portable"
SRC="$W/src"
PREFIX="$W/prefix"
JOBS=8
mkdir -p "$SRC" "$W/work" "$PREFIX"

# name, file, URL, SHA-256
SOURCES="
zlib zlib-1.3.1.tar.gz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
libpng libpng-1.6.47.tar.gz https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.47.tar.gz 631a4c58ea6c10c81f160c4b21fa8495b715d251698ebc2552077e8450f30454
libogg libogg-1.3.5.tar.gz https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz 0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664
libvorbis libvorbis-1.3.7.tar.gz https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz 0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab
SDL2 SDL2-2.32.10.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz 5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
sdl12-compat sdl12-compat-1.2.72.tar.gz https://github.com/libsdl-org/sdl12-compat/archive/refs/tags/release-1.2.72.tar.gz daf6726b89d71120395472dd3cbc16c7a3b0bcbe2c1495de90885d4c2b266d3e
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
	echo "$sum  $SRC/$file" | sha256sum -c --quiet - >&2 || { echo "$file: wrong SHA-256" >&2; exit 1; }
	rm -rf "$W/work/$1"; mkdir -p "$W/work/$1"
	tar xzf "$SRC/$file" -C "$W/work/$1" --strip-components=1
	cd "$W/work/$1"
}

# The same glibc pins as the game (glibc_pins.h), position-independent code, no build paths,
# and for the 2012 sources C11 (GCC 15 reads C23 by default) and warnings, not errors, for what
# GCC 14 made errors (their function pointers differ from libpng's only by const).
FLAGS="-O2 -fPIC -fno-tree-vectorize -include $P/glibc_pins.h -ffile-prefix-map=$W/work/="
OLD_C="-std=gnu11 -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration -Wno-error=int-conversion"
export CFLAGS="$FLAGS $OLD_C" CXXFLAGS="$FLAGS" CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
CMAKE="cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_PREFIX_PATH=$PREFIX"

# What the libraries still ask of a newer glibc (the C23 strtol and sscanf families glibc's
# headers choose, auto-vectorized maths is off above), defined hidden in each of them.
cc -O2 -fPIC -include "$P/glibc_compat.h" -c "$P/glibc_compat.c" -o "$W/glibc_compat.o"
COMPAT="$W/glibc_compat.o"

# libtool writes the build's library folder into every library it links (a home path, and a
# RUNPATH that would hide lib/ from the libraries): this switches that off after configure.
no_rpath() { sed -i 's|^hardcode_libdir_flag_spec=.*|hardcode_libdir_flag_spec=""|; s|^runpath_var=LD_RUN_PATH|runpath_var=NO_RUNPATH|' libtool; }

echo "zlib";       unpack zlib;              ./configure --prefix="$PREFIX" --static > /dev/null; make -j$JOBS > /dev/null; make install > /dev/null
echo "libpng";     unpack libpng;            $CMAKE -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF \
	-DZLIB_LIBRARY="$PREFIX/lib/libz.a" -DZLIB_INCLUDE_DIR="$PREFIX/include" -B build > /dev/null; cmake --build build -j$JOBS > /dev/null; cmake --install build > /dev/null
echo "libogg";     unpack libogg;            ./configure --prefix="$PREFIX" --disable-shared --enable-static --with-pic > /dev/null; make -j$JOBS > /dev/null; make install > /dev/null
echo "libvorbis";  unpack libvorbis;         ./configure --prefix="$PREFIX" --disable-shared --enable-static --with-pic --with-ogg="$PREFIX" > /dev/null; make -j$JOBS > /dev/null; make install > /dev/null
# SDL2: every backend the system provides is opened when the game starts (the *_SHARED options),
# so the library itself needs only glibc.
echo "SDL2";       unpack SDL2;              $CMAKE -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF -DSDL_RPATH=OFF \
	-DSDL_X11_SHARED=ON -DSDL_WAYLAND=ON -DSDL_WAYLAND_SHARED=ON -DSDL_WAYLAND_LIBDECOR_SHARED=ON -DSDL_PULSEAUDIO_SHARED=ON -DSDL_ALSA_SHARED=ON \
	-DSDL_KMSDRM_SHARED=ON -DSDL_PIPEWIRE_SHARED=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF -DSDL_HIDAPI_LIBUSB=OFF \
	-DHAVE_STRLCPY=0 -DHAVE_STRLCAT=0 -DHAVE_WCSLCPY=0 -DHAVE_WCSLCAT=0 -DCMAKE_SHARED_LINKER_FLAGS="$COMPAT" -B build > /dev/null; cmake --build build -j$JOBS > /dev/null; cmake --install build > /dev/null
echo "sdl12-compat"; unpack sdl12-compat;         $CMAKE -DSDL2_INCLUDE_DIR="$PREFIX/include/SDL2" -DSDL12TESTS=OFF -DSTATICDEVEL=OFF -B build > /dev/null
cmake --build build -j$JOBS > /dev/null; cmake --install build > /dev/null
[ -f "$PREFIX/lib/pkgconfig/sdl.pc" ] || ln -sf sdl12_compat.pc "$PREFIX/lib/pkgconfig/sdl.pc"
echo "SDL_image";  unpack SDL_image;         ./configure --prefix="$PREFIX" --with-sdl-prefix="$PREFIX" --disable-static --enable-shared \
	--enable-png --disable-png-shared --disable-jpg --disable-tif --disable-webp LIBS="-lz -lm $COMPAT" > /dev/null; no_rpath; make -j$JOBS > /dev/null; make install > /dev/null
# (SDL_mixer links its library without LIBS: the compat object goes in with LDFLAGS)
echo "SDL_mixer";  unpack SDL_mixer;         ./configure --prefix="$PREFIX" --with-sdl-prefix="$PREFIX" --disable-static --enable-shared \
	--enable-music-wave --enable-music-ogg --disable-music-ogg-shared --enable-music-midi --enable-music-timidity-midi --disable-music-native-midi \
	--disable-music-fluidsynth-midi --disable-music-mod --disable-music-mp3 --disable-music-flac LIBS="-lvorbis -logg -lm" LDFLAGS="$LDFLAGS $COMPAT" > /dev/null
no_rpath; make -j$JOBS > /dev/null; make install > /dev/null
echo "SDL_gfx";    unpack SDL_gfx;           ./configure --prefix="$PREFIX" --with-sdl-prefix="$PREFIX" --enable-static --disable-shared --disable-mmx --with-pic > /dev/null
make -j$JOBS > /dev/null; make install > /dev/null
echo "libraries built in $PREFIX"
