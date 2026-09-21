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
bool SampleLocalVehicleIdentity(uint16_t &modelId, uint8_t &colour1,
                                uint8_t &colour2, Vec3 &pos, Quat &rot);

// Fills `out` from the vehicle the local player is driving. False when the
// player is on foot, is a passenger rather than the driver, or there's no
// player at all - in every one of those cases this machine isn't the owner
// and has nothing authoritative to say.
bool SampleLocalVehicle(VehicleStateBody &out);

// Creates a vehicle the engine's own way, following COMMAND_CREATE_CAR's
// sequence exactly (addresses.h, "vehicle lifecycle"). Sets
// RemoteVehicle::poolHandle. False if the model is not loaded or the
// constructor did not complete.
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

} // namespace coopiii::game
