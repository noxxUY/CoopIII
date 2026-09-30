// Somebody else's crowd, built only where it is any use to us.
//
// Every replica of another machine's pedestrians and traffic is a real CPed or
// CVehicle here, and the engine counts it exactly like one of its own: a ped in
// ms_nTotalPeds against MaxNumberOfPedsInUse, a traffic car in NumRandomCars
// against the density gate (docs/population.md §1.3.1-1.3.2). That is what
// makes two players in one street share it rather than double it. It is also
// what emptied the street of a player on the other side of the city: his
// engine counted a crowd that was standing next to somebody else, stopped
// generating, and left him walking through a ghost town, with no police car
// ever coming for him because GenerateOneRandomCar's police arm sits behind
// the same gate.
//
// So a replica is built only within REPLICA_BUILD_M of the local player and
// taken down past REPLICA_DROP_M, and its row is held meanwhile. The gap
// between the two is so a car driving along the edge is not built and
// destroyed on alternate frames.
//
// And a wanted player's machine gets the room its police need even when the
// street around it is full of somebody else's traffic (PoliceCarDue and
// TrafficRoomForPolice, below).
//
// Nothing here reads a game global or calls a game function.
#pragma once

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii {
struct WorldBridge;
}

namespace coopiii::game {

// The engine half, game/crowdrange.cpp. Takes GenerateRandomCars' two calls
// into GenerateOneRandomCar; not fatal, the log says what is lost. Remove
// also gives the adrenaline pill its clock back.
bool InstallCrowdRange();
void RemoveCrowdRange();
void AddCrowdRangeToBridge(WorldBridge &bridge);

// Points the E8 call at `site` at `to`, only while it still calls `from`.
// False, having written nothing, otherwise. Shared with game/pedspeech.cpp.
bool RedirectCallSite(uintptr_t site, uintptr_t from, uintptr_t to);

// What CPopulation::AddToPopulation's cops-on-foot gate reads for the length of
// one call (docs/wanted.md 6.1): our own cops in ms_nNumCop, plus somebody
// else's built here, which the engine counted as civilians because a replica
// is a CCivilianPed. So several wanted machines in one street make one
// street's worth of cops between them, not one each. Never less than our own,
// whatever the replica count says.
inline int32_t CopsTheGateSees(int32_t ownCops, uint32_t copReplicas) {
	const uint32_t cap   = 0x3FFFFFFFu;
	const uint32_t extra = copReplicas < cap ? copReplicas : cap;
	if (ownCops < 0)
		ownCops = 0;
	const int64_t sum = static_cast<int64_t>(ownCops) + extra;
	return sum > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int32_t>(sum);
}

constexpr float REPLICA_BUILD_M = 160.0f;
constexpr float REPLICA_DROP_M  = 200.0f;

// Is a replica `dx`, `dy` metres from the local player one to hold built?
// Flat distance: Liberty City's heights are a few storeys, and a bridge deck
// over a street is still that street's traffic.
inline bool ReplicaInRange(bool wasInRange, float dx, float dy) {
	const float d2    = dx * dx + dy * dy;
	const float limit = wasInRange ? REPLICA_DROP_M : REPLICA_BUILD_M;
	return d2 <= limit * limit;
}

// GenerateOneRandomCar's police decision, as the engine takes it after both of
// its gates (game/crowdaddr.h has the instructions). `lawCars` is whatever the
// caller wants NumLawEnforcerCars to read.
inline bool PoliceCarDue(int32_t level, int32_t lawCars, uint8_t maxLawCars,
                         uint8_t currentCops, uint8_t maxCops, uint32_t nowMs,
                         uint32_t lastLawCarMs) {
	if (level <= 1)
		return false;
	if (lawCars >= static_cast<int32_t>(maxLawCars))
		return false;
	if (currentCops >= maxCops)
		return false;
	if (level > 3)
		return true;
	if (level > 2 && nowMs > lastLawCarMs + 5000u)
		return true;
	return nowMs > lastLawCarMs + 8000u;
}

// What somebody else's traffic, built here, puts into the counters the
// generator reads.
struct ReplicaTraffic {
	int32_t random = 0;   // RANDOM_VEHICLE copies: each is one in NumRandomCars
	int32_t law    = 0;   // of which police: each is one in NumLawEnforcerCars too
};

// How far to lower NumRandomCars and NumLawEnforcerCars, and raise
// MaxNumberOfCarsInUse, for the length of one GenerateOneRandomCar call.
struct TrafficRoom {
	int32_t random = 0;
	int32_t law    = 0;
	int32_t cap    = 0;
	bool Any() const { return random != 0 || law != 0 || cap != 0; }
};

// Room is made only when the engine would make a police car with the replicas
// left out of the count - so what the extra room buys is police for the
// player who is wanted, and not a second street's worth of traffic around him.
// Never more than the counters hold, so nothing reads negative.
inline TrafficRoom TrafficRoomForPolice(const ReplicaTraffic &replicas,
                                        int32_t numRandom, int32_t numLaw,
                                        int32_t level, uint8_t maxLawCars,
                                        uint8_t currentCops, uint8_t maxCops,
                                        uint32_t nowMs, uint32_t lastLawCarMs) {
	TrafficRoom room;
	if (replicas.random <= 0 && replicas.law <= 0)
		return room;
	const int32_t law = replicas.law < numLaw ? (replicas.law > 0 ? replicas.law : 0)
	                                          : (numLaw > 0 ? numLaw : 0);
	if (!PoliceCarDue(level, numLaw - law, maxLawCars, currentCops, maxCops, nowMs,
	                  lastLawCarMs))
		return room;
	room.random = replicas.random < numRandom ? (replicas.random > 0 ? replicas.random : 0)
	                                          : (numRandom > 0 ? numRandom : 0);
	room.law    = law;
	// The six-term sum against MaxNumberOfCarsInUse counts a police car twice,
	// once as random and once as law (game/carlife.h, TrafficSumShare).
	room.cap    = room.random + room.law;
	return room;
}

// ---- the spot another machine's crowd already stands on ----------------------
//
// Both generators ask CWorld::FindObjectsKindaColliding whether the spot they
// picked is free (game/crowdaddr.h), and it only sees what is built here. A
// car or pedestrian another machine hosts is not built here while its model
// is still streaming in, nor past REPLICA_BUILD_M - and the traffic
// generator makes cars out to 130 m times GenerationDistMultiplier, which is
// 70/FOV times the aspect over 4:3, so past 160 m on a widescreen. So our
// engine made its own car on a spot somebody else's already held, and when
// the replica was built it was built on top of it: two of one car, locked
// into each other. The same for a pedestrian while his model streams.
//
// So the test also counts the other machines' crowd that is not built here,
// where its host last put it, as a sphere of its own: half a car's length, a
// pedestrian's shoulders. It refuses only a spot one of them is standing on,
// never an area, so nothing is starved: the generator picks another spot, as
// it does whenever the street is busy.
constexpr float UNBUILT_CAR_SPAN_M = 3.0f;
constexpr float UNBUILT_PED_SPAN_M = 1.0f;
// How far from our player an unbuilt row can be and still be somewhere the
// generators might pick: past the traffic generator's widest ring
// (130 m x 1.5 x the widest reasonable aspect), and the pedestrian one's.
constexpr float UNBUILT_CAR_NOTE_M = 300.0f;
constexpr float UNBUILT_PED_NOTE_M = 150.0f;
constexpr uint32_t MAX_UNBUILT_NOTED = 64;

// Is the spot `at`, `radius` round, one an unbuilt car (when `vehicles`) or
// pedestrian (when `peds`) of somebody else's stands on? Flat distance, as
// the generators ask it.
inline bool SpotTakenByUnbuilt(const Vec3 &at, float radius, bool vehicles, bool peds,
                               const Vec3 *cars, uint32_t carCount, const Vec3 *pedsAt,
                               uint32_t pedCount) {
	if (!(radius >= 0.0f))
		return false;
	auto touches = [&at](const Vec3 &p, float reach) {
		const float dx = p.x - at.x, dy = p.y - at.y;
		return dx * dx + dy * dy < reach * reach;
	};
	if (vehicles)
		for (uint32_t i = 0; i < carCount; ++i)
			if (touches(cars[i], radius + UNBUILT_CAR_SPAN_M))
				return true;
	if (peds)
		for (uint32_t i = 0; i < pedCount; ++i)
			if (touches(pedsAt[i], radius + UNBUILT_PED_SPAN_M))
				return true;
	return false;
}

// Should a row not built here be noted for SpotTakenByUnbuilt, `dx`, `dy`
// from our centre?
inline bool UnbuiltWorthNoting(bool car, float dx, float dy) {
	const float limit = car ? UNBUILT_CAR_NOTE_M : UNBUILT_PED_NOTE_M;
	return dx * dx + dy * dy <= limit * limit;
}

} // namespace coopiii::game
