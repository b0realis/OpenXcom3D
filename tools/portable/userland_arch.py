# Arch Linux userland without pacman or root, to try the release binary on the newest libraries:
# reads the core and extra package databases, resolves the dependencies of the given packages
# (default: what the game needs), downloads and unpacks them into FOLDER/root.
# Usage: python3 userland_arch.py FOLDER [PACKAGE...]
import io, os, re, subprocess, sys, tarfile, urllib.request
MIRROR = "https://geo.mirror.pkgbuild.com"
D = sys.argv[1]
WANT = sys.argv[2:] or ["glibc", "gcc-libs", "sdl12-compat", "sdl_image", "sdl_mixer", "sdl_gfx", "libglvnd"]
os.makedirs(D + "/pkgs", exist_ok=True)
os.makedirs(D + "/root", exist_ok=True)
packages, provides = {}, {}
for repo in ["core", "extra"]:
    data = urllib.request.urlopen("%s/%s/os/x86_64/%s.db" % (MIRROR, repo, repo)).read()
    with tarfile.open(fileobj=io.BytesIO(data)) as db:
        for member in db.getmembers():
            if not member.name.endswith("/desc"):
                continue
            text = db.extractfile(member).read().decode()
            fields, key = {}, None
            for line in text.splitlines():
                if line.startswith("%") and line.endswith("%"):
                    key = line.strip("%"); fields[key] = []
                elif line and key:
                    fields[key].append(line)
            name = fields["NAME"][0]
            packages[name] = (repo, fields["FILENAME"][0], fields.get("DEPENDS", []))
            for provided in fields.get("PROVIDES", []) + [name]:
                provides.setdefault(re.split("[<>=]", provided)[0], name)
def bare(dependency):
    return re.split("[<>=]", dependency)[0]
needed, todo = set(), list(WANT)
while todo:
    name = bare(todo.pop())
    real = name if name in packages else provides.get(name)
    if real is None:
        print("unresolved:", name); continue
    if real in needed:
        continue
    needed.add(real)
    todo.extend(packages[real][2])
print(len(needed), "packages")
for name in sorted(needed):
    repo, filename, _ = packages[name]
    path = D + "/pkgs/" + filename
    if not os.path.exists(path):
        urllib.request.urlretrieve("%s/%s/os/x86_64/%s" % (MIRROR, repo, filename), path)
    subprocess.run(["tar", "-xf", path, "-C", D + "/root", "--exclude=.PKGINFO", "--exclude=.MTREE",
                    "--exclude=.BUILDINFO", "--exclude=.INSTALL"], check=True, stderr=subprocess.DEVNULL)
print("glibc:", packages["glibc"][1])
print("arch root ready")
