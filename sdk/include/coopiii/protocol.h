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
// 7: damage, death and respawn. DamageBody grew `direction`, S_Welcome grew
//    `flags` so a client knows whether the session has friendly fire on, and
//    C_Death was added because the shapes reserved at version 5 had the
//    server announcing a death it has no way of knowing about.
//    docs/protocol.md §1.10.
// 8: time of day and weather follow the host's game instead of the server's
//    synthetic clock. C_WorldState added, S_WorldState and S_Welcome carry
//    hostPlayerId and the second weather type. docs/protocol.md §2.7.
constexpr uint16_t PROTOCOL_VERSION = 8;
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
	// Out of order because the block above was numbered before anyone had
	// worked out who gets to announce a death. It isn't the server: only the
	// machine that owns a player knows what that player's health really is.
	// See C_Death.
	OP_C_DEATH           = 0x29,

	OP_C_ENTER_VEHICLE   = 0x30,
	OP_S_ENTER_VEHICLE   = 0x31,
	OP_C_EXIT_VEHICLE    = 0x32,
	OP_S_EXIT_VEHICLE    = 0x33,
	OP_S_VEHICLE_SPAWN   = 0x34,
	OP_S_VEHICLE_DESPAWN = 0x35,

	OP_S_WORLD_STATE     = 0x40,
	OP_C_WORLD_STATE     = 0x41,

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

// Session-wide rules a client has to know about, handed over in S_Welcome.
enum SessionFlags : uint8_t {
	// docs/roadmap.md §5.2: server-configurable, off by default. The server
	// is the one that enforces it, by refusing to relay a C_Damage between
	// players at all. This bit exists because one kind of damage never
	// reaches the server: an explosion is replayed at a fixed world position
	// and every machine decides for itself whether its own player is
	// standing in it (§1.9.2). A client that knows friendly fire is off
	// makes its own player immune for the length of that replay.
	SESSION_FRIENDLY_FIRE = 1 << 0,
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
	uint8_t  weatherOld;
	// Who the session is taking its time of day from, or INVALID_PLAYER
	// while nobody has been picked. Here as well as in S_WorldState so a
	// newcomer who turns out to be the host never applies the hour above:
	// the host's own game is what that hour is supposed to be tracking.
	uint8_t  hostPlayerId;
	uint8_t  flags;         // SessionFlags
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

	// ASSOC_RUNNING on the animation in animId2, and it is the difference
	// between a player aiming and a player firing.
	//
	// A weapon has one animation covering the draw, the ready pose, the shot
	// and the recovery. CPed::PointGunAt parks it on the ready frame and
	// clears ASSOC_RUNNING; CPed::FireGun sets it running and loops it over
	// the firing part. Send the id and the phase without this bit and the
	// receiver has no way to tell the two apart, so it plays the whole thing
	// from the top, forever, and what you see is a player drawing their gun
	// over and over and never firing it.
	//
	// Costs nothing: a spare bit in a byte that was already on the wire.
	PF_ANIM2_RUNNING = 1 << 2,
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

// One hit, as the attacker's machine resolved it, before anything was
// applied to anyone.
//
// This is the shooter's own CPed::InflictDamage call, taken away from their
// engine and put on the wire instead. Every field is an argument of that
// function, unchanged: no multiplier, no armour, no reduction. The victim's
// machine feeds them back into its own InflictDamage, so armour, the player's
// own damage multiplier, the hit reaction and death all happen exactly where
// single player puts them.
//
// Why the attacker decides and the victim applies, rather than either one
// doing both:
//
//   The attacker fired a ray from a position only they know, at an instant
//   only they know. Let the victim work out whether they were hit and the
//   question becomes "was A's ped, interpolated 100 ms late, in front of me",
//   which is how you get shot around corners. So the hit is the attacker's.
//
//   The health is the victim's. Nobody else has their armour, their current
//   state, or whether some mission just made them invulnerable, and two
//   machines subtracting from the same health pool disagree within seconds.
//
// docs/protocol.md §1.10.
struct DamageBody {
	uint16_t victimNetId;
	uint8_t  weapon;        // eWeaponType, and only the ones §1.10.1 allows
	float    amount;        // CPed::InflictDamage's `damage`, raw
	uint8_t  piece;         // ePedPieceTypes, 0..6
	// Which side the hit came from, 0 front, 1 left, 2 back, 3 right. Picks
	// between the four ANIM_STD_HIGHIMPACT_* reactions. Costs a byte and is
	// the difference between being knocked the way you were shot and always
	// falling on your face.
	uint8_t  direction;
};

struct C_Damage {
	static constexpr uint8_t OPCODE = OP_C_DAMAGE;
	PacketHeader hdr;
	DamageBody body;
};

// Sent to the victim alone, not broadcast. Nobody else needs it: the health
// it produces rides the victim's own snapshots a moment later, and the hit
// reaction is an animation that rides them too.
struct S_Damage {
	static constexpr uint8_t OPCODE = OP_S_DAMAGE;
	PacketHeader hdr;
	uint8_t    attackerId;
	DamageBody body;
};

// "I died." Sent by the machine whose player it is, and by no one else.
//
// The version-5 draft had only S_Death, which put the decision on the server.
// That contradicts the one rule the whole design rests on: a player's health
// lives on their own machine, so their own machine is the only thing that can
// say when it ran out. The server relays and keeps score; it doesn't decide.
//
// animId is the animation the engine picked for this particular death, taken
// from the CPed::SetDie call it made. A headshot, a drowning and a car
// knocking you over are three different animations and the observer has no
// way to work out which. ANIM_NONE means the sender couldn't capture one and
// the observer should use its default.
struct C_Death {
	static constexpr uint8_t OPCODE = OP_C_DEATH;
	PacketHeader hdr;
	// Whoever damaged the sender last, if it was recent enough to be the
	// reason. INVALID_NETID for drowning, a fall, a car, or a kill nobody
	// has a claim on.
	uint16_t killerNetId;
	uint16_t animId;
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

// "I'm alive again, over here."
//
// GTA III resurrects the same CPed rather than making a new one
// (CGameLogic::RestorePlayerStuffDuringResurrection), so on the owner's
// machine a respawn is a teleport and a health reset and nothing else. On
// every other machine it isn't: what they have is a corpse, in the state
// CPed::SetDie left it, with its collision cleared and its health at zero.
// There's no un-die, so the corpse gets destroyed and a fresh ped built the
// same way the first one was.
//
// The transform is here because the two ends of that are half a city apart.
// A ped rebuilt from the snapshot stream alone would be born at the place its
// owner died and then snap to the hospital once the buffer caught up.
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

// Time of day and sky, as one player's game has them.
//
// The session follows a designated host player rather than a clock the
// server keeps on its own. The server has no GTA III running, so a clock it
// invented is nobody's; the host has one, the campaign's script can move it
// (SET_TIME_OF_DAY, FORCE_WEATHER), and docs/campaign.md already puts the
// script on the host. §2.7 is the argument in full.
//
// Two weather types because CWeather doesn't have one. It blends from
// OldWeatherType to NewWeatherType across a game hour, so a single type
// describes the destination and not the sky. The blend position isn't sent:
// CWeather::Update recomputes it as CClock::GetMinutes()/60 every frame, so
// once the clock matches, the blend matches for free.
struct WorldStateBody {
	uint8_t hour, minute;
	uint8_t weather;        // eWeatherType: 0 sunny, 1 cloudy, 2 rainy, 3 foggy
	uint8_t weatherOld;     // the one being blended out of
};

// Only the host sends this, once a second. The server drops it from anyone
// else, the same way it drops a vehicle snapshot from a player who isn't
// driving that vehicle.
struct C_WorldState {
	static constexpr uint8_t OPCODE = OP_C_WORLD_STATE;
	PacketHeader   hdr;
	WorldStateBody body;
};

struct S_WorldState {
	static constexpr uint8_t OPCODE = OP_S_WORLD_STATE;
	PacketHeader   hdr;
	WorldStateBody body;
	// The current host. Carried on every world packet rather than announced
	// once, because it's how a client finds out it has become the host after
	// the previous one quit, and because it's free here.
	uint8_t        hostPlayerId;
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
static_assert(sizeof(S_Welcome)       == 17, "welcome layout");

static_assert(sizeof(WorldStateBody)  == 4,  "world state layout");
static_assert(sizeof(C_WorldState)    == 9,  "world state layout");
static_assert(sizeof(S_WorldState)    == 10, "world state layout");

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

// 2 victim + 1 weapon + 4 amount + 1 piece + 1 direction
static_assert(sizeof(DamageBody)      == 9,  "damage layout");
static_assert(sizeof(C_Damage)        == 14, "damage layout");
static_assert(sizeof(S_Damage)        == 15, "damage layout");
static_assert(offsetof(DamageBody, piece) == 7, "piece and direction are last");
static_assert(sizeof(C_Death)         == 9,  "death layout");
static_assert(sizeof(S_Death)         == 10, "death layout");
static_assert(sizeof(RespawnBody)     == 16, "respawn layout");
static_assert(sizeof(C_Respawn)       == 21, "respawn layout");
static_assert(sizeof(S_Respawn)       == 22, "respawn layout");

} // namespace coopiii
