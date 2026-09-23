#pragma once

#include <cstdint>

namespace coopiii {
struct WorldBridge;
}

namespace coopiii::game {

// Riding in somebody else's car.
//
// GTA III has no way for the player to be a passenger. Walk up to a car with
// a driver in it and the enter key jacks them; there is no seat to ask for,
// because in single player nobody is ever driving a car you would want to
// ride in. A co-op mod is the first thing that wants one, so this is the one
// place CoopIII adds a control the original game does not have.
//
// The seating itself is the engine's, not ours. CPed::WarpPedIntoCar branches
// on the ped's objective: OBJECTIVE_ENTER_CAR_AS_DRIVER takes the driver's
// seat, and anything on the other arm takes *the first free passenger slot*,
// which the engine picks itself. So CoopIII does not choose a seat - it asks,
// and then reads back which one it got, because that number is what the rest
// of the session needs.
//
// See addresses.h, CPed__WarpPedIntoCar, for why the objective has to be set
// first and what happens if it is not.
//
// That reads-it-back story is now only the fallback's. The animated entry has
// to name a door, and a door is a seat, so it picks the slot before the walk
// and finds out afterwards whether it kept it. SeatLocalPlayerIn below says
// what that costs.

// Which key asks for a seat. A virtual-key code; 'G' by default, settable
// from CoopIII.ini before the feature is installed.
void SetSeatKey(int virtualKey);

// True once per press, never while held. Called from the client's own tick,
// so the edge is per tick rather than per frame - a key held across two
// frames of one tick is still one request.
bool LocalWantsSeatToggle();

// Put the local player in the first free passenger seat of the car this pool
// ref names. Returns the seat index the engine gave it (1..8), or -1 if it
// could not: no such car, no free seat, or the player is not in a state to
// be seated.
//
// Never seats anyone in the driver's seat. Taking the wheel is the enter key
// and the engine's business; this is only ever the seat beside it.
//
// `seatAsked` is written on the SEAT_LOCAL_WALKING answer, and only then: it
// is the slot the engine was asked for before the walk started, which is what
// the session is told straight away so every other machine animates the same
// entry at the same time. It is a request and not a fact - another ped can
// take that slot while we are walking - so PollLocalSeatEntry still reports
// the seat the engine actually gave, and the caller corrects the session if
// the two differ. May be null.
int32_t SeatLocalPlayerIn(int32_t vehicleHandle, uint8_t *seatAsked);

// Get out of the car we are riding in. The same key does both, because the
// game's own exit could not be shown to work on a warped-in passenger and a
// seat you cannot leave is worse than no seat. Returns false if we were not
// riding in anything.
bool UnseatLocalPlayer();

// Is the local player a passenger right now - in a car, but not driving it?
bool LocalIsPassenger();

// What SeatLocalPlayerIn means when it does not return a seat number.
//
// The seat key now asks the engine to walk the player to the door and open
// it, the same way a remote player's replica has since entercar landed. That
// takes frames, so the answer is no longer available on the frame the key was
// pressed. SEAT_LOCAL_REFUSED and SEAT_LOCAL_WALKING live in client.h, beside
// the ones the replica path uses.

// Drive an entry that SeatLocalPlayerIn started. Same three answers, with a
// seat number meaning it finished this frame. Cheap and safe to call every
// frame with nothing in flight.
int32_t PollLocalSeatEntry();

void AddSeatToBridge(WorldBridge &bridge);

} // namespace coopiii::game
