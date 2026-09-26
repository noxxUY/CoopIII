// The session's one mission, script-engine side. missionsync.h is the session
// side, docs/missions.md the design, and missionaddr.h every address and
// offset this reaches, with why none of it lives in addresses.h.
//
// Off unless CoopIII.ini says `missions = on`: InstallMissionHooks refuses
// otherwise, and with nothing installed every mission is this machine's own,
// as it always was.
//
// What it does, all of it on the game thread:
//
//   - **A contact's start gate.** CAN_PLAYER_START_MISSION (03EE) is asked by
//     every contact's and payphone's trigger right before it commits to the
//     launch. When the engine says yes, the session is asked
//     (MissionSync::AskStartGate) and the answer is forced to no until the
//     session grants it. The start's area is the last locate the same thread
//     passed, the marker the player stands in. The three save points ask 03EE
//     too, and are told apart by what follows the question
//     (ClassifyStartGate).
//   - **The launch and the end.** START_MISSION (0417) reports which mission
//     this machine just started, and REGISTER_MISSION_PASSED (0318) seen in
//     between that it was passed. The end is its script's own end
//     (TERMINATE_THIS_SCRIPT), not MISSION_HAS_FINISHED (00D8): a failure
//     runs that twice, and everything the mission's cleanup does between
//     the first and its end, the blips it takes down, the pedestrians it
//     deletes, the flags it puts back, is still the mission's and goes to
//     everybody. A failed mission's cars that nobody sits in go after it, on
//     every screen, so a retry starts on a clear street.
//   - **Checkpoints.** In the mission's own script, a location check the owner
//     passes whose area holds one of the mission's coordinate blips waits for
//     everybody (MissionSync::AskCheckpoint): until then it is forced to no,
//     and the script simply keeps waiting, as it does for the owner.
//   - **A participant's $ONMISSION** is held at 1 each frame while the
//     session's mission runs, and set back to 0 after, unless this machine has
//     a mission of its own loaded.
//   - **The death rule.** The owner's running mission fails the way the
//     engine fails it for the owner's own death (UnwindForDeatharrest).
//
// A condition is only ever forced when that is safe to do: never under an
// `if or`, where one false does not decide the block, and never under a NOT.
#pragma once

#include "missionaddr.h"
#include "replay.h"

#include <coopiii/mission.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace coopiii {
class Client;
struct WorldBridge;
}

namespace coopiii::game {

// ---- pure, for tools/clienttest ---------------------------------------------------

// One operand at `ip`: its value as an integer and how many bytes it took,
// or false for a type this cannot read without the running script (a
// local variable) or past the end.
inline bool ReadIntOperand(const uint8_t *space, uint32_t size, uint32_t ip, int32_t *value,
                           uint32_t *length) {
	if (ip >= size)
		return false;
	const uint8_t type = space[ip];
	switch (type) {
	case scripts::PARAM_INT8:
		if (ip + 2 > size)
			return false;
		*value  = static_cast<int8_t>(space[ip + 1]);
		*length = 2;
		return true;
	case scripts::PARAM_INT16:
		if (ip + 3 > size)
			return false;
		*value  = static_cast<int16_t>(space[ip + 1] | (space[ip + 2] << 8));
		*length = 3;
		return true;
	case scripts::PARAM_INT32:
		if (ip + 5 > size)
			return false;
		*value = static_cast<int32_t>(static_cast<uint32_t>(space[ip + 1]) |
		                              (static_cast<uint32_t>(space[ip + 2]) << 8) |
		                              (static_cast<uint32_t>(space[ip + 3]) << 16) |
		                              (static_cast<uint32_t>(space[ip + 4]) << 24));
		*length = 5;
		return true;
	case scripts::PARAM_GLOBAL: {
		if (ip + 3 > size)
			return false;
		const uint32_t at = static_cast<uint32_t>(space[ip + 1] | (space[ip + 2] << 8));
		if (at + 4 > size)
			return false;
		std::memcpy(value, space + at, 4);
		*length = 3;
		return true;
	}
	default:
		return false;
	}
}

// How many bytes an operand of this type takes, its type byte included, or 0
// for one that is not a single typed operand (an 8-byte text label has no
// type byte at all).
inline uint32_t OperandLength(uint8_t type) {
	switch (type) {
	case scripts::PARAM_INT32:  return 5;
	case scripts::PARAM_GLOBAL: return 3;
	case scripts::PARAM_LOCAL:  return 3;
	case scripts::PARAM_INT8:   return 2;
	case scripts::PARAM_INT16:  return 3;
	case scripts::PARAM_FLOAT:  return 3;   // 16 bits of 1/16ths in GTA III
	default:                    return 0;
	}
}

// What a CAN_PLAYER_START_MISSION is asked for, from the instruction its
// `goto_if_false` falls through to. Every one of the 69 in main.scm is
// `03EE player / 004D goto_if_false label` and then, when it passes:
//
//   03EF  make the player safe, then fade and launch   42 contacts
//   024E  switch the phone off, then the call           17 payphones
//   0004  $RAYS_CUTSCENE_FLAG = 1                        5, Ray's phone
//   0417  START_MISSION at once                          2, Give Me Liberty
//   00D6  another `if`: $ONMISSION, the door, the menu   3, the save points
//
// `ip` is where 03EE's operand starts, which is where the dispatcher leaves
// the script's instruction pointer.
enum class GateKind : uint8_t { Launch, NotALaunch, Unknown };

inline GateKind ClassifyStartGate(const uint8_t *space, uint32_t size, uint32_t ip) {
	if (ip >= size)
		return GateKind::Unknown;
	uint32_t at  = ip;
	uint32_t len = OperandLength(space[at]);
	if (len == 0 || at + len + 2 > size)
		return GateKind::Unknown;
	at += len;
	if (space[at] != 0x4D || space[at + 1] != 0x00)   // 004D, goto_if_false
		return GateKind::Unknown;
	at += 2;
	len = at < size ? OperandLength(space[at]) : 0;
	if (len == 0 || at + len + 2 > size)
		return GateKind::Unknown;
	at += len;
	const uint16_t next = static_cast<uint16_t>((space[at] | (space[at + 1] << 8)) & 0x7FFF);
	switch (next) {
	case 0x03EF:
	case 0x024E:
	case 0x0004:
	case scripts::op::LOAD_AND_LAUNCH_MISSION_INTERNAL:
		return GateKind::Launch;
	case 0x00D6:
		return GateKind::NotALaunch;
	default:
		return GateKind::Unknown;
	}
}

// The mission a START_MISSION within `window` bytes after `ip` launches, or
// MISSION_NONE when there is none. A contact's trigger reaches its 0417 a few
// dozen bytes after its 03EE, past the make-safe, the fade and the title, and
// a payphone's up to 1652 bytes after, past the whole call; a save point's is
// followed by nothing like it for 2000. The bytes are only searched, not
// decoded, so a match must also name a mission that exists: it is the hint
// for the words on the HUD, never what decides whether a gate is one.
constexpr uint32_t LAUNCH_LOOKAHEAD_BYTES = 2048;

inline uint16_t FindLaunchAhead(const uint8_t *space, uint32_t size, uint32_t ip,
                                uint32_t window = LAUNCH_LOOKAHEAD_BYTES) {
	const uint32_t end = ip + window < size ? ip + window : size;
	for (uint32_t at = ip; at + 2 < end; ++at) {
		if (space[at] != (scripts::op::LOAD_AND_LAUNCH_MISSION_INTERNAL & 0xFF) ||
		    space[at + 1] != (scripts::op::LOAD_AND_LAUNCH_MISSION_INTERNAL >> 8))
			continue;
		int32_t  number = 0;
		uint32_t len    = 0;
		if (space[at + 2] == scripts::PARAM_GLOBAL)
			continue;   // never how main.scm names a mission, and not worth a guess
		if (ReadIntOperand(space, size, at + 2, &number, &len) && number >= 0 &&
		    number < MISSION_COUNT)
			return static_cast<uint16_t>(number);
	}
	return MISSION_NONE;
}

// The area a location condition describes, from its operands as
// CollectParameters left them: the player, then the area, then the sphere
// flag. False for anything that is not about an area of the map.
inline bool AreaForCondition(int32_t command, const float *params, MissionArea *out) {
	using namespace scripts::op;
	if (command >= LOCATE_PLAYER_ANY_MEANS_2D && command <= LOCATE_STOPPED_PLAYER_IN_CAR_2D) {
		*out = MissionAreaLocate2D(params[1], params[2], params[3], params[4]);
		return true;
	}
	if (command >= LOCATE_PLAYER_ANY_MEANS_3D && command <= LOCATE_STOPPED_PLAYER_IN_CAR_3D) {
		*out = MissionAreaLocate3D(params[1], params[2], params[3], params[4], params[5], params[6]);
		return true;
	}
	if (command == IS_PLAYER_IN_AREA_2D ||
	    (command >= IS_PLAYER_IN_AREA_ON_FOOT_2D && command <= IS_PLAYER_STOPPED_IN_AREA_IN_CAR_2D)) {
		*out = MissionAreaCorners2D(params[1], params[2], params[3], params[4]);
		return true;
	}
	if (command == IS_PLAYER_IN_AREA_3D ||
	    (command >= IS_PLAYER_IN_AREA_ON_FOOT_3D && command <= IS_PLAYER_STOPPED_IN_AREA_IN_CAR_3D)) {
		*out = MissionAreaCorners3D(params[1], params[2], params[3], params[4], params[5], params[6]);
		return true;
	}
	return false;
}

inline bool IsLocationCondition(int32_t command) {
	MissionArea unused{};
	const float zeros[8] = {};
	return AreaForCondition(command, zeros, &unused);
}

// A coordinate blip the mission script put on the radar.
struct CoordBlip {
	int32_t handle = -1;
	float   x      = 0.0f;
	float   y      = 0.0f;
};

// A checkpoint is a location check whose area holds one of the mission's
// coordinate blips, which is how the game draws "go here" (mission-audit.md
// C1). A metre of slack, because a script puts the blip and the locate at the
// same coordinates written twice, and GTA III's floats are 1/16ths.
inline bool HoldsCoordBlip(const MissionArea &area, const CoordBlip *blips, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		const Vec3 at{blips[i].x, blips[i].y, area.centre.z};
		MissionArea flat = area;
		flat.shape       = MISSION_AREA_BOX2D;
		if (InMissionArea(at, flat, 1.0f))
			return true;
	}
	return false;
}

// The script START_MISSION put in the mission slot: the intro, an info scene,
// or a mission this machine runs. Read off SCRIPT_MISSION_SLOT and never off
// SCRIPT_MISSION_RULES, which seven of main.scm's own threads carry for the
// whole game (missionaddr.h).
inline bool IsMissionSlotScript(const void *script) {
	const uint8_t *s = static_cast<const uint8_t *>(script);
	return s && s[scripts::layout::SCRIPT_MISSION_SLOT] != 0;
}

// Whether any script on a running list, from its head, is the mission slot's.
inline bool AnyMissionSlotScript(uintptr_t head) {
	for (uintptr_t s = head; s != 0;
	     s = *reinterpret_cast<const uintptr_t *>(s + scripts::layout::SCRIPT_NEXT))
		if (IsMissionSlotScript(reinterpret_cast<const void *>(s)))
			return true;
	return false;
}

// Whether a condition may be forced to no after the engine said yes: not
// under a NOT, where the engine's yes was its no, and not inside an `if or`,
// where one condition's no does not decide the block.
inline bool MayForceCondition(uint16_t andOr, bool notFlag) {
	return !notFlag && !(andOr >= scripts::ANDOR_ORS_1 && andOr <= scripts::ANDOR_ORS_8);
}

// What CRunningScript::DoDeatharrestCheck does to a mission's script when its
// player dies or is busted, done to the owner's mission for somebody else:
// back to the bottom of the gosub stack, where every mission's top level asks
// HAS_DEATHARREST_BEEN_EXECUTED and runs its own failure (re3 Script.cpp,
// DoDeatharrestCheck; every one of the 80 does it, missions.md 5.7). `script`
// is the CRunningScript's memory. False, changing nothing, when the script
// is not inside its mission's body, or has its check disarmed and
// `evenDisarmed` is not set.
//
// Two missions turn the check off for their whole length, TAXI and HOOD1
// (Uzi Money), and watch their player themselves. Neither has a failure
// branch at its top level: the gosub after its body is its cleanup, so
// unwound anyway, each ends there the way it ends its shift or its frenzy.
inline bool UnwindForDeatharrest(uint8_t *script, int32_t *onMission, bool evenDisarmed = false) {
	using namespace scripts::layout;
	if (!script || (script[SCRIPT_DEATHARREST_ARMED] == 0 && !evenDisarmed))
		return false;
	uint16_t sp = 0;
	std::memcpy(&sp, script + SCRIPT_SP, 2);
	if (sp == 0 || sp > scripts::SCRIPT_STACK_DEPTH)
		return false;
	uint32_t bottom = 0;
	std::memcpy(&bottom, script + SCRIPT_STACK, 4);
	const uint16_t zero = 0;
	std::memcpy(script + SCRIPT_SP, &zero, 2);
	std::memcpy(script + SCRIPT_IP, &bottom, 4);
	script[SCRIPT_DEATHARREST_DONE] = 1;
	const uint32_t now = 0;
	std::memcpy(script + SCRIPT_WAKE_TIME, &now, 4);
	if (onMission)
		*onMission = 0;
	return true;
}

// A condition the engine has just answered: what it found, before the NOT
// the script wrote in front of it, and what the script's flag must say for it
// to have been true after all (mission-audit.md R13, answered for every
// participant).
inline bool RawCondition(uint8_t condResult, bool notFlag) { return (condResult != 0) != notFlag; }
inline uint8_t TrueAfterAll(bool notFlag) { return notFlag ? 0 : 1; }

// ---- what a mission leaves behind (protocol.h, C_CampaignDelta) ----------------------

// How an instruction writes the global that is its first operand.
//   Value: a number, never a handle. The plain assignment and the arithmetic
//     with a literal (0004..0015), and with another variable (0058..0075),
//     the timed float arithmetic (0078..0082), a float copied (0086, 0088),
//     a conversion (008C, 008D, 0090, 0091) and ABS (0094, 0096). Nothing
//     adds, scales or converts a handle, and a float is never one.
//   Copy: an int copied from another global (0084). What it copies decides:
//     `$RC1_RECORD = $COUNTER_RC` is a record, `$INJURED_PED_9 =
//     $INJURED_PED_TEMP` a pedestrian (CampaignWrites::NoteCopy).
//   CopyLocal: an int copied from a local (008A). A value: its one use in
//     the campaign is a particle's time (ASUSB3).
// Only the forms whose first operand is a global are here; the local ones
// write nothing the campaign keeps.
enum class Assignment : uint8_t { None, Value, Copy, CopyLocal };

inline Assignment AssignmentOf(int32_t command) {
	switch (command) {
	case 0x0004: case 0x0005: case 0x0008: case 0x0009: case 0x000C:
	case 0x000D: case 0x0010: case 0x0011: case 0x0014: case 0x0015:
	case 0x0058: case 0x0059: case 0x005E: case 0x005F:   // += var, += lvar
	case 0x0060: case 0x0061: case 0x0066: case 0x0067:   // -=
	case 0x0068: case 0x0069: case 0x006C: case 0x006D:   // *=
	case 0x0070: case 0x0071: case 0x0074: case 0x0075:   // /=
	case 0x0078: case 0x007A: case 0x007C:                // +=@
	case 0x007E: case 0x0080: case 0x0082:                // -=@
	case 0x0086: case 0x0088:                             // float = float
	case 0x008C: case 0x008D: case 0x0090: case 0x0091:   // conversions
	case 0x0094: case 0x0096:                             // ABS
		return Assignment::Value;
	case 0x0084:
		return Assignment::Copy;
	case 0x008A:
		return Assignment::CopyLocal;
	default:
		return Assignment::None;
	}
}

inline bool IsTrackedAssignment(int32_t command) { return AssignmentOf(command) != Assignment::None; }

// An instruction that puts a handle into its last operand: a pedestrian, a
// car, an object, a blip, a pickup, a sphere, a fire, a car generator, a
// sound (re3's handlers that store a pool reference or a blip, pickup,
// sphere or fire index; the operand counts are SCM.ini's). An output is
// always a variable, three bytes with its type, so the last operand starts
// three bytes before the next instruction whichever handler this is.
inline bool WritesHandle(int32_t command) {
	switch (command) {
	case 0x0053:   // CREATE_PLAYER
	case 0x009A:   // CREATE_CHAR
	case 0x00A5:   // CREATE_CAR
	case 0x00D9: case 0x00DA:   // STORE_CAR_CHAR_IS_IN, STORE_CAR_PLAYER_IS_IN
	case 0x0107:   // CREATE_OBJECT
	case 0x0129:   // CREATE_CHAR_INSIDE_CAR
	case 0x014B:   // CREATE_CAR_GENERATOR
	case 0x0161: case 0x0162: case 0x0163: case 0x0167:   // ADD_BLIP_FOR_*_OLD
	case 0x0186: case 0x0187: case 0x0188: case 0x0189: case 0x018A:   // ADD_BLIP_FOR_*
	case 0x018D:   // ADD_CONTINUOUS_SOUND
	case 0x01C8:   // CREATE_CHAR_AS_PASSENGER
	case 0x01F5:   // GET_PLAYER_CHAR
	case 0x0213:   // CREATE_PICKUP
	case 0x029B:   // CREATE_OBJECT_NO_OFFSET
	case 0x02A4: case 0x02A5: case 0x02A6: case 0x02A7: case 0x02A8:   // ADD_SPRITE_BLIP_FOR_*
	case 0x02CF:   // START_SCRIPT_FIRE
	case 0x02DC: case 0x02DD:   // GET_RANDOM_CHAR_IN_AREA / _ZONE
	case 0x02E1:   // CREATE_MONEY_PICKUP
	case 0x02E5: case 0x02F4:   // CREATE_CUTSCENE_OBJECT, CREATE_CUTSCENE_HEAD
	case 0x0325: case 0x0326:   // START_CAR_FIRE, START_CHAR_FIRE
	case 0x0327: case 0x0328:   // GET_RANDOM_CAR_OF_TYPE_IN_AREA / _ZONE
	case 0x032B:   // CREATE_PICKUP_WITH_AMMO
	case 0x035B:   // CREATE_FLOATING_PACKAGE
	case 0x0376:   // CREATE_RANDOM_CHAR
	case 0x03B9:   // GRAB_CATALINA_HELI
	case 0x03BC:   // ADD_SPHERE
	case 0x03C0: case 0x03C1:   // STORE_CAR_*_IS_IN_NO_SAVE
	case 0x03DB: case 0x03DC: case 0x03DD:   // ADD_*BLIP_FOR_PICKUP
	case 0x0432:   // GET_CHAR_IN_CAR_PASSENGER_SEAT
	case 0x0439:   // GET_CHASE_CAR
	case 0x045D:   // GET_CLOSEST_OBJECT_OF_TYPE
	case 0x0469: case 0x046A:   // GET_RANDOM_COP_IN_AREA / _ZONE
	case 0x046C:   // GET_DRIVER_OF_CAR
		return true;
	default:
		return false;
	}
}

// The global an instruction that has just run stored its handle in, from
// where the next instruction starts: its last operand, when that is a
// global. 0 for a local, or for a script that did not move on.
inline uint16_t HandleOutputGlobal(const uint8_t *space, uint32_t size, uint32_t before, uint32_t after) {
	if (after < before + 3 || after > size || space[after - 3] != scripts::PARAM_GLOBAL)
		return 0;
	return static_cast<uint16_t>(space[after - 2] | (space[after - 1] << 8));
}

// What the owner's mission has written into main.scm's globals, each with
// what it held before the mission's first write and whether what the last
// write left there is a handle. A handle is the owner's engine's and nobody
// else's, so the delta never carries one; a global the mission filled with a
// pedestrian and then set back to 0 is a value again.
template <size_t N>
class CampaignWrites {
public:
	void   Clear() { m_count = 0; }
	size_t Count() const { return m_count; }

	// A write to `at`, which held `before` until now. False when the table
	// is full and `at` was not in it.
	bool Note(uint16_t at, int32_t before, bool handle) {
		for (size_t i = 0; i < m_count; ++i)
			if (m_at[i] == at) {
				m_handle[i] = handle;
				return true;
			}
		if (m_count == N)
			return false;
		m_at[m_count]     = at;
		m_before[m_count] = before;
		m_handle[m_count] = handle;
		++m_count;
		return true;
	}

	// `$at = $from`. A value only when the mission put one into `from`: a
	// global it never wrote may be any of the main script's handles, and a
	// copy of it is left out rather than guessed at.
	bool NoteCopy(uint16_t at, int32_t before, uint16_t from) {
		const int i = Find(from);
		return Note(at, before, i < 0 || m_handle[i]);
	}

	bool Known(uint16_t at) const { return Find(at) >= 0; }
	bool HoldsHandle(uint16_t at) const {
		const int i = Find(at);
		return i >= 0 && m_handle[i];
	}

	// The globals that hold a value other than the one they started with,
	// into `out` (room for N). How many.
	size_t Changed(const uint8_t *space, uint16_t *out) const {
		size_t n = 0;
		for (size_t i = 0; i < m_count; ++i) {
			int32_t now = 0;
			std::memcpy(&now, space + m_at[i], 4);
			if (!m_handle[i] && now != m_before[i])
				out[n++] = m_at[i];
		}
		return n;
	}

private:
	int Find(uint16_t at) const {
		for (size_t i = 0; i < m_count; ++i)
			if (m_at[i] == at)
				return static_cast<int>(i);
		return -1;
	}

	uint16_t m_at[N];
	int32_t  m_before[N];
	bool     m_handle[N];
	size_t   m_count = 0;
};

// ---- the owner's player as a character (missions.md 11.3) ----------------------------

// What the camera points at when the owner's mission points it at the
// owner's own player ped (CAMERA_ON_PED, 0159, on the ped GET_PLAYER_CHAR
// made of the player: Arms Shortage's run to Phil's bunker), in
// place of a netId: each participant's camera goes to its own player, who is
// the story's player on that screen (mission-audit.md R12). Past every netId,
// which is 16 bits.
constexpr int32_t WIRE_OWN_PLAYER = 0x10000;

// The instructions that may name the owner's player that way. Only the
// camera: a pedestrian's orders given to a participant's own player would
// walk it off wherever the owner's walked, which nobody asked of it.
inline bool MayNameOwnPlayer(uint16_t opcode) { return opcode == 0x0159; }

// A scene the script frames itself: the widescreen bars on with the camera
// fixed or pointed (8-Ball's walk, Chaperone's club), which is a cutscene to
// whoever watches it, though no cutscene file plays. The other players are
// hidden for it as for a cutscene, and nothing hurts this machine's own
// player meanwhile, since it stands frozen where the owner left it.
inline bool IsScriptedScene(bool widescreen, bool cutscene) { return widescreen || cutscene; }

// Where main.scm's globals end: its first instruction is the GOTO over them
// (0002, an int32 label). 0 for a script space that does not start that way.
inline uint32_t GlobalsEnd(const uint8_t *space, uint32_t size) {
	if (size < 8 || space[0] != 0x02 || space[1] != 0x00 || space[2] != scripts::PARAM_INT32)
		return 0;
	uint32_t end = 0;
	std::memcpy(&end, space + 3, 4);
	return end > 8 && end < size ? end : 0;
}

// A START_NEW_SCRIPT label that is a main-script thread. A mission's own
// helper threads have labels relative to the mission, stored negative, and
// die with it.
inline bool IsMainThreadLabel(int32_t label) {
	return label >= 0 && static_cast<uint32_t>(label) < scripts::MAIN_SCRIPT_SIZE;
}

// START_NEW_SCRIPT `label`, with no arguments: the label as an int32, then
// the end of its argument list.
inline size_t StartThreadCode(int32_t label, uint8_t (&out)[8]) {
	out[0] = 0x4F;
	out[1] = 0x00;
	out[2] = scripts::PARAM_INT32;
	std::memcpy(out + 3, &label, 4);
	out[7] = 0x00;
	return 8;
}

// The name a thread gives itself, when NAME_THREAD (03A4) is the first thing
// it does: the helper threads missions start all do, and no mission trigger
// does. A text label is eight bytes as they are, with no type before them.
inline bool ThreadNameAt(const uint8_t *space, uint32_t size, int32_t label, char (&out)[8]) {
	if (label < 0 || static_cast<uint32_t>(label) + 10 > size)
		return false;
	const uint8_t *at = space + label;
	if (at[0] != 0xA4 || at[1] != 0x03 || at[2] == 0)
		return false;
	std::memcpy(out, at + 2, 8);
	return true;
}

// What a thread that never names itself is called on a machine that started
// it for a mission: its label, as "@01A2B3". NAME_THREAD's eight bytes, so a
// save keeps it with the thread.
inline void ThreadTag(int32_t label, char (&out)[8]) {
	static const char hex[] = "0123456789ABCDEF";
	const uint32_t    v     = static_cast<uint32_t>(label);
	out[0]                  = '@';
	for (int i = 0; i < 6; ++i)
		out[1 + i] = hex[(v >> (4 * (5 - i))) & 0xF];
	out[7] = '\0';
}

// FNV-1a over main.scm past its globals: its code and its tables, which
// nothing writes while the game runs and no save carries. Two machines with
// the same number have the same labels and the same globals. 0 before
// main.scm is loaded.
inline uint32_t ScriptCodeHash(const uint8_t *space, uint32_t size) {
	const uint32_t from = GlobalsEnd(space, size);
	if (from == 0)
		return 0;
	uint32_t h = 2166136261u;
	for (uint32_t i = from; i < size; ++i) {
		h ^= space[i];
		h *= 16777619u;
	}
	return h ? h : 1;
}

// One instruction the mission ran that the world keeps (replay.h,
// Kind::World), as a part of its delta of its own.
inline CampaignDeltaBody CampaignOpPart(uint16_t missionNumber, const uint8_t *code, size_t length,
                                        uint32_t scriptHash) {
	CampaignDeltaBody b{};
	b.missionNumber = missionNumber;
	b.scriptHash    = scriptHash;
	if (length >= 2 && length <= MISSION_EFFECT_CODE) {
		b.opLength = static_cast<uint8_t>(length);
		std::memcpy(b.op, code, length);
	}
	return b;
}

// The delta for `missionNumber`, CAMPAIGN_VALUES globals a part, the threads
// in the last. Each global's value is read out of `space` as it is now.
// How many parts were filled, 0 when `maxParts` is too few for it.
inline size_t BuildCampaignDelta(uint16_t missionNumber, const uint16_t *offsets, size_t count,
                                 const uint8_t *space, const CampaignThread *threads,
                                 size_t threadCount, uint32_t scriptHash,
                                 CampaignDeltaBody *parts, size_t maxParts) {
	const size_t needed = count == 0 ? 1 : (count + CAMPAIGN_VALUES - 1) / CAMPAIGN_VALUES;
	if (needed > maxParts || threadCount > CAMPAIGN_THREADS)
		return 0;
	for (size_t p = 0; p < needed; ++p) {
		CampaignDeltaBody &b = parts[p];
		b                    = CampaignDeltaBody{};
		b.missionNumber      = missionNumber;
		b.scriptHash         = scriptHash;
		const size_t from    = p * CAMPAIGN_VALUES;
		const size_t n = count - from < CAMPAIGN_VALUES ? count - from : CAMPAIGN_VALUES;
		for (size_t i = 0; i < n && from + i < count; ++i) {
			b.values[i].offset = offsets[from + i];
			std::memcpy(&b.values[i].value, space + offsets[from + i], 4);
		}
		b.valueCount = static_cast<uint8_t>(count == 0 ? 0 : n);
		if (p + 1 == needed) {
			b.last        = 1;
			b.threadCount = static_cast<uint8_t>(threadCount);
			for (size_t t = 0; t < threadCount; ++t)
				b.threads[t] = threads[t];
		}
	}
	return needed;
}

// ---- a participant's spot (missions.md 11.2) ----------------------------------------

// Where participant `rank` of `count` (the owner not counted) stands when the
// mission puts its owner at (x, y): on a ring `radius` out, each at an angle
// of its own, and an eighth of the ring further round for each attempt the
// buildings refuse. SPREAD_RADIUS_M is a step and a half, for a pedestrian
// beside a pedestrian (an enemy's copies).
constexpr uint8_t SPREAD_ATTEMPTS = 8;
constexpr float   SPREAD_RADIUS_M = 1.5f;

inline void SpreadSpot(float x, float y, uint8_t rank, uint8_t count, uint8_t attempt, float *outX,
                       float *outY, float radius = SPREAD_RADIUS_M) {
	const float ring  = count > 4 ? static_cast<float>(count) : 4.0f;
	const float angle = 6.28318531f * (static_cast<float>(rank) / ring +
	                                   static_cast<float>(attempt) / SPREAD_ATTEMPTS);
	*outX = x + radius * std::cos(angle);
	*outY = y + radius * std::sin(angle);
}

// The rings a participant moved by the owner's SET_PLAYER_COORDINATES tries,
// widest first. The engine moves a player in a car by moving the car
// (re3 Script.cpp, COMMAND_SET_PLAYER_COORDINATES), and the owner may be in
// one too, so a ring a step and a half out put cars into each other and a
// player on foot into the owner's car. On foot: clear of a car at the owner's
// spot, whichever way it faces. In a car: a car's length and a half from the
// owner's, and with seven of them round it still six metres apart, more
// than a car is long. The narrower ring is for a spot the buildings leave no
// room round.
constexpr float SPOT_FOOT_RADII_M[2] = {3.0f, 1.5f};
constexpr float SPOT_CAR_RADII_M[2]  = {7.0f, 4.5f};

inline const float *SpotRadii(bool inCar) { return inCar ? SPOT_CAR_RADII_M : SPOT_FOOT_RADII_M; }

// ---- what the owner's mission has up (missions.md 11.5) ------------------------------
//
// Somebody who comes into the mission while it runs, back from a dropped
// connection or new to the session, has missed everything that went out
// before them. So the owner hands them what of it still stands: its blips
// and what was done to them, its pickups, fires and objects, and the HUD's
// timer and counter. Kept in the order it went to everybody. Whatever takes
// something away takes everything done to it along, and a change made again
// replaces the last one, so what is handed over is the mission's picture now
// and not its history.
constexpr size_t MAX_STANDING = 128;

// What one effect is about: a blip, pickup, fire or sphere by the owner's
// handle, an object by its global, one of the HUD's two widgets by the global
// it shows, or a setting of the streets (a density, a zone, a gang's weapons)
// by what it sets, where only the last one said counts.
enum class StandingSubject : uint8_t { None, Blip, Pickup, Fire, Object, Timer, Counter, Sphere, Setting };

struct StandingAbout {
	StandingSubject subject = StandingSubject::None;
	int32_t         id      = 0;
	bool            makes   = false;   // it is what puts the subject up
};

inline uint16_t EffectOpcode(const MissionEffectBody &b) {
	return b.length >= 2 ? static_cast<uint16_t>(b.code[0] | (b.code[1] << 8)) : 0;
}

// The global a widget's DISPLAY or CLEAR names: `02 lo hi` right after the
// opcode.
inline int32_t WidgetGlobalOf(const MissionEffectBody &b) {
	if (b.length < 5 || b.code[2] != scripts::PARAM_GLOBAL)
		return 0;
	return b.code[3] | (b.code[4] << 8);
}

inline StandingAbout AboutOf(const MissionEffectBody &b) {
	StandingAbout  a;
	const uint16_t opcode = EffectOpcode(b);
	switch (b.kind) {
	case MISSION_EFFECT_BLIP_NEW:   return {StandingSubject::Blip, b.ownerBlip, true};
	case MISSION_EFFECT_BLIP_USE:   return {StandingSubject::Blip, b.ownerBlip, false};
	case MISSION_EFFECT_PICKUP_NEW: return {StandingSubject::Pickup, b.ownerBlip, true};
	case MISSION_EFFECT_FIRE_NEW:   return {StandingSubject::Fire, b.ownerBlip, true};
	case MISSION_EFFECT_SPHERE_NEW: return {StandingSubject::Sphere, b.ownerBlip, true};
	case MISSION_EFFECT_RUN:        break;
	default:                        return a;
	}
	int32_t  literal = 0;
	uint16_t global  = 0;
	switch (opcode) {
	case scripts::op::REMOVE_PICKUP:
		if (replay::LiteralAt(b.code, b.length, 0, &literal))
			a = {StandingSubject::Pickup, literal, false};
		break;
	case scripts::op::REMOVE_SCRIPT_FIRE:
		if (replay::LiteralAt(b.code, b.length, 0, &literal))
			a = {StandingSubject::Fire, literal, false};
		break;
	case scripts::op::CREATE_OBJECT:
	case scripts::op::CREATE_OBJECT_NO_OFFSET:
		if (replay::OutGlobalOf(b.code, b.length, &global))
			a = {StandingSubject::Object, global, true};
		break;
	case scripts::op::DELETE_OBJECT:
	case scripts::op::MARK_OBJECT_AS_NO_LONGER_NEEDED:
	case scripts::op::SET_OBJECT_COLLISION:
	case scripts::op::SET_OBJECT_DYNAMIC:
	case scripts::op::FLASH_OBJECT:
		if (replay::ObjectGlobals(b.code, b.length, &global, 1) == 1)
			a = {StandingSubject::Object, global, false};
		break;
	case scripts::op::DISPLAY_ONSCREEN_TIMER:  a = {StandingSubject::Timer, WidgetGlobalOf(b), true}; break;
	case scripts::op::CLEAR_ONSCREEN_TIMER:    a = {StandingSubject::Timer, WidgetGlobalOf(b), false}; break;
	case scripts::op::FREEZE_ONSCREEN_TIMER:   a = {StandingSubject::Timer, 0, false}; break;
	case scripts::op::DISPLAY_ONSCREEN_COUNTER:
	case scripts::op::DISPLAY_ONSCREEN_COUNTER_WITH_STRING:
		a = {StandingSubject::Counter, WidgetGlobalOf(b), true};
		break;
	case scripts::op::CLEAR_ONSCREEN_COUNTER:  a = {StandingSubject::Counter, WidgetGlobalOf(b), false}; break;
	case 0x03BD:   // REMOVE_SPHERE
		if (replay::LiteralAt(b.code, b.length, 0, &literal))
			a = {StandingSubject::Sphere, literal, false};
		break;
	// The streets' settings, each one by what it sets: the densities by
	// themselves, a zone by its name and day or night, a gang by its number.
	case 0x03DE:   // SET_PED_DENSITY_MULTIPLIER
	case 0x01EB:   // SET_CAR_DENSITY_MULTIPLIER
	case 0x0152:   // SET_ZONE_CAR_INFO
	case 0x015C:   // SET_ZONE_PED_INFO
	case 0x0237: { // SET_GANG_WEAPONS
		uint32_t     h    = 2166136261u;
		const size_t upTo = opcode == 0x0237 ? 7u : opcode == 0x03DE || opcode == 0x01EB ? 2u : 13u;
		for (size_t i = 2; i < upTo && i < b.length; ++i) {
			h ^= b.code[i];
			h *= 16777619u;
		}
		a = {StandingSubject::Setting, static_cast<int32_t>((static_cast<uint32_t>(opcode) << 16) ^ (h & 0xFFFF)),
		     true};
		break;
	}
	// A gang's threat: on and off are one setting, so the later replaces the
	// earlier for a joiner, whichever it is.
	case 0x03F1:   // SET_THREAT_FOR_PED_TYPE
	case 0x03F2: { // CLEAR_THREAT_FOR_PED_TYPE
		uint32_t h = 2166136261u;
		for (size_t i = 2; i < 12 && i < b.length; ++i) {
			h ^= b.code[i];
			h *= 16777619u;
		}
		a = {StandingSubject::Setting, static_cast<int32_t>((0x03F1u << 16) ^ (h & 0xFFFF)), true};
		break;
	}
	default: break;
	}
	return a;
}

class StandingEffects {
public:
	// One effect that went to everybody, kept or what it takes away taken
	// away. False only when it stands and there is no room left for it.
	bool Note(const MissionEffectBody &b) {
		const StandingAbout a = AboutOf(b);
		if (a.subject == StandingSubject::None)
			return true;
		const uint16_t opcode = EffectOpcode(b);
		// The HUD has one timer and one counter: showing another replaces
		// the one up, and clearing one that is not up does nothing.
		const bool widget = a.subject == StandingSubject::Timer || a.subject == StandingSubject::Counter;
		if (a.makes) {
			Drop(a.subject, widget, a.id, ANY_OPCODE);
			return Keep(b);
		}
		const MissionEffectBody *maker = Maker(a.subject, widget, a.id);
		if (!maker)
			return true;   // about something that does not stand
		const bool takesAway =
		    opcode == scripts::op::REMOVE_BLIP || opcode == scripts::op::REMOVE_PICKUP ||
		    opcode == scripts::op::REMOVE_SCRIPT_FIRE || opcode == scripts::op::DELETE_OBJECT ||
		    opcode == scripts::op::MARK_OBJECT_AS_NO_LONGER_NEEDED || opcode == scripts::op::CLEAR_ONSCREEN_TIMER ||
		    opcode == scripts::op::CLEAR_ONSCREEN_COUNTER || opcode == 0x03BD;
		if (takesAway) {
			if (!widget || AboutOf(*maker).id == a.id)
				Drop(a.subject, widget, a.id, ANY_OPCODE);
			return true;
		}
		Drop(a.subject, widget, a.id, opcode);
		return Keep(b);
	}

	size_t                   Count() const { return m_count; }
	const MissionEffectBody &At(size_t i) const { return m_list[i]; }
	void                     Clear() { m_count = 0; }

private:
	static constexpr uint32_t ANY_OPCODE = 0x10000;

	bool Keep(const MissionEffectBody &b) {
		if (m_count == MAX_STANDING)
			return false;
		m_list[m_count++] = b;
		return true;
	}

	// What puts the subject up: the one with `id`, or with `anyId` the HUD's
	// one timer or counter whatever it shows.
	const MissionEffectBody *Maker(StandingSubject subject, bool anyId, int32_t id) const {
		for (size_t i = 0; i < m_count; ++i) {
			const StandingAbout a = AboutOf(m_list[i]);
			if (a.makes && a.subject == subject && (anyId || a.id == id))
				return &m_list[i];
		}
		return nullptr;
	}

	// Every entry about the subject, of `opcode` or of any, out; the rest
	// keep their order.
	void Drop(StandingSubject subject, bool anyId, int32_t id, uint32_t opcode) {
		size_t kept = 0;
		for (size_t i = 0; i < m_count; ++i) {
			const StandingAbout a  = AboutOf(m_list[i]);
			const bool          it = a.subject == subject && (anyId || a.id == id) &&
			                (opcode == ANY_OPCODE || EffectOpcode(m_list[i]) == opcode);
			if (!it)
				m_list[kept++] = m_list[i];
		}
		m_count = kept;
	}

	MissionEffectBody m_list[MAX_STANDING] = {};
	size_t            m_count              = 0;
};

// An object where it is now, for somebody who comes in after the mission
// moved it: SET_OBJECT_COORDINATES and SET_OBJECT_HEADING by its global, as
// replay::Encode would have written them.
inline MissionEffectBody ObjectPlaceEffect(uint16_t missionNumber, uint16_t global, float x, float y,
                                           float z) {
	MissionEffectBody b{};
	b.missionNumber = missionNumber;
	b.kind          = MISSION_EFFECT_RUN;
	b.handleAt      = 0xFF;
	b.ownerBlip     = -1;
	const uint16_t opcode = static_cast<uint16_t>(scripts::op::SET_OBJECT_COORDINATES);
	size_t         n      = 0;
	b.code[n++]           = static_cast<uint8_t>(opcode & 0xFF);
	b.code[n++]           = static_cast<uint8_t>(opcode >> 8);
	b.code[n++]           = scripts::PARAM_GLOBAL;
	b.code[n++]           = static_cast<uint8_t>(global & 0xFF);
	b.code[n++]           = static_cast<uint8_t>(global >> 8);
	const float values[3] = {x, y, z};
	for (float v : values) {
		b.code[n++] = scripts::PARAM_INT32;
		std::memcpy(b.code + n, &v, 4);
		n += 4;
	}
	b.length = static_cast<uint8_t>(n);
	return b;
}

inline MissionEffectBody ObjectHeadingEffect(uint16_t missionNumber, uint16_t global, float degrees) {
	MissionEffectBody b{};
	b.missionNumber = missionNumber;
	b.kind          = MISSION_EFFECT_RUN;
	b.handleAt      = 0xFF;
	b.ownerBlip     = -1;
	const uint16_t opcode = static_cast<uint16_t>(scripts::op::SET_OBJECT_HEADING);
	b.code[0]             = static_cast<uint8_t>(opcode & 0xFF);
	b.code[1]             = static_cast<uint8_t>(opcode >> 8);
	b.code[2]             = scripts::PARAM_GLOBAL;
	b.code[3]             = static_cast<uint8_t>(global & 0xFF);
	b.code[4]             = static_cast<uint8_t>(global >> 8);
	b.code[5]             = scripts::PARAM_INT32;
	std::memcpy(b.code + 6, &degrees, 4);
	b.length = 10;
	return b;
}

// The heading SET_OBJECT_HEADING takes for an entity whose matrix faces
// (fx, fy): CPlaceable::GetHeading's atan2(-x, y), in degrees in [0, 360).
inline float HeadingDegrees(float fx, float fy) {
	float d = std::atan2(-fx, fy) * 57.2957795f;
	if (d < 0.0f)
		d += 360.0f;
	return d >= 360.0f ? 0.0f : d;
}

// ---- more enemies, that count like the originals (missions.md 10.5) ---------------

// Whose copies the mission gets: a generic enemy, never a player, a cop, a
// medic or a fireman, and never a special character, the one of each the story
// has. Every SPECIALnn the missions make is made PEDTYPE_SPECIAL (21).
inline bool MayCopyPedType(int32_t pedType) {
	return pedType >= 4 && pedType <= 20 && pedType != 6 && pedType != 16 && pedType != 17;
}

// The script only has the original's handle, and the original's handle
// becomes a group: the original and its copies. While the owner's mission runs
// an instruction, the ped pool answers that handle with the group's
// representative, the original while it lives and then any live copy, so
// every instruction that finds out where the enemy is or what it is doing,
// IS_CHAR_DEAD included, asks the group: it is dead when every member is.
constexpr size_t MAX_ENEMY_GROUPS = 32;

class EnemyGroups {
public:
	bool Has(int32_t original) const { return Find(original) != nullptr; }

	bool IsCopy(int32_t handle) const {
		for (size_t i = 0; i < m_count; ++i)
			for (uint8_t c = 0; c < m_groups[i].count; ++c)
				if (m_groups[i].copies[c] == handle)
					return true;
		return false;
	}

	// A group for `original`, with no copies yet. False when full, or when it
	// is a member of one already.
	bool Add(int32_t original) {
		if (m_count == MAX_ENEMY_GROUPS || Has(original) || IsCopy(original))
			return false;
		m_groups[m_count++] = Group{original, {}, 0};
		return true;
	}

	bool AddCopy(int32_t original, int32_t copy) {
		Group *g = Find(original);
		if (!g || g->count == MISSION_ENEMY_COPIES_MAX || Has(copy) || IsCopy(copy))
			return false;
		g->copies[g->count++] = copy;
		return true;
	}

	// Who stands for `original` now: `alive(handle)` says whether a member
	// lives. The original when it is not a group's, or nobody lives.
	template <class Alive>
	int32_t Representative(int32_t original, Alive alive) const {
		const Group *g = Find(original);
		if (!g || alive(original))
			return original;
		for (uint8_t c = 0; c < g->count; ++c)
			if (alive(g->copies[c]))
				return g->copies[c];
		return original;
	}

	// The group's copies, at most `max`, and the group gone: the script let
	// go of the enemy, and of every copy with it.
	size_t Dissolve(int32_t original, int32_t *copies, size_t max) {
		for (size_t i = 0; i < m_count; ++i) {
			if (m_groups[i].original != original)
				continue;
			size_t n = 0;
			for (uint8_t c = 0; c < m_groups[i].count && n < max; ++c)
				copies[n++] = m_groups[i].copies[c];
			m_groups[i] = m_groups[--m_count];
			return n;
		}
		return 0;
	}

	// Every copy of every group, for the mission's end.
	size_t AllCopies(int32_t *out, size_t max) const {
		size_t n = 0;
		for (size_t i = 0; i < m_count; ++i)
			for (uint8_t c = 0; c < m_groups[i].count && n < max; ++c)
				out[n++] = m_groups[i].copies[c];
		return n;
	}

	size_t Count() const { return m_count; }
	void   Clear() { m_count = 0; }

private:
	struct Group {
		int32_t original;
		int32_t copies[MISSION_ENEMY_COPIES_MAX];
		uint8_t count;
	};

	const Group *Find(int32_t original) const {
		for (size_t i = 0; i < m_count; ++i)
			if (m_groups[i].original == original)
				return &m_groups[i];
		return nullptr;
	}
	Group *Find(int32_t original) {
		return const_cast<Group *>(static_cast<const EnemyGroups *>(this)->Find(original));
	}

	Group  m_groups[MAX_ENEMY_GROUPS] = {};
	size_t m_count                    = 0;
};

// ---- the seats the mission's passengers need (mission-audit.md R4) -----------------

// What the owner's machine says of a session car that `boarding` of its
// mission's pedestrians are heading for the seats of, with `taken` of its
// `maxPassengers` passenger seats sat in here already: kept, and left too
// when fewer are free than are needed.
inline uint8_t SeatFlagsFor(uint8_t boarding, uint8_t maxPassengers, uint8_t taken) {
	if (boarding == 0)
		return 0;
	const uint8_t free = taken >= maxPassengers ? 0 : static_cast<uint8_t>(maxPassengers - taken);
	return static_cast<uint8_t>(MISSION_SEAT_KEPT | (boarding > free ? MISSION_SEAT_LEAVE : 0));
}

// And who of the players riding in it gets out when it is left: as many as
// the seats short, from the last passenger seat forward. `riders[s]` is the
// player whose copy sits in passenger slot s here, INVALID_PLAYER for
// anybody else or nobody. Only players count: the mission's own pedestrians
// keep their seats, and a car short of seats with no player in it names
// nobody. The Paramedic's third patient, with two participants in the back of
// a three-seat ambulance, takes one of them out and not both.
inline uint8_t LeaveMaskFor(uint8_t boarding, uint8_t maxPassengers, uint8_t taken,
                            const uint8_t *riders, uint8_t slots) {
	const uint8_t free = taken >= maxPassengers ? 0 : static_cast<uint8_t>(maxPassengers - taken);
	if (boarding <= free)
		return 0;
	uint8_t need = static_cast<uint8_t>(boarding - free);
	uint8_t mask = 0;
	for (uint8_t s = slots; s-- > 0 && need != 0;)
		if (riders[s] < MAX_PLAYERS && (mask & (1u << riders[s])) == 0) {
			mask = static_cast<uint8_t>(mask | (1u << riders[s]));
			--need;
		}
	return mask;
}

// ---- everybody into the car the mission put its player in (protocol.h, C_MissionBoard)

// The passenger seats the owner's machine hands out, one to each player in
// `order` (nearest the car first) while there are free ones: `freeSeats` is
// FreeSeatMask's, bit s for wire seat s, and the first `keep` of them stay
// for the mission's pedestrians heading for the car. seats[id] is set to the
// wire seat player `id` gets and left alone for the others. How many got one.
inline uint8_t AssignBoardingSeats(uint16_t freeSeats, uint8_t keep, const uint8_t *order,
                                   size_t n, uint8_t *seats) {
	uint8_t given = 0;
	size_t  next  = 0;
	for (uint8_t s = 1; s <= 4; ++s) {
		if ((freeSeats & (1u << s)) == 0)
			continue;
		if (keep != 0) {
			--keep;
			continue;
		}
		while (next < n && order[next] >= MAX_PLAYERS)
			++next;
		if (next == n)
			break;
		seats[order[next++]] = s;
		++given;
	}
	return given;
}

// The participant's half is seatplan.h's PickBoardSeat.

// Whether the owner's checkpoints stop waiting for a participant: the owner
// is on the water and the participant has no boat to follow in, his copy
// seated in one on the owner's machine. A participant nobody here can see
// has no boat that anybody knows of either.
inline bool CheckpointOutOfReach(bool ownerInBoat, bool theyInBoat) {
	return ownerInBoat && !theyInBoat;
}

// ---- "get back in the vehicle" (the owner's run on 2026-09-24) ---------------------
//
// A mission tells its player to get back into the car it is meant to be in
// when it finds them out of it: "Hey! Get back in the vehicle!" at 53 sites
// and "Get back into the Stretch!" at 7, every one of them behind a NOT
// IS_PLAYER_IN_CAR on the mission's car. The owner's script asks about the
// owner, so what it prints is the owner's alone and goes nowhere else; each
// participant who gets out of that car is told the same, alone, by the owner's
// machine, which sees them get out.
constexpr size_t TEXT_LABEL = 8;

inline bool IsGetBackInLabel(const uint8_t *label) {
	static const char *const kLabels[] = {"IN_VEH", "FM1_1"};
	for (const char *known : kLabels) {
		size_t n = 0;
		while (known[n] != '\0' && label[n] == static_cast<uint8_t>(known[n]))
			++n;
		if (known[n] != '\0')
			continue;
		bool padded = true;
		for (size_t i = n; i < TEXT_LABEL; ++i)
			padded = padded && label[i] == 0;
		if (padded)
			return true;
	}
	return false;
}

// An encoded instruction (replay.h) that puts one of those words on the
// screen or takes it off: PRINT, PRINT_NOW, PRINT_SOON or CLEAR_THIS_PRINT,
// whose text label is the eight bytes after the opcode.
inline bool IsGetBackInPrint(const uint8_t *code, size_t length) {
	if (length < 2 + TEXT_LABEL)
		return false;
	const int32_t opcode = code[0] | (code[1] << 8);
	if (opcode != scripts::op::PRINT && opcode != scripts::op::PRINT_NOW &&
	    opcode != scripts::op::PRINT_SOON && opcode != scripts::op::CLEAR_THIS_PRINT)
		return false;
	return IsGetBackInLabel(code + 2);
}

// Which of the two a mission prints: the first PRINT_NOW of one in its
// `size` bytes (the mission slot, as START_MISSION loaded it), or IN_VEH for
// a mission that prints neither.
inline void GetBackInLabelOf(const uint8_t *mission, uint32_t size, uint8_t (&out)[TEXT_LABEL]) {
	const uint8_t opLo = static_cast<uint8_t>(scripts::op::PRINT_NOW & 0xFF);
	const uint8_t opHi = static_cast<uint8_t>(scripts::op::PRINT_NOW >> 8);
	for (uint32_t at = 0; at + 2 + TEXT_LABEL <= size; ++at)
		if (mission[at] == opLo && mission[at + 1] == opHi && IsGetBackInLabel(mission + at + 2)) {
			std::memcpy(out, mission + at + 2, TEXT_LABEL);
			return;
		}
	std::memset(out, 0, TEXT_LABEL);
	std::memcpy(out, "IN_VEH", 6);
}

// PRINT_NOW `label` for five seconds, the way the missions print it, for
// `playerId` alone.
inline MissionEffectBody GetBackInEffect(uint16_t missionNumber, const uint8_t (&label)[TEXT_LABEL],
                                         uint8_t playerId) {
	MissionEffectBody b{};
	b.missionNumber = missionNumber;
	b.kind          = MISSION_EFFECT_RUN;
	b.handleAt      = 0xFF;
	b.ownerBlip     = -1;
	b.onlyTo        = static_cast<uint8_t>(playerId + 1);
	size_t n        = 0;
	b.code[n++]     = static_cast<uint8_t>(scripts::op::PRINT_NOW & 0xFF);
	b.code[n++]     = static_cast<uint8_t>(scripts::op::PRINT_NOW >> 8);
	std::memcpy(b.code + n, label, TEXT_LABEL);
	n += TEXT_LABEL;
	for (int32_t v : {5000, 1}) {
		b.code[n++] = scripts::PARAM_INT32;
		std::memcpy(b.code + n, &v, 4);
		n += 4;
	}
	b.length = static_cast<uint8_t>(n);
	return b;
}

// Who got out of the cars the owner's mission asks IS_PLAYER_IN_CAR about.
// Only while it asks: a participant was in the car at one ask and is not at
// the next. A car it has not asked about for GET_BACK_IN_FORGET_MS is one it
// has stopped caring about, and the next ask starts afresh, so getting out
// once the mission has moved on to something else says nothing.
constexpr uint32_t GET_BACK_IN_FORGET_MS = 1500;
constexpr size_t   GET_BACK_IN_CARS      = 4;

class GetBackInWatch {
public:
	// The mission asked about the car the session calls `netId`; `inside` is
	// who of the participants sits in it now, bit i for player i. Who has
	// just got out of it.
	uint8_t Asked(uint16_t netId, uint8_t inside, uint32_t nowMs) {
		Car *car    = nullptr;
		Car *oldest = &m_cars[0];
		for (Car &c : m_cars) {
			if (c.netId == netId && c.used)
				car = &c;
			if (!c.used || (oldest->used && nowMs - c.askedMs > nowMs - oldest->askedMs))
				oldest = &c;
		}
		if (!car || nowMs - car->askedMs > GET_BACK_IN_FORGET_MS) {
			if (!car)
				car = oldest;
			*car = Car{true, netId, inside, nowMs};
			return 0;
		}
		const uint8_t out = static_cast<uint8_t>(car->inside & ~inside);
		car->inside       = inside;
		car->askedMs      = nowMs;
		return out;
	}

	void Clear() {
		for (Car &c : m_cars)
			c = Car{};
	}

private:
	struct Car {
		bool     used    = false;
		uint16_t netId   = 0;
		uint8_t  inside  = 0;
		uint32_t askedMs = 0;
	};
	Car m_cars[GET_BACK_IN_CARS];
};

// ---- the blue markers (the run on 2026-09-24) ---------------------------------------
//
// A location check whose last operand, the sphere, is set draws a blue marker
// on the ground where it looks (CTheScripts::HighlightImportantArea,
// addresses.h), and it is the check that draws it, again every frame the
// script asks. No blip, and no instruction the replay could send: only the
// owner's machine ever drew the mission's markers. Give Me Liberty's "Stop in
// the center of the blue marker" was on the screen of whoever ran the mission
// and nobody else's, whoever drove.
//
// So the owner's machine watches what its mission draws and says when a
// marker goes up, again every MARKER_RESEND_MS while it stays (which is also
// how somebody who comes in late gets it), and when it has not been drawn for
// MARKER_GONE_MS, that it came down. It goes as the instruction that draws it
// once: IS_PLAYER_IN_AREA_3D, player 0, the two corners at the marker's height
// and the sphere, every operand a literal, and the owner's id for the marker
// in ownerBlip. The sphere clear takes it down. A participant never runs it:
// its own engine draws the marker every frame until it comes down, the
// mission ends, or nothing has been heard of it for MARKER_FORGET_MS.
struct MarkerArea {
	float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f, z = 0.0f;
};

constexpr uint32_t MARKER_GONE_MS       = 400;
constexpr uint32_t MARKER_RESEND_MS     = 2000;
constexpr uint32_t MARKER_FORGET_MS     = 3 * MARKER_RESEND_MS;
// A marker that moves (one a check draws around a pedestrian) is told again
// at most this often, and only for more than MARKER_MOVE_M.
constexpr uint32_t MARKER_MOVE_MS       = 250;
constexpr float    MARKER_MOVE_M        = 0.5f;
constexpr size_t   MAX_MARKERS          = 16;
constexpr size_t   MARKER_OPERANDS      = 8;
constexpr size_t   MARKER_EFFECT_LENGTH = 2 + MARKER_OPERANDS * 5;

inline MissionEffectBody MarkerEffect(uint16_t missionNumber, uint32_t id, const MarkerArea &a,
                                      bool up) {
	MissionEffectBody b{};
	b.missionNumber = missionNumber;
	b.kind          = MISSION_EFFECT_RUN;
	b.handleAt      = 0xFF;
	b.ownerBlip     = static_cast<int32_t>(id);
	const uint16_t opcode = static_cast<uint16_t>(scripts::op::IS_PLAYER_IN_AREA_3D);
	b.code[0]             = static_cast<uint8_t>(opcode & 0xFF);
	b.code[1]             = static_cast<uint8_t>(opcode >> 8);
	int32_t values[MARKER_OPERANDS] = {0};
	const float corners[6] = {a.x1, a.y1, a.z, a.x2, a.y2, a.z};
	std::memcpy(values + 1, corners, sizeof corners);
	values[7] = up ? 1 : 0;
	size_t n  = 2;
	for (int32_t v : values) {
		b.code[n++] = scripts::PARAM_INT32;
		std::memcpy(b.code + n, &v, 4);
		n += 4;
	}
	b.length = static_cast<uint8_t>(n);
	return b;
}

// One of those, read back: false for anything else.
inline bool ReadMarkerEffect(const MissionEffectBody &b, uint32_t *id, MarkerArea *a, bool *up) {
	if (b.kind != MISSION_EFFECT_RUN || b.length != MARKER_EFFECT_LENGTH ||
	    EffectOpcode(b) != scripts::op::IS_PLAYER_IN_AREA_3D)
		return false;
	int32_t values[MARKER_OPERANDS];
	for (size_t i = 0; i < MARKER_OPERANDS; ++i) {
		if (b.code[2 + i * 5] != scripts::PARAM_INT32)
			return false;
		std::memcpy(&values[i], b.code + 3 + i * 5, 4);
	}
	if (values[0] != 0)
		return false;
	float corners[6];
	std::memcpy(corners, values + 1, sizeof corners);
	for (float f : corners)
		if (!std::isfinite(f))
			return false;
	*id = static_cast<uint32_t>(b.ownerBlip);
	*a  = MarkerArea{corners[0], corners[1], corners[3], corners[4], corners[2]};
	*up = values[7] != 0;
	return true;
}

inline bool MarkerMoved(const MarkerArea &a, const MarkerArea &b) {
	return std::fabs(a.x1 - b.x1) > MARKER_MOVE_M || std::fabs(a.y1 - b.y1) > MARKER_MOVE_M ||
	       std::fabs(a.x2 - b.x2) > MARKER_MOVE_M || std::fabs(a.y2 - b.y2) > MARKER_MOVE_M ||
	       std::fabs(a.z - b.z) > MARKER_MOVE_M;
}

// The owner's side: what its mission drew, and what everybody has been told.
class OwnMarkers {
public:
	// The mission drew `id` this frame. False when there is no room to keep it.
	bool Drawn(uint32_t id, const MarkerArea &a, uint32_t nowMs) {
		for (size_t i = 0; i < m_count; ++i)
			if (m_marks[i].id == id) {
				m_marks[i].area    = a;
				m_marks[i].drawnMs = nowMs;
				return true;
			}
		if (m_count == MAX_MARKERS)
			return false;
		m_marks[m_count++] = Mark{id, a, a, nowMs, 0, false};
		return true;
	}

	// `tell(id, area, up, first)` for what everybody has to hear now: a
	// marker that went up (`first` the first time), is still up and due
	// again, moved, or came down.
	template <class Tell>
	void Tick(uint32_t nowMs, Tell tell) {
		size_t kept = 0;
		for (size_t i = 0; i < m_count; ++i) {
			Mark &m = m_marks[i];
			if (nowMs - m.drawnMs >= MARKER_GONE_MS) {
				if (m.told)
					tell(m.id, m.area, false, false);
				continue;
			}
			const bool first = !m.told;
			if (first || nowMs - m.sentMs >= MARKER_RESEND_MS ||
			    (MarkerMoved(m.area, m.sent) && nowMs - m.sentMs >= MARKER_MOVE_MS)) {
				tell(m.id, m.area, true, first);
				m.told   = true;
				m.sent   = m.area;
				m.sentMs = nowMs;
			}
			m_marks[kept++] = m;
		}
		m_count = kept;
	}

	size_t Count() const { return m_count; }
	void   Clear() { m_count = 0; }

private:
	struct Mark {
		uint32_t   id = 0;
		MarkerArea area, sent;
		uint32_t   drawnMs = 0, sentMs = 0;
		bool       told    = false;
	};
	Mark   m_marks[MAX_MARKERS];
	size_t m_count = 0;
};

// A participant's side: the owner's markers this machine draws.
class ShownMarkers {
public:
	// One heard. Up: kept, or moved if it is kept already. Down: gone. False
	// only for one that went up with no room left for it.
	bool Heard(uint32_t id, const MarkerArea &a, bool up, uint32_t nowMs) {
		for (size_t i = 0; i < m_count; ++i) {
			if (m_marks[i].id != id)
				continue;
			if (up)
				m_marks[i] = Mark{id, a, nowMs};
			else
				m_marks[i] = m_marks[--m_count];
			return true;
		}
		if (!up)
			return true;
		if (m_count == MAX_MARKERS)
			return false;
		m_marks[m_count++] = Mark{id, a, nowMs};
		return true;
	}

	bool Has(uint32_t id) const {
		for (size_t i = 0; i < m_count; ++i)
			if (m_marks[i].id == id)
				return true;
		return false;
	}

	// `draw(id, area)` for each still up; one not heard of for
	// MARKER_FORGET_MS is let go of first.
	template <class Draw>
	void Each(uint32_t nowMs, Draw draw) {
		size_t kept = 0;
		for (size_t i = 0; i < m_count; ++i) {
			if (nowMs - m_marks[i].heardMs >= MARKER_FORGET_MS)
				continue;
			m_marks[kept++] = m_marks[i];
			draw(m_marks[i].id, m_marks[i].area);
		}
		m_count = kept;
	}

	size_t Count() const { return m_count; }
	void   Clear() { m_count = 0; }

private:
	struct Mark {
		uint32_t   id = 0;
		MarkerArea area;
		uint32_t   heardMs = 0;
	};
	Mark   m_marks[MAX_MARKERS];
	size_t m_count = 0;
};

// ---- the engine half ---------------------------------------------------------------

// Every detour and the checks behind them, when CoopIII.ini says so. False,
// with nothing installed, when it does not or when a range handler does not
// look like one (missionaddr.h); the log says which.
bool InstallMissionHooks(bool enabled, Client &client);
void RemoveMissionHooks();

// MissionBridge's entries.
void AddMissionsToBridge(WorldBridge &bridge);

// While the session's mission, on this machine that owns it, runs one of its
// instructions: what the engine puts into the world then is the mission's,
// and population.cpp hosts it for everybody (docs/missions.md 5.3).
bool MissionMakingEntities();
// The session's mission runs here, as its owner.
bool OwnMissionRunning();
// While it does: `entity` is the replica of a player in it, or the car one
// sits in. What the engine credits to it is a participant's doing.
bool MissionParticipantEntity(const void *entity);

// Somebody else's hit killed a pedestrian this machine hosts: whether the
// engine's own kill register takes it as the player's here, as a rampage's
// does, because the session's mission runs with this machine in it
// (mission-audit.md R11, game/darkel.cpp's CreditRemotePedKill).
bool MissionCreditsRemoteKills();
// CDarkel::RegisterKillByPlayer counted `victim` here: a participant's machine
// hands it to the owner's mission (protocol.h, C_MissionKill).
void MissionKillRegistered(void *victim);

// A pickup at (x, y) of `model` is one of the session's mission's stash,
// which every player takes their own of (protocol.h, PICKUP_F_STASH).
bool MissionStashAt(float x, float y, int16_t model);

// CPed::ScanForThreats' answer for `ped`, as the engine found it, on this
// machine: for a pedestrian the session's mission made here that fears the
// player (SET_CHAR_THREAT_SEARCH's THREAT_PLAYER1), a participant it can see
// is the player too, and the nearer one is the threat (game/cheats.cpp's
// detour asks).
uint32_t MissionThreat(void *ped, uint32_t found);

// Once a frame, before CGame::Process: a participant's $ONMISSION.
void TickMissions();

// From the CHud::Draw detour: who a start or a checkpoint of the session's
// mission waits for, in the HUD's corner (MissionSync::WaitLine).
void DrawMissionWait();

// Once a frame, after CGame::Process and before the frame is drawn: the
// owner's blue markers, on a participant's screen.
void DrawMissionMarkers();

} // namespace coopiii::game
