/*
 * ==============================================================================================
 *  OpenXcom3D: a first-person 3D battlescape for OpenXcom Extended
 * ==============================================================================================
 *
 *  Our philosophy
 *
 *  - Faithful first. The 3D view should feel as if it had shipped with UFO: Enemy Unknown on
 *    the day the game came out.
 *  - Another camera on the same battle. The original rules decide everything: time units,
 *    energy, accuracy, line of fire, reaction fire, doors and morale. The 3D view only shows
 *    what the game does.
 *  - Only the original assets: the game's own maps, voxel shapes, sprites, palette and sounds.
 *  - Every addition is a setting. Jumping, night vision as gear, falling, FPS-style shooting
 *    and dual wield can all be switched off.
 *  - Light on the engine. Our code lives in this folder, the core only gets small hooks, and
 *    without the OpenXcom3D mod nothing changes.
 *  - Runs anywhere: a software renderer on the CPU, no graphics card needed.
 *  - Written for people: descriptive names, and a comment for every function and every block.
 *
 *  Date: 2026-11-06
 *
 *  We wish everyone all the best and all health!
 * ==============================================================================================
 */
/*
 * Copyright 2010-2026 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
/*
 * FirstPersonCrashLog.cpp: Android's crash log (the Meta Quest build only; see the header).
 *
 * A crash's signal is caught (catchCrashSignals, on the thread's own signal stack: a stack
 * overflow too) and handed to the game's crash dump (CrossPlatform::crashDump), whose stack
 * trace comes here (logCrashPlace): the thread, the signal's details and where it happened (the
 * registers), then the stack as the unwinder walks it (_Unwind_Backtrace; each frame's library,
 * offset and nearest exported symbol by dladdr, and the library's build ID). Every line goes to
 * the system's log first (logcat, tag OpenXcom3D; no memory taken: the crash may be in the
 * allocator), then to the game's (openxcom.log), in the system crash report's frame format
 * ("#NN pc <offset>  <library> (<symbol>+<n>) (BuildId: ...)"), so ndk-stack or llvm-addr2line
 * name each frame's source line from the build's unstripped library (quest3/BUILD.md). The
 * signal then goes on to the system's own handler (its crash report: logcat's tag DEBUG, a
 * tombstone), which ends the game.
 */
#ifdef __ANDROID__
#include "FirstPersonCrashLog.h"
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
#include <android/log.h>
#include "../Engine/CrossPlatform.h"
#include "../Engine/Logger.h"

namespace OpenXcom
{

namespace FirstPerson
{

namespace
{

/// The signals that are crashes (OXCE's handler took only the segfault).
const int CrashSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
/// The most frames the stack's walk logs.
const int MaxFrames = 64;
/// A call's return address is this far past the call: a frame's line is the call's (the address is taken back by it,
/// as the system's crash report does). arm64's instructions are 4 bytes; on x86 any byte of the call will do.
#if defined(__aarch64__) || defined(__arm__)
const uintptr_t CallBack = 4;
#else
const uintptr_t CallBack = 1;
#endif
/// The line that starts a crash report for ndk-stack (the system's own report starts with it too).
const char *const CrashBanner = "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***";

/// Each crash signal's handler before ours (the system's crash report), by signal.
struct sigaction previousActions[NSIG];
/// The crash being logged: its signal's details and the registers where it happened (null: none; the stack is then
/// walked from where it is logged, as for a C++ exception).
siginfo_t *crashInfo = nullptr;
void *crashContext = nullptr;
/// A crash is being logged (another thread's crash meanwhile waits for it).
std::atomic<bool> crashLogging{false};

/// A line of the crash log: to the system's log first (no memory taken: the crash may be in the allocator), then the
/// game's.
void crashLine(const char *line)
{
	__android_log_print(ANDROID_LOG_FATAL, "OpenXcom3D", "%s", line);
	Log(LOG_FATAL) << line;
}

/// What dl_iterate_phdr looks for: the library loaded at an address, and its build ID as hex, once found.
struct BuildIdSearch
{
	uintptr_t base;
	char hex[2 * 64 + 1];
};

/**
 * dl_iterate_phdr's callback: in the library loaded where the search's base is, its build ID (the GNU note the linker
 * writes; ndk-stack checks it against the symbols' library).
 * @param info The library.
 * @param search The search (BuildIdSearch).
 * @return 1 once the library was found (the walk ends), else 0.
 */
int findBuildId(struct dl_phdr_info *info, size_t, void *search)
{
	BuildIdSearch &wanted = *(BuildIdSearch *)search;
	// The library's loaded segments: is the base one of them?
	bool found = false;
	for (int i = 0; i < info->dlpi_phnum && !found; ++i)
	{
		const ElfW(Phdr) &segment = info->dlpi_phdr[i];
		const uintptr_t start = info->dlpi_addr + segment.p_vaddr;
		found = segment.p_type == PT_LOAD && wanted.base >= start && wanted.base < start + segment.p_memsz;
	}
	if (!found)
	{
		return 0;
	}
	// Its notes: the one named GNU of the build ID's type, as hex.
	for (int i = 0; i < info->dlpi_phnum; ++i)
	{
		const ElfW(Phdr) &segment = info->dlpi_phdr[i];
		if (segment.p_type != PT_NOTE)
		{
			continue;
		}
		const char *note = (const char *)(info->dlpi_addr + segment.p_vaddr);
		const char *notesEnd = note + segment.p_memsz;
		while (note + sizeof(ElfW(Nhdr)) <= notesEnd)
		{
			const ElfW(Nhdr) &header = *(const ElfW(Nhdr) *)note;
			const char *name = note + sizeof(ElfW(Nhdr));
			const unsigned char *description = (const unsigned char *)(name + ((header.n_namesz + 3) & ~3u));
			if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz == 4 && memcmp(name, "GNU", 4) == 0)
			{
				const size_t bytes = std::min<size_t>(header.n_descsz, (sizeof(wanted.hex) - 1) / 2);
				for (size_t byte = 0; byte < bytes; ++byte)
				{
					snprintf(wanted.hex + 2 * byte, 3, "%02x", description[byte]);
				}
				return 1;
			}
			note = (const char *)description + ((header.n_descsz + 3) & ~3u);
		}
	}
	return 1;
}

/**
 * A frame's line, as the system's crash report has it: "#NN pc <offset in its library>  <library> (<nearest
 * symbol>+<offset>) (BuildId: <id>)". (The nearest symbol is the nearest the library exports: the unstripped
 * library's line, from ndk-stack or llvm-addr2line, is the exact place.)
 * @param index The frame's number.
 * @param address Its address in memory.
 * @param line The line, written here.
 * @param size Its room.
 */
void frameLine(int index, uintptr_t address, char *line, size_t size)
{
	Dl_info place;
	if (!dladdr((void *)address, &place) || !place.dli_fname)
	{
		snprintf(line, size, "    #%02d pc %016" PRIxPTR "  <unknown>", index, address);
		return;
	}
	// The nearest symbol, its C++ name if it has one.
	char symbol[320] = "";
	if (place.dli_sname && place.dli_saddr)
	{
		int status = -1;
		char *readable = (place.dli_sname[0] == '_' && place.dli_sname[1] == 'Z') ? abi::__cxa_demangle(place.dli_sname, nullptr, nullptr, &status) : nullptr;
		snprintf(symbol, sizeof(symbol), " (%s+%" PRIuPTR ")", (status == 0 && readable) ? readable : place.dli_sname, address - (uintptr_t)place.dli_saddr);
		free(readable);
	}
	// The library's build ID.
	BuildIdSearch search;
	search.base = (uintptr_t)place.dli_fbase;
	search.hex[0] = '\0';
	dl_iterate_phdr(findBuildId, &search);
	char buildId[sizeof(search.hex) + 16] = "";
	if (search.hex[0])
	{
		snprintf(buildId, sizeof(buildId), " (BuildId: %s)", search.hex);
	}
	snprintf(line, size, "    #%02d pc %016" PRIxPTR "  %s%s%s", index, address - (uintptr_t)place.dli_fbase, place.dli_fname, symbol, buildId);
}

/// The stack as _Unwind_Backtrace walks it: each frame's address, and whether that's the very instruction (where a
/// signal stopped it) rather than a call's return address.
struct StackWalk
{
	uintptr_t addresses[MaxFrames];
	bool exact[MaxFrames];
	int count = 0;
};

/// _Unwind_Backtrace's callback: notes a frame, until the walk's list is full.
_Unwind_Reason_Code noteFrame(struct _Unwind_Context *context, void *walk)
{
	StackWalk &frames = *(StackWalk *)walk;
	int beforeInstruction = 0;
	const uintptr_t address = _Unwind_GetIPInfo(context, &beforeInstruction);
	if (address == 0)
	{
		return _URC_END_OF_STACK;
	}
	frames.addresses[frames.count] = address;
	frames.exact[frames.count] = beforeInstruction != 0;
	++frames.count;
	return frames.count < MaxFrames ? _URC_NO_REASON : _URC_END_OF_STACK;
}

/**
 * Hands a crash's signal on to the handler it had before ours (the system's crash report): that handler back in place,
 * and the signal sent again if it was sent (an abort; it waits until this handler returns). A fault happens again by
 * itself, once this handler returns, into that handler.
 * @param signal The signal.
 * @param info Its details.
 */
void handOnCrash(int signal, siginfo_t *info)
{
	sigaction(signal, &previousActions[signal], nullptr);
	if (info && info->si_code <= 0)
	{
		syscall(__NR_rt_tgsigqueueinfo, getpid(), gettid(), signal, info);
	}
}

/**
 * The crash signals' handler: the crash logged (the game's crash dump, whose stack trace is logCrashPlace's, with these
 * registers), then handed on to the system's handler. A second thread's crash meanwhile waits a few seconds for the
 * first's log first.
 * @param signal The signal.
 * @param info Its details.
 * @param context The registers where it happened.
 */
void crashHandler(int signal, siginfo_t *info, void *context)
{
	bool first = false;
	if (!crashLogging.compare_exchange_strong(first, true))
	{
		sleep(3);
		handOnCrash(signal, info);
		return;
	}
	crashInfo = info;
	crashContext = context;
	CrossPlatform::crashDump(&signal, "");
	handOnCrash(signal, info);
}

}

/**
 * Catches the crash signals (see the header), on the thread's own signal stack (Android gives every thread one), so a
 * stack overflow is logged too; each goes on to the system's handler after (see crashHandler).
 */
void catchCrashSignals()
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = crashHandler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (int signal : CrashSignals)
	{
		sigaction(signal, &action, &previousActions[signal]);
	}
}

/**
 * Logs the crash's place: the thread, the signal's details and the registers' places (the instruction, and on arm64
 * the return address in the link register), then the stack, frame by frame (see the file's comment).
 * @param context The registers where it happened, or null: the crash being handled's (none for a C++ exception: the
 * stack is then walked from here).
 */
void logCrashPlace(void *context)
{
	const siginfo_t *info = crashInfo;
	if (!context)
	{
		context = crashContext;
	}
	char line[768];
	// The thread (the game's is SDLThread, the VR session's "OpenXcom3D XR") and the signal.
	char thread[17] = "";
	prctl(PR_GET_NAME, thread, 0, 0, 0);
	crashLine(CrashBanner);
	if (info)
	{
		snprintf(line, sizeof(line), "crash: signal %d, code %d, fault address %p, thread %s (tid %d)", info->si_signo, info->si_code, info->si_addr, thread, (int)gettid());
	}
	else
	{
		snprintf(line, sizeof(line), "crash: thread %s (tid %d)", thread, (int)gettid());
	}
	crashLine(line);
#if defined(__aarch64__)
	crashLine("crash: ABI: 'arm64'");
#elif defined(__x86_64__)
	crashLine("crash: ABI: 'x86_64'");
#endif
	// Where it happened, from the registers: the instruction (#00) and, on arm64, the return address in the link
	// register (#01: the caller, or the function itself after a call it made).
	if (context)
	{
		const ucontext_t &registers = *(const ucontext_t *)context;
#if defined(__aarch64__)
		crashLine("crash: where it happened (the registers: pc, then the link register)");
		frameLine(0, (uintptr_t)registers.uc_mcontext.pc, line, sizeof(line));
		crashLine(line);
		frameLine(1, (uintptr_t)registers.uc_mcontext.regs[30] - CallBack, line, sizeof(line));
		crashLine(line);
#elif defined(__x86_64__)
		crashLine("crash: where it happened (the registers: the instruction)");
		frameLine(0, (uintptr_t)registers.uc_mcontext.gregs[REG_RIP], line, sizeof(line));
		crashLine(line);
#endif
		crashLine("crash: the stack follows");
		crashLine(CrashBanner);
	}
	// The stack, as the unwinder walks it from here (this handler's own frames first, then the signal's, then where it
	// happened and its callers); a return address taken back to its call.
	StackWalk walk;
	_Unwind_Backtrace(noteFrame, &walk);
	for (int frame = 0; frame < walk.count; ++frame)
	{
		frameLine(frame, walk.exact[frame] ? walk.addresses[frame] : walk.addresses[frame] - CallBack, line, sizeof(line));
		crashLine(line);
	}
	snprintf(line, sizeof(line), "crash: %d frames%s", walk.count, info ? " (the system's own crash report follows: logcat tag DEBUG)" : "");
	crashLine(line);
}

}

}

#endif

// Thanks mom and dad! You were the ones who exposed me to my first comp.
