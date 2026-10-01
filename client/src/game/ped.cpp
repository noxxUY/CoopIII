#include "ped.h"

#include "addresses.h"
#include "animcb.h"
#include "animrevive.h"
#include "boat.h"
#include "carremoval.h"
#include "carstatus.h"
#include "clock.h"
#include "combat.h"
#include "driveby.h"
#include "hook/hook.h"
#include "leadcheck.h"
#include "log.h"
#include "look.h"
#include "passengeraim.h"
#include "passexit.h"
#include "pedaim.h"
#include "pedanim.h"
#include "population.h"
#include "remoteloco.h"
#include "ride.h"
#include "seatplan.h"
#include "teardown.h"
#include "vehicle.h"

#include <cstring>
#include <windows.h>

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

// One play-through of an association, in seconds, or 0 when it has no
// hierarchy to say (ANIMHIER_TOTAL_LENGTH).
float AnimLength(void *assoc) {
	void *const hier = assoc ? Field<void *>(assoc, ANIM_HIERARCHY) : nullptr;
	if (!hier)
		return 0.0f;
	const float len = Field<float>(hier, ANIMHIER_TOTAL_LENGTH);
	return std::isfinite(len) && len > 0.0f ? len : 0.0f;
}

// locoWeight is indexed by the id itself.
static_assert(LOCO_ANIM_IDS[LOCO_WALK] == ANIM_STD_WALK && LOCO_ANIM_IDS[LOCO_RUN] == ANIM_STD_RUN &&
                  LOCO_ANIM_IDS[LOCO_SPRINT] == ANIM_STD_RUNFAST &&
                  LOCO_ANIM_IDS[LOCO_IDLE] == ANIM_STD_IDLE && LOCO_ANIM_IDS[LOCO_STARTWALK] == 4,
              "the five walking-style ids, in their own order");

// The legs as our engine is blending them (PlayerStateBody::locoWeight): the
// weight of each of the five walking-style animations over the whole body,
// and where the strongest movement one is in its stride. What
// CPlayerPed::SetRealMoveAnim leaves on the clump, read rather than guessed.
void ReadLegs(void *clump, uint8_t (&weight)[LOCO_ANIMS], uint8_t &phase) {
	float sum[LOCO_ANIMS] = {};
	float bestStride      = 0.0f;
	void *stride          = nullptr;
	ForEachAnim(clump, [&](void *assoc) {
		const int32_t flags = Field<int32_t>(assoc, ANIM_FLAGS);
		if (flags & ASSOC_PARTIAL)
			return;
		const int32_t id    = Field<int32_t>(assoc, ANIM_ID);
		const float   blend = Field<float>(assoc, ANIM_BLEND_AMOUNT);
		if (id < 0 || id >= static_cast<int32_t>(LOCO_ANIMS) || !(blend > 0.0f))
			return;
		sum[id] += blend;
		if ((flags & ASSOC_MOVEMENT) && blend > bestStride) {
			bestStride = blend;
			stride     = assoc;
		}
	});
	for (size_t i = 0; i < LOCO_ANIMS; ++i)
		weight[i] = LocoWeightOnWire(sum[i]);
	phase = stride ? LocoPhaseOnWire(Field<float>(stride, ANIM_CURRENT_TIME), AnimLength(stride))
	               : 0;
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
	ReadLegs(ClumpOf(ped), out.locoWeight, out.locoPhase);

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
	// An id off the wire with no model info behind it: RequestModel reads the
	// info's txd slot before it looks at anything else.
	if (!VehicleModelInfo(modelId))
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

// Which model each remote Claude is built from, or waiting on: MI_PLAYER, or
// a special slot holding their look (PrepareRemoteLook, further down).
uint16_t g_lookModelOf[MAX_PLAYERS] = {};
// Set when HookedRequestSpecialModel took a player's ped down, so the next
// ResolveRemote says why the handle stopped resolving.
bool     g_lookTakenDown[MAX_PLAYERS] = {};

// CBaseModelInfo::m_name of a model, or null for one that isn't there.
const char *ModelName(uint32_t modelId) {
	if (modelId >= MODELINFO_SIZE)
		return nullptr;
	const uint8_t *const mi =
	    reinterpret_cast<const uint8_t *const *>(CModelInfo__ms_modelInfoPtrs)[modelId];
	return mi ? reinterpret_cast<const char *>(mi + offs::MODELINFO_NAME) : nullptr;
}

void RememberRemotePed(const RemotePlayer &player) {
	if (player.playerId >= MAX_PLAYERS)
		return;
	g_remotePeds[player.playerId] = RemotePedIdentity{player.poolHandle, player.netId,
	                                                  player.playerId};
}

void ForgetRemotePed(const RemotePlayer &player) {
	if (player.playerId < MAX_PLAYERS) {
		g_remotePeds[player.playerId]    = RemotePedIdentity{};
		g_lookModelOf[player.playerId]   = MI_PLAYER;
		g_lookTakenDown[player.playerId] = false;
	}
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
//
// All of it is game/teardown.h's DestroyPed now, which also refuses a ped
// that is no longer live in its pool slot or is already being taken apart.
void DestroyRemotePed(void *ped) {
	DestroyPed(ped, /*countedMissionPed=*/true, "a remote player's ped");
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
	if (player.playerId < MAX_PLAYERS && g_lookTakenDown[player.playerId]) {
		Log("bridge: %s's ped was taken down while a script changed the model it "
		    "was built from; building it again",
		    player.nick.c_str());
	} else {
		Log("bridge: %s's ped is gone from under us (%s)", player.nick.c_str(),
		    ped == nullptr
		        ? "pool slot no longer matches the handle"
		        : (Field<uintptr_t>(ped, offs::VTABLE) == CPlaceable__vtable
		               ? "object has been through its destructor"
		               : "object is no longer a CCivilianPed"));
	}
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

	// Claude in the other outfit is built from the special slot
	// PrepareRemoteLook loaded for them. Checked again here rather than
	// trusted: the slot has to still hold their look, or the ped comes out
	// in whatever a mission put there.
	const uint16_t lookModel =
	    player.playerId < MAX_PLAYERS ? g_lookModelOf[player.playerId] : MI_PLAYER;
	if (model == MI_PLAYER && lookModel != MI_PLAYER) {
		const char *const name = ModelName(lookModel);
		if (!HasModelLoaded(lookModel) || !SameLook(name, player.look))
			return false;
		model = lookModel;
		Log("look: building %s from special slot %d ('%s')", player.nick.c_str(),
		    lookModel - MI_SPECIAL01 + 1, name);
	}

	// The model comes off the wire, and a loaded car or object id would be
	// read by CPed::SetModelIndex as a CPedModelInfo.
	if (!IsPedModel(model)) {
		static uint32_t said = 0;
		if (said++ < 4)
			Log("bridge: %s's model %u is not a pedestrian's here; built from the player's",
			    player.nick.c_str(), static_cast<unsigned>(model));
		model = MI_PLAYER;
	}

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
// comes from what this file blends: ApplyLegs for the walk, run, sprint,
// idle and start-walk (docs/protocol.md 1.8.4), BlendRemoteAnim for the
// rest. docs/protocol.md §1.13.4.

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

// The association a ped's m_pVehicleAnim points at, or null: never one of
// those to drop for room. The engine keeps that pointer raw - a seat's
// CAR_SIT, a door's open or close with the callback that ends the chain -
// and an association dropped here is deleted on the engine's next pass,
// leaving the ped holding freed memory.
void *VehicleAnimOf(void *ped) {
	return ped ? Field<void *>(ped, offs::PED_VEHICLE_ANIM) : nullptr;
}

bool BlendRemoteAnim(RemotePlayer &player, void *ped, void *clump, int pedGroup, uint16_t animId,
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
	if (!FindAnimById(clump, animId) && !MakeAnimRoom(player, clump, VehicleAnimOf(ped)))
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

	// Not for a movement animation. Its speed does set its rate, all
	// movement animations sharing one (ANIMHIER_TOTAL_LENGTH), but walk, run
	// and sprint are ApplyLegs's, which sets their speed from the sender's
	// with the stride kept in step. This path only sees them when that one
	// could not run.
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
//
// Condemned: a negative blend delta and ASSOC_DELETEFADEDOUT, which both
// routes above leave. Catching it before the association disappears is what
// keeps the gap invisible, and it costs nothing - CAnimManager::BlendAnimation
// revives an association it finds by recomputing the delta as
// (1 - blendAmount) * delta, which is positive.
//
// Spent is the third route, and the one a landing takes: a negative delta
// with no delete flag, parked at weight 0 by UpdateBlend and kept there
// (game/animrevive.h). Found by id, it looks alive; it weighs nothing.
AnimLife LifeOf(void *assoc) {
	return ClassifyAnim(Field<float>(assoc, ANIM_BLEND_AMOUNT),
	                    Field<float>(assoc, ANIM_BLEND_DELTA),
	                    Field<int32_t>(assoc, ANIM_FLAGS));
}

// What the clump holds for one wanted id: the liveliest association with it,
// and one of the engine's own moves playing over it, if there is one.
//
// Liveliest, not first. BlendAnimation revives the *last* association with
// the id it is given (`mov ebx,ecx` on every match, 0x004037CE) and
// AddAnimation puts a new one at the front, so with two on the clump a
// first-match lookup can keep judging the spent one after the other was
// brought back - and blend again every frame.
struct AnimOnClump {
	void    *assoc = nullptr;
	AnimLife life  = AnimLife::SPENT;
	uint16_t move  = ANIM_NONE;
};

AnimOnClump LookForAnim(void *clump, uint16_t want) {
	AnimOnClump seen;
	ForEachAnim(clump, [&](void *assoc) {
		const int32_t id = Field<int32_t>(assoc, ANIM_ID);
		if (id == static_cast<int32_t>(want)) {
			const AnimLife life = LifeOf(assoc);
			if (!seen.assoc || Livelier(life, seen.life)) {
				seen.assoc = assoc;
				seen.life  = life;
			}
		} else if (seen.move == ANIM_NONE && id >= 0 && id < ANIM_NONE &&
		           EngineMoveHoldsOff(want, static_cast<uint16_t>(id),
		                              Field<float>(assoc, ANIM_BLEND_AMOUNT),
		                              Field<float>(assoc, ANIM_BLEND_DELTA),
		                              Field<int32_t>(assoc, ANIM_FLAGS))) {
			seen.move = static_cast<uint16_t>(id);
		}
	});
	return seen;
}

bool g_saidOverlayHeld    = false;
bool g_saidOverlayRevived = false;
bool g_saidBaseRevived    = false;

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
void ApplyOverlay(RemotePlayer &player, void *ped, void *clump, int pedGroup) {
	const uint16_t want = player.shown.animId2;

	if (want == ANIM_NONE) {
		if (player.appliedAnimId2 != ANIM_NONE) {
			FadeOutPartial(clump, player.appliedAnimId2);
			player.appliedAnimId2 = ANIM_NONE;
		}
		return;
	}

	const bool running = (player.shown.flags & PF_ANIM2_RUNNING) != 0;

	// Start it, or bring it back if something ended it behind our back:
	// ASSOC_FADEOUTWHENDONE when it ran to the end, CPed::SetMoveAnim's purge
	// of every partial on a change of move state, or a landing or a get-up
	// zeroing every partial on the clump (LifeOf above).
	const AnimOnClump seen  = LookForAnim(clump, want);
	const uint32_t    state = Field<uint32_t>(ped, offs::PED_STATE);
	const bool        hold  = EngineMoveHolds(
        PedDownOrGettingUp(state),
        PedAirborneOrLanding(Field<uint8_t>(ped, offs::PED_FLAGS_B), state),
        seen.move != ANIM_NONE);

	void *assoc = seen.assoc;
	switch (PlanOverlay(assoc != nullptr, seen.life, running, hold)) {
	case OverlayStep::KEEP:
		break;

	case OverlayStep::LET_END:
		// The rocket launcher is what this is for.
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
		return;

	case OverlayStep::HOLD:
		// Our engine is landing, falling or getting this ped up on its own
		// account. Blending a partial now would condemn that animation
		// (BlendAnimation fades every other partial), so the overlay goes back
		// on the first frame after it, and the wipe at the end of it is what
		// it comes back from.
		if (!g_saidOverlayHeld) {
			g_saidOverlayHeld = true;
			Log("bridge: %s's overlay %02Xh waits for our engine to finish its own "
			    "move (state %u, anim %Xh) before it goes back on",
			    player.nick.c_str(), want, state, seen.move);
		}
		return;

	case OverlayStep::APPLY:
		if (!BlendRemoteAnim(player, ped, clump, pedGroup, want, player.shown.animTime2,
		                     1.0f, false))
			return;
		assoc = LookForAnim(clump, want).assoc;
		if (!assoc)
			return;
		if (seen.assoc && seen.life == AnimLife::SPENT && !g_saidOverlayRevived) {
			g_saidOverlayRevived = true;
			Log("bridge: %s's overlay %02Xh was on the clump at weight 0, zeroed by "
			    "our own engine (a landing or a get-up), and was put back at %.2f s",
			    player.nick.c_str(), want, SafeAnimTime(player.shown.animTime2));
		}
		break;
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
		// exactly where its owner is holding it. It can still be zeroed, which
		// is the spent case above, and the reason an aim used to vanish on
		// landing for good.
		if (std::fabs(Field<float>(assoc, ANIM_CURRENT_TIME) -
		              SafeAnimTime(player.shown.animTime2)) > ANIM_PHASE_TOLERANCE)
			SeekAnim(assoc, player.shown.animTime2);
		return;
	}

	// Running. The loop belongs to the weapon, so it only applies when this
	// overlay really is that weapon's animation; a punch or a throw has no
	// loop and plays straight through.
	void *const info = WeaponInfo(player.shown.weapon);
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
	const int wanted = static_cast<int>(player.shown.animGroup);

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

		if (!MakeAnimRoom(player, clump, VehicleAnimOf(ped)))
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

// The simulated time this frame is about to cover, in seconds: the engine's
// own step (interp.h has the unit), which is what the animation walk will
// advance every association by.
float FrameSeconds() {
	float step = 0.0f;
	FiniteOr(Global<float>(CTimer__ms_fTimeStep), 0.0f, step);
	const float s = step / ENGINE_STEPS_PER_SECOND;
	return s < 0.0f ? 0.0f : (s > 0.1f ? 0.1f : s);
}

bool g_saidLegs      = false;
bool g_saidLegsGroup = false;

// The hierarchy a group plays for an id, or null when the group cannot answer
// for it. The group's assocList is an array of CAnimBlendAssociation, 0x40
// apart (GetAnimation is `shl eax,6 / add eax,[ecx]`), each holding the
// hierarchy it was made from. Two groups that name the same animation share
// it ("walk_player" in the player's four armed styles), which is the test
// CPlayerPed::ReApplyMoveAnims makes by name.
void *GroupHierarchy(int group, uint16_t id) {
	if (!ValidAnimGroup(group) || static_cast<int32_t>(id) >= AnimGroupCount(group))
		return nullptr;
	const uintptr_t groups = Global<uintptr_t>(CAnimManager__ms_aAnimAssocGroups);
	if (!groups)
		return nullptr;
	uint8_t *const list = *reinterpret_cast<uint8_t **>(
	    groups + static_cast<size_t>(group) * SIZEOF_ANIMGROUP + ANIMGROUP_ASSOC_LIST);
	if (!list)
		return nullptr;
	return Field<void *>(list + static_cast<size_t>(id) * SIZEOF_ANIM_ASSOC, ANIM_HIERARCHY);
}

// A remote player's legs, played the way their engine is playing them
// (docs/protocol.md 1.8.4, remoteloco.h).
//
// This replaces blending the one strongest id at SetMoveAnim's crossfade of
// 1.0, which was a full second, and which is where the sliding came from:
//
//   - the owner's CPlayerPed::SetRealMoveAnim does not crossfade its walk and
//     run, it writes their weights every frame, and starts a walk with the
//     start-walk at full weight while deleting the idle outright. A second's
//     crossfade on this side left the legs mostly idle for most of a second
//     of a player already running at full speed, and mostly running for most
//     of a second after they had stopped;
//   - between a walk and a run the owner plays both. Playing only the
//     stronger at full weight moves the ped at the owner's speed on the
//     other stride;
//   - the phase was seeked once, from a snapshot up to 40 ms older than the
//     instant being drawn, and never again.
//
// So each of the five gets the weight the sender gave it, blended between the
// snapshots either side of the drawn instant, set directly as the owner's
// engine sets it; walk, run and sprint play at the sender's speed, trimmed a
// little fast or slow to stay on the sender's stride; and every other
// whole-body animation fades out, as BlendAnimation would fade it. The
// weights are written with a zero delta before the engine's animation walk
// runs, so nothing our engine blends on the replica afterwards (a get-up's
// SetMoveAnim, BlendAnimation's fade of everything else) lasts past the
// next frame.
//
// False when it could not run, and the old one-id path plays instead.
bool ApplyLegs(RemotePlayer &player, void *ped, void *clump, int pedGroup) {
	const LegPose &legs = player.legs;
	if (!legs.valid)
		return false;
	const int32_t stdCount = AnimGroupCount(ASSOCGRP_STD);
	if (stdCount <= 0)
		return false;
	const int32_t pedCount    = AnimGroupCount(pedGroup);
	void *const   vehicleAnim = VehicleAnimOf(ped);
	const float   dt          = FrameSeconds();

	// One association per id, the liveliest and then the heaviest; a second
	// with the same id would add its weight to ours, so it goes. Anything
	// else whole-body is faded, once: a delta already below zero is left to
	// finish. Never the seat's animation, which the engine holds a raw
	// pointer to.
	void *have[LOCO_ANIMS] = {};
	ForEachAnim(clump, [&](void *assoc) {
		if (assoc == vehicleAnim)
			return;
		const int32_t flags = Field<int32_t>(assoc, ANIM_FLAGS);
		if (flags & ASSOC_PARTIAL)
			return;
		const int32_t id = Field<int32_t>(assoc, ANIM_ID);
		if (id >= 0 && id < static_cast<int32_t>(LOCO_ANIMS)) {
			void *&slot = have[id];
			if (!slot) {
				slot = assoc;
				return;
			}
			// Condemned counts least here, not in between: it is the one
			// something already decided to take off, like the old group's
			// copy ApplyAnimGroup has just replaced. On a tie the heavier,
			// then the first found, which is the newest (AddAnimation
			// prepends).
			auto rank = [](AnimLife life) {
				return life == AnimLife::LIVE ? 2 : life == AnimLife::SPENT ? 1 : 0;
			};
			const int  mine = rank(LifeOf(slot)), theirs = rank(LifeOf(assoc));
			const bool keepNew =
			    theirs > mine || (theirs == mine && Field<float>(assoc, ANIM_BLEND_AMOUNT) >
			                                            Field<float>(slot, ANIM_BLEND_AMOUNT));
			DropAnimNow(keepNew ? slot : assoc);
			if (keepNew)
				slot = assoc;
			return;
		}
		if (LifeOf(assoc) == AnimLife::LIVE) {
			Field<float>(assoc, ANIM_BLEND_DELTA) = -8.0f;
			Field<int32_t>(assoc, ANIM_FLAGS)     = flags | ASSOC_DELETEFADEDOUT;
		}
	});

	bool strideBefore = false;
	for (size_t i = 0; i < LOCO_ANIMS; ++i)
		strideBefore = strideBefore || (have[i] && IsStrideAnim(LOCO_ANIM_IDS[i]));

	using AddFn = void *(__cdecl *)(void *, int, int);
	bool strideAdded = false;
	for (size_t i = 0; i < LOCO_ANIMS; ++i) {
		const uint16_t id     = LOCO_ANIM_IDS[i];
		float          target = legs.weight[i];
		target                = target > 0.0f ? (target < 1.0f ? target : 1.0f) : 0.0f;
		void *assoc           = have[i];

		if (!assoc) {
			if (!(target > 0.001f))
				continue;
			// Out of the ped's own style where it has the id, which the
			// strafe groups do for the start-walk too: walk_start_left is
			// the side-step a strafe begins with.
			const AnimPlan plan = PlanAnim(id, pedGroup, pedCount, stdCount);
			if (!plan.valid || !MakeAnimRoom(player, clump, vehicleAnim))
				continue;
			assoc = Func<AddFn>(CAnimManager__AddAnimation)(clump, plan.group, static_cast<int>(id));
			if (!assoc)
				continue;
			Field<float>(assoc, ANIM_BLEND_AMOUNT) = 0.0f;
			have[i] = assoc;
			strideAdded = strideAdded || IsStrideAnim(id);
		} else if (target > 0.001f) {
			// Out of the wrong style: the walking style changed while this
			// one was off the clump, or ApplyAnimGroup found no room to
			// re-hang it. A strafe played on the forward run is the slide
			// this is here to end, so it is re-hung the same way, weight
			// and all.
			const AnimPlan plan = PlanAnim(id, pedGroup, pedCount, stdCount);
			void *const    want = plan.valid ? GroupHierarchy(plan.group, id) : nullptr;
			if (want && Field<void *>(assoc, ANIM_HIERARCHY) != want &&
			    MakeAnimRoom(player, clump, vehicleAnim)) {
				void *const fresh =
				    Func<AddFn>(CAnimManager__AddAnimation)(clump, plan.group, static_cast<int>(id));
				if (fresh) {
					Field<float>(fresh, ANIM_BLEND_AMOUNT) = Field<float>(assoc, ANIM_BLEND_AMOUNT);
					DropAnimNow(assoc);
					assoc   = fresh;
					have[i] = fresh;
					if (!g_saidLegsGroup) {
						g_saidLegsGroup = true;
						Log("bridge: %s's %02Xh was playing out of another walking style and "
						    "was put back on group %d",
						    player.nick.c_str(), id, plan.group);
					}
				}
			}
		}

		const float now = ApproachWeight(Field<float>(assoc, ANIM_BLEND_AMOUNT), target, dt);
		if (!(now > 0.0f) && !(target > 0.0f)) {
			DropAnimNow(assoc);
			have[i] = nullptr;
			continue;
		}
		Field<float>(assoc, ANIM_BLEND_AMOUNT) = now;
		Field<float>(assoc, ANIM_BLEND_DELTA)  = 0.0f;
		int32_t &flags = Field<int32_t>(assoc, ANIM_FLAGS);
		flags &= ~ASSOC_DELETEFADEDOUT;
		if (id != LOCO_ANIM_IDS[LOCO_STARTWALK])
			flags |= ASSOC_RUNNING;   // they all loop; the start-walk plays once
	}

	// The stride. Every movement animation shares one normalised phase, so
	// the heaviest one's is everybody's.
	void *lead   = nullptr;
	float leadW  = -1.0f;
	for (size_t i = 0; i < LOCO_ANIMS; ++i) {
		if (!have[i] || !IsStrideAnim(LOCO_ANIM_IDS[i]))
			continue;
		const float w = Field<float>(have[i], ANIM_BLEND_AMOUNT);
		if (w > leadW) {
			leadW = w;
			lead  = have[i];
		}
	}
	float scale = 1.0f;
	if (lead && legs.havePhase) {
		const float len = AnimLength(lead);
		if (len > 0.0f) {
			const StrideStep step =
			    PlanStride(legs.phase, Field<float>(lead, ANIM_CURRENT_TIME) / len);
			if (step.seek || (strideAdded && !strideBefore)) {
				for (size_t i = 0; i < LOCO_ANIMS; ++i)
					if (have[i] && IsStrideAnim(LOCO_ANIM_IDS[i]))
						SeekAnim(have[i], legs.phase * AnimLength(have[i]));
			} else {
				scale = step.scale;
			}
		}
	}
	for (size_t i = 0; i < LOCO_ANIMS; ++i)
		if (have[i] && IsStrideAnim(LOCO_ANIM_IDS[i]))
			Field<float>(have[i], ANIM_SPEED) = legs.speed * scale;

	// The start-walk, on its own clock. Only ever put back where the sender
	// has it, never trimmed: it plays once, and a stop short of the end is
	// what hands over to the walk.
	if (void *const start = have[LOCO_STARTWALK]; start && legs.haveStartTime) {
		const float len = AnimLength(start);
		if (len > 0.0f) {
			const float want = legs.startTime < len * 0.999f ? legs.startTime : len * 0.999f;
			if (std::fabs(Field<float>(start, ANIM_CURRENT_TIME) - want) > LEGS_START_SEEK_S) {
				SeekAnim(start, want);
				Field<int32_t>(start, ANIM_FLAGS) |= ASSOC_RUNNING;
			}
		}
	}

	if (!g_saidLegs) {
		g_saidLegs = true;
		Log("bridge: %s's legs are played off their engine's weights: walk %.2f run %.2f "
		    "sprint %.2f idle %.2f start %.2f, stride %.2f",
		    player.nick.c_str(), legs.weight[LOCO_WALK], legs.weight[LOCO_RUN],
		    legs.weight[LOCO_SPRINT], legs.weight[LOCO_IDLE], legs.weight[LOCO_STARTWALK],
		    legs.havePhase ? legs.phase : -1.0f);
	}
	return true;
}

void ApplyAnimation(RemotePlayer &player, void *ped) {
	void *clump = ClumpOf(ped);
	if (!clump)
		return;

	// Before anything else gets blended: the group decides which walk a
	// walk actually is.
	ApplyAnimGroup(player, ped, clump);

	const int pedGroup = Field<int32_t>(ped, offs::PED_ANIM_GROUP);

	// The walk, run, sprint, idle and start-walk, by weight. Every frame,
	// since the weights move every frame.
	if (ApplyLegs(player, ped, clump, pedGroup)) {
		player.appliedAnimId = player.shown.animId;
		ApplyOverlay(player, ped, clump, pedGroup);
		return;
	}

	// Anything else, one id: on change, and again when the id has not
	// changed but nothing with it is live any more. Our engine does that to
	// a base on its own when it gets the ped up: PedGetupCB ends in
	// SetMoveAnim, and its blend condemns every
	// other base on the clump (addresses.h, "landing and getting up").
	const uint16_t base  = player.shown.animId;
	const bool     again = base != ANIM_NONE && base == player.appliedAnimId;
	const bool     live  = again && LookForAnim(clump, base).life == AnimLife::LIVE;
	if (BaseNeedsBlend(base, player.appliedAnimId, live) &&
	    BlendRemoteAnim(player, ped, clump, pedGroup, base, player.shown.animTime,
	                    player.shown.animSpeed, true)) {
		player.appliedAnimId = base;
		if (again && !g_saidBaseRevived) {
			g_saidBaseRevived = true;
			Log("bridge: %s's base animation %02Xh had been taken off by our own "
			    "engine and was put back",
			    player.nick.c_str(), base);
		}
	}

	// The overlay is driven every frame too, and has more ways to go: it can
	// end, freeze, be purged or be zeroed while the id on the wire never
	// moves.
	ApplyOverlay(player, ped, clump, pedGroup);
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
	if (player.shown.flags & PF_FIRING)
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

	const bool aiming = (player.shown.flags & PF_AIMING) != 0;
	if (aiming) {
		float yaw = 0.0f;
		FiniteOr(player.shown.aimYaw, Field<float>(ped, offs::PED_ROT_CUR), yaw);
		using AimFn = void(__thiscall *)(void *, float);
		Func<AimFn>(CPed__SetAimFlag)(ped, WrapAngle(yaw));
		g_replicaPitch.Set(ped, player.shown.aimPitch, FrameNow());
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

// Start a fire on this ped without waking the burning-ped logic.
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

// A mission's cutscene is playing here (SetRemotePlayersHidden): the other
// players are not drawn. bIsVisible is cleared on each of them and set again
// after on the ones this cleared it on, and on no other.
bool g_remoteHiddenForScene = false;

void ApplyRemoteSceneVisibility(RemotePlayer &player, void *ped) {
	uint8_t &flags = Field<uint8_t>(ped, offs::ENTITY_FLAGS_B);
	if (g_remoteHiddenForScene) {
		flags = static_cast<uint8_t>(flags & ~offs::ENTITY_IS_VISIBLE);
		player.hiddenForScene = true;
	} else if (player.hiddenForScene) {
		flags = static_cast<uint8_t>(flags | offs::ENTITY_IS_VISIBLE);
		player.hiddenForScene = false;
	}
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
	const bool want = (player.shown.flags & PF_ON_FIRE) != 0;
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
bool GiveBackExitDoor(void *ped, void *car);
int  AbandonPedEnterCar(void *ped);

int PassengerSlotOf(void *car, void *ped) {
	void *const   *seats = &Field<void *>(car, offs::VEH_PASSENGERS);
	const uint8_t  max   = Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS);
	const uint8_t  n = max < offs::VEH_MAX_PASSENGERS ? max : offs::VEH_MAX_PASSENGERS;
	for (uint8_t i = 0; i < n; ++i)
		if (seats[i] == ped)
			return i;
	return -1;
}

// The ped in a seat, or null. Whoever is there is taken out before somebody
// else is put in only when seatplan.h says so (SeatPedInCarAs below).
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
// The seating goes to another free passenger seat, or is refused, rather than
// forced. A remote player standing beside his own car is a cosmetic
// disagreement for as long as this machine has the local player in it; the
// local player being thrown out of a car is not. The owner's own mission
// pedestrians are kept the same way: a player's seating that would take one
// of them out is sent to another seat, or refused.
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

// Who holds a seat, as far as who may take it from them goes (seatplan.h).
// A remote player's ped and our copy of somebody else's pedestrian are told
// apart by the two indexes the damage detour already trusts; anything else in
// a seat is this machine's own engine's.
SeatHolder HolderOf(void *car, uint8_t seat) {
	void *const occupant = SeatOccupant(car, seat);
	if (!occupant)
		return SeatHolder::Empty;
	if (occupant == PlayerPed())
		return SeatHolder::LocalPlayer;
	uint16_t netId = INVALID_NETID;
	if (LookupRemotePed(occupant, netId))
		return SeatHolder::RemotePlayer;
	if (AmbientReplicaForPed(occupant, netId))
		return SeatHolder::Replica;
	return SeatHolder::EnginePed;
}

// A passenger slot whose door somebody is climbing in by is his: the entry
// ends in PedSetInCarCB's AddPassenger on that slot, which does nothing when
// the slot is full and leaves him in the car with no seat (addresses.h, the
// door flags; seatplan.h). The fourth slot has no door of its own.
uint16_t FreeSeatsOf(void *car) {
	bool        taken[SEAT_PLAN_PASSENGER_SLOTS] = {};
	void *const *seats = &Field<void *>(car, offs::VEH_PASSENGERS);
	const uint8_t gettingIn = Field<uint8_t>(car, offs::VEH_GETTING_IN_FLAGS);
	const uint8_t doorOf[SEAT_PLAN_PASSENGER_SLOTS] = {
	    offs::CAR_DOOR_FLAG_RF, offs::CAR_DOOR_FLAG_LR, offs::CAR_DOOR_FLAG_RR, 0};
	for (uint8_t i = 0; i < SEAT_PLAN_PASSENGER_SLOTS; ++i)
		taken[i] = seats[i] != nullptr || (gettingIn & doorOf[i]) != 0;
	return FreeSeatMask(Field<void *>(car, offs::VEH_DRIVER) != nullptr, taken,
	                    Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS));
}

// The warp's passenger arm takes the first free slot of the four
// (0x004D7DC9..0x004D7E3D), and the seat asked for can be a later one. So the
// ped is moved across the way the arm files him: the pointer, then a
// reference on the new slot for his destruction to nil (RegisterReference,
// as at 0x004D7DE5). The one left on the old slot is dropped by the engine's
// own PruneReferences, which unlinks every reference that no longer points
// at its entity - CAutomobile::ProcessControl calls it on every car every
// frame (0x00531A38). CPed::SetPedPositionInCar picks the seat's position by
// which slot holds the ped, so this is what sits him where his host has him.
void PutPassengerInSlot(void *car, void *ped, uint8_t slot) {
	const int at = PassengerSlotOf(car, ped);
	if (at < 0 || at == slot || slot >= SEAT_PLAN_PASSENGER_SLOTS ||
	    slot >= Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS))
		return;
	void **const seats = &Field<void *>(car, offs::VEH_PASSENGERS);
	if (seats[slot] != nullptr)
		return;
	seats[slot] = ped;
	seats[at]   = nullptr;
	using RegisterFn = void(__thiscall *)(void *, void **);
	Func<RegisterFn>(CEntity__RegisterReference)(ped, &seats[slot]);
	Func<void(__thiscall *)(void *)>(CEntity__PruneReferences)(ped);
}

// Move whoever sits in wire seat `from` along to the free wire seat `to` of the
// same car (seatplan.h, SeatMove::Shift). Only somebody sitting: a ped still
// climbing in or already getting out is left to his animation, whose end
// reads the slot. True when `from` is empty afterwards.
bool ShiftRider(void *car, uint8_t from, uint8_t to) {
	if (from == 0 || to == 0 || from > SEAT_PLAN_PASSENGER_SLOTS || to > SEAT_PLAN_PASSENGER_SLOTS)
		return false;
	void *const rider = SeatOccupant(car, from);
	if (!rider || Field<uint32_t>(rider, offs::PED_STATE) != PEDSTATE_DRIVING)
		return false;
	PutPassengerInSlot(car, rider, static_cast<uint8_t>(to - 1));
	return SeatOccupant(car, from) == nullptr;
}

// Two sitting passengers change slots. The same filing PutPassengerInSlot
// does, twice: both pointers, a reference on each new slot, and the two stale
// ones pruned - each old slot now points at the other ped, so
// PruneReferences drops it.
bool SwapRiders(void *car, uint8_t a, uint8_t b) {
	if (a == 0 || b == 0 || a == b || a > SEAT_PLAN_PASSENGER_SLOTS || b > SEAT_PLAN_PASSENGER_SLOTS)
		return false;
	const uint8_t max = Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS);
	if (a > max || b > max)
		return false;
	void **const seats = &Field<void *>(car, offs::VEH_PASSENGERS);
	void *const  pa    = seats[a - 1];
	void *const  pb    = seats[b - 1];
	if (!pa || !pb || Field<uint32_t>(pa, offs::PED_STATE) != PEDSTATE_DRIVING ||
	    Field<uint32_t>(pb, offs::PED_STATE) != PEDSTATE_DRIVING)
		return false;
	seats[a - 1] = pb;
	seats[b - 1] = pa;
	using RegisterFn = void(__thiscall *)(void *, void **);
	Func<RegisterFn>(CEntity__RegisterReference)(pb, &seats[a - 1]);
	Func<RegisterFn>(CEntity__RegisterReference)(pa, &seats[b - 1]);
	Func<void(__thiscall *)(void *)>(CEntity__PruneReferences)(pa);
	Func<void(__thiscall *)(void *)>(CEntity__PruneReferences)(pb);
	return true;
}

// Said a few times, because who could not be seated where is the first thing
// to look at when somebody stands in a car on one screen.
int g_seatRefusalsSaid = 0;

const char *HolderName(SeatHolder h) {
	switch (h) {
	case SeatHolder::LocalPlayer:  return "us";
	case SeatHolder::RemotePlayer: return "another player";
	case SeatHolder::Replica:      return "our copy of somebody else's pedestrian";
	case SeatHolder::EnginePed:    return "a pedestrian our own engine runs";
	default:                       return "nobody";
	}
}

// COMMAND_WARP_CHAR_INTO_CAR's order, SetObjective then WarpPedIntoCar, into
// the seat seatplan.h gives. The seat the ped ended up in, or
// AMBIENT_SEAT_NONE_FREE when there was none to give (nothing was touched),
// or AMBIENT_SEAT_REFUSED when the engine would not seat him.
int SeatPedInCarAs(void *ped, void *car, uint8_t seat, SeatComer who) {
	if (!ped || !car)
		return AMBIENT_SEAT_REFUSED;

	// A dead ped can't be seated. Failing quietly here beats failing inside
	// the warp: CPed::SetObjective returns on PED_DIE/PED_DEAD before writing
	// anything, so the objective would stay whatever it already was and
	// WarpPedIntoCar would take its no-seat branch.
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return AMBIENT_SEAT_REFUSED;

	// Never over the top of the local player, and never taking this
	// machine's own pedestrian out of his seat for somebody else's say-so:
	// seatplan.h has both arguments. The eviction that is left is the one
	// the session decides - a jack's driver, a stale copy of a player.
	const SeatHolder holder = HolderOf(car, seat);
	SeatPlan         plan   = PlanSeat(seat, holder, FreeSeatsOf(car), who);
	if (plan.move == SeatMove::Shift) {
		if (ShiftRider(car, plan.seat, plan.shiftTo)) {
			if (g_seatRefusalsSaid < 4) {
				++g_seatRefusalsSaid;
				Log("bridge: seat %u holds %s, who moves along to seat %u so that "
				    "another player sits where his own screen has him",
				    seat, HolderName(holder), plan.shiftTo);
			}
		} else {
			plan = SeatPlan{SeatMove::Elsewhere, plan.shiftTo};
		}
	}
	if (plan.move == SeatMove::Refuse) {
		if (g_seatRefusalsSaid < 4) {
			++g_seatRefusalsSaid;
			Log("bridge: nowhere to seat %s in that car: seat %u holds %s and no "
			    "other passenger seat is free",
			    who == SeatComer::Replica ? "a copy of somebody else's pedestrian"
			                              : "another player",
			    seat, HolderName(holder));
		}
		return AMBIENT_SEAT_NONE_FREE;
	}
	if (plan.move == SeatMove::Evict)
		if (void *const occupant = SeatOccupant(car, plan.seat))
			UnseatPedFromCar(occupant);
	if (plan.move == SeatMove::Elsewhere && g_seatRefusalsSaid < 4) {
		++g_seatRefusalsSaid;
		Log("bridge: seat %u holds %s, so %s sits in seat %u instead", seat,
		    HolderName(holder),
		    who == SeatComer::Replica ? "a copy of somebody else's pedestrian"
		                              : "another player",
		    plan.seat);
	}
	const uint8_t target = plan.seat;

	const uint32_t objective = target == 0 ? OBJECTIVE_ENTER_CAR_AS_DRIVER
	                                       : OBJECTIVE_ENTER_CAR_AS_PASSENGER;

	using ObjectiveFn = void(__thiscall *)(void *, uint32_t, void *);
	using WarpFn      = void(__thiscall *)(void *, void *);

	// The warp replaces m_pMyVehicle. A door or rolling-close animation still
	// playing would finish on this car, or on none (game/animcb.h).
	LetGoOfCarChain(ped);

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
	// stopped running, and the driver logic drove it instead - MISSION_NONE's
	// brake and handbrake, or the cruise of a traffic car he'd taken.
	// game/carstatus.h has the rest.
	uint8_t &flags = Field<uint8_t>(car, offs::ENTITY_FLAGS);
	const uint8_t before = static_cast<uint8_t>(flags >> ENTITY_STATUS_SHIFT);

	Func<WarpFn>(CPed__WarpPedIntoCar)(ped, car);

	// Did it actually take? The warp fails silently and half-applied, so
	// what gets checked here is the seat pointer - the thing it would have
	// skipped on failure.
	const bool seated = target == 0 ? Field<void *>(car, offs::VEH_DRIVER) == ped
	                                : PassengerSlotOf(car, ped) >= 0;
	if (seated && target != 0)
		PutPassengerInSlot(car, ped, static_cast<uint8_t>(target - 1));

	// A seating that didn't take changes nothing about the car either.
	const uint8_t after = seated ? StatusAfterSeating(before, target == 0) : before;
	flags = static_cast<uint8_t>((flags & 0x07u) | (after << ENTITY_STATUS_SHIFT));

	if (!seated) {
		UnseatPedFromCar(ped);
		return AMBIENT_SEAT_REFUSED;
	}
	return target == 0 ? 0 : 1 + PassengerSlotOf(car, ped);
}

bool SeatPedInCar(void *ped, void *car, uint8_t seat) {
	return SeatPedInCarAs(ped, car, seat, SeatComer::RemotePlayer) >= 0;
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

	// Before anything here touches the seat or m_pMyVehicle, and before the
	// partial fade below would bring any of their callbacks forward to the
	// next frame (game/animcb.h).
	LetGoOfCarChain(ped);

	// Every vehicle CoopIII creates is a CAutomobile or a CBoat (vehicle.cpp,
	// game/boat.h), so this vtable check is the same net ResolveRemote uses on
	// peds - a slot that still resolves while no longer holding what we think
	// it does. Everything written below is CVehicle's or CPhysical's, so it is
	// valid on both. Leaving the boat out would skip RemoveDriver for a boat,
	// and destroying it would then leave its pDriver pointing at our ped.
	if (car && IsBuiltVehicleVtable(Field<uintptr_t>(car, offs::VTABLE))) {
		// Before anything else, while the ped still says how it was leaving.
		// A replica whose exit is still playing when its owner's exit packet
		// lands is taken out right here, halfway through the animation, and
		// the door it claimed has to be given back the way ~CPed would.
		GiveBackExitDoor(ped, car);

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

			// What the engine's own exit does for a driver and the script's
			// warp does not: a police car, Enforcer or Rhino stops being locked
			// the moment its driver is out (game/carremoval.h).
			ReleaseInitialDoorLock(car);
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
	// remote ped must not run its own NPC logic". Do not remove either as
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

// Something that changes for as long as an entry's animation is playing, for
// EntryWatch (client.h). Every link of the chain is m_pVehicleAnim in turn,
// and a playing association's currentTime moves every frame; which one it is
// and where it has got to is enough. 0 when there is none.
uint32_t EntryProgressMark(void *ped) {
	void *const anim = Field<void *>(ped, offs::PED_VEHICLE_ANIM);
	if (!anim)
		return 0;
	const uint32_t time = Field<uint32_t>(anim, ANIM_CURRENT_TIME);   // the float's bits
	const uint32_t id   = static_cast<uint32_t>(Field<int32_t>(anim, ANIM_ID));
	return (time ^ (id << 20) ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(anim))) | 1u;
}

// The objective triple, cleared. Written out rather than routed through
// CPed::ClearObjective for the reason UnseatPedFromCar gives: what is wanted
// is a flat "no objective", not whatever SetObjective would restore instead,
// and OBJECTIVE_NONE is half of the off switch for a remote ped's NPC logic
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

// The exit's half of the same problem. A ped taken out of a seat while its
// get-out animation is still playing never reaches PedSetOutCarCB, which is
// what clears its door's bit in m_nGettingOutFlags - and
// SetEnterCar refuses a door whose bit is set, silently, for as long as the
// car lives. addresses.h, GettingOutFlagsAfterUnseat, has the three sites.
//
// The door is shut as well: the animation swung it open, and a few tests
// after the flags SetEnterCar asks IsDoorReady || IsDoorFullyOpen (vtable
// 0x60 and 0x64, at 0x004E09F1 and 0x004E0A00), which a door left hanging
// halfway fails just as surely.
//
// Returns true if there was a door to give back. Only ever does anything for
// a ped in PED_EXIT_CAR or PED_DRAG_FROM_CAR, so it is safe on any ped.
bool GiveBackExitDoor(void *ped, void *car) {
	if (!ped || !car)
		return false;
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	const uint16_t door  = Field<uint16_t>(ped, offs::PED_VEH_DOOR);
	const uint8_t  flag  = DoorFlag(door);
	if (!flag)
		return false;

	uint8_t      &out    = Field<uint8_t>(car, offs::VEH_GETTING_OUT_FLAGS);
	const uint8_t before = out;
	out = GettingOutFlagsAfterUnseat(before, state, flag);
	if (out == before)
		return false;

	ShutDoorAfterAbandonedEntry(car, door);

	static int said = 0;
	if (said < 3) {
		++said;
		Log("bridge: a ped left a car halfway through getting out (state %u, "
		    "door %u); gave the door back, getting-out flags 0x%02X -> 0x%02X",
		    state, door, before, out);
	}
	return true;
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
	// QuitEnteringCar fades m_pVehicleAnim and the partial fade below fades the
	// rest, and neither takes a callback off: each would fire a frame later, by
	// then on a ped the caller may have warped into a seat (game/animcb.h).
	DropCarChainCallbacks(ped);
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

bool BeginPedEnterCar(void *ped, void *car, uint8_t seat, uint8_t doorSeat,
                      SeatComer who = SeatComer::RemotePlayer) {
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

	// Not into a seat the local player is in, nor into one a pedestrian holds:
	// this entry ends in PedSetInCarCB putting the ped in the slot its door
	// names, on top of whoever is there, a second from now. The warp behind
	// it asks seatplan.h and finds another seat (DoorEntryMayTake).
	//
	// A player's seat held by this machine's own pedestrian is his once that
	// pedestrian has moved along to another free seat of the car, the move
	// the warp would make (seatplan.h, SeatMove::Shift).
	SeatHolder holder = HolderOf(car, seat);
	if (holder == SeatHolder::EnginePed && seat != 0 && who == SeatComer::RemotePlayer) {
		const SeatPlan plan = PlanSeat(seat, holder, FreeSeatsOf(car), who);
		if (plan.move == SeatMove::Shift && ShiftRider(car, seat, plan.shiftTo))
			holder = SeatHolder::Empty;
	}
	if (!DoorEntryMayTake(seat, holder, who))
		return false;

	// Make room, the way the warp does, for the two the door may take: a
	// jack's driver and a stale copy of another player. Two peds cannot both
	// be the driver.
	if (holder != SeatHolder::Empty)
		if (void *const occupant = SeatOccupant(car, seat))
			if (occupant != ped)
				UnseatPedFromCar(occupant);

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

// ---- a jack, on a replica ---------------------------------------------------
//
// CPed::SetCarJack_AllClear on the replica of a player whose own engine is
// jacking (S_JackingVehicle). From there this machine's engine plays the whole
// thing by itself: align, open the door, PedAnimDoorOpenCB calls
// SetBeingDraggedFromCar on whoever is in that seat HERE, the jacker pulls,
// climbs in, shuts the door, and PedSetInCarCB seats him. addresses.h, "a jack,
// played on a replica", has each step.
//
// Not SetCarJack itself: its MISSION_VEHICLE gate at 0x004E032B refuses a
// CCivilianPed on every car a session has, and that gate is there to stop NPC
// peds stealing mission cars. This jack was already decided on the jacker's
// machine. The other gates are asked below, in SetCarJack's order, so a jack
// that would not start there does not start here either.
//
// **The seat this empties may be the local player's, and that is the point.**
// SeatHeldByLocalPlayer refuses a replica any *seating* over him, and still
// does. This is not a seating: it is the engine's own jack, and on the
// victim's machine the victim's own engine is the one dragging him out - the
// only machine that may. It is only ever started from S_JackingVehicle.

// The door flag of the seat `ped` holds in `car`, or 0 for a seat without a
// door of its own. DoorForSeat read for the seat the ped is in.
uint8_t SeatDoorFlagOf(void *car, void *ped) {
	if (Field<void *>(car, offs::VEH_DRIVER) == ped)
		return DoorFlag(DoorForSeat(0));
	const int slot = PassengerSlotOf(car, ped);
	return slot >= 0 && slot < 3 ? DoorFlag(DoorForSeat(static_cast<uint8_t>(slot + 1)))
	                             : uint8_t{0};
}

// Is our engine taking this ped out of its seat through a jack played here? A
// PullOut (client.h), off addresses.h's EngineTakingPedOut.
uint8_t PedPullOut(void *ped) {
	if (!ped)
		return PULL_NONE;
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	// Through the tail too: PedSetDraggedOutCarCB clears bInVehicle and the
	// state stays 33h until the delete callback puts the ped down beside the
	// car. Touched in between, that callback lands on top of whatever we did.
	if (state == PEDSTATE_DRAG_FROM_CAR)
		return PULL_DRAGGED;
	if (!Field<bool>(ped, offs::PED_IN_VEHICLE))
		return PULL_NONE;
	void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
	if (!car || Field<uintptr_t>(car, offs::VTABLE) != CAutomobile__vtable)
		return PULL_NONE;
	return EngineTakingPedOut(state, SeatDoorFlagOf(car, ped),
	                          Field<uint8_t>(car, offs::VEH_GETTING_IN_FLAGS),
	                          Field<uint8_t>(car, offs::VEH_FLAGS_C))
	           ? PULL_COMING
	           : PULL_NONE;
}

bool BeginPedJackCar(void *ped, void *car, uint8_t doorSeat) {
	if (!ped || !car || ped == PlayerPed())
		return false;

	// Doors that swing, so a CAutomobile, which also rules out a boat. Not a
	// bus: SetCarJack's bus arm takes the driver whatever the door, and
	// PedAnimDoorOpenCB then plays an ordinary get-in on it.
	if (Field<uintptr_t>(car, offs::VTABLE) != CAutomobile__vtable)
		return false;
	if (Field<uint8_t>(car, offs::VEH_FLAGS_B_BUS) & offs::VEH_IS_BUS)
		return false;

	const uint16_t door     = DoorForSeat(doorSeat);
	const uint8_t  flag     = DoorFlag(door);
	const uint32_t doorEnum = CarDoorEnumFor(door);
	if (!door || !flag || !doorEnum)
		return false;

	// The ped in that door's seat, and SetCarJack's three questions about it
	// (0x004E0347, 0x004E038C, 0x004E0390): not doing a drive-by, there, and
	// in PED_DRIVING. PedAnimDoorOpenCB asks the last again and adds
	// bDontDragMeOutCar (0x004DE864); asked here so the refusal is ours.
	void *const victim = SeatOccupant(car, doorSeat);
	if (!victim || victim == ped)
		return false;
	if (Field<uint32_t>(victim, offs::PED_STATE) != PEDSTATE_DRIVING)
		return false;
	if (Field<uint8_t>(victim, offs::PED_FLAGS_F) & offs::PED_DONT_DRAG_ME_OUT)
		return false;
	if (Func<bool(__thiscall *)(void *)>(CPed__IsPedDoingDriveByShooting)(victim))
		return false;

	// The jacker: alive, on foot, not already entering (0x004E0360, 0x004E03B7).
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD || state == PEDSTATE_CARJACK)
		return false;
	if (Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;
	if (Field<void *>(ped, offs::PED_VEHICLE_ANIM))
		return false;
	if (!(Field<float>(ped, offs::PED_HEALTH) > 0.0f))
		return false;
	if (!ClumpOf(ped))
		return false;

	// The door: free on both sides, nobody else jacking this car, and ready or
	// fully open (vtable 60h then 64h, 0x004E03A1 / 0x004E03B0).
	if (Field<uint8_t>(car, offs::VEH_GETTING_IN_FLAGS) & flag)
		return false;
	if (Field<uint8_t>(car, offs::VEH_GETTING_OUT_FLAGS) & flag)
		return false;
	if (Field<uint8_t>(car, offs::VEH_FLAGS_C) & offs::VEH_IS_BEING_CARJACKED)
		return false;
	using DoorTestFn  = bool(__thiscall *)(void *, uint32_t);
	const uintptr_t vt = Field<uintptr_t>(car, offs::VTABLE);
	const auto ready   = *reinterpret_cast<DoorTestFn *>(vt + VEH_VT_IS_DOOR_READY);
	const auto open    = *reinterpret_cast<DoorTestFn *>(vt + VEH_VT_IS_DOOR_FULLY_OPEN);
	if (!ready(car, doorEnum) && !open(car, doorEnum))
		return false;

	// Ours, as for any entry: a car that is going anywhere, or a jacker who is
	// across the street.
	const float *const carPos   = &Field<float>(car, offs::POSITION);
	const float *const pedPos   = &Field<float>(ped, offs::POSITION);
	const float *const carSpeed = &Field<float>(car, offs::MOVE_SPEED);
	const float        speedSq  = carSpeed[0] * carSpeed[0] +
	                      carSpeed[1] * carSpeed[1] + carSpeed[2] * carSpeed[2];
	if (!(speedSq <= ENTER_MAX_CAR_SPEED_SQ))
		return false;
	if (DistanceSq(pedPos, carPos) > ENTER_MAX_PED_DIST_SQ)
		return false;

	// The objective first, for two readers: PedAnimPullPedOutCB quits anything
	// that is not ENTER_CAR_AS_DRIVER (0x004DEC03), and PedSetInCarCB picks
	// SetDriver from it. A jack is always for the wheel. PedSetInCarCB's
	// RestorePreviousObjective puts back the NONE the replica had, as for an
	// ordinary entry.
	using ObjectiveFn = void(__thiscall *)(void *, uint32_t, void *);
	Func<ObjectiveFn>(CPed__SetObjective)(ped, OBJECTIVE_ENTER_CAR_AS_DRIVER, car);

	// PedAnimAlignCB and PedAnimDoorOpenCB switch on m_vehDoor, not on the
	// argument; SetCarJack would have left it set by SeekCar.
	Field<uint16_t>(ped, offs::PED_VEH_DOOR) = door;

	using AllClearFn = void(__thiscall *)(void *, void *, uint32_t, uint32_t);
	Func<AllClearFn>(CPed__SetCarJack_AllClear)(ped, car, door, flag);

	if (Field<uint32_t>(ped, offs::PED_STATE) != PEDSTATE_CARJACK ||
	    !PedIsEnteringCar(ped, car)) {
		ClearPedObjective(ped);
		return false;
	}
	return true;
}

// The way out, animated. There is no guard list to mirror here because
// SetExitCar does its own: CanPedExitCar covers the car being upside down or
// moving too fast, and the PED_EXIT_CAR / PED_DRAG_FROM_CAR test covers
// being asked twice. The door is the one the ped's seat has: SetExitCar's own
// pick for a pedestrian's copy, and for another player's copy riding as a
// passenger that seat's door whatever the room test says, the door his own
// screen has him leave by (game/passexit.h).
bool BeginPedExitCar(void *ped, bool player) {
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

	ExitCarByOwnDoor(ped, car, player);

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
	if (!BeginPedEnterCar(ped, car, seat, doorSeat))
		return false;
	player.enterWatch.Begin(WallClock::NowMs(), EntryProgressMark(ped));
	return true;
}

uint8_t PollRemoteEntry(RemotePlayer &player, void *ped, void *car, uint8_t seat);

uint8_t PollSeatRemotePed(RemotePlayer &player, RemoteVehicle &vehicle, uint8_t seat) {
	void *const ped = ResolveRemote(player);
	if (!ped)
		return SEAT_LOST;
	void *const car = ResolveRemoteVehicle(vehicle);
	if (!car)
		return SEAT_LOST;
	return PollRemoteEntry(player, ped, car, seat);
}

// The car of a jack is named by pool handle, because it may be traffic. Same
// vtable test as everywhere a handle is turned back into a car: a slot the
// engine has reused resolves, and is not the car.
void *JackCar(int32_t carHandle) {
	void *const car = carHandle >= 0 ? AmbientCarFromRef(carHandle) : nullptr;
	return car && Field<uintptr_t>(car, offs::VTABLE) == CAutomobile__vtable ? car : nullptr;
}

bool BeginJackRemotePed(RemotePlayer &player, int32_t carHandle, uint8_t doorSeat) {
	void *const ped = ResolveRemote(player);
	if (!ped)
		return false;
	void *const car = JackCar(carHandle);
	if (!car)
		return false;

	// Who is in that seat here, for the log: the local player on the victim's
	// own machine, a replica or a traffic driver anywhere else.
	void *const victim = SeatOccupant(car, doorSeat);
	if (!BeginPedJackCar(ped, car, doorSeat)) {
		// The jacker's machine has already pulled our pedestrian out of that
		// seat on its screen. Our copy of the jacker could not start the jack
		// here - the car still rolling under our driver, the jacker's copy a
		// few metres behind his own - so our driver is dragged out anyway,
		// the engine's own drag, and lands beside the door as he does there.
		// Left in the seat he drives on here while the jacker drives it
		// there, and every other screen puts him where his seat is.
		uint16_t hostedNet = INVALID_NETID;
		if (victim && victim != PlayerPed() && HostedPedNetIdFor(victim, hostedNet) &&
		    Field<uint32_t>(victim, offs::PED_STATE) == PEDSTATE_DRIVING) {
			using DragFn = void(__thiscall *)(void *, void *, uint32_t, uint8_t);
			Func<DragFn>(CPed__SetBeingDraggedFromCar)(victim, car, DoorForSeat(doorSeat), 0);
			static bool saidDragged = false;
			if (!saidDragged) {
				saidDragged = true;
				Log("bridge: %s's jack would not start here, and the driver is our "
				    "pedestrian net %u; dragged out of the car by our engine as on "
				    "their screen (state %u)",
				    player.nick.c_str(), hostedNet, Field<uint32_t>(victim, offs::PED_STATE));
			}
		}
		static int said = 0;
		if (said < 3) {
			++said;
			Log("bridge: %s's jack would not start here (seat of door %u holds %s, "
			    "state %u); they get in at the claim instead",
			    player.nick.c_str(), doorSeat,
			    !victim ? "nobody" : victim == PlayerPed() ? "us" : "a ped",
			    victim ? Field<uint32_t>(victim, offs::PED_STATE) : 0u);
		}
		return false;
	}
	player.enterWatch.Begin(WallClock::NowMs(), EntryProgressMark(ped));

	static int said = 0;
	if (said < 3) {
		++said;
		Log("bridge: %s is jacking a car through the door of seat %u; our engine "
		    "drags out %s",
		    player.nick.c_str(), doorSeat,
		    victim == PlayerPed() ? "the local player" : "the ped in that seat");
	}
	return true;
}

uint8_t PollJackRemotePed(RemotePlayer &player, int32_t carHandle) {
	void *const ped = ResolveRemote(player);
	if (!ped)
		return SEAT_LOST;
	void *const car = JackCar(carHandle);
	if (!car)
		return SEAT_LOST;
	return PollRemoteEntry(player, ped, car, 0);
}

uint8_t RemoteBeingPulledOut(RemotePlayer &player) {
	return PedPullOut(ResolveRemote(player));
}

uint8_t LocalBeingPulledOut() { return PedPullOut(PlayerPed()); }

uint8_t PollRemoteEntry(RemotePlayer &player, void *ped, void *car, uint8_t seat) {
	const uint8_t progress = PollPedEnterCar(ped, car, seat);
	if (progress != SEAT_RUNNING)
		return progress;

	// Still in PED_ENTER_CAR, but is the animation going anywhere? A stuck one
	// is handed back as lost now rather than left to the deadline, which is
	// long enough for the slowest door there is. client.h, EntryWatch.
	if (player.enterWatch.Stalled(WallClock::NowMs(), EntryProgressMark(ped))) {
		static bool said = false;
		if (!said) {
			said = true;
			Log("bridge: %s's door-opening animation stopped moving for %u ms, so "
			    "the entry is given up on",
			    player.nick.c_str(), SEAT_ANIM_STALL_MS);
		}
		return SEAT_LOST;
	}
	return SEAT_RUNNING;
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
	return ped && BeginPedExitCar(ped, /*player=*/true);
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
static_assert(PEDSTATE_ENTER_CAR == PEDSTATE_ON_WIRE_ENTER_CAR &&
                  PEDSTATE_CARJACK == PEDSTATE_ON_WIRE_CARJACK,
              "protocol.h's PED_ENTER_CAR and PED_CARJACK, which the server reads, "
              "must match addresses.h's");

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

// The gun in the hand: the uzi for a driver, the passenger's own gun for a
// passenger (passengeraim.h, SeatedPoseWeapon). The player's own seat keeps
// the uzi there (RemoveWeaponWhenEnteringVehicle, player arm), but the warp
// that seats a remote ped takes the model off, so it goes back on here.
// SetCurrentWeapon removes every weapon atomic before it adds one
// (0x004CFA94, and RemoveWeaponModel ignores its argument), so this never
// stacks two.
void ArmForDriveBy(RemotePlayer &player, void *ped) {
	const uint8_t want = SeatedPoseWeapon(player.last.weapon);
	if (!GiveWeaponTo(player, ped, want))
		return;   // streaming; next frame
	player.driveByArmed = true;
	if (Field<int32_t>(ped, offs::PED_WEP_MODEL_ID) != -1 &&
	    Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON) == want)
		return;
	void *const info = WeaponInfo(want);
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
	Func<SetFn>(CPed__SetCurrentWeapon)(ped, want);
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

// ---- the diagonal walk's legs ----------------------------------------------
//
// remoteloco.h has the why and addresses.h the instructions. ApplyRemotePose
// writes the yaw for this frame into g_legTwist from PreFrame; the redirected
// call to CalculateNewVelocity then does, for a remote player's copy, what the
// engine's own tail of that function does for the local player alone: turn
// both upper legs with RotateTorso. Same place in the frame as the engine's,
// after the animation walk has posed the clump, so nothing re-poses it before
// it is drawn, and it happens once per animation update.

LegTwistTable g_legTwist;
bool          g_legTwistRedirected = false;
bool          g_saidLegTwist       = false;

struct LimbOrientation {
	float yaw;
	float pitch;
};

void TwistUpperLegs(void *ped, float yaw) {
	void *const clump = ClumpOf(ped);
	if (!clump)
		return;
	// The engine's own two conditions on the clump: the idle mostly off, and
	// no fight stance.
	if (void *const idle = FindAnimById(clump, ANIM_STD_IDLE))
		if (!(Field<float>(idle, ANIM_BLEND_AMOUNT) < LEG_TWIST_IDLE_MAX_BLEND))
			return;
	if (FindAnimById(clump, ANIM_STD_FIGHT_IDLE_ID))
		return;

	void *const left  = Field<void *>(ped, offs::PED_FRAMES + PED_NODE_UPPERLEGL * sizeof(void *));
	void *const right = Field<void *>(ped, offs::PED_FRAMES + PED_NODE_UPPERLEGR * sizeof(void *));
	if (!left || !right)
		return;

	using RotateFn = void(__thiscall *)(void *, void *, LimbOrientation *, uint8_t);
	void *const ik = reinterpret_cast<uint8_t *>(ped) + offs::PED_IK;
	LimbOrientation limb{yaw, 0.0f};
	Func<RotateFn>(CPedIK__RotateTorso)(ik, left, &limb, 0);
	limb = LimbOrientation{yaw, 0.0f};
	Func<RotateFn>(CPedIK__RotateTorso)(ik, right, &limb, 0);

	if (!g_saidLegTwist) {
		g_saidLegTwist = true;
		Log("ped: a remote player walking at an angle to their facing has their legs "
		    "turned %.0f degrees toward it, as their own engine does",
		    yaw * 57.2957795f);
	}
}

void __fastcall NewVelocityThenLegs(void *ped, void * /*edx*/) {
	Func<void(__thiscall *)(void *)>(CPed__CalculateNewVelocity)(ped);
	float yaw = 0.0f;
	if (g_legTwist.Find(ped, FrameNow(), yaw))
		TwistUpperLegs(ped, yaw);
}

bool RedirectCallSite(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

// The twist for this frame, from PreFrame. Eased toward the target, and only
// written into the table while there is something to turn.
void PlanRemoteLegTwist(RemotePlayer &player, void *ped, float heading) {
	const float target =
	    player.haveDrawn
	        ? LegTwistFor(heading, player.drawnMps.x, player.drawnMps.y,
	                      LegTwistState(player.shown.pedState))
	        : 0.0f;
	player.legTwist = ApproachLegTwist(player.legTwist, target, FrameSeconds());
	if (std::fabs(player.legTwist) > 0.01f)
		g_legTwist.Set(ped, player.legTwist, FrameNow());
}

// ---- a copy left lying down (animrevive.h, StandUpDue) ---------------------

bool g_saidStoodUp = false;

// Is our copy down: in PED_FALL or PED_GETUP by its own engine, or carrying a
// live knockdown the wire does not name?
bool CopyIsDown(RemotePlayer &player, void *clump, uint32_t pedState) {
	if (PedDownOrGettingUp(pedState))
		return true;
	bool down = false;
	ForEachAnim(clump, [&](void *assoc) {
		if (down || !(Field<int32_t>(assoc, ANIM_FLAGS) & ASSOC_PARTIAL))
			return;
		const int32_t id = Field<int32_t>(assoc, ANIM_ID);
		if (id < 0 || !IsKnockdownAnim(static_cast<uint16_t>(id)) ||
		    id == static_cast<int32_t>(player.shown.animId2))
			return;
		down = LifeOf(assoc) == AnimLife::LIVE && Field<float>(assoc, ANIM_BLEND_AMOUNT) > 0.0f;
	});
	return down;
}

// What PedGetupCB does at the end of a get-up, without the get-up: the
// knockdown and get-up partials go, the state is PED_IDLE (docs/protocol.md
// 1.13.3, where a copy is kept), and the three bits a get-up clears are
// cleared. The legs and the overlay are ApplyAnimation's, straight after.
void StandCopyUp(RemotePlayer &player, void *ped, void *clump, uint32_t pedState) {
	int condemned = 0;
	ForEachAnim(clump, [&](void *assoc) {
		const int32_t flags = Field<int32_t>(assoc, ANIM_FLAGS);
		if (!(flags & ASSOC_PARTIAL))
			return;
		const int32_t id = Field<int32_t>(assoc, ANIM_ID);
		if (id < 0 || id == static_cast<int32_t>(player.shown.animId2))
			return;
		if (!IsKnockdownAnim(static_cast<uint16_t>(id)) && !IsGetUpAnim(static_cast<uint16_t>(id)))
			return;
		if (LifeOf(assoc) == AnimLife::CONDEMNED)
			return;
		Field<float>(assoc, ANIM_BLEND_DELTA) = -8.0f;
		Field<int32_t>(assoc, ANIM_FLAGS)     = flags | ASSOC_DELETEFADEDOUT;
		++condemned;
	});

	if (PedDownOrGettingUp(pedState)) {
		Field<uint32_t>(ped, offs::PED_STATE)      = PEDSTATE_IDLE;
		Field<uint32_t>(ped, offs::PED_LAST_STATE) = PEDSTATE_NONE;
		Func<void(__thiscall *)(void *, int32_t)>(CPed__SetMoveState)(ped, PEDMOVE_STILL);
	}
	Field<uint8_t>(ped, offs::PED_FLAGS_C) &= static_cast<uint8_t>(~offs::PED_UPDATE_ANIM_HEADING);
	Field<uint8_t>(ped, offs::PED_FLAGS_E) &= static_cast<uint8_t>(~offs::PED_GETUP_ANIM_STARTED);
	Field<uint8_t>(ped, offs::PED_FLAGS_I) &= static_cast<uint8_t>(~offs::PED_FALLEN_DOWN);

	if (IsKnockdownAnim(player.appliedAnimId2) || IsGetUpAnim(player.appliedAnimId2))
		player.appliedAnimId2 = ANIM_NONE;

	if (!g_saidStoodUp) {
		g_saidStoodUp = true;
		Log("bridge: %s's copy was still down here (state %u, %d knockdown/get-up "
		    "animation(s)) %u ms after they were up on their own screen; stood it up "
		    "(said once)",
		    player.nick.c_str(), pedState, condemned, STAND_UP_AFTER_MS);
	}
}

void ReconcileCopyDown(RemotePlayer &player, void *ped, uint32_t pedState) {
	void *const clump = ClumpOf(ped);
	if (!clump)
		return;
	const bool ownerUp = !player.dead && OwnerIsUp(player.shown.pedState, player.shown.animId2);
	const bool down    = ownerUp && CopyIsDown(player, clump, pedState);
	if (StandUpDue(player.downTiming, player.downSinceMs, down, ownerUp, WallClock::NowMs()))
		StandCopyUp(player, ped, clump, pedState);
}

bool RebuildForFreedLookSlot(RemotePlayer &player, void *ped);

void ApplyRemotePose(RemotePlayer &player, const Pose &pose) {
	void *ped = ResolveRemote(player);
	if (!ped)
		return;
	if (RebuildForFreedLookSlot(player, ped))
		return;

	// Before every early return too: a seated or dead player is in the scene
	// as much as a standing one. Only the bit this set is ever cleared again.
	ApplyRemoteSceneVisibility(player, ped);

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
	//
	// PED_DRAG_FROM_CAR too, for the frame between the drag's finish callback
	// clearing bInVehicle and its delete callback putting the ped down beside
	// the door: a jack played here is still carrying the ped out.
	const uint32_t pedState      = Field<uint32_t>(ped, offs::PED_STATE);
	const bool     engineHasThem = Field<bool>(ped, offs::PED_IN_VEHICLE) ||
	                           pedState == PEDSTATE_ENTER_CAR ||
	                           pedState == PEDSTATE_CARJACK ||
	                           pedState == PEDSTATE_DRAG_FROM_CAR;
	if ((player.Seated() || player.Entering()) && engineHasThem) {
		Field<float>(ped, offs::PED_HEALTH) = ReplicaHealth(player.last.health, player.deathApplied);
		Field<float>(ped, offs::PED_ARMOUR) = player.last.armour;
		ApplySeatedDriveBy(player, ped, pedState);
		KeepSeatedPose(ped);
		return;
	}
	EndSeatedDriveBy(player, ped);

	// In a seat by our engine's account while the session has not caught up:
	// the engine places a seated ped from its car every frame, and nothing
	// the pose stream would play here - a walk, the owner's knockdown - may
	// go on somebody sitting in a car.
	if (KeepSeatedPose(ped)) {
		Field<float>(ped, offs::PED_HEALTH) = ReplicaHealth(player.last.health, player.deathApplied);
		Field<float>(ped, offs::PED_ARMOUR) = player.last.armour;
		return;
	}

	// Position and facing go through PlaceRemotePed, which also pushes the
	// matrix into the clump's RenderWare frame and re-files the ped in the
	// sector grid. Writing the matrix alone is exactly what made the first
	// two remote players invisible - see the comment on that function.
	PlaceRemotePed(ped, pose.pos, pose.heading, /*inWorld=*/true);

	// Heading is a scalar yaw (docs/protocol.md §1.7). Both members get set,
	// same as the engine's own SetHeading - leave m_fRotationDest alone and
	// the ped's own logic just rotates it straight back.
	Field<float>(ped, offs::PED_ROT_CUR)  = WrapAngle(pose.heading);
	Field<float>(ped, offs::PED_ROT_DEST) = WrapAngle(pose.heading);

	// Velocity gets written so the engine's own animation picking and
	// collision see a moving ped, not one that teleports every frame.
	// Off the wire, and the engine moves the ped by it and files it in the
	// sector grid before the next frame's ClampToWorld: a number, and no
	// more than 5 units a step (250 m/s at 50 steps a second).
	//
	// The drawn pose's own, when there is one, rather than the snapshot's:
	// the engine moves the ped by it once more before the frame is drawn, and
	// the snapshot's is up to 40 ms older than the pose. At a stop that was a
	// ped nudged on at running speed for a frame and pulled back the next,
	// for as long as the older snapshot was the one shown.
	const Vec3 speed =
	    player.haveDrawn
	        ? Vec3{player.drawnMps.x / ENGINE_STEPS_PER_SECOND, player.drawnMps.y / ENGINE_STEPS_PER_SECOND,
	               player.drawnMps.z / ENGINE_STEPS_PER_SECOND}
	        : player.shown.moveSpeed;
	float *vel = &Field<float>(ped, offs::MOVE_SPEED);
	vel[0]     = WireSpeed(speed.x);
	vel[1]     = WireSpeed(speed.y);
	vel[2]     = WireSpeed(speed.z);

	// The owner's health, but never low enough for our own engine to kill the
	// ped with its default fall before the owner's death, with the animation
	// the owner's engine picked, has been applied (remotebody.h).
	Field<float>(ped, offs::PED_HEALTH) = ReplicaHealth(player.last.health, player.deathApplied);
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
	Field<uint32_t>(ped, offs::PED_MOVE_STATE) = ClampMoveState(player.shown.moveState);

	ApplyWeapon(player, ped);
	ApplyFiring(player, ped);
	ApplyAim(player, ped);
	// Before the animation, so a copy stood up here gets its legs and overlay
	// on the same frame.
	ReconcileCopyDown(player, ped, pedState);
	ApplyAnimation(player, ped);
	PlanRemoteLegTwist(player, ped, pose.heading);
}

// ---- Claude's clothes on a remote player ----------------------------------
//
// look.h has the why. This is the engine half: read the four special slots,
// load a look into one, and get out of the way when a script wants one - or
// wants model 0 renamed under a remote ped built from it.

using RequestSpecialModelFn = void(__cdecl *)(int32_t, const char *, int32_t);

static_assert(LOOK_SLOTS == NUM_SPECIAL_CHARS, "one LookSlot per special char");
static_assert(LOOK_MI_PLAYER == MI_PLAYER && LOOK_MI_SPECIAL01 == MI_SPECIAL01,
              "look.h's rename test names the same models");
static_assert(LOOK_HELD_FLAGS == (STREAMFLAGS_DONT_REMOVE | STREAMFLAGS_SCRIPTOWNED),
              "look.h's idea of a held model is the streamer's");

Detour g_specialModelHook;
bool   g_lookHooked               = false;
bool   g_lookSlotOurs[LOOK_SLOTS] = {};
// Our own RequestSpecialModel calls go through the detour too.
bool   g_requestingLook  = false;
bool   g_saidLookRefused = false;
bool   g_saidNoLookSlot  = false;
bool   g_saidLookMissing = false;

uint8_t StreamingByte(uint32_t modelId, size_t field) {
	return *reinterpret_cast<const uint8_t *>(CStreaming__ms_aInfoForModel +
	                                          modelId * STREAMING_INFO_STRIDE + field);
}

LookSlot ReadLookSlot(int i) {
	const uint16_t id = static_cast<uint16_t>(MI_SPECIAL01 + i);
	LookSlot       s;
	if (const char *const name = ModelName(id)) {
		std::memcpy(s.name, name, sizeof s.name);
		s.name[sizeof s.name - 1] = '\0';
		s.refs = *reinterpret_cast<const uint16_t *>(name - offs::MODELINFO_NAME +
		                                             offs::MODELINFO_REFCOUNT);
	}
	s.loadState = StreamingByte(id, STREAMING_LOADSTATE_OFFS);
	s.flags     = StreamingByte(id, STREAMING_FLAGS_OFFS);
	s.ours      = g_lookSlotOurs[i];
	return s;
}

bool LookModelWanted(uint16_t model) {
	for (uint16_t m : g_lookModelOf)
		if (m == model)
			return true;
	return false;
}

// Hands back every slot we hold that nothing is built from and nobody is
// waiting on, the way UNLOAD_SPECIAL_CHARACTER does it.
void ReleaseLookSlots() {
	for (int i = 0; i < LOOK_SLOTS; ++i) {
		const uint16_t model = static_cast<uint16_t>(MI_SPECIAL01 + i);
		const LookSlot s     = ReadLookSlot(i);
		if (!LookSlotReleasable(s, LookModelWanted(model)))
			continue;
		Func<void(__cdecl *)(int32_t)>(CStreaming__SetMissionDoesntRequireModel)(model);
		g_lookSlotOurs[i] = false;
		Log("look: special slot %d ('%s') is free again", i + 1, s.name);
	}
}

// RequestSpecialModel never checks FindItem's answer (addresses.h), so a
// name that isn't in gta3.img is checked here instead.
bool LookInImage(const char *look) {
	void *const dir = Global<void *>(CStreaming__ms_pExtraObjectsDir);
	if (!dir)
		return false;
	using FindFn    = bool(__thiscall *)(void *, const char *, uint32_t *, uint32_t *);
	uint32_t offset = 0, size = 0;
	return Func<FindFn>(CDirectory__FindItem)(dir, look, &offset, &size);
}

bool PrepareRemoteLook(RemotePlayer &player) {
	if (!g_lookHooked || player.playerId >= MAX_PLAYERS)
		return IsModelReady(player.modelId);

	uint16_t &chosen = g_lookModelOf[player.playerId];
	if (player.modelId != MI_PLAYER) {
		chosen = MI_PLAYER;
		return IsModelReady(player.modelId);
	}
	// Our model 0 is what their look is measured against, and it's the old
	// gate as well: no local player yet, no remote ones. It's also unloaded
	// for the few frames UNDRESS_CHAR takes to swap it.
	if (!HasModelLoaded(MI_PLAYER))
		return false;

	const char *const ours = ModelName(MI_PLAYER);
	LookSlot          slots[LOOK_SLOTS];
	for (int i = 0; i < LOOK_SLOTS; ++i)
		slots[i] = ReadLookSlot(i);

	chosen                = MI_PLAYER;
	const LookChoice pick = PickLookSlot(player.look, ours, slots);
	switch (pick.pick) {
	case LookPick::Model0:
		break;
	case LookPick::Refused:
		if (!g_saidLookRefused) {
			g_saidLookRefused = true;
			Log("look: %s is wearing '%s', which isn't one of Claude's; building "
			    "them from our model 0 ('%s')",
			    player.nick.c_str(), player.look, ours);
		}
		break;
	case LookPick::NoSlot:
		if (!g_saidNoLookSlot) {
			g_saidNoLookSlot = true;
			Log("look: all four special slots are in use by the script; %s gets our "
			    "model 0 ('%s') instead of '%s' until one is free",
			    player.nick.c_str(), ours, player.look);
		}
		break;
	case LookPick::Slot: {
		const int      i     = pick.slot;
		const uint16_t model = static_cast<uint16_t>(MI_SPECIAL01 + i);
		if (!slots[i].ours || !SameLook(slots[i].name, player.look) ||
		    slots[i].loadState == STREAMING_NOTLOADED) {
			if (!LookInImage(player.look)) {
				if (!g_saidLookMissing) {
					g_saidLookMissing = true;
					Log("look: '%s' (%s) isn't in gta3.img here; building them from "
					    "our model 0 ('%s')",
					    player.look, player.nick.c_str(), ours);
				}
				break;
			}
			if (!slots[i].ours)
				Log("look: loading '%s' into special slot %d for %s", player.look, i + 1,
				    player.nick.c_str());
			g_requestingLook = true;
			Func<RequestSpecialModelFn>(CStreaming__RequestSpecialModel)(
			    model, player.look, STREAMFLAGS_SCRIPTOWNED | STREAMFLAGS_PRIORITY);
			g_requestingLook  = false;
			g_lookSlotOurs[i] = true;
		}
		chosen = model;
		break;
	}
	}

	ReleaseLookSlots();
	return chosen == MI_PLAYER || HasModelLoaded(chosen);
}

// Before a script renames model 0 or a special slot, any remote ped built
// from it comes down - the same thing UNDRESS_CHAR does to the one ped it
// knows about (addresses.h, CStreaming__RequestSpecialModel). ResolveRemote
// notices the handle has gone and the two-phase spawn builds the ped again
// once the model has streamed back in, in whichever model its look now
// needs.
//
// A special slot a script asks for stops being ours even when the name
// doesn't change: from here on it is the mission's to unload.
void MakeRoomForSpecialModel(int32_t modelId, const char *name) {
	const bool slot = modelId >= MI_SPECIAL01 && modelId < MI_SPECIAL01 + LOOK_SLOTS;
	if (modelId != MI_PLAYER && !slot)
		return;

	if (slot && g_lookSlotOurs[modelId - MI_SPECIAL01]) {
		g_lookSlotOurs[modelId - MI_SPECIAL01] = false;
		Log("look: the script wants special slot %d for '%s'; giving it back",
		    modelId - MI_SPECIAL01 + 1, name);
	}

	// The engine's own test: the same name is only a RequestModel and
	// touches nothing built from the model.
	const char *const current = ModelName(static_cast<uint32_t>(modelId));
	if (!RenameTakesModelAway(modelId, current, name))
		return;

	using GetPedFn = void *(__cdecl *)(int32_t);
	for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
		if (g_remotePeds[i].poolHandle < 0)
			continue;
		void *const ped = Func<GetPedFn>(CPools__GetPed)(g_remotePeds[i].poolHandle);
		if (!ped || Field<uintptr_t>(ped, offs::VTABLE) != CCivilianPed__vtable ||
		    Field<uint16_t>(ped, offs::MODEL_INDEX) != modelId)
			continue;

		Log("look: '%s' is replacing '%s' in model %d; taking player %u's ped down "
		    "first",
		    name, current, modelId, i);
		EndRemoteProjectilesOf(i);
		// Out of any car first: WarpPedIntoCar registered a pointer into this
		// ped on the car, and nothing else would take it back.
		UnseatPedFromCar(ped);
		DestroyRemotePed(ped);
		g_remotePeds[i]    = RemotePedIdentity{};
		g_lookTakenDown[i] = true;
	}
	// And any replica of another machine's pedestrian built from it: a
	// mission's special character, hosted by the mission's owner, whose
	// despawn can still be on its way when the next scene's
	// LOAD_SPECIAL_CHARACTER reaches the same slot here.
	TakeDownReplicasOfModel(static_cast<uint16_t>(modelId));
}

// A remote Claude who got our model 0 because every slot was a mission's,
// checked every 64 frames: once one is free again, the ped is taken back and
// the two-phase spawn builds it in the right clothes. Left alone while
// seated or dying, where a rebuild costs more than the wrong shirt does.
bool RebuildForFreedLookSlot(RemotePlayer &player, void *ped) {
	if (!g_lookHooked || (FrameNow() & 63) != 0 || player.modelId != MI_PLAYER ||
	    player.playerId >= MAX_PLAYERS ||
	    Field<uint16_t>(ped, offs::MODEL_INDEX) != MI_PLAYER || player.Seated() ||
	    Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return false;

	LookSlot slots[LOOK_SLOTS];
	for (int i = 0; i < LOOK_SLOTS; ++i)
		slots[i] = ReadLookSlot(i);
	if (PickLookSlot(player.look, ModelName(MI_PLAYER), slots).pick != LookPick::Slot ||
	    !LookInImage(player.look))
		return false;

	Log("look: a special slot is free now; rebuilding %s in '%s'", player.nick.c_str(),
	    player.look);
	EndRemoteProjectilesOf(player.playerId);
	DestroyRemotePed(ped);
	player.poolHandle   = -1;
	player.spawnPending = true;
	ForgetRemotePed(player);
	return true;
}

void __cdecl HookedRequestSpecialModel(int32_t modelId, const char *name, int32_t flags) {
	if (!g_requestingLook && name)
		MakeRoomForSpecialModel(modelId, name);
	g_specialModelHook.Original<RequestSpecialModelFn>()(modelId, name, flags);
}

// Model 0's name, which is which clothes the local player is in.
bool SampleLocalPlayerLook(char (&look)[PLAYER_LOOK_LEN]) {
	if (!PlayerPed())
		return false;
	const char *const name = ModelName(MI_PLAYER);
	if (!name)
		return false;
	std::memcpy(look, name, PLAYER_LOOK_LEN);
	return CleanPlayerLook(look);
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
	    state == PEDSTATE_CARJACK || state == PEDSTATE_DRAG_FROM_CAR)
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
bool SampleLocalCarEntry(LocalCarEntry &out) { return SamplePedCarEntry(PlayerPed(), out); }

bool SamplePedCarEntry(void *ped, LocalCarEntry &out) {
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

	// A jack: SetCarJack's state, or the quick jack PedAnimAlignCB can turn an
	// ordinary SetEnterCar into when there is a driver behind that door
	// (addresses.h, ANIM_STD_CAR_QJACK). The second only shows once the align
	// animation is over, so it goes out a moment after the plain intent.
	void *const anim = Field<void *>(ped, offs::PED_VEHICLE_ANIM);
	out.jack = state == PEDSTATE_CARJACK ||
	           (anim && Field<int32_t>(anim, ANIM_ID) == ANIM_STD_CAR_QJACK);
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

bool ReleaseExitDoor(void *ped, void *car) { return GiveBackExitDoor(ped, car); }

uint32_t CarEntryMark(void *ped) { return ped ? EntryProgressMark(ped) : 0; }

// ---- back on his feet -------------------------------------------------------
//
// m_pVehicleAnim is the one thing about a car the ped carries after leaving
// it that the engine never cleans up for anybody else: every exit of its own
// nils it (PedSetOutCarCB, QuitEnteringCar, the warp-out handler), and
// SetEnterCar refuses a ped who still has one (addresses.h,
// PEDSTATE_SEEK_CAR). A seat left by CoopIII's own hand without it is a
// player who walks up to every car afterwards and never gets in.

bool ClumpHoldsAnim(void *clump, void *assoc) {
	bool found = false;
	if (clump && assoc)
		ForEachAnim(clump, [&](void *a) {
			if (a == assoc)
				found = true;
		});
	return found;
}

int ForgetVehicleAnim(void *ped) {
	if (!ped)
		return VEHICLE_ANIM_NONE;
	void *const anim = Field<void *>(ped, offs::PED_VEHICLE_ANIM);
	if (!anim)
		return VEHICLE_ANIM_NONE;
	Field<void *>(ped, offs::PED_VEHICLE_ANIM) = nullptr;
	// Only written to while the clump still holds it. Its car sitting
	// animation is ASSOC_DELETEFADEDOUT (AnimManager.cpp:139), so a pointer
	// left behind by a ped who has walked off is freed memory by now, and a
	// blend delta written there lands on the heap.
	if (!ClumpHoldsAnim(ClumpOf(ped), anim))
		return VEHICLE_ANIM_DANGLING;
	Field<float>(anim, ANIM_BLEND_DELTA) = -1000.0f;
	return VEHICLE_ANIM_FADED;
}

namespace {

bool g_saidDroppedCarCallbacks = false;
bool g_saidFinishedRollingDoor = false;

// Every live ped in the pool: size at +8, flags at +4 with 0x80 free, entries
// at +0 strided by sizeof(CPlayerPed), the engine's own walk.
template <class Fn>
void ForEachPoolPed(Fn fn) {
	auto *const pool = Global<uint8_t *>(CPools__ms_pPedPool);
	if (!pool)
		return;
	uint8_t *const entries = Field<uint8_t *>(pool, object::POOL_ENTRIES);
	uint8_t *const flags   = Field<uint8_t *>(pool, object::POOL_FLAGS);
	const int32_t  size    = Field<int32_t>(pool, object::POOL_SIZE);
	if (!entries || !flags || size <= 0 || size > 1024)
		return;
	for (int32_t i = 0; i < size; ++i)
		if (!(flags[i] & object::POOLFLAG_ISFREE))
			fn(entries + static_cast<size_t>(i) * offs::SIZEOF_PLAYER_PED);
}

} // namespace

int DropCarChainCallbacks(void *ped) {
	void *const clump = ped ? ClumpOf(ped) : nullptr;
	if (!clump)
		return 0;
	void *const car        = Field<void *>(ped, offs::PED_MY_VEHICLE);
	const bool  automobile = car && Field<uintptr_t>(car, offs::VTABLE) == CAutomobile__vtable;
	void *const vehicleAnim = Field<void *>(ped, offs::PED_VEHICLE_ANIM);
	const uintptr_t rollingGuard = RollingDoorGuardAddress();
	const uintptr_t trainGuard   = OutTrainGuardAddress();

	// Collected, and the ones to finish run after the walk: a callback run
	// from inside it would be free to change the list being walked.
	constexpr int MAX_FINISH = 4;
	void *finish[MAX_FINISH];
	int   finishing = 0;
	int   dropped   = 0;
	bool  hadVehicleAnim = false;
	ForEachAnim(clump, [&](void *assoc) {
		const int32_t   type = Field<int32_t>(assoc, ANIM_CALLBACK_TYPE);
		const uintptr_t fn   = EngineCarCallback(Field<uintptr_t>(assoc, ANIM_CALLBACK),
		                                         rollingGuard, trainGuard);
		if (!PendingCarCallback(type, fn, Field<void *>(assoc, ANIM_CALLBACK_ARG) == ped))
			return;
		const bool now = EndPendingCarCallback(fn, type, automobile) == CarCallbackEnd::FinishNow;
		Field<int32_t>(assoc, ANIM_CALLBACK_TYPE) = ANIM_CB_NONE;
		Field<float>(assoc, ANIM_BLEND_DELTA)     = -1000.0f;
		Field<int32_t>(assoc, ANIM_FLAGS) |= ASSOC_DELETEFADEDOUT;
		if (now && finishing < MAX_FINISH)
			finish[finishing++] = assoc;
		if (assoc == vehicleAnim)
			hadVehicleAnim = true;
		++dropped;
	});
	// It is deleted on the engine's next pass over the clump, and the ped must
	// not be left pointing at it - the warp-out handler's own -1000 and nil.
	if (hadVehicleAnim)
		Field<void *>(ped, offs::PED_VEHICLE_ANIM) = nullptr;

	// The engine's own end of the rolling close, on the car it was for: the
	// door shut, its bit in m_nGettingOutFlags cleared, a swinging door OK.
	for (int i = 0; i < finishing; ++i)
		Func<void(__cdecl *)(void *, void *)>(CPed__PedAnimDoorCloseRollingCB)(finish[i], ped);

	if (dropped != 0 && !g_saidDroppedCarCallbacks) {
		g_saidDroppedCarCallbacks = true;
		Log("animcb: a ped had %d car animation(s) with a callback still to come when "
		    "CoopIII changed his seat (state %u); taken off and faded out, so none "
		    "fires on a car he has left (said once)",
		    dropped, Field<uint32_t>(ped, offs::PED_STATE));
	}
	if (finishing != 0 && !g_saidFinishedRollingDoor) {
		g_saidFinishedRollingDoor = true;
		Log("animcb: finished a rolling door close now instead, which gives the car its "
		    "front left door back (said once)");
	}
	return dropped;
}

int LetGoOfCarChain(void *ped) {
	if (!ped)
		return 0;
	// Halfway through a door. Its callbacks used to end the entry for it, a
	// frame late - every one of them answers a ped who is no longer entering
	// with QuitEnteringCar - and by then he could be in a seat this caller
	// gave him. So the engine's own way out of an entry runs now instead:
	// the car's count, its door's bit, the jack flag and the door itself.
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_ENTER_CAR || state == PEDSTATE_CARJACK)
		return AbandonPedEnterCar(ped);
	return DropCarChainCallbacks(ped);
}

int DropCarChainCallbacksOnCar(void *vehicle) {
	if (!vehicle)
		return 0;
	int n = 0;
	ForEachPoolPed([&](void *ped) {
		if (Field<void *>(ped, offs::PED_MY_VEHICLE) == vehicle)
			n += DropCarChainCallbacks(ped);
	});
	return n;
}

int RepointAnimCallbacks(uintptr_t from, uintptr_t to) {
	if (!from)
		return 0;
	int n = 0;
	ForEachPoolPed([&](void *ped) {
		if (void *const clump = ClumpOf(ped))
			ForEachAnim(clump, [&](void *assoc) {
				if (Field<uintptr_t>(assoc, ANIM_CALLBACK) == from) {
					Field<uintptr_t>(assoc, ANIM_CALLBACK) = to;
					++n;
				}
			});
	});
	return n;
}

void PutPedOnFoot(void *ped, bool leftASeat) {
	if (!ped)
		return;

	// While m_pMyVehicle still says which car: a rolling door close still
	// playing would otherwise finish on nothing (addresses.h, 0x004E4BAB).
	LetGoOfCarChain(ped);

	Field<bool>(ped, offs::PED_IN_VEHICLE)     = false;
	Field<void *>(ped, offs::PED_MY_VEHICLE)   = nullptr;
	Field<uint32_t>(ped, offs::PED_STATE)      = PEDSTATE_IDLE;
	Field<uint32_t>(ped, offs::PED_LAST_STATE) = PEDSTATE_NONE;
	Field<uint8_t>(ped, offs::ENTITY_FLAGS_A) |= offs::ENTITY_USES_COLLISION;
	float *const vel = &Field<float>(ped, offs::MOVE_SPEED);
	vel[0] = vel[1] = vel[2] = 0.0f;

	// A player left holding ENTER_CAR and a car pointer walks back to it.
	ClearPedObjective(ped);

	if (leftASeat) {
		// The seat took the weapon out of his hand (RemoveWeaponWhenEntering
		// Vehicle), and the warp-out handler puts it back the same way.
		// Guarded on m_wepModelID: AddWeaponModel adds a second atomic to a
		// hand that still has one.
		const uint8_t slot = Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON);
		if (slot < offs::NUM_WEAPON_SLOTS && Field<int32_t>(ped, offs::PED_WEP_MODEL_ID) == -1) {
			const uint32_t type = Field<uint32_t>(
			    ped, offs::PED_WEAPONS + slot * offs::SIZEOF_WEAPON + offs::WEAPON_TYPE);
			using InfoFn = void *(__cdecl *)(int);
			if (void *const info = Func<InfoFn>(CWeaponInfo__GetWeaponInfo)(static_cast<int>(type)))
				Func<void(__thiscall *)(void *, int32_t)>(CPed__AddWeaponModel)(
				    ped, Field<int32_t>(info, WEAPONINFO_MODEL_ID));
		}
		Func<void(__thiscall *)(void *)>(CPed__RemoveInCarAnims)(ped);
	}

	ForgetVehicleAnim(ped);
	Func<void(__thiscall *)(void *)>(CPed__RestartNonPartialAnims)(ped);
	Func<void(__thiscall *)(void *, int32_t)>(CPed__SetMoveState)(ped, PEDMOVE_NONE);
	if (void *const clump = ClumpOf(ped)) {
		using BlendFn = void *(__cdecl *)(void *, int, int, float);
		Func<BlendFn>(CAnimManager__BlendAnimation)(
		    clump, Field<int32_t>(ped, offs::PED_ANIM_GROUP), ANIM_STD_IDLE, PED_IDLE_BLEND_DELTA);
	}
}

void ClearEnterObjective(void *ped) {
	if (!ped)
		return;
	ClearPedObjective(ped);
	if (Field<uint32_t>(ped, offs::PED_STATE) == PEDSTATE_SEEK_CAR) {
		Field<uint32_t>(ped, offs::PED_STATE) = PEDSTATE_IDLE;
		Func<void(__thiscall *)(void *, int32_t)>(CPed__SetMoveState)(ped, PEDMOVE_STILL);
	}
}

uint8_t PullOutOf(void *ped) { return PedPullOut(ped); }

bool StartReplicaCarEntry(void *ped, void *car, uint8_t seat, uint8_t doorSeat) {
	return BeginPedEnterCar(ped, car, seat, doorSeat, SeatComer::Replica);
}

bool StartCarExit(void *ped) { return BeginPedExitCar(ped, /*player=*/false); }

int WireSeatOf(void *car, void *ped) {
	if (!car || !ped)
		return -1;
	if (Field<void *>(car, offs::VEH_DRIVER) == ped)
		return 0;
	const int slot = PassengerSlotOf(car, ped);
	return slot >= 0 ? slot + 1 : -1;
}

uint16_t FreeSeatsForWarp(void *car) { return car ? FreeSeatsOf(car) : uint16_t{0}; }

int MovePassengerToSeat(void *car, void *ped, uint8_t seat) {
	if (!car || !ped || seat == 0 || seat > SEAT_PLAN_PASSENGER_SLOTS)
		return WireSeatOf(car, ped);
	const int have = WireSeatOf(car, ped);
	if (have <= 0 || have == seat || !(FreeSeatsOf(car) & (1u << seat)))
		return have;
	PutPassengerInSlot(car, ped, static_cast<uint8_t>(seat - 1));
	return WireSeatOf(car, ped);
}

// A player's copy sitting in another passenger seat than the session gives
// him, sent there because the seat was somebody's at the time (seatplan.h,
// PlanSettle).
bool SettleRemoteSeat(RemotePlayer &player, RemoteVehicle &vehicle, uint8_t seat) {
	void *const ped = ResolveRemote(player);
	void *const car = ped ? ResolveRemoteVehicle(vehicle) : nullptr;
	if (!car || !Field<bool>(ped, offs::PED_IN_VEHICLE) ||
	    Field<void *>(ped, offs::PED_MY_VEHICLE) != car)
		return false;
	const int have = WireSeatOf(car, ped);
	if (have <= 0 || have == seat || seat == 0 || seat > SEAT_PLAN_PASSENGER_SLOTS)
		return false;

	const SeatHolder holder   = HolderOf(car, seat);
	void *const      occupant = SeatOccupant(car, seat);
	const bool climbing =
	    (Field<uint8_t>(car, offs::VEH_GETTING_IN_FLAGS) & DoorFlag(DoorForSeat(seat))) != 0;
	const bool settled  = !climbing && Field<uint32_t>(ped, offs::PED_STATE) == PEDSTATE_DRIVING &&
	                     (!occupant || Field<uint32_t>(occupant, offs::PED_STATE) == PEDSTATE_DRIVING);
	const SettleMove move = PlanSettle(static_cast<uint8_t>(have), seat, holder, settled);
	bool moved = false;
	if (move == SettleMove::Move)
		moved = MovePassengerToSeat(car, ped, seat) == seat;
	else if (move == SettleMove::Swap)
		moved = SwapRiders(car, static_cast<uint8_t>(have), seat);
	if (moved) {
		static int said = 0;
		if (said < 3) {
			++said;
			Log("bridge: %s sits in seat %u of vehicle %u here now, where his own screen "
			    "has him, rather than seat %d%s",
			    player.nick.c_str(), seat, vehicle.netId, have,
			    move == SettleMove::Swap ? "; whoever had it took that one" : "");
		}
	}
	return moved;
}

// The two things game/combat.cpp needs from in here, nothing else.
//
// Both thin wrappers on purpose, not copies. ResolveRemotePed is the only
// place the pool handle and the vtable are checked together, and a second
// copy of that check is just a second place to forget to update. GiveWeaponTo
// owns `RemotePlayer::appliedWeapon`, and two writers of that field would
// fight over when a weapon model gets rebuilt.
void *ResolveRemotePed(RemotePlayer &player) { return ResolveRemote(player); }

void SetRemotePlayersHidden(bool hidden) { g_remoteHiddenForScene = hidden; }

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

bool InstallLegTwist() {
	g_legTwist.Clear();
	if (!g_legTwistRedirected)
		g_legTwistRedirected =
		    RedirectCallSite(CPed__ProcessControl_NewVelocity, CPed__CalculateNewVelocity,
		                     reinterpret_cast<uintptr_t>(&NewVelocityThenLegs));
	if (!g_legTwistRedirected) {
		Log("ped: FAILED to redirect the call at 0x%08X; it no longer calls "
		    "CalculateNewVelocity (0x%08X), so remote players walk diagonally on "
		    "straight legs",
		    static_cast<unsigned>(CPed__ProcessControl_NewVelocity),
		    static_cast<unsigned>(CPed__CalculateNewVelocity));
		return false;
	}
	Log("ped: CPed::ProcessControl's CalculateNewVelocity call comes to us first, for "
	    "a remote player's diagonal walk");
	return true;
}

void RemoveLegTwist() {
	if (g_legTwistRedirected &&
	    RedirectCallSite(CPed__ProcessControl_NewVelocity,
	                     reinterpret_cast<uintptr_t>(&NewVelocityThenLegs),
	                     CPed__CalculateNewVelocity))
		g_legTwistRedirected = false;
	g_legTwist.Clear();
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

const char *LocalPlayerModelName() { return ModelName(MI_PLAYER); }

bool LookInGameImage(const char *look) { return look && LookInImage(look); }

uint16_t RemotePlayerPedsBuiltFrom(int32_t modelId) {
	using GetPedFn = void *(__cdecl *)(int32_t);
	uint16_t n = 0;
	for (const RemotePedIdentity &r : g_remotePeds) {
		if (r.poolHandle < 0)
			continue;
		void *const ped = Func<GetPedFn>(CPools__GetPed)(r.poolHandle);
		if (ped && Field<uintptr_t>(ped, offs::VTABLE) == CCivilianPed__vtable &&
		    Field<uint16_t>(ped, offs::MODEL_INDEX) == modelId)
			++n;
	}
	return n;
}

bool InstallLookHook() {
	if (!g_specialModelHook.Install("CStreaming::RequestSpecialModel",
	                                reinterpret_cast<void *>(CStreaming__RequestSpecialModel),
	                                reinterpret_cast<void *>(&HookedRequestSpecialModel))) {
		Log("look: FAILED to hook CStreaming::RequestSpecialModel at 0x%08X; every "
		    "remote Claude wears whatever our model 0 is",
		    CStreaming__RequestSpecialModel);
		for (const auto &f : HookFailures())
			Log("look:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}
	g_lookHooked = true;
	Log("look: hooked CStreaming::RequestSpecialModel at 0x%08X",
	    CStreaming__RequestSpecialModel);
	return true;
}

// After Client::Stop, so no remote ped is built from a slot any more and
// every one we hold can go back.
void RemoveLookHook() {
	g_specialModelHook.Remove();
	g_lookHooked = false;
	for (uint16_t &m : g_lookModelOf)
		m = MI_PLAYER;
	ReleaseLookSlots();
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
			    PruneAnims(clump, animId, ANIM_NONE, VehicleAnimOf(ped),
			               AnimClumpSurplus(count));
			if (!AnimClumpHasRoom(count - dropped))
				return false;
		}
	}

	using BlendFn = void *(__cdecl *)(void *, int, int, float);
	return Func<BlendFn>(CAnimManager__BlendAnimation)(
	           clump, plan.group, static_cast<int>(animId), plan.blendDelta) != nullptr;
}

bool g_saidSeatKept = false;

bool KeepSeatedPose(void *ped) {
	if (!ped || !Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	void *const    car   = Field<void *>(ped, offs::PED_MY_VEHICLE);
	void *const    clump = ClumpOf(ped);
	if (!car || !clump || !SeatedPoseStateToKeep(state))
		return true;

	// Off the clump: everything a seat has no business playing. Never the
	// association m_pVehicleAnim holds, which the engine keeps a raw pointer to.
	void *const vehicleAnim = VehicleAnimOf(ped);
	int         dropped     = 0;
	uint16_t    firstId     = ANIM_NONE;
	ForEachAnim(clump, [&](void *assoc) {
		if (assoc == vehicleAnim)
			return;
		const int32_t id = Field<int32_t>(assoc, ANIM_ID);
		if (id < 0 || !SeatedPoseForbids(static_cast<uint16_t>(id)))
			return;
		if (Field<float>(assoc, ANIM_BLEND_AMOUNT) <= 0.0f && LifeOf(assoc) != AnimLife::LIVE)
			return;
		Field<float>(assoc, ANIM_BLEND_AMOUNT) = 0.0f;
		Field<float>(assoc, ANIM_BLEND_DELTA)  = -1000.0f;
		Field<int32_t>(assoc, ANIM_FLAGS) |= ASSOC_DELETEFADEDOUT;
		if (firstId == ANIM_NONE)
			firstId = static_cast<uint16_t>(id);
		++dropped;
	});

	// A fall or get-up our engine started on a ped that never left the seat.
	const bool wasDown = state != PEDSTATE_DRIVING;
	if (wasDown) {
		Field<uint32_t>(ped, offs::PED_STATE) = PEDSTATE_DRIVING;
		Field<uint8_t>(ped, offs::PED_FLAGS_C) &= static_cast<uint8_t>(~offs::PED_UPDATE_ANIM_HEADING);
		Field<uint8_t>(ped, offs::PED_FLAGS_E) &= static_cast<uint8_t>(~offs::PED_GETUP_ANIM_STARTED);
		Field<uint8_t>(ped, offs::PED_FLAGS_I) &= static_cast<uint8_t>(~offs::PED_FALLEN_DOWN);
	}

	// And the seat back on, the way PedSetInCarCB sits a ped (addresses.h, "a
	// seated ped built again"), when it is gone or was never there. Only when
	// m_pVehicleAnim holds nothing or holds a sit already: anything else in it
	// is a door or a shuffle whose callback the engine is waiting on. Not in a
	// boat, where PedSetInCarCB sits nobody in any animation (0x004CF31E).
	if (Field<int32_t>(car, offs::VEH_TYPE) == VEHICLE_TYPE_BOAT) {
		if ((dropped > 0 || wasDown) && !g_saidSeatKept) {
			g_saidSeatKept = true;
			Log("bridge: a copy at a boat's wheel was playing what a seat does not (%d "
			    "animation(s), first %02Xh, state %u); taken off (said once)",
			    dropped, firstId, state);
		}
		return true;
	}
	const bool     driver = Field<void *>(car, offs::VEH_DRIVER) == ped;
	const bool     low    = (Field<uint8_t>(car, offs::VEH_FLAGS_B_BUS) & offs::VEH_IS_LOW) != 0;
	const uint16_t sit    = driver ? (low ? ANIM_STD_CAR_SIT_LO : ANIM_STD_CAR_SIT)
	                               : (low ? ANIM_STD_CAR_SIT_P_LO : ANIM_STD_CAR_SIT_P);
	// A pointer to something no longer on the clump counts as nothing.
	const bool    held   = vehicleAnim && ClumpHoldsAnim(clump, vehicleAnim);
	const int32_t heldId = held ? Field<int32_t>(vehicleAnim, ANIM_ID) : -1;
	const bool    heldIsSit =
	    heldId >= ANIM_STD_CAR_SIT && heldId <= ANIM_STD_CAR_SIT_P_LO;
	bool resat = false;
	if (!held || heldIsSit) {
		const bool live = held && LifeOf(vehicleAnim) == AnimLife::LIVE &&
		                  Field<float>(vehicleAnim, ANIM_BLEND_AMOUNT) > 0.0f;
		if (!live || dropped > 0) {
			using BlendFn = void *(__cdecl *)(void *, int, int, float);
			if (void *const assoc = Func<BlendFn>(CAnimManager__BlendAnimation)(
			        clump, ASSOCGRP_STD, sit, CAR_SIT_BLEND_DELTA)) {
				Field<void *>(ped, offs::PED_VEHICLE_ANIM) = assoc;
				Func<void(__thiscall *)(void *)>(CPed__StopNonPartialAnims)(ped);
				resat = true;
			}
		}
	}

	if ((dropped > 0 || wasDown || resat) && !g_saidSeatKept) {
		g_saidSeatKept = true;
		Log("bridge: a copy sitting in a car was playing what a seat does not (%d "
		    "animation(s), first %02Xh, state %u); taken off and sat back down with "
		    "%02Xh (said once)",
		    dropped, firstId, state, sit);
	}
	return true;
}

bool g_saidReplicaAnimRevived = false;

bool ReplicaAnimNeedsBlend(void *ped, uint16_t want, uint16_t applied) {
	if (want == ANIM_NONE)
		return false;
	if (want != applied)
		return true;
	// Only the loops. A base that plays once (a stop, a start-walk, a dodge)
	// ends on its own, and with no phase on the wire a revival would start it
	// again from the top.
	if (want > ANIM_STD_IDLE)
		return false;
	void *const clump = ped ? ClumpOf(ped) : nullptr;
	if (!clump)
		return false;
	if (LookForAnim(clump, want).life == AnimLife::LIVE)
		return false;
	if (!g_saidReplicaAnimRevived) {
		g_saidReplicaAnimRevived = true;
		Log("population: a pedestrian replica's %02Xh had been taken off by our own "
		    "engine and goes back on (said once)",
		    want);
	}
	return true;
}

int SeatReplicaPed(void *ped, void *car, uint8_t seat) {
	return SeatPedInCarAs(ped, car, seat, SeatComer::Replica);
}

void UnseatReplicaPed(void *ped) { UnseatPedFromCar(ped); }

WorldBridge MakeWorldBridge() {
	WorldBridge b;
	b.SampleLocalPlayer = &SampleLocalPlayer;
	b.RequestModel      = &RequestModel;
	b.IsModelReady      = &IsModelReady;
	b.SampleLocalPlayerModel = &SampleLocalPlayerModel;
	b.SampleLocalPlayerLook  = &SampleLocalPlayerLook;
	b.PrepareRemoteLook      = &PrepareRemoteLook;
	b.SampleLocalAmmo        = &SampleLocalAmmo;
	b.ApplyRemoteAmmo        = &ApplyRemoteAmmoSlot;
	b.ApplyRemotePose   = &ApplyRemotePose;
	b.SampleLocalRide   = &SampleLocalRide;
	b.VehicleRideFrame  = &VehicleRideFrame;
	b.TrainRideFrame    = &TrainRideFrame;
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
	b.VehicleOnItsRoof      = &VehicleOnItsRoof;
	b.ReadVehicleColours    = &ReadVehicleColours;
	b.WriteVehicleColours   = &WriteVehicleColours;
	b.TakeVehicleBack       = &TakeVehicleBack;
	// What shape a car is in (docs/cardamage.md). Beside the state pair
	// because they are the same seam, and separate from it because damage is
	// an event on the reliable channel and state is a 25 Hz sample.
	b.SampleLocalVehicleDamage    = &SampleLocalVehicleDamage;
	b.SampleObservedVehicleDamage = &SampleObservedVehicleDamage;
	b.SampleLocalVehicleBomb      = &SampleLocalVehicleBomb;
	b.SampleObservedVehicleBomb   = &SampleObservedVehicleBomb;
	b.ApplyRemoteVehicleBomb      = &ApplyRemoteVehicleBomb;
	b.DetonateRemoteVehicleBomb   = &DetonateRemoteVehicleBomb;
	b.ApplyRemoteVehicleDamage    = &ApplyRemoteVehicleDamage;
	b.SeatRemotePed        = &SeatRemotePed;
	b.UnseatRemotePed      = &UnseatRemotePed;
	b.BeginSeatRemotePed   = &BeginSeatRemotePed;
	b.PollSeatRemotePed    = &PollSeatRemotePed;
	b.AbandonSeatRemotePed = &AbandonSeatRemotePed;
	b.BeginUnseatRemotePed = &BeginUnseatRemotePed;
	b.SettleRemoteSeat     = &SettleRemoteSeat;
	b.SampleLocalCarEntry  = &SampleLocalCarEntry;
	b.BeginJackRemotePed   = &BeginJackRemotePed;
	b.PollJackRemotePed    = &PollJackRemotePed;
	b.RemoteBeingPulledOut = &RemoteBeingPulledOut;
	b.LocalBeingPulledOut  = &LocalBeingPulledOut;

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
