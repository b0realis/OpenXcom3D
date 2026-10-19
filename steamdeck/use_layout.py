#!/usr/bin/env python3
"""
Makes Steam use OpenXcom3D's Steam Deck controller layout (controller_neptune_openxcom3d.vdf)
for the OpenXcom3D shortcuts in your Steam library (play.sh in an OpenXcom3D folder, or the
OpenXcom3D AppImage, added as non-Steam games):

1. puts the layout among Steam's controller templates (controller_base/templates);
2. finds those shortcuts in each Steam user's shortcuts.vdf;
3. sets the template for them in that user's configset_controller_neptune.vdf (where Steam keeps
   which layout each game uses on the Steam Deck), after saving a copy of the file.

Steam rewrites these files while it runs: add_controller_layout.sh closes it first and starts it
again after. Run it again after adding another OpenXcom3D shortcut.
"""
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT = "controller_neptune_openxcom3d.vdf"
HOME = os.path.expanduser("~")
# Steam's folders: installed natively (two names for it) or as a Flatpak.
STEAM_ROOTS = [os.path.join(HOME, ".local/share/Steam"), os.path.join(HOME, ".steam/steam"),
               os.path.join(HOME, ".var/app/com.valvesoftware.Steam/.local/share/Steam")]


def read_binary_vdf(data, at=0):
    """Reads a binary KeyValues map (shortcuts.vdf) from data at position at: (dict, next position)."""
    result = {}
    while True:
        kind = data[at]
        at += 1
        if kind == 8:  # end of this map
            return result, at
        end = data.index(b"\0", at)
        key = data[at:end].decode("utf-8", "replace")
        at = end + 1
        if kind == 0:  # a map
            value, at = read_binary_vdf(data, at)
        elif kind == 1:  # a string
            end = data.index(b"\0", at)
            value = data[at:end].decode("utf-8", "replace")
            at = end + 1
        elif kind == 2:  # a 32-bit number
            value = struct.unpack("<i", data[at:at + 4])[0]
            at += 4
        else:
            raise ValueError("unknown binary VDF type %d" % kind)
        result[key] = value


def read_text_vdf(text):
    """Reads a text KeyValues file (configset_*.vdf) into nested dicts, in order."""
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text)
    stack = [{}]
    key = None
    for quoted, brace in tokens:
        if brace == "{":
            child = {}
            stack[-1][key] = child
            stack.append(child)
            key = None
        elif brace == "}":
            stack.pop()
        elif key is None:
            key = quoted
        else:
            stack[-1][key] = quoted
            key = None
    return stack[0]


def write_text_vdf(tree, depth=0):
    """Writes nested dicts as a text KeyValues file, laid out as Steam writes it (tabs)."""
    lines = []
    tabs = "\t" * depth
    for key, value in tree.items():
        if isinstance(value, dict):
            lines.append('%s"%s"\n%s{\n%s%s}\n' % (tabs, key, tabs, write_text_vdf(value, depth + 1), tabs))
        else:
            lines.append('%s"%s"\t\t"%s"\n' % (tabs, key, value))
    return "".join(lines)


def configset_keys(name):
    """The names Steam may file a non-Steam game's layout under: its shortcut name in lower case,
    with dots dropped (run.sh is filed as runsh), and with all punctuation but - and _ dropped."""
    lower = name.lower()
    return list(dict.fromkeys([lower, lower.replace(".", ""), re.sub(r"[^a-z0-9 _-]", "", lower)]))


def is_openxcom3d(shortcut):
    """Is this non-Steam shortcut OpenXcom3D: play.sh in an OpenXcom3D folder, or the AppImage?"""
    exe = (shortcut.get("Exe") or shortcut.get("exe") or "").strip('"')
    name = shortcut.get("AppName") or shortcut.get("appname") or ""
    if "openxcom3d" in name.lower():
        return True
    folder, file = os.path.split(exe)
    return "openxcom3d" in exe.lower() and (file == "play.sh" or file.lower().endswith(".appimage"))


def main():
    roots = [root for root in STEAM_ROOTS if os.path.isdir(os.path.join(root, "controller_base"))]
    seen = set()
    roots = [root for root in roots if not (os.path.realpath(root) in seen or seen.add(os.path.realpath(root)))]
    if not roots:
        print("Steam wasn't found for this user.")
        return 1
    found_any = False
    for root in roots:
        # 1. The layout among Steam's templates.
        templates = os.path.join(root, "controller_base", "templates")
        os.makedirs(templates, exist_ok=True)
        shutil.copy(os.path.join(HERE, LAYOUT), os.path.join(templates, LAYOUT))
        print("Layout added to %s" % templates)

        # 2. and 3. Each Steam user's OpenXcom3D shortcuts, set to use it.
        userdata = os.path.join(root, "userdata")
        for user in sorted(os.listdir(userdata)) if os.path.isdir(userdata) else []:
            shortcuts_file = os.path.join(userdata, user, "config", "shortcuts.vdf")
            if not os.path.isfile(shortcuts_file):
                continue
            tree, _ = read_binary_vdf(open(shortcuts_file, "rb").read())
            shortcuts = [entry for entry in tree.get("shortcuts", {}).values() if isinstance(entry, dict) and is_openxcom3d(entry)]
            if not shortcuts:
                continue
            config_folder = os.path.join(root, "steamapps", "common", "Steam Controller Configs", user, "config")
            os.makedirs(config_folder, exist_ok=True)
            configset_file = os.path.join(config_folder, "configset_controller_neptune.vdf")
            configset = {"controller_config": {}}
            if os.path.isfile(configset_file):
                configset = read_text_vdf(open(configset_file, encoding="utf-8").read()) or configset
                shutil.copy(configset_file, configset_file + ".before-openxcom3d")
            games = configset.setdefault("controller_config", {})
            for shortcut in shortcuts:
                name = shortcut.get("AppName") or shortcut.get("appname") or ""
                for key in configset_keys(name):
                    games[key] = {"template": LAYOUT}
                print("Steam user %s: '%s' uses the OpenXcom3D layout" % (user, name))
                found_any = True
            with open(configset_file, "w", encoding="utf-8") as out:
                out.write(write_text_vdf(configset))
    if not found_any:
        print("No OpenXcom3D shortcut in Steam yet: add play.sh (or the AppImage) as a non-Steam game, then run this again.")
        print("(Meanwhile the layout can be picked by hand: controller settings > Edit Layout > Templates > OpenXcom3D.)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
