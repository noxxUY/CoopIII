#include "vehicle.h"

#include "addresses.h"
#include "cardamage.h"
#include "pedanim.h"
#include "population.h"
#include "../hook/hook.h"
#include "../log.h"
#include "../quat.h"

namespace coopiii::game {

namespace {

void *PlayerPed() {
	return Func<void *(__cdecl *)()>(FindPlayerPed)();
}

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
constexpr size_t MAX_OBSERVED = 64;   // Client::MAX_REMOTE_VEHICLES

struct Observed {
	int32_t  handle        = -1;
	uint16_t netId          = 0;
	uint8_t  driverPlayerId = 0xFF;
};

Observed g_observed[MAX_OBSERVED];

Observed *FindObserved(const void *vehicle) {
	if (!vehicle)
		return nullptr;
	for (Observed &o : g_observed) {
		if (o.handle < 0)
			continue;
		if (VehicleFromRef(o.handle) == vehicle)
			return &o;
	}
	return nullptr;
}

void RememberObserved(int32_t handle, uint16_t netId) {
	if (handle < 0)
		return;
	for (Observed &o : g_observed)
		if (o.handle < 0) {
			o = Observed{handle, netId, 0xFF};
			return;
		}
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

void ForgetObserved(uint16_t netId) {
	for (Observed &o : g_observed)
		if (o.handle >= 0 && o.netId == netId)
			o = Observed{};
}

// Who the session says is driving a given observed car. Called every frame
// from CorrectRemoteVehicle rather than only from ApplyRemoteVehicle, because
// the detour's refusal rule turns on it and ApplyRemoteVehicle is skipped for
// a wreck and for a car with no snapshot yet. A car whose row still said
// "nobody" would be one this machine's own engine felt free to destroy.
void NoteObservedDriver(const void *vehicle, uint8_t driverPlayerId) {
	if (Observed *o = FindObserved(vehicle))
		o->driverPlayerId = driverPlayerId;
}

// Set while CoopIII is driving BlowUpCar rather than the engine. Same shape
// and same job as combat.cpp's g_replaying: it is what lets the one blast
// that *is* authorised through the rule written to stop all the others.
bool g_replayingBlast = false;

struct BlastGuard {
	BlastGuard() { g_replayingBlast = true; }
	~BlastGuard() { g_replayingBlast = false; }
};

// One line each, first time only. A car that quietly fails to explode is
// indistinguishable from a feature nobody built - that lesson cost this
// project a session in M4 - so each link of the chain says itself once.
bool g_saidBlastSent    = false;
bool g_saidBlastApplied = false;
bool g_saidBlastRefused = false;
bool g_saidWreckReaped  = false;
bool g_saidTookOver     = false;
bool g_saidBurning      = false;

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
// Eight rather than the driver queue's four: a single rocket into a row of
// parked cars is one frame and several wrecks, and dropping one of them is a
// car that stays intact on every other screen.
constexpr uint8_t MAX_PENDING_UNOWNED = 8;
UnownedVehicleKey g_unowned[MAX_PENDING_UNOWNED];
uint8_t           g_unownedHead  = 0;
uint8_t           g_unownedCount = 0;

void PushUnownedBlast(const UnownedVehicleKey &key) {
	// Already queued: two calls for one car in one drain window. Cheap to
	// check and it keeps the queue for distinct cars.
	for (uint8_t i = 0; i < g_unownedCount; ++i) {
		const UnownedVehicleKey &k =
		    g_unowned[(g_unownedHead + i) % MAX_PENDING_UNOWNED];
		if (k.kind == key.kind && k.id == key.id)
			return;
	}
	if (g_unownedCount == MAX_PENDING_UNOWNED) {
		g_unownedHead = static_cast<uint8_t>((g_unownedHead + 1) % MAX_PENDING_UNOWNED);
		--g_unownedCount;
	}
	g_unowned[(g_unownedHead + g_unownedCount) % MAX_PENDING_UNOWNED] = key;
	++g_unownedCount;
}

// One detour per function. CAutomobile::BlowUpCar and CBoat::BlowUpCar are
// genuinely different functions (addresses.h) and a detour on one catches only
// that one, so both are hooked and both land in the same handler below.
Detour g_blowUpCar;
Detour g_blowUpBoat;

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

	if (const Observed *o = weDrive ? nullptr : FindObserved(self)) {
		if (o->driverPlayerId != 0xFF) {
			Field<float>(self, offs::AUTO_FIRE_BLOWUP_TIMER) = 0.0f;
			if (!g_saidBlastRefused) {
				g_saidBlastRefused = true;
				Log("bridge: refused to blow up vehicle %u - player %u is driving "
				    "it, so it is theirs to destroy",
				    o->netId, o->driverPlayerId);
			}
			return;
		}
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
	if (!vehicle)
		return false;

	// Only the driver is authoritative here. A passenger sees the same car
	// the driver does, and if it also sent state the two would fight it out
	// at 25 Hz.
	if (Field<void *>(vehicle, offs::VEH_DRIVER) != PlayerPed())
		return false;

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

	return true;
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

	void *const mem = Func<NewFn>(CVehicle__operator_new)(offs::SIZEOF_AUTOMOBILE);
	if (!mem)
		return false;   // the vehicle pool is full

	// ---- extras, and this is the only moment they can be set --------------
	//
	// The extras are RwAtomics cloned into the clump by
	// CVehicleModelInfo::CreateInstance, which runs INSIDE the constructor
	// below (CAutomobile ctor -> CVehicle::SetModelIndex ->
	// CEntity::SetModelIndex -> CreateRwObject). So the override goes in
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
		Func<CtorFn>(CAutomobile__ctor)(
		    mem, model, static_cast<uint8_t>(VEHICLE_CREATED_BY_MISSION));
	}

	// Learned this the hard way on the ped spawn path: if the constructor
	// bailed, the object isn't a vehicle and everything after this writes
	// into garbage.
	if (Field<void *>(mem, 0) == nullptr) {
		Log("bridge: the CAutomobile constructor did not complete for model %u", model);
		return false;
	}

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

	Func<JoinFn>(CCarCtrl__JoinCarWithRoadSystem)(mem);

	// AutoPilot, so the engine's traffic AI has nothing to say about a car a
	// remote player is driving.
	//
	// The AUTOPILOT_* offsets are absolute from the vehicle, not relative to
	// the CAutoPilot sub-object - the static_asserts in addresses.h check
	// them against VEH_AUTOPILOT rather than defining them from it. Pass the
	// sub-object as the base here instead and it writes 0x12C bytes past
	// every field.
	Field<uint8_t>(mem, offs::AUTOPILOT_CAR_MISSION)     = 0;   // MISSION_NONE
	Field<uint8_t>(mem, offs::AUTOPILOT_TEMP_ACTION)     = 0;   // TEMPACT_NONE
	Field<uint8_t>(mem, offs::AUTOPILOT_DRIVING_STYLE)   = 0;   // STOP_FOR_CARS
	Field<int8_t>(mem, offs::AUTOPILOT_CURRENT_LANE)     = 0;
	Field<int8_t>(mem, offs::AUTOPILOT_NEXT_LANE)        = 0;
	Field<uint8_t>(mem, offs::AUTOPILOT_CRUISE_SPEED)    = 9;
	Field<float>(mem, offs::AUTOPILOT_MAX_TRAFFIC_SPEED) = 9.0f;

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

	Log("bridge: spawned vehicle %u as a mission car (ref %d, model %u)",
	    vehicle.netId, vehicle.poolHandle, model);
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

	// By hand, before CWorld::Remove, because CWorld::Remove will not do it
	// for a vehicle that has gone static and a parked one always has
	// (addresses.h, WorldRemoveUnlinksFromMovingList). Leave the node behind
	// and the next CWorld::Process reads m_rwObject off a freed pool slot.
	// RemoveFromMovingList checks m_movingListNode itself, so this is free
	// when the car was never in the list.
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
// Zeroed every frame rather than once on the transition, and that is
// deliberate. CorrectRemoteVehicle pins the transform after physics every
// frame, so a car held a hair off the ground never lands - and the engine
// would keep adding gravity to m_vecMoveSpeed.z with nothing to take it out
// again. One frame of gravity cannot reach 0.04; twenty can.
void RestRemoteVehicle(RemoteVehicle &vehicle) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return;

	NoteObservedDriver(v, 0xFF);

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
	// headlights are left alone for the same reason plus one more: those go
	// through appliedFlags, which is a change-detector, and two functions
	// writing it on alternate frames would restart the siren sixty times a
	// second.
}

void CorrectRemoteVehicle(RemoteVehicle &vehicle, const VehicleTransform &at) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return;

	// Who the session says is driving, for the BlowUpCar detour's refusal
	// rule. Refreshed here because this is the one place that runs every
	// frame for every observed car with both the roster entry and the live
	// object in hand - ApplyRemoteVehicle stops running for a wreck and
	// never runs for a car with no snapshot yet.
	NoteObservedDriver(v, vehicle.driverPlayerId);

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
}

void ApplyRemoteVehicle(RemoteVehicle &vehicle, const VehicleStateBody &body) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return;

	// Who is driving it, for the BlowUpCar detour. Also done from
	// CorrectRemoteVehicle, which is the one that runs unconditionally; this
	// one is here so the very first snapshot does not have to wait a frame.
	NoteObservedDriver(v, vehicle.driverPlayerId);

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
	if (vehicle.driverPlayerId != 0xFF)
		Field<float>(v, offs::AUTO_FIRE_BLOWUP_TIMER) = 0.0f;

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
	WriteVec3(v, offs::MOVE_SPEED, move);
	WriteVec3(v, offs::TURN_SPEED, turn);

	// Controls - so the front wheels turn and the brake lights come on.
	FiniteOr(body.steer, 0.0f, Field<float>(v, offs::VEH_STEER_ANGLE));
	FiniteOr(body.gas, 0.0f, Field<float>(v, offs::VEH_GAS_PEDAL));
	FiniteOr(body.brake, 0.0f, Field<float>(v, offs::VEH_BRAKE_PEDAL));
	Field<uint8_t>(v, offs::VEH_CURRENT_GEAR) = body.gear;

	FiniteOr(body.health, 1000.0f, Field<float>(v, offs::VEH_HEALTH));

	// Flags on change only - the siren restarts its sound every time it's set.
	if (body.flags != vehicle.appliedFlags) {
		SetBit(v, offs::VEH_FLAGS_A, offs::VEH_ENGINE_ON, (body.flags & VEH_ENGINE_ON) != 0);
		SetBit(v, offs::VEH_FLAGS_A, offs::VEH_LIGHTS_ON, (body.flags & VEH_LIGHTS) != 0);
		Field<bool>(v, offs::VEH_SIREN_OR_ALARM) = (body.flags & VEH_SIREN) != 0;
		vehicle.appliedFlags = body.flags;
	}
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

} // namespace

bool SampleLocalVehicleDamage(VehicleDamageBody &out) {
	void *const vehicle = PlayerVehicle();
	if (!vehicle)
		return false;
	if (Field<void *>(vehicle, offs::VEH_DRIVER) != PlayerPed())
		return false;   // a passenger owns nothing, same test as everywhere else

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

void ApplyRemoteVehicleDamage(RemoteVehicle &vehicle,
                              const VehicleDamageBody &body, bool flying) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return;

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
		    vehicle.netId, static_cast<unsigned>(wantPanels),
		    static_cast<unsigned>(wantDoors));
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

// ---- destruction -----------------------------------------------------------

bool InstallVehicleHooks() {
	for (Observed &o : g_observed)
		o = Observed{};
	g_blastHead = g_blastCount = 0;
	g_unownedHead = g_unownedCount = 0;
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

	// A separate function, so a separate detour. Only the send half depends
	// on it today - SpawnRemoteVehicle always builds a CAutomobile - so
	// losing it costs "the local player's boat exploding says nothing" and
	// nothing else. Said in its own line either way rather than folded into
	// the car's, because a half-installed pair is the sort of thing that
	// looks like a working feature until somebody takes a boat out.
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

	if (car && boat)
		return true;
	for (const auto &f : HookFailures())
		Log("vehicle:   %s: %s", f.name.c_str(), f.reason.c_str());
	return false;
}

void RemoveVehicleHooks() {
	g_blowUpCar.Remove();
	g_blowUpBoat.Remove();
	SetComponentsToUse(VEHICLE_COMPS_RANDOM, VEHICLE_COMPS_RANDOM);
	g_blastHead = g_blastCount = 0;
	g_unownedHead = g_unownedCount = 0;
	// Re-measured next time, because a reload rebuilds the generator array.
	g_cargenBaseline = 0;
	for (Observed &o : g_observed)
		o = Observed{};
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
	uint8_t n = 0;
	while (n < max && g_unownedCount > 0) {
		// Zeroed transform, not an unread one. A parked car is where the map
		// put it on every machine and the receiver never looks at these
		// twenty-eight bytes - but a packet with uninitialised bytes on it is
		// a packet nobody can read a capture of, and this queue has always
		// been the half of 5.8 that carries no position on purpose.
		out[n]        = UnownedBlast{};
		out[n++].key  = g_unowned[g_unownedHead];
		g_unownedHead = static_cast<uint8_t>((g_unownedHead + 1) % MAX_PENDING_UNOWNED);
		--g_unownedCount;
	}
	return n;
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
	// 3-7, exactly as CREATE_CAR's `and al,7 / or al,20h` does. It is also
	// what stops the local engine's traffic AI driving the replica: the
	// autopilot only runs for a car whose status says it is being simulated.
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
	return true;
}

void DespawnAmbientCarReplica(RemoteAmbientCar &car) {
	using RemoveFn = void(__cdecl *)(void *);
	using RefsFn   = void(__cdecl *)(void *);

	if (car.poolHandle < 0)
		return;
	void *const v  = VehicleFromRef(car.poolHandle);
	car.poolHandle = -1;
	if (!v)
		return;   // the engine already took it

	// Before CWorld::Remove, because CWorld::Remove will not unlink a vehicle
	// that has gone static - and a replica is exactly the car that goes
	// static, since it is put where the wire says and otherwise stands
	// perfectly still. Leave the node behind and the next CWorld::Process
	// reads m_rwObject off a freed pool slot.
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
		car.poolHandle   = -1;
		car.spawnPending = true;
		return;
	}

	// The local player has got into it. The session still believes its owner
	// decides where this car goes, and correcting it after every frame of
	// physics is a car you can sit in and cannot drive an inch - which is
	// precisely the bug CorrectRemoteVehicles has a guard for.
	//
	// Nothing here claims the car: that is an ownership handoff and
	// population.md §1.3.1 is explicit that this design does not promise one.
	// What the guard buys is that the car drives while the session is still
	// telling everybody else where its owner thinks it is.
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

	PlaceVehicle(v, at.pos, at.rot, /*inWorld=*/true);
}

bool SampleHostedCar(int32_t poolHandle, AmbientCarState &out) {
	void *const v = VehicleFromRef(poolHandle);
	if (!v)
		return false;

	out.pad[0] = out.pad[1] = 0;
	out.pos      = ReadVec3(v, offs::POSITION);
	out.rot      = QuatFromAxes(ReadVec3(v, offs::MATRIX_RIGHT),
	                            ReadVec3(v, offs::MATRIX_FWD),
	                            ReadVec3(v, offs::MATRIX_UP));
	out.velocity = ReadVec3(v, offs::MOVE_SPEED);
	return true;
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
