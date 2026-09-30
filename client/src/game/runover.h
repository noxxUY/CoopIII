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

namespace coopiii {
struct RemotePlayer;
}

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

// ---- our car into somebody else's pedestrian (docs/protocol.md 1.59) --------
//
// The copy on our screen used to be a wall. KillPedWithCar ran on it, the
// kill arm's InflictDamage was refused (combat.cpp: nobody here hurts a
// pedestrian this machine does not host), and with no death there was no fall
// either, so the ped stayed standing and the arm's `car->ApplyMoveForce(-100)`
// hit our car again on every frame of contact. Its host never heard of it: its
// copy of our car, a snapshot late, usually missed him.
//
// So both halves happen now. Our copy goes down at once - the knock arm's own
// SetFall already does that, the kill arm gets the same fall - which also
// ends the pushing, since KillPedWithCar returns early for a ped in PED_FALL.
// And the hit goes to his host as C_PedDamage with the cause
// WEAPONTYPE_RAMMEDBYCAR and the impulse, where the real ped goes through the
// same KillPedWithCar with the host's copy of our car.

// The fall our copy is put in when the kill arm left him standing. `direction`
// is the one the arm handed InflictDamage; the knock arm uses the same sum.
inline uint16_t RunOverFallAnim(uint32_t direction) {
	return static_cast<uint16_t>(ANIM_STD_HIGHIMPACT_FRONT + (direction & 3u));
}

// Does the copy still need putting down after the engine's call?
inline bool RunOverLeftStanding(uint32_t pedState) {
	return pedState != PEDSTATE_FALL && pedState != PEDSTATE_DIE && pedState != PEDSTATE_DEAD;
}

// Once per pedestrian per contact: the fall stops KillPedWithCar being asked
// again, and this is the backstop for a fall the engine refused.
constexpr uint32_t RUN_OVER_FORWARD_GAP_MS = 500;

inline bool RunOverForwardDue(uint16_t lastNetId, uint32_t lastMs, uint16_t netId,
                              uint32_t nowMs) {
	return netId != lastNetId || nowMs - lastMs >= RUN_OVER_FORWARD_GAP_MS;
}

// The impulse off the wire, bounded: finite and not negative, and nothing
// past what KillPedWithCar tells apart (its highest threshold is 12).
constexpr float MAX_RUN_OVER_IMPULSE = 1000.0f;

inline bool RunOverImpulseFromWire(float wire, float &out) {
	if (!(wire == wire) || wire < 0.0f)
		return false;
	out = wire > MAX_RUN_OVER_IMPULSE ? MAX_RUN_OVER_IMPULSE : wire;
	return true;
}

// Is a C_PedDamage a run-over rather than a hit?
inline bool IsRunOverForward(uint8_t weapon) { return weapon == WEAPONTYPE_RAMMEDBYCAR; }

// The host's half: a player's car ran into its pedestrian `ped` on their
// screen. Runs CPed::KillPedWithCar with our copy of that player's car.

void ApplyRunOverOnHostedPed(RemotePlayer *attacker, void *ped, uint16_t netId,
                             float impulse);

// Four call sites in the engine are pointed at runover.cpp: the IsPlayer test,
// and the InflictDamage of the player arm and of KillPedWithCar's two arms.
// Not fatal if they fail: a teammate's car goes back to costing a flat 20,
// friendly fire or not.
bool InstallRunOverHooks();
void RemoveRunOverHooks();

} // namespace coopiii::game
