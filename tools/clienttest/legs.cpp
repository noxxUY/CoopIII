// A remote player's legs: client/src/remoteloco.h, BodyTimeline::LegsAt, and
// the image facts ped.cpp's ApplyLegs rests on.

#include "remotebody.h"
#include "remoteloco.h"
#include "game/pedanim.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_legFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_legFailures;
}

bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

PlayerStateBody Legs(uint16_t animId, float walk, float run, float sprint, float idle, float start,
                     float phase = 0.0f) {
	PlayerStateBody b{};
	b.animId                      = animId;
	b.animSpeed                   = 1.0f;
	b.locoWeight[LOCO_WALK]       = LocoWeightOnWire(walk);
	b.locoWeight[LOCO_RUN]        = LocoWeightOnWire(run);
	b.locoWeight[LOCO_SPRINT]     = LocoWeightOnWire(sprint);
	b.locoWeight[LOCO_IDLE]       = LocoWeightOnWire(idle);
	b.locoWeight[LOCO_STARTWALK]  = LocoWeightOnWire(start);
	b.locoPhase                   = LocoPhaseOnWire(phase, 1.0f);
	return b;
}

void TestTheWire() {
	std::printf("\nthe legs on the wire\n");
	Check(LocoWeightOnWire(0.0f) == 0 && LocoWeightOnWire(1.0f) == 255 &&
	          LocoWeightOnWire(2.0f) == 255 && LocoWeightOnWire(-1.0f) == 0 &&
	          LocoWeightOnWire(NAN) == 0,
	      "a weight is 0..255, saturated, and a NaN is nothing");
	Check(Near(LocoWeightOffWire(LocoWeightOnWire(0.37f)), 0.37f, 0.003f),
	      "and comes back within a 255th");
	Check(LocoPhaseOnWire(0.0f, 0.8f) == 0 && LocoPhaseOnWire(0.4f, 0.8f) == 128,
	      "a phase is the time over the length, in 256ths");
	Check(LocoPhaseOnWire(0.8f, 0.8f) == 0 && LocoPhaseOnWire(0.9f, 0.8f) == 32,
	      "past the length is the next stride, not the end of this one");
	Check(LocoPhaseOnWire(0.3f, 0.0f) == 0 && LocoPhaseOnWire(NAN, 1.0f) == 0,
	      "no length or no time is phase 0, not a division by zero");
	Check(LocoPhaseOnWire(0.999f, 1.0f) == 0, "the very top of a stride rounds to its start");
	Check(Near(LocoPhaseOffWire(LocoPhaseOnWire(0.6f, 1.0f)), 0.6f, 0.004f),
	      "and a phase comes back within a 256th");
}

void TestOneSnapshotsLegs() {
	std::printf("\none snapshot's legs\n");
	LegPose l = LegsOf(Legs(1, 0.3f, 0.7f, 0.0f, 0.0f, 0.0f, 0.25f));
	Check(l.valid && Near(l.weight[LOCO_WALK], 0.3f) && Near(l.weight[LOCO_RUN], 0.7f),
	      "a walk and a run sharing the weight are both played, as sent");
	Check(l.havePhase && Near(l.phase, 0.25f), "with the stride they are on");
	Check(!l.haveStartTime, "and no start-walk clock while the run is the strongest");

	PlayerStateBody idle = Legs(3, 0, 0, 0, 1.0f, 0, 0.5f);
	Check(!LegsOf(idle).havePhase, "standing, there is no stride to keep");

	PlayerStateBody start = Legs(4, 0, 0, 0, 0, 1.0f);
	start.animTime        = 0.2f;
	l                     = LegsOf(start);
	Check(l.valid && l.haveStartTime && Near(l.startTime, 0.2f),
	      "the start-walk brings its own time along");

	PlayerStateBody bare{};
	bare.animId = 1;
	l           = LegsOf(bare);
	Check(l.valid && l.weight[LOCO_RUN] == 1.0f && l.weight[LOCO_WALK] == 0.0f && !l.havePhase,
	      "no weights sent: the strongest id at full weight");

	PlayerStateBody sit = Legs(0x5A, 0, 0, 0, 0, 0);
	Check(!LegsOf(sit).valid, "anything but the five as the strongest is not the legs' to play");

	PlayerStateBody fast = Legs(1, 0, 1.0f, 0, 0, 0);
	fast.animSpeed       = 2.0f;
	Check(Near(LegsOf(fast).speed, 2.0f), "the adrenaline pill's double speed comes across");
	fast.animSpeed = 40.0f;
	Check(LegsOf(fast).speed == 1.0f, "a speed nobody plays is 1");
	fast.animSpeed = NAN;
	Check(LegsOf(fast).speed == 1.0f, "so is a NaN");
}

void TestBetweenTwoSnapshots() {
	std::printf("\nbetween two snapshots\n");
	const LegPose a = LegsOf(Legs(0, 1.0f, 0.0f, 0, 0, 0, 0.9f));
	const LegPose b = LegsOf(Legs(1, 0.0f, 1.0f, 0, 0, 0, 0.1f));
	const LegPose m = LerpLegs(a, b, 0.5f, 0.02f);
	Check(Near(m.weight[LOCO_WALK], 0.5f) && Near(m.weight[LOCO_RUN], 0.5f),
	      "a walk turning into a run is half of each halfway, not a 40 ms step");
	Check(Near(m.phase, 0.0f) || Near(m.phase, 1.0f), "and the stride goes forward over its top");
	Check(Near(PhaseGap(0.05f, 0.95f), 0.1f) && Near(PhaseGap(0.95f, 0.05f), -0.1f),
	      "the gap between two phases is the short way round");
	Check(Near(WrapPhase(-0.25f), 0.75f) && Near(WrapPhase(2.5f), 0.5f) && WrapPhase(NAN) == 0.0f,
	      "and a phase is always 0..1");

	LegPose other = LegsOf(Legs(0x20, 0, 0, 0, 0, 0));
	Check(LerpLegs(a, other, 0.9f, 0.0f).valid && !LerpLegs(other, a, 0.1f, 0.0f).valid,
	      "whether to drive them steps with the earlier snapshot, as the rest of the body does");

	PlayerStateBody s = Legs(4, 0, 0, 0, 0, 1.0f);
	s.animTime        = 0.1f;
	const LegPose st  = LerpLegs(LegsOf(s), LegsOf(s), 0.5f, 0.02f);
	Check(Near(st.startTime, 0.12f), "the start-walk's clock runs on from the snapshot");
}

void TestTheTimeline() {
	std::printf("\nthe legs at the pose's instant\n");
	BodyTimeline t;
	LegPose      l;
	Check(!t.LegsAt(1000, l), "nothing before the first snapshot");
	t.Push(1000, Legs(0, 1.0f, 0, 0, 0, 0, 0.2f), PlayerRideBody{});
	t.Push(1040, Legs(1, 0.2f, 0.8f, 0, 0, 0, 0.3f), PlayerRideBody{});
	Check(t.LegsAt(1020, l) && Near(l.weight[LOCO_WALK], 0.6f) && Near(l.weight[LOCO_RUN], 0.4f) &&
	          Near(l.phase, 0.25f),
	      "halfway between two snapshots, halfway between their legs and their strides");
	Check(t.LegsAt(900, l) && l.weight[LOCO_WALK] == 1.0f && l.havePhase,
	      "the oldest while the clock is behind them all");
	Check(t.LegsAt(1100, l) && Near(l.weight[LOCO_RUN], 0.8f) && !l.havePhase,
	      "past the newest the weights hold and the stride plays on by itself");

	PlayerStateBody s = Legs(4, 0, 0, 0, 0, 1.0f);
	s.animTime        = 0.3f;
	BodyTimeline st;
	st.Push(2000, s, PlayerRideBody{});
	Check(st.LegsAt(2050, l) && Near(l.startTime, 0.35f),
	      "a start-walk past the newest carries on playing");
}

void TestAStalledPlayerStands() {
	std::printf("\na stalled stream's legs\n");
	const PlayerStateBody still = FrozenBody(Legs(1, 0.4f, 0.6f, 0, 0, 0, 0.5f));
	const LegPose         l     = LegsOf(still);
	Check(l.valid && l.weight[LOCO_IDLE] == 1.0f && l.weight[LOCO_RUN] == 0.0f &&
	          l.weight[LOCO_WALK] == 0.0f && !l.havePhase,
	      "a run frozen by the stall is all idle");
	Check(LegsOf(FrozenBody(Legs(0x20, 0, 0, 0, 0, 0))).valid == false,
	      "and what was not the legs' stays not theirs");
}

void TestTheGroundDecidesAStandingPose() {
	std::printf("\nlegs that say standing on a pose that travels\n");
	const LegPose idle = LegsOf(Legs(3, 0, 0, 0, 1.0f, 0));
	LegPose       g    = LegsForGround(idle, 2.0f, 0.0f, false);
	Check(g.weight[LOCO_WALK] == 1.0f && g.weight[LOCO_IDLE] == 0.0f,
	      "two metres a second on an idle is a walk");
	g = LegsForGround(idle, 5.0f, 0.0f, false);
	Check(g.weight[LOCO_RUN] == 1.0f, "five is a run");
	Check(LegsForGround(idle, 1.0f, 0.0f, false).weight[LOCO_IDLE] == 1.0f,
	      "a drift below walking pace is left standing");
	Check(LegsForGround(idle, 8.0f, 0.0f, true).weight[LOCO_IDLE] == 1.0f,
	      "standing on a moving bus is standing");
	Check(LegsForGround(idle, 5.0f, -4.0f, false).weight[LOCO_IDLE] == 1.0f,
	      "falling is the fall's business");
	const LegPose run = LegsOf(Legs(1, 0, 1.0f, 0, 0, 0, 0.3f));
	g                 = LegsForGround(run, 0.0f, 0.0f, false);
	Check(g.weight[LOCO_RUN] == 1.0f && g.havePhase,
	      "running into a wall is still running: the sender's legs win when they move");
	Check(LegsForGround(run, 2.0f, 0.0f, false).havePhase, "and a moving sender's stride is kept");
}

void TestKeepingTheStride() {
	std::printf("\nkeeping the stride\n");
	StrideStep s = PlanStride(0.5f, 0.5f);
	Check(!s.seek && s.scale == 1.0f, "on the stride: as sent");
	s = PlanStride(0.55f, 0.5f);
	Check(!s.seek && Near(s.scale, 1.1f), "a twentieth behind: a tenth faster");
	s = PlanStride(0.45f, 0.5f);
	Check(!s.seek && Near(s.scale, 0.9f), "a twentieth ahead: a tenth slower");
	s = PlanStride(0.65f, 0.5f);
	Check(!s.seek && Near(s.scale, 1.0f + LEGS_SPEED_TRIM), "and never more than the trim");
	Check(PlanStride(0.8f, 0.5f).seek, "a stride out by more than a fifth is jumped to");
	Check(!PlanStride(0.02f, 0.97f).seek && PlanStride(0.02f, 0.97f).scale > 1.0f,
	      "just over the top of the stride is just behind, not most of a stride ahead");

	Check(Near(ApproachWeight(0.0f, 1.0f, 0.05f), 0.5f), "a weight moves ten a second");
	Check(ApproachWeight(0.95f, 1.0f, 0.05f) == 1.0f, "and stops on its target");
	Check(ApproachWeight(0.3f, 0.0f, 0.1f) == 0.0f, "down as well as up");
	Check(ApproachWeight(NAN, 1.0f, 0.02f) > 0.0f && ApproachWeight(NAN, 1.0f, 0.02f) <= 0.2f,
	      "a NaN on the clump starts again from nothing");
	Check(ApproachWeight(0.4f, 1.0f, -1.0f) == 0.4f, "no time, no change");
}

// Frame by frame: a sender whose stride runs at 1.3 a second, sampled at 25 Hz
// with timestamps a frame off either way, drawn 100 ms later on a 60 Hz screen
// whose legs start a third of a stride out. The trim has to bring them onto
// the sender's stride and keep them there without ever jumping again.
void TestTheStrideConverges() {
	std::printf("\nthe stride, frame by frame\n");
	BodyTimeline t;
	const float  rate   = 1.3f;
	uint32_t     sendMs = 1000;
	int          sent   = 0;
	// Everything sent up to 100 ms after the drawn instant has arrived; the
	// buffer only keeps its last 32, as it does in a game.
	auto arrive = [&](float drawnMs) {
		while (static_cast<float>(sendMs) <= drawnMs + 100.0f) {
			const int   jitter = (sent * 7 % 3 - 1) * 16;   // -16, 0, +16 ms
			const float phase  = (static_cast<float>(sendMs) + jitter) / 1000.0f * rate;
			t.Push(sendMs, Legs(1, 0, 1.0f, 0, 0, 0, WrapPhase(phase)), PlayerRideBody{});
			sendMs += 40;
			++sent;
		}
	};

	float local = 0.0f;
	int   seeks = 0, lateSeeks = 0;
	float worst = 0.0f, minScale = 2.0f, maxScale = 0.0f;
	bool  started = false;
	for (float ms = 1100.0f; ms < 4800.0f; ms += 1000.0f / 60.0f) {
		arrive(ms);
		LegPose l;
		t.LegsAt(static_cast<uint32_t>(ms), l);
		if (!started) {
			local   = WrapPhase(l.phase + 0.33f);
			started = true;
		}
		if (ms > 2100.0f)
			worst = std::fmax(worst, std::fabs(PhaseGap(l.phase, local)));
		const StrideStep step  = PlanStride(l.phase, local);
		float            scale = 1.0f;
		if (step.seek) {
			local = l.phase;
			++seeks;
			if (ms > 1150.0f)
				++lateSeeks;
		} else {
			scale = step.scale;
		}
		minScale = std::fmin(minScale, scale);
		maxScale = std::fmax(maxScale, scale);
		local    = WrapPhase(local + rate * scale / 60.0f);
	}
	Check(seeks == 1 && lateSeeks == 0, "a third of a stride out is one jump, at the start");
	Check(worst < 0.03f, "after a second the legs stay within 3% of the sender's stride");
	Check(minScale >= 1.0f - LEGS_SPEED_TRIM && maxScale <= 1.0f + LEGS_SPEED_TRIM,
	      "and never play faster or slower than the trim allows");
	Check(maxScale < 1.08f && minScale > 0.92f,
	      "a timestamp a frame off moves the stride by a few percent, not a stutter");
}

void TestTheStartWalkComesFromTheStyle() {
	std::printf("\nthe start-walk\n");
	constexpr int PLAYERLEFT = 20, STD_COUNT = 173, STRAFE_COUNT = 5, GANG = 8, GANG_COUNT = 4;
	const AnimPlan side = PlanAnim(ANIM_STD_STARTWALK, PLAYERLEFT, STRAFE_COUNT, STD_COUNT);
	Check(side.valid && side.group == PLAYERLEFT,
	      "a strafe starts with its own side-step, not ASSOCGRP_STD's forward one");
	const AnimPlan gang = PlanAnim(ANIM_STD_STARTWALK, GANG, GANG_COUNT, STD_COUNT);
	Check(gang.valid && gang.group == ASSOCGRP_STD,
	      "a style with four falls back rather than reading past its end");
	Check(!PlanAnim(ANIM_STD_STARTWALK, PLAYERLEFT, STRAFE_COUNT, 0).valid,
	      "nothing before the anim files load");
	Check(LOCO_ANIM_IDS[LOCO_STARTWALK] == ANIM_STD_STARTWALK && LOCO_ANIM_IDS[LOCO_IDLE] == ANIM_STD_IDLE &&
	          LOCO_ANIM_IDS[LOCO_SPRINT] == ANIM_STD_RUNFAST,
	      "the wire's five are the engine's ids");
	Check(IsStrideAnim(ANIM_STD_RUNFAST) && !IsStrideAnim(ANIM_STD_IDLE) &&
	          !IsStrideAnim(ANIM_STD_STARTWALK),
	      "walk, run and sprint share the stride; idle and start-walk do not");
}

// ---- the image ------------------------------------------------------------------

// A diagonal walk: the owner faces the camera and walks at an angle to it, and
// their engine turns the upper legs toward the walk (LegTwistFor).
void TestTheDiagonalLegs() {
	std::printf("\nthe legs turn toward a diagonal walk\n");
	constexpr float DEG = 0.0174532925f;
	Check(Near(LegTwistFor(0.0f, 0.0f, 5.0f, true), 0.0f), "straight ahead: no twist");
	Check(Near(LegTwistFor(0.0f, 3.5f, 3.5f, true), -45.0f * DEG),
	      "forward and to the right: the legs turn 45 degrees toward it");
	Check(Near(LegTwistFor(0.0f, -3.5f, 3.5f, true), 45.0f * DEG), "and to the left the other way");
	Check(LegTwistFor(0.0f, 5.0f, 0.0f, true) == 0.0f,
	      "straight sideways is a strafe, which the walking style does and not the legs");
	Check(LegTwistFor(0.0f, 2.9f, 1.7f, true) == 0.0f, "so is anything past 50 degrees");
	Check(Near(LegTwistFor(0.0f, 0.0f, -5.0f, true), 0.0f), "walking backward: none");
	Check(Near(LegTwistFor(0.0f, 3.5f, -3.5f, true), 45.0f * DEG),
	      "back and to the right folds through half a turn, as the engine folds it");
	const float h = 3.0f;
	Check(Near(LegTwistFor(h, -std::sin(h + 0.5f) * 4.0f, std::cos(h + 0.5f) * 4.0f, true), 0.5f),
	      "measured from the facing, across the seam at pi");
	Check(LegTwistFor(0.0f, 0.2f, 0.2f, true) == 0.0f, "too slow to say where it is going");
	Check(LegTwistFor(0.0f, 3.5f, 3.5f, false) == 0.0f,
	      "and nothing in a state the engine would not do it in");
	Check(LegTwistState(1) && LegTwistState(0) && LegTwistState(22) && LegTwistState(35) &&
	          !LegTwistState(36) && !LegTwistState(44) && !LegTwistState(48),
	      "CanStrafeOrMouseControl's states: idle, aiming, jumping yes; fallen, driving, dying no");

	Check(Near(ApproachLegTwist(0.0f, 0.8f, 0.05f), 0.4f) &&
	          ApproachLegTwist(0.7f, 0.8f, 0.05f) == 0.8f &&
	          Near(ApproachLegTwist(0.8f, 0.0f, 0.05f), 0.4f),
	      "eased at 8 radians a second either way");

	LegTwistTable table;
	int a = 0, b = 0;
	table.Set(&a, 0.3f, 10);
	float yaw = 0.0f;
	Check(table.Find(&a, 10, yaw) && Near(yaw, 0.3f), "written for this frame, found this frame");
	Check(!table.Find(&a, 11, yaw), "and not the next one");
	Check(!table.Find(&b, 10, yaw), "and only for its own ped");
	int peds[MAX_PLAYERS] = {};
	for (int &p : peds)
		table.Set(&p, 0.1f, 11);
	Check(table.Find(&peds[MAX_PLAYERS - 1], 11, yaw), "a stale entry makes room for a new frame");
}

bool LoadExe(std::vector<uint8_t> &image, std::string &from) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_GTA3_EXE"))
		candidates.push_back(env);
	candidates.push_back("reference/bin/gta3.exe");
	candidates.push_back("../../../../reference/bin/gta3.exe");
	for (const std::string &path : candidates) {
		FILE *fh = std::fopen(path.c_str(), "rb");
		if (!fh)
			continue;
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		image.resize(size > 0 ? size_t(size) : 0);
		const size_t got = image.empty() ? 0 : std::fread(image.data(), 1, image.size(), fh);
		std::fclose(fh);
		if (got == image.size() && image.size() == IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

bool BytesAt(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> bytes) {
	size_t i = 0;
	for (const uint8_t b : bytes)
		if (img[va - IMAGE_BASE + i++] != b)
			return false;
	return true;
}

int32_t DescFlags(const std::vector<uint8_t> &img, uint16_t id) {
	return static_cast<int32_t>(Dword(img, CAnimManager__aStdAnimDescs + 8u * id + 4));
}

bool NameIs(const std::vector<uint8_t> &img, uint16_t id, const char *name) {
	const uint32_t p = Dword(img, CAnimManager__aStdAnimNames + 4u * id);
	if (p < IMAGE_BASE || p >= IMAGE_BASE + IMAGE_SIZE)
		return false;
	return std::strcmp(reinterpret_cast<const char *>(&img[p - IMAGE_BASE]), name) == 0;
}

void TestAgainstTheImage() {
	std::printf("\nthe legs against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "legs against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	static_assert(ANIMHIER_TOTAL_LENGTH == 0x20 && ANIM_HIERARCHY == 0x14 && ANIM_SPEED == 0x24,
	              "the offsets the bytes below spell");
	Check(BytesAt(img, 0x00403214, {0x8B, 0x43, 0x14, 0xD8, 0x48, 0x20}),
	      "UpdateTime scales a movement anim's relSpeed by hierarchy +20h, totalLength");
	Check(BytesAt(img, 0x00403205, {0x83, 0xE0, 0x20, 0x75, 0x06, 0xD9, 0x43, 0x24}),
	      "and takes its own speed only when ASSOC_MOVEMENT is clear");
	Check(BytesAt(img, 0x00402536, {0x8B, 0x45, 0x14, 0xD9, 0x40, 0x20, 0xD8, 0x75, 0x24, 0xD9,
	                                0x45, 0x18}),
	      "while the clump's walk sums totalLength / speed * blendAmount over them, so speed "
	      "sets the shared rate");
	Check(BytesAt(img, 0x0040252B, {0x8B, 0x45, 0x30, 0x46, 0x83, 0xE0, 0x20}),
	      "for exactly the ASSOC_MOVEMENT ones");

	constexpr int32_t STRIDE = ASSOC_REPEAT | ASSOC_MOVEMENT;
	Check((DescFlags(img, ANIM_STD_WALK) & STRIDE) == STRIDE &&
	          (DescFlags(img, ANIM_STD_RUN) & STRIDE) == STRIDE &&
	          (DescFlags(img, ANIM_STD_RUNFAST) & STRIDE) == STRIDE,
	      "walk, run and sprint loop and are movement animations");
	Check((DescFlags(img, ANIM_STD_IDLE) & (ASSOC_REPEAT | ASSOC_MOVEMENT | ASSOC_PARTIAL)) ==
	          ASSOC_REPEAT,
	      "the idle loops on its own clock");
	Check((DescFlags(img, ANIM_STD_STARTWALK) &
	       (ASSOC_REPEAT | ASSOC_MOVEMENT | ASSOC_PARTIAL | ASSOC_FADEOUTWHENDONE)) == 0,
	      "the start-walk plays once, is not a movement animation and does not fade itself");
	Check(NameIs(img, ANIM_STD_WALK, "walk_civi") && NameIs(img, ANIM_STD_RUNFAST, "sprint_panic") &&
	          NameIs(img, ANIM_STD_IDLE, "idle_stance") && NameIs(img, ANIM_STD_STARTWALK, "walk_start"),
	      "and they are the five by name");

	// The diagonal walk's legs (LegTwistFor).
	auto floatAt = [&](uint32_t va) {
		const uint32_t bits = Dword(img, va);
		float f;
		std::memcpy(&f, &bits, sizeof f);
		return f;
	};
	Check(Near(floatAt(0x005F84AC), LEG_TWIST_MAX_RAD, 1e-6f) &&
	          Near(floatAt(0x005F84A8), -LEG_TWIST_MAX_RAD, 1e-6f) &&
	          Near(floatAt(0x005F84B4), LEG_TWIST_FLIP_RAD, 1e-6f) &&
	          Near(floatAt(0x005F84B8), -LEG_TWIST_FLIP_RAD, 1e-6f) &&
	          Near(floatAt(0x005F84B0), LEG_TWIST_IDLE_MAX_BLEND, 1e-6f),
	      "the 50 and 100 degree bounds and the idle's 0.5 are the retail ones");
	Check(BytesAt(img, CPed__ProcessControl_NewVelocity, {0xE8}) &&
	          CPed__ProcessControl_NewVelocity + 5 + Dword(img, CPed__ProcessControl_NewVelocity + 1) ==
	              CPed__CalculateNewVelocity,
	      "ProcessControl calls CalculateNewVelocity at the site we redirect");
	int callers = 0;
	for (uint32_t va = IMAGE_BASE + 0x1000; va + 5 < IMAGE_BASE + IMAGE_SIZE; ++va)
		if (img[va - IMAGE_BASE] == 0xE8 && va + 5 + Dword(img, va + 1) == CPed__CalculateNewVelocity)
			++callers;
	Check(callers == 1, "and nothing else in the image calls it");
	Check(BytesAt(img, 0x004C7897, {0x8B, 0x83, 0xC0, 0x01, 0, 0}) &&
	          BytesAt(img, 0x004C78AC, {0x8B, 0x83, 0xC4, 0x01, 0, 0}) &&
	          offs::PED_FRAMES + PED_NODE_UPPERLEGL * 4 == 0x1C0 &&
	          offs::PED_FRAMES + PED_NODE_UPPERLEGR * 4 == 0x1C4,
	      "its tail turns m_pFrames[7] and [8]");
	Check(BytesAt(img, 0x004C78A1, {0x8D, 0x8B, 0xF0, 0x01, 0, 0}) && offs::PED_IK == 0x1F0 &&
	          0x004C78A7 + 5 + Dword(img, 0x004C78A8) == CPedIK__RotateTorso &&
	          0x004C78C2 + 5 + Dword(img, 0x004C78C3) == CPedIK__RotateTorso &&
	          BytesAt(img, 0x004EE2B3, {0xC2, 0x0C, 0x00}),
	      "with CPedIK::RotateTorso on the IK at +1F0h, three arguments");
	Check(BytesAt(img, 0x004C77E0, {0x6A, ANIM_STD_FIGHT_IDLE_ID}) &&
	          BytesAt(img, 0x004C77CF, {0x6A, 0x03}),
	      "only while the idle is mostly off and there is no fight stance");
	Check(BytesAt(img, 0x004CE7D6, {0x83, 0xFA, 0x01}) && BytesAt(img, 0x004CE7E7, {0x83, 0xFA, 0x16}) &&
	          BytesAt(img, 0x004CE7F4, {0x83, 0xFA, 0x23}),
	      "and in CanStrafeOrMouseControl's states");
}

} // namespace

int RunLegsTests() {
	std::printf("\n---- a remote player's legs (remoteloco.h) ----\n");
	TestTheWire();
	TestOneSnapshotsLegs();
	TestBetweenTwoSnapshots();
	TestTheTimeline();
	TestAStalledPlayerStands();
	TestTheGroundDecidesAStandingPose();
	TestKeepingTheStride();
	TestTheStrideConverges();
	TestTheStartWalkComesFromTheStyle();
	TestTheDiagonalLegs();
	TestAgainstTheImage();
	return g_legFailures;
}
