// When to put back a remote player's animation that this machine's own engine
// took off their ped, and when to wait. Pure: tools/clienttest covers it.
//
// The snapshot names two animations, the dominant base and the dominant
// partial (animId / animId2). ped.cpp used to blend each one when its id
// changed and trust the clump after that. The observer's engine does not
// leave the clump alone, though: it runs CPed::ProcessControl on the replica
// like on any ped, and three things in there take animations away without the
// id on the wire moving at all (addresses.h, "landing and getting up"):
//
//   landing     CPed::SetLanding writes -1000 into the blendDelta of every
//               partial and sets no DELETEFADEDOUT. UpdateBlend then parks each
//               one at weight 0, delta 0, and keeps it. Nothing revives it.
//   getting up  CPed::PedGetupCB does the same wipe, then SetMoveAnim blends a
//               base of its own over the one CoopIII put on.
//   dry land    ProcessBuoyancy's arm for a ped pulled out of the water.
//
// On the owner's screen their own weapon code puts the aim back the same frame,
// so the wire keeps naming it. On ours the association is still found by id,
// looks alive to a lookup, and weighs nothing: the arm with the gun is gone
// until the owner does something that changes the id.
//
// The rule is the one the drive-by already follows (a delta below zero counts
// as not there): judge an association by its weight, not by being found.
#pragma once

#include "addresses.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

// What one association is doing, as far as being seen goes.
enum class AnimLife : uint8_t {
	SPENT,       // weight gone or going, and nothing will delete it or bring it back
	CONDEMNED,   // fading and deleted when it gets there
	LIVE,        // on screen, or fading in
};

// UpdateBlend's own tests: `delta < 0` for fading, `[+30h] and 4` for being
// deleted at the bottom. A negative delta without the flag is a wipe in
// progress, since it can only end at 0/0. NaN counts as spent, so a corrupt
// association gets one fresh blend rather than trust.
inline AnimLife ClassifyAnim(float blendAmount, float blendDelta, int32_t flags) {
	if (blendDelta < 0.0f)
		return (flags & ASSOC_DELETEFADEDOUT) ? AnimLife::CONDEMNED : AnimLife::SPENT;
	if (blendAmount > 0.0f || blendDelta > 0.0f)
		return AnimLife::LIVE;
	return AnimLife::SPENT;
}

// Of two associations with the same id, the one to judge the clump by.
inline bool Livelier(AnimLife a, AnimLife b) {
	return static_cast<uint8_t>(a) > static_cast<uint8_t>(b);
}

// The engine's own whole-body moves, all partials: get-ups 90h..93h, the jump
// 94h..96h, the fall and the landing 97h..9Ah.
inline bool IsEngineMoveAnim(uint16_t id) {
	return id >= ANIM_STD_GET_UP && id <= ANIM_STD_FALL_COLLAPSE;
}

// Airborne or landing by the replica's own engine: the two bits SetInTheAir
// and SetLanding write (PedLandCB clears the second), and PED_JUMP.
inline bool PedAirborneOrLanding(uint8_t flagsB, uint32_t pedState) {
	return (flagsB & (offs::PED_IS_IN_THE_AIR | offs::PED_IS_LANDING)) != 0 ||
	       pedState == PEDSTATE_JUMP;
}

// Knocked down or getting up by its own engine. Whatever SetFall picked is on
// screen, KO or high impact, and it ends in SetGetUp and PedGetupCB's wipe.
inline bool PedDownOrGettingUp(uint32_t pedState) {
	return pedState == PEDSTATE_FALL || pedState == PEDSTATE_GETUP;
}

// Is this association one of the family, still playing, and not the overlay
// we are about to blend? Blending a partial condemns every other partial
// (BlendAnimation, 0x004037E8..0x00403827), so putting the gun back while the
// landing plays ends the landing. Only while it is running, so a glide parked
// on its last frame does not hold anything off for the rest of the fall. And
// never when the wire itself names one of the family: the owner jumping or
// landing is the same move, not a fight over it.
inline bool EngineMoveHoldsOff(uint16_t want, uint16_t id, float blendAmount,
                               float blendDelta, int32_t flags) {
	return !IsEngineMoveAnim(want) && id != want && IsEngineMoveAnim(id) &&
	       (flags & ASSOC_RUNNING) != 0 &&
	       ClassifyAnim(blendAmount, blendDelta, flags) == AnimLife::LIVE;
}

// The `engineMove` PlanOverlay takes. Down or getting up holds on the state
// alone, since a knockdown is parked on its last frame for as long as the ped
// lies there. In the air it takes a family anim actually playing as well, so a
// bit the engine never cleared cannot hold an overlay off by itself.
inline bool EngineMoveHolds(bool down, bool airborne, bool familyAnimPlaying) {
	return down || (airborne && familyAnimPlaying);
}

enum class OverlayStep : uint8_t {
	KEEP,      // on screen: leave its weight alone
	APPLY,     // missing, spent, or condemned while the owner still plays it
	HOLD,      // needs putting back, after the engine's own move is done
	LET_END,   // condemned and the owner has stopped running theirs
};

// The overlay the wire names, against what the clump has for it.
//
// `present`/`life`: the liveliest association with that id. `ownerRunning` is
// PF_ANIM2_RUNNING. `engineMove`: EngineMoveHolds.
//
// LET_END is the rocket launcher's rule (AGENTS.md, "The rocket launcher plays
// once"): an animation that finished and is fading, whose owner has stopped
// playing theirs, is meant to be ending. A spent one is not that - nothing
// ended it, the landing did - so it is put back whether or not it runs, which
// is what brings back an aim, since an aim is a weapon anim held still.
inline OverlayStep PlanOverlay(bool present, AnimLife life, bool ownerRunning,
                               bool engineMove) {
	if (present && life == AnimLife::LIVE)
		return OverlayStep::KEEP;
	if (present && life == AnimLife::CONDEMNED && !ownerRunning)
		return OverlayStep::LET_END;
	return engineMove ? OverlayStep::HOLD : OverlayStep::APPLY;
}

// The base animation: blend it when the id changed, and also when the id
// did not change but nothing with it is live any more. No hold, because every
// move in the family is a partial and a base blend does not touch partials.
inline bool BaseNeedsBlend(uint16_t want, uint16_t applied, bool liveOnClump) {
	if (want == ANIM_NONE)
		return false;
	return want != applied || !liveOnClump;
}

} // namespace coopiii::game
