#!/bin/sh
# Ubuntu 22.04 (glibc 2.35) userland without root, to try the release binary on old libraries:
# its packages for SDL 1.2 and friends, with every dependency, unpacked into FOLDER/root.
# Run a binary on it with run_on_userland.sh.
set -e
D=${1:?usage: userland_ubuntu2204.sh FOLDER}
mkdir -p $D/apt/lists/partial $D/apt/cache/archives/partial $D/etc $D/root $D/debs
echo "deb [trusted=yes arch=amd64] http://archive.ubuntu.com/ubuntu jammy main universe" > $D/etc/sources.list
: > $D/apt/status
A="-o Dir::Etc::SourceList=$D/etc/sources.list -o Dir::Etc::SourceParts=/dev/null -o Dir::State::Lists=$D/apt/lists -o Dir::Cache=$D/apt/cache -o Dir::State::status=$D/apt/status -o APT::Architecture=amd64 -o APT::Architectures=amd64 -o Debug::NoLocking=1"
apt-get $A update > $D/update.log 2>&1
# the packages to unpack with their dependencies (PKGS=... to choose others, e.g. a bare desktop
# without SDL, for the release that comes with its own)
PKGS=${PKGS:-"libc6 libstdc++6 libgcc-s1 libsdl1.2debian libsdl-image1.2 libsdl-mixer1.2 libsdl-gfx1.2-5 libgl1"}
apt-cache $A depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces --no-enhances $PKGS 2>/dev/null | grep "^[a-z0-9]" | sort -u > $D/pkglist.txt
wc -l < $D/pkglist.txt
cd $D/debs && apt-get $A download $(cat $D/pkglist.txt) > $D/download.log 2>&1 || true
ls $D/debs | wc -l
for f in $D/debs/*.deb; do dpkg-deb -x "$f" $D/root; done
ls -la $D/root/lib/x86_64-linux-gnu/libc.so.6 $D/root/usr/lib/x86_64-linux-gnu/libSDL-1.2.so.0 $D/root/usr/lib/x86_64-linux-gnu/libSDL_gfx.so.15 2>&1
echo "jammy root ready"
