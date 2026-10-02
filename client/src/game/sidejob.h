// The odd jobs belong to whoever drives.
//
// Paramedic, Firefighter, Vigilante and Taxi Driver start from main.scm's
// own threads, one per job, on every machine: IS_PLAYER_IN_MODEL (or
// IS_PLAYER_IN_TAXI) for the job's vehicle, then IS_BUTTON_PRESSED 0 19, the
// sub-mission key (14 under controller setup 3), held and let go, then
// START_MISSION. Neither test cares which seat the player is in, and in
// single player nobody is ever a passenger, so a player riding in a
// teammate's ambulance could start a Paramedic shift in a car somebody else
// drives. The same key in the mission is its cancel.
//
// So the key does nothing for a passenger: the one question those threads
// ask about it is answered "not pressed" while the local player sits in a car
// and not at its wheel. Nothing else in main.scm asks for either button, and
// GET_PAD_STATE, which reads the pad for a script to keep, is left alone. The
// job's help box still tells a passenger which key it is, since that is the
// in-model test's and not the key's; pressing it does nothing.
//
// A driver's shift is the session's mission like any other (missions.md
// 5.2): a passenger is a participant, sees its text, blips and counters, the
// fares and patients it makes, and is paid what it pays (mission-audit.md,
// odd jobs). With `missions = off` it stays the driver's own, as every
// mission is.
#pragma once

#include <cstdint>

namespace coopiii::game {

// Whether the script may hear a button the local player's pad reports as
// down. Pure, so tools/clienttest walks it.
inline bool SubMissionKeyCounts(int32_t pad, int32_t button, bool passenger) {
	const bool jobKey = pad == 0 && (button == 19 || button == 14);
	return !(jobKey && passenger);
}

// The other starts that are their vehicle's: RC's van at its spot, the 4x4
// runs' and Mayhem's cars in their zone, and an odd job's key should the
// redirect below be missing. Each is held at START_MISSION by the session's
// mission (game/mission.cpp, LaunchGate), which claims it for whoever's
// machine got there. A passenger's machine gets there too, riding into the
// zone in the back of the right car, so from a passenger seat the start is
// held without a claim until he gets out or the session has a mission, and
// from on foot it is never claimed at all: it goes round its trigger's loop,
// which is what a start given up does.
enum class VehicleStart : uint8_t {
	Ask,      // the session's gate, as for any start
	Hold,     // START_MISSION again next frame, nothing claimed
	GiveUp,   // past it, and round the trigger's loop
};

inline VehicleStart VehicleStartGate(bool inCar, bool passenger, bool missionRunning) {
	if (!inCar || (passenger && missionRunning))
		return VehicleStart::GiveUp;
	return passenger ? VehicleStart::Hold : VehicleStart::Ask;
}

// A start given up goes on past START_MISSION, and every odd job's trigger
// marks its job as begun in the instruction straight after it: Taxi Driver's
// `0004 $ON_TAXI_MISSION = 1`, the other three a help flag they have set
// already. The taxi's is the one that matters: its loop starts a shift only
// while $ON_TAXI_MISSION is 0, and only the shift's own cleanup puts it back.
// A driver held at the start (a teammate further than 50 m) who got out of
// the taxi left that machine's taxi trigger dead until a load. So a given-up
// odd job skips that SET_VAR_INT global to 1 with it: how many bytes, 0 when
// what follows is anything else.
inline uint32_t GivenUpSequelLength(const uint8_t *space, uint32_t size, uint32_t at,
                                    int32_t missionNumber) {
	constexpr uint32_t LENGTH = 7;   // 04 00, 02 global, 04 int8 1
	if (missionNumber < 11 || missionNumber > 14 || at + LENGTH > size)
		return 0;
	const uint8_t *p = space + at;
	return p[0] == 0x04 && p[1] == 0x00 && p[2] == 0x02 && p[5] == 0x04 && p[6] == 0x01 ? LENGTH : 0;
}

// Takes IS_BUTTON_PRESSED's call to GetPadState. Not fatal: without it a
// passenger's key reaches START_MISSION, and with `missions = on` is held
// there by VehicleStartGate.
bool InstallSideJobKey();
void RemoveSideJobKey();

} // namespace coopiii::game
