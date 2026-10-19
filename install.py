#!/usr/bin/env python3
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
"""

import argparse
import multiprocessing
import os
import shutil
import subprocess
import sys
from pathlib import Path

VERSION = "0.7"
BASE_REPO = "https://github.com/MeridianOXC/OpenXcom.git"
BASE_BRANCH = "oxce-plus"
BASE_COMMIT = "f0134ef8065799c7723953d64b565f7832bfd782"
BASE_VERSION = "OpenXcom Extended 8.7.1 (v2026-09-19)"
PATCH_NAME = "openxcom3d.patch"
RELEASE_NAME = "OpenXcom3D-%s-linux-x86_64" % VERSION

HERE = Path(__file__).resolve().parent

APT_PACKAGES = ["git", "cmake", "g++", "make", "pkg-config", "libsdl1.2-dev", "libsdl-mixer1.2-dev",
                "libsdl-image1.2-dev", "libsdl-gfx1.2-dev", "zlib1g-dev", "libglu1-mesa-dev"]
PKGCONFIG_LIBS = ["sdl", "SDL_mixer", "SDL_image", "SDL_gfx", "zlib", "gl"]

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

PLAY_SH = """#!/bin/sh
# Starts OpenXcom3D with the data and the profile in this folder.
# Extra arguments are passed on, e.g.  ./play.sh -load mysave.sav
HERE="$(cd "$(dirname "$0")" && pwd)"
exec "$HERE/bin/openxcom" -data "$HERE/data/" -user "$HERE/user/" -cfg "$HERE/user/" "$@"
"""

DESKTOP = """[Desktop Entry]
Type=Application
Name=OpenXcom3D
Comment=X-COM with a first-person 3D battlescape (OpenXcom Extended + OpenXcom3D)
Exec=%(play)s
Path=%(dir)s
Terminal=false
Categories=Game;StrategyGame;
"""


def say(text=""):
    print(text, flush=True)


def fail(text):
    say("error: " + text)
    sys.exit(1)


def run(cmd, cwd=None):
    say("  $ " + " ".join(str(c) for c in cmd))
    result = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None)
    if result.returncode != 0:
        fail("command failed (exit %d): %s" % (result.returncode, " ".join(str(c) for c in cmd)))


def ask(question, default=None, assume_yes=False):
    if assume_yes:
        return default
    prompt = "%s [%s]: " % (question, default) if default else "%s: " % question
    try:
        answer = input(prompt).strip()
    except EOFError:
        answer = ""
    return answer or default


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
    """Works out (or asks for) the folder of one of the original games; None to skip it."""
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
    for folder in candidates:
        if looks_like_game(folder, kind):
            if given or assume_yes:
                return folder.resolve()
            answer = ask("Use %s data from %s? (y/n)" % (kind, folder), "y")
            if answer.lower().startswith("y"):
                return folder.resolve()
    if given:
        fail("%s doesn't look like the %s data folder (it needs GEODATA, GEOGRAPH, MAPS, UFOGRAPH)" % (given, kind))
    if assume_yes:
        return None
    while True:
        answer = ask("Folder with the original %s data (empty to skip)" % kind, "")
        if not answer:
            return None
        folder = Path(answer).expanduser()
        if looks_like_game(folder, kind):
            return folder.resolve()
        say("  that folder has no GEODATA/GEOGRAPH/MAPS/UFOGRAPH; try again")


def check_tools():
    """Reports what's missing to build from source; returns True if everything is there."""
    ok = True
    for tool in ["git", "cmake", "make", "c++", "pkg-config"]:
        if not shutil.which(tool):
            say("  missing: %s" % tool)
            ok = False
    if shutil.which("pkg-config"):
        for lib in PKGCONFIG_LIBS:
            if subprocess.run(["pkg-config", "--exists", lib]).returncode != 0:
                say("  missing library: %s" % lib)
                ok = False
    if not ok:
        say("\nOn Debian/Ubuntu these come with:\n  sudo apt install " + " ".join(APT_PACKAGES))
    else:
        say("  build tools and libraries: all present")
    return ok


def build_from_source(workdir, repo, jobs):
    """Clones OXCE at the base commit, applies the OpenXcom3D patch and builds it."""
    patch = HERE / PATCH_NAME
    if not patch.is_file():
        fail("%s not found next to this script" % PATCH_NAME)
    if not check_tools():
        fail("install the missing build tools first")
    src = workdir / "OpenXcom"
    workdir.mkdir(parents=True, exist_ok=True)
    if not src.exists():
        run(["git", "clone", "--branch", BASE_BRANCH, repo, src])
    run(["git", "-C", src, "fetch", "--quiet", "origin", BASE_COMMIT])
    run(["git", "-C", src, "checkout", "--quiet", "--detach", BASE_COMMIT])
    check = subprocess.run(["git", "-C", str(src), "apply", "--check", str(patch)])
    if check.returncode == 0:
        run(["git", "-C", src, "apply", patch])
    elif subprocess.run(["git", "-C", str(src), "apply", "--check", "--reverse", str(patch)]).returncode == 0:
        say("  the patch is already applied")
    else:
        fail("the patch doesn't apply to %s at %s" % (src, BASE_COMMIT))
    build = src / "build"
    run(["cmake", "-S", src, "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DDEV_BUILD=OFF", "-DBUILD_PACKAGE=OFF"])
    run(["cmake", "--build", build, "--", "-j%d" % jobs])
    return build / "bin", src / "mod" / "OpenXcom3D"


def find_release():
    """The prebuilt files: this folder if it is a release, else the release folder next to it."""
    for folder in [HERE, HERE / "release" / RELEASE_NAME]:
        if (folder / "bin" / "openxcom").is_file() and (folder / "data" / "standard").is_dir():
            mod = folder / "user" / "mods" / "OpenXcom3D"
            if mod.is_dir():
                return folder / "bin", folder / "data", mod
    return None


def install(target, binary_dir, data_dir, mod_dir, ufo, tftd, desktop):
    """Lays out bin/, data/, user/ (mod, links to the games, options) and play.sh in the target."""
    target.mkdir(parents=True, exist_ok=True)
    (target / "bin").mkdir(exist_ok=True)
    shutil.copy2(binary_dir / "openxcom", target / "bin" / "openxcom")
    os.chmod(target / "bin" / "openxcom", 0o755)
    for part in ["common", "standard"]:
        source = data_dir / part
        if not source.is_dir():
            fail("missing %s" % source)
        shutil.copytree(source, target / "data" / part, dirs_exist_ok=True)
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
    options = user / "options.cfg"
    if options.exists():
        say("  keeping the existing %s (enable OpenXcom3D in Mods if it isn't)" % options)
    else:
        options.write_text(OPTIONS_CFG % {"ufo": "true" if ufo or not tftd else "false",
                                          "tftd": "true" if tftd and not ufo else "false"})
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
    parser = argparse.ArgumentParser(description="Install OpenXcom3D %s (based on %s, %s@%s)" % (VERSION, BASE_VERSION, BASE_BRANCH, BASE_COMMIT[:10]))
    parser.add_argument("--target", help="install folder (default ~/Games/OpenXcom3D)")
    parser.add_argument("--ufo", help="folder with the original UFO: Enemy Unknown / UFO Defense data")
    parser.add_argument("--tftd", help="folder with the original Terror From The Deep data")
    parser.add_argument("--from-source", action="store_true", help="build from the OXCE base commit plus the OpenXcom3D patch")
    parser.add_argument("--workdir", help="where to clone and build with --from-source (default ~/.cache/openxcom3d-build)")
    parser.add_argument("--repo", default=BASE_REPO, help="OXCE repository to clone (default %(default)s)")
    parser.add_argument("--jobs", type=int, default=min(8, multiprocessing.cpu_count()), help="parallel build jobs")
    parser.add_argument("--desktop", action="store_true", help="also add a menu entry")
    parser.add_argument("--check", action="store_true", help="only check the build tools and libraries")
    parser.add_argument("--yes", action="store_true", help="don't ask, use the defaults and what was given")
    args = parser.parse_args()

    say("OpenXcom3D %s installer" % VERSION)
    say("  base: %s, branch %s, commit %s (%s)" % (BASE_REPO, BASE_BRANCH, BASE_COMMIT, BASE_VERSION))
    if not sys.platform.startswith("linux"):
        fail("this installer is for Linux")
    if args.check:
        sys.exit(0 if check_tools() else 1)

    target = Path(args.target or ask("Install folder", str(Path.home() / "Games" / "OpenXcom3D"), args.yes)).expanduser().resolve()
    ufo = find_game(args.ufo, "UFO", args.yes)
    tftd = find_game(args.tftd, "TFTD", args.yes)
    if not ufo and not tftd:
        fail("at least one of the original games is needed (--ufo or --tftd)")
    for name, folder in [("UFO", ufo), ("TFTD", tftd)]:
        if folder and (folder == target or target in folder.parents or folder in target.parents):
            fail("the install folder must be outside the %s data folder (%s), which is never written to" % (name, folder))

    if args.from_source:
        workdir = Path(args.workdir or Path.home() / ".cache" / "openxcom3d-build").expanduser().resolve()
        binary_dir, mod_dir = build_from_source(workdir, args.repo, max(1, args.jobs))
        data_dir = binary_dir
    else:
        found = find_release()
        if not found:
            fail("no prebuilt release found next to this script; use --from-source to build one")
        binary_dir, data_dir, mod_dir = found

    say("Installing into %s" % target)
    play = install(target, binary_dir, data_dir, mod_dir, ufo, tftd, args.desktop)
    say("")
    say("Done. Start the game with:\n  %s" % play)
    say("In battle press F8 (or click the 3D button at the top left) for the first-person view.")


if __name__ == "__main__":
    main()
