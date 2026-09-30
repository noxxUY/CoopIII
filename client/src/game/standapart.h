// Two players on one spot.
//
// Every machine puts its own player where its own engine or scripts say, and
// several of those places are the same for everybody: main.scm's new-game
// player and the end of the intro (811.875, -939.9375, Give Me Liberty's
// marker), a hospital's or a police station's respawn point, the spot a
// save loads at. None of those go through the session. Two players put there
// stand inside each other, and each engine's collision pushes its own player
// off the other's copy while that copy is put back where its owner's
// snapshots say, every frame. With both on one point neither push has a
// direction the two machines agree on, so both can go the same way and one
// follows the other across the street.
//
// So each machine watches its own player against every other one on foot.
// Of two who overlap, one stays where they are, and that is decided the same
// way on both machines (StandOutranks): the owner of the session's mission
// while one runs, else the lower player id. The other one moves:
//
//   - stacked on the same point, it takes its own place on a ring round it,
//     the place its rank among the session's players gives it, tested free
//     of buildings, cars and objects by the engine's own line test and on
//     ground near the other's, and keeps its heading;
//   - merely too close, it steps away, a little each frame, where the line
//     ahead of it is clear.
//
// Neither happens before they have overlapped for STAND_HOLD_MS, so brushing
// past somebody is left to the engine. Nothing puts a player back: CoopIII
// never writes the local player's position from the wire.
#pragma once

#include "mission.h"

#include <coopiii/mission.h>
#include <coopiii/protocol.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coopiii {
class Client;
}

namespace coopiii::game {

// ---- pure, for tools/clienttest ---------------------------------------------------

// Nearer than this, flat, and two players stand on one point: somebody put
// both there.
constexpr float    STAND_STACKED_M = 0.35f;
// Nearer than this and their bodies are inside each other (a pedestrian's
// collision spheres are 0.35 m round, so two touch at 0.7).
constexpr float    STAND_CLOSE_M   = 0.6f;
// Further apart than this in height is a bridge over a road, not an overlap.
constexpr float    STAND_HEIGHT_M  = 1.5f;
// How long an overlap lasts before anybody is moved.
constexpr uint32_t STAND_HOLD_MS   = 400;
// How fast a player who is too close steps away: a walk.
constexpr float    STAND_NUDGE_MPS = 1.5f;
// The rings a stacked player's place is looked for on, widest first.
constexpr float    STAND_SLOT_RADII_M[2] = {1.5f, 1.0f};
// Ground under a place further than this from the ground under the other
// player is a ledge or the road under a bridge.
constexpr float    STAND_MAX_STEP_M = 1.0f;

struct StandPeer {
	uint8_t playerId = INVALID_PLAYER;
	Vec3    pos      = {};
};

// Whether player `a` keeps their spot against player `b`. `first` is the
// session's mission's owner while one runs, INVALID_PLAYER otherwise.
inline bool StandOutranks(uint8_t a, uint8_t b, uint8_t first) {
	if (a == b)
		return false;
	if (first != INVALID_PLAYER) {
		if (a == first)
			return true;
		if (b == first)
			return false;
	}
	return a < b;
}

// Our place round `anchorId`'s spot: how many of the session's other players
// (`players`, a PlayerBit mask) outrank us, the anchor left out, and how many
// places there are. Taken from the whole session rather than from who stands
// there now, so a player who already moved does not give its place to the
// next one.
inline void StandRank(uint8_t localId, uint8_t anchorId, uint8_t players, uint8_t first,
                      uint8_t *rank, uint8_t *count) {
	uint8_t r = 0, c = 1;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if ((players & PlayerBit(id)) == 0 || id == localId || id == anchorId)
			continue;
		++c;
		if (StandOutranks(id, localId, first))
			++r;
	}
	*rank  = r;
	*count = c;
}

enum class StandMove : uint8_t {
	None,    // nobody who outranks us is inside us
	Slot,    // on their very spot: to our own place round it
	Nudge,   // too close: a step away
};

struct StandVerdict {
	StandMove move     = StandMove::None;
	uint8_t   anchorId = INVALID_PLAYER;   // who stays
	Vec3      anchor   = {};
	float     apartM   = 0.0f;             // flat, from them to us
	uint8_t   rank     = 0;                // StandRank
	uint8_t   count    = 1;
	float     awayX    = 1.0f;             // which way a step goes, a unit vector
	float     awayY    = 0.0f;
};

// What our player at `me` does about the others on foot. Of several on our
// point, the one who outranks everybody is who we move round, so every
// machine rings the same spot; otherwise the nearest one we are too close to.
inline StandVerdict DecideStand(uint8_t localId, const Vec3 &me, const StandPeer *peers, size_t n,
                                uint8_t players, uint8_t first) {
	StandVerdict v;
	if (!std::isfinite(me.x) || !std::isfinite(me.y) || !std::isfinite(me.z))
		return v;
	const StandPeer *stacked = nullptr, *close = nullptr;
	float            stackedD = 0.0f, closeD = STAND_CLOSE_M;
	for (size_t i = 0; i < n; ++i) {
		const StandPeer &p = peers[i];
		if (p.playerId == localId || !StandOutranks(p.playerId, localId, first))
			continue;
		const float dx = me.x - p.pos.x, dy = me.y - p.pos.y, dz = me.z - p.pos.z;
		if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) ||
		    std::fabs(dz) > STAND_HEIGHT_M)
			continue;
		const float d = std::sqrt(dx * dx + dy * dy);
		if (d < STAND_STACKED_M &&
		    (!stacked || StandOutranks(p.playerId, stacked->playerId, first))) {
			stacked  = &p;
			stackedD = d;
		}
		if (d < closeD) {
			close  = &p;
			closeD = d;
		}
	}
	const StandPeer *who = stacked ? stacked : close;
	if (!who)
		return v;
	v.move     = stacked ? StandMove::Slot : StandMove::Nudge;
	v.anchorId = who->playerId;
	v.anchor   = who->pos;
	v.apartM   = stacked ? stackedD : closeD;
	StandRank(localId, who->playerId, players, first, &v.rank, &v.count);
	const float dx = me.x - who->pos.x, dy = me.y - who->pos.y;
	if (v.apartM > 0.01f) {
		v.awayX = dx / v.apartM;
		v.awayY = dy / v.apartM;
	} else {
		// No way away from a point we are on: our place's way round it.
		float sx = 0.0f, sy = 0.0f;
		SpreadSpot(0.0f, 0.0f, v.rank, v.count, 0, &sx, &sy, 1.0f);
		v.awayX = sx;
		v.awayY = sy;
	}
	return v;
}

// A step away, `dtMs` after the last one: a walk's worth, no more than takes
// us clear, and nothing for a frame that took a long time.
inline float StandNudgeStep(float apartM, uint32_t dtMs) {
	const uint32_t dt    = dtMs > 100 ? 100 : dtMs;
	const float    walk  = STAND_NUDGE_MPS * static_cast<float>(dt) / 1000.0f;
	const float    clear = STAND_CLOSE_M + 0.05f - apartM;
	return clear <= 0.0f ? 0.0f : walk < clear ? walk : clear;
}

// Whether a verdict has held long enough to act on: the same player in us
// for STAND_HOLD_MS. Anything else starts the wait again.
class StandWatch {
public:
	bool Due(const StandVerdict &v, uint32_t nowMs) {
		if (v.move == StandMove::None) {
			m_anchor = INVALID_PLAYER;
			return false;
		}
		if (v.anchorId != m_anchor) {
			m_anchor = v.anchorId;
			m_since  = nowMs;
			return false;
		}
		return static_cast<uint32_t>(nowMs - m_since) >= STAND_HOLD_MS;
	}
	void Reset() { m_anchor = INVALID_PLAYER; }

private:
	uint8_t  m_anchor = INVALID_PLAYER;
	uint32_t m_since  = 0;
};

// ---- the game half ----------------------------------------------------------------

void InstallStandApart(Client &client);
void RemoveStandApart();

// Once a frame, from the frame pump before CGame::Process.
void TickStandApart();

// What last put our player where they are, for the log line of a move that
// follows it: "the mission's SET_PLAYER_COORDINATES", "a summon". A string
// that lives for the whole run.
void NoteStandPlacement(const char *what);

} // namespace coopiii::game
