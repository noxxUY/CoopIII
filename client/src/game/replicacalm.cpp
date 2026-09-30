#include "replicacalm.h"

#include "addresses.h"
#include "leadcheck.h"
#include "ped.h"
#include "../log.h"

#include <cstring>
#include <windows.h>

namespace coopiii::game {

namespace {

bool IsRemotePlayer(void *ped) {
	uint16_t netId = 0;
	return RemotePlayerForPed(ped, netId);
}

bool g_saidStepRefused = false;
bool g_saidLookRefused = false;

void __fastcall EvasiveStep(void *self, void * /*edx*/, void *reason, uint32_t animType) {
	if (IsRemotePlayer(self)) {
		if (!g_saidStepRefused) {
			g_saidStepRefused = true;
			Log("calm: a remote player's copy was bumped (step kind %u) and stays where "
			    "its owner put it",
			    animType & 0xFF);
		}
		return;
	}
	using Fn = void(__thiscall *)(void *, void *, uint32_t);
	Func<Fn>(CPed__SetEvasiveStep)(self, reason, animType);
}

void LookRefused() {
	if (!g_saidLookRefused) {
		g_saidLookRefused = true;
		Log("calm: a remote passenger's copy was about to look round at somebody "
		    "passing; it looks where its owner does");
	}
}

void __fastcall LookForPeds(void *self, void * /*edx*/) {
	if (IsRemotePlayer(self)) {
		LookRefused();
		return;
	}
	Func<void(__thiscall *)(void *)>(CPed__LookForSexyPeds)(self);
}

void __fastcall LookForCars(void *self, void * /*edx*/) {
	if (IsRemotePlayer(self)) {
		LookRefused();
		return;
	}
	Func<void(__thiscall *)(void *)>(CPed__LookForSexyCars)(self);
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

struct Site {
	uintptr_t at;
	uintptr_t engine;
	uintptr_t ours;
	bool      done;
};

Site g_sites[] = {
	{EVASIVE_STEP_SITES[0], CPed__SetEvasiveStep, reinterpret_cast<uintptr_t>(&EvasiveStep), false},
	{EVASIVE_STEP_SITES[1], CPed__SetEvasiveStep, reinterpret_cast<uintptr_t>(&EvasiveStep), false},
	{EVASIVE_STEP_SITES[2], CPed__SetEvasiveStep, reinterpret_cast<uintptr_t>(&EvasiveStep), false},
	{EVASIVE_STEP_SITES[3], CPed__SetEvasiveStep, reinterpret_cast<uintptr_t>(&EvasiveStep), false},
	{EVASIVE_STEP_SITES[4], CPed__SetEvasiveStep, reinterpret_cast<uintptr_t>(&EvasiveStep), false},
	{EVASIVE_STEP_SITES[5], CPed__SetEvasiveStep, reinterpret_cast<uintptr_t>(&EvasiveStep), false},
	{LOOK_PEDS_SITE, CPed__LookForSexyPeds, reinterpret_cast<uintptr_t>(&LookForPeds), false},
	{LOOK_CARS_SITE, CPed__LookForSexyCars, reinterpret_cast<uintptr_t>(&LookForCars), false},
};

} // namespace

bool InstallReplicaCalm() {
	int done = 0, total = 0;
	for (Site &s : g_sites) {
		++total;
		if (!s.done)
			s.done = Redirect(s.at, s.engine, s.ours);
		if (s.done)
			++done;
		else
			Log("calm: FAILED to redirect the call at 0x%08X; it no longer calls 0x%08X",
			    static_cast<unsigned>(s.at), static_cast<unsigned>(s.engine));
	}
	Log("calm: %d of %d side-step and look-around calls come to us first", done, total);
	return done == total;
}

void RemoveReplicaCalm() {
	for (Site &s : g_sites)
		if (s.done && Redirect(s.at, s.ours, s.engine))
			s.done = false;
}

} // namespace coopiii::game
