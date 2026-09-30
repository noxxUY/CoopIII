// What a remote player's body does between two snapshots, beside the pose
// that InterpBuffer works out (docs/protocol.md 1.7.1 and 1.7.2).
//
// No engine dependency, same as interp.h, so clienttest covers all of it.
#pragma once

#include "coopiii/protocol.h"
#include "interp.h"
#include "remoteloco.h"

#include <cmath>
#include <cstdint>
#include <deque>

namespace coopiii {

// ---- the body played at the pose's instant ----------------------------------

// Every snapshot's body and ride, kept next to the pose InterpBuffer keeps, so
// the animation can be played at the same instant as the position instead of
// 100 ms ahead of it.
class BodyTimeline {
public:
	static constexpr size_t MAX_SAMPLES = InterpBuffer::MAX_SAMPLES;

	// Same rule as InterpBuffer::Push: an older or repeated instant is dropped,
	// so the two stay in step sample for sample.
	void Push(uint32_t sendTimeMs, const PlayerStateBody &body, const PlayerRideBody &ride) {
		if (!m_samples.empty() && sendTimeMs <= m_samples.back().timeMs)
			return;
		m_samples.push_back({sendTimeMs, body, ride});
		while (m_samples.size() > MAX_SAMPLES)
			m_samples.pop_front();
	}
	void   Clear() { m_samples.clear(); }
	bool   Empty() const { return m_samples.empty(); }
	size_t Size() const { return m_samples.size(); }

	// The newest body at or before `atMs`, on the sender's clock. The oldest one
	// while the clock is still behind everything, which is every stream's first
	// 100 ms. Null when there is nothing.
	//
	// A step, not a blend. The animation is a phase the engine plays on from
	// there, and a weapon or a flag has nothing in between to blend to.
	const PlayerStateBody *BodyAt(uint32_t atMs) const {
		if (m_samples.empty())
			return nullptr;
		const Sample *best = &m_samples.front();
		for (const Sample &s : m_samples) {
			if (static_cast<int32_t>(s.timeMs - atMs) > 0)
				break;
			best = &s;
		}
		return &best->body;
	}

	// Where on what they were riding at `atMs`, when they were: the offset
	// interpolated between the two samples either side, if both ride the same
	// thing; held at the newest past the end, and at the oldest before the
	// start. False when they were not riding, or were getting on or off, and
	// the world position is the answer.
	bool RideAt(uint32_t atMs, PlayerRideBody &out) const {
		if (m_samples.empty())
			return false;
		const Sample &oldest = m_samples.front();
		const Sample &newest = m_samples.back();
		if (static_cast<int32_t>(atMs - newest.timeMs) >= 0)
			return Take(newest.ride, out);
		if (static_cast<int32_t>(atMs - oldest.timeMs) <= 0)
			return Take(oldest.ride, out);
		for (size_t i = 0; i + 1 < m_samples.size(); ++i) {
			const Sample &a = m_samples[i];
			const Sample &b = m_samples[i + 1];
			if (static_cast<int32_t>(atMs - b.timeMs) > 0)
				continue;
			if (!SameRide(a.ride, b.ride))
				return false;
			const uint32_t span = b.timeMs - a.timeMs;
			const float    t    = span == 0 ? 1.0f : static_cast<float>(atMs - a.timeMs) / span;
			out         = b.ride;
			out.offset  = {a.ride.offset.x + (b.ride.offset.x - a.ride.offset.x) * t,
			               a.ride.offset.y + (b.ride.offset.y - a.ride.offset.y) * t,
			               a.ride.offset.z + (b.ride.offset.z - a.ride.offset.z) * t};
			out.heading = LerpAngle(a.ride.heading, b.ride.heading, t);
			return true;
		}
		return false;
	}

	// The legs at `atMs` (remoteloco.h): the weights and the stride blended
	// between the two snapshots either side, whether to drive them at all
	// from the one at or before, as BodyAt picks it. Past the newest the
	// newest's weights hold and the stride is left to play on, since there is
	// nothing to say where it went. False when there is nothing.
	bool LegsAt(uint32_t atMs, LegPose &out) const {
		if (m_samples.empty())
			return false;
		const Sample &oldest = m_samples.front();
		const Sample &newest = m_samples.back();
		if (static_cast<int32_t>(atMs - newest.timeMs) >= 0) {
			out = LegsOf(newest.body);
			const float age = static_cast<float>(atMs - newest.timeMs) / 1000.0f;
			out.havePhase = false;
			if (out.haveStartTime)
				out.startTime += age;
			return true;
		}
		if (static_cast<int32_t>(atMs - oldest.timeMs) <= 0) {
			out = LegsOf(oldest.body);
			return true;
		}
		for (size_t i = 0; i + 1 < m_samples.size(); ++i) {
			const Sample &a = m_samples[i];
			const Sample &b = m_samples[i + 1];
			if (static_cast<int32_t>(atMs - b.timeMs) >= 0)
				continue;
			const uint32_t span = b.timeMs - a.timeMs;
			const uint32_t into = atMs - a.timeMs;
			const float    t    = span == 0 ? 1.0f : static_cast<float>(into) / span;
			out = LerpLegs(LegsOf(a.body), LegsOf(b.body), t, into / 1000.0f);
			return true;
		}
		out = LegsOf(newest.body);
		return true;
	}

	static bool SameRide(const PlayerRideBody &a, const PlayerRideBody &b) {
		return a.kind != RIDE_NONE && a.kind == b.kind && a.track == b.track && a.id == b.id;
	}

private:
	struct Sample {
		uint32_t        timeMs;
		PlayerStateBody body;
		PlayerRideBody  ride;
	};

	static bool Take(const PlayerRideBody &ride, PlayerRideBody &out) {
		if (ride.kind == RIDE_NONE)
			return false;
		out = ride;
		return true;
	}

	std::deque<Sample> m_samples;
};

// ---- a stream that has stopped ---------------------------------------------

// Past this since the last snapshot landed, the copy is not moving any more.
// Measured on the arrival clock rather than the playback clock on purpose: the
// playback clock eases back toward the delayed target all through a stall and
// settles about 220 ms past the newest sample (interp.h), so it never gets to
// EXTRAPOLATE_MS and a test on it never fires.
constexpr uint32_t REMOTE_STALL_MS = InterpBuffer::DELAY_MS + EXTRAPOLATE_MS;

inline bool RemoteStreamStalled(uint32_t nowMs, uint32_t heardAtMs) {
	return static_cast<int32_t>(nowMs - heardAtMs) >= static_cast<int32_t>(REMOTE_STALL_MS);
}

// The locomotion ids CPed::SetMoveAnim plays (addresses.h, ANIM_STD_WALK..IDLE)
// plus the start-walk, which is the one other that loops a stride. Kept here
// as numbers so this header stays out of the game layer.
constexpr uint16_t BODY_ANIM_WALK      = 0;
constexpr uint16_t BODY_ANIM_RUN       = 1;
constexpr uint16_t BODY_ANIM_RUNFAST   = 2;
constexpr uint16_t BODY_ANIM_IDLE      = 3;
constexpr uint16_t BODY_ANIM_STARTWALK = 4;
constexpr uint8_t  BODY_MOVE_STILL     = 1;   // PEDMOVE_STILL

// A body standing where the stream left it, instead of running on the spot.
inline PlayerStateBody FrozenBody(PlayerStateBody body) {
	body.moveSpeed = {0.0f, 0.0f, 0.0f};
	body.moveState = BODY_MOVE_STILL;
	if (body.animId == BODY_ANIM_WALK || body.animId == BODY_ANIM_RUN ||
	    body.animId == BODY_ANIM_RUNFAST || body.animId == BODY_ANIM_STARTWALK) {
		body.animId    = BODY_ANIM_IDLE;
		body.animTime  = 0.0f;
		body.animSpeed = 1.0f;
		for (uint8_t &w : body.locoWeight)
			w = 0;
		body.locoWeight[LOCO_IDLE] = 255;
		body.locoPhase             = 0;
	}
	return body;
}

// ---- the health a replica is given --------------------------------------------

// Our engine kills a ped on its own at 1.0 or less: CPed::ProcessControl at
// 0x004C8DDA (`fld [ebx+2C0h] / fcomp [5F8440h]`, 1.0f, then state <= 22h, not
// in the air, not landing, and SetDie(0Dh, 4.0, 0.0) - a plain front knockdown).
// CPed::SetGetUp (0x004D0F51, below 1.0) and the entering-car arm at 0x004E0D56
// (0.0 or less) do the same. A replica copying its owner's health every frame
// hits the first of them the frame the zero lands, before the reliable death
// with the real animation has been applied, and KillRemotePed then finds a
// corpse and leaves it lying the wrong way. So it is kept above all three until
// the death is ours to apply. Nothing reads the ped's own copy for display.
constexpr float REPLICA_HEALTH_FLOOR = 2.0f;

inline float ReplicaHealth(float wire, bool deathApplied) {
	if (deathApplied)
		return wire;
	return wire > REPLICA_HEALTH_FLOOR ? wire : REPLICA_HEALTH_FLOOR;
}

// ---- where on the vehicle ----------------------------------------------------

// An entity's placement as the engine keeps it: position and the three rows
// of CMatrix. A point `o` in its frame is `pos + right*o.x + fwd*o.y + up*o.z`.
struct RideFrame {
	Vec3 pos{}, right{1, 0, 0}, fwd{0, 1, 0}, up{0, 0, 1};
};

// The heading the engine gives an entity with this forward row. A ped's
// m_fRotationCur is the same angle for its own matrix (SetHeading builds it
// with SetRotateZ, forward = (-sin h, cos h)), so the two subtract.
inline float RideFrameHeading(const RideFrame &f) {
	return std::atan2(-f.fwd.x, f.fwd.y);
}

inline Vec3 RideOffsetOf(const RideFrame &f, const Vec3 &world) {
	const Vec3 d{world.x - f.pos.x, world.y - f.pos.y, world.z - f.pos.z};
	return {d.x * f.right.x + d.y * f.right.y + d.z * f.right.z,
	        d.x * f.fwd.x + d.y * f.fwd.y + d.z * f.fwd.z,
	        d.x * f.up.x + d.y * f.up.y + d.z * f.up.z};
}

inline Vec3 RideWorldOf(const RideFrame &f, const Vec3 &o) {
	return {f.pos.x + f.right.x * o.x + f.fwd.x * o.y + f.up.x * o.z,
	        f.pos.y + f.right.y * o.x + f.fwd.y * o.y + f.up.y * o.z,
	        f.pos.z + f.right.z * o.x + f.fwd.z * o.y + f.up.z * o.z};
}

// Past this the owner is not standing on it any more, whatever the engine's
// pointer still says: a ped who jumped off keeps m_pCurrentPhysSurface until
// it lands on something else.
constexpr float RIDE_MAX_OFFSET_M = 30.0f;

inline bool RideOffsetPlausible(const Vec3 &o) {
	return std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z) &&
	       o.x * o.x + o.y * o.y + o.z * o.z <= RIDE_MAX_OFFSET_M * RIDE_MAX_OFFSET_M;
}

} // namespace coopiii
