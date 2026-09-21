// Firing, projectiles and explosions - the engine side of combat.
//
// docs/protocol.md §1.9 is the design, and it's worth reading before this
// file, because two decisions here look arbitrary without it:
//
//   - a shot is a reliable event and the trigger is a snapshot flag - two
//     different things, not two ways of saying the same one;
//   - an observer *animates* a thrown molotov but doesn't get to end it. The
//     thrower's machine says where it went off, same as it says where its
//     car ended up.
//
// Everything above the line below is pure arithmetic over the constants in
// addresses.h, so tools/clienttest covers it without GTA III running. That
// split matters more than usual here: the weapon whitelist is a decision,
// and getting it wrong means calling an engine fire path that reads the
// observer's own camera.
#pragma once

#include "addresses.h"

#include "client.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

// ---- decisions ------------------------------------------------------------

// Does this weapon put a CProjectileInfo entry in the world - something with
// a flight, a bounce and an explosion of its own? Read straight off
// CWeapon::Fire's switch: these three are the arms that reach FireProjectile.
inline bool IsProjectileWeapon(uint8_t weapon) {
	return weapon == WEAPONTYPE_ROCKETLAUNCHER || weapon == WEAPONTYPE_MOLOTOV ||
	       weapon == WEAPONTYPE_GRENADE;
}

// The eExplosionType a projectile of this weapon produces, or -1.
//
// Not a table CoopIII invented - it's the three-way switch at the top of
// CProjectileInfo::RemoveProjectile, transcribed. Used to bound an explosion
// type arriving off the wire against the weapon that supposedly caused it.
// On the way out it's never even needed, since the type gets sampled from
// the engine rather than derived here.
inline int ExplosionTypeForWeapon(uint8_t weapon) {
	switch (weapon) {
	case WEAPONTYPE_GRENADE:        return EXPLOSION_GRENADE;
	case WEAPONTYPE_MOLOTOV:        return EXPLOSION_MOLOTOV;
	case WEAPONTYPE_ROCKETLAUNCHER: return EXPLOSION_ROCKET;
	default:                        return -1;
	}
}

// A type byte off the wire is about to be multiplied up into CExplosion's
// own array. re3 declares ten of them.
inline bool IsKnownExplosionType(uint8_t type) {
	return type < EXPLOSION_TYPE_COUNT;
}

// May an observer replay this weapon through the engine's own CWeapon::Fire?
//
// Five get refused, each for its own reason - docs/protocol.md §1.9.2 has
// the full writeup, but they all boil down to the fire path reading
// something that belongs to *this* machine, not to the shooter:
//
//   UNARMED / BASEBALLBAT  melee. No projectile, no flash - everything
//                          visible about a punch is the animation, which
//                          the snapshot already carries, and the rest is
//                          just damage.
//   SNIPERRIFLE            CWeapon::FireSniper returns false unless this
//                          machine's camera is in first-person, and
//                          otherwise fires along that camera's Front.
//   FLAMETHROWER           hands the shot to CShotInfo, which keeps
//                          damaging long after the guarded call returns.
//   DETONATOR              sets off bombs that were never synced.
//
// HELICANNON (13) and anything above it aren't inventory weapons at all and
// get refused by the bound check.
inline bool IsReplayableWeapon(uint8_t weapon) {
	switch (weapon) {
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_AK47:
	case WEAPONTYPE_M16:
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_MOLOTOV:
	case WEAPONTYPE_GRENADE:
		return true;
	default:
		return false;
	}
}

// ---- engine ---------------------------------------------------------------
//
// Nothing below this line is reachable without the game. All three detours
// install together and remove together. A failure to install any one of
// them gets recorded through hook/hook.h's failure list rather than being
// fatal - a session with no muzzle flashes still beats a game that won't
// start at all.

// Detours CWeapon::Fire, CExplosion::AddExplosion and
// CProjectileInfo::RemoveProjectile. Returns false if any failed; the ones
// that succeeded stay installed and the reasons are in HookFailures().
bool InstallCombatHooks();
void RemoveCombatHooks();
bool CombatHooksInstalled();

// Hands over what the local player fired or blew up since the last call,
// oldest first, and returns how many got written.
//
// Sampled by detour rather than polled, because a shot is an event: an Uzi
// empties a clip between two 25 Hz samples, and a missed rocket is a missing
// explosion. The queue is small and bounded - if the game thread somehow
// outruns the drain, the oldest events drop and the newest ones stay, since
// a stale muzzle flash is worth less than a current one.
uint8_t DrainLocalCombat(CombatEvent *out, uint8_t max);

// Replay somebody else's shot on their ped.
void ReplayRemoteShot(RemotePlayer &player, const ShotBody &shot);

// Play somebody else's explosion, and end whatever projectile of theirs this
// machine was still animating.
//
// A player who disconnects mid-throw needs no cleanup here: the projectile
// CoopIII was animating still reaches CProjectileInfo::Update's deadline,
// and the detour turns that removal into a silent one. It just vanishes
// instead of exploding somewhere nobody actually decided.
void PlayRemoteExplosion(RemotePlayer &player, const ExplosionBody &body);

} // namespace coopiii::game
