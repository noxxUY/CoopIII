// Wire contract shared by client/ and server/. Layouts are pinned down by the
// static_asserts at the bottom. Update docs/protocol.md first, then this file.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coopiii {

// 2: PlayerStateBody grew the animation block (animId/animTime/animSpeed,
//    plus a partial-animation id and time) and a flags byte. See
//    docs/protocol.md §1.8.
// 3: EnterVehicleBody gained the identity fields a client sends when it's
//    first into a car (see that struct). Server flat-out rejects any other
//    version, so an old client fails to connect instead of misreading a
//    packet.
// 4: C_PlayerModel / S_PlayerModel added. A player's model can change after
//    joining, so the join packet isn't the last word on it anymore.
// 5: combat. ShotBody grew `speed` so a thrown projectile's initial velocity
//    makes it across the wire, plus C_Explosion / S_Explosion (only the
//    projectile's owner gets to say where it went off). docs/protocol.md
//    §1.9.
// 6: PlayerStateBody gained animGroup. GTA III has no sideways-walk
//    animation id; strafing is a different group entirely, with its own
//    walk and run, same for every armed stance. See that field below.
constexpr uint16_t PROTOCOL_VERSION = 6;
constexpr uint16_t DEFAULT_PORT     = 2001;
constexpr uint8_t  MAX_PLAYERS      = 8;
constexpr uint8_t  SNAPSHOT_HZ      = 25;   // docs/protocol.md §1.2
constexpr size_t   NICK_LEN         = 24;
constexpr size_t   CHAT_LEN         = 128;
constexpr uint8_t  INVALID_PLAYER   = 0xFF;
constexpr uint16_t INVALID_NETID    = 0;

// "no animation in this slot". AnimationId is a dense enum starting at 0, so
// it needs an out-of-band value rather than a zero, since ANIM_STD_WALK is 0.
constexpr uint16_t ANIM_NONE = 0xFFFF;

enum Channel : uint8_t {
	CH_SNAPSHOT = 0,   // unreliable, sequenced
	CH_EVENT    = 1,   // reliable, ordered
	CH_COUNT
};

enum Opcode : uint8_t {
	OP_C_HELLO           = 0x01,
	OP_S_WELCOME         = 0x02,
	OP_S_PLAYER_JOIN     = 0x03,
	OP_S_PLAYER_LEAVE    = 0x04,
	OP_C_PLAYER_MODEL    = 0x05,
	OP_S_PLAYER_MODEL    = 0x06,

	OP_C_PLAYER_STATE    = 0x10,
	OP_S_PLAYER_STATE    = 0x11,
	OP_C_VEHICLE_STATE   = 0x12,
	OP_S_VEHICLE_STATE   = 0x13,

	OP_C_SHOT            = 0x20,
	OP_S_SHOT            = 0x21,
	OP_C_DAMAGE          = 0x22,
	OP_S_DAMAGE          = 0x23,
	OP_S_DEATH           = 0x24,
	OP_C_RESPAWN         = 0x25,
	OP_S_RESPAWN         = 0x26,
	OP_C_EXPLOSION       = 0x27,
	OP_S_EXPLOSION       = 0x28,

	OP_C_ENTER_VEHICLE   = 0x30,
	OP_S_ENTER_VEHICLE   = 0x31,
	OP_C_EXIT_VEHICLE    = 0x32,
	OP_S_EXIT_VEHICLE    = 0x33,
	OP_S_VEHICLE_SPAWN   = 0x34,
	OP_S_VEHICLE_DESPAWN = 0x35,

	OP_S_WORLD_STATE     = 0x40,

	OP_C_CHAT            = 0x50,
	OP_S_CHAT            = 0x51,
};

enum LeaveReason : uint8_t {
	LEAVE_QUIT    = 0,
	LEAVE_TIMEOUT = 1,
	LEAVE_KICKED  = 2,
};

enum RejectReason : uint8_t {
	REJECT_NONE            = 0,
	REJECT_BAD_VERSION     = 1,
	REJECT_FULL            = 2,
};

#pragma pack(push, 1)

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

// sendTimeMs is the sender's own monotonic clock, ms since its CoopIII
// started. Clocks aren't shared between machines, so interpolation buffers
// are keyed per sender rather than compared across them.
//
// This is NOT CTimer::GetTimeInMilliseconds(). That one stops while the game
// is paused and gets multiplied by ms_fTimeScale, which both missions
// (SET_TIME_SCALE) and player death (1/3 slow motion) change. If we used it,
// a dying player's timestamps would drop to a third speed of everyone
// else's and their interpolation would desync. Need a clock nothing in the
// game can rescale.
struct PacketHeader {
	uint8_t  opcode;
	uint32_t sendTimeMs;
};

// ---- session -------------------------------------------------------------

struct C_Hello {
	static constexpr uint8_t OPCODE = OP_C_HELLO;
	PacketHeader hdr;
	uint16_t protocolVersion;
	char     nick[NICK_LEN];
	uint16_t modelId;
};

// On accept, followed by one S_PlayerJoin per player already in the session.
struct S_Welcome {
	static constexpr uint8_t OPCODE = OP_S_WELCOME;
	PacketHeader hdr;
	uint8_t  reject;        // RejectReason; if != REJECT_NONE the rest is unset
	uint8_t  playerId;
	uint16_t netId;
	uint8_t  maxPlayers;
	uint8_t  snapshotHz;
	uint8_t  hour, minute;
	uint8_t  weather;
};

struct S_PlayerJoin {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_JOIN;
	PacketHeader hdr;
	uint8_t  playerId;
	uint16_t netId;
	char     nick[NICK_LEN];
	uint16_t modelId;
	Vec3     pos;
	float    heading;
};

struct S_PlayerLeave {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_LEAVE;
	PacketHeader hdr;
	uint8_t playerId;
	uint8_t reason;         // LeaveReason
};

// Sent only when a player's model actually changes, not on every join.
// Reliable, change-only: a model swap happens maybe a handful of times per
// playthrough, so cramming it into the 25 Hz snapshot would burn two bytes
// forty times a second for nothing.
//
// Stock GTA III never triggers this at all: one protagonist model, no
// outfits (see MI_PLAYER in client/src/game/addresses.h for the evidence).
// But that's a fact about the stock script data, not the engine. Nothing
// stops a ped's model index from changing, some mod probably will change it,
// and a remote player wearing the wrong body is exactly the kind of bug
// that's obvious on screen and invisible in any log.
struct C_PlayerModel {
	static constexpr uint8_t OPCODE = OP_C_PLAYER_MODEL;
	PacketHeader hdr;
	uint16_t modelId;
};

struct S_PlayerModel {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_MODEL;
	PacketHeader hdr;
	uint8_t  playerId;
	uint16_t modelId;
};

// ---- snapshots (CH_SNAPSHOT) ---------------------------------------------

enum PlayerFlags : uint8_t {
	PF_AIMING = 1 << 0,   // CPed::bIsAimingGun, aimYaw is a real target
	// CPed::bIsShooting: the state of holding the trigger, not the act of
	// discharging. 25 Hz can't carry one event per bullet, so individual
	// shots go out on C_Shot (reliable channel) and this flag just keeps the
	// ped in a firing posture in between. docs/protocol.md §1.9.
	PF_FIRING = 1 << 1,
};

// Field sources are re3 CPed/CPhysical members, see docs/protocol.md §1.7.
//
// Two animation slots because a ped's clump holds a list of blended
// associations, not a single animation (§1.8). animId is the dominant
// whole-body one, animId2 is the dominant ASSOC_PARTIAL overlay (firing,
// punching, that sort of thing lives here). Either can be ANIM_NONE.
//
// animTime is CAnimBlendAssociation::currentTime in seconds, the phase to
// resume at on the receiver so a run cycle doesn't restart mid-stride.
// blendAmount is NOT sent. The receiver blends in with the engine's own
// delta rather than fighting it.
struct PlayerStateBody {
	Vec3     pos;
	float    heading;       // CPed::m_fRotationCur
	Vec3     moveSpeed;     // CPhysical::m_vecMoveSpeed
	uint8_t  moveState;     // eMoveState
	uint8_t  pedState;      // PedState

	// CPed::m_animGroup, the walking style. Makes a remote player actually
	// face the way they're going.
	//
	// GTA III has no sideways-walk animation id. Strafing is a whole other
	// group instead: ASSOCGRP_PLAYERLEFT, _PLAYERRIGHT and _PLAYERBACK each
	// hold their own ANIM_STD_WALK/ANIM_STD_RUN, and
	// CPlayerPed::ProcessAnimGroups swaps the group once the walk angle
	// crosses 50 degrees. Same field also carries the weapon (rocket
	// launcher = ASSOCGRP_PLAYERROCKET, pistol = ASSOCGRP_PLAYER1ARMED, etc).
	//
	// So animId alone means nothing without this. Skip it and a player
	// strafing left gets sent plain ANIM_STD_RUN, which the receiver plays
	// from the default group, which is a forward run, so they slide sideways while
	// sprinting at nothing. First bug anyone spotted on screen.
	uint8_t  animGroup;     // AssocGroupId, 0..NUM_ANIM_ASSOC_GROUPS-1

	uint16_t animId;        // AnimationId, or ANIM_NONE
	float    animTime;      // CAnimBlendAssociation::currentTime, seconds
	float    animSpeed;     // CAnimBlendAssociation::speed
	uint16_t animId2;       // partial-overlay AnimationId, or ANIM_NONE
	float    animTime2;
	float    health;
	float    armour;
	uint8_t  weapon;        // eWeaponType
	// World-space aim direction. yaw is CPed::m_fLookDirection while aiming;
	// pitch is CPedIK::m_torsoOrient.pitch. §1.8.3 on why only yaw is applied.
	float    aimYaw, aimPitch;
	uint8_t  flags;         // PlayerFlags
};

struct C_PlayerState {
	static constexpr uint8_t OPCODE = OP_C_PLAYER_STATE;
	PacketHeader hdr;
	PlayerStateBody body;
};

struct S_PlayerState {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_STATE;
	PacketHeader hdr;
	uint8_t playerId;
	PlayerStateBody body;
};

enum VehicleFlags : uint8_t {
	VEH_ENGINE_ON = 1 << 0,   // CVehicle::bEngineOn
	VEH_SIREN     = 1 << 1,   // CVehicle::m_bSirenOrAlarm
	VEH_LIGHTS    = 1 << 2,
};

struct VehicleStateBody {
	uint16_t netId;
	Vec3     pos;
	Quat     rot;
	Vec3     moveSpeed;     // CPhysical::m_vecMoveSpeed
	Vec3     turnSpeed;     // CPhysical::m_vecTurnSpeed
	float    steer, gas, brake;   // CVehicle::m_fSteerAngle/m_fGasPedal/m_fBrakePedal
	uint8_t  gear;          // CVehicle::m_nCurrentGear
	float    health;        // CVehicle::m_fHealth, 1000 = full
	uint8_t  flags;         // VehicleFlags
};

struct C_VehicleState {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_STATE;
	PacketHeader hdr;
	VehicleStateBody body;
};

struct S_VehicleState {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_STATE;
	PacketHeader hdr;
	uint8_t playerId;       // driver
	VehicleStateBody body;
};

// ---- combat (CH_EVENT) ---------------------------------------------------

// One discharge of one weapon, as the shooter's machine saw it.
//
// Per-event on the reliable channel, not a snapshot bit. A shot is discrete,
// a snapshot is a sample, and an Uzi can empty a clip between two 25 Hz ticks,
// and a dropped rocket is a missing explosion, not a slightly wrong one.
// PF_FIRING in the snapshot covers the state of holding the trigger; this
// covers actual shots fired.
//
// origin is whatever the engine used as the fire source: CWeapon::Fire's
// fireSource, the muzzle, not the ped's centre.
//
// dir and speed mean different things depending on weapon type, which is
// really the whole reason this struct exists:
//
//   instant hit (pistol, uzi, shotgun, AK, M16): dir is the shooter's forward
//     vector at the moment of firing, speed is 0. The receiver doesn't
//     actually aim with dir. It replays through the engine's own
//     CWeapon::Fire, which aims off the ped's matrix, and the pose stream
//     already keeps that close enough. Sent anyway since the owner's aim is
//     the only correct one, and M3's damage model will want it.
//
//   projectile (rocket, molotov, grenade): dir is the unit direction of the
//     projectile's initial CPhysical::m_vecMoveSpeed, speed its magnitude.
//     These get applied exactly, because a molotov arc that starts from a
//     locally re-derived velocity lands on a different street, so the velocity
//     CProjectileInfo::AddProjectile computes depends on the thrower's
//     heading and the throw charge, neither of which an observer knows
//     first-hand.
struct ShotBody {
	uint8_t weapon;       // eWeaponType
	Vec3    origin;
	Vec3    dir;
	float   speed;
};

struct C_Shot {
	static constexpr uint8_t OPCODE = OP_C_SHOT;
	PacketHeader hdr;
	ShotBody body;
};

struct S_Shot {
	static constexpr uint8_t OPCODE = OP_S_SHOT;
	PacketHeader hdr;
	uint8_t  playerId;
	ShotBody body;
};

struct DamageBody {
	uint16_t victimNetId;
	uint8_t  weapon;
	float    amount;
	uint8_t  piece;         // ePedPieceTypes
};

struct C_Damage {
	static constexpr uint8_t OPCODE = OP_C_DAMAGE;
	PacketHeader hdr;
	DamageBody body;
};

struct S_Damage {
	static constexpr uint8_t OPCODE = OP_S_DAMAGE;
	PacketHeader hdr;
	uint8_t    attackerId;
	DamageBody body;
};

struct S_Death {
	static constexpr uint8_t OPCODE = OP_S_DEATH;
	PacketHeader hdr;
	uint8_t  playerId;
	uint16_t killerNetId;
	uint16_t animId;
};

// Where a player's explosion actually went off.
//
// A projectile flies for a couple seconds and GTA III's physics is
// frame-rate coupled (§1.2), so two machines starting the same molotov from
// the same place at the same velocity still don't land it in the same spot, and
// the gap grows with every bounce. Observers animate the projectile locally
// but don't get to decide where it ends: only the thrower's machine says
// where it exploded, everyone else just plays that.
//
// Same idea as correcting a locally-simulated remote car
// (client/src/game/vehicle.h). Also why this needs its own event instead of
// being derived from C_Shot.
struct ExplosionBody {
	uint8_t type;         // eExplosionType: 0 grenade, 1 molotov, 2 rocket
	Vec3    pos;
};

struct C_Explosion {
	static constexpr uint8_t OPCODE = OP_C_EXPLOSION;
	PacketHeader  hdr;
	ExplosionBody body;
};

struct S_Explosion {
	static constexpr uint8_t OPCODE = OP_S_EXPLOSION;
	PacketHeader  hdr;
	uint8_t       playerId;   // whose explosion it is
	ExplosionBody body;
};

struct RespawnBody {
	Vec3  pos;
	float heading;
};

struct C_Respawn {
	static constexpr uint8_t OPCODE = OP_C_RESPAWN;
	PacketHeader hdr;
	RespawnBody body;
};

struct S_Respawn {
	static constexpr uint8_t OPCODE = OP_S_RESPAWN;
	PacketHeader hdr;
	uint8_t     playerId;
	RespawnBody body;
};

// ---- vehicles (CH_EVENT) -------------------------------------------------

// Getting into a car. First time anyone gets into a given car, this is also
// what tells the session it exists.
//
// GTA III spawns its own traffic and parked cars locally, differently on
// every machine, so there's no shared vehicle world to point at. Instead of
// trying to sync all of Liberty City's traffic, a car enters the session
// only when someone gets in it: client sends its identity with
// netId == INVALID_NETID, server hands out a netId and tells everyone else
// to spawn a matching one. Cars nobody's touched just stay local and
// unsynced. Cheap, and nobody notices two players seeing different traffic.
//
// Identity fields only matter when netId is INVALID_NETID. The normal case
// (getting into a car the session already knows) just needs netId and seat.
struct EnterVehicleBody {
	uint16_t netId;
	uint8_t  seat;          // 0 is the driver
	uint8_t  jack;          // pulling the current occupant out

	uint16_t modelId;
	uint8_t  colour1, colour2;
	uint8_t  pad;
	Vec3     pos;
	Quat     rot;
};

struct C_EnterVehicle {
	static constexpr uint8_t OPCODE = OP_C_ENTER_VEHICLE;
	PacketHeader hdr;
	EnterVehicleBody body;
};

struct S_EnterVehicle {
	static constexpr uint8_t OPCODE = OP_S_ENTER_VEHICLE;
	PacketHeader hdr;
	uint8_t          playerId;
	EnterVehicleBody body;
};

struct C_ExitVehicle {
	static constexpr uint8_t OPCODE = OP_C_EXIT_VEHICLE;
	PacketHeader hdr;
	uint16_t netId;
};

struct S_ExitVehicle {
	static constexpr uint8_t OPCODE = OP_S_EXIT_VEHICLE;
	PacketHeader hdr;
	uint8_t  playerId;
	uint16_t netId;
};

struct S_VehicleSpawn {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_SPAWN;
	PacketHeader hdr;
	uint16_t netId;
	uint16_t modelId;
	Vec3     pos;
	Quat     rot;
	uint8_t  colour1, colour2;   // CVehicle::m_currentColour1/2
};

struct S_VehicleDespawn {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_DESPAWN;
	PacketHeader hdr;
	uint16_t netId;
};

// ---- world (CH_EVENT) ----------------------------------------------------

struct S_WorldState {
	static constexpr uint8_t OPCODE = OP_S_WORLD_STATE;
	PacketHeader hdr;
	uint8_t hour, minute;
	uint8_t weather;        // eWeatherType: 0 sunny, 1 cloudy, 2 rainy, 3 foggy
};

// ---- chat (CH_EVENT) -----------------------------------------------------

struct C_Chat {
	static constexpr uint8_t OPCODE = OP_C_CHAT;
	PacketHeader hdr;
	char text[CHAT_LEN];
};

struct S_Chat {
	static constexpr uint8_t OPCODE = OP_S_CHAT;
	PacketHeader hdr;
	uint8_t playerId;
	char    text[CHAT_LEN];
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader)    == 5,  "header layout");
static_assert(sizeof(PlayerStateBody) == 65, "player state layout");
static_assert(sizeof(C_PlayerState)   == 70, "player snapshot layout");
static_assert(sizeof(S_PlayerState)   == 71, "player snapshot layout");
static_assert(offsetof(PlayerStateBody, animGroup) == 30, "animation block");
static_assert(offsetof(PlayerStateBody, animId)   == 31, "animation block");
static_assert(offsetof(PlayerStateBody, animId2)  == 41, "animation block");
static_assert(offsetof(PlayerStateBody, aimYaw)   == 56, "aim block");
static_assert(offsetof(PlayerStateBody, flags)    == 64, "flags is last");
static_assert(sizeof(VehicleStateBody)== 72, "vehicle state layout");
static_assert(sizeof(S_VehicleState)  == 78, "vehicle snapshot layout");
static_assert(sizeof(C_Hello)         == 33, "hello layout");
static_assert(sizeof(S_Welcome)       == 14, "welcome layout");

// 2 netId + 1 seat + 1 jack + 2 model + 1 + 1 colour + 1 pad + 12 pos + 16 rot
static_assert(sizeof(EnterVehicleBody) == 37, "enter-vehicle layout");
static_assert(sizeof(C_EnterVehicle)  == 42, "enter-vehicle layout");
static_assert(sizeof(S_EnterVehicle)  == 43, "enter-vehicle layout");
static_assert(sizeof(S_VehicleSpawn)  == 39, "vehicle spawn layout");
static_assert(sizeof(C_PlayerModel)   == 7,  "player model layout");
static_assert(sizeof(S_PlayerModel)   == 8,  "player model layout");

// 1 weapon + 12 origin + 12 dir + 4 speed
static_assert(sizeof(ShotBody)        == 29, "shot layout");
static_assert(sizeof(C_Shot)          == 34, "shot layout");
static_assert(sizeof(S_Shot)          == 35, "shot layout");
static_assert(offsetof(ShotBody, speed) == 25, "speed follows dir");
static_assert(sizeof(ExplosionBody)   == 13, "explosion layout");
static_assert(sizeof(C_Explosion)     == 18, "explosion layout");
static_assert(sizeof(S_Explosion)     == 19, "explosion layout");

} // namespace coopiii
