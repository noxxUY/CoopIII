// Who stands in for "the player" in a condition of the owner's mission, for
// the missions where the owner's own answer softlocks or fails a step a
// participant does (docs/mission-audit.md, §3, Marty Chonks and Luigi, and
// Donald Love and King Courtney in Staunton).
// The rules are pure and tested in tools/clienttest/standin.cpp; mission.cpp
// asks them.
//
// Three rules, each only ever widening an answer to yes:
//
//   - **Escort.** LOCATE_PLAYER_*_CHAR (00E9..00EB, 00FB..00FD) about one of
//     the mission's pedestrians is yes for a participant who sits in one of
//     the mission's cars, or in the car the pedestrian sits in, inside the
//     box. Marty's four jobs ask it twice: the pick-up ("in a car within 8 m
//     of the bank manager", then IS_PLAYER_IN_CAR on Marty's car) and "You
//     have left the Bank Manager behind!" at 30 m, which failed the mission
//     whenever a guest drove the passenger and the owner followed further
//     back. Only for the missions in EscortMission: Drive Misty For Me and
//     Luigi's Girls take Misty to the car the owner stored, so a guest beside
//     her must not answer for him.
//   - **A respray in a block.** HAS_RESPRAY_HAPPENED is only ever asked inside
//     an `if or` with IS_PLAYER_IN_CAR and the owner stopped in the Pay'n'Spray
//     (The Thieves, Don't Spank Ma Bitch Up), where the single-condition
//     widening never reached it. A participant's respray answers it there too,
//     and is only taken when the block went the way that yes pushed it
//     (HeldAnswerSpent), so a frame where the rest of the block still says no
//     does not lose the respray, as asking it in single player would.
//   - **The car at the place.** A location check of the owner, asked in the
//     same block after IS_PLAYER_IN_CAR was answered for a participant in the
//     car, is yes when that car stands at the place (5 m of slack, stopped
//     where the check asks for stopped): one state of the world, the car with
//     a player in it at the place, satisfies both. The mirror of
//     InCarAtThePlace (mission.h), which covers a location asked first.
//
// And one exception to R4b, for Kenji's casino (mission-audit.md, §3, Kenji
// step by step): **the car the owner was walked out of.** Deal Steal and Shima
// end with a scene at the casino: controls off (SET_PLAYER_CONTROL, which
// every participant's machine runs too), the owner's own ped (GET_PLAYER_CHAR)
// told to LEAVE_CAR the car he came in, then `while is_player_in_car` that
// car. Widened for anybody in it, a participant riding or driving with the
// owner, frozen in his seat by the same scene, held the loop for ever. That
// car's IS_PLAYER_IN_CAR is the owner's own answer until the script gives
// him his controls back.
// One more, for Ray's Evidence Dash: a participant on one of the files the
// prosecution's car drops picks it up (IsEvidenceLocate).
//
// And one hint: Give Me Liberty's arrival at the hideout asks
// IS_PLAYER_SITTING_IN_CAR of the owner, which is not widened (the scene after
// it walks the owner's own ped out of the Kuruma). With a guest driving the
// Kuruma there and the owner elsewhere the mission waited with nothing on the
// owner's screen; it now tells him to get in.
//
// Staunton, Love's and Courtney's missions (mission-audit.md §3, "Donald Love
// and King Courtney in Staunton, step by step"), read off the retail main.scm:
//
//   - **Anybody at the place.** Liberator's garage doors and compound and
//     Bling-Bling Scramble's checkpoints are in nearchar.h's table of places
//     anybody answers; what each asks besides the place (on foot, in a car)
//     is PlaceNeedsOf's, and whether a participant is it PlayerAnswersPlace.
//   - **Anybody in the model.** Liberator's IS_PLAYER_IN_MODEL, both single:
//     the wait for a Colombian car and the checkpoint it puts back. A guest
//     who 'jacked the car is in one, and the owner follows him in through
//     the gate (game/gates.h opens it for anybody's own Colombian car).
//   - **A guest's gang car.** Gangcar Round-Up asks IS_PLAYER_IN_ANY_CAR,
//     IS_PLAYER_IN_MODEL of each of the three gang models and then
//     STORE_CAR_PLAYER_IS_IN, all single, and hands the stored car to the
//     lock-up. With the owner not in a gang car himself, those three are
//     answered for one participant who sits in one (RoundUpSubject), the
//     car he sits in is the one stored, and the lock-up takes it from him
//     (R5). Before, a guest alone in a Mafia Sentinel did nothing.
//
// Asuka's Staunton missions add three more (docs/mission-audit.md, §3, Asuka
// in Staunton):
//
//   - **A box anybody keeps.** Sayonara Salvatore opens Salvatore's garage for
//     the convoy, shuts it once Salvatore is inside and the player is not,
//     opens it again while the player is inside, and fails the mission when
//     the door is down. Asked of the owner alone, the door came down on a
//     guest standing in the garage: the mission failed and the guest was
//     shut in for good, the garage left closed. Either check of that box is
//     yes for any participant inside it, so it shuts only on nobody.
//   - **Who the guards spotted.** The same mission asks HAS_CHAR_SPOTTED_PLAYER
//     of each guard (widened to any participant the guard can see, R13) and
//     then, in two `if`s of their own, whether the player is out of two
//     places where being seen does not count. Those were the owner's places:
//     a guest seen in one of them gave the game away, and a guest seen in the
//     open was let off whenever the owner stood in one. The two checks right
//     after a spotting now ask about whoever was spotted (SpottedStandIn).
//   - **Clears that spare a rider.** Two-Faced Tanner clears 20 m round
//     Tanner's car twice, as the scene starts and as the chase does. The
//     start is a checkpoint, so everybody stops inside that circle, and R4c's
//     clear put every guest out of his car and took it away a second before
//     the chase. There a car a participant sits in is spared, as the owner's is.
//
// Shoreside Vale adds four (mission-audit.md §3, "Shoreside Vale, step by
// step"): Escort Service's stash garage is a box anybody keeps, as
// Salvatore's is; S.A.M.'s cargo is picked up by a participant on it, as
// Evidence Dash's files are; S.A.M.'s boats are anybody's model; and Decoy's
// end is asked of whoever drives the decoy van (DecoyRider).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "nearchar.h"

namespace coopiii::game::standin {

// ---- escort ------------------------------------------------------------------------

constexpr uint16_t THE_CROOK       = 15;
constexpr uint16_t HER_LOVER       = 18;
constexpr uint16_t GIVE_ME_LIBERTY = 19;

// Marty Chonks' four payphone jobs: a passenger picked up in Marty's car and
// driven to the factory, with a left-behind check on the way.
inline bool EscortMission(uint16_t missionNumber) {
	return missionNumber >= THE_CROOK && missionNumber <= HER_LOVER;
}

enum class Means : uint8_t { Any, OnFoot, InCar };

struct CharLocate {
	bool  is3d  = false;
	Means means = Means::Any;
};

// The player-near-a-pedestrian locates, and what each asks.
inline bool CharLocateOf(int32_t command, CharLocate *out) {
	switch (command) {
	case 0x00E9: *out = {false, Means::Any}; return true;
	case 0x00EA: *out = {false, Means::OnFoot}; return true;
	case 0x00EB: *out = {false, Means::InCar}; return true;
	case 0x00FB: *out = {true, Means::Any}; return true;
	case 0x00FC: *out = {true, Means::OnFoot}; return true;
	case 0x00FD: *out = {true, Means::InCar}; return true;
	default: return false;
	}
}

// The engine's own box (re3 LocatePlayerCharCommand): inclusive, around the
// pedestrian, or the car he sits in.
inline bool InCharBox(float dx, float dy, float dz, float rx, float ry, float rz, bool is3d) {
	return std::fabs(dx) <= rx && std::fabs(dy) <= ry && (!is3d || std::fabs(dz) <= rz);
}

// A participant as the owner's machine sees him beside the pedestrian.
struct Escort {
	float dx = 0.0f, dy = 0.0f, dz = 0.0f;   // his car from the pedestrian (or his car)
	bool  seated       = false;              // his copy sits in a car
	bool  inMissionCar = false;              // that car is one the mission made
	bool  withChar     = false;              // the pedestrian sits in it too
	bool  valid        = false;              // in the owner's mission, alive
};

// Who answers the locate: the nearest participant inside the box who sits in
// one of the mission's cars or beside the pedestrian, or -1. Nobody for an
// on-foot locate: the rule is about the car the passenger is going with.
inline int EscortFor(const Escort *e, size_t n, const CharLocate &l, float rx, float ry, float rz) {
	if (l.means == Means::OnFoot)
		return -1;
	int   best  = -1;
	float bestD = 0.0f;
	for (size_t i = 0; i < n; ++i) {
		if (!e[i].valid || !e[i].seated || !(e[i].inMissionCar || e[i].withChar) ||
		    !InCharBox(e[i].dx, e[i].dy, e[i].dz, rx, ry, rz, l.is3d))
			continue;
		const float d = e[i].dx * e[i].dx + e[i].dy * e[i].dy + e[i].dz * e[i].dz;
		if (best < 0 || d < bestD) {
			best  = static_cast<int>(i);
			bestD = d;
		}
	}
	return best;
}

// ---- an answer held until its block is decided -------------------------------------

enum class Block : uint8_t { Single, And, Or, Other };

// The and/or counter as it was before the condition ran.
inline Block BlockOf(uint16_t andOrBefore) {
	if (andOrBefore == 0)
		return Block::Single;
	if (andOrBefore >= 1 && andOrBefore <= 8)
		return Block::And;
	if (andOrBefore >= 21 && andOrBefore <= 28)
		return Block::Or;
	return Block::Other;
}

// Whether a yes given for somebody else's machine was spent by its block,
// `final` being the block's result at its goto_if_false. Kept only where the
// other conditions decided the block against it: an `if and` that came out
// false although this one said yes, an `if or` that came out true although
// this one, under its NOT, said no. There the mission did not move on, and
// the same respray answers the next ask.
inline bool HeldAnswerSpent(Block block, bool notFlag, bool final) {
	const bool said = !notFlag;   // what the yes left in the flag
	switch (block) {
	case Block::And: return !(said && !final);
	case Block::Or: return !(!said && final);
	default: return true;
	}
}

// ---- the car at the place ----------------------------------------------------------

struct PlaceNeeds {
	bool onFoot  = false;
	bool stopped = false;
	bool inCar   = false;
};

// What a location condition asks of the player besides where he is: on
// foot, in a car, or stopped. False for anything that is not one.
inline bool PlaceNeedsOf(int32_t command, PlaceNeeds *out) {
	// LOCATE_*_2D 00E3..00E8 and _3D 00F5..00FA: any means, on foot, in car,
	// then the three stopped.
	if ((command >= 0x00E3 && command <= 0x00E8) || (command >= 0x00F5 && command <= 0x00FA)) {
		const int k = (command >= 0x00F5 ? command - 0x00F5 : command - 0x00E3);
		*out = {k % 3 == 1, k >= 3, k % 3 == 2};
		return true;
	}
	if (command == 0x0056 || command == 0x0057) {
		*out = {false, false, false};
		return true;
	}
	// IS_PLAYER_IN_AREA_ON_FOOT_2D 0197 .. STOPPED_IN_AREA_IN_CAR_2D 019B, and
	// the 3D five 019C..01A0: on foot, in car, stopped, stopped on foot,
	// stopped in car.
	if (command >= 0x0197 && command <= 0x01A0) {
		const int k = (command - 0x0197) % 5;
		*out = {k == 0 || k == 3, k >= 2, k == 1 || k == 4};
		return true;
	}
	return false;
}

// Whether a car with a participant in it, at the place, answers it.
inline bool CarAnswersPlace(const PlaceNeeds &needs, bool carStopped) {
	return !needs.onFoot && (!needs.stopped || carStopped);
}

// A car that moves slower than this (m_vecMoveSpeed, metres per 1/50 s) is
// stopped: 3.6 km/h, as The Fuzz Ball's stop.
constexpr float STOPPED_SPEED = 0.02f;

inline bool CarStopped(float vx, float vy, float vz) {
	return vx * vx + vy * vy + vz * vz <= STOPPED_SPEED * STOPPED_SPEED;
}

// Whether a participant, seated or not, stopped or not, is what a place asks
// besides where he is.
inline bool PlayerAnswersPlace(const PlaceNeeds &needs, bool seated, bool stopped) {
	if (needs.onFoot && seated)
		return false;
	if (needs.inCar && !seated)
		return false;
	return !needs.stopped || stopped;
}

// Liberator's IS_PLAYER_IN_MODEL: both are single conditions about the car
// the group takes to the compound.
//
// And S.A.M.'s boats (mission-audit.md §3, "Shoreside Vale, step by step"):
// every IS_PLAYER_IN_MODEL there asks whether the player is in a Reefer, a
// Predator or a Speeder, three to a block (`if or` of the three, `if and` of
// their NOTs): the boat or the buoy marker, and, once the plane is down,
// whether the cargo is taken by a locate (in a boat) or by touching it. A
// guest in a boat answers them for the group, through each block's own flag,
// so the cargo locates run with the owner on the shore.
inline bool AnybodyInModel(uint16_t mission) {
	return mission == nearchar::LIBERATOR || mission == nearchar::SAM;
}

// ---- Gangcar Round-Up: a guest's gang car ------------------------------------------

constexpr uint16_t GANGCAR_ROUND_UP = 65;

// The three cars Courtney wants, by the models the retail script asks:
// #MAFIA (134), #YAKUZA (136) and #DIABLOS (137).
inline bool IsRoundUpModel(int32_t model) { return model == 134 || model == 136 || model == 137; }

// Who IS_PLAYER_IN_ANY_CAR, IS_PLAYER_IN_MODEL and STORE_CAR_PLAYER_IS_IN are
// answered for: -1 for the owner himself, when he sits in one of the three
// models (or nobody else does), otherwise the first participant who does.
// `carModels[i]` is the model of the car participant i sits in, -1 for none
// or for no participant.
inline int RoundUpSubject(int32_t ownerCarModel, const int32_t *carModels, size_t n) {
	if (IsRoundUpModel(ownerCarModel))
		return -1;
	for (size_t i = 0; i < n; ++i)
		if (IsRoundUpModel(carModels[i]))
			return static_cast<int>(i);
	return -1;

}

// ---- the car the owner was walked out of -------------------------------------------

struct WalkedOut {
	int32_t car  = 0;
	bool    held = false;
};

// LEAVE_CAR (01D3) from the owner's mission: only an order to the owner's own
// ped counts.
inline void NoteLeaveCar(WalkedOut &w, bool ownPlayer, int32_t car) {
	if (ownPlayer)
		w = WalkedOut{car, true};
}

// SET_PLAYER_CONTROL (01B4): the scene is over once the controls are back.
inline void NoteControl(WalkedOut &w, bool on) {
	if (on)
		w = WalkedOut{};
}

// Whether IS_PLAYER_IN_CAR on `car` stays the owner's own answer.
inline bool InCarIsOwnersAlone(const WalkedOut &w, int32_t car) { return w.held && w.car == car; }

// ---- Evidence Dash's files ---------------------------------------------------------
//
// The six files the prosecution's car drops are objects, not pickups, and
// the script collects one when its player stands on it: a locate of the
// player around where the owner's copy of the file lies, 3D 1.5 m once it
// has been down half a second, 2D 1.5 m after 10 s and 2D 30 m after two
// minutes (00F5 and 00E3 at $OBJECT_CURRENT_COORDS). Its only other locate of
// the player at a place is 2D 150 m round the car's start, which picks where
// the next car after a decoy is made. Anybody picks a file up: the
// collection deletes the object on every machine and counts for everybody,
// as a briefcase does (mission-audit.md R2). Every machine throws its own
// copy of a file into the air off its own copy of the car, so a participant's
// copy lands a little away from the owner's; EVIDENCE_SLACK is how far.
constexpr uint16_t EVIDENCE_DASH    = 56;
constexpr float    EVIDENCE_MAX_BOX = 30.0f;
constexpr float    EVIDENCE_SLACK   = 3.0f;

inline bool IsEvidenceLocate(uint16_t missionNumber, int32_t command, float rx, float ry) {
	return missionNumber == EVIDENCE_DASH && (command == 0x00E3 || command == 0x00F5) &&
	       rx <= EVIDENCE_MAX_BOX + 0.25f && ry <= EVIDENCE_MAX_BOX + 0.25f;
}

// A participant `d` from the owner's file is on it.
inline bool OnTheEvidence(float dx, float dy, float dz, float rx, float ry, float rz, bool is3d) {
	return InCharBox(dx, dy, dz, rx + EVIDENCE_SLACK, ry + EVIDENCE_SLACK, rz + EVIDENCE_SLACK, is3d);
}

// S.A.M.'s cargo the same way (mission-audit.md §3, "Shoreside Vale, step by
// step"): the eight packages the shot-down plane leaves are objects thrown
// onto the water (ADD_TO_OBJECT_VELOCITY, replayed), and with the player in a
// boat the script collects one when he comes within 4 m of where the owner's
// copy floats (00F5 4 x 4 x 4 at $CHARLIE_n_X, read off that copy each
// frame). Every machine's copy is thrown and drifts on its own water, so a
// guest's lands further from the owner's than a file off the Bobcat:
// CARGO_SLACK. Its other 00F5 is the 1 m on-foot stash at the end, not this.
constexpr uint16_t SAM         = nearchar::SAM;
constexpr float    CARGO_BOX   = 4.0f;
constexpr float    CARGO_SLACK = 4.0f;

inline bool IsCargoLocate(uint16_t missionNumber, int32_t command, float rx, float ry) {
	return missionNumber == SAM && command == 0x00F5 && std::fabs(rx - CARGO_BOX) <= 0.25f &&
	       std::fabs(ry - CARGO_BOX) <= 0.25f;
}

// The slack a participant on one of the mission's objects gets, or a negative
// number for a locate that is not about picking one up.
inline float CollectSlack(uint16_t missionNumber, int32_t command, float rx, float ry) {
	if (IsEvidenceLocate(missionNumber, command, rx, ry))
		return EVIDENCE_SLACK;
	if (IsCargoLocate(missionNumber, command, rx, ry))
		return CARGO_SLACK;
	return -1.0f;
}

inline bool OnTheObject(float dx, float dy, float dz, float rx, float ry, float rz, bool is3d, float slack) {
	return InCharBox(dx, dy, dz, rx + slack, ry + slack, rz + slack, is3d);
}

// ---- Decoy's end -------------------------------------------------------------------
//
// Decoy passes when the 3-minute clock runs out with "the player" more than
// 160 m from the warehouse (00E3 at -1026.5, -73.5), and fails with "You
// failed to lead the police far enough away!" otherwise. The decoy is the
// Securicar: anybody may drive it (R4b keeps the 15 s "return to the
// Securicar" countdown off while anybody is in it). With a guest driving it
// away and the owner left by the warehouse, the owner's own answer failed
// the mission the group had done; with the owner gone and a guest standing
// by the warehouse, the general rule would have failed it as well. The check
// is about the van: while a participant sits in it and the owner does not,
// it is answered for the van, and otherwise it is the owner's own.
constexpr uint16_t DECOY = 69;

inline bool IsDecoyEnd(uint16_t missionNumber, int32_t command, float x, float y, float rx, float ry) {
	return missionNumber == DECOY && command == 0x00E3 && nearchar::SameSpot(x, -1026.5f) &&
	       nearchar::SameSpot(y, -73.5f) && nearchar::SameSpot(rx, 160.0f) &&
	       nearchar::SameSpot(ry, 160.0f);
}

// Whom the end is about: -1 for the owner's own answer (he sits in the van,
// or no participant does), else the first participant in it.
inline int DecoyRider(bool ownerInVan, const bool *inVan, size_t n) {
	if (ownerInVan)
		return -1;
	for (size_t i = 0; i < n; ++i)
		if (inVan[i])
			return static_cast<int>(i);
	return -1;
}

// ---- the hideout hint --------------------------------------------------------------

constexpr uint32_t SIT_HINT_EVERY_MS = 6000;

inline bool SitHintDue(uint32_t lastMs, uint32_t nowMs) {
	return lastMs == 0 || nowMs - lastMs >= SIT_HINT_EVERY_MS;
}

// ---- Asuka in Staunton -------------------------------------------------------------

constexpr uint16_t SAYONARA_SALVATORE = 44;
constexpr uint16_t TWO_FACED_TANNER   = 48;

constexpr int32_t IS_PLAYER_IN_AREA_2D = 0x0056;
constexpr int32_t IS_PLAYER_IN_AREA_3D = 0x0057;

// An area the script names by its two corners, z left out of a 2D one.
struct Box {
	float x1, y1, z1, x2, y2, z2;
	bool  is3d;
};

// The script's corners are 1/16ths written as literals: the same corner
// written twice reads the same, and a half of a sixteenth is room enough.
inline bool SameCorner(float a, float b) {
	return std::fabs(a - b) <= 1.0f / 32.0f;
}

// Whether an area check's corners, as its operands give them after the
// player (x1 y1 x2 y2, or x1 y1 z1 x2 y2 z2), are `b`'s.
inline bool IsBox(const Box &b, const float *corners) {
	if (!b.is3d)
		return SameCorner(corners[0], b.x1) && SameCorner(corners[1], b.y1) &&
		       SameCorner(corners[2], b.x2) && SameCorner(corners[3], b.y2);
	return SameCorner(corners[0], b.x1) && SameCorner(corners[1], b.y1) && SameCorner(corners[2], b.z1) &&
	       SameCorner(corners[3], b.x2) && SameCorner(corners[4], b.y2) && SameCorner(corners[5], b.z2);
}

// The engine's own test (CPlaceable::IsWithinArea): inclusive, whichever way
// round the corners are.
inline bool InBox(const Box &b, float x, float y, float z) {
	const float lx = b.x1 < b.x2 ? b.x1 : b.x2, hx = b.x1 < b.x2 ? b.x2 : b.x1;
	const float ly = b.y1 < b.y2 ? b.y1 : b.y2, hy = b.y1 < b.y2 ? b.y2 : b.y1;
	if (x < lx || x > hx || y < ly || y > hy)
		return false;
	if (!b.is3d)
		return true;
	const float lz = b.z1 < b.z2 ? b.z1 : b.z2, hz = b.z1 < b.z2 ? b.z2 : b.z1;
	return z >= lz && z <= hz;
}

// Inside Salvatore's garage, as Sayonara Salvatore asks it (both checks:
// `not` in it to shut the door, in it to open it again).
constexpr Box SALVATORES_GARAGE_INSIDE = {1427.5625f, -187.25f, 49.5f, 1442.5625f, -179.0f, 53.75f, true};

// The area of an area check of the mission that any participant inside
// answers yes, as the owner inside would, or null.
// Escort Service's end the same way (mission-audit.md §3, "Shoreside Vale,
// step by step"): Love's stash garage shuts once the truck has stopped
// inside and "the player" is not in it (`not 0056`), opens again while he is,
// and the mission passes on the shut door. Asked of the owner alone, it came
// down on a guest who had followed the truck in, and the cleanup leaves it
// shut: the guest was shut in Love's garage for good.
constexpr uint16_t ESCORT_SERVICE = nearchar::ESCORT_SERVICE;
constexpr Box      LOVES_STASH_INSIDE = {-1049.125f, -77.4375f, 0.0f, -1037.1875f, -69.125f, 0.0f, false};

inline const Box *AreaAnybodyKeeps(uint16_t mission, int32_t command, const float *corners) {
	if (mission == SAYONARA_SALVATORE && command == IS_PLAYER_IN_AREA_3D &&
	    IsBox(SALVATORES_GARAGE_INSIDE, corners))
		return &SALVATORES_GARAGE_INSIDE;
	if (mission == ESCORT_SERVICE && command == IS_PLAYER_IN_AREA_2D && IsBox(LOVES_STASH_INSIDE, corners))
		return &LOVES_STASH_INSIDE;
	return nullptr;
}

// The two places where Luigi's guards seeing the player does not count:
// behind the club, and across the street.
constexpr Box SPOTTED_DOES_NOT_COUNT[2] = {
    {845.75f, -443.8125f, 0.0f, 890.75f, -433.8125f, 0.0f, false},
    {920.0625f, -408.8125f, 0.0f, 931.3125f, -398.0625f, 0.0f, false},
};

// Which of the two an area check is, or -1.
inline int SafeBoxOf(int32_t command, const float *corners) {
	if (command != IS_PLAYER_IN_AREA_2D)
		return -1;
	for (int i = 0; i < 2; ++i)
		if (IsBox(SPOTTED_DOES_NOT_COUNT[i], corners))
			return i;
	return -1;
}

inline bool InSafeBox(float x, float y) {
	return InBox(SPOTTED_DOES_NOT_COUNT[0], x, y, 0.0f) || InBox(SPOTTED_DOES_NOT_COUNT[1], x, y, 0.0f);
}

// A participant as the guard asking sees him.
struct Watched {
	bool valid = false;   // in the owner's mission, his copy here and up
	bool seen  = false;   // the guard can see him (R13's CanSee)
	bool safe  = false;   // he stands in one of the two places
};

// Whom the two checks after a spotting are about, or -1 for the owner's own
// answers: a participant seen out of the two places first, since that gives
// the game away whatever the owner is doing; else, with the owner not seen,
// one seen inside them, whose place then lets him off as it would the owner.
// With the owner seen in the open, it is the owner who gave it away.
inline int SpottedStandIn(bool ownerSeen, bool ownerSafe, const Watched *w, size_t n) {
	if (ownerSeen && !ownerSafe)
		return -1;
	int safeOne = -1;
	for (size_t i = 0; i < n; ++i) {
		if (!w[i].valid || !w[i].seen)
			continue;
		if (!w[i].safe)
			return static_cast<int>(i);
		if (safeOne < 0)
			safeOne = static_cast<int>(i);
	}
	return ownerSeen ? -1 : safeOne;
}

// How far after the spotting, in script bytes, its two place checks come:
// `goto_if_false`, `if`, the first check (19 bytes from the spotting's
// operands), the second's `goto_if_false` and `if`, the second (49). Every
// spotting in the mission is followed by the same two, and a later check of
// the same places (the guards' flanking after "You have been spotted!") is
// far beyond this.
constexpr uint32_t SPOTTED_CHECKS_WITHIN = 64;

inline bool AsksAfterSpotting(uint32_t spottedIp, uint32_t ip) {
	return ip > spottedIp && ip - spottedIp <= SPOTTED_CHECKS_WITHIN;
}

// Whether a CLEAR_AREA of the mission spares a session car a participant sits
// in, as the engine spares the owner's.
inline bool ClearSparesRiders(uint16_t mission) {
	return mission == TWO_FACED_TANNER;
}

} // namespace coopiii::game::standin
