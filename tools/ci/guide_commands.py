"""Writes the build guide's commands (pages/index.html) for one route as a shell script, so the
automatic builds run exactly what a reader copies from the page, step by step.

usage: python3 guide_commands.py ROUTE [DISTRO] [--root] [--steps=2,3] > guide.sh
  ROUTE    linux, win (Windows with MSYS2) or cross (the Windows zip built on Linux)
  DISTRO   for the steps that differ by distribution: a name from the page ("Ubuntu", "Fedora",
           "Arch"); not needed for win
  --root   the commands run as root (a container): "sudo " is left out
  --steps  only these steps (all of them, 2 to 5, by default)

Only what a machine without a person at it needs is changed, and the script says so next to
each change:
- package managers don't ask (apt-get -y after an update, dnf -y, pacman --noconfirm; Arch
  syncs first, and MSYS2's update runs twice, as the page says to when it closes the window);
- the patch is the one in this checkout ($GUIDE_PATCH), not main's on GitHub, so a change is
  tested before it's merged; the cross route builds in this checkout ($GUIDE_REPO) instead of a
  fresh clone of it;
- there's no game data here: the line that links or copies it is left out, and the game is
  started with -version (it says its version and quits), which shows it runs.
"""
import html.parser
import os
import sys

# Elements that never have a closing tag (the stack below must not wait for one)
VOID_TAGS = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}


class GuideReader(html.parser.HTMLParser):
    """Collects every command block (<pre class="cmds">) with its step, its routes (the nearest
    data-os around it) and its distribution (the <p class="distro"> just before it)."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = []          # the open elements: (tag, attributes)
        self.blocks = []         # (step number, set of routes, distro or None, text)
        self.distro = None       # the last distribution name seen, for the next block
        self.reading = None      # what's being read: "distro", "cmds" or None
        self.text = ""

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        classes = (attributes.get("class") or "").split()
        if tag not in VOID_TAGS:
            self.stack.append((tag, attributes))
        if tag == "p" and "distro" in classes:
            self.reading, self.text = "distro", ""
        elif tag == "pre" and "cmds" in classes:
            self.reading, self.text = "cmds", ""

    def handle_endtag(self, tag):
        if tag in VOID_TAGS:
            return
        # A distribution's name, or a whole command block, ends with its element
        if self.reading == "distro" and tag == "p":
            self.distro, self.reading = self.text.strip(), None
        elif self.reading == "cmds" and tag == "pre":
            self.blocks.append((self.step(), self.routes(), self.distro, self.text.strip("\n")))
            self.distro, self.reading = None, None
        # Close the element (and any left open inside it)
        while self.stack:
            if self.stack.pop()[0] == tag:
                break

    def handle_data(self, data):
        if self.reading:
            self.text += data

    def step(self):
        """The number of the step (li id="step-N") the block is in, or 0 outside the steps."""
        for tag, attributes in reversed(self.stack):
            if tag == "li" and (attributes.get("id") or "").startswith("step-"):
                return int(attributes["id"][5:])
        return 0

    def routes(self):
        """The routes of the nearest element around the block that names them (data-os)."""
        for tag, attributes in reversed(self.stack):
            if "data-os" in attributes:
                return set(attributes["data-os"].split())
        return {"linux", "win", "cross"}


def unattended(line, route, root):
    """The command lines a machine runs for one line of the guide (the line itself unless it
    needs a change, with the original kept as a comment)."""
    original = line
    command = line[5:] if root and line.startswith("sudo ") else line
    changed = []
    # Package managers that don't ask
    if command.startswith(("apt install ", "sudo apt install ")):
        prefix = "sudo " if command.startswith("sudo ") else ""
        packages = command.split("apt install ", 1)[1]
        changed = [prefix + "apt-get update", prefix + "env DEBIAN_FRONTEND=noninteractive apt-get install -y " + packages]
    elif " dnf install " in " " + command:
        changed = [command.replace("dnf install ", "dnf install -y ", 1)]
    elif route == "win" and command.strip() == "pacman -Syu":
        changed = ["pacman -Syu --noconfirm", "pacman -Syu --noconfirm"]
    elif "pacman -S " in command:
        flags = "-S --needed --noconfirm" if route == "win" else "-Syu --needed --noconfirm"
        changed = [command.replace("-S --needed", flags, 1)]
    # This checkout's patch, and this checkout for the cross route
    elif command.startswith("curl ") and command.endswith("/openxcom3d.patch"):
        changed = ['cp "$GUIDE_PATCH" openxcom3d.patch']
    elif route == "cross" and command.startswith("git clone https://github.com/b0realis/OpenXcom3D"):
        changed = ['cd "$GUIDE_REPO"']
    elif route == "cross" and command == "cd OpenXcom3D":
        changed = ["# (already there)"]
    # No game data on this machine; the game says its version instead of starting
    elif "steamapps/common" in command:
        changed = ["# (no game data on this machine: left out)"]
    elif command.startswith("build/bin/openxcom"):
        changed = [command + " -version"]
    if not changed:
        return [command] if command == original else ["# guide: " + original, command]
    return ["# guide: " + original] + changed


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    root = "--root" in sys.argv
    steps = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--steps=")]
    steps = {int(n) for n in steps[0].split(",")} if steps else None
    if not args or args[0] not in ("linux", "win", "cross"):
        sys.exit(__doc__)
    route = args[0]
    distro = args[1] if len(args) > 1 else None

    # The page, read
    page = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "pages", "index.html")
    reader = GuideReader()
    with open(page, encoding="utf-8") as f:
        reader.feed(f.read())

    # The route's blocks, step by step: the distribution's where the page has one per distribution
    chosen = []
    for step, routes, block_distro, text in reader.blocks:
        if step == 0 or route not in routes or (steps and step not in steps):
            continue
        if block_distro is not None and (distro is None or distro.lower() not in block_distro.lower()):
            continue
        chosen.append((step, block_distro, text))
    if not chosen:
        sys.exit("the guide has no commands for %s %s" % (route, distro or ""))
    if any(d is not None for _, d, _ in chosen) and distro is None:
        sys.exit("the %s route differs by distribution: name one" % route)

    # The script
    out = ["#!/bin/sh",
           "# The build guide's commands (pages/index.html) for the %s route%s, written out by" % (route, ", " + distro if distro else ""),
           "# tools/ci/guide_commands.py. Lines it changed for a machine without a person at it carry",
           "# the guide's own line above them as a comment.",
           "set -ex"]
    for step, block_distro, text in chosen:
        out.append("")
        out.append("# step %d%s" % (step, " (" + block_distro + ")" if block_distro else ""))
        for line in text.split("\n"):
            out += unattended(line, route, root)
    print("\n".join(out))


if __name__ == "__main__":
    main()
