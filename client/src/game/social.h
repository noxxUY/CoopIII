// The small things the engine gets wrong about the other players because it
// only ever knew one: whom the lock-on picks, what a respawn clears, who
// honked, whose foot is on the gas, and whom a gang that hates the player
// goes for. Each is a call the engine makes on its own, pointed at us at the
// call site; nothing here is an inline detour on a function's entry.
//
// Every address below was read out of the retail 1.0 gta3.exe, with the
// witness beside it, and tools/clienttest/social.cpp checks each one against
// the image when COOPIII_GTA3_EXE names one. The decisions are the inline
// functions at the bottom, which clienttest runs without a game.
#pragma once

#include "addresses.h"
#include "emergencyaddr.h"

#include <cstdint>

namespace coopiii {
class Client;
struct WorldBridge;
} // namespace coopiii

namespace coopiii::game {

// ---- the lock-on ----------------------------------------------------------------
//
// CPlayerPed::FindWeaponLockOnTarget (0x004F28D0, called at 0x004F21B7) and
// FindNextWeaponLockOnTarget (0x004F2D50, called at 0x004F224F and
// 0x004F226A) walk the ped pool from the top. A ped is skipped for being
// FindPlayerPed (0x004F29DB / 0x004F2E10), for PED_DIE or PED_DEAD
// (`cmp eax,30h` / `cmp eax,31h`), for bInVehicle (`cmp byte [esi+314h],0`)
// and for following the player (`cmp [esi+180h],eax`, m_leader). What is
// left goes through CPed::OurPedCanSeeThisOne (0x004C5700, __thiscall, one
// ped, `ret 4`, bool in al) and, if that says yes, is scored. A remote
// player's ped is an ordinary CCivilianPed to all of that, so the lock-on
// picks a teammate as happily as a gang member. The sight test is the one
// call on each loop's path that is about this ped alone, so it is the one
// redirected: a ped the lock-on "cannot see" is never scored.
constexpr uintptr_t CPlayerPed__FindWeaponLockOnTarget     = 0x004F28D0;   // recorded
constexpr uintptr_t CPlayerPed__FindNextWeaponLockOnTarget = 0x004F2D50;   // recorded
constexpr uintptr_t CPed__OurPedCanSeeThisOne              = 0x004C5700;
constexpr uintptr_t LockOn_CanSeeCall                      = 0x004F2A0D;
constexpr uintptr_t NextLockOn_CanSeeCall                  = 0x004F2E48;

// ---- what a respawn clears --------------------------------------------------------
//
// CWorld::ClearExcitingStuffFromArea (0x004B4E70, __cdecl: a CVector*, a
// radius, a bool). CGameLogic::Update calls it three times, from its wasted,
// busted and failed-critical-mission arms (0x00421585, 0x004217A2,
// 0x0042194E), each with `push 1 / push [005ECD70h] / push eax`: the player's
// position, 4000.0 - the whole map - and true. After its ped and car loops it
// calls, in this order:
//
//   0x004B5058  0x004BBED0  CObject::DeleteAllTempObjectsInArea (x, y, z, r)
//   0x004B5071  0x00479DB0  gFireManager.ExtinguishPoint (ecx = 0x008F31D0,
//                           x, y, z, r, `ret 10h`): every fire it finds
//   0x004B5082  0x004B5460  CWorld::ExtinguishAllCarFiresInArea (x, y, z, r):
//                           0x00552AF0 on every car in range, which raises
//                           its health to 300.0 ([006025A0h]) if it is under
//                           and zeroes the fire timer at +530h
//   0x004B5096  0x0055AD40  CExplosion::RemoveAllExplosionsInArea (x, y, z, r)
//   0x004B50A5  0x0055BB80  CProjectileInfo::RemoveAllProjectiles, behind
//                           `cmp byte [esp+3Ch],0` - the bool - and followed
//                           by CShadows::TidyUpShadows (0x00517570)
//
// In a session that runs on one machine, the one whose player died, and it
// puts out fires, saves burning cars and deletes rockets in flight across the
// whole map on that machine alone: a car whose fire timer this machine runs
// never blows up, and a grenade somebody else threw is gone from one screen.
// So in a session, and only while one of those three calls is in progress,
// the four are skipped. CLEAR_AREA (0x0044D8DD) calls the same function and
// is left alone: a script's clear is the script's.
constexpr uintptr_t CWorld__ClearExcitingStuffFromArea   = 0x004B4E70;
constexpr uintptr_t GameLogic_WastedClearCall            = 0x00421585;
constexpr uintptr_t GameLogic_BustedClearCall            = 0x004217A2;
constexpr uintptr_t GameLogic_FailedClearCall            = 0x0042194E;
// gFireManager (addresses.h) and CFireManager::ExtinguishPoint
// (emergencyaddr.h) are the water cannon's already.
static_assert(gFireManager == 0x008F31D0 && CFireManager__ExtinguishPoint == 0x00479DB0,
              "the clear's fire call is the water cannon's");
constexpr uintptr_t CWorld__ExtinguishAllCarFiresInArea  = 0x004B5460;
constexpr uintptr_t CExplosion__RemoveAllExplosionsInArea = 0x0055AD40;
constexpr uintptr_t CProjectileInfo__RemoveAllProjectiles = 0x0055BB80;
constexpr uintptr_t ClearExciting_FireCall               = 0x004B5071;
constexpr uintptr_t ClearExciting_CarFireCall            = 0x004B5082;
constexpr uintptr_t ClearExciting_ExplosionCall          = 0x004B5096;
constexpr uintptr_t ClearExciting_ProjectileCall         = 0x004B50A5;

// ---- a honk from somebody else's car -------------------------------------------------
//
// CCarCtrl::SlowCarDownForPedsSectorList (0x00419300), the car's scan of the
// pedestrians in front of it. A ped that is not already dodging reaches
//
//   00419675  mov cl,[ebp+50h] / shr cl,3 / movzx eax,cl   the car's status
//   0041967E  test eax,eax / jne 00419786                 STATUS_PLAYER only
//   00419686  cmp esi,9 / je 00419786                     not PED_FLEE_ENTITY
//   0041968F  cmp byte [ebx+160h],1 / jne 00419786        a random ped
//
// and then flees the car if it faces it or if the car's horn timer (+22Ch)
// is running (0x0041974E). A remote player's car is PHYSICS here, never
// PLAYER, so the pedestrians this machine hosts never run from a player who
// honks at them. The status test is inline, so it is the one place here that
// is patched rather than called: the seventeen bytes from 0x00419675 become
// a jump to a stub that answers "a player's car" for the car of a remote
// player at the wheel as well, and goes back to 0x00419686 or on to
// 0x00419786. Nothing in the image jumps into the middle of them (the only
// branch to any of them is 0x0041958A's, to the first).
constexpr uintptr_t SlowCarDown_StatusTest  = 0x00419675;
constexpr uintptr_t SlowCarDown_PlayerCar   = 0x00419686;
constexpr uintptr_t SlowCarDown_NotPlayer   = 0x00419786;
constexpr uint8_t   SLOWCARDOWN_STATUS_TEST[17] = {0x8A, 0x4D, 0x50, 0xC0, 0xE9, 0x03,
                                                   0x0F, 0xB6, 0xC1, 0x85, 0xC0, 0x0F,
                                                   0x85, 0x00, 0x01, 0x00, 0x00};

// ---- whose foot is on the gas --------------------------------------------------------
//
// The audio plays FindPlayerVehicle's engine off the pad, not off the car:
// cAudioManager::ProcessVehicleEngine tests `FindPlayerVehicle() == veh`
// (0x0056A6B0) and hands it to ProcessPlayersVehicleEngine (0x0056B0D0),
// which reads CPad::GetAccelerate (0x00493780, __thiscall on Pads[0]
// 0x006F0360, plain `ret`, 0..255 in ax) at 0x0056B297. The Dodo's arm does
// the same at 0x0056AE43, and the boat engine takes the larger of
// GetAccelerate and GetBrake (0x004935A0) in each of its two arms
// (0x0056DFCD..0x0056DFF5, 0x0056E219..0x0056E245). FindPlayerVehicle is the
// car the player sits in, in any seat, so a passenger's own W key revs the
// engine while the driver's foot does nothing to it. For a passenger the
// pedals are the car's own, which the driver's machine sets.
constexpr uintptr_t CPad__GetAccelerate = 0x00493780;
constexpr uintptr_t CPad__GetBrake      = 0x004935A0;
constexpr uintptr_t ENGINE_ACCELERATE_CALLS[] = {0x0056AE43, 0x0056B297, 0x0056DFD9,
                                                 0x0056DFE8, 0x0056E225, 0x0056E234};
constexpr uintptr_t ENGINE_BRAKE_CALLS[] = {0x0056DFCD, 0x0056DFF5, 0x0056E219, 0x0056E245};

// ---- the everyday gangs ----------------------------------------------------------
//
// The gangs are ePedType 7..15 (PEDTYPE_GANG1..GANG9). A threat a mission
// sets with SET_THREAT_FOR_PED_TYPE (03F1) lands in the type's m_threats,
// which a gang member copies into its own m_fearFlags when it is built
// (addresses.h, CPedType). CPed::ScanForThreats only ever looks for this
// machine's player, so a gang set on "the player" goes for one player on
// each machine: whoever's machine hosts it.
constexpr int PEDTYPE_GANG_FIRST = 7;
constexpr int PEDTYPE_GANG_LAST  = 15;

// ---- the decisions ------------------------------------------------------------------

// The lock-on passes over a teammate unless friendly fire is on.
inline bool LockOnSkips(bool remotePlayer, bool friendlyFire) {
	return remotePlayer && !friendlyFire;
}

// What a respawn clears in a session: the peds, the cars and the temporary
// objects, as in single player, but not the fires, the burning cars, the
// explosions or the projectiles, which are everybody's.
inline bool RespawnClearsEffects(bool inSession, bool respawning) {
	return !(inSession && respawning);
}

// The pad's answer, 0..255, out of a pedal the car holds (-1..1 for the gas,
// 0..1 for the brake): the gas the replay path uses, and nothing for a
// pedal that is pushed the other way.
inline int32_t PedalAsPad(float pedal) {
	if (!(pedal > 0.0f))
		return 0;
	if (pedal >= 1.0f)
		return 255;
	return static_cast<int32_t>(pedal * 255.0f);
}

// A gang member this machine made at random, set on the player: it may be
// set on another player just as well.
inline bool GangHatesPlayers(int32_t pedType, uint8_t createdBy, uint32_t fearFlags,
                             uint32_t playerFlag) {
	return pedType >= PEDTYPE_GANG_FIRST && pedType <= PEDTYPE_GANG_LAST &&
	       createdBy == CHAR_CREATED_BY_RANDOM && playerFlag != 0 &&
	       (fearFlags & playerFlag) != 0;
}

// ---- for dllmain.cpp and cheats.cpp ----------------------------------------------

// The redirections above. Not fatal, each one on its own: without it the
// engine does what it always did, and the log says which.
bool InstallSocialHooks(Client &client);
void RemoveSocialHooks();
// Once a frame, before CGame::Process: which cars remote players are at the
// wheel of, for the honk.
void TickSocial();
void AddSocialToBridge(WorldBridge &bridge);

// After ScanForThreats (cheats.cpp) and MissionThreat: an everyday gang
// member of ours that fears the player, and found nobody or found ours, is
// pointed at the nearest remote player it can see instead when that one is
// nearer. `found` is what it had.
uint32_t EverydayGangThreat(void *ped, uint32_t found);

} // namespace coopiii::game
