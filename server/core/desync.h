// What an owner said about where its player or car was, for the last second or
// so, and how far an observer's copy is from it. protocol.h, C_DesyncProbe.
#pragma once

#include "coopiii/protocol.h"

#include <cmath>
#include <cstdint>

namespace coopiii {

// A copy this far from the owner, or further, gets a line in the server's log.
constexpr float DESYNC_LOG_M = 5.0f;
// And a probe further off than this from the newest sample is on some other
// timeline: a car whose reporter changed and whose observer is still playing
// the old one out. A stall does not get this far; the client's clock settles
// a couple of hundred milliseconds past the newest sample and stays there.
constexpr uint32_t DESYNC_WINDOW_MS = 1000;
// At most one of those lines per prober this often.
constexpr uint32_t DESYNC_LOG_EVERY_MS = 10000;

class PoseHistory {
public:
	static constexpr uint8_t SIZE = 32;   // 1.3 s at 25 Hz

	// PED_SNAP_M for a player, CAR_SNAP_M for a car: whichever the client's
	// buffer for it jumps at. And whether that buffer runs on along the last
	// velocity past its newest sample, as a player's and a session car's do
	// for up to EXTRAPOLATE_MS, or holds there, as traffic and pedestrians do.
	PoseHistory() = default;
	PoseHistory(float snapM, bool extrapolates) : m_snapM(snapM), m_extrapolates(extrapolates) {}

	// Samples from one reporter at a time. Their clocks are not each
	// other's, so a new reporter starts a new history rather than being
	// mixed into the old one. Older than the newest is dropped, as the
	// client's buffers drop it.
	// `velocity` in m/s, the unit the client's buffers take it in.
	void Note(uint32_t atMs, const Vec3 &pos, uint8_t reporter, const Vec3 &velocity = {}) {
		if (reporter != m_reporter) {
			m_count    = 0;
			m_reporter = reporter;
		}
		if (m_count > 0 && static_cast<int32_t>(atMs - Newest().atMs) <= 0)
			return;
		m_samples[(m_first + m_count) % SIZE] = Sample{atMs, pos, velocity};
		if (m_count < SIZE)
			++m_count;
		else
			m_first = static_cast<uint8_t>((m_first + 1) % SIZE);
	}

	void Clear() {
		m_count    = 0;
		m_reporter = INVALID_PLAYER;
	}

	uint8_t Reporter() const { return m_reporter; }
	uint8_t Count() const { return m_count; }

	// Where it was at `atMs`, between the two samples either side of it. Past
	// the newest it is where the client's buffer would have it too: run on
	// along the newest velocity for up to EXTRAPOLATE_MS, or held. Before the
	// oldest there is no telling.
	bool At(uint32_t atMs, Vec3 &out) const {
		if (m_count == 0)
			return false;
		const Sample &newest = Newest();
		const int32_t ahead  = static_cast<int32_t>(atMs - newest.atMs);
		if (ahead > static_cast<int32_t>(DESYNC_WINDOW_MS) ||
		    ahead < -static_cast<int32_t>(DESYNC_WINDOW_MS))
			return false;
		if (ahead >= 0) {
			out = newest.pos;
			if (m_extrapolates) {
				const float dt =
				    static_cast<float>(ahead < static_cast<int32_t>(EXTRAPOLATE_MS)
				                           ? ahead
				                           : static_cast<int32_t>(EXTRAPOLATE_MS)) /
				    1000.0f;
				out.x += newest.velocity.x * dt;
				out.y += newest.velocity.y * dt;
				out.z += newest.velocity.z * dt;
			}
			return true;
		}
		for (uint8_t i = m_count - 1; i > 0; --i) {
			const Sample &b = Nth(i);
			const Sample &a = Nth(static_cast<uint8_t>(i - 1));
			if (static_cast<int32_t>(atMs - a.atMs) < 0)
				continue;
			const float gx = b.pos.x - a.pos.x, gy = b.pos.y - a.pos.y, gz = b.pos.z - a.pos.z;
			if (gx * gx + gy * gy + gz * gz > m_snapM * m_snapM) {
				out = b.pos;
				return true;
			}
			const float span = static_cast<float>(static_cast<int32_t>(b.atMs - a.atMs));
			const float t =
			    span > 0.0f ? static_cast<float>(static_cast<int32_t>(atMs - a.atMs)) / span : 0.0f;
			out = Vec3{a.pos.x + (b.pos.x - a.pos.x) * t, a.pos.y + (b.pos.y - a.pos.y) * t,
			           a.pos.z + (b.pos.z - a.pos.z) * t};
			return true;
		}
		return false;
	}

private:
	struct Sample {
		uint32_t atMs;
		Vec3     pos;
		Vec3     velocity;
	};

	const Sample &Nth(uint8_t i) const { return m_samples[(m_first + i) % SIZE]; }
	const Sample &Newest() const { return Nth(static_cast<uint8_t>(m_count - 1)); }

	Sample  m_samples[SIZE] = {};
	float   m_snapM        = PED_SNAP_M;
	bool    m_extrapolates = true;
	uint8_t m_first    = 0;
	uint8_t m_count    = 0;
	uint8_t m_reporter = INVALID_PLAYER;
};

// The engine's per-step move speed on the wire, in the m/s a history keeps.
inline Vec3 MoveSpeedMps(const Vec3 &moveSpeed) {
	return Vec3{moveSpeed.x * ENGINE_STEPS_PER_SECOND, moveSpeed.y * ENGINE_STEPS_PER_SECOND,
	            moveSpeed.z * ENGINE_STEPS_PER_SECOND};
}

// The answer for one row: centimetres between the copy and the owner at the
// copy's instant, or DESYNC_UNKNOWN.
inline uint16_t DesyncOffCm(const PoseHistory &history, const DesyncProbeRow &row) {
	Vec3 truth{};
	if (!history.At(row.atMs, truth))
		return DESYNC_UNKNOWN;
	const float dx = row.pos.x - truth.x, dy = row.pos.y - truth.y, dz = row.pos.z - truth.z;
	const float cm = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.0f;
	if (std::isnan(cm))
		return DESYNC_UNKNOWN;
	if (cm >= static_cast<float>(DESYNC_MAX_CM))
		return DESYNC_MAX_CM;
	return static_cast<uint16_t>(cm + 0.5f);
}

} // namespace coopiii
