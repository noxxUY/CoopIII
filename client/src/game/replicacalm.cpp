#include "replicacalm.h"

#include "addresses.h"
#include "leadcheck.h"
#include "ped.h"
#include "../log.h"

#include <cstring>
#include <windows.h>

namespace coopiii::game {

namespace {

// CPed::SetEvasiveStep(CEntity *reason, uint8 animType), __thiscall, ret 8.
// It opens `cmp [esi+224h],1Fh` (already PED_STEP_AWAY), then IsPedInControl
// (0x004CE6C0), then only refuses for animType 0 when the ped is not the
// player and bRespondsToThreats (byte C +0x156 bit 1) is clear:
//
//   004D30F3  call 004D48E0 / test al,al / jne 004D3108     IsPlayer
//   004D30FC  mov al,[esi+156h] / shr al,1 / and al,1 / jne  bRespondsToThreats
//   004D3108  test bl,bl / je 004D338C                      animType == 0: return
//
// SpawnRemote clears bRespondsToThreats, so the CCarCtrl calls (type 0,
// 0x004195C2 and its two neighbours) never move a remote player. These six
// pass 1 or 2 and do, every one with the stepping ped in ecx out of ebx:
//
//   004C9288, 004C9335   CPed::ProcessControl, a car coming at the ped (push 1)
//   004EBBA6, 004EBFB2   the ped-on-ped collision block before KillPedWithCar (push 1)
//   004EBC1B, 004EC3D3   the same block (push 2)
constexpr uintptr_t CPed__SetEvasiveStep = 0x004D30C0;
constexpr uintptr_t EVASIVE_STEP_SITES[] = {0x004C9288, 0x004C9335, 0x004EBBA6,
                                            0x004EBC1B, 0x004EBFB2, 0x004EC3D3};

// CPed::LookForSexyPeds / LookForSexyCars, __thiscall, no arguments. Both
// open `call IsPedInControl` then `cmp [ebx+224h],2Ch` (PED_DRIVING) and
// both arm m_lookTimer (+0x4CC) with 4000 and 10000 ms (0FA0h at 0x004D4F04,
// 2710h at 0x004D4F38; the cars' 9C40h price test at 0x004D5006). The
// passenger arm of ProcessControl calls them back to back:
//
//   004CB574  call 004D4DF0    LookForSexyPeds
//   004CB57B  call 004D4F50    LookForSexyCars
constexpr uintptr_t CPed__LookForSexyPeds = 0x004D4DF0;
constexpr uintptr_t CPed__LookForSexyCars = 0x004D4F50;
constexpr uintptr_t LOOK_PEDS_SITE        = 0x004CB574;
constexpr uintptr_t LOOK_CARS_SITE        = 0x004CB57B;

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
