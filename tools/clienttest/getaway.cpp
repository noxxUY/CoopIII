// The Getaway's robbers' driver (game/getaway.h): whose car the mission's
// questions follow once the robbers are in it.
#include "game/anyplace.h"
#include "game/getaway.h"
#include "game/mission.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failed = 0;

void Check(bool ok, const char *what) {
	if (!ok) {
		++g_failed;
		std::printf("FAIL getaway: %s\n", what);
	}
}

using namespace coopiii::game::getaway;
namespace anyplace = coopiii::game::anyplace;
namespace scripts  = coopiii::game::scripts;

constexpr size_t N = 4;

// A participant driving a car with `free` seats at the pickup, arrived `order`th.
Candidate Driving(int free, uint32_t order) {
	Candidate c;
	c.valid     = true;
	c.drives    = true;
	c.freeSeats = free;
	c.nearby      = true;
	c.arrived   = order;
	return c;
}

void TestBeforeTheyBoard() {
	Candidate c[N];
	const int owner = 0;
	Check(ChooseDriver(c, N, owner, 3, false, -1, false) == -1, "nobody there: nobody");

	c[0] = Driving(3, 2);
	c[1] = Driving(3, 1);
	Check(ChooseDriver(c, N, owner, 3, false, -1, false) == 0,
	      "the owner in a car with the seats free takes them, though a guest got there first");

	c[0] = Driving(1, 0);
	Check(ChooseDriver(c, N, owner, 3, false, -1, false) == 1,
	      "the owner in a car with too few seats: the guest who arrived with enough");
	c[2] = Driving(3, 0);
	c[1] = Driving(3, 5);
	c[2] = Driving(3, 3);
	Check(ChooseDriver(c, N, owner, 3, false, -1, false) == 2,
	      "two guests with seats: the first to arrive");

	c[0].valid = false;
	c[1].nearby = false;
	c[2].nearby = false;
	Check(ChooseDriver(c, N, owner, 3, false, -1, false) == -1, "not at the pickup: nobody");

	c[1] = Driving(3, 4);
	c[1].drives = false;
	Check(ChooseDriver(c, N, owner, 3, false, -1, false) == -1, "on foot or riding: nobody");

	c[1] = Driving(2, 1);
	Check(ChooseDriver(c, N, owner, 3, false, -1, false) == -1,
	      "a guest with two seats free does not take three robbers");
	Check(ChooseDriver(c, N, owner, 2, false, -1, false) == 1,
	      "with two robbers left alive two seats do");
}

void TestOnceTheyExist() {
	Candidate c[N];
	c[0] = Driving(3, 2);
	c[1] = Driving(3, 1);
	// The robbers exist, the guest was the one: kept while he still could take them.
	Check(ChooseDriver(c, N, 0, 3, false, 1, true) == 1, "the choice is kept once the robbers exist");
	Check(ChooseDriver(c, N, 0, 3, false, 1, false) == 0, "and not before: the owner has the priority");
	c[1].freeSeats = 1;
	Check(ChooseDriver(c, N, 0, 3, false, 1, true) == 0, "no longer able to: the owner");
}

void TestRobbersInACar() {
	Candidate c[N];
	c[0] = Driving(0, 1);
	c[1] = Driving(0, 2);
	c[1].drivesRobbers = true;
	c[1].inRobbersCar  = true;
	Check(ChooseDriver(c, N, 0, 3, true, -1, true) == 1,
	      "the robbers in the guest's car: the guest, though the owner drives one too");
	c[0].drivesRobbers = true;
	c[0].inRobbersCar  = true;
	Check(ChooseDriver(c, N, 0, 3, true, -1, true) == 0, "two drive it: the owner");
	Check(ChooseDriver(c, N, 0, 3, true, 1, true) == 1, "the one already chosen stays");

	c[0].drivesRobbers = false;
	c[1].drivesRobbers = false;
	c[1].inRobbersCar  = false;
	Check(ChooseDriver(c, N, 0, 3, true, -1, true) == 0,
	      "the owner riding in their car with nobody at the wheel: he is the player in it");
	c[0].inRobbersCar = false;
	Check(ChooseDriver(c, N, 0, 3, true, -1, true) == -1,
	      "nobody of the group in the robbers' car: nobody, whatever the owner drives");

	c[2].valid         = true;
	c[2].inRobbersCar  = true;
	Check(ChooseDriver(c, N, 0, 3, true, -1, true) == 2, "a guest riding in it, nobody at the wheel");
}

void TestWhatIsShown() {
	Check(ShowsCrewHints(1, 1, 0) && !ShowsCrewHints(0, 1, 0),
	      "the markers and the car's prompts are the driver's alone");
	Check(ShowsCrewHints(0, -1, 0) && !ShowsCrewHints(1, -1, 0),
	      "with nobody chosen they are the owner's, not both players'");
	uint8_t label[8] = {};
	std::memcpy(label, "HORN", 4);
	Check(IsCrewLabel(label), "'Sound the horn' is about the car");
	std::memcpy(label, "JM6_5\0\0\0", 8);
	Check(IsCrewLabel(label), "'You need a getaway vehicle' too");
	std::memcpy(label, "NODOORS\0", 8);
	Check(IsCrewLabel(label), "and 'They ain't sardines'");
	std::memcpy(label, "JM6_7\0\0\0", 8);
	Check(!IsCrewLabel(label), "'You need all 3' is everybody's");
	std::memcpy(label, "OUTTIME\0", 8);
	Check(!IsCrewLabel(label), "so is the clock");
	std::memcpy(label, "HORNS\0\0\0", 8);
	Check(!IsCrewLabel(label), "a label that only starts like one is not");
}

void TestPlaces() {
	Check(AtThePickup(1087.2f, -226.2f) && AtThePickup(1087.0f, -212.5f) && !AtThePickup(1087.0f, -205.0f),
	      "the pickup is Joey's box and the street round it");
	Check(SeatsNeeded(0) == 3 && SeatsNeeded(2) == 2 && SeatsNeeded(3) == 3,
	      "three seats for three robbers, fewer for the ones left");
	Check(NobodyAnswersAsOwner(true) && !NobodyAnswersAsOwner(false),
	      "with nobody in their car, only an on-foot question stays the owner's");
}

void TestFlag() {
	using scripts::ANDOR_NONE;
	using coopiii::game::CompareFlagFor;
	Check(CompareFlagFor(0, ANDOR_NONE, false, true) == 1 && CompareFlagFor(0, ANDOR_NONE, false, false) == 0,
	      "a single condition: the answer");
	Check(CompareFlagFor(0, ANDOR_NONE, true, true) == 0 && CompareFlagFor(0, ANDOR_NONE, true, false) == 1,
	      "under a NOT: the other way round");
	Check(CompareFlagFor(1, 1, false, false) == 0 && CompareFlagFor(1, 1, false, true) == 1,
	      "an `if and`: no brings it down, yes leaves it");
	Check(CompareFlagFor(0, 21, false, true) == 1 && CompareFlagFor(0, 21, false, false) == 0,
	      "an `if or`: yes raises it, no leaves it");
	Check(CompareFlagFor(1, 21, true, false) == 1, "a NOT in an `if or` that said no raises it");
	Check(CompareFlagFor(1, 1, true, true) == 0, "a NOT in an `if and` that said yes brings it down");
}

void TestTheOwnersSite() {
	Check(anyplace::ExcludedWhy(29, 0x019B, nullptr) != nullptr,
	      "the mission's places stay out of the general rule: getaway.h answers them");
	Check(anyplace::ExcludedWhy(21, 0x00E8, nullptr) != nullptr,
	      "and Drive Misty For Me's, the same way");
}

void TestMisty() {
	const Ride *misty = RideOf(DRIVE_MISTY_FOR_ME);
	Check(misty && RideOf(THE_GETAWAY) == &RIDES[0] && !RideOf(24),
	      "a ride for Drive Misty For Me and The Getaway, none for the rest");
	if (!misty)
		return;
	Check(AtThePickup(937.875f, -259.75f, *misty) && !AtThePickup(1087.2f, -226.2f, *misty),
	      "Misty's pickup is outside her flat, not Joey's");
	Check(SeatsNeeded(0, *misty) == 1, "one seat for Misty");
	uint8_t label[8] = {};
	std::memcpy(label, "HEY4", 4);
	Check(IsCrewLabel(label, *misty) && !IsCrewLabel(label),
	      "'Go and get her' is the driver's in Misty's ride alone");
	std::memcpy(label, "LM3_2\0\0\0", 8);
	Check(!IsCrewLabel(label, *misty), "'Take Misty to Joey's' is everybody's");
	Candidate c[N];
	c[0].valid = true;
	c[1]       = Driving(1, 1);
	Check(ChooseDriver(c, N, 0, SeatsNeeded(0, *misty), false, -1, false) == 1,
	      "a guest who stops for her with a free seat picks her up");
}

} // namespace

int RunGetawayTests() {
	g_failed = 0;
	TestBeforeTheyBoard();
	TestOnceTheyExist();
	TestRobbersInACar();
	TestWhatIsShown();
	TestPlaces();
	TestFlag();
	TestTheOwnersSite();
	TestMisty();
	if (g_failed == 0)
		std::printf("getaway: ok\n");
	return g_failed;
}
