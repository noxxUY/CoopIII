// fontcull.h has the bug and the plan; addresses.h has the bytes.
//
// Done from PreFrame rather than from the boot thread: PrintChar runs for every
// glyph of every string, and eight bytes cannot be written in one go. On the
// game thread, between frames, nothing is inside them.
//
// Not undone on unload. The fix points at the game's own global and nothing
// of ours, and rewriting it from the unloading thread is the race above.
#include "fontcull.h"

#include "leadcheck.h"
#include "../log.h"

#include <windows.h>

namespace coopiii::game {

namespace {

bool g_tried = false;
bool g_fixed = false;

} // namespace

void FixFontCull() {
	if (g_tried)
		return;
	g_tried = true;

	const uint8_t *const block = Ptr<uint8_t>(CFont__PrintChar);
	const uint8_t *const call  = Ptr<uint8_t>(CFont__PrintString_PrintCharCall);

	// Somebody else's PrintChar would never reach these bytes.
	if (!RelCallAt(call, CFont__PrintString_PrintCharCall, CFont__PrintChar)) {
		Log("font: CFont::PrintString's call at 0x%08X no longer goes to PrintChar, so "
		    "its y test was left alone; in a window taller than it is wide, text below "
		    "y = width may not print",
		    static_cast<unsigned>(CFont__PrintString_PrintCharCall));
		return;
	}

	switch (ClassifyFontCull(block)) {
	case FontCullBytes::Height:
		g_fixed = true;
		Log("font: CFont::PrintChar already tests y against the screen height");
		return;
	case FontCullBytes::Foreign: {
		const size_t i = FirstForeignByte(block);
		Log("font: CFont::PrintChar is not retail at 0x%08X (%02X, retail has %02X), so "
		    "its y test was left alone; in a window taller than it is wide, text below "
		    "y = width may not print",
		    static_cast<unsigned>(CFont__PrintChar + i), block[i], PRINTCHAR_CULL_RETAIL[i]);
		return;
	}
	case FontCullBytes::Retail:
		break;
	}

	void *const  site = Ptr<void>(CFont__PrintChar_CullY);
	const size_t len  = sizeof(PRINTCHAR_CULL_Y_HEIGHT);
	DWORD        old  = 0;
	if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) {
		Log("font: could not unprotect CFont::PrintChar's y test at 0x%08X (error %lu); "
		    "left as retail",
		    static_cast<unsigned>(CFont__PrintChar_CullY), GetLastError());
		return;
	}
	std::memcpy(site, PRINTCHAR_CULL_Y_HEIGHT, len);
	VirtualProtect(site, len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, len);
	if (ClassifyFontCull(block) != FontCullBytes::Height) {
		Log("font: rewrote CFont::PrintChar's y test at 0x%08X and it did not read back",
		    static_cast<unsigned>(CFont__PrintChar_CullY));
		return;
	}
	g_fixed = true;
	Log("font: CFont::PrintChar tests y against the screen height now, not the width "
	    "(0x%08X); text prints all the way down a window taller than it is wide",
	    static_cast<unsigned>(CFont__PrintChar_CullY));
}

float CurrentTextCullLine(float screenW, float screenH) {
	return TextCullLine(screenW, screenH, g_fixed);
}

} // namespace coopiii::game
