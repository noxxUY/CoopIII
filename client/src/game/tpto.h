// TPTO<n>, the first of CoopIII's own typed cheats (cheats.h, COOP_CHEATS):
// typed in play the way the game's cheats are, it puts this player beside
// player n, n being the number the Tab list puts before a
// name (chatfeed.h, ListNumber: the slot plus one). docs/cheats.md 7.
//
// **On foot it is the move the session already makes.** The rampage vote and
// the session's mission bring a player beside somebody with
// MovePlayerBeside (rampagevote.h): out of the way of any car another player
// sits in, on the ground, in sight of him, facing him, CStreaming::LoadScene
// round the spot first and, for another island, the player held where the
// target is until CCollision::Update has loaded it. TPTO is one more caller.
//
// **At the wheel the car comes along**, the way SET_PLAYER_COORDINATES moves
// the car of a player in one (re3 Script.cpp, the handler at 0x0043A995):
// the car's own Teleport through its vtable at the ground plus
// GetDistanceFromCentreOfMassToBaseOfModel. Only a CAutomobile, whose slot
// 11 is CAutomobile::Teleport (addresses.h); never onto another island,
// where the car would arrive before the ground under it; and only onto a
// spot CWorld::TestSphereAgainstWorld finds empty, instead of the handler's
// ClearSpaceForMissionEntity, which makes room by deleting cars that may well
// be somebody's. A passenger is refused: the car is the driver's to move.
//
// Everything that decides is here, with no engine, so tools/clienttest runs
// it; game/tpto.cpp reads the facts and does the move.
#pragma once

#include "rampagevote.h"
#include "../chatfeed.h"

#include <coopiii/protocol.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace coopiii::game {

// What TPTO does, or why it does nothing.
enum class Tpto : uint8_t {
	OnFoot,         // go
	InCar,          // go, and take the car we drive
	NotInSession,   // nothing to go to; said only in the log
	Off,            // the server's coopCheats is off
	Mission,        // on a mission, and the server keeps them out of missions
	NoSuchPlayer,
	Self,
	NoPed,          // no player in the world: a loading screen
	Busted,
	Wasted,
	Cutscene,
	Busy,           // already being moved
	NoPlace,        // nothing has said where he is yet
	IslandShut,     // his island is not open in our story yet
	Passenger,
	Vehicle,        // a vehicle the move cannot take: a boat, a plane, a train
	IslandInCar,    // he is on another island and we are driving
	NoRoom,         // no empty spot for the car beside him (decided in the game half)
};

struct TptoFacts {
	bool          inSession  = false;
	uint8_t       rule       = COOP_CHEATS_OUTSIDE_MISSIONS;
	uint8_t       localId    = INVALID_PLAYER;
	uint8_t       targetId   = INVALID_PLAYER;
	bool          targetHere = false;   // that slot is somebody in the session
	bool          targetPlaced = false; // and something says where he is
	TeleportFacts self;                 // rampagevote.h; onMission is the script's flag
	bool          moving     = false;   // a move is under way already
	bool          inCar      = false;
	bool          driver     = false;
	bool          carMovable = false;   // a CAutomobile
	bool          otherIsland = false;  // his island is not the one loaded here
	bool          islandOpen = true;
};

// The digit typed after TPTO, as the slot it names.
inline uint8_t TptoTargetOf(char digit) {
	return digit >= '0' && digit <= '9' ? PlayerIdFromListNumber(static_cast<unsigned>(digit - '0'))
	                                    : static_cast<uint8_t>(INVALID_PLAYER);
}

inline Tpto DecideTpto(const TptoFacts &f) {
	if (!f.inSession)
		return Tpto::NotInSession;
	if (f.rule == COOP_CHEATS_OFF)
		return Tpto::Off;
	if (f.targetId == f.localId && f.localId < MAX_PLAYERS)
		return Tpto::Self;
	if (f.targetId >= MAX_PLAYERS || !f.targetHere)
		return Tpto::NoSuchPlayer;

	// Our own player, the way the vote's move is refused (DecideTeleport),
	// with the mission left to the server's rule.
	TeleportFacts self = f.self;
	self.onMission     = false;
	switch (DecideTeleport(self)) {
	case RAMPAGE_SKIPPED_NO_PED:   return Tpto::NoPed;
	case RAMPAGE_SKIPPED_ARRESTED: return Tpto::Busted;
	case RAMPAGE_SKIPPED_DEAD:     return Tpto::Wasted;
	case RAMPAGE_SKIPPED_CUTSCENE: return Tpto::Cutscene;
	default:                       break;
	}
	// The script's own flag, which a running rampage sets as well and the
	// session's mission holds at one on everybody in it.
	if (f.self.onMission && f.rule != COOP_CHEATS_ALWAYS)
		return Tpto::Mission;
	if (f.moving)
		return Tpto::Busy;
	if (!f.targetPlaced)
		return Tpto::NoPlace;
	if (!f.islandOpen)
		return Tpto::IslandShut;
	if (f.inCar) {
		if (!f.driver)
			return Tpto::Passenger;
		if (!f.carMovable)
			return Tpto::Vehicle;
		if (f.otherIsland)
			return Tpto::IslandInCar;
		return Tpto::InCar;
	}
	return Tpto::OnFoot;
}

// The line in the chat feed when it does nothing, or "" for nothing to say
// (it went, or there is no session to say it in). `number` is what was typed
// after TPTO, `nick` the target's name or null.
inline void TptoMessage(Tpto v, unsigned number, const char *nick, char *out, size_t cap) {
	if (cap == 0)
		return;
	const char *who = nick && nick[0] ? nick : "them";
	switch (v) {
	case Tpto::Off:          std::snprintf(out, cap, "TPTO is switched off on this server"); return;
	case Tpto::Mission:      std::snprintf(out, cap, "TPTO: not during a mission"); return;
	case Tpto::NoSuchPlayer: std::snprintf(out, cap, "TPTO: nobody is number %u", number); return;
	case Tpto::Self:         std::snprintf(out, cap, "TPTO: number %u is you", number); return;
	case Tpto::NoPed:        std::snprintf(out, cap, "TPTO: not now"); return;
	case Tpto::Busted:       std::snprintf(out, cap, "TPTO: not while busted"); return;
	case Tpto::Wasted:       std::snprintf(out, cap, "TPTO: not while wasted"); return;
	case Tpto::Cutscene:     std::snprintf(out, cap, "TPTO: not during a cutscene"); return;
	case Tpto::Busy:         std::snprintf(out, cap, "TPTO: already on the way"); return;
	case Tpto::NoPlace:      std::snprintf(out, cap, "TPTO: nobody knows where %s is yet", who); return;
	case Tpto::IslandShut:
		std::snprintf(out, cap, "TPTO: %s is on an island your story has not opened", who);
		return;
	case Tpto::Passenger:    std::snprintf(out, cap, "TPTO: get out of the car first"); return;
	case Tpto::Vehicle:      std::snprintf(out, cap, "TPTO: cannot take this vehicle along"); return;
	case Tpto::IslandInCar:
		std::snprintf(out, cap, "TPTO: %s is on another island, leave the car first", who);
		return;
	case Tpto::NoRoom:       std::snprintf(out, cap, "TPTO: no room for the car beside %s", who); return;
	case Tpto::OnFoot:
	case Tpto::InCar:
	case Tpto::NotInSession: break;
	}
	out[0] = '\0';
}

// Where the car may go round him: eight directions on a ring a car and a half
// out, then on one further, the first empty one taken. The mission's own car
// ring is 7 m (mission.h, SPOT_CAR_RADII_M); a car is under 6 m long, so at 7
// m his car and ours do not touch whichever way either faces.
constexpr int   TPTO_CAR_DIRECTIONS = 8;
constexpr int   TPTO_CAR_TRIES      = TPTO_CAR_DIRECTIONS * 2;
constexpr float TPTO_CAR_RADII_M[2] = {7.0f, 10.0f};

inline void TptoCarSpot(int attempt, float *dx, float *dy) {
	const int   ring  = attempt < TPTO_CAR_DIRECTIONS ? 0 : 1;
	const float angle = 6.2831853f * static_cast<float>(attempt % TPTO_CAR_DIRECTIONS) /
	                    static_cast<float>(TPTO_CAR_DIRECTIONS);
	*dx = std::cos(angle) * TPTO_CAR_RADII_M[ring];
	*dy = std::sin(angle) * TPTO_CAR_RADII_M[ring];
}

// ---------------------------------------------------------------------------
// The game half (game/tpto.cpp)
// ---------------------------------------------------------------------------

// Remembers the client. Nothing is hooked.
void InstallTpto(Client &client);
void RemoveTpto();

// Once a frame, from the frame pump before CGame::Process and before the
// vote's move is ticked: CoopIII's typed cheats since the last frame
// (cheats.h, DrainCoopCheats), each one done or refused.
void TickCoopCheats();

} // namespace coopiii::game
