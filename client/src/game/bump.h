// The engine half of ramming a car another machine simulates
// (client/src/bumpsync.h, docs/protocol.md 1.71).
//
// CAutomobile's ProcessControl slot is taken (addresses.h, "what a collision
// did to a car") so that each car's speed is noted between its own
// ProcessControl and the frame's collisions. After physics, a copy's speed
// less that note is what the collisions did to it, and its damage record says
// what hit it. SpeedSamples is the table those notes go in, here so clienttest
// can run it.
#pragma once

#include "../bumpsync.h"
#include "../interp.h"

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

// One frame's notes, by car. Open addressing over a fixed table, where a slot
// from an earlier frame counts as free, so nothing is ever cleared: a car is
// found only if it was noted this frame, and only once (Take), which keeps a
// paused game - same frame, no physics - from reading one collision over and
// over.
class SpeedSamples {
public:
	static constexpr size_t SIZE = 64;

	bool Note(const void *car, uint32_t frame, const Vec3 &move, const Vec3 &turn) {
		const size_t h = Hash(car);
		for (size_t i = 0; i < SIZE; ++i) {
			Slot &s = m_slots[(h + i) % SIZE];
			if (s.car == car || !s.live || s.frame != frame) {
				s.car   = car;
				s.frame = frame;
				s.live  = true;
				s.taken = false;
				s.move  = move;
				s.turn  = turn;
				return true;
			}
		}
		return false;
	}

	bool Take(const void *car, uint32_t frame, Vec3 &move, Vec3 &turn) {
		const size_t h = Hash(car);
		for (size_t i = 0; i < SIZE; ++i) {
			Slot &s = m_slots[(h + i) % SIZE];
			if (!s.live || s.frame != frame)
				return false;   // it would have gone here
			if (s.car != car)
				continue;
			if (s.taken)
				return false;
			s.taken = true;
			move    = s.move;
			turn    = s.turn;
			return true;
		}
		return false;
	}

private:
	struct Slot {
		const void *car   = nullptr;
		uint32_t    frame = 0;
		bool        live  = false;
		bool        taken = false;
		Vec3        move{};
		Vec3        turn{};
	};
	static size_t Hash(const void *car) {
		return (reinterpret_cast<uintptr_t>(car) >> 4) % SIZE;
	}
	Slot m_slots[SIZE];
};

// What the frame's collisions did: the speed after physics less the note.
inline Vec3 SpeedChange(const Vec3 &after, const Vec3 &noted) {
	return Vec3{after.x - noted.x, after.y - noted.y, after.z - noted.z};
}

bool InstallBumpWatch();
void RemoveBumpWatch();

// WorldBridge's four (client.h, "ramming somebody else's car").
bool    ReadCopyContact(int32_t poolHandle, CopyContact &out);
bool    ReadVehiclePose(int32_t poolHandle, VehicleTransform &out);
bool    ApplyVehicleBump(int32_t target, int32_t by, const Vec3 &move, const Vec3 &turn,
                         float impulse, uint8_t piece);

} // namespace coopiii::game
