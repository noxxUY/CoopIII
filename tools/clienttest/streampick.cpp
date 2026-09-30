// Which hosted peds and cars get a row in a batch: client/src/game/streampick.h.
//
// The samplers themselves read the engine and can't run here. What can is the
// rule they hand their candidates to, run over many ticks the way the 10 Hz
// stream runs it, against the one thing the old rule got wrong: a host with
// more than a batch's worth left the rest without a row for good.

#include "game/streampick.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_streamFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_streamFailures;
}

float Sq(float m) { return m * m; }

// A host's worth of candidates, ticked the way SampleHostedCars ticks them.
struct Crowd {
	struct Entity {
		StreamRow row;
		uint16_t  weight   = 1;
		uint32_t  says     = 0;
		uint8_t   deadline = 0;
		float     dist2    = 0.0f;

		uint32_t sent     = 0;
		int32_t  lastTick = -1;
		uint32_t longest  = 0;   // most ticks between two of its rows
		uint8_t  waited   = 0;   // what PickStreamRows said last time
	};
	std::vector<Entity> e;
	int32_t             tick = 0;

	uint32_t Add(uint16_t weight, float dist2 = 0.0f) {
		Entity x;
		x.weight = weight;
		x.dist2  = dist2;
		e.push_back(x);
		return static_cast<uint32_t>(e.size() - 1);
	}

	// One batch; returns the indices picked, in order.
	std::vector<uint32_t> Tick(uint32_t slots) {
		std::vector<StreamCandidate> c(e.size());
		for (uint32_t i = 0; i < e.size(); ++i) {
			c[i].row      = &e[i].row;
			c[i].weight   = e[i].weight;
			c[i].says     = e[i].says;
			c[i].deadline = e[i].deadline;
			c[i].dist2    = e[i].dist2;
			c[i].index    = i;
		}
		const uint32_t n = PickStreamRows(c.data(), static_cast<uint32_t>(c.size()), slots);
		std::vector<uint32_t> picked;
		for (uint32_t i = 0; i < n; ++i) {
			Entity &x = e[c[i].index];
			if (x.lastTick >= 0 && static_cast<uint32_t>(tick - x.lastTick) > x.longest)
				x.longest = static_cast<uint32_t>(tick - x.lastTick);
			x.lastTick = tick;
			x.waited   = c[i].waited;
			++x.sent;
			picked.push_back(c[i].index);
		}
		++tick;
		return picked;
	}

	void Run(uint32_t ticks, uint32_t slots) {
		for (uint32_t t = 0; t < ticks; ++t)
			Tick(slots);
	}

	// Past the first few batches, where everything is fresh and goes first.
	void Forget() {
		for (Entity &x : e) {
			x.sent    = 0;
			x.longest = 0;
		}
	}

	bool EveryoneWithin(uint32_t ticks) const {
		for (const Entity &x : e)
			if (x.sent == 0 || x.longest > ticks)
				return false;
		return true;
	}
};

bool Has(const std::vector<uint32_t> &v, uint32_t i) {
	for (uint32_t x : v)
		if (x == i)
			return true;
	return false;
}

void TestTheWeightFallsOffWithDistance() {
	std::printf("a row's weight, from the distance to the nearest other player\n");
	Check(StreamWeight(true, 0.0f) == STREAM_WEIGHT_MAX, "standing on it: the most");
	Check(StreamWeight(true, Sq(40.0f)) == 8, "40 m: still 8");
	Check(StreamWeight(true, Sq(41.0f)) == 7, "and just past it, 7");
	Check(StreamWeight(true, Sq(80.0f)) == 4, "twice as far, half as much");
	Check(StreamWeight(true, Sq(160.0f)) == 2, "160 m: 2");
	Check(StreamWeight(true, Sq(320.0f)) == 1, "320 m: 1");
	Check(StreamWeight(true, Sq(3000.0f)) == 1, "never below 1, so nothing is left out for good");
	Check(StreamWeight(false, 0.0f) == 1, "nobody else placed: a plain rotation");
	Check(StreamWeight(true, std::numeric_limits<float>::quiet_NaN()) == 1,
	      "a NaN distance is the floor, not the top");
	uint16_t last = STREAM_WEIGHT_MAX;
	bool     monotone = true;
	for (float d = 0.0f; d < 1000.0f; d += 7.0f) {
		const uint16_t w = StreamWeight(true, Sq(d));
		if (w > last || w < 1 || w > STREAM_WEIGHT_MAX)
			monotone = false;
		last = w;
	}
	Check(monotone, "and it only ever goes down, between 1 and the most");
}

void TestTheNearestViewer() {
	std::printf("the nearest other player\n");
	float d2 = -1.0f;
	Check(!NearestViewerDist2(Vec3{0, 0, 0}, nullptr, 0, d2) && d2 == -1.0f,
	      "nobody: no answer, and the distance is left alone");
	const Vec3 viewers[] = {{100, 0, 0}, {0, 30, 0}, {-50, -50, 0}};
	Check(NearestViewerDist2(Vec3{0, 0, 0}, viewers, 3, d2) && d2 == Sq(30.0f),
	      "the nearest of three, whichever slot it is in");
	Check(NearestViewerDist2(Vec3{0, 0, 40}, viewers + 1, 1, d2) && d2 == Sq(30.0f) + Sq(40.0f),
	      "in three dimensions");
}

void TestTheDeadlinesFitWhatTheReceiverHolds() {
	std::printf("the rows that cannot wait their turn\n");
	Check(STREAM_HONK_DEADLINE == 1, "a honking car goes out every batch");
	Check(STREAM_HONK_DEADLINE * STREAM_TICK_MS * 2 <= HORN_FRESH_MS,
	      "which leaves a lost batch inside the receiver's HORN_FRESH_MS");
	Check(STREAM_FIRE_DEADLINE == 5, "a burning ped at least every fifth");
	Check(STREAM_FIRE_DEADLINE * STREAM_TICK_MS * 2 <= REMOTE_FIRE_MS,
	      "which leaves a lost batch inside REMOTE_FIRE_MS");
	Check(StreamDeadlineTicks(0) == 1 && StreamDeadlineTicks(60000) == 254,
	      "never 0, which would mean no deadline, and never past the counter");
}

void TestAnNpcRoundGoesToWhoeverCanSeeIt() {
	std::printf("who a pedestrian's round is worth sending to\n");
	const Vec3 near[] = {{0, 100, 0}, {5000, 0, 0}};
	Check(NpcShotWorthSending(Vec3{0, 0, 0}, near, 2), "somebody 100 m away sees it");
	const Vec3 far[] = {{0, 151, 0}, {5000, 0, 0}};
	Check(!NpcShotWorthSending(Vec3{0, 0, 0}, far, 2), "nobody within 150 m, nobody sees it");
	Check(NpcShotWorthSending(Vec3{0, 0, 0}, nullptr, 0),
	      "and with nobody placed yet it goes anyway");
}

void TestWhatTheObserverHolds() {
	std::printf("what a row says that the observer holds until the next\n");
	const uint32_t onFoot = PedRowSays(INVALID_NETID, 0, 0);
	Check(PedRowSays(12, 0, 0) != onFoot, "getting into a car is a change");
	Check(PedRowSays(12, 1, 0) != PedRowSays(12, 0, 0), "so is changing seat");
	Check(PedRowSays(12, 0, 0) != PedRowSays(13, 0, 0), "and changing car");
	Check(PedRowSays(INVALID_NETID, 0, AMBIENT_PED_ON_FIRE) != onFoot, "and catching fire");
	const uint32_t car = CarRowSays(1000, false, false);
	Check(CarRowSays(999, false, false) != car, "a point of health is a change");
	Check(CarRowSays(1000, true, false) != car, "so is a siren");
	Check(CarRowSays(1000, false, true) != car, "and a horn");
	Check(CarRowSays(1000, true, false) != CarRowSays(1000, false, true),
	      "and the siren and the horn are not the same bit");
}

void TestAFewRowsAllGoEveryTick() {
	std::printf("fewer than a batch\n");
	Crowd c;
	for (int i = 0; i < 5; ++i)
		c.Add(static_cast<uint16_t>(1 + i));
	c.Run(50, MAX_CAR_STATES);
	bool all = true;
	for (const auto &x : c.e)
		if (x.sent != 50)
			all = false;
	Check(all, "every one goes out every tick, as it always did");
}

void TestNothingIsLeftFrozen() {
	std::printf("more than a batch - the bug\n");

	// Twelve cars, every one of them on top of the one player who can see
	// them, and eight rows. Nearest-first with no memory sent the same eight
	// every tick for as long as the cars were hosted.
	Crowd cars;
	for (int i = 0; i < 12; ++i)
		cars.Add(STREAM_WEIGHT_MAX, Sq(static_cast<float>(i)));
	cars.Run(200, MAX_CAR_STATES);
	Check(cars.EveryoneWithin(2), "twelve cars in eight rows: every one at least every 2nd batch");
	uint32_t total = 0;
	for (const auto &x : cars.e)
		total += x.sent;
	Check(total == 200u * MAX_CAR_STATES, "and every batch is full, so it costs what it did");

	// 25 peds, the engine's own crowd, in twelve rows, at every distance.
	Crowd peds;
	for (int i = 0; i < 25; ++i)
		peds.Add(StreamWeight(true, Sq(10.0f + 15.0f * i)), Sq(10.0f + 15.0f * i));
	peds.Run(300, MAX_PED_STATES);
	Check(peds.EveryoneWithin(20), "25 peds in twelve rows: nobody waits two seconds");
	Check(peds.e.front().sent > peds.e.back().sent,
	      "and the one beside a player goes out more often than the one 370 m away");
	Check(peds.e.front().longest <= 2, "that one waits no more than one batch");
}

void TestTheNearRowsGoMostOften() {
	std::printf("near rows, far rows\n");
	Crowd c;
	for (int i = 0; i < 4; ++i)
		c.Add(8, Sq(10.0f));
	for (int i = 0; i < 8; ++i)
		c.Add(1, Sq(500.0f));
	c.Run(100, MAX_CAR_STATES);
	bool nearEveryTick = true;
	for (int i = 0; i < 4; ++i)
		if (c.e[i].sent != 100)
			nearEveryTick = false;
	Check(nearEveryTick, "four cars beside a player go out in every batch");
	bool farEveryOther = true;
	for (int i = 4; i < 12; ++i)
		if (c.e[i].sent < 49 || c.e[i].longest > 2)
			farEveryOther = false;
	Check(farEveryOther, "and eight far ones share the other four rows, every second batch");
}

void TestAFreshRowGoesFirst() {
	std::printf("a row that has never gone out\n");
	Crowd c;
	for (int i = 0; i < MAX_CAR_STATES; ++i)
		c.Add(8);
	c.Run(20, MAX_CAR_STATES);
	const uint32_t late = c.Add(1, Sq(1000.0f));
	const auto     batch = c.Tick(MAX_CAR_STATES);
	Check(Has(batch, late), "goes out in the first batch after it is named");
	Check(c.e[late].waited == 0, "and says it waited nothing, since it had no row before");
	Check(c.e[late].row.sinceSent == 0 && c.e[late].row.credit == 0,
	      "and is marked sent like any other");
}

void TestAChangeGoesOutNextTick() {
	std::printf("a row whose seat, siren or health just changed\n");
	Crowd c;
	for (int i = 0; i < 20; ++i)
		c.Add(8, Sq(5.0f));
	const uint32_t driver = c.Add(1, Sq(900.0f));
	c.Run(50, MAX_PED_STATES);

	// Wait for it to go out, then change it on the tick right after.
	for (int i = 0; i < 100 && !Has(c.Tick(MAX_PED_STATES), driver); ++i) {
	}
	c.e[driver].says = PedRowSays(INVALID_NETID, 0, 0) ^ 1u;
	Check(Has(c.Tick(MAX_PED_STATES), driver),
	      "goes out on the very next batch, however low its credit");
	Check(!Has(c.Tick(MAX_PED_STATES), driver),
	      "and once it has, it takes its turn again");
}

void TestADeadlineIsKept() {
	std::printf("a honk and a fire\n");
	Crowd cars;
	for (int i = 0; i < 20; ++i)
		cars.Add(8, Sq(5.0f));
	const uint32_t honker = cars.Add(1, Sq(900.0f));
	cars.e[honker].deadline = STREAM_HONK_DEADLINE;
	cars.Run(10, MAX_CAR_STATES);
	cars.Forget();
	cars.Run(100, MAX_CAR_STATES);
	Check(cars.e[honker].sent == 100, "a far honking car goes out in every batch");

	Crowd peds;
	for (int i = 0; i < 30; ++i)
		peds.Add(8, Sq(5.0f));
	const uint32_t burning = peds.Add(1, Sq(900.0f));
	peds.e[burning].deadline = STREAM_FIRE_DEADLINE;
	peds.Run(10, MAX_PED_STATES);
	peds.Forget();
	peds.Run(200, MAX_PED_STATES);
	Check(peds.e[burning].longest <= STREAM_FIRE_DEADLINE,
	      "a far burning ped never waits longer than its deadline");
	Check(peds.e[burning].longest * STREAM_TICK_MS < REMOTE_FIRE_MS,
	      "which is short of the second his fire lasts on the receiver");
}

void TestTiesGoToTheNearer() {
	std::printf("equal credit\n");
	Crowd c;
	const uint32_t far  = c.Add(1, Sq(200.0f));
	const uint32_t near = c.Add(1, Sq(20.0f));
	c.Tick(2);   // both fresh; both go
	const auto one = c.Tick(1);
	Check(one.size() == 1 && one[0] == near, "the nearer of two equal rows goes first");
	const auto next = c.Tick(1);
	Check(next.size() == 1 && next[0] == far, "and the other one is next");
}

void TestTheCountersHold() {
	std::printf("the counters\n");
	StreamRow       row;
	StreamCandidate k;
	k.row    = &row;
	k.weight = STREAM_WEIGHT_MAX;
	PickStreamRows(&k, 1, 1);
	for (int i = 0; i < 10000; ++i)
		PickStreamRows(&k, 1, 0);
	Check(row.credit == 0xFFFF, "credit saturates rather than wrapping to nothing");
	Check(row.sinceSent == 254, "and so does the wait, short of 'never sent'");
	PickStreamRows(&k, 1, 1);
	Check(k.waited == 254 && row.credit == 0 && row.sinceSent == 0,
	      "a row that finally goes out reports the wait and starts again");

	StreamCandidate none[1];
	Check(PickStreamRows(none, 0, MAX_CAR_STATES) == 0, "no candidates, no rows");
}

} // namespace

int RunStreamPickTests() {
	g_streamFailures = 0;
	TestTheWeightFallsOffWithDistance();
	TestTheNearestViewer();
	TestTheDeadlinesFitWhatTheReceiverHolds();
	TestWhatTheObserverHolds();
	TestAnNpcRoundGoesToWhoeverCanSeeIt();
	TestAFewRowsAllGoEveryTick();
	TestNothingIsLeftFrozen();
	TestTheNearRowsGoMostOften();
	TestAFreshRowGoesFirst();
	TestAChangeGoesOutNextTick();
	TestADeadlineIsKept();
	TestTiesGoToTheNearer();
	TestTheCountersHold();
	return g_streamFailures;
}
