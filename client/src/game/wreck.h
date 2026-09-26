// A car's end: who decides that a burning car goes up, what a blast on a car
// nobody holds does to the session, where a wreck comes to rest, and the shell
// a late joiner is handed.
//
// Pure, like horn.h, so tools/clienttest covers every decision here without a
// game and checks the transcription it rests on (addresses.h, "a wreck
// without its blast") against the retail image when it is given one. The
// engine side is game/vehicle.cpp, the roster's is client.cpp, and the
// server's is Session::DestroyVehicle and Session::NoteWreckState.
#pragma once

#include "addresses.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

// ---- the fire timer of a car nobody holds -----------------------------------
//
// CAutomobile::ProcessControl blows up a car that has spent five seconds of
// its own frames under 250 health (0x00534510 .. 0x005347AB), and it asks
// nothing else. A car somebody drives or settles has its timer held on every
// other machine (ApplyRemoteVehicle), so one engine decides. A car nobody
// holds used to be every machine's: with its custodian gone or out of time,
// each engine counted its own five seconds from its own frame and blew its own
// copy up, a different moment on every screen and an explosion, a fire and a
// payout on each.
//
// Now one machine counts for it: the session host, which is always there. The
// car is pinned where the session last had it on every machine, the host's
// included, so the host's engine needs no ground under it to count five
// seconds, and its BlowUpCar goes out as UNOWNED_SESSION and wrecks every
// other copy through the ordinary replay. With no host known every machine
// counts, which is how it was.
//
// `holder` is the driver, or the custodian when there is none (client.h,
// VehicleHolder). A car we hold is never written by ApplyRemoteVehicle, so the
// first answer only matters for completeness.
inline bool FireTimerRunsHere(uint8_t holder, uint8_t localPlayerId,
                              uint8_t hostPlayerId) {
	if (holder != INVALID_PLAYER)
		return holder == localPlayerId;
	if (hostPlayerId == INVALID_PLAYER || localPlayerId == INVALID_PLAYER)
		return true;
	return hostPlayerId == localPlayerId;
}

// ---- a blast on a car nobody holds -------------------------------------------
//
// Every machine replays the explosion at the same place, so every copy takes
// the same off it, and ObservedRow::blastHealth keeps that on each of them.
// What none of them did was tell the session: its row kept the health from
// before the blast, so a joiner was handed the car undamaged, a copy rebuilt
// after the engine reaped it came back undamaged, and the next player to hold
// it reported the health their copy had, which was the old one if their copy
// had missed the blast.
//
// So the machine whose explosion it was asks for the car, the way a shot does
// (Session::CustodyForHit): a C_VehicleHit with the blast's own cause. The
// server makes it the custodian and sends the hit back, which no machine
// applies, because every machine already took the blast itself
// (IsForwardableDamage refuses cause 18). Its engine then keeps the car's
// health and its fire timer, and its snapshots put the blast into the
// session's row. The explosion's creator is the culprit InflictDamage is
// handed: CWorld::TriggerExplosionSectorList pushes its fifth argument there
// (0x004B18C6) and 18 as the cause (0x004B18C4), and a projectile's explosion
// is created by its thrower (CProjectileInfo::RemoveProjectile, 0x0055B73D).
// One machine per blast, then: the thrower's. A blast nobody's player made
// (a car going up in traffic, the script) asks for nothing and leaves the
// per-machine floor to hold it.
inline bool BlastAsksForCar(bool byLocalPlayer, uint8_t weapon, bool wreckedAfter) {
	return byLocalPlayer && weapon == WEAPONTYPE_EXPLOSION && !wreckedAfter;
}

// ---- where a wreck comes to rest ---------------------------------------------
//
// BlowUpCar adds 0.13 to the car's vertical speed and the explosion does the
// rest: a wreck hops, rolls on with what it had, drops into the water it was
// over. Only the machine that decided the wreck simulated any of that - every
// other machine held its copy where the blast happened, in mid-air if that is
// where it was, and a car that blew up over the harbour stood on the water.
//
// A wreck is now settled like any car nobody drives. The session makes the
// machine that decided it the custodian (Session::DestroyVehicle), which
// streams it until it lies still, and every other machine follows. Its
// snapshots carry VEH_WRECKED, which is what tells them from the snapshots
// its last driver sent before the blast, still in flight: those never carry
// it, and a copy that took one would be dragged back to where the car was a
// moment before it exploded.
enum class WreckStateUse : uint8_t {
	Car,     // an ordinary snapshot of a car that isn't a wreck
	Wreck,   // a wreck's settle, from the machine settling it: take the transform
	Drop,    // anything else
};

inline WreckStateUse ClassifyWreckState(bool destroyed, uint8_t senderId,
                                        uint8_t custodianPlayerId, uint8_t flags) {
	const bool saysWrecked = (flags & VEH_WRECKED) != 0;
	if (!destroyed)
		// A wreck's settle that overtook the event that makes it one. The event
		// is reliable and on its way; the transform can wait for it.
		return saysWrecked ? WreckStateUse::Drop : WreckStateUse::Car;
	if (!saysWrecked || custodianPlayerId == INVALID_PLAYER ||
	    senderId != custodianPlayerId)
		return WreckStateUse::Drop;
	return WreckStateUse::Wreck;
}

// ---- the shell a joiner is handed ----------------------------------------------
//
// What BlowUpCar leaves on a car, without the explosion, the fire, the camera
// shake or the flying wheel (addresses.h, "a wreck without its blast"). A
// boat's BlowUpCar is shorter: no time of death and no damage model.
struct QuietWreckPlan {
	bool automobile = false;   // time of death, FuckCarCompletely, bomb, siren, taxi
	bool bodywork   = false;   // the bumpers, the doors and the front left wheel
};

inline QuietWreckPlan PlanQuietWreck(int32_t vehicleType, uint16_t modelId) {
	QuietWreckPlan plan;
	plan.automobile = vehicleType == VEHICLE_TYPE_CAR;
	plan.bodywork   = plan.automobile && modelId != MI_RCBANDIT;
	return plan;
}

// BlowUpCar's eight damage calls, in its order: (component, panel) for the
// two bumpers and (component, door) for the six doors.
struct WreckPart {
	uint8_t component;
	uint8_t part;
};

constexpr WreckPart WRECK_BUMPERS[2] = {
    {CAR_BUMP_FRONT, VEHBUMPER_FRONT},
    {CAR_BUMP_REAR, VEHBUMPER_REAR},
};

constexpr WreckPart WRECK_DOORS[6] = {
    {CAR_BONNET, DOOR_BONNET},       {CAR_BOOT, DOOR_BOOT},
    {CAR_DOOR_LF, DOOR_FRONT_LEFT},  {CAR_DOOR_RF, DOOR_FRONT_RIGHT},
    {CAR_DOOR_LR, DOOR_REAR_LEFT},   {CAR_DOOR_RR, DOOR_REAR_RIGHT},
};

} // namespace coopiii::game
