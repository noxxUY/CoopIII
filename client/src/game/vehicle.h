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

// Puts the vehicle back where the session says it is, and pushes the matrix
// to the RenderWare frame. Called every frame after the world has updated -
// a remote car is still simulated locally, since that's what turns its
// wheels and works its suspension, so the correction has to be the last
// thing that touches it before the frame draws.
void CorrectRemoteVehicle(RemoteVehicle &vehicle, const VehicleTransform &at);

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
