// What the session's mission takes that the session already has.
//
// The owner's mission makes its cars and its people inside its own
// instructions, and population.cpp announces whatever such an instruction
// adds to the world as the mission's own (docs/missions.md 5.3). But an
// instruction can add back something it never made. SET_PLAYER_COORDINATES
// with the owner sitting in a car teleports the car, and CAutomobile::Teleport
// is CWorld::Remove and CWorld::Add. When the car is another player's, or the
// owner's copy of it, the add looked exactly like a new mission car: a session
// car is a MISSION_VEHICLE on every machine (carlife.cpp), and so is a
// replica. It went out as a new car under a new name, and the player whose car
// it was saw it twice (Taking Out The Laundry, the owner riding in a guest's
// car: docs/missions.md 15, "What the mission takes that is already there").
//
// So a car or a pedestrian the session already names is never the mission's:
// a session car (anybody's, the local player's own included), a copy of
// somebody else's traffic, a remote player or a copy of somebody else's
// pedestrian. The mission still reaches it by the name it already has
// (game/mission.cpp, NetIdForCar), and its cleanup never takes it.
//
// And the other end, for a machine that hears of a mission car anyway (an
// older owner): a mission car of the same model whose host puts it where a
// session car of ours stands is that session car. Two cars of one model
// cannot be closer than this, centre to centre, without being inside each
// other, so it is never built here and the mission's name for it reaches the
// session car instead (Client::SameAsSessionCar).
//
// Pure: game/population.cpp and Client decide with it, and
// tools/clienttest/missiontake.cpp walks it.
#pragma once

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

// What an entity added to the world inside one of the session's mission's
// instructions is, for hosting.
enum class MissionAdd : uint8_t {
	NotMission,   // not added by the mission, or not one of its kind
	Mission,      // the mission's own: announced with the mission bit
	OnSession,    // the session has it already under a name of its own
};

// A car: `madeByMission` is VehicleCreatedBy == MISSION_VEHICLE, `onSession`
// is a session car or a copy of somebody else's traffic here.
constexpr MissionAdd MissionCarAdd(bool inMissionInstruction, bool madeByMission,
                                   bool onSession) {
	if (!inMissionInstruction || !madeByMission)
		return MissionAdd::NotMission;
	return onSession ? MissionAdd::OnSession : MissionAdd::Mission;
}

// A pedestrian: `remotePlayer` is another player's ped here, `replica` a copy
// of somebody else's pedestrian. The local player never reaches this.
constexpr MissionAdd MissionPedAdd(bool inMissionInstruction, bool madeByMission,
                                   bool remotePlayer, bool replica) {
	if (!inMissionInstruction || !madeByMission)
		return MissionAdd::NotMission;
	return remotePlayer || replica ? MissionAdd::OnSession : MissionAdd::Mission;
}

// Closer than this, centre to centre, two cars of one model are one car: about
// half a car's width, so two parked door to door are still two.
constexpr float MISSION_CAR_SAME_AS_SESSION_M = 1.0f;

inline bool MissionCarIsSessionCar(uint16_t missionModel, const Vec3 &missionAt,
                                   uint16_t sessionModel, const Vec3 &sessionAt) {
	if (missionModel != sessionModel)
		return false;
	const float dx = missionAt.x - sessionAt.x;
	const float dy = missionAt.y - sessionAt.y;
	const float dz = missionAt.z - sessionAt.z;
	return dx * dx + dy * dy + dz * dz <
	       MISSION_CAR_SAME_AS_SESSION_M * MISSION_CAR_SAME_AS_SESSION_M;
}

} // namespace coopiii::game
