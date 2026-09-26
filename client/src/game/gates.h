// main.scm's seven scripted gates, open on every machine while anybody's own
// script wants them open.
//
// gates.sc starts one main-script thread per gate object init.sc creates: the
// two Staunton police HQ gates, the Colombians' compound in Aspatria and
// their mansion in Cedar Grove, Phil's, the fish factory's and the dog-food
// factory's. Each wakes once a second and, only while the *local* player is
// in the gate's zone, slides the gate open when he is in the right box or the
// right car (a police car, a Colombian car, the fish van) and shut when he is
// not. A teammate driving the police car through saw the gate open on his own
// screen and drove into it on everybody else's. The mission replay never
// covered it: its SLIDE_OBJECT entry replays mission scripts only, and these
// are the main script's own threads.
//
// **The garages' union again (garage.h).** A gate belongs to the map. Each
// machine reports one bit per gate, "my own GATES thread last asked for this
// gate open", and a gate is held open while anybody's bit is set:
//
//   - The bit is read off the thread's own SLIDE_OBJECT. The script asks for
//     the open position every second while it wants the gate open and for the
//     shut one while it does not, so the bit is "an open request inside the
//     last 2.5 s and no shut request since". A player who leaves the zone
//     stops asking, and his bit lapses.
//   - Held open, the local thread's request to shut is answered "done"
//     without moving anything, so its loop ends as it would have and the
//     gate stays up. What moves it open, on a machine whose own thread may
//     not be running at all (its player is across the city), is the engine's
//     own SLIDE_OBJECT, run once a frame on a script of CoopIII's with the
//     same operands gates.sc uses.
//   - Released, a gate CoopIII opened is slid shut again the same way, with
//     the engine's collision check on as the script's own close has it - but
//     only while the local thread is not looking after it itself.
//
// **A gate is named by where gates.sc slides it.** The two positions are
// literals in the script and the same on every machine; the gate's object is
// whatever the global init.sc stored it in holds, found by the CREATE_OBJECT
// that put it at its shut position. main.scm is only read, never written.
//
// The one part that needs the mission hooks is the reading: SLIDE_OBJECT is
// seen in the script range handler game/mission.cpp detours, so with
// `missions = off` this machine never says it wants a gate open (and so
// holds nothing for a teammate's view of it), while still opening gates for
// everybody else.
#pragma once

#include "../client.h"

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

struct GateDef {
	const char *name;
	float       shut[3];
	float       open[3];
	float       speed[3];   // gates.sc's own step per frame
};

// In gates.sc's order. Every coordinate is a multiple of 1/16, which is what
// a SCM float literal is (a 16-bit fixed point over 16), so the script's
// -1107.938 is -1107.9375 exactly.
inline const GateDef &Gate(size_t i) {
	static const GateDef kGates[GATE_COUNT] = {
	    {"fish factory", {1016.0f, -1107.9375f, 12.25f}, {1016.0f, -1099.4375f, 12.25f},
	     {0.0f, 0.0625f, 0.0f}},
	    {"dog food factory", {1250.375f, -812.0f, 13.9375f}, {1244.375f, -818.0f, 13.9375f},
	     {0.0625f, 0.0625f, 0.0f}},
	    {"police HQ 1", {366.125f, -1128.5f, 21.9375f}, {358.125f, -1128.5f, 21.9375f},
	     {0.0625f, 0.0f, 0.0f}},
	    {"police HQ 2", {326.25f, -1128.5f, 21.9375f}, {332.0f, -1128.5f, 21.9375f},
	     {0.0625f, 0.0f, 0.0f}},
	    {"Colombian compound", {91.5625f, -318.5625f, 15.25f}, {91.5625f, -327.3125f, 15.25f},
	     {0.0f, 0.0625f, 0.0f}},
	    {"Phil's", {147.1875f, 207.3125f, 10.5625f}, {147.1875f, 214.5f, 10.5625f},
	     {0.0f, 0.0625f, 0.0f}},
	    {"Colombian mansion", {-363.0f, 250.4375f, 61.3125f}, {-370.0f, 250.4375f, 61.3125f},
	     {0.0625f, 0.0f, 0.0f}},
	};
	return kGates[i < GATE_COUNT ? i : 0];
}

// Which gate a SLIDE_OBJECT to (x, y, z) moves, and whether that is its open
// end; -1 for none. A hundredth of a metre, since both sides are the same
// literal: Arms Shortage slides Phil's gate to 214.8125, which is its own
// business and not this.
inline int GateForTarget(float x, float y, float z, bool *toOpen) {
	auto atPoint = [&](const float *p) {
		const float dx = p[0] - x, dy = p[1] - y, dz = p[2] - z;
		return dx * dx + dy * dy + dz * dz < 0.01f * 0.01f;
	};
	for (size_t i = 0; i < GATE_COUNT; ++i) {
		if (atPoint(Gate(i).open)) {
			if (toOpen)
				*toOpen = true;
			return static_cast<int>(i);
		}
		if (atPoint(Gate(i).shut)) {
			if (toOpen)
				*toOpen = false;
			return static_cast<int>(i);
		}
	}
	return -1;
}

// How long an open request stands. The thread asks once a second while the
// gate is up, so this is two and a half of its waits.
constexpr uint32_t kGateOpenHoldMs = 2500;
// How long after the local thread last touched a gate it is still the one
// looking after it.
constexpr uint32_t kGateLocalActiveMs = 1500;

// The bit this machine reports. Times are 0 for never.
constexpr bool GateWantedOpen(uint32_t lastOpenMs, uint32_t lastShutMs, uint32_t nowMs) {
	return lastOpenMs != 0 && lastOpenMs >= lastShutMs && nowMs - lastOpenMs < kGateOpenHoldMs;
}

// The local thread's own request, answered "done" without running it: it asks
// to shut a gate somebody else is holding open.
constexpr bool SwallowGateSlide(bool heldByOthers, bool toOpen) {
	return heldByOthers && !toOpen;
}

enum class GateDrive : uint8_t { None, Open, Shut };

// What CoopIII does to a gate this frame. `held` is the union of the others'
// bits, `localOpen` this machine's own, `driven` that CoopIII opened it and
// has not seen it shut since, `localActive` that the local thread touched it
// within kGateLocalActiveMs and so is closing it itself.
constexpr GateDrive DecideGateDrive(bool held, bool localOpen, bool driven, bool localActive) {
	if (held && !localOpen)
		return GateDrive::Open;
	if (!held && driven && !localActive)
		return GateDrive::Shut;
	return GateDrive::None;
}

// Where main.scm keeps a gate: the global operand of the CREATE_OBJECT
// (029B, `create_object #MODEL at x y z` into a global) that puts an object
// at the gate's shut position. 0 when there is none. Reads only; `space` is
// main.scm's part of the script space.
//
// An operand is a type byte and its value: 1 four bytes, 2 a global's offset
// and 3 a local's (two each), 4 one byte, 5 two, 6 a float as two bytes over
// 16 (addresses.h, SCRIPT_PARAM_*).
inline uint16_t FindGateGlobal(const uint8_t *space, size_t size, const GateDef &gate) {
	for (size_t at = 0; at + 2 < size; ++at) {
		if (space[at] != 0x9B || space[at + 1] != 0x02)
			continue;
		size_t p = at + 2;
		// The model, a literal integer of any width.
		if (p >= size)
			continue;
		const uint8_t modelType = space[p];
		const size_t  modelSize = modelType == 1 ? 4 : modelType == 4 ? 1 : modelType == 5 ? 2 : 0;
		if (modelSize == 0)
			continue;
		p += 1 + modelSize;
		bool match = true;
		for (int k = 0; k < 3 && match; ++k) {
			if (p + 3 > size || space[p] != 6) {
				match = false;
				break;
			}
			const int16_t fixed = static_cast<int16_t>(space[p + 1] | (space[p + 2] << 8));
			const float   d     = static_cast<float>(fixed) / 16.0f - gate.shut[k];
			match               = d < 0.01f && d > -0.01f;
			p += 3;
		}
		if (!match || p + 3 > size || space[p] != 2)
			continue;
		const uint16_t global = static_cast<uint16_t>(space[p + 1] | (space[p + 2] << 8));
		if (global >= 8)
			return global;
	}
	return 0;
}

// game/mission.cpp's SLIDE_OBJECT, before it runs. True when the gate
// module has dealt with it and `result` is the handler's return value.
bool GateSlide(void *script, int8_t *result);

void AddGatesToBridge(WorldBridge &bridge);

} // namespace coopiii::game
