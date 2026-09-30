// A stunt is paid for on the machine whose engine flew the car.
//
// main.scm's HJ and USJ threads run on every machine, each about its own
// player and whatever car that player sits in, driver or passenger. Both
// start from IS_CAR_IN_AIR_PROPER, which says a car is in the air when its
// physics touched nothing this frame (addresses.h, "is a car in the air").
// A copy of somebody else's car is not simulated here: it is corrected onto
// its owner's stream every frame, or held where the session left it, and a
// held car's physics are skipped outright, which zeroes that count every
// frame. So a player riding in a teammate's car, or sitting in one parked
// under them, was "in the air" to this machine's script, and the bonus it
// then worked out - INSANE STUNT BONUS and its cash, the jump stats - was
// made of the copy's corrections.
//
// The answer is taken off the one call that hands it to the script
// (IS_CAR_IN_AIR_PROPER_ANSWER): a car this machine's engine does not move
// is never in the air to it. A car the local player drives is ours whatever
// the session says (CarOwnerHere asks the driver's seat first), so a real
// jump still counts. The driver's machine runs the same threads about its
// own player, and a passenger's jump was never one in single player either.
//
// Nor is it one here, whoever moves the car: a player riding in a car is
// never in the air to his own stunt threads, even in one this machine
// simulates (a car nobody else has a claim on, or one a script sat him in
// beside a driver of its own). So the unique jump's $5000, its stat and the
// insane stunt bonus go to the driver's machine alone, which runs them about
// its own player. The riders see the jump's slow-motion shot instead
// (game/ridecam.h).
#pragma once

#include "vehicle.h"

namespace coopiii::game {

inline bool AirborneCounts(bool engineSaysAirborne, CarOwner owner, bool passenger = false) {
	return engineSaysAirborne && owner == CarOwner::Local && !passenger;
}

// Points IS_CAR_IN_AIR_PROPER's answer at us. Not fatal: without it the
// stunt threads ask the engine as they always did.
bool InstallStuntGuard();
void RemoveStuntGuard();

} // namespace coopiii::game
