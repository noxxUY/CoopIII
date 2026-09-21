// The network clock.
//
// Not the engine's, on purpose. CTimer::GetTimeInMilliseconds() stops while
// the game is paused and gets scaled by ms_fTimeScale, which missions set via
// SET_TIME_SCALE and which drops to 1/3 on player death for the slow-motion
// effect. A dying player would then emit timestamps at a third of everyone
// else's rate and desync their own interpolation. See docs/protocol.md §1.2.
//
// Nothing in the game can rescale this one. It's the only clock allowed to
// fill PacketHeader::sendTimeMs or drive the send cadence.
#pragma once

#include <chrono>
#include <cstdint>

namespace coopiii {

class WallClock {
public:
	// Called once when CoopIII starts. Later calls do nothing, so the epoch
	// stays fixed for the life of the process.
	static void Start() {
		if (!s_started) {
			s_epoch   = Now();
			s_started = true;
		}
	}

	// Milliseconds since Start(). Monotonic, wraps after ~49 days. The
	// interpolation buffer already handles that by rejecting out-of-order
	// samples instead of trusting arithmetic across the wrap.
	static uint32_t NowMs() {
		const auto delta = Now() - s_epoch;
		return static_cast<uint32_t>(
		    std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
	}

private:
	using Clock = std::chrono::steady_clock;
	static Clock::time_point Now() { return Clock::now(); }

	static inline Clock::time_point s_epoch{};
	static inline bool              s_started = false;
};

// Fires at a fixed rate off WallClock. Used for the 25 Hz snapshot cadence.
// Frame-counting won't work here: the target install runs at 60 FPS and
// 60/25 isn't an integer (docs/compat.md §2.4).
class RateLimiter {
public:
	explicit RateLimiter(uint32_t hz)
	    : m_intervalMs(hz ? 1000u / hz : 0u) {}

	// True at most once per interval. Advances by whole intervals instead of
	// resetting to now so the cadence doesn't drift with frame timing.
	bool Ready(uint32_t nowMs) {
		if (m_intervalMs == 0)
			return true;
		if (m_nextMs == 0) {
			m_nextMs = nowMs + m_intervalMs;
			return true;
		}
		if (static_cast<int32_t>(nowMs - m_nextMs) < 0)
			return false;
		m_nextMs += m_intervalMs;
		// A long stall (streaming hitch, alt-tab) shouldn't cause a burst of
		// catch-up sends afterward, so just skip to the next interval.
		if (static_cast<int32_t>(nowMs - m_nextMs) > 0)
			m_nextMs = nowMs + m_intervalMs;
		return true;
	}

	uint32_t IntervalMs() const { return m_intervalMs; }

private:
	uint32_t m_intervalMs;
	uint32_t m_nextMs = 0;
};

} // namespace coopiii
