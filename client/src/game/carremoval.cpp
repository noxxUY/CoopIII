// The engine half of game/carremoval.h.
#include "carremoval.h"

#include "carlife.h"
#include "leadcheck.h"
#include "teardown.h"
#include "vehicle.h"
#include "../log.h"

#include <windows.h>

#include <cstring>

namespace coopiii::game {

namespace {

// Client::MAX_REMOTE_VEHICLES.
constexpr size_t MAX_ROWS = 64;

// Every session car with a CVehicle here, rewritten once a frame by the
// roster (NoteVehicleRemovers). Handles, never pointers: a car the engine
// deletes stops resolving, and a stale row can then match nothing.
VehicleRemover g_rows[MAX_ROWS];
uint8_t        g_rowCount = 0;

// What our engine took since Client last asked, and the netIds it has taken
// at all, for ResolveRemoteVehicle to ask about until the row is marked.
constexpr size_t MAX_PENDING = 16;
VehicleRemoval   g_pending[MAX_PENDING];
uint8_t          g_pendingCount = 0;
constexpr size_t MAX_TAKEN = 32;
uint16_t         g_taken[MAX_TAKEN] = {};
uint8_t          g_takenNext = 0;

bool g_saidRefusedDestroy = false;
bool g_saidCrusher        = false;
bool g_saidCrane          = false;
bool g_saidStore          = false;
bool g_saidStaleTarget    = false;
bool g_saidStaleCrane     = false;

void *CarAt(int32_t handle) { return handle < 0 ? nullptr : AmbientCarFromRef(handle); }

const VehicleRemover *RowFor(const void *vehicle) {
	if (!vehicle)
		return nullptr;
	for (uint8_t i = 0; i < g_rowCount; ++i)
		if (CarAt(g_rows[i].poolHandle) == vehicle)
			return &g_rows[i];
	return nullptr;
}

// May this engine take `vehicle` away? `session` says whether it is one of the
// session's cars (and then `netId` names it). A car somebody else's machine
// hosts as traffic is not ours to crush either; a car only this machine has
// always is.
bool MayTake(void *vehicle, uint16_t &netId, bool &session) {
	netId   = INVALID_NETID;
	session = false;
	if (!vehicle)
		return true;
	if (const VehicleRemover *r = RowFor(vehicle)) {
		netId   = r->netId;
		session = true;
		return r->weMay;
	}
	uint16_t id     = INVALID_NETID;
	bool     others = false;
	if (SessionCarFor(vehicle, id, others))
		return !others;
	return true;
}

void PushRemoval(uint16_t netId, uint8_t reason) {
	if (netId == INVALID_NETID)
		return;
	g_taken[g_takenNext] = netId;
	g_takenNext          = static_cast<uint8_t>((g_takenNext + 1) % MAX_TAKEN);
	for (uint8_t i = 0; i < g_pendingCount; ++i)
		if (g_pending[i].netId == netId)
			return;
	if (g_pendingCount < MAX_PENDING)
		g_pending[g_pendingCount++] = VehicleRemoval{netId, reason};
	Log("carremoval: our engine took session car %u away (reason %u)", netId,
	    static_cast<unsigned>(reason));
}

// ---- the four garage deliveries -------------------------------------------

using DestroyFn = void(__cdecl *)(void *);

void DestroyFor(void *vehicle, uint8_t reason) {
	// The garage's own pointer, which is raw: SetTargetCarForMissonGarage
	// (0x00426BD0) registers no reference, so nothing tells a garage its car
	// has gone. A car CoopIII already deleted is refused here rather than
	// handed to CWorld::Remove, which would call through the vtable a
	// finished destructor leaves behind (game/teardown.h). The engine nils
	// the garage's target itself on the way back, so refusing is all it takes.
	const EntityState state = VehicleState(vehicle);
	if (state != EntityState::Live) {
		if (!g_saidStaleTarget) {
			g_saidStaleTarget = true;
			Log("carremoval: a garage here tried to take away a car that is %s; left "
			    "it alone (said once)",
			    EntityStateName(state));
		}
		return;
	}
	uint16_t netId   = INVALID_NETID;
	bool     session = false;
	if (!MayTake(vehicle, netId, session)) {
		// A car another machine holds. The crusher can no longer pick one
		// and every other delivery takes the local player's own car, so this
		// is a race; leaving the car is the safe half of it.
		if (!g_saidRefusedDestroy) {
			g_saidRefusedDestroy = true;
			Log("carremoval: a garage here tried to take session car %u, which "
			    "another machine holds; left it (said once)", netId);
		}
		return;
	}
	if (session)
		PushRemoval(netId, reason);
	// Any other garage still pointed at it is told too; the one delivering
	// nils its own target when this returns.
	ForgetEngineRawPointersTo(vehicle);
	Func<DestroyFn>(DestroyVehicleAndDriverAndPassengers)(vehicle);
}

void __cdecl DestroyMission(void *v)   { DestroyFor(v, VEHICLE_REMOVED_MISSION); }
void __cdecl DestroyCollected(void *v) { DestroyFor(v, VEHICLE_REMOVED_COLLECTED); }
void __cdecl DestroyCraig(void *v)     { DestroyFor(v, VEHICLE_REMOVED_EXPORTED); }
void __cdecl DestroyCrushed(void *v)   { DestroyFor(v, VEHICLE_REMOVED_CRUSHED); }

// ---- the safehouse --------------------------------------------------------

using StoreFn = void(__thiscall *)(void *, void *, int32_t);

bool InsideGarage(void *garage, void *vehicle) {
	const float x = Field<float>(vehicle, offs::POSITION);
	const float y = Field<float>(vehicle, offs::POSITION + 4);
	const float z = Field<float>(vehicle, offs::POSITION + 8);
	// The store's own strict compares, 0x004278E5..0x0042793B.
	return x > Field<float>(garage, offs::GARAGE_X1) && x < Field<float>(garage, offs::GARAGE_X2) &&
	       y > Field<float>(garage, offs::GARAGE_Y1) && y < Field<float>(garage, offs::GARAGE_Y2) &&
	       z > Field<float>(garage, offs::GARAGE_Z1) && z < Field<float>(garage, offs::GARAGE_Z2);
}

// StoreAndRemoveCarsForThisHideout stores and deletes every car in the box
// that is not MISSION_VEHICLE, and does nothing at all to one that is. So a
// car the holder stores is made this engine's own first (a copy is a mission
// car), and a car another machine holds reads MISSION for the length of the
// call and is left standing - until the holder's own store ends it.
void __fastcall StoreHideout(void *garage, void * /*edx*/, void *cars, int32_t max) {
	void   *borrowed[MAX_ROWS];
	uint8_t was[MAX_ROWS];
	size_t  count = 0;
	for (uint8_t i = 0; i < g_rowCount; ++i) {
		void *const v = CarAt(g_rows[i].poolHandle);
		if (!v || !InsideGarage(garage, v))
			continue;
		uint8_t &by = Field<uint8_t>(v, offs::VEH_CREATED_BY);
		if (g_rows[i].weMay) {
			if (by == VEHICLE_CREATED_BY_MISSION)
				HandCopyToEngine(v);
			// The store deletes it, and tells no garage.
			ForgetEngineRawPointersTo(v);
			PushRemoval(g_rows[i].netId, VEHICLE_REMOVED_STORED);
			if (!g_saidStore) {
				g_saidStore = true;
				Log("carremoval: the safehouse is storing session car %u; it is "
				    "this game's own car now", g_rows[i].netId);
			}
		} else if (by != VEHICLE_CREATED_BY_MISSION && count < MAX_ROWS) {
			was[count]        = by;
			borrowed[count++] = v;
			by                = static_cast<uint8_t>(VEHICLE_CREATED_BY_MISSION);
		}
	}
	Func<StoreFn>(CGarage__StoreAndRemoveCarsForThisHideout)(garage, cars, max);
	// Never deleted: the store leaves a mission car entirely alone.
	for (size_t i = 0; i < count; ++i)
		Field<uint8_t>(borrowed[i], offs::VEH_CREATED_BY) = was[i];
}

// ---- the crane ------------------------------------------------------------

using FindCarFn = void(__thiscall *)(void *, void *);

// FindCarInSectorList skips any car whose scan code is already the current
// one, and nothing else reads that code before the scan advances it again.
// So a car another machine holds is stamped seen before the walk, and the
// crane never picks it up, never lifts it and never pays for it.
void __fastcall FindCarSkippingOthers(void *crane, void * /*edx*/, void *list) {
	const uint16_t code = Global<uint16_t>(CWorld__ms_nCurrentScanCode);
	for (void *node = list ? *reinterpret_cast<void **>(list) : nullptr; node;
	     node       = *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(node) + 8)) {
		void *const v = *reinterpret_cast<void **>(node);
		uint16_t    netId   = INVALID_NETID;
		bool        session = false;
		if (v && !MayTake(v, netId, session)) {
			Field<uint16_t>(v, offs::SCAN_CODE) = code;
			if (!g_saidCrane) {
				g_saidCrane = true;
				Log("carremoval: kept the crane off a car another machine holds "
				    "(said once)");
			}
		}
	}
	Func<FindCarFn>(CCrane__FindCarInSectorList)(crane, list);
}

using RemoveFn = void(__cdecl *)(void *);

void __cdecl CraneRemoves(void *vehicle) {
	// The crane picked this car up this frame, so it is live unless something
	// is badly wrong - and then the deleting destructor the engine runs next
	// fails however this goes. Said, so a crash there has a line before it.
	const EntityState state = VehicleState(vehicle);
	if (state != EntityState::Live && !g_saidStaleCrane) {
		g_saidStaleCrane = true;
		Log("carremoval: the military crane is delivering a car that is %s (said once)",
		    EntityStateName(state));
	}
	uint16_t netId   = INVALID_NETID;
	bool     session = false;
	if (MayTake(vehicle, netId, session) && session)
		PushRemoval(netId, VEHICLE_REMOVED_CRANE);
	if (state == EntityState::Live)
		ForgetEngineRawPointersTo(vehicle);
	// Always: the deleting destructor is the next instruction.
	Func<RemoveFn>(CWorld__Remove)(vehicle);
}

// ---- the call sites -------------------------------------------------------

struct Site {
	uintptr_t   at;
	uintptr_t   engine;
	uintptr_t   ours;
	const char *what;
	bool        taken;
};

Site g_sites[] = {
    {GARAGE_DESTROY_MISSION_CALL, DestroyVehicleAndDriverAndPassengers,
     reinterpret_cast<uintptr_t>(&DestroyMission), "a mission garage's delivery", false},
    {GARAGE_DESTROY_COLLECTED_CALL, DestroyVehicleAndDriverAndPassengers,
     reinterpret_cast<uintptr_t>(&DestroyCollected), "the police and bank-van garage", false},
    {GARAGE_DESTROY_CRAIG_CALL, DestroyVehicleAndDriverAndPassengers,
     reinterpret_cast<uintptr_t>(&DestroyCraig), "Craig's garage", false},
    {GARAGE_DESTROY_CRUSHER_CALL, DestroyVehicleAndDriverAndPassengers,
     reinterpret_cast<uintptr_t>(&DestroyCrushed), "the crusher", false},
    {HIDEOUT_STORE_UPDATE_CALL, CGarage__StoreAndRemoveCarsForThisHideout,
     reinterpret_cast<uintptr_t>(&StoreHideout), "the safehouse closing", false},
    {HIDEOUT_STORE_CLOSE_CALL, CGarage__StoreAndRemoveCarsForThisHideout,
     reinterpret_cast<uintptr_t>(&StoreHideout), "the safehouses shut for a save", false},
    {CRANE_FIND_VEHICLES_CALL, CCrane__FindCarInSectorList,
     reinterpret_cast<uintptr_t>(&FindCarSkippingOthers), "the crane's search", false},
    {CRANE_FIND_OVERLAP_CALL, CCrane__FindCarInSectorList,
     reinterpret_cast<uintptr_t>(&FindCarSkippingOthers), "the crane's search, overlap list", false},
    {CRANE_MILITARY_REMOVE_CALL, CWorld__Remove,
     reinterpret_cast<uintptr_t>(&CraneRemoves), "the military crane's delivery", false},
};

bool RedirectCall(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

// ---- Craig ----------------------------------------------------------------

// The car whose VehicleCreatedBy reads RANDOM for one Update of Craig's
// garage, or null.
void *g_craigBorrowed = nullptr;

// ---- the bridge -----------------------------------------------------------

void NoteVehicleRemoversImpl(const VehicleRemover *rows, uint8_t count) {
	g_rowCount = count > MAX_ROWS ? static_cast<uint8_t>(MAX_ROWS) : count;
	for (uint8_t i = 0; i < g_rowCount; ++i)
		g_rows[i] = rows[i];
	// Once a frame, which is what the parked-car claim needs: the generator
	// has let go of its car by the time the claim is sent, so the name is
	// taken while the player is still on his way in.
	NoteCarBeingEntered();
}

uint8_t DrainVehicleRemovalsImpl(VehicleRemoval *out, uint8_t max) {
	uint8_t n = 0;
	for (; n < g_pendingCount && n < max; ++n)
		out[n] = g_pending[n];
	g_pendingCount = 0;
	return n;
}


} // namespace

bool EngineTookCarAway(uint16_t netId) {
	if (netId == INVALID_NETID)
		return false;
	for (uint16_t id : g_taken)
		if (id == netId)
			return true;
	return false;
}

void ReleaseInitialDoorLock(void *vehicle) {
	if (!vehicle)
		return;
	int32_t &lock = Field<int32_t>(vehicle, offs::VEH_DOOR_LOCK);
	if (lock == CARLOCK_LOCKED_INITIALLY)
		lock = CARLOCK_UNLOCKED;
}

void CarRemovalBeforeGarageUpdate(void *garage) {
	const uint8_t type  = Field<uint8_t>(garage, offs::GARAGE_TYPE);
	const uint8_t state = Field<uint8_t>(garage, offs::GARAGE_STATE);

	// The crusher's target, if it is somebody else's: taken back before the
	// arm that pays for it can run.
	if (type == GARAGE_CRUSHER)
		CarRemovalAfterGarageUpdate(garage, state);

	g_craigBorrowed = nullptr;
	if (!IsCraigsGarage(type) || state != GS_FULLYCLOSED)
		return;
	void *const car = Func<void *(__cdecl *)()>(FindPlayerVehicle)();
	const VehicleRemover *r = RowFor(car);
	if (!r || !r->weMay)
		return;
	uint8_t &by = Field<uint8_t>(car, offs::VEH_CREATED_BY);
	if (by != VEHICLE_CREATED_BY_MISSION)
		return;
	by              = static_cast<uint8_t>(VEHICLE_CREATED_BY_RANDOM);
	g_craigBorrowed = car;
}

void CarRemovalAfterGarageUpdate(void *garage, uint8_t stateBefore) {
	if (g_craigBorrowed) {
		Field<uint8_t>(g_craigBorrowed, offs::VEH_CREATED_BY) =
		    static_cast<uint8_t>(VEHICLE_CREATED_BY_MISSION);
		g_craigBorrowed = nullptr;
	}

	if (Field<uint8_t>(garage, offs::GARAGE_TYPE) != GARAGE_CRUSHER)
		return;
	void *&target = Field<void *>(garage, offs::GARAGE_TARGET);
	uint16_t netId   = INVALID_NETID;
	bool     session = false;
	if (!target || MayTake(target, netId, session))
		return;

	// Nulled the way the engine nulls it, and the reference it registered
	// pruned, so the car's own teardown later does not reach back into a
	// garage that has moved on to another car.
	void *const car = target;
	target          = nullptr;
	Func<void(__thiscall *)(void *)>(CEntity__PruneReferences)(car);
	// Picked this frame: the scan also moved the crusher to closing, and with
	// no target the closing arm would only open it again.
	if (stateBefore == GS_OPENED && Field<uint8_t>(garage, offs::GARAGE_STATE) == GS_CLOSING)
		Field<uint8_t>(garage, offs::GARAGE_STATE) = GS_OPENED;
	if (!g_saidCrusher) {
		g_saidCrusher = true;
		Log("carremoval: kept the crusher off car %u, which another machine "
		    "holds (said once)", netId);
	}
}

bool InstallCarRemovalHooks() {
	g_rowCount     = 0;
	g_pendingCount = 0;
	std::memset(g_taken, 0, sizeof g_taken);
	size_t taken = 0;
	for (Site &s : g_sites) {
		if (!s.taken)
			s.taken = RedirectCall(s.at, s.engine, s.ours);
		if (s.taken)
			++taken;
		else
			Log("carremoval: FAILED to take the call at 0x%08X (%s); that one "
			    "stays every machine's own", static_cast<unsigned>(s.at), s.what);
	}
	Log("carremoval: took %u of %u call sites for the crusher, the crane and the "
	    "garages", static_cast<unsigned>(taken),
	    static_cast<unsigned>(sizeof g_sites / sizeof g_sites[0]));
	return taken == sizeof g_sites / sizeof g_sites[0];
}

void RemoveCarRemovalHooks() {
	for (Site &s : g_sites)
		if (s.taken && RedirectCall(s.at, s.ours, s.engine))
			s.taken = false;
	g_rowCount     = 0;
	g_pendingCount = 0;
}

void AddCarRemovalToBridge(WorldBridge &bridge) {
	bridge.NoteVehicleRemovers  = &NoteVehicleRemoversImpl;
	bridge.DrainVehicleRemovals = &DrainVehicleRemovalsImpl;
	bridge.TakeOverParkedCar    = &TakeOverParkedCar;
}

} // namespace coopiii::game
