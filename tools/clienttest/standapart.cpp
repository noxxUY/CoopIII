// Two players on one spot (game/standapart.h): who keeps it, where the other
// goes, and when. game/standapart.cpp does the engine part and none of that
// runs here; every decision it makes does.

#include "game/standapart.h"

#include <cmath>
#include <cstdio>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_standFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_standFailures;
}

float Flat(float ax, float ay, float bx, float by) {
	return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

// main.scm's new-game player, where every machine's intro ends.
constexpr Vec3 BRIDGE{811.875f, -939.9375f, 35.75f};

uint8_t Mask(std::initializer_list<uint8_t> ids) {
	uint8_t m = 0;
	for (uint8_t id : ids)
		m = static_cast<uint8_t>(m | PlayerBit(id));
	return m;
}

void TestWhoKeepsTheSpot() {
	std::printf("\nwho keeps a spot two players were put on\n");
	Check(StandOutranks(0, 1, INVALID_PLAYER) && !StandOutranks(1, 0, INVALID_PLAYER),
	      "with no mission running, the lower player id stays");
	Check(StandOutranks(2, 0, 2) && !StandOutranks(0, 2, 2),
	      "with one running, its owner stays whatever the ids say");
	Check(StandOutranks(0, 1, 2) && !StandOutranks(1, 0, 2),
	      "and between two others the lower id still does");
	Check(!StandOutranks(3, 3, INVALID_PLAYER), "nobody outranks themselves");
}

void TestGiveMeLiberty() {
	std::printf("\nboth intros end on Give Me Liberty's marker\n");
	const uint8_t   players = Mask({0, 1});
	const StandPeer alice{0, BRIDGE};
	const StandPeer bob{1, BRIDGE};

	const StandVerdict a = DecideStand(0, BRIDGE, &bob, 1, players, INVALID_PLAYER);
	Check(a.move == StandMove::None, "alice, 0, stays on the marker");
	const StandVerdict b = DecideStand(1, BRIDGE, &alice, 1, players, INVALID_PLAYER);
	Check(b.move == StandMove::Slot && b.anchorId == 0 && b.rank == 0 && b.count == 1,
	      "bob, 1, is on her spot and takes the first place round it");
	Check(std::fabs(b.awayX * b.awayX + b.awayY * b.awayY - 1.0f) < 0.01f,
	      "with a way off it even though the two are on one point");

	float sx = 0, sy = 0;
	SpreadSpot(b.anchor.x, b.anchor.y, b.rank, b.count, 0, &sx, &sy, STAND_SLOT_RADII_M[0]);
	const float out = Flat(sx, sy, BRIDGE.x, BRIDGE.y);
	Check(out > STAND_CLOSE_M * 2.0f && out < 2.0f,
	      "his place is clear of her, and near enough to be beside her");

	// Once he is there, nothing moves either of them again.
	const Vec3      moved{sx, sy, BRIDGE.z};
	const StandPeer bobThere{1, moved};
	Check(DecideStand(1, moved, &alice, 1, players, INVALID_PLAYER).move == StandMove::None,
	      "from his place he is clear of her");
	Check(DecideStand(0, BRIDGE, &bobThere, 1, players, INVALID_PLAYER).move == StandMove::None,
	      "and she is never moved onto or off anything");

	// Give Me Liberty then starts with bob its owner: he keeps his place and
	// alice, now outranked, is not inside him.
	Check(DecideStand(0, BRIDGE, &bobThere, 1, players, 1).move == StandMove::None,
	      "the mission starting with bob its owner moves nobody");
	// Had it started first, with both still on the marker, bob would stay.
	const StandVerdict o = DecideStand(0, BRIDGE, &bob, 1, players, 1);
	Check(o.move == StandMove::Slot && o.anchorId == 1,
	      "on one point in bob's mission, alice is the one who moves");
	Check(DecideStand(1, BRIDGE, &alice, 1, players, 1).move == StandMove::None,
	      "and bob, its owner, keeps the exact spot");
}

void TestFourOnOneSpot() {
	std::printf("\nfour players respawned at one hospital\n");
	const uint8_t players = Mask({0, 1, 2, 3});
	StandPeer     all[4]  = {{0, BRIDGE}, {1, BRIDGE}, {2, BRIDGE}, {3, BRIDGE}};
	float         px[4] = {}, py[4] = {};
	for (uint8_t me = 1; me < 4; ++me) {
		StandPeer others[3];
		size_t    n = 0;
		for (const StandPeer &p : all)
			if (p.playerId != me)
				others[n++] = p;
		const StandVerdict v = DecideStand(me, BRIDGE, others, n, players, INVALID_PLAYER);
		Check(v.move == StandMove::Slot && v.anchorId == 0 && v.rank == me - 1 && v.count == 3,
		      me == 1   ? "1 goes round 0, in the first place of three"
		      : me == 2 ? "2 goes round 0 too, in the second"
		                : "3 in the third");
		SpreadSpot(BRIDGE.x, BRIDGE.y, v.rank, v.count, 0, &px[me], &py[me], STAND_SLOT_RADII_M[0]);
	}
	bool apart = true;
	for (int i = 1; i < 4; ++i)
		for (int j = i + 1; j < 4; ++j)
			apart = apart && Flat(px[i], py[i], px[j], py[j]) > STAND_CLOSE_M * 2.0f;
	Check(apart, "and no two of the three places are on each other");

	// 1 has already gone: 2 and 3 keep the places they had, rather than 2
	// taking 1's now that 1 is not on the spot any more.
	all[1].pos = Vec3{px[1], py[1], BRIDGE.z};
	StandPeer others[3] = {all[0], all[1], all[3]};
	const StandVerdict v = DecideStand(2, BRIDGE, others, 3, players, INVALID_PLAYER);
	Check(v.rank == 1 && v.count == 3, "a place is the rank's, not the order they moved in");
}

void TestTooCloseSteps() {
	std::printf("\ntoo close, not on one point\n");
	const uint8_t   players = Mask({0, 1});
	const StandPeer alice{0, Vec3{0.0f, 0.0f, 10.0f}};
	const StandVerdict v =
	    DecideStand(1, Vec3{0.4f, 0.0f, 10.0f}, &alice, 1, players, INVALID_PLAYER);
	Check(v.move == StandMove::Nudge && std::fabs(v.awayX - 1.0f) < 0.01f &&
	          std::fabs(v.awayY) < 0.01f,
	      "0.4 m from her, bob steps straight away from her");
	Check(DecideStand(1, Vec3{0.8f, 0.0f, 10.0f}, &alice, 1, players, INVALID_PLAYER).move ==
	          StandMove::None,
	      "0.8 m from her is standing beside her, left alone");
	Check(DecideStand(1, Vec3{0.0f, 0.0f, 13.0f}, &alice, 1, players, INVALID_PLAYER).move ==
	          StandMove::None,
	      "three metres over her is a bridge, not her");
	const StandPeer nowhere{0, Vec3{NAN, 0.0f, 0.0f}};
	Check(DecideStand(1, Vec3{0.0f, 0.0f, 0.0f}, &nowhere, 1, players, INVALID_PLAYER).move ==
	          StandMove::None,
	      "a position with a NaN in it is nobody");

	Check(std::fabs(StandNudgeStep(0.4f, 16) - STAND_NUDGE_MPS * 0.016f) < 0.001f,
	      "a frame's step is a walk's worth");
	Check(StandNudgeStep(0.4f, 5000) <= STAND_NUDGE_MPS * 0.1f + 0.001f,
	      "a long frame does not make it a jump");
	Check(StandNudgeStep(0.62f, 100) <= 0.031f && StandNudgeStep(0.7f, 16) == 0.0f,
	      "and it never steps further than takes it clear");
}

void TestItWaitsBeforeMoving() {
	std::printf("\nbrushing past is left to the engine\n");
	const uint8_t      players = Mask({0, 1});
	const StandPeer    alice{0, BRIDGE};
	const StandVerdict v = DecideStand(1, BRIDGE, &alice, 1, players, INVALID_PLAYER);
	StandWatch         w;
	Check(!w.Due(v, 1000), "not at the first frame of it");
	Check(!w.Due(v, 1000 + STAND_HOLD_MS - 1), "nor just before the wait is over");
	Check(w.Due(v, 1000 + STAND_HOLD_MS), "but once it has lasted");
	Check(!w.Due(StandVerdict{}, 1500), "apart again, it stops");
	Check(!w.Due(v, 1600), "and starts waiting afresh");
}

void TestTheMissionRingKeepsPlacesApart() {
	std::printf("\nthe mission's ring leaves each participant their own place\n");
	// With two participants on a ring of four, the first one's second try
	// is the second one's own place.
	float ax = 0, ay = 0, bx = 0, by = 0;
	SpreadSpot(0.0f, 0.0f, 0, 2, 2, &ax, &ay, 3.0f);
	SpreadSpot(0.0f, 0.0f, 1, 2, 0, &bx, &by, 3.0f);
	Check(Flat(ax, ay, bx, by) < 0.01f, "the try that lands on the other's place exists");
	Check(SpreadSpotTaken(0, 2, 2), "and is skipped");
	Check(!SpreadSpotTaken(0, 2, 0) && !SpreadSpotTaken(0, 2, 1) && !SpreadSpotTaken(0, 1, 2),
	      "a first place, half a place round, and a lone participant are not");
	bool clash = false;
	for (uint8_t count = 1; count <= 7; ++count)
		for (uint8_t rank = 0; rank < count; ++rank)
			for (uint8_t attempt = 1; attempt < SPREAD_ATTEMPTS; ++attempt) {
				if (SpreadSpotTaken(rank, count, attempt))
					continue;
				float x = 0, y = 0;
				SpreadSpot(0.0f, 0.0f, rank, count, attempt, &x, &y, 1.5f);
				for (uint8_t other = 0; other < count; ++other) {
					float ox = 0, oy = 0;
					SpreadSpot(0.0f, 0.0f, other, count, 0, &ox, &oy, 1.5f);
					if (other != rank && Flat(x, y, ox, oy) < 0.5f)
						clash = true;
				}
			}
	Check(!clash, "no try left over is within half a metre of another's own place");
}

} // namespace

int RunStandApartTests() {
	g_standFailures = 0;
	TestWhoKeepsTheSpot();
	TestGiveMeLiberty();
	TestFourOnOneSpot();
	TestTooCloseSteps();
	TestItWaitsBeforeMoving();
	TestTheMissionRingKeepsPlacesApart();
	return g_standFailures;
}
