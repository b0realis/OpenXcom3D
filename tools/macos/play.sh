#!/bin/sh
# Starts OpenXcom3D on macOS: the app's own starter (OpenXcom3D.app/Contents/MacOS/openxcom3d;
# make_release_macos.sh puts it there). The profile (saves, options, the log) is in
# ~/Library/Application Support/OpenXcom3D: it gets the mod that goes with this game every time,
# and the starting options the first time. The first time, it also links the original games'
# data into the profile: a UFO or TFTD folder there, next to the app or in the home folder, or
# X-COM: UFO Defense and Terror From The Deep from a Steam library or a GOG app.
# Extra arguments are passed on, e.g.  .../openxcom3d -load mysave.sav
CONTENTS="$(cd "$(dirname "$0")/.." && pwd)"
APP="$(dirname "$CONTENTS")"
NEXT_TO="$(dirname "$APP")"
PROFILE="$HOME/Library/Application Support/OpenXcom3D"

# Says something the player must know: on the terminal, and in a window when there's none
# (started from the Finder or the Dock). OPENXCOM3D_NO_DIALOG=1 keeps it to the terminal (the
# automatic builds' check).
tell() {
	echo "$1"
	[ -t 1 ] && return
	[ -n "$OPENXCOM3D_NO_DIALOG" ] && return
	osascript - "$1" > /dev/null 2>&1 <<'APPLESCRIPT'
on run argv
	display alert "OpenXcom3D" message (item 1 of argv) as critical
end run
APPLESCRIPT
}

# The profile: the mod that goes with this game, and the starting options the first time
mkdir -p "$PROFILE/mods/OpenXcom3D"
cp -R "$CONTENTS/Resources/mods/OpenXcom3D/." "$PROFILE/mods/OpenXcom3D/"
[ -f "$PROFILE/options.cfg" ] || cp "$CONTENTS/Resources/options.cfg" "$PROFILE/options.cfg"

# Is this an original game's data folder? (Both games have these folders, in either case.)
is_game() {
	[ -d "$1" ] || return 1
	for part in GEODATA GEOGRAPH MAPS UFOGRAPH; do
		ls "$1" 2>/dev/null | grep -qix "$part" || return 1
	done
}

# The Steam libraries: Steam's own folder and the other ones it lists (another disk).
steam_libraries() {
	steam="$HOME/Library/Application Support/Steam"
	[ -d "$steam/steamapps" ] || return
	echo "$steam"
	sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$steam/steamapps/libraryfolders.vdf" 2>/dev/null
}

# A game's data folder anywhere inside a folder (an app made for macOS keeps the DOS game
# inside itself, a few folders down): the first folder in it that is one, if any.
data_inside() {
	[ -d "$1" ] || return
	find "$1" -maxdepth 7 -type d -iname GEODATA 2>/dev/null | while read -r found; do
		if is_game "$(dirname "$found")"; then dirname "$found"; break; fi
	done
}

# Links one game's data into the profile unless it's there: the game's name (UFO or TFTD), its
# folder in a Steam library, and a word in the name of its macOS app (GOG's, say). A link left
# pointing nowhere (the data moved) is replaced.
link_game() {
	is_game "$PROFILE/$1" && return
	[ -L "$PROFILE/$1" ] && rm "$PROFILE/$1"
	[ -e "$PROFILE/$1" ] && return
	{
		echo "$NEXT_TO/$1"; echo "$HOME/$1"
		steam_libraries | while read -r library; do
			echo "$library/steamapps/common/$2"
			data_inside "$library/steamapps/common/$(dirname "$2")"
		done
		for apps in /Applications "$HOME/Applications"; do
			for app in "$apps"/*"$3"*.app; do data_inside "$app"; done
		done
	} | while read -r folder; do
		if is_game "$folder"; then
			ln -sfn "$folder" "$PROFILE/$1" && echo "OpenXcom3D: using the $1 data in $folder"
			break
		fi
	done
}
link_game UFO "XCom UFO Defense/XCOM" "UFO"
link_game TFTD "X-COM Terror from the Deep/TFD" "Terror"
if ! is_game "$PROFILE/UFO" && ! is_game "$PROFILE/TFTD"; then
	tell "OpenXcom3D needs the data of the original game: X-COM: UFO Defense (UFO: Enemy Unknown) and/or X-COM: Terror From The Deep.

Copy the game's folder (the one with GEODATA, GEOGRAPH, MAPS and UFOGRAPH in it) to
  $PROFILE/UFO   (or TFTD)
or put it next to OpenXcom3D.app as UFO (or TFTD), and start OpenXcom3D again."
	exit 1
fi

# A profile the game hasn't written yet (only the starting options): switch on the game that's
# there (UFO if both).
if [ "$(wc -l < "$PROFILE/options.cfg")" -lt 25 ] && ! is_game "$PROFILE/UFO"; then
	awk '{ line[NR] = $0 } /id: xcom1$/ { sub(/true/, "false", line[NR-1]) } /id: xcom2$/ { sub(/false/, "true", line[NR-1]) }
		END { for (i = 1; i <= NR; i++) print line[i] }' "$PROFILE/options.cfg" > "$PROFILE/options.cfg.new" &&
		mv "$PROFILE/options.cfg.new" "$PROFILE/options.cfg"
fi

exec "$CONTENTS/MacOS/openxcom" -data "$CONTENTS/Resources/data/" -user "$PROFILE/" -cfg "$PROFILE/" "$@"
