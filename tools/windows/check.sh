#!/bin/sh
# Checks the Windows game and the two DLLs it comes with (see configure.sh and build_libs.sh): a
# 64-bit Windows program (no console window), loading nothing but Windows' own DLLs and SDL.dll
# (the C++ runtime and the codecs are linked in); SDL.dll needing only Windows (it opens SDL2.dll
# itself), SDL2.dll only Windows; and no home folder in any of them.
# Usage: check.sh EXE [DLLDIR]
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OBJDUMP="$ROOT/OpenXcom/build-win64/deps/toolchain/bin/x86_64-w64-mingw32-objdump"
E="$1"
L="$2"
[ -f "$E" ] || { echo "usage: check.sh EXE [DLLDIR]"; exit 1; }
ok=1
# The DLLs a file loads, one per line
needed() { "$OBJDUMP" -p "$1" | sed -n 's/^[[:space:]]*DLL Name: //p'; }
# Complains about each DLL a file loads that isn't in the list given (Windows' C runtime, the
# api-ms-win-crt-* DLLs, is always allowed): the file and the allowed names, any case.
only_needs() {
	f="$1"; shift
	for dll in $(needed "$f"); do
		lower=$(echo "$dll" | tr 'A-Z' 'a-z')
		case "$lower" in api-ms-win-crt-*) continue ;; esac
		echo " $* " | tr 'A-Z' 'a-z' | grep -q " $lower " || { echo "$(basename "$f") loads $dll, which isn't Windows' own (or link it in)"; ok=0; }
	done
}
# A file's home folder paths (the build's own, made relative by -ffile-prefix-map)
no_home() { if strings "$1" | grep -q "$HOME"; then echo "$(basename "$1") has $HOME paths in it"; ok=0; fi; }
"$OBJDUMP" -p "$E" | grep -q "Magic.*PE32+" || { echo "$(basename "$E") isn't 64-bit"; ok=0; }
"$OBJDUMP" -p "$E" | grep -q "Subsystem.*Windows GUI" || { echo "$(basename "$E") would open a console window"; ok=0; }
only_needs "$E" KERNEL32.dll USER32.dll GDI32.dll ADVAPI32.dll SHELL32.dll SHLWAPI.dll WININET.dll urlmon.dll \
	OPENGL32.dll imagehlp.dll dbghelp.dll WINMM.dll SDL.dll
no_home "$E"
if [ -n "$L" ]; then
	for dll in SDL.dll SDL2.dll; do
		[ -f "$L/$dll" ] || { echo "$L lacks $dll"; ok=0; continue; }
		no_home "$L/$dll"
	done
	only_needs "$L/SDL.dll" KERNEL32.dll USER32.dll
	only_needs "$L/SDL2.dll" KERNEL32.dll USER32.dll GDI32.dll WINMM.dll IMM32.dll ole32.dll OLEAUT32.dll VERSION.dll \
		ADVAPI32.dll SETUPAPI.dll SHELL32.dll
fi
[ $ok = 1 ] && echo "windows: 64-bit, loads $(needed "$E" | grep -vi '^api-ms-win-crt' | tr '\n' ' ')(and the C runtime)${L:+, SDL.dll and SDL2.dll next to it}"
[ $ok = 1 ]
