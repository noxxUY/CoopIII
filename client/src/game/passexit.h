// A passenger gets out by his own door.
//
// Reported from a session: sitting in the front passenger seat of a car with
// nobody at the wheel, the exit key put the player out on the driver's side,
// jumped across to the driver's door before the animation started.
//
// The engine's own exit is CPed::SetExitCar, and it does pick the seat's door
// first. Then it asks the car whether there is room to stand there and, on a
// no, goes round to the other side (addresses.h, "the door a passenger gets
// out by"): the front passenger's other side is the driver's door, taken when
// the wheel is empty, and for a player even when it is not. A ped in the car
// but in no seat of it gets the driver's door from the first pick already.
// Single player never shows either, because its player is never a passenger.
// In a session he is one all the time, and the room test is the engine's word
// against what the player can see: nothing in the way of the door he sat by.
//
// So for a player riding as a passenger - ours, and another player's copy
// here, which has to leave by the same door his own screen shows - the door
// is his seat's own, and the room test is answered yes for that door alone.
// PedSetOutCarCB asks the same question again at the end of the exit, at a
// call of its own, and stands him clear of the car when the answer is no, so
// a real wall still does not keep him inside it.
//
// The decisions are here and pure, so tools/clienttest walks them.
#pragma once

#include "addresses.h"

#include <cstdint>

namespace coopiii::game {

// Where a ped sits in a car, as SetExitCar reads it.
constexpr int EXIT_SEAT_DRIVER = -2;   // CVehicle::pDriver
constexpr int EXIT_SEAT_NONE   = -1;   // in the car, in none of its seats

// The door a passenger leaves by: the seat's own, the mapping SetExitCar
// makes (0x004E10F7..0x004E1163). 0 leaves the choice to the engine: the
// driver, a bus (every seat gets out at the front), and a seat past the third
// slot, which has no door of its own.
//
// A ped in no seat is drawn in the front passenger's (CPed::SetPedPositionInCar
// has no other arm for him), so that door is his.
inline constexpr uint16_t OwnExitDoor(int seat, bool bus) {
	if (bus)
		return 0;
	switch (seat) {
	case EXIT_SEAT_NONE:
	case 0:  return offs::CAR_DOOR_RF;
	case 1:  return offs::CAR_DOOR_LR;
	case 2:  return offs::CAR_DOOR_RR;
	default: return 0;
	}
}

// Whether that door is on the right of the car: the side he steps down on
// when he is put out rather than animated out (game/seat.cpp,
// UnseatLocalPlayer).
inline constexpr bool ExitDoorOnRight(uint16_t door) {
	return door == offs::CAR_DOOR_RF || door == offs::CAR_DOOR_RR;
}

// What SetExitCar is told about the room at `asked`, while `held` is the door
// a player is being let out by (0 for none): the engine's answer, and yes for
// the held door.
inline constexpr bool ExitRoomAnswer(bool engineSays, uint16_t asked, uint16_t held) {
	return engineSays || (held != 0 && asked == held);
}

// A new driver's PedSetInCarCB tells every RANDOM_CHAR passenger to leave
// (addresses.h, "a new driver clears the car of its passengers"). The player
// is RANDOM_CHAR as well, so another player taking the wheel threw ours out of
// the seat beside him. A player stays - ours, or another player's copy, whose
// seat is his own machine's to give up - and a pedestrian goes, as in single
// player.
inline constexpr bool NewDriverSendsOut(bool localPlayer, bool remotePlayer) {
	return !localPlayer && !remotePlayer;
}

// ---- a passenger can always get out ------------------------------------------
//
// Reported from a car-bomb mission: the bomb was armed, the owner got out, and
// the player riding beside him pressed the exit key and stayed in his seat
// until the car went up with him. The exit key is CPlayerInfo::Process's
// (re3 PlayerInfo.cpp:127-150, the `cmp dword [ecx+224h],4` tests at
// 0x004A00D9 and 0x004A0224): it gives up for a car whose door lock is
// CARLOCK_LOCKED_PLAYER_INSIDE, which a mission's LOCK_CAR_DOORS on its car
// reaches every copy with (replay.h), for a wrecked car, and through
// CPad::GetExitVehicle for anybody whose controls the mission has taken,
// which SET_PLAYER_CONTROL does to every participant while the owner's
// scene plays. All of that is the owner's mission speaking to the owner. A
// player riding in a car another player holds is let out by the way the seat
// key lets him out (seat.h, UnseatLocalPlayer) when the engine has not
// started his exit PASSENGER_EXIT_WAIT_MS after he pressed it, and the car is
// slow enough for the engine's own exit (CPlayerInfo's 0.17 of move speed).
// Never the driver, never a boat, never while a cutscene of the engine's own
// runs.
constexpr uint32_t PASSENGER_EXIT_WAIT_MS   = 400;
constexpr float    PASSENGER_EXIT_MAX_SPEED = 0.17f;

inline bool PassengerLetOut(bool stillRiding, bool engineExiting, bool isCar, float speed,
                            bool cutscene, uint32_t sincePressMs) {
	return stillRiding && !engineExiting && isCar && speed < PASSENGER_EXIT_MAX_SPEED && !cutscene &&
	       sincePressMs >= PASSENGER_EXIT_WAIT_MS;
}

// Once a frame, before CGame::Process: the exit key, read off the pad itself
// (CPad NewState.Triangle, which the keyboard's enter/exit binding writes and
// which GetExitVehicle reads in pad modes 0, 1 and 3), for our player riding
// as a passenger.
void TickPassengerExit(uint32_t nowMs);

// Takes the LEAVE_CAR arm's call to SetExitCar and the room test inside it,
// each only while it still holds what the retail image has there, and the
// new driver's call that sends the passengers out. False when any could not
// be taken; the log says which, and without the room test a passenger with
// nothing beside his door still gets out by it.
bool InstallPassengerExit();
void RemovePassengerExit();

// SetExitCar on a ped, with its door chosen as above when `player` - another
// player's copy - is riding as a passenger. The engine's own choice for
// everybody else. What BeginPedExitCar calls.
void ExitCarByOwnDoor(void *ped, void *car, bool player);

} // namespace coopiii::game
