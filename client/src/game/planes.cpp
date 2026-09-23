#include "planes.h"

#include "addresses.h"
#include "hook/hook.h"
#include "log.h"
#include "planetime.h"
#include "sessionclock.h"

namespace coopiii::game {
namespace {

Detour g_update;

// Static member, no arguments, like UpdateTrains.
using UpdatePlanesFn = void(__cdecl *)();

// Once each way in the log, as in trains.cpp.
bool g_onSessionClock = false;

void __cdecl HookedUpdatePlanes() {
	uint32_t session = 0;
	if (!SessionClockNow(session)) {
		if (g_onSessionClock) {
			g_onSessionClock = false;
			Log("planes: back on this machine's own clock");
		}
		g_update.Original<UpdatePlanesFn>()();
		return;
	}

	uint32_t &timer   = Global<uint32_t>(CTimer__m_snTimeInMilliseconds);
	uint32_t &drugRun = Global<uint32_t>(CPlane__CesnaMissionStartTime);
	uint32_t &dropOff = Global<uint32_t>(CPlane__DropOffCesnaMissionStartTime);
	const uint32_t ours = timer;

	if (!g_onSessionClock) {
		g_onSessionClock = true;
		Log("planes: on the session clock, %d ms from ours; airliner 0 is "
		    "%u ms into its %u ms loop",
		    static_cast<int32_t>(session - ours), PlaneCycleMs(session, 0),
		    PLANE_PERIOD_MS);
	}

	// Same swap as the trains: UpdatePlanes reads the global once and writes
	// that value back (addresses.h). The Cessna start times go with it, both
	// of them every time, whether or not a Cessna is flying: UpdatePlanes only
	// reads them, and moving one that nothing reads costs nothing.
	timer   = session;
	drugRun = MissionStartOnSessionClock(drugRun, ours, session);
	dropOff = MissionStartOnSessionClock(dropOff, ours, session);
	g_update.Original<UpdatePlanesFn>()();
	dropOff = MissionStartOnOurClock(dropOff, ours, session);
	drugRun = MissionStartOnOurClock(drugRun, ours, session);
	timer   = ours;
}

} // namespace

bool InstallPlaneClock() {
	if (!g_update.Install("CPlane::UpdatePlanes",
	                      reinterpret_cast<void *>(CPlane__UpdatePlanes),
	                      reinterpret_cast<void *>(&HookedUpdatePlanes))) {
		Log("planes: FAILED to hook CPlane::UpdatePlanes at 0x%08X; every "
		    "machine will fly its own planes",
		    CPlane__UpdatePlanes);
		for (const auto &f : HookFailures())
			Log("planes:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}
	Log("planes: hooked CPlane::UpdatePlanes at 0x%08X", CPlane__UpdatePlanes);
	return true;
}

void RemovePlaneClock() {
	if (!g_update.IsInstalled())
		return;
	g_update.Remove();
	g_onSessionClock = false;
}

} // namespace coopiii::game
