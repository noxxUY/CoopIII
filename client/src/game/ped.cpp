#include "ped.h"

#include "addresses.h"
#include "boat.h"
#include "carstatus.h"
#include "clock.h"
#include "combat.h"
#include "driveby.h"
#include "hook/hook.h"
#include "log.h"
#include "pedaim.h"
#include "pedanim.h"
#include "vehicle.h"


namespace coopiii::game {

namespace {

void *PlayerPed() {
	return Func<void *(__cdecl *)()>(FindPlayerPed)();
}

// ---- the ped's animation list ---------------------------------------------
//
// Animations aren't stored on the CPed itself. They live on its RenderWare
// clump, in a linked list hung off an RW plugin whose data pointer sits at
// the *runtime* offset in ClumpOffset. Reading it is the same three
// instructions CAnimManager::BlendAnimation opens with (addresses.h), and
// that's also where the list's shape got verified.

void *ClumpOf(void *entity) { return Field<void *>(entity, offs::RW_OBJECT); }

// The CAnimBlendClumpData for a clump, or null if the plugin isn't attached
// or the clump has no animation data yet. Uses the same success test
// RpAnimBlendPluginAttach itself uses: `ClumpOffset > 0`.
void *AnimClumpData(void *clump) {
	if (!clump)
		return nullptr;
	const int32_t offset = Global<int32_t>(RpAnimBlend__ClumpOffset);
	if (offset <= 0)
		return nullptr;
	return *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(clump) +
	                                  offset);
}

// Walks the clump's associations, calling fn(assoc) on each. This is
// BlendAnimation's own loop: head is the clump data's first dword, each
// link's next is its own first dword, and an association starts four bytes
// before its link (the class has a vtable, so re3's `offsetof(assoc, link)`
// comes out to 4 in the retail build).
//
// The 64 cap isn't about the engine misbehaving, it's about us. A ped we've
// already lost can still resolve for one more frame, and walking a freed
// list with no bound turns into an infinite loop on the game thread instead
// of a crash - much harder to diagnose.
//
// It used to say "no ped legitimately has anywhere close to 64 associations"
// and treat that as reassurance. It is true and it was the wrong thing to be
// reassured by: the engine breaks at thirteen, not at sixty-four.
// RpAnimBlendClumpUpdateAnimations has room for twelve and bounds-checks
// nothing, so a clump CoopIII has overfilled makes it write over its own
// return address. MAX_REMOTE_ANIM_ASSOCS in pedanim.h is the number that
// matters; this one only stops a runaway walk.
template <class Fn>
void ForEachAnim(void *clump, Fn fn) {
	void *data = AnimClumpData(clump);
	if (!data)
		return;

	constexpr int MAX_ASSOCS = 64;
	void *link = *reinterpret_cast<void **>(
	    reinterpret_cast<uint8_t *>(data) + ANIMCLUMP_LINK_NEXT);
	for (int i = 0; link && i < MAX_ASSOCS; ++i) {
		void *assoc = reinterpret_cast<uint8_t *>(link) - ANIM_LINK_TO_ASSOC;
		fn(assoc);
		link = *reinterpret_cast<void **>(link);
	}
}

struct AnimSample {
	uint16_t id    = ANIM_NONE;
	float    time  = 0.0f;
	float    speed = 1.0f;
	// ASSOC_RUNNING. A weapon animation parked on its ready frame with this
	// clear is a player aiming; the same id and the same phase with it set is
	// a player firing. docs/protocol.md §1.9.4.
	bool     running = false;
};

void ReadAnim(void *assoc, AnimSample &out) {
	const int32_t id = Field<int32_t>(assoc, ANIM_ID);
	if (id < 0 || id >= ANIM_NONE)
		return;   // not a value that survives the uint16 on the wire
	out.id      = static_cast<uint16_t>(id);
	out.time    = Field<float>(assoc, ANIM_CURRENT_TIME);
	out.speed   = Field<float>(assoc, ANIM_SPEED);
	out.running = (Field<int32_t>(assoc, ANIM_FLAGS) & ASSOC_RUNNING) != 0;
}

// The two animations worth sending (docs/protocol.md §1.8): the dominant
// whole-body one, and the dominant ASSOC_PARTIAL overlay. "Dominant" means
// highest blendAmount - same rule the engine's own
// RpAnimBlendClumpGetMainAssociation / GetMainPartialAssociation use.
void ReadDominantAnims(void *clump, AnimSample &base, AnimSample &partial) {
	float bestBase = 0.0f, bestPartial = 0.0f;
	ForEachAnim(clump, [&](void *assoc) {
		const float   blend = Field<float>(assoc, ANIM_BLEND_AMOUNT);
		const int32_t flags = Field<int32_t>(assoc, ANIM_FLAGS);
		if (!(blend > 0.0f))
			return;   // also rejects NaN, which > does and >= would not
		if (flags & ASSOC_PARTIAL) {
			if (blend > bestPartial) {
				bestPartial = blend;
				ReadAnim(assoc, partial);
			}
		} else if (blend > bestBase) {
			bestBase = blend;
			ReadAnim(assoc, base);
		}
	});
}

// ---- aim pitch -------------------------------------------------------------
//
// One detour on CPedIK::PointGunInDirection does both halves. For the local
// player it writes down what the engine asked for, which is what goes out as
// aimYaw/aimPitch (pedaim.h says why the ped's own fields won't do). For a
// remote player it replaces the 0.0f AimGun passes every non-player ped with
// the pitch their owner sent (addresses.h has AimGun's three arms).
//
// Why here and not somewhere earlier in the frame: CWorld::Process runs the
// animation walk over the moving list (UpdateAnimations at 0x004B1B64) and
// only then the ProcessControl walk (vtable +0x20 at 0x004B1B99), which is
// where AimGun sits. The animation walk rebuilds every bone's modelling
// matrix from its key frames, so a bone or a limb angle written from PreFrame
// is gone before AimGun runs, and m_torsoOrient.pitch written directly gets
// dragged 7 degrees back toward zero by MoveLimb before RotateTorso uses it.
// The argument is read inside the ProcessControl walk, after the animation
// has been applied. Nothing re-poses the clump between there and Idle's
// render calls, so what RotateTorso leaves is what gets drawn.

Detour            g_pointGunHook;
ReplicaPitchTable g_replicaPitch;
LocalAimRecord    g_localAim;
bool              g_saidPitchApplied = false;
bool              g_saidPitchSampled = false;

uint32_t FrameNow() { return Global<uint32_t>(CTimer__m_FrameCounter); }

using PointGunHookFn = bool(__fastcall *)(void *, void *, float, float);

bool __fastcall HookedPointGunInDirection(void *ik, void * /*edx*/, float yaw,
                                          float pitch) {
	void *const    ped = ik ? Field<void *>(ik, PEDIK_PED) : nullptr;
	const uint32_t now = FrameNow();

	float wire = 0.0f;
	if (g_replicaPitch.Find(ped, now, wire)) {
		if (!g_saidPitchApplied) {
			g_saidPitchApplied = true;
			Log("ped: first remote aim pitch handed to the engine's IK: %.1f degrees "
			    "(AimGun had %.1f). Positive is down",
			    wire * 57.2957795f, pitch * 57.2957795f);
		}
		pitch = wire;
	} else if (ped && ped == PlayerPed()) {
		g_localAim = LocalAimRecord{ped, yaw, pitch, now, true};
		if (!g_saidPitchSampled) {
			g_saidPitchSampled = true;
			Log("ped: our own aim is now read off PointGunInDirection: yaw %.2f, "
			    "pitch %.1f degrees",
			    yaw, pitch * 57.2957795f);
		}
	}

	return g_pointGunHook.Original<PointGunHookFn>()(ik, nullptr, yaw, pitch);
}

} // namespace

bool LocalPlayerExists() {
	return PlayerPed() != nullptr;
}

// Whether this session reports ammunition honestly. Set from S_Welcome via
// WorldBridge::SetAmmoSync, and read in the three places the engine decides
// how much ammunition a remote ped has: here (sampling), GiveWeaponTo
// (spawning) and combat.cpp (replaying a shot).
//
// Off is not a degraded version of on - it is the behaviour this file has
// always had, and the file-scope default is off so a client that never gets
// a welcome behaves the way it used to.
bool g_ammoSync = false;

// &ped->m_weapons[slot], or null for a slot number that is not a weapon.
//
// CPed::m_weapons is thirteen 0x18-byte elements at +0x35C and the engine's
// own eWeaponType doubles as the index, so the bound is the whole safety
// argument: a slot of 13 or more reads past the array and into
// m_storedWeapon and m_currentWeapon, which are the next members.
static_assert(INVENTORY_SLOTS == offs::NUM_WEAPON_SLOTS,
              "the wire's inventory is CPed::m_weapons, not a parallel idea of one");

void *WeaponSlot(void *ped, uint8_t slot) {
	if (slot >= offs::NUM_WEAPON_SLOTS)
		return nullptr;
	return reinterpret_cast<uint8_t *>(ped) + offs::PED_WEAPONS +
	       static_cast<size_t>(slot) * offs::SIZEOF_WEAPON;
}

// m_nAmmoInClip is an int32 in the engine and a uint16 on the wire, because
// CWeapon::Reload caps it at the weapon's m_nAmountofAmmunition and the
// largest value in stock weapon.dat is 1000. weapon.dat is data the player
// can edit though, so this saturates rather than truncating: a modded clip
// past 65535 is reported as 65535, which is wrong by a number nobody can
// count, where a truncation would be wrong by 65536 and could read as zero.
uint16_t ClipOnWire(int32_t clip) {
	if (clip <= 0)
		return 0;
	if (clip > 0xFFFF)
		return 0xFFFF;
	return static_cast<uint16_t>(clip);
}

// And the total, which CPed::GiveWeapon caps at 99999 (`cmp eax,0x1869F` at
// 0x004CF9E0) - so it fits a uint32 with room to spare and only needs the
// negative guarded.
uint32_t TotalOnWire(int32_t total) {
	return total <= 0 ? 0u : static_cast<uint32_t>(total);
}

bool SampleLocalPlayer(PlayerStateBody &out) {
	void *ped = PlayerPed();
	if (!ped)
		return false;

	out.pos       = ReadVec3(ped, offs::POSITION);
	out.moveSpeed = ReadVec3(ped, offs::MOVE_SPEED);
	out.heading   = Field<float>(ped, offs::PED_ROT_CUR);
	out.health    = Field<float>(ped, offs::PED_HEALTH);
	out.armour    = Field<float>(ped, offs::PED_ARMOUR);

	// Both are 32-bit enums in the engine, but the wire carries them as bytes
	// (docs/protocol.md §1.7). Every PedState and eMoveState value fits in one.
	out.pedState  = static_cast<uint8_t>(Field<uint32_t>(ped, offs::PED_STATE));
	out.moveState = static_cast<uint8_t>(Field<uint32_t>(ped, offs::PED_MOVE_STATE));

	// Walking style. CPlayerPed::ProcessAnimGroups already picked it this
	// frame based on the walk angle and the weapon - we're just reading its
	// decision, not making our own.
	out.animGroup = static_cast<uint8_t>(Field<int32_t>(ped, offs::PED_ANIM_GROUP));

	// Dominant whole-body association plus the dominant partial overlay. The
	// overlay is the part people actually notice: firing, punching and
	// throwing are all partials layered on top of the walk cycle. Skip it and
	// a remote player fires their gun with nothing moving on screen.
	AnimSample base, partial;
	ReadDominantAnims(ClumpOf(ped), base, partial);
	out.animId    = base.id;
	out.animTime  = base.time;
	out.animSpeed = base.speed;
	out.animId2   = partial.id;
	out.animTime2 = partial.time;

	// In a car, a drive-by is the overlay whatever the blend amounts say.
	// Held the way DoDriveByShootings tests it (driveby.h, DriveByHeld).
	if (Field<bool>(ped, offs::PED_IN_VEHICLE)) {
		AnimSample held;
		ForEachAnim(ClumpOf(ped), [&](void *assoc) {
			const int32_t id = Field<int32_t>(assoc, ANIM_ID);
			if (held.id == ANIM_NONE && id >= 0 && IsDriveByAnim(static_cast<uint16_t>(id)) &&
			    DriveByHeld(true, Field<float>(assoc, ANIM_BLEND_DELTA)))
				ReadAnim(assoc, held);
		});
		out.animId2 = DriveByOverlayOnWire(out.animId2, true, held.id);
		if (IsDriveByAnim(held.id)) {
			out.animTime2   = held.time;
			partial.running = held.running;
		}
	}

	// Weapon. m_currentWeapon doubles as an index into m_weapons and as the
	// eWeaponType itself, so the slot gets bounds-checked before use. This is
	// exactly why sampling was held off until the offsets were confirmed.
	out.weapon    = WEAPONTYPE_UNARMED;
	out.ammoClip  = 0;
	out.ammoTotal = 0;
	const uint8_t slot = Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON);
	if (void *const held = WeaponSlot(ped, slot)) {
		const uint32_t type = Field<uint32_t>(held, offs::WEAPON_TYPE);
		if (IsInventoryWeapon(static_cast<uint8_t>(type)))
			out.weapon = static_cast<uint8_t>(type);

		// The count for the gun in our hands, and only that one. The other
		// twelve slots go out on C_PlayerAmmo when they change - see
		// Client::SendLocalAmmo and AmmoSlotBody.
		//
		// Read off the slot the engine is actually indexing rather than off
		// out.weapon: if some other mod has left a slot whose m_eWeaponType
		// does not match its index, the engine still fires out of this one.
		if (g_ammoSync) {
			out.ammoClip  = ClipOnWire(Field<int32_t>(held, offs::WEAPON_AMMO_IN_CLIP));
			out.ammoTotal = TotalOnWire(Field<int32_t>(held, offs::WEAPON_AMMO_TOTAL));
		}
	}

	// Aim. While aiming, m_fLookDirection is the target yaw the engine is
	// easing the torso toward. Sending that instead of the already-eased
	// torso angle means the receiver eases once, not twice. When not aiming
	// we report where the torso really is so a ped mid-restore still looks
	// right. Both world-space (docs/protocol.md §1.8.3).
	const uint8_t flagsA = Field<uint8_t>(ped, offs::PED_FLAGS_A);
	const uint8_t flagsC = Field<uint8_t>(ped, offs::PED_FLAGS_C);
	const bool    aiming = (flagsA & offs::PED_IS_AIMING_GUN) != 0;

	out.flags = 0;
	if (aiming)
		out.flags |= PF_AIMING;
	if (flagsC & offs::PED_IS_SHOOTING)
		out.flags |= PF_FIRING;
	if (partial.running)
		out.flags |= PF_ANIM2_RUNNING;
	// Are we alight? m_pFire is the engine's own answer, kept by CFire itself
	// - StartFire writes it and Extinguish nils it - so there is no second
	// piece of state to go stale. Nothing here looks at the fire's position,
	// its strength or who lit it: an observer is being told that this player
	// is burning, not where the fire is (docs/roadmap.md §5.7).
	if (Field<void *>(ped, PED_FIRE))
		out.flags |= PF_ON_FIRE;

	// While aiming, both angles come from what the last AimGun asked
	// PointGunInDirection for, when the detour above saw it. That is the aim
	// the engine drew, for a pistol's arm as much as a rifle's torso and for
	// a lock-on as much as a free aim. The fields below are the fallback for
	// a session where the detour didn't install, and what goes out when the
	// player isn't aiming.
	if (aiming && LocalAimFresh(g_localAim, ped, FrameNow())) {
		out.aimYaw   = WrapAngle(g_localAim.yaw);
		out.aimPitch = AimPitchFromWire(g_localAim.pitch);
		return true;
	}
	out.aimYaw = WrapAngle(
	    aiming ? Field<float>(ped, offs::PED_LOOK_DIRECTION)
	           : out.heading + Field<float>(ped, offs::PED_IK_TORSO_YAW));
	out.aimPitch = Field<float>(ped, offs::PED_IK_TORSO_PITCH);
	return true;
}

namespace {

void RequestModel(uint16_t modelId) {
	// MI_PLAYER never gets requested here. It's already loaded for as long as
	// there's a local player, and marking it script-owned would fight the
	// game's own reload of slot 0 for the opening's prison clothes (see
	// addresses.h). IsModelReady still gates on it though, and that has the
	// side benefit of not creating remote peds until this machine has a
	// player of its own.
	if (modelId == MI_PLAYER || modelId >= MODELINFO_SIZE)
		return;

	// CStreaming::RequestModel(id, flags). STREAMFLAGS_DEPENDENCY(0x02) |
	// STREAMFLAGS_SCRIPTOWNED(0x08) stops the streamer evicting a model a
	// remote player still needs while out of view.
	Func<void(__cdecl *)(int32_t, int32_t)>(CStreaming__RequestModel)(
	    static_cast<int32_t>(modelId), 0x02 | 0x08);
}

bool IsModelReady(uint16_t modelId) {
	return HasModelLoaded(modelId);
}

// What the local player is wearing, read off the ped rather than assumed.
//
// In stock GTA III this is always MI_PLAYER, so yes, this function is
// basically a constant with extra steps. Read it anyway: "the protagonist
// has one model" is a fact about the script data, not the engine. Nothing
// stops a ped's model index changing, and if some other mod does that, every
// other player ends up in the wrong body with nothing in the log explaining
// why.
bool SampleLocalPlayerModel(uint16_t &modelId) {
	void *const ped = PlayerPed();
	if (!ped)
		return false;
	modelId = Field<uint16_t>(ped, offs::MODEL_INDEX);
	return true;
}

// Every one of the local player's thirteen weapon slots, plus which one is
// in their hands.
//
// An unowned slot is reported as unowned rather than skipped, because losing
// a weapon is a fact somebody has to be told: `CWeapon::Initialise` puts
// WEAPONTYPE_UNARMED back in the slot, HasWeapon goes false, and an observer
// who never hears about it leaves that player armed for the rest of the
// session.
//
// `held` is m_currentWeapon, which is the index into the array as well as
// the eWeaponType (addresses.h). Out of range it is reported as
// WEAPONTYPE_UNARMED, whose slot is 0 - which is the slot the engine itself
// falls back to.
bool SampleLocalAmmo(AmmoSlotBody *out, uint8_t &held) {
	void *const ped = PlayerPed();
	if (!ped || !out)
		return false;

	const uint8_t current = Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON);
	held = current < offs::NUM_WEAPON_SLOTS ? current : WEAPONTYPE_UNARMED;

	for (uint8_t w = 0; w < INVENTORY_SLOTS; ++w) {
		out[w].weapon = w;
		out[w].flags  = 0;
		out[w].clip   = 0;
		out[w].total  = 0;
		void *const slot = WeaponSlot(ped, w);
		if (!slot)
			continue;
		// A slot the player does not own holds a m_eWeaponType that is not
		// its own index - that is exactly the test CPed::GiveWeapon uses for
		// HasWeapon (`cmp [esi+ebx+35Ch],ebp` at 0x004CF9C9). It goes out as
		// an unowned slot rather than as an empty one: the leftover members
		// are not a count of anything, and "I do not have this weapon" is
		// news in its own right.
		if (Field<uint32_t>(slot, offs::WEAPON_TYPE) != w)
			continue;
		out[w].flags = AMMO_SLOT_OWNED;
		out[w].clip  = ClipOnWire(Field<int32_t>(slot, offs::WEAPON_AMMO_IN_CLIP));
		out[w].total = TotalOnWire(Field<int32_t>(slot, offs::WEAPON_AMMO_TOTAL));
	}
	return true;
}

// ---- which peds belong to which player ------------------------------------
//
// The reverse of RemotePlayer::poolHandle, and it exists for exactly one
// caller: the CPed::InflictDamage detour, which is handed a raw CPed* by the
// engine and has to answer "is this somebody else's player?" before it
// decides whether the local machine may hurt it. There is no time to go and
// ask the roster - the answer has to be right there, in the middle of the
// engine's own call.
//
// Keyed by the pool reference rather than by the pointer, and that's the
// whole safety argument. A raw pointer table would keep matching after the
// engine recycled the slot, and the first civilian to inherit it would start
// reporting its bullet wounds to the network as a player's. CPools::GetPed
// compares the slot's flags byte, so a stale reference resolves to null
// instead (ResolveRemote below has the long version).
struct RemotePedIdentity {
	int32_t  poolHandle = -1;
	uint16_t netId      = INVALID_NETID;
	uint8_t  playerId   = 0xFF;
};

RemotePedIdentity g_remotePeds[MAX_PLAYERS];

void RememberRemotePed(const RemotePlayer &player) {
	if (player.playerId >= MAX_PLAYERS)
		return;
	g_remotePeds[player.playerId] = RemotePedIdentity{player.poolHandle, player.netId,
	                                                  player.playerId};
}

void ForgetRemotePed(const RemotePlayer &player) {
	if (player.playerId < MAX_PLAYERS)
		g_remotePeds[player.playerId] = RemotePedIdentity{};
}

// Eight comparisons, each one a CPools::GetPed. That's cheap enough for the
// place this is called from: CPed::InflictDamage runs when something actually
// hits something, not once per ped per frame.
bool LookupRemotePed(const void *ped, uint16_t &netId) {
	if (!ped)
		return false;
	using GetPedFn = void *(__cdecl *)(int32_t);
	for (const RemotePedIdentity &id : g_remotePeds) {
		if (id.poolHandle < 0)
			continue;
		if (Func<GetPedFn>(CPools__GetPed)(id.poolHandle) != ped)
			continue;
		netId = id.netId;
		return true;
	}
	return false;
}

// Turns a remote player's pool handle back into a live CPed, or null.
//
// RemotePlayer::poolHandle is the engine's own ped reference format:
// CPools::GetPedRef returns (slotIndex << 8) | the pool's flags byte, and
// CPools::GetPed compares that whole byte again on the way back. The byte
// packs a 7-bit id in the low bits and the slot's "free" flag in the top
// bit, so a stale handle stops resolving both when the slot gets reused (id
// moves on) and when the ped is simply deleted (free bit flips). That
// distinction matters a lot in practice - the first in-game run kept a raw
// pointer instead, kept writing into a ped the engine had already destroyed,
// and then destroyed it a second time on top.
//
// The vtable check below is the second net: for a slot that somehow still
// resolves but no longer holds our ped.
// Destroy a ped CoopIII created, the way COMMAND_DELETE_CHAR does, plus the
// one thing the engine's own teardown is allowed to skip.
//
// DELETE_CHAR's whole teardown is three steps - RemoveReferencesToDeletedObject,
// the deleting destructor through vtable slot 0, and --ms_nTotalMissionPeds -
// and it leaves the world lists to ~CPed, whose first statement is
// CWorld::Remove(this). That is enough for the script, and it was not enough
// here.
//
// CWorld::Remove only unlinks an entity from ms_listMovingEntityPtrs when
// bIsStatic is clear (addresses.h has the three instructions that prove it).
// A ped that has gone to sleep still holds its node, so destroying it leaves
// that node in the list pointing at a pool slot that has just been freed, and
// CWorld::Process reads m_rwObject straight off it on the next frame. The
// crash lands at 0x004B1B25 with nothing in the stack to say who did it.
//
// A remote ped is exactly the entity this happens to. Its velocity is written
// from the wire every frame, so a remote player who stands still hands
// CPhysical::ProcessControl ten quiet frames in a row and gets put to sleep.
//
// RemoveFromMovingList is guarded by m_movingListNode, so calling it
// unconditionally costs four instructions when the ped was never linked.
//
// The fire table is the other engine container that can be holding this ped,
// and that one the engine really does clean up itself: ~CPed opens with
// `if (m_pFire) m_pFire->Extinguish()` at 0x004C51CF, and CFire::Extinguish
// nils both the fire's m_pEntity and the ped's m_pFire. Worth stating rather
// than assuming, since the last two crashes in this project were both a
// container nobody had checked.
void DestroyRemotePed(void *ped) {
	using ThisFn       = void(__thiscall *)(void *);
	using RemoveRefsFn = void(__cdecl *)(void *);
	using DtorFn       = void(__thiscall *)(void *, int);

	if (NeedsMovingListUnlink(Field<uint8_t>(ped, offs::ENTITY_FLAGS_A),
	                          Field<void *>(ped, offs::MOVING_LIST_NODE) != nullptr))
		Log("bridge: a remote ped went static while still in the moving list; "
		    "unlinking it by hand, because CWorld::Remove would have walked past it");
	Func<ThisFn>(CPhysical__RemoveFromMovingList)(ped);

	Func<RemoveRefsFn>(CWorld__RemoveReferencesToDeletedObject)(ped);

	void *const *vtable  = *reinterpret_cast<void *const *const *>(ped);
	auto         deleter = reinterpret_cast<DtorFn>(vtable[VTABLE_DELETING_DTOR]);
	deleter(ped, 1);   // 1 = also free the memory

	--Global<uint32_t>(CPopulation__ms_nTotalMissionPeds);
}

void *ResolveRemote(RemotePlayer &player) {
	if (player.poolHandle < 0)
		return nullptr;

	using GetPedFn = void *(__cdecl *)(int32_t);
	void *ped      = Func<GetPedFn>(CPools__GetPed)(player.poolHandle);

	if (ped && Field<uintptr_t>(ped, offs::VTABLE) == CCivilianPed__vtable) {
		// The engine has decided this ped has to go. bRemoveFromWorld is the
		// only flag that lets it delete a ped from inside CWorld::Process's
		// own walk over the moving list, and CoopIII would find out a frame
		// later, from a node pointing at freed memory.
		//
		// It gets set by things that have nothing to do with us:
		// CAutomobile::BlowUpCar flags a car's driver and passengers,
		// CPed::SetDie flags a non-player ped it finds in PED_DRIVING, and
		// CPed::ProcessControl flags any ped whose m_pMyVehicle has gone.
		//
		// So the ped is taken back and destroyed here, on our terms, in
		// PreFrame, before CGame::Process gets a chance. Clearing the handle
		// re-arms the two-phase spawn, so the player comes back.
		if (Field<uint8_t>(ped, offs::ENTITY_FLAGS_D) & offs::ENTITY_REMOVE_FROM_WORLD) {
			Log("bridge: the engine flagged %s's ped for destruction; taking it back "
			    "before CWorld::Process does",
			    player.nick.c_str());
			DestroyRemotePed(ped);
			player.poolHandle   = -1;
			player.spawnPending = true;
			ForgetRemotePed(player);
			return nullptr;
		}
		return ped;
	}

	// Losing a ped isn't fatal, but it can never be silent - docs/compat.md
	// §2.5. Clearing the handle also re-arms the two-phase spawn in
	// Client::UpdateRemotes, so the player comes back eventually. Logged
	// once and not once per frame, since the handle gets cleared right after.
	Log("bridge: %s's ped is gone from under us (%s)", player.nick.c_str(),
	    ped == nullptr
	        ? "pool slot no longer matches the handle"
	        : (Field<uintptr_t>(ped, offs::VTABLE) == CPlaceable__vtable
	               ? "object has been through its destructor"
	               : "object is no longer a CCivilianPed"));
	player.poolHandle   = -1;
	player.spawnPending = true;
	ForgetRemotePed(player);
	return nullptr;
}

// ---- placing a ped the engine did not move itself -------------------------
//
// Writing the position into the entity's matrix is maybe a third of the job.
// The other two thirds are why the second in-game run's remote player was
// still invisible even after it stopped being deleted.
//
// The RenderWare frame. CEntity::CreateRwObject attaches the entity's CMatrix
// to the clump's RwFrame matrix via CMatrix::AttachRW, which copies once, at
// attach time - i.e. whatever identity matrix the constructor left, at the
// world origin. After that the two live in separate memory, and only
// CMatrix::UpdateRW copies one into the other. CEntity::UpdateRwFrame then
// dirties the frame so RenderWare recomputes its LTM. Need both, in that
// order.
//
// The sector grid. CWorld::Add files an entity into the sectors its bounding
// rect covers and never re-reads its position after that. CPhysical::
// RemoveAndAdd is what re-files it, called every frame for every entity the
// engine's own physics actually moved.
//
// The facing. What the ped is drawn with is the matrix's rotation.
// m_fRotationCur is only the value CPed::ProcessControl eases toward - it
// never reaches the matrix on a ped the engine isn't moving itself.
//
// CWorld::Process does all three, but behind `if (!bIsInSafePosition)`,
// meaning only for entities its own physics moved. A ped teleported in from
// outside never looks like one of those, so it gets none of the three.
//
// This is what identified the bug, measured on the live game on 2026-09-21:
// the ped's position orbited the local player correctly at 4 m, while its
// clump's frame matrix sat frozen at exactly (0.00 0.00 0.00) and its
// m_scanCode stayed 0. It would have drawn at the world origin even if the
// render scan had ever visited it - and it never did, because the ped was
// still filed in whatever sector it happened to get created in.
//
// `inWorld` is false for the one call that happens before CWorld::Add: an
// entity not yet in the world must be filed, not re-filed.
void PlaceRemotePed(void *ped, const Vec3 &pos, float heading, bool inWorld) {
	using RotateFn = void(__thiscall *)(void *, float, float, float);
	using ThisFn   = void(__thiscall *)(void *);

	void *const matrix = reinterpret_cast<uint8_t *>(ped) + offs::MATRIX;

	// COMMAND_CREATE_CHAR's own SetOrientation is this call followed by
	// restoring the position, since CMatrix::SetRotate zeroes it out. So:
	// rotate first, write position second.
	float yaw = 0.0f;
	FiniteOr(heading, 0.0f, yaw);
	Func<RotateFn>(CMatrix__SetRotate)(matrix, 0.0f, 0.0f, WrapAngle(yaw));

	// Clamped: the next two calls turn x and y into subscripts into
	// CWorld::ms_aSectors, and neither one bounds-checks (pedanim.h).
	float *const p = &Field<float>(ped, offs::POSITION);
	p[0]           = ClampToWorld(pos.x);
	p[1]           = ClampToWorld(pos.y);
	FiniteOr(pos.z, 0.0f, p[2]);

	Func<ThisFn>(CMatrix__UpdateRW)(matrix);
	Func<ThisFn>(CEntity__UpdateRwFrame)(ped);
	if (inWorld)
		Func<ThisFn>(CPhysical__RemoveAndAdd)(ped);
}

// Creates a ped exactly the way COMMAND_CREATE_CHAR does: allocate from the
// ped pool, run the CCivilianPed constructor, register it, place it, add to
// the world. Doing exactly what the engine does is the whole safety argument
// here (see the provenance note in addresses.h).
//
// Registration isn't decoration, skip it and the ped goes invisible - that's
// what happened on the first in-game run. CPed::CPed leaves CharCreatedBy as
// RANDOM_CHAR, which counts the ped as ambient population, and the engine's
// own sweeps (CPopulation::ManagePopulation, MoveCarsAndPedsOutOfAbandonedZones,
// CWorld::ClearPedsFromArea, CWorld::ClearExcitingStuffFromArea,
// CWorld::RemoveFallenPeds) deleted it within seconds. Every one of those
// gates on CPed::CanBeDeleted(), which comes down to this one byte.
bool SpawnRemote(RemotePlayer &player) {
	using NewFn   = void *(__cdecl *)(size_t);
	using CtorFn  = void(__thiscall *)(void *, int, int);
	using AddFn   = void(__cdecl *)(void *);
	using RefFn   = int32_t(__cdecl *)(void *);
	using LevelFn = uint8_t(__cdecl *)(const float *);

	// Refuse to create a ped we can't place. The caller already checks this,
	// but a ped added to the world at the origin just drowns before the first
	// position arrives, and that failure is silent and a pain to diagnose -
	// worth ruling out here too.
	//
	// Uses the newest snapshot rather than an interpolated pose: a ped is
	// born where the session last said this player is, and there's nothing
	// gained by placing it a hundred milliseconds in the past. Reading
	// `last` directly also avoids advancing the playback clock, which
	// belongs to the frame pump, not to a one-off spawn.
	if (!player.haveState)
		return false;
	const Vec3  bornAt     = player.last.pos;
	const float bornFacing = player.last.heading;

	// 0 is a model index here, not "unset" - it's MI_PLAYER, what every remote
	// player wears. Treating it as unset is exactly what put a random
	// civilian in the other player's seat for the whole first two-client run.
	uint16_t model = player.modelId;
	if (!HasModelLoaded(model)) {
		// UpdateRemotes only calls us once IsModelReady says yes, but the
		// streamer can evict in between, and constructing against an
		// unloaded model crashes rather than just producing a missing ped.
		// Falling back to a civilian beats refusing to show the player at
		// all.
		if (model == MI_PLAYER && HasModelLoaded(MI_MALE01)) {
			Log("bridge: the player model is not loaded; %s gets a civilian",
			    player.nick.c_str());
			model = MI_MALE01;
		} else {
			return false;
		}
	}

	void *ped = Func<NewFn>(CPed__operator_new)(offs::SIZEOF_PED);
	if (!ped) {
		Log("bridge: ped pool is full; cannot spawn %s", player.nick.c_str());
		return false;
	}

	Func<CtorFn>(CCivilianPed__ctor)(ped, PEDTYPE_CIVMALE, model);

	// Check the constructor actually finished, and log it either way. A
	// half-constructed ped and a destroyed one look nearly identical in
	// memory - the last session burned a whole run telling them apart. The
	// vtable and the clump are what actually distinguish the two.
	const uintptr_t vtable = Field<uintptr_t>(ped, offs::VTABLE);
	void *const     clump  = Field<void *>(ped, offs::RW_OBJECT);
	if (vtable != CCivilianPed__vtable || clump == nullptr) {
		Log("bridge: CCivilianPed ctor did not complete for %s "
		    "(vtable %08X, want %08X; clump %p) - abandoning the slot",
		    player.nick.c_str(), static_cast<unsigned>(vtable),
		    static_cast<unsigned>(CCivilianPed__vtable), clump);
		// Leaked on purpose, not freed: an object whose constructor didn't
		// finish must never be run through a destructor.
		return false;
	}

	// Everything COMMAND_CREATE_CHAR does between the constructor and
	// CWorld::Add (handler 0x0043BB25 onward), in the handler's own order.
	Field<uint8_t>(ped, offs::PED_CHAR_CREATED_BY) = CHAR_CREATED_BY_MISSION;
	Field<uint8_t>(ped, offs::PED_FLAGS_C) &=
	    static_cast<uint8_t>(~offs::PED_RESPONDS_TO_THREATS);
	Field<uint8_t>(ped, offs::PED_FLAGS_G) &=
	    static_cast<uint8_t>(~offs::PED_ALLOW_MEDICS);

	// A remote player's damage is decided by whoever owns them, and it
	// arrives over the wire as health. Local physics gets no vote - it only
	// ever sees a ped teleported 25 times a second, not a motion any damage
	// model was ever written to handle. Before this fix, the ped died in the
	// same frame its first collision got processed (flagsA 0x91 -> 0xD3,
	// pedState PED_IDLE -> PED_DIE), and a dead ped is exactly why the
	// renderer accepted it and drew nothing.
	//
	// Host-authoritative rule from docs/roadmap.md §5, applied to one entity:
	// observers don't get to decide damage.
	Field<uint8_t>(ped, offs::ENTITY_FLAGS_C) |= static_cast<uint8_t>(
	    offs::ENTITY_BULLET_PROOF | offs::ENTITY_FIRE_PROOF |
	    offs::ENTITY_COLLISION_PROOF | offs::ENTITY_MELEE_PROOF);

	// And the fifth proof - not in that byte with its four siblings, and not
	// covered by any of them either. CPed::InflictDamage tests bExplosionProof
	// alone for ROCKETLAUNCHER, GRENADE, MOLOTOV and the generic EXPLOSION
	// cause (re3 PedFight.cpp:2245-2271); bFireProof only covers the
	// flamethrower, not a blast. Skip this and the first grenade anyone
	// throws has every observer deciding every remote player's health - the
	// one thing this whole file exists to prevent. addresses.h names the
	// script handler that pins the bit.
	Field<uint8_t>(ped, offs::ENTITY_FLAGS_B) |= offs::ENTITY_EXPLOSION_PROOF;

	// Place before adding: CWorld::Add files the entity into the sector grid
	// by its position, so adding first would file it at the origin, and
	// nothing re-reads the position afterward - it'd stay filed there until
	// ApplyRemotePose re-files it.
	//
	// The position was already taken and validated at the top of this
	// function, so this placement call is unconditional - there's no path
	// through here that adds an unplaced ped to the world.
	PlaceRemotePed(ped, bornAt, bornFacing, /*inWorld=*/false);
	Field<float>(ped, offs::PED_ROT_CUR)  = WrapAngle(bornFacing);
	Field<float>(ped, offs::PED_ROT_DEST) = WrapAngle(bornFacing);

	// CWorld::Add's last act is AddToMovingList, and that function has no
	// check for an entity that is already in the list: it overwrites
	// m_movingListNode with the new node and forgets the old one, which stays
	// linked forever with nothing left that can unlink it. So a second
	// CWorld::Add on one entity orphans a node, and an orphaned node outlives
	// the entity it points at.
	//
	// A ped straight out of the constructor cannot be in the list, because
	// CPhysical's constructor nils the field. Checking anyway costs one load
	// and turns the day this stops being true into a log line.
	if (Field<void *>(ped, offs::MOVING_LIST_NODE) != nullptr) {
		Log("bridge: a newly constructed ped is already in the moving list; "
		    "unlinking before CWorld::Add, or the node it holds would be orphaned");
		Func<void(__thiscall *)(void *)>(CPhysical__RemoveFromMovingList)(ped);
	}

	Func<AddFn>(CWorld__Add)(ped);

	// LEVEL_IGNORE, not whatever level the position falls in.
	//
	// CREATE_CHAR calls CTheZones::GetLevelFromPosition here because a
	// mission ped belongs to an island and should get culled along with it.
	// A remote player must not be culled that way: the engine drops entities
	// whose m_nZoneLevel disagrees with CGame::currLevel, and that's exactly
	// what happened when this got measured on the live game - the ped
	// existed, had a clump and bIsVisible, sat 4.00 m from the player, and
	// was simply never drawn. Its m_nZoneLevel was 2 while the local
	// player's was -1.
	//
	// -1 is what the player ped itself uses. re3 notes LEVEL_IGNORE is "only
	// used in CPhysical's m_nZoneLevel" (Game.h:4) - it means "don't cull me
	// by level," which is exactly what a networked entity wants.
	Field<int8_t>(ped, offs::ZONE_LEVEL) = LEVEL_IGNORE;
	++Global<uint32_t>(CPopulation__ms_nTotalMissionPeds);

	player.poolHandle = Func<RefFn>(CPools__GetPedRef)(ped);
	RememberRemotePed(player);

	// A new ped inherits none of what got driven into the old one, and this
	// path also runs when a ped comes back after the engine took it away.
	// Leave these set and ApplyRemotePose skips the first weapon and
	// animation as "already applied" - a remote player respawns unarmed in a
	// T-pose and stays that way until they next switch weapons.
	player.appliedAnimId  = ANIM_NONE;
	player.appliedAnimId2 = ANIM_NONE;
	player.appliedWeapon  = 0xFFFF;
	player.appliedDriveBy = ANIM_NONE;
	player.driveByArmed   = false;
	// A brand new ped is not on fire, whatever the old one was doing. The
	// old ped's fire, if it had one, was put out by ~CPed on the way down
	// (0x004C51CF: `if (m_pFire) m_pFire->Extinguish()`), so this is
	// forgetting a slot the engine has already handed back, not leaking one.
	player.fireSlot = -1;

	Log("bridge: spawned %s as a mission ped (ref %d, model %u, clump %p)",
	    player.nick.c_str(), player.poolHandle, model, clump);
	return true;
}

// COMMAND_DELETE_CHAR's teardown, through DestroyRemotePed above.
//
// Resolving first is what makes this safe to call on a player whose ped the
// engine already took: ResolveRemote says so, clears the handle, and returns
// null. It can also destroy the ped itself, for a ped the engine has flagged,
// in which case there is nothing left to do here.
void DespawnRemote(RemotePlayer &player) {
	EndRemoteProjectilesOf(player.playerId);
	void *ped = ResolveRemote(player);
	player.poolHandle   = -1;
	player.spawnPending = false;
	player.fireSlot     = -1;
	ForgetRemotePed(player);
	if (!ped)
		return;   // already gone, and ResolveRemote already said why

	DestroyRemotePed(ped);
}

// ---- animation, weapon and aim on a remote ped ----------------------------
//
// Everything below drives an engine *input* and lets the engine do the work,
// instead of writing the result directly. Not a style choice -
// docs/protocol.md §1.8.3 explains why it's the only thing that actually
// works: CWorld::Process rewrites every moving entity's bone matrices from
// its animations, and only then runs ProcessControl. CoopIII's inbound hook
// sits before both, so a bone matrix written here is gone before the frame
// draws. A bIsAimingGun written here, on the other hand, is exactly what the
// engine reads on its way past.
//
// m_nMoveState is the exception, and it took until 2026-09-22 to notice:
// CPed::Idle runs from ProcessControl's state switch, before SetMoveAnim,
// and its non-still arm ends in `if (!IsPlayer()) SetMoveState(PEDMOVE_STILL)`
// (addresses.h, CPed__Idle). So the move state written from PreFrame is
// always overwritten before anything reads it, and the locomotion animation
// has only ever come from BlendRemoteAnim below. docs/protocol.md §1.13.4.

// CAnimBlendAssocGroup::numAssociations for one group, or 0 if the anim
// files haven't loaded yet. This is what makes playing a received animId
// safe at all - the engine's own lookup is just a shift and an add, nothing
// in between (addresses.h, CAnimManager section).
int32_t AnimGroupCount(int group) {
	if (!ValidAnimGroup(group))
		return 0;
	const uintptr_t groups = Global<uintptr_t>(CAnimManager__ms_aAnimAssocGroups);
	if (!groups)
		return 0;
	const int32_t count = *reinterpret_cast<int32_t *>(
	    groups + static_cast<size_t>(group) * SIZEOF_ANIMGROUP + ANIMGROUP_COUNT);
	return count > 0 ? count : 0;
}

// An animation time off the wire, bounded before it reaches the engine.
// CAnimBlendAssociation::SetCurrentTime walks a repeating animation forward
// one totalLength at a time until it's in range, so a huge value becomes a
// long loop on the game thread, not a bad read. No GTA III animation runs
// anywhere near 30 seconds.
float SafeAnimTime(float wire) {
	float t = 0.0f;
	if (!FiniteOr(wire, 0.0f, t) || t < 0.0f)
		return 0.0f;
	return t > 30.0f ? 30.0f : t;
}

// Starts `animId` on the ped's clump through the engine's own blender, then
// seeks it to the phase the sender was at. Returns false if the id can't be
// played safely; the caller leaves its applied-id alone in that case and
// tries again next change.
// Both defined below, with the rest of the clump bookkeeping.
void *FindAnimById(void *clump, uint16_t animId);
bool  MakeAnimRoom(RemotePlayer &player, void *clump, void *keepAssoc);

bool BlendRemoteAnim(RemotePlayer &player, void *clump, int pedGroup, uint16_t animId,
                     float animTime, float animSpeed, bool applySpeed) {
	const AnimPlan plan = PlanAnim(animId, pedGroup, AnimGroupCount(pedGroup),
	                               AnimGroupCount(ASSOCGRP_STD));
	if (!plan.valid)
		return false;

	// BlendAnimation adds an association when it does not find one, and a
	// clump that is already at the engine's limit cannot take another
	// without RpAnimBlendClumpUpdateAnimations writing past the end of its
	// node array. Reviving one that is already there is free, so only a
	// genuine addition has to ask.
	if (!FindAnimById(clump, animId) && !MakeAnimRoom(player, clump, nullptr))
		return false;

	using BlendFn = void *(__cdecl *)(void *, int, int, float);
	void *assoc   = Func<BlendFn>(CAnimManager__BlendAnimation)(
        clump, plan.group, static_cast<int>(animId), plan.blendDelta);
	if (!assoc)
		return false;

	// Seek through SetCurrentTime, not by writing currentTime directly - the
	// engine function also uncompresses the hierarchy and re-seeks every
	// node's keyframe. A raw write leaves those cursors pointing at the old
	// phase, so the animation plays from the wrong keyframes until it happens
	// to wrap around.
	using SeekFn = void(__thiscall *)(void *, float);
	Func<SeekFn>(CAnimBlendAssociation__SetCurrentTime)(assoc, SafeAnimTime(animTime));

	// speed only applies to non-movement animations. A movement anim takes
	// its rate from the ped's actual velocity, already synced - and that
	// self-syncing beats anything the wire could carry anyway.
	if (applySpeed && !(Field<int32_t>(assoc, ANIM_FLAGS) & ASSOC_MOVEMENT)) {
		float speed = 1.0f;
		FiniteOr(animSpeed, 1.0f, speed);
		Field<float>(assoc, ANIM_SPEED) = speed;
	}
	return true;
}

// Fade a partial overlay out when the sender stops playing one.
// BlendAnimation only fades the animations it's replacing, so "no overlay at
// all" needs to be said explicitly - same delta and flag the engine uses
// when a FADEOUTWHENDONE animation finishes on its own
// (CAnimBlendAssociation::UpdateTime).
void FadeOutPartial(void *clump, uint16_t animId) {
	ForEachAnim(clump, [&](void *assoc) {
		const int32_t flags = Field<int32_t>(assoc, ANIM_FLAGS);
		if (!(flags & ASSOC_PARTIAL))
			return;
		if (Field<int32_t>(assoc, ANIM_ID) != static_cast<int32_t>(animId))
			return;
		Field<float>(assoc, ANIM_BLEND_DELTA) = -4.0f;
		Field<int32_t>(assoc, ANIM_FLAGS)     = flags | ASSOC_DELETEFADEDOUT;
	});
}

// Fade out every partial on a clump, without needing to know their ids.
//
// A seated ped carries partials CoopIII never added and has no record of.
// CPed::ProcessControl blends ANIM_STD_CAR_DRIVE_LEFT and its siblings off
// the car's own m_fSteerAngle (re3 Ped.cpp:2880-2945), and those are
// ASSOC_PARTIAL. Once the ped is out of the car, nothing maintains them and
// nothing removes them: FadeOutPartial only knows the id CoopIII applied, so
// the steering pose stays on a ped standing in the street at whatever weight
// it had. It used to clear itself when the player jumped, because a change
// of move state makes CPed::SetMoveAnim purge the partials for its own
// reasons.
//
// Safe to do wholesale at the moment a seat is given up: whatever overlay
// the wire wants gets re-driven on the next frame anyway, since the seat
// also clears appliedAnimId2.
int FadeOutAllPartials(void *clump) {
	int n = 0;
	ForEachAnim(clump, [&](void *assoc) {
		const int32_t flags = Field<int32_t>(assoc, ANIM_FLAGS);
		if (!(flags & ASSOC_PARTIAL))
			return;
		Field<float>(assoc, ANIM_BLEND_DELTA) = -4.0f;
		Field<int32_t>(assoc, ANIM_FLAGS)     = flags | ASSOC_DELETEFADEDOUT;
		++n;
	});
	return n;
}

// How many animations are on this clump right now.
//
// This is the number RpAnimBlendClumpUpdateAnimations is about to index its
// twelve-slot node array with, so it is the number that decides whether the
// engine is about to corrupt its own stack. Every association counts:
// UpdateBlend only returns false for one that deletes itself on that very
// call, and a fading association is still there until it reaches zero.
int CountAnims(void *clump) {
	int n = 0;
	ForEachAnim(clump, [&](void *) { ++n; });
	return n;
}

// Mark an association for deletion on the engine's next pass over the clump.
//
// Not a fade: blendAmount goes to zero and the delta negative, which is the
// exact condition CAnimBlendAssociation::UpdateBlend deletes on, and
// UpdateBlend runs at the top of RpAnimBlendClumpUpdateAnimations. So an
// association dropped here is gone before the node array is filled, in the
// same frame, rather than lingering for the quarter second a fade takes.
void DropAnimNow(void *assoc) {
	Field<float>(assoc, ANIM_BLEND_AMOUNT) = 0.0f;
	Field<float>(assoc, ANIM_BLEND_DELTA)  = -1.0f;
	Field<int32_t>(assoc, ANIM_FLAGS) |= ASSOC_DELETEFADEDOUT;
}

// Get a clump back under the engine's limit, weakest animations first.
//
// "Weakest" is lowest blendAmount, which is the engine's own idea of what is
// least visible: RpAnimBlendClumpGetMainAssociation picks the highest. The
// two animations CoopIII is currently driving are never dropped, because
// dropping them would just have the next frame add them again and we would
// be back here.
//
// Returns how many were dropped.
// `keepAssoc`, when non-null, is one more that must survive whatever its
// blend weight is: the ped's m_pVehicleAnim. That association is not one
// CoopIII applied and has no id here to spare it by, and it is the only
// thing holding the finish callback that ends a door-opening chain. Drop it
// and the ped stays in PED_ENTER_CAR with nothing left to finish - a remote
// player frozen half inside a car, which is precisely the failure the seat
// deadline exists to make impossible and which should not be reachable from
// inside the safety net either.
int PruneAnims(void *clump, uint16_t keepA, uint16_t keepB, void *keepAssoc,
               int surplus) {
	if (surplus <= 0)
		return 0;

	// Small and fixed: this runs on the game thread and the walk above is
	// already bounded at 64.
	constexpr int MAX_TRACKED = 64;
	void  *assocs[MAX_TRACKED];
	float  blends[MAX_TRACKED];
	int    n = 0;

	ForEachAnim(clump, [&](void *assoc) {
		if (n >= MAX_TRACKED)
			return;
		if (assoc == keepAssoc)
			return;
		const int32_t id = Field<int32_t>(assoc, ANIM_ID);
		if (id == static_cast<int32_t>(keepA) || id == static_cast<int32_t>(keepB))
			return;
		// Already on its way out, so it is not part of the problem.
		if (Field<float>(assoc, ANIM_BLEND_DELTA) < 0.0f &&
		    (Field<int32_t>(assoc, ANIM_FLAGS) & ASSOC_DELETEFADEDOUT))
			return;
		assocs[n] = assoc;
		blends[n] = Field<float>(assoc, ANIM_BLEND_AMOUNT);
		++n;
	});

	int dropped = 0;
	while (dropped < surplus) {
		int weakest = -1;
		for (int i = 0; i < n; ++i)
			if (assocs[i] && (weakest < 0 || blends[i] < blends[weakest]))
				weakest = i;
		if (weakest < 0)
			break;   // nothing left that may be dropped
		DropAnimNow(assocs[weakest]);
		assocs[weakest] = nullptr;
		++dropped;
	}
	return dropped;
}

// Make room for one more animation on this clump, and say whether there is
// any. False means the caller must not add: the engine's node array is full
// and one more association writes past the end of it.
// Keep a clump inside the engine's limit whatever put the animations there.
//
// MakeAnimRoom only runs when CoopIII wants to add one, and CoopIII is not
// the only thing adding. The engine's two contributors are now known by
// name rather than guessed at (docs/protocol.md §1.13.5): CPed::Idle blends
// ANIM_STD_IDLE_BIGGUN on a 3000-8500 ms random timer while the ped is
// still, and CPed::SetInTheAir blends ANIM_STD_FALL_GLIDE whenever
// CheckIfInTheAir finds no ground under a ped whose position came off the
// wire. Neither asks anybody, and the second one fires on ordinary streamed
// movement. So this runs unconditionally, once per remote ped per frame,
// before CGame::Process gets the chance to walk the clump.
//
// It is the last line of defence for a crash that has no symptoms until it
// happens, so it is deliberately not conditional on anything CoopIII knows.
void EnforceAnimLimit(RemotePlayer &player, void *ped, void *clump) {
	const int count = CountAnims(clump);
	const int surplus = AnimClumpSurplus(count + 1);   // +1: room to still add one
	if (surplus <= 0)
		return;

	const int dropped =
	    PruneAnims(clump, player.appliedAnimId, player.appliedAnimId2,
	               ped ? Field<void *>(ped, offs::PED_VEHICLE_ANIM) : nullptr,
	               surplus);
	if (dropped > 0)
		Log("bridge: %s's ped was carrying %d animations, past what the engine's "
		    "node array can index; dropped %d",
		    player.nick.c_str(), count, dropped);
}

bool MakeAnimRoom(RemotePlayer &player, void *clump, void *keepAssoc) {
	const int count = CountAnims(clump);
	if (AnimClumpHasRoom(count))
		return true;

	const int dropped = PruneAnims(clump, player.appliedAnimId,
	                               player.appliedAnimId2, keepAssoc,
	                               AnimClumpSurplus(count));
	if (dropped > 0)
		Log("bridge: %s's ped had %d animations on it, which is past what "
		    "RpAnimBlendClumpUpdateAnimations can index; dropped %d",
		    player.nick.c_str(), count, dropped);

	return AnimClumpHasRoom(count - dropped);
}

// The first association on the clump with this id, or null.
void *FindAnimById(void *clump, uint16_t animId) {
	void *found = nullptr;
	ForEachAnim(clump, [&](void *assoc) {
		if (!found && Field<int32_t>(assoc, ANIM_ID) == static_cast<int32_t>(animId))
			found = assoc;
	});
	return found;
}

// Is an overlay we drove still alive, or has the engine already ended it
// behind our back?
//
// This is the difference between a remote player firing and a remote player
// firing once. Every weapon animation is declared
// ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL in the engine's own table
// (CAnimManager's ms_aAnimAssocDefinitions, and
// CAnimBlendAssociation::Init copies those flags onto every copy), so one
// cycle plays, CAnimBlendAssociation::UpdateTime sets blendDelta to -4 and
// ASSOC_DELETEFADEDOUT, and the association is gone a quarter of a second
// later. On the shooter's own machine CPed::FireGun puts it straight back
// for the next round. An observer has no FireGun: the id just sits on the
// wire for as long as the trigger is held, unchanged, with nothing playing.
//
// CPed::SetMoveAnim ends overlays the same way, fading out every partial
// that is *not* ASSOC_FADEOUTWHENDONE whenever the move state changes to a
// walk, a run or a sprint. Weapon animations are spared by that flag;
// knockdowns and punches are not, and they die the same silent death.
//
// Neither one changes the id on the wire, so neither one is visible to a
// comparison against the last id applied. Hence this.
bool AnimIsCondemned(void *assoc) {

	// Condemned: something has given it a negative blend delta and asked for
	// it to be deleted once it reaches zero. Both routes above leave exactly
	// that state. Catching it here rather than waiting for the association
	// to disappear is what keeps the gap invisible, and it costs nothing -
	// CAnimManager::BlendAnimation revives an association it finds by
	// recomputing the delta as (1 - blendAmount) * delta, which is positive.
	const int32_t flags = Field<int32_t>(assoc, ANIM_FLAGS);
	return (flags & ASSOC_DELETEFADEDOUT) != 0 &&
	       Field<float>(assoc, ANIM_BLEND_DELTA) < 0.0f;
}

// CWeaponInfo for a weapon type, or null. Declared here because the overlay
// below needs the firing loop out of it; the definition is further down with
// the rest of the weapon code.
void *WeaponInfo(uint8_t weaponType);

// Seek an association to a phase through the engine's own function, which
// also re-seeks every node's keyframe cursor. Writing currentTime directly
// leaves those pointing at the old phase.
void SeekAnim(void *assoc, float time) {
	using SeekFn = void(__thiscall *)(void *, float);
	Func<SeekFn>(CAnimBlendAssociation__SetCurrentTime)(assoc, SafeAnimTime(time));
}

// How far the local phase may drift from the wire's before it gets pulled
// back. Only used while the overlay is frozen, where the wire value does not
// change at all, so in practice this fires once and then never again.
constexpr float ANIM_PHASE_TOLERANCE = 0.01f;

// The partial overlay: the weapon animation, a punch, a throw.
//
// This is the part that looked wrong on screen, and the reason is that a
// weapon in GTA III has one animation, not three. The draw, the ready pose,
// the shot and the recovery are all frames of ANIM_STD_WEAPON_HGUN_BODY and
// its siblings. Which part you see is decided by two things that used to be
// missing from this seam entirely:
//
//   whether the association is running. CPed::PointGunAt parks it on
//   m_fAnimLoopStart and clears ASSOC_RUNNING, and that frozen frame is the
//   aim. A new association is created running (CAnimManager::AddAnimation
//   ends in Start(0.0f) for anything that is not a movement anim), so an
//   observer that only copies the id and the phase gets a gun being drawn,
//   played to the end, deleted by ASSOC_FADEOUTWHENDONE, and started again.
//   Over and over, and never the firing part. PF_ANIM2_RUNNING fixes that.
//
//   where it loops. CPed::FireGun wraps back to m_fAnimLoopStart the moment
//   the playhead passes m_fAnimLoopEnd while the trigger is held, so a firing
//   weapon cycles the middle of its animation and never reaches the end.
//   Replicating that loop is what makes a remote player firing look like a
//   local player firing, which is the only test that means anything here.
void ApplyOverlay(RemotePlayer &player, void *clump, int pedGroup) {
	const uint16_t want = player.last.animId2;

	if (want == ANIM_NONE) {
		if (player.appliedAnimId2 != ANIM_NONE) {
			FadeOutPartial(clump, player.appliedAnimId2);
			player.appliedAnimId2 = ANIM_NONE;
		}
		return;
	}

	const bool running = (player.last.flags & PF_ANIM2_RUNNING) != 0;

	// Start it, or revive it if something has condemned it behind our back:
	// ASSOC_FADEOUTWHENDONE when it ran to the end, or CPed::SetMoveAnim's
	// purge of every partial on a change of move state.
	void *assoc = FindAnimById(clump, want);

	// With one exception, and the rocket launcher is what it is for.
	//
	// An animation that has finished and is fading, whose owner has also
	// stopped running theirs, is an animation that is *supposed* to be
	// ending. Reviving it holds a pose its owner is already blending out of.
	//
	// Every weapon that loops gets out of this by never finishing:
	// CPed::FireGun wraps it at m_fAnimLoopEnd while the trigger is held.
	// The rocket launcher does not loop. weapon.dat gives it an
	// m_fAnimLoopEnd of 99 frames, 3.3 seconds, longer than the animation
	// itself, and CPed::FireGun's other arm for ending an attack is switched
	// off for projectile weapons: `!IsRunning() && m_eWeaponFire !=
	// WEAPON_FIRE_PROJECTILE`. So a rocket's animation plays once, finishes,
	// fades, and goes, which is the whole of how a rocket launcher looks.
	// Reviving it turned that into a pose held until the sender stopped
	// naming it.
	if (assoc && AnimIsCondemned(assoc) && !running)
		return;

	if (!assoc || AnimIsCondemned(assoc)) {
		if (!BlendRemoteAnim(player, clump, pedGroup, want, player.last.animTime2,
		                     1.0f, false))
			return;
		assoc = FindAnimById(clump, want);
		if (!assoc)
			return;
	}
	player.appliedAnimId2 = want;

	int32_t &flags = Field<int32_t>(assoc, ANIM_FLAGS);
	if (running)
		flags |= ASSOC_RUNNING;
	else
		flags = flags & ~ASSOC_RUNNING;

	if (!running) {
		// Held, not played. Nothing advances it, so it can never reach the
		// end, never get condemned and never restart. One seek and it sits
		// exactly where its owner is holding it.
		if (std::fabs(Field<float>(assoc, ANIM_CURRENT_TIME) -
		              SafeAnimTime(player.last.animTime2)) > ANIM_PHASE_TOLERANCE)
			SeekAnim(assoc, player.last.animTime2);
		return;
	}

	// Running. The loop belongs to the weapon, so it only applies when this
	// overlay really is that weapon's animation; a punch or a throw has no
	// loop and plays straight through.
	void *const info = WeaponInfo(player.last.weapon);
	if (!info)
		return;
	const int32_t animId  = Field<int32_t>(info, WEAPONINFO_ANIM_TO_PLAY);
	const int32_t animId2 = Field<int32_t>(info, WEAPONINFO_ANIM2_TO_PLAY);
	if (animId != static_cast<int32_t>(want) && animId2 != static_cast<int32_t>(want))
		return;

	const float loopStart = Field<float>(info, WEAPONINFO_ANIM_LOOP_START);
	const float loopEnd   = Field<float>(info, WEAPONINFO_ANIM_LOOP_END);
	if (WeaponAnimShouldLoop(Field<float>(assoc, ANIM_CURRENT_TIME), loopStart,
	                         loopEnd)) {
		flags |= ASSOC_RUNNING;   // Start() sets this; SetCurrentTime does not
		SeekAnim(assoc, loopStart);
	}
}

// The five animations CPed::SetMoveAnim can pick from, and therefore exactly
// the ones that need re-hanging when the walking style changes. re3's
// CPlayerPed::ReApplyMoveAnims (PlayerPed.cpp:209) names the same five.
constexpr uint16_t MOVE_ANIMS[] = {
    0,   // ANIM_STD_WALK
    1,   // ANIM_STD_RUN
    2,   // ANIM_STD_RUNFAST
    3,   // ANIM_STD_IDLE
    4,   // ANIM_STD_STARTWALK
};

// Change a remote ped's walking style and re-hang its locomotion onto the
// new one.
//
// This is the whole trick behind making a strafing player look like one.
// GTA III has no sideways walk animation - ASSOCGRP_PLAYERLEFT holds its own
// ANIM_STD_WALK and ANIM_STD_RUN, and CPlayerPed::ProcessAnimGroups swaps the
// whole group as the walk angle crosses fifty degrees. The id on the wire is
// identical either way, so without swapping the group a player strafing left
// gets drawn sprinting forward instead.
//
// Writing m_animGroup alone doesn't do it. The reason is the first line of
// CPed::SetMoveAnim: `if (m_nStoredMoveState == m_nMoveState) return`. A
// player who starts strafing while already running changes group without
// changing move state, so the engine never looks again and the old group's
// forward run just keeps playing.
//
// So the swap happens here instead, the same way the engine does it for its
// own player in CPlayerPed::ReApplyMoveAnims: for each locomotion animation
// on the clump, add the same id out of the new group, hand it the old one's
// blendAmount and blendDelta so the stride doesn't restart, and tear the old
// one down. Built on top of CAnimManager::AddAnimation instead of calling
// ReApplyMoveAnims directly - that's a CPlayerPed method, and every call it
// makes is already a verified address in this file, so finding it would buy
// nothing.
void ApplyAnimGroup(RemotePlayer &player, void *ped, void *clump) {
	const int wanted = static_cast<int>(player.last.animGroup);

	// A group this build doesn't have, or one whose animations never
	// loaded. AnimGroupCount is the same bounds check BlendRemoteAnim relies
	// on - skip it and you get an unchecked index into ms_aAnimAssocGroups.
	const int count = AnimGroupCount(wanted);
	if (!ValidAnimGroup(wanted) || count <= 0)
		return;

	// Four, because that's what CPed::SetMoveAnim itself needs - its four
	// arms ask the ped's group for ANIM_STD_WALK, _RUN, _RUNFAST and _IDLE,
	// ids 0 to 3, and it doesn't check either. This value came off a socket,
	// so a group that can't answer those four hands the *engine* an
	// out-of-range lookup on a later frame, somewhere CoopIII has no hook.
	constexpr int SETMOVEANIM_NEEDS = 4;
	if (count < SETMOVEANIM_NEEDS)
		return;

	if (Field<int32_t>(ped, offs::PED_ANIM_GROUP) == wanted)
		return;

	Field<int32_t>(ped, offs::PED_ANIM_GROUP) = wanted;

	// Five at once is the biggest single thing CoopIII does to a clump, and
	// the engine's node array only holds twelve. Each one replaces an
	// animation that is already there, but the replacement and the original
	// both exist until the original's fade completes, so a group change
	// briefly doubles the locomotion animations.
	using AddFn = void *(__cdecl *)(void *, int, int);
	for (const uint16_t id : MOVE_ANIMS) {
		// Same bound as PlanAnim, and it's not optional here either. A
		// walking style holds four or five animations, not the whole
		// AnimationId namespace, and CAnimManager::GetAnimAssociation is a
		// shift and an add with nothing in between. Ask a four-entry group
		// for ANIM_STD_STARTWALK and you get the element past the end of its
		// list, whose hierarchy pointer is whatever happens to follow it in
		// memory. The crash isn't even here - it's one frame later inside
		// CWorld::Process, when the engine walks the association list to
		// update it. Measured: c0000005 at address 0, reported against
		// 0x0048C97F, the return address of CGame::Process's call into
		// CWorld::Process.
		if (id >= count)
			continue;

		void *const old = FindAnimById(clump, id);
		if (!old)
			continue;   // not playing: nothing to re-hang

		if (!MakeAnimRoom(player, clump, nullptr))
			break;   // no room, and the old group keeps playing

		void *const fresh =
		    Func<AddFn>(CAnimManager__AddAnimation)(clump, wanted, static_cast<int>(id));
		if (!fresh)
			continue;

		Field<float>(fresh, ANIM_BLEND_AMOUNT) = Field<float>(old, ANIM_BLEND_AMOUNT);
		Field<float>(fresh, ANIM_BLEND_DELTA)  = Field<float>(old, ANIM_BLEND_DELTA);

		// Gone on the engine's next pass, not faded. The replacement already
		// carries this one's blend, so there is nothing to fade out, and
		// leaving it on the clump for even one frame is a slot the node
		// array does not have.
		DropAnimNow(old);
	}

	// Whatever base animation we last drove belongs to the old group now.
	player.appliedAnimId = ANIM_NONE;
}

void ApplyAnimation(RemotePlayer &player, void *ped) {
	void *clump = ClumpOf(ped);
	if (!clump)
		return;

	// Before anything else gets blended: the group decides which walk a
	// walk actually is.
	ApplyAnimGroup(player, ped, clump);

	const int pedGroup = Field<int32_t>(ped, offs::PED_ANIM_GROUP);

	if (player.last.animId != player.appliedAnimId &&
	    BlendRemoteAnim(player, clump, pedGroup, player.last.animId,
	                    player.last.animTime, player.last.animSpeed, true)) {
		player.appliedAnimId = player.last.animId;
	}

	// The overlay is driven every frame rather than on change, because
	// unlike a base animation it can end, freeze or be purged while the id
	// on the wire never moves.
	ApplyOverlay(player, clump, pedGroup);
}

// CWeaponInfo for a bounded weapon type. GetWeaponInfo is `imul eax,eax,54h
// / add eax,<table>` with no range check of its own, so the bound lives
// here instead.
void *WeaponInfo(uint8_t weaponType) {
	if (!IsInventoryWeapon(weaponType))
		return nullptr;
	using InfoFn = void *(__cdecl *)(int);
	return Func<InfoFn>(CWeaponInfo__GetWeaponInfo)(static_cast<int>(weaponType));
}

// Give the remote ped the weapon its owner is holding, and put the model in
// its hand. That last part is CPed::SetCurrentWeapon's job and can't be done
// by writing m_currentWeapon alone, since the model is an RwAtomic attached
// to the right-hand bone.
//
// The streaming check isn't optional. SetCurrentWeapon calls AddWeaponModel,
// which calls CreateInstance() on the weapon's model info - for a model the
// streamer hasn't loaded that instantiates a null clump. So instead the
// model gets requested and the change retried on a later frame, the same
// two-phase shape as spawning the ped itself (docs/protocol.md §1.6).
//
// Returns true once the ped is actually holding `want`, which is what makes
// this usable as a precondition for replaying a shot and not just a cosmetic
// update. False means "not yet": the model is still streaming, caller should
// try again next frame.
// Defined just below, and used from inside GiveWeaponTo: the two are the
// spawn half and the steady half of the same job.
void WriteSlotAmmo(void *ped, uint8_t slot, uint16_t clip, uint32_t total);

// The engine half of GiveWeaponTo, for any ped CoopIII drives: a remote
// player's, or a replica of somebody else's pedestrian.
bool PutWeaponInHand(void *ped, uint8_t want) {
	void *info = WeaponInfo(want);
	if (!info)
		return false;

	// -1 means the weapon puts nothing in the hand. CPed::AddWeaponModel
	// takes the same value and bails immediately on it (`cmp ebx,-1`).
	const int32_t model = Field<int32_t>(info, WEAPONINFO_MODEL_ID);
	if (model >= 0) {
		// HasModelLoaded is an unchecked index into ms_aInfoForModel. This id
		// comes from the game's own weapon table, not off the wire, so it
		// should always be in range - but "should" is exactly how the last
		// out-of-bounds read got written, and re3's MODELINFOSIZE (config.h:13)
		// says where the array actually ends.
		constexpr int32_t MODELINFO_SIZE = 5500;
		if (model >= MODELINFO_SIZE)
			return false;
		if (!HasModelLoaded(static_cast<uint32_t>(model))) {
			RequestModel(static_cast<uint16_t>(model));
			return false;   // retried next frame; appliedWeapon stays unset
		}
	}

	// With ammo sync off: the invented amount this file has always used. A
	// remote player's clip affects nothing anyone can see, and an empty one
	// leaves the weapon in WEAPONSTATE_OUT_OF_AMMO with the wrong idle pose,
	// so give enough that the engine treats the gun as usable. GiveWeapon
	// only adds on a weapon change and the engine caps the total at 99999.
	constexpr uint32_t REMOTE_AMMO = 1000;

	using GiveFn = uint32_t(__thiscall *)(void *, int, uint32_t);
	using SetFn  = void(__thiscall *)(void *, uint32_t);
	Func<GiveFn>(CPed__GiveWeapon)(ped, static_cast<int>(want), REMOTE_AMMO);
	Func<SetFn>(CPed__SetCurrentWeapon)(ped, want);
	return true;
}

bool GiveWeaponTo(RemotePlayer &player, void *ped, uint8_t want) {
	if (!IsInventoryWeapon(want))
		return false;
	if (want == player.appliedWeapon)
		return true;
	if (!PutWeaponInHand(ped, want))
		return false;
	player.appliedWeapon = want;

	// With it on, the invented amount is immediately overwritten with what
	// its owner says. GiveWeapon is still the call that makes the slot
	// exist: it runs CWeapon::Initialise on a slot the ped did not have, and
	// SetCurrentWeapon is what puts the model in the hand. Neither of them
	// is asked to be the source of the number.
	if (g_ammoSync)
		WriteSlotAmmo(ped, want, player.ammoClip[want], player.ammoTotal[want]);
	return true;
}

// Hold a remote ped's weapon slot at the count its owner reported.
//
// Called every time a pose is applied, which is 25 Hz, because this machine
// has two other writers of the same four bytes and neither of them is the
// authority:
//
//   CWeapon::Fire decrements m_nAmmoInClip at 0x0055C7D1 and m_nAmmoTotal at
//   0x0055C7E9 for every shot combat.cpp replays;
//
//   CCivilianPed::ProcessControl runs CWeapon::Update on the held slot every
//   frame, which fires CWeapon::Reload (0x005639D0) on its own CTimer
//   schedule and refills the clip out of the total.
//
// Neither is wrong to do it - they are the engine behaving normally - but
// the owner is authoritative for their own ped everywhere else in CoopIII
// and there is no reason for ammunition to be the exception. So the wire
// wins, restated often enough that nothing local can accumulate.
void WriteSlotAmmo(void *ped, uint8_t slot, uint16_t clip, uint32_t total) {
	void *const weapon = WeaponSlot(ped, slot);
	if (!weapon)
		return;
	// A slot the ped was never given. Writing into it would leave a CWeapon
	// whose m_eWeaponType does not match its index, which is what the engine
	// reads as HasWeapon.
	if (Field<uint32_t>(weapon, offs::WEAPON_TYPE) != slot)
		return;

	Field<int32_t>(weapon, offs::WEAPON_AMMO_IN_CLIP) = static_cast<int32_t>(clip);
	Field<int32_t>(weapon, offs::WEAPON_AMMO_TOTAL)   = static_cast<int32_t>(
	    total > 0x7FFFFFFFu ? 0x7FFFFFFF : total);

	// The state is derived rather than sent. WEAPONSTATE_RELOADING is a
	// deadline in CTimer::GetTimeInMilliseconds, which is a local clock that
	// pauses and gets rescaled (protocol.h, PacketHeader) - a remote
	// machine's copy of it would mean nothing here. What matters is the one
	// distinction anybody can see: a gun with nothing behind it is out of
	// ammo, and a gun with something behind it is ready to fire.
	Field<uint32_t>(weapon, offs::WEAPON_STATE) =
	    (clip == 0 && total == 0) ? WEAPONSTATE_OUT_OF_AMMO : WEAPONSTATE_READY;
}

void ApplyWeapon(RemotePlayer &player, void *ped) {
	GiveWeaponTo(player, ped, player.last.weapon);

	// The held slot, every tick, from the snapshot that carried it. Outside
	// GiveWeaponTo because that one returns early when the weapon has not
	// changed, and the count changes while the weapon does not - which is
	// what a firefight is.
	if (g_ammoSync && player.appliedWeapon == player.last.weapon &&
	    player.last.weapon < INVENTORY_SLOTS)
		WriteSlotAmmo(ped, player.last.weapon, player.last.ammoClip,
		              player.last.ammoTotal);
}

// A slot this player is carrying but not holding, off C_PlayerAmmo.
//
// The ped may not have the weapon at all, and giving it one costs nothing
// here: CPed::GiveWeapon on its own touches no model. Only
// CPed::SetCurrentWeapon instantiates an RwAtomic, and this deliberately
// does not call it - putting somebody's spare shotgun in their hands because
// they picked up shells for it is not what happened on their machine.
//
// Zero ammunition still gets the weapon given. The alternative is skipping
// it, and then a player who has fired their pistol dry stops existing as a
// pistol owner to everybody else - HasWeapon goes false and
// GET_AMMO_IN_CHAR_WEAPON answers 0 for the wrong reason.
void ApplyRemoteAmmoSlot(RemotePlayer &player, const AmmoSlotBody &slot) {
	void *const ped = ResolveRemote(player);
	if (!ped || !IsInventoryWeapon(slot.weapon))
		return;

	using GiveFn = uint32_t(__thiscall *)(void *, int, uint32_t);
	void *const weapon = WeaponSlot(ped, slot.weapon);
	if (!weapon)
		return;

	// They no longer have it - dropped on death, or taken by the script.
	//
	// The slot is emptied by hand rather than through an engine call. The
	// engine's own way back is CWeapon::Initialise, which also reloads and
	// touches the weapon model; all that is wanted here is for HasWeapon to
	// go false, and HasWeapon is `m_eWeaponType == index`. The weapon in the
	// hand is left alone - that one belongs to ApplyWeapon, which has the
	// model to tear down as well and gets its answer from the snapshot.
	if (!(slot.flags & AMMO_SLOT_OWNED)) {
		if (slot.weapon == player.appliedWeapon)
			return;
		Field<uint32_t>(weapon, offs::WEAPON_TYPE)        = WEAPONTYPE_UNARMED;
		Field<uint32_t>(weapon, offs::WEAPON_STATE)       = WEAPONSTATE_READY;
		Field<int32_t>(weapon, offs::WEAPON_AMMO_IN_CLIP) = 0;
		Field<int32_t>(weapon, offs::WEAPON_AMMO_TOTAL)   = 0;
		Field<uint32_t>(weapon, offs::WEAPON_TIMER)       = 0;
		return;
	}

	if (Field<uint32_t>(weapon, offs::WEAPON_TYPE) != slot.weapon)
		Func<GiveFn>(CPed__GiveWeapon)(ped, static_cast<int>(slot.weapon), 0u);

	WriteSlotAmmo(ped, slot.weapon, slot.clip, slot.total);
}

// bIsShooting - the state of holding the trigger down, not a shot itself.
//
// Worth being honest about what this does, because it used to claim more.
// The flag drives nothing visual. In the whole engine it is written in one
// place, the tail of CWeapon::Fire, and read in four, all of them script
// opcodes (IS_CHAR_SHOOTING_IN_AREA and its neighbours). It is not what puts
// a ped in a firing posture, and it is not what plays the firing animation:
// that's bIsAttacking, which CPed::FireGun acts on by actually firing the
// weapon, which is not an observer's decision to make.
//
// What makes a remote player look like they're shooting is the overlay
// animation in ApplyAnimation, which their own machine sampled off its own
// ped. This flag is mirrored anyway so a co-op mission script asking "is
// that character shooting" gets the same answer on every machine.
void ApplyFiring(RemotePlayer &player, void *ped) {
	uint8_t &flags = Field<uint8_t>(ped, offs::PED_FLAGS_C);
	if (player.last.flags & PF_FIRING)
		flags |= offs::PED_IS_SHOOTING;
	else
		flags = static_cast<uint8_t>(flags & ~offs::PED_IS_SHOOTING);
}

// Aim through CPed::SetAimFlag / ClearAimFlag, not by writing bIsAimingGun
// directly - SetAimFlag also works out AIMS_WITH_ARM from the current
// weapon's flags, so a pistol aims with the arm and a rifle with the torso
// without CoopIII having to know which is which.
//
// Yaw goes in through SetAimFlag. Pitch can't: CPed::AimGun passes a hard
// zero for anything that isn't PEDTYPE_PLAYER1..4, so it goes into
// g_replicaPitch instead and HookedPointGunInDirection swaps it in when
// AimGun runs later this frame.
void ApplyAim(RemotePlayer &player, void *ped) {
	// SetAimFlag reads GetWeapon()->m_eWeaponType and feeds it straight to
	// GetWeaponInfo, so the ped's current slot has to be in range first.
	if (Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON) >= offs::NUM_WEAPON_SLOTS)
		return;

	const bool aiming = (player.last.flags & PF_AIMING) != 0;
	if (aiming) {
		float yaw = 0.0f;
		FiniteOr(player.last.aimYaw, Field<float>(ped, offs::PED_ROT_CUR), yaw);
		using AimFn = void(__thiscall *)(void *, float);
		Func<AimFn>(CPed__SetAimFlag)(ped, WrapAngle(yaw));
		g_replicaPitch.Set(ped, player.last.aimPitch, FrameNow());
	} else if (Field<uint8_t>(ped, offs::PED_FLAGS_A) & offs::PED_IS_AIMING_GUN) {
		// Only when it is actually set: ClearAimFlag is what starts the
		// gun-lowering animation, and re-running it every frame would keep
		// restarting it.
		using ClearFn = void(__thiscall *)(void *);
		Func<ClearFn>(CPed__ClearAimFlag)(ped);
	}
}


// ---- fire on a remote player's body ---------------------------------------
//
// docs/roadmap.md §5.7 phase three, and one sentence covers the whole
// design: **the owner says whether they are burning, every observer lights
// its own copy, and nobody's fire but your own may cost you health.**
//
// The part §5.7 was worried about is the part that turned out not to exist.
// Its objection was that CFireManager::StartFire's ped arm runs SetFlee,
// SetMoveState(PEDMOVE_SPRINT), SetMoveAnim() and SetPedState(PED_ON_FIRE)
// straight into the pose stream CoopIII overwrites every frame. All four are
// real, and all four sit inside a branch the engine takes only for a ped
// that is not FindPlayerPed() - for the player it jumps the lot and lands on
// the shared tail (addresses.h has the six instructions). So the engine
// already has a way to burn a ped whose movement is not its to decide, and
// CoopIII does not have to invent one: LightRemoteFire below is that shared
// tail, transcribed in its order, with the branch not taken.
//
// That is also the answer to "which way does the information travel". One
// way, from the burning player outwards, as one bit in a flags byte that was
// already on the wire. An observer's fire writes nothing into the ped it
// burns: every frame ProcessFire reads the ped's matrix, asserts the
// back-pointer and calls CPed::InflictDamage, and the only write it has left
// is behind `if (InflictDamage(...))`. That call is refused twice over - by
// bFireProof, which is the first and only thing the cause-9 arm of
// InflictDamage tests, and by combat.cpp's detour, which refuses anything
// aimed at a remote player's ped before it looks at why - so the write
// behind it is unreachable by construction.

uint32_t NowMs() {
	return Global<uint32_t>(CTimer__m_snTimeInMilliseconds);
}

// Start a fire on this ped without waking the burning-ped AI.
//
// Every write here is one StartFire makes at 0x0047971E..0x00479884, in that
// order, and there are no others - notably field_20 and m_nFiremenPuttingOut
// keep whatever the last tenant of the slot left in them, because StartFire
// does not touch them either and only CFire::CFire ever has.
//
// One thing in the shared part is deliberately not done: at 0x004796C7 the
// engine registers an EVENT_PED_SET_ON_FIRE naming the entity that lit the
// victim, when there is one. Ours would say the remote ped set itself on
// fire, which is true of the pointer and false of the world. The ambient
// reaction is covered anyway - ReportThisFire below registers an EVENT_FIRE
// at the position, which is what makes bystanders notice a fire that is
// really there.
//
// m_pSource is the burning ped itself rather than nil or whoever lit them.
// Nil would make this terrain, and terrain ignores friendly fire: a teammate
// brushing past you would set you alight and kill you in a session with
// friendly fire off, because the fire ProcessFire spreads to the local
// player inherits this exact pointer and combat.cpp gates on it. Their ped
// is the honest answer for a fire spreading off their body, and it routes
// the spread through the rule that already exists (§1.10.6) instead of
// around it.
void *LightRemoteFire(void *ped) {
	using NextFreeFn = void *(__thiscall *)(void *);
	void *fire = Func<NextFreeFn>(CFireManager__GetNextFreeFire)(
	    reinterpret_cast<void *>(gFireManager));
	if (!fire)
		return nullptr;

	const uint32_t now = NowMs();
	const float   *pos = &Field<float>(ped, offs::POSITION);

	Field<uint8_t>(fire, FIRE_ONGOING) = 1;
	Field<uint8_t>(fire, FIRE_SCRIPT)  = 0;
	Field<float>(fire, FIRE_POS + 0)   = pos[0];
	Field<float>(fire, FIRE_POS + 4)   = pos[1];
	Field<float>(fire, FIRE_POS + 8)   = pos[2];

	// The engine picks 3333 ms for a player and ten seconds plus a random
	// spread for anyone else. An observer wants neither: this is a cap on
	// how long a fire may outlive the last snapshot that asked for it, and
	// the owner re-arms it every frame they are still alight.
	Field<uint32_t>(fire, FIRE_EXTINGUISH) = now + REMOTE_FIRE_MS;
	Field<uint32_t>(fire, FIRE_START_TIME) = now + 400;

	using RegisterFn = void(__thiscall *)(void *, void **);
	Field<void *>(fire, FIRE_ENTITY) = ped;
	Func<RegisterFn>(CEntity__RegisterReference)(ped, &Field<void *>(fire, FIRE_ENTITY));
	Field<void *>(fire, FIRE_SOURCE) = ped;
	Func<RegisterFn>(CEntity__RegisterReference)(ped, &Field<void *>(fire, FIRE_SOURCE));

	using ReportFn = void(__thiscall *)(void *);
	Func<ReportFn>(CFire__ReportThisFire)(fire);

	Field<uint32_t>(fire, FIRE_NEXT_FLAMES) = 0;
	Field<float>(fire, FIRE_STRENGTH)       = FIRE_PED_STRENGTH;
	Field<uint8_t>(fire, FIRE_PROPAGATION)  = 1;
	Field<uint8_t>(fire, FIRE_AUDIO_SET)    = 1;

	// The two-way link the engine asserts on every ProcessFire: a fire whose
	// entity has stopped pointing back at it puts itself out. StartFire
	// writes this before the tail; so does this.
	Field<void *>(ped, PED_FIRE) = fire;
	return fire;
}

// Is the fire currently on this ped the one we lit for this player?
//
// Asked of the slot's contents rather than of a remembered pointer. A slot
// gets re-let the moment its fire goes out, so "the fire at index N" is not
// an identity - "the fire at index N that is alight and still points at our
// ped" is.
bool FireIsOurs(const RemotePlayer &player, void *ped, void *fire) {
	return WatchedPedFireIsOurs(player.fireSlot, ped, fire);
}

// Four lines, one each, and between them the log answers the whole chain in
// a glance: did the owner say it, did the engine let us act on it, did
// anything else get there first, and did we run out of room. Same shape as
// the damage chain, and for the same reason - the round before last was lost
// to a path that failed silently at every step.
bool g_saidLit      = false;
bool g_saidNoSlot   = false;
bool g_saidNotOurs  = false;
bool g_saidWaiting  = false;

void ApplyRemoteFire(RemotePlayer &player, void *ped) {
	void      *fire = Field<void *>(ped, PED_FIRE);
	const bool want = (player.last.flags & PF_ON_FIRE) != 0;
	const bool ours = FireIsOurs(player, ped, fire);

	using InControlFn = bool(__thiscall *)(void *);
	const bool inControl = Func<InControlFn>(CPed__IsPedInControl)(ped);

	switch (PlanRemoteFire(want, fire != nullptr, ours, inControl)) {
	case FireAction::NOTHING:
		if (want && !g_saidWaiting) {
			g_saidWaiting = true;
			Log("fire: %s is burning but their ped is not in control here "
			    "(state %u, health %.0f), so there is nowhere to put the flame "
			    "yet. Trying again every frame",
			    player.nick.c_str(), Field<uint32_t>(ped, offs::PED_STATE),
			    Field<float>(ped, offs::PED_HEALTH));
		}
		return;

	case FireAction::LIGHT: {
		void *lit = LightRemoteFire(ped);
		if (!lit) {
			player.fireSlot = -1;
			if (!g_saidNoSlot) {
				g_saidNoSlot = true;
				Log("fire: all %u fire slots are taken, so %s burns on their own "
				    "screen and not on this one",
				    static_cast<unsigned>(NUM_FIRES), player.nick.c_str());
			}
			return;
		}
		player.fireSlot = static_cast<int8_t>(FireSlotIndex(lit));
		if (!g_saidLit) {
			g_saidLit = true;
			Log("fire: %s is on fire, lit our own copy on their ped in slot %d. "
			    "No flee, no sprint, no PED_ON_FIRE - their pose still comes off "
			    "the wire and their health is still theirs to decide",
			    player.nick.c_str(), static_cast<int>(player.fireSlot));
		}
		return;
	}

	case FireAction::KEEP:
		// Push the cap back rather than rebuilding the fire. ProcessFire is
		// already deriving its position from the ped's matrix every frame,
		// so there is nothing else about it that goes stale.
		Field<uint32_t>(fire, FIRE_EXTINGUISH) = NowMs() + REMOTE_FIRE_MS;
		return;

	case FireAction::EXTINGUISH: {
		if (!ours && !g_saidNotOurs) {
			g_saidNotOurs = true;
			Log("fire: something in our own engine set %s's ped alight (ped state "
			    "%u). That fire comes with SetFlee and PED_ON_FIRE attached, so it "
			    "is going out; if they really are burning we relight it ourselves "
			    "next frame",
			    player.nick.c_str(), Field<uint32_t>(ped, offs::PED_STATE));
		}
		using ExtinguishFn = void(__thiscall *)(void *);
		Func<ExtinguishFn>(CFire__Extinguish)(fire);
		player.fireSlot = -1;
		return;
	}
	}
}

// ---- seating a remote ped in a car ----------------------------------------
//
// COMMAND_WARP_CHAR_INTO_CAR's own order: SetObjective, then WarpPedIntoCar.
// The order matters functionally, not just stylistically - WarpPedIntoCar
// branches on m_objective, and given anything other than one of the two
// ENTER_CAR objectives it still sets bInVehicle and PED_DRIVING and then
// returns having assigned no seat at all. addresses.h has the disassembly
// that shows this.
//
// That warp is no longer the way a remote player normally gets into a car -
// it is what happens when the real way cannot be made to work. The real way
// is CPed::SetEnterCar, further down, and the two have to live beside each
// other because the first thing the animated entry needs is somewhere to
// fail to.
//
// This comment used to say the animated entry was skipped on purpose,
// because "an observer cannot drive a multi-second negotiation off a stream
// of events that just says they're in the car now". Half of that is still
// true and is the reason for every guard below: the entry really can be
// refused or interrupted at any point, silently. What was wrong was the
// conclusion. An observer does not have to *drive* the negotiation - it
// starts it, watches it, and takes the seat by force if it does not finish.
// The seat is guaranteed either way; only the door opening is optional.

// Which passenger slot holds this ped, or -1. Bounded by the car's own
// m_nNumMaxPassengers, capped at the array's actual length - that field is
// just a byte the handling data fills in, and the array is eight pointers
// no matter what it says. Declared early because the seating path uses this
// to undo a warp that half-applied.
void UnseatRemotePed(RemotePlayer &player);

// The engine halves of the two above, on raw CPed / CVehicle pointers.
//
// Split out rather than copied when the ambient population seam needed to
// seat a traffic driver (docs/population.md §3 step 6). Everything in here
// is about a ped and a car; everything left in the two functions below is
// about a RemotePlayer's bookkeeping and its log line. A third hand-written
// copy of `SetObjective then WarpPedIntoCar` is a third place to get the
// order wrong, and getting it wrong is a ped who believes he is in a car
// that has never heard of him.
bool SeatPedInCar(void *ped, void *car, uint8_t seat);
int  UnseatPedFromCar(void *ped);

int PassengerSlotOf(void *car, void *ped) {
	void *const   *seats = &Field<void *>(car, offs::VEH_PASSENGERS);
	const uint8_t  max   = Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS);
	const uint8_t  n = max < offs::VEH_MAX_PASSENGERS ? max : offs::VEH_MAX_PASSENGERS;
	for (uint8_t i = 0; i < n; ++i)
		if (seats[i] == ped)
			return i;
	return -1;
}

// Whoever is already in that seat, taken out of it before somebody else is
// put in. Returns true if it had to move anyone.
//
// The engine will not do this for you. CVehicle::SetDriver is two stores and
// a RegisterReference; it overwrites pDriver and never looks at the ped that
// was there, which is then left with bInVehicle set, m_pMyVehicle pointing
// at a car that has never heard of it, and PED_DRIVING. CWorld::Process then
// calls SetPedPositionInCar on it every frame, asking a car which seat this
// ped is in and being told none.
//
// This is not the observer deciding that somebody left a car. It is the
// observer making room for a seating the session has already stated: two
// peds cannot both be the driver, and the one the session names wins. The
// player who lost the seat gets their own exit event a moment later and the
// two agree from then on.
//
// **Never the local player, and that exception is the whole of a bug.** Every
// sentence above is about replicas. Applied to the person playing the game it
// says something entirely different: that a statement about where somebody
// else's ped is sitting may reach into this engine and tear the player out of
// a car he is driving - RemoveDriver, STATUS_ABANDONED, engine off, both
// velocities zeroed, bInVehicle false, PED_IDLE - and then put a CCivilianPed
// in his seat. From that frame on his car is not his: m_pDriver names a
// replica, so every ownership guard in the vehicle seam reads false, and
// Client::UpdateRemoteVehicles starts writing the previous driver's throttle,
// gear and m_vecMoveSpeed onto it before each frame's physics. The car drives
// off with no key pressed, or refuses to move, depending on what that last
// snapshot happened to hold. It is also why he cannot get out: the only way
// out of a car is CPed::SetExitCar, whose first act is CVehicle::CanPedExitCar,
// which refuses any car whose m_vecMoveSpeed magnitude-squared is over 0.005 -
// eight times tighter than the gate on the way in (addresses.h).
//
// And it needs no exotic trigger. A remote player's replica getting reaped and
// respawned clears Client's record of where it was sitting, so the next pass
// re-states the seating; a replicated traffic driver's seat is re-stated on
// every stream batch by design (docs/population.md §3 step 6). Either one lands
// on whichever car the local player happens to be driving at the time, if it is
// the car the session says that ped is in.
//
// The seating is refused rather than forced, so the caller reports failure and
// the ped stays on foot. A remote player standing beside his own car is a
// cosmetic disagreement for as long as this machine has the local player in it;
// the local player being thrown out of a car is not.
//
// The session *can* take a car off the local player - a carjack is exactly
// that - but not through here. That goes through the server's arbitration and
// WorldBridge::SurrenderVehicleSeat, which is one statement about one car
// rather than a side effect of seating a ped.
void *SeatOccupant(void *car, uint8_t seat) {
	if (seat == 0)
		return Field<void *>(car, offs::VEH_DRIVER);
	const size_t slot = static_cast<size_t>(seat) - 1;
	if (slot < offs::VEH_MAX_PASSENGERS)
		return (&Field<void *>(car, offs::VEH_PASSENGERS))[slot];
	return nullptr;
}

// Is the person playing the game in this seat?
//
// Checked by both ways of seating a ped rather than only by the eviction,
// because refusing to *empty* the seat is not enough on its own: both paths end
// at CVehicle::SetDriver, which is two stores and a RegisterReference and
// overwrites pDriver without looking at who was there. The whole seating has to
// be refused, so the answer is a predicate rather than a return code.
bool SeatHeldByLocalPlayer(void *car, uint8_t seat) {
	void *const occupant = SeatOccupant(car, seat);
	if (!occupant || occupant != PlayerPed())
		return false;

	static bool said = false;
	if (!said) {
		said = true;
		Log("bridge: refused to seat a replica in seat %u - the local player is "
		    "in it. The session saying somebody else is sitting there is a "
		    "statement about a replica and never a reason to empty the player's "
		    "seat (and this will not be said again)",
		    seat);
	}
	return true;
}

bool EvictSeatOccupant(void *car, uint8_t seat, void *incoming) {
	void *const occupant = SeatOccupant(car, seat);
	if (!occupant || occupant == incoming)
		return false;
	if (occupant == PlayerPed())
		return false;   // SeatHeldByLocalPlayer has already refused the seating

	UnseatPedFromCar(occupant);
	return true;
}

bool SeatPedInCar(void *ped, void *car, uint8_t seat) {
	if (!ped || !car)
		return false;

	// Never over the top of the local player. EvictSeatOccupant is where the
	// whole of this is argued; the short version is that WarpPedIntoCar ends at
	// CVehicle::SetDriver, which would take the wheel off him whether or not
	// the eviction ran.
	if (SeatHeldByLocalPlayer(car, seat))
		return false;

	EvictSeatOccupant(car, seat, ped);

	// A dead ped can't be seated. Failing quietly here beats failing inside
	// the warp: CPed::SetObjective returns on PED_DIE/PED_DEAD before writing
	// anything, so the objective would stay whatever it already was and
	// WarpPedIntoCar would take its no-seat branch.
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return false;

	const uint32_t objective = seat == 0 ? OBJECTIVE_ENTER_CAR_AS_DRIVER
	                                     : OBJECTIVE_ENTER_CAR_AS_PASSENGER;

	using ObjectiveFn = void(__thiscall *)(void *, uint32_t, void *);
	using WarpFn      = void(__thiscall *)(void *, void *);
	Func<ObjectiveFn>(CPed__SetObjective)(ped, objective, car);

	// A boat. SetObjective undoes an ENTER_CAR objective on a boat for any ped
	// that is not the player (0x004D8519, addresses.h "boats"), so the warp
	// below would read whatever objective came before - OBJECTIVE_NONE for a
	// replica - and take its no-seat arm. The objective is the only thing the
	// warp reads; it writes m_pMyVehicle and m_carInObjective itself, with
	// their references. So it is put back by hand, and only when the engine
	// took it away.
	if (SetObjectiveRefusesNonPlayer(Field<int32_t>(car, offs::VEH_TYPE)) &&
	    Field<uint32_t>(ped, offs::PED_OBJECTIVE) != objective) {
		Field<uint32_t>(ped, offs::PED_OBJECTIVE) = objective;
		static bool said = false;
		if (!said) {
			said = true;
			Log("bridge: seating a replica in a boat - CPed::SetObjective "
			    "refuses that for anybody but the player, so the objective "
			    "the warp reads was written directly (and this will not be "
			    "said again)");
		}
	}

	// The car's status, read before the warp writes its own over it
	// (0x004D7E9B: PHYSICS for anybody who isn't a player, in both seat arms,
	// and in the passenger arm even when every slot was full and nobody got
	// one). Put back straight after, whether the seating took or not.
	//
	// A passenger's warp leaves the car as it was. The one that mattered is
	// the local player's own car with a remote player getting in beside him:
	// it went from PLAYER to PHYSICS, the PLAYER arm that reads his pad
	// stopped running, and the car AI drove it instead - MISSION_NONE's brake
	// and handbrake, or the cruise of a traffic car he'd taken. game/carstatus.h
	// has the rest.
	uint8_t &flags = Field<uint8_t>(car, offs::ENTITY_FLAGS);
	const uint8_t before = static_cast<uint8_t>(flags >> ENTITY_STATUS_SHIFT);

	Func<WarpFn>(CPed__WarpPedIntoCar)(ped, car);

	// Did it actually take? The warp fails silently and half-applied, so
	// what gets checked here is the seat pointer - the thing it would have
	// skipped on failure.
	const bool seated = seat == 0 ? Field<void *>(car, offs::VEH_DRIVER) == ped
	                              : PassengerSlotOf(car, ped) >= 0;

	// A seating that didn't take changes nothing about the car either.
	const uint8_t after = seated ? StatusAfterSeating(before, seat == 0) : before;
	flags = static_cast<uint8_t>((flags & 0x07u) | (after << ENTITY_STATUS_SHIFT));

	if (!seated) {
		UnseatPedFromCar(ped);
		return false;
	}
	return true;
}

bool SeatRemotePed(RemotePlayer &player, RemoteVehicle &vehicle, uint8_t seat) {
	void *const ped = ResolveRemote(player);
	if (!ped)
		return false;
	void *const car = ResolveRemoteVehicle(vehicle);
	if (!car)
		return false;

	if (SeatPedInCar(ped, car, seat))
		return true;

	Log("bridge: %s did not take seat %u of vehicle %u; putting them back "
	    "on foot",
	    player.nick.c_str(), seat, vehicle.netId);
	// SeatPedInCar already put the engine state back; this is the
	// RemotePlayer bookkeeping half, which it knows nothing about.
	UnseatRemotePed(player);
	return false;
}

// The other direction. There's no WarpPedOutOfCar to call - the sequence
// lives open-coded inside COMMAND_WARP_CHAR_FROM_CAR_TO_COORD's handler.
// This is that sequence, minus the teleport (the pose stream decides where
// the ped goes) and minus CPed::RemoveInCarAnims (player-only, addresses.h
// explains why).
//
// Safe to call on a ped who isn't in a car at all, which is what lets it
// double as a plain "make sure they're on foot".
int UnseatPedFromCar(void *ped) {
	if (!ped)
		return 0;

	// "In a vehicle" and "has a vehicle" are two separate questions, and both
	// need asking. When a car gets destroyed under a seated ped, the
	// reference WarpPedIntoCar registered goes to null but nothing clears
	// bInVehicle - trust the flag alone and you dereference null, trust the
	// pointer alone and you act on a car this ped got out of long ago.
	void *const car = Field<bool>(ped, offs::PED_IN_VEHICLE)
	                      ? Field<void *>(ped, offs::PED_MY_VEHICLE)
	                      : nullptr;

	// Every vehicle CoopIII creates is a CAutomobile or a CBoat (vehicle.cpp,
	// game/boat.h), so this vtable check is the same net ResolveRemote uses on
	// peds - a slot that still resolves while no longer holding what we think
	// it does. Everything written below is CVehicle's or CPhysical's, so it is
	// valid on both. Leaving the boat out would skip RemoveDriver for a boat,
	// and destroying it would then leave its pDriver pointing at our ped.
	if (car && IsBuiltVehicleVtable(Field<uintptr_t>(car, offs::VTABLE))) {
		if (Field<void *>(car, offs::VEH_DRIVER) == ped) {
			Func<void(__thiscall *)(void *)>(CVehicle__RemoveDriver)(car);

			// SetStatus keeps m_type in bits 0-2, replaces m_status in 3-7 -
			// same `and al,7 / or al,20h` the spawn path uses.
			uint8_t &status = Field<uint8_t>(car, offs::ENTITY_FLAGS);
			status          = static_cast<uint8_t>(
                (status & 0x07u) |
                (ENTITY_STATUS_ABANDONED << ENTITY_STATUS_SHIFT));

			uint8_t &flagsA = Field<uint8_t>(car, offs::VEH_FLAGS_A);
			flagsA = static_cast<uint8_t>(flagsA & ~offs::VEH_ENGINE_ON);

			Field<uint8_t>(car, offs::AUTOPILOT_CRUISE_SPEED) = 0;
		} else {
			Func<void(__thiscall *)(void *, void *)>(CVehicle__RemovePassenger)(
			    car, ped);
		}

		// The script's handler stops the car whoever got out of it. Not the
		// local player's: a remote passenger stepping out of a car he is
		// driving would otherwise stop it dead under him, at whatever speed
		// the passenger's own machine let him leave at.
		void *const local = PlayerPed();
		if (!local || Field<void *>(car, offs::VEH_DRIVER) != local) {
			float *const move = &Field<float>(car, offs::MOVE_SPEED);
			move[0] = 0.0f;
			move[1] = 0.0f;
			move[2] = VEH_EXIT_SETTLE_SPEED_Z;
			float *const turn = &Field<float>(car, offs::TURN_SPEED);
			turn[0] = turn[1] = turn[2] = 0.0f;
		}
	}

	Field<bool>(ped, offs::PED_IN_VEHICLE)     = false;
	Field<void *>(ped, offs::PED_MY_VEHICLE)   = nullptr;
	Field<uint32_t>(ped, offs::PED_STATE)      = PEDSTATE_IDLE;
	Field<uint32_t>(ped, offs::PED_LAST_STATE) = PEDSTATE_NONE;
	Field<uint8_t>(ped, offs::ENTITY_FLAGS_A) |= offs::ENTITY_USES_COLLISION;

	float *const vel = &Field<float>(ped, offs::MOVE_SPEED);
	vel[0] = vel[1] = vel[2] = 0.0f;

	// The get-in/get-out association, torn down the way the handler does it -
	// a blend delta that large finishes the fade in one frame. A warped ped
	// never played one of these, so this branch is basically always a no-op,
	// but the field belongs to the seat and it costs nothing to leave nothing
	// behind.
	if (void *const anim = Field<void *>(ped, offs::PED_VEHICLE_ANIM)) {
		Field<float>(anim, ANIM_BLEND_DELTA)       = -1000.0f;
		Field<void *>(ped, offs::PED_VEHICLE_ANIM) = nullptr;
	}

	// The objective goes with the seat, and this is a part the handler does
	// *not* do - the script always follows the warp with something that sets
	// one, so it can afford to leave ENTER_CAR_AS_DRIVER standing. CoopIII
	// can't afford that: a CCivilianPed left holding that objective and a car
	// pointer walks back to the car and tries to get back in, fighting the
	// pose stream the whole way. Written directly rather than routed through
	// CPed::SetObjective, because what's wanted is a flat "no objective," not
	// whatever SetObjective would restore instead.
	//
	// It is also, as of 2026-09-22, the answer to docs/protocol.md §6's
	// first open question. CPed::ProcessObjective's own guard is
	// `cmp dword [ebx+164h],0 / je` - a ped holding OBJECTIVE_NONE runs no
	// objective at all, whatever its state, whether or not ProcessControl
	// runs. This write and the PED_IDLE one above are the whole of "a
	// remote ped must not run its own AI". Do not remove either as
	// housekeeping; §1.13.3 is why.
	Field<uint32_t>(ped, offs::PED_OBJECTIVE)      = OBJECTIVE_NONE;
	Field<uint32_t>(ped, offs::PED_PREV_OBJECTIVE) = OBJECTIVE_NONE;
	Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE) = nullptr;

	// Nothing driven into this ped survived the trip - the seat changed its
	// state and its animations, and the weapon model may have gone with them.
	// Forgetting what was applied makes the next frame re-drive all of it.
	//
	// Forgetting is not enough for the partials, though. The steering
	// animation belonged to the engine rather than to us, so there is no id
	// here to fade and ApplyOverlay would leave it running forever. See
	// FadeOutAllPartials.
	void *const clump = ClumpOf(ped);
	return clump ? FadeOutAllPartials(clump) : 0;
}

void UnseatRemotePed(RemotePlayer &player) {
	void *const ped = ResolveRemote(player);
	if (!ped)
		return;

	const int dropped = UnseatPedFromCar(ped);

	// Once, because a stuck steering pose is exactly the kind of thing that
	// gets reported as "sometimes the driving animation stays on forever"
	// and is impossible to place afterwards.
	static bool said = false;
	if (dropped > 0 && !said) {
		said = true;
		Log("bridge: %s left a seat still carrying %d partial animation(s); "
		    "faded them out, because the engine's steering pose is not one "
		    "we applied and nothing else would have removed it",
		    player.nick.c_str(), dropped);
	}

	player.appliedWeapon  = 0xFFFF;
	player.appliedAnimId  = ANIM_NONE;
	player.appliedAnimId2 = ANIM_NONE;
	player.appliedDriveBy = ANIM_NONE;
	player.driveByArmed   = false;
}

// ---- and the same two with the door open ----------------------------------
//
// CPed::SetEnterCar and CPed::SetExitCar. Everything above this line happens
// in one call; everything below takes about a second, can be refused without
// saying so, and is driven to a deadline by Client::UpdateRemoteSeats with
// the warp standing behind it.
//
// Three facts out of addresses.h shape all of it:
//
//  1. Neither call *does* the entry. They put the ped into PED_ENTER_CAR or
//     PED_EXIT_CAR and hang an animation on it; CWorld::Process's walk over
//     the moving list is what calls EnterCar() / ExitCar() every frame after
//     that, and a chain of animation-finish callbacks ends with the seat
//     being assigned. So there is no return value to read and no callback to
//     hook - the only honest way to know how it went is to look at the ped.
//
//  2. SetEnterCar refuses silently. Health, IsPedInControl, the door's two
//     busy flags, bIsBeingCarJacked and m_pVehicleAnim are each enough for
//     it to fall through to SetMoveState(PEDMOVE_STILL) and return, leaving
//     the ped standing exactly where it was with nothing to show for the
//     call. Hence the guards below: not because the engine would crash, but
//     because a refusal that is noticed here becomes a warp this frame
//     instead of a ped standing still until the deadline runs out.
//
//  3. Giving up costs a call. An entry dropped without QuitEnteringCar
//     leaves the door's bit set in m_nGettingInFlags and m_nNumGettingIn
//     counting somebody who is not coming - and that flag is the first thing
//     SetEnterCar tests, so the door is then refused to everybody for the
//     rest of the car's life. That is the door-shaped version of the stuck
//     driving animation FadeOutAllPartials exists for, and it is why
//     AbandonSeatRemotePed is on the bridge rather than being something the
//     client could forget.

// Which door belongs to which seat. Not a choice: SetExitCar picks the door
// from the seat the ped is sitting in, and this is that mapping read
// backwards, so the two directions agree by construction.
uint16_t DoorForSeat(uint8_t seat) {
	switch (seat) {
	case 0:  return offs::CAR_DOOR_LF;   // pDriver
	case 1:  return offs::CAR_DOOR_RF;   // pPassengers[0]
	case 2:  return offs::CAR_DOOR_LR;   // pPassengers[1]
	case 3:  return offs::CAR_DOOR_RR;   // pPassengers[2]
	default: return 0;                   // no door of its own, so no animation
	}
}

uint8_t DoorFlag(uint16_t door) {
	switch (door) {
	case offs::CAR_DOOR_LF: return offs::CAR_DOOR_FLAG_LF;
	case offs::CAR_DOOR_LR: return offs::CAR_DOOR_FLAG_LR;
	case offs::CAR_DOOR_RF: return offs::CAR_DOOR_FLAG_RF;
	case offs::CAR_DOOR_RR: return offs::CAR_DOOR_FLAG_RR;
	default:                return 0;
	}
}

// How far a car may be drifting and still be worth walking up to, squared.
//
// The entry animation lines the ped up against the car's door over several
// frames, and a car that is going anywhere leaves it behind - the ped ends
// up being dragged along beside a moving vehicle, which looks far worse than
// appearing in the seat.
//
// This used to be sq(0.5f), chosen as "the width of parked, settling on its
// suspension", and it was a guess that was looser than the engine's. There
// are two gates inside the entry and both are 0.2 m/s: CVehicle::CanPedEnterCar
// tests sq(0.2f) on the move and turn speeds before SeekCar hands over, and
// the door-opening callback tests the magnitude against 0.2f again at
// 0x004DE758 - and *that* one does not refuse the entry, it calls
// QuitEnteringCar and SetFall(1000). A car creeping at 0.3 m/s passed this
// check, the ped walked up to it, and the engine knocked him into the road.
// addresses.h, PED_ENTER_MAX_SPEED_SQ, has both disassemblies.
constexpr float ENTER_MAX_CAR_SPEED_SQ = PED_ENTER_MAX_SPEED_SQ;
static_assert(ENTER_MAX_CAR_SPEED_SQ == VEH_ENTER_MAX_SPEED_SQ,
              "the pre-check and CVehicle::CanPedEnterCar must gate on the "
              "same speed, or the walk starts and the entry is then refused");

// And how far away the ped may be, squared. Beyond this the engine walks
// them across the street to the handle, which is a second of a remote player
// moving somewhere their owner never went.
constexpr float ENTER_MAX_PED_DIST_SQ = 8.0f * 8.0f;

float DistanceSq(const float *a, const float *b) {
	const float dx = a[0] - b[0];
	const float dy = a[1] - b[1];
	const float dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

// Is this ped in the seat we asked for?
//
// The driver's seat is exact. A passenger's is not, and deliberately so:
// PedSetInCarCB puts a passenger in the slot its door names and the warp
// path takes the first free slot, so "somewhere in this car" is the same
// standard SeatPedInCar has always held itself to. Tightening it here and
// not there would just mean the animation reports failure for a seating the
// warp would have called a success.
bool PedIsInSeat(void *ped, void *car, uint8_t seat) {
	return seat == 0 ? Field<void *>(car, offs::VEH_DRIVER) == ped
	                 : PassengerSlotOf(car, ped) >= 0;
}

bool PedIsEnteringCar(void *ped, void *car) {
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state != PEDSTATE_ENTER_CAR && state != PEDSTATE_CARJACK)
		return false;
	// m_pMyVehicle is set by SetEnterCar_AllClear and nilled by the
	// reference the car registered on it, so this also answers "is the car
	// this entry was for still there".
	return Field<void *>(ped, offs::PED_MY_VEHICLE) == car;
}

// The objective triple, cleared. Written out rather than routed through
// CPed::ClearObjective for the reason UnseatPedFromCar gives: what is wanted
// is a flat "no objective", not whatever SetObjective would restore instead,
// and OBJECTIVE_NONE is half of the off switch for a remote ped's AI
// (docs/protocol.md §1.13.3).
void ClearPedObjective(void *ped) {
	Field<uint32_t>(ped, offs::PED_OBJECTIVE)      = OBJECTIVE_NONE;
	Field<uint32_t>(ped, offs::PED_PREV_OBJECTIVE) = OBJECTIVE_NONE;
	Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE) = nullptr;
}

// Shut a door an abandoned entry left hanging open.
//
// QuitEnteringCar makes no call on the car at all - there is no
// `call [reg+5Ch]` anywhere in 0x004E0E00..0x004E0F96 - so it hands back the
// door's bit in m_nGettingInFlags and leaves the door itself wherever the
// animation had swung it to. With nobody in the seat, nothing ever swings it
// back: only somebody else's get-in or get-out touches that door again, and
// until then the car sits in the street with a door open and no driver.
//
// One call fixes it, and it is the engine's own: ProcessOpenDoor through
// vtable slot 0x5C with the closing animation and a time past its end, which
// is exactly what CPed::PedAnimDoorCloseCB does at the end of every real
// get-in. addresses.h, ANIM_STD_CAR_CLOSE_DOOR_LHS, has the disassembly for
// the id and for what 1.0f does once it is in there.
//
// Called with the car and door read off the ped BEFORE QuitEnteringCar runs,
// because there is no promise about what it leaves behind on them.
void ShutDoorAfterAbandonedEntry(void *car, uint16_t door) {
	if (!car || !door)
		return;
	// Only a CAutomobile has doors that swing. CVehicle::ProcessOpenDoor is a
	// do-nothing in the base class and a boat gets the base one, so this is
	// about not relying on that rather than about avoiding a crash.
	if (Field<uintptr_t>(car, offs::VTABLE) != CAutomobile__vtable)
		return;

	const uintptr_t vt = Field<uintptr_t>(car, offs::VTABLE);
	using OpenDoorFn   = void(__thiscall *)(void *, uint32_t, uint32_t, float);
	const auto fn      = *reinterpret_cast<OpenDoorFn *>(vt + VEH_VT_PROCESS_OPEN_DOOR);
	if (!fn)
		return;
	fn(car, door, ANIM_STD_CAR_CLOSE_DOOR_LHS, 1.0f);
}

int AbandonPedEnterCar(void *ped) {
	if (!ped)
		return 0;

	// Read first: the entry is about to be taken off the ped, and the door
	// has to be shut with the pair the ped was using when it still had them.
	void *const    car  = Field<void *>(ped, offs::PED_MY_VEHICLE);
	const uint16_t door = Field<uint16_t>(ped, offs::PED_VEH_DOOR);

	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	const bool     wasEntering =
	    state == PEDSTATE_ENTER_CAR || state == PEDSTATE_CARJACK;
	if (wasEntering)
		Func<void(__thiscall *)(void *)>(CPed__QuitEnteringCar)(ped);

	// And put the door back. Only for an entry that was actually running:
	// the ped's m_vehDoor is not cleared when one ends, so a ped that was
	// standing around holding a stale door would otherwise have this reach
	// into a car it has nothing to do with and slam a door somebody else is
	// halfway through opening.
	if (wasEntering)
		ShutDoorAfterAbandonedEntry(car, door);

	// QuitEnteringCar nils m_pVehicleAnim and restores bUsesCollision, and
	// leaves the ped idle. It does not clear the objective - the engine's
	// own callers always follow it with something that sets one, and we
	// cannot afford to.
	ClearPedObjective(ped);

	// The same wholesale partial fade the exit does, for the same reason:
	// the door-opening chain is not made of animations CoopIII applied, so
	// there is no id to fade and nothing else would remove one that is left
	// over. Cheap, and this is the path that runs when something has already
	// gone differently from the plan.
	void *const clump = ClumpOf(ped);
	return clump ? FadeOutAllPartials(clump) : 0;
}

bool BeginPedEnterCar(void *ped, void *car, uint8_t seat, uint8_t doorSeat) {
	if (!ped || !car)
		return false;

	// Not a replica into a boat. The engine's boat entry is reached through
	// the ENTER_CAR objective, which SetObjective refuses on a boat for
	// anybody but the player (addresses.h, "boats"), so the animation would
	// start with no seat at the end of it. Refused here, the caller warps,
	// and SeatPedInCar puts the objective back for the warp.
	//
	// The local player is let through. seat.cpp's StartCarEntry comes here
	// for his own passenger entry, and for him the objective holds.
	if (ped != PlayerPed() &&
	    !ReplicaMayAnimateEntry(Field<int32_t>(car, offs::VEH_TYPE)))
		return false;

	// A door of its own, or there is no animation to play. Seats past the
	// fourth share the rear doors and the engine picks for itself; rather
	// than guess, those are warped.
	if (!DoorForSeat(seat))
		return false;

	// The door the ped goes in THROUGH, which is not always the seat's own.
	//
	// The engine walks a driver to the *nearest* door (CPed::SeekCar ->
	// CPed::GetNearestDoor, addresses.h) and shuffles him across the front
	// seats inside the car. So an entry into seat 0 through the front-right
	// door is an ordinary thing the engine does every time somebody presses
	// the enter key on the passenger side, and a replica told only the seat
	// opens the wrong door and gets dragged round the car to it. The caller
	// says which door; it defaults to the seat's own, which is what every
	// entry CoopIII starts by itself uses.
	const uint16_t door = DoorForSeat(doorSeat);
	const uint8_t  flag = DoorFlag(door);
	if (!door || !flag)
		return false;

	// Everything SetEnterCar itself would refuse on, asked first so a
	// refusal becomes a warp this frame rather than a ped standing still
	// until the deadline. In the engine's own order.
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return false;
	if (Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;   // already in something; the caller takes them out first
	if (Field<void *>(ped, offs::PED_VEHICLE_ANIM))
		return false;
	if (Field<float>(ped, offs::PED_HEALTH) <= 0.0f)
		return false;
	if (!ClumpOf(ped))
		return false;

	const uint8_t gettingIn  = Field<uint8_t>(car, offs::VEH_GETTING_IN_FLAGS);
	const uint8_t gettingOut = Field<uint8_t>(car, offs::VEH_GETTING_OUT_FLAGS);
	if ((gettingIn & flag) || (gettingOut & flag))
		return false;
	if (Field<uint8_t>(car, offs::VEH_FLAGS_C) & offs::VEH_IS_BEING_CARJACKED)
		return false;

	// Two of our own, which the engine does not check because in single
	// player nobody ever asks a ped to get into a car that is 30 metres away
	// and accelerating.
	const float *const carPos   = &Field<float>(car, offs::POSITION);
	const float *const pedPos   = &Field<float>(ped, offs::POSITION);
	const float *const carSpeed = &Field<float>(car, offs::MOVE_SPEED);
	const float        speedSq  = carSpeed[0] * carSpeed[0] +
	                      carSpeed[1] * carSpeed[1] + carSpeed[2] * carSpeed[2];
	if (!(speedSq <= ENTER_MAX_CAR_SPEED_SQ))
		return false;   // written to catch a NaN as well as a fast car
	if (DistanceSq(pedPos, carPos) > ENTER_MAX_PED_DIST_SQ)
		return false;

	// Not into a seat the local player is in. Same rule as the warp's, and the
	// same reason: this entry ends in PedSetInCarCB calling CVehicle::SetDriver,
	// so letting it start would take the wheel off him a second from now
	// instead of immediately. EvictSeatOccupant has the argument.
	if (SeatHeldByLocalPlayer(car, seat))
		return false;

	// Make room, the same way the warp does, and for the same reason: two
	// peds cannot both be the driver.
	EvictSeatOccupant(car, seat, ped);

	// The objective first. SetEnterCar_AllClear reads it to decide whether
	// the right-front door counts as a jack, and PedSetInCarCB reads it
	// again at the end to decide between SetDriver and AddPassenger - so
	// this is what actually chooses the seat, exactly as it is for the warp.
	//
	// It also means m_objective is not OBJECTIVE_NONE for the length of the
	// animation, which is the one window in which a remote ped runs
	// CPed::ProcessObjective. That arm is `if (IsPedInControl())` and
	// IsPedInControl is false in PED_ENTER_CAR, so it does nothing; and
	// PedSetInCarCB ends with RestorePreviousObjective, which puts back the
	// m_prevObjective SetObjective saved - OBJECTIVE_NONE, because that is
	// what UnseatPedFromCar leaves behind. The off switch survives the trip.
	const uint32_t objective = seat == 0 ? OBJECTIVE_ENTER_CAR_AS_DRIVER
	                                     : OBJECTIVE_ENTER_CAR_AS_PASSENGER;
	using ObjectiveFn = void(__thiscall *)(void *, uint32_t, void *);
	Func<ObjectiveFn>(CPed__SetObjective)(ped, objective, car);

	// m_vehDoor is an input, not an output: SetEnterCar switches on it to
	// find the door flag and the door node, and SetEnterCar_AllClear writes
	// the argument straight back into it. A word, not a dword.
	Field<uint16_t>(ped, offs::PED_VEH_DOOR) = door;

	using EnterFn = void(__thiscall *)(void *, void *, uint32_t);
	Func<EnterFn>(CPed__SetEnterCar)(ped, car, door);

	// Did it take? This is the whole reason the call is wrapped: it has no
	// return value and its refusal is a move-state write.
	if (!PedIsEnteringCar(ped, car)) {
		ClearPedObjective(ped);
		return false;
	}
	return true;
}

uint8_t PollPedEnterCar(void *ped, void *car, uint8_t seat) {
	if (!ped || !car)
		return SEAT_LOST;
	if (PedIsInSeat(ped, car, seat)) {
		// The engine's own end of the entry left m_objective wherever
		// RestorePreviousObjective put it. Make it flat, because a
		// CCivilianPed holding an ENTER_CAR objective and a car pointer is
		// the thing that walks back to the car and tries again.
		ClearPedObjective(ped);
		return SEAT_DONE;
	}
	return PedIsEnteringCar(ped, car) ? SEAT_RUNNING : SEAT_LOST;
}

// The way out, animated. There is no guard list to mirror here because
// SetExitCar does its own: CanPedExitCar covers the car being upside down or
// moving too fast, and the PED_EXIT_CAR / PED_DRAG_FROM_CAR test covers
// being asked twice. Passing 0 for the door lets it work the door out from
// the seat the ped is actually in, which is the only version of that mapping
// that cannot disagree with the engine.
bool BeginPedExitCar(void *ped) {
	if (!ped || !Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;
	void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
	if (!car || Field<uintptr_t>(car, offs::VTABLE) != CAutomobile__vtable)
		return false;

	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_EXIT_CAR || state == PEDSTATE_DRAG_FROM_CAR)
		return true;   // already on its way out; saying no would start a warp
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return false;

	using ExitFn = void(__thiscall *)(void *, void *, uint32_t);
	Func<ExitFn>(CPed__SetExitCar)(ped, car, 0);

	return Field<uint32_t>(ped, offs::PED_STATE) == PEDSTATE_EXIT_CAR;
}

bool BeginSeatRemotePed(RemotePlayer &player, RemoteVehicle &vehicle, uint8_t seat,
                        uint8_t doorSeat) {
	void *const ped = ResolveRemote(player);
	if (!ped)
		return false;
	void *const car = ResolveRemoteVehicle(vehicle);
	if (!car)
		return false;
	return BeginPedEnterCar(ped, car, seat, doorSeat);
}

uint8_t PollSeatRemotePed(RemotePlayer &player, RemoteVehicle &vehicle, uint8_t seat) {
	void *const ped = ResolveRemote(player);
	if (!ped)
		return SEAT_LOST;
	void *const car = ResolveRemoteVehicle(vehicle);
	if (!car)
		return SEAT_LOST;
	return PollPedEnterCar(ped, car, seat);
}

void AbandonSeatRemotePed(RemotePlayer &player) {
	void *const ped = ResolveRemote(player);
	if (!ped)
		return;

	const int dropped = AbandonPedEnterCar(ped);

	static bool said = false;
	if (!said) {
		said = true;
		Log("bridge: %s's door-opening animation was taken off them before it "
		    "finished (%d partial animation(s) faded); the seat the session "
		    "gave them is applied directly instead",
		    player.nick.c_str(), dropped);
	}

	// Whatever the entry blended is gone, so whatever the wire wants has to
	// be driven again from scratch.
	player.appliedAnimId  = ANIM_NONE;
	player.appliedAnimId2 = ANIM_NONE;
}

bool BeginUnseatRemotePed(RemotePlayer &player) {
	void *const ped = ResolveRemote(player);
	return ped && BeginPedExitCar(ped);
}

// client.h carries its own copy of PED_EXIT_CAR, because the client layer is
// engine-free and cannot include addresses.h. Two hardcoded copies of a
// number that only agree with themselves are worth nothing; this is what
// makes them agree with each other.
static_assert(WIRE_PEDSTATE_EXIT_CAR == PEDSTATE_EXIT_CAR,
              "client.h's idea of PED_EXIT_CAR and addresses.h's must match");
static_assert(WIRE_PEDSTATE_DRIVING == PEDSTATE_DRIVING &&
                  WIRE_PEDSTATE_DRAG_FROM_CAR == PEDSTATE_DRAG_FROM_CAR &&
                  WIRE_PEDSTATE_ARRESTED == PEDSTATE_ARRESTED,
              "client.h's copies of PED_DRIVING, PED_DRAG_FROM_CAR and "
              "PED_ARRESTED must match addresses.h's");

// ---- the drive-by, on a ped in a seat ---------------------------------------
//
// DoDriveByShootings is the player's car's (addresses.h, "the drive-by"), so
// nothing in this machine's engine will ever put a remote driver's arm out of
// the window. These are its three anim writes, made for the side the wire
// names, and the uzi it assumes is already in the hand.

// Both sides dropped the way its no-look arm drops them.
void DropDriveByPose(void *clump) {
	for (const uint16_t id : {ANIM_STD_CAR_DRIVEBY_LEFT, ANIM_STD_CAR_DRIVEBY_RIGHT})
		if (void *const assoc = FindAnimById(clump, id))
			Field<float>(assoc, ANIM_BLEND_DELTA) = DRIVEBY_ANIM_DROP_DELTA;
}

// The uzi in the hand. The player's own seat keeps it there
// (RemoveWeaponWhenEnteringVehicle, player arm), but the warp that seats a
// remote ped takes the model off, so it goes back on here. SetCurrentWeapon
// removes every weapon atomic before it adds one (0x004CFA94, and
// RemoveWeaponModel ignores its argument), so this never stacks two.
void ArmForDriveBy(RemotePlayer &player, void *ped) {
	if (!GiveWeaponTo(player, ped, WEAPONTYPE_UZI))
		return;   // streaming; next frame
	player.driveByArmed = true;
	if (Field<int32_t>(ped, offs::PED_WEP_MODEL_ID) != -1)
		return;
	void *const info = WeaponInfo(WEAPONTYPE_UZI);
	if (!info)
		return;
	const int32_t model = Field<int32_t>(info, WEAPONINFO_MODEL_ID);
	if (model < 0)
		return;
	if (!HasModelLoaded(static_cast<uint32_t>(model))) {
		RequestModel(static_cast<uint16_t>(model));
		return;
	}
	using SetFn = void(__thiscall *)(void *, uint32_t);
	Func<SetFn>(CPed__SetCurrentWeapon)(ped, WEAPONTYPE_UZI);
}

bool g_saidDriveByPosed = false;

void ApplySeatedDriveBy(RemotePlayer &player, void *ped, uint32_t pedState) {
	void *const clump = ClumpOf(ped);
	if (!clump)
		return;

	const bool     driving = pedState == PEDSTATE_DRIVING && Field<bool>(ped, offs::PED_IN_VEHICLE);
	const uint16_t want    = driving ? DriveByPoseToHold(player.last.animId2,
	                                                     player.driveByShotAnim,
	                                                     WallClock::NowMs() - player.driveByShotMs)
	                                 : ANIM_NONE;
	if (want == ANIM_NONE) {
		if (player.appliedDriveBy != ANIM_NONE) {
			DropDriveByPose(clump);
			player.appliedDriveBy = ANIM_NONE;
		}
		return;
	}

	ArmForDriveBy(player, ped);

	// 0x005640C2 for the left, 0x00564120 for the right.
	if (void *const other = FindAnimById(clump, DriveByOtherSide(want)))
		Field<float>(other, ANIM_BLEND_DELTA) = DRIVEBY_ANIM_DROP_DELTA;

	void *const assoc = FindAnimById(clump, want);
	if (assoc && DriveByHeld(true, Field<float>(assoc, ANIM_BLEND_DELTA))) {
		Field<int32_t>(assoc, ANIM_FLAGS) |= ASSOC_RUNNING;
	} else {
		if (!MakeAnimRoom(player, clump, Field<void *>(ped, offs::PED_VEHICLE_ANIM)))
			return;
		using AddFn = void *(__cdecl *)(void *, int, int);
		if (!Func<AddFn>(CAnimManager__AddAnimation)(clump, ASSOCGRP_STD, static_cast<int>(want)))
			return;
	}

	if (player.appliedDriveBy == ANIM_NONE && !g_saidDriveByPosed) {
		g_saidDriveByPosed = true;
		Log("bridge: %s is shooting out of the %s window, and their ped here has the "
		    "arm out (%s)",
		    player.nick.c_str(), want == ANIM_STD_CAR_DRIVEBY_LEFT ? "left" : "right",
		    IsDriveByAnim(player.last.animId2) ? "off the snapshot" : "off a round");
	}
	player.appliedDriveBy = want;
}

// Out of the seat: nothing of the drive-by may follow them onto the street.
// The uzi was put in the hand by us, so the weapon is forgotten and the pose
// stream puts back whatever they hold, one atomic, through SetCurrentWeapon.
void EndSeatedDriveBy(RemotePlayer &player, void *ped) {
	if (player.appliedDriveBy != ANIM_NONE) {
		if (void *const clump = ClumpOf(ped))
			DropDriveByPose(clump);
		player.appliedDriveBy = ANIM_NONE;
	}
	if (player.driveByArmed) {
		player.driveByArmed  = false;
		player.appliedWeapon = 0xFFFF;
	}
}

void ApplyRemotePose(RemotePlayer &player, const Pose &pose) {
	void *ped = ResolveRemote(player);
	if (!ped)
		return;

	// Before anything else, and before every early return below, because it
	// is not about what CoopIII is applying this frame. A clump with more
	// than twelve animations on it makes RpAnimBlendClumpUpdateAnimations
	// write its node array over its own return address, and the ped being
	// seated or dead does not make that any less true.
	EnforceAnimLimit(player, ped, ClumpOf(ped));

	// Also before every early return below, and for a reason of its own: a
	// player who catches fire and then gets into a car, or dies, still has
	// to stop burning here when they stop burning there. The reconciliation
	// refuses to *start* a fire on a ped the engine has taken over
	// (CPed::IsPedInControl), but putting one out is always allowed.
	ApplyRemoteFire(player, ped);

	// A seated ped belongs to the engine now. CWorld::Process - its fifth
	// walk over the moving list, not CPed::ProcessControl, which is where
	// this comment used to point - calls SetPedPositionInCar on anything
	// with bInVehicle set and puts it back in its seat from the car's own
	// matrix every frame, so everything
	// below would get overwritten at best - and at worst the re-file and the
	// move state drag the ped half out of the car for the part of the frame
	// physics and collision actually look at.
	//
	// This matters more than any of the addresses do: the car has become the
	// authority on where this player is, so the pose stream has to stop
	// being one too. Health still applies, since that's about the player and
	// not about where they are.
	//
	// The same holds while they are *climbing in*, and for the same reason
	// rather than a similar one: it is the same walk over the moving list,
	// calling EnterCar() instead of SetPedPositionInCar, and LineUpPedWithCar
	// is what carries the ped from where it was standing to the seat. A
	// position written over the top of that is the ped being yanked back to
	// the pavement 25 times a second for the length of the animation.
	//
	// And the second half of the test is the ped's, not the session's, which
	// is what makes this self-healing. The condition below is
	// CWorld::Process's own (`bInVehicle && ... || EnteringCar()`, re3
	// World.cpp:1971), so it is true exactly when the engine is in fact
	// positioning this ped and false the instant it stops - whatever the
	// session still believes. Three things that used to need handling stop
	// needing it: a ped dropped out of a seat to make room for somebody the
	// session says is driving; a get-out animation that finishes before the
	// exit event arrives; and an entry that the engine abandoned without
	// anybody noticing. In all three the ped is back on the pose stream on
	// the very next frame instead of standing frozen until a packet says so.
	const uint32_t pedState      = Field<uint32_t>(ped, offs::PED_STATE);
	const bool     engineHasThem = Field<bool>(ped, offs::PED_IN_VEHICLE) ||
	                           pedState == PEDSTATE_ENTER_CAR ||
	                           pedState == PEDSTATE_CARJACK;
	if ((player.Seated() || player.Entering()) && engineHasThem) {
		Field<float>(ped, offs::PED_HEALTH) = player.last.health;
		Field<float>(ped, offs::PED_ARMOUR) = player.last.armour;
		ApplySeatedDriveBy(player, ped, pedState);
		return;
	}
	EndSeatedDriveBy(player, ped);

	// Position and facing go through PlaceRemotePed, which also pushes the
	// matrix into the clump's RenderWare frame and re-files the ped in the
	// sector grid. Writing the matrix alone is exactly what made the first
	// two remote players invisible - see the comment on that function.
	PlaceRemotePed(ped, pose.pos, pose.heading, /*inWorld=*/true);

	// Heading is a scalar yaw (docs/protocol.md §1.7). Both members get set,
	// same as the engine's own SetHeading - leave m_fRotationDest alone and
	// the ped's AI just rotates it straight back.
	Field<float>(ped, offs::PED_ROT_CUR)  = WrapAngle(pose.heading);
	Field<float>(ped, offs::PED_ROT_DEST) = WrapAngle(pose.heading);

	// Velocity gets written so the engine's own animation picking and
	// collision see a moving ped, not one that teleports every frame.
	float *vel = &Field<float>(ped, offs::MOVE_SPEED);
	vel[0]     = player.last.moveSpeed.x;
	vel[1]     = player.last.moveSpeed.y;
	vel[2]     = player.last.moveSpeed.z;

	Field<float>(ped, offs::PED_HEALTH) = player.last.health;
	Field<float>(ped, offs::PED_ARMOUR) = player.last.armour;

	// A corpse gets carried, not driven.
	//
	// Position still comes off the wire, because the owner's own ped is
	// still falling over and its snapshots say where it lands. Everything
	// below does not: CPed::SetDie hands the clump a death animation and
	// CPed::ProcessControl plays it out, and re-blending the walk the
	// snapshot happens to still name, or asking a dead ped to aim, is how
	// you get a corpse standing up mid-fall. The state is read off the ped
	// rather than off player.last.health because the ped is what actually
	// has a death animation running. Read once at the top of the function,
	// because the seat test above needs the same value.
	if (pedState == PEDSTATE_DIE || pedState == PEDSTATE_DEAD)
		return;

	// The move state, which this comment used to claim was what made the
	// remote player walk. It is not, and never was: CPed::Idle writes
	// PEDMOVE_STILL over it every frame before CPed::SetMoveAnim reads it,
	// because the ped is a CCivilianPed and Idle leaves only the player
	// alone (docs/protocol.md §1.13.4). The walk comes from ApplyAnimation
	// blending the wire's animId directly, below.
	//
	// It is still written, because it is what the ped reports to anything
	// else that asks - a co-op mission script included - and because it
	// costs one store. Nothing may be built on it being read by the engine.
	Field<uint32_t>(ped, offs::PED_MOVE_STATE) = ClampMoveState(player.last.moveState);

	ApplyWeapon(player, ped);
	ApplyFiring(player, ped);
	ApplyAim(player, ped);
	ApplyAnimation(player, ped);
}

} // namespace

void *LightWatchedPedFire(void *ped) { return ped ? LightRemoteFire(ped) : nullptr; }

// Where our engine has a remote player's ped, for a desync probe. Read without
// ResolveRemote, which may destroy a ped the engine has flagged and is only
// run before CGame::Process; this runs after it. And refused while the engine
// is putting the ped in a car, by the same test ApplyRemotePose makes, since
// the car says where they are then.
bool SampleRemotePedPosition(const RemotePlayer &player, Vec3 &out) {
	if (player.poolHandle < 0)
		return false;
	using GetPedFn = void *(__cdecl *)(int32_t);
	void *const ped = Func<GetPedFn>(CPools__GetPed)(player.poolHandle);
	if (!ped || Field<uintptr_t>(ped, offs::VTABLE) != CCivilianPed__vtable)
		return false;
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (Field<bool>(ped, offs::PED_IN_VEHICLE) || state == PEDSTATE_ENTER_CAR ||
	    state == PEDSTATE_CARJACK)
		return false;
	out = Vec3{Field<float>(ped, offs::POSITION + 0), Field<float>(ped, offs::POSITION + 4),
	           Field<float>(ped, offs::POSITION + 8)};
	return true;
}

bool WatchedPedFireIsOurs(int8_t fireSlot, void *ped, void *fire) {
	if (!fire || fireSlot < 0)
		return false;
	if (reinterpret_cast<uintptr_t>(fire) != FireSlot(static_cast<size_t>(fireSlot)))
		return false;
	return Field<uint8_t>(fire, FIRE_ONGOING) != 0 &&
	       Field<void *>(fire, FIRE_ENTITY) == ped;
}

// Is the local player in the middle of getting into a car, and if so, into
// which seat, through which door?
//
// True from the frame CPed::SetEnterCar takes - the align animation, which is
// the first thing anybody sees - to the frame the ped is in the seat. That is
// the window the other machines have to be told about, because the claim they
// already get is sent when this window closes: SampleLocalVehicleIdentity
// asks CVehicle::m_pDriver, and m_pDriver is written by PedSetInCarCB at the
// very end of the chain.
//
// The door is read off the ped rather than derived from the seat, and that is
// the whole point of this function. CPed::SeekCar sends a driver's entry
// through CPed::GetNearestDoor, so pressing the enter key beside the passenger
// door puts m_vehDoor at the front-right and the engine shuffles across inside
// the car. addresses.h, CPed__GetNearestDoor.
bool SampleLocalCarEntry(LocalCarEntry &out) {
	void *const ped = PlayerPed();
	if (!ped)
		return false;

	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state != PEDSTATE_ENTER_CAR && state != PEDSTATE_CARJACK)
		return false;

	// SetEnterCar_AllClear is what writes this, so it is set for exactly the
	// same window as the state above.
	void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
	if (!car)
		return false;

	const int32_t handle =
	    Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(car);
	if (handle < 0)
		return false;

	// m_vehDoor, a word. The seat-shaped name for it is what goes on the
	// wire, so that the receiving side can run it back through the one
	// seat-to-door table this project has (DoorForSeat) instead of carrying a
	// second copy of the engine's door constants.
	const uint16_t door = Field<uint16_t>(ped, offs::PED_VEH_DOOR);
	int32_t        doorSeat = -1;
	for (uint8_t s = 0; s < 4; ++s)
		if (DoorForSeat(s) == door)
			doorSeat = s;
	if (doorSeat < 0)
		return false;   // a coach, a train, a van's rear: not ours to animate

	// And where it ends. The objective is what PedSetInCarCB reads to choose
	// between SetDriver and AddPassenger, so it is the engine's own answer to
	// "which seat is this", available a second before the seat is.
	const uint32_t objective = Field<uint32_t>(ped, offs::PED_OBJECTIVE);
	out.vehicleHandle = handle;
	out.seat          = objective == OBJECTIVE_ENTER_CAR_AS_DRIVER
	                        ? 0
	                        : static_cast<uint8_t>(doorSeat);
	out.door          = static_cast<uint8_t>(doorSeat);
	return true;
}

bool StartCarEntry(void *ped, void *car, uint8_t seat) {
	// The local player's own passenger entry goes in through the seat's own
	// door: nothing has chosen a different one, and seat.cpp picked the slot
	// before the walk precisely so the door would be known. The door that
	// differs from the seat is the driver's, and the driver's entry is the
	// engine's own key, not this one.
	return BeginPedEnterCar(ped, car, seat, seat);
}

uint8_t PollCarEntry(void *ped, void *car, uint8_t seat) {
	return PollPedEnterCar(ped, car, seat);
}

int CancelCarEntry(void *ped) {
	if (!ped)
		return 0;
	return AbandonPedEnterCar(ped);
}

// The two things game/combat.cpp needs from in here, nothing else.
//
// Both thin wrappers on purpose, not copies. ResolveRemotePed is the only
// place the pool handle and the vtable are checked together, and a second
// copy of that check is just a second place to forget to update. GiveWeaponTo
// owns `RemotePlayer::appliedWeapon`, and two writers of that field would
// fight over when a weapon model gets rebuilt.
void *ResolveRemotePed(RemotePlayer &player) { return ResolveRemote(player); }

void SetAmmoSync(bool enabled) { g_ammoSync = enabled; }
bool AmmoSyncOn() { return g_ammoSync; }

void WriteRemoteSlotAmmo(void *ped, uint8_t slot, uint16_t clip, uint32_t total) {
	WriteSlotAmmo(ped, slot, clip, total);
}

bool GiveRemoteWeapon(RemotePlayer &player, void *ped, uint8_t weapon) {
	return GiveWeaponTo(player, ped, weapon);
}

bool PutReplicaWeaponInHand(void *ped, uint8_t weapon) {
	return ped && IsInventoryWeapon(weapon) && PutWeaponInHand(ped, weapon);
}

uint8_t HeldWeaponType(void *ped) {
	if (!ped)
		return WEAPONTYPE_UNARMED;
	void *const held = WeaponSlot(ped, Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON));
	if (!held)
		return WEAPONTYPE_UNARMED;
	const uint32_t type = Field<uint32_t>(held, offs::WEAPON_TYPE);
	return IsInventoryWeapon(static_cast<uint8_t>(type)) ? static_cast<uint8_t>(type)
	                                                      : WEAPONTYPE_UNARMED;
}

bool RemotePlayerForPed(const void *ped, uint16_t &netId) {
	return LookupRemotePed(ped, netId);
}

bool InstallAimPitchHook() {
	g_replicaPitch.Clear();
	g_localAim = LocalAimRecord{};
	if (!g_pointGunHook.Install("CPedIK::PointGunInDirection",
	                            reinterpret_cast<void *>(CPedIK__PointGunInDirection),
	                            reinterpret_cast<void *>(&HookedPointGunInDirection))) {
		Log("ped: FAILED to hook CPedIK::PointGunInDirection at 0x%08X; remote "
		    "players aim level and our own lock-on aim goes out as the old guess",
		    CPedIK__PointGunInDirection);
		for (const auto &f : HookFailures())
			Log("ped:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}
	Log("ped: hooked CPedIK::PointGunInDirection at 0x%08X", CPedIK__PointGunInDirection);
	return true;
}

void RemoveAimPitchHook() {
	g_pointGunHook.Remove();
	g_replicaPitch.Clear();
	g_localAim = LocalAimRecord{};
}

int32_t StdAnimGroupCount() { return AnimGroupCount(ASSOCGRP_STD); }

// ---- what game/world.cpp's CWorld::Process sweep hands every entity -------
//
// The same twelve-slot bound MakeAnimRoom and EnforceAnimLimit enforce, but
// applied to *every* clump on the moving list rather than only to the peds
// CoopIII drives - which is the difference between a rule about CoopIII's
// peds and a rule about the engine's array.
//
// It has to be this wide. CoopIII changes what the engine's own pedestrians
// do: it replicates them, it seats them, it takes limbs off them, it kills
// them from the wire and it keeps them alive past a death their host already
// ran. Any of that can leave a ped CoopIII does not have a roster entry for
// carrying more associations than the engine can index, and the engine's
// reaction is not a wrong pose, it is
// RpAnimBlendClumpUpdateAnimations writing its node array over its own saved
// registers and handing CWorld::Process's walk a cursor made of animation
// data (addresses.h, AnimNodeArrayOverflow).
//
// Mirrors walk 1's own two tests before it touches anything: a null
// m_rwObject and a non-clump are exactly what the engine skips, and looking
// at either would be looking at something that is not an animation list.
void ClampClumpAnimations(void *entity) {
	if (!entity)
		return;
	void *const clump = ClumpOf(entity);
	if (!clump)
		return;
	// `cmp byte ptr [eax],2` at 0x004B1B2C. RwObject::type, rpCLUMP.
	if (*reinterpret_cast<const uint8_t *>(clump) != RW_TYPE_CLUMP)
		return;

	const int count = CountAnims(clump);
	if (!AnimNodeArrayOverflows(count))
		return;

	// One association is kept back whatever its weight: a ped's
	// m_pVehicleAnim. It is not one CoopIII applied, so there is no id to
	// spare it by, and it carries the callback that finishes a door-opening
	// chain - drop it and the ped stays in PED_ENTER_CAR with nothing left
	// to end it. The type test is what makes reading +0x1D8 legal: this
	// sweep is handed vehicles and objects too, and that offset is a ped
	// field.
	void *keepAssoc = nullptr;
	if ((Field<uint8_t>(entity, offs::ENTITY_FLAGS) & 7) == offs::ENTITY_TYPE_PED)
		keepAssoc = Field<void *>(entity, offs::PED_VEHICLE_ANIM);

	const int dropped =
	    PruneAnims(clump, ANIM_NONE, ANIM_NONE, keepAssoc,
	               count - MAX_CLUMP_ANIM_ASSOCS);

	static bool said = false;
	if (!said) {
		said = true;
		Log("world: an entity on the moving list was carrying %d animations, "
		    "past the %d RpAnimBlendClumpUpdateAnimations can index; dropped "
		    "%d before CWorld::Process walked it. Its vtable is %08X (and this "
		    "will not be said again)",
		    count, MAX_CLUMP_ANIM_ASSOCS, dropped,
		    static_cast<unsigned>(Field<uintptr_t>(entity, offs::VTABLE)));
	}
}

// ---- what the ambient population seam borrows -----------------------------
//
// Four thin exports, and each one is here rather than copied into
// population.cpp because the copy is what would go wrong. PlaceRemotePed's
// three steps took two in-game sessions to find; the seating order is a
// precondition rather than tidiness; and the animation group bound is an
// unchecked subscript into a four-element array. None of that gets safer for
// being written out a second time next to a different roster type.

void PlaceReplicaPed(void *ped, const Vec3 &pos, float heading, bool inWorld) {
	if (ped)
		PlaceRemotePed(ped, pos, heading, inWorld);
}

uint16_t ReadPedBaseAnim(void *ped) {
	if (!ped)
		return ANIM_NONE;
	void *const clump = ClumpOf(ped);
	if (!clump)
		return ANIM_NONE;
	AnimSample base, partial;
	ReadDominantAnims(clump, base, partial);
	return base.id;
}

// One animation onto a replica's clump, through the engine's own blender.
//
// The whole of docs/protocol.md §1.13.4 in one call: this, not m_nMoveState,
// is what makes a non-player ped walk. `CPed::Idle` overwrites the move state
// with PEDMOVE_STILL before `SetMoveAnim` can read it, so a replicated
// pedestrian that is only told its move state stands in its idle pose and
// slides.
//
// No animTime and no speed, unlike a remote player's. An ambient ped's
// locomotion animation takes its rate from the engine and its phase is not
// something anyone can see is wrong on a stranger - which is exactly the
// argument for leaving both off the wire (protocol.h, AmbientPedState).
bool BlendReplicaAnim(void *ped, uint16_t animId) {
	if (!ped)
		return false;
	void *const clump = ClumpOf(ped);
	if (!clump)
		return false;

	const int pedGroup = Field<int32_t>(ped, offs::PED_ANIM_GROUP);
	const AnimPlan plan = PlanAnim(animId, pedGroup, AnimGroupCount(pedGroup),
	                               AnimGroupCount(ASSOCGRP_STD));
	if (!plan.valid)
		return false;

	// The same twelve-slot limit a remote player's clump lives under, and it
	// is not a soft one: past MAX_CLUMP_ANIM_ASSOCS,
	// RpAnimBlendClumpUpdateAnimations writes its node array over its own
	// return address. A replica carries far fewer animations than a player
	// does - one locomotion anim from here, plus whatever CPed::Idle and
	// SetInTheAir add on their own (§1.13.5) - so this should never fire.
	// "Should never" is why it is checked rather than assumed.
	if (!FindAnimById(clump, animId)) {
		const int count = CountAnims(clump);
		if (!AnimClumpHasRoom(count)) {
			const int dropped =
			    PruneAnims(clump, animId, ANIM_NONE, nullptr,
			               AnimClumpSurplus(count));
			if (!AnimClumpHasRoom(count - dropped))
				return false;
		}
	}

	using BlendFn = void *(__cdecl *)(void *, int, int, float);
	return Func<BlendFn>(CAnimManager__BlendAnimation)(
	           clump, plan.group, static_cast<int>(animId), plan.blendDelta) != nullptr;
}

bool SeatReplicaPed(void *ped, void *car, uint8_t seat) {
	return SeatPedInCar(ped, car, seat);
}

void UnseatReplicaPed(void *ped) { UnseatPedFromCar(ped); }

WorldBridge MakeWorldBridge() {
	WorldBridge b;
	b.SampleLocalPlayer = &SampleLocalPlayer;
	b.RequestModel      = &RequestModel;
	b.IsModelReady      = &IsModelReady;
	b.SampleLocalPlayerModel = &SampleLocalPlayerModel;
	b.SampleLocalAmmo        = &SampleLocalAmmo;
	b.ApplyRemoteAmmo        = &ApplyRemoteAmmoSlot;
	b.ApplyRemotePose   = &ApplyRemotePose;
	b.SampleRemotePedPosition = &SampleRemotePedPosition;
	b.SpawnRemote       = &SpawnRemote;
	b.DespawnRemote     = &DespawnRemote;

	b.SampleLocalVehicle   = &SampleLocalVehicle;
	b.SpawnRemoteVehicle   = &SpawnRemoteVehicle;
	b.DespawnRemoteVehicle = &DespawnRemoteVehicle;
	b.SampleLocalVehicleIdentity = &SampleLocalVehicleIdentity;
	b.SampleLocalVehicleHandle   = &SampleLocalVehicleHandle;
	b.ApplyRemoteVehicle   = &ApplyRemoteVehicle;
	b.RestRemoteVehicle    = &RestRemoteVehicle;
	b.CorrectRemoteVehicle = &CorrectRemoteVehicle;
	// And the exception to resting and pinning: one machine, named by the
	// session, finishes what a driverless car was doing instead of holding it
	// in the pose its last driver left it in. protocol.h, S_VehicleCustody.
	b.SampleObservedVehicle = &SampleObservedVehicle;
	b.VehicleAtRest         = &VehicleAtRest;
	b.VehicleBurning        = &VehicleBurning;
	b.VehiclePushedByUs     = &VehiclePushedByUs;
	b.VehicleSinking        = &VehicleSinking;
	b.TakeVehicleBack       = &TakeVehicleBack;
	// What shape a car is in (docs/cardamage.md). Beside the state pair
	// because they are the same seam, and separate from it because damage is
	// an event on the reliable channel and state is a 25 Hz sample.
	b.SampleLocalVehicleDamage    = &SampleLocalVehicleDamage;
	b.SampleObservedVehicleDamage = &SampleObservedVehicleDamage;
	b.ApplyRemoteVehicleDamage    = &ApplyRemoteVehicleDamage;
	b.SeatRemotePed        = &SeatRemotePed;
	b.UnseatRemotePed      = &UnseatRemotePed;
	b.BeginSeatRemotePed   = &BeginSeatRemotePed;
	b.PollSeatRemotePed    = &PollSeatRemotePed;
	b.AbandonSeatRemotePed = &AbandonSeatRemotePed;
	b.BeginUnseatRemotePed = &BeginUnseatRemotePed;
	b.SampleLocalCarEntry  = &SampleLocalCarEntry;

	// Combat. These three live in game/combat.cpp because they're driven by
	// detours rather than the frame pump, and because every address they use
	// came out of one pass over one script opcode - keeping them together
	// keeps that provenance together too.
	b.DrainLocalCombat    = &DrainLocalCombat;
	b.ReplayRemoteShot    = &ReplayRemoteShot;
	b.PlayRemoteExplosion = &PlayRemoteExplosion;
	b.ApplyRemoteDamage   = &ApplyRemoteDamage;
	b.KillRemotePed       = &KillRemotePed;
	// The pedestrian half of ApplyRemoteDamage, and it is wired here rather
	// than in AddPopulationToBridge beside the other ambient entries because it
	// is combat.cpp's function: the bounds it applies and the call it makes are
	// the ones directly above, and the only thing it borrows from
	// population.cpp is the netId lookup. Not gated on the population hooks
	// either - with them missing this machine hosts no named pedestrians, so
	// nothing ever resolves and the function is simply never useful.
	b.ApplyRemotePedDamage = &ApplyRemotePedDamage;
	// And the other direction: somebody else's pedestrian firing where we can
	// see it, and hitting us.
	b.ReplayAmbientShot   = &ReplayAmbientShot;
	b.ApplyNpcDamage      = &ApplyNpcDamage;
	b.SetFriendlyFire     = &SetFriendlyFire;
	b.SetAmmoSync         = &SetAmmoSync;

	// Asked of CVehicle::m_pDriver, not of the roster. Wired in down here
	// rather than beside the other vehicle entries because it answers a
	// question about the *engine*, and because the thing it protects is the
	// one place a claim in flight can leave a car frozen under its own
	// driver. WorldBridge::LocalDrivesVehicle says the rest.
	b.LocalDrivesVehicle = &LocalDrivesVehicle;

	// And the one case where that question's answer is stale rather than
	// wrong: the session has given a car we are sitting at the wheel of to the
	// player who jacked us. WorldBridge::SurrenderVehicleSeat.
	b.SurrenderVehicleSeat = &SurrenderVehicleSeat;

	// Every address in the spawn path comes from the game's own
	// COMMAND_CREATE_CHAR / COMMAND_DELETE_CHAR handlers (addresses.h records
	// how they were found), so this does what the engine does instead of
	// approximating it - registration included, which the first in-game run
	// skipped and is why that run's ped got reaped by the population manager
	// before it ever rendered.
	Log("bridge: full - local sampling, ped spawn/despawn, pose, animation, "
	    "weapon and aim, vehicle spawn/despawn, state and seating, plus "
	    "firing, projectiles, explosions, damage, death and respawn");
	return b;
}

} // namespace coopiii::game
