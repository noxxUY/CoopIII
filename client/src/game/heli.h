// The police helicopter, engine side. helisync.h is the session side and
// protocol.h's entry 32 is the design; addresses.h, "the police
// helicopter", has every address this file uses and how each was found.
//
// Six detours, all on Heli.cpp functions plus one on CWanted:
//
//   CHeli::UpdateHelis           what left its slot, and why; and the reward
//                                taken back when somebody else shot it down
//   CHeli::ProcessControl        a replica gets the owner's transform instead
//                                of the AI
//   CHeli::TestBulletCollision   replayed bullets stop hitting our own
//   CHeli::TestRocketCollision   helicopter, and our own hit a replica
//   CHeli::SpecialHeliPreRender  a replica's searchlight and tail light
//   CWanted::RegisterCrime_Immediately   only while UpdateHelis is blowing
//                                up a helicopter somebody else shot down
//
// and a seventh, on a lead, when its call sites check out:
//
//   CPlane::TestRocketCollision  somebody else's rocket passes a plane by
//
// The rules the engine applies are transcribed below as pure functions, so
// tools/clienttest checks them without a game.
#pragma once

#include "helisync.h"
#include "leadcheck.h"

#include <cstdint>
#include <cstring>

namespace coopiii {
struct WorldBridge;
}

namespace coopiii::game {

// The fields TestBulletCollision and TestRocketCollision read and write on
// one helicopter.
struct HeliDamageState {
	uint8_t  status          = HELI_STATUS_HOVER;
	uint8_t  heliType        = 0;
	bool     bulletProof     = false;
	bool     explosionProof  = false;
	uint32_t bulletDamage    = 0;
	uint32_t explosionTimer  = 0;
	float    angularSpeed    = 0.0f;
};

// Would this hit bring it down (or keep it coming down)? Asked first so the
// engine's rand() is only drawn when the engine would draw it.
inline bool HeliHitBringsDown(const HeliDamageState &h, uint8_t kind, uint16_t damage) {
	constexpr uint32_t BULLET_LIMIT          = 700;
	constexpr uint32_t BULLET_LIMIT_CATALINA = 400;
	if (kind == HELI_HIT_ROCKET)
		return !h.explosionProof;
	if (kind == HELI_HIT_BULLET && !h.bulletProof)
		return h.bulletDamage + damage >
		       (h.heliType == 2 ? BULLET_LIMIT_CATALINA : BULLET_LIMIT);
	return false;
}

// One hit, the way the engine's own arm for its own array applies it
// (addresses.h). `randomLow` is `rand() < 3FFFh`, which picks the spin. True
// when the hit landed at all; the caller compares status before and after to
// see whether this one brought it down.
inline bool ApplyHeliHitRule(HeliDamageState &h, uint8_t kind, uint16_t damage,
                             uint32_t nowMs, bool randomLow) {
	constexpr uint32_t EXPLODE_AFTER_MS = 10000;

	if (kind == HELI_HIT_ROCKET) {
		if (h.explosionProof)
			return false;
	} else if (kind == HELI_HIT_BULLET) {
		if (h.bulletProof)
			return false;
	} else {
		return false;
	}

	const bool down = HeliHitBringsDown(h, kind, damage);
	if (kind == HELI_HIT_BULLET)
		h.bulletDamage += damage;

	if (down) {
		h.angularSpeed   = (randomLow ? 1.0f : 0.0f) * 0.1f - 0.05f;
		h.status         = HELI_STATUS_SHOT_DOWN;
		h.explosionTimer = nowMs + EXPLODE_AFTER_MS;
	}
	return true;
}

// Will UpdateHelis blow this helicopter up on this call? The test at
// 0x00549C6C..0x00549C91: status 3 and the time strictly past the timer.
inline bool HeliExplodesThisUpdate(uint8_t status, uint32_t nowMs,
                                   uint32_t explosionTimer) {
	return status == HELI_STATUS_SHOT_DOWN && nowMs > explosionTimer;
}

// Who a shoot-down belongs to, as each hit lands. The first hit that takes a
// helicopter to SHOT_DOWN decides it and nothing after changes it: the engine
// goes on re-arming the timer on every later hit, but the helicopter was
// already coming down.
inline uint8_t HeliCreditAfterHit(uint8_t creditBefore, uint8_t statusBefore,
                                  uint8_t statusAfter, uint8_t hitBy) {
	if (statusBefore != HELI_STATUS_SHOT_DOWN && statusAfter == HELI_STATUS_SHOT_DOWN)
		return hitBy;
	return creditBefore;
}

// What UpdateHelis pays for one helicopter, in the four places it pays.
struct HeliRewards {
	int32_t money          = 0;
	int32_t helisDestroyed = 0;
	int32_t peopleKilled   = 0;
	int32_t copsKilled     = 0;
};

constexpr HeliRewards HELI_REWARD_EACH = {250, 1, 2, 2};

// The owner's engine paid `exploded` helicopters' worth between `before` and
// `after`, and `notOurs` of them were brought down by somebody else. What the
// four values should be put back to. Only when the engine moved by exactly
// what UpdateHelis pays: anything else means something other than the
// explosion moved them in the same call, and a guess would be worse than
// leaving the payment where it is. False then, and `out` is `after`.
inline bool WithholdHeliRewards(const HeliRewards &before, const HeliRewards &after,
                                int32_t exploded, int32_t notOurs,
                                HeliRewards &out) {
	out = after;
	if (notOurs <= 0 || exploded < notOurs)
		return false;
	const HeliRewards &e = HELI_REWARD_EACH;
	if (after.money - before.money != e.money * exploded ||
	    after.helisDestroyed - before.helisDestroyed != e.helisDestroyed * exploded ||
	    after.peopleKilled - before.peopleKilled != e.peopleKilled * exploded ||
	    after.copsKilled - before.copsKilled != e.copsKilled * exploded)
		return false;
	out.money          -= e.money * notOurs;
	out.helisDestroyed -= e.helisDestroyed * notOurs;
	out.peopleKilled   -= e.peopleKilled * notOurs;
	out.copsKilled     -= e.copsKilled * notOurs;
	return true;
}

// Does this call to RegisterCrime_Immediately come from the explosion of a
// helicopter somebody else shot down? `withheldSlots` has bit n set for slot
// n while UpdateHelis runs. The engine's own arguments say which slot it is:
// crime 0Ch, id 4D83h + slot (0x0054A10B..0x0054A11B).
inline bool IsWithheldHeliCrime(int32_t crime, uint32_t id, uint8_t withheldSlots) {
	if (crime != 12 || id < 0x4D83u || id >= 0x4D83u + HELI_POLICE_SLOTS)
		return false;
	return (withheldSlots & (1u << (id - 0x4D83u))) != 0;
}

// ---- a plane, and somebody else's rocket ----------------------------------
//
// CPlane::TestRocketCollision is CProjectileInfo::Update's other rocket test,
// straight after the helicopter's. For an observer's copy of somebody else's
// rocket it crashed the Dodo here and registered the crime against *our*
// player - stars for a rocket we never fired - and the crash blasts, which name
// FindPlayerPed as their culprit, went out as ours on top of the shooter's.
// The shooter's machine is the one that decides that rocket, so here it passes
// the plane by and the owner's explosion ends it, as every other remote
// projectile ends.
//
// Its address is a lead, not in addresses.h (docs/addresses-unverified.md):
// the detour goes in only when both of the helicopter test's call sites, which
// addresses.h does prove, have a call to it within a few bytes, taking one
// dword and popping it the way a one-argument __cdecl call does.
constexpr uintptr_t PLANE_ROCKET_TEST_LEAD = 0x0054DE90;
constexpr uintptr_t HELI_ROCKET_CALL_SITES[2] = {0x0055B8E2, 0x0055B9BC};
constexpr size_t    PLANE_CALL_WINDOW = 0x30;

// Installs the six detours, and the plane's when its lead checks out. False if
// any of the six failed; the ones that did install stay, and each failure says
// in the log what it costs.
bool InstallHeliHooks();
void RemoveHeliHooks();

// Wires HeliBridge. Replicas are only offered when the ProcessControl detour
// is in, because a replica without it runs the real AI - after this
// machine's player, with this machine's wanted level.
void AddHeliToBridge(WorldBridge &bridge);

} // namespace coopiii::game
