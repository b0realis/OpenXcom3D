#!/bin/sh
# Makes Steam use OpenXcom3D's Steam Deck controller layout for the OpenXcom3D shortcuts in your
# library (play.sh or the AppImage, added as non-Steam games): see use_layout.py. Steam rewrites
# its controller files while it runs, so it is closed first and started again after.
# Run it in Desktop Mode; run it again after adding another OpenXcom3D shortcut.
HERE="$(cd "$(dirname "$0")" && pwd)"
running=""
if pgrep -x steam > /dev/null 2>&1; then
	running=1
	echo "Closing Steam for a moment..."
	steam -shutdown > /dev/null 2>&1
	n=0
	while pgrep -x steam > /dev/null 2>&1 && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
	if pgrep -x steam > /dev/null 2>&1; then
		echo "Steam didn't close: close it yourself and run this again."
		exit 1
	fi
fi
python3 "$HERE/use_layout.py"
status=$?
if [ -n "$running" ]; then
	echo "Starting Steam again..."
	(setsid steam > /dev/null 2>&1 &)
fi
exit $status
