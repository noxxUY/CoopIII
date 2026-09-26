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

} // namespace coopiii::game
