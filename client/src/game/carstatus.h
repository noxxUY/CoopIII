// The status a car copy is in, and whether this machine's car AI may drive it.
//
// Pure, so tools/clienttest covers every decision in here without a game. The
// engine side is SeatPedInCar and UnseatPedFromCar in game/ped.cpp,
// WarpIntoSeat in game/seat.cpp, and the three detours in game/vehicle.cpp on
// CCarAI::UpdateCarAI, CCarCtrl::SteerAICarWithPhysics and, for a boat,
// CCarCtrl::SteerAIBoatWithPhysics. addresses.h, "the car AI a status runs",
// has the switch these choose an arm of.
//
// Several comments in this tree said every replica CoopIII builds is status 4,
// ABANDONED. The spawn does write 4. Seating a ped in it doesn't leave it
// there: CPed::WarpPedIntoCar writes PHYSICS (3) for any ped that isn't a
// player (0x004D7E9B), in the driver's arm and in the passenger arm alike,
// and PLAYER (0) for one that is (0x004D7E83). The animated entry,
// CPed::PedSetInCarCB, writes 3 at 0x004CF4C2 too, but only for the driver's
// seat; a passenger getting in by the door leaves the status alone.
//
// So, per kind of car, with who writes it:
//
//   the local player's own car        PLAYER (0), the engine on his entry.
//                                     A remote passenger warped in used to
//                                     turn it into 3 - the car AI took the
//                                     wheel off him with MISSION_NONE's brake
//                                     and handbrake, or whatever mission the
//                                     car still held. Put back after the warp.
//   a session car another player      PHYSICS (3), the engine at the seating.
//   drives, his ped in the seat       Kept, with UpdateCarAI and
//                                     SteerAICarWithPhysics skipped for it
//                                     (SteerAIBoatWithPhysics for a boat).
//   a session car nobody drives,      ABANDONED (4): the spawn, RemoveDriver,
//   parked or being settled           UnseatPedFromCar. A passenger's warp no
//                                     longer changes it.
//   a copy the local player warps     unchanged. The warp writes PLAYER (0)
//   into as a passenger (seat key)    for him, which would read his horn key
//                                     into somebody else's car. Put back.
//   a traffic replica with its        PHYSICS (3), the warp, AI skipped.
//   host's driver in it
//   a traffic replica with nobody     ABANDONED (4), SpawnAmbientCarReplica.
//   a wreck                           WRECKED (5), BlowUpCar. A seating never
//                                     takes it off: the warp would write 3
//                                     over a 5 and un-wreck it.
//
// Why a driven copy stays 3 rather than being put back to 4. The ABANDONED
// arm has no AI, and the horn and the siren replays were written for it, but
// CPed::SeekCar also switches on the status when a ped reaches a door
// (0x004D44E8, table 0x005F89E0). For 0, 2, 3 and 11 an occupied seat is a
// SetCarJack (0x004D4569); for 4 only the front passenger's seat is checked
// (0x004D45B3), and the driver's door goes straight to SetEnterCar - the
// local player would get in on top of the remote driver instead of pulling
// him out. And 4 never draws brake lights: CAutomobile::PreRender skips that
// block for 4 and 5 (0x00539806).
//
// Why not the others. 0 reads this machine's pad into the car (the horn at
// 0x00534191, DoDriveByShootings at 0x00531A5D). 1, PLAYBACKFROMBUFFER, has
// the right ProcessControl arm - none, straight to the physics - but SeekCar
// sends 1 to its exit (0x004D461E), so the local player would walk to the
// door and stand there. 11 brakes at 1.0 with the handbrake on. 2 is rails.
//
// So 3, and the AI it runs is what goes. What that leaves in the frame is the
// owner's own steer, gas and brake, which ApplyRemoteVehicle writes before
// CGame::Process and which the AI used to overwrite with MISSION_NONE's
// before the physics ever read them.
#pragma once

#include "addresses.h"

#include <cstdint>

namespace coopiii::game {

// The status a car should be left in once a ped has been warped into it.
//
//   before       bits 3-7 of the entity flags, read just before the warp
//   driverSeat   the warp was into seat 0
//
// A driver's seat takes 3, which is what the warp itself wrote and what the
// animated entry writes too. A passenger's seat takes nothing: the warp's
// write is the warp's alone, PedSetInCarCB makes none for a passenger, and
// the car goes on being whatever it was - the local player's, somebody else's
// or nobody's. A wreck stays a wreck either way.
inline constexpr uint8_t StatusAfterSeating(uint8_t before, bool driverSeat) {
	if (before == ENTITY_STATUS_WRECKED)
		return ENTITY_STATUS_WRECKED;
	return driverSeat ? ENTITY_STATUS_PHYSICS : before;
}

// May this machine's car AI run on this car this frame?
//
//   status       as ProcessControl reads it; the detours are only reached
//                from the SIMPLE and PHYSICS arms, and this is what lets them
//                skip the table walk for every other car
//   copy         it has a row in the Observed or the Replica table: a session
//                car or somebody else's traffic
//   localDrives  CVehicle::m_pDriver is the local player
//
// A copy's controls are its owner's, so nothing here steers it. A car the
// local player drives is his engine's whatever the tables say, and a car that
// is no copy at all is this machine's own traffic, which the AI is for.
inline constexpr bool CarAiMayRun(uint8_t status, bool copy, bool localDrives) {
	if (status != ENTITY_STATUS_PHYSICS)
		return true;
	return !copy || localDrives;
}

} // namespace coopiii::game
