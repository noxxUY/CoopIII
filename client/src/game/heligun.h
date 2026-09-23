// The police helicopter's gun. game/heli.h shares the helicopter; this shares
// what it fires. protocol.h's unnumbered entry after 32 is the design and
// addresses.h, "the police helicopter's gun", has the function it copies.
//
// Two halves:
//
//   the owner    a detour on FireOneInstantHitRound notices the rounds the
//                local engine's own police helicopters fire and queues them
//                for HeliSync, which sends C_HeliShot. The round itself goes
//                on exactly as the engine made it.
//
//   an observer  DrawHeliShot draws one of those rounds from the replica.
//                It is the cosmetic half of FireOneInstantHitRound, call by
//                call, and it never calls the function itself: that one takes
//                health off whatever it hits, and on this machine nothing may.
#pragma once

#include "helisync.h"

#include <cstdint>

namespace coopiii {
struct WorldBridge;
}

namespace coopiii::game {

// Which police slot fired a round from `source`. ProcessControl puts the
// source exactly HELI_SHOT_MUZZLE_OFFSET (3 m) out from the helicopter's own
// position toward the player, so the slot whose helicopter is that far away
// is the one. -1 when neither is: the script's or Catalina's helicopter, which
// fire through the same call and are not shared.
constexpr float HELI_SHOT_SLOT_TOLERANCE = 0.05f;

inline int HeliShotSlot(const Vec3 &source, const bool present[HELI_POLICE_SLOTS],
                        const Vec3 pos[HELI_POLICE_SLOTS]) {
	int   best     = -1;
	float bestMiss = HELI_SHOT_SLOT_TOLERANCE;
	for (int slot = 0; slot < HELI_POLICE_SLOTS; ++slot) {
		if (!present[slot])
			continue;
		const float dx = source.x - pos[slot].x;
		const float dy = source.y - pos[slot].y;
		const float dz = source.z - pos[slot].z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		// |d - 3| < tol without a square root: (3 - tol)^2 < d2 < (3 + tol)^2.
		constexpr float lo = (3.0f - HELI_SHOT_SLOT_TOLERANCE) * (3.0f - HELI_SHOT_SLOT_TOLERANCE);
		constexpr float hi = (3.0f + HELI_SHOT_SLOT_TOLERANCE) * (3.0f + HELI_SHOT_SLOT_TOLERANCE);
		if (!(d2 > lo && d2 < hi))
			continue;
		const float miss = d2 > 9.0f ? (d2 - 9.0f) / 6.0f : (9.0f - d2) / 6.0f;
		if (miss < bestMiss) {
			bestMiss = miss;
			best     = slot;
		}
	}
	return best;
}

// What FireOneInstantHitRound does at the far end, by what its line found:
// the switch at 0x00563E29 over table 0x00603238, and the water test when it
// found nothing. `entityType` is bits 0-2 of the victim's flags, or -1 for no
// victim.
enum class HeliImpact : uint8_t {
	WaterOrNothing,   // no victim: a splash if there is water under the target
	Building,         // script sound 6Ah and a puff of smoke at the point
	Vehicle,          // SOUND_WEAPON_HIT_VEHICLE on the car
	Ped,              // SOUND_WEAPON_HIT_PED on the ped
	Object,           // script sound 6Bh at the point
	Dummy,            // script sound 6Ch at the point
	Silent,           // a type the switch has no arm for
};

constexpr HeliImpact HeliImpactFor(int entityType) {
	switch (entityType) {
	case -1: return HeliImpact::WaterOrNothing;
	case 1:  return HeliImpact::Building;
	case 2:  return HeliImpact::Vehicle;
	case 3:  return HeliImpact::Ped;
	case 4:  return HeliImpact::Object;
	case 5:  return HeliImpact::Dummy;
	default: return HeliImpact::Silent;
	}
}

// The tracer's velocity: (target - source) * 0.15, 0x00563BE2..0x00563C29.
inline Vec3 HeliTracerVelocity(const Vec3 &source, const Vec3 &target) {
	constexpr float k = 0.15f;
	return Vec3{(target.x - source.x) * k, (target.y - source.y) * k,
	            (target.z - source.z) * k};
}

// One detour, on FireOneInstantHitRound. Not fatal: without it our own
// helicopter's rounds stay on this machine, which is how it was before.
bool InstallHeliGunHook();
void RemoveHeliGunHook();

// DrainOwnHeliShots when the detour is in, and DrawHeliShot always: drawing
// needs nothing hooked.
void AddHeliGunToBridge(WorldBridge &bridge);

} // namespace coopiii::game
