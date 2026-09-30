// A session car the engine takes away on purpose: the crusher, the military
// crane, Craig's import/export garages, the police and bank-van garage, a
// mission garage, and a safehouse garage storing what is parked in it.
// docs/protocol.md 1.44 is the design; this is what a reader of the code needs.
//
// **Every one of those runs on every machine.** Each engine has its own copy
// of a session car, and each one's crusher scans its whole vehicle pool, each
// one's crane walks its own sectors, and each one pays its own player. So a
// car driven into the crusher paid everybody standing near it, and then came
// back: a session car that leaves the pool without being a wreck is rebuilt
// where it last was (ResolveRemoteVehicle), because until now the only way
// that happened was the engine reaping something it should not have.
//
// **So exactly one machine may let its engine do it** - the one holding the
// car: its driver, else its custodian, else whoever drove it last, else the
// session host (MayRemoveCar). On every other machine the crusher and the
// crane cannot see the car and the safehouse garage leaves it alone. The
// holder's engine does what it always does, pays its own player through its
// own code, and C_VehicleRemoved ends the car everywhere else, which is the
// same as a wreck: gone, not rebuilt.
//
// **A copy is a mission car, and Craig and the safehouse refuse mission
// cars.** Copies stay MISSION_VEHICLE (game/carlife.h has why). Craig's
// "come back when you're not so busy" test reads the byte in an arm that
// destroys nothing, so the holder's copy reads RANDOM for that one call. The
// safehouse is handed the car for real: a copy the holder stores becomes this
// engine's own car (HandCopyToEngine), which is what a stored car is, and the
// session lets go of it.
//
// **Craig's lists and the crane's were already the session's** (C_CarLists,
// protocol 63). With one engine delivering, the model is paid for once, to
// the player who brought it, and the change to the list goes out as any does.
#pragma once

#include <cstdint>

#include "addresses.h"
#include "../client.h"
#include "coopiii/protocol.h"

namespace coopiii::game {

// Installs the call-site redirections. Not fatal: whatever does not install
// stays as it was, which is every machine's engine acting on its own copy.
bool InstallCarRemovalHooks();
void RemoveCarRemovalHooks();
// Adds the removal entries, and the parked-car takeover, to a built bridge.
void AddCarRemovalToBridge(WorldBridge &bridge);

// Called from the CGarage::Update detour (game/garage.cpp) either side of the
// engine's own Update: keeps the crusher off a car this machine does not
// hold, and lets the holder's copy past Craig's mission-car refusal.
void CarRemovalBeforeGarageUpdate(void *garage);
void CarRemovalAfterGarageUpdate(void *garage, uint8_t stateBefore);

// Our engine took this session car away on purpose and it is not to be
// rebuilt. Asked by ResolveRemoteVehicle between the engine doing it and
// Client marking the row.
bool EngineTookCarAway(uint16_t netId);

// A car the automobile constructor locked because of its model (police,
// Enforcer, Rhino: CARLOCK_LOCKED_INITIALLY). The engine only unlocks it when
// its driver gets out through the engine's own exit, which never happens to a
// copy on this machine, so a copy of a car somebody is driving or has driven
// is unlocked by hand - as it already is on the machine that claimed it.
void ReleaseInitialDoorLock(void *vehicle);

// ---- the parts that do not need a running game ----------------------------

// Whether this machine is the one whose engine may take the car away. The
// order is the ownership order everywhere else: a driver, then a custodian,
// then the player who drove it last if he is still here, and for a car nobody
// has ever driven in this session (a joiner's backfill), the host. Never two
// machines at once, as long as they agree on the roster.
inline bool MayRemoveCar(uint8_t driver, uint8_t custodian, uint8_t lastDriver,
                         bool lastDriverHere, uint8_t local, uint8_t host) {
	if (local == INVALID_PLAYER)
		return false;
	if (driver != INVALID_PLAYER)
		return driver == local;
	if (custodian != INVALID_PLAYER)
		return custodian == local;
	if (lastDriver != INVALID_PLAYER && lastDriverHere)
		return lastDriver == local;
	return host == local;
}

// Which reason a garage's own delivery is, by eGarageType. NONE for a type
// that never destroys a car.
inline uint8_t RemovalReasonForGarage(uint8_t type) {
	switch (type) {
	case GARAGE_CRUSHER:             return VEHICLE_REMOVED_CRUSHED;
	case GARAGE_COLLECTCARS_1:
	case GARAGE_COLLECTCARS_2:
	case GARAGE_COLLECTCARS_3:       return VEHICLE_REMOVED_EXPORTED;
	case GARAGE_COLLECTSPECIFICCARS: return VEHICLE_REMOVED_COLLECTED;
	case GARAGE_MISSION:             return VEHICLE_REMOVED_MISSION;
	case GARAGE_HIDEOUT_ONE:
	case GARAGE_HIDEOUT_TWO:
	case GARAGE_HIDEOUT_THREE:       return VEHICLE_REMOVED_STORED;
	default:                         return VEHICLE_REMOVED_NONE;
	}
}

inline bool IsCraigsGarage(uint8_t type) {
	return type >= GARAGE_COLLECTCARS_1 && type <= GARAGE_COLLECTCARS_3;
}

// A generator index on the wire: plus one, so a zeroed body is "none".
inline uint16_t ParkedSlotFor(int32_t generator) {
	return generator < 0 || generator > 0xFFFE ? 0
	                                           : static_cast<uint16_t>(generator + 1);
}
inline int32_t GeneratorForSlot(uint16_t parkedSlot) {
	return parkedSlot == 0 ? -1 : static_cast<int32_t>(parkedSlot) - 1;
}

} // namespace coopiii::game
