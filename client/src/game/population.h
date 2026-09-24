// The ambient population seam: one door in, one door out.
//
// docs/population.md §1.1. `CWorld::Add` is the single function every entity
// walks through on its way into the world, whoever created it and for
// whatever reason - the pedestrian generator, the traffic generator, a script
// spawn, a car falling out of a crane. Hooking it means all of them register
// the same way, with no list of creation paths to keep up to date and nothing
// to forget when the engine takes a path this project has never seen.
//
// What this file does *not* do is decide anything. It watches entities
// arrive, tells the client which of them are ambient pedestrians this machine
// now hosts, and builds replicas of the ones other machines host. The
// engine's own generator is untouched: population.md §3 step 3 is the part
// that tells it how crowded the street really is, and until that lands this
// seam doubles the crowd rather than sharing it.
//
// ## The asymmetry, which is the sharp edge here
//
// `CWorld::Add` and `CWorld::Remove` are not a matching pair. Both are gated
// on `bIsStatic` (addresses.h, the moving-list block), which is what once
// left a freed node in `ms_listMovingEntityPtrs` and crashed the game. So
// nothing in here assumes a Remove for every Add: the Remove hook is a fast
// path and the authority is a sweep over the pool handles, which notices a
// ped that went away through any route at all, including no route.
#pragma once

#include "addresses.h"

#include "../client.h"

#include <cstdint>

namespace coopiii::game {

// Installs the CWorld::Add / CWorld::Remove detours. Safe to call twice.
// A failure is recorded through the usual hook diagnostics and is not fatal:
// with no hooks, CoopIII simply never notices an ambient ped, which is where
// this project was before any of it existed.
bool InstallPopulationHooks();
void RemovePopulationHooks();

// Fills the ambient-population half of the bridge. Additive, the same shape
// as AddWorldToBridge and AddSeatToBridge.
void AddPopulationToBridge(WorldBridge &bridge);

// The local engine has just killed a pedestrian. Queued for the session if it
// is one this machine hosts and the session already has a name for it, and
// ignored otherwise - a replica, or a ped still inside the round trip of its
// own naming.
//
// Called from game/combat.cpp, which owns the CPed::SetDie detour: one
// address carries one hook, and that hook has captured the local player's
// death animation since M4. The caller has already established that this
// particular call actually killed the ped - SetDie returns without doing
// anything for one that was already dying. The filter this side is about
// entitlement, not about whether a death happened.
void NoteHostedPedDeath(void *ped, uint16_t animId);

// Is this CPed a replica of a pedestrian some *other* machine hosts, and if so
// what does the session call it?
//
// The reverse of `RemoteAmbientPed::poolHandle`, and it exists for the same
// single caller game/ped.cpp's RemotePlayerForPed exists for: the
// CPed::InflictDamage detour, which is handed a raw CPed* by the engine and
// has to decide, in the middle of the engine's own call, whether this machine
// is entitled to hurt it. There is no time to go and ask the roster.
//
// Answered off a pointer *and* a pool reference together, which is the whole
// safety argument and the same one HostedPed makes: the pointer says which
// pedestrian the row used to mean, and CPools::GetPed is what stops a recycled
// slot still matching. A pointer-only table would keep saying yes after the
// engine reused the slot, and the first civilian to inherit it would have its
// bullet wounds reported to the session as somebody else's pedestrian's.
bool AmbientReplicaForPed(const void *ped, uint16_t &netId);

// The live CPed of a pedestrian *this* machine hosts and the session has
// already named, or null.
//
// The other direction of NoteHostedPedDeath's filter, and gated the same way,
// because it answers the same question about entitlement: only a ped this
// machine hosts under a netId can be named by anybody else, so only those can
// be resolved. A replica fails it, which is what stops a machine being talked
// into hurting a pedestrian it is only watching.
void *ResolveHostedPed(uint16_t netId);

// The same for a traffic car: the live CVehicle this machine hosts under that
// netId, or null. A replica fails it, so a C_CarHit can only ever land on the
// host's own car. docs/protocol.md §1.23.
void *ResolveHostedCar(uint16_t netId);
int32_t HostedCarHandle(uint16_t netId);
// The other way round, for a jack of our own traffic: its netId, or
// INVALID_NETID.
uint16_t HostedCarNetId(int32_t handle);
// Whether our engine is dragging this replica out of its seat, as a PullOut.
uint8_t AmbientBeingPulledOut(const RemoteAmbientPed &ped);
// A new session: every ped and car this machine hosts goes out again as new.
void RestartHostedNames();

// Does this machine host this CPed / CVehicle, and has the session named it?
//
// For the two InflictDamage detours, which get a bare pointer in the middle of
// somebody else's replayed shot and need to know whether the shooter has a
// replica of it - and so whether their forwarded hit is already coming
// (combat.h, ReplayedShotMayDamage). Named is what says a replica can exist;
// hosted-but-unnamed is still inside its own naming round trip. Same liveness
// test as ResolveHostedPed: the handle has to still resolve to this object.
bool HostedPedFor(const void *ped, bool &named);
bool HostedCarFor(const void *vehicle, bool &named);

// The netId of a pedestrian this machine hosts and the session has named, for
// the hits and rounds combat.cpp forwards on his behalf (protocol.h,
// C_NpcShot). HostedPedFor's liveness test.
bool HostedPedNetIdFor(const void *ped, uint16_t &netId);

// The live CPed of a replica this machine built for somebody else's
// pedestrian, or null: the pool handle has to resolve and the vtable has to be
// CCivilianPed's, the test every write into a replica goes through.
void *AmbientReplicaPed(const RemoteAmbientPed &ped);

// Where our engine has a replica, for a desync probe: a ped (`car` false) or a
// traffic car, by pool reference. False with nothing there, or a seated ped.
bool SampleReplicaPosition(int32_t poolHandle, bool car, Vec3 &out);

// Put `weapon` in a replica's hand. False while it has no replica, is seated,
// dying or dead, or the weapon's model is still streaming.
bool ArmAmbientReplica(RemoteAmbientPed &ped, uint8_t weapon);

// How many ambient peds this machine is currently hosting, for diagnostics.
uint32_t HostedAmbientPedCount();

// How many ambient traffic cars this machine is hosting, and how many
// replicas of other machines' cars it is holding. Both for the crowd
// measurement (docs/population.md §1.3.1 and §3 step 4) rather than for
// anything that decides.
uint32_t HostedAmbientCarCount();
uint32_t AmbientCarReplicaCount();

// While one of these is alive, the CWorld::Add and CWorld::Remove detours
// ignore whatever walks through them.
//
// Exists because CoopIII calls both functions itself, to put replicas into
// the world and take them out again, and without the suppression the Add
// detour would hand a replica straight back as an entity this machine
// created - announcing somebody else's pedestrian, or somebody else's car,
// to the session as one of ours. The failure is loud (an entity that exists
// twice on every other screen) but only after it has been on the wire.
//
// A plain flag rather than a thread_local: every call CoopIII makes is on the
// game thread inside the frame pump, and the engine has no other thread that
// adds entities.
//
// game/vehicle.cpp uses it too, which is why it is declared here rather than
// kept private to population.cpp.
struct ReplicaScope {
	ReplicaScope();
	~ReplicaScope();
	ReplicaScope(const ReplicaScope &) = delete;
	ReplicaScope &operator=(const ReplicaScope &) = delete;
};

// ---- a replica the local engine decided to kill ----------------------------
//
// docs/population.md §5.6, third bullet. Everything above shares a
// pedestrian's death *outwards*, from the machine that hosts him. This is the
// other direction: an observer's own engine putting a replica in the ground
// on its own account, which is a machine deciding something it is not
// entitled to decide. The answer is to stop it happening and, where it cannot
// be stopped, to put the replica back - not to tell anybody, because a
// replica knows nothing its host does not.
//
// The four things below are the arithmetic that decision rests on, kept in
// the header so tools/clienttest can walk them without GTA III running. Every
// number in them was read out of `reference/bin/gta3.exe`, and the addresses
// are in the comments so the next person can check rather than trust.

// ---- what CPed::InflictDamage checks before it hurts a pedestrian ----------
//
// The measurement the prevent-or-report decision turned on, and it is the
// pedestrian twin of the leak in `CVehicle::InflictDamage`.
//
// `CPed::InflictDamage` (0x004EA420) dispatches on the damage cause through
// `jmp dword [eax*4+0x005F9EB8]` at 0x004EA5AB, guarded by `cmp eax,15h / ja`
// at 0x004EA5A2 - so causes 0..21 have an arm and 22 and up fall into the
// default. Reading the 22 entries out of the file at 0x001F9EB8 and walking
// each arm gives the table below. Five arms read one of `CEntity`'s five
// proof flags and **six causes read nothing at all**:
//
//   0x004EA5B2  [ebp+53h]>>3&1  bMeleeProof      0 UNARMED
//   0x004EA668  [ebp+53h]>>3&1  bMeleeProof      1 BASEBALLBAT
//   0x004EA719  [ebp+53h]   &1  bBulletProof     2..7 the guns
//   0x004EA898  [ebp+53h]>>1&1  bFireProof       9 FLAMETHROWER
//   0x004EA8BA  [ebp+52h]>>1&1  bExplosionProof  8 ROCKET, 11 GRENADE,
//                                                18 EXPLOSION
//   0x004EA99F  [ebp+52h]>>1&1  bExplosionProof  10 MOLOTOV
//   0x004EA9EC  [ebp+53h]>>2&1  bCollisionProof  16 RAMMED, 17 RUNOVER
//   0x004EABB4  [ebp+53h]>>2&1  bCollisionProof  21 FALL
//   0x004EABAD  nothing                          20 DROWNING
//   0x004EABFB  nothing, it is the default       12, 13, 14, 15, 19, 22+
//
// So the flags `SpawnAmbientReplica` sets are a backstop and were never a
// mechanism, exactly as `docs/protocol.md` §1.10.2 says for a car. That is
// why the refusal below is stated on the *object* rather than on the cause.
enum class PedProof : uint8_t {
	None = 0,
	Bullet,
	Fire,
	Collision,
	Melee,
	Explosion,
};

inline PedProof PedProofForDamageCause(uint8_t cause) {
	switch (cause) {
	case WEAPONTYPE_UNARMED:
	case WEAPONTYPE_BASEBALLBAT:
		return PedProof::Melee;
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_AK47:
	case WEAPONTYPE_M16:
	case WEAPONTYPE_SNIPERRIFLE:
		return PedProof::Bullet;
	case WEAPONTYPE_FLAMETHROWER:
		return PedProof::Fire;
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_MOLOTOV:
	case WEAPONTYPE_GRENADE:
	case WEAPONTYPE_EXPLOSION:
		return PedProof::Explosion;
	case WEAPONTYPE_RAMMEDBYCAR:
	case WEAPONTYPE_RUNOVERBYCAR:
	case WEAPONTYPE_FALL:
		return PedProof::Collision;
	default:
		// 12 DETONATOR, 13, 14, 15 ARMOUR, 19 UZI_DRIVEBY, 20 DROWNING, and
		// everything from 22 up. The arm runs straight into the armour and
		// health arithmetic at 0x004EABFB with no flag in front of it.
		return PedProof::None;
	}
}

// Would the proof flags on their own have stopped this cause reaching a
// replica's health? The negative answer for six causes is the whole argument
// for refusing by object.
inline bool PedProofFlagsStopCause(uint8_t cause) {
	return PedProofForDamageCause(cause) != PedProof::None;
}

// ---- the CPed::SetDie detour's verdict for one call ------------------------
//
// `CPed::SetDie` (0x004D37D0) is the single door every death in the image
// walks through bar one, and refusing it for a replica is cause-independent -
// which is what the table above says it has to be.
//
// `RefuseAndHeal` and not just `Refuse`, because two of the callers zero the
// health *before* they call: `CPed::SetGetUp` writes `m_fHealth = 0` at
// 0x004D0F7A and then calls SetDie at 0x004D0F95, and
// `CPed::InflictDamage` does the same at 0x004EAD02. Refusing the call alone
// would leave a replica walking around on zero health, which
// `CPed::ProcessControl`'s `m_fHealth <= 1.0f` arm (0x004C8DDA, the constant
// at 0x005F8440 is 1.0f) would then try to kill on every single frame. So the
// refusal puts the health back to what `CPed::CPed` gives a new one -
// `mov dword [ebx+2C0h], 42C80000h` at 0x004C4225, which is 100.0f.
// `mov dword ptr [ebx+000002C0h],42C80000h` at 0x004C4225, inside
// CPed::CPed (0x004C41C0), two instructions before it zeroes the armour at
// +0x2C4. An ambient ped's health is deliberately not on the wire
// (protocol.h, AmbientPedState), so there is nothing better to restore to
// than what the engine itself hands a pedestrian it has just built - and a
// replica that never takes damage never needs anything better.
constexpr float REPLICA_FULL_HEALTH = 100.0f;

enum class SetDieVerdict : uint8_t {
	Run,            // not a replica, or the session asked for this death
	RefuseAndHeal,  // this machine's engine deciding about somebody else's ped
};

inline SetDieVerdict PlanReplicaSetDie(bool isReplica, bool sessionAsked) {
	return (isReplica && !sessionAsked) ? SetDieVerdict::RefuseAndHeal
	                                    : SetDieVerdict::Run;
}

// ---- the backstop -----------------------------------------------------------
//
// Has the local engine killed this replica behind the session's back?
//
// Tested on the ped's *state* rather than on the route that got it there,
// which is the point: it is complete by construction, including for the one
// route that never touches `CPed::SetDie` at all (see below) and for any
// route nobody has found yet.
//
// `sessionSaysDead` is `RemoteAmbientPed::dead`. Without it this would
// resurrect every corpse the host legitimately reported, on the frame after
// `KillAmbientReplica` laid it down.
inline bool ReplicaDiedUnasked(uint32_t pedState, bool sessionSaysDead) {
	if (sessionSaysDead)
		return false;
	return pedState == PEDSTATE_DIE || pedState == PEDSTATE_DEAD;
}

// ---- the one death that never calls CPed::SetDie ----------------------------
//
// `CAutomobile::BlowUpCar` (0x0053BC60) kills its driver at 0x0053BDA7 and
// each passenger at 0x0053BE07, and it reads no proof flag on either. Which
// of the two arms it takes is decided by `cmp dword [ecx+224h],2Ch` at
// 0x0053BDC4 and 0x0053BE26 - `m_nPedState == PED_DRIVING`:
//
//   PED_DRIVING     CPed::SetDead (0x004D3970, `m_fHealth = 0` at 0x004D397B,
//                   `m_nPedState = PED_DEAD` at 0x0053BDCD/0x0053BE2F) and
//                   then, for anything that is not the player, vtable +0x40 -
//                   FlagToDestroyWhenNextProcessed;
//   anything else   CPed::SetDie (0x0053BDFE/0x0053BE60).
//
// `CBoat::BlowUpCar` (0x00541CB0) does the same at 0x00541D68.
//
// The SetDie arm is covered by the refusal above. The SetDead arm is not -
// it is a different address - and it is the reason the state test exists.
inline bool BlowUpCarUsesSetDieOnOccupant(uint32_t pedState) {
	return pedState != PEDSTATE_DRIVING;
}

inline bool BlowUpCarDestroysOccupant(uint32_t pedState) {
	return pedState == PEDSTATE_DRIVING;
}

// The CPed::SetDie detour's half of the refusal, kept here so the whole
// policy lives in population.cpp and game/combat.cpp only has to ask.
//
// Returns true when the call must not reach the engine: `ped` is a replica of
// somebody else's pedestrian and nothing in the session asked for it to die.
// The health is put back on the way out, for the reason PlanReplicaSetDie
// gives.
bool RefuseLocalReplicaDeath(void *ped, uint16_t &netId);

// While one of these is alive, RefuseLocalReplicaDeath says no to nothing.
//
// KillAmbientReplica carries out a death the *host* reported, and it does it
// by calling the engine's own CPed::SetDie - through the same address, so
// through the same detour. Without this the observer's half of a reported
// death would be refused by the observer's own guard, which is the one bug a
// refusal like this is guaranteed to introduce if nobody writes it down.
struct HostDeathScope {
	HostDeathScope();
	~HostDeathScope();
	HostDeathScope(const HostDeathScope &) = delete;
	HostDeathScope &operator=(const HostDeathScope &) = delete;
};

} // namespace coopiii::game
