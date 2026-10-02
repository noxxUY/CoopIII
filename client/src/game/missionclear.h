// What the session's mission lets go of, on every machine.
//
// The owner's mission makes its pedestrians and cars on its owner's machine,
// and every other machine holds a copy marked AMBIENT_MISSION: built wherever
// it stands, never faded, never handed on, because the mission's instructions
// name it (docs/missions.md 5.3). The owner's engine is the one that decides
// when it stops being the mission's, and it does that three ways:
//
//   - it takes it away: DELETE_CAR and DELETE_CHAR, a CLEAR_AREA, a failed
//     mission's cars cleared for the retry, the crusher, the reapers once it
//     is nobody's. Each of those reaches the others as a despawn (the sweep
//     in population.cpp sees a hosted entity gone from its pool);
//   - it stops holding it as the mission's and keeps it: the script marks it
//     no longer needed, or the mission's cleanup on a pass or a fail does that
//     to everything the mission still had (CTheScripts::CleanUpThisVehicle and
//     CleanUpThisPed, re3 Script5.cpp: created by RANDOM from then on, a car
//     unlocked). Nothing travelled for that, and every other machine kept its
//     copy as the mission's for ever, a car standing in some street the owner
//     had left, built on the far side of the city;
//   - it never says: a despawn that went missing, or an entity the owner's
//     engine took some way nobody heard of. The copy stayed.
//
// So the owner says when its engine stops holding one as the mission's
// (C_MissionRelease), and from then on every copy is ordinary crowd under the
// rules crowd follows. After its mission ends it says once more what it still
// holds, and the session takes everything else of the mission's away from
// every machine: a retry starts with nothing of the last try's.
//
// A copy that goes because the owner's mission let go of it fades out on a
// watcher's screen and goes at once off it (game/crowdfade.h), the way the
// engine takes its own crowd. A car the local player is in or getting in or
// out of here is handed to this engine instead, as it always was; but a car
// he only sat in once is not, which it used to be: CPed::m_pMyVehicle stays
// on the last car a ped left (re3 Ped.cpp, PedSetOutCarCB clears bInVehicle
// and leaves the pointer), so every car a player had ever got out of was
// kept on his screen as his own engine's when its owner took it away (the
// crushed car of Dead Skunk In The Trunk stayed on the owner's screen).
//
// Pure: game/population.cpp, game/teardown.cpp and Client decide with it,
// and tools/clienttest/missionclear.cpp walks it.
#pragma once

#include "addresses.h"

#include <cstdint>

namespace coopiii::game {

// Is the local player in this car, or on his way in or out of it? Not
// m_pMyVehicle alone, which still names the last car he left: bInVehicle, or
// one of the states a walk to a door, an entry, a jack or an exit passes
// through with m_pMyVehicle set (addresses.h, PEDSTATE_SEEK_CAR and the four
// after PEDSTATE_DRAG_FROM_CAR).
constexpr bool LocalAboardCar(bool myVehicleIsThis, bool inVehicle, uint32_t pedState) {
	if (!myVehicleIsThis)
		return false;
	return inVehicle || pedState == PEDSTATE_SEEK_CAR || pedState == PEDSTATE_CARJACK ||
	       pedState == PEDSTATE_DRAG_FROM_CAR || pedState == PEDSTATE_ENTER_CAR ||
	       pedState == PEDSTATE_EXIT_CAR;
}

// What the owner does with one pedestrian or car it hosts.
enum class MissionHold : uint8_t {
	Keep,          // not the mission's on the session, or still the mission's
	Release,       // the session is to hear it is ordinary crowd now
	WaitForName,   // let go of, and its name has not come back yet
};

// `asMission` is how the session knows it (HostedCar::mission),
// `stillMissions` whether the engine still has it as created by the mission
// (MISSION_VEHICLE, MISSION_CHAR), `missionOver` the owner's mission ended:
// then everything it still hosts as the mission's is let go of.
constexpr MissionHold MissionEntityHold(bool asMission, bool named, bool stillMissions,
                                        bool missionOver) {
	if (!asMission)
		return MissionHold::Keep;
	if (stillMissions && !missionOver)
		return MissionHold::Keep;
	return named ? MissionHold::Release : MissionHold::WaitForName;
}

// The last batch after the owner's mission ended waits this long at most for
// a name still on its way back, so that what the session hears of it is a
// release and not a gone; past it, it goes out anyway.
constexpr uint32_t MISSION_FINAL_WAIT_MS = 2000;

constexpr bool FinalReleaseReady(uint32_t waitingForNames, uint32_t waitedMs) {
	return waitingForNames == 0 || waitedMs >= MISSION_FINAL_WAIT_MS;
}

// A row of S_MissionRelease applies to a copy here only when the copy is the
// releasing player's: a name the session has given to somebody else's since
// is not touched.
constexpr bool ReleaseRowApplies(bool known, uint8_t copyOwner, uint8_t releasingOwner) {
	return known && copyOwner == releasingOwner;
}

} // namespace coopiii::game
