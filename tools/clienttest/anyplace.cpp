// "The player" at a place for any participant with the owner nearby
// (client/src/game/anyplace.h): the block arithmetic, checked against a model
// of the engine's own UpdateCompareFlag over every small block, who answers,
// Mike Lips Last Lunch's ending at the bistro, the sites that stay the
// owner's, and, with a retail exe handed over, where the zones live.

#include "game/addresses.h"
#include "game/anyplace.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_apFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_apFailures;
}

// ---- the engine's block, as re3 Script.cpp has it --------------------------------------

struct Engine {
	uint16_t andOr = 0;
	uint8_t  flag  = 0;

	// ANDOR (00D6): 0 for one condition, 1..8 for an `if and` of n+1, 21..28
	// for an `if or` of n-19.
	void If(uint16_t n) {
		andOr = n;
		if (n >= 1 && n <= 8) {
			flag = 1;
			++andOr;
		} else if (n >= 21 && n <= 28) {
			flag = 0;
			++andOr;
		}
	}
	// UpdateCompareFlag.
	void Ask(bool raw, bool notFlag) {
		const bool v = notFlag ? !raw : raw;
		if (andOr == 0) {
			flag = v ? 1 : 0;
			return;
		}
		if (andOr >= 1 && andOr <= 8) {
			flag = static_cast<uint8_t>(flag & (v ? 1 : 0));
			andOr = andOr == 1 ? 0 : static_cast<uint16_t>(andOr - 1);
		} else if (andOr >= 21 && andOr <= 28) {
			flag = static_cast<uint8_t>(flag | (v ? 1 : 0));
			andOr = andOr == 21 ? 0 : static_cast<uint16_t>(andOr - 1);
		}
	}
};

// mission.h's CompareFlagIfTrue, which the client writes the yes with.
uint8_t YesFlag(uint8_t before, uint16_t andOrBefore, bool notFlag) {
	const uint8_t f = notFlag ? 0 : 1;
	if (andOrBefore == 0)
		return f;
	if (andOrBefore >= 1 && andOrBefore <= 8)
		return static_cast<uint8_t>(before & f);
	if (andOrBefore >= 21 && andOrBefore <= 28)
		return static_cast<uint8_t>(before | f);
	return before;
}

// One block of `count` conditions, the place at `placeAt` (the owner's own
// answer no), the others' raw answers in `others` (bit i) and their NOTs in
// `nots` (bit i, the place's included). Run as the client runs it, the
// place widened where `widen`, and with a car beside it that is not at the
// place when `conflict`. The result at the goto_if_false.
uint8_t RunBlock(bool isOr, int count, int placeAt, unsigned others, unsigned nots, bool widen,
                 bool conflict) {
	Engine e;
	e.If(count == 1 ? 0 : static_cast<uint16_t>((isOr ? 20 : 0) + count - 1));
	anyplace::Plan plan{};
	for (int i = 0; i < count; ++i) {
		const bool     notFlag = (nots >> i & 1) != 0;
		const uint16_t before  = e.andOr;
		const uint8_t  flagWas = e.flag;
		if (i == placeAt) {
			e.Ask(false, notFlag);
			// As AnybodyNearby: nothing to do where the block is decided already.
			const uint8_t yes = YesFlag(flagWas, before, notFlag);
			plan              = anyplace::PlanFor(before, notFlag);
			plan.valid        = plan.valid && widen && e.flag != yes;
			if (plan.valid && plan.now)
				e.flag = yes;
		} else {
			e.Ask((others >> i & 1) != 0, notFlag);
		}
	}
	if (anyplace::SettleWrites(plan, conflict))
		e.flag = plan.settleTo;
	return e.flag;
}

// The same block run by the engine alone, the place's raw answer `place`.
uint8_t Plain(bool isOr, int count, int placeAt, unsigned others, unsigned nots, bool place) {
	Engine e;
	e.If(count == 1 ? 0 : static_cast<uint16_t>((isOr ? 20 : 0) + count - 1));
	for (int i = 0; i < count; ++i)
		e.Ask(i == placeAt ? place : (others >> i & 1) != 0, (nots >> i & 1) != 0);
	return e.flag;
}

void TestTheBlock() {
	std::printf("\nthe place's yes inside its block\n");
	Check(anyplace::PlanFor(0, false).now && !anyplace::PlanFor(0, false).settle &&
	          anyplace::PlanFor(0, true).now,
	      "a single condition is answered as it is asked, and nothing waits for the goto_if_false");
	Check(!anyplace::PlanFor(9, false).valid && !anyplace::PlanFor(30, true).valid,
	      "an and/or state the engine itself ignores is left alone");

	// Every block of one to four conditions, `if and` and `if or`, the place
	// anywhere in it, every answer and NOT of the others.
	bool asYes = true, asNo = true;
	for (int isOr = 0; isOr < 2; ++isOr)
		for (int count = 1; count <= 4; ++count)
			for (int placeAt = 0; placeAt < count; ++placeAt)
				for (unsigned others = 0; others < (1u << count); ++others)
					for (unsigned nots = 0; nots < (1u << count); ++nots) {
						const bool o = isOr != 0;
						asYes = asYes && RunBlock(o, count, placeAt, others, nots, true, false) ==
						                     Plain(o, count, placeAt, others, nots, true);
						// A car beside the place needs a block of two at least.
						asNo = asNo && (count == 1 ||
						                RunBlock(o, count, placeAt, others, nots, true, true) ==
						                    Plain(o, count, placeAt, others, nots, false));
					}
	Check(asYes, "with nothing against it, every block comes out as it would with the place yes");
	Check(asNo, "with a car beside it not at the place, every block comes out as the owner's no");

	using anyplace::Beside;
	Check(anyplace::BesideOf(0x00DC) == Beside::Car && anyplace::BesideOf(0x80DC) == Beside::Car,
	      "IS_PLAYER_IN_CAR beside the place asks where that car is");
	Check(anyplace::BesideOf(0x00E0) == Beside::AnyCar && anyplace::BesideOf(0x00DE) == Beside::AnyCar &&
	          anyplace::BesideOf(0x0442) == Beside::AnyCar &&
	          anyplace::BesideOf(0x0443) == Beside::AnyCar,
	      "any car, a model, sitting in a car: the owner's own state");
	Check(anyplace::BesideOf(0x010F) == Beside::Nothing && anyplace::BesideOf(0x0122) == Beside::Nothing &&
	          anyplace::BesideOf(0x0447) == Beside::Nothing && anyplace::BesideOf(0x0038) == Beside::Nothing,
	      "his wanted level, his horn, his phone and the script's flags still have to hold, and do not "
	      "change who is at the place");
}

void TestWhoAnswers() {
	std::printf("\nwho is at the place, and whether the owner is near it\n");
	const MissionArea door = MissionAreaLocate2D(100.0f, 100.0f, 3.0f, 3.0f);
	Check(anyplace::OwnerNearby(door, Vec3{100.0f, 163.0f, 0.0f}) &&
	          anyplace::OwnerNearby(door, Vec3{163.0f, 37.0f, 40.0f}),
	      "the owner 60 m from the area, or up on a roof over it, is nearby");
	Check(!anyplace::OwnerNearby(door, Vec3{100.0f, 163.5f, 0.0f}) &&
	          !anyplace::OwnerNearby(door, Vec3{300.0f, 100.0f, 0.0f}),
	      "a step further, or a block away, is not");
	Check(anyplace::OWNER_NEARBY_M == 60.0f, "nearby is 60 m");

	standin::PlaceNeeds any{}, foot{}, car{}, stoppedCar{};
	standin::PlaceNeedsOf(0x00E3, &any);
	standin::PlaceNeedsOf(0x00E4, &foot);
	standin::PlaceNeedsOf(0x00E5, &car);
	standin::PlaceNeedsOf(0x00E8, &stoppedCar);

	anyplace::Candidate walker{true, Vec3{101.0f, 99.0f, 0.0f}, false, true, false};
	anyplace::Candidate driver{true, Vec3{99.0f, 102.0f, 0.0f}, true, false, false};
	Check(anyplace::Answers(any, door, walker, false) && anyplace::Answers(foot, door, walker, false) &&
	          !anyplace::Answers(car, door, walker, false),
	      "on foot at the door: any means and on foot, not in a car");
	Check(anyplace::Answers(car, door, driver, false) && !anyplace::Answers(foot, door, driver, false) &&
	          !anyplace::Answers(stoppedCar, door, driver, false),
	      "driving through: in a car, not on foot, not stopped");
	driver.stopped = true;
	Check(anyplace::Answers(stoppedCar, door, driver, false), "stopped there in his car: stopped in a car");
	Check(!anyplace::Answers(stoppedCar, door, driver, true),
	      "with the mission's stored car held, his own car is not it");
	driver.inStoredCar = true;
	Check(anyplace::Answers(stoppedCar, door, driver, true), "the stored car itself, with him at the wheel, is");
	anyplace::Candidate away{true, Vec3{110.0f, 100.0f, 0.0f}, false, true, false};
	anyplace::Candidate gone{};
	gone.at = Vec3{100.0f, 100.0f, 0.0f};
	Check(!anyplace::Answers(any, door, away, false) && !anyplace::Answers(any, door, gone, false),
	      "outside the area, or not in the mission, is nobody");
	const anyplace::Candidate c[4] = {gone, away, walker, driver};
	Check(anyplace::WhoAnswers(any, door, c, 4, false) == 2 &&
	          anyplace::WhoAnswers(car, door, c, 4, false) == 3 &&
	          anyplace::WhoAnswers(foot, door, c, 4, true) == -1,
	      "the first who is there as the check asks answers it");

	const float min[3] = {10.0f, 20.0f, -5.0f}, max[3] = {50.0f, 80.0f, 30.0f};
	const MissionArea zone = anyplace::ZoneArea(min, max);
	anyplace::Candidate inZone{true, Vec3{49.0f, 21.0f, 0.0f}, true, false, false};
	Check(anyplace::Answers(standin::PlaceNeeds{}, zone, inZone, false), "a zone is its box, any means");
	inZone.at.z = 31.0f;
	Check(!anyplace::Answers(standin::PlaceNeeds{}, zone, inZone, false), "and its height counts, as the engine's");
}

// Mike Lips Last Lunch, the end (retail JOEY1 at 3673):
//
//   if or
//     not car $LIPSFORELLI_CAR stopped in the bistro's space
//     is_player_in_area_3d 1306 -482 49 to 1350 -444 59         (by the car)
//     not is_player_in_area_3d 1306 -484 49 to 1370 -434 69     (out of sight)
//   goto_if_false @4037   (go on: the car parked, the player at the edge)
//
// A guest parked the car and walked to the edge; the owner stood across the
// street. The mission waited for the owner while the clock ran.
uint8_t BistroEnding(bool carParked, const anyplace::Candidate &guest, const Vec3 &owner) {
	const MissionArea inner = MissionAreaCorners3D(1306.0f, -482.0f, 49.0f, 1350.0f, -444.0f, 59.0f);
	const MissionArea outer = MissionAreaCorners3D(1306.0f, -484.0f, 49.0f, 1370.0f, -434.0f, 69.0f);
	const auto ownerIn = [&](const MissionArea &a) { return InMissionArea(owner, a, 0.0f); };
	standin::PlaceNeeds any{};
	standin::PlaceNeedsOf(0x0057, &any);
	const anyplace::Candidate c[1] = {guest};
	Engine e;
	e.If(22);
	e.Ask(carParked, true);
	// The two places, each the owner's own answer and then the rule's.
	anyplace::Plan plans[2];
	for (int k = 0; k < 2; ++k) {
		const MissionArea &a       = k == 0 ? inner : outer;
		const bool         notFlag = k == 1;
		const uint16_t     before  = e.andOr;
		const uint8_t      flagWas = e.flag;
		e.Ask(ownerIn(a), notFlag);
		plans[k] = anyplace::PlanFor(before, notFlag);
		const bool widen = !ownerIn(a) && anyplace::OwnerNearby(a, owner) &&
		                   anyplace::WhoAnswers(any, a, c, 1, false) >= 0;
		plans[k].valid = plans[k].valid && widen;
		if (plans[k].valid && plans[k].now)
			e.flag = YesFlag(flagWas, before, notFlag);
	}
	for (const anyplace::Plan &p : plans)
		if (anyplace::SettleWrites(p, false))
			e.flag = p.settleTo;
	return e.flag;
}

void TestMikeLipsBistro() {
	std::printf("\nMike Lips Last Lunch: the guest parks the car and walks to the edge\n");
	const anyplace::Candidate atTheEdge{true, Vec3{1360.0f, -440.0f, 50.0f}, false, true, false};
	const anyplace::Candidate byTheCar{true, Vec3{1336.0f, -461.0f, 50.0f}, false, true, false};
	const Vec3 acrossTheStreet{1300.0f, -500.0f, 50.0f};   // outside both boxes, 16 m off
	const Vec3 aBlockAway{1250.0f, -560.0f, 50.0f};        // 76 m off
	const Vec3 ownerByTheCar{1338.0f, -460.0f, 50.0f};

	Check(BistroEnding(true, atTheEdge, acrossTheStreet) == 0,
	      "the car parked, the guest at the edge and the owner across the street: Lips comes out");
	Check(BistroEnding(true, byTheCar, acrossTheStreet) == 1,
	      "the guest still by the car: it waits, as for the owner standing there");
	Check(BistroEnding(true, atTheEdge, aBlockAway) == 1,
	      "the owner a block away: it waits for him, the guest alone does not end it");
	Check(BistroEnding(false, atTheEdge, acrossTheStreet) == 1, "the car not parked: it waits");
	Check(BistroEnding(true, atTheEdge, ownerByTheCar) == 1,
	      "the owner himself by the car: his own answer still keeps it waiting");
	const anyplace::Candidate nobody{};
	Check(BistroEnding(true, nobody, acrossTheStreet) == 1, "nobody at the edge: it waits");
	Check(anyplace::ExcludedWhy(24, 0x0057, nullptr) == nullptr,
	      "nothing in Mike Lips Last Lunch stays the owner's");
}

void TestTheOwnersSites() {
	std::printf("\nthe places that stay the owner's\n");
	const float hideout[7]  = {0.0f, 879.375f, -303.375f, 7.25f, 870.0625f, -311.6875f, 10.0f};
	const float tonis[7]    = {0.0f, 1219.5625f, -320.6875f, 27.375f, 1.0f, 1.0f, 2.0f};
	const float casino[7]   = {0.0f, 452.25f, -1465.75f, 17.5625f, 4.0f, 4.0f, 4.0f};
	const float laundry[7]  = {0.0f, 839.1875f, -667.375f, 14.0f, 842.0625f, -673.875f, 17.0f};
	const float joeys[5]    = {0.0f, 1089.875f, -223.875f, 1084.5f, -228.5f};
	Check(anyplace::ExcludedWhy(19, 0x01A0, hideout) != nullptr,
	      "Give Me Liberty's hideout: the scene walks the owner out of the Kuruma");
	Check(anyplace::ExcludedWhy(31, 0x00F6, tonis) != nullptr, "The Pick-Up: the walk into Toni's");
	Check(anyplace::ExcludedWhy(51, 0x00F8, casino) != nullptr &&
	          anyplace::ExcludedWhy(52, 0x00F8, casino) != nullptr,
	      "Deal Steal's and Shima's casino: the owner walked out of his car");
	Check(anyplace::ExcludedWhy(29, 0x019B, joeys) != nullptr,
	      "The Getaway: asked of the robbers' driver, not by the general rule");
	Check(anyplace::ExcludedWhy(14, anyplace::OP_IS_PLAYER_IN_ZONE, nullptr) != nullptr &&
	          anyplace::ExcludedWhy(64, anyplace::OP_IS_PLAYER_IN_ZONE, nullptr) == nullptr,
	      "the taxi's fares pick by the owner's zone; Uzi Rider's Hepburn Heights is anybody's");
	Check(anyplace::ExcludedWhy(27, 0x01A0, laundry) == nullptr,
	      "Cipriani's Chauffeur's laundry stop is anybody's, the owner nearby");
	Check(anyplace::ExcludedWhy(19, 0x00E5, laundry) == nullptr,
	      "a site is its mission, command and place: another place in the same mission is not it");
	Check(anyplace::ExcludedWhy(61, 0x0057, laundry) != nullptr && anyplace::ExcludedWhy(58, 0x00E3, casino) != nullptr,
	      "Waka-Gashira's car park and Plaster Blaster's decoy stay the stealth rules'");
}

// ---- against the retail exe -------------------------------------------------------------

constexpr uint32_t IMAGE_BASE = 0x00400000;
constexpr size_t   IMAGE_SIZE = 2383872;

bool LoadExe(std::vector<uint8_t> &image) {
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
		if (got == image.size() && image.size() == IMAGE_SIZE)
			return true;
	}
	return false;
}

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
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

void TestTheZones() {
	std::printf("\nCTheZones, for IS_PLAYER_IN_ZONE asked of a participant\n");
	std::vector<uint8_t> img;
	if (!LoadExe(img)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check it\n");
		return;
	}
	using namespace game::zones;
	Check(CallsAt(img, IN_ZONE_FINDS_LABEL, CTheZones__FindZoneByLabel) &&
	          CallsAt(img, IN_ZONE_GETS_ZONE, CTheZones__GetZone) &&
	          CallsAt(img, IN_ZONE_TESTS_POINT, CTheZones__PointLiesWithinZone),
	      "IS_PLAYER_IN_ZONE finds the label, gets the zone and tests the point");
	Check(Bytes(img, CTheZones__GetZone, {0x0F, 0xB7, 0x44, 0x24, 0x04, 0x6B, 0xC0}) &&
	          img[CTheZones__GetZone + 7 - IMAGE_BASE] == SIZEOF_ZONE &&
	          Bytes(img, CTheZones__GetZone + 8, {0x05}) &&
	          Dword(img, CTheZones__GetZone + 9) == CTheZones__ZoneArray &&
	          Bytes(img, CTheZones__GetZone + 13, {0xC3}),
	      "GetZone is ZoneArray at 0x0086BEE0 plus 0x38 a zone");
	Check(Bytes(img, CTheZones__FindZoneByLabel + 0x25, {0x66, 0x83, 0x3D}) &&
	          Dword(img, CTheZones__FindZoneByLabel + 0x28) == CTheZones__TotalNumberOfZones &&
	          CallsAt(img, CTheZones__FindZoneByLabel + 0x47, CTheZones__GetZone) &&
	          Bytes(img, CTheZones__FindZoneByLabel + 0x4C, {0x8B, 0x10, 0x8B, 0x40, 0x04}),
	      "the label is looked up up to TotalNumberOfZones, by the zone's first eight bytes");
	Check(Bytes(img, CTheZones__PointLiesWithinZone + 8, {0xD9, 0x02, 0xD8, 0x51, uint8_t(ZONE_MIN)}) &&
	          Bytes(img, CTheZones__PointLiesWithinZone + 0x14, {0xD8, 0x51, uint8_t(ZONE_MAX)}) &&
	          Bytes(img, CTheZones__PointLiesWithinZone + 0x2B, {0xD8, 0x59, uint8_t(ZONE_MIN + 4)}) &&
	          Bytes(img, CTheZones__PointLiesWithinZone + 0x35, {0xD8, 0x51, uint8_t(ZONE_MAX + 4)}) &&
	          Bytes(img, CTheZones__PointLiesWithinZone + 0x4C, {0xD8, 0x59, uint8_t(ZONE_MIN + 8)}) &&
	          Bytes(img, CTheZones__PointLiesWithinZone + 0x56, {0xD8, 0x51, uint8_t(ZONE_MAX + 8)}),
	      "PointLiesWithinZone: min at +08h, max at +14h, x, y and z");
}

} // namespace

int RunAnyplaceTests() {
	g_apFailures = 0;
	TestTheBlock();
	TestWhoAnswers();
	TestMikeLipsBistro();
	TestTheOwnersSites();
	TestTheZones();
	return g_apFailures;
}
