#include "object.h"

#include "addresses.h"
#include "hook/hook.h"
#include "log.h"

#include <cmath>
#include <cstring>

namespace coopiii::game {
namespace {

namespace obj = ::coopiii::game::object;

ObjectCallbacks g_cb;
ObjectStats     g_stats;

Detour g_damage;      // CObject::ObjectDamage
Detour g_explosion;   // CWorld::TriggerExplosion

// CObject::ObjectDamage is __thiscall(float). The float does not fit in a
// register under that convention, so __fastcall with a dummy edx lines up
// byte for byte: ecx = this, the float on the stack, callee cleans 4.
using ObjectDamageFn = void(__fastcall *)(void *self, void * /*edx*/, float amount);

// CWorld::TriggerExplosion is __cdecl. Nothing here reads its arguments - the
// detour exists only to bracket the call - so they are taken as opaque and
// forwarded unchanged.
using TriggerExplosionFn = void(__cdecl *)(const void *position, float radius,
                                           float power, void *creator,
                                           bool processBombTimer);

// Depth rather than a bool, because CExplosion::AddExplosion and
// CExplosion::Update are two separate callers and there is nothing in the
// engine that promises one cannot end up inside the other.
int g_insideExplosion = 0;

// Set while OnObjectBrokenElsewhere is replaying somebody else's break
// through the engine. Without it the detour would see its own applied damage
// as a local break and report it straight back out, and with two players that
// is an infinite loop rather than one wasted packet.
bool g_applying = false;

// ---------------------------------------------------------------------------
// Reading a CObject
// ---------------------------------------------------------------------------

uint8_t FlagsA(void *entity) { return Field<uint8_t>(entity, offs::ENTITY_FLAGS_A); }
uint8_t FlagsB(void *entity) { return Field<uint8_t>(entity, offs::ENTITY_FLAGS_B); }

uint8_t BreakStateOf(void *object) {
	return BreakStateFromFlags(FlagsA(object), FlagsB(object));
}

// The key: where the MAP put it, not where it is now. They are the same
// number until something knocks the object loose, and at that moment the
// entity's own position stops being a name for it.
ObjectIdent IdentOf(void *object) {
	ObjectIdent id{};
	const float *p = &Field<float>(object, obj::OBJECT_MATRIX_POS);
	id.pos.x      = p[0];
	id.pos.y      = p[1];
	id.pos.z      = p[2];
	id.modelIndex = Field<int16_t>(object, offs::MODEL_INDEX);
	return id;
}

bool IsMapObject(void *object) {
	return Field<uint8_t>(object, obj::CREATED_BY) == obj::GAME_OBJECT;
}

bool IsPickupObject(void *object) {
	return (Field<uint8_t>(object, obj::OBJECT_FLAGS) & obj::OBJ_IS_PICKUP) != 0;
}

// ---------------------------------------------------------------------------
// Pools
// ---------------------------------------------------------------------------
//
// The three-field CPool header, read the way CPopulation::ManagePopulation's
// own inlined walk reads it. Nothing here calls into the engine's pool code,
// because the one thing that has to be right - "is this pointer a live slot
// in this pool" - is arithmetic, and doing it here is what makes it safe to
// ask about a pointer the engine handed us out of m_pDamageEntity.

ObjectPoolView ViewPool(uintptr_t poolPointer, size_t stride) {
	ObjectPoolView v;
	auto *pool = *reinterpret_cast<uint8_t **>(poolPointer);
	if (!pool)
		return v;
	v.entries = *reinterpret_cast<uint8_t **>(pool + obj::POOL_ENTRIES);
	v.flags   = *reinterpret_cast<uint8_t **>(pool + obj::POOL_FLAGS);
	v.size    = *reinterpret_cast<int32_t *>(pool + obj::POOL_SIZE);
	v.stride  = stride;
	return v;
}

bool SlotIsLive(const ObjectPoolView &v, int32_t index) {
	return (v.flags[index] & obj::POOLFLAG_ISFREE) == 0;
}

// The slot index of a pointer, or -1 if it is not one.
//
// Three things have to hold and all three are checked: inside the array, on a
// slot boundary, and not free. The third is the one that matters most here -
// m_pDamageEntity is a raw pointer the engine wrote at some earlier point in
// the frame and never clears, so it can name a slot whose entity has since
// been deleted.
int32_t SlotIndexOf(const ObjectPoolView &v, const void *entity) {
	if (!v.entries || !v.flags || v.size <= 0 || !entity || v.stride == 0)
		return -1;
	const auto *p = static_cast<const uint8_t *>(entity);
	if (p < v.entries)
		return -1;
	const size_t offset = static_cast<size_t>(p - v.entries);
	if (offset % v.stride != 0)
		return -1;
	const size_t index = offset / v.stride;
	if (index >= static_cast<size_t>(v.size))
		return -1;
	if (!SlotIsLive(v, static_cast<int32_t>(index)))
		return -1;
	return static_cast<int32_t>(index);
}

// ---------------------------------------------------------------------------
// Who caused it
// ---------------------------------------------------------------------------

using PoolRefFn = int32_t(__cdecl *)(void *entity);

BreakCause CauseOf(void *object) {
	// CPhysical::m_pDamageEntity. Set by CPhysical's collision bookkeeping
	// alongside m_fDamageImpulse, which is the argument CObject::ProcessControl
	// passes straight into ObjectDamage.
	void *cause = Field<void *>(object, obj::DAMAGE_ENTITY);
	if (!cause)
		return BreakCause::NOBODY;

	const uint8_t type = Field<uint8_t>(cause, offs::ENTITY_FLAGS) & 0x07;

	if (type == ::coopiii::game::ENTITY_TYPE_VEHICLE) {
		const ObjectPoolView pool =
		    ViewPool(CPools__ms_pVehiclePool, offs::SIZEOF_AUTOMOBILE);
		if (SlotIndexOf(pool, cause) < 0)
			return BreakCause::NOBODY;   // stale pointer, not a live car
		if (!g_cb.IsReplicatedVehicle)
			return BreakCause::OURS;
		const int32_t ref = Func<PoolRefFn>(CPools__GetVehicleRef)(cause);
		return g_cb.IsReplicatedVehicle(ref) ? BreakCause::REPLICA
		                                     : BreakCause::OURS;
	}

	if (type == offs::ENTITY_TYPE_PED) {
		const ObjectPoolView pool =
		    ViewPool(CPools__ms_pPedPool, offs::SIZEOF_PLAYER_PED);
		if (SlotIndexOf(pool, cause) < 0)
			return BreakCause::NOBODY;
		if (!g_cb.IsReplicatedPed)
			return BreakCause::OURS;
		const int32_t ref = Func<PoolRefFn>(CPools__GetPedRef)(cause);
		return g_cb.IsReplicatedPed(ref) ? BreakCause::REPLICA
		                                 : BreakCause::OURS;
	}

	// A building, another object, a dummy. Nobody owns it, so this falls to
	// the host under MayReportBreak's last arm.
	return BreakCause::NOBODY;
}

// ---------------------------------------------------------------------------
// Finding our copy of somebody else's object
// ---------------------------------------------------------------------------

// Run the engine's own ObjectDamage, without letting the detour treat it as a
// local break. Calls the real address rather than the trampoline on purpose:
// the detour has to run, because the detour is what holds g_applying, and if
// the hook failed to install there is no trampoline to call at all.
void ReplayDamage(void *object, float amount) {
	Func<ObjectDamageFn>(obj::CObject__ObjectDamage)(object, nullptr, amount);
}

// ---------------------------------------------------------------------------
// The detours
// ---------------------------------------------------------------------------

void __fastcall HookedObjectDamage(void *self, void * /*edx*/, float amount) {
	if (!self) {
		// Cannot happen from any of the five call sites, all of which reach
		// this through a live entity pointer. Forwarding rather than
		// returning keeps the detour honest about being a pass-through.
		g_damage.Original<ObjectDamageFn>()(self, nullptr, amount);
		return;
	}

	const uint8_t before = BreakStateOf(self);
	g_damage.Original<ObjectDamageFn>()(self, nullptr, amount);
	const uint8_t after = BreakStateOf(self);

	// The overwhelmingly common case: the amount did not beat the threshold,
	// or the object has no damage effect at all, and ObjectDamage returned
	// without writing anything. This is on the collision path of every frame,
	// so it stays two byte reads and a compare.
	if (after == before)
		return;

	++g_stats.breaksSeen;

	if (g_applying)
		return;   // this break is one we were told about, not one we saw

	if (!IsMapObject(self) || IsPickupObject(self)) {
		++g_stats.skippedNotMapObj;
		return;
	}
	if (g_insideExplosion > 0) {
		++g_stats.skippedExplosion;
		return;
	}

	const bool haveSession = g_cb.HaveSession && g_cb.HaveSession();
	if (!haveSession)
		return;

	const bool       isHost = g_cb.IsHost && g_cb.IsHost();
	const BreakCause cause  = CauseOf(self);

	if (!MayReportBreak(Field<uint8_t>(self, obj::CREATED_BY),
	                    IsPickupObject(self), haveSession,
	                    g_insideExplosion > 0, cause, isHost)) {
		if (cause == BreakCause::REPLICA)
			++g_stats.skippedReplica;
		else if (cause == BreakCause::NOBODY)
			++g_stats.skippedUnowned;
		return;
	}

	ObjectBreakBody body{};
	body.ident  = IdentOf(self);
	body.amount = amount;
	body.state  = after;

	if (g_cb.Broken && g_cb.Broken(body))
		++g_stats.reported;
	else
		++g_stats.unsent;
}

void __cdecl HookedTriggerExplosion(const void *position, float radius,
                                    float power, void *creator,
                                    bool processBombTimer) {
	++g_insideExplosion;
	g_explosion.Original<TriggerExplosionFn>()(position, radius, power, creator,
	                                           processBombTimer);
	--g_insideExplosion;
}

}  // namespace

// ---------------------------------------------------------------------------

bool SameObject(const ObjectIdent &a, const ObjectIdent &b) {
	if (a.modelIndex != b.modelIndex)
		return false;
	const float dx = a.pos.x - b.pos.x;
	const float dy = a.pos.y - b.pos.y;
	const float dz = a.pos.z - b.pos.z;
	return dx * dx + dy * dy + dz * dz <= kObjectIdentToleranceSq;
}

void SetObjectCallbacks(const ObjectCallbacks &callbacks) { g_cb = callbacks; }

ObjectPoolView LiveObjectPool() {
	return ViewPool(CPools__ms_pObjectPool, obj::OBJECT_POOL_STRIDE);
}

// A linear walk of 450 slots, and deliberately so: this runs once per inbound
// packet, a break is a rare event, and an index would be a second structure
// to keep in step with a pool the engine churns every frame.
void *FindObjectByIdent(const ObjectPoolView &pool, const ObjectIdent &want,
                        bool *ambiguous) {
	if (ambiguous)
		*ambiguous = false;
	if (!pool.entries || !pool.flags || pool.stride == 0)
		return nullptr;

	void *best     = nullptr;
	float bestDist = kObjectIdentToleranceSq;
	int   matches  = 0;

	for (int32_t i = 0; i < pool.size; ++i) {
		if (!SlotIsLive(pool, i))
			continue;
		void *candidate = pool.entries + static_cast<size_t>(i) * pool.stride;
		if (Field<int16_t>(candidate, offs::MODEL_INDEX) != want.modelIndex)
			continue;
		if (!IsMapObject(candidate) || IsPickupObject(candidate))
			continue;

		const ObjectIdent here = IdentOf(candidate);
		const float dx = here.pos.x - want.pos.x;
		const float dy = here.pos.y - want.pos.y;
		const float dz = here.pos.z - want.pos.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 > kObjectIdentToleranceSq)
			continue;

		++matches;
		if (d2 <= bestDist) {
			bestDist = d2;
			best     = candidate;
		}
	}

	if (ambiguous)
		*ambiguous = matches > 1;
	return best;
}

const ObjectStats &GetObjectStats() { return g_stats; }

void OnObjectBrokenElsewhere(const ObjectBreakBody &body) {
	++g_stats.received;

	bool  ambiguous = false;
	void *object    = FindObjectByIdent(LiveObjectPool(), body.ident, &ambiguous);
	if (ambiguous)
		++g_stats.identCollisions;

	if (!object) {
		// Ordinary, not an error. Their player is standing next to it and
		// ours is not, so our copy is a CDummyObject 80 m outside the
		// conversion range and there is nothing here to break. Walking over
		// there builds a pristine one, which is what single player does too.
		++g_stats.receivedUnmatched;
		return;
	}

	const uint8_t local   = BreakStateOf(object);
	int           replays = BreakReplaysNeeded(local, body.state);
	if (replays == 0) {
		++g_stats.receivedNoop;
		return;
	}

	// The amount the reporter's engine actually used. ObjectDamage reads it
	// twice - once for `amount * m_fCollisionDamageMultiplier > 150.0f` and
	// once for the debris velocity - so replaying with the real number is
	// what makes the crate burst the same way on both screens.
	//
	// Clamped up, and only up, for the one case a straight replay would get
	// wrong: the two machines can disagree by a hair on an impulse that was
	// only just over the line, and an applier that silently did nothing would
	// leave the object standing here and gone there. The receiver is not
	// deciding anything by doing this - the reporter already decided, and this
	// is how the decision is carried out.
	float amount = body.amount;
	const float multiplier = Field<float>(object, obj::DAMAGE_MULTIPLIER);
	if (multiplier > 0.0f) {
		const float needed = (obj::OBJECT_DAMAGE_THRESHOLD / multiplier) * 1.01f;
		if (amount < needed)
			amount = needed;
	}

	g_applying = true;
	for (int i = 0; i < replays; ++i) {
		ReplayDamage(object, amount);
		++g_stats.replaysRun;
	}
	g_applying = false;

	if (BreakStateOf(object) == local)
		++g_stats.applyFailed;
}

bool InstallObjectHooks() {
	g_stats           = ObjectStats{};
	g_insideExplosion = 0;
	g_applying        = false;

	const bool damage = g_damage.Install(
	    "CObject::ObjectDamage",
	    reinterpret_cast<void *>(obj::CObject__ObjectDamage),
	    reinterpret_cast<void *>(&HookedObjectDamage));

	// Second, and separately reported, because the two failures are not the
	// same failure. Without the explosion guard everything still works and
	// every blast sends a handful of duplicate packets; without the damage
	// hook nothing works at all.
	const bool explosion = g_explosion.Install(
	    "CWorld::TriggerExplosion",
	    reinterpret_cast<void *>(obj::CWorld__TriggerExplosion),
	    reinterpret_cast<void *>(&HookedTriggerExplosion));

	if (damage && explosion) {
		Log("object: hooked CObject::ObjectDamage at 0x%08X and "
		    "CWorld::TriggerExplosion at 0x%08X - breaking street objects is "
		    "shared, and explosions stay quiet because they already agree",
		    obj::CObject__ObjectDamage, obj::CWorld__TriggerExplosion);
		return true;
	}

	if (damage && !explosion) {
		Log("object: hooked CObject::ObjectDamage but NOT "
		    "CWorld::TriggerExplosion at 0x%08X - breaking is shared, and "
		    "every explosion will now also send one packet per object in its "
		    "radius that the other machines had already worked out for "
		    "themselves",
		    obj::CWorld__TriggerExplosion);
	} else {
		Log("object: FAILED to hook CObject::ObjectDamage at 0x%08X - lamp "
		    "posts, meters and crates stay broken on one machine only",
		    obj::CObject__ObjectDamage);
	}
	for (const auto &f : HookFailures())
		Log("object:   %s: %s", f.name.c_str(), f.reason.c_str());
	return false;
}

void RemoveObjectHooks() {
	g_cb = ObjectCallbacks{};
	g_damage.Remove();
	g_explosion.Remove();
	g_insideExplosion = 0;
	g_applying        = false;
}

bool ObjectHooksInstalled() { return g_damage.IsInstalled(); }

void AddObjectsToBridge(WorldBridge &bridge) {
	bridge.ObjectBroken = &OnObjectBrokenElsewhere;
}

}  // namespace coopiii::game
