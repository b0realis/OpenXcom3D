#!/bin/sh
# Checks that the macOS app runs on any Mac from macOS 11 on, Apple silicon and Intel alike,
# with nothing installed: the game and its two libraries hold both kinds of Mac, ask for no newer
# macOS, need only the system's libraries and the ones inside the app (which find each other
# there), carry no home folder paths, and the app is signed (ad hoc: Apple silicon runs nothing
# unsigned).
# Usage: check.sh OpenXcom3D.app
APP="$1"
C="$APP/Contents"
[ -x "$C/MacOS/openxcom" ] || { echo "usage: check.sh OpenXcom3D.app"; exit 1; }
ok=1
for f in "$C/MacOS/openxcom" "$C/Frameworks/libSDL-1.2.0.dylib" "$C/Frameworks/libSDL2-2.0.0.dylib"; do
	name=$(basename "$f")
	[ -f "$f" ] || { echo "the app lacks $name"; ok=0; continue; }
	# both kinds of Mac, and macOS 11 at most for each
	for arch in arm64 x86_64; do
		lipo "$f" -verify_arch "$arch" 2>/dev/null || { echo "$name has no $arch part"; ok=0; continue; }
		minos=$(vtool -arch "$arch" -show-build "$f" 2>/dev/null | awk '$1 == "minos" { print $2 }')
		[ -n "$minos" ] || minos=$(otool -arch "$arch" -l "$f" | awk '$1 == "version" && seen { print $2; exit } /LC_VERSION_MIN_MACOSX/ { seen = 1 }')
		if [ -z "$minos" ] || [ "$(echo "$minos" | awk -F. '{ print ($1 > 11 || ($1 == 11 && $2 > 0)) }')" = 1 ]; then
			echo "$name ($arch) asks for macOS ${minos:-?} (at most 11.0)"; ok=0
		fi
		# what it loads: the system's, or a library inside the app
		for dep in $(otool -arch "$arch" -L "$f" | tail -n +2 | awk '{ print $1 }'); do
			case "$dep" in
				/usr/lib/*|/System/Library/*) ;;
				@rpath/libSDL-1.2.0.dylib|@rpath/libSDL2-2.0.0.dylib) [ -f "$C/Frameworks/${dep#@rpath/}" ] || { echo "$name needs $dep, which the app lacks"; ok=0; } ;;
				*) echo "$name ($arch) needs $dep, which isn't on every Mac"; ok=0 ;;
			esac
		done
	done
	if strings "$f" | grep -q "$HOME"; then echo "$name has $HOME paths in it"; ok=0; fi
done
# the game finds the libraries inside the app, and only there
rpaths=$(otool -l "$C/MacOS/openxcom" | awk '$1 == "cmd" && $2 == "LC_RPATH" { getline; getline; print $2 }' | sort -u)
[ "$rpaths" = "@executable_path/../Frameworks" ] || { echo "the game's library path is not just @executable_path/../Frameworks: $rpaths"; ok=0; }
# the starter the app opens, and the data, mod and options it hands the game
exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$C/Info.plist" 2>/dev/null)
[ -x "$C/MacOS/$exe" ] || { echo "Info.plist names a starter the app lacks: $exe"; ok=0; }
for part in Resources/data/common Resources/data/standard Resources/mods/OpenXcom3D/metadata.yml Resources/options.cfg; do
	[ -e "$C/$part" ] || { echo "the app lacks $part"; ok=0; }
done
codesign --verify --strict "$APP" 2>/dev/null || { echo "the app's signature doesn't check out"; ok=0; }
[ $ok = 1 ] && echo "macOS app: arm64 and x86_64, macOS 11 or newer, SDL inside, signed ad hoc"
[ $ok = 1 ]
