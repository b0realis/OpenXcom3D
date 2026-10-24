#!/bin/sh
# Runs a binary on a userland made by userland_ubuntu2204.sh or userland_arch.py, with that
# system's own dynamic loader and libraries only (not this machine's).
# Usage: run_on_userland.sh FOLDER BINARY [ARGUMENTS...]   e.g. ... -help, or -data DIR -load SAVE
# With LD_BIND_NOW=1 every symbol is bound at start, so a missing glibc version shows at once.
R="$1/root"; shift
if [ -x "$R/usr/lib/ld-linux-x86-64.so.2" ] && [ ! -L "$R/usr/lib/ld-linux-x86-64.so.2" ]; then
	exec "$R/usr/lib/ld-linux-x86-64.so.2" --inhibit-cache --library-path "$R/usr/lib" "$@"
fi
exec "$R/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" --inhibit-cache \
	--library-path "$R/lib/x86_64-linux-gnu:$R/usr/lib/x86_64-linux-gnu:$R/usr/lib/x86_64-linux-gnu/pulseaudio" "$@"
