#!/usr/bin/env python3
# ==============================================================================================
#  OpenXcom3D: a first-person 3D battlescape for OpenXcom Extended
# ==============================================================================================
#
#  Our philosophy
#
#  - Faithful first. The 3D view should feel as if it had shipped with UFO: Enemy Unknown on
#    the day the game came out.
#  - Another camera on the same battle. The original rules decide everything: time units,
#    energy, accuracy, line of fire, reaction fire, doors and morale. The 3D view only shows
#    what the game does.
#  - Only the original assets: the game's own maps, voxel shapes, sprites, palette and sounds.
#  - Every addition is a setting. Jumping, night vision as gear, falling, FPS-style shooting
#    and dual wield can all be switched off.
#  - Light on the engine. Our code lives in its own folder, the core only gets small hooks, and
#    without the OpenXcom3D mod nothing changes.
#  - Runs anywhere: a software renderer on the CPU, no graphics card needed.
#  - Written for people: descriptive names, and a comment for every function and every block.
#
#  Date: 2026-10-19
#
#  We wish everyone all the best and all health!
# ==============================================================================================
"""
OpenXcom3D installer (Linux).

OpenXcom3D is OpenXcom Extended (OXCE) with a first-person 3D battlescape add-on and its
data mod. This script installs it with your own copy of the original games' data:

  python3 install.py                       install the prebuilt release (asks where things are)
  python3 install.py --ufo ~/UFO --tftd ~/TFTD --target ~/Games/OpenXcom3D --yes
  python3 install.py --from-source         clone OXCE at the exact base commit, apply the
                                           OpenXcom3D patch, build it, then install
  python3 install.py --check               check the build tools and libraries only

The original game folders (UFO: Enemy Unknown / X-COM: UFO Defense, and Terror From The
Deep) are never modified: the installed profile only links to them.

An installation looks like this:

  <target>/bin/openxcom        the game
  <target>/lib/                the SDL libraries it comes with (the prebuilt release)
  <target>/data/               the engine's own data (common/, standard/)
  <target>/user/               the profile: options.cfg, the OpenXcom3D mod, links to the games
  <target>/play.sh             starts the game with that data and profile

The prebuilt release needs nothing installed: only glibc 2.34 or newer (2021 on) and the
system's own display, sound and OpenGL, which every desktop and the Steam Deck have.
"""

import argparse
import multiprocessing
import os
import shutil
import subprocess
import sys
from pathlib import Path

# ---- What is installed, and what it is built from ----

VERSION = "1.03"
# OpenXcom3D is a patch on one exact commit of OpenXcom Extended (OXCE).
BASE_REPO = "https://github.com/MeridianOXC/OpenXcom.git"
BASE_BRANCH = "oxce-plus"
BASE_COMMIT = "f0134ef8065799c7723953d64b565f7832bfd782"
BASE_VERSION = "OpenXcom Extended 8.7.1 (v2026-09-19)"
PATCH_NAME = "openxcom3d.patch"
RELEASE_NAME = "OpenXcom3D-%s-linux-x86_64" % VERSION

# The folder this script is in (a release, or a copy of the repository).
HERE = Path(__file__).resolve().parent

# ---- What building from source needs ----

# Debian / Ubuntu packages for the build tools and libraries, and the libraries pkg-config checks.
APT_PACKAGES = ["git", "cmake", "g++", "make", "pkg-config", "libsdl1.2-dev", "libsdl-mixer1.2-dev",
                "libsdl-image1.2-dev", "libsdl-gfx1.2-dev", "zlib1g-dev", "libglu1-mesa-dev"]
PKGCONFIG_LIBS = ["sdl", "SDL_mixer", "SDL_image", "SDL_gfx", "zlib", "gl"]

# How to get what the game needs, per family of Linux systems (told apart by /etc/os-release):
# to run the prebuilt game (it comes with its SDL libraries: only OpenGL could be missing, on a
# system without graphics drivers), and to build it from source.
FAMILY_NAMES = {"debian": "Debian, Ubuntu, Mint", "arch": "Arch, Manjaro", "fedora": "Fedora"}
RUN_PACKAGES = {
    "debian": "sudo apt install libgl1",
    "arch": "sudo pacman -S libglvnd",
    "fedora": "sudo dnf install libglvnd-glx",
}
BUILD_PACKAGES = {
    "debian": "sudo apt install " + " ".join(APT_PACKAGES),
    "arch": "sudo pacman -S --needed base-devel git cmake sdl12-compat sdl_image sdl_mixer sdl_gfx glu zlib",
    "fedora": "sudo dnf install git cmake gcc-c++ make pkgconf sdl12-compat-devel SDL_image-devel "
              "SDL_mixer-devel SDL_gfx-devel zlib-devel mesa-libGLU-devel",
}

# ---- Files written into the installation ----

# A fresh profile's options: the games found switched on (UFO first if both), the OpenXcom3D mod
# on, and a 1280x800 window.
OPTIONS_CFG = """mods:
  - active: %(ufo)s
    id: xcom1
  - active: %(tftd)s
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
"""

# The starter script: runs the game with this installation's data and profile, after saying
# plainly what the system lacks, if anything (the release's play.sh is this one too).
PLAY_SH = r"""#!/bin/sh
# Starts OpenXcom3D. Run from its folder, it plays with the data and the profile there (user/);
# run as the AppImage, its profile is in ~/.local/share/openxcom3d. The first time, it links the
# original games' data into the profile: a UFO or TFTD folder in this folder, next to it or in
# the home folder, or X-COM: UFO Defense and Terror From The Deep from a Steam library.
# Extra arguments are passed on, e.g.  ./play.sh -load mysave.sav
HERE="$(cd "$(dirname "$0")" && pwd)"

# Says something the player must know: on the terminal, and in a window when there's none
# (started from a menu, a file manager or Steam).
tell() {
	echo "$1"
	[ -t 1 ] && return
	zenity --error --no-wrap --title=OpenXcom3D --text="$1" 2>/dev/null ||
		kdialog --title OpenXcom3D --error "$1" 2>/dev/null || xmessage -center "$1" 2>/dev/null
}

# The profile, and where else to look for the games' data.
if [ -n "$APPIMAGE" ] && [ -n "$APPDIR" ]; then
	# The AppImage can't be written to: the profile is in the home folder. It gets the mod that
	# goes with this game every time, and the starting options the first time.
	HERE="$APPDIR"
	PROFILE="${XDG_DATA_HOME:-$HOME/.local/share}/openxcom3d"
	NEXT_TO="$(dirname "$APPIMAGE")"
	mkdir -p "$PROFILE/mods/OpenXcom3D"
	cp -R "$APPDIR/user/mods/OpenXcom3D/." "$PROFILE/mods/OpenXcom3D/"
	[ -f "$PROFILE/options.cfg" ] || cp "$APPDIR/user/options.cfg" "$PROFILE/options.cfg"
else
	PROFILE="$HERE/user"
	NEXT_TO="$(dirname "$HERE")"
fi

# What this system lacks to start the game, if anything (it comes with its SDL libraries).
missing=$(ldd "$HERE/bin/openxcom" 2>&1 | grep "not found" | sed 's/^[[:space:]]*//')
if [ -n "$missing" ]; then
	if echo "$missing" | grep -q GLIBC_; then
		tell "OpenXcom3D can't start: this system's C library is older than glibc 2.34 (2021).
Build the game here instead:  python3 install.py --from-source
($missing)"
	else
		tell "OpenXcom3D can't start: this system lacks
$missing
That's the OpenGL library, which comes with the graphics drivers:
  Debian, Ubuntu, Mint:  sudo apt install libgl1
  Arch, Manjaro:         sudo pacman -S libglvnd
  Fedora:                sudo dnf install libglvnd-glx"
	fi
	exit 1
fi

# Is this an original game's data folder? (Both games have these folders, in either case.)
is_game() {
	[ -d "$1" ] || return 1
	for part in GEODATA GEOGRAPH MAPS UFOGRAPH; do
		ls "$1" 2>/dev/null | grep -qix "$part" || return 1
	done
}

# The Steam libraries: Steam's own folder (installed natively, as a Flatpak or as a Snap) and
# the other ones it lists (another disk, the Steam Deck's SD card).
steam_libraries() {
	for steam in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/debian-installation" \
		"$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
		[ -d "$steam/steamapps" ] || continue
		echo "$steam"
		sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$steam/steamapps/libraryfolders.vdf" 2>/dev/null
	done
}

# Links one game's data into the profile unless it's there: the game's name (UFO or TFTD) and
# its folder in a Steam library. A link left pointing nowhere (the data moved) is replaced.
link_game() {
	[ -e "$PROFILE/$1" ] && return
	{
		echo "$HERE/$1"; echo "$NEXT_TO/$1"; echo "$HOME/$1"
		steam_libraries | while read -r library; do echo "$library/steamapps/common/$2"; done
	} | while read -r folder; do
		if is_game "$folder"; then
			ln -sfn "$folder" "$PROFILE/$1" && echo "OpenXcom3D: using the $1 data in $folder"
			break
		fi
	done
}
link_game UFO "XCom UFO Defense/XCOM"
link_game TFTD "X-COM Terror from the Deep/TFD"
if [ ! -e "$PROFILE/UFO" ] && [ ! -e "$PROFILE/TFTD" ]; then
	tell "OpenXcom3D needs the data of the original game: X-COM: UFO Defense (UFO: Enemy Unknown)
and/or X-COM: Terror From The Deep. Install it on Steam (it is then found by itself), or
copy the game's folder (the one with GEODATA, GEOGRAPH, MAPS and UFOGRAPH in it) to
  $PROFILE/UFO   (or TFTD)
and start OpenXcom3D again."
	exit 1
fi

# A profile the game hasn't written yet (only the starting options): switch on the game that's
# there (UFO if both), and play fullscreen on the Steam Deck.
if [ "$(wc -l < "$PROFILE/options.cfg")" -lt 25 ]; then
	if [ ! -e "$PROFILE/UFO" ]; then
		awk '{ line[NR] = $0 } /id: xcom1$/ { sub(/true/, "false", line[NR-1]) } /id: xcom2$/ { sub(/false/, "true", line[NR-1]) }
			END { for (i = 1; i <= NR; i++) print line[i] }' "$PROFILE/options.cfg" > "$PROFILE/options.cfg.new" &&
			mv "$PROFILE/options.cfg.new" "$PROFILE/options.cfg"
	fi
	if grep -qs '^ID=steamos' /etc/os-release; then
		sed 's/^  fullscreen: false/  fullscreen: true/' "$PROFILE/options.cfg" > "$PROFILE/options.cfg.new" &&
			mv "$PROFILE/options.cfg.new" "$PROFILE/options.cfg"
	fi
fi

exec "$HERE/bin/openxcom" -data "$HERE/data/" -user "$PROFILE/" -cfg "$PROFILE/" "$@"
"""

# A desktop menu entry (with --desktop).
DESKTOP = """[Desktop Entry]
Type=Application
Name=OpenXcom3D
Comment=X-COM with a first-person 3D battlescape (OpenXcom Extended + OpenXcom3D)
Exec=%(play)s
Path=%(dir)s
Terminal=false
Categories=Game;StrategyGame;
"""


# ---- Small helpers ----

def say(text=""):
    """Prints a line at once (so it shows up between the output of long commands)."""
    print(text, flush=True)


def fail(text):
    """Prints an error and stops the installer."""
    say("error: " + text)
    sys.exit(1)


def run(command, cwd=None):
    """Runs a command, showing it first; stops the installer if it fails."""
    say("  $ " + " ".join(str(part) for part in command))
    result = subprocess.run([str(part) for part in command], cwd=str(cwd) if cwd else None)
    if result.returncode != 0:
        fail("command failed (exit %d): %s" % (result.returncode, " ".join(str(part) for part in command)))


def ask(question, default=None, assume_yes=False):
    """Asks the user (Enter takes the default); with --yes, or without a terminal, takes the default."""
    if assume_yes:
        return default
    prompt = "%s [%s]: " % (question, default) if default else "%s: " % question
    try:
        answer = input(prompt).strip()
    except EOFError:
        answer = ""
    return answer or default


# ---- Finding the original games ----

def find_child(folder, name):
    """A child folder by name, ignoring case (the original data comes in either case)."""
    for child in folder.iterdir():
        if child.name.lower() == name.lower():
            return child
    return None


def looks_like_game(folder, kind):
    """Is this an original game data folder? Both games have GEODATA, GEOGRAPH, MAPS, UFOGRAPH."""
    if not folder or not folder.is_dir():
        return False
    needed = ["GEODATA", "GEOGRAPH", "MAPS", "UFOGRAPH"]
    return all(find_child(folder, name) is not None for name in needed)


def find_game(given, kind, assume_yes):
    """
    Works out (or asks for) the folder of one of the original games ("UFO" or "TFTD").
    Returns None to install without it.
    """
    # Where to look: the folder given on the command line, or the usual places (next to this
    # script, Steam's library, the home folder).
    candidates = []
    if given:
        candidates.append(Path(given).expanduser())
    else:
        home = Path.home()
        steam = home / ".local/share/Steam/steamapps/common"
        if kind == "UFO":
            candidates += [HERE / "UFO", HERE.parent / "UFO", steam / "XCom UFO Defense/XCOM", home / "UFO"]
        else:
            candidates += [HERE / "TFTD", HERE.parent / "TFTD", steam / "X-COM Terror from the Deep/TFD", home / "TFTD"]

    # The first one that holds the game's data, confirmed by the user (unless given or --yes).
    for folder in candidates:
        if looks_like_game(folder, kind):
            if given or assume_yes:
                return folder.resolve()
            answer = ask("Use %s data from %s? (y/n)" % (kind, folder), "y")
            if answer.lower().startswith("y"):
                return folder.resolve()

    # A folder given on the command line must be right; with --yes, a game not found is skipped.
    if given:
        fail("%s doesn't look like the %s data folder (it needs GEODATA, GEOGRAPH, MAPS, UFOGRAPH)" % (given, kind))
    if assume_yes:
        return None

    # Otherwise ask until we get a good folder, or nothing (skip this game).
    while True:
        answer = ask("Folder with the original %s data (empty to skip)" % kind, "")
        if not answer:
            return None
        folder = Path(answer).expanduser()
        if looks_like_game(folder, kind):
            return folder.resolve()
        say("  that folder has no GEODATA/GEOGRAPH/MAPS/UFOGRAPH; try again")


def distro_family():
    """This system's family of Linux systems (debian, arch or fedora), or None if it's another."""
    try:
        with open("/etc/os-release") as release:
            fields = dict(line.rstrip("\n").split("=", 1) for line in release if "=" in line)
    except OSError:
        return None
    names = (fields.get("ID", "") + " " + fields.get("ID_LIKE", "")).replace('"', "").split()
    for family, members in [("debian", ["debian", "ubuntu"]), ("arch", ["arch"]), ("fedora", ["fedora", "rhel", "centos"])]:
        if any(name in members for name in names):
            return family
    return None


def show_packages(commands):
    """Says how to install what's missing: this system's command, or every family's."""
    family = distro_family()
    if family:
        say("  " + commands[family])
    else:
        for family, command in commands.items():
            say("  %-22s %s" % (FAMILY_NAMES[family] + ":", command))


# ---- Getting the game: built from source, or the prebuilt release ----

def check_runtime(binary):
    """
    Checks that this system can run the prebuilt game, and says what's missing and how to get
    it. Returns True if nothing is missing. (It comes with its SDL libraries in lib/: only an old
    glibc or a missing OpenGL library can stop it.)
    """
    problems = []
    old_glibc = False
    # The libraries the game asks for when it starts (ldd says which aren't there).
    result = subprocess.run(["ldd", str(binary)], capture_output=True, text=True)
    for line in (result.stdout + result.stderr).splitlines():
        if "not found" in line:
            problems.append(line.strip())
            old_glibc = old_glibc or "GLIBC_" in line
    if not problems:
        return True
    say("  this system lacks what the game needs to run:")
    for problem in problems:
        say("    " + problem)
    if old_glibc:
        say("  its C library is older than glibc 2.34: build the game here instead, with --from-source")
    else:
        say("  that's the OpenGL library (it comes with the graphics drivers); install it with:")
        show_packages(RUN_PACKAGES)
    return False


def check_tools():
    """Reports what's missing to build from source; returns True if everything is there."""
    all_present = True
    # The programs.
    for tool in ["git", "cmake", "make", "c++", "pkg-config"]:
        if not shutil.which(tool):
            say("  missing: %s" % tool)
            all_present = False
    # The libraries (as far as pkg-config can tell).
    if shutil.which("pkg-config"):
        for library in PKGCONFIG_LIBS:
            if subprocess.run(["pkg-config", "--exists", library]).returncode != 0:
                say("  missing library: %s" % library)
                all_present = False
    # How to get what's missing.
    if not all_present:
        say("\nInstall them with:")
        show_packages(BUILD_PACKAGES)
    else:
        say("  build tools and libraries: all present")
    return all_present


def build_from_source(workdir, repo, jobs):
    """
    Clones OXCE at the base commit, applies the OpenXcom3D patch and builds it.
    Returns the folder with the built game (and its data), and the OpenXcom3D mod folder.
    """
    patch = HERE / PATCH_NAME
    if not patch.is_file():
        fail("%s not found next to this script" % PATCH_NAME)
    if not check_tools():
        fail("install the missing build tools first")

    # The source at exactly the base commit (a clone made earlier is reused).
    source = workdir / "OpenXcom"
    workdir.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        run(["git", "clone", "--branch", BASE_BRANCH, repo, source])
    run(["git", "-C", source, "fetch", "--quiet", "origin", BASE_COMMIT])
    run(["git", "-C", source, "checkout", "--quiet", "--detach", BASE_COMMIT])

    # The patch on top (a source tree that already has it, from an earlier run, is fine).
    check = subprocess.run(["git", "-C", str(source), "apply", "--check", str(patch)])
    if check.returncode == 0:
        run(["git", "-C", source, "apply", patch])
    elif subprocess.run(["git", "-C", str(source), "apply", "--check", "--reverse", str(patch)]).returncode == 0:
        say("  the patch is already applied")
    else:
        fail("the patch doesn't apply to %s at %s" % (source, BASE_COMMIT))

    # A release build.
    build = source / "build"
    run(["cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DDEV_BUILD=OFF", "-DBUILD_PACKAGE=OFF"])
    run(["cmake", "--build", build, "--", "-j%d" % jobs])
    return build / "bin", source / "mod" / "OpenXcom3D"


def find_release():
    """
    The prebuilt files: this folder if it is a release, else the release folder next to it.
    Returns the folders of the game, its data and the OpenXcom3D mod, or None if there's none
    (the game's libraries are in lib/ next to bin/).
    """
    for folder in [HERE, HERE / "release" / RELEASE_NAME]:
        if (folder / "bin" / "openxcom").is_file() and (folder / "data" / "standard").is_dir():
            mod = folder / "user" / "mods" / "OpenXcom3D"
            if mod.is_dir():
                return folder / "bin", folder / "data", mod
    return None


# ---- Installing ----

def install(target, binary_dir, data_dir, mod_dir, ufo, tftd, desktop):
    """Lays out bin/, data/, user/ (mod, links to the games, options) and play.sh in the target."""
    # The game itself, and the libraries the prebuilt one comes with (lib/ next to bin/; a game
    # built from source uses the system's).
    target.mkdir(parents=True, exist_ok=True)
    (target / "bin").mkdir(exist_ok=True)
    shutil.copy2(binary_dir / "openxcom", target / "bin" / "openxcom")
    os.chmod(target / "bin" / "openxcom", 0o755)
    if (binary_dir.parent / "lib" / "libSDL2-2.0.so.0").exists():
        shutil.copytree(binary_dir.parent / "lib", target / "lib", dirs_exist_ok=True)

    # The engine's own data.
    for part in ["common", "standard"]:
        data_source = data_dir / part
        if not data_source.is_dir():
            fail("missing %s" % data_source)
        shutil.copytree(data_source, target / "data" / part, dirs_exist_ok=True)

    # The profile: the OpenXcom3D mod, and links to the original games (never copies, so their
    # folders are never written to). A real folder already there is kept.
    user = target / "user"
    (user / "mods").mkdir(parents=True, exist_ok=True)
    shutil.copytree(mod_dir, user / "mods" / "OpenXcom3D", dirs_exist_ok=True)
    for name, folder in [("UFO", ufo), ("TFTD", tftd)]:
        link = user / name
        if folder is None:
            continue
        if link.is_symlink() or link.exists():
            if link.is_symlink():
                link.unlink()
            else:
                say("  keeping the existing %s (not a link)" % link)
                continue
        link.symlink_to(folder, target_is_directory=True)
        say("  linked %s -> %s" % (link, folder))

    # The options, only for a new profile (an existing one keeps the player's settings).
    options = user / "options.cfg"
    if options.exists():
        say("  keeping the existing %s (enable OpenXcom3D in Mods if it isn't)" % options)
    else:
        options.write_text(OPTIONS_CFG % {"ufo": "true" if ufo or not tftd else "false",
                                          "tftd": "true" if tftd and not ufo else "false"})

    # The starter script, the documentation, and (if asked) a menu entry.
    play = target / "play.sh"
    play.write_text(PLAY_SH)
    os.chmod(play, 0o755)
    for doc in ["README.md", "OpenXcom3D_manual.md"]:
        if (HERE / doc).is_file() and (HERE / doc).resolve() != (target / doc).resolve():
            shutil.copy2(HERE / doc, target / doc)
    if desktop:
        apps = Path.home() / ".local/share/applications"
        apps.mkdir(parents=True, exist_ok=True)
        (apps / "openxcom3d.desktop").write_text(DESKTOP % {"play": play, "dir": target})
        say("  menu entry: %s" % (apps / "openxcom3d.desktop"))
    return play


def main():
    """Reads the command line, finds the games and the game (prebuilt or built), and installs."""
    # The command line.
    parser = argparse.ArgumentParser(description="Install OpenXcom3D %s (based on %s, %s@%s)" % (VERSION, BASE_VERSION, BASE_BRANCH, BASE_COMMIT[:10]))
    parser.add_argument("--target", help="install folder (default ~/Games/OpenXcom3D)")
    parser.add_argument("--ufo", help="folder with the original UFO: Enemy Unknown / UFO Defense data")
    parser.add_argument("--tftd", help="folder with the original Terror From The Deep data")
    parser.add_argument("--from-source", action="store_true", help="build from the OXCE base commit plus the OpenXcom3D patch")
    parser.add_argument("--workdir", help="where to clone and build with --from-source (default ~/.cache/openxcom3d-build)")
    parser.add_argument("--repo", default=BASE_REPO, help="OXCE repository to clone (default %(default)s)")
    parser.add_argument("--jobs", type=int, default=min(8, multiprocessing.cpu_count()), help="parallel build jobs")
    parser.add_argument("--desktop", action="store_true", help="also add a menu entry")
    parser.add_argument("--check", action="store_true", help="only check the build tools and libraries (and what the prebuilt game needs)")
    parser.add_argument("--yes", action="store_true", help="don't ask, use the defaults and what was given")
    args = parser.parse_args()

    say("OpenXcom3D %s installer" % VERSION)
    say("  base: %s, branch %s, commit %s (%s)" % (BASE_REPO, BASE_BRANCH, BASE_COMMIT, BASE_VERSION))
    if not sys.platform.startswith("linux"):
        fail("this installer is for Linux")
    if args.check:
        # The build tools, and if this is a release, what its prebuilt game needs to run.
        found = find_release()
        runnable = check_runtime(found[0] / "openxcom") if found else True
        sys.exit(0 if check_tools() and runnable else 1)

    # Where to install, and the original games (at least one; neither may contain the install
    # folder, nor sit inside it, since their folders are never written to).
    target = Path(args.target or ask("Install folder", str(Path.home() / "Games" / "OpenXcom3D"), args.yes)).expanduser().resolve()
    ufo = find_game(args.ufo, "UFO", args.yes)
    tftd = find_game(args.tftd, "TFTD", args.yes)
    if not ufo and not tftd:
        fail("at least one of the original games is needed (--ufo or --tftd)")
    for name, folder in [("UFO", ufo), ("TFTD", tftd)]:
        if folder and (folder == target or target in folder.parents or folder in target.parents):
            fail("the install folder must be outside the %s data folder (%s), which is never written to" % (name, folder))

    # The game: built from source, or the prebuilt release next to this script.
    if args.from_source:
        workdir = Path(args.workdir or Path.home() / ".cache" / "openxcom3d-build").expanduser().resolve()
        binary_dir, mod_dir = build_from_source(workdir, args.repo, max(1, args.jobs))
        data_dir = binary_dir
    else:
        found = find_release()
        if not found:
            fail("no prebuilt release found next to this script; use --from-source to build one")
        binary_dir, data_dir, mod_dir = found

    # Install it, and say how to start.
    say("Installing into %s" % target)
    play = install(target, binary_dir, data_dir, mod_dir, ufo, tftd, args.desktop)
    runnable = args.from_source or check_runtime(target / "bin" / "openxcom")
    say("")
    if not runnable:
        say("Installed, but install what's missing (above) before playing.")
    say("Done. Start the game with:\n  %s" % play)
    say("In battle press F8 (or click the 3D button at the top left) for the first-person view.")


if __name__ == "__main__":
    main()

# Thanks mom and dad! You were the ones who exposed me to my first comp.
