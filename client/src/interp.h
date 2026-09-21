// Snapshot buffer + interpolation for remote entities (docs/protocol.md §2.6).
//
// No game or engine dependency here on purpose - it just takes snapshots in
// and gives a pose out. That's the part most likely to look wrong on screen,
// so keeping it testable without GTA III running matters more than usual.
#pragma once

#include "coopiii/protocol.h"

#include <cstdint>
#include <deque>

namespace coopiii {

struct Pose {
	Vec3  pos     = {};
	float heading = 0.0f;
};

// Shortest-path angle lerp. Ped headings are a scalar yaw in radians
// (CPed::m_fRotationCur, re3 src/peds/Ped.h:444) and wrap at ±pi, so a plain
// lerp spins the long way round whenever it crosses that seam.
float LerpAngle(float from, float to, float t);

// How far the playback clock below is allowed to drift from what the newest
// snapshot says before it gets snapped back rather than eased. A gap this
// big means a stall, a reconnect, or a respawn - not normal drift.
constexpr uint32_t CLOCK_RESYNC_MS = 500;

// Fraction of the remaining error corrected per frame otherwise. Kept small
// so the correction stays invisible; at 60 fps there are plenty of frames to
// spread it across.
constexpr float CLOCK_EASE = 0.05f;

// Where in a sender's timeline we're currently rendering.
//
// This class exists because of a bug that was easy to write and impossible
// to miss on screen. Both buffers used to render at `newestSnapshot -
// DELAY_MS`, which looks correct on paper and is wrong every frame: the
// newest snapshot's timestamp only changes when a snapshot *arrives*, so the
// render instant only advanced 25 times a second. Every frame in between
// sampled the same instant and produced the same pose. Remote players were
// perfectly interpolated and perfectly still two frames out of three, then
// jumped 40ms on the third - a 25 Hz stutter riding a 60 Hz screen, which
// reads as lag, not as a clock standing still.
//
// So now the clock is kept explicitly, moved forward by however much
// *local* time passed, and eased toward wherever the stream says it should
// be (the two machines' clocks run at slightly different rates, and the
// sender's is the one that matters). Snapping straight to the target instead
// of easing would just be the original bug wearing a different hat.
class PlaybackClock {
public:
	// `localNowMs` is any monotonic local millisecond count; it's never
	// compared against a sender's clock, only differenced against itself.
	// `targetMs` is where the newest snapshot says we should be, in the
	// sender's timebase. Returns the instant to render.
	uint32_t Advance(uint32_t localNowMs, uint32_t targetMs);

	// Forgets the timeline. Whatever arrives next starts a fresh one -
	// carrying the old one forward would spend the first half-second easing
	// toward a number that no longer means anything.
	void Stop() { m_running = false; }
	bool Running() const { return m_running; }

private:
	uint32_t m_renderTimeMs = 0;
	uint32_t m_lastLocalMs  = 0;
	bool     m_running      = false;
};

class InterpBuffer {
public:
	// How far behind the newest snapshot we render. One snapshot interval
	// (1000/25 = 40ms) plus margin for jitter.
	static constexpr uint32_t DELAY_MS = 100;
	// Past this, extrapolation is worse than just freezing in place.
	static constexpr uint32_t MAX_EXTRAPOLATE_MS = 250;
	// Past this gap, interpolating would drag the entity across the map -
	// snap instead (respawn, teleport, or a long stall).
	static constexpr float SNAP_DISTANCE = 5.0f;

	static constexpr size_t MAX_SAMPLES = 32;

	void Push(uint32_t sendTimeMs, const Vec3 &pos, float heading, const Vec3 &velocity);
	void Clear() { m_samples.clear(); }
	bool Empty() const { return m_samples.empty(); }
	size_t Size() const { return m_samples.size(); }

	// `renderTimeMs` is the *sender's* clock, so callers should track the
	// remote's newest sendTimeMs rather than reach for a local one (the two
	// clocks aren't shared).
	bool Sample(uint32_t renderTimeMs, Pose &out) const;

	// Renders DELAY_MS behind the newest snapshot, on a clock that advances
	// with the local frame rather than with arrivals.
	//
	// This used to just be `Sample(newestReceived - DELAY_MS)`, which is
	// wrong in a way that's easy to miss on paper and impossible to miss on
	// screen: the newest snapshot's timestamp only changes when a snapshot
	// *arrives*, so the render time only moved 25 times a second. Every frame
	// in between sampled the same instant and produced the same pose. Remote
	// players ended up perfectly interpolated and perfectly still for two
	// frames out of three, then jumped 40ms on the third - reads as lag, but
	// it's really a clock that isn't running.
	//
	// So the playback clock lives here now, in the sender's timebase, and
	// gets moved forward by however much local time passed. `localNowMs` is
	// any monotonic local millisecond count (WallClock::NowMs); it's never
	// compared against a sender's clock, only differenced against itself.
	bool SampleDelayed(uint32_t localNowMs, Pose &out);

	uint32_t NewestTimeMs() const {
		return m_samples.empty() ? 0 : m_samples.back().timeMs;
	}

private:
	struct Snapshot {
		uint32_t timeMs;
		Vec3     pos;
		float    heading;
		Vec3     velocity;
	};

	std::deque<Snapshot> m_samples;
	PlaybackClock        m_clock;
};

// ---- vehicles --------------------------------------------------------------

struct VehicleTransform {
	Vec3 pos = {};
	Quat rot = {0.0f, 0.0f, 0.0f, 1.0f};
};

// Shortest-path spherical interpolation between two orientations.
//
// A car's rotation can't be lerped component-wise the way a ped's heading
// can. A quaternion and its negation represent the same orientation, so a
// plain lerp takes the long way round half the time, and a car changing
// lanes would barrel-roll. This negates one end when the two point away from
// each other, and falls back to a normalised lerp once the angle gets small
// enough that the sines underflow.
Quat Slerp(const Quat &a, const Quat &b, float t);

// Same buffer as above, but carrying a full orientation.
//
// Kept as its own type rather than templated over the rotation, because the
// two differ in more than just the rotation type. Vehicles move a lot faster
// than peds, so the distance that means "this is a teleport, don't slide
// through it" is a different number here - get it wrong and it reads as
// either rubber-banding or a car that ignores a respawn.
class VehicleInterpBuffer {
public:
	static constexpr uint32_t DELAY_MS           = 100;
	static constexpr uint32_t MAX_EXTRAPOLATE_MS = 250;

	// 20m, not the ped's 5. A car at 100 km/h covers about 1.1m between
	// snapshots, more downhill, so 5 would read ordinary driving as a
	// teleport and snap constantly.
	static constexpr float  SNAP_DISTANCE = 20.0f;
	static constexpr size_t MAX_SAMPLES   = 32;

	void Push(uint32_t sendTimeMs, const Vec3 &pos, const Quat &rot,
	          const Vec3 &velocity);
	void Clear() { m_samples.clear(); }
	bool Empty() const { return m_samples.empty(); }
	size_t Size() const { return m_samples.size(); }

	bool Sample(uint32_t renderTimeMs, VehicleTransform &out) const;

	// Same fix, same clock-that-moves-with-the-frame trick as the ped
	// buffer, and the same reason: a car corrected every frame from a clock
	// that only moves on arrival is a car that moves in 25 Hz steps while
	// its wheels are turning at 60.
	bool SampleDelayed(uint32_t localNowMs, VehicleTransform &out);

	uint32_t NewestTimeMs() const {
		return m_samples.empty() ? 0 : m_samples.back().timeMs;
	}

private:
	struct Snapshot {
		uint32_t timeMs;
		Vec3     pos;
		Quat     rot;
		Vec3     velocity;
	};

	std::deque<Snapshot> m_samples;
	PlaybackClock        m_clock;
};

} // namespace coopiii
