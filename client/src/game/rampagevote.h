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
#include <cstddef>
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
constexpr float VOTE_SPREAD_RADIUS_M   = 3.0f;
constexpr int   VOTE_SPREAD_ATTEMPTS   = 8;
// How far a spot's ground may be from the toucher's before it counts as a
// roof, a ledge or the road under a bridge rather than where he is standing.
constexpr float VOTE_SPREAD_MAX_STEP_M = 2.0f;

struct VoteSpreadSpot {
	float dx = 0.0f;
	float dy = 0.0f;
};

// Where `slot` of `count` goes, relative to the toucher, on its `attempt`th
// try. Attempt 0 is its own place on the ring. Later ones go half a place
// either side, then the same angles closer in and further out, so a spot
// against a wall still ends up near where it was meant to be.
// `radius` is the ring's; the other tries scale with it.
inline VoteSpreadSpot VoteSpreadCandidate(uint8_t slot, uint8_t count, int attempt,
                                          float radius = VOTE_SPREAD_RADIUS_M) {
	constexpr float kTwoPi = 6.28318530718f;
	if (count == 0)
		count = 1;
	const float step  = kTwoPi / static_cast<float>(count);
	float       angle = step * static_cast<float>(slot % count);
	float       r     = radius;
	switch (attempt) {
	case 0: break;
	case 1: angle += step * 0.5f; break;
	case 2: angle -= step * 0.5f; break;
	case 3: r = radius * 0.6f; break;
	case 4: r = radius * 1.5f; break;
	case 5: angle += step * 0.5f; r = radius * 0.6f; break;
	case 6: angle -= step * 0.5f; r = radius * 1.5f; break;
	default: r = radius * 0.4f; break;   // almost on top of him, as a last resort
	}
	return VoteSpreadSpot{std::cos(angle) * r, std::sin(angle) * r};
}

// ---------------------------------------------------------------------------
// The cars other players sit in
// ---------------------------------------------------------------------------
//
// The move puts a player down on foot round a spot that is where another
// player stands, and when that player sits in a car the spot is the car. The
// Fuzz Ball did it: a participant who had fallen behind was taken out of his
// own car and put down three metres from where the owner's taxi had been a
// snapshot ago, which was under it by then, and the physics stood him on its
// roof. So a place is never one a car somebody else sits in covers, or is
// about to: its body, a step to spare, and the road it covers in the next two
// seconds. Round a car the ring is a car's length and a half out
// (mission.h, SPOT_CAR_RADII_M, which says why), and the spot itself is never
// the fallback: that is the car.
struct MoveKeepOut {
	float x = 0.0f, y = 0.0f;     // where our copy of it is
	float vx = 0.0f, vy = 0.0f;   // metres per second
	float radius = 0.0f;          // CEntity::GetBoundRadius
};

constexpr float  MOVE_KEEP_OUT_MARGIN_M = 1.5f;
constexpr float  MOVE_KEEP_OUT_LEAD_S   = 2.0f;
constexpr float  MOVE_CAR_RING_M        = 7.0f;
// A car further than this from the spot reaches no place on either ring in
// the lead time at town speeds.
constexpr float  MOVE_KEEP_OUT_NEAR_M   = 60.0f;
constexpr size_t MOVE_KEEP_OUTS         = 8;
// A bounding radius past this is not a car's, and one that is not read is
// taken to be a long car's.
constexpr float  MOVE_CAR_RADIUS_MAX_M      = 30.0f;
constexpr float  MOVE_CAR_RADIUS_FALLBACK_M = 4.0f;

// How far (sx, sy) is outside what car `c` covers: its body with a step to
// spare, along the road it covers in the next MOVE_KEEP_OUT_LEAD_S. Negative
// inside it.
inline float SpotClearance(float sx, float sy, const MoveKeepOut &c) {
	// The nearest point of that stretch, from where it is to where it will be.
	const float ex = c.vx * MOVE_KEEP_OUT_LEAD_S, ey = c.vy * MOVE_KEEP_OUT_LEAD_S;
	const float len2 = ex * ex + ey * ey;
	float       t    = 0.0f;
	if (len2 > 1.0e-6f) {
		t = ((sx - c.x) * ex + (sy - c.y) * ey) / len2;
		t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	}
	const float dx = sx - (c.x + ex * t), dy = sy - (c.y + ey * t);
	return std::sqrt(dx * dx + dy * dy) - (c.radius + MOVE_KEEP_OUT_MARGIN_M);
}

// The least of it over every car; a large number with none.
inline float SpotClearanceOfCars(float sx, float sy, const MoveKeepOut *cars, size_t n) {
	float least = 1.0e9f;
	for (size_t i = 0; i < n; ++i) {
		const float c = SpotClearance(sx, sy, cars[i]);
		if (c < least)
			least = c;
	}
	return least;
}

inline bool SpotClearOfCars(float sx, float sy, const MoveKeepOut *cars, size_t n) {
	return SpotClearanceOfCars(sx, sy, cars, n) >= 0.0f;
}

// The ring round the spot: a car's size when a car somebody sits in is near it.
inline float MoveRingRadius(size_t carsNear) {
	return carsNear != 0 ? MOVE_CAR_RING_M : VOTE_SPREAD_RADIUS_M;
}

// Which try a player moved round (atX, atY) is put down on.
//
// With no car near, the ring's own tries (VoteSpreadCandidate), the first
// whose ground `groundOk(x, y)` accepts, and none (-1) when nothing does,
// which puts him on the spot itself as it always did.
//
// With cars near, never the spot, which is a car: the car ring's tries and
// then the same ones twice as far out, the first no car covers whose ground
// is good; else the first no car covers, ground or not; else, with every one
// of them covered, the one furthest outside the cars (`covered`).
struct MoveSpotPick {
	int   attempt  = -1;   // 0..2*VOTE_SPREAD_ATTEMPTS-1; the second lap is twice as far
	bool  groundOk = false;
	bool  covered  = false;
	float dx = 0.0f, dy = 0.0f;
};

constexpr int MOVE_SPOT_TRIES = VOTE_SPREAD_ATTEMPTS * 2;

inline VoteSpreadSpot MoveSpotTry(uint8_t slot, uint8_t count, int attempt, size_t carsNear) {
	const float radius = MoveRingRadius(carsNear);
	return VoteSpreadCandidate(slot, count, attempt % VOTE_SPREAD_ATTEMPTS,
	                           attempt < VOTE_SPREAD_ATTEMPTS ? radius : radius * 2.0f);
}

template <class GroundOk>
inline MoveSpotPick PickMoveSpot(float atX, float atY, uint8_t slot, uint8_t count,
                                 const MoveKeepOut *cars, size_t n, GroundOk groundOk) {
	if (n == 0) {
		for (int a = 0; a < VOTE_SPREAD_ATTEMPTS; ++a) {
			const VoteSpreadSpot s = MoveSpotTry(slot, count, a, 0);
			if (groundOk(atX + s.dx, atY + s.dy))
				return MoveSpotPick{a, true, false, s.dx, s.dy};
		}
		return MoveSpotPick{};
	}
	MoveSpotPick uncovered, best;
	float        bestClear = -1.0e9f;
	for (int a = 0; a < MOVE_SPOT_TRIES; ++a) {
		const VoteSpreadSpot s     = MoveSpotTry(slot, count, a, n);
		const float          clear = SpotClearanceOfCars(atX + s.dx, atY + s.dy, cars, n);
		if (clear < 0.0f) {
			if (clear > bestClear) {
				bestClear = clear;
				best      = MoveSpotPick{a, false, true, s.dx, s.dy};
			}
			continue;
		}
		if (groundOk(atX + s.dx, atY + s.dy))
			return MoveSpotPick{a, true, false, s.dx, s.dy};
		if (uncovered.attempt < 0)
			uncovered = MoveSpotPick{a, false, false, s.dx, s.dy};
	}
	return uncovered.attempt >= 0 ? uncovered : best;
}

// Is ground found at a spot where he can stand next to the toucher?
inline bool VoteSpreadGroundOk(bool found, float groundZ, float starterGroundZ) {
	return found && std::fabs(groundZ - starterGroundZ) <= VOTE_SPREAD_MAX_STEP_M;
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

// The island a move to a place on island `target` has to load before the
// player is put down there, with `loaded` the one whose collision is in
// memory: 0 when nothing does, the water between the islands (level 0)
// included.
inline int32_t IslandToLoadFirst(int32_t target, int32_t loaded) {
	return target != 0 && target != loaded ? target : 0;
}

// Whether the owner's LOAD_COLLISION_WITH_SCREEN of island `wanted` may run
// on a participant whose own player stands on island `here`. The owner's
// script decided it from where the owner stands (S.A.M. near the platform);
// loaded under a player standing on another island, it takes his ground away
// until CCollision::Update puts his own back, behind a second loading screen.
// Between the islands (0) the owner's word is as good as any.
inline bool MayLoadIslandUnder(int32_t wanted, int32_t here) {
	return here == 0 || here == wanted;
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

// The same move for the session's mission (docs/missions.md 11.5): this
// machine's player out of any car and put down in place `slot` of `count`
// round `pos`, where player `targetId` stands, the other island loaded first
// if it is on one. MayMovePlayer says whether it can be done now: not while
// dead, being arrested, in a cutscene, or already on the way somewhere.
// MovePlayerBeside is false, doing nothing, when it can't.
bool MayMovePlayer();
bool MovePlayerBeside(const Vec3 &pos, uint8_t targetId, uint8_t slot, uint8_t count);

// Our copy of the car another player sits in, by his ped here or, while that
// is being built, by the seat the session gave him; null on foot, for our own
// player, or for a car not built here. And how fast our copy of a car goes,
// in metres per second. What the move keeps clear of (MoveKeepOut), and what
// the session's mission waits on before it brings anybody to a driving owner
// (mission.h, BringWaitsForOwnersCar).
void *CarPlayerSitsIn(uint8_t playerId);
float CarSpeedMps(void *car);

// The island (x, y, z) is on, by CTheZones::GetLevelFromPosition, and the
// one whose collision is in memory here. The move above and the mission's
// own moves (game/mission.cpp, Teleport) both start from these.
int32_t IslandAt(float x, float y, float z);
int32_t IslandLoaded();

} // namespace coopiii::game
