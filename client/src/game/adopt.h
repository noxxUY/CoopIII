// Turning a replica back into an entity this machine hosts (protocol.h,
// S_AmbientAdopt).
//
// The server has decided who takes a leaver's pedestrians and traffic; this
// is what the machine it named does about each one, kept as arithmetic over
// the bytes involved so tools/clienttest can walk it. game/population.cpp and
// game/vehicle.cpp read the bytes out of the engine, run them through here
// and write them back.
//
// The recipe is "undo what the replica spawn did, back to what the
// constructor leaves", because that is what a generator's ped or car is: a
// constructed entity the population code never touched again. Every value
// below is in addresses.h beside the instruction it came from.
#pragma once

#include "addresses.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

enum class AdoptPlan : uint8_t {
	Convert,        // take it over
	Release,        // cannot: say C_PedDespawn / C_CarDespawn as its new owner
	KeepForClaim,   // a car the local player is at the wheel of, see below
};

// A pedestrian. Nothing to convert without a replica here - still streaming
// its model, or the roster was full - and a corpse or a non-civilian is the
// server's release too (server/core/adopt.h); asked again here because the
// server may be older, and because `dead` is this machine's own record.
inline AdoptPlan PlanPedAdoption(bool haveRow, bool haveReplica, bool dead, uint8_t pedType) {
	if (!haveRow || !haveReplica || dead || !AmbientPedTypeAdoptable(pedType))
		return AdoptPlan::Release;
	return AdoptPlan::Convert;
}

// A traffic car. The one case that is neither: the local player is at the
// wheel of it and has asked the session to make it a session car
// (S_CarPromoted). Converted, the hosted-traffic sweep would see our player in
// the driver's seat, drop it and send a despawn under the promotion; released,
// the car would be destroyed under him. So the row stays a replica with us as
// its owner, and the promotion that is already on its way takes it from there
// as a replica we had (Client::OnCarPromoted).
inline AdoptPlan PlanCarAdoption(bool haveRow, bool haveReplica, bool destroyed,
                                 bool localAtWheel) {
	if (!haveRow || !haveReplica || destroyed)
		return AdoptPlan::Release;
	if (localAtWheel)
		return AdoptPlan::KeepForClaim;
	return AdoptPlan::Convert;
}

// ---- a pedestrian ------------------------------------------------------------

struct AdoptPedBytes {
	uint8_t createdBy = 0;   // offs::PED_CHAR_CREATED_BY
	uint8_t entityB   = 0;   // offs::ENTITY_FLAGS_B, bExplosionProof
	uint8_t entityC   = 0;   // offs::ENTITY_FLAGS_C, the other four proofs
	uint8_t pedC      = 0;   // offs::PED_FLAGS_C, bRespondsToThreats
	uint8_t pedG      = 0;   // offs::PED_FLAGS_G, bAllowMedicsToReviveMe
	int8_t  zone      = 0;   // offs::ZONE_LEVEL
};

// What SpawnAmbientReplica changed, put back. Nothing else in the bytes moves.
inline AdoptPedBytes PedBytesAfterAdoption(AdoptPedBytes b) {
	b.createdBy = static_cast<uint8_t>(CHAR_CREATED_BY_RANDOM);
	b.entityB   = static_cast<uint8_t>(b.entityB & ~offs::ENTITY_EXPLOSION_PROOF);
	b.entityC   = static_cast<uint8_t>(
        b.entityC & ~(offs::ENTITY_BULLET_PROOF | offs::ENTITY_FIRE_PROOF |
                      offs::ENTITY_COLLISION_PROOF | offs::ENTITY_MELEE_PROOF));
	b.pedC      = static_cast<uint8_t>(b.pedC | offs::PED_RESPONDS_TO_THREATS);
	b.pedG      = static_cast<uint8_t>(b.pedG | offs::PED_ALLOW_MEDICS);
	b.zone      = LEVEL_GENERIC;
	return b;
}

// Only a ped on his feet is given somewhere to walk. One in a car is the
// car's (SetWanderPath would only arm bStartWanderPathOnFoot for when he gets
// out, and ClearAll would take him out of PED_DRIVING), and a dying one is
// left to finish.
inline bool AdoptedPedWanders(bool inVehicle, uint32_t pedState) {
	return !inVehicle && pedState != PEDSTATE_DIE && pedState != PEDSTATE_DEAD;
}

// The direction to wander in, 0..7, from the way he is facing, so he walks on
// rather than turning round. CPathFind::FindNextNodeWandering reads direction
// d as (sin(d*pi/4), cos(d*pi/4)) - 0 north, 2 east - and a ped facing
// `heading` looks along (-sin h, cos h), so d = -h / (pi/4), rounded. A wrong
// guess costs a ped turning round, nothing more: SetWanderPath tries all
// eight before it gives up. Never outside 0..7, which the script handler
// bounds before its own call (addresses.h, CPed__SetWanderPath).
inline uint8_t WanderDirForHeading(float heading) {
	if (!(heading == heading) || heading > 1.0e6f || heading < -1.0e6f)
		return 0;
	constexpr float EIGHTH = 0.78539816f;
	float steps = -heading / EIGHTH;
	int   d     = static_cast<int>(steps < 0.0f ? steps - 0.5f : steps + 0.5f);
	d %= 8;
	if (d < 0)
		d += 8;
	return static_cast<uint8_t>(d);
}

// ---- a traffic car -----------------------------------------------------------

struct AdoptCarBytes {
	uint8_t status     = 0;      // ENTITY_FLAGS bits 3-7
	uint8_t flagsA     = 0;      // offs::VEH_FLAGS_A: bIsLocked, bEngineOn
	uint8_t flagsC     = 0;      // offs::VEH_FLAGS_C: bHasBeenOwnedByPlayer
	uint8_t entityC    = 0;      // offs::ENTITY_FLAGS_C: bCollisionProof
	uint8_t mission    = 0;      // offs::AUTOPILOT_CAR_MISSION
	uint8_t cruise     = 0;      // offs::AUTOPILOT_CRUISE_SPEED
	float   maxTraffic = 0.0f;   // offs::AUTOPILOT_MAX_TRAFFIC_SPEED
	int8_t  zone       = 0;      // offs::ZONE_LEVEL
};

// Is there somebody at the wheel who can drive it away?
inline bool AdoptedCarIsDriven(bool hasDriver, uint32_t driverState) {
	return hasDriver && driverState != PEDSTATE_DIE && driverState != PEDSTATE_DEAD;
}

// Unlocked, which is the half of the reaping gate a RANDOM_VEHICLE replica
// carried alone; not owned by any player; no longer held collision-proof by
// the observed-car rule; CREATE_CAR's speeds where the replica spawn zeroed
// them; the generic level. Then, driven, what CAR_WANDER_RANDOMLY writes on
// top: cruise mission, engine on, cruise speed at least 6 - and a driven car
// the engine left ABANDONED is put in PHYSICS, the status every seating in
// this project leaves a car with a driver in (game/carstatus.h). The road
// join and the anti-reverse timer are calls and a clock, done by the caller.
inline AdoptCarBytes CarBytesAfterAdoption(AdoptCarBytes b, bool driven) {
	b.flagsA  = static_cast<uint8_t>(b.flagsA & ~offs::VEH_IS_LOCKED);
	b.flagsC  = static_cast<uint8_t>(b.flagsC & ~offs::VEH_HAS_BEEN_OWNED_BY_PLAYER);
	b.entityC = static_cast<uint8_t>(b.entityC & ~offs::ENTITY_COLLISION_PROOF);
	b.zone    = LEVEL_GENERIC;
	if (b.cruise == 0)
		b.cruise = static_cast<uint8_t>(CREATE_CAR_CRUISE_SPEED);
	if (!(b.maxTraffic > 0.0f))
		b.maxTraffic = CREATE_CAR_CRUISE_SPEED;

	if (!driven) {
		b.mission = CAR_MISSION_NONE;
		return b;
	}
	b.mission = CAR_MISSION_CRUISE;
	b.flagsA  = static_cast<uint8_t>(b.flagsA | offs::VEH_ENGINE_ON);
	if (b.cruise < CAR_WANDER_MIN_CRUISE)
		b.cruise = CAR_WANDER_MIN_CRUISE;
	if (b.status == ENTITY_STATUS_ABANDONED)
		b.status = ENTITY_STATUS_PHYSICS;
	return b;
}

} // namespace coopiii::game
