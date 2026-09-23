#include "object.h"

#include "addresses.h"
#include "hook/hook.h"
#include "log.h"
#include "pedanim.h"

#include <cmath>
#include <cstring>

namespace coopiii::game {
namespace {

namespace obj = ::coopiii::game::object;

ObjectCallbacks g_cb;
ObjectStats     g_stats;

Detour g_damage;      // CObject::ObjectDamage
Detour g_explosion;   // CWorld::TriggerExplosion
Detour g_moving;      // CPhysical::AddToMovingList

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

// CPhysical::AddToMovingList is __thiscall(void) - `push ebx / mov ebx,ecx /
// push 0Ch / call CPools::ms_pPtrNodePool::New` at 0x004958F0, five clean
// bytes with no rel32 in them. Hooked as __fastcall with a dummy edx, the
// same trick the damage detour uses.
using AddToMovingListFn = void(__fastcall *)(void *self, void * /*edx*/);

// The three the receiver needs to move an object the engine already owns.
// CMatrix::UpdateRW is __thiscall on the CMatrix, which for an entity is the
// entity plus offs::MATRIX; the other two are __thiscall on the entity.
using MatrixUpdateRWFn = void(__fastcall *)(void *matrix, void * /*edx*/);
using EntityThisFn     = void(__fastcall *)(void *self, void * /*edx*/);

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
// The objects this machine owes everybody a resting place for
// ---------------------------------------------------------------------------
//
// Entered by the AddToMovingList detour at the moment an object comes loose,
// and drained by TickUprootedObjects once the engine says it has stopped.
// Small, fixed and on the game thread only - there is no allocation anywhere
// in this feature and there is not going to be one.
//
// Both halves of the entry are kept on purpose. `object` is how the sleep is
// noticed without searching 450 slots every frame; `ident` is how that
// pointer is checked before it is believed, because the object pool churns
// every frame and a freed slot is reused immediately. A pointer that is no
// longer a live slot holding the object we named is simply dropped.
struct Watched {
	void       *object = nullptr;
	ObjectIdent ident{};
};

Watched g_watched[kMaxUprootedWatched];
int     g_watchedCount = 0;

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

// One of the 1851. m_nCollisionDamageEffect is the same gate ObjectDamage
// itself opens with, and it is what separates street furniture from the
// garage doors, buoys and weapon models that share the pool.
bool IsBreakable(void *object) {
	return Field<uint8_t>(object, obj::DAMAGE_EFFECT) != obj::DAMAGE_EFFECT_NONE;
}

bool IsLoose(void *object) { return ObjectIsLoose(FlagsA(object)); }

bool IsEntityOfType(void *entity, uint8_t type) {
	return (Field<uint8_t>(entity, offs::ENTITY_FLAGS) & 0x07) == type;
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
// Following an object that came loose
// ---------------------------------------------------------------------------

// Is this still the object we put in the table?
//
// Three questions, and the pool answers all of them. The slot has to still be
// live (the pool churns every frame, and a freed slot is reused immediately),
// it has to still be a breakable map object, and the placement it carries has
// to still be the one we named. The third is what makes the first two safe:
// even a slot that was freed and refilled with a different lamp post fails
// here, because m_objectMatrix is the map's coordinate and no two of them
// are inside 0.25 m of each other.
bool StillOurs(const Watched &w) {
	if (!w.object)
		return false;
	const ObjectPoolView pool = LiveObjectPool();
	if (SlotIndexOf(pool, w.object) < 0)
		return false;
	if (!IsMapObject(w.object) || IsPickupObject(w.object))
		return false;
	return SameObject(IdentOf(w.object), w.ident);
}

bool AlreadyWatching(const void *object) {
	for (int i = 0; i < g_watchedCount; ++i)
		if (g_watched[i].object == object)
			return true;
	return false;
}

void Watch(void *object) {
	if (AlreadyWatching(object))
		return;
	if (g_watchedCount >= kMaxUprootedWatched) {
		++g_stats.uprootsDropped;
		return;
	}
	g_watched[g_watchedCount].object = object;
	g_watched[g_watchedCount].ident  = IdentOf(object);
	++g_watchedCount;
	++g_stats.uprootsWatched;
}

void Forget(int index) {
	g_watched[index] = g_watched[g_watchedCount - 1];
	g_watched[g_watchedCount - 1] = Watched{};
	--g_watchedCount;
}

ObjectRestBody RestOf(void *object, const ObjectIdent &ident) {
	ObjectRestBody body{};
	body.ident = ident;

	const float *right = &Field<float>(object, offs::MATRIX_RIGHT);
	const float *fwd   = &Field<float>(object, offs::MATRIX_FWD);
	const float *up    = &Field<float>(object, offs::MATRIX_UP);
	const float *pos   = &Field<float>(object, offs::POSITION);

	body.right.x   = right[0]; body.right.y   = right[1]; body.right.z   = right[2];
	body.forward.x = fwd[0];   body.forward.y = fwd[1];   body.forward.z = fwd[2];
	body.up.x      = up[0];    body.up.y      = up[1];    body.up.z      = up[2];
	body.pos.x     = pos[0];   body.pos.y     = pos[1];   body.pos.z     = pos[2];
	return body;
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
	// The uproot bit rides along rather than travelling on its own. A car
	// that knocks a post down does both in the same contact - CPhysical
	// clears bIsStatic while it resolves it and CObject::ProcessControl
	// passes the impulse to ObjectDamage on the next frame - so by the time
	// we are here the post is already loose, and saying so costs no bytes and
	// lets the observer drop its own copy now instead of a second from now.
	body.state  = static_cast<uint8_t>(after | (IsLoose(self) ? OBJ_BREAK_UPROOTED
	                                                          : 0));

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

// Something just came loose.
//
// This is the whole of "uprooted": the caller has already cleared bIsStatic
// and is now handing the object to CWorld::ms_listMovingEntityPtrs, and from
// the next frame the engine simulates it. Every uproot in the image arrives
// here - the CPhysical collision arm, both CWorld explosion arms and the
// three CWeapon arms - which is why one detour covers a mechanism that has no
// function of its own.
//
// Read-only with respect to the list. The original runs first and then this
// looks at the object; nothing here allocates, links or unlinks a node.
//
// The first two reads are what keep it off the critical path: CWorld::Add
// routes every non-static entity through here, so a ped or a car reaching
// this detour costs one byte read and a compare.
void __fastcall HookedAddToMovingList(void *self, void * /*edx*/) {
	g_moving.Original<AddToMovingListFn>()(self, nullptr);

	if (!self || !IsEntityOfType(self, obj::ENTITY_TYPE_OBJECT))
		return;
	if (!IsBreakable(self) || !IsMapObject(self) || IsPickupObject(self))
		return;

	++g_stats.uprootsSeen;

	if (g_applying)
		return;   // we did this, on somebody else's word

	// Already agreed everywhere, and for the reason the break is: the blast's
	// power is a pure function of the two positions and the radius, so every
	// machine ran the same `power > m_fUprootLimit` on the same number. One
	// rocket into a row of bins would otherwise be one reliable packet per
	// bin. docs/objects.md 2.1.
	if (g_insideExplosion > 0) {
		++g_stats.uprootsBlast;
		return;
	}

	if (!g_cb.HaveSession || !g_cb.HaveSession())
		return;

	// The cause, while it is still a live fact. CPhysical::ProcessControl
	// zeroes m_fDamageImpulse and m_pDamageEntity at the top of the next
	// frame, so a non-zero impulse here belongs to the collision that is
	// unwinding around this call.
	const bool hadImpulse = Field<float>(self, obj::DAMAGE_IMPULSE) > 0.0f;
	const bool replaying  = g_cb.InReplayedShot && g_cb.InReplayedShot();
	const BreakCause cause =
	    UprootCause(hadImpulse, hadImpulse ? CauseOf(self) : BreakCause::NOBODY,
	                replaying);

	const bool isHost = g_cb.IsHost && g_cb.IsHost();
	if (!MayReportBreak(Field<uint8_t>(self, obj::CREATED_BY),
	                    IsPickupObject(self), /*haveSession*/ true,
	                    /*insideExplosion*/ false, cause, isHost)) {
		++g_stats.uprootsNotOurs;
		return;
	}

	Watch(self);
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

namespace {

bool RowIsSane(const Vec3 &v) {
	const float m = v.x * v.x + v.y * v.y + v.z * v.z;
	if (!(m == m))   // NaN fails its own comparison, and so does a NaN sum
		return false;
	return m > 0.25f && m < 4.0f;   // half to double unit length
}

bool AxisIsSane(float f) { return f == f && f > -1.0e9f && f < 1.0e9f; }

}  // namespace

bool SaneRotation(const ObjectRestBody &body) {
	return RowIsSane(body.right) && RowIsSane(body.forward) &&
	       RowIsSane(body.up) && AxisIsSane(body.pos.x) &&
	       AxisIsSane(body.pos.y) && AxisIsSane(body.pos.z);
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

	// Their copy came loose. Drop ours now rather than leaving it standing
	// until the resting place arrives - the fall is the half of this the
	// player is looking at, and a post that stands for a second and then
	// teleports flat is worse than one that goes over and then settles.
	//
	// Guarded on bIsStatic, and that guard is the whole safety of it: the
	// engine's invariant is that CWorld::ms_listMovingEntityPtrs holds
	// exactly the non-static entities, so linking one that is already in
	// there is the second node client/src/game/movinglist.h exists to clean
	// up after. Nothing here unlinks anything either - CWorld::Process does
	// that itself the moment bIsStatic goes back on.
	if ((body.state & OBJ_BREAK_UPROOTED) && !IsLoose(object)) {
		Field<uint8_t>(object, offs::ENTITY_FLAGS_A) &=
		    static_cast<uint8_t>(~offs::ENTITY_IS_STATIC);
		g_applying = true;
		Func<AddToMovingListFn>(CPhysical__AddToMovingList)(object, nullptr);
		g_applying = false;
		++g_stats.looseFromWire;
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

void OnObjectSettledElsewhere(const ObjectRestBody &body) {
	++g_stats.restsReceived;

	bool  ambiguous = false;
	void *object    = FindObjectByIdent(LiveObjectPool(), body.ident, &ambiguous);
	if (ambiguous)
		++g_stats.identCollisions;

	if (!object) {
		// The ordinary case, same as a break: our copy is a CDummyObject
		// because nobody here is within 80 m of it. Walking over there builds
		// a pristine one standing up, which is what single player does after
		// you drive away and is the state both machines will agree on again.
		++g_stats.restsUnmatched;
		return;
	}

	// A rotation off the wire goes straight into the matrix the collision
	// code reads, so it is checked before it is written and the whole packet
	// is refused if any of it is wrong. pedanim.h's rule: NaN propagates, and
	// the fault it causes lands somewhere with no obvious connection to
	// netcode. A near-unit length on all three rows is the cheap version of
	// "is this a rotation", and it rejects the all-zero body a truncated or
	// zero-filled packet would carry.
	if (!SaneRotation(body)) {
		++g_stats.restsRefused;
		return;
	}

	// Clamped for the reason every wire position in this project is clamped:
	// CPhysical::RemoveAndAdd turns the position into a subscript into
	// CWorld::ms_aSectors with no bounds check of its own.
	float *right = &Field<float>(object, offs::MATRIX_RIGHT);
	float *fwd   = &Field<float>(object, offs::MATRIX_FWD);
	float *up    = &Field<float>(object, offs::MATRIX_UP);
	float *pos   = &Field<float>(object, offs::POSITION);

	right[0] = body.right.x;   right[1] = body.right.y;   right[2] = body.right.z;
	fwd[0]   = body.forward.x; fwd[1]   = body.forward.y; fwd[2]   = body.forward.z;
	up[0]    = body.up.x;      up[1]    = body.up.y;      up[2]    = body.up.z;

	const float keepZ = pos[2];
	pos[0] = ClampToWorld(body.pos.x);
	pos[1] = ClampToWorld(body.pos.y);
	FiniteOr(body.pos.z, keepZ, pos[2]);

	// The engine's own three steps, in the engine's own order. UpdateRW
	// copies the CMatrix into the attached RwMatrix, UpdateRwFrame is what
	// makes RenderWare recompute the LTM, and RemoveAndAdd re-files the
	// entity in the sector grid - without which it would be lying in a
	// sector CRenderer::ScanWorld never walks and would simply stop being
	// drawn.
	Func<MatrixUpdateRWFn>(CMatrix__UpdateRW)(
	    reinterpret_cast<uint8_t *>(object) + offs::MATRIX, nullptr);
	Func<EntityThisFn>(CEntity__UpdateRwFrame)(object, nullptr);
	Func<EntityThisFn>(CPhysical__RemoveAndAdd)(object, nullptr);

	// Asleep, the way CPhysical::ProcessControl leaves one: speeds and
	// frictions zeroed, m_nStaticFrames pinned at the engine's own ceiling,
	// bIsStatic back on. That last write is also all the moving list needs -
	// CWorld::Process unlinks a static entity on its own next pass, at
	// 0x004B1BA8.
	//
	// Twelve floats, 0x78 to 0xA4: m_vecMoveSpeed, m_vecTurnSpeed and both
	// frictions. That is exactly what the sleep path writes at 0x004960FB
	// through 0x00496168 - it sets each friction from the speed it has just
	// zeroed - and doing the same thing means a copy put down off the wire is
	// in the same state as one that stopped here.
	for (size_t o = offs::MOVE_SPEED; o < offs::MOVE_SPEED + 0x30; o += 4)
		Field<float>(object, o) = 0.0f;
	Field<uint8_t>(object, obj::STATIC_FRAMES) = obj::STATIC_FRAMES_ASLEEP;
	Field<uint8_t>(object, offs::ENTITY_FLAGS_A) |= offs::ENTITY_IS_STATIC;

	++g_stats.restsApplied;
}

void TickUprootedObjects() {
	for (int i = 0; i < g_watchedCount;) {
		Watched &w = g_watched[i];

		if (!StillOurs(w)) {
			// It left the pool, went back to being a dummy, or the slot is
			// somebody else's now. The 80 m horizon does this constantly and
			// it is not an error - the state has a lifetime of one visit and
			// the engine is the thing that ends it.
			++g_stats.uprootsLost;
			Forget(i);
			continue;
		}

		// Not asleep yet. The test is the engine's own: CPhysical::Process-
		// Control counts quiet frames and calls SetIsStatic(true) past ten,
		// and CWorld::Process unlinks it on the same pass. Asking bIsStatic
		// is asking the engine whether it has finished, rather than guessing
		// at a speed threshold that would have to be tuned.
		if (IsLoose(w.object)) {
			++i;
			continue;
		}

		const ObjectRestBody body = RestOf(w.object, w.ident);
		if (g_cb.Settled && g_cb.Settled(body))
			++g_stats.restsSent;
		else
			++g_stats.restsUnsent;

		Forget(i);
	}
}

bool InstallObjectHooks() {
	g_stats           = ObjectStats{};
	g_insideExplosion = 0;
	g_applying        = false;
	g_watchedCount    = 0;
	for (Watched &w : g_watched)
		w = Watched{};

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

	// Third, and separately reported for the same reason. Without it
	// breaking still travels and every knocked-over lamp post goes back to
	// standing on one screen and lying on the other - which is exactly the
	// state this feature was in before it existed, so the log has to be able
	// to say that is what happened.
	const bool moving = g_moving.Install(
	    "CPhysical::AddToMovingList",
	    reinterpret_cast<void *>(CPhysical__AddToMovingList),
	    reinterpret_cast<void *>(&HookedAddToMovingList));

	if (damage && explosion && moving) {
		Log("object: hooked CObject::ObjectDamage at 0x%08X, "
		    "CWorld::TriggerExplosion at 0x%08X and CPhysical::AddToMovingList "
		    "at 0x%08X - breaking street objects is shared, knocked-over ones "
		    "lie down in the same place on every screen, and explosions stay "
		    "quiet because they already agree",
		    obj::CObject__ObjectDamage, obj::CWorld__TriggerExplosion,
		    CPhysical__AddToMovingList);
		return true;
	}

	if (damage && !moving) {
		Log("object: hooked CObject::ObjectDamage but NOT "
		    "CPhysical::AddToMovingList at 0x%08X - a lamp post will show its "
		    "damaged model on both screens and may still be standing on one "
		    "of them, which is where this feature started",
		    CPhysical__AddToMovingList);
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
	g_moving.Remove();
	g_insideExplosion = 0;
	g_applying        = false;
	g_watchedCount    = 0;
	for (Watched &w : g_watched)
		w = Watched{};
}

bool ObjectHooksInstalled() { return g_damage.IsInstalled(); }

void AddObjectsToBridge(WorldBridge &bridge) {
	bridge.ObjectBroken  = &OnObjectBrokenElsewhere;
	bridge.ObjectSettled = &OnObjectSettledElsewhere;
}

}  // namespace coopiii::game
