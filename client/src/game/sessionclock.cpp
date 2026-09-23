#include "sessionclock.h"

#include "clock.h"

namespace coopiii::game {
namespace {

// Set from PreFrame through the bridge, read inside CGame::Process a few
// milliseconds later. Both on the game thread.
bool     g_valid  = false;
uint32_t g_offset = 0;

// SessionClockThisFrame's value, taken on its first call after PreFrame.
bool     g_frameTaken = false;
uint32_t g_frameMs    = 0;

void SetSessionClock(bool valid, uint32_t offsetMs) {
	g_valid      = valid;
	g_offset     = offsetMs;
	g_frameTaken = false;
}

} // namespace

bool SessionClockNow(uint32_t &out) {
	if (!g_valid)
		return false;
	// Now, not as of PreFrame: everything CGame::Process did before the
	// caller is between the two, and it isn't the same amount of work on
	// every machine.
	out = WallClock::NowMs() + g_offset;
	return true;
}

bool SessionClockThisFrame(uint32_t &out) {
	if (!g_valid)
		return false;
	if (!g_frameTaken) {
		g_frameMs    = WallClock::NowMs() + g_offset;
		g_frameTaken = true;
	}
	out = g_frameMs;
	return true;
}

void AddSessionClockToBridge(WorldBridge &bridge) {
	bridge.SetSessionClock = &SetSessionClock;
}

} // namespace coopiii::game
