// Where the Shoreside lift bridge is, as a function of the clock.
//
// CBridge::Update (0x00413AC0) raises and lowers the lift bridge between
// Staunton Island and Shoreside Vale from `(t - epoch) & 0xFFFF`, where t is
// CTimer::m_snTimeInMilliseconds and the epoch is CTimer's own value on the
// first frame after the script has run COMMERCIAL_PASSED (addresses.h, "lift
// bridge"). Two machines disagree about both numbers, so in a session
// game/liftbridge.cpp picks the epoch for each call that makes the phase
// `session & 0xFFFF` - the same on every machine, and no packet needed to
// agree it, because the epoch in single player is arbitrary anyway.
//
// The bridge also switches the traffic paths across it (bBridgeLights on
// every car path link in a fixed rectangle), and only on two of its state
// changes. A clock that jumps can skip one of them, and then traffic queues
// at a bridge that is down, or drives at one that is up.
// LinksAfterBridgeUpdate is the rule that puts that right.
//
// Pure arithmetic, so clienttest runs all of it.
#pragma once

#include <cstdint>

namespace coopiii {

// Transcribed from CBridge::Update, not from re3. `and edx,0FFFFh` on
// `t - epoch`, then unsigned compares against these.
constexpr uint32_t BRIDGE_PERIOD_MS          = 0x10000;   // 65.536 s
constexpr uint32_t BRIDGE_DOWN_FROM_MS       = 0x2710;    // 10000
constexpr uint32_t BRIDGE_WARNING_FROM_MS    = 0x9C40;    // 40000
constexpr uint32_t BRIDGE_RAISING_FROM_MS    = 0xC350;    // 50000
constexpr uint32_t BRIDGE_UP_FROM_MS         = 0xEA60;    // 60000

// CBridge::State (0x008F2A1C), as the immediates Update writes into it.
enum : int32_t {
	BRIDGE_LOCKED          = 0,   // COMMERCIAL_PASSED not run yet: up, and stays up
	BRIDGE_UP              = 1,
	BRIDGE_LOWERING        = 2,
	BRIDGE_DOWN            = 3,
	BRIDGE_ABOUT_TO_RAISE  = 4,
	BRIDGE_RAISING         = 5,
};

constexpr uint32_t BridgePhaseMs(uint32_t clockMs, uint32_t epochMs) {
	return (clockMs - epochMs) & (BRIDGE_PERIOD_MS - 1);
}

// The state Update writes for a phase, once the bridge is working.
constexpr int32_t BridgeStateAt(uint32_t phaseMs) {
	return phaseMs < BRIDGE_DOWN_FROM_MS      ? BRIDGE_LOWERING
	       : phaseMs < BRIDGE_WARNING_FROM_MS ? BRIDGE_DOWN
	       : phaseMs < BRIDGE_RAISING_FROM_MS ? BRIDGE_ABOUT_TO_RAISE
	       : phaseMs < BRIDGE_UP_FROM_MS      ? BRIDGE_RAISING
	                                          : BRIDGE_UP;
}

// The epoch to put in TimeOfBridgeBecomingOperational (0x008F2BC0) before a
// call, so that Update, reading CTimer's own `oursMs`, computes the phase
// `sessionMs & 0xFFFF`. CTimer itself is never touched. Zero is the engine's
// "not stamped yet" and would be overwritten with CTimer, so a zero is moved
// by one whole period, which the mask can't see.
constexpr uint32_t BridgeEpochForSession(uint32_t oursMs, uint32_t sessionMs) {
	return oursMs - sessionMs != 0 ? oursMs - sessionMs : BRIDGE_PERIOD_MS;
}

// What bBridgeLights on the bridge's links should be for a state: cars stop
// for the bridge in every state but down. That is what the engine's own two
// edges leave behind on an unbroken cycle - CBridge::Init lights them,
// ABOUT_TO_RAISE after DOWN lights them, DOWN after LOWERING puts them out.
constexpr bool BridgeLinksLit(int32_t state) {
	return state != BRIDGE_DOWN;
}

enum class BridgeLinks : uint8_t { Leave, Light, PutOut };

// What to do about the links after one Update, given the state before and
// after it and whether CBridge::Init had run since the previous Update (Init
// lights the links whatever the bridge is doing). Leave means the engine
// already left them right: nothing changed, or the change was one step of the
// ordinary cycle, which either is one of the engine's two edges or doesn't
// touch the links. Anything else - a step skipped, a step backwards - is a
// clock that jumped, and the links are set for where the bridge now is.
constexpr BridgeLinks LinksAfterBridgeUpdate(int32_t before, int32_t after, bool initRan) {
	if (initRan)
		return BridgeLinksLit(after) ? BridgeLinks::Leave : BridgeLinks::PutOut;
	if (before == after)
		return BridgeLinks::Leave;
	const bool oneStep = (before == BRIDGE_LOCKED && after == BRIDGE_LOWERING) ||
	                     (before == BRIDGE_UP && after == BRIDGE_LOWERING) ||
	                     (before == BRIDGE_LOWERING && after == BRIDGE_DOWN) ||
	                     (before == BRIDGE_DOWN && after == BRIDGE_ABOUT_TO_RAISE) ||
	                     (before == BRIDGE_ABOUT_TO_RAISE && after == BRIDGE_RAISING) ||
	                     (before == BRIDGE_RAISING && after == BRIDGE_UP);
	if (oneStep)
		return BridgeLinks::Leave;
	return BridgeLinksLit(after) ? BridgeLinks::Light : BridgeLinks::PutOut;
}

} // namespace coopiii
