// The Getaway (mission 29, Joey's bank job): the three robbers ride in
// whichever participant's car they board, and the mission's checks follow
// that car (docs/mission-audit.md, R4 and the Portland missions' table).
//
// The script, read off the retail main.scm, asks everything of its player:
//
//   - Joey's: `if or` of NOT 019B (the player stopped in a car in the box),
//     NOT 0122 (the horn) and a wanted level; then 00DA stores the car he is
//     in, 01EA reads its seats ("They ain't sardines!" under three), 00DE
//     refuses a bus or a coach, and the robbers are made and tied to him
//     with 01DF. Each waits for 00DB, itself in that car.
//   - On the way: 00E0 stores the car again every frame the player is in a
//     car; NOT 0320 puts a marker above a robber who is not in his group,
//     and 00E9 (30 m) ties him back and takes it off; the stop at the bank
//     is 019E/01A0, "the player in a car there", and past it the robbers
//     get out and run.
//   - Back at Joey's: 0199 and 00E0 again, 0320 for "Don't split up", 00DB
//     on the stored car, and the end.
//
// With two players the owner's own car is not always the one that will do:
// the guest comes in a car of his own, and the three robbers and two players
// do not fit in one. So the mission follows the **robbers' driver**:
//
//   - Before the robbers are in a car: the owner, when he drives a car with
//     the seats free at the pickup, else the first participant who
//     arrived there with enough free seats. Once the robbers exist the choice
//     is kept while it still holds, so it does not swap under them.
//   - While the robbers are in a car: whoever drives it (the owner first).
//     When nobody of the group sits in it, nobody does: the owner in some
//     other car is not "the player in the robbers' car".
//
// The owner as the driver is the script exactly as it was. A guest as the
// driver stands in for the player in the questions above (game/mission.cpp,
// the Getaway block), and the marker above a robber and the instructions
// about the car are shown to the driver alone, since they are what he has to
// do, not what everybody has.
//
// Drive Misty For Me follows its driver the same way (RIDES, below).
//
// Pure, like standin.h, so tools/clienttest/getaway.cpp covers it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coopiii::game::getaway {

constexpr uint16_t THE_GETAWAY        = 29;
constexpr uint16_t DRIVE_MISTY_FOR_ME = 21;

constexpr int32_t OP_STORE_CAR_PLAYER_IS_IN             = 0x00DA;
constexpr int32_t OP_IS_PLAYER_IN_MODEL                 = 0x00DE;
constexpr int32_t OP_IS_PLAYER_IN_ANY_CAR               = 0x00E0;
constexpr int32_t OP_LOCATE_PLAYER_ANY_MEANS_CHAR_2D    = 0x00E9;
constexpr int32_t OP_IS_PLAYER_PRESSING_HORN            = 0x0122;
constexpr int32_t OP_ADD_BLIP_FOR_CHAR                  = 0x0187;
constexpr int32_t OP_SET_PLAYER_AS_LEADER               = 0x01DF;
constexpr int32_t OP_IS_PLAYER_SITTING_IN_ANY_CAR       = 0x0443;

// What one mission's ride is made of.
struct Ride {
	uint16_t           mission;
	int                passengers;       // how many ride with the driver
	float              pickupX, pickupY; // where the driver stops for them
	float              pickupReachM;     // how far from it a car is "there"
	float              passengerReachM;  // how far from a passenger a car is still with him
	const char *const *labels;           // what the script prints about the car
	size_t             labelCount;
};

// The Getaway: three robbers; the middle of the box 019B asks about at Joey's
// (5.4 x 4.6 m), a car pulling up round it still starting the choice; the
// script's own 30 m locate, and a little more.
inline constexpr const char *GETAWAY_LABELS[] = {"HORN", "JM6_5", "NODOORS", "JM6_6", "EBAL_5", "HEY2"};

// Drive Misty For Me (21) asks the same of its player with one passenger:
// 00E0 and 00DA for the car, the 3 m stop outside Misty's flat (00E8) with
// the horn (0122) and 0443, 00DE for the camera, 01DF and 0320 with the 8 m
// 00E9 that ties her back ("HEY4" and its marker), and the stop at Joey's
// with her beside it. The same slack as The Getaway's.
inline constexpr const char *MISTY_LABELS[] = {"HEY4", "IN_VEH2"};

inline constexpr Ride RIDES[] = {
    {THE_GETAWAY, 3, 1087.1875f, -226.1875f, 14.0f, 40.0f, GETAWAY_LABELS,
     sizeof GETAWAY_LABELS / sizeof GETAWAY_LABELS[0]},
    {DRIVE_MISTY_FOR_ME, 1, 937.875f, -259.75f, 14.0f, 40.0f, MISTY_LABELS,
     sizeof MISTY_LABELS / sizeof MISTY_LABELS[0]},
};

// The ride of mission `number`, or null.
inline const Ride *RideOf(uint16_t number) {
	for (const Ride &r : RIDES)
		if (r.mission == number)
			return &r;
	return nullptr;
}

// Whether `x, y` is at the pickup.
inline bool AtThePickup(float x, float y, const Ride &ride = RIDES[0]) {
	const float dx = x - ride.pickupX, dy = y - ride.pickupY;
	return dx * dx + dy * dy <= ride.pickupReachM * ride.pickupReachM;
}

// One participant, the owner included, as the owner's machine sees him.
struct Candidate {
	bool     valid          = false;   // in the mission, alive, his ped here
	bool     drives         = false;   // at the wheel of a car
	bool     inRobbersCar   = false;   // seated in the car a robber sits in
	bool     drivesRobbers  = false;   // at its wheel
	int      freeSeats      = 0;       // free passenger seats of the car he drives
	bool     nearby           = false;   // at the pickup, or with the robbers
	uint32_t arrived        = 0;       // 1, 2, ...: the order he qualified in; 0 for not yet
};

// Whether `c` could take the robbers: driving, with `need` seats free,
// where they are.
inline bool CouldTakeThem(const Candidate &c, int need) {
	return c.valid && c.drives && c.nearby && c.freeSeats >= need;
}

// Who the mission's questions are about, as an index into `c`, or -1 for
// nobody. `owner` is the owner's own index. `robbersSeated`: a robber sits in
// a car. `previous`: the last answer, kept by `keep` (the robbers exist) while
// it still holds.
inline int ChooseDriver(const Candidate *c, size_t n, int owner, int need, bool robbersSeated,
                        int previous, bool keep) {
	auto ok = [&](int i) { return i >= 0 && static_cast<size_t>(i) < n && c[i].valid; };
	if (robbersSeated) {
		if (ok(previous) && c[previous].drivesRobbers)
			return previous;
		if (ok(owner) && c[owner].drivesRobbers)
			return owner;
		for (size_t i = 0; i < n; ++i)
			if (c[i].valid && c[i].drivesRobbers)
				return static_cast<int>(i);
		if (ok(previous) && c[previous].inRobbersCar)
			return previous;
		if (ok(owner) && c[owner].inRobbersCar)
			return owner;
		for (size_t i = 0; i < n; ++i)
			if (c[i].valid && c[i].inRobbersCar)
				return static_cast<int>(i);
		return -1;
	}
	if (keep && ok(previous) && CouldTakeThem(c[previous], need))
		return previous;
	if (ok(owner) && CouldTakeThem(c[owner], need))
		return owner;
	int best = -1;
	for (size_t i = 0; i < n; ++i) {
		if (!CouldTakeThem(c[i], need) || c[i].arrived == 0)
			continue;
		if (best < 0 || c[i].arrived < c[best].arrived)
			best = static_cast<int>(i);
	}
	return best;
}

// How many seats the robbers still to board need: the robbers alive, or all
// of them before any exists.
inline int SeatsNeeded(int aliveRobbers, const Ride &ride = RIDES[0]) {
	return aliveRobbers > 0 ? aliveRobbers : ride.passengers;
}

// With the robbers in a car and nobody of the group in it, what a question
// about the player in a car is: the owner in some other car is not the
// player in the robbers' car. Only what is asked on foot stays the owner's.
inline bool NobodyAnswersAsOwner(bool askedOnFoot) { return askedOnFoot; }

// Whether a participant is shown what the script says to its player about the
// car: the robbers' markers and "sound the horn", "get a vehicle". The
// driver's alone, or the owner's when there is none.
inline bool ShowsCrewHints(int viewer, int driver, int owner) {
	return viewer == (driver >= 0 ? driver : owner);
}

// The words the script prints about the car, to whoever has to deal with it.
// The rest of what it prints (the plot, the cops, "You need all 3") is for
// everybody.
inline bool IsCrewLabel(const uint8_t *label, const Ride &ride = RIDES[0]) {
	for (size_t k = 0; k < ride.labelCount; ++k) {
		const char *const known = ride.labels[k];
		size_t n = 0;
		while (known[n] != '\0' && label[n] == static_cast<uint8_t>(known[n]))
			++n;
		if (known[n] != '\0')
			continue;
		bool padded = true;
		for (size_t i = n; i < 8; ++i)
			padded = padded && label[i] == 0;
		if (padded)
			return true;
	}
	return false;
}

} // namespace coopiii::game::getaway
