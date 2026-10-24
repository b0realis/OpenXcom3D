/*
 * OpenXcom3D release builds only: binds the calls that newer glibc versions gave a new version
 * to their old versions, which every newer glibc still carries (for programs built before), so
 * what is built here starts on glibc 2.34 too: hypot (a new version in glibc 2.35), fmod, fmodf
 * (2.38), sqrtf, asinf, acosf, atan2f, log10f, remainder, remainderf (2.43). glibc_compat.h
 * puts it in front of the game's sources; build_libs.sh in front of the bundled libraries' (on
 * its own: it includes nothing, so their sources still choose their own glibc features).
 */
#ifndef OPENXCOM3D_GLIBC_PINS_H
#define OPENXCOM3D_GLIBC_PINS_H

__asm__(".symver hypot,hypot@GLIBC_2.2.5");
__asm__(".symver fmod,fmod@GLIBC_2.2.5");
__asm__(".symver fmodf,fmodf@GLIBC_2.2.5");
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
__asm__(".symver asinf,asinf@GLIBC_2.2.5");
__asm__(".symver acosf,acosf@GLIBC_2.2.5");
__asm__(".symver atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver log10f,log10f@GLIBC_2.2.5");
__asm__(".symver remainder,remainder@GLIBC_2.2.5");
__asm__(".symver remainderf,remainderf@GLIBC_2.2.5");

#endif
