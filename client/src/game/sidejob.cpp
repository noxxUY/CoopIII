#include "sidejob.h"

#include "addresses.h"
#include "leadcheck.h"
#include "seat.h"
#include "../log.h"

#include <cstring>

#include <windows.h>

namespace coopiii::game {

namespace {

using PadStateFn = int16_t(__thiscall *)(void *script, int32_t pad, int32_t button);

bool g_installed     = false;
bool g_saidPassenger = false;

// 0x0043DB17. GetPadState is __thiscall with two dwords and `ret 8`, which
// __fastcall with the spare edx is too. Only the low word of each argument is
// meaningful: the handler pushes esi and eax after loading si and ax.
int16_t __fastcall PadStateForButton(void *script, void * /*edx*/, int32_t pad, int32_t button) {
	const int16_t down = Func<PadStateFn>(CRunningScript__GetPadState)(script, pad, button);
	if (down == 0)
		return down;
	const int32_t p = static_cast<int16_t>(pad & 0xFFFF);
	const int32_t b = static_cast<int16_t>(button & 0xFFFF);
	if (SubMissionKeyCounts(p, b, LocalIsPassenger()))
		return down;
	if (!g_saidPassenger) {
		g_saidPassenger = true;
		Log("sidejob: the sub-mission key was pressed in a car somebody else drives; the "
		    "script hears nothing, so an odd job starts only from the wheel (said once)");
	}
	return 0;
}

bool Redirect(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

} // namespace

bool InstallSideJobKey() {
	if (g_installed)
		return true;
	g_installed = Redirect(IS_BUTTON_PRESSED_PadStateCall, CRunningScript__GetPadState,
	                       reinterpret_cast<uintptr_t>(&PadStateForButton));
	if (g_installed)
		Log("sidejob: IS_BUTTON_PRESSED's pad read at 0x%08X comes to us; a passenger's "
		    "sub-mission key starts no odd job",
		    static_cast<unsigned>(IS_BUTTON_PRESSED_PadStateCall));
	else
		Log("sidejob: FAILED to take IS_BUTTON_PRESSED's pad read at 0x%08X; it no longer "
		    "calls 0x%08X, so a passenger's key can start an odd job in a teammate's car",
		    static_cast<unsigned>(IS_BUTTON_PRESSED_PadStateCall),
		    static_cast<unsigned>(CRunningScript__GetPadState));
	return g_installed;
}

void RemoveSideJobKey() {
	if (g_installed &&
	    Redirect(IS_BUTTON_PRESSED_PadStateCall, reinterpret_cast<uintptr_t>(&PadStateForButton),
	             CRunningScript__GetPadState))
		g_installed = false;
}

} // namespace coopiii::game
