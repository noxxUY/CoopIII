// Fists and the bat: the decisions, and nothing that needs the game.
//
// addresses.h, "fists and the bat", has the engine side. The short version is
// that a melee hit is two things and only one of them is InflictDamage. The
// rest - the victim defending, falling over, being shoved - happens on the
// struck ped inside the striker's own call, CPed::FightStrike for fists and
// CWeapon::FireMelee for the bat, before and after the damage.
//
// So the split is the one bullets already have, carried one step further:
//
//   the striker's machine   finds the hit, keeps the damage off another
//                           machine's ped and forwards it (combat.cpp,
//                           HookedInflictDamage), and now keeps the reaction
//                           off that ped as well. It adds which path landed
//                           the hit and with what move: MELEE_* on the wire.
//   the owner's machine     plays the struck ped's half of that path on the
//                           real ped, in the engine's order, through the
//                           engine's own StartFightDefend, SetFall and
//                           ApplyMoveForce, reading the health either side of
//                           InflictDamage the way the engine does.
//   everybody else          sees both through the pose streams. A copy never
//                           punches anybody and never reacts to a punch.
//
// Nothing here reaches a car. Neither path looks at anything but m_nearPeds.
#pragma once

#include "addresses.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

struct MeleeTag {
	uint8_t melee    = MELEE_NONE;
	uint8_t hitLevel = 0;
};

inline uint8_t MeleeKind(const MeleeTag &tag) { return tag.melee & MELEE_KIND_MASK; }

inline bool IsMeleeCause(uint8_t weapon) {
	return weapon == WEAPONTYPE_UNARMED || weapon == WEAPONTYPE_BASEBALLBAT;
}

// m_curFightMove is about to index the fight move table.
inline bool IsFightMove(int32_t move) { return move >= 0 && move < NUM_FIGHTMOVES; }

// What FightStrike's own tests read off the striker. `levelOfMove` is the
// table row's hitLevel.
inline MeleeTag StrikeTag(int32_t move, uint8_t levelOfMove, bool armed) {
	MeleeTag t;
	t.melee    = MELEE_STRIKE;
	t.hitLevel = levelOfMove < MELEE_HIT_LEVELS ? levelOfMove : 0;
	if (armed)
		t.melee |= MELEE_ARMED;
	if (move == FIGHTMOVE_GROUNDKICK)
		t.melee |= MELEE_GROUND_KICK;
	return t;
}

inline MeleeTag SwingTag(bool heavy) {
	MeleeTag t;
	t.melee = MELEE_SWING;
	if (heavy)
		t.melee |= MELEE_HEAVY;
	return t;
}

// The two bytes off the wire, or MELEE_NONE when they don't add up. A tag
// that is refused still leaves the damage: it is only the reaction that
// needs them to make sense.
//
// A strike always hurts with cause 0 (the `push 0` at 0x004E93A4) and always
// has a move that strikes, so a level of 1..4. A swing is the weapon's own
// cause and its level is worked out here, not sent.
inline MeleeTag ReadMeleeTag(uint8_t weapon, uint8_t melee, uint8_t hitLevel) {
	MeleeTag t;
	if (melee & ~MELEE_KNOWN_BITS)
		return t;
	switch (melee & MELEE_KIND_MASK) {
	case MELEE_STRIKE:
		if (weapon != WEAPONTYPE_UNARMED || hitLevel == 0 || hitLevel >= MELEE_HIT_LEVELS)
			return t;
		if (melee & MELEE_HEAVY)
			return t;
		t.melee    = melee;
		t.hitLevel = hitLevel;
		return t;
	case MELEE_SWING:
		if (!IsMeleeCause(weapon) || (melee & (MELEE_ARMED | MELEE_GROUND_KICK)))
			return t;
		t.melee = melee;
		return t;
	default:
		return t;
	}
}

inline bool PedStateOnGround(uint32_t state) {
	return state == PEDSTATE_FALL || state == PEDSTATE_DIE || state == PEDSTATE_DEAD;
}

inline bool PedStateDyingOrDead(uint32_t state) {
	return state == PEDSTATE_DIE || state == PEDSTATE_DEAD;
}

// Both paths leave a player who is getting up alone - no defend, no damage.
// On the striker's machine the victim is a copy, which is never a player, so
// only the owner can ask this.
inline bool MeleeSparesVictim(bool victimIsPlayer, uint32_t state) {
	return victimIsPlayer && state == PEDSTATE_GETUP;
}

// FightStrike's damageMult, back out of the amount it became. The amount is
// always damageMult * 3.0f exactly.
inline uint8_t StrikeDamageMult(float amount) {
	const float m = amount / STRIKE_DAMAGE_PER_MULT;
	if (!(m > 0.0f))
		return 0;
	if (m >= 255.0f)
		return 255;
	return static_cast<uint8_t>(m + 0.5f);
}

// StartFightDefend's second and third arguments.
inline uint8_t DefendHitLevel(const MeleeTag &tag, uint8_t weapon, uint32_t victimState) {
	if (MeleeKind(tag) == MELEE_STRIKE)
		return tag.hitLevel;
	return weapon == WEAPONTYPE_BASEBALLBAT && PedStateOnGround(victimState) ? HITLEVEL_GROUND
	                                                                          : HITLEVEL_HIGH;
}

inline uint8_t DefendArg(const MeleeTag &tag, bool victimIsPlayer, uint8_t damageMult) {
	if (MeleeKind(tag) == MELEE_STRIKE)
		return (tag.melee & MELEE_ARMED) && !victimIsPlayer ? STRIKE_ARMED_DEFEND : damageMult;
	return SWING_DEFEND;
}

// Does a strike knock him over? `state` and `after` are read after the
// damage, `before` ahead of it. The striker is a player: CoopIII only sends
// the local player's hits.
inline bool StrikeKnocksDown(uint32_t state, float before, float after, bool victimIsPlayer,
                             bool armed, bool oneHitKnockdown) {
	if (PedStateOnGround(state) || !(after > 0.0f))
		return false;
	return (after < STRIKE_KNOCK_NPC_BELOW && before > STRIKE_KNOCK_NPC_BELOW && !victimIsPlayer) ||
	       (after < MELEE_KNOCK_BELOW && before > MELEE_KNOCK_BELOW) || armed || oneHitKnockdown;
}

// The strike's shove, as a multiple of the unit direction away from the
// striker (with z = 1). Only taken when the victim is dying or off his feet.
inline float StrikePushScale(bool groundKick, bool dying, uint8_t damageMult) {
	float k = static_cast<float>(damageMult);
	if (groundKick) {
		k = k * STRIKE_GROUNDKICK_MULT;
		if (STRIKE_GROUNDKICK_CAP < k)
			k = STRIKE_GROUNDKICK_CAP;
	} else if (dying && damageMult < STRIKE_DYING_BELOW) {
		k = k * STRIKE_DYING_MULT;
		if (STRIKE_DYING_CAP < k)
			k = STRIKE_DYING_CAP;
	}
	return k * STRIKE_PUSH_SCALE;
}

// Does a swing knock him over? Same reading order as the strike.
inline bool SwingKnocksDown(uint32_t state, float before, float after, bool bat,
                            bool victimIsPlayer) {
	if (PedStateOnGround(state) || !(after > 0.0f))
		return false;
	return (after < MELEE_KNOCK_BELOW && before > MELEE_KNOCK_BELOW) || (bat && !victimIsPlayer);
}

inline int32_t SwingFallMs(bool bat, bool victimIsPlayer) {
	return bat && !victimIsPlayer ? SWING_FALL_NPC_BAT_MS : SWING_FALL_MS;
}

// The shove a swing gives a ped that is already dying, when it didn't knock
// him down. Not for the heavy swing.
inline bool SwingShovesDying(uint32_t state, bool heavy) {
	return state == PEDSTATE_DIE && !heavy;
}

// The amount FireMelee would have passed had it known its victim was a
// player. The striker's engine asks IsPlayer of its copy, which never is, so
// a plain bat hit on another player came out at half. The heavy swing and
// adrenaline are decided by the striker and come first in the engine too.
inline float SwingAmountForPlayer(float amount, bool bat, bool heavy, bool adrenaline) {
	return bat && !heavy && !adrenaline ? amount * SWING_PLAYER_BAT_MULT : amount;
}

// The same two for a pedestrian's hit on a player, which is what an NPC's hit
// on the wire always is (protocol.h, C_NpcDamage). His engine asked IsPlayer
// of its copy of the victim, so a bat still came out at half; the heavy swing
// and adrenaline arms want a player swinging (0x0055CE0E) and never apply.
inline float NpcSwingAmountForPlayer(float amount, bool bat) {
	return SwingAmountForPlayer(amount, bat, false, false);
}

// And his strike's MELEE_ARMED is dropped: it only knocks a victim down when a
// player is striking (004E9482), and against a player it changes nothing else
// (DefendArg), so on the wire it could only ever mislead.
inline MeleeTag NpcMeleeTagForPlayer(MeleeTag tag) {
	if (MeleeKind(tag) == MELEE_STRIKE)
		tag.melee = static_cast<uint8_t>(tag.melee & ~MELEE_ARMED);
	return tag;
}

// May the fight code react on this ped here? Not while a strike or a swing
// is running and the ped is another machine's: its owner plays the reaction.
inline bool CopyMayReactToMelee(bool insideMelee, bool otherMachinesPed) {
	return !(insideMelee && otherMachinesPed);
}

} // namespace coopiii::game
