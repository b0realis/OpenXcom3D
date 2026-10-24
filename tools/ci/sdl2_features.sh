#!/bin/sh
# Checks that the SDL2 the Linux release comes with was built with the features it was built
# with so far (sdl2_linux_features.txt, the 1.03 release's): SDL2 switches a backend off
# without a word when its headers are missing at build time, so a build machine with other
# packages makes a different SDL2. The list is every #define of SDL_config.h but the C
# library's own (HAVE_*, SIZEOF_*).
# Usage: sdl2_features.sh PREFIX/include/SDL2/SDL_config.h tools/ci/sdl2_linux_features.txt
# (and to write the list anew:  sdl2_features.sh SDL_config.h > sdl2_linux_features.txt)
CONFIG="$1"
EXPECTED="$2"
[ -f "$CONFIG" ] || { echo "usage: sdl2_features.sh SDL_config.h [EXPECTED]"; exit 1; }
features() {
	grep '^#define ' "$CONFIG" | grep -v '^#define \(HAVE_\|SIZEOF_\|SDL_config_h_\|_SDL_config\)' | awk '{ $1 = ""; sub(/^ /, ""); print }' | sort
}
if [ -z "$EXPECTED" ]; then features; exit 0; fi
if features | diff -u "$EXPECTED" - ; then
	echo "SDL2: the same features as the releases' ($(grep -c 'DRIVER' "$EXPECTED") drivers)"
else
	echo "SDL2 was built with other features than the releases' (above: - expected, + this build)"
	exit 1
fi
