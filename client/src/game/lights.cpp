#include "lights.h"

#include "addresses.h"
#include "hook/hook.h"
#include "lighttime.h"
#include "log.h"
#include "sessionclock.h"

#include <cstdint>

namespace coopiii::game {
namespace {

// Static, no arguments, the answer in al.
using LightFn = uint8_t(__cdecl *)();

Detour g_cars1;
Detour g_cars2;
Detour g_peds;

// Once each way in the log, as in trains.cpp.
bool g_onSessionClock = false;

// The clock the lights run on this frame. False means there is no session,
// and the caller runs the engine's own function, which reads CTimer.
//
// Answered here rather than by putting the session's clock in CTimer's place
// around the original, the way trains.cpp does. The three are whole leaves -
// one read of the clock, a mask, compares, ret - so lighttime.h is all of
// each one, and there is nothing inside them a swap would be needed to reach.
// They are also called for every car near a light and every light on screen,
// many times a frame, and a swap would put a foreign value in CTimer and take
// it back each time. Answering directly never lets the engine see anything
// but its own clock.
//
// Once a frame rather than at the call, so every car and every light asked
// during one frame gets the same answer, as they would from CTimer.
bool LightClock(uint32_t &out) {
	if (SessionClockThisFrame(out)) {
		if (!g_onSessionClock) {
			g_onSessionClock = true;
			const uint32_t ours = Global<uint32_t>(CTimer__m_snTimeInMilliseconds);
			Log("lights: on the session clock, %d ms from ours; %u ms into the "
			    "%u ms cycle, where ours says %u",
			    static_cast<int32_t>(out - ours), LightCycleMs(out), LIGHT_PERIOD_MS,
			    LightCycleMs(ours));
		}
		return true;
	}
	if (g_onSessionClock) {
		g_onSessionClock = false;
		Log("lights: back on this machine's own clock");
	}
	return false;
}

uint8_t __cdecl HookedLightForCars1() {
	uint32_t t = 0;
	return LightClock(t) ? CarLights1(t) : g_cars1.Original<LightFn>()();
}

uint8_t __cdecl HookedLightForCars2() {
	uint32_t t = 0;
	return LightClock(t) ? CarLights2(t) : g_cars2.Original<LightFn>()();
}

uint8_t __cdecl HookedLightForPeds() {
	uint32_t t = 0;
	return LightClock(t) ? PedLights(t) : g_peds.Original<LightFn>()();
}

bool InstallOne(Detour &d, const char *name, uintptr_t target, void *hook) {
	if (d.Install(name, reinterpret_cast<void *>(target), hook)) {
		Log("lights: hooked %s at 0x%08X", name, target);
		return true;
	}
	Log("lights: FAILED to hook %s at 0x%08X; that light runs on this "
	    "machine's own clock",
	    name, target);
	return false;
}

} // namespace

bool InstallLightClock() {
	bool ok = InstallOne(g_cars1, "CTrafficLights::LightForCars1",
	                     CTrafficLights__LightForCars1,
	                     reinterpret_cast<void *>(&HookedLightForCars1));
	ok = InstallOne(g_cars2, "CTrafficLights::LightForCars2",
	                CTrafficLights__LightForCars2,
	                reinterpret_cast<void *>(&HookedLightForCars2)) && ok;
	ok = InstallOne(g_peds, "CTrafficLights::LightForPeds", CTrafficLights__LightForPeds,
	                reinterpret_cast<void *>(&HookedLightForPeds)) && ok;
	if (!ok)
		for (const auto &f : HookFailures())
			Log("lights:   %s: %s", f.name.c_str(), f.reason.c_str());
	return ok;
}

void RemoveLightClock() {
	g_peds.Remove();
	g_cars2.Remove();
	g_cars1.Remove();
	g_onSessionClock = false;
}

} // namespace coopiii::game
