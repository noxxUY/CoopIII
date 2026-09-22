#include "seat.h"

#include "addresses.h"
#include "pedanim.h"
#include "vehicle.h"
#include "../client.h"
#include "../log.h"

#include <windows.h>

namespace coopiii::game {

namespace {

void *PlayerPed() {
	return Func<void *(__cdecl *)()>(FindPlayerPed)();
}

int g_seatKey = 'G';

// Edge state for the key. Held across calls, read only from the game thread.
bool g_keyWasDown = false;

bool g_saidSeated   = false;
bool g_saidNoSeat   = false;
bool g_saidNotReady = false;

// The car a pool ref names, or null.
void *VehicleFromHandle(int32_t handle) {
	if (handle < 0)
		return nullptr;
	using GetFn = void *(__cdecl *)(void *, int32_t);
	void *const pool = Global<void *>(CPools__ms_pVehiclePool);
	if (!pool)
		return nullptr;
	return Func<GetFn>(CPools__GetVehicle)(pool, handle);
}

// Which passenger slot holds this ped, or -1. Seat numbering on the wire is
// the driver's seat as 0 and passenger slot n as n + 1, which is how
// SeatRemotePed reads it back on the other side.
int32_t PassengerSeatOf(void *vehicle, void *ped) {
	for (size_t i = 0; i < offs::VEH_MAX_PASSENGERS; ++i) {
		const size_t at = offs::VEH_PASSENGERS + i * sizeof(void *);
		if (Field<void *>(vehicle, at) == ped)
			return static_cast<int32_t>(i) + 1;
	}
	return -1;
}

// Is there room for one more passenger? Asked before warping rather than
// after, because WarpPedIntoCar's passenger arm walks the slots and simply
// does nothing when they are all taken - it still sets bInVehicle and the
// ped's state first, which would leave the player believing they are in a
// car that never gave them a seat. addresses.h, CPed__WarpPedIntoCar.
bool HasFreePassengerSeat(void *vehicle) {
	const uint8_t maxPassengers = Field<uint8_t>(vehicle, offs::VEH_NUM_MAX_PASSENGERS);
	const size_t  seats =
	    maxPassengers < offs::VEH_MAX_PASSENGERS ? maxPassengers : offs::VEH_MAX_PASSENGERS;
	for (size_t i = 0; i < seats; ++i) {
		const size_t at = offs::VEH_PASSENGERS + i * sizeof(void *);
		if (Field<void *>(vehicle, at) == nullptr)
			return true;
	}
	return false;
}

} // namespace

void SetSeatKey(int virtualKey) {
	if (virtualKey > 0 && virtualKey < 256)
		g_seatKey = virtualKey;
}

bool LocalWantsSeatToggle() {
	// GetAsyncKeyState rather than the game's own CPad, deliberately. GTA III
	// composes CPad from DirectInput device state, and this is a key the
	// original game has no binding for, so there is nothing in CPad to read.
	// Polled here rather than hooked: a key with no meaning to the engine
	// needs no hook, and CPad::UpdatePads is somebody else's seam.
	const bool down = (GetAsyncKeyState(g_seatKey) & 0x8000) != 0;
	const bool edge = down && !g_keyWasDown;
	g_keyWasDown    = down;

	// Only when the game has the keyboard. Alt-tabbed, the key belongs to
	// whatever window has focus, and a co-op mod that seats you in a car
	// because you typed a G in a chat window somewhere else is worse than one
	// with no passenger seat at all.
	if (edge && GetForegroundWindow() != GetActiveWindow())
		return false;
	return edge;
}

bool LocalIsPassenger() {
	void *const ped = PlayerPed();
	if (!ped || !Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;
	void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
	if (!car)
		return false;
	return Field<void *>(car, offs::VEH_DRIVER) != ped;
}

int32_t SeatLocalPlayerIn(int32_t vehicleHandle) {
	void *const ped = PlayerPed();
	if (!ped)
		return -1;

	// Already in something. Asking for a seat while sitting in one is not an
	// error worth logging every time somebody leans on the key.
	if (Field<bool>(ped, offs::PED_IN_VEHICLE))
		return -1;

	// A ped that is not standing around cannot be put in a car without
	// fighting whatever it is doing. The engine's own enter path refuses for
	// the same reason.
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD) {
		if (!g_saidNotReady) {
			g_saidNotReady = true;
			Log("seat: asked for a passenger seat while dying or dead, so no");
		}
		return -1;
	}

	void *const vehicle = VehicleFromHandle(vehicleHandle);
	if (!vehicle)
		return -1;

	if (!HasFreePassengerSeat(vehicle)) {
		if (!g_saidNoSeat) {
			g_saidNoSeat = true;
			Log("seat: the nearest car has no free passenger seat, so nobody got "
			    "in. It holds %u passengers",
			    Field<uint8_t>(vehicle, offs::VEH_NUM_MAX_PASSENGERS));
		}
		return -1;
	}

	// The objective first, and it is not ceremony: WarpPedIntoCar reads it to
	// decide which seat to give. Anything that is not the driver's objective
	// takes the passenger arm, which walks to the first free slot itself.
	using ObjFn  = void(__thiscall *)(void *, uint32_t, void *);
	using WarpFn = void(__thiscall *)(void *, void *);
	Func<ObjFn>(CPed__SetObjective)(ped, OBJECTIVE_ENTER_CAR_AS_PASSENGER, vehicle);
	Func<WarpFn>(CPed__WarpPedIntoCar)(ped, vehicle);

	// Which seat it actually gave us. Read back rather than assumed, because
	// the engine chose it and the number is what the session has to be told.
	const int32_t seat = PassengerSeatOf(vehicle, ped);
	if (seat < 0) {
		// It said yes and then seated nobody. Undo what the warp set so the
		// player is not left believing they are in a car with no seat.
		Field<bool>(ped, offs::PED_IN_VEHICLE)   = false;
		Field<void *>(ped, offs::PED_MY_VEHICLE) = nullptr;
		Field<uint32_t>(ped, offs::PED_OBJECTIVE)      = OBJECTIVE_NONE;
		Field<uint32_t>(ped, offs::PED_PREV_OBJECTIVE) = OBJECTIVE_NONE;
		Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE) = nullptr;
		Log("seat: CPed::WarpPedIntoCar took the passenger arm and left us in no "
		    "slot at all; put the player back on the pavement");
		return -1;
	}

	// The objective goes once the seat is taken, for the same reason
	// UnseatRemotePed clears it: a ped left holding ENTER_CAR_AS_PASSENGER
	// and a car pointer keeps trying to carry the objective out.
	Field<uint32_t>(ped, offs::PED_OBJECTIVE)      = OBJECTIVE_NONE;
	Field<uint32_t>(ped, offs::PED_PREV_OBJECTIVE) = OBJECTIVE_NONE;
	Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE) = nullptr;

	if (!g_saidSeated) {
		g_saidSeated = true;
		Log("seat: got into somebody else's car as a passenger, seat %d. The "
		    "driver owns the physics, so nothing about this car goes on the "
		    "wire from here",
		    seat);
	}
	return seat;
}

void AddSeatToBridge(WorldBridge &bridge) {
	bridge.LocalWantsSeatToggle = &LocalWantsSeatToggle;
	bridge.SeatLocalPlayerIn    = &SeatLocalPlayerIn;
	bridge.LocalIsPassenger     = &LocalIsPassenger;
}

} // namespace coopiii::game
