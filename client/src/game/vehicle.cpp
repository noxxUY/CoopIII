#include "vehicle.h"

#include "addresses.h"
#include "pedanim.h"
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

} // namespace

// Resolves the handle, and if the engine has taken the vehicle away, says
// so once and re-arms the spawn. Mirrors ResolveRemote in ped.cpp.
//
// Not file-private, unlike its ped counterpart - seating a remote ped needs
// both halves, and the ped half (CPed::WarpPedIntoCar) lives in ped.cpp.
void *ResolveRemoteVehicle(RemoteVehicle &vehicle) {
	if (vehicle.poolHandle < 0)
		return nullptr;
	void *const v = VehicleFromRef(vehicle.poolHandle);
	if (!v) {
		Log("bridge: vehicle %u (handle %d) is gone from the pool; will respawn",
		    vehicle.netId, vehicle.poolHandle);
		vehicle.poolHandle   = -1;
		vehicle.spawnPending = true;
		vehicle.appliedFlags = 0xFF;
	}
	return v;
}

bool LocalPlayerInVehicle() {
	return PlayerVehicle() != nullptr;
}

bool SampleLocalVehicleIdentity(uint16_t &modelId, uint8_t &colour1,
                                uint8_t &colour2, Vec3 &pos, Quat &rot) {
	void *const vehicle = PlayerVehicle();
	if (!vehicle)
		return false;
	if (Field<void *>(vehicle, offs::VEH_DRIVER) != PlayerPed())
		return false;

	modelId = Field<uint16_t>(vehicle, offs::MODEL_INDEX);
	colour1 = Field<uint8_t>(vehicle, offs::VEH_COLOUR1);
	colour2 = Field<uint8_t>(vehicle, offs::VEH_COLOUR2);
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

	// VEHICLE_CREATED_BY_MISSION is half the deletion gate, and it's the
	// constructor's second argument. The other half is bIsLocked, below.
	Func<CtorFn>(CAutomobile__ctor)(mem, model,
	                                static_cast<uint8_t>(VEHICLE_CREATED_BY_MISSION));

	// Learned this the hard way on the ped spawn path: if the constructor
	// bailed, the object isn't a vehicle and everything after this writes
	// into garbage.
	if (Field<void *>(mem, 0) == nullptr) {
		Log("bridge: the CAutomobile constructor did not complete for model %u", model);
		return false;
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

	Func<AddFn>(CWorld__Add)(mem);

	// LEVEL_IGNORE, same reason as a remote ped: the engine culls entities
	// whose m_nZoneLevel disagrees with CGame::currLevel, and a remote
	// player can be standing on a different island from us entirely.
	Field<int8_t>(mem, offs::ZONE_LEVEL) = LEVEL_IGNORE;

	vehicle.poolHandle   = VehicleRef(mem);
	vehicle.appliedFlags = 0xFF;
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

void CorrectRemoteVehicle(RemoteVehicle &vehicle, const VehicleTransform &at) {
	void *const v = ResolveRemoteVehicle(vehicle);
	if (!v)
		return;
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

} // namespace coopiii::game
