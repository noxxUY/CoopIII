// A session car's own state that its snapshot has no room for: the alarm and
// where its gun points.
//
// Both are things only one machine's engine ever changes. An alarm goes off
// on the machine whose car generator parked the car armed, and nowhere else:
// every other copy is one CoopIII built, and the constructor leaves it
// disarmed (protocol.h, S_VehicleAlarm). The tank's turret and the fire
// truck's water cannon turn only for the car the local player is driving
// (protocol.h, S_VehicleAim). So the machine simulating the car says, and
// every other copy is written.
//
// Pure arithmetic, so clienttest runs all of it; game/carextras.cpp is the
// engine side.
#pragma once

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii {

// ---- the alarm --------------------------------------------------------------

// How far the engine's own countdown and the wall clock may drift apart
// before the machine simulating the car says it again. The engine takes
// CTimer::ms_fTimeStep's worth off every frame (0x005525A0), which is game
// time, and a copy elsewhere is timed on WallClock.
constexpr uint32_t ALARM_SLACK_MS = 2000;

// What is left of an alarm that ends at `untilMs` (WallClock, 0 for none),
// never more than the engine ever sets.
constexpr uint16_t AlarmLeftMs(uint32_t untilMs, uint32_t nowMs) {
	if (untilMs == 0)
		return 0;
	const int32_t left = static_cast<int32_t>(untilMs - nowMs);
	if (left <= 0)
		return 0;
	return left > static_cast<int32_t>(VEHICLE_ALARM_MS) ? VEHICLE_ALARM_MS
	                                                     : static_cast<uint16_t>(left);
}

// When an alarm with `remainingMs` left ends. Never 0, which is "none".
constexpr uint32_t AlarmUntilMs(uint16_t remainingMs, uint32_t nowMs) {
	if (remainingMs == 0)
		return 0;
	const uint16_t r = remainingMs > VEHICLE_ALARM_MS ? VEHICLE_ALARM_MS : remainingMs;
	const uint32_t until = nowMs + r;
	return until != 0 ? until : 1u;
}

enum class AlarmNews : uint8_t {
	None,
	Started,   // send what the engine has left
	Stopped,   // send 0
};

// For a car this machine simulates: what its engine's alarm says, against
// what the session has been told.
//
//   engineMs   the copy's m_nAlarmState as milliseconds left, 0 for none and
//              for an armed car that has not gone off
//   untilMs    the row's record of the alarm already told, 0 for none. It is
//              kept past its end until the engine's own alarm reads 0, so a
//              countdown that runs a little behind the wall clock is not a
//              second alarm.
//
// An alarm the session heard from somebody else and wrote onto this copy is
// counting down here in step with the record, so taking the wheel of a car
// whose alarm is going says nothing.
constexpr AlarmNews AlarmNewsFor(uint16_t engineMs, uint32_t untilMs, uint32_t nowMs) {
	const uint16_t told = AlarmLeftMs(untilMs, nowMs);
	if (engineMs > 0) {
		if (untilMs == 0)
			return AlarmNews::Started;
		return static_cast<uint32_t>(engineMs) > told + ALARM_SLACK_MS ? AlarmNews::Started
		                                                                 : AlarmNews::None;
	}
	return told > ALARM_SLACK_MS ? AlarmNews::Stopped : AlarmNews::None;
}

// ---- where a car's gun points -----------------------------------------------

// The two models whose control code reads the pad into m_fCarGunLR/UD:
// CAutomobile::ProcessControl sends 0x61 to FireTruckControl (`cmp eax,61h`
// at 0x00531FEE) and 0x7A to TankControl (`cmp eax,7Ah` at 0x00532001), and
// CAutomobile::Render turns the turret for 0x7A alone (0x00539ED0).
constexpr uint16_t CAR_GUN_MODEL_FIRETRUCK = 0x61;   // 97
constexpr uint16_t CAR_GUN_MODEL_RHINO     = 0x7A;   // 122

constexpr bool CarHasGun(uint16_t model) {
	return model == CAR_GUN_MODEL_FIRETRUCK || model == CAR_GUN_MODEL_RHINO;
}

// Under this a turn is not worth a packet: a tenth of a degree. The turret
// turns at most 0.00015 rad per unit of stick per step (0x00600810), which is
// 0.02 a frame at full lock.
constexpr float    AIM_EPSILON    = 0.002f;
// And a turret that has not moved is said again this often, so a lost packet
// is not the last word on it.
constexpr uint32_t AIM_REFRESH_MS = 1000;

constexpr bool AimFinite(float lr, float ud) {
	return lr == lr && ud == ud && lr < 1e6f && lr > -1e6f && ud < 1e6f && ud > -1e6f;
}

constexpr float AimAbs(float x) { return x < 0.0f ? -x : x; }

// Whether the driver's machine sends the aim now.
//
//   sent          it has sent one for this car since it took the wheel
//   sentLR/UD     what it sent last
//   sentAtMs      when, on WallClock
constexpr bool AimShouldSend(bool sent, float sentLR, float sentUD, uint32_t sentAtMs,
                             float lr, float ud, uint32_t nowMs) {
	if (!AimFinite(lr, ud))
		return false;
	if (!sent)
		return true;
	if (AimAbs(lr - sentLR) > AIM_EPSILON || AimAbs(ud - sentUD) > AIM_EPSILON)
		return true;
	return static_cast<uint32_t>(nowMs - sentAtMs) >= AIM_REFRESH_MS;
}

} // namespace coopiii
