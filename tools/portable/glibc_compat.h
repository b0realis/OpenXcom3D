/*
 * OpenXcom3D release builds only: tools/portable/configure.sh puts this in front of every
 * source file. It lets the prebuilt binary start on older Linux systems too, back to glibc 2.34
 * (2021: Ubuntu 22.04, Debian 12, Fedora 35, RHEL 9, and every rolling distribution).
 *
 * A newer glibc gives some functions a new version, and a program built with it asks for that
 * version, which an older glibc doesn't have. Every newer glibc still carries the old versions
 * (for programs built before), so binding the calls to them works on old and new systems alike:
 * - the functions in glibc_pins.h;
 * - since 2.38, programs built with _GNU_SOURCE get C23 versions of strtol and sscanf (they
 *   also read 0b binary numbers, which nothing here reads): switched back to the classic ones.
 * tools/portable/check.sh refuses a binary that still asks for anything newer than 2.34.
 */
#ifndef OPENXCOM3D_GLIBC_COMPAT_H
#define OPENXCOM3D_GLIBC_COMPAT_H

#include <features.h>
#undef __GLIBC_USE_C23_STRTOL
#define __GLIBC_USE_C23_STRTOL 0

#include "glibc_pins.h"

#endif
