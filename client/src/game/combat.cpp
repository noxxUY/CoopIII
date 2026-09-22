#include "combat.h"

#include "hook/hook.h"
#include "log.h"
#include "ped.h"
#include "pedanim.h"

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

Detour g_fire;
Detour g_explode;
Detour g_removeProjectile;
Detour g_inflictDamage;
Detour g_setDie;
Detour g_lineOfSight;
Detour g_bulletImpact;
Detour g_doomAiming;

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
bool g_saidAimApplied    = false;
bool g_saidAimNotAsked   = false;
bool g_saidAimNoDir      = false;
bool g_saidNoLocalRay    = false;
bool g_saidTrailRepaired = false;

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
	// The far end of the last ray, and the pointer the engine handed us for
	// it. The pointer is the evidence, not the value - see HookedDoBulletImpact.
	const float *point2  = nullptr;
	Vec3         farEnd{};
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
		g_localRay.point2 = point2;
		g_localRay.farEnd = Vec3{point2[0], point2[1], point2[2]};
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

	// The line the engine actually traced, averaged over however many rays
	// this discharge was. This is the aim - the camera's, the lock-on's or the
	// hand bone's, whichever branch ran - and no branch logic of our own was
	// needed to get it, which is the point of sampling rather than deriving.
	if (IsInstantHitWeapon(ev.shot.weapon)) {
		Vec3 aim;
		if (g_localRay.rays > 0 && UnitDirection(g_localRay.sum, aim)) {
			ev.shot.dir = aim;
		} else if (!g_saidNoLocalRay) {
			g_saidNoLocalRay = true;
			Log("combat: our own shot with weapon %u traced no line we could read, so "
			    "the direction on the wire is our ped's body heading rather than our "
			    "aim. Everyone watching will draw the trail flat",
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
                       uint32_t piece, uint32_t direction) {
	CombatEvent ev;
	ev.kind               = CombatEvent::DAMAGE;
	ev.damage.victimNetId = victimNetId;
	ev.damage.weapon      = static_cast<uint8_t>(method);
	ev.damage.amount      = damage;
	ev.damage.piece       = static_cast<uint8_t>(piece);
	ev.damage.direction   = static_cast<uint8_t>(direction);
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

	const bool fired =
	    g_fire.Original<FireHookFn>()(self, nullptr, shooter, fireSource);

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
	if (g_replaying && type >= 0 && IsProjectileExplosion(static_cast<uint8_t>(type)))
		return false;

	const bool added = g_explode.Original<AddExplosionFn>()(explodingEntity, culprit,
	                                                        type, pos, lifetime);

	// Only the local player's own explosions. A car blowing up because the
	// city AI decided so already happens on every machine - relaying it
	// would just double it up.
	if (added && pos && culprit && culprit == PlayerPed() && type >= 0 &&
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

	uint16_t victimNetId = INVALID_NETID;
	if (self && RemotePlayerForPed(self, victimNetId)) {
		// Somebody else's player. Their health is theirs.
		//
		// Only what the local player did deliberately goes on the wire. A
		// city NPC shooting a remote player is a shot that happened in one
		// simulation and not in the others (docs/protocol.md §3), and
		// forwarding it would kill someone with a cop they can't see.
		const bool ours = !g_replaying && damagedBy && damagedBy == localPed;
		if (ours && IsForwardableDamage(static_cast<uint8_t>(method))) {
			RecordLocalDamage(victimNetId, method, damage, piece, direction);
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

	return g_inflictDamage.Original<InflictHookFn>()(self, nullptr, damagedBy, method,
	                                                 damage, piece, direction);
}

// __thiscall void CPed::SetDie(AnimationId anim, float delta, float speed).
//
// Here to answer one question: which animation did the engine pick for the
// local player's death? Nothing else can say. The snapshot can't - a death
// animation is created with a blendAmount of 0 and doesn't become the
// dominant one until several frames later, by which point the player has
// been lying on the floor with nobody else told about it.
//
// The announcement itself doesn't depend on this hook. Client::UpdateLocalLife
// watches the sampled health and announces a death with ANIM_NONE if this
// never fired, so a failed install costs the right animation and nothing
// else.
using SetDieHookFn = void(__fastcall *)(void *, void *, uint32_t, float, float);
using SetDieThisFn = void(__thiscall *)(void *, uint32_t, float, float);

void __fastcall HookedSetDie(void *self, void * /*edx*/, uint32_t animId, float delta,
                             float speed) {
	const bool localPlayer = self && self == PlayerPed();

	// SetDie returns without doing anything for a ped that's already dying,
	// and for a player MakePlayerSafe has made undamageable. Both would look
	// like a death from the outside, so the test is the transition rather
	// than the state: alive before, PED_DIE after.
	const uint32_t before =
	    localPlayer ? Field<uint32_t>(self, offs::PED_STATE) : PEDSTATE_NONE;
	const bool wasDying = before == PEDSTATE_DIE || before == PEDSTATE_DEAD;

	g_setDie.Original<SetDieHookFn>()(self, nullptr, animId, delta, speed);

	if (localPlayer && !wasDying &&
	    Field<uint32_t>(self, offs::PED_STATE) == PEDSTATE_DIE)
		RecordLocalDeath(animId);
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
	g_count = g_head = 0;
	g_friendlyFire   = false;
	g_localRay       = LocalRay{};
	g_replayAim      = ReplayAim{};
}

bool CombatHooksInstalled() {
	return g_fire.IsInstalled() && g_explode.IsInstalled() &&
	       g_removeProjectile.IsInstalled() && g_inflictDamage.IsInstalled() &&
	       g_setDie.IsInstalled() && g_lineOfSight.IsInstalled() &&
	       g_bulletImpact.IsInstalled() && g_doomAiming.IsInstalled();
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

void ReplayRemoteShot(RemotePlayer &player, const ShotBody &shot) {
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

	// A seated ped's gun is the car's business - drive-bys run through a
	// different engine path (CWeapon::FireFromCar) that isn't synced. A
	// dying ped has already stopped: CWeapon::Fire would still go through,
	// and a muzzle flash out of a corpse looks worse than nothing at all.
	if (Field<bool>(ped, offs::PED_IN_VEHICLE)) {
		RefuseShot(GATE_SEATED, "their ped is in a car and drive-bys are not synced",
		           player, shot.weapon);
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

	// CWeapon::Fire refuses on an empty clip and on a reloading or
	// out-of-ammo state, and reloads on its own schedule via CTimer. A
	// remote player's clip isn't something anyone can see, and it's not
	// something this machine gets an opinion on - so the slot gets put back
	// into a state the engine will always fire from. docs/protocol.md §1.9.6.
	constexpr int32_t REMOTE_CLIP = 500;
	Field<uint32_t>(weapon, offs::WEAPON_STATE)       = WEAPONSTATE_READY;
	Field<int32_t>(weapon, offs::WEAPON_AMMO_IN_CLIP) = REMOTE_CLIP;
	Field<int32_t>(weapon, offs::WEAPON_AMMO_TOTAL)   = REMOTE_CLIP;
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

	{
		ReplayGuard guard;
		Func<FireThisFn>(CWeapon__Fire)(weapon, ped, source);
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
	{
		RemoteDamageGuard guard;
		Func<InflictThisFn>(CPed__InflictDamage)(ped, culprit, body.weapon, amount,
		                                         body.piece, direction);
	}

	if (!g_saidHitApplied) {
		g_saidHitApplied = true;
		Log("combat: took our first hit off the wire, %.0f from %s with cause %u",
		    amount, attacker ? attacker->nick.c_str() : "someone we have no ped for",
		    body.weapon);
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
