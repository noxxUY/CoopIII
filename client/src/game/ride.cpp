#include "ride.h"

#include "addresses.h"
#include "log.h"
#include "pedanim.h"
#include "vehicle.h"

namespace coopiii::game {

namespace {

void *PlayerPed() {
	return Func<void *(__cdecl *)()>(FindPlayerPed)();
}

bool IsVehicle(void *entity) {
	return entity &&
	       (Field<uint8_t>(entity, offs::ENTITY_FLAGS) & 7) == ENTITY_TYPE_VEHICLE;
}

bool IsTrain(void *vehicle) {
	return IsVehicle(vehicle) && Field<int32_t>(vehicle, offs::VEH_TYPE) == VEHICLE_TYPE_TRAIN &&
	       Field<uintptr_t>(vehicle, offs::VTABLE) == CTrain__vtable;
}

RideFrame FrameOf(void *entity) {
	RideFrame f;
	f.pos   = ReadVec3(entity, offs::POSITION);
	f.right = ReadVec3(entity, offs::MATRIX_RIGHT);
	f.fwd   = ReadVec3(entity, offs::MATRIX_FWD);
	f.up    = ReadVec3(entity, offs::MATRIX_UP);
	return f;
}

// Where on `vehicle` the ped is, in its own frame.
void Measure(void *ped, void *vehicle, LocalRide &out) {
	const RideFrame f = FrameOf(vehicle);
	out.offset        = RideOffsetOf(f, ReadVec3(ped, offs::POSITION));
	out.heading       = WrapAngle(Field<float>(ped, offs::PED_ROT_CUR) - RideFrameHeading(f));
}

bool g_saidTrainMissing = false;

} // namespace

bool SampleLocalRide(LocalRide &out) {
	void *const ped = PlayerPed();
	if (!ped)
		return false;
	out = LocalRide{};

	// In a wagon. The engine puts the ped there itself every frame
	// (CWorld::Process -> SetPedPositionInTrain), off the wagon's own matrix,
	// which is exactly the case the world position lags.
	if (Field<bool>(ped, offs::PED_IN_VEHICLE)) {
		void *const vehicle = Field<void *>(ped, offs::PED_MY_VEHICLE);
		if (!IsTrain(vehicle))
			return false;   // a car seat: the seat code has that
		out.kind  = RIDE_TRAIN;
		out.track = Field<uint8_t>(vehicle, TRAIN_TRACK_ID);
		out.wagon = static_cast<uint16_t>(Field<int16_t>(vehicle, TRAIN_WAGON_ID));
		Measure(ped, vehicle, out);
		return true;
	}

	// On one: the roof of a bus, a boat's deck, a wagon's roof.
	void *const surface = Field<void *>(ped, offs::PED_CURRENT_PHYS_SURFACE);
	if (!IsVehicle(surface))
		return false;
	if (IsTrain(surface)) {
		out.kind  = RIDE_TRAIN;
		out.track = Field<uint8_t>(surface, TRAIN_TRACK_ID);
		out.wagon = static_cast<uint16_t>(Field<int16_t>(surface, TRAIN_WAGON_ID));
	} else {
		out.kind          = RIDE_VEHICLE;
		out.vehicleHandle = AmbientCarRef(surface);
		if (out.vehicleHandle < 0)
			return false;
	}
	Measure(ped, surface, out);
	return true;
}

bool VehicleRideFrame(int32_t handle, RideFrame &out) {
	void *const vehicle = AmbientCarFromRef(handle);
	if (!IsVehicle(vehicle))
		return false;
	out = FrameOf(vehicle);
	return true;
}

// The wagon, found by walking the vehicle pool the way the engine's own walks
// do (0x00418324: size at +8, flags at +4 with 0x80 free, entries at +0 strided
// by sizeof(CAutomobile)). Thirteen wagons in a pool of 110, once a frame per
// rider, so no cache.
bool TrainRideFrame(uint8_t track, uint16_t wagon, RideFrame &out) {
	if (track >= TRAIN_TRACKS || wagon >= TRAIN_WAGONS_ON_TRACK[track])
		return false;
	auto *const pool = Global<uint8_t *>(CPools__ms_pVehiclePool);
	if (!pool)
		return false;
	uint8_t *const entries = Field<uint8_t *>(pool, object::POOL_ENTRIES);
	uint8_t *const flags   = Field<uint8_t *>(pool, object::POOL_FLAGS);
	const int32_t  size    = Field<int32_t>(pool, object::POOL_SIZE);
	if (!entries || !flags || size <= 0 || size > VEHICLE_POOL_SIZE)
		return false;
	for (int32_t i = 0; i < size; ++i) {
		if (flags[i] & object::POOLFLAG_ISFREE)
			continue;
		void *const v = entries + static_cast<size_t>(i) * offs::SIZEOF_AUTOMOBILE;
		if (!IsTrain(v) || Field<uint8_t>(v, TRAIN_TRACK_ID) != track ||
		    static_cast<uint16_t>(Field<int16_t>(v, TRAIN_WAGON_ID)) != wagon)
			continue;
		out = FrameOf(v);
		return true;
	}
	if (!g_saidTrainMissing) {
		g_saidTrainMissing = true;
		Log("ride: a player rides wagon %u of track %u and we have no such wagon; they "
		    "stay on their world position",
		    wagon, track);
	}
	return false;
}

} // namespace coopiii::game
