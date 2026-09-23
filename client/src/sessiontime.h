// The server's clock, as estimated from here.
//
// Some of GTA III's world is placed by the clock and nothing else: every
// frame, the engine works out where each train (traintime.h) and each
// background plane (planetime.h) is, what every traffic light shows
// (lighttime.h) and how far the lift bridge is raised (liftbridgetime.h)
// from CTimer::m_snTimeInMilliseconds, and keeps nothing from the frame
// before that would change them - bar the bridge's starting time, which
// liftbridgetime.h deals with. So two machines show the same train, plane,
// light or bridge exactly when they feed the engine the same number. CTimer's
// own number can't be that: a save carries it and loading one puts it back
// (`mov edi,885B48h / movs` at 0x00590C0B), so it starts wherever each
// player's save left it, then stops on a loading screen and is scaled by
// ms_fTimeScale, and about 700 places in the engine store deadlines against
// it, so moving it would fire or freeze every one of them.
//
// What every machine can agree on is the server's clock. The server stamps
// S_Welcome and S_WorldState with its own NowMs() rather than relaying
// anyone else's, and S_WorldState goes out at least once a second. This
// estimates that clock from those headers. game/sessionclock.cpp holds the
// result on the engine side. game/trains.cpp and game/planes.cpp each put it
// in CTimer's place for the length of the one engine call that needs it;
// game/lights.cpp answers for the lights from it without touching CTimer, and
// game/liftbridge.cpp moves the bridge's epoch instead.
//
// Pure arithmetic, no engine and no network, so clienttest runs all of it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coopiii {

// session = local + offset, all modulo 2^32. The local clock is WallClock.
class SessionTimeBase {
public:
	// How many samples the estimate is taken over. S_WorldState is 1 Hz, so
	// this is the last sixteen seconds or so: long enough that one of them
	// crossed the network without queueing, short enough that two steady
	// clocks can't drift apart inside it by anything worth measuring.
	static constexpr size_t WINDOW = 16;

	// The first few samples are taken as they come, each replacing the last.
	// ENet's round trip starts life as a 500 ms placeholder and only settles
	// once some reliable traffic has been acknowledged, so the first estimate
	// can be a quarter of a second out. Snapping while that settles costs a
	// couple of jumps in the first seconds of a session; slewing through it
	// would take five seconds of trains and planes running fast.
	static constexpr uint32_t WARMUP_SAMPLES = 3;

	// Past this the two clocks are not drifting, they are different clocks,
	// and the only sane thing is to jump.
	static constexpr int32_t SNAP_MS = 1000;

	// Otherwise the correction is spread out: at most one millisecond of it
	// per this many elapsed. 5%, which nobody can see on a train doing
	// 30 m/s, and it can never run the session clock backwards.
	static constexpr uint32_t SLEW_DIVISOR = 20;

	void Reset() { *this = SessionTimeBase{}; }

	bool     Valid() const { return m_valid; }
	uint32_t OffsetMs() const { return m_applied; }
	uint32_t TargetOffsetMs() const { return m_target; }
	uint32_t Now(uint32_t localMs) const { return localMs + m_applied; }

	// `serverMs` is the header of a packet the server stamped with its own
	// clock, `localMs` our WallClock when we read it, `rttMs` ENet's round
	// trip at that moment.
	void AddSample(uint32_t serverMs, uint32_t localMs, uint32_t rttMs) {
		Sample &s = m_samples[m_next];
		s.raw     = serverMs - localMs;
		s.rttMs   = rttMs;
		m_next    = (m_next + 1) % WINDOW;
		if (m_count < WINDOW)
			++m_count;
		++m_seen;

		// raw is the true offset minus however long this sample took to get
		// here, so the largest raw is the one that was held up least. Signed
		// comparison of the difference, so a wrap of either clock can't pick
		// the wrong one.
		uint32_t best   = m_samples[0].raw;
		uint32_t minRtt = m_samples[0].rttMs;
		for (size_t i = 1; i < m_count; ++i) {
			if (static_cast<int32_t>(m_samples[i].raw - best) > 0)
				best = m_samples[i].raw;
			if (m_samples[i].rttMs < minRtt)
				minRtt = m_samples[i].rttMs;
		}
		// Half the best round trip in the window stands in for the one-way
		// trip that best sample made.
		m_target = best + minRtt / 2;

		const int32_t error = static_cast<int32_t>(m_target - m_applied);
		if (!m_valid || m_seen <= WARMUP_SAMPLES || error > SNAP_MS || error < -SNAP_MS) {
			m_applied = m_target;
			m_budget  = 0;
			m_valid   = true;
			++m_snaps;
		}
		m_lastTickMs = localMs;
	}

	// Called once a frame. Walks the applied offset toward the target.
	void Tick(uint32_t localMs) {
		const uint32_t elapsed = localMs - m_lastTickMs;
		m_lastTickMs           = localMs;
		if (!m_valid)
			return;

		const int32_t error = static_cast<int32_t>(m_target - m_applied);
		if (error == 0) {
			m_budget = 0;
			return;
		}
		// A frame that took longer than a second is a hitch, not time the
		// correction should get to spend all at once.
		m_budget += elapsed > 1000 ? 1000 : elapsed;
		const uint32_t allowed = m_budget / SLEW_DIVISOR;
		if (allowed == 0)
			return;
		m_budget -= allowed * SLEW_DIVISOR;

		const uint32_t magnitude =
		    error > 0 ? static_cast<uint32_t>(error) : static_cast<uint32_t>(-error);
		const uint32_t step = magnitude < allowed ? magnitude : allowed;
		m_applied = error > 0 ? m_applied + step : m_applied - step;
	}

	// How many times the offset jumped rather than slid. For the log.
	uint32_t Snaps() const { return m_snaps; }

private:
	struct Sample {
		uint32_t raw   = 0;   // serverMs - localMs
		uint32_t rttMs = 0;
	};

	Sample   m_samples[WINDOW]{};
	size_t   m_next       = 0;
	size_t   m_count      = 0;
	uint32_t m_seen       = 0;
	uint32_t m_target     = 0;
	uint32_t m_applied    = 0;
	uint32_t m_budget     = 0;
	uint32_t m_lastTickMs = 0;
	uint32_t m_snaps      = 0;
	bool     m_valid      = false;
};

} // namespace coopiii
