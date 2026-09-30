// Aim pitch on a remote player's ped: the pure half of it, client/src/game/pedaim.h.
//
// The detour itself can't run here - it sits on CPedIK::PointGunInDirection
// inside the game. What can is every decision it makes: whether a pitch off
// the wire is usable, which ped gets it and for which frame, and when our own
// recorded aim is still the one to send.

#include "game/pedaim.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_aimFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_aimFailures;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

void TestAPitchOffTheWireIsMadeSafe() {
	std::printf("a pitch off the wire, before it reaches the IK\n");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();

	Check(Near(AimPitchFromWire(0.3f), 0.3f), "an ordinary pitch is passed through");
	Check(Near(AimPitchFromWire(-0.7f), -0.7f), "so is a negative one, which is up");
	Check(AimPitchFromWire(nan) == 0.0f, "NaN reads as level");
	Check(AimPitchFromWire(inf) == 0.0f, "+inf reads as level");
	Check(AimPitchFromWire(-inf) == 0.0f, "-inf reads as level");
	Check(Near(AimPitchFromWire(2.5f), AIM_PITCH_LIMIT), "past straight down is straight down");
	Check(Near(AimPitchFromWire(-2.5f), -AIM_PITCH_LIMIT), "past straight up is straight up");
	Check(Near(AimPitchFromWire(1.0e8f), AIM_PITCH_LIMIT),
	      "a huge finite value is clamped, not wrapped");
	Check(AIM_PITCH_LIMIT > 70.0f * 0.0174533f,
	      "the clamp is wider than the upper arm's own 70 degrees, so it never shows");
}

void TestOnlyThisFramesReplicasGetAPitch() {
	std::printf("which peds get the wire's pitch\n");
	int peds[MAX_PLAYERS + 2] = {};
	const void *a = &peds[0];
	const void *b = &peds[1];

	ReplicaPitchTable t;
	float p = 99.0f;
	Check(!t.Find(a, 10, p) && p == 99.0f, "an empty table answers nobody");

	t.Set(a, 0.25f, 10);
	Check(t.Find(a, 10, p) && Near(p, 0.25f), "a replica set this frame gets its pitch");
	Check(!t.Find(a, 11, p), "and not next frame - AimGun has to be asked again");
	Check(!t.Find(a, 9, p), "nor a frame before it");
	Check(!t.Find(b, 10, p), "a ped nobody set, like the local player, keeps its own pitch");
	Check(!t.Find(nullptr, 10, p), "a null ped is nobody");

	t.Set(nullptr, 0.5f, 10);
	Check(!t.Find(nullptr, 10, p), "and setting a null ped does nothing");

	t.Set(a, -0.4f, 10);
	Check(t.Find(a, 10, p) && Near(p, -0.4f), "setting a ped twice in a frame keeps the newest");

	t.Set(a, std::numeric_limits<float>::quiet_NaN(), 10);
	Check(t.Find(a, 10, p) && p == 0.0f, "the table stores the made-safe pitch, not the raw one");

	t.Clear();
	for (int i = 0; i < MAX_PLAYERS; ++i)
		t.Set(&peds[i], 0.01f * float(i + 1), 20);
	bool all = true;
	for (int i = 0; i < MAX_PLAYERS; ++i)
		all = all && t.Find(&peds[i], 20, p) && Near(p, 0.01f * float(i + 1));
	Check(all, "every player aiming in the same frame fits");

	t.Set(&peds[MAX_PLAYERS], 0.9f, 20);
	Check(!t.Find(&peds[MAX_PLAYERS], 20, p), "one more than that in a frame is dropped");
	Check(t.Find(&peds[0], 20, p) && Near(p, 0.01f), "and doesn't evict anyone already there");

	t.Set(&peds[MAX_PLAYERS + 1], 0.6f, 21);
	Check(t.Find(&peds[MAX_PLAYERS + 1], 21, p) && Near(p, 0.6f),
	      "next frame, last frame's entries make room");
	Check(!t.Find(&peds[1], 21, p), "and none of them count for it");
}

void TestOurOwnRecordedAimIsFreshOrNot() {
	std::printf("when our own recorded aim is the one to send\n");
	int me = 0, other = 0;

	LocalAimRecord rec;
	Check(!LocalAimFresh(rec, &me, 5), "nothing recorded yet");

	rec = LocalAimRecord{&me, 1.0f, -0.2f, 100, true};
	Check(LocalAimFresh(rec, &me, 100), "same frame");
	Check(LocalAimFresh(rec, &me, 101), "one frame old, the usual case from PreFrame");
	Check(LocalAimFresh(rec, &me, 100 + LOCAL_AIM_MAX_AGE_FRAMES), "at the limit");
	Check(!LocalAimFresh(rec, &me, 101 + LOCAL_AIM_MAX_AGE_FRAMES), "past it");
	Check(!LocalAimFresh(rec, &me, 99), "from the future, after a load reset the counter");
	Check(!LocalAimFresh(rec, &other, 100), "recorded for a different player ped");
	Check(!LocalAimFresh(rec, nullptr, 100), "no player ped at all");

	rec = LocalAimRecord{&me, 0.0f, 0.0f, 0xFFFFFFFFu, true};
	Check(LocalAimFresh(rec, &me, 1), "across the frame counter wrapping");

	rec.valid = false;
	rec.frame = 1;
	Check(!LocalAimFresh(rec, &me, 1), "an invalid record never counts");
}

} // namespace

int RunAimPitchTests() {
	g_aimFailures = 0;
	TestAPitchOffTheWireIsMadeSafe();
	TestOnlyThisFramesReplicasGetAPitch();
	TestOurOwnRecordedAimIsFreshOrNot();
	return g_aimFailures;
}
