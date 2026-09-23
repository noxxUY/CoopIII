#include "combat.h"

#include "clock.h"
// For CreditRemotePedKill. A hit off the wire that kills one of our own
// pedestrians is the one kill in the game the engine credits to nobody.
#include "darkel.h"
#include "driveby.h"
#include "hook/hook.h"
#include "log.h"
#include "melee.h"
#include "ped.h"
#include "pedanim.h"
// For NoteHostedPedDeath. The CPed::SetDie detour lives here and the ambient
// half of what it witnesses is answered there - see HookedSetDie.
#include "population.h"
// For ReportOurFlameOnCar. A flame reaching a car goes out on the vehicle
// queues, which live there.
#include "vehicle.h"

#include <cstdio>

namespace coopiii::game {

namespace {

// ---- the three detours ----------------------------------------------------
//
// One seam each for the three questions CoopIII needs answered, and every
// one is a place the engine already funnels everything through - none of
// these are places CoopIII invented:
//
//   CWeapon::Fire                     what did the local player just fire,
//                                     and from where - also, on the way in,
//                                     the function an observer calls to
//                                     replay it
//   CExplosion::AddExplosion          where did the local player's grenade,
//                                     molotov or rocket actually go off
//   CProjectileInfo::RemoveProjectile the one place a projectile turns into
//                                     an explosion, and therefore the one
//                                     place to stop an observer deciding that
//   CPed::InflictDamage               everything in the engine that hurts a
//                                     ped goes through here, which makes it
//                                     both the one place to stop this machine
//                                     hurting somebody else's player and the
//                                     one place to learn that it tried
//   CPed::SetDie                      and the one place the engine decides
//                                     which animation a death plays
//   CWeapon::ProcessLineOfSight       the one place every instant-hit path
//                                     states the line it is about to test,
//                                     which is the only honest answer to
//                                     "where did this shot actually go"
//   CWeapon::DoBulletImpact           and the one place the trail's far end
//                                     is chosen, which retail gets wrong for
//                                     one branch out of four
//   CWeapon::DoDoomAiming             the engine's own seam for moving a shot
//                                     that has been aimed and not yet traced
//   CBulletTraces::AddTrace           and the one place the streak a player
//                                     actually sees is decided, which is not
//                                     the same line as the ray above
//   CShotInfo::Update                 the only place a flame is still a
//                                     flame and not just a fire
//   CFireManager::StartFire           and the one call it makes on the thing
//                                     it reached
//   CPed::ReactToAttack               the first thing a round does to a ped
//   CPed::SetFall                     and the last thing a shotgun round does
//                                     to one - both kept off the local player
//                                     for the length of a replay
//   CWeapon::FireFromCar              a drive-by round, which never goes
//                                     near CWeapon::Fire (driveby.h)
//   CPed::FightStrike                 a punch or a kick landing, and
//   CWeapon::FireMelee                a bat landing - the two windows in which
//                                     the fight code reacts on a victim
//   CPed::StartFightDefend            the reaction both of them start with
//                                     (melee.h)

Detour g_fire;
Detour g_explode;
Detour g_removeProjectile;
Detour g_inflictDamage;
Detour g_setDie;
Detour g_lineOfSight;
Detour g_bulletImpact;
Detour g_doomAiming;
Detour g_addTrace;
Detour g_reactToAttack;
Detour g_setFall;
Detour g_fireFromCar;
Detour g_fightStrike;
Detour g_fireMelee;
Detour g_startFightDefend;

// What S_Welcome said about friendly fire (docs/roadmap.md §5.2). Off until
// a session says otherwise, which is also the right answer for a client that
// isn't in one.
bool g_friendlyFire = false;

// Set while CoopIII is driving the engine rather than the player.
//
// Two jobs, both matter. First, it stops a replayed shot from being sampled
// and sent straight back out - that'd be a feedback loop over the network.
// Second, it suppresses CExplosion::AddExplosion outright:
// CWeapon::FireProjectile explodes a throw on the spot when line of sight is
// blocked, and an observer replaying somebody else's throw must not decide
// that for them - the thrower's own machine already hit that same branch and
// its C_Explosion is already on its way.
bool g_replaying = false;

struct ReplayGuard {
	ReplayGuard() { g_replaying = true; }
	~ReplayGuard() { g_replaying = false; }
};

// Set for the length of CWeapon::Fire when the shooter is the local player
// and it isn't a replay. CHeli::TestBulletCollision is called from inside it
// with no shooter argument, and game/heli.cpp needs to know whose bullet it
// is testing.
bool g_localFiring = false;

// Set while CoopIII is applying a hit that already came off the wire.
//
// ApplyRemoteDamage credits the attacker's own ped as the culprit, so the
// engine's blood, its threat entity and CDarkel's kill register all point at
// the player who did it rather than at nobody. That is the right thing to
// pass and it collides head-on with the rule below, which refuses anything a
// remote ped tries to take off the local player. This is how the one hit
// that *is* authorised gets through the rule written to stop all the others.
//
// It cost a whole session to find: with it missing, the shooter converted the
// hit, the server relayed it, the victim called InflictDamage, and the
// victim's own detour threw it away. Nothing crashed and nothing logged.
bool g_applyingRemoteDamage = false;

struct RemoteDamageGuard {
	RemoteDamageGuard() { g_applyingRemoteDamage = true; }
	~RemoteDamageGuard() { g_applyingRemoteDamage = false; }
};

// One line the first time each thing happens, and nothing after that.
//
// Damage is four to ten events a second, so anything logged per hit would
// drown the file. But a feature that silently does nothing is what cost this
// round, and "no line either way" was indistinguishable from "not built". So
// each outcome says itself once, and between them the log answers the only
// question worth asking in one glance: did the shooter decide, did the wire
// carry it, did the victim apply it.
bool g_saidHitSent         = false;
bool g_saidHitApplied      = false;
bool g_saidHitRefused      = false;
bool g_saidHitNotForwarded = false;

// The same four questions again, for a hit on a *pedestrian* somebody else
// hosts, and they need their own lines rather than sharing the ones above.
//
// "El remote sigue sin poder hacerle daño a los NPC del host" was reported
// twice, and what made it hard the second time is that the log for the player
// direction was working perfectly: `our first hit on a remote player` was in
// the file, so the chain looked alive. There was simply no line anywhere for a
// pedestrian, because there was no code - and a direction that does not exist
// looks exactly like a direction that is broken. So each step of this one says
// itself once, and between them they answer the same three-part question:
//
//   sent          the shooter decided a hit on somebody else's pedestrian
//   notForwarded  it decided one and the cause is not the shooter's to decide
//   applied       the machine that owns the ped fed it to its own engine
//   noMove        it fed it in and the ped's health did not move, which is a
//                 different bug in a different place and has to read
//                 differently
//   gone          the report named a pedestrian this machine no longer has
//   inCar         the ped was in a seat, where retail 1.0 clamps it to 1.0f
//                 health and refuses to kill it whatever the hit was
bool g_saidPedHitSent         = false;
bool g_saidPedHitNotForwarded = false;
bool g_saidPedHitApplied      = false;
bool g_saidPedHitNoMove       = false;
bool g_saidPedHitGone         = false;
bool g_saidPedHitInCar        = false;

// A replayed round refused on one of our own named pedestrians, because the
// shooter's C_PedDamage for it is the hit that counts (combat.h,
// ReplayedShotMayDamage). And the barrel equivalent, in HookedAddExplosion.
bool g_saidReplayPedRefused = false;
bool g_saidReplayBarrel     = false;

// A replayed round reached our own ped and wasn't allowed to move him, and a
// forwarded hit played the reaction instead (combat.h, LocalPlayerHitReaction).
bool g_saidReplayReactionFenced = false;
bool g_saidHitReactionPlayed    = false;

// The same idea for fire, and it needs its own lines rather than sharing the
// ones above.
//
// "el fuego no quema" was reported off a session where the flame was visibly
// coming out of a remote player's flamethrower and nothing burned, and the
// log had nothing at all to say about it - not because the chain broke
// quietly, but because no step of it had ever been asked to speak. Fire hits
// once per frame, so nothing here may log per hit; what it can do is say
// which link of the chain it got to, once each.
//
//   burned      the rule let it through and health actually moved
//   noMove      the rule let it through and health did not move, which is a
//               different bug in a different place and has to look different
//   refused     friendly fire is off and this was a player's own fire
bool g_saidFireBurned  = false;
bool g_saidFireNoMove  = false;
bool g_saidFireRefused = false;

// And the same idea again for the replay itself, because this round was lost
// the same way: "la explosion del lanzacohetes llega pero no se syncean las
// balas volando" - the blast arrived, nothing flew, and the log had not one
// word to say about either. ReplayRemoteShot had five places it could return
// without firing and none of them spoke.
//
// Four lines between them answer the only question worth asking about a
// replayed shot, in one glance:
//
//   refused    the replay stopped before CWeapon::Fire, and which gate did it
//   replayed   the engine's own fire path ran for somebody else's shot
//   nothing    it ran, for a projectile weapon, and made nothing to fly
//   flying     it made one, and here is where it was moved to
//
// Plus one more for the far end: how long the first one stayed in the air,
// and whether this machine ended it or its owner did. Two frames and a full
// second look identical on screen - they are opposite bugs.
bool g_saidShotReplayed     = false;
bool g_saidNoProjectile     = false;
bool g_saidProjectileFlying = false;
bool g_saidProjectileEnded  = false;

// And once more for where the bullet went, which is this round's report:
// "el que dispara ve el trail de las balas hacia un lado y el otro player
// tambien las ve pero un poco corridas de lugar".
//
// Four lines, and between them they say which half of the trail was wrong:
//
//   aimed      the first replayed shot we pointed along its owner's own line,
//              and by how many degrees that differed from where this
//              machine's copy of their ped was facing. That number is the
//              size of the bug, measured rather than argued about.
//   notAsked   the engine never called DoDoomAiming for a replayed shot, so
//              it took a branch that does not ask - the shot went along the
//              ped's heading and there was nothing we could do about it
//   noDir      the wire carried something that is not a direction
//   noRay      our own shot traced no line, so the direction we put on the
//              wire is the old ped-forward fallback rather than the aim
//   repaired   retail drew one of our own trails to an uninitialised point
//              and we gave it the ray's far end instead
//   measured   how far apart our own ray and our own trail actually are, in
//              metres and degrees, the first time we fire. That number is the
//              one this round turned on and nobody had ever taken it
bool g_saidAimApplied    = false;
bool g_saidAimNotAsked   = false;
bool g_saidAimNoDir      = false;
bool g_saidNoLocalRay    = false;
bool g_saidTrailRepaired = false;
bool g_saidTrailMeasured = false;
bool g_saidTrailOrigin   = false;
bool g_saidReplayTrail   = false;

// One flag per gate rather than one for all of them, because the gates are
// not alternatives. "No ped yet" is normal in the first second of a session
// and would otherwise be the only refusal ever reported, hiding a weapon
// model that never streams for the rest of the hour.
enum ShotGate {
	GATE_WEAPON = 0,
	GATE_NO_PED,
	GATE_SEATED,
	GATE_DEAD,
	GATE_MODEL,
	GATE_COUNT,
};

bool g_saidGate[GATE_COUNT] = {};

void RefuseShot(ShotGate gate, const char *why, const RemotePlayer &player,
                uint8_t weapon) {
	if (g_saidGate[gate])
		return;
	g_saidGate[gate] = true;
	Log("combat: did not replay a shot for player net %u with weapon %u, because %s. "
	    "Their explosion, if the shot had one, still arrives on its own packet",
	    player.netId, weapon, why);
}

void *PlayerPed() { return Func<void *(__cdecl *)()>(FindPlayerPed)(); }

// How many of gFireManager's 40 slots are alight right now.
//
// Walked rather than read off m_nTotalFires, because that counter is not a
// count: CFire::Extinguish only decrements it for a fire that is not a script
// fire, and StartScriptFire never increments it at all (addresses.h). Forty
// byte reads from a fixed global array, bounded by the engine's own loop
// bound, so it is cheap enough to put in a log line and safe enough to run
// from inside a detour.
uint32_t BurningFires() {
	return CountOngoingFires([](uintptr_t at) {
		return *reinterpret_cast<const uint8_t *>(at) != 0;
	});
}

// ---- the local player's combat events, waiting for the next frame ---------
//
// Written from inside a detour on the game thread, read from PostFrame on
// the same thread - no locking needed. Bounded and drop-oldest rather than
// growing, because the only way this fills up is the drain not running (a
// disconnected client still has its hooks installed), and in that case the
// newest muzzle flash is the one worth keeping.

constexpr uint8_t MAX_PENDING = 32;

CombatEvent g_pending[MAX_PENDING];
uint8_t     g_head    = 0;
uint8_t     g_count   = 0;
uint32_t    g_dropped = 0;

void Push(const CombatEvent &ev) {
	if (g_count == MAX_PENDING) {
		g_head = static_cast<uint8_t>((g_head + 1) % MAX_PENDING);
		--g_count;
		// Once, not once per drop - a stuck drain would otherwise write a
		// log line per bullet.
		if (g_dropped++ == 0)
			Log("combat: the local event queue is full; dropping the oldest");
	}
	g_pending[(g_head + g_count) % MAX_PENDING] = ev;
	++g_count;
}

// ---- projectiles ----------------------------------------------------------

void *ProjInfo(int slot) {
	return reinterpret_cast<void *>(gaProjectileInfo +
	                                static_cast<size_t>(slot) * SIZEOF_PROJECTILEINFO);
}

void *ProjObject(int slot) {
	return reinterpret_cast<void *const *>(CProjectileInfo__ms_apProjectile)[slot];
}

bool ProjInUse(int slot) {
	return Field<uint8_t>(ProjInfo(slot), PROJINFO_IN_USE) != 0;
}

// Which slots are busy right now, so the one CWeapon::Fire is about to fill
// can be identified afterward by taking a difference. CProjectileInfo::
// AddProjectile grabs the first free slot and returns nothing but a bool -
// there's no other way to learn which one it used.
uint32_t InUseMask() {
	uint32_t mask = 0;
	for (int i = 0; i < NUM_PROJECTILES; ++i)
		if (ProjInUse(i))
			mask |= 1u << i;
	return mask;
}

// A projectile CoopIII created for a remote player - animated, not owned.
// `source` is the ped it was created for, and it's the second net: for the
// case where the engine frees a slot through a route that skips
// RemoveProjectile (CProjectileInfo::RemoveAllProjectiles does exactly that
// on a level change), and the slot then gets reused by ordinary gameplay.
struct Tracked {
	bool     active   = false;
	uint8_t  playerId = 0xFF;
	void    *source   = nullptr;
	// CTimer ms at creation, so the one log line about a projectile ending
	// can say how long it was in the air. "Two frames" and "the full second
	// and a bit" are the same event from the outside and completely
	// different bugs.
	uint32_t bornMs   = 0;
};

Tracked g_tracked[NUM_PROJECTILES];

// Set while CoopIII is ending a projectile on purpose, because its owner's
// explosion arrived. Every other removal is this machine's engine deciding,
// and the two have to read differently in the log.
bool g_endingOurs = false;

bool StillOurs(int slot) {
	return g_tracked[slot].active && ProjInUse(slot) &&
	       Field<void *>(ProjInfo(slot), PROJINFO_SOURCE) == g_tracked[slot].source;
}

// The slot a CProjectileInfo* refers to, or -1 if it is not one of ours.
int TrackedSlotOf(const void *info) {
	const uintptr_t addr = reinterpret_cast<uintptr_t>(info);
	if (addr < gaProjectileInfo)
		return -1;
	const uintptr_t delta = addr - gaProjectileInfo;
	if (delta % SIZEOF_PROJECTILEINFO != 0)
		return -1;
	const size_t slot = delta / SIZEOF_PROJECTILEINFO;
	if (slot >= static_cast<size_t>(NUM_PROJECTILES))
		return -1;
	return StillOurs(static_cast<int>(slot)) ? static_cast<int>(slot) : -1;
}

// End one of ours, without an explosion. Goes through the engine's own
// RemoveProjectile, and therefore through the detour below - which is what
// strips the explosion out of it.
void EndTracked(int slot) {
	if (!StillOurs(slot)) {
		g_tracked[slot] = Tracked{};
		return;
	}
	void *const obj = ProjObject(slot);
	if (!obj) {
		g_tracked[slot] = Tracked{};
		return;
	}
	using RemoveFn = void(__cdecl *)(void *, void *);
	g_endingOurs   = true;
	Func<RemoveFn>(CProjectileInfo__RemoveProjectile)(ProjInfo(slot), obj);
	g_endingOurs = false;
}

// ---- putting somebody else's projectile where they threw it ---------------
//
// The velocity was always corrected here; the position and the rotation are
// new, and the position is the one that was costing a whole weapon.
//
// combat.h's ProjectileSpawnsAtThrower carries the disassembly. The short
// version: CProjectileInfo::AddProjectile's rocket arm for a ped that is
// neither the player nor chasing anybody copies the *ped's* matrix and never
// reads the fire source, so a replayed rocket is born inside the remote
// player's own collision instead of a metre out in front of them. It is then
// removed almost immediately and silently, because
//
//   - CProjectileInfo::Update (0x0055B7C0) sweeps a line from m_vecPos to the
//     projectile's current position every frame and removes a rocket whose
//     sweep is not clear. The seven flags it passes at 0x0055B8B5 are
//     buildings, vehicles, peds, objects and three zeroes - peds included,
//     and the only thing CWorld::pIgnoreEntity holds for that sweep is the
//     projectile itself, never the ped that threw it;
//   - the same function removes it outright if bHasCollided is set
//     (0x0055B89E, byte B bit 3), which is what a CObject born inside a ped's
//     collision gets on its first physics step;
//   - and CoopIII's own RemoveProjectile detour blanks the weapon type on the
//     way past, so that removal makes no explosion and no sound. It vanishes.
//
// The owner's C_Explosion still arrives a second later and still plays in the
// right street, which is exactly what was reported: the blast syncs, the
// rocket doesn't.
//
// Three writes plus three engine calls, and none of the three calls is
// optional. ped.cpp's PlaceRemotePed has the long version of why: the CMatrix
// and the RwFrame are separate memory after CEntity::CreateRwObject attached
// them, and CWorld::Add files an entity into the sector grid once and never
// re-reads its position.
void PlaceRemoteProjectile(int slot, const ShotBody &shot) {
	using ThisFn = void(__thiscall *)(void *);

	void *const obj  = ProjObject(slot);
	void *const info = ProjInfo(slot);
	if (!obj)
		return;

	// Rotation first, so the position below is the last word on the matrix.
	// A direction that isn't one leaves the engine's own rotation alone -
	// the rocket then points along the thrower's heading, which is wrong to
	// look at and safe to fly.
	Vec3 right, forward, up;
	if (ProjectileBasis(shot.dir, right, forward, up)) {
		WriteVec3(obj, offs::MATRIX_RIGHT, right);
		WriteVec3(obj, offs::MATRIX_FWD, forward);
		WriteVec3(obj, offs::MATRIX_UP, up);
	}

	// Clamped for the usual reason: CPhysical::RemoveAndAdd below turns x and
	// y into subscripts into CWorld::ms_aSectors with no bounds check
	// (pedanim.h).
	float *const p = &Field<float>(obj, offs::POSITION);
	p[0]           = ClampToWorld(shot.origin.x);
	p[1]           = ClampToWorld(shot.origin.y);
	float z        = p[2];
	FiniteOr(shot.origin.z, p[2], z);
	p[2] = z;

	// The velocity the thrower's own engine computed, not a re-derivation of
	// it. AddProjectile's answer depends on the thrower's heading, whether
	// they are the player, whether they have a seek target and how long the
	// attack button was held, and an observer has none of that.
	float *const velocity = &Field<float>(obj, offs::MOVE_SPEED);
	float        speed    = 0.0f;
	if (FiniteOr(shot.speed, 0.0f, speed) && speed > 0.0f) {
		float d[3];
		if (FiniteOr(shot.dir.x, 0.0f, d[0]) && FiniteOr(shot.dir.y, 0.0f, d[1]) &&
		    FiniteOr(shot.dir.z, 0.0f, d[2])) {
			velocity[0] = d[0] * speed;
			velocity[1] = d[1] * speed;
			velocity[2] = d[2] * speed;
		}
	}

	Func<ThisFn>(CMatrix__UpdateRW)(reinterpret_cast<uint8_t *>(obj) + offs::MATRIX);
	Func<ThisFn>(CEntity__UpdateRwFrame)(obj);
	Func<ThisFn>(CPhysical__RemoveAndAdd)(obj);

	// And the sweep's other end. AddProjectile set m_vecPos to wherever it
	// put the object; leave it and the very first sweep runs from inside the
	// thrower back out to here, through the ped, and ends the rocket before
	// the frame it was created in is even drawn.
	WriteVec3(info, PROJINFO_POS, Vec3{p[0], p[1], p[2]});
}

void EndTrackedFor(uint8_t playerId) {
	for (int i = 0; i < NUM_PROJECTILES; ++i)
		if (g_tracked[i].active && g_tracked[i].playerId == playerId)
			EndTracked(i);
}

// ---- where a shot actually went -------------------------------------------
//
// docs/protocol.md 1.9.7 is the design. The short version:
//
// The origin was always right - it is CWeapon::Fire's own fireSource and it
// has been on the wire since M3. The direction never was. For every ped that
// is not the local player, CWeapon::FireInstantHit builds the shot's target
// out of the *ped's matrix forward*, flattened to 2D, with target.z copied
// from source.z as a dword (addresses.h has both instructions). So a replayed
// shot on an observer's machine:
//
//   - has no pitch at all. Someone firing up at a rooftop draws a horizontal
//     tracer on every screen but their own;
//   - has the yaw of an interpolated ped's *body*, which is neither their aim
//     nor current. At 25 Hz a running player turns several degrees between
//     snapshots, and the aim can be a long way off the body in any case.
//
// Both halves of the report, and neither is the resolution: a trail is two
// world-space points from end to end (addresses.h, CBulletTraces).
//
// So the direction goes on the wire, sampled from the line the shooter's own
// engine traced, and an observer turns the engine's proposal onto it inside
// CWeapon::DoDoomAiming - the one function the fire path hands a writable
// target, whose entire job in retail is to move a shot that has been aimed
// and not yet traced.

// The ray the local player's own engine is testing, for the shot it is in the
// middle of firing.
//
// `sampling` is only true between the entry and the exit of the local
// player's own CWeapon::Fire, so the detour costs one predictable branch for
// every other ped in the city.
//
// The sum is of *unit* directions, one per ray, because one trigger pull is
// not always one ray: a shotgun traces five, 7.5 degrees apart and symmetric
// about the aim, and each one is truncated at whatever it hit, so averaging
// the raw vectors would weight the pellet that flew furthest. Averaging the
// unit directions puts the middle of the cone on the wire, which is what the
// observer's own five pellets then spread around.
struct LocalRay {
	bool         sampling = false;
	int          rays     = 0;
	Vec3         sum{};
	// Where the ray started. On three of FireInstantHit's four branches this
	// is the muzzle; on the 3rd-person mouse camera branch it is the muzzle
	// projected onto the camera's own axis, and that difference is the whole
	// of this round.
	Vec3         nearEnd{};
	// The far end of the last ray, and the pointer the engine handed us for
	// it. The pointer is the evidence, not the value - see HookedDoBulletImpact.
	const float *point2  = nullptr;
	Vec3         farEnd{};

	// And the segment the engine actually *drew*, which is a different line.
	int  trails = 0;
	Vec3 trailSum{};
	Vec3 trailStart{};
	Vec3 trailEnd{};
};

LocalRay g_localRay;

// Where a shot we are replaying for somebody else is supposed to go.
//
// `nominal` is what this machine's engine is about to use on its own - the
// remote ped's flattened forward - and it is read before the fire path runs
// rather than re-derived inside it, because by then it is buried in the x87
// stack. The rotation from nominal to dir is what gets applied.
struct ReplayAim {
	void *ped     = nullptr;
	Vec3  dir{};
	Vec3  nominal{};
	int   applied = 0;
};

ReplayAim g_replayAim;

// __cdecl bool CWeapon::ProcessLineOfSight(CVector const &point1,
//     CVector const &point2, CColPoint &point, CEntity *&entity,
//     eWeaponType type, CEntity *shooter, bool x7).
//
// Read-only. It never changes an argument and never skips the original: this
// is the sampler, and the only thing it is allowed to do is notice.
//
// The bools are uint32_t rather than bool so they are forwarded as the exact
// dwords the caller pushed. The engine reads each as a byte, so it would not
// matter either way, and "would not matter" is not a thing to be relying on
// in a function that is called for every shot in the city.
using LineOfSightFn = bool(__cdecl *)(const float *, const float *, void *, void **,
                                      uint32_t, void *, uint32_t, uint32_t, uint32_t,
                                      uint32_t, uint32_t, uint32_t, uint32_t);

bool __cdecl HookedProcessLineOfSight(const float *point1, const float *point2,
                                      void *point, void **entity, uint32_t type,
                                      void *shooter, uint32_t buildings,
                                      uint32_t vehicles, uint32_t peds, uint32_t objects,
                                      uint32_t dummies, uint32_t seeThrough,
                                      uint32_t someObjects) {
	if (g_localRay.sampling && point1 && point2) {
		Vec3 unit;
		if (UnitDirection(Vec3{point2[0] - point1[0], point2[1] - point1[1],
		                       point2[2] - point1[2]},
		                  unit)) {
			g_localRay.sum.x += unit.x;
			g_localRay.sum.y += unit.y;
			g_localRay.sum.z += unit.z;
			++g_localRay.rays;
		}
		g_localRay.point2  = point2;
		g_localRay.nearEnd = Vec3{point1[0], point1[1], point1[2]};
		g_localRay.farEnd  = Vec3{point2[0], point2[1], point2[2]};
	}

	return g_lineOfSight.Original<LineOfSightFn>()(point1, point2, point, entity, type,
	                                               shooter, buildings, vehicles, peds,
	                                               objects, dummies, seeThrough,
	                                               someObjects);
}

// __thiscall void CWeapon::DoBulletImpact(CEntity *shooter, CEntity *victim,
//     CVector *source, CVector *target, CColPoint *point, CVector2D ahead).
//
// Here for one reason, and it is a retail bug rather than a CoopIII one.
//
// FireInstantHit's 3rd-person-mouse-camera branch traces its ray through its
// own `src`/`trgt` locals and never writes the `target` local. The shared
// tail passes `&target` here anyway, and the no-victim arm of this function
// is `CBulletTraces::AddTrace(source, target)`. So every mouse-aimed shot
// that hits nothing draws its trail from the muzzle to whatever the last
// caller happened to leave in that stack slot - which is most likely a target
// from an earlier shot, i.e. a trail that points somewhere the player was
// aiming a moment ago. "El que dispara ve el trail de las balas hacia un
// lado", exactly.
//
// The test for it is exact rather than a tolerance, which is the only reason
// this detour is here at all. Three of FireInstantHit's four branches hand
// `&target` to ProcessLineOfSight themselves, so the pointer the sampler above
// recorded *is* the pointer arriving here. The fourth hands it `&trgt`. So:
// same pointer means the branch that ran wrote it, different pointer means
// nothing did. No distance, no epsilon, no guess.
//
// Strictly cosmetic. The far end written here has already been traced; only
// the line that gets drawn changes.
using ImpactThisFn = void(__thiscall *)(void *, void *, void *, float *, float *, void *,
                                        float, float);
using ImpactHookFn = void(__fastcall *)(void *, void *, void *, void *, float *, float *,
                                        void *, float, float);

void __fastcall HookedDoBulletImpact(void *self, void * /*edx*/, void *shooter,
                                     void *victim, float *source, float *target,
                                     void *point, float aheadX, float aheadY) {
	if (g_localRay.sampling && !victim && target && g_localRay.rays > 0 &&
	    static_cast<const float *>(target) != g_localRay.point2) {
		target[0] = g_localRay.farEnd.x;
		target[1] = g_localRay.farEnd.y;
		target[2] = g_localRay.farEnd.z;

		if (!g_saidTrailRepaired) {
			g_saidTrailRepaired = true;
			Log("combat: retail was about to draw one of our own bullet trails to an "
			    "uninitialised point - CWeapon::FireInstantHit's mouse-camera branch "
			    "traces its own locals and never writes the target the tail draws. "
			    "Gave it the far end of the ray it actually tested, (%.1f %.1f %.1f). "
			    "This one is single player's bug too, and it is why the shooter's own "
			    "trail wanders",
			    g_localRay.farEnd.x, g_localRay.farEnd.y, g_localRay.farEnd.z);
		}
	}

	g_bulletImpact.Original<ImpactHookFn>()(self, nullptr, shooter, victim, source,
	                                        target, point, aheadX, aheadY);
}

// ---- fists and the bat, on the striker's machine -----------------------------
//
// melee.h has the split. Set for the length of a FightStrike or FireMelee
// call, whoever is striking: a copy of another machine's ped never reacts to
// a melee hit here, ours or a local NPC's, because its owner plays that.
struct MeleeCall {
	bool     active     = false;
	void    *striker    = nullptr;
	MeleeTag tag        = {};
	bool     adrenaline = false;   // the striker's, for SwingAmountForPlayer
};
MeleeCall g_melee;

bool g_saidMeleeFenced  = false;
bool g_saidMeleeApplied = false;

bool IsOtherMachinesPed(const void *ped) {
	uint16_t netId = INVALID_NETID;
	return RemotePlayerForPed(ped, netId) || AmbientReplicaForPed(ped, netId);
}

bool MeleeKeepsOff(const void *ped) {
	return ped && !CopyMayReactToMelee(g_melee.active, IsOtherMachinesPed(ped));
}

void SayMeleeFenced(const char *what) {
	if (g_saidMeleeFenced)
		return;
	g_saidMeleeFenced = true;
	Log("combat: kept %s off another machine's ped during a melee hit. Its owner "
	    "plays the reaction from the hit we forward", what);
}

// The fight code shoves a struck ped and knocks him off his feet with plain
// writes. On another machine's peds those are put back when the call returns,
// the same way ReplayBodyFence does it for a replayed round.
class MeleeScope {
public:
	MeleeScope(void *striker, const MeleeTag &tag, bool adrenaline) : m_prev(g_melee) {
		g_melee.active     = true;
		g_melee.striker    = striker;
		g_melee.tag        = tag;
		g_melee.adrenaline = adrenaline;

		if (!striker)
			return;
		const uint16_t count = Field<uint16_t>(striker, PED_NUM_NEAR_PEDS);
		for (uint16_t i = 0; i < count && i < PED_NEAR_PEDS_MAX; ++i) {
			void *const ped = Field<void *>(striker, PED_NEAR_PEDS + i * 4u);
			if (!ped || !IsOtherMachinesPed(ped))
				continue;
			Row &r  = m_rows[m_count++];
			r.ped   = ped;
			r.speed = ReadVec3(ped, offs::MOVE_SPEED);
			r.flags = Field<uint8_t>(ped, offs::PED_FLAGS);
		}
	}
	~MeleeScope() {
		for (int i = 0; i < m_count; ++i) {
			const Row &r = m_rows[i];
			WriteVec3(r.ped, offs::MOVE_SPEED, r.speed);
			uint8_t &flags = Field<uint8_t>(r.ped, offs::PED_FLAGS);
			flags = static_cast<uint8_t>((flags & ~offs::PED_IS_STANDING) |
			                             (r.flags & offs::PED_IS_STANDING));
		}
		g_melee = m_prev;
	}
	MeleeScope(const MeleeScope &)            = delete;
	MeleeScope &operator=(const MeleeScope &) = delete;

private:
	struct Row {
		void   *ped   = nullptr;
		Vec3    speed = {};
		uint8_t flags = 0;
	};
	MeleeCall m_prev;
	Row       m_rows[PED_NEAR_PEDS_MAX];
	int       m_count = 0;
};

// The melee bytes for a hit InflictDamage is being asked to make right now,
// if it is the local player's strike or swing.
MeleeTag LocalMeleeTag(void *damagedBy, uint32_t method) {
	if (!g_melee.active || !damagedBy || damagedBy != g_melee.striker ||
	    damagedBy != PlayerPed() || !IsMeleeCause(static_cast<uint8_t>(method)))
		return MeleeTag{};
	return g_melee.tag;
}

// ---- keeping a replayed round off the local player's body -------------------
//
// combat.h, LocalPlayerHitReaction, and addresses.h (CPed__ReactToAttack) for
// the order the engine runs things in. Four reactions reach a ped a round hits
// before the damage call, and each is stopped for the local player where it's
// cheapest to stop exactly:
//
//   ReactToAttack      detour below. The look, and the order to our followers
//   SetFall            detour below. The shotgun knockdown
//   the flinch         DoBulletImpact's own gate, the hit anim delay, held
//                      shut by ReplayBodyFence. It also covers
//                      ClearAttackByRemovingAnim, which is what stopped us
//                      firing
//   ApplyMoveForce     the shotgun shove. ReplayBodyFence puts the move speed
//                      and bIsStanding back afterwards, since the local player
//                      isn't processed while CWeapon::Fire runs
//
// Every one of them is only for the local player. Anything else the replay
// hits reacts the way it always did: a pedestrian this machine hosts still
// flinches and falls, and ReplayedShotMayDamage still decides its health.
// The trail, the blood, the sound and the event are untouched.

// __thiscall void CPed::ReactToAttack(CEntity *attacker), `ret 4`.
using ReactHookFn = void(__fastcall *)(void *, void *, void *);

void __fastcall HookedReactToAttack(void *self, void * /*edx*/, void *attacker) {
	if (g_replaying && self && self == PlayerPed()) {
		if (!g_saidReplayReactionFenced) {
			g_saidReplayReactionFenced = true;
			Log("combat: a replayed round from a remote player hit our own ped on this "
			    "screen and was kept from moving us. If it hit on their screen too, "
			    "their C_Damage brings the reaction along with the health");
		}
		return;
	}
	if (MeleeKeepsOff(self)) {
		SayMeleeFenced("ReactToAttack");
		return;
	}
	g_reactToAttack.Original<ReactHookFn>()(self, nullptr, attacker);
}

// __thiscall void CPed::SetFall(int time, AnimationId anim, bool bSkipAnim),
// `ret 0Ch`.
using SetFallThisFn = void(__thiscall *)(void *, int32_t, uint32_t, uint8_t);
using SetFallHookFn = void(__fastcall *)(void *, void *, int32_t, uint32_t, uint8_t);

void __fastcall HookedSetFall(void *self, void * /*edx*/, int32_t time, uint32_t anim,
                              uint8_t skipAnim) {
	if (g_replaying && self && self == PlayerPed())
		return;
	if (MeleeKeepsOff(self)) {
		SayMeleeFenced("the knockdown");
		return;
	}
	g_setFall.Original<SetFallHookFn>()(self, nullptr, time, anim, skipAnim);
}

// __thiscall void CPed::StartFightDefend(uint8 dir, uint8 hitLevel, uint8 x),
// `ret 0Ch`. The callers push whole registers, so the three are passed on as
// they came.
using DefendThisFn = void(__thiscall *)(void *, uint32_t, uint32_t, uint32_t);
using DefendHookFn = void(__fastcall *)(void *, void *, uint32_t, uint32_t, uint32_t);

void __fastcall HookedStartFightDefend(void *self, void * /*edx*/, uint32_t dir,
                                       uint32_t hitLevel, uint32_t x) {
	if (MeleeKeepsOff(self)) {
		SayMeleeFenced("the defend");
		return;
	}
	g_startFightDefend.Original<DefendHookFn>()(self, nullptr, dir, hitLevel, x);
}

// __thiscall bool CPed::FightStrike(CVector &node), `ret 4`.
using StrikeHookFn = bool(__fastcall *)(void *, void *, void *);

// A copy of another machine's ped never lands a blow here. Nothing is meant to
// get one this far - no objective, never PED_FIGHT or PED_ATTACK on its own -
// and this is what makes that a rule rather than a hope.
bool CopyMayStrike(void *striker) {
	if (!striker || !IsOtherMachinesPed(striker))
		return true;
	static bool said = false;
	if (!said) {
		said = true;
		Log("combat: another machine's ped tried to land a melee hit on this machine "
		    "and was stopped. Its owner decides its punches");
	}
	return false;
}

bool __fastcall HookedFightStrike(void *self, void * /*edx*/, void *node) {
	if (!CopyMayStrike(self))
		return false;
	MeleeTag tag;
	if (self) {
		const int32_t move  = Field<int32_t>(self, offs::PED_FIGHT_MOVE);
		const uint8_t level = IsFightMove(move)
		                          ? *reinterpret_cast<const uint8_t *>(
		                                tFightMoves + static_cast<size_t>(move) * SIZEOF_FIGHTMOVE +
		                                FIGHTMOVE_HIT_LEVEL)
		                          : 0;
		// FightStrike's own `m_weapons[m_currentWeapon].m_eWeaponType != 0`.
		const uint8_t slot  = Field<uint8_t>(self, offs::PED_CURRENT_WEAPON);
		const bool    armed = slot < offs::NUM_WEAPON_SLOTS &&
		                   Field<uint32_t>(self, offs::PED_WEAPONS +
		                                             slot * offs::SIZEOF_WEAPON +
		                                             offs::WEAPON_TYPE) != WEAPONTYPE_UNARMED;
		tag = StrikeTag(move, level, armed);
	}
	MeleeScope scope(self, tag, false);
	return g_fightStrike.Original<StrikeHookFn>()(self, nullptr, node);
}

// __thiscall bool CWeapon::FireMelee(CEntity *shooter, CVector &source),
// `ret 8`. `self` is the weapon.
using SwingHookFn = bool(__fastcall *)(void *, void *, void *, void *);

bool __fastcall HookedFireMelee(void *self, void * /*edx*/, void *shooter, void *source) {
	void *striker    = nullptr;
	bool  heavy      = false;
	bool  adrenaline = false;
	if (self && shooter &&
	    (Field<uint8_t>(shooter, offs::ENTITY_FLAGS) & 7) == offs::ENTITY_TYPE_PED) {
		if (!CopyMayStrike(shooter))
			return true;   // what FireMelee answers, having hit nobody
		striker = shooter;
		// anim2Playing, the way FireMelee reads it at 0x0055CA57.
		const uint32_t type = Field<uint32_t>(self, offs::WEAPON_TYPE);
		void *const clump   = Field<void *>(shooter, offs::RW_OBJECT);
		if (type <= WEAPONTYPE_LAST_INVENTORY && clump) {
			using InfoFn  = void *(__cdecl *)(uint32_t);
			using AssocFn = void *(__cdecl *)(void *, uint32_t);
			void *const info = Func<InfoFn>(CWeaponInfo__GetWeaponInfo)(type);
			heavy = info && Func<AssocFn>(RpAnimBlendClumpGetAssociation)(
			                    clump, Field<uint32_t>(info, WEAPONINFO_ANIM2_TO_PLAY)) != nullptr;
		}
		adrenaline = shooter == PlayerPed() &&
		             Field<uint8_t>(shooter, offs::PLAYER_ADRENALINE) != 0;
	}
	MeleeScope scope(striker, SwingTag(heavy), adrenaline);
	return g_fireMelee.Original<SwingHookFn>()(self, nullptr, shooter, source);
}

// Held for the length of the CWeapon::Fire call in ReplayRemoteShot.
//
// Only fields the fire path writes on a struck ped, and only the local
// player's. The hit anim delay is parked where DoBulletImpact's `jae` can't
// pass it and put back to whatever it was, so a real flinch that was already
// holding keeps its own timer.
class ReplayBodyFence {
public:
	explicit ReplayBodyFence(void *ped) : m_ped(ped) {
		if (!m_ped)
			return;
		m_hold  = Field<uint32_t>(m_ped, offs::PLAYER_HIT_ANIM_DELAY);
		m_speed = ReadVec3(m_ped, offs::MOVE_SPEED);
		m_flags = Field<uint8_t>(m_ped, offs::PED_FLAGS);
		Field<uint32_t>(m_ped, offs::PLAYER_HIT_ANIM_DELAY) = REPLAY_HIT_ANIM_HOLD;
	}
	~ReplayBodyFence() {
		if (!m_ped)
			return;
		Field<uint32_t>(m_ped, offs::PLAYER_HIT_ANIM_DELAY) = m_hold;
		Field<float>(m_ped, offs::MOVE_SPEED)     = m_speed.x;
		Field<float>(m_ped, offs::MOVE_SPEED + 4) = m_speed.y;
		Field<float>(m_ped, offs::MOVE_SPEED + 8) = m_speed.z;
		uint8_t &flags = Field<uint8_t>(m_ped, offs::PED_FLAGS);
		flags          = static_cast<uint8_t>((flags & ~offs::PED_IS_STANDING) |
                                     (m_flags & offs::PED_IS_STANDING));
	}
	ReplayBodyFence(const ReplayBodyFence &)            = delete;
	ReplayBodyFence &operator=(const ReplayBodyFence &) = delete;

private:
	void    *m_ped   = nullptr;
	uint32_t m_hold  = 0;
	Vec3     m_speed = {};
	uint8_t  m_flags = 0;
};

// The reaction the engine's fire path would have played on the local player
// for a hit that arrived as C_Damage, played the way that path plays it.
// Called ahead of InflictDamage, which is the engine's order as well.
void PlayForwardedHitReaction(void *ped, void *attackerPed, uint8_t weapon,
                              uint32_t direction) {
	const HitReactionRule rule =
	    LocalPlayerHitReaction(false, weapon, Field<bool>(ped, offs::PED_IN_VEHICLE));
	if (rule.kind == HitReaction::None)
		return;

	// Every one of those arms skips a dying or dead ped before it reacts
	// (0x0055FA52, 0x00560F42, 0x005586AE, 0x00562B23).
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return;

	void *const clump = Field<void *>(ped, offs::RW_OBJECT);
	if (!clump)
		return;   // nothing to animate, and SetFall would blend on it too

	const uint32_t now = Global<uint32_t>(CTimer__m_snTimeInMilliseconds);

	if (rule.kind == HitReaction::Flinch) {
		using InControlFn = bool(__thiscall *)(void *);
		using ClearFn     = void(__thiscall *)(void *);
		using AddFn       = void *(__cdecl *)(void *, int, int);

		if (rule.inControl &&
		    (!Func<InControlFn>(CPed__IsPedInControl)(ped) ||
		     (Field<uint8_t>(ped, offs::PED_FLAGS_E) & offs::PED_IS_DUCKING)))
			return;
		if (rule.holdGate &&
		    !HitAnimHoldOver(Field<uint32_t>(ped, offs::PLAYER_HIT_ANIM_DELAY), now))
			return;

		Func<ClearFn>(CPed__ClearAttackByRemovingAnim)(ped);
		const uint32_t anim = ANIM_SHOT_FRONT_PARTIAL + (rule.withDir ? direction : 0u);
		void *const assoc   = Func<AddFn>(CAnimManager__AddAnimation)(
            clump, ASSOCGRP_STD, static_cast<int>(anim));
		if (assoc) {
			Field<float>(assoc, ANIM_BLEND_AMOUNT) = 0.0f;
			Field<float>(assoc, ANIM_BLEND_DELTA)  = HIT_ANIM_BLEND_DELTA;
		}
		if (rule.holdGate)
			Field<uint32_t>(ped, offs::PLAYER_HIT_ANIM_DELAY) = now + rule.holdMs;
	} else {
		using PushFn = void(__thiscall *)(void *, float, float, float);

		// FireShotgun pushes along the line from the fire source. The
		// attacker's ped stands in for it; without one, the direction the
		// shooter's engine worked out is turned back into a line.
		const Vec3 pos = ReadVec3(ped, offs::POSITION);
		Vec3       toward{};
		bool       have = false;
		if (attackerPed) {
			const Vec3 at = ReadVec3(attackerPed, offs::POSITION);
			have = UnitDirection(Vec3{at.x - pos.x, at.y - pos.y, 0.0f}, toward);
		}
		if (!have) {
			Vec3 fwd;
			if (!FlatHeadingDirection(ReadVec3(ped, offs::MATRIX_FWD), fwd))
				return;
			toward = TowardShooter(fwd, direction);
		}

		const bool mayFall = ShotgunMayKnockDown(Field<uint32_t>(ped, offs::PED_GETUP_TIMER), now);
		uint8_t   &flags   = Field<uint8_t>(ped, offs::PED_FLAGS);
		if ((flags & offs::PED_IS_STANDING) && mayFall) {
			flags = static_cast<uint8_t>(flags & ~offs::PED_IS_STANDING);
			Func<PushFn>(CPhysical__ApplyMoveForce)(ped, toward.x * SHOTGUN_PUSH_FALL,
			                                        toward.y * SHOTGUN_PUSH_FALL,
			                                        SHOTGUN_PUSH_FALL_Z);
		} else {
			Func<PushFn>(CPhysical__ApplyMoveForce)(ped, toward.x * SHOTGUN_PUSH_STAND,
			                                        toward.y * SHOTGUN_PUSH_STAND, 0.0f);
		}
		if (mayFall)
			Func<SetFallThisFn>(CPed__SetFall)(ped, SHOTGUN_FALL_MS,
			                                   ANIM_KO_SKID_FRONT + direction, 0);
	}

	if (!g_saidHitReactionPlayed) {
		g_saidHitReactionPlayed = true;
		Log("combat: played the engine's own hit reaction for a forwarded hit, cause %u "
		    "direction %u (%s). The replay of the same round doesn't move us any more, "
		    "so this is the only place it comes from",
		    weapon, direction,
		    rule.kind == HitReaction::Knockdown ? "shotgun shove" : "flinch");
	}
}

// ---- fists and the bat, on the machine that owns the victim -------------------
//
// melee.h. The struck ped's half of FightStrike or FireMelee, played on the
// real ped from the two melee bytes, in the engine's order around the one
// InflictDamage call the caller makes.
using ReactThisFn = void(__thiscall *)(void *, void *);
using PushThisFn  = void(__thiscall *)(void *, float, float, float);

struct MeleeReaction {
	bool    play       = false;
	float   before     = 0.0f;
	uint8_t damageMult = 0;
};

// Flat unit vector from the victim toward whoever hit him: his copy here when
// there is one, else the side the striker's engine said it came from.
bool TowardAttacker(void *ped, void *attackerPed, uint32_t direction, Vec3 &out) {
	if (attackerPed) {
		const Vec3 pos = ReadVec3(ped, offs::POSITION);
		const Vec3 at  = ReadVec3(attackerPed, offs::POSITION);
		if (UnitDirection(Vec3{at.x - pos.x, at.y - pos.y, 0.0f}, out))
			return true;
	}
	Vec3 fwd;
	if (!FlatHeadingDirection(ReadVec3(ped, offs::MATRIX_FWD), fwd))
		return false;
	out = TowardShooter(fwd, direction);
	return true;
}

// Up to the damage: both paths react and defend first. False when the engine
// would have left this victim alone, damage and all.
bool MeleeBeforeDamage(void *ped, bool isPlayer, void *attackerPed, const MeleeTag &tag,
                       uint8_t weapon, float amount, uint32_t direction, MeleeReaction &out) {
	out = MeleeReaction{};
	if (MeleeKind(tag) == MELEE_NONE)
		return true;

	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (MeleeSparesVictim(isPlayer, state))
		return false;

	// In a seat the defend needs him in control and the fall and the shove
	// have nothing to move. The damage still goes in, and the engine clamps it.
	if (Field<bool>(ped, offs::PED_IN_VEHICLE) || !Field<void *>(ped, offs::RW_OBJECT))
		return true;

	out.play       = true;
	out.before     = Field<float>(ped, offs::PED_HEALTH);
	out.damageMult = StrikeDamageMult(amount);

	// Not on a player. His ReactToAttack is his gang turning on the attacker,
	// kept out for the same reason PlayForwardedHitReaction keeps it out.
	if (!isPlayer && attackerPed && !PedStateDyingOrDead(state))
		Func<ReactThisFn>(CPed__ReactToAttack)(ped, attackerPed);

	Func<DefendThisFn>(CPed__StartFightDefend)(ped, direction,
	                                          DefendHitLevel(tag, weapon, state),
	                                          DefendArg(tag, isPlayer, out.damageMult));
	return true;
}

// After the damage: the fall and the shove, off the health either side of it.
void MeleeAfterDamage(void *ped, bool isPlayer, void *attackerPed, const MeleeTag &tag,
                      uint8_t weapon, uint32_t direction, const MeleeReaction &r) {
	if (!r.play)
		return;

	const float    after  = Field<float>(ped, offs::PED_HEALTH);
	uint32_t       state  = Field<uint32_t>(ped, offs::PED_STATE);
	uint8_t       &flags  = Field<uint8_t>(ped, offs::PED_FLAGS);
	const uint32_t koAnim = ANIM_KO_SKID_FRONT + direction;
	Vec3           toward{};
	const bool     haveDir = TowardAttacker(ped, attackerPed, direction, toward);

	if (MeleeKind(tag) == MELEE_STRIKE) {
		void *const stats  = Field<void *>(ped, offs::PED_STATS);
		const bool  oneHit = stats && (Field<uint16_t>(stats, offs::PEDSTATS_FLAGS) &
		                               STAT_ONE_HIT_KNOCKDOWN) != 0;
		if (StrikeKnocksDown(state, r.before, after, isPlayer,
		                     (tag.melee & MELEE_ARMED) != 0, oneHit)) {
			Func<SetFallThisFn>(CPed__SetFall)(ped, 0, koAnim, 0);
			if (Field<uint32_t>(ped, offs::PED_STATE) == PEDSTATE_FALL)
				flags = static_cast<uint8_t>(flags & ~offs::PED_IS_STANDING);
		}
		state = Field<uint32_t>(ped, offs::PED_STATE);
		if ((state == PEDSTATE_DIE || !(flags & offs::PED_IS_STANDING)) && haveDir) {
			flags = static_cast<uint8_t>(flags & ~offs::PED_IS_STANDING);
			const float k = StrikePushScale((tag.melee & MELEE_GROUND_KICK) != 0,
			                                state == PEDSTATE_DIE, r.damageMult);
			Func<PushThisFn>(CPhysical__ApplyMoveForce)(ped, -toward.x * k, -toward.y * k, k);
		}
		return;
	}

	const bool bat   = weapon == WEAPONTYPE_BASEBALLBAT;
	const bool heavy = (tag.melee & MELEE_HEAVY) != 0;
	if (!PedStateOnGround(state)) {
		if (!SwingKnocksDown(state, r.before, after, bat, isPlayer))
			return;
		if (haveDir) {
			flags = static_cast<uint8_t>(flags & ~offs::PED_IS_STANDING);
			Func<PushThisFn>(CPhysical__ApplyMoveForce)(ped, toward.x * SWING_PUSH,
			                                            toward.y * SWING_PUSH, SWING_PUSH_Z);
		}
		Func<SetFallThisFn>(CPed__SetFall)(ped, SwingFallMs(bat, isPlayer), koAnim, 0);
	} else if (SwingShovesDying(state, heavy) && haveDir) {
		flags = static_cast<uint8_t>(flags & ~offs::PED_IS_STANDING);
		Func<PushThisFn>(CPhysical__ApplyMoveForce)(ped, toward.x * SWING_PUSH,
		                                            toward.y * SWING_PUSH, SWING_PUSH_Z);
	}
}

void SayMeleeApplied(const MeleeTag &tag, uint8_t weapon, bool player) {
	if (g_saidMeleeApplied)
		return;
	g_saidMeleeApplied = true;
	Log("combat: played the struck side of a %s off the wire on our own %s (cause %u, "
	    "level %u)", MeleeKind(tag) == MELEE_STRIKE ? "punch" : "swing",
	    player ? "player" : "pedestrian", weapon, tag.hitLevel);
}

// __cdecl void CBulletTraces::AddTrace(CVector *start, CVector *target).
//
// The streak, and the only thing in this file that measures what the player
// actually *saw* rather than what the engine computed on the way there.
//
// Read out of the retail function rather than re3's declaration: 0x00518E90
// is `xor eax,eax / push ebx`, then `mov edx,[esp+8]` and `mov ecx,[esp+0Ch]`
// for the two arguments and a bare `ret`, so it is __cdecl with start first.
// It copies edx's three floats to 0x0072B1B8 + slot*0x1C and ecx's three to
// +0x0C, sets the in-use byte at +0x18 and gives the slot a life of
// `25 + GetRandomNumber() % 32` frames at +0x1A.
//
// Why sample here rather than at the ray: because the two are different lines
// and it is this one that is drawn. On FireInstantHit's 3rd-person mouse
// camera branch - the branch a mouse-aiming player is on for every shot - the
// ray runs from the muzzle projected onto the camera axis, while the trail
// runs from the muzzle. An observer handed the *ray's* direction and told to
// fire from the muzzle draws a line parallel to the shooter's ray, offset by
// however far the muzzle sits off that axis. Handed the *trail's* direction it
// draws the shooter's own segment, because both ends now come from the same
// two points.
//
// One trigger pull can produce several: a shotgun draws one per pellet. The
// unit directions are summed for the same reason the rays are - the average of
// the unit vectors is the middle of the cone, where the average of the raw
// vectors would be dragged by whichever pellet flew furthest.
//
// Read-only. The original always runs, unchanged, for everybody.
using AddTraceFn = void(__cdecl *)(const float *, const float *);

void __cdecl HookedAddTrace(const float *start, const float *target) {
	if (g_localRay.sampling && start && target) {
		Vec3 unit;
		if (UnitDirection(Vec3{target[0] - start[0], target[1] - start[1],
		                       target[2] - start[2]},
		                  unit)) {
			g_localRay.trailSum.x += unit.x;
			g_localRay.trailSum.y += unit.y;
			g_localRay.trailSum.z += unit.z;
			if (g_localRay.trails == 0) {
				g_localRay.trailStart = Vec3{start[0], start[1], start[2]};
				g_localRay.trailEnd   = Vec3{target[0], target[1], target[2]};
			}
			++g_localRay.trails;
		}
	} else if (g_replaying && g_replayAim.ped && start && target &&
	           !g_saidReplayTrail) {
		// The other end of the same measurement, once. The shooter's log says
		// where their streak went; this says where ours went for the same
		// weapon. Two machines, two lines, and until now the only instrument
		// for comparing them was somebody looking at two screens.
		g_saidReplayTrail = true;
		Log("combat: drew a remote player's first bullet trail from (%.2f %.2f %.2f) "
		    "to (%.2f %.2f %.2f), along the (%.2f %.2f %.2f) their own machine sent. "
		    "That start is the muzzle off the wire, not our copy of their ped",
		    start[0], start[1], start[2], target[0], target[1], target[2],
		    g_replayAim.dir.x, g_replayAim.dir.y, g_replayAim.dir.z);
	}

	g_addTrace.Original<AddTraceFn>()(start, target);
}

// __cdecl void CWeapon::DoDoomAiming(CEntity *shooter, CVector *source,
//                                    CVector *target).
//
// The write seam, and the whole fix on the observer's side.
//
// For a shot we are replaying, the original is not called at all. Everything
// it does is nudge target->z toward a ped *this* machine found near the
// shooter, which is this machine deciding where somebody else's bullet goes -
// the same rule that stops an observer deciding damage, applied to the shot
// that would cause it. The owner's own engine already ran its own auto-aim,
// or its camera, or its lock-on, and the answer to all three is on the wire.
//
// Every other caller - every ped in the city, and the local player - goes
// straight through.
using DoomAimingFn = void(__cdecl *)(void *, float *, float *);

void __cdecl HookedDoDoomAiming(void *shooter, float *source, float *target) {
	if (g_replayAim.ped && shooter == g_replayAim.ped && source && target) {
		Vec3 turned;
		if (RotateOnto(g_replayAim.nominal, g_replayAim.dir,
		               Vec3{target[0] - source[0], target[1] - source[1],
		                    target[2] - source[2]},
		               turned)) {
			target[0] = source[0] + turned.x;
			target[1] = source[1] + turned.y;
			target[2] = source[2] + turned.z;
			++g_replayAim.applied;

			if (!g_saidAimApplied) {
				g_saidAimApplied = true;
				const float dot = vec::Dot(g_replayAim.nominal, g_replayAim.dir);
				const float clamped = dot > 1.0f ? 1.0f : (dot < -1.0f ? -1.0f : dot);
				Log("combat: pointed our first replayed shot along its owner's own line "
				    "instead of along our copy of their ped. The two were %.1f degrees "
				    "apart - their line is (%.2f %.2f %.2f), their ped here is facing "
				    "(%.2f %.2f %.2f). Anything more than about a degree is a trail in "
				    "the wrong street",
				    std::acos(clamped) * 57.2957795f, g_replayAim.dir.x,
				    g_replayAim.dir.y, g_replayAim.dir.z, g_replayAim.nominal.x,
				    g_replayAim.nominal.y, g_replayAim.nominal.z);
			}
			return;
		}
	}

	g_doomAiming.Original<DoomAimingFn>()(shooter, source, target);
}

// ---- sampling the local player --------------------------------------------

// The muzzle, as the engine computed it.
//
// CWeapon::Fire takes a fireSource, and CPed::FireGun always passes one,
// built from the weapon's own fire offset through the ped's hand bone - the
// right point for a muzzle flash, and getting it wrong is obvious on
// screen. The null case covers the handful of callers that let Fire derive
// its own; there the best available answer is the shooter's position raised
// by the same 0.6 the engine uses.
void ReadFireSource(const float *fireSource, const void *shooter, Vec3 &out) {
	if (fireSource) {
		out = Vec3{fireSource[0], fireSource[1], fireSource[2]};
		return;
	}
	const float *pos =
	    &Field<float>(const_cast<void *>(shooter), offs::POSITION);
	out = Vec3{pos[0], pos[1], pos[2] + 0.6f};
}

void ReadForward(const void *entity, Vec3 &out) {
	const float *fwd = &Field<float>(const_cast<void *>(entity), offs::MATRIX_FWD);
	out = Vec3{fwd[0], fwd[1], fwd[2]};
}

// Turns a velocity into the unit direction plus magnitude the wire carries.
// Zero-length is left as a zero direction rather than normalised - dividing
// by it is exactly how a NaN gets onto the network.
void SplitVelocity(const float *velocity, Vec3 &dir, float &speed) {
	const float len2 = velocity[0] * velocity[0] + velocity[1] * velocity[1] +
	                   velocity[2] * velocity[2];
	if (!(len2 > 1.0e-12f)) {
		dir   = Vec3{0.0f, 0.0f, 0.0f};
		speed = 0.0f;
		return;
	}
	const float len = std::sqrt(len2);
	dir   = Vec3{velocity[0] / len, velocity[1] / len, velocity[2] / len};
	speed = len;
}

// A shot the local player just fired. `before` is the projectile in-use mask
// from immediately before the call, so a newly created projectile can be found
// and its real initial velocity read off the object rather than re-derived.
void RecordLocalShot(const void *weapon, const void *shooter,
                     const float *fireSource, uint32_t before) {
	CombatEvent ev;
	ev.kind        = CombatEvent::SHOT;
	ev.shot.weapon = static_cast<uint8_t>(Field<uint32_t>(
	    const_cast<void *>(weapon), offs::WEAPON_TYPE));
	ev.shot.speed  = 0.0f;
	ReadFireSource(fireSource, shooter, ev.shot.origin);

	// The fallback, and until this round it was the only thing `dir` ever
	// carried for a bullet: the shooter's body forward. It is what the
	// observer's engine would have derived for itself anyway, so falling back
	// to it costs exactly the behaviour this change is fixing and never less.
	ReadForward(shooter, ev.shot.dir);

	// The line the engine actually *drew*, averaged over however many streaks
	// this discharge was, falling back to the line it traced and then to the
	// body. combat.h's ChooseShotDirection is the decision and the comment
	// above it is why it is in that order.
	//
	// Measured from the muzzle, because the muzzle is what goes on the wire as
	// the origin, and an origin and a direction taken off two different lines
	// reconstruct neither of them.
	if (IsInstantHitWeapon(ev.shot.weapon)) {
		Vec3                aim;
		const ShotAimSource from =
		    ChooseShotDirection(g_localRay.trailSum, g_localRay.trails, g_localRay.sum,
		                        g_localRay.rays, ev.shot.dir, aim);
		if (from != AIM_FROM_NOTHING)
			ev.shot.dir = aim;

		// The measurement this round exists for, once per session: how far the
		// ray the engine tested is from the streak it drew. Everything above
		// is an argument that they differ; this is the number.
		if (from == AIM_FROM_TRAIL && g_localRay.rays > 0 && !g_saidTrailMeasured) {
			g_saidTrailMeasured = true;
			Vec3 rayDir;
			if (UnitDirection(g_localRay.sum, rayDir)) {
				const float dot     = vec::Dot(rayDir, aim);
				const float clamped = dot > 1.0f ? 1.0f : (dot < -1.0f ? -1.0f : dot);
				const Vec3  off{g_localRay.nearEnd.x - ev.shot.origin.x,
				                g_localRay.nearEnd.y - ev.shot.origin.y,
				                g_localRay.nearEnd.z - ev.shot.origin.z};
				Log("combat: our own shot traced a ray from (%.2f %.2f %.2f) and drew "
				    "its trail from (%.2f %.2f %.2f) - %.2f m apart, %.1f degrees "
				    "between the two directions. The wire carries the trail's, because "
				    "the origin on it is the muzzle. Anything above a few centimetres "
				    "here is CWeapon::FireInstantHit's mouse-camera branch tracing from "
				    "the camera axis instead of the barrel",
				    g_localRay.nearEnd.x, g_localRay.nearEnd.y, g_localRay.nearEnd.z,
				    g_localRay.trailStart.x, g_localRay.trailStart.y,
				    g_localRay.trailStart.z, std::sqrt(vec::LengthSq(off)),
				    std::acos(clamped) * 57.2957795f);
			}
		}

		// And the assumption underneath all of it, stated once rather than
		// assumed forever: the streak starts at the muzzle we are sending.
		// Three of the four branches copy CWeapon::Fire's fireSource into the
		// local the trail is drawn from, so this should be zero; if it ever is
		// not, the observer's flash and the observer's trail come apart and
		// the log says so instead of the user having to.
		if (from == AIM_FROM_TRAIL && !g_saidTrailOrigin) {
			const Vec3 gap{g_localRay.trailStart.x - ev.shot.origin.x,
			               g_localRay.trailStart.y - ev.shot.origin.y,
			               g_localRay.trailStart.z - ev.shot.origin.z};
			if (vec::LengthSq(gap) > 0.01f) {
				g_saidTrailOrigin = true;
				Log("combat: our own trail does not start where we say the shot does - "
				    "the streak begins at (%.2f %.2f %.2f) and the wire carries "
				    "(%.2f %.2f %.2f), %.2f m away. Everyone watching will draw the "
				    "muzzle flash and the trail from that one point anyway",
				    g_localRay.trailStart.x, g_localRay.trailStart.y,
				    g_localRay.trailStart.z, ev.shot.origin.x, ev.shot.origin.y,
				    ev.shot.origin.z, std::sqrt(vec::LengthSq(gap)));
			}
		}

		if (from == AIM_FROM_BODY && !g_saidNoLocalRay) {
			g_saidNoLocalRay = true;
			Log("combat: our own shot with weapon %u drew no trail and traced no line "
			    "we could read, so the direction on the wire is our ped's body heading "
			    "rather than our aim. Everyone watching will draw the trail flat",
			    ev.shot.weapon);
		}
	}

	if (IsProjectileWeapon(ev.shot.weapon)) {
		const uint32_t created = InUseMask() & ~before;
		for (int i = 0; i < NUM_PROJECTILES; ++i) {
			if (!(created & (1u << i)))
				continue;
			if (Field<void *>(ProjInfo(i), PROJINFO_SOURCE) != shooter)
				continue;
			void *const obj = ProjObject(i);
			if (!obj)
				continue;
			// The object's own state, not a re-derivation of it. The velocity
			// AddProjectile computed depends on the thrower's heading, whether
			// they're the player, whether they have a seek target, and how
			// long the attack button was held - an observer has none of that
			// information.
			const float *pos = &Field<float>(obj, offs::POSITION);
			ev.shot.origin   = Vec3{pos[0], pos[1], pos[2]};
			SplitVelocity(&Field<float>(obj, offs::MOVE_SPEED), ev.shot.dir,
			              ev.shot.speed);
			break;
		}
	}

	Push(ev);
}

void RecordLocalExplosion(uint8_t type, const float *pos) {
	CombatEvent ev;
	ev.kind           = CombatEvent::EXPLOSION;
	ev.explosion.type = type;
	ev.explosion.pos  = Vec3{pos[0], pos[1], pos[2]};
	Push(ev);
}

// A hit the local engine just resolved on somebody else's player, recorded
// *instead* of being applied. The arguments are CPed::InflictDamage's own,
// untouched: no multiplier, no armour, no clamp. All of that belongs to the
// victim's machine, which is the only one that knows their armour and their
// state.
void RecordLocalDamage(uint16_t victimNetId, uint32_t method, float damage,
                       uint32_t piece, uint32_t direction, const MeleeTag &melee = {}) {
	CombatEvent ev;
	ev.kind               = CombatEvent::DAMAGE;
	ev.damage.victimNetId = victimNetId;
	ev.damage.weapon      = static_cast<uint8_t>(method);
	ev.damage.amount      = damage;
	ev.damage.piece       = static_cast<uint8_t>(piece);
	ev.damage.direction   = static_cast<uint8_t>(direction);
	ev.damage.melee       = melee.melee;
	ev.damage.hitLevel    = melee.hitLevel;
	Push(ev);
}

// The same thing, about a pedestrian somebody else hosts instead of a player.
//
// Separate from RecordLocalDamage rather than a flag on it, because the two
// netIds are not in the same namespace: one names a player slot the server
// keeps and the other names a row in its ambient table. A single queue entry
// with a "which kind" byte would have one packet's worth of routing decided by
// a byte nobody reading the wire could check.
void RecordLocalPedDamage(uint16_t pedNetId, uint32_t method, float damage,
                          uint32_t piece, uint32_t direction, const MeleeTag &melee = {}) {
	CombatEvent ev;
	ev.kind                = CombatEvent::PED_DAMAGE;
	ev.pedDamage.netId     = pedNetId;
	ev.pedDamage.weapon    = static_cast<uint8_t>(method);
	ev.pedDamage.amount    = damage;
	ev.pedDamage.piece     = static_cast<uint8_t>(piece);
	ev.pedDamage.direction = static_cast<uint8_t>(direction);
	ev.pedDamage.melee     = melee.melee;
	ev.pedDamage.hitLevel  = melee.hitLevel;
	Push(ev);
}

void RecordLocalDeath(uint32_t animId) {
	CombatEvent ev;
	ev.kind = CombatEvent::DEATH;
	ev.deathAnimId = animId < ANIM_NONE ? static_cast<uint16_t>(animId) : ANIM_NONE;
	Push(ev);
}

// ---- the detours ----------------------------------------------------------

// __thiscall bool CWeapon::Fire(CEntity *shooter, CVector *fireSource).
//
// Written as __fastcall because that's how MSVC lets a free function receive
// `this` in ecx: __fastcall takes its first argument in ecx, second in edx,
// so a dummy second parameter lines the rest up on the stack exactly the way
// __thiscall left them - and both conventions have the callee clean the
// stack arguments. Get this wrong and the stack unbalances on every single
// shot in the game, including the local player's own.
using FireThisFn = bool(__thiscall *)(void *, void *, float *);
using FireHookFn = bool(__fastcall *)(void *, void *, void *, float *);

bool __fastcall HookedFire(void *self, void * /*edx*/, void *shooter,
                           float *fireSource) {
	// The mask has to be taken before the original runs, and only when it
	// could actually matter - this hook sits on the hot path for every ped
	// in the city carrying a gun.
	const bool projectile =
	    self && IsProjectileWeapon(static_cast<uint8_t>(
	                Field<uint32_t>(self, offs::WEAPON_TYPE)));
	const uint32_t before = projectile ? InUseMask() : 0u;

	// This is the window the ray sampler runs in, and it has to be opened
	// before the original rather than after it, because the ray is traced and
	// forgotten inside the call. Only the local player's own trigger pull: a
	// replayed shot is already somebody else's line being drawn, and a city
	// NPC's is nobody's business.
	const bool ours = !g_replaying && shooter && shooter == PlayerPed();
	if (ours) {
		g_localRay          = LocalRay{};
		g_localRay.sampling = true;
	}

	g_localFiring = ours;
	const bool fired =
	    g_fire.Original<FireHookFn>()(self, nullptr, shooter, fireSource);
	g_localFiring = false;

	// Closed only by whoever opened it. Nothing nests today - the replay is
	// driven from the net drain, not from inside a local shot - and a window
	// that closes itself from a call it did not open is how that stops being
	// true quietly.
	if (ours)
		g_localRay.sampling = false;

	if (fired && ours)
		RecordLocalShot(self, shooter, fireSource, before);

	return fired;
}

// ---- our drive-by ------------------------------------------------------------

bool g_saidDriveBySent  = false;
bool g_saidDriveByNoLine = false;

// One round out of our car's window, as the line the engine drew for it.
// FireInstantHitFromCar traces once and draws once, hit or miss, and both go
// through the samplers above because the window is open (driveby.h).
void RecordLocalDriveBy() {
	DriveByLine line;
	if (!DriveByLineFrom(g_localRay.trails, g_localRay.trailStart, g_localRay.trailEnd,
	                     g_localRay.rays, g_localRay.nearEnd, g_localRay.farEnd, line)) {
		if (!g_saidDriveByNoLine) {
			g_saidDriveByNoLine = true;
			Log("combat: a drive-by round of ours drew no trail and traced no line we "
			    "could read, so nobody else sees it. The damage it did still goes out");
		}
		return;
	}

	CombatEvent ev;
	ev.kind        = CombatEvent::SHOT;
	ev.shot.weapon = WEAPONTYPE_UZI_DRIVEBY;
	ev.shot.origin = line.origin;
	ev.shot.dir    = line.dir;
	ev.shot.speed  = line.length;
	Push(ev);

	if (!g_saidDriveBySent) {
		g_saidDriveBySent = true;
		Log("combat: our first drive-by round went out as a shot with weapon %u, from "
		    "(%.1f %.1f %.1f) along (%.2f %.2f %.2f), a %.1f m trail (%s)",
		    WEAPONTYPE_UZI_DRIVEBY, line.origin.x, line.origin.y, line.origin.z,
		    line.dir.x, line.dir.y, line.dir.z, line.length,
		    g_localRay.trails > 0 ? "the one we drew" : "no trail, the ray's line");
	}
}

// __thiscall bool CWeapon::FireFromCar(CAutomobile *shooter, bool left), `ret 8`.
//
// Only DoDriveByShootings calls it (0x005641BF), and only for the car the
// local pad drives, so every call here is ours in practice. The driver test
// is there anyway, because a car that is status 0 for somebody else's ped is
// exactly the kind of half-state seat.cpp works to avoid.
using FireFromCarHookFn = bool(__fastcall *)(void *, void *, void *, uint32_t);

bool __fastcall HookedFireFromCar(void *self, void * /*edx*/, void *car, uint32_t left) {
	const bool ours = !g_replaying && car &&
	                  Field<void *>(car, offs::VEH_DRIVER) == PlayerPed();
	if (ours) {
		g_localRay          = LocalRay{};
		g_localRay.sampling = true;
	}

	const bool fired = g_fireFromCar.Original<FireFromCarHookFn>()(self, nullptr, car, left);

	if (ours) {
		g_localRay.sampling = false;
		if (fired)
			RecordLocalDriveBy();
	}
	return fired;
}

// ---- somebody else's drive-by ------------------------------------------------
//
// FireInstantHitFromCar's cosmetic half, call for call, from the round on the
// wire. The engine's function is never called here: it would aim with
// DoDriveByAutoAiming around our player, register the gunshot against our
// player, and take health off whatever it hit - three things this machine
// does not get to do. The hits were decided on the shooter's machine and
// arrive as C_Damage, C_PedDamage, C_VehicleHit or C_CarHit like any other.

using AddParticleFn = void *(__cdecl *)(int32_t, const float *, const float *, void *,
                                        float, int32_t, int32_t, int32_t, int32_t);
using AddLightFn    = void(__cdecl *)(uint32_t, float, float, float, float, float, float,
                                      float, float, float, float, uint32_t, uint32_t);
using WorldLineFn   = bool(__cdecl *)(const float *, const float *, void *, void **,
                                      uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                      uint32_t, uint32_t);
using PlayOneShotFn = void(__thiscall *)(void *, int32_t, uint32_t, float);
using ScriptSoundFn = void(__cdecl *)(uint32_t, const float *);

void PlayOn(void *entity, uint16_t sound, float volume) {
	const int32_t id = Field<int32_t>(entity, offs::PHYSICAL_AUDIO_ENTITY);
	if (id < 0)
		return;
	Func<PlayOneShotFn>(CAudioEngine__PlayOneShot)(
	    reinterpret_cast<void *>(DMAudio_Object), id, sound, volume);
}

// What the far end sounds like: table 0x00603214, less CPed::Say. Found with
// a line-of-sight query of our own, which fills a col point and nothing else,
// over the trail plus a hand's width so the surface it ended on is found.
void DriveByImpact(const float *source, const float *end, const Vec3 &dir, void *ownCar) {
	constexpr float PAST_END = 0.25f;
	const float probe[3] = {end[0] + dir.x * PAST_END, end[1] + dir.y * PAST_END,
	                        end[2] + dir.z * PAST_END};
	alignas(16) uint8_t colPoint[64] = {};
	void *victim = nullptr;
	Func<WorldLineFn>(CWorld__ProcessLineOfSight)(source, probe, colPoint, &victim, 1, 1, 1,
	                                              1, 1, 1, 0);
	if (!victim || victim == ownCar)
		return;   // the no-victim arm plays nothing, and our copy of their car is not a hit
	const float *const point = reinterpret_cast<const float *>(colPoint);
	switch (Field<uint8_t>(victim, offs::ENTITY_FLAGS) & 7) {
	case 1:
		Func<ScriptSoundFn>(PlayOneShotScriptObject)(SCRIPT_SOUND_BULLET_HIT_GROUND_1, point);
		break;
	case 2:
		PlayOn(victim, SOUND_WEAPON_HIT_VEHICLE, 1.0f);
		break;
	case 3:
		PlayOn(victim, SOUND_WEAPON_HIT_PED, 1.0f);
		break;
	case 4:
		Func<ScriptSoundFn>(PlayOneShotScriptObject)(SCRIPT_SOUND_BULLET_HIT_GROUND_2, point);
		break;
	case 5:
		Func<ScriptSoundFn>(PlayOneShotScriptObject)(SCRIPT_SOUND_BULLET_HIT_GROUND_3, point);
		break;
	default:
		break;
	}
}

bool g_saidDriveByDrawn  = false;
bool g_saidDriveByNoDir  = false;
bool g_saidDriveByNoSeat = false;

void DrawRemoteDriveBy(RemotePlayer &player, void *ped, const ShotBody &shot) {
	Vec3 dir;
	if (!UnitDirection(shot.dir, dir)) {
		if (!g_saidDriveByNoDir) {
			g_saidDriveByNoDir = true;
			Log("combat: a drive-by round from player net %u carried (%.2f %.2f %.2f), "
			    "which is not a direction, so it was not drawn",
			    player.netId, shot.dir.x, shot.dir.y, shot.dir.z);
		}
		return;
	}

	// Clamped like every wire position: both ends go into
	// CWorld::ProcessLineOfSight (pedanim.h, ClampToWorld).
	float source[3], end[3];
	source[0] = ClampToWorld(shot.origin.x);
	source[1] = ClampToWorld(shot.origin.y);
	if (!FiniteOr(shot.origin.z, 0.0f, source[2]))
		return;
	const float length = DriveByTrailLength(shot.speed);
	end[0] = ClampToWorld(source[0] + dir.x * length);
	end[1] = ClampToWorld(source[1] + dir.y * length);
	end[2] = source[2] + dir.z * length;

	void *const car = Field<bool>(ped, offs::PED_IN_VEHICLE)
	                      ? Field<void *>(ped, offs::PED_MY_VEHICLE)
	                      : nullptr;

	const float zero[3] = {0.0f, 0.0f, 0.0f};
	Func<AddParticleFn>(CParticle__AddParticle)(PARTICLE_GUNFLASH, source, zero, nullptr,
	                                            0.0f, 0, 0, 0, 0);
	Func<AddLightFn>(CPointLights__AddLight)(0, source[0], source[1], source[2], 0.0f, 0.0f,
	                                         0.0f, DRIVEBY_LIGHT_RADIUS, 1.0f, 0.8f, 0.0f,
	                                         0, 0);
	Func<AddTraceFn>(CBulletTraces__AddTrace)(source, end);
	DriveByImpact(source, end, dir, car);

	// The report comes from the car, as FireFromCar plays it (0x0055C996). A
	// ped that is not in one yet here plays it from the ped, whose own
	// one-shot picks the uzi's sample off the weapon in its hand.
	PlayOn(car ? car : ped, SOUND_WEAPON_SHOT_FIRED, 0.0f);

	if (car) {
		player.driveByShotAnim = DriveByAnimForShot(dir, ReadVec3(car, offs::MATRIX_RIGHT));
		player.driveByShotMs   = WallClock::NowMs();
	} else if (!g_saidDriveByNoSeat) {
		g_saidDriveByNoSeat = true;
		Log("combat: a drive-by round from player net %u arrived while their ped here "
		    "is not in a car yet; drew the round, and there is no window to lean out of",
		    player.netId);
	}

	if (!g_saidDriveByDrawn) {
		g_saidDriveByDrawn = true;
		Log("combat: drew our first remote drive-by round - player net %u, from "
		    "(%.1f %.1f %.1f) along (%.2f %.2f %.2f), %.1f m of trail. Its hits come "
		    "from their machine, not from this one",
		    player.netId, source[0], source[1], source[2], dir.x, dir.y, dir.z, length);
	}
}

// __cdecl bool CExplosion::AddExplosion(CEntity *explodingEntity,
//                                       CEntity *culprit, eExplosionType type,
//                                       const CVector &pos, uint32 lifetime).
using AddExplosionFn = bool(__cdecl *)(void *, void *, int, const float *, uint32_t);

bool __cdecl HookedAddExplosion(void *explodingEntity, void *culprit, int type,
                                const float *pos, uint32_t lifetime) {
	// While replaying somebody else's shot, this machine doesn't get to
	// decide where their projectile went off (see g_replaying). The refusal
	// is reported as "no explosion was added", the same answer the engine
	// gives when it is out of slots, and every caller already handles it.
	//
	// Only the projectile's own three types, though. This used to refuse
	// every explosion in the world for the length of the call, which is a
	// much bigger claim than §1.9.3 makes: a replayed bullet is a real
	// bullet in this world, and a car it sets off has to be allowed to go
	// up. Swallowing that left CAutomobile::BlowUpCar half done, with the
	// car wrecked, its occupants flagged bRemoveFromWorld and no blast, and
	// a half-torn-down car full of peds is not a state to leave the engine in.
	//
	// A barrel is refused as well, for the reason in combat.h
	// (ReplayMayAddExplosion): its culprit is FindPlayerPed() on every machine,
	// so the shooter already relays it.
	if (g_replaying && !ReplayMayAddExplosion(type)) {
		if (type == EXPLOSION_BARREL && !g_saidReplayBarrel) {
			g_saidReplayBarrel = true;
			Log("combat: a replayed shot hit an explosive barrel here and we did not "
			    "blow it up. The shooter's machine did, and its C_Explosion is the "
			    "one blast");
		}
		return false;
	}

	const bool added = g_explode.Original<AddExplosionFn>()(explodingEntity, culprit,
	                                                        type, pos, lifetime);

	// Only the local player's own explosions. A car blowing up because the
	// city AI decided so already happens on every machine - relaying it
	// would just double it up. And never from inside a replay, which is
	// somebody else's shot even when the engine names our ped.
	if (added && !g_replaying && pos && culprit && culprit == PlayerPed() && type >= 0 &&
	    IsKnownExplosionType(static_cast<uint8_t>(type)))
		RecordLocalExplosion(static_cast<uint8_t>(type), pos);

	return added;
}

// __cdecl void CProjectileInfo::RemoveProjectile(CProjectileInfo *info,
//                                                CProjectile *projectile).
using RemoveProjectileFn = void(__cdecl *)(void *, void *);

void __cdecl HookedRemoveProjectile(void *info, void *projectile) {
	const int slot = TrackedSlotOf(info);
	if (slot >= 0) {
		if (!g_saidProjectileEnded) {
			g_saidProjectileEnded = true;
			const uint32_t now =
			    Global<uint32_t>(CTimer__m_snTimeInMilliseconds);
			Log("combat: the first projectile we were animating for a remote player "
			    "ended after %u ms, %s. Either way it leaves no explosion of ours - "
			    "only its owner says where theirs went off",
			    now - g_tracked[slot].bornMs,
			    g_endingOurs
			        ? "because their explosion arrived and we ended it"
			        : "because this machine's engine removed it - it collided, or "
			          "CProjectileInfo::Update swept it into a ped, a car or a wall. "
			          "A few ms here means it was born somewhere it could not fly out "
			          "of");
		}

		// The whole trick, and why this function got disassembled instead of
		// just located: its explosion path is a three-way switch on
		// m_eWeaponType that falls through into the teardown. Blank the type
		// and the original still frees the slot, removes the object from the
		// world, and runs its deleting destructor - every last thing it
		// normally does - but never reaches CExplosion::AddExplosion. The
		// engine cleans up its own object; CoopIII just declines to decide
		// where it went off.
		Field<uint32_t>(info, PROJINFO_WEAPON_TYPE) = WEAPONTYPE_UNARMED;
		g_tracked[slot] = Tracked{};
	}
	g_removeProjectile.Original<RemoveProjectileFn>()(info, projectile);
}

// ---- our flame reaching what another machine owns --------------------------
//
// combat.h, IsOurFlame, is the rule and says why each half of it is needed.
// These two detours are where its two inputs come from.

Detour g_shotUpdate;
Detour g_startFire;

// True for exactly the length of CShotInfo::Update. Nothing nests: Update is
// called from one place (CWeapon::UpdateWeapons) and calls nothing that calls
// it again.
bool g_inShotUpdate = false;

FlameReportThrottle g_flameThrottle;

bool g_saidFlamePedSent    = false;
bool g_saidFlameCarSent    = false;
bool g_saidFlamePedLit     = false;
bool g_saidFlamePedRefused = false;
bool g_saidFlamePedNoLight = false;

uint32_t FlameClockMs() { return Global<uint32_t>(CTimer__m_snTimeInMilliseconds); }

// The pedestrian half. A replica is bFireProof, so CShotInfo::Update skips it
// at 0x0055C1D9 and never gets to StartFire - which is why this runs after
// Update rather than inside StartFire. Same list, same position, same radius
// and the same order of tests the engine just used, minus the proof flag.
//
// `ours` says which slots were alight and ours going in. A slot whose
// lifespan ran out this frame is cleared at 0x0055C085 and then processed
// anyway, so "in use afterwards" would miss its last frame.
void ReportOurFlameOnReplicaPeds(void *localPed, const bool (&ours)[NUM_SHOT_INFOS]) {
	uint16_t count = Field<uint16_t>(localPed, PED_NUM_NEAR_PEDS);
	if (count > PED_NEAR_PEDS_MAX)
		count = PED_NEAR_PEDS_MAX;

	using InControlFn = bool(__thiscall *)(void *);
	const uint32_t now = FlameClockMs();

	for (size_t s = 0; s < NUM_SHOT_INFOS; ++s) {
		if (!ours[s])
			continue;
		const uintptr_t slot   = gaShotInfo + s * SIZEOF_SHOTINFO;
		const float    *pos    = reinterpret_cast<const float *>(slot + SHOT_POS);
		const float     radius = *reinterpret_cast<const float *>(slot + SHOT_RADIUS);

		for (uint16_t i = 0; i < count; ++i) {
			void *const ped = Field<void *>(localPed, PED_NEAR_PEDS + i * 4u);

			// Matched on the pointer and then on the pool handle, without
			// reading the ped - so it also stands in for the engine's own
			// CPed::IsPointerValid, which a stale entry here would need.
			uint16_t netId = INVALID_NETID;
			if (!ped || !AmbientReplicaForPed(ped, netId))
				continue;
			if (!Func<InControlFn>(CPed__IsPedInControl)(ped))
				continue;

			const float *p  = &Field<float>(ped, offs::POSITION);
			const float  dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
			if (!FlameReachesPed(dx * dx + dy * dy + dz * dz, radius))
				continue;
			if (!g_flameThrottle.Due(FlameTargetKind::Pedestrian, netId, now))
				continue;

			RecordLocalPedDamage(netId, WEAPONTYPE_FLAMETHROWER, 0.0f, PEDPIECE_TORSO, 0);
			if (!g_saidFlamePedSent) {
				g_saidFlamePedSent = true;
				Log("combat: our flame reached pedestrian net %u, who belongs to "
				    "another machine. Sent as C_PedDamage with cause 9: their "
				    "machine lights him with its own CFireManager::StartFire and "
				    "its own fire does the burning", netId);
			}
		}
	}
}

// __cdecl void CShotInfo::Update(). No arguments, plain `ret`.
using ShotUpdateFn = void(__cdecl *)();

void __cdecl HookedShotInfoUpdate() {
	void *const localPed = PlayerPed();

	bool ours[NUM_SHOT_INFOS] = {};
	bool any                  = false;
	if (localPed) {
		for (size_t s = 0; s < NUM_SHOT_INFOS; ++s) {
			const uintptr_t slot = gaShotInfo + s * SIZEOF_SHOTINFO;
			if (*reinterpret_cast<const uint8_t *>(slot + SHOT_IN_USE) &&
			    *reinterpret_cast<void *const *>(slot + SHOT_SOURCE) == localPed) {
				ours[s] = true;
				any     = true;
			}
		}
	}

	g_inShotUpdate = true;
	g_shotUpdate.Original<ShotUpdateFn>()();
	g_inShotUpdate = false;

	if (any)
		ReportOurFlameOnReplicaPeds(localPed, ours);
}

// __thiscall CFire *CFireManager::StartFire(CEntity *entityOnFire,
//     CEntity *fleeFrom, float strength, bool propagation). ret 10h.
//
// The bool is taken as the dword the caller pushed (`push 1` at 0x0055C228
// and 0x004B3F8B), and handed back unchanged.
using StartFireHookFn = void *(__fastcall *)(void *, void *, void *, void *, float,
                                             uint32_t);

void *__fastcall HookedStartFire(void *self, void * /*edx*/, void *entity,
                                 void *fleeFrom, float strength, uint32_t propagation) {
	// The car half. SetCarsOnFire has no bFireProof to stop it on a replica car
	// - CoopIII sets only bCollisionProof on one - so the engine gets this far
	// and this is where it is caught.
	//
	// The engine still lights our copy afterwards, on purpose. Nothing tells
	// this machine that a car another machine owns is burning, so that copy is
	// the only flame our player sees on the car he is torching. It costs
	// nothing: its CFire hits CVehicle::InflictDamage every frame with cause 9,
	// the vehicle detour refuses that on a car we don't own, and cause 9 is
	// never forwarded as damage - only this ignition is.
	if (g_inShotUpdate && entity && fleeFrom) {
		void *const localPed = PlayerPed();
		if (IsOurFlame(true, localPed != nullptr && fleeFrom == localPed) &&
		    (Field<uint8_t>(entity, offs::ENTITY_FLAGS) & 7) == ENTITY_TYPE_VEHICLE &&
		    ReportOurFlameOnCar(entity, g_flameThrottle, FlameClockMs()) &&
		    !g_saidFlameCarSent) {
			g_saidFlameCarSent = true;
			Log("combat: our flame reached a car another machine owns, and the "
			    "ignition went to that machine. Our copy of the car burns here "
			    "too, for the look of it - it can't take health off anything");
		}
	}

	return g_startFire.Original<StartFireHookFn>()(self, nullptr, entity, fleeFrom,
	                                               strength, propagation);
}

// The owner's half: another player's flame reached one of our pedestrians on
// their screen, so light him here, with the engine's own StartFire and the
// same 0.8f CShotInfo::Update pushes (0x00603028).
//
// StartFire alone rather than the whole of CShotInfo's ped arm. Before its
// StartFire that arm calls SetFindPathAndFlee(source, 10000) and
// SetMoveState(SPRINT) for a ped who is not the player (0x0055C1F8,
// 0x0055C21B), and StartFire's own ped arm makes both calls again (0x00479669,
// 0x004796B1) before SetMoveAnim and PED_ON_FIRE. The arm's only other writes
// are a zero into [ped+2BCh] and bit 5 of [ped+157h], which StartFire clears
// again at 0x004796A7.
//
// The culprit is our copy of the shooter, so the flee runs from him and the
// CFire's m_pSource names him - the same choice ApplyRemotePedDamage makes for
// a bullet. A burning ped who brushes against our own player sets him alight
// with that source, and combat.h's RemoteMayDamageLocalPlayer gates that on
// friendly fire, as it gates everything else the shooter lit.
void LightHostedPedForFlame(RemotePlayer *attacker, void *ped, uint16_t netId) {
	const bool fireProof = (Field<uint8_t>(ped, offs::ENTITY_FLAGS_C) &
	                        offs::ENTITY_FIRE_PROOF) != 0;
	if (!OwnerLightsFlame(fireProof, /*wrecked=*/false)) {
		if (!g_saidFlamePedNoLight) {
			g_saidFlamePedNoLight = true;
			Log("combat: a flame reached our pedestrian net %u on another screen, "
			    "and he is bFireProof here, which is the test CShotInfo::Update "
			    "would have made (0x0055C1D9). Not lit", netId);
		}
		return;
	}

	void *const culprit = attacker ? ResolveRemotePed(*attacker) : nullptr;

	using StartFireFn = void *(__thiscall *)(void *, void *, void *, float, uint32_t);
	void *const fire  = Func<StartFireFn>(CFireManager__StartFireEntity)(
        reinterpret_cast<void *>(gFireManager), ped, culprit, FIRE_PED_STRENGTH, 1u);

	if (fire) {
		if (!g_saidFlamePedLit) {
			g_saidFlamePedLit = true;
			Log("combat: lit our pedestrian net %u because %s's flame reached him "
			    "on their screen. Our fire burns him from here on, and his death, "
			    "if it comes, goes out as C_PedDeath like any other",
			    netId, attacker ? attacker->nick.c_str() : "somebody");
		}
	} else if (!g_saidFlamePedRefused) {
		// StartFire's own no: already burning (our copy of their flame
		// probably got him first), not in control, or no free fire slot.
		g_saidFlamePedRefused = true;
		Log("combat: a flame reached our pedestrian net %u on another screen and "
		    "StartFire declined to light him (state %u, %s). Usually that is our "
		    "own replay of the same flame having got there first",
		    netId, Field<uint32_t>(ped, offs::PED_STATE),
		    Field<void *>(ped, PED_FIRE) ? "already burning" : "not burning");
	}
}

// __thiscall bool CPed::InflictDamage(CEntity *damagedBy, eWeaponType method,
//                                     float damage, ePedPieceTypes pedPiece,
//                                     uint8 direction).
//
// Same __fastcall trick as HookedFire, and the same reason: it's how a free
// function receives `this` in ecx with the five stack arguments left exactly
// where __thiscall put them. `ret 14h` is those five slots.
//
// This detour is where the host-authoritative rule stops being a property of
// five flags on a ped and becomes a property of the code.
//
// The flags stay. bBulletProof and its four siblings are still set in
// ped.cpp, because a detour that failed to install has to fail closed, and
// because §1.9.2's explosion rule genuinely depends on bExplosionProof. But
// they were never enough on their own: InflictDamage checks a proof flag
// inside a switch on the damage cause, and two of that switch's arms check
// nothing at all. WEAPONTYPE_DROWNING is one of them, so a remote ped
// standing in water on an observer's machine drowned locally and stayed a
// corpse for everyone watching, whatever its owner's health said. The
// `default:` arm is the other.
//
// So the rule is stated here, once, positively: nothing on this machine may
// damage a player this machine does not own. Anything the local player did
// on purpose becomes a packet instead.
// The last cause that actually reached the local player, and when.
//
// Kept for one reader: HookedSetDie, which has the animation the engine picked
// and no idea why. CPed::SetGetUp's crush death makes no InflictDamage call in
// the frame it kills - it writes m_fHealth = 0 and calls SetDie itself - so the
// cause has to come from the frame before it. Hence a remembered value with an
// age on it rather than an argument. combat.h, DeathCauseFor.
//
// Only what got through is recorded. A hit this machine refused on somebody
// else's behalf never touched our health and is not why we died.
struct LastLocalDamage {
	uint8_t  cause = 0;
	uint32_t atMs  = 0;
	bool     have  = false;
};
LastLocalDamage g_lastLocalDamage;

void NoteLocalDamage(uint32_t method) {
	g_lastLocalDamage.cause = static_cast<uint8_t>(method);
	g_lastLocalDamage.atMs  = WallClock::NowMs();
	g_lastLocalDamage.have  = true;
}

using InflictThisFn = bool(__thiscall *)(void *, void *, uint32_t, float, uint32_t,
                                         uint32_t);
using InflictHookFn = bool(__fastcall *)(void *, void *, void *, uint32_t, float,
                                         uint32_t, uint32_t);

bool __fastcall HookedInflictDamage(void *self, void * /*edx*/, void *damagedBy,
                                    uint32_t method, float damage, uint32_t piece,
                                    uint32_t direction) {
	void *const localPed = PlayerPed();

	// Nothing that happens inside a replay is allowed to hurt anyone. A
	// replayed shot is somebody else's bullet being drawn, not fired: the
	// hit it would find here is this machine's answer to a question only the
	// shooter's machine may answer, off a ped interpolated 100 ms late.
	//
	// ReplayRemoteShot also flips the local player bulletproof around the
	// call, and that isn't redundant - it's the half that still works when
	// this hook failed to install.
	if (g_replaying && self && self == localPed)
		return false;

	// Somebody else's ped is trying to hurt our player. It doesn't get to.
	//
	// Every hit a remote player legitimately lands on us arrives as S_Damage,
	// decided on their machine from their own trace. Anything that reaches
	// here instead is this machine working it out for itself, off a ped
	// interpolated 100 ms late, and the answer would be wrong.
	//
	// This is also what makes the flamethrower replayable. Its CShotInfo
	// outlives the call that made it and lights fires for a second or more
	// afterwards, and every one of those fires names the remote ped as its
	// source, so refusing by culprit covers the whole thing where a guard
	// around the call never could. combat.h, RemoteMayDamageLocalPlayer.
	// Except the one hit that is allowed: our own ApplyRemoteDamage, applying
	// what the attacker's machine already decided. Without this exemption the
	// rule refuses the packets the whole feature exists to deliver.
	uint16_t   attackerNetId = INVALID_NETID;
	const bool fromRemotePed = !g_applyingRemoteDamage && localPed &&
	                           self == localPed && damagedBy &&
	                           damagedBy != localPed &&
	                           RemotePlayerForPed(damagedBy, attackerNetId);
	const bool fire = IsFireDamage(static_cast<uint8_t>(method));

	if (fromRemotePed &&
	    !RemoteMayDamageLocalPlayer(static_cast<uint8_t>(method), g_friendlyFire)) {
		// Fire refused is its own line. It is the only refusal here that is
		// about the session's settings rather than about authority, and from
		// the outside it looks exactly like the bug this round fixed, so it
		// has to say which one it is.
		if (fire) {
			if (!g_saidFireRefused) {
				g_saidFireRefused = true;
				Log("combat: a fire lit by player net %u is burning us and we are "
				    "refusing it, because this session has friendly fire off. The "
				    "flame is real and so is the fire; only the damage is declined. "
				    "%u fires are alight on this machine",
				    attackerNetId, BurningFires());
			}
		} else if (!g_saidHitRefused) {
			g_saidHitRefused = true;
			Log("combat: refused a hit our own engine tried to land on us from "
			    "player net %u with cause %u. That is the rule: their machine "
			    "decides their hits and sends them, this one does not guess",
			    attackerNetId, method);
		}
		return false;
	}

	// Fire is getting through. Say so once, and say whether it did anything.
	//
	// The second half is the part worth having. "The detour allowed it" and
	// "the player's health went down" are two different claims and the gap
	// between them is where the next round of this would be lost: a player
	// MakePlayerSafe has made undamageable, a m_bCanBeDamaged of false, or a
	// hit so small it rounds to nothing all look identical from here unless
	// the health is read either side of the call. So it is.
	if (fire && localPed && self == localPed &&
	    (!g_saidFireBurned || !g_saidFireNoMove)) {
		NoteLocalDamage(method);
		const float before = Field<float>(localPed, offs::PED_HEALTH);
		const bool  died   = g_inflictDamage.Original<InflictHookFn>()(
            self, nullptr, damagedBy, method, damage, piece, direction);
		const float after  = Field<float>(localPed, offs::PED_HEALTH);

		if (after < before) {
			if (!g_saidFireBurned) {
				g_saidFireBurned = true;

				// Who lit it, from the fire's own m_pSource, which is what
				// arrives here as damagedBy. Three answers and they mean
				// three different things: a null source is terrain and
				// ignores friendly fire, our own ped is our own flame, and a
				// remote ped is the case this whole change is about.
				char who[48];
				if (!damagedBy)
					std::snprintf(who, sizeof who, "nobody - this fire is terrain");
				else if (damagedBy == localPed)
					std::snprintf(who, sizeof who, "us, with our own flame");
				else if (attackerNetId != INVALID_NETID)
					std::snprintf(who, sizeof who, "player net %u", attackerNetId);
				else
					std::snprintf(who, sizeof who, "something local, not a player");

				Log("combat: fire is burning us for real - %.2f health this frame, "
				    "%.0f left, lit by %s, %u fires alight on this machine",
				    before - after, after, who, BurningFires());
			}
		} else if (!g_saidFireNoMove) {
			g_saidFireNoMove = true;
			Log("combat: fire reached CPed::InflictDamage and took no health off "
			    "(%.0f before, %.0f after, %.2f asked for). The rule is not what is "
			    "stopping it - look at m_bCanBeDamaged, MakePlayerSafe, or armour",
			    before, after, damage);
		}
		return died;
	}

	// Ours, for the two forwarding branches below. A drive-by round names our
	// car rather than our ped (driveby.h, HitIsOurs), so the car is only
	// looked up for that one cause.
	const bool culpritIsOurCar =
	    damagedBy && method == WEAPONTYPE_UZI_DRIVEBY &&
	    damagedBy == Func<void *(__cdecl *)()>(FindPlayerVehicle)();
	const bool ours = !g_replaying && damagedBy &&
	                  HitIsOurs(damagedBy == localPed, culpritIsOurCar,
	                            static_cast<uint8_t>(method));

	uint16_t victimNetId = INVALID_NETID;
	if (self && RemotePlayerForPed(self, victimNetId)) {
		// Somebody else's player. Their health is theirs.
		//
		// Only what the local player did deliberately goes on the wire. A
		// city NPC shooting a remote player is a shot that happened in one
		// simulation and not in the others (docs/protocol.md §3), and
		// forwarding it would kill someone with a cop they can't see.
		if (ours && IsForwardableDamage(static_cast<uint8_t>(method))) {
			// A strike or a swing carries its path, and a swing its amount as
			// if the engine had known this was a player (melee.h).
			const MeleeTag melee = LocalMeleeTag(damagedBy, method);
			if (MeleeKind(melee) == MELEE_SWING)
				damage = SwingAmountForPlayer(damage, method == WEAPONTYPE_BASEBALLBAT,
				                              (melee.melee & MELEE_HEAVY) != 0,
				                              g_melee.adrenaline);
			RecordLocalDamage(victimNetId, method, damage, piece, direction, melee);
			if (!g_saidHitSent) {
				g_saidHitSent = true;
				Log("combat: our first hit on a remote player, net %u for %.0f with "
				    "cause %u, is on its way as C_Damage", victimNetId, damage, method);
			}
		} else if (ours && !g_saidHitNotForwarded) {
			// Our own hit, on a real player, and not forwarded. Always a
			// decision rather than a failure, but a decision worth seeing.
			g_saidHitNotForwarded = true;
			Log("combat: our hit on player net %u is not forwarded, because cause "
			    "%u is not one the shooter gets to decide (combat.h, "
			    "IsForwardableDamage)", victimNetId, method);
		}

		// False is "the ped did not die", which is what every caller of this
		// function already handles for a hit that wasn't fatal.
		return false;
	}

	// Somebody else's *pedestrian*, and until this branch existed there was
	// nothing here at all. That absence is the whole of the bug: a replica is
	// bullet-, fire-, melee- and explosion-proof on purpose
	// (population.cpp, SpawnAmbientReplica), so the switch inside this function
	// threw every shot away, the fall-through below happily called the original
	// which then did nothing, and no packet went anywhere. Shoot a pedestrian
	// somebody else hosts and there is no reaction, no blood and no death - on
	// either screen.
	//
	// So it is stated here the same way the player rule above is, positively
	// and in both halves. Nothing on this machine may damage a pedestrian this
	// machine does not own, and anything the local player did on purpose
	// becomes a packet instead.
	//
	// The refusal is not redundant with the proof flags, for exactly the reason
	// the player rule is not: the switch this function makes on the damage
	// cause has two arms that check no flag at all. WEAPONTYPE_DROWNING is one,
	// and a replica standing in water on this machine drowned locally and then
	// stayed a corpse forever, because ApplyAmbientPedState refuses to drive
	// anything into a dead replica and nothing off the wire ever resets
	// m_nPedState. The `default:` arm is the other.
	//
	// **A blast is not routed through here and must not be.** An explosion is
	// replayed on every machine at a position everybody agreed on, so the
	// machine that hosts the pedestrian puts its own copy in its own blast and
	// its own engine kills it - and that death already travels on C_PedDeath.
	// Forwarding it as damage as well would apply it twice, which is the same
	// argument IsForwardableDamage makes for a player (§1.10.1).
	uint16_t pedNetId = INVALID_NETID;
	if (self && AmbientReplicaForPed(self, pedNetId)) {
		// Already a corpse here. The report would be refused by the server
		// (Session::NotePedDeath has marked the row dead) and refused again by
		// the owner's own engine at 0x004EA485, so the only thing sending it
		// would achieve is a reliable packet per round of a burst fired into a
		// body. Dropped before it costs anything.
		const uint32_t state  = Field<uint32_t>(self, offs::PED_STATE);
		const bool     corpse = state == PEDSTATE_DIE || state == PEDSTATE_DEAD;

		// Only what the local player did deliberately, the same test and the
		// same reason as the player branch: a city NPC shooting a replica is a
		// shot that happened in one simulation and not in the others, and
		// forwarding it would have this machine's traffic killing another
		// machine's pedestrians.
		if (ours && !corpse &&
		    IsForwardableDamage(static_cast<uint8_t>(method))) {
			RecordLocalPedDamage(pedNetId, method, damage, piece, direction,
			                     LocalMeleeTag(damagedBy, method));
			if (!g_saidPedHitSent) {
				g_saidPedHitSent = true;
				Log("combat: our first hit on a hosted pedestrian, net %u for %.0f "
				    "with cause %u piece %u, is on its way as C_PedDamage. Their "
				    "machine applies it through its own CPed::InflictDamage and "
				    "tells the session what came of it", pedNetId, damage, method,
				    piece);
			}
		} else if (ours && !corpse && !g_saidPedHitNotForwarded) {
			g_saidPedHitNotForwarded = true;
			Log("combat: our hit on pedestrian net %u is not forwarded, because "
			    "cause %u is not one the shooter gets to decide (combat.h, "
			    "IsForwardableDamage). A blast gets there on its own, because "
			    "every machine replays it at the same place", pedNetId, method);
		}

		return false;
	}

	// One of our own pedestrians, hit by somebody else's replayed round.
	//
	// If the session has named it, the shooter has a replica of it, and
	// whatever their round did to that replica is already on its way here as
	// C_PedDamage. Letting this one through as well took the health twice. The
	// flinch, the fall and the blood were drawn before this call and stay.
	if (g_replaying && self && self != localPed) {
		bool named = false;
		const bool hosted = HostedPedFor(self, named);
		if (!ReplayedShotMayDamage(ClassifyReplayPed(hosted, named),
		                           static_cast<uint8_t>(method))) {
			if (!g_saidReplayPedRefused) {
				g_saidReplayPedRefused = true;
				Log("combat: a replayed shot reached one of our own pedestrians (cause "
				    "%u, %.0f) and we did not take the health. The shooter's "
				    "C_PedDamage for it is the hit that counts",
				    method, damage);
			}
			return false;
		}
	}

	// Anything still here is a hit this machine is letting through, and if it
	// is ours it is a candidate reason for the next death. Recorded before the
	// call because the call can be the death.
	if (localPed && self == localPed)
		NoteLocalDamage(method);

	return g_inflictDamage.Original<InflictHookFn>()(self, nullptr, damagedBy, method,
	                                                 damage, piece, direction);
}

// __thiscall void CPed::SetDie(AnimationId anim, float delta, float speed).
//
// Two things ride this one hook, because CPed::SetDie is one address and an
// address carries one detour.
//
// **The local player.** Which animation did the engine pick for this death?
// Nothing else can say. The snapshot can't - a death animation is created
// with a blendAmount of 0 and doesn't become the dominant one until several
// frames later, by which point the player has been lying on the floor with
// nobody else told about it.
//
// The announcement itself doesn't depend on this hook. Client::UpdateLocalLife
// watches the sampled health and announces a death with ANIM_NONE if this
// never fired, so a failed install costs the right animation and nothing
// else.
//
// **Every other ped.** This is also the only moment an *ambient* pedestrian
// this machine hosts dies, and unlike the player's death there is no second
// witness at all: an ambient ped carries no health on the wire
// (protocol.h, AmbientPedState, and the omission is deliberate), so nothing
// downstream could infer it. game/population.cpp decides which of those peds
// this machine is entitled to speak about - the same hosted-and-named filter
// its BodyPartHook applies - and queues the ones that pass.
//
// A failed install therefore costs the whole ambient death feature, not just
// its animation. That is stated at the call rather than worked out later.
using SetDieHookFn = void(__fastcall *)(void *, void *, uint32_t, float, float);
using SetDieThisFn = void(__thiscall *)(void *, uint32_t, float, float);

void __fastcall HookedSetDie(void *self, void * /*edx*/, uint32_t animId, float delta,
                             float speed) {
	const bool localPlayer = self && self == PlayerPed();

	// SetDie returns without doing anything for a ped that's already dying,
	// and for a player MakePlayerSafe has made undamageable. Both would look
	// like a death from the outside, so the test is the transition rather
	// than the state: alive before, PED_DIE after.
	//
	// Read for every ped now, not just the player. It is one dword off an
	// object the engine is about to rewrite anyway, and the alternative -
	// trusting the call - announces a death every time something shoots a
	// corpse.
	const uint32_t before = self ? Field<uint32_t>(self, offs::PED_STATE) : PEDSTATE_NONE;
	const bool wasDying   = before == PEDSTATE_DIE || before == PEDSTATE_DEAD;

	// **Somebody else's pedestrian, and this machine is not entitled to kill
	// him.** The third bullet of docs/population.md §5.6, and the only one of
	// the three that is a rule being broken rather than a fact going
	// unreported.
	//
	// It is a refusal and not a packet, and the reasoning is all in
	// population.cpp's RefuseLocalReplicaDeath - short version: the host's
	// copy of this pedestrian is alive, so the observer has discovered
	// nothing, and an observer that announced its own divergence would be
	// asking the session to pick a winner between two machines about a
	// pedestrian only one of them owns.
	//
	// Before the original and before the local player's own arm, because the
	// original is the thing being refused. The local player is never a
	// replica, so that test is cheap insurance rather than logic.
	//
	// Not for a ped that is already down, and that is not a micro-optimisation:
	// SetDie returns without doing anything for one, so there is nothing to
	// refuse - but the refusal puts the health back on its way out, and doing
	// that to a replica the host correctly reported dead would hand a corpse
	// a hundred health every time something shot it. The transition test the
	// rest of this function already makes is the right test here too.
	uint16_t replicaNetId = INVALID_NETID;
	if (!localPlayer && !wasDying && RefuseLocalReplicaDeath(self, replicaNetId))
		return;

	g_setDie.Original<SetDieHookFn>()(self, nullptr, animId, delta, speed);

	if (!self || wasDying || Field<uint32_t>(self, offs::PED_STATE) != PEDSTATE_DIE)
		return;

	if (localPlayer) {
		// Said here and not in Client::AnnounceDeath, because this is the only
		// place that still has the three facts at the same time: the animation
		// the engine picked, whether we were in a seat when it picked it, and
		// what last damaged us. By the time the queue is drained the ped is a
		// corpse and bInVehicle has been cleared.
		//
		// combat.h has the disassembly this reads from. The short version:
		// animation 173 comes from exactly two places in the image, and a ped
		// in a car can only die of drowning, so those two facts together name
		// the death.
		const bool       inVehicle = Field<bool>(self, offs::PED_IN_VEHICLE);
		const DeathCause cause =
		    DeathCauseFor(g_lastLocalDamage.have, g_lastLocalDamage.cause,
		                  WallClock::NowMs() - g_lastLocalDamage.atMs);
		Log("combat: we died - anim %u, %sin a vehicle, last cause %s (%u) %u ms "
		    "ago. %s",
		    animId, inVehicle ? "" : "not ",
		    g_lastLocalDamage.have ? "recorded" : "none",
		    g_lastLocalDamage.have ? g_lastLocalDamage.cause : 0u,
		    g_lastLocalDamage.have
		        ? WallClock::NowMs() - g_lastLocalDamage.atMs
		        : 0u,
		    DeathStory(static_cast<uint16_t>(animId), inVehicle, cause));
		g_lastLocalDamage = LastLocalDamage{};

		RecordLocalDeath(animId);
		return;
	}

	// Not the player, so it is a pedestrian - ours, somebody else's replica,
	// or one the session has never heard of. population.cpp is the only
	// thing that can tell those apart, and it drops the two it is not
	// entitled to speak about.
	//
	// animId is narrowed to 16 bits here and on the wire, which is the same
	// width PlayerStateBody and C_Death already use for an AnimationId. The
	// retail enum's last member is 0ADh.
	NoteHostedPedDeath(self, static_cast<uint16_t>(animId));
}

// One line at startup, and it exists to close a question rather than to change
// anything: **is the trail business about the screen resolution?**
//
// The trail itself cannot be - it is two world-space points from end to end
// (addresses.h). But the *aim* on the 3rd-person mouse camera branch is scaled
// horizontally by an aspect ratio, and in retail that aspect ratio is one of
// two constants picked by the widescreen menu toggle rather than the shape of
// the window. With the toggle not matching what is being rendered, every shot
// leaves the barrel about 0.8 degrees to one side of the crosshair - always the
// same side, on every screen, with or without CoopIII.
//
// And this install has ThirteenAG's Widescreen Fix in `_ESSENTIALS`, whose
// whole job is that class of problem. So rather than assume, read it: if the
// selection site still holds retail's bytes, nothing has touched it and the
// two constants below are what the game aims with. If it does not, somebody
// has, and the numbers are theirs.
void ReportAimAspect() {
	const uint8_t *const site =
	    reinterpret_cast<const uint8_t *>(CCamera__AimAspectSelect);
	bool retail = true;
	for (size_t i = 0; i < sizeof(kAimAspectSelectBytes); ++i)
		if (site[i] != kAimAspectSelectBytes[i])
			retail = false;

	if (!retail) {
		Log("combat: something has rewritten how the game turns the crosshair into a "
		    "shot direction (0x%08X is no longer retail's `cmp byte [0x%08X],0`). The "
		    "Widescreen Fix is the likely author and that is a good thing - but it "
		    "means the aiming aspect ratio here is not one this file can predict",
		    CCamera__AimAspectSelect, CMenuManager__PrefsUseWideScreen);
		return;
	}

	Log("combat: the game still aims the retail way - a mouse-aimed shot's horizontal "
	    "angle is scaled by %.4f or %.4f depending on the widescreen toggle (currently "
	    "%s), not by the shape of the window. The crosshair is drawn at a fixed "
	    "fraction of the screen, so with those two disagreeing every shot leaves the "
	    "barrel a fraction of a degree to one side of the sight, on every screen at "
	    "once. Not a sync bug, and not something the wire can fix",
	    Global<float>(kAimAspectWide), Global<float>(kAimAspectStandard),
	    Global<uint8_t>(CMenuManager__PrefsUseWideScreen) ? "on" : "off");
}

} // namespace

// ---- installation ---------------------------------------------------------

bool InstallCombatHooks() {
	for (Tracked &t : g_tracked)
		t = Tracked{};
	g_head = g_count = 0;
	g_dropped        = 0;
	g_localRay       = LocalRay{};
	g_replayAim      = ReplayAim{};
	g_lastLocalDamage = LastLocalDamage{};
	g_inShotUpdate    = false;
	g_flameThrottle   = FlameReportThrottle{};
	g_melee           = MeleeCall{};

	struct Spec {
		const char *name;
		uintptr_t   target;
		void       *replacement;
		Detour     *detour;
	};
	const Spec specs[] = {
	    {"CWeapon::Fire", CWeapon__Fire, reinterpret_cast<void *>(&HookedFire), &g_fire},
	    {"CExplosion::AddExplosion", CExplosion__AddExplosion,
	     reinterpret_cast<void *>(&HookedAddExplosion), &g_explode},
	    {"CProjectileInfo::RemoveProjectile", CProjectileInfo__RemoveProjectile,
	     reinterpret_cast<void *>(&HookedRemoveProjectile), &g_removeProjectile},
	    {"CPed::InflictDamage", CPed__InflictDamage,
	     reinterpret_cast<void *>(&HookedInflictDamage), &g_inflictDamage},
	    {"CPed::SetDie", CPed__SetDie, reinterpret_cast<void *>(&HookedSetDie), &g_setDie},
	    {"CWeapon::ProcessLineOfSight", CWeapon__ProcessLineOfSight,
	     reinterpret_cast<void *>(&HookedProcessLineOfSight), &g_lineOfSight},
	    {"CWeapon::DoBulletImpact", CWeapon__DoBulletImpact,
	     reinterpret_cast<void *>(&HookedDoBulletImpact), &g_bulletImpact},
	    {"CWeapon::DoDoomAiming", CWeapon__DoDoomAiming,
	     reinterpret_cast<void *>(&HookedDoDoomAiming), &g_doomAiming},
	    {"CBulletTraces::AddTrace", CBulletTraces__AddTrace,
	     reinterpret_cast<void *>(&HookedAddTrace), &g_addTrace},
	    {"CShotInfo::Update", CShotInfo__Update,
	     reinterpret_cast<void *>(&HookedShotInfoUpdate), &g_shotUpdate},
	    {"CFireManager::StartFire", CFireManager__StartFireEntity,
	     reinterpret_cast<void *>(&HookedStartFire), &g_startFire},
	    {"CPed::ReactToAttack", CPed__ReactToAttack,
	     reinterpret_cast<void *>(&HookedReactToAttack), &g_reactToAttack},
	    {"CPed::SetFall", CPed__SetFall, reinterpret_cast<void *>(&HookedSetFall),
	     &g_setFall},
	    {"CWeapon::FireFromCar", CWeapon__FireFromCar,
	     reinterpret_cast<void *>(&HookedFireFromCar), &g_fireFromCar},
	    {"CPed::FightStrike", CPed__FightStrike,
	     reinterpret_cast<void *>(&HookedFightStrike), &g_fightStrike},
	    {"CWeapon::FireMelee", CWeapon__FireMelee,
	     reinterpret_cast<void *>(&HookedFireMelee), &g_fireMelee},
	    {"CPed::StartFightDefend", CPed__StartFightDefend,
	     reinterpret_cast<void *>(&HookedStartFightDefend), &g_startFightDefend},
	};

	bool all = true;
	for (const Spec &s : specs) {
		if (s.detour->Install(s.name, reinterpret_cast<void *>(s.target),
		                      s.replacement)) {
			Log("combat: hooked %s at 0x%08X", s.name, s.target);
			continue;
		}
		all = false;
		Log("combat: FAILED to hook %s at 0x%08X", s.name, s.target);
	}

	if (!all)
		for (const auto &f : HookFailures())
			Log("combat:   %s: %s", f.name.c_str(), f.reason.c_str());

	ReportAimAspect();

	return all;
}

void RemoveCombatHooks() {
	// Order matters here for the same reason it does anywhere a ped gets
	// destroyed before its car: stop animating other people's projectiles
	// before the detour that keeps them from exploding goes away. Skip this
	// order and every in-flight projectile detonates wherever the unhooking
	// happened to catch it.
	for (int i = 0; i < NUM_PROJECTILES; ++i)
		if (g_tracked[i].active)
			EndTracked(i);

	g_fire.Remove();
	g_explode.Remove();
	g_removeProjectile.Remove();
	g_inflictDamage.Remove();
	g_setDie.Remove();
	g_lineOfSight.Remove();
	g_bulletImpact.Remove();
	g_doomAiming.Remove();
	g_addTrace.Remove();
	g_shotUpdate.Remove();
	g_startFire.Remove();
	g_reactToAttack.Remove();
	g_setFall.Remove();
	g_fireFromCar.Remove();
	g_fightStrike.Remove();
	g_fireMelee.Remove();
	g_startFightDefend.Remove();
	g_melee        = MeleeCall{};
	g_inShotUpdate = false;
	g_flameThrottle = FlameReportThrottle{};
	g_count = g_head = 0;
	g_friendlyFire   = false;
	g_localRay       = LocalRay{};
	g_replayAim      = ReplayAim{};
}

bool CombatHooksInstalled() {
	return g_fire.IsInstalled() && g_explode.IsInstalled() &&
	       g_removeProjectile.IsInstalled() && g_inflictDamage.IsInstalled() &&
	       g_setDie.IsInstalled() && g_lineOfSight.IsInstalled() &&
	       g_bulletImpact.IsInstalled() && g_doomAiming.IsInstalled() &&
	       g_addTrace.IsInstalled() && g_shotUpdate.IsInstalled() &&
	       g_startFire.IsInstalled() && g_reactToAttack.IsInstalled() &&
	       g_setFall.IsInstalled() && g_fireFromCar.IsInstalled() &&
	       g_fightStrike.IsInstalled() && g_fireMelee.IsInstalled() &&
	       g_startFightDefend.IsInstalled();
}

void SetFriendlyFire(bool enabled) { g_friendlyFire = enabled; }

uint8_t DrainLocalCombat(CombatEvent *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && g_count > 0) {
		out[n++] = g_pending[g_head];
		g_head   = static_cast<uint8_t>((g_head + 1) % MAX_PENDING);
		--g_count;
	}
	return n;
}

// ---- replaying somebody else's shot ---------------------------------------

bool ReplayingRemoteShot() { return g_replaying; }

bool LocalPlayerFiring() { return g_localFiring; }

bool IsRemotePlayersProjectile(const void *info) { return TrackedSlotOf(info) >= 0; }

void ReplayRemoteShot(RemotePlayer &player, const ShotBody &shot) {
	// A drive-by round is drawn, not replayed: driveby.h says why.
	if (shot.weapon == WEAPONTYPE_UZI_DRIVEBY) {
		void *const ped = ResolveRemotePed(player);
		if (!ped) {
			RefuseShot(GATE_NO_PED, "we have no ped for them right now", player, shot.weapon);
			return;
		}
		DrawRemoteDriveBy(player, ped, shot);
		return;
	}

	if (!IsReplayableWeapon(shot.weapon)) {
		RefuseShot(GATE_WEAPON,
		           "that weapon is not one an observer replays (combat.h, "
		           "IsReplayableWeapon)", player, shot.weapon);
		return;
	}

	void *const ped = ResolveRemotePed(player);
	if (!ped) {
		RefuseShot(GATE_NO_PED, "we have no ped for them right now", player, shot.weapon);
		return;
	}

	// A seated ped's gun is the car's business - a drive-by arrives as weapon
	// 19 and is drawn above, so anything else from a seat is a round fired on
	// foot that raced the seating here. A dying ped has already stopped:
	// CWeapon::Fire would still go through, and a muzzle flash out of a
	// corpse looks worse than nothing at all.
	if (Field<bool>(ped, offs::PED_IN_VEHICLE)) {
		RefuseShot(GATE_SEATED, "their ped is in a car here and the round was not a "
		           "drive-by", player, shot.weapon);
		return;
	}
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD) {
		RefuseShot(GATE_DEAD, "their ped is dying or dead on this machine", player,
		           shot.weapon);
		return;
	}

	// Not yet: the weapon's model is still streaming. Dropped rather than
	// retried, same as the shot itself - by the time it loads, this round
	// is already over.
	if (!GiveRemoteWeapon(player, ped, shot.weapon)) {
		RefuseShot(GATE_MODEL, "their weapon's model has not finished streaming here",
		           player, shot.weapon);
		return;
	}

	void *const weapon = reinterpret_cast<uint8_t *>(ped) + offs::PED_WEAPONS +
	                     static_cast<size_t>(shot.weapon) * offs::SIZEOF_WEAPON;

	// The shot goes off no matter what this machine's copy of their clip
	// says, and it goes off through the engine's own path.
	//
	// CWeapon::Fire refuses on an empty clip - `cmp dword [edi+8],0 / jg` at
	// 0x0055C4A2, which returns false before anything is drawn - and it also
	// refuses while the slot is reloading or out of ammo. Their machine has
	// already decided they fired. If this copy were allowed to disagree,
	// remote players would stop shooting on your screen while they were
	// still shooting on theirs, which is a worse bug than the one ammo sync
	// exists to fix: it is silent, and it only happens in a long firefight.
	//
	// So the slot is forced into a state Fire cannot refuse, and whatever
	// Fire spends out of it is thrown away afterwards - see the restore
	// below. Nothing here is the authority on the number; the wire is.
	constexpr int32_t REPLAY_CLIP = 500;
	const uint8_t     firedSlot   = shot.weapon;
	Field<uint32_t>(weapon, offs::WEAPON_STATE)       = WEAPONSTATE_READY;
	Field<int32_t>(weapon, offs::WEAPON_AMMO_IN_CLIP) = REPLAY_CLIP;
	Field<int32_t>(weapon, offs::WEAPON_AMMO_TOTAL)   = REPLAY_CLIP;
	Field<uint32_t>(weapon, offs::WEAPON_TIMER)       = 0;

	// Clamped for the same reason every other wire position is: the fire
	// source becomes one end of a CWorld::ProcessLineOfSight call, which
	// turns it into a subscript into ms_aSectors with no bounds check of its
	// own (pedanim.h). Fire also *writes through* this pointer on the rocket
	// path, so it needs to be a local of ours, not the packet's own buffer.
	float source[3];
	source[0] = ClampToWorld(shot.origin.x);
	source[1] = ClampToWorld(shot.origin.y);
	if (!FiniteOr(shot.origin.z, Field<float>(ped, offs::POSITION + 8), source[2]))
		source[2] = Field<float>(ped, offs::POSITION + 8);

	// Everything a replayed bullet could hit that has an owner gets made
	// untouchable, just for this call. Remote peds already are, permanently;
	// the local player isn't, and can't be left that way afterward.
	//
	// The bit gets restored, not the whole byte - the player might
	// legitimately be bulletproof from the cheat, and writing the byte back
	// wholesale would undo anything else the shot itself changed in it.
	void *const  localPed  = PlayerPed();
	const bool   wasProof  = localPed && (Field<uint8_t>(localPed, offs::ENTITY_FLAGS_C) &
	                                    offs::ENTITY_BULLET_PROOF) != 0;
	if (localPed)
		Field<uint8_t>(localPed, offs::ENTITY_FLAGS_C) |= offs::ENTITY_BULLET_PROOF;

	const bool projectile = IsProjectileWeapon(shot.weapon);
	const uint32_t before = projectile ? InUseMask() : 0u;

	// Point the shot where its owner pointed it (docs/protocol.md 1.9.7).
	//
	// `nominal` is read here rather than inside the detour because this is the
	// last moment it is a field: by the time FireInstantHit calls
	// DoDoomAiming, the heading it derived is three x87 registers deep.
	g_replayAim = ReplayAim{};
	if (IsInstantHitWeapon(shot.weapon)) {
		Vec3 wire, nominal;
		if (UnitDirection(shot.dir, wire) &&
		    FlatHeadingDirection(ReadVec3(ped, offs::MATRIX_FWD), nominal)) {
			g_replayAim.ped     = ped;
			g_replayAim.dir     = wire;
			g_replayAim.nominal = nominal;
		} else if (!g_saidAimNoDir) {
			g_saidAimNoDir = true;
			Log("combat: a shot from player net %u carried (%.2f %.2f %.2f), which is "
			    "not a direction, so it goes wherever our copy of their ped is facing",
			    player.netId, shot.dir.x, shot.dir.y, shot.dir.z);
		}
	}

	// A ped with a gun target takes FireInstantHit's first branch, which aims
	// at that target and never calls DoDoomAiming - so the correction above
	// would silently do nothing. Nothing in CoopIII sets this on a remote ped;
	// the engine's own AI can, and a remote player's shot is not the engine's
	// to aim. Put back immediately afterwards, same value, so the reference
	// the target registered on this field is untouched.
	void *const pointGunAt = Field<void *>(ped, offs::PED_POINT_GUN_AT);
	if (pointGunAt)
		Field<void *>(ped, offs::PED_POINT_GUN_AT) = nullptr;

	// The health is refused inside the call (HookedInflictDamage); the
	// reaction the fire path plays before that is fenced here and in
	// HookedReactToAttack / HookedSetFall. combat.h, LocalPlayerHitReaction.
	{
		ReplayGuard     guard;
		ReplayBodyFence fence(localPed);
		Func<FireThisFn>(CWeapon__Fire)(weapon, ped, source);
	}

	// Undo what the engine just spent.
	//
	// CWeapon::Fire's tail decrements m_nAmmoInClip (`dec dword [edi+8]` at
	// 0x0055C7D1) and, for a ped that is not the local player, m_nAmmoTotal
	// as well (0x0055C7E9, taken whenever the total is under 25000). Both of
	// those are this machine's opinion about somebody else's ammunition, and
	// this machine does not get one - the owner is authoritative for their
	// own ped, which is the rule everywhere else in CoopIII.
	//
	// Restored to the last number their own engine reported for this exact
	// slot, which is why RemotePlayer keeps a table per weapon rather than
	// only the held one: `shot.weapon` is what the shooter fired, and a
	// snapshot for a different weapon can easily be the most recent thing to
	// arrive.
	//
	// With ammo sync off the slot goes back to the fixed amount GiveWeaponTo
	// hands out, which is where it was before this call.
	if (AmmoSyncOn() && firedSlot < INVENTORY_SLOTS && player.ammoKnown[firedSlot]) {
		WriteRemoteSlotAmmo(ped, firedSlot, player.ammoClip[firedSlot],
		                    player.ammoTotal[firedSlot]);
	} else {
		Field<uint32_t>(weapon, offs::WEAPON_STATE)       = WEAPONSTATE_READY;
		Field<int32_t>(weapon, offs::WEAPON_AMMO_IN_CLIP) = REPLAY_CLIP;
		Field<int32_t>(weapon, offs::WEAPON_AMMO_TOTAL)   = REPLAY_CLIP;
	}

	if (pointGunAt)
		Field<void *>(ped, offs::PED_POINT_GUN_AT) = pointGunAt;

	// The engine never asked us where the shot was going. That means it took
	// a branch that does not call DoDoomAiming, and the shot went along the
	// remote ped's body heading - which is the bug this is here to fix, so it
	// says so rather than looking like success.
	if (g_replayAim.ped && g_replayAim.applied == 0 && !g_saidAimNotAsked) {
		g_saidAimNotAsked = true;
		Log("combat: replayed a shot for player net %u with weapon %u and the engine "
		    "never called CWeapon::DoDoomAiming, so we could not aim it. It went along "
		    "their ped's heading here. Either the hook is not installed (the lines "
		    "above say) or FireInstantHit took a branch that skips it",
		    player.netId, shot.weapon);
	}
	g_replayAim = ReplayAim{};

	if (localPed && !wasProof)
		Field<uint8_t>(localPed, offs::ENTITY_FLAGS_C) = static_cast<uint8_t>(
		    Field<uint8_t>(localPed, offs::ENTITY_FLAGS_C) & ~offs::ENTITY_BULLET_PROOF);

	if (!g_saidShotReplayed) {
		g_saidShotReplayed = true;
		Log("combat: replayed our first remote shot through the engine's own "
		    "CWeapon::Fire - player net %u, weapon %u, from (%.1f %.1f %.1f) along "
		    "(%.2f %.2f %.2f)",
		    player.netId, shot.weapon, shot.origin.x, shot.origin.y, shot.origin.z,
		    shot.dir.x, shot.dir.y, shot.dir.z);
	}

	if (!projectile)
		return;

	// The projectile the engine just created, moved to where its owner threw
	// it, then marked as ours to animate rather than end.
	const uint32_t created = InUseMask() & ~before;
	int            slot    = -1;
	for (int i = 0; i < NUM_PROJECTILES; ++i) {
		if (!(created & (1u << i)))
			continue;
		if (Field<void *>(ProjInfo(i), PROJINFO_SOURCE) != ped)
			continue;
		if (!ProjObject(i))
			continue;
		slot = i;
		break;
	}

	if (slot < 0) {
		// The fire path ran and made nothing to fly. Only two things in
		// CWeapon::FireProjectile do that: its line-of-sight check failed, in
		// which case it called RemoveNotAdd and the replay guard swallowed the
		// blast, or all 32 CProjectileInfo slots are busy. Either way the
		// owner's own explosion is still on its way and will still play, which
		// is precisely the "the blast syncs, nothing flies" symptom - so it
		// gets a line of its own rather than looking like the bug below.
		if (!g_saidNoProjectile) {
			g_saidNoProjectile = true;
			Log("combat: replayed a projectile shot for player net %u with weapon %u "
			    "and the engine created nothing to fly. CWeapon::FireProjectile only "
			    "skips CProjectileInfo::AddProjectile when its line-of-sight check "
			    "fails or all %d slots are in use. Their explosion still arrives on "
			    "its own packet",
			    player.netId, shot.weapon, NUM_PROJECTILES);
		}
		return;
	}

	PlaceRemoteProjectile(slot, shot);

	g_tracked[slot].active   = true;
	g_tracked[slot].playerId = player.playerId;
	g_tracked[slot].source   = ped;
	g_tracked[slot].bornMs   = Global<uint32_t>(CTimer__m_snTimeInMilliseconds);

	if (!g_saidProjectileFlying) {
		g_saidProjectileFlying = true;
		const float *const p = &Field<float>(ProjObject(slot), offs::POSITION);
		Log("combat: a remote player's projectile is in the air - weapon %u, slot %d, "
		    "at (%.1f %.1f %.1f) heading (%.2f %.2f %.2f) at %.2f. %s",
		    shot.weapon, slot, p[0], p[1], p[2], shot.dir.x, shot.dir.y, shot.dir.z,
		    shot.speed,
		    ProjectileSpawnsAtThrower(shot.weapon)
		        ? "It had to be moved: CProjectileInfo::AddProjectile's rocket arm "
		          "for a ped that is not the player ignores the fire source and "
		          "leaves the missile inside the thrower"
		        : "It was already close to right - this weapon's arm does read the "
		          "fire source - and is now exactly where its owner had it");
	}
}

void PlayRemoteExplosion(RemotePlayer &player, const ExplosionBody &body) {
	if (!IsKnownExplosionType(body.type))
		return;

	// Their projectile has arrived, wherever this machine happened to have
	// it flying. Ended first, so the bottle isn't still in the air over the
	// fire it just started.
	EndTrackedFor(player.playerId);

	float pos[3];
	pos[0] = ClampToWorld(body.pos.x);
	pos[1] = ClampToWorld(body.pos.y);
	if (!FiniteOr(body.pos.z, 0.0f, pos[2]))
		return;   // a NaN height isn't a place - drop the whole event

	// Credited to their ped where there is one, so the blame the engine
	// records points at the player who threw it instead of at nobody. Null
	// is fine too - it's what the script's own ADD_EXPLOSION passes.
	//
	// Not guarded by g_replaying: this explosion is supposed to happen. It
	// won't get relayed back out either, since the culprit isn't the local
	// player - same test the sampler uses.
	void *const culprit = ResolveRemotePed(player);

	// Friendly fire, the one place the server can't enforce it.
	//
	// Every other kind of damage between players is a C_Damage the server
	// can simply refuse to relay. A blast isn't: it's replayed here, at a
	// position its owner chose, and this machine's own engine decides
	// whether the local player is standing in it. So this machine has to be
	// the one that declines, and the same bit flip ped.cpp uses on remote
	// peds does it.
	//
	// The bit gets restored rather than the byte, same as the bulletproof
	// flip in ReplayRemoteShot: the player may legitimately already be
	// explosion-proof from a cheat or a mission.
	void *const  localPed = PlayerPed();
	const bool   guard    = !g_friendlyFire && localPed != nullptr;
	const bool   wasProof =
	    guard && (Field<uint8_t>(localPed, offs::ENTITY_FLAGS_B) &
	                offs::ENTITY_EXPLOSION_PROOF) != 0;
	if (guard)
		Field<uint8_t>(localPed, offs::ENTITY_FLAGS_B) |= offs::ENTITY_EXPLOSION_PROOF;

	using AddFn = bool(__cdecl *)(void *, void *, int, const float *, uint32_t);
	Func<AddFn>(CExplosion__AddExplosion)(nullptr, culprit,
	                                      static_cast<int>(body.type), pos, 0);

	if (guard && !wasProof)
		Field<uint8_t>(localPed, offs::ENTITY_FLAGS_B) = static_cast<uint8_t>(
		    Field<uint8_t>(localPed, offs::ENTITY_FLAGS_B) & ~offs::ENTITY_EXPLOSION_PROOF);
}

// ---- damage, death and respawn ---------------------------------------------

void ApplyRemoteDamage(RemotePlayer *attacker, const DamageBody &body) {
	void *const ped = PlayerPed();
	if (!ped)
		return;   // no player right now: menus, loading, between lives

	// Everything below is a bound on something that arrived off a socket and
	// is about to be handed to the engine. The weapon steers a switch in
	// InflictDamage, the piece steers another one and decides which limb
	// comes off, and the amount reaches CPed::m_fHealth - which is read by
	// the HUD, by the AI and, through the death path, by the ped's matrix.
	if (!IsForwardableDamage(body.weapon))
		return;
	if (!IsKnownPedPiece(body.piece))
		return;

	float amount = 0.0f;
	if (!FiniteOr(body.amount, 0.0f, amount) || !(amount > 0.0f))
		return;
	if (amount > MAX_REMOTE_DAMAGE)
		amount = MAX_REMOTE_DAMAGE;

	const uint32_t direction =
	    IsKnownDamageDirection(body.direction) ? body.direction : 0u;

	// Blame. Passing their ped means the engine's own bookkeeping - the
	// threat entity, CDarkel's kill register, the blood - points at the
	// player who did it rather than at nobody. Null is fine and means the
	// same thing the script's own damage calls mean by it.
	void *const culprit = attacker ? ResolveRemotePed(*attacker) : nullptr;

	// Through the real function, not the trampoline. The detour above passes
	// straight through for a ped that isn't a remote player, and the local
	// player never is, so this reaches the engine either way - including on
	// a build where the hook failed to install.
	//
	// Guarded, because the culprit being the attacker's ped is exactly what
	// the detour's remote-attacker rule refuses. That rule is for hits this
	// machine invented; this one was decided by the machine entitled to
	// decide it and has already crossed the network.
	// The reaction first, as the engine's fire path does it. InflictDamage
	// plays none for a hit that doesn't kill, and the replay of this round is
	// no longer allowed to (combat.h, LocalPlayerHitReaction).
	PlayForwardedHitReaction(ped, culprit, body.weapon, direction);

	// A punch or a bat brings the fight code's half with it (melee.h).
	const MeleeTag melee = ReadMeleeTag(body.weapon, body.melee, body.hitLevel);
	MeleeReaction  fight;
	if (!MeleeBeforeDamage(ped, true, culprit, melee, body.weapon, amount, direction, fight))
		return;

	{
		RemoteDamageGuard guard;
		Func<InflictThisFn>(CPed__InflictDamage)(ped, culprit, body.weapon, amount,
		                                         body.piece, direction);
	}

	if (fight.play) {
		MeleeAfterDamage(ped, true, culprit, melee, body.weapon, direction, fight);
		SayMeleeApplied(melee, body.weapon, true);
	}

	if (!g_saidHitApplied) {
		g_saidHitApplied = true;
		Log("combat: took our first hit off the wire, %.0f from %s with cause %u",
		    amount, attacker ? attacker->nick.c_str() : "someone we have no ped for",
		    body.weapon);
	}
}

void ApplyRemotePedDamage(RemotePlayer *attacker, const PedDamageBody &body) {
	// The one thing this machine is entitled to hurt on somebody else's word:
	// a pedestrian of its own that the session has named. population.cpp's
	// filter is the entitlement, and it is the same filter NoteHostedPedDeath
	// uses in the other direction - a replica fails it, which is what stops a
	// machine being talked into hurting a pedestrian it is only watching.
	void *const ped = ResolveHostedPed(body.netId);
	if (!ped) {
		// A netId we no longer host. Dropped, not retried, and that is the
		// decision rather than an omission: a hit only means anything at the
		// instant it happened, and there is nothing left to land it on.
		// CPopulation reaps pedestrians constantly and the round trip is
		// several frames long, so this is the ordinary race and not an error.
		if (!g_saidPedHitGone) {
			g_saidPedHitGone = true;
			Log("combat: a hit arrived for pedestrian net %u and we do not host one "
			    "under that name any more - our engine reaped him inside the round "
			    "trip. Dropped, because a hit is only worth anything at the moment "
			    "it happened", body.netId);
		}
		return;
	}

	// Every bound below is on something that arrived off a socket and is about
	// to be handed to the engine, and each one is the same bound
	// ApplyRemoteDamage applies for a player: the weapon steers a switch inside
	// InflictDamage, the piece steers another one and decides which limb comes
	// off, the direction indexes two four-entry jump tables, and the amount
	// reaches CPed::m_fHealth - from where the death path takes it into the
	// ped's own matrix.
	//
	// Cause 9 first, because it is not damage at all: the shooter's flame
	// reached this pedestrian on their screen (combat.h, IsFlameIgnition).
	if (IsFlameIgnition(body.weapon)) {
		LightHostedPedForFlame(attacker, ped, body.netId);
		return;
	}
	if (!IsForwardableDamage(body.weapon))
		return;
	if (!IsKnownPedPiece(body.piece))
		return;

	float amount = 0.0f;
	if (!FiniteOr(body.amount, 0.0f, amount) || !(amount > 0.0f))
		return;
	if (amount > MAX_REMOTE_DAMAGE)
		amount = MAX_REMOTE_DAMAGE;

	const uint32_t direction =
	    IsKnownDamageDirection(body.direction) ? body.direction : 0u;

	// Blame, and it is the same choice ApplyRemoteDamage makes for the same
	// reason: passing the shooter's own ped means the engine's bookkeeping - the
	// blood, the threat entity, the ped's own reaction - points at the player
	// who did it rather than at nobody. Null is fine and means what the
	// script's own damage calls mean by it; the function's `test esi,esi / je`
	// at 0x004EAACC skips only the car-ramming speed block and the damage still
	// lands.
	//
	// What it does *not* buy is the kill register. At 0x004EAD1A the engine
	// credits CDarkel only when `damagedBy` is FindPlayerPed() or
	// FindPlayerVehicle(), and on this machine the shooter is a replica of
	// somebody else's ped - so the engine registers a co-op kill on an NPC on
	// neither machine: the shooter cannot either, because its own
	// InflictDamage returned before reaching that arm. During a rampage that
	// is put right below, by CreditRemotePedKill (game/darkel.cpp) once the
	// ped is dead. Outside one, nothing registers it.
	void *const culprit = attacker ? ResolveRemotePed(*attacker) : nullptr;

	// Read either side of the call, because "the packet arrived" and "the
	// pedestrian was hurt" are two different claims and the gap between them is
	// exactly where the last two rounds of this were lost.
	const float    before      = Field<float>(ped, offs::PED_HEALTH);
	const uint32_t stateBefore = Field<uint32_t>(ped, offs::PED_STATE);

	// A punch or a bat: the ped reacts, defends, and after the damage falls
	// and is shoved, here where he lives (melee.h). A pedestrian is never a
	// player, so nothing spares him.
	const MeleeTag melee = ReadMeleeTag(body.weapon, body.melee, body.hitLevel);
	MeleeReaction  fight;
	MeleeBeforeDamage(ped, false, culprit, melee, body.weapon, amount, direction, fight);

	// Through the real function rather than the trampoline, the same as
	// ApplyRemoteDamage: the detour passes straight through for a ped that is
	// neither the local player nor a replica, and a pedestrian this machine
	// hosts is neither - so this reaches the engine on a build where the hook
	// failed to install too.
	//
	// The remote-damage guard is not needed here and is deliberately not taken.
	// The rule it exists to get past is the one about the *local player's*
	// health, and `self` is not the local player; taking the guard anyway would
	// widen a session-wide flag over a call that does not need it.
	Func<InflictThisFn>(CPed__InflictDamage)(ped, culprit, body.weapon, amount,
	                                         body.piece, direction);

	if (fight.play) {
		MeleeAfterDamage(ped, false, culprit, melee, body.weapon, direction, fight);
		SayMeleeApplied(melee, body.weapon, false);
	}

	const float after = Field<float>(ped, offs::PED_HEALTH);

	// The hit killed him, and the kill has just been credited to nobody.
	//
	// `culprit` above is the replica of the shooter's ped, which is the right
	// entity for the blood, the threat and the reaction - and is precisely
	// what stops the engine crediting the kill, because the test at
	// 0x004EAD1A only accepts FindPlayerPed() or FindPlayerVehicle(). So the
	// engine sent this one to RegisterKillNotByPlayer, and the shooter's own
	// machine could not register it either. It is a rampage kill made by a
	// player and it is counting for nobody in the session.
	//
	// game/darkel.cpp puts it through the engine's own register instead,
	// which is also what sends it to every other machine. Nothing happens
	// here when no frenzy is running, which is almost always.
	const uint32_t stateAfter = Field<uint32_t>(ped, offs::PED_STATE);
	const bool     wasAlive   = stateBefore != PEDSTATE_DIE && stateBefore != PEDSTATE_DEAD;
	const bool     nowDead    = stateAfter == PEDSTATE_DIE || stateAfter == PEDSTATE_DEAD;
	if (wasAlive && nowDead)
		CreditRemotePedKill(ped, body.weapon, body.piece);

	// A pedestrian in a seat, which is worth its own line because "I shot the
	// driver and nothing happened" is a true statement about retail GTA III and
	// the log should say so rather than leave it looking like a sync bug.
	// InflictDamage's in-vehicle arm sends everything but drowning to
	// 0x004EADD0, which writes 1.0f into m_fHealth and returns false.
	if (!g_saidPedHitInCar && Field<bool>(ped, offs::PED_IN_VEHICLE)) {
		g_saidPedHitInCar = true;
		Log("combat: the first hit we took off the wire for one of our pedestrians "
		    "landed on one sitting in a car (net %u, %.0f health before, %.0f "
		    "after). Retail 1.0 cannot kill a ped in a seat with anything but "
		    "drowning - CPed::InflictDamage clamps him to exactly 1.0f at "
		    "0x004EADD0 and answers 'did not die'. So he will not drop, here or "
		    "on the shooter's screen, and that is the game rather than the wire",
		    body.netId, before, after);
	}

	if (after < before) {
		if (!g_saidPedHitApplied) {
			g_saidPedHitApplied = true;
			Log("combat: hurt one of our own pedestrians off the wire - net %u, "
			    "%.2f health this hit, %.0f left, reported by %s with cause %u. "
			    "Whatever comes of it goes back out on our own C_PedBodyPart and "
			    "C_PedDeath", body.netId, before - after, after,
			    attacker ? attacker->nick.c_str() : "someone we have no ped for",
			    body.weapon);
		}
	} else if (!g_saidPedHitNoMove) {
		g_saidPedHitNoMove = true;
		Log("combat: a hit off the wire reached CPed::InflictDamage on our "
		    "pedestrian net %u and took no health off (%.0f before, %.0f after, "
		    "%.2f asked for). The authority rule is not what stopped it - look at "
		    "bUsesCollision, at the ped already dying, or at a seat",
		    body.netId, before, after, amount);
	}
}

void KillRemotePed(RemotePlayer &player, uint16_t animId) {
	void *const ped = ResolveRemotePed(player);
	if (!ped)
		return;

	// Already a corpse. SetDie returns early for this itself, but calling it
	// twice is still worth not doing: the second call would be a second
	// SetStoredState over a state that is already the death's.
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return;

	// The id goes to CAnimManager::BlendAnimation against ASSOCGRP_STD with
	// nothing in between, so it gets bounded against that group's real size
	// first. PlanDeathAnim is pure arithmetic and clienttest covers it.
	const uint16_t anim = PlanDeathAnim(animId, StdAnimGroupCount());

	Func<SetDieThisFn>(CPed__SetDie)(ped, anim, PED_DIE_DELTA, PED_DIE_SPEED);

	// Whatever was driven into this ped is gone with it. ClearAll took the
	// weapon model off the hand and SetDie replaced the animation, so
	// leaving these set would have ApplyRemotePose skip both as "already
	// applied" if the ped somehow came back.
	player.appliedWeapon  = 0xFFFF;
	player.appliedAnimId  = ANIM_NONE;
	player.appliedAnimId2 = ANIM_NONE;
}

} // namespace coopiii::game
