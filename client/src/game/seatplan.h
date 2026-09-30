// Who gets which seat when CoopIII puts somebody in a car.
//
// Pure, so tools/clienttest covers every answer without a game. The engine
// side is SeatPedInCar and BeginPedEnterCar in game/ped.cpp; the numbers are
// the wire's, 0 for the driver and 1 + n for passenger slot n.
//
// Plus one decision about the local player: where a script of ours puts him
// when another player already has the wheel (PlanScriptedWheel, below).
//
// Two kinds of ped get seated by CoopIII: another player's ped, where the
// session says they sit, and a copy of somebody else's pedestrian, where its
// host says he sits (population.cpp). The mission's pedestrians are the
// second kind: 8-Ball riding in the car a participant drives, Misty in the
// owner's.
//
// The rule that decides it: **a seat is only ever taken from somebody whose
// being there is this machine's to undo.** A stale copy of another player can
// be moved, because the session has just said where that player really sits.
// A copy of a pedestrian can be moved by a player, because his host keeps the
// real seat and ours is only what he looks like here. This machine's own
// pedestrians, the owner's 8-Ball among them, are never taken out of a car to
// make room for anybody but a driver being jacked: taking the owner's 8-Ball
// out of his seat for a participant is the mission waiting forever for him to
// get back in. They can be moved along to another free passenger seat of the
// same car for a player, though, and are: a player keeps the seat he is in on
// his own screen, so Misty in the front on the owner's machine while the
// participant sits there on his is two screens that disagree, and the one
// that can change without anybody leaving the car is hers. Every script check
// on her is any seat - IS_CHAR_IN_CAR (0x0043D8E7) is bInVehicle and
// m_pMyVehicle, IS_CHAR_SITTING_IN_CAR (0x00589BBA) is PED_DRIVING and
// m_pMyVehicle - so the move costs the mission nothing.
// And nobody takes the local player's seat, ever.
//
// Anybody refused the seat they asked for gets another free passenger seat if
// the car has one. A pedestrian left standing where the seat is ends up inside
// the car, in the way of its collision, which is the car that barely moves.
// A seat whose door somebody is climbing in by counts as taken: the entry ends
// in CVehicle::AddPassenger on that slot, which does nothing when it is full,
// and leaves him in the car with no seat.
#pragma once

#include <cstdint>

namespace coopiii::game {

// Who holds a seat on this machine.
enum class SeatHolder : uint8_t {
	Empty,
	LocalPlayer,    // the person playing this game
	RemotePlayer,   // another player's ped
	Replica,        // our copy of a pedestrian somebody else hosts
	EnginePed,      // a pedestrian this machine's own engine runs
};

// Who is being seated.
enum class SeatComer : uint8_t {
	RemotePlayer,   // the session says a player sits there
	Replica,        // its host says a pedestrian sits there
};

enum class SeatMove : uint8_t {
	Sit,         // the seat is free
	Evict,       // take it, and whoever holds it comes out first
	Elsewhere,   // another free passenger seat, SeatPlan::seat
	Refuse,      // none to be had: nothing changes, ask again later
	Shift,       // take it, and whoever holds it moves to SeatPlan::shiftTo
};

struct SeatPlan {
	SeatMove move;
	uint8_t  seat;
	uint8_t  shiftTo = 0;
};

// Bit s for each free wire seat s of a car with `maxPassengers` passenger
// slots, from the driver and the passenger slots as the engine holds them
// (true for taken). Slots past the fourth are left out: CPed::WarpPedIntoCar
// only ever looks at the first four (0x004D7DC9..0x004D7E3D).
constexpr uint8_t SEAT_PLAN_PASSENGER_SLOTS = 4;

inline constexpr uint16_t FreeSeatMask(bool driverTaken, const bool *passengerTaken,
                                       uint8_t maxPassengers) {
	uint16_t mask = driverTaken ? 0 : 1u;
	const uint8_t n =
	    maxPassengers < SEAT_PLAN_PASSENGER_SLOTS ? maxPassengers : SEAT_PLAN_PASSENGER_SLOTS;
	for (uint8_t i = 0; i < n; ++i)
		if (!passengerTaken[i])
			mask = static_cast<uint16_t>(mask | (1u << (i + 1)));
	return mask;
}

// The lowest free passenger seat, or 0 for none.
inline constexpr uint8_t FirstFreePassengerSeat(uint16_t freeSeats) {
	for (uint8_t s = 1; s <= SEAT_PLAN_PASSENGER_SLOTS; ++s)
		if (freeSeats & (1u << s))
			return s;
	return 0;
}

inline constexpr SeatPlan PlanSeat(uint8_t wanted, SeatHolder holder, uint16_t freeSeats,
                                   SeatComer who) {
	const uint8_t other = FirstFreePassengerSeat(freeSeats);
	const SeatPlan elsewhereOrRefuse =
	    other != 0 ? SeatPlan{SeatMove::Elsewhere, other} : SeatPlan{SeatMove::Refuse, wanted};

	// A passenger seat past the ones the warp can give is any free one.
	if (wanted > SEAT_PLAN_PASSENGER_SLOTS)
		return elsewhereOrRefuse;
	if (holder == SeatHolder::Empty) {
		if (wanted == 0 || (freeSeats & (1u << wanted)) != 0)
			return SeatPlan{SeatMove::Sit, wanted};
		return elsewhereOrRefuse;   // a slot this car does not have
	}
	if (holder == SeatHolder::LocalPlayer)
		return wanted == 0 ? SeatPlan{SeatMove::Refuse, wanted} : elsewhereOrRefuse;

	if (wanted == 0) {
		// The wheel. A player the session puts there takes it from anybody but
		// the local player: that is a jack, and the engine's own drag or the
		// warp behind it empties the seat. A pedestrian's copy takes it only
		// from another copy, whose host has nothing to say about this one.
		if (who == SeatComer::RemotePlayer)
			return SeatPlan{SeatMove::Evict, 0};
		return holder == SeatHolder::Replica ? SeatPlan{SeatMove::Evict, 0}
		                                     : SeatPlan{SeatMove::Refuse, 0};
	}

	switch (holder) {
	case SeatHolder::RemotePlayer:
		// The session has just said this seat is the coming player's.
		if (who == SeatComer::RemotePlayer)
			return SeatPlan{SeatMove::Evict, wanted};
		return elsewhereOrRefuse;
	case SeatHolder::Replica:
		// Only what he looks like here. A player moves him when there is no
		// other seat; another copy never does, or two copies would take the
		// seat off each other every frame.
		if (other != 0)
			return SeatPlan{SeatMove::Elsewhere, other};
		return who == SeatComer::RemotePlayer ? SeatPlan{SeatMove::Evict, wanted}
		                                      : SeatPlan{SeatMove::Refuse, wanted};
	case SeatHolder::EnginePed:
	default:
		// This machine's own pedestrian: along to another seat for a player,
		// never out. For a copy of somebody else's pedestrian, nothing moves.
		if (who == SeatComer::RemotePlayer && other != 0)
			return SeatPlan{SeatMove::Shift, wanted, other};
		return elsewhereOrRefuse;
	}
}

// A player's copy seated here, and the passenger seat the session says he has.
// They differ when this machine had to put him elsewhere: the seat was
// somebody's at the time, often this machine's own pedestrian still climbing
// in by its door. Once that has settled he goes to his own seat, and whoever
// took it goes to the one he leaves, so every screen has him where his own
// does.
//
//   have          the wire seat his copy is in here
//   want          the one the session gave him
//   holder        who holds `want` here
//   settled       he and that holder are sitting (PED_DRIVING), and nobody
//                 is halfway through that seat's door
enum class SettleMove : uint8_t {
	Stay,   // leave him where he is
	Move,   // his own seat is free: into it
	Swap,   // he and its holder change places
};

inline constexpr SettleMove PlanSettle(uint8_t have, uint8_t want, SeatHolder holder,
                                       bool settled) {
	if (have == want || have == 0 || want == 0 || want > SEAT_PLAN_PASSENGER_SLOTS ||
	    have > SEAT_PLAN_PASSENGER_SLOTS || !settled)
		return SettleMove::Stay;
	switch (holder) {
	case SeatHolder::Empty:     return SettleMove::Move;
	case SeatHolder::EnginePed:
	case SeatHolder::Replica:   return SettleMove::Swap;
	default:                    return SettleMove::Stay;
	}
}

// A script of this machine's putting the local player at the wheel of a car:
// SET_CHAR_OBJ_ENTER_CAR_AS_DRIVER, which walks him there, or
// WARP_PLAYER_INTO_CAR and WARP_CHAR_INTO_CAR, which put him straight in. With
// another player already driving, the walk ends in the engine's own jack of
// that player's copy (CPed::SeekCar, a driver behind the door) and the warp in
// SetDriver on top of him; either way the session hands the car over and the
// other player is out of it on his own screen. Give Me Liberty did it to a
// participant who had taken the Kuruma's wheel before the change of clothes.
//
// So he rides instead, in a free passenger seat, and the other player keeps
// driving. He is put straight into it, whichever instruction it was. Walked
// to a passenger door instead, he stood beside the car for sixteen seconds
// before he got in, which is the engine's own passenger timer for a mission
// char with the controls off (SetObjective, `push 36B0h` at 0x004D85EF) plus
// the door. Only a player already in a car is still given the walk, which
// the engine finishes at once for him. What Give Me Liberty asks afterwards
// does not need him at the
// wheel: IS_PLAYER_IN_CAR reads bInVehicle and m_pMyVehicle, any seat
// (missionaddr.h), and FindPlayerVehicle, which the in-car checks go through,
// hands back m_pMyVehicle for a passenger too (0x004A10D5..0x004A10DE). With
// no passenger seat free the script has its way, jack and all, because a
// mission waiting forever for its player to get in is worse.
//
//   anotherPlayerDrives  a remote player's ped is in the driver's seat here,
//                        or the session says a player other than us drives it
//   passengerSeatFree    one of the four slots CPed::WarpPedIntoCar looks at
//   inCar                the local player is in a car already
//   entering             and is opening a door or pulling somebody out now:
//                        too late to turn round, the engine is mid-animation
//   warp                 the instruction is one of the two warps
enum class WheelMove : uint8_t {
	Drive,   // what the script said
	Ride,    // a passenger seat of the same car
};

inline constexpr WheelMove PlanScriptedWheel(bool anotherPlayerDrives, bool passengerSeatFree,
                                             bool inCar, bool entering, bool warp) {
	if (!anotherPlayerDrives || !passengerSeatFree || entering)
		return WheelMove::Drive;
	// The passenger warp starts from the pavement. The walk does not care:
	// the engine's objective finishes at once for a player already inside.
	if (warp && inCar)
		return WheelMove::Drive;
	return WheelMove::Ride;
}

// The seat a participant takes of the ones the mission owner's machine handed
// out in the car its mission put its player in (protocol.h, C_MissionBoard;
// the owner's half is game/mission.h's AssignBoardingSeats): his own if it is
// free on his machine, or else one free here that nobody else was given,
// since his copy of the car can be a moment behind the owner's. 0 for none:
// he stays where he is. Bit s of `givenToOthers` for each wire seat somebody
// else got, so two participants never go for the same one.
inline constexpr uint8_t PickBoardSeat(uint16_t freeHere, uint8_t given, uint16_t givenToOthers) {
	if (given == 0)
		return 0;
	if (given <= SEAT_PLAN_PASSENGER_SLOTS && (freeHere & (1u << given)) != 0)
		return given;
	for (uint8_t s = 1; s <= SEAT_PLAN_PASSENGER_SLOTS; ++s)
		if ((freeHere & (1u << s)) != 0 && (givenToOthers & (1u << s)) == 0)
			return s;
	return 0;
}

// Which car a copy of somebody else's pedestrian is to sit in here, from
// the car his host's row names (AmbientPedState::vehicleNetId). netIds are one
// space, so the number is either traffic this machine has a row for or a
// session car.
enum class SeatCar : uint8_t {
	None,      // on foot, or a car that is not here yet
	Traffic,   // his host's own traffic, our copy of it
	Session,   // a car a player has claimed: the owner's, a participant's, a promoted one
};

//   named          his host names a car
//   trafficRow     the number is traffic this machine has a row for
//   trafficBuilt   and our copy of it exists
//   sameHost       and it is hosted by his host, who is the only one who can
//                  say which of its peds sits in it
//   sessionBuilt   the number is a session car and our CVehicle for it exists
inline constexpr SeatCar SeatCarFor(bool named, bool trafficRow, bool trafficBuilt,
                                    bool sameHost, bool sessionBuilt) {
	if (!named)
		return SeatCar::None;
	if (trafficRow)
		return trafficBuilt && sameHost ? SeatCar::Traffic : SeatCar::None;
	return sessionBuilt ? SeatCar::Session : SeatCar::None;
}

// Whether the door-opening entry may go for this seat. It ends in
// PedSetInCarCB putting the ped in the slot its door names, on top of
// whoever is there, so only an empty seat, a jack's driver or a stale copy of
// another player is walked to. Anything else is seated the warp's way, which
// asks PlanSeat. A copy of somebody else's pedestrian opens a door only to an
// empty seat: his host's engine chose that door because the seat was free
// there, and here it is nobody's to take from anybody for him.
inline constexpr bool DoorEntryMayTake(uint8_t seat, SeatHolder holder,
                                       SeatComer who = SeatComer::RemotePlayer) {
	if (holder == SeatHolder::Empty)
		return true;
	if (holder == SeatHolder::LocalPlayer || who == SeatComer::Replica)
		return false;
	if (seat == 0)
		return true;
	return holder == SeatHolder::RemotePlayer;
}

} // namespace coopiii::game
