#include "liftbridge.h"

#include "addresses.h"
#include "hook/hook.h"
#include "liftbridgetime.h"
#include "log.h"
#include "sessionclock.h"

#include <cstdint>

namespace coopiii::game {
namespace {

Detour g_update;

// Static, no arguments.
using UpdateFn = void(__cdecl *)();
// CPathFind::SetLinksBridgeLights: __thiscall, `ret 14h`.
using SetLinksFn = void(__thiscall *)(void *paths, float x1, float x2, float y1, float y2,
                                      bool enable);

// Once each way in the log, as in trains.cpp.
bool g_onSessionClock = false;

void SetBridgeLinks(bool lit) {
	Func<SetLinksFn>(CPathFind__SetLinksBridgeLights)(
	    Ptr<void>(ThePaths), Global<float>(CBridge__LinksX1), Global<float>(CBridge__LinksX2),
	    Global<float>(CBridge__LinksY1), Global<float>(CBridge__LinksY2), lit);
}

void __cdecl HookedBridgeUpdate() {
	uint32_t session = 0;
	if (!SessionClockNow(session)) {
		if (g_onSessionClock) {
			g_onSessionClock = false;
			Log("liftbridge: back on this machine's own clock");
		}
		// The epoch the session left behind stays, so the bridge carries on
		// from where the session had it rather than jumping back.
		g_update.Original<UpdateFn>()();
		return;
	}

	const uint32_t ours = Global<uint32_t>(CTimer__m_snTimeInMilliseconds);
	// Init writes -1 here and every Update that gets past its entity check
	// writes a height between 0 and 25, so -1 means Init ran since the last
	// one - and lit the links, whatever the bridge is doing.
	const bool initRan = Global<float>(CBridge__OldLift) == -1.0f;

	// The only thing Update reads that is ours to choose. Written every call:
	// CTimer and the session clock don't run at quite the same rate.
	Global<uint32_t>(CBridge__TimeOfBridgeBecomingOperational) =
	    BridgeEpochForSession(ours, session);
	g_update.Original<UpdateFn>()();

	// Without both entities Update returned before touching anything.
	if (!Global<void *>(CBridge__pLiftPart) || !Global<void *>(CBridge__pWeight))
		return;

	const int32_t before = Global<int32_t>(CBridge__OldState);
	const int32_t after  = Global<int32_t>(CBridge__State);
	if (!g_onSessionClock) {
		g_onSessionClock = true;
		Log("liftbridge: on the session clock, %d ms from ours; %u ms into its "
		    "%u ms cycle, state %d (was %d)%s",
		    static_cast<int32_t>(session - ours), BridgePhaseMs(session, 0),
		    BRIDGE_PERIOD_MS, after, before,
		    Global<int32_t>(CStats__CommercialPassed) ? "" : ", locked until Commercial is passed");
	}

	switch (LinksAfterBridgeUpdate(before, after, initRan)) {
	case BridgeLinks::Leave:
		break;
	case BridgeLinks::Light:
		Log("liftbridge: state %d after %d skipped an edge; stopping traffic for the bridge",
		    after, before);
		SetBridgeLinks(true);
		break;
	case BridgeLinks::PutOut:
		Log("liftbridge: state %d after %d%s; letting traffic over the bridge", after, before,
		    initRan ? " with the links just reset" : " skipped an edge");
		SetBridgeLinks(false);
		break;
	}
}

} // namespace

bool InstallLiftBridgeClock() {
	if (!g_update.Install("CBridge::Update", reinterpret_cast<void *>(CBridge__Update),
	                      reinterpret_cast<void *>(&HookedBridgeUpdate))) {
		Log("liftbridge: FAILED to hook CBridge::Update at 0x%08X; every machine "
		    "will run its own bridge",
		    CBridge__Update);
		for (const auto &f : HookFailures())
			Log("liftbridge:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}
	Log("liftbridge: hooked CBridge::Update at 0x%08X", CBridge__Update);
	return true;
}

void RemoveLiftBridgeClock() {
	if (!g_update.IsInstalled())
		return;
	g_update.Remove();
	g_onSessionClock = false;
}

} // namespace coopiii::game
