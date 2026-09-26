// A car's alarm, its gun and two flags of its own: the engine side.
//
// carextrasync.h has the rules and protocol.h the wire; this is
// the reads and writes, all of them plain fields proved in addresses.h
// ("a car's alarm, its gun and its taxi light"). Nothing is hooked.
//
// - The alarm is CVehicle::m_nAlarmState. The copy of a car somebody else's
//   engine set off is given the milliseconds left, and its own
//   ProcessCarAlarm, audio and PreRender do the rest: the sound, and the
//   lights flashing.
// - The gun is CAutomobile::m_fCarGunLR/UD, for the tank and the fire truck.
//   CAutomobile::Render turns the tank's turret by it on every copy.
// - The taxi light and the handbrake ride the snapshot's flags byte
//   (protocol.h, VEH_TAXI_LIGHT and VEH_HANDBRAKE): sampled beside the
//   engine and siren, written beside them.
#pragma once

#include "client.h"

#include <cstdint>

namespace coopiii::game {

// Wires WorldBridge's four alarm and gun entries.
void AddCarExtrasToBridge(WorldBridge &bridge);

// VEH_TAXI_LIGHT and VEH_HANDBRAKE as `vehicle` has them. The taxi light is
// asked of a car only (a boat's +0x4D9 is something else); the handbrake of
// anything.
uint8_t SampleCarStateFlags(void *vehicle);

// Puts the two onto a copy from a snapshot's flags. `driven`: the flags came
// from somebody at the wheel, so the handbrake means something. A car nobody
// drives keeps whatever its own status arm writes there.
void ApplyCarStateFlags(void *vehicle, uint8_t flags, bool driven);

} // namespace coopiii::game
