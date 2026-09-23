// Where the background planes are, as a function of the clock.
//
// CPlane::UpdatePlanes (0x0054BEC0) places the three airliners that taxi, take
// off and land on flight.dat and the three Dodos that circle on flight2.dat from
// CTimer::m_snTimeInMilliseconds, every frame, and CPlane::ProcessControl
// turns that into a matrix without reading anything the last frame left
// behind that would move a plane (addresses.h, "planes"). So in a session the
// planes run on the server's clock (sessiontime.h), exactly like the trains,
// and nothing about a plane is on the wire.
//
// The one thing that is not a function of the clock is the two mission
// Cessnas, which fly from a start time the script stamped with CTimer's own
// value. MissionStartOnSessionClock is what keeps them on their schedule.
//
// Pure arithmetic, so clienttest runs all of it.
#pragma once

#include <cstdint>

namespace coopiii {

// Transcribed from UpdatePlanes, not from re3. Both the airliners and the
// Dodos take `(t + i * 0x2AAAA) & 0x7FFFF` for plane i of three, so the whole
// sky repeats every 0x80000 ms and two clocks that agree modulo that agree
// about every plane. 0x2AAAA is 0x80000 / 3 rounded down, as the engine has
// it: `add ecx,2AAAAh` in the airliner loop, `lea ecx,[edx+2AAAAh]` and
// `lea ecx,[edx+55554h]` for the Dodos.
constexpr uint32_t PLANE_PERIOD_MS  = 0x80000;   // 524.288 s
constexpr uint32_t PLANE_STAGGER_MS = 0x2AAAA;   // added per plane
constexpr uint32_t PLANES_PER_PATH  = 3;

// Where plane `index` is in the loop for a given clock, as the engine
// computes it before scaling by the flight's duration.
constexpr uint32_t PlaneCycleMs(uint32_t clockMs, uint32_t index) {
	return (clockMs + index * PLANE_STAGGER_MS) & (PLANE_PERIOD_MS - 1);
}

// How long each mission Cessna flies before UpdatePlanes lands it:
// `cmp ebp,1F448h` and `cmp edx,7F448h`, both on `t - start`.
constexpr uint32_t DRUG_RUN_CESNA_FLIGHT_MS = 128072;
constexpr uint32_t DROP_OFF_CESNA_FLIGHT_MS = 521288;

// UpdatePlanes flies a mission Cessna from `t - start`, where `start` was
// CTimer's value when the script created it. Handed the session's clock
// instead of CTimer's, it would see an elapsed time that is off by however far
// apart the two clocks are, and land the plane early or keep it in the air.
// So for the length of the call the start is moved by the same amount the
// clock was, which leaves `t - start` exactly what single player would see.
// All modulo 2^32, like the engine's own subtraction.
constexpr uint32_t MissionStartOnSessionClock(uint32_t startMs, uint32_t oursMs,
                                              uint32_t sessionMs) {
	return startMs + (sessionMs - oursMs);
}

// And back, after the call. Exact, whatever the two clocks are.
constexpr uint32_t MissionStartOnOurClock(uint32_t shiftedMs, uint32_t oursMs,
                                          uint32_t sessionMs) {
	return shiftedMs - (sessionMs - oursMs);
}

} // namespace coopiii
