#include "ped.h"

#include "addresses.h"
#include "combat.h"
#include "log.h"
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

} // namespace

bool LocalPlayerExists() {
	return PlayerPed() != nullptr;
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

	// Weapon. m_currentWeapon doubles as an index into m_weapons and as the
	// eWeaponType itself, so the slot gets bounds-checked before use. This is
	// exactly why sampling was held off until the offsets were confirmed.
	out.weapon = WEAPONTYPE_UNARMED;
	const uint8_t slot = Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON);
	if (slot < offs::NUM_WEAPON_SLOTS) {
		const uint32_t type = Field<uint32_t>(
		    ped, offs::PED_WEAPONS + slot * offs::SIZEOF_WEAPON + offs::WEAPON_TYPE);
		if (IsInventoryWeapon(static_cast<uint8_t>(type)))
			out.weapon = static_cast<uint8_t>(type);
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
	if (modelId == MI_PLAYER)
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
	void *ped = ResolveRemote(player);
	player.poolHandle   = -1;
	player.spawnPending = false;
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
// draws. An m_nMoveState or bIsAimingGun written here, on the other hand, is
// exactly what the engine reads on its way past.

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
bool  MakeAnimRoom(RemotePlayer &player, void *clump);

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
	if (!FindAnimById(clump, animId) && !MakeAnimRoom(player, clump))
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
int PruneAnims(void *clump, uint16_t keepA, uint16_t keepB, int surplus) {
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
// the only thing adding: CPed::ProcessControl and CPed::SetMoveAnim blend
// animations onto a remote ped every frame without asking anybody. So this
// runs unconditionally, once per remote ped per frame, before CGame::Process
// gets the chance to walk the clump.
//
// It is the last line of defence for a crash that has no symptoms until it
// happens, so it is deliberately not conditional on anything CoopIII knows.
void EnforceAnimLimit(RemotePlayer &player, void *clump) {
	const int count = CountAnims(clump);
	const int surplus = AnimClumpSurplus(count + 1);   // +1: room to still add one
	if (surplus <= 0)
		return;

	const int dropped =
	    PruneAnims(clump, player.appliedAnimId, player.appliedAnimId2, surplus);
	if (dropped > 0)
		Log("bridge: %s's ped was carrying %d animations, past what the engine's "
		    "node array can index; dropped %d",
		    player.nick.c_str(), count, dropped);
}

bool MakeAnimRoom(RemotePlayer &player, void *clump) {
	const int count = CountAnims(clump);
	if (AnimClumpHasRoom(count))
		return true;

	const int dropped = PruneAnims(clump, player.appliedAnimId,
	                               player.appliedAnimId2, AnimClumpSurplus(count));
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

		if (!MakeAnimRoom(player, clump))
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
bool GiveWeaponTo(RemotePlayer &player, void *ped, uint8_t want) {
	if (!IsInventoryWeapon(want))
		return false;
	if (want == player.appliedWeapon)
		return true;

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

	// Ammo isn't on the wire yet (M3 owns that). A remote player's clip only
	// affects what CoopIII draws, and an empty one leaves the weapon stuck
	// in WEAPONSTATE_OUT_OF_AMMO with the wrong idle pose - so give enough
	// that the engine treats the gun as usable. GiveWeapon only adds on a
	// weapon change, and the engine caps the total at 99999 regardless.
	constexpr uint32_t REMOTE_AMMO = 1000;

	using GiveFn = uint32_t(__thiscall *)(void *, int, uint32_t);
	using SetFn  = void(__thiscall *)(void *, uint32_t);
	Func<GiveFn>(CPed__GiveWeapon)(ped, static_cast<int>(want), REMOTE_AMMO);
	Func<SetFn>(CPed__SetCurrentWeapon)(ped, want);
	player.appliedWeapon = want;
	return true;
}

void ApplyWeapon(RemotePlayer &player, void *ped) {
	GiveWeaponTo(player, ped, player.last.weapon);
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
// Only yaw gets applied. CPed::AimGun passes a hard zero as the pitch for
// anything that isn't the local CPlayerPed, and CPedIK::MoveLimb drags the
// torso back toward it at 7 degrees per timestep. Writing the real pitch at
// 25 Hz against that would look like a twitch, not an aim (§1.8.3).
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
	} else if (Field<uint8_t>(ped, offs::PED_FLAGS_A) & offs::PED_IS_AIMING_GUN) {
		// Only when it is actually set: ClearAimFlag is what starts the
		// gun-lowering animation, and re-running it every frame would keep
		// restarting it.
		using ClearFn = void(__thiscall *)(void *);
		Func<ClearFn>(CPed__ClearAimFlag)(ped);
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
// The animated entry (CPed::SetEnterCar) is skipped on purpose. It's a
// multi-second negotiation with door states, a walk to the handle, and an
// animation that can get interrupted or refused - none of which an observer
// can drive off a stream of events that just says "they're in the car now".
// The owner's machine plays that animation for its own player; this machine
// only hears the result.

// Which passenger slot holds this ped, or -1. Bounded by the car's own
// m_nNumMaxPassengers, capped at the array's actual length - that field is
// just a byte the handling data fills in, and the array is eight pointers
// no matter what it says. Declared early because the seating path uses this
// to undo a warp that half-applied.
void UnseatRemotePed(RemotePlayer &player);

int PassengerSlotOf(void *car, void *ped) {
	void *const   *seats = &Field<void *>(car, offs::VEH_PASSENGERS);
	const uint8_t  max   = Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS);
	const uint8_t  n = max < offs::VEH_MAX_PASSENGERS ? max : offs::VEH_MAX_PASSENGERS;
	for (uint8_t i = 0; i < n; ++i)
		if (seats[i] == ped)
			return i;
	return -1;
}

bool SeatRemotePed(RemotePlayer &player, RemoteVehicle &vehicle, uint8_t seat) {
	void *const ped = ResolveRemote(player);
	if (!ped)
		return false;
	void *const car = ResolveRemoteVehicle(vehicle);
	if (!car)
		return false;

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
	Func<WarpFn>(CPed__WarpPedIntoCar)(ped, car);

	// Did it actually take? The warp fails silently and half-applied, so
	// what gets checked here is the seat pointer - the thing it would have
	// skipped on failure.
	const bool seated = seat == 0 ? Field<void *>(car, offs::VEH_DRIVER) == ped
	                              : PassengerSlotOf(car, ped) >= 0;
	if (!seated) {
		Log("bridge: %s did not take seat %u of vehicle %u; putting them back "
		    "on foot",
		    player.nick.c_str(), seat, vehicle.netId);
		UnseatRemotePed(player);
		return false;
	}
	return true;
}

// The other direction. There's no WarpPedOutOfCar to call - the sequence
// lives open-coded inside COMMAND_WARP_CHAR_FROM_CAR_TO_COORD's handler.
// This is that sequence, minus the teleport (the pose stream decides where
// the ped goes) and minus CPed::RemoveInCarAnims (player-only, addresses.h
// explains why).
//
// Safe to call on a ped who isn't in a car at all, which is what lets it
// double as a plain "make sure they're on foot".
void UnseatRemotePed(RemotePlayer &player) {
	void *const ped = ResolveRemote(player);
	if (!ped)
		return;

	// "In a vehicle" and "has a vehicle" are two separate questions, and both
	// need asking. When a car gets destroyed under a seated ped, the
	// reference WarpPedIntoCar registered goes to null but nothing clears
	// bInVehicle - trust the flag alone and you dereference null, trust the
	// pointer alone and you act on a car this ped got out of long ago.
	void *const car = Field<bool>(ped, offs::PED_IN_VEHICLE)
	                      ? Field<void *>(ped, offs::PED_MY_VEHICLE)
	                      : nullptr;

	// Every vehicle CoopIII creates is a CAutomobile (vehicle.cpp), so this
	// vtable check is the same net ResolveRemote uses on peds - a slot that
	// still resolves while no longer holding what we think it does.
	if (car && Field<uintptr_t>(car, offs::VTABLE) == CAutomobile__vtable) {
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

		float *const move = &Field<float>(car, offs::MOVE_SPEED);
		move[0] = 0.0f;
		move[1] = 0.0f;
		move[2] = VEH_EXIT_SETTLE_SPEED_Z;
		float *const turn = &Field<float>(car, offs::TURN_SPEED);
		turn[0] = turn[1] = turn[2] = 0.0f;
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
	Field<uint32_t>(ped, offs::PED_OBJECTIVE)      = OBJECTIVE_NONE;
	Field<uint32_t>(ped, offs::PED_PREV_OBJECTIVE) = OBJECTIVE_NONE;
	Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE) = nullptr;

	// Nothing driven into this ped survived the trip - the seat changed its
	// state and its animations, and the weapon model may have gone with them.
	// Forgetting what was applied makes the next frame re-drive all of it.
	player.appliedWeapon  = 0xFFFF;
	player.appliedAnimId  = ANIM_NONE;
	player.appliedAnimId2 = ANIM_NONE;
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
	EnforceAnimLimit(player, ClumpOf(ped));

	// A seated ped belongs to the engine now. CPed::ProcessControl puts it
	// back in its seat from the car's own matrix every frame, so everything
	// below would get overwritten at best - and at worst the re-file and the
	// move state drag the ped half out of the car for the part of the frame
	// physics and collision actually look at.
	//
	// This matters more than any of the addresses do: the car has become the
	// authority on where this player is, so the pose stream has to stop
	// being one too. Health still applies, since that's about the player and
	// not about where they are.
	if (player.Seated()) {
		Field<float>(ped, offs::PED_HEALTH) = player.last.health;
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
	// has a death animation running.
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return;

	// The locomotion animation isn't chosen here. Writing m_nMoveState is
	// what makes CPed::SetMoveAnim - called later this same frame by
	// CPed::ProcessControl - blend the walk, run, sprint or idle for this
	// ped's own animation style. Do it the other way round, blending the
	// animation directly and leaving move state stale, and the engine just
	// changes its mind a few milliseconds later and fades ours back out.
	Field<uint32_t>(ped, offs::PED_MOVE_STATE) = ClampMoveState(player.last.moveState);

	ApplyWeapon(player, ped);
	ApplyFiring(player, ped);
	ApplyAim(player, ped);
	ApplyAnimation(player, ped);
}

} // namespace

// The two things game/combat.cpp needs from in here, nothing else.
//
// Both thin wrappers on purpose, not copies. ResolveRemotePed is the only
// place the pool handle and the vtable are checked together, and a second
// copy of that check is just a second place to forget to update. GiveWeaponTo
// owns `RemotePlayer::appliedWeapon`, and two writers of that field would
// fight over when a weapon model gets rebuilt.
void *ResolveRemotePed(RemotePlayer &player) { return ResolveRemote(player); }

bool GiveRemoteWeapon(RemotePlayer &player, void *ped, uint8_t weapon) {
	return GiveWeaponTo(player, ped, weapon);
}

bool RemotePlayerForPed(const void *ped, uint16_t &netId) {
	return LookupRemotePed(ped, netId);
}

int32_t StdAnimGroupCount() { return AnimGroupCount(ASSOCGRP_STD); }

WorldBridge MakeWorldBridge() {
	WorldBridge b;
	b.SampleLocalPlayer = &SampleLocalPlayer;
	b.RequestModel      = &RequestModel;
	b.IsModelReady      = &IsModelReady;
	b.SampleLocalPlayerModel = &SampleLocalPlayerModel;
	b.ApplyRemotePose   = &ApplyRemotePose;
	b.SpawnRemote       = &SpawnRemote;
	b.DespawnRemote     = &DespawnRemote;

	b.SampleLocalVehicle   = &SampleLocalVehicle;
	b.SpawnRemoteVehicle   = &SpawnRemoteVehicle;
	b.DespawnRemoteVehicle = &DespawnRemoteVehicle;
	b.SampleLocalVehicleIdentity = &SampleLocalVehicleIdentity;
	b.ApplyRemoteVehicle   = &ApplyRemoteVehicle;
	b.CorrectRemoteVehicle = &CorrectRemoteVehicle;
	b.SeatRemotePed        = &SeatRemotePed;
	b.UnseatRemotePed      = &UnseatRemotePed;

	// Combat. These three live in game/combat.cpp because they're driven by
	// detours rather than the frame pump, and because every address they use
	// came out of one pass over one script opcode - keeping them together
	// keeps that provenance together too.
	b.DrainLocalCombat    = &DrainLocalCombat;
	b.ReplayRemoteShot    = &ReplayRemoteShot;
	b.PlayRemoteExplosion = &PlayRemoteExplosion;
	b.ApplyRemoteDamage   = &ApplyRemoteDamage;
	b.KillRemotePed       = &KillRemotePed;
	b.SetFriendlyFire     = &SetFriendlyFire;

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
