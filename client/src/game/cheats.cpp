#include "cheats.h"

#include "mission.h"
#include "ped.h"
#include "population.h"
#include "social.h"
#include "hook/hook.h"
#include "log.h"

#include <windows.h>

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

// The cheats as the running code compares them (cheats.h, TypedCheatTable),
// for the chat key's two rules and the detour's compares. Retail's until
// InstallCheatHooks reads it.
TypedCheatTable g_typed = TableFromSites();

// CoopIII's own cheats typed since the last drain, and the engine's buffer as
// it was the last time it was looked at: a key the handler pushed nothing for
// leaves it the same, and the same buffer is not the same cheat typed again.
constexpr uint8_t MAX_COOP_PENDING = 4;
CoopCheatHit      g_coopPending[MAX_COOP_PENDING];
uint8_t           g_coopCount = 0;
char              g_seenBuffer[KEYBOARD_CHEAT_STRING_LEN] = {};

using HandlerFn    = void(__cdecl *)();
using CheatStrFn   = void(__thiscall *)(void *, char);
using ScanFn       = uint32_t(__thiscall *)(void *);
using FindPlayerFn = void *(__cdecl *)();

bool WorldIsUp() { return Global<uint32_t>(gGameState) == GS_PLAYING_GAME; }

// What a row's cheat is typed as in this game, for the log: the running
// table's, which is what was typed, not retail's.
const char *RowName(uint8_t id) {
	static char names[CHEAT_COUNT][KEYBOARD_CHEAT_STRING_LEN + 1];
	if (id >= CHEAT_COUNT)
		return "?";
	const TypedRow &row = g_typed.rows[id];
	const uint8_t   len = RowTypedLength(row);
	for (uint8_t i = 0; i < len; ++i)
		names[id][i] = row.reversed[len - 1 - i];
	names[id][len] = '\0';
	return names[id];
}

// The retail spelling, for the log line that says another plugin changed it.
const char *RetailName(uint8_t id) {
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
		    RowName(id), static_cast<unsigned>(MAX_PENDING_CHEATS));
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
			Log("cheats: %s, here only", RowName(id));
		return;

	case CheatVerdict::Refused:
		Log("cheats: %s refused - %s", RowName(id), CheatRefusalReason(g_ctx.rule));
		return;

	case CheatVerdict::SendToHost:
		Queue(id, 0);
		Log("cheats: %s goes to the host; our sky follows theirs", RowName(id));
		return;

	case CheatVerdict::RunHereAndSend: {
		RunTyped(id);
		uint8_t state = 0;
		if (!CurrentCheatState(id, ReadEngine(), state)) {
			// Only the time scale can fail this, and only when something other
			// than the two cheats set it - a mission's slow motion. Sending a
			// state we cannot name would move everybody else somewhere else.
			Log("cheats: %s ran here but the time scale is %.3f, which no cheat "
			    "makes; not sent", RowName(id),
			    static_cast<double>(Global<float>(CTimer__ms_fTimeScale)));
			return;
		}
		Queue(id, state);
		Log("cheats: %s, here and for everybody (state %u)", RowName(id),
		    static_cast<unsigned>(state));
		return;
	}
	}
}

// __thiscall void CPad::AddToPCCheatString(char c).  ret 4.
//
// Outside a session the engine's own function runs and nothing here is
// involved. Inside one, the same shift and the same 23 compares happen here,
// as this game has them (g_typed, read off the running code), in the same
// order, into the same buffer - and each match goes through
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
	const uint8_t n = MatchTypedCheats(g_typed, buffer, ids, CHEAT_COUNT);
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
	// The session's mission's own pedestrians see its participants as the
	// player too (mission.h).
	// And an everyday gang member set on the player, the other players too.
	return EverydayGangThreat(ped, MissionThreat(ped, g_scanForThreats.Original<ScanFn>()(ped)));
}

uint8_t LiveByte(const void * /*ctx*/, uint32_t va) { return *Ptr<uint8_t>(va); }

// `n` bytes from `va` can be read: a string another plugin's row points into
// its own module is checked before it is touched.
bool Readable(const void * /*ctx*/, uint32_t va, size_t n) {
	MEMORY_BASIC_INFORMATION mbi{};
	uint32_t                 at  = va;
	const uint32_t           end = va + static_cast<uint32_t>(n);
	while (at < end) {
		if (VirtualQuery(reinterpret_cast<const void *>(static_cast<uintptr_t>(at)), &mbi,
		                 sizeof mbi) == 0 ||
		    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
			return false;
		const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (next <= at)
			return false;
		at = static_cast<uint32_t>(next);
	}
	return true;
}

const char *FaultText(CheatImageFault f) {
	switch (f) {
	case CheatImageFault::Prologue:
		return "it does not start the way the retail one does - another plugin has hooked it";
	case CheatImageFault::Shift:
		return "the shift into the buffer is not retail's";
	case CheatImageFault::Shape:
		return "its rows are not in the shape CoopIII knows";
	case CheatImageFault::Unreadable:
		return "a row's string cannot be read";
	case CheatImageFault::Length:
		return "a row compares nothing, or more than the buffer has";
	case CheatImageFault::Row:
		return "a row is neither retail's nor a rewrite CoopIII knows";
	case CheatImageFault::None:
		break;
	}
	return "?";
}

// The code that is actually running, against what the detour would imitate
// (cheats.h, CheckCheatImage). The exe passed its MD5 check before anything
// was hooked, but what runs is the exe plus whatever every other plugin has
// patched into it since (docs/compat.md). The table the chat key and the
// detour go by is read off that code. True when the detour may go in.
bool CheckRunningCode() {
	const CheatImageCheck c = CheckCheatImage(&LiveByte, &Readable, nullptr);
	g_typed                 = c.table;
	if (!c.tableRead) {
		Log("cheats: cannot read the cheat table off CPad::AddToPCCheatString - %s (row "
		    "%u); the chat key goes by retail's cheats", FaultText(c.fault),
		    static_cast<unsigned>(c.row));
		return false;
	}

	const TypedCheatTable retail = TableFromSites();
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		const TypedRow &a = c.table.rows[id], &b = retail.rows[id];
		if (a.length == b.length && std::memcmp(a.reversed, b.reversed, sizeof a.reversed) == 0)
			continue;
		Log("cheats: row %u is %s here (%u compared), %s in retail (%s); the chat key goes "
		    "by what is here", static_cast<unsigned>(id), RowName(id),
		    static_cast<unsigned>(a.length), RetailName(id),
		    c.rewritten[id] != 0 ? KNOWN_CHEAT_ROWS[c.rewritten[id] - 1].by
		                         : "not a rewrite CoopIII knows");
	}

	if (!c.MayHook()) {
		if (c.row < CHEAT_COUNT)
			Log("cheats: CPad::AddToPCCheatString: %s (row %u, %s)", FaultText(c.fault),
			    static_cast<unsigned>(c.row), RowName(c.row));
		else
			Log("cheats: CPad::AddToPCCheatString: %s", FaultText(c.fault));
		return false;
	}
	if (c.verdict == CheatImageVerdict::Known)
		Log("cheats: CPad::AddToPCCheatString has rows rewritten the way CoopIII knows; "
		    "in a session cheats are matched as this game spells them");
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
		    "%.3f, which no cheat makes", RowName(id),
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
	// After every other plugin has patched what it patches: this runs once
	// the game loop does (dllmain.cpp).
	const bool mayHook = CheckRunningCode();
	g_coopCount = 0;
	std::memcpy(g_seenBuffer, Ptr<char>(CPad__KeyBoardCheatString), sizeof g_seenBuffer);

	bool ok = true;
	if (!mayHook) {
		Log("cheats: NOT hooking CPad::AddToPCCheatString - the running code is not "
		    "what it would imitate, so cheats run as in single player");
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
	g_coopCount    = 0;
}

char EngineCheatCharFor(int vk) {
	// What the key handler does before AddToPCCheatString sees a key:
	// `push 2 / push edx / call [MapVirtualKeyA] / and eax,0FFFFh /
	// cmp eax,0FFh / jge skip` at 0x00583D90.
	const UINT c = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0xFFFF;
	return c != 0 && c < 0xFF ? static_cast<char>(c) : '\0';
}

bool ChatKeyWouldFinishCheat(char key) {
	return ChatKeyFinishesCheat(g_typed, Ptr<char>(CPad__KeyBoardCheatString), key);
}

bool FinishCheatFromChatLine(char key, const char *line) {
	char       keys[KEYBOARD_CHEAT_STRING_LEN];
	uint8_t    count = 0;
	TypedMatch what;
	if (!CheatFinishedInChatLine(g_typed, Ptr<char>(CPad__KeyBoardCheatString), key, line, keys,
	                             &count, &what))
		return false;
	if (what.engineCount != 0)
		Log("cheats: %s ran into the chat key and was finished in the chat line; "
		    "handing it to the game, the line is not sent", RowName(what.engine[0]));
	else
		Log("cheats: %s%c ran into the chat key and was finished in the chat line; the "
		    "line is not sent", COOP_CHEATS[what.coop.id].word, what.coop.arg ? what.coop.arg : ' ');
	// The engine's key handler passes CPad::GetPad(0), i.e. Pads[0]
	// (0x00583F17 .. 0x005841C7). Through the address, so it goes through
	// the detour above when that is installed.
	for (uint8_t i = 0; i < count; ++i)
		Func<CheatStrFn>(CPad__AddToPCCheatString)(Ptr<void>(CPad__Pads), keys[i]);
	// Ours are read off the buffer those keys just went into, the same as
	// when they are typed with the line shut.
	NoticeTypedKeys();
	return true;
}

void NoticeTypedKeys() {
	const char *const buffer = Ptr<char>(CPad__KeyBoardCheatString);
	if (std::memcmp(buffer, g_seenBuffer, sizeof g_seenBuffer) == 0)
		return;
	std::memcpy(g_seenBuffer, buffer, sizeof g_seenBuffer);
	CoopCheatHit hit;
	if (!MatchCoopCheat(buffer, &hit))
		return;
	if (g_coopCount == MAX_COOP_PENDING) {
		Log("cheats: %s%c typed with %u of CoopIII's cheats already waiting; dropped",
		    COOP_CHEATS[hit.id].word, hit.arg ? hit.arg : ' ',
		    static_cast<unsigned>(MAX_COOP_PENDING));
		return;
	}
	g_coopPending[g_coopCount++] = hit;
}

uint8_t DrainCoopCheats(CoopCheatHit *out, uint8_t max) {
	const uint8_t n = g_coopCount < max ? g_coopCount : max;
	for (uint8_t i = 0; i < n; ++i)
		out[i] = g_coopPending[i];
	for (uint8_t i = n; i < g_coopCount; ++i)
		g_coopPending[i - n] = g_coopPending[i];
	g_coopCount = static_cast<uint8_t>(g_coopCount - n);
	return n;
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
