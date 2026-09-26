// A player standing on something that moves: the engine half of
// docs/protocol.md 1.7.1. The arithmetic is client/src/remotebody.h.
#pragma once

#include "client.h"

namespace coopiii::game {

// The vehicle under the local player's feet, or the train wagon they sit in,
// and where on it they are.
bool SampleLocalRide(LocalRide &out);

// Our engine's placement of a vehicle by pool reference, or of a wagon by
// track and wagon id.
bool VehicleRideFrame(int32_t handle, RideFrame &out);
bool TrainRideFrame(uint8_t track, uint16_t wagon, RideFrame &out);

} // namespace coopiii::game
