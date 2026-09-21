#include "combat.h"

#include "hook/hook.h"
#include "log.h"
#include "ped.h"
#include "pedanim.h"

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

Detour g_fire;
Detour g_explode;
Detour g_removeProjectile;

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

void *PlayerPed() { return Func<void *(__cdecl *)()>(FindPlayerPed)(); }

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
};

Tracked g_tracked[NUM_PROJECTILES];

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
	Func<RemoveFn>(CProjectileInfo__RemoveProjectile)(ProjInfo(slot), obj);
}

void EndTrackedFor(uint8_t playerId) {
	for (int i = 0; i < NUM_PROJECTILES; ++i)
		if (g_tracked[i].active && g_tracked[i].playerId == playerId)
			EndTracked(i);
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
	ReadForward(shooter, ev.shot.dir);

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

	const bool fired =
	    g_fire.Original<FireHookFn>()(self, nullptr, shooter, fireSource);

	if (fired && !g_replaying && shooter && shooter == PlayerPed())
		RecordLocalShot(self, shooter, fireSource, before);

	return fired;
}

// __cdecl bool CExplosion::AddExplosion(CEntity *explodingEntity,
//                                       CEntity *culprit, eExplosionType type,
//                                       const CVector &pos, uint32 lifetime).
using AddExplosionFn = bool(__cdecl *)(void *, void *, int, const float *, uint32_t);

bool __cdecl HookedAddExplosion(void *explodingEntity, void *culprit, int type,
                                const float *pos, uint32_t lifetime) {
	// While replaying somebody else's shot, this machine isn't allowed to
	// decide an explosion at all (see g_replaying). The refusal gets
	// reported as "no explosion was added," which is exactly what the
	// engine's own out-of-slots answer looks like - and every caller already
	// handles that case.
	if (g_replaying)
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

} // namespace

// ---- installation ---------------------------------------------------------

bool InstallCombatHooks() {
	for (Tracked &t : g_tracked)
		t = Tracked{};
	g_head = g_count = 0;
	g_dropped        = 0;

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
	g_count = g_head = 0;
}

bool CombatHooksInstalled() {
	return g_fire.IsInstalled() && g_explode.IsInstalled() &&
	       g_removeProjectile.IsInstalled();
}

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
	if (!IsReplayableWeapon(shot.weapon))
		return;

	void *const ped = ResolveRemotePed(player);
	if (!ped)
		return;

	// A seated ped's gun is the car's business - drive-bys run through a
	// different engine path (CWeapon::FireFromCar) that isn't synced. A
	// dying ped has already stopped: CWeapon::Fire would still go through,
	// and a muzzle flash out of a corpse looks worse than nothing at all.
	if (Field<bool>(ped, offs::PED_IN_VEHICLE))
		return;
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return;

	// Not yet: the weapon's model is still streaming. Dropped rather than
	// retried, same as the shot itself - by the time it loads, this round
	// is already over.
	if (!GiveRemoteWeapon(player, ped, shot.weapon))
		return;

	void *const weapon = reinterpret_cast<uint8_t *>(ped) + offs::PED_WEAPONS +
	                     static_cast<size_t>(shot.weapon) * offs::SIZEOF_WEAPON;

	// CWeapon::Fire refuses on an empty clip and on a reloading or
	// out-of-ammo state, and reloads on its own schedule via CTimer. A
	// remote player's clip isn't something anyone can see, and it's not
	// something this machine gets an opinion on - so the slot gets put back
	// into a state the engine will always fire from. docs/protocol.md §1.9.4.
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

	{
		ReplayGuard guard;
		Func<FireThisFn>(CWeapon__Fire)(weapon, ped, source);
	}

	if (localPed && !wasProof)
		Field<uint8_t>(localPed, offs::ENTITY_FLAGS_C) = static_cast<uint8_t>(
		    Field<uint8_t>(localPed, offs::ENTITY_FLAGS_C) & ~offs::ENTITY_BULLET_PROOF);

	if (!projectile)
		return;

	// The projectile the engine just created, corrected to the arc its
	// owner actually threw, then marked as ours to animate rather than end.
	const uint32_t created = InUseMask() & ~before;
	for (int i = 0; i < NUM_PROJECTILES; ++i) {
		if (!(created & (1u << i)))
			continue;
		if (Field<void *>(ProjInfo(i), PROJINFO_SOURCE) != ped)
			continue;
		void *const obj = ProjObject(i);
		if (!obj)
			continue;

		// Velocity only - the position AddProjectile chose is left alone. It
		// derives from this ped's matrix, which the pose stream keeps within
		// one snapshot of the truth (half a metre, at a run), while the
		// velocity isn't derivable here at all and is what decides the whole
		// arc. Moving the object too would mean re-filing it in the sector
		// grid - three more engine calls to fix half a metre.
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

		g_tracked[i].active   = true;
		g_tracked[i].playerId = player.playerId;
		g_tracked[i].source   = ped;
		break;
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

	using AddFn = bool(__cdecl *)(void *, void *, int, const float *, uint32_t);
	Func<AddFn>(CExplosion__AddExplosion)(nullptr, culprit,
	                                      static_cast<int>(body.type), pos, 0);
}

} // namespace coopiii::game
