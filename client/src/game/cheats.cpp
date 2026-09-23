#include "cheats.h"

#include "ped.h"
#include "population.h"
#include "hook/hook.h"
#include "log.h"

#include <cstring>

namespace coopiii::game {

namespace {

Detour g_cheatString;
Detour g_scanForThreats;

// What Client last told us. Written from PreFrame and on a welcome, read from
// inside the key handler - both on the game thread, which is the one the
// window procedure runs on in this game (addresses.h, "cheats").
CheatContext g_ctx;

// The cheats typed here that have to go somewhere. Nobody types eight cheats
// between two frames; the bound is there so a stuck key cannot grow it.
constexpr uint8_t MAX_PENDING_CHEATS = 8;
CheatBody         g_pending[MAX_PENDING_CHEATS];
uint8_t           g_pendingCount = 0;

bool g_saidReplicaThreat = false;
bool g_saidPassengerHeal = false;

using HandlerFn    = void(__cdecl *)();
using CheatStrFn   = void(__thiscall *)(void *, char);
using ScanFn       = uint32_t(__thiscall *)(void *);
using FindPlayerFn = void *(__cdecl *)();

bool WorldIsUp() { return Global<uint32_t>(gGameState) == GS_PLAYING_GAME; }

// The name a player types, for the log. The table holds it backwards, the
// way the engine compares it.
const char *TypedName(uint8_t id) {
	static char names[CHEAT_COUNT][KEYBOARD_CHEAT_STRING_LEN + 1];
	if (id >= CHEAT_COUNT)
		return "?";
	char *name = names[id];
	if (name[0] == '\0') {
		// strlen, not the pushed length: BOOOOORING's is longer than it is.
		const char  *reversed = CHEAT_SITES[id].reversed;
		const size_t len      = std::strlen(reversed);
		for (size_t i = 0; i < len; ++i)
			name[i] = reversed[len - 1 - i];
		name[len] = '\0';
	}
	return name;
}

void RunHandler(uint8_t id) { Func<HandlerFn>(CHEAT_SITES[id].handler)(); }

EngineCheatState ReadEngine() {
	EngineCheatState s;
	s.timeScale       = Global<float>(CTimer__ms_fTimeScale);
	s.givePedsWeapons = Global<uint8_t>(CPopulation__ms_bGivePedsWeapons) != 0;
	s.fastTime        = Global<uint8_t>(gbFastTime) != 0;
	return s;
}

void Queue(uint8_t id, uint8_t state) {
	if (g_pendingCount == MAX_PENDING_CHEATS) {
		Log("cheats: %s did not go out - %u cheats are already waiting",
		    TypedName(id), static_cast<unsigned>(MAX_PENDING_CHEATS));
		return;
	}
	g_pending[g_pendingCount].cheat = id;
	g_pending[g_pendingCount].state = state;
	++g_pendingCount;
}

// GESUNDHEIT, with the car half kept to the driver (HealthCheatMayRepairCar).
// The handler still runs - the help text and the player's health are the
// engine's - and the two fields it would have written on a car we are only
// riding in are put back straight after. Nothing runs in between: it is the
// same thread, inside one call.
void RunHealthCheat() {
	void *const ped     = Func<FindPlayerFn>(FindPlayerPed)();
	void *const vehicle = Func<FindPlayerFn>(FindPlayerVehicle)();
	const bool  weDrive =
	    vehicle && ped && Field<void *>(vehicle, offs::VEH_DRIVER) == ped;

	if (HealthCheatMayRepairCar(g_ctx.inSession, vehicle != nullptr, weDrive)) {
		RunHandler(CHEAT_HEALTH);
		return;
	}

	const size_t engineField = offs::AUTO_DAMAGE_MANAGER + offs::DMG_ENGINE_STATUS;
	const bool   isCar  = Field<int32_t>(vehicle, offs::VEH_TYPE) == VEHICLE_TYPE_CAR;
	const float  health = Field<float>(vehicle, offs::VEH_HEALTH);
	const uint8_t engine = isCar ? Field<uint8_t>(vehicle, engineField) : 0;

	RunHandler(CHEAT_HEALTH);

	Field<float>(vehicle, offs::VEH_HEALTH) = health;
	if (isCar)
		Field<uint8_t>(vehicle, engineField) = engine;

	if (!g_saidPassengerHeal) {
		g_saidPassengerHeal = true;
		Log("cheats: GESUNDHEIT healed us and left the car alone - we are a "
		    "passenger, and the driver's machine decides what shape it is in");
	}
}

void RunTyped(uint8_t id) {
	if (id == CHEAT_HEALTH)
		RunHealthCheat();
	else
		RunHandler(id);
}

void Dispatch(uint8_t id) {
	switch (PlanTypedCheat(g_ctx, id)) {
	case CheatVerdict::Vanilla:
	case CheatVerdict::RunHere:
		RunTyped(id);
		if (id == CHEAT_BLOW_UP_CARS)
			Log("cheats: BANGBANGBANG - every car somebody else owns is refused "
			    "by the BlowUpCar detour; the parked cars and our own traffic "
			    "that it wrecked are on their way to everybody");
		else
			Log("cheats: %s, here only", TypedName(id));
		return;

	case CheatVerdict::Refused:
		Log("cheats: %s refused - %s", TypedName(id), CheatRefusalReason(g_ctx.rule));
		return;

	case CheatVerdict::SendToHost:
		Queue(id, 0);
		Log("cheats: %s goes to the host; our sky follows theirs", TypedName(id));
		return;

	case CheatVerdict::RunHereAndSend: {
		RunTyped(id);
		uint8_t state = 0;
		if (!CurrentCheatState(id, ReadEngine(), state)) {
			// Only the time scale can fail this, and only when something other
			// than the two cheats set it - a mission's slow motion. Sending a
			// state we cannot name would move everybody else somewhere else.
			Log("cheats: %s ran here but the time scale is %.3f, which no cheat "
			    "makes; not sent", TypedName(id),
			    static_cast<double>(Global<float>(CTimer__ms_fTimeScale)));
			return;
		}
		Queue(id, state);
		Log("cheats: %s, here and for everybody (state %u)", TypedName(id),
		    static_cast<unsigned>(state));
		return;
	}
	}
}

// __thiscall void CPad::AddToPCCheatString(char c).  ret 4.
//
// Outside a session the engine's own function runs and nothing here is
// involved. Inside one, the same shift and the same 23 compares happen here,
// in the same order, into the same buffer - and each match goes through
// Dispatch instead of straight to its handler. The handlers are the engine's
// either way.
void __fastcall HookedAddToPCCheatString(void *pad, void * /*edx*/, char c) {
	if (!g_ctx.inSession) {
		g_cheatString.Original<CheatStrFn>()(pad, c);
		return;
	}

	char *const buffer = Ptr<char>(CPad__KeyBoardCheatString);
	PushCheatChar(buffer, c);

	uint8_t       ids[CHEAT_COUNT];
	const uint8_t n = MatchTypedCheats(buffer, ids, CHEAT_COUNT);
	for (uint8_t i = 0; i < n; ++i)
		Dispatch(ids[i]);
}

// __thiscall uint32 CPed::ScanForThreats().  ret.
uint32_t __fastcall HookedScanForThreats(void *ped, void * /*edx*/) {
	uint16_t   netId   = INVALID_NETID;
	const bool remote  = RemotePlayerForPed(ped, netId);
	const bool replica = !remote && AmbientReplicaForPed(ped, netId);
	if (!MayScanForThreats(remote, replica)) {
		if (!g_saidReplicaThreat && Field<uint32_t>(ped, offs::PED_FEAR_FLAGS) != 0) {
			g_saidReplicaThreat = true;
			Log("cheats: %s %u fears 0x%08X and was not allowed to act on it - "
			    "its owner's machine decides what it does (said once)",
			    remote ? "remote player" : "replica pedestrian",
			    static_cast<unsigned>(netId),
			    Field<uint32_t>(ped, offs::PED_FEAR_FLAGS));
		}
		return 0;
	}
	return g_scanForThreats.Original<ScanFn>()(ped);
}

uint8_t LiveByte(const void * /*ctx*/, uint32_t va) { return *Ptr<uint8_t>(va); }

// The table in addresses.h against the code that is actually running. The
// exe passed its MD5 check before anything was hooked, but what runs is the
// exe plus whatever every other plugin has patched into it since
// (docs/compat.md), and CoopIII is about to replace this function in a
// session. So it only does that while the live code is, row for row, the code
// the table was read from: a length, a string or a handler that differs means
// the function no longer does what CoopIII would be imitating.
bool TableMatchesImage() {
	if (std::memcmp(Ptr<uint8_t>(CPad__AddToPCCheatString),
	                CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE,
	                sizeof(CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE)) != 0) {
		Log("cheats: CPad::AddToPCCheatString does not start the way the retail "
		    "one does - another plugin has hooked it");
		return false;
	}
	uint8_t bad = 0;
	if (!CheatRowsMatchTable(&LiveByte, nullptr, &bad)) {
		if (bad < CHEAT_COUNT)
			Log("cheats: row %u (%s) of CPad::AddToPCCheatString is not the one "
			    "CoopIII has on record", static_cast<unsigned>(bad), TypedName(bad));
		else
			Log("cheats: CPad::AddToPCCheatString does not end where CoopIII has "
			    "it ending");
		return false;
	}
	return true;
}

// ---- the bridge -------------------------------------------------------------

void SetCheatSession(bool inSession, bool isHost, uint8_t rule) {
	if (g_ctx.inSession && !inSession)
		g_pendingCount = 0;   // for a session that has gone
	g_ctx.inSession = inSession;
	g_ctx.isHost    = isHost;
	g_ctx.rule      = rule;
}

uint8_t DrainLocalCheats(CheatBody *out, uint8_t max) {
	const uint8_t n = g_pendingCount < max ? g_pendingCount : max;
	for (uint8_t i = 0; i < n; ++i)
		out[i] = g_pending[i];
	for (uint8_t i = n; i < g_pendingCount; ++i)
		g_pending[i - n] = g_pending[i];
	g_pendingCount = static_cast<uint8_t>(g_pendingCount - n);
	return n;
}

void ApplyRoutedCheat(uint8_t id, uint8_t state) {
	if (!WorldIsUp())
		return;
	const CheatSteps steps = PlanRoutedCheat(id, state, ReadEngine());
	if (steps.refused) {
		Log("cheats: cannot bring %s (state %u) about here - the time scale is "
		    "%.3f, which no cheat makes", TypedName(id),
		    static_cast<unsigned>(state),
		    static_cast<double>(Global<float>(CTimer__ms_fTimeScale)));
		return;
	}
	for (uint8_t i = 0; i < steps.count; ++i)
		RunHandler(steps.handler);
}

} // namespace

bool InstallCheatHooks() {
	g_ctx          = CheatContext{};
	g_pendingCount = 0;

	bool ok = true;
	if (!TableMatchesImage()) {
		Log("cheats: NOT hooking CPad::AddToPCCheatString - the running code "
		    "disagrees with the table, so cheats run as in single player");
		ok = false;
	} else if (!g_cheatString.Install("CPad::AddToPCCheatString",
	                                  reinterpret_cast<void *>(CPad__AddToPCCheatString),
	                                  reinterpret_cast<void *>(&HookedAddToPCCheatString))) {
		Log("cheats: FAILED to hook CPad::AddToPCCheatString at 0x%08X; cheats "
		    "run as in single player, so a sky cheat on a non-host lasts until "
		    "the next world packet", CPad__AddToPCCheatString);
		ok = false;
	} else {
		Log("cheats: hooked CPad::AddToPCCheatString at 0x%08X", CPad__AddToPCCheatString);
	}

	if (!g_scanForThreats.Install("CPed::ScanForThreats",
	                              reinterpret_cast<void *>(CPed__ScanForThreats),
	                              reinterpret_cast<void *>(&HookedScanForThreats))) {
		Log("cheats: FAILED to hook CPed::ScanForThreats at 0x%08X; a replica "
		    "can react to a threat on this machine until its owner's stream "
		    "puts it back", CPed__ScanForThreats);
		ok = false;
	} else {
		Log("cheats: hooked CPed::ScanForThreats at 0x%08X", CPed__ScanForThreats);
	}

	if (!ok)
		for (const auto &f : HookFailures())
			Log("cheats:   %s: %s", f.name.c_str(), f.reason.c_str());
	return ok;
}

void RemoveCheatHooks() {
	g_cheatString.Remove();
	g_scanForThreats.Remove();
	g_ctx          = CheatContext{};
	g_pendingCount = 0;
}

void AddCheatsToBridge(WorldBridge &bridge) {
	if (!g_cheatString.IsInstalled())
		return;   // nothing typed here is ours to route
	bridge.SetCheatSession  = &SetCheatSession;
	bridge.DrainLocalCheats = &DrainLocalCheats;
	bridge.ApplyRoutedCheat = &ApplyRoutedCheat;
	Log("bridge: cheats run where what they change is owned");
}

} // namespace coopiii::game
