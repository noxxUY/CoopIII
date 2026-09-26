// A unique stunt jump's shot, from its driver to the players riding with him
// (protocol.h, StuntCameraBody; docs/protocol.md §1.35).
//
// Who hears it is a question about seats and nothing else, so it is answered
// here over the session's players with no socket around it, and
// tools/sessiontest walks it. server.h does the sending.
//
// The rules:
//
//   - a shot starts only from the player the session has at the car's wheel.
//     Anybody else's is refused: a passenger's machine never runs a jump
//     (the client's stunt guard), and a shot from nobody's car goes nowhere;
//   - it ends from whoever started it, wherever he sits by then: a jump that
//     fails because the car was left ends after the exit has reached the
//     session, and the riders must still be told;
//   - it goes to every player in a passenger seat of that car, and to nobody
//     else - not the sender, not somebody standing beside the car.
#pragma once

#include "coopiii/protocol.h"
#include "session.h"

#include <cstdint>
#include <vector>

namespace coopiii {

inline bool StuntShotSenderOk(const Player &sender, const StuntCameraBody &body) {
	if (body.netId == INVALID_NETID)
		return false;
	if (body.on == 0)
		return true;
	return sender.vehicleNetId == body.netId && sender.seat == 0;
}

// The ids of the players to tell, in roster order. Empty when the sender may
// not send it or nobody rides with him.
inline std::vector<uint8_t> StuntShotAudience(const std::vector<Player> &players,
                                              const Player &sender,
                                              const StuntCameraBody &body) {
	std::vector<uint8_t> out;
	if (!StuntShotSenderOk(sender, body))
		return out;
	for (const Player &p : players)
		if (p.id != sender.id && p.vehicleNetId == body.netId && p.seat != 0)
			out.push_back(p.id);
	return out;
}

} // namespace coopiii
