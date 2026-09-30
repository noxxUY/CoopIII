// The mines a mission drops, gone off on every machine (protocol.h,
// C_MineBlast, docs/protocol.md 1.36).
//
// DROP_MINE and DROP_NAUTICAL_MINE are on the replay list (game/replay.h), so
// every participant's engine lays its own mine at the owner's spot, and each
// copy arms and goes off by what its own engine sees: a car over it, or its
// ten seconds running out (addresses.h, "the mines"). Which machine that is
// first depends on where everybody's cars are, and nobody owns a mine, so
// whoever's goes off first says where, and every other machine takes its own
// out of the world at that place and sets off the same explosion there.
//
// The one call CPickup::Update makes to CExplosion::AddExplosion, 0x004309EB,
// is pointed at MineExplosion: the explosion goes through as before, and where
// it went off is queued for the session. Nothing about the arming changes.
//
// The rules are pure so tools/clienttest checks them without a game.
#pragma once

#include "addresses.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii {
struct WorldBridge;
}

namespace coopiii::game {

inline bool IsMinePickup(uint8_t type) {
	return type >= PICKUP_MINE_INACTIVE && type <= PICKUP_NAUTICAL_MINE_ARMED;
}

// Somebody else's mine and ours are the same one when they are this close.
// Every copy was laid at the same spot and none moves sideways; a nautical
// one rides its own machine's waves (CWaterLevel), so height gets more room.
constexpr float MINE_SAME_XY_M = 2.0f;
constexpr float MINE_SAME_Z_M  = 3.0f;

inline bool SameMinePlace(const Vec3 &a, const Vec3 &b) {
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return dx * dx + dy * dy <= MINE_SAME_XY_M * MINE_SAME_XY_M && dz <= MINE_SAME_Z_M &&
	       dz >= -MINE_SAME_Z_M;
}

// The blasts this machine has had, its own and the ones it was told of, for
// long enough that the same mine said a second time - two machines whose
// copies went off in the same moment both say so - is not set off again.
constexpr uint32_t MINE_BLAST_MEMORY_MS = 5000;
constexpr uint8_t  MINE_BLAST_MEMORY    = 8;

struct RecentMineBlasts {
	Vec3     pos[MINE_BLAST_MEMORY]  = {};
	uint32_t atMs[MINE_BLAST_MEMORY] = {};
	bool     used[MINE_BLAST_MEMORY] = {};
	uint8_t  next                    = 0;

	void Note(const Vec3 &p, uint32_t nowMs) {
		pos[next]  = p;
		atMs[next] = nowMs;
		used[next] = true;
		next       = static_cast<uint8_t>((next + 1) % MINE_BLAST_MEMORY);
	}

	bool Had(const Vec3 &p, uint32_t nowMs) const {
		for (uint8_t i = 0; i < MINE_BLAST_MEMORY; ++i)
			if (used[i] && nowMs - atMs[i] < MINE_BLAST_MEMORY_MS && SameMinePlace(pos[i], p))
				return true;
		return false;
	}
};

// What a blast somebody else had comes to here.
enum class MineBlastHere : uint8_t {
	Already,    // this machine had it already; nothing
	OurMine,    // our copy of the mine goes, and the explosion is where it was
	NoMine,     // we have no mine there: the explosion alone, where they said
};

inline MineBlastHere PlanMineBlast(bool hadIt, bool haveMine) {
	if (hadIt)
		return MineBlastHere::Already;
	return haveMine ? MineBlastHere::OurMine : MineBlastHere::NoMine;
}

// Points the explosion call at MineExplosion. False, and the log says so,
// when the bytes there are not the retail call; mines are then each
// machine's own, as they were.
bool InstallMineHooks();
void RemoveMineHooks();

void AddMinesToBridge(WorldBridge &bridge);

} // namespace coopiii::game
