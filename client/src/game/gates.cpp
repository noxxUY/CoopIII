#include "gates.h"

#include "addresses.h"
#include "../clock.h"
#include "../log.h"

#include <cstring>

namespace coopiii::game {

namespace {

using CollectFn = void(__thiscall *)(void *script, uint32_t *ip, int16_t count);
using CompareFn = void(__thiscall *)(void *script, bool value);
using OneFn     = int8_t(__thiscall *)(void *script);
using GetObjFn  = void *(__cdecl *)(int32_t handle);
using PlayerFn  = void *(__cdecl *)();

constexpr uint16_t OP_SLIDE_OBJECT = 0x034E;

struct GateState {
	uint32_t lastOpenMs  = 0;   // the local thread asked for open
	uint32_t lastShutMs  = 0;   // ...for shut
	uint32_t lastLocalMs = 0;   // either
	bool     driven      = false;
};

GateState g_gates[GATE_COUNT];
uint8_t   g_held      = 0;   // everybody else's union
uint16_t  g_global[GATE_COUNT] = {};
bool      g_scanned   = false;
bool      g_saidHold  = false;
bool      g_saidLocal = false;
bool      g_saidSwallow = false;

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

// SLIDE_OBJECT <the gate's global> to `where`, gates.sc's speed, and whether
// to stop for something in the way - on our own script. False when there is
// nothing to run it on.
bool Slide(size_t i, const float *where, bool collide) {
	uint8_t *at = g_runnerCode;
	*at++       = static_cast<uint8_t>(OP_SLIDE_OBJECT & 0xFF);
	*at++       = static_cast<uint8_t>(OP_SLIDE_OBJECT >> 8);
	*at++       = SCRIPT_PARAM_GLOBAL;
	std::memcpy(at, &g_global[i], 2);
	at += 2;
	for (int k = 0; k < 3; ++k)
		PutFloat(at, where[k]);
	for (int k = 0; k < 3; ++k)
		PutFloat(at, Gate(i).speed[k]);
	*at++ = SCRIPT_PARAM_INT8;
	*at++ = collide ? 1 : 0;

	std::memset(g_runner, 0, sizeof g_runner);
	std::memcpy(g_runner + offs::SCRIPT_NAME, "coopiii", 8);
	Field<uint32_t>(g_runner, offs::SCRIPT_IP) =
	    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_runnerCode) - CTheScripts__ScriptSpace);
	Func<OneFn>(CRunningScript__ProcessCommands)(g_runner);
	return true;
}

void *GateObject(size_t i) {
	if (g_global[i] == 0)
		return nullptr;
	const int32_t handle = Global<int32_t>(CTheScripts__ScriptSpace + g_global[i]);
	return Func<GetObjFn>(CPools__GetObject)(handle);
}

bool At(void *object, const float *p) {
	const float *pos = &Field<float>(object, offs::POSITION);
	const float dx = pos[0] - p[0], dy = pos[1] - p[1], dz = pos[2] - p[2];
	return dx * dx + dy * dy + dz * dz < 0.001f;
}

bool HaveWorld() { return Func<PlayerFn>(FindPlayerPed)() != nullptr; }

// Once there is a world: which global holds which gate.
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
}

// ---- the bridge -------------------------------------------------------------

bool SampleLocalGatesImpl(uint8_t &mask) {
	if (!HaveWorld())
		return false;
	const uint32_t now = WallClock::NowMs();
	mask               = 0;
	for (size_t i = 0; i < GATE_COUNT; ++i)
		if (GateWantedOpen(g_gates[i].lastOpenMs, g_gates[i].lastShutMs, now))
			mask = static_cast<uint8_t>(mask | (1u << i));
	return true;
}

void ApplyRemoteGatesImpl(uint8_t mask) {
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
}

}  // namespace

bool GateSlide(void *script, int8_t *result) {
	if (!script || Field<uint8_t>(script, offs::SCRIPT_IS_MISSION) != 0 || IsOurRunner(script))
		return false;

	// Read without moving: CollectParameters on a copy of the ip.
	uint32_t ip = Field<uint32_t>(script, offs::SCRIPT_IP);
	Func<CollectFn>(CTheScripts__CollectParameters)(script, &ip, 8);
	const float *params = Ptr<float>(CTheScripts__ScriptParams);
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

void AddGatesToBridge(WorldBridge &bridge) {
	for (GateState &g : g_gates)
		g = GateState{};
	g_held    = 0;
	g_scanned = false;
	bridge.SampleLocalGates = &SampleLocalGatesImpl;
	bridge.ApplyRemoteGates = &ApplyRemoteGatesImpl;
}

} // namespace coopiii::game
