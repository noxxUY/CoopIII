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
// MISSION_RAMPLAYER_FARAWAY, 2, and the driver logic writes 2 and 3 back and
// forth at 0x00414168 and 0x00413FA2.
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

// ---- a participant's ram (mission-audit.md R1) -----------------------------------
//
// A collision is not forwarded. Every machine simulates its own copy of a car
// it does not drive with the collision proof on (vehicle.cpp,
// SetVehicleObserved), so the participant's machine never works out a dent on
// the owner's mission car at all. The owner's engine does: its copy of the
// participant's car runs into the mission car there, and
// CAutomobile::VehicleDamage prices it with the participant's car as
// m_pDamageEntity. Two things in it then refuse a participant what the owner
// gets:
//
//   0x0052F653  bOnlyDamagedByPlayer: not FindPlayerPed / FindPlayerVehicle,
//               return. Van Heist's Securicar has it (02AA immune_to_nonplayer).
//   0x0052FE2A  `cmp word [ebp+5Ch],76h` (MI_SECURICA), then m_pDamageEntity's
//               status, `mov cl,[eax+50h] / shr cl,3`, must be 0, STATUS_PLAYER,
//               for the damage to be multiplied by 7 (0x006005D4). Van Heist
//               counts that health down to 750 and 600 to open the van.
//
// The owner's copy of a participant's car is not STATUS_PLAYER, so for the
// one VehicleDamage call of a mission car a participant's car is ramming, the
// flag is lifted and the rammer's status bits read STATUS_PLAYER; its type
// bits, which 0x0052F6AA reads, are left alone. A pedestrian run over or
// rammed by a participant's car is the same flag on CPed::InflictDamage, with
// the car's cause.
inline bool ParticipantCollisionCounts(uint8_t cause) {
	return cause == WEAPONTYPE_RAMMEDBYCAR || cause == WEAPONTYPE_RUNOVERBYCAR;
}

// CPlaceable's type-and-status byte with the status taken to STATUS_PLAYER (0)
// and the type (bits 0-2) as it was.
inline uint8_t FlagsAsPlayersCar(uint8_t typeAndStatus) {
	return static_cast<uint8_t>(typeAndStatus & 0x07);
}

constexpr uintptr_t VEHICLE_DAMAGE_SECURICA_MODEL   = 0x0052FE2A;   // cmp word [ebp+5Ch],76h
constexpr uintptr_t VEHICLE_DAMAGE_RAMMER_STATUS    = 0x0052FE3B;   // mov cl,[eax+50h]
constexpr uintptr_t VEHICLE_DAMAGE_ONLY_BY_PLAYER   = 0x0052F653;
constexpr uint16_t  MI_SECURICA                     = 0x76;

// ---- a participant's blast that killed a pedestrian here (mission-audit.md R11) -----
//
// A grenade or rocket goes off on every machine, and on the machine hosting a
// pedestrian it kills, its explosion's creator is the thrower's replica. So
// CPed::InflictDamage (0x004EAD55, its one call) sends that death to
// CDarkel::RegisterKillNotByPlayer, whose one instruction is a statistic, and
// nobody credits it: blasts are not forwarded as hits (combat.h,
// IsForwardableDamage), and the thrower's machine only has a copy of the
// pedestrian, whose death it refuses. The host credits it, as it does
// somebody else's forwarded bullet (darkel.h, CreditRemotePedKill), when one
// of the explosions still going off here is a remote player's and reaches the
// pedestrian. That register is handed only the victim and the weapon, so the
// blast is found by where it is: gaExplosion, verified against the retail
// image in CExplosion::AddExplosion (0x005591C0): 0x30 rows of 0x3C bytes at
// 0x0064E208, a free one found by `cmp byte [eax+0064E22Ch],0` (+0x24,
// m_nIteration, 1 from the moment it is added), the place at +0x04 (three
// `fstp [ebx+4..0Ch]`), the radius at +0x10 and the creator, the culprit
// argument, at +0x18 (0x005592AD).
struct BlastSeen {
	float x = 0.0f, y = 0.0f, z = 0.0f;
	float radius       = 0.0f;
	bool  active       = false;   // m_nIteration != 0
	bool  remotePlayer = false;   // its creator is a remote player's replica or car
	uint8_t player     = 0xFF;    // whose, when remotePlayer; INVALID_PLAYER if unknown
};

constexpr float BLAST_REACH_SLACK_M = 4.0f;   // the radius grows as it goes off

// The first remote player's blast that reaches the victim, or `n`.
inline size_t RemoteBlastReaching(float vx, float vy, float vz, const BlastSeen *blasts, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		const BlastSeen &b = blasts[i];
		if (!b.active || !b.remotePlayer)
			continue;
		const float r  = (b.radius > 0.0f ? b.radius : 0.0f) + BLAST_REACH_SLACK_M;
		const float dx = vx - b.x, dy = vy - b.y, dz = vz - b.z;
		if (dx * dx + dy * dy + dz * dz <= r * r)
			return i;
	}
	return n;
}

inline bool KilledByRemoteBlast(float vx, float vy, float vz, const BlastSeen *blasts, size_t n) {
	return RemoteBlastReaching(vx, vy, vz, blasts, n) < n;
}

// Whose it was, for his stats screen (darkel.h, CreditKillStats).
inline uint8_t RemoteBlastKiller(float vx, float vy, float vz, const BlastSeen *blasts, size_t n) {
	const size_t i = RemoteBlastReaching(vx, vy, vz, blasts, n);
	return i < n ? blasts[i].player : static_cast<uint8_t>(0xFF);
}

// ---- keeping quiet, two stealth checks the script only asks of its player -----
//
// (mission-audit.md R13.) Read off the retail main.scm, which the converted
// source this project maps from has neither mission in:
//
// Deal Steal (mission 51) asks, at two places in its approach loop (mission
// offsets 0xA16 and 0xCC2), `if 0 / LOCATE_PLAYER_ANY_MEANS_2D (00E3) 231.0625
// -26.25 radius 10 10` and then `if 21 / NOT IS_PLAYER_IN_MODEL 135 /
// IS_PLAYER_SHOOTING (02DF)`: at the rendezvous, not in a Yardie car or
// shooting, KM3_14, "the deal is off". Its gosub at 0x1344 asks the same at 6 m
// with `if 0 / 02DF` alone, and sets the flag that turns the Yardies on the
// player.
//
// Plaster Blaster (mission 58) asks `if 0 / 00E3 $13048 $13052 radius 25 25`
// at three places (0x785, 0x910, 0xA98) and, true, has the decoy ambulance
// drive off: "You've been spotted!!".
//
// Both are single conditions, so a participant's answer only ever widens the
// owner's to true (mission.cpp). The one condition of Deal Steal inside an OR
// group is forced true only in that group, where a true one makes the group
// true whatever the others say.
namespace stealth {

constexpr uint16_t DEAL_STEAL      = 51;
constexpr uint16_t PLASTER_BLASTER = 58;
constexpr float    DEAL_X          = 231.0625f;   // the rendezvous
constexpr float    DEAL_Y          = -26.25f;
constexpr int32_t  YARDIE_CAR      = 135;          // the model Deal Steal wants

// LOCATE_PLAYER_ANY_MEANS_2D's own test (re3 Script.cpp, LocatePlayerCommand):
// strictly inside the box of half-sizes rx, ry around the point.
inline bool InLocateBox(float px, float py, float cx, float cy, float rx, float ry) {
	return cx - rx < px && px < cx + rx && cy - ry < py && py < cy + ry;
}

// Whether a locate the owner's mission asks is Deal Steal's rendezvous (a
// quarter of a metre of slack; the script writes it in 1/16ths).
inline bool IsDealRendezvous(float x, float y) {
	return std::fabs(x - DEAL_X) < 0.25f && std::fabs(y - DEAL_Y) < 0.25f;
}

// Plaster Blaster's three locates are its only 25 m ones.
inline bool IsDecoyLocate(float rx, float ry) {
	return std::fabs(rx - 25.0f) < 0.25f && std::fabs(ry - 25.0f) < 0.25f;
}

struct Watcher {
	float   x = 0.0f, y = 0.0f;
	bool    seated   = false;
	int32_t carModel = -1;
	bool    firing   = false;
};

// A participant at Deal Steal's rendezvous gives the deal away when not in a
// Yardie car, or shooting.
inline bool BlowsDealCover(const Watcher &w, float cx, float cy, float rx, float ry) {
	if (!InLocateBox(w.x, w.y, cx, cy, rx, ry))
		return false;
	return !(w.seated && w.carModel == YARDIE_CAR) || w.firing;
}

// A participant inside Plaster Blaster's 25 m box is seen, whatever they do.
inline bool SpotsDecoy(const Watcher &w, float cx, float cy, float rx, float ry) {
	return InLocateBox(w.x, w.y, cx, cy, rx, ry);
}

// May a condition that has just been answered be made true, by the and/or
// state it was asked under (read before it ran): a single condition, or one
// of an OR group. Never under AND, where a true one proves nothing.
inline bool MayForceTrueUnder(uint16_t andOrBefore) {
	return andOrBefore == SCRIPT_ANDOR_NONE ||
	       (andOrBefore >= SCRIPT_ANDOR_ORS_1 && andOrBefore <= SCRIPT_ANDOR_ORS_8);
}

} // namespace stealth

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
