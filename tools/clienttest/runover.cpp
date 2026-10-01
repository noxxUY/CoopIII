// A car hitting somebody on foot across two machines: client/src/game/runover.h.
//
// The redirects can't run here. What can is the arithmetic the player arm does
// with them in place, who gets to decide each kind of hit, the friendly fire
// rule, and - with a retail exe handed over - the call sites and constants
// read back out of it.

#include "game/combat.h"
#include "game/runover.h"
#include "game/vehicle.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_runOverFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_runOverFailures;
}

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

void TestTheCauses() {
	std::printf("the two car causes\n");
	Check(IsCarHitCause(WEAPONTYPE_RAMMEDBYCAR) && IsCarHitCause(WEAPONTYPE_RUNOVERBYCAR),
	      "rammed and run over are car hits");
	Check(!IsCarHitCause(WEAPONTYPE_ARMOUR) && !IsCarHitCause(WEAPONTYPE_EXPLOSION) &&
	          !IsCarHitCause(WEAPONTYPE_UNARMED) && !IsCarHitCause(WEAPONTYPE_FALL),
	      "and their neighbours and a punch are not");
}

void TestATeammatesCarIsPricedBySpeed() {
	std::printf("what the player arm asks for\n");
	Check(DriverCountsAsPlayer(true, false) && DriverCountsAsPlayer(false, true) &&
	          !DriverCountsAsPlayer(false, false),
	      "a player at the wheel by ped type or by being a session player");

	const float fifty = 50.0f / KMH_PER_SPEED;   // head on, all of it closing
	Check(Near(CarHitOnPlayerDamage(fifty, true, false), 277.8f, 0.1f),
	      "a player's car closing at 50 km/h asks for 278");
	Check(PlayerHealthFor(CarHitOnPlayerDamage(fifty, true, false)) > 90.0f,
	      "which is over 90 health off a player");
	Check(CarHitOnPlayerDamage(fifty, false, false) == CAR_HIT_FLAT_DAMAGE,
	      "the same car with nobody the engine calls a player asks for the flat 20");
	Check(Near(PlayerHealthFor(CAR_HIT_FLAT_DAMAGE), 6.6f),
	      "6.6 health, which is what a teammate's car used to cost");
	Check(CarHitOnPlayerDamage(0.3f, false, true) == CAR_HIT_TRAIN_DAMAGE,
	      "a train nobody drives is 50");
	Check(Near(CarHitOnPlayerDamage(0.3f, true, true), 300.0f),
	      "the driver test comes before the train test, as at 0x004C93A0");
	Check(Near(CarHitOnPlayerDamage(CAR_HIT_MIN_CLOSING, true, false), 100.0f),
	      "the slowest closing speed that hits asks for 100");

	Check(!PlayerArmHits(CAR_HIT_MIN_SPEED_SQ, 1.0f), "a car at 0.05 is a bump");
	Check(!PlayerArmHits(1.0f, CAR_HIT_MIN_CLOSING), "one closing at 0.1 is a look");
	Check(PlayerArmHits(0.04f, 0.15f), "one doing 36 km/h at us is a hit");
}

void TestWhoDecides() {
	std::printf("who decides a car hit\n");
	//                     cause  us     other  ours   ourCar theirCar
	Check(ClassifyCarHit(true, true, false, false, false, true) ==
	          CarHit::OnUsByAnotherPlayer,
	      "another player's car on us is ours to decide");
	Check(ClassifyCarHit(true, true, false, false, false, false) == CarHit::None,
	      "a car nobody plays on us is the engine's, as in single player");
	Check(ClassifyCarHit(false, true, false, false, false, true) == CarHit::None,
	      "a cause that isn't a car is not this file's");
	Check(ClassifyCarHit(true, false, true, false, true, false) == CarHit::OurCarOnTheirs,
	      "our car on another machine's ped is theirs to decide");
	Check(ClassifyCarHit(true, false, true, false, false, true) == CarHit::None,
	      "somebody else's car on somebody else's ped is nothing to do with us");
	Check(ClassifyCarHit(true, false, false, true, false, true) == CarHit::TheirCarOnOurs,
	      "another player's car on a pedestrian we host is ours to decide");
	Check(ClassifyCarHit(true, false, false, true, false, false) == CarHit::None,
	      "and traffic on him is plain single player");
}

void TestFriendlyFire() {
	std::printf("friendly fire on a car hit\n");
	Check(!CarHitMayLand(CarHit::OnUsByAnotherPlayer, false),
	      "off: a teammate's car keeps its hands off our health");
	Check(CarHitMayLand(CarHit::OnUsByAnotherPlayer, true), "on: it lands");
	Check(CarHitMayLand(CarHit::TheirCarOnOurs, false) &&
	          CarHitMayLand(CarHit::OurCarOnTheirs, false) &&
	          CarHitMayLand(CarHit::None, false),
	      "a pedestrian is nobody's teammate");
}

// ---- against the real exe ---------------------------------------------------

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

float Float(const std::vector<uint8_t> &img, uint32_t va) {
	const uint32_t bits = Dword(img, va);
	float f;
	std::memcpy(&f, &bits, sizeof f);
	return f;
}

bool CallsAt(const std::vector<uint8_t> &img, uint32_t site, uint32_t target) {
	return img[site - IMAGE_BASE] == 0xE8 && site + 5 + Dword(img, site + 1) == target;
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> b) {
	size_t i = 0;
	for (uint8_t x : b)
		if (img[va - IMAGE_BASE + i++] != x)
			return false;
	return true;
}

void TestAgainstTheImage() {
	std::printf("\nthe car hit against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "car hit against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(Bytes(img, 0x004C93A0, {0x8B, 0x8D}) && Dword(img, 0x004C93A2) == offs::VEH_DRIVER,
	      "the player arm loads collidingVeh->pDriver");
	Check(CallsAt(img, CPed__ProcessControl_CarDriverIsPlayer, CPed__IsPlayer),
	      "and asks IsPlayer of it at 0x004C93AA");
	Check(Bytes(img, 0x004C93B7, {0xD8, 0x0D}) && Dword(img, 0x004C93B9) == 0x005F8528 &&
	          Float(img, 0x005F8528) == CAR_HIT_PER_CLOSING,
	      "a player's car is closing speed times 1000");
	Check(Bytes(img, 0x004C93C4, {0x83, 0xF8, uint8_t(MI_TRAIN)}) &&
	          Float(img, 0x005F842C) == CAR_HIT_TRAIN_DAMAGE,
	      "a train is 50");
	Check(Bytes(img, 0x004C93D1, {0xD9, 0x05}) && Dword(img, 0x004C93D3) == 0x005F8424 &&
	          Float(img, 0x005F8424) == CAR_HIT_FLAT_DAMAGE,
	      "anything else is 20");
	Check(Float(img, 0x005F8470) == CAR_HIT_MIN_CLOSING &&
	          Float(img, 0x005F8520) == CAR_HIT_MIN_SPEED_SQ,
	      "the two thresholds");
	Check(Bytes(img, 0x004C93E7, {0x6A, 0x10}) &&
	          CallsAt(img, CPed__ProcessControl_CarHitDamage, CPed__InflictDamage),
	      "the hit goes to InflictDamage as RAMMEDBYCAR at 0x004C93ED");
	Check(Bytes(img, 0x004EA505, {0xD8, 0x0D}) && Dword(img, 0x004EA507) == 0x005F9C80 &&
	          Float(img, 0x005F9C80) == PLAYER_DAMAGE_SCALE,
	      "which takes a third of it off a player");

	Check(CallsAt(img, CPed__KillPedWithCar_KillDamage, CPed__InflictDamage) &&
	          Float(img, 0x005F9AC8) == 1000.0f,
	      "KillPedWithCar's kill arm is InflictDamage for 1000 at 0x004ECD24");
	Check(Bytes(img, 0x004ECE8E, {0x6A, 0x10}) &&
	          CallsAt(img, CPed__KillPedWithCar_KnockDamage, CPed__InflictDamage),
	      "and its knockdown is InflictDamage as RAMMEDBYCAR at 0x004ECE91");

	int callers = 0;
	for (uint32_t va = IMAGE_BASE + 0x1000; va + 5 < IMAGE_BASE + IMAGE_SIZE; ++va)
		if (CallsAt(img, va, CPed__KillPedWithCar))
			++callers;
	Check(callers == 3 && CallsAt(img, 0x0049D760, CPed__KillPedWithCar) &&
	          CallsAt(img, 0x0049D7AF, CPed__KillPedWithCar) &&
	          CallsAt(img, 0x004C9439, CPed__KillPedWithCar),
	      "KillPedWithCar has the three callers addresses.h names");
	Check(CallsAt(img, CPhysical__Collision_KillPedWithCar, CPed__KillPedWithCar) &&
	          Bytes(img, 0x0049D752, {0x8B, 0x4C, 0x24, 0x14}) &&
	          CallsAt(img, CPed__ProcessControl_KillPedWithCar, CPed__KillPedWithCar) &&
	          Bytes(img, 0x004C9430, {0x89, 0xD9}),
	      "the car's and the ped's collision call it with the ped in ecx");
	// InflictDamage's arm for the two car causes (addresses.h, 0x004EAA1E).
	Check(CallsAt(img, 0x004EA9EC, CGeneral__GetRandomNumber) &&
	          Bytes(img, 0x004EA9F4, {0x24, 0x03}) &&
	          Bytes(img, 0x004EA444, {0x8B, 0x7C, 0x24, 0x40}) &&
	          Bytes(img, 0x004EAA10, {0x0F, 0xB6, 0x54, 0x24, 0x44, 0x83, 0xFA, 0x03}),
	      "the car arm draws rand() & 3 and switches on the direction, the piece in edi");
	Check(Bytes(img, 0x004EAA1E, {0xFF, 0x24, 0x95}) &&
	          Dword(img, 0x004EAA21) == INFLICT_DAMAGE_CAR_ANIM_TABLE &&
	          Dword(img, INFLICT_DAMAGE_CAR_ANIM_TABLE) == 0x004EAA25 &&
	          Dword(img, INFLICT_DAMAGE_CAR_ANIM_TABLE + 4) == 0x004EAA5C &&
	          Dword(img, INFLICT_DAMAGE_CAR_ANIM_TABLE + 8) == 0x004EAA77 &&
	          Dword(img, INFLICT_DAMAGE_CAR_ANIM_TABLE + 12) == 0x004EAAB7,
	      "its four directions");
	Check(Bytes(img, 0x004EAA25, {0x83, 0xFF, PEDPIECE_LEFTARM, 0x75, 0x04, 0x3C, 0x01, 0x77}) &&
	          Bytes(img, 0x004EAA2E, {0x83, 0xFF, PEDPIECE_MID, 0x75, 0x0E, 0x3C, 0x01, 0x75}) &&
	          Bytes(img, 0x004EAA37, {0xBB, uint8_t(ANIM_STD_HIGHIMPACT_LEFT)}) &&
	          Bytes(img, 0x004EAA41, {0x83, 0xFF, PEDPIECE_RIGHTARM, 0x75, 0x04, 0x3C, 0x01, 0x77}) &&
	          Bytes(img, 0x004EAA4A, {0x83, 0xFF, PEDPIECE_MID, 0x75, 0x06, 0x3C, 0x02}) &&
	          Bytes(img, 0x004EAAC7, {0xBB, uint8_t(ANIM_STD_HIGHIMPACT_RIGHT)}) &&
	          Bytes(img, 0x004EAA55, {0xBB, uint8_t(ANIM_STD_HIGHIMPACT_FRONT)}),
	      "from the front: an arm or the middle by rand, else straight back");
	Check(Bytes(img, 0x004EAA5C, {0x83, 0xBD, 0x24, 0x02, 0x00, 0x00,
	                              uint8_t(PEDSTATE_DIVE_AWAY)}) &&
	          Bytes(img, 0x004EAA65, {0xBB, uint8_t(ANIM_STD_SPINFORWARD_LEFT)}) &&
	          Bytes(img, 0x004EAA70, {0xBB, uint8_t(ANIM_STD_HIGHIMPACT_LEFT)}) &&
	          Bytes(img, 0x004EAAB7, {0x83, 0xBD, 0x24, 0x02, 0x00, 0x00,
	                                  uint8_t(PEDSTATE_DIVE_AWAY)}) &&
	          Bytes(img, 0x004EAAC0, {0xBB, uint8_t(ANIM_STD_SPINFORWARD_RIGHT)}),
	      "from the side: a spin only while diving away");
	Check(Bytes(img, 0x004EAA77, {0x83, 0xFF, PEDPIECE_LEFTARM}) &&
	          Bytes(img, 0x004EAA89, {0xBB, uint8_t(ANIM_STD_SPINFORWARD_LEFT)}) &&
	          Bytes(img, 0x004EAA90, {0x83, 0xFF, PEDPIECE_RIGHTARM}) &&
	          Bytes(img, 0x004EAAA2, {0xBB, uint8_t(ANIM_STD_SPINFORWARD_RIGHT)}) &&
	          Bytes(img, 0x004EAAB0, {0xBB, uint8_t(ANIM_STD_HIGHIMPACT_BACK)}),
	      "from behind: the same tests spin him forward");
	Check(Bytes(img, 0x004EA43F, {0xBB, uint8_t(ANIM_STD_KO_FRONT), 0x00, 0x00, 0x00}),
	      "and the default is the knockout");
	Check(Bytes(img, 0x004EA580, {0x83, 0xBD, 0x24, 0x02, 0x00, 0x00, uint8_t(PEDSTATE_FALL)}) &&
	          Bytes(img, 0x004EA589, {0xBB, uint8_t(ANIM_STD_NUM & 0xFF), 0x00, 0x00, 0x00}),
	      "a ped dying while he falls, head up, gets no new animation");

	Check(Bytes(img, 0x004ECE98, {0x83, 0x04, 0x24, uint8_t(ANIM_STD_HIGHIMPACT_FRONT), 0x6A,
	                              0x01}) &&
	          Bytes(img, 0x004ECEA2, {0x68, 0xE8, 0x03, 0x00, 0x00}) &&
	          CallsAt(img, 0x004ECEA7, CPed__SetFall),
	      "the knock arm falls with SetFall(1000, dir + 19h, true), the fall the kill "
	      "arm's copy is given");
}

void TestARunOverOnAnotherMachinesPedestrian() {
	std::printf("\nour car into somebody else's pedestrian\n");
	Check(RunOverLeftStanding(PEDSTATE_IDLE) && !RunOverLeftStanding(PEDSTATE_FALL) &&
	          !RunOverLeftStanding(PEDSTATE_DIE) && !RunOverLeftStanding(PEDSTATE_DEAD),
	      "only a copy still on its feet is put down");
	Check(RunOverForwardDue(INVALID_NETID, 0, 5, 100) &&
	          !RunOverForwardDue(5, 100, 5, 100 + RUN_OVER_FORWARD_GAP_MS - 1) &&
	          RunOverForwardDue(5, 100, 5, 100 + RUN_OVER_FORWARD_GAP_MS) &&
	          RunOverForwardDue(5, 100, 6, 101),
	      "one hit per pedestrian per contact goes to his host");
	float out = -1.0f;
	Check(RunOverImpulseFromWire(8.5f, out) && out == 8.5f, "an impulse is taken as it is");
	Check(RunOverImpulseFromWire(1.0e9f, out) && out == MAX_RUN_OVER_IMPULSE,
	      "and bounded");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(!RunOverImpulseFromWire(nan, out) && !RunOverImpulseFromWire(-1.0f, out),
	      "and nothing that is not one");
	Check(IsRunOverForward(WEAPONTYPE_RAMMEDBYCAR) && !IsRunOverForward(WEAPONTYPE_RUNOVERBYCAR) &&
	          !IsRunOverForward(WEAPONTYPE_COLT45),
	      "a run-over goes out as RAMMEDBYCAR");
	Check(!IsForwardableDamage(WEAPONTYPE_RAMMEDBYCAR),
	      "which is still no damage an attacker decides");

	std::printf("\nthe copy goes down the way a ped of ours would\n");
	// direction, rand() & 3, piece, state - InflictDamage's car arm.
	Check(RunOverKillAnim(0, 0, PEDPIECE_TORSO, PEDSTATE_IDLE) == ANIM_STD_HIGHIMPACT_FRONT &&
	          RunOverKillAnim(1, 0, PEDPIECE_TORSO, PEDSTATE_IDLE) == ANIM_STD_HIGHIMPACT_LEFT &&
	          RunOverKillAnim(2, 0, PEDPIECE_TORSO, PEDSTATE_IDLE) == ANIM_STD_HIGHIMPACT_BACK &&
	          RunOverKillAnim(3, 0, PEDPIECE_TORSO, PEDSTATE_IDLE) == ANIM_STD_HIGHIMPACT_RIGHT,
	      "a body hit falls the way it was hit from");
	Check(RunOverKillAnim(0, 2, PEDPIECE_LEFTARM, PEDSTATE_IDLE) == ANIM_STD_HIGHIMPACT_LEFT &&
	          RunOverKillAnim(0, 1, PEDPIECE_LEFTARM, PEDSTATE_IDLE) == ANIM_STD_HIGHIMPACT_FRONT &&
	          RunOverKillAnim(0, 3, PEDPIECE_RIGHTARM, PEDSTATE_IDLE) == ANIM_STD_HIGHIMPACT_RIGHT &&
	          RunOverKillAnim(0, 1, PEDPIECE_MID, PEDSTATE_IDLE) == ANIM_STD_HIGHIMPACT_LEFT &&
	          RunOverKillAnim(0, 2, PEDPIECE_MID, PEDSTATE_IDLE) == ANIM_STD_HIGHIMPACT_RIGHT,
	      "from the front, an arm or the middle turns him to that side");
	Check(RunOverKillAnim(2, 1, PEDPIECE_MID, PEDSTATE_IDLE) == ANIM_STD_SPINFORWARD_LEFT &&
	          RunOverKillAnim(2, 2, PEDPIECE_MID, PEDSTATE_IDLE) == ANIM_STD_SPINFORWARD_RIGHT &&
	          RunOverKillAnim(2, 0, PEDPIECE_MID, PEDSTATE_IDLE) == ANIM_STD_HIGHIMPACT_BACK,
	      "over the bonnet (direction turned round by two) he spins forward");
	Check(RunOverKillAnim(1, 0, PEDPIECE_TORSO, PEDSTATE_DIVE_AWAY) ==
	              ANIM_STD_SPINFORWARD_LEFT &&
	          RunOverKillAnim(3, 0, PEDPIECE_TORSO, PEDSTATE_DIVE_AWAY) ==
	              ANIM_STD_SPINFORWARD_RIGHT,
	      "and diving away, from the side, too");
	Check(RunOverKillAnim(4, 0, PEDPIECE_TORSO, PEDSTATE_IDLE) == ANIM_STD_KO_FRONT,
	      "a direction past the table is the default knockout");

	Check(RunOverHoldsPose(true, 1000, 1000, PEDSTATE_FALL) &&
	          RunOverHoldsPose(true, 1000, 1000 + RUN_OVER_HOLD_MS - 1, PEDSTATE_GETUP),
	      "down from our car, his host's rows wait while he falls and gets up");
	Check(!RunOverHoldsPose(true, 1000, 1000 + RUN_OVER_HOLD_MS, PEDSTATE_FALL) &&
	          !RunOverHoldsPose(true, 1000, 1200, PEDSTATE_IDLE) &&
	          !RunOverHoldsPose(false, 1000, 1200, PEDSTATE_FALL),
	      "not once he is up, not for ever, and not for a fall that was not ours");
	Check(RunOverHoldsPose(true, 0xFFFFFF00u, 0x00000100u, PEDSTATE_FALL),
	      "across the clock wrapping");
	Check(RunOverDeathAnim(true, PEDSTATE_FALL, ANIM_STD_HIGHIMPACT_BACK) == ANIM_STD_NUM &&
	          RunOverDeathAnim(true, PEDSTATE_GETUP, ANIM_STD_HIGHIMPACT_BACK) ==
	              ANIM_STD_HIGHIMPACT_BACK &&
	          RunOverDeathAnim(false, PEDSTATE_FALL, ANIM_STD_HIGHIMPACT_BACK) ==
	              ANIM_STD_HIGHIMPACT_BACK,
	      "his host's death while he falls from our hit goes on with the fall");
}

void TestACopyOfAnotherMachinesCar() {
	std::printf("\nour car into another machine's traffic car\n");
	const Vec3 doing60{0.33f, 0.0f, 0.0f};
	const Vec3 got = AmbientCopyMoveSpeed(doing60, 0);
	Check(got.x == doing60.x && got.y == 0.0f && got.z == 0.0f,
	      "while its rows come, the copy carries its host's speed");
	const Vec3 stopped = AmbientCopyMoveSpeed(doing60, AMBIENT_COPY_MOVING_MS);
	Check(stopped.x == 0.0f && stopped.y == 0.0f && stopped.z == 0.0f,
	      "and none once they stop, where the snap holds it");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	const Vec3  bad = AmbientCopyMoveSpeed(Vec3{nan, 0.0f, 0.0f}, 0);
	const Vec3  big = AmbientCopyMoveSpeed(Vec3{inf, 0.0f, 0.0f}, 0);
	Check(bad.x == 0.0f && big.x == 0.0f, "nothing that is not a speed reaches the engine");
	const Vec3 fast = AmbientCopyMoveSpeed(Vec3{100.0f, 0.0f, 0.0f}, 0);
	Check(fast.x == WIRE_MOVE_MAX, "and a wild one is held to what a car can do");
}

} // namespace

int RunRunOverTests() {
	g_runOverFailures = 0;
	TestTheCauses();
	TestATeammatesCarIsPricedBySpeed();
	TestWhoDecides();
	TestFriendlyFire();
	TestARunOverOnAnotherMachinesPedestrian();
	TestACopyOfAnotherMachinesCar();
	TestAgainstTheImage();
	return g_runOverFailures;
}
