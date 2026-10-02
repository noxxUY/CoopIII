// Who stands in for the player in the Portland missions' conditions
// (game/standin.h), and in Kenji's (standin.h's walked-out car, nearchar.h's
// Shima and Smack Down).
#include "game/nearchar.h"
#include "game/standin.h"

#include <cstdio>

namespace {

int g_failed = 0;

void Check(bool ok, const char *what) {
	if (!ok) {
		++g_failed;
		std::printf("FAIL standin: %s\n", what);
	}
}

using namespace coopiii::game::standin;
namespace nearchar = coopiii::game::nearchar;

void TestEscort() {
	Check(EscortMission(15) && EscortMission(16) && EscortMission(17) && EscortMission(18),
	      "Marty's four jobs take a participant driving the passenger");
	Check(!EscortMission(19) && !EscortMission(21) && !EscortMission(23) && !EscortMission(36),
	      "Misty's, The Fuzz Ball's and Curly's locates are not this rule's");

	CharLocate l{};
	Check(CharLocateOf(0x00EB, &l) && !l.is3d && l.means == Means::InCar, "00EB: in a car, 2D");
	Check(CharLocateOf(0x00FB, &l) && l.is3d && l.means == Means::Any, "00FB: any means, 3D");
	Check(CharLocateOf(0x00FC, &l) && l.means == Means::OnFoot, "00FC: on foot");
	Check(!CharLocateOf(0x00EC, &l) && !CharLocateOf(0x00E3, &l),
	      "a pedestrian's own locate and a place are not about the player near a pedestrian");

	Check(InCharBox(8.0f, -8.0f, 50.0f, 8.0f, 8.0f, 0.0f, false), "2D: the box is inclusive, z ignored");
	Check(!InCharBox(8.1f, 0.0f, 0.0f, 8.0f, 8.0f, 8.0f, true), "outside in x");
	Check(!InCharBox(0.0f, 0.0f, 31.0f, 30.0f, 30.0f, 30.0f, true), "3D: outside in z");

	// The bank manager at the kerb, a guest in Marty's car 5 m away.
	Escort e[4]{};
	e[1] = Escort{5.0f, 0.0f, 0.0f, true, true, false, true};
	CharLocate pickup{false, Means::InCar};
	Check(EscortFor(e, 4, pickup, 8.0f, 8.0f, 0.0f) == 1, "the guest in Marty's car picks him up");
	// The same guest in a car of his own: not the car the passenger is going with.
	e[1].inMissionCar = false;
	Check(EscortFor(e, 4, pickup, 8.0f, 8.0f, 0.0f) == -1, "a guest in his own car does not");
	// On foot beside him: no car to take him in.
	e[1] = Escort{1.0f, 0.0f, 0.0f, false, false, false, true};
	Check(EscortFor(e, 4, CharLocate{true, Means::Any}, 30.0f, 30.0f, 30.0f) == -1,
	      "a guest on foot is not the car he rides in");
	// Riding: the passenger sits in the car the guest drives, whatever car it is.
	e[1] = Escort{0.0f, 0.0f, 0.0f, true, false, true, true};
	CharLocate behind{true, Means::Any};
	Check(EscortFor(e, 4, behind, 30.0f, 30.0f, 30.0f) == 1,
	      "the guest driving him is not leaving him behind");
	Check(EscortFor(e, 4, CharLocate{true, Means::OnFoot}, 30.0f, 30.0f, 30.0f) == -1,
	      "an on-foot locate is never answered by a car");
	// Two: the nearer answers, an invalid one never.
	e[2] = Escort{2.0f, 2.0f, 0.0f, true, true, false, true};
	e[1] = Escort{20.0f, 0.0f, 0.0f, true, true, false, true};
	e[3] = Escort{0.5f, 0.0f, 0.0f, true, true, false, false};
	Check(EscortFor(e, 4, behind, 30.0f, 30.0f, 30.0f) == 2, "the nearest valid one answers");
	e[2].valid = e[1].valid = false;
	Check(EscortFor(e, 4, behind, 30.0f, 30.0f, 30.0f) == -1, "nobody valid, nobody answers");
}

void TestHeldRespray() {
	Check(BlockOf(0) == Block::Single && BlockOf(2) == Block::And && BlockOf(23) == Block::Or &&
	          BlockOf(9) == Block::Other,
	      "the and/or counter's three kinds");
	// The Thieves: if or (NOT respray, NOT in car, NOT in the shop). The
	// respray's yes leaves 0; the block comes out 0 when the rest agree.
	Check(HeldAnswerSpent(Block::Or, true, false), "everything held: the loop ends and the respray is spent");
	Check(!HeldAnswerSpent(Block::Or, true, true),
	      "the car not at the shop yet: the respray is kept for the next ask");
	Check(HeldAnswerSpent(Block::Or, false, true), "if or (respray, ...) true: spent");
	Check(HeldAnswerSpent(Block::And, false, true), "if and true: spent");
	Check(!HeldAnswerSpent(Block::And, false, false), "if and failed on another condition: kept");
	Check(HeldAnswerSpent(Block::And, true, false), "NOT respray in an if and: spent, as asking it is");
}

void TestCarAtThePlace() {
	PlaceNeeds n{};
	Check(PlaceNeedsOf(0x0199, &n) && !n.onFoot && n.stopped, "0199: stopped in the Pay'n'Spray");
	Check(PlaceNeedsOf(0x01A0, &n) && !n.onFoot && n.stopped, "01A0: stopped in the area in a car");
	Check(PlaceNeedsOf(0x019C, &n) && n.onFoot && !n.stopped, "019C: on foot, 3D");
	Check(PlaceNeedsOf(0x019A, &n) && n.onFoot && n.stopped, "019A: stopped on foot");
	Check(PlaceNeedsOf(0x0198, &n) && !n.onFoot && !n.stopped, "0198: in a car");
	Check(PlaceNeedsOf(0x00E4, &n) && n.onFoot && !n.stopped, "00E4: locate on foot");
	Check(PlaceNeedsOf(0x00E8, &n) && !n.onFoot && n.stopped, "00E8: locate stopped in a car");
	Check(PlaceNeedsOf(0x00F9, &n) && n.onFoot && n.stopped, "00F9: locate stopped on foot, 3D");
	Check(PlaceNeedsOf(0x0057, &n) && !n.onFoot && !n.stopped, "0057: in the area");
	Check(!PlaceNeedsOf(0x00E9, &n) && !PlaceNeedsOf(0x01A1, &n), "not a place of the player");

	Check(CarAnswersPlace(PlaceNeeds{false, true}, true), "a stopped car answers a stopped check");
	Check(!CarAnswersPlace(PlaceNeeds{false, true}, false), "a moving one does not");
	Check(CarAnswersPlace(PlaceNeeds{false, false}, false), "any car answers an in-area check");
	Check(!CarAnswersPlace(PlaceNeeds{true, false}, true), "a car never answers an on-foot check");
	Check(CarStopped(0.01f, 0.01f, 0.0f) && !CarStopped(0.03f, 0.0f, 0.0f), "stopped is 3.6 km/h");
}

void TestStaunton() {
	PlaceNeeds n{};
	Check(PlaceNeedsOf(0x00E5, &n) && n.inCar && !n.onFoot && !n.stopped, "00E5: locate in a car");
	Check(PlaceNeedsOf(0x019B, &n) && n.inCar && n.stopped, "019B: stopped in the area in a car");
	Check(PlaceNeedsOf(0x0057, &n) && !n.inCar && !n.onFoot, "0057: any means");

	// Liberator's garage doors, its compound and not its gate; Bling-Bling's
	// checkpoint; nothing of anybody else's.
	const float door[8]     = {0.0f, 63.0f, -317.5f, 13.0f, 69.25f, -321.6875f, 20.0f, 0.0f};
	const float compound[8] = {0.0f, 31.0f, -317.0f, 14.0f, 91.0f, -394.0f, 25.0f, 0.0f};
	const float gate[8]     = {0.0f, 92.1875f, -329.375f, 15.0f, 96.375f, -315.75f, 18.0f, 0.0f};
	Check(nearchar::AnswersPlaceForAnybody(nearchar::LIBERATOR, 0x019C, door), "a guest on foot at a garage door opens it");
	Check(nearchar::AnswersPlaceForAnybody(nearchar::LIBERATOR, 0x0057, compound), "a guest in the compound sets the guards going");
	Check(!nearchar::AnswersPlaceForAnybody(nearchar::LIBERATOR, 0x0057, gate), "the gate's message stays the owner's");
	Check(nearchar::AnswersPlaceForAnybody(nearchar::BLING_BLING_SCRAMBLE, 0x00E5, door), "a guest's car scores a checkpoint");
	Check(!nearchar::AnswersPlaceForAnybody(nearchar::BLING_BLING_SCRAMBLE, 0x019B, door) &&
	          !nearchar::AnswersPlaceForAnybody(nearchar::BLING_BLING_SCRAMBLE, 0x00E4, door),
	      "the start and the on-foot hint stay the owner's");
	Check(!nearchar::AnswersPlaceForAnybody(61, 0x019C, door) && !nearchar::AnswersPlaceForAnybody(40, 0x00E5, door),
	      "no other mission's place is anybody's");
	const float dealer[6] = {0.0f, 39.25f, -880.5625f, 90.0f, 90.0f, 0.0f};
	const float diablos[8] = {0.0f, 940.0f, -185.0f, 30.0f, 25.0f, 25.0f, 10.0f, 0.0f};
	Check(nearchar::AnswersPlaceForAnybody(nearchar::SMACK_DOWN, 0x00E3, dealer) &&
	          nearchar::AnswersPlaceForAnybody(nearchar::SHIMA, 0x00F5, diablos),
	      "one table: Smack Down's dealers and Shima's Diablos read from the same operands");

	PlaceNeeds onFoot{}, inCar{};
	PlaceNeedsOf(0x019C, &onFoot);
	PlaceNeedsOf(0x00E5, &inCar);
	Check(PlayerAnswersPlace(onFoot, false, false) && !PlayerAnswersPlace(onFoot, true, true),
	      "a door opens for a guest on foot, not one in a car");
	Check(PlayerAnswersPlace(inCar, true, false) && !PlayerAnswersPlace(inCar, false, false),
	      "a checkpoint scores for a guest in a car, not one on foot");
	PlaceNeeds stopped{};
	PlaceNeedsOf(0x019B, &stopped);
	Check(PlayerAnswersPlace(stopped, true, true) && !PlayerAnswersPlace(stopped, true, false),
	      "a stopped check wants the car stopped");

	Check(AnybodyInModel(nearchar::LIBERATOR) && !AnybodyInModel(61) && !AnybodyInModel(36),
	      "Liberator's Colombian car is anybody's; Waka-Gashira's and Curly's are not");
}

void TestRoundUp() {
	Check(IsRoundUpModel(134) && IsRoundUpModel(136) && IsRoundUpModel(137) && !IsRoundUpModel(135) &&
	          !IsRoundUpModel(138) && !IsRoundUpModel(-1),
	      "the Sentinel, the Stinger and the Stallion, not a Yardie or a Colombian car");
	int32_t models[4] = {-1, -1, -1, -1};
	Check(RoundUpSubject(-1, models, 4) == -1, "nobody in a gang car: the owner's own answer");
	models[2] = 137;
	Check(RoundUpSubject(-1, models, 4) == 2, "the owner on foot, a guest in a Diablo car: the guest");
	Check(RoundUpSubject(90, models, 4) == 2, "the owner in a car of his own: still the guest");
	Check(RoundUpSubject(134, models, 4) == -1, "the owner in a Mafia car himself: the owner");
	models[1] = 136;
	Check(RoundUpSubject(-1, models, 4) == 1, "two guests in gang cars: the first");
	models[1] = 135;
	Check(RoundUpSubject(-1, models, 4) == 2, "a Yardie car is not one Courtney wants");
}

void TestEvidence() {
	Check(IsEvidenceLocate(56, 0x00F5, 1.5f, 1.5f) && IsEvidenceLocate(56, 0x00E3, 1.5f, 1.5f) &&
	          IsEvidenceLocate(56, 0x00E3, 30.0f, 30.0f),
	      "Evidence Dash's three locates at a dropped file");
	Check(!IsEvidenceLocate(56, 0x00E3, 150.0f, 150.0f),
	      "not the 150 m one round the car's start, which picks where the next car is made");
	Check(!IsEvidenceLocate(55, 0x00E3, 4.0f, 4.0f) && !IsEvidenceLocate(58, 0x00E3, 25.0f, 25.0f) &&
	          !IsEvidenceLocate(56, 0x00E9, 1.5f, 1.5f),
	      "nor any other mission's, nor a locate against a pedestrian");
	Check(OnTheEvidence(1.0f, -1.0f, 0.5f, 1.5f, 1.5f, 1.5f, true),
	      "a participant on the owner's copy of the file picks it up");
	Check(OnTheEvidence(1.5f + EVIDENCE_SLACK, 0.0f, 0.0f, 1.5f, 1.5f, 1.5f, true) &&
	          !OnTheEvidence(1.6f + EVIDENCE_SLACK, 0.0f, 0.0f, 1.5f, 1.5f, 1.5f, true),
	      "and on his own copy, a few metres off where his car threw it");
	Check(OnTheEvidence(0.0f, 0.0f, 90.0f, 30.0f, 30.0f, 0.0f, false) &&
	          !OnTheEvidence(0.0f, 34.0f, 0.0f, 30.0f, 30.0f, 0.0f, false),
	      "the 2D ones leave the height out");
}

void TestSitHint() {
	Check(SitHintDue(0, 5), "the first hint goes at once");
	Check(!SitHintDue(1000, 1000 + SIT_HINT_EVERY_MS - 1) && SitHintDue(1000, 1000 + SIT_HINT_EVERY_MS),
	      "then once every six seconds");
}

void TestSalvatoresGarage() {
	// The two checks as Sayonara Salvatore writes them: x1 y1 z1 x2 y2 z2,
	// 1/16ths (1427.563 is 1427.5625).
	const float garage[6] = {1427.5625f, -187.25f, 49.5f, 1442.5625f, -179.0f, 53.75f};
	const Box  *box       = AreaAnybodyKeeps(SAYONARA_SALVATORE, IS_PLAYER_IN_AREA_3D, garage);
	Check(box != nullptr, "Salvatore's garage is kept open by anybody inside");
	Check(!AreaAnybodyKeeps(TWO_FACED_TANNER, IS_PLAYER_IN_AREA_3D, garage) &&
	          !AreaAnybodyKeeps(SAYONARA_SALVATORE, IS_PLAYER_IN_AREA_2D, garage),
	      "in that mission only, and only the 3D check");
	const float club[6] = {878.75f, -427.375f, 0.0f, 890.75f, -403.875f, 0.0f};
	Check(!AreaAnybodyKeeps(SAYONARA_SALVATORE, IS_PLAYER_IN_AREA_3D, club),
	      "the mission's other places stay the owner's");
	Check(box && InBox(*box, 1435.0f, -183.0f, 51.0f), "a guest standing in the garage is inside");
	Check(box && !InBox(*box, 1435.0f, -183.0f, 55.0f) && !InBox(*box, 1426.0f, -183.0f, 51.0f),
	      "on its roof or in the drive is not");
	const Box swapped = {10.0f, 10.0f, 0.0f, 0.0f, 0.0f, 0.0f, false};
	Check(InBox(swapped, 5.0f, 10.0f, 99.0f) && !InBox(swapped, 10.1f, 5.0f, 0.0f),
	      "corners either way round, inclusive, z left out of a 2D box");
}

void TestWhoTheGuardsSpotted() {
	const float behind[4] = {845.75f, -443.8125f, 890.75f, -433.8125f};
	const float across[4] = {920.0625f, -408.8125f, 931.3125f, -398.0625f};
	const float steps[4]  = {878.75f, -427.375f, 890.75f, -403.875f};
	Check(SafeBoxOf(IS_PLAYER_IN_AREA_2D, behind) == 0 && SafeBoxOf(IS_PLAYER_IN_AREA_2D, across) == 1,
	      "the two places a guard seeing the player does not count");
	Check(SafeBoxOf(IS_PLAYER_IN_AREA_2D, steps) == -1 && SafeBoxOf(IS_PLAYER_IN_AREA_3D, behind) == -1,
	      "and nothing else");
	Check(InSafeBox(860.0f, -440.0f) && InSafeBox(925.0f, -400.0f) && !InSafeBox(900.0f, -420.0f),
	      "behind the club and across the street are safe, the street is not");

	Watched w[4]{};
	Check(SpottedStandIn(false, false, w, 4) == -1, "nobody seen: nobody stands in");
	Check(SpottedStandIn(true, false, w, 4) == -1, "the owner seen in the open: his own answers stand");
	w[2] = Watched{true, true, false};
	Check(SpottedStandIn(true, false, w, 4) == -1, "and stay his with a guest seen too");
	Check(SpottedStandIn(false, false, w, 4) == 2, "a guest seen in the open, the owner not: the guest");
	Check(SpottedStandIn(true, true, w, 4) == 2,
	      "the owner seen where it does not count, a guest seen in the open: the guest gives it away");
	w[2].safe = true;
	Check(SpottedStandIn(true, true, w, 4) == -1, "both seen where it does not count: the owner's answers");
	Check(SpottedStandIn(false, false, w, 4) == 2,
	      "only a guest seen, and where it does not count: his place lets him off");
	w[1] = Watched{true, true, false};
	Check(SpottedStandIn(false, false, w, 4) == 1, "one in the open beats one where it does not count");
	w[1].valid = false;
	Check(SpottedStandIn(false, false, w, 4) == 2, "somebody not in the mission is nobody");
	w[2].seen = false;
	Check(SpottedStandIn(false, false, w, 4) == -1, "a guest the guard cannot see is not spotted");

	Check(AsksAfterSpotting(1000, 1019) && AsksAfterSpotting(1000, 1049),
	      "the two checks after a spotting, 19 and 49 bytes on");
	Check(!AsksAfterSpotting(1000, 1000) && !AsksAfterSpotting(1000, 999) &&
	          !AsksAfterSpotting(1000, 1000 + SPOTTED_CHECKS_WITHIN + 1),
	      "not the spotting itself, nothing before it, nothing far after");
}

void TestTannersClears() {
	Check(ClearSparesRiders(TWO_FACED_TANNER), "Two-Faced Tanner's clears spare a guest's car");
	Check(!ClearSparesRiders(SAYONARA_SALVATORE) && !ClearSparesRiders(27),
	      "nobody else's: Luigi's street and Joey's garage door are cleared as before (R4c)");
}

// Deal Steal's and Shima's casino: controls off, the owner's ped told out of
// car 0x1A01, then `while is_player_in_car` it.
void TestWalkedOut() {
	WalkedOut w{};
	Check(!InCarIsOwnersAlone(w, 0x1A01), "before the scene a participant in the car answers it");
	NoteLeaveCar(w, false, 0x1A01);
	Check(!InCarIsOwnersAlone(w, 0x1A01), "Kanbu or the contact getting out changes nothing");
	NoteLeaveCar(w, true, 0x1A01);
	Check(InCarIsOwnersAlone(w, 0x1A01),
	      "the owner's own ped walked out: a frozen guest in the car no longer holds the loop");
	Check(!InCarIsOwnersAlone(w, 0x2B02), "another car is still anybody's");
	NoteControl(w, false);
	Check(InCarIsOwnersAlone(w, 0x1A01), "the scene taking the controls again keeps it");
	NoteControl(w, true);
	Check(!InCarIsOwnersAlone(w, 0x1A01), "the controls back: the scene is over");
	NoteLeaveCar(w, true, 0);
	Check(InCarIsOwnersAlone(w, 0), "a car whose handle is 0 is still remembered");
}

void TestKenjisTargets() {
	using namespace coopiii::game::nearchar;
	Check(AnswersForNearest(SHIMA) && AnswersForNearest(SMACK_DOWN),
	      "Shima's gunman and Smack Down's dealers are near whoever is nearest them");
	Check(!AnswersForNearest(49) && !AnswersForNearest(50) && !AnswersForNearest(51),
	      "Kanbu and Deal Steal's contact follow the owner; their locates stay his");

	Check(AnswersPlaceForAnybody(SMACK_DOWN, 0x00E3, 39.25f, -880.5625f, 90.0f, 90.0f) &&
	          AnswersPlaceForAnybody(SMACK_DOWN, 0x00E3, -12.0f, -300.0f, 90.0f, 90.0f),
	      "a dealer is made for anybody within 90 m of his marker, wherever it was put");
	Check(!AnswersPlaceForAnybody(SMACK_DOWN, 0x00F5, 39.25f, -880.5625f, 90.0f, 90.0f) &&
	          !AnswersPlaceForAnybody(SMACK_DOWN, 0x00E3, 39.25f, -880.5625f, 10.0f, 10.0f),
	      "nothing else in Smack Down is");
	Check(AnswersPlaceForAnybody(SHIMA, 0x00F5, 940.0f, -185.0f, 25.0f, 25.0f),
	      "the Diablos turn on anybody within 25 m");
	Check(!AnswersPlaceForAnybody(SHIMA, 0x00F8, 452.25f, -1465.75f, 4.0f, 4.0f) &&
	          !AnswersPlaceForAnybody(SHIMA, 0x00F5, 452.25f, -1465.75f, 25.0f, 25.0f),
	      "the casino and the store stay checkpoints");
	Check(!AnswersPlaceForAnybody(51, 0x00E3, 231.0625f, -26.25f, 10.0f, 10.0f) &&
	          !AnswersPlaceForAnybody(36, 0x00E3, 39.25f, -880.5625f, 90.0f, 90.0f),
	      "Deal Steal's rendezvous is a stealth check, not this; no other mission is");
	Check(InLocateBox(0.0f, 25.0f, -10.0f, 25.0f, 25.0f, 10.0f, true) &&
	          !InLocateBox(0.0f, 0.0f, 10.5f, 25.0f, 25.0f, 10.0f, true),
	      "the 3D box asks the height");
}

} // namespace

int RunStandInTests() {
	g_failed = 0;
	TestEscort();
	TestHeldRespray();
	TestCarAtThePlace();
	TestEvidence();
	TestSitHint();
	TestStaunton();
	TestRoundUp();
	TestSalvatoresGarage();
	TestWhoTheGuardsSpotted();
	TestTannersClears();
	TestWalkedOut();
	TestKenjisTargets();
	if (g_failed == 0)
		std::printf("standin: ok\n");
	return g_failed;
}
