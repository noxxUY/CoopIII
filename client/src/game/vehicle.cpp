#include "vehicle.h"

#include "addresses.h"
#include "adopt.h"
#include "boat.h"
#include "cardamage.h"
#include "carlife.h"
#include "carstatus.h"
#include "combat.h"
#include "horn.h"
#include "observed.h"
#include "ped.h"
#include "pedanim.h"
#include "population.h"
#include "siren.h"
#include "wreckqueue.h"
#include "../hook/hook.h"
#include "../log.h"
#include "../quat.h"

namespace coopiii::game {

namespace {

void *PlayerPed() {
	return Func<void *(__cdecl *)()>(FindPlayerPed)();
}

// How far to the side of the car to put the local player when the session
// takes a car off him. A GTA III car is about two metres across, so this
// clears the body and no more - the same step game/seat.cpp puts a passenger
// down with, and for the same reason.
constexpr float PED_STEP_OUT_M = 2.0f;

// The vehicle the local player is in, or null. Reads CPed::m_pMyVehicle, but
// only after CPed::bInVehicle confirms the pointer means anything -
// m_pMyVehicle is left stale after an exit, so trusting it alone reports a
// player still driving a car they got out of two streets ago.
void *PlayerVehicle() {
	void *const ped = PlayerPed();
	if (!ped)
		return nullptr;
	// bInVehicle is a plain bool in re3 (Ped.h:455), not a bitfield bit.
	if (!Field<bool>(ped, offs::PED_IN_VEHICLE))
		return nullptr;
	return Field<void *>(ped, offs::PED_MY_VEHICLE);
}

// Vehicle pool handles, same idea as RemotePlayer::poolHandle for peds:
// CPool::GetAt compares the whole flags byte, whose top bit is the slot's
// free flag, so a handle stops resolving the instant the engine deletes the
// vehicle - reused slot or not. A raw pointer would keep resolving into
// whatever took the slot, which is how you end up writing a network packet
// onto some random traffic car.
int32_t VehicleRef(void *vehicle) {
	return Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(vehicle);
}

void *VehicleFromRef(int32_t ref) {
	if (ref < 0)
		return nullptr;
	return Func<void *(__cdecl *)(int32_t)>(CPools__GetVehicle)(ref);
}

void SetBit(void *object, size_t offset, uint8_t mask, bool on) {
	uint8_t &byte = Field<uint8_t>(object, offset);
	byte = static_cast<uint8_t>(on ? (byte | mask) : (byte & ~mask));
}

bool GetBit(void *object, size_t offset, uint8_t mask) {
	return (Field<uint8_t>(object, offset) & mask) != 0;
}

// Writes the rotation and position into the vehicle's CMatrix and pushes it
// to the RenderWare frame.
//
// The push isn't optional, and it's the single most expensive lesson in this
// codebase so far: CEntity::CreateRwObject attaches the CMatrix to the
// clump's frame matrix exactly once, at attach time, and they're separate
// memory after that. Write the matrix and stop there and the visual stays
// at the origin.
void PlaceVehicle(void *vehicle, const Vec3 &pos, const Quat &rot, bool inWorld) {
	using ThisFn = void(__thiscall *)(void *);

	void *const matrix = reinterpret_cast<uint8_t *>(vehicle) + offs::MATRIX;

	// MATRIX_RIGHT/FWD/UP are offsets from the ENTITY, not the matrix -
	// MATRIX_RIGHT is 0x04 and so is MATRIX, since the matrix begins with its
	// right row. Use `matrix` as the base here and the 4 gets added twice,
	// shifting every basis vector one float along - scrambles the rotation
	// while leaving the position (an absolute offset) fine. Symptom: a car
	// in exactly the right place, lying on its side. This is what ped.cpp's
	// header means by "a field offset that is off by four does not crash".
	Vec3 right, forward, up;
	AxesFromQuat(rot, right, forward, up);
	WriteVec3(vehicle, offs::MATRIX_RIGHT, right);
	WriteVec3(vehicle, offs::MATRIX_FWD, forward);
	WriteVec3(vehicle, offs::MATRIX_UP, up);

	// Clamped for the same reason the ped's position is: the sector re-file
	// below turns x and y into subscripts into CWorld::ms_aSectors with no
	// bounds check of its own, and these floats arrived over a socket.
	float *const p = &Field<float>(vehicle, offs::POSITION);
	p[0]           = ClampToWorld(pos.x);
	p[1]           = ClampToWorld(pos.y);
	FiniteOr(pos.z, 0.0f, p[2]);

	Func<ThisFn>(CMatrix__UpdateRW)(matrix);
	Func<ThisFn>(CEntity__UpdateRwFrame)(vehicle);
	if (inWorld)
		Func<ThisFn>(CPhysical__RemoveAndAdd)(vehicle);
}

uint8_t VehicleStatus(void *vehicle) {
	// Bits 3-7 of the entity flags byte. Same layout the spawn writes
	// STATUS_ABANDONED into, read back the way CCarCtrl::PossiblyRemoveVehicle
	// reads it: `shr dl,3`.
	return static_cast<uint8_t>(Field<uint8_t>(vehicle, offs::ENTITY_FLAGS) >>
	                            ENTITY_STATUS_SHIFT);
}

bool IsWrecked(void *vehicle) {
	return VehicleStatus(vehicle) == ENTITY_STATUS_WRECKED;
}

// CVehicle::m_vehType, which the constructor stamps and never changes. Every
// CAutomobile-only write in this file asks this first (game/boat.h).
int32_t VehicleTypeOf(void *vehicle) {
	return Field<int32_t>(vehicle, offs::VEH_TYPE);
}

// Holds the five-second fire timer at zero, on whichever member this vehicle
// keeps it in. A car's is at +0x530 and a boat's at +0x2CC, and the car's
// offset is past the end of a boat (addresses.h, "boats").
void HoldFireTimer(void *vehicle) {
	const size_t at = FireBlowUpTimerOffset(VehicleTypeOf(vehicle));
	if (at)
		Field<float>(vehicle, at) = 0.0f;
}

// CREATE_CAR's class choice for a model id off the wire. Reads the model info
// itself rather than calling CModelInfo::IsBoatModel, which has no null check.
VehicleBuild VehicleBuildForModel(uint32_t model) {
	void *const info = VehicleModelInfo(model);
	if (!info)
		return VehicleBuild::None;
	return VehicleBuildFor(true, Field<uint8_t>(info, offs::MODELINFO_TYPE),
	                       Field<int32_t>(info, offs::MODELINFO_VEHICLE_TYPE));
}

// ---- which cars this machine is only watching ------------------------------
//
// The BlowUpCar detour gets a bare CVehicle* and has to answer two questions
// about it: is this one of ours to decide, and if not, whose is it. Nothing on
// the object itself says so - bIsLocked and VehicleCreatedBy are shared with
// every mission car the campaign ever made - so CoopIII keeps its own table,
// filled at the two moments a remote car definitely starts and stops existing.
//
// A row holds a pool HANDLE, never a pointer, and the lookup resolves it. That
// is not tidiness. CCarCtrl::PossiblyRemoveVehicle takes a wreck out of the
// pool 60 seconds later without asking anybody (addresses.h), and the slot is
// then free for the engine's own traffic - so a row holding a raw pointer would
// start matching an unrelated taxi, and the detour would refuse to let that
// taxi blow up for the rest of the session. CPool::GetAt compares the whole
// flags byte including the slot's free bit, so a dead handle resolves to null
// and can never match anything.
//
// Every row in the session's vehicle roster that has a CVehicle here has a row
// in this table too, whoever built the car: SpawnRemoteVehicle for a replica,
// AdoptPromotedCar for traffic that became a session car, and
// AdoptClaimedVehicle for a car the local player claimed out of his own
// street. The last one is the car that goes on being ours after somebody else
// takes it, and without a row it was a car another player drives that this
// engine still dented and blew up by itself. game/observed.h has the table.
constexpr size_t MAX_OBSERVED = 64;   // Client::MAX_REMOTE_VEHICLES

using Observed = ObservedRow;

ObservedTable<MAX_OBSERVED> g_observed;

Observed *FindObserved(const void *vehicle) {
	return g_observed.Find(vehicle, &VehicleFromRef);
}

void RememberObserved(int32_t handle, uint16_t netId) {
	if (g_observed.Remember(handle, netId, &VehicleFromRef))
		return;
	// Full means the roster is already full, so this cannot happen without a
	// bug upstream. Say so once rather than silently watching a car nobody
	// can refuse to destroy.
	static bool said = false;
	if (!said) {
		said = true;
		Log("bridge: the observed-vehicle table is full at %u; vehicle %u will "
		    "not be protected from this machine's own engine",
		    static_cast<unsigned>(MAX_OBSERVED), netId);
	}
}

void ForgetObserved(uint16_t netId) { g_observed.Forget(netId); }

// A session car with nobody at the wheel and nobody settling it, us included.
// The caller has already ruled out the local player driving it.
bool Unheld(const Observed *o) {
	return o != nullptr && o->driverPlayerId == 0xFF &&
	       o->custodianPlayerId == 0xFF && !o->weSettle;
}

// Who else holds a car is written by NoteVehicleHolders, below, and nothing
// else. It used to be written from CorrectRemoteVehicle, ApplyRemoteVehicle
// and RestRemoteVehicle, which never run for a car this machine drives or
// settles - so the row kept naming whoever held the car before us, and the
// detours refused our own custody car on their word.

// ---- which cars are replicas of somebody else's traffic --------------------
//
// The same idea as the Observed table, for the ambient roster. The detours
// get a bare CVehicle*, and to forward a hit they need the netId as well as
// the fact that it's a replica. So SpawnAmbientCarReplica writes a row and
// the despawn, the promotion and a reaped handle clear it.
//
// Handles, not pointers, for the reason the Observed table gives. A row whose
// handle has stopped resolving can never match, and RememberReplica reuses it.
constexpr size_t MAX_REPLICAS = 128;   // Client::MAX_REMOTE_CARS

struct ReplicaRow {
	int32_t  handle = -1;
	uint16_t netId  = 0;
	// Its host's ped rows put somebody in the driver's seat, and its siren is
	// on. Rewritten every frame by CorrectAmbientCarReplica; only the siren
	// detour reads it.
	bool     driverSaid = false;
};

ReplicaRow g_replicas[MAX_REPLICAS];

const ReplicaRow *FindReplica(const void *vehicle) {
	if (!vehicle)
		return nullptr;
	for (const ReplicaRow &r : g_replicas) {
		if (r.handle < 0)
			continue;
		if (VehicleFromRef(r.handle) == vehicle)
			return &r;
	}
	return nullptr;
}

void ForgetReplica(uint16_t netId) {
	for (ReplicaRow &r : g_replicas)
		if (r.handle >= 0 && r.netId == netId)
			r = ReplicaRow{};
}

void RememberReplica(int32_t handle, uint16_t netId) {
	if (handle < 0)
		return;
	ForgetReplica(netId);
	for (ReplicaRow &r : g_replicas)
		if (r.handle < 0 || VehicleFromRef(r.handle) == nullptr) {
			r = ReplicaRow{handle, netId};
			return;
		}
	static bool said = false;
	if (!said) {
		said = true;
		Log("bridge: the traffic replica table is full at %u; replica %u will "
		    "take damage here like a car nobody owns",
		    static_cast<unsigned>(MAX_REPLICAS), netId);
	}
}

// Set while CoopIII is driving BlowUpCar rather than the engine. Same shape
// and same job as combat.cpp's g_replaying: it is what lets the one blast
// that *is* authorised through the rule written to stop all the others.
bool g_replayingBlast = false;

struct BlastGuard {
	BlastGuard() { g_replayingBlast = true; }
	~BlastGuard() { g_replayingBlast = false; }
};

// The same thing for the hit this machine is applying to its OWN car off
// somebody else's report. Separate from the blast flag rather than shared:
// the two guard different functions, and a single flag would mean one of them
// could open the other's door.
bool g_replayingHit = false;

struct HitGuard {
	HitGuard() { g_replayingHit = true; }
	~HitGuard() { g_replayingHit = false; }
};

// One line each, first time only. A car that quietly fails to explode is
// indistinguishable from a feature nobody built - that lesson cost this
// project a session in M4 - so each link of the chain says itself once.
bool g_saidBlastSent    = false;
bool g_saidBlastApplied = false;
bool g_saidBlastRefused = false;
bool g_saidSettleBlastRefused = false;
bool g_saidWreckReaped  = false;
bool g_saidTookOver     = false;
bool g_saidBurning      = false;
bool g_saidReplicaBlastRefused = false;
bool g_saidCarHitSent          = false;
bool g_saidCarHitApplied       = false;
bool g_saidCarHitGone          = false;
bool g_saidHornSent            = false;
bool g_saidHornReplayed        = false;

// How many more snapshots the horn bit rides after the horn stopped. One
// local car at a time, so one of these (game/horn.h, HornOnWire).
HornTail g_hornTail;

// The local player's car blowing up, waiting to be picked up by Client.
//
// Four is plenty: this is one event per car per life, and the local player
// only has one car. Oldest drops rather than newest, same as the combat
// queue - though if this ever overflows something upstream is very wrong.
constexpr uint8_t MAX_PENDING_BLASTS = 4;
LocalVehicleBlast g_blasts[MAX_PENDING_BLASTS];
uint8_t           g_blastHead  = 0;
uint8_t           g_blastCount = 0;

void PushLocalBlast(const LocalVehicleBlast &blast) {
	if (g_blastCount == MAX_PENDING_BLASTS) {
		g_blastHead = static_cast<uint8_t>((g_blastHead + 1) % MAX_PENDING_BLASTS);
		--g_blastCount;
	}
	g_blasts[(g_blastHead + g_blastCount) % MAX_PENDING_BLASTS] = blast;
	++g_blastCount;
}

// ---- the name the map gives a parked car -----------------------------------
//
// docs/roadmap.md 5.8. A parked car exists on every machine and nobody
// created it, so there is no creator to announce it and nothing to allocate a
// netId against. docs/population.md 1.2 says identity has to be handed out
// rather than chosen - and for a parked car the MAP hands it out.
//
// Every PARKED_VEHICLE in GTA III comes out of a car generator, and the
// generators live in one fixed array filled from the IPLs in file order. Same
// files, same order, same index, on every machine, with no round trip.
// addresses.h, "parked cars, and the name the map already gives them", has
// the disassembly that pins the array, its stride and its bound.

void *CarGeneratorAt(uint32_t index) {
	return reinterpret_cast<uint8_t *>(CTheCarGenerators__CarGeneratorArray) +
	       static_cast<size_t>(index) * offs::SIZEOF_CARGENERATOR;
}

uint32_t LiveCarGeneratorCount() {
	const int32_t n = *reinterpret_cast<int32_t *>(
	    CTheCarGenerators__NumOfCarGenerators);
	if (n <= 0)
		return 0;
	return static_cast<uint32_t>(n);
}

// The count as it was when the world had finished loading, which is the range
// of indices that mean the same thing on two machines.
//
// The script can create car generators at runtime and they are appended to
// the same array, but only the host runs the script (docs/campaign.md) - so
// from the first script generator onwards the two arrays stop agreeing and an
// index past this point names two different things. Snapshotted once, and
// re-snapshotted if the live count ever drops, which is a level or save
// reload rebuilding the array underneath us.
uint32_t g_cargenBaseline = 0;
bool     g_saidCargenBaseline = false;

uint32_t MapCarGeneratorCount() {
	const uint32_t live = LiveCarGeneratorCount();
	if (g_cargenBaseline == 0 || live < g_cargenBaseline) {
		g_cargenBaseline = live;
		if (!g_saidCargenBaseline && live > 0) {
			g_saidCargenBaseline = true;
			Log("vehicle: the map has %u car generators; parked cars are named "
			    "by their index in that range",
			    live);
		}
	}
	return g_cargenBaseline;
}

// Which generator, if any, currently owns this car. -1 for every other car in
// the game, which is what makes this the filter as well as the lookup: a
// traffic car, a replica and a claimed car all fail it, because none of them
// is any generator's.
//
// The comparison is against CPools::GetVehicleRef, and that it is *the same
// encoding* is proved rather than assumed: CPools::GetVehicleRef is four
// instructions around the same CPool::GetIndex the generator calls, with the
// same pool in ecx (addresses.h). A ref carries the slot's flags byte, so a
// generator whose car has been reaped does not match whatever took the slot.
int32_t FindCarGeneratorFor(void *vehicle) {
	if (!vehicle)
		return -1;
	const int32_t  ref = VehicleRef(vehicle);
	const uint32_t n   = MapCarGeneratorCount();
	for (uint32_t i = 0; i < n; ++i)
		if (Field<int32_t>(CarGeneratorAt(i), offs::CARGEN_VEHICLE_HANDLE) == ref)
			return static_cast<int32_t>(i);
	return -1;
}

// The car a generator is currently holding, or null. Goes through
// CPools::GetVehicle, so a handle whose slot has been freed resolves to null
// rather than to whatever the engine put there next.
void *CarFromGenerator(uint32_t index) {
	if (index >= MapCarGeneratorCount())
		return nullptr;
	const int32_t handle =
	    Field<int32_t>(CarGeneratorAt(index), offs::CARGEN_VEHICLE_HANDLE);
	if (handle < 0)
		return nullptr;
	return VehicleFromRef(handle);
}

// Unowned cars this machine's engine destroyed, waiting for Client to put
// them on the wire.
//
// It used to hold eight, sized for a rocket into a row of parked cars.
// BANGBANGBANG wrecks the whole pool in one frame and the ninth car onwards
// fell off the front, intact on every other screen. So it holds the pool:
// nothing can wreck more cars in a frame than there are (game/wreckqueue.h).
//
// The transform is zeroed rather than left unread. A parked car is where the
// map put it on every machine and the receiver never looks at these bytes,
// but a packet with uninitialised bytes on it is a packet nobody can read a
// capture of.
WreckQueue<VEHICLE_POOL_SIZE> g_unowned;
static_assert(WreckQueue<VEHICLE_POOL_SIZE>::kCapacity >= size_t(VEHICLE_POOL_SIZE),
              "one frame can wreck every car in the pool");

void PushUnownedBlast(const UnownedVehicleKey &key) {
	// Already queued: two calls for one car in one drain window. The queue
	// refuses the second, which keeps it for distinct cars.
	UnownedBlast blast{};
	blast.key = key;
	g_unowned.Push(blast);
}

// Hits the local player landed on cars other people are driving, waiting for
// Client to name them and put them on the wire.
//
// Sixteen, and it is the widest queue in this file on purpose. A blast is one
// event per car per life; a burst from an Uzi is one of these per round, and
// several of them land inside one frame at 60 FPS. Oldest drops rather than
// newest, the same as the combat queue: if this ever overflows, the rounds
// worth keeping are the ones still in the air.
constexpr uint8_t MAX_PENDING_HITS = 16;
VehicleHitBody    g_hits[MAX_PENDING_HITS];
uint8_t           g_hitHead  = 0;
uint8_t           g_hitCount = 0;

// Not deduplicated, and that is a decision rather than an omission. Two rounds
// into the same car are two hits and the engine would apply both in single
// player; collapsing them here would make a shotgun cost what a pistol costs.
// Contrast PushUnownedBlast above, which does dedupe, because a car can only
// be destroyed once.
void PushVehicleHit(const VehicleHitBody &hit) {
	if (g_hitCount == MAX_PENDING_HITS) {
		g_hitHead = static_cast<uint8_t>((g_hitHead + 1) % MAX_PENDING_HITS);
		--g_hitCount;
	}
	g_hits[(g_hitHead + g_hitCount) % MAX_PENDING_HITS] = hit;
	++g_hitCount;
}

// The same queue for hits on traffic replicas. Separate because the two go out
// as different packets to owners the server finds by different rules.
VehicleHitBody g_carHits[MAX_PENDING_HITS];
uint8_t        g_carHitHead  = 0;
uint8_t        g_carHitCount = 0;

void PushCarHit(const VehicleHitBody &hit) {
	if (g_carHitCount == MAX_PENDING_HITS) {
		g_carHitHead = static_cast<uint8_t>((g_carHitHead + 1) % MAX_PENDING_HITS);
		--g_carHitCount;
	}
	g_carHits[(g_carHitHead + g_carHitCount) % MAX_PENDING_HITS] = hit;
	++g_carHitCount;
}

// And rounds our own pedestrians landed on a car another player drives or
// settles (protocol.h, C_NpcVehicleHit). A third queue because the packet
// names the pedestrian as well.
NpcVehicleHit g_npcHits[MAX_PENDING_HITS];
uint8_t       g_npcHitHead  = 0;
uint8_t       g_npcHitCount = 0;

void PushNpcVehicleHit(const NpcVehicleHit &hit) {
	if (g_npcHitCount == MAX_PENDING_HITS) {
		g_npcHitHead = static_cast<uint8_t>((g_npcHitHead + 1) % MAX_PENDING_HITS);
		--g_npcHitCount;
	}
	g_npcHits[(g_npcHitHead + g_npcHitCount) % MAX_PENDING_HITS] = hit;
	++g_npcHitCount;
}

// One detour per function. CAutomobile::BlowUpCar and CBoat::BlowUpCar are
// genuinely different functions (addresses.h) and a detour on one catches only
// that one, so both are hooked and both land in the same handler below.
Detour g_blowUpCar;
Detour g_blowUpBoat;
Detour g_inflictDamage;
Detour g_sirenAudio;   // cAudioManager::ProcessVehicleSirenOrAlarm, game/siren.h
Detour g_carAiUpdate;  // CCarAI::UpdateCarAI, game/carstatus.h
Detour g_carAiSteer;   // CCarCtrl::SteerAICarWithPhysics, game/carstatus.h
Detour g_boatAiSteer;  // CCarCtrl::SteerAIBoatWithPhysics, game/carstatus.h

// __thiscall void CAutomobile::BlowUpCar(CEntity *culprit).
//
// The same __fastcall trick combat.cpp uses on CPed::InflictDamage: it is how
// a free function receives `this` in ecx with the stack argument left exactly
// where __thiscall put it.
using BlowUpThisFn = void(__thiscall *)(void *, void *);
using BlowUpHookFn = void(__fastcall *)(void *, void *, void *);

// Shared by both detours. `original` is whichever trampoline the call came
// through, so a boat's explosion goes back into CBoat's body and a car's into
// CAutomobile's - the one thing the two must not share.
void BlowUpCarCommon(Detour &detour, void *self, void *culprit) {
	if (!self || g_replayingBlast) {
		detour.Original<BlowUpThisFn>()(self, culprit);
		return;
	}

	// Are we the ones driving it? The driver's seat, not "is the local player
	// inside" - a passenger is not an owner. Same test SampleLocalVehicle and
	// CorrectRemoteVehicle's guard use, and it has to be the same one or this
	// machine ends up owning a car for one purpose and not the other.
	//
	// The null check is not paranoia: with no player ped, a car with an empty
	// driver's seat would compare equal to it, and every piece of traffic
	// that blew up during a load would be announced as ours.
	void *const localPed = PlayerPed();
	const bool  weDrive =
	    localPed != nullptr && Field<void *>(self, offs::VEH_DRIVER) == localPed;

	// Somebody else's car. Refused outright, because a wreck is a place as
	// well as an event and an observer has neither the moment nor the
	// position its owner will pick. Their machine is already deciding; their
	// C_VehicleBlowUp is what brings it here.
	//
	// Asked only when we are NOT the one driving. A car the session still
	// thinks a remote player owns, that the local player has since got into,
	// is ours in every way that matters to this machine's engine - and
	// refusing to let it explode under its actual driver would be a worse
	// symptom than the one this whole change is about. The session's idea of
	// who owns it catches up through the ownership handoff, which does not
	// exist yet (docs/roadmap.md M2, docs/protocol.md §1.11.5).
	//
	// The fire timer gets wound back at the same time. Without that the
	// engine retries this every single frame for as long as the car burns,
	// and CAutomobile::ProcessControl pays the local player
	// AwardMoneyForExplosion before each attempt (addresses.h). vehicle.cpp
	// holds the timer at zero for exactly this case anyway; this is the
	// backstop for the other two callers, CVehicle::InflictDamage and
	// ProcessDelayedExplosion, which no timer can head off.
	// Set when this is a car the session has a row for and records no driver
	// for. INVALID_NETID otherwise, including for the car we are driving
	// ourselves - that one goes out as C_VehicleBlowUp.
	uint16_t sessionNetId = INVALID_NETID;

	// A replica of somebody else's traffic is refused the same way, and for
	// the same reason: its host decides when it's finished, and that decision
	// arrives as C_UnownedBlowUp and comes back in through
	// BlowUpCarAsOwnerSaid. Letting this one through wrecked the car on this
	// screen only and killed its driver replica on the way, through
	// CPed::SetDead at 0x0053BDCD, which no SetDie guard can see.
	//
	// A car another player is settling is refused too. Their engine is the
	// only one simulating it until they hand it back, and their wreck reaches
	// us as C_UnownedBlowUp through WreckSessionVehicle, under the replay
	// guard - so it's a wreck here once and isn't counted toward a rampage
	// twice.
	const Observed *const o        = weDrive ? nullptr : FindObserved(self);
	const bool            driven   = o != nullptr && o->driverPlayerId != 0xFF;
	const bool            settling = o != nullptr && !driven &&
	                                 o->custodianPlayerId != 0xFF;
	const ReplicaRow     *replica  =
	    (weDrive || driven || settling) ? nullptr : FindReplica(self);

	if (!MayBlowUpCar(ClassifyCar(false, weDrive, driven, settling,
	                              replica != nullptr, Unheld(o)))) {
		// Through HoldFireTimer, because this detour is also the boat's and a
		// boat's timer is not at the car's offset - it reads the object's own
		// m_vehType, so it is right whichever detour brought us here.
		HoldFireTimer(self);
		if (driven && !g_saidBlastRefused) {
			g_saidBlastRefused = true;
			Log("bridge: refused to blow up vehicle %u - player %u is driving "
			    "it, so it is theirs to destroy",
			    o->netId, o->driverPlayerId);
		}
		if (settling && !g_saidSettleBlastRefused) {
			g_saidSettleBlastRefused = true;
			Log("bridge: refused to blow up vehicle %u - player %u is settling "
			    "it, so until they hand it back it is theirs to destroy",
			    o->netId, o->custodianPlayerId);
		}
		if (replica && !g_saidReplicaBlastRefused) {
			g_saidReplicaBlastRefused = true;
			Log("bridge: refused to blow up traffic replica %u - the machine "
			    "hosting it decides that, and says so with C_UnownedBlowUp",
			    replica->netId);
		}
		return;
	}

	if (o) {
		// A synced car with nobody in it is nobody's, so this machine keeps
		// its ordinary behaviour and lets the engine destroy it - and then
		// says so, below, because until now nobody did. That is
		// docs/roadmap.md 5.8's literal case: a car somebody claimed, drove,
		// parked and walked away from, blown up with no driver to report it
		// and a healthy row kept for it in the session forever.
		sessionNetId = o->netId;
	}

	// Sampled before the call, because BlowUpCar is the last moment the car
	// is where the owner's own physics left it - it adds 0.13 to the vertical
	// speed on the way through and the explosion does the rest.
	void *const       ours = weDrive ? self : nullptr;
	LocalVehicleBlast blast{};
	if (ours) {
		blast.pos = ReadVec3(ours, offs::POSITION);
		blast.rot = QuatFromAxes(ReadVec3(ours, offs::MATRIX_RIGHT),
		                         ReadVec3(ours, offs::MATRIX_FWD),
		                         ReadVec3(ours, offs::MATRIX_UP));
	}

	// Which generator's car this is, asked BEFORE the call and not after.
	// CAutomobile::BlowUpCar does not take the car out of the pool, but the
	// answer is a property of the car at the moment it was destroyed and
	// reading it first is what makes that true rather than nearly true.
	//
	// Not asked for a car we are driving: that one goes out as
	// C_VehicleBlowUp with a netId and a transform, and asking would be a
	// hundred-and-sixty-entry scan on the path that does not need it. In
	// practice it would not match either - CCarGenerator::Process hands its
	// car over the moment the status reads STATUS_PLAYER, which is the engine
	// itself agreeing that a parked car somebody got into stopped being
	// parked.
	const int32_t cargen = (ours || sessionNetId != INVALID_NETID)
	                           ? -1
	                           : FindCarGeneratorFor(self);

	detour.Original<BlowUpThisFn>()(self, culprit);

	// A car nobody owns just became a wreck here. docs/roadmap.md 5.8.
	//
	// Announced, and NOT suppressed, which is the decision this whole change
	// turns on. An observer is refused a remote player's car above, because
	// that car's owner is already deciding and an observer has neither the
	// moment nor the position they will pick. This car has no owner and no
	// transform to disagree about - it is where the map put it on every
	// machine - so there is nothing for two machines to get differently, and
	// the usual cause is an explosion that was replayed on both of them
	// anyway. Holding it back would mean waiting for a packet to tell us
	// something our own engine had just worked out correctly.
	//
	// So everybody who was there says so, the server keeps the first and
	// drops the rest, and anyone who was not there gets told.
	if (!ours && IsWrecked(self)) {
		UnownedVehicleKey key{};
		key.pad = 0;
		// A synced car with no driver takes precedence over a generator
		// match, and in practice only one of the two is ever set: a claimed
		// car is CoopIII's own object and no generator's, and the engine
		// releases a generator's car the moment somebody gets into it. The
		// order is written down anyway, because "whichever matched" is not a
		// rule anybody can check later.
		bool named = false;
		if (sessionNetId != INVALID_NETID) {
			key.kind = UNOWNED_SESSION;
			key.id   = sessionNetId;
			named    = true;
		} else if (cargen >= 0) {
			key.kind = UNOWNED_PARKED;
			key.id   = static_cast<uint16_t>(cargen);
			named    = true;
		}
		// Anything else is shared traffic, and this is not the seam that
		// reports it. A hosted traffic car's wreck is noticed by the status
		// poll in game/population.cpp and announced as UNOWNED_AMBIENT by
		// the machine that generated it; a replica is never announced at
		// all. Both halves of roadmap.md 5.8 are closed.
		if (named)
			PushUnownedBlast(key);
	}

	// Announced only if it actually happened. BlowUpCar returns without doing
	// anything when bCanBeDamaged is clear, which the campaign uses during
	// cutscenes, and announcing a blast that did not happen would wreck the
	// car on every screen but this one.
	if (ours && IsWrecked(ours)) {
		PushLocalBlast(blast);
		if (!g_saidBlastSent) {
			g_saidBlastSent = true;
			Log("bridge: our car blew up at %.1f %.1f %.1f; telling the session",
			    blast.pos.x, blast.pos.y, blast.pos.z);
		}
	}
}

void __fastcall HookedBlowUpCar(void *self, void * /*edx*/, void *culprit) {
	BlowUpCarCommon(g_blowUpCar, self, culprit);
}

void __fastcall HookedBlowUpBoat(void *self, void * /*edx*/, void *culprit) {
	BlowUpCarCommon(g_blowUpBoat, self, culprit);
}

// ---- a hit on a car somebody else is driving, or hosting -------------------
//
// __thiscall void CVehicle::InflictDamage(CEntity *culprit, eWeaponType,
//                                         float damage). ret 0Ch.
//
// Written __fastcall for the same reason every other detour in this codebase
// is: it is how a free function receives `this` in ecx with the three stack
// arguments left exactly where __thiscall put them, and both conventions have
// the callee clean them. The signature, the convention and the argument count
// were re-read out of the retail image for this change rather than inherited -
// addresses.h carries the transcription, including the twelve bytes of
// alignment padding that prove 0x00551950 is a function start and the `ret 0Ch`
// at every one of the seven exits that proves there are three arguments.
//
// This detour is to a car what combat.cpp's CPed::InflictDamage detour is to a
// pedestrian, and it exists for a reason that is sharper here than there.
//
// **The proof flags were never going to be the mechanism.** CoopIII sets
// exactly one on an observed car - bCollisionProof, in SetVehicleObserved
// below - and it cannot set the rest: bExplosionProof would stop a replayed
// blast reaching a car it is supposed to reach identically on every machine.
// Even all five would not close it, because the flags sit inside a switch on
// the damage cause and four of that switch's arms check nothing at all
// (0x00551A10, reached for causes 12, 14, 15, 17 and everything from 20 up).
// So the rule is stated here, once, positively: nothing on this machine may
// take health off a car this machine does not drive, and anything the local
// player did on purpose becomes a packet instead.
//
// **A blast is not routed through here and must not be.** An explosion is
// replayed on every machine at a position everybody agreed on, and
// CWorld::TriggerExplosionSectorList's damage multiplier is a function of the
// blast position and the car's position and nothing else - so the owner's own
// engine puts its own car in its own blast and decides what it costs, and if
// that kills it the wreck travels on C_VehicleBlowUp with the transform its
// physics chose. Forwarding it as damage as well would apply it twice, which
// is the argument IsForwardableDamage already makes for a player and for a
// pedestrian. What is new for a car is the other half: the observer must also
// *refuse* it locally, because unlike a replica ped a replica car is not
// explosion-proof and unlike a player its copy cannot then blow up - the
// BlowUpCar detour above refuses that - so it would sit at zero health, on
// fire, forever. That is §1.11.1's bug exactly.
//
// A traffic replica gets the same treatment with its host in the driver's
// place (docs/protocol.md §1.23). Both detours refuse it together, for the
// reason just given, and CorrectAmbientCarReplica holds the host's health on
// it every frame as a backstop for the writers that never call this function:
// the upside-down drain at 0x0052F472, the burning-occupant write of 75.0f at
// 0x00479959 and the engine-status drain at 0x005347E0.
using InflictDamageThisFn = void(__thiscall *)(void *, void *, uint32_t, float);
using InflictDamageHookFn = void(__fastcall *)(void *, void *, void *, uint32_t,
                                               float);

bool g_saidHitSent         = false;
bool g_saidNpcHitSent      = false;
bool g_saidUnheldHitSent   = false;
bool g_saidHitNotForwarded = false;
bool g_saidHitApplied      = false;
bool g_saidHitNoMove       = false;
bool g_saidHitGone         = false;
bool g_saidReplayHitRefused = false;

void __fastcall HookedInflictDamage(void *self, void * /*edx*/, void *culprit,
                                    uint32_t weapon, float damage) {
	// Our own ApplyRemoteVehicleHit, putting somebody else's report into our
	// own car. It goes straight through: the car is ours, the decision was
	// ours to make, and the guard below would not catch it anyway - but a
	// replay that ran through the reporting arm would be this machine
	// forwarding a hit back at the machine that sent it.
	if (!self || g_replayingHit) {
		g_inflictDamage.Original<InflictDamageHookFn>()(self, nullptr, culprit,
		                                                weapon, damage);
		return;
	}

	// Whose car is it? Three kinds belong to another machine: a session car
	// the session says somebody else is driving (protocol 23), one somebody
	// else is settling after getting out of it (S_VehicleCustody), and a
	// replica of traffic somebody else hosts (§1.23). Everything else -
	// traffic we host, a parked car - is this engine's to damage, a replayed
	// blast damages every copy of those the same way, and roadmap.md §5.8's
	// wreck report carries what is left over.
	//
	// The custodian is in that list because its engine is the one simulating
	// the car and streaming its health, exactly like a driver's. Left out, a
	// shot here took health off our copy, the custodian's next snapshot put it
	// back, and a car that died here during the settle blew up on this screen
	// and went out as UNOWNED_SESSION while the custodian's copy was fine.
	//
	// A session car nobody is in or settling is Nobody, and it used to be
	// treated as ours. Its health is the session's last word and every machine
	// writes it back each frame, so a shot here lasted one frame and a parked
	// car could be emptied into without ever catching fire. Now our own hit
	// goes out and the server makes us the custodian, whose engine then keeps
	// the health and the fire timer for everybody. A blast still lands here.
	//
	// The local player at the wheel wins over all of them, the same as in
	// BlowUpCarCommon: a car the session hasn't caught up on yet is still
	// ours to this engine, and refusing damage under its actual driver would
	// be the worse bug.
	void *const           localPed = PlayerPed();
	const bool            weDrive =
	    localPed != nullptr && Field<void *>(self, offs::VEH_DRIVER) == localPed;
	const Observed *const o        = weDrive ? nullptr : FindObserved(self);
	const bool            driven   = o != nullptr && o->driverPlayerId != 0xFF;
	const bool            settling = o != nullptr && !driven &&
	                                 o->custodianPlayerId != 0xFF;
	const ReplicaRow     *replica  =
	    (weDrive || driven || settling) ? nullptr : FindReplica(self);
	const CarOwner owner =
	    ClassifyCar(false, weDrive, driven, settling, replica != nullptr, Unheld(o));

	// Our own car, hit by somebody else's replayed round. If they would have
	// forwarded a hit on it - the car we drive, the one we settle, our named
	// traffic - that packet is the hit, and this would be the same round a
	// second time (combat.h, ReplayedShotMayDamage). The sparks and the sound
	// are DoBulletImpact's own and were drawn before this call.
	if (owner == CarOwner::Local && ReplayingRemoteShot()) {
		bool       named  = false;
		const bool hosted = !weDrive && HostedCarFor(self, named);
		const ReplayTarget target =
		    ClassifyReplayCar(owner, weDrive, o != nullptr && o->weSettle, hosted, named);
		if (!ReplayedShotMayDamage(target, static_cast<uint8_t>(weapon))) {
			if (!g_saidReplayHitRefused) {
				g_saidReplayHitRefused = true;
				Log("vehicle: a replayed shot reached %s (cause %u, %.0f) and we did "
				    "not take the health. The shooter's %s for it is the hit that "
				    "counts",
				    target == ReplayTarget::CarWeDrive    ? "the car we are driving"
				    : target == ReplayTarget::CarWeSettle ? "the car we are settling"
				                                          : "one of our traffic cars",
				    weapon, damage,
				    target == ReplayTarget::NamedHostedCar ? "C_CarHit" : "C_VehicleHit");
			}
			return;
		}
	}

	// What the local player did deliberately is reported, the same test as
	// the pedestrian branch in combat.cpp, and so is a round one of our named
	// pedestrians fired at a car a player holds (DecideCarDamage says which).
	// Any other city NPC shooting a replica happened in one simulation only,
	// and forwarding it would have this machine's traffic shooting up another
	// machine's cars. Not into a wreck either: the owner's InflictDamage
	// leaves at 0x00551A10 for health <= 0.
	const bool ours     = localPed != nullptr && culprit == localPed;
	uint16_t   npcNetId = INVALID_NETID;
	const bool byNpc    = !ours && culprit && HostedPedNetIdFor(culprit, npcNetId);
	const CarDamageVerdict verdict = DecideCarDamage(
	    owner, ours, IsWrecked(self), static_cast<uint8_t>(weapon), byNpc);

	if (verdict == CarDamageVerdict::Apply) {
		g_inflictDamage.Original<InflictDamageHookFn>()(self, nullptr, culprit,
		                                                weapon, damage);
		// A blast on a car nobody holds: what it left stays, and nothing
		// else that lowers the health does (HealthToWrite).
		if (owner == CarOwner::Nobody)
			g_observed.NoteBlast(self, Field<float>(self, offs::VEH_HEALTH),
			                     &VehicleFromRef);
		return;
	}

	// Refused from here on. A traffic replica's hit goes to its host.
	if (owner == CarOwner::RemoteHost) {
		if (verdict == CarDamageVerdict::Forward) {
			VehicleHitBody hit{};
			hit.netId  = replica->netId;
			hit.weapon = static_cast<uint8_t>(weapon);
			hit.amount = damage;
			PushCarHit(hit);
			if (!g_saidCarHitSent) {
				g_saidCarHitSent = true;
				Log("vehicle: our first hit on traffic replica %u, %.0f with "
				    "cause %u, is refused here and goes to its host as C_CarHit",
				    replica->netId, damage, weapon);
			}
		}
		return;
	}

	// One of our pedestrians' rounds on a car a player holds goes to them on
	// its own packet, which never makes anybody a custodian.
	if (verdict == CarDamageVerdict::Forward && byNpc) {
		NpcVehicleHit hit{};
		hit.pedNetId     = npcNetId;
		hit.body.netId   = o->netId;
		hit.body.weapon  = static_cast<uint8_t>(weapon);
		hit.body.amount  = damage;
		PushNpcVehicleHit(hit);
		if (!g_saidNpcHitSent) {
			g_saidNpcHitSent = true;
			Log("vehicle: our first pedestrian's round on a car a player holds - "
			    "pedestrian net %u hit car net %u for %.0f with cause %u - is refused "
			    "here and goes to them as C_NpcVehicleHit", npcNetId, o->netId, damage,
			    weapon);
		}
		return;
	}

	// A driven car and a settling one both go out as C_VehicleHit. The server
	// sends it to the driver, or to the custodian when there is no driver
	// (Session::VehicleHitRecipient), so the packet doesn't have to say which.
	// With neither it makes us the custodian and sends it back
	// (Session::CustodyForHit).
	if (verdict == CarDamageVerdict::Forward) {
		VehicleHitBody hit{};
		hit.netId  = o->netId;
		hit.weapon = static_cast<uint8_t>(weapon);
		hit.amount = damage;
		PushVehicleHit(hit);

		if (owner == CarOwner::Nobody) {
			if (!g_saidUnheldHitSent) {
				g_saidUnheldHitSent = true;
				Log("vehicle: our first hit on car net %u, which nobody is driving "
				    "or settling, %.0f with cause %u, is refused here and goes out "
				    "as C_VehicleHit. The session makes us its custodian and hands "
				    "it back, so one engine keeps its health and its fire timer",
				    o->netId, damage, weapon);
			}
		} else if (!g_saidHitSent) {
			g_saidHitSent = true;
			Log("vehicle: our first hit on somebody else's car, net %u for %.0f "
			    "with cause %u, is on its way as C_VehicleHit. Player %u's "
			    "machine (%s) applies it through its own CVehicle::InflictDamage, "
			    "and whatever comes of it - the smoke, the dents, the wreck - "
			    "comes back on the packets that already carry those",
			    o->netId, damage, weapon,
			    static_cast<unsigned>(driven ? o->driverPlayerId
			                                 : o->custodianPlayerId),
			    driven ? "driving it" : "settling it");
		}
	} else if (ours && !IsWrecked(self) &&
	           !IsFireDamage(static_cast<uint8_t>(weapon)) && !g_saidHitNotForwarded) {
		// Cause 9 is left out: that is our own flame's fire burning our copy
		// of the car, and its ignition already went out (ReportOurFlameOnCar).
		g_saidHitNotForwarded = true;
		Log("vehicle: our hit on car net %u is refused here and not forwarded, "
		    "because cause %u is not one the shooter gets to decide "
		    "(combat.h, IsForwardableDamage). A blast gets there on its own, "
		    "because every machine replays it at the same place",
		    o->netId, weapon);
	}
}

// ---- extras ---------------------------------------------------------------
//
// The engine's own override, set immediately before the constructor and put
// back immediately after. addresses.h, "a vehicle's extra components", has
// the disassembly and the two traps; the one that makes the reset mandatory is
// that CreateInstance leaves the override alone for a model with no
// components at all, so the next car created on this machine would wear it.
void SetComponentsToUse(int8_t comp1, int8_t comp2) {
	auto *const p = reinterpret_cast<int8_t *>(CVehicleModelInfo__ms_compsToUse);
	p[0]          = comp1;
	p[1]          = comp2;
}

struct ComponentOverride {
	ComponentOverride(int8_t comp1, int8_t comp2) {
		SetComponentsToUse(comp1, comp2);
	}
	~ComponentOverride() {
		SetComponentsToUse(VEHICLE_COMPS_RANDOM, VEHICLE_COMPS_RANDOM);
	}
};

bool g_saidExtraRefused = false;

} // namespace

// Resolves the handle, and if the engine has taken the vehicle away, says
// so once and re-arms the spawn. Mirrors ResolveRemote in ped.cpp.
//
// Not file-private, unlike its ped counterpart - seating a remote ped needs
// both halves, and the ped half (CPed::WarpPedIntoCar) lives in ped.cpp.
void *ResolveRemoteVehicle(RemoteVehicle &vehicle) {
	if (vehicle.poolHandle < 0)
		return nullptr;
	const int32_t handle = vehicle.poolHandle;
	void *const   v      = VehicleFromRef(handle);
	if (!v) {
		ForgetObserved(vehicle.netId);
		vehicle.poolHandle   = -1;
		vehicle.appliedFlags = 0xFF;
		// Whatever this row was, it is not "one of our own engine's cars
		// that we are keeping a record of" any more - that car is gone. Any
		// replacement is a replica CoopIII built, which is ours to destroy
		// when the session ends. Clearing this with the handle keeps the two
		// facts from drifting apart. See RemoteVehicle::ours.
		vehicle.ours         = false;

		// A wreck is meant to go. CCarCtrl::PossiblyRemoveVehicle deletes any
		// STATUS_WRECKED vehicle a minute after it died, and that branch is
		// the one reaping site in the engine that tests neither bIsLocked nor
		// CanBeDeleted (addresses.h) - so CoopIII's two registration gates do
		// not hold against it and were never meant to. Respawning here would
		// put a brand new, undamaged car where a burnt one had just been
		// cleared away.
		if (vehicle.destroyed) {
			if (!g_saidWreckReaped) {
				g_saidWreckReaped = true;
				Log("bridge: vehicle %u was a wreck and the engine has cleared it "
				    "away; not respawning it",
				    vehicle.netId);
			}
			return nullptr;
		}

		Log("bridge: vehicle %u (handle %d) is gone from the pool; will respawn",
		    vehicle.netId, handle);
		vehicle.spawnPending = true;
	}
	return v;
}

bool LocalPlayerInVehicle() {
	return PlayerVehicle() != nullptr;
}

// Is the local player in this row's driver's seat, as the engine has it?
//
// Read off the car rather than off the ped on purpose: CPed::m_pMyVehicle is
// also set for a passenger and stays set through the whole get-in and get-out
// animation, and a passenger is not an owner. CVehicle::m_pDriver is the same
// thing CorrectRemoteVehicle has always tested, and this is that test given a
// name so both halves of the pair can make it.
//
// Deliberately NOT through ResolveRemoteVehicle. That one has a side effect -
// a handle the pool no longer honours re-arms the spawn and logs - and asking
// "are we driving this" must not create a car. The handle is resolved straight,
// and a handle that no longer resolves is simply not ours.
bool LocalDrivesVehicle(const RemoteVehicle &vehicle) {
	if (vehicle.poolHandle < 0)
		return false;
	void *const v = VehicleFromRef(vehicle.poolHandle);
	if (!v)
		return false;
	void *const ped = PlayerPed();
	return ped != nullptr && Field<void *>(v, offs::VEH_DRIVER) == ped;
}

// The pool ref of the car the local player is in, or -1.
//
// This is what tells a late joiner that the car it just got into is one the
// session already knows about. Without it the claim goes out with
// INVALID_NETID, the server hands back a second netId for a car it already
// has, and the joiner ends up driving one copy while observing the other -
// wheels turning, engine running, car pinned in place by the correction that
// puts an observed car back where the session last saw it.
//
// A ref rather than a pointer for the reason every handle here is a ref:
// CPool::GetAt tests the slot's free flag, so this stops resolving the
// instant the engine deletes the car, reused slot or not.
int32_t SampleLocalVehicleHandle() {
	void *const vehicle = PlayerVehicle();
	return vehicle ? VehicleRef(vehicle) : -1;
}

bool SampleLocalVehicleIdentity(VehicleIdentity &out) {
	void *const vehicle = PlayerVehicle();
	if (!vehicle)
		return false;
	if (Field<void *>(vehicle, offs::VEH_DRIVER) != PlayerPed())
		return false;

	uint16_t &modelId = out.modelId;
	uint8_t  &colour1 = out.colour1;
	uint8_t  &colour2 = out.colour2;
	Vec3     &pos     = out.pos;
	Quat     &rot     = out.rot;

	modelId = Field<uint16_t>(vehicle, offs::MODEL_INDEX);
	colour1 = Field<uint8_t>(vehicle, offs::VEH_COLOUR1);
	colour2 = Field<uint8_t>(vehicle, offs::VEH_COLOUR2);

	// m_aExtras, the record CVehicle::SetModelIndex made of what this
	// machine's CreateInstance rolled. Read rather than re-derived: the only
	// machine that knows which components this car has is the one whose
	// engine chose them.
	//
	// Clamped on the way out as well as on the way in. -1 is the engine's own
	// "nothing fitted" and anything else out of range would have to be memory
	// corruption, but it costs nothing to refuse to put it on the wire.
	const int comps = VehicleModelCompCount(modelId);
	out.extra1 = ClampVehicleExtra(Field<int8_t>(vehicle, offs::VEH_EXTRAS), comps);
	out.extra2 = ClampVehicleExtra(Field<int8_t>(vehicle, offs::VEH_EXTRAS + 1), comps);

	pos     = ReadVec3(vehicle, offs::POSITION);
	rot     = QuatFromAxes(ReadVec3(vehicle, offs::MATRIX_RIGHT),
	                       ReadVec3(vehicle, offs::MATRIX_FWD),
	                       ReadVec3(vehicle, offs::MATRIX_UP));
	return true;
}

bool SampleLocalVehicle(VehicleStateBody &out) {
	void *const vehicle = PlayerVehicle();
	if (!vehicle) {
		g_hornTail = HornTail{};
		return false;
	}

	// Only the driver is authoritative here. A passenger sees the same car
	// the driver does, and if it also sent state the two would fight it out
	// at 25 Hz.
	if (Field<void *>(vehicle, offs::VEH_DRIVER) != PlayerPed()) {
		g_hornTail = HornTail{};
		return false;
	}

	out = VehicleStateBody{};
	out.netId = 0;   // filled in by the caller: netIds are the server's

	out.pos = ReadVec3(vehicle, offs::POSITION);
	out.rot = QuatFromAxes(ReadVec3(vehicle, offs::MATRIX_RIGHT),
	                       ReadVec3(vehicle, offs::MATRIX_FWD),
	                       ReadVec3(vehicle, offs::MATRIX_UP));

	out.moveSpeed = ReadVec3(vehicle, offs::MOVE_SPEED);
	out.turnSpeed = ReadVec3(vehicle, offs::TURN_SPEED);

	out.steer  = Field<float>(vehicle, offs::VEH_STEER_ANGLE);
	out.gas    = Field<float>(vehicle, offs::VEH_GAS_PEDAL);
	out.brake  = Field<float>(vehicle, offs::VEH_BRAKE_PEDAL);
	out.gear   = Field<uint8_t>(vehicle, offs::VEH_CURRENT_GEAR);
	out.health = Field<float>(vehicle, offs::VEH_HEALTH);

	out.flags = 0;
	if (GetBit(vehicle, offs::VEH_FLAGS_A, offs::VEH_ENGINE_ON))
		out.flags |= VEH_ENGINE_ON;
	if (Field<bool>(vehicle, offs::VEH_SIREN_OR_ALARM))
		out.flags |= VEH_SIREN;
	if (GetBit(vehicle, offs::VEH_FLAGS_A, offs::VEH_LIGHTS_ON))
		out.flags |= VEH_LIGHTS;

	// The horn as the engine has already decided it this frame, not the key:
	// this runs after CGame::Process, where ProcessControl wrote 1 or 0 for a
	// car in status PLAYER. On a siren car a tap of the key writes 0 and
	// toggles the siren, and on Mr Whoopee the key works the jingle and the
	// timer is not written at all - the key is not the horn. game/horn.h.
	const bool honking = Field<uint8_t>(vehicle, offs::VEH_HORN_TIMER) != 0;
	if (HornOnWire(honking, g_hornTail)) {
		out.flags |= VEH_HORN;
		if (!g_saidHornSent) {
			g_saidHornSent = true;
			Log("bridge: our first honk is on its way out");
		}
	}

	return true;
}

// ---- a car nobody is sitting in (protocol.h, S_VehicleCustody) -------------
//
// Reads the same fields SampleLocalVehicle does, off a car named by its roster
// row rather than by the local player's seat. Split rather than generalised
// because the two refusals are opposites and both are load-bearing:
// SampleLocalVehicle refuses everything the local player is not at the wheel
// of, and this one is only ever called for a car nobody is in at all.
bool SampleObservedVehicle(RemoteVehicle &vehicle, VehicleStateBody &out) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return false;

	// A wreck is finished, and reporting it would relight it - the same
	// refusal ApplyRemoteVehicle and RestRemoteVehicle make and for the same
	// reason. A car that blew up while it was settling is a car whose settle
	// is over.
	if (IsWrecked(v)) {
		vehicle.destroyed = true;
		return false;
	}

	out       = VehicleStateBody{};
	out.netId = 0;   // filled in by the caller: netIds are the server's

	out.pos = ReadVec3(v, offs::POSITION);
	out.rot = QuatFromAxes(ReadVec3(v, offs::MATRIX_RIGHT),
	                       ReadVec3(v, offs::MATRIX_FWD),
	                       ReadVec3(v, offs::MATRIX_UP));

	out.moveSpeed = ReadVec3(v, offs::MOVE_SPEED);
	out.turnSpeed = ReadVec3(v, offs::TURN_SPEED);

	out.steer  = Field<float>(v, offs::VEH_STEER_ANGLE);
	out.gas    = Field<float>(v, offs::VEH_GAS_PEDAL);
	out.brake  = Field<float>(v, offs::VEH_BRAKE_PEDAL);
	out.gear   = Field<uint8_t>(v, offs::VEH_CURRENT_GEAR);
	out.health = Field<float>(v, offs::VEH_HEALTH);

	out.flags = 0;
	if (GetBit(v, offs::VEH_FLAGS_A, offs::VEH_ENGINE_ON))
		out.flags |= VEH_ENGINE_ON;
	if (Field<bool>(v, offs::VEH_SIREN_OR_ALARM))
		out.flags |= VEH_SIREN;
	if (GetBit(v, offs::VEH_FLAGS_A, offs::VEH_LIGHTS_ON))
		out.flags |= VEH_LIGHTS;

	return true;
}

// Has it stopped? The speed numbers below answer it, and they are
// CanPedExitCar's - the tightest gates in the engine - because the whole
// point of letting a car settle is that a player can then get into it and out
// of it again.
//
// `bIsStatic` is tested first but never answers for a car. The quiet-frame
// counter in CPhysical::ProcessControl that sets it is behind a type test at
// 0x00495F9A - an object, or a ped without bPedPhysics - and a vehicle jumps
// past it to 0x00496179. CAutomobile::ProcessControl keeps its own count
// (0x00531EDD) and only skips its physics and zeroes both speeds, and no
// vehicle code in the image sets the bit. A car at rest is never static; the
// test stays as a guard in case something else ever sets it.
//
// A car the engine has taken out of the pool answers true: there is nothing
// left to settle, and the caller's alternative is to keep the custody open
// against an object that no longer exists. ResolveRemoteVehicle has already
// re-armed the spawn by then.
bool VehicleAtRest(RemoteVehicle &vehicle) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return true;

	if (GetBit(v, offs::ENTITY_FLAGS_A, offs::ENTITY_IS_STATIC))
		return true;

	const Vec3 move = ReadVec3(v, offs::MOVE_SPEED);
	const Vec3 turn = ReadVec3(v, offs::TURN_SPEED);
	const float moveSq = move.x * move.x + move.y * move.y + move.z * move.z;
	return VehicleAtRestNumbers(moveSq, turn.x, turn.y, turn.z);
}

// The custodian's half of who owns a fire. Its engine is the only one running
// the timer (ApplyRemoteVehicle holds everybody else's), so while this says
// yes the car is not handed back - a car given back mid-burn has every
// machine's timer running at once and as many BlowUpCars as players.
bool VehicleBurning(RemoteVehicle &vehicle) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return false;
	return VehicleOnFire(VehicleTypeOf(v), Field<float>(v, offs::VEH_HEALTH),
	                     IsWrecked(v), Field<void *>(v, VEHICLE_FIRE) != nullptr);
}

// Under the surface and still moving: a car on its way to the bottom. Asked of
// CWaterLevel::GetWaterLevel with bDontCheckZ set, since its own depth test
// refuses anything more than a few metres down, which is exactly a sinking car.
// A boat is on the water, not under it.
bool VehicleSinking(RemoteVehicle &vehicle) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v || VehicleTypeOf(v) == VEHICLE_TYPE_BOAT || IsWrecked(v))
		return false;
	const Vec3 at    = ReadVec3(v, offs::POSITION);
	float      level = 0.0f;
	using WaterLevelFn = bool(__cdecl *)(float, float, float, float *, uint32_t);
	if (!Func<WaterLevelFn>(CWaterLevel__GetWaterLevel)(at.x, at.y, at.z, &level, 1))
		return false;
	if (!(at.z < level))
		return false;
	return StillSinking(ReadVec3(v, offs::MOVE_SPEED));
}

void TakeVehicleBack(RemoteVehicle &vehicle) {
	SetVehicleObserved(ResolveRemoteVehicle(vehicle), false);
}

// Has the local player's car just shoved this one? Asked of a session car
// nobody holds, which the correction pins where the session last had it, after
// the frame's physics and before the correction puts it back. The engine has
// written the collision into the car it hit - the impulse, and m_pDamageEntity
// beside it, the pair CObject::ProcessControl opens on (object::DAMAGE_ENTITY)
// - so a car our car has just hit and set moving is one we are pushing. The
// entity is compared and never followed: a wrong answer is a push not noticed.
bool VehiclePushedByUs(RemoteVehicle &vehicle) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v || IsWrecked(v))
		return false;
	void *const ours = Func<void *(__cdecl *)()>(FindPlayerVehicle)();
	if (!ours || ours == v)
		return false;
	// At its wheel, not riding in it: a passenger's car is somebody else's to
	// push with, and their machine asks for itself.
	void *const ped = PlayerPed();
	if (!ped || Field<void *>(ours, offs::VEH_DRIVER) != ped)
		return false;
	if (!(Field<float>(v, offs::VEH_DAMAGE_IMPULSE) > 0.0f))
		return false;
	if (Field<void *>(v, object::DAMAGE_ENTITY) != ours)
		return false;
	return MovedByPush(ReadVec3(v, offs::MOVE_SPEED), ReadVec3(v, offs::TURN_SPEED));
}

// A traffic car the session has just made a session car (protocol.h,
// S_CarPromoted). The roster has already moved the pool handle across; this is
// everything that has to happen to the CVehicle itself.
//
// One job, and the interesting part is what is deliberately not done here.
//
// The car stops being ours to dent. It has a driver now and that driver is not
// this machine, which is the same pairing CorrectRemoteVehicle keeps between
// the transform and bCollisionProof - an observer that goes on denting a car
// it does not own accumulates damage nobody else has, and damage only ever
// climbs. If the local player IS the new driver, CorrectRemoteVehicle clears
// it again on its very next pass by the same test that decides the transform,
// which is one frame of a flag nothing reads until the collision solver runs
// and far better than two functions disagreeing about who owns the question.
//
// **The pedestrian its own CCarCtrl put at the wheel is not taken out here**,
// and that is not an omission. A traffic car usually has an AI driver, and the
// session is about to seat a real player in seat 0 of it - but that seating is
// the existing one: S_EnterVehicle records the driver, Client::UpdateRemoteSeats
// carries it out on the first frame both halves exist, and game/ped.cpp's
// SeatPedInCar opens by calling EvictSeatOccupant for exactly this case. Doing
// it a second time from here would be a second machine for the same job, and
// the one in ped.cpp is the one that also knows not to throw the local player
// out of a seat.
//
// `weHostedIt` therefore changes nothing on the engine side. It is taken as an
// argument because the roster's use of it - never running the deleting
// destructor on a car this engine made - is the half of the promotion that is
// genuinely dangerous, and a seam that is handed the fact is a seam the next
// reader can see was thought about.
void AdoptPromotedCar(RemoteVehicle &vehicle, bool weHostedIt) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return;

	SetVehicleObserved(v, true);

	// It stops being a traffic replica and starts being a driven car, and the
	// two detours have to agree. SpawnRemoteVehicle never ran for it, so
	// nothing else would put it in the Observed table - and without a row a
	// driven car on an observer is one this engine damages and blows up on
	// its own, and one whose hits never reach the driver (protocol 23).
	// Remembered on the old host too: somebody else drives its car now.
	// NoteVehicleHolders fills in the driver before the next frame.
	ForgetReplica(vehicle.netId);
	if (!FindObserved(v))
		RememberObserved(vehicle.poolHandle, vehicle.netId);

	// A replica we built is a copy now, so it becomes the same kind of car
	// SpawnRemoteVehicle builds: MISSION_VEHICLE and on the copy list, which
	// keeps it out of the traffic budget, the save and a hideout garage like
	// every other copy (game/carlife.h). Our own engine's car is left as it
	// is; it was never CoopIII's.
	if (!weHostedIt) {
		MakeCopyAMissionCar(v);
		NoteSessionCopy(vehicle.poolHandle);
	}

	Log("bridge: traffic car %u is a session car now (%s)", vehicle.netId,
	    weHostedIt ? "our own engine's, so never ours to destroy"
	               : "a replica we already had");
}

// The car the local player is sitting in has just been given a netId by the
// session (Client::AdoptOurClaimedVehicle). The same registration
// AdoptPromotedCar makes, for the same reason: SpawnRemoteVehicle never ran
// for this car, so nothing else puts it in the Observed table.
//
// It changes nothing while we drive. Both detours ask m_pDriver first and
// never look at the table for a car the local player is at the wheel of, so
// our own car still dents and blows up under us exactly as it did. What the
// row is for is afterwards: get out, somebody else takes it (a plain entry or
// a jack), and NoteVehicleHolders writes their id into the row - after which
// a shot from us is refused here and goes to them as C_VehicleHit, and our
// engine no longer gets to blow up a car they are driving. And while nobody
// drives it, the row is what names the wreck UNOWNED_SESSION if it goes up
// here; without it the blast was nobody's and stayed on this screen.
//
// Deliberately not SetVehicleObserved. The collision-proof bit follows the
// driver's seat and CorrectRemoteVehicle / SurrenderVehicleSeat already move
// it; setting it here would make the car we are driving collision-proof.
void AdoptClaimedVehicle(RemoteVehicle &vehicle) {
	if (vehicle.poolHandle < 0)
		return;
	void *const v = VehicleFromRef(vehicle.poolHandle);
	if (!v)
		return;
	// Already there - a replica we built, a promoted car, or our own car
	// claimed again. The session has just named us, so nobody else is at the
	// wheel or settling it. NoteVehicleHolders would say the same before the
	// next frame's physics; this is the frame in between.
	if (Observed *const o = FindObserved(v)) {
		o->driverPlayerId    = 0xFF;
		o->custodianPlayerId = 0xFF;
		o->weSettle          = false;
		return;
	}
	RememberObserved(vehicle.poolHandle, vehicle.netId);
	static bool said = false;
	if (!said) {
		said = true;
		Log("bridge: vehicle %u is the car we claimed; it has an observed row "
		    "now, for when somebody else is driving it", vehicle.netId);
	}
}

// The session has let go of a car our own engine made (RemoteVehicle::ours),
// so the roster drops the row without destroying the car. The engine half:
// the Observed row goes, and the collision-proof bit comes off if somebody
// else had it last. Left behind, the row would go on refusing damage to one of
// the player's own cars on the word of a session that is over, and the bit
// would leave it undentable for good.
//
// Not through ResolveRemoteVehicle. That re-arms a spawn when the handle is
// dead, and this row is about to be wiped anyway.
void ReleaseOwnVehicle(RemoteVehicle &vehicle) {
	ForgetObserved(vehicle.netId);
	if (void *const v = VehicleFromRef(vehicle.poolHandle))
		SetVehicleObserved(v, false);
}

// The one writer of who else holds a car, for both detours. Client calls it
// for every row with a CVehicle, before every frame's physics, and has
// already turned the roster's names into "somebody other than us" - so this
// only stores.
void NoteVehicleHolders(uint16_t netId, uint8_t driverPlayerId,
                        uint8_t custodianPlayerId, bool weSettle,
                        bool blastFloorEnds) {
	g_observed.NoteHolders(netId, driverPlayerId, custodianPlayerId, weSettle,
	                       blastFloorEnds);
}

bool SpawnRemoteVehicle(RemoteVehicle &vehicle) {
	using NewFn    = void *(__cdecl *)(size_t);
	using CtorFn   = void(__thiscall *)(void *, int, uint8_t);
	using AddFn    = void(__cdecl *)(void *);
	using ClearFn  = void(__cdecl *)(const float *, void *);
	using JoinFn   = void(__cdecl *)(void *);
	using LevelFn  = uint8_t(__cdecl *)(const float *);
	using BaseFn   = float(__thiscall *)(void *);

	if (!vehicle.haveState)
		return false;   // nothing is created before we know where it goes

	const uint16_t model = vehicle.modelId;
	if (!model || !HasModelLoaded(model))
		return false;

	// Which class, decided the way CREATE_CAR decides it: by the model info's
	// vehicle type, not by a list of model ids. Before this a boat was built
	// as a CAutomobile wearing a boat's model - car suspension and four
	// wheels that aren't there, on water. game/boat.h.
	const VehicleBuild build = VehicleBuildForModel(model);
	if (build == VehicleBuild::None) {
		static bool said = false;
		if (!said) {
			said = true;
			Log("bridge: vehicle %u has model %u, which is not a car or a boat "
			    "model here; not building it (and this will not be said again)",
			    vehicle.netId, model);
		}
		return false;
	}
	const bool boat = build == VehicleBuild::Boat;

	void *const mem = Func<NewFn>(CVehicle__operator_new)(VehicleBuildSize(build));
	if (!mem)
		return false;   // the vehicle pool is full

	// ---- extras, and this is the only moment they can be set --------------
	//
	// The extras are RwAtomics cloned into the clump by
	// CVehicleModelInfo::CreateInstance, which runs INSIDE the constructor
	// below (CAutomobile ctor -> CVehicle::SetModelIndex ->
	// CEntity::SetModelIndex -> CreateRwObject; CBoat's reaches the same
	// place through vtable slot 3 at 0x0053E481). So the override goes in
	// first and comes back out afterwards, which is exactly what the engine's
	// own garage code does (CStoredCar::RestoreCar).
	//
	// Writing m_aExtras after the fact instead would compile, run, change the
	// two bytes and change nothing on screen - which is the trap this whole
	// feature is about, since the colours two blocks down genuinely do work
	// that way.
	//
	// Bounded here, because CreateInstance's `m_comps[comp]` checks only for
	// -1 and these two bytes came off a socket. An out-of-range index would
	// read past a six-entry array and hand the result to RpAtomicClone.
	const int     comps  = VehicleModelCompCount(model);
	const int8_t  extra1 = ClampVehicleExtra(vehicle.extra1, comps);
	const int8_t  extra2 = ClampVehicleExtra(vehicle.extra2, comps);
	if (!g_saidExtraRefused &&
	    (extra1 != vehicle.extra1 || extra2 != vehicle.extra2)) {
		g_saidExtraRefused = true;
		Log("bridge: vehicle %u asked for extras %d/%d and model %u only has "
		    "%d; fitting %d/%d instead",
		    vehicle.netId, static_cast<int>(vehicle.extra1),
		    static_cast<int>(vehicle.extra2), model, comps,
		    static_cast<int>(extra1), static_cast<int>(extra2));
	}

	// VEHICLE_CREATED_BY_MISSION is half the deletion gate, and it's the
	// constructor's second argument. The other half is bIsLocked, below.
	{
		ComponentOverride override(extra1, extra2);
		Func<CtorFn>(VehicleBuildCtor(build))(
		    mem, model, static_cast<uint8_t>(VEHICLE_CREATED_BY_MISSION));
	}

	// Learned this the hard way on the ped spawn path: if the constructor
	// bailed, the object isn't a vehicle and everything after this writes
	// into garbage.
	if (Field<void *>(mem, 0) == nullptr) {
		Log("bridge: the %s constructor did not complete for model %u",
		    boat ? "CBoat" : "CAutomobile", model);
		return false;
	}

	// And it is the class we asked for. Every guard downstream reads
	// m_vehType off the object rather than remembering the build, so this is
	// the one place the two could be seen to disagree.
	if (VehicleTypeOf(mem) != VehicleBuildType(build))
		Log("bridge: vehicle %u was built as a %s and reads m_vehType %d",
		    vehicle.netId, boat ? "CBoat" : "CAutomobile",
		    static_cast<int>(VehicleTypeOf(mem)));

	// Said once, and it is the line that answers "did the extras arrive".
	// Reads them back off the car rather than repeating what was asked for,
	// so a mismatch here means the override did not take and the next
	// session does not have to guess which half failed.
	{
		static bool said = false;
		if (!said) {
			said = true;
			Log("bridge: vehicle %u spawned with extras %d/%d (asked for %d/%d, "
			    "model %u has %d)",
			    vehicle.netId,
			    static_cast<int>(Field<int8_t>(mem, offs::VEH_EXTRAS)),
			    static_cast<int>(Field<int8_t>(mem, offs::VEH_EXTRAS + 1)),
			    static_cast<int>(extra1), static_cast<int>(extra2), model, comps);
		}
	}

	// CREATE_CAR raises the spawn to sit on its wheels instead of with its
	// centre of mass on the ground. Skip this and every car drops half a
	// car-length into the road on creation.
	Vec3 pos = vehicle.last.pos;
	pos.z += Func<BaseFn>(CVehicle__GetDistanceFromCentreOfMassToBaseOfModel)(mem);

	PlaceVehicle(mem, pos, vehicle.last.rot, /*inWorld=*/false);

	const float clearAt[3] = {pos.x, pos.y, pos.z};
	Func<ClearFn>(CTheScripts__ClearSpaceForMissionEntity)(clearAt, mem);

	// ---- registration. Both gates, in the handler's order. ----------------
	//
	// Every reaping site in the engine tests `!bIsLocked && CanBeDeleted()`.
	// VEHICLE_CREATED_BY_MISSION above satisfies CanBeDeleted; bIsLocked is
	// a separate write, and missing it leaves half of every one of those
	// tests wide open. addresses.h has the list of sites.
	//
	// SetStatus keeps m_type (bits 0-2) and replaces m_status (bits 3-7).
	// The handler is `and al,7 / or al,20h` - keep the low three. Invert
	// that mask (which the comment in addresses.h used to say) and the car
	// turns into ENTITY_TYPE_NOTHING while keeping the old status, and
	// nothing about that failure points back at this line.
	uint8_t &status = Field<uint8_t>(mem, offs::ENTITY_FLAGS);
	status          = static_cast<uint8_t>(
        (status & 0x07u) |
        (ENTITY_STATUS_ABANDONED << ENTITY_STATUS_SHIFT));
	SetBit(mem, offs::VEH_FLAGS_A, offs::VEH_IS_LOCKED, true);

	// Not for a boat. CREATE_CAR's boat branch never calls it, and the
	// nearest node it would find is a road one (addresses.h, "boats").
	if (JoinsRoadSystemOnSpawn(build))
		Func<JoinFn>(CCarCtrl__JoinCarWithRoadSystem)(mem);

	// AutoPilot, so the engine's traffic AI has nothing to say about a car a
	// remote player is driving. The fields are CVehicle's, so they are safe
	// on either class; only the cruise speed differs, 9 against a boat's 20,
	// each straight out of its branch of CREATE_CAR.
	//
	// The AUTOPILOT_* offsets are absolute from the vehicle, not relative to
	// the CAutoPilot sub-object - the static_asserts in addresses.h check
	// them against VEH_AUTOPILOT rather than defining them from it. Pass the
	// sub-object as the base here instead and it writes 0x12C bytes past
	// every field.
	const float cruise = SpawnCruiseSpeed(build);
	Field<uint8_t>(mem, offs::AUTOPILOT_CAR_MISSION)     = 0;   // MISSION_NONE
	Field<uint8_t>(mem, offs::AUTOPILOT_TEMP_ACTION)     = 0;   // TEMPACT_NONE
	Field<uint8_t>(mem, offs::AUTOPILOT_DRIVING_STYLE)   = 0;   // STOP_FOR_CARS
	Field<int8_t>(mem, offs::AUTOPILOT_CURRENT_LANE)     = 0;
	Field<int8_t>(mem, offs::AUTOPILOT_NEXT_LANE)        = 0;
	Field<uint8_t>(mem, offs::AUTOPILOT_CRUISE_SPEED)    = static_cast<uint8_t>(cruise);
	Field<float>(mem, offs::AUTOPILOT_MAX_TRAFFIC_SPEED) = cruise;

	SetBit(mem, offs::VEH_FLAGS_A, offs::VEH_ENGINE_ON, false);

	const float at[3] = {pos.x, pos.y, pos.z};
	Field<int8_t>(mem, offs::ZONE_LEVEL) =
	    static_cast<int8_t>(Func<LevelFn>(CTheZones__GetLevelFromPosition)(at));

	SetBit(mem, offs::VEH_FLAGS_C, offs::VEH_HAS_BEEN_OWNED_BY_PLAYER, true);

	// Colours, which aren't part of CREATE_CAR - the model picks them at
	// random there, and random on two machines just means two different cars.
	Field<uint8_t>(mem, offs::VEH_COLOUR1) = vehicle.colour1;
	Field<uint8_t>(mem, offs::VEH_COLOUR2) = vehicle.colour2;

	// Same guard as the ped spawn, for the same reason: CWorld::Add's
	// AddToMovingList has no check for an entity that is already in the list,
	// and a second add orphans a node that nothing can ever unlink again.
	if (Field<void *>(mem, offs::MOVING_LIST_NODE) != nullptr) {
		Log("bridge: a newly constructed vehicle is already in the moving list; "
		    "unlinking before CWorld::Add");
		Func<void(__thiscall *)(void *)>(CPhysical__RemoveFromMovingList)(mem);
	}

	Func<AddFn>(CWorld__Add)(mem);

	// LEVEL_IGNORE, same reason as a remote ped: the engine culls entities
	// whose m_nZoneLevel disagrees with CGame::currLevel, and a remote
	// player can be standing on a different island from us entirely.
	Field<int8_t>(mem, offs::ZONE_LEVEL) = LEVEL_IGNORE;

	vehicle.poolHandle   = VehicleRef(mem);
	vehicle.appliedFlags = 0xFF;
	vehicle.destroyed    = false;

	// So the BlowUpCar detour can tell this car is one we are watching rather
	// than one of this machine's own. Registered after the pool handle, so a
	// constructor that bailed never leaves a row behind, and by handle rather
	// than by pointer so it cannot outlive the car.
	RememberObserved(vehicle.poolHandle, vehicle.netId);

	// And on the copy list, which is what the traffic allowance counts and
	// what the save detour hides. game/carlife.h.
	NoteSessionCopy(vehicle.poolHandle);

	Log("bridge: spawned vehicle %u as a mission %s (ref %d, model %u)",
	    vehicle.netId, boat ? "boat" : "car", vehicle.poolHandle, model);
	return true;
}

void DespawnRemoteVehicle(RemoteVehicle &vehicle) {
	using RemoveFn = void(__cdecl *)(void *);
	using RefsFn   = void(__cdecl *)(void *);

	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v) {
		vehicle.poolHandle = -1;
		return;
	}
	ForgetObserved(vehicle.netId);

	// Not with the local player in it, on his way in or on his way out.
	// m_pMyVehicle is set for all three (see LocalDrivesVehicle). The session
	// never releases a car anybody is in, so this is the dropped connection
	// with the player driving somebody else's car, or a release that raced
	// him getting in. Deleting it would leave him with bInVehicle set and a
	// nulled m_pMyVehicle. game/carlife.h, CopyEnd::HandToEngine.
	void *const ped = PlayerPed();
	const bool aboard = ped != nullptr && Field<void *>(ped, offs::PED_MY_VEHICLE) == v;
	if (HowToEndCopy(vehicle.ours, aboard) == CopyEnd::HandToEngine) {
		HandCopyToEngine(v);
		Log("bridge: vehicle %u was let go of with us in it; it's this engine's "
		    "car now instead of being destroyed", vehicle.netId);
		vehicle.poolHandle   = -1;
		vehicle.appliedFlags = 0xFF;
		return;
	}
	ForgetSessionCopy(vehicle.poolHandle);

	// By hand, before CWorld::Remove, because CWorld::Remove will not do it
	// for an entity that has gone static (addresses.h,
	// WorldRemoveUnlinksFromMovingList). Leave the node behind and the next
	// CWorld::Process reads m_rwObject off a freed pool slot.
	//
	// A parked car does not go static, though this used to say it always
	// did: the engine never sets bIsStatic on a vehicle (see VehicleAtRest).
	// So this is a guard rather than the fix for a known case, and the log
	// line says so if something ever sets the bit. RemoveFromMovingList
	// checks m_movingListNode itself, so this is free when the car was never
	// in the list.
	if (NeedsMovingListUnlink(Field<uint8_t>(v, offs::ENTITY_FLAGS_A),
	                          Field<void *>(v, offs::MOVING_LIST_NODE) != nullptr))
		Log("bridge: vehicle %u went static while still in the moving list; "
		    "unlinking it by hand", vehicle.netId);
	Func<void(__thiscall *)(void *)>(CPhysical__RemoveFromMovingList)(v);

	Func<RemoveFn>(CWorld__Remove)(v);
	Func<RefsFn>(CWorld__RemoveReferencesToDeletedObject)(v);

	// Through the object's own vtable slot 0, with the "delete" flag, so the
	// engine runs CVehicle::operator delete and the slot goes back to the
	// vehicle pool. The ped despawn crash was exactly this call going
	// through a stale vtable into the global operator delete, handing a
	// pool pointer to the CRT heap.
	void *const vtable = Field<void *>(v, 0);
	if (vtable) {
		using DtorFn = void *(__thiscall *)(void *, uint8_t);
		Func<DtorFn>(*reinterpret_cast<uintptr_t *>(vtable))(v, 1);
	}

	vehicle.poolHandle   = -1;
	vehicle.appliedFlags = 0xFF;
}

// A car in the session that nobody is sitting in the driver's seat of.
//
// The other half of the ownership rule, and the half that was missing: an
// observer may animate what it is watching, and a car nobody is driving has
// nothing to animate. Until now ApplyRemoteVehicle went on replaying the last
// driver's snapshot at it - velocity, steering, throttle, engine - for the
// rest of the session, every frame.
//
// **That is what makes a used car impossible to get back into.**
// `vehicle.last` is frozen at whatever snapshot arrived last before its owner
// stepped out, and C_VehicleState is unreliable and unordered, so "last to
// arrive" is not even reliably "last to be sent". Written back every frame it
// holds m_vecMoveSpeed at whatever speed the car was doing, and
// CVehicle::CanPedEnterCar (0x005522F0 - the disassembly and its three
// constants are in addresses.h) refuses any car whose m_vecMoveSpeed or
// m_vecTurnSpeed has a magnitude-squared over 0.04. CPed::SeekCar answers
// that refusal with CPed::RestorePreviousState and leaves m_objective at
// ENTER_CAR_AS_DRIVER, so the next frame walks the ped back to the same door.
// There is no timeout in it: the player walks at the car forever.
//
// **And the same replay is what makes a car impossible to get OUT of, on a
// threshold eight times tighter.** CVehicle::CanPedExitCar (0x005523C0, in
// addresses.h beside CPed__SetExitCar) refuses any car whose m_vecMoveSpeed
// magnitude-squared is over 0.005 or whose m_vecTurnSpeed has any component
// past 0.01 - against sq(0.2) = 0.04 on the way in - and CPed::SetExitCar's
// answer to a refusal is to return having done nothing. There is one call site
// of SetExitCar in the whole image and the player's exit key goes through it,
// so a car this machine is writing a stale velocity onto is a car the player
// cannot leave, with no message and nothing that retries. That is why every
// writer in this file is guarded on who is in the driver's seat, and why the
// guard has to be right rather than nearly right: the failure is silent.
//
// Zeroed every frame rather than once on the transition, and that is
// deliberate. CorrectRemoteVehicle pins the transform after physics every
// frame, so a car held a hair off the ground never lands - and the engine
// would keep adding gravity to m_vecMoveSpeed.z with nothing to take it out
// again. One frame of gravity cannot reach 0.04; twenty can.
void RestRemoteVehicle(RemoteVehicle &vehicle) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return;

	// A wreck is finished; the same refusal ApplyRemoteVehicle makes, and for
	// the same reason. Nothing below belongs on one.
	if (IsWrecked(v)) {
		vehicle.destroyed = true;
		return;
	}

	const Vec3 rest{0.0f, 0.0f, 0.0f};
	WriteVec3(v, offs::MOVE_SPEED, rest);
	WriteVec3(v, offs::TURN_SPEED, rest);
	Field<float>(v, offs::VEH_STEER_ANGLE) = 0.0f;
	Field<float>(v, offs::VEH_GAS_PEDAL)   = 0.0f;
	Field<float>(v, offs::VEH_BRAKE_PEDAL) = 0.0f;

	// Health, damage and the wreck flag are deliberately NOT touched here.
	// They belong to the session whether or not anybody is driving - a joiner
	// has to be shown the shot-up car somebody parked - and ApplyRemoteVehicle
	// has already written them this frame. The engine's idle note and the
	// siren are left alone for the same reason plus one more: those go
	// through appliedFlags, a change detector, which only works with one
	// function writing it. The headlights are left alone too;
	// ApplyRemoteVehicle writes them every frame, from the last driver's
	// snapshot.
}

// ---- the losing end of a carjack ------------------------------------------
//
// A car changes hands in one process. The jacker's engine plays CPed::SetCarJack,
// drags whoever was in the seat out of it and puts its own player behind the
// wheel; the victim's engine is told nothing and still has *its* player behind
// the wheel. Both machines then answer "we drive this" to
// CVehicle::m_pDriver - the question every guard in this file asks - and from
// there the car is two different cars: each end refuses to apply the other's
// state, and the server's arbitration means only one of them is being believed
// by anybody else.
//
// Normally the victim's engine is shown the jack now: S_JackingVehicle has the
// jacker's replica play it here, from SetCarJack_AllClear, and our own engine
// drags the player out (ped.cpp, BeginPedJackCar) - Client holds this back
// while that is happening. This is what is left when it could not be played:
// an older jacker or server, or a jack this machine refused to start. The end
// of the jack without its animation, which is the same sequence
// COMMAND_WARP_CHAR_FROM_CAR_TO_COORD's handler runs and which addresses.h has
// transcribed: take the driver off the car, put the ped on foot, stand him
// beside it.
//
// Three differences from ped.cpp's UnseatPedFromCar, and each one is here
// because this is the local player rather than a replica:
//
//   - CPed::RemoveInCarAnims is called. It opens with `call CPed::IsPlayer /
//     test al,al / je ret`, so it is a no-op on a replica and the remote path
//     skips it for that reason; on the player it is what takes the sitting and
//     steering poses off, and without it he stands up still holding the wheel.
//   - the weapon goes back in his hand. The engine takes the model off in
//     PedSetInCarCB and puts it back in PedSetOutCarCB, and this exit does not
//     run PedSetOutCarCB - so a jacked player would be empty-handed until the
//     next time he changed weapon. CPed::SetCurrentWeapon does both halves and
//     the model is certainly streamed in, since he was holding it a moment ago.
//   - the ped is put down beside the car. A replica is pulled out by the next
//     snapshot its owner sends; the local player has nothing pulling him, so
//     he would be left standing inside the car's own collision.
//
// The car's velocities are deliberately NOT zeroed, which is the other
// difference from the remote path. Its new owner is driving it: it is doing
// forty and ApplyRemoteVehicle is about to say so. Zeroing them here would put
// one frame of standstill into a car somebody else is accelerating.
bool SurrenderVehicleSeat(RemoteVehicle &vehicle) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return false;

	void *const ped = PlayerPed();
	if (!ped || Field<void *>(v, offs::VEH_DRIVER) != ped)
		return false;   // not at its wheel after all; nothing to hand over

	// Jacked while already on the way out: the exit's door goes back before
	// the state it is read from is overwritten below. ped.h, ReleaseExitDoor.
	ReleaseExitDoor(ped, v);

	// The car, first. CVehicle::RemoveDriver is five instructions and does two
	// things, both of them checked against the image rather than assumed:
	//
	//   005520A0  mov al,[ecx+50h] / and al,7 / or al,20h / mov [ecx+50h],al
	//   005520AA  mov dword [ecx+1A4h],0
	//
	// which is SetStatus(STATUS_ABANDONED) - 4 shifted into bits 3-7 is 0x20 -
	// and pDriver = nil. So the status write the warp-out handler appears to
	// make around it is already made, and repeating it here would be two
	// identical stores with a comment claiming the second one mattered.
	Func<void(__thiscall *)(void *)>(CVehicle__RemoveDriver)(v);
	static_assert((ENTITY_STATUS_ABANDONED << ENTITY_STATUS_SHIFT) == 0x20,
	              "RemoveDriver's `or al,20h` is STATUS_ABANDONED, so this "
	              "function does not have to write the status itself");

	uint8_t &flagsA = Field<uint8_t>(v, offs::VEH_FLAGS_A);
	flagsA = static_cast<uint8_t>(flagsA & ~offs::VEH_ENGINE_ON);
	Field<uint8_t>(v, offs::AUTOPILOT_CRUISE_SPEED) = 0;

	// appliedFlags is a change detector, and the engine flag has just been
	// changed behind its back. Forget what was applied so the new owner's very
	// next snapshot puts the engine, the lights and the siren back the way its
	// own machine has them.
	vehicle.appliedFlags = 0xFF;

	// And then the ped, exactly as the warp-out handler leaves one.
	Field<bool>(ped, offs::PED_IN_VEHICLE)     = false;
	Field<void *>(ped, offs::PED_MY_VEHICLE)   = nullptr;
	Field<uint32_t>(ped, offs::PED_STATE)      = PEDSTATE_IDLE;
	Field<uint32_t>(ped, offs::PED_LAST_STATE) = PEDSTATE_NONE;
	Field<uint8_t>(ped, offs::ENTITY_FLAGS_A) |= offs::ENTITY_USES_COLLISION;

	Field<uint32_t>(ped, offs::PED_OBJECTIVE)      = OBJECTIVE_NONE;
	Field<uint32_t>(ped, offs::PED_PREV_OBJECTIVE) = OBJECTIVE_NONE;
	Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE) = nullptr;

	float *const vel = &Field<float>(ped, offs::MOVE_SPEED);
	vel[0] = vel[1] = vel[2] = 0.0f;

	if (void *const anim = Field<void *>(ped, offs::PED_VEHICLE_ANIM)) {
		Field<float>(anim, ANIM_BLEND_DELTA)       = -1000.0f;
		Field<void *>(ped, offs::PED_VEHICLE_ANIM) = nullptr;
	}

	Func<void(__thiscall *)(void *)>(CPed__RemoveInCarAnims)(ped);

	const uint8_t slot = Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON);
	if (slot < offs::NUM_WEAPON_SLOTS)
		Func<void(__thiscall *)(void *, uint32_t)>(CPed__SetCurrentWeapon)(ped, slot);

	// Beside it, on the driver's side. The right row of the car's matrix is a
	// unit vector across the car, and the driver sits on the left, so the step
	// is along minus that - the door he came in by. A metre up as well, so the
	// drop settles onto the pavement rather than through it.
	const float *const right = &Field<float>(v, offs::MATRIX_RIGHT);
	const float *const from  = &Field<float>(v, offs::POSITION);
	float *const       to    = &Field<float>(ped, offs::POSITION);
	to[0] = from[0] - right[0] * PED_STEP_OUT_M;
	to[1] = from[1] - right[1] * PED_STEP_OUT_M;
	to[2] = from[2] - right[2] * PED_STEP_OUT_M + 1.0f;

	// And it is no longer ours to dent, in the same frame it stops being ours
	// to drive. The pair CorrectRemoteVehicle keeps, kept here too, because
	// this is the other way round the same transition can happen.
	SetVehicleObserved(v, true);

	Log("bridge: handed the driver's seat of vehicle %u over - the session says "
	    "player %u has it now, and this machine had us in it",
	    vehicle.netId, static_cast<unsigned>(vehicle.driverPlayerId));
	return true;
}

void CorrectRemoteVehicle(RemoteVehicle &vehicle, const VehicleTransform &at) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return;

	// Whoever is in the driver's seat owns the car. If that is the local
	// player, this machine has stopped being an observer of it and must stop
	// writing the session's transform onto it - sixty times a second, after
	// physics, which is a car you can get into and cannot drive an inch.
	//
	// Passengers are not owners, so this deliberately tests the driver's seat
	// and not "is the local player inside". It is the same test
	// SampleLocalVehicle uses to decide what this machine may send.
	//
	// This is a guard, not the ownership handoff (docs/roadmap.md M2). The
	// session still believes somebody else's netId names this car, and the
	// local claim makes a second netId for it. What the guard buys is that
	// the car drives while that is being sorted out.
	if (Field<void *>(v, offs::VEH_DRIVER) == PlayerPed()) {
		// And it is ours to dent again, in the same frame it stops being
		// corrected. Miss this and you get a car you can drive into a wall
		// forever without marking it - the flag is cleared by the same test
		// that decides the transform, because they are the same question.
		SetVehicleObserved(v, false);
		if (!g_saidTookOver) {
			g_saidTookOver = true;
			Log("bridge: we are in the driver's seat of vehicle %u, so this "
			    "machine has stopped correcting it",
			    vehicle.netId);
		}
		return;
	}

	// An observer does not decide a car's shape any more than it decides where
	// it ends up. Without this the local collision solver keeps denting a car
	// somebody else owns, and because damage only ever climbs, an invented
	// dent is permanent. docs/cardamage.md §5.3.
	SetVehicleObserved(v, true);

	// Runs after CGame::Process, so this is the last word on where the car
	// sits before the frame draws. Writing it before physics instead is
	// what made the first synced car sit at an angle throwing off collision
	// sparks - the engine took our transform as a starting point and then
	// moved it on its own.
	PlaceVehicle(v, at.pos, at.rot, /*inWorld=*/true);

	// The owner's horn. Here, after CGame::Process, and every frame, because
	// the engine takes the timer back from a copy inside every frame: a copy
	// with its driver seated is STATUS_PHYSICS and the horn block takes one
	// off it (ReduceHornCounter, 0x005341B5), and one without is ABANDONED,
	// whose arm zeroes it (0x00531BAC). DMAudio.Service is the next thing the
	// game does after this returns, so it hears what is written here. 42 and
	// not the owner's 1, because a car that is not the local player's is
	// played through a rhythm and column 43 of every rhythm is silence
	// (game/horn.h).
	//
	// And 0 when he isn't honking, written rather than left to the engine.
	// The countdown would otherwise carry a released horn on through the
	// rest of its rhythm, up to 41 more frames of it.
	//
	// No pedestrian flees from this, here or anywhere. The flee is decided in
	// the car's own ped scan and only for a car in status PLAYER. The evasive
	// step and dive read the timer from the same scan, and on a driven copy
	// that runs after the horn block has taken 42 to 41 rather than to 0, so
	// they see a horn; what they do with it is behind their own early returns
	// (addresses.h, "the horn") and hasn't been watched in a game.
	// So the machine hosting the pedestrians a remote player honks at does not
	// make them run - single player would. Getting that back means running
	// the engine's flee branch for a copy, and the one way into it, status
	// PLAYER, also has ProcessControl read this machine's own pad into the
	// car. Not done; not faked.
	if (!IsWrecked(v)) {
		Field<uint8_t>(v, offs::VEH_HORN_TIMER) =
		    vehicle.hornSounding ? HORN_REPLAY_TIMER : uint8_t{0};
		if (vehicle.hornSounding && !g_saidHornReplayed) {
			g_saidHornReplayed = true;
			Log("bridge: vehicle %u's driver is honking, and we are sounding it",
			    vehicle.netId);
		}
	}
}

void ApplyRemoteVehicle(RemoteVehicle &vehicle, const VehicleStateBody &body) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return;

	// A wreck is finished. Nothing below belongs on one: its health is zero
	// by definition, its engine and lights are off because BlowUpCar turned
	// them off, and its owner has stopped sending - so the last snapshot we
	// hold is from before the blast and writing it back would relight a
	// burnt-out car and re-arm the fire timer under it.
	//
	// Asked of the engine rather than remembered, so a car this machine's own
	// engine wrecked (a synced car nobody is driving is nobody's, see the
	// detour) is caught by the same test as one the wire wrecked.
	if (IsWrecked(v)) {
		vehicle.destroyed = true;
		return;
	}

	// The five-second fire timer, held at zero for as long as somebody else is
	// driving. That timer is the only path in the engine from a raw m_fHealth
	// to a destroyed car (addresses.h), and the health it reads arrived over a
	// socket - so left alone it makes this machine decide, five seconds after
	// the wire said 249, that a car it does not own is finished.
	//
	// Only the timer is held. The flames and the smoke are drawn off
	// m_fHealth in the same block and are untouched, so a burning car still
	// burns here exactly as it does on its driver's screen. What is taken
	// away is the decision, not the picture.
	//
	// A custodian counts as an owner here. For the length of a settle the car
	// is being simulated and reported by one machine exactly as a driven one
	// is, so an observer deciding five seconds later that it is finished is
	// the same mistake with the same cause.
	//
	// A boat has the same timer on a different member, armed below 150 health
	// rather than 250 and never reset by the engine at all, so it goes through
	// the helper that knows which (addresses.h, "boats").
	if (vehicle.driverPlayerId != 0xFF ||
	    vehicle.custodianPlayerId != INVALID_PLAYER)
		HoldFireTimer(v);

	// Did a dying car's health actually reach us? Said once, because it is
	// the question the next live session has to answer and neither the old
	// code nor the old log had anything to say about it: a car that is at
	// zero health here and still not a wreck is the whole bug in one line,
	// and a car that never drops below 250 here means its owner's last
	// snapshot went out before the damage did, which is a different bug in a
	// different place.
	//
	// Also reports whether ProcessControl is even running on it. A car the
	// engine has put to sleep is off the moving list, so none of the damage
	// block above runs for it at all - no smoke, no flames, no timer - and
	// that would look exactly like "it never exploded".
	if (!g_saidBurning && body.health < VEH_FIRE_HEALTH) {
		g_saidBurning = true;
		Log("bridge: vehicle %u is down to %.0f health on the wire (static %d, "
		    "in the moving list %d)",
		    vehicle.netId, body.health,
		    GetBit(v, offs::ENTITY_FLAGS_A, offs::ENTITY_IS_STATIC) ? 1 : 0,
		    Field<void *>(v, offs::MOVING_LIST_NODE) != nullptr ? 1 : 0);
	}

	// Velocities are written, not derived. The observer's physics isn't
	// deciding where this car goes, but the engine still reads these for
	// wheel animation, engine note and skid marks - a car with the right
	// position and zero velocity just slides around looking wrong.
	Vec3 move = body.moveSpeed, turn = body.turnSpeed;
	FiniteOr(move.x, 0.0f, move.x);
	FiniteOr(move.y, 0.0f, move.y);
	FiniteOr(move.z, 0.0f, move.z);
	FiniteOr(turn.x, 0.0f, turn.x);
	FiniteOr(turn.y, 0.0f, turn.y);
	FiniteOr(turn.z, 0.0f, turn.z);
	move   = HeldMoveSpeed(move);
	turn.x = HeldWithin(turn.x, WIRE_TURN_MAX);
	turn.y = HeldWithin(turn.y, WIRE_TURN_MAX);
	turn.z = HeldWithin(turn.z, WIRE_TURN_MAX);
	WriteVec3(v, offs::MOVE_SPEED, move);
	WriteVec3(v, offs::TURN_SPEED, turn);

	// Controls - so the front wheels turn and the brake lights come on. For a
	// copy with its driver in the seat (STATUS_PHYSICS, game/carstatus.h)
	// these are what the physics runs on this frame, now that the car AI is
	// kept off it; before that SteerAICarWithPhysics wrote MISSION_NONE's
	// over them first. A car nobody is driving is ABANDONED, and that arm
	// writes its own over them anyway.
	//
	// The handbrake is not on the wire, so it is off. The AI was the only
	// thing that set it on a copy.
	FiniteOr(body.steer, 0.0f, Field<float>(v, offs::VEH_STEER_ANGLE));
	float gas = 0.0f, brake = 0.0f;
	FiniteOr(body.gas, 0.0f, gas);
	FiniteOr(body.brake, 0.0f, brake);
	Field<float>(v, offs::VEH_GAS_PEDAL)   = HeldWithin(gas, 1.0f);
	Field<float>(v, offs::VEH_BRAKE_PEDAL) = HeldWithin(brake, 1.0f);
	SetBit(v, offs::VEH_FLAGS_A, offs::VEH_HANDBRAKE_ON, false);
	// The transmission reads its gear table with this. Past reverse and five
	// forward gears is left to the copy's own engine (addresses-unverified.md,
	// "a gear off the wire").
	if (body.gear <= WIRE_GEAR_MAX)
		Field<uint8_t>(v, offs::VEH_CURRENT_GEAR) = body.gear;

	const Observed *const row = FindObserved(v);
	Field<float>(v, offs::VEH_HEALTH) =
	    HealthToWrite(body.health, row != nullptr && row->blasted,
	                  row != nullptr ? row->blastHealth : 0.0f);

	// Engine and siren on change only. Not because rewriting the siren would
	// restart it, which is what this used to say: the audio reads the byte
	// every frame and nothing keys on it being written. On change is simply
	// enough, since nothing in the engine clears the siren of a live copy -
	// UpdateCarAI, the one writer that could reach a driven one, is skipped
	// for it (game/siren.h has the list of writers). Hearing it is a separate
	// matter; game/siren.h again.
	//
	// Two bits are left out of the comparison, because the engine takes them
	// back from a copy by itself and a change detector never notices:
	//
	//   - the horn, which the ABANDONED arm of ProcessControl zeroes and the
	//     horn block counts down (0x005341B5) for every other status that
	//     isn't PLAYER. It is written per frame from CorrectRemoteVehicle,
	//     after physics, and left out here so that a honk is not two
	//     "changes" that rewrite the engine and the siren for nothing.
	//   - the headlights. CAutomobile::PreRender switches the lights of a car
	//     in status 4 OFF once it is more than 100 m (|dx| + |dy|, 0x006005BC)
	//     from the camera, and never back on (0x005380BE-0x00538145) - that
	//     is the engine putting out the lights of a car somebody parked. A
	//     copy nobody is driving is status 4, and so is a driven one whose
	//     driver's ped isn't in the seat here yet. Written every frame, near
	//     they stay on; far, PreRender puts them out again before it draws.
	//     A driven copy with its driver seated is status 3, and for that one
	//     PreRender replaces bLightsOn with its own time-of-day answer every
	//     frame (0x005380C3), the same thing it does for the owner's own car -
	//     so on those this write is overwritten before anything draws.
	const uint8_t onChange = VehicleFlagsWrittenOnChange(body.flags);
	if (onChange != vehicle.appliedFlags) {
		SetBit(v, offs::VEH_FLAGS_A, offs::VEH_ENGINE_ON, (body.flags & VEH_ENGINE_ON) != 0);
		Field<bool>(v, offs::VEH_SIREN_OR_ALARM) = (body.flags & VEH_SIREN) != 0;
		vehicle.appliedFlags = onChange;
	}
	SetBit(v, offs::VEH_FLAGS_A, offs::VEH_LIGHTS_ON, (body.flags & VEH_LIGHTS) != 0);
}

// ---- the damage model (docs/cardamage.md) ----------------------------------

namespace {

// CDamageManager lives at CAutomobile+0x288, which is SIZEOF_VEHICLE - proved
// twice in addresses.h, by BlowUpCar's `lea ecx,[ebx+288h]` and by
// InflictDamage's.
void *DamageManager(void *vehicle) {
	return reinterpret_cast<uint8_t *>(vehicle) + offs::AUTO_DAMAGE_MANAGER;
}

using GetStatusFn = int32_t(__thiscall *)(void *, int32_t);
using SetStatusFn = void(__thiscall *)(void *, int32_t, int32_t);

// The three appliers all take (component, index, noFlyingComponents) and all
// `ret 0Ch`, so the bool rides a full dword like every other stack argument.
using ApplyFn = void(__thiscall *)(void *, int32_t, int32_t, int32_t);

int32_t PanelStatus(void *vehicle, unsigned panel) {
	return Func<GetStatusFn>(CDamageManager__GetPanelStatus)(
	    DamageManager(vehicle), static_cast<int32_t>(panel));
}

int32_t DoorStatus(void *vehicle, unsigned door) {
	return Func<GetStatusFn>(CDamageManager__GetDoorStatus)(
	    DamageManager(vehicle), static_cast<int32_t>(door));
}

// Reads the car and reports it in the shape the wire speaks. Split out so the
// send path and the "did anything change" test share one reading - two
// slightly different samplers of the same object is how a field ends up being
// sent and never applied.
void ReadDamage(void *vehicle, uint32_t &panels, uint16_t &doors) {
	panels = 0;
	doors  = 0;
	for (unsigned i = 0; i < NUM_PANELS; ++i)
		SetPanelLevel(panels, i, static_cast<uint8_t>(PanelStatus(vehicle, i)));
	for (unsigned i = 0; i < NUM_DOORS; ++i)
		SetDoorLevel(doors, i,
		             DoorLevel(static_cast<uint8_t>(DoorStatus(vehicle, i))));
}

bool g_saidDamageApplied = false;

// The two refusals every damage sample makes, whoever it is being sampled for.
// Shared by the driver's sampler and the custodian's so the two cannot drift
// apart on what counts as a car worth reporting.
bool SampleDamageOf(void *vehicle, VehicleDamageBody &out) {
	// Only a car has a CDamageManager. In a CBoat the same offset holds the
	// boat's own floats (0.25f, 0.35f, 0.7f... out of CBoat::CBoat at
	// 0x0053E588), so reading them as panels and doors puts whatever bytes
	// those floats happen to be on the wire as a boat's damage.
	if (!HasAutomobileBody(VehicleTypeOf(vehicle)))
		return false;

	// A wreck has nothing to say. CDamageManager::FuckCarCompletely gave it
	// six missing doors and a zeroed panel word with no input of any kind, and
	// every observer ran the same function inside its own BlowUpCar
	// (docs/protocol.md §1.11). Reporting it would be telling people something
	// their own engine worked out correctly.
	if (IsWrecked(vehicle))
		return false;

	out       = VehicleDamageBody{};
	out.netId = 0;   // the caller's: netIds are the server's
	ReadDamage(vehicle, out.panels, out.doors);
	return true;
}

} // namespace

bool SampleLocalVehicleDamage(VehicleDamageBody &out) {
	void *const vehicle = PlayerVehicle();
	if (!vehicle)
		return false;
	if (Field<void *>(vehicle, offs::VEH_DRIVER) != PlayerPed())
		return false;   // a passenger owns nothing, same test as everywhere else
	return SampleDamageOf(vehicle, out);
}

// The car this machine is settling for the session. Found by its roster row
// the way SampleObservedVehicle finds it, because nobody is in the seat
// SampleLocalVehicleDamage reads from. The custodian's engine is the one
// denting it for those two seconds, so this is the only machine that can say
// what it looks like afterwards.
bool SampleObservedVehicleDamage(RemoteVehicle &vehicle, VehicleDamageBody &out) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return false;
	return SampleDamageOf(v, out);
}

namespace {

void ApplyDamageToCar(void *v, uint16_t netId, const VehicleDamageBody &body, bool flying) {
	// Bounded before anything is written. SetDoorStatus is
	// `mov byte [ecx+edx+9],al` with no compare in front of it, so a door
	// index off a socket writes a byte wherever the index reaches; these two
	// are what make sure no index above five or six ever gets that far.
	const uint32_t wantPanels = CleanPanelWord(body.panels);
	const uint16_t wantDoors  = CleanDoorWord(body.doors);

	// A wreck's damage is its own engine's. Writing a pre-blast panel word
	// over it would be the same mistake as writing a pre-blast health.
	if (IsWrecked(v))
		return;

	// Nothing below is valid on anything but a CAutomobile. DamageManager()
	// is +0x288, which in a CBoat is the boat's own first member, and the
	// three appliers are CAutomobile methods that reach into the car's own
	// frame nodes. A panel word for a boat can still arrive - a build from
	// before this change sampled one - and it is dropped here, once said.
	if (!HasAutomobileBody(VehicleTypeOf(v))) {
		static bool said = false;
		if (!said) {
			said = true;
			Log("bridge: dropped car damage addressed to vehicle %u, which is "
			    "not a car here (m_vehType %d)",
			    netId, static_cast<int>(VehicleTypeOf(v)));
		}
		return;
	}

	const int32_t noFlying = flying ? 0 : 1;

	// ---- panels ----------------------------------------------------------
	//
	// Status first, then the applier, which is the order
	// CReplay::ProcessCarUpdate uses and the only order that works: every
	// applier re-reads the status out of the car's own CDamageManager rather
	// than taking a value.
	for (unsigned i = 0; i < NUM_PANELS; ++i) {
		const uint8_t have = static_cast<uint8_t>(PanelStatus(v, i));
		const uint8_t want = GetPanelLevel(wantPanels, i);
		if (want <= have)
			continue;   // the merge is a maximum, here as on the server

		Func<SetStatusFn>(CDamageManager__SetPanelStatus)(
		    DamageManager(v), static_cast<int32_t>(i), want);

		// The two bumpers go through SetBumperDamage and the five panels
		// through SetPanelDamage. They differ only in which COMPGROUP_ they
		// hand SpawnFlyingComponent, i.e. in how the part tumbles, so calling
		// the wrong one is a near-miss rather than a crash.
		const int32_t node = CarNodeForPanel(i);
		Func<ApplyFn>(PanelIsBumper(i) ? CAutomobile__SetBumperDamage
		                               : CAutomobile__SetPanelDamage)(
		    v, node, static_cast<int32_t>(i), noFlying);
	}

	// ---- lights ----------------------------------------------------------
	//
	// Derived, not received. SetLightStatus has one caller in the image and it
	// is always passed the literal 1, two instructions from the
	// ProgressPanelDamage call for the same panel - so a broken light is
	// exactly a panel that has been hit, and putting it on the wire would be
	// paying for something the receiver already knows.
	//
	// This is also the one part of a car's damage where writing the number is
	// enough: every read of GetLightStatus is fresh, every frame, in the
	// render pass. Nothing was cloned into the clump for it.
	for (unsigned i = 0; i <= VEHPANEL_WINDSCREEN; ++i)
		if (GetPanelLevel(wantPanels, i) != PANEL_STATUS_OK)
			Func<SetStatusFn>(CDamageManager__SetLightStatus)(
			    DamageManager(v), static_cast<int32_t>(i), 1);

	// ---- doors -----------------------------------------------------------
	//
	// The level, not the byte. m_doorStatus has four values and two of them
	// are a door somebody opened; a ped getting in writes SWINGING over
	// MISSING unconditionally, so the raw byte is the last thing that happened
	// to the door rather than what the car is wearing.
	for (unsigned i = 0; i < NUM_DOORS; ++i) {
		const uint8_t have = DoorLevel(static_cast<uint8_t>(DoorStatus(v, i)));
		const uint8_t want = GetDoorLevel(wantDoors, i);
		if (want <= have)
			continue;

		Func<SetStatusFn>(CDamageManager__SetDoorStatus)(
		    DamageManager(v), static_cast<int32_t>(i),
		    DoorStatusForLevel(want));

		const int32_t node = CarNodeForDoor(i);
		Func<ApplyFn>(CAutomobile__SetDoorDamage)(
		    v, node, static_cast<int32_t>(i), noFlying);
	}

	if (!g_saidDamageApplied) {
		g_saidDamageApplied = true;
		Log("bridge: took a car's damage off the wire; vehicle %u now has "
		    "panels %08X doors %04X",
		    netId, static_cast<unsigned>(wantPanels),
		    static_cast<unsigned>(wantDoors));
	}
}

} // namespace

void ApplyRemoteVehicleDamage(RemoteVehicle &vehicle,
                              const VehicleDamageBody &body, bool flying) {
	if (void *const v = ResolveRemoteVehicle(vehicle))
		ApplyDamageToCar(v, vehicle.netId, body, flying);
}

bool SampleHostedCarDamage(int32_t poolHandle, VehicleDamageBody &out) {
	void *const v = poolHandle >= 0 ? VehicleFromRef(poolHandle) : nullptr;
	return v && SampleDamageOf(v, out);
}

// A replica that has gone from the pool is left for CorrectAmbientCarReplica
// to notice and re-arm; the roster keeps the words and puts them back on the
// new one.
void ApplyAmbientCarDamage(RemoteAmbientCar &car, const VehicleDamageBody &body,
                           bool flying) {
	void *const v = car.poolHandle >= 0 ? VehicleFromRef(car.poolHandle) : nullptr;
	if (!v)
		return;
	ApplyDamageToCar(v, car.netId, body, flying);

	static bool said = false;
	if (!said) {
		said = true;
		Log("population: put its host's dents on traffic car %u (panels %08X doors "
		    "%04X)", car.netId, static_cast<unsigned>(body.panels),
		    static_cast<unsigned>(body.doors));
	}
}

// One bit, and it is the engine's own. CAutomobile::VehicleDamage tests
// bCollisionProof once - `mov al,[ebp+53h] / shr al,2 / and al,1 / je` at
// 0x0052F685 - and returns, before the first of its thirteen ApplyDamage
// calls. No switch in front of it, which is what makes this a complete switch
// and not the backstop the ped proof flags turned out to be
// (docs/protocol.md §1.10.2).
//
// It also stops CVehicle::InflictDamage taking health for
// WEAPONTYPE_RAMMEDBYCAR, which is the same rule rather than a side effect: an
// observer decides neither a car's shape nor its health.
void SetVehicleObserved(void *vehicle, bool observed) {
	if (!vehicle)
		return;
	SetBit(vehicle, offs::ENTITY_FLAGS_C, offs::ENTITY_COLLISION_PROOF, observed);
}

// ---- the siren (game/siren.h) ----------------------------------------------
//
// __thiscall bool cAudioManager::ProcessVehicleSirenOrAlarm(cVehicleParams *),
// ret 4. __fastcall for the reason the other detours give: `this` in ecx, the
// one stack argument where __thiscall left it, and the callee cleans it.
//
// For a copy the session says somebody else is driving and which is still
// ABANDONED here, the status bits read PHYSICS for the length of this call and
// ABANDONED again the moment it returns. Only the status bits are put back,
// not the whole byte, so m_type is never touched.
//
// That is narrower than it was written for. A copy with its driver's ped in
// the seat is PHYSICS already (game/carstatus.h) and the audio plays its
// siren with no help. What is left for this is the time the session names a
// driver whose ped isn't in the seat here: not streamed in yet, the seating
// refused, or the entry still walking to the door. Somebody else's traffic
// gets the same, when a ped row from its host names a driver for it.

namespace {

bool g_saidSirenReplayed = false;

bool SirenGateOpensFor(void *vehicle) {
	if (VehicleTypeOf(vehicle) != VEHICLE_TYPE_CAR)
		return false;
	const uint16_t model   = static_cast<uint16_t>(Field<int16_t>(vehicle, offs::MODEL_INDEX));
	const uint8_t  status  = VehicleStatus(vehicle);
	const bool     sirenOn = Field<bool>(vehicle, offs::VEH_SIREN_OR_ALARM);
	// Asked with the driver taken for granted first, so the table walk below
	// only happens for a status-4 siren car with its siren on. This runs for
	// every car the audio looks at, and hardly any of them is that.
	if (!ReplicaSirenGateOpens(model, status, sirenOn, /*remoteDriver=*/true))
		return false;
	if (const Observed *const row = FindObserved(vehicle))
		return row->driverPlayerId != 0xFF;
	// Somebody else's traffic: its host's ped rows name a driver whose
	// replica isn't in the seat here yet.
	const ReplicaRow *const replica = FindReplica(vehicle);
	return replica != nullptr && replica->driverSaid;
}

bool __fastcall HookedProcessVehicleSirenOrAlarm(void *self, void * /*edx*/,
                                                 void *params) {
	using Fn = bool(__fastcall *)(void *, void *, void *);
	const Fn original = g_sirenAudio.Original<Fn>();

	void *const v = params ? Field<void *>(params, offs::AUDIO_PARAMS_VEHICLE) : nullptr;
	if (!v || !SirenGateOpensFor(v))
		return original(self, nullptr, params);

	constexpr uint8_t TYPE_BITS = 0x07;
	uint8_t      &flags = Field<uint8_t>(v, offs::ENTITY_FLAGS);
	const uint8_t held  = flags;
	flags = static_cast<uint8_t>((held & TYPE_BITS) |
	                             (ENTITY_STATUS_PHYSICS << ENTITY_STATUS_SHIFT));
	const bool result = original(self, nullptr, params);
	flags = static_cast<uint8_t>((flags & TYPE_BITS) | (held & ~TYPE_BITS));

	if (!g_saidSirenReplayed) {
		g_saidSirenReplayed = true;
		const Observed *const row = FindObserved(v);
		const ReplicaRow *const replica = row ? nullptr : FindReplica(v);
		Log("bridge: %s %u's driver has the siren on, and we are sounding it",
		    row ? "vehicle" : "ambient car",
		    row ? static_cast<unsigned>(row->netId)
		        : replica ? static_cast<unsigned>(replica->netId) : 0u);
	}
	return result;
}

} // namespace

// ---- the car AI on a copy (game/carstatus.h) -------------------------------
//
// A copy with somebody in the driver's seat is STATUS_PHYSICS, because the
// engine makes it that when it seats the ped, and ProcessControl's PHYSICS
// arm then runs this machine's car AI on it every frame. With no mission that
// was steer 0, gas 0, brake 0.5 and the handbrake on, written over the
// owner's controls after ApplyRemoteVehicle had put them there and before the
// physics read them. The correction after CGame::Process still put the car
// where its owner had it, so what that cost was everything else: the owner's
// speed bled off inside every frame, rear wheels locked, front wheels
// straight, brake lights on whatever he was doing, and a siren car picking up
// a 45 on the horn at random off UpdateCarAI's last few lines.
//
// Both calls are skipped for a copy, so the arm's only other call,
// PlayHornIfNecessary, is left, and it honks only when the autopilot's
// slowed-down bits are set - which ProcessControl clears at 0x00531892 before
// the switch and only the steering sets again.
//
// A boat copy gets seated the same way (the objective is written by hand,
// SeatPedInCar) and ends up PHYSICS the same way. Its arm is one call,
// SteerAIBoatWithPhysics, and with no mission that published gas 0 and
// steer 0 over the owner's, so it goes too.

namespace {

bool g_saidCarAiSkipped = false;

bool CarAiMayRunOn(void *vehicle) {
	const uint8_t status = VehicleStatus(vehicle);
	// Every car on rails comes through UpdateCarAI, so the table walks below
	// only happen for a car that could be a driven copy at all.
	if (status != ENTITY_STATUS_PHYSICS)
		return true;
	const bool local = Field<void *>(vehicle, offs::VEH_DRIVER) == PlayerPed();
	const bool copy  = !local && (FindObserved(vehicle) || FindReplica(vehicle));
	if (CarAiMayRun(status, copy, local))
		return true;
	if (!g_saidCarAiSkipped) {
		g_saidCarAiSkipped = true;
		Log("bridge: a car somebody else drives is in STATUS_PHYSICS here, and "
		    "this machine's car AI is being kept off it");
	}
	return false;
}

void __cdecl HookedUpdateCarAI(void *vehicle) {
	if (vehicle && !CarAiMayRunOn(vehicle))
		return;
	g_carAiUpdate.Original<void(__cdecl *)(void *)>()(vehicle);
}

void __cdecl HookedSteerAICarWithPhysics(void *vehicle) {
	if (vehicle && !CarAiMayRunOn(vehicle))
		return;
	g_carAiSteer.Original<void(__cdecl *)(void *)>()(vehicle);
}

void __cdecl HookedSteerAIBoatWithPhysics(void *vehicle) {
	if (vehicle && !CarAiMayRunOn(vehicle))
		return;
	g_boatAiSteer.Original<void(__cdecl *)(void *)>()(vehicle);
}

} // namespace

// ---- destruction -----------------------------------------------------------

bool InstallVehicleHooks() {
	for (Observed &o : g_observed)
		o = Observed{};
	g_blastHead = g_blastCount = 0;
	g_unowned.Clear();
	g_hitHead = g_hitCount = 0;
	g_carHitHead = g_carHitCount = 0;
	g_saidDamageApplied = false;

	// Taken here rather than lazily, and the timing is the point. The hooks
	// go in once the game loop is actually running, so the map IPLs have been
	// read and the script has barely started - which is precisely the moment
	// the car generator array is the map's and nothing else's. Measured at
	// the first parked car to explode instead, a mission that had already
	// created generators of its own would have baked them into the range
	// CoopIII considers shared, and only on the host. Zero here just means
	// the world is not up yet; MapCarGeneratorCount tries again.
	g_cargenBaseline = 0;
	MapCarGeneratorCount();

	// Left as the engine has it, in case a previous run was torn down
	// mid-spawn. Costs two bytes and means a stale override can never reach
	// the first car this session creates.
	SetComponentsToUse(VEHICLE_COMPS_RANDOM, VEHICLE_COMPS_RANDOM);

	const bool car = g_blowUpCar.Install(
	    "CAutomobile::BlowUpCar", reinterpret_cast<void *>(CAutomobile__BlowUpCar),
	    reinterpret_cast<void *>(&HookedBlowUpCar));
	if (car)
		Log("vehicle: hooked CAutomobile::BlowUpCar at 0x%08X",
		    static_cast<unsigned>(CAutomobile__BlowUpCar));
	else
		Log("vehicle: FAILED to hook CAutomobile::BlowUpCar at 0x%08X - a car "
		    "exploding will not travel, and this machine will keep deciding "
		    "when somebody else's car is finished",
		    static_cast<unsigned>(CAutomobile__BlowUpCar));

	// A separate function, so a separate detour. SpawnRemoteVehicle builds a
	// real CBoat for a boat model now, so both halves depend on it: without
	// it the local player's boat exploding says nothing, and an observed boat
	// somebody else is driving can be destroyed by this machine's own engine.
	// Said in its own line either way rather than folded into the car's,
	// because a half-installed pair is the sort of thing that looks like a
	// working feature until somebody takes a boat out.
	const bool boat = g_blowUpBoat.Install(
	    "CBoat::BlowUpCar", reinterpret_cast<void *>(CBoat__BlowUpCar),
	    reinterpret_cast<void *>(&HookedBlowUpBoat));
	if (boat)
		Log("vehicle: hooked CBoat::BlowUpCar at 0x%08X",
		    static_cast<unsigned>(CBoat__BlowUpCar));
	else
		Log("vehicle: FAILED to hook CBoat::BlowUpCar at 0x%08X - a boat "
		    "exploding will not travel",
		    static_cast<unsigned>(CBoat__BlowUpCar));

	// The third detour, and the only one of the three whose absence is a
	// correctness bug rather than a missing feature. Without the two above, a
	// car's destruction does not travel; without this one, every machine keeps
	// deciding what somebody else's car is worth and the two copies diverge -
	// which is the behaviour it replaces, so it is not fatal either. Said in
	// its own line for the reason the boat's is: a half-installed set is the
	// sort of thing that looks like a working feature.
	const bool hit = g_inflictDamage.Install(
	    "CVehicle::InflictDamage",
	    reinterpret_cast<void *>(CVehicle__InflictDamage),
	    reinterpret_cast<void *>(&HookedInflictDamage));
	if (hit)
		Log("vehicle: hooked CVehicle::InflictDamage at 0x%08X",
		    static_cast<unsigned>(CVehicle__InflictDamage));
	else
		Log("vehicle: FAILED to hook CVehicle::InflictDamage at 0x%08X - "
		    "shooting somebody else's car will go on taking health off our own "
		    "copy of it and nothing will travel, so the two will diverge",
		    static_cast<unsigned>(CVehicle__InflictDamage));

	// Kept out of the return value. The caller's failure line is about cars
	// exploding, and all this one costs is a remote siren nobody hears, which
	// is how it always was. Said on its own line instead.
	g_saidSirenReplayed = false;
	if (g_sirenAudio.Install("cAudioManager::ProcessVehicleSirenOrAlarm",
	                         reinterpret_cast<void *>(cAudioManager__ProcessVehicleSirenOrAlarm),
	                         reinterpret_cast<void *>(&HookedProcessVehicleSirenOrAlarm)))
		Log("vehicle: hooked cAudioManager::ProcessVehicleSirenOrAlarm at 0x%08X",
		    static_cast<unsigned>(cAudioManager__ProcessVehicleSirenOrAlarm));
	else
		Log("vehicle: FAILED to hook cAudioManager::ProcessVehicleSirenOrAlarm "
		    "at 0x%08X - a remote player's siren will flash and stay silent",
		    static_cast<unsigned>(cAudioManager__ProcessVehicleSirenOrAlarm));

	// Out of the return value too, for the siren's reason: the caller's line
	// is about cars exploding. InstallSaveGuard says loudly what its failure
	// means on its own.
	InstallSaveGuard();
	// Also out of the return value. Without them a driven copy is steered by
	// this machine's AI between corrections, which is how it has always been.
	g_saidCarAiSkipped = false;
	if (g_carAiUpdate.Install("CCarAI::UpdateCarAI",
	                          reinterpret_cast<void *>(CCarAI__UpdateCarAI),
	                          reinterpret_cast<void *>(&HookedUpdateCarAI)))
		Log("vehicle: hooked CCarAI::UpdateCarAI at 0x%08X",
		    static_cast<unsigned>(CCarAI__UpdateCarAI));
	else
		Log("vehicle: FAILED to hook CCarAI::UpdateCarAI at 0x%08X",
		    static_cast<unsigned>(CCarAI__UpdateCarAI));
	if (g_carAiSteer.Install("CCarCtrl::SteerAICarWithPhysics",
	                         reinterpret_cast<void *>(CCarCtrl__SteerAICarWithPhysics),
	                         reinterpret_cast<void *>(&HookedSteerAICarWithPhysics)))
		Log("vehicle: hooked CCarCtrl::SteerAICarWithPhysics at 0x%08X",
		    static_cast<unsigned>(CCarCtrl__SteerAICarWithPhysics));
	else
		Log("vehicle: FAILED to hook CCarCtrl::SteerAICarWithPhysics at 0x%08X - "
		    "a car somebody else drives brakes against its own speed here",
		    static_cast<unsigned>(CCarCtrl__SteerAICarWithPhysics));
	if (g_boatAiSteer.Install("CCarCtrl::SteerAIBoatWithPhysics",
	                          reinterpret_cast<void *>(CCarCtrl__SteerAIBoatWithPhysics),
	                          reinterpret_cast<void *>(&HookedSteerAIBoatWithPhysics)))
		Log("vehicle: hooked CCarCtrl::SteerAIBoatWithPhysics at 0x%08X",
		    static_cast<unsigned>(CCarCtrl__SteerAIBoatWithPhysics));
	else
		Log("vehicle: FAILED to hook CCarCtrl::SteerAIBoatWithPhysics at 0x%08X - "
		    "a boat somebody else drives has no throttle here between corrections",
		    static_cast<unsigned>(CCarCtrl__SteerAIBoatWithPhysics));

	if (car && boat && hit)
		return true;
	for (const auto &f : HookFailures())
		Log("vehicle:   %s: %s", f.name.c_str(), f.reason.c_str());
	return false;
}

void RemoveVehicleHooks() {
	g_blowUpCar.Remove();
	g_blowUpBoat.Remove();
	g_inflictDamage.Remove();
	g_sirenAudio.Remove();
	// And MaxNumberOfCarsInUse goes back to what the engine set.
	RemoveSaveGuard();
	g_carAiUpdate.Remove();
	g_carAiSteer.Remove();
	g_boatAiSteer.Remove();
	SetComponentsToUse(VEHICLE_COMPS_RANDOM, VEHICLE_COMPS_RANDOM);
	g_blastHead = g_blastCount = 0;
	g_unowned.Clear();
	g_hitHead = g_hitCount = 0;
	g_carHitHead = g_carHitCount = 0;
	// Re-measured next time, because a reload rebuilds the generator array.
	g_cargenBaseline = 0;
	for (Observed &o : g_observed)
		o = Observed{};
	for (ReplicaRow &r : g_replicas)
		r = ReplicaRow{};
}

bool VehicleHooksInstalled() { return g_blowUpCar.IsInstalled(); }

uint8_t DrainLocalVehicleBlasts(LocalVehicleBlast *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && g_blastCount > 0) {
		out[n++]    = g_blasts[g_blastHead];
		g_blastHead = static_cast<uint8_t>((g_blastHead + 1) % MAX_PENDING_BLASTS);
		--g_blastCount;
	}
	return n;
}

uint8_t DrainLocalVehicleHits(VehicleHitBody *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && g_hitCount > 0) {
		out[n++]  = g_hits[g_hitHead];
		g_hitHead = static_cast<uint8_t>((g_hitHead + 1) % MAX_PENDING_HITS);
		--g_hitCount;
	}
	return n;
}

uint8_t DrainNpcVehicleHits(NpcVehicleHit *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && g_npcHitCount > 0) {
		out[n++]     = g_npcHits[g_npcHitHead];
		g_npcHitHead = static_cast<uint8_t>((g_npcHitHead + 1) % MAX_PENDING_HITS);
		--g_npcHitCount;
	}
	return n;
}

uint8_t DrainLocalCarHits(VehicleHitBody *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && g_carHitCount > 0) {
		out[n++]     = g_carHits[g_carHitHead];
		g_carHitHead = static_cast<uint8_t>((g_carHitHead + 1) % MAX_PENDING_HITS);
		--g_carHitCount;
	}
	return n;
}

// ---- a flame reaching a car (combat.h, IsOurFlame) --------------------------

bool ReportOurFlameOnCar(void *vehicle, FlameReportThrottle &throttle, uint32_t nowMs) {
	if (!vehicle || IsWrecked(vehicle))
		return false;

	// Whose car it is, asked exactly the way HookedInflictDamage asks it, so
	// the fire and the bullet can never disagree about who owns a car.
	void *const           localPed = PlayerPed();
	const bool            weDrive =
	    localPed != nullptr && Field<void *>(vehicle, offs::VEH_DRIVER) == localPed;
	const Observed *const o        = weDrive ? nullptr : FindObserved(vehicle);
	const bool            driven   = o != nullptr && o->driverPlayerId != 0xFF;
	const bool            settling = o != nullptr && !driven &&
	                                 o->custodianPlayerId != 0xFF;
	const ReplicaRow     *replica  =
	    (weDrive || driven || settling) ? nullptr : FindReplica(vehicle);
	const CarOwner owner =
	    ClassifyCar(false, weDrive, driven, settling, replica != nullptr, Unheld(o));

	if (!FlameGoesToOwner(owner))
		return false;

	VehicleHitBody hit{};
	hit.weapon = WEAPONTYPE_FLAMETHROWER;
	hit.amount = 0.0f;

	if (owner == CarOwner::RemoteHost) {
		if (!throttle.Due(FlameTargetKind::Traffic, replica->netId, nowMs))
			return false;
		hit.netId = replica->netId;
		PushCarHit(hit);
		return true;
	}

	if (!throttle.Due(FlameTargetKind::DrivenCar, o->netId, nowMs))
		return false;
	hit.netId = o->netId;
	PushVehicleHit(hit);
	return true;
}

// ---- who decides a wreck, for the money it pays (game/money.h) -------------

WreckDecider WhoDecidesWreckHere(void *vehicle, UnownedVehicleKey &key) {
	key      = UnownedVehicleKey{};
	key.kind = MONEY_AWARD_UNKEYED;
	if (!vehicle)
		return WreckDecider::Elsewhere;

	// BlowUpCarCommon's questions, in its order.
	void *const           localPed = PlayerPed();
	const bool            weDrive =
	    localPed != nullptr && Field<void *>(vehicle, offs::VEH_DRIVER) == localPed;
	const Observed *const o        = weDrive ? nullptr : FindObserved(vehicle);
	const bool            driven   = o != nullptr && o->driverPlayerId != 0xFF;
	const bool            settling = o != nullptr && !driven &&
	                                 o->custodianPlayerId != 0xFF;
	const ReplicaRow     *replica  =
	    (weDrive || driven || settling) ? nullptr : FindReplica(vehicle);
	const CarOwner owner =
	    ClassifyCar(false, weDrive, driven, settling, replica != nullptr);

	bool       named  = false;
	const bool hosted = MayBlowUpCar(owner) && !weDrive && !o &&
	                    HostedCarFor(vehicle, named);
	const WreckDecider d =
	    WhoDecidesWreck(owner, weDrive, o != nullptr && o->weSettle, hosted);
	if (d != WreckDecider::Everywhere)
		return d;

	// The names BlowUpCarCommon announces the same wreck under, so every
	// machine that decides it asks the server about the same car.
	if (o) {
		key.kind = UNOWNED_SESSION;
		key.id   = o->netId;
	} else {
		const int32_t cargen = FindCarGeneratorFor(vehicle);
		if (cargen >= 0) {
			key.kind = UNOWNED_PARKED;
			key.id   = static_cast<uint16_t>(cargen);
		}
	}
	return d;
}

uint8_t RemoteDriverOf(const void *vehicle) {
	const Observed *const o = FindObserved(vehicle);
	return o != nullptr ? o->driverPlayerId : INVALID_PLAYER;
}

namespace {

bool g_saidFlameCarLit     = false;
bool g_saidFlameCarRefused = false;
bool g_saidFlameCarNoLight = false;

// The owner's half: somebody's flame reached one of our cars on their screen.
// Light it with the engine's own StartFire and the 0.8f SetCarsOnFire pushes
// (0x005F7A50). The burning is then ours: the CFire hits
// CVehicle::InflictDamage with cause 9 every frame, and the detour lets that
// through because the car is ours.
void LightOurCarForFlame(void *v, RemotePlayer *attacker, uint16_t netId,
                         const char *whose) {
	const bool fireProof = (Field<uint8_t>(v, offs::ENTITY_FLAGS_C) &
	                        offs::ENTITY_FIRE_PROOF) != 0;
	if (!OwnerLightsFlame(fireProof, IsWrecked(v))) {
		if (!g_saidFlameCarNoLight) {
			g_saidFlameCarNoLight = true;
			Log("vehicle: a flame reached %s %u on another screen, and here it is "
			    "%s, which SetCarsOnFire would have skipped (0x004B3EFB). Not lit",
			    whose, netId, fireProof ? "bFireProof" : "a wreck");
		}
		return;
	}

	// The shooter's ped, the same culprit a bullet gets, so the fire's
	// m_pSource and the car's eventual m_pSetOnFireEntity blame him.
	void *const culprit = attacker ? ResolveRemotePed(*attacker) : nullptr;

	using StartFireFn = void *(__thiscall *)(void *, void *, void *, float, uint32_t);
	void *const fire  = Func<StartFireFn>(CFireManager__StartFireEntity)(
        reinterpret_cast<void *>(gFireManager), v, culprit, FIRE_PED_STRENGTH, 1u);

	if (fire) {
		if (!g_saidFlameCarLit) {
			g_saidFlameCarLit = true;
			Log("vehicle: lit %s %u because %s's flame reached it on their screen. "
			    "The fire and whatever it costs the car are ours from here",
			    whose, netId, attacker ? attacker->nick.c_str() : "somebody");
		}
	} else if (!g_saidFlameCarRefused) {
		// StartFire's own no: already burning, which is usually our replay of
		// the same flame getting there first, or an engine past 225
		// (0x00479606), or no free fire slot.
		g_saidFlameCarRefused = true;
		Log("vehicle: a flame reached %s %u on another screen and StartFire "
		    "declined to light it (%s)", whose, netId,
		    Field<void *>(v, VEHICLE_FIRE) ? "already burning"
		                                   : "engine too far gone, or no free slot");
	}
}

} // namespace

// Somebody shot a replica of one of our traffic cars. The host end of §1.23,
// and ApplyRemoteVehicleHit with the roster lookup swapped for the hosted
// table: population.cpp holds the handle, this file holds the engine calls.
void ApplyHostedCarHit(uint16_t netId, RemotePlayer *attacker,
                       const VehicleHitBody &body) {
	void *const v = ResolveHostedCar(netId);
	if (!v) {
		// Recycled, promoted or never ours. Dropped, the same decision
		// ApplyRemoteVehicleHit makes: a hit is only worth anything on the car
		// that was standing there.
		if (!g_saidCarHitGone) {
			g_saidCarHitGone = true;
			Log("vehicle: a hit arrived for traffic car %u and we don't host it "
			    "any more. Dropped", netId);
		}
		return;
	}
	if (IsWrecked(v))
		return;

	// Cause 9 is an ignition, not a hit (combat.h, IsFlameIgnition).
	if (IsFlameIgnition(body.weapon)) {
		LightOurCarForFlame(v, attacker, netId, "our traffic car");
		return;
	}

	// The same bounds ApplyRemoteVehicleHit applies to the same two fields.
	if (!IsForwardableDamage(body.weapon))
		return;
	float amount = 0.0f;
	if (!FiniteOr(body.amount, 0.0f, amount) || !(amount > 0.0f))
		return;
	if (amount > MAX_REMOTE_DAMAGE)
		amount = MAX_REMOTE_DAMAGE;

	// The shooter's own ped as the culprit, so m_pSetOnFireEntity (0x00551BF3)
	// and the eventual BlowUpCar blame them. It is a replica here, not
	// FindPlayerPed(), so combat.cpp's AddExplosion detour doesn't relay the
	// car's explosion as ours; the wreck goes out as C_UnownedBlowUp instead.
	void *const culprit = attacker ? ResolveRemotePed(*attacker) : nullptr;

	const float before = Field<float>(v, offs::VEH_HEALTH);
	{
		HitGuard guard;
		Func<InflictDamageThisFn>(CVehicle__InflictDamage)(v, culprit, body.weapon,
		                                                   amount);
	}
	const float after = Field<float>(v, offs::VEH_HEALTH);

	if (!g_saidCarHitApplied) {
		g_saidCarHitApplied = true;
		Log("vehicle: somebody shot our traffic car %u off the wire - %.0f before, "
		    "%.0f after, cause %u. The health goes back out on the car stream",
		    netId, before, after, body.weapon);
	}
}

bool BlowUpCarAsOwnerSaid(void *vehicle) {
	if (!vehicle)
		return false;
	using BlowUpSlotFn = void(__thiscall *)(void *, void *);
	void *const vtable = Field<void *>(vehicle, 0);
	if (!vtable)
		return false;
	const uintptr_t slot = reinterpret_cast<uintptr_t *>(vtable)[VTABLE_BLOW_UP_CAR];
	if (!slot)
		return false;
	BlastGuard guard;
	// Null culprit, what the script's own BLOW_UP_CAR passes. Crediting the
	// local player would pay them for a car somebody else's engine finished.
	reinterpret_cast<BlowUpSlotFn>(slot)(vehicle, nullptr);
	return true;
}

bool ReplayingVehicleBlast() { return g_replayingBlast; }

VehicleBlastReplayScope::VehicleBlastReplayScope() : m_was(g_replayingBlast) {
	g_replayingBlast = true;
}

VehicleBlastReplayScope::~VehicleBlastReplayScope() { g_replayingBlast = m_was; }

// Asked from inside BlowUpCar, so the car is still in the pool, its generator
// still holds its handle and m_pDriver hasn't been cleared. The order is
// BlowUpCarCommon's: our own car first, then the Observed table, then the
// generators.
WreckForRampage DescribeWreckForRampage(void *vehicle) {
	WreckForRampage out;
	out.replay = g_replayingBlast;
	if (out.replay || !vehicle)
		return out;

	// Ours. Nobody else may decide it, so it needs no name.
	void *const localPed = PlayerPed();
	if (localPed && Field<void *>(vehicle, offs::VEH_DRIVER) == localPed)
		return out;

	if (const Observed *o = FindObserved(vehicle)) {
		// A car another player drives or settles never gets here: the detour
		// refuses it. With nobody at the wheel every machine's copy can go up
		// on its own, which is what the key is for - and our own custody car
		// is keyed too, because the settle can end on another machine a few
		// frames before it ends here and that machine's engine may get there
		// as well.
		if (o->driverPlayerId == 0xFF) {
			out.key.kind = UNOWNED_SESSION;
			out.key.id   = o->netId;
		}
		return out;
	}

	// Traffic we host, or a car only this machine has: one machine decides
	// it and it goes unkeyed. A replica never gets here unless it's a replay.
	const int32_t cargen = FindCarGeneratorFor(vehicle);
	if (cargen >= 0) {
		out.key.kind = UNOWNED_PARKED;
		out.key.id   = static_cast<uint16_t>(cargen);
	}
	return out;
}

// Somebody shot our car, and their machine is telling us so.
//
// The one thing this machine is entitled to hurt on somebody else's word: a
// car of its own that the session has named and records the local player as
// driving, or as settling. Client checks the second half against its own
// roster before it gets here; this end checks that the engine still agrees,
// because those are two different questions and the round trip is several
// frames long.
namespace {

// Both of the next two: a hit a player reported, or one their pedestrian
// landed, blamed on our copy of whichever it was.
void ApplyReportedVehicleHit(RemoteVehicle &vehicle, RemotePlayer *attacker,
                             RemoteAmbientPed *npc, const VehicleHitBody &body,
                             bool settling) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v) {
		// A car the pool no longer has. Dropped, not retried, and that is the
		// decision rather than an omission - the same one ApplyRemotePedDamage
		// makes: a hit only means anything at the instant it happened, and
		// there is nothing left to land it on.
		if (!g_saidHitGone) {
			g_saidHitGone = true;
			Log("vehicle: a hit arrived for car net %u and the engine no longer "
			    "has it. Dropped, because a hit is only worth anything on the "
			    "car that was standing there", body.netId);
		}
		return;
	}

	// The engine's own answer to "is this still ours", asked here rather than
	// taken from the roster for the reason §1.11.5 and version 22 both turn
	// on: a jack happens inside one process, so the two can disagree for as
	// long as it takes the server to break the tie. If we are not at the
	// wheel, we are not the machine allowed to decide this - unless we are
	// settling it, and then nobody may be at the wheel.
	void *const localPed = PlayerPed();
	void *const atWheel  = Field<void *>(v, offs::VEH_DRIVER);
	if (!MayTakeReportedHit(localPed != nullptr && atWheel == localPed,
	                        atWheel == nullptr, settling))
		return;

	// A wreck is finished. The engine would refuse it anyway - health <= 0
	// leaves at 0x00551A10 before any arithmetic - but calling into it with a
	// burst that was in flight when the car exploded is a per-round trip into
	// a function that will do nothing.
	if (IsWrecked(v))
		return;

	// Every bound below is on something that arrived off a socket and is about
	// to be handed to the engine, and each is the bound ApplyRemotePedDamage
	// applies to the same field. The weapon steers a twenty-entry jump table
	// at 0x006026CC and is written into m_nLastWeaponDamage; the amount
	// reaches m_fHealth, and from there the fatal arm and the car's own
	// matrix.
	//
	// Cause 9 is the exception: it is an ignition and carries no amount
	// (combat.h, IsFlameIgnition).
	if (IsFlameIgnition(body.weapon)) {
		LightOurCarForFlame(v, attacker, body.netId,
		                    settling ? "the car we are settling" : "our car");
		return;
	}
	if (!IsForwardableDamage(body.weapon))
		return;

	float amount = 0.0f;
	if (!FiniteOr(body.amount, 0.0f, amount) || !(amount > 0.0f))
		return;
	if (amount > MAX_REMOTE_DAMAGE)
		amount = MAX_REMOTE_DAMAGE;

	// Blame, and it is the same choice ApplyRemotePedDamage makes for the same
	// reason: naming the shooter's own ped puts the engine's bookkeeping - the
	// scorch marks, m_pSetOnFireEntity, whoever gets blamed for the explosion -
	// on the player who did it rather than on nobody. Null is legitimate and
	// means what the script's own damage calls mean by it.
	//
	// It buys one thing it cannot buy for a pedestrian, and costs one it does
	// not cost there. What it buys: if this hit sets the car on fire,
	// m_pSetOnFireEntity is written at 0x00551BF3 and the five-second timer
	// later hands that same entity to BlowUpCar, so the car that burns out
	// blames its shooter. What it costs: at 0x00551972 a car carrying
	// bOnlyDamagedByPlayer demands a culprit that is FindPlayerPed() or
	// FindPlayerVehicle(), and a replica of somebody else's ped is neither, so
	// such a car takes nothing off the wire. That is a residual and it is left
	// standing - the way round it is to name our own player as the culprit,
	// which is a lie about who fired, and the flag exists precisely to stop
	// anybody but the player hurting that car.
	//
	// A pedestrian's round is blamed on our replica of him, so a car his host's
	// cop set alight burns out credited to the cop rather than to his host's
	// player.
	void *const culprit = npc        ? AmbientReplicaPed(*npc)
	                      : attacker ? ResolveRemotePed(*attacker)
	                                 : nullptr;

	const float before = Field<float>(v, offs::VEH_HEALTH);

	// Through the real function rather than the trampoline, the same as
	// ApplyRemotePedDamage and for the same reason: the detour above passes
	// straight through for a car this machine is driving, so this reaches the
	// engine on a build where the hook failed to install too.
	//
	// The guard is taken anyway. It is not redundant with that pass-through:
	// it is what makes the intent explicit at the one call site that is
	// allowed, so a future change to the detour's conditions cannot quietly
	// turn this call into a report.
	{
		HitGuard guard;
		Func<InflictDamageThisFn>(CVehicle__InflictDamage)(v, culprit, body.weapon,
		                                                   amount);
	}

	const float after = Field<float>(v, offs::VEH_HEALTH);

	// Read either side of the call, because "the packet arrived" and "the car
	// was hurt" are two different claims and the gap between them is where
	// every previous round of this was lost.
	if (after < before) {
		if (!g_saidHitApplied) {
			g_saidHitApplied = true;
			Log("vehicle: somebody shot our car off the wire - net %u, %.2f "
			    "health this hit, %.0f left, cause %u, %s. Whatever comes of it "
			    "goes back out on our own snapshot and, if it kills the car, on "
			    "our own %s",
			    body.netId, before - after, after, body.weapon,
			    settling ? "while we settle it" : "while we drive it",
			    settling ? "C_UnownedBlowUp" : "C_VehicleBlowUp");
		}
	} else if (!g_saidHitNoMove) {
		g_saidHitNoMove = true;
		Log("vehicle: a hit off the wire reached CVehicle::InflictDamage on our "
		    "car net %u and took no health off (%.0f before, %.0f after, %.2f "
		    "asked for). The authority rule is not what stopped it - look at "
		    "bCanBeDamaged, or at bOnlyDamagedByPlayer, which refuses any "
		    "culprit that is not this machine's own player",
		    body.netId, before, after, amount);
	}
}

} // namespace

void ApplyRemoteVehicleHit(RemoteVehicle &vehicle, RemotePlayer *attacker,
                           const VehicleHitBody &body, bool settling) {
	ApplyReportedVehicleHit(vehicle, attacker, nullptr, body, settling);
}

void ApplyNpcVehicleHit(RemoteVehicle &vehicle, RemoteAmbientPed *attacker,
                        const VehicleHitBody &body, bool settling) {
	ApplyReportedVehicleHit(vehicle, nullptr, attacker, body, settling);
}

bool BlowUpRemoteVehicle(RemoteVehicle &vehicle, const Vec3 &pos, const Quat &rot) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return false;

	// Already a wreck - this machine's own engine got there first, or the
	// event arrived twice. Either way there is nothing to do and running
	// BlowUpCar again would hand its occupants to the engine a second time.
	if (IsWrecked(v))
		return true;

	// Placed first, then blown up, and in that order for a reason: BlowUpCar
	// reads GetPosition() for the explosion, the camera shake and the fire it
	// starts, so the car has to already be where its owner says it ended up.
	// Correcting afterwards would put the wreck right and leave the blast in
	// the street the observer's physics had guessed.
	PlaceVehicle(v, pos, rot, /*inWorld=*/true);

	{
		BlastGuard guard;   // so the detour's refusal does not eat our own call
		using BlowUpSlotFn = void(__thiscall *)(void *, void *);
		void *const vtable = Field<void *>(v, 0);
		if (!vtable)
			return false;
		// Through the object's own vtable, the way CAutomobile::ProcessControl
		// itself calls it (`call dword [ebx+74h]`). A CBoat has its own
		// BlowUpCar at a different address, and this is what gets it right
		// without CoopIII having to know which it is holding.
		const uintptr_t slot =
		    reinterpret_cast<uintptr_t *>(vtable)[VTABLE_BLOW_UP_CAR];
		if (!slot)
			return false;
		// Culprit is null, which is what the script's own BLOW_UP_CAR passes.
		// Crediting the driver would be wrong twice over: they are usually the
		// victim, and CDarkel would register everyone in the car as their kill
		// on every screen at once.
		reinterpret_cast<BlowUpSlotFn>(slot)(v, nullptr);
	}

	vehicle.destroyed = true;

	if (!g_saidBlastApplied) {
		g_saidBlastApplied = true;
		Log("bridge: took a car blast off the wire; vehicle %u is a wreck at "
		    "%.1f %.1f %.1f",
		    vehicle.netId, pos.x, pos.y, pos.z);
	}
	return true;
}

uint8_t DrainUnownedBlasts(UnownedBlast *out, uint8_t max) {
	// The transforms come out zeroed: PushUnownedBlast never fills one in,
	// because this queue has always been the half of 5.8 that carries no
	// position on purpose.
	return g_unowned.Drain(out, max);
}

UnownedWreckOutcome WreckUnownedVehicle(const UnownedVehicleKey &key) {
	// UNOWNED_AMBIENT is sent, but not through here: a traffic car is
	// resolved through the ambient roster in game/population.cpp, which
	// owns that half. This function is the parked half and knows only about
	// the map's car generators, so any other kind is a key it cannot read.
	if (key.kind != UNOWNED_PARKED)
		return UnownedWreckOutcome::BadKey;

	// Outside the map's own range of generators. Either the sender has a
	// script generator we do not (only the host runs the script), or the
	// number is nonsense off a socket. Both are "never resolvable here", and
	// the bound is the engine's own: CTheCarGenerators::Process never walks
	// past NumOfCarGenerators either.
	if (key.id >= MapCarGeneratorCount())
		return UnownedWreckOutcome::BadKey;

	void *const v = CarFromGenerator(key.id);
	if (!v)
		return UnownedWreckOutcome::NotHere;   // not streamed in, or reaped

	// Already a wreck, which is the ordinary answer: the explosion that did
	// it was replayed on this machine too and our own engine got there first.
	if (IsWrecked(v))
		return UnownedWreckOutcome::Already;

	// No PlaceVehicle in front of this, and that absence is the design. A
	// driven car is wherever its owner's physics left it and only they know;
	// a parked car is where the map put it, here and everywhere, so writing a
	// transform would be this machine copying its own map data over itself -
	// and a wire float landing in a matrix is exactly the kind of thing
	// ClampToWorld exists to survive. Nothing arrives, so nothing is written.
	{
		BlastGuard   guard;   // our own call must not come back as a report
		using BlowUpSlotFn = void(__thiscall *)(void *, void *);
		void *const  vtable = Field<void *>(v, 0);
		if (!vtable)
			return UnownedWreckOutcome::NotHere;
		const uintptr_t slot =
		    reinterpret_cast<uintptr_t *>(vtable)[VTABLE_BLOW_UP_CAR];
		if (!slot)
			return UnownedWreckOutcome::NotHere;
		// Null culprit, the same thing the script's own BLOW_UP_CAR passes.
		// Crediting the reporter would pay this machine's player for a kill
		// somebody else's rocket made.
		reinterpret_cast<BlowUpSlotFn>(slot)(v, nullptr);
	}

	// BlowUpCar returns having done nothing at all when bCanBeDamaged is
	// clear, which the campaign uses during cutscenes. Saying "wrecked" then
	// would retire the instruction with the car still intact - so the answer
	// is "not now", and the loop asks again next frame.
	return IsWrecked(v) ? UnownedWreckOutcome::Wrecked
	                    : UnownedWreckOutcome::NotHere;
}

void AddVehicleBlastToBridge(WorldBridge &bridge) {
	bridge.DrainLocalVehicleBlasts = &DrainLocalVehicleBlasts;
	bridge.BlowUpRemoteVehicle     = &BlowUpRemoteVehicle;
	bridge.DrainUnownedBlasts      = &DrainUnownedBlasts;
	bridge.WreckUnownedVehicle     = &WreckUnownedVehicle;
	// Both halves of the hit exchange hang off a detour rather than off the
	// frame pump, exactly like the four above, so they install and fail with
	// them and belong here rather than in MakeWorldBridge.
	bridge.DrainLocalVehicleHits   = &DrainLocalVehicleHits;
	bridge.ApplyRemoteVehicleHit   = &ApplyRemoteVehicleHit;
	bridge.DrainNpcVehicleHits     = &DrainNpcVehicleHits;
	bridge.ApplyNpcVehicleHit      = &ApplyNpcVehicleHit;
	bridge.DrainLocalCarHits       = &DrainLocalCarHits;
	bridge.ApplyHostedCarHit       = &ApplyHostedCarHit;
	// And the two that keep the detours' table in step with a car we claimed.
	// A row is only worth anything to the detours, so they go in with them.
	bridge.AdoptClaimedVehicle     = &AdoptClaimedVehicle;
	bridge.ReleaseOwnVehicle       = &ReleaseOwnVehicle;
	bridge.NoteVehicleHolders      = &NoteVehicleHolders;
	// Not a detour, so it doesn't depend on any of them having installed.
	bridge.UpdateTrafficAllowance  = &UpdateTrafficAllowance;
}

// ---- ambient traffic (docs/population.md §3 step 4) ------------------------
//
// A replica of somebody else's traffic car. Almost SpawnRemoteVehicle, and
// the differences are all in one place and all deliberate.
//
// **VehicleCreatedBy is RANDOM_VEHICLE here, not MISSION_VEHICLE**, and that
// is the whole reason this is a separate function rather than a flag on the
// other one. The byte at +0x1F4 decides which engine counter this car lands
// in, because CVehicle::CVehicle calls CCarCtrl::UpdateCarCount and
// UpdateCarCount switches on it (addresses.h, "who maintains those counters").
// A traffic car on the machine that made it is RANDOM_VEHICLE and counted in
// NumRandomCars; a replica of it created as MISSION_VEHICLE would be counted
// in NumMissionCars instead - a counter the traffic generator's *first* gate
// does not read. Every observer's engine would then go on generating as if
// the street were empty, and the city would double.
//
// Created as RANDOM_VEHICLE, the replica lands in the same counter its
// original did, both of the generator's gates see it, and the arithmetic
// works out the way CPed::CPed already makes it work out for pedestrians
// (population.md §1.3.1). Which is the answer this had to be *measured* to
// justify, not assumed - see the crowd report in population.cpp.
//
// The price of RANDOM_VEHICLE is that CVehicle::CanBeDeleted returns true for
// it, so the other half of the reaping gate has to carry the whole load:
// bIsLocked. Every site in the engine tests `!bIsLocked && CanBeDeleted()`
// (addresses.h has the list), so a locked random car is as safe as a locked
// mission one - and the single site where being locked is what routes a car
// to deletion only ever deletes a minute-old wreck, which is behaviour a
// replica should have anyway.
bool SpawnAmbientCarReplica(RemoteAmbientCar &car) {
	using NewFn   = void *(__cdecl *)(size_t);
	using CtorFn  = void(__thiscall *)(void *, int, uint8_t);
	using AddFn   = void(__cdecl *)(void *);
	using JoinFn  = void(__cdecl *)(void *);
	using LevelFn = uint8_t(__cdecl *)(const float *);
	using BaseFn  = float(__thiscall *)(void *);

	if (car.poolHandle >= 0)
		return true;

	const uint16_t model = car.body.modelId;
	if (!model || !HasModelLoaded(model))
		return false;

	// A CAutomobile, and only for a car model. Traffic is never anything else:
	// the only callers of CBoat::CBoat in the image are CREATE_CAR, the save
	// loader, the car generators and the replay (addresses.h, "boats"), none
	// of them CCarCtrl. So a boat model here came off a socket wrong, and a
	// CAutomobile wearing it is the bug SpawnRemoteVehicle was fixed for.
	if (VehicleBuildForModel(model) != VehicleBuild::Automobile)
		return false;

	void *const mem = Func<NewFn>(CVehicle__operator_new)(offs::SIZEOF_AUTOMOBILE);
	if (!mem) {
		// Once, not per car: the thing producing these is a traffic
		// generator and it never stops trying.
		static bool said = false;
		if (!said) {
			said = true;
			Log("population: the vehicle pool is full; ambient car %u and the "
			    "ones after it stay unreplicated (and this will not be said "
			    "again)", car.netId);
		}
		return false;
	}

	// The extras, and this is the only moment they can be set - they are
	// RwAtomics cloned into the clump inside the constructor. Same mechanism
	// and the same bounds check as SpawnRemoteVehicle, for the same reason:
	// CreateInstance subscripts m_comps with no range check of its own and
	// these two bytes came off a socket.
	const int    comps  = VehicleModelCompCount(model);
	const int8_t extra1 = ClampVehicleExtra(car.body.extra1, comps);
	const int8_t extra2 = ClampVehicleExtra(car.body.extra2, comps);

	{
		ComponentOverride override(extra1, extra2);
		Func<CtorFn>(CAutomobile__ctor)(
		    mem, model, static_cast<uint8_t>(VEHICLE_CREATED_BY_RANDOM));
	}

	if (Field<void *>(mem, 0) == nullptr) {
		Log("population: the CAutomobile constructor did not complete for "
		    "ambient car %u (model %u)", car.netId, model);
		// Leaked deliberately. An object whose constructor did not finish
		// must never be run through a destructor.
		return false;
	}

	Vec3 pos = car.body.pos;
	pos.z += Func<BaseFn>(CVehicle__GetDistanceFromCentreOfMassToBaseOfModel)(mem);

	PlaceVehicle(mem, pos, car.body.rot, /*inWorld=*/false);

	// Deliberately NOT CTheScripts::ClearSpaceForMissionEntity.
	//
	// The claimed-car spawn calls it because a mission car appears where a
	// script said to put it and whatever is in the way has to go. A traffic
	// replica appears where somebody else's traffic actually is, which in a
	// shared city is a street with other cars in it - and clearing space for
	// every one of a dozen replicas would delete this machine's own traffic
	// to make room for a copy of somebody else's.

	// STATUS_ABANDONED keeps m_type in bits 0-2 and replaces m_status in bits
	// 3-7, exactly as CREATE_CAR's `and al,7 / or al,20h` does. The ABANDONED
	// arm runs no car AI. It lasts until the host's driver is seated in it,
	// which makes it PHYSICS (game/carstatus.h), and from then on it is the
	// detours on UpdateCarAI and SteerAICarWithPhysics that keep the AI off.
	uint8_t &status = Field<uint8_t>(mem, offs::ENTITY_FLAGS);
	status          = static_cast<uint8_t>(
        (status & 0x07u) | (ENTITY_STATUS_ABANDONED << ENTITY_STATUS_SHIFT));

	// The whole deletion gate, since CanBeDeleted is open for a RANDOM_VEHICLE.
	SetBit(mem, offs::VEH_FLAGS_A, offs::VEH_IS_LOCKED, true);

	Func<JoinFn>(CCarCtrl__JoinCarWithRoadSystem)(mem);

	// The autopilot is zeroed rather than replicated, and that is a decision
	// the design left open ("replicated, re-derived, or simply not needed").
	//
	// Not needed. The autopilot is the state that decides where a car is
	// *going*, and a replica decides nothing - its transform arrives off the
	// wire and is written back after every frame of local physics. Replicating
	// it would put a second opinion about the car's route on a machine that is
	// forbidden to act on one, and cost bandwidth per car to do it. What the
	// road-system join above is for is the route nodes the engine wants
	// populated for a car that exists at all; that is re-derived locally from
	// the position, which is free and cannot disagree with anything.
	Field<uint8_t>(mem, offs::AUTOPILOT_CAR_MISSION)     = 0;   // MISSION_NONE
	Field<uint8_t>(mem, offs::AUTOPILOT_TEMP_ACTION)     = 0;   // TEMPACT_NONE
	Field<uint8_t>(mem, offs::AUTOPILOT_DRIVING_STYLE)   = 0;   // STOP_FOR_CARS
	Field<int8_t>(mem, offs::AUTOPILOT_CURRENT_LANE)     = 0;
	Field<int8_t>(mem, offs::AUTOPILOT_NEXT_LANE)        = 0;
	Field<uint8_t>(mem, offs::AUTOPILOT_CRUISE_SPEED)    = 0;
	Field<float>(mem, offs::AUTOPILOT_MAX_TRAFFIC_SPEED) = 0.0f;

	SetBit(mem, offs::VEH_FLAGS_A, offs::VEH_ENGINE_ON, false);

	const float at[3] = {pos.x, pos.y, pos.z};
	Field<int8_t>(mem, offs::ZONE_LEVEL) =
	    static_cast<int8_t>(Func<LevelFn>(CTheZones__GetLevelFromPosition)(at));

	SetBit(mem, offs::VEH_FLAGS_C, offs::VEH_HAS_BEEN_OWNED_BY_PLAYER, true);

	// The colours the owner's engine rolled, not the ones ours would. Same
	// bug family as the extras and fixed the other way round, because the
	// renderer reads these every frame (game/vehicle.h, "Extras").
	Field<uint8_t>(mem, offs::VEH_COLOUR1) = car.body.colour1;
	Field<uint8_t>(mem, offs::VEH_COLOUR2) = car.body.colour2;

	if (Field<void *>(mem, offs::MOVING_LIST_NODE) != nullptr) {
		Log("population: a newly constructed ambient car is already in the "
		    "moving list; unlinking before CWorld::Add");
		Func<void(__thiscall *)(void *)>(CPhysical__RemoveFromMovingList)(mem);
	}

	// Through the population seam's own suppression, or the CWorld::Add
	// detour would hand this replica straight back as a car this machine
	// created and announce somebody else's traffic to the session as ours.
	{
		ReplicaScope scope;
		Func<AddFn>(CWorld__Add)(mem);
	}

	// LEVEL_IGNORE, same reason as everything else CoopIII creates: the
	// engine culls entities whose m_nZoneLevel disagrees with
	// CGame::currLevel, and a session spans islands.
	Field<int8_t>(mem, offs::ZONE_LEVEL) = LEVEL_IGNORE;

	car.poolHandle = VehicleRef(mem);
	// So the two detours know this one is somebody else's to damage and to
	// blow up. docs/protocol.md §1.23.
	RememberReplica(car.poolHandle, car.netId);
	return true;
}

void DespawnAmbientCarReplica(RemoteAmbientCar &car) {
	using RemoveFn = void(__cdecl *)(void *);
	using RefsFn   = void(__cdecl *)(void *);

	if (car.poolHandle < 0)
		return;
	void *const v  = VehicleFromRef(car.poolHandle);
	car.poolHandle = -1;
	ForgetReplica(car.netId);
	if (!v)
		return;   // the engine already took it

	// Not with the local player in it, on the way in or on the way out:
	// m_pMyVehicle is set for all three. Its host can reap it, or the session
	// end, while we sit at its wheel waiting for the promotion, and deleted
	// then it leaves the player with bInVehicle set and a nulled m_pMyVehicle
	// (game/carlife.h, CopyEnd::HandToEngine), or kills them half way through
	// the door. It becomes this engine's own car instead, the way a session
	// car does in DespawnRemoteVehicle.
	void *const ped = PlayerPed();
	if (ped != nullptr && Field<void *>(ped, offs::PED_MY_VEHICLE) == v) {
		HandCopyToEngine(v);
		Log("population: ambient car %u was let go of with us in it; it's this "
		    "engine's car now instead of being destroyed", car.netId);
		return;
	}

	// Before CWorld::Remove, because CWorld::Remove will not unlink an entity
	// that has gone static (addresses.h, WorldRemoveUnlinksFromMovingList),
	// and a node left behind has the next CWorld::Process reading m_rwObject
	// off a freed pool slot.
	//
	// A replica does not go static on its own, though this used to say it
	// did. CPhysical::ProcessControl's quiet-frame counter is behind a type
	// test at 0x00495F9A - an object, or a ped without bPedPhysics - and a
	// vehicle jumps past it to 0x00496179 (addresses.h, the note after the
	// boat's exit gate). So this is a guard, not the fix for a known case:
	// RemoveFromMovingList checks m_movingListNode itself and costs nothing
	// when there is no node, and the log line says so if something else
	// ever sets the bit.
	if (NeedsMovingListUnlink(Field<uint8_t>(v, offs::ENTITY_FLAGS_A),
	                          Field<void *>(v, offs::MOVING_LIST_NODE) != nullptr))
		Log("population: ambient car %u went static while still in the moving "
		    "list; unlinking it by hand", car.netId);
	Func<void(__thiscall *)(void *)>(CPhysical__RemoveFromMovingList)(v);

	{
		// ~CVehicle reaches CWorld::Remove, and so our own Remove detour.
		// Suppressed for the same reason the Add is.
		ReplicaScope scope;
		Func<RemoveFn>(CWorld__Remove)(v);
		Func<RefsFn>(CWorld__RemoveReferencesToDeletedObject)(v);

		// Through the object's own vtable, never the global operator delete -
		// that is exactly what turned the first ped despawn into a heap
		// corruption.
		void *const vtable = Field<void *>(v, 0);
		if (vtable) {
			using DtorFn = void *(__thiscall *)(void *, uint8_t);
			Func<DtorFn>(*reinterpret_cast<uintptr_t *>(vtable))(v, 1);
		}
	}
}

void CorrectAmbientCarReplica(RemoteAmbientCar &car, const VehicleTransform &at) {
	if (car.poolHandle < 0)
		return;
	void *const v = VehicleFromRef(car.poolHandle);
	if (!v) {
		// Gone from the pool under us. Re-arm the spawn rather than going on
		// writing into a slot somebody else owns now - Client will build it
		// again on the next pass, which is the same recovery
		// ResolveRemoteVehicle does for a claimed car.
		ForgetReplica(car.netId);
		car.poolHandle   = -1;
		car.spawnPending = true;
		return;
	}

	// The local player has got into it. The session still believes its owner
	// decides where this car goes, and correcting it after every frame of
	// physics is a car you can sit in and cannot drive an inch - which is
	// precisely the bug CorrectRemoteVehicles has a guard for.
	//
	// This guard is no longer the whole answer, and it used to be. The roster
	// now asks the same question out loud through LocalDrivesAmbientCar and
	// claims the car (protocol.h, S_CarPromoted), so what this buys is only
	// the handful of frames the claim takes to go round - the car drives
	// while the session is still telling everybody else where its old owner
	// thinks it is. Left on its own it did that for ever.
	if (Field<void *>(v, offs::VEH_DRIVER) == PlayerPed()) {
		SetVehicleObserved(v, false);
		static bool said = false;
		if (!said) {
			said = true;
			Log("population: we are in the driver's seat of ambient car %u, so "
			    "this machine has stopped correcting it (and will not say so "
			    "again)", car.netId);
		}
		return;
	}

	// A replica is the clearest case of all: its transform is written from the
	// wire after every frame of physics, so every collision it appears to have
	// is one nobody simulated. docs/cardamage.md §2.6.
	SetVehicleObserved(v, true);

	// The host's health, every frame, and the fire timer held at zero under
	// it. The two detours refuse InflictDamage and BlowUpCar on a replica, but
	// three writers never call either: the upside-down drain at 0x0052F472
	// (above VehicleDamage's bCollisionProof test), CFire::ProcessFire putting
	// 75.0f on the car of a burning occupant at 0x00479959, and the
	// engine-status drain at 0x005347E0. Whatever they did this frame is put
	// back here, after physics. Below 250 the flames still draw off m_fHealth,
	// so a car burning on the host burns here too, and only the timer is kept
	// from deciding anything (the same split ApplyRemoteVehicle makes).
	//
	// Not on a wreck: the host has finished with it, and writing health back
	// into a shell relights it.
	if (!IsWrecked(v)) {
		Field<float>(v, offs::VEH_HEALTH)             = car.health;
		Field<float>(v, offs::AUTO_FIRE_BLOWUP_TIMER) = 0.0f;
	}

	PlaceVehicle(v, at.pos, at.rot, /*inWorld=*/true);

	// The host's horn, counted down here (game/horn.h, TrafficHornTimer). After
	// CGame::Process and every frame for the reason the player's horn gives in
	// CorrectRemoteVehicle: a replica with its driver seated is STATUS_PHYSICS,
	// whose horn block takes one off the timer every frame, and one without
	// is ABANDONED, whose arm zeroes it; DMAudio.Service is next. When the
	// host stops, this writes the 0 itself rather than leave a driven
	// replica counting the rest of the rhythm down on its own.
	if (!IsWrecked(v)) {
		Field<uint8_t>(v, offs::VEH_HORN_TIMER) = car.hornTimer;
		static bool said = false;
		if (car.hornTimer != 0 && !said) {
			said = true;
			Log("population: ambient car %u is honking on its host, and we are "
			    "sounding it (and will not say so again)", car.netId);
		}
	}

	// The host's siren (game/siren.h, ReplicaTrafficSirenOn). Every frame and
	// after physics, so whatever this machine's engine did to the byte in
	// between is put back before PreRender draws the light bar and the audio
	// reads it. Writing 1 over a 1 changes nothing either of them can see.
	if (!IsWrecked(v)) {
		Field<bool>(v, offs::VEH_SIREN_OR_ALARM) = car.sirenOn;
		static bool said = false;
		if (car.sirenOn && !said) {
			said = true;
			Log("population: ambient car %u has its siren going on its host, "
			    "and here too (and will not say so again)", car.netId);
		}
	}
	for (ReplicaRow &r : g_replicas)
		if (r.handle == car.poolHandle && r.netId == car.netId)
			r.driverSaid = car.sirenOn && car.driverSaid;
}

bool AdoptAmbientCarReplica(RemoteAmbientCar &car) {
	using JoinFn = void(__cdecl *)(void *);

	void *const v = VehicleFromRef(car.poolHandle);
	if (!v || IsWrecked(v) || VehicleTypeOf(v) != VEHICLE_TYPE_CAR)
		return false;

	// First, so from here on the damage, blow-up, siren and car AI detours
	// all see an ordinary car.
	ForgetReplica(car.netId);

	void *const    driver = Field<void *>(v, offs::VEH_DRIVER);
	const bool     driven = AdoptedCarIsDriven(
        driver != nullptr, driver ? Field<uint32_t>(driver, offs::PED_STATE) : 0);

	uint8_t &flags = Field<uint8_t>(v, offs::ENTITY_FLAGS);
	AdoptCarBytes b;
	b.status     = static_cast<uint8_t>(flags >> ENTITY_STATUS_SHIFT);
	b.flagsA     = Field<uint8_t>(v, offs::VEH_FLAGS_A);
	b.flagsC     = Field<uint8_t>(v, offs::VEH_FLAGS_C);
	b.entityC    = Field<uint8_t>(v, offs::ENTITY_FLAGS_C);
	b.mission    = Field<uint8_t>(v, offs::AUTOPILOT_CAR_MISSION);
	b.cruise     = Field<uint8_t>(v, offs::AUTOPILOT_CRUISE_SPEED);
	b.maxTraffic = Field<float>(v, offs::AUTOPILOT_MAX_TRAFFIC_SPEED);
	b.zone       = Field<int8_t>(v, offs::ZONE_LEVEL);

	b = CarBytesAfterAdoption(b, driven);

	flags = static_cast<uint8_t>((flags & 0x07u) | (b.status << ENTITY_STATUS_SHIFT));
	Field<uint8_t>(v, offs::VEH_FLAGS_A)                = b.flagsA;
	Field<uint8_t>(v, offs::VEH_FLAGS_C)                = b.flagsC;
	Field<uint8_t>(v, offs::ENTITY_FLAGS_C)             = b.entityC;
	Field<uint8_t>(v, offs::AUTOPILOT_CAR_MISSION)      = b.mission;
	Field<uint8_t>(v, offs::AUTOPILOT_CRUISE_SPEED)     = b.cruise;
	Field<float>(v, offs::AUTOPILOT_MAX_TRAFFIC_SPEED)  = b.maxTraffic;
	Field<int8_t>(v, offs::ZONE_LEVEL)                  = b.zone;
	// The horn the correction was counting down for its old host.
	Field<uint8_t>(v, offs::VEH_HORN_TIMER) = 0;

	if (driven) {
		// COMMAND_CAR_WANDER_RANDOMLY's two calls-and-clocks, in its order:
		// the road join first, the anti-reverse stamp last.
		Func<JoinFn>(CCarCtrl__JoinCarWithRoadSystem)(v);
		Field<uint32_t>(v, offs::AUTOPILOT_ANTI_REVERSE_TIMER) =
		    Global<uint32_t>(CTimer__m_snTimeInMilliseconds);
	}

	static bool said = false;
	if (!said) {
		said = true;
		Log("population: took over traffic car %u from the replica we had - %s",
		    car.netId,
		    driven ? "its driver cruises on under our own car AI"
		           : "nobody at the wheel, so it stays parked where it is");
	}
	return true;
}

// Is the local player at the wheel of this replica? (protocol.h, S_CarPromoted.)
//
// The same question CorrectAmbientCarReplica has asked the engine since the
// day it was written, given a name and a return value so the roster can act on
// the answer instead of the correction pass quietly stepping aside from it.
//
// CVehicle::m_pDriver and not the roster, for the reason the session-car
// version gives: the roster's answer is a round trip behind the engine's, and
// the whole gap between the two is a car the player is sitting in that nobody
// has claimed. Seat 0 only - a passenger in somebody else's traffic changes
// nothing about who is steering it.
bool LocalDrivesAmbientCar(const RemoteAmbientCar &car) {
	if (car.poolHandle < 0)
		return false;
	void *const v = VehicleFromRef(car.poolHandle);
	if (!v)
		return false;
	void *const player = PlayerPed();
	return player != nullptr && Field<void *>(v, offs::VEH_DRIVER) == player;
}

bool SampleHostedCar(int32_t poolHandle, AmbientCarState &out) {
	void *const v = VehicleFromRef(poolHandle);
	if (!v)
		return false;

	out.health   = EncodeAmbientHealth(Field<float>(v, offs::VEH_HEALTH));
	out.pos      = ReadVec3(v, offs::POSITION);
	out.rot      = QuatFromAxes(ReadVec3(v, offs::MATRIX_RIGHT),
	                            ReadVec3(v, offs::MATRIX_FWD),
	                            ReadVec3(v, offs::MATRIX_UP));
	out.velocity = ReadVec3(v, offs::MOVE_SPEED);
	return true;
}

bool HostedCarHonking(int32_t poolHandle) {
	void *const v = VehicleFromRef(poolHandle);
	if (!v)
		return false;
	return TrafficHornOnWire(static_cast<uint16_t>(Field<int16_t>(v, offs::MODEL_INDEX)),
	                         Field<bool>(v, offs::VEH_SIREN_OR_ALARM),
	                         Field<uint8_t>(v, offs::VEH_HORN_TIMER));
}

bool HostedCarSirenOn(int32_t poolHandle) {
	void *const v = VehicleFromRef(poolHandle);
	return v != nullptr && Field<bool>(v, offs::VEH_SIREN_OR_ALARM);
}

void PlaceCarForBlast(void *vehicle, const BlastTransform &where) {
	if (!vehicle)
		return;
	// inWorld, because this car is already in the world: it is a replica the
	// streamer built and the ambient correction has been moving every frame.
	// Passing false would take it through the add path a second time, which
	// is the sequence that once left a freed node in ms_listMovingEntityPtrs.
	PlaceVehicle(vehicle, where.pos, where.rot, /*inWorld=*/true);
}

bool ReadCarBlastTransform(void *vehicle, BlastTransform &out) {
	if (!vehicle)
		return false;
	out.pos = ReadVec3(vehicle, offs::POSITION);
	out.rot = QuatFromAxes(ReadVec3(vehicle, offs::MATRIX_RIGHT),
	                       ReadVec3(vehicle, offs::MATRIX_FWD),
	                       ReadVec3(vehicle, offs::MATRIX_UP));
	return true;
}

bool SampleAmbientCarIdentity(void *vehicle, AmbientCarBody &out) {
	if (!vehicle)
		return false;

	const uint32_t model = Field<uint32_t>(vehicle, offs::MODEL_INDEX);
	if (model == 0 || model > 0xFFFF)
		return false;

	out.modelId = static_cast<uint16_t>(model);
	out.colour1 = Field<uint8_t>(vehicle, offs::VEH_COLOUR1);
	out.colour2 = Field<uint8_t>(vehicle, offs::VEH_COLOUR2);

	const int comps = VehicleModelCompCount(out.modelId);
	out.extra1 = ClampVehicleExtra(Field<int8_t>(vehicle, offs::VEH_EXTRAS), comps);
	out.extra2 = ClampVehicleExtra(Field<int8_t>(vehicle, offs::VEH_EXTRAS + 1), comps);
	out.pad[0] = out.pad[1] = 0;

	out.pos = ReadVec3(vehicle, offs::POSITION);
	out.rot = QuatFromAxes(ReadVec3(vehicle, offs::MATRIX_RIGHT),
	                       ReadVec3(vehicle, offs::MATRIX_FWD),
	                       ReadVec3(vehicle, offs::MATRIX_UP));
	return true;
}

void *AmbientCarFromRef(int32_t poolHandle) { return VehicleFromRef(poolHandle); }
int32_t AmbientCarRef(void *vehicle) { return VehicleRef(vehicle); }

} // namespace coopiii::game
