#include "interp.h"

#include <cmath>

namespace coopiii {

namespace {

constexpr float PI     = 3.14159265358979323846f;
constexpr float TWO_PI = 2.0f * PI;

Vec3 Lerp(const Vec3 &a, const Vec3 &b, float t) {
	return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

float DistanceSq(const Vec3 &a, const Vec3 &b) {
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return dx * dx + dy * dy + dz * dz;
}

} // namespace

float LerpAngle(float from, float to, float t) {
	float delta = std::fmod(to - from, TWO_PI);
	if (delta > PI)
		delta -= TWO_PI;
	else if (delta < -PI)
		delta += TWO_PI;
	return from + delta * t;
}

void InterpBuffer::Push(uint32_t sendTimeMs, const Vec3 &pos, float heading,
                        const Vec3 &velocity) {
	// Out-of-order arrivals get dropped. The snapshot channel is unsequenced,
	// so a stale packet can land after a newer one, and applying it would
	// rewind the entity.
	if (!m_samples.empty() && sendTimeMs <= m_samples.back().timeMs)
		return;

	m_samples.push_back({sendTimeMs, pos, heading, velocity});
	while (m_samples.size() > MAX_SAMPLES)
		m_samples.pop_front();
}

bool InterpBuffer::Sample(uint32_t renderTimeMs, Pose &out) const {
	if (m_samples.empty())
		return false;

	const Snapshot &newest = m_samples.back();

	if (m_samples.size() == 1 || renderTimeMs >= newest.timeMs) {
		// Ahead of everything we have. Extrapolate along the last known
		// velocity for a bounded window, then just hold position instead of
		// letting it drift off forever.
		const uint32_t ahead = renderTimeMs - newest.timeMs;
		const float    dt    = std::min(ahead, MAX_EXTRAPOLATE_MS) / 1000.0f;
		out.pos     = {newest.pos.x + newest.velocity.x * dt,
		               newest.pos.y + newest.velocity.y * dt,
		               newest.pos.z + newest.velocity.z * dt};
		out.heading = newest.heading;
		return true;
	}

	const Snapshot &oldest = m_samples.front();
	if (renderTimeMs <= oldest.timeMs) {
		out.pos     = oldest.pos;
		out.heading = oldest.heading;
		return true;
	}

	for (size_t i = 0; i + 1 < m_samples.size(); ++i) {
		const Snapshot &a = m_samples[i];
		const Snapshot &b = m_samples[i + 1];
		if (renderTimeMs < a.timeMs || renderTimeMs > b.timeMs)
			continue;

		// A big jump between neighbours means a teleport or a desync
		// recovery, not motion. Sliding through it would look absurd.
		if (DistanceSq(a.pos, b.pos) > SNAP_DISTANCE * SNAP_DISTANCE) {
			out.pos     = b.pos;
			out.heading = b.heading;
			return true;
		}

		const uint32_t span = b.timeMs - a.timeMs;
		const float    t    = span == 0 ? 1.0f
		                                : static_cast<float>(renderTimeMs - a.timeMs) / span;
		out.pos     = Lerp(a.pos, b.pos, t);
		out.heading = LerpAngle(a.heading, b.heading, t);
		return true;
	}

	out.pos     = newest.pos;
	out.heading = newest.heading;
	return true;
}

// ---- vehicles --------------------------------------------------------------

Quat Slerp(const Quat &a, const Quat &b, float t) {
	float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;

	// q and -q are the same orientation. Skip this and the interpolation
	// takes the long way round whenever the two happen to be signed
	// differently - on a car that reads as a barrel roll mid lane-change.
	Quat end = b;
	if (dot < 0.0f) {
		end = Quat{-b.x, -b.y, -b.z, -b.w};
		dot = -dot;
	}

	float wa, wb;
	if (dot > 0.9995f) {
		// Nearly the same orientation, so sin(theta) underflows and the
		// division below loses all its precision. A straight lerp looks the
		// same here anyway, and it can't divide by zero.
		wa = 1.0f - t;
		wb = t;
	} else {
		const float theta    = std::acos(dot < -1.0f ? -1.0f : (dot > 1.0f ? 1.0f : dot));
		const float sinTheta = std::sin(theta);
		wa = std::sin((1.0f - t) * theta) / sinTheta;
		wb = std::sin(t * theta) / sinTheta;
	}

	Quat out{a.x * wa + end.x * wb, a.y * wa + end.y * wb, a.z * wa + end.z * wb,
	         a.w * wa + end.w * wb};

	// Renormalise. The lerp branch above doesn't preserve length, and a
	// non-unit quaternion turns into a scaled car once it reaches the engine.
	const float len2 = out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w;
	if (len2 > 1e-12f) {
		const float inv = 1.0f / std::sqrt(len2);
		out.x *= inv;
		out.y *= inv;
		out.z *= inv;
		out.w *= inv;
	} else {
		out = Quat{0.0f, 0.0f, 0.0f, 1.0f};
	}
	return out;
}

void VehicleInterpBuffer::Push(uint32_t sendTimeMs, const Vec3 &pos, const Quat &rot,
                               const Vec3 &velocity) {
	if (!m_samples.empty() && sendTimeMs <= m_samples.back().timeMs)
		return;
	m_samples.push_back({sendTimeMs, pos, rot, velocity});
	while (m_samples.size() > MAX_SAMPLES)
		m_samples.pop_front();
}

bool VehicleInterpBuffer::Sample(uint32_t renderTimeMs, VehicleTransform &out) const {
	if (m_samples.empty())
		return false;

	const Snapshot &newest = m_samples.back();

	if (m_samples.size() == 1 || renderTimeMs >= newest.timeMs) {
		const uint32_t ahead = renderTimeMs - newest.timeMs;
		const float    dt    = std::min(ahead, MAX_EXTRAPOLATE_MS) / 1000.0f;
		out.pos = {newest.pos.x + newest.velocity.x * dt,
		           newest.pos.y + newest.velocity.y * dt,
		           newest.pos.z + newest.velocity.z * dt};
		// Orientation is held, not extrapolated. Spinning a car forward on
		// its last angular velocity looks far worse than a briefly stale
		// heading - the error just compounds.
		out.rot = newest.rot;
		return true;
	}

	const Snapshot &oldest = m_samples.front();
	if (renderTimeMs <= oldest.timeMs) {
		out.pos = oldest.pos;
		out.rot = oldest.rot;
		return true;
	}

	for (size_t i = 0; i + 1 < m_samples.size(); ++i) {
		const Snapshot &a = m_samples[i];
		const Snapshot &b = m_samples[i + 1];
		if (renderTimeMs < a.timeMs || renderTimeMs > b.timeMs)
			continue;

		if (DistanceSq(a.pos, b.pos) > SNAP_DISTANCE * SNAP_DISTANCE) {
			out.pos = b.pos;
			out.rot = b.rot;
			return true;
		}

		const uint32_t span = b.timeMs - a.timeMs;
		const float    t    = span == 0 ? 1.0f
		                                : static_cast<float>(renderTimeMs - a.timeMs) / span;
		out.pos = Lerp(a.pos, b.pos, t);
		out.rot = Slerp(a.rot, b.rot, t);
		return true;
	}

	out.pos = newest.pos;
	out.rot = newest.rot;
	return true;
}

bool VehicleInterpBuffer::SampleDelayed(uint32_t localNowMs,
                                        VehicleTransform &out) {
	if (m_samples.empty()) {
		m_clock.Stop();
		return false;
	}
	const uint32_t newest = m_samples.back().timeMs;
	const uint32_t target = newest > DELAY_MS ? newest - DELAY_MS : 0;
	return Sample(m_clock.Advance(localNowMs, target), out);
}

uint32_t PlaybackClock::Advance(uint32_t localNowMs, uint32_t targetMs) {
	if (!m_running) {
		m_renderTimeMs = targetMs;
		m_running      = true;
		m_lastLocalMs  = localNowMs;
		return m_renderTimeMs;
	}

	// Move with the frame, not the packet. This one line is the whole fix -
	// everything below just keeps the two clocks from drifting apart.
	m_renderTimeMs += localNowMs - m_lastLocalMs;
	m_lastLocalMs = localNowMs;

	// Signed on purpose - the error goes both ways, and unsigned arithmetic
	// here is how you get a one-frame jump to four billion.
	const int32_t error =
	    static_cast<int32_t>(targetMs) - static_cast<int32_t>(m_renderTimeMs);

	if (error > static_cast<int32_t>(CLOCK_RESYNC_MS) ||
	    error < -static_cast<int32_t>(CLOCK_RESYNC_MS)) {
		m_renderTimeMs = targetMs;
	} else {
		m_renderTimeMs = static_cast<uint32_t>(
		    static_cast<int32_t>(m_renderTimeMs) +
		    static_cast<int32_t>(static_cast<float>(error) * CLOCK_EASE));
	}
	return m_renderTimeMs;
}

bool InterpBuffer::SampleDelayed(uint32_t localNowMs, Pose &out) {
	if (m_samples.empty()) {
		m_clock.Stop();
		return false;
	}

	const uint32_t newest = m_samples.back().timeMs;
	// Early in a session the buffer is younger than the delay itself, so
	// clamp here instead of underflowing the unsigned subtraction.
	const uint32_t target = newest > DELAY_MS ? newest - DELAY_MS : 0;

	return Sample(m_clock.Advance(localNowMs, target), out);
}

} // namespace coopiii
