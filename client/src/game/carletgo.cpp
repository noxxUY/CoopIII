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

} // namespace

bool InstallCarLetGo() {
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
	for (size_t i = 0; i < 2; ++i)
		if (g_callsTaken[i] &&
		    RedirectCall(POSSIBLY_REMOVE_VEHICLE_CALLS[i],
		                 reinterpret_cast<uintptr_t>(&PossiblyRemoveVehicleHere),
		                 CCarCtrl__PossiblyRemoveVehicle))
			g_callsTaken[i] = false;
	g_reaping = nullptr;
}

void *CarBeingReaped() { return g_reaping; }

} // namespace coopiii::game
