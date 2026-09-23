// What the traffic lights show, as a function of the clock.
//
// GTA III's traffic lights have no state. CTrafficLights::LightForCars1,
// LightForCars2 and LightForPeds each read CTimer::m_snTimeInMilliseconds,
// mask it to a 16384 ms cycle and compare it against three fixed thresholds
// (addresses.h, "traffic lights"). Everything that obeys a light - a car
// deciding to stop, a pedestrian deciding to cross, the light drawing itself -
// calls one of the three. So two machines show the same lights exactly when
// the three return the same thing, and in a session game/lights.cpp makes them
// answer for the server's clock (sessiontime.h) instead of CTimer's. Nothing
// about a light is on the wire.
//
// Pure arithmetic, so clienttest runs all of it.
#pragma once

#include <cstdint>

namespace coopiii {

// Transcribed from the three functions, not from re3. Each one is
// `mov eax,[00885B48h] / and eax,3FFFh` followed by unsigned compares.
constexpr uint32_t LIGHT_PERIOD_MS = 0x4000;   // 16.384 s

// The engine's return values, the byte callers test in al.
enum : uint8_t {
	CAR_LIGHT_GREEN = 0,
	CAR_LIGHT_AMBER = 1,
	CAR_LIGHT_RED   = 2,
};
enum : uint8_t {
	PED_LIGHT_WALK       = 0,
	PED_LIGHT_WALK_BLINK = 1,
	PED_LIGHT_DONT_WALK  = 2,
};

// The thresholds, as the immediates the engine compares against.
constexpr uint32_t CARS1_AMBER_FROM_MS = 0x1388;   // 5000
constexpr uint32_t CARS1_RED_FROM_MS   = 0x1770;   // 6000
constexpr uint32_t CARS2_GREEN_FROM_MS = 0x1770;   // 6000
constexpr uint32_t CARS2_AMBER_FROM_MS = 0x2AF8;   // 11000
constexpr uint32_t CARS2_RED_FROM_MS   = 0x2EE0;   // 12000
constexpr uint32_t PEDS_WALK_FROM_MS   = 0x2EE0;   // 12000
constexpr uint32_t PEDS_BLINK_FROM_MS  = 0x3C18;   // 15384

constexpr uint32_t LightCycleMs(uint32_t clockMs) {
	return clockMs & (LIGHT_PERIOD_MS - 1);
}

// 0x00455760. One of the two directions at a junction: green, amber, then red
// for the rest of the cycle.
constexpr uint8_t CarLights1(uint32_t clockMs) {
	return LightCycleMs(clockMs) < CARS1_AMBER_FROM_MS ? CAR_LIGHT_GREEN
	       : LightCycleMs(clockMs) < CARS1_RED_FROM_MS ? CAR_LIGHT_AMBER
	                                                   : CAR_LIGHT_RED;
}

// 0x00455790. The crossing direction: red while the first has its green and
// amber, then its own green and amber, then red again.
constexpr uint8_t CarLights2(uint32_t clockMs) {
	return LightCycleMs(clockMs) < CARS2_GREEN_FROM_MS ? CAR_LIGHT_RED
	       : LightCycleMs(clockMs) < CARS2_AMBER_FROM_MS ? CAR_LIGHT_GREEN
	       : LightCycleMs(clockMs) < CARS2_RED_FROM_MS   ? CAR_LIGHT_AMBER
	                                                     : CAR_LIGHT_RED;
}

// 0x004557D0. Pedestrians walk while both car directions are red.
constexpr uint8_t PedLights(uint32_t clockMs) {
	return LightCycleMs(clockMs) < PEDS_WALK_FROM_MS    ? PED_LIGHT_DONT_WALK
	       : LightCycleMs(clockMs) < PEDS_BLINK_FROM_MS ? PED_LIGHT_WALK
	                                                    : PED_LIGHT_WALK_BLINK;
}

} // namespace coopiii
