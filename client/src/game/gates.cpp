#include "gates.h"

#include "addresses.h"
#include "../clock.h"
#include "../log.h"

#include <cmath>
#include <cstring>

namespace coopiii::game {

namespace {

using CollectFn = void(__thiscall *)(void *script, uint32_t *ip, int16_t count);
using CompareFn = void(__thiscall *)(void *script, bool value);
using OneFn     = int8_t(__thiscall *)(void *script);
using GetObjFn  = void *(__cdecl *)(int32_t handle);
using PlayerFn  = void *(__cdecl *)();

constexpr uint16_t OP_SLIDE_OBJECT  = 0x034E;
constexpr uint16_t OP_ROTATE_OBJECT = 0x034D;

struct GateState {
	uint32_t lastOpenMs  = 0;   // the local thread asked for open
	uint32_t lastShutMs  = 0;   // ...for shut
	uint32_t lastLocalMs = 0;   // either
	bool     driven      = false;
};

GateState g_gates[GATE_COUNT];
uint16_t  g_held      = 0;   // everybody else's union
uint16_t  g_global[GATE_COUNT] = {};
bool      g_scanned   = false;
bool      g_saidHold  = false;
bool      g_saidLocal = false;
bool      g_saidSwallow = false;

// A safehouse door (gates.h, HideoutDoor): the same bookkeeping as one gate,
// the global init.sc keeps each half in, and when a mission script last moved
// it.
struct DoorState {
	GateState g;
	uint16_t  global[2]   = {};
	uint32_t  missionMs   = 0;
	bool      saidHold    = false;
	bool      saidLocal   = false;
	bool      saidSwallow = false;
	bool      saidMission = false;
};

DoorState g_doors[HIDEOUT_DOOR_COUNT];
// Our own move of a door is running, so GateSlide and GateRotate let it
// through untouched.
bool      g_driving = false;

// A script of CoopIII's, run once over a single instruction, the way
// game/mission.cpp runs the owner's: the engine then does exactly what it
// does for its own scripts. Named like mission.cpp's so either module can
// tell its own apart from main.scm's threads.
alignas(16) uint8_t g_runner[0x100];
uint8_t             g_runnerCode[48];

bool IsOurRunner(const void *script) {
	return std::memcmp(static_cast<const uint8_t *>(script) + offs::SCRIPT_NAME, "coopiii", 8) == 0;
}

void PutFloat(uint8_t *&at, float value) {
	const int16_t fixed = static_cast<int16_t>(value * 16.0f);
	*at++ = SCRIPT_PARAM_FLOAT;
	std::memcpy(at, &fixed, 2);
	at += 2;
}

void PutGlobal(uint8_t *&at, uint16_t op, uint16_t global) {
	*at++ = static_cast<uint8_t>(op & 0xFF);
	*at++ = static_cast<uint8_t>(op >> 8);
	*at++ = SCRIPT_PARAM_GLOBAL;
	std::memcpy(at, &global, 2);
	at += 2;
}

void RunOurRunner() {
	std::memset(g_runner, 0, sizeof g_runner);
	std::memcpy(g_runner + offs::SCRIPT_NAME, "coopiii", 8);
	Field<uint32_t>(g_runner, offs::SCRIPT_IP) =
	    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_runnerCode) - CTheScripts__ScriptSpace);
	Func<OneFn>(CRunningScript__ProcessCommands)(g_runner);
}

// SLIDE_OBJECT <global> to `where` at `speed`, and whether to stop for
// something in the way - on our own script.
void SlideObject(uint16_t global, const float *where, const float *speed, bool collide) {
	uint8_t *at = g_runnerCode;
	PutGlobal(at, OP_SLIDE_OBJECT, global);
	for (int k = 0; k < 3; ++k)
		PutFloat(at, where[k]);
	for (int k = 0; k < 3; ++k)
		PutFloat(at, speed[k]);
	*at++ = SCRIPT_PARAM_INT8;
	*at++ = collide ? 1 : 0;
	RunOurRunner();
}

// A gate, gates.sc's speed.
void Slide(size_t i, const float *where, bool collide) {
	SlideObject(g_global[i], where, Gate(i).speed, collide);
}

// ROTATE_OBJECT <global> to `angle` at `step`, no collision check, as
// save.sc's own - on our own script.
void RotateObject(uint16_t global, float angle, float step) {
	uint8_t *at = g_runnerCode;
	PutGlobal(at, OP_ROTATE_OBJECT, global);
	PutFloat(at, angle);
	PutFloat(at, step);
	*at++ = SCRIPT_PARAM_INT8;
	*at++ = 0;
	RunOurRunner();
}

void *ObjectOfGlobal(uint16_t global) {
	if (global == 0)
		return nullptr;
	return Func<GetObjFn>(CPools__GetObject)(Global<int32_t>(CTheScripts__ScriptSpace + global));
}

// The handler's own heading (0x004498BF): Atan2(-forward.x, forward.y) in
// degrees, 0..360.
float DoorHeading(void *object) {
	const float fx = Field<float>(object, offs::MATRIX_FWD + 0);
	const float fy = Field<float>(object, offs::MATRIX_FWD + 4);
	float       h  = std::atan2(-fx, fy) * (180.0f / 3.14159265358979323846f);
	if (h < 0.0f)
		h += 360.0f;
	return h;
}

void *GateObject(size_t i) { return ObjectOfGlobal(g_global[i]); }

bool At(void *object, const float *p) {
	const float *pos = &Field<float>(object, offs::POSITION);
	const float dx = pos[0] - p[0], dy = pos[1] - p[1], dz = pos[2] - p[2];
	return dx * dx + dy * dy + dz * dz < 0.001f;
}

bool HaveWorld() { return Func<PlayerFn>(FindPlayerPed)() != nullptr; }

// Once there is a world: which global holds which gate, and which door.
void ScanOnce() {
	if (g_scanned)
		return;
	g_scanned     = true;
	size_t found  = 0;
	for (size_t i = 0; i < GATE_COUNT; ++i) {
		g_global[i] = FindGateGlobal(Ptr<uint8_t>(CTheScripts__ScriptSpace), SIZE_MAIN_SCRIPT, Gate(i));
		found += g_global[i] != 0 ? 1 : 0;
	}
	Log("gates: %u of %u gates found in main.scm by where init.sc puts them",
	    static_cast<unsigned>(found), static_cast<unsigned>(GATE_COUNT));

	for (size_t d = 0; d < HIDEOUT_DOOR_COUNT; ++d) {
		const HideoutDoorDef &def  = HideoutDoor(d);
		DoorState            &door = g_doors[d];
		bool                  all  = true;
		for (size_t h = 0; h < def.halves; ++h) {
			const GateDef half{def.name,
			                   {def.at[h][0], def.at[h][1], def.at[h][2]},
			                   {def.at[h][0], def.at[h][1], def.at[h][2]},
			                   {0.0f, 0.0f, 0.0f}};
			door.global[h] =
			    FindGateGlobal(Ptr<uint8_t>(CTheScripts__ScriptSpace), SIZE_MAIN_SCRIPT, half);
			all = all && door.global[h] != 0;
		}
		if (!all) {
			// Half a door is no door: neither half is looked after.
			door.global[0] = door.global[1] = 0;
			Log("gates: the %s safehouse door is not in main.scm where init.sc puts it; "
			    "it stays each player's own",
			    def.name);
			continue;
		}
		if (def.halves == 2)
			Log("gates: the %s safehouse door is main.scm's globals at %u and %u", def.name,
			    static_cast<unsigned>(door.global[0]), static_cast<unsigned>(door.global[1]));
		else
			Log("gates: the %s safehouse door is main.scm's global at %u", def.name,
			    static_cast<unsigned>(door.global[0]));
	}
}

// Which door's half a script handle is; -1 for none.
int DoorOfHandle(int32_t handle, size_t *half, bool slides) {
	for (size_t d = 0; d < HIDEOUT_DOOR_COUNT; ++d) {
		const HideoutDoorDef &def = HideoutDoor(d);
		if (def.slides != slides)
			continue;
		for (size_t h = 0; h < def.halves; ++h) {
			const uint16_t global = g_doors[d].global[h];
			if (global != 0 && handle == Global<int32_t>(CTheScripts__ScriptSpace + global)) {
				if (half)
					*half = h;
				return static_cast<int>(d);
			}
		}
	}
	return -1;
}

// ---- the bridge -------------------------------------------------------------

bool SampleLocalGatesImpl(uint16_t &mask) {
	if (!HaveWorld())
		return false;
	const uint32_t now = WallClock::NowMs();
	mask               = 0;
	for (size_t i = 0; i < GATE_COUNT; ++i)
		if (GateWantedOpen(g_gates[i].lastOpenMs, g_gates[i].lastShutMs, now))
			mask = static_cast<uint16_t>(mask | (1u << i));
	for (size_t d = 0; d < HIDEOUT_DOOR_COUNT; ++d)
		if (GateWantedOpen(g_doors[d].g.lastOpenMs, g_doors[d].g.lastShutMs, now))
			mask = static_cast<uint16_t>(mask | HideoutDoor(d).bit);
	return true;
}

// Whether every half of a door is at its open (or shut) end.
bool DoorAt(size_t d, bool open) {
	const HideoutDoorDef &def = HideoutDoor(d);
	for (size_t h = 0; h < def.halves; ++h) {
		void *const object = ObjectOfGlobal(g_doors[d].global[h]);
		if (!object)
			return false;
		const bool there = def.slides
		                       ? At(object, open ? def.open[h] : def.at[h])
		                       : DoorHeadingAt(DoorHeading(object),
		                                       open ? def.openHeading : def.shutHeading);
		if (!there)
			return false;
	}
	return true;
}

// Moves every half of a door one step toward its open (or shut) end.
void MoveDoor(size_t d, bool open) {
	const HideoutDoorDef &def = HideoutDoor(d);
	g_driving                 = true;
	for (size_t h = 0; h < def.halves; ++h) {
		if (def.slides)
			SlideObject(g_doors[d].global[h], open ? def.open[h] : def.at[h], def.speed, false);
		else
			RotateObject(g_doors[d].global[h], open ? def.openHeading : def.shutHeading,
			             def.step);
	}
	g_driving = false;
}

// A door's half of ApplyRemoteGatesImpl.
void ApplyRemoteDoor(size_t d, bool held, uint32_t now) {
	DoorState &door   = g_doors[d];
	GateState &g      = door.g;
	const bool local  = GateWantedOpen(g.lastOpenMs, g.lastShutMs, now);
	const bool active = g.lastLocalMs != 0 && now - g.lastLocalMs < kGateLocalActiveMs;
	const GateDrive drive = DecideGateDrive(held, local, g.driven, active);
	if (drive == GateDrive::None)
		return;
	// A mission is moving it. Neither way, until it has stopped.
	if (DoorLeftToMission(door.missionMs, now))
		return;
	for (size_t h = 0; h < HideoutDoor(d).halves; ++h)
		if (!ObjectOfGlobal(door.global[h]))
			return;   // not made yet, or not in main.scm
	if (drive == GateDrive::Open) {
		g.driven = true;
		if (!DoorAt(d, true))
			MoveDoor(d, true);
		if (!door.saidHold) {
			door.saidHold = true;
			Log("gates: holding the %s safehouse door open for another player",
			    HideoutDoor(d).name);
		}
	} else {
		MoveDoor(d, false);
		if (DoorAt(d, false))
			g.driven = false;
	}
}

void ApplyRemoteGatesImpl(uint16_t mask) {
	g_held = mask;
	if (!HaveWorld())
		return;
	if (mask != 0)
		ScanOnce();
	if (!g_scanned)
		return;

	const uint32_t now = WallClock::NowMs();
	for (size_t i = 0; i < GATE_COUNT; ++i) {
		GateState &g      = g_gates[i];
		const bool held   = (mask & (1u << i)) != 0;
		const bool local  = GateWantedOpen(g.lastOpenMs, g.lastShutMs, now);
		const bool active = g.lastLocalMs != 0 && now - g.lastLocalMs < kGateLocalActiveMs;
		const GateDrive d = DecideGateDrive(held, local, g.driven, active);
		if (d == GateDrive::None)
			continue;
		void *const object = GateObject(i);
		if (!object)
			continue;   // not made yet, or the save took it away
		if (d == GateDrive::Open) {
			g.driven = true;
			if (!At(object, Gate(i).open))
				Slide(i, Gate(i).open, false);
			if (!g_saidHold) {
				g_saidHold = true;
				Log("gates: holding the %s gate open for another player", Gate(i).name);
			}
		} else {
			// The script's own close stops for whatever is in the way, and so
			// does this one: a car under it keeps it up another frame.
			Slide(i, Gate(i).shut, true);
			if (At(object, Gate(i).shut))
				g.driven = false;
		}
	}
	for (size_t d = 0; d < HIDEOUT_DOOR_COUNT; ++d)
		ApplyRemoteDoor(d, (mask & HideoutDoor(d).bit) != 0, now);
}

// A save thread's move of door `d` toward its open end (`open` 1) or its
// shut end (0), read off the script at `script` whose parameters end at
// `ip`. True when it was answered here.
bool DoorMove(size_t d, void *script, uint32_t ip, int open, int8_t *result) {
	DoorState     &door = g_doors[d];
	GateState     &g    = door.g;
	const uint32_t now  = WallClock::NowMs() | 1u;
	g.lastLocalMs       = now;
	g.driven            = false;   // the local thread is looking after it now
	if (open == 1) {
		g.lastOpenMs = now;
		if (!door.saidLocal) {
			door.saidLocal = true;
			Log("gates: our own save thread opened the %s safehouse door; telling the "
			    "session",
			    HideoutDoor(d).name);
		}
	} else {
		g.lastShutMs = now;
	}

	if (!SwallowGateSlide((g_held & HideoutDoor(d).bit) != 0, open == 1))
		return false;

	// Somebody else has it open: the thread's loop ends as it would have on
	// arrival, and the door stays where it is.
	Field<uint32_t>(script, offs::SCRIPT_IP) = ip;
	Func<CompareFn>(CRunningScript__UpdateCompareFlag)(script, true);
	if (!door.saidSwallow) {
		door.saidSwallow = true;
		Log("gates: our save thread wanted the %s safehouse door shut and another player "
		    "has it open; it stays open",
		    HideoutDoor(d).name);
	}
	*result = 0;
	return true;
}

// A mission's own move of a door, here or replayed on a runner of ours.
void DoorMovedByMission(size_t d) {
	g_doors[d].missionMs = WallClock::NowMs() | 1u;
	if (!g_doors[d].saidMission) {
		g_doors[d].saidMission = true;
		Log("gates: a mission is moving the %s safehouse door; it is left to the mission",
		    HideoutDoor(d).name);
	}
}

bool HaveAnyDoor() {
	for (const DoorState &door : g_doors)
		if (door.global[0] != 0)
			return true;
	return false;
}

}  // namespace

bool GateSlide(void *script, int8_t *result) {
	if (!script || g_driving)
		return false;
	const bool mission =
	    Field<uint8_t>(script, offs::SCRIPT_IS_MISSION) != 0 || IsOurRunner(script);
	if (!g_scanned && HaveWorld())
		ScanOnce();
	if (mission && !HaveAnyDoor())
		return false;

	// Read without moving: CollectParameters on a copy of the ip.
	uint32_t ip = Field<uint32_t>(script, offs::SCRIPT_IP);
	Func<CollectFn>(CTheScripts__CollectParameters)(script, &ip, 8);
	const float *params = Ptr<float>(CTheScripts__ScriptParams);

	// A half of Staunton's door, by whose object it slides.
	size_t    half = 0;
	const int door = DoorOfHandle(Ptr<int32_t>(CTheScripts__ScriptParams)[0], &half, true);
	if (door >= 0) {
		if (mission) {
			DoorMovedByMission(static_cast<size_t>(door));
			return false;
		}
		const int open = DoorSlideTargetOpen(HideoutDoor(static_cast<size_t>(door)), half,
		                                     params[1], params[2], params[3]);
		if (open < 0)
			return false;
		return DoorMove(static_cast<size_t>(door), script, ip, open, result);
	}
	if (mission)
		return false;

	bool       toOpen   = false;
	const int  gate     = GateForTarget(params[1], params[2], params[3], &toOpen);
	if (gate < 0)
		return false;

	GateState     &g   = g_gates[gate];
	const uint32_t now = WallClock::NowMs() | 1u;
	g.lastLocalMs      = now;
	g.driven           = false;   // the local thread is looking after it now
	if (toOpen) {
		g.lastOpenMs = now;
		if (!g_saidLocal) {
			g_saidLocal = true;
			Log("gates: our own GATES thread opened the %s gate; telling the session",
			    Gate(gate).name);
		}
	} else {
		g.lastShutMs = now;
	}

	if (!SwallowGateSlide((g_held & (1u << gate)) != 0, toOpen))
		return false;

	// Somebody else has it open. The thread's close loop ends as it would
	// have on arrival - it then finds the gate is not where it asked and so
	// plays no clunk - and the gate stays up.
	Field<uint32_t>(script, offs::SCRIPT_IP) = ip;
	Func<CompareFn>(CRunningScript__UpdateCompareFlag)(script, true);
	if (!g_saidSwallow) {
		g_saidSwallow = true;
		Log("gates: our GATES thread wanted the %s gate shut and another player has it "
		    "open; it stays open",
		    Gate(gate).name);
	}
	*result = 0;
	return true;
}

bool GateRotate(void *script, int8_t *result) {
	if (!script || g_driving)
		return false;
	if (!g_scanned) {
		if (!HaveWorld())
			return false;
		ScanOnce();
	}
	if (!HaveAnyDoor())
		return false;

	// Read without moving, as GateSlide does. Whose object it turns first:
	// anything but a door is none of this module's business.
	uint32_t ip = Field<uint32_t>(script, offs::SCRIPT_IP);
	Func<CollectFn>(CTheScripts__CollectParameters)(script, &ip, 4);
	const int door = DoorOfHandle(Ptr<int32_t>(CTheScripts__ScriptParams)[0], nullptr, false);
	if (door < 0)
		return false;

	if (Field<uint8_t>(script, offs::SCRIPT_IS_MISSION) != 0 || IsOurRunner(script)) {
		DoorMovedByMission(static_cast<size_t>(door));
		return false;
	}

	const int open = DoorTargetOpen(HideoutDoor(static_cast<size_t>(door)),
	                                Ptr<float>(CTheScripts__ScriptParams)[1]);
	if (open < 0)
		return false;
	return DoorMove(static_cast<size_t>(door), script, ip, open, result);
}

void AddGatesToBridge(WorldBridge &bridge) {
	for (GateState &g : g_gates)
		g = GateState{};
	for (DoorState &door : g_doors)
		door = DoorState{};
	g_held    = 0;
	g_scanned = false;
	bridge.SampleLocalGates = &SampleLocalGatesImpl;
	bridge.ApplyRemoteGates = &ApplyRemoteGatesImpl;
}

} // namespace coopiii::game
