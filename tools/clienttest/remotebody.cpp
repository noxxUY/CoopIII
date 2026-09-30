// A remote player's body between two snapshots: client/src/remotebody.h, and
// the two pieces of the HUD that show what that body is carrying.

#include "remotebody.h"
#include "game/nametag.h"
#include "game/radar.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_bodyFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_bodyFailures;
}

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

PlayerStateBody Body(uint16_t animId) {
	PlayerStateBody b{};
	b.animId = animId;
	b.health = 100.0f;
	return b;
}

PlayerRideBody Ride(uint8_t kind, uint16_t id, float y) {
	PlayerRideBody r{};
	r.kind   = kind;
	r.id     = id;
	r.offset = {0.0f, y, 1.0f};
	return r;
}

void TestTheBodyIsTheOneAtThePosesInstant() {
	std::printf("\nthe body played is the one at the pose's instant\n");
	BodyTimeline t;
	Check(t.BodyAt(1000) == nullptr, "nothing before the first snapshot");
	for (uint16_t i = 0; i < 5; ++i)
		t.Push(1000 + i * 40, Body(i), PlayerRideBody{});
	Check(t.BodyAt(900)->animId == 0, "the oldest while the clock is still behind them all");
	Check(t.BodyAt(1040)->animId == 1, "the one stamped at that instant");
	Check(t.BodyAt(1079)->animId == 1, "and it holds until the next one's instant");
	Check(t.BodyAt(1080)->animId == 2, "then steps");
	Check(t.BodyAt(5000)->animId == 4, "the newest past the end");

	t.Push(1060, Body(99), PlayerRideBody{});
	Check(t.Size() == 5 && t.BodyAt(1060)->animId == 1,
	      "an older snapshot arriving late is dropped, as InterpBuffer drops it");
}

void TestTheRideIsInterpolatedOnTheVehicle() {
	std::printf("\nwhere on the vehicle\n");
	BodyTimeline t;
	PlayerRideBody r;
	t.Push(1000, Body(0), Ride(RIDE_VEHICLE, 7, 0.0f));
	t.Push(1040, Body(0), Ride(RIDE_VEHICLE, 7, 4.0f));
	Check(t.RideAt(1020, r) && r.id == 7 && Near(r.offset.y, 2.0f),
	      "halfway between two snapshots on the same car, halfway along it");
	Check(t.RideAt(2000, r) && Near(r.offset.y, 4.0f),
	      "held on the car past the newest, not extrapolated off it");
	t.Push(1080, Body(0), Ride(RIDE_VEHICLE, 8, 0.0f));
	Check(!t.RideAt(1060, r), "across a change of car, the world position answers");
	t.Push(1120, Body(0), PlayerRideBody{});
	Check(!t.RideAt(1200, r), "and once off it, no ride at all");

	BodyTimeline train;
	PlayerRideBody a = Ride(RIDE_TRAIN, 3, 0.0f), b = a;
	b.track = 1;
	train.Push(1000, Body(0), a);
	train.Push(1040, Body(0), b);
	Check(!train.RideAt(1020, r), "wagon 3 of the El is not wagon 3 of the subway");
}

void TestAFrameRoundTrips() {
	std::printf("\nthe vehicle's own frame\n");
	RideFrame f;
	f.pos = {100.0f, -50.0f, 10.0f};
	// Turned 90 degrees left: forward is -x, right is +y.
	f.right = {0.0f, 1.0f, 0.0f};
	f.fwd   = {-1.0f, 0.0f, 0.0f};
	f.up    = {0.0f, 0.0f, 1.0f};
	const Vec3 world{98.0f, -49.0f, 11.5f};
	const Vec3 o    = RideOffsetOf(f, world);
	Check(Near(o.x, 1.0f) && Near(o.y, 2.0f) && Near(o.z, 1.5f),
	      "a point two metres ahead and one to the right is (1, 2, 1.5) on it");
	const Vec3 back = RideWorldOf(f, o);
	Check(Near(back.x, world.x) && Near(back.y, world.y) && Near(back.z, world.z),
	      "and comes back to the same world point");
	Check(Near(RideFrameHeading(f), 1.5707963f),
	      "a car facing -x has the heading a ped facing -x has");

	f.pos    = {300.0f, 0.0f, 0.0f};
	const Vec3 moved = RideWorldOf(f, o);
	Check(Near(moved.x, 298.0f) && Near(moved.y, 1.0f),
	      "the same offset on the car somewhere else is the rider somewhere else");
	Check(RideOffsetPlausible(o) && !RideOffsetPlausible({0.0f, 40.0f, 0.0f}),
	      "forty metres off a car is not standing on it");
	Check(!RideOffsetPlausible({NAN, 0.0f, 0.0f}), "nor is a NaN");
}

void TestAStalledStreamStandsStill() {
	std::printf("\na player whose stream has stopped\n");
	Check(!RemoteStreamStalled(1000 + REMOTE_STALL_MS - 1, 1000), "just inside the limit is still moving");
	Check(RemoteStreamStalled(1000 + REMOTE_STALL_MS, 1000), "past it the copy stands");
	Check(REMOTE_STALL_MS == InterpBuffer::DELAY_MS + EXTRAPOLATE_MS,
	      "which is the delay plus the extrapolation, when the pose has stopped moving");

	PlayerStateBody run = Body(BODY_ANIM_RUN);
	run.moveSpeed = {0.2f, 0.1f, 0.0f};
	run.moveState = 3;
	run.animId2   = 0x77;
	const PlayerStateBody still = FrozenBody(run);
	Check(still.animId == BODY_ANIM_IDLE && still.moveState == BODY_MOVE_STILL &&
	          still.moveSpeed.x == 0.0f && still.moveSpeed.y == 0.0f,
	      "a run becomes the idle, with no speed and PEDMOVE_STILL");
	Check(still.animId2 == 0x77, "the overlay is left as it was");
	Check(FrozenBody(Body(BODY_ANIM_STARTWALK)).animId == BODY_ANIM_IDLE, "so does a start-walk");
	Check(FrozenBody(Body(0x20)).animId == 0x20, "anything that is not a stride is left alone");
}

void TestTheReplicaIsNotKilledByOurEngine() {
	std::printf("\nthe health a replica is given\n");
	Check(ReplicaHealth(64.0f, false) == 64.0f, "the owner's health, ordinarily");
	Check(ReplicaHealth(0.0f, false) > 1.0f,
	      "above ProcessControl's 1.0 while the owner's death has not been applied");
	Check(ReplicaHealth(1.0f, false) > 1.0f, "including exactly 1.0, which it also kills at");
	Check(ReplicaHealth(NAN, false) == REPLICA_HEALTH_FLOOR, "and a NaN");
	Check(ReplicaHealth(0.0f, true) == 0.0f, "the real zero once the death is ours");
}

void TestArmourOnTheTag() {
	std::printf("\narmour on a nametag\n");
	char out[16];
	Check(TagArmour(50.0f, 100.0f, out, sizeof out) && std::strcmp(out, "[ 50") == 0,
	      "the shield and the number, the HUD's way");
	Check(!TagArmour(1.0f, 100.0f, out, sizeof out) && out[0] == '\0',
	      "nothing at 1.0 or less, CHud::Draw's own gate");
	Check(!TagArmour(80.0f, 0.0f, out, sizeof out), "nothing on a wasted player");
	Check(TagArmour(2000.0f, 100.0f, out, sizeof out) && std::strcmp(out, "[ 999") == 0,
	      "capped at three digits");
}

void TestADeadPlayersArrowIsGrey() {
	std::printf("\na dead player on the radar\n");
	const ArrowRgb live = PlayerArrowRgb(2, false);
	const ArrowRgb dead = DeadArrowRgb(live);
	Check(dead.r == dead.g && dead.g == dead.b, "grey");
	Check(dead.r < 255 && (dead.r != live.r || dead.g != live.g || dead.b != live.b),
	      "and not their live colour");
	Check(DeadArrowAlpha(200) == 100, "half as solid");
}

} // namespace

int RunRemoteBodyTests() {
	TestTheBodyIsTheOneAtThePosesInstant();
	TestTheRideIsInterpolatedOnTheVehicle();
	TestAFrameRoundTrips();
	TestAStalledStreamStandsStill();
	TestTheReplicaIsNotKilledByOurEngine();
	TestArmourOnTheTag();
	TestADeadPlayersArrowIsGrey();
	return g_bodyFailures;
}
