// The tank's cannon and the fire truck's water cannon are the driver's.
//
// CAutomobile::TankControl and FireTruckControl each open by asking whether
// the car is FindPlayerVehicle, and only then read the pad (addresses.h, "who
// works the gun"). FindPlayerVehicle names the car the local player sits in,
// whatever the seat. So on a passenger's machine the tank he rides in turned
// and fired off his pad, its shell went out as his explosion, and the fire
// truck sprayed off his fire button and went out as the truck's jet. With the
// passenger's free aim on the same left button, every round he fired from
// the window fired the cannon too.
//
// Both of those calls are taken. They answer with the car only when the
// local player is at its wheel, and nothing otherwise: the tank then does
// what it does for any car the player is not in (nothing, and the driver's
// aim off the wire is what the turret shows), and the fire truck goes to its
// NPC arm, where game/emergency.cpp refuses a jet from a truck another
// machine moves and sprays the driver's instead.
#pragma once

#include <cstdint>

namespace coopiii::game {

// The car whose gun the local pad works: the car FindPlayerVehicle gave,
// when the local player is its driver. Null for a passenger in anybody's car,
// somebody else's or the traffic's, and for a player on foot.
inline void *CarGunForPad(void *playerVehicle, const void *driverOfIt, const void *playerPed) {
	if (!playerVehicle || !playerPed || driverOfIt != playerPed)
		return nullptr;
	return playerVehicle;
}

// Whether the local player is at `car`'s wheel, as its CVehicle::m_pDriver
// has it. The test CarGunForPad makes, for anybody else who needs it.
bool LocalPlayerDrives(const void *car);

// Takes both calls, each only while it still calls FindPlayerVehicle. False
// when either could not be taken; the one that was stays taken, and the log
// names the other.
bool InstallCarGunGate();
void RemoveCarGunGate();

} // namespace coopiii::game
