#include "heli.h"

#include "addresses.h"
#include "combat.h"
#include "pedanim.h"
#include "vehicle.h"
#include "../client.h"
#include "../hook/hook.h"
#include "../log.h"
#include "../quat.h"

#include <cmath>

namespace coopiii::game {

// Same number as interp.h's ENGINE_STEPS_PER_SECOND, read off the same CTimer
// code. The helicopter converts on the wire, everything else converts where it
// goes into an interpolation buffer (MoveSpeedToMps).
static_assert(HELI_MOVE_SPEED_TO_MPS == ENGINE_STEPS_PER_SECOND,
              "one engine step is 1/50 s for helicopters and cars alike");

namespace {

using ThisFn = void(__thiscall *)(void *);

Detour g_update;      // CHeli::UpdateHelis
Detour g_process;     // CHeli::ProcessControl
Detour g_bullet;      // CHeli::TestBulletCollision
Detour g_rocket;      // CHeli::TestRocketCollision
Detour g_preRender;   // CHeli::SpecialHeliPreRender
Detour g_crime;       // CWanted::RegisterCrime_Immediately
Detour g_planeRocket; // CPlane::TestRocketCollision, a lead (heli.h)

void *PlayerPed() { return Func<void *(__cdecl *)()>(FindPlayerPed)(); }

int32_t VehicleRef(void *vehicle) {
	return Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(vehicle);
}

void *VehicleFromRef(int32_t ref) {
	if (ref < 0)
		return nullptr;
	return Func<void *(__cdecl *)(int32_t)>(CPools__GetVehicle)(ref);
}

void *HeliInSlot(int slot) {
	return Global<void *>(CHeli__pHelis + static_cast<uintptr_t>(slot) * 4);
}

bool IsHeli(void *entity) {
	return entity && Field<uintptr_t>(entity, offs::VTABLE) == CHeli__vtable;
}

uint32_t Now() { return Global<uint32_t>(CTimer__m_snTimeInMilliseconds); }

void SetBit(void *object, size_t offset, uint8_t mask, bool on) {
	uint8_t &b = Field<uint8_t>(object, offset);
	b = static_cast<uint8_t>(on ? (b | mask) : (b & ~mask));
}

bool GetBit(void *object, size_t offset, uint8_t mask) {
	return (Field<uint8_t>(object, offset) & mask) != 0;
}

// ---- our own helicopters ----------------------------------------------------

// Who brought down the helicopter in each police slot, when it was somebody
// else. Tied to the object: a new helicopter in the slot starts with nobody.
struct OwnSlot {
	void   *heli   = nullptr;
	uint8_t credit = INVALID_PLAYER;
};
OwnSlot g_own[HELI_POLICE_SLOTS];

uint8_t &CreditFor(int slot, void *heli) {
	if (g_own[slot].heli != heli)
		g_own[slot] = OwnSlot{heli, INVALID_PLAYER};
	return g_own[slot].credit;
}

constexpr uint8_t MAX_GONE = 8;
OwnHeliGone g_gone[MAX_GONE];
uint8_t     g_goneCount = 0;

void PushGone(const OwnHeliGone &g) {
	if (g_goneCount == MAX_GONE) {
		for (uint8_t i = 1; i < MAX_GONE; ++i)
			g_gone[i - 1] = g_gone[i];
		--g_goneCount;
	}
	g_gone[g_goneCount++] = g;
}

// Slots whose explosion is running on somebody else's credit, for the length
// of one UpdateHelis call. Bit n is slot n.
uint8_t g_withheldCrimeSlots = 0;

HeliRewards ReadRewards() {
	const uintptr_t player =
	    CWorld__Players + Global<uint8_t>(CWorld__PlayerInFocus) * offs::PLAYERINFO_STRIDE;
	HeliRewards r;
	r.money          = Global<int32_t>(player + offs::PLAYERINFO_MONEY);
	r.helisDestroyed = Global<int32_t>(CStats__HelisDestroyed);
	r.peopleKilled   = Global<int32_t>(CStats__PeopleKilledByPlayer);
	r.copsKilled     = Global<int32_t>(CStats__CopsKilled);
	return r;
}

void WriteRewards(const HeliRewards &r) {
	const uintptr_t player =
	    CWorld__Players + Global<uint8_t>(CWorld__PlayerInFocus) * offs::PLAYERINFO_STRIDE;
	Global<int32_t>(player + offs::PLAYERINFO_MONEY) = r.money;
	Global<int32_t>(CStats__HelisDestroyed)         = r.helisDestroyed;
	Global<int32_t>(CStats__PeopleKilledByPlayer)   = r.peopleKilled;
	Global<int32_t>(CStats__CopsKilled)             = r.copsKilled;
}

bool g_saidWithheld      = false;
bool g_saidPlaneRocket   = false;
bool g_saidWithholdMiss  = false;
bool g_saidReplayShield  = false;
bool g_saidCredited      = false;
bool g_saidReplicaHit    = false;

// ---- replicas ---------------------------------------------------------------

struct Replica {
	int32_t          handle = -1;
	void            *heli   = nullptr;
	uint8_t          owner  = INVALID_PLAYER;
	uint8_t          slot   = 0;
	uint16_t         serial = 0;
	VehicleTransform at{};
	HeliLook         look{};
};
Replica g_replicas[MAX_REMOTE_HELIS];

// The live replica for this object, or null. Checked against the pool, so a
// slot the engine has freed and reused is never taken for ours.
Replica *ReplicaFor(void *heli) {
	if (!heli)
		return nullptr;
	for (Replica &r : g_replicas)
		if (r.handle >= 0 && r.heli == heli) {
			if (VehicleFromRef(r.handle) == heli)
				return &r;
			r = Replica{};
			return nullptr;
		}
	return nullptr;
}

Replica *ReplicaByHandle(int32_t handle) {
	if (handle < 0)
		return nullptr;
	for (Replica &r : g_replicas)
		if (r.handle == handle) {
			void *const h = VehicleFromRef(handle);
			if (h && h == r.heli && IsHeli(h))
				return &r;
			r = Replica{};
			return nullptr;
		}
	return nullptr;
}

constexpr uint8_t MAX_HITS = 32;
LocalHeliHit g_hits[MAX_HITS];
uint8_t      g_hitCount = 0;

void PushHit(const Replica &r, uint8_t kind, uint16_t damage) {
	if (g_hitCount == MAX_HITS)
		return;   // an uzi at point blank; the next frame drains it
	LocalHeliHit &h = g_hits[g_hitCount++];
	h.owner  = r.owner;
	h.slot   = r.slot;
	h.serial = r.serial;
	h.kind   = kind;
	h.damage = damage;
	if (!g_saidReplicaHit) {
		g_saidReplicaHit = true;
		Log("heli: we hit player %u's helicopter %u with a %s; the hit goes to "
		    "them",
		    r.owner, r.serial, kind == HELI_HIT_ROCKET ? "rocket" : "bullet");
	}
}

// Where a replica goes, the way PlaceVehicle in vehicle.cpp writes it. The
// matrix only; ProcessControl's own tail does the rest.
void WriteTransform(void *heli, const VehicleTransform &at) {
	Vec3 right, forward, up;
	AxesFromQuat(at.rot, right, forward, up);
	WriteVec3(heli, offs::MATRIX_RIGHT, right);
	WriteVec3(heli, offs::MATRIX_FWD, forward);
	WriteVec3(heli, offs::MATRIX_UP, up);
	float *const p = &Field<float>(heli, offs::POSITION);
	p[0]           = ClampToWorld(at.pos.x);
	p[1]           = ClampToWorld(at.pos.y);
	FiniteOr(at.pos.z, p[2], p[2]);
}

float Finite(float v, float fallback) {
	float out = fallback;
	FiniteOr(v, fallback, out);
	return out;
}

// ---- the detours ------------------------------------------------------------

using UpdateFn = void(__cdecl *)();

void __cdecl HookedUpdateHelis() {
	struct Before {
		void   *heli     = nullptr;
		int32_t handle   = -1;
		bool    explodes = false;
		uint8_t credit   = INVALID_PLAYER;
		Vec3    pos{};
	};
	Before  b[HELI_POLICE_SLOTS];
	int32_t exploding = 0, notOurs = 0;
	uint8_t withheld  = 0;
	const uint32_t now = Now();

	for (int slot = 0; slot < HELI_POLICE_SLOTS; ++slot) {
		void *const heli = HeliInSlot(slot);
		if (!IsHeli(heli))
			continue;
		b[slot].heli     = heli;
		b[slot].handle   = VehicleRef(heli);
		b[slot].pos      = ReadVec3(heli, offs::POSITION);
		b[slot].credit   = CreditFor(slot, heli);
		b[slot].explodes = HeliExplodesThisUpdate(
		    Field<uint8_t>(heli, offs::HELI_STATUS), now,
		    Field<uint32_t>(heli, offs::HELI_EXPLOSION_TIMER));
		if (b[slot].explodes) {
			++exploding;
			if (b[slot].credit != INVALID_PLAYER) {
				++notOurs;
				withheld = static_cast<uint8_t>(withheld | (1u << slot));
			}
		}
	}

	// The script's and Catalina's helicopters pay out of the same branch, so
	// they are counted for the arithmetic below though nothing else here
	// touches them.
	for (int slot = HELI_POLICE_SLOTS; slot < HELI_SLOTS; ++slot) {
		void *const heli = HeliInSlot(slot);
		if (IsHeli(heli) &&
		    HeliExplodesThisUpdate(Field<uint8_t>(heli, offs::HELI_STATUS), now,
		                           Field<uint32_t>(heli, offs::HELI_EXPLOSION_TIMER)))
			++exploding;
	}

	const HeliRewards before = notOurs ? ReadRewards() : HeliRewards{};
	g_withheldCrimeSlots     = withheld;
	g_update.Original<UpdateFn>()();
	g_withheldCrimeSlots     = 0;

	bool kept = false;
	if (notOurs) {
		HeliRewards fixed;
		if (WithholdHeliRewards(before, ReadRewards(), exploding, notOurs, fixed)) {
			WriteRewards(fixed);
			if (!g_saidWithheld) {
				g_saidWithheld = true;
				Log("heli: somebody else shot our helicopter down, so the $250, the "
				    "crime and the statistics our engine gives for it were taken "
				    "back. Their machine records the crime and the statistics");
			}
		} else {
			kept = true;
			if (!g_saidWithholdMiss) {
				g_saidWithholdMiss = true;
				Log("heli: a helicopter somebody else shot down exploded here, and "
				    "the money or the statistics moved by something other than "
				    "UpdateHelis's reward in the same call; left as they are, and "
				    "the shooter is told so it does not pay itself as well");
			}
		}
	}

	for (int slot = 0; slot < HELI_POLICE_SLOTS; ++slot) {
		if (!b[slot].heli || HeliInSlot(slot) == b[slot].heli)
			continue;
		OwnHeliGone g;
		g.slot           = static_cast<uint8_t>(slot);
		g.handle         = b[slot].handle;
		g.reason         = b[slot].explodes ? HELI_GONE_SHOT_DOWN : HELI_GONE_FLEW_AWAY;
		g.creditPlayerId = b[slot].explodes ? b[slot].credit : INVALID_PLAYER;
		g.ownerKept      = kept && g.creditPlayerId != INVALID_PLAYER;
		g.pos            = b[slot].pos;
		PushGone(g);
		g_own[slot] = OwnSlot{};
	}
}

using ProcessThisFn = void(__thiscall *)(void *);

// What CHeli::ProcessControl does for a replica: none of the AI, the
// transform the owner streamed, and then the engine's own last four lines
// (0x00549827..0x00549842) so the replica is filed, drawn and treated by
// CWorld::Process exactly like the owner's helicopter.
void ReplicaProcessControl(void *heli, const Replica &r) {
	WriteTransform(heli, r.at);

	const float k = 1.0f / HELI_MOVE_SPEED_TO_MPS;
	WriteVec3(heli, offs::MOVE_SPEED,
	          Vec3{Finite(r.look.velocity.x, 0.0f) * k, Finite(r.look.velocity.y, 0.0f) * k,
	               Finite(r.look.velocity.z, 0.0f) * k});
	WriteVec3(heli, offs::TURN_SPEED, Vec3{0.0f, 0.0f, 0.0f});
	Field<float>(heli, offs::HELI_SEARCHLIGHT_X) = Finite(r.look.searchLightX, 0.0f);
	Field<float>(heli, offs::HELI_SEARCHLIGHT_Y) = Finite(r.look.searchLightY, 0.0f);
	float intensity = Finite(r.look.searchLightIntensity, 0.0f);
	intensity       = intensity < 0.0f ? 0.0f : intensity > 1.0f ? 1.0f : intensity;
	Field<float>(heli, offs::HELI_SEARCHLIGHT_INTENSITY) = intensity;
	if (IsKnownHeliStatus(r.look.status))
		Field<uint8_t>(heli, offs::HELI_STATUS) = r.look.status;

	Func<ThisFn>(CPhysical__RemoveAndAdd)(heli);
	SetBit(heli, offs::ENTITY_FLAGS_A, offs::ENTITY_IS_IN_SAFE_POSITION, true);
	Func<ThisFn>(CMatrix__UpdateRW)(reinterpret_cast<uint8_t *>(heli) + offs::MATRIX);
	Func<ThisFn>(CEntity__UpdateRwFrame)(heli);
}

void __fastcall HookedHeliProcessControl(void *self, void * /*edx*/) {
	if (const Replica *r = ReplicaFor(self)) {
		ReplicaProcessControl(self, *r);
		return;
	}
	g_process.Original<ProcessThisFn>()(self);
}

// The two police slots, made proof against something for the length of one
// call, and put back exactly as they were.
struct Shield {
	void   *heli[HELI_POLICE_SLOTS] = {};
	size_t  offset = 0;
	uint8_t mask   = 0;

	Shield(size_t off, uint8_t m) : offset(off), mask(m) {
		for (int slot = 0; slot < HELI_POLICE_SLOTS; ++slot) {
			void *const h = HeliInSlot(slot);
			if (IsHeli(h) && !GetBit(h, offset, mask)) {
				SetBit(h, offset, mask, true);
				heli[slot] = h;
			}
		}
	}
	~Shield() {
		for (void *h : heli)
			if (h)
				SetBit(h, offset, mask, false);
	}
	bool Any() const { return heli[0] || heli[1]; }
};

using BulletFn = bool(__cdecl *)(float *, float *, float *, int32_t);

bool __cdecl HookedTestBulletCollision(float *line0, float *line1, float *bulletPos,
                                       int32_t damage) {
	bool hit;
	if (ReplayingRemoteShot()) {
		// Somebody else's bullet, drawn here. It must not hurt our own
		// helicopter: their machine is already telling us about it on
		// C_HeliHit, and counting both is every bullet twice.
		Shield shield(offs::ENTITY_FLAGS_C, offs::ENTITY_BULLET_PROOF);
		if (shield.Any() && !g_saidReplayShield) {
			g_saidReplayShield = true;
			Log("heli: a replayed shot passed our helicopter; it only counts when "
			    "the shooter's C_HeliHit arrives");
		}
		hit = g_bullet.Original<BulletFn>()(line0, line1, bulletPos, damage);
	} else {
		hit = g_bullet.Original<BulletFn>()(line0, line1, bulletPos, damage);
	}

	// Our own bullet against somebody else's helicopter. The engine's test,
	// on the one set of helicopters the engine doesn't know about.
	if (!ReplayingRemoteShot() && LocalPlayerFiring() && line0 && line1 && bulletPos) {
		using DistFn = float(__cdecl *)(const float *, const float *, const float *);
		for (Replica &r : g_replicas) {
			if (r.handle < 0)
				continue;
			void *const heli = VehicleFromRef(r.handle);
			if (!heli || heli != r.heli || GetBit(heli, offs::ENTITY_FLAGS_C,
			                                      offs::ENTITY_BULLET_PROOF))
				continue;
			const float *pos = &Field<float>(heli, offs::POSITION);
			if (!(Func<DistFn>(CCollision__DistToLine)(line0, line1, pos) <
			      HELI_BULLET_RADIUS))
				continue;
			const float dx = pos[0] - line0[0], dy = pos[1] - line0[1], dz = pos[2] - line0[2];
			const float lx = line1[0] - line0[0], ly = line1[1] - line0[1], lz = line1[2] - line0[2];
			const float toHeli = std::sqrt(dx * dx + dy * dy + dz * dz);
			const float length = std::sqrt(lx * lx + ly * ly + lz * lz);
			if (length > 0.0f) {
				const float along = (toHeli - HELI_BULLET_RADIUS > 1.0f
				                         ? toHeli - HELI_BULLET_RADIUS
				                         : 1.0f) / length;
				bulletPos[0] = line0[0] + lx * along;
				bulletPos[1] = line0[1] + ly * along;
				bulletPos[2] = line0[2] + lz * along;
			}
			PushHit(r, HELI_HIT_BULLET, static_cast<uint16_t>(damage > 0 ? damage : 0));
			hit = true;
		}
	}
	return hit;
}

// The rocket CProjectileInfo::Update is asking about. It hands over a stack
// copy of the projectile's position (0x0055B8DD, 0x0055B9B7) and nothing
// else, so the rocket is found by that position.
const void *RocketAt(const float *pos) {
	if (!pos)
		return nullptr;
	for (int i = 0; i < NUM_PROJECTILES; ++i) {
		const uintptr_t info = gaProjectileInfo + static_cast<uintptr_t>(i) * SIZEOF_PROJECTILEINFO;
		if (!Global<bool>(info + PROJINFO_IN_USE) ||
		    Global<int32_t>(info + PROJINFO_WEAPON_TYPE) != 8)
			continue;
		void *const obj = Global<void *>(CProjectileInfo__ms_apProjectile +
		                                 static_cast<uintptr_t>(i) * 4);
		if (!obj)
			continue;
		const float *p = &Field<float>(obj, offs::POSITION);
		if (p[0] == pos[0] && p[1] == pos[1] && p[2] == pos[2])
			return reinterpret_cast<const void *>(info);
	}
	return nullptr;
}

using RocketFn = bool(__cdecl *)(float *);

bool __cdecl HookedTestRocketCollision(float *pos) {
	const void *info    = RocketAt(pos);
	const bool  remote  = info && IsRemotePlayersProjectile(info);
	void *const player  = PlayerPed();
	const bool  ours    = info && !remote && player &&
	                  Global<void *>(reinterpret_cast<uintptr_t>(info) +
	                                 PROJINFO_SOURCE) == player;

	bool hit;
	if (remote) {
		Shield shield(offs::ENTITY_FLAGS_B, offs::ENTITY_EXPLOSION_PROOF);
		hit = g_rocket.Original<RocketFn>()(pos);
	} else {
		hit = g_rocket.Original<RocketFn>()(pos);
	}

	if (ours && pos) {
		for (Replica &r : g_replicas) {
			if (r.handle < 0)
				continue;
			void *const heli = VehicleFromRef(r.handle);
			if (!heli || heli != r.heli ||
			    GetBit(heli, offs::ENTITY_FLAGS_B, offs::ENTITY_EXPLOSION_PROOF))
				continue;
			const float *p = &Field<float>(heli, offs::POSITION);
			const float dx = pos[0] - p[0], dy = pos[1] - p[1], dz = pos[2] - p[2];
			if (!(dx * dx + dy * dy + dz * dz < HELI_ROCKET_RADIUS_SQ))
				continue;
			PushHit(r, HELI_HIT_ROCKET, 0);
			hit = true;
		}
	}
	return hit;
}

// Somebody else's rocket passes a plane by here: the machine that fired it
// decides whether it hit, and its explosion is what ends our copy.
bool __cdecl HookedPlaneTestRocketCollision(float *pos) {
	const void *info = RocketAt(pos);
	if (info && IsRemotePlayersProjectile(info)) {
		if (!g_saidPlaneRocket) {
			g_saidPlaneRocket = true;
			Log("heli: refused our copy of somebody else's rocket a plane here - its "
			    "own machine decides whether it hit, so no crash and no stars here");
		}
		return false;
	}
	return g_planeRocket.Original<RocketFn>()(pos);
}

// Both of CProjectileInfo::Update's calls to the helicopter's rocket test,
// with a one-argument call to the lead beside each.
bool PlaneRocketTestIsWhereTheLeadSays() {
	for (const uintptr_t site : HELI_ROCKET_CALL_SITES) {
		if (!RelCallAt(Ptr<uint8_t>(site), site, CHeli__TestRocketCollision))
			return false;
		const uintptr_t from = site - PLANE_CALL_WINDOW;
		if (!CdeclCallIn(Ptr<uint8_t>(from), from, 2 * PLANE_CALL_WINDOW + 5,
		                 PLANE_ROCKET_TEST_LEAD))
			return false;
	}
	return true;
}

void InstallPlaneRocketHook() {
	if (g_planeRocket.IsInstalled())
		return;
	if (!PlaneRocketTestIsWhereTheLeadSays()) {
		Log("heli: CProjectileInfo::Update does not call 0x%08X beside the helicopter's "
		    "rocket test, so it is not taken for CPlane::TestRocketCollision; somebody "
		    "else's rocket can still crash a plane here and give us the stars",
		    static_cast<unsigned>(PLANE_ROCKET_TEST_LEAD));
		return;
	}
	if (g_planeRocket.Install("CPlane::TestRocketCollision",
	                          reinterpret_cast<void *>(PLANE_ROCKET_TEST_LEAD),
	                          reinterpret_cast<void *>(&HookedPlaneTestRocketCollision)))
		Log("heli: hooked CPlane::TestRocketCollision at 0x%08X, found beside both of "
		    "the helicopter's rocket tests", static_cast<unsigned>(PLANE_ROCKET_TEST_LEAD));
	else
		Log("heli: FAILED to hook CPlane::TestRocketCollision at 0x%08X; somebody "
		    "else's rocket can still crash a plane here and give us the stars",
		    static_cast<unsigned>(PLANE_ROCKET_TEST_LEAD));
}

using PreRenderFn = void(__cdecl *)();

void __cdecl HookedSpecialHeliPreRender() {
	g_preRender.Original<PreRenderFn>()();
	// PreRenderAlways calls FindPlayerCoors, which does not check for a
	// player ped (addresses.h). The engine's own helicopters never exist
	// without one; a replica can, for a frame, during a load.
	if (!PlayerPed())
		return;
	for (Replica &r : g_replicas) {
		if (r.handle < 0)
			continue;
		void *const heli = VehicleFromRef(r.handle);
		if (heli && heli == r.heli && IsHeli(heli))
			Func<ThisFn>(CHeli__PreRenderAlways)(heli);
	}
}

using CrimeThisFn = void(__thiscall *)(void *, int32_t, const float *, uint32_t, uint32_t);

void __fastcall HookedRegisterCrime(void *self, void * /*edx*/, int32_t crime,
                                    const float *pos, uint32_t id, uint32_t dontCare) {
	if (g_withheldCrimeSlots && IsWithheldHeliCrime(crime, id, g_withheldCrimeSlots))
		return;
	g_crime.Original<CrimeThisFn>()(self, crime, pos, id, dontCare);
}

// ---- the bridge -------------------------------------------------------------

uint8_t SampleOwnHelis(OwnHeliSample *out, uint8_t max) {
	uint8_t n = 0;
	for (int slot = 0; slot < HELI_POLICE_SLOTS && n < max; ++slot) {
		void *const heli = HeliInSlot(slot);
		if (!IsHeli(heli))
			continue;
		(void)CreditFor(slot, heli);   // a new helicopter starts with nobody

		OwnHeliSample &s = out[n++];
		s        = OwnHeliSample{};
		s.handle = VehicleRef(heli);
		HeliStateBody &b = s.body;
		b.slot   = static_cast<uint8_t>(slot);
		b.status = Field<uint8_t>(heli, offs::HELI_STATUS);
		b.flags  = GetBit(heli, offs::ENTITY_FLAGS_B, offs::ENTITY_RENDER_SCORCHED)
		               ? HELI_FLAG_TAIL_BLOWN
		               : 0;
		b.pos    = ReadVec3(heli, offs::POSITION);
		b.rot    = QuatFromAxes(ReadVec3(heli, offs::MATRIX_RIGHT),
		                        ReadVec3(heli, offs::MATRIX_FWD),
		                        ReadVec3(heli, offs::MATRIX_UP));
		const Vec3 v = ReadVec3(heli, offs::MOVE_SPEED);
		b.velocity = Vec3{v.x * HELI_MOVE_SPEED_TO_MPS, v.y * HELI_MOVE_SPEED_TO_MPS,
		                  v.z * HELI_MOVE_SPEED_TO_MPS};
		b.searchLightX         = Field<float>(heli, offs::HELI_SEARCHLIGHT_X);
		b.searchLightY         = Field<float>(heli, offs::HELI_SEARCHLIGHT_Y);
		b.searchLightIntensity = Field<float>(heli, offs::HELI_SEARCHLIGHT_INTENSITY);
	}
	return n;
}

uint8_t DrainOwnHeliGone(OwnHeliGone *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && n < g_goneCount) {
		out[n] = g_gone[n];
		++n;
	}
	for (uint8_t i = n; i < g_goneCount; ++i)
		g_gone[i - n] = g_gone[i];
	g_goneCount = static_cast<uint8_t>(g_goneCount - n);
	return n;
}

bool ApplyHeliHit(uint8_t slot, int32_t handle, uint8_t attackerId, const HeliHitBody &hit) {
	if (!IsPoliceHeliSlot(slot))
		return false;
	void *const heli = HeliInSlot(slot);
	if (!IsHeli(heli) || VehicleRef(heli) != handle)
		return false;

	HeliDamageState s;
	s.status         = Field<uint8_t>(heli, offs::HELI_STATUS);
	s.heliType       = Field<uint8_t>(heli, offs::HELI_TYPE);
	s.bulletProof    = GetBit(heli, offs::ENTITY_FLAGS_C, offs::ENTITY_BULLET_PROOF);
	s.explosionProof = GetBit(heli, offs::ENTITY_FLAGS_B, offs::ENTITY_EXPLOSION_PROOF);
	s.bulletDamage   = Field<uint32_t>(heli, offs::HELI_BULLET_DAMAGE);
	s.explosionTimer = Field<uint32_t>(heli, offs::HELI_EXPLOSION_TIMER);
	s.angularSpeed   = Field<float>(heli, offs::HELI_ANGULAR_SPEED);

	const uint8_t statusBefore = s.status;
	bool randomLow = false;
	if (HeliHitBringsDown(s, hit.kind, hit.damage)) {
		const int32_t r = Func<int32_t(__cdecl *)()>(CGeneral__GetRandomNumber)();
		randomLow       = static_cast<uint16_t>(r) < 0x3FFF;
	}
	if (!ApplyHeliHitRule(s, hit.kind, hit.damage, Now(), randomLow))
		return false;

	Field<uint8_t>(heli, offs::HELI_STATUS)           = s.status;
	Field<uint32_t>(heli, offs::HELI_BULLET_DAMAGE)   = s.bulletDamage;
	Field<uint32_t>(heli, offs::HELI_EXPLOSION_TIMER) = s.explosionTimer;
	Field<float>(heli, offs::HELI_ANGULAR_SPEED)      = s.angularSpeed;

	uint8_t &credit = CreditFor(slot, heli);
	const uint8_t was = credit;
	credit = HeliCreditAfterHit(credit, statusBefore, s.status, attackerId);
	if (credit != was && !g_saidCredited) {
		g_saidCredited = true;
		Log("heli: player %u's %s brought our helicopter down; it explodes on our "
		    "engine in ten seconds and the credit is theirs",
		    attackerId, hit.kind == HELI_HIT_ROCKET ? "rocket" : "gunfire");
	}
	return true;
}

void ResetHeliSession() {
	for (OwnSlot &o : g_own)
		o = OwnSlot{};
	g_goneCount = 0;
	g_hitCount  = 0;
}

uint8_t DrainLocalHeliHits(LocalHeliHit *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && n < g_hitCount) {
		out[n] = g_hits[n];
		++n;
	}
	for (uint8_t i = n; i < g_hitCount; ++i)
		g_hits[i - n] = g_hits[i];
	g_hitCount = static_cast<uint8_t>(g_hitCount - n);
	return n;
}

// Our hit brought somebody else's helicopter down. What UpdateHelis does for
// the local player after an explosion, minus what the owner's machine does
// for itself (the blast, the debris, CDarkel) and minus the $250.
void CreditHeliShootDown(uint8_t slot, const Vec3 & /*where*/, bool statistics) {
	void *const ped = PlayerPed();
	if (!ped || !IsPoliceHeliSlot(slot))
		return;
	void *const wanted = Field<void *>(ped, offs::PLAYER_PED_WANTED);

	if (statistics) {
		Global<int32_t>(CStats__HelisDestroyed) += HELI_REWARD_EACH.helisDestroyed;
		Global<int32_t>(CStats__PeopleKilledByPlayer) += HELI_REWARD_EACH.peopleKilled;
		Global<int32_t>(CStats__CopsKilled) += HELI_REWARD_EACH.copsKilled;
	}

	// The engine reports the crime at the player's own position
	// (0x0054A0FB..0x0054A104), so that is where it goes here too.
	if (wanted) {
		const Vec3 at = ReadVec3(ped, offs::POSITION);
		const float p[3] = {at.x, at.y, at.z};
		Func<CrimeThisFn>(CWanted__RegisterCrime_Immediately)(
		    wanted, CRIME_SHOOT_HELI, p, HELI_CRIME_ID_BASE + slot, 0);
	}
}

// The $250 the owner's engine took back (WithholdHeliRewards), when the money
// rule pays it here instead. Into PlayerInFocus's m_nMoney, where UpdateHelis
// puts it (0x0054A0DF).
void PayHeliShootDown() {
	if (!PlayerPed())
		return;
	const uintptr_t player =
	    CWorld__Players + Global<uint8_t>(CWorld__PlayerInFocus) * offs::PLAYERINFO_STRIDE;
	Global<int32_t>(player + offs::PLAYERINFO_MONEY) += HELI_REWARD_EACH.money;
}

bool RequestHeliModel() {
	if (HasModelLoaded(MI_CHOPPER))
		return true;
	Func<void(__cdecl *)(int32_t, int32_t)>(CStreaming__RequestModel)(MI_CHOPPER, 0);
	return false;
}

bool SpawnHeliReplica(RemoteHeli &row) {
	using NewFn  = void *(__cdecl *)(size_t);
	using CtorFn = void(__thiscall *)(void *, int32_t, uint8_t);
	using AddFn  = void(__cdecl *)(void *);

	// Not without a player ped: the replica's own PreRender asks
	// FindPlayerCoors, which has no null check.
	if (!g_process.IsInstalled() || !HasModelLoaded(MI_CHOPPER) || !PlayerPed())
		return false;

	Replica *slot = nullptr;
	for (Replica &r : g_replicas)
		if (r.handle < 0 || !VehicleFromRef(r.handle)) {
			slot = &r;
			break;
		}
	if (!slot)
		return false;

	void *const mem = Func<NewFn>(CVehicle__operator_new)(SIZEOF_HELI);
	if (!mem)
		return false;   // the vehicle pool is full
	// GenerateHeli's own two arguments: MI_CHOPPER, PERMANENT_VEHICLE.
	Func<CtorFn>(CHeli__CHeli)(mem, MI_CHOPPER,
	                           static_cast<uint8_t>(VEHICLE_CREATED_BY_PERMANENT));
	if (!IsHeli(mem)) {
		Log("heli: the CHeli constructor did not complete (vtable %08X)",
		    static_cast<unsigned>(Field<uintptr_t>(mem, 0)));
		return false;
	}

	WriteTransform(mem, row.last);
	Func<ThisFn>(CMatrix__UpdateRW)(reinterpret_cast<uint8_t *>(mem) + offs::MATRIX);
	Func<ThisFn>(CEntity__UpdateRwFrame)(mem);

	// The rest of GenerateHeli's registration, in its order, less the one
	// line that matters most: it is never put in pHelis.
	uint8_t &status = Field<uint8_t>(mem, offs::ENTITY_FLAGS);
	status = static_cast<uint8_t>((status & 0x07u) |
	                              (ENTITY_STATUS_ABANDONED << ENTITY_STATUS_SHIFT));
	SetBit(mem, offs::VEH_FLAGS_A, offs::VEH_IS_LOCKED, true);
	Field<uint8_t>(mem, offs::HELI_ID)   = row.slot;
	Field<uint8_t>(mem, offs::HELI_TYPE) = HELI_TYPE_RANDOM;

	if (Field<void *>(mem, offs::MOVING_LIST_NODE) != nullptr)
		Func<ThisFn>(CPhysical__RemoveFromMovingList)(mem);
	Func<AddFn>(CWorld__Add)(mem);

	*slot        = Replica{};
	slot->handle = VehicleRef(mem);
	slot->heli   = mem;
	slot->owner  = row.owner;
	slot->slot   = row.slot;
	slot->serial = row.serial;
	slot->at     = row.last;
	slot->look   = row.look;

	row.poolHandle = slot->handle;
	return true;
}

void DespawnHeliReplica(RemoteHeli &row) {
	using RemoveFn = void(__cdecl *)(void *);
	Replica *const r = ReplicaByHandle(row.poolHandle);
	row.poolHandle   = -1;
	if (!r)
		return;
	void *const heli = r->heli;
	*r = Replica{};

	// The vehicles' teardown (vehicle.cpp, DespawnRemoteVehicle): off the
	// moving list by hand first, because CWorld::Remove won't do it for an
	// entity that has gone static, then Remove, the references, and the
	// deleting destructor through the object's own vtable.
	Func<ThisFn>(CPhysical__RemoveFromMovingList)(heli);
	Func<RemoveFn>(CWorld__Remove)(heli);
	Func<RemoveFn>(CWorld__RemoveReferencesToDeletedObject)(heli);
	using DtorFn = void *(__thiscall *)(void *, uint8_t);
	Func<DtorFn>(*reinterpret_cast<uintptr_t *>(Field<void *>(heli, 0)))(heli, 1);
}

bool PoseHeliReplica(RemoteHeli &row, const VehicleTransform &at, const HeliLook &look) {
	Replica *const r = ReplicaByHandle(row.poolHandle);
	if (!r)
		return false;
	r->at   = at;
	r->look = look;
	return true;
}

using ExplodeFn = bool(__cdecl *)(void *, void *, int, const float *, uint32_t);
using ComponentFn = void *(__thiscall *)(void *, int32_t);

// UpdateHelis's first half, on the replica: 0x0054A244..0x0054A349 less the
// camera shake.
void BlowTailOffHeliReplica(RemoteHeli &row) {
	Replica *const r = ReplicaByHandle(row.poolHandle);
	if (!r)
		return;
	VehicleBlastReplayScope replay;
	void *const heli = r->heli;
	Func<ComponentFn>(CHeli__SpawnFlyingComponent)(heli, HELI_NODE_BACKROTOR);
	Func<ComponentFn>(CHeli__SpawnFlyingComponent)(heli, HELI_NODE_TAIL);
	SetBit(heli, offs::ENTITY_FLAGS_B, offs::ENTITY_RENDER_SCORCHED, true);
	const Vec3 pos = ReadVec3(heli, offs::POSITION);
	const Vec3 fwd = ReadVec3(heli, offs::MATRIX_FWD);
	const float at[3] = {pos.x - HELI_FIRST_BLAST_BACK * fwd.x,
	                     pos.y - HELI_FIRST_BLAST_BACK * fwd.y,
	                     pos.z - HELI_FIRST_BLAST_BACK * fwd.z};
	Func<ExplodeFn>(CExplosion__AddExplosion)(nullptr, nullptr, EXPLOSION_HELI, at, 0);
}

// And the second, at the place the owner's engine blew it up. The debris
// particles are left out; everything else is the owner's list. The count on
// CDarkel and the reward are not here at all - they were the owner's.
void ExplodeHeliReplica(RemoteHeli &row, const Vec3 &pos) {
	VehicleBlastReplayScope replay;
	float at[3] = {pos.x, pos.y, pos.z};
	if (!FiniteOr(at[0], 0.0f, at[0]) || !FiniteOr(at[1], 0.0f, at[1]) ||
	    !FiniteOr(at[2], 0.0f, at[2]))
		return;

	if (Replica *const r = ReplicaByHandle(row.poolHandle)) {
		void *const heli = r->heli;
		VehicleTransform where = r->at;
		where.pos = pos;
		WriteTransform(heli, where);
		Func<ThisFn>(CMatrix__UpdateRW)(reinterpret_cast<uint8_t *>(heli) + offs::MATRIX);
		Func<ThisFn>(CEntity__UpdateRwFrame)(heli);
		Func<ComponentFn>(CHeli__SpawnFlyingComponent)(heli, HELI_NODE_SKID_LEFT);
		Func<ComponentFn>(CHeli__SpawnFlyingComponent)(heli, HELI_NODE_SKID_RIGHT);
		Func<ComponentFn>(CHeli__SpawnFlyingComponent)(heli, HELI_NODE_TOPROTOR);
	}
	Func<ExplodeFn>(CExplosion__AddExplosion)(nullptr, nullptr, EXPLOSION_HELI, at, 0);
}

template <class F>
bool InstallOne(Detour &d, const char *name, uintptr_t at, F *fn, const char *cost) {
	if (d.IsInstalled())
		return true;
	if (d.Install(name, reinterpret_cast<void *>(at), reinterpret_cast<void *>(fn))) {
		Log("heli: hooked %s at 0x%08X", name, static_cast<unsigned>(at));
		return true;
	}
	Log("heli: FAILED to hook %s at 0x%08X; %s", name, static_cast<unsigned>(at), cost);
	return false;
}

} // namespace

bool InstallHeliHooks() {
	bool ok = true;
	ok &= InstallOne(g_process, "CHeli::ProcessControl", CHeli__ProcessControl,
	                 &HookedHeliProcessControl,
	                 "no replica of anybody else's helicopter is built here");
	ok &= InstallOne(g_update, "CHeli::UpdateHelis", CHeli__UpdateHelis,
	                 &HookedUpdateHelis,
	                 "our helicopter's end is only noticed as it leaving its slot, "
	                 "with no explosion for the others and no credit");
	ok &= InstallOne(g_bullet, "CHeli::TestBulletCollision", CHeli__TestBulletCollision,
	                 &HookedTestBulletCollision,
	                 "bullets can't hit anybody else's helicopter, and replayed "
	                 "ones still hit ours");
	ok &= InstallOne(g_rocket, "CHeli::TestRocketCollision", CHeli__TestRocketCollision,
	                 &HookedTestRocketCollision,
	                 "rockets can't hit anybody else's helicopter, and replayed "
	                 "ones still hit ours");
	ok &= InstallOne(g_preRender, "CHeli::SpecialHeliPreRender",
	                 CHeli__SpecialHeliPreRender, &HookedSpecialHeliPreRender,
	                 "a replica has no searchlight and no tail light");
	ok &= InstallOne(g_crime, "CWanted::RegisterCrime_Immediately",
	                 CWanted__RegisterCrime_Immediately, &HookedRegisterCrime,
	                 "shooting down somebody else's helicopter here puts the crime on "
	                 "the owner as well as on the shooter");
	if (!ok)
		for (const auto &f : HookFailures())
			Log("heli:   %s: %s", f.name.c_str(), f.reason.c_str());
	// Only with the helicopter's rocket test in, for RocketAt and for the
	// projectile table it reads. Not counted in `ok`: its address is a lead.
	if (g_rocket.IsInstalled())
		InstallPlaneRocketHook();
	return ok;
}

void RemoveHeliHooks() {
	for (Detour *d : {&g_planeRocket, &g_crime, &g_preRender, &g_rocket, &g_bullet, &g_update,
	                  &g_process})
		if (d->IsInstalled())
			d->Remove();
	g_withheldCrimeSlots = 0;
}

void AddHeliToBridge(WorldBridge &bridge) {
	HeliBridge &b = bridge.heli;
	b.SampleOwnHelis     = &SampleOwnHelis;
	b.ResetHeliSession   = &ResetHeliSession;
	if (g_update.IsInstalled())
		b.DrainOwnHeliGone = &DrainOwnHeliGone;
	b.ApplyHeliHit       = &ApplyHeliHit;
	if (g_bullet.IsInstalled() || g_rocket.IsInstalled())
		b.DrainLocalHeliHits = &DrainLocalHeliHits;
	b.CreditHeliShootDown = &CreditHeliShootDown;
	b.PayHeliShootDown    = &PayHeliShootDown;
	if (g_process.IsInstalled()) {
		b.RequestHeliModel       = &RequestHeliModel;
		b.SpawnHeliReplica       = &SpawnHeliReplica;
		b.DespawnHeliReplica     = &DespawnHeliReplica;
		b.PoseHeliReplica        = &PoseHeliReplica;
		b.BlowTailOffHeliReplica = &BlowTailOffHeliReplica;
		b.ExplodeHeliReplica     = &ExplodeHeliReplica;
	}
}

} // namespace coopiii::game
