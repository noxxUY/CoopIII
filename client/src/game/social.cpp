#include "social.h"

#include "combat.h"
#include "leadcheck.h"
#include "ped.h"
#include "../client.h"
#include "../log.h"

#include <cstring>

#include <windows.h>

namespace coopiii::game {

namespace {

Client *g_client = nullptr;

using FindFn        = void *(__cdecl *)();
using GetPedFn      = void *(__cdecl *)(int32_t);
using CanSeeFn      = bool(__thiscall *)(void *, void *);
using ClearFn       = void(__cdecl *)(const void *, float, int32_t);
using PointFn       = void(__thiscall *)(void *, float, float, float, float);
using AreaFn        = void(__cdecl *)(float, float, float, float);
using VoidFn        = void(__cdecl *)();
using PadFn         = int32_t(__thiscall *)(void *);
using LineOfSightFn = bool(__cdecl *)(const float *, const float *, int, int, int, int, int, int,
                                      int);
using RegisterRefFn = void(__thiscall *)(void *, void **);

bool InSession() {
	return g_client && g_client->IsConnected() && g_client->LocalPlayerId() < MAX_PLAYERS;
}

// The ped a remote player's row names, if it is still ours to name.
void *PlayerPedOf(uint8_t id) {
	if (!g_client || id >= MAX_PLAYERS || id == g_client->LocalPlayerId())
		return nullptr;
	const RemotePlayer &p = g_client->PlayerSlot(id);
	if (!p.active || p.poolHandle < 0)
		return nullptr;
	void *const ped = Func<GetPedFn>(CPools__GetPed)(p.poolHandle);
	if (!ped || Field<uintptr_t>(ped, offs::VTABLE) != CCivilianPed__vtable)
		return nullptr;
	return ped;
}

// Points the call at `site` at `to`, only while it still calls `from`.
bool Redirect(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

bool WriteCode(uintptr_t at, const uint8_t *bytes, size_t len) {
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(at), len, PAGE_EXECUTE_READWRITE, &old))
		return false;
	std::memcpy(reinterpret_cast<void *>(at), bytes, len);
	VirtualProtect(reinterpret_cast<void *>(at), len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(at), len);
	return true;
}

// ---- the lock-on ------------------------------------------------------------------

bool g_saidLockOn = false;

bool __fastcall LockOnCanSee(void *self, void * /*edx*/, void *other) {
	uint16_t netId = INVALID_NETID;
	if (LockOnSkips(RemotePlayerForPed(other, netId), FriendlyFireOn())) {
		if (!g_saidLockOn) {
			g_saidLockOn = true;
			Log("social: the lock-on passed over player net %u; friendly fire is off (said once)",
			    static_cast<unsigned>(netId));
		}
		return false;
	}
	return Func<CanSeeFn>(CPed__OurPedCanSeeThisOne)(self, other);
}

// ---- what a respawn clears ----------------------------------------------------------

bool g_respawning = false;
bool g_saidRespawnClear = false;

bool Clears() {
	const bool clears = RespawnClearsEffects(InSession(), g_respawning);
	if (!clears && !g_saidRespawnClear) {
		g_saidRespawnClear = true;
		Log("social: a respawn cleared the peds and cars here and left the fires, burning cars, "
		    "explosions and projectiles to the session (said once)");
	}
	return clears;
}

void __cdecl RespawnClear(const void *pos, float radius, int32_t projectilesToo) {
	g_respawning = true;
	Func<ClearFn>(CWorld__ClearExcitingStuffFromArea)(pos, radius, projectilesToo);
	g_respawning = false;
}

void __fastcall ExtinguishPoint(void *manager, void * /*edx*/, float x, float y, float z,
                                float r) {
	if (Clears())
		Func<PointFn>(CFireManager__ExtinguishPoint)(manager, x, y, z, r);
}

void __cdecl ExtinguishCarFires(float x, float y, float z, float r) {
	if (Clears())
		Func<AreaFn>(CWorld__ExtinguishAllCarFiresInArea)(x, y, z, r);
}

void __cdecl RemoveExplosions(float x, float y, float z, float r) {
	if (Clears())
		Func<AreaFn>(CExplosion__RemoveAllExplosionsInArea)(x, y, z, r);
}

void __cdecl RemoveProjectiles() {
	if (Clears())
		Func<VoidFn>(CProjectileInfo__RemoveAllProjectiles)();
}

// ---- the honk ---------------------------------------------------------------------

// The cars a remote player is at the wheel of here, rebuilt every frame. Read
// by the stub below in the middle of the engine's pedestrian scan, so it is a
// plain array of pointers and nothing the stub has to call to consult.
constexpr int MAX_REMOTE_CARS = 8;
static_assert(MAX_REMOTE_CARS == MAX_PLAYERS, "the stub's loop bound is written out below");
void     *g_remoteCars[MAX_REMOTE_CARS] = {};
uintptr_t g_playerCarGoesOn = SlowCarDown_PlayerCar;
uintptr_t g_notPlayerCar    = SlowCarDown_NotPlayer;
bool      g_hornPatched     = false;

// In place of 0x00419675..0x00419685. ebp is the car; eax and cl come out as
// the original leaves them for a player's car, and nothing else is touched.
__declspec(naked) void SlowCarDownStatusTest() {
	__asm {
		mov   cl, byte ptr [ebp + 50h]
		shr   cl, 3
		movzx eax, cl
		test  eax, eax
		jz    player
		push  ecx
		xor   ecx, ecx
	next:
		cmp   ecx, 8
		jae   notours
		cmp   ebp, dword ptr g_remoteCars[ecx * 4]
		je    ours
		inc   ecx
		jmp   next
	ours:
		pop   ecx
		xor   eax, eax
		jmp   dword ptr [g_playerCarGoesOn]
	notours:
		pop   ecx
		jmp   dword ptr [g_notPlayerCar]
	player:
		jmp   dword ptr [g_playerCarGoesOn]
	}
}

bool PatchHorn() {
	if (std::memcmp(Ptr<uint8_t>(SlowCarDown_StatusTest), SLOWCARDOWN_STATUS_TEST,
	                sizeof SLOWCARDOWN_STATUS_TEST) != 0)
		return false;
	uint8_t code[sizeof SLOWCARDOWN_STATUS_TEST];
	std::memset(code, 0x90, sizeof code);
	code[0]           = 0xE9;
	const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&SlowCarDownStatusTest) -
	                                         (SlowCarDown_StatusTest + 5));
	std::memcpy(code + 1, &rel, sizeof rel);
	return WriteCode(SlowCarDown_StatusTest, code, sizeof code);
}

// ---- whose foot is on the gas -------------------------------------------------------

// The car the local player sits in when somebody else is at its wheel, or
// nobody is: its pedals are not ours to press.
void *CarWeRideIn() {
	if (!InSession())
		return nullptr;
	void *const car = Func<FindFn>(FindPlayerVehicle)();
	if (!car)
		return nullptr;
	return Field<void *>(car, offs::VEH_DRIVER) == Func<FindFn>(FindPlayerPed)() ? nullptr : car;
}

int32_t __fastcall EngineAccelerate(void *pad, void * /*edx*/) {
	if (void *const car = CarWeRideIn())
		return PedalAsPad(Field<float>(car, offs::VEH_GAS_PEDAL));
	return Func<PadFn>(CPad__GetAccelerate)(pad);
}

int32_t __fastcall EngineBrake(void *pad, void * /*edx*/) {
	if (void *const car = CarWeRideIn())
		return PedalAsPad(Field<float>(car, offs::VEH_BRAKE_PEDAL));
	return Func<PadFn>(CPad__GetBrake)(pad);
}

// ---- the table -----------------------------------------------------------------------

struct Site {
	uintptr_t   at;
	uintptr_t   engine;
	uintptr_t   ours;
	const char *what;
	bool        done;
};

Site g_sites[] = {
    {LockOn_CanSeeCall, CPed__OurPedCanSeeThisOne, reinterpret_cast<uintptr_t>(&LockOnCanSee),
     "the lock-on's sight test", false},
    {NextLockOn_CanSeeCall, CPed__OurPedCanSeeThisOne, reinterpret_cast<uintptr_t>(&LockOnCanSee),
     "the next lock-on's sight test", false},
    {GameLogic_WastedClearCall, CWorld__ClearExcitingStuffFromArea,
     reinterpret_cast<uintptr_t>(&RespawnClear), "the hospital's clear", false},
    {GameLogic_BustedClearCall, CWorld__ClearExcitingStuffFromArea,
     reinterpret_cast<uintptr_t>(&RespawnClear), "the police station's clear", false},
    {GameLogic_FailedClearCall, CWorld__ClearExcitingStuffFromArea,
     reinterpret_cast<uintptr_t>(&RespawnClear), "the failed mission's clear", false},
    {ClearExciting_FireCall, CFireManager__ExtinguishPoint,
     reinterpret_cast<uintptr_t>(&ExtinguishPoint), "the clear's fires", false},
    {ClearExciting_CarFireCall, CWorld__ExtinguishAllCarFiresInArea,
     reinterpret_cast<uintptr_t>(&ExtinguishCarFires), "the clear's burning cars", false},
    {ClearExciting_ExplosionCall, CExplosion__RemoveAllExplosionsInArea,
     reinterpret_cast<uintptr_t>(&RemoveExplosions), "the clear's explosions", false},
    {ClearExciting_ProjectileCall, CProjectileInfo__RemoveAllProjectiles,
     reinterpret_cast<uintptr_t>(&RemoveProjectiles), "the clear's projectiles", false},
    {ENGINE_ACCELERATE_CALLS[0], CPad__GetAccelerate, reinterpret_cast<uintptr_t>(&EngineAccelerate),
     "the Dodo's gas", false},
    {ENGINE_ACCELERATE_CALLS[1], CPad__GetAccelerate, reinterpret_cast<uintptr_t>(&EngineAccelerate),
     "the engine's gas", false},
    {ENGINE_ACCELERATE_CALLS[2], CPad__GetAccelerate, reinterpret_cast<uintptr_t>(&EngineAccelerate),
     "the boat's gas", false},
    {ENGINE_ACCELERATE_CALLS[3], CPad__GetAccelerate, reinterpret_cast<uintptr_t>(&EngineAccelerate),
     "the boat's gas", false},
    {ENGINE_ACCELERATE_CALLS[4], CPad__GetAccelerate, reinterpret_cast<uintptr_t>(&EngineAccelerate),
     "the boat's gas", false},
    {ENGINE_ACCELERATE_CALLS[5], CPad__GetAccelerate, reinterpret_cast<uintptr_t>(&EngineAccelerate),
     "the boat's gas", false},
    {ENGINE_BRAKE_CALLS[0], CPad__GetBrake, reinterpret_cast<uintptr_t>(&EngineBrake),
     "the boat's brake", false},
    {ENGINE_BRAKE_CALLS[1], CPad__GetBrake, reinterpret_cast<uintptr_t>(&EngineBrake),
     "the boat's brake", false},
    {ENGINE_BRAKE_CALLS[2], CPad__GetBrake, reinterpret_cast<uintptr_t>(&EngineBrake),
     "the boat's brake", false},
    {ENGINE_BRAKE_CALLS[3], CPad__GetBrake, reinterpret_cast<uintptr_t>(&EngineBrake),
     "the boat's brake", false},
};

// ---- the gangs ------------------------------------------------------------------------

float DistanceSq(void *a, void *b) {
	const float *pa = &Field<float>(a, offs::POSITION);
	const float *pb = &Field<float>(b, offs::POSITION);
	const float  dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
	return dx * dx + dy * dy + dz * dz;
}

// In front of it, within 40 m, and nothing solid between its head and them.
bool GangCanSee(void *ped, void *target) {
	const float *n  = &Field<float>(ped, offs::POSITION);
	const float *t  = &Field<float>(target, offs::POSITION);
	const float *f  = &Field<float>(ped, offs::MATRIX_FWD);
	const float  dx = t[0] - n[0], dy = t[1] - n[1];
	if (dx * f[0] + dy * f[1] < 0.0f || dx * dx + dy * dy >= 40.0f * 40.0f)
		return false;
	const float head[3] = {n[0], n[1], n[2] + 1.0f};
	const float at[3]   = {t[0], t[1], t[2]};
	return Func<LineOfSightFn>(CWorld__GetIsLineOfSightClear)(head, at, 1, 0, 0, 0, 0, 0, 0);
}

bool g_saidGang = false;

bool MenuUp() {
	return Global<int32_t>(gGameState) == GS_PLAYING_GAME &&
	       Global<uint8_t>(CMenuManager__m_bMenuActive) != 0;
}

} // namespace

bool RespawnClearing() { return g_respawning; }

uint32_t EverydayGangThreat(void *ped, uint32_t found) {
	if (!ped || !InSession())
		return found;
	const uintptr_t playerType = Global<uintptr_t>(CPedType__ms_apPedType);
	const uint32_t  playerFlag =
	    playerType ? *reinterpret_cast<const uint32_t *>(playerType + offs::PEDTYPE_FLAG) : 0;
	if (!GangHatesPlayers(Field<int32_t>(ped, offs::PED_TYPE),
	                      Field<uint8_t>(ped, offs::PED_CHAR_CREATED_BY),
	                      Field<uint32_t>(ped, offs::PED_FEAR_FLAGS), playerFlag))
		return found;
	// A gun, an explosion or somebody else stays what the engine found.
	if (found != 0 && found != playerFlag)
		return found;
	void **const threat = &Field<void *>(ped, offs::PED_THREAT_ENTITY);
	float        best   = found != 0 && *threat ? DistanceSq(ped, *threat) : 40.0f * 40.0f;
	void        *nearer = nullptr;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		void *const other = PlayerPedOf(id);
		if (!other || g_client->PlayerSlot(id).dead ||
		    Field<float>(other, offs::PED_HEALTH) <= 0.0f)
			continue;
		const float d = DistanceSq(ped, other);
		if (d < best && GangCanSee(ped, other)) {
			best   = d;
			nearer = other;
		}
	}
	if (!nearer)
		return found;
	*threat = nearer;
	Func<RegisterRefFn>(CEntity__RegisterReference)(nearer, threat);
	if (!g_saidGang) {
		g_saidGang = true;
		Log("social: a gang member set on the player saw another player first (said once)");
	}
	return playerFlag;
}

bool InstallSocialHooks(Client &client) {
	g_client = &client;
	bool ok  = true;
	for (Site &s : g_sites) {
		if (s.done)
			continue;
		s.done = Redirect(s.at, s.engine, s.ours);
		if (!s.done)
			Log("social: FAILED to redirect %s at 0x%08X; it no longer calls 0x%08X", s.what,
			    static_cast<unsigned>(s.at), static_cast<unsigned>(s.engine));
		ok &= s.done;
	}
	if (!g_hornPatched) {
		g_hornPatched = PatchHorn();
		if (!g_hornPatched)
			Log("social: 0x%08X is not the car scan's status test CoopIII has on record; a "
			    "remote player's honk does not scatter our pedestrians",
			    static_cast<unsigned>(SlowCarDown_StatusTest));
		ok &= g_hornPatched;
	}
	Log("social: the lock-on, the respawn's clear, the honk and a passenger's engine %s",
	    ok ? "come to us" : "are partly the engine's own; see the lines above");
	return ok;
}

void RemoveSocialHooks() {
	for (Site &s : g_sites)
		if (s.done && Redirect(s.at, s.ours, s.engine))
			s.done = false;
	if (g_hornPatched &&
	    WriteCode(SlowCarDown_StatusTest, SLOWCARDOWN_STATUS_TEST, sizeof SLOWCARDOWN_STATUS_TEST))
		g_hornPatched = false;
	std::memset(g_remoteCars, 0, sizeof g_remoteCars);
	g_client = nullptr;
}

void TickSocial() {
	void *cars[MAX_REMOTE_CARS] = {};
	int   n = 0;
	if (InSession()) {
		for (uint8_t id = 0; id < MAX_PLAYERS && n < MAX_REMOTE_CARS; ++id) {
			void *const ped = PlayerPedOf(id);
			if (!ped || !Field<bool>(ped, offs::PED_IN_VEHICLE))
				continue;
			void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
			if (car && Field<void *>(car, offs::VEH_DRIVER) == ped)
				cars[n++] = car;
		}
	}
	std::memcpy(g_remoteCars, cars, sizeof g_remoteCars);
}

void AddSocialToBridge(WorldBridge &bridge) { bridge.LocalMenuUp = &MenuUp; }

} // namespace coopiii::game
