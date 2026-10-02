// "The player" at a place, for any participant, with the owner nearby: the
// general rule behind every location condition of the owner's mission
// (docs/mission-audit.md, R4d, where every such condition in the retail
// main.scm is classified).
//
// A location condition asked by the owner's mission script about its own
// player, IS_PLAYER_IN_AREA_*, LOCATE_PLAYER_* and LOCATE_STOPPED_PLAYER_*
// (2D and 3D, any means, on foot, in a car), IS_PLAYER_STOPPED_IN_AREA_* and
// IS_PLAYER_IN_ZONE, is answered yes when the owner's machine said no and
//
//   - a participant of the mission satisfies the same area the same way (on
//     foot, in a car, stopped: standin.h PlaceNeedsOf and PlayerAnswersPlace),
//   - and the owner stands within OWNER_NEARBY_M of that area, on foot or in
//     any car (the area grown by that much on every side, height left out).
//
// The answer only ever goes from no to yes, through the flag the block would
// have had (mission.h CompareFlagIfTrue), so a NOT, an `if and` and an `if
// or` come out as they would with the owner there. nearchar.h's table of
// places anybody answers (no owner nearby needed) and standin.h's boxes
// anybody keeps are the specific cases in front of it, on the same path
// (mission.cpp, AnybodyAtPlace).
//
// What stays the owner's own answer:
//
//   - **A scene.** While the script's own scene runs (widescreen, a cutscene,
//     or the controls taken off the player by SET_PLAYER_CONTROL), and while
//     the owner's ped is walked out of a car (standin.h WalkedOut): a place
//     asked then is about where the script is moving the owner's own ped.
//   - **The stored car.** Once the mission has stored its player's car
//     (STORE_CAR_PLAYER_IS_IN) and that car is still there, only a
//     participant sitting in it answers: what the mission does next at the
//     place it does to that car (Misty walks to it, the patients get out of
//     it, Kanbu's bomb goes in it). An on-foot place then stays the owner's.
//   - **A car beside it in the block.** An `if and` or `if or` that also asks
//     IS_PLAYER_IN_ANY_CAR, IS_PLAYER_IN_MODEL or IS_PLAYER_SITTING_IN_(ANY_)CAR
//     is one state of the owner's (his car, then what he does at the place),
//     and one that asks IS_PLAYER_IN_CAR is answered only when that car
//     stands at the place, 5 m of slack (InCarAtThePlace's rule), so a guest
//     on foot at the door and the owner in Toni's car 50 m off is not "in
//     Toni's car at the door". Conditions after the place in the block have
//     not run when it is answered, so the yes is settled at the block's
//     goto_if_false (PlanFor, SettleWrites), always to what the block would
//     have been without it or with it, never to anything else.
//   - **The sites in ExcludedWhy**, where the next steps act on the owner's
//     own ped or car at that place and none of the rules above sees it.
//
// Checkpoints (mission.h HoldsCoordBlip) still wait for everybody but the
// owner: a yes this rule gives goes through MissionSync::AskCheckpoint like
// the owner's own, so every other participant has to be inside (5 m of
// slack), and the owner only has to be nearby.
#pragma once

#include "standin.h"

#include <coopiii/mission.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace coopiii::game::anyplace {

// How near the place the owner has to be: the distance a summoned player is
// left to walk (mission.h MISSION_MOVE_STAY_M), a little more than the 50 m
// a start allows. Across the street in his own car, or round the corner on
// foot, he is with the group; a block away he is not.
constexpr float OWNER_NEARBY_M = 60.0f;

constexpr int32_t OP_IS_PLAYER_IN_CAR              = 0x00DC;
constexpr int32_t OP_IS_PLAYER_IN_MODEL            = 0x00DE;
constexpr int32_t OP_IS_PLAYER_IN_ANY_CAR          = 0x00E0;
constexpr int32_t OP_IS_PLAYER_IN_ZONE             = 0x0121;
constexpr int32_t OP_IS_PLAYER_SITTING_IN_CAR      = 0x0442;
constexpr int32_t OP_IS_PLAYER_SITTING_IN_ANY_CAR  = 0x0443;
constexpr int32_t OP_STORE_CAR_PLAYER_IS_IN        = 0x00DA;

// Whether the owner is near enough the area for a participant's answer.
inline bool OwnerNearby(const MissionArea &area, const Vec3 &owner) {
	MissionArea flat = area;
	flat.shape       = MISSION_AREA_BOX2D;
	return InMissionArea(owner, flat, OWNER_NEARBY_M);
}

// ---- the block -----------------------------------------------------------------------

// A condition beside the place in its block, about the owner's car.
enum class Beside : uint8_t {
	Nothing,
	Car,      // IS_PLAYER_IN_CAR: that car has to stand at the place
	AnyCar,   // any other: the block is the owner's own state
};

inline Beside BesideOf(int32_t command) {
	switch (command & 0x7FFF) {
	case OP_IS_PLAYER_IN_CAR: return Beside::Car;
	case OP_IS_PLAYER_IN_MODEL:
	case OP_IS_PLAYER_IN_ANY_CAR:
	case OP_IS_PLAYER_SITTING_IN_CAR:
	case OP_IS_PLAYER_SITTING_IN_ANY_CAR: return Beside::AnyCar;
	default: return Beside::Nothing;
	}
}

// When the yes is written, and what the block's goto_if_false is left with.
// A yes inside a block is written as the place is asked when what follows can
// only lower it back (an `if and`, or a NOT in an `if or`): if a car beside
// it turns out to be the owner's own state, the block is put back to what it
// would have been without it. Otherwise (a NOT in an `if and`, a plain place
// in an `if or`) it is written at the goto_if_false, once nothing beside it
// says otherwise. In every case that value is a constant: one condition's
// yes or no decides an `if and`'s no and an `if or`'s yes on its own.
struct Plan {
	bool    valid      = false;
	bool    now        = false;   // write the yes as the place is asked
	bool    settle     = false;   // look again at the block's goto_if_false
	uint8_t settleTo   = 0;       // what to leave there
	bool    onConflict = false;   // ... when a car beside it says no (else when nothing does)
};

inline Plan PlanFor(uint16_t andOrBefore, bool notFlag) {
	if (andOrBefore == 0)
		return Plan{true, true, false, 0, false};
	if (andOrBefore >= 1 && andOrBefore <= 8)
		return notFlag ? Plan{true, false, true, 0, false} : Plan{true, true, true, 0, true};
	if (andOrBefore >= 21 && andOrBefore <= 28)
		return notFlag ? Plan{true, true, true, 1, true} : Plan{true, false, true, 1, false};
	return Plan{};
}

// Whether the goto_if_false of a block holding `plan`'s yes writes settleTo.
inline bool SettleWrites(const Plan &plan, bool conflict) {
	return plan.valid && plan.settle && conflict == plan.onConflict;
}

// ---- who answers ---------------------------------------------------------------------

// A participant as the owner's machine sees him.
struct Candidate {
	bool valid       = false;   // in the owner's mission, his state heard
	Vec3 at{};                  // where he is (his car, when he sits in one)
	bool seated      = false;
	bool stopped     = false;
	bool inStoredCar = false;   // sits in the car the mission stored
};

// Whether `c` satisfies the place: inside the area, as the check asks, and
// in the stored car when the mission holds one.
inline bool Answers(const standin::PlaceNeeds &needs, const MissionArea &area, const Candidate &c,
                    bool storedCarHeld) {
	return c.valid && (!storedCarHeld || c.inStoredCar) && InMissionArea(c.at, area, 0.0f) &&
	       standin::PlayerAnswersPlace(needs, c.seated, c.stopped);
}

// The first participant who answers, or -1.
inline int WhoAnswers(const standin::PlaceNeeds &needs, const MissionArea &area,
                      const Candidate *c, size_t n, bool storedCarHeld) {
	for (size_t i = 0; i < n; ++i)
		if (Answers(needs, area, c[i], storedCarHeld))
			return static_cast<int>(i);
	return -1;
}

// ---- the sites that stay the owner's -------------------------------------------------
//
// Read off the retail main.scm (the table in mission-audit.md R4d says why
// each). A site is the mission, the command (0 for any) and the first two
// operands after the player as the handler collected them: a locate's centre
// or an area's first corner (NaN for any).

constexpr float ANY = std::numeric_limits<float>::quiet_NaN();

// `r`, when given, is a locate's x radius: two locates round the same point
// that the script asks for different things.
struct Site {
	uint16_t    mission;
	int32_t     command;
	float       x, y;
	const char *why;
	float       r = ANY;
};

inline constexpr Site OWNERS_SITES[] = {
    {14, OP_IS_PLAYER_IN_ZONE, ANY, ANY, "the fare's destination is picked by where the owner is"},
    {19, 0, 879.375f, -303.375f, "the scene at the hideout walks the owner out of the Kuruma"},
    {19, 0, 903.75f, -420.1875f, "the scene at Luigi's walks the owner's ped to the back door"},
    {21, 0, ANY, ANY, "Misty rides with whoever picks her up, asked of him (getaway.h)"},
    {29, 0, ANY, ANY, "the robbers ride with whoever drives them, asked of him (getaway.h)"},
    {31, 0x00F6, 1219.5625f, -320.6875f, "the walk into Toni's is the owner's ped"},
    {32, 0x00F6, 1191.6875f, -870.0f, "the scene puts the owner at the wheel of the Stretch"},
    {35, 0x00E3, 1443.5625f, -188.25f, "the end scene walks the owner's ped to Salvatore's door"},
    {35, 0x00E7, 1440.625f, -181.375f, "the end scene walks the owner's ped to Salvatore's door"},
    {35, 0x00E4, 1436.25f, -180.625f, "the end scene walks the owner's ped to Salvatore's door"},
    {51, 0, 231.0625f, -26.25f, "the rendezvous is a stealth check, asked of each participant"},
    {51, 0x00F8, 452.25f, -1465.75f, "the casino scene walks the owner out of his car"},
    {52, 0x00F8, 452.25f, -1465.75f, "the casino scene walks the owner out of his car"},
    {52, 0x00F8, -91.5f, -484.1875f, "the store scene walks the owner out of his car"},
    {55, 0x00E3, ANY, ANY, "the scenes with Phil run the owner's own ped"},
    {58, 0x00E3, ANY, ANY, "the decoy's 25 m is a stealth check, asked of each participant"},
    {61, 0, ANY, ANY, "the car park is a stealth check asked of each participant"},
    {62, 0, 87.25f, -1548.5625f, "the walk into Love's is the owner's ped"},
    {62, 0, 87.4375f, -1548.6875f, "the walk into Love's is the owner's ped"},
    {63, 0x019B, 45.0f, 65.0f, "the race starts with the owner's car stored and locked"},
    {63, 0x00E4, ANY, ANY, "the race starts with the owner's car stored and locked"},
    {67, 0, 87.25f, -1548.5625f, "the walk into Love's is the owner's ped"},
    {67, 0, 87.4375f, -1548.6875f, "the walk into Love's is the owner's ped"},
    {69, 0x00E3, -1026.5f, -73.5f, "Decoy's end is asked of whoever drives the decoy van", 160.0f},
    {73, 0x00E3, -805.0f, -1310.0f, "the island it loads behind a screen is the owner's, for where he is",
     160.0f},
};

// The script writes each coordinate as a literal in 1/16ths; the same
// coordinate written twice reads the same.
inline bool SameCoord(float site, float asked) {
	return std::isnan(site) || std::fabs(site - asked) <= 1.0f / 32.0f;
}

// Where a locate's x radius is among the operands the handler collected (the
// player first), or -1 for anything that is not a locate at a point.
inline int RadiusAt(int32_t command) {
	if (command >= 0x00E3 && command <= 0x00E8)
		return 3;
	if (command >= 0x00F5 && command <= 0x00FA)
		return 4;
	return -1;
}

// Why a site stays the owner's, or null. `params` are the operands as the
// handler collected them, the player first; null for IS_PLAYER_IN_ZONE.
inline const char *ExcludedWhy(uint16_t mission, int32_t command, const float *params) {
	command &= 0x7FFF;
	for (const Site &s : OWNERS_SITES) {
		if (s.mission != mission || (s.command != 0 && s.command != command))
			continue;
		if (!std::isnan(s.r)) {
			const int at = RadiusAt(command);
			if (!params || at < 0 || !SameCoord(s.r, params[at]))
				continue;
		}
		if (std::isnan(s.x) && std::isnan(s.y))
			return s.why;
		if (params && SameCoord(s.x, params[1]) && SameCoord(s.y, params[2]))
			return s.why;
	}
	return nullptr;
}

// ---- a stored car that is only a target ----------------------------------------------
//
// Grand Theft Aero stores its player's car 23 times, every one of them to
// hand it straight to a Colombian as the car to destroy (00DA, then 01D9 on
// the same global), never as a car the story uses. Taken as the mission's
// stored car, the first goon to spot the owner in a car left every place
// after it (the yard, the lift up the tower) to a participant sitting in that
// car, and an on-foot one to the owner alone. In those missions a store is
// not the stored car. Marked Man's are left as they are: the CIA chase the
// car Ray rides in, and its places are the owner's by design (the table
// above, R4d).
inline bool StoreIsOnlyATarget(uint16_t mission) { return mission == 67; }

// ---- IS_PLAYER_IN_ZONE ---------------------------------------------------------------

// A zone's box as the engine tests it (CTheZones::PointLiesWithinZone,
// 0x004B6710): min and max on each axis, inclusive.
inline MissionArea ZoneArea(const float *min, const float *max) {
	return MissionAreaCorners3D(min[0], min[1], min[2], max[0], max[1], max[2]);
}

} // namespace coopiii::game::anyplace
