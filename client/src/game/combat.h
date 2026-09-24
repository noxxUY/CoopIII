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

inline float Dot(const Vec3 &a, const Vec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float LengthSq(const Vec3 &v) { return v.x * v.x + v.y * v.y + v.z * v.z; }

} // namespace vec

// ---- which shots are a ray the engine traces ------------------------------
//
// CWeapon::Fire's jump table at 0x00603184, arms 2/3/5 and 4 and 6: the five
// weapons whose discharge ends in CWeapon::ProcessLineOfSight, and therefore
// the five whose direction the fix below can steer.
//
// The sniper is deliberately absent even though its round is a line too. It is
// not replayed through the engine (IsReplayableWeapon says why) and its line
// comes off the camera rather than off a ray (SniperLineFromCamera), so there
// is nothing here to aim.
//
// The flamethrower is absent because it traces nothing: FireAreaEffect hands
// the shot to CShotInfo, which is a moving volume rather than a line, and it
// has no trail and no impact point. Its direction still comes off the remote
// ped's heading on an observer's machine, which is a known residual rather
// than an oversight.
inline bool IsInstantHitWeapon(uint8_t weapon) {
	switch (weapon) {
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_AK47:
	case WEAPONTYPE_M16:
		return true;
	default:
		return false;
	}
}

// ---- aiming a shot that belongs to somebody else --------------------------
//
// A unit vector, or false if `v` is not a direction. Same guards as
// ProjectileBasis below and for the same reason: every one of these arrives
// off a socket or out of an engine struct, and a NaN that reaches a target
// position becomes a NaN subscript into CWorld::ms_aSectors.
inline bool UnitDirection(const Vec3 &v, Vec3 &out) {
	const float len2 = vec::LengthSq(v);
	// Negated so a NaN, which compares false against everything, is refused
	// by both halves rather than sneaking through one of them.
	if (!(len2 > 1.0e-8f) || !(len2 < 1.0e8f))
		return false;
	const float inv = 1.0f / std::sqrt(len2);
	out             = Vec3{v.x * inv, v.y * inv, v.z * inv};
	return true;
}

// The direction the engine itself derives from a ped's matrix when that ped
// fires: the forward row, flattened to 2D and normalised.
//
// Not an approximation of it - it is the same number. FireInstantHit takes
// `heading = Atan2(-fwd.x, fwd.y)` and then uses `(-Sin(heading),
// Cos(heading))`, and sin(atan2(-x,y)) is -x/r with cos y/r, so the pair is
// (x/r, y/r) with r the 2D length. addresses.h carries the disassembly.
//
// False for a forward row with no horizontal component at all, which a ped
// standing on the ground does not have.
inline bool FlatHeadingDirection(const Vec3 &forward, Vec3 &out) {
	return UnitDirection(Vec3{forward.x, forward.y, 0.0f}, out);
}

// `v` turned by the rotation that takes the unit vector `from` onto the unit
// vector `to`, preserving its length.
//
// Why a rotation rather than "replace the direction": one trigger pull of a
// shotgun is five separate rays, 7.5 degrees apart, and the engine builds each
// one as the ped's heading plus an offset. Rotating the engine's own proposal
// keeps that spread and moves the whole cone onto the shooter's line;
// replacing it would stack all five pellets on one another.
//
// Rodrigues, written so the axis is never normalised: with k = from x to and
// s2 = |k|^2, the usual v*cos + (n x v)*sin + n*(n.v)*(1-cos) becomes
// v*c + (k x v) + k*(k.v)*(1-c)/s2, which has one division and no square
// root.
//
// Two degenerate cases, and they are not the same:
//   from ~= to    nothing to turn. v comes back unchanged.
//   from ~= -to   the axis is undefined - every axis perpendicular to `from`
//                 is a valid answer and they give different results. Rather
//                 than pick one, the whole vector is laid along `to` at its
//                 own length. A shotgun fired while its owner's ped faces
//                 the other way loses its spread for that one shot, which is
//                 a great deal better than losing its direction.
inline bool RotateOnto(const Vec3 &from, const Vec3 &to, const Vec3 &v, Vec3 &out) {
	Vec3 f, t;
	if (!UnitDirection(from, f) || !UnitDirection(to, t))
		return false;

	const float len2 = vec::LengthSq(v);
	if (!(len2 >= 0.0f) || !(len2 < 1.0e12f))
		return false;   // a NaN or an absurd length in the engine's proposal

	const Vec3  k  = vec::Cross(f, t);
	const float s2 = vec::LengthSq(k);
	const float c  = vec::Dot(f, t);

	if (!(s2 > 1.0e-12f)) {
		if (c > 0.0f) {
			out = v;   // already pointing the same way
			return true;
		}
		const float len = std::sqrt(len2);
		out             = Vec3{t.x * len, t.y * len, t.z * len};
		return true;
	}

	const Vec3  kxv   = vec::Cross(k, v);
	const float scale = vec::Dot(k, v) * (1.0f - c) / s2;
	out               = Vec3{v.x * c + kxv.x + k.x * scale,
                             v.y * c + kxv.y + k.y * scale,
                             v.z * c + kxv.z + k.z * scale};
	return true;
}

// ---- which line the wire should carry -------------------------------------
//
// There are three candidate directions for an instant-hit shot, and they are
// not three ways of saying the same thing. Retail's 3rd-person mouse camera
// branch - the one every mouse-aiming player is on - pulls them apart:
//
//   the trail  CBulletTraces::AddTrace(source, target). source is the muzzle
//              and target is the impact. This is the segment the shooter's
//              own screen drew, which is the thing the report is about.
//   the ray    CWeapon::ProcessLineOfSight(point1, point2). On that branch
//              point1 is *not* the muzzle: it is the muzzle projected onto the
//              camera's own axis (CCamera::Find3rdPersonCamTargetVector does
//              `source += Dot(pos - source, target) * target`). So the ray and
//              the trail are two different lines that happen to end near each
//              other.
//   the body   the ped's matrix forward. What an observer would derive on its
//              own, and what the wire carried before docs/protocol.md 1.9.7.
//
// The origin on the wire is the muzzle, because that is where the muzzle flash
// has to come out. So the direction has to be measured from the muzzle too, or
// the observer draws a line parallel to the shooter's ray instead of the one
// the shooter saw - offset by however far the muzzle is off the camera axis,
// which is tens of centimetres and grows into degrees at close range.
//
// Hence the order: the drawn trail first, the traced ray second, the body
// last. Each fallback is a real case rather than defensive padding - a weapon
// that draws no trail (CWeapon::FireM16_1stPerson) still traces a ray, and a
// weapon that does neither still has a ped pointing somewhere.
enum ShotAimSource {
	AIM_FROM_TRAIL = 0,   // the segment the engine drew
	AIM_FROM_RAY,         // the segment the engine tested
	AIM_FROM_BODY,        // the ped's own heading
	AIM_FROM_NOTHING,     // not even that was usable
};

inline ShotAimSource ChooseShotDirection(const Vec3 &trailSum, int trails,
                                         const Vec3 &raySum, int rays,
                                         const Vec3 &forward, Vec3 &out) {
	if (trails > 0 && UnitDirection(trailSum, out))
		return AIM_FROM_TRAIL;
	if (rays > 0 && UnitDirection(raySum, out))
		return AIM_FROM_RAY;
	if (UnitDirection(forward, out))
		return AIM_FROM_BODY;
	out = Vec3{0.0f, 0.0f, 0.0f};
	return AIM_FROM_NOTHING;
}

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

// The most a replayed projectile may be sent off at, in the engine's units a
// step. A bound rather than the engine's number: a thrower's engine sends
// single figures. A speed off the wire that is wrong by orders of magnitude
// puts the object far outside the world on its first physics step, and
// CPhysical::RemoveAndAdd then files it past the end of CWorld::ms_aSectors
// (pedanim.h, ClampToWorld) - clamping the position it starts from does not
// help with where the velocity takes it.
constexpr float REPLAYED_PROJECTILE_MAX_SPEED = 10.0f;

// The velocity a replayed projectile starts with: along the unit `forward`
// ProjectileBasis made of the wire's direction, which may not have been unit
// itself, at the wire's speed held to that bound. False when there is no
// speed to give it, NaN included.
inline bool ReplayedProjectileVelocity(const Vec3 &forward, float speed, Vec3 &out) {
	if (!(speed > 0.0f))
		return false;
	if (!(speed < REPLAYED_PROJECTILE_MAX_SPEED))
		speed = REPLAYED_PROJECTILE_MAX_SPEED;
	out = Vec3{forward.x * speed, forward.y * speed, forward.z * speed};
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

// Does an explosion our player caused go out as C_Explosion? Not a car's own:
// every machine that has the car blows up its copy through the wreck's own
// packet (C_VehicleBlowUp, C_UnownedBlowUp) or its own BlowUpCar, and each of
// those calls AddExplosion itself, so relaying it as well went off twice on
// every other screen.
inline bool RelaysLocalExplosion(uint8_t type) {
	return IsKnownExplosionType(type) && type != EXPLOSION_CAR &&
	       type != EXPLOSION_CAR_QUICK;
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
//   UNARMED / BASEBALLBAT  melee. No projectile, no flash - the swing is
//                          the animation, which the snapshot already
//                          carries, and the hit and the victim's reaction
//                          travel with the damage (melee.h).
//   SNIPERRIFLE            CWeapon::FireSniper returns false unless this
//                          machine's camera is in first-person, and
//                          otherwise fires along that camera's Front. Its
//                          round is heard instead (SniperProbe below).
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
// ---- the sniper's round, heard rather than replayed --------------------------
//
// An observer cannot run CWeapon::FireSniper (above), and would draw nothing
// if it could: CBulletTraces::AddTrace has six callers and the sniper's round
// is none of them (addresses.h), so a sniper leaves no streak anywhere. What
// somebody standing nearby gets from the engine is the report - CWeapon::Fire's
// join point plays SOUND_WEAPON_SHOT_FIRED on the shooter, and the audio picks
// the sample off the weapon in his hand - and the sound of where the round
// landed. That is what the observer plays, along the line the shooter's camera
// was looking down.
//
// The far end is found by the observer's own line-of-sight query, which only
// places a sound. It starts a metre out so it doesn't find the shooter's own
// copy, and stops at SNIPER_PROBE_M, which is the observer's choice rather
// than the engine's: past it the impact is too far off to be told apart.
constexpr float SNIPER_PROBE_SKIP_M = 1.0f;
constexpr float SNIPER_PROBE_M      = 300.0f;

inline bool SniperProbe(const Vec3 &origin, const Vec3 &wireDir, Vec3 &start, Vec3 &end) {
	Vec3 dir;
	if (!UnitDirection(wireDir, dir))
		return false;
	start = Vec3{origin.x + dir.x * SNIPER_PROBE_SKIP_M, origin.y + dir.y * SNIPER_PROBE_SKIP_M,
	             origin.z + dir.z * SNIPER_PROBE_SKIP_M};
	end   = Vec3{origin.x + dir.x * SNIPER_PROBE_M, origin.y + dir.y * SNIPER_PROBE_M,
	             origin.z + dir.z * SNIPER_PROBE_M};
	return true;
}

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

// ---- our flame reaching something another machine owns --------------------
//
// addresses.h, "the flamethrower, from trigger to fire", has the path. The
// short version: the flamethrower never hurts anybody itself. It lights a
// CFire with CFireManager::StartFire, and the CFire does the damage, frame by
// frame, through InflictDamage(m_pSource, 9). By the time that reaches either
// InflictDamage detour it is just "a fire", and a fire can come from anywhere.
// So a flame is recognised at the ignition, not at the damage.
//
// A flame reaching a pedestrian or a car that belongs to another machine goes
// out on the packet that already carries a hit on it - C_PedDamage,
// C_VehicleHit or C_CarHit - with cause 9. On those three packets cause 9
// never meant damage (IsForwardableDamage has always refused it, and every
// receiver checks), so it is free to mean "our flame reached this; light it".
// The owner calls the engine's own StartFire on its own entity, the owner's
// CFire does the burning, and the damage stays where the entity lives.
//
// The owner already does part of this on its own. Every machine replays our
// flamethrower (IsReplayableWeapon), so the owner's engine runs a CShotInfo of
// its own from its copy of our ped and lights whatever that reaches. That copy
// is 100 ms old and aims along its body rather than our camera, so it misses
// things our screen hit. The packet covers those. Both can land on the same
// target, and that is fine: StartFire refuses an entity that is already
// burning (0x004795A6 for a ped, 0x004795DD for a car), so there is still one
// fire and one lot of damage.

// Is this ignition our own flamethrower's?
//
// Both halves are needed, and each one alone is wrong:
//
//   inside CShotInfo::Update   every StartFire made while it runs is the
//                              flamethrower's (0x0055C232, and 0x004B3F9C
//                              through SetCarsOnFire at 0x0055C26C). Nothing
//                              else in the frame runs inside it.
//   fleeFrom is our player     the slot's own m_sourceEntity, passed straight
//                              through. Another player's replayed flame
//                              names their ped here, so it stays theirs.
//
// Why the window is needed and not just the source: our own molotov and
// rocket name our ped too. Their fire comes out of CExplosion::Update
// (SetPedsOnFire 0x0055A71B, SetCarsOnFire 0x0055A739), with m_pCreatorEntity
// as fleeFrom, and every machine replays that explosion and lights its own
// entities from it. Forwarding it would light a fire the owner's own engine
// already decided about. Outside the window, so never forwarded.
//
// And why the source is needed and not just the window: a replayed flame is
// in the window as well. Its m_sourceEntity is our copy of the other player's
// ped, and its owner's machine is the one that forwards it.
//
// What else can light a fire, and why none of it can pass this:
//   CFire::ProcessFire spreading   calls StartFire(FindPlayerPed()) from
//                                  CFireManager::Update, outside the window,
//                                  and only ever onto our own player
//   CAutomobile::BlowUpCar         outside the window; lights the car itself
//   ped.cpp's LightRemoteFire      no StartFire call at all
inline bool IsOurFlame(bool insideShotInfoUpdate, bool fleeFromIsLocalPlayer) {
	return insideShotInfoUpdate && fleeFromIsLocalPlayer;
}

// Does a flame slot reach a ped at this squared distance?
//
// CShotInfo::Update's own test, transcribed. The radius is floored at 1.0f
// (0x0055C148, the constant at 0x00603024) and the squared distance is
// compared against it unsquared (0x0055C1CB) - which is what the engine does,
// not a slip here. A NaN anywhere refuses, the same as the x87 compare.
//
// Needed because a replica pedestrian never gets as far as StartFire on the
// shooter's machine: it is bFireProof, and 0x0055C1D9 skips it one test
// before the call. So for a pedestrian the reach is re-asked, over the same
// list (the shooter's m_nearPeds), with the same position and radius the
// engine has just used, and the proof flag left out - the flag is ours, not
// the pedestrian's. The owner asks the real ped's flag (OwnerLightsFlame).
constexpr float FLAME_MIN_REACH = 1.0f;

inline bool FlameReachesPed(float distSq, float radius) {
	const float r = radius < FLAME_MIN_REACH ? FLAME_MIN_REACH : radius;
	return distSq < r;
}

// On C_PedDamage, C_VehicleHit and C_CarHit: is this an ignition rather than
// a hit? The amount travels as 0 and is never read.
inline bool IsFlameIgnition(uint8_t weapon) { return weapon == WEAPONTYPE_FLAMETHROWER; }

// May the owner light the entity our flame reached?
//
// The two tests the engine makes before StartFire that the shooter could not
// make for it: bFireProof on the real entity (0x0055C1D9 for a ped,
// 0x004B3EFB for a car) and, for a car, SetCarsOnFire's STATUS_WRECKED test.
// Everything else - already burning, not in control, an engine past 225 - is
// StartFire's own, and it answers nil for those.
inline bool OwnerLightsFlame(bool fireProof, bool wrecked) {
	return !fireProof && !wrecked;
}

// How often one target may be reported. The flame reaches the same ped every
// frame for as long as it stands in it, and a burning ped burns for ten
// seconds, so one report a second is plenty and the owner's StartFire refuses
// the repeats anyway.
constexpr uint32_t FLAME_REPORT_EVERY_MS = 1000;

enum class FlameTargetKind : uint8_t {
	Pedestrian,    // C_PedDamage, an ambient netId
	DrivenCar,     // C_VehicleHit, a session vehicle netId
	Traffic,       // C_CarHit, an ambient car netId
};

// Remembers when each target was last reported, so the owner hears about it
// once a second at most. Sixteen rows, oldest recycled: more burning targets
// than that at once only costs an early repeat.
struct FlameReportThrottle {
	struct Row {
		bool            used   = false;
		FlameTargetKind kind   = FlameTargetKind::Pedestrian;
		uint16_t        netId  = 0;
		uint32_t        lastMs = 0;
	};
	static constexpr size_t ROWS = 16;
	Row rows[ROWS];

	// True, and remembered, when this target is due a report at `nowMs`.
	bool Due(FlameTargetKind kind, uint16_t netId, uint32_t nowMs) {
		Row *oldest = &rows[0];
		for (Row &r : rows) {
			if (r.used && r.kind == kind && r.netId == netId) {
				if (nowMs - r.lastMs < FLAME_REPORT_EVERY_MS)
					return false;
				r.lastMs = nowMs;
				return true;
			}
			if (!r.used)
				oldest = &r;
			else if (oldest->used && nowMs - r.lastMs > nowMs - oldest->lastMs)
				oldest = &r;
		}
		oldest->used   = true;
		oldest->kind   = kind;
		oldest->netId  = netId;
		oldest->lastMs = nowMs;
		return true;
	}
};

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
// The round itself is heard on every machine instead (SniperProbe).
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

// ---- a replayed shot hitting something this machine owns -------------------
//
// Every shot is taken twice on the machine that owns what it hit. The
// shooter's engine hits its copy, refuses it and forwards the hit - C_Damage,
// C_PedDamage, C_VehicleHit or C_CarHit - and the owner applies that through
// its own InflictDamage. And the same shot arrives as C_Shot and is replayed
// here through the real CWeapon::Fire, whose round lands in the same
// InflictDamage with the same culprit (our copy of the shooter's ped) and the
// same m_nDamage:
//
//   CWeapon::Fire 0x0055C380
//     FireInstantHit 0x0055D2E0 -> DoBulletImpact 0x0055F950
//                                    ped      0x0055FCEA  CPed::InflictDamage
//                                    vehicle  0x0055FF7A  CVehicle::InflictDamage
//     FireShotgun    0x00560620      ped      0x00561112  CPed::InflictDamage
//                                    vehicle  0x00561259  CVehicle::InflictDamage
//
// So anything with a forwarded path lost twice the health per round whenever
// the replay's ray found it too. The forwarded hit is the one that counts: it
// is what the shooter saw hit, from their own pose and aim. The replay is a
// re-trace from a ped interpolated 100 ms late. It still runs and still draws
// everything - trail, sparks, blood, the flinch and the shotgun knockdown, the
// sound, glass, a loose bin - and only the health is refused.
//
// One thing narrows the ped half, and it is why the pistol and the rifles hit
// fewer pedestrians here than the shotgun did. DoBulletImpact's ped arm is
// `victim type != shooter type || shooter type == PLAYER2` (0x0055FA1D-
// 0x0055FA35, else the jump table at 0x006031DC sends a ped to the arm that
// does nothing). A remote player's ped is PEDTYPE_CIVMALE, so a replayed
// instant-hit round passes through male civilians and hurts everyone else.
// FireShotgun's ped arm (0x00560F2F) has no such test.
//
// Where there is no forwarded path the replay is the only way the shot can
// reach this machine's copy, and it is left alone:
//
//   a hosted ped or car not yet named   no replica anywhere, nobody to forward
//   a ped the session doesn't sync      mission and script characters
//   a car nobody holds                  a parked car, traffic past the hosting
//                                       cap: each machine damages its own copy
//                                       and roadmap.md 5.8 carries the wreck
//
// A session car nobody is driving or settling is not in that list any more.
// The shooter forwards to the session, which makes them its custodian, so
// every other machine refuses the replayed round on it (vehicle.h, Nobody).
//
// Players are not part of this. The local player has been refused inside a
// replay since M3 (the bulletproof flip and the first test in
// HookedInflictDamage), and another machine's ped - player, replica ped or car
// - is refused here whatever fired at it. The local player's reaction to a
// replayed round is kept off as well, which is a separate job: see
// LocalPlayerHitReaction below.
enum class ReplayTarget : uint8_t {
	LocalPlayer,         // C_Damage
	OtherMachines,       // a remote player, a replica ped, a replica or held car
	NamedHostedPed,      // C_PedDamage
	UnnamedHostedPed,
	UnsyncedPed,
	CarWeDrive,          // C_VehicleHit
	CarWeSettle,         // C_VehicleHit, sent to the custodian
	NamedHostedCar,      // C_CarHit
	UnnamedHostedCar,
	NobodysCar,
};

// Does the shooter's machine forward a ray or melee hit on this target to us?
inline bool ShooterForwardsHitOn(ReplayTarget target) {
	switch (target) {
	case ReplayTarget::LocalPlayer:
	case ReplayTarget::NamedHostedPed:
	case ReplayTarget::CarWeDrive:
	case ReplayTarget::CarWeSettle:
	case ReplayTarget::NamedHostedCar:
		return true;
	default:
		return false;
	}
}

// May damage with this cause, dealt inside a replayed shot, land on it here?
//
// Only the causes the shooter would forward are in question. Anything else a
// replay sets off is this machine's own event: a parked car the replayed
// round finishes goes up here, and its blast (cause 18) reaching our peds and
// cars is a blast in our world, which is how every blast is decided.
inline bool ReplayedShotMayDamage(ReplayTarget target, uint8_t cause) {
	if (target == ReplayTarget::LocalPlayer ||
	    target == ReplayTarget::OtherMachines)
		return false;
	if (!IsForwardableDamage(cause))
		return true;
	return !ShooterForwardsHitOn(target);
}

// A pedestrian that has got past the player and replica rules is one of ours.
inline ReplayTarget ClassifyReplayPed(bool hosted, bool named) {
	if (!hosted)
		return ReplayTarget::UnsyncedPed;
	return named ? ReplayTarget::NamedHostedPed : ReplayTarget::UnnamedHostedPed;
}

// May a replayed shot add this explosion here?
//
// Not a projectile's: the thrower decides where theirs went off (§1.9.3).
// Not a barrel's either. BlowUpExplosiveThings names FindPlayerPed() as the
// culprit whoever fired (addresses.h, EXPLOSION_BARREL), so the shooter's
// machine relays its own barrel as C_Explosion - and a replayed round into
// the same barrel here used to blow it a second time and then relay that as
// ours, so both machines got two blasts. The barrel here still takes the
// kick and is marked damaged; the blast comes off the wire.
//
// Everything else goes up. A car the replayed round finishes has to, or
// BlowUpCar is left half done (HookedAddExplosion).
inline bool ReplayMayAddExplosion(int type) {
	if (type < 0)
		return true;
	const uint8_t t = static_cast<uint8_t>(type);
	return !IsProjectileExplosion(t) && t != EXPLOSION_BARREL;
}

// ---- what a round does to the local player's body ---------------------------
//
// The health and the reaction are separate in the engine and they were
// separate here too, by accident. The health is one call, InflictDamage, and
// HookedInflictDamage refuses it for the local player inside a replay. The
// reaction is everything the fire path does before that call: ReactToAttack,
// the flinch that also cancels our own firing, and the shotgun's shove and
// knockdown. None of it tests a proof flag (addresses.h, CPed__ReactToAttack).
// So a teammate's replayed round that crossed our ped on this screen still
// moved us, with friendly fire off.
//
// The reaction goes with the damage. A replayed round never reacts on the
// local player, whatever the session's friendly fire says:
//
//   friendly fire off  the server drops C_Damage, so nothing happens at all
//   friendly fire on   the shooter's C_Damage is the hit that counts, and
//                      ApplyRemoteDamage plays the reaction along with it
//
// That also fixes the opposite case. A round that hit on the shooter's screen
// and missed in our replay used to cost health with no flinch, because the
// replay's ray was the only thing that ever played one.
//
// A forwarded hit plays what the engine's fire path for that weapon plays on
// a player, and no more. ReactToAttack stays out: its gang arm sends anyone
// following us after the shooter's ped for thirty seconds, which would be a
// mission companion opening fire on a teammate.
enum class HitReaction : uint8_t {
	None,
	Flinch,      // ClearAttackByRemovingAnim, then ANIM_SHOT_FRONT_PARTIAL
	Knockdown,   // FireShotgun's shove, and SetFall if the get-up timer allows
};

struct HitReactionRule {
	HitReaction kind      = HitReaction::None;
	bool        inControl = false;   // skipped unless IsPedInControl and not ducking
	bool        holdGate  = false;   // skipped while CPlayerPed's hit anim delay runs
	bool        withDir   = false;   // 1Dh + direction, or always 1Dh
	uint32_t    holdMs    = 0;       // how long the delay is set for afterwards
};

// The engine's own reaction for a round of this weapon reaching a player on
// foot, one row per fire path (addresses.h has the disassembly for each).
// Fists and the bat react through the fight code, which isn't a bullet path,
// so they're None here and melee.h plays theirs.
inline HitReactionRule EngineHitReaction(uint8_t weapon) {
	HitReactionRule r;
	switch (weapon) {
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_UZI:
		// DoBulletImpact, player arm
		r = {HitReaction::Flinch, true, true, true, HIT_ANIM_HOLD_MS};
		break;
	case WEAPONTYPE_AK47:
	case WEAPONTYPE_M16:
		r = {HitReaction::Flinch, true, true, true, HIT_ANIM_HOLD_RIFLE_MS};
		break;
	case WEAPONTYPE_SNIPERRIFLE:
		// CBulletInfo::Update: in control and not ducking, front anim only
		r = {HitReaction::Flinch, true, false, false, 0};
		break;
	case WEAPONTYPE_UZI_DRIVEBY:
		// FireInstantHitFromCar: no gate at all
		r = {HitReaction::Flinch, false, false, true, 0};
		break;
	case WEAPONTYPE_SHOTGUN:
		r.kind = HitReaction::Knockdown;
		break;
	default:
		break;
	}
	return r;
}

// What one round may do to the local player's body on this machine.
//
// `replayed` is a round from ReplayRemoteShot. `seated` is the player in a
// car, where retail's rays find the car and not him, so there's nothing to
// shove or knock over.
inline HitReactionRule LocalPlayerHitReaction(bool replayed, uint8_t weapon, bool seated) {
	if (replayed || seated)
		return HitReactionRule{};
	return EngineHitReaction(weapon);
}

// DoBulletImpact's `cmp [victim+55Ch],now / jae skip`: the flinch only plays
// once the delay is over. Unsigned, as in the binary.
inline bool HitAnimHoldOver(uint32_t holdUntil, uint32_t now) { return holdUntil < now; }

// What ReplayRemoteShot parks the local player's hit anim delay at for the
// length of the replay. HitAnimHoldOver is false for it at every `now`.
constexpr uint32_t REPLAY_HIT_ANIM_HOLD = 0xFFFFFFFFu;

// FireShotgun's `cmp [victim+4C8h],esi / jbe` with esi = now + 0FFFFF448h:
// a ped that got up more than three seconds ago may be knocked down again.
// The subtraction wraps exactly the way the engine's does.
inline bool ShotgunMayKnockDown(uint32_t getUpTimer, uint32_t now) {
	return getUpTimer <= now - GETUP_GRACE_MS;
}

// The flat direction from the player toward whoever shot him, rebuilt from
// CPed::GetLocalDirection's quadrant when there's no ped to measure from.
// 0 is in front and each step is a quarter turn anticlockwise (left, back,
// right), since that function adds 45 degrees to Heading() - m_fRotationCur
// and divides by 90. `flatForward` is the ped's forward row, flattened and
// normalised.
inline Vec3 TowardShooter(const Vec3 &flatForward, uint32_t direction) {
	Vec3 v{flatForward.x, flatForward.y, 0.0f};
	for (uint32_t i = 0; i < (direction & 3u); ++i)
		v = Vec3{-v.y, v.x, 0.0f};
	return v;
}

// ---------------------------------------------------------------------------
// What killed the local player
// ---------------------------------------------------------------------------
//
// Written for one report - "me subí a un auto y me morí" - and kept because
// that report could not be answered from the log. `client: we died (killer net
// 0, anim 173)` was everything CoopIII had to say about it, and anim 173 alone
// is genuinely ambiguous. This turns it into a sentence.
//
// **ANIM_STD_NUM is a fingerprint, and it has exactly two sources.** 0ADh is
// pushed to CPed::SetDie (0x004D37D0) from two places in the whole retail
// image, found by scanning for `68 AD 00 00 00` (four hits, two of which are
// a `call [ebp+5Ch]` and not SetDie at all) and resolving each rel32:
//
//   0x004D0F90  inside CPed::SetGetUp (0x004D0F20, the same function
//               PEDSTATE_GETUP already names from CPed::ProcessControl).
//               `mov dword [ebx+2C0h],0` - m_fHealth = 0 - then
//               SetDie(0ADh, 4.0f, 0.0f). re3 Ped.cpp:5250-5252: the arm taken
//               when a knocked-down ped has under 1.0 health AND its head is
//               not above -0.3, i.e. it is pinned under something. The ramp
//               into it is 40 bytes earlier at 0x004D10E1:
//               `push 0 / push 0 / push [005F8528h] / mov ecx,ebx / push 11h /
//               push 0 / call InflictDamage` -
//               InflictDamage(nil, WEAPONTYPE_RUNOVERBYCAR,
//                             CTimer::GetTimeStep(), PEDPIECE_TORSO, 0),
//               every frame for as long as a car is sitting on the player.
//
//   0x004EADB8  inside CPed::InflictDamage, the in-vehicle arm already
//               transcribed at CPed__InflictDamage in addresses.h.
//
// **And the in-vehicle arm is reachable by one cause only.** At **0x004EADD0**,
// which the `jne` at 0x004EAD85 jumps to when the cause is not drowning, is
// `mov dword [ebp+2C0h],3F800000h / xor al,al` - m_fHealth = 1.0f, return
// false. That is the whole of the `method != WEAPONTYPE_DROWNING` side of
// re3 PedFight.cpp:2408-2460 with no VC_PED_PORTS block in it, which is the
// positive proof that retail 1.0 has none. So:
//
// (This used to say 0x004EADCD, which is three bytes early: 0x004EADCB is the
// drowning arm's own `ret 14h` and 0x004EADCE is an alignment `mov eax,eax`.
// The fact was right, the address was not. CanKillPedInVehicle below carries
// the corrected transcription and clienttest walks it.)
//
//   > A ped with bInVehicle set cannot be killed in retail GTA III 1.0 by
//   > anything except drowning. Every other cause clamps its health to
//   > exactly 1.0f and answers "did not die".
//
// Drowning in a car comes from CAutomobile::ProcessBuoyancy, which at
// 0x00530A9D does `mov ecx,[ebp+1A4h]` (m_pDriver, the offset this file
// already records as VEH_DRIVER) and then
// InflictDamage(nil, 14h, CTimer::ms_fTimeStep, 0, 0), with the passenger
// copy at 0x00530AED.
//
// What that buys the next session: a death "getting into a car" is either the
// car being in water, or the player being on foot with a car on top of them -
// and those two are a very long way apart. The second one is the one CoopIII
// can cause: a replica car is placed by the network, not driven there.
enum class DeathCause : uint8_t {
	UNKNOWN,     // no damage reached us recently; the engine decided alone
	DROWNED,     // WEAPONTYPE_DROWNING - the only in-vehicle death there is
	CRUSHED,     // run over / rammed while on foot, which is what SetGetUp's
	             // ANIM_STD_NUM arm is the end of
	ORDINARY,    // an ordinary cause; the number says which
};

// How stale a recorded cause may be and still be called the reason. Generous
// on purpose: SetGetUp's crush is one InflictDamage per frame, so the last one
// is microseconds old, while a drowning tick can be a frame or two back.
constexpr uint32_t DEATH_CAUSE_WINDOW_MS = 250;

// `haveCause` is false when nothing has damaged us at all this life.
inline DeathCause DeathCauseFor(bool haveCause, uint8_t cause, uint32_t ageMs) {
	if (!haveCause || ageMs > DEATH_CAUSE_WINDOW_MS)
		return DeathCause::UNKNOWN;
	if (cause == WEAPONTYPE_DROWNING)
		return DeathCause::DROWNED;
	if (cause == WEAPONTYPE_RUNOVERBYCAR || cause == WEAPONTYPE_RAMMEDBYCAR)
		return DeathCause::CRUSHED;
	return DeathCause::ORDINARY;
}

// One sentence per combination, because the combination is the finding and a
// reader of the log should not have to hold the disassembly in their head.
inline const char *DeathStory(uint16_t animId, bool inVehicle, DeathCause cause) {
	if (inVehicle) {
		if (cause == DeathCause::DROWNED)
			return "in a car, in the water - the only death the engine has for "
			       "somebody in a seat";
		return "in a car, and NOT by drowning, which retail 1.0 has no path for. "
		       "Either bInVehicle was stale or something wrote the health "
		       "directly; say so, it is a finding";
	}
	if (animId != ANIM_STD_NUM)
		return "on foot, with a die animation, so an ordinary hit";
	if (cause == DeathCause::CRUSHED)
		return "on foot and crushed - knocked down with a car resting on us, "
		       "taking CTimer::GetTimeStep() of RUNOVERBYCAR per frame until "
		       "CPed::SetGetUp gave up. A replica car is placed by the network, "
		       "not driven there";
	return "on foot, ANIM_STD_NUM, and nothing damaged us recently - which "
	       "leaves CPed::SetGetUp deciding we could not get up";
}

// Can a hit with this cause kill a ped who is sitting in a vehicle?
//
// One cause, and it is measured rather than reasoned. `CPed::InflictDamage`
// tests bInVehicle at 0x004EACF3 (`cmp byte [ebp+314h],0 / jne 004EAD80`) and
// the in-vehicle arm immediately asks one question:
//
//   004EAD80  cmp  dword [esp+38h], 14h      method != WEAPONTYPE_DROWNING
//   004EAD85  jne  004EADD0
//   004EAD87  mov  dword [ebp+2C0h], 0       drowning: m_fHealth = 0
//   004EADB8  push 0ADh / call 004D37D0      SetDie(ANIM_STD_NUM, 4.0f, 0.0f)
//   004EADC5  mov  al,1 / ret 14h            "it died"
//   ...
//   004EADD0  mov  dword [ebp+2C0h], 3F800000h   everything else: health = 1.0f
//   004EADDA  xor  al,al / ret 14h               "it did not die"
//
// That is the whole `method != WEAPONTYPE_DROWNING` side of re3
// PedFight.cpp:2408-2460 with no VC_PED_PORTS block in it, which is the
// positive proof that retail 1.0 has none.
//
// > A ped with bInVehicle set cannot be killed in retail GTA III 1.0 by
// > anything except drowning. Every other cause clamps its health to exactly
// > 1.0f and answers "did not die".
//
// It is here as arithmetic so a test can walk it against IsForwardableDamage
// and pin the consequence, which is the thing the pedestrian-damage path must
// not pretend otherwise about: **no cause this wire can carry is able to kill
// a ped in a seat.** Drowning is not forwardable (it happens on the machine the
// ped is drowning on), so the intersection of the two lists is empty. A hit on
// a driver is sent, applied, and clamps him to 1.0f on the machine that owns
// him - which is exactly what single player does, and therefore what every
// screen in the session agrees on.
//
// The address this used to be written as, in the DeathStory block above, was
// 0x004EADCD. That is three bytes early: 0x004EADCB is the `ret 14h` of the
// drowning arm and 0x004EADCE is an alignment `mov eax,eax`. The fact was
// right and the address was not.
inline bool CanKillPedInVehicle(uint8_t weapon) {
	return weapon == WEAPONTYPE_DROWNING;
}

// ePedPieceTypes and the hit direction, both bounded because both arrive off
// a socket and both steer a switch inside CPed::InflictDamage.
inline bool IsKnownPedPiece(uint8_t piece) { return piece < PEDPIECE_COUNT; }
inline bool IsKnownDamageDirection(uint8_t direction) {
	return direction < PED_DAMAGE_DIRECTIONS;
}

// ---- engine ---------------------------------------------------------------
//
// Nothing below this line is reachable without the game. All seventeen detours
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
// CProjectileInfo::RemoveProjectile, CPed::InflictDamage, CPed::SetDie,
// CWeapon::ProcessLineOfSight, CWeapon::DoBulletImpact, CWeapon::DoDoomAiming,
// CBulletTraces::AddTrace, CShotInfo::Update, CFireManager::StartFire,
// CPed::ReactToAttack, CPed::SetFall, CWeapon::FireFromCar, CPed::FightStrike,
// CWeapon::FireMelee and CPed::StartFightDefend.
// Returns false if any failed; the ones that succeeded stay installed and the
// reasons are in HookFailures().
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

// Replay somebody else's shot on their ped, or draw it when it was a drive-by
// round (driveby.h).
void ReplayRemoteShot(RemotePlayer &player, const ShotBody &shot);

// Is one of those replays running right now?
//
// True for exactly the duration of the CWeapon::Fire call ReplayRemoteShot
// makes, which is the only window in which this machine's engine is resolving
// somebody else's bullet. It exists because that bullet can knock a street
// object loose - CWeapon::DoBulletImpact's object arm clears bIsStatic for
// anything with m_fUprootLimit <= 0 - and a bullet writes no collision
// record, so game/object.cpp has no other way to tell an uproot it caused
// from one it is only watching. docs/objects.md 5.
bool ReplayingRemoteShot();

// Is CWeapon::Fire running for the local player's own trigger pull right now?
// For game/heli.cpp: CHeli::TestBulletCollision is called from inside Fire
// and is never told who fired.
bool LocalPlayerFiring();

// Is this CProjectileInfo* one this machine is animating for a remote player?
// For game/heli.cpp: CHeli::TestRocketCollision is handed a position and
// nothing else, and a rocket somebody else fired must not bring down the
// local player's own helicopter here - that player's C_HeliHit will.
bool IsRemotePlayersProjectile(const void *info);

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

// Hurt a pedestrian *this machine hosts* with somebody else's hit, through the
// same CPed::InflictDamage. `attacker` may be null.
//
// The pedestrian half of the function above, and the direction the ambient
// population never had. Everything the engine does to a hit happens here and
// not on the shooter's machine: the ped's own health, the flinch, the limb the
// roll picks, the blood, the weapon it drops and the death. The shooter sent
// the argument list; this machine produces the outcome, and then 17's
// C_PedBodyPart and 18's C_PedDeath carry the visible parts of that outcome
// back out to everybody, including the shooter.
//
// Three things it deliberately does not do.
//
// It does not check whether the ped is already a corpse. `CPed::InflictDamage`
// does, at 0x004EA485 (`[ebp+224h] == 30h or 31h`, DyingOrDead), and returns
// false without touching anything - so a burst that was in flight when the ped
// dropped is refused by the engine that refuses it in single player, rather
// than by a second copy of that rule here.
//
// It does not care about friendly fire. That is a rule about players hurting
// each other (docs/roadmap.md §5.2); a pedestrian is not a player and every
// session lets everybody shoot NPCs.
//
// And it does not make a seated pedestrian die. It cannot: InflictDamage's
// in-vehicle arm clamps m_fHealth to exactly 1.0f at 0x004EADD0 and answers
// "did not die" for every cause but drowning. The hit is applied unchanged and
// the driver survives on one health, which is what retail 1.0 does and
// therefore what every machine in the session agrees on.
void ApplyRemotePedDamage(RemotePlayer *attacker, const PedDamageBody &body);

// Draw a round somebody else's pedestrian fired, on our replica of him
// (protocol.h, C_NpcShot). ReplayRemoteShot's replay, for the five guns that
// trace a ray: whatever it hits here is refused, because his host decided it.
void ReplayAmbientShot(RemoteAmbientPed &ped, const ShotBody &shot);

// Hurt the local player with a hit somebody else's pedestrian landed on our
// copy on his host's machine (C_NpcDamage). ApplyRemoteDamage's path, blamed
// on our replica of him; `attacker` may be null.
void ApplyNpcDamage(RemoteAmbientPed *attacker, const DamageBody &body);

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

// Ends every projectile we are animating for `playerId`, before their ped
// goes (ped.cpp, DespawnRemote).
void EndRemoteProjectilesOf(uint8_t playerId);

} // namespace coopiii::game
