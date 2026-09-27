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
// resolves references somebody registered.
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
};

inline OccupantEnd HowToEndOccupant(bool present, bool isLocalPlayer, bool pedLive,
                                    bool thinksItIsInThisCar) {
	if (!present)
		return OccupantEnd::None;
	if (isLocalPlayer)
		return OccupantEnd::RefuseCar;
	if (!pedLive || !thinksItIsInThisCar)
		return OccupantEnd::ClearSeat;
	return OccupantEnd::FlagPed;
}

// ---- the engine side ----------------------------------------------------------

EntityState VehicleState(void *vehicle);
EntityState PedState(void *ped);
inline bool VehicleIsLive(void *vehicle) { return VehicleState(vehicle) == EntityState::Live; }
inline bool PedIsLive(void *ped) { return PedState(ped) == EntityState::Live; }

// Nulls every raw pointer to `vehicle` the engine keeps without a registered
// reference - the 32 garages' m_pTarget. Called by DestroyVehicle, and before
// anything that deletes a car through the engine (DELETE_CAR, a garage's own
// delivery). How many there were.
size_t ForgetEngineRawPointersTo(void *vehicle);

// The one way CoopIII takes a vehicle out of the world. False, having done
// nothing, for one that is not live (said once) or that the local player is
// in. `who` names the caller in that line.
bool DestroyVehicle(void *vehicle, const char *who);

// The same for a pedestrian CoopIII made. `countedMissionPed` undoes
// CREATE_CHAR's ++ms_nTotalMissionPeds.
bool DestroyPed(void *ped, bool countedMissionPed, const char *who);

// Whether `entity` is being taken apart by one of the two above right now.
bool BeingRemovedByUs(const void *entity);

} // namespace coopiii::game
