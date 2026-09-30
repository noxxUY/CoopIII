// A unique stunt jump's slow-motion shot on the screen of a player riding in
// the car (game/ridecam.h, docs/protocol.md §1.35). Pure: when the shot
// goes up and when it comes down, from the packets and from the seat.
#pragma once

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii {

// The longest a shot is held with no word from the driver. The jump plays at
// a quarter speed on the driver's machine and USJ waits 600 ms of game time
// once the car lands, so a long jump is ten seconds of real time; twice that
// covers it, and a driver who drops out mid-air does not leave the rider
// staring at a fixed camera.
constexpr uint32_t STUNT_SHOT_MAX_MS = 20000;

struct StuntShot {
	uint16_t netId   = INVALID_NETID;   // the car the shot looks at, while it is up
	uint32_t sinceMs = 0;

	bool Up() const { return netId != INVALID_NETID; }
};

enum class StuntShotStep : uint8_t {
	None,
	Show,   // put the shot on our camera
	End,    // take it off
};

// A packet from the driver of `netId`. A start is only ours to show while we
// ride in that car; an end only ends the shot of that car.
inline StuntShotStep StuntShotOnPacket(const StuntShot &shot, bool on, uint16_t netId,
                                       bool ridingIt) {
	if (netId == INVALID_NETID)
		return StuntShotStep::None;
	if (on)
		return ridingIt ? StuntShotStep::Show : StuntShotStep::None;
	return shot.netId == netId ? StuntShotStep::End : StuntShotStep::None;
}

// Every frame: a shot whose car we no longer ride in, or that has been up too
// long, comes down.
inline bool StuntShotOver(const StuntShot &shot, bool ridingIt, uint32_t nowMs) {
	return shot.Up() && (!ridingIt || nowMs - shot.sinceMs >= STUNT_SHOT_MAX_MS);
}

} // namespace coopiii
