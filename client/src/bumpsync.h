// Ramming a car another machine simulates (docs/protocol.md 1.71).
//
// Our copy of somebody else's car is put where its owner says after every
// frame of physics, so whatever our car did to it in a collision was undone
// the same frame: the copy stood like a car bolted to the road and our car
// bounced off it. Two halves fix that, both here as arithmetic so clienttest
// runs all of it.
//
// Ours, the rammer's: for a short window after our car hits the copy, our
// engine keeps it - the correction stops, the hit moves it - and then it is
// blended back onto its owner's stream rather than snapped. What the
// collision did to its speed and spin goes to the owner as C_VehicleBump.
//
// The owner's: a bump lands on its real car after BUMP_HOLD_MS, unless its own
// engine had the same two cars touching around then, in which case its own
// collision was this one and the bump is dropped. Each real car takes one
// collision either way, and the dents come off the owner's VehicleDamage and
// travel the way they always did (docs/cardamage.md), so nothing is counted
// twice.
#pragma once

#include "interp.h"

#include <coopiii/protocol.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coopiii {

// Less than this and nothing is sent: a car resting against ours.
constexpr float    BUMP_MIN_IMPULSE   = 5.0f;
// How long our engine keeps a copy after the last contact, and the most it
// keeps one however long the shoving goes on. Long enough for the bump to
// reach the owner, sit out its hold and come back on the owner's stream.
constexpr uint32_t BUMP_LOOSE_MS      = 600;
constexpr uint32_t BUMP_LOOSE_MAX_MS  = 3000;
// And the blend back onto the stream after it.
constexpr uint32_t BUMP_BLEND_MS      = 400;
// How long the owner waits before putting a bump on its car: its own copy of
// our car arrives VehicleInterpBuffer::DELAY_MS late, so if its engine is going
// to see the same collision it sees it inside this.
constexpr uint32_t BUMP_HOLD_MS       = 250;
// A bump older than this when it is judged is not this collision any more.
constexpr uint32_t BUMP_STALE_MS      = 2000;
// The most a bump may change a car's speed, per engine step: 50 m/s, and half
// a radian a step of spin. Far past a real collision, short of a car leaving
// the map.
constexpr float    BUMP_MOVE_MAX      = 1.0f;
constexpr float    BUMP_TURN_MAX      = 0.5f;
// The two cars on the owner's screen, past which a bump is not about them.
constexpr float    BUMP_APPLY_RANGE_M = 20.0f;

constexpr size_t MAX_BUMP_TOUCHES = 16;
constexpr size_t MAX_PENDING_BUMPS = 8;

// ---- the wire ---------------------------------------------------------------

inline int16_t PackBumpComponent(float v) {
	if (!std::isfinite(v))
		return 0;
	const float scaled = v * VEHICLE_BUMP_UNITS;
	if (scaled >= 32767.0f)
		return 32767;
	if (scaled <= -32767.0f)
		return -32767;
	return static_cast<int16_t>(std::lround(scaled));
}

inline float UnpackBumpComponent(int16_t v) { return static_cast<float>(v) / VEHICLE_BUMP_UNITS; }

inline uint16_t PackBumpImpulse(float impulse) {
	if (!(impulse > 0.0f))
		return 0;
	if (impulse >= 65535.0f)
		return 65535;
	return static_cast<uint16_t>(std::lround(impulse));
}

// Shortened to `limit` keeping its direction, as HeldMoveSpeed does.
inline Vec3 BumpHeldTo(const Vec3 &v, float limit) {
	const float sq = v.x * v.x + v.y * v.y + v.z * v.z;
	if (!(sq > limit * limit))
		return v;
	const float k = limit / std::sqrt(sq);
	return Vec3{v.x * k, v.y * k, v.z * k};
}

// What a received bump asks of the car, bounded.
inline void BumpFromWire(const VehicleBumpBody &body, Vec3 &move, Vec3 &turn, float &impulse) {
	move    = BumpHeldTo(Vec3{UnpackBumpComponent(body.move[0]), UnpackBumpComponent(body.move[1]),
	                          UnpackBumpComponent(body.move[2])},
	                     BUMP_MOVE_MAX);
	turn    = BumpHeldTo(Vec3{UnpackBumpComponent(body.turn[0]), UnpackBumpComponent(body.turn[1]),
	                          UnpackBumpComponent(body.turn[2])},
	                     BUMP_TURN_MAX);
	impulse = static_cast<float>(body.impulse);
}

// ---- the rammer's side ------------------------------------------------------

// What our engine says happened to one copy this frame (WorldBridge::
// ReadCopyContact). Only `fresh` copies are believed: the engine ran their
// ProcessControl this frame, so the collision record is this frame's and
// `move`/`turn` are what the frame's collisions did, the car's own driving
// already counted before them.
struct CopyContact {
	bool     fresh   = false;
	float    impulse = 0.0f;           // m_fDamageImpulse, the biggest this frame
	uint8_t  piece   = 0;              // m_nDamagePieceType
	bool     byOurWheel = false;       // what hit it: the car our player drives,
	uint16_t byHosted   = INVALID_NETID;   // a traffic car our engine hosts,
	int32_t  byHandle   = -1;          // or any other vehicle, by pool reference
	Vec3     move{};
	Vec3     turn{};
};

// One copy's half, kept on its roster row.
struct BumpOut {
	// Our engine has the copy, not its owner's stream.
	bool     loose        = false;
	uint32_t looseSinceMs = 0;
	uint32_t looseUntilMs = 0;
	// Then it is blended back: the copy's pose less the stream's when the
	// window closed, taken off over BUMP_BLEND_MS.
	bool     blending     = false;
	uint32_t blendSinceMs = 0;
	Vec3     blendOffset{};
	Quat     blendFrom{0.0f, 0.0f, 0.0f, 1.0f};
	// The collision, summed until it goes out.
	bool     pending  = false;
	Vec3     move{};
	Vec3     turn{};
	float    impulse  = 0.0f;
	uint8_t  piece    = 0;
	uint32_t sentAtMs = 0;
	bool     sentOnce = false;
};

// Our car has just hit the copy. It is kept loose from now, and with
// `forward` what the hit did is added to what goes to its owner. A car nobody
// holds is loose and not forwarded: the shove's custody moves it.
inline void NoteBump(BumpOut &b, const CopyContact &c, bool forward, uint32_t nowMs) {
	if (!b.loose) {
		b.loose        = true;
		b.looseSinceMs = nowMs;
	}
	b.blending = false;
	const uint32_t held = nowMs - b.looseSinceMs;
	b.looseUntilMs = held + BUMP_LOOSE_MS > BUMP_LOOSE_MAX_MS ? b.looseSinceMs + BUMP_LOOSE_MAX_MS
	                                                          : nowMs + BUMP_LOOSE_MS;
	if (!forward)
		return;
	b.pending = true;
	b.move.x += c.move.x;
	b.move.y += c.move.y;
	b.move.z += c.move.z;
	b.turn.x += c.turn.x;
	b.turn.y += c.turn.y;
	b.turn.z += c.turn.z;
	if (c.impulse > b.impulse) {
		b.impulse = c.impulse;
		b.piece   = c.piece;
	}
}

// The bump that is due, if one is: at most one every VEHICLE_BUMP_EVERY_MS a
// car, with everything since the last folded into it.
inline bool TakeBump(BumpOut &b, uint32_t nowMs, VehicleBumpBody &out) {
	if (!b.pending || (b.sentOnce && nowMs - b.sentAtMs < VEHICLE_BUMP_EVERY_MS))
		return false;
	out.move[0] = PackBumpComponent(b.move.x);
	out.move[1] = PackBumpComponent(b.move.y);
	out.move[2] = PackBumpComponent(b.move.z);
	out.turn[0] = PackBumpComponent(b.turn.x);
	out.turn[1] = PackBumpComponent(b.turn.y);
	out.turn[2] = PackBumpComponent(b.turn.z);
	out.impulse = PackBumpImpulse(b.impulse);
	out.piece   = b.piece;
	out.pad     = 0;
	b.pending   = false;
	b.move      = Vec3{};
	b.turn      = Vec3{};
	b.impulse   = 0.0f;
	b.piece     = 0;
	b.sentAtMs  = nowMs;
	b.sentOnce  = true;
	return true;
}

enum class BumpPhase : uint8_t {
	Stream,       // corrected as always
	Loose,        // not corrected: our engine has it
	StartBlend,   // the window has just closed; BeginBlendBack, then Blend
	Blend,        // corrected onto BlendedPose
};

inline BumpPhase StepBump(BumpOut &b, uint32_t nowMs) {
	if (b.loose) {
		if (static_cast<int32_t>(nowMs - b.looseUntilMs) < 0)
			return BumpPhase::Loose;
		b.loose = false;
		return BumpPhase::StartBlend;
	}
	if (b.blending) {
		if (nowMs - b.blendSinceMs < BUMP_BLEND_MS)
			return BumpPhase::Blend;
		b.blending = false;
	}
	return BumpPhase::Stream;
}

// The window has closed with the copy at `ours` and the stream at `stream`.
// False, and no blend, when they are a teleport apart: that is the stream
// being right about something else, and it is snapped to as always.
inline bool BeginBlendBack(BumpOut &b, const VehicleTransform &ours, const VehicleTransform &stream,
                           uint32_t nowMs) {
	const Vec3  off{ours.pos.x - stream.pos.x, ours.pos.y - stream.pos.y,
	                ours.pos.z - stream.pos.z};
	const float sq = off.x * off.x + off.y * off.y + off.z * off.z;
	if (!(sq <= VehicleInterpBuffer::SNAP_DISTANCE * VehicleInterpBuffer::SNAP_DISTANCE))
		return false;
	b.blending     = true;
	b.blendSinceMs = nowMs;
	b.blendOffset  = off;
	b.blendFrom    = ours.rot;
	return true;
}

// Where the copy goes this frame of the blend: the stream, plus what is left
// of the offset, and turned from where the window left it to the stream's.
inline VehicleTransform BlendedPose(const BumpOut &b, const VehicleTransform &stream,
                                    uint32_t nowMs) {
	float t = static_cast<float>(nowMs - b.blendSinceMs) / static_cast<float>(BUMP_BLEND_MS);
	t       = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	const float      left = 1.0f - t;
	VehicleTransform out;
	out.pos = Vec3{stream.pos.x + b.blendOffset.x * left, stream.pos.y + b.blendOffset.y * left,
	               stream.pos.z + b.blendOffset.z * left};
	out.rot = Slerp(b.blendFrom, stream.rot, t);
	return out;
}

// ---- the owner's side -------------------------------------------------------

struct BumpTouch {
	uint16_t ours   = INVALID_NETID;
	uint16_t theirs = INVALID_NETID;
	uint32_t atMs   = 0;
};

struct PendingBump {
	bool            active    = false;
	uint8_t         from      = INVALID_PLAYER;
	uint32_t        arrivedMs = 0;
	VehicleBumpBody body{};
};

enum class BumpVerdict : uint8_t { Wait, Apply, Drop };

// The last few times our engine had a car of ours (driven, settled, or hosted
// traffic) touch a copy of somebody else's, and the bumps waiting to be put
// on ours.
class BumpInbox {
public:
	void NoteTouch(uint16_t ours, uint16_t theirs, uint32_t nowMs) {
		if (ours == INVALID_NETID || theirs == INVALID_NETID)
			return;
		BumpTouch *slot = &m_touches[0];
		for (BumpTouch &t : m_touches) {
			if (t.ours == ours && t.theirs == theirs) {
				slot = &t;
				break;
			}
			if (static_cast<int32_t>(t.atMs - slot->atMs) < 0 || t.ours == INVALID_NETID)
				slot = &t;
		}
		slot->ours   = ours;
		slot->theirs = theirs;
		slot->atMs   = nowMs;
	}

	// Did our engine have these two touching at or after `sinceMs`?
	bool TouchedSince(uint16_t ours, uint16_t theirs, uint32_t sinceMs) const {
		for (const BumpTouch &t : m_touches)
			if (t.ours == ours && t.theirs == theirs &&
			    static_cast<int32_t>(t.atMs - sinceMs) >= 0)
				return true;
		return false;
	}

	// False when it is full of bumps nobody has judged yet.
	bool Add(uint8_t from, const VehicleBumpBody &body, uint32_t nowMs) {
		for (PendingBump &p : m_pending)
			if (!p.active) {
				p.active    = true;
				p.from      = from;
				p.arrivedMs = nowMs;
				p.body      = body;
				return true;
			}
		return false;
	}

	BumpVerdict Judge(const PendingBump &p, uint32_t nowMs) const {
		if (TouchedSince(p.body.netId, p.body.byNetId, p.arrivedMs - BUMP_HOLD_MS))
			return BumpVerdict::Drop;
		const uint32_t age = nowMs - p.arrivedMs;
		if (age > BUMP_STALE_MS)
			return BumpVerdict::Drop;
		return age >= BUMP_HOLD_MS ? BumpVerdict::Apply : BumpVerdict::Wait;
	}

	PendingBump *Pending() { return m_pending; }
	size_t       PendingCount() const {
		size_t n = 0;
		for (const PendingBump &p : m_pending)
			n += p.active ? 1 : 0;
		return n;
	}

	void Clear() { *this = BumpInbox{}; }

private:
	BumpTouch   m_touches[MAX_BUMP_TOUCHES];
	PendingBump m_pending[MAX_PENDING_BUMPS];
};

} // namespace coopiii
