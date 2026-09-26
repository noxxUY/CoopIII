// The engine half of game/crowdrange.h: where the population is centred, the
// traffic generator's police arm given room for a wanted player, and the
// adrenaline pill kept off the clock in a session. game/crowdaddr.h has every
// address with the instruction that proves it.
#include "crowdrange.h"

#include "crowdaddr.h"
#include "leadcheck.h"
#include "../client.h"
#include "../log.h"

#include <windows.h>

#include <cstring>

namespace coopiii::game {

namespace {

using PedFn     = void *(__cdecl *)();
using CentreFn  = const float *(__cdecl *)(int32_t);
using GetCarFn  = void *(__cdecl *)(int32_t);
using VoidFn    = void(__cdecl *)();

// Points the `call` at `site` at `to`, only while it still calls `from`.
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

bool WriteCode(uintptr_t site, const uint8_t *bytes, size_t len) {
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), len, PAGE_EXECUTE_READWRITE, &old))
		return false;
	std::memcpy(reinterpret_cast<void *>(site), bytes, len);
	VirtualProtect(reinterpret_cast<void *>(site), len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), len);
	return true;
}

void *PlayerPed() { return Func<PedFn>(FindPlayerPed)(); }

// ---- the centre ------------------------------------------------------------

bool SampleCrowdCentre(Vec3 &out) {
	if (!PlayerPed())
		return false;
	const float *p = Func<CentreFn>(FindPlayerCentreOfWorld)(
	    static_cast<int32_t>(Global<uint8_t>(CWorld__PlayerInFocus)));
	if (!p)
		return false;
	out = Vec3{p[0], p[1], p[2]};
	return true;
}

// ---- room for the police -----------------------------------------------------

constexpr size_t MAX_HANDLES = 128;   // Client::MAX_REMOTE_CARS
int32_t  g_handles[MAX_HANDLES];
uint32_t g_handleCount = 0;
bool     g_callsTaken[2] = {false, false};
uint32_t g_roomMade = 0;

void NoteBuiltCarReplicas(const int32_t *handles, uint32_t count) {
	g_handleCount = count < MAX_HANDLES ? count : static_cast<uint32_t>(MAX_HANDLES);
	std::memcpy(g_handles, handles, g_handleCount * sizeof(int32_t));
}

// Resolved at the call, not when noted: a copy the engine took since then has
// already been taken off the counters by its destructor.
ReplicaTraffic CountReplicaTraffic() {
	ReplicaTraffic t;
	for (uint32_t i = 0; i < g_handleCount; ++i) {
		void *const v = Func<GetCarFn>(CPools__GetVehicle)(g_handles[i]);
		if (!v || Field<uint8_t>(v, offs::VEH_CREATED_BY) != VEHICLE_CREATED_BY_RANDOM)
			continue;
		++t.random;
		if ((Field<uint8_t>(v, offs::VEH_FLAGS_A) & offs::VEH_IS_LAW_ENFORCER) != 0)
			++t.law;
	}
	return t;
}

// GenerateOneRandomCar, as GenerateRandomCars calls it. For a wanted player
// whose police the engine would make if somebody else's traffic were not in
// the count, the count is lowered for the one call and put back by exactly as
// much after it - whatever the call itself made or took away is the engine's
// own and stays.
void __cdecl GenerateOneRandomCarWithRoom() {
	void *const ped = PlayerPed();
	TrafficRoom room;
	if (ped != nullptr && g_handleCount != 0) {
		void *const wanted = Field<void *>(ped, offs::PLAYER_PED_WANTED);
		if (wanted != nullptr)
			room = TrafficRoomForPolice(
			    CountReplicaTraffic(), Global<int32_t>(CCarCtrl__NumRandomCars),
			    Global<int32_t>(CCarCtrl__NumLawEnforcerCars),
			    Field<int32_t>(wanted, offs::WANTED_LEVEL),
			    Field<uint8_t>(wanted, offs::WANTED_MAX_LAW_CARS),
			    Field<uint8_t>(wanted, offs::WANTED_CURRENT_COPS),
			    Field<uint8_t>(wanted, offs::WANTED_MAX_COPS),
			    Global<uint32_t>(CTimer__m_snTimeInMilliseconds),
			    Global<uint32_t>(CCarCtrl__LastTimeLawEnforcerCreated));
	}
	if (!room.Any()) {
		Func<VoidFn>(CCarCtrl__GenerateOneRandomCar)();
		return;
	}

	Global<int32_t>(CCarCtrl__NumRandomCars) -= room.random;
	Global<int32_t>(CCarCtrl__NumLawEnforcerCars) -= room.law;
	Global<int32_t>(CCarCtrl__MaxNumberOfCarsInUse) += room.cap;
	Func<VoidFn>(CCarCtrl__GenerateOneRandomCar)();
	Global<int32_t>(CCarCtrl__NumRandomCars) += room.random;
	Global<int32_t>(CCarCtrl__NumLawEnforcerCars) += room.law;
	Global<int32_t>(CCarCtrl__MaxNumberOfCarsInUse) -= room.cap;

	if (g_roomMade++ == 0)
		Log("crowd: we are wanted and the street is full of other machines' traffic; "
		    "left %d of their car(s) (%d police) out of the generator's count so our "
		    "own police can come (said once)",
		    room.random, room.law);
}

// ---- the adrenaline pill -----------------------------------------------------

constexpr uint8_t NOPS[sizeof ADRENALINE_SLOWDOWN_BYTES] = {0x90, 0x90, 0x90, 0x90, 0x90,
                                                            0x90, 0x90, 0x90, 0x90, 0x90};
bool g_adrenalineHeld   = false;
bool g_adrenalineRefused = false;

void HoldAdrenalineClock(bool inSession) {
	if (inSession == g_adrenalineHeld || g_adrenalineRefused)
		return;
	const uintptr_t site = ADRENALINE_SLOWDOWN_STORE;
	if (inSession) {
		if (std::memcmp(Ptr<uint8_t>(site), ADRENALINE_SLOWDOWN_BYTES,
		                sizeof ADRENALINE_SLOWDOWN_BYTES) != 0 ||
		    !WriteCode(site, NOPS, sizeof NOPS)) {
			g_adrenalineRefused = true;
			Log("crowd: the adrenaline pill's slow-down at 0x%08X is not the bytes we "
			    "know; left alone, so a pill still slows this machine's world to a "
			    "third for everybody", static_cast<unsigned>(site));
			return;
		}
		g_adrenalineHeld = true;
		// A pill taken before the session began has the clock at a third
		// already, and nothing would put it back until it ran out.
		void *const ped = PlayerPed();
		float &scale    = Global<float>(CTimer__ms_fTimeScale);
		if (ped != nullptr && Field<uint8_t>(ped, offs::PLAYER_ADRENALINE) != 0 &&
		    scale < 0.34f && scale > 0.33f)
			scale = 1.0f;
		Log("crowd: in a session the adrenaline pill no longer slows the clock; the "
		    "rest of what it does stays");
		return;
	}
	if (WriteCode(site, ADRENALINE_SLOWDOWN_BYTES, sizeof ADRENALINE_SLOWDOWN_BYTES))
		g_adrenalineHeld = false;
}

} // namespace

bool InstallCrowdRange() {
	size_t taken = 0;
	for (size_t i = 0; i < 2; ++i) {
		if (!g_callsTaken[i])
			g_callsTaken[i] =
			    RedirectCall(GENERATE_ONE_RANDOM_CAR_CALLS[i], CCarCtrl__GenerateOneRandomCar,
			                 reinterpret_cast<uintptr_t>(&GenerateOneRandomCarWithRoom));
		taken += g_callsTaken[i] ? 1 : 0;
	}
	if (taken == 2)
		Log("crowd: took the traffic generator's two calls, so a wanted player's police "
		    "are not crowded out by other machines' traffic");
	else
		Log("crowd: FAILED to take %u of the traffic generator's two calls; a wanted "
		    "player among other machines' traffic may see few police cars",
		    static_cast<unsigned>(2 - taken));
	return taken == 2;
}

void RemoveCrowdRange() {
	for (size_t i = 0; i < 2; ++i)
		if (g_callsTaken[i] &&
		    RedirectCall(GENERATE_ONE_RANDOM_CAR_CALLS[i],
		                 reinterpret_cast<uintptr_t>(&GenerateOneRandomCarWithRoom),
		                 CCarCtrl__GenerateOneRandomCar))
			g_callsTaken[i] = false;
	HoldAdrenalineClock(false);
	g_handleCount = 0;
}

void AddCrowdRangeToBridge(WorldBridge &bridge) {
	bridge.SampleCrowdCentre    = &SampleCrowdCentre;
	bridge.NoteBuiltCarReplicas = &NoteBuiltCarReplicas;
	bridge.HoldAdrenalineClock  = &HoldAdrenalineClock;
}

} // namespace coopiii::game
