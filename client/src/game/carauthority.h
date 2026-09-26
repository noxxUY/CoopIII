// Which machine simulates a car, and so which machine's word moves it.
//
// Pure, so tools/clienttest walks a mission car through every hand it passes
// without a game. game/mission.cpp asks it where the owner's mission's word to
// a car goes (mission-audit.md R9); the frame pump in client.cpp keeps the same
// rule with the same inputs (VehicleIsOursToDrive, HaveCustodyOf, the ambient
// correction), each written before this and tested on its own.
//
// One rule, for a mission car as for any car, and it is the rule the session
// already had:
//
//   somebody drives it     their machine simulates it and streams it; every
//                          other machine's copy follows that stream, the
//                          mission owner's included
//   nobody drives it and   the player settling it (S_VehicleCustody)
//   somebody settles it
//   nobody holds it        a session car is held where the session last had it
//                          on every machine, each copy its own
//   nobody has claimed it  its host simulates it and streams it as traffic; a
//                          mission's car is its owner's until a player takes
//                          its wheel (docs/missions.md 5.3)
//
// So the mission owner's machine stops moving a car of its mission the moment
// somebody else takes the wheel: the car becomes a session car
// (S_CarPromoted), the owner stops hosting it, and its own copy follows the
// driver's stream from then on. The script keeps its handle, which never
// changes hands, and reads that copy: where it is, its health, and who sits
// in it, the owner's own mission pedestrians included, whom nobody is allowed
// to take out of their seats (game/seatplan.h).
//
// What made Give Me Liberty's Kuruma barely move with a participant at its
// wheel was not a second authority over the car. It was 8-Ball's copy
// standing inside it on that machine, put there every frame from where the
// owner's lagging copy of the car had him sitting, and run into by the car
// every frame.
#pragma once

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

enum class CarSim : uint8_t {
	Here,        // this machine's engine: nothing corrects it here
	Player,      // `player`'s machine; our copy follows its stream
	Everybody,   // nobody's: each machine holds its own copy where it was left
};

struct CarSimulator {
	CarSim  where;
	uint8_t player;
};

//   sessionCar   the session has claimed it (a RemoteVehicle row)
//   driver       its driver, INVALID_PLAYER for none
//   custodian    who settles it while nobody drives, INVALID_PLAYER for none
//   host         for a car nobody has claimed: whose engine made it and
//                streams it, INVALID_PLAYER for this machine's own
//   localId      this machine's player
inline constexpr CarSimulator WhoSimulates(bool sessionCar, uint8_t driver, uint8_t custodian,
                                           uint8_t host, uint8_t localId) {
	if (!sessionCar) {
		if (host == INVALID_PLAYER || host == localId)
			return CarSimulator{CarSim::Here, localId};
		return CarSimulator{CarSim::Player, host};
	}
	const uint8_t holder = driver != INVALID_PLAYER ? driver : custodian;
	if (holder == INVALID_PLAYER)
		return CarSimulator{CarSim::Everybody, INVALID_PLAYER};
	if (holder == localId)
		return CarSimulator{CarSim::Here, localId};
	return CarSimulator{CarSim::Player, holder};
}

// Where the owner's mission's word to a car goes (replay::Kind::Holder): only
// to the machine simulating it when that is somebody else's, to nobody when it
// is this one (its engine has run it and its stream carries what it did), and
// to every machine when nobody holds it.
enum class CarWordTo : uint8_t { Nobody, OnePlayer, Everybody };

inline constexpr CarWordTo WhereTheWordGoes(CarSimulator sim) {
	switch (sim.where) {
	case CarSim::Here:   return CarWordTo::Nobody;
	case CarSim::Player: return CarWordTo::OnePlayer;
	default:             return CarWordTo::Everybody;
	}
}

} // namespace coopiii::game
