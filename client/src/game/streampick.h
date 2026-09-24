// Which of the peds and cars this machine hosts get a row in this tick's batch.
//
// docs/population.md §2.1. A batch carries MAX_PED_STATES peds or
// MAX_CAR_STATES cars, and a host routinely has more than that: CPopulation
// keeps about 25 pedestrians around its player and CCarCtrl a dozen cars. The
// samplers used to take the nearest the host's own player, with no memory
// from one tick to the next, so the same twelve went out every tick and the
// rest never did. On every other screen those stood where they were last
// heard of for as long as their host kept them.
//
// Now every row has a credit. Each tick it is left out, the credit grows by the
// row's weight, and the batch is filled from the highest credit down. A row
// nobody is near still gets its turn, the rows somebody is standing next to go
// out nearly every tick, and the batch is the same size it always was.
//
// The weight is the distance to the nearest *other* player, not to ours. This
// machine draws what it hosts straight from its own engine; the batch is only
// ever read by everybody else.
//
// A few rows can't wait their turn: one that has never been sent, one whose
// seat, siren, fire or health just changed (the observer holds those between
// rows, so a late row is a wrong car rather than a late one), and one whose
// receiver lets something lapse when the rows stop - a honk after
// HORN_FRESH_MS, a burning ped after REMOTE_FIRE_MS.
#pragma once

#include "horn.h"
#include "pedanim.h"

#include <coopiii/protocol.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace coopiii::game {

// Both batches go out at this rate (Client::SendHostedPedStates and
// SendHostedCarStates). A deadline below is counted in these.
constexpr uint32_t STREAM_TICK_MS = 100;

// A row's worth for one tick: STREAM_WEIGHT_REF_M over the distance to the
// nearest other player, from STREAM_WEIGHT_MAX down to 1. Inverse distance
// because that is how far a moving thing appears to move on screen: a car
// twice as far away needs rows half as often to look as smooth.
//
//   40 m or nearer   8        80 m   4        160 m   2        320 m on   1
//
// 1 is also the weight of everything when nobody else's position is known,
// which makes the batch a plain rotation.
constexpr float    STREAM_WEIGHT_REF_M = 320.0f;
constexpr uint16_t STREAM_WEIGHT_MAX   = 8;

inline uint16_t StreamWeight(bool anyViewer, float dist2) {
	if (!anyViewer || !(dist2 >= 0.0f))
		return 1;
	const float d = std::sqrt(dist2);
	if (d * STREAM_WEIGHT_MAX <= STREAM_WEIGHT_REF_M)
		return STREAM_WEIGHT_MAX;
	const float w = STREAM_WEIGHT_REF_M / d;
	return static_cast<uint16_t>(w < 1.0f ? 1.0f : w);
}

// Squared distance from `at` to the nearest of `viewers`. False when there
// are none, and `dist2` is left alone.
inline bool NearestViewerDist2(const Vec3 &at, const Vec3 *viewers, uint32_t count,
                               float &dist2) {
	bool any = false;
	for (uint32_t i = 0; i < count; ++i) {
		const float dx = at.x - viewers[i].x, dy = at.y - viewers[i].y,
		            dz = at.z - viewers[i].z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (!any || d2 < dist2) {
			dist2 = d2;
			any   = true;
		}
	}
	return any;
}

// The most ticks a receiver can go between rows before something it holds
// lapses after `freshMs`. Half of it, so one lost batch still lands inside.
constexpr uint8_t StreamDeadlineTicks(uint32_t freshMs) {
	const uint32_t half = freshMs / STREAM_TICK_MS / 2;
	return static_cast<uint8_t>(half == 0 ? 1 : half > 254 ? 254 : half);
}

constexpr uint8_t STREAM_HONK_DEADLINE = StreamDeadlineTicks(HORN_FRESH_MS);
constexpr uint8_t STREAM_FIRE_DEADLINE = StreamDeadlineTicks(REMOTE_FIRE_MS);

// What the observer holds from one row to the next, packed so a change is one
// compare. A ped's seat and flags; a car's health, siren and horn.
inline uint32_t PedRowSays(uint16_t vehicleNetId, uint8_t seat, uint8_t flags) {
	return (static_cast<uint32_t>(vehicleNetId) << 16) |
	       (static_cast<uint32_t>(seat) << 8) | flags;
}

inline uint32_t CarRowSays(uint16_t health, bool siren, bool horn) {
	return static_cast<uint32_t>(health) | (siren ? 1u << 16 : 0u) |
	       (horn ? 1u << 17 : 0u);
}

// An NPC's round (protocol.h, C_NpcShot) is only drawn, so it is only worth
// the packet to somebody close enough to see the streak or hear the report.
// With nobody else placed yet it goes anyway: a new arrival has no position
// until their first snapshot, and a round is cheap.
constexpr float NPC_SHOT_SEEN_M = 150.0f;

inline bool NpcShotWorthSending(const Vec3 &origin, const Vec3 *viewers, uint32_t count) {
	float d2 = 0.0f;
	if (!NearestViewerDist2(origin, viewers, count, d2))
		return true;
	return d2 <= NPC_SHOT_SEEN_M * NPC_SHOT_SEEN_M;
}

constexpr uint8_t STREAM_NEVER_SENT = 0xFF;

// Kept per hosted entity from tick to tick. A fresh one has never been sent.
struct StreamRow {
	uint16_t credit    = 0;
	// Ticks since this row last went out, saturating at 254.
	uint8_t  sinceSent = STREAM_NEVER_SENT;
	// The *Says value of the last row that went out.
	uint32_t said      = 0;
};

struct StreamCandidate {
	StreamRow *row      = nullptr;
	uint16_t   weight   = 1;
	uint32_t   says     = 0;
	// STREAM_*_DEADLINE while it applies, 0 for none.
	uint8_t    deadline = 0;
	// To the nearest other player; breaks ties between equal credits.
	float      dist2    = 0.0f;
	// The caller's, carried through the sort.
	uint32_t   index    = 0;

	// Set by PickStreamRows.
	bool       urgent   = false;
	// For a picked row: ticks since its previous one, 0 for its first.
	uint8_t    waited   = 0;
};

inline bool StreamsBefore(const StreamCandidate &a, const StreamCandidate &b) {
	if (a.urgent != b.urgent)
		return a.urgent;
	if (a.row->credit != b.row->credit)
		return a.row->credit > b.row->credit;
	if (a.dist2 != b.dist2)
		return a.dist2 < b.dist2;
	return a.index < b.index;
}

// One tick. Every candidate's row takes this tick's weight, the best `slots`
// are moved to the front of `c` in the order they should be written, and
// their rows are marked sent. Returns how many that is.
inline uint32_t PickStreamRows(StreamCandidate *c, uint32_t n, uint32_t slots) {
	for (uint32_t i = 0; i < n; ++i) {
		StreamRow &row = *c[i].row;
		if (row.sinceSent != STREAM_NEVER_SENT && row.sinceSent < 254)
			++row.sinceSent;
		const uint32_t credit = static_cast<uint32_t>(row.credit) + c[i].weight;
		row.credit = static_cast<uint16_t>(credit > 0xFFFF ? 0xFFFF : credit);
		c[i].urgent = row.sinceSent == STREAM_NEVER_SENT || c[i].says != row.said ||
		              (c[i].deadline != 0 && row.sinceSent >= c[i].deadline);
	}

	const uint32_t picked = slots < n ? slots : n;
	std::partial_sort(c, c + picked, c + n, StreamsBefore);

	for (uint32_t i = 0; i < picked; ++i) {
		StreamRow &row = *c[i].row;
		c[i].waited   = row.sinceSent == STREAM_NEVER_SENT ? uint8_t{0} : row.sinceSent;
		row.credit    = 0;
		row.sinceSent = 0;
		row.said      = c[i].says;
	}
	return picked;
}

} // namespace coopiii::game
