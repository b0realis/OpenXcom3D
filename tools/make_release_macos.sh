#!/bin/sh
# Packages release/OpenXcom3D-<version>-macos/ (OpenXcom3D.app with the docs next to it) and its
# .zip from the macOS build in OpenXcom/build-macos (tools/macos: build_libs.sh, configure.sh,
# then cmake --build there). The app runs as it is on macOS 11 and newer, Apple silicon and
# Intel: only the original games' data is needed. It's signed ad hoc, not by Apple: macOS asks
# once before opening it (README-macOS.txt says how).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# the version: the mod's (OpenXcom/mod/OpenXcom3D/metadata.yml, which the patch adds)
VERSION=$(sed -n 's/^version: *//p' "$ROOT/OpenXcom/mod/OpenXcom3D/metadata.yml")
[ -n "$VERSION" ] || { echo "no OpenXcom/mod/OpenXcom3D/metadata.yml: put OXCE into OpenXcom and apply openxcom3d.patch first"; exit 1; }
NAME=OpenXcom3D-$VERSION-macos
R="$ROOT/release/$NAME"
APP="$R/OpenXcom3D.app"
C="$APP/Contents"
B="$ROOT/OpenXcom/build-macos"
D="$B/deps"
[ -x "$B/bin/openxcom" ] || { echo "build OpenXcom/build-macos first (see tools/macos/configure.sh)"; exit 1; }
rm -rf "$R"
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources/data" "$C/Resources/mods" "$R/licenses"

# The game, and SDL 1.2 (sdl12-compat) and SDL2 next to each other in Frameworks: the game finds
# SDL 1.2 there by its library path, and sdl12-compat looks for SDL2 next to itself.
cp "$B/bin/openxcom" "$C/MacOS/openxcom"
cp "$D/prefix/lib/libSDL-1.2.0.dylib" "$D/prefix/lib/libSDL2-2.0.0.dylib" "$C/Frameworks/"
chmod 755 "$C/MacOS/openxcom" "$C"/Frameworks/*.dylib
install_name_tool -id @rpath/libSDL-1.2.0.dylib "$C/Frameworks/libSDL-1.2.0.dylib"
install_name_tool -id @rpath/libSDL2-2.0.0.dylib "$C/Frameworks/libSDL2-2.0.0.dylib"
built_as=$(otool -arch arm64 -L "$C/MacOS/openxcom" | awk '/libSDL-1\.2/ { print $1; exit }')
[ "$built_as" = "@rpath/libSDL-1.2.0.dylib" ] || install_name_tool -change "$built_as" @rpath/libSDL-1.2.0.dylib "$C/MacOS/openxcom"
# (the build's own library paths go, the app's Frameworks folder comes in)
for path in $(otool -l "$C/MacOS/openxcom" | awk '$1 == "cmd" && $2 == "LC_RPATH" { getline; getline; print $2 }' | sort -u); do
	install_name_tool -delete_rpath "$path" "$C/MacOS/openxcom"
done
install_name_tool -add_rpath @executable_path/../Frameworks "$C/MacOS/openxcom"

# The starter the app opens (it finds the games' data and hands the game its folders), OXCE's
# data, the mod, the starting options and the icon
cp "$ROOT/tools/macos/play.sh" "$C/MacOS/openxcom3d"
chmod 755 "$C/MacOS/openxcom3d"
cp -R "$B/bin/common" "$B/bin/standard" "$C/Resources/data/"
cp -R "$ROOT/OpenXcom/mod/OpenXcom3D" "$C/Resources/mods/OpenXcom3D"
cp "$ROOT/OpenXcom/res/mac/AppIcon.icns" "$C/Resources/AppIcon.icns"
cat > "$C/Resources/options.cfg" <<'CFG'
mods:
  - active: true
    id: xcom1
  - active: false
    id: xcom2
  - active: true
    id: OpenXcom3D
options:
  displayWidth: 1280
  displayHeight: 800
  fullscreen: false
  keepAspectRatio: true
  battlescapeScale: 4
  geoscapeScale: 4
  FPS: 60
CFG
cat > "$C/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleDisplayName</key><string>OpenXcom3D</string>
	<key>CFBundleExecutable</key><string>openxcom3d</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundleIdentifier</key><string>io.github.b0realis.openxcom3d</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>OpenXcom3D</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleVersion</key><string>$VERSION</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PLIST

# Signed ad hoc, the libraries first, then the game, then the app (Apple silicon runs nothing
# unsigned, and the paths changed above undid the linker's own signature)
codesign --force --sign - "$C"/Frameworks/*.dylib
codesign --force --sign - "$C/MacOS/openxcom"
codesign --force --sign - "$APP"
sh "$ROOT/tools/macos/check.sh" "$APP"

# Next to the app: how to open it and where the games' data goes, the docs and the licences
# (all allow this; the game itself is GPL 3, see LICENSE.txt)
cp "$ROOT/tools/macos/README-macOS.txt" "$ROOT/README.md" "$ROOT/OpenXcom3D_manual.md" "$ROOT/openxcom3d.patch" "$R/"
cp "$ROOT/OpenXcom/LICENSE.txt" "$R/LICENSE.txt"
for part in SDL2/LICENSE.txt sdl12-compat/LICENSE.txt SDL_image/COPYING SDL_mixer/COPYING SDL_mixer/timidity/COPYING \
	SDL_gfx/COPYING libpng/LICENSE libogg/COPYING libvorbis/COPYING; do
	cp "$D/work/$part" "$R/licenses/$(echo "$part" | tr '/' '-')"
done
cat > "$R/licenses/README.txt" <<'TXT'
The libraries in OpenXcom3D.app/Contents/Frameworks (libSDL2-2.0.0.dylib, libSDL-1.2.0.dylib) and
linked into OpenXcom3D.app/Contents/MacOS/openxcom, built from their unchanged sources by
tools/macos/build_libs.sh in the OpenXcom3D repository:

  SDL2 2.32.10          zlib licence        https://github.com/libsdl-org/SDL
  sdl12-compat 1.2.74   zlib licence        https://github.com/libsdl-org/sdl12-compat
  SDL_image 1.2.12      zlib licence        https://github.com/libsdl-org/SDL_image (SDL-1.2 branch)
  SDL_mixer 1.2.12      zlib licence        https://github.com/libsdl-org/SDL_mixer (SDL-1.2 branch)
                        (its TiMidity: Perl Artistic Licence)
  SDL_gfx 2.0.26        zlib licence        https://www.ferzkopp.net/wordpress/2016/01/02/sdl_gfx-sdl2_gfx/
  libpng 1.6.47         libpng licence      https://github.com/pnggroup/libpng
  libogg 1.3.5          BSD 3-clause        https://xiph.org/ogg/
  libvorbis 1.3.7       BSD 3-clause        https://xiph.org/vorbis/

zlib is macOS's own.
TXT
# the zip, made by ditto (it keeps the app's signature and its starter's permissions)
rm -f "$ROOT/release/$NAME.zip"
(cd "$ROOT/release" && ditto -c -k --keepParent "$NAME" "$NAME.zip")
echo "release: $R ($(du -sh "$R" | cut -f1)), zip: $ROOT/release/$NAME.zip ($(du -sh "$ROOT/release/$NAME.zip" | cut -f1))"
