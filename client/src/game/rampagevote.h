// The vote before a rampage, on this machine: the help box, the two keys, and
// the move to the player who touched the skull.
//
// protocol.h (RampageVoteBody) has the whole exchange and
// server/core/rampagevote.h the rules. Three things happen here and nowhere
// else:
//
// 1. **The line.** The vote is shown in the game's own help box, top left,
//    through CHud::SetHelpMessage - the beep, the fade and the box are the
//    game's. From then on the count and the seconds are kept current by
//    writing the three buffers CHud::Draw reads while the box is up, which
//    changes the text without fading it out and back or beeping again
//    (addresses.h, "the help box"). A help line the game put up itself - a
//    mission's, a cheat's - is never pushed off: ours waits until the box is
//    free, and if the game takes the box while ours is up, ours steps aside.
//
// 2. **The keys.** Y and N by default, voteYesKey and voteNoKey in
//    CoopIII.ini. Read with GetAsyncKeyState, the way the seat key is, so
//    nothing is taken from the game: GTA III binds neither, and a key that is
//    only polled can't be swallowed. Only while a vote is open, only with the
//    game's window in front, never while the chat line is open or the menu is
//    up, and never for the toucher, whose touch was his yes.
//
// 3. **The move.** Each machine moves its own player and nobody else's. Not a
//    player who is dead, being arrested, in a cutscene or on a mission - he
//    still voted, he just stays put, and the server is told why. Anyone else
//    is taken out of whatever car he is in the way WARP_CHAR_FROM_CAR_TO_COORD
//    does it, put in a ring round the toucher on ground the engine's own
//    vertical line test finds, and turned to face him.
//
//    Another island is the engine's business: the player is put down at the
//    toucher's height, and the frame's own CCollision::Update loads that
//    island's collision before CWorld::Process runs (addresses.h, "collision
//    is per island"). He is held there until it has, then placed on the
//    ground, and the scene round him is loaded the way LOAD_SCENE loads it.
//    An island his story hasn't opened is not a reason to stay behind - the
//    point of the vote is to be together - but the log says so.
#pragma once

#include <coopiii/protocol.h>

#include <cmath>
#include <cstdint>

namespace coopiii {
class Client;
}

namespace coopiii::game {

// ---------------------------------------------------------------------------
// The ring
// ---------------------------------------------------------------------------

// How far from the toucher, and how many other spots to try when one has no
// ground under it. Three metres gives seven players two and a half metres
// each, which is room to turn round in and close enough to see who's who.
constexpr float SPREAD_RADIUS_M   = 3.0f;
constexpr int   SPREAD_ATTEMPTS   = 8;
// How far a spot's ground may be from the toucher's before it counts as a
// roof, a ledge or the road under a bridge rather than where he is standing.
constexpr float SPREAD_MAX_STEP_M = 2.0f;

struct SpreadSpot {
	float dx = 0.0f;
	float dy = 0.0f;
};

// Where `slot` of `count` goes, relative to the toucher, on its `attempt`th
// try. Attempt 0 is its own place on the ring. Later ones go half a place
// either side, then the same angles closer in and further out, so a spot
// against a wall still ends up near where it was meant to be.
inline SpreadSpot SpreadCandidate(uint8_t slot, uint8_t count, int attempt) {
	constexpr float kTwoPi = 6.28318530718f;
	if (count == 0)
		count = 1;
	const float step  = kTwoPi / static_cast<float>(count);
	float       angle = step * static_cast<float>(slot % count);
	float       r     = SPREAD_RADIUS_M;
	switch (attempt) {
	case 0: break;
	case 1: angle += step * 0.5f; break;
	case 2: angle -= step * 0.5f; break;
	case 3: r = SPREAD_RADIUS_M * 0.6f; break;
	case 4: r = SPREAD_RADIUS_M * 1.5f; break;
	case 5: angle += step * 0.5f; r = SPREAD_RADIUS_M * 0.6f; break;
	case 6: angle -= step * 0.5f; r = SPREAD_RADIUS_M * 1.5f; break;
	default: r = 1.2f; break;   // almost on top of him, as a last resort
	}
	return SpreadSpot{std::cos(angle) * r, std::sin(angle) * r};
}

// Is ground found at a spot where he can stand next to the toucher?
inline bool SpreadGroundOk(bool found, float groundZ, float starterGroundZ) {
	return found && std::fabs(groundZ - starterGroundZ) <= SPREAD_MAX_STEP_M;
}

// The heading that faces `to` from `from`, in the engine's convention:
// forward is (-sin h, cos h), so h = atan2(-dx, dy).
inline float HeadingToward(float fromX, float fromY, float toX, float toY) {
	return std::atan2(-(toX - fromX), toY - fromY);
}

// ---------------------------------------------------------------------------
// Who moves
// ---------------------------------------------------------------------------

struct TeleportFacts {
	bool     havePed        = false;
	float    health         = 100.0f;
	uint32_t pedState       = 1;       // PEDSTATE_IDLE
	uint8_t  wbState        = 0;       // WBSTATE_PLAYING
	bool     cutscene       = false;
	bool     onMission      = false;   // CTheScripts::IsPlayerOnAMission
	bool     frenzyOngoing  = false;   // CDarkel::FrenzyOnGoing
};

// RampageArrival: RAMPAGE_ARRIVED to go, or why not.
//
// The mission test has one exception. rampage.sc sets $ONMISSION as it starts
// the frenzy, and on this machine that can happen before the move is carried
// out - the collection and the move are sent a moment apart. A frenzy running
// is that rampage, not a mission.
inline uint8_t DecideTeleport(const TeleportFacts &f) {
	if (!f.havePed)
		return RAMPAGE_SKIPPED_NO_PED;
	if (f.wbState == 2 /* WBSTATE_BUSTED */ || f.pedState == 56 /* PEDSTATE_ARRESTED */)
		return RAMPAGE_SKIPPED_ARRESTED;
	if (f.wbState != 0 || f.health <= 0.0f || f.pedState == 48 /* DIE */ ||
	    f.pedState == 49 /* DEAD */)
		return RAMPAGE_SKIPPED_DEAD;
	if (f.cutscene)
		return RAMPAGE_SKIPPED_CUTSCENE;
	if (f.onMission && !f.frenzyOngoing)
		return RAMPAGE_SKIPPED_MISSION;
	return RAMPAGE_ARRIVED;
}

// Has this machine's story opened `level` yet? Portland always; Staunton once
// portland_complete has run; Shoreside once staunton_complete has.
inline bool IslandOpen(int32_t level, bool industrialPassed, bool commercialPassed) {
	if (level == 2)
		return industrialPassed;
	if (level == 3)
		return commercialPassed;
	return true;
}

// ---------------------------------------------------------------------------
// The game half
// ---------------------------------------------------------------------------

// Keys from CoopIII.ini, as virtual-key codes. Y and N if never called.
void SetRampageVoteKeys(int yesKey, int noKey);

// Remembers the client. Nothing is hooked.
void InstallRampageVote(Client &client);
void RemoveRampageVote();

// Once a frame, from the frame pump before CGame::Process: the help box, the
// keys, and a move that is waiting or in progress.
void TickRampageVote();

} // namespace coopiii::game
