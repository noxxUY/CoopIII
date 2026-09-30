// A remote player's animations after our own engine has had a go at the ped:
// client/src/game/animrevive.h.
//
// The engine half is a few lines of arithmetic, so it is transcribed here from
// the image (UpdateBlend 0x004032B0, UpdateTime 0x004031F0, the wipe
// 0x00405520, BlendAnimation's found arm 0x00403887) and the rule is run
// against it frame by frame. The image checks at the end pin the bytes those
// transcriptions came from, when a retail gta3.exe is at hand.

#include "game/animrevive.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_reviveFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_reviveFailures;
}

// ---- the engine, transcribed ----------------------------------------------

struct Assoc {
	uint16_t id     = 0;
	float    amount = 1.0f;
	float    delta  = 0.0f;
	int32_t  flags  = 0;
	float    time   = 0.0f;
	float    length = 1.0f;
	bool     gone   = false;
	bool     landCB = false;   // PedLandCB is its finish callback
};

constexpr int32_t WEAPON_FLAGS = ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL;   // 31h..3Ah, 18h
constexpr int32_t LAND_FLAGS   = ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | 0x40;   // 99h, 58h
constexpr int32_t GLIDE_FLAGS  = ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL;    // 98h, 14h

// 0x004032B0.
void UpdateBlend(Assoc &a, float dt) {
	a.amount += a.delta * dt;
	if (!(a.amount > 0.0f) && a.delta < 0.0f) {
		a.amount = 0.0f;
		a.delta  = 0.0f;
		if (a.flags & ASSOC_DELETEFADEDOUT)
			a.gone = true;
		return;
	}
	if (a.amount > 1.0f) {
		a.amount = 1.0f;
		if (a.delta > 0.0f)
			a.delta = 0.0f;
	}
}

// 0x004031F0, the part past the end of a non-repeating animation. Returns
// whether the finish callback ran.
bool UpdateTime(Assoc &a, float dt) {
	if (!(a.flags & ASSOC_RUNNING))
		return false;
	a.time += dt;
	if (a.time < a.length)
		return false;
	a.time = a.length;
	a.flags &= ~ASSOC_RUNNING;
	if (a.flags & ASSOC_FADEOUTWHENDONE) {
		a.flags |= ASSOC_DELETEFADEDOUT;
		a.delta = -4.0f;
	}
	return a.landCB;
}

// 0x00405520 with the mask SetLanding and PedGetupCB pass.
void WipePartials(std::vector<Assoc> &clump) {
	for (Assoc &a : clump)
		if (!a.gone && (a.flags & ASSOC_PARTIAL))
			a.delta = LANDING_PARTIAL_DELTA;
}

// BlendAnimation (0x00403710) for a partial: every other partial condemned,
// the found one given (1 - amount) * delta, a new one added at the front at
// weight 0 when there is none.
void BlendPartial(std::vector<Assoc> &clump, uint16_t id, float delta, int32_t flags) {
	Assoc *found = nullptr;
	for (Assoc &a : clump) {
		if (a.gone || !(a.flags & ASSOC_PARTIAL))
			continue;
		if (a.id == id) {
			found = &a;
			continue;
		}
		if (a.amount > 0.0f) {
			const float d = -delta * a.amount;
			if (d < a.delta)
				a.delta = d;
		} else {
			a.delta = -1.0f;
		}
		a.flags |= ASSOC_DELETEFADEDOUT;
	}
	if (found) {
		found->delta = (1.0f - found->amount) * delta;
		return;
	}
	Assoc fresh;
	fresh.id     = id;
	fresh.amount = 0.0f;
	fresh.delta  = delta;
	fresh.flags  = flags | ASSOC_RUNNING;
	clump.insert(clump.begin(), fresh);
}

struct Replica {
	std::vector<Assoc> clump;
	uint8_t            flagsB = 0;
	uint32_t           state  = 1;   // PED_IDLE
};

// ped.cpp's ApplyOverlay, reduced to the decision and its one side effect.
OverlayStep Drive(Replica &r, uint16_t want, bool ownerRunning, int &blends) {
	bool     present = false;
	AnimLife life    = AnimLife::SPENT;
	bool     move    = false;
	for (const Assoc &a : r.clump) {
		if (a.gone)
			continue;
		if (a.id == want) {
			const AnimLife l = ClassifyAnim(a.amount, a.delta, a.flags);
			if (!present || Livelier(l, life))
				life = l;
			present = true;
		} else if (EngineMoveHoldsOff(want, a.id, a.amount, a.delta, a.flags)) {
			move = true;
		}
	}
	const bool hold = EngineMoveHolds(PedDownOrGettingUp(r.state),
	                                  PedAirborneOrLanding(r.flagsB, r.state), move);
	const OverlayStep step = PlanOverlay(present, life, ownerRunning, hold);
	if (step == OverlayStep::APPLY) {
		BlendPartial(r.clump, want, 8.0f, WEAPON_FLAGS);
		++blends;
	}
	return step;
}

// One engine frame on the replica, after CoopIII's PreFrame: time, then blend,
// the order RpAnimBlendClumpUpdateAnimations calls them in.
void EngineFrame(Replica &r, float dt) {
	for (Assoc &a : r.clump) {
		if (a.gone)
			continue;
		if (UpdateTime(a, dt)) {   // PedLandCB, 0x004CE8A0
			a.delta = LANDING_PARTIAL_DELTA;
			r.flagsB &= static_cast<uint8_t>(~offs::PED_IS_LANDING);
		}
		UpdateBlend(a, dt);
	}
}

// SetLanding, 0x004D0E40, with no FALL_fall on the clump.
void Land(Replica &r) {
	WipePartials(r.clump);
	Assoc land;
	land.id     = ANIM_STD_FALL_LAND;
	land.flags  = LAND_FLAGS | ASSOC_RUNNING;
	land.length = 0.5f;
	land.landCB = true;
	r.clump.insert(r.clump.begin(), land);
	r.flagsB = static_cast<uint8_t>((r.flagsB & ~offs::PED_IS_IN_THE_AIR) | offs::PED_IS_LANDING);
}

const Assoc *Find(const Replica &r, uint16_t id) {
	for (const Assoc &a : r.clump)
		if (!a.gone && a.id == id)
			return &a;
	return nullptr;
}

constexpr uint16_t AIM   = 0x31;   // WEAPON_hgun_body
constexpr float    FRAME = 1.0f / 60.0f;

// ---- the tests ------------------------------------------------------------

void TestTheThreeLives() {
	std::printf("what an association's weight says about it\n");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(ClassifyAnim(1.0f, 0.0f, WEAPON_FLAGS) == AnimLife::LIVE, "full weight, still");
	Check(ClassifyAnim(0.0f, 8.0f, WEAPON_FLAGS) == AnimLife::LIVE,
	      "weight 0 fading in, as BlendAnimation adds one");
	Check(ClassifyAnim(0.5f, -4.0f, WEAPON_FLAGS | ASSOC_DELETEFADEDOUT) == AnimLife::CONDEMNED,
	      "fading with the delete flag is condemned");
	Check(ClassifyAnim(1.0f, LANDING_PARTIAL_DELTA, WEAPON_FLAGS) == AnimLife::SPENT,
	      "the landing's -1000 with no delete flag is spent before UpdateBlend even runs");
	Check(ClassifyAnim(0.0f, 0.0f, WEAPON_FLAGS) == AnimLife::SPENT,
	      "weight 0, delta 0: what UpdateBlend leaves");
	Check(ClassifyAnim(nan, 0.0f, WEAPON_FLAGS) == AnimLife::SPENT &&
	          ClassifyAnim(0.0f, nan, WEAPON_FLAGS) == AnimLife::SPENT,
	      "NaN anywhere is spent, so it gets one fresh blend");
	Check(Livelier(AnimLife::LIVE, AnimLife::CONDEMNED) &&
	          Livelier(AnimLife::CONDEMNED, AnimLife::SPENT) &&
	          !Livelier(AnimLife::SPENT, AnimLife::LIVE),
	      "live beats condemned beats spent");
}

void TestWhatTheLandingLeaves() {
	std::printf("the landing's wipe, run through UpdateBlend\n");
	std::vector<Assoc> clump(2);
	clump[0].id    = AIM;
	clump[0].flags = WEAPON_FLAGS;   // an aim: held, not running
	clump[1].id    = 0x77;
	clump[1].flags = GLIDE_FLAGS;    // Driveby_L's flags, 14h
	WipePartials(clump);
	for (int i = 0; i < 600; ++i)
		for (Assoc &a : clump)
			if (!a.gone)
				UpdateBlend(a, FRAME);
	Check(!clump[0].gone && clump[0].amount == 0.0f && clump[0].delta == 0.0f,
	      "a weapon anim is still on the clump ten seconds later, at weight 0");
	Check(ClassifyAnim(clump[0].amount, clump[0].delta, clump[0].flags) == AnimLife::SPENT,
	      "and it reads as spent");
	Check(clump[1].gone, "a drive-by anim, which carries the delete flag, is gone");
}

void TestThePlan() {
	std::printf("the overlay plan\n");
	using S = OverlayStep;
	Check(PlanOverlay(true, AnimLife::LIVE, false, false) == S::KEEP, "live: keep");
	Check(PlanOverlay(true, AnimLife::LIVE, true, true) == S::KEEP,
	      "live stays live whatever the engine is doing");
	Check(PlanOverlay(false, AnimLife::SPENT, true, false) == S::APPLY, "missing: apply");
	Check(PlanOverlay(true, AnimLife::SPENT, false, false) == S::APPLY,
	      "spent and held still: apply - the aim after a landing");
	Check(PlanOverlay(true, AnimLife::SPENT, true, false) == S::APPLY, "spent and running: apply");
	Check(PlanOverlay(true, AnimLife::CONDEMNED, true, false) == S::APPLY,
	      "condemned while the owner still runs it: apply, the firing loop");
	Check(PlanOverlay(true, AnimLife::CONDEMNED, false, false) == S::LET_END,
	      "condemned and the owner stopped: let it end, the rocket");
	Check(PlanOverlay(true, AnimLife::CONDEMNED, false, true) == S::LET_END,
	      "and that holds while the engine is mid-move too");
	Check(PlanOverlay(false, AnimLife::SPENT, true, true) == S::HOLD &&
	          PlanOverlay(true, AnimLife::SPENT, false, true) == S::HOLD &&
	          PlanOverlay(true, AnimLife::CONDEMNED, true, true) == S::HOLD,
	      "everything that would apply waits while the engine's own move plays");
}

void TestTheFamily() {
	std::printf("the engine's own moves\n");
	Check(!IsEngineMoveAnim(0x8F) && IsEngineMoveAnim(ANIM_STD_GET_UP) &&
	          IsEngineMoveAnim(ANIM_STD_FALL_COLLAPSE) && !IsEngineMoveAnim(0x9B),
	      "90h Getup through 9Ah FALL_collapse, not VAN_getout or EV_step");
	Check(!IsEngineMoveAnim(ANIM_NONE) && !IsEngineMoveAnim(AIM), "not a weapon anim, not none");

	const int32_t playing = LAND_FLAGS | ASSOC_RUNNING;
	Check(EngineMoveHoldsOff(AIM, ANIM_STD_FALL_LAND, 1.0f, 0.0f, playing),
	      "a landing that is playing holds an aim off");
	Check(!EngineMoveHoldsOff(AIM, ANIM_STD_FALL_GLIDE, 1.0f, 0.0f, GLIDE_FLAGS),
	      "a glide parked on its last frame does not");
	Check(!EngineMoveHoldsOff(AIM, ANIM_STD_FALL_LAND, 0.8f, -4.0f,
	                          playing | ASSOC_DELETEFADEDOUT),
	      "a landing that finished and is fading does not");
	Check(!EngineMoveHoldsOff(AIM, ANIM_STD_FALL_LAND, 0.0f, 0.0f, playing),
	      "a spent one does not");
	Check(!EngineMoveHoldsOff(AIM, 0x0D, 1.0f, 0.0f, 0x850 | ASSOC_RUNNING),
	      "a knockdown is not in the family; the state holds for that");
	Check(!EngineMoveHoldsOff(ANIM_STD_JUMP_LAND, ANIM_STD_FALL_GLIDE, 1.0f, 0.0f,
	                          GLIDE_FLAGS | ASSOC_RUNNING),
	      "the owner's own landing on the wire is never held off by ours");

	Check(PedAirborneOrLanding(offs::PED_IS_IN_THE_AIR, 1) &&
	          PedAirborneOrLanding(offs::PED_IS_LANDING, 1) &&
	          PedAirborneOrLanding(0, PEDSTATE_JUMP) && !PedAirborneOrLanding(0x01 | 0x04, 1),
	      "bits 3 and 4 of byte B and PED_JUMP, not bIsRestoringGun or its neighbours");
	Check(PedDownOrGettingUp(PEDSTATE_FALL) && PedDownOrGettingUp(PEDSTATE_GETUP) &&
	          !PedDownOrGettingUp(PEDSTATE_JUMP) && !PedDownOrGettingUp(1),
	      "PED_FALL and PED_GETUP are down");
	Check(EngineMoveHolds(true, false, false), "down holds on its own");
	Check(EngineMoveHolds(false, true, true), "airborne holds with a move playing");
	Check(!EngineMoveHolds(false, true, false),
	      "an airborne bit with nothing playing holds nothing");
	Check(!EngineMoveHolds(false, false, true),
	      "and a family anim we applied ourselves holds nothing without the engine's bits");
}

void TestALandingFrameByFrame() {
	std::printf("an aim through a landing, frame by frame\n");
	Replica r;
	Assoc   aim;
	aim.id    = AIM;
	aim.flags = WEAPON_FLAGS;
	r.clump.push_back(aim);
	int blends = 0;

	Check(Drive(r, AIM, false, blends) == OverlayStep::KEEP && blends == 0,
	      "before: the aim is live and left alone");

	Land(r);
	Check(Drive(r, AIM, false, blends) == OverlayStep::HOLD,
	      "the frame the landing starts, the aim waits");
	int held = 0;
	for (int i = 0; i < 120 && Drive(r, AIM, false, blends) == OverlayStep::HOLD; ++i) {
		EngineFrame(r, FRAME);
		++held;
	}
	Check(held > 20 && held < 40, "for the half second the landing plays, and no longer");
	Check(!(r.flagsB & offs::PED_IS_LANDING), "PedLandCB has cleared bIsLanding");
	Check(blends == 1, "then it is blended once");

	const Assoc *back = Find(r, AIM);
	Check(back && back->delta > 0.0f, "the same association, fading back in");
	for (int i = 0; i < 60; ++i) {
		Drive(r, AIM, false, blends);
		EngineFrame(r, FRAME);
	}
	back = Find(r, AIM);
	Check(back && back->amount == 1.0f, "and at full weight a second later");
	Check(blends == 1, "without being blended again on any frame after that");
	Check(!Find(r, ANIM_STD_FALL_LAND), "the landing ran its whole course and went");
}

void TestTheOldRuleNeverCameBack() {
	std::printf("the same landing under the rule this replaced\n");
	Replica r;
	Assoc   aim;
	aim.id    = AIM;
	aim.flags = WEAPON_FLAGS;
	r.clump.push_back(aim);
	Land(r);
	int blends = 0;
	for (int i = 0; i < 600; ++i) {
		// found by id and not condemned: left alone
		const Assoc *a = Find(r, AIM);
		const bool condemned = a && (a->flags & ASSOC_DELETEFADEDOUT) && a->delta < 0.0f;
		if (!a || condemned) {
			BlendPartial(r.clump, AIM, 8.0f, WEAPON_FLAGS);
			++blends;
		}
		EngineFrame(r, FRAME);
	}
	const Assoc *a = Find(r, AIM);
	Check(a && a->amount == 0.0f && blends == 0,
	      "ten seconds on it is still found, still weighs nothing, never blended: the bug");
}

void TestAGetUp() {
	std::printf("an aim through our engine's own knockdown and get-up\n");
	Replica r;
	Assoc   aim;
	aim.id    = AIM;
	aim.flags = WEAPON_FLAGS;
	r.clump.push_back(aim);
	int blends = 0;

	r.state = PEDSTATE_FALL;
	Check(Drive(r, AIM, false, blends) == OverlayStep::KEEP,
	      "knocked down with the aim still live: nothing to do");
	BlendPartial(r.clump, 0x0D, 8.0f, 0x850);   // SetFall's KO condemns it
	for (int i = 0; i < 30; ++i)
		EngineFrame(r, FRAME);
	Check(!Find(r, AIM), "SetFall's blend condemned the aim and it went");
	Check(Drive(r, AIM, false, blends) == OverlayStep::HOLD && blends == 0,
	      "and while the ped lies there it waits, KO parked or not");
	r.state = PEDSTATE_GETUP;
	Check(Drive(r, AIM, false, blends) == OverlayStep::HOLD, "through the get-up too");
	WipePartials(r.clump);   // PedGetupCB
	r.state = 1;             // RestorePreviousState
	Check(Drive(r, AIM, false, blends) == OverlayStep::APPLY && blends == 1,
	      "and goes back on the frame the ped is up");
}

void TestTheBase() {
	std::printf("the base animation\n");
	Check(BaseNeedsBlend(1, ANIM_NONE, false), "a new id is blended");
	Check(BaseNeedsBlend(1, 0, true), "a changed id is blended");
	Check(!BaseNeedsBlend(1, 1, true), "the same id, live: left alone");
	Check(BaseNeedsBlend(1, 1, false),
	      "the same id, taken off by our engine (SetMoveAnim after a get-up): blended again");
	Check(!BaseNeedsBlend(ANIM_NONE, 1, false), "none on the wire blends nothing");
}

// ---- the bytes the transcriptions came from --------------------------------

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

uint32_t CallTarget(const std::vector<uint8_t> &img, uint32_t site) {
	return site + 5 + Dword(img, site + 1);
}

bool BytesAt(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> bytes) {
	size_t i = 0;
	for (const uint8_t b : bytes)
		if (img[va - IMAGE_BASE + i++] != b)
			return false;
	return true;
}

// The wipe's argument run: push [5F84E0h] / push 10h / push eax / call.
bool WipeCallAt(const std::vector<uint8_t> &img, uint32_t call) {
	return BytesAt(img, call - 9, {0xFF, 0x35, 0xE0, 0x84, 0x5F, 0x00, 0x6A, 0x10, 0x50, 0xE8}) &&
	       CallTarget(img, call) == RpAnimBlendClumpSetBlendDeltas;
}

int32_t DescFlags(const std::vector<uint8_t> &img, uint16_t id) {
	return static_cast<int32_t>(Dword(img, CAnimManager__aStdAnimDescs + 8u * id + 4));
}

bool DescId(const std::vector<uint8_t> &img, uint16_t id) {
	return Dword(img, CAnimManager__aStdAnimDescs + 8u * id) == id;
}

bool NameIs(const std::vector<uint8_t> &img, uint16_t id, const char *name) {
	const uint32_t p = Dword(img, CAnimManager__aStdAnimNames + 4u * id);
	if (p < IMAGE_BASE || p >= IMAGE_BASE + IMAGE_SIZE)
		return false;
	return std::strcmp(reinterpret_cast<const char *>(&img[p - IMAGE_BASE]), name) == 0;
}

void TestAgainstTheImage() {
	std::printf("\nthe wipe against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "wipe against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(Dword(img, 0x005F84E0) == 0xC47A0000u, "5F84E0h is -1000.0f");
	Check(BytesAt(img, 0x00405549, {0x8B, 0x43, 0x30, 0x21, 0xD0, 0x74, 0x03, 0xD9, 0x53, 0x1C}),
	      "SetBlendDeltas tests flags & mask and writes +1Ch, nothing else");

	int callers = 0;
	for (uint32_t va = IMAGE_BASE + 0x1000; va + 5 < IMAGE_BASE + IMAGE_SIZE; ++va)
		if (img[va - IMAGE_BASE] == 0xE8 && CallTarget(img, va) == RpAnimBlendClumpSetBlendDeltas)
			++callers;
	Check(callers == 3, "three callers in the whole image");
	Check(WipeCallAt(img, 0x004D0E75), "SetLanding wipes every partial with -1000");
	Check(WipeCallAt(img, 0x004CE82F), "so does PedGetupCB");
	Check(WipeCallAt(img, 0x004C831B), "and ProcessBuoyancy's dry-land arm");
	Check(BytesAt(img, 0x004CE815, {0x83, 0xBB, 0x24, 0x02, 0x00, 0x00, 0x25}),
	      "PedGetupCB's wipe is behind m_nPedState == 25h, PED_GETUP");

	Check(BytesAt(img, 0x004032EC, {0xC7, 0x45, 0x18, 0, 0, 0, 0}) &&
	          BytesAt(img, 0x00403306, {0xC7, 0x45, 0x1C, 0, 0, 0, 0}) &&
	          BytesAt(img, 0x0040330D, {0x8B, 0x45, 0x30, 0x83, 0xE0, 0x04}),
	      "UpdateBlend zeroes weight and delta, then deletes only on flag 4");
	Check(BytesAt(img, 0x00403263, {0x74, 0x0B, 0x83, 0x4B, 0x30, 0x04, 0xC7, 0x43, 0x1C,
	                                0x00, 0x00, 0x80, 0xC0}),
	      "UpdateTime condemns a finished FADEOUTWHENDONE with -4");
	Check(BytesAt(img, 0x00403887, {0xD9, 0x05, 0x84, 0xA0, 0x5E, 0x00, 0xD8, 0x63, 0x18,
	                                0xD8, 0x4C, 0x24, 0x38, 0xD9, 0x5B, 0x1C}) &&
	          Dword(img, 0x005EA084) == 0x3F800000u,
	      "BlendAnimation revives a found one with (1 - amount) * delta");
	Check(BytesAt(img, 0x004037C9, {0x3B, 0x41, 0x2C, 0x75, 0x04, 0x89, 0xCB, 0xEB}),
	      "and keeps walking after a match, so it revives the last one");

	Check(CallTarget(img, 0x004D0E2D) == CPed__SetLanding &&
	          CallTarget(img, 0x004CA573) == CPed__SetLanding,
	      "InTheAir and ProcessControl call SetLanding");
	Check(BytesAt(img, 0x004D0EF0, {0x68}) && Dword(img, 0x004D0EF1) == CPed__PedLandCB,
	      "SetLanding hangs PedLandCB on its anim");
	Check(BytesAt(img, 0x004D0F00, {0x24, 0xF7}) && BytesAt(img, 0x004D0F0E, {0x24, 0xEF, 0x0C, 0x10}) &&
	          (0xF7 & offs::PED_IS_IN_THE_AIR) == 0 && offs::PED_IS_LANDING == 0x10,
	      "and clears bIsInTheAir (8) and sets bIsLanding (10h) in byte B");
	Check(BytesAt(img, 0x004D0CB6, {0x24, 0xF7, 0x0C, 0x08, 0x88, 0x83, 0x55, 0x01, 0, 0}),
	      "SetInTheAir sets bit 3 of +155h");
	Check(BytesAt(img, 0x004CE8AF, {0x8A, 0x81, 0x55, 0x01, 0, 0, 0x24, 0xEF}),
	      "PedLandCB clears bit 4 of +155h");
	Check(BytesAt(img, 0x004D118C, {0x68, 0x93, 0, 0, 0}) &&
	          BytesAt(img, 0x004D11A9, {0x68, 0x90, 0, 0, 0}) &&
	          BytesAt(img, 0x004D11BC, {0x68}) && Dword(img, 0x004D11BD) == CPed__PedGetupCB,
	      "SetGetUp blends 93h or 90h with PedGetupCB");
	Check(BytesAt(img, 0x004D7445, {0xC7, 0x83, 0x24, 0x02, 0, 0}) &&
	          Dword(img, 0x004D744B) == PEDSTATE_JUMP,
	      "SetJump's state is 23h");

	bool family = true;
	for (uint16_t id = ANIM_STD_GET_UP; id <= ANIM_STD_FALL_COLLAPSE; ++id)
		family = family && DescId(img, id) && (DescFlags(img, id) & ASSOC_PARTIAL);
	Check(family, "every one of 90h..9Ah is a partial");
	Check(NameIs(img, ANIM_STD_GET_UP, "Getup") && NameIs(img, ANIM_STD_GET_UP_FRONT, "Getup_front") &&
	          NameIs(img, ANIM_STD_JUMP_LAUNCH, "JUMP_launch") &&
	          NameIs(img, ANIM_STD_FALL_LAND, "FALL_land") &&
	          NameIs(img, ANIM_STD_FALL_COLLAPSE, "FALL_collapse") && NameIs(img, 0x9B, "EV_step"),
	      "and they are the get-ups, the jump and the fall by name");
	Check(DescFlags(img, ANIM_STD_FALL_LAND) == LAND_FLAGS &&
	          DescFlags(img, ANIM_STD_FALL_GLIDE) == GLIDE_FLAGS,
	      "the flags the landing test uses are FALL_land's and FALL_glide's");

	bool spendable = NameIs(img, 0x0A, "idle_armed") &&
	                 (DescFlags(img, 0x0A) & (ASSOC_PARTIAL | ASSOC_DELETEFADEDOUT)) == ASSOC_PARTIAL;
	for (uint16_t id = 0x31; id <= 0x3A; ++id)
		spendable = spendable && (DescFlags(img, id) & (ASSOC_PARTIAL | ASSOC_DELETEFADEDOUT)) ==
		                             ASSOC_PARTIAL;
	Check(spendable && NameIs(img, AIM, "WEAPON_hgun_body") && DescFlags(img, 0x32) == WEAPON_FLAGS,
	      "idle_armed and the weapon anims are partials the wipe leaves spent");
	Check((DescFlags(img, 0x77) & ASSOC_DELETEFADEDOUT) && (DescFlags(img, 0x78) & ASSOC_DELETEFADEDOUT),
	      "the drive-by pair is deleted by it instead");

	// A copy left lying down (StandUpDue): the knockdowns stay on their last
	// frame, and SetGetUp gives up for a non-player with a car at its feet.
	bool knockdowns = true;
	for (uint16_t id = ANIM_KNOCKDOWN_FIRST; id <= ANIM_KNOCKDOWN_LAST; ++id)
		knockdowns = knockdowns && DescId(img, id) && (DescFlags(img, id) & ASSOC_PARTIAL) &&
		             !(DescFlags(img, id) & (ASSOC_FADEOUTWHENDONE | ASSOC_DELETEFADEDOUT));
	Check(knockdowns, "0Dh..1Ch are partials that neither fade nor delete themselves");
	Check(DescFlags(img, 0x1D) & ASSOC_FADEOUTWHENDONE,
	      "and the first after them, a flinch, ends on its own");
	Check(BytesAt(img, 0x004D100B, {0xE8}) && BytesAt(img, 0x004D1015, {0x83, 0xB8, 0x84, 0x02, 0, 0, 0x05, 0x75, 0x79}),
	      "SetGetUp asks IsPositionClearOfCars and leaves on any car that is not a bike");
	Check(BytesAt(img, 0x004D10A7, {0xE8}) && CallTarget(img, 0x004D10A7) == 0x004D48E0 &&
	          BytesAt(img, 0x004D10C7, {0x80, 0xB8, 0xDF, 0, 0, 0, 0, 0x59, 0x74, 0x15}),
	      "where a ped that is not the player only acts with the controls off");
	Check(BytesAt(img, 0x004D0F31, {0x8A, 0x83, 0x58, 0x01, 0, 0, 0xC0, 0xE8, 0x05}) &&
	          BytesAt(img, 0x004D109F, {0x24, 0xDF}) && offs::PED_GETUP_ANIM_STARTED == 0x20,
	      "bGetUpAnimStarted is bit 5 of +158h");
	Check(BytesAt(img, 0x004CE837, {0x8A, 0x83, 0x5C, 0x01, 0, 0, 0x24, 0xEF}) &&
	          offs::PED_FLAGS_I == 0x15C && offs::PED_FALLEN_DOWN == 0x10,
	      "PedGetupCB clears bFallenDown, bit 4 of +15Ch");
	Check(BytesAt(img, 0x004D0FA1, {0x8A, 0x83, 0x56, 0x01, 0, 0, 0xC0, 0xE8, 0x04}) &&
	          offs::PED_UPDATE_ANIM_HEADING == 0x10,
	      "bUpdateAnimHeading is bit 4 of +156h");
}

// A copy that went down on our screen while its owner stayed up, or was left
// holding a knockdown the owner has got up from.
void TestACopyLeftLyingDown() {
	std::printf("\na copy left lying down is stood up\n");
	Check(IsKnockdownAnim(ANIM_STD_KO_FRONT) && IsKnockdownAnim(0x1C) && !IsKnockdownAnim(0x0C) &&
	          !IsKnockdownAnim(0x1D),
	      "the knockdowns are 0Dh..1Ch");
	Check(IsGetUpAnim(ANIM_STD_GET_UP) && IsGetUpAnim(ANIM_STD_GET_UP_FRONT) &&
	          !IsGetUpAnim(ANIM_STD_JUMP_LAUNCH),
	      "and the get-ups 90h..93h");
	Check(OwnerIsUp(1, ANIM_NONE) && OwnerIsUp(PEDSTATE_JUMP, 0x94),
	      "an owner idling or jumping is up");
	Check(!OwnerIsUp(PEDSTATE_FALL, ANIM_NONE) && !OwnerIsUp(PEDSTATE_GETUP, ANIM_NONE) &&
	          !OwnerIsUp(PEDSTATE_DIE, ANIM_NONE) && !OwnerIsUp(PEDSTATE_DEAD, ANIM_NONE),
	      "one falling, getting up, dying or dead is not");
	Check(!OwnerIsUp(1, 0x19), "nor one still playing a knockdown, whatever his state");

	bool     timing = false;
	uint32_t since  = 0;
	Check(!StandUpDue(timing, since, true, true, 1000), "the first frame down starts the clock");
	Check(!StandUpDue(timing, since, true, true, 1000 + STAND_UP_AFTER_MS - 1),
	      "and nothing happens inside the grace");
	Check(StandUpDue(timing, since, true, true, 1000 + STAND_UP_AFTER_MS),
	      "past it the copy is stood up");
	Check(!timing, "once");

	timing = false;
	StandUpDue(timing, since, true, true, 5000);
	Check(!StandUpDue(timing, since, true, false, 5100),
	      "the owner going down as well is a knockdown that really happened");
	Check(!StandUpDue(timing, since, true, true, 5000 + STAND_UP_AFTER_MS + 10),
	      "and it restarts the clock rather than carrying the old one");
	Check(!StandUpDue(timing, since, false, true, 9000) && !timing,
	      "a copy that got up by itself has nothing to answer for");

	timing = false;
	StandUpDue(timing, since, true, true, 0xFFFFFF00u);
	Check(StandUpDue(timing, since, true, true, 0xFFFFFF00u + STAND_UP_AFTER_MS),
	      "across the millisecond clock wrapping");
}

} // namespace

int RunAnimReviveTests() {
	std::printf("\n---- animations our engine took back (game/animrevive.h) ----\n");
	TestTheThreeLives();
	TestWhatTheLandingLeaves();
	TestThePlan();
	TestTheFamily();
	TestALandingFrameByFrame();
	TestTheOldRuleNeverCameBack();
	TestAGetUp();
	TestTheBase();
	TestACopyLeftLyingDown();
	TestAgainstTheImage();
	return g_reviveFailures;
}
