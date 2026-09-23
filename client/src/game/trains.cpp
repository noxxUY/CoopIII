#include "trains.h"

#include "addresses.h"
#include "hook/hook.h"
#include "log.h"
#include "sessionclock.h"
#include "traintime.h"

namespace coopiii::game {
namespace {

Detour g_update;

// Static member, no arguments, like CTimer::Update.
using UpdateTrainsFn = void(__cdecl *)();

// So the log says once that the trains moved over, and once that they came
// back, and not sixty times a second.
bool g_onSessionClock = false;

void __cdecl HookedUpdateTrains() {
	uint32_t session = 0;
	if (!SessionClockNow(session)) {
		if (g_onSessionClock) {
			g_onSessionClock = false;
			Log("trains: back on this machine's own clock");
		}
		g_update.Original<UpdateTrainsFn>()();
		return;
	}

	uint32_t &timer = Global<uint32_t>(CTimer__m_snTimeInMilliseconds);
	const uint32_t ours = timer;

	if (!g_onSessionClock) {
		g_onSessionClock = true;
		Log("trains: on the session clock, %d ms from ours; El train 0 is "
		    "%u ms into its %u ms loop",
		    static_cast<int32_t>(session - ours),
		    TrainCycleMs(session, EL_PERIOD_MS, 0), EL_PERIOD_MS);
	}

	// UpdateTrains reads this global once per line and writes the same value
	// back at the end of each (addresses.h). Nothing in between uses it for
	// anything but the trains, so only they see the swap, and CTimer gets
	// its own value back right after.
	timer = session;
	g_update.Original<UpdateTrainsFn>()();
	timer = ours;
}

} // namespace

bool InstallTrainClock() {
	if (!g_update.Install("CTrain::UpdateTrains",
	                      reinterpret_cast<void *>(CTrain__UpdateTrains),
	                      reinterpret_cast<void *>(&HookedUpdateTrains))) {
		Log("trains: FAILED to hook CTrain::UpdateTrains at 0x%08X; every "
		    "machine will run its own trains",
		    CTrain__UpdateTrains);
		for (const auto &f : HookFailures())
			Log("trains:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}
	Log("trains: hooked CTrain::UpdateTrains at 0x%08X", CTrain__UpdateTrains);
	return true;
}

void RemoveTrainClock() {
	if (!g_update.IsInstalled())
		return;
	g_update.Remove();
	g_onSessionClock = false;
}

} // namespace coopiii::game
