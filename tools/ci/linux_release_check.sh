#!/bin/sh
# Checks the Linux release on one distribution (the automatic builds run it in containers of
# several, from the oldest glibc the release supports, 2.34, to the newest):
# - the game loads with that system's own libraries and the ones it comes with, every symbol
#   bound at start (a missing glibc version shows at once), and says its version;
# - play.sh, with no game data anywhere, says where the data goes and stops;
# - the AppImage does the same, after putting the mod in its profile (~/.local/share/openxcom3d).
# There's no game data on these machines, so that's as far as the game gets.
# Usage: linux_release_check.sh OpenXcom3D-<version>-linux-x86_64.tar.gz OpenXcom3D-<version>-x86_64.AppImage
TARBALL="$1"
APPIMAGE="$2"
[ -f "$TARBALL" ] && [ -f "$APPIMAGE" ] || { echo "usage: linux_release_check.sh TARBALL APPIMAGE"; exit 1; }
failures=0
# One check's result (its exit status, what it checks), and a count of the failed ones
check() {
	if [ "$1" = 0 ]; then echo "ok    $2"; else echo "FAIL  $2"; failures=$((failures + 1)); fi
}
work=$(mktemp -d)
mkdir -p "$work/userhome"
echo "on $(. /etc/os-release && echo "$PRETTY_NAME"), $(ldd --version 2>&1 | head -1)"

# The tarball: the game itself, then its starter
tar xzf "$TARBALL" -C "$work"
R=$(ls -d "$work"/OpenXcom3D-*-linux-x86_64)
out=$(LD_BIND_NOW=1 "$R/bin/openxcom" -version 2>&1); code=$?
check $code "the game loads and says its version: $out"
out=$(HOME="$work/userhome" sh "$R/play.sh" 2>&1); code=$?
[ $code = 1 ] && echo "$out" | grep -q "needs the data of the original game"
check $? "play.sh without game data says where it goes (exit $code)"
[ $code = 1 ] || echo "$out"

# The AppImage (unpacked as it runs: containers have no FUSE)
cp "$APPIMAGE" "$work/OpenXcom3D.AppImage"
chmod +x "$work/OpenXcom3D.AppImage"
out=$(cd "$work" && HOME="$work/userhome" APPIMAGE_EXTRACT_AND_RUN=1 ./OpenXcom3D.AppImage 2>&1); code=$?
[ $code = 1 ] && echo "$out" | grep -q "needs the data of the original game"
check $? "the AppImage without game data says where it goes (exit $code)"
[ $code = 1 ] || echo "$out"
[ -f "$work/userhome/.local/share/openxcom3d/mods/OpenXcom3D/metadata.yml" ] && [ -f "$work/userhome/.local/share/openxcom3d/options.cfg" ]
check $? "the AppImage put the mod and the starting options in its profile"

[ $failures = 0 ] || { echo "$failures check(s) failed"; exit 1; }
echo "all checks passed"
