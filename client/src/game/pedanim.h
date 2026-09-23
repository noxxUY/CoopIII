// Deciding *how* to replay a remote player's animation, weapon and aim,
// separated out from the code that actually touches game memory.
//
// Everything here is pure arithmetic over the constants in addresses.h, so
// it compiles and runs outside GTA III and is covered by tools/clienttest.
// That matters more than it looks: the single most dangerous decision in the
// whole animation path is *which anim group an id may be looked up in*, and
// that's a decision, not a memory access. Get it wrong and you read past the
// end of a four-element array and hand the result to the blender.
//
// docs/protocol.md §1.8.1 has the long version. Short version:
//
//   CAnimManager::BlendAnimation(clump, group, animId, delta) resolves the
//   animation as ms_aAnimAssocGroups[group].assocList[animId], with no
//   bounds check anywhere in it (retail CAnimBlendAssocGroup::GetAnimation
//   is `shl eax,6 / add eax,[ecx] / ret`). ASSOCGRP_STD holds the whole
//   AnimationId namespace; every other group is a walking *style* holding
//   four or five locomotion anims. So only ANIM_STD_WALK..ANIM_STD_IDLE may
//   be played out of a ped's own m_animGroup.
#pragma once

#include "addresses.h"

#include <coopiii/protocol.h>

#include <cmath>
#include <cstdint>

namespace coopiii::game {

// A group index is about to become a subscript into ms_aAnimAssocGroups, so
// it gets bounded before use - including the one read out of the ped's own
// m_animGroup, since that's just a field in memory like any other.
inline bool ValidAnimGroup(int group) {
	return group >= 0 && group < NUM_ANIM_ASSOC_GROUPS;
}

// How a received animId should be played on a particular ped.
struct AnimPlan {
	bool  valid      = false;   // false: do not call BlendAnimation at all
	int   group      = ASSOCGRP_STD;
	float blendDelta = 0.0f;
};

// `pedGroup` is CPed::m_animGroup; `pedGroupCount` and `stdGroupCount` are
// the two groups' CAnimBlendAssocGroup::numAssociations, read at the call
// site because they're set once when the anim files load and never again
// after that.
//
// Falling back to ASSOCGRP_STD for a locomotion id the ped's own group
// lacks isn't really a compromise: every group's table is parallel, so
// index 1 is "that group's run" and ASSOCGRP_STD's index 1 is the generic
// run. A generic walk is a cosmetic downgrade. An out-of-bounds read is not.
inline AnimPlan PlanAnim(uint16_t animId, int pedGroup, int pedGroupCount,
                         int stdGroupCount) {
	AnimPlan plan;
	if (animId == ANIM_NONE || stdGroupCount <= 0)
		return plan;

	if (animId <= ANIM_STD_IDLE) {
		// Locomotion. The engine's own CPed::SetMoveAnim uses 1.0 here - a
		// slow crossfade. A walk that snaps straight into a run looks wrong
		// in a way that's hard to name and easy to spot.
		plan.blendDelta = 1.0f;
		if (ValidAnimGroup(pedGroup) && animId < pedGroupCount) {
			plan.group = pedGroup;
			plan.valid = true;
			return plan;
		}
		if (animId < stdGroupCount) {
			plan.group = ASSOCGRP_STD;
			plan.valid = true;
		}
		return plan;
	}

	// Everything else - jumps, falls, knockdowns, getting up, car anims,
	// firing. CPed uses 4.0 or 8.0 for these throughout; 8.0 is for the ones
	// that need to be on screen immediately.
	if (animId < stdGroupCount) {
		plan.group      = ASSOCGRP_STD;
		plan.blendDelta = 8.0f;
		plan.valid      = true;
	}
	return plan;
}

// Which animation a received death should actually play.
//
// CPed::SetDie passes its animId straight to CAnimManager::BlendAnimation
// against ASSOCGRP_STD, so the same rule as everywhere else applies: the id
// becomes a subscript with nothing checking it. The one id that is always
// safe is ANIM_STD_NUM, the value SetDie tests for and answers by playing
// nothing at all - which makes it the right fallback when the animations
// aren't loaded yet.
//
// ANIM_NONE on the wire means the sender couldn't capture what its engine
// picked, usually because the CPed::SetDie detour didn't install. A front
// knockdown is the engine's own default for that, and it's what InflictDamage
// starts every hit with.
inline uint16_t PlanDeathAnim(uint16_t wire, int stdGroupCount) {
	if (stdGroupCount <= 0)
		return ANIM_STD_NUM;
	if (wire == ANIM_STD_NUM)
		return ANIM_STD_NUM;   // the sender really did die without one
	if (wire == ANIM_NONE || wire >= stdGroupCount)
		return ANIM_STD_KO_FRONT < stdGroupCount ? ANIM_STD_KO_FRONT : ANIM_STD_NUM;
	return wire;
}

// How many animations CoopIII will leave on a remote ped's clump.
//
// The hard limit is the engine's, and it is not a soft one: past
// MAX_CLUMP_ANIM_ASSOCS, RpAnimBlendClumpUpdateAnimations writes its node
// array over its own saved registers, its return address and its arguments
// (addresses.h has the stack map and the crash that proved it). Two are kept
// back for the engine, which adds its own animations to a ped without asking
// anybody: CPed::SetMoveAnim alone can blend a locomotion anim and fade three
// others in the same frame.
constexpr int MAX_REMOTE_ANIM_ASSOCS = MAX_CLUMP_ANIM_ASSOCS - 2;   // 9

// May another animation be added to a clump that currently has `count`?
inline bool AnimClumpHasRoom(int count) { return count < MAX_REMOTE_ANIM_ASSOCS; }

// How many have to go before one more can be added. Zero when there is room.
inline int AnimClumpSurplus(int count) {
	const int over = count - (MAX_REMOTE_ANIM_ASSOCS - 1);
	return over > 0 ? over : 0;
}

// Has a running weapon overlay reached the end of its firing loop?
//
// CPed::FireGun's last act, while the trigger is still held, is
// `weaponAnimAssoc->Start(ourWeapon->m_fAnimLoopStart)` once currentTime
// passes m_fAnimLoopEnd (re3 PedFight.cpp:712-722). So a firing weapon never
// plays past the loop end: it cycles over the part of the animation that is
// the shot, and the draw at the front and the recovery at the back are only
// seen when the attack starts and stops.
//
// An observer that lets the same animation free-run plays the whole thing
// instead, hits ASSOC_FADEOUTWHENDONE, and starts again from the draw. So
// the loop gets replicated rather than the phase re-seeded off a snapshot
// that is only accurate 25 times a second.
//
// loopEnd <= loopStart means weapon.dat has nothing useful for this weapon,
// and then the answer is always no. Reading those two values out of a file
// the player can edit is exactly the situation where "the data will be
// sensible" is not an assumption worth making.
inline bool WeaponAnimShouldLoop(float currentTime, float loopStart, float loopEnd) {
	if (!(loopEnd > loopStart) || !(loopStart >= 0.0f))
		return false;
	return currentTime > loopEnd;
}

// eMoveState arrives as a byte from a machine we don't control. The engine
// reads it as a 32-bit enum and switches on it with no default bound, so a
// value past PEDMOVE_SPRINT falls through CPed::SetMoveAnim's switch and
// leaves m_nStoredMoveState lying about what's actually playing.
inline uint32_t ClampMoveState(uint8_t wire) {
	return wire > PEDMOVE_LAST ? PEDMOVE_STILL : static_cast<uint32_t>(wire);
}

// Only 0..12 are inventory weapons with a CPed::m_weapons slot. Everything
// from WEAPONTYPE_LAST_WEAPONTYPE upward is a damage *cause* (drowning,
// falling, being run over), and indexing m_weapons with one of those runs
// off the end of the array into m_currentWeapon and beyond.
inline bool IsInventoryWeapon(uint8_t weaponType) {
	return weaponType <= WEAPONTYPE_LAST_INVENTORY;
}

// Fold an angle into [-pi, pi], the range CPed headings live in. Aim yaw is
// sampled as heading + torso yaw, which can wander outside that range;
// CGeneral::LimitRadianAngle does the same thing inside the engine itself.
//
// Uses fmod rather than a subtract-until-in-range loop, on purpose. The
// input is a float read out of game memory or off a socket, and a loop over
// something like 1e30 doesn't crash - it just hangs the game thread, which
// is the worst possible thing to diagnose.
//
// Vec3 accessors on a game object. Live here rather than in addresses.h
// because they need the protocol's Vec3, and addresses.h is meant to stay
// includable without linking the sdk.
inline Vec3 ReadVec3(void *object, size_t offset) {
	const float *v = &Field<float>(object, offset);
	return Vec3{v[0], v[1], v[2]};
}

inline void WriteVec3(void *object, size_t offset, const Vec3 &value) {
	float *v = &Field<float>(object, offset);
	v[0]     = value.x;
	v[1]     = value.y;
	v[2]     = value.z;
}

inline float WrapAngle(float radians) {
	constexpr float kPi  = 3.14159265358979323846f;
	constexpr float kTau = 2.0f * kPi;

	// Negated so NaN, which compares false against everything, falls into this path.
	if (!(radians > -1.0e6f && radians < 1.0e6f))
		return 0.0f;

	float folded = std::fmod(radians + kPi, kTau);
	if (folded < 0.0f)
		folded += kTau;
	return folded - kPi;
}

// A float that came off the wire and is about to be written into the engine.
// NaN propagates - a NaN heading reaches the ped's matrix, the matrix
// reaches collision, and the resulting fault lands somewhere with no
// obvious connection to netcode at all.
inline bool FiniteOr(float value, float fallback, float &out) {
	// Self-comparison fails only for NaN. The magnitude bound catches
	// infinities and anything else a corrupt packet might carry.
	if (value == value && value > -1.0e9f && value < 1.0e9f) {
		out = value;
		return true;
	}
	out = fallback;
	return false;
}

// ---- fire on a remote player's body ---------------------------------------
//
// What to do this frame about a remote player's flames, as a decision rather
// than as memory writes, so tools/clienttest can pin the whole truth table
// without a running game.
//
// Three inputs and they mean three different things:
//
//   wantBurning  what the owner said in their last snapshot (PF_ON_FIRE).
//                Their machine is where their fire actually is, so this is
//                the only authority on whether that player is alight.
//   ours         the fire currently on that ped is the one CoopIII lit for
//                them. Any other fire on it came from the local engine, and
//                that fire arrives with SetFlee and PED_ON_FIRE attached,
//                which is the one thing this whole feature is built to keep
//                out of the pose stream. (Retail cannot actually light a
//                bFireProof ped - CShotInfo::Update tests the flag at
//                0x0055C1D9, addresses.h - so this is a seatbelt.)
//   inControl    CPed::IsPedInControl, the engine's own gate. False for a
//                seated, dying or dead ped, and the reason this is a loop
//                and not an event handler.
//
// The rule in one line: **a remote ped's fire is ours and is burning a ped
// the engine still controls, or it is wrong.**
//
// inControl gates keeping a fire and not just starting one, and that is not
// symmetry for its own sake. CFire::ProcessFire's ped arm has a branch for a
// burning ped who is *in a car*: it reads m_pMyVehicle and writes 75.0f into
// the car's m_fHealth (`mov [edi+200h], 42960000h` at 0x00479959), which is
// how getting into a car while alight wrecks it in single player. On an
// observer that would be this machine deciding the health of somebody else's
// car from a fire this machine started, which is the one thing this whole
// feature is not allowed to do. A burning player who gets in a car stops
// burning here; their own machine wrecks their own car and the result
// arrives on the vehicle's stream like every other thing about it.
enum class FireAction : uint8_t {
	NOTHING,      // leave it alone - includes "asked to burn, can't yet"
	LIGHT,        // start one, no AI
	KEEP,         // ours and still wanted: push the extinguish time back
	EXTINGUISH,   // CFire::Extinguish, and let the engine unwind its own state
};

inline FireAction PlanRemoteFire(bool wantBurning, bool haveFire, bool ours,
                                 bool inControl) {
	if (!haveFire)
		return (wantBurning && inControl) ? FireAction::LIGHT : FireAction::NOTHING;

	// Not ours, whatever the owner says. Putting it out is also how the
	// engine's own burning-ped AI gets unwound: CFire::Extinguish calls
	// CPed::RestorePreviousState, which pops the state CPed::SetFlee stored
	// on the way in. CoopIII restores nothing by hand.
	if (!ours)
		return FireAction::EXTINGUISH;

	return (wantBurning && inControl) ? FireAction::KEEP : FireAction::EXTINGUISH;
}

// How long an observer's copy of somebody else's fire may outlive the last
// thing they said about it.
//
// Snapshots arrive at 25 Hz and the interpolation buffer extrapolates for at
// most 250 ms past the newest one, so a burning player re-arms this roughly
// every frame. It exists for the case where that stops: a stalled
// connection, a client that went away without saying goodbye. The engine
// gives a burning civilian ten seconds and a burning player 3333 ms; an
// observer needs neither, because the owner is going to say so again in
// 40 ms or not at all.
constexpr uint32_t REMOTE_FIRE_MS = 1000;

// The wantBurning above, for a pedestrian somebody else hosts.
//
// A player restates PF_ON_FIRE 25 times a second for as long as they are
// connected. A pedestrian's row only comes while he is one of the twelve his
// host streams (protocol.h, MAX_PED_STATES), so the last thing said about him
// can be old news. It counts for REMOTE_FIRE_MS and then the replica stops
// burning, which is the same cap the fire itself carries. Unsigned, so a
// WallClock wrap still reads as a short gap.
inline bool AmbientPedShouldBurn(bool saidBurning, uint32_t saidAtMs, uint32_t nowMs) {
	return saidBurning && nowMs - saidAtMs < REMOTE_FIRE_MS;
}

// A coordinate about to reach the sector grid.
//
// Writing a wild position into an entity's matrix is survivable - it's just
// a float, the entity draws somewhere silly. Handing one to CWorld::Add or
// CPhysical::RemoveAndAdd is not: both turn it into a sector index with
// `x * 0.025 + 50` and index CWorld::ms_aSectors with no bounds check
// whatsoever (re3 has asserts there; the retail build has nothing). A
// position 100 km out writes a CPtrNode pointer thousands of entries past
// the end of the array.
//
// So every coordinate that will be used to *file* an entity gets clamped to
// the world first. The margin keeps the bounding rect's far edge inside the
// grid too, since RemoveAndAdd indexes both edges, not just the centre.
inline float ClampToWorld(float value) {
	constexpr float kMargin = 2.0f * SECTOR_SIZE_XY;
	constexpr float kMin    = WORLD_MIN_XY + kMargin;
	constexpr float kMax    = WORLD_MAX_XY - kMargin;

	float v = 0.0f;
	if (!FiniteOr(value, 0.0f, v))
		return 0.0f;
	if (v < kMin)
		return kMin;
	if (v > kMax)
		return kMax;
	return v;
}

} // namespace coopiii::game
