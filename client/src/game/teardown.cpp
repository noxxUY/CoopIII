// The engine half of game/teardown.h.
#include "teardown.h"

#include "missionclear.h"
#include "ped.h"
#include "../log.h"

namespace coopiii::game {

namespace {

RemovalsInFlight g_inFlight;

bool g_saidStaleVehicle = false;
bool g_saidStalePed     = false;
bool g_saidRefused      = false;
bool g_saidGarage       = false;
bool g_saidCamera       = false;
bool g_saidBoat         = false;
bool g_saidOccupant     = false;
bool g_saidStaleObject  = false;
bool g_saidBadHandle    = false;

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

// The pool's GetAt through CPools, for a handle the pool can answer for.
void *At(uintptr_t poolGlobal, uintptr_t getter, int32_t handle) {
	if (handle < 0)
		return nullptr;   // "none", which every caller already means by it
	auto *const   pool = Global<uint8_t *>(poolGlobal);
	const int32_t size = pool ? Field<int32_t>(pool, object::POOL_SIZE) : 0;
	if (!PoolHandleShapeOk(handle, size)) {
		if (!g_saidBadHandle) {
			g_saidBadHandle = true;
			Log("teardown: a kept handle %08X is not one its pool of %d can answer for; "
			    "taken as gone (said once)",
			    static_cast<unsigned>(handle), static_cast<int>(size));
		}
		return nullptr;
	}
	return Func<void *(__cdecl *)(int32_t)>(getter)(handle);
}

void *LocalPlayerPed() { return Func<void *(__cdecl *)()>(FindPlayerPed)(); }

// The people in a car that is about to go, the way
// DestroyVehicleAndDriverAndPassengers does it: each one flagged, which takes
// it out of its seat (and nils its m_pMyVehicle) before the car is removed.
// False when the local player is aboard, and then nothing has been touched.
bool EndOccupants(void *vehicle) {
	void *const  me    = LocalPlayerPed();
	const size_t seats = 1 + offs::VEH_MAX_PASSENGERS;
	// In it, or on the way in or out. Not the last car he got out of, which
	// m_pMyVehicle goes on naming: its own reference nils that pointer when the
	// car is deleted, and keeping the car for it left every car a player had
	// left behind on his screen for ever (game/missionclear.h).
	if (LocalPlayerAboard(vehicle))
		return false;
	for (size_t i = 0; i < seats; ++i)
		if (me && Field<void *>(vehicle, offs::VEH_DRIVER + i * 4) == me)
			return false;

	for (size_t i = 0; i < seats; ++i) {
		void *&seat     = Field<void *>(vehicle, offs::VEH_DRIVER + i * 4);
		void *const ped = seat;
		const bool live = ped && PedIsLive(ped);
		// A mission's character riding in it (a girl following a player's copy
		// into his car): deleted, the script's next instruction on his handle
		// reads through the null GetAt gives it.
		const uint32_t state = live ? Field<uint32_t>(ped, offs::PED_STATE) : 0;
		const bool aliveMissionChar =
		    live && Field<uint8_t>(ped, offs::PED_CHAR_CREATED_BY) == CHAR_CREATED_BY_MISSION &&
		    state != PEDSTATE_DIE && state != PEDSTATE_DEAD;
		const OccupantEnd end =
		    HowToEndOccupant(ped != nullptr, false, live,
		                     live && Field<void *>(ped, offs::PED_MY_VEHICLE) == vehicle,
		                     aliveMissionChar);
		if (end == OccupantEnd::None)
			continue;
		if (!g_saidOccupant) {
			g_saidOccupant = true;
			Log("teardown: a car we are taking away still had somebody in seat %u; %s",
			    static_cast<unsigned>(i),
			    end == OccupantEnd::FlagPed     ? "handed them to the engine the way its own "
			                                      "garage delivery does"
			    : end == OccupantEnd::PutOnFoot ? "a mission's character, put on foot where "
			                                      "he sat, alive"
			                                    : "emptied the seat, the ped in it is gone");
		}
		if (end == OccupantEnd::PutOnFoot)
			PutPedOnFoot(ped, /*leftASeat=*/true);
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

bool LocalPlayerAboard(const void *vehicle) {
	void *const me = LocalPlayerPed();
	if (!me || !vehicle)
		return false;
	return LocalAboardCar(Field<void *>(me, offs::PED_MY_VEHICLE) == vehicle,
	                      Field<uint8_t>(me, offs::PED_IN_VEHICLE) != 0,
	                      Field<uint32_t>(me, offs::PED_STATE));
}

EntityState VehicleState(void *vehicle) {
	return StateIn(CPools__ms_pVehiclePool, offs::SIZEOF_AUTOMOBILE, vehicle);
}

bool PedPoolHasRoom() {
	auto *const pool = Global<uint8_t *>(CPools__ms_pPedPool);
	return pool && PoolHasFreeSlot(Field<uint8_t *>(pool, object::POOL_FLAGS),
	                               Field<int32_t>(pool, object::POOL_SIZE));
}

EntityState PedState(void *ped) {
	return StateIn(CPools__ms_pPedPool, offs::SIZEOF_PLAYER_PED, ped);
}

EntityState ObjectState(void *object) {
	return StateIn(CPools__ms_pObjectPool, object::OBJECT_POOL_STRIDE, object);
}

void *PedAt(int32_t handle) { return At(CPools__ms_pPedPool, CPools__GetPed, handle); }
void *VehicleAt(int32_t handle) {
	return At(CPools__ms_pVehiclePool, CPools__GetVehicle, handle);
}
void *ObjectAt(int32_t handle) { return At(CPools__ms_pObjectPool, CPools__GetObject, handle); }

bool BeingRemovedByUs(const void *entity) { return g_inFlight.Has(entity); }

size_t ForgetEngineRawPointersTo(void *entity) {
	if (!entity)
		return 0;
	const size_t garages =
	    ClearGarageTargets(Ptr<uint8_t>(CGarages__aGarages), NUM_GARAGES, SIZEOF_GARAGE,
	                       offs::GARAGE_TARGET, entity);
	if (garages != 0 && !g_saidGarage) {
		g_saidGarage = true;
		Log("teardown: a garage here was still waiting for a car we are taking away; "
		    "it waits for nothing now instead of for a freed slot (said once)");
	}

	const size_t camera = RepointCameraTargets(Ptr<uint8_t>(TheCamera), entity, LocalPlayerPed());
	if (camera != 0 && !g_saidCamera) {
		g_saidCamera = true;
		Log("teardown: the camera was still on something we are taking away; it is on "
		    "the player now, as it would be once the engine noticed (said once)");
	}

	size_t boats = 0;
	if (auto *const pool = Global<uint8_t *>(CPools__ms_pVehiclePool))
		boats = ClearBoatCulprits(Field<uint8_t *>(pool, object::POOL_ENTRIES),
		                          Field<uint8_t *>(pool, object::POOL_FLAGS),
		                          Field<int32_t>(pool, object::POOL_SIZE), offs::SIZEOF_AUTOMOBILE,
		                          entity);
	if (boats != 0 && !g_saidBoat) {
		g_saidBoat = true;
		Log("teardown: a burning boat here named something we are taking away as the one "
		    "who set it alight; it blows up blaming nobody instead of a freed slot (said once)");
	}

	// Peds whose m_pMyVehicle is this car: the engine's reference nils it under
	// them when the car goes, and a car animation of theirs still playing
	// would finish on no car (game/animcb.h).
	size_t peds = 0;
	if ((Field<uint8_t>(entity, offs::ENTITY_FLAGS) & 7) == ENTITY_TYPE_VEHICLE)
		peds = static_cast<size_t>(DropCarChainCallbacksOnCar(entity));
	return garages + camera + boats + peds;
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
	ForgetEngineRawPointersTo(ped);
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

bool DestroyObject(void *object, const char *who) {
	const EntityState state = ObjectState(object);
	if (state != EntityState::Live) {
		if (!g_saidStaleObject) {
			g_saidStaleObject = true;
			Log("teardown: %s asked to take away an object that is %s; left it alone "
			    "(said once)",
			    who, EntityStateName(state));
		}
		return false;
	}
	g_inFlight.Enter(object);
	UnlinkMoving(object, who);
	// COMMAND_DELETE_OBJECT's order: out of the world, the references, the
	// deleting destructor.
	Func<CdeclFn>(CWorld__Remove)(object);
	Func<CdeclFn>(CWorld__RemoveReferencesToDeletedObject)(object);
	DeleteThroughVtable(object);
	g_inFlight.Leave(object);
	return true;
}

} // namespace coopiii::game
