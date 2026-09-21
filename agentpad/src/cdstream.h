// Gives this copy of GTA III its own streaming semaphore so two copies can run
// side by side.
//
// Background: the game has no single-instance guard of its own. Checked the
// retail v1.0 import table for one: no CreateMutexA/W, no OpenMutex, no
// CreateEventA, no FindWindowA/W, no EnumWindows, no delay-load table. 172
// imports across ten DLLs, none of them the thing that would make a second
// copy exit. LoadLibraryA is in there but only ever loads DDRAW.DLL, D3D8.DLL
// and dpnhpast.dll, and "mutex" doesn't appear anywhere in the image, so it's
// not resolving anything dynamically either.
//
// What it does have is one named kernel object, and it's a trap for exactly
// this scenario:
//
//     CdStreamInitThread:
//       0x00405BD7  push 0x005EC034            ; "CdStream"
//       0x00405BDC  push 5                     ; lMaximumCount
//       0x00405BDE  push 0                     ; lInitialCount
//       0x00405BE0  push 0                     ; lpSemaphoreAttributes
//       0x00405BF1  call dword [0x0061D3D8]    ; KERNEL32!CreateSemaphoreA
//       0x00405BF9  mov  dword [0x0062129C], eax
//
// Matches re3's CdStream.cpp:76 exactly. re3 itself patches this line under
// FIX_BUGS to CreateSemaphore(nil, 0, 5, nil), so the upstream fix is just to
// stop naming the thing.
//
// Because it's session-named, the second process doesn't get its own
// semaphore, CreateSemaphoreA just hands back a handle to the first copy's.
// From then on both processes share one signal. CdStreamRead pushes onto its
// own per-process queue and calls ReleaseSemaphore; whichever process's
// streaming thread happens to be waiting wakes up, and CdStreamThread pops
// from *its own* queue regardless. If that queue is empty, GetFirstInQueue
// just returns the zeroed head, channel 0. End result: one game re-reads
// channel 0 for nothing while the other one's actual request sits stuck until
// something else happens to release the semaphore. Looks like random
// streaming stalls and missing models in whatever copy loses the race, and
// gives no hint as to why.
//
// Fix here is to rename the string, in our own process's memory, before the
// game gets to read it. Nine bytes at 0x005EC034: "CdStream\0" becomes "Cd"
// plus six base-62 digits of the pid, same length, so nothing past the
// original string moves. (There's four bytes of NUL padding after it before
// the next string at 0x005EC040, checked, not using it.) Six base-62 digits
// is 62^6 = 5.68e10 possible values, comfortably more than the 32-bit pid
// space, so two live instances can't collide.
//
// Doesn't touch the game's files, doesn't touch anything else in the image.
// Verifies the nine bytes byte-for-byte before writing: if that address isn't
// exactly "CdStream\0" (wrong build, or something else got there first) it
// bails and says why.
//
// Why this happens in DllMain specifically: docs/compat.md §2.2 says nothing
// gets hooked or scanned there, and that's still true: no hook installed, no
// scanning, no code rewritten, no library loaded. Just a nine-byte store into
// a .data string constant that nothing else has reason to touch.
//
// It has to run there because of timing. CdStreamInitThread runs inside
// CGame::InitialiseOnceBeforeRW, which WinMain reaches via
// RsEventHandler(rsINITIALIZE) before gGameState ever leaves GS_START_UP.
// There's no game-state signal to hook off of, and spinning up a worker
// thread from DllMain would just be racing the game's own startup to write
// first. DllMain is the only point guaranteed to run before that read: the
// ASI loader pulls our DLL in during the game's static import resolution, so
// all of it executes before gta3.exe even reaches its entry point.
#pragma once

#include <cstdint>

#include "settings.h"

namespace agentpad {

// Retail v1.0 address of the "CdStream" string literal, in .data. That's
// mapped read/write already so VirtualProtect isn't strictly needed, but we
// take it anyway instead of assuming.
inline constexpr uintptr_t CdStreamSemaphoreName = 0x005EC034;

struct CdStreamPatch {
	bool applied = false;
	// Set either way, fired or not. Goes straight into the log.
	const char *reason = "";
	// Name actually installed, if applied.
	char name[16] = {0};
};

// Decides whether to rename and does it. Call exactly once, from DllMain,
// before any game code runs.
//
// mode is Settings::multiInstance:
//   Off  - never patch.
//   Auto - only if a "CdStream" semaphore already exists, meaning another
//          copy is already running. First copy stays untouched, only the
//          second one deviates. Smallest footprint for the common case.
//   On   - always patch.
CdStreamPatch ApplyCdStreamPatch(Settings::MultiInstance mode, uint32_t pid);

// The name this process would use. Exposed for tests. Must come out exactly
// eight characters (fits the original string's footprint) and must be
// injective over pids.
void FormatCdStreamName(uint32_t pid, char *out, size_t cap);

} // namespace agentpad
