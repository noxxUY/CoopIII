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

#include <cmath>
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

// ---- where the engine puts a projectile it makes for a remote player ------
//
// Sounds like a detail, and it's the whole of the rocket bug.
//
// CProjectileInfo::AddProjectile (0x0055B030) builds a CMatrix per weapon and
// then assigns that matrix to the new CProjectile whole - rotation *and*
// translation - so whatever is in its position field is where the projectile
// is born. Three of the four arms put the `pos` argument there:
//
//   grenade                     0x0055B11C  fld [esp+7Ch] / fadd [esp+0D4h] /
//                                           fstp [esp+7Ch], and the same for
//                                           y and z
//   molotov                     0x0055B25B  the same three, field for field
//   rocket, thrown by a player  0x0055B3BC  pos copied in whole, after the
//                                           camera basis
//   rocket, at a seek target    0x0055B471  matrix built from two angles, then
//                                           += pos
//
// The fourth doesn't. A rocket from a ped that is neither the player nor
// chasing anybody is all of this:
//
//   0055B4A6  lea  eax, [ebx+4]      &ped->m_matrix
//   0055B4A9  lea  ecx, [esp+4Ch]    the local matrix
//   0055B4AD  push eax
//   0055B4AE  call 004B8F40          CMatrix::operator=
//   0055B4B3  ...                    straight on to the velocity
//
// `pos` is never read on that path. The rocket is created at *the ped's own
// origin* - hip height, inside their collision - and the fire source the
// caller worked out is thrown away.
//
// Every remote player is a CCivilianPed with no seek target, so every
// replayed rocket takes that fourth arm. On its owner's machine the same
// rocket takes the first one and starts a metre out in front of them. That
// asymmetry is why the bug is invisible in single player: retail GTA III has
// no NPC who fires a rocket launcher, so nothing else in the game has ever
// run this arm.
//
// Grenades and molotovs are *not* affected. Their arms are the two that add
// `pos`, so an observer's bottle starts where the thrower's bottle started.
inline bool ProjectileSpawnsAtThrower(uint8_t weapon) {
	return weapon == WEAPONTYPE_ROCKETLAUNCHER;
}

namespace vec {

inline Vec3 Cross(const Vec3 &a, const Vec3 &b) {
	return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float LengthSq(const Vec3 &v) { return v.x * v.x + v.y * v.y + v.z * v.z; }

} // namespace vec

// The three matrix rows for a projectile flying along `dir`, or false if
// `dir` isn't a direction.
//
// Not invented here: it's what AddProjectile's *player* arm does, which is
// the one that looks right on the thrower's own screen. That arm takes the
// camera's Front as forward, the camera's Up as up, and
// CrossProduct(Up, Front) as right (0x0055B32D onward). An observer has no
// camera for somebody else's shot, so world up stands in for the camera's,
// and the handedness still agrees with the engine's: with f = (0,1,0) and
// u = (0,0,1), Cross(u, f) is (-1,0,0), which is exactly what the engine
// produces for a camera looking down +Y.
//
// The identity Cross(up, forward) == right holds for what comes out of here
// too, since up is built as Cross(forward, right) and (f x r) x f == r for
// orthonormal f and r. clienttest pins that rather than trusting it.
//
// False on a zero-length, NaN or infinite direction. The caller then leaves
// the engine's own rotation where it is: a rocket pointing the wrong way is
// a cosmetic problem, and a matrix with a NaN in it is a crash three
// subsystems later.
inline bool ProjectileBasis(const Vec3 &dir, Vec3 &right, Vec3 &forward, Vec3 &up) {
	const float len2 = vec::LengthSq(dir);
	// Negated so a NaN, which compares false against everything, is refused
	// by both halves rather than sneaking through one of them.
	if (!(len2 > 1.0e-8f) || !(len2 < 1.0e8f))
		return false;

	const float inv = 1.0f / std::sqrt(len2);
	const Vec3  f{dir.x * inv, dir.y * inv, dir.z * inv};

	// Cross(worldUp, f), which degenerates exactly when the shot is straight
	// up or straight down - something a rocket launcher does. World forward
	// stands in for the reference there, and the result is still orthonormal.
	Vec3 r = vec::Cross(Vec3{0.0f, 0.0f, 1.0f}, f);
	if (!(vec::LengthSq(r) > 1.0e-6f))
		r = vec::Cross(Vec3{0.0f, 1.0f, 0.0f}, f);

	const float rlen2 = vec::LengthSq(r);
	if (!(rlen2 > 1.0e-12f))
		return false;
	const float rinv = 1.0f / std::sqrt(rlen2);

	right   = Vec3{r.x * rinv, r.y * rinv, r.z * rinv};
	forward = f;
	up      = vec::Cross(forward, right);
	return true;
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

// Is this one of the three explosions a thrown or fired projectile makes?
//
// The other seven are things the local world decided on its own: a car, a
// barrel, a heli. That difference matters while an observer is replaying
// somebody else's shot. A replayed bullet is still a real bullet in this
// world, and if it sets a parked car on fire that car's explosion belongs
// here and has to happen. Only the projectile's own explosion is the
// thrower's to decide (§1.9.3).
inline bool IsProjectileExplosion(uint8_t type) {
	return type == EXPLOSION_GRENADE || type == EXPLOSION_MOLOTOV ||
	       type == EXPLOSION_ROCKET;
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
//   DETONATOR              sets off bombs that were never synced.
//
// The flamethrower used to be on that list and no longer is. The reason it
// was there is real and still stands: CWeapon::FireAreaEffect hands the shot
// to CShotInfo, whose slot lives for the weapon's m_fLifespan and keeps
// setting things alight every frame until it expires, long after the call
// that created it returned. A guard that only covers the call does not cover
// that.
//
// What changed is that the guard is no longer the call. CPed::InflictDamage
// refuses anything a remote player's ped tries to do to the local player's
// health (RemoteMayDamageLocalPlayer below), and that holds for as long as
// the CShotInfo and every CFire it starts live, without a timer and without
// holding an engine flag across frames. CShotInfo::Update itself only lights
// fires, and it skips bFireProof peds, which every remote player already is.
//
// So the flame comes out, and since phase two it burns the person standing
// in it as well - but still without this machine deciding anything about
// anybody else. The burning is decided by the victim, about the victim, from
// a fire that is physically in the victim's world: RemoteMayDamageLocalPlayer
// below, and docs/roadmap.md §5.7.
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
	case WEAPONTYPE_FLAMETHROWER:
	case WEAPONTYPE_MOLOTOV:
	case WEAPONTYPE_GRENADE:
		return true;
	default:
		return false;
	}
}

// May something coming off a remote player's ped reduce the local player's
// health on this machine?
//
// Almost never, and the exceptions are the interesting part. Everything a
// remote player legitimately does to us arrives as S_Damage, decided on
// their machine from their own bullet trace (§1.10.1). Anything else that
// names their ped as the culprit is this machine guessing, off a ped that is
// interpolated 100 ms into the past. That is the hole this rule closed and
// nothing below reopens it.
//
// Two causes are let through, and they are the same argument twice.
//
// **The blast** (§1.9.2). An explosion is replayed at a fixed world position
// that its owner chose, so "was I standing in it" is a question about us,
// answered here, with nothing stale in it. Friendly fire does not appear in
// this predicate for the blast because its gate is upstream: combat.cpp flips
// the local player bExplosionProof around the replay, so a friendly's blast
// never reaches InflictDamage at all when the session has it off.
//
// **The fire** (§1.10.6, docs/roadmap.md §5.7), and this is the one that
// changed. WEAPONTYPE_FLAMETHROWER arriving here does not mean "a remote
// player shot us with a flamethrower". In retail 1.0 it means one thing and
// there is no second thing it can mean: *a CFire is burning us, and it
// remembers who lit it*. Proved from the binary rather than assumed - of the
// 21 `call CPed::InflictDamage` sites in the image, exactly two push 9, and
// both of them are inside CFire::ProcessFire:
//
//   0x0047998D   the FindPlayerPed() arm    1.2f * timestep, PEDPIECE_TORSO
//   0x004799B0   the other-ped arm          the same, plus bRenderScorched
//
// So the culprit on this call is a *souvenir*, not an input. The decision was
// "is the local player's position inside a fire that exists in this world",
// taken by this machine about its own player, from its own position, this
// frame. Nothing interpolated goes into it. The remote ped is carried purely
// so the engine's blood, its threat entity and CDarkel's kill register point
// at the player who lit it instead of at nobody - the same reason
// ApplyRemoteDamage names an attacker.
//
// That is why fire is never forwarded as damage and never will be
// (IsForwardableDamage still refuses it): there is nobody to forward it from.
// Most fires have no source at all - a car burning out, a script fire, the
// puddle a molotov leaves - and a rule that only works when somebody owns the
// fire is not a rule about fire.
//
// Friendly fire *is* in the predicate for the fire case, because m_pSource is
// exactly the thing that distinguishes an attack from terrain and it arrives
// here as `damagedBy`:
//
//   source is nil          nobody's fire. Terrain. It burns whoever walks
//                          into it, friendly fire or not - roadmap §5.7 and
//                          M4 both already say so. This predicate never even
//                          sees it, since the caller only asks about a
//                          culprit that resolved to a remote player.
//   source is their ped    their fire, lit deliberately, at us. Same standing
//                          as their bullets and their blast, so the same
//                          switch decides it. The server cannot: fire damage
//                          never passes through it (§1.10.3's exception,
//                          which now covers two things rather than one).
inline bool RemoteMayDamageLocalPlayer(uint8_t weapon, bool friendlyFire) {
	switch (weapon) {
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_MOLOTOV:
	case WEAPONTYPE_GRENADE:
	case WEAPONTYPE_EXPLOSION:
		return true;
	case WEAPONTYPE_FLAMETHROWER:
		return friendlyFire;
	default:
		return false;
	}
}

// Is this damage cause one only a CFire can produce?
//
// One cause, and the list is short because the binary says it is short: the
// only two producers of WEAPONTYPE_FLAMETHROWER in retail 1.0 are
// CFire::ProcessFire's two InflictDamage calls (addresses above). Used to
// tell "fire burned me" apart from "somebody shot me" in the log, which is
// the distinction the last round had no way to make.
inline bool IsFireDamage(uint8_t weapon) {
	return weapon == WEAPONTYPE_FLAMETHROWER;
}

// ---- damage decisions -----------------------------------------------------

// The largest single hit CoopIII will apply from a packet.
//
// Not an attempt at anti-cheat - docs/protocol.md §2.1 says plainly that a
// malicious client can cheat and that's a deliberate trade. This is a bound,
// so a corrupt or hostile float can't reach CPed::m_fHealth and turn it into
// a NaN that then propagates through the ped's matrix into collision. The
// biggest deliberate single hit in the game is CPed::KillPedWithCar's
// 1000.0f, so nothing legitimate is being clipped.
constexpr float MAX_REMOTE_DAMAGE = 1000.0f;

// May a hit with this weapon be taken off the local engine and put on the
// wire as the victim's business?
//
// Yes for everything whose damage the attacker's machine resolved as a ray
// or a melee reach. That's the case where the attacker is genuinely the only
// one who knows: the ray started from their position, at their instant, along
// their aim, and a victim asked to work it out for themselves would be doing
// it from a ped interpolated 100 ms into the past. docs/protocol.md §1.10.1.
//
// No for everything else, and each "no" has its own reason:
//
//   ROCKETLAUNCHER / MOLOTOV / GRENADE / EXPLOSION
//                    already handled, and handled better. An explosion is
//                    replayed at a fixed world position every machine agrees
//                    on, so "was I in it" is a question about the victim,
//                    answered on the victim's machine with nothing stale in
//                    it (§1.9.2). Forwarding it as well would apply it twice.
//   FLAMETHROWER     this cause is never a shot, it's a CFire burning
//                    somebody, and the fire is already in the victim's world
//                    at a position their own engine computed. Deciding it
//                    here would be deciding it twice and worse: CFire hits
//                    once per frame for as long as it burns, so one trigger
//                    pull becomes sixty packets a second. The victim decides
//                    their own burning - RemoteMayDamageLocalPlayer above.
//   RAMMEDBYCAR /    a remote ped is teleported 25 times a second, which is
//   RUNOVERBYCAR     not a motion any collision test was written for, and
//                    CPed::KillPedWithCar's hit is a flat 1000. Driving past
//                    a friend would kill them at random.
//   DROWNING / FALL  these happen to a player on their own machine, where
//                    they're already handled properly. An observer deciding
//                    them is the "born in water" bug from Area B wearing a
//                    different hat.
//
// SNIPERRIFLE is on the yes list even though §1.9.2 refuses to *replay* a
// sniper shot. The two questions aren't the same one: the replay is refused
// because CWeapon::FireSniper fires along the observer's own camera, while
// the damage was resolved on the shooter's machine like any other bullet.
// The result is a sniper that hurts without a flash, which is a cosmetic gap
// rather than a missing weapon.
inline bool IsForwardableDamage(uint8_t weapon) {
	switch (weapon) {
	case WEAPONTYPE_UNARMED:
	case WEAPONTYPE_BASEBALLBAT:
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_AK47:
	case WEAPONTYPE_M16:
	case WEAPONTYPE_SNIPERRIFLE:
	case WEAPONTYPE_UZI_DRIVEBY:
		return true;
	default:
		return false;
	}
}

// ePedPieceTypes and the hit direction, both bounded because both arrive off
// a socket and both steer a switch inside CPed::InflictDamage.
inline bool IsKnownPedPiece(uint8_t piece) { return piece < PEDPIECE_COUNT; }
inline bool IsKnownDamageDirection(uint8_t direction) {
	return direction < PED_DAMAGE_DIRECTIONS;
}

// ---- engine ---------------------------------------------------------------
//
// Nothing below this line is reachable without the game. All five detours
// install together and remove together. A failure to install any one of
// them gets recorded through hook/hook.h's failure list rather than being
// fatal - a session with no muzzle flashes still beats a game that won't
// start at all.
//
// CPed::InflictDamage is the exception to "not fatal", in spirit if not in
// code. Without it nothing can hurt a remote player and the proof flags on
// ped.cpp go back to being the only thing holding the line, which is the
// state M3 left the project in. The log says so rather than refusing to
// start.

// Detours CWeapon::Fire, CExplosion::AddExplosion,
// CProjectileInfo::RemoveProjectile, CPed::InflictDamage and CPed::SetDie.
// Returns false if any failed; the ones that succeeded stay installed and
// the reasons are in HookFailures().
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

// Hurt the local player with somebody else's hit, through the engine's own
// CPed::InflictDamage. `attacker` may be null.
//
// Everything single player does to a hit happens here and not on the
// attacker's machine: the player's own damage multiplier, armour, the hit
// reaction animation, the limb that comes off, and the death. The wire
// carries the argument list, not the outcome.
void ApplyRemoteDamage(RemotePlayer *attacker, const DamageBody &body);

// Kill a remote player's ped with the animation their engine chose.
//
// One way. CPed::SetDie zeroes the health and clears the collision, so the
// route back is a new ped rather than an undo - Client::OnRespawn does that
// through the ordinary spawn path.
void KillRemotePed(RemotePlayer &player, uint16_t animId);

// Whether this session lets players hurt each other. The server enforces it
// by refusing to relay a C_Damage; this covers the blast an observer replays
// for itself, which never goes near the server (docs/protocol.md §1.10.3).
void SetFriendlyFire(bool enabled);

} // namespace coopiii::game
