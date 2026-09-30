#include "copcrime.h"

#include "population.h"
#include "../hook/hook.h"
#include "../log.h"

#include <cstring>

namespace coopiii::game {

namespace {

Detour g_report;

// The bool travels as the whole dword the caller pushed; the function only
// reads its low byte (`mov al,[esp+34h]`), so it is passed on untouched.
using ReportFn = void(__cdecl *)(int32_t, uintptr_t, uint32_t);

bool     g_saidReplica = false;
bool     g_saidCop[EVENT_LAST_REPORTED + 1] = {};
uint32_t g_corrected = 0;

void *PlayerWanted() {
	void *const ped = Func<void *(__cdecl *)()>(FindPlayerPed)();
	return ped ? Field<void *>(ped, offs::PLAYER_PED_WANTED) : nullptr;
}

const char *Verb(int32_t event) {
	switch (event) {
	case EVENT_ASSAULT_POLICE:  return "hit";
	case EVENT_HIT_AND_RUN_COP: return "ran over";
	case EVENT_SHOOT_COP:       return "shot or blew up";
	case EVENT_COP_SET_ON_FIRE: return "set fire to";
	default:                    return "hurt";
	}
}

void __cdecl HookedReportCrimeForEvent(int32_t type, uintptr_t crimeId, uint32_t copsDontCare) {
	const ReportFn original = g_report.Original<ReportFn>();

	uint16_t netId    = INVALID_NETID;
	uint8_t  hostType = 0;
	if (!IsCivilianPedEvent(type) ||
	    !AmbientReplicaHostPedType(reinterpret_cast<const void *>(crimeId), netId, hostType)) {
		original(type, crimeId, copsDontCare);
		return;
	}

	// Once, whatever the host said: proof the path is reached at all.
	if (!g_saidReplica) {
		g_saidReplica = true;
		Log("wanted: first crime of ours against another machine's pedestrian - "
		    "event %d on ped net %u, whose host made it ped type %u",
		    type, netId, hostType);
	}

	const int32_t asHost = EventAgainstReplica(type, hostType);
	if (asHost == type) {
		original(type, crimeId, copsDontCare);
		return;
	}

	++g_corrected;
	void *const   wanted      = PlayerWanted();
	const int32_t starsBefore = wanted ? Field<int32_t>(wanted, offs::WANTED_LEVEL) : -1;
	const int32_t chaosBefore = wanted ? Field<int32_t>(wanted, offs::WANTED_CHAOS) : -1;

	original(asHost, crimeId, copsDontCare);

	if (g_saidCop[asHost])
		return;
	g_saidCop[asHost] = true;
	const int32_t starsAfter = wanted ? Field<int32_t>(wanted, offs::WANTED_LEVEL) : -1;
	const int32_t chaosAfter = wanted ? Field<int32_t>(wanted, offs::WANTED_CHAOS) : -1;
	Log("wanted: we %s a policeman another machine hosts (ped net %u) - event %d "
	    "became %d, crime %d instead of %d. Stars %d -> %d, chaos %d -> %d. "
	    "%u corrected so far; this kind will not be said again",
	    Verb(asHost), netId, type, asHost, CrimeForEvent(asHost), CrimeForEvent(type),
	    starsBefore, starsAfter, chaosBefore, chaosAfter, g_corrected);
}

} // namespace

bool InstallCopCrimeHook() {
	if (g_report.IsInstalled())
		return true;
	if (std::memcmp(Ptr<uint8_t>(CEventList__ReportCrimeForEvent),
	                REPORT_CRIME_FOR_EVENT_PROLOGUE,
	                sizeof(REPORT_CRIME_FOR_EVENT_PROLOGUE)) != 0) {
		Log("wanted: CEventList::ReportCrimeForEvent at 0x%08X is not the retail "
		    "prologue, left alone; a policeman another machine hosts counts as a "
		    "civilian when we hurt him",
		    static_cast<unsigned>(CEventList__ReportCrimeForEvent));
		return false;
	}
	if (!g_report.Install("CEventList::ReportCrimeForEvent",
	                      reinterpret_cast<void *>(CEventList__ReportCrimeForEvent),
	                      reinterpret_cast<void *>(&HookedReportCrimeForEvent))) {
		Log("wanted: FAILED to hook CEventList::ReportCrimeForEvent at 0x%08X; a "
		    "policeman another machine hosts counts as a civilian when we hurt him",
		    static_cast<unsigned>(CEventList__ReportCrimeForEvent));
		for (const auto &f : HookFailures())
			Log("wanted:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}
	Log("wanted: hooked CEventList::ReportCrimeForEvent at 0x%08X; hurting a "
	    "policeman another machine hosts is reported as hurting a policeman",
	    static_cast<unsigned>(CEventList__ReportCrimeForEvent));
	return true;
}

void RemoveCopCrimeHook() {
	g_report.Remove();
}

} // namespace coopiii::game
