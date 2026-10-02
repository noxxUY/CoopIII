// A locate against one of the mission's own pedestrians, answered for the
// player nearest him: the missions where "the player" near the target means
// whoever of the group is on him, not the owner alone.
//
// The engine's LOCATE_PLAYER_ANY_MEANS_CHAR_2D and _3D (00E9, 00FB) ask
// whether the player is inside a box round the pedestrian: |dx| <= rx and
// |dy| <= ry, and |dz| <= rz in 3D, the pedestrian's car standing for him
// when he sits in one (re3's LocatePlayerCharCommand). In the missions
// of the table below every such locate is about a target the whole group
// goes after, read off the retail main.scm:
//
//   25 Farewell 'Chunky' Lee Chong: `not 00FB` 25 m and 20 m hold the scene
//      before the fight back, and five `not 00E9 ... 160 m` fail it with
//      "He's clean out of here!". A helper on Chunky's heels with the owner
//      a block behind failed it.
//   41 I Scream, You Scream: eight 00FB 8 m, each a gang member turning on
//      "the player" (KILL_PLAYER, which goes for the nearest participant).
//      A helper walking into them was ignored.
//   43 Big'N'Veiny: 00E9 30 m against the thief stops the countdown. A
//      helper who found him first left the clock running out.
//   33 Triads And Tribulations: each warlord's guards come out at 80 m
//      (00E9), he runs at 30 m (in an `if and` beside a flag), and the Mafia
//      escort is let go of past 120 m. A guest at the fish factory alone met
//      a warlord with no guards who never moved.
//   36 Cutting The Grass: every locate is against Curly Bob, the
//      Spookometer's 40, 30 and 20 m and "Curly got away!"'s 160 m, which
//      sits in an `if and` with Curly off the owner's screen. The Mafia car
//      test after a locate is asked of the player nearest him
//      (game/mission.cpp, InModelOfNearest).
//   46 Paparazzi Purge: "He's clean out of here!" fails the mission at 160 m
//      from the spy on foot (two 00E9), as from his boat and his car below.
//   52 Shima: the Diablo who stands by the second briefcase in Belleville
//      Park turns on "the player" at 10 m (00E9, KILL_PLAYER). A guest who
//      went for that briefcase alone walked past a gunman who never moved.
//   53 Smack Down: a dealer is let go of, back to a marker where he stood,
//      once "the player" is more than 90 m from him (`not 00E9`). A guest
//      chasing a dealer with the owner 100 m off lost him mid-fight.
//   54 Silence The Sneak: both locates are against McAffrey, "McAffrey
//      escaped!" (`not 00E9 160 m` in an `if and` with him off the owner's
//      screen, then 4 s) and the `if or` that puts his marker back. A guest
//      on the getaway car with the owner a block behind lost him.
//   57 Gone Fishing: all nine against Ray's partner, his escape at 160 m (as
//      McAffrey's), how fast his car goes at 30 and 80 m, when he gets out at
//      120 m and steals a car at 100 m, and the 5 m inside which he drops no
//      mine. Whoever chases him.
//
// LOCATE_PLAYER_ANY_MEANS_CAR_2D and _3D (01FC, 01FF) against one of the
// mission's cars, the same way, in the missions of AnswersNearCarForNearest:
//
//   56 Evidence Dash: the prosecution's car speeds up the nearer the player
//      is (20, 50, 90 and 130 m, single conditions), and the photos are lost
//      when it sinks with the player within 50 m (`not 01FC`). A guest on
//      its tail with the owner far behind had it crawl, and sinking it
//      passed the mission instead of failing it.
//   57 Gone Fishing: the partner's boat goes 35 with the player within 80 m
//      and back to 25 past 100 m.
//   46 Paparazzi Purge: the spy boat makes a run for it once the player is
//      within 55 m (01FC, in an `if and` beside the boat being unhurt), and
//      "He's clean out of here!" fails the mission at 160 m from the boat
//      (twenty 01FC) and from the Stallion he swaps it for (three more). A
//      guest in the Predator on the boat's tail, with the owner in a boat of
//      his own further back, failed it.
//
// And in Shoreside Vale (mission-audit.md §3, "Shoreside Vale, step by
// step"), the on-foot and in-car forms (01FD, 01FE, 0200, 0201) as well:
//
//   67 Grand Theft Aero: "Track down the Colombians" waits for the player on
//      foot within 6 m of the Panlantic van parked by the hangar (`not
//      01FD`), and only the owner walking up to it moved the mission on.
//   68 Escort Service: the truck sets off once the player is in a car within
//      15 m of it (`not 01FE` in an `if or` with its spot on the owner's
//      screen), and "You'll need a car!" is the player on foot near it.
//
// The on-foot and in-car forms are answered alone or in an `if or` only
// (CarLocateMayWiden): Escort Service's `if and` of "on foot within 1 m of
// the truck" and IS_PLAYER_STOPPED would otherwise take a guest at the
// truck and the owner standing still somewhere else for one player.
//
// Each is a single condition or sits beside a flag of the script's own (or,
// once, beside Curly off the owner's screen, which only makes losing him
// harder, and once beside Paparazzi Purge's spy boat's own health), so one participant's
// state answers the whole block, which is
// what C1 (mission-audit.md) asks of a widened condition. The answer is only ever
// widened to yes, so the owner standing there still is. The Getaway's 30 m
// locates against its thugs are left alone: they tie a thug to the group's
// driver, whom getaway.h answers for.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coopiii::game::nearchar {

constexpr uint16_t FAREWELL_CHUNKY_LEE_CHONG = 25;
constexpr uint16_t I_SCREAM_YOU_SCREAM       = 41;
constexpr uint16_t BIG_N_VEINY               = 43;
constexpr uint16_t TRIADS_AND_TRIBULATIONS   = 33;
constexpr uint16_t CUTTING_THE_GRASS         = 36;
constexpr uint16_t PAPARAZZI_PURGE           = 46;
constexpr uint16_t SHIMA                     = 52;
constexpr uint16_t SMACK_DOWN                = 53;
constexpr uint16_t SILENCE_THE_SNEAK         = 54;
constexpr uint16_t EVIDENCE_DASH             = 56;
constexpr uint16_t GONE_FISHING              = 57;

inline bool AnswersForNearest(uint16_t mission) {
	return mission == FAREWELL_CHUNKY_LEE_CHONG || mission == I_SCREAM_YOU_SCREAM ||
	       mission == BIG_N_VEINY || mission == TRIADS_AND_TRIBULATIONS ||
	       mission == CUTTING_THE_GRASS || mission == PAPARAZZI_PURGE || mission == SHIMA ||
	       mission == SMACK_DOWN || mission == SILENCE_THE_SNEAK || mission == GONE_FISHING;
}

// And two locates at a place, not a pedestrian, that are the same question:
// "is anybody of the group near the targets yet", answered yes for a
// participant inside the box, and never a checkpoint everybody must reach.
//
//   53 Smack Down: each dealer is made when the player comes within 90 m of
//      his marker (00E3, at the marker's own variables). The marker is a
//      coordinate blip, so the locate also read as a checkpoint (C1), and no
//      dealer was made until every participant stood within 95 m of him: two
//      players hunting apart saw none, and the 80 s clock took them off the
//      streets unmet.
//   52 Shima: the five Diablos turn on the player within 25 m of (940, -185)
//      (00F5). A guest who got there first fought five men who stood still.
//
// Both are single conditions, so one participant's state answers the block.
//
// Shoreside Vale has the same question four more times (mission-audit.md §3,
// "Shoreside Vale, step by step"), each a single condition whose yes only
// sets enemies on the group:
//
//   71 Bait: each cartel car waits by its marker and sets off after "the
//      player" within 30 m of A, 40 m of B or 30 m of D (00E3, 00E3, 00F5),
//      ramming whoever is nearest (RetargetRam); D's car is only made once
//      the player is within 70 m (00F5). The markers are coordinate blips,
//      so each read as a checkpoint as well: a guest who drew a car to the
//      trap alone waited for the whole group to stand by its marker first.
//   73 S.A.M.: the Colombians on the docks by the runway turn on "the
//      player" within 60 m and 80 m of their three spots (00E3).
//   78 Rumble: the nine Nines turn on the player once he is in the fighting
//      ground (0057), and the contact leaves him to fight.
//   79 The Exchange: the guards round the helipad turn on the player in its
//      box (0057).
constexpr int32_t OP_LOCATE_ANY_MEANS_2D = 0x00E3;
constexpr int32_t OP_LOCATE_ANY_MEANS_3D = 0x00F5;

constexpr uint16_t BAIT         = 71;
constexpr uint16_t SAM          = 73;
constexpr uint16_t RUMBLE       = 78;
constexpr uint16_t THE_EXCHANGE = 79;

// The script writes a coordinate as a literal in 1/16ths (or as a global set
// from one): the same coordinate written twice reads the same.
inline bool SameSpot(float a, float b) { return std::fabs(a - b) <= 1.0f / 32.0f; }

struct Spot {
	float x, y, r;
};

inline bool AtSpot(const Spot &s, float x, float y, float rx, float ry) {
	return SameSpot(s.x, x) && SameSpot(s.y, y) && SameSpot(s.r, rx) && SameSpot(s.r, ry);
}

constexpr Spot BAIT_CARTEL_2D[] = {{-996.8125f, -247.5f, 30.0f}, {-877.0f, 562.0f, 40.0f}};
constexpr Spot BAIT_CARTEL_3D[] = {{-459.0f, 251.5f, 30.0f}, {-459.0f, 251.5f, 70.0f}};
constexpr Spot SAM_DOCKS[]      = {
    {-1019.0f, -1263.0f, 60.0f}, {-1385.25f, -1035.0f, 80.0f}, {-1478.25f, -1062.75f, 80.0f}};

template <size_t N>
inline bool AtAnySpot(const Spot (&spots)[N], float x, float y, float rx, float ry) {
	for (const Spot &s : spots)
		if (AtSpot(s, x, y, rx, ry))
			return true;
	return false;
}

inline bool AnswersPlaceForAnybody(uint16_t mission, int32_t command, float x, float y, float rx,
                                   float ry) {
	if (mission == SMACK_DOWN)
		return command == OP_LOCATE_ANY_MEANS_2D && rx == 90.0f && ry == 90.0f;
	if (mission == SHIMA)
		return command == OP_LOCATE_ANY_MEANS_3D && rx == 25.0f && ry == 25.0f &&
		       std::fabs(x - 940.0f) <= 1.0f && std::fabs(y + 185.0f) <= 1.0f;
	if (mission == BAIT)
		return command == OP_LOCATE_ANY_MEANS_2D ? AtAnySpot(BAIT_CARTEL_2D, x, y, rx, ry)
		                                         : command == OP_LOCATE_ANY_MEANS_3D &&
		                                               AtAnySpot(BAIT_CARTEL_3D, x, y, rx, ry);
	if (mission == SAM)
		return command == OP_LOCATE_ANY_MEANS_2D && AtAnySpot(SAM_DOCKS, x, y, rx, ry);
	return false;
}

// An area's two corners, as IS_PLAYER_IN_AREA_3D names them.
struct Area3D {
	float x1, y1, z1, x2, y2, z2;
};

constexpr Area3D RUMBLE_GROUND        = {-247.25f, 333.875f, 2.0f, -209.5f, 250.1875f, 15.0f};
constexpr Area3D THE_EXCHANGE_HELIPAD = {-1142.0f, 327.75f, 29.0f, -1215.5625f, 368.375f, 40.0f};

inline bool IsArea(const Area3D &a, const float *c) {
	return SameSpot(a.x1, c[0]) && SameSpot(a.y1, c[1]) && SameSpot(a.z1, c[2]) &&
	       SameSpot(a.x2, c[3]) && SameSpot(a.y2, c[4]) && SameSpot(a.z2, c[5]);
}

// And three more in Staunton, the same question in another form: "is
// anybody of the group there yet", each a single condition whose yes only
// opens a door or scores for the group (mission-audit.md §3, "Donald Love
// and King Courtney in Staunton"):
//
//   60 Liberator: each of the five garages opens for the player on foot in
//      front of its door (019C; every 019C in the mission is one of them),
//      and the compound, `is_player_in_area_3d 31 -317 14 to 91 -394 25`
//      (0057), sends the guards to their posts. Only the owner could open the
//      garage the Old Oriental Gentleman is in. The other 0057, the box at
//      the gate, only says the gate wants a Colombian car, and stays his.
//   63 Bling-Bling Scramble: the checkpoint ahead (00E5, in a car). A guest's
//      car first there scores it for the group, as the owner's would; before,
//      a racer took the point.
//
// What each asks besides the place (on foot, in a car) is the engine's own,
// from the command (standin.h, PlaceNeedsOf).
constexpr uint16_t LIBERATOR            = 60;
constexpr uint16_t BLING_BLING_SCRAMBLE = 63;

constexpr int32_t OP_IS_PLAYER_IN_AREA_3D         = 0x0057;
constexpr int32_t OP_LOCATE_IN_CAR_2D             = 0x00E5;
constexpr int32_t OP_IS_PLAYER_IN_AREA_ON_FOOT_3D = 0x019C;

inline bool IsLiberatorCompound(float x1, float y1, float z1, float x2, float y2, float z2) {
	auto near = [](float a, float b) { return std::fabs(a - b) < 0.25f; };
	return near(x1, 31.0f) && near(y1, -317.0f) && near(z1, 14.0f) && near(x2, 91.0f) &&
	       near(y2, -394.0f) && near(z2, 25.0f);
}

// The whole table, from the operands as CollectParameters left them (the
// player first): a locate's point and box, or an area's two corners.
inline bool AnswersPlaceForAnybody(uint16_t mission, int32_t command, const float *p) {
	if (mission == LIBERATOR)
		return command == OP_IS_PLAYER_IN_AREA_ON_FOOT_3D ||
		       (command == OP_IS_PLAYER_IN_AREA_3D &&
		        IsLiberatorCompound(p[1], p[2], p[3], p[4], p[5], p[6]));
	if (mission == BLING_BLING_SCRAMBLE)
		return command == OP_LOCATE_IN_CAR_2D;
	if (mission == RUMBLE)
		return command == OP_IS_PLAYER_IN_AREA_3D && IsArea(RUMBLE_GROUND, p + 1);
	if (mission == THE_EXCHANGE)
		return command == OP_IS_PLAYER_IN_AREA_3D && IsArea(THE_EXCHANGE_HELIPAD, p + 1);
	if (command == OP_LOCATE_ANY_MEANS_2D)
		return AnswersPlaceForAnybody(mission, command, p[1], p[2], p[3], p[4]);
	if (command == OP_LOCATE_ANY_MEANS_3D)
		return AnswersPlaceForAnybody(mission, command, p[1], p[2], p[4], p[5]);
	return false;
}

// The player near one of the mission's cars (01FC..0201).
constexpr uint16_t GRAND_THEFT_AERO = 67;
constexpr uint16_t ESCORT_SERVICE   = 68;

inline bool AnswersNearCarForNearest(uint16_t mission) {
	return mission == EVIDENCE_DASH || mission == GONE_FISHING || mission == PAPARAZZI_PURGE ||
	       mission == GRAND_THEFT_AERO || mission == ESCORT_SERVICE;
}

constexpr int32_t OP_LOCATE_PLAYER_ANY_MEANS_CAR_2D = 0x01FC;
constexpr int32_t OP_LOCATE_PLAYER_ON_FOOT_CAR_2D   = 0x01FD;
constexpr int32_t OP_LOCATE_PLAYER_IN_CAR_CAR_2D    = 0x01FE;
constexpr int32_t OP_LOCATE_PLAYER_ANY_MEANS_CAR_3D = 0x01FF;
constexpr int32_t OP_LOCATE_PLAYER_ON_FOOT_CAR_3D   = 0x0200;
constexpr int32_t OP_LOCATE_PLAYER_IN_CAR_CAR_3D    = 0x0201;

enum class CarMeans : uint8_t { Any, OnFoot, InCar };

struct CarLocate {
	bool     is3d  = false;
	CarMeans means = CarMeans::Any;
};

// The player-near-a-car locates, and what each asks of him.
inline bool CarLocateOf(int32_t command, CarLocate *out) {
	switch (command) {
	case OP_LOCATE_PLAYER_ANY_MEANS_CAR_2D: *out = {false, CarMeans::Any}; return true;
	case OP_LOCATE_PLAYER_ON_FOOT_CAR_2D: *out = {false, CarMeans::OnFoot}; return true;
	case OP_LOCATE_PLAYER_IN_CAR_CAR_2D: *out = {false, CarMeans::InCar}; return true;
	case OP_LOCATE_PLAYER_ANY_MEANS_CAR_3D: *out = {true, CarMeans::Any}; return true;
	case OP_LOCATE_PLAYER_ON_FOOT_CAR_3D: *out = {true, CarMeans::OnFoot}; return true;
	case OP_LOCATE_PLAYER_IN_CAR_CAR_3D: *out = {true, CarMeans::InCar}; return true;
	default: return false;
	}
}

// Whether a participant's answer may stand for the block (the and/or counter
// before the condition): any means in any block, as the missions above ask
// it; on foot or in a car alone or in an `if or` only.
inline bool CarLocateMayWiden(const CarLocate &l, uint16_t andOrBefore) {
	if (l.means == CarMeans::Any)
		return true;
	return andOrBefore == 0 || (andOrBefore >= 21 && andOrBefore <= 28);
}

// Whether a participant, seated or not, is what the locate asks.
inline bool MeansFits(CarMeans means, bool seated) {
	return means == CarMeans::Any || (means == CarMeans::InCar) == seated;
}

// The engine's box: inclusive at both ends. `in3d` false leaves the height
// out, as the 2D locate does.
inline bool InLocateBox(float dx, float dy, float dz, float rx, float ry, float rz, bool in3d) {
	return std::fabs(dx) <= rx && std::fabs(dy) <= ry && (!in3d || std::fabs(dz) <= rz);
}

} // namespace coopiii::game::nearchar
