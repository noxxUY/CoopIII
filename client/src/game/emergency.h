// Emergency services on every screen: a medic's revive and a fire truck's
// water cannon. docs/protocol.md 1.37 is the design; game/emergencyaddr.h has
// every address with the instruction that proves it.
//
// **The revive.** A medic is a pedestrian, so his NPC logic runs on the one
// machine that hosts him, and the pedestrian he treats is often somebody
// else's. So:
//
//   - a replica's corpse is made an accident here, the way its host's engine
//     makes its own (ReportAccident, with the replica's two refusing bytes
//     lifted for the call), and MedicAI's three questions to
//     FindNearestAccident see replicas for the length of the call;
//   - the medic's CPR runs on whatever copy is in front of him, and the one
//     call in the image a medic stands anybody up with, SetGetUp at
//     0x004C3B33, is taken: whoever he stood up goes to the session
//     (C_PedRevive), the pedestrian's host included;
//   - every other machine stands its copy up with the medic's own
//     instructions, in the medic's own order.
//
// **The water cannon.** CAutomobile::FireTruckControl's one call into
// CWaterCannons::UpdateOne (0x00522B27) is taken:
//
//   - on the machine that aims the truck - its driver's, or for a truck
//     nobody drives the host of that traffic car - the jet goes through and
//     out on the wire in the truck's own frame (cannonsync.h);
//   - on a copy another machine moves, the local engine's jet is refused,
//     and the jet off the wire is sprayed instead, every frame;
//   - what the water does is each engine's own, about what it owns: it puts
//     out fires on this machine's own things and on the pavement, and knocks
//     down this machine's player and pedestrians. Somebody else's pedestrian,
//     player or car is left alone here; its owner's copy of the same jet
//     decides, and its stream brings the result.
#pragma once

#include "emergencyaddr.h"
#include "../client.h"

#include <cstdint>

namespace coopiii::game {

// Takes the eight call sites. Not fatal: each one that fails leaves its half
// the way it was, and the log names it.
bool InstallEmergencyHooks();
void RemoveEmergencyHooks();

// Fills the bridge's emergency entries and its CannonBridge.
void AddEmergencyToBridge(WorldBridge &bridge);

// ---- the rules, pure so tools/clienttest walks them ---------------------------

// FireTruckControl's jet, on this machine's copy of a truck. The local
// driver's always runs: his is the truck's jet. `localDriver` is the car's
// m_pDriver being our ped, never FindPlayerVehicle, which names a passenger's
// truck as well (game/cargun.h). Any other jet runs on a truck the session
// has no name for or that this machine moves, and is refused on one another
// machine moves - a passenger's included; its jet comes off the wire.
inline constexpr bool CannonInputMayRun(bool localDriver, bool sessionCar, bool othersMoveIt) {
	return localDriver || !(sessionCar && othersMoveIt);
}

// Who the water may knock down here: anybody but another player's ped and a
// replica of somebody else's pedestrian.
inline constexpr bool WaterMayMovePed(bool remotePlayer, bool replica) {
	return !remotePlayer && !replica;
}

// Which fires the water may put out here. A fire on the pavement is every
// machine's own copy of one lit at the same place, and each machine's copy of
// the jet puts its own out. A fire on a ped or a car is its owner's.
enum class FireOn : uint8_t { Nothing, OurThing, SomebodyElses };

inline constexpr bool WaterMayPutOut(FireOn on) { return on != FireOn::SomebodyElses; }

// What a revived pedestrian goes back to once he is up. The medic's own choice
// for a pedestrian this machine hosts, and nothing for a replica: a replica
// holding PED_NONE is set idle by RestorePreviousState's MISSION_CHAR arm,
// where WANDER_PATH would have it pick a path of its own and walk it.
inline constexpr uint32_t ReviveLastState(bool replica) {
	return replica ? PEDSTATE_NONE : MEDIC_REVIVE_LAST_STATE;
}

// A dead pedestrian's own state, the only one a revive is for.
inline constexpr bool DeadForMedic(uint32_t pedState) {
	return pedState == PEDSTATE_DIE || pedState == PEDSTATE_DEAD;
}

} // namespace coopiii::game
