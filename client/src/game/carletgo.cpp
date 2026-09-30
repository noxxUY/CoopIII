// The engine half of game/carletgo.h: the two calls into
// CCarCtrl::PossiblyRemoveVehicle, taken so the CWorld::Remove detour in
// game/population.cpp can tell which car the reaper is dropping.
#include "carletgo.h"

#include "leadcheck.h"
#include "../log.h"

#include <windows.h>

#include <cstring>

namespace coopiii::game {

namespace {

using PossiblyRemoveFn = void(__cdecl *)(void *);

void *g_reaping           = nullptr;
bool  g_callsTaken[2]     = {false, false};
void *g_pedReaping        = nullptr;
bool  g_pedCallTaken      = false;

// Points the `call` at `site` at `to`, only while it still calls `from`.
bool RedirectCall(uintptr_t site, uintptr_t from, uintptr_t to) {
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

// The reaper, as both of its callers call it. Nested calls cannot happen -
// it calls nothing that reaches either caller - but the outer value is put
// back anyway, so a surprise would cost a wrong reason and not a stale one.
void __cdecl PossiblyRemoveVehicleHere(void *vehicle) {
	void *const outer = g_reaping;
	g_reaping         = vehicle;
	Func<PossiblyRemoveFn>(CCarCtrl__PossiblyRemoveVehicle)(vehicle);
	g_reaping = outer;
}

// CPopulation::RemovePed, as ManagePopulation calls it: the ped reaper's one
// way of taking a pedestrian away (carletgo.h).
void __cdecl RemovePedHere(void *ped) {
	void *const outer = g_pedReaping;
	g_pedReaping      = ped;
	Func<PossiblyRemoveFn>(CPopulation__RemovePed)(ped);
	g_pedReaping = outer;
}

bool InstallPedLetGo() {
	if (!g_pedCallTaken)
		g_pedCallTaken = RedirectCall(MANAGE_POPULATION_REMOVE_PED_CALL, CPopulation__RemovePed,
		                              reinterpret_cast<uintptr_t>(&RemovePedHere));
	if (g_pedCallTaken)
		Log("letgo: took the pedestrian reaper's call; a pedestrian of ours it drops beside "
		    "another player is handed to him instead of despawned");
	else
		Log("letgo: FAILED to take the pedestrian reaper's call at 0x%08X; a pedestrian of "
		    "ours our engine drops there is despawned even in front of another player",
		    static_cast<unsigned>(MANAGE_POPULATION_REMOVE_PED_CALL));
	return g_pedCallTaken;
}

void RemovePedLetGo() {
	if (g_pedCallTaken &&
	    RedirectCall(MANAGE_POPULATION_REMOVE_PED_CALL, reinterpret_cast<uintptr_t>(&RemovePedHere),
	                 CPopulation__RemovePed))
		g_pedCallTaken = false;
	g_pedReaping = nullptr;
}

} // namespace

bool InstallCarLetGo() {
	InstallPedLetGo();
	size_t taken = 0;
	for (size_t i = 0; i < 2; ++i) {
		if (!g_callsTaken[i])
			g_callsTaken[i] =
			    RedirectCall(POSSIBLY_REMOVE_VEHICLE_CALLS[i], CCarCtrl__PossiblyRemoveVehicle,
			                 reinterpret_cast<uintptr_t>(&PossiblyRemoveVehicleHere));
		taken += g_callsTaken[i] ? 1 : 0;
	}
	if (taken == 2)
		Log("letgo: took the traffic reaper's two calls; a car of ours it drops beside "
		    "another player is handed to him instead of despawned");
	else
		Log("letgo: FAILED to take %u of the traffic reaper's two calls; a car of ours our "
		    "engine drops there is despawned even in front of another player",
		    static_cast<unsigned>(2 - taken));
	return taken == 2;
}

void RemoveCarLetGo() {
	RemovePedLetGo();
	for (size_t i = 0; i < 2; ++i)
		if (g_callsTaken[i] &&
		    RedirectCall(POSSIBLY_REMOVE_VEHICLE_CALLS[i],
		                 reinterpret_cast<uintptr_t>(&PossiblyRemoveVehicleHere),
		                 CCarCtrl__PossiblyRemoveVehicle))
			g_callsTaken[i] = false;
	g_reaping = nullptr;
}

void *CarBeingReaped() { return g_reaping; }

void *PedBeingReaped() { return g_pedReaping; }

} // namespace coopiii::game
