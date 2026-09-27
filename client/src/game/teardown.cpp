// The engine half of game/teardown.h.
#include "teardown.h"

#include "../log.h"

namespace coopiii::game {

namespace {

RemovalsInFlight g_inFlight;

bool g_saidStaleVehicle = false;
bool g_saidStalePed     = false;
bool g_saidRefused      = false;
bool g_saidGarage       = false;
bool g_saidOccupant     = false;

using ThisFn    = void(__thiscall *)(void *);
using CdeclFn   = void(__cdecl *)(void *);
using DeleteFn  = void *(__thiscall *)(void *, int);

EntityState StateIn(uintptr_t poolGlobal, size_t stride, void *entity) {
	if (!entity)
		return EntityState::NotInPool;
	auto *const pool = Global<uint8_t *>(poolGlobal);
	if (!pool)
		return EntityState::NotInPool;
	const uintptr_t entries = Field<uintptr_t>(pool, object::POOL_ENTRIES);
	auto *const     flags   = Field<uint8_t *>(pool, object::POOL_FLAGS);
	const int32_t   size    = Field<int32_t>(pool, object::POOL_SIZE);
	const int32_t   slot    = PoolSlotOf(entries, stride, size, reinterpret_cast<uintptr_t>(entity));
	if (slot < 0 || !flags)
		return EntityState::NotInPool;
	// Read only once the slot is known to be one of the pool's own, so the
	// vtable read is always inside the pool's entry array.
	return ClassifyEntity(slot, flags[slot], Field<uintptr_t>(entity, offs::VTABLE),
	                      g_inFlight.Has(entity));
}

void *LocalPlayerPed() { return Func<void *(__cdecl *)()>(FindPlayerPed)(); }

// The people in a car that is about to go, the way
// DestroyVehicleAndDriverAndPassengers does it: each one flagged, which takes
// it out of its seat (and nils its m_pMyVehicle) before the car is removed.
// False when the local player is aboard, and then nothing has been touched.
bool EndOccupants(void *vehicle) {
	void *const  me    = LocalPlayerPed();
	const size_t seats = 1 + offs::VEH_MAX_PASSENGERS;
	// m_pMyVehicle is set on the way in and on the way out as well as in a
	// seat, so it is the wider of the two tests.
	if (me && Field<void *>(me, offs::PED_MY_VEHICLE) == vehicle)
		return false;
	for (size_t i = 0; i < seats; ++i)
		if (me && Field<void *>(vehicle, offs::VEH_DRIVER + i * 4) == me)
			return false;

	for (size_t i = 0; i < seats; ++i) {
		void *&seat     = Field<void *>(vehicle, offs::VEH_DRIVER + i * 4);
		void *const ped = seat;
		const bool live = ped && PedIsLive(ped);
		const OccupantEnd end =
		    HowToEndOccupant(ped != nullptr, false, live,
		                     live && Field<void *>(ped, offs::PED_MY_VEHICLE) == vehicle);
		if (end == OccupantEnd::None)
			continue;
		if (!g_saidOccupant) {
			g_saidOccupant = true;
			Log("teardown: a car we are taking away still had somebody in seat %u; %s",
			    static_cast<unsigned>(i),
			    end == OccupantEnd::FlagPed ? "handed them to the engine the way its own "
			                                  "garage delivery does"
			                                : "emptied the seat, the ped in it is gone");
		}
		if (end == OccupantEnd::FlagPed) {
			void *const *vt = *reinterpret_cast<void *const *const *>(ped);
			reinterpret_cast<ThisFn>(vt[VTABLE_FLAG_TO_DESTROY])(ped);
		}
		// FlagToDestroyWhenNextProcessed empties the seat itself for a ped
		// that is in this car; this covers the one that was not.
		seat = nullptr;
	}
	return true;
}

// Unlinked by hand before CWorld::Remove, which will not do it for an entity
// that went static (addresses.h, WorldRemoveUnlinksFromMovingList).
// RemoveFromMovingList checks m_movingListNode itself.
void UnlinkMoving(void *entity, const char *who) {
	if (NeedsMovingListUnlink(Field<uint8_t>(entity, offs::ENTITY_FLAGS_A),
	                          Field<void *>(entity, offs::MOVING_LIST_NODE) != nullptr))
		Log("teardown: %s went static while still in the moving list; unlinking it "
		    "by hand",
		    who);
	Func<ThisFn>(CPhysical__RemoveFromMovingList)(entity);
}

void DeleteThroughVtable(void *entity) {
	// Slot 0 with 1: the class's own deleting destructor, so the slot goes
	// back to its pool. Never the global operator delete.
	void *const *vt = *reinterpret_cast<void *const *const *>(entity);
	reinterpret_cast<DeleteFn>(vt[VTABLE_DELETING_DTOR])(entity, 1);
}

} // namespace

EntityState VehicleState(void *vehicle) {
	return StateIn(CPools__ms_pVehiclePool, offs::SIZEOF_AUTOMOBILE, vehicle);
}

EntityState PedState(void *ped) {
	return StateIn(CPools__ms_pPedPool, offs::SIZEOF_PLAYER_PED, ped);
}

bool BeingRemovedByUs(const void *entity) { return g_inFlight.Has(entity); }

size_t ForgetEngineRawPointersTo(void *vehicle) {
	const size_t cleared =
	    ClearGarageTargets(Ptr<uint8_t>(CGarages__aGarages), NUM_GARAGES, SIZEOF_GARAGE,
	                       offs::GARAGE_TARGET, vehicle);
	if (cleared != 0 && !g_saidGarage) {
		g_saidGarage = true;
		Log("teardown: a garage here was still waiting for a car we are taking away; "
		    "it waits for nothing now instead of for a freed slot (said once)");
	}
	return cleared;
}

bool DestroyVehicle(void *vehicle, const char *who) {
	const EntityState state = VehicleState(vehicle);
	if (state != EntityState::Live) {
		if (!g_saidStaleVehicle) {
			g_saidStaleVehicle = true;
			Log("teardown: %s asked to take away a car that is %s; left it alone "
			    "(said once)",
			    who, EntityStateName(state));
		}
		return false;
	}
	if (!EndOccupants(vehicle)) {
		if (!g_saidRefused) {
			g_saidRefused = true;
			Log("teardown: %s asked to take away the car the local player is in; "
			    "refused (said once)",
			    who);
		}
		return false;
	}
	g_inFlight.Enter(vehicle);
	ForgetEngineRawPointersTo(vehicle);
	UnlinkMoving(vehicle, who);
	Func<CdeclFn>(CWorld__Remove)(vehicle);
	Func<CdeclFn>(CWorld__RemoveReferencesToDeletedObject)(vehicle);
	DeleteThroughVtable(vehicle);
	g_inFlight.Leave(vehicle);
	return true;
}

bool DestroyPed(void *ped, bool countedMissionPed, const char *who) {
	const EntityState state = PedState(ped);
	if (state != EntityState::Live) {
		if (!g_saidStalePed) {
			g_saidStalePed = true;
			Log("teardown: %s asked to take away a pedestrian that is %s; left it "
			    "alone (said once)",
			    who, EntityStateName(state));
		}
		return false;
	}
	if (ped == LocalPlayerPed())
		return false;
	g_inFlight.Enter(ped);
	UnlinkMoving(ped, who);
	// COMMAND_DELETE_CHAR's own teardown from here: the references, then the
	// deleting destructor, whose ~CPed opens with CWorld::Remove(this).
	Func<CdeclFn>(CWorld__RemoveReferencesToDeletedObject)(ped);
	DeleteThroughVtable(ped);
	g_inFlight.Leave(ped);
	if (countedMissionPed) {
		uint32_t &n = Global<uint32_t>(CPopulation__ms_nTotalMissionPeds);
		if (n > 0)
			--n;
	}
	return true;
}

} // namespace coopiii::game
