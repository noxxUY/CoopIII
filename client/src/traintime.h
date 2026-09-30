// Where the El and the subway are, as a function of the clock.
//
// A GTA III train has no state worth sending. CTrain::UpdateTrains
// (0x0054F3A0) works out where each train is on its track from
// CTimer::m_snTimeInMilliseconds and nothing else, every frame, and
// CTrain::ProcessControl only reads the result (addresses.h, "trains"). So
// in a session the trains run on the server's clock (sessiontime.h), which
// game/trains.cpp hands to UpdateTrains in place of CTimer's for the length
// of that one call, and nothing about a train is on the wire.
//
// Pure arithmetic, so clienttest runs all of it.
#pragma once

#include <cstdint>

namespace coopiii {

// Transcribed from UpdateTrains, not from re3. The El loop takes
// `(t + i * 0x10000) & 0x1FFFF` for its two trains, the subway
// `(t + i * 0x10000) & 0x3FFFF` for its four, so both lines are periodic in
// 0x40000 ms and two clocks that agree modulo that agree about every train.
constexpr uint32_t EL_PERIOD_MS      = 0x20000;   // 131.072 s, 2 trains
constexpr uint32_t SUBWAY_PERIOD_MS  = 0x40000;   // 262.144 s, 4 trains
constexpr uint32_t TRAIN_STAGGER_MS  = 0x10000;   // added per train, both lines

// Where train `index` is in its line's cycle for a given clock, as the
// engine computes it before scaling by the track's duration.
constexpr uint32_t TrainCycleMs(uint32_t clockMs, uint32_t periodMs, uint32_t index) {
	return (clockMs + index * TRAIN_STAGGER_MS) & (periodMs - 1);
}

} // namespace coopiii
