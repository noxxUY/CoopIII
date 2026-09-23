// A session car's copy, from being built to being let go - the parts of it
// that can be decided without the engine. game/carlife.cpp is the engine half.
//
// A copy is the CVehicle this machine builds for a car somebody else claimed
// (SpawnRemoteVehicle), or a traffic replica that became a session car when
// somebody took its wheel (AdoptPromotedCar). It is built MISSION_VEHICLE and
// locked, as CREATE_CAR builds a mission car, and that is what keeps the
// engine from reaping it: CanBeDeleted says no for MISSION_VEHICLE and every
// reaping site also tests bIsLocked (addresses.h, "READ THIS BEFORE WRITING
// THE SPAWN"). MISSION_VEHICLE also buys the rest of what a mission car gets:
// a hideout garage won't store and delete it (0x0042794C), Craig won't take it
// (0x00423BDB), CWorld::RemoveFallenCars puts it back on a road instead of
// deleting it. None of that is wanted any less for a copy, so the creator
// type stays.
//
// What was wrong with it is the two things MISSION_VEHICLE also costs, and
// both are fixed around the car rather than by changing what it is:
//
//   - it is counted in NumMissionCars, which is in the traffic generator's
//     sum against MaxNumberOfCarsInUse. Twelve copies and no traffic, cop car
//     or ambulance is ever generated again on that machine. Fixed by raising
//     the cap by exactly what the copies add (TrafficAllowance).
//   - CPools::SaveVehiclePool writes every empty MISSION_VEHICLE into the
//     single-player save. Fixed by having the copies read as something else
//     for the length of that one call (CreatedByDuringSave).
//
// Nothing in here reads a game global or calls a game function.
#pragma once

#include <cstddef>
#include <cstdint>

#include "addresses.h"

namespace coopiii::game {

// ---- the traffic budget ------------------------------------------------------

// How many terms of the generator's six-term sum one car adds
// (addresses.h, the sum at 0x00416710 in GenerateOneRandomCar). UpdateCarCount decides
// it off VehicleCreatedBy, and a RANDOM_VEHICLE police car goes into two
// counters at once: NumLawEnforcerCars and NumRandomCars (0x004202FF).
// PERMANENT_VEHICLE goes into NumPermanentCars, which the sum leaves out.
inline int TrafficSumShare(uint8_t createdBy, bool lawEnforcer) {
	switch (createdBy) {
	case VEHICLE_CREATED_BY_RANDOM:  return lawEnforcer ? 2 : 1;
	case VEHICLE_CREATED_BY_MISSION: return 1;
	case VEHICLE_CREATED_BY_PARKED:  return 1;
	default:                         return 0;   // PERMANENT, or not a car
	}
}

// The six counters the gates read, in the order the generator adds them.
struct CarCounters {
	int32_t random    = 0;   // 0x00943118
	int32_t law       = 0;   // 0x008F1B38
	int32_t mission   = 0;   // 0x008F1B54
	int32_t parked    = 0;   // 0x008F29E0
	int32_t fireOrAmb = 0;   // 0x00885BB0
	int32_t ambOrFire = 0;   // 0x009411F0
};

inline int32_t CarCounterSum(const CarCounters &c) {
	return c.random + c.law + c.mission + c.parked + c.fireOrAmb + c.ambOrFire;
}

// GenerateOneRandomCar's second gate, 0x00416710-0x00416739: `jl` carries on,
// so the engine makes another car only while the sum is under the cap.
inline bool GeneratorUnderCap(const CarCounters &c, int32_t cap) {
	return CarCounterSum(c) < cap;
}

// What MaxNumberOfCarsInUse has to be for the copies to cost nothing: the
// value the engine set at startup plus what the copies put into the sum. All
// three gates compare the same sum to the same global (addresses.h), so this
// one number fixes all three.
inline int32_t TrafficAllowance(int32_t engineCap, int32_t copiesShare) {
	if (copiesShare <= 0)
		return engineCap;
	return engineCap + copiesShare;
}

// ---- the save ------------------------------------------------------------------

// CPools::SaveVehiclePool's test, both walks (addresses.h, 0x004A20E0 and
// 0x004A21E2): nobody in any seat, a car or a boat, and MISSION_VEHICLE.
inline bool RetailSaveWrites(bool hasDriver, bool hasPassenger, int32_t vehType,
                             uint8_t createdBy) {
	if (hasDriver || hasPassenger)
		return false;
	if (vehType != VEHICLE_TYPE_CAR && vehType != VEHICLE_TYPE_BOAT)
		return false;
	return createdBy == VEHICLE_CREATED_BY_MISSION;
}

// What a car's VehicleCreatedBy reads while SaveVehiclePool runs.
//
// A copy of somebody else's car reads PERMANENT_VEHICLE, which the save skips,
// and is put back the moment the call returns. Only the save runs in between:
// nothing is constructed or destroyed, so UpdateCarCount never sees the
// borrowed value and the counters stay right. Any value but 2 would do;
// PERMANENT is the one whose meaning is closest, "not this game's to manage".
//
// A car this machine's own engine made is never touched, whoever has driven
// it since. If the campaign left a mission car in the street and the local
// player took it for a spin in the session, it belongs in his save exactly as
// much as it would have without CoopIII.
inline uint8_t CreatedByDuringSave(bool sessionCopy, uint8_t createdBy) {
	if (sessionCopy && createdBy == VEHICLE_CREATED_BY_MISSION)
		return static_cast<uint8_t>(VEHICLE_CREATED_BY_PERMANENT);
	return createdBy;
}

// ---- the end of a copy --------------------------------------------------------

// What to do with this machine's CVehicle when the session lets go of a car
// (S_VehicleDespawn) or the session itself goes away.
enum class CopyEnd : uint8_t {
	// A copy nobody here is in. Deleted through the engine's own destructor.
	Destroy,
	// A copy the local player is sitting in. The session only releases a car
	// nobody is in or near, so this is a race or a dropped connection - and
	// deleting the car around the player leaves CPed::bInVehicle set with
	// m_pMyVehicle nulled, which the engine follows. So the car is given to
	// the engine instead: RANDOM_VEHICLE, unlocked, counted like any other
	// car, and the next claim makes it a session car again.
	HandToEngine,
	// This engine's own car (RemoteVehicle::ours). Never CoopIII's to delete;
	// the row goes and the car stays, for the engine to remove by its own
	// rules like any car the player left behind.
	Leave,
};

inline CopyEnd HowToEndCopy(bool ours, bool localPlayerAboard) {
	if (ours)
		return CopyEnd::Leave;
	return localPlayerAboard ? CopyEnd::HandToEngine : CopyEnd::Destroy;
}

// ---- which CVehicles are copies -----------------------------------------------
//
// The save detour and the traffic allowance both need the answer inside the
// engine's own call, where the roster isn't to hand, so the engine seam keeps
// its own list: a handle goes in when CoopIII builds or adopts a copy, and
// comes out when CoopIII destroys it or gives it to the engine. A copy the
// engine deletes by itself (a wreck, a reload) just stops resolving - handles
// go dead when their slot is freed, and a dead one is reused, the same
// discipline as ObservedTable.
template <size_t N>
class CopyList {
public:
	void Clear() {
		for (int32_t &h : m_handles)
			h = -1;
	}

	template <class Resolve>
	bool Add(int32_t handle, Resolve resolve) {
		if (handle < 0)
			return true;
		for (int32_t h : m_handles)
			if (h == handle)
				return true;
		for (int32_t &h : m_handles)
			if (h < 0 || resolve(h) == nullptr) {
				h = handle;
				return true;
			}
		return false;
	}

	void Remove(int32_t handle) {
		for (int32_t &h : m_handles)
			if (h == handle)
				h = -1;
	}

	// Calls fn(vehicle) for every copy that is still in the pool.
	template <class Resolve, class Fn>
	void ForEachLive(Resolve resolve, Fn fn) const {
		for (int32_t h : m_handles) {
			if (h < 0)
				continue;
			if (void *const v = resolve(h))
				fn(v);
		}
	}

	template <class Resolve>
	bool Contains(const void *vehicle, Resolve resolve) const {
		if (!vehicle)
			return false;
		for (int32_t h : m_handles)
			if (h >= 0 && resolve(h) == vehicle)
				return true;
		return false;
	}

private:
	int32_t m_handles[N] = {};

public:
	CopyList() { Clear(); }
};

} // namespace coopiii::game
