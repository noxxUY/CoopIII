// A remote player's legs: which of the five walking animations carry how much
// weight, and where in the stride they are, at the instant the pose is drawn
// (docs/protocol.md 1.8.4).
//
// No engine dependency, same as interp.h and remotebody.h, so clienttest
// covers every decision here. ped.cpp only turns a LegPose into blend amounts,
// speeds and seeks on the clump.
#pragma once

#include "coopiii/protocol.h"

#include <cmath>
#include <cstdint>

namespace coopiii {

// Indices into PlayerStateBody::locoWeight, and the AnimationIds they stand
// for (LOCO_ANIM_IDS).
constexpr size_t LOCO_WALK      = 0;
constexpr size_t LOCO_RUN       = 1;
constexpr size_t LOCO_SPRINT    = 2;
constexpr size_t LOCO_IDLE      = 3;
constexpr size_t LOCO_STARTWALK = 4;

// One of the five a walking style holds.
inline bool IsLocoAnim(uint16_t id) { return id < LOCO_ANIMS; }

// The three the engine flags ASSOC_MOVEMENT and plays on one shared,
// normalised phase: walk, run and sprint. The start-walk and the idle play on
// their own clocks.
inline bool IsStrideAnim(uint16_t id) { return id <= LOCO_ANIM_IDS[LOCO_SPRINT]; }

// ---- on the wire -------------------------------------------------------------

inline uint8_t LocoWeightOnWire(float w) {
	if (!(w > 0.0f))
		return 0;   // NaN too
	if (w >= 1.0f)
		return 255;
	return static_cast<uint8_t>(w * 255.0f + 0.5f);
}

inline float LocoWeightOffWire(uint8_t w) { return w / 255.0f; }

// currentTime over totalLength, as 0..255. A time at or past the length is
// the top of the next stride, not the end of this one.
inline uint8_t LocoPhaseOnWire(float time, float length) {
	if (!(length > 0.0f) || !std::isfinite(time))
		return 0;
	float f = std::fmod(time / length, 1.0f);
	if (f < 0.0f)
		f += 1.0f;
	const int q = static_cast<int>(f * 256.0f + 0.5f);
	return static_cast<uint8_t>(q & 0xFF);
}

inline float LocoPhaseOffWire(uint8_t p) { return p / 256.0f; }

// ---- one instant's legs ------------------------------------------------------

struct LegPose {
	// Drive the legs from this at all. Only while the strongest whole-body
	// animation the sender has is one of the five; a car seat, a stop from a
	// sprint or a fight stance go the old way, one id at a time.
	bool  valid = false;
	float weight[LOCO_ANIMS] = {};

	// Where walk, run and sprint are in their stride, 0..1. Unknown past the
	// newest snapshot: the legs play on from where they are instead.
	bool  havePhase = false;
	float phase     = 0.0f;

	// The start-walk's own time, when it is the strongest. It is not a
	// movement animation, so it has a clock of its own and plays once.
	bool  haveStartTime = false;
	float startTime     = 0.0f;

	// The sender's rate for its movement animations (CAnimBlendAssociation::
	// speed), 1 but for the adrenaline pill.
	float speed = 1.0f;
};

inline float LegSpeedOffWire(const PlayerStateBody &b) {
	if (!IsStrideAnim(b.animId) || !(b.animSpeed >= 0.5f) || !(b.animSpeed <= 2.5f))
		return 1.0f;
	return b.animSpeed;
}

// One snapshot's legs, as sent. A sender that sent no weights at all still
// names its strongest animation, and that one is played at full weight.
inline LegPose LegsOf(const PlayerStateBody &b) {
	LegPose legs;
	legs.valid = IsLocoAnim(b.animId);
	legs.speed = LegSpeedOffWire(b);

	float sum = 0.0f, stride = 0.0f;
	for (size_t i = 0; i < LOCO_ANIMS; ++i) {
		legs.weight[i] = LocoWeightOffWire(b.locoWeight[i]);
		sum += legs.weight[i];
		if (IsStrideAnim(LOCO_ANIM_IDS[i]))
			stride += legs.weight[i];
	}
	if (sum <= 0.0f) {
		for (float &w : legs.weight)
			w = 0.0f;
		if (legs.valid)
			legs.weight[b.animId] = 1.0f;
	} else if (stride > 0.0f) {
		legs.havePhase = true;
		legs.phase     = LocoPhaseOffWire(b.locoPhase);
	}
	if (b.animId == LOCO_ANIM_IDS[LOCO_STARTWALK]) {
		legs.haveStartTime = std::isfinite(b.animTime) && b.animTime >= 0.0f;
		legs.startTime     = legs.haveStartTime ? b.animTime : 0.0f;
	}
	return legs;
}

// Wrap a phase into [0, 1).
inline float WrapPhase(float p) {
	if (!std::isfinite(p))
		return 0.0f;
	float f = std::fmod(p, 1.0f);
	return f < 0.0f ? f + 1.0f : f;
}

// How far `target` is ahead of `local`, the short way round: [-0.5, 0.5).
inline float PhaseGap(float target, float local) {
	float d = WrapPhase(target) - WrapPhase(local);
	if (d >= 0.5f)
		d -= 1.0f;
	else if (d < -0.5f)
		d += 1.0f;
	return d;
}

// The legs between two snapshots, `t` of the way from `a` to `b`, `ageMs`
// past `a`. Whether to drive them at all steps with `a`, the same as the rest
// of the body (BodyTimeline::BodyAt); the weights and the stride are blended,
// so a walk turning into a run is a walk turning into a run and not two
// 40 ms steps.
inline LegPose LerpLegs(const LegPose &a, const LegPose &b, float t, float ageSeconds) {
	LegPose out = a;
	if (!(t > 0.0f))
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	if (a.valid && b.valid) {
		for (size_t i = 0; i < LOCO_ANIMS; ++i)
			out.weight[i] = a.weight[i] + (b.weight[i] - a.weight[i]) * t;
		out.speed = a.speed + (b.speed - a.speed) * t;
	}
	if (a.havePhase && b.havePhase)
		out.phase = WrapPhase(a.phase + PhaseGap(b.phase, a.phase) * t);
	if (a.haveStartTime && ageSeconds > 0.0f)
		out.startTime = a.startTime + ageSeconds;
	return out;
}

// ---- what the pose says when the legs do not ---------------------------------

// Below this a player is standing, above it they are going somewhere. The
// player's own walk covers about 1.5 m/s and the run about 5.
constexpr float LEGS_MOVING_MPS = 1.5f;
constexpr float LEGS_RUN_MPS    = 3.5f;
// Past this vertically they are in the air, and a jump or a fall has the body.
constexpr float LEGS_AIRBORNE_MPS = 1.0f;

// Legs that say "standing" on a pose that is visibly travelling: a sender
// whose clump holds something this does not model. The stride follows the
// ground instead, so nobody glides across the street on an idle. Never for a
// rider (the vehicle moves them, not their legs) or anybody in the air.
inline LegPose LegsForGround(LegPose legs, float planarMps, float verticalMps, bool riding) {
	if (!legs.valid || riding || !(planarMps >= LEGS_MOVING_MPS) ||
	    !(std::fabs(verticalMps) < LEGS_AIRBORNE_MPS))
		return legs;
	float moving = 0.0f;
	for (size_t i = 0; i < LOCO_ANIMS; ++i)
		if (i != LOCO_IDLE)
			moving += legs.weight[i];
	if (moving > 0.05f)
		return legs;
	for (float &w : legs.weight)
		w = 0.0f;
	legs.weight[planarMps >= LEGS_RUN_MPS ? LOCO_RUN : LOCO_WALK] = 1.0f;
	legs.havePhase     = false;
	legs.haveStartTime = false;
	legs.speed         = 1.0f;
	return legs;
}

// ---- keeping the stride with the sender's -------------------------------------

// A fifth of a stride out and it is not worth catching up: jump there.
constexpr float LEGS_PHASE_SEEK = 0.2f;
// Otherwise the gap is closed by playing a little fast or slow: this much
// extra speed per stride of gap, and never more than LEGS_SPEED_TRIM either
// way. A tenth of a stride behind plays 20% fast, which closes it in about
// half a second; a timestamp that is a frame off moves the legs by a few
// percent instead of making them stutter.
constexpr float LEGS_PHASE_GAIN = 2.0f;
constexpr float LEGS_SPEED_TRIM = 0.2f;
// The start-walk plays once, so it is only ever put back where it should be.
constexpr float LEGS_START_SEEK_S = 0.08f;

struct StrideStep {
	bool  seek  = false;
	float scale = 1.0f;
};

inline StrideStep PlanStride(float target, float local) {
	StrideStep step;
	const float gap = PhaseGap(target, local);
	if (std::fabs(gap) > LEGS_PHASE_SEEK) {
		step.seek = true;
		return step;
	}
	float trim = gap * LEGS_PHASE_GAIN;
	if (trim > LEGS_SPEED_TRIM)
		trim = LEGS_SPEED_TRIM;
	else if (trim < -LEGS_SPEED_TRIM)
		trim = -LEGS_SPEED_TRIM;
	step.scale = 1.0f + trim;
	return step;
}

// How fast a weight on the clump follows the one it should have, per second.
// Ten is a tenth of a second from nothing to everything: quick enough that
// the legs never lag the pose, slow enough to hide the jump when something
// else (our own engine, a seat, a stop) has had the clump for a moment.
constexpr float LEGS_BLEND_RATE = 10.0f;

inline float ApproachWeight(float current, float target, float dtSeconds) {
	if (!std::isfinite(current))
		current = 0.0f;
	const float step = LEGS_BLEND_RATE * (dtSeconds > 0.0f ? dtSeconds : 0.0f);
	if (current < target)
		return current + step >= target ? target : current + step;
	return current - step <= target ? target : current - step;
}

// ---- legs turned toward where the body is going (docs/protocol.md 1.54) -----
//
// With the PC's mouse camera the player faces where the camera looks and walks
// wherever the keys say. CPed::CalculateNewVelocity (0x004C73F0) does the
// diagonal: when the walk angle is within 50 degrees of the facing it sends
// the ped along the walk angle instead of straight ahead, and turns both upper
// legs toward it with CPedIK::RotateTorso (the two calls at 0x004C78A7 and
// 0x004C78C2, on m_pFrames[7] and [8]). Past 50 degrees the anim group swaps
// to a strafe instead and the legs stay put. All of it is gated on
// `FindPlayerPed() == this`, so a remote player's copy moves diagonally with
// its legs running straight ahead. The copy has no pad, so the walk angle is
// the drawn pose's velocity against its heading.
//
// The constants are the retail ones: 0x005F84AC (50 degrees, the window),
// 0x005F84B4 (100 degrees, past which the angle is taken as walking backward)
// and 0x005F848C (pi).
constexpr float LEG_TWIST_PI       = 3.14159265f;
constexpr float LEG_TWIST_MAX_RAD  = 0.8726646f;
constexpr float LEG_TWIST_FLIP_RAD = 1.745329f;
// Below this the velocity says nothing about a direction.
constexpr float LEG_TWIST_MIN_MPS = 0.5f;
// How fast the drawn twist follows the target, radians a second. The engine
// sets it outright from the pad; ours comes off a velocity that steps every
// 40 ms, so it is eased: 50 degrees in about a tenth of a second.
constexpr float LEG_TWIST_RATE = 8.0f;

// CPed::CanStrafeOrMouseControl (0x004CE7D0): PED_NONE, IDLE, FLEE_POS,
// FLEE_ENTITY, ATTACK, FIGHT, AIM_GUN and JUMP. The owner's state, off the wire.
inline bool LegTwistState(uint8_t pedState) {
	return pedState == 0 || pedState == 1 || pedState == 8 || pedState == 9 ||
	       pedState == 16 || pedState == 17 || pedState == 22 || pedState == 35;
}

inline float WrapLegAngle(float a) {
	if (!std::isfinite(a))
		return 0.0f;
	a = std::fmod(a + LEG_TWIST_PI, 2.0f * LEG_TWIST_PI);
	if (a < 0.0f)
		a += 2.0f * LEG_TWIST_PI;
	return a - LEG_TWIST_PI;
}

// The upper-leg yaw for a ped facing `heading` and moving at (vx, vy) m/s:
// zero when there is none to give.
inline float LegTwistFor(float heading, float vx, float vy, bool stateAllows) {
	if (!stateAllows || !std::isfinite(heading))
		return 0.0f;
	const float mps = std::sqrt(vx * vx + vy * vy);
	if (!(mps >= LEG_TWIST_MIN_MPS))
		return 0.0f;
	float yaw = WrapLegAngle(std::atan2(-vx, vy) - heading);
	if (yaw > LEG_TWIST_FLIP_RAD)
		yaw -= LEG_TWIST_PI;
	else if (yaw < -LEG_TWIST_FLIP_RAD)
		yaw += LEG_TWIST_PI;
	if (!(yaw > -LEG_TWIST_MAX_RAD && yaw < LEG_TWIST_MAX_RAD))
		return 0.0f;
	return yaw;
}

inline float ApproachLegTwist(float current, float target, float dtSeconds) {
	if (!std::isfinite(current))
		current = 0.0f;
	const float step = LEG_TWIST_RATE * (dtSeconds > 0.0f ? dtSeconds : 0.0f);
	if (current < target)
		return current + step >= target ? target : current + step;
	return current - step <= target ? target : current - step;
}

// Which replicas get their legs turned this frame, and by how much. Filled
// from PreFrame, read inside the same CGame::Process by the call-site
// redirect on CalculateNewVelocity. An entry only counts for the frame it was
// written in, so a ped that stops, or a pool slot reused, falls out by itself.
// Keyed by pointer, compared and never followed.
class LegTwistTable {
public:
	void Clear() {
		for (Entry &e : m_entries)
			e = Entry{};
	}

	void Set(const void *ped, float yaw, uint32_t frame) {
		if (!ped)
			return;
		Entry *slot = nullptr;
		for (Entry &e : m_entries) {
			if (e.ped == ped) {
				slot = &e;
				break;
			}
			if (!slot && (!e.ped || e.frame != frame))
				slot = &e;
		}
		if (!slot)
			return;
		slot->ped   = ped;
		slot->yaw   = yaw;
		slot->frame = frame;
	}

	bool Find(const void *ped, uint32_t frame, float &yaw) const {
		if (!ped)
			return false;
		for (const Entry &e : m_entries) {
			if (e.ped == ped && e.frame == frame) {
				yaw = e.yaw;
				return true;
			}
		}
		return false;
	}

private:
	struct Entry {
		const void *ped   = nullptr;
		float       yaw   = 0.0f;
		uint32_t    frame = 0;
	};
	Entry m_entries[MAX_PLAYERS];
};

} // namespace coopiii
