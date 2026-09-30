#include "emergency.h"

#include "cargun.h"
#include "leadcheck.h"
#include "ped.h"
#include "population.h"
#include "vehicle.h"
#include "../log.h"

#include <windows.h>

#include <cstring>

namespace coopiii::game {

namespace {

using ThisFn         = void(__thiscall *)(void *);
using FindAccidentFn = void *(__thiscall *)(void *, float, float, float, float *);
using ReportFn       = void(__thiscall *)(void *, void *);
using MoveStateFn    = void(__thiscall *)(void *, uint32_t);
using UpdateOneFn    = void(__cdecl *)(uint32_t, Vec3 *, Vec3 *);
using ExtinguishAtFn = void(__thiscall *)(void *, float, float, float, float);
using SideFn         = int(__thiscall *)(void *, const void *);
using ForceFn        = void(__thiscall *)(void *, float, float, float);
using FallFn         = void(__thiscall *)(void *, uint32_t, uint32_t, uint32_t);

// Points the `call` at `site` at `to`, only while it still calls `from`.
bool RedirectCall(uintptr_t site, uintptr_t from, uintptr_t to) {
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

void *AccidentVictim(size_t i) {
	return Field<void *>(Ptr<void>(gAccidentManager + i * SIZEOF_CACCIDENT), ACCIDENT_VICTIM);
}

bool IsAccidentVictim(const void *ped) {
	for (size_t i = 0; i < NUM_ACCIDENTS; ++i)
		if (AccidentVictim(i) == ped)
			return true;
	return false;
}

// ---- the medic ------------------------------------------------------------------

// MedicAI's three questions to FindNearestAccident, asked with every replica
// victim reading RANDOM_CHAR for the length of the call. The function only
// reads, so nothing outlives it; the bytes go back in reverse.
void *__fastcall FindAccidentSeeingReplicas(void *self, void * /*edx*/, float x, float y,
                                            float z, float *dist) {
	struct Lifted {
		void   *ped;
		uint8_t was;
	};
	Lifted  lifted[NUM_ACCIDENTS];
	size_t  n = 0;
	for (size_t i = 0; i < NUM_ACCIDENTS; ++i) {
		void    *victim = AccidentVictim(i);
		uint16_t netId  = INVALID_NETID;
		if (!victim || !AmbientReplicaForPed(victim, netId))
			continue;
		uint8_t &by = Field<uint8_t>(victim, offs::PED_CHAR_CREATED_BY);
		lifted[n++] = {victim, by};
		by          = CHAR_CREATED_BY_RANDOM_BYTE;
	}
	void *const found =
	    Func<FindAccidentFn>(CAccidentManager__FindNearestAccident)(self, x, y, z, dist);
	while (n > 0) {
		--n;
		Field<uint8_t>(lifted[n].ped, offs::PED_CHAR_CREATED_BY) = lifted[n].was;
	}
	return found;
}

constexpr size_t MAX_QUEUED_REVIVES = 16;
uint16_t         g_revives[MAX_QUEUED_REVIVES];
uint32_t         g_reviveCount = 0;
bool             g_saidRevive  = false;
bool             g_saidNoName  = false;

void QueueRevive(uint16_t netId) {
	for (uint32_t i = 0; i < g_reviveCount; ++i)
		if (g_revives[i] == netId)
			return;
	if (g_reviveCount < MAX_QUEUED_REVIVES)
		g_revives[g_reviveCount++] = netId;
}

// The medic's own SetGetUp, from the one place in MedicAI that stands a patient
// up. Everything the medic wrote before it stays; what follows it in his arm
// runs on return, as always.
void __fastcall MedicStandsUp(void *ped, void * /*edx*/) {
	Func<ThisFn>(CPed__SetGetUp)(ped);
	if (!ped)
		return;

	uint16_t   netId   = INVALID_NETID;
	const bool replica = AmbientReplicaForPed(ped, netId);
	if (replica) {
		// The medic gave him WANDER_PATH to go back to, and a replica that
		// went back to it would choose a path and walk it here.
		Field<uint32_t>(ped, offs::PED_LAST_STATE) = ReviveLastState(true);
		QueueRevive(netId);
	} else if (HostedPedNetIdFor(ped, netId) && !HostedMissionEntity(ped)) {
		QueueRevive(netId);
	} else {
		if (!g_saidNoName) {
			g_saidNoName = true;
			Log("emergency: a medic stood up a pedestrian the session has no name for; "
			    "he gets up here only (and this will not be said again)");
		}
		return;
	}
	if (!g_saidRevive) {
		g_saidRevive = true;
		Log("emergency: one of our medics stood pedestrian %u up (%s)", netId,
		    replica ? "our copy of somebody else's" : "one we host");
	}
}

uint32_t DrainMedicRevives(uint16_t *out, uint32_t max) {
	const uint32_t n = g_reviveCount < max ? g_reviveCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_revives[i];
	for (uint32_t i = n; i < g_reviveCount; ++i)
		g_revives[i - n] = g_revives[i];
	g_reviveCount -= n;
	return n;
}

// The medic's instructions after the CPR, in his order (emergencyaddr.h), on a
// ped somebody else's medic stood up. False for one that is not dead here.
bool StandUpLikeAMedic(void *ped, uint32_t lastState) {
	if (!DeadForMedic(Field<uint32_t>(ped, offs::PED_STATE)))
		return false;
	Field<float>(ped, offs::PED_HEALTH)        = MEDIC_REVIVE_HEALTH;
	Field<uint32_t>(ped, offs::PED_STATE)      = PEDSTATE_NONE;
	Field<uint32_t>(ped, offs::PED_LAST_STATE) = lastState;
	Func<ThisFn>(CPed__SetGetUp)(ped);
	Field<uint8_t>(ped, offs::ENTITY_FLAGS_A) |= offs::ENTITY_USES_COLLISION;
	Func<MoveStateFn>(CPed__SetMoveState)(ped, PEDMOVE_WALK);
	Func<ThisFn>(CPed__RestartNonPartialAnims)(ped);
	Field<uint8_t>(ped, offs::PED_FLAGS_D) &= static_cast<uint8_t>(~offs::PED_DIE_ANIM_PLAYING);
	Field<uint8_t>(ped, offs::PED_FLAGS_H) &= static_cast<uint8_t>(~offs::PED_KNOCKED_UP_INTO_AIR);
	Field<void *>(ped, offs::PED_COLLIDING_ENTITY) = nullptr;
	return true;
}

bool ReviveAmbientReplica(RemoteAmbientPed &row) {
	void *const ped = AmbientReplicaPed(row);
	if (!ped)
		return false;
	if (StandUpLikeAMedic(ped, ReviveLastState(true))) {
		// Whatever the stream had put on him went with the death; the next row
		// puts it back.
		row.appliedAnimId = ANIM_NONE;
		row.appliedWeapon = 0xFF;
	}
	return true;
}

bool ReviveHostedPed(uint16_t netId) {
	void *const ped = ResolveHostedPed(netId);
	if (!ped || HostedMissionEntity(ped))
		return false;
	return StandUpLikeAMedic(ped, ReviveLastState(false));
}

// A replica's body, made an accident the way its host's engine made the real
// one: through ReportAccident, with the two bytes that refuse a replica -
// MISSION_CHAR and bAllowMedicsToReviveMe clear, both SpawnAmbientReplica's -
// lifted for the call. Everything else it tests is the body's own.
void OfferCorpseToMedics(RemoteAmbientPed &row) {
	void *const ped = AmbientReplicaPed(row);
	if (!ped || !DeadForMedic(Field<uint32_t>(ped, offs::PED_STATE)))
		return;
	uint8_t      &by    = Field<uint8_t>(ped, offs::PED_CHAR_CREATED_BY);
	uint8_t      &g     = Field<uint8_t>(ped, offs::PED_FLAGS_G);
	const uint8_t wasBy = by;
	const uint8_t wasG  = g;
	by = CHAR_CREATED_BY_RANDOM_BYTE;
	g  = static_cast<uint8_t>(g | offs::PED_ALLOW_MEDICS);
	Func<ReportFn>(CAccidentManager__ReportAccident)(Ptr<void>(gAccidentManager), ped);
	by = wasBy;
	g  = wasG;

	static bool said = false;
	if (!said && IsAccidentVictim(ped)) {
		said = true;
		Log("emergency: pedestrian %u lies dead here because his host's engine killed "
		    "him, and our medics will come for him like any of our own", row.netId);
	}
}

// ---- the water cannon -----------------------------------------------------------

CarFrame FrameOf(void *car) {
	CarFrame f;
	f.right = Field<Vec3>(car, offs::MATRIX_RIGHT);
	f.fwd   = Field<Vec3>(car, offs::MATRIX_FWD);
	f.up    = Field<Vec3>(car, offs::MATRIX_UP);
	f.pos   = Field<Vec3>(car, offs::POSITION);
	return f;
}

LocalCannonJet g_jets[MAX_LOCAL_JETS];
uint32_t       g_jetCount       = 0;
uint32_t       g_jetsRefused    = 0;
bool           g_saidJetRefused = false;

void HoldJet(uint16_t netId, const Vec3 &pos, const Vec3 &dir) {
	for (uint32_t i = 0; i < g_jetCount; ++i)
		if (g_jets[i].netId == netId) {
			g_jets[i].pos = pos;
			g_jets[i].dir = dir;
			return;
		}
	if (g_jetCount < MAX_LOCAL_JETS)
		g_jets[g_jetCount++] = {netId, pos, dir};
}

// FireTruckControl's jet, from either arm. Whether we drive is asked of the
// car, not of FindPlayerVehicle, which names a passenger's car too: should
// game/cargun.h's gate be down, a passenger's player arm still runs, and this
// is what keeps his jet off a truck somebody else drives.
void __cdecl CannonInput(uint32_t id, Vec3 *pos, Vec3 *dir) {
	void *const car      = reinterpret_cast<void *>(static_cast<uintptr_t>(id));
	const bool  driver   = LocalPlayerDrives(car);
	uint16_t    netId    = INVALID_NETID;
	bool        othersMove = false;
	const bool  named    = car != nullptr && SessionCarFor(car, netId, othersMove);
	if (!CannonInputMayRun(driver, named, othersMove)) {
		++g_jetsRefused;
		if (!g_saidJetRefused) {
			g_saidJetRefused = true;
			Log("emergency: our engine's fire truck logic tried to spray from truck %u, "
			    "which another machine moves; its jet comes from there instead", netId);
		}
		return;
	}
	Func<UpdateOneFn>(CWaterCannons__UpdateOne)(id, pos, dir);
	if (!named || netId == INVALID_NETID || !pos || !dir)
		return;
	const CarFrame f = FrameOf(car);
	HoldJet(netId, WorldPointToCar(f, *pos), WorldVectorToCar(f, *dir));
}

uint32_t DrainLocalJets(LocalCannonJet *out, uint32_t max) {
	const uint32_t n = g_jetCount < max ? g_jetCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_jets[i];
	g_jetCount = 0;
	return n;
}

// Somebody else's jet, from our copy of the truck, the way FireTruckControl
// hands one over. A passenger's copy sprays it as well: it is the jet of the
// truck he rides in, and its driver's to aim.
bool SprayCopy(uint16_t netId, const Vec3 &pos, const Vec3 &dir) {
	void *const car = CopyOfCar(netId);
	if (!car || LocalPlayerDrives(car))
		return false;
	if (Field<int16_t>(car, offs::MODEL_INDEX) != FIRETRUCK_MODEL)
		return false;
	if ((Field<uint8_t>(car, offs::ENTITY_FLAGS) >> ENTITY_STATUS_SHIFT) == ENTITY_STATUS_WRECKED)
		return false;
	const CarFrame f     = FrameOf(car);
	Vec3           start = CarPointToWorld(f, pos);
	Vec3           way   = CarVectorToWorld(f, dir);
	Func<UpdateOneFn>(CWaterCannons__UpdateOne)(
	    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(car)), &start, &way);
	return true;
}

// Whose is the thing a fire burns on.
FireOn WhatBurns(void *entity) {
	if (!entity)
		return FireOn::Nothing;
	const uint8_t type = Field<uint8_t>(entity, offs::ENTITY_FLAGS) & 0x07u;
	uint16_t      netId = INVALID_NETID;
	if (type == offs::ENTITY_TYPE_PED)
		return WaterMayMovePed(RemotePlayerForPed(entity, netId),
		                       AmbientReplicaForPed(entity, netId))
		           ? FireOn::OurThing
		           : FireOn::SomebodyElses;
	if (type == ENTITY_TYPE_VEHICLE) {
		bool othersMove = false;
		if (SessionCarFor(entity, netId, othersMove) && othersMove)
			return FireOn::SomebodyElses;
	}
	return FireOn::OurThing;
}

bool g_saidFireLeft = false;

// Update_OncePerFrame's ExtinguishPoint, with every fire on somebody else's
// thing reading "not burning" for the length of the call. ExtinguishPoint
// skips a fire that is not ongoing (`cmp byte [ebx+esi+4],0` at 0x00479DE2)
// and touches nothing else about it.
void __fastcall ExtinguishOnlyOurs(void *manager, void * /*edx*/, float x, float y, float z,
                                   float range) {
	bool held[NUM_FIRES] = {};
	for (size_t i = 0; i < NUM_FIRES; ++i) {
		void *const fire = Ptr<void>(FireSlot(i));
		if (!Field<uint8_t>(fire, FIRE_ONGOING))
			continue;
		if (WaterMayPutOut(WhatBurns(Field<void *>(fire, FIRE_ENTITY))))
			continue;
		held[i]                        = true;
		Field<uint8_t>(fire, FIRE_ONGOING) = 0;
	}
	Func<ExtinguishAtFn>(CFireManager__ExtinguishPoint)(manager, x, y, z, range);
	for (size_t i = 0; i < NUM_FIRES; ++i) {
		if (!held[i])
			continue;
		Field<uint8_t>(Ptr<void>(FireSlot(i)), FIRE_ONGOING) = 1;
		if (!g_saidFireLeft) {
			g_saidFireLeft = true;
			Log("emergency: a jet here left burning a fire on somebody else's ped or car; "
			    "its owner's copy of the jet puts it out (and this will not be said again)");
		}
	}
}

// PushPeds, one ped at a time: the side it is hit from is asked first, so that
// is where the ped is looked at, and the three calls after it are refused for
// one this machine does not own. The two stores PushPeds makes in between
// (bIsStanding and the move speed) are put back.
struct Push {
	void   *ped     = nullptr;
	bool    refused = false;
	uint8_t flagsA  = 0;
	Vec3    speed   = {};
};
Push     g_push;
uint32_t g_pushesRefused = 0;

int __fastcall PushSide(void *ped, void * /*edx*/, const void *axis) {
	uint16_t netId = INVALID_NETID;
	g_push.ped     = ped;
	g_push.refused = ped != nullptr &&
	                 !WaterMayMovePed(RemotePlayerForPed(ped, netId),
	                                  AmbientReplicaForPed(ped, netId));
	if (g_push.refused) {
		g_push.flagsA = Field<uint8_t>(ped, offs::PED_FLAGS_A);
		g_push.speed  = Field<Vec3>(ped, offs::MOVE_SPEED);
	}
	return Func<SideFn>(CPed__GetLocalDirection)(ped, axis);
}

void __fastcall PushForce(void *ped, void * /*edx*/, float x, float y, float z) {
	if (g_push.refused && ped == g_push.ped)
		return;
	Func<ForceFn>(CPhysical__ApplyMoveForce)(ped, x, y, z);
}

void __fastcall PushFall(void *ped, void * /*edx*/, uint32_t timeMs, uint32_t anim,
                         uint32_t flag) {
	if (g_push.refused && ped == g_push.ped) {
		const uint8_t standing = offs::PED_IS_STANDING;
		uint8_t      &flagsA   = Field<uint8_t>(ped, offs::PED_FLAGS_A);
		flagsA = static_cast<uint8_t>((flagsA & ~standing) | (g_push.flagsA & standing));
		Field<Vec3>(ped, offs::MOVE_SPEED) = g_push.speed;
		if (++g_pushesRefused == 1)
			Log("emergency: a jet here reached somebody else's pedestrian or player and "
			    "left him standing; his own machine's copy of the jet knocks him down");
		return;
	}
	Func<FallFn>(CPed__SetFall)(ped, timeMs, anim, flag);
}

void __fastcall PushExtinguish(void *fire, void * /*edx*/) {
	if (g_push.refused)
		return;
	Func<ThisFn>(CFire__Extinguish)(fire);
}

// ---- the call sites ---------------------------------------------------------------

struct Site {
	uintptr_t   at;
	uintptr_t   engine;
	uintptr_t   ours;
	const char *what;
	bool        taken;
};

Site g_sites[] = {
    {MEDIC_FIND_ACCIDENT_CALLS[0], CAccidentManager__FindNearestAccident,
     reinterpret_cast<uintptr_t>(&FindAccidentSeeingReplicas), "a medic in his ambulance", false},
    {MEDIC_FIND_ACCIDENT_CALLS[1], CAccidentManager__FindNearestAccident,
     reinterpret_cast<uintptr_t>(&FindAccidentSeeingReplicas), "a medic choosing a patient", false},
    {MEDIC_FIND_ACCIDENT_CALLS[2], CAccidentManager__FindNearestAccident,
     reinterpret_cast<uintptr_t>(&FindAccidentSeeingReplicas), "a medic changing patient", false},
    {MEDIC_REVIVE_GETUP_CALL, CPed__SetGetUp, reinterpret_cast<uintptr_t>(&MedicStandsUp),
     "a medic's revive", false},
    {FIRE_TRUCK_CANNON_CALL, CWaterCannons__UpdateOne, reinterpret_cast<uintptr_t>(&CannonInput),
     "a fire truck's jet", false},
    {WATER_CANNON_EXTINGUISH_CALL, CFireManager__ExtinguishPoint,
     reinterpret_cast<uintptr_t>(&ExtinguishOnlyOurs), "the water on fires", false},
    {PUSH_PEDS_SIDE_CALL, CPed__GetLocalDirection, reinterpret_cast<uintptr_t>(&PushSide),
     "the water on a pedestrian", false},
    {PUSH_PEDS_FORCE_CALL, CPhysical__ApplyMoveForce, reinterpret_cast<uintptr_t>(&PushForce),
     "the water lifting him", false},
    {PUSH_PEDS_FALL_CALL, CPed__SetFall, reinterpret_cast<uintptr_t>(&PushFall),
     "the water knocking him down", false},
    {PUSH_PEDS_EXTINGUISH_CALL, CFire__Extinguish, reinterpret_cast<uintptr_t>(&PushExtinguish),
     "the water on a burning pedestrian", false},
};

} // namespace

bool InstallEmergencyHooks() {
	// The four PushPeds sites are one decision: any one of them without the
	// others would refuse half a knockdown.
	bool pushOk = true;
	for (Site &s : g_sites)
		if (s.at >= PUSH_PEDS_SIDE_CALL && s.at <= PUSH_PEDS_EXTINGUISH_CALL)
			pushOk = pushOk && RelCallAt(Ptr<uint8_t>(s.at), s.at, s.engine);

	size_t taken = 0;
	for (Site &s : g_sites) {
		const bool push = s.at >= PUSH_PEDS_SIDE_CALL && s.at <= PUSH_PEDS_EXTINGUISH_CALL;
		if (!s.taken && (!push || pushOk))
			s.taken = RedirectCall(s.at, s.engine, s.ours);
		if (s.taken)
			++taken;
		else
			Log("emergency: FAILED to take the call at 0x%08X (%s); that stays this "
			    "machine's own", static_cast<unsigned>(s.at), s.what);
	}
	Log("emergency: took %u of %u call sites for the medics and the fire trucks",
	    static_cast<unsigned>(taken), static_cast<unsigned>(sizeof g_sites / sizeof g_sites[0]));
	return taken == sizeof g_sites / sizeof g_sites[0];
}

void RemoveEmergencyHooks() {
	for (Site &s : g_sites)
		if (s.taken && RedirectCall(s.at, s.ours, s.engine))
			s.taken = false;
	g_reviveCount = 0;
	g_jetCount    = 0;
	g_push        = Push{};
}

void AddEmergencyToBridge(WorldBridge &bridge) {
	bridge.DrainMedicRevives    = &DrainMedicRevives;
	bridge.ReviveAmbientReplica = &ReviveAmbientReplica;
	bridge.ReviveHostedPed      = &ReviveHostedPed;
	bridge.OfferCorpseToMedics  = &OfferCorpseToMedics;
	bridge.cannon.DrainLocalJets = &DrainLocalJets;
	bridge.cannon.SprayCopy      = &SprayCopy;
}

} // namespace coopiii::game
