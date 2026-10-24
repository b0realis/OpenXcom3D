#!/bin/sh
# Puts OXCE into OpenXcom/ at exactly the commit OpenXcom3D is made for, with openxcom3d.patch
# applied: what every build here starts from (the automatic builds, and anyone building the
# releases by hand). The repository, branch and commit are install.py's, so they're written down
# in one place only. Only that one commit is fetched, which is quick.
# Usage: sh tools/ci/fetch_oxce.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# install.py's BASE_REPO, BASE_BRANCH and BASE_COMMIT
base() { sed -n "s/^$1 = \"\(.*\)\"$/\1/p" "$ROOT/install.py"; }
REPO=$(base BASE_REPO); BRANCH=$(base BASE_BRANCH); COMMIT=$(base BASE_COMMIT)
[ -n "$REPO" ] && [ -n "$COMMIT" ] || { echo "install.py has no BASE_REPO or BASE_COMMIT"; exit 1; }
[ ! -e "$ROOT/OpenXcom" ] || { echo "OpenXcom/ is there already: remove it first"; exit 1; }
echo "OXCE $BRANCH at $COMMIT, from $REPO"
git init -q "$ROOT/OpenXcom"
git -C "$ROOT/OpenXcom" remote add origin "$REPO"
git -C "$ROOT/OpenXcom" fetch -q --depth 1 origin "$COMMIT"
git -C "$ROOT/OpenXcom" checkout -q --detach FETCH_HEAD
# (Git for Windows would turn the patch's line ends into CRLF: keep them as they are)
git -C "$ROOT/OpenXcom" config core.autocrlf false
git -C "$ROOT/OpenXcom" apply "$ROOT/openxcom3d.patch"
echo "patch applied"
# The add-on's source and the mod in this repository are copies of what the patch adds, for
# reading on GitHub: they must say the same as the patch.
if [ -d "$ROOT/src/FirstPerson" ]; then
	diff -r "$ROOT/src/FirstPerson" "$ROOT/OpenXcom/src/FirstPerson" > /dev/null || { echo "src/FirstPerson differs from what the patch adds"; exit 1; }
	diff -r "$ROOT/mod/OpenXcom3D" "$ROOT/OpenXcom/mod/OpenXcom3D" > /dev/null || { echo "mod/OpenXcom3D differs from what the patch adds"; exit 1; }
	echo "src/FirstPerson and mod/OpenXcom3D match the patch"
fi
