#include "cargun.h"

#include "addresses.h"
#include "leadcheck.h"
#include "../log.h"

#include <windows.h>

#include <cstring>

namespace coopiii::game {

namespace {

using CarFn = void *(__cdecl *)();

bool g_saidPassenger = false;

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

void *PlayerPed() { return Func<CarFn>(FindPlayerPed)(); }

// In place of FindPlayerVehicle, at the top of TankControl and
// FireTruckControl. __cdecl with nothing to pass, like the original; ebx,
// which holds the car in both, is callee-saved.
void *__cdecl CarForPad() {
	void *const car = Func<CarFn>(FindPlayerVehicle)();
	if (!car)
		return nullptr;
	void *const ped  = PlayerPed();
	void *const ours = CarGunForPad(car, Field<void *>(car, offs::VEH_DRIVER), ped);
	if (!ours && !g_saidPassenger) {
		g_saidPassenger = true;
		Log("cargun: we sit in a tank or fire truck we are not driving; its gun is its "
		    "driver's, and our pad leaves it alone (said once)");
	}
	return ours;
}

struct Site {
	uintptr_t   at;
	const char *what;
	bool        taken;
};

Site g_sites[] = {
    {FIRE_TRUCK_PLAYER_CAR_CALL, "the fire truck's", false},
    {TANK_PLAYER_CAR_CALL, "the tank's", false},
};

} // namespace

bool LocalPlayerDrives(const void *car) {
	if (!car)
		return false;
	void *const ped = PlayerPed();
	return ped != nullptr && Field<void *>(const_cast<void *>(car), offs::VEH_DRIVER) == ped;
}

bool InstallCarGunGate() {
	bool all = true;
	for (Site &s : g_sites) {
		if (!s.taken)
			s.taken = RedirectCall(s.at, FindPlayerVehicle, reinterpret_cast<uintptr_t>(&CarForPad));
		if (!s.taken) {
			all = false;
			Log("cargun: FAILED to take %s FindPlayerVehicle call at 0x%08X; a passenger "
			    "there still works that gun from his own pad",
			    s.what, static_cast<unsigned>(s.at));
		}
	}
	if (all)
		Log("cargun: the tank's cannon and the fire truck's water cannon answer to the "
		    "driver's pad only");
	return all;
}

void RemoveCarGunGate() {
	for (Site &s : g_sites)
		if (s.taken && RedirectCall(s.at, reinterpret_cast<uintptr_t>(&CarForPad), FindPlayerVehicle))
			s.taken = false;
}

} // namespace coopiii::game
