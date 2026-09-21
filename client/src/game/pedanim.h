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
// because they need the protocol's Vec3, and AgentPad includes addresses.h
// on purpose without linking the sdk.
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
