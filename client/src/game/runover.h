// A car hitting somebody on foot, when the car and the ped live on different
// machines.
//
// addresses.h, "a car hitting a ped", has the engine side. The machine whose
// engine runs the victim's CPed::ProcessControl decides, from its own copy of
// the car, and nothing is forwarded:
//
//   the local player   our ProcessControl's player arm. It prices a hit by
//                      closing speed only when the car's driver IsPlayer(),
//                      and a remote player's ped is a CCivilianPed, so a
//                      teammate's car cost a flat 20 - 6.6 health. The call
//                      at 0x004C93AA now asks us as well.
//   our pedestrian     KillPedWithCar, off the collision impulse. It never
//                      asks who drives, and the copy of the car it meets
//                      carries its owner's velocity (ApplyRemoteVehicle), so
//                      this one already came out the way single player does.
//   somebody else's    refused here by combat.cpp's InflictDamage detour and
//                      never forwarded (IsForwardableDamage says no to both
//                      car causes), so there is only ever one decision.
//
// Friendly fire. A teammate's car is theirs the way their bullets are, and a
// collision never crosses the server, so the victim's machine is the one that
// refuses it - the same place a friendly blast or fire is refused. Only the
// health is declined. The knockdown stays: the car did hit us, in our world,
// just as a refused fire still burns.
//
// The rules are pure so tools/clienttest checks them without a game.
#pragma once

#include "addresses.h"

#include <cstdint>

namespace coopiii::game {

inline bool IsCarHitCause(uint32_t method) {
	return method == WEAPONTYPE_RAMMEDBYCAR || method == WEAPONTYPE_RUNOVERBYCAR;
}

// The 0x004C93AA test: a player at the wheel, by ped type or by being one of
// the session's players.
inline bool DriverCountsAsPlayer(bool pedTypeIsPlayer, bool remotePlayersPed) {
	return pedTypeIsPlayer || remotePlayersPed;
}

// What the player arm hands InflictDamage for a hit that got past its two
// thresholds (0x004C93A0-0x004C93D7).
inline float CarHitOnPlayerDamage(float closing, bool driverIsPlayer, bool train) {
	if (driverIsPlayer)
		return closing * CAR_HIT_PER_CLOSING;
	return train ? CAR_HIT_TRAIN_DAMAGE : CAR_HIT_FLAT_DAMAGE;
}

// Does the player arm hit at all? The car has to be moving and closing on us.
inline bool PlayerArmHits(float carSpeedSq, float closing) {
	return carSpeedSq > CAR_HIT_MIN_SPEED_SQ && closing > CAR_HIT_MIN_CLOSING;
}

// What a hit costs a player before armour (InflictDamage, 0x004EA505).
inline float PlayerHealthFor(float damage) { return damage * PLAYER_DAMAGE_SCALE; }

// m_vecMoveSpeed is metres per 1/50 s.
constexpr float KMH_PER_SPEED = 50.0f * 3.6f;

enum class CarHit : uint8_t {
	None,               // not a car cause, or nothing session-shaped about it
	OnUsByAnotherPlayer,
	OurCarOnTheirs,     // their machine decides, from its copy of our car
	TheirCarOnOurs,     // ours decides, from our copy of their car
};

inline CarHit ClassifyCarHit(bool carHitCause, bool victimIsUs, bool victimIsOtherMachines,
                             bool victimHostedHere, bool carIsOurs, bool carIsAnotherPlayers) {
	if (!carHitCause)
		return CarHit::None;
	if (victimIsUs)
		return carIsAnotherPlayers ? CarHit::OnUsByAnotherPlayer : CarHit::None;
	if (victimIsOtherMachines)
		return carIsOurs ? CarHit::OurCarOnTheirs : CarHit::None;
	if (victimHostedHere && carIsAnotherPlayers)
		return CarHit::TheirCarOnOurs;
	return CarHit::None;
}

// May the health go? Only a teammate's car on us is a question, and friendly
// fire answers it.
inline bool CarHitMayLand(CarHit hit, bool friendlyFire) {
	return hit != CarHit::OnUsByAnotherPlayer || friendlyFire;
}

// Four call sites in the engine are pointed at runover.cpp: the IsPlayer test,
// and the InflictDamage of the player arm and of KillPedWithCar's two arms.
// Not fatal if they fail: a teammate's car goes back to costing a flat 20,
// friendly fire or not.
bool InstallRunOverHooks();
void RemoveRunOverHooks();

} // namespace coopiii::game
