// Where "there" is, for a mission's start and its checkpoints.
//
// The server decides a start (it grants the session's one mission only when
// everybody is at it), and the owner's machine decides a checkpoint (it has
// every player's position already). Both ask the same question, so they
// share the answer: inside the area, or no more than the session's margin
// outside it, 5 m by default (docs/missions.md 5.6 and 9).
//
// The engine's locates are axis-aligned boxes, a centre and a half-size on
// each axis, so "no more than 5 m outside" is the box grown by 5 m on every
// side. At a corner that reaches a little further than a circle would, which
// only ever lets a friend count who is standing next to the owner anyway.
#pragma once

#include "protocol.h"

#include <cmath>
#include <cstdint>

namespace coopiii {

// One player, as far as a mission's start or checkpoint cares: where their
// own machine last said they were.
struct MissionPresence {
	uint8_t playerId = INVALID_PLAYER;
	bool    havePos  = false;   // nothing has said where they are yet
	Vec3    pos      = {};
	// Their game is in a mission of its own, a new game's intro say: nowhere,
	// whatever havePos says (docs/missions.md 11.6).
	bool    busy     = false;
};

inline uint8_t PlayerBit(uint8_t playerId) {
	return playerId < MAX_PLAYERS ? static_cast<uint8_t>(1u << playerId) : 0;
}

// Inside `a` grown by `marginM` on every side. A 2D box ignores height. An
// owner-shaped area is the margin around its centre, height included, so a
// friend on the bridge overhead is not beside the owner. A position with a
// NaN in it is nowhere.
inline bool InMissionArea(const Vec3 &p, const MissionArea &a, float marginM) {
	if (!(marginM >= 0.0f))
		marginM = 0.0f;
	const bool  owner = a.shape == MISSION_AREA_OWNER;
	const float hx    = (owner ? 0.0f : std::fabs(a.half.x)) + marginM;
	const float hy    = (owner ? 0.0f : std::fabs(a.half.y)) + marginM;
	if (!(std::fabs(p.x - a.centre.x) <= hx) || !(std::fabs(p.y - a.centre.y) <= hy))
		return false;
	if (a.shape == MISSION_AREA_BOX2D)
		return true;
	const float hz = (owner ? 0.0f : std::fabs(a.half.z)) + marginM;
	return std::fabs(p.z - a.centre.z) <= hz;
}

// The areas the engine's own conditions describe, as their operands give
// them: a locate is a centre and a radius on each axis, an IS_*_IN_AREA two
// opposite corners in either order.
inline MissionArea MissionAreaLocate2D(float x, float y, float rx, float ry) {
	MissionArea a{};
	a.centre = {x, y, 0.0f};
	a.half   = {std::fabs(rx), std::fabs(ry), 0.0f};
	a.shape  = MISSION_AREA_BOX2D;
	return a;
}

inline MissionArea MissionAreaLocate3D(float x, float y, float z, float rx, float ry, float rz) {
	MissionArea a{};
	a.centre = {x, y, z};
	a.half   = {std::fabs(rx), std::fabs(ry), std::fabs(rz)};
	a.shape  = MISSION_AREA_BOX3D;
	return a;
}

inline MissionArea MissionAreaCorners2D(float x1, float y1, float x2, float y2) {
	return MissionAreaLocate2D((x1 + x2) * 0.5f, (y1 + y2) * 0.5f, (x2 - x1) * 0.5f,
	                           (y2 - y1) * 0.5f);
}

inline MissionArea MissionAreaCorners3D(float x1, float y1, float z1, float x2, float y2,
                                        float z2) {
	return MissionAreaLocate3D((x1 + x2) * 0.5f, (y1 + y2) * 0.5f, (z1 + z2) * 0.5f,
	                           (x2 - x1) * 0.5f, (y2 - y1) * 0.5f, (z2 - z1) * 0.5f);
}

inline MissionArea MissionAreaAround(const Vec3 &owner) {
	MissionArea a{};
	a.centre = owner;
	a.shape  = MISSION_AREA_OWNER;
	return a;
}

inline float MarginMetres(uint16_t marginCm) { return static_cast<float>(marginCm) / 100.0f; }

// How far from a mission's start a player still counts as at it when it
// launches: 50 m outside the start's area (docs/missions.md 5.6,
// protocol.md 1.55). A friend in his own car across the street, or on foot
// round the corner from the door, is in; one a block away is waited for. It
// is the start's only: a checkpoint keeps the session's margin. A server
// margin wider than this wins.
constexpr float MISSION_START_RADIUS_M = 50.0f;

inline float StartMarginMetres(uint16_t marginCm) {
	const float m = MarginMetres(marginCm);
	return m > MISSION_START_RADIUS_M ? m : MISSION_START_RADIUS_M;
}

// Whether a contact's marker at (x, y) is the one a locate's `area` is
// about. The marker and the locate are the same spot written twice in
// main.scm, not always to the same figure (Toni's is 1.3 m off his locate's
// centre, Asuka's 4 m along a box 4.5 m long), and the marker's height is
// often -100 for the ground to be found, so this looks across the map only,
// with `slackM` round the box.
inline bool AreaHoldsMarker(const MissionArea &area, float x, float y, float slackM) {
	if (area.shape == MISSION_AREA_OWNER)
		return false;
	MissionArea flat = area;
	flat.shape       = MISSION_AREA_BOX2D;
	return InMissionArea(Vec3{x, y, 0.0f}, flat, slackM);
}

// The 80 missions in main.scm's mission table, by START_MISSION's operand, as
// the game titles them: for the server's log and the HUD's "bob started ...".
constexpr uint16_t MISSION_COUNT = 80;

// Paramedic, Firefighter, Vigilante and Taxi Driver: a shift that ends rather
// than a mission that is passed. None of the four registers a pass.
inline bool IsOddJob(uint16_t number) { return number >= 11 && number <= 14; }

// Which kind of start a mission has (MissionKind): the four RC runs, the three
// 4x4 runs and Multistorey Mayhem, the odd jobs, and every other one a contact
// or a payphone's.
inline uint8_t MissionKindOf(uint16_t number) {
	if (IsOddJob(number))
		return MISSION_KIND_ODDJOB;
	if (number >= 3 && number <= 6)
		return MISSION_KIND_RC;
	if (number >= 7 && number <= 10)
		return MISSION_KIND_4X4;
	return MISSION_KIND_STORY;
}

// The two story missions that are races against the game's own drivers:
// Turismo (DIABLO1) and Bling-Bling Scramble (YARD1). Neither puts a timer
// on the screen, and the rivals drive on whoever is waited for.
inline bool IsRaceMission(uint16_t number) { return number == 40 || number == 63; }

// The one mission whose players are meant to split up: The Fuzz Ball (23),
// where each drives his own car round the girls (client game/fuzzball.h).
// Nobody there is brought to the owner for being far behind; he is
// somewhere else on purpose.
inline bool PlayersSplitUp(uint16_t number) { return number == 23; }

// Whether a checkpoint of the mission `number` waits for every participant
// (docs/missions.md 5.6). Not while a countdown is on the screen, and never
// in a race, an odd job or the RC, 4x4 and Mayhem runs: the clock and the
// rivals go on while anybody waits, so there the owner's arrival is the
// checkpoint, and whoever falls far behind is brought along instead
// (MISSION_BEHIND_*).
inline bool CheckpointsWait(uint16_t number, bool timerUp) {
	return !timerUp && MissionKindOf(number) == MISSION_KIND_STORY && !IsRaceMission(number);
}

// The same with the server's say in it (S_MissionState): no checkpoint waits
// when its wait is 0 seconds, and every one does, clock or race, when the
// server turned MISSION_FLAG_TIMED_CHECKPOINTS on.
inline bool CheckpointsWait(uint16_t number, bool timerUp, uint8_t missionFlags,
                            uint16_t checkpointWaitS) {
	if (checkpointWaitS == 0)
		return false;
	return (missionFlags & MISSION_FLAG_TIMED_CHECKPOINTS) != 0 ||
	       CheckpointsWait(number, timerUp);
}

inline const char *MissionName(uint16_t number) {
	static const char *const kNames[MISSION_COUNT] = {
	    "Intro Movie",
	    "Hospital Info Scene",
	    "Police Station Info Scene",
	    "RC Diablo Destruction",
	    "RC Mafia Massacre",
	    "RC Rumpo Rampage",
	    "RC Casino Calamity",
	    "Patriot Playground",
	    "A Ride In The Park",
	    "Gripped!",
	    "Multistorey Mayhem",
	    "Paramedic",
	    "Firefighter",
	    "Vigilante",
	    "Taxi Driver",
	    "The Crook",
	    "The Thieves",
	    "The Wife",
	    "Her Lover",
	    // EIGHT, one script for both: GIVE ME LIBERTY is its first title, and
	    // LUIGI'S GIRLS only comes once 8-Ball is at the hideout.
	    "Give Me Liberty",
	    "Don't Spank Ma Bitch Up",
	    "Drive Misty For Me",
	    "Pump-Action Pimp",
	    "The Fuzz Ball",
	    "Mike Lips Last Lunch",
	    "Farewell 'Chunky' Lee Chong",
	    "Van Heist",
	    "Cipriani's Chauffeur",
	    "Dead Skunk In The Trunk",
	    "The Getaway",
	    "Taking Out The Laundry",
	    "The Pick-Up",
	    "Salvatore's Called A Meeting",
	    "Triads And Tribulations",
	    "Blow Fish",
	    "Chaperone",
	    "Cutting The Grass",
	    "Bomb Da Base: Act I",
	    "Bomb Da Base: Act II",
	    "Last Requests",
	    "Turismo",
	    "I Scream, You Scream",
	    "Trial By Fire",
	    "Big'N'Veiny",
	    "Sayonara Salvatore",
	    "Under Surveillance",
	    "Paparazzi Purge",
	    "Payday For Ray",
	    "Two-Faced Tanner",
	    "Kanbu Bust-Out",
	    "Grand Theft Auto",
	    "Deal Steal",
	    "Shima",
	    "Smack Down",
	    "Silence The Sneak",
	    "Arms Shortage",
	    "Evidence Dash",
	    "Gone Fishing",
	    "Plaster Blaster",
	    "Marked Man",
	    "Liberator",
	    "Waka-Gashira Wipeout!",
	    "A Drop In The Ocean",
	    "Bling-Bling Scramble",
	    "Uzi Rider",
	    "Gangcar Round-Up",
	    "Kingdom Come",
	    "Grand Theft Aero",
	    "Escort Service",
	    "Decoy",
	    "Love's Disappearance",
	    "Bait",
	    "Espresso-2-Go!",
	    "S.A.M.",
	    "Uzi Money",
	    "Toyminator",
	    "Rigged To Blow",
	    "Bullion Run",
	    "Rumble",
	    "The Exchange",
	};
	return number < MISSION_COUNT ? kNames[number] : "a mission";
}

} // namespace coopiii
