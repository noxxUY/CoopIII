// Unit tests for the remote-entity interpolation buffer.
//   xmake build interptest && xmake run interptest

#include "interp.h"

#include <cmath>
#include <cstdio>

using namespace coopiii;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

bool Near(float a, float b, float eps = 0.001f) {
	return std::fabs(a - b) < eps;
}

constexpr float PI = 3.14159265358979323846f;

void TestEmpty() {
	std::printf("\nempty buffer\n");
	InterpBuffer buf;
	Pose pose;
	Check(!buf.Sample(0, pose), "sampling an empty buffer fails");
	Check(!buf.SampleDelayed(0, pose), "delayed sample of empty buffer fails");
}

// ---- vehicles --------------------------------------------------------------

Quat Yaw(float radians) {
	return Quat{0.0f, 0.0f, std::sin(radians * 0.5f), std::cos(radians * 0.5f)};
}

// Two quaternions represent the same orientation if they agree up to sign,
// so the comparison has to allow for that. It's the whole reason Slerp needs
// its negation branch - ignore it here and a broken Slerp would still pass.
bool SameOrientation(const Quat &a, const Quat &b, float eps = 1e-3f) {
	const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	return std::fabs(std::fabs(dot) - 1.0f) < eps;
}

float YawOf(const Quat &q) {
	return 2.0f * std::atan2(q.z, q.w);
}

void TestSlerpEndpoints() {
	std::printf("\nslerp endpoints\n");
	const Quat a = Yaw(0.0f);
	const Quat b = Yaw(1.0f);
	Check(SameOrientation(Slerp(a, b, 0.0f), a), "t=0 is the start");
	Check(SameOrientation(Slerp(a, b, 1.0f), b), "t=1 is the end");

	const Quat mid = Slerp(a, b, 0.5f);
	Check(std::fabs(YawOf(mid) - 0.5f) < 1e-3f, "t=0.5 is halfway round");

	const float len = std::sqrt(mid.x * mid.x + mid.y * mid.y + mid.z * mid.z +
	                            mid.w * mid.w);
	Check(std::fabs(len - 1.0f) < 1e-4f, "and the result is still unit length");
}

void TestSlerpTakesTheShortWay() {
	std::printf("\nslerp does not go the long way round\n");
	// 170 degrees apart, far end negated. Same orientation, but it's the sign
	// that makes a naive lerp travel 190 degrees the wrong way round. On a car
	// that's a barrel roll mid lane-change.
	const Quat a = Yaw(0.0f);
	const Quat b = Yaw(2.9670f);            // 170 degrees
	const Quat bNeg{-b.x, -b.y, -b.z, -b.w};

	const Quat viaB    = Slerp(a, b, 0.5f);
	const Quat viaNegB = Slerp(a, bNeg, 0.5f);
	Check(SameOrientation(viaB, viaNegB),
	      "the negated end gives the same midpoint, not the opposite one");

	// Crossing the +/-pi seam the short way: 170 to -170 degrees is 20
	// degrees, not 340.
	const Quat from = Yaw(2.9670f);
	const Quat to   = Yaw(-2.9670f);
	const float midYaw = YawOf(Slerp(from, to, 0.5f));
	Check(std::fabs(std::fabs(midYaw) - 3.1416f) < 0.05f,
	      "170 to -170 passes through pi, not through zero");
}

void TestSlerpDegenerate() {
	std::printf("\nslerp, hostile input\n");
	const Quat a = Yaw(0.3f);
	Check(SameOrientation(Slerp(a, a, 0.5f), a),
	      "interpolating a rotation with itself is that rotation");

	const Quat zero{0.0f, 0.0f, 0.0f, 0.0f};
	const Quat r = Slerp(zero, zero, 0.5f);
	Check(r.w == r.w && r.x == r.x, "a zero pair does not produce NaN");
	Check(std::fabs(r.w - 1.0f) < 1e-4f, "it falls back to identity");
}

void TestVehicleBufferInterpolates() {
	std::printf("\nvehicle buffer\n");
	VehicleInterpBuffer b;
	Check(b.Empty(), "starts empty");

	VehicleTransform out;
	Check(!b.SampleDelayed(0, out), "an empty buffer samples nothing");

	b.Push(1000, Vec3{0.0f, 0.0f, 0.0f}, Yaw(0.0f), Vec3{});
	b.Push(1040, Vec3{4.0f, 0.0f, 0.0f}, Yaw(1.0f), Vec3{});

	Check(b.Sample(1020, out), "samples between two snapshots");
	Check(std::fabs(out.pos.x - 2.0f) < 1e-3f, "position is halfway");
	Check(std::fabs(YawOf(out.rot) - 0.5f) < 1e-3f, "and so is the rotation");
}

void TestVehicleSnapDistanceIsNotThePeds() {
	std::printf("\na fast car is not a teleport\n");
	// 8m in one 40ms snapshot works out to 200 km/h, which is just ordinary
	// driving in this game (not 200 m/s, do the unit conversion). The ped
	// buffer's 5m threshold would read that as a teleport and snap every frame.
	VehicleInterpBuffer b;
	b.Push(1000, Vec3{0.0f, 0.0f, 0.0f}, Yaw(0.0f), Vec3{});
	b.Push(1040, Vec3{8.0f, 0.0f, 0.0f}, Yaw(0.0f), Vec3{});

	VehicleTransform out;
	Check(b.Sample(1020, out), "samples");
	Check(std::fabs(out.pos.x - 4.0f) < 1e-3f,
	      "a fast car is interpolated, not snapped");

	// A real teleport still snaps.
	VehicleInterpBuffer t;
	t.Push(1000, Vec3{0.0f, 0.0f, 0.0f}, Yaw(0.0f), Vec3{});
	t.Push(1040, Vec3{500.0f, 0.0f, 0.0f}, Yaw(0.0f), Vec3{});
	Check(t.Sample(1020, out), "samples");
	Check(out.pos.x == 500.0f, "a respawn across the map snaps to the new one");
}

void TestVehicleRotationIsHeldNotExtrapolated() {
	std::printf("\nextrapolation holds the rotation\n");
	// Position extrapolates along the last velocity; rotation doesn't. Spin a
	// car on through its last angular velocity and the error compounds until
	// it's facing anywhere - much worse than a heading that's briefly stale.
	VehicleInterpBuffer b;
	b.Push(1000, Vec3{0.0f, 0.0f, 0.0f}, Yaw(0.7f), Vec3{10.0f, 0.0f, 0.0f});

	VehicleTransform out;
	Check(b.Sample(1100, out), "samples ahead of the newest snapshot");
	Check(out.pos.x > 0.5f, "position moved along the velocity");
	Check(std::fabs(YawOf(out.rot) - 0.7f) < 1e-3f, "rotation did not");
}

void TestMidpoint() {
	std::printf("\ninterpolation\n");
	InterpBuffer buf;
	buf.Push(1000, {0.0f, 0.0f, 0.0f}, 0.0f, {});
	buf.Push(1040, {4.0f, 2.0f, 0.0f}, 0.0f, {});

	Pose pose;
	Check(buf.Sample(1020, pose), "sample between two snapshots");
	Check(Near(pose.pos.x, 2.0f) && Near(pose.pos.y, 1.0f), "midpoint is halfway");

	Check(buf.Sample(1010, pose), "sample at a quarter");
	Check(Near(pose.pos.x, 1.0f), "quarter point is a quarter of the way");
}

void TestOutOfOrderDropped() {
	std::printf("\nout-of-order arrivals\n");
	InterpBuffer buf;
	buf.Push(1000, {0.0f, 0.0f, 0.0f}, 0.0f, {});
	buf.Push(1040, {4.0f, 0.0f, 0.0f}, 0.0f, {});
	const size_t before = buf.Size();

	buf.Push(1020, {99.0f, 99.0f, 99.0f}, 0.0f, {});   // stale, arrives late
	Check(buf.Size() == before, "stale snapshot is dropped");

	Pose pose;
	buf.Sample(1020, pose);
	Check(Near(pose.pos.x, 2.0f), "stale data did not corrupt the path");
}

void TestExtrapolationIsBounded() {
	std::printf("\nextrapolation\n");
	InterpBuffer buf;
	buf.Push(1000, {0.0f, 0.0f, 0.0f}, 0.0f, {10.0f, 0.0f, 0.0f});   // 10 u/s

	Pose pose;
	buf.Sample(1100, pose);   // 100ms ahead
	Check(Near(pose.pos.x, 1.0f), "extrapolates along velocity");

	buf.Sample(1250, pose);   // 250ms, at the cap
	Check(Near(pose.pos.x, 2.5f), "extrapolates up to the cap");

	Pose far;
	buf.Sample(5000, far);    // way past the cap
	Check(Near(far.pos.x, 2.5f), "freezes past the cap instead of drifting");
}

void TestSnapOnTeleport() {
	std::printf("\nteleport handling\n");
	InterpBuffer buf;
	buf.Push(1000, {0.0f, 0.0f, 0.0f}, 0.0f, {});
	buf.Push(1040, {500.0f, 500.0f, 0.0f}, 0.0f, {});   // respawn across the map

	Pose pose;
	buf.Sample(1020, pose);
	Check(Near(pose.pos.x, 500.0f) && Near(pose.pos.y, 500.0f),
	      "snaps to destination instead of sliding");

	InterpBuffer small;
	small.Push(1000, {0.0f, 0.0f, 0.0f}, 0.0f, {});
	small.Push(1040, {2.0f, 0.0f, 0.0f}, 0.0f, {});   // under SNAP_DISTANCE
	small.Sample(1020, pose);
	Check(Near(pose.pos.x, 1.0f), "normal movement still interpolates");
}

void TestAngleWrap() {
	std::printf("\nheading wrap\n");
	Check(Near(LerpAngle(0.0f, PI / 2.0f, 0.5f), PI / 4.0f), "plain lerp works");

	// Crossing the ±pi seam: from 170° to -170° is +20°, not -340°.
	const float from = 170.0f * PI / 180.0f;
	const float to   = -170.0f * PI / 180.0f;
	const float mid  = LerpAngle(from, to, 0.5f);
	const float deg  = mid * 180.0f / PI;
	Check(deg > 179.0f || deg < -179.0f, "takes the short way across the seam");

	InterpBuffer buf;
	buf.Push(1000, {}, from, {});
	buf.Push(1040, {}, to, {});
	Pose pose;
	buf.Sample(1020, pose);
	const float poseDeg = pose.heading * 180.0f / PI;
	Check(poseDeg > 179.0f || poseDeg < -179.0f, "buffer uses the short way too");
}

void TestDelayedSampling() {
	std::printf("\ndelayed sampling\n");
	InterpBuffer buf;
	// 25 Hz stream: a snapshot every 40ms.
	for (uint32_t t = 1000; t <= 1400; t += 40) {
		const float x = (t - 1000) / 40.0f;
		buf.Push(t, {x, 0.0f, 0.0f}, 0.0f, {});
	}

	Pose pose;
	Check(buf.SampleDelayed(5000, pose), "delayed sample succeeds");
	// Newest is t=1400 (x=10); rendering 100ms back lands at t=1300 (x=7.5).
	Check(Near(pose.pos.x, 7.5f), "renders 100ms behind the newest snapshot");
}

void TestBufferBounded() {
	std::printf("\nbuffer growth\n");
	InterpBuffer buf;
	for (uint32_t t = 0; t < 1000; t += 40)
		buf.Push(t, {}, 0.0f, {});
	Check(buf.Size() <= InterpBuffer::MAX_SAMPLES, "buffer stays bounded");
}

} // namespace

// This is the bug the whole clock exists for.
//
// Snapshots arrive at 25 Hz, frames happen at 60. The old SampleDelayed
// rendered at `newestSnapshot - DELAY_MS`, so the sampled instant only moved
// when a packet arrived. Two frames out of three came out identical to the
// last one, then the third jumped 40ms. On screen: a remote player moving at
// 25fps inside a 60fps game. Looks exactly like lag.
//
// So the test isn't "the pose is correct" - it always was. It's "the pose
// changes on a frame where nothing arrived".
void TestClockAdvancesBetweenSnapshots() {
	std::printf("\nthe playback clock runs on frames, not on packets\n");
	InterpBuffer buf;

	// A second of snapshots, one every 40ms, moving 1m each.
	for (uint32_t t = 1000; t <= 2000; t += 40)
		buf.Push(t, {(t - 1000) / 40.0f, 0.0f, 0.0f}, 0.0f, {});

	Pose a, b, c;
	Check(buf.SampleDelayed(10000, a), "first frame samples");

	// Three more frames at 60 Hz, nothing arriving in between. This is
	// exactly the situation the old code stood still through.
	Check(buf.SampleDelayed(10017, b), "second frame samples");
	Check(buf.SampleDelayed(10033, c), "third frame samples");

	Check(b.pos.x > a.pos.x, "the pose advances on a frame with no new snapshot");
	Check(c.pos.x > b.pos.x, "and on the next one");

	// Roughly a frame's worth of travel each time: 16ms at 1m per 40ms is
	// 0.4m. Bounds are generous on purpose - the point is the order of
	// magnitude, not the exact easing constant, which is free to change.
	const float step = b.pos.x - a.pos.x;
	Check(step > 0.1f && step < 1.0f, "by about one frame's worth, not one snapshot's");
}

// A clock that only ever advances drifts away from the sender's over time,
// so it gets eased back. A big gap isn't drift though, it's a stall or a
// reconnect, and easing through one of those at 5% a frame would take
// several seconds.
void TestClockResyncsAfterAGap() {
	std::printf("\nthe clock re-anchors after a gap\n");
	InterpBuffer buf;
	buf.Push(1000, {0.0f, 0.0f, 0.0f}, 0.0f, {});
	buf.Push(1040, {1.0f, 0.0f, 0.0f}, 0.0f, {});

	Pose out;
	Check(buf.SampleDelayed(10000, out), "running");

	// The stream jumps forward ten seconds. Whatever the clock was doing, it
	// needs to be over there now, not creeping toward it.
	buf.Push(11000, {50.0f, 0.0f, 0.0f}, 0.0f, {});
	buf.Push(11040, {51.0f, 0.0f, 0.0f}, 0.0f, {});
	Check(buf.SampleDelayed(10017, out), "still sampling");
	Check(out.pos.x > 40.0f, "re-anchored to the new timeline rather than easing");
}

// A buffer that empties and refills starts a new timeline. Carry the old
// clock into it and you'd spend the first half-second easing toward a number
// that no longer means anything.
void TestEmptyingStopsTheClock() {
	std::printf("\nan emptied buffer forgets its clock\n");
	InterpBuffer buf;
	buf.Push(1000, {0.0f, 0.0f, 0.0f}, 0.0f, {});
	buf.Push(1040, {1.0f, 0.0f, 0.0f}, 0.0f, {});

	Pose out;
	Check(buf.SampleDelayed(10000, out), "running");

	buf.Clear();
	Check(!buf.SampleDelayed(10017, out), "nothing to sample while empty");

	buf.Push(50000, {99.0f, 0.0f, 0.0f}, 0.0f, {});
	buf.Push(50040, {100.0f, 0.0f, 0.0f}, 0.0f, {});
	Check(buf.SampleDelayed(10033, out), "samples again once refilled");
	Check(out.pos.x > 90.0f, "on the new timeline, with no memory of the old one");
}

int main() {
	TestEmpty();
	TestClockAdvancesBetweenSnapshots();
	TestClockResyncsAfterAGap();
	TestEmptyingStopsTheClock();
	TestMidpoint();
	TestOutOfOrderDropped();
	TestExtrapolationIsBounded();
	TestSnapOnTeleport();
	TestAngleWrap();
	TestDelayedSampling();
	TestBufferBounded();
	TestSlerpEndpoints();
	TestSlerpTakesTheShortWay();
	TestSlerpDegenerate();
	TestVehicleBufferInterpolates();
	TestVehicleSnapDistanceIsNotThePeds();
	TestVehicleRotationIsHeldNotExtrapolated();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
