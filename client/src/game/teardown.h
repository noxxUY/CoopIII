// Taking an engine entity out of the world on CoopIII's own initiative, in one
// place, and only when it is still there to take.
//
// **The crash this exists for.** A participant's game died at 0x004AEA77, the
// instruction after `call [esi+8]` in CWorld::Remove, the moment the mission
// that owned Luigi's lockup was passed. The car handed to CWorld::Remove had
// already been destroyed: its vtable was CPlaceable's (0x005F6A28), whose slot
// 2 is a zero, so the virtual Remove called address 0 and the reported
// address is where it would have come back to.
//
// What handed it over was the participant's own garage. The mission ran
// SET_TARGET_CAR_FOR_MISSION_GARAGE (021B) on the owner's machine, CoopIII
// replayed it on the participant with the participant's copy of the car, and
// retail's CGarages::SetTargetCarForMissonGarage (0x00426BD0) stores the
// pointer and nothing else - no RegisterReference, unlike re3's version of it.
// So when the owner's garage shut on the car and the session ended it
// everywhere, CoopIII deleted the copy and the participant's garage went on
// holding a raw pointer to a freed slot. Its GARAGE_MISSION closing arm then
// hands m_pTarget to DestroyVehicleAndDriverAndPassengers, which is the
// CWorld::Remove above.
//
// **The rule.** Nothing CoopIII destroys may still be named by an engine
// container that the engine itself does not clean up. For vehicles the
// garages' m_pTarget is that container: CWorld::RemoveReferencesToDeletedObject
// walks the ped, vehicle and object pools and nothing else, and ~CEntity only
// resolves references somebody registered. Two more hold cars and peds alike:
// the camera's target until CCamera::Process registers it, and a boat's
// m_pSetOnFireEntity, which nothing ever registers.
//
// **The helper.** DestroyVehicle and DestroyPed are the only way CoopIII
// takes an entity out of the world. Each one first proves the pointer is
// still a live entity in its own pool - the slot's free bit clear, and a
// vtable that is not the CPlaceable one a finished destructor leaves behind -
// and that it is not already being taken apart further up the stack. A stale
// one is skipped and said once. Then, in the engine's own order
// (DestroyVehicleAndDriverAndPassengers, 0x00552760): the people in it, the
// raw pointers nobody else clears, the moving list, the world, the
// references, and the deleting destructor through the object's own vtable.
//
// A pointer cannot be validated with CPools::GetVehicleRef and GetVehicle:
// GetVehicleRef (0x00429050) builds the handle out of whatever the flag byte
// says now, free bit included, and GetAt (0x0043EAF0) only compares the two,
// so a freed pointer round-trips to itself. The free bit has to be tested.
#pragma once

#include <cstddef>
#include <cstdint>

#include "addresses.h"

namespace coopiii::game {

// ---- the parts that do not need a running game ----------------------------

// Which slot of a CPool a pointer is the start of, or -1 when it is not the
// start of any: outside the entry array, past `size`, or part-way into one.
inline int32_t PoolSlotOf(uintptr_t entries, size_t stride, int32_t size, uintptr_t p) {
	if (entries == 0 || stride == 0 || size <= 0 || p < entries)
		return -1;
	const uintptr_t delta = p - entries;
	if (delta % stride != 0)
		return -1;
	const uintptr_t index = delta / stride;
	return index < static_cast<uintptr_t>(size) ? static_cast<int32_t>(index) : -1;
}

enum class EntityState : uint8_t {
	Live         = 0,
	NotInPool    = 1,   // not the start of a slot of the pool it should be in
	FreedSlot    = 2,   // the slot's POOLFLAG_ISFREE is set
	Destructed   = 3,   // the destructor chain has run: CPlaceable's vtable
	BeingRemoved = 4,   // already on its way out further up this stack
};

// `flag` is the pool's flag byte for `slot`, and only read when slot >= 0.
inline EntityState ClassifyEntity(int32_t slot, uint8_t flag, uintptr_t vtable,
                                  bool beingRemoved) {
	if (slot < 0)
		return EntityState::NotInPool;
	if (flag & object::POOLFLAG_ISFREE)
		return EntityState::FreedSlot;
	if (vtable == 0 || vtable == CPlaceable__vtable)
		return EntityState::Destructed;
	if (beingRemoved)
		return EntityState::BeingRemoved;
	return EntityState::Live;
}

// Whether a pool handle can even be asked about. CPool::GetAt (0x0043EAF0
// and its siblings) is `sar eax,8 / movzx ecx,byte [flags+eax]` with no
// bound on either side, so a handle of -1 reads the byte before the flag
// array and one past the pool's size reads past it; if that stray byte
// happens to equal the handle's low byte, GetAt hands back a pointer outside
// the entry array. A low byte with the free bit set can only have come from
// a slot that was free when the handle was made (GetVehicleRef on a pointer
// that had already gone, the note at the top), and GetAt would resolve it
// straight back to that freed slot.
inline bool PoolHandleShapeOk(int32_t handle, int32_t poolSize) {
	if (handle < 0 || poolSize <= 0)
		return false;
	if (handle & object::POOLFLAG_ISFREE)
		return false;
	return (handle >> 8) < poolSize;
}

inline const char *EntityStateName(EntityState s) {
	switch (s) {
	case EntityState::Live:         return "live";
	case EntityState::NotInPool:    return "not in its pool";
	case EntityState::FreedSlot:    return "in a freed pool slot";
	case EntityState::Destructed:   return "already destroyed";
	case EntityState::BeingRemoved: return "already being removed";
	}
	return "?";
}

// The entities being taken apart right now, so a teardown that reaches back
// into itself - a destructor calling CWorld::Remove, a detour on the way -
// cannot take the same one apart twice. Tiny on purpose: nothing nests more
// than a couple deep.
class RemovalsInFlight {
public:
	static constexpr size_t CAPACITY = 8;

	bool Has(const void *p) const {
		for (size_t i = 0; i < m_count; ++i)
			if (m_items[i] == p)
				return true;
		return false;
	}
	// False when it is already in flight, or when there is no room; either
	// way the caller must not go on.
	bool Enter(const void *p) {
		if (!p || Has(p) || m_count >= CAPACITY)
			return false;
		m_items[m_count++] = p;
		return true;
	}
	void Leave(const void *p) {
		for (size_t i = 0; i < m_count; ++i)
			if (m_items[i] == p) {
				m_items[i] = m_items[--m_count];
				return;
			}
	}
	size_t Count() const { return m_count; }

private:
	const void *m_items[CAPACITY] = {};
	size_t      m_count           = 0;
};

// Every garage in `garages` (count of them, `stride` apart) whose pointer at
// `targetOffset` is `car` has it nulled. How many were. Written over plain
// memory so a test can hand it an array of its own.
inline size_t ClearGarageTargets(uint8_t *garages, size_t count, size_t stride,
                                 size_t targetOffset, const void *car) {
	if (!garages || !car)
		return 0;
	size_t cleared = 0;
	for (size_t i = 0; i < count; ++i) {
		void *&target = *reinterpret_cast<void **>(garages + i * stride + targetOffset);
		if (target == car) {
			target = nullptr;
			++cleared;
		}
	}
	return cleared;
}

// The camera's hold on an entity that is about to go (addresses.h, "who the
// camera is looking at"). pTargetEntity at `camera + CAMERA_TARGET` becomes
// `player` when it is the one going, the way CCamera::Process replaces a nil
// one with FindPlayerPed(); then every CCam whose CamTargetEntity is the one
// going takes pTargetEntity, the way Process fills a nil one. How many of the
// four pointers changed. Plain memory, so a test can hand it a buffer.
inline size_t RepointCameraTargets(uint8_t *camera, const void *dying, void *player) {
	if (!camera || !dying)
		return 0;
	size_t changed = 0;
	void *&target  = *reinterpret_cast<void **>(camera + offs::CAMERA_TARGET);
	if (target == dying) {
		target = player == dying ? nullptr : player;
		++changed;
	}
	for (size_t i = 0; i < CAMERA_NUM_CAMS; ++i) {
		void *&cam = *reinterpret_cast<void **>(camera + offs::CAMERA_CAMS +
		                                        i * CAMERA_CAM_STRIDE + offs::CAM_TARGET_ENTITY);
		if (cam == dying) {
			cam = target;
			++changed;
		}
	}
	return changed;
}

// Every live boat in a vehicle pool (`entries`, `flags`, `size`, `stride`
// apart) whose m_pSetOnFireEntity is `dying` has it nilled: the one culprit
// the engine keeps without a reference (addresses.h, BOAT_SET_ON_FIRE_STORE).
// A free slot is never read past its flag byte. How many.
inline size_t ClearBoatCulprits(uint8_t *entries, const uint8_t *flags, int32_t size,
                                size_t stride, const void *dying) {
	if (!entries || !flags || size <= 0 || stride == 0 || !dying)
		return 0;
	size_t cleared = 0;
	for (int32_t i = 0; i < size; ++i) {
		if (flags[i] & object::POOLFLAG_ISFREE)
			continue;
		uint8_t *const v = entries + static_cast<size_t>(i) * stride;
		if (*reinterpret_cast<const int32_t *>(v + offs::VEH_TYPE) != VEHICLE_TYPE_BOAT)
			continue;
		void *&culprit = *reinterpret_cast<void **>(v + offs::BOAT_SET_ON_FIRE_ENTITY);
		if (culprit == dying) {
			culprit = nullptr;
			++cleared;
		}
	}
	return cleared;
}

// What to do with somebody still sitting in a car that is about to go, in the
// engine's own order. The local player is never touched: a car he is in is
// not destroyed at all (the callers hand it to the engine instead).
enum class OccupantEnd : uint8_t {
	None       = 0,   // an empty seat
	ClearSeat  = 1,   // a pointer to a ped that is gone, or that thinks it is
	                  // in some other car: only the seat is ours to empty
	FlagPed    = 2,   // CPed::FlagToDestroyWhenNextProcessed, which gets it out
	                  // of the seat and hands it to the engine to delete
	RefuseCar  = 3,   // the local player: the car must not go
	PutOnFoot  = 4,   // a living character of a mission script: out of the
	                  // seat alive, since the script still holds his handle and
	                  // its next char instruction on a deleted one reads a null
};

inline OccupantEnd HowToEndOccupant(bool present, bool isLocalPlayer, bool pedLive,
                                    bool thinksItIsInThisCar, bool aliveMissionChar = false) {
	if (!present)
		return OccupantEnd::None;
	if (isLocalPlayer)
		return OccupantEnd::RefuseCar;
	if (!pedLive || !thinksItIsInThisCar)
		return OccupantEnd::ClearSeat;
	return aliveMissionChar ? OccupantEnd::PutOnFoot : OccupantEnd::FlagPed;
}

// A free slot in a pool's flags (POOLFLAG_ISFREE, 0x80): CPopulation::AddPed
// goes on with a null when CPed::operator new finds none (0x004F53A2).
inline bool PoolHasFreeSlot(const uint8_t *flags, int32_t size) {
	if (!flags || size <= 0)
		return false;
	for (int32_t i = 0; i < size; ++i)
		if ((flags[i] & 0x80) != 0)
			return true;
	return false;
}

// Whether the engine's ped pool has a free slot.
bool PedPoolHasRoom();

// ---- the engine side ----------------------------------------------------------

EntityState VehicleState(void *vehicle);
EntityState PedState(void *ped);
EntityState ObjectState(void *object);

// CPools::GetPed / GetVehicle / GetObject, for a handle that has been kept
// across frames or read off a script: null for one PoolHandleShapeOk turns
// away, instead of the engine reading outside its flag array.
void *PedAt(int32_t handle);
void *VehicleAt(int32_t handle);
void *ObjectAt(int32_t handle);
inline bool VehicleIsLive(void *vehicle) { return VehicleState(vehicle) == EntityState::Live; }
inline bool PedIsLive(void *ped) { return PedState(ped) == EntityState::Live; }

// Nulls every raw pointer to `entity` the engine keeps without a registered
// reference: the 32 garages' m_pTarget, the camera's target between a
// TakeControl and the next CCamera::Process (put on the player instead, as
// Process would), and a burning boat's culprit. For a car, also the callbacks
// of the car animations on every ped whose m_pMyVehicle it is, which the
// car's own reference is about to nil (ped.h, DropCarChainCallbacksOnCar).
// Called by DestroyVehicle and
// DestroyPed, and before anything that deletes a car through the engine
// (DELETE_CAR, a garage's own delivery). How many there were.
size_t ForgetEngineRawPointersTo(void *entity);

// Whether the local player is in `vehicle`, or walking to its door, getting
// in or getting out (game/missionclear.h, LocalAboardCar). Not merely the
// last car he left, which m_pMyVehicle still names.
bool LocalPlayerAboard(const void *vehicle);

// The one way CoopIII takes a vehicle out of the world. False, having done
// nothing, for one that is not live (said once) or that the local player is
// in. `who` names the caller in that line.
bool DestroyVehicle(void *vehicle, const char *who);

// The same for a pedestrian CoopIII made. `countedMissionPed` undoes
// CREATE_CHAR's ++ms_nTotalMissionPeds.
bool DestroyPed(void *ped, bool countedMissionPed, const char *who);

// The same for an object the engine made and CoopIII takes away on its
// behalf: a pickup somebody else collected, a mine somebody else's copy of
// set off. What the engine's own pickup arms do (0x00430FAC, and
// 0x004309F0..0x00430A15 for a mine) - CWorld::Remove and the deleting
// destructor - after the checks above, with the moving-list unlink and
// DELETE_OBJECT's reference sweep in between.
bool DestroyObject(void *object, const char *who);

// Whether `entity` is being taken apart by one of the three above right now.
bool BeingRemovedByUs(const void *entity);

} // namespace coopiii::game
