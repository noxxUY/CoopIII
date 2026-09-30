// A car hitting somebody on foot across two machines: client/src/game/runover.h.
//
// The redirects can't run here. What can is the arithmetic the player arm does
// with them in place, who gets to decide each kind of hit, the friendly fire
// rule, and - with a retail exe handed over - the call sites and constants
// read back out of it.

#include "game/combat.h"
#include "game/runover.h"

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
	Check(Bytes(img, 0x004ECE98, {0x83, 0x04, 0x24, uint8_t(ANIM_STD_HIGHIMPACT_FRONT), 0x6A,
	                              0x01}) &&
	          Bytes(img, 0x004ECEA2, {0x68, 0xE8, 0x03, 0x00, 0x00}) &&
	          CallsAt(img, 0x004ECEA7, CPed__SetFall),
	      "the knock arm falls with SetFall(1000, dir + 19h, true), the fall the kill "
	      "arm's copy is given");
}

void TestARunOverOnAnotherMachinesPedestrian() {
	std::printf("\nour car into somebody else's pedestrian\n");
	Check(RunOverFallAnim(0) == ANIM_STD_HIGHIMPACT_FRONT &&
	          RunOverFallAnim(3) == ANIM_STD_HIGHIMPACT_FRONT + 3 &&
	          RunOverFallAnim(6) == ANIM_STD_HIGHIMPACT_FRONT + 2,
	      "the fall is the knock arm's, by direction");
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
}

} // namespace

int RunRunOverTests() {
	g_runOverFailures = 0;
	TestTheCauses();
	TestATeammatesCarIsPricedBySpeed();
	TestWhoDecides();
	TestFriendlyFire();
	TestARunOverOnAnotherMachinesPedestrian();
	TestAgainstTheImage();
	return g_runOverFailures;
}
