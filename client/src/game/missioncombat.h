// The owner's mission against every player in it: whose blast counts against
// its only-the-player targets, and which player each of its enemies goes for.
//
// Pure, so tools/clienttest covers every answer without a game. The engine
// side is MissionHitScope in population.cpp (the blast and the fire) and the
// enemy sweep in mission.cpp (SweepMissionEnemies).
//
// ---- the numbers, out of the retail image ------------------------------------
//
// eObjective. CPed::SetObjective (0x004D83E0), when the objective is the one
// the ped already holds, jumps through the table at 0x005F8F90 indexed by
// objective - 6. Entries 7 and 8 land on `cmp [ebx+16Ch],ebp` (0x004D8457),
// the same arm as 11, 12, 20 and 25: re3's KILL_CHAR_ON_FOOT and
// KILL_CHAR_ANY_MEANS against m_pedInObjective. Entry 19 lands on
// `cmp [ebx+170h],ebp` (0x004D8449) with 14, 15, 28 and 31: DESTROY_CAR
// against m_carInObjective. And the handlers push them: 01C9's at 0x00441BF8
// ends `push 7 / call 0x004D83E0`, 01D9's at 0x00442082 `push 13h`.
//
// The same function opens with the trap every re-issue has to step round:
// `mov eax,[ebx+168h] / cmp eax,esi / jne / test eax,eax / je` then return
// (0x004D8403) - an objective equal to the stored m_prevObjective is refused
// outright, whatever the ped is doing now.
//
// eCarMission and CAutoPilot::m_pTargetCar. SET_CAR_RAM_CAR (032C, the 800
// table's entry 12, 0x00448B2A) collects two handles, looks both up in the
// vehicle pool and calls CCarAI::TellCarToRamOtherCar (0x00415D90), which is
// `mov [ebx+198h],ecx` (the target, then RegisterReference on it) and
// `mov byte [ebx+15Ah],0Fh`: RAMCAR_FARAWAY into m_nCarMission. The collision
// test at 0x0041ACF0 reads `cmp byte [ebp+15Ah],3` then FindPlayerVehicle,
// and `cmp byte [ebp+15Ah],10h` then `cmp edi,[ebp+198h]`: RAMPLAYER_CLOSE
// against the player's car, RAMCAR_CLOSE against the target. The
// FARAWAY-to-CLOSE switch writes 10h at 0x00414DAA and the way back 0Fh at
// 0x00414EBE. SET_CAR_MISSION (00AF, the 100 table's entry 75, 0x0043CE04)
// writes its second operand into [car+15Ah] as it stands; the missions pass
// MISSION_RAMPLAYER_FARAWAY, 2, and the AI writes 2 and 3 back and forth at
// 0x00414168 and 0x00413FA2.
#pragma once

#include "addresses.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coopiii::game {

namespace mcombat {

constexpr uint32_t OBJECTIVE_KILL_CHAR_ON_FOOT   = 7;
constexpr uint32_t OBJECTIVE_KILL_CHAR_ANY_MEANS = 8;
constexpr uint32_t OBJECTIVE_DESTROY_CAR         = 19;

constexpr uint8_t CARMISSION_NONE               = 0;
constexpr uint8_t CARMISSION_RAMPLAYER_FARAWAY  = 2;
constexpr uint8_t CARMISSION_RAMPLAYER_CLOSE    = 3;
constexpr uint8_t CARMISSION_RAMCAR_FARAWAY     = 15;
constexpr uint8_t CARMISSION_RAMCAR_CLOSE       = 16;

constexpr size_t PED_IN_OBJECTIVE        = 0x16C;   // CPed::m_pedInObjective
constexpr size_t AUTOPILOT_TARGET_CAR    = 0x198;   // CAutoPilot::m_pTargetCar, from the vehicle

// The opcodes the sweep runs, each one the engine's own handler.
constexpr uint16_t OP_SET_CHAR_OBJ_NO_OBJ     = 0x011C;
constexpr uint16_t OP_SET_CHAR_OBJ_DESTROY_CAR = 0x01D9;
constexpr uint16_t OP_SET_CAR_MISSION         = 0x00AF;
constexpr uint16_t OP_CAR_SET_IDLE            = 0x00A9;
constexpr uint16_t OP_SET_CAR_RAM_CAR         = 0x032C;
constexpr uint16_t OP_MARK_CAR_AS_NO_LONGER_NEEDED = 0x01C3;

// Where the proofs above sit, for clienttest's check against the image.
constexpr uintptr_t SET_OBJECTIVE_SAME_TABLE      = 0x005F8F90;
constexpr uintptr_t SET_OBJECTIVE_PED_ARM         = 0x004D8457;
constexpr uintptr_t SET_OBJECTIVE_CAR_ARM         = 0x004D8449;
constexpr uintptr_t KILL_CHAR_ON_FOOT_HANDLER     = 0x00441BF8;
constexpr uintptr_t DESTROY_CAR_HANDLER           = 0x00442082;
constexpr uintptr_t SET_CAR_MISSION_HANDLER       = 0x0043CE04;
constexpr uintptr_t SET_CAR_RAM_CAR_HANDLER       = 0x00448B2A;
constexpr uintptr_t CCarAI__TellCarToRamOtherCar  = 0x00415D90;
constexpr uintptr_t RAM_COLLISION_TEST            = 0x0041ACF0;

inline bool IsKillObjective(uint32_t objective) {
	return objective == OBJECTIVE_KILL_CHAR_ON_FOOT || objective == OBJECTIVE_KILL_CHAR_ANY_MEANS;
}
inline bool IsRamPlayer(uint8_t mission) {
	return mission == CARMISSION_RAMPLAYER_FARAWAY || mission == CARMISSION_RAMPLAYER_CLOSE;
}
inline bool IsRamCar(uint8_t mission) {
	return mission == CARMISSION_RAMCAR_FARAWAY || mission == CARMISSION_RAMCAR_CLOSE;
}

} // namespace mcombat

// ---- a participant's blast and fire (mission-audit.md R1) ----------------------
//
// SET_CHAR_ONLY_DAMAGED_BY_PLAYER and its car twin refuse any culprit that is
// not this machine's player. A participant's bullet and ram reach the owner
// as a packet and are applied under MissionHitScope. A blast does not: every
// machine replays it at its place, and the owner's engine puts the mission's
// entities in its own copy with the participant's replica as the culprit. A
// fire the blast leaves names the same replica as its source. So those are
// the causes let through on the owner's machine: the four a blast arrives as
// and the one only a CFire makes. CPed::InflictDamage already waives the flag
// for WEAPONTYPE_EXPLOSION (0x004EA4E3, `cmp [esp+38h],12h`); a car's
// (0x00551972) waives nothing, and neither waives the fire.
inline bool ParticipantBlastCounts(uint8_t cause) {
	switch (cause) {
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_FLAMETHROWER:
	case WEAPONTYPE_MOLOTOV:
	case WEAPONTYPE_GRENADE:
	case WEAPONTYPE_EXPLOSION:
		return true;
	default:
		return false;
	}
}

// ---- which player an enemy goes for -------------------------------------------
//
// Once an enemy has somebody, it keeps them unless somebody else is a good
// deal nearer, it is not already in the thick of it, and it has not just
// changed its mind. An enemy whose target is gone (dead, rebuilt, out of the
// car it was told to destroy) takes the nearest at once.
constexpr uint32_t ENEMY_HOLD_MS      = 4000;   // at least this long on one target
constexpr float    ENEMY_ENGAGED_M    = 15.0f;  // this close, it stays in the fight it has
constexpr float    ENEMY_SWITCH_RATIO = 0.6f;   // the other must be this much nearer...
constexpr float    ENEMY_SWITCH_GAIN_M = 10.0f; // ...and by this much at least
constexpr uint32_t ENEMY_SWEEP_MS     = 500;

// `distSq[i]` and `valid[i]` for each candidate i; `current` the one it has,
// or -1. The candidate to go for, or -1 for nobody.
inline int ChooseEnemyTarget(int current, const float *distSq, const bool *valid, size_t n,
                             uint32_t msSinceSwitch) {
	int best = -1;
	for (size_t i = 0; i < n; ++i)
		if (valid[i] && (best < 0 || distSq[i] < distSq[best]))
			best = static_cast<int>(i);
	const bool haveCurrent = current >= 0 && static_cast<size_t>(current) < n && valid[current];
	if (!haveCurrent)
		return best;
	if (best < 0 || best == current || msSinceSwitch < ENEMY_HOLD_MS)
		return current;
	const float dc = std::sqrt(distSq[current]);
	const float db = std::sqrt(distSq[best]);
	if (dc <= ENEMY_ENGAGED_M)
		return current;
	return (db <= dc * ENEMY_SWITCH_RATIO && dc - db >= ENEMY_SWITCH_GAIN_M) ? best : current;
}

// What an enemy the sweep watches is doing with the order it was given.
enum class EnemyHold : uint8_t {
	Engaged,   // on it, at a player (or, for `targetIsPlayer` false, at nobody's any more)
	Busy,      // on its way through a step of it (out of a car, into one): left alone
	Lost,      // it ran out: its target died or was rebuilt, and the engine gave up
	Foreign,   // the script gave it something else to do: no longer the sweep's
};

// A pedestrian: `ours` whether the objective it holds is the kind it was
// given, `prevOurs` the same of m_prevObjective, `targetGone` that the
// objective's target is null, dead or a wreck, and `targetIsPlayer` that it
// is a player or a player's car. A completed objective restores the previous
// one, which for these is none (CPed::ProcessObjective's tail, re3
// PedAI.cpp:1941): OBJECTIVE_NONE is how "ran out" looks, and the script's own
// SET_CHAR_OBJ_NO_OBJ is taken off the sweep before it runs, so it never
// reaches here.
inline EnemyHold ClassifyEnemyPed(bool ours, uint32_t objective, bool prevOurs, bool targetGone,
                                  bool targetIsPlayer) {
	if (ours) {
		if (targetGone)
			return EnemyHold::Lost;
		return targetIsPlayer ? EnemyHold::Engaged : EnemyHold::Foreign;
	}
	if (prevOurs)
		return EnemyHold::Busy;
	return objective == 0 ? EnemyHold::Lost : EnemyHold::Foreign;
}

// A rammer. RAMCAR with its target gone falls to MISSION_NONE on its own
// (CCarAI::UpdateCarAI, re3 CarAI.cpp:276); the script's own ways there,
// SET_CAR_MISSION and CAR_SET_IDLE, take it off the sweep before they run.
inline EnemyHold ClassifyRammer(uint8_t mission, bool targetCarGone) {
	if (mcombat::IsRamPlayer(mission))
		return EnemyHold::Engaged;
	if (mcombat::IsRamCar(mission))
		return targetCarGone ? EnemyHold::Lost : EnemyHold::Engaged;
	return mission == mcombat::CARMISSION_NONE ? EnemyHold::Lost : EnemyHold::Foreign;
}

} // namespace coopiii::game
