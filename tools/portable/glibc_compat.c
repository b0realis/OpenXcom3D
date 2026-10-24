/*
 * OpenXcom3D release builds only (see configure.sh and build_libs.sh): the C++ runtime linked
 * into the game, and the bundled libraries, were built against a new glibc and ask for a few
 * functions an older one doesn't have. They are defined here with the classic calls, hidden
 * inside the game and each library (everything else keeps using the system's own).
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#define HIDDEN __attribute__((visibility("hidden")))

/* glibc 2.38's C23 strtol family and sscanf family (they also read 0b binary numbers, which
   nothing here reads): the classic ones (glibc_compat.h switches the C23 names off here). */
HIDDEN long __isoc23_strtol(const char *text, char **end, int base)
{
	return strtol(text, end, base);
}

HIDDEN unsigned long __isoc23_strtoul(const char *text, char **end, int base)
{
	return strtoul(text, end, base);
}

HIDDEN long long __isoc23_strtoll(const char *text, char **end, int base)
{
	return strtoll(text, end, base);
}

HIDDEN unsigned long long __isoc23_strtoull(const char *text, char **end, int base)
{
	return strtoull(text, end, base);
}

HIDDEN int __isoc23_vsscanf(const char *text, const char *format, va_list arguments)
{
	return vsscanf(text, format, arguments);
}

HIDDEN int __isoc23_sscanf(const char *text, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	int read = vsscanf(text, format, arguments);
	va_end(arguments);
	return read;
}

HIDDEN int __isoc23_fscanf(FILE *file, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	int read = vfscanf(file, format, arguments);
	va_end(arguments);
	return read;
}

/* glibc 2.36's arc4random (std::random_device): random bits from the kernel, or from
   /dev/urandom on kernels without getrandom, or the clock as the last resort. */
HIDDEN uint32_t arc4random(void)
{
	uint32_t value = 0;
	if (getrandom(&value, sizeof(value), 0) == (ssize_t)sizeof(value))
	{
		return value;
	}
	int device = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (device >= 0)
	{
		ssize_t got = read(device, &value, sizeof(value));
		close(device);
		if (got == (ssize_t)sizeof(value))
		{
			return value;
		}
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_nsec ^ (now.tv_sec << 20) ^ (uintptr_t)&now);
}
