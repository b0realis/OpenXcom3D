#!/bin/sh
# Checks that the game and the libraries it comes with run on old and new Linux systems (see
# configure.sh and build_libs.sh): nothing asks for more than glibc 2.34, the C++ runtime is
# linked in, the game finds its libraries in ../lib, the libraries need nothing but glibc (and
# each other), and there is no home folder in any of it.
# Usage: check.sh BINARY [LIBDIR]
B="$1"
L="$2"
[ -x "$B" ] || { echo "usage: check.sh BINARY [LIBDIR]"; exit 1; }
ok=1
# The newest glibc a file asks for, and complaints about anything newer than 2.34.
glibc_ok() {
	newest=$(objdump -T "$1" | grep -o "GLIBC_[0-9.]*" | sed 's/GLIBC_//' | sort -V | tail -1)
	if [ "$(printf '%s\n2.34\n' "$newest" | sort -V | tail -1)" != "2.34" ]; then
		echo "$(basename "$1") asks for glibc $newest (at most 2.34):"; objdump -T "$1" | grep -E "GLIBC_2\.(3[5-9]|[4-9][0-9])" | awk '{print "  " $(NF-1), $NF}'
		return 1
	fi
}
needed() { readelf -d "$1" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'; }
glibc_ok "$B" || ok=0
if objdump -T "$B" | grep -q "GLIBCXX_\|CXXABI_"; then echo "needs the system's libstdc++ (link it in)"; ok=0; fi
# libgcc_s: only interfaces from GCC 4.8 (2013) and before
gcc_newest=$(objdump -T "$B" | grep -o "GCC_[0-9.]*" | sed 's/GCC_//' | sort -V | tail -1)
if [ -n "$gcc_newest" ] && [ "$(printf '%s\n4.8.0\n' "$gcc_newest" | sort -V | tail -1)" != "4.8.0" ]; then echo "asks libgcc_s for GCC_$gcc_newest"; ok=0; fi
for lib in $(needed "$B"); do
	case "$lib" in
		libSDL-1.2.so.0|libSDL_image-1.2.so.0|libSDL_mixer-1.2.so.0|libGL.so.1|libgcc_s.so.1|libm.so.6|libc.so.6|ld-linux-x86-64.so.2) ;;
		*) echo "needs $lib, which isn't the same everywhere"; ok=0 ;;
	esac
done
readelf -d "$B" | grep -q 'RPATH.*\[\$ORIGIN/../lib\]' || { echo "doesn't look for its libraries in ../lib (an RPATH of \$ORIGIN/../lib)"; ok=0; }
if strings "$B" | grep -q "$HOME"; then echo "has $HOME paths in it"; ok=0; fi
# The libraries it comes with: SDL2, sdl12-compat, SDL_image, SDL_mixer.
if [ -n "$L" ]; then
	for name in libSDL2-2.0.so.0 libSDL-1.2.so.0 libSDL_image-1.2.so.0 libSDL_mixer-1.2.so.0; do
		f="$L/$name"
		[ -f "$f" ] || { echo "lib/ lacks $name"; ok=0; continue; }
		glibc_ok "$f" || ok=0
		for lib in $(needed "$f"); do
			case "$lib" in
				libSDL-1.2.so.0|libm.so.6|libc.so.6) ;;
				*) echo "$name needs $lib, which it should open at start or carry"; ok=0 ;;
			esac
		done
		readelf -d "$f" | grep -q "RPATH\|RUNPATH" && { echo "$name has a library path of its own (it hides ../lib)"; ok=0; }
		strings "$f" | grep -q "$HOME" && { echo "$name has $HOME paths in it"; ok=0; }
	done
fi
[ $ok = 1 ] && echo "portable: glibc 2.34 or newer; needs $(needed "$B" | tr '\n' ' ')${L:+(SDL from lib/)}"
[ $ok = 1 ]
