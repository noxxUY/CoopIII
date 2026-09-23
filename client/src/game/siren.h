// The siren: why a replica's was never heard, and when the audio is told the
// truth about one.
//
// Pure, like horn.h, so tools/clienttest covers every decision in here without
// a game. The engine side is one detour in game/vehicle.cpp, on
// cAudioManager::ProcessVehicleSirenOrAlarm.
//
// The byte is CVehicle::m_bSirenOrAlarm (+0x22E). ApplyRemoteVehicle has always
// copied the driver's onto the replica, which is why the lights flashed. The
// sound is ProcessVehicleSirenOrAlarm (0x0056C420, one caller, 0x00569C2E), and
// for a siren model it tests the status before it queues anything:
//
//   0056C446  cmp byte [ebp+22Eh],0        the siren byte (or the alarm below)
//   0056C4AE  call 0056C3C0                cAudioManager::UsesSiren(m_nIndex)
//   0056C4B3  test al,al / je 0056C543     not a siren model: the alarm path,
//                                          which has no status test at all
//   0056C4BB  mov cl,[ebp+50h] / shr cl,3
//   0056C4C4  cmp eax,4 / jne 0056C4D4     not ABANDONED: queue the siren
//   0056C4C9  mov al,1 / ... / ret 4       ABANDONED: silence
//
// That's the engine keeping quiet a police car its driver walked away from.
// This used to say every replica CoopIII builds is status 4, so the gate
// silenced every remote player's siren, driver or no driver. That was only
// true of a copy with nobody in it. A copy with its driver's ped in the seat
// is PHYSICS (3), because seating a ped that isn't a player writes that
// (game/carstatus.h), and the gate never held its siren back. So what the
// detour below is for is the gap between the two: the session says somebody
// is driving, and his ped isn't in the seat here - not streamed in yet, the
// seating refused, the entry still walking to the door. Nothing after
// 0x0056C4C7 reads the status again: 0x0056C4D4 picks the fast wail when the
// horn timer is running (except on the fire truck), and the rest is the
// sample, the volume and AddSampleToRequestedQueue (0x0057B070).
//
// So the detour changes one thing for the length of one call: for a copy in
// status 4 that somebody else is driving, it sets the status bits to PHYSICS
// (3), calls the original, and puts them back. PHYSICS because that's what the
// engine's own siren cars are in, and what the same copy will be once its
// driver sits down - the emergency-vehicle spawn at 0x00420212 writes
// `and al,7 / or al,18h` (3 << 3) and switches the siren on at 0x00420267. No
// other code runs while the status reads 3, so ProcessControl never sees it,
// and ProcessControl is the reason STATUS_PLAYER is off the table: its horn
// block reads this machine's pad into a car in that status (0x00534191).
//
// "The siren restarts its sound every time it's set" was a comment in
// ApplyRemoteVehicle, and it isn't true. The audio reads the byte afresh every
// frame at 0x0056C446 and asks for the same sample on the same counter (5)
// every frame it is set, the same thing it does for the player's own car with
// the siren held on - writing 1 over a 1 changes nothing it can see. The byte
// is still written only when the driver's flags change, and that is enough.
// The writers of +0x22E in the image are CCarAI::UpdateCarAI (which only the
// SIMPLE and PHYSICS arms call, and which is skipped for a copy), three places
// that are building a new car (0x00416F67, 0x00420267, 0x00437596), the
// player's horn block (status PLAYER), the script's siren switch (0x0044D963),
// a medic turning off his own ambulance's (0x004C32BC), the constructor and
// BlowUpCar. None of them reaches a live copy.
#pragma once

#include "addresses.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

// cVehicleParams::m_nIndex, the audio's name for a vehicle model: the model
// index minus 90, from ProcessVehicle's `movsx eax,[ebx+5Ch] / add eax,
// 0FFFFFFA6h` at 0x00569A7F.
inline constexpr uint16_t AUDIO_VEHICLE_INDEX_BASE = 90;

inline constexpr uint16_t MODEL_FIRETRUK = 97;
inline constexpr uint16_t MODEL_AMBULAN  = 106;
inline constexpr uint16_t MODEL_FBICAR   = 107;
inline constexpr uint16_t MODEL_MRWHOOP  = 113;
inline constexpr uint16_t MODEL_POLICE   = 116;
inline constexpr uint16_t MODEL_ENFORCER = 117;
inline constexpr uint16_t MODEL_PREDATOR = 120;

// cAudioManager::UsesSiren (0x0056C3C0): `lea eax,[edx-7] / cmp eax,17h / ja
// false`, then the table at 0x00607494, whose true entries (0x0056C3DA) are
// indices 7, 16, 17, 26, 27 and 30.
//
// Not the engine's CVehicle::UsesSiren (0x00552200, table 0x0060271C), which
// adds Mr Whoopee. His jingle goes down the alarm path here and was never
// behind the gate.
//
// The Predator is in the list and never reaches it: ProcessVehicle only calls
// ProcessVehicleSirenOrAlarm on the automobile arm of its switch on m_vehType
// (0x00569AE7, table 0x00607414, type 0 -> 0x00569B65). A boat's siren is
// silent in single player too.
inline constexpr bool AudioUsesSiren(uint16_t model) {
	switch (model) {
	case MODEL_FIRETRUK:
	case MODEL_AMBULAN:
	case MODEL_FBICAR:
	case MODEL_POLICE:
	case MODEL_ENFORCER:
	case MODEL_PREDATOR:
		return true;
	default:
		return false;
	}
}

// cAudioManager::UsesSirenSwitching (0x0056C3F0): `lea eax,[edx-10h] / cmp
// eax,0Eh`, table 0x006074F4, true (0x0056C40A) at indices 16, 26, 27 and 30.
// ProcessVehicleHorn plays no horn on one of these while its siren is on
// (0x0056C226-0x0056C23C); the timer turns the siren into the fast wail instead.
inline constexpr bool AudioSwitchesSirenForHorn(uint16_t model) {
	switch (model) {
	case MODEL_AMBULAN:
	case MODEL_POLICE:
	case MODEL_ENFORCER:
	case MODEL_PREDATOR:
		return true;
	default:
		return false;
	}
}

// Should the siren audio be shown this car as something other than abandoned?
//
//   model         CEntity::m_modelIndex
//   status        bits 3-7 of the entity flags byte, as the audio reads them
//   sirenOn       m_bSirenOrAlarm. Without it the call queues nothing anyway,
//                 and this keeps the detour's hands off every car that isn't
//                 about to make a sound.
//   remoteDriver  the session says another player is at the wheel, or for a
//                 traffic replica a ped row from its host says somebody is in
//                 the driver's seat. A replica with nobody driving it is what
//                 the gate is for: on its owner's machine a car he got out of
//                 is ABANDONED and silent, lights still going, and this keeps
//                 the two the same.
//
// Every car that isn't a replica fails on remoteDriver, which the caller
// decides from the Observed table or the traffic replica table.
//
// Only status 4 opens it. Once the driver's ped is in the seat here the copy
// is PHYSICS, which the audio never held back, so what is left is the gap
// between the session naming a driver and his ped sitting down. A wreck is 5
// and stays silent, as the owner's own wreck does.
inline constexpr bool ReplicaSirenGateOpens(uint16_t model, uint8_t status,
                                            bool sirenOn, bool remoteDriver) {
	return sirenOn && remoteDriver && status == ENTITY_STATUS_ABANDONED &&
	       AudioUsesSiren(model);
}

// The flags ApplyRemoteVehicle writes only when they change: the engine's idle
// note and the siren. The horn and the headlights are written every frame,
// because the engine takes both back from a copy by itself and a change
// detector would never notice: the horn in ProcessControl (the ABANDONED arm
// zeroes it, the horn block counts it down for a driven copy in PHYSICS), the
// lights in PreRender past 100 m for a copy in status 4. A driven copy's
// lights PreRender sets from its own clock every frame whatever is written.
inline constexpr uint8_t VEH_FLAGS_EVERY_FRAME = VEH_HORN | VEH_LIGHTS;

inline constexpr uint8_t VehicleFlagsWrittenOnChange(uint8_t flags) {
	return static_cast<uint8_t>(flags & ~VEH_FLAGS_EVERY_FRAME);
}

// ---- somebody else's traffic -----------------------------------------------
//
// On the host a traffic car's siren is switched by its car AI (UpdateCarAI's
// police missions), by the code that builds a car with it already going - an
// emergency car sent to a call (0x00420267), one siren car in two built at
// 0x00437596 (re3 has that in the road blocks), and Mr Whoopee, whose jingle
// is the same byte (0x00416F67) - and by a medic sending his ambulance back
// to cruising (0x004C32BC). A replica runs none of those: its AI is skipped
// (game/carstatus.h) or, with nobody in it, never called. So the host sends
// the byte, one bit a row (protocol.h, CarStateSirenBit), and the replica is
// given it after every frame's physics.
//
// The byte is the light bar. CAutomobile::PreRender (0x00535B40) switches on
// the model through the table at 0x00600AD0 and tests the byte at 0x005373F0
// for the fire truck, the ambulance, the police car and the Enforcer, and at
// 0x00537E4F for the FBI car. Nothing before it looks at the status in a way
// that skips it: a status-4 car goes to 0x0053655E at 0x00536062 and falls
// through to the switch like any other. So a copy with the byte set flashes
// whatever its status, and always did. Mr Whoopee has no lights (his entry is
// 0x00537F82, the end of the switch) and his jingle goes down the alarm path,
// which has no status test, so his replica plays it whoever sits in it - as
// the host's does.
//
// The sound is the gate above, and it's left to do what it does on the host.
// With the host's driver seated the replica is PHYSICS and the audio plays
// it. With his crew out - the police car parked beside a wanted player, cops
// on foot and the lights still going - the host's car is ABANDONED and silent,
// and the replica, its peds unseated by the same rows, is status 4 and silent
// too. The detour opens the gate only for the time between a ped row naming
// a driver and his replica sitting down.
//
// Held, not timed out, unlike the horn: the rows stop whenever the car drops
// out of its host's eight, and the car is held where it was last seen. Its
// light bar stays as it was last seen too. A wreck never keeps one: BlowUpCar
// clears the byte on the host (0x0053BE8D) and the session says it's a wreck.
inline constexpr bool ReplicaTrafficSirenOn(bool onWire, bool destroyed) {
	return onWire && !destroyed;
}

} // namespace coopiii::game
