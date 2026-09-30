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

// ---- the safehouse doors (protocol.h, GATE_BIT_*_DOOR) ----------------------
//
// Not gates and not garages: init.sc makes each safehouse's pedestrian door a
// script object and save.sc's thread for that island moves it, every 250 ms
// while the *local* player is in the safehouse's zone, open when he is not on
// a mission and shut when he is, and shut and open again around the save
// screen. So it stood open on the screen of whoever walked up and shut on
// everybody else's.
//
//   Portland   I_SAVE, 'REDLIGH'. $PORTLAND_HIDEOUT_DOOR, #PLAYERSDOOR at
//              890.875 -307.6875 8.75, turned by ROTATE_OBJECT (034D) to 210
//              (open, once Luigi's Girls is done) or 0, step 10.
//   Staunton   C_SAVE, 'PARK'. Two halves, $STAUNTON_HIDEOUT_DOOR1 and 2,
//              made at 103.8125 and 102.1875 -482.75 16.25, slid apart by
//              SLIDE_OBJECT (034E) to 105.3125 and 100.6875, and back, at
//              0.0625 a frame along x. Both halves in one `if or`.
//   Shoreside  S_SAVE, 'PROJECT'. $SSV_HIDEOUT_DOOR, #NEWTOWERDOOR1 at
//              -664.3125 2.875 19.5, turned to 250 (open) or 180, step 10.
//
// Every literal above was read off the retail main.scm, not the decompile.
//
// The same union as the gates, one bit a door. The bit is "my own save thread
// asked for the open end in the last 2.5 s and not for the shut one since";
// held by anybody else, the local thread's move to the shut end is answered
// done without moving it, and the door is moved open on a script of
// CoopIII's with save.sc's own operands. Released, a door CoopIII opened is
// moved back to the shut end.
//
// Found by its object, not its target: a move is a door's when its handle is
// the one the door's global holds. A mission script moving a door
// (19_8ball.sc turns Portland's, both ways) is never touched, and while one
// has moved it in the last kDoorMissionHoldMs CoopIII leaves that door alone
// - two machines moving it opposite ways every frame would hold a mission's
// `while not rotate_object` loop open for ever.
enum : uint8_t {
	DOOR_PORTLAND,
	DOOR_STAUNTON,
	DOOR_SHORESIDE,
	HIDEOUT_DOOR_COUNT,
};

constexpr float    HIDEOUT_DOOR_AT[3]  = {890.875f, -307.6875f, 8.75f};
constexpr float    HIDEOUT_DOOR_OPEN   = 210.0f;
constexpr float    HIDEOUT_DOOR_SHUT   = 0.0f;
constexpr float    HIDEOUT_DOOR_STEP   = 10.0f;   // save.sc's own, per frame
constexpr uint32_t kDoorMissionHoldMs  = 3000;

struct HideoutDoorDef {
	const char *name;
	uint16_t    bit;
	// Slid in two halves (Staunton) rather than turned whole.
	bool        slides;
	uint8_t     halves;
	// Where init.sc makes each half, which is its shut end.
	float       at[2][3];
	// Slid: where save.sc slides each half open, and its step a frame.
	float       open[2][3];
	float       speed[3];
	// Turned: save.sc's headings and its step a frame.
	float       openHeading;
	float       shutHeading;
	float       step;
};

inline const HideoutDoorDef &HideoutDoor(size_t d) {
	static const HideoutDoorDef kDoors[HIDEOUT_DOOR_COUNT] = {
	    {"Portland", GATE_BIT_HIDEOUT_DOOR, false, 1,
	     {{HIDEOUT_DOOR_AT[0], HIDEOUT_DOOR_AT[1], HIDEOUT_DOOR_AT[2]}, {}},
	     {}, {}, HIDEOUT_DOOR_OPEN, HIDEOUT_DOOR_SHUT, HIDEOUT_DOOR_STEP},
	    {"Staunton", GATE_BIT_STAUNTON_DOOR, true, 2,
	     {{103.8125f, -482.75f, 16.25f}, {102.1875f, -482.75f, 16.25f}},
	     {{105.3125f, -482.75f, 16.25f}, {100.6875f, -482.75f, 16.25f}},
	     {0.0625f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f},
	    {"Shoreside", GATE_BIT_SHORESIDE_DOOR, false, 1,
	     {{-664.3125f, 2.875f, 19.5f}, {}},
	     {}, {}, 250.0f, 180.0f, 10.0f},
	};
	return kDoors[d < HIDEOUT_DOOR_COUNT ? d : 0];
}

// Which way a ROTATE_OBJECT's target turns a turned door; -1 for an angle
// that is neither (the save screen's shut with a step of 8 is still a shut).
inline int DoorTargetOpen(const HideoutDoorDef &door, float target) {
	if (target > door.openHeading - 0.01f && target < door.openHeading + 0.01f)
		return 1;
	if (target > door.shutHeading - 0.01f && target < door.shutHeading + 0.01f)
		return 0;
	return -1;
}

// Which end a SLIDE_OBJECT's target moves one half of a slid door to; -1 for
// neither. A hundredth of a metre, as GateForTarget.
inline int DoorSlideTargetOpen(const HideoutDoorDef &door, size_t half, float x, float y,
                               float z) {
	if (half >= door.halves)
		return -1;
	auto atPoint = [&](const float *p) {
		const float dx = p[0] - x, dy = p[1] - y, dz = p[2] - z;
		return dx * dx + dy * dy + dz * dz < 0.01f * 0.01f;
	};
	if (atPoint(door.open[half]))
		return 1;
	if (atPoint(door.at[half]))
		return 0;
	return -1;
}

// Two headings in degrees, 0..360 as the handler takes them, within half a
// degree of each other across the wrap.
inline bool DoorHeadingAt(float heading, float target) {
	float d = heading - target;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return d < 0.5f && d > -0.5f;
}

// Whether CoopIII keeps its hands off a door a mission moved at
// `missionMs` (0 for never).
constexpr bool DoorLeftToMission(uint32_t missionMs, uint32_t nowMs) {
	return missionMs != 0 && nowMs - missionMs < kDoorMissionHoldMs;
}

// game/mission.cpp's ROTATE_OBJECT, before it runs; GateSlide's shape.
bool GateRotate(void *script, int8_t *result);

void AddGatesToBridge(WorldBridge &bridge);

} // namespace coopiii::game
