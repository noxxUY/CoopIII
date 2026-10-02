// The Shoreside Vale missions' rules (docs/mission-audit.md §3, "Shoreside
// Vale, step by step"): nearchar.h's car locates and places, standin.h's
// stash garage, cargo, boats and decoy van, anyplace.h's sites and stores.
#include "game/anyplace.h"
#include "game/nearchar.h"
#include "game/standin.h"

#include <cstdio>

namespace {

int g_failed = 0;

void Check(bool ok, const char *what) {
	if (!ok) {
		++g_failed;
		std::printf("FAIL shoreside: %s\n", what);
	}
}

namespace nearchar = coopiii::game::nearchar;
namespace standin  = coopiii::game::standin;
namespace anyplace = coopiii::game::anyplace;

void TestCarLocates() {
	using nearchar::CarLocate;
	using nearchar::CarMeans;
	CarLocate l{};
	Check(nearchar::CarLocateOf(0x01FD, &l) && !l.is3d && l.means == CarMeans::OnFoot,
	      "01FD: on foot near a car, 2D (Grand Theft Aero's van)");
	Check(nearchar::CarLocateOf(0x01FE, &l) && !l.is3d && l.means == CarMeans::InCar,
	      "01FE: in a car near a car, 2D (Escort Service's truck)");
	Check(nearchar::CarLocateOf(0x0201, &l) && l.is3d && l.means == CarMeans::InCar, "0201: in a car, 3D");
	Check(nearchar::CarLocateOf(0x01FF, &l) && l.is3d && l.means == CarMeans::Any, "01FF: any means, 3D");
	Check(!nearchar::CarLocateOf(0x0202, &l) && !nearchar::CarLocateOf(0x01FB, &l),
	      "a pedestrian near a car and a car's own locate are not the player near a car");

	Check(nearchar::AnswersNearCarForNearest(nearchar::GRAND_THEFT_AERO) &&
	          nearchar::AnswersNearCarForNearest(nearchar::ESCORT_SERVICE),
	      "Grand Theft Aero's van and Escort Service's truck are in the car table");
	Check(!nearchar::AnswersNearCarForNearest(69) && !nearchar::AnswersNearCarForNearest(77),
	      "Decoy's van and Bullion Run are not");

	const CarLocate onFoot{false, CarMeans::OnFoot}, any{false, CarMeans::Any};
	Check(nearchar::CarLocateMayWiden(onFoot, 0) && nearchar::CarLocateMayWiden(onFoot, 21),
	      "on foot near the car: alone, or in an `if or`");
	Check(!nearchar::CarLocateMayWiden(onFoot, 1) && !nearchar::CarLocateMayWiden(onFoot, 2),
	      "never in an `if and`: Escort Service's 1 m beside IS_PLAYER_STOPPED is one player");
	Check(nearchar::CarLocateMayWiden(any, 1), "any means keeps the blocks it had (Paparazzi Purge's `if and`)");

	Check(nearchar::MeansFits(CarMeans::OnFoot, false) && !nearchar::MeansFits(CarMeans::OnFoot, true),
	      "on foot: a seated guest is not on foot");
	Check(nearchar::MeansFits(CarMeans::InCar, true) && !nearchar::MeansFits(CarMeans::InCar, false),
	      "in a car: a guest on foot is not in one");
	Check(nearchar::MeansFits(CarMeans::Any, true) && nearchar::MeansFits(CarMeans::Any, false), "any means: both");
}

void TestPlacesForAnybody() {
	using nearchar::AnswersPlaceForAnybody;
	Check(AnswersPlaceForAnybody(nearchar::BAIT, 0x00E3, -996.8125f, -247.5f, 30.0f, 30.0f) &&
	          AnswersPlaceForAnybody(nearchar::BAIT, 0x00E3, -877.0f, 562.0f, 40.0f, 40.0f),
	      "Bait: a guest by cartel A or B sets its car off");
	Check(AnswersPlaceForAnybody(nearchar::BAIT, 0x00F5, -459.0f, 251.5f, 30.0f, 30.0f) &&
	          AnswersPlaceForAnybody(nearchar::BAIT, 0x00F5, -459.0f, 251.5f, 70.0f, 70.0f),
	      "Bait: cartel D is made and set off for a guest");
	Check(!AnswersPlaceForAnybody(nearchar::BAIT, 0x00E3, -996.8125f, -247.5f, 40.0f, 40.0f) &&
	          !AnswersPlaceForAnybody(nearchar::BAIT, 0x00F5, -1254.0f, 85.0f, -1178.0f, 160.0f),
	      "Bait: another radius, and the killzone's own check, are not");
	Check(AnswersPlaceForAnybody(nearchar::SAM, 0x00E3, -1019.0f, -1263.0f, 60.0f, 60.0f) &&
	          AnswersPlaceForAnybody(nearchar::SAM, 0x00E3, -1385.25f, -1035.0f, 80.0f, 80.0f) &&
	          AnswersPlaceForAnybody(nearchar::SAM, 0x00E3, -1478.25f, -1062.75f, 80.0f, 80.0f),
	      "S.A.M.: the Colombians on the docks turn on a guest");
	Check(!AnswersPlaceForAnybody(nearchar::SAM, 0x00E3, -805.0f, -1310.0f, 15.0f, 15.0f) &&
	          !AnswersPlaceForAnybody(nearchar::SAM, 0x00E3, -805.0f, -1310.0f, 160.0f, 160.0f),
	      "S.A.M.: the buoy and the island load are not");
	Check(!AnswersPlaceForAnybody(nearchar::BAIT, 0x00E3, -1019.0f, -1263.0f, 60.0f, 60.0f) &&
	          !AnswersPlaceForAnybody(nearchar::SAM, 0x00E3, -996.8125f, -247.5f, 30.0f, 30.0f),
	      "each mission's spots are its own");

	// The operand form, the player first: IS_PLAYER_IN_AREA_3D's corners.
	const float ground[7]  = {0, -247.25f, 333.875f, 2.0f, -209.5f, 250.1875f, 15.0f};
	const float helipad[7] = {0, -1142.0f, 327.75f, 29.0f, -1215.5625f, 368.375f, 40.0f};
	const float mansion[7] = {0, -448.0f, 241.6875f, 50.0f, -292.375f, 365.1875f, 90.0f};
	Check(AnswersPlaceForAnybody(nearchar::RUMBLE, 0x0057, ground),
	      "Rumble: a guest on the fighting ground brings the Nines out");
	Check(AnswersPlaceForAnybody(nearchar::THE_EXCHANGE, 0x0057, helipad),
	      "The Exchange: a guest on the helipad brings its guards out");
	Check(!AnswersPlaceForAnybody(nearchar::THE_EXCHANGE, 0x0057, mansion) &&
	          !AnswersPlaceForAnybody(nearchar::RUMBLE, 0x0057, helipad) &&
	          !AnswersPlaceForAnybody(nearchar::THE_EXCHANGE, 0x0056, helipad),
	      "the mansion's wait, another mission's box and another command are not");
	const float cartelA[7] = {0, -996.8125f, -247.5f, 30.0f, 30.0f, 0, 0};
	Check(AnswersPlaceForAnybody(nearchar::BAIT, 0x00E3, cartelA), "the operand form reaches Bait's spots");
}

void TestLovesStash() {
	using namespace standin;
	const float inside[4] = {-1049.125f, -77.4375f, -1037.1875f, -69.125f};
	const Box  *box       = AreaAnybodyKeeps(ESCORT_SERVICE, IS_PLAYER_IN_AREA_2D, inside);
	Check(box && InBox(*box, -1043.0f, -73.0f, 40.0f),
	      "Escort Service: a guest inside Love's stash keeps its door up");
	Check(box && !InBox(*box, -1030.0f, -73.0f, 40.0f), "and one outside does not");
	Check(!AreaAnybodyKeeps(ESCORT_SERVICE, IS_PLAYER_IN_AREA_3D, inside) &&
	          !AreaAnybodyKeeps(67, IS_PLAYER_IN_AREA_2D, inside),
	      "only that check of that mission");
	const float rounded[4] = {-1049.125f, -77.4375f, -1037.188f, -69.125f};
	Check(AreaAnybodyKeeps(ESCORT_SERVICE, IS_PLAYER_IN_AREA_2D, rounded) != nullptr,
	      "the corner as the decompile rounds it is the same corner");
}

void TestCargo() {
	using namespace standin;
	Check(IsCargoLocate(SAM, 0x00F5, 4.0f, 4.0f), "S.A.M.'s 4 m locate at a package is the cargo");
	Check(!IsCargoLocate(SAM, 0x00F6, 1.0f, 1.0f) && !IsCargoLocate(SAM, 0x00E3, 15.0f, 15.0f) &&
	          !IsCargoLocate(SAM, 0x00E3, 4.0f, 4.0f) && !IsCargoLocate(56, 0x00F5, 4.0f, 4.0f),
	      "the stash, the buoy, a 2D locate and Evidence Dash are not");
	Check(CollectSlack(SAM, 0x00F5, 4.0f, 4.0f) == CARGO_SLACK &&
	          CollectSlack(EVIDENCE_DASH, 0x00F5, 1.5f, 1.5f) == EVIDENCE_SLACK &&
	          CollectSlack(SAM, 0x00E3, 60.0f, 60.0f) < 0.0f,
	      "each object gets its own slack, anything else none");
	Check(OnTheObject(7.5f, 0.0f, 0.0f, 4.0f, 4.0f, 4.0f, true, CARGO_SLACK) &&
	          !OnTheObject(8.5f, 0.0f, 0.0f, 4.0f, 4.0f, 4.0f, true, CARGO_SLACK),
	      "a guest's copy drifts up to the slack away from the owner's");
	Check(OnTheEvidence(4.0f, 0.0f, 0.0f, 1.5f, 1.5f, 1.5f, true) ==
	          OnTheObject(4.0f, 0.0f, 0.0f, 1.5f, 1.5f, 1.5f, true, EVIDENCE_SLACK),
	      "Evidence Dash's files are unchanged");
	Check(AnybodyInModel(SAM) && AnybodyInModel(nearchar::LIBERATOR) && !AnybodyInModel(72),
	      "S.A.M.'s boats are anybody's model, as Liberator's Colombian car");
}

void TestDecoy() {
	using namespace standin;
	Check(IsDecoyEnd(DECOY, 0x00E3, -1026.5f, -73.5f, 160.0f, 160.0f), "Decoy's end is the 160 m round the warehouse");
	Check(!IsDecoyEnd(DECOY, 0x00E3, -1026.5f, -73.5f, 200.0f, 200.0f),
	      "the 200 m approach that makes the van is not");
	Check(!IsDecoyEnd(DECOY, 0x00F5, -1026.5f, -73.5f, 160.0f, 160.0f) &&
	          !IsDecoyEnd(68, 0x00E3, -1026.5f, -73.5f, 160.0f, 160.0f),
	      "another command or mission is not");

	bool in[4] = {false, false, true, false};
	Check(DecoyRider(false, in, 4) == 2, "a guest driving the van answers for it");
	Check(DecoyRider(true, in, 4) == -1, "the owner in the van answers himself");
	bool none[4] = {};
	Check(DecoyRider(false, none, 4) == -1, "nobody in the van: the owner's own answer");
}

void TestSites() {
	const float decoyEnd[7]   = {0, -1026.5f, -73.5f, 160.0f, 160.0f, 0, 0};
	const float decoyStart[7] = {0, -1026.5f, -73.5f, 200.0f, 200.0f, 0, 0};
	Check(anyplace::ExcludedWhy(69, 0x00E3, decoyEnd) != nullptr,
	      "Decoy's end is not the general rule's: a guest by the warehouse does not fail it");
	Check(anyplace::ExcludedWhy(69, 0x00E3, decoyStart) == nullptr,
	      "the approach round the same point is: the van is made for a guest with the owner near");
	const float island[7] = {0, -805.0f, -1310.0f, 160.0f, 160.0f, 0, 0};
	const float buoy[7]   = {0, -805.0f, -1310.0f, 15.0f, 15.0f, 0, 0};
	Check(anyplace::ExcludedWhy(73, 0x00E3, island) != nullptr,
	      "S.A.M.'s island load is the owner's, for where he is");
	Check(anyplace::ExcludedWhy(73, 0x00E3, buoy) == nullptr, "and the buoy at the same point is anybody's");
	Check(anyplace::ExcludedWhy(69, 0x00E3, nullptr) == nullptr, "a site with a radius needs the operands");
	Check(anyplace::RadiusAt(0x00E3) == 3 && anyplace::RadiusAt(0x00FA) == 4 && anyplace::RadiusAt(0x0057) < 0,
	      "a locate's radius follows its point, 2D and 3D");
	const float loves[7] = {0, 87.25f, -1548.5625f, 2.0f, 1.0f, 2.0f, 0};
	Check(anyplace::ExcludedWhy(67, 0x00F9, loves) != nullptr, "the walk into Love's still is the owner's");

	Check(anyplace::StoreIsOnlyATarget(67) && !anyplace::StoreIsOnlyATarget(59) &&
	          !anyplace::StoreIsOnlyATarget(77),
	      "Grand Theft Aero's stores are only the goons' targets; Marked Man's and Bullion Run's stay stored cars");
}

} // namespace

int RunShoresideTests() {
	g_failed = 0;
	TestCarLocates();
	TestPlacesForAnybody();
	TestLovesStash();
	TestCargo();
	TestDecoy();
	TestSites();
	if (g_failed == 0)
		std::printf("shoreside: ok\n");
	return g_failed;
}
