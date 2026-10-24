OpenXcom3D for macOS (macOS 11 and newer, Apple silicon and Intel)

OpenXcom Extended with a first-person 3D battlescape. It needs the original game's data:
X-COM: UFO Defense (UFO: Enemy Unknown) and/or X-COM: Terror From The Deep.

This macOS build is new and made by GitHub's automatic builds: it hasn't been played on a
real Mac yet. Reports of how it runs are very welcome (see the address at the bottom).

Opening it the first time
  The app is signed ad hoc, not by Apple, so macOS stops it the first time ("can't be
  opened", or "Apple could not verify ..."). Either:
  - open it once, then go to System Settings > Privacy & Security and click "Open Anyway"
    next to OpenXcom3D; or
  - in Terminal:  xattr -dr com.apple.quarantine /path/to/OpenXcom3D.app
  After that it opens with a double-click like any other app.

The game's data
  The first time, it looks for the games in your Steam library and in apps named after
  them in Applications (GOG's), and links them into its profile (it changes nothing
  there). Otherwise copy the game's folder (the one with GEODATA, GEOGRAPH, MAPS and
  UFOGRAPH in it) to
    ~/Library/Application Support/OpenXcom3D/UFO   (or TFTD)
  or put it next to OpenXcom3D.app, named UFO (or TFTD). The app says so if it finds none.

In battle press F8 (or click the 3D button at the top left) for the first-person view. Every
key and setting is in OpenXcom3D_manual.md (a text file: any editor or browser shows it).

Your saves, the options and the log (openxcom.log) are in
  ~/Library/Application Support/OpenXcom3D
Source, news and reports: https://github.com/b0realis/OpenXcom3D
