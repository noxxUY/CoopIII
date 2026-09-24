// The car horn: what a honk is in GTA III, and when a replica should sound one.
//
// Pure, like pedanim.h, so tools/clienttest covers every decision in here
// without a game. The engine side is three writes in game/vehicle.cpp; the
// proof for every address named below is in addresses.h under "the horn".
//
// What the engine keeps is one byte, CVehicle::m_nCarHornTimer (+0x22C), and
// it means two different things depending on who is driving:
//
//   - the local player's car (status PLAYER): 1 on every frame the horn key is
//     down, 0 otherwise. The audio sounds any non-zero value, no pattern.
//   - everybody else's car: a countdown. The traffic AI's PlayCarHorn sets 45,
//     CAutomobile::ProcessControl takes one off per frame, and the audio plays
//     it through one of eight on/off rhythms, indexed by (44 - timer).
//
// A replica is never PLAYER here, so it is the second kind: ABANDONED while
// nobody sits in it, PHYSICS once its driver's ped is seated
// (game/carstatus.h). So copying the honker's 1 onto it plays column 43 of the
// rhythm table, and column 43 is off in all eight rhythms: a replica told "1"
// is silent. The siren has its own status-4 trap, a different one: the audio
// refuses to play it at all on an ABANDONED car (game/siren.h).
//
// ProcessControl also takes the timer back on every frame it runs: the
// ABANDONED arm zeroes it, and for PHYSICS the horn block takes one off
// (ReduceHornCounter, 0x005341B5). So the replica's horn is written after
// CGame::Process, from the per-frame correction, and read by the audio in the
// same frame - DMAudio.Service is the very next call after CGame::Process
// returns (0x0048E4A0).
//
// Whose pedestrians react, which is what the timer is for besides the sound:
//
//   - the flee (a honked-at pedestrian runs from the car) is decided in the
//     car's own pedestrian scan, and only for a car in status PLAYER and a
//     ped the engine made at random. A replica is neither, so it never
//     happens on an observer, horn or no horn.
//   - the evasive step and dive read the timer too, and they are reached
//     from that same scan, which runs after the status switch and the horn
//     block. On an empty replica the ABANDONED arm has zeroed it by then. On
//     a seated one, PHYSICS, the horn block has only taken 42 to 41, so they
//     see a horn; past their own early returns (addresses.h, "the horn"),
//     which nobody has watched in a game yet. The only other route in is a
//     ped's own collision code, which already passes the anim type the horn
//     would have forced.
//   - a replica pedestrian (someone else's, drawn here) is out of reach of
//     all three even on the honker's machine, where the horn is real: it is a
//     MISSION_CHAR, which the flee skips, and has bRespondsToThreats clear,
//     which both evasions test before they ever look at the horn.
//
// So the honker's own engine is the only one that makes pedestrians react,
// and only its own pedestrians. The machine that hosts the pedestrians
// standing in front of him does not hear about it. That is a gap, not a
// choice; it is spelled out in vehicle.cpp at the replay.
#pragma once

#include "siren.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

// ---- the engine's rhythms ---------------------------------------------------
//
// hornPatternsArray, 8 x 44 bytes at 0x00606AB8, transcribed from the image
// rather than from re3 (they agree). Read by cAudioManager::ProcessVehicleHorn
// as [m_nCarHornPattern][44 - m_nCarHornTimer] for any car that is not the
// local player's.
inline constexpr int HORN_PATTERNS       = 8;
inline constexpr int HORN_PATTERN_LENGTH = 44;

inline constexpr uint8_t HORN_PATTERN_TABLE[HORN_PATTERNS][HORN_PATTERN_LENGTH] = {
    {0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
    {0,0,1,1,1,1,1,0,0,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
    {0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,1,1,1,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,0,0,1,1,1,0,0,1,1,1,1,1,1,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,1,1,1,1,0,0,1,1,1,1,1,0,0,0,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0},
};

// Does a car that is not the local player's sound its horn this frame, given
// the timer and the rhythm it holds? The audio's own arithmetic, clamp
// included. What it leaves out is the re-pick at exactly 44, which chooses a
// new rhythm from the frame counter - which is why HORN_REPLAY_TIMER is not 44.
inline constexpr bool ReplicaHornAudible(uint8_t pattern, uint8_t timer) {
	if (timer == 0 || pattern >= HORN_PATTERNS)
		return false;
	const int clamped = timer > HORN_PATTERN_LENGTH ? HORN_PATTERN_LENGTH : timer;
	return HORN_PATTERN_TABLE[pattern][HORN_PATTERN_LENGTH - clamped] != 0;
}

// What a replica's timer is held at while its owner is honking.
//
// 42 reads column 2, which is on in every one of the eight rhythms, so it
// sounds whichever rhythm the car happens to be holding and needs no write to
// m_nCarHornPattern. Holding it there every frame makes it a steady note,
// which is what the honker hears too: his own car is status PLAYER and plays
// any non-zero timer without a rhythm.
inline constexpr uint8_t HORN_REPLAY_TIMER = 42;

inline constexpr bool SoundsInEveryPattern(uint8_t timer) {
	for (int p = 0; p < HORN_PATTERNS; ++p)
		if (!ReplicaHornAudible(static_cast<uint8_t>(p), timer))
			return false;
	return true;
}
static_assert(SoundsInEveryPattern(HORN_REPLAY_TIMER),
              "the replay value has to sound whatever rhythm the replica holds");

// ---- the sending end -----------------------------------------------------------
//
// The snapshot samples the engine's own verdict, m_nCarHornTimer != 0 on the
// car the local player is driving, rather than the horn key. The key is not
// the horn: a siren car turns a tap into a siren toggle and needs three frames
// of key for a horn, Mr Whoopee turns it into the jingle, and the Yardie Lobo
// has no horn at all. The engine has already sorted all of that out by the
// time the timer is written.
//
// The bit rides the unreliable snapshot, and a honk is short. So once the horn
// has been seen, the bit stays set for HORN_TAIL_SNAPSHOTS more snapshots
// after it stops. A tap that only one snapshot saw then goes out in two, and
// losing either still delivers it; the cost is that every honk ends 40 ms
// later on other screens than on the honker's.
inline constexpr uint8_t HORN_TAIL_SNAPSHOTS = 1;

struct HornTail {
	uint8_t left = 0;
};

// Whether this snapshot carries VEH_HORN. Called once per snapshot sent.
inline bool HornOnWire(bool sounding, HornTail &tail) {
	if (sounding) {
		tail.left = HORN_TAIL_SNAPSHOTS;
		return true;
	}
	if (tail.left > 0) {
		--tail.left;
		return true;
	}
	return false;
}

// ---- the receiving end ---------------------------------------------------------
//
// How long a snapshot that says "horn" is believed. A horn is a state held
// between snapshots, so a lost snapshot in the middle of one changes nothing:
// the last one stays applied. What must not happen is the state outliving its
// sender - a driver who pauses, alt-tabs or drops with the key down would
// leave the car honking on every other screen until somebody got into it.
// 250 ms is the vehicle interpolator's own extrapolation cap
// (VehicleInterpBuffer::MAX_EXTRAPOLATE_MS): past it the transform stops
// being predicted too, and six snapshots in a row have gone missing.
inline constexpr uint32_t HORN_FRESH_MS = 250;

// Should this replica be honking this frame?
//
//   flags          the newest VehicleFlags the row holds
//   hasDriver      somebody is in its driver's seat, per enter/exit. A car
//                  with nobody at the wheel cannot be honking, whatever the
//                  last snapshot said, and exit is reliable where the
//                  snapshot is not.
//   destroyed      it is a wreck. Nobody honks a wreck, and the engine zeroes
//                  a wreck's timer in its own status arm anyway.
//   lastStateAtMs  when that snapshot arrived, on WallClock; 0 for never. A
//                  spawn carries flags too, but it is not a snapshot and does
//                  not stamp this, so a late joiner is never greeted by a
//                  horn the server remembered from the last snapshot it saw.
inline bool ReplicaHornSounds(uint8_t flags, bool hasDriver, bool destroyed,
                              uint32_t lastStateAtMs, uint32_t nowMs) {
	if (!(flags & VEH_HORN) || !hasDriver || destroyed || lastStateAtMs == 0)
		return false;
	return static_cast<uint32_t>(nowMs - lastStateAtMs) <= HORN_FRESH_MS;
}

// ---- somebody else's traffic -----------------------------------------------
//
// A traffic car honks on the machine hosting it, in status SIMPLE or PHYSICS.
// Both arms of CAutomobile::ProcessControl call PlayHornIfNecessary
// (0x0053C4B0, from 0x00531B18 and 0x00531B4D), which calls PlayCarHorn
// through vtable +0x88 while the autopilot says it was slowed down for cars or
// peds (bits 0 and 1 of +0x165) and the car isn't waiting at a light
// (0x0042E220). PlayCarHorn (0x0053C450) does nothing while the timer runs;
// otherwise it draws rand() & 7 and sets 45 on 0-3 - four draws in eight, the
// driver shouting as well on 2 and 3 - and only shouts on 4-7. The same frame
// takes one off (ReduceHornCounter, 0x00531B1F in the SIMPLE arm, 0x005341B5
// for PHYSICS), so the audio's first look is at 44, which is where it picks a
// rhythm, and it plays that rhythm down to 1. A car held up at a junction
// starts a new honk nearly every time the last one runs out.
//
// None of that happens to a replica. With nobody in it, it is status 4, whose
// arm runs no AI and zeroes the timer every frame. With the host's driver
// seated it is PHYSICS, but the car AI is skipped for it (game/carstatus.h)
// and ProcessControl clears the slowed-down bits before the switch
// (0x00531892), so PlayHornIfNecessary never fires and the horn block only
// counts down what CoopIII wrote. So the host sends a bit per car
// (protocol.h, CarStateHornBit) and the replica runs the countdown itself,
// written after CGame::Process like the player's horn. It doesn't try to line
// up with the host's countdown, which it can't see at 10 Hz. It doesn't need
// to: every rhythm opens with two silent columns and ends with at least one,
// so a honk that starts a few frames off sounds the same.

// Would the host's own audio play this car's horn right now? The three early
// outs of ProcessVehicleHorn, so the bit never asks a replica to honk where
// the host is doing something else:
//
//   - a siren-switching car with its siren on (0x0056C23C): the timer is the
//     fast wail on the host. The siren travels with traffic now, so a new
//     replica given the timer would wail too, but a build from before the
//     siren bit has no siren on it and would honk. So the replica wails at
//     the ordinary rate while the host's is on the fast one.
//   - Mr Whoopee (0x0056C257).
//   - no timer (0x0056C26C).
inline constexpr bool TrafficHornOnWire(uint16_t model, bool sirenOn, uint8_t timer) {
	if (timer == 0 || model == MODEL_MRWHOOP)
		return false;
	return !(sirenOn && AudioSwitchesSirenForHorn(model));
}

// Is a traffic replica honking this frame?
//
//   onWire        the bit from the newest row its host sent
//   destroyed     the session says it's a wreck
//   lastRowAtMs   when that row arrived, on WallClock; 0 for never
//
// HORN_FRESH_MS again, and at the car stream's 10 Hz it covers one lost batch
// (the next row lands 200 ms after the last). A honking car goes out in every
// batch whatever its turn (game/streampick.h, STREAM_HONK_DEADLINE), and a car
// whose host stops streaming it falls silent 250 ms later instead of honking
// for good.
inline bool ReplicaTrafficHornSounds(bool onWire, bool destroyed,
                                     uint32_t lastRowAtMs, uint32_t nowMs) {
	if (!onWire || destroyed || lastRowAtMs == 0)
		return false;
	return static_cast<uint32_t>(nowMs - lastRowAtMs) <= HORN_FRESH_MS;
}

// Where a replica's countdown starts: the first value the host's audio sees.
inline constexpr uint8_t TRAFFIC_HORN_START = HORN_PATTERN_LENGTH;   // 44

// The timer to write into the replica this frame, 0 for "leave the engine's
// zero". Called once a frame; `left` is the row's own countdown, the value to
// write next and 0 when no honk is running. While `sounding` it counts 44 down
// to 1 and starts again, and the moment it stops, so does the honk.
inline uint8_t TrafficHornTimer(bool sounding, uint8_t &left) {
	if (!sounding) {
		left = 0;
		return 0;
	}
	if (left == 0)
		left = TRAFFIC_HORN_START;
	return left--;
}

} // namespace coopiii::game
