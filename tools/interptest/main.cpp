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

// ---- engine speed ----------------------------------------------------------
//
// m_vecMoveSpeed is metres per 1/50 s step; the buffers extrapolate in m/s.
// interp.h has the addresses.

void TestEngineSpeedUnits() {
	std::printf("\nengine speed to metres per second\n");
	Check(ENGINE_STEPS_PER_SECOND == 50.0f, "one engine step is 1/50 s");
	const Vec3 v = MoveSpeedToMps(Vec3{0.5f, -0.1f, 0.02f});
	Check(Near(v.x, 25.0f) && Near(v.y, -5.0f) && Near(v.z, 1.0f),
	      "0.5 a step is 25 m/s, on every axis");
}

void TestACarCoastsAtItsRealSpeed() {
	std::printf("\na car coasting on a lost packet\n");
	VehicleInterpBuffer b;
	b.Push(1000, Vec3{0.0f, 0.0f, 0.0f}, Yaw(0.0f), MoveSpeedToMps(Vec3{0.5f, 0.0f, 0.0f}));

	VehicleTransform out;
	b.Sample(1200, out);
	Check(Near(out.pos.x, 5.0f), "0.5 a step (90 km/h) covers 5 m in 200 ms");
	b.Sample(5000, out);
	Check(Near(out.pos.x, 6.25f), "and stops at the 250 ms cap, 6.25 m");

	InterpBuffer p;
	p.Push(1000, {0.0f, 0.0f, 0.0f}, 0.0f, MoveSpeedToMps(Vec3{0.1f, 0.0f, 0.0f}));
	Pose pose;
	p.Sample(1100, pose);
	Check(Near(pose.pos.x, 0.5f), "a ped running at 0.1 a step covers 0.5 m in 100 ms");
}

void TestALoneSampleIsHeldNotThrownForward() {
	std::printf("\nthe first sample of a stream\n");
	// SampleDelayed starts DELAY_MS behind the only sample there is. That
	// used to wrap and extrapolate the full 250 ms.
	const Vec3 fast = MoveSpeedToMps(Vec3{0.5f, 0.0f, 0.0f});

	VehicleInterpBuffer b;
	b.Push(5000, Vec3{10.0f, 0.0f, 0.0f}, Yaw(0.0f), fast);
	VehicleTransform out;
	Check(b.SampleDelayed(20000, out), "samples");
	Check(Near(out.pos.x, 10.0f), "a car's first frame is where it was sent from");
	Check(b.Sample(4900, out) && Near(out.pos.x, 10.0f), "and so is any instant before it");

	InterpBuffer p;
	p.Push(5000, {3.0f, 0.0f, 0.0f}, 0.0f, fast);
	Pose pose;
	Check(p.SampleDelayed(20000, pose), "samples");
	Check(Near(pose.pos.x, 3.0f), "same for a ped");
}

// Frames at 60 fps and a car at 25 m/s streamed at `intervalMs`, skipping the
// samples in [dropFrom, dropTo). Returns the smallest and largest distance the
// car moved in one frame once the playback clock has settled.
struct Steps {
	float min = 1e9f;
	float max = 0.0f;
};

Steps Drive(uint32_t intervalMs, uint32_t dropFrom, uint32_t dropTo,
            uint32_t jitterMs = 0) {
	constexpr float    MPS   = 25.0f;
	constexpr uint32_t START = 10000;
	const Vec3 vel = MoveSpeedToMps(Vec3{MPS / ENGINE_STEPS_PER_SECOND, 0.0f, 0.0f});

	VehicleInterpBuffer b;
	uint32_t nextSend = START;
	float    lastX    = 0.0f;
	bool     have     = false;
	Steps    s;
	static const uint32_t FRAME[3] = {17, 17, 16};
	uint32_t now = START;
	// Each sample lands 0..jitterMs after it was sent, in a fixed shuffle so
	// the run is the same every time.
	auto arrives = [&](uint32_t sent) {
		return sent + (jitterMs ? (sent / intervalMs * 37) % (jitterMs + 1) : 0);
	};
	for (int f = 0; now < START + 2000; ++f) {
		while (arrives(nextSend) <= now) {
			if (nextSend < dropFrom || nextSend >= dropTo)
				b.Push(nextSend, Vec3{MPS * (nextSend - START) / 1000.0f, 0.0f, 0.0f},
				       Yaw(0.0f), vel);
			nextSend += intervalMs;
		}
		VehicleTransform at;
		if (b.SampleDelayed(now, at)) {
			if (have && now > START + 300) {
				const float step = at.pos.x - lastX;
				s.min = step < s.min ? step : s.min;
				s.max = step > s.max ? step : s.max;
			}
			lastX = at.pos.x;
			have  = true;
		}
		now += FRAME[f % 3];
	}
	return s;
}

void TestLostPacketsDoNotStallACar() {
	std::printf("\nthree lost packets on a car at 25 m/s\n");
	// About 0.42 m a frame, a little less while the clock eases back. With
	// the velocity fifty times too small the frames inside the gap moved
	// under a centimetre and the first one after it jumped well over a metre.
	const Steps s = Drive(40, 11000, 11120);
	std::printf("    per-frame step %.3f .. %.3f m\n", s.min, s.max);
	Check(s.min > 0.2f, "it keeps moving through the gap");
	Check(s.max < 0.55f, "and doesn't jump when the stream comes back");
}

void TestTenHertzTrafficKeepsMoving() {
	std::printf("\none lost row at 10 Hz, extrapolated\n");
	// The traffic stream's limiter lands on a 60 fps frame, so rows really
	// go out about every 117 ms, with some jitter on arrival. One lost row
	// is a 234 ms gap, and a 100 ms delay doesn't cover it. This is plain
	// SampleDelayed, which bridges the gap along the last velocity. Traffic
	// itself no longer reads it that way - see the SampleDelayedHeld tests
	// below for what it does instead and what that costs.
	const Steps s = Drive(117, 10930, 10940, 30);
	std::printf("    per-frame step %.3f .. %.3f m\n", s.min, s.max);
	Check(s.min > 0.2f, "it keeps moving through the gap");
	Check(s.max < 0.55f, "and doesn't catch up in a jump");
}

void TestAStallStaysInsideTheCap() {
	std::printf("\na stream that stops\n");
	VehicleInterpBuffer b;
	const Vec3 vel = MoveSpeedToMps(Vec3{0.5f, 0.0f, 0.0f});
	for (uint32_t t = 10000; t <= 10400; t += 40)
		b.Push(t, Vec3{25.0f * (t - 10000) / 1000.0f, 0.0f, 0.0f}, Yaw(0.0f), vel);
	const float lastX = 25.0f * 0.4f;

	static const uint32_t FRAME[3] = {17, 17, 16};
	uint32_t now = 10000;
	VehicleTransform at;
	float furthest = 0.0f;
	for (int f = 0; now < 13000; ++f) {
		b.SampleDelayed(now, at);
		furthest = at.pos.x > furthest ? at.pos.x : furthest;
		now += FRAME[f % 3];
	}
	std::printf("    held %.2f m past the last sample\n", at.pos.x - lastX);
	Check(furthest - lastX <= 6.25f + 0.01f, "never further than 250 ms at 25 m/s");
	Check(at.pos.x - lastX > 5.0f, "and holds most of that, not snapping back");
}

// ---- traffic: held on the last row, never coasted past it ------------------
//
// A traffic car's host streams only the eight nearest its own player, so a
// car's rows can stop for good while it lives on. SampleDelayedHeld is what
// Client::CorrectAmbientCars reads, and these pin what it does with a stream
// that stops and starts.

// One row of a host's car: when it was sent, when it lands here, where the
// car was and how fast it was going (m/s along x).
struct Row {
	uint32_t sent;
	uint32_t arrives;
	float    x;
	float    mps;
};

// Frames at 60 fps from `start` to `end`, pushing each row once it has
// arrived and sampling either way. Records every frame's x.
struct Trace {
	float    xs[400];
	uint32_t at[400];
	int      n = 0;
};

Trace Play(const Row *rows, int count, uint32_t start, uint32_t end, bool held) {
	static const uint32_t FRAME[3] = {17, 17, 16};
	VehicleInterpBuffer b;
	Trace t;
	int next = 0;
	uint32_t now = start;
	for (int f = 0; now < end && t.n < 400; ++f) {
		while (next < count && rows[next].arrives <= now) {
			b.Push(rows[next].sent, Vec3{rows[next].x, 0.0f, 0.0f}, Yaw(0.3f),
			       Vec3{rows[next].mps, 0.0f, 0.0f});
			++next;
		}
		VehicleTransform out;
		if (held ? b.SampleDelayedHeld(now, out) : b.SampleDelayed(now, out)) {
			t.xs[t.n] = out.pos.x;
			t.at[t.n] = now;
			++t.n;
		}
		now += FRAME[f % 3];
	}
	return t;
}

// A car at 25 m/s whose rows go out every 117 ms from 10000, landing 0..jitter
// ms late, with the rows sent in [quietFrom, quietTo) never arriving.
int Cruise(Row *out, int max, uint32_t quietFrom, uint32_t quietTo, uint32_t jitter,
           uint32_t until) {
	int n = 0;
	for (uint32_t s = 10000; s < until && n < max; s += 117) {
		if (s >= quietFrom && s < quietTo)
			continue;
		const uint32_t late = jitter ? (s / 117 * 37) % (jitter + 1) : 0;
		out[n++] = Row{s, s + late, 25.0f * (s - 10000) / 1000.0f, 25.0f};
	}
	return n;
}

void TestHeldIsSampleDelayedWhileRowsFlow() {
	std::printf("\ntraffic that is streaming normally\n");
	// Up to 50 ms of arrival jitter the playback clock never reaches the
	// newest row, so the clamp never fires. Measured past that it depends on
	// how the late rows fall, which is the point of the next two tests.
	bool same = true;
	for (uint32_t jitter = 0; jitter <= 50; jitter += 10) {
		Row rows[64];
		const int n = Cruise(rows, 64, 0, 0, jitter, 13000);
		const Trace plain = Play(rows, n, 10000, 13000, false);
		const Trace held  = Play(rows, n, 10000, 13000, true);
		same = same && plain.n == held.n;
		for (int i = 0; same && i < plain.n; ++i)
			same = plain.xs[i] == held.xs[i];
	}
	Check(same, "10 Hz with up to 50 ms of jitter: every frame is where SampleDelayed puts it");
}

void TestHeldStopsOnTheLastRow() {
	std::printf("\ntraffic that drops out of its host's nearest eight\n");
	// Rows up to 11053, then nothing: the car is still on its host, it just
	// isn't one of the eight any more.
	Row rows[64];
	const int n = Cruise(rows, 64, 11100, 99999, 0, 14000);
	const float lastX = rows[n - 1].x;

	const Trace held = Play(rows, n, 10000, 14000, true);
	float furthest = 0.0f;
	for (int i = 0; i < held.n; ++i)
		furthest = held.xs[i] > furthest ? held.xs[i] : furthest;
	std::printf("    last row at %.2f m, held at %.4f m, furthest %.4f m\n", lastX,
	            held.xs[held.n - 1], furthest);
	Check(furthest <= lastX + 1e-4f, "never drawn past the last row its host sent");
	Check(Near(held.xs[held.n - 1], lastX), "and stands exactly on it");

	const Trace plain = Play(rows, n, 10000, 14000, false);
	std::printf("    SampleDelayed would have stood it at %.2f m\n",
	            plain.xs[plain.n - 1]);
	Check(plain.xs[plain.n - 1] - lastX > 5.0f,
	      "(where SampleDelayed leaves it: over 5 m on at 25 m/s)");

	VehicleInterpBuffer b;
	b.Push(1000, Vec3{0.0f, 0.0f, 0.0f}, Yaw(0.0f), Vec3{25.0f, 0.0f, 0.0f});
	b.Push(1100, Vec3{2.5f, 0.0f, 0.0f}, Yaw(0.7f), Vec3{25.0f, 0.0f, 0.0f});
	VehicleTransform out;
	for (uint32_t now = 0; now <= 3000; now += 16)
		b.SampleDelayedHeld(now, out);
	Check(Near(out.pos.x, 2.5f) && SameOrientation(out.rot, Yaw(0.7f)),
	      "position and heading both the newest row's");
}

void TestHeldTrafficBridgesALostRowWithoutAJump() {
	std::printf("\none lost row of traffic, held\n");
	// The cost of not extrapolating: a lost row stops the car for as long as
	// the row is overdue. What it must not do is jump when the row turns up.
	Row rows[64];
	const int n = Cruise(rows, 64, 10936, 10937, 30, 12500);   // loses 10936
	const Trace t = Play(rows, n, 10000, 12500, true);
	float maxStep = 0.0f;
	float minStep = 1e9f;
	int   stoppedFrames = 0;
	for (int i = 1; i < t.n; ++i) {
		if (t.at[i] < 10300)
			continue;
		const float step = t.xs[i] - t.xs[i - 1];
		maxStep = step > maxStep ? step : maxStep;
		minStep = step < minStep ? step : minStep;
		if (step < 0.01f)
			++stoppedFrames;
	}
	std::printf("    per-frame step %.3f .. %.3f m, %d frame(s) standing\n", minStep,
	            maxStep, stoppedFrames);
	Check(minStep >= 0.0f, "it never goes backwards");
	// A 17 ms frame at 25 m/s is 0.425 m. It comes out of the stop at about
	// 1.3 times that for a few frames while the clock eases back, which is
	// catching up, not a jump; the extrapolated version tops out at 0.525.
	Check(maxStep < 0.6f, "and doesn't catch up in a jump");
	Check(stoppedFrames <= 6, "it stands for a handful of frames, not a batch and more");
}

void TestHeldTrafficComesBackForwards() {
	std::printf("\ntraffic that comes back into the eight\n");
	// Braking for a light while nobody is told. The rows stop at 25 m/s; the
	// host's car pulls up 4 m further on and is standing there when it gets
	// back into the eight a second and a half later. Extrapolating put the
	// replica about 5.5 m on, past where the host stopped, so the first row
	// back dragged it backwards. Held, it waits on the last row and then
	// moves up.
	Row rows[64];
	int n = Cruise(rows, 64, 11100, 99999, 0, 11100);
	const float lastX = rows[n - 1].x;
	const float stopX = lastX + 4.0f;
	for (uint32_t s = 12600; s < 14500; s += 117)
		rows[n++] = Row{s, s, stopX, 0.0f};

	const Trace held  = Play(rows, n, 10000, 14500, true);
	const Trace plain = Play(rows, n, 10000, 14500, false);
	auto backwards = [](const Trace &t) {
		float worst = 0.0f;
		for (int i = 1; i < t.n; ++i) {
			const float step = t.xs[i] - t.xs[i - 1];
			worst = step < worst ? step : worst;
		}
		return worst;
	};
	std::printf("    worst backward step: held %.2f m, SampleDelayed %.2f m\n",
	            backwards(held), backwards(plain));
	Check(backwards(held) >= 0.0f, "held, it only ever moves forwards");
	Check(Near(held.xs[held.n - 1], stopX), "and ends where its host's car stopped");
	Check(backwards(plain) < -1.0f, "(extrapolated, it was pulled back over a metre)");

	// And a gap long enough to leave the clock behind by more than
	// CLOCK_RESYNC_MS, on a car still driving: it moves up to the stream and
	// carries on with it, in one step forward and no second copy of anything
	// - the same buffer, the same samples.
	Row far[64];
	int m = Cruise(far, 64, 11100, 12900, 0, 14500);
	const Trace t = Play(far, m, 10000, 14500, true);
	Check(backwards(t) >= 0.0f, "a long gap on a moving car: forwards only");
	const float endX = t.xs[t.n - 1];
	const float lastSent = far[m - 1].x;
	std::printf("    back on the stream at %.2f m, newest row %.2f m\n", endX, lastSent);
	const float endStep = t.xs[t.n - 1] - t.xs[t.n - 2];
	Check(endX > lastSent - 4.5f && endX <= lastSent && endStep > 0.3f && endStep < 0.55f,
	      "and runs DELAY_MS behind it at its own speed again, as if it had never left");
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
	TestEngineSpeedUnits();
	TestACarCoastsAtItsRealSpeed();
	TestALoneSampleIsHeldNotThrownForward();
	TestLostPacketsDoNotStallACar();
	TestTenHertzTrafficKeepsMoving();
	TestAStallStaysInsideTheCap();
	TestHeldIsSampleDelayedWhileRowsFlow();
	TestHeldStopsOnTheLastRow();
	TestHeldTrafficBridgesALostRowWithoutAJump();
	TestHeldTrafficComesBackForwards();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
