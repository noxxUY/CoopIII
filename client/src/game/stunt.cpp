#include "stunt.h"

#include "leadcheck.h"
#include "vehicle.h"
#include "../log.h"

#include <cstring>

#include <windows.h>

namespace coopiii::game {

namespace {

using CompareFn = void(__thiscall *)(void *script, uint32_t result);
using GetCarFn  = void *(__cdecl *)(int32_t handle);

bool g_installed        = false;
bool g_saidRefused      = false;
bool g_saidRiding       = false;
bool g_saidOwnCarInAir  = false;

// In the car and not at its wheel.
bool RidingIn(void *car) {
	void *const ped = Func<void *(__cdecl *)()>(FindPlayerPed)();
	return car && ped && Field<bool>(ped, offs::PED_IN_VEHICLE) &&
	       Field<void *>(ped, offs::PED_MY_VEHICLE) == car &&
	       Field<void *>(car, offs::VEH_DRIVER) != ped;
}

// 0x00442925. UpdateCompareFlag is __thiscall with the one byte pushed as a
// dword, `ret 4`; __fastcall puts `this` in ecx and leaves the argument where
// the caller pushed it. The NOT is the engine's own to apply, after us.
void __fastcall AirborneAnswer(void *script, void * /*edx*/, uint32_t result) {
	const bool airborne = (result & 0xFF) != 0;
	if (airborne) {
		const int32_t handle = *Ptr<int32_t>(CTheScripts__ScriptParams);
		void *const   car    = Func<GetCarFn>(CPools__GetVehicle)(handle);
		const CarOwner owner = CarOwnerHere(car);
		const bool     riding = RidingIn(car);
		if (!AirborneCounts(true, owner, riding)) {
			result &= ~0xFFu;
			if (riding && owner == CarOwner::Local) {
				if (!g_saidRiding) {
					g_saidRiding = true;
					Log("stunt: the script asked whether the car we ride in is in the air; "
					    "a jump is its driver's, so the answer is no (said once)");
				}
			} else if (!g_saidRefused) {
				g_saidRefused = true;
				Log("stunt: the script asked whether a car we are in is in the air, and it "
				    "is somebody else's car to simulate (%s), so the answer is no. A copy "
				    "that touches nothing is a corrected or parked one, not a jump",
				    owner == CarOwner::RemoteDriver      ? "another player drives it"
				    : owner == CarOwner::RemoteCustodian ? "another player is settling it"
				    : owner == CarOwner::RemoteHost      ? "another machine's traffic"
				                                         : "a session car nobody holds");
			}
		} else if (!g_saidOwnCarInAir) {
			g_saidOwnCarInAir = true;
			Log("stunt: a car this machine simulates left the ground; the stunt script "
			    "hears it as it always did (said once)");
		}
	}
	Func<CompareFn>(CRunningScript__UpdateCompareFlag)(script, result);
}

// Points the call at `site` at `to`, only while it still calls `from`.
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

} // namespace

bool InstallStuntGuard() {
	if (g_installed)
		return true;
	g_installed = Redirect(IS_CAR_IN_AIR_PROPER_ANSWER, CRunningScript__UpdateCompareFlag,
	                       reinterpret_cast<uintptr_t>(&AirborneAnswer));
	if (g_installed)
		Log("stunt: IS_CAR_IN_AIR_PROPER's answer at 0x%08X comes to us first; a car "
		    "somebody else simulates is never in the air to the stunt scripts",
		    static_cast<unsigned>(IS_CAR_IN_AIR_PROPER_ANSWER));
	else
		Log("stunt: FAILED to redirect IS_CAR_IN_AIR_PROPER's answer at 0x%08X; it no "
		    "longer calls 0x%08X, so riding in a teammate's car can pay a stunt bonus",
		    static_cast<unsigned>(IS_CAR_IN_AIR_PROPER_ANSWER),
		    static_cast<unsigned>(CRunningScript__UpdateCompareFlag));
	return g_installed;
}

void RemoveStuntGuard() {
	if (g_installed &&
	    Redirect(IS_CAR_IN_AIR_PROPER_ANSWER, reinterpret_cast<uintptr_t>(&AirborneAnswer),
	             CRunningScript__UpdateCompareFlag))
		g_installed = false;
}

} // namespace coopiii::game
