#!/bin/sh
# Packages release/OpenXcom3D-<version>-linux-x86_64/ (and a .tar.gz without the local game links)
# and release/OpenXcom3D-<version>-x86_64.AppImage from the release build in OpenXcom/build, the
# libraries it comes with (tools/portable/build_libs.sh), the mod, the docs, install.py and
# openxcom3d.patch. Both run as they are: only the original games' data is needed.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# the version: the mod's (OpenXcom/mod/OpenXcom3D/metadata.yml, which the patch adds)
VERSION=$(sed -n 's/^version: *//p' "$ROOT/OpenXcom/mod/OpenXcom3D/metadata.yml")
[ -n "$VERSION" ] || { echo "no OpenXcom/mod/OpenXcom3D/metadata.yml: put OXCE into OpenXcom and apply openxcom3d.patch first"; exit 1; }
NAME=OpenXcom3D-$VERSION-linux-x86_64
R="$ROOT/release/$NAME"
B="$ROOT/OpenXcom/build/bin"
W="$ROOT/OpenXcom/build/portable"
[ -x "$B/openxcom" ] || { echo "build OpenXcom/build first"; exit 1; }
[ -f "$ROOT/openxcom3d.patch" ] || { echo "run tools/make_patch.sh first"; exit 1; }
# the release build must run on old and new Linux systems with only the libraries it comes with,
# and carry no home paths (tools/portable/build_libs.sh and configure.sh set it up; check.sh
# says what's wrong if they didn't)
sh "$ROOT/tools/portable/check.sh" "$B/openxcom" "$W/prefix/lib" || { echo "set up the release build with tools/portable/build_libs.sh and configure.sh"; exit 1; }
rm -rf "$R"
mkdir -p "$R/bin" "$R/lib" "$R/data" "$R/user/mods" "$R/licenses"
# without their symbol tables (the crash log only reads the exported symbols, which stay)
strip -o "$R/bin/openxcom" "$B/openxcom"
for lib in libSDL2-2.0.so.0 libSDL-1.2.so.0 libSDL_image-1.2.so.0 libSDL_mixer-1.2.so.0; do
	strip --strip-unneeded -o "$R/lib/$lib" "$W/prefix/lib/$lib"
done
# the libraries' licences (all allow this; the game itself is GPL 3, see LICENSE in the source)
for part in SDL2/LICENSE.txt sdl12-compat/LICENSE.txt SDL_image/COPYING SDL_mixer/COPYING SDL_mixer/timidity/COPYING \
	SDL_gfx/COPYING zlib/LICENSE libpng/LICENSE libogg/COPYING libvorbis/COPYING; do
	cp "$W/work/$part" "$R/licenses/$(echo "$part" | tr '/' '-')"
done
cat > "$R/licenses/README.txt" <<'TXT'
The libraries in lib/ (and linked into bin/openxcom), built from their unchanged sources by
tools/portable/build_libs.sh in the OpenXcom3D repository:

  SDL2 2.32.10          zlib licence        https://github.com/libsdl-org/SDL
  sdl12-compat 1.2.72   zlib licence        https://github.com/libsdl-org/sdl12-compat
  SDL_image 1.2.12      zlib licence        https://github.com/libsdl-org/SDL_image (SDL-1.2 branch)
  SDL_mixer 1.2.12      zlib licence        https://github.com/libsdl-org/SDL_mixer (SDL-1.2 branch)
                        (its TiMidity: Perl Artistic Licence)
  SDL_gfx 2.0.26        zlib licence        https://www.ferzkopp.net/wordpress/2016/01/02/sdl_gfx-sdl2_gfx/
  zlib 1.3.1            zlib licence        https://zlib.net
  libpng 1.6.47         libpng licence      https://github.com/pnggroup/libpng
  libogg 1.3.5          BSD 3-clause        https://xiph.org/ogg/
  libvorbis 1.3.7       BSD 3-clause        https://xiph.org/vorbis/
TXT
cp -r "$B/common" "$B/standard" "$R/data/"
cp -r "$ROOT/OpenXcom/mod/OpenXcom3D" "$R/user/mods/OpenXcom3D"
cat > "$R/user/options.cfg" <<'CFG'
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
# the starter script is the installer's (it finds the games' data and says plainly what's missing)
python3 -I -c 'import runpy, sys; sys.stdout.write(runpy.run_path(sys.argv[1])["PLAY_SH"])' "$ROOT/install.py" > "$R/play.sh"
chmod +x "$R/play.sh" "$R/bin/openxcom"
cp "$ROOT/install.py" "$ROOT/README.md" "$ROOT/OpenXcom3D_manual.md" "$ROOT/openxcom3d.patch" "$R/"
# the Steam Deck controller layout, and the script that makes Steam use it for OpenXcom3D
mkdir -p "$R/steamdeck"
cp "$ROOT/steamdeck/controller_neptune_openxcom3d.vdf" "$ROOT/steamdeck/add_controller_layout.sh" "$ROOT/steamdeck/use_layout.py" "$R/steamdeck/"
cp "$ROOT/OpenXcom/LICENSE.txt" "$R/LICENSE.txt"
# the tarball has no game data links; play.sh finds the games (or install.py links them). Its
# files are recorded as root's (owner 0/0, no names): tar would otherwise store this machine's
# user name in every header.
(cd "$ROOT/release" && tar --owner=0 --group=0 --numeric-owner -czf "$NAME.tar.gz" "$NAME")

# The AppImage: the same files, with play.sh as its AppRun (the profile goes to
# ~/.local/share/openxcom3d), made with a pinned appimagetool and runtime.
A="$W/appimage"
mkdir -p "$A"
fetch() { # file URL SHA-256
	[ -f "$A/$1" ] || curl -sSfL -o "$A/$1" "$2"
	echo "$3  $A/$1" | sha256sum -c --quiet - || { echo "$1: wrong SHA-256"; exit 1; }
}
fetch appimagetool-1.9.1-x86_64.AppImage https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
fetch runtime-20251108-x86_64 https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
chmod +x "$A/appimagetool-1.9.1-x86_64.AppImage"
D="$A/OpenXcom3D.AppDir"
rm -rf "$D"
mkdir -p "$D"
cp -r "$R/bin" "$R/lib" "$R/data" "$R/user" "$R/licenses" "$R/README.md" "$R/OpenXcom3D_manual.md" "$R/LICENSE.txt" "$D/"
cp "$R/play.sh" "$D/AppRun"
cp "$ROOT/OpenXcom/res/linux/icons/openxcom_128x128.png" "$D/openxcom3d.png"
cp "$D/openxcom3d.png" "$D/.DirIcon"
cat > "$D/openxcom3d.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=OpenXcom3D
Comment=X-COM with a first-person 3D battlescape (OpenXcom Extended + OpenXcom3D)
Exec=openxcom3d
Icon=openxcom3d
Terminal=false
Categories=Game;StrategyGame;
DESKTOP
rm -f "$ROOT/release/OpenXcom3D-$VERSION-x86_64.AppImage"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$A/appimagetool-1.9.1-x86_64.AppImage" --no-appstream --runtime-file "$A/runtime-20251108-x86_64" \
	"$D" "$ROOT/release/OpenXcom3D-$VERSION-x86_64.AppImage" > "$A/appimagetool.log" 2>&1 || { cat "$A/appimagetool.log"; exit 1; }

# the local folder links this machine's game data so it plays straight away
[ -d "$ROOT/UFO" ] && ln -s "$ROOT/UFO" "$R/user/UFO"
[ -d "$ROOT/TFTD" ] && ln -s "$ROOT/TFTD" "$R/user/TFTD"
echo "release: $R ($(du -sh "$R" | cut -f1)), tarball: $ROOT/release/$NAME.tar.gz ($(du -sh "$ROOT/release/$NAME.tar.gz" | cut -f1)),"
echo "AppImage: $ROOT/release/OpenXcom3D-$VERSION-x86_64.AppImage ($(du -sh "$ROOT/release/OpenXcom3D-$VERSION-x86_64.AppImage" | cut -f1))"
