// The engine half of game/carlife.h: which CVehicles are copies, what they
// cost the traffic budget, keeping them out of the save, and handing one back
// to the engine.
#include "carlife.h"

#include "vehicle.h"

#include "../hook/hook.h"
#include "../log.h"

namespace coopiii::game {

namespace {

int32_t CarRef(void *vehicle) {
	return Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(vehicle);
}

void *CarAt(int32_t ref) {
	if (ref < 0)
		return nullptr;
	return Func<void *(__cdecl *)(int32_t)>(CPools__GetVehicle)(ref);
}

// Client::MAX_REMOTE_VEHICLES, the most copies there can be at once.
constexpr size_t MAX_COPIES = 64;

CopyList<MAX_COPIES> g_copies;

// MaxNumberOfCarsInUse as the engine last set it, and what we last wrote over
// it. -1 until the first frame has read it.
int32_t g_engineCap  = -1;
int32_t g_writtenCap = -1;
bool    g_saidShare  = false;

Detour g_saveVehiclePool;
using SaveVehiclePoolFn = void(__cdecl *)(uint8_t *, uint32_t *);

// CPools::SaveVehiclePool, with every copy reading PERMANENT_VEHICLE for the
// length of the call. addresses.h has the function; carlife.h has why this
// value and why nothing else notices.
void __cdecl HookedSaveVehiclePool(uint8_t *buf, uint32_t *size) {
	void  *borrowed[MAX_COPIES];
	size_t count = 0;
	size_t live  = 0;
	g_copies.ForEachLive(&CarAt, [&](void *v) {
		++live;
		uint8_t      &by     = Field<uint8_t>(v, offs::VEH_CREATED_BY);
		const uint8_t during = CreatedByDuringSave(true, by);
		if (during == by || count >= MAX_COPIES)
			return;
		borrowed[count++] = v;
		by                = during;
	});

	g_saveVehiclePool.Original<SaveVehiclePoolFn>()(buf, size);

	for (size_t i = 0; i < count; ++i)
		Field<uint8_t>(borrowed[i], offs::VEH_CREATED_BY) =
		    static_cast<uint8_t>(VEHICLE_CREATED_BY_MISSION);

	Log("save: kept %u of %u session car(s) out of the save", static_cast<unsigned>(count),
	    static_cast<unsigned>(live));
}

} // namespace

void NoteSessionCopy(int32_t handle) {
	if (g_copies.Add(handle, &CarAt))
		return;
	static bool said = false;
	if (!said) {
		said = true;
		Log("carlife: the copy list is full at %u; a copy that doesn't fit "
		    "costs traffic and would go into a save",
		    static_cast<unsigned>(MAX_COPIES));
	}
}

void ForgetSessionCopy(int32_t handle) { g_copies.Remove(handle); }

void UpdateTrafficAllowance() {
	int32_t &cap = Global<int32_t>(CCarCtrl__MaxNumberOfCarsInUse);

	// Read once, and again if anything but us has written it since. Only
	// CIniFile::LoadIniFile does, at startup, but a value we did not write is
	// the engine's by definition.
	if (g_engineCap < 0 || (g_writtenCap >= 0 && cap != g_writtenCap)) {
		g_engineCap = cap;
		Log("carlife: the engine's traffic cap is %d cars; session copies go "
		    "on top of it", g_engineCap);
	}

	int32_t share = 0;
	g_copies.ForEachLive(&CarAt, [&](void *v) {
		share += TrafficSumShare(
		    Field<uint8_t>(v, offs::VEH_CREATED_BY),
		    (Field<uint8_t>(v, offs::VEH_FLAGS_A) & offs::VEH_IS_LAW_ENFORCER) != 0);
	});

	const int32_t want = TrafficAllowance(g_engineCap, share);
	if (cap != want)
		cap = want;
	g_writtenCap = want;

	if (share > 0 && !g_saidShare) {
		g_saidShare = true;
		Log("carlife: %d session car(s) here; the traffic cap reads %d so they "
		    "don't count against this machine's traffic (said once)",
		    share, want);
	}
}

void RestoreTrafficCap() {
	if (g_engineCap >= 0)
		Global<int32_t>(CCarCtrl__MaxNumberOfCarsInUse) = g_engineCap;
	g_engineCap  = -1;
	g_writtenCap = -1;
}

void MakeCopyAMissionCar(void *vehicle) {
	using CountFn = void(__cdecl *)(void *, uint8_t);
	uint8_t &by = Field<uint8_t>(vehicle, offs::VEH_CREATED_BY);
	if (by == VEHICLE_CREATED_BY_MISSION)
		return;
	// The counters follow the byte, the way CTheScripts' STORE_CAR does it
	// but through UpdateCarCount itself, so a police car's second counter
	// comes out with it.
	Func<CountFn>(CCarCtrl__UpdateCarCount)(vehicle, 1);
	by = static_cast<uint8_t>(VEHICLE_CREATED_BY_MISSION);
	Func<CountFn>(CCarCtrl__UpdateCarCount)(vehicle, 0);
}

void HandCopyToEngine(void *vehicle) {
	using CountFn = void(__cdecl *)(void *, uint8_t);
	if (!vehicle)
		return;
	ForgetSessionCopy(CarRef(vehicle));

	// re3's CTheScripts::CleanUpThisVehicle is this, for a mission car a
	// script has finished with: unlock, RANDOM_VEHICLE, counters moved. The
	// counters go through UpdateCarCount so they move exactly as far as the
	// destructor will later move them back.
	uint8_t &by = Field<uint8_t>(vehicle, offs::VEH_CREATED_BY);
	if (by != VEHICLE_CREATED_BY_RANDOM) {
		Func<CountFn>(CCarCtrl__UpdateCarCount)(vehicle, 1);
		by = static_cast<uint8_t>(VEHICLE_CREATED_BY_RANDOM);
		Func<CountFn>(CCarCtrl__UpdateCarCount)(vehicle, 0);
	}
	uint8_t &flags = Field<uint8_t>(vehicle, offs::VEH_FLAGS_A);
	flags          = static_cast<uint8_t>(flags & ~offs::VEH_IS_LOCKED);
	SetVehicleObserved(vehicle, false);
}

bool InstallSaveGuard() {
	g_copies.Clear();
	if (g_saveVehiclePool.Install("CPools::SaveVehiclePool",
	                              reinterpret_cast<void *>(CPools__SaveVehiclePool),
	                              reinterpret_cast<void *>(&HookedSaveVehiclePool))) {
		Log("carlife: hooked CPools::SaveVehiclePool at 0x%08X",
		    static_cast<unsigned>(CPools__SaveVehiclePool));
		return true;
	}
	Log("carlife: FAILED to hook CPools::SaveVehiclePool at 0x%08X - saving "
	    "during a session WILL write other players' parked cars into your "
	    "single-player save. Don't save while connected.",
	    static_cast<unsigned>(CPools__SaveVehiclePool));
	return false;
}

void RemoveSaveGuard() {
	g_saveVehiclePool.Remove();
	g_copies.Clear();
	RestoreTrafficCap();
}

} // namespace coopiii::game
