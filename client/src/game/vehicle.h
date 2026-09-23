// Reading and writing CVehicles - the vehicle half of WorldBridge.
//
// Same rule as ped.h: nothing here uses an offset that hasn't been confirmed
// against the disassembly. All 22 CVehicle offsets and the whole CREATE_CAR
// path got verified on 2026-09-21 and carry their proof in addresses.h.
//
// ---------------------------------------------------------------------------
// Who owns a vehicle
// ---------------------------------------------------------------------------
//
// The driver does. A car with a driver gets simulated by that driver's
// machine, and every other machine is an observer that just writes down
// what it's told - transform, velocities, control positions - and lets its
// own physics do nothing. Same rule as a remote ped, same reason: two
// machines running physics on one object disagree within a second, and the
// disagreement looks like rubber-banding, not like an obvious bug.
//
// `STATUS_PLAYER_REMOTE` is NOT how this is done. Looks like the obvious
// answer, and it isn't - see docs/protocol.md §1.4.
#pragma once

#include "client.h"

#include <coopiii/protocol.h>

namespace coopiii::game {

// True when the local player is sitting in a vehicle (driving or riding).
bool LocalPlayerInVehicle();

// True when the local player is in this row's driver's seat, off the engine's
// own CVehicle::m_pDriver. See WorldBridge::LocalDrivesVehicle for why the
// roster's answer to the same question is not good enough.
bool LocalDrivesVehicle(const RemoteVehicle &vehicle);

// The live CVehicle behind a roster entry, or null when the engine no
// longer has it - in which case the entry's spawn gets re-armed on the way
// out, so the caller only ever has to handle "not right now".
//
// Public because seating a remote ped needs both a ped and a car, and the
// call that does it (CPed::WarpPedIntoCar) is a CPed method, so that code
// lives in ped.cpp. Nothing else outside this file should need it.
void *ResolveRemoteVehicle(RemoteVehicle &vehicle);

// Identity of the vehicle the local player is driving, for the claim packet
// that introduces it to the session (EnterVehicleBody in protocol.h). Returns
// false for the same reasons SampleLocalVehicle does.
//
// One struct rather than five out-params because the list grew: the extras
// joined the colours at protocol 9, and "add another reference argument" was
// already the wrong shape at four.
bool SampleLocalVehicleIdentity(VehicleIdentity &out);
int32_t SampleLocalVehicleHandle();

// Fills `out` from the vehicle the local player is driving. False when the
// player is on foot, is a passenger rather than the driver, or there's no
// player at all - in every one of those cases this machine isn't the owner
// and has nothing authoritative to say.
bool SampleLocalVehicle(VehicleStateBody &out);

// Creates a vehicle the engine's own way, following COMMAND_CREATE_CAR's
// sequence exactly (addresses.h, "vehicle lifecycle"). Sets
// RemoteVehicle::poolHandle. False if the model is not loaded or the
// constructor did not complete.
//
// Also applies the car's extras, and that has to happen here rather than
// anywhere else: they are RwAtomics cloned into the clump during construction,
// so the engine's CVehicleModelInfo::ms_compsToUse override is set immediately
// before the constructor call and put back immediately after. See the
// "Extras" note below and addresses.h.
bool SpawnRemoteVehicle(RemoteVehicle &vehicle);

// Removes it the engine's own way: CWorld::Remove,
// RemoveReferencesToDeletedObject, then the deleting destructor through the
// vehicle's own vtable - never the global operator delete, which is exactly
// what turned the ped despawn into a heap corruption.
void DespawnRemoteVehicle(RemoteVehicle &vehicle);

// Writes an observed state onto an already-spawned vehicle.
void ApplyRemoteVehicle(RemoteVehicle &vehicle, const VehicleStateBody &body);
void RestRemoteVehicle(RemoteVehicle &vehicle);

// Puts the vehicle back where the session says it is, and pushes the matrix
// to the RenderWare frame. Called every frame after the world has updated -
// a remote car is still simulated locally, since that's what turns its
// wheels and works its suspension, so the correction has to be the last
// thing that touches it before the frame draws.
void CorrectRemoteVehicle(RemoteVehicle &vehicle, const VehicleTransform &at);

// ---------------------------------------------------------------------------
// The damage model (docs/cardamage.md)
// ---------------------------------------------------------------------------
//
// Panels and doors, and nothing else. Wheels, lights and the engine status all
// converge on their own and the design says why with the disassembly; the
// short version is in protocol.h beside VehicleDamageBody.
//
// The shape of this is the one M4 settled for a ped and §1.11 settled for a
// wreck: read what this machine's own engine decided, put it on the wire as an
// event, and let every observer replay it through the engine's own function.
// What is new is only that damage accumulates, so the packet carries absolute
// state and the merge is a maximum.

// What shape the car the local player is driving is in. False for the same
// reasons SampleLocalVehicle is false, plus one more: a car that is already a
// wreck has nothing to report, because FuckCarCompletely gave it the same
// damage on every machine.
//
// `netId` is left at zero - netIds are the session's, and Client fills it.
bool SampleLocalVehicleDamage(VehicleDamageBody &out);

// Make an observed car wear the damage the session says it has.
//
// Writing the status bytes is NOT enough, which is the same lesson as the
// explosion: a panel you can see is an RpAtomic that was swapped or hidden
// when the status changed, so every component that moves needs its status
// written *and* CAutomobile::SetPanelDamage / SetBumperDamage / SetDoorDamage
// called for it - which is exactly the pair CReplay::ProcessCarUpdate does six
// times in a row, for exactly this problem.
//
// `flying` picks the appliers' `noFlyingComponents` argument, inverted:
//
//   true  - a change that arrived while the car is in front of you. The part
//           flies off. Each machine makes its own CObject for it, the same way
//           each machine breaks its own lamp posts.
//   false - a spawn or a backfill. A car that has been missing its boot for
//           ten minutes simply arrives without one, rather than greeting a
//           late joiner with a shower of doors out of the object pool. This is
//           the case CReplay passes `true` for, and the reason CoopIII does
//           not use CAutomobile::SetupDamageAfterLoad, which is the one-call
//           version and passes `false` unconditionally.
//
// Never lowers anything: the merge is a componentwise maximum, here as on the
// server, because no applier in the engine has an arm that restores an atomic.
void ApplyRemoteVehicleDamage(RemoteVehicle &vehicle,
                              const VehicleDamageBody &body, bool flying);

// Stop, or resume, this machine's own engine deciding a car's damage.
//
// One bit: CEntity::bCollisionProof. CAutomobile::VehicleDamage tests it once,
// unconditionally, and returns - no switch in front of it, unlike the ped case
// docs/protocol.md §1.10.2 is about - so it is a complete switch and not a
// backstop. Set on a car this machine is only watching, cleared the moment the
// local player takes the driver's seat, by the same test that decides whether
// to correct the transform.
//
// Without it an observer's own collisions keep denting a car it does not own,
// and because damage only climbs those invented dents are permanent.
void SetVehicleObserved(void *vehicle, bool observed);

// ---------------------------------------------------------------------------
// Destruction
// ---------------------------------------------------------------------------
//
// A car exploding is an event, and m_fHealth is only a number. Writing zero
// into an observer's copy of a remote car produces a car with no health that
// is not destroyed, because nothing in the engine watches health for zero -
// destruction goes through CAutomobile::BlowUpCar, and its callers are a
// damage transition, a bomb timer and a five-second fire timer. addresses.h
// has all three transcribed under "a car's destruction".
//
// So it travels as its own event, and the rule is CoopIII's usual one: the
// machine that owns the entity decides. A car's owner is its driver, so the
// driver's machine announces the blast and every observer replays it through
// the same engine function, at the transform the owner sends. One call gives
// both halves of what has to match - the explosion and the burnt shell it
// leaves - because BlowUpCar is where the engine decides both.
//
// A synced car with nobody driving it has no owner and therefore nobody
// entitled to destroy it. Those are left to each machine's own engine, the
// same way untouched traffic already is. docs/protocol.md §1.11.3.

// ---------------------------------------------------------------------------
// Ambient traffic (docs/population.md §3 step 4)
// ---------------------------------------------------------------------------
//
// The same four operations as a claimed car, on a car nobody is driving.
// game/population.cpp owns the seam that decides which cars these are and
// keeps the books; these are the engine calls, because everything they need -
// the CREATE_CAR sequence, the extras override, the matrix push - already
// lives in this file.
//
// The one difference that matters is in SpawnAmbientCarReplica's comment:
// VehicleCreatedBy is RANDOM_VEHICLE, not MISSION_VEHICLE, and it decides
// which engine counter the replica is counted in. Read it before changing it.

bool SpawnAmbientCarReplica(RemoteAmbientCar &car);
void DespawnAmbientCarReplica(RemoteAmbientCar &car);
void CorrectAmbientCarReplica(RemoteAmbientCar &car, const VehicleTransform &at);

// Reads one hosted car's transform and velocity out of the engine. False when
// the pool no longer has it, which is the caller's cue to drop the row.
bool SampleHostedCar(int32_t poolHandle, AmbientCarState &out);

// Where a car is and which way up it is, in the shape the unowned blow-up
// packet carries. Read at the moment a host notices one of its own traffic
// cars has become a wreck, which is the only moment that answer is worth
// anything - see BlastTransform in protocol.h.
//
// Split out rather than reusing SampleHostedCar because that one also reads
// velocity and needs a pool handle, and the sweep that calls this already has
// the pointer in hand: a wreck has no velocity worth sending and re-resolving
// a handle it just walked would be work for nothing.
bool ReadCarBlastTransform(void *vehicle, BlastTransform &out);

// Put a car exactly where a blow-up report says it blew up, immediately
// before running BlowUpCar on it. The order matters and is the one
// BlowUpRemoteVehicle has always used: BlowUpCar reads GetPosition() for the
// explosion, the camera shake and the fire it lights, so a car corrected
// afterwards moves its shell and leaves all three in the wrong street.
void PlaceCarForBlast(void *vehicle, const BlastTransform &where);

// Everything that describes what a car *is*, for the spawn announcement:
// model, both colours, both extras, and where it is right now.
bool SampleAmbientCarIdentity(void *vehicle, AmbientCarBody &out);

// CPools::GetVehicleRef / GetVehicle, so population.cpp can hold engine
// references rather than raw pointers - a handle stops resolving the moment
// the engine deletes the car, reused slot or not.
void   *AmbientCarFromRef(int32_t poolHandle);
int32_t AmbientCarRef(void *vehicle);

// `LocalVehicleBlast` is declared in client.h beside CombatEvent, because
// WorldBridge names it and client.h may not include this file.

// Detours CAutomobile::BlowUpCar. Not fatal if it fails, and the log says so:
// without it a car's destruction neither travels nor gets held back, which is
// exactly the behaviour this replaces.
bool InstallVehicleHooks();
void RemoveVehicleHooks();
bool VehicleHooksInstalled();

// Hands over the blasts the local player's own car suffered since the last
// call. Bounded, oldest first, same shape as DrainLocalCombat and for the
// same reason: a car blowing up is an event the engine knows about and a
// 25 Hz sample does not.
//
// The netId isn't in here. The detour only knows "the car the local player is
// driving"; Client is the half that knows what the session calls it.
uint8_t DrainLocalVehicleBlasts(LocalVehicleBlast *out, uint8_t max);

// Replay somebody else's car blowing up: put it where they say it ended up,
// then run the engine's own CAutomobile::BlowUpCar on it.
//
// Returns false when the car is no longer in the pool, which is a "not now"
// rather than an error - Client marks the entry destroyed either way, because
// a car whose owner says it is wrecked must never be respawned as a new one.
bool BlowUpRemoteVehicle(RemoteVehicle &vehicle, const Vec3 &pos, const Quat &rot);

// ---------------------------------------------------------------------------
// Cars nobody owns (docs/roadmap.md 5.8)
// ---------------------------------------------------------------------------
//
// The driver reports their own car. A parked car has no driver, so it had
// nobody - and blowing up a parked car is the commonest way a car is
// destroyed in this game.
//
// Two kinds of car, and this file owns one of them. A car the session has a
// row for - claimed, driven, parked, walked away from - travels as
// UNOWNED_SESSION with its netId, and the roster resolves that
// (Client::WreckSessionVehicle), because a netId is the session's name for a
// car rather than the engine's. What is below is the other kind.
//
// The name a parked car travels under is its car generator index. Every
// PARKED_VEHICLE comes out of CTheCarGenerators::CarGeneratorArray, the array
// is filled from the IPLs in file order, and the same files in the same order
// give the same index on every machine - so the map hands the identity out
// and the server never has to. addresses.h, "parked cars, and the name the
// map already gives them", has the proof, including that the generator's
// m_nVehicleHandle is a pool ref in exactly CPools::GetVehicleRef's encoding.
//
// The scope is deliberately one fact and not a state stream, and the reason
// is worth reading before anything is added to it. An explosion is already
// replayed on every machine at a position everyone agrees on, and
// CWorld::TriggerExplosion damages every car in the radius through
// CVehicle::InflictDamage with a multiplier that is a function of distance
// and nothing else. So a parked car blown up by a rocket is ALREADY a wreck
// everywhere. What does not converge is damage that accumulates - gunfire
// spread is rolled per machine, a shove from a replica is not a collision
// anybody simulated twice - and the engine's five-second fire timer turns a
// difference in health into a difference in whether the car ever explodes at
// all. This carries that difference and nothing else.
//
// Nothing else about an unowned car travels, on purpose:
//   - its position, because the map put it there on every machine and a
//     transform stream for every parked car in Liberty City is not a budget
//     anybody has (docs/population.md 2.1);
//   - its health, because writing health destroys nothing and a low one arms
//     the fire timer on the receiver, which is an observer deciding to
//     destroy somebody else's car five seconds later;
//   - its doors, panels and dents, because the cause already travels and the
//     divergence is a dent.

// The unowned cars this machine's engine destroyed since the last call. Same
// shape as DrainLocalVehicleBlasts, and off the same detour.
uint8_t DrainUnownedBlasts(UnownedBlast *out, uint8_t max);

// Make the car this key names a wreck here, through the engine's own
// BlowUpCar. Idempotent, and honest about "not right now": a backfilled wreck
// routinely names a car that is three streets away and not streamed in.
UnownedWreckOutcome WreckUnownedVehicle(const UnownedVehicleKey &key);

// Wires the two of those into the bridge. Separate from MakeWorldBridge for
// the same reason AddWorldToBridge is: they hang off a detour rather than the
// frame pump, and they install and fail on their own.
void AddVehicleBlastToBridge(WorldBridge &bridge);

// ---------------------------------------------------------------------------
// Extras
// ---------------------------------------------------------------------------
//
// "Hay autos que a un player le aparecen con extras que el otro jugador no ve
// y viceversa." The engine picks a car's extra components at spawn, on each
// machine, out of CVehicleModelInfo::ChooseComponent - so two machines roll
// independently and two players get two different cars.
//
// This is the same family of bug as the paint job and it carries on the wire
// next to it: EnterVehicleBody and S_VehicleSpawn now hold extra1/extra2, the
// claimer's own CVehicle::m_aExtras, and every observer fits those.
//
// It is NOT fixed the same way, and that is the only surprising thing here.
// A colour is read by the renderer every frame, so writing m_currentColour1/2
// after construction changes the car. m_aExtras is a record of a decision
// already taken: the components are RwAtomics that were cloned into the clump
// while the car was being built, and writing those two bytes afterwards
// changes the record and nothing on screen. So the choice is forced before
// the constructor, through the engine's own one-shot override, which exists
// because the garages restore a saved car's variation exactly this way.
//
// docs/protocol.md §1.12. The mechanism, its two traps, and the unchecked
// subscript that makes a wire value dangerous are in addresses.h under
// "a vehicle's extra components".

} // namespace coopiii::game
