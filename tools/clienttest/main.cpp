// Unit tests for the client roster and the engine seam, no socket and no
// game involved. Drives Client::HandleMessage directly and checks what it
// asks the WorldBridge to do.
//
//   xmake build clienttest && xmake run clienttest
//
// This is the layer where mistakes stay quiet: a player who joins but never
// gets spawned, a ped left behind after a disconnect, our own snapshots
// echoed back and rendered as a second copy of ourselves. None of that
// crashes.

#include "client.h"
#include "game/boat.h"
#include "game/cardamage.h"
#include "game/carlife.h"
#include "game/combat.h"
#include "game/darkel.h"
#include "game/driveby.h"
#include "game/garage.h"
#include "game/heli.h"
#include "game/heligun.h"
#include "game/horn.h"
#include "game/look.h"
#include "game/melee.h"
#include "game/movinglist.h"
#include "game/nametag.h"
#include "game/observed.h"
#include "game/pedanim.h"
#include "game/pickup.h"
#include "game/population.h"
#include "game/radar.h"
#include "game/sessionclock.h"
#include "game/vehicle.h"
#include "game/wanted.h"
#include "liftbridgetime.h"
#include "lighttime.h"
#include "planetime.h"
#include "traintime.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace coopiii;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

template <class T>
Message Wrap(const T &pkt, Channel ch) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = ch;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

// hostPlayerId defaults to "nobody", so the tests that predate the world
// clock keep behaving the way they did: not the host, and nothing to follow.
S_Welcome MakeWelcome(uint8_t playerId, uint8_t reject = 0,
                      uint8_t hostPlayerId = INVALID_PLAYER) {
	S_Welcome w;
	InitHeader(w, 1000);
	w.reject       = reject;
	w.playerId     = playerId;
	w.netId        = uint16_t(100 + playerId);
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hour         = 12;
	w.minute       = 0;
	w.weather      = 0;
	w.weatherOld   = 0;
	w.hostPlayerId = hostPlayerId;
	// A default welcome: no friendly fire, and the per-player wanted rule,
	// which is what a zero byte means. This used to be left indeterminate,
	// which made every test that did not set it explicitly depend on whatever
	// was on the stack - and the wanted tests below are the first ones that
	// actually read it.
	w.flags        = 0;
	return w;
}

S_WorldState MakeWorldState(uint8_t hostPlayerId, uint8_t hour, uint8_t minute,
                            uint8_t weather = 0, uint8_t weatherOld = 0) {
	S_WorldState w;
	InitHeader(w, 1000);
	w.body.hour       = hour;
	w.body.minute     = minute;
	w.body.weather    = weather;
	w.body.weatherOld = weatherOld;
	w.hostPlayerId    = hostPlayerId;
	return w;
}

S_PlayerJoin MakeJoin(uint8_t playerId, const char *nick, uint16_t modelId = 7) {
	S_PlayerJoin j;
	InitHeader(j, 1000);
	j.playerId = playerId;
	j.netId    = uint16_t(200 + playerId);
	j.modelId  = modelId;
	j.pos      = {1.0f, 2.0f, 3.0f};
	j.heading  = 0.0f;
	std::memset(j.nick, 0, sizeof(j.nick));
	std::strncpy(j.nick, nick, sizeof(j.nick) - 1);
	return j;
}

S_PlayerState MakeState(uint8_t playerId, uint32_t timeMs, float x) {
	S_PlayerState s;
	InitHeader(s, timeMs);
	s.playerId       = playerId;
	s.body.pos       = {x, 0.0f, 0.0f};
	s.body.heading   = 0.0f;
	s.body.moveSpeed = {1.0f, 0.0f, 0.0f};
	s.body.health    = 100.0f;
	return s;
}

// The same snapshot, carrying stars. docs/wanted.md §4.3: the level is three
// spare bits of a flags byte that was already going out, and the fourth says
// whether it is this player's own or the session's.
S_PlayerState MakeWantedState(uint8_t playerId, uint32_t timeMs, float x,
                              uint8_t wanted, bool borrowed = false) {
	S_PlayerState s = MakeState(playerId, timeMs, x);
	s.body.flags    = FlagsWithWanted(s.body.flags, wanted, borrowed);
	return s;
}

// ---- a recording bridge ---------------------------------------------------

struct Recorder {
	std::vector<uint16_t> modelRequests;
	int                   spawns    = 0;
	int                   despawns  = 0;
	int                   poses     = 0;
	Pose                  lastPose  = {};
	bool                  modelReady = false;
	// What SampleLocalPlayerLook reads back, and what PrepareRemoteLook says.
	char                  localLook[PLAYER_LOOK_LEN] = {};
	bool                  lookReady    = false;
	int                   lookPrepares = 0;
	int                   nextHandle = 1;
	// Set to make the next pose write behave the way the real bridge does
	// when the engine has destroyed the ped under us: drop the handle and
	// re-arm the spawn. See game/ped.cpp ResolveRemote.
	bool                  loseNextPose = false;

	// The local player's CWanted, such as it is. `havePlayerWanted` off is
	// the menus/loading/just-died case, where the real bridge reports false
	// and Client has to leave the whole thing alone rather than read a zero.
	bool    havePlayerWanted = true;
	uint8_t engineWanted     = 0;
	int     wantedWrites     = 0;
	uint8_t lastWantedWrite  = 0xFF;

	int              vehicleSpawns   = 0;
	int              vehicleDespawns = 0;
	int              vehicleApplies  = 0;
	// WorldBridge::UpdateTrafficAllowance, and how many cars had been built
	// when it last ran.
	int              trafficAllowances       = 0;
	int              vehicleSpawnsAtAllowance = 0;
	int              vehicleAtRest   = 0;
	// WorldBridge::SurrenderVehicleSeat - the losing end of a carjack.
	int              vehicleSurrenders = 0;

	// Custody of a car nobody is driving (protocol.h, S_VehicleCustody).
	// `carAtRest` is what the engine would answer about the car this machine
	// is settling; the default is "still moving", because a settle that ended
	// on its first frame would test nothing.
	int              observedSamples  = 0;
	bool             observedSampleOk = true;
	int              atRestCalls      = 0;
	bool             carAtRest        = false;
	// Whether the car we are settling is on fire. Not, by default.
	bool             carBurning       = false;
	// What the engine would say about the dents on a car: the one we are
	// settling, and the one we are driving. Undamaged by default, so no test
	// written before these existed ever sees a report go out.
	int               observedDamageSamples = 0;
	VehicleDamageBody observedDamage{};
	VehicleDamageBody localDamage{};
	// A traffic car that has stopped being traffic (S_CarPromoted).
	int              promotedAdoptions    = 0;
	bool             lastPromotionWasOurs = false;
	// A car the local player claimed, registered with the detours' table
	// when the claim comes back, and let go of when the session drops it.
	int              claimedAdoptions = 0;
	int              ownReleases      = 0;
	// Vehicle handles the fake pool no longer honours, as if the engine had
	// reaped the car. Everything else resolves.
	std::vector<int32_t> reapedVehicleHandles;
	// The engine ref of the ambient replica the local player is at the wheel
	// of, or -1 for on foot. Matched against the row's own handle, the same
	// way localVehicleHandle is, so a test that puts the player in one car
	// does not accidentally claim another.
	int32_t          ambientDrivenHandle = -1;
	// What shape a car was told it is in, and whether the parts were told to
	// fly off. The flag is the interesting one: a live change makes them fly,
	// a spawn or a backfill must not, or a joiner is greeted by a shower of
	// doors out of the object pool.
	int               vehicleDamageApplies = 0;
	VehicleDamageBody lastVehicleDamage{};
	bool              lastDamageFlying = false;
	int              vehicleCorrections = 0;
	VehicleTransform lastCorrection  = {};
	// What the row said about the horn when it was handed to the correction,
	// which is the call the engine seam sounds it from.
	bool             lastCorrectionHorn = false;
	VehicleStateBody lastVehicleBody = {};
	int              nextVehicleHandle = 1;

	// What SampleLocalVehicleIdentity reports. `drivingLocally` off means the
	// player is on foot, which is the default.
	bool     drivingLocally = false;
	uint16_t localModel     = 90;
	// The m_vecMoveSpeed both vehicle samplers report, in the engine's unit.
	// Zero unless a test is about it.
	Vec3     sampledMoveSpeed = {};
	// The engine ref of the car the local player is in, as
	// SampleLocalVehicleHandle would report it. -1 is "a car from our own
	// world", which is every car for anybody who was in the session when it
	// was claimed; a handle matching a spawned RemoteVehicle is the late
	// joiner's case, where the car in the street is one CoopIII made.
	int32_t  localVehicleHandle = -1;

	// The extras on the car the local player is driving, as its own engine
	// rolled them. 3 and -1 rather than 0 and 0 so a claim that dropped them
	// is distinguishable from one that carried them.
	int8_t   localExtra1    = 3;
	int8_t   localExtra2    = -1;

	// Seating. `seatAttempts` counts calls, `seats` counts the ones that
	// took - the difference is the whole point of the refusal case below.
	int      seatAttempts    = 0;
	int      seats           = 0;
	int      unseats         = 0;
	uint16_t lastSeatVehicle = INVALID_NETID;
	uint8_t  lastSeatIndex   = 0xFF;
	// Makes SeatRemotePed say no with both halves still present, i.e. the
	// engine refusing a warp. Nothing about a later frame changes that
	// answer, so the client has to stop asking.
	bool     refuseSeating   = false;
	// How many unseats had happened by the time the last vehicle despawn ran.
	// A car destroyed under a seated ped leaves that ped following a null
	// pointer, so the order here is not an implementation detail - it matters.
	int      unseatsAtLastVehicleDespawn = -1;
	// Whether the pose write was told the ped is seated. The suppression
	// itself lives in game/ped.cpp and needs a running game; what can be
	// pinned down here is that the flag it reads says the right thing.
	int      posesWhileSeated = 0;

	// The door-opening version of the same thing. `animEntry` is what the
	// engine would say to BeginSeatRemotePed: false is a refusal, which is
	// every existing test in this file, because every one of them was
	// written before there was an animation to refuse.
	bool     animEntry       = false;
	uint8_t  animProgress    = SEAT_RUNNING;
	int      seatAnimBegins  = 0;
	// Which door the last animated entry was asked to use, as a seat number.
	// 0xFF until one is. This is the byte the whole intent packet exists to
	// carry, so a test that does not look at it is not testing the fix.
	uint8_t  lastSeatDoor    = 0xFF;

	// A jack played here (S_JackingVehicle). `jackStarts` is what
	// BeginJackRemotePed says, `jackProgress` what polling it says.
	bool     jackStarts      = true;
	uint8_t  jackProgress    = SEAT_RUNNING;
	int      jackBegins      = 0;
	int      jackPolls       = 0;
	int32_t  lastJackHandle  = -1;
	uint8_t  lastJackDoor    = 0xFF;
	// What our engine is doing to a seated ped: by player slot, to the ambient
	// replicas, and to the local player. A PullOut.
	uint8_t  remotePull[MAX_PLAYERS] = {};
	uint8_t  ambientPull     = PULL_NONE;
	uint8_t  localPull       = PULL_NONE;

	// What our own engine says we are doing about a car right now, and how
	// many times it was asked. The sampler is cheap and runs every tick; the
	// packet must not.
	bool          localEntryActive  = false;
	LocalCarEntry localEntry{};
	int           localEntrySamples = 0;
	int      seatAnimPolls   = 0;
	int      seatAnimAborts  = 0;
	int      exitAnimBegins  = 0;
	// The counts as they stood when the last abort ran. A door given back
	// after the ped has already been warped into the seat is a door given
	// back too late, so the ordering is the thing worth pinning.
	int      seatsAtLastAbort = -1;

	// Combat. `shotsReplayed` counts calls that reached the bridge at all -
	// that's the interesting number. A shot offered to a player with no ped
	// has to be dropped rather than queued, and the only way to see that
	// from here is that the bridge was never asked.
	int      shotsReplayed  = 0;
	int      explosions     = 0;
	ShotBody lastShot{};
	ExplosionBody lastExplosion{};
	uint8_t  lastShotPlayer = 0xFF;

	// What DrainLocalCombat will hand over next, and how many times it was
	// asked for anything at all.
	std::vector<CombatEvent> localCombat;
	int                      drains = 0;

	// Damage, death and respawn. `damageAttacker` is 0xFF when the bridge
	// was handed a null attacker, which is the "their ped hasn't streamed
	// in" case and still has to hurt.
	int        damages        = 0;
	DamageBody lastDamage{};
	uint8_t    damageAttacker = 0xFF;
	int        kills          = 0;
	uint8_t    lastKilled     = 0xFF;
	uint16_t   lastKillAnim   = ANIM_NONE;
	int        friendlyFireCalls = 0;
	bool       friendlyFire      = false;

	// Ammunition (docs/protocol.md 1.9.6). `ammoSyncCalls` is how many times
	// the switch reached the engine seam at all: with it never called, the
	// seam keeps the behaviour it had before ammo sync existed, which is the
	// thing the off case has to preserve.
	int          ammoSyncCalls  = 0;
	bool         ammoSync       = false;
	int          ammoSamples    = 0;
	int          ammoApplies    = 0;
	AmmoSlotBody lastAmmoSlot{};
	uint8_t      lastAmmoPlayer = 0xFF;
	// What SampleLocalAmmo will report, and whether there is a player ped to
	// read at all.
	AmmoSlotBody localAmmo[INVENTORY_SLOTS] = {};
	uint8_t      localHeld     = 0;
	bool         haveLocalAmmo = true;
	// A car blowing up. `blowUps` counts calls that reached the bridge at
	// all, which is the number that matters: an event for a car we do not
	// have has to be dropped rather than queued.
	// The extras the roster had on the entry when the spawn was asked for.
	int8_t   lastSpawnExtra1 = 0;
	int8_t   lastSpawnExtra2 = 0;

	int      blowUps          = 0;
	uint16_t lastBlowUpNetId  = INVALID_NETID;
	Vec3     lastBlowUpPos    = {};
	// Makes BlowUpRemoteVehicle report that the car is no longer in the pool,
	// which is a "not now" and must still leave the roster entry destroyed.
	bool     blowUpFails      = false;
	// What DrainLocalVehicleBlasts hands over next.
	std::vector<LocalVehicleBlast> localBlasts;

	// Cars nobody owns (docs/roadmap.md 5.8). `unownedOutcome` is what the
	// engine seam pretends happened: the default is that we did the
	// destroying, and the other three are the reasons an instruction is or is
	// not retired.
	int                 unownedWrecks  = 0;
	UnownedVehicleKey   lastUnownedKey = {};
	UnownedWreckOutcome unownedOutcome = UnownedWreckOutcome::Wrecked;
	std::vector<UnownedBlast> unownedBlasts;

	// Hits somebody else landed on the car *this* machine is driving. The one
	// packet about a claimed car that travels towards its owner.
	int            vehicleHits        = 0;
	VehicleHitBody lastVehicleHit{};
	// The attacker slot the client resolved, or 0xFF for none - recorded as an
	// id rather than a pointer for the same reason pedDamageAttacker is: a null
	// attacker is a real and ordinary case, not a failure.
	uint8_t        vehicleHitAttacker = 0xFF;
	uint16_t       lastVehicleHitRow  = INVALID_NETID;
	// Whether Client handed it over as the car's custodian rather than its
	// driver, which changes what the engine end checks.
	bool           lastVehicleHitSettling = false;
	// And the outbound queue, so the send path's filters can be exercised
	// without a socket.
	std::vector<VehicleHitBody> localVehicleHits;
	int                         vehicleHitDrains = 0;

	// The traffic pair (§1.23): hits on a car this machine hosts, and hits the
	// detour queued against somebody else's.
	int                         carHits        = 0;
	VehicleHitBody              lastCarHit{};
	uint8_t                     carHitAttacker = 0xFF;
	std::vector<VehicleHitBody> localCarHits;
	int                         carHitDrains   = 0;

	// The traffic half of the same thing, through game/population.cpp rather
	// than game/vehicle.cpp - a hosted car's netId rather than a name the map
	// hands out.
	int                 ambientWrecks        = 0;
	uint16_t            lastAmbientWreckNetId = INVALID_NETID;
	BlastTransform      lastAmbientWreckWhere = {};
	UnownedWreckOutcome ambientWreckOutcome  = UnownedWreckOutcome::Wrecked;
	std::vector<UnownedBlast> ambientWreckQueue;

	// Garages. `localGarageMask` is what this machine's own state machine
	// would report, `appliedGarageMask` is the union the client pushed back
	// into the seam. `haveGarages` off is the frontend, where CGarages
	// is not being updated at all.
	uint32_t localGarageMask   = 0;
	bool     haveGarages       = true;
	int      garageSamples     = 0;
	int      garageApplies     = 0;
	uint32_t appliedGarageMask = 0xFFFFFFFFu;   // never a legal first value
	std::vector<LocalRespray> localResprays;
	int         resprayApplies     = 0;
	bool        lastResprayHadCar  = false;
	ResprayBody lastRespray{};

	// World. `world` is what this machine's engine would report; the rest is
	// what the client asked to have done to it.
	WorldState world{12, 0, 0, 0};
	bool       haveWorld       = true;
	int        worldSamples    = 0;
	int        timeApplies     = 0;
	int        weatherApplies  = 0;
	int        weatherReleases = 0;
	uint8_t    appliedHour       = 0xFF;
	uint8_t    appliedMinute     = 0xFF;
	uint8_t    appliedWeather    = 0xFF;
	uint8_t    appliedWeatherOld = 0xFF;

	// The session clock, as PreFrame last handed it over.
	int      sessionClockCalls  = 0;
	bool     sessionClockValid  = false;
	uint32_t sessionClockOffset = 0;

	// Pickups. `grantsConsumed` counts grants the seam could actually use;
	// `grantFails` makes the next one report that the object has gone, which
	// is the case the client has to turn back into a release rather than sit
	// on. The real seam's decision needs a running game; what can be pinned
	// down here is the routing, and the routing is one comparison that is
	// easy to get backwards.
	int         grantsConsumed = 0;
	int         grantFails     = 0;
	int         remoteTakes    = 0;
	int         denials        = 0;
	int         pickupResets   = 0;
	PickupIdent lastGranted{};
	PickupIdent lastRemoteTake{};

	// Ped drops. Same shape and the same limit: whether the engine really
	// makes the pickup needs a running game, and what can be pinned here is
	// that the relay is read as somebody else's rather than as our own.
	int            drops = 0;
	PickupDropBody lastDrop{};

	// Ambient population. `ambientApplies` counts every pose the stream
	// drove into a replica, which is the number that says a pedestrian is
	// walking rather than standing: step 2 shipped a replica that was placed
	// once at creation and never touched again.
	int      ambientPedSpawns   = 0;
	int      ambientPedDespawns = 0;
	int      ambientApplies     = 0;
	int      ambientFires       = 0;
	bool     lastAmbientFireSaid = false;
	Pose     lastAmbientPose{};
	int      nextAmbientPedHandle = 1;

	int      ambientCarSpawns   = 0;
	int      ambientCarDespawns = 0;
	int      nextAmbientCarHandle = 1;

	// A leaver's crowd handed to us (S_AmbientAdopt): how many replicas the
	// engine seam turned into ours, and in what order - a stamp per call off
	// one counter, so a test can tell peds went before cars. `refuseAdoption`
	// makes the seam say no, as it does with its hosted table full.
	int      pedAdoptions     = 0;
	int      carAdoptions     = 0;
	int      adoptionStamp    = 0;
	int      lastPedAdoptedAt = 0;
	int      lastCarAdoptedAt = 0;
	bool     refuseAdoption   = false;

	// A traffic replica made to wear its host's dents, and our own traffic's
	// dents waiting to be drained.
	int               ambientCarDents = 0;
	VehicleDamageBody lastAmbientCarDent{};
	bool              lastAmbientCarDentFlying = false;
	VehicleDamageBody hostedDents[4]{};
	uint32_t          hostedDentCount = 0;

	// Limbs taken off a replica because its host's engine did.
	int      limbs             = 0;
	// And off a remote player's ped.
	int      playerLimbs        = 0;
	uint8_t  lastPlayerLimbOf   = 0xFF;
	uint8_t  lastPlayerLimbNode = 0;
	uint16_t lastLimbPed       = INVALID_NETID;
	uint8_t  lastLimbNode      = 0xFF;
	int8_t   lastLimbDirection = -1;

	// Replicas killed because their host's engine killed the original.
	// `attempts` counts calls and `ambientKills` the ones that took, the
	// same split the seating uses: a refusal is a "not yet" the client has
	// to keep retrying.
	int      ambientKillAttempts = 0;
	int      ambientKills        = 0;
	uint16_t lastKilledPed       = INVALID_NETID;
	uint16_t lastAmbientKillAnim = 0;
	bool     refuseAmbientKill   = false;
	// How many unseats had happened by the time the last kill ran.
	// CPed::SetDie's PED_DRIVING arm hands a non-player ped to the engine to
	// destroy, so the ordering is the thing being pinned, not the effect.
	int      unseatsAtLastAmbientKill = -1;

	// Hits somebody else's player landed on a pedestrian *this* machine hosts.
	// The direction the two above do not have.
	int           pedDamages         = 0;
	PedDamageBody lastPedDamage{};
	// The attacker slot the client resolved, or 0xFF for none. Recorded as an
	// id rather than a pointer for the same reason `damageAttacker` is: a null
	// attacker is a real and ordinary case, not a failure.
	uint8_t       pedDamageAttacker  = 0xFF;

	// Somebody else's pedestrians fighting: what the pose pass armed a
	// replica with, the rounds handed over to be drawn, and the hits on us.
	int        replicaArms        = 0;
	uint8_t    lastReplicaArm     = 0xFF;
	int        npcCarHits         = 0;
	uint16_t   npcCarHitAttacker  = 0;
	uint16_t   npcCarHitRow       = 0;
	bool       replicaArmReady    = true;
	int        npcShots           = 0;
	uint16_t   lastNpcShotPed     = 0;
	ShotBody   lastNpcShot{};
	int        npcDamages         = 0;
	uint16_t   npcDamageAttacker  = 0;   // the replica's netId, 0 for none
	DamageBody lastNpcDamage{};

	// The traffic driver. `attempts` counts calls and `seats` the ones that
	// took, the same split the player seating uses and for the same reason.
	int      ambientSeatAttempts = 0;
	int      ambientSeats        = 0;
	int      ambientUnseats      = 0;
	uint16_t lastAmbientSeatCar  = INVALID_NETID;
	// What CorrectAmbientCarReplica was last told to write into the horn.
	uint8_t  lastAmbientHornTimer = 0;
	// And into the siren byte, and whether the host named a driver for it.
	bool     lastAmbientSirenOn   = false;
	bool     lastAmbientDriverSaid = false;
	int      ambientCorrections   = 0;
	// And where it was told to put the car.
	VehicleTransform lastAmbientCarAt{};
	uint8_t  lastAmbientSeatIdx  = 0xFF;
	bool     refuseAmbientSeating = false;
	// The engine has taken the ped replica away underneath us. What the real
	// AmbientReplicaIsAlive discovers by resolving the pool handle and
	// checking the vtable, the stub is simply told.
	bool     ambientReplicaTaken  = false;
	int      ambientLivenessChecks = 0;
	// What CPools::GetPed would find the replica's m_nPedState to be. The
	// stub feeds it to the real game::ReplicaDiedUnasked, so the client loop
	// is driven by the same decision the engine seam makes rather than by a
	// second copy of it that only agrees with itself.
	uint32_t ambientReplicaState = game::PEDSTATE_NONE;
	int      ambientDeathsRecovered = 0;
	// How many unseats had happened by the time the last ambient car
	// despawn ran. Destroying a car under a seated ped leaves that ped
	// following a null pointer, so the order is the thing being pinned.
	int      unseatsAtLastAmbientCarDespawn = -1;

	// ---- the seat key, on this machine --------------------------------
	//
	// The local half of riding in somebody else's car. The engine walks the
	// player to the door and that takes about a second, so `localSeatAnswer`
	// is what SeatLocalPlayerIn says on the frame of the press and
	// `localPollAnswer` is what PollLocalSeatEntry says afterwards - which is
	// the whole shape of the thing being tested, because the bug was *when*
	// the session gets told rather than what it gets told.
	bool     wantSeatToggle   = false;
	bool     localIsPassenger = false;
	int32_t  localSeatAnswer  = SEAT_LOCAL_WALKING;
	uint8_t  localSeatAsked   = 2;
	int32_t  localPollAnswer  = SEAT_LOCAL_WALKING;
	int      localSeatCalls   = 0;
	int      localPollCalls   = 0;
	int      localUnseats     = 0;
	int32_t  lastSeatHandle   = -1;

	// ---- rampages (game/darkel.h) --------------------------------------
	//
	// What the seam was asked to write into CDarkel, which is the whole of
	// what a rampage packet does on the receiving side.
	int      rampageRuleSets   = 0;
	uint8_t  rampageRule       = 0xFF;
	int      rampageOpens      = 0;
	uint16_t rampageKillsNeeded = 0;
	uint32_t rampageElapsedMs  = 0;
	int      rampageCredits    = 0;
	uint16_t rampageLastModel  = 0;
	uint8_t  rampageLastWeapon = 0;
	bool     rampageLastHead   = false;
	int      rampageVerdicts   = 0;
	uint8_t  rampageVerdict    = 0;
	int      rampageResets     = 0;
	int      rampageCarCredits = 0;
	uint16_t rampageCarModel   = 0;
	UnownedVehicleKey rampageCarKey{};

	// Breakable street objects. The seam that produces these lives in
	// game/object.cpp and needs a running engine; what Client owns, and what
	// these record, is the routing - which packet reaches the bridge and
	// which one is dropped because it is our own words coming back.
	int            objectBreaks   = 0;
	ObjectBreakBody lastObjectBreak{};
	int            objectRests    = 0;
	ObjectRestBody lastObjectRest{};
};

Recorder g_rec;

// ---- the detours' table, over a fake pool ----------------------------------
//
// game/observed.h is the real table game/vehicle.cpp keeps; only the pool is
// made up. A handle resolves to a pointer derived from it unless a test has
// reaped it, which is CPools::GetVehicle's behaviour in the only way the table
// can see it. The stubs below write to it at the same moments the engine seam
// does (SpawnRemoteVehicle, DespawnRemoteVehicle, AdoptPromotedCar,
// AdoptClaimedVehicle, ReleaseOwnVehicle, NoteVehicleHolders), so a test can
// ask what the two detours would conclude.
game::ObservedTable<64> g_observedHere;

void *FakeVehicleAt(int32_t handle) {
	if (handle < 0)
		return nullptr;
	for (int32_t h : g_rec.reapedVehicleHandles)
		if (h == handle)
			return nullptr;
	return reinterpret_cast<void *>(static_cast<uintptr_t>(handle) * 16u + 0x10000u);
}

// The one writer of who else holds a car, as game/vehicle.cpp has it.
void RecNoteVehicleHolders(uint16_t netId, uint8_t driverPlayerId,
                           uint8_t custodianPlayerId, bool weSettle,
                           bool blastFloorEnds) {
	g_observedHere.NoteHolders(netId, driverPlayerId, custodianPlayerId, weSettle,
	                           blastFloorEnds);
}

void RecRequestModel(uint16_t id) { g_rec.modelRequests.push_back(id); }
bool RecIsModelReady(uint16_t)    { return g_rec.modelReady; }

bool RecSampleLocalLook(char (&look)[PLAYER_LOOK_LEN]) {
	if (g_rec.localLook[0] == '\0')
		return false;
	std::memcpy(look, g_rec.localLook, sizeof look);
	return true;
}

bool RecPrepareLook(RemotePlayer &) {
	++g_rec.lookPrepares;
	return g_rec.lookReady;
}

bool RecSpawn(RemotePlayer &p) {
	p.poolHandle = g_rec.nextHandle++;
	++g_rec.spawns;
	return true;
}

void RecDespawn(RemotePlayer &p) {
	p.poolHandle = -1;
	++g_rec.despawns;
}

void RecApplyPose(RemotePlayer &p, const Pose &pose) {
	if (g_rec.loseNextPose) {
		g_rec.loseNextPose = false;
		p.poolHandle       = -1;
		p.spawnPending     = true;
		return;
	}
	++g_rec.poses;
	if (p.Seated())
		++g_rec.posesWhileSeated;
	g_rec.lastPose = pose;
}

bool RecSpawnVehicle(RemoteVehicle &v) {
	v.poolHandle = g_rec.nextVehicleHandle++;
	++g_rec.vehicleSpawns;
	// What the roster handed the spawn. The real one can only apply extras
	// while it is constructing the car, so if they are not on the entry by
	// the time this is called they are lost for that car's whole life.
	g_rec.lastSpawnExtra1 = v.extra1;
	g_rec.lastSpawnExtra2 = v.extra2;
	g_observedHere.Remember(v.poolHandle, v.netId, &FakeVehicleAt);
	return true;
}

void RecDespawnVehicle(RemoteVehicle &v) {
	g_observedHere.Forget(v.netId);
	v.poolHandle = -1;
	++g_rec.vehicleDespawns;
	g_rec.unseatsAtLastVehicleDespawn = g_rec.unseats;
}

void RecUpdateTrafficAllowance() {
	++g_rec.trafficAllowances;
	g_rec.vehicleSpawnsAtAllowance = g_rec.vehicleSpawns;
}

bool RecSeat(RemotePlayer &, RemoteVehicle &v, uint8_t seat) {
	++g_rec.seatAttempts;
	if (g_rec.refuseSeating)
		return false;
	++g_rec.seats;
	g_rec.lastSeatVehicle = v.netId;
	g_rec.lastSeatIndex   = seat;
	return true;
}

void RecUnseat(RemotePlayer &) { ++g_rec.unseats; }

bool RecSampleLocalCarEntry(LocalCarEntry &out) {
	++g_rec.localEntrySamples;
	if (!g_rec.localEntryActive)
		return false;
	out = g_rec.localEntry;
	return true;
}

bool RecBeginSeat(RemotePlayer &, RemoteVehicle &v, uint8_t seat, uint8_t doorSeat) {
	++g_rec.seatAnimBegins;
	g_rec.lastSeatDoor = doorSeat;
	if (!g_rec.animEntry)
		return false;
	g_rec.lastSeatVehicle = v.netId;
	g_rec.lastSeatIndex   = seat;
	return true;
}

uint8_t RecPollSeat(RemotePlayer &, RemoteVehicle &, uint8_t) {
	++g_rec.seatAnimPolls;
	return g_rec.animProgress;
}

void RecAbandonSeat(RemotePlayer &) {
	++g_rec.seatAnimAborts;
	g_rec.seatsAtLastAbort = g_rec.seats;
}

bool RecBeginUnseat(RemotePlayer &) {
	++g_rec.exitAnimBegins;
	return true;
}

bool RecBeginJack(RemotePlayer &, int32_t carHandle, uint8_t doorSeat) {
	++g_rec.jackBegins;
	g_rec.lastJackHandle = carHandle;
	g_rec.lastJackDoor   = doorSeat;
	return g_rec.jackStarts;
}

uint8_t RecPollJack(RemotePlayer &, int32_t) {
	++g_rec.jackPolls;
	return g_rec.jackProgress;
}

uint8_t RecRemotePull(RemotePlayer &p) {
	return p.playerId < MAX_PLAYERS ? g_rec.remotePull[p.playerId] : uint8_t{PULL_NONE};
}

uint8_t RecAmbientPull(const RemoteAmbientPed &) { return g_rec.ambientPull; }

uint8_t RecLocalPull() { return g_rec.localPull; }

bool RecBlowUpVehicle(RemoteVehicle &v, const Vec3 &pos, const Quat &) {
	++g_rec.blowUps;
	g_rec.lastBlowUpNetId = v.netId;
	g_rec.lastBlowUpPos   = pos;
	if (g_rec.blowUpFails)
		return false;
	return true;
}

UnownedWreckOutcome RecWreckUnowned(const UnownedVehicleKey &key) {
	++g_rec.unownedWrecks;
	g_rec.lastUnownedKey = key;
	return g_rec.unownedOutcome;
}

uint8_t RecDrainUnownedBlasts(UnownedBlast *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && !g_rec.unownedBlasts.empty()) {
		out[n++] = g_rec.unownedBlasts.front();
		g_rec.unownedBlasts.erase(g_rec.unownedBlasts.begin());
	}
	return n;
}

void RecApplyRemoteVehicleHit(RemoteVehicle &vehicle, RemotePlayer *attacker,
                              const VehicleHitBody &body, bool settling) {
	++g_rec.vehicleHits;
	g_rec.lastVehicleHitSettling = settling;
	g_rec.lastVehicleHit     = body;
	g_rec.lastVehicleHitRow  = vehicle.netId;
	g_rec.vehicleHitAttacker = attacker ? attacker->playerId : 0xFF;
}

void RecApplyNpcVehicleHit(RemoteVehicle &vehicle, RemoteAmbientPed *attacker,
                           const VehicleHitBody &, bool) {
	++g_rec.npcCarHits;
	g_rec.npcCarHitRow      = vehicle.netId;
	g_rec.npcCarHitAttacker = attacker ? attacker->netId : 0;
}

void RecApplyHostedCarHit(uint16_t, RemotePlayer *attacker, const VehicleHitBody &body) {
	++g_rec.carHits;
	g_rec.lastCarHit     = body;
	g_rec.carHitAttacker = attacker ? attacker->playerId : 0xFF;
}

uint8_t RecDrainLocalCarHits(VehicleHitBody *out, uint8_t max) {
	++g_rec.carHitDrains;
	uint8_t n = 0;
	while (n < max && !g_rec.localCarHits.empty()) {
		out[n++] = g_rec.localCarHits.front();
		g_rec.localCarHits.erase(g_rec.localCarHits.begin());
	}
	return n;
}

uint8_t RecDrainLocalVehicleHits(VehicleHitBody *out, uint8_t max) {
	++g_rec.vehicleHitDrains;
	uint8_t n = 0;
	while (n < max && !g_rec.localVehicleHits.empty()) {
		out[n++] = g_rec.localVehicleHits.front();
		g_rec.localVehicleHits.erase(g_rec.localVehicleHits.begin());
	}
	return n;
}

UnownedWreckOutcome RecWreckAmbientCar(RemoteAmbientCar &car, const BlastTransform &where) {
	++g_rec.ambientWrecks;
	g_rec.lastAmbientWreckNetId = car.netId;
	g_rec.lastAmbientWreckWhere = where;
	return g_rec.ambientWreckOutcome;
}

uint8_t RecDrainAmbientWrecks(UnownedBlast *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && !g_rec.ambientWreckQueue.empty()) {
		out[n++] = g_rec.ambientWreckQueue.front();
		g_rec.ambientWreckQueue.erase(g_rec.ambientWreckQueue.begin());
	}
	return n;
}

uint8_t RecDrainLocalBlasts(LocalVehicleBlast *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && !g_rec.localBlasts.empty()) {
		out[n++] = g_rec.localBlasts.front();
		g_rec.localBlasts.erase(g_rec.localBlasts.begin());
	}
	return n;
}

void RecReplayShot(RemotePlayer &p, const ShotBody &shot) {
	++g_rec.shotsReplayed;
	g_rec.lastShot       = shot;
	g_rec.lastShotPlayer = p.playerId;
}

void RecPlayExplosion(RemotePlayer &, const ExplosionBody &body) {
	++g_rec.explosions;
	g_rec.lastExplosion = body;
}

void RecApplyDamage(RemotePlayer *attacker, const DamageBody &body) {
	++g_rec.damages;
	g_rec.lastDamage     = body;
	g_rec.damageAttacker = attacker ? attacker->playerId : 0xFF;
}

void RecKill(RemotePlayer &p, uint16_t animId) {
	++g_rec.kills;
	g_rec.lastKilled   = p.playerId;
	g_rec.lastKillAnim = animId;
}

void RecSetFriendlyFire(bool on) {
	++g_rec.friendlyFireCalls;
	g_rec.friendlyFire = on;
}

void RecSetAmmoSync(bool on) {
	++g_rec.ammoSyncCalls;
	g_rec.ammoSync = on;
}

bool RecSampleLocalAmmo(AmmoSlotBody *out, uint8_t &held) {
	++g_rec.ammoSamples;
	if (!g_rec.haveLocalAmmo)
		return false;
	for (uint8_t w = 0; w < INVENTORY_SLOTS; ++w)
		out[w] = g_rec.localAmmo[w];
	held = g_rec.localHeld;
	return true;
}

void RecApplyRemoteAmmo(RemotePlayer &p, const AmmoSlotBody &slot) {
	++g_rec.ammoApplies;
	g_rec.lastAmmoSlot   = slot;
	g_rec.lastAmmoPlayer = p.playerId;
}

// ---- the wanted level -----------------------------------------------------
//
// A stand-in CWanted: one number CoopIII reads and sometimes writes. The
// write counter is what most of these tests actually check, because the thing
// that goes wrong quietly is CoopIII writing the level it just read - which
// in the real engine resets the player's accumulated chaos and stops them
// ever reaching the next star.

bool RecSampleWanted(uint8_t &level) {
	if (!g_rec.havePlayerWanted)
		return false;
	level = g_rec.engineWanted;
	return true;
}

void RecWriteWanted(uint8_t level) {
	++g_rec.wantedWrites;
	g_rec.lastWantedWrite = level;
	g_rec.engineWanted    = level;
}

uint8_t RecDrainLocalCombat(CombatEvent *out, uint8_t max) {
	++g_rec.drains;
	uint8_t n = 0;
	while (n < max && !g_rec.localCombat.empty()) {
		out[n++] = g_rec.localCombat.front();
		g_rec.localCombat.erase(g_rec.localCombat.begin());
	}
	return n;
}

void RecCorrectVehicle(RemoteVehicle &v, const VehicleTransform &at) {
	++g_rec.vehicleCorrections;
	g_rec.lastCorrection     = at;
	g_rec.lastCorrectionHorn = v.hornSounding;
}

bool RecSampleWorld(WorldState &out) {
	++g_rec.worldSamples;
	if (!g_rec.haveWorld)
		return false;
	out = g_rec.world;
	return true;
}

void RecApplyWorldTime(uint8_t hour, uint8_t minute) {
	++g_rec.timeApplies;
	g_rec.appliedHour   = hour;
	g_rec.appliedMinute = minute;
}

void RecSetSessionClock(bool valid, uint32_t offsetMs) {
	++g_rec.sessionClockCalls;
	g_rec.sessionClockValid  = valid;
	g_rec.sessionClockOffset = offsetMs;
}

void RecApplyWorldWeather(uint8_t weather, uint8_t weatherOld) {
	++g_rec.weatherApplies;
	g_rec.appliedWeather    = weather;
	g_rec.appliedWeatherOld = weatherOld;
}

void RecReleaseWorldWeather() { ++g_rec.weatherReleases; }

void RecRestVehicle(RemoteVehicle &) {
	++g_rec.vehicleAtRest;
}

// Custody of a car nobody is driving. The sampler stands in for reading a
// CVehicle off its roster row, and the rest test for asking the engine
// whether it has stopped - which is the only thing that ends a settle short
// of the timeout.
bool RecSampleObservedVehicle(RemoteVehicle &, VehicleStateBody &out) {
	++g_rec.observedSamples;
	if (!g_rec.observedSampleOk)
		return false;
	out           = VehicleStateBody{};
	out.pos       = {11.0f, 12.0f, 13.0f};
	out.rot       = {0.0f, 0.0f, 0.0f, 1.0f};
	out.moveSpeed = g_rec.sampledMoveSpeed;
	out.health    = 1000.0f;
	return true;
}

bool RecVehicleAtRest(RemoteVehicle &) {
	++g_rec.atRestCalls;
	return g_rec.carAtRest;
}

bool RecVehicleBurning(RemoteVehicle &) { return g_rec.carBurning; }

bool RecSampleObservedVehicleDamage(RemoteVehicle &, VehicleDamageBody &out) {
	++g_rec.observedDamageSamples;
	out = g_rec.observedDamage;
	return true;
}

bool RecSampleLocalVehicleDamage(VehicleDamageBody &out) {
	if (!g_rec.drivingLocally)
		return false;
	out = g_rec.localDamage;
	return true;
}

void RecAdoptPromotedCar(RemoteVehicle &v, bool weHostedIt) {
	++g_rec.promotedAdoptions;
	g_rec.lastPromotionWasOurs = weHostedIt;
	if (!g_observedHere.Find(FakeVehicleAt(v.poolHandle), &FakeVehicleAt))
		g_observedHere.Remember(v.poolHandle, v.netId, &FakeVehicleAt);
}

void RecAdoptClaimedVehicle(RemoteVehicle &v) {
	++g_rec.claimedAdoptions;
	void *const car = FakeVehicleAt(v.poolHandle);
	if (!car)
		return;
	if (game::ObservedRow *const o = g_observedHere.Find(car, &FakeVehicleAt)) {
		o->driverPlayerId    = 0xFF;
		o->custodianPlayerId = 0xFF;
		o->weSettle          = false;
	} else
		g_observedHere.Remember(v.poolHandle, v.netId, &FakeVehicleAt);
}

void RecReleaseOwnVehicle(RemoteVehicle &v) {
	++g_rec.ownReleases;
	g_observedHere.Forget(v.netId);
}

bool RecLocalDrivesAmbientCar(const RemoteAmbientCar &car) {
	return g_rec.ambientDrivenHandle >= 0 &&
	       g_rec.ambientDrivenHandle == car.poolHandle;
}

bool RecLocalDrivesVehicle(const RemoteVehicle &vehicle);

// The engine half of the handover. In the real thing this takes the local
// player out of the driver's seat; here it does what that makes true, which is
// that the engine stops saying we drive anything - so a test gets the same
// consequence the game does without an engine.
bool RecSurrenderVehicleSeat(RemoteVehicle &vehicle) {
	++g_rec.vehicleSurrenders;
	if (!RecLocalDrivesVehicle(vehicle))
		return false;
	g_rec.drivingLocally     = false;
	g_rec.localVehicleHandle = -1;
	return true;
}

void RecApplyVehicle(RemoteVehicle &, const VehicleStateBody &body) {
	++g_rec.vehicleApplies;
	g_rec.lastVehicleBody = body;
}

void RecApplyVehicleDamage(RemoteVehicle &, const VehicleDamageBody &body,
                           bool flying) {
	++g_rec.vehicleDamageApplies;
	g_rec.lastVehicleDamage = body;
	g_rec.lastDamageFlying  = flying;
}

bool RecSampleLocalVehicle(VehicleStateBody &out) {
	if (!g_rec.drivingLocally)
		return false;
	out           = VehicleStateBody{};
	out.moveSpeed = g_rec.sampledMoveSpeed;
	out.health    = 1000.0f;
	return true;
}

bool RecSampleLocalVehicleIdentity(VehicleIdentity &out) {
	if (!g_rec.drivingLocally)
		return false;
	out.modelId = g_rec.localModel;
	out.colour1 = 1;
	out.colour2 = 2;
	out.extra1  = g_rec.localExtra1;
	out.extra2  = g_rec.localExtra2;
	out.pos     = Vec3{5.0f, 6.0f, 7.0f};
	out.rot     = Quat{0.0f, 0.0f, 0.0f, 1.0f};
	return true;
}

int32_t RecSampleLocalVehicleHandle() {
	return g_rec.drivingLocally ? g_rec.localVehicleHandle : -1;
}

// What the engine would say: is the local player in this row's driver's seat?
// Derived from the same two fields the handle sampler uses, so a test that
// puts the player in a car gets a consistent answer from both.
bool RecLocalDrivesVehicle(const RemoteVehicle &vehicle) {
	return g_rec.drivingLocally && g_rec.localVehicleHandle >= 0 &&
	       g_rec.localVehicleHandle == vehicle.poolHandle;
}

bool RecPickupGranted(const PickupIdent &ident) {
	if (g_rec.grantFails > 0) {
		--g_rec.grantFails;
		return false;
	}
	++g_rec.grantsConsumed;
	g_rec.lastGranted = ident;
	return true;
}

void RecPickupTakenByOther(const PickupIdent &ident) {
	++g_rec.remoteTakes;
	g_rec.lastRemoteTake = ident;
}

void RecPickupDenied(const PickupIdent &) { ++g_rec.denials; }
void RecPickupsReset()                    { ++g_rec.pickupResets; }

void RecPickupDropped(const PickupDropBody &drop) {
	++g_rec.drops;
	g_rec.lastDrop = drop;
}

bool RecSpawnAmbientPed(RemoteAmbientPed &p) {
	p.poolHandle = g_rec.nextAmbientPedHandle++;
	++g_rec.ambientPedSpawns;
	return true;
}

void RecDespawnAmbientPed(RemoteAmbientPed &p) {
	p.poolHandle = -1;
	++g_rec.ambientPedDespawns;
}

// The same two writes the real one makes in game/population.cpp, so what this
// pins is the client loop's half of the recovery: that the check is asked at
// all, that it is asked before the seat pass and the pose pass, and that a
// false re-arms the spawn without anything else having to know.
bool RecAmbientReplicaIsAlive(RemoteAmbientPed &p) {
	++g_rec.ambientLivenessChecks;
	if (!g_rec.ambientReplicaTaken) {
		// The third condition, and the real one: a replica our own engine put
		// in the ground with nothing in the session having asked. The real
		// function destroys the body before it re-arms, because a corpse
		// CanBeDeleted refuses would lie there for good, so the stub counts a
		// despawn where the taken-by-the-engine arm below does not.
		if (!game::ReplicaDiedUnasked(g_rec.ambientReplicaState, p.dead))
			return true;
		++g_rec.ambientDeathsRecovered;
		++g_rec.ambientPedDespawns;
	}
	p.poolHandle           = -1;
	p.spawnPending         = true;
	p.seatedVehicleNetId   = INVALID_NETID;
	return false;
}

void RecApplyAmbientPed(RemoteAmbientPed &, const Pose &at) {
	++g_rec.ambientApplies;
	g_rec.lastAmbientPose = at;
}

void RecApplyAmbientPedFire(RemoteAmbientPed &ped) {
	++g_rec.ambientFires;
	g_rec.lastAmbientFireSaid = ped.fireSaid;
}

bool RecSpawnAmbientCar(RemoteAmbientCar &c) {
	c.poolHandle = g_rec.nextAmbientCarHandle++;
	++g_rec.ambientCarSpawns;
	return true;
}

void RecApplyAmbientCarDamage(RemoteAmbientCar &, const VehicleDamageBody &body, bool flying) {
	++g_rec.ambientCarDents;
	g_rec.lastAmbientCarDent       = body;
	g_rec.lastAmbientCarDentFlying = flying;
}

uint32_t RecDrainHostedCarDamage(VehicleDamageBody *out, uint32_t max) {
	const uint32_t n = g_rec.hostedDentCount < max ? g_rec.hostedDentCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_rec.hostedDents[i];
	g_rec.hostedDentCount = 0;
	return n;
}

void RecDespawnAmbientCar(RemoteAmbientCar &c) {
	c.poolHandle = -1;
	++g_rec.ambientCarDespawns;
	g_rec.unseatsAtLastAmbientCarDespawn = g_rec.ambientUnseats;
}

void RecCorrectAmbientCar(RemoteAmbientCar &c, const VehicleTransform &at) {
	g_rec.lastAmbientHornTimer = c.hornTimer;
	g_rec.lastAmbientSirenOn    = c.sirenOn;
	g_rec.lastAmbientDriverSaid = c.driverSaid;
	g_rec.lastAmbientCarAt     = at;
	++g_rec.ambientCorrections;
}

bool RecSeatAmbientPed(RemoteAmbientPed &, RemoteAmbientCar &car, uint8_t seat) {
	++g_rec.ambientSeatAttempts;
	if (g_rec.refuseAmbientSeating)
		return false;
	++g_rec.ambientSeats;
	g_rec.lastAmbientSeatCar = car.netId;
	g_rec.lastAmbientSeatIdx = seat;
	return true;
}

void RecUnseatAmbientPed(RemoteAmbientPed &) { ++g_rec.ambientUnseats; }

// The same write the real seam makes on success: the handle is cleared, so the
// roster can drop the row without the object being destroyed.
bool RecAdoptAmbientPed(RemoteAmbientPed &p) {
	if (g_rec.refuseAdoption)
		return false;
	++g_rec.pedAdoptions;
	g_rec.lastPedAdoptedAt = ++g_rec.adoptionStamp;
	p.poolHandle = -1;
	return true;
}

bool RecAdoptAmbientCar(RemoteAmbientCar &c) {
	if (g_rec.refuseAdoption)
		return false;
	++g_rec.carAdoptions;
	g_rec.lastCarAdoptedAt = ++g_rec.adoptionStamp;
	c.poolHandle = -1;
	return true;
}

bool RecKillAmbientReplica(RemoteAmbientPed &p, uint16_t animId) {
	++g_rec.ambientKillAttempts;
	if (g_rec.refuseAmbientKill)
		return false;
	++g_rec.ambientKills;
	g_rec.lastKilledPed = p.netId;
	g_rec.lastAmbientKillAnim = animId;
	g_rec.unseatsAtLastAmbientKill = g_rec.ambientUnseats;
	return true;
}

void RecApplyRemotePedDamage(RemotePlayer *attacker, const PedDamageBody &body) {
	++g_rec.pedDamages;
	g_rec.lastPedDamage     = body;
	g_rec.pedDamageAttacker = attacker ? attacker->playerId : 0xFF;
}

bool RecArmAmbientReplica(RemoteAmbientPed &, uint8_t weapon) {
	++g_rec.replicaArms;
	g_rec.lastReplicaArm = weapon;
	return g_rec.replicaArmReady;
}

void RecReplayAmbientShot(RemoteAmbientPed &p, const ShotBody &shot) {
	++g_rec.npcShots;
	g_rec.lastNpcShotPed = p.netId;
	g_rec.lastNpcShot    = shot;
}

void RecApplyNpcDamage(RemoteAmbientPed *attacker, const DamageBody &body) {
	++g_rec.npcDamages;
	g_rec.npcDamageAttacker = attacker ? attacker->netId : 0;
	g_rec.lastNpcDamage     = body;
}

bool RecRemovePlayerBodyPart(RemotePlayer &p, uint8_t node, int8_t) {
	++g_rec.playerLimbs;
	g_rec.lastPlayerLimbOf   = p.playerId;
	g_rec.lastPlayerLimbNode = node;
	return true;
}

bool RecRemoveAmbientBodyPart(RemoteAmbientPed &p, uint8_t node, int8_t direction) {
	++g_rec.limbs;
	g_rec.lastLimbPed       = p.netId;
	g_rec.lastLimbNode      = node;
	g_rec.lastLimbDirection = direction;
	return true;
}

// ---- garages ---------------------------------------------------------------

bool RecSampleLocalGarages(uint32_t &mask) {
	++g_rec.garageSamples;
	if (!g_rec.haveGarages)
		return false;
	mask = g_rec.localGarageMask;
	return true;
}

void RecApplyRemoteGarages(uint32_t mask) {
	++g_rec.garageApplies;
	g_rec.appliedGarageMask = mask;
}

uint8_t RecDrainLocalResprays(LocalRespray *out, uint8_t max) {
	uint8_t n = 0;
	for (const LocalRespray &r : g_rec.localResprays) {
		if (n >= max)
			break;
		out[n++] = r;
	}
	g_rec.localResprays.clear();
	return n;
}

void RecApplyRemoteRespray(RemoteVehicle *vehicle, const ResprayBody &body) {
	++g_rec.resprayApplies;
	g_rec.lastResprayHadCar = vehicle != nullptr;
	g_rec.lastRespray       = body;
}

// The local player, as the seat key's range test reads them. At the origin,
// so any car the vehicle helpers put down is within SEAT_RANGE_M.
bool RecSampleLocalPlayer(PlayerStateBody &out) {
	out        = PlayerStateBody{};
	out.pos    = {10.0f, 20.0f, 30.0f};
	out.health = 1000.0f;
	return true;
}

bool RecLocalWantsSeatToggle() {
	const bool want     = g_rec.wantSeatToggle;
	g_rec.wantSeatToggle = false;   // an edge, the same as the real one
	return want;
}

bool RecLocalIsPassenger() { return g_rec.localIsPassenger; }

int32_t RecSeatLocalPlayerIn(int32_t handle, uint8_t *seatAsked) {
	++g_rec.localSeatCalls;
	g_rec.lastSeatHandle = handle;
	if (g_rec.localSeatAnswer == SEAT_LOCAL_WALKING && seatAsked)
		*seatAsked = g_rec.localSeatAsked;
	return g_rec.localSeatAnswer;
}

int32_t RecPollLocalSeatEntry() {
	++g_rec.localPollCalls;
	return g_rec.localPollAnswer;
}

bool RecUnseatLocalPlayer() {
	++g_rec.localUnseats;
	g_rec.localIsPassenger = false;
	return true;
}

void RecSetRampageRule(uint8_t rule) {
	++g_rec.rampageRuleSets;
	g_rec.rampageRule = rule;
}

void RecApplyRampageOpen(uint16_t killsNeeded, uint32_t elapsedMs) {
	++g_rec.rampageOpens;
	g_rec.rampageKillsNeeded = killsNeeded;
	g_rec.rampageElapsedMs   = elapsedMs;
}

void RecCreditRampageKill(uint16_t model, uint8_t weapon, bool headshot) {
	++g_rec.rampageCredits;
	g_rec.rampageLastModel  = model;
	g_rec.rampageLastWeapon = weapon;
	g_rec.rampageLastHead   = headshot;
}

void RecApplyRampageVerdict(uint8_t outcome) {
	++g_rec.rampageVerdicts;
	g_rec.rampageVerdict = outcome;
}

void RecResetRampage() { ++g_rec.rampageResets; }

void RecCreditRampageCar(uint16_t model, const UnownedVehicleKey &key) {
	++g_rec.rampageCarCredits;
	g_rec.rampageCarModel = model;
	g_rec.rampageCarKey   = key;
}

void RecObjectBroken(const ObjectBreakBody &body) {
	++g_rec.objectBreaks;
	g_rec.lastObjectBreak = body;
}

void RecObjectSettled(const ObjectRestBody &body) {
	++g_rec.objectRests;
	g_rec.lastObjectRest = body;
}

WorldBridge RecordingBridge() {
	g_rec = Recorder{};
	g_observedHere.Clear();
	WorldBridge b;
	b.SetRampageRule      = &RecSetRampageRule;
	b.ApplyRampageOpen    = &RecApplyRampageOpen;
	b.CreditRampageKill   = &RecCreditRampageKill;
	b.CreditRampageCar    = &RecCreditRampageCar;
	b.ApplyRampageVerdict = &RecApplyRampageVerdict;
	b.ResetRampage        = &RecResetRampage;
	b.ObjectBroken        = &RecObjectBroken;
	b.ObjectSettled       = &RecObjectSettled;
	b.SampleLocalPlayer     = &RecSampleLocalPlayer;
	b.LocalWantsSeatToggle  = &RecLocalWantsSeatToggle;
	b.LocalIsPassenger      = &RecLocalIsPassenger;
	b.SeatLocalPlayerIn     = &RecSeatLocalPlayerIn;
	b.PollLocalSeatEntry    = &RecPollLocalSeatEntry;
	b.UnseatLocalPlayer     = &RecUnseatLocalPlayer;
	b.RequestModel    = &RecRequestModel;
	b.IsModelReady    = &RecIsModelReady;
	b.SpawnRemote     = &RecSpawn;
	b.DespawnRemote   = &RecDespawn;
	b.ApplyRemotePose = &RecApplyPose;

	b.SampleLocalVehicle         = &RecSampleLocalVehicle;
	b.SampleLocalVehicleIdentity = &RecSampleLocalVehicleIdentity;
	b.SampleLocalVehicleHandle   = &RecSampleLocalVehicleHandle;
	b.SpawnRemoteVehicle         = &RecSpawnVehicle;
	b.DespawnRemoteVehicle       = &RecDespawnVehicle;
	b.AdoptClaimedVehicle        = &RecAdoptClaimedVehicle;
	b.ReleaseOwnVehicle          = &RecReleaseOwnVehicle;
	b.ApplyRemoteVehicle         = &RecApplyVehicle;
	b.RestRemoteVehicle          = &RecRestVehicle;
	b.ApplyRemoteVehicleDamage   = &RecApplyVehicleDamage;
	b.CorrectRemoteVehicle       = &RecCorrectVehicle;
	b.SampleObservedVehicle      = &RecSampleObservedVehicle;
	b.VehicleAtRest              = &RecVehicleAtRest;
	b.VehicleBurning             = &RecVehicleBurning;
	b.SampleObservedVehicleDamage = &RecSampleObservedVehicleDamage;
	b.SampleLocalVehicleDamage    = &RecSampleLocalVehicleDamage;
	b.SeatRemotePed              = &RecSeat;
	b.UnseatRemotePed            = &RecUnseat;
	b.BeginSeatRemotePed         = &RecBeginSeat;
	b.PollSeatRemotePed          = &RecPollSeat;
	b.AbandonSeatRemotePed       = &RecAbandonSeat;
	b.BeginUnseatRemotePed       = &RecBeginUnseat;
	b.SampleLocalCarEntry        = &RecSampleLocalCarEntry;
	b.BeginJackRemotePed         = &RecBeginJack;
	b.PollJackRemotePed          = &RecPollJack;
	b.RemoteBeingPulledOut       = &RecRemotePull;
	b.AmbientBeingPulledOut      = &RecAmbientPull;
	b.LocalBeingPulledOut        = &RecLocalPull;
	b.BlowUpRemoteVehicle        = &RecBlowUpVehicle;
	b.DrainLocalVehicleBlasts    = &RecDrainLocalBlasts;
	b.WreckUnownedVehicle        = &RecWreckUnowned;
	b.DrainUnownedBlasts         = &RecDrainUnownedBlasts;
	b.DrainLocalVehicleHits      = &RecDrainLocalVehicleHits;
	b.ApplyRemoteVehicleHit      = &RecApplyRemoteVehicleHit;
	b.ApplyNpcVehicleHit         = &RecApplyNpcVehicleHit;
	b.NoteVehicleHolders         = &RecNoteVehicleHolders;
	b.UpdateTrafficAllowance     = &RecUpdateTrafficAllowance;
	b.DrainLocalCarHits         = &RecDrainLocalCarHits;
	b.ApplyHostedCarHit          = &RecApplyHostedCarHit;

	b.DrainLocalCombat    = &RecDrainLocalCombat;
	b.ReplayRemoteShot    = &RecReplayShot;
	b.PlayRemoteExplosion = &RecPlayExplosion;
	b.ApplyRemoteDamage   = &RecApplyDamage;
	b.KillRemotePed       = &RecKill;
	b.SetFriendlyFire     = &RecSetFriendlyFire;
	b.SetAmmoSync         = &RecSetAmmoSync;
	b.SampleLocalAmmo     = &RecSampleLocalAmmo;
	b.ApplyRemoteAmmo     = &RecApplyRemoteAmmo;

	b.SampleLocalWantedLevel = &RecSampleWanted;
	b.WriteLocalWantedLevel  = &RecWriteWanted;

	b.LocalDrivesVehicle   = &RecLocalDrivesVehicle;
	b.SurrenderVehicleSeat = &RecSurrenderVehicleSeat;

	b.SampleWorld         = &RecSampleWorld;
	b.ApplyWorldTime      = &RecApplyWorldTime;
	b.ApplyWorldWeather   = &RecApplyWorldWeather;
	b.ReleaseWorldWeather = &RecReleaseWorldWeather;
	b.SetSessionClock     = &RecSetSessionClock;

	b.SpawnAmbientReplica     = &RecSpawnAmbientPed;
	b.DespawnAmbientReplica   = &RecDespawnAmbientPed;
	b.AmbientReplicaIsAlive   = &RecAmbientReplicaIsAlive;
	b.ApplyAmbientPedState    = &RecApplyAmbientPed;
	b.ApplyAmbientPedFire     = &RecApplyAmbientPedFire;
	b.SeatAmbientPed          = &RecSeatAmbientPed;
	b.UnseatAmbientPed        = &RecUnseatAmbientPed;
	b.RemoveAmbientBodyPart   = &RecRemoveAmbientBodyPart;
	b.RemovePlayerBodyPart    = &RecRemovePlayerBodyPart;
	b.KillAmbientReplica      = &RecKillAmbientReplica;
	b.ApplyRemotePedDamage    = &RecApplyRemotePedDamage;
	b.ArmAmbientReplica       = &RecArmAmbientReplica;
	b.ReplayAmbientShot       = &RecReplayAmbientShot;
	b.ApplyNpcDamage          = &RecApplyNpcDamage;
	b.SpawnAmbientCarReplica  = &RecSpawnAmbientCar;
	b.ApplyAmbientCarDamage   = &RecApplyAmbientCarDamage;
	b.DrainHostedCarDamage    = &RecDrainHostedCarDamage;
	b.DespawnAmbientCarReplica = &RecDespawnAmbientCar;
	b.CorrectAmbientCarReplica = &RecCorrectAmbientCar;
	b.LocalDrivesAmbientCar    = &RecLocalDrivesAmbientCar;
	b.AdoptPromotedCar         = &RecAdoptPromotedCar;
	b.AdoptAmbientPed          = &RecAdoptAmbientPed;
	b.AdoptAmbientCar          = &RecAdoptAmbientCar;
	b.WreckAmbientCarReplica   = &RecWreckAmbientCar;
	b.DrainAmbientWrecks       = &RecDrainAmbientWrecks;

	b.PickupGrantedToUs  = &RecPickupGranted;
	b.PickupTakenByOther = &RecPickupTakenByOther;
	b.PickupDenied       = &RecPickupDenied;
	b.PickupsReset       = &RecPickupsReset;
	b.PickupDropped      = &RecPickupDropped;

	b.SampleLocalGarages = &RecSampleLocalGarages;
	b.ApplyRemoteGarages = &RecApplyRemoteGarages;
	b.DrainLocalResprays = &RecDrainLocalResprays;
	b.ApplyRemoteRespray = &RecApplyRemoteRespray;
	return b;
}

// ---- tests ----------------------------------------------------------------

// Feeds enough snapshots for InterpBuffer::SampleDelayed to produce a pose.
//
// A remote ped doesn't get created until we know where to put it. The join
// is reliable and arrives first; the first snapshot is unreliable and
// arrives later; a ped created in that gap is born at the world origin,
// which in GTA III happens to be water. Measured in the live game: born
// PED_IDLE with 100 health, PED_DIE eight frames later, and from then on the
// renderer accepted it and drew nothing. So every test that expects a spawn
// has to say where first.
void FeedPosition(Client &c, uint8_t playerId) {
	for (uint32_t i = 0; i <= 8; ++i)
		c.HandleMessage(Wrap(MakeState(playerId, 1000 + i * 40, float(i)), CH_SNAPSHOT));
}

// ---- vehicles --------------------------------------------------------------

S_VehicleSpawn MakeVehicleSpawn(uint16_t netId, uint16_t modelId = 90,
                                float x = 10.0f) {
	S_VehicleSpawn s;
	InitHeader(s, 1000);
	s.netId   = netId;
	s.modelId = modelId;
	s.pos     = {x, 20.0f, 30.0f};
	s.rot     = {0.0f, 0.0f, 0.0f, 1.0f};
	s.colour1 = 3;
	s.colour2 = 7;
	// Condition, as of protocol 9. A healthy car by default so every test
	// that predates it keeps meaning what it meant; the ones that care say so.
	s.health  = 1000.0f;
	s.flags   = 0;
	s.extra1  = 2;
	s.extra2  = -1;
	return s;
}

S_VehicleState MakeVehicleState(uint8_t playerId, uint16_t netId, float x) {
	S_VehicleState s;
	InitHeader(s, 1000);
	s.playerId    = playerId;
	s.body        = VehicleStateBody{};
	s.body.netId  = netId;
	s.body.pos    = {x, 0.0f, 0.0f};
	s.body.rot    = {0.0f, 0.0f, 0.0f, 1.0f};
	s.body.health = 1000.0f;
	return s;
}

// A stand-in for the socket, so the claim handshake can be checked without a
// server. Client::Start is never called, so m_net isn't connected and Send
// is a no-op - which is exactly what makes this a test of the *decision* to
// send rather than of the transport.
//
// The decision is the part worth pinning down. Claiming is driven by a local
// condition (we're driving something the session hasn't named) and answered
// asynchronously. Get it wrong and the failure mode is a claim sent 25 times
// a second until the reply lands, registering the same car over and over.
void TestVehicleClaimIsSentOnce() {
	std::printf("\nclaiming a car we just got into\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	g_rec.drivingLocally = true;

	// PostFrame needs a live socket, so drive the same path the frame pump
	// would - through the public seam - and check the observable state
	// instead: after the first claim the client is waiting, not re-claiming.
	Check(c.LocalVehicleNetId() == INVALID_NETID, "no netId before the server answers");

	// The server answers, addressed to us.
	S_EnterVehicle enter;
	InitHeader(enter, 1000);
	enter.playerId     = 0;   // our own id, from MakeWelcome(0)
	enter.body         = EnterVehicleBody{};
	enter.body.netId   = 77;
	enter.body.seat    = 0;
	enter.body.modelId = 90;
	c.HandleMessage(Wrap(enter, CH_EVENT));
	Check(c.LocalVehicleNetId() == 77, "the reply is what tells us our netId");

	// Getting out clears it, so the next car is claimed afresh rather than
	// reported under the old car's identity.
	S_ExitVehicle exit;
	InitHeader(exit, 2000);
	exit.playerId = 0;
	exit.netId    = 77;
	c.HandleMessage(Wrap(exit, CH_EVENT));
	Check(c.LocalVehicleNetId() == INVALID_NETID, "getting out clears it");
}
// A claim the session says no to. Before 19 the server answered a claim it
// could not grant by saying nothing at all, and the client - which sends the
// claim once and then waits - sat at the wheel of a car the session thought
// belonged to nobody for the rest of the session.
void TestARefusedClaimIsNotAName() {
	std::printf("\na claim the session refuses\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	g_rec.drivingLocally = true;

	// Our own id, and INVALID_NETID where a grant would name a car. A client
	// that read this as a grant would believe its car was called 0xFFFF and
	// would stream snapshots for a netId nobody has.
	S_EnterVehicle no;
	InitHeader(no, 1000);
	no.playerId   = 0;
	no.body       = EnterVehicleBody{};
	no.body.netId = INVALID_NETID;
	c.HandleMessage(Wrap(no, CH_EVENT));
	Check(c.LocalVehicleNetId() == INVALID_NETID, "a refusal leaves us unnamed");

	// And it is a pause, not a verdict: the next answer is a real one and is
	// taken, which is what the retry exists to reach.
	S_EnterVehicle yes;
	InitHeader(yes, 2000);
	yes.playerId     = 0;
	yes.body         = EnterVehicleBody{};
	yes.body.netId   = 42;
	yes.body.seat    = 0;
	yes.body.modelId = 90;
	c.HandleMessage(Wrap(yes, CH_EVENT));
	Check(c.LocalVehicleNetId() == 42, "a later yes is still taken");
}

// ---- garages, doors and the Pay'n'Spray -----------------------------------

S_GarageState MakeGarageState(uint8_t playerId, uint32_t mask) {
	S_GarageState g{};
	InitHeader(g, 1000);
	g.playerId       = playerId;
	g.body.deviating = mask;
	return g;
}

// The whole authority decision in one test: a door is open if *anybody* says
// so, and it stays open while any one of them still does.
//
// The alternative rules both have a name in this repo and both are wrong
// here. Last-writer-wins would have the first player to walk away shut the
// door on the second. roadmap.md 5.8's first-report-wins is right for a car
// being destroyed, which happens once - a door is a level and it has to be
// able to go back.
void TestGarageMaskIsAUnion() {
	std::printf("\ngarages: the union, not the newest report\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(2, "bob"), CH_EVENT));

	c.Tick();
	Check(g_rec.appliedGarageMask == 0, "nothing held when nobody says anything");

	c.HandleMessage(Wrap(MakeGarageState(1, 1u << 3), CH_EVENT));
	c.Tick();
	Check(g_rec.appliedGarageMask == (1u << 3), "one player holds garage 3");

	c.HandleMessage(Wrap(MakeGarageState(2, 1u << 5), CH_EVENT));
	c.Tick();
	Check(g_rec.appliedGarageMask == ((1u << 3) | (1u << 5)),
	      "two players hold two garages, and both are held");

	// Alice and Bob are in the same safehouse. Alice leaves; the door must
	// not shut on Bob.
	c.HandleMessage(Wrap(MakeGarageState(1, 1u << 5), CH_EVENT));
	c.Tick();
	Check(g_rec.appliedGarageMask == (1u << 5),
	      "alice's garage closes, bob's stays open");
	c.HandleMessage(Wrap(MakeGarageState(1, 0), CH_EVENT));
	c.Tick();
	Check(g_rec.appliedGarageMask == (1u << 5),
	      "alice walking away does not shut the door bob is standing in");
	c.HandleMessage(Wrap(MakeGarageState(2, 0), CH_EVENT));
	c.Tick();
	Check(g_rec.appliedGarageMask == 0, "the last one out shuts it");
}

// A player who quits while standing in their safehouse would otherwise hold
// that door open on every other machine for the rest of the session: the mask
// is a level and nothing else would ever contradict it.
void TestLeavingReleasesGarages() {
	std::printf("\ngarages: a player who leaves lets go of their doors\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeGarageState(1, 1u << 7), CH_EVENT));
	c.Tick();
	Check(g_rec.appliedGarageMask == (1u << 7), "held while she is here");

	S_PlayerLeave leave;
	InitHeader(leave, 2000);
	leave.playerId = 1;
	leave.reason   = LEAVE_QUIT;
	c.HandleMessage(Wrap(leave, CH_EVENT));
	c.Tick();
	Check(g_rec.appliedGarageMask == 0, "and released when she quits");
}

// Our own mask must never be fed back into the union we hold our own doors
// to. Nothing relays a client its own packet today; the point is that nothing
// has to promise not to, because two machines each holding a door on the
// other's say-so is a door neither of them can release.
// A knocked-over lamp post, from the client's side of the seam.
//
// game/object.cpp is what decides an object broke and who is entitled to say
// so, and none of that can run without a game. What Client owns is the
// routing, and the routing has one rule in it worth pinning: a report that
// comes back stamped with our own id is dropped. It matters more for the
// resting place than for the break - replaying our own break would push a
// change-then-smash object one step further than our own engine took it, and
// applying our own resting place would write a matrix we are in the middle of
// producing.
void TestAKnockedOverPostArrivesAndOurOwnDoesNot() {
	std::printf("\nstreet objects: which reports reach the engine seam\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(3), CH_EVENT));

	S_ObjectBroken broke;
	InitHeader(broke, 1000);
	broke.playerId          = 1;
	broke.body.ident.pos    = {100.0f, -200.0f, 15.0f};
	broke.body.ident.modelIndex = 1393;
	broke.body.amount       = 480.0f;
	broke.body.state        = OBJ_BREAK_RENDER_DAMAGED | OBJ_BREAK_UPROOTED;
	c.HandleMessage(Wrap(broke, CH_EVENT));
	Check(g_rec.objectBreaks == 1, "somebody else's break reaches the seam");
	Check((g_rec.lastObjectBreak.state & OBJ_BREAK_UPROOTED) != 0,
	      "carrying the bit that says the post also came loose");

	S_ObjectSettled rest;
	InitHeader(rest, 1010);
	rest.playerId           = 1;
	rest.body.ident.pos     = {100.0f, -200.0f, 15.0f};
	rest.body.ident.modelIndex = 1393;
	rest.body.right         = {1.0f, 0.0f, 0.0f};
	rest.body.forward       = {0.0f, 0.0f, -1.0f};
	rest.body.up            = {0.0f, 1.0f, 0.0f};
	rest.body.pos           = {101.2f, -200.4f, 13.9f};
	c.HandleMessage(Wrap(rest, CH_EVENT));
	Check(g_rec.objectRests == 1, "and so does where it came to rest");
	Check(g_rec.lastObjectRest.pos.x == 101.2f &&
	          g_rec.lastObjectRest.forward.z == -1.0f,
	      "unchanged - the matrix is the reporter's, not something we derive");

	// The ident is still the placement, three metres from where the object is
	// now lying. That is the whole reason the key could not be the entity's
	// own position: by the time this packet exists, the object has moved, and
	// on two machines it has moved in two different directions.
	Check(g_rec.lastObjectRest.ident.pos.x == 100.0f &&
	          g_rec.lastObjectRest.ident.pos.z == 15.0f,
	      "and it is named by where the map put it, not by where it ended up");

	S_ObjectBroken ours = broke;
	ours.playerId = 3;
	c.HandleMessage(Wrap(ours, CH_EVENT));
	Check(g_rec.objectBreaks == 1, "our own break does not come back to us");

	S_ObjectSettled oursRest = rest;
	oursRest.playerId = 3;
	c.HandleMessage(Wrap(oursRest, CH_EVENT));
	Check(g_rec.objectRests == 1, "nor our own resting place");
}

// Wired is not connected, the distinction pickup.h paid for once already. A
// client that has a bridge but no socket must not claim a report went out.
void TestNoSocketMeansNoObjectReportWentOut() {
	std::printf("\nstreet objects: a report with nowhere to go\n");
	Client c;
	c.SetBridge(RecordingBridge());

	ObjectBreakBody body{};
	body.ident.modelIndex = 1393;
	Check(!c.ReportObjectBroken(body),
	      "a break with no session says so rather than claiming it was shared");

	ObjectRestBody rest{};
	rest.ident.modelIndex = 1393;
	Check(!c.ReportObjectSettled(rest),
	      "and so does a resting place - the seam counts the two apart");
}

void TestOurOwnGarageMaskIsNotHeldAgainstUs() {
	std::printf("\ngarages: our own mask is not somebody else's\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(3), CH_EVENT));
	c.HandleMessage(Wrap(MakeGarageState(3, 0xFFFFFFFFu), CH_EVENT));
	c.Tick();
	Check(g_rec.appliedGarageMask == 0, "a packet addressed with our own id is ignored");
	Check(c.RemoteGarageMask() == 0, "and the union stays empty");
}

// The frontend and a loading screen. CGarages::Update is not called there, so
// the seam says "no world" and the client must not invent a mask from it.
void TestNoGarageReportWithoutAWorld() {
	std::printf("\ngarages: nothing to report with no world\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.haveGarages   = false;
	g_rec.garageSamples = 0;
	g_rec.garageApplies = 0;   // the welcome's own roster clear already pushed one
	c.Tick();
	Check(g_rec.garageSamples == 1, "the seam is still asked");
	// The union still gets pushed - a machine with no world of its own still
	// has to be told what everybody else's doors are doing, so that the first
	// frame after the loading screen is already right.
	Check(g_rec.garageApplies == 1, "and the union is still pushed");
}

// A respray is repair plus repaint plus a wanted level, and only the first two
// are this feature's business. What is checked here is the routing: the packet
// finds the right car, and one addressed to us is not replayed on us.
void TestResprayFindsTheCar() {
	std::printf("\ngarages: a respray lands on the car it names\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.Tick();

	S_Respray r{};
	InitHeader(r, 1000);
	r.playerId           = 1;
	r.body.vehicleNetId  = 80;
	r.body.garage        = 9;
	r.body.colour1       = 41;
	r.body.colour2       = 7;
	c.HandleMessage(Wrap(r, CH_EVENT));
	Check(g_rec.resprayApplies == 1, "the seam was asked once");
	Check(g_rec.lastResprayHadCar, "and it was handed the car");
	Check(g_rec.lastRespray.colour1 == 41 && g_rec.lastRespray.colour2 == 7,
	      "with the colours the owner's engine chose, not colours of its own");
}

// Somebody drove an unclaimed traffic car into a spray shop. There is nothing
// on this machine to repaint, and the seam still gets called: the decision
// about what "no car" means belongs on the engine side with the rest of it,
// and a seam that is only called when there is something to do is a seam
// nobody can log.
void TestResprayForACarWeDoNotHave() {
	std::printf("\ngarages: a respray for a car the session has no row for\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	S_Respray r{};
	InitHeader(r, 1000);
	r.playerId          = 1;
	r.body.vehicleNetId = INVALID_NETID;
	r.body.garage       = 9;
	c.HandleMessage(Wrap(r, CH_EVENT));
	Check(g_rec.resprayApplies == 1, "the seam is still asked");
	Check(!g_rec.lastResprayHadCar, "and told there is no car");

	// And our own respray is never replayed on us: our engine did it.
	S_Respray mine{};
	InitHeader(mine, 2000);
	mine.playerId          = 0;
	mine.body.vehicleNetId = INVALID_NETID;
	c.HandleMessage(Wrap(mine, CH_EVENT));
	Check(g_rec.resprayApplies == 1, "our own respray is not replayed on us");
}

// Disconnecting has to hand every door back to the local engine, which is
// single player behaving exactly as it always did. Clearing the roster is not
// enough on its own - the seam holds the union, and a garage left held open
// would stay held forever, because the detour only lets go when the union
// says nobody wants it.
void TestDisconnectReleasesEveryDoor() {
	std::printf("\ngarages: a disconnect hands every door back\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeGarageState(1, 0x0000000Fu), CH_EVENT));
	c.Tick();
	Check(g_rec.appliedGarageMask == 0x0000000Fu, "four doors held");

	// A welcome for a new session is what ClearRoster runs on.
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	Check(g_rec.appliedGarageMask == 0,
	      "the seam was told to let go, without waiting for a frame");
}

// The three decisions the engine seam makes, with no engine involved: what to
// do to a garage somebody else is holding, what state a latch writes, and
// whether the engine's own arm may run.
void TestGarageHoldDecisions() {
	std::printf("\ngarages: holding one open, and holding one shut\n");
	using namespace coopiii::game;

	// Nobody is asking: never touch anything.
	Check(DecideGarageHold(GARAGE_HIDEOUT_ONE, GS_FULLYCLOSED, false, true) ==
	          GarageHold::None,
	      "an unheld garage is left entirely alone");

	// A safehouse garage somebody else is standing in. The local arm decides
	// to close it every single frame, because the local player is miles away;
	// the door has not moved yet, so the answer is a silent latch and not a
	// trip back through GS_OPENING. That distinction is the whole reason
	// GarageHold has three values: re-running the arrival would play the
	// garage-door sound sixty times a second.
	Check(DecideGarageHold(GARAGE_HIDEOUT_ONE, GS_CLOSING, true, true) ==
	          GarageHold::Latch,
	      "a door still at the top is latched open, silently");
	Check(GarageLatchState(GARAGE_HIDEOUT_ONE) == GS_OPENED,
	      "and latched to GS_OPENED, not GS_OPENEDCONTAINSCAR");

	// Arrived mid-close, or a hold that turned up late: the door really is
	// somewhere in between, so the engine's own ramp has to run.
	Check(DecideGarageHold(GARAGE_HIDEOUT_ONE, GS_CLOSING, true, false) ==
	          GarageHold::Ramp,
	      "a door part way down is wound back through the engine's own ramp");
	Check(DecideGarageHold(GARAGE_HIDEOUT_ONE, GS_OPENING, true, false) ==
	          GarageHold::None,
	      "a door already on its way up needs nothing from us");

	// A spray shop is the mirror image: it rests open, so a hold is a hold
	// *shut*.
	Check(GarageRestsOpen(GARAGE_RESPRAY) && GarageRestsOpen(GARAGE_BOMBSHOP1) &&
	          GarageRestsOpen(GARAGE_CRUSHER),
	      "the shops stand open; everything else stands shut");
	Check(!GarageRestsOpen(GARAGE_HIDEOUT_ONE) &&
	          !GarageRestsOpen(GARAGE_FOR_SCRIPT_TO_OPEN) &&
	          !GarageRestsOpen(GARAGE_MISSION),
	      "hideouts, lockups and script doors rest shut");
	Check(DecideGarageHold(GARAGE_RESPRAY, GS_OPENED, true, false) ==
	          GarageHold::Ramp,
	      "a spray shop somebody else is inside is wound shut");
	Check(GarageLatchState(GARAGE_RESPRAY) == GS_FULLYCLOSED,
	      "and latched to GS_FULLYCLOSED, not GS_CLOSEDCONTAINSCAR");
	Check(DecideGarageHold(GARAGE_RESPRAY, GS_CLOSING, true, false) ==
	          GarageHold::None,
	      "a door already on its way down is left to the engine's own ramp");
	Check(DecideGarageHold(GARAGE_RESPRAY, GS_FULLYCLOSED, true, true) ==
	          GarageHold::None,
	      "and once it is down there is nothing left to do - the arm that "
	      "would reopen it is the one GarageUpdateMayRun refuses");
	// The one hold-shut case that does need a silent latch: the door is
	// already down and the state says open anyway. Reachable when a hold
	// arrives in the frame between a local visit's ramp finishing and its own
	// arm running.
	Check(DecideGarageHold(GARAGE_RESPRAY, GS_OPENING, true, true) ==
	          GarageHold::Latch,
	      "a shut door that thinks it is opening is latched shut in silence");

	// A type the script has not created: there is nothing there to hold.
	Check(DecideGarageHold(GARAGE_NONE, GS_FULLYCLOSED, true, true) ==
	          GarageHold::None,
	      "a garage that does not exist is never held");
}

// The one that stops an observer handing itself a free respray.
//
// Every effect of a Pay'n'Spray - the repair, the repaint, the money, the
// bomb, the crusher and the wanted level - lives in the GS_FULLYCLOSED arm of
// *this machine's* CGarage::Update, and it acts on FindPlayerVehicle() and
// FindPlayerPed(). Let it run while a garage is being held shut for somebody
// else and the observer's own car gets repainted and the observer's own stars
// get cleared, because a stranger across the city paid for a paint job.
void TestServicedGarageArmIsSuppressed() {
	std::printf("\ngarages: an observer does not finish somebody else's respray\n");
	using namespace coopiii::game;

	Check(GarageUpdateMayRun(GARAGE_RESPRAY, GS_FULLYCLOSED, false),
	      "unheld, the arm always runs - this is single player");
	Check(!GarageUpdateMayRun(GARAGE_RESPRAY, GS_FULLYCLOSED, true),
	      "held shut and arrived, the completion arm is somebody else's");
	Check(!GarageUpdateMayRun(GARAGE_BOMBSHOP2, GS_FULLYCLOSED, true),
	      "and the same for a bomb shop");
	Check(!GarageUpdateMayRun(GARAGE_CRUSHER, GS_FULLYCLOSED, true),
	      "and the crusher");

	// The moving arms are pure animation - ramp, push to the door entities,
	// arrive, play a sound - so they always run, and that is what gets the
	// door and the sound right on every machine for free.
	Check(GarageUpdateMayRun(GARAGE_RESPRAY, GS_CLOSING, true),
	      "the closing ramp is animation and still runs");
	Check(GarageUpdateMayRun(GARAGE_RESPRAY, GS_OPENING, true),
	      "so is the opening ramp");

	// A garage that rests shut has a harmless resting arm, and letting it run
	// is what keeps the garage camera, the hideout car store and the
	// "you cannot store any more cars" message working for the local player.
	Check(GarageUpdateMayRun(GARAGE_HIDEOUT_ONE, GS_OPENED, true),
	      "a held-open safehouse still runs its own arm");
	Check(GarageUpdateMayRun(GARAGE_HIDEOUT_ONE, GS_FULLYCLOSED, true),
	      "and its closed arm too");
}

// What one bit means. This is the thing that goes on the wire, and it is the
// only classification the whole feature needs.
void TestGarageDeviationIsWhatTravels() {
	std::printf("\ngarages: one bit, and what it means\n");
	using namespace coopiii::game;

	// A safehouse garage: open is news, shut is not.
	Check(!GarageDeviates(GARAGE_HIDEOUT_ONE, GS_FULLYCLOSED), "shut is where it rests");
	Check(GarageDeviates(GARAGE_HIDEOUT_ONE, GS_OPENING),
	      "a door on its way up is already news - the bit says where it is "
	      "heading, not where it has got to");
	Check(GarageDeviates(GARAGE_HIDEOUT_ONE, GS_OPENED), "and open is news");
	Check(!GarageDeviates(GARAGE_HIDEOUT_ONE, GS_CLOSING),
	      "and a door on its way down has already stopped being news");

	// A spray shop: the other way round.
	Check(!GarageDeviates(GARAGE_RESPRAY, GS_OPENED), "a spray shop rests open");
	Check(GarageDeviates(GARAGE_RESPRAY, GS_CLOSING), "closing over a car is news");
	Check(GarageDeviates(GARAGE_RESPRAY, GS_FULLYCLOSED), "and so is being shut");
	Check(!GarageDeviates(GARAGE_RESPRAY, GS_OPENING),
	      "and opening again is the visit ending");

	// GS_OPENEDCONTAINSCAR is open and GS_CLOSEDCONTAINSCAR is shut, which is
	// exactly what CGarages::IsGarageOpen says (`cmp dl,1 / cmp dl,4`).
	Check(GarageStateIsOpen(GS_OPENED) && GarageStateIsOpen(GS_OPENEDCONTAINSCAR),
	      "the engine's own two open states");
	Check(!GarageStateIsOpen(GS_OPENING) && !GarageStateIsOpen(GS_CLOSING),
	      "a door in motion is not open - IsGarageOpen agrees");

	// A slot the script never used is never news, whatever is in its state
	// byte.
	Check(!GarageDeviates(GARAGE_NONE, GS_OPENED), "an unused slot says nothing");

	// And the width of the thing. Thirty-two is CGarages::Update's own loop
	// bound, a literal `cmp ebx,20h`, not CGarages::NumGarages.
	Check(coopiii::game::NUM_GARAGES == 32 && coopiii::NUM_GARAGES == 32,
	      "the engine's 32 garages and the wire's mask agree");
	Check(coopiii::game::SIZEOF_GARAGE == 0x8C,
	      "and the stride the OPEN_GARAGE handler multiplies by");
}

void TestRemoteDriverIsRecorded() {
	std::printf("\nwho is driving what\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.Tick();

	S_EnterVehicle enter;
	InitHeader(enter, 1000);
	enter.playerId   = 1;
	enter.body       = EnterVehicleBody{};
	enter.body.netId = 80;
	enter.body.seat  = 0;
	c.HandleMessage(Wrap(enter, CH_EVENT));
	Check(c.VehicleByNetId(80)->driverPlayerId == 1, "alice is driving car 80");
	Check(c.LocalVehicleNetId() == INVALID_NETID,
	      "and somebody else's car is not ours");

	S_ExitVehicle exit;
	InitHeader(exit, 2000);
	exit.playerId = 1;
	exit.netId    = 80;
	c.HandleMessage(Wrap(exit, CH_EVENT));
	Check(c.VehicleByNetId(80)->driverPlayerId == 0xFF, "and now nobody is");
	Check(c.VehicleCount() == 1,
	      "a parked car stays in the session, somebody left it there");
}

// ---- seating ---------------------------------------------------------------
//
// Seating is written as a reconciliation rather than an event handler, and
// these are the races that's for. The enter event is reliable and arrives at
// once; the ped and the car each take as long as their own model takes to
// stream, and either can be taken away again afterward. Every test below is
// just a different order of those three things.

S_EnterVehicle MakeEnter(uint8_t playerId, uint16_t netId, uint8_t seat = 0) {
	S_EnterVehicle e;
	InitHeader(e, 1000);
	e.playerId   = playerId;
	e.body       = EnterVehicleBody{};
	e.body.netId = netId;
	e.body.seat  = seat;
	return e;
}

S_EnteringVehicle MakeEntering(uint8_t playerId, uint16_t netId, uint8_t seat,
                               uint8_t door) {
	S_EnteringVehicle e;
	InitHeader(e, 1000);
	e.playerId   = playerId;
	e.body       = EnteringVehicleBody{};
	e.body.netId = netId;
	e.body.seat  = seat;
	e.body.door  = door;
	return e;
}

S_ExitVehicle MakeExit(uint8_t playerId, uint16_t netId) {
	S_ExitVehicle e;
	InitHeader(e, 2000);
	e.playerId = playerId;
	e.netId    = netId;
	return e;
}

// Gets to the state the seating tests start from: alice joined, her model is
// available, and she has a ped in the world.
void GiveAliceAPed(Client &c) {
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	g_rec.modelReady = true;
	FeedPosition(c, 1);
	c.Tick();
}

void TestModelChangeRebuildsThePed() {
	std::printf("\na player who changes model\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	Check(g_rec.spawns == 1 && c.PlayerSlot(1).modelId == 7, "spawned as model 7");

	S_PlayerModel skin;
	InitHeader(skin, 2000);
	skin.playerId = 1;
	skin.modelId  = 7;
	c.HandleMessage(Wrap(skin, CH_EVENT));
	Check(g_rec.despawns == 0, "the same model again changes nothing");

	skin.modelId = 0;   // MI_PLAYER
	c.HandleMessage(Wrap(skin, CH_EVENT));
	Check(c.PlayerSlot(1).modelId == 0, "the roster takes the new model");
	Check(g_rec.despawns == 1, "and the old ped is destroyed");
	Check(c.PlayerSlot(1).poolHandle == -1, "leaving nothing behind");
	Check(c.PlayerSlot(1).spawnPending, "with the spawn re-armed");
	// A new ped inherits none of what was driven into the old one, and a
	// remembered animation or weapon would stop it being applied again.
	Check(c.PlayerSlot(1).appliedWeapon == 0xFFFF,
	      "and nothing remembered from the old body");

	c.Tick();
	Check(g_rec.spawns == 2, "the new body is created");
	Check(c.PlayerSlot(1).poolHandle > 0, "with a fresh handle");
}

void TestModelChangeWhileSeated() {
	std::printf("\nchanging model in the driver's seat\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "seated");

	S_PlayerModel skin;
	InitHeader(skin, 2000);
	skin.playerId = 1;
	skin.modelId  = 0;
	c.HandleMessage(Wrap(skin, CH_EVENT));
	Check(g_rec.unseats == 1, "out of the car before the ped is destroyed");
	Check(!c.PlayerSlot(1).Seated(), "and not seated in the meantime");
	Check(c.PlayerSlot(1).seatVehicleNetId == 80,
	      "but the session still says she is driving");

	c.Tick();
	Check(g_rec.spawns == 2, "the new body is created");
	Check(c.PlayerSlot(1).Seated(), "and put back in the car");
}

S_PlayerLook MakeLook(uint8_t playerId, const char *look) {
	S_PlayerLook l;
	InitHeader(l, 2000);
	l.playerId = playerId;
	std::memset(l.look, 0, sizeof l.look);
	std::strncpy(l.look, look, sizeof l.look - 1);
	return l;
}

void TestCleanPlayerLook() {
	std::printf("\ncleaning a look off the wire\n");
	char a[PLAYER_LOOK_LEN] = "PLAYERP";
	Check(CleanPlayerLook(a) && std::strcmp(a, "playerp") == 0, "lower-cased, the way the engine keeps it");

	char b[PLAYER_LOOK_LEN] = "play er";
	Check(!CleanPlayerLook(b) && b[0] == '\0', "a space is refused and the buffer zeroed");

	char c[PLAYER_LOOK_LEN] = "";
	Check(!CleanPlayerLook(c), "an empty name is not a look");

	char d[PLAYER_LOOK_LEN];
	std::memset(d, 'a', sizeof d);
	Check(!CleanPlayerLook(d) && d[0] == '\0', "an unterminated one is refused");

	char e[PLAYER_LOOK_LEN] = {};
	std::memcpy(e, "player\0junk", 11);
	Check(CleanPlayerLook(e) && e[7] == '\0' && e[8] == '\0',
	      "whatever follows the terminator goes");
}

void TestLookSlotPicking() {
	std::printf("\nwhich body a remote Claude is built from\n");
	using namespace coopiii::game;

	LookSlot slots[LOOK_SLOTS];
	auto name = [&](int i, const char *n) {
		std::memset(slots[i].name, 0, sizeof slots[i].name);
		std::strncpy(slots[i].name, n, sizeof slots[i].name - 1);
	};

	LookChoice c = PickLookSlot("player", "player", slots);
	Check(c.pick == LookPick::Model0, "the same clothes as ours is model 0");
	c = PickLookSlot("", "playerp", slots);
	Check(c.pick == LookPick::Model0, "a player who never said is model 0");
	c = PickLookSlot("PLAYER", "player", slots);
	Check(c.pick == LookPick::Model0, "and the case of the name doesn't matter");

	c = PickLookSlot("playerp", "player", slots);
	Check(c.pick == LookPick::Slot && c.slot == 3,
	      "the other clothes go in special04, the one missions reach for last");

	c = PickLookSlot("eight2", "player", slots);
	Check(c.pick == LookPick::Refused, "8-Ball's clothes are not Claude's");

	// A mission holding 04, and 03 built from by a ped somebody forgot.
	slots[3].flags = 0x02;   // SCRIPTOWNED
	slots[2].refs  = 1;
	c = PickLookSlot("playerp", "player", slots);
	Check(c.pick == LookPick::Slot && c.slot == 1, "a held or used slot is skipped");

	slots[1].loadState = 2;   // INQUEUE for somebody else
	slots[1].flags     = 0x02;
	slots[0].flags     = 0x01;   // DONT_REMOVE
	c = PickLookSlot("playerp", "player", slots);
	Check(c.pick == LookPick::NoSlot, "with all four taken there is nowhere to go");

	// Ours, loaded with this look, is shared rather than a second one taken.
	slots[1]      = LookSlot{};
	slots[1].ours = true;
	slots[1].refs = 1;
	name(1, "playerp");
	c = PickLookSlot("playerp", "player", slots);
	Check(c.pick == LookPick::Slot && c.slot == 1, "a slot we hold with this look is shared");
	c = PickLookSlot("playerx", "player", slots);
	Check(c.pick == LookPick::NoSlot, "but not handed out for a different one");

	// A free slot that already holds the look is picked over a higher one.
	LookSlot fresh[LOOK_SLOTS];
	std::strcpy(fresh[0].name, "playerp");
	fresh[0].loadState = 1;
	c = PickLookSlot("playerp", "player", fresh);
	Check(c.pick == LookPick::Slot && c.slot == 0, "a free slot already loaded with it saves a load");

	LookSlot held;
	held.ours = true;
	Check(LookSlotReleasable(held, false), "a slot of ours nobody wants goes back");
	Check(!LookSlotReleasable(held, true), "not while a player is waiting on it");
	held.refs = 1;
	Check(!LookSlotReleasable(held, false), "nor while a ped is built from it");
	Check(!LookSlotFree(held), "and ours is never free to somebody else");
}

void TestLookChangeRebuildsThePed() {
	std::printf("\na player who changes clothes\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice", 0), CH_EVENT));
	g_rec.modelReady = true;
	FeedPosition(c, 1);
	c.Tick();
	Check(g_rec.spawns == 1, "alice is built as Claude");

	c.HandleMessage(Wrap(MakeLook(1, "PLAYERP"), CH_EVENT));
	Check(std::strcmp(c.PlayerSlot(1).look, "playerp") == 0,
	      "the roster takes her look, lower case");
	Check(g_rec.despawns == 1, "and her ped comes down");
	Check(c.PlayerSlot(1).spawnPending, "to be built again");

	c.HandleMessage(Wrap(MakeLook(1, "playerp"), CH_EVENT));
	Check(g_rec.despawns == 1, "the same look again changes nothing");

	c.Tick();
	Check(g_rec.spawns == 2, "the new body is built");

	c.HandleMessage(Wrap(MakeLook(1, "play er"), CH_EVENT));
	Check(std::strcmp(c.PlayerSlot(1).look, "playerp") == 0 && g_rec.despawns == 1,
	      "a name that doesn't clean is dropped");
}

void TestLookOnAnotherModelKeepsThePed() {
	std::printf("\na look for somebody who isn't Claude\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);   // model 7

	c.HandleMessage(Wrap(MakeLook(1, "playerp"), CH_EVENT));
	Check(std::strcmp(c.PlayerSlot(1).look, "playerp") == 0, "the look is kept");
	Check(g_rec.despawns == 0, "but her ped is left alone");
}

void TestSpawnWaitsForTheLook() {
	std::printf("\nClaude is built once his clothes are in\n");
	WorldBridge b        = RecordingBridge();
	b.PrepareRemoteLook  = &RecPrepareLook;
	Client c;
	c.SetBridge(b);
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice", 0), CH_EVENT));
	g_rec.modelReady = true;
	g_rec.lookReady  = false;
	FeedPosition(c, 1);
	c.Tick();
	Check(g_rec.lookPrepares > 0, "the look is asked for");
	Check(g_rec.spawns == 0, "and model 0 being ready isn't enough on its own");

	g_rec.lookReady = true;
	c.Tick();
	Check(g_rec.spawns == 1, "built once the look is ready");
	const int asked = g_rec.lookPrepares;
	c.Tick();
	Check(g_rec.lookPrepares == asked, "and not asked again once there is a ped");
}

void TestOurLookIsSentOnChange() {
	std::printf("\ntelling the session what we're wearing\n");
	WorldBridge b           = RecordingBridge();
	b.SampleLocalPlayerLook = &RecSampleLocalLook;
	Client c;
	c.SetBridge(b);
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	c.SendLocalLookForTest();
	Check(c.SentLookForTest()[0] == '\0', "nothing without a player ped");

	std::strcpy(g_rec.localLook, "playerp");
	c.SendLocalLookForTest();
	Check(std::strcmp(c.SentLookForTest(), "playerp") == 0, "the prison clothes go out");

	std::strcpy(g_rec.localLook, "player");
	c.SendLocalLookForTest();
	Check(std::strcmp(c.SentLookForTest(), "player") == 0, "and the change after 8-Ball");

	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	Check(c.SentLookForTest()[0] == '\0', "a new session hasn't heard it yet");
}

void TestSeatingWaitsForTheCar() {
	std::printf("\nseating a remote driver\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);

	// The enter event beats the car it names - that's the normal case. It's
	// reliable, and the spawn packet for a car claimed on the same frame is
	// one round trip and one model load behind.
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(g_rec.seatAttempts == 0, "nothing is attempted while the car does not exist");
	Check(!c.PlayerSlot(1).Seated(), "and she is not considered seated");
	Check(c.PlayerSlot(1).seatVehicleNetId == 80,
	      "but the request stands, it is not dropped for arriving early");

	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.Tick();
	Check(g_rec.seats == 1, "she is seated on the first frame the car exists");
	Check(g_rec.lastSeatVehicle == 80 && g_rec.lastSeatIndex == 0,
	      "in car 80, as the driver");
	Check(c.PlayerSlot(1).Seated(), "and the roster agrees");

	c.Tick();
	c.Tick();
	Check(g_rec.seatAttempts == 1, "and she is not seated again every frame");
}

void TestSeatedPedStopsBeingPositioned() {
	std::printf("\na seated ped belongs to the car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "seated");

	// The suppression itself lives in game/ped.cpp and needs a live engine to
	// observe. What's checkable here is the flag it reads: the bridge gets
	// told the ped is seated on every pose it's offered from then on, and
	// that's what lets it decline to write a position the car now owns.
	const int before = g_rec.posesWhileSeated;
	for (uint32_t i = 0; i <= 8; ++i)
		c.HandleMessage(Wrap(MakeState(1, 2000 + i * 40, float(i)), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.posesWhileSeated > before,
	      "every pose offered while seated is flagged as such");

	S_ExitVehicle exit;
	InitHeader(exit, 3000);
	exit.playerId = 1;
	exit.netId    = 80;
	c.HandleMessage(Wrap(exit, CH_EVENT));
	c.Tick();
	Check(g_rec.unseats == 1, "getting out is driven through the bridge once");
	Check(!c.PlayerSlot(1).Seated(), "and she is on foot again");

	const int flagged = g_rec.posesWhileSeated;
	for (uint32_t i = 0; i <= 8; ++i)
		c.HandleMessage(Wrap(MakeState(1, 4000 + i * 40, float(i)), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.posesWhileSeated == flagged,
	      "and her position is hers again");
}

void TestCarIsEmptiedBeforeItIsDestroyed() {
	std::printf("\nthe order of a despawn under a seated ped\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "seated");

	S_VehicleDespawn gone;
	InitHeader(gone, 2000);
	gone.netId = 80;
	c.HandleMessage(Wrap(gone, CH_EVENT));

	Check(g_rec.vehicleDespawns == 1, "the car is destroyed");
	// The engine nils m_pMyVehicle through the reference it registered, but
	// nothing clears bInVehicle. So a ped still seated when its car goes is
	// a ped following a null pointer through ProcessControl.
	Check(g_rec.unseatsAtLastVehicleDespawn == 1,
	      "and she was taken out of it first, not after");
	Check(!c.PlayerSlot(1).Seated(), "she is on foot");
}

void TestLosingThePedLosesTheSeat() {
	std::printf("\na ped taken away while seated\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "seated");

	// The engine destroys the ped under us. ResolveRemote clears the handle
	// and re-arms the spawn - the seat has to go with it, otherwise the new
	// ped never gets put back in the car because the roster thinks it's
	// already there.
	g_rec.loseNextPose = true;
	for (uint32_t i = 0; i <= 8; ++i)
		c.HandleMessage(Wrap(MakeState(1, 2000 + i * 40, float(i)), CH_SNAPSHOT));
	c.Tick();
	Check(c.PlayerSlot(1).poolHandle == -1, "the ped is gone");
	Check(!c.PlayerSlot(1).Seated(), "so nothing is seated any more");
	Check(c.PlayerSlot(1).seatVehicleNetId == 80, "but the session still says she is driving");

	c.Tick();
	Check(c.PlayerSlot(1).poolHandle > 0, "the ped comes back");
	Check(c.PlayerSlot(1).Seated(), "and it is put back in the car");
}

void TestRefusedSeatingIsNotRetriedForever() {
	std::printf("\na seating the engine refuses\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	// Both halves present and the bridge still says no. Nothing about the
	// next frame changes that answer, and asking again sixty times a second
	// is exactly how a one-line bug turns into a frozen game.
	g_rec.refuseSeating = true;
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	c.Tick();
	c.Tick();
	Check(g_rec.seatAttempts == 1, "asked once");
	Check(g_rec.seats == 0, "and nobody is seated");
	Check(!c.PlayerSlot(1).Seated(), "the roster does not pretend otherwise");

	// A fresh event is a fresh instruction though - the give-up applies to
	// this attempt, not to this player forever.
	g_rec.refuseSeating = false;
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(g_rec.seats == 1, "a later event is acted on");
}

// ---- getting in with the door open ----------------------------------------
//
// One rule holds every one of these together, and it is the one worth
// breaking the suite over: whichever way the animation goes, the player ends
// up in the seat the session gave them. The animation is allowed to be
// refused, interrupted, overtaken and timed out; it is not allowed to leave
// anybody halfway.

void TestAnimatedEntryIsTriedBeforeTheWarp() {
	std::printf("\nwalking up to the door instead of appearing in the seat\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	g_rec.animEntry    = true;
	g_rec.animProgress = SEAT_RUNNING;
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(g_rec.seatAnimBegins == 1, "the engine is asked to open the door");
	Check(g_rec.seatAttempts == 0, "and nobody is warped while it might work");
	Check(c.PlayerSlot(1).Entering(), "she is on her way in");
	Check(!c.PlayerSlot(1).Seated(), "but not in the seat yet");

	// Still walking. Nothing new is started and nothing is given up on.
	c.Tick();
	c.Tick();
	Check(g_rec.seatAnimBegins == 1, "the entry is not started again every frame");
	Check(g_rec.seatAnimAborts == 0, "nor abandoned while it is still going");
	Check(g_rec.seatAttempts == 0, "and still no warp");

	g_rec.animProgress = SEAT_DONE;
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "she gets in");
	Check(!c.PlayerSlot(1).Entering(), "and is no longer on her way");
	Check(g_rec.seatAttempts == 0,
	      "having reached the seat without ever being put there");
	Check(g_rec.seatAnimAborts == 0, "and with nothing to give back");
}

// ---- and getting in through the door the owner actually used --------------
//
// Everything above this line is about an entry the session has already
// confirmed. These are about the second before that: the owner's engine has
// started the entry and nothing has confirmed anything yet.

void TestAnIntentOpensTheDoorTheOwnerUsed() {
	std::printf("\ngetting in through the near door, not the seat's door\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	g_rec.animEntry    = true;
	g_rec.animProgress = SEAT_RUNNING;

	// Seat 0 through the front-right door: somebody who pressed the enter key
	// standing on the passenger side. The engine opens the near door and
	// shuffles them across inside, and an observer that opens the driver's
	// door instead drags the replica round the car to it.
	c.HandleMessage(Wrap(MakeEntering(1, 80, 0, 1), CH_EVENT));
	c.Tick();
	Check(g_rec.seatAnimBegins == 1,
	      "the entry starts on the intent, before any claim");
	Check(g_rec.lastSeatDoor == 1, "through the door the owner said, not seat 0's");
	Check(g_rec.lastSeatIndex == 0, "into the seat the owner said");
	Check(c.PlayerSlot(1).Entering(), "she is on her way in");
	Check(g_rec.seatAttempts == 0, "and nothing was warped on a promise");

	// The engine finishes it. The claim has still not arrived, and that is
	// the normal case: it is sent at the end of the owner's entry and this
	// one ended first.
	g_rec.animProgress = SEAT_DONE;
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "she gets in without waiting for the claim");
	Check(g_rec.seatAttempts == 0, "still never warped");

	// And the claim, when it turns up, agrees with what already happened.
	c.HandleMessage(Wrap(MakeEnter(1, 80, 0), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "the claim confirms rather than redoes it");
	Check(g_rec.seatAnimBegins == 1, "no second entry is played");
	Check(g_rec.unseats == 0, "and she is not taken out and put back");
}

void TestOurOwnEntryIsAnnouncedOnceAndRearmed() {
	std::printf("\ntelling the session we are getting in, once\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.Tick();   // which is what actually builds the car and gives it a handle
	// The pool slot the session's car landed in. Our engine hands back the
	// same number for the car we are climbing into, and matching on it is the
	// only thing that turns "that car over there" into a netId - two identical
	// parked cars side by side are an ordinary sight in Liberty City.
	const int32_t handle = c.VehicleByNetId(80)->poolHandle;

	g_rec.localEntryActive = true;
	g_rec.localEntry       = LocalCarEntry{handle, 0, 1};

	c.TickEnteringForTest();
	Check(c.AnnouncedEntryHandleForTest() == handle, "the entry is announced");

	// Twenty-five of these run inside one entry. None of them is a packet.
	const int after = g_rec.localEntrySamples;
	c.TickEnteringForTest();
	c.TickEnteringForTest();
	Check(g_rec.localEntrySamples == after + 2, "the ped is still asked each tick");
	Check(c.AnnouncedEntryHandleForTest() == handle,
	      "and nothing new is said while nothing has changed");

	// The entry ends, one way or the other, and the next one has to be able
	// to say so - including a second entry into the same car through the same
	// door, which is what walking away and coming back looks like.
	g_rec.localEntryActive = false;
	c.TickEnteringForTest();
	Check(c.AnnouncedEntryHandleForTest() == -1, "the next entry is re-armed");
}

void TestAnIntentNeverWarps() {
	std::printf("\nan intent is not a seat\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	// The engine refuses to animate it - the car is rolling, the door is in
	// use, whatever. On a confirmed seat this is the frame the warp happens.
	// On an intent it must not be: nothing has said she is in that car, only
	// that she started getting into it.
	g_rec.animEntry = false;
	c.HandleMessage(Wrap(MakeEntering(1, 80, 0, 0), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.seatAnimBegins == 1, "the door was offered");
	Check(g_rec.seatAttempts == 0, "and she was NOT put in the seat");
	Check(!c.PlayerSlot(1).Seated(), "she is still on foot");

	// The claim is what changes that, and when it comes the ordinary path
	// runs: one attempt at the door, then the warp behind it.
	c.HandleMessage(Wrap(MakeEnter(1, 80, 0), CH_EVENT));
	c.Tick();
	Check(g_rec.seats == 1, "the claim seats her");
	Check(c.PlayerSlot(1).Seated(), "the roster agrees");
}

void TestAnIntentDoesNotTakeAnOccupiedSeat() {
	std::printf("\nan intent does not pull anybody out of a seat\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeJoin(2, "bob"), CH_EVENT));
	FeedPosition(c, 2);
	c.Tick();
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	// Bob is driving, and the session says so.
	g_rec.animEntry = false;
	c.HandleMessage(Wrap(MakeEnter(2, 80, 0), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(2).Seated(), "Bob has the wheel");

	// Alice starts jacking him. Her intent must not make room: starting the
	// entry would evict Bob's replica a second before the server has decided
	// anything, which is this machine deciding that Bob left a car.
	const int begins = g_rec.seatAnimBegins;
	g_rec.animEntry  = true;
	c.HandleMessage(Wrap(MakeEntering(1, 80, 0, 0), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.seatAnimBegins == begins, "her entry is not started on the intent");
	Check(c.PlayerSlot(2).Seated(), "and Bob is still driving");
	Check(!c.PlayerSlot(1).Entering(), "she is waiting for the session to decide");

	// Which it does, in the order protocol 22 fixed: the loser first, then
	// the winner. From there the ordinary path runs.
	c.HandleMessage(Wrap(MakeExit(2, 80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80, 0), CH_EVENT));
	c.Tick();
	Check(g_rec.seatAnimBegins == begins + 1, "the claim starts it");
	Check(!c.PlayerSlot(2).Seated(), "and Bob is out");
}

void TestAnAbandonedEntryDoesNotLeaveHerInTheCar() {
	std::printf("\nan entry nobody ever confirmed\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	g_rec.animEntry    = true;
	g_rec.animProgress = SEAT_DONE;
	c.HandleMessage(Wrap(MakeEntering(1, 80, 0, 0), CH_EVENT));
	c.Tick();   // starts it
	c.Tick();   // and the poll that finds it finished
	Check(c.PlayerSlot(1).Seated(), "the entry finished on this machine");

	// And the claim never comes: on her machine she was shot out of the
	// doorway, or gave up. Nothing retracts an intent, because nothing on the
	// wire describes an entry that did not happen - so it has to lapse, and
	// everything it caused has to go with it.
	Check(c.PlayerSlot(1).enterIntentNetId != INVALID_NETID,
	      "the intent is still standing meanwhile");
	c.LapseEnterIntentForTest(1);   // as if the intent's lifetime had passed
	c.Tick();
	Check(!c.PlayerSlot(1).Seated(),
	      "she is taken back out of a car the session never said she was in");
	Check(g_rec.unseats >= 1, "through the ordinary way out");
}

void TestARefusedAnimationWarpsOnTheSameFrame() {
	std::printf("\nan entry the engine will not start\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	// The car is driving off, the door is already in use, the ped is dying -
	// the bridge collapses all of those into a no, and a no must not cost a
	// frame. Anything else is a remote player standing next to a car for a
	// moment for no reason anybody can see.
	g_rec.animEntry = false;
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(g_rec.seatAnimBegins == 1, "it was offered");
	Check(g_rec.seats == 1, "and she is in the seat on that same frame");
	Check(c.PlayerSlot(1).Seated(), "the roster agrees");
	Check(!c.PlayerSlot(1).Entering(), "and nothing is left in flight");
	Check(g_rec.seatAnimAborts == 0, "nothing was started, so nothing to abandon");
}

void TestAnAnimationThatStopsFallsBackToTheWarp() {
	std::printf("\nan entry the engine gives up on\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	g_rec.animEntry    = true;
	g_rec.animProgress = SEAT_RUNNING;
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Entering(), "on her way in");

	// Shot at, knocked over, the door blocked - the engine drops the entry
	// and says nothing. This is the case that would otherwise leave a ped
	// standing in the street forever believing it was getting into a car.
	g_rec.animProgress = SEAT_LOST;
	c.Tick();
	Check(g_rec.seatAnimAborts == 1, "the door is given back");
	Check(g_rec.seats == 1, "and she is put in the seat instead");
	Check(g_rec.seatsAtLastAbort == 0,
	      "in that order - the door goes back before the seat is taken");
	Check(c.PlayerSlot(1).Seated() && !c.PlayerSlot(1).Entering(),
	      "and the two states agree again");
}

void TestAnEntryThatNeverFinishesIsTimedOut() {
	std::printf("\nan entry that just never finishes\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.SetSeatAnimTimeoutMs(0);
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	// The nastiest case, because the engine is still reporting progress. An
	// animation that plays forever - and this project has had one - looks
	// exactly like one that is about to finish, so the only thing that can
	// end it is a clock.
	g_rec.animEntry    = true;
	g_rec.animProgress = SEAT_RUNNING;
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Entering(), "it started");

	c.Tick();
	Check(g_rec.seatAnimAborts == 1, "and it is taken off her when time is up");
	Check(c.PlayerSlot(1).Seated(), "she is in the seat");
	Check(g_rec.seats == 1, "put there by the warp, which is what it is for");
}

// The seat key teleporting the player into the back of a Pony. The entry was
// timed out at 2.5 s and the chain for a van's back door is 3.53 s long, so
// it was warped every single time. What decides it now is whether the
// animation is moving, and these are the numbers from anim\ped.ifp.
void TestAnEntryIsJudgedByItsAnimation() {
	std::printf("\nan entry is waited for as long as its animation moves\n");

	constexpr uint32_t FRONT_DOOR_MS = 33 + 933 + 633 + 467;    // align, open, getin, close
	constexpr uint32_t VAN_BACK_MS   = 33 + 533 + 1600 + 1367;  // VAN_openL, getinL, closeL
	constexpr uint32_t SHUFFLE_MS    = FRONT_DOOR_MS + 633;     // driver by the other door
	Check(SEAT_ANIM_TIMEOUT_MS > VAN_BACK_MS + VAN_BACK_MS / 2,
	      "the cap covers a van's back door with half as much again to spare");
	Check(SEAT_ANIM_TIMEOUT_MS > SHUFFLE_MS * 2,
	      "and a driver who went in by the passenger door twice over");
	Check(ENTER_INTENT_TTL_MS > SEAT_ANIM_TIMEOUT_MS,
	      "an intent outlives the entry it started");

	// A van's back door played at 25 Hz of looks, the mark moving every look.
	EntryWatch w;
	w.Begin(1000, 7);
	bool stalled = false;
	uint32_t t = 1000;
	for (uint32_t mark = 8; t < 1000 + VAN_BACK_MS; ++mark) {
		t += 40;
		stalled = stalled || w.Stalled(t, mark);
	}
	Check(!stalled, "a van's back door, moving the whole time, is never a stall");

	// Looks every 40 ms with the mark held, up to `until`; the last answer.
	const auto hold = [](EntryWatch &e, uint32_t &now, uint32_t until, uint32_t mark) {
		bool last = false;
		while (now + 40 <= until) {
			now += 40;
			last = e.Stalled(now, mark);
		}
		return last;
	};

	// The same animation stuck on one frame.
	EntryWatch s;
	s.Begin(1000, 7);
	uint32_t ts = 1000;
	Check(!s.Stalled(ts += 40, 7), "one look without movement is not a stall");
	Check(!hold(s, ts, 1000 + SEAT_ANIM_STALL_MS - 40, 7), "nor is most of a second");
	Check(hold(s, ts, 1000 + SEAT_ANIM_STALL_MS, 7), "a second of nothing is");

	// Moving again resets it.
	EntryWatch r;
	r.Begin(1000, 7);
	uint32_t tr = 1000;
	hold(r, tr, 1920, 7);
	Check(!r.Stalled(tr += 40, 8), "movement starts the count again");
	const uint32_t moved = tr;
	Check(!hold(r, tr, moved + SEAT_ANIM_STALL_MS - 40, 8), "and counts from where it moved");
	Check(hold(r, tr, moved + SEAT_ANIM_STALL_MS, 8), "to a full second");

	// Frames that never ran - the window dragged, a debugger - do not count.
	EntryWatch g;
	g.Begin(1000, 7);
	uint32_t tg = 1000;
	hold(g, tg, 1400, 7);
	tg += 5000;
	Check(!g.Stalled(tg, 7),
	      "five seconds with no frames at all is not the animation's fault");
	const uint32_t back = tg;
	Check(!hold(g, tg, back + SEAT_ANIM_STALL_MS - 400 - 40, 7),
	      "and what was left of the second before the gap is all it has after it");
	Check(hold(g, tg, back + SEAT_ANIM_STALL_MS - 400 + 40, 7),
	      "so a real stall across the gap is still caught");
}

// The seat key picked seat 1 wherever the player stood, and seat 1's door is
// on the right: from the driver's side the ped was lined up through the car.
void TestTheSeatKeyPicksADoorOnOurSide() {
	std::printf("\nthe seat key asks for a door on the player's own side\n");
	constexpr uint16_t S1 = 1u << 1, S2 = 1u << 2, S3 = 1u << 3, S4 = 1u << 4;

	Check(PickPassengerSeat(S1 | S2 | S3, /*onRight=*/false) == 2,
	      "on the left of a four-door car: the back door on the left");
	Check(PickPassengerSeat(S1 | S2 | S3, true) == 1,
	      "on the right: the front passenger's door");
	Check(PickPassengerSeat(S2 | S3, true) == 3,
	      "on the right with the front taken: the back door on that side");
	Check(PickPassengerSeat(S1, false) == 1,
	      "a two-door car has one passenger door, whichever side we are on");
	Check(PickPassengerSeat(S1 | S3, false) == 1,
	      "nothing free on our side: the lowest free seat, as before");
	Check(PickPassengerSeat(S4, true) == 4, "a seat with no door of its own still counts");
	Check(PickPassengerSeat(0, true) == -1 && PickPassengerSeat(0, false) == -1,
	      "and a full car is no seat at all");
	Check(PickPassengerSeat(1u << 0, false) == -1,
	      "bit 0 is the driver's seat and is never offered");
}

// The car the jacked player could never get back into. A replica taken out
// of a seat while its get-out animation was still playing - its owner's exit
// packet landing first - left the door's bit in m_nGettingOutFlags, and
// SetEnterCar refuses that door for good.
void TestAWarpedExitGivesTheDoorBack() {
	std::printf("\na ped taken out mid-exit gives its door back\n");
	using game::GettingOutFlagsAfterUnseat;
	namespace o = game::offs;

	const uint8_t lf = o::CAR_DOOR_FLAG_LF;
	const uint8_t rf = o::CAR_DOOR_FLAG_RF;
	Check(GettingOutFlagsAfterUnseat(lf, game::PEDSTATE_EXIT_CAR, lf) == 0,
	      "halfway out of the driver's door: the door is free again");
	Check(GettingOutFlagsAfterUnseat(lf | rf, game::PEDSTATE_EXIT_CAR, lf) == rf,
	      "only that door - somebody else getting out the other side keeps theirs");
	Check(GettingOutFlagsAfterUnseat(rf, game::PEDSTATE_DRAG_FROM_CAR, rf) == 0,
	      "being dragged out is the same claim, and ~CPed clears it the same way");
	Check(GettingOutFlagsAfterUnseat(lf, game::PEDSTATE_DRIVING, lf) == lf,
	      "a ped just sitting there never claimed a door, so nothing is touched");
	Check(GettingOutFlagsAfterUnseat(lf, game::PEDSTATE_IDLE, lf) == lf,
	      "and neither did one already on foot");
	Check(GettingOutFlagsAfterUnseat(lf, game::PEDSTATE_ENTER_CAR, lf) == lf,
	      "getting in is the other byte, and QuitEnteringCar's job");
}

void TestOneAnimatedAttemptPerEnterEvent() {
	std::printf("\none go at the door per thing the session says\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.SetSeatAnimTimeoutMs(0);
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	g_rec.animEntry    = true;
	g_rec.animProgress = SEAT_RUNNING;
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.seatAnimBegins == 1 && g_rec.seats == 1,
	      "the first attempt times out into a warp");

	// Now take the ped away and give it back. The seat has to be applied to
	// the new one - and applied, not attempted again, because an entry that
	// could not finish once will not finish now either.
	g_rec.loseNextPose = true;
	for (uint32_t i = 0; i <= 8; ++i)
		c.HandleMessage(Wrap(MakeState(1, 2000 + i * 40, float(i)), CH_SNAPSHOT));
	c.Tick();
	c.Tick();
	Check(g_rec.seatAnimBegins == 1, "the door is not tried a second time");
	Check(g_rec.seats == 2, "but she is back in the seat");

	// Getting out and back in is a fresh go, though. The give-up is about one
	// attempt, not about this player for the rest of the match. Note that
	// re-stating the seat she is already in earns nothing: the session and
	// the engine already agree, and there is nothing to animate towards.
	S_ExitVehicle out;
	InitHeader(out, 4000);
	out.playerId = 1;
	out.netId    = 80;
	c.HandleMessage(Wrap(out, CH_EVENT));
	c.Tick();
	Check(!c.PlayerSlot(1).Seated(), "she is out");

	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(g_rec.seatAnimBegins == 2, "and getting in again earns a new attempt");
}

void TestAnEntryIsAbandonedWhenTheCarGoes() {
	std::printf("\nthe car taken away mid-entry\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	g_rec.animEntry    = true;
	g_rec.animProgress = SEAT_RUNNING;
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Entering(), "halfway through the door");

	// A car about to be destroyed has to let go of everybody first, and
	// "everybody" includes the player who is not in it yet. That player is
	// holding one of its doors.
	S_VehicleDespawn gone;
	InitHeader(gone, 3000);
	gone.netId = 80;
	c.HandleMessage(Wrap(gone, CH_EVENT));
	Check(g_rec.seatAnimAborts == 1, "the entry comes off before the car does");
	Check(!c.PlayerSlot(1).Entering(), "and nothing is left pointing at it");
	Check(g_rec.seats == 0, "with no seat taken in a car that is going away");
}

void TestAnEntryIsAbandonedWhenThePlayerDies() {
	std::printf("\nkilled on the way to the door\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));

	g_rec.animEntry    = true;
	g_rec.animProgress = SEAT_RUNNING;
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Entering(), "halfway through the door");

	S_Death died;
	InitHeader(died, 3000);
	died.playerId    = 1;
	died.killerNetId = INVALID_NETID;
	died.animId      = 0;
	c.HandleMessage(Wrap(died, CH_EVENT));
	Check(g_rec.seatAnimAborts == 1, "the entry is taken off the corpse");
	Check(!c.PlayerSlot(1).Entering(), "which is not going to finish it");
	Check(g_rec.seats == 0, "and a corpse is not warped into the seat either");
}

void TestExitAnimationStartsFromTheSnapshot() {
	std::printf("\ngetting out on the snapshot, not on the event\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "in the car");
	Check(g_rec.exitAnimBegins == 0, "and not getting out of it");

	// The owner's own engine has started the get-out. That is on the
	// snapshot a whole animation before S_ExitVehicle is sent, because
	// S_ExitVehicle is sent when their bInVehicle goes false, which is the
	// last thing the animation does.
	S_PlayerState leaving = MakeState(1, 3000, 5.0f);
	leaving.body.pedState = WIRE_PEDSTATE_EXIT_CAR;
	c.HandleMessage(Wrap(leaving, CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.exitAnimBegins >= 1, "the door opens here too");
	Check(c.PlayerSlot(1).Seated(),
	      "and the session still says she is in it, because she still is");

	// And the event is what actually ends it.
	S_ExitVehicle out;
	InitHeader(out, 3200);
	out.playerId = 1;
	out.netId    = 80;
	c.HandleMessage(Wrap(out, CH_EVENT));
	c.Tick();
	Check(!c.PlayerSlot(1).Seated(), "the event is still the authority");
	Check(g_rec.unseats >= 1, "and the plain teardown still runs behind it");
}

// ---- and telling the session at the right moment ---------------------------
//
// "Cuando entra con la G sí hace la animación, pero la puerta no se le abre al
// que está manejando el auto."
//
// The other machine's door is opened by its own replica entry and by nothing
// else: a door is swung frame by frame by the entering ped's animation, and no
// packet in this protocol carries an open door. So the announcement has to
// reach the other machine while there is still an entry left to play. It used
// to be sent when the engine finished seating the player - about a second and
// a half late - and by then the replica entry either started against a ped the
// pose stream had already carried into the car, or never ran.
//
// A car within SEAT_RANGE_M of RecSampleLocalPlayer's position, which every
// one of these needs.
void PutACarNextToUs(Client &c) {
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.Tick();
}

void TestTheSeatIsAnnouncedWhenTheWalkStarts() {
	std::printf("\nthe seat goes on the wire when the entry starts\n");
	Client c;
	c.SetBridge(RecordingBridge());
	PutACarNextToUs(c);

	g_rec.wantSeatToggle  = true;
	g_rec.localSeatAnswer = SEAT_LOCAL_WALKING;
	g_rec.localSeatAsked  = 2;
	c.TickSeat();
	Check(g_rec.localSeatCalls == 1, "the engine was asked to walk us to the door");
	Check(g_rec.lastSeatHandle >= 0, "for the car the roster named");
	Check(c.PendingSeatNetId() == 80, "the entry is in flight");
	Check(c.LocalSeatNetId() == 80,
	      "and the session has already been told, so the other machines can "
	      "open the same door at the same time");

	// Still walking. Nothing new goes out and nothing is taken back.
	g_rec.localPollAnswer = SEAT_LOCAL_WALKING;
	c.TickSeat();
	c.TickSeat();
	Check(g_rec.localPollCalls == 2, "the entry is driven every tick");
	Check(g_rec.localSeatCalls == 1, "and not started a second time");
	Check(c.LocalSeatNetId() == 80, "the announcement stands while it runs");

	// The engine hands over the seat it was asked for, so there is nothing to
	// correct and no second packet to send.
	g_rec.localPollAnswer  = 2;
	g_rec.localIsPassenger = true;
	c.TickSeat();
	Check(c.PendingSeatNetId() == INVALID_NETID, "the entry is over");
	Check(c.LocalSeatNetId() == 80, "and we are recorded as riding in it");
}

void TestAnEntryThatNeverFinishesTakesItsSeatBack() {
	std::printf("\nan entry that never finished gives the seat back\n");
	Client c;
	c.SetBridge(RecordingBridge());
	PutACarNextToUs(c);

	g_rec.wantSeatToggle  = true;
	g_rec.localSeatAnswer = SEAT_LOCAL_WALKING;
	c.TickSeat();
	Check(c.LocalSeatNetId() == 80, "announced at the start");

	// The car was destroyed under us, or the deadline ran out and the warp
	// behind it could not seat us either.
	g_rec.localPollAnswer  = SEAT_LOCAL_REFUSED;
	g_rec.localIsPassenger = false;
	c.TickSeat();
	Check(c.PendingSeatNetId() == INVALID_NETID, "the entry is over");

	// The retraction is the ordinary exit, on the next tick, because the seat
	// now reads empty. Nothing was invented for this case.
	c.TickSeat();
	Check(c.LocalSeatNetId() == INVALID_NETID,
	      "and the session is told we are not in it after all");
}

void TestASeatThatCameOutDifferentIsCorrected() {
	std::printf("\nthe engine gave a different seat from the one we asked for\n");
	Client c;
	c.SetBridge(RecordingBridge());
	PutACarNextToUs(c);

	// The slot has to be picked before the walk - the engine's entry animates
	// to a door - and a second is long enough for another ped to take it.
	g_rec.wantSeatToggle  = true;
	g_rec.localSeatAnswer = SEAT_LOCAL_WALKING;
	g_rec.localSeatAsked  = 1;
	c.TickSeat();
	Check(c.LocalSeatNetId() == 80, "seat 1 announced");

	g_rec.localPollAnswer  = 3;
	g_rec.localIsPassenger = true;
	c.TickSeat();
	Check(c.LocalSeatNetId() == 80,
	      "still the same car, and the session has been told the real seat");
	Check(c.PendingSeatNetId() == INVALID_NETID, "with nothing left in flight");
}

void TestTheWarpFallbackStillAnnouncesExactlyOnce() {
	std::printf("\nthe warp fallback still announces, and only once\n");
	Client c;
	c.SetBridge(RecordingBridge());
	PutACarNextToUs(c);

	// A car on its roof, or moving: game/seat.cpp warps instead, and hands
	// back a seat on the frame of the press.
	g_rec.wantSeatToggle  = true;
	g_rec.localSeatAnswer = 2;
	c.TickSeat();
	Check(c.PendingSeatNetId() == INVALID_NETID, "there is no walk to drive");
	Check(c.LocalSeatNetId() == 80, "and the session was told straight away");
	Check(g_rec.localPollCalls == 0, "nothing polls an entry that never started");

	// And it is not announced again on the next tick.
	g_rec.localIsPassenger = true;
	c.TickSeat();
	Check(c.LocalSeatNetId() == 80, "still one seat, still the same car");
}

// The other half of the reported bug, and the one that made it look like the
// door simply did not exist.
//
// The server broadcasts an enter back to the sender too - that is how a driver
// learns the netId of the car it claimed. A passenger's copy was being read
// the same way, so m_localVehicleNetId ended up naming a car we are not
// driving; SendLocalVehicle then read "not driving, but we have a car", which
// is its definition of having just got out, and sent C_ExitVehicle for the car
// we had got into one tick earlier. Every other machine took us straight back
// out of the seat, about forty milliseconds in - before the door had moved.
void TestOurOwnPassengerSeatIsNotReadAsACarWeDrive() {
	std::printf("\nour own passenger seat is not a car we are driving\n");
	Client c;
	c.SetBridge(RecordingBridge());
	PutACarNextToUs(c);

	g_rec.wantSeatToggle  = true;
	g_rec.localSeatAnswer = SEAT_LOCAL_WALKING;
	g_rec.localSeatAsked  = 2;
	c.TickSeat();
	Check(c.LocalSeatNetId() == 80, "we said we were getting in");

	// It comes back off the server, addressed to us, as every enter does.
	S_EnterVehicle mine;
	InitHeader(mine, 1000);
	mine.playerId   = 0;   // our own id, from MakeWelcome(0)
	mine.body       = EnterVehicleBody{};
	mine.body.netId = 80;
	mine.body.seat  = 2;
	c.HandleMessage(Wrap(mine, CH_EVENT));
	Check(c.LocalVehicleNetId() == INVALID_NETID,
	      "a seat beside the driver is not a car of ours to report");

	// Which is what stops the spurious exit: with no car of ours, there is
	// nothing for SendLocalVehicle to say we got out of.
	g_rec.drivingLocally = false;
	c.TickLocalVehicle();
	Check(c.LocalSeatNetId() == 80, "so the seat survives the next tick");

	// The driver's seat still works exactly as it did.
	S_EnterVehicle driving;
	InitHeader(driving, 1100);
	driving.playerId   = 0;
	driving.body       = EnterVehicleBody{};
	driving.body.netId = 81;
	driving.body.seat  = 0;
	c.HandleMessage(Wrap(driving, CH_EVENT));
	Check(c.LocalVehicleNetId() == 81, "the wheel is still a claim we answer");
}

void TestExitAnimationIsNotStartedOnFoot() {
	std::printf("\na get-out animation for somebody who is not in a car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);

	// A stale or nonsense pedState off the wire must not reach the engine as
	// an instruction. Nothing in the roster says she is in a car, so there
	// is nothing to get out of.
	S_PlayerState leaving = MakeState(1, 3000, 5.0f);
	leaving.body.pedState = WIRE_PEDSTATE_EXIT_CAR;
	c.HandleMessage(Wrap(leaving, CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.exitAnimBegins == 0, "nothing is asked of the engine");
}

void TestLeavingWhileSeated() {
	std::printf("\nleaving the session from the driver's seat\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();

	S_PlayerLeave leave;
	InitHeader(leave, 2000);
	leave.playerId = 1;
	leave.reason   = LEAVE_QUIT;
	c.HandleMessage(Wrap(leave, CH_EVENT));
	Check(g_rec.unseats == 1, "she is taken out of the car before her ped goes");
	Check(g_rec.despawns == 1, "and then the ped is destroyed");
	Check(c.VehicleCount() == 1, "the car she was driving stays parked");
}

void TestVehicleSpawnNeedsAModel() {
	std::printf("\nvehicle spawn waits for the streamer\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	c.HandleMessage(Wrap(MakeVehicleSpawn(50), CH_EVENT));
	Check(c.VehicleCount() == 1, "the spawn packet registers the vehicle");
	Check(g_rec.modelRequests.size() == 1, "and asks the streamer for its model");

	g_rec.modelReady = false;
	c.Tick();
	Check(g_rec.vehicleSpawns == 0, "nothing is created while the model loads");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "created once the model is ready");
	Check(c.VehicleByNetId(50) != nullptr, "and it is findable by netId");
	Check(c.VehicleByNetId(50)->poolHandle >= 0, "with a pool handle");
	Check(c.VehicleByNetId(50)->colour1 == 3 && c.VehicleByNetId(50)->colour2 == 7,
	      "colours come from the spawn, not from the model's random pick");

	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "not created twice");
}

void TestVehicleSpawnCarriesItsOwnPosition() {
	std::printf("\na vehicle spawn is enough to place it\n");
	// Unlike a player join, S_VehicleSpawn carries pos and rot, so there's no
	// window where the vehicle exists without a position. That window is
	// what drowned the first remote peds - see TestNoSpawnWithoutAPosition.
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;

	c.HandleMessage(Wrap(MakeVehicleSpawn(51, 90, 123.0f), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "one tick is enough, no snapshot needed");
	Check(c.VehicleByNetId(51)->last.pos.x == 123.0f,
	      "and it is placed where the spawn said");
	Check(c.VehicleByNetId(51)->haveState,
	      "the spawn counts as knowing where it is");
}

void TestVehicleStateBeforeSpawn() {
	std::printf("\nvehicle snapshot before its spawn\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;

	// A snapshot carries no model, so there's nothing to create from it. It
	// must not invent a vehicle, same as a stray player snapshot must not
	// invent a player.
	c.HandleMessage(Wrap(MakeVehicleState(1, 60, 5.0f), CH_SNAPSHOT));
	Check(c.VehicleCount() == 0, "a snapshot never creates a vehicle");
	c.Tick();
	Check(g_rec.vehicleSpawns == 0, "and nothing reaches the bridge");
}

void TestVehicleStateApplied() {
	std::printf("\nvehicle state reaches the bridge\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(52), CH_EVENT));
	c.Tick();

	c.HandleMessage(Wrap(MakeVehicleState(1, 52, 77.0f), CH_SNAPSHOT));
	const int before = g_rec.vehicleApplies;
	c.Tick();
	Check(g_rec.vehicleApplies > before, "the newest state is written through");
	Check(g_rec.lastVehicleBody.pos.x == 77.0f, "with the position it carried");
	// Not the driver on purpose - a snapshot says where a car is, not who's
	// in it. Enter and exit are reliable and ordered; this is neither, and a
	// snapshot that overtook an exit would seat somebody who'd just got out.
	Check(c.VehicleByNetId(52)->driverPlayerId == 0xFF,
	      "but a snapshot does not decide who is driving");
}

void TestVehicleCorrectedEveryFrame() {
	std::printf("\nthe correction runs per frame, not per snapshot\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(90), CH_EVENT));
	c.Tick();

	// Snapshots arrive at 25 Hz and frames happen at 60. If the correction
	// only ran when a snapshot arrived, local physics would own the car for
	// two frames out of every three - which is what made the first synced
	// car sit at an angle throwing sparks.
	const int before = g_rec.vehicleCorrections;
	for (int i = 0; i < 5; ++i)
		c.Tick();
	Check(g_rec.vehicleCorrections == before + 5,
	      "five frames with no new snapshot still correct five times");

	// And with no snapshots at all, it holds the spawn position instead of
	// letting gravity walk the car down the street.
	Check(g_rec.lastCorrection.pos.x == 10.0f,
	      "held at where the spawn put it");
}

void TestVehicleCorrectionStopsWithTheVehicle() {
	std::printf("\nno vehicle, no correction\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(91), CH_EVENT));
	c.Tick();

	S_VehicleDespawn d;
	InitHeader(d, 2000);
	d.netId = 91;
	c.HandleMessage(Wrap(d, CH_EVENT));

	const int before = g_rec.vehicleCorrections;
	c.Tick();
	c.Tick();
	Check(g_rec.vehicleCorrections == before,
	      "a despawned vehicle is not corrected into existence");
}

// ---- the horn (game/horn.h) --------------------------------------------------
//
// The engine half is three writes in vehicle.cpp and none of it runs here.
// What runs here is every decision that leads to them: which value a replica
// is given, when a snapshot carries the bit, and when a row says to sound it.

// The rhythm table is the engine's, and the one fact the replay rests on is a
// column of it. If somebody "fixes" HORN_REPLAY_TIMER to the 1 the sender has,
// this is the test that says why that is silence.
void TestHornReplayValueSoundsInEveryRhythm() {
	std::printf("\nthe horn: which timer value a replica can be given\n");
	using namespace game;

	bool all = true;
	for (int p = 0; p < HORN_PATTERNS; ++p)
		all = all && ReplicaHornAudible(static_cast<uint8_t>(p), HORN_REPLAY_TIMER);
	Check(all, "42 sounds in all eight rhythms, whichever the replica holds");

	bool anyAtOne = false;
	for (int p = 0; p < HORN_PATTERNS; ++p)
		anyAtOne = anyAtOne || ReplicaHornAudible(static_cast<uint8_t>(p), 1);
	Check(!anyAtOne, "the honker's own 1 is silent in every rhythm on a replica");

	bool anyAt44 = false;
	for (int p = 0; p < HORN_PATTERNS; ++p)
		anyAt44 = anyAt44 || ReplicaHornAudible(static_cast<uint8_t>(p), 44);
	Check(!anyAt44, "and so is 44, the value that re-picks the rhythm");
	Check(!ReplicaHornAudible(0, 45) && !ReplicaHornAudible(0, 255),
	      "above 44 the audio clamps to 44, so still silent");
	Check(!ReplicaHornAudible(0, 0), "a zero timer is no horn");
	Check(!ReplicaHornAudible(HORN_PATTERNS, HORN_REPLAY_TIMER),
	      "a rhythm index past the table is refused rather than read");

	// Spot checks against the bytes at 0x00606AB8, one per row, chosen where
	// the rows differ from each other.
	Check(HORN_PATTERN_TABLE[0][17] == 0 && HORN_PATTERN_TABLE[1][17] == 1,
	      "rows 0 and 1 part at column 17");
	Check(HORN_PATTERN_TABLE[2][12] == 0 && HORN_PATTERN_TABLE[3][7] == 0,
	      "rows 2 and 3 have their first gaps where the image has them");
	Check(HORN_PATTERN_TABLE[4][10] == 1 && HORN_PATTERN_TABLE[4][11] == 0,
	      "row 4 is one long blast, ending at column 10");
	Check(HORN_PATTERN_TABLE[5][5] == 0 && HORN_PATTERN_TABLE[5][8] == 1,
	      "row 5 is two short ones");
	Check(HORN_PATTERN_TABLE[6][41] == 1 && HORN_PATTERN_TABLE[7][38] == 1 &&
	          HORN_PATTERN_TABLE[7][39] == 0,
	      "rows 6 and 7 end where the image says");
}

// The sending end. The bit rides an unreliable snapshot, so a tap only one
// snapshot saw is sent twice - lose either and it still lands.
void TestHornRidesOneSnapshotPastTheEnd() {
	std::printf("\nthe horn: what the snapshot carries\n");
	using game::HornOnWire;
	using game::HornTail;

	HornTail t;
	Check(!HornOnWire(false, t) && !HornOnWire(false, t),
	      "no horn, no bit");

	const bool tap1 = HornOnWire(true, t);
	const bool tap2 = HornOnWire(false, t);
	const bool tap3 = HornOnWire(false, t);
	Check(tap1 && tap2 && !tap3,
	      "a tap one snapshot saw goes out in two, and then stops");

	HornTail held;
	bool     allHeld = true;
	for (int i = 0; i < 10; ++i)
		allHeld = allHeld && HornOnWire(true, held);
	Check(allHeld, "a held horn is in every snapshot while it is held");
	Check(HornOnWire(false, held), "and in one more after it is let go");
	Check(!HornOnWire(false, held), "and then not");

	HornTail again;
	HornOnWire(true, again);
	HornOnWire(false, again);   // the tail
	Check(HornOnWire(true, again) && HornOnWire(false, again) &&
	          !HornOnWire(false, again),
	      "a second tap during the tail starts its own tail");
}

// The receiving end, as a pure decision.
void TestReplicaHornDecision() {
	std::printf("\nthe horn: when a replica sounds it\n");
	using game::ReplicaHornSounds;
	using game::HORN_FRESH_MS;

	const uint32_t at = 50000;
	Check(ReplicaHornSounds(VEH_HORN, true, false, at, at + 10),
	      "a fresh snapshot with the bit, somebody driving: honk");
	Check(!ReplicaHornSounds(VEH_ENGINE_ON | VEH_LIGHTS, true, false, at, at + 10),
	      "no bit, no honk");
	Check(!ReplicaHornSounds(VEH_HORN, false, false, at, at + 10),
	      "nobody at the wheel cannot be honking, whatever the snapshot said");
	Check(!ReplicaHornSounds(VEH_HORN, true, true, at, at + 10),
	      "a wreck does not honk");
	Check(!ReplicaHornSounds(VEH_HORN, true, false, 0, at),
	      "no snapshot yet - flags from a spawn - is never a honk");
	Check(ReplicaHornSounds(VEH_HORN, true, false, at, at + HORN_FRESH_MS),
	      "still believed at the freshness limit");
	Check(!ReplicaHornSounds(VEH_HORN, true, false, at, at + HORN_FRESH_MS + 1),
	      "and not a millisecond past it, so a driver who stops sending "
	      "stops honking");
	Check(ReplicaHornSounds(VEH_HORN, true, false, 0xFFFFFFF0u, 0x00000010u),
	      "across the clock's wrap, 32 ms is 32 ms");
	Check(!ReplicaHornSounds(VEH_HORN, true, false, 0xFFFFFF00u, 0x00000010u),
	      "and 272 ms is stale");
}

// And through the client, which is where the row is stamped and the decision
// is made on the frame it is carried out.
void TestReplicaHornThroughTheClient() {
	std::printf("\nthe horn: through the client, frame by frame\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	g_rec.modelReady = true;

	// The server's backfill remembers the last snapshot's flags, horn and
	// all. A joiner must not be greeted by that.
	S_VehicleSpawn spawn = MakeVehicleSpawn(80);
	spawn.flags          = VEH_ENGINE_ON | VEH_HORN;
	c.HandleMessage(Wrap(spawn, CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "the car is built");
	Check(!g_rec.lastCorrectionHorn,
	      "a spawn that says horn is not a honk - it is not a snapshot");

	S_VehicleState honk = MakeVehicleState(1, 80, 12.0f);
	honk.body.flags     = VEH_ENGINE_ON | VEH_HORN;
	c.HandleMessage(Wrap(honk, CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastCorrectionHorn, "alice's snapshot says horn: sounded");
	c.Tick();
	c.Tick();
	Check(g_rec.lastCorrectionHorn,
	      "and still sounded on the frames between snapshots - the engine "
	      "zeroes it every frame, so it is re-said every frame");
	Check((g_rec.lastVehicleBody.flags & VEH_HORN) != 0,
	      "the row handed to ApplyRemoteVehicle still carries the bit");

	S_VehicleState quiet = MakeVehicleState(1, 80, 13.0f);
	quiet.hdr.sendTimeMs = 1040;   // not InitHeader, which zeroes the packet
	quiet.body.flags = VEH_ENGINE_ON;
	c.HandleMessage(Wrap(quiet, CH_SNAPSHOT));
	c.Tick();
	Check(c.VehicleByNetId(80)->last.pos.x == 13.0f,
	      "(that snapshot reached the row - the checks below mean something)");
	Check(!g_rec.lastCorrectionHorn, "her next snapshot lets go: silent");

	S_VehicleState honk2 = MakeVehicleState(1, 80, 14.0f);
	honk2.hdr.sendTimeMs = 1080;   // not InitHeader, which zeroes the packet
	honk2.body.flags = VEH_ENGINE_ON | VEH_HORN;
	c.HandleMessage(Wrap(honk2, CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastCorrectionHorn, "honking again");
	c.HandleMessage(Wrap(MakeExit(1, 80), CH_EVENT));
	c.Tick();
	Check(!g_rec.lastCorrectionHorn,
	      "she gets out with the key down: the exit ends it, not a snapshot");

	// And a driver who simply stops sending. The only test here that waits
	// on the real clock, because the row is stamped with it on arrival.
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	S_VehicleState honk3 = MakeVehicleState(1, 80, 15.0f);
	honk3.hdr.sendTimeMs = 1120;   // not InitHeader, which zeroes the packet
	honk3.body.flags = VEH_ENGINE_ON | VEH_HORN;
	c.HandleMessage(Wrap(honk3, CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastCorrectionHorn, "back in, honking");
	std::this_thread::sleep_for(
	    std::chrono::milliseconds(game::HORN_FRESH_MS + 60));
	c.Tick();
	Check(!g_rec.lastCorrectionHorn,
	      "and silent once nothing has arrived for longer than HORN_FRESH_MS");
}

// ---- a car blowing up ------------------------------------------------------
//
// The rule these pin down is docs/protocol.md 1.11: destruction is an event,
// it is decided by the machine driving the car, and an observer replays it
// rather than working it out. None of the engine half is reachable from here,
// so what is pinned is the decision that leads to it - which is the half that
// was wrong, since the old code had no decision at all.

S_VehicleBlowUp MakeVehicleBlowUp(uint16_t netId, uint8_t playerId = 1,
                                  float x = 77.0f) {
	S_VehicleBlowUp b;
	InitHeader(b, 3000);
	b.playerId   = playerId;
	b.body.netId = netId;
	b.body.pos   = {x, 5.0f, 9.0f};
	b.body.rot   = {0.0f, 0.0f, 0.0f, 1.0f};
	return b;
}

void TestVehicleBlowUpIsReplayed() {
	std::printf("\na car blowing up is replayed, not worked out\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(41), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "the car is there to begin with");

	c.HandleMessage(Wrap(MakeVehicleBlowUp(41), CH_EVENT));
	Check(g_rec.blowUps == 1, "the bridge is told to blow it up");
	Check(g_rec.lastBlowUpNetId == 41, "and told which car");
	Check(g_rec.lastBlowUpPos.x == 77.0f,
	      "at the position its owner sent, not wherever we had it");

	const RemoteVehicle *v = c.VehicleByNetId(41);
	Check(v != nullptr && v->destroyed, "the roster knows it is a wreck");
	Check(v != nullptr && v->driverPlayerId == 0xFF, "and that nobody drives it");

	// Twice is once. The event is reliable, but a duplicate must not hand the
	// occupants of that car to the engine a second time.
	c.HandleMessage(Wrap(MakeVehicleBlowUp(41), CH_EVENT));
	Check(g_rec.blowUps == 1, "a second blast for the same car does nothing");
}

void TestVehicleBlowUpForACarWeDoNotHave() {
	std::printf("\na blast for a car we do not have\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// Dropped, not queued and not created from. The packet carries no model,
	// so there is nothing to build, and a blast is not a reason to invent a
	// car - same rule OnVehicleState follows.
	c.HandleMessage(Wrap(MakeVehicleBlowUp(99), CH_EVENT));
	Check(g_rec.blowUps == 0, "nothing reaches the bridge");
	Check(c.VehicleCount() == 0, "and no vehicle is created");
}

void TestWreckIsNeverRespawned() {
	std::printf("\na wreck is never respawned as a new car\n");
	// The engine clears a wreck away by itself a minute after it dies, through
	// the one reaping path that tests neither bIsLocked nor CanBeDeleted
	// (addresses.h). The roster sees the empty pool slot; what it must not do
	// is read that as "build another one".
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(42), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "spawned to begin with");

	c.HandleMessage(Wrap(MakeVehicleBlowUp(42), CH_EVENT));

	// The engine takes the wreck away. ResolveRemoteVehicle does this for
	// real; from here it is the same write it makes.
	RemoteVehicle *v = const_cast<RemoteVehicle *>(c.VehicleByNetId(42));
	Check(v != nullptr, "the entry is still there");
	v->poolHandle = -1;

	for (int i = 0; i < 10; ++i)
		c.Tick();
	Check(g_rec.vehicleSpawns == 1, "and it is not built again");
}

void TestWreckIgnoresLaterSnapshots() {
	std::printf("\na wreck takes no more orders\n");
	// The last snapshots its owner sent were sampled before the blast and are
	// still in flight on an unreliable channel. Applying one would relight a
	// burnt-out car and drag it away from its own explosion.
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(43), CH_EVENT));
	c.Tick();

	c.HandleMessage(Wrap(MakeVehicleBlowUp(43, 1, 77.0f), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleState(1, 43, 500.0f), CH_SNAPSHOT));

	const RemoteVehicle *v = c.VehicleByNetId(43);
	Check(v != nullptr && v->last.pos.x == 77.0f,
	      "the wreck stays where the blast left it");
	Check(v != nullptr && v->last.health == 0.0f, "with no health");
}

void TestBlowUpEmptiesTheCarFirst() {
	std::printf("\neverybody gets out before the car goes up\n");
	// Same ordering TestCarIsEmptiedBeforeItIsDestroyed pins for the despawn,
	// and for a sharper reason here: CAutomobile::BlowUpCar hands every
	// occupant to the engine to destroy on its own schedule, so a ped taken
	// out afterwards is taken out of a car that has already let go of it.
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(44), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 44), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "seated to begin with");

	const int unseatsBefore = g_rec.unseats;
	c.HandleMessage(Wrap(MakeVehicleBlowUp(44), CH_EVENT));
	Check(g_rec.unseats == unseatsBefore + 1, "the rider is taken out");
	Check(g_rec.blowUps == 1, "and then the car goes up");
}

void TestWreckIsStillHeldInPlace() {
	std::printf("\na wreck is still held in place\n");
	// Deliberately still corrected. Its owner has stopped sending, so the
	// interpolator holds it at the blast position - which is the one place
	// both machines agree on. Letting local physics own it instead is how the
	// two wrecks end up in different streets.
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(45), CH_EVENT));
	c.Tick();
	c.HandleMessage(Wrap(MakeVehicleBlowUp(45, 1, 77.0f), CH_EVENT));

	const int before = g_rec.vehicleCorrections;
	c.Tick();
	c.Tick();
	Check(g_rec.vehicleCorrections == before + 2, "still corrected every frame");
	Check(g_rec.lastCorrection.pos.x == 77.0f, "and held where the blast left it");
}

void TestVehicleModelIsAskedForAgainOnRespawn() {
	std::printf("\na lost car asks for its model again\n");
	// This is the half of the reported bug that made it permanent. Nothing
	// else in the game holds a reference to a model only CoopIII's car was
	// using, so once that car leaves the pool the streamer is free to throw
	// the model out - and the respawn loop then waits on an IsModelReady that
	// will never come true again. Reconnecting brought the car back because
	// OnVehicleSpawn was the only place that ever asked.
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(46, 91), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "spawned to begin with");

	// The engine takes it away, and the streamer drops the model with it.
	RemoteVehicle *v = const_cast<RemoteVehicle *>(c.VehicleByNetId(46));
	Check(v != nullptr, "the entry survives");
	v->poolHandle    = -1;
	v->spawnPending  = true;
	g_rec.modelReady = false;
	g_rec.modelRequests.clear();

	c.Tick();
	Check(!g_rec.modelRequests.empty() && g_rec.modelRequests.back() == 91,
	      "the model is asked for again rather than waited on");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.vehicleSpawns == 2, "and the car comes back");
}

// ---- extras ---------------------------------------------------------------
//
// docs/protocol.md §1.12. Nothing about the engine half is reachable from
// here: the fix is a write to CVehicleModelInfo::ms_compsToUse in the two
// instructions before a constructor call. What is reachable, and what was
// missing, is that the identity carries the extras at all and that they are on
// the roster entry before the spawn asks for them - because the spawn is the
// only moment they can be applied.

void TestVehicleSpawnCarriesExtras() {
	std::printf("\na car's extras reach the spawn\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;

	// MakeVehicleSpawn sends 2 and -1: one fitted component and one empty
	// slot, which is the mixed case. Zeroes would pass whether or not the
	// field crossed the wire.
	c.HandleMessage(Wrap(MakeVehicleSpawn(50), CH_EVENT));

	const RemoteVehicle *v = c.VehicleByNetId(50);
	Check(v != nullptr && v->extra1 == 2, "the first extra is on the roster");
	Check(v != nullptr && v->extra2 == -1, "and an empty slot stays empty");

	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "the car is built");
	Check(g_rec.lastSpawnExtra1 == 2 && g_rec.lastSpawnExtra2 == -1,
	      "and the spawn was handed both, since that is its only chance");
}

void TestExtrasAreClampedToTheModel() {
	std::printf("\nan extra the model does not have is refused\n");
	// CVehicleModelInfo::CreateInstance subscripts m_comps[6] with whatever
	// it is given and checks only for -1 (addresses.h, 0x0051FCE5). These two
	// bytes arrive over a socket. This is the same class of bug as the
	// four-entry animation group and the twelve-slot node array, and it is
	// the third time, so the bound is pinned here rather than trusted.
	// Qualified because `using namespace coopiii::game` is further down this
	// file than these tests are.
	using game::ClampVehicleExtra;
	constexpr int8_t NONE = game::VEHICLE_EXTRA_NONE;

	Check(ClampVehicleExtra(0, 3) == 0, "a component the model has is kept");
	Check(ClampVehicleExtra(2, 3) == 2, "and so is the last one");
	Check(ClampVehicleExtra(3, 3) == NONE,
	      "one past the end is not, because there is no bounds check downstream");
	Check(ClampVehicleExtra(100, 3) == NONE, "nor is nonsense");
	Check(ClampVehicleExtra(127, 6) == NONE,
	      "nor is the largest a signed byte can hold");

	Check(ClampVehicleExtra(-1, 3) == NONE,
	      "-1 stays -1: it is the engine's own 'fit nothing'");
	Check(ClampVehicleExtra(-2, 3) == NONE,
	      "and so does -2, which is the *override's* sentinel and must never "
	      "be mistaken for a component");

	Check(ClampVehicleExtra(0, 0) == NONE,
	      "a model with no components gets nothing fitted");
	Check(ClampVehicleExtra(0, -1) == NONE,
	      "and neither does a model that is not loaded at all");

	// Six, and it is not a number from re3: m_comps runs from +0x1DC up to
	// m_numComps at +0x1F4, which is 24 bytes, which is six pointers.
	Check(game::MAX_VEHICLE_COMPS == 6, "m_comps holds six");
	Check(ClampVehicleExtra(6, 100) == NONE,
	      "and a model claiming more than six still cannot index past it");
}

void TestVehicleDespawn() {
	std::printf("\nvehicle despawn\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(53), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "spawned to begin with");

	S_VehicleDespawn d;
	InitHeader(d, 2000);
	d.netId = 53;
	c.HandleMessage(Wrap(d, CH_EVENT));
	Check(g_rec.vehicleDespawns == 1, "the bridge is told to destroy it");
	Check(c.VehicleCount() == 0, "and the slot is freed");
	Check(c.VehicleByNetId(53) == nullptr, "it is no longer findable");
}

void TestDisconnectClearsVehicles() {
	std::printf("\ndisconnect clears vehicles too\n");
	// A mission vehicle has both its deletion gates shut on purpose, so one
	// left behind by a dead session is one nothing in the engine will ever
	// clean up.
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(54), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 1 && c.VehicleCount() == 1, "one car in session one");

	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));   // a new session
	Check(g_rec.vehicleDespawns == 1, "the old car is destroyed");
	Check(c.VehicleCount() == 0, "and the roster is empty");
}

void TestWelcome() {
	std::printf("\nwelcome\n");
	Client c;
	c.SetBridge(RecordingBridge());

	Check(c.LocalPlayerId() == 0xFF, "no local id before a welcome");
	c.HandleMessage(Wrap(MakeWelcome(3), CH_EVENT));
	Check(c.LocalPlayerId() == 3, "welcome assigns the local player id");
	Check(c.RemoteCount() == 0, "a fresh session has no remotes");
}

void TestRejectedWelcome() {
	std::printf("\nrejected welcome\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(2, /*reject*/ 1), CH_EVENT));
	Check(c.LocalPlayerId() == 0xFF,
	      "a rejection does not assign a player id");
}

void TestJoinAndLeave() {
	std::printf("\njoin and leave\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	FeedPosition(c, 1);
	c.HandleMessage(Wrap(MakeJoin(2, "bob"), CH_EVENT));
	Check(c.RemoteCount() == 2, "two remotes joined");
	Check(c.PlayerSlot(1).nick == "alice", "nick is read from the wire");
	Check(c.PlayerSlot(1).netId == 201, "netId is recorded");
	Check(g_rec.modelRequests.size() == 2, "a model is requested per join");
	Check(c.PlayerSlot(1).spawnPending, "spawn is pending until the model loads");

	S_PlayerLeave leave;
	InitHeader(leave, 2000);
	leave.playerId = 1;
	leave.reason   = 0;
	c.HandleMessage(Wrap(leave, CH_EVENT));
	Check(c.RemoteCount() == 1, "leaving removes the remote");
	Check(!c.PlayerSlot(1).active, "the slot is freed");
	Check(c.PlayerSlot(2).active, "the other player is untouched");
}

void TestSelfIsNotARemote() {
	std::printf("\nself is not a remote\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(4), CH_EVENT));

	// The server backfills the roster, which includes us.
	c.HandleMessage(Wrap(MakeJoin(4, "me"), CH_EVENT));
	Check(c.RemoteCount() == 0, "our own join is ignored");

	c.HandleMessage(Wrap(MakeState(4, 1000, 5.0f), CH_SNAPSHOT));
	Check(!c.PlayerSlot(4).haveState, "our own snapshots are ignored");
}

void TestStateBeforeJoin() {
	std::printf("\nsnapshot before join\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// Snapshots are unreliable-sequenced and the join is reliable, so a
	// snapshot can genuinely arrive first. It must not create a ghost.
	c.HandleMessage(Wrap(MakeState(5, 1000, 1.0f), CH_SNAPSHOT));
	Check(c.RemoteCount() == 0, "a snapshot never creates a player");
	Check(!c.PlayerSlot(5).active, "the slot stays empty");
}

void TestTwoPhaseSpawn() {
	std::printf("\ntwo-phase spawn\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	FeedPosition(c, 1);

	g_rec.modelReady = false;
	c.Tick();
	Check(g_rec.spawns == 0, "nothing spawns while the model is still loading");
	Check(c.PlayerSlot(1).poolHandle == -1, "no pool handle yet");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.spawns == 1, "spawns once the model is ready");
	Check(c.PlayerSlot(1).poolHandle == 1, "the bridge's handle is recorded");
	Check(!c.PlayerSlot(1).spawnPending, "spawn is no longer pending");

	c.Tick();
	Check(g_rec.spawns == 1, "does not spawn the same player twice");
}

void TestNoSpawnWithoutAPosition() {
	std::printf("\nspawn needs a position, not just a model\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	// The model is ready but no snapshot has arrived. This is the ordinary
	// case, not a corner: the join is reliable and the first snapshot is
	// unreliable, so there is always a gap.
	g_rec.modelReady = true;
	for (int i = 0; i < 10; ++i)
		c.Tick();
	Check(g_rec.spawns == 0, "a known model is not enough to create a ped");
	Check(c.PlayerSlot(1).poolHandle == -1, "no ped exists yet");
	Check(c.PlayerSlot(1).spawnPending,
	      "the spawn stays pending rather than being abandoned");

	// Cost of getting this wrong, measured in the live game: the ped was
	// born at the world origin, which is water. PED_IDLE with 100 health at
	// frame 14659, PED_DIE at 14667, PED_DEAD at 14727. Everything
	// downstream still looked healthy - it was scanned once per frame,
	// passed the cull zone, had alpha 255 and entered ms_aVisibleEntityPtrs,
	// and drew nothing.
	FeedPosition(c, 1);
	c.Tick();
	Check(g_rec.spawns == 1, "the first snapshot is what releases the spawn");
	Check(c.PlayerSlot(1).poolHandle >= 0, "and the ped exists now");
}

void TestPoseApplied() {
	std::printf("\npose application\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	FeedPosition(c, 1);
	g_rec.modelReady = true;
	c.Tick();

	for (uint32_t i = 0; i <= 8; ++i)
		c.HandleMessage(Wrap(MakeState(1, 1000 + i * 40, float(i)), CH_SNAPSHOT));

	const int before = g_rec.poses;
	c.Tick();
	Check(g_rec.poses > before, "an interpolated pose reaches the bridge");
	Check(g_rec.lastPose.pos.x > 0.0f && g_rec.lastPose.pos.x < 8.0f,
	      "the pose is between the oldest and newest snapshot (interpolated, "
	      "not the raw newest)");
	Check(c.PlayerSlot(1).haveState, "the raw snapshot is kept for the "
	                                 "fields interpolation does not cover");
	Check(c.PlayerSlot(1).last.health == 100.0f, "non-interpolated fields survive");
}

void TestPedLostToTheEngineIsRespawned() {
	std::printf("\na ped the engine destroys\n");
	// GTA III owns the ped pool. Even a MISSION_CHAR ped can go away (a
	// level change, a restart, CGame::ReInitGameObjectVariables), and the
	// bridge detects that through the pool handle rather than assuming its
	// pointer is still good (game/ped.cpp ResolveRemote). What the client
	// owes in return is to notice the cleared handle and create it again,
	// instead of leaving the player invisible for the rest of the session.
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	FeedPosition(c, 1);
	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.spawns == 1, "spawned once to begin with");

	for (uint32_t i = 0; i <= 8; ++i)
		c.HandleMessage(Wrap(MakeState(1, 1000 + i * 40, float(i)), CH_SNAPSHOT));

	g_rec.loseNextPose = true;
	c.Tick();
	Check(c.PlayerSlot(1).poolHandle == -1, "the lost ped's handle is dropped");
	Check(c.PlayerSlot(1).spawnPending, "and the spawn is re-armed");
	Check(g_rec.despawns == 0,
	      "nothing is destroyed: the engine already did that, and destroying "
	      "it again is what corrupted the pool");

	c.Tick();
	Check(g_rec.spawns == 2, "the next frame creates the ped again");
	Check(c.PlayerSlot(1).poolHandle >= 0, "with a fresh handle");
	Check(!c.PlayerSlot(1).spawnPending, "and nothing is left pending");
}

void TestNoBridgeIsSafe() {
	std::printf("\nrunning without a bridge\n");
	// Area B has not landed: CoopIII must still keep an accurate roster
	// rather than crashing on a null function pointer.
	Client c;
	c.SetBridge(WorldBridge{});
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	FeedPosition(c, 1);
	c.HandleMessage(Wrap(MakeState(1, 1000, 1.0f), CH_SNAPSHOT));
	c.Tick();
	Check(c.RemoteCount() == 1, "the roster still works with no bridge");
	Check(c.PlayerSlot(1).poolHandle == -1, "nothing is spawned");
	Check(!c.PlayerSlot(1).spawnPending,
	      "no spawn is left pending when there is nothing to spawn with");
}

void TestWelcomeClearsPreviousSession() {
	std::printf("\nreconnect\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	FeedPosition(c, 1);
	g_rec.modelReady = true;
	c.Tick();
	Check(c.RemoteCount() == 1 && g_rec.spawns == 1, "session one is populated");

	// Reconnecting hands out fresh ids - nothing from the old session is valid.
	c.HandleMessage(Wrap(MakeWelcome(2), CH_EVENT));
	Check(c.RemoteCount() == 0, "a new welcome clears the roster");
	Check(g_rec.despawns == 1, "and despawns the peds it had created");
	Check(c.LocalPlayerId() == 2, "the new local id is taken");
}

// ---- combat ----------------------------------------------------------------
//
// Shots and explosions are the one part of the roster that is *not* a
// reconciliation. Everything else here drives a state until the engine
// agrees; these two happen once, at an instant, and the tests below are
// mostly about what the client refuses to do with them. Retrying a muzzle
// flash is how one bullet becomes a burst.

S_Shot MakeShot(uint8_t playerId, uint8_t weapon, float x = 1.0f) {
	S_Shot s;
	InitHeader(s, 1000);
	s.playerId    = playerId;
	s.body.weapon = weapon;
	s.body.origin = {x, 2.0f, 3.0f};
	s.body.dir    = {0.0f, 1.0f, 0.0f};
	s.body.speed  = 0.25f;
	return s;
}

S_Explosion MakeExplosion(uint8_t playerId, uint8_t type, float x = 40.0f) {
	S_Explosion e;
	InitHeader(e, 1000);
	e.playerId  = playerId;
	e.body.type = type;
	e.body.pos  = {x, 50.0f, 6.0f};
	return e;
}

void TestShotNeedsAPed() {
	std::printf("\na shot is dropped, not queued\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	// Alice has joined but her model is still streaming, so there's no
	// barrel for the bullet to come out of. A shot held until the ped exists
	// would arrive as a flash from a gun that was pointing somewhere else
	// by then.
	c.HandleMessage(Wrap(MakeShot(1, 3 /*uzi*/), CH_EVENT));
	Check(g_rec.shotsReplayed == 0, "nothing is replayed while she has no ped");

	FeedPosition(c, 1);
	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.spawns == 1, "she has a ped now");

	c.Tick();
	c.Tick();
	Check(g_rec.shotsReplayed == 0,
	      "and the shot that arrived early is not replayed late");

	c.HandleMessage(Wrap(MakeShot(1, 3), CH_EVENT));
	Check(g_rec.shotsReplayed == 1, "a shot that arrives with a ped present is replayed");
	Check(g_rec.lastShotPlayer == 1, "for the right player");
	Check(g_rec.lastShot.weapon == 3 && g_rec.lastShot.speed == 0.25f,
	      "with the weapon and the speed the wire carried");
}

void TestShotIsReplayedOncePerPacket() {
	std::printf("\none packet, one shot\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);

	c.HandleMessage(Wrap(MakeShot(1, 2 /*colt45*/), CH_EVENT));
	c.HandleMessage(Wrap(MakeShot(1, 2), CH_EVENT));
	c.HandleMessage(Wrap(MakeShot(1, 2), CH_EVENT));
	Check(g_rec.shotsReplayed == 3, "three packets fire three times");

	// And the frame pump must not fire them again. This is the difference
	// between a shot and a seating - a seating that didn't take gets retried
	// every frame, a shot that already happened is simply over.
	c.Tick();
	c.Tick();
	Check(g_rec.shotsReplayed == 3, "and ticking does not fire any of them again");
}

void TestADriveByRoundReachesTheSeamWhole() {
	std::printf("\na drive-by round goes to the seam like any shot\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);

	// The client does not know which weapons the engine replays and which it
	// only draws - that is combat.cpp's - so 19 is not filtered here, and the
	// trail length riding in `speed` arrives untouched.
	S_Shot s = MakeShot(1, 19 /*uzi drive-by*/);
	s.body.speed = 17.5f;
	c.HandleMessage(Wrap(s, CH_EVENT));
	Check(g_rec.shotsReplayed == 1, "one round, one call");
	Check(g_rec.lastShot.weapon == 19 && g_rec.lastShot.speed == 17.5f,
	      "with the weapon and the trail length the wire carried");
	c.Tick();
	Check(g_rec.shotsReplayed == 1, "and it is not drawn again next frame");
}

void TestOurOwnShotsAreNotReplayed() {
	std::printf("\nour own shots come back and are ignored\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(4), CH_EVENT));

	// The server excludes the sender, but a relay is a relay and the client
	// must not depend on that. Firing our own shot a second time would
	// double every muzzle flash, and for a grenade, throw two.
	c.HandleMessage(Wrap(MakeShot(4, 11 /*grenade*/), CH_EVENT));
	Check(g_rec.shotsReplayed == 0, "our own playerId is refused");

	c.HandleMessage(Wrap(MakeExplosion(4, 0 /*grenade*/), CH_EVENT));
	Check(g_rec.explosions == 0, "and so is our own explosion");
}

void TestExplosionDoesNotNeedAPed() {
	std::printf("\nan explosion carries its own position\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	// The opposite rule from a shot, on purpose. A shot needs a barrel; an
	// explosion is a place. The case this protects is the important one:
	// the thrower was far enough away that their ped never streamed in
	// here, and the fire still has to appear in the right street.
	Check(c.PlayerSlot(1).poolHandle == -1, "alice has no ped");
	c.HandleMessage(Wrap(MakeExplosion(1, 1 /*molotov*/, 123.0f), CH_EVENT));
	Check(g_rec.explosions == 1, "the explosion is played anyway");
	Check(g_rec.lastExplosion.pos.x == 123.0f, "at the position the owner sent");
	Check(g_rec.lastExplosion.type == 1, "with the type the owner sent");
}

void TestCombatFromAStranger() {
	std::printf("\ncombat from a player we have never heard of\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// Same rule as a stray snapshot: an event never invents a player. The
	// join is reliable and would have arrived first if there were one.
	c.HandleMessage(Wrap(MakeShot(5, 2), CH_EVENT));
	c.HandleMessage(Wrap(MakeExplosion(5, 2), CH_EVENT));
	Check(g_rec.shotsReplayed == 0 && g_rec.explosions == 0,
	      "neither reaches the bridge");
	Check(c.RemoteCount() == 0, "and no player is created");

	// An out-of-range slot is a corrupt or hostile packet, not a race.
	c.HandleMessage(Wrap(MakeShot(200, 2), CH_EVENT));
	c.HandleMessage(Wrap(MakeExplosion(200, 2), CH_EVENT));
	Check(g_rec.shotsReplayed == 0 && g_rec.explosions == 0,
	      "and a playerId past the table is refused before it indexes it");
}

void TestLocalCombatIsDrainedNotSampled() {
	std::printf("\nthe local player's own combat events\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// PostFrame needs a live socket, so what's checkable here is the shape of
	// the seam rather than the send: the bridge hands over a queue, the
	// client takes a bounded number per frame, and the queue belongs to the
	// bridge.
	CombatEvent shot;
	shot.kind        = CombatEvent::SHOT;
	shot.shot.weapon = 3;
	for (int i = 0; i < 12; ++i)
		g_rec.localCombat.push_back(shot);

	CombatEvent   scratch[8];
	const uint8_t first = RecDrainLocalCombat(scratch, 8);
	Check(first == 8, "a drain takes at most what it was offered room for");
	Check(g_rec.localCombat.size() == 4, "and leaves the rest for the next frame");

	const uint8_t second = RecDrainLocalCombat(scratch, 8);
	Check(second == 4, "the next drain takes the remainder");
	Check(RecDrainLocalCombat(scratch, 8) == 0, "and then there is nothing left");
}

// ---- damage, death and respawn ---------------------------------------------
//
// The rule these tests exist to pin down is one sentence: the attacker
// decides that a hit happened, the victim decides what it costs. Everything
// here is a way of getting that wrong.

S_Damage MakeDamage(uint8_t attackerId, uint16_t victimNetId, uint8_t weapon = 3,
                    float amount = 25.0f) {
	S_Damage d;
	InitHeader(d, 1000);
	d.attackerId       = attackerId;
	d.body.victimNetId = victimNetId;
	d.body.weapon      = weapon;
	d.body.amount      = amount;
	d.body.piece       = 0;
	d.body.direction   = 2;
	return d;
}

S_Death MakeDeath(uint8_t playerId, uint16_t animId = 13, uint16_t killer = 0) {
	S_Death d;
	InitHeader(d, 1000);
	d.playerId    = playerId;
	d.killerNetId = killer;
	d.animId      = animId;
	return d;
}

S_Respawn MakeRespawn(uint8_t playerId, float x) {
	S_Respawn r;
	InitHeader(r, 1000);
	r.playerId    = playerId;
	r.body.pos    = {x, 500.0f, 10.0f};
	r.body.heading = 1.0f;
	return r;
}

void TestDamageOnlyLandsOnUs() {
	std::printf("\ndamage addressed to somebody else\n");
	Client c;
	c.SetBridge(RecordingBridge());
	// MakeWelcome gives player 0 netId 100.
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	// The server sends an S_Damage to the victim alone, but a relay is a
	// relay and the client must not lean on that. Hurting ourselves because
	// a packet arrived is the one failure the whole design exists to stop.
	c.HandleMessage(Wrap(MakeDamage(1, 999), CH_EVENT));
	Check(g_rec.damages == 0, "a netId that isn't ours is refused");

	c.HandleMessage(Wrap(MakeDamage(1, 100), CH_EVENT));
	Check(g_rec.damages == 1, "a netId that is ours is applied");
	Check(g_rec.damageAttacker == 1, "credited to the attacker we know about");
	Check(g_rec.lastDamage.amount == 25.0f && g_rec.lastDamage.direction == 2,
	      "with the arguments the attacker's own InflictDamage call had");

	// An attacker whose ped never streamed in still hurts. The bridge gets a
	// null attacker and the hit lands anyway.
	c.HandleMessage(Wrap(MakeDamage(5, 100), CH_EVENT));
	Check(g_rec.damages == 2 && g_rec.damageAttacker == 0xFF,
	      "an attacker we have no slot for still lands the hit");
}

void TestDeathKillsTheirPed() {
	std::printf("\na remote player dies\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);

	// The death is recorded by the handler and carried out by the frame, the
	// same way a seating is. In the real client there is no gap - PreFrame
	// drains the inbound queue and then runs UpdateRemotes - but a test can
	// tell them apart, and the split is what lets a death that arrives while
	// the ped is still streaming in survive until there is a ped to apply it
	// to.
	c.HandleMessage(Wrap(MakeDeath(1, 17 /*ANIM_STD_KO_SHOT_FACE*/), CH_EVENT));
	c.Tick();
	Check(g_rec.kills == 1, "her ped is killed");
	Check(g_rec.lastKilled == 1 && g_rec.lastKillAnim == 17,
	      "with the animation her own engine chose");

	// Not twice. A second S_Death for the same life would run SetDie over a
	// ped that has already been through it.
	c.Tick();
	c.Tick();
	Check(g_rec.kills == 1, "and ticking does not kill her again");

	// Our own death comes back off the relay and is ignored: we already died
	// locally, that's where the packet came from.
	c.HandleMessage(Wrap(MakeDeath(0), CH_EVENT));
	Check(g_rec.kills == 1, "our own playerId is refused");
}

void TestDeathTakesThemOutOfTheCarFirst() {
	std::printf("\ndying in a car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);

	c.HandleMessage(Wrap(MakeVehicleSpawn(9), CH_EVENT));
	c.Tick();
	S_EnterVehicle in;
	InitHeader(in, 1000);
	in.playerId    = 1;
	in.body        = EnterVehicleBody{};
	in.body.netId  = 9;
	in.body.seat   = 0;
	c.HandleMessage(Wrap(in, CH_EVENT));
	c.Tick();
	Check(g_rec.seats == 1, "she is in the car");

	// CPed::SetDie's PED_DRIVING arm calls FlagToDestroyWhenNextProcessed on
	// anything that isn't the player ped, and every remote player is a
	// CCivilianPed. Killing a seated one hands it to the engine to delete.
	const int unseatsBefore = g_rec.unseats;
	c.HandleMessage(Wrap(MakeDeath(1), CH_EVENT));
	Check(g_rec.unseats == unseatsBefore + 1, "she comes out of the seat");
	Check(g_rec.kills == 0, "before anything kills her");
	c.Tick();
	Check(g_rec.kills == 1, "and then gets killed");

	// And the standing instruction goes with her, or the reconciliation loop
	// puts the corpse straight back behind the wheel.
	c.Tick();
	c.Tick();
	Check(g_rec.seats == 1, "the corpse is not seated again");
}

void TestRespawnRebuildsThePed() {
	std::printf("\na remote player respawns\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeDeath(1), CH_EVENT));

	c.HandleMessage(Wrap(MakeRespawn(1, 777.0f), CH_EVENT));
	Check(g_rec.despawns == 1, "the corpse is destroyed");
	Check(c.PlayerSlot(1).poolHandle == -1, "and the handle is dropped");
	Check(c.PlayerSlot(1).last.pos.x == 777.0f,
	      "the hospital position seeds the next spawn");
	Check(c.PlayerSlot(1).last.health == 100.0f, "with full health");

	// Nothing until real positions arrive. The old life's snapshots were half
	// a city away, so the buffer was cleared rather than interpolated across.
	const int spawnsBefore = g_rec.spawns;
	c.Tick();
	Check(g_rec.spawns == spawnsBefore, "no ped yet, the buffer is empty");

	FeedPosition(c, 1);
	c.Tick();
	Check(g_rec.spawns == spawnsBefore + 1, "and a fresh ped once they do");
	Check(c.PlayerSlot(1).appliedWeapon == 0xFFFF,
	      "which starts with nothing applied to it");
}

void TestFriendlyFireReachesTheBridge() {
	std::printf("\nthe session's friendly fire setting\n");
	Client c;
	c.SetBridge(RecordingBridge());

	S_Welcome off = MakeWelcome(0);
	off.flags     = 0;
	c.HandleMessage(Wrap(off, CH_EVENT));
	Check(g_rec.friendlyFireCalls == 1 && !g_rec.friendlyFire,
	      "off by default, and the bridge is told");

	// The server enforces it by refusing to relay a C_Damage. The bridge
	// needs to know anyway, because an explosion is replayed locally and
	// never passes through the server to be refused.
	S_Welcome on = MakeWelcome(0);
	on.flags     = SESSION_FRIENDLY_FIRE;
	c.HandleMessage(Wrap(on, CH_EVENT));
	Check(g_rec.friendlyFireCalls == 2 && g_rec.friendlyFire, "and on when it's on");
}

void TestLifeStateMachine() {
	std::printf("\nnoticing our own death and respawn\n");

	// One announcement per death, whichever of the two noticed first. The
	// CPed::SetDie detour usually gets there, and the health poll is what
	// still works when that hook failed to install.
	Check(LifeEventFor(100.0f, false) == LifeEvent::NOTHING, "alive and quiet");
	Check(LifeEventFor(0.0f, false) == LifeEvent::DIED, "health hits zero");
	Check(LifeEventFor(0.0f, true) == LifeEvent::NOTHING,
	      "and is not announced a second time");
	Check(LifeEventFor(100.0f, true) == LifeEvent::RESPAWNED, "health comes back");
	Check(LifeEventFor(100.0f, false) == LifeEvent::NOTHING,
	      "and a respawn is not announced twice either");

	// A NaN read out of a ped we lost counts as dead, not as alive. `>` is
	// false for NaN either way round, so the test is written as a negation.
	Check(LifeEventFor(std::numeric_limits<float>::quiet_NaN(), false) == LifeEvent::DIED,
	      "a NaN health is dead, not alive");

	// Kill credit is recency. Five seconds is the window.
	constexpr uint32_t WINDOW = 5000;
	Check(KillCreditFor(INVALID_NETID, 0, 1000, WINDOW) == INVALID_NETID,
	      "nobody hurt us, nobody gets the kill");
	Check(KillCreditFor(42, 1000, 3000, WINDOW) == 42, "a recent attacker gets it");
	Check(KillCreditFor(42, 1000, 1000 + WINDOW, WINDOW) == 42,
	      "the edge of the window still counts");
	Check(KillCreditFor(42, 1000, 1001 + WINDOW, WINDOW) == INVALID_NETID,
	      "past it, the drowning is our own fault");
	// The wall clock is a uint32 and a long session wraps it. Unsigned
	// subtraction has to read that as a small elapsed time, not a huge one.
	Check(KillCreditFor(42, 0xFFFFF000u, 0x00000100u, WINDOW) == 42,
	      "a clock that wrapped is still a recent attacker");
}

// ---- busted ----------------------------------------------------------------
//
// An arrest never touches health: it stays where it was, and the police
// station hands back 100 like a hospital does. So the health machine above is
// blind to it, and a player who was busted used to reach the police station
// on everybody else's screen as a snapshot that jumped across the map. The ped
// state is what sees it - CCopPed::SetArrestPlayer writes PED_ARRESTED and the
// station's CPlayerPed::SetInitialState writes PED_IDLE over it.

PlayerStateBody LifeSample(float health, uint8_t pedState, float x = 5.0f) {
	PlayerStateBody b{};
	b.pos      = {x, 20.0f, 3.0f};
	b.health   = health;
	b.pedState = pedState;
	return b;
}

void TestAnArrestIsNoticedByThePedState() {
	std::printf("\nnoticing our own arrest\n");
	constexpr uint8_t IDLE     = 1;
	constexpr uint8_t ARRESTED = WIRE_PEDSTATE_ARRESTED;

	Check(LocalLifeEventFor(100.0f, IDLE, false, false) == LifeEvent::NOTHING,
	      "alive and walking about");
	Check(LocalLifeEventFor(100.0f, ARRESTED, false, false) == LifeEvent::BUSTED,
	      "PED_ARRESTED is an arrest, with full health");
	Check(LocalLifeEventFor(100.0f, ARRESTED, false, true) == LifeEvent::NOTHING,
	      "noted once, not on every one of the hundred samples it lasts");
	Check(LocalLifeEventFor(100.0f, IDLE, false, true) == LifeEvent::RESPAWNED,
	      "and leaving it is the police station, which goes out as a respawn");
	Check(LocalLifeEventFor(55.0f, IDLE, false, true) == LifeEvent::RESPAWNED,
	      "whatever health that sample happens to carry");

	// A death beats an arrest. SetArrestPlayer clears m_bCanBeDamaged so it
	// should not happen, but if it did the corpse has to be announced.
	Check(LocalLifeEventFor(0.0f, ARRESTED, false, true) == LifeEvent::DIED,
	      "dying while arrested is a death");
	Check(LocalLifeEventFor(std::numeric_limits<float>::quiet_NaN(), ARRESTED, false,
	                        false) == LifeEvent::DIED,
	      "and a NaN is still dead, not busted");

	// Without an arrest in it, it is exactly the old machine.
	const float   healths[] = {100.0f, 1.0f, 0.0f, -5.0f};
	const uint8_t states[]  = {0, IDLE, 44, 48, 49, 54};
	bool          same      = true;
	for (float h : healths)
		for (uint8_t s : states)
			for (bool announced : {false, true})
				same = same && LocalLifeEventFor(h, s, announced, false) ==
				                   LifeEventFor(h, announced);
	Check(same, "no arrest in it, no difference from the death-only machine");

	// And through the client, one sample at a time.
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	c.TickLocalLifeForTest(LifeSample(100.0f, IDLE));
	Check(!c.ArrestNotedForTest() && !c.DeathAnnouncedForTest(), "nothing to say");

	c.TickLocalLifeForTest(LifeSample(100.0f, ARRESTED));
	Check(c.ArrestNotedForTest(), "busted is noted");
	Check(!c.DeathAnnouncedForTest(), "and is not a death");
	for (int i = 0; i < 100; ++i)
		c.TickLocalLifeForTest(LifeSample(100.0f, ARRESTED));
	Check(c.ArrestNotedForTest(), "and stays noted for the four seconds it lasts");

	c.TickLocalLifeForTest(LifeSample(100.0f, IDLE, 1200.0f));
	Check(!c.ArrestNotedForTest(), "the police station clears it");
	Check(!c.DeathAnnouncedForTest(), "without anybody having died");

	// Busted and then killed: the death takes over, and the flag goes with
	// it so the hospital does not produce a second respawn.
	c.TickLocalLifeForTest(LifeSample(100.0f, ARRESTED));
	c.TickLocalLifeForTest(LifeSample(0.0f, 49));
	Check(c.DeathAnnouncedForTest() && !c.ArrestNotedForTest(),
	      "a death while arrested is a death, and only a death");
	c.TickLocalLifeForTest(LifeSample(100.0f, IDLE));
	Check(!c.DeathAnnouncedForTest() && !c.ArrestNotedForTest(),
	      "and one respawn ends both");

	// A dropped connection forgets it, like it forgets a death.
	c.TickLocalLifeForTest(LifeSample(100.0f, ARRESTED));
	c.ClearRosterForTest();
	Check(!c.ArrestNotedForTest(), "a new session has not been told we were busted");
}

// What everybody else sees at the other end: a player who was never dead
// arriving at the police station. S_Respawn is the same packet a hospital
// sends and it does the same thing, which is the point of using it.
void TestABustedPlayerComesBackAtThePoliceStation() {
	std::printf("\na remote player leaves the police station\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "she was busted sitting in her car");

	// Arrested in the seat with no drag - the door was blocked, or the cop
	// caught her climbing in. The snapshots say PED_ARRESTED and she stays
	// exactly where she is.
	const int unseatsBefore = g_rec.unseats;
	S_PlayerState held      = MakeState(1, 3000, 5.0f);
	held.body.pedState      = WIRE_PEDSTATE_ARRESTED;
	c.HandleMessage(Wrap(held, CH_SNAPSHOT));
	c.Tick();
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "an arrest in the seat keeps her in it");
	Check(g_rec.unseats == unseatsBefore, "nothing takes her out");

	const int despawnsBefore = g_rec.despawns;
	c.HandleMessage(Wrap(MakeRespawn(1, 1200.0f), CH_EVENT));
	Check(!c.PlayerSlot(1).Seated(), "the police station is not in her car");
	Check(g_rec.unseats == unseatsBefore + 1, "she comes out of the seat first");
	Check(g_rec.despawns == despawnsBefore + 1, "and the ped is rebuilt");
	Check(g_rec.kills == 0, "nobody was killed on the way");
	Check(c.PlayerSlot(1).last.pos.x == 1200.0f, "seeded at the police station");

	// Not put back behind the wheel: the respawn dropped the standing
	// instruction, the same as it does for a death.
	const int seatsBefore = g_rec.seats;
	FeedPosition(c, 1);
	c.Tick();
	c.Tick();
	Check(g_rec.seats == seatsBefore, "and not seated again");
}

// A cop at a stopped car drags the driver out, then arrests him. On his own
// screen that is CPed::SetBeingDraggedFromCar and the JACKEDCAR animation. On
// everybody else's it used to be nothing: the replica sat in the seat until
// the S_ExitVehicle at the end of the drag, then stood up beside the car.
void TestADragTakesThemOutOfTheSeat() {
	std::printf("\na remote player dragged out of a car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "in the car");

	const int unseatsBefore = g_rec.unseats;
	const int seatsBefore   = g_rec.seats;
	S_PlayerState dragged   = MakeState(1, 3000, 5.0f);
	dragged.body.pedState   = WIRE_PEDSTATE_DRAG_FROM_CAR;
	c.HandleMessage(Wrap(dragged, CH_SNAPSHOT));
	c.Tick();
	Check(!c.PlayerSlot(1).Seated(), "the seat stands down on the snapshot");
	Check(g_rec.unseats == unseatsBefore + 1, "through the ordinary unseat");
	Check(c.PlayerSlot(1).draggedFromNetId == 80, "remembering which car");
	Check(c.PlayerSlot(1).seatVehicleNetId == 80,
	      "while the session still says she is in it, because nothing has said "
	      "otherwise yet");

	// Halfway out the cop arrests her. Still coming out of the door.
	S_PlayerState busted = MakeState(1, 3040, 5.0f);
	busted.body.pedState = WIRE_PEDSTATE_ARRESTED;
	c.HandleMessage(Wrap(busted, CH_SNAPSHOT));
	c.Tick();
	c.Tick();
	Check(!c.PlayerSlot(1).Seated() && g_rec.seats == seatsBefore,
	      "PED_ARRESTED after a drag does not put her back");

	// The exit lands. The latch has nothing left to do.
	c.HandleMessage(Wrap(MakeExit(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).draggedFromNetId == INVALID_NETID, "the exit ends the latch");
	Check(g_rec.seats == seatsBefore, "and she is on foot, where she landed");
}

void TestTheDragLatch() {
	std::printf("\nwhen a drag keeps somebody out of their seat\n");
	constexpr uint16_t CAR  = 80;
	constexpr uint8_t  IDLE = 1, GETUP = 37;

	Check(DragLatchFor(INVALID_NETID, CAR, WIRE_PEDSTATE_DRIVING) == INVALID_NETID,
	      "sitting in it is not being dragged out of it");
	Check(DragLatchFor(INVALID_NETID, CAR, WIRE_PEDSTATE_ARRESTED) == INVALID_NETID,
	      "busted in the seat is not a drag either");
	Check(DragLatchFor(INVALID_NETID, CAR, WIRE_PEDSTATE_DRAG_FROM_CAR) == CAR,
	      "PED_DRAG_FROM_CAR is");
	Check(DragLatchFor(CAR, CAR, WIRE_PEDSTATE_ARRESTED) == CAR,
	      "held through the arrest that follows it");
	Check(DragLatchFor(CAR, CAR, GETUP) == CAR,
	      "and through a snapshot that beat the exit here");
	Check(DragLatchFor(CAR, INVALID_NETID, IDLE) == INVALID_NETID,
	      "dropped when the session takes the seat away");
	Check(DragLatchFor(CAR, 81, WIRE_PEDSTATE_DRIVING) == INVALID_NETID,
	      "or gives them a different one");
	Check(DragLatchFor(CAR, CAR, WIRE_PEDSTATE_DRIVING) == INVALID_NETID,
	      "or when they report sitting in a car again");
	Check(DragLatchFor(INVALID_NETID, INVALID_NETID, WIRE_PEDSTATE_DRAG_FROM_CAR) ==
	          INVALID_NETID,
	      "no seat, nothing to keep them out of");
}

// ---- animation / weapon / aim decisions -----------------------------------
//
// These are the parts of client/src/game/ped.cpp that are pure arithmetic,
// and therefore testable without GTA III. They're also the parts where a
// mistake is a memory-safety bug rather than a cosmetic one:
// CAnimManager::BlendAnimation indexes its group's association array with
// animId and doesn't bounds-check, and CPed::m_weapons is indexed with a
// weapon type straight off the wire. docs/protocol.md §1.8.1.

using namespace coopiii::game;

// The numbers a real session would produce: ASSOCGRP_STD holds the whole
// AnimationId namespace, a style group holds four locomotion anims.
//
// 173 and not re3's 175, because the retail 1.0 table is shorter than re3's
// enum: CPed::SetDie tests ANIM_STD_NUM as `cmp esi,0ADh`, which is 173. The
// real bound always comes off the group's own numAssociations at runtime
// (ped.cpp AnimGroupCount); this is just a plausible stand-in for it.
constexpr int STD_COUNT   = 173;
constexpr int STYLE_GROUP = 8;   // ASSOCGRP_GANG1
constexpr int STYLE_COUNT = 4;

void TestAnimGroupChoice() {
	std::printf("\nanimation group choice\n");

	// Locomotion plays out of the ped's own style - a gang member walks
	// like a gang member.
	AnimPlan walk = PlanAnim(ANIM_STD_WALK, STYLE_GROUP, STYLE_COUNT, STD_COUNT);
	Check(walk.valid && walk.group == STYLE_GROUP, "walk uses the ped's own group");
	Check(walk.blendDelta == 1.0f, "locomotion crossfades slowly, as SetMoveAnim does");

	AnimPlan idle = PlanAnim(ANIM_STD_IDLE, STYLE_GROUP, STYLE_COUNT, STD_COUNT);
	Check(idle.valid && idle.group == STYLE_GROUP, "idle is the last id a style group has");

	// One past the end of a four-entry group. This is the case that reads
	// out of bounds if it's not caught, and it's one id away from the one
	// above it.
	AnimPlan startWalk = PlanAnim(4, STYLE_GROUP, STYLE_COUNT, STD_COUNT);
	Check(startWalk.valid && startWalk.group == ASSOCGRP_STD,
	      "an id past the style group falls back to ASSOCGRP_STD");

	AnimPlan ko = PlanAnim(13, STYLE_GROUP, STYLE_COUNT, STD_COUNT);
	Check(ko.valid && ko.group == ASSOCGRP_STD, "a knockdown plays out of ASSOCGRP_STD");
	Check(ko.blendDelta == 8.0f, "and blends in fast");

	// A locomotion id the style group does have, but ASSOCGRP_STD would too.
	// The ped's own group still wins.
	AnimPlan run = PlanAnim(ANIM_STD_RUN, STYLE_GROUP, STYLE_COUNT, STD_COUNT);
	Check(run.group == STYLE_GROUP, "the style group is preferred where it can serve");
}

void TestAnimGroupRefusals() {
	std::printf("\nanimation ids that must be refused\n");

	Check(!PlanAnim(ANIM_NONE, STYLE_GROUP, STYLE_COUNT, STD_COUNT).valid,
	      "ANIM_NONE is not an animation");
	Check(!PlanAnim(STD_COUNT, STYLE_GROUP, STYLE_COUNT, STD_COUNT).valid,
	      "one past the end of ASSOCGRP_STD is refused");
	Check(!PlanAnim(60000, STYLE_GROUP, STYLE_COUNT, STD_COUNT).valid,
	      "a nonsense id from a hostile sender is refused");

	// Before CAnimManager::LoadAnimFiles has run, every group is empty, and
	// the client can be connected and applying poses in that window.
	Check(!PlanAnim(ANIM_STD_WALK, STYLE_GROUP, 0, 0).valid,
	      "nothing is playable before the anim files load");
	Check(!PlanAnim(13, STYLE_GROUP, STYLE_COUNT, 0).valid,
	      "an empty ASSOCGRP_STD refuses even with a populated style group");

	// m_animGroup is a field in memory like any other, so it's bounded too.
	Check(PlanAnim(ANIM_STD_WALK, -1, STYLE_COUNT, STD_COUNT).group == ASSOCGRP_STD,
	      "a negative m_animGroup falls back rather than indexing backwards");
	Check(PlanAnim(ANIM_STD_WALK, 9999, STYLE_COUNT, STD_COUNT).group == ASSOCGRP_STD,
	      "an out-of-range m_animGroup falls back too");
	Check(!ValidAnimGroup(NUM_ANIM_ASSOC_GROUPS), "the group count is exclusive");
	Check(ValidAnimGroup(NUM_ANIM_ASSOC_GROUPS - 1), "and the last group is usable");
}

void TestWireValueBounds() {
	std::printf("\nbounds on values that came off the wire\n");

	Check(ClampMoveState(2) == PEDMOVE_WALK, "a normal move state passes through");
	Check(ClampMoveState(PEDMOVE_LAST) == PEDMOVE_SPRINT, "sprint is the last valid one");
	Check(ClampMoveState(200) == PEDMOVE_STILL, "anything past it becomes still");

	Check(IsInventoryWeapon(WEAPONTYPE_UNARMED), "unarmed has a slot");
	Check(IsInventoryWeapon(WEAPONTYPE_LAST_INVENTORY), "so does the last inventory weapon");
	Check(!IsInventoryWeapon(WEAPONTYPE_LAST_INVENTORY + 1),
	      "WEAPONTYPE_LAST_WEAPONTYPE is a damage cause, not a slot");
	Check(!IsInventoryWeapon(255), "and neither is a byte of garbage");

	float out = -1.0f;
	Check(FiniteOr(2.5f, 0.0f, out) && out == 2.5f, "a real float passes through");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(!FiniteOr(nan, 7.0f, out) && out == 7.0f, "NaN is replaced by the fallback");
	const float inf = std::numeric_limits<float>::infinity();
	Check(!FiniteOr(inf, 7.0f, out), "infinity is rejected");
	Check(!FiniteOr(-inf, 7.0f, out), "so is negative infinity");
}

// The weapon whitelist is a decision, and the cost of getting it wrong isn't
// cosmetic. CWeapon::FireSniper reads *this* machine's camera, and
// FireAreaEffect hands the flamethrower to a system that keeps damaging
// after the guard is gone. game/combat.h carries the reason for each refusal.
void TestReplayableWeapons() {
	std::printf("\nwhich weapons an observer may replay\n");

	Check(IsReplayableWeapon(WEAPONTYPE_COLT45), "a pistol is an ordinary instant hit");
	Check(IsReplayableWeapon(WEAPONTYPE_UZI), "so is an uzi");
	Check(IsReplayableWeapon(WEAPONTYPE_SHOTGUN), "the shotgun has its own fire path and is fine");
	Check(IsReplayableWeapon(WEAPONTYPE_AK47) && IsReplayableWeapon(WEAPONTYPE_M16),
	      "both rifles are fine - the M16's first-person arm is gated on "
	      "shooter == FindPlayerPed(), which a remote ped is not");

	Check(!IsReplayableWeapon(WEAPONTYPE_SNIPERRIFLE),
	      "the sniper is refused: FireSniper fires along this machine's camera");
	// The flamethrower used to be refused here, because CShotInfo keeps
	// lighting fires long after the call that made it returned. It is
	// replayed now, and the guard moved to where it can cover that: the
	// culprit rule in CPed::InflictDamage, which holds for as long as the
	// CShotInfo and its fires live. What that rule now lets through is fire
	// and only fire - see TestRemoteDamageToTheLocalPlayer.
	Check(IsReplayableWeapon(WEAPONTYPE_FLAMETHROWER),
	      "the flamethrower is replayed, so the flame comes out");
	Check(!IsReplayableWeapon(WEAPONTYPE_DETONATOR),
	      "the detonator is refused: it sets off bombs nobody synced");
	Check(!IsReplayableWeapon(WEAPONTYPE_UNARMED) &&
	          !IsReplayableWeapon(WEAPONTYPE_BASEBALLBAT),
	      "melee is refused: it's damage, and its animation already rides "
	      "the snapshot");
	Check(!IsReplayableWeapon(13), "the helicannon is not an inventory weapon");
	Check(!IsReplayableWeapon(255), "and neither is a byte of garbage");
}

void TestDamageDecisions() {
	std::printf("\nwhich damage may be forwarded\n");

	// Rays and melee: the attacker's machine is the only one that can answer.
	Check(IsForwardableDamage(WEAPONTYPE_COLT45), "a pistol");
	Check(IsForwardableDamage(WEAPONTYPE_UNARMED), "a fist");
	Check(IsForwardableDamage(WEAPONTYPE_BASEBALLBAT), "a bat");
	Check(IsForwardableDamage(WEAPONTYPE_UZI_DRIVEBY), "a drive-by");

	// The sniper is refused a *replay* because FireSniper reads the
	// observer's own camera, and still forwards its damage, because that was
	// resolved on the shooter's machine like any other bullet. Two different
	// questions.
	Check(IsForwardableDamage(WEAPONTYPE_SNIPERRIFLE) && !IsReplayableWeapon(WEAPONTYPE_SNIPERRIFLE),
	      "a sniper hurts without being replayed");

	// Explosions are already handled, at a fixed world position every
	// machine agrees on. Forwarding them as well would apply them twice.
	Check(!IsForwardableDamage(WEAPONTYPE_GRENADE), "a grenade is not forwarded");
	Check(!IsForwardableDamage(WEAPONTYPE_MOLOTOV), "nor a molotov");
	Check(!IsForwardableDamage(WEAPONTYPE_ROCKETLAUNCHER), "nor a rocket");
	Check(!IsForwardableDamage(WEAPONTYPE_EXPLOSION), "nor a generic blast");

	// And these are the victim's own business, or nobody's.
	Check(!IsForwardableDamage(WEAPONTYPE_FLAMETHROWER),
	      "fire is decided where it burns, not where it was lit");
	Check(!IsForwardableDamage(WEAPONTYPE_RAMMEDBYCAR), "a car is not a decision we get");
	Check(!IsForwardableDamage(WEAPONTYPE_RUNOVERBYCAR), "nor running someone over");
	Check(!IsForwardableDamage(WEAPONTYPE_DROWNING), "drowning happens where you drown");
	Check(!IsForwardableDamage(WEAPONTYPE_FALL), "and so does falling");

	// Both of these steer a switch inside CPed::InflictDamage, and both
	// arrive off a socket.
	Check(IsKnownPedPiece(PEDPIECE_TORSO) && IsKnownPedPiece(PEDPIECE_HEAD),
	      "the seven pieces are accepted");
	Check(!IsKnownPedPiece(PEDPIECE_COUNT) && !IsKnownPedPiece(200),
	      "and nothing past them");
	Check(IsKnownDamageDirection(0) && IsKnownDamageDirection(3), "four directions");
	Check(!IsKnownDamageDirection(4), "and no fifth");
}

// The check that would have caught the crash at 0x004B1B25.
//
// That was a CPtrNode left in CWorld::ms_listMovingEntityPtrs pointing at a
// ped CoopIII had just destroyed, dereferenced by CWorld::Process on the next
// frame. No unit test can walk the engine's own list, so what gets pinned
// here is the decision that leads to it: whether the engine's teardown is
// going to do the unlink, or whether it has to be done by hand first.
//
// Writing this table out is the check. The gate is invisible when you read
// CWorld::Add and CWorld::Remove as a pair, because they look symmetric and
// the asymmetry is in the flag changing between them - and "an entity freed
// while something in the engine still points at it" is now the second crash
// this project has shipped into the game.
void TestMovingListTeardown() {
	std::printf("\nwho unlinks an entity from the moving list\n");

	constexpr uint8_t MOVING = 0;
	constexpr uint8_t STATIC = offs::ENTITY_IS_STATIC;

	// CWorld::Remove is `if (!bIsStatic) RemoveFromMovingList()`, verified
	// byte for byte at 0x004AEA84.
	Check(WorldRemoveUnlinksFromMovingList(MOVING),
	      "a moving entity gets unlinked by the engine");
	Check(!WorldRemoveUnlinksFromMovingList(STATIC),
	      "a static one does not, and that is the whole bug");

	// Other flags in the same byte must not change the answer. bIsStatic is
	// bit 2; bUsesCollision and bIsInSafePosition share the byte and move
	// constantly.
	Check(WorldRemoveUnlinksFromMovingList(offs::ENTITY_USES_COLLISION |
	                                       offs::ENTITY_IS_IN_SAFE_POSITION),
	      "the neighbouring flags in byte A are not read");
	Check(!WorldRemoveUnlinksFromMovingList(STATIC | offs::ENTITY_USES_COLLISION),
	      "and do not mask it either");

	// The only combination that needs a hand: still in the list, and static.
	Check(NeedsMovingListUnlink(STATIC, /*linked=*/true),
	      "static and still linked has to be unlinked by hand");
	Check(!NeedsMovingListUnlink(STATIC, /*linked=*/false),
	      "static and already unlinked needs nothing");
	Check(!NeedsMovingListUnlink(MOVING, /*linked=*/true),
	      "moving and linked is the engine's job");
	Check(!NeedsMovingListUnlink(MOVING, /*linked=*/false), "and so is neither");
}

// The check for the second crash at the same address, which had a register
// dump: `node->item` was 0x0000020E. 526. A small integer sitting where an
// entity pointer belongs, in a node the moving list still held.
//
// CoopIII cannot walk the engine's list from here, and it cannot know why a
// bad node appeared. What it can do is refuse to let CWorld::Process
// dereference one, and the decision that makes that possible is arithmetic.
void TestMovingListNodeSanity() {
	std::printf("\nwhat a moving list node has to look like\n");

	// The value from the crash. This is the whole test.
	Check(!LooksLikeGameObject(0x0000020E), "526 is not a pointer to anything");

	// And the three reasons it isn't, each on its own, because a check that
	// only rejects one observed value is not a check.
	Check(!LooksLikeGameObject(0), "nor is null");
	Check(!LooksLikeGameObject(0x0000FFFC),
	      "nor is anything in the reserved region below 64K");
	Check(!LooksLikeGameObject(0x80000000),
	      "nor is kernel space, which gta3.exe cannot allocate in");
	Check(!LooksLikeGameObject(0x1BA96B6A),
	      "nor is a misaligned address, and every vtabled object is 4-aligned");

	// Real pool addresses from the crash dump's own registers.
	Check(LooksLikeGameObject(0x1BA96B68), "a heap address is accepted");
	Check(LooksLikeGameObject(0x16964EDC), "and so is another one");

	// The second half: an accepted pointer still has to hold a vtable, and
	// every CEntity vtable lives in the exe's read-only data.
	Check(IsImageAddress(CCivilianPed__vtable), "CCivilianPed's vtable is in the image");
	Check(IsImageAddress(CPlaceable__vtable), "and so is CPlaceable's");
	Check(!IsImageAddress(0x1BA96B68), "a heap address is not");
	Check(!IsImageAddress(IMAGE_BASE - 4), "nor is anything below the image");
	Check(!IsImageAddress(IMAGE_BASE + IMAGE_SIZE), "nor one byte past the end of it");

	// Put together: the node from the crash is refused before anything
	// dereferences it, and a healthy one is let through.
	Check(!MovingListNodeIsSane(0x0000020E, CCivilianPed__vtable),
	      "the node that crashed the game is refused");
	Check(!MovingListNodeIsSane(0x1BA96B68, 0x1BA98CC0),
	      "and so is a block that has been recycled for something else");
	Check(MovingListNodeIsSane(0x1BA96B68, CCivilianPed__vtable),
	      "a live ped is let through");
}

// The weapon animation, which is one animation and not three.
//
// The draw, the ready pose, the shot and the recovery are all frames of the
// same association. Which part you see is whether it is running and where it
// loops, and getting that wrong is what made a remote player draw their gun
// over and over without ever firing it.
// The check for the crash at 0x004025D2, which is the same shape of bug as
// the two before it and the first one with arithmetic that matches a
// register dump exactly.
//
// RpAnimBlendClumpUpdateAnimations builds an array of the nodes it is about
// to blend, in a 12-entry local, and neither fills nor terminates it with a
// bound. Past that the array runs into the function's own saved registers,
// its return address and its arguments. The dump had ESI at 0x11, and
// nodes[17] is exactly the clump argument, which the terminator then wrote
// null over.
//
// No unit test can count the associations on a real clump. What it can pin
// is the number, which is the thing that was wrong: re3 declares this array
// as sixteen and the retail build has twelve, so anyone sizing a cap from
// the decompilation would be four over and would not find out until the game
// corrupted its own stack.
void TestAnimClumpLimit() {
	std::printf("\nhow many animations a clump may carry\n");

	// The engine's number, read off `sub esp,40h` with the array at
	// esp+0x10, not off re3's declaration.
	Check(ANIM_UPDATE_NODE_SLOTS == 12, "the node array has twelve slots");
	Check(ANIM_UPDATE_NODE_SLOTS != 16,
	      "and not re3's sixteen, which would be four past the end of the frame");

	// The terminator is written at nodes[count], so a count equal to the
	// slot count already writes one past.
	Check(MAX_CLUMP_ANIM_ASSOCS == ANIM_UPDATE_NODE_SLOTS - 1,
	      "so the last safe count is one below it");

	// And CoopIII keeps back room for the animations the engine adds on its
	// own, which it does every frame without asking.
	Check(MAX_REMOTE_ANIM_ASSOCS < MAX_CLUMP_ANIM_ASSOCS,
	      "CoopIII stops short of the engine's limit");

	Check(AnimClumpHasRoom(0), "an empty clump has room");
	Check(AnimClumpHasRoom(MAX_REMOTE_ANIM_ASSOCS - 1), "and one below the cap");
	Check(!AnimClumpHasRoom(MAX_REMOTE_ANIM_ASSOCS), "at the cap it does not");
	Check(!AnimClumpHasRoom(MAX_CLUMP_ANIM_ASSOCS), "nor at the engine's limit");
	Check(!AnimClumpHasRoom(17),
	      "and nor at seventeen, which is the count that crashed the game");

	// The surplus is how many have to go before one more can be added.
	Check(AnimClumpSurplus(0) == 0, "nothing to drop from an empty clump");
	Check(AnimClumpSurplus(MAX_REMOTE_ANIM_ASSOCS - 1) == 0,
	      "nor with a slot still free");
	Check(AnimClumpSurplus(MAX_REMOTE_ANIM_ASSOCS) == 1, "one over, drop one");
	Check(AnimClumpSurplus(17) == 17 - (MAX_REMOTE_ANIM_ASSOCS - 1),
	      "and seventeen has to lose enough to get back under");
}

// Which count destroys which register, which is the half of the stack map
// that was written down backwards.
//
// "Past twelve is bad" is true and is not enough. The prologue at 0x004024B0
// is `53 56 57 55 83 EC 40` - push ebx, esi, edi, ebp - so ebp is the one
// nearest esp and the saved registers run ebp, edi, esi, ebx from nodes[12]
// up, not ebx, esi, edi, ebp. addresses.h said the latter until 2026-09-22.
//
// It matters because CWorld::Process's walk over the moving list keeps its
// node cursor in edi across the call. Under the old table a fifteenth
// animation was the dangerous one; under the real one it is the fourteenth,
// and the fourteenth is the only count that both faults at 0x004B1B25 and
// leaves ebx alone - which is exactly what the 2026-09-22 dump showed
// (EBX 0xFFFFFFFF, ESI 0, EDI 0, EBP 0, fault reading 0x4C).
void TestAnimNodeArrayOverflowTargets() {
	std::printf("\nwhat an overfull clump writes over\n");

	Check(AnimNodeArrayTarget(0) == AnimNodeArrayOverflow::None,
	      "an empty clump writes its terminator inside the array");
	Check(AnimNodeArrayTarget(MAX_CLUMP_ANIM_ASSOCS) == AnimNodeArrayOverflow::None,
	      "and so does the last safe count, eleven");
	Check(!AnimNodeArrayOverflows(MAX_CLUMP_ANIM_ASSOCS),
	      "eleven does not overflow");
	Check(AnimNodeArrayOverflows(MAX_CLUMP_ANIM_ASSOCS + 1),
	      "twelve does");

	Check(AnimNodeArrayTarget(12) == AnimNodeArrayOverflow::SavedEbp,
	      "twelve lands on saved ebp, which the walk overwrites anyway");
	Check(AnimNodeArrayTarget(13) == AnimNodeArrayOverflow::SavedEdi,
	      "thirteen nils saved edi, which ends walk 1 early and does not crash");
	Check(AnimNodeArrayTarget(14) == AnimNodeArrayOverflow::SavedEsi,
	      "fourteen nils saved esi and hands the walk a CAnimBlendNode as its "
	      "node cursor - this is the crash at 0x004B1B25");
	Check(AnimNodeArrayTarget(15) == AnimNodeArrayOverflow::SavedEbx,
	      "fifteen lands on saved ebx");
	Check(AnimNodeArrayTarget(16) == AnimNodeArrayOverflow::ReturnAddress,
	      "sixteen on the return address");
	Check(AnimNodeArrayTarget(17) == AnimNodeArrayOverflow::ClumpArgument,
	      "and seventeen on the clump argument, which is the 0x004025D2 crash");

	// The mapping is not the old one. Pinned as its own check because the
	// old one is what a reader of this project's notes would expect.
	Check(AnimNodeArrayTarget(12) != AnimNodeArrayOverflow::SavedEbx,
	      "nodes[12] is not saved ebx, whatever addresses.h used to say");
	Check(AnimNodeArrayTarget(15) != AnimNodeArrayOverflow::SavedEbp,
	      "and nodes[15] is not saved ebp");
}

// ---------------------------------------------------------------------------
// The moving-list sweep itself, run for real
// ---------------------------------------------------------------------------
//
// Until now the only headless coverage of this was the two predicates. The
// walk - the splice, the head case, the tail case, the node cap - only ever
// ran inside the game, and the splice is the part that writes into the
// engine's own list. game/movinglist.h exists so it can run here, over a
// list this test builds and then reads back.

struct TestNode {
	uintptr_t item = 0;
	uintptr_t prev = 0;
	uintptr_t next = 0;
};

int g_inspected = 0;

void CountInspected(void *) { ++g_inspected; }

// A stand-in for one CAnimBlendAssociation's list link, so the cost test can
// make the inspector do the work the real one does.
struct FakeAssoc {
	FakeAssoc *next = nullptr;
	float      blend = 0.5f;
};

// A stand-in entity: the sweep only ever reads its first dword, looking for
// something in the image's address range the way a real vtable is.
struct TestEntity {
	uintptr_t  vtable = CCivilianPed__vtable;
	FakeAssoc *anims  = nullptr;
	uint8_t    rest[0x58] = {};
};

int g_fakeAnimTotal = 0;

// Shaped like ClampClumpAnimations: count the associations, decide there is
// nothing to do. The count is what costs; the drop never happens.
void WalkFakeAnims(void *entity) {
	int n = 0;
	for (const FakeAssoc *a = static_cast<TestEntity *>(entity)->anims;
	     a != nullptr && n < 64; a = a->next)
		++n;
	if (AnimNodeArrayOverflows(n))
		++g_fakeAnimTotal;
	g_fakeAnimTotal += n != 0 ? 1 : 0;
}

// Build a doubly-linked list of `n` nodes over `nodes`, each pointing at the
// matching entity, and return the head.
uintptr_t BuildList(TestNode *nodes, TestEntity *ents, int n) {
	for (int i = 0; i < n; ++i) {
		nodes[i].item = reinterpret_cast<uintptr_t>(&ents[i]);
		nodes[i].prev = i == 0 ? 0 : reinterpret_cast<uintptr_t>(&nodes[i - 1]);
		nodes[i].next = i == n - 1 ? 0 : reinterpret_cast<uintptr_t>(&nodes[i + 1]);
	}
	return reinterpret_cast<uintptr_t>(&nodes[0]);
}

int WalkLength(uintptr_t head) {
	int n = 0;
	for (uintptr_t p = head; p != 0 && n < 100000; ++n)
		p = reinterpret_cast<TestNode *>(p)->next;
	return n;
}

void TestMovingListSweep() {
	std::printf("\nsweeping CWorld::ms_listMovingEntityPtrs\n");

	constexpr int N = 8;
	TestNode  nodes[N];
	TestEntity ents[N];

	// A healthy list is left exactly as it was found.
	{
		uintptr_t head = BuildList(nodes, ents, N);
		g_inspected = 0;
		const auto a = SweepMovingList(&head, &CountInspected);
		Check(a.walked == N, "every node is visited");
		Check(a.unlinked == 0, "a healthy list loses nothing");
		Check(!a.faulted && !a.truncated, "and the walk neither faults nor truncates");
		Check(a.inspected == N, "every surviving entity reaches the inspector");
		Check(g_inspected == N, "and the inspector really was called");
		Check(WalkLength(head) == N, "the list is still N long");
	}

	// The crash's own shape: item 0, next 0, i.e. a null entity on the tail.
	// The old PreFrame guard would have caught this one too - it never got
	// the chance, which is why the sweep moved.
	{
		uintptr_t head = BuildList(nodes, ents, N);
		nodes[N - 1].item = 0;
		const auto a = SweepMovingList(&head, nullptr);
		Check(a.unlinked == 1, "a null item on the tail is taken out");
		Check(a.firstBadItem == 0, "and reported as the value it held");
		Check(a.firstBadNode == reinterpret_cast<uintptr_t>(&nodes[N - 1]),
		      "with the node's address, which is the evidence the crash lacked");
		Check(nodes[N - 2].next == 0, "the new tail's next is nil");
		Check(WalkLength(head) == N - 1, "and the list is one shorter");
	}

	// 526, the value from the second crash. It is rejected before anything
	// dereferences it - that ordering is the point.
	{
		uintptr_t head = BuildList(nodes, ents, N);
		nodes[3].item = 0x0000020E;
		const auto a = SweepMovingList(&head, nullptr);
		Check(a.unlinked == 1, "526 is taken out");
		Check(a.firstBadItem == 0x0000020E, "and named in the audit");
		Check(nodes[2].next == reinterpret_cast<uintptr_t>(&nodes[4]),
		      "the node before it now points past it");
		Check(nodes[4].prev == reinterpret_cast<uintptr_t>(&nodes[2]),
		      "and the node after it points back");
		Check(WalkLength(head) == N - 1, "one node gone");
	}

	// The head case, which is the one with no `prev` to write through.
	{
		uintptr_t head = BuildList(nodes, ents, N);
		nodes[0].item = 0;
		const auto a = SweepMovingList(&head, nullptr);
		Check(a.unlinked == 1, "a bad head node is taken out");
		Check(head == reinterpret_cast<uintptr_t>(&nodes[1]),
		      "and the list head itself is moved on");
		Check(nodes[1].prev == 0, "the new head has no prev");
	}

	// Two in a row, because splicing one out must not leave `prev` pointing
	// at a node that has itself just been removed.
	{
		uintptr_t head = BuildList(nodes, ents, N);
		nodes[3].item = 0;
		nodes[4].item = 0x0000020E;
		const auto a = SweepMovingList(&head, nullptr);
		Check(a.unlinked == 2, "both go");
		Check(nodes[2].next == reinterpret_cast<uintptr_t>(&nodes[5]),
		      "and the survivors are joined to each other, not to a removed node");
		Check(nodes[5].prev == reinterpret_cast<uintptr_t>(&nodes[2]),
		      "in both directions");
		Check(WalkLength(head) == N - 2, "two nodes gone");
	}

	// An entity whose block has been recycled for something that is not a
	// game object: the pointer is plausible, the vtable is not.
	{
		uintptr_t head = BuildList(nodes, ents, N);
		ents[2].vtable = 0x1BA98CC0;
		const auto a = SweepMovingList(&head, nullptr);
		Check(a.unlinked == 1, "a recycled block is taken out");
		ents[2].vtable = CCivilianPed__vtable;
	}

	// A circular `next` chain. The cap is not about the game, it is about
	// not turning a crash into a hang on the game thread.
	{
		uintptr_t head = BuildList(nodes, ents, N);
		nodes[N - 1].next = reinterpret_cast<uintptr_t>(&nodes[0]);
		const auto a = SweepMovingList(&head, nullptr);
		Check(a.truncated, "a circular list is reported as truncated");
		Check(a.walked == MOVING_LIST_MAX_NODES, "at the cap and not past it");
		nodes[N - 1].next = 0;
	}
}

// What the sweep costs, on this machine, over a list the size of a real one.
//
// The number is the answer to "may this run once a frame on the game
// thread". It is printed rather than asserted tightly, because a timing
// assertion on a shared machine is a flaky test; the assertion is only that
// it is nowhere near a frame's worth of time.
void TestMovingListSweepCost() {
	std::printf("\nwhat the sweep costs\n");

	constexpr int N     = 400;   // a busy Liberty City frame
	constexpr int ROUNDS = 2000;

	std::vector<TestNode>   nodes(N);
	std::vector<TestEntity> ents(N);
	uintptr_t head = BuildList(nodes.data(), ents.data(), N);

	// Warm the cache the way a real frame would have it warm.
	for (int i = 0; i < 100; ++i)
		SweepMovingList(&head, nullptr);

	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < ROUNDS; ++i)
		SweepMovingList(&head, nullptr);
	const auto t1 = std::chrono::steady_clock::now();

	const double ns =
	    static_cast<double>(
	        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
	    ROUNDS;

	std::printf("  links only:   %d nodes, %.2f us per sweep (%.1f ns per node)\n",
	            N, ns / 1000.0, ns / N);

	// And again with an inspector shaped like the real one: per entity, one
	// indirect call and a walk down a four-link association list, which is
	// what ClampClumpAnimations does to a clump that is nowhere near the
	// limit - i.e. every clump, in every ordinary frame.
	std::vector<FakeAssoc> assocs(N * 4);
	for (int i = 0; i < N; ++i) {
		for (int k = 0; k < 4; ++k)
			assocs[i * 4 + k].next =
			    k == 3 ? nullptr : &assocs[i * 4 + k + 1];
		ents[i].anims = &assocs[i * 4];
	}
	g_fakeAnimTotal = 0;

	for (int i = 0; i < 100; ++i)
		SweepMovingList(&head, &WalkFakeAnims);

	const auto t2 = std::chrono::steady_clock::now();
	for (int i = 0; i < ROUNDS; ++i)
		SweepMovingList(&head, &WalkFakeAnims);
	const auto t3 = std::chrono::steady_clock::now();

	const double nsFull =
	    static_cast<double>(
	        std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count()) /
	    ROUNDS;

	std::printf("  with the anim inspector: %.2f us per sweep (%.1f ns per node)\n",
	            nsFull / 1000.0, nsFull / N);
	std::printf("  a 60 FPS frame is 16667 us, so that is %.4f%% of one\n",
	            (nsFull / 1000.0) / 16667.0 * 100.0);

	Check(g_fakeAnimTotal > 0, "the inspector really ran");
	Check(ns / 1000.0 < 1000.0,
	      "a link-only sweep of 400 nodes is well under a millisecond");
	Check(nsFull / 1000.0 < 1000.0,
	      "and so is one that looks at every clump's animations");
}

// Weapons whose animation does not loop, and the numbers that say so.
//
// Taken from the game's own data\weapon.dat, which CWeaponInfo::LoadWeaponData
// divides by 30 to get seconds. The rocket launcher's row is
// `WEAPON_sniper null 0 99 14 0 175 804`: loop start 0, loop end 99 frames.
void TestNonLoopingWeapons() {
	std::printf("\nweapons whose animation plays once\n");

	// 99 frames is 3.3 seconds, longer than the animation, so the wrap that
	// CPed::FireGun does at m_fAnimLoopEnd never comes. A rocket plays once
	// and fades, and that is what it is supposed to look like.
	constexpr float ROCKET_LOOP_START = 0.0f;
	constexpr float ROCKET_LOOP_END   = 99.0f / 30.0f;
	Check(!WeaponAnimShouldLoop(0.5f, ROCKET_LOOP_START, ROCKET_LOOP_END),
	      "the rocket launcher does not wrap mid-animation");
	Check(!WeaponAnimShouldLoop(3.0f, ROCKET_LOOP_START, ROCKET_LOOP_END),
	      "nor near the end of any real animation");

	// The pistol's row is `WEAPON_hgun_body RBLOCK_Cshoot 8 15 9 9 173 192`,
	// so it wraps half a second in, over and over, which is what holding the
	// trigger looks like.
	constexpr float PISTOL_LOOP_START = 8.0f / 30.0f;
	constexpr float PISTOL_LOOP_END   = 15.0f / 30.0f;
	Check(WeaponAnimShouldLoop(0.55f, PISTOL_LOOP_START, PISTOL_LOOP_END),
	      "the pistol wraps at fifteen frames");
	Check(!WeaponAnimShouldLoop(0.30f, PISTOL_LOOP_START, PISTOL_LOOP_END),
	      "and not before it");

	// The sniper rifle shares WEAPON_sniper with the rocket launcher and has
	// a completely different loop, `0 10 3 0`. Reading the bounds off the
	// weapon rather than off the animation id is what keeps those apart.
	constexpr float SNIPER_LOOP_END = 10.0f / 30.0f;
	Check(WeaponAnimShouldLoop(0.5f, 0.0f, SNIPER_LOOP_END),
	      "the same animation loops for the sniper, which is why the bounds "
	      "come from the weapon and not from the id");
}

void TestWeaponAnimLoop() {
	std::printf("\nthe firing loop of a weapon animation\n");

	// Plausible pistol numbers: the loop is the middle of the animation, the
	// draw is in front of it and the recovery behind.
	constexpr float START = 0.20f;
	constexpr float END   = 0.55f;

	Check(!WeaponAnimShouldLoop(0.10f, START, END), "mid-draw, keep playing");
	Check(!WeaponAnimShouldLoop(START, START, END), "at the loop start, keep playing");
	Check(!WeaponAnimShouldLoop(0.40f, START, END), "mid-shot, keep playing");
	Check(!WeaponAnimShouldLoop(END, START, END), "exactly at the end, not yet");
	Check(WeaponAnimShouldLoop(0.56f, START, END), "past it, wrap back");
	Check(WeaponAnimShouldLoop(2.0f, START, END),
	      "and a frame that overshot badly still wraps");

	// weapon.dat is a file the player can edit, so neither number is
	// trustworthy. A weapon with no usable loop plays straight through, the
	// way a punch or a throw does.
	Check(!WeaponAnimShouldLoop(1.0f, 0.0f, 0.0f), "no loop declared, no loop");
	Check(!WeaponAnimShouldLoop(1.0f, 0.5f, 0.5f), "an empty range is not a loop");
	Check(!WeaponAnimShouldLoop(1.0f, 0.8f, 0.2f), "nor is a backwards one");
	Check(!WeaponAnimShouldLoop(1.0f, -1.0f, 2.0f), "nor one starting before zero");
}

// Which weapon an observer replays, and what a remote player's ped is
// allowed to do to us. The two are related: the flamethrower only came off
// the refused list because the second predicate covers what the first one
// lets loose.
void TestRemoteDamageToTheLocalPlayer() {
	std::printf("\nwhat a remote player's ped may do to us\n");

	// An explosion is replayed at a fixed position everyone agrees on, so
	// this machine answering "was I in it" is answering a question about
	// itself with nothing stale involved. Friendly fire does not appear in
	// the answer: a blast's gate is the bExplosionProof flip around the
	// replay, upstream of this.
	for (bool ff : {false, true}) {
		Check(RemoteMayDamageLocalPlayer(WEAPONTYPE_GRENADE, ff), "a grenade blast may");
		Check(RemoteMayDamageLocalPlayer(WEAPONTYPE_MOLOTOV, ff), "and a molotov's");
		Check(RemoteMayDamageLocalPlayer(WEAPONTYPE_ROCKETLAUNCHER, ff),
		      "and a rocket's");
		Check(RemoteMayDamageLocalPlayer(WEAPONTYPE_EXPLOSION, ff),
		      "and a generic blast");

		// A bullet may not, either way. It arrives as S_Damage, decided by
		// the shooter, or it does not arrive at all.
		Check(!RemoteMayDamageLocalPlayer(WEAPONTYPE_COLT45, ff), "a bullet may not");
		Check(!RemoteMayDamageLocalPlayer(WEAPONTYPE_UNARMED, ff), "nor a fist");
	}

	// Fire, and this is the change. WEAPONTYPE_FLAMETHROWER reaching
	// CPed::InflictDamage is not a shot - the only two producers of it in
	// retail 1.0 are CFire::ProcessFire's own calls - so letting it through
	// admits exactly one thing: a fire that is in this world, at a position
	// this engine computed, burning the player standing in it. The decision
	// is the victim's, about the victim, with nothing interpolated in it.
	Check(RemoteMayDamageLocalPlayer(WEAPONTYPE_FLAMETHROWER, true),
	      "a player's fire burns us when the session allows it");
	Check(!RemoteMayDamageLocalPlayer(WEAPONTYPE_FLAMETHROWER, false),
	      "and does not when friendly fire is off");
	Check(IsFireDamage(WEAPONTYPE_FLAMETHROWER), "that cause means fire");
	Check(!IsFireDamage(WEAPONTYPE_COLT45) && !IsFireDamage(WEAPONTYPE_EXPLOSION),
	      "and nothing else does");

	// The flame itself is replayed, so it is visible. Its damage still never
	// goes on the wire in either direction: there is nobody to send it from
	// when the fire has no source, and sixty packets a second when it does.
	Check(IsReplayableWeapon(WEAPONTYPE_FLAMETHROWER), "the flame comes out");
	Check(!IsForwardableDamage(WEAPONTYPE_FLAMETHROWER),
	      "and its damage is still nobody's to forward");

	// The refusals that did not change.
	Check(!IsReplayableWeapon(WEAPONTYPE_SNIPERRIFLE), "the sniper is still refused");
	Check(!IsReplayableWeapon(WEAPONTYPE_DETONATOR), "and the detonator");
	Check(!IsReplayableWeapon(WEAPONTYPE_UNARMED) &&
	          !IsReplayableWeapon(WEAPONTYPE_BASEBALLBAT),
	      "and melee, which is animation and damage and nothing else");
}

// gFireManager's table, and the walk over it.
//
// The class this covers is the one that has now bitten this project three
// times: driving an engine structure past a bound the engine does not check,
// where re3's constant is not a retail fact. re3 says NUM_FIRES is 40 and so
// does the retail binary - this time - and the point of pinning it here is
// that the next person to change it has to go and look again rather than
// trust either.
//
// No unit test can read gFireManager, so what this pins is the arithmetic:
// forty slots, forty-eight bytes apart, starting one dword into the manager,
// and a walk that touches every one of them exactly once and nothing past
// the last.
void TestFireTable() {
	std::printf("\nthe fire table\n");

	Check(NUM_FIRES == 40, "forty fires, out of three separate loop bounds");
	Check(SIZEOF_CFIRE == 0x30, "forty-eight bytes each, out of the same three");
	Check(FIREMGR_TOTAL == 0 && FIREMGR_FIRES == 4,
	      "m_nTotalFires first, then the array");

	// The fields ProcessFire and Extinguish actually read, in the order the
	// constructor writes them.
	Check(FIRE_ONGOING == 0x00 && FIRE_SCRIPT == 0x01,
	      "the two bytes GetNextFreeFire tests are the first two");
	Check(FIRE_POS == 0x04 && FIRE_ENTITY == 0x10 && FIRE_SOURCE == 0x14,
	      "position, then the entity, then who lit it");
	Check(FIRE_POS + 12 == FIRE_ENTITY, "a CVector's three floats fit between them");

	// The walk. Every address it reads is recorded, so both ends of the
	// array can be checked rather than just the count.
	std::vector<uintptr_t> read;
	const uint32_t         alight = CountOngoingFires([&](uintptr_t at) {
        read.push_back(at);
        // Pretend every third slot is burning, so the count is a real
        // count and not a constant.
        return ((at - gFireManager - FIREMGR_FIRES) / SIZEOF_CFIRE) % 3 == 0;
    });

	Check(read.size() == NUM_FIRES, "the walk visits every slot once");
	Check(alight == 14, "and counts the ones that are alight");
	Check(!read.empty() && read.front() == gFireManager + 4,
	      "starting at the first fire, not at the manager");
	Check(!read.empty() &&
	          read.back() == gFireManager + 4 + (NUM_FIRES - 1) * SIZEOF_CFIRE,
	      "and ending on the fortieth");

	// The one that matters: the last byte touched has to be inside the
	// manager. 4 + 40*48 is 1924, and the last ongoing flag sits at 1876.
	Check(!read.empty() && read.back() - gFireManager <
	                           FIREMGR_FIRES + NUM_FIRES * SIZEOF_CFIRE,
	      "and never past the end of the array");

	// Forty-one would be the bug. Written out because "it is 40" is a claim
	// and "41 would read into whatever follows gFireManager" is the reason
	// the claim had to be measured.
	Check(gFireManager + FIREMGR_FIRES + NUM_FIRES * SIZEOF_CFIRE == 0x008F3954,
	      "the manager ends at 0x008F3954, and slot 40 would start there");
}

void TestDeathAnimChoice() {
	std::printf("\nwhich animation a death plays\n");

	// CPed::SetDie hands its id straight to BlendAnimation against
	// ASSOCGRP_STD with nothing in between, so the bound is the group's own
	// size. 173 is the retail ANIM_STD_NUM, one below re3's.
	Check(PlanDeathAnim(17, STD_COUNT) == 17, "a real id is played as sent");
	Check(PlanDeathAnim(ANIM_NONE, STD_COUNT) == ANIM_STD_KO_FRONT,
	      "a sender with no animation gets the engine's own default");
	Check(PlanDeathAnim(9999, STD_COUNT) == ANIM_STD_KO_FRONT,
	      "and so does an id off the end of the group");
	Check(PlanDeathAnim(ANIM_STD_NUM, STD_COUNT) == ANIM_STD_NUM,
	      "ANIM_STD_NUM means they really died without one");

	// Before the anim files load there is no group to index at all, and the
	// id that means "play nothing" is the only safe answer.
	Check(PlanDeathAnim(17, 0) == ANIM_STD_NUM, "nothing loaded, nothing played");
	Check(PlanDeathAnim(ANIM_NONE, 4) == ANIM_STD_NUM,
	      "a group too small for the default plays nothing rather than reading past it");
}

void TestProjectileWeapons() {
	std::printf("\nwhich weapons put something in the air\n");

	Check(IsProjectileWeapon(WEAPONTYPE_ROCKETLAUNCHER), "a rocket flies");
	Check(IsProjectileWeapon(WEAPONTYPE_MOLOTOV), "a molotov flies");
	Check(IsProjectileWeapon(WEAPONTYPE_GRENADE), "a grenade flies");
	Check(!IsProjectileWeapon(WEAPONTYPE_SHOTGUN), "a shotgun does not");
	Check(!IsProjectileWeapon(WEAPONTYPE_FLAMETHROWER),
	      "and neither does the flamethrower, whatever it looks like - "
	      "CProjectileInfo::AddProjectile calls Error() on it");

	// These three come straight out of CProjectileInfo::RemoveProjectile's
	// switch statement, so a mistake here means a molotov leaving a crater.
	Check(ExplosionTypeForWeapon(WEAPONTYPE_GRENADE) == EXPLOSION_GRENADE, "grenade -> grenade");
	Check(ExplosionTypeForWeapon(WEAPONTYPE_MOLOTOV) == EXPLOSION_MOLOTOV, "molotov -> molotov");
	Check(ExplosionTypeForWeapon(WEAPONTYPE_ROCKETLAUNCHER) == EXPLOSION_ROCKET, "rocket -> rocket");
	Check(ExplosionTypeForWeapon(WEAPONTYPE_UZI) == -1, "a bullet has no explosion");

	// A type byte off the wire is about to be indexed into CExplosion's own
	// array, which has no bounds check of its own.
	Check(IsKnownExplosionType(EXPLOSION_GRENADE), "0 is a type");
	Check(IsKnownExplosionType(EXPLOSION_TYPE_COUNT - 1), "and so is the last one");
	Check(!IsKnownExplosionType(EXPLOSION_TYPE_COUNT), "one past the end is not");
	Check(!IsKnownExplosionType(255), "and neither is a byte of garbage");
}

// ---- where a replayed projectile starts, and which way it points -----------
//
// The rocket that arrived as an explosion and never flew. The whole of it is
// one arm of CProjectileInfo::AddProjectile that ignores its `pos` argument,
// and the whole of the fix is knowing which arm that is, so that is what gets
// pinned here rather than the effect.

void TestProjectileSpawnPoint() {
	std::printf("\nwhich projectiles the engine puts somewhere we did not ask for\n");

	// 0x0055B4A6: `matrix = ped->GetMatrix()`, then straight on to the
	// velocity. The pos argument is never read, so the rocket is born at the
	// thrower's own origin, inside their collision.
	Check(ProjectileSpawnsAtThrower(WEAPONTYPE_ROCKETLAUNCHER),
	      "a rocket from a ped that is not the player starts inside that ped");

	// 0x0055B11C and 0x0055B25B: both add pos into the matrix, field for
	// field. These two were never broken and must not be 'fixed'.
	Check(!ProjectileSpawnsAtThrower(WEAPONTYPE_GRENADE),
	      "a grenade starts where it was thrown from");
	Check(!ProjectileSpawnsAtThrower(WEAPONTYPE_MOLOTOV),
	      "and so does a molotov");
	Check(!ProjectileSpawnsAtThrower(WEAPONTYPE_UZI),
	      "a weapon with no projectile has no spawn point to get wrong");
}

void TestProjectileBasis() {
	std::printf("\nthe matrix rows a projectile flies with\n");

	auto len = [](const Vec3 &v) {
		return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	};
	auto dot = [](const Vec3 &a, const Vec3 &b) {
		return a.x * b.x + a.y * b.y + a.z * b.z;
	};
	auto closeTo = [](float a, float b) { return std::fabs(a - b) < 1.0e-4f; };
	auto sameVec = [&](const Vec3 &a, const Vec3 &b) {
		return closeTo(a.x, b.x) && closeTo(a.y, b.y) && closeTo(a.z, b.z);
	};

	// An orthonormal frame or nothing. A matrix that is neither is what
	// reaches RenderWare's frame and the projectile's own collision box.
	auto orthonormal = [&](const Vec3 &d) {
		Vec3 r{}, f{}, u{};
		if (!ProjectileBasis(d, r, f, u))
			return false;
		return closeTo(len(r), 1.0f) && closeTo(len(f), 1.0f) && closeTo(len(u), 1.0f) &&
		       closeTo(dot(r, f), 0.0f) && closeTo(dot(f, u), 0.0f) && closeTo(dot(u, r), 0.0f);
	};

	Check(orthonormal(Vec3{0.0f, 1.0f, 0.0f}), "due north is an orthonormal frame");
	Check(orthonormal(Vec3{0.6f, -0.3f, 0.2f}), "and so is an arbitrary aim");

	// A rocket launcher points straight up perfectly happily, and that is
	// exactly where Cross(worldUp, dir) collapses to nothing. If the fallback
	// reference is missing this is the case that produces a zero row.
	Check(orthonormal(Vec3{0.0f, 0.0f, 1.0f}), "straight up still has a frame");
	Check(orthonormal(Vec3{0.0f, 0.0f, -1.0f}), "so does straight down");

	Vec3 r{}, f{}, u{};
	Check(ProjectileBasis(Vec3{0.0f, 4.0f, 0.0f}, r, f, u) &&
	          sameVec(f, Vec3{0.0f, 1.0f, 0.0f}),
	      "forward is the direction, normalised - the wire carries dir and speed "
	      "separately and only dir belongs in the matrix");

	// And the velocity goes along that, at a speed held to a bound.
	Vec3 vel{};
	Check(ReplayedProjectileVelocity(f, 1.5f, vel) && sameVec(vel, Vec3{0.0f, 1.5f, 0.0f}),
	      "a projectile flies along the unit forward at the wire's own speed");
	Check(ReplayedProjectileVelocity(f, 1.0e8f, vel) &&
	          sameVec(vel, Vec3{0.0f, REPLAYED_PROJECTILE_MAX_SPEED, 0.0f}),
	      "a speed wrong by orders of magnitude is held to the bound");
	Check(ReplayedProjectileVelocity(f, std::numeric_limits<float>::infinity(), vel) &&
	          sameVec(vel, Vec3{0.0f, REPLAYED_PROJECTILE_MAX_SPEED, 0.0f}),
	      "so is an infinite one");
	Check(!ReplayedProjectileVelocity(f, 0.0f, vel) &&
	          !ReplayedProjectileVelocity(f, std::numeric_limits<float>::quiet_NaN(), vel) &&
	          !ReplayedProjectileVelocity(f, -2.0f, vel),
	      "and no speed, a NaN or a negative one leaves the engine's own");

	// CProjectileInfo::AddProjectile's player arm is
	// `right = CrossProduct(Up, Front)`, and what comes out of here has to
	// obey the same identity or a replayed rocket is mirrored against the
	// one its owner is looking at.
	Check(ProjectileBasis(Vec3{0.3f, 0.5f, -0.8f}, r, f, u) &&
	          sameVec(Vec3{u.y * f.z - u.z * f.y, u.z * f.x - u.x * f.z,
	                       u.x * f.y - u.y * f.x},
	                  r),
	      "right is CrossProduct(up, forward), the engine's own handedness");

	// Everything below is a direction that is not one, and each of them can
	// arrive off a socket. A NaN row in an entity matrix does not fault where
	// it is written; it faults in collision, a frame or two later.
	Check(!ProjectileBasis(Vec3{0.0f, 0.0f, 0.0f}, r, f, u),
	      "a zero direction is refused rather than normalised by zero");

	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	Check(!ProjectileBasis(Vec3{nan, 0.0f, 0.0f}, r, f, u), "a NaN is refused");
	Check(!ProjectileBasis(Vec3{0.0f, inf, 0.0f}, r, f, u), "an infinity is refused");
	Check(!ProjectileBasis(Vec3{1.0e-9f, 0.0f, 0.0f}, r, f, u),
	      "and so is a direction too short to normalise without exploding");
}

// ---- aiming somebody else's shot --------------------------------------------

namespace aim {

float Len(const Vec3 &v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
float Dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
bool  Close(float a, float b, float eps = 1.0e-4f) { return std::fabs(a - b) < eps; }
bool  SameVec(const Vec3 &a, const Vec3 &b, float eps = 1.0e-4f) {
	 return Close(a.x, b.x, eps) && Close(a.y, b.y, eps) && Close(a.z, b.z, eps);
}

} // namespace aim

void TestInstantHitWeapons() {
	std::printf("\nwhich shots are a line the engine traces\n");

	// CWeapon::Fire's jump table, arms 2/3/5 (FireInstantHit), 4 (FireShotgun)
	// and 6 (M16). These five are the ones whose direction the replay can
	// steer, because they are the ones that reach CWeapon::DoDoomAiming.
	Check(IsInstantHitWeapon(WEAPONTYPE_COLT45) && IsInstantHitWeapon(WEAPONTYPE_UZI) &&
	          IsInstantHitWeapon(WEAPONTYPE_AK47) && IsInstantHitWeapon(WEAPONTYPE_M16),
	      "the four ordinary guns trace a ray");
	Check(IsInstantHitWeapon(WEAPONTYPE_SHOTGUN),
	      "and so does the shotgun, five times per trigger pull");

	// Each of these would be a log line about a shot that has no line, every
	// time somebody fired one.
	Check(!IsInstantHitWeapon(WEAPONTYPE_FLAMETHROWER),
	      "the flamethrower traces nothing - CShotInfo is a volume, not a line");
	Check(!IsInstantHitWeapon(WEAPONTYPE_ROCKETLAUNCHER) &&
	          !IsInstantHitWeapon(WEAPONTYPE_GRENADE) &&
	          !IsInstantHitWeapon(WEAPONTYPE_MOLOTOV),
	      "a projectile is an entity with a velocity, not a ray");
	Check(!IsInstantHitWeapon(WEAPONTYPE_SNIPERRIFLE),
	      "the sniper traces one and is never replayed, so it is left out on purpose");
	Check(!IsInstantHitWeapon(WEAPONTYPE_UNARMED) &&
	          !IsInstantHitWeapon(WEAPONTYPE_BASEBALLBAT) &&
	          !IsInstantHitWeapon(255),
	      "melee and garbage trace nothing");
}

void TestFlatHeadingDirection() {
	std::printf("\nthe direction the engine derives from a ped's matrix\n");

	// FireInstantHit does heading = Atan2(-fwd.x, fwd.y) and then uses
	// (-Sin(heading), Cos(heading)), which is (fwd.x, fwd.y) normalised in 2D.
	// If this disagrees with the engine, the rotation below turns the shot
	// from the wrong starting point and the trail lands somewhere new rather
	// than somewhere right.
	Vec3 out{};
	Check(FlatHeadingDirection(Vec3{0.0f, 1.0f, 0.0f}, out) &&
	          aim::SameVec(out, Vec3{0.0f, 1.0f, 0.0f}),
	      "due north stays due north");
	Check(FlatHeadingDirection(Vec3{3.0f, 4.0f, 0.0f}, out) &&
	          aim::SameVec(out, Vec3{0.6f, 0.8f, 0.0f}),
	      "and an unnormalised row comes back unit");

	// A ped leaning on a slope still shoots along its heading: the engine
	// throws the z away and so must this.
	Check(FlatHeadingDirection(Vec3{0.0f, 0.8f, 0.6f}, out) &&
	          aim::SameVec(out, Vec3{0.0f, 1.0f, 0.0f}),
	      "the forward row's z is dropped, not projected");

	Check(!FlatHeadingDirection(Vec3{0.0f, 0.0f, 1.0f}, out),
	      "a forward with no horizontal component is refused rather than divided by zero");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(!FlatHeadingDirection(Vec3{nan, 1.0f, 0.0f}, out), "and so is a NaN");
}

void TestRotateOnto() {
	std::printf("\nturning the engine's proposal onto the shooter's own line\n");

	Vec3 out{};

	// The single-bullet case, which is the whole fix: the engine proposes a
	// flat ray along the ped's heading, at the weapon's range, and what comes
	// back has to be the same length along the wire's direction. 30 m is a
	// pistol's range.
	const Vec3 nominal{0.0f, 1.0f, 0.0f};
	const Vec3 wire{0.0f, 0.8f, 0.6f};   // aimed up at a rooftop
	Check(RotateOnto(nominal, wire, Vec3{0.0f, 30.0f, 0.0f}, out) &&
	          aim::SameVec(out, Vec3{0.0f, 24.0f, 18.0f}, 1.0e-3f),
	      "a flat 30 m ray becomes a 30 m ray along the aim, pitch and all");
	Check(aim::Close(aim::Len(out), 30.0f, 1.0e-3f), "and it is still 30 m long");

	// Length is the thing that must never change: it is the weapon's range,
	// which the engine worked out from its own CWeaponInfo and which an
	// observer has no business adjusting.
	for (const Vec3 &d : {Vec3{1.0f, 0.0f, 0.0f}, Vec3{-0.3f, 0.5f, 0.8f},
	                      Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, -1.0f}}) {
		Vec3 turned{};
		Check(RotateOnto(nominal, d, Vec3{0.0f, 40.0f, 0.0f}, turned) &&
		          aim::Close(aim::Len(turned), 40.0f, 1.0e-3f),
		      "a rotation preserves the ray's length whatever it is turned onto");
	}

	// The shotgun. Five pellets, 7.5 degrees apart, built as the heading plus
	// an offset - so the rotation has to move the cone and keep the spread. A
	// replacement instead of a rotation would stack all five on one another,
	// and a shotgun that fires a single pellet is a visible regression on the
	// machine that did not fire it.
	{
		const float deg = 7.5f * 3.14159265f / 180.0f;
		const Vec3  pellet{std::sin(deg) * 30.0f, std::cos(deg) * 30.0f, 0.0f};
		Vec3        centre{}, edge{};
		Check(RotateOnto(nominal, wire, Vec3{0.0f, 30.0f, 0.0f}, centre) &&
		          RotateOnto(nominal, wire, pellet, edge),
		      "both the middle pellet and an outer one turn");

		Vec3 uc{}, ue{};
		Check(UnitDirection(centre, uc) && UnitDirection(edge, ue), "both are directions");
		const float apart = std::acos(aim::Dot(uc, ue)) * 57.2957795f;
		Check(aim::Close(apart, 7.5f, 0.05f),
		      "and they are still 7.5 degrees apart afterwards - the spread survives");
	}

	// Nothing to turn. The engine's proposal is already right, which is what
	// happens when the shooter was aiming along their own heading.
	Check(RotateOnto(nominal, nominal, Vec3{0.0f, 30.0f, 0.0f}, out) &&
	          aim::SameVec(out, Vec3{0.0f, 30.0f, 0.0f}),
	      "a shot already on the right line is left alone");

	// The undefined case: every axis perpendicular to `from` is a valid
	// rotation and they give different answers, so the vector is laid along
	// `to` at its own length instead of picking one at random. Losing a
	// shotgun's spread for one shot beats losing its direction.
	Check(RotateOnto(nominal, Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, 30.0f, 0.0f}, out) &&
	          aim::SameVec(out, Vec3{0.0f, -30.0f, 0.0f}, 1.0e-3f),
	      "a shot fired exactly backwards still comes out backwards at full length");

	// Everything below arrives off a socket or out of an engine struct, and
	// the result of any of them is a target position, which becomes a
	// subscript into CWorld::ms_aSectors.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	Check(!RotateOnto(Vec3{0.0f, 0.0f, 0.0f}, wire, Vec3{0.0f, 30.0f, 0.0f}, out),
	      "a zero nominal is refused");
	Check(!RotateOnto(nominal, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 30.0f, 0.0f}, out),
	      "a zero wire direction is refused");
	Check(!RotateOnto(nominal, Vec3{nan, 0.0f, 0.0f}, Vec3{0.0f, 30.0f, 0.0f}, out),
	      "a NaN direction is refused");
	Check(!RotateOnto(nominal, wire, Vec3{0.0f, inf, 0.0f}, out),
	      "and so is an infinite proposal from the engine");
	Check(!RotateOnto(nominal, wire, Vec3{nan, 0.0f, 0.0f}, out),
	      "and a NaN one");
}

void TestShotDirectionIsAveraged() {
	std::printf("\nthe direction a discharge puts on the wire\n");

	// What the sampler does: sum the unit direction of every ray this
	// discharge traced, then normalise. Unit, because a shotgun's five rays
	// are each truncated at whatever they hit - summing the raw vectors would
	// weight the pellet that happened to fly furthest and put the wire's
	// direction off to one side of the cone, which is the bug being fixed
	// rather than a fix for it.
	const float deg = 7.5f * 3.14159265f / 180.0f;
	Vec3        sum{0.0f, 0.0f, 0.0f};
	for (int i = -2; i <= 2; ++i) {
		const float a = deg * static_cast<float>(i);
		// A pellet that hit a wall at 2 m and one that flew the full 40.
		const float reach = (i == -2) ? 2.0f : 40.0f;
		Vec3        unit{};
		Check(UnitDirection(Vec3{std::sin(a) * reach, std::cos(a) * reach, 0.0f}, unit),
		      "each pellet is a direction");
		sum.x += unit.x;
		sum.y += unit.y;
		sum.z += unit.z;
	}

	Vec3 mean{};
	Check(UnitDirection(sum, mean), "five pellets average to a direction");
	Check(aim::Close(mean.x, 0.0f, 1.0e-4f) && aim::Close(mean.y, 1.0f, 1.0e-4f),
	      "and the average is the middle of the cone, not the pellet that flew furthest");

	// One ray is the ordinary case and has to come through untouched.
	Vec3 single{};
	Check(UnitDirection(Vec3{0.0f, 26.0f, 13.0f}, single) &&
	          aim::Close(aim::Len(single), 1.0f),
	      "a single ray normalises to itself");
	Check(single.z > 0.4f,
	      "and keeps its pitch - the whole point, since the engine's own answer has none");
}

void TestShotDirectionPrefersTheDrawnTrail() {
	std::printf("\nwhich of the three lines a shot puts on the wire\n");

	const Vec3 body{0.0f, 1.0f, 0.0f};

	// The case the round is about, with the numbers the geometry produces. The
	// muzzle is at the origin; CWeapon::FireInstantHit's mouse-camera branch
	// traces its ray from the muzzle projected onto the camera axis, half a
	// metre above it, out to a wall 5 m ahead. So the ray points slightly down
	// and the streak the player sees points straight ahead, and those are the
	// two candidates.
	//
	// Same trigger pull, same shot, 5.7 degrees apart - which is the size of
	// the report, in one test, without the game.
	Vec3       aimed{};
	const Vec3 ray{0.0f, 5.0f, -0.5f};      // camera axis down onto the wall
	const Vec3 trail{0.0f, 5.0f, 0.0f};     // muzzle straight to the same wall
	Vec3       rayUnit{}, trailUnit{};
	Check(UnitDirection(ray, rayUnit) && UnitDirection(trail, trailUnit),
	      "the ray and the streak are both directions");

	Check(ChooseShotDirection(trailUnit, 1, rayUnit, 1, body, aimed) == AIM_FROM_TRAIL,
	      "with both in hand, the wire carries the line that was drawn");
	Check(aim::Close(aimed.z, 0.0f, 1.0e-4f),
	      "which is the one that starts at the muzzle we also send");

	const float dot = rayUnit.x * trailUnit.x + rayUnit.y * trailUnit.y +
	                  rayUnit.z * trailUnit.z;
	Check(dot < 0.999f, "and the two really are different lines, not the same one twice");

	// A weapon that traces but never draws - CWeapon::FireM16_1stPerson is the
	// real one. The ray is then the best answer there is and it is a good one,
	// because that branch traces from the camera and the player is looking
	// down it.
	Check(ChooseShotDirection(Vec3{0.0f, 0.0f, 0.0f}, 0, rayUnit, 1, body, aimed) ==
	          AIM_FROM_RAY,
	      "a shot that drew nothing falls back to the ray it tested");
	Check(aim::Close(aimed.z, rayUnit.z, 1.0e-4f), "and keeps that ray's pitch");

	// And neither. The body is what the observer's own engine would derive
	// anyway, so falling back to it costs exactly nothing over doing nothing.
	Check(ChooseShotDirection(Vec3{}, 0, Vec3{}, 0, body, aimed) == AIM_FROM_BODY &&
	          aim::Close(aimed.y, 1.0f, 1.0e-4f),
	      "and with neither, the ped's own heading");

	// A count without a vector, and a vector without a count, are both
	// nonsense arriving from a detour that half-ran. Neither may be used.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(ChooseShotDirection(Vec3{nan, 0.0f, 0.0f}, 1, rayUnit, 1, body, aimed) ==
	          AIM_FROM_RAY,
	      "a NaN trail is refused and the ray is used instead");
	Check(ChooseShotDirection(Vec3{0.0f, 0.0f, 0.0f}, 3, Vec3{0.0f, 0.0f, 0.0f}, 3,
	                          body, aimed) == AIM_FROM_BODY,
	      "three pellets that cancelled out are not a direction either");
	Check(ChooseShotDirection(Vec3{}, 0, Vec3{}, 0, Vec3{0.0f, 0.0f, 0.0f}, aimed) ==
	          AIM_FROM_NOTHING,
	      "and a ped with no forward leaves the caller's own value alone");
}

// ---- time of day and weather ------------------------------------------------

void TestClockDriftIsCircular() {
	std::printf("\nhow far apart two times of day are\n");

	Check(ClockDriftMinutes(12, 0, 12, 0) == 0, "the same time is no distance");
	Check(ClockDriftMinutes(12, 0, 12, 5) == 5, "ahead of us is positive");
	Check(ClockDriftMinutes(12, 5, 12, 0) == -5, "behind us is negative");
	Check(ClockDriftMinutes(11, 55, 12, 5) == 10, "and it crosses the hour");

	// The one that a plain subtraction gets wrong, and it gets it wrong by
	// most of a day - which would then look like an enormous drift and snap
	// the clock every second at midnight.
	Check(ClockDriftMinutes(23, 59, 0, 1) == 2, "midnight is two minutes away, not 1438");
	Check(ClockDriftMinutes(0, 1, 23, 59) == -2, "and two minutes back the other way");

	const int halfADay = ClockDriftMinutes(12, 0, 0, 0);
	Check(halfADay == 720 || halfADay == -720, "opposite sides of the dial are 12 hours");
}

void TestClockOnlyMovesWhenItIsWorthIt() {
	std::printf("\ncorrecting a drifting clock\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(1, 0, /*host=*/0), CH_EVENT));
	g_rec.timeApplies = 0;
	g_rec.world       = WorldState{12, 0, 0, 0};

	c.HandleMessage(Wrap(MakeWorldState(0, 12, 2), CH_EVENT));
	Check(g_rec.timeApplies == 0, "two minutes out is left alone");

	c.HandleMessage(Wrap(MakeWorldState(0, 11, 58), CH_EVENT));
	Check(g_rec.timeApplies == 0, "and so is two minutes out the other way");

	c.HandleMessage(Wrap(MakeWorldState(0, 12, 3), CH_EVENT));
	Check(g_rec.timeApplies == 0, "the tolerance itself is not past the tolerance");

	c.HandleMessage(Wrap(MakeWorldState(0, 12, 20), CH_EVENT));
	Check(g_rec.timeApplies == 1, "twenty minutes out gets moved");
	Check(g_rec.appliedHour == 12 && g_rec.appliedMinute == 20,
	      "and moved to what the session said, not part of the way");

	// A client that joined at a different hour is the normal case, and the
	// arithmetic has to survive it being on the far side of midnight.
	g_rec.world = WorldState{23, 55, 0, 0};
	c.HandleMessage(Wrap(MakeWorldState(0, 0, 30), CH_EVENT));
	Check(g_rec.timeApplies == 2 && g_rec.appliedHour == 0,
	      "and across midnight too");
}

void TestTheHostKeepsItsOwnClock() {
	std::printf("\nthe host is not corrected\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0, 0, /*host=*/0), CH_EVENT));

	Check(c.IsHost(), "the welcome says we're the host");
	Check(g_rec.timeApplies == 0, "so the welcome's hour isn't applied to us");

	g_rec.world = WorldState{12, 0, 0, 0};
	c.HandleMessage(Wrap(MakeWorldState(0, 3, 0, 2, 1), CH_EVENT));
	Check(g_rec.timeApplies == 0, "and neither is a world packet's");
	Check(g_rec.weatherApplies == 0, "nor its weather - it's a copy of ours");

	// A machine that has been the host since it joined may have a
	// ForcedWeatherType its own mission script set. FORCE_WEATHER is a
	// campaign opcode; clearing it mid-mission would be us changing the
	// weather rather than following it.
	Check(g_rec.weatherReleases == 0,
	      "and a host that never followed anyone leaves its own sky alone");
}

// ---- the trains (traintime.h) and the session clock (sessiontime.h) --------

void TestTrainCycleIsTheEnginesMask() {
	std::printf("\nwhere a train is in its loop\n");
	// UpdateTrains: `lea eax,[edx+ebx] / and eax,1FFFFh`, ebx += 10000h per
	// train, and the same with 3FFFFh for the subway.
	Check(TrainCycleMs(0x12345, EL_PERIOD_MS, 0) == 0x12345, "El train 0 is the clock, masked");
	Check(TrainCycleMs(0x12345, EL_PERIOD_MS, 1) == 0x02345,
	      "El train 1 is half a loop on, and wraps");
	Check(TrainCycleMs(0x12345, SUBWAY_PERIOD_MS, 3) == 0x02345,
	      "subway train 3 is three quarters on, and wraps");

	// The whole reason one number is enough: two clocks that agree modulo
	// the subway's period agree about every train on both lines.
	const uint32_t a = 0x0003A1C7, b = a + 7 * SUBWAY_PERIOD_MS;
	bool same = true;
	for (uint32_t i = 0; i < 2; ++i)
		same = same && TrainCycleMs(a, EL_PERIOD_MS, i) == TrainCycleMs(b, EL_PERIOD_MS, i);
	for (uint32_t i = 0; i < 4; ++i)
		same = same && TrainCycleMs(a, SUBWAY_PERIOD_MS, i) == TrainCycleMs(b, SUBWAY_PERIOD_MS, i);
	Check(same, "clocks equal modulo 0x40000 put all six trains in the same place");

	// And half a subway period out is the El's whole period, so the El can't
	// tell and the subway can.
	const uint32_t c = a + EL_PERIOD_MS;
	Check(TrainCycleMs(a, EL_PERIOD_MS, 0) == TrainCycleMs(c, EL_PERIOD_MS, 0),
	      "0x20000 out is invisible on the El");
	Check(TrainCycleMs(a, SUBWAY_PERIOD_MS, 0) != TrainCycleMs(c, SUBWAY_PERIOD_MS, 0),
	      "and is not on the subway");

	// A uint32 wrap is just another multiple of the period.
	Check(TrainCycleMs(0xFFFFFFFFu, SUBWAY_PERIOD_MS, 0) == SUBWAY_PERIOD_MS - 1 &&
	          TrainCycleMs(0u, SUBWAY_PERIOD_MS, 0) == 0,
	      "the wrap of the clock is a wrap of the loop");
}

void TestSessionTimeWarmsUpBySnapping() {
	std::printf("\nthe session clock's first samples\n");
	SessionTimeBase t;
	Check(!t.Valid(), "nothing until the server has said something");

	// ENet's placeholder round trip is 500 ms, so the first estimate is a
	// quarter of a second out...
	t.AddSample(10000, 0, 500);
	Check(t.Valid() && t.OffsetMs() == 10250, "the first sample is taken whole");

	// ...and the next one, with a real round trip, drags it straight back
	// rather than slewing there over five seconds.
	t.AddSample(11000, 1000, 20);
	Check(t.OffsetMs() == 10010, "a better round trip replaces it at once while warming up");
	t.AddSample(12000, 2000, 20);
	Check(t.Snaps() == 3, "every warm-up sample is a snap");
	Check(t.Now(2000) == 12010, "and the session time is the server's plus half a round trip");
}

void TestSessionTimePicksTheLeastDelayedSample() {
	std::printf("\na late packet is not a slow clock\n");
	SessionTimeBase t;
	for (uint32_t i = 0; i < SessionTimeBase::WARMUP_SAMPLES; ++i)
		t.AddSample(10000 + i * 1000, i * 1000, 0);
	const uint32_t before = t.TargetOffsetMs();

	// Retransmitted, queued behind a frame, whatever: it arrived 30 ms after
	// it was stamped, so it looks like a server 30 ms behind.
	t.AddSample(20000, 10030, 0);
	Check(t.TargetOffsetMs() == before, "a delayed sample doesn't pull the estimate back");

	// A sample can't arrive before it was sent, so a larger raw offset is a
	// fresher reading, and it wins.
	t.AddSample(21000 + 5, 11000, 0);
	Check(t.TargetOffsetMs() == before + 5, "the least-delayed sample in the window wins");
}

void TestSessionTimeSlewsWithoutRunningBackwards() {
	std::printf("\ncorrecting the session clock\n");
	SessionTimeBase t;
	for (uint32_t i = 0; i < SessionTimeBase::WARMUP_SAMPLES; ++i)
		t.AddSample(10000 + i * 1000, i * 1000, 0);
	Check(t.OffsetMs() == 10000, "warmed up");

	// 40 ms fresher than anything so far: slide, don't jump.
	uint32_t local = 3000;
	t.AddSample(13040, local, 0);
	Check(t.TargetOffsetMs() == 10040 && t.OffsetMs() == 10000,
	      "a small error is not applied at once");

	uint32_t prev = t.Now(local);
	bool monotonic = true, overshot = false;
	for (int frame = 0; frame < 20; ++frame) {
		local += 16;
		t.Tick(local);
		monotonic = monotonic && static_cast<int32_t>(t.Now(local) - prev) > 0;
		overshot  = overshot || t.OffsetMs() > 10040;
		prev      = t.Now(local);
	}
	Check(t.OffsetMs() == 10016, "320 ms of frames buys 16 ms of correction");
	for (int frame = 0; frame < 100; ++frame) {
		local += 16;
		t.Tick(local);
		overshot = overshot || t.OffsetMs() > 10040;
	}
	Check(t.OffsetMs() == 10040 && !overshot, "and it arrives without overshooting");

	// Now the other way. Sixteen samples 200 ms behind push the old ones out
	// of the window, and the clock has to come back.
	for (size_t i = 0; i < SessionTimeBase::WINDOW; ++i) {
		local += 1000;
		t.AddSample(local + 9840, local, 0);
	}
	Check(t.TargetOffsetMs() == 9840 && t.OffsetMs() == 10040,
	      "a clock found to be ahead is not jumped back");
	prev = t.Now(local);
	for (int frame = 0; frame < 400; ++frame) {
		local += 16;
		t.Tick(local);
		monotonic = monotonic && static_cast<int32_t>(t.Now(local) - prev) > 0;
		prev      = t.Now(local);
	}
	Check(t.OffsetMs() == 9840, "it slides back to the server");
	Check(monotonic, "and the session time never ran backwards on the way");

	// A hitch of ten seconds is not ten seconds of correction.
	t.AddSample(local + 1000 + 9840 + 600, local + 1000, 0);
	local += 1000;
	t.Tick(local + 10000);
	Check(t.OffsetMs() == 9840 + 50, "a long frame is capped at one second's worth");
}

void TestSessionTimeJumpsToADifferentClock() {
	std::printf("\na different clock altogether\n");
	SessionTimeBase t;
	for (uint32_t i = 0; i < SessionTimeBase::WARMUP_SAMPLES; ++i)
		t.AddSample(10000 + i * 1000, i * 1000, 0);
	t.AddSample(3000 + 60000, 3000, 0);
	Check(t.OffsetMs() == 60000, "a minute out is jumped, not slewed");
	Check(t.Snaps() == SessionTimeBase::WARMUP_SAMPLES + 1, "and counted as a snap");

	t.Reset();
	Check(!t.Valid() && t.Snaps() == 0, "a reset forgets everything");
}

void TestSessionTimeAcrossTheWrap() {
	std::printf("\nthe session clock and a uint32 wrap\n");
	SessionTimeBase t;
	t.AddSample(0xFFFFFF00u, 100, 0);
	t.AddSample(0x000002E8u, 1100, 0);   // 0xFFFFFF00 + 1000, wrapped
	Check(t.Now(1100) == 0x000002E8u, "the server's wrap is just arithmetic");
	t.AddSample(0x000006D0u + 7, 2100, 0);
	Check(t.Now(2100) == 0x000006D0u + 7, "and so is the comparison of two samples across it");
}

void TestTrainsRunOnTheServersClock() {
	std::printf("\nthe session's trains\n");
	Client c;
	c.SetBridge(RecordingBridge());
	Check(!c.SessionTime().Valid(), "not connected, the trains are the engine's");

	S_Welcome w          = MakeWelcome(1, 0, /*host=*/0);
	w.hdr.sendTimeMs     = 5000000;
	c.HandleMessage(Wrap(w, CH_EVENT));
	const int32_t err =
	    static_cast<int32_t>(c.SessionTime().Now(WallClock::NowMs()) - 5000000u);
	Check(c.SessionTime().Valid() && err >= 0 && err < 1000,
	      "the welcome's header is the server's clock, and we take it");

	// Everything else the server sends on somebody's behalf carries the
	// sender's clock. Player 0's snapshot says nothing about the server.
	const uint32_t target = c.SessionTime().TargetOffsetMs();
	c.HandleMessage(Wrap(MakeState(0, 123, 1.0f), CH_SNAPSHOT));
	Check(c.SessionTime().TargetOffsetMs() == target, "a relayed header is not a sample");

	S_WorldState ws  = MakeWorldState(0, 12, 0);
	ws.hdr.sendTimeMs = 5000000 + 1000;
	c.HandleMessage(Wrap(ws, CH_EVENT));
	Check(c.SessionTime().Valid(), "world packets keep it fed");

	c.PreFrame();
	Check(g_rec.sessionClockCalls == 1 && g_rec.sessionClockValid &&
	          g_rec.sessionClockOffset == c.SessionTime().OffsetMs(),
	      "PreFrame hands it to the engine side, before CGame::Process");

	c.ClearRosterForTest();
	c.PreFrame();
	Check(g_rec.sessionClockCalls == 2 && !g_rec.sessionClockValid,
	      "a lost session gives the trains back to CTimer");

	// The host is not special here, unlike the time of day.
	Client h;
	h.SetBridge(RecordingBridge());
	S_Welcome hw      = MakeWelcome(0, 0, /*host=*/0);
	hw.hdr.sendTimeMs = 7000000;
	h.HandleMessage(Wrap(hw, CH_EVENT));
	Check(h.IsHost() && h.SessionTime().Valid(), "the host's trains follow the server too");
}

// ---- the planes (planetime.h) ----------------------------------------------

void TestPlaneCycleIsTheEnginesMask() {
	std::printf("\nwhere a plane is in its loop\n");
	// UpdatePlanes: `lea eax,[edx+ecx] / and eax,7FFFFh`, ecx += 2AAAAh per
	// airliner; `lea ecx,[edx+2AAAAh]` and `lea ecx,[edx+55554h]` for the
	// second and third Dodo.
	Check(PlaneCycleMs(0x12345, 0) == 0x12345, "plane 0 is the clock, masked");
	Check(PlaneCycleMs(0x12345, 1) == 0x12345 + 0x2AAAA, "plane 1 is a third of a loop on");
	Check(PlaneCycleMs(0, 2) == 0x55554, "plane 2 is the engine's 55554h, not 0x80000 * 2 / 3");
	Check(PlaneCycleMs(0x7FFFF, 1) == 0x2AAA9, "and wraps at the loop's end");

	const uint32_t a = 0x0006B2F1, b = a + 5 * PLANE_PERIOD_MS;
	bool same = true;
	for (uint32_t i = 0; i < PLANES_PER_PATH; ++i)
		same = same && PlaneCycleMs(a, i) == PlaneCycleMs(b, i);
	Check(same, "clocks equal modulo 0x80000 put every plane in the same place");

	// The subway's period is not enough for the sky: two clocks that agree
	// about every train can still disagree about every plane. The session
	// clock is one number, so it agrees about both.
	const uint32_t c = a + SUBWAY_PERIOD_MS;
	Check(TrainCycleMs(a, SUBWAY_PERIOD_MS, 0) == TrainCycleMs(c, SUBWAY_PERIOD_MS, 0) &&
	          PlaneCycleMs(a, 0) != PlaneCycleMs(c, 0),
	      "0x40000 out is invisible to the trains and not to the planes");

	Check(PlaneCycleMs(0xFFFFFFFFu, 0) == PLANE_PERIOD_MS - 1 && PlaneCycleMs(0u, 0) == 0,
	      "the wrap of the clock is a wrap of the loop");
}

void TestMissionCessnaKeepsItsSchedule() {
	std::printf("\na mission Cessna on the session clock\n");
	// The script stamps the start with CTimer's value; UpdatePlanes flies it
	// from `t - start`. Whatever the two clocks are, moving the start with
	// the clock has to leave that difference exactly where it was.
	struct Case {
		uint32_t ours, session, start;
	};
	const Case cases[] = {
	    {100000, 100000, 90000},              // no session: nothing moves
	    {100000, 5000000, 90000},             // session well ahead
	    {5000000, 100000, 4990000},           // session behind
	    {0x00000100u, 0xFFFFFF00u, 0xFFFFF000u},   // our clock has wrapped
	    {0xFFFFFF00u, 0x00000100u, 0xFFFFF000u},   // the session's has
	    {100000, 100000 + 0x80000000u, 99999},     // as far apart as 2^32 allows
	};
	bool elapsedKept = true, roundTrip = true, landingKept = true, pathKept = true;
	for (const Case &k : cases) {
		const uint32_t shifted = MissionStartOnSessionClock(k.start, k.ours, k.session);
		elapsedKept = elapsedKept && k.session - shifted == k.ours - k.start;
		roundTrip   = roundTrip &&
		            MissionStartOnOurClock(shifted, k.ours, k.session) == k.start;
		// The two tests UpdatePlanes makes on the difference.
		landingKept = landingKept &&
		              ((k.session - shifted >= DRUG_RUN_CESNA_FLIGHT_MS) ==
		               (k.ours - k.start >= DRUG_RUN_CESNA_FLIGHT_MS));
		pathKept = pathKept &&
		           ((k.session - shifted) & 0x1FFFF) == ((k.ours - k.start) & 0x1FFFF);
	}
	Check(elapsedKept, "the flight time is what single player would see");
	Check(roundTrip, "and the start goes back exactly after the call");
	Check(landingKept && pathKept, "so it lands, and flies, on the same schedule");

	// What it is for. Ten seconds into the drug run, on a session clock five
	// minutes ahead of ours: left alone, UpdatePlanes would land it at once.
	const uint32_t ours = 1000000, start = ours - 10000, session = ours + 300000;
	Check(session - start >= DRUG_RUN_CESNA_FLIGHT_MS,
	      "unshifted, the session clock lands a Cessna that has just taken off");
	Check(session - MissionStartOnSessionClock(start, ours, session) == 10000,
	      "shifted, it is ten seconds into its flight");
	Check(DROP_OFF_CESNA_FLIGHT_MS == 0x7F448 && DRUG_RUN_CESNA_FLIGHT_MS == 0x1F448,
	      "the flight times are the engine's immediates");
}


// ---- the traffic lights (lighttime.h) --------------------------------------

void TestLightsAreTheEnginesThresholds() {
	std::printf("\nwhat a traffic light shows\n");
	// The immediates in LightForCars1 (0x00455760), LightForCars2
	// (0x00455790) and LightForPeds (0x004557D0), each after `and eax,3FFFh`.
	struct Row {
		uint32_t t;
		uint8_t  cars1, cars2, peds;
	};
	const Row rows[] = {
	    {0, CAR_LIGHT_GREEN, CAR_LIGHT_RED, PED_LIGHT_DONT_WALK},
	    {4999, CAR_LIGHT_GREEN, CAR_LIGHT_RED, PED_LIGHT_DONT_WALK},
	    {5000, CAR_LIGHT_AMBER, CAR_LIGHT_RED, PED_LIGHT_DONT_WALK},
	    {5999, CAR_LIGHT_AMBER, CAR_LIGHT_RED, PED_LIGHT_DONT_WALK},
	    {6000, CAR_LIGHT_RED, CAR_LIGHT_GREEN, PED_LIGHT_DONT_WALK},
	    {10999, CAR_LIGHT_RED, CAR_LIGHT_GREEN, PED_LIGHT_DONT_WALK},
	    {11000, CAR_LIGHT_RED, CAR_LIGHT_AMBER, PED_LIGHT_DONT_WALK},
	    {11999, CAR_LIGHT_RED, CAR_LIGHT_AMBER, PED_LIGHT_DONT_WALK},
	    {12000, CAR_LIGHT_RED, CAR_LIGHT_RED, PED_LIGHT_WALK},
	    {15383, CAR_LIGHT_RED, CAR_LIGHT_RED, PED_LIGHT_WALK},
	    {15384, CAR_LIGHT_RED, CAR_LIGHT_RED, PED_LIGHT_WALK_BLINK},
	    {16383, CAR_LIGHT_RED, CAR_LIGHT_RED, PED_LIGHT_WALK_BLINK},
	    {16384, CAR_LIGHT_GREEN, CAR_LIGHT_RED, PED_LIGHT_DONT_WALK},
	};
	bool all = true;
	for (const Row &r : rows)
		all = all && CarLights1(r.t) == r.cars1 && CarLights2(r.t) == r.cars2 &&
		      PedLights(r.t) == r.peds;
	Check(all, "every threshold is the engine's immediate, on both sides of it");
	Check(LIGHT_PERIOD_MS == 16384 && CARS1_AMBER_FROM_MS == 5000 &&
	          CARS2_AMBER_FROM_MS == 11000 && PEDS_BLINK_FROM_MS == 15384,
	      "a 16.384 s cycle: 1388h, 1770h, 2AF8h, 2EE0h, 3C18h");

	// What the three have to agree on, over the whole cycle: the two
	// directions never both go, and nobody is told to walk into traffic.
	bool neverBothGo = true, walkOnlyOnRed = true, cars1Order = true;
	for (uint32_t t = 0; t < LIGHT_PERIOD_MS; ++t) {
		neverBothGo = neverBothGo &&
		              (CarLights1(t) == CAR_LIGHT_RED || CarLights2(t) == CAR_LIGHT_RED);
		if (PedLights(t) != PED_LIGHT_DONT_WALK)
			walkOnlyOnRed = walkOnlyOnRed && CarLights1(t) == CAR_LIGHT_RED &&
			                CarLights2(t) == CAR_LIGHT_RED;
		// Green, amber, red and never back within a cycle.
		if (t > 0)
			cars1Order = cars1Order && CarLights1(t) >= CarLights1(t - 1);
	}
	Check(neverBothGo, "one of the two directions is red at every millisecond");
	Check(walkOnlyOnRed, "pedestrians only walk while both are red");
	Check(cars1Order, "and a light goes green, amber, red without stepping back");

	Check(CarLights1(0xFFFFFFFFu) == CarLights1(LIGHT_PERIOD_MS - 1) &&
	          PedLights(0u) == PED_LIGHT_DONT_WALK,
	      "the wrap of the clock is a wrap of the cycle");
}

void TestLightsAgreeOnTheSessionClock() {
	std::printf("\nthe same lights on two machines\n");
	// Two players whose saves put their CTimers nowhere near each other, and
	// whose wall clocks started at different times. They hear one server.
	const uint32_t oursA = 1234567, oursB = 98765;
	uint32_t       wallA = 5000, wallB = 777000;
	SessionTimeBase a, b;
	for (uint32_t i = 0; i < SessionTimeBase::WARMUP_SAMPLES + 2; ++i) {
		const uint32_t server = 40000000 + i * 1000;
		a.AddSample(server, wallA, 20);
		b.AddSample(server, wallB, 20);
		wallA += 1000;
		wallB += 1000;
	}
	// Ten minutes of frames, at the same instant on both.
	bool sameLights = true, oursDiffer = false;
	for (uint32_t ms = 0; ms < 600000; ms += 16) {
		const uint32_t sa = a.Now(wallA + ms), sb = b.Now(wallB + ms);
		sameLights = sameLights && CarLights1(sa) == CarLights1(sb) &&
		             CarLights2(sa) == CarLights2(sb) && PedLights(sa) == PedLights(sb);
		oursDiffer = oursDiffer || CarLights1(oursA + ms) != CarLights1(oursB + ms);
	}
	Check(oursDiffer, "on their own CTimers the two junctions disagree");
	Check(sameLights, "on the session clock every light agrees, every frame");

	const uint32_t s = 0x0012A5C3;
	Check(CarLights2(s) == CarLights2(s + 9 * LIGHT_PERIOD_MS) &&
	          PedLights(s) == PedLights(s + 9 * LIGHT_PERIOD_MS),
	      "clocks equal modulo 16384 show the same lights");
	Check(CarLights1(s) != CarLights1(s + LIGHT_PERIOD_MS / 2),
	      "and half a cycle out is a different light");
}

void TestLightsFallBackWithoutASession() {
	std::printf("\nthe lights without a session\n");
	// The real engine-side clock (game/sessionclock.cpp), wired to a real
	// Client the way dllmain wires it. When it says no, game/lights.cpp calls
	// the engine's own function, which reads CTimer.
	WorldBridge wb = RecordingBridge();
	game::AddSessionClockToBridge(wb);
	Client c;
	c.SetBridge(wb);

	c.PreFrame();
	uint32_t t = 0;
	Check(!game::SessionClockThisFrame(t) && !game::SessionClockNow(t),
	      "not connected: no session clock, the lights are CTimer's");

	S_Welcome w      = MakeWelcome(1, 0, /*host=*/0);
	w.hdr.sendTimeMs = 9000000;
	c.HandleMessage(Wrap(w, CH_EVENT));
	c.PreFrame();
	uint32_t      first = 0;
	const bool    on    = game::SessionClockThisFrame(first);
	const int32_t err =
	    static_cast<int32_t>(first - c.SessionTime().Now(WallClock::NowMs()));
	Check(on && err <= 0 && err > -1000, "connected: the lights read the server's clock");

	// Hold the frame until the wall clock has visibly moved on.
	const uint32_t start = WallClock::NowMs();
	while (WallClock::NowMs() - start < 3) {
	}
	uint32_t again = 0, now = 0;
	game::SessionClockThisFrame(again);
	game::SessionClockNow(now);
	Check(again == first && now != first,
	      "every light asked in one frame gets the same time, as from CTimer");

	c.PreFrame();
	uint32_t next = 0;
	game::SessionClockThisFrame(next);
	Check(static_cast<int32_t>(next - first) >= 3, "and the next frame takes a new one");

	c.ClearRosterForTest();
	c.PreFrame();
	Check(!game::SessionClockThisFrame(t),
	      "a lost session gives the lights back to CTimer on the next frame");
}

// ---- the lift bridge (liftbridgetime.h) ------------------------------------

void TestBridgeStatesAreTheEnginesThresholds() {
	std::printf("\nwhere the lift bridge is in its cycle\n");
	// CBridge::Update: `sub edx,[008F2BC0h] / and edx,0FFFFh`, then 2710h,
	// 9C40h, C350h, EA60h.
	Check(BridgeStateAt(0) == BRIDGE_LOWERING && BridgeStateAt(9999) == BRIDGE_LOWERING,
	      "the first ten seconds it comes down");
	Check(BridgeStateAt(10000) == BRIDGE_DOWN && BridgeStateAt(39999) == BRIDGE_DOWN,
	      "thirty seconds down");
	Check(BridgeStateAt(40000) == BRIDGE_ABOUT_TO_RAISE &&
	          BridgeStateAt(49999) == BRIDGE_ABOUT_TO_RAISE,
	      "ten seconds of warning");
	Check(BridgeStateAt(50000) == BRIDGE_RAISING && BridgeStateAt(59999) == BRIDGE_RAISING,
	      "ten seconds going up");
	Check(BridgeStateAt(60000) == BRIDGE_UP && BridgeStateAt(65535) == BRIDGE_UP,
	      "and up for the rest of the 65.536 s");
	Check(BridgePhaseMs(5, 10) == 65531, "the elapsed time is taken modulo 2^16");
}

void TestBridgeFollowsTheSessionClock() {
	std::printf("\nthe lift bridge on the session clock\n");
	struct Case {
		uint32_t ours, session;
	};
	const Case cases[] = {
	    {100000, 100000},             // the two clocks agree: a zero epoch, moved
	    {100000, 5000000},
	    {5000000, 100000},
	    {0x00000100u, 0xFFFFFF00u},   // ours has wrapped
	    {0xFFFFFF00u, 0x00000100u},   // the session's has
	    {77, 77 + 0x80000000u},
	};
	bool phase = true, stamped = true;
	for (const Case &k : cases) {
		const uint32_t epoch = BridgeEpochForSession(k.ours, k.session);
		phase   = phase && BridgePhaseMs(k.ours, epoch) == (k.session & 0xFFFF);
		stamped = stamped && epoch != 0;
	}
	Check(phase, "Update, reading CTimer, computes the session clock's phase");
	Check(stamped, "and never finds a zero epoch, which it would restamp");

	const uint32_t session = 31415926;
	Check(BridgeStateAt(BridgePhaseMs(1000, BridgeEpochForSession(1000, session))) ==
	          BridgeStateAt(BridgePhaseMs(9876543, BridgeEpochForSession(9876543, session))),
	      "two CTimers, one session clock: both players see the same bridge");

	// The session ends, the epoch stays where the last call left it, and
	// CTimer carries on: so does the bridge, from where it was.
	const uint32_t ours = 200000, last = 7654321;
	const uint32_t epoch = BridgeEpochForSession(ours, last);
	Check(BridgePhaseMs(ours + 16, epoch) == ((last + 16) & 0xFFFF),
	      "leaving the session doesn't move the bridge");
}

void TestBridgeLinksSurviveAJump() {
	std::printf("\nthe bridge's traffic links when the clock jumps\n");
	// Model the engine: Init lights the links, 4 after 3 lights them, 3 after
	// 2 puts them out, nothing else touches them. Then apply the rule. The
	// links have to be right for wherever the bridge now is, from any state to
	// any state, with and without Init in between.
	bool allRight = true;
	for (int32_t before = BRIDGE_LOCKED; before <= BRIDGE_RAISING; ++before)
		for (int32_t after = BRIDGE_LOCKED; after <= BRIDGE_RAISING; ++after)
			for (int init = 0; init < 2; ++init) {
				bool links = BridgeLinksLit(before);
				if (init)
					links = true;
				if (after == BRIDGE_ABOUT_TO_RAISE && before == BRIDGE_DOWN)
					links = true;
				if (after == BRIDGE_DOWN && before == BRIDGE_LOWERING)
					links = false;
				switch (LinksAfterBridgeUpdate(before, after, init != 0)) {
				case BridgeLinks::Leave: break;
				case BridgeLinks::Light: links = true; break;
				case BridgeLinks::PutOut: links = false; break;
				}
				allRight = allRight && links == BridgeLinksLit(after);
			}
	Check(allRight, "whatever the jump, the links end up right for the bridge");

	// On an unbroken cycle it never does anything: the engine's edges are
	// enough there, as in single player.
	bool steadyIsFree = LinksAfterBridgeUpdate(BRIDGE_LOCKED, BRIDGE_LOWERING, false) ==
	                    BridgeLinks::Leave;
	for (uint32_t t = 16; t < 3 * BRIDGE_PERIOD_MS; t += 16)
		steadyIsFree = steadyIsFree &&
		               LinksAfterBridgeUpdate(BridgeStateAt((t - 16) & 0xFFFF),
		                                      BridgeStateAt(t & 0xFFFF),
		                                      false) == BridgeLinks::Leave;
	Check(steadyIsFree, "a bridge going round its cycle is left to the engine");

	Check(LinksAfterBridgeUpdate(BRIDGE_DOWN, BRIDGE_UP, false) == BridgeLinks::Light,
	      "joining with the bridge down and finding it up stops the traffic");
	Check(LinksAfterBridgeUpdate(BRIDGE_UP, BRIDGE_DOWN, false) == BridgeLinks::PutOut,
	      "and the other way lets it over");
	Check(LinksAfterBridgeUpdate(BRIDGE_DOWN, BRIDGE_DOWN, true) == BridgeLinks::PutOut,
	      "a save loaded with the bridge down doesn't leave it shut for 30 s");
}

void TestWelcomeSetsTheClockStraightAway() {
	std::printf("\njoining a session already in progress\n");
	Client c;
	c.SetBridge(RecordingBridge());
	g_rec.world = WorldState{12, 0, 0, 0};

	S_Welcome w = MakeWelcome(1, 0, /*host=*/0);
	w.hour      = 3;
	w.minute    = 30;
	w.weather   = 2;
	w.weatherOld = 1;
	c.HandleMessage(Wrap(w, CH_EVENT));

	Check(!c.IsHost(), "player 1 isn't the host here");
	Check(g_rec.timeApplies == 1 && g_rec.appliedHour == 3 &&
	          g_rec.appliedMinute == 30,
	      "the welcome moves the clock without waiting for a world packet");
	Check(g_rec.weatherApplies == 1 && g_rec.appliedWeather == 2 &&
	          g_rec.appliedWeatherOld == 1,
	      "and brings the sky over with it");
}

void TestWeatherIsAPairAndIsWrittenEveryTime() {
	std::printf("\nmirroring the host's sky\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(1, 0, /*host=*/0), CH_EVENT));
	g_rec.weatherApplies = 0;

	c.HandleMessage(Wrap(MakeWorldState(0, 12, 0, /*weather=*/2, /*old=*/1), CH_EVENT));
	Check(g_rec.weatherApplies == 1, "a world packet writes the weather");
	Check(g_rec.appliedWeather == 2 && g_rec.appliedWeatherOld == 1,
	      "both ends of the blend, not just the one being blended towards");

	// Not change-gated on purpose: it's two 16-bit stores, and the local
	// weather rotation picks its own next type every game hour. Skipping a
	// write because nothing changed on the wire would let that win.
	c.HandleMessage(Wrap(MakeWorldState(0, 12, 0, 2, 1), CH_EVENT));
	Check(g_rec.weatherApplies == 2, "and writes it again next second");
}

void TestBecomingTheHostGivesTheSkyBack() {
	std::printf("\nthe host leaving\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(1, 0, /*host=*/0), CH_EVENT));
	Check(g_rec.weatherReleases == 0, "an ordinary client's sky stays pinned");

	// Player 0 quit, so the server says we have it now. A pinned sky never
	// rotates again, so the new host has to be unpinned or the session gets
	// one weather type for the rest of its life.
	c.HandleMessage(Wrap(MakeWorldState(1, 12, 0), CH_EVENT));
	Check(c.IsHost(), "the world packet is how we find out");
	Check(g_rec.weatherReleases == 1, "and the sky goes back to the engine");

	c.HandleMessage(Wrap(MakeWorldState(1, 12, 1), CH_EVENT));
	Check(g_rec.weatherReleases == 1, "once, not once a second");
}

void TestRubbishWorldStateIsIgnored() {
	std::printf("\nworld state that can't be true\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(1, 0, /*host=*/0), CH_EVENT));
	g_rec.timeApplies    = 0;
	g_rec.weatherApplies = 0;

	// CClock only tests its hour for >= 24 *after* an increment, so garbage
	// written into it doesn't come back on its own.
	c.HandleMessage(Wrap(MakeWorldState(0, 25, 0), CH_EVENT));
	Check(g_rec.timeApplies == 0, "hour 25 isn't a time of day");

	c.HandleMessage(Wrap(MakeWorldState(0, 12, 99), CH_EVENT));
	Check(g_rec.timeApplies == 0, "minute 99 isn't either");

	// eWeatherType indexes arrays inside CWeather with no bounds check. The
	// two packets above carried a valid weather with their bad time, which
	// is the point: a bad hour doesn't throw the sky away with it.
	Check(g_rec.weatherApplies == 2, "a bad hour doesn't stop the sky arriving");

	const int before = g_rec.weatherApplies;
	c.HandleMessage(Wrap(MakeWorldState(0, 12, 0, /*weather=*/9), CH_EVENT));
	Check(g_rec.weatherApplies == before, "and weather 9 doesn't exist");
}

void TestNoWorldToReadMeansNoCorrection() {
	std::printf("\nnothing to correct yet\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(1, 0, /*host=*/0), CH_EVENT));
	g_rec.timeApplies = 0;

	// A loading screen. There's no clock to compare against, so there's no
	// drift to act on - and the next packet is a second away.
	g_rec.haveWorld = false;
	c.HandleMessage(Wrap(MakeWorldState(0, 3, 0), CH_EVENT));
	Check(g_rec.timeApplies == 0, "a clock we can't read isn't moved");

	g_rec.haveWorld = true;
	c.HandleMessage(Wrap(MakeWorldState(0, 3, 0), CH_EVENT));
	Check(g_rec.timeApplies == 1, "and it's picked up as soon as it can be");
}

void TestDisconnectDropsTheHost() {
	std::printf("\nthe session ending\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0, 0, /*host=*/0), CH_EVENT));
	Check(c.IsHost(), "we were the host");

	// A second welcome is a new session. Nobody's clock is ours to follow
	// until the new one says whose it is.
	c.HandleMessage(Wrap(MakeWelcome(2, 0, INVALID_PLAYER), CH_EVENT));
	Check(!c.IsHost(), "and we aren't any more");
	Check(c.HostPlayerId() == INVALID_PLAYER, "with nobody else holding it either");
}

void TestAngleWrap() {
	std::printf("\naim yaw wrapping\n");
	constexpr float kPi = 3.14159265358979323846f;

	auto near = [](float a, float b) { return a - b < 1.0e-4f && b - a < 1.0e-4f; };

	Check(near(WrapAngle(0.5f), 0.5f), "an in-range angle is untouched");
	Check(near(WrapAngle(kPi * 1.5f), -kPi * 0.5f), "past +pi wraps to negative");
	Check(near(WrapAngle(-kPi * 1.5f), kPi * 0.5f), "past -pi wraps to positive");
	// heading + torso yaw is how aimYaw gets sampled, and both can be near pi.
	Check(WrapAngle(kPi - 0.1f + 0.9f) < 0.0f,
	      "a heading plus a torso yaw that crosses the seam comes out the far side");
}

// ---- nametags -------------------------------------------------------------
//
// Only the arithmetic. What a tag looks like needs two running games and a
// pair of eyes; what it does at 50 metres, with a name full of rubbish, or
// with eight players standing on the same doorstep does not.

void TestTagDistanceCurve() {
	std::printf("\nnametag size and fade with distance\n");

	const TagPlan close = PlanTag(6.0f);
	Check(close.draw && close.alpha == 255, "a player at 6 m is drawn solid");
	Check(close.scale > 1.1f, "and at full size");

	const TagPlan behind = PlanTag(-4.0f);
	Check(!behind.draw, "a player behind the camera gets no tag");
	const TagPlan onTheLens = PlanTag(0.0f);
	Check(!onTheLens.draw, "and neither does one at exactly zero depth");

	// The tag has to be gone before the ped is, or it pops off a player who
	// is still on screen.
	Check(!PlanTag(TAG_RANGE_M).draw, "nothing at the streaming radius");
	Check(!PlanTag(TAG_RANGE_M + 50.0f).draw, "nor past it");

	const TagPlan fading = PlanTag(TAG_RANGE_M - TAG_FADE_BAND_M * 0.5f);
	Check(fading.draw && fading.alpha > 0 && fading.alpha < 255,
	      "halfway through the fade band it is half there");
	Check(PlanTag(TAG_RANGE_M - TAG_FADE_BAND_M - 1.0f).alpha == 255,
	      "just inside the band it is still solid");

	Check(PlanTag(40.0f).scale < PlanTag(20.0f).scale,
	      "further away is smaller");
	Check(PlanTag(TAG_FAR_M * 2.0f).scale == 0.0f,
	      "past the range there is no scale to speak of");
	// The floor is what keeps a distant tag legible instead of a smudge.
	const TagPlan farOff = PlanTag(TAG_RANGE_M - 0.5f);
	Check(farOff.scale >= TAG_MIN_SCALE, "and it never shrinks past the floor");

	// The range itself, pinned to the number that was asked for rather than to
	// whatever the constant happens to say, so moving it back is a test
	// failure and not a silent change.
	Check(TAG_RANGE_M == 50.0f, "tags are gone by 50 m");
	Check(PlanTag(39.0f).alpha == 255, "solid at 39 m");
	Check(PlanTag(45.0f).alpha > 0 && PlanTag(45.0f).alpha < 255, "going at 45 m");
	Check(!PlanTag(50.0f).draw, "gone at 50 m");
	// A fifth of the range spent fading, the same proportion as before.
	Check(TAG_FADE_BAND_M * 5.0f == TAG_RANGE_M, "the fade band is a fifth of the range");
}

void TestTagSizeIsResolutionIndependent() {
	std::printf("\nnametag size comes from the screen height and nothing else\n");

	// A player about ten metres away, which is what the first in-game shot of
	// this showed.
	const float ten = PlanTag(10.0f).scale;

	const TagMetrics small = MeasureTag(600.0f, ten);
	const TagMetrics hd    = MeasureTag(1080.0f, ten);
	const TagMetrics uhd   = MeasureTag(2160.0f, ten);

	auto near = [](float a, float b) {
		const float d = a > b ? a - b : b - a;
		return d < 1.0e-4f;
	};

	// Same fraction of the screen on every one of them. This is the property
	// that was asked for: change resolution and the tag moves with it.
	Check(near(small.nameH / 600.0f, hd.nameH / 1080.0f) &&
	          near(hd.nameH / 1080.0f, uhd.nameH / 2160.0f),
	      "the name is the same fraction of the screen at 600, 1080 and 2160");
	Check(near(small.icon / 600.0f, uhd.icon / 2160.0f),
	      "and so is the weapon icon");
	Check(near(small.headGap / 600.0f, uhd.headGap / 2160.0f),
	      "and the gap above the player's head");
	Check(near(small.columnH / 600.0f, uhd.columnH / 2160.0f),
	      "and the block of text as a whole");

	// Doubling the screen doubles every length, exactly.
	Check(near(uhd.nameH, hd.nameH * 2.0f), "twice the screen is twice the tag");

	// A glyph cell is 32*scaleX by 20*scaleY. Its shape must not change with
	// the aspect ratio, which is what the old width-scaled version got wrong
	// and what the installed widescreen fix exists to prevent.
	const float cellAspect = (32.0f * hd.nameScaleX) / hd.nameH;
	const float uhdAspect  = (32.0f * uhd.nameScaleX) / uhd.nameH;
	Check(near(cellAspect, uhdAspect), "letters keep their shape at any size");
	Check(cellAspect < 1.0f && cellAspect > 0.85f,
	      "and it is the shape the game's own heading font has, not a stretched one");

	// The absolute size, pinned so that shrinking it further or letting it
	// creep back up is a deliberate act. At ten metres the name is about two
	// percent of the screen height.
	const float fraction = hd.nameH / 1080.0f;
	Check(fraction > 0.016f && fraction < 0.024f,
	      "at ten metres a name is about two percent of the screen height");

	// The first in-game run had this three times too big. Pin the correction
	// against the number it used to be.
	const float wasScaleY = 1.10f;   // the old TAG_NAME_SCALE_Y
	const float nowScaleY = TAG_FONT_SHAPE_Y * TAG_NAME_SIZE;
	Check(wasScaleY / nowScaleY > 2.5f && wasScaleY / nowScaleY < 3.5f,
	      "which is about a third of what the first in-game run drew");

	// Proportions inside the tag.
	Check(hd.hpH < hd.nameH, "the health line is smaller than the name");
	Check(hd.icon < hd.columnH,
	      "and the icon is shorter than the two lines it sits beside");
	Check(hd.icon > hd.nameH,
	      "but still taller than one line of it, so it reads as an icon");
	Check(hd.shadow >= TAG_SHADOW_MIN_PX && hd.shadow < hd.nameH * 0.2f,
	      "the drop shadow is visible without being a smear");
}

void TestProbeBudget() {
	std::printf("\nline of sight probes are rationed\n");

	bool    eligible[MAX_PLAYERS];
	uint8_t out[MAX_PLAYERS];
	for (int i = 0; i < MAX_PLAYERS; ++i)
		eligible[i] = true;

	// Eight players must not mean eight rays.
	uint8_t cursor = 0;
	int     seen[MAX_PLAYERS] = {};
	for (int frame = 0; frame < MAX_PLAYERS; ++frame) {
		const int n = PlanProbes(eligible, MAX_PLAYERS, TAG_PROBE_BUDGET, cursor, out);
		Check(n <= TAG_PROBE_BUDGET, "the budget is never exceeded");
		for (int i = 0; i < n; ++i)
			++seen[out[i]];
	}

	bool everyone = true;
	for (int i = 0; i < MAX_PLAYERS; ++i)
		if (seen[i] == 0)
			everyone = false;
	Check(everyone, "and every player is still tested within eight frames");

	bool evenly = true;
	for (int i = 0; i < MAX_PLAYERS; ++i)
		if (seen[i] != 1)
			evenly = false;
	Check(evenly, "exactly once each, so nobody is starved and nobody is favoured");

	// Slots that failed the free gates must never cost a ray.
	for (int i = 0; i < MAX_PLAYERS; ++i)
		eligible[i] = (i == 5);
	cursor = 0;
	bool onlyFive = true;
	for (int frame = 0; frame < 20; ++frame) {
		const int n = PlanProbes(eligible, MAX_PLAYERS, TAG_PROBE_BUDGET, cursor, out);
		for (int i = 0; i < n; ++i)
			if (out[i] != 5)
				onlyFive = false;
	}
	Check(onlyFive, "a slot that is off screen or out of range is never probed");

	for (int i = 0; i < MAX_PLAYERS; ++i)
		eligible[i] = false;
	cursor = 0;
	Check(PlanProbes(eligible, MAX_PLAYERS, TAG_PROBE_BUDGET, cursor, out) == 0,
	      "nobody eligible means no rays at all");

	// A cursor left pointing past the end by a roster that shrank.
	for (int i = 0; i < MAX_PLAYERS; ++i)
		eligible[i] = true;
	cursor = 200;
	Check(PlanProbes(eligible, MAX_PLAYERS, TAG_PROBE_BUDGET, cursor, out) == 1,
	      "a stale cursor past the end wraps instead of picking nothing");
}

void TestOcclusionFade() {
	std::printf("\nnametags fade behind walls rather than blinking\n");

	// End to end takes the whole interval, not one frame.
	float v = 1.0f;
	v = StepVisibility(v, false, 16);
	Check(v > 0.8f && v < 1.0f, "one frame behind a wall barely moves it");

	int frames = 0;
	while (v > 0.0f && frames < 1000) {
		v = StepVisibility(v, false, 16);
		++frames;
	}
	Check(v == 0.0f, "held behind a wall it does reach zero");
	Check(frames * 16 >= int(TAG_OCCLUSION_FADE_MS) - 32,
	      "and takes about the fade interval to get there");

	// The endpoints hold. Stepping past them would make the tag take longer to
	// react the longer it had been sitting still.
	v = 0.0f;
	for (int i = 0; i < 40; ++i)
		v = StepVisibility(v, true, TAG_MAX_STEP_MS);
	Check(v == 1.0f, "coming back it stops at fully visible and stays there");
	for (int i = 0; i < 40; ++i)
		v = StepVisibility(v, false, TAG_MAX_STEP_MS);
	Check(v == 0.0f, "and going it stops at fully hidden and stays there");

	// The clamp is what stops a loading screen from snapping every tag.
	const float slow = StepVisibility(1.0f, false, 5000);
	const float fast = StepVisibility(1.0f, false, TAG_MAX_STEP_MS);
	Check(slow == fast, "a huge frame gap is treated as the longest believable one");

	// The thin obstacle. A lamp post blocks the line for about a tenth of a
	// second, and at one probe per frame per eight players that is at most a
	// couple of blocked answers. The tag has to dip and come back, not vanish.
	v = 1.0f;
	for (int i = 0; i < 2; ++i)
		v = StepVisibility(v, false, 16);
	Check(v > 0.5f, "two blocked answers only dim a tag, they do not hide it");
	Check(TagAlpha(255, v) > 128, "which is a dip on screen, not a blink");
	for (int i = 0; i < 2; ++i)
		v = StepVisibility(v, true, 16);
	Check(v == 1.0f, "and it is back to solid two frames after the post is passed");
}

void TestOcclusionAlpha() {
	std::printf("\nocclusion reuses the distance alpha rather than adding a second\n");

	Check(TagAlpha(255, 1.0f) == 255, "visible and close is fully solid");
	Check(TagAlpha(255, 0.0f) == 0, "hidden is hidden however close they are");
	// A player far enough to be fading who also steps behind a wall must not
	// come out brighter than either curve alone would have made them.
	const uint8_t halfDistance = PlanTag(TAG_RANGE_M - TAG_FADE_BAND_M * 0.5f).alpha;
	Check(TagAlpha(halfDistance, 0.5f) < halfDistance,
	      "the two fades multiply instead of fighting");
	Check(TagAlpha(halfDistance, 1.0f) == halfDistance,
	      "and a clear line leaves the distance fade exactly as it was");
}

void TestTagHealthText() {
	std::printf("\nnametag health text\n");
	char out[16];

	TagHealth(100.0f, out, sizeof(out));
	Check(std::string(out) == "{ 100", "full health is the heart and a number");
	TagHealth(63.7f, out, sizeof(out));
	Check(std::string(out) == "{ 64", "and it rounds rather than truncating");

	TagHealth(0.0f, out, sizeof(out));
	Check(std::string(out) == "WASTED", "no health is the word, not a zero");
	TagHealth(-5.0f, out, sizeof(out));
	Check(std::string(out) == "WASTED", "and so is negative health");

	// A player on 0.4 health is alive. Saying WASTED over someone still
	// standing is worse than rounding up.
	TagHealth(0.4f, out, sizeof(out));
	Check(std::string(out) == "{ 1", "a sliver of health still reads as alive");

	TagHealth(100000.0f, out, sizeof(out));
	Check(std::string(out) == "{ 999", "a silly health value is clamped, not wrapped");
}

void TestTagNameIsSafeForCFont() {
	std::printf("\nnametag names are made safe for CFont\n");
	char out[TAG_NAME_MAX + 1];

	TagName("noxx", out, sizeof(out));
	Check(std::string(out) == "NOXX", "names are drawn in caps, like the rest of the HUD");

	// CFont::PrintString returns without drawing anything when the first
	// character is '*', so a nickname could otherwise hide its own tag.
	TagName("*ghost", out, sizeof(out));
	Check(out[0] != '*', "a leading asterisk cannot blank the whole string");

	// '~' opens a formatting token, which would eat the rest of the line and
	// could recolour it.
	TagName("a~b", out, sizeof(out));
	Check(std::string(out).find('~') == std::string::npos,
	      "a tilde cannot open a colour token");

	// CFont indexes Size[style][c - ' '], 193 entries wide.
	TagName("\x01\xFF", out, sizeof(out));
	Check(std::string(out) == "??", "bytes outside printable ASCII are replaced");

	TagName("", out, sizeof(out));
	Check(out[0] != '\0', "an empty nickname still draws something");
	TagName(nullptr, out, sizeof(out));
	Check(out[0] != '\0', "and so does no nickname at all");

	TagName("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", out, sizeof(out));
	Check(std::strlen(out) == TAG_NAME_MAX,
	      "a long nickname is cut, not shrunk: a tag that resizes is a tag that moves");
}

void TestEightTagsDoNotStack() {
	std::printf("\neight players bunched together\n");

	// Every tag at the same screen point, which is the doorway case.
	TagBox placed[8];
	int    count = 0;
	for (int i = 0; i < 8; ++i) {
		const TagBox want{100.0f, 200.0f, 380.0f, 400.0f};
		const float  lift = TagLift(want, placed, count, 2.0f);
		placed[count]     = TagBox{want.left, want.right, want.top - lift,
                               want.bottom - lift};
		++count;
	}

	Check(placed[0].top == 380.0f, "the nearest player keeps the spot over their head");

	bool anyOverlap = false;
	for (int i = 0; i < count; ++i)
		for (int j = i + 1; j < count; ++j)
			if (TagBoxesOverlap(placed[i], placed[j], 0.0f))
				anyOverlap = true;
	Check(!anyOverlap, "and none of the other seven ends up on top of another");

	bool climbing = true;
	for (int i = 1; i < count; ++i)
		if (placed[i].top >= placed[i - 1].top)
			climbing = false;
	Check(climbing, "each one is lifted above the last, in order");

	// Tags that were never going to collide must not be moved at all.
	const TagBox a{0.0f, 50.0f, 100.0f, 130.0f};
	const TagBox b{300.0f, 350.0f, 100.0f, 130.0f};
	Check(TagLift(b, &a, 1, 2.0f) == 0.0f, "a tag well clear of another stays put");
}

// ---- joining a session that is already running ------------------------------
//
// One question, asked of everything in the roster: what does a player who was
// here from the start see, and what does a player who joined a minute ago
// see? Every test here is a place where the two used to differ, and every one
// of those differences had the same cause - the backfill rebuilt an object
// from its spawn identity and left its current condition to a live stream
// that either arrives 40 ms late or, for the cases that matter most, never.

S_PlayerJoin MakeBackfillJoin(uint8_t playerId, const char *nick, float health = 100.0f,
                              uint8_t weapon = 0, float x = 500.0f) {
	S_PlayerJoin j = MakeJoin(playerId, nick);
	j.pos          = {x, 600.0f, 10.0f};
	j.heading      = 1.0f;
	j.health       = health;
	j.armour       = 25.0f;
	j.weapon       = weapon;
	j.flags        = PJF_POS_VALID;
	j.deathAnimId  = ANIM_NONE;
	return j;
}

void TestABackfilledPlayerExistsWithoutASnapshot() {
	std::printf("\njoining next to somebody who is not moving\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;

	// A ped is never created before we know where to put it, and until
	// protocol 9 the only thing that ever said where was a snapshot. A player
	// in the frontend, on a loading screen or in a cutscene has no ped to
	// sample and sends none at all - so a joiner could stand next to them and
	// see nothing for as long as they stayed there.
	c.HandleMessage(Wrap(MakeBackfillJoin(1, "alice", 42.0f, /*weapon=*/6), CH_EVENT));
	c.Tick();

	Check(g_rec.spawns == 1, "she is created from the join packet alone");
	Check(g_rec.lastPose.pos.x == 500.0f, "at the position the session gave");
	Check(c.PlayerSlot(1).last.health == 42.0f && c.PlayerSlot(1).last.armour == 25.0f,
	      "on the health and armour she actually has");
	Check(c.PlayerSlot(1).last.weapon == 6, "holding what she is actually holding");
	// Zero is ANIM_STD_WALK, so a zeroed seed would have a stationary player
	// walking on the spot until their first snapshot arrived.
	Check(c.PlayerSlot(1).last.animId == ANIM_NONE &&
	          c.PlayerSlot(1).last.animId2 == ANIM_NONE,
	      "and no animation guessed at on her behalf");
}

void TestAJoinWithoutAPositionStillCreatesNothing() {
	std::printf("\na player the session has never heard from\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;

	// MakeJoin leaves the flags at zero, which is what the server sends for
	// somebody who connected this instant: their pos is the struct's zero,
	// and the origin in GTA III is the water off Portland.
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.spawns == 0, "nothing is created at the origin");

	FeedPosition(c, 1);
	c.Tick();
	Check(g_rec.spawns == 1, "and she appears once she says where she is");
}

void TestTheSeedGivesWayToTheirOwnStream() {
	std::printf("\nthe join position handing over\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeBackfillJoin(1, "alice"), CH_EVENT));
	c.Tick();
	Check(g_rec.lastPose.pos.x == 500.0f, "the seed places her to begin with");

	FeedPosition(c, 1);
	c.Tick();
	Check(g_rec.lastPose.pos.x != 500.0f, "her own snapshots take over");

	// And the seed does not come back. It is where she was standing when *we*
	// joined, so falling back to it during a later stall would be a teleport
	// across the city dressed up as a recovery.
	const Pose stalled = g_rec.lastPose;
	for (int i = 0; i < 60; ++i)
		c.Tick();
	Check(g_rec.lastPose.pos.x != 500.0f, "and never comes back on a stall");
	Check(g_rec.lastPose.pos.x >= stalled.pos.x,
	      "the buffer holds or extrapolates instead");
}

void TestABackfilledCorpseIsACorpse() {
	std::printf("\njoining while somebody is lying in the road\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;

	// Death is an event, and an event only reaches whoever was connected when
	// it happened. Before this, a joiner got a live player, standing up, on
	// zero health, until the corpse got up by itself.
	S_PlayerJoin dead = MakeBackfillJoin(1, "alice", 0.0f);
	dead.flags |= PJF_DEAD;
	dead.deathAnimId = 17;
	c.HandleMessage(Wrap(dead, CH_EVENT));
	c.Tick();

	Check(g_rec.spawns == 1, "her ped is created");
	Check(g_rec.kills == 1, "and killed on the same frame it appears");
	Check(g_rec.lastKillAnim == 17, "with the animation her own engine chose");

	// Once. The reconciliation runs every frame and SetDie over a ped that
	// has already been through it is not something to do sixty times a
	// second.
	c.Tick();
	c.Tick();
	Check(g_rec.kills == 1, "once, not every frame");

	// And she gets up properly when the session says so.
	c.HandleMessage(Wrap(MakeRespawn(1, 777.0f), CH_EVENT));
	FeedPosition(c, 1);
	c.Tick();
	Check(g_rec.kills == 1, "a respawned player is not killed again");
	Check(g_rec.spawns == 2, "she is rebuilt alive");
}

void TestADeathDuringTheModelStreamIsNotLost() {
	std::printf("\ndying while still streaming in\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = false;
	c.HandleMessage(Wrap(MakeBackfillJoin(1, "alice"), CH_EVENT));
	c.Tick();
	Check(g_rec.spawns == 0, "no ped yet, the model is still loading");

	// The old handler was "if there is a ped, kill it", which quietly dropped
	// the death here and left a player walking around on zero health. Same
	// shape as the join case above, which is why both go through one path.
	c.HandleMessage(Wrap(MakeDeath(1, 13), CH_EVENT));
	c.Tick();
	Check(g_rec.kills == 0, "and nothing to kill");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.spawns == 1, "the ped arrives");
	Check(g_rec.kills == 1 && g_rec.lastKillAnim == 13, "and the death is still waiting");
}

void TestABackfilledCorpseIsNotPutInACar() {
	std::printf("\na corpse the session still has in a seat\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;

	S_PlayerJoin dead = MakeBackfillJoin(1, "alice", 0.0f);
	dead.flags |= PJF_DEAD;
	c.HandleMessage(Wrap(dead, CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleSpawn(9), CH_EVENT));

	S_EnterVehicle in;
	InitHeader(in, 1000);
	in.playerId   = 1;
	in.body       = EnterVehicleBody{};
	in.body.netId = 9;
	in.body.seat  = 0;
	c.HandleMessage(Wrap(in, CH_EVENT));

	c.Tick();
	c.Tick();
	Check(g_rec.seats == 0, "a corpse is warped into nothing");
	Check(g_rec.kills == 1, "and is still a corpse");
}

void TestABackfilledCarArrivesInTheConditionItIsIn() {
	std::printf("\njoining after somebody has wrecked a car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;

	// The owner's bug, from the other end. The spawn used to be read as
	// identity only and the client filled the rest in with "brand new", so a
	// car that had been blown up came back drivable-looking and not drivable:
	// the model was built healthy and the live stream then wrote dead health
	// onto it.
	S_VehicleSpawn hurt = MakeVehicleSpawn(9);
	hurt.health         = 120.0f;
	hurt.flags          = VEH_ENGINE_ON | VEH_SIREN;
	c.HandleMessage(Wrap(hurt, CH_EVENT));
	c.Tick();

	Check(g_rec.vehicleSpawns == 1, "the car is created");
	Check(c.VehicleByNetId(9) != nullptr && c.VehicleByNetId(9)->last.health == 120.0f,
	      "on the health it has actually got left, not a hardcoded 1000");
	Check(g_rec.lastVehicleBody.health == 120.0f,
	      "and that is what reaches the engine seam");
	Check(g_rec.lastVehicleBody.flags == (VEH_ENGINE_ON | VEH_SIREN),
	      "engine and siren along with it, from the spawn rather than 40 ms later");
}

void TestAWreckedSpawnSaysSo() {
	std::printf("\na spawn packet that admits the car is finished\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;

	// The server does not currently put a wreck in a backfill at all, so this
	// only arrives if something upstream decides a wreck is worth showing.
	// What is pinned here is that the bit survives the trip and reaches the
	// seam that would have to act on it, rather than being dropped on the
	// floor the way the health was.
	S_VehicleSpawn wreck = MakeVehicleSpawn(9);
	wreck.health         = 0.0f;
	wreck.flags          = VEH_WRECKED;
	c.HandleMessage(Wrap(wreck, CH_EVENT));
	c.Tick();

	Check(c.VehicleByNetId(9) != nullptr &&
	          (c.VehicleByNetId(9)->last.flags & VEH_WRECKED) != 0,
	      "VEH_WRECKED is kept");
	Check((g_rec.lastVehicleBody.flags & VEH_WRECKED) != 0,
	      "and handed to the vehicle seam, which is whose decision it is");
}

// ---- getting into a car the session already knows about ---------------------
//
// This is the other half of what the owner reported, and it is a late-joiner
// bug for a reason that is worth stating plainly: a car only ever arrives as
// a CVehicle *CoopIII created* for somebody who was not in the session when
// it was claimed. Everyone who was there has it as a car from their own
// world. So with two clients started together this never happens, and the
// first person to restart their game hits it the moment they get into
// anything.
//
// What happened then: the claim went out as a brand new car, the session
// handed back a second netId for a car it already had, and this machine ended
// up driving one number while still observing the other - the same physical
// vehicle. CorrectRemoteVehicles put it back where the session last saw it
// after every frame of physics. "Te podés subir y todo, no se puede manejar
// ni nada."

// Spawns the session's car 80 and hands the local player the wheel of that
// very CVehicle. Returns the engine ref so a test can point the sampler
// somewhere else.
int32_t GiveUsTheSessionsCar(Client &c, uint16_t netId = 80) {
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(netId), CH_EVENT));
	c.Tick();
	const int32_t handle = c.VehicleByNetId(netId) ? c.VehicleByNetId(netId)->poolHandle : -1;
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = handle;
	return handle;
}

void TestTheSessionsOwnCarIsTakenOverNotClaimedTwice() {
	std::printf("\ngetting into a car that came out of the backfill\n");
	Client c;
	c.SetBridge(RecordingBridge());
	const int32_t handle = GiveUsTheSessionsCar(c);
	Check(handle > 0, "the backfilled car was spawned and we are sitting in it");

	// Somebody else's history, from before we got in. Its disappearance is
	// what says the takeover branch ran rather than the claim branch: there
	// is no socket here, so the packet itself cannot be inspected, and this
	// is the side effect that only one of the two paths has.
	c.HandleMessage(Wrap(MakeVehicleState(1, 80, 15.0f), CH_SNAPSHOT));
	Check(c.VehicleByNetId(80)->interp.Size() == 1, "and it has a previous driver's pose");

	c.TickLocalVehicle();
	Check(c.VehicleByNetId(80)->interp.Empty(),
	      "we took it over, rather than registering it a second time");

	// The reply, under the netId it already had - not a new one.
	S_EnterVehicle enter;
	InitHeader(enter, 1000);
	enter.playerId   = 0;
	enter.body       = EnterVehicleBody{};
	enter.body.netId = 80;
	enter.body.seat  = 0;
	c.HandleMessage(Wrap(enter, CH_EVENT));
	Check(c.LocalVehicleNetId() == 80, "and the session agrees it is the same car");
	Check(c.VehicleCount() == 1, "one car, not two");
}

void TestACarWeDriveIsNotCorrectedUnderUs() {
	std::printf("\nthe car you can climb into and cannot move\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsTheSessionsCar(c);
	c.HandleMessage(Wrap(MakeVehicleState(1, 80, 15.0f), CH_SNAPSHOT));

	// While it is still somebody else's, every frame corrects it. That is
	// the design and it is right: an observer undoes the frame of local
	// physics that just ran on a car it is only watching.
	//
	// Stood outside it for this half, deliberately. GiveUsTheSessionsCar puts
	// the local player at the wheel, and a car the *engine* says we are
	// driving is not corrected either - game/vehicle.cpp's CorrectRemoteVehicle
	// has refused that since it was written, off CVehicle::m_pDriver. Leaving
	// the sampler saying "we are driving" here only ever tested the stub.
	g_rec.drivingLocally     = false;
	g_rec.localVehicleHandle = -1;
	const int correctionsBefore = g_rec.vehicleCorrections;
	c.Tick();
	Check(g_rec.vehicleCorrections > correctionsBefore,
	      "a car somebody else is driving is put back every frame");
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = c.VehicleByNetId(80)->poolHandle;

	// Now it is ours.
	S_EnterVehicle enter;
	InitHeader(enter, 1000);
	enter.playerId   = 0;
	enter.body       = EnterVehicleBody{};
	enter.body.netId = 80;
	enter.body.seat  = 0;
	c.HandleMessage(Wrap(enter, CH_EVENT));

	const int corrections = g_rec.vehicleCorrections;
	const int applies     = g_rec.vehicleApplies;
	c.Tick();
	c.Tick();
	c.Tick();
	// This is the bug, as a number. Three frames, three corrections before,
	// and the accelerator did nothing because the last thing to touch the
	// car each frame was CoopIII putting it back.
	Check(g_rec.vehicleCorrections == corrections,
	      "a car we are driving ourselves is not put back at all");
	// Nor is the last driver's health, gear and engine flag written onto it.
	// It is our car now; its condition is ours to report.
	Check(g_rec.vehicleApplies == applies,
	      "and nobody else's controls are written onto it either");

	// Get out, and it goes back to being watched.
	S_ExitVehicle exit;
	InitHeader(exit, 2000);
	exit.playerId = 0;
	exit.netId    = 80;
	c.HandleMessage(Wrap(exit, CH_EVENT));
	g_rec.drivingLocally = false;
	c.Tick();
	Check(g_rec.vehicleCorrections > corrections, "and corrected again once we are out");
}

void TestOurOwnReportsHoldTheCarWeParked() {
	std::printf("\nparking a car we took over\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsTheSessionsCar(c);
	c.TickLocalVehicle();

	S_EnterVehicle enter;
	InitHeader(enter, 1000);
	enter.playerId   = 0;
	enter.body       = EnterVehicleBody{};
	enter.body.netId = 80;
	enter.body.seat  = 0;
	c.HandleMessage(Wrap(enter, CH_EVENT));
	Check(c.VehicleByNetId(80)->interp.Empty(), "its buffer starts empty");

	// Our own snapshots go into it as well as onto the wire. Nothing reads
	// them while we are driving; the moment we step out they are what holds
	// the car where we parked it, which is where every other machine in the
	// session has it. Without this the row has no history at all and the car
	// drifts on local physics on this screen and no other.
	// Not a count: Push drops a sample whose timestamp is not newer than the
	// last one, and two of these run inside the same millisecond. What
	// matters is that our reports land in it at all.
	c.TickLocalVehicle();
	Check(!c.VehicleByNetId(80)->interp.Empty(),
	      "and fills with what we are reporting about it");
}

// ---- being jacked -----------------------------------------------------------
//
// The three things the owner reported on the 2026-09-23 build are one defect
// and this is it: a car has no handover. "Volvió a pasar lo de que se bugeó y
// el auto no avanza o va hacia atrás. O también me pasó que solo iba para
// adelante solo sin que yo apriete la W." And, from the other side of a jack:
// "le robé el auto a otro jugador y no puedo salir de él... en la pantalla del
// otro jugador el auto quedó estático en el lugar viejo."
//
// A carjack happens in exactly one process. The jacker's engine plays the
// animation, drags the replica out of the seat and puts its own player behind
// the wheel; the victim's engine is never told anything and still has *its*
// player behind the wheel. Every ownership guard in the vehicle seam asks
// CVehicle::m_pDriver, so both machines answer "we drive it" and both refuse
// to apply the other's state - the car drives away on one screen and stands
// still on the other, and each end is behaving correctly given what it can see.
//
// The server breaks the tie, because nothing either client can look at does
// (Session::NoteEnterVehicle). What is below is the losing client doing as it
// is told.

void TestBeingJackedHandsTheCarOver() {
	std::printf("\nsomebody jacks the car we are driving\n");
	Client c;
	c.SetBridge(RecordingBridge());
	const int32_t handle = GiveUsTheSessionsCar(c);
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	Check(handle > 0, "we are at the wheel of the session's car 80");

	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(0, 80, 0), CH_EVENT));
	Check(c.LocalVehicleNetId() == 80, "and the session says it is ours");

	// Alice jacks it. This is the only witness: nothing in this process saw
	// her do it.
	c.HandleMessage(Wrap(MakeEnter(1, 80, 0), CH_EVENT));
	const RemoteVehicle *v = c.VehicleByNetId(80);
	Check(v != nullptr && v->driverPlayerId == 1, "alice is recorded at its wheel");
	Check(v->surrendered, "so the row says we have lost it, engine or no engine");
	Check(c.LocalVehicleNetId() == INVALID_NETID,
	      "and we stop being its owner in the session");

	// The handover, once, and then the car is one we watch. Before this
	// existed, m_localVehicleNetId stayed at 80 and DrivenLocally stayed true,
	// so every snapshot alice sent was thrown away and the car sat in the
	// street here while she drove it away there.
	const int corrections = g_rec.vehicleCorrections;
	c.Tick();
	Check(g_rec.vehicleSurrenders == 1, "the seat is handed over");
	c.Tick();
	Check(g_rec.vehicleSurrenders == 1, "once, not once a frame");
	Check(g_rec.vehicleCorrections > corrections,
	      "and her transform is written onto the car again");
}

void TestALostCarIsNotClaimedStraightBack() {
	std::printf("\nthe car we lost is not taken back\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsTheSessionsCar(c);
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(0, 80, 0), CH_EVENT));

	// Jacked, but with the engine still insisting we are the driver - which is
	// the real state on the victim's machine until the handover runs, and is
	// the state the claim path has to survive. The bridge is emptied of the
	// handover so the test can hold that state for as long as it likes.
	WorldBridge b = RecordingBridge();
	// RecordingBridge resets the recorder, so put the car back.
	g_rec.modelReady         = true;
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = c.VehicleByNetId(80)->poolHandle;
	b.SurrenderVehicleSeat   = nullptr;
	c.SetBridge(b);

	c.HandleMessage(Wrap(MakeEnter(1, 80, 0), CH_EVENT));
	Check(c.VehicleByNetId(80)->surrendered, "the row says we lost it");

	// Two things must not happen here, and they are the two arms of the claim.
	// It must not re-claim car 80 - that is a tug of war at the claim rate,
	// and a handover the loser can undo is not a handover. And it must not
	// fall through to the "unknown car" arm either, which would register the
	// CVehicle we are sitting in under a *second* netId and leave every other
	// machine holding two cars for it.
	c.TickLocalVehicle();
	c.TickLocalVehicle();
	Check(c.LocalVehicleNetId() == INVALID_NETID, "and we do not take it back");
	Check(c.VehicleCount() == 1, "nor register it a second time");
	Check(c.VehicleByNetId(80)->driverPlayerId == 1, "it is still alice's");
}

void TestGettingBackOutAndInIsAnOrdinaryClaimAgain() {
	std::printf("\ngetting back into a car we were jacked out of\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsTheSessionsCar(c);
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(0, 80, 0), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80, 0), CH_EVENT));
	c.Tick();   // the handover; the stub takes us out of the seat
	Check(!g_rec.drivingLocally, "we are out of it");

	// `surrendered` is a statement about a contradiction, not a punishment.
	// It has to lapse the moment the engine agrees we are not driving the car,
	// or the row would refuse to be claimed for the rest of the session.
	//
	// One more frame for it, and that is the real shape rather than an
	// allowance the test makes: the flag is cleared by asking the engine, and
	// the pass that hands the seat over has already asked. Nothing depends on
	// which frame it clears in - all it gates is a claim, and there is nothing
	// to claim until the player gets into something.
	c.Tick();
	Check(!c.VehicleByNetId(80)->surrendered,
	      "and the row stops saying we lost it, because there is nothing left "
	      "to contradict");

	// Alice parks it and walks off; we get in again. An ordinary claim.
	c.HandleMessage(Wrap(MakeExit(1, 80), CH_EVENT));
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = c.VehicleByNetId(80)->poolHandle;
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(0, 80, 0), CH_EVENT));
	Check(c.LocalVehicleNetId() == 80, "so the same car can be ours again");
	Check(c.VehicleCount() == 1, "and it is still one car");
}

void TestACarWeAreOnlyWatchingChangingDriverIsNotAHandover() {
	std::printf("\nsomebody else's car changing hands\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(2, "bob"), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.Tick();

	// Two other players trading a car between them is the ordinary case and
	// must cost nothing: the guards already say the car is not ours, so there
	// is no contradiction to resolve and nobody's seat to hand over.
	c.HandleMessage(Wrap(MakeEnter(1, 80, 0), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(2, 80, 0), CH_EVENT));
	c.Tick();
	Check(c.VehicleByNetId(80)->driverPlayerId == 2, "bob took it from alice");
	Check(!c.VehicleByNetId(80)->surrendered, "and nothing of ours was surrendered");
	Check(g_rec.vehicleSurrenders == 0, "so the engine was never asked to hand a seat over");
}

void TestACarFromOurOwnWorldIsStillClaimedNormally() {
	std::printf("\ngetting into an ordinary car off the street\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsTheSessionsCar(c);
	c.HandleMessage(Wrap(MakeVehicleState(1, 80, 15.0f), CH_SNAPSHOT));

	// The common case, and the only one that existed before: the car we are
	// in is one our own engine made, so no row matches its ref and the claim
	// has to introduce it with its full identity.
	g_rec.localVehicleHandle = 4242;
	c.TickLocalVehicle();
	Check(c.VehicleByNetId(80)->interp.Size() == 1,
	      "car 80 is left alone - we are not in it");
	Check(c.LocalVehicleNetId() == INVALID_NETID,
	      "and we are waiting for a netId of our own");
}

// ---- the ownership handoff --------------------------------------------------
//
// A session car has exactly one owner at a time: whoever is in seat 0. The
// owner simulates it and sends C_VehicleState; everybody else corrects it and
// must not write anything that makes their engine act on it. Ownership moves
// only through the reliable, ordered enter/exit pair, and a car with no owner
// is simulated by nobody.
//
// Everything below is a hole in that rule that the 2026-09-22 session fell
// into, with the log line that shows it.

void TestClaimingOurOwnCarKeepsARowForIt() {
	std::printf("\nthe car we claimed stays in the roster\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady         = true;
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = 4242;   // one of our own engine's cars

	// The claim goes out and the server answers with a fresh netId. The
	// server deliberately does NOT send us the S_VehicleSpawn it broadcasts
	// to everybody else - we already have the car. That used to leave this
	// machine as the only one in the session with no record of it.
	S_EnterVehicle enter;
	InitHeader(enter, 1000);
	enter.playerId     = 0;
	enter.body         = EnterVehicleBody{};
	enter.body.netId   = 364;
	enter.body.seat    = 0;
	enter.body.modelId = 104;
	enter.body.extra1  = 3;
	enter.body.extra2  = -1;
	c.HandleMessage(Wrap(enter, CH_EVENT));

	Check(c.LocalVehicleNetId() == 364, "the reply names our car");
	const RemoteVehicle *v = c.VehicleByNetId(364);
	Check(v != nullptr, "and the roster now has a row for it");
	Check(v->poolHandle == 4242, "pointing at the CVehicle we are sitting in");
	Check(v->ours, "marked as ours, so nothing spawns or destroys it");
	Check(v->driverPlayerId == 0, "with us recorded as the driver");
	Check(v->modelId == 104 && v->extra1 == 3 && v->extra2 == -1,
	      "and the identity we claimed it with");

	// It must not be treated as a car waiting to be built.
	const int spawns = g_rec.vehicleSpawns;
	c.Tick();
	c.Tick();
	Check(g_rec.vehicleSpawns == spawns,
	      "the car we are sitting in is never spawned a second time");
	Check(g_rec.vehicleApplies == 0,
	      "and nobody writes controls onto a car we are driving");
}

void TestGettingBackIntoOurOwnCarDoesNotRegisterItTwice() {
	std::printf("\ngetting out of our own car and back into it\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady         = true;
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = 4242;

	S_EnterVehicle enter;
	InitHeader(enter, 1000);
	enter.playerId     = 0;
	enter.body         = EnterVehicleBody{};
	enter.body.netId   = 364;
	enter.body.seat    = 0;
	enter.body.modelId = 104;
	c.HandleMessage(Wrap(enter, CH_EVENT));

	// Out. The car stays: somebody parked it, it did not stop existing.
	g_rec.drivingLocally = false;
	c.TickLocalVehicle();
	Check(c.LocalVehicleNetId() == INVALID_NETID, "we are on foot");
	Check(c.VehicleByNetId(364) != nullptr,
	      "and the car is still a car the session and this machine both know");
	Check(c.VehicleByNetId(364)->driverPlayerId == 0xFF, "with nobody driving it");

	// Back in, same CVehicle. This is the moment that used to produce a
	// second netId for one physical car - 364 and 475 in the session log,
	// same model 104, same extras 3/-1, no despawn in between - and leave
	// every observer holding two cars, one of which nobody drives.
	g_rec.drivingLocally = true;
	c.TickLocalVehicle();
	Check(c.VehicleCount() == 1, "one car, not two");
	Check(c.LocalVehicleNetId() == INVALID_NETID,
	      "the re-claim is out and we are waiting for the answer");

	S_EnterVehicle again;
	InitHeader(again, 3000);
	again.playerId   = 0;
	again.body       = EnterVehicleBody{};
	again.body.netId = 364;   // the same number, because we asked by netId
	again.body.seat  = 0;
	c.HandleMessage(Wrap(again, CH_EVENT));
	Check(c.LocalVehicleNetId() == 364, "and it is the same car it always was");
	Check(c.VehicleCount() == 1, "still one car");
	Check(c.VehicleByNetId(364)->modelId == 104,
	      "a re-claim carries only the netId, so it must not wipe the identity");
}

void TestOurOwnCarSurvivesTheSessionEnding() {
	std::printf("\nlosing the session does not delete our own car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady         = true;
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = 4242;
	c.HandleMessage(Wrap(MakeEnter(0, 364), CH_EVENT));
	Check(c.VehicleByNetId(364) != nullptr, "we have a row for our own car");

	// DespawnRemoteVehicle runs the engine's deleting destructor. On a
	// replica CoopIII built that is right; on one of this engine's own
	// traffic cars - with the player quite possibly sitting in it - it is a
	// CVehicle deleted underneath them because a socket closed.
	const int despawns = g_rec.vehicleDespawns;
	c.HandleMessage(Wrap(MakeVehicleSpawn(90), CH_EVENT));
	c.Tick();
	c.ClearRosterForTest();
	Check(g_rec.vehicleDespawns == despawns + 1,
	      "the replica we built is destroyed, exactly as before");
	Check(c.VehicleCount() == 0, "and the roster is empty either way");
}

void TestADriverlessCarIsPutAtRest() {
	std::printf("\na car nobody is driving is simulated by nobody\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.Tick();

	// Alice drives it, at speed.
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	S_VehicleState fast = MakeVehicleState(1, 80, 15.0f);
	fast.body.moveSpeed = Vec3{0.9f, 0.0f, 0.0f};
	fast.body.gas       = 1.0f;
	c.HandleMessage(Wrap(fast, CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastVehicleBody.moveSpeed.x == 0.9f,
	      "her car is written through with the speed she is doing");

	// She gets out. Her last snapshot is now a lie about a parked car, and
	// C_VehicleState is unreliable and unordered, so it is not even reliably
	// the last one she sent. Replayed every frame it holds m_vecMoveSpeed
	// above CVehicle::CanPedEnterCar's 0.04 forever, and nobody can ever get
	// in: CPed::SeekCar answers the refusal with RestorePreviousState and
	// leaves the objective standing, so the player walks to the door, is
	// refused, and walks to the door again.
	c.HandleMessage(Wrap(MakeExit(1, 80), CH_EVENT));
	const int restBefore = g_rec.vehicleAtRest;
	c.Tick();
	Check(c.VehicleByNetId(80)->driverPlayerId == 0xFF, "nobody is driving it");
	Check(g_rec.lastVehicleBody.moveSpeed.x == 0.9f,
	      "the roster still holds her last snapshot - it is the car's history");
	Check(g_rec.vehicleAtRest == restBefore + 1,
	      "but what reaches the engine is a car at rest, not her last frame");

	// And it stays at rest for as long as it stays parked. Once-on-the-
	// transition is not enough: the transform is pinned after physics every
	// frame, so a car whose pinned position is a hair off the ground would
	// otherwise accumulate gravity into m_vecMoveSpeed.z with nothing ever
	// taking it out again.
	c.Tick();
	c.Tick();
	Check(g_rec.vehicleAtRest == restBefore + 3,
	      "every frame it is parked, not just the first");

	// She gets back in and it is hers to drive again.
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	const int rest = g_rec.vehicleAtRest;
	c.Tick();
	Check(g_rec.vehicleAtRest == rest, "and it is simulated again once she is back in");
	Check(g_rec.lastVehicleBody.gas == 1.0f, "with her controls, not zeros");
}

// ---- the player arrow on the radar ----------------------------------------
//
// No unit test can draw anything, so what these pin is the arithmetic that
// decides what gets drawn - and, more to the point, that it is the retail
// arithmetic. CRadar::DrawRotatingRadarSprite is not called; its body is
// reproduced so the arrow can be given a colour, and a reproduction that has
// quietly drifted from the original is the failure this section exists to
// catch. Every number below is quoted from the disassembly in addresses.h.

bool NearEnough(float a, float b, float tol = 0.0005f) {
	const float d = a - b;
	return (d < 0.0f ? -d : d) <= tol;
}

void TestArrowSpriteSize() {
	std::printf("\nthe arrow is the size the engine draws its own\n");

	// SCREEN_SCALE_X(8.0) at the reference width is 8 exactly. That is the
	// radius the four corners sit on rather than half the width of the quad,
	// so the arrow is about 11 px on a side at 640x448 - see TestArrowQuad.
	Check(NearEnough(RadarSpriteHalf(640, 1.0f / HUD_REF_WIDTH), 8.0f),
	      "8 px half-extent at the HUD's own 640, from the 8.0 at 0x005F7174");
	Check(NearEnough(RadarSpriteHalf(448, 1.0f / HUD_REF_HEIGHT), 8.0f),
	      "and 8 at 448, so the arrow starts square");

	// The part that is retail rather than re3. 1366 * (1/640) * 8 is 17.075,
	// and 0x004A5D10 puts it through `fistp` with the control word set to
	// round toward zero. re3 leaves it a float.
	Check(NearEnough(RadarSpriteHalf(1366, 1.0f / HUD_REF_WIDTH), 17.0f),
	      "17, not 17.075: retail truncates both half-extents to whole pixels");
	Check(NearEnough(RadarSpriteHalf(1920, 1.0f / HUD_REF_WIDTH), 24.0f),
	      "24 at 1920, where the truncation happens to be exact");

	// No floor under the truncation, deliberately. The game's own arrow
	// disappears at the same width for the same reason, and an arrow CoopIII
	// kept visible on a radar with no arrow on it would be worse than none.
	Check(RadarSpriteHalf(40, 1.0f / HUD_REF_WIDTH) == 0.0f,
	      "below about 80 px of screen the engine's own arrow truncates away too");
	Check(RadarSpriteHalf(0, 1.0f / HUD_REF_WIDTH) == 0.0f &&
	          RadarSpriteHalf(-100, 1.0f / HUD_REF_WIDTH) == 0.0f &&
	          RadarSpriteHalf(1920, 0.0f) == 0.0f,
	      "and nothing nonsensical multiplies its way through");
}

void TestArrowQuad() {
	std::printf("\nthe four corners, which are DrawRotatingRadarSprite's\n");

	// i*HALFPI + (angle - PI/4), x = cx + sin(a)*halfX, y = cy + cos(a)*halfY.
	// At angle 0 the first corner is at -PI/4, so sin is -1/sqrt(2) and cos is
	// +1/sqrt(2): down and to the left of centre, down because screen y grows
	// downwards.
	const float k    = 0.70710678f;
	ArrowQuad   flat = MakeArrowQuad(100.0f, 200.0f, 0.0f, 10.0f, 10.0f);
	Check(NearEnough(flat.x[0], 100.0f - 10.0f * k) &&
	          NearEnough(flat.y[0], 200.0f + 10.0f * k),
	      "corner 0 at angle 0 is the PI/4 correction, nothing else");

	// Four corners a quarter turn apart, each exactly one half-extent from
	// the centre - the corners sit on the circle of radius halfX, not on the
	// corners of a halfX square. So the arrow's sides are sqrt(2) * halfX
	// long, about 11 px where a DrawRadarSprite blip is 16, and it is smaller
	// than the compass on the same radar. That is the engine's, not a
	// rounding error, and it is the size the local player's own arrow is.
	for (int i = 0; i < 4; ++i) {
		const float dx = flat.x[i] - 100.0f;
		const float dy = flat.y[i] - 200.0f;
		Check(NearEnough(std::sqrt(dx * dx + dy * dy), 10.0f, 0.001f),
		      "every corner sits one half-extent from the centre, so the quad is "
		      "sqrt(2) half-extents on a side");
	}

	// The whole point of the sprite. Turn the player a quarter turn and the
	// quad turns with them, which is what a square blip could never do.
	ArrowQuad turned = MakeArrowQuad(100.0f, 200.0f, RADAR_HALFPI, 10.0f, 10.0f);
	Check(NearEnough(turned.x[0], flat.x[1], 0.001f) &&
	          NearEnough(turned.y[0], flat.y[1], 0.001f),
	      "a quarter turn moves corner 0 onto where corner 1 was");

	// Non-square half-extents, which is what a 16:9 screen gives, have to
	// stretch the quad rather than rotate it wrong.
	ArrowQuad wide = MakeArrowQuad(0.0f, 0.0f, 0.0f, 20.0f, 10.0f);
	Check(NearEnough(wide.x[0], -20.0f * k) && NearEnough(wide.y[0], 10.0f * k),
	      "halfX and halfY apply to x and y separately, as the engine does");

	// The tail of 0x004A5D10 draws curPosn 3, 2, 0, 1. A triangle fan wants
	// its corners walked round the quad; this order wrong is a bow tie, not a
	// rotated square.
	Check(ARROW_DRAW_ORDER[0] == 3 && ARROW_DRAW_ORDER[1] == 2 &&
	          ARROW_DRAW_ORDER[2] == 0 && ARROW_DRAW_ORDER[3] == 1,
	      "and they go to CSprite2d::Draw in the order 3, 2, 0, 1");
}

void TestArrowAngle() {
	std::printf("\nwhich way the arrow points\n");

	// TransformRealWorldPointToRadarSpace hands back (sin, cos) of the angle
	// the radar is rotated by, so the angle comes back out of an atan2.
	for (float a = -3.0f; a < 3.1f; a += 0.75f) {
		Check(NearEnough(RadarCameraHeading(std::sin(a), std::cos(a)), a, 0.001f),
		      "the camera heading round-trips through sin and cos");
	}

	// DrawBlips' own expression, heading - (PI + forward.Heading()). Facing
	// the same way as the camera gives -PI, which is exactly the angle your
	// own arrow is drawn at, and is the one case worth naming.
	Check(NearEnough(ArrowAngle(1.25f, 1.25f), -RADAR_PI),
	      "facing where the camera faces is the same angle as your own arrow");

	// The top-down branch of DrawBlips uses PI + heading instead of
	// heading - PI, and recovering the camera heading from the transform
	// gives 0 there, so this produces the other one. They are the same angle.
	const float topDown = ArrowAngle(0.4f, 0.0f);
	Check(NearEnough(std::sin(topDown), std::sin(0.4f + RADAR_PI), 0.001f) &&
	          NearEnough(std::cos(topDown), std::cos(0.4f + RADAR_PI), 0.001f),
	      "and heading - PI is heading + PI, which is the top-down camera case "
	      "handled without a second branch");
}

void TestArrowColour() {
	std::printf("\nwhat colour a remote player is, and why\n");

	// Not chosen: the two colours the game itself attaches to a meaning.
	// ADD_BLIP_FOR_CHAR (handler 0x0044079E) pushes 1, ADD_BLIP_FOR_CAR
	// pushes 0. The square this replaced was already these two.
	Check(ArrowTraceColour(false) == RADAR_TRACE_GREEN && RADAR_TRACE_GREEN == 1,
	      "on foot: green, which is ADD_BLIP_FOR_CHAR's own colour");
	Check(ArrowTraceColour(true) == RADAR_TRACE_RED && RADAR_TRACE_RED == 0,
	      "in a car: red, which is ADD_BLIP_FOR_CAR's");
	Check(ArrowTraceColour(true) != ArrowTraceColour(false),
	      "so a driving player still reads differently, as the square did");

	// GetRadarTraceColour's green arm is `mov eax,5FA06AFFh` at 0x004A5BE4,
	// taken when m_bDim is set - which is what every script blip in the game
	// gets. DrawBlips takes the result apart as >>24, >>16, >>8.
	const ArrowRgb green = UnpackTraceColour(0x5FA06AFFu);
	Check(green.r == 0x5F && green.g == 0xA0 && green.b == 0x6A,
	      "0x5FA06AFF unpacks as 0xRRGGBBAA, which is how DrawBlips reads it");

	// The low byte looks like an alpha and is not used as one by anything.
	// DrawBlips passes its own, and so does this file - CalculateBlipAlpha's,
	// so a player pinned to the rim comes out dimmer.
	const ArrowRgb lowByteIgnored = UnpackTraceColour(0x11223300u);
	Check(lowByteIgnored.r == 0x11 && lowByteIgnored.g == 0x22 &&
	          lowByteIgnored.b == 0x33,
	      "and the low byte is ignored rather than becoming the alpha");

	// What is drawn now: the player's chat colour, darker in a vehicle.
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		const NickColour chat = ChatNickColour(id);
		const ArrowRgb   foot = PlayerArrowRgb(id, false);
		const ArrowRgb   car  = PlayerArrowRgb(id, true);
		if (foot.r != chat.r || foot.g != chat.g || foot.b != chat.b ||
		    !(car.r < foot.r && car.g < foot.g && car.b < foot.b)) {
			Check(false, "every player's arrow is their chat colour, darker in a vehicle");
			return;
		}
	}
	Check(true, "every player's arrow is their chat colour, darker in a vehicle");
}

void TestArrowWanted() {
	std::printf("\nwho gets an arrow\n");

	Check(ArrowWanted(true, true),
	      "an active player something has said a position for");
	Check(!ArrowWanted(false, true), "an empty slot gets nothing");
	Check(!ArrowWanted(true, false),
	      "nor does a player whose first snapshot has not arrived - a marker at "
	      "the world origin is a marker in the water off Portland");

	// Only ever read, never divided by without this. DrawMap writes it every
	// frame, 120 m on foot ramping to 350 in a fast car.
	Check(RadarRangeUsable(RADAR_RANGE_ON_FOOT_M) &&
	          RadarRangeUsable(RADAR_RANGE_AT_SPEED_M),
	      "120 m on foot and 350 in a fast car are both usable ranges");
	Check(!RadarRangeUsable(0.0f) && !RadarRangeUsable(-1.0f),
	      "a zero or negative radar range is refused rather than used");
}

void TestArrowSpriteTable() {
	std::printf("\nthe sprite, and where it lives\n");

	// The table at 0x005F6CA4 reads 0, 008F1A40, 008F5FB4, 00885B24,
	// 008F6268, ... and CRadar::LoadTextures gives 0x008F6268 the texture
	// "radar_centre" (0x005F7020). Entry 14 is 008F6274, and that is what
	// DrawBlips draws the compass with as `push 0Eh / call 004A5EF0` - the
	// cross-check that makes the indices measured rather than copied out of
	// re3's enum.
	Check(CRadar__RadarSprites + RADAR_SPRITE_CENTRE * 4 == 0x005F6CB4,
	      "RadarSprites[4] is the slot holding CRadar::CentreSprite");
	Check(CRadar__RadarSprites + RADAR_SPRITE_NORTH * 4 == 0x005F6CDC,
	      "and RadarSprites[14] is NorthSprite, which DrawBlips asks for by "
	      "index - so the numbering is witnessed twice");
	Check(CRadar__CentreSprite == 0x008F6268,
	      "CentreSprite itself, from LoadTextures' `mov ecx,008F6268h`");

	// The reason none of this can be a blip. A table entry with
	// m_eRadarSprite set to 4 would draw the arrowhead - through
	// DrawRadarSprite, which does not rotate. The rotation exists only on the
	// path the local player's own marker takes.
	Check(RADAR_SPRITE_CENTRE != RADAR_SPRITE_NONE,
	      "the arrowhead is a sprite index, but not one DrawBlips rotates");
}

// ---- cars nobody owns (docs/roadmap.md 5.8) --------------------------------
//
// The engine half is not reachable from here - the decision is. And the
// decision is the half that was missing: C_VehicleBlowUp is sent by a driver,
// a parked car has none, so nothing about one ever reached the wire.

S_UnownedBlowUp MakeUnownedBlowUp(uint16_t id, uint8_t kind = UNOWNED_PARKED,
                                  uint8_t reporter = 1, Vec3 at = {0.0f, 0.0f, 0.0f}) {
	S_UnownedBlowUp b{};
	InitHeader(b, 4000);
	b.where.pos = at;
	b.where.rot = {0.0f, 0.0f, 0.0f, 1.0f};
	b.reporterPlayerId = reporter;
	b.pad[0] = b.pad[1] = b.pad[2] = 0;
	b.key.kind = kind;
	b.key.pad  = 0;
	b.key.id   = id;
	return b;
}

void TestAnUnownedWreckIsAppliedOnTheNextFrame() {
	std::printf("\na parked car the session says is finished gets wrecked here\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// Nothing happens in the handler. The car may be three streets away and
	// not streamed in, which is the normal case for a backfill, so the packet
	// records a standing instruction and the frame pump carries it out.
	c.HandleMessage(Wrap(MakeUnownedBlowUp(412), CH_EVENT));
	Check(g_rec.unownedWrecks == 0, "the handler applies nothing by itself");

	c.Tick();
	Check(g_rec.unownedWrecks == 1, "the frame pump does");
	Check(g_rec.lastUnownedKey.id == 412, "and names the generator it was told");
	Check(g_rec.lastUnownedKey.kind == UNOWNED_PARKED, "with the kind intact");

	// Retired once it lands. Asking the engine to blow up the same car every
	// frame for the rest of the session would be a wreck with a permanent
	// explosion on it.
	c.Tick();
	Check(g_rec.unownedWrecks == 1, "and it is asked exactly once");
}

void TestAnUnownedWreckThatIsNotHereYetIsRetried() {
	std::printf("\na wreck for a car that has not streamed in is kept\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	g_rec.unownedOutcome = UnownedWreckOutcome::NotHere;
	c.HandleMessage(Wrap(MakeUnownedBlowUp(5), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.unownedWrecks == 2, "asked again on the next frame");

	// And it stops the moment the car turns up, rather than on a timer.
	g_rec.unownedOutcome = UnownedWreckOutcome::Wrecked;
	c.Tick();
	c.Tick();
	Check(g_rec.unownedWrecks == 3, "and stops as soon as it lands");
}

void TestAnUnownedWreckThatIsAlreadyAWreckIsDropped() {
	std::printf("\nthe ordinary case: our own engine got there first\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// This is what usually happens, and it is why the feature is small: the
	// explosion that destroyed the car was replayed on this machine too, so
	// the car is already a wreck when the packet lands.
	g_rec.unownedOutcome = UnownedWreckOutcome::Already;
	c.HandleMessage(Wrap(MakeUnownedBlowUp(6), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.unownedWrecks == 1, "asked once and then let go");
}

void TestAnUnresolvableUnownedKeyIsDropped() {
	std::printf("\na key this machine can never resolve is dropped\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// A generator index past the map's own range, or a kind we do not speak.
	// Retrying would be sixty asks a second forever for a car that does not
	// exist here and never will.
	g_rec.unownedOutcome = UnownedWreckOutcome::BadKey;
	c.HandleMessage(Wrap(MakeUnownedBlowUp(9999), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.unownedWrecks == 1, "asked once and then let go");
}

void TestAParkedSessionCarIsBlownUpThroughTheRoster() {
	std::printf("\na session car with no driver, blown up by somebody else\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(55), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "the car is there to begin with");

	// Not through WreckUnownedVehicle: a netId is the session's name for a
	// car, not the engine's, so the roster is what resolves it - and it
	// resolves to the same BlowUpRemoteVehicle a driver's own blast uses.
	c.HandleMessage(Wrap(MakeUnownedBlowUp(55, UNOWNED_SESSION), CH_EVENT));
	c.Tick();
	Check(g_rec.unownedWrecks == 0, "the engine-key seam is not asked");
	Check(g_rec.blowUps == 1, "the roster's own blow-up is");
	Check(g_rec.lastBlowUpNetId == 55, "and it names the car");

	const RemoteVehicle *v = c.VehicleByNetId(55);
	Check(v != nullptr && v->destroyed, "the roster knows it is a wreck");

	c.Tick();
	Check(g_rec.blowUps == 1, "and it is asked exactly once");
}

void TestASessionCarWeDoNotHaveIsDropped() {
	std::printf("\na session blast for a car this machine never heard of\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// Dropped rather than retried. The packet carries no model, so there is
	// nothing to build from, and a blast is not a reason to invent a car -
	// the same rule OnVehicleBlowUp follows.
	c.HandleMessage(Wrap(MakeUnownedBlowUp(404, UNOWNED_SESSION), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.blowUps == 0, "nothing reaches the bridge");
	Check(c.VehicleCount() == 0, "and no vehicle is created");
}

void TestTheSameUnownedCarIsOnlyHeldOnce() {
	std::printf("\ntwo reports of one parked car are one instruction\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// The server drops a duplicate, but the backfill and a live relay can
	// still both name the same car to a joiner within a frame of each other.
	g_rec.unownedOutcome = UnownedWreckOutcome::NotHere;
	c.HandleMessage(Wrap(MakeUnownedBlowUp(77), CH_EVENT));
	c.HandleMessage(Wrap(MakeUnownedBlowUp(77, UNOWNED_PARKED, 2), CH_EVENT));
	c.Tick();
	Check(g_rec.unownedWrecks == 1, "one attempt per frame, not two");
}



// ---- ammunition (docs/protocol.md 1.9.6) -----------------------------------

S_PlayerAmmo MakeAmmo(uint8_t playerId, uint8_t weapon, uint16_t clip, uint32_t total,
                      bool owned = true) {
	S_PlayerAmmo a;
	InitHeader(a, 1000);
	a.playerId    = playerId;
	a.slot.weapon = weapon;
	a.slot.flags  = owned ? AMMO_SLOT_OWNED : 0;
	a.slot.clip   = clip;
	a.slot.total  = total;
	return a;
}

void TestTheSwitchReachesTheEngine() {
	std::printf("\nthe ammo switch comes off the welcome\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	Check(g_rec.ammoSyncCalls == 1, "the seam is told, once");
	Check(!g_rec.ammoSync, "and a welcome with no flags means off");

	Client on;
	on.SetBridge(RecordingBridge());
	S_Welcome w = MakeWelcome(0);
	w.flags     = SESSION_AMMO_SYNC;
	on.HandleMessage(Wrap(w, CH_EVENT));
	Check(g_rec.ammoSync, "and the bit is what turns it on");

	// The two session flags are independent: a server may want honest ammo
	// without letting players shoot each other.
	Client ff;
	ff.SetBridge(RecordingBridge());
	S_Welcome only = MakeWelcome(0);
	only.flags     = SESSION_FRIENDLY_FIRE;
	ff.HandleMessage(Wrap(only, CH_EVENT));
	Check(g_rec.friendlyFire && !g_rec.ammoSync,
	      "friendly fire on does not drag ammo sync on with it");
}

void TestAmmoIsIgnoredWithTheSwitchOff() {
	std::printf("\nwith ammo sync off nothing is read and nothing is sent\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// An S_PlayerAmmo that arrives anyway is ignored rather than
	// applied - the server should not have sent it, and a client that
	// trusted it would be honest about ammunition in a session that said
	// it would not be.
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	FeedPosition(c, 1);
	g_rec.modelReady = true;
	c.Tick();
	c.HandleMessage(Wrap(MakeAmmo(1, 4, 8, 40), CH_EVENT));
	Check(g_rec.ammoApplies == 0, "a stray ammo packet is not applied");
	Check(!c.PlayerSlot(1).ammoKnown[4], "and nothing is recorded for that slot");
}

void TestAStoredSlotIsRecordedAndApplied() {
	std::printf("\na weapon somebody is carrying but not holding\n");
	Client c;
	c.SetBridge(RecordingBridge());
	S_Welcome w = MakeWelcome(0);
	w.flags     = SESSION_AMMO_SYNC;
	c.HandleMessage(Wrap(w, CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	FeedPosition(c, 1);
	g_rec.modelReady = true;
	c.Tick();
	Check(c.PlayerSlot(1).poolHandle >= 0, "alice has a ped");

	// The snapshots FeedPosition sent already taught the roster about the
	// weapon alice is holding, and the spawn replayed that one. Measure the
	// delta, not the total.
	const int spawned = g_rec.ammoApplies;

	c.HandleMessage(Wrap(MakeAmmo(1, 4, 8, 40), CH_EVENT));
	Check(c.PlayerSlot(1).ammoKnown[4], "the slot is recorded");
	Check(c.PlayerSlot(1).ammoClip[4] == 8, "clip");
	Check(c.PlayerSlot(1).ammoTotal[4] == 40, "total");
	Check(g_rec.ammoApplies == spawned + 1,
	      "and written onto the ped straight away");
	Check(g_rec.lastAmmoPlayer == 1 && g_rec.lastAmmoSlot.weapon == 4,
	      "for the right player and the right slot");

	// A slot number that is not one of the thirteen would index past
	// CPed::m_weapons and into m_currentWeapon. It has to be refused here as
	// well as on the server, because the server is not the only thing that
	// can put a packet on this socket.
	const int before = g_rec.ammoApplies;
	c.HandleMessage(Wrap(MakeAmmo(1, 13, 1, 1), CH_EVENT));
	c.HandleMessage(Wrap(MakeAmmo(1, 200, 1, 1), CH_EVENT));
	Check(g_rec.ammoApplies == before, "a slot that is not a weapon is dropped");

	// Our own id is never a remote. The server does not send us our own, but
	// the client is what has to be sure.
	c.HandleMessage(Wrap(MakeAmmo(0, 2, 5, 5), CH_EVENT));
	Check(g_rec.ammoApplies == before, "and neither is our own player id");

	// Putting the weapon down. The packet still reaches the seam - a ped
	// holding a shotgun it no longer owns has to be told - but the roster
	// stops claiming it, so a rebuilt ped is not handed it again.
	c.HandleMessage(Wrap(MakeAmmo(1, 4, 0, 0, /*owned*/ false), CH_EVENT));
	Check(!c.PlayerSlot(1).ammoKnown[4], "an unowned slot goes back to unknown");
	Check(c.PlayerSlot(1).ammoTotal[4] == 0, "and its count with it");
	Check(g_rec.ammoApplies == before + 1, "the seam is still told");
	Check((g_rec.lastAmmoSlot.flags & AMMO_SLOT_OWNED) == 0,
	      "and told that they do not have it, not that they have none of it");
}

void TestTheHeldWeaponArrivesOnTheSnapshot() {
	std::printf("\nthe held weapon's count rides the snapshot\n");
	Client c;
	c.SetBridge(RecordingBridge());
	S_Welcome w = MakeWelcome(0);
	w.flags     = SESSION_AMMO_SYNC;
	c.HandleMessage(Wrap(w, CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	S_PlayerState s;
	InitHeader(s, 1000);
	s.playerId       = 1;
	s.body           = PlayerStateBody{};
	s.body.pos       = {10.0f, 20.0f, 3.0f};
	s.body.weapon    = 2;    // WEAPONTYPE_COLT45
	s.body.ammoClip  = 11;
	s.body.ammoTotal = 83;
	c.HandleMessage(Wrap(s, CH_EVENT));

	Check(c.PlayerSlot(1).ammoKnown[2], "the held slot is learned from the snapshot");
	Check(c.PlayerSlot(1).ammoClip[2] == 11, "clip");
	Check(c.PlayerSlot(1).ammoTotal[2] == 83, "total");
	Check(g_rec.ammoApplies == 0,
	      "and it does not go through ApplyRemoteAmmo - the pose does that");

	// The next one corrects it, which is the whole reason this rides an
	// unreliable stream rather than a reliable packet per bullet.
	s.body.ammoClip = 10;
	s.body.ammoTotal = 82;
	c.HandleMessage(Wrap(s, CH_EVENT));
	Check(c.PlayerSlot(1).ammoClip[2] == 10, "a later snapshot corrects it");
}

void TestAFreshPedIsHandedTheWholeInventory() {
	std::printf("\na ped that spawns late still gets what its owner is carrying\n");
	Client c;
	c.SetBridge(RecordingBridge());
	S_Welcome w = MakeWelcome(0);
	w.flags     = SESSION_AMMO_SYNC;
	c.HandleMessage(Wrap(w, CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	FeedPosition(c, 1);

	// The backfill arrives while the model is still streaming, which is the
	// ordinary case: C_PlayerAmmo is reliable and a model is not.
	g_rec.modelReady = false;
	c.HandleMessage(Wrap(MakeAmmo(1, 4, 8, 40), CH_EVENT));
	c.HandleMessage(Wrap(MakeAmmo(1, 5, 0, 3), CH_EVENT));
	c.Tick();
	Check(g_rec.ammoApplies == 0, "nothing is written while there is no ped");
	Check(c.PlayerSlot(1).ammoKnown[4] && c.PlayerSlot(1).ammoKnown[5],
	      "but both slots are remembered");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.spawns == 1, "the ped exists now");
	// Three: the shotgun, the grenades, and the weapon the snapshots said
	// alice is holding, which the roster learned the same way.
	Check(g_rec.ammoApplies == 3, "every known slot was replayed onto it");

	const int afterSpawn = g_rec.ammoApplies;

	// Once, not every frame afterwards.
	c.Tick();
	c.Tick();
	Check(g_rec.ammoApplies == afterSpawn,
	      "the replay is a one-off, not a per-frame write");
}



} // namespace


// ---- pickups ---------------------------------------------------------------
//
// Client's whole job here is routing. The decision about who gets a pickup is
// the server's and what happens to it is the engine's; what this layer does
// is read one S_PickupTaken as "ours" or "somebody else's", and that is a
// single comparison which is easy to write backwards and impossible to notice
// afterwards - getting it inverted means every player watches their own
// pickups be collected by a stranger while quietly taking everyone else's.

S_PickupTaken MakeTaken(uint8_t playerId, float x, uint8_t type = 2) {
	S_PickupTaken t;
	InitHeader(t, 1000);
	t.playerId         = playerId;
	t.ident.pos        = {x, 5.0f, 1.0f};
	t.ident.modelIndex = 100;
	t.ident.type       = type;
	t.ident.flags      = 0;
	return t;
}

S_PickupGrant MakeGrant(float x, uint8_t type = 2) {
	S_PickupGrant g;
	InitHeader(g, 1000);
	g.ident.pos        = {x, 5.0f, 1.0f};
	g.ident.modelIndex = 100;
	g.ident.type       = type;
	g.ident.flags      = 0;
	return g;
}

// ---- a car's damage model (docs/cardamage.md) ------------------------------

S_VehicleDamage MakeVehicleDamage(uint16_t netId, uint32_t panels, uint16_t doors,
                           uint8_t from = 1) {
	S_VehicleDamage d{};
	InitHeader(d, 1000);
	d.playerId    = from;
	d.body.netId  = netId;
	d.body.panels = panels;
	d.body.doors  = doors;
	return d;
}

void TestTheDoorByteIsNotTheDamage() {
	std::printf("\na door somebody opened is not a door somebody broke\n");

	// The engine's own enum is OK, SMASHED, SWINGING, MISSING - and that is
	// not a damage ordering. SWINGING sits above SMASHED and means less than
	// it. Eight sites in CPed write SWINGING and one writes it back to OK,
	// every time a passenger gets in or out.
	Check(game::DoorLevel(DOOR_STATUS_OK) == game::DOORDMG_NONE,
	      "a shut door is undamaged");
	Check(game::DoorLevel(DOOR_STATUS_SWINGING) == game::DOORDMG_NONE,
	      "and so is an open one, however far up the enum it sits");
	Check(game::DoorLevel(DOOR_STATUS_SMASHED) == game::DOORDMG_SMASHED,
	      "a dented door is damage");
	Check(game::DoorLevel(DOOR_STATUS_MISSING) == game::DOORDMG_GONE,
	      "a missing one is more of it");

	// This is the trap the mapping exists for: CPed writes SWINGING over
	// MISSING unconditionally (0x004DE71E, `push 2 / push door / call
	// SetDoorStatus`), so the raw byte can go DOWN while the car still has a
	// hole in it - nothing puts a hidden atomic back except a respray. Sending
	// the raw byte would ask observers to un-lose a door.
	Check(DOOR_STATUS_SWINGING > DOOR_STATUS_SMASHED,
	      "the raw byte's order is the engine's, and it is not monotone");
	Check(game::DoorLevel(DOOR_STATUS_SWINGING) <
	          game::DoorLevel(DOOR_STATUS_SMASHED),
	      "the wire's order is, which is the whole reason for the mapping");

	Check(game::DoorStatusForLevel(game::DOORDMG_GONE) == DOOR_STATUS_MISSING &&
	          game::DoorStatusForLevel(game::DOORDMG_SMASHED) == DOOR_STATUS_SMASHED,
	      "and it round-trips back to a byte the engine understands");
}

void TestDamageIsAMonotoneJoin() {
	std::printf("\nmerging two machines' idea of the same car\n");

	// Every ladder in CDamageManager climbs: ProgressPanelDamage refuses at 3,
	// ProgressDoorDamage refuses at 3, and nothing but CAutomobile::Fix ever
	// lowers either. So componentwise maximum is not a network heuristic, it
	// is what the engine would have produced had one machine simulated every
	// collision - and that is what lets roadmap.md §5.8's "anybody may report
	// it" carry over to a value that is not a boolean.
	uint32_t p = 0;
	uint16_t d = 0;
	SetPanelLevel(p, VEHPANEL_FRONT_LEFT, PANEL_STATUS_MISSING);
	SetDoorLevel(d, DOOR_BOOT, game::DOORDMG_GONE);

	uint32_t q = 0;
	uint16_t e = 0;
	SetPanelLevel(q, VEHPANEL_REAR_RIGHT, PANEL_STATUS_SMASHED1);
	SetDoorLevel(e, DOOR_FRONT_LEFT, game::DOORDMG_SMASHED);

	uint32_t lhsP = p; uint16_t lhsD = d;
	MergeDamage(lhsP, lhsD, q, e);
	uint32_t rhsP = q; uint16_t rhsD = e;
	MergeDamage(rhsP, rhsD, p, d);
	Check(lhsP == rhsP && lhsD == rhsD, "the merge is commutative");

	uint32_t againP = lhsP; uint16_t againD = lhsD;
	MergeDamage(againP, againD, q, e);
	Check(againP == lhsP && againD == lhsD,
	      "and idempotent, so a duplicate packet changes nothing");

	Check(GetPanelLevel(lhsP, VEHPANEL_FRONT_LEFT) == PANEL_STATUS_MISSING &&
	          GetPanelLevel(lhsP, VEHPANEL_REAR_RIGHT) == PANEL_STATUS_SMASHED1 &&
	          GetDoorLevel(lhsD, DOOR_BOOT) == game::DOORDMG_GONE &&
	          GetDoorLevel(lhsD, DOOR_FRONT_LEFT) == game::DOORDMG_SMASHED,
	      "two machines that each saw half of a shunt agree on the union");

	// And it never goes down, which matters because no applier in the engine
	// has an arm that restores an atomic: a lower value would be silently
	// ignored by the game and remembered as true by the session.
	uint32_t worseP = lhsP; uint16_t worseD = lhsD;
	MergeDamage(worseP, worseD, 0, 0);
	Check(worseP == lhsP && worseD == lhsD, "an empty report takes nothing off");

	uint32_t lowerP = 0; uint16_t lowerD = 0;
	SetPanelLevel(lowerP, VEHPANEL_FRONT_LEFT, PANEL_STATUS_SMASHED1);
	uint32_t keepP = lhsP; uint16_t keepD = lhsD;
	MergeDamage(keepP, keepD, lowerP, lowerD);
	Check(GetPanelLevel(keepP, VEHPANEL_FRONT_LEFT) == PANEL_STATUS_MISSING,
	      "and a stale, gentler report cannot un-break a panel");

	Check(!DamageGrew(lhsP, lhsD, q, e), "nothing new means no packet");
	Check(DamageGrew(lhsP, lhsD, 0, static_cast<uint16_t>(3u << (DOOR_REAR_RIGHT * 2))),
	      "and something new means one");
}

void TestTheWireHasNoRoomForABadIndex() {
	std::printf("\nthe fourth unchecked subscript\n");

	// CDamageManager::SetDoorStatus is `mov byte [ecx+edx+9],al` and
	// SetWheelStatus is `mov byte [ecx+edx+5],al`, neither with a compare in
	// front of it: a door index of 24 writes a byte into m_panelStatus, an
	// index of 0x100 writes one into a CDoor. SetPanelStatus and
	// SetLightStatus are gentler only by accident - they compute a shift and
	// x86 `shl` masks the count to five bits, so a panel of 8 quietly
	// rewrites panel 0 rather than corrupting anything.
	//
	// Same class as the four-entry anim group, the twelve-slot node array and
	// CreateInstance's six-entry m_comps. Fourth time, so it is a test.
	// Six two-bit fields, each clamped to the top level the wire has a
	// meaning for - so all-ones becomes 0b10 repeated, not 0b11 repeated.
	// A level of 3 would be handed to DoorStatusForLevel, which has no case
	// for it, and the door would be written OK: a bad index turned into a
	// quietly wrong car rather than a refused packet.
	Check(CleanDoorWord(0xFFFF) == 0x0AAA,
	      "the door word carries six doors, each clamped to 'gone'");
	Check(CleanPanelWord(0xFFFFFFFFu) == 0x3333333u,
	      "and seven panels, each clamped to ePanelStatus's own top value");
	Check((CleanDoorWord(0xFFFF) & ~VEH_DAMAGE_DOOR_MASK) == 0 &&
	          (CleanPanelWord(0xFFFFFFFFu) & ~VEH_DAMAGE_PANEL_MASK) == 0,
	      "and nothing above either of them survives at all");

	for (unsigned i = 0; i < VEH_DAMAGE_DOORS; ++i)
		Check(GetDoorLevel(CleanDoorWord(0xFFFF), i) <= VEH_DOOR_LEVEL_MAX,
		      "no door survives cleaning above the level the engine has a case for");
	for (unsigned i = 0; i < VEH_DAMAGE_PANELS; ++i)
		Check(GetPanelLevel(CleanPanelWord(0xFFFFFFFFu), i) <= VEH_PANEL_LEVEL_MAX,
		      "and no panel does either");

	// Out-of-range accessors are total rather than undefined, so a loop that
	// runs one too far cannot become a write.
	uint16_t d = 0;
	SetDoorLevel(d, VEH_DAMAGE_DOORS, game::DOORDMG_GONE);
	Check(d == 0, "writing door six writes nothing");
	uint32_t p = 0;
	SetPanelLevel(p, VEH_DAMAGE_PANELS, PANEL_STATUS_MISSING);
	Check(p == 0, "writing panel seven writes nothing");
	Check(GetDoorLevel(0xFFFF, 99) == 0 && GetPanelLevel(0xFFFFFFFFu, 99) == 0,
	      "and reading past the end reads nothing");
}

void TestTheComponentPairing() {
	std::printf("\nwhich RwFrame each panel and door lives on\n");

	// SetPanelDamage, SetBumperDamage and SetDoorDamage each open with
	// `if(m_aCarNodes[component] == nil) return`, so a wrong pairing is a
	// silent no-op rather than a crash - the damage travels, arrives, is
	// written into CDamageManager, and nothing changes on screen. These two
	// tables are transcribed from the engine's own `push` pairs, and this is
	// what pins them.
	Check(game::CarNodeForDoor(DOOR_BONNET) == CAR_BONNET &&
	          game::CarNodeForDoor(DOOR_BOOT) == CAR_BOOT &&
	          game::CarNodeForDoor(DOOR_FRONT_LEFT) == CAR_DOOR_LF &&
	          game::CarNodeForDoor(DOOR_FRONT_RIGHT) == CAR_DOOR_RF &&
	          game::CarNodeForDoor(DOOR_REAR_LEFT) == CAR_DOOR_LR &&
	          game::CarNodeForDoor(DOOR_REAR_RIGHT) == CAR_DOOR_RR,
	      "the six doors pair the way BlowUpCar pushes them");

	// The pairing is not the identity and it is not sorted, which is exactly
	// why it has to be written down: door 2 is node 15 and door 3 is node 11.
	Check(game::CarNodeForDoor(DOOR_FRONT_LEFT) == 15 &&
	          game::CarNodeForDoor(DOOR_FRONT_RIGHT) == 11,
	      "left is 15 and right is 11, not the other way round");

	Check(game::CarNodeForPanel(VEHPANEL_FRONT_LEFT) == CAR_WING_LF &&
	          game::CarNodeForPanel(VEHPANEL_FRONT_RIGHT) == CAR_WING_RF &&
	          game::CarNodeForPanel(VEHPANEL_REAR_LEFT) == CAR_WING_LR &&
	          game::CarNodeForPanel(VEHPANEL_REAR_RIGHT) == CAR_WING_RR &&
	          game::CarNodeForPanel(VEHBUMPER_FRONT) == CAR_BUMP_FRONT &&
	          game::CarNodeForPanel(VEHBUMPER_REAR) == CAR_BUMP_REAR,
	      "and the panels the way SetupDamageAfterLoad pushes them");

	Check(game::PanelIsBumper(VEHBUMPER_FRONT) && game::PanelIsBumper(VEHBUMPER_REAR) &&
	          !game::PanelIsBumper(VEHPANEL_WINDSCREEN),
	      "the two bumpers go through SetBumperDamage and the rest do not");

	// A light is a panel that has been hit - SetLightStatus has one caller,
	// always passed the literal 1, two instructions from the
	// ProgressPanelDamage call for the same panel. So it is derived here
	// rather than carried.
	uint32_t panels = 0;
	SetPanelLevel(panels, VEHPANEL_FRONT_RIGHT, PANEL_STATUS_SMASHED1);
	const uint32_t lights = game::LightWordFromPanels(panels);
	Check(((lights >> (VEHPANEL_FRONT_RIGHT * 2)) & 3) == 1,
	      "a damaged panel breaks its light");
	Check(((lights >> (VEHPANEL_FRONT_LEFT * 2)) & 3) == 0,
	      "and only its own");
	Check(game::LightWordFromPanels(game::WRECK_PANEL_WORD) == 0,
	      "a wreck's zeroed panel word leaves no light broken, which is what "
	      "FuckCarCompletely does on every machine");
}

void TestADentReachesTheCar() {
	std::printf("\nsomebody else's car gets worse\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(9), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "the car is here");
	Check(g_rec.vehicleDamageApplies == 0, "and undamaged, so nothing is applied");

	uint32_t panels = 0;
	SetPanelLevel(panels, VEHPANEL_FRONT_LEFT, PANEL_STATUS_MISSING);
	uint16_t doors = 0;
	SetDoorLevel(doors, DOOR_BOOT, game::DOORDMG_GONE);
	c.HandleMessage(Wrap(MakeVehicleDamage(9, panels, doors), CH_EVENT));

	Check(g_rec.vehicleDamageApplies == 1, "the dent reaches the engine seam");
	Check(g_rec.lastVehicleDamage.panels == panels &&
	          g_rec.lastVehicleDamage.doors == doors,
	      "with what the session now believes");
	Check(g_rec.lastDamageFlying,
	      "and the parts fly off, because this happened in front of you");

	// Absolute state, so the same packet again says nothing new. Without this
	// a client that resent its word would make every observer re-run
	// CAutomobile::SetDoorDamage - RenderWare work - for a door that has not
	// moved.
	c.HandleMessage(Wrap(MakeVehicleDamage(9, panels, doors), CH_EVENT));
	Check(g_rec.vehicleDamageApplies == 1, "a duplicate is not applied again");
}

// The cross-branch collision: the damage work declared the spray shop out of
// scope because garages were unsynced, the garage work then built the spray
// shop, and a resprayed car stayed dented everywhere but on its owner's screen.
void TestARespraysClearLetsTheRowGoOfTheDents() {
	std::printf("\na repair is the one report that lowers a car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(9), CH_EVENT));
	c.Tick();

	uint32_t panels = 0;
	SetPanelLevel(panels, VEHPANEL_FRONT_LEFT, PANEL_STATUS_MISSING);
	uint16_t doors = 0;
	SetDoorLevel(doors, DOOR_BOOT, game::DOORDMG_GONE);
	c.HandleMessage(Wrap(MakeVehicleDamage(9, panels, doors), CH_EVENT));
	Check(g_rec.vehicleDamageApplies == 1, "the car is dented");

	const int appliesBefore = g_rec.vehicleDamageApplies;
	c.HandleMessage(Wrap(MakeVehicleDamage(9, VEH_DAMAGE_RESET, 0), CH_EVENT));

	Check(c.VehicleByNetId(9)->damagePanels == 0 &&
	          c.VehicleByNetId(9)->damageDoors == 0,
	      "and the repair empties the row");
	// ApplyRemoteVehicleDamage never lowers anything - it writes a status and
	// calls the engine's applier per component, and there is no per-component
	// undo. The only thing that cleans the car is the CAutomobile::Fix the
	// respray packet just ahead of this one already ran.
	Check(g_rec.vehicleDamageApplies == appliesBefore,
	      "without asking the engine to un-dent anything");

	c.Tick();
	Check(g_rec.vehicleDamageApplies == appliesBefore,
	      "and nothing puts the dents back on the next frame");

	// The half that would otherwise stay broken for the rest of the session:
	// a later scrape gentler than the one that was sprayed off.
	uint32_t light = 0;
	SetPanelLevel(light, VEHPANEL_FRONT_LEFT, PANEL_STATUS_SMASHED1);
	c.HandleMessage(Wrap(MakeVehicleDamage(9, light, 0), CH_EVENT));
	Check(g_rec.vehicleDamageApplies == appliesBefore + 1,
	      "a scrape lighter than the one sprayed off is news again");
}

void TestADentBeforeTheCarIsNotLost() {
	std::printf("\na dent that overtakes its own car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = false;   // the model is still streaming

	c.HandleMessage(Wrap(MakeVehicleSpawn(9), CH_EVENT));
	uint32_t panels = 0;
	SetPanelLevel(panels, VEHPANEL_REAR_LEFT, PANEL_STATUS_SMASHED1);
	c.HandleMessage(Wrap(MakeVehicleDamage(9, panels, 0), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 0, "the car is not here yet");
	Check(g_rec.vehicleDamageApplies == 0, "so nothing was written into it");
	Check(c.VehicleByNetId(9) != nullptr && c.VehicleByNetId(9)->damagePanels == panels,
	      "but the row is holding it - reconciliation, not an event handler");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "the model arrives and the car is built");
	Check(g_rec.vehicleDamageApplies == 1, "and it is built into the shape it is in");
	Check(!g_rec.lastDamageFlying,
	      "with the flying components OFF, so a joiner is not handed a shower "
	      "of doors out of the object pool");
	c.Tick();
	Check(g_rec.vehicleDamageApplies == 1, "once, not once a frame");
}

void TestADentIsNeverAppliedToACarWeDrive() {
	std::printf("\nour own car is ours to dent\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsTheSessionsCar(c);

	// Through the takeover, so the session agrees the car is ours: the claim,
	// then the reply under the netId it already had.
	c.TickLocalVehicle();
	S_EnterVehicle enter;
	InitHeader(enter, 1000);
	enter.playerId   = 0;
	enter.body       = EnterVehicleBody{};
	enter.body.netId = 80;
	enter.body.seat  = 0;
	c.HandleMessage(Wrap(enter, CH_EVENT));
	Check(c.LocalVehicleNetId() == 80, "we are the recorded driver");

	uint16_t doors = 0;
	SetDoorLevel(doors, DOOR_FRONT_LEFT, game::DOORDMG_GONE);
	c.HandleMessage(Wrap(MakeVehicleDamage(80, 0, doors), CH_EVENT));

	Check(g_rec.vehicleDamageApplies == 0,
	      "nothing is written into a car we are sitting in the driver's seat of");
	Check(c.VehicleByNetId(80) != nullptr && c.VehicleByNetId(80)->damageDoors == doors,
	      "though the session's record of it is still kept, and is still right");
}

void TestAWreckTakesNoMoreDents() {
	std::printf("\na wreck is finished\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(9), CH_EVENT));
	c.Tick();

	S_VehicleBlowUp blast;
	InitHeader(blast, 1000);
	blast.playerId    = 1;
	blast.body.netId  = 9;
	blast.body.pos    = {1.0f, 2.0f, 3.0f};
	blast.body.rot    = {0.0f, 0.0f, 0.0f, 1.0f};
	c.HandleMessage(Wrap(blast, CH_EVENT));
	Check(c.VehicleByNetId(9) != nullptr && c.VehicleByNetId(9)->destroyed,
	      "the car is a wreck");

	// CDamageManager::FuckCarCompletely gave it six missing doors and a zeroed
	// panel word with no input of any kind, on every machine, inside the
	// BlowUpCar every machine just ran. A pre-blast word arriving late would
	// be a machine remembering a car less broken than the one on screen.
	uint32_t panels = 0;
	SetPanelLevel(panels, VEHPANEL_WINDSCREEN, PANEL_STATUS_SMASHED1);
	c.HandleMessage(Wrap(MakeVehicleDamage(9, panels, 0), CH_EVENT));
	Check(g_rec.vehicleDamageApplies == 0, "and it takes no more damage off the wire");
}

void TestADentForACarWeHaveNeverHeardOf() {
	std::printf("\na dent is not a reason to invent a car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;

	c.HandleMessage(Wrap(MakeVehicleDamage(77, 0xF, 0), CH_EVENT));
	c.Tick();
	Check(c.VehicleCount() == 0, "no row is created; the packet carries no model");
	Check(g_rec.vehicleSpawns == 0, "and nothing is built");
}

void TestAGrantUnblocksAndRemovesNothing() {
	std::printf("a grant unblocks our copy and removes nobody's\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	c.HandleMessage(Wrap(MakeGrant(10.0f), CH_EVENT));
	Check(g_rec.grantsConsumed == 1, "the seam was told to unblock it");
	Check(g_rec.remoteTakes == 0, "and was not told to remove it");
	Check(g_rec.lastGranted.pos.x == 10.0f, "with the ident we claimed");
}

void TestOurOwnCollectionIsNeverReplayedOnUs() {
	std::printf("our own collection is never replayed on us\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// The server leaves the collector out of this broadcast, so it should
	// never arrive - but if it does, our engine has already removed the
	// pickup and replaying the removal would destroy an object twice.
	c.HandleMessage(Wrap(MakeTaken(/*playerId=*/0, 10.0f), CH_EVENT));
	Check(g_rec.remoteTakes == 0, "nothing was removed a second time");
	Check(g_rec.grantsConsumed == 0, "and it was not mistaken for a grant");
}

void TestSomebodyElsesPickupIsRemoved() {
	std::printf("somebody else's pickup is removed from our world\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	c.HandleMessage(Wrap(MakeTaken(/*playerId=*/3, 20.0f), CH_EVENT));
	Check(g_rec.remoteTakes == 1, "the seam was told to remove it");
	Check(g_rec.grantsConsumed == 0, "and was not handed a grant");
	Check(g_rec.lastRemoteTake.pos.x == 20.0f, "with the right ident");
}

void TestAGrantWeCannotUseIsHandedBack() {
	std::printf("a grant with nothing there to take is released\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.grantFails = 1;

	// The send goes nowhere without a socket, which is fine: what matters is
	// that the client does not treat a refused grant as a consumed one and
	// leave the session holding a pickup nobody has.
	c.HandleMessage(Wrap(MakeGrant(30.0f), CH_EVENT));
	Check(g_rec.grantsConsumed == 0, "nothing was consumed");
	Check(g_rec.remoteTakes == 0, "and nothing was removed either");
}

void TestADenialOnlyEndsTheClaim() {
	std::printf("a denial only ends the claim\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	S_PickupDenied d;
	InitHeader(d, 1000);
	d.ident.pos        = {40.0f, 5.0f, 1.0f};
	d.ident.modelIndex = 100;
	d.ident.type       = 2;
	d.ident.flags      = 0;
	c.HandleMessage(Wrap(d, CH_EVENT));

	Check(g_rec.denials == 1, "the seam was told");
	Check(g_rec.remoteTakes == 0,
	      "and nothing was removed - the engine never saw an object there, "
	      "which is the whole reason the arbitration comes first");
}

// ---- ped drops -------------------------------------------------------------
//
// docs/pickups.md 10. Two things are worth pinning without a game, and
// neither of them is "does the engine make a pickup".
//
// The first is the decision itself, which is arithmetic on three facts and is
// the whole of the suppression. Writing the truth table out is the check: the
// interesting rows are the ones where the answer is "do nothing" for two
// completely different reasons, and a REFUSE that quietly became a LOCAL
// would put a remote player's gun back on our pavement without any symptom
// this side of a second machine.
void TestWhoMayMakeADrop() {
	std::printf("who is allowed to make a ped's drop\n");
	using coopiii::game::DecideDrop;
	using coopiii::game::DropDecision;

	constexpr uint8_t RANDOM  = 1;   // CHAR_CREATED_BY_RANDOM
	constexpr uint8_t MISSION = 2;   // CHAR_CREATED_BY_MISSION
	constexpr uint8_t UNKNOWN = 0;

	// A ped CoopIII built for somebody else. Refused whatever it is and
	// whatever state the session is in - including while the connection is
	// going down, which is why the replica test comes first and not last.
	Check(DecideDrop(true, true, MISSION) == DropDecision::REFUSE,
	      "a remote player's ped drops nothing here");
	Check(DecideDrop(true, false, MISSION) == DropDecision::REFUSE,
	      "and still nothing with no session left");
	Check(DecideDrop(true, true, RANDOM) == DropDecision::REFUSE,
	      "the replica test wins over the created-by byte");

	// A pedestrian this engine generated. The only thing that is announced.
	Check(DecideDrop(false, true, RANDOM) == DropDecision::ANNOUNCE,
	      "our own pedestrian's drop is shared");
	Check(DecideDrop(false, false, RANDOM) == DropDecision::LOCAL,
	      "with nobody to tell, it is just the local engine's business");

	// A script ped. Every machine has its own copy and its own copy will drop
	// its own, so announcing one machine's would make a second pickup on
	// every other screen. Deliberately unchanged from single player.
	Check(DecideDrop(false, true, MISSION) == DropDecision::LOCAL,
	      "a mission ped's drop stays local - M5 owns that, not this");
	Check(DecideDrop(false, true, UNKNOWN) == DropDecision::LOCAL,
	      "and anything the engine has not labelled stays local too");
}

// The second is the routing, which is the same single comparison that
// S_PickupTaken needs and is wrong in the same invisible way: a machine that
// rebuilt its own drop would end up with two pickups of one model inside the
// 0.25 m ident tolerance, and FindSlot refuses to tell those apart for as
// long as they both live.
S_PickupDrop MakeDrop(uint8_t playerId, float x, uint16_t quantity) {
	S_PickupDrop d;
	InitHeader(d, 1000);
	d.playerId              = playerId;
	d.body.ident.pos        = {x, 5.0f, 1.0f};
	d.body.ident.modelIndex = 138;
	d.body.ident.type       = 7;   // PICKUP_MONEY
	d.body.ident.flags      = 0;
	d.body.quantity         = quantity;
	d.body.pad              = 0;
	return d;
}

void TestSomebodyElsesDropIsBuiltHere() {
	std::printf("somebody else's ped drop is built in our world\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	c.HandleMessage(Wrap(MakeDrop(/*playerId=*/3, 60.0f, 47), CH_EVENT));
	Check(g_rec.drops == 1, "the seam was told to make it");
	Check(g_rec.lastDrop.ident.pos.x == 60.0f, "at the owner's coordinate");
	Check(g_rec.lastDrop.quantity == 47,
	      "with the amount their engine rolled, which nobody else could have "
	      "worked out");
	Check(g_rec.remoteTakes == 0, "and nothing was removed");
}

void TestOurOwnDropIsNeverBuiltTwice() {
	std::printf("our own ped drop is never built a second time\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// The server leaves the owner out of the relay, so this should never
	// arrive. If it does, our engine already made the pickup and making it
	// again would leave two of one model within the ident tolerance -
	// permanently unclaimable on this machine, for both of them.
	c.HandleMessage(Wrap(MakeDrop(/*playerId=*/0, 60.0f, 47), CH_EVENT));
	Check(g_rec.drops == 0, "nothing was made a second time");
}

void TestDisconnectGivesThePickupsBackToTheEngine() {
	std::printf("with no session, pickups are the local engine's again\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	const int atWelcome = g_rec.pickupResets;
	c.Stop();
	Check(g_rec.pickupResets > atWelcome, "the gate was dropped");
}

// ---- the ambient ped stream and the traffic driver -------------------------
//
// docs/population.md §3 step 6. Step 2 deliberately shipped a replica that is
// created where the session says and left there, and what that looks like in
// a running game is a city of statues. These pin the decisions that stop it:
// that a streamed pose reaches the replica at all, that a ped nobody is
// streaming is held rather than dropped, and that the driver link is a
// standing instruction rather than an event - which is what makes every race
// around it fall out of one loop.

S_PedSpawn MakeAmbientPedSpawn(uint16_t netId, uint8_t owner = 1,
                               float x = 10.0f) {
	S_PedSpawn s;
	InitHeader(s, 1000);
	s.ownerPlayerId = owner;
	s.tempId        = 0;   // a backfill, i.e. "not one of ours"
	s.netId         = netId;
	s.body.modelId  = 7;
	s.body.pedType  = 0;
	s.body.pad      = 0;
	s.body.pos      = {x, 20.0f, 30.0f};
	s.body.heading  = 0.0f;
	return s;
}

S_CarSpawn MakeAmbientCarSpawn(uint16_t netId, uint8_t owner = 1) {
	S_CarSpawn s;
	InitHeader(s, 1000);
	s.ownerPlayerId = owner;
	s.tempId        = 0;
	s.netId         = netId;
	s.body.modelId  = 90;
	s.body.colour1  = 1;
	s.body.colour2  = 2;
	s.body.extra1   = -1;
	s.body.extra2   = -1;
	s.body.pad[0] = s.body.pad[1] = 0;
	s.body.pos      = {40.0f, 50.0f, 60.0f};
	s.body.rot      = {0.0f, 0.0f, 0.0f, 1.0f};
	return s;
}

// One row, which is all any of these need. `vehicleNetId` of INVALID_NETID is
// a pedestrian on foot.
S_PedStates MakePedStates(uint8_t owner, uint16_t netId, uint32_t timeMs,
                          float x, uint16_t animId = 0,
                          uint16_t vehicleNetId = INVALID_NETID,
                          uint8_t seat = 0, uint8_t flags = 0) {
	S_PedStates s{};
	InitHeader(s, timeMs);
	s.ownerPlayerId      = owner;
	s.count              = 1;
	s.peds[0].netId        = netId;
	s.peds[0].animId       = animId;
	s.peds[0].vehicleNetId = vehicleNetId;
	s.peds[0].seat         = seat;
	s.peds[0].flags        = flags;
	s.peds[0].pos          = {x, 20.0f, 30.0f};
	s.peds[0].heading      = 0.0f;
	return s;
}

// A replicated ped that exists, with the model already in.
void GiveUsAnAmbientPed(Client &c, uint16_t netId = 500, float x = 10.0f) {
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientPedSpawn(netId, 1, x), CH_EVENT));
	c.Tick();
}

void TestAmbientPedIsToldWhereItIs() {
	std::printf("\na replicated pedestrian stops being a statue\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c);
	Check(g_rec.ambientPedSpawns == 1, "the replica exists");

	c.HandleMessage(Wrap(MakePedStates(1, 500, 2000, 77.0f, /*animId=*/1),
	                     CH_SNAPSHOT));
	const RemoteAmbientPed *p = c.AmbientPed(500);
	Check(p != nullptr && p->last.pos.x == 77.0f,
	      "the streamed position reached the roster");
	Check(p != nullptr && p->animId == 1,
	      "and so did the animation, which is the only thing that has ever "
	      "made a non-player ped walk");

	const int before = g_rec.ambientApplies;
	c.Tick();
	Check(g_rec.ambientApplies == before + 1, "and it is driven into the engine");
}

// ---- somebody else's pedestrians fighting (protocol.h, C_NpcShot) ----------

void TestTheWeaponRidesTheFlagsByte() {
	std::printf("\nthe weapon in a pedestrian's hand, in his row's flags byte\n");
	bool all = true;
	for (uint8_t w = 0; w < INVENTORY_SLOTS; ++w)
		if (AmbientPedWeapon(AmbientPedFlagsWithWeapon(0, w)) != w)
			all = false;
	Check(all, "all thirteen inventory weapons go through and come back");
	const uint8_t both = AmbientPedFlagsWithWeapon(AMBIENT_PED_ON_FIRE, WEAPONTYPE_M16);
	Check((both & AMBIENT_PED_ON_FIRE) && AmbientPedWeapon(both) == WEAPONTYPE_M16,
	      "beside the fire bit, which it leaves alone");
	Check((AmbientPedFlagsWithWeapon(0, 12) & ~AMBIENT_PED_WEAPON_MASK) == 0,
	      "and nothing outside its own four bits");
	Check(AmbientPedWeapon(0) == WEAPONTYPE_UNARMED,
	      "an older sender's zero is unarmed, which is what every replica held");
	Check(AmbientPedWeapon(AmbientPedFlagsWithWeapon(0, INVENTORY_SLOTS)) ==
	          WEAPONTYPE_UNARMED,
	      "a weapon that is not in the inventory goes out as unarmed");
	Check(AmbientPedWeapon(0xFF) == WEAPONTYPE_UNARMED,
	      "and bits no weapon can have read as unarmed");
}

void TestAReplicaIsArmedAsItsHostHasHim() {
	std::printf("\na replica holds the gun his host is holding\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c);

	c.HandleMessage(Wrap(MakePedStates(1, 500, 2000, 77.0f, 1, INVALID_NETID, 0,
	                                   AmbientPedFlagsWithWeapon(0, WEAPONTYPE_COLT45)),
	                     CH_SNAPSHOT));
	const RemoteAmbientPed *p = c.AmbientPed(500);
	Check(p != nullptr && p->weapon == WEAPONTYPE_COLT45, "the row's weapon is kept");

	const int armed = g_rec.replicaArms;
	c.Tick();
	Check(g_rec.replicaArms == armed + 1 && g_rec.lastReplicaArm == WEAPONTYPE_COLT45 &&
	          p->appliedWeapon == WEAPONTYPE_COLT45,
	      "and put in the replica's hand");
	c.Tick();
	Check(g_rec.replicaArms == armed + 1, "once, not every frame");

	g_rec.replicaArmReady = false;
	c.HandleMessage(Wrap(MakePedStates(1, 500, 2100, 77.0f, 1, INVALID_NETID, 0,
	                                   AmbientPedFlagsWithWeapon(0, WEAPONTYPE_UZI)),
	                     CH_SNAPSHOT));
	c.Tick();
	c.Tick();
	Check(g_rec.replicaArms == armed + 3 && p->appliedWeapon == WEAPONTYPE_COLT45,
	      "a gun whose model is still streaming is asked for again, frame after frame");
	g_rec.replicaArmReady = true;
	c.Tick();
	Check(p->appliedWeapon == WEAPONTYPE_UZI, "and held once it is in");
}

S_NpcShot MakeNpcShot(uint8_t owner, uint16_t pedNetId, uint8_t weapon = WEAPONTYPE_UZI) {
	S_NpcShot s;
	InitHeader(s, 2000);
	s.ownerPlayerId = owner;
	s.pedNetId      = pedNetId;
	s.body.weapon   = weapon;
	s.body.origin   = {1.0f, 2.0f, 3.0f};
	s.body.dir      = {0.0f, 1.0f, 0.0f};
	s.body.speed    = 0.0f;
	return s;
}

void TestAnNpcRoundIsHandedOverToBeDrawn() {
	std::printf("\na round somebody else's pedestrian fired\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c);

	c.HandleMessage(Wrap(MakeNpcShot(1, 500), CH_SNAPSHOT));
	Check(g_rec.npcShots == 1 && g_rec.lastNpcShotPed == 500 &&
	          g_rec.lastNpcShot.weapon == WEAPONTYPE_UZI,
	      "is handed to the engine to draw on our replica of him");

	c.HandleMessage(Wrap(MakeNpcShot(2, 500), CH_SNAPSHOT));
	Check(g_rec.npcShots == 1, "not when it claims to come from a machine that does not host him");
	c.HandleMessage(Wrap(MakeNpcShot(1, 501), CH_SNAPSHOT));
	Check(g_rec.npcShots == 1, "nor for a pedestrian we have never been told about");
	c.HandleMessage(Wrap(MakeNpcShot(0, 500), CH_SNAPSHOT));
	Check(g_rec.npcShots == 1, "nor as our own, which is the echo");
}

S_NpcDamage MakeNpcDamage(uint8_t owner, uint16_t attacker, uint16_t victimNetId) {
	S_NpcDamage d;
	InitHeader(d, 2000);
	d.ownerPlayerId      = owner;
	d.attackerPedNetId   = attacker;
	d.body.victimNetId   = victimNetId;
	d.body.weapon        = WEAPONTYPE_COLT45;
	d.body.amount        = 25.0f;
	d.body.piece         = 3;
	d.body.direction     = 1;
	return d;
}

void TestAnNpcHitLandsOnUsAndNobodyElse() {
	std::printf("\na hit somebody else's pedestrian landed on our copy\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c);   // welcomes us as player 0, net 100

	c.HandleMessage(Wrap(MakeNpcDamage(1, 500, 100), CH_EVENT));
	Check(g_rec.npcDamages == 1 && g_rec.npcDamageAttacker == 500,
	      "is applied to us, blamed on our replica of him");
	Check(g_rec.lastNpcDamage.weapon == WEAPONTYPE_COLT45 &&
	          g_rec.lastNpcDamage.amount == 25.0f && g_rec.lastNpcDamage.piece == 3 &&
	          g_rec.lastNpcDamage.direction == 1,
	      "with the engine's own arguments, untouched");

	c.HandleMessage(Wrap(MakeNpcDamage(1, 500, 101), CH_EVENT));
	Check(g_rec.npcDamages == 1, "a hit meant for somebody else is never applied to us");

	c.HandleMessage(Wrap(MakeNpcDamage(2, 500, 100), CH_EVENT));
	Check(g_rec.npcDamages == 2 && g_rec.npcDamageAttacker == 0,
	      "a hit whose host does not match our row still lands, with nobody to blame");
	c.HandleMessage(Wrap(MakeNpcDamage(1, 999, 100), CH_EVENT));
	Check(g_rec.npcDamages == 3 && g_rec.npcDamageAttacker == 0,
	      "and so does one from a pedestrian we have no replica of");
}

void TestABurningPedestrianBurnsOnEveryScreen() {
	std::printf("a pedestrian his host has set alight burns here too\n");

	// The bit lives where the padding was, so nothing moved.
	Check(offsetof(AmbientPedState, flags) == 7 && sizeof(AmbientPedState) == 24,
	      "the flags byte is the old pad byte, and the row is still 24 bytes");

	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c);

	c.HandleMessage(Wrap(MakePedStates(1, 500, 2000, 77.0f, 1, INVALID_NETID, 0,
	                                   AMBIENT_PED_ON_FIRE),
	                     CH_SNAPSHOT));
	const RemoteAmbientPed *p = c.AmbientPed(500);
	Check(p != nullptr && p->fireSaid, "the host's word that he is burning is kept");
	Check(p != nullptr && WallClock::NowMs() - p->fireSaidMs < 1000,
	      "with when we heard it, on our own clock");

	const int before = g_rec.ambientFires;
	c.Tick();
	Check(g_rec.ambientFires == before + 1 && g_rec.lastAmbientFireSaid,
	      "and the replica is asked to burn, every frame");

	c.HandleMessage(Wrap(MakePedStates(1, 500, 2100, 77.0f, 1), CH_SNAPSHOT));
	c.Tick();
	Check(p != nullptr && !p->fireSaid && !g_rec.lastAmbientFireSaid,
	      "and to stop the moment the host says he has gone out");

	// Only his host may say it, the same as for his position.
	c.HandleMessage(Wrap(MakePedStates(2, 500, 2200, 77.0f, 1, INVALID_NETID, 0,
	                                   AMBIENT_PED_ON_FIRE),
	                     CH_SNAPSHOT));
	Check(p != nullptr && !p->fireSaid, "a row from anybody else changes nothing");

	// And the hold: a pedestrian who drops out of his host's twelve stops
	// getting rows, and his fire stops being vouched for.
	Check(AmbientPedShouldBurn(true, 5000, 5000) && AmbientPedShouldBurn(true, 5000, 5999),
	      "a fresh word keeps him burning");
	Check(!AmbientPedShouldBurn(true, 5000, 6000),
	      "a second-old one does not - the same cap the fire itself has");
	Check(!AmbientPedShouldBurn(false, 5000, 5000), "nor does being told he's fine");
	Check(AmbientPedShouldBurn(true, 0xFFFFFF00u, 0x00000100u),
	      "and a clock wrap in between is a short gap, not a long one");
}

// ---- limbs (protocol version 17) -------------------------------------------
//
// Whether the engine really takes the head off needs a running game. What is
// pinned here is the routing: a limb reaches the replica it names, and the
// two things that must never reach the engine - a ped we do not have and a
// node that is not one of the five - do not.

S_PedBodyPart MakeLimb(uint16_t netId, uint8_t node, int8_t direction = 1) {
	S_PedBodyPart s;
	InitHeader(s, 1000);
	s.body.netId     = netId;
	s.body.node      = node;
	s.body.direction = direction;
	return s;
}

void TestALimbComesOffTheReplica() {
	std::printf("\na limb the host's engine took off comes off our replica\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 500);

	c.HandleMessage(Wrap(MakeLimb(500, /*node=*/2, /*direction=*/3), CH_EVENT));
	Check(g_rec.limbs == 1, "the seam was told");
	Check(g_rec.lastLimbPed == 500 && g_rec.lastLimbNode == 2,
	      "the head, of the ped the host named");
	Check(g_rec.lastLimbDirection == 3,
	      "flying the way the host's hit sent it");
}

void TestALimbForAPedWeDoNotHaveIsDropped() {
	std::printf("a limb for a ped we never built goes nowhere\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 500);

	c.HandleMessage(Wrap(MakeLimb(501, 2), CH_EVENT));
	Check(g_rec.limbs == 0, "nothing was asked of the engine");
}

void TestAPlayersOwnLimbComesOffTheirPed() {
	std::printf("a player's own limb comes off their ped here\n");
	Client c;
	c.SetBridge(RecordingBridge());
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeBackfillJoin(1, "alice"), CH_EVENT));
	c.Tick();
	Check(g_rec.spawns == 1, "(alice's ped is built)");

	c.HandleMessage(Wrap(MakeLimb(201, /*node=*/2), CH_EVENT));   // alice's netId
	Check(g_rec.playerLimbs == 1 && g_rec.lastPlayerLimbOf == 1 && g_rec.lastPlayerLimbNode == 2,
	      "her head, off her ped, because her machine said so");
	Check(g_rec.limbs == 0, "and no pedestrian was touched for it");

	c.HandleMessage(Wrap(MakeLimb(201, 0), CH_EVENT));
	c.HandleMessage(Wrap(MakeLimb(777, 2), CH_EVENT));
	Check(g_rec.playerLimbs == 1, "not the torso, and not for a netId nobody has");
}

void TestANodeThatIsNotALimbIsRefused() {
	std::printf("a node that is not one of the five never reaches the engine\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 500);

	// The node indexes CPed::m_pFrames. The torso is a real node and still
	// not a limb; twelve is past the end of the array.
	c.HandleMessage(Wrap(MakeLimb(500, 0), CH_EVENT));
	c.HandleMessage(Wrap(MakeLimb(500, 12), CH_EVENT));
	Check(g_rec.limbs == 0, "neither was passed on");
	Check(IsRemovableBodyPart(2) && IsRemovableBodyPart(3) &&
	          IsRemovableBodyPart(4) && IsRemovableBodyPart(7) &&
	          IsRemovableBodyPart(8),
	      "and the five InflictDamage passes all are");
}

// ---- an ambient pedestrian dying -------------------------------------------
//
// Whether CPed::SetDie really drops the body needs a running game. What is
// pinned here is everything around it: that a death survives arriving before
// its own ped, that it is carried out once and not once a frame, that it
// takes the corpse out of its car first and does not let the reconciliation
// loop put him back, and that a refusal is a "not yet" rather than a loss.

S_PedDeath MakePedDeath(uint16_t netId, uint16_t animId = 13) {
	S_PedDeath s;
	InitHeader(s, 1000);
	s.body.netId  = netId;
	s.body.animId = animId;
	return s;
}

void TestAPedDiesOnTheReplica() {
	std::printf("\na pedestrian the host killed drops on our screen too\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 520);

	c.HandleMessage(Wrap(MakePedDeath(520, /*animId=*/17), CH_EVENT));
	Check(g_rec.ambientKills == 0,
	      "recorded, not carried out on the packet");
	c.Tick();
	Check(g_rec.ambientKills == 1, "and carried out on the next frame");
	Check(g_rec.lastKilledPed == 520 && g_rec.lastAmbientKillAnim == 17,
	      "the ped the host named, in the animation his engine chose");

	// Once. A second SetDie is a second SetStoredState over a state that is
	// already the death's.
	c.Tick();
	c.Tick();
	Check(g_rec.ambientKills == 1, "and not again on every frame after");
}

void TestADeathThatOvertakesItsOwnSpawn() {
	std::printf("a death that beats its own ped into the session still lands\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// The model is not in yet, so the replica does not exist - which is the
	// ordinary case for a joiner's backfill, where the spawn and the death
	// arrive in the same burst.
	g_rec.modelReady = false;
	c.HandleMessage(Wrap(MakeAmbientPedSpawn(521), CH_EVENT));
	c.HandleMessage(Wrap(MakePedDeath(521), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientPedSpawns == 0 && g_rec.ambientKills == 0,
	      "nothing exists to kill yet, and the death is not thrown away");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.ambientPedSpawns == 1, "the replica is built");
	Check(g_rec.ambientKills == 1,
	      "and killed on the same frame, so he is never seen standing up");
}

void TestADeathForAPedWeDoNotHaveIsDropped() {
	std::printf("a death for a ped we never built goes nowhere\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 522);

	c.HandleMessage(Wrap(MakePedDeath(523), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientKillAttempts == 0, "nothing was asked of the engine");
}

void TestADeathTheEngineRefusedIsRetried() {
	std::printf("a kill the engine would not take is retried, not lost\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 524);

	g_rec.refuseAmbientKill = true;
	c.HandleMessage(Wrap(MakePedDeath(524), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.ambientKillAttempts == 2 && g_rec.ambientKills == 0,
	      "asked twice, refused twice");

	// `dead` is a standing fact about the pedestrian and outlives any
	// particular replica of him - a pool that was full a frame ago is the
	// real version of this.
	g_rec.refuseAmbientKill = false;
	c.Tick();
	Check(g_rec.ambientKills == 1, "and it takes as soon as it can");
}

void TestARebuiltReplicaDiesAgain() {
	std::printf("a replica rebuilt after it died does not come back standing\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 526);

	c.HandleMessage(Wrap(MakePedDeath(526), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientKills == 1, "he dies");

	// The engine took the replica away - a full pool, a reaping sweep,
	// anything. `dead` is a fact about the pedestrian and `deathApplied` is
	// only a note about the copy that is gone.
	RemoteAmbientPed *p = const_cast<RemoteAmbientPed *>(c.AmbientPed(526));
	Check(p != nullptr, "the row is still there");
	if (!p)
		return;
	p->poolHandle   = -1;
	p->spawnPending = true;

	c.Tick();
	Check(g_rec.ambientPedSpawns == 2, "a new replica is built");
	Check(g_rec.ambientKills == 2, "and it is killed on the frame it appears");
}

// ---- shooting somebody else's pedestrian -----------------------------------
//
// The direction the two blocks above do not have. Whether the pedestrian
// actually flinches needs a running game; what is pinned here is everything
// around it - that the hit reaches the seam, that the blame resolves and a hit
// with nobody to blame still lands, that it is carried out on the packet rather
// than recorded on a row, that a build without the seam is silent rather than
// broken, and the one constraint the engine puts on the whole feature.

S_PedDamage MakePedDamage(uint8_t attackerId, uint16_t netId, uint8_t weapon = 3,
                          float amount = 12.0f, uint8_t piece = 0,
                          uint8_t direction = 2) {
	S_PedDamage d;
	InitHeader(d, 1000);
	d.attackerId    = attackerId;
	d.body.netId    = netId;
	d.body.weapon   = weapon;
	d.body.amount   = amount;
	d.body.piece    = piece;
	d.body.direction = direction;
	return d;
}

void TestAHitOnOurPedestrianReachesTheEngine() {
	std::printf("\na hit somebody else landed on one of our pedestrians\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	// No spawn packet and no roster row: this is the one inbound ambient
	// packet about a pedestrian *this* machine hosts, so `m_peds` has nothing
	// in it and the netId is resolved behind the bridge.
	c.HandleMessage(Wrap(MakePedDamage(1, 700, /*weapon=*/5, /*amount=*/34.0f,
	                                   /*piece=*/3, /*direction=*/1),
	                     CH_EVENT));
	Check(g_rec.pedDamages == 1, "it goes straight to the seam");
	Check(g_rec.lastPedDamage.netId == 700,
	      "naming the pedestrian the shooter shot");
	Check(g_rec.lastPedDamage.weapon == 5 && g_rec.lastPedDamage.amount == 34.0f,
	      "with the cause and the raw amount the shooter's own InflictDamage had");
	Check(g_rec.lastPedDamage.piece == 3 && g_rec.lastPedDamage.direction == 1,
	      "and the piece and the side, which pick the limb and the knockdown");
	Check(g_rec.pedDamageAttacker == 1, "credited to the player who fired");

	// Carried out on the packet, not recorded and reconciled. A hit is not a
	// standing fact about a pedestrian the way a death is - it is a thing that
	// happened to whoever was standing there - so a frame changes nothing.
	const int applied = g_rec.pedDamages;
	c.Tick();
	c.Tick();
	Check(g_rec.pedDamages == applied, "and it is not replayed every frame");
}

void TestAHitWithNobodyToBlameStillLands() {
	std::printf("a hit from a player whose ped we do not have still lands\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// Slot 5 has never joined, so there is no attacker to credit. The
	// pedestrian is hurt anyway: the blame is for the engine's bookkeeping and
	// the shot happened either way, exactly as it does for a player's hit.
	c.HandleMessage(Wrap(MakePedDamage(5, 701), CH_EVENT));
	Check(g_rec.pedDamages == 1, "the hit lands");
	Check(g_rec.pedDamageAttacker == 0xFF, "with nobody to credit it to");

	// And a packet naming us as the attacker of a hit on our own pedestrian is
	// not a thing the server sends - the ownership test in Server::OnPedDamage
	// is inverted precisely to stop it. If one arrives anyway it is still our
	// ped, so the hit is still ours to apply; what it must not do is credit us
	// as a remote player, because we are not one.
	c.HandleMessage(Wrap(MakePedDamage(/*attackerId=*/0, 701), CH_EVENT));
	Check(g_rec.pedDamages == 2 && g_rec.pedDamageAttacker == 0xFF,
	      "and a hit addressed from our own slot credits nobody");
}

void TestNoPedDamageSeamIsSilentNotBroken() {
	std::printf("a build with no seam to apply a hit with says so\n");
	Client c;
	WorldBridge bare = RecordingBridge();
	bare.ApplyRemotePedDamage = nullptr;
	c.SetBridge(bare);
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	c.HandleMessage(Wrap(MakePedDamage(1, 702), CH_EVENT));
	Check(g_rec.pedDamages == 0, "nothing is applied and nothing crashes");
}

// ---- shooting somebody else's car (protocol.h, VehicleHitBody) -------------
//
// The pedestrian exchange above with the nouns changed, and one thing it does
// not have: a car this machine is driving is a car this machine already holds a
// roster row for, so the inbound half has a row to check and the ped's does
// not. Everything that can be decided without an engine is decided in these
// tests; what needs one - the detour's refusal, and CVehicle::InflictDamage
// itself - is game/vehicle.cpp's and is exercised by hand.

S_VehicleHit MakeVehicleHit(uint8_t attackerId, uint16_t netId,
                            uint8_t weapon = 3, float amount = 25.0f) {
	S_VehicleHit h;
	InitHeader(h, 1000);
	h.attackerId  = attackerId;
	h.body.netId  = netId;
	h.body.weapon = weapon;
	h.body.amount = amount;
	return h;
}

// Puts the local player at the wheel of car 77, the way the session does it:
// an S_EnterVehicle addressed to our own slot. That is what fills
// m_localVehicleNetId, and DrivenLocally is what every guard below asks.
void GiveUsCar77(Client &c) {
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleSpawn(77), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(0, 77), CH_EVENT));
}

void TestAHitOnOurCarReachesTheEngine() {
	std::printf("\na hit somebody else landed on the car we are driving\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsCar77(c);
	Check(c.LocalVehicleNetId() == 77, "we are driving car 77");

	c.HandleMessage(Wrap(MakeVehicleHit(1, 77, /*weapon=*/5, /*amount=*/34.0f),
	                     CH_EVENT));
	Check(g_rec.vehicleHits == 1, "it goes straight to the seam");
	Check(g_rec.lastVehicleHitRow == 77, "against our own roster row");
	Check(g_rec.lastVehicleHit.weapon == 5 && g_rec.lastVehicleHit.amount == 34.0f,
	      "with the cause and the raw amount the shooter's own InflictDamage had");
	Check(g_rec.vehicleHitAttacker == 1, "credited to the player who fired");

	// Carried out on the packet, not recorded and reconciled. A hit is not a
	// standing fact about a car the way its dents are - it is a thing that
	// happened to whoever was at the wheel - so a frame changes nothing, and
	// the health it produced leaves on the snapshot that already carries one.
	const int applied = g_rec.vehicleHits;
	c.Tick();
	c.Tick();
	Check(g_rec.vehicleHits == applied, "and it is not replayed every frame");
}

S_NpcVehicleHit MakeNpcVehicleHit(uint8_t owner, uint16_t ped, uint16_t netId) {
	S_NpcVehicleHit h;
	InitHeader(h, 1000);
	h.ownerPlayerId    = owner;
	h.attackerPedNetId = ped;
	h.body.netId       = netId;
	h.body.weapon      = WEAPONTYPE_COLT45;
	h.body.amount      = 25.0f;
	return h;
}

void TestAPedestriansRoundOnOurCarReachesTheEngine() {
	std::printf("\na round somebody else's pedestrian put into the car we drive\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsCar77(c);
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientPedSpawn(500, 1, 10.0f), CH_EVENT));

	c.HandleMessage(Wrap(MakeNpcVehicleHit(1, 500, 77), CH_EVENT));
	Check(g_rec.npcCarHits == 1 && g_rec.npcCarHitRow == 77 && g_rec.npcCarHitAttacker == 500,
	      "goes to the seam against our row, blamed on our replica of him");
	Check(g_rec.vehicleHits == 0, "and not as a player's hit");

	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.HandleMessage(Wrap(MakeNpcVehicleHit(1, 500, 80), CH_EVENT));
	Check(g_rec.npcCarHits == 1, "a car we only watch takes nothing");

	c.HandleMessage(Wrap(MakeNpcVehicleHit(2, 500, 77), CH_EVENT));
	Check(g_rec.npcCarHits == 2 && g_rec.npcCarHitAttacker == 0,
	      "a round whose host does not match our row still lands, blamed on nobody");
}

void TestAHitOnACarWeAreNotDrivingIsRefused() {
	std::printf("a hit addressed to us about a car that is not ours\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsCar77(c);

	// Alice's car, which this machine only watches. Ours is not the machine
	// entitled to take health off it, and the whole point of the change is
	// that it stopped doing so - applying one because a packet asked would put
	// the rule back exactly where it was, only politely.
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleHit(1, 80), CH_EVENT));
	Check(g_rec.vehicleHits == 0, "nothing is applied to a car we only watch");

	// And one about a car the session has never named.
	c.HandleMessage(Wrap(MakeVehicleHit(1, 999), CH_EVENT));
	Check(g_rec.vehicleHits == 0, "nor to a car nobody has ever heard of");

	// Once we get out of our own, hits about it stop landing too: nobody is
	// driving it, so nobody is entitled to decide its condition, and a burst
	// still in flight when we stepped out must not go on hurting it.
	c.HandleMessage(Wrap(MakeExit(0, 77), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleHit(1, 77), CH_EVENT));
	Check(g_rec.vehicleHits == 0, "nor to the car we have just got out of");
}

void TestAHitWithNobodyToBlameStillLandsOnOurCar() {
	std::printf("a hit from a player whose ped we do not have still lands\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsCar77(c);

	// Slot 5 never joined, so there is nobody to credit. The car is hurt
	// anyway: the blame buys the scorch marks and m_pSetOnFireEntity, and the
	// shot happened either way - exactly as it does for a player's hit and a
	// pedestrian's.
	c.HandleMessage(Wrap(MakeVehicleHit(5, 77), CH_EVENT));
	Check(g_rec.vehicleHits == 1, "the hit lands");
	Check(g_rec.vehicleHitAttacker == 0xFF, "with nobody to credit it to");

	// And one addressed from our own slot. The server's ownership test is
	// inverted precisely to stop it ever being sent; if one turns up anyway it
	// is still our car, so it is still ours to apply - what it must not do is
	// credit us as a remote player, because we are not one.
	c.HandleMessage(Wrap(MakeVehicleHit(/*attackerId=*/0, 77), CH_EVENT));
	Check(g_rec.vehicleHits == 2 && g_rec.vehicleHitAttacker == 0xFF,
	      "and a hit addressed from our own slot credits nobody");
}

void TestAWreckTakesNoMoreHits() {
	std::printf("a hit that arrives after our car has already blown up\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsCar77(c);

	S_VehicleBlowUp blast;
	InitHeader(blast, 2000);
	blast.playerId  = 0;
	blast.body      = VehicleBlowUpBody{};
	blast.body.netId = 77;
	blast.body.pos   = {1.0f, 2.0f, 3.0f};
	blast.body.rot   = {0.0f, 0.0f, 0.0f, 1.0f};
	c.HandleMessage(Wrap(blast, CH_EVENT));

	// The rest of a burst that was in the air when it went up. Refused here as
	// well as by the server and by the engine - CVehicle::InflictDamage leaves
	// at 0x00551A10 for a car at zero health - because three cheap refusals
	// beat one round trip per round.
	c.HandleMessage(Wrap(MakeVehicleHit(1, 77), CH_EVENT));
	Check(g_rec.vehicleHits == 0, "a wreck takes no more hits");
}

void TestNoVehicleHitSeamIsSilentNotBroken() {
	std::printf("a build with no seam to apply a car hit with says so\n");
	Client c;
	WorldBridge bare = RecordingBridge();
	bare.ApplyRemoteVehicleHit = nullptr;
	c.SetBridge(bare);
	GiveUsCar77(c);

	c.HandleMessage(Wrap(MakeVehicleHit(1, 77), CH_EVENT));
	Check(g_rec.vehicleHits == 0, "nothing is applied and nothing crashes");
}

void TestWhichHitsAreWorthSending() {
	std::printf("which of our own hits are worth a packet\n");

	// Pure, so it is checked directly rather than through a socket the test
	// harness does not have. Each "no" is a race that really happens in the
	// milliseconds between the trigger and the send.
	RemoteVehicle v;
	v.active         = true;
	v.netId          = 80;
	v.driverPlayerId = 1;

	Check(VehicleHitIsWorthSending(v, INVALID_NETID),
	      "a car somebody else is driving is worth telling the session about");

	RemoteVehicle gone = v;
	gone.active        = false;
	Check(!VehicleHitIsWorthSending(gone, INVALID_NETID),
	      "a row the roster has dropped is not");

	RemoteVehicle parked  = v;
	parked.driverPlayerId = INVALID_PLAYER;
	Check(VehicleHitIsWorthSending(parked, INVALID_NETID),
	      "and a session car nobody holds is too - the server makes the shooter "
	      "its custodian, or its health is the last snapshot for good");

	RemoteVehicle settling     = parked;
	settling.custodianPlayerId = 1;
	Check(VehicleHitIsWorthSending(settling, INVALID_NETID),
	      "and a car nobody is in that somebody is settling is theirs, and is");

	RemoteVehicle wreck = v;
	wreck.destroyed     = true;
	Check(!VehicleHitIsWorthSending(wreck, INVALID_NETID),
	      "nor a car the session has already written off");

	Check(!VehicleHitIsWorthSending(v, /*localVehicleNetId=*/80),
	      "nor one we have got into since firing at it");
	Check(VehicleHitIsWorthSending(v, /*localVehicleNetId=*/77),
	      "but driving some other car changes nothing");
}

void TestACarHitCarriesThreeFieldsBecauseTheEngineTakesThree() {
	std::printf("a car's hit is a ped's minus the two a car does not have\n");

	// CPed::InflictDamage closes `ret 14h` - five dword arguments - and
	// CVehicle::InflictDamage closes `ret 0Ch`, three. The wire carries four of
	// the ped's five (the culprit cannot travel) and two of the car's three,
	// plus the netId that names the target on each. A car has no pedPiece and
	// no hit direction because there is no limb to take off and no knockdown
	// animation to choose.
	Check(sizeof(VehicleHitBody) + 4 == sizeof(PedDamageBody),
	      "exactly the piece, the direction and the two melee bytes smaller");
	Check(sizeof(S_VehicleHit) == sizeof(C_VehicleHit) + 1,
	      "and the relay adds the attacker's slot and nothing else");

	// No health anywhere in it, and that is the field whose absence is
	// load-bearing: writing a health destroys nothing and arms the engine's
	// five-second fire timer under somebody else's car (§1.11.1). Checked by
	// size rather than by name, because a field added later would show up here
	// before anybody read the comment.
	Check(sizeof(VehicleHitBody) == 7,
	      "netId, weapon and amount - no health, no position, no shot vector");

	// And the two directions do not share an opcode. C_VehicleDamage is
	// absolute cosmetic state sent BY the driver and merged as a maximum; this
	// is a delta sent TO the driver and never merged.
	Check(C_VehicleHit::OPCODE != C_VehicleDamage::OPCODE &&
	          S_VehicleHit::OPCODE != S_VehicleDamage::OPCODE,
	      "they are four different packets, not one packet with a flag");
}

// ---- shooting somebody else's traffic (docs/protocol.md §1.23) -------------
//
// The detours themselves need an engine. What they decide doesn't, and it
// lives in game/vehicle.h as ClassifyCar / DecideCarDamage / MayBlowUpCar so
// every case can be walked here.

void TestWhoMayDamageACar() {
	std::printf("\nwho gets to take health off a car, and blow it up\n");
	using game::CarDamageVerdict;
	using game::CarOwner;
	using game::ClassifyCar;
	using game::DecideCarDamage;
	using game::MayBlowUpCar;

	// ClassifyCar(authorised, weDrive, remoteDriver, remoteCustodian,
	//             trafficReplica)
	Check(ClassifyCar(false, false, false, false, false) == CarOwner::Local,
	      "a car nobody else has a claim on is this engine's");
	Check(ClassifyCar(false, false, true, false, false) == CarOwner::RemoteDriver,
	      "a session car somebody else drives is theirs");
	Check(ClassifyCar(false, false, false, true, false) == CarOwner::RemoteCustodian,
	      "a session car somebody else is settling is theirs too");
	Check(ClassifyCar(false, false, true, true, false) == CarOwner::RemoteDriver,
	      "a driver outranks a custodian, as in Session::MayReportVehicle");
	Check(ClassifyCar(false, false, false, false, true) == CarOwner::RemoteHost,
	      "a traffic replica is its host's");
	Check(ClassifyCar(false, true, false, false, true) == CarOwner::Local,
	      "a replica we have taken the wheel of is ours before the promotion lands");
	Check(ClassifyCar(false, true, true, false, false) == CarOwner::Local,
	      "and so is a car we jacked before the session caught up");
	Check(ClassifyCar(false, true, false, true, false) == CarOwner::Local,
	      "and one we got back into while somebody was still settling it");
	Check(ClassifyCar(true, false, false, false, true) == CarOwner::Local &&
	          ClassifyCar(true, false, true, false, false) == CarOwner::Local &&
	          ClassifyCar(true, false, false, true, false) == CarOwner::Local,
	      "CoopIII applying somebody's report is always let through");

	// The two halves have to agree on every car, or a replica sits at zero
	// health on fire (damage let through, blow-up refused) or blows up
	// without ever being hurt (the other way round).
	bool agree = true;
	for (int bits = 0; bits < 32; ++bits) {
		const CarOwner owner =
		    ClassifyCar((bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0,
		                (bits & 8) != 0, (bits & 16) != 0);
		for (int weapon = 0; weapon < 256; ++weapon) {
			const bool applies =
			    DecideCarDamage(owner, true, false, static_cast<uint8_t>(weapon)) ==
			    CarDamageVerdict::Apply;
			if (applies != MayBlowUpCar(owner))
				agree = false;
		}
	}
	Check(agree, "damage and blow-up are refused together, for every car and cause");

	// What a replica does with each cause the local player could land on it.
	bool bulletsGo = true, otherStays = true;
	for (int weapon = 0; weapon < 256; ++weapon) {
		const CarDamageVerdict v =
		    DecideCarDamage(CarOwner::RemoteHost, true, false,
		                    static_cast<uint8_t>(weapon));
		if (IsForwardableDamage(static_cast<uint8_t>(weapon))) {
			if (v != CarDamageVerdict::Forward)
				bulletsGo = false;
		} else if (v != CarDamageVerdict::Refuse) {
			otherStays = false;
		}
	}
	Check(bulletsGo, "every ray and melee reach we land on a replica goes to its host");
	Check(otherStays, "everything else is refused here and sent nowhere");
	Check(DecideCarDamage(CarOwner::RemoteHost, true, false, WEAPONTYPE_EXPLOSION) ==
	          CarDamageVerdict::Refuse,
	      "a blast is refused and not forwarded - the host's replay of it counts");
	Check(DecideCarDamage(CarOwner::RemoteHost, true, false, WEAPONTYPE_FLAMETHROWER) ==
	          CarDamageVerdict::Refuse,
	      "and so is fire, which can be a replayed explosion's");
	Check(DecideCarDamage(CarOwner::RemoteHost, false, false, WEAPONTYPE_M16) ==
	          CarDamageVerdict::Refuse,
	      "a city NPC's shot at a replica is refused and not forwarded");
	Check(DecideCarDamage(CarOwner::RemoteHost, true, true, WEAPONTYPE_M16) ==
	          CarDamageVerdict::Refuse,
	      "nor is a round into a shell");
	Check(DecideCarDamage(CarOwner::RemoteDriver, true, false, WEAPONTYPE_UZI) ==
	          CarDamageVerdict::Forward,
	      "protocol 23 is unchanged: a driven car's hit still goes to its driver");
	Check(DecideCarDamage(CarOwner::RemoteCustodian, true, false, WEAPONTYPE_UZI) ==
	              CarDamageVerdict::Forward &&
	          !MayBlowUpCar(CarOwner::RemoteCustodian),
	      "a settling car's hit goes to its custodian, and it can't blow up here");
	Check(DecideCarDamage(CarOwner::RemoteCustodian, true, false,
	                      WEAPONTYPE_EXPLOSION) == CarDamageVerdict::Refuse,
	      "a blast on it is refused - the custodian's replay of it counts");
	Check(DecideCarDamage(CarOwner::Local, false, false, WEAPONTYPE_EXPLOSION) ==
	          CarDamageVerdict::Apply,
	      "and a car that is ours, or nobody's, takes whatever the engine gives it");

	// One of our own pedestrians' rounds (protocol.h, C_NpcVehicleHit).
	Check(DecideCarDamage(CarOwner::RemoteDriver, false, false, WEAPONTYPE_UZI, true) ==
	              CarDamageVerdict::Forward &&
	          DecideCarDamage(CarOwner::RemoteCustodian, false, false, WEAPONTYPE_UZI,
	                          true) == CarDamageVerdict::Forward,
	      "our pedestrian's round on a car a player drives or settles goes to them");
	Check(DecideCarDamage(CarOwner::RemoteHost, false, false, WEAPONTYPE_UZI, true) ==
	              CarDamageVerdict::Refuse &&
	          DecideCarDamage(CarOwner::Nobody, false, false, WEAPONTYPE_UZI, true) ==
	              CarDamageVerdict::Refuse,
	      "but not on traffic or a car nobody holds, which no player is in");
	Check(DecideCarDamage(CarOwner::RemoteDriver, false, true, WEAPONTYPE_UZI, true) ==
	              CarDamageVerdict::Refuse &&
	          DecideCarDamage(CarOwner::RemoteDriver, false, false, WEAPONTYPE_EXPLOSION,
	                          true) == CarDamageVerdict::Refuse,
	      "nor into a shell, nor a blast");
	Check(DecideCarDamage(CarOwner::Local, false, false, WEAPONTYPE_UZI, true) ==
	          CarDamageVerdict::Apply,
	      "and our own car takes it here, as it always did");
}

// ---- a replayed shot on something this machine owns -----------------------
//
// combat.h, ReplayedShotMayDamage. The shooter forwards what their round did
// to their copy, and the same round is replayed here through CWeapon::Fire.
// For every target, exactly one of the two may take the health.

void TestAReplayedShotCountsOnce() {
	std::printf("\na replayed shot and a forwarded hit count once between them\n");
	using game::CarDamageVerdict;
	using game::CarOwner;
	using game::ClassifyReplayCar;
	using game::ClassifyReplayPed;
	using game::DecideCarDamage;
	using game::ReplayedShotMayDamage;
	using game::ReplayMayAddExplosion;
	using game::ReplayTarget;
	using game::ShooterForwardsHitOn;

	// Every round a replay can land is a cause the shooter forwards, so the
	// refusal below covers all of them. Shotgun included: FireShotgun hits
	// every ped type, where DoBulletImpact skips the shooter's own.
	bool allForwardable = true;
	for (int w = 0; w < 256; ++w)
		if (IsInstantHitWeapon(static_cast<uint8_t>(w)) &&
		    !IsForwardableDamage(static_cast<uint8_t>(w)))
			allForwardable = false;
	Check(allForwardable, "every instant-hit weapon's damage is one the shooter forwards");

	struct Row {
		ReplayTarget target;
		bool         forwarded;
		const char  *what;
	};
	const Row rows[] = {
	    {ReplayTarget::NamedHostedPed,   true,  "a pedestrian we host and the session named"},
	    {ReplayTarget::UnnamedHostedPed, false, "a pedestrian we host, not yet named"},
	    {ReplayTarget::UnsyncedPed,      false, "a mission or script character"},
	    {ReplayTarget::CarWeDrive,       true,  "the car we are driving"},
	    {ReplayTarget::CarWeSettle,      true,  "the car we are settling"},
	    {ReplayTarget::NamedHostedCar,   true,  "a traffic car we host and the session named"},
	    {ReplayTarget::UnnamedHostedCar, false, "a traffic car we host, not yet named"},
	    {ReplayTarget::NobodysCar,       false, "a parked car, or a session car nobody holds"},
	};
	for (const Row &r : rows) {
		Check(ShooterForwardsHitOn(r.target) == r.forwarded, r.what);
		bool rounds = true, other = true;
		for (int w = 0; w < 256; ++w) {
			const bool may = ReplayedShotMayDamage(r.target, static_cast<uint8_t>(w));
			if (IsForwardableDamage(static_cast<uint8_t>(w))) {
				if (may == r.forwarded)
					rounds = false;
			} else if (!may) {
				other = false;
			}
		}
		Check(rounds, r.forwarded
		                  ? "  a replayed round takes no health off it"
		                  : "  a replayed round is the only way the shot reaches it, so it lands");
		Check(other, "  and a blast or a fire the replay set off still reaches it");
	}

	bool never = true;
	for (int w = 0; w < 256; ++w)
		if (ReplayedShotMayDamage(ReplayTarget::LocalPlayer, static_cast<uint8_t>(w)) ||
		    ReplayedShotMayDamage(ReplayTarget::OtherMachines, static_cast<uint8_t>(w)))
			never = false;
	Check(never, "the local player and other machines' things: nothing, whatever the cause");

	// Cars, against the shooter's own detour. How their machine sees each of
	// our cars, then: they forward exactly when we refuse the replay.
	struct Pair {
		ReplayTarget ours;
		CarOwner     theirs;
		const char  *what;
	};
	const Pair pairs[] = {
	    {ReplayTarget::CarWeDrive,       CarOwner::RemoteDriver,    "our car: driven, to them"},
	    {ReplayTarget::CarWeSettle,      CarOwner::RemoteCustodian, "our settle: custody, to them"},
	    {ReplayTarget::NamedHostedCar,   CarOwner::RemoteHost,      "our traffic: a replica, to them"},
	    {ReplayTarget::UnnamedHostedCar, CarOwner::Local,           "unnamed traffic: theirs to damage"},
	    {ReplayTarget::NobodysCar,       CarOwner::Local,           "a parked car: theirs to damage too"},
	};
	for (const Pair &p : pairs) {
		bool once = true;
		for (int w = 0; w < 256; ++w) {
			const bool forwards =
			    DecideCarDamage(p.theirs, true, false, static_cast<uint8_t>(w)) ==
			    CarDamageVerdict::Forward;
			const bool replay = ReplayedShotMayDamage(p.ours, static_cast<uint8_t>(w));
			if (IsForwardableDamage(static_cast<uint8_t>(w)) && forwards == replay)
				once = false;
		}
		Check(once, p.what);
	}

	// A named pedestrian is a replica on the shooter's machine, and
	// HookedInflictDamage forwards a round on a replica for exactly the
	// forwardable causes.
	bool pedOnce = true;
	for (int w = 0; w < 256; ++w)
		if (ReplayedShotMayDamage(ReplayTarget::NamedHostedPed, static_cast<uint8_t>(w)) ==
		    IsForwardableDamage(static_cast<uint8_t>(w)))
			pedOnce = false;
	Check(pedOnce, "a named pedestrian: their C_PedDamage or our replay, never both");

	// ClassifyReplayCar(owner, weDrive, weSettle, hostedTraffic, named)
	Check(ClassifyReplayCar(CarOwner::Local, true, false, false, false) ==
	          ReplayTarget::CarWeDrive,
	      "at our wheel: the car we drive");
	Check(ClassifyReplayCar(CarOwner::Local, true, true, true, true) ==
	          ReplayTarget::CarWeDrive,
	      "the wheel outranks everything else about it");
	Check(ClassifyReplayCar(CarOwner::Local, false, true, false, false) ==
	          ReplayTarget::CarWeSettle,
	      "our custody");
	Check(ClassifyReplayCar(CarOwner::Local, false, false, true, true) ==
	          ReplayTarget::NamedHostedCar,
	      "our named traffic");
	Check(ClassifyReplayCar(CarOwner::Local, false, false, true, false) ==
	          ReplayTarget::UnnamedHostedCar,
	      "our traffic still inside its naming round trip");
	Check(ClassifyReplayCar(CarOwner::Local, false, false, false, false) ==
	          ReplayTarget::NobodysCar,
	      "anything else local is nobody's");
	Check(ClassifyReplayCar(CarOwner::RemoteDriver, false, false, false, false) ==
	              ReplayTarget::OtherMachines &&
	          ClassifyReplayCar(CarOwner::RemoteCustodian, false, false, false, false) ==
	              ReplayTarget::OtherMachines &&
	          ClassifyReplayCar(CarOwner::RemoteHost, false, false, false, false) ==
	              ReplayTarget::OtherMachines,
	      "a car another machine holds is theirs");

	// ClassifyReplayPed(hosted, named)
	Check(ClassifyReplayPed(true, true) == ReplayTarget::NamedHostedPed &&
	          ClassifyReplayPed(true, false) == ReplayTarget::UnnamedHostedPed &&
	          ClassifyReplayPed(false, false) == ReplayTarget::UnsyncedPed &&
	          ClassifyReplayPed(false, true) == ReplayTarget::UnsyncedPed,
	      "pedestrians: named, unnamed, or not the session's at all");

	// The explosions a replay may add here.
	Check(!ReplayMayAddExplosion(EXPLOSION_GRENADE) &&
	          !ReplayMayAddExplosion(EXPLOSION_MOLOTOV) &&
	          !ReplayMayAddExplosion(EXPLOSION_ROCKET),
	      "not a projectile's - the thrower says where it went off");
	Check(!ReplayMayAddExplosion(EXPLOSION_BARREL),
	      "not a barrel's - the shooter relays their own as C_Explosion");
	Check(ReplayMayAddExplosion(3) && ReplayMayAddExplosion(4) &&
	          ReplayMayAddExplosion(EXPLOSION_HELI),
	      "a car the replayed round finished goes up, or BlowUpCar is left half done");
	Check(ReplayMayAddExplosion(-1), "a negative type is not ours to judge");
}

// ---- what a round does to the local player's body -------------------------
//
// combat.h, LocalPlayerHitReaction. The reaction goes with the damage: a
// replayed round never moves us, and a forwarded hit plays what the engine's
// own fire path for that weapon would have.

void TestAReplayedRoundDoesNotMoveUs() {
	std::printf("\na replayed round never moves us, the forwarded hit does\n");
	using game::EngineHitReaction;
	using game::HitAnimHoldOver;
	using game::HitReaction;
	using game::HitReactionRule;
	using game::LocalPlayerHitReaction;
	using game::REPLAY_HIT_ANIM_HOLD;
	using game::ShotgunMayKnockDown;
	using game::TowardShooter;

	bool still = true;
	for (int w = 0; w < 256; ++w)
		for (int seated = 0; seated < 2; ++seated)
			if (LocalPlayerHitReaction(true, static_cast<uint8_t>(w), seated != 0).kind !=
			    HitReaction::None)
				still = false;
	Check(still, "a replayed round: no reaction for any cause, on foot or seated");

	// Both kinds of session, counting what one round that hit us on the
	// shooter's screen does here. Friendly fire off, the server drops the
	// C_Damage and the replay is all that arrives. On, both arrive.
	const uint8_t guns[] = {game::WEAPONTYPE_COLT45, game::WEAPONTYPE_UZI,
	                        game::WEAPONTYPE_SHOTGUN, game::WEAPONTYPE_AK47,
	                        game::WEAPONTYPE_M16};
	for (int ff = 0; ff < 2; ++ff) {
		bool once = true;
		for (uint8_t w : guns) {
			int reactions = 0;
			if (LocalPlayerHitReaction(true, w, false).kind != HitReaction::None)
				++reactions;
			if (ff && LocalPlayerHitReaction(false, w, false).kind != HitReaction::None)
				++reactions;
			if (reactions != ff)
				once = false;
		}
		Check(once, ff ? "friendly fire on: one reaction per hit, and it's the forwarded one"
		               : "friendly fire off: a teammate's round never moves us");
	}

	// A forwarded hit with no replay behind it - the replay missed here. One
	// row per fire path, from the disassembly in addresses.h.
	struct Row {
		uint8_t     weapon;
		HitReaction kind;
		bool        inControl, holdGate, withDir;
		uint32_t    holdMs;
		const char *what;
	};
	const Row rows[] = {
	    {game::WEAPONTYPE_COLT45, HitReaction::Flinch, true, true, true, 1000,
	     "pistol: DoBulletImpact's flinch, held a second"},
	    {game::WEAPONTYPE_UZI, HitReaction::Flinch, true, true, true, 1000,
	     "uzi: the same"},
	    {game::WEAPONTYPE_AK47, HitReaction::Flinch, true, true, true, 2500,
	     "AK: the same, held two and a half"},
	    {game::WEAPONTYPE_M16, HitReaction::Flinch, true, true, true, 2500,
	     "M16: the same as the AK"},
	    {game::WEAPONTYPE_SNIPERRIFLE, HitReaction::Flinch, true, false, false, 0,
	     "sniper: CBulletInfo's flinch, front anim, no hold"},
	    {game::WEAPONTYPE_UZI_DRIVEBY, HitReaction::Flinch, false, false, true, 0,
	     "drive-by: FireInstantHitFromCar's flinch, no gate"},
	    {game::WEAPONTYPE_SHOTGUN, HitReaction::Knockdown, false, false, false, 0,
	     "shotgun: FireShotgun's shove and knockdown"},
	};
	for (const Row &r : rows) {
		const HitReactionRule got = LocalPlayerHitReaction(false, r.weapon, false);
		Check(got.kind == r.kind && got.inControl == r.inControl &&
		          got.holdGate == r.holdGate && got.withDir == r.withDir &&
		          got.holdMs == r.holdMs,
		      r.what);
	}
	Check(LocalPlayerHitReaction(false, game::WEAPONTYPE_UNARMED, false).kind ==
	              HitReaction::None &&
	          LocalPlayerHitReaction(false, game::WEAPONTYPE_BASEBALLBAT, false).kind ==
	              HitReaction::None,
	      "fists and the bat: nothing here, melee reacts through the fight code");
	bool seatedStill = true;
	for (uint8_t w : guns)
		if (LocalPlayerHitReaction(false, w, true).kind != HitReaction::None)
			seatedStill = false;
	Check(seatedStill, "seated: a forwarded hit doesn't shove or flinch us either");

	// Only a cause the shooter forwards can arrive as C_Damage at all.
	bool onlyForwarded = true;
	for (int w = 0; w < 256; ++w)
		if (EngineHitReaction(static_cast<uint8_t>(w)).kind != HitReaction::None &&
		    !IsForwardableDamage(static_cast<uint8_t>(w)))
			onlyForwarded = false;
	Check(onlyForwarded, "every cause with a reaction is one C_Damage can carry");

	// The direction off the wire picks one of four anims per block.
	bool fourDirs = true;
	for (int d = 0; d < 256; ++d)
		if (IsKnownDamageDirection(static_cast<uint8_t>(d)) && d > 3)
			fourDirs = false;
	Check(fourDirs, "a direction that gets through is 0..3, so 1Dh + dir stays a shot anim");

	// The gates, unsigned the way the binary compares them.
	Check(HitAnimHoldOver(1000, 1001) && !HitAnimHoldOver(1001, 1001) &&
	          !HitAnimHoldOver(5000, 1001),
	      "the flinch waits for the hold: `jae` skips it while the delay isn't over");
	const uint32_t nows[] = {0u, 1u, 2999u, 3000u, 123456u, 0xFFFFFFFEu, 0xFFFFFFFFu};
	bool shut = true;
	for (uint32_t now : nows)
		if (HitAnimHoldOver(REPLAY_HIT_ANIM_HOLD, now))
			shut = false;
	Check(shut, "the hold a replay parks is never over, at any time");

	Check(ShotgunMayKnockDown(0, 10000) && ShotgunMayKnockDown(7000, 10000) &&
	          !ShotgunMayKnockDown(7001, 10000),
	      "the shotgun knocks us down again only three seconds after getting up");
	Check(!ShotgunMayKnockDown(0xFFFFFFFFu, 10000),
	      "and never out of a fall SetFall made timeless");
	Check(ShotgunMayKnockDown(0, 1000),
	      "early in a session now - 3000 wraps, exactly like the engine's add");

	const Vec3 north{0.0f, 1.0f, 0.0f};
	const Vec3 dirs[] = {TowardShooter(north, 0), TowardShooter(north, 1),
	                     TowardShooter(north, 2), TowardShooter(north, 3),
	                     TowardShooter(north, 5)};
	Check(dirs[0].x == 0.0f && dirs[0].y == 1.0f, "direction 0: the shooter is in front");
	Check(dirs[1].x == -1.0f && dirs[1].y == 0.0f,
	      "direction 1: on the left, a quarter turn anticlockwise");
	Check(dirs[2].x == 0.0f && dirs[2].y == -1.0f, "direction 2: behind");
	Check(dirs[3].x == 1.0f && dirs[3].y == 0.0f, "direction 3: on the right");
	Check(dirs[4].x == dirs[1].x && dirs[4].y == dirs[1].y, "and only the low two bits count");
}

void TestTrafficHealthOnTheWire() {
	std::printf("\na traffic car's health rides the old padding\n");
	Check(EncodeAmbientHealth(1000.0f) == 1000, "full is 1000");
	Check(EncodeAmbientHealth(249.6f) == 250 && EncodeAmbientHealth(249.4f) == 249,
	      "whole points, rounded");
	Check(EncodeAmbientHealth(0.0f) == 1 && EncodeAmbientHealth(-40.0f) == 1,
	      "a live car never encodes to the 'unsaid' value");
	Check(EncodeAmbientHealth(std::numeric_limits<float>::quiet_NaN()) ==
	          AMBIENT_HEALTH_UNSAID,
	      "NaN is not a health");
	Check(EncodeAmbientHealth(1.0e9f) == 65535, "and a huge one clamps");

	float h = 777.0f;
	Check(!DecodeAmbientHealth(AMBIENT_HEALTH_UNSAID, h) && h == 777.0f,
	      "unsaid leaves the old value alone");
	Check(DecodeAmbientHealth(240, h) && h == 240.0f, "anything else is the number");

	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(610), CH_EVENT));
	Check(c.AmbientCar(610) && c.AmbientCar(610)->health == 1000.0f,
	      "a replica starts at full health");

	S_CarStates s{};
	InitHeader(s, 2000);
	s.ownerPlayerId      = 1;
	s.count              = 1;
	s.cars[0].netId      = 610;
	s.cars[0].health     = 240;
	s.cars[0].rot        = {0.0f, 0.0f, 0.0f, 1.0f};
	c.HandleMessage(Wrap(s, CH_SNAPSHOT));
	Check(c.AmbientCar(610)->health == 240.0f, "and takes what its host streams");

	s.hdr.sendTimeMs = 2100;
	s.cars[0].health = AMBIENT_HEALTH_UNSAID;
	c.HandleMessage(Wrap(s, CH_SNAPSHOT));
	Check(c.AmbientCar(610)->health == 240.0f,
	      "a row that doesn't say keeps the last value");

	s.hdr.sendTimeMs = 2200;
	s.ownerPlayerId  = 2;
	s.cars[0].health = 5;
	c.HandleMessage(Wrap(s, CH_SNAPSHOT));
	Check(c.AmbientCar(610)->health == 240.0f,
	      "and a machine that doesn't host it says nothing about it");
}

// A traffic car honking on its host, through OnCarStates and the per-frame
// correction. The pure half is in siren.cpp; this is the row being stamped
// and the countdown reaching the seam.
void TestTrafficHornThroughTheClient() {
	std::printf("\na traffic car's horn, through the client\n");
	Client c;
	c.SetBridge(RecordingBridge());
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(620), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientCarSpawns == 1, "the replica is built");
	Check(g_rec.ambientCorrections > 0 && g_rec.lastAmbientHornTimer == 0,
	      "and corrected every frame, silent");

	auto batch = [](uint32_t at, uint8_t owner, uint8_t mask) {
		S_CarStates s{};
		InitHeader(s, at);
		s.ownerPlayerId = owner;
		s.count         = 2;
		s.hornMask      = mask;
		s.cars[0].netId = 999;   // a car this machine has never heard of
		s.cars[0].rot   = {0.0f, 0.0f, 0.0f, 1.0f};
		s.cars[1].netId = 620;
		s.cars[1].rot   = {0.0f, 0.0f, 0.0f, 1.0f};
		return s;
	};

	c.HandleMessage(Wrap(batch(2000, 1, CarStateHornBit(1)), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastAmbientHornTimer == 44,
	      "the host says row 1 is honking: the replica starts at 44");
	c.Tick();
	Check(g_rec.lastAmbientHornTimer == 43, "and counts down one a frame");

	c.HandleMessage(Wrap(batch(2100, 1, CarStateHornBit(0)), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastAmbientHornTimer == 0,
	      "a bit on somebody else's row is not this car's");

	c.HandleMessage(Wrap(batch(2200, 1, CarStateHornBit(1)), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastAmbientHornTimer == 44, "honking again, from the top");
	c.HandleMessage(Wrap(batch(2300, 1, 0), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastAmbientHornTimer == 0, "the next batch lets go: silent at once");

	c.HandleMessage(Wrap(batch(2400, 2, CarStateHornBit(1)), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastAmbientHornTimer == 0,
	      "a machine that doesn't host it can't make it honk");

	c.HandleMessage(Wrap(batch(2500, 1, CarStateHornBit(1)), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastAmbientHornTimer == 44, "back on");
	std::this_thread::sleep_for(
	    std::chrono::milliseconds(game::HORN_FRESH_MS + 60));
	c.Tick();
	Check(g_rec.lastAmbientHornTimer == 0,
	      "and silent once its host has said nothing for HORN_FRESH_MS");
}

// A police car in somebody else's traffic with its siren on: the bit off the
// row, held between rows, and the driver the host's ped rows name for it.
void TestTrafficSirenThroughTheClient() {
	std::printf("\na traffic car's siren, through the client\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 530);
	c.HandleMessage(Wrap(MakeAmbientPedSpawn(531, 1, 12.0f), CH_EVENT));
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(630), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientCarSpawns == 1 && !g_rec.lastAmbientSirenOn,
	      "the replica is built with its siren off");

	auto batch = [](uint32_t at, uint8_t owner, uint8_t sirens, bool withCar = true) {
		S_CarStates s{};
		InitHeader(s, at);
		s.ownerPlayerId = owner;
		s.count         = 2;
		s.sirenMask     = sirens;
		s.cars[0].netId = 998;
		s.cars[0].rot   = {0.0f, 0.0f, 0.0f, 1.0f};
		s.cars[1].netId = withCar ? 630 : 997;
		s.cars[1].rot   = {0.0f, 0.0f, 0.0f, 1.0f};
		return s;
	};

	c.HandleMessage(Wrap(batch(2000, 1, CarStateSirenBit(0)), CH_SNAPSHOT));
	c.Tick();
	Check(!g_rec.lastAmbientSirenOn, "a bit on somebody else's row is not this car's");

	c.HandleMessage(Wrap(batch(2100, 1, CarStateSirenBit(1)), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastAmbientSirenOn, "the host says row 1's siren is on: it's on here");
	Check(!g_rec.lastAmbientDriverSaid, "nobody named at the wheel yet");

	c.HandleMessage(Wrap(batch(2200, 1, 0, /*withCar=*/false), CH_SNAPSHOT));
	c.Tick();
	std::this_thread::sleep_for(
	    std::chrono::milliseconds(game::HORN_FRESH_MS + 60));
	c.Tick();
	Check(g_rec.lastAmbientSirenOn,
	      "a batch without it, and a quiet spell, leave it as last seen");

	c.HandleMessage(Wrap(batch(2300, 2, 0), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastAmbientSirenOn, "a machine that doesn't host it can't turn it off");

	c.HandleMessage(Wrap(MakePedStates(1, 531, 2300, 12.0f, 0, /*vehicleNetId=*/630,
	                                   /*seat=*/1),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(!g_rec.lastAmbientDriverSaid, "a passenger is not a driver");

	c.HandleMessage(Wrap(MakePedStates(2, 530, 2300, 10.0f, 0, 630, 0), CH_SNAPSHOT));
	c.Tick();
	Check(!g_rec.lastAmbientDriverSaid,
	      "nor is a driver named by a machine that doesn't host the ped");

	c.HandleMessage(Wrap(MakePedStates(1, 530, 2400, 10.0f, 0, 630, 0), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.lastAmbientDriverSaid, "the host puts somebody in seat 0: a driver");

	c.HandleMessage(Wrap(MakePedStates(1, 530, 2500, 10.0f), CH_SNAPSHOT));
	c.Tick();
	Check(!g_rec.lastAmbientDriverSaid && g_rec.lastAmbientSirenOn,
	      "he gets out: lights still on, nobody at the wheel");

	c.HandleMessage(Wrap(batch(2600, 1, 0), CH_SNAPSHOT));
	c.Tick();
	Check(!g_rec.lastAmbientSirenOn, "the next row with it says off: off");
}

S_CarHit MakeCarHit(uint8_t attackerId, uint16_t netId, uint8_t weapon = 3,
                    float amount = 25.0f) {
	S_CarHit h;
	InitHeader(h, 1000);
	h.attackerId  = attackerId;
	h.body.netId  = netId;
	h.body.weapon = weapon;
	h.body.amount = amount;
	return h;
}

void TestAHitOnOurTrafficReachesTheEngine() {
	std::printf("a hit somebody landed on a replica of our traffic\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	c.HandleMessage(Wrap(MakeCarHit(1, 610, /*weapon=*/5, /*amount=*/34.0f), CH_EVENT));
	Check(g_rec.carHits == 1, "it goes straight to the seam, by netId");
	Check(g_rec.lastCarHit.netId == 610 && g_rec.lastCarHit.weapon == 5 &&
	          g_rec.lastCarHit.amount == 34.0f,
	      "with the three fields the shooter's own InflictDamage had");
	Check(g_rec.carHitAttacker == 1, "credited to the player who fired");

	c.HandleMessage(Wrap(MakeCarHit(5, 610), CH_EVENT));
	Check(g_rec.carHits == 2 && g_rec.carHitAttacker == 0xFF,
	      "a shooter we don't know still lands the hit, blaming nobody");
	c.HandleMessage(Wrap(MakeCarHit(0, 610), CH_EVENT));
	Check(g_rec.carHits == 3 && g_rec.carHitAttacker == 0xFF,
	      "and one claiming to be us is not blamed on our own ped");

	const int applied = g_rec.carHits;
	c.Tick();
	c.Tick();
	Check(g_rec.carHits == applied, "applied on the packet, not every frame");

	WorldBridge bare = RecordingBridge();
	bare.ApplyHostedCarHit = nullptr;
	Client d;
	d.SetBridge(bare);
	d.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	d.HandleMessage(Wrap(MakeCarHit(1, 610), CH_EVENT));
	Check(g_rec.carHits == 0, "a build with no seam drops it without crashing");
}

void TestWhichTrafficHitsAreWorthSending() {
	std::printf("which hits on a traffic replica go to its host\n");
	RemoteAmbientCar car;
	car.active        = true;
	car.netId         = 610;
	car.ownerPlayerId = 1;
	Check(CarHitIsWorthSending(car, 0), "a live replica somebody else hosts");

	RemoteAmbientCar gone = car;
	gone.active = false;
	Check(!CarHitIsWorthSending(gone, 0), "not a row the roster has dropped");
	RemoteAmbientCar wreck = car;
	wreck.destroyed = true;
	Check(!CarHitIsWorthSending(wreck, 0), "not a car the host already burned out");
	RemoteAmbientCar mine = car;
	mine.claimPending = true;
	Check(!CarHitIsWorthSending(mine, 0),
	      "not one we're driving and have asked to promote");
	Check(!CarHitIsWorthSending(car, 1), "not one the session says we host");
	RemoteAmbientCar orphan = car;
	orphan.ownerPlayerId = INVALID_PLAYER;
	Check(!CarHitIsWorthSending(orphan, 0), "not one with no host at all");

	// The send path drains the queue whether or not anything goes out, so a
	// hit on a car the roster no longer has can't pile up behind it.
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(610), CH_EVENT));
	VehicleHitBody hit{};
	hit.netId  = 610;
	hit.weapon = WEAPONTYPE_AK47;
	hit.amount = 30.0f;
	g_rec.localCarHits.push_back(hit);
	hit.netId = 9999;
	g_rec.localCarHits.push_back(hit);
	c.TickCarHitsForTest();
	Check(g_rec.carHitDrains == 1 && g_rec.localCarHits.empty(),
	      "one drain takes both, the known car and the unknown one");
}

void TestAPedHitIsItsOwnKindOfEvent() {
	std::printf("a hit on a pedestrian and a hit on a player are two events\n");

	// The two netIds name different tables on the server - a player slot and a
	// row in the ambient roster - so they are two packets and two relays. A
	// single kind with a "which" byte would put one packet's routing in a byte
	// nobody reading the wire could check.
	CombatEvent onAPlayer;
	onAPlayer.kind               = CombatEvent::DAMAGE;
	onAPlayer.damage.victimNetId = 100;

	CombatEvent onAPed;
	onAPed.kind            = CombatEvent::PED_DAMAGE;
	onAPed.pedDamage.netId = 100;

	Check(onAPlayer.kind != onAPed.kind, "the drain can tell them apart");
	Check(sizeof(PedDamageBody) == sizeof(DamageBody),
	      "and they are the same five arguments of the same engine function");

	// The queue is the bridge's and the drain is bounded, the same as every
	// other combat event. Checked through the recorder rather than the socket,
	// because PostFrame needs a live one.
	g_rec = Recorder{};
	for (int i = 0; i < 3; ++i)
		g_rec.localCombat.push_back(onAPed);
	CombatEvent   scratch[8];
	const uint8_t n = RecDrainLocalCombat(scratch, 8);
	Check(n == 3, "three queued, three drained");
	Check(scratch[0].kind == CombatEvent::PED_DAMAGE &&
	          scratch[0].pedDamage.netId == 100,
	      "and they come back out as what they went in as");
}

void TestAPedInACarCannotBeShotToDeath() {
	std::printf("the one thing the engine will not let this feature do\n");

	// Measured, not reasoned. CPed::InflictDamage's in-vehicle arm at
	// 0x004EACF3 sends everything but WEAPONTYPE_DROWNING to 0x004EADD0, which
	// writes 1.0f into m_fHealth and answers "did not die".
	Check(CanKillPedInVehicle(WEAPONTYPE_DROWNING),
	      "drowning is the only death there is for a ped in a seat");
	Check(!CanKillPedInVehicle(WEAPONTYPE_COLT45) &&
	          !CanKillPedInVehicle(WEAPONTYPE_M16) &&
	          !CanKillPedInVehicle(WEAPONTYPE_ROCKETLAUNCHER) &&
	          !CanKillPedInVehicle(WEAPONTYPE_UNARMED),
	      "and nothing else is, however much damage it carries");

	// The consequence, walked over every byte a weapon field can hold rather
	// than asserted: no cause this wire can carry is able to kill a ped in a
	// car. Drowning happens on the machine the ped is drowning on and is not
	// forwardable, so the two lists do not intersect - which is why the apply
	// path does not special-case a seated ped and must never synthesise a
	// death the engine declined to give.
	int both = 0;
	for (int w = 0; w < 256; ++w) {
		const uint8_t weapon = static_cast<uint8_t>(w);
		if (IsForwardableDamage(weapon) && CanKillPedInVehicle(weapon))
			++both;
	}
	Check(both == 0,
	      "nothing that may be forwarded can kill a ped in a seat, over all 256 "
	      "causes");
}

// ---- our flame reaching what another machine owns --------------------------
//
// combat.h, IsOurFlame and below. The flamethrower lights; the fire burns.
// What the wire carries is the ignition, recognised inside CShotInfo::Update
// by its source, and never the burning.

void TestOnlyOurOwnFlameIsForwarded() {
	std::printf("\nonly our own flamethrower's ignition goes to the owner\n");

	Check(IsOurFlame(/*insideShotInfoUpdate=*/true, /*fleeFromIsLocalPlayer=*/true),
	      "our flame, lit inside CShotInfo::Update with our ped as the source");

	// The double count this has to rule out. Our own molotov and rocket name
	// our ped as the source too, but their fires come out of
	// CExplosion::Update, and the owner replays that explosion and lights its
	// own entities from it.
	Check(!IsOurFlame(false, true),
	      "not our molotov's or our rocket's fire: same source, but it comes out "
	      "of CExplosion::Update and every machine lights its own from the replay");
	// Somebody else's flamethrower, replayed here: it is in the window, and
	// its source is our copy of their ped. Their machine forwards it.
	Check(!IsOurFlame(true, false),
	      "not another player's replayed flame, which is theirs to forward");
	Check(!IsOurFlame(false, false),
	      "and not another player's replayed explosion");

	// And the burning itself is never forwarded, only the ignition. Our fire on
	// our copy of somebody's car hits CVehicle::InflictDamage with cause 9
	// every frame; forwarding that would be sixty packets a second and a
	// second lot of damage on top of the owner's own fire.
	Check(!IsForwardableDamage(WEAPONTYPE_FLAMETHROWER),
	      "cause 9 is still not forwardable as damage");
	Check(DecideCarDamage(CarOwner::RemoteHost, true, false, WEAPONTYPE_FLAMETHROWER) ==
	              CarDamageVerdict::Refuse &&
	          DecideCarDamage(CarOwner::RemoteDriver, true, false,
	                          WEAPONTYPE_FLAMETHROWER) == CarDamageVerdict::Refuse &&
	          DecideCarDamage(CarOwner::RemoteCustodian, true, false,
	                          WEAPONTYPE_FLAMETHROWER) == CarDamageVerdict::Refuse,
	      "so our fire on our copy of their car is refused here and not sent");
	Check(DecideCarDamage(CarOwner::Local, false, false, WEAPONTYPE_FLAMETHROWER) ==
	          CarDamageVerdict::Apply,
	      "while the owner's own fire on its own car does the damage");

	Check(!FlameGoesToOwner(CarOwner::Local),
	      "a car we drive or host burns here and nobody is told");
	Check(FlameGoesToOwner(CarOwner::RemoteDriver) &&
	          FlameGoesToOwner(CarOwner::RemoteCustodian) &&
	          FlameGoesToOwner(CarOwner::RemoteHost),
	      "a car anybody else holds gets the ignition");
}

void TestFlameReachIsTheEngines() {
	std::printf("the flame reaches a pedestrian by CShotInfo::Update's own test\n");

	// 0x0055C148: the radius is floored at 1.0f.
	Check(FlameReachesPed(0.9f, 0.2f), "a young flame still reaches one unit");
	Check(!FlameReachesPed(1.0f, 0.2f), "and the compare is strict, like fcomp's");

	// 0x0055C1CB compares the squared distance against the radius itself.
	// Transcribed, not corrected: a replica is reached exactly when the real
	// pedestrian would have been.
	Check(FlameReachesPed(3.0f, 4.0f), "1.7 away from a radius-4 flame is inside");
	Check(!FlameReachesPed(5.0f, 4.0f),
	      "2.2 away is not, because the engine never squares the radius");

	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(!FlameReachesPed(nan, 4.0f) && !FlameReachesPed(0.5f, nan),
	      "a NaN reaches nobody, same as the x87 compare");
}

void TestAFlameOnTheWireIsAnIgnition() {
	std::printf("cause 9 on a hit packet means light it\n");

	Check(IsFlameIgnition(WEAPONTYPE_FLAMETHROWER), "cause 9 is the ignition");

	// The two readings of a hit packet never overlap, so a receiver can branch
	// on the ignition first and the damage path never sees one - and a build
	// from before this, which only has the damage path, drops it.
	int both = 0, ignitions = 0;
	for (int w = 0; w < 256; ++w) {
		const uint8_t weapon = static_cast<uint8_t>(w);
		if (IsFlameIgnition(weapon))
			++ignitions;
		if (IsFlameIgnition(weapon) && IsForwardableDamage(weapon))
			++both;
	}
	Check(ignitions == 1, "one cause, and only one, is an ignition");
	Check(both == 0, "and no cause is both an ignition and a hit, over all 256");

	// The owner makes the two tests the shooter could not.
	Check(OwnerLightsFlame(false, false), "the owner lights what can burn");
	Check(!OwnerLightsFlame(true, false), "not something bFireProof on its machine");
	Check(!OwnerLightsFlame(false, true), "and not a wreck");
}

void TestAFlameIsReportedOnceASecond() {
	std::printf("a flame standing on somebody is reported once a second\n");

	FlameReportThrottle t;
	Check(t.Due(FlameTargetKind::Pedestrian, 700, 5000), "the first reach goes out");
	Check(!t.Due(FlameTargetKind::Pedestrian, 700, 5016),
	      "the next frame's does not");
	Check(!t.Due(FlameTargetKind::Pedestrian, 700, 5999), "nor anything inside the second");
	Check(t.Due(FlameTargetKind::Pedestrian, 700, 6000), "then again, once");
	Check(t.Due(FlameTargetKind::Pedestrian, 701, 6001), "another pedestrian is his own");
	Check(t.Due(FlameTargetKind::Traffic, 700, 6002),
	      "and a traffic car under the same number is a different thing");
	Check(t.Due(FlameTargetKind::DrivenCar, 700, 6003),
	      "as is a session car under it");

	// The timer is CTimer's, which wraps. Unsigned subtraction still reads the
	// gap across the wrap.
	FlameReportThrottle w;
	Check(w.Due(FlameTargetKind::Pedestrian, 1, 0xFFFFFF00u), "just before the wrap");
	Check(!w.Due(FlameTargetKind::Pedestrian, 1, 0x00000010u),
	      "0x110 ms later, across it, is still too soon");
	Check(w.Due(FlameTargetKind::Pedestrian, 1, 0x00000400u), "and 1.28 s later is not");

	// More burning targets than rows costs an early repeat for the stalest
	// one, never a missed first report.
	FlameReportThrottle full;
	for (uint16_t id = 0; id < FlameReportThrottle::ROWS; ++id)
		full.Due(FlameTargetKind::Pedestrian, id, 100u + id);
	Check(full.Due(FlameTargetKind::Pedestrian, 999, 200),
	      "a seventeenth target is still reported");
	Check(full.Due(FlameTargetKind::Pedestrian, 0, 201),
	      "at the cost of the stalest row, which may report again early");
	Check(!full.Due(FlameTargetKind::Pedestrian, 15, 202),
	      "while the freshest keep their place");
}

void TestAFlameReachesTheSeamLikeAHit() {
	std::printf("an ignition off the wire reaches the seam like any hit\n");

	// The client does not read the cause on any of the three packets, which
	// is what lets them carry an ignition without a routing change here or on
	// the server (Session::PedDamageRecipient and friends take no weapon).
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	c.HandleMessage(Wrap(MakePedDamage(1, 700, WEAPONTYPE_FLAMETHROWER, 0.0f,
	                                   PEDPIECE_TORSO, 0),
	                     CH_EVENT));
	Check(g_rec.pedDamages == 1 &&
	          g_rec.lastPedDamage.weapon == WEAPONTYPE_FLAMETHROWER &&
	          g_rec.lastPedDamage.amount == 0.0f,
	      "a flame on one of our pedestrians, with no amount");
	Check(g_rec.pedDamageAttacker == 1, "blaming the player whose flame it was");

	c.HandleMessage(Wrap(MakeCarHit(1, 610, WEAPONTYPE_FLAMETHROWER, 0.0f), CH_EVENT));
	Check(g_rec.carHits == 1 && g_rec.lastCarHit.weapon == WEAPONTYPE_FLAMETHROWER,
	      "a flame on our traffic");

	Client d;
	d.SetBridge(RecordingBridge());
	GiveUsCar77(d);
	d.HandleMessage(Wrap(MakeVehicleHit(1, 77, WEAPONTYPE_FLAMETHROWER, 0.0f), CH_EVENT));
	Check(g_rec.vehicleHits == 1 &&
	          g_rec.lastVehicleHit.weapon == WEAPONTYPE_FLAMETHROWER,
	      "and a flame on the car we are driving");
}

void TestADyingDriverLeavesHisSeatFirst() {
	std::printf("\na dying traffic driver is taken out of his car first\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 525);
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(610), CH_EVENT));
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 525, 2000, 11.0f, 0,
	                                   /*vehicleNetId=*/610, /*seat=*/0),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.ambientSeats == 1, "he is driving");

	const int unseatsBefore = g_rec.ambientUnseats;
	c.HandleMessage(Wrap(MakePedDeath(525), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientKills == 1, "he dies");
	Check(g_rec.ambientUnseats == unseatsBefore + 1,
	      "and he came out of the seat to do it");
	// The ordering, not the effect. CPed::SetDie's PED_DRIVING arm calls
	// FlagToDestroyWhenNextProcessed on anything that is not the player, and
	// every replica is a CCivilianPed - so a seated one killed in place is
	// handed to the engine to delete.
	Check(g_rec.unseatsAtLastAmbientKill == unseatsBefore + 1,
	      "the unseat had already happened when the kill ran");

	// And the standing seat request is cancelled, or the reconciliation loop
	// puts the corpse back behind the wheel on the very next frame.
	const int seats = g_rec.ambientSeats;
	c.Tick();
	c.Tick();
	Check(g_rec.ambientSeats == seats, "and he is not put back in it");
}

void TestAmbientPedNobodyStreamsIsHeld() {
	std::printf("\na pedestrian outside the batch is held, not dropped\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 501, /*x=*/13.0f);

	// Only twelve hosted peds fit in a packet, nearest the sender first, so
	// a replica can go a long time with nothing said about it. That is
	// §2.1's far band and it has to mean "stand where you were last seen",
	// not "stop existing".
	const int before = g_rec.ambientApplies;
	for (int i = 0; i < 5; ++i)
		c.Tick();
	Check(g_rec.ambientApplies == before + 5,
	      "five frames with no state at all still place it five times");
	Check(g_rec.lastAmbientPose.pos.x == 13.0f, "at where the spawn put it");
}

void TestAmbientPedStatesFromTheWrongMachine() {
	std::printf("\nonly the machine hosting a ped may say where it is\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 502, /*x=*/13.0f);

	c.HandleMessage(Wrap(MakePedStates(/*owner=*/4, 502, 2000, 99.0f),
	                     CH_SNAPSHOT));
	const RemoteAmbientPed *p = c.AmbientPed(502);
	Check(p != nullptr && p->last.pos.x == 13.0f,
	      "a batch from somebody who does not host it changes nothing");
}

void TestTrafficDriverTakesHisSeat() {
	std::printf("\nthe traffic driver gets into his car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 510);
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(600), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientCarSpawns == 1, "the car exists too");

	c.HandleMessage(Wrap(MakePedStates(1, 510, 2000, 11.0f, 0,
	                                   /*vehicleNetId=*/600, /*seat=*/0),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.ambientSeats == 1, "he is seated");
	Check(g_rec.lastAmbientSeatCar == 600 && g_rec.lastAmbientSeatIdx == 0,
	      "in car 600, as the driver");

	// A seated ped is positioned by CWorld::Process from the car's own
	// matrix, every frame. Writing a position over the top of that is what
	// would drag him half out of the seat for exactly the part of the frame
	// collision looks at.
	const int applies = g_rec.ambientApplies;
	c.HandleMessage(Wrap(MakePedStates(1, 510, 2100, 12.0f, 0, 600, 0),
	                     CH_SNAPSHOT));
	c.Tick();
	c.Tick();
	Check(g_rec.ambientApplies == applies,
	      "and his position stops being written, because the car owns it now");
	Check(g_rec.ambientSeatAttempts == 1,
	      "and a restated instruction does not re-seat him every frame");
}

void TestDriverSeatWaitsForTheCar() {
	std::printf("\nthe link arrives before the car, which is the normal case\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 511);

	// The ped and the car are announced separately and each waits on its own
	// model, so the pairing routinely names a car this machine does not have
	// yet. As a standing instruction that is a "not yet"; as an event it
	// would be a driver who never sits down.
	c.HandleMessage(Wrap(MakePedStates(1, 511, 2000, 11.0f, 0, 601, 0),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.ambientSeatAttempts == 0, "nothing is attempted yet");
	Check(c.AmbientPed(511) != nullptr && !c.AmbientPed(511)->Seated(),
	      "and he is not considered seated");
	Check(c.AmbientPed(511)->seatVehicleNetId == 601,
	      "but the instruction stands");

	c.HandleMessage(Wrap(MakeAmbientCarSpawn(601), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientSeats == 1, "he is seated on the first frame both exist");
}

// ---- a traffic car its host destroyed (docs/roadmap.md 5.8) ---------------
//
// The ambient half. Most of an ambient car's destruction already travels -
// the explosion that killed it was replayed here at a position everybody
// agreed on - but the replicas have independent healths (nothing on the
// ambient wire carries a condition) and a destruction nobody's player caused
// is never relayed at all, so this is the backstop for both.

void TestAnAmbientWreckIsAppliedToTheReplica() {
	std::printf("\na traffic car its host destroyed becomes a wreck here\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(610), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientCarSpawns == 1, "the replica exists");

	c.HandleMessage(Wrap(
	    MakeUnownedBlowUp(610, UNOWNED_AMBIENT, 1, Vec3{120.5f, -33.25f, 8.0f}),
	    CH_EVENT));
	Check(g_rec.ambientWrecks == 0, "the handler applies nothing by itself");

	c.Tick();
	Check(g_rec.ambientWrecks == 1, "the frame pump does");
	Check(g_rec.lastAmbientWreckNetId == 610, "on the car it was told about");
	// The whole point of protocol 16. The replica has been sitting wherever
	// the last stream tick left it, which is from before the explosion,
	// because a host stops streaming a car the frame it burns out.
	Check(g_rec.lastAmbientWreckWhere.pos.x == 120.5f &&
	          g_rec.lastAmbientWreckWhere.pos.y == -33.25f &&
	          g_rec.lastAmbientWreckWhere.pos.z == 8.0f,
	      "and at the place the host says it blew up, not where our replica was");
	Check(c.AmbientCar(610) != nullptr && c.AmbientCar(610)->destroyed,
	      "and the roster agrees it is finished");

	// Not through the parked resolver: a netId is the session's name for a
	// replica, not a name the map hands out.
	Check(g_rec.unownedWrecks == 0, "and not through the parked seam");

	c.Tick();
	Check(g_rec.ambientWrecks == 1, "asked exactly once");
}

void TestAnAmbientWreckForACarWeDoNotHaveIsDropped() {
	std::printf("\na wreck for traffic this machine never heard of\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// Nothing to build from - the packet carries no model - and a wreck is
	// not a reason to invent a car.
	c.HandleMessage(Wrap(MakeUnownedBlowUp(4242, UNOWNED_AMBIENT), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.ambientWrecks == 0, "the engine is never asked");
}

void TestAWreckedAmbientCarIsNeverBuilt() {
	std::printf("\na traffic car destroyed while its model was still loading\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// The spawn arrives, the model is not resident yet, so there is a row and
	// no replica. This is the ordinary state for a backfilled car.
	g_rec.modelReady = false;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(611), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientCarSpawns == 0, "nothing is built yet");

	g_rec.ambientWreckOutcome = UnownedWreckOutcome::NotHere;
	c.HandleMessage(Wrap(MakeUnownedBlowUp(611, UNOWNED_AMBIENT), CH_EVENT));
	c.Tick();
	Check(c.AmbientCar(611) != nullptr && c.AmbientCar(611)->destroyed,
	      "but the session has written it off");

	// The model lands. Building the car now would put a pristine one in a
	// street where every other machine has a burnt shell - and the same guard
	// is what stops the local engine's reaping of a wreck bringing it back.
	g_rec.modelReady = true;
	c.Tick();
	c.Tick();
	Check(g_rec.ambientCarSpawns == 0, "and it is never built");
}

void TestAmbientCarIsEmptiedBeforeItIsDestroyed() {
	std::printf("\nthe order of an ambient despawn under a seated driver\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 512);
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(602), CH_EVENT));
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 512, 2000, 11.0f, 0, 602, 0),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(c.AmbientPed(512)->Seated(), "seated");

	S_CarDespawn gone;
	InitHeader(gone, 3000);
	gone.netId = 602;
	c.HandleMessage(Wrap(gone, CH_EVENT));

	Check(g_rec.ambientCarDespawns == 1, "the car is destroyed");
	Check(g_rec.unseatsAtLastAmbientCarDespawn == 1,
	      "and he was taken out of it first, not after");
	Check(!c.AmbientPed(512)->Seated(), "he is on foot");
	Check(c.AmbientPed(512)->seatVehicleNetId == INVALID_NETID,
	      "and the instruction went with the car, so nothing asks for it again");
}

void TestAmbientPedIsEmptiedBeforeItIsDestroyed() {
	std::printf("\nand the other way round\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 513);
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(603), CH_EVENT));
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 513, 2000, 11.0f, 0, 603, 0),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(c.AmbientPed(513)->Seated(), "seated");

	S_PedDespawn gone;
	InitHeader(gone, 3000);
	gone.netId = 513;
	c.HandleMessage(Wrap(gone, CH_EVENT));
	Check(g_rec.ambientUnseats == 1, "he is taken out of the seat");
	Check(g_rec.ambientPedDespawns == 1, "and then destroyed");
}

void TestRefusedAmbientSeatingIsNotRetriedForever() {
	std::printf("\na seating the engine refuses is asked for once\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 514);
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(604), CH_EVENT));
	c.Tick();
	g_rec.refuseAmbientSeating = true;

	c.HandleMessage(Wrap(MakePedStates(1, 514, 2000, 11.0f, 0, 604, 0),
	                     CH_SNAPSHOT));
	for (int i = 0; i < 5; ++i)
		c.Tick();
	Check(g_rec.ambientSeatAttempts == 1,
	      "with both halves present, a refusal is not retried sixty times a "
	      "second");
	Check(g_rec.ambientSeats == 0, "and he is not considered seated");

	// The owner says it again on its next batch, which is the whole
	// advantage of a stream over an event: an event that was refused is gone.
	g_rec.refuseAmbientSeating = false;
	c.HandleMessage(Wrap(MakePedStates(1, 514, 2100, 11.0f, 0, 604, 0),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.ambientSeats == 1, "and the next batch re-states it");
}

// The bug: nothing ever reset RemoteAmbientPed::poolHandle, so a replica the
// engine deleted left a row pointing at a pool slot that would never resolve
// again - no ped, no rebuild, and two comments saying the spawn pass would
// deal with it.
void TestAmbientReplicaTakenByTheEngineIsRebuilt() {
	std::printf("\na ped replica the engine took away is built again\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 516);
	Check(g_rec.ambientPedSpawns == 1, "the replica exists");
	Check(g_rec.ambientLivenessChecks == 0,
	      "nothing was checked before there was anything to check");

	c.Tick();
	Check(g_rec.ambientLivenessChecks == 1,
	      "a live replica is asked about once a frame");
	Check(g_rec.ambientPedSpawns == 1, "and a live one is not rebuilt");

	// And now the engine takes it. The check and the rebuild are the same
	// loop, deliberately: the replica is back in the same frame it went,
	// rather than in the next one the way CorrectAmbientCarReplica's recovery
	// leaves a car for a frame. There is nothing to wait for - the row still
	// knows the model, the owner and the pose.
	g_rec.ambientReplicaTaken = false;
	g_rec.ambientReplicaTaken = true;
	c.Tick();
	g_rec.ambientReplicaTaken = false;
	Check(g_rec.ambientPedSpawns == 2,
	      "the frame that notices is also the frame that rebuilds");
	Check(c.AmbientPed(516) != nullptr && c.AmbientPed(516)->poolHandle >= 0,
	      "and the row names the new one");

	// The row survived: the session never said this pedestrian went away, so
	// forgetting it would be this machine deciding somebody else's ped died.
	Check(c.AmbientPed(516)->netId == 516, "the row is the same row");
	Check(g_rec.ambientPedDespawns == 0,
	      "and nothing was destroyed - the engine already had");

	// The rebuild is one ped, not two. A recovery that re-armed a spawn
	// without clearing the handle first would leave the old object standing
	// in the street with nothing tracking it, which is exactly the duplicate
	// this branch went looking for.
	for (int i = 0; i < 10; ++i)
		c.Tick();
	Check(g_rec.ambientPedSpawns == 2, "and it stays one ped");

	// And the handle really is cleared rather than the same-frame rebuild
	// merely hiding it. With the model evicted there is nothing to rebuild
	// with, so the row has to sit there with no ped and the spawn armed -
	// which is the state the bug could never reach, because nothing reset
	// the handle and so `spawnPending` was never looked at again.
	g_rec.modelReady          = false;
	g_rec.ambientReplicaTaken = true;
	c.Tick();
	g_rec.ambientReplicaTaken = false;
	Check(c.AmbientPed(516)->poolHandle < 0,
	      "the handle is cleared rather than left pointing at a freed slot");
	Check(c.AmbientPed(516)->spawnPending, "and the spawn is armed");
	Check(g_rec.ambientPedSpawns == 2, "with nothing built yet");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.ambientPedSpawns == 3,
	      "and it is built as soon as the model comes back");
}

// The seat pass reads poolHandle, and it runs after the spawn pass. If the
// liveness check ran anywhere later, the seat pass would spend that frame
// asking the engine to seat a ped that no longer exists - and its own
// "the engine refused, drop the instruction" arm would throw away a standing
// instruction that is still true.
void TestASeatedReplicaTakenByTheEngineKeepsItsInstruction() {
	std::printf("\na seated replica the engine takes keeps its seat order\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 517);
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(606), CH_EVENT));
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 517, 2000, 11.0f, 0, 606, 0),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(c.AmbientPed(517)->Seated(), "seated");

	const int seatsBefore = g_rec.ambientSeats;

	// With the model gone there is nothing to rebuild with, so this is the
	// frame where the row has no ped at all - the one the seat pass has to
	// survive. It must not spend it asking the engine to seat a ped that does
	// not exist, because its own "the engine refused, so stop asking" arm
	// would then retire an instruction that is still true.
	g_rec.modelReady          = false;
	g_rec.ambientReplicaTaken = true;
	c.Tick();
	g_rec.ambientReplicaTaken = false;
	Check(!c.AmbientPed(517)->Seated(),
	      "a ped that no longer exists is not in a seat");
	Check(c.AmbientPed(517)->seatVehicleNetId == 606,
	      "but the session still says he belongs in that car");
	Check(g_rec.ambientSeats == seatsBefore,
	      "and nothing was asked to seat a ped that is not there");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.ambientSeats == seatsBefore + 1,
	      "so the rebuilt ped is put back in it");
	Check(c.AmbientPed(517)->Seated(), "and he is driving again");
}

// ---- a replica that dies on its own (docs/population.md §5.6) --------------
//
// The last case §5 left open, and the answer to it is that nothing goes on
// the wire: an observer's copy of somebody else's pedestrian dying is this
// machine being wrong, not this machine knowing something. These pin the
// arithmetic that decision rests on, all of it read out of the retail image.

// The measurement the whole choice turned on. If the proof flags were a
// complete switch then setting them would be the fix and none of the rest of
// this would exist - so the table is checked rather than trusted, the same way
// docs/protocol.md §1.10.2 checks the vehicle one.
void TestWhichPedDamageCausesTheProofFlagsMiss() {
	std::printf("\nsix of CPed::InflictDamage's causes read no proof flag\n");

	using game::PedProof;
	using game::PedProofForDamageCause;

	Check(PedProofForDamageCause(game::WEAPONTYPE_UNARMED) == PedProof::Melee &&
	          PedProofForDamageCause(game::WEAPONTYPE_BASEBALLBAT) ==
	              PedProof::Melee,
	      "a fist and a bat read bMeleeProof");
	Check(PedProofForDamageCause(game::WEAPONTYPE_COLT45) == PedProof::Bullet &&
	          PedProofForDamageCause(game::WEAPONTYPE_SNIPERRIFLE) ==
	              PedProof::Bullet,
	      "the six guns read bBulletProof");
	Check(PedProofForDamageCause(game::WEAPONTYPE_FLAMETHROWER) == PedProof::Fire,
	      "the flamethrower reads bFireProof");
	Check(PedProofForDamageCause(game::WEAPONTYPE_ROCKETLAUNCHER) ==
	              PedProof::Explosion &&
	          PedProofForDamageCause(game::WEAPONTYPE_GRENADE) ==
	              PedProof::Explosion &&
	          PedProofForDamageCause(game::WEAPONTYPE_MOLOTOV) ==
	              PedProof::Explosion &&
	          PedProofForDamageCause(game::WEAPONTYPE_EXPLOSION) ==
	              PedProof::Explosion,
	      "the four blasts read bExplosionProof");
	Check(PedProofForDamageCause(game::WEAPONTYPE_RAMMEDBYCAR) ==
	              PedProof::Collision &&
	          PedProofForDamageCause(game::WEAPONTYPE_RUNOVERBYCAR) ==
	              PedProof::Collision &&
	          PedProofForDamageCause(game::WEAPONTYPE_FALL) == PedProof::Collision,
	      "a car and a fall read bCollisionProof");

	// The two the code names by hand, because they are the two that actually
	// bit: a replica standing in water drowned locally, and the default arm
	// is whatever nobody thought of.
	Check(!game::PedProofFlagsStopCause(game::WEAPONTYPE_DROWNING),
	      "drowning reads nothing - 0x004EABAD sets the animation and jumps "
	      "straight to the arithmetic");
	Check(!game::PedProofFlagsStopCause(game::WEAPONTYPE_UZI_DRIVEBY),
	      "and a drive-by lands in the default arm with no flag either");

	// And the whole range, so a seventh cannot be added without this failing.
	// 0..21 have an arm (`cmp eax,15h / ja` at 0x004EA5A2); 22 and up do not.
	int leaksInRange = 0;
	for (int cause = 0; cause < 256; ++cause) {
		const bool stopped =
		    game::PedProofFlagsStopCause(static_cast<uint8_t>(cause));
		if (cause > 21) {
			if (stopped)
				++g_failures;
			continue;
		}
		if (!stopped)
			++leaksInRange;
	}
	Check(leaksInRange == 6,
	      "exactly six of the twenty-two arms read no flag at all");
	Check(true, "and everything from 22 up falls into the default with them");
}

// The refusal itself, which is one test on the object rather than on the
// cause - that being the point of the table above.
void TestALocalDeathOfSomebodyElsesPedestrianIsRefused() {
	std::printf("\nonly the host decides that its pedestrian is dead\n");

	using game::PlanReplicaSetDie;
	using game::SetDieVerdict;

	Check(PlanReplicaSetDie(/*isReplica=*/true, /*sessionAsked=*/false) ==
	          SetDieVerdict::RefuseAndHeal,
	      "our engine killing somebody else's ped is refused");

	// The one bug a refusal like this is guaranteed to introduce if nobody
	// writes it down: KillAmbientReplica carries out a death the host
	// reported by calling the engine's own CPed::SetDie, through the same
	// address and so through the same detour.
	Check(PlanReplicaSetDie(/*isReplica=*/true, /*sessionAsked=*/true) ==
	          SetDieVerdict::Run,
	      "but the session's own kill goes through");

	Check(PlanReplicaSetDie(/*isReplica=*/false, /*sessionAsked=*/false) ==
	          SetDieVerdict::Run,
	      "and a pedestrian we host dies normally - that death is ours to "
	      "announce");
	Check(PlanReplicaSetDie(/*isReplica=*/false, /*sessionAsked=*/true) ==
	          SetDieVerdict::Run,
	      "as does one nobody has a name for yet");
}

// CAutomobile::BlowUpCar is the one death in the image that can reach a
// seated replica without going anywhere near CPed::SetDie, so which of its
// two arms a ped takes decides whether the refusal can see it at all.
void TestBlowUpCarsTwoArmsOnItsOccupants() {
	std::printf("\nwhich arm of BlowUpCar an occupant takes\n");

	Check(game::BlowUpCarDestroysOccupant(game::PEDSTATE_DRIVING),
	      "a ped at the wheel goes through CPed::SetDead and is flagged for "
	      "destruction");
	Check(!game::BlowUpCarUsesSetDieOnOccupant(game::PEDSTATE_DRIVING),
	      "so no guard on CPed::SetDie can ever see it");

	Check(game::BlowUpCarUsesSetDieOnOccupant(game::PEDSTATE_IDLE) &&
	          game::BlowUpCarUsesSetDieOnOccupant(game::PEDSTATE_DIE),
	      "anything else takes the SetDie arm, which the refusal does see");
	Check(!game::BlowUpCarDestroysOccupant(game::PEDSTATE_IDLE),
	      "and is not flagged for destruction");
}

// The backstop, which is what covers the arm the refusal cannot see - and
// anything else nobody has found, because it tests the state and not the
// route.
void TestADeadReplicaIsOnlyRebuiltIfNobodyAskedForIt() {
	std::printf("\na corpse nobody asked for is not a corpse\n");

	Check(game::ReplicaDiedUnasked(game::PEDSTATE_DIE, /*sessionSaysDead=*/false),
	      "PED_DIE with nothing on the wire is our own engine's mistake");
	Check(game::ReplicaDiedUnasked(game::PEDSTATE_DEAD, false),
	      "and so is PED_DEAD");

	// Without this the recovery is a resurrection machine: every corpse the
	// host legitimately reported would be rebuilt on the frame after
	// KillAmbientReplica laid it down, and killed again on the one after
	// that, for as long as the body lay there.
	Check(!game::ReplicaDiedUnasked(game::PEDSTATE_DIE, true) &&
	          !game::ReplicaDiedUnasked(game::PEDSTATE_DEAD, true),
	      "a death the session reported stays down");

	Check(!game::ReplicaDiedUnasked(game::PEDSTATE_IDLE, false) &&
	          !game::ReplicaDiedUnasked(game::PEDSTATE_DRIVING, false),
	      "and a replica that is merely standing or driving is alive");
}

// The client loop's half, through the stub, which runs the real decision.
void TestAReplicaOurOwnEngineKilledIsRebuilt() {
	std::printf("\na replica our own engine killed is rebuilt from the host\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 530);
	c.Tick();
	Check(g_rec.ambientPedSpawns == 1, "one replica");

	// One frame of this machine's engine being wrong. The same shape as the
	// engine taking the replica away: noticed and rebuilt in the same frame,
	// because the row still knows the model, the owner and the pose.
	g_rec.ambientReplicaState = game::PEDSTATE_DEAD;
	c.Tick();
	g_rec.ambientReplicaState = game::PEDSTATE_NONE;

	Check(g_rec.ambientDeathsRecovered == 1, "noticed once");
	Check(g_rec.ambientPedDespawns == 1,
	      "the body is taken away rather than abandoned - CanBeDeleted would "
	      "refuse to collect it");
	Check(g_rec.ambientPedSpawns == 2, "and rebuilt in the same frame");
	Check(c.AmbientPed(530) != nullptr && c.AmbientPed(530)->poolHandle >= 0,
	      "the row names the new one");
	Check(!c.AmbientPed(530)->dead,
	      "and the session still says this pedestrian is alive, because he is");

	for (int i = 0; i < 10; ++i)
		c.Tick();
	Check(g_rec.ambientPedSpawns == 2, "it stays one ped");
	Check(g_rec.ambientKills == 0 && g_rec.ambientKillAttempts == 0,
	      "and nothing was killed - the host's copy never died, so the death "
	      "pass has nothing to carry out");
}

void TestACorpseTheSessionReportedIsNotResurrected() {
	std::printf("a corpse the host reported is left where it fell\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 531);
	c.Tick();
	c.HandleMessage(Wrap(MakePedDeath(531, /*animId=*/17), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientKills == 1, "the host's death is carried out");

	// And now the replica reads dead for the rest of the session, because it
	// is. The recovery must not touch it: rebuilding here would put a live
	// pedestrian back on his feet in a session that knows he is dead, and the
	// death pass would kill him again on the next frame, forever.
	const int spawnsBefore = g_rec.ambientPedSpawns;
	g_rec.ambientReplicaState = game::PEDSTATE_DEAD;
	for (int i = 0; i < 10; ++i)
		c.Tick();
	g_rec.ambientReplicaState = game::PEDSTATE_NONE;

	Check(g_rec.ambientDeathsRecovered == 0, "nothing is recovered");
	Check(g_rec.ambientPedSpawns == spawnsBefore, "nothing is rebuilt");
	Check(g_rec.ambientKills == 1, "and nothing is killed a second time");
}

// ---- the wanted level (docs/wanted.md) ------------------------------------
//
// Most of these go straight at game::PlanWanted rather than through Client,
// because the awkward part of this feature is not the plumbing - it is four
// lines of arithmetic with one state variable, and the cases that break it
// are ones no amount of playing reaches by accident. The `shared` deadlock in
// particular costs a whole live session to discover and about ten
// microseconds to check here.
//
// The engine is a uint8. That is not a simplification of CWanted, it is what
// CoopIII actually touches: one read and one call to the engine's own setter.

// Runs one tick of the decision over a tiny bit of state, the way Client
// does, so a test can write a sequence of ticks and read the outcome.
struct WantedSim {
	uint8_t rule    = WANTED_RULE_PERPLAYER;
	uint8_t engine  = 0;   // stands in for CWanted::m_nWantedLevel
	uint8_t own     = 0;
	uint8_t applied = 0;
	int     writes  = 0;
	uint8_t sent    = 0;
	bool    borrowed = false;

	void Tick(uint8_t floor) {
		const game::WantedPlan plan =
		    game::PlanWanted(rule, engine, applied, own, floor);
		if (plan.write) {
			++writes;
			engine = plan.target;
		}
		own      = plan.own;
		applied  = plan.applied;
		sent     = plan.target;
		borrowed = plan.borrowed;
	}
};

void TestWantedBitsSurviveTheWire() {
	std::printf("\nthe stars ride four spare bits and disturb nothing else\n");

	// Every other flag set, so a wanted level written over them would show.
	const uint8_t others = PF_AIMING | PF_FIRING | PF_ANIM2_RUNNING | PF_ON_FIRE;
	for (uint8_t level = 0; level <= 6; ++level) {
		const uint8_t f = FlagsWithWanted(others, level, false);
		if (WantedFromFlags(f) != level || (f & 0x0F) != others) {
			Check(false, "a level round-trips without touching the other bits");
			return;
		}
	}
	Check(true, "every level 0..6 round-trips without touching the other bits");

	Check((FlagsWithWanted(0, 4, true) & PF_WANTED_BORROWED) != 0,
	      "the borrowed bit is carried");
	Check(WantedFromFlags(FlagsWithWanted(0, 4, true)) == 4,
	      "and does not leak into the level");

	// Three bits hold 0..7 and the engine's ceiling is 6
	// (CWanted::MaximumWantedLevel). A seven off the wire is a corrupted or
	// hostile packet, and clamping is what stops it becoming a level written
	// back into somebody's engine.
	Check(WantedFromFlags(uint8_t(7 << PF_WANTED_SHIFT)) == WANTED_LEVEL_CEILING,
	      "seven stars is not a state GTA III has, so it reads as six");
	Check((FlagsWithWanted(0, 7, false) & PF_WANTED_BORROWED) == 0,
	      "and an over-range level cannot spill into the borrowed bit");

	// The session rule, the other direction.
	Check(WantedRuleFromFlags(FlagsWithWantedRule(SESSION_FRIENDLY_FIRE,
	                                              WANTED_RULE_SHARED)) ==
	          WANTED_RULE_SHARED,
	      "the rule round-trips beside friendly fire");
	Check((FlagsWithWantedRule(SESSION_FRIENDLY_FIRE, WANTED_RULE_OFF) &
	       SESSION_FRIENDLY_FIRE) != 0,
	      "and does not clear it");
	Check(WantedRuleFromFlags(uint8_t(3 << SESSION_WANTED_SHIFT)) ==
	          WANTED_RULE_PERPLAYER,
	      "a fourth rule that does not exist falls back to the default");
}

void TestWhoLendsUsStars() {
	std::printf("\nwhose stars count toward ours, and it is not the same "
	            "question in both rules\n");

	// perplayer: the car, and nothing else about them.
	Check(game::WantedPeerRaisesFloor(WANTED_RULE_PERPLAYER, 80, 80, false),
	      "someone in our car lends us theirs");
	Check(game::WantedPeerRaisesFloor(WANTED_RULE_PERPLAYER, 80, 80, true),
	      "even a level they borrowed - they are still wanted and their own "
	      "engine is still making police for them");
	Check(!game::WantedPeerRaisesFloor(WANTED_RULE_PERPLAYER, 80, 81, false),
	      "someone in a different car does not");
	Check(!game::WantedPeerRaisesFloor(WANTED_RULE_PERPLAYER, INVALID_NETID,
	                                   INVALID_NETID, false),
	      "and two players on foot are not in the same car, which is the one "
	      "case a bare equality would get wrong");

	// shared: did they earn it?
	Check(game::WantedPeerRaisesFloor(WANTED_RULE_SHARED, INVALID_NETID,
	                                  INVALID_NETID, false),
	      "in shared, anybody who earned theirs counts, car or no car");
	Check(!game::WantedPeerRaisesFloor(WANTED_RULE_SHARED, 80, 80, true),
	      "and a borrowed level never does - counting echoes is the deadlock");

	Check(!game::WantedPeerRaisesFloor(WANTED_RULE_OFF, 80, 80, false),
	      "off lends nothing");
}

void TestWantedIsInvisibleToALonePlayer() {
	std::printf("\nnobody in your car: CoopIII never touches your stars\n");
	WantedSim s;

	// The engine climbing on its own, a star at a time, the way a crime
	// spree goes.
	for (uint8_t level = 0; level <= 6; ++level) {
		s.engine = level;
		s.Tick(0);
		if (s.own != level || s.sent != level) {
			Check(false, "own follows the engine");
			return;
		}
	}
	Check(true, "own follows the engine all the way to six");
	Check(s.writes == 0,
	      "and not one write - CWanted::SetWantedLevel resets m_nChaos to the "
	      "bottom of the bracket, so writing the level a player already has "
	      "would stop them ever climbing");
	Check(!s.borrowed, "nothing is reported as borrowed");

	// And the other direction: a death, a bust, a bribe, a spray.
	s.engine = 0;
	s.Tick(0);
	Check(s.own == 0 && s.sent == 0 && s.writes == 0,
	      "a level the engine took away is gone, with no write either");
}

void TestABorrowedStarBecomesYours() {
	std::printf("\nriding with a wanted player: you get the heat and you "
	            "keep it\n");
	WantedSim s;

	s.Tick(4);
	Check(s.writes == 1 && s.engine == 4, "getting in gives us their four");
	Check(!s.borrowed,
	      "and in perplayer it is ours at once - nobody is going to take it "
	      "back, so there is nothing for the borrowed bit to say");

	// Repeated ticks in the same car must not keep writing: the engine is
	// already where we want it.
	s.Tick(4);
	s.Tick(4);
	Check(s.writes == 1, "and it is written once, not once per tick");

	// Out of the car. The floor goes, the stars do not.
	s.Tick(0);
	Check(s.engine == 4 && s.writes == 1,
	      "getting out does not take it away - a level that evaporated at the "
	      "door would leave you on the pavement in front of four police cars "
	      "with no stars");

	// And the owner's own engine is still the only thing that ends it.
	s.engine = 0;   // busted
	s.Tick(0);
	Check(s.own == 0 && s.sent == 0, "being busted ends it, as in single player");
}

void TestSharedComesBackDown() {
	std::printf("\nshared: the session's level comes back down (the deadlock "
	            "this bit exists for)\n");
	WantedSim a, b;
	a.rule = b.rule = WANTED_RULE_SHARED;

	// A earns four. Nobody else is wanted, so nothing is written and A is
	// reporting a level it reached by itself.
	a.engine = 4;
	a.Tick(0);
	Check(a.sent == 4 && !a.borrowed && a.writes == 0,
	      "A earned four and says so");

	// B takes the session's level.
	b.Tick(a.sent);
	Check(b.engine == 4 && b.writes == 1, "B is raised to four");
	Check(b.borrowed,
	      "and says it is the session's, which is the whole of the fix");

	// A is unaffected by B's echo, because an echo does not count.
	a.Tick(0);
	Check(a.sent == 4 && !a.borrowed, "A still holds its own four");

	// A dies.
	a.engine = 0;
	a.Tick(0);
	Check(a.sent == 0 && !a.borrowed, "A clears, and reports zero");

	// And B follows it down. Without PF_WANTED_BORROWED, B would still be
	// reporting four here, A would be raised straight back to four by it, and
	// neither would ever get out.
	b.Tick(0);
	Check(b.engine == 0 && b.writes == 2, "B comes down with it");
}

void TestSharedDoesNotUnearnAnybody() {
	std::printf("\nshared: two players who each earned three keep three\n");
	WantedSim a, b;
	a.rule = b.rule = WANTED_RULE_SHARED;
	a.engine = 3;
	b.engine = 3;

	for (int i = 0; i < 3; ++i) {
		a.Tick(b.borrowed ? 0 : b.sent);
		b.Tick(a.borrowed ? 0 : a.sent);
	}
	Check(a.writes == 0 && b.writes == 0,
	      "neither is written to - the floor is what each already holds");
	Check(!a.borrowed && !b.borrowed,
	      "and neither is marked borrowed, so neither can pull the other down");

	// A dies. In shared this is the mode working: B is still wanted at three
	// and the session shares the highest level, so A is straight back to it.
	a.engine = 0;
	a.Tick(b.borrowed ? 0 : b.sent);
	Check(a.engine == 3 && a.borrowed,
	      "A dies and is re-wanted by the session, now holding a borrowed "
	      "three rather than its own");
	b.Tick(a.borrowed ? 0 : a.sent);
	Check(b.engine == 3 && b.writes == 0, "and B, who earned its three, keeps it");
}

void TestWantedOffIsAClamp() {
	std::printf("\noff: the engine is put back to zero, and that is all it is\n");
	WantedSim s;
	s.rule = WANTED_RULE_OFF;

	s.engine = 3;
	s.Tick(0);
	Check(s.engine == 0 && s.writes == 1, "three stars are taken away");
	Check(s.sent == 0 && !s.borrowed, "and nothing is reported");

	s.Tick(6);
	Check(s.engine == 0 && s.writes == 1,
	      "and nobody can lend us any, however wanted they are");

	// The honest part: a crime still registers first and is undone after.
	s.engine = 1;
	s.Tick(0);
	Check(s.writes == 2,
	      "a crime committed between two ticks raises the level and is taken "
	      "back on the next one - off is a clamp, not a police-free city");
}

void TestOnlyTheCarYouAreInLendsYouStars() {
	std::printf("\nthrough Client: the car is the rule\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeWantedState(1, 1000, 5.0f, 4), CH_SNAPSHOT));
	// Alice gets into car 80. We are still on foot.
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(g_rec.wantedWrites == 0 && g_rec.engineWanted == 0,
	      "standing next to a wanted player does nothing - no source says "
	      "proximity spreads it and GTA III gives us nothing to hang it on");

	// We get into a different car.
	c.HandleMessage(Wrap(MakeEnter(0, 81), CH_EVENT));
	c.Tick();
	Check(g_rec.wantedWrites == 0, "nor does being in a different car");

	// And now the same one.
	c.HandleMessage(Wrap(MakeEnter(0, 80), CH_EVENT));
	c.Tick();
	Check(g_rec.wantedWrites == 1 && g_rec.engineWanted == 4,
	      "getting into her car gives us her four");

	c.Tick();
	c.Tick();
	Check(g_rec.wantedWrites == 1, "written once, not once a frame");

	// Out again. The stars stay.
	S_ExitVehicle exit;
	InitHeader(exit, 2000);
	exit.playerId = 0;
	exit.netId    = 80;
	c.HandleMessage(Wrap(exit, CH_EVENT));
	c.Tick();
	Check(g_rec.engineWanted == 4 && g_rec.wantedWrites == 1,
	      "and getting out does not take them back");
}

void TestABorrowedLevelDoesNotEchoInSharedMode() {
	std::printf("\nthrough Client: a borrowed level is not a reason to be "
	            "wanted\n");
	Client c;
	c.SetBridge(RecordingBridge());
	S_Welcome w = MakeWelcome(0);
	w.flags     = FlagsWithWantedRule(0, WANTED_RULE_SHARED);
	c.HandleMessage(Wrap(w, CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	// Alice is reporting four stars that she borrowed from somebody else.
	c.HandleMessage(Wrap(MakeWantedState(1, 1000, 5.0f, 4, /*borrowed=*/true),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.wantedWrites == 0,
	      "an echo of somebody else's level does not raise us");

	// The same four, earned.
	c.HandleMessage(Wrap(MakeWantedState(1, 1040, 6.0f, 4, /*borrowed=*/false),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.wantedWrites == 1 && g_rec.engineWanted == 4,
	      "a level she earned does, from anywhere in the city");

	// She loses it. We follow her down, because ours was only ever hers.
	c.HandleMessage(Wrap(MakeWantedState(1, 1080, 7.0f, 0), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.engineWanted == 0 && g_rec.wantedWrites == 2,
	      "and when hers goes, ours goes with it");
}

void TestNoPlayerPedMeansNoWantedDecision() {
	std::printf("\nno player ped: the whole thing is left alone\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeWantedState(1, 1000, 5.0f, 5), CH_SNAPSHOT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(0, 80), CH_EVENT));

	// Menus, a load, or the seconds between dying and waking at the hospital.
	g_rec.havePlayerWanted = false;
	for (int i = 0; i < 5; ++i)
		c.Tick();
	Check(g_rec.wantedWrites == 0, "nothing is written into a player who is not there");

	// And nothing was recorded either. Treating the absence as a zero would
	// have left `applied` at zero and `own` at zero, which is exactly what a
	// player who has just been busted also reads - so the session would hand
	// their own stars straight back to them as a borrowed level.
	g_rec.havePlayerWanted = true;
	g_rec.engineWanted     = 0;
	c.Tick();
	Check(g_rec.engineWanted == 5 && g_rec.wantedWrites == 1,
	      "and when the ped comes back, the car's level is applied then");
}

void TestTheSessionsWantedRuleArrives() {
	std::printf("\nthe server says which rule is in force\n");
	Client c;
	c.SetBridge(RecordingBridge());

	// Shared, and a player who is nowhere near us.
	S_Welcome w = MakeWelcome(0);
	w.flags     = FlagsWithWantedRule(0, WANTED_RULE_SHARED);
	c.HandleMessage(Wrap(w, CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeWantedState(1, 1000, 500.0f, 3), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.engineWanted == 3,
	      "in shared, a player on the other side of the city raises us");

	// Off, on a fresh session.
	Client d;
	d.SetBridge(RecordingBridge());
	g_rec.engineWanted = 2;
	S_Welcome off = MakeWelcome(0);
	off.flags     = FlagsWithWantedRule(0, WANTED_RULE_OFF);
	d.HandleMessage(Wrap(off, CH_EVENT));
	d.Tick();
	Check(g_rec.engineWanted == 0, "off takes our own stars away too");

	// And a server too old to set the bits leaves the default in place, which
	// is the per-player rule rather than "off".
	Client e;
	e.SetBridge(RecordingBridge());
	g_rec.engineWanted = 2;
	e.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	e.Tick();
	Check(g_rec.engineWanted == 2 && g_rec.wantedWrites == 0,
	      "a welcome with no rule bits means per player, not off");
}

void TestDisconnectClearsAmbientPeds() {
	std::printf("\na disconnect empties the seats before it drops the peds\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 515);
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(605), CH_EVENT));
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 515, 2000, 11.0f, 0, 605, 0),
	                     CH_SNAPSHOT));
	c.Tick();
	Check(c.AmbientPed(515)->Seated(), "seated");

	c.Stop();
	Check(g_rec.ambientUnseats == 1, "taken out of the car");
	Check(g_rec.ambientPedDespawns == 1, "the replica is gone");
	Check(g_rec.ambientCarDespawns == 1, "and so is the car");
	Check(c.AmbientPed(515) == nullptr, "and the roster is empty");
}

// ---- a claim that never comes back ----------------------------------------
//
// "No se mueve, o se mueve super lentísimo", from the driver's own screen.
//
// Getting into one of the session's cars sends the takeover claim once, and
// Client::m_vehicleClaimPending is what stops it being sent again. Until the
// reply lands, DrivenLocally is false - the session has not named the car ours
// yet - so UpdateRemoteVehicles used to go on writing the previous driver's
// snapshot onto the car we are sitting in, every frame. One round trip of that
// is nothing. A claim that is refused or dropped never ends, and then the car
// is held at whatever its last driver was doing, which for a car somebody
// parked is a standstill.
//
// The reply is deliberately never delivered here. That is the whole test.
void TestACarWeAreDrivingIsNotWrittenToBeforeTheClaimComesBack() {
	std::printf("\nthe claim that never came back\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsTheSessionsCar(c);

	// Somebody else's last snapshot, which is what would be replayed.
	c.HandleMessage(Wrap(MakeVehicleState(1, 80, 15.0f), CH_SNAPSHOT));

	// The claim goes out. No S_EnterVehicle ever answers it.
	c.TickLocalVehicle();
	Check(c.LocalVehicleNetId() == INVALID_NETID,
	      "the session has not named the car ours");

	const int applies     = g_rec.vehicleApplies;
	const int corrections = g_rec.vehicleCorrections;
	c.Tick();
	c.Tick();
	c.Tick();
	Check(g_rec.vehicleApplies == applies,
	      "nothing is written onto a car the engine says we are driving");
	Check(g_rec.vehicleCorrections == corrections,
	      "and it is not put back underneath us either");

	// Step out and it is an ordinary observed car again. Without this the fix
	// would read as "a car we ever touched is never written to again", which
	// is a different and much worse bug.
	g_rec.drivingLocally     = false;
	g_rec.localVehicleHandle = -1;
	c.Tick();
	Check(g_rec.vehicleApplies > applies, "once we are out, the session owns it again");
}

// ---- naming the death -----------------------------------------------------
//
// The pure half of "me subí a un auto y me morí". game/combat.h carries the
// disassembly; this pins the two conclusions that come out of it, so that a
// later edit cannot quietly widen either one.
void TestADeathCauseIsNamed() {
	std::printf("\nwhat killed us\n");

	Check(game::DeathCauseFor(false, 0, 0) == game::DeathCause::UNKNOWN,
	      "nothing damaged us, so nothing is claimed");
	Check(game::DeathCauseFor(true, game::WEAPONTYPE_DROWNING, 0) ==
	          game::DeathCause::DROWNED,
	      "WEAPONTYPE_DROWNING is a drowning");
	Check(game::DeathCauseFor(true, game::WEAPONTYPE_RUNOVERBYCAR, 0) ==
	          game::DeathCause::CRUSHED,
	      "RUNOVERBYCAR is a crush");
	Check(game::DeathCauseFor(true, game::WEAPONTYPE_RAMMEDBYCAR, 0) ==
	          game::DeathCause::CRUSHED,
	      "and so is RAMMEDBYCAR");
	Check(game::DeathCauseFor(true, game::WEAPONTYPE_COLT45, 0) ==
	          game::DeathCause::ORDINARY,
	      "a bullet is an ordinary cause");

	// Stale is not a cause. A player shot half a second before they walked
	// into the river did not die of the bullet.
	Check(game::DeathCauseFor(true, game::WEAPONTYPE_COLT45,
	                          game::DEATH_CAUSE_WINDOW_MS + 1) ==
	          game::DeathCause::UNKNOWN,
	      "a cause older than the window is not the reason");

	// The two sentences that matter are distinguishable, because the whole
	// point is that anim 173 alone is not.
	const char *inCar = game::DeathStory(game::ANIM_STD_NUM, /*inVehicle=*/true,
	                                     game::DeathCause::DROWNED);
	const char *crush = game::DeathStory(game::ANIM_STD_NUM, /*inVehicle=*/false,
	                                     game::DeathCause::CRUSHED);
	Check(std::strcmp(inCar, crush) != 0,
	      "the same animation in a car and on foot are told apart");

	// And an in-vehicle death that is not a drowning is called out rather
	// than explained away: retail 1.0 has no path to it.
	const char *impossible = game::DeathStory(game::ANIM_STD_NUM, true,
	                                          game::DeathCause::ORDINARY);
	Check(std::strcmp(impossible, inCar) != 0,
	      "an in-car death that is not drowning reads as a finding");
}

// ---------------------------------------------------------------------------
// A car nobody is driving (protocol.h, S_VehicleCustody)
// ---------------------------------------------------------------------------
//
// The bug these are about: a car left leaning against a wall is pinned in that
// pose for ever, on every machine, and can never be entered again. Nothing
// re-reports a driverless car, so whatever transform it had at the instant its
// driver left is written back onto it after physics, every frame, for the rest
// of the session. CVehicle::CanPedEnterCar (0x005522F0) then refuses it - and
// refuses it for a car on its *side*, up.z inside +-0.1, not outside it, which
// is worth pinning as a number because it reads backwards from how the symptom
// is described.
//
// What these check is the decision, because that is where it lives: one
// machine simulates, everybody else follows, and the moment nobody is settling
// it the behaviour goes back to being exactly what it was.

// The session's car 80, spawned, with nobody in it.
void ParkOneCar(Client &c, uint16_t netId = 80) {
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(netId), CH_EVENT));
	c.Tick();
}

S_VehicleCustody MakeCustody(uint16_t netId, uint8_t playerId) {
	S_VehicleCustody p;
	InitHeader(p, 1000);
	p.netId    = netId;
	p.playerId = playerId;
	p.pad      = 0;
	return p;
}

void TestAParkedCarWithNoCustodianIsStillPinnedAndRested() {
	std::printf("\na parked car nobody is settling: nothing changes at all\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);

	const int corrections = g_rec.vehicleCorrections;
	const int rests       = g_rec.vehicleAtRest;
	c.Tick();
	c.Tick();
	c.Tick();

	// This is the check that says the common case did not regress. A car
	// standing in the street for ten minutes is the thing pinning is right
	// for: it costs nothing, it cannot drift, and every machine agrees on it
	// to the bit. Custody is the exception and this is the rule, so the rule
	// has to be reachable without it.
	Check(g_rec.vehicleCorrections == corrections + 3,
	      "still put back after every single frame of physics");
	Check(g_rec.vehicleAtRest > rests,
	      "and still rested, so its last driver's velocity cannot pin it");
	Check(c.VehicleByNetId(80)->custodianPlayerId == INVALID_PLAYER,
	      "and nobody has been asked to simulate it");
}

void TestTheCarWeAreSettlingIsNeitherRestedNorPinned() {
	std::printf("\nthe machine settling a car stops correcting it\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);

	c.HandleMessage(Wrap(MakeCustody(80, /*us=*/0), CH_EVENT));
	Check(c.VehicleByNetId(80)->custodianPlayerId == 0, "the session has asked us");

	const int corrections = g_rec.vehicleCorrections;
	const int rests       = g_rec.vehicleAtRest;
	const int applies     = g_rec.vehicleApplies;
	c.Tick();
	c.Tick();
	c.Tick();

	// All three, and each one for its own reason. The correction is what
	// makes a pose permanent, so it has to stop or nothing else matters. The
	// rest write would hold the velocities at zero, so gravity could never
	// accumulate and the car would never fall. And the apply would write the
	// *previous driver's* controls back onto a car we are now simulating.
	Check(g_rec.vehicleCorrections == corrections,
	      "not corrected, so one frame of gravity finally gets to finish");
	Check(g_rec.vehicleAtRest == rests, "not rested, so the fall can start");
	Check(g_rec.vehicleApplies == applies,
	      "and not handed its last driver's controls either");
}

void TestSomebodyElseSettlingACarIsFollowedAndNotRested() {
	std::printf("\nwatching somebody else settle a car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeVehicleState(1, 80, 15.0f), CH_SNAPSHOT));

	c.HandleMessage(Wrap(MakeCustody(80, /*somebody else=*/1), CH_EVENT));

	const int corrections = g_rec.vehicleCorrections;
	const int rests       = g_rec.vehicleAtRest;
	const int applies     = g_rec.vehicleApplies;
	c.Tick();
	c.Tick();

	Check(g_rec.vehicleCorrections > corrections,
	      "an observer still follows it every frame");
	Check(g_rec.vehicleApplies > applies,
	      "and still animates it off what its custodian reports");
	// The one thing that changes for an observer. Resting zeroes the
	// velocities, and a tumbling car with zero velocity has still wheels, a
	// sleeping suspension and no engine note - it slides rather than falls.
	Check(g_rec.vehicleAtRest == rests,
	      "but does not rest it, because somebody really is moving it");
}

void TestASettleEndsOnARunOfQuietSamplesAndNotOnOne() {
	std::printf("\nwhen a settling car is called finished\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));

	g_rec.carAtRest = false;
	for (int i = 0; i < 20; ++i)
		c.TickCustody();
	Check(!c.VehicleByNetId(80)->settleReported,
	      "a car that is still moving is not handed back");
	Check(g_rec.observedSamples >= 20,
	      "and it is being reported on the session's behalf while it moves");

	// One still sample is not rest. A car at the top of a bounce reads still
	// for exactly one frame, and handing it back there leaves it pinned in
	// mid-air - the same bug, reached by being impatient about it.
	g_rec.carAtRest = true;
	c.TickCustody();
	g_rec.carAtRest = false;
	c.TickCustody();
	Check(c.VehicleByNetId(80)->restFrames == 0,
	      "one quiet sample followed by a moving one is not a run");
	Check(!c.VehicleByNetId(80)->settleReported, "so it is still ours");

	g_rec.carAtRest = true;
	for (int i = 0; i < VEHICLE_REST_FRAMES; ++i)
		c.TickCustody();
	Check(c.VehicleByNetId(80)->settleReported,
	      "a full run of quiet samples hands it back");

	// Once. The row keeps the custodian until the server's own answer comes
	// round, so without this it would say so on every tick in between.
	const int samples = g_rec.observedSamples;
	c.TickCustody();
	c.TickCustody();
	Check(g_rec.observedSamples == samples,
	      "and it stops reporting the car the moment it has said so");
}

void TestASettleThatNeverSettlesIsGivenUpOn() {
	std::printf("\na car that will not settle\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));

	// Bouncing on a kerb for ever, or a custodian that has walked out of the
	// streamer's range and is simulating something that is no longer really
	// there. Either way the window is bounded, and what it falls back to is
	// the behaviour that was there before custody existed.
	g_rec.carAtRest = false;
	RemoteVehicle *v = const_cast<RemoteVehicle *>(c.VehicleByNetId(80));
	v->settleEndsAtMs = 0;   // as if VEHICLE_SETTLE_MS had already elapsed
	c.TickCustody();
	Check(v->settleReported, "the window closes whether or not the car stopped");
}

void TestADriverEndsOurSettle() {
	std::printf("\nsomebody gets into a car we are settling\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeJoin(1, "bob"), CH_EVENT));
	c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
	Check(c.VehicleByNetId(80)->custodianPlayerId == 0, "we are settling it");

	S_EnterVehicle enter;
	InitHeader(enter, 1000);
	enter.playerId   = 1;
	enter.body       = EnterVehicleBody{};
	enter.body.netId = 80;
	enter.body.seat  = 0;
	c.HandleMessage(Wrap(enter, CH_EVENT));

	Check(c.VehicleByNetId(80)->custodianPlayerId == INVALID_PLAYER,
	      "and a driver ends that with no packet spent saying so");

	const int corrections = g_rec.vehicleCorrections;
	c.Tick();
	Check(g_rec.vehicleCorrections > corrections,
	      "so we are back to following the car its driver is reporting");
}

void TestACustodianLeavingReleasesTheCarHere() {
	std::printf("\nthe machine settling a car disconnects\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeJoin(1, "bob"), CH_EVENT));
	c.HandleMessage(Wrap(MakeCustody(80, 1), CH_EVENT));

	const int rests = g_rec.vehicleAtRest;
	c.Tick();
	Check(g_rec.vehicleAtRest == rests, "while bob has it, nobody rests it");

	S_PlayerLeave left;
	InitHeader(left, 1000);
	left.playerId = 1;
	left.reason   = 0;
	c.HandleMessage(Wrap(left, CH_EVENT));

	Check(c.VehicleByNetId(80)->custodianPlayerId == INVALID_PLAYER,
	      "an ownership does not outlive the player who held it");
	c.Tick();
	Check(g_rec.vehicleAtRest > rests,
	      "so the car goes back to being rested and pinned like any parked one");
}

// ---------------------------------------------------------------------------
// A traffic car that has stopped being traffic (protocol.h, S_CarPromoted)
// ---------------------------------------------------------------------------

S_CarPromoted MakeCarPromoted(uint16_t netId, uint8_t driver, uint8_t wasOwner) {
	S_CarPromoted p;
	InitHeader(p, 1000);
	p.netId            = netId;
	p.driverPlayerId   = driver;
	p.wasOwnerPlayerId = wasOwner;
	p.body             = MakeAmbientCarSpawn(netId, wasOwner).body;
	return p;
}

void TestTakingTheWheelOfSomebodyElsesTrafficClaimsIt() {
	std::printf("\ngetting into somebody else's traffic\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(300, /*owner=*/1), CH_EVENT));
	c.Tick();
	const RemoteAmbientCar *car = c.AmbientCar(300);
	Check(car != nullptr && car->poolHandle >= 0, "we have a replica of it");

	c.TickAmbientClaims();
	Check(!c.AmbientCar(300)->claimPending, "standing next to it claims nothing");

	// Now at its wheel. Up to here the roster said nothing at all: the
	// correction pass quietly stepped aside and the session went on telling
	// everybody else where its old owner thought the car was.
	g_rec.ambientDrivenHandle = car->poolHandle;
	c.TickAmbientClaims();
	Check(c.AmbientCar(300)->claimPending, "taking the wheel asks for it");

	// Once, not once a tick. The claim is reliable, so it gets there.
	c.TickAmbientClaims();
	c.TickAmbientClaims();
	Check(c.AmbientCar(300)->claimPending, "and only asks once");

	// Out again before the answer came back, so a later entry asks afresh.
	g_rec.ambientDrivenHandle = -1;
	c.TickAmbientClaims();
	Check(!c.AmbientCar(300)->claimPending, "getting out drops the claim");
}

void TestATrafficCarIsNotAlsoClaimedAsABrandNewOne() {
	std::printf("\nthe traffic car that must not be registered twice\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(300, /*owner=*/1), CH_EVENT));
	c.Tick();
	const int32_t handle = c.AmbientCar(300)->poolHandle;

	// At the wheel of it, and the ordinary vehicle path sees exactly what it
	// sees for any car: the local player is driving something the session has
	// not named. ObservedVehicleWeAreDriving looks in the *vehicle* roster and
	// this car is not in it, so without the guard the claim falls through to
	// "introduce an unknown car" and the session allocates a second netId for
	// a CVehicle it already has - which is roadmap.md §5.8.1 case 2 reached
	// from the other roster, and every observer then holds two cars, one
	// following the driver and one frozen.
	g_rec.drivingLocally      = true;
	g_rec.localVehicleHandle  = handle;
	g_rec.ambientDrivenHandle = handle;

	c.TickLocalVehicle();
	Check(!c.VehicleClaimPending(),
	      "the vehicle path leaves it alone, because it is not an unknown car");
	Check(c.LocalVehicleNetId() == INVALID_NETID, "and names nothing yet");

	c.TickAmbientClaims();
	Check(c.AmbientCar(300)->claimPending,
	      "the promotion path asks for it instead, under the number the "
	      "session already has");
}

void TestAPromotedTrafficCarKeepsItsObjectAndItsNumber() {
	std::printf("\na traffic car becomes a session car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(300, /*owner=*/1), CH_EVENT));
	c.Tick();
	const int32_t handle   = c.AmbientCar(300)->poolHandle;
	const int     despawns = g_rec.ambientCarDespawns;
	const int     spawns   = g_rec.vehicleSpawns;

	c.HandleMessage(Wrap(MakeCarPromoted(300, /*driver=*/2, /*wasOwner=*/1), CH_EVENT));
	c.Tick();

	Check(c.AmbientCar(300) == nullptr, "the traffic row is gone");
	const RemoteVehicle *v = c.VehicleByNetId(300);
	Check(v != nullptr, "and a vehicle row has taken it over");
	// The whole point: one CVehicle, one netId, nothing built and nothing
	// destroyed. A promotion that despawned and respawned would flicker the
	// car on every screen, and on the machine that made it would delete one
	// of the player's own traffic cars.
	Check(v != nullptr && v->netId == 300, "under the same netId");
	Check(v != nullptr && v->poolHandle == handle, "holding the same object");
	Check(g_rec.ambientCarDespawns == despawns, "nothing was destroyed");
	Check(g_rec.vehicleSpawns == spawns, "and nothing was built");
	Check(g_rec.promotedAdoptions == 1, "the engine seam was told once");
	Check(!g_rec.lastPromotionWasOurs,
	      "and told that this is a replica, not a car our own engine made");
	Check(v != nullptr && !v->ours,
	      "so the roster may destroy it later, as it may any replica");
}

void TestTheMachineThatMadeTheCarNeverDestroysIt() {
	std::printf("\nthe machine whose engine made the promoted car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	// We are player 1, and the car about to be promoted was ours as traffic.
	c.HandleMessage(Wrap(MakeWelcome(1), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(300, /*owner=*/2), CH_EVENT));
	c.Tick();
	const int32_t handle = c.AmbientCar(300)->poolHandle;

	c.HandleMessage(Wrap(MakeCarPromoted(300, /*driver=*/2, /*wasOwner=*/1), CH_EVENT));
	const RemoteVehicle *v = c.VehicleByNetId(300);
	Check(v != nullptr && v->poolHandle == handle, "we keep the car we had");
	Check(g_rec.lastPromotionWasOurs,
	      "and the seam is told this one is our own engine's work");
	Check(v != nullptr && v->ours,
	      "which is what keeps the deleting destructor away from it - a car "
	      "this engine made is not CoopIII's to destroy, possibly with a "
	      "player in it, because a socket closed");

	const int despawns = g_rec.vehicleDespawns;
	S_VehicleDespawn gone;
	InitHeader(gone, 1000);
	gone.netId = 300;
	c.HandleMessage(Wrap(gone, CH_EVENT));
	Check(g_rec.vehicleDespawns == despawns,
	      "so even the session dropping it does not delete it here");
}

// ---------------------------------------------------------------------------
// The car we claimed, as the two vehicle detours see it
// ---------------------------------------------------------------------------
//
// CVehicle::InflictDamage and BlowUpCar are detoured in game/vehicle.cpp and
// decide from four things: is the local player at the wheel, does the
// Observed table have a row naming a driver, or a custodian, and is it a
// traffic replica. This is that decision, over the real table, with the local
// player's own car as the subject. The expressions for "driven" and
// "settling" are copied from the two detours, and have to stay the same as
// theirs.
struct DetourView {
	game::CarOwner         owner;
	game::CarDamageVerdict shot;       // our player's M16 round
	bool                   mayBlowUp;
	bool                   wreckNamed; // a blast here goes out as UNOWNED_SESSION
};

DetourView WhatTheDetoursSee(int32_t handle, bool weDrive) {
	const game::ObservedRow *const o =
	    weDrive ? nullptr : g_observedHere.Find(FakeVehicleAt(handle), &FakeVehicleAt);
	const bool driven   = o != nullptr && o->driverPlayerId != 0xFF;
	const bool settling = o != nullptr && !driven && o->custodianPlayerId != 0xFF;
	const bool unheld   = o != nullptr && !driven && !settling && !o->weSettle;
	DetourView view{};
	view.owner      = game::ClassifyCar(false, weDrive, driven, settling, false, unheld);
	view.shot       = game::DecideCarDamage(view.owner, true, false, WEAPONTYPE_M16);
	view.mayBlowUp  = game::MayBlowUpCar(view.owner);
	view.wreckNamed = !weDrive && view.mayBlowUp && o != nullptr;
	return view;
}

// Claims one of our own engine's cars as net 364, as player 0, with alice
// (player 1) in the session. The handle is 4242 and resolves in the fake pool.
void ClaimOurOwnCar(Client &c) {
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	g_rec.modelReady         = true;
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = 4242;
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(0, 364), CH_EVENT));
}

void TestTheObservedTable() {
	std::printf("\nthe table the vehicle detours read\n");
	game::ObservedTable<4> t;
	g_rec = Recorder{};

	Check(t.Remember(10, 80, &FakeVehicleAt), "a row goes in");
	Check(t.Find(FakeVehicleAt(10), &FakeVehicleAt) != nullptr &&
	          t.Find(FakeVehicleAt(10), &FakeVehicleAt)->netId == 80,
	      "and is found by the car, not by the number");
	Check(t.Find(FakeVehicleAt(10), &FakeVehicleAt)->driverPlayerId == 0xFF,
	      "with nobody at the wheel until the roster says otherwise");

	t.NoteHolders(80, 2, 0xFF, false);
	Check(t.Find(FakeVehicleAt(10), &FakeVehicleAt)->driverPlayerId == 2,
	      "the driver is written onto the row");
	t.NoteHolders(80, 0xFF, 3, false);
	Check(t.Find(FakeVehicleAt(10), &FakeVehicleAt)->driverPlayerId == 0xFF &&
	          t.Find(FakeVehicleAt(10), &FakeVehicleAt)->custodianPlayerId == 3,
	      "and a custodian, once nobody drives it");
	t.NoteHolders(80, 2, 3, false);
	Check(t.Find(FakeVehicleAt(10), &FakeVehicleAt)->custodianPlayerId == 0xFF,
	      "but never alongside a driver");
	t.NoteHolders(80, 0xFF, 0xFF, true);
	Check(t.Find(FakeVehicleAt(10), &FakeVehicleAt)->weSettle,
	      "our own custody is written onto the row");
	t.NoteHolders(80, 2, 0xFF, true);
	Check(!t.Find(FakeVehicleAt(10), &FakeVehicleAt)->weSettle,
	      "but not beside a driver - a driver ends a custody");
	t.NoteHolders(80, 0xFF, 3, true);
	Check(!t.Find(FakeVehicleAt(10), &FakeVehicleAt)->weSettle,
	      "nor beside somebody else's");
	t.NoteHolders(80, 0xFF, 0xFF, false);
	Check(!t.Find(FakeVehicleAt(10), &FakeVehicleAt)->weSettle,
	      "and it goes when the roster stops saying it");

	// Rebuilt under the same netId after the engine reaped it: one row, the
	// new car's, and the old handle matches nothing.
	g_rec.reapedVehicleHandles.push_back(10);
	Check(t.Find(FakeVehicleAt(10), &FakeVehicleAt) == nullptr,
	      "a reaped car matches nothing");
	t.Remember(11, 80, &FakeVehicleAt);
	Check(t.Count() == 1, "a rebuilt car replaces its old row rather than adding one");

	// Four slots; one taken. A reaped row is free again even if nobody said
	// ForgetObserved for it.
	t.Remember(20, 81, &FakeVehicleAt);
	t.Remember(21, 82, &FakeVehicleAt);
	t.Remember(22, 83, &FakeVehicleAt);
	Check(!t.Remember(23, 84, &FakeVehicleAt), "a full table says so");
	g_rec.reapedVehicleHandles.push_back(21);
	Check(t.Remember(23, 84, &FakeVehicleAt),
	      "and a row whose car the engine has taken is reused");

	t.Forget(84);
	Check(t.Find(FakeVehicleAt(23), &FakeVehicleAt) == nullptr, "a forgotten row is gone");
	g_rec = Recorder{};
}

void TestOurClaimedCarIsInTheDetoursTable() {
	std::printf("\nthe car we claimed, through its three owners\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ClaimOurOwnCar(c);
	const RemoteVehicle *v = c.VehicleByNetId(364);
	Check(v != nullptr && v->ours && v->poolHandle == 4242, "our own car, claimed as 364");
	Check(g_rec.claimedAdoptions == 1, "the engine seam was told about it");
	Check(g_observedHere.Count() == 1, "and the detours' table has a row for it");

	// (a) We are driving it. The table is not even asked: our engine damages
	// and blows up our own car exactly as single player does.
	DetourView d = WhatTheDetoursSee(4242, /*weDrive=*/true);
	Check(d.owner == game::CarOwner::Local && d.mayBlowUp &&
	          d.shot == game::CarDamageVerdict::Apply,
	      "(a) at our wheel it is ours to dent and to blow up");

	// (b) Out of it, nobody driving. Still this engine's to damage, and now a
	// wreck here has a name the session knows.
	g_rec.drivingLocally = false;
	c.TickLocalVehicle();
	c.Tick();
	d = WhatTheDetoursSee(4242, false);
	Check(d.owner == game::CarOwner::Nobody && d.shot == game::CarDamageVerdict::Forward,
	      "(b) parked and nobody holding it, our round goes to the session, which "
	      "makes us its custodian");
	Check(d.mayBlowUp, "(b) and a blast that kills it still wrecks it here");
	Check(d.wreckNamed,
	      "(b) and if it blows up here it goes out as the session's car 364, "
	      "not as nobody's");

	// Custody: we were the last driver, so we settle it. Same answer.
	c.HandleMessage(Wrap(MakeCustody(364, 0), CH_EVENT));
	c.Tick();
	d = WhatTheDetoursSee(4242, false);
	Check(d.owner == game::CarOwner::Local, "(b) while we settle it, likewise");

	// (c) Alice gets in and drives off. The first frame her state reaches us,
	// the row names her.
	c.HandleMessage(Wrap(MakeEnter(1, 364), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleState(1, 364, 15.0f), CH_SNAPSHOT));
	c.Tick();
	d = WhatTheDetoursSee(4242, false);
	Check(d.owner == game::CarOwner::RemoteDriver,
	      "(c) with alice at the wheel it is hers");
	Check(d.shot == game::CarDamageVerdict::Forward,
	      "(c) our shot is refused here and sent to her");
	Check(!d.mayBlowUp, "(c) and our engine may not blow it up");

	// She parks it. Ours to damage again.
	c.HandleMessage(Wrap(MakeExit(1, 364), CH_EVENT));
	c.Tick();
	d = WhatTheDetoursSee(4242, false);
	Check(d.owner == game::CarOwner::Nobody && d.wreckNamed,
	      "once she gets out it is nobody's again");

	// And we take it back: one row still, and ours at the wheel.
	g_rec.drivingLocally = true;
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(0, 364), CH_EVENT));
	Check(g_observedHere.Count() == 1, "getting back in does not add a second row");
	d = WhatTheDetoursSee(4242, true);
	Check(d.owner == game::CarOwner::Local && d.mayBlowUp,
	      "and at our wheel it is ours again");
}

void TestOurClaimedCarJackedOffUsIsRefusedHere() {
	std::printf("\nthe car we claimed, jacked off us\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ClaimOurOwnCar(c);

	// Alice jacks it. Until the handover runs our engine still has us at the
	// wheel, and for that frame it is still ours to this engine - the same
	// rule every session car has had since protocol 22.
	c.HandleMessage(Wrap(MakeEnter(1, 364), CH_EVENT));
	Check(c.VehicleByNetId(364)->surrendered, "the row says we lost it");
	Check(WhatTheDetoursSee(4242, true).owner == game::CarOwner::Local,
	      "before the handover our engine still has us at the wheel");

	c.HandleMessage(Wrap(MakeVehicleState(1, 364, 15.0f), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.vehicleSurrenders == 1, "the seat is handed over");
	c.Tick();
	const DetourView d = WhatTheDetoursSee(4242, false);
	Check(d.owner == game::CarOwner::RemoteDriver && !d.mayBlowUp &&
	          d.shot == game::CarDamageVerdict::Forward,
	      "after it, shooting the car she took off us goes to her and does not "
	      "wreck our copy");
}

void TestACarWeTookBackIsNotStillHersInTheTable() {
	std::printf("\njacking our car back and settling it\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ClaimOurOwnCar(c);
	g_rec.drivingLocally = false;
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(1, 364), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleState(1, 364, 15.0f), CH_SNAPSHOT));
	c.Tick();
	Check(WhatTheDetoursSee(4242, false).owner == game::CarOwner::RemoteDriver,
	      "alice has our car");

	// We take it off her. The correction pass skips a car we drive or settle,
	// so it can't be what clears her name off the row - the claim reply and
	// Client::NoteVehicleHolders are.
	g_rec.drivingLocally = true;
	c.Tick();
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(0, 364), CH_EVENT));
	g_rec.drivingLocally = false;
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeCustody(364, 0), CH_EVENT));
	c.Tick();
	c.Tick();
	const DetourView d = WhatTheDetoursSee(4242, false);
	Check(d.owner == game::CarOwner::Local && d.mayBlowUp,
	      "settling it after we got out, it is ours and not still hers");
}

// ---------------------------------------------------------------------------
// A car somebody is settling (S_VehicleCustody), as the two detours see it
// ---------------------------------------------------------------------------
//
// The custodian's engine is the only one simulating the car until it hands it
// back, so it owns the car's condition the way a driver does. Everybody else
// refuses damage and blow-up on it and forwards their own hits.

// We are player 0 and watch car 80. Alice (1) drives it and gets out, and the
// session names her its custodian. Bob (2) is there too.
int32_t WatchAliceSettleCar80(Client &c) {
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(2, "bob"), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleState(1, 80, 15.0f), CH_SNAPSHOT));
	c.Tick();
	c.HandleMessage(Wrap(MakeExit(1, 80), CH_EVENT));
	c.HandleMessage(Wrap(MakeCustody(80, 1), CH_EVENT));
	c.Tick();
	return c.VehicleByNetId(80)->poolHandle;
}

void TestAHitOnACarSomebodyElseIsSettlingIsTheirs() {
	std::printf("\na car somebody else is settling is theirs to damage\n");
	Client c;
	c.SetBridge(RecordingBridge());
	const int32_t handle = WatchAliceSettleCar80(c);
	Check(handle >= 0, "car 80 is here");

	const DetourView d = WhatTheDetoursSee(handle, false);
	Check(d.owner == game::CarOwner::RemoteCustodian,
	      "alice is settling it, so it is hers");
	Check(d.shot == game::CarDamageVerdict::Forward,
	      "our shot is refused here and sent to her, like a shot at a car she drives");
	Check(!d.mayBlowUp, "and our engine may not blow it up");
	Check(VehicleHitIsWorthSending(*c.VehicleByNetId(80), INVALID_NETID),
	      "the hit is worth a packet - the server has somebody to give it to");

	c.Tick();
	c.Tick();
	Check(WhatTheDetoursSee(handle, false).owner == game::CarOwner::RemoteCustodian,
	      "and it stays hers from frame to frame, correction pass or not");

	// Her wreck comes back as UNOWNED_SESSION and is replayed here once.
	const int blasts = g_rec.blowUps;
	c.HandleMessage(Wrap(MakeUnownedBlowUp(80, UNOWNED_SESSION, 1), CH_EVENT));
	c.Tick();
	Check(g_rec.blowUps == blasts + 1 && g_rec.lastBlowUpNetId == 80,
	      "her wreck is replayed here, once");
	c.HandleMessage(Wrap(MakeUnownedBlowUp(80, UNOWNED_SESSION, 1), CH_EVENT));
	c.Tick();
	Check(g_rec.blowUps == blasts + 1, "and a second report of it replays nothing");
}

void TestASettleEndingHandsTheCarBack() {
	std::printf("\nwhen somebody else's settle ends, the car is nobody's again\n");

	// She says it has stopped and the session hands it back.
	{
		Client c;
		c.SetBridge(RecordingBridge());
		const int32_t handle = WatchAliceSettleCar80(c);
		c.HandleMessage(Wrap(MakeCustody(80, INVALID_PLAYER), CH_EVENT));
		c.Tick();
		const DetourView d = WhatTheDetoursSee(handle, false);
		Check(d.owner == game::CarOwner::Nobody && d.mayBlowUp && d.wreckNamed,
		      "settled: nobody's again, and a wreck here is UNOWNED_SESSION");
		Check(VehicleHitIsWorthSending(*c.VehicleByNetId(80), INVALID_NETID),
		      "and a hit we queued before it ended goes out - the server gives "
		      "us the car to settle");
	}

	// She quits halfway through. Handed to nobody, not to the next player.
	{
		Client c;
		c.SetBridge(RecordingBridge());
		const int32_t handle = WatchAliceSettleCar80(c);
		S_PlayerLeave leave;
		InitHeader(leave, 2000);
		leave.playerId = 1;
		leave.reason   = LEAVE_QUIT;
		c.HandleMessage(Wrap(leave, CH_EVENT));
		c.Tick();
		Check(WhatTheDetoursSee(handle, false).owner == game::CarOwner::Nobody,
		      "custodian gone: nobody's, not the next player's");
	}

	// Bob gets in before it has stopped. A driver ends a custody.
	{
		Client c;
		c.SetBridge(RecordingBridge());
		const int32_t handle = WatchAliceSettleCar80(c);
		c.HandleMessage(Wrap(MakeEnter(2, 80), CH_EVENT));
		c.Tick();
		const DetourView d = WhatTheDetoursSee(handle, false);
		Check(d.owner == game::CarOwner::RemoteDriver && !d.mayBlowUp,
		      "bob at the wheel: it is his now");
		const game::ObservedRow *o =
		    g_observedHere.Find(FakeVehicleAt(handle), &FakeVehicleAt);
		Check(o && o->driverPlayerId == 2 && o->custodianPlayerId == 0xFF,
		      "and the row names bob and nobody else");
	}
}

void TestOurOwnSettleIsOursToDamage() {
	std::printf("\nthe car we are settling is ours to damage, and to be shot in\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeCustody(80, /*us=*/0), CH_EVENT));
	c.Tick();
	const int32_t handle = c.VehicleByNetId(80)->poolHandle;

	const DetourView d = WhatTheDetoursSee(handle, false);
	Check(d.owner == game::CarOwner::Local && d.mayBlowUp &&
	          d.shot == game::CarDamageVerdict::Apply,
	      "our engine dents it and may blow it up");
	Check(d.wreckNamed, "and a wreck here goes out as UNOWNED_SESSION");

	// Her replayed round is not. Everything she hits it with comes to us as
	// C_VehicleHit, so a replay of the same round must not take health too.
	const game::ObservedRow *row =
	    g_observedHere.Find(FakeVehicleAt(handle), &FakeVehicleAt);
	Check(row && row->weSettle, "the detours' row knows the custody is ours");
	Check(game::ClassifyReplayCar(d.owner, false, row && row->weSettle, false, false) ==
	              game::ReplayTarget::CarWeSettle &&
	          !game::ReplayedShotMayDamage(game::ReplayTarget::CarWeSettle,
	                                       WEAPONTYPE_M16),
	      "so a replayed round leaves its health to her forwarded hit");

	// Alice shoots it. The server sends her hit to us.
	c.HandleMessage(Wrap(MakeVehicleHit(1, 80, /*weapon=*/5, /*amount=*/30.0f),
	                     CH_EVENT));
	Check(g_rec.vehicleHits == 1 && g_rec.lastVehicleHitRow == 80,
	      "her hit reaches the engine seam");
	Check(g_rec.lastVehicleHitSettling,
	      "marked as a custody hit, so the engine end checks for an empty seat");
	Check(g_rec.vehicleHitAttacker == 1, "credited to her");

	// Not handed back the moment it stops: the rest of her burst is coming.
	g_rec.carAtRest = true;
	for (int i = 0; i < VEHICLE_REST_FRAMES; ++i)
		c.TickCustody();
	Check(!c.VehicleByNetId(80)->settleReported,
	      "a car shot a moment ago is kept, at rest or not");

	// We tell the session it has stopped. The server may still route one
	// more hit to us before it has read that.
	const_cast<RemoteVehicle *>(c.VehicleByNetId(80))->holdUntilMs = 0;
	c.TickCustody();
	Check(c.VehicleByNetId(80)->settleReported, "we have said we are finished");
	c.HandleMessage(Wrap(MakeVehicleHit(1, 80), CH_EVENT));
	Check(g_rec.vehicleHits == 1,
	      "a hit after that is dropped - we don't report the car any more");
	c.Tick();
	row = g_observedHere.Find(FakeVehicleAt(handle), &FakeVehicleAt);
	Check(row && !row->weSettle,
	      "and from then on a replayed round is the only way her shot reaches it");

	c.HandleMessage(Wrap(MakeCustody(80, INVALID_PLAYER), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleHit(1, 80), CH_EVENT));
	Check(g_rec.vehicleHits == 1, "and so is one after the session took it back");
}

void TestAHitThatLandsAfterOurSettleEnded() {
	std::printf("\na hit for our settle that lands after it ended\n");

	// Alice got in before it arrived. It's her car and her engine's hit.
	{
		Client c;
		c.SetBridge(RecordingBridge());
		ParkOneCar(c);
		c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
		c.HandleMessage(Wrap(MakeJoin(2, "bob"), CH_EVENT));
		c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
		c.Tick();
		c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
		Check(c.VehicleByNetId(80)->custodianPlayerId == INVALID_PLAYER,
		      "her getting in ended our custody");
		c.HandleMessage(Wrap(MakeVehicleHit(2, 80), CH_EVENT));
		Check(g_rec.vehicleHits == 0, "bob's late hit is not ours to apply");
	}

	// A wreck takes nothing, custody or not.
	{
		Client c;
		c.SetBridge(RecordingBridge());
		ParkOneCar(c);
		c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
		c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
		c.HandleMessage(Wrap(MakeUnownedBlowUp(80, UNOWNED_SESSION, 1), CH_EVENT));
		c.Tick();
		c.HandleMessage(Wrap(MakeVehicleHit(1, 80), CH_EVENT));
		Check(g_rec.vehicleHits == 0, "the rest of a burst into a wreck is dropped");
	}
}

// ---- a session car nobody holds, and who owns its fire ----------------------
//
// Every machine writes the session's last health onto a parked session car
// every frame, so a hit taken locally lasted one frame and the car never
// caught fire. A hit on one now makes the shooter its custodian, and a
// custodian keeps a burning car until it goes up.

void TestACarNobodyHoldsIsShotThroughTheSession() {
	std::printf("\na session car nobody holds, as the detours see it\n");
	using game::CarDamageVerdict;
	using game::CarOwner;
	using game::ClassifyCar;
	using game::DecideCarDamage;

	Check(ClassifyCar(false, false, false, false, false, true) == CarOwner::Nobody,
	      "a row naming nobody is Nobody");
	Check(ClassifyCar(false, true, false, false, false, true) == CarOwner::Local &&
	          ClassifyCar(true, false, false, false, false, true) == CarOwner::Local &&
	          ClassifyCar(false, false, true, false, false, true) == CarOwner::RemoteDriver &&
	          ClassifyCar(false, false, false, true, false, true) == CarOwner::RemoteCustodian,
	      "and anybody actually holding it outranks that");

	Check(DecideCarDamage(CarOwner::Nobody, true, false, WEAPONTYPE_M16) ==
	          CarDamageVerdict::Forward,
	      "our round goes to the session");
	Check(DecideCarDamage(CarOwner::Nobody, false, false, WEAPONTYPE_M16) ==
	          CarDamageVerdict::Refuse,
	      "a replay of somebody else's takes nothing - theirs went out too");
	Check(DecideCarDamage(CarOwner::Nobody, true, true, WEAPONTYPE_M16) ==
	          CarDamageVerdict::Refuse,
	      "nor does anything fired into a wreck");
	Check(DecideCarDamage(CarOwner::Nobody, false, false, WEAPONTYPE_EXPLOSION) ==
	              CarDamageVerdict::Apply &&
	          DecideCarDamage(CarOwner::Nobody, true, false, WEAPONTYPE_GRENADE) ==
	              CarDamageVerdict::Apply,
	      "a blast still lands on every copy, as it always did");
	Check(DecideCarDamage(CarOwner::Nobody, true, false, WEAPONTYPE_FLAMETHROWER) ==
	              CarDamageVerdict::Refuse &&
	          game::FlameGoesToOwner(CarOwner::Nobody),
	      "a fire takes nothing here, and our flame goes to the session instead");
	Check(game::MayBlowUpCar(CarOwner::Nobody),
	      "and it may still go up here - a blast that kills it kills it everywhere");
}

void TestWhatBurningMeansToACustodian() {
	std::printf("\nwhen the car we settle is on fire\n");
	using game::VehicleOnFire;
	Check(VehicleOnFire(game::VEHICLE_TYPE_CAR, 249.0f, false, false) &&
	          !VehicleOnFire(game::VEHICLE_TYPE_CAR, 250.0f, false, false),
	      "a car under 250, the fire block's own test (0x00534510)");
	Check(VehicleOnFire(game::VEHICLE_TYPE_BOAT, 149.0f, false, false) &&
	          !VehicleOnFire(game::VEHICLE_TYPE_BOAT, 200.0f, false, false),
	      "a boat under 150 (0x0053F917)");
	Check(VehicleOnFire(game::VEHICLE_TYPE_CAR, 1000.0f, false, true),
	      "or a CFire on it, before the health has got there");
	Check(!VehicleOnFire(game::VEHICLE_TYPE_CAR, 0.0f, true, true),
	      "and never a wreck");
	Check(!VehicleOnFire(-1, 0.0f, false, false), "nor something that is neither");
}

void TestOurHitOnACarNobodyHoldsComesBackToUs() {
	std::printf("\nour hit on a car nobody holds\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.Tick();
	const int32_t handle = c.VehicleByNetId(80)->poolHandle;

	DetourView d = WhatTheDetoursSee(handle, false);
	Check(d.owner == game::CarOwner::Nobody && d.shot == game::CarDamageVerdict::Forward,
	      "nobody holds car 80, so our round is refused here and sent");
	Check(VehicleHitIsWorthSending(*c.VehicleByNetId(80), INVALID_NETID),
	      "and it is worth the packet");

	// The server makes us the custodian and sends our own hit back behind it.
	c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleHit(/*attackerId=*/0, 80, 5, 30.0f), CH_EVENT));
	Check(g_rec.vehicleHits == 1 && g_rec.lastVehicleHitRow == 80 &&
	          g_rec.lastVehicleHitSettling,
	      "it lands in the car we are now settling");
	Check(g_rec.vehicleHitAttacker == 0xFF, "credited to no remote player - it was us");
	Check(c.VehicleByNetId(80)->holdUntilMs != 0,
	      "and the custody is held for the rest of the burst");

	c.Tick();
	d = WhatTheDetoursSee(handle, false);
	Check(d.owner == game::CarOwner::Local && d.shot == game::CarDamageVerdict::Apply &&
	          d.mayBlowUp,
	      "from then on our engine takes the hits and decides the wreck");

	// Alice is the one who shot it, on another screen: then it's hers.
	Client o;
	o.SetBridge(RecordingBridge());
	ParkOneCar(o);
	o.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	o.HandleMessage(Wrap(MakeCustody(80, 1), CH_EVENT));
	o.Tick();
	d = WhatTheDetoursSee(o.VehicleByNetId(80)->poolHandle, false);
	Check(d.owner == game::CarOwner::RemoteCustodian && !d.mayBlowUp,
	      "a car alice shot is hers to burn, and ours never blows it up by itself");
}

void TestABurningCarIsKeptUntilItGoesUp() {
	std::printf("\na custodian keeps a burning car\n");
	Check(CustodyMayEnd(true, false, 5000, 9000, 0, 0),
	      "stopped and not burning: handed back");
	Check(CustodyMayEnd(false, false, 9000, 9000, 0, 0), "or out of time");
	Check(!CustodyMayEnd(true, false, 5000, 9000, 5500, 0),
	      "but not while a hit is still fresh");
	Check(!CustodyMayEnd(true, true, 20000, 9000, 0, 10000),
	      "and never while it burns, stopped and late or not");
	Check(CustodyMayEnd(true, true, 10000 + CUSTODY_BURN_CAP_MS, 9000, 0, 10000),
	      "until a burn that has not gone up in CUSTODY_BURN_CAP_MS");

	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
	RemoteVehicle *v   = const_cast<RemoteVehicle *>(c.VehicleByNetId(80));
	g_rec.carAtRest    = true;
	g_rec.carBurning   = true;
	v->settleEndsAtMs  = 0;   // as if VEHICLE_SETTLE_MS had already gone by
	const int samples  = g_rec.observedSamples;
	for (int i = 0; i < 30; ++i)
		c.TickCustody();
	Check(!v->settleReported,
	      "a burning car is not handed back, stopped and out of time");
	Check(v->burnSinceMs != 0, "when it caught fire is written down");
	Check(g_rec.observedSamples >= samples + 30,
	      "and its health keeps going out - that is the fire everybody else draws");

	g_rec.carBurning = false;
	c.TickCustody();
	Check(v->settleReported && v->burnSinceMs == 0,
	      "once it stops burning the settle ends as it always did");

	// A burn that stopped running here - a paused game - is let go of.
	Client c2;
	c2.SetBridge(RecordingBridge());
	ParkOneCar(c2);
	c2.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
	RemoteVehicle *v2 = const_cast<RemoteVehicle *>(c2.VehicleByNetId(80));
	g_rec.carBurning  = true;
	c2.TickCustody();
	Check(!v2->settleReported, "burning");
	v2->burnSinceMs -= CUSTODY_BURN_CAP_MS;
	c2.TickCustody();
	Check(v2->settleReported, "and given back once it has burned past the cap");

	g_rec.carBurning = false;
	g_rec.carAtRest  = false;
}

void TestOurSettleAfterTakingOverSomebodyElsesIsOurs() {
	std::printf("\ngetting into a car mid-settle, then settling it ourselves\n");
	Client c;
	c.SetBridge(RecordingBridge());
	const int32_t handle = WatchAliceSettleCar80(c);
	Check(WhatTheDetoursSee(handle, false).owner == game::CarOwner::RemoteCustodian,
	      "alice is settling car 80");

	// We get in before it has stopped. Nothing corrects a car we drive, and
	// that pass used to be what wrote the table - so the row went on naming
	// alice as its custodian for as long as we had it.
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = handle;
	c.Tick();
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(0, 80), CH_EVENT));
	Check(c.VehicleByNetId(80)->custodianPlayerId == INVALID_PLAYER,
	      "our own claim ends her custody on our roster too");
	c.Tick();

	// And out again: ours to settle.
	g_rec.drivingLocally     = false;
	g_rec.localVehicleHandle = -1;
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
	c.Tick();
	const DetourView d = WhatTheDetoursSee(handle, false);
	Check(d.owner == game::CarOwner::Local && d.mayBlowUp,
	      "our own custody car is ours to damage, not still alice's");
}

// ---- the dents a settling car takes -----------------------------------------
//
// Every other machine holds a car in custody pinned and collision-proof, so
// the custodian's engine is the only one that dents it. Before this the dents
// only went out from a driver, and a car that rolled into a wall after its
// driver bailed stayed straight everywhere else for good: the merge is a
// maximum and nothing ever sent the dent.

VehicleDamageBody DentWord(unsigned panel, uint8_t level) {
	VehicleDamageBody b{};
	SetPanelLevel(b.panels, panel, level);
	return b;
}

void TestADentWhileWeSettleACarIsSent() {
	std::printf("\na dent our engine makes while we settle a car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeCustody(80, /*us=*/0), CH_EVENT));

	c.TickCustody();
	Check(g_rec.observedDamageSamples == 1, "the car we are settling is read for dents");
	Check(c.DamageReportsSentForTest() == 0, "an undamaged car says nothing");

	g_rec.observedDamage = DentWord(VEHPANEL_FRONT_LEFT, PANEL_STATUS_SMASHED1);
	c.TickCustody();
	Check(c.DamageReportsSentForTest() == 1, "it hits the wall and the dent goes out");
	Check(c.VehicleByNetId(80)->settleDamagePanels == g_rec.observedDamage.panels,
	      "and the high-water mark is what we said");

	c.TickCustody();
	c.TickCustody();
	Check(c.DamageReportsSentForTest() == 1, "the same dent is not said twice");

	g_rec.observedDamage = DentWord(VEHPANEL_FRONT_LEFT, PANEL_STATUS_MISSING);
	c.TickCustody();
	Check(c.DamageReportsSentForTest() == 2, "a worse one is");

	// And the other end of the wire: somebody watching the settle applies it
	// with the parts flying, like any dent from a driver.
	Client o;
	o.SetBridge(RecordingBridge());
	WatchAliceSettleCar80(o);
	uint32_t panels = 0;
	SetPanelLevel(panels, VEHPANEL_FRONT_LEFT, PANEL_STATUS_SMASHED1);
	o.HandleMessage(Wrap(MakeVehicleDamage(80, panels, 0, /*alice=*/1), CH_EVENT));
	Check(g_rec.vehicleDamageApplies == 1 && g_rec.lastDamageFlying,
	      "an observer puts the custodian's dent on its copy");
}

void TestTheLastDentGoesOutBeforeTheSettle() {
	std::printf("\nthe dent in the frame a settle ends\n");

	// It comes to rest.
	{
		Client c;
		c.SetBridge(RecordingBridge());
		ParkOneCar(c);
		c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
		g_rec.carAtRest = true;
		for (int i = 0; i + 1 < VEHICLE_REST_FRAMES; ++i)
			c.TickCustody();
		Check(!c.VehicleByNetId(80)->settleReported, "one quiet sample short");

		g_rec.observedDamage = DentWord(VEHPANEL_REAR_RIGHT, PANEL_STATUS_SMASHED2);
		c.TickCustody();
		Check(c.VehicleByNetId(80)->settleReported, "this pass ends the settle");
		Check(c.DamageReportsSentForTest() == 1,
		      "and the dent from it went out on the same pass, ahead of the settle");
	}

	// Or the window closes on it.
	{
		Client c;
		c.SetBridge(RecordingBridge());
		ParkOneCar(c);
		c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
		RemoteVehicle *v = const_cast<RemoteVehicle *>(c.VehicleByNetId(80));
		v->settleEndsAtMs    = 0;
		g_rec.observedDamage = DentWord(VEHBUMPER_FRONT, PANEL_STATUS_SMASHED1);
		c.TickCustody();
		Check(v->settleReported && c.DamageReportsSentForTest() == 1,
		      "given up on, and the dent still went out first");
	}
}

void TestADentAfterOurSettleIsNotOursToSend() {
	std::printf("\na dent after our settle is over\n");

	// We said it stopped. The server ends the custody on reading that, so
	// anything after it would be refused - and is not sent.
	{
		Client c;
		c.SetBridge(RecordingBridge());
		ParkOneCar(c);
		c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
		RemoteVehicle *v = const_cast<RemoteVehicle *>(c.VehicleByNetId(80));
		v->settleEndsAtMs = 0;
		c.TickCustody();
		Check(v->settleReported, "we have handed it back");

		g_rec.observedDamage = DentWord(VEHPANEL_WINDSCREEN, PANEL_STATUS_SMASHED1);
		c.TickCustody();
		Check(c.DamageReportsSentForTest() == 0, "a dent after that is not sent");
		c.HandleMessage(Wrap(MakeCustody(80, INVALID_PLAYER), CH_EVENT));
		c.TickCustody();
		Check(c.DamageReportsSentForTest() == 0,
		      "nor once the session has said it is nobody's");
	}

	// Somebody got in. Their engine dents it now, and they report it.
	{
		Client c;
		c.SetBridge(RecordingBridge());
		ParkOneCar(c);
		c.HandleMessage(Wrap(MakeJoin(1, "bob"), CH_EVENT));
		c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
		c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
		g_rec.observedDamage = DentWord(VEHPANEL_FRONT_RIGHT, PANEL_STATUS_MISSING);
		c.TickCustody();
		Check(c.DamageReportsSentForTest() == 0, "a new driver ends our reporting");
		Check(g_rec.observedDamageSamples == 0, "we don't even look");
	}
}

void TestALighterDentIsStillNewsAfterASettle() {
	std::printf("\na lighter dent after a settle that sent a heavy one\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	const int32_t handle = c.VehicleByNetId(80)->poolHandle;

	c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
	g_rec.observedDamage = DentWord(VEHPANEL_FRONT_LEFT, PANEL_STATUS_MISSING);
	c.TickCustody();
	Check(c.DamageReportsSentForTest() == 1, "the settle sent a missing wing");
	c.HandleMessage(Wrap(MakeCustody(80, INVALID_PLAYER), CH_EVENT));

	// We drive it next, after a respray took the wing back. The driver keeps
	// its own mark, so what the settle said can't hide this.
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = handle;
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(0, 80), CH_EVENT));
	Check(c.LocalVehicleNetId() == 80, "we are its driver");
	g_rec.localDamage = DentWord(VEHPANEL_FRONT_LEFT, PANEL_STATUS_SMASHED1);
	c.TickLocalDamageForTest();
	Check(c.DamageReportsSentForTest() == 2, "our lighter dent as its driver goes out");

	// And out again, into a second settle of the same car. The first one's
	// mark would call this nothing new; the grant starts a fresh one.
	g_rec.drivingLocally     = false;
	g_rec.localVehicleHandle = -1;
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
	Check(c.VehicleByNetId(80)->settleDamagePanels == 0,
	      "a new custody starts with an empty mark");
	g_rec.observedDamage = DentWord(VEHPANEL_FRONT_LEFT, PANEL_STATUS_SMASHED1);
	c.TickCustody();
	Check(c.DamageReportsSentForTest() == 3, "and the lighter dent is news to it");
}

void TestWhoElseHoldsACar() {
	std::printf("\nthe names the detours' table is given\n");
	RemoteVehicle v;
	v.active = true;
	v.netId  = 80;

	VehicleHolders h = OtherVehicleHolders(v, 0);
	Check(h.driver == INVALID_PLAYER && h.custodian == INVALID_PLAYER,
	      "nobody in it and nobody settling it: nobody");

	v.driverPlayerId = 1;
	h = OtherVehicleHolders(v, 0);
	Check(h.driver == 1 && h.custodian == INVALID_PLAYER, "somebody else driving");

	v.driverPlayerId = 0;
	h = OtherVehicleHolders(v, 0);
	Check(h.driver == INVALID_PLAYER, "never us - a car we drive is ours");

	v.driverPlayerId    = INVALID_PLAYER;
	v.custodianPlayerId = 1;
	h = OtherVehicleHolders(v, 0);
	Check(h.driver == INVALID_PLAYER && h.custodian == 1, "somebody else settling");

	v.custodianPlayerId = 0;
	h = OtherVehicleHolders(v, 0);
	Check(h.custodian == INVALID_PLAYER, "never us settling it either");

	v.driverPlayerId    = 2;
	v.custodianPlayerId = 1;
	h = OtherVehicleHolders(v, 0);
	Check(h.driver == 2 && h.custodian == INVALID_PLAYER,
	      "a stale custodian beside a driver names nobody");

	v.driverPlayerId = 0;
	h = OtherVehicleHolders(v, 0);
	Check(h.driver == INVALID_PLAYER && h.custodian == INVALID_PLAYER,
	      "and beside us at the wheel it doesn't make the car theirs");
	Check(!h.weSettle, "nor ours to settle - we drive it");

	// weSettle is TakeReportedVehicleHit's custody half, and nothing else:
	// the hits other machines forward to us are exactly the ones a replayed
	// round must not take a second time.
	bool same = true;
	const uint8_t ids[] = {0, 1, 2, INVALID_PLAYER};
	for (uint8_t driver : ids)
		for (uint8_t custodian : ids)
			for (int reported = 0; reported < 2; ++reported) {
				RemoteVehicle r;
				r.active            = true;
				r.netId             = 80;
				r.driverPlayerId    = driver;
				r.custodianPlayerId = custodian;
				r.settleReported    = reported != 0;
				const bool fromHolders = OtherVehicleHolders(r, 0).weSettle;
				const bool takesHits   = driver == INVALID_PLAYER &&
				                         TakeReportedVehicleHit(r, false, 0);
				if (fromHolders != takesHits)
					same = false;
			}
	Check(same, "our custody is named exactly when forwarded hits on it are ours");

	// The receiving end.
	RemoteVehicle ours;
	ours.active            = true;
	ours.netId             = 80;
	ours.custodianPlayerId = 0;
	Check(TakeReportedVehicleHit(ours, false, 0), "a hit on our settle is ours");
	ours.settleReported = true;
	Check(!TakeReportedVehicleHit(ours, false, 0),
	      "not after we have said it stopped");
	ours.settleReported    = false;
	ours.custodianPlayerId = 1;
	Check(!TakeReportedVehicleHit(ours, false, 0), "nor somebody else's settle");
	Check(TakeReportedVehicleHit(ours, true, 0), "a car we drive always is");
	ours.destroyed = true;
	Check(!TakeReportedVehicleHit(ours, true, 0), "unless it's a wreck");

	// And the engine's half of it.
	Check(game::MayTakeReportedHit(true, false, false), "at our wheel: yes");
	Check(game::MayTakeReportedHit(false, true, true), "settling, empty seat: yes");
	Check(!game::MayTakeReportedHit(false, false, true),
	      "settling with somebody at the wheel: no, they're the driver now");
	Check(!game::MayTakeReportedHit(false, true, false),
	      "an empty seat alone is not a reason");
}

void TestWithoutTheRowOurClaimedCarWasOursToWreck() {
	std::printf("\nthe gap, reproduced: a claimed car with no row\n");
	Client c;
	WorldBridge b = RecordingBridge();
	b.AdoptClaimedVehicle = nullptr;   // the seam as it was before
	c.SetBridge(b);
	ClaimOurOwnCar(c);
	g_rec.drivingLocally = false;
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(1, 364), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleState(1, 364, 15.0f), CH_SNAPSHOT));
	c.Tick();

	// The roster knows alice is driving. The detours never hear about it.
	Check(c.VehicleByNetId(364)->driverPlayerId == 1, "the roster has alice at the wheel");
	const DetourView d = WhatTheDetoursSee(4242, false);
	Check(d.owner == game::CarOwner::Local && d.mayBlowUp,
	      "and with no row our engine would have wrecked her car by itself");
}

void TestOurOwnCarLeavesTheTableWithTheSession() {
	std::printf("\nour own car is let go of, not destroyed\n");
	Client c;
	c.SetBridge(RecordingBridge());
	ClaimOurOwnCar(c);
	g_rec.drivingLocally = false;
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(1, 364), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleState(1, 364, 15.0f), CH_SNAPSHOT));
	c.Tick();
	Check(WhatTheDetoursSee(4242, false).owner == game::CarOwner::RemoteDriver,
	      "alice is driving our car");

	// The session goes. The car stays ours and in the street, and the row
	// that said alice drives it must not outlive her.
	const int despawns = g_rec.vehicleDespawns;
	c.ClearRosterForTest();
	Check(g_rec.vehicleDespawns == despawns, "the car is not destroyed");
	Check(g_rec.ownReleases == 1, "it is let go of instead");
	Check(g_observedHere.Count() == 0, "and the detours forget it");
	Check(WhatTheDetoursSee(4242, false).owner == game::CarOwner::Local,
	      "so after the session it is single player's car again");

	// Same when the session drops just that car.
	Client c2;
	c2.SetBridge(RecordingBridge());
	ClaimOurOwnCar(c2);
	S_VehicleDespawn gone;
	InitHeader(gone, 1000);
	gone.netId = 364;
	c2.HandleMessage(Wrap(gone, CH_EVENT));
	Check(g_rec.vehicleDespawns == 0 && g_rec.ownReleases == 1,
	      "a despawn lets go of our own car rather than destroying it");
	Check(g_observedHere.Count() == 0, "and takes its row with it");
}

void TestAPromotedCarWeHostedLeavesTheTableToo() {
	std::printf("\nthe promoted car our engine made, dropped by the session\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(1), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(300, /*owner=*/2), CH_EVENT));
	c.Tick();
	c.HandleMessage(Wrap(MakeCarPromoted(300, /*driver=*/2, /*wasOwner=*/1), CH_EVENT));
	Check(c.VehicleByNetId(300)->ours, "our own engine's car, promoted");
	Check(g_observedHere.Count() == 1, "registered with the detours");

	S_VehicleDespawn gone;
	InitHeader(gone, 1000);
	gone.netId = 300;
	c.HandleMessage(Wrap(gone, CH_EVENT));
	Check(g_rec.ownReleases == 1 && g_observedHere.Count() == 0,
	      "dropped by the session, its row goes with it and the car stays");
}

// ---------------------------------------------------------------------------
// A copy's life (game/carlife.h)
// ---------------------------------------------------------------------------

uint8_t By(int createdBy) { return static_cast<uint8_t>(createdBy); }

void TestCopiesDontEatTheTrafficBudget() {
	std::printf("\nsession copies and the traffic cap\n");
	using namespace game;

	// 12.0f * CarNumberMultiplier at the default multiplier (0x0059BF9E).
	const int32_t cap = 12;
	CarCounters c;
	c.mission = 12;
	Check(!GeneratorUnderCap(c, cap),
	      "twelve copies used to take the whole budget: no traffic at all");
	Check(GeneratorUnderCap(c, TrafficAllowance(cap, 12)),
	      "with the allowance the generator carries on");
	c.random = 11;
	Check(GeneratorUnderCap(c, TrafficAllowance(cap, 12)), "up to its own twelve");
	c.random = 12;
	Check(!GeneratorUnderCap(c, TrafficAllowance(cap, 12)), "and not one more");
	Check(TrafficAllowance(cap, 0) == cap && TrafficAllowance(cap, -2) == cap,
	      "no copies, the engine's own cap, and never less");

	// UpdateCarCount's arms (0x004202E0).
	Check(TrafficSumShare(By(VEHICLE_CREATED_BY_MISSION), true) == 1,
	      "a mission police car is counted once");
	Check(TrafficSumShare(By(VEHICLE_CREATED_BY_RANDOM), true) == 2,
	      "a random one twice, law and random");
	Check(TrafficSumShare(By(VEHICLE_CREATED_BY_RANDOM), false) == 1 &&
	          TrafficSumShare(By(VEHICLE_CREATED_BY_PARKED), false) == 1,
	      "random and parked once");
	Check(TrafficSumShare(By(VEHICLE_CREATED_BY_PERMANENT), false) == 0 &&
	          TrafficSumShare(0, false) == 0,
	      "permanent and garbage not at all");
}

void TestACopyStaysOutOfTheSave() {
	std::printf("\nsession copies and the single-player save\n");
	using namespace game;
	const uint8_t mission = By(VEHICLE_CREATED_BY_MISSION);

	// CPools::SaveVehiclePool's own test (0x004A21E2).
	Check(RetailSaveWrites(false, false, VEHICLE_TYPE_CAR, mission) &&
	          RetailSaveWrites(false, false, VEHICLE_TYPE_BOAT, mission),
	      "retail saves an empty mission car or boat");
	Check(!RetailSaveWrites(true, false, VEHICLE_TYPE_CAR, mission) &&
	          !RetailSaveWrites(false, true, VEHICLE_TYPE_CAR, mission),
	      "not one with anybody in it");
	Check(!RetailSaveWrites(false, false, VEHICLE_TYPE_CAR, By(VEHICLE_CREATED_BY_RANDOM)),
	      "nor traffic");
	Check(!RetailSaveWrites(false, false, 5, mission), "nor anything but a car or a boat");

	const uint8_t during = CreatedByDuringSave(true, mission);
	Check(!RetailSaveWrites(false, false, VEHICLE_TYPE_CAR, during),
	      "a parked copy isn't written");
	Check(during == By(VEHICLE_CREATED_BY_PERMANENT), "it reads PERMANENT for the call");
	Check(CreatedByDuringSave(false, mission) == mission,
	      "a mission car this engine made is still saved");
	Check(CreatedByDuringSave(true, By(VEHICLE_CREATED_BY_RANDOM)) ==
	          By(VEHICLE_CREATED_BY_RANDOM),
	      "and a copy given back to the engine reads what it is");
}

void TestHowACopyEnds() {
	std::printf("\nwhat a release does to this machine's car\n");
	using namespace game;
	Check(HowToEndCopy(false, false) == CopyEnd::Destroy, "a copy nobody here is in is destroyed");
	Check(HowToEndCopy(false, true) == CopyEnd::HandToEngine,
	      "one we're sitting in goes to the engine instead");
	Check(HowToEndCopy(true, false) == CopyEnd::Leave && HowToEndCopy(true, true) == CopyEnd::Leave,
	      "and our own engine's car is left alone either way");
}

void TestTheCopyList() {
	std::printf("\nthe engine seam's list of copies\n");
	using namespace game;
	int   cars[4] = {};
	void *pool[4] = {&cars[0], &cars[1], &cars[2], &cars[3]};
	auto  resolve = [&](int32_t h) -> void * { return h >= 0 && h < 4 ? pool[h] : nullptr; };

	CopyList<2> list;
	Check(list.Add(1, resolve) && list.Add(2, resolve), "two copies fit a list of two");
	Check(list.Add(1, resolve), "one added twice is still one");
	Check(!list.Add(3, resolve), "a third doesn't fit");
	Check(list.Add(-1, resolve), "a dead handle is nothing to add");

	pool[2] = nullptr;   // the engine deleted it by itself
	Check(list.Add(3, resolve), "the slot of a car the engine took is reused");
	int live = 0;
	list.ForEachLive(resolve, [&](void *) { ++live; });
	Check(live == 2, "and both copies are seen");
	Check(list.Contains(&cars[3], resolve) && !list.Contains(&cars[2], resolve) &&
	          !list.Contains(nullptr, resolve),
	      "Contains goes by the car, not the handle");

	list.Remove(1);
	Check(!list.Contains(&cars[1], resolve), "a removed copy is gone");
	list.Clear();
	live = 0;
	list.ForEachLive(resolve, [&](void *) { ++live; });
	Check(live == 0, "and Clear empties it");
}

void TestTheTrafficAllowanceRunsAfterTheSpawns() {
	std::printf("\nthe traffic allowance, once a frame\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	const int before = g_rec.trafficAllowances;
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 1, "the copy is built");
	Check(g_rec.trafficAllowances == before + 1, "the allowance runs once in the frame");
	Check(g_rec.vehicleSpawnsAtAllowance == 1, "after the car it has to count");
	c.Tick();
	c.Tick();
	Check(g_rec.trafficAllowances == before + 3, "and every frame after");
}

void TestAReleaseLeavesNobodyWaiting() {
	std::printf("\na released car nobody is waiting for\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);

	// Her enter beat the spawn, and the release beat it too.
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).seatVehicleNetId == 80, "alice is waiting for car 80");
	S_VehicleDespawn gone;
	InitHeader(gone, 2000);
	gone.netId = 80;
	c.HandleMessage(Wrap(gone, CH_EVENT));
	Check(c.PlayerSlot(1).seatVehicleNetId == INVALID_NETID,
	      "the release cancels it, though the car never arrived");

	// The same with the car there and her seated.
	c.HandleMessage(Wrap(MakeVehicleSpawn(81), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 81), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "seated in 81");
	gone.netId = 81;
	c.HandleMessage(Wrap(gone, CH_EVENT));
	Check(!c.PlayerSlot(1).Seated() && c.PlayerSlot(1).seatVehicleNetId == INVALID_NETID,
	      "out of it, and not put back");
	c.Tick();
	Check(g_rec.seats == 1, "not on the next frame either");
}

void TestOurCarReleasedUnderUsIsClaimedAgain() {
	std::printf("\nthe session lets go of the car we're in\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.drivingLocally = true;

	S_EnterVehicle yes;
	InitHeader(yes, 1000);
	yes.playerId     = 0;
	yes.body         = EnterVehicleBody{};
	yes.body.netId   = 77;
	yes.body.modelId = 90;
	c.HandleMessage(Wrap(yes, CH_EVENT));
	Check(c.LocalVehicleNetId() == 77, "we drive 77");

	S_VehicleDespawn gone;
	InitHeader(gone, 2000);
	gone.netId = 78;
	c.HandleMessage(Wrap(gone, CH_EVENT));
	Check(c.LocalVehicleNetId() == 77, "another car going changes nothing");
	gone.netId = 77;
	c.HandleMessage(Wrap(gone, CH_EVENT));
	Check(c.LocalVehicleNetId() == INVALID_NETID,
	      "ours going forgets the name, so the car is claimed afresh");
}

// ---------------------------------------------------------------------------
// The two gates, as numbers (addresses.h)
// ---------------------------------------------------------------------------

void TestTheEntryGateRefusesACarOnItsSideAndNotOneUpsideDown() {
	std::printf("\nwhat CVehicle::CanPedEnterCar actually refuses\n");

	// Read backwards from the way the bug is described, which is why it is
	// written down as a predicate and checked here. 0x00552307 compares up.z
	// with +0.1f and jumps to the velocity tests when it is GREATER;
	// 0x0055231C compares with -0.1f and jumps there when it is LESS. The
	// fallthrough - the refusal - is everything in between.
	Check(game::VehiclePoseAllowsPedThrough(1.0f), "a car on its wheels, obviously");
	Check(game::VehiclePoseAllowsPedThrough(-1.0f),
	      "and one on its roof, which is the surprising half and is correct");
	Check(!game::VehiclePoseAllowsPedThrough(0.0f),
	      "a car exactly on its side is refused");
	Check(!game::VehiclePoseAllowsPedThrough(0.1f),
	      "and so is one at the boundary, because the test is strict");
	Check(!game::VehiclePoseAllowsPedThrough(-0.1f), "on both sides of it");
	Check(game::VehiclePoseAllowsPedThrough(0.11f),
	      "a hair past it and the car is usable again");

	// Which is why a wedged car has to be allowed to fall. Nothing in the
	// engine rights a car on its side - single player does not either - so
	// the fix is not to correct the pose, it is to stop pinning the car in it.
	Check(game::VEH_ENTER_MAX_SPEED_SQ == 0.04f,
	      "the entry speed gate is sq(0.2f), read out of 0x00602594");
}

void TestARestedCarPassesBothGates() {
	std::printf("\nwhen a custodian may call a car finished\n");

	// The tightest numbers in the engine, and deliberately so: a car handed
	// back to the pinned world has to be one a player can get into AND out
	// of. The exit gate is eight times stricter about speed than the entry
	// gate (0.005 against 0.04), so meeting the entry gate alone would hand
	// back a car whose occupant then cannot get out and has no way to tell
	// that from the car simply not responding.
	Check(game::VehicleAtRestNumbers(0.0f, 0.0f, 0.0f, 0.0f), "a still car is at rest");
	Check(game::VehicleAtRestNumbers(0.004f, 0.009f, -0.009f, 0.009f),
	      "and so is one just inside every bound");
	Check(!game::VehicleAtRestNumbers(0.006f, 0.0f, 0.0f, 0.0f),
	      "a car still rolling is not, at a speed the entry gate would allow");
	Check(game::VEH_ENTER_MAX_SPEED_SQ > game::VEH_EXIT_MAX_SPEED_SQ,
	      "which is the point: getting out is stricter than getting in");

	// Per component and not a magnitude, because that is what 0x0055243A,
	// 0x00552464 and 0x0055248E do - three fabs compares, not one
	// MagnitudeSqr. A car spinning about one axis at 0.015 fails, although
	// the magnitude of that vector is well under any squared bound.
	Check(!game::VehicleAtRestNumbers(0.0f, 0.0f, 0.0f, 0.015f),
	      "one axis over the turn bound is enough, on its own");
	Check(!game::VehicleAtRestNumbers(0.0f, -0.015f, 0.0f, 0.0f),
	      "and the sign of it does not matter");
}

// ---------------------------------------------------------------------------
// Boats - docs/roadmap.md M2, client/src/game/boat.h
// ---------------------------------------------------------------------------

// CREATE_CAR's own choice, read off the model info: m_type at +0x2A has to be
// MITYPE_VEHICLE and the vehicle type at +0x58 picks the class
// (CModelInfo::IsBoatModel at 0x0050BB90, the bike test at 0x0043C5ED).
void TestABoatModelIsBuiltAsABoat() {
	std::printf("\nwhich class a synced vehicle is built as\n");

	using game::VehicleBuild;
	using game::VehicleBuildFor;

	Check(VehicleBuildFor(true, game::MITYPE_VEHICLE, game::VEHICLE_TYPE_BOAT) ==
	          VehicleBuild::Boat,
	      "a boat model is built as a boat");
	Check(VehicleBuildFor(true, game::MITYPE_VEHICLE, game::VEHICLE_TYPE_CAR) ==
	          VehicleBuild::Automobile,
	      "a car model is built as a CAutomobile, as before");

	// The engine would crash on the first and misread the second. Both came
	// off a socket here, so both are refused.
	Check(VehicleBuildFor(false, game::MITYPE_VEHICLE, game::VEHICLE_TYPE_CAR) ==
	          VehicleBuild::None,
	      "no model info, nothing built");
	Check(VehicleBuildFor(true, 6 /* MITYPE_PED */, game::VEHICLE_TYPE_BOAT) ==
	          VehicleBuild::None,
	      "a model that is not a vehicle's is refused even when +0x58 says boat");
	Check(VehicleBuildFor(true, game::MITYPE_VEHICLE, game::VEHICLE_TYPE_BIKE) ==
	          VehicleBuild::None,
	      "a bike, which CREATE_CAR builds nothing for either");

	// Left as CREATE_CAR has them. What a synced train is belongs to the
	// train work, and changing it from here would be deciding it.
	Check(VehicleBuildFor(true, game::MITYPE_VEHICLE, game::VEHICLE_TYPE_TRAIN) ==
	          VehicleBuild::Automobile,
	      "a train model is left where CREATE_CAR leaves it");

	// Size, constructor and the type the constructor stamps, as one set.
	Check(game::VehicleBuildSize(VehicleBuild::Boat) == 0x484,
	      "a boat is allocated at CREATE_CAR's `push 484h`");
	Check(game::VehicleBuildSize(VehicleBuild::Automobile) == 0x5A8,
	      "a car at `push 5A8h`");
	Check(game::VehicleBuildSize(VehicleBuild::Boat) <= game::offs::SIZEOF_AUTOMOBILE,
	      "and a boat fits a slot of a pool strided at 0x5A8");
	Check(game::VehicleBuildCtor(VehicleBuild::Boat) == 0x0053E3E0 &&
	          game::VehicleBuildCtor(VehicleBuild::Automobile) == 0x0052C6B0,
	      "each class gets its own constructor");
	Check(game::VehicleBuildCtor(VehicleBuild::None) == 0 &&
	          game::VehicleBuildSize(VehicleBuild::None) == 0,
	      "and None gets neither");
	Check(game::VehicleBuildType(VehicleBuild::Boat) == 1 &&
	          game::VehicleBuildType(VehicleBuild::Automobile) == 0,
	      "m_vehType as the two constructors write it (0x0053E42A, 0x0052C766)");

	// The car branch joins the road system and the boat branch does not;
	// the cruise speeds are 9 and 20.
	Check(game::JoinsRoadSystemOnSpawn(VehicleBuild::Automobile) &&
	          !game::JoinsRoadSystemOnSpawn(VehicleBuild::Boat),
	      "only a car is joined to the road system");
	Check(game::SpawnCruiseSpeed(VehicleBuild::Automobile) == 9.0f &&
	          game::SpawnCruiseSpeed(VehicleBuild::Boat) == 20.0f,
	      "cruise speed out of each branch, 9 and 20");
}

// Every CAutomobile-only write in game/vehicle.cpp and game/ped.cpp, asked of
// the type the built object carries. A wrong answer here is a write into a
// boat's own members, which does not crash and does not look like anything.
void TestTheWritersThatMustNotTouchABoat() {
	std::printf("\nwhich writers may touch a boat\n");

	const int32_t boat = game::VehicleBuildType(game::VehicleBuild::Boat);
	const int32_t car  = game::VehicleBuildType(game::VehicleBuild::Automobile);

	// The damage model: CDamageManager at +0x288 and the three appliers.
	Check(game::HasAutomobileBody(car), "a car has a damage manager at +0x288");
	Check(!game::HasAutomobileBody(boat),
	      "a boat does not - +0x288 is its own first float");
	Check(!game::HasAutomobileBody(game::VEHICLE_TYPE_TRAIN),
	      "and neither does anything else");

	// The fire timer: the car's is past the end of a boat.
	Check(game::FireBlowUpTimerOffset(car) == 0x530,
	      "a car's fire timer is +0x530");
	Check(game::FireBlowUpTimerOffset(boat) == 0x2CC,
	      "a boat's is +0x2CC, the one CBoat::ProcessControl adds to at 0x0053FB04");
	Check(game::FireBlowUpTimerOffset(car) >= game::offs::SIZEOF_BOAT,
	      "the car's offset lies outside a CBoat");
	Check(game::FireBlowUpTimerOffset(boat) + sizeof(float) <= game::offs::SIZEOF_BOAT &&
	          game::FireBlowUpTimerOffset(boat) >= game::offs::SIZEOF_VEHICLE,
	      "the boat's lies inside it, among CBoat's own members");
	Check(game::FireBlowUpTimerOffset(game::VEHICLE_TYPE_TRAIN) == 0,
	      "a type with no known timer is left alone");

	// The seat. SetObjective undoes ENTER_CAR on a boat for a non-player
	// (0x004D8519), so the warp needs the objective put back, and the
	// animated entry is not tried.
	Check(game::SetObjectiveRefusesNonPlayer(boat) &&
	          !game::SetObjectiveRefusesNonPlayer(car),
	      "only a boat has its ENTER_CAR objective undone for a replica");
	Check(!game::ReplicaMayAnimateEntry(boat) && game::ReplicaMayAnimateEntry(car),
	      "so a replica is warped into a boat and walked to a car");

	// The unseat's liveness test has to know both classes, or a ped in a
	// boat is never taken out of it before the boat is destroyed.
	Check(game::IsBuiltVehicleVtable(game::CAutomobile__vtable) &&
	          game::IsBuiltVehicleVtable(game::CBoat__vtable),
	      "both built classes pass the unseat's vtable test");
	Check(!game::IsBuiltVehicleVtable(game::CVehicle__vtable) &&
	          !game::IsBuiltVehicleVtable(game::CPlaceable__vtable) &&
	          !game::IsBuiltVehicleVtable(0),
	      "a destroyed or half-built one does not");
}

// Nothing on a boat ever sets bIsStatic (CPhysical::ProcessControl skips
// vehicles at 0x00495F9A and CBoat has no counter of its own), so a boat's
// settle rests on VehicleAtRestNumbers alone. What makes that enough is that
// a boat can only be left through SetExitCar, and so through CanPedExitCar's
// numbers, which are the same ones. Checked on the edges, so that tightening
// the settle past the exit gate shows up here as a boat that never settles.
void TestABoatItsDriverCouldLeaveIsAlreadyAtRest() {
	std::printf("\nhow a boat settles\n");

	const float s = game::VEH_EXIT_MAX_SPEED_SQ;
	const float t = game::VEH_EXIT_MAX_TURN;
	Check(game::VehicleAtRestNumbers(s, t, -t, t),
	      "the fastest a driver may step off at is already at rest");
	Check(game::VehicleAtRestNumbers(s * 0.5f, 0.0f, t * 0.5f, 0.0f),
	      "and so is anything slower");
	Check(!game::VehicleAtRestNumbers(s * 1.5f, 0.0f, 0.0f, 0.0f),
	      "a boat still coasting faster than that is not, and waits for the window");
}

// ---------------------------------------------------------------------------
// Rampages - docs/roadmap.md 5.10, client/src/game/darkel.h
// ---------------------------------------------------------------------------

// The engine's own qualification test, transcribed into game/darkel.h from
// CDarkel::RegisterKillByPlayer. Every case below is one of the branches at
// 0x00420F7C..0x00420FF9 and the numbers are eWeaponType as retail holds it.
void TestWhichKillsCountTowardARampage() {
	std::printf("\nwhich kills count, by the engine's own test\n");

	game::FrenzyTargets f;
	f.weapon = 6;        // M16, what rampage 01 is started with
	f.model1 = 108;      // GANG05
	f.model2 = 109;      // GANG06

	Check(game::RampageKillCounts(f, 108, 6, false), "the right gang, the right gun");
	Check(game::RampageKillCounts(f, 109, 6, false), "and the second model too");
	Check(!game::RampageKillCounts(f, 110, 6, false), "the wrong gang does not count");
	Check(!game::RampageKillCounts(f, 108, 2, false), "nor the wrong gun");

	// The five aliases, and they are the reason this is a function rather
	// than one comparison.
	Check(game::RampageKillCounts(f, 108, 0x12, false),
	      "an explosion counts for every rampage there is");

	game::FrenzyTargets uzi;
	uzi.weapon = 3;
	uzi.model1 = FRENZY_ANY_PED;
	Check(game::RampageKillCounts(uzi, 999, 0x13, false),
	      "a drive-by uzi is an uzi, and ANY_PED takes anybody");

	game::FrenzyTargets ram;
	ram.weapon = 0x11;   // RUNOVERBYCAR
	ram.model1 = FRENZY_ANY_PED;
	Check(game::RampageKillCounts(ram, 7, 0x10, false),
	      "rammed counts for a run-over rampage");

	game::FrenzyTargets molotov;
	molotov.weapon = 0x0A;
	molotov.model1 = FRENZY_ANY_PED;
	Check(game::RampageKillCounts(molotov, 7, 9, false),
	      "a flamethrower counts for a molotov rampage");

	// The three headshot rampages rampage.sc starts with 0367.
	game::FrenzyTargets sniper;
	sniper.weapon       = 7;
	sniper.model1       = 112;
	sniper.needHeadShot = true;
	Check(!game::RampageKillCounts(sniper, 112, 7, false),
	      "a body shot does not count when the rampage wants heads");
	Check(game::RampageKillCounts(sniper, 112, 7, true), "a headshot does");

	// FRENZY_ANY_CAR is the same global with a different sentinel, and it
	// belongs to RegisterCarBlownUpByPlayer. No pedestrian may satisfy it.
	game::FrenzyTargets cars;
	cars.weapon = 8;
	cars.model1 = FRENZY_ANY_CAR;
	Check(!game::RampageKillCounts(cars, 7, 8, false),
	      "a pedestrian never counts toward a vehicle rampage");
}

void TestARampageOpensAndTheTargetArrives() {
	std::printf("\nthe session's rampage reaching a client\n");
	Client c;
	c.SetBridge(RecordingBridge());

	S_Welcome w = MakeWelcome(3);
	w.flags     = FlagsWithRampageRule(0, RAMPAGE_RULE_SCALED);
	c.HandleMessage(Wrap(w, CH_EVENT));
	Check(g_rec.rampageRuleSets == 1 && g_rec.rampageRule == RAMPAGE_RULE_SCALED,
	      "the seam is told the session's rule on the welcome");

	S_RampageOpen open;
	InitHeader(open, 1000);
	open.body.frenzyId    = 7;
	open.body.killsNeeded = 40;
	open.body.elapsedMs   = 0;
	c.HandleMessage(Wrap(open, CH_EVENT));
	Check(g_rec.rampageOpens == 1 && g_rec.rampageKillsNeeded == 40,
	      "and the number the session is playing for");

	// A kill from somebody else, for this frenzy.
	S_RampageKill kill;
	InitHeader(kill, 1010);
	kill.byPlayer      = 1;
	kill.body.frenzyId = 7;
	kill.body.model    = 108;
	kill.body.weapon   = 6;
	kill.body.flags    = RK_F_HEADSHOT;
	c.HandleMessage(Wrap(kill, CH_EVENT));
	Check(g_rec.rampageCredits == 1 && g_rec.rampageLastModel == 108 &&
	          g_rec.rampageLastWeapon == 6 && g_rec.rampageLastHead,
	      "a kill off the wire reaches our own CDarkel with all three arguments");

	// Our own, echoed back. The server leaves the reporter out, so this
	// should never arrive - and if it did it would count one kill twice.
	kill.byPlayer = 3;
	c.HandleMessage(Wrap(kill, CH_EVENT));
	Check(g_rec.rampageCredits == 1, "our own kill coming back is not counted again");

	// A straggler for a frenzy that is not the open one.
	kill.byPlayer      = 1;
	kill.body.frenzyId = 6;
	c.HandleMessage(Wrap(kill, CH_EVENT));
	Check(g_rec.rampageCredits == 1, "and neither is a kill for an older frenzy");

	S_RampageEnd end;
	InitHeader(end, 1020);
	end.body.frenzyId = 7;
	end.body.outcome  = RAMPAGE_PASSED;
	c.HandleMessage(Wrap(end, CH_EVENT));
	Check(g_rec.rampageVerdicts == 1 && g_rec.rampageVerdict == RAMPAGE_PASSED,
	      "the session's verdict is handed to the seam");

	// And the frenzy is shut here too: anything else for it is stale.
	c.HandleMessage(Wrap(end, CH_EVENT));
	Check(g_rec.rampageVerdicts == 1, "a second copy of the verdict changes nothing");
	c.HandleMessage(Wrap(kill, CH_EVENT));
	Check(g_rec.rampageCredits == 1, "and a kill after the ending is dropped");
}

void TestNothingIsCreditedWithoutAnOpenFrenzy() {
	std::printf("\na kill with no rampage open\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// A client that has not been told a frenzy is open has nothing to credit
	// against, and this is the ordinary case for anything that arrives out of
	// order - not an error.
	S_RampageKill kill;
	InitHeader(kill, 1000);
	kill.byPlayer      = 1;
	kill.body.frenzyId = 4;
	kill.body.model    = 108;
	kill.body.weapon   = 6;
	kill.body.flags    = 0;
	c.HandleMessage(Wrap(kill, CH_EVENT));
	Check(g_rec.rampageCredits == 0, "nothing is credited");

	S_RampageEnd end;
	InitHeader(end, 1001);
	end.body.frenzyId = 4;
	end.body.outcome  = RAMPAGE_FAILED;
	c.HandleMessage(Wrap(end, CH_EVENT));
	Check(g_rec.rampageVerdicts == 0, "and no verdict is applied");
}

void TestAWelcomeWithNoRampageBitsSharesAnyway() {
	std::printf("\na server too old to set the rampage bits\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	// roadmap.md 5.10 is the decision, so it is the default: an unset field
	// must not mean "off" the way it would if the bits had been chosen the
	// other way round.
	Check(g_rec.rampageRule == RAMPAGE_RULE_SHARED,
	      "the default is shared, which is the decision");
}

void TestTheRampageBitsDoNotCollideWithTheOtherRules() {
	std::printf("\nthe session flags do not tread on each other\n");
	// The wanted work and the ammo work already collided over bit 1 once
	// (protocol.h, SessionFlags). Two bits later, the same check.
	uint8_t flags = 0;
	flags = FlagsWithWantedRule(flags, WANTED_RULE_OFF);
	flags = FlagsWithRampageRule(flags, RAMPAGE_RULE_SCALED);
	flags |= SESSION_AMMO_SYNC;
	flags |= SESSION_FRIENDLY_FIRE;
	Check(WantedRuleFromFlags(flags) == WANTED_RULE_OFF, "the wanted rule survives");
	Check(RampageRuleFromFlags(flags) == RAMPAGE_RULE_SCALED, "and the rampage rule");
	Check((flags & SESSION_AMMO_SYNC) != 0, "and ammo sync");
	Check((flags & SESSION_FRIENDLY_FIRE) != 0, "and friendly fire");
	// 3 is not a rule.
	Check(RampageRuleFromFlags(uint8_t(3 << SESSION_RAMPAGE_SHIFT)) ==
	          RAMPAGE_RULE_SHARED,
	      "a value from a newer build falls back to the default");
}

// ---- vehicle rampages (game/darkel.h, "one car, one count") -------------

UnownedVehicleKey CarKey(uint8_t kind, uint16_t id) {
	UnownedVehicleKey k{};
	k.kind = kind;
	k.pad  = 0;
	k.id   = id;
	return k;
}

void TestWhichCarsCountTowardARampage() {
	std::printf("\nwhich cars count toward a rampage\n");

	// Rampage 02: `targets ALL_CARS -1 -1 -1`, which is -2 in the first slot.
	game::FrenzyTargets any;
	any.weapon = 8;   // rocket launcher, and the car register never reads it
	any.model1 = FRENZY_ANY_CAR;
	any.model2 = any.model3 = any.model4 = -1;
	Check(game::RampageCarCounts(any, 90), "any car counts for ALL_CARS");
	Check(game::RampageCarCounts(any, 116), "whatever the model");

	game::FrenzyTargets some;
	some.model1 = 110;
	some.model2 = 116;
	some.model3 = some.model4 = -1;
	Check(game::RampageCarCounts(some, 116), "a listed model counts");
	Check(!game::RampageCarCounts(some, 90), "an unlisted one doesn't");

	// A pedestrian rampage holds -1 in the first slot. The car register only
	// compares the first slot against -2, so no car ever counts toward one.
	game::FrenzyTargets peds;
	peds.model1 = FRENZY_ANY_PED;
	Check(!game::RampageCarCounts(peds, 90), "no car counts toward a pedestrian rampage");

	Check(RampageCarKeyed(CarKey(UNOWNED_PARKED, 3)), "a parked car carries a name");
	Check(RampageCarKeyed(CarKey(UNOWNED_SESSION, 3)),
	      "so does a session car nobody drives");
	Check(!RampageCarKeyed(CarKey(UNOWNED_AMBIENT, 3)),
	      "traffic doesn't: only its host can decide it");
	Check(!RampageCarKeyed(CarKey(RAMPAGE_CAR_UNKEYED, 3)), "and unkeyed is unkeyed");
}

void TestTheCarTallyDecision() {
	std::printf("\nwhether a car that blew up here is ours to count\n");
	using game::CarWreckTally;
	using game::TallyCarWreck;
	Check(TallyCarWreck(false, false, false) == CarWreckTally::Engine,
	      "no shared frenzy: the register runs as retail");
	Check(TallyCarWreck(false, true, true) == CarWreckTally::Engine,
	      "including for a replay, which is what rampages = off means");
	Check(TallyCarWreck(true, false, false) == CarWreckTally::Count,
	      "our own engine decided it: count it and say so");
	Check(TallyCarWreck(true, true, false) == CarWreckTally::Withhold,
	      "a replay of another machine's wreck is kept off the counter");
	Check(TallyCarWreck(true, false, true) == CarWreckTally::Withhold,
	      "and so is a named car the session already counted here");

	game::CountedCars c;
	Check(!c.Add(CarKey(RAMPAGE_CAR_UNKEYED, 0)), "an unkeyed car is never remembered");
	Check(!c.Has(CarKey(RAMPAGE_CAR_UNKEYED, 0)), "so it is never 'already counted'");
	Check(c.Add(CarKey(UNOWNED_PARKED, 7)), "a parked car is");
	Check(!c.Add(CarKey(UNOWNED_PARKED, 7)), "once");
	Check(!c.Has(CarKey(UNOWNED_SESSION, 7)), "and the kind is part of the name");
	c.Clear();
	Check(!c.Has(CarKey(UNOWNED_PARKED, 7)), "a new frenzy starts with nothing counted");

	for (uint16_t i = 0; i < game::CountedCars::CAPACITY + 1; ++i)
		c.Add(CarKey(UNOWNED_PARKED, i));
	Check(!c.Has(CarKey(UNOWNED_PARKED, 0)), "full, the oldest name goes first");
	Check(c.Has(CarKey(UNOWNED_PARKED, game::CountedCars::CAPACITY)),
	      "and the newest is kept");

	game::FrenzyTargets any;
	any.model1 = FRENZY_ANY_CAR;
	Check(game::CarFromWireCounts(true, any, 90, false), "a car off the wire counts");
	Check(!game::CarFromWireCounts(false, any, 90, false), "not with no frenzy running");
	Check(!game::CarFromWireCounts(true, any, 90, true), "nor when it already counted here");
}

// Two machines' worth of the pure half of game/darkel.cpp, walked through
// every way a car could count twice or not at all. The engine's own counter
// is `counter`, the rampage is Rampage 02 (13 cars, any model).
struct CarTallyMachine {
	game::FrenzyTargets frenzy;
	game::CountedCars   counted;
	int                 counter = 13;

	CarTallyMachine() {
		frenzy.model1 = FRENZY_ANY_CAR;
		frenzy.model2 = frenzy.model3 = frenzy.model4 = -1;
	}

	// Our own BlowUpCar ran. True when it counted and went on the wire.
	bool Wreck(int32_t model, bool replay, const UnownedVehicleKey &key) {
		if (game::TallyCarWreck(true, replay, counted.Has(key)) !=
		    game::CarWreckTally::Count)
			return false;
		if (!game::RampageCarCounts(frenzy, model))
			return false;
		--counter;
		counted.Add(key);
		return true;
	}

	void FromWire(int32_t model, const UnownedVehicleKey &key) {
		if (!game::CarFromWireCounts(true, frenzy, model, counted.Has(key)))
			return;
		counted.Add(key);
		--counter;
	}
};

void TestEveryCarCountsOnceOnEveryMachine() {
	std::printf("\na vehicle rampage counts each car once, everywhere\n");
	CarTallyMachine a, b;
	const UnownedVehicleKey none = CarKey(RAMPAGE_CAR_UNKEYED, 0);

	// B shoots A's traffic. A's engine decides and counts it; B replays the
	// host's wreck, which used to count on B as well, and then gets A's relay.
	Check(a.Wreck(90, false, none), "the host's own wreck counts and is reported");
	Check(!b.Wreck(90, true, none), "the shooter's replay of it doesn't");
	b.FromWire(90, none);
	Check(a.counter == 12 && b.counter == 12, "one car, one count on each machine");

	// A parked car, and each machine's copy went up in its own copy of the
	// blast before either heard from the other.
	const UnownedVehicleKey parked = CarKey(UNOWNED_PARKED, 41);
	Check(a.Wreck(95, false, parked), "A's copy counts");
	Check(b.Wreck(95, false, parked), "B's copy counts too, independently");
	a.FromWire(95, parked);
	b.FromWire(95, parked);
	Check(a.counter == 11 && b.counter == 11,
	      "and neither counts the other's report of the same car");

	// The relay arrives before B's own copy goes up (a bomb timer is one to
	// three seconds and rolled on each machine).
	const UnownedVehicleKey late = CarKey(UNOWNED_PARKED, 42);
	a.Wreck(95, false, late);
	b.FromWire(95, late);
	Check(!b.Wreck(95, false, late), "a copy that goes up after the relay is held back");
	Check(a.counter == 10 && b.counter == 10, "still once each");

	// S_UnownedBlowUp reached B first and B replayed the wreck.
	const UnownedVehicleKey told = CarKey(UNOWNED_PARKED, 43);
	a.Wreck(95, false, told);
	Check(!b.Wreck(95, true, told), "a replayed parked car is held back");
	b.FromWire(95, told);
	Check(a.counter == 9 && b.counter == 9, "and the relay counts it");

	// A session car nobody was driving, same rules under a different name.
	const UnownedVehicleKey abandoned = CarKey(UNOWNED_SESSION, 5);
	b.Wreck(116, false, abandoned);
	a.Wreck(116, false, abandoned);
	a.FromWire(116, abandoned);
	b.FromWire(116, abandoned);
	Check(a.counter == 8 && b.counter == 8, "an abandoned session car counts once too");

	// A car only B's engine has - traffic A never had a replica of, too far
	// away. The case that used to count for B and nobody else.
	Check(b.Wreck(90, false, none), "a car only B has counts on B");
	a.FromWire(90, none);
	Check(a.counter == 7 && b.counter == 7, "and now on A");

	// Two different unkeyed cars of the same model are two cars.
	b.Wreck(90, false, none);
	a.FromWire(90, none);
	Check(a.counter == 6 && b.counter == 6, "the same model twice is two cars");
}

void TestARampageCarReachesTheSeam() {
	std::printf("\na car off the wire reaching the seam\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(3), CH_EVENT));

	S_RampageCar car;
	InitHeader(car, 1000);
	car.byPlayer      = 1;
	car.body.frenzyId = 9;
	car.body.model    = 116;
	car.body.key      = CarKey(UNOWNED_PARKED, 41);
	c.HandleMessage(Wrap(car, CH_EVENT));
	Check(g_rec.rampageCarCredits == 0, "nothing with no frenzy open");

	S_RampageOpen open;
	InitHeader(open, 1001);
	open.body.frenzyId    = 9;
	open.body.killsNeeded = 13;
	open.body.elapsedMs   = 0;
	c.HandleMessage(Wrap(open, CH_EVENT));

	c.HandleMessage(Wrap(car, CH_EVENT));
	Check(g_rec.rampageCarCredits == 1 && g_rec.rampageCarModel == 116,
	      "a car for this frenzy reaches the seam with its model");
	Check(g_rec.rampageCarKey.kind == UNOWNED_PARKED && g_rec.rampageCarKey.id == 41,
	      "and its name, which the seam needs to count it once");
	Check(g_rec.rampageCredits == 0, "and it isn't handed over as a pedestrian");

	car.byPlayer = 3;
	c.HandleMessage(Wrap(car, CH_EVENT));
	Check(g_rec.rampageCarCredits == 1, "our own car coming back is dropped");

	car.byPlayer      = 1;
	car.body.frenzyId = 8;
	c.HandleMessage(Wrap(car, CH_EVENT));
	Check(g_rec.rampageCarCredits == 1, "and so is one for an older frenzy");
}

// ---- the chat feed, fed by the session (chatfeed.h) ------------------------

S_Chat MakeChat(uint8_t playerId, const char *text) {
	S_Chat c;
	InitHeader(c, 1500);
	c.playerId = playerId;
	std::memset(c.text, 0, sizeof c.text);
	std::strncpy(c.text, text, sizeof c.text - 1);
	return c;
}

bool FeedLineIs(const Client &c, size_t i, FeedKind kind, const char *text) {
	return i < c.Feed().Count() && c.Feed().Line(i).kind == kind &&
	       std::strcmp(c.Feed().Line(i).text, text) == 0;
}

void TestTheFeedHearsTheSession() {
	std::printf("\nthe chat feed hears the session\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	Check(c.Feed().Count() == 1 && FeedLineIs(c, 0, FeedKind::Notice, "connected to the session"),
	      "getting in says so");
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	Check(c.Feed().Count() == 2 && FeedLineIs(c, 1, FeedKind::Notice, "back in the session"),
	      "and getting back in after a loss says that");
	c.ClearFeedForTest();
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	Check(c.Feed().Count() == 0, "somebody who was here before us is not announced");

	S_PlayerJoin bob = MakeJoin(2, "bob");
	bob.flags |= PJF_ARRIVED;
	c.HandleMessage(Wrap(bob, CH_EVENT));
	Check(c.Feed().Count() == 1 && FeedLineIs(c, 0, FeedKind::Notice, "bob joined"),
	      "somebody arriving now is");

	c.HandleMessage(Wrap(MakeChat(1, "hello"), CH_EVENT));
	Check(FeedLineIs(c, 1, FeedKind::Chat, "alice: hello"), "what alice said, under her name");
	c.HandleMessage(Wrap(MakeChat(0, "hi"), CH_EVENT));
	Check(FeedLineIs(c, 2, FeedKind::Chat, "you: hi"), "our own line, as the server relayed it");
	c.HandleMessage(Wrap(MakeChat(6, "boo"), CH_EVENT));
	Check(FeedLineIs(c, 3, FeedKind::Chat, "?: boo"), "a slot nobody told us about is nobody");

	Check(c.Feed().Line(1).playerId == 1 && c.Feed().Line(1).nickLen == 6 &&
	          c.Feed().Line(2).playerId == 0 && c.Feed().Line(3).playerId == INVALID_PLAYER,
	      "each name in its player's colour, and nobody's in nobody's");

	S_Chat unterminated = MakeChat(1, "");
	std::memset(unterminated.text, 'x', sizeof unterminated.text);
	c.HandleMessage(Wrap(unterminated, CH_EVENT));
	size_t xs = 0;
	for (size_t i = 4; i < c.Feed().Count(); ++i)
		for (const char *t = c.Feed().Line(i).text; *t; ++t)
			xs += *t == 'x';
	Check(c.Feed().Count() == 7 && xs == CHAT_LEN - 1,
	      "a line with no end on the wire is ended at the packet's, on three lines");

	S_PlayerLeave leave;
	InitHeader(leave, 2000);
	leave.playerId = 2;
	leave.reason   = LEAVE_TIMEOUT;
	c.HandleMessage(Wrap(leave, CH_EVENT));
	Check(FeedLineIs(c, 7, FeedKind::Notice, "bob lost connection"), "and somebody going");
	c.HandleMessage(Wrap(leave, CH_EVENT));
	Check(c.Feed().Count() == 8 && FeedLineIs(c, 0, FeedKind::Notice, "bob joined"),
	      "a second leave for the same slot says nothing");
}

S_VehicleSpawn MakeRejoinSpawn(uint16_t netId, uint16_t model) {
	S_VehicleSpawn s{};
	InitHeader(s, 1000);
	s.netId   = netId;
	s.modelId = model;
	s.pos     = {10.0f, 20.0f, 3.0f};
	s.rot     = Quat{0.0f, 0.0f, 0.0f, 1.0f};
	s.health  = 1000.0f;
	s.extra1  = -1;
	s.extra2  = -1;
	return s;
}

void TestBackInTheCarWeWereIn() {
	std::printf("\nback in the session in the car we were in\n");
	Client c;
	c.SetBridge(RecordingBridge());
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeRejoinSpawn(300, 90), CH_EVENT));
	c.HandleMessage(Wrap(MakeRejoinSpawn(301, 91), CH_EVENT));
	c.Tick();
	Check(g_rec.vehicleSpawns == 2, "(two cars in the first session)");
	const int32_t ours = c.VehicleByNetId(300)->poolHandle;
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = ours;

	// The connection drops and comes back: a second welcome.
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeRejoinSpawn(300, 90), CH_EVENT));
	c.HandleMessage(Wrap(MakeRejoinSpawn(301, 91), CH_EVENT));
	c.Tick();
	const RemoteVehicle *back = c.VehicleByNetId(300);
	Check(back != nullptr && back->poolHandle == ours && back->ours && !back->spawnPending,
	      "the car we are in is taken up again, as our engine's own");
	Check(g_rec.vehicleSpawns == 3, "and only the other one is built again");
	c.TickLocalVehicle();
	Check(!c.VehicleClaimPending(), "its claim waits for the seats behind it");
	c.EndRejoinWaitForTest();
	c.TickLocalVehicle();
	Check(c.VehicleClaimPending(), "and goes out once nobody has said otherwise");

	// Out of the car by the time the session hands it back: built as usual.
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.localVehicleHandle = -1;
	c.HandleMessage(Wrap(MakeRejoinSpawn(300, 90), CH_EVENT));
	c.Tick();
	Check(c.VehicleByNetId(300)->poolHandle != ours && g_rec.vehicleSpawns == 4,
	      "a car we have got out of since is built the ordinary way");
}

void TestSomebodyElseHasTheCarWeWereIn() {
	std::printf("\nsomebody else has the car we were in\n");
	Client c;
	c.SetBridge(RecordingBridge());
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeRejoinSpawn(300, 90), CH_EVENT));
	c.Tick();
	const int32_t ours = c.VehicleByNetId(300)->poolHandle;
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = ours;

	// Bob got into it while we were gone, and the seats say so.
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "bob"), CH_EVENT));
	c.HandleMessage(Wrap(MakeRejoinSpawn(300, 90), CH_EVENT));
	const int releases = g_rec.ownReleases;
	c.HandleMessage(Wrap(MakeEnter(1, 300), CH_EVENT));
	const RemoteVehicle *bobs = c.VehicleByNetId(300);
	Check(bobs->poolHandle < 0 && !bobs->ours && bobs->spawnPending &&
	          g_rec.ownReleases == releases + 1,
	      "bob's car is let go of, to be built as bob's");
	Check(!bobs->surrendered && g_rec.vehicleSurrenders == 0,
	      "and we are not put out of the car we are in");
	c.Tick();
	Check(c.VehicleByNetId(300)->poolHandle >= 0 && c.VehicleByNetId(300)->poolHandle != ours,
	      "bob's is a car of its own");
	c.EndRejoinWaitForTest();
	c.TickLocalVehicle();
	Check(c.VehicleClaimPending(), "and ours is claimed as a new one");
}

void TestTheCarWeWereInNeverComesBack() {
	std::printf("\nthe car we were in never comes back\n");
	Client c;
	c.SetBridge(RecordingBridge());
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeRejoinSpawn(300, 90), CH_EVENT));
	c.Tick();
	const int32_t ours = c.VehicleByNetId(300)->poolHandle;
	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = ours;

	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.TickLocalVehicle();
	Check(!c.VehicleClaimPending(), "nothing is claimed while the backfill may still hand it back");
	c.EndRejoinWaitForTest();
	c.TickLocalVehicle();
	Check(c.VehicleClaimPending(), "and it is claimed as a new car once that has passed");

	S_EnterVehicle reply = MakeEnter(0, 305);
	c.HandleMessage(Wrap(reply, CH_EVENT));
	const int spawns = g_rec.vehicleSpawns;
	c.HandleMessage(Wrap(MakeRejoinSpawn(300, 90), CH_EVENT));
	c.Tick();
	Check(c.VehicleByNetId(305)->poolHandle == ours &&
	          c.VehicleByNetId(300)->poolHandle != ours && g_rec.vehicleSpawns == spawns + 1,
	      "an old number turning up later is some other car, built as one");
}

void TestLimbsFromNoSessionStayBehind() {
	std::printf("\nlimbs from no session stay behind\n");
	static uint32_t queued;
	queued = 20;
	WorldBridge b = RecordingBridge();
	b.DrainAmbientBodyParts = [](PedBodyPartBody *out, uint32_t max) -> uint32_t {
		const uint32_t n = queued < max ? queued : max;
		for (uint32_t i = 0; i < n; ++i)
			out[i] = PedBodyPartBody{INVALID_NETID, 2, 0};
		queued -= n;
		return n;
	};
	Client c;
	c.SetBridge(b);
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	Check(queued == 0, "a welcome empties the queue before our new netId can go on them");
}

bool g_pedSampleOk = true;
bool RecSampleRemotePedPosition(const RemotePlayer &, Vec3 &out) {
	if (!g_pedSampleOk)
		return false;
	out = Vec3{1.5f, 2.5f, 3.5f};
	return true;
}

void TestTheDesyncProbeSaysWhereOurCopiesAre() {
	std::printf("\nthe desync probe says where our copies are\n");
	WorldBridge b = RecordingBridge();
	b.SampleRemotePedPosition = &RecSampleRemotePedPosition;
	g_pedSampleOk             = true;
	Client c;
	c.SetBridge(b);
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	C_DesyncProbe probe;
	Check(c.BuildDesyncProbe(probe) == 0 && probe.count == 0, "nothing to say about nobody");

	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	for (uint32_t t = 4840; t <= 5040; t += 40)
		c.HandleMessage(Wrap(MakeState(1, t, 1.0f), CH_SNAPSHOT));
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	for (uint32_t t = 800; t <= 1000; t += 40) {
		S_VehicleState st = MakeVehicleState(2, 80, 10.0f);
		st.hdr.sendTimeMs = t;
		c.HandleMessage(Wrap(st, CH_SNAPSHOT));
	}
	c.Tick();
	c.Tick();
	Check(c.PlayerSlot(1).poolHandle >= 0 && c.VehicleByNetId(80)->poolHandle >= 0,
	      "(alice and a car of the session's, both built)");

	Check(c.BuildDesyncProbe(probe) == 2, "one row for alice and one for the car");
	const uint16_t aliceNet = c.PlayerSlot(1).netId;
	Check(probe.rows[0].netId == aliceNet && probe.rows[0].pos.x == 1.5f &&
	          probe.rows[0].atMs >= 4840 && probe.rows[0].atMs <= 5040,
	      "alice where our engine has her, at the instant of her clock we drew");
	Check(probe.rows[1].netId == 80 && probe.rows[1].pos.x == 11.0f && probe.rows[1].atMs <= 1000,
	      "the car where our engine has it, on its own driver's clock");

	g_pedSampleOk = false;
	Check(c.BuildDesyncProbe(probe) == 1 && probe.rows[0].netId == 80,
	      "a ped the engine is seating is left to its car");
	g_pedSampleOk = true;

	g_rec.drivingLocally     = true;
	g_rec.localVehicleHandle = c.VehicleByNetId(80)->poolHandle;
	Check(c.BuildDesyncProbe(probe) == 1 && probe.rows[0].netId == aliceNet,
	      "and a car we are driving is not ours to be wrong about");
	g_rec.drivingLocally = false;

	S_DesyncReport report;
	InitHeader(report, 6000);
	report.count          = 3;
	report.rows[0].netId  = aliceNet;
	report.rows[0].offCm  = 740;
	report.rows[1].netId  = 80;
	report.rows[1].offCm  = 30;
	report.rows[2].netId  = 999;
	report.rows[2].offCm  = 100;
	c.HandleMessage(Wrap(report, CH_SNAPSHOT));
	Check(c.DesyncOf(1) == 740, "the server's word on alice is kept");
	uint16_t    car  = 0;
	const char *kind = "";
	Check(c.WorstCopyDesync(car, kind) == 30 && car == 80 && std::strcmp(kind, "vehicle") == 0,
	      "and on the car");
	Check(c.DesyncOf(2) == DESYNC_UNKNOWN, "nobody else is said to be anywhere");

	report.rows[0].offCm = DESYNC_UNKNOWN;
	report.count         = 1;
	c.HandleMessage(Wrap(report, CH_SNAPSHOT));
	Check(c.DesyncOf(1) == 740, "a row the server could not compare leaves the last word");
}

void TestACarChangingHandsIsOnItsNewDriversClock() {
	std::printf("\na car changing hands is on its new driver's clock\n");
	Client c;
	c.SetBridge(RecordingBridge());
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(2, "bob"), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));

	// Alice's game has been running for an hour and a half.
	for (uint32_t t = 5400000; t <= 5400080; t += 40) {
		S_VehicleState st = MakeVehicleState(1, 80, 10.0f);
		st.hdr.sendTimeMs = t;
		c.HandleMessage(Wrap(st, CH_SNAPSHOT));
	}
	Check(c.VehicleByNetId(80)->interp.NewestTimeMs() == 5400080, "(alice's snapshots)");

	// Bob's game started a minute ago, and bob takes the wheel.
	c.HandleMessage(Wrap(MakeEnter(2, 80), CH_EVENT));
	S_VehicleState st = MakeVehicleState(2, 80, 14.0f);
	st.hdr.sendTimeMs = 60000;
	c.HandleMessage(Wrap(st, CH_SNAPSHOT));
	Check(c.VehicleByNetId(80)->interp.NewestTimeMs() == 60000 &&
	          c.VehicleByNetId(80)->interp.Size() == 1,
	      "bob's first snapshot is taken, not dropped as older than alice's");
	st.hdr.sendTimeMs = 60040;
	c.HandleMessage(Wrap(st, CH_SNAPSHOT));
	Check(c.VehicleByNetId(80)->interp.Size() == 2, "and so is the next");

	S_VehicleState late = MakeVehicleState(1, 80, 10.0f);
	late.hdr.sendTimeMs = 5400120;
	c.HandleMessage(Wrap(late, CH_SNAPSHOT));
	Check(c.VehicleByNetId(80)->interp.NewestTimeMs() == 60040 &&
	          c.VehicleByNetId(80)->interp.Size() == 2,
	      "a late one of alice's from before the change is not let back in");

	// Bob goes, and whoever gets the slot next starts a clock of their own.
	S_PlayerLeave leave;
	InitHeader(leave, 70000);
	leave.playerId = 2;
	c.HandleMessage(Wrap(leave, CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(2, "carol"), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(2, 80), CH_EVENT));
	st.hdr.sendTimeMs = 900;
	c.HandleMessage(Wrap(st, CH_SNAPSHOT));
	Check(c.VehicleByNetId(80)->interp.NewestTimeMs() == 900,
	      "the next player in bob's slot is not held to bob's clock");
}

void TestACopyHeldAtItsOldestIsNotProbed() {
	std::printf("\na copy held at its oldest sample is not probed\n");
	InterpBuffer b;
	b.Push(1000, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, Vec3{});
	Pose     pose;
	uint32_t at = 0;
	b.SampleDelayed(5000, pose);
	Check(!b.RenderedAt(at), "a fresh buffer drawn before its first sample says nothing");
	b.Push(1040, Vec3{1.0f, 0.0f, 0.0f}, 0.0f, Vec3{});
	b.Push(1080, Vec3{2.0f, 0.0f, 0.0f}, 0.0f, Vec3{});
	b.Push(1120, Vec3{3.0f, 0.0f, 0.0f}, 0.0f, Vec3{});
	for (uint32_t t = 5016; t <= 5200; t += 16)
		b.SampleDelayed(t, pose);
	Check(b.RenderedAt(at) && at >= 1000, "and says when once its clock is inside what it holds");
}

bool RecSampleReplicaPosition(int32_t, bool car, Vec3 &out) {
	out = car ? Vec3{7.0f, 8.0f, 9.0f} : Vec3{4.0f, 5.0f, 6.0f};
	return true;
}

void TestTheProbeTakesInTheCrowdAndTheTraffic() {
	std::printf("\nthe desync probe takes in the crowd and the traffic\n");
	WorldBridge b = RecordingBridge();
	b.SampleReplicaPosition = &RecSampleReplicaPosition;
	Client c;
	c.SetBridge(b);
	GiveUsAnAmbientPed(c, 500);
	for (uint32_t t = 1800; t <= 2000; t += 40)
		c.HandleMessage(Wrap(MakePedStates(1, 500, t, 77.0f), CH_SNAPSHOT));
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(610), CH_EVENT));
	S_CarStates cars{};
	InitHeader(cars, 3000);
	cars.ownerPlayerId  = 1;
	cars.count          = 1;
	cars.cars[0].netId  = 610;
	cars.cars[0].rot    = {0.0f, 0.0f, 0.0f, 1.0f};
	for (uint32_t t = 2800; t <= 3000; t += 40) {
		cars.hdr.sendTimeMs = t;
		c.HandleMessage(Wrap(cars, CH_SNAPSHOT));
	}
	c.Tick();
	c.Tick();
	Check(c.AmbientPed(500)->poolHandle >= 0 && c.AmbientCar(610)->poolHandle >= 0,
	      "(a pedestrian and a traffic car of somebody else's, both built)");

	C_DesyncProbe probe;
	Check(c.BuildDesyncProbe(probe) == 2, "both are in the probe");
	bool sawPed = false, sawCar = false;
	for (uint8_t i = 0; i < probe.count; ++i) {
		if (probe.rows[i].netId == 500 && probe.rows[i].pos.x == 4.0f && probe.rows[i].atMs <= 2000)
			sawPed = true;
		if (probe.rows[i].netId == 610 && probe.rows[i].pos.x == 7.0f && probe.rows[i].atMs <= 3000)
			sawCar = true;
	}
	Check(sawPed && sawCar, "each where our engine has it, on its host's clock");
	g_rec.ambientDrivenHandle = c.AmbientCar(610)->poolHandle;
	Check(c.BuildDesyncProbe(probe) == 1 && probe.rows[0].netId == 500,
	      "a traffic car we have taken the wheel of is ours, not a copy");
	g_rec.ambientDrivenHandle = -1;

	S_DesyncReport report;
	InitHeader(report, 6000);
	report.count         = 2;
	report.rows[0].netId = 500;
	report.rows[0].offCm = 120;
	report.rows[1].netId = 610;
	report.rows[1].offCm = 950;
	c.HandleMessage(Wrap(report, CH_SNAPSHOT));
	uint16_t    worst = 0;
	const char *what  = "";
	Check(c.WorstCopyDesync(worst, what) == 950 && worst == 610 &&
	          std::strcmp(what, "traffic car") == 0,
	      "and the furthest of them is the one named under the list");
}

bool g_pushedByUs = false;
bool RecVehiclePushedByUs(RemoteVehicle &) { return g_pushedByUs; }

void TestAShoveAsksToSettleAParkedCar() {
	std::printf("\na shove asks to settle a parked car\n");
	WorldBridge b = RecordingBridge();
	b.VehiclePushedByUs = &RecVehiclePushedByUs;
	g_pushedByUs        = false;
	Client c;
	c.SetBridge(b);
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.Tick();
	Check(c.VehicleByNetId(80)->poolHandle >= 0, "(a parked session car, nobody holding it)");

	c.Tick();
	Check(c.PushAsksForTest() == 0, "standing still, nothing is asked");
	g_pushedByUs = true;
	c.Tick();
	Check(c.PushAsksForTest() == 1, "our car shoving it asks for it");
	c.Tick();
	c.Tick();
	Check(c.PushAsksForTest() == 1, "once, not every frame of the same shove");

	Client driven;
	driven.SetBridge(b);
	driven.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	driven.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	driven.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	driven.HandleMessage(Wrap(MakeEnter(1, 80), CH_EVENT));
	driven.Tick();
	driven.Tick();
	Check(driven.PushAsksForTest() == 0, "and a car somebody is driving is theirs to move");

	// A wreck is nobody's to move.
	Client wreck;
	wreck.SetBridge(b);
	wreck.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	wreck.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	g_pushedByUs = false;
	wreck.Tick();
	const_cast<RemoteVehicle *>(wreck.VehicleByNetId(80))->destroyed = true;
	g_pushedByUs = true;
	wreck.Tick();
	Check(wreck.PushAsksForTest() == 0, "a wreck is not asked for");

	// Granted: every frame of the shove starts the settle again.
	c.HandleMessage(Wrap(MakeCustody(80, /*us=*/0), CH_EVENT));
	RemoteVehicle *const held = const_cast<RemoteVehicle *>(c.VehicleByNetId(80));
	held->settleEndsAtMs      = 1;
	c.TickCustody();
	Check(held->settleEndsAtMs - held->holdUntilMs == VEHICLE_SETTLE_MS - CUSTODY_HIT_HOLD_MS,
	      "shoved while we settle it, the window starts again from this frame");
	Check(!held->settleReported, "and it is not handed back while it is pushed");
	g_pushedByUs = false;
}

void TestWireMotionIsHeld() {
	std::printf("\na car's motion off the wire is held\n");
	const Vec3 slow = HeldMoveSpeed(Vec3{1.0f, 0.5f, 0.0f});
	Check(slow.x == 1.0f && slow.y == 0.5f, "a real speed goes through untouched");
	const Vec3 wild = HeldMoveSpeed(Vec3{3.0e7f, 4.0e7f, 0.0f});
	Check(std::fabs(std::sqrt(wild.x * wild.x + wild.y * wild.y) - WIRE_MOVE_MAX) < 0.01f &&
	          wild.x > 0.0f && wild.y > wild.x,
	      "a wild one keeps its direction and is held to the bound");
	Check(HeldWithin(0.4f, 1.0f) == 0.4f && HeldWithin(9.0f, 1.0f) == 1.0f &&
	          HeldWithin(-9.0f, 1.0f) == -1.0f,
	      "pedals and turn rates are held either way");
}

void TestACarsOwnBlastIsNotRelayed() {
	std::printf("\na car's own blast is not relayed\n");
	Check(!RelaysLocalExplosion(EXPLOSION_CAR) && !RelaysLocalExplosion(EXPLOSION_CAR_QUICK),
	      "a car our player blew up goes out on its wreck's own packet, not as an explosion too");
	Check(RelaysLocalExplosion(EXPLOSION_GRENADE) && RelaysLocalExplosion(EXPLOSION_ROCKET) &&
	          RelaysLocalExplosion(EXPLOSION_BARREL),
	      "while our grenades, rockets and barrels still do");
	Check(!RelaysLocalExplosion(EXPLOSION_TYPE_COUNT), "and nothing unknown");
}

void TestANudgeIsAPush() {
	std::printf("\na nudge is a push\n");
	Check(!MovedByPush(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}),
	      "a car left standing is not being pushed");
	Check(MovedByPush(Vec3{0.005f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}),
	      "one moving across the ground at a quarter of a metre a second is");
	Check(VehicleAtRestNumbers(0.005f * 0.005f, 0.0f, 0.0f, 0.0f),
	      "(which the rest test would call at rest)");
	Check(!MovedByPush(Vec3{0.003f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}),
	      "one creeping at less than a fifth of a metre a second is not");
	Check(!MovedByPush(Vec3{0.0f, 0.0f, -0.02f}, Vec3{0.0f, 0.0f, 0.0f}),
	      "nor one only falling, which gravity does to every pinned car each step");
	Check(!MovedByPush(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.01f, -0.01f, 0.0f}),
	      "nor one rocking on its springs against ours");
	Check(MovedByPush(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.002f}),
	      "and one the shove has turned is");
}

void TestALateExitEchoKeepsOurNewCar() {
	std::printf("\na late exit echo keeps our new car\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(0, 80), CH_EVENT));
	Check(c.LocalVehicleNetId() == 80, "(our claim came back as car 80)");
	c.HandleMessage(Wrap(MakeExit(0, 70), CH_EVENT));
	Check(c.LocalVehicleNetId() == 80, "the exit from the car before it leaves the name alone");
	c.HandleMessage(Wrap(MakeExit(0, 80), CH_EVENT));
	Check(c.LocalVehicleNetId() == INVALID_NETID, "and the one for this car is ours");
}

int32_t RecHostedCarHandle(uint16_t netId) { return netId == 90 ? 44 : -1; }

void TestOurOwnTrafficPromotedIsNotBuiltAgain() {
	std::printf("\nour own traffic, promoted, is not built again\n");
	WorldBridge b = RecordingBridge();
	b.HostedCarHandle = &RecHostedCarHandle;
	Client c;
	c.SetBridge(b);
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.HandleMessage(Wrap(MakeCarPromoted(90, /*driver=*/1, /*wasOwner=*/0), CH_EVENT));
	const RemoteVehicle *v = c.VehicleByNetId(90);
	Check(v && v->poolHandle == 44 && !v->spawnPending,
	      "the car our engine made is the session car now, and nothing is built");
	Check(v && v->ours, "and it is never ours to destroy");
}

int g_namesRestarted = 0;
void RecRestartHostedNames() { ++g_namesRestarted; }

void TestAWelcomeAnnouncesOurCrowdAgain() {
	std::printf("\na welcome announces our crowd again\n");
	WorldBridge b = RecordingBridge();
	b.RestartHostedNames = &RecRestartHostedNames;
	Client c;
	c.SetBridge(b);
	g_namesRestarted = 0;
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	Check(g_namesRestarted == 1,
	      "whatever the last session called our peds and cars, this one hears of them anew");
}

int g_takenBack = 0;
void RecTakeVehicleBack(RemoteVehicle &) { ++g_takenBack; }

void TestACarWeHoldIsOursToDent() {
	std::printf("\na car we hold is ours to dent\n");
	WorldBridge b = RecordingBridge();
	b.TakeVehicleBack = &RecTakeVehicleBack;
	Client c;
	c.SetBridge(b);
	ParkOneCar(c);
	g_takenBack = 0;
	c.Tick();
	Check(g_takenBack == 0, "a parked copy stays proof against our collisions");
	c.HandleMessage(Wrap(MakeCustody(80, /*us=*/0), CH_EVENT));
	c.Tick();
	Check(g_takenBack == 1, "one we settle is ours to dent, every frame");
	c.HandleMessage(Wrap(MakeCustody(80, INVALID_PLAYER), CH_EVENT));
	c.Tick();
	Check(g_takenBack == 1, "and handed back it is a copy again");
}

bool g_sinking = false;
bool RecVehicleSinking(RemoteVehicle &) { return g_sinking; }

void TestASinkingCarIsKeptUntilTheBottom() {
	std::printf("\na sinking car is kept until it lies on the bottom\n");
	Check(StillSinking(Vec3{0.0f, 0.0f, -0.003f}), "going down at a seventh of a metre a second is sinking");
	Check(game::VehicleAtRestNumbers(0.003f * 0.003f, 0.0f, 0.0f, 0.0f),
	      "(which the rest test calls at rest)");
	Check(StillSinking(Vec3{0.005f, 0.0f, 0.0f}), "and so is drifting along under water");
	Check(!StillSinking(Vec3{0.0f, 0.0f, -0.001f}) && !StillSinking(Vec3{0.0f, 0.0f, 0.0f}),
	      "lying on the bottom, or all but, is not");

	WorldBridge b = RecordingBridge();
	b.VehicleSinking = &RecVehicleSinking;
	Client c;
	c.SetBridge(b);
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeCustody(80, /*us=*/0), CH_EVENT));
	RemoteVehicle *const v = const_cast<RemoteVehicle *>(c.VehicleByNetId(80));
	g_rec.carAtRest = true;
	g_sinking       = true;
	v->settleEndsAtMs = 1;
	for (int i = 0; i < VEHICLE_REST_FRAMES + 2; ++i)
		c.TickCustody();
	Check(!v->settleReported, "going down, it is kept however still the rest test thinks it is");
	const uint32_t since = v->sinkSinceMs;
	Check(since != 0, "(and when it started going down is noted)");
	g_sinking = false;
	v->holdUntilMs = WallClock::NowMs() + 60000;
	c.TickCustody();
	g_sinking = true;
	c.TickCustody();
	Check(v->sinkSinceMs == since, "a frame out of the test does not start the count again");

	v->sinkSinceMs = WallClock::NowMs() - CUSTODY_SINK_CAP_MS - 10;
	v->holdUntilMs = 0;
	c.TickCustody();
	Check(v->settleReported, "and past the cap it is given back all the same");

	c.HandleMessage(Wrap(MakeCustody(80, INVALID_PLAYER), CH_EVENT));
	Check(v->sinkSinceMs == 0, "the count goes with the custody");
	c.HandleMessage(Wrap(MakeCustody(80, 0), CH_EVENT));
	Check(v->sinkSinceMs == 0, "and a new one starts without it");
	g_sinking       = false;
	g_rec.carAtRest = false;
}

void TestABlastOnAParkedCarStays() {
	std::printf("\na blast on a parked car stays\n");
	Check(HealthToWrite(1000.0f, true, 620.0f) == 620.0f,
	      "a car nobody holds keeps what the explosion took off it");
	Check(HealthToWrite(700.0f, true, 1000.0f) == 700.0f,
	      "and never comes back above the session's word");
	Check(HealthToWrite(1000.0f, false, 0.0f) == 1000.0f,
	      "with no blast the session's word is written back over anything else");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(HealthToWrite(nan, false, 0.0f) == 1000.0f && HealthToWrite(700.0f, true, nan) == 700.0f,
	      "and nothing that is not a number is written");

	// The table the detour writes it into.
	game::ObservedTable<4> t;
	Check(t.Remember(12, 80, &FakeVehicleAt), "a row for car 80");
	const void *car = FakeVehicleAt(12);
	t.NoteBlast(car, 620.0f, &FakeVehicleAt);
	const game::ObservedRow *row = t.Find(car, &FakeVehicleAt);
	Check(row && row->blasted && row->blastHealth == 620.0f, "the blast is kept on the row");
	t.NoteBlast(car, 400.0f, &FakeVehicleAt);
	Check(row->blastHealth == 400.0f, "a second blast takes it lower");
	t.NoteHolders(80, 0xFF, 1, false);
	Check(row->blasted, "somebody taking the car does not end it on its own");
	t.NoteHolders(80, 0xFF, 1, false, /*blastFloorEnds=*/true);
	Check(!row->blasted, "their word on it does");
	t.NoteBlast(FakeVehicleAt(13), 100.0f, &FakeVehicleAt);
	Check(!row->blasted, "a blast on a car with no row marks nothing");
	Check(t.Remember(12, 80, &FakeVehicleAt) && !t.Find(car, &FakeVehicleAt)->blasted,
	      "and a fresh row starts without one");

	// Through the client: nobody holds it, then somebody does and has not
	// said anything yet, then they have.
	Client c;
	c.SetBridge(RecordingBridge());
	ParkOneCar(c);
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	c.Tick();
	const int32_t handle = c.VehicleByNetId(80)->poolHandle;
	auto rowHere = [&]() { return g_observedHere.Find(FakeVehicleAt(handle), &FakeVehicleAt); };
	g_observedHere.NoteBlast(FakeVehicleAt(handle), 500.0f, &FakeVehicleAt);
	c.Tick();
	Check(rowHere() && rowHere()->blasted, "a blast on a parked car stays over a frame");

	c.HandleMessage(Wrap(MakeCustody(80, 1), CH_EVENT));
	c.Tick();
	Check(rowHere()->blasted,
	      "and while its new custodian has said nothing, the session's older word does not undo it");
	S_VehicleState state = MakeVehicleState(1, 80, 0.0f);
	state.body.health    = 500.0f;
	c.HandleMessage(Wrap(state, CH_SNAPSHOT));
	c.Tick();
	Check(!rowHere()->blasted, "the custodian's first snapshot is the car's health from then on");

	// The same player again, after a spell with nobody.
	c.HandleMessage(Wrap(MakeCustody(80, INVALID_PLAYER), CH_EVENT));
	c.Tick();
	g_observedHere.NoteBlast(FakeVehicleAt(handle), 300.0f, &FakeVehicleAt);
	c.HandleMessage(Wrap(MakeCustody(80, 1), CH_EVENT));
	c.Tick();
	Check(rowHere()->blasted,
	      "a custodian heard from last time has to be heard from again");
	c.HandleMessage(Wrap(state, CH_SNAPSHOT));
	c.Tick();
	Check(!rowHere()->blasted, "and is");

	// Ours to settle: our own engine says from then on.
	c.HandleMessage(Wrap(MakeCustody(80, INVALID_PLAYER), CH_EVENT));
	c.Tick();
	g_observedHere.NoteBlast(FakeVehicleAt(handle), 300.0f, &FakeVehicleAt);
	c.HandleMessage(Wrap(MakeCustody(80, /*us=*/0), CH_EVENT));
	c.Tick();
	Check(!rowHere()->blasted, "a car we settle ends it at once");
}

void TestQuietCountsFromTheJoin() {
	std::printf("\nquiet counts from the join\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	const uint32_t before = WallClock::NowMs();
	S_PlayerJoin alice = MakeJoin(1, "alice");
	alice.flags |= PJF_POS_VALID;
	c.HandleMessage(Wrap(alice, CH_EVENT));
	Check(c.PlayerSlot(1).haveState && c.PlayerSlot(1).heardAtMs >= before,
	      "somebody who has not sent a snapshot yet has been quiet since they were announced");
}

void TestBeingTurnedAwaySaysWhy() {
	std::printf("\nbeing turned away says why\n");
	Client c;
	c.SetBridge(RecordingBridge());
	S_Welcome no = MakeWelcome(0);
	no.reject = REJECT_BAD_PASSWORD;
	c.HandleMessage(Wrap(no, CH_EVENT));
	Check(c.Feed().Count() == 1 &&
	          FeedLineIs(c, 0, FeedKind::Notice,
	                     "the server wants a password (CoopIII.ini, password)"),
	      "a wrong or missing password is on the HUD, with where to put it");
	no.reject = REJECT_FULL;
	c.HandleMessage(Wrap(no, CH_EVENT));
	Check(FeedLineIs(c, 1, FeedKind::Notice, "the server is full; asking again in a while"),
	      "and a full server says it will be asked again");
	Check(c.LocalPlayerId() >= MAX_PLAYERS, "and nothing of the session is taken from either");
}

void TestThePingsAreTheServers() {
	std::printf("\nthe pings are the server's\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	Check(c.PingOf(0) == PING_NONE && c.PingOf(1) == PING_NONE, "nothing before it has said");

	S_PlayerPings p;
	InitHeader(p, 1000);
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		p.rttMs[id] = PING_NONE;
	p.rttMs[0] = 31;
	p.rttMs[1] = 87;
	c.HandleMessage(Wrap(p, CH_SNAPSHOT));
	Check(c.PingOf(0) == 31 && c.PingOf(1) == 87 && c.PingOf(2) == PING_NONE,
	      "ours and alice's, and nobody's for an empty slot");
	Check(c.PingOf(INVALID_PLAYER) == PING_NONE, "and nothing for a slot that is not one");
}

// The seam the typed line leaves by. Nothing goes out without a socket, but
// the line is taken, once, and nothing is taken when nothing was typed.
int  g_typedTakes = 0;
bool g_typedReady = false;

void TestATypedLineIsTakenOnce() {
	std::printf("\na typed line is taken once\n");
	Client c;
	WorldBridge bridge = RecordingBridge();
	g_typedTakes = 0;
	g_typedReady = true;
	bridge.TakeTypedChat = [](char (&out)[CHAT_LEN]) -> bool {
		++g_typedTakes;
		if (!g_typedReady)
			return false;
		g_typedReady = false;
		std::strcpy(out, "on my way");
		return true;
	};
	c.SetBridge(bridge);
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.TickChatForTest();
	c.TickChatForTest();
	Check(g_typedTakes == 2 && !g_typedReady, "asked each frame, and the line was taken");
	Check(c.Feed().Count() == 1, "our own line waits for the server to relay it");
}

// ---- a traffic car's dents, from its host (Session::NoteCarDamage) ---------

S_VehicleDamage MakeDent(uint16_t netId, uint8_t from, unsigned panel, uint8_t level,
                         unsigned door = 0, uint8_t doorLevel = 0) {
	S_VehicleDamage d{};
	InitHeader(d, 1500);
	d.playerId   = from;
	d.body.netId = netId;
	SetPanelLevel(d.body.panels, panel, level);
	if (doorLevel)
		SetDoorLevel(d.body.doors, door, doorLevel);
	return d;
}

void TestTrafficDentsThroughTheClient() {
	std::printf("\na traffic car's dents, through the client\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(640, /*owner=*/1), CH_EVENT));

	// Before the replica exists: the row keeps them.
	c.HandleMessage(Wrap(MakeDent(640, 1, 2, 2), CH_EVENT));
	Check(g_rec.ambientCarDents == 0 && GetPanelLevel(c.AmbientCar(640)->damagePanels, 2) == 2,
	      "a dent before the replica is built waits on its row");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.ambientCarSpawns == 1 && g_rec.ambientCarDents == 1 &&
	          !g_rec.lastAmbientCarDentFlying &&
	          GetPanelLevel(g_rec.lastAmbientCarDent.panels, 2) == 2,
	      "and the replica arrives wearing it, with no parts flying");

	c.HandleMessage(Wrap(MakeDent(640, 1, 0, 1, 3, 2), CH_EVENT));
	Check(g_rec.ambientCarDents == 2 && g_rec.lastAmbientCarDentFlying &&
	          GetPanelLevel(g_rec.lastAmbientCarDent.panels, 2) == 2 &&
	          GetPanelLevel(g_rec.lastAmbientCarDent.panels, 0) == 1 &&
	          GetDoorLevel(g_rec.lastAmbientCarDent.doors, 3) == 2,
	      "a new one in front of us flies, on top of the old");

	c.HandleMessage(Wrap(MakeDent(640, 1, 2, 1), CH_EVENT));
	Check(g_rec.ambientCarDents == 2, "a milder one says nothing new");

	S_VehicleDamage reset = MakeDent(640, 1, 0, 0);
	reset.body.panels = VEH_DAMAGE_RESET;
	c.HandleMessage(Wrap(reset, CH_EVENT));
	Check(GetPanelLevel(c.AmbientCar(640)->damagePanels, 2) == 2,
	      "and traffic is never resprayed, so a repair marker is not believed");

	c.HandleMessage(Wrap(MakeCarPromoted(640, /*driver=*/2, /*wasOwner=*/1), CH_EVENT));
	const RemoteVehicle *v = c.VehicleByNetId(640);
	Check(v != nullptr && GetPanelLevel(v->damagePanels, 2) == 2 &&
	          GetDoorLevel(v->damageDoors, 3) == 2 && !v->damagePending,
	      "somebody taking the wheel keeps them, on a car that already wears them");

	c.HandleMessage(Wrap(MakeAmbientCarSpawn(641, /*owner=*/1), CH_EVENT));
	c.HandleMessage(Wrap(MakeDent(641, 1, 1, 3), CH_EVENT));
	const RemoteAmbientCar *unbuilt = c.AmbientCar(641);
	Check(unbuilt != nullptr && unbuilt->poolHandle < 0, "(a second car, not built yet)");
	c.HandleMessage(Wrap(MakeCarPromoted(641, /*driver=*/2, /*wasOwner=*/1), CH_EVENT));
	const RemoteVehicle *later = c.VehicleByNetId(641);
	Check(later != nullptr && later->damagePending &&
	          GetPanelLevel(later->damagePanels, 1) == 3,
	      "and a car still to be built puts them on after its spawn");
}

void TestOurTrafficsDentsGoOut() {
	std::printf("\nour own traffic's dents go out\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	const uint32_t before = c.DamageReportsSentForTest();
	g_rec.hostedDents[0]  = MakeDent(700, 0, 1, 2).body;
	g_rec.hostedDents[1]  = MakeDent(701, 0, 0, 1).body;
	g_rec.hostedDentCount = 2;
	c.TickHostedCarDamageForTest();
	Check(c.DamageReportsSentForTest() == before + 2 && g_rec.hostedDentCount == 0,
	      "every dent the engine seam hands over is reported");
	c.TickHostedCarDamageForTest();
	Check(c.DamageReportsSentForTest() == before + 2, "and nothing when it hands over none");
}

// ---------------------------------------------------------------------------
// A leaver's crowd (protocol.h, S_AmbientAdopt)
// ---------------------------------------------------------------------------

S_AmbientAdopt MakeAdopt(uint8_t wasOwner) {
	S_AmbientAdopt a{};
	InitHeader(a, 1000);
	a.wasOwnerPlayerId = wasOwner;
	a.count            = 0;
	return a;
}

void AddAdoptRow(S_AmbientAdopt &a, uint16_t netId, uint8_t kind, uint8_t newOwner) {
	a.rows[a.count++] = AmbientAdoptRow{netId, kind, newOwner};
}

// A civilian, which is what can be handed on. The default spawn helper's
// pedType 0 is PLAYER1, and nobody can adopt one of those.
void GiveUsACivilian(Client &c, uint16_t netId, uint8_t owner = 1) {
	S_PedSpawn s = MakeAmbientPedSpawn(netId, owner);
	s.body.pedType = AMBIENT_PEDTYPE_CIVMALE;
	c.HandleMessage(Wrap(s, CH_EVENT));
}

void TestALeaversPedIsAdoptedByUs() {
	std::printf("\na leaver's pedestrian handed to us\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	GiveUsACivilian(c, 500);
	c.Tick();
	Check(g_rec.ambientPedSpawns == 1, "we hold a replica of alice's pedestrian");

	S_AmbientAdopt a = MakeAdopt(1);
	AddAdoptRow(a, 500, AMBIENT_ADOPT_PED, 0);
	c.HandleMessage(Wrap(a, CH_EVENT));
	Check(g_rec.pedAdoptions == 1, "the engine seam is asked to make him ours");
	Check(c.AmbientPed(500) == nullptr, "and he is off the replica roster");
	Check(g_rec.ambientPedDespawns == 0, "without the object being destroyed");

	c.Tick();
	Check(g_rec.ambientPedSpawns == 1, "and nothing builds him again");
}

void TestSomebodyElseAdoptsAndWeFollow() {
	std::printf("\na leaver's pedestrian handed to somebody else\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	GiveUsACivilian(c, 500);
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 500, 2000, 30.0f), CH_SNAPSHOT));

	S_AmbientAdopt a = MakeAdopt(1);
	AddAdoptRow(a, 500, AMBIENT_ADOPT_PED, 2);
	c.HandleMessage(Wrap(a, CH_EVENT));
	const RemoteAmbientPed *p = c.AmbientPed(500);
	Check(p != nullptr && p->ownerPlayerId == 2 && p->poolHandle >= 0,
	      "the replica stays and bob owns the row");
	Check(g_rec.pedAdoptions == 0 && g_rec.ambientPedDespawns == 0,
	      "nothing converted, nothing destroyed");

	c.HandleMessage(Wrap(MakePedStates(1, 500, 2040, 99.0f), CH_SNAPSHOT));
	Check(c.AmbientPed(500)->last.pos.x == 30.0f, "a late row from alice is not taken");
	// Stamped on bob's clock, far behind anything alice sent.
	c.HandleMessage(Wrap(MakePedStates(2, 500, 50, 44.0f), CH_SNAPSHOT));
	Check(c.AmbientPed(500)->last.pos.x == 44.0f, "bob's are");
	const int applies = g_rec.ambientApplies;
	c.Tick();
	Check(g_rec.ambientApplies == applies + 1 && g_rec.lastAmbientPose.pos.x == 44.0f,
	      "and drive the replica, alice's rows gone from its buffer");
}

void TestWhatWeCannotTakeIsLetGo() {
	std::printf("\nwhat we cannot take is let go of\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	// A cop, whose replica here is a civilian and so not the same man.
	g_rec.modelReady = true;
	S_PedSpawn cop = MakeAmbientPedSpawn(502);
	cop.body.pedType = 6;
	c.HandleMessage(Wrap(cop, CH_EVENT));
	c.Tick();
	// And a civilian whose model has not come in, so no replica at all.
	g_rec.modelReady = false;
	GiveUsACivilian(c, 503);
	c.Tick();
	const int spawns    = g_rec.ambientPedSpawns;
	const int despawns  = g_rec.ambientPedDespawns;

	S_AmbientAdopt a = MakeAdopt(1);
	AddAdoptRow(a, 503, AMBIENT_ADOPT_PED, 0);
	AddAdoptRow(a, 502, AMBIENT_ADOPT_PED, 0);
	AddAdoptRow(a, 777, AMBIENT_ADOPT_PED, 0);   // a row we never had
	c.HandleMessage(Wrap(a, CH_EVENT));
	Check(g_rec.pedAdoptions == 0, "none of the three is converted");
	Check(c.AmbientPed(503) == nullptr && c.AmbientPed(502) == nullptr,
	      "the rows go - we are their owner now and we let them go");
	Check(g_rec.ambientPedDespawns == despawns + 1,
	      "the one with a replica has it taken away, the one without has nothing to take");

	g_rec.modelReady = true;
	c.Tick();
	Check(g_rec.ambientPedSpawns == spawns, "and neither is built again");

	// The seam itself saying no - its table is full - ends the same way.
	GiveUsACivilian(c, 504);
	c.Tick();
	g_rec.refuseAdoption = true;
	S_AmbientAdopt b = MakeAdopt(1);
	AddAdoptRow(b, 504, AMBIENT_ADOPT_PED, 0);
	c.HandleMessage(Wrap(b, CH_EVENT));
	Check(c.AmbientPed(504) == nullptr && g_rec.ambientPedDespawns == despawns + 2,
	      "a refusal is a release");
}

void TestACarAndItsDriverAreTakenTogether() {
	std::printf("\na car and its driver handed to us in one packet\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(700), CH_EVENT));
	GiveUsACivilian(c, 500);
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 500, 2000, 40.0f, 0, 700, 0), CH_SNAPSHOT));
	c.Tick();
	Check(c.AmbientPed(500) && c.AmbientPed(500)->Seated(), "alice's driver sits in her car");
	const int unseats = g_rec.ambientUnseats;

	// The car row first on the wire, the way the server packs a group.
	S_AmbientAdopt a = MakeAdopt(1);
	AddAdoptRow(a, 700, AMBIENT_ADOPT_CAR, 0);
	AddAdoptRow(a, 500, AMBIENT_ADOPT_PED, 0);
	c.HandleMessage(Wrap(a, CH_EVENT));
	Check(g_rec.carAdoptions == 1 && g_rec.pedAdoptions == 1, "both are made ours");
	Check(g_rec.lastPedAdoptedAt < g_rec.lastCarAdoptedAt,
	      "the driver before the car, whatever order they came in");
	Check(g_rec.ambientUnseats == unseats, "and nobody is taken out of a seat to do it");
	Check(c.AmbientCar(700) == nullptr && c.AmbientPed(500) == nullptr &&
	          g_rec.ambientCarDespawns == 0,
	      "both rows gone, neither object destroyed");
}

void TestAnObserverKeepsTheDriverSeated() {
	std::printf("\na car and its driver handed to somebody else\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(700), CH_EVENT));
	GiveUsACivilian(c, 500);
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 500, 2000, 40.0f, 0, 700, 0), CH_SNAPSHOT));
	c.Tick();
	const int unseats = g_rec.ambientUnseats;

	S_AmbientAdopt a = MakeAdopt(1);
	AddAdoptRow(a, 700, AMBIENT_ADOPT_CAR, 2);
	AddAdoptRow(a, 500, AMBIENT_ADOPT_PED, 2);
	c.HandleMessage(Wrap(a, CH_EVENT));
	c.Tick();
	Check(c.AmbientCar(700)->ownerPlayerId == 2 && c.AmbientPed(500)->ownerPlayerId == 2,
	      "both rows are bob's");
	Check(c.AmbientPed(500)->Seated() && g_rec.ambientUnseats == unseats,
	      "and the driver never leaves his seat - the seat pass sees one owner");
}

void TestTheCarWeAreDrivingWaitsForItsClaim() {
	std::printf("\nhanded the car we are at the wheel of\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(300), CH_EVENT));
	c.Tick();
	const int32_t handle = c.AmbientCar(300)->poolHandle;
	g_rec.ambientDrivenHandle = handle;
	c.TickAmbientClaims();

	S_AmbientAdopt a = MakeAdopt(1);
	AddAdoptRow(a, 300, AMBIENT_ADOPT_CAR, 0);
	c.HandleMessage(Wrap(a, CH_EVENT));
	const RemoteAmbientCar *car = c.AmbientCar(300);
	Check(g_rec.carAdoptions == 0 && g_rec.ambientCarDespawns == 0,
	      "neither converted nor destroyed with us in it");
	Check(car && car->ownerPlayerId == 0 && car->poolHandle == handle,
	      "the replica waits, filed as ours");

	c.HandleMessage(Wrap(MakeCarPromoted(300, 0, 0), CH_EVENT));
	Check(g_rec.promotedAdoptions == 1 && !g_rec.lastPromotionWasOurs,
	      "the promotion treats it as the replica it is, not our engine's own car");
	Check(c.AmbientCar(300) == nullptr, "and the traffic row is gone");
}

void TestAnAdoptPacketIsBounded() {
	std::printf("\nan adoption packet is read within its own bounds\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	GiveUsACivilian(c, 500);
	c.Tick();

	S_AmbientAdopt a = MakeAdopt(1);
	AddAdoptRow(a, 500, 9, 0);                       // a kind this build has never heard of
	AddAdoptRow(a, 500, AMBIENT_ADOPT_PED, 200);     // nobody's slot
	c.HandleMessage(Wrap(a, CH_EVENT));
	Check(c.AmbientPed(500) && c.AmbientPed(500)->ownerPlayerId == 1 &&
	          g_rec.pedAdoptions == 0,
	      "an unknown kind and a slot that cannot exist change nothing");

	a.count = 255;
	c.HandleMessage(Wrap(a, CH_EVENT));
	Check(c.AmbientPed(500) != nullptr, "and a count past the array reads only the array");
}

// tools/clienttest/adopt.cpp
int RunAdoptTests();

// tools/clienttest/rampagevote.cpp
int RunRampageVoteTests();

// tools/clienttest/aimpitch.cpp
int RunAimPitchTests();

// tools/clienttest/siren.cpp
int RunSirenTests();

// tools/clienttest/cheats.cpp
int RunCheatTests();

// tools/clienttest/driveby.cpp
int RunDriveByTests();

// tools/clienttest/money.cpp
int RunMoneyTests();

// tools/clienttest/streampick.cpp
int RunStreamPickTests();

// tools/clienttest/chatfeed.cpp
int RunChatFeedTests();

// ---- who the ambient batches are ranked for (game/streampick.h) ------------

// The batches go to everybody else, so they are ranked by where everybody else
// is - never by us, and never by a player whose position nobody has told us.
void TestTheBatchesAreRankedByWhereTheOthersAre() {
	std::printf("\nwhere the other players are, for the ambient batches\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	Vec3 at[MAX_PLAYERS];
	Check(c.ViewerPositions(at, MAX_PLAYERS) == 0, "alone: nobody to rank for");

	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	Check(c.ViewerPositions(at, MAX_PLAYERS) == 0,
	      "a join with no position counts for nothing, not for the origin");

	c.HandleMessage(Wrap(MakeState(1, 1100, 42.0f), CH_SNAPSHOT));
	Check(c.ViewerPositions(at, MAX_PLAYERS) == 1 && at[0].x == 42.0f,
	      "her first snapshot places her");
	c.HandleMessage(Wrap(MakeState(1, 1200, 50.0f), CH_SNAPSHOT));
	Check(c.ViewerPositions(at, MAX_PLAYERS) == 1 && at[0].x == 50.0f,
	      "and the newest one moves her");

	c.HandleMessage(Wrap(MakeState(0, 1300, 900.0f), CH_SNAPSHOT));
	Check(c.ViewerPositions(at, MAX_PLAYERS) == 1 && at[0].x == 50.0f,
	      "our own echoed snapshot is not a viewer");

	S_PlayerJoin bob = MakeJoin(2, "bob");
	bob.flags        = PJF_POS_VALID;
	bob.pos          = {-7.0f, 8.0f, 9.0f};
	c.HandleMessage(Wrap(bob, CH_EVENT));
	Check(c.ViewerPositions(at, MAX_PLAYERS) == 2, "a join that says where he is counts at once");
	Check(c.ViewerPositions(at, 1) == 1, "and never more than there is room for");

	S_PlayerLeave leave;
	InitHeader(leave, 2000);
	leave.playerId = 1;
	leave.reason   = LEAVE_QUIT;
	c.HandleMessage(Wrap(leave, CH_EVENT));
	Check(c.ViewerPositions(at, MAX_PLAYERS) == 1 && at[0].x == -7.0f,
	      "somebody who left is nobody's viewer");
}

// ---- the police helicopter (helisync.h, game/heli.h) -----------------------

// A recording HeliBridge. Plain function pointers, so the state is a global,
// the same as g_rec.
struct HeliRec {
	// The owner's engine.
	std::vector<OwnHeliSample> own;
	std::vector<OwnHeliGone>   gone;
	int          applies        = 0;
	bool         applyTakes     = true;
	uint8_t      lastApplySlot  = 0xFF;
	int32_t      lastApplyHandle = -1;
	uint8_t      lastAttacker   = 0xFF;
	HeliHitBody  lastApplied{};
	int          resets         = 0;

	// The shooter's.
	std::vector<LocalHeliHit> hits;
	int     hitDrains = 0;
	int     credits   = 0;
	int     creditsWithoutStatistics = 0;
	uint8_t lastCreditSlot = 0xFF;

	// The observer's.
	bool    modelReady  = true;
	int     modelAsks   = 0;
	int     spawns      = 0;
	int     despawns    = 0;
	int     poses       = 0;
	bool    loseNextPose = false;
	VehicleTransform lastPose{};
	HeliLook         lastLook{};
	int     tails       = 0;
	int     explodes    = 0;
	Vec3    lastExplodeAt{};
	bool    lastExplodeHadReplica = false;
	int     despawnsAtLastExplode = -1;
	int     nextHandle  = 500;

	// The gun: the owner's rounds, and the observer's drawing of them.
	std::vector<OwnHeliShot> ownShots;
	int     draws       = 0;
	bool    drawTakes   = true;
	int32_t lastDrawHandle = -1;
	Vec3    lastDrawSource{};
	Vec3    lastDrawTarget{};

	// What went out, in order.
	std::vector<Message> sent;
};
HeliRec g_heli;

uint8_t HeliRecSample(OwnHeliSample *out, uint8_t max) {
	uint8_t n = 0;
	for (const OwnHeliSample &s : g_heli.own)
		if (n < max)
			out[n++] = s;
	return n;
}
uint8_t HeliRecDrainGone(OwnHeliGone *out, uint8_t max) {
	uint8_t n = 0;
	for (const OwnHeliGone &g : g_heli.gone)
		if (n < max)
			out[n++] = g;
	g_heli.gone.clear();
	return n;
}
bool HeliRecApply(uint8_t slot, int32_t handle, uint8_t attacker, const HeliHitBody &hit) {
	++g_heli.applies;
	g_heli.lastApplySlot   = slot;
	g_heli.lastApplyHandle = handle;
	g_heli.lastAttacker    = attacker;
	g_heli.lastApplied     = hit;
	return g_heli.applyTakes;
}
void HeliRecReset() { ++g_heli.resets; }
uint8_t HeliRecDrainHits(LocalHeliHit *out, uint8_t max) {
	++g_heli.hitDrains;
	uint8_t n = 0;
	for (const LocalHeliHit &h : g_heli.hits)
		if (n < max)
			out[n++] = h;
	g_heli.hits.clear();
	return n;
}
void HeliRecCredit(uint8_t slot, const Vec3 &, bool statistics) {
	++g_heli.credits;
	if (!statistics)
		++g_heli.creditsWithoutStatistics;
	g_heli.lastCreditSlot = slot;
}
bool HeliRecModel() {
	++g_heli.modelAsks;
	return g_heli.modelReady;
}
bool HeliRecSpawn(RemoteHeli &h) {
	++g_heli.spawns;
	h.poolHandle = g_heli.nextHandle++;
	return true;
}
void HeliRecDespawn(RemoteHeli &h) {
	++g_heli.despawns;
	h.poolHandle = -1;
}
bool HeliRecPose(RemoteHeli &, const VehicleTransform &at, const HeliLook &look) {
	if (g_heli.loseNextPose) {
		g_heli.loseNextPose = false;
		return false;
	}
	++g_heli.poses;
	g_heli.lastPose = at;
	g_heli.lastLook = look;
	return true;
}
void HeliRecTail(RemoteHeli &) { ++g_heli.tails; }
void HeliRecExplode(RemoteHeli &h, const Vec3 &at) {
	++g_heli.explodes;
	g_heli.lastExplodeAt         = at;
	g_heli.lastExplodeHadReplica = h.Spawned();
	g_heli.despawnsAtLastExplode = g_heli.despawns;
}
uint8_t HeliRecDrainShots(OwnHeliShot *out, uint8_t max) {
	uint8_t n = 0;
	for (const OwnHeliShot &s : g_heli.ownShots)
		if (n < max)
			out[n++] = s;
	g_heli.ownShots.clear();
	return n;
}
bool HeliRecDraw(RemoteHeli &h, const Vec3 &source, const Vec3 &target) {
	++g_heli.draws;
	g_heli.lastDrawHandle = h.poolHandle;
	g_heli.lastDrawSource = source;
	g_heli.lastDrawTarget = target;
	return g_heli.drawTakes;
}
void HeliRecSend(void *, const void *bytes, size_t len, Channel ch) {
	Message m;
	m.opcode  = static_cast<const uint8_t *>(bytes)[0];
	m.channel = ch;
	m.data.assign(static_cast<const uint8_t *>(bytes),
	              static_cast<const uint8_t *>(bytes) + len);
	g_heli.sent.push_back(m);
}

HeliBridge HeliRecordingBridge() {
	g_heli = HeliRec{};
	HeliBridge b;
	b.SampleOwnHelis         = &HeliRecSample;
	b.DrainOwnHeliGone       = &HeliRecDrainGone;
	b.ApplyHeliHit           = &HeliRecApply;
	b.ResetHeliSession       = &HeliRecReset;
	b.DrainLocalHeliHits     = &HeliRecDrainHits;
	b.CreditHeliShootDown    = &HeliRecCredit;
	b.RequestHeliModel       = &HeliRecModel;
	b.SpawnHeliReplica       = &HeliRecSpawn;
	b.DespawnHeliReplica     = &HeliRecDespawn;
	b.PoseHeliReplica        = &HeliRecPose;
	b.BlowTailOffHeliReplica = &HeliRecTail;
	b.ExplodeHeliReplica     = &HeliRecExplode;
	b.DrainOwnHeliShots      = &HeliRecDrainShots;
	b.DrawHeliShot           = &HeliRecDraw;
	return b;
}

// A HeliSync wired to the recorder. The bridge has to outlive the sync, so it
// is a static here the way m_bridge is a member of Client.
HeliSync &FreshHeliSync() {
	static HeliBridge bridge;
	static HeliSync   sync;
	bridge = HeliRecordingBridge();
	sync   = HeliSync{};
	sync.Bind(&bridge, &HeliRecSend, nullptr);
	return sync;
}

size_t SentCount(uint8_t opcode) {
	size_t n = 0;
	for (const Message &m : g_heli.sent)
		if (m.opcode == opcode)
			++n;
	return n;
}

template <class T>
const T *LastSent() {
	for (auto it = g_heli.sent.rbegin(); it != g_heli.sent.rend(); ++it)
		if (const T *p = it->as<T>())
			return p;
	return nullptr;
}

S_HeliState MakeHeliState(uint8_t owner, uint16_t serial, uint32_t timeMs,
                          float x, uint8_t status = HELI_STATUS_CHASE,
                          uint8_t slot = 0) {
	S_HeliState s;
	InitHeader(s, timeMs);
	s.ownerPlayerId = owner;
	s.body          = HeliStateBody{};
	s.body.serial   = serial;
	s.body.slot     = slot;
	s.body.status   = status;
	s.body.pos      = {x, 100.0f, 60.0f};
	s.body.rot      = {0.0f, 0.0f, 0.0f, 1.0f};
	s.body.searchLightIntensity = 1.0f;
	return s;
}

S_HeliGone MakeHeliGone(uint8_t owner, uint16_t serial, uint8_t reason,
                        uint8_t credit = INVALID_PLAYER, uint8_t slot = 0) {
	S_HeliGone g;
	InitHeader(g, 1000);
	g.ownerPlayerId       = owner;
	g.body                = HeliGoneBody{};
	g.body.serial         = serial;
	g.body.slot           = slot;
	g.body.reason         = reason;
	g.body.creditPlayerId = credit;
	g.body.pos            = {5.0f, 6.0f, 70.0f};
	return g;
}

OwnHeliSample MakeOwnHeli(uint8_t slot, int32_t handle, float x) {
	OwnHeliSample s;
	s.handle      = handle;
	s.body        = HeliStateBody{};
	s.body.slot   = slot;
	s.body.status = HELI_STATUS_CHASE;
	s.body.pos    = {x, 0.0f, 50.0f};
	s.body.rot    = {0.0f, 0.0f, 0.0f, 1.0f};
	return s;
}

void TestTheHeliHitRuleIsTheEngines() {
	std::printf("a hit on a helicopter costs what TestBulletCollision says\n");
	game::HeliDamageState h;
	const uint32_t now = 50000;

	// Every caller of the engine's test passes 4, and the limit is `> 700`
	// (`cmp dword [eax+308h],2BCh / jbe`), so the 175th bullet leaves it at
	// exactly 700 and still flying and the 176th brings it down.
	for (int i = 0; i < 175; ++i)
		game::ApplyHeliHitRule(h, HELI_HIT_BULLET, 4, now, true);
	Check(h.bulletDamage == 700 && h.status == HELI_STATUS_HOVER,
	      "175 rounds of 4 is 700, and 700 is not past the limit");
	Check(game::HeliHitBringsDown(h, HELI_HIT_BULLET, 4), "the next one would be");
	game::ApplyHeliHitRule(h, HELI_HIT_BULLET, 4, now, true);
	Check(h.status == HELI_STATUS_SHOT_DOWN, "the 176th brings it down");
	Check(h.explosionTimer == now + 10000, "and it goes off ten seconds later");
	Check(h.angularSpeed > 0.049f && h.angularSpeed < 0.051f,
	      "spinning at +0.05 when rand() came in under 3FFFh");

	game::ApplyHeliHitRule(h, HELI_HIT_BULLET, 4, now + 3000, false);
	Check(h.explosionTimer == now + 13000,
	      "a hit on one already coming down starts the ten seconds again");
	Check(h.angularSpeed < -0.049f && h.angularSpeed > -0.051f,
	      "and re-rolls the spin, -0.05 this time");

	game::HeliDamageState catalina;
	catalina.heliType     = 2;
	catalina.bulletDamage = 400;
	game::ApplyHeliHitRule(catalina, HELI_HIT_BULLET, 4, now, true);
	Check(catalina.status == HELI_STATUS_SHOT_DOWN, "Catalina's comes down past 400");

	game::HeliDamageState proof;
	proof.bulletProof = true;
	Check(!game::ApplyHeliHitRule(proof, HELI_HIT_BULLET, 4, now, true) &&
	          proof.bulletDamage == 0,
	      "a bulletproof one takes nothing");
	Check(game::ApplyHeliHitRule(proof, HELI_HIT_ROCKET, 0, now, true) &&
	          proof.status == HELI_STATUS_SHOT_DOWN,
	      "and a rocket still brings it down, which reads the other flag");

	game::HeliDamageState rocketProof;
	rocketProof.explosionProof = true;
	Check(!game::ApplyHeliHitRule(rocketProof, HELI_HIT_ROCKET, 0, now, true) &&
	          rocketProof.status == HELI_STATUS_HOVER,
	      "an explosion-proof one shrugs off a rocket");

	game::HeliDamageState fresh;
	game::ApplyHeliHitRule(fresh, HELI_HIT_ROCKET, 0, now, true);
	Check(fresh.status == HELI_STATUS_SHOT_DOWN && fresh.bulletDamage == 0,
	      "one rocket is enough, and it counts no damage");
	Check(!game::ApplyHeliHitRule(fresh, 7, 4, now, true), "an unknown kind is refused");
}

void TestWhenUpdateHelisBlowsItUp() {
	std::printf("the explosion runs on the frame after the timer, not on it\n");
	Check(!game::HeliExplodesThisUpdate(HELI_STATUS_SHOT_DOWN, 1000, 1000),
	      "`jbe` skips it while the time equals the timer");
	Check(game::HeliExplodesThisUpdate(HELI_STATUS_SHOT_DOWN, 1001, 1000),
	      "one millisecond past, it goes");
	Check(!game::HeliExplodesThisUpdate(HELI_STATUS_FLY_AWAY, 5000, 1000),
	      "only a helicopter that was shot down");
}

void TestTheFirstHitThatBringsItDownIsTheCredit() {
	std::printf("whose hit brought the helicopter down\n");
	uint8_t credit = INVALID_PLAYER;
	credit = game::HeliCreditAfterHit(credit, HELI_STATUS_CHASE, HELI_STATUS_CHASE, 2);
	Check(credit == INVALID_PLAYER, "a hit that doesn't bring it down credits nobody");
	credit = game::HeliCreditAfterHit(credit, HELI_STATUS_CHASE, HELI_STATUS_SHOT_DOWN, 2);
	Check(credit == 2, "the one that does is the credit");
	credit = game::HeliCreditAfterHit(credit, HELI_STATUS_SHOT_DOWN,
	                                  HELI_STATUS_SHOT_DOWN, 5);
	Check(credit == 2, "and a later hit on the way down doesn't take it");
}

void TestTheRewardIsTakenBackOnlyWhenItIsExactlyTheEngines() {
	std::printf("the owner's engine doesn't pay for a helicopter somebody else shot down\n");
	const game::HeliRewards before{1000, 3, 40, 12};
	game::HeliRewards after{1250, 4, 42, 14};
	game::HeliRewards out;
	Check(game::WithholdHeliRewards(before, after, 1, 1, out) && out.money == 1000 &&
	          out.helisDestroyed == 3 && out.peopleKilled == 40 && out.copsKilled == 12,
	      "exactly one helicopter's worth, all four taken back");

	Check(!game::WithholdHeliRewards(before, after, 1, 0, out) && out.money == 1250,
	      "our own shoot-down is left alone");

	game::HeliRewards two{1500, 5, 44, 16};
	Check(game::WithholdHeliRewards(before, two, 2, 1, out) && out.money == 1250 &&
	          out.helisDestroyed == 4,
	      "two in one call, one of them ours: one taken back");

	game::HeliRewards odd{1300, 4, 42, 14};
	Check(!game::WithholdHeliRewards(before, odd, 1, 1, out) && out.money == 1300,
	      "money that moved by something else in the same call is left as it is");
}

void TestOnlyTheExplodingHelisCrimeIsWithheld() {
	std::printf("which RegisterCrime_Immediately call is held back\n");
	Check(game::IsWithheldHeliCrime(12, 0x4D83, 0x1), "slot 0's, while slot 0 is held");
	Check(game::IsWithheldHeliCrime(12, 0x4D84, 0x2), "slot 1's, while slot 1 is held");
	Check(!game::IsWithheldHeliCrime(12, 0x4D84, 0x1), "not slot 1's while only 0 is");
	Check(!game::IsWithheldHeliCrime(12, 0x4D85, 0x3), "not the script helicopter's");
	Check(!game::IsWithheldHeliCrime(4, 0x4D83, 0x1), "not some other crime");
	Check(!game::IsWithheldHeliCrime(12, 0x4D83, 0x0), "and nothing outside the call");
}

void TestAHeliHitOnTheWireIsBounded() {
	std::printf("what a helicopter hit may carry\n");
	HeliHitBody b{};
	b.slot   = 0;
	b.kind   = HELI_HIT_BULLET;
	b.damage = 4;
	Check(IsSaneHeliHit(b), "a bullet with the engine's own 4");
	b.damage = HELI_HIT_MAX_DAMAGE + 1;
	Check(!IsSaneHeliHit(b), "not one that would end it in a few packets");
	b.damage = 0;
	Check(!IsSaneHeliHit(b), "not a bullet with nothing in it");
	b.kind = HELI_HIT_ROCKET;
	Check(IsSaneHeliHit(b), "a rocket carries no damage");
	b.slot = 2;
	Check(!IsSaneHeliHit(b), "not the script helicopter's slot");
	b.slot = 0;
	b.kind = 9;
	Check(!IsSaneHeliHit(b), "not a kind nobody defined");
}

void TestAReplicaIsBuiltAndFollowsTheStream() {
	std::printf("somebody else's helicopter is built here and follows its owner\n");
	HeliSync &h = FreshHeliSync();
	g_heli.modelReady = false;

	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	Check(h.ActiveCount() == 1, "a state makes a row");
	h.Tick(5000);
	Check(g_heli.modelAsks == 1 && g_heli.spawns == 0,
	      "nothing is built before MI_CHOPPER is loaded");

	g_heli.modelReady = true;
	h.Tick(5016);
	Check(g_heli.spawns == 1 && g_heli.poses == 1, "built, then placed in the same frame");
	Check(h.IsReplica(500), "and the handle is one of ours");

	h.OnState(MakeHeliState(1, 7, 1100, 20.0f), 0, 5100);
	h.Tick(5200);
	Check(g_heli.spawns == 1 && g_heli.poses == 2, "a second state moves it, no rebuild");
	Check(g_heli.lastLook.searchLightIntensity == 1.0f,
	      "the searchlight is the owner's, not ours");

	h.OnState(MakeHeliState(0, 8, 1100, 20.0f), 0, 5200);
	Check(h.Find(0, 8) == nullptr, "our own helicopter coming back is dropped");
	h.OnState(MakeHeliState(1, 9, 1100, 20.0f, HELI_STATUS_CHASE, 2), 0, 5200);
	Check(h.Find(1, 9) == nullptr, "the script helicopter's slot is not ours to show");
}

void TestAShotDownHelicopterExplodesHereAndStaysGone() {
	std::printf("a helicopter shot down on its owner's machine\n");
	HeliSync &h = FreshHeliSync();
	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	h.Tick(5000);

	h.OnGone(MakeHeliGone(1, 7, HELI_GONE_SHOT_DOWN), 0);
	Check(g_heli.explodes == 1 && g_heli.lastExplodeHadReplica,
	      "the blast is played on the replica");
	Check(g_heli.despawnsAtLastExplode == 0 && g_heli.despawns == 1,
	      "before the replica is taken away, which is what the debris is cut from");
	Check(g_heli.lastExplodeAt.z == 70.0f, "where the owner's engine blew it up");
	Check(h.ActiveCount() == 0, "and the row is gone");
	Check(g_heli.credits == 0, "nobody here is credited when nobody is named");

	h.OnState(MakeHeliState(1, 7, 900, 10.0f), 0, 5050);
	Check(h.ActiveCount() == 0,
	      "a state that overtook the C_HeliGone does not build it again");

	h.OnState(MakeHeliState(1, 11, 2000, 10.0f), 0, 6000);
	h.OnGone(MakeHeliGone(1, 11, HELI_GONE_FLEW_AWAY), 0);
	Check(g_heli.explodes == 1 && h.ActiveCount() == 0,
	      "one that flew away is just taken away, no blast");
}

void TestTheShooterIsCreditedAndNobodyIsPaid() {
	std::printf("the shooter's machine gets the credit for somebody else's helicopter\n");
	HeliSync &h = FreshHeliSync();
	h.OnGone(MakeHeliGone(1, 7, HELI_GONE_SHOT_DOWN, 0, 1), 0);
	Check(g_heli.credits == 1 && g_heli.lastCreditSlot == 1,
	      "ours, even with no replica here, and with the owner's slot");
	h.OnGone(MakeHeliGone(1, 8, HELI_GONE_SHOT_DOWN, 2), 0);
	Check(g_heli.credits == 1, "somebody else's credit is not ours");
	h.OnGone(MakeHeliGone(1, 9, HELI_GONE_FLEW_AWAY, 0), 0);
	Check(g_heli.credits == 1, "and a helicopter that flew away credits nobody");
	h.OnGone(MakeHeliGone(1, 7, HELI_GONE_SHOT_DOWN, 0, 1), 0);
	Check(g_heli.credits == 2,
	      "the sync does not dedupe the credit - the server relays a gone once");
}

void TestAHelicopterNobodyStreamsClimbsAwayAndGoes() {
	std::printf("a helicopter whose stream stops is not left hovering\n");
	HeliSync &h = FreshHeliSync();
	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	h.Tick(5000);
	const float startZ = g_heli.lastPose.pos.z;

	h.Tick(5000 + HELI_STALE_MS);
	Check(h.ActiveCount() == 1 && g_heli.despawns == 0, "held while it is only late");
	h.Tick(5000 + HELI_STALE_MS + 16);
	const RemoteHeli *row = h.Find(1, 7);
	Check(row && row->orphaned, "abandoned once it is past HELI_STALE_MS");

	h.Tick(5000 + HELI_STALE_MS + 16 + 3000);
	Check(g_heli.lastPose.pos.z > startZ + 25.0f && g_heli.lastLook.searchLightIntensity == 0.0f,
	      "climbing, with its searchlight off");

	h.Tick(5000 + HELI_STALE_MS + 16 + HELI_ORPHAN_MAX_MS);
	Check(h.ActiveCount() == 0 && g_heli.despawns == 1, "and taken away");

	// The height at which the engine itself deletes one flying away.
	VehicleTransform high;
	high.pos.z = 145.0f;
	Check(!OrphanedHeliIsGone(OrphanedHeliAt(high, 0), 0), "not at 145 m");
	Check(OrphanedHeliIsGone(OrphanedHeliAt(high, 600), 600), "gone past 150 m");
}

void TestAStreamThatComesBackIsFollowedAgain() {
	std::printf("a helicopter whose stream resumes is not lost\n");
	HeliSync &h = FreshHeliSync();
	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	h.Tick(5000);
	h.Tick(5000 + HELI_STALE_MS + 16);
	Check(h.Find(1, 7) && h.Find(1, 7)->orphaned, "abandoned");
	h.OnState(MakeHeliState(1, 7, 4000, 30.0f), 0, 8000);
	Check(h.Find(1, 7) && !h.Find(1, 7)->orphaned, "and taken back when it streams again");
}

void TestAnOwnerLeavingTakesHisHelicopterAway() {
	std::printf("a helicopter whose owner leaves the session\n");
	HeliSync &h = FreshHeliSync();
	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	h.OnState(MakeHeliState(2, 3, 1000, 50.0f), 0, 5000);
	h.Tick(5000);
	h.OnOwnerLeft(1, 5016);
	Check(h.Find(1, 7) && h.Find(1, 7)->orphaned && h.Find(1, 7)->ownerLeft,
	      "his climbs away");
	Check(h.Find(2, 3) && !h.Find(2, 3)->orphaned, "nobody else's");
	h.OnState(MakeHeliState(1, 7, 1100, 10.0f), 0, 5100);
	Check(h.Find(1, 7)->orphaned, "a late state from him changes nothing");
	h.Tick(5016 + HELI_ORPHAN_MAX_MS);
	Check(h.Find(1, 7) == nullptr && h.ActiveCount() == 1, "and it goes");

	// The slot is somebody else's now, serials from 1 again.
	h.OnGone(MakeHeliGone(1, 1, HELI_GONE_FLEW_AWAY), 0);
	h.OnOwnerLeft(1, 30000);
	h.OnState(MakeHeliState(1, 1, 1000, 10.0f), 0, 30000);
	Check(h.Find(1, 1) != nullptr, "a newcomer in the slot isn't refused his serial 1");
}

void TestTheTailComesOffOnce() {
	std::printf("the first half of the explosion plays once\n");
	HeliSync &h = FreshHeliSync();
	S_HeliState s = MakeHeliState(1, 7, 1000, 10.0f, HELI_STATUS_SHOT_DOWN);
	h.OnState(s, 0, 5000);
	h.Tick(5000);
	Check(g_heli.tails == 0, "not before the owner's engine blew it");
	s.hdr.sendTimeMs = 1100;
	s.body.flags     = HELI_FLAG_TAIL_BLOWN;
	h.OnState(s, 0, 5100);
	h.Tick(5100);
	h.Tick(5116);
	s.hdr.sendTimeMs = 1200;
	h.OnState(s, 0, 5200);
	h.Tick(5200);
	Check(g_heli.tails == 1, "once, however many states carry the flag");
}

void TestAReplicaTheEngineTookIsBuiltAgain() {
	std::printf("a replica the engine deleted under us\n");
	HeliSync &h = FreshHeliSync();
	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	h.Tick(5000);
	g_heli.loseNextPose = true;
	h.Tick(5016);
	Check(h.Find(1, 7) && !h.Find(1, 7)->Spawned(), "the handle is dropped");
	h.Tick(5032);
	Check(g_heli.spawns == 2, "and it is built again next frame");
}

void TestOurHelicopterGoesOutUnderASerial() {
	std::printf("our own helicopter, streamed and ended\n");
	HeliSync &h = FreshHeliSync();
	g_heli.own.push_back(MakeOwnHeli(0, 900, 1.0f));

	h.Send(0, 10000);
	Check(h.OwnSerial(0) == 1, "the first helicopter is serial 1");
	const C_HeliState *st = LastSent<C_HeliState>();
	Check(st && st->body.serial == 1 && st->body.slot == 0 && st->body.pos.x == 1.0f,
	      "and goes out with its serial and its slot");
	Check(SentCount(OP_C_HELI_STATE) == 1, "one state");

	h.Send(0, 10050);
	Check(SentCount(OP_C_HELI_STATE) == 1, "not again within 100 ms");
	h.Send(0, 10100);
	Check(SentCount(OP_C_HELI_STATE) == 2, "again at HELI_STATE_HZ");
	Check(g_heli.sent.back().channel == CH_SNAPSHOT, "on the unreliable channel");

	OwnHeliGone g;
	g.slot           = 0;
	g.handle         = 900;
	g.reason         = HELI_GONE_SHOT_DOWN;
	g.creditPlayerId = 3;
	g.pos            = {7.0f, 8.0f, 9.0f};
	g_heli.gone.push_back(g);
	g_heli.own.clear();
	h.Send(0, 10200);
	const C_HeliGone *gone = LastSent<C_HeliGone>();
	Check(gone && gone->body.serial == 1 && gone->body.reason == HELI_GONE_SHOT_DOWN &&
	          gone->body.creditPlayerId == 3 && gone->body.pos.z == 9.0f,
	      "shot down, with the credit and where it went off");
	Check(SentCount(OP_C_HELI_GONE) == 1, "once");
	Check(g_heli.sent.back().channel == CH_EVENT, "reliably");
	Check(gone && gone->body.flags == 0, "and nothing said about the reward when it was taken back");

	g_heli.own.push_back(MakeOwnHeli(0, 901, 2.0f));
	h.Send(0, 70000);
	Check(h.OwnSerial(0) == 2, "the next one in the same slot is a new serial");

	OwnHeliGone keptGone = g;
	keptGone.handle    = 901;
	keptGone.ownerKept = true;
	g_heli.gone.push_back(keptGone);
	g_heli.own.clear();
	h.Send(0, 70100);
	const C_HeliGone *kept = LastSent<C_HeliGone>();
	Check(kept && kept->body.serial == 2 && kept->body.flags == HELI_GONE_OWNER_KEPT,
	      "a reward our engine could not take back is said, so the shooter does not pay itself too");
}

void TestOurHelicopterVanishingIsStillToldOnce() {
	std::printf("our helicopter leaving its slot without the UpdateHelis detour\n");
	HeliSync &h = FreshHeliSync();
	g_heli.own.push_back(MakeOwnHeli(1, 900, 1.0f));
	h.Send(0, 10000);
	g_heli.own.clear();
	h.Send(0, 10016);
	const C_HeliGone *gone = LastSent<C_HeliGone>();
	Check(gone && gone->body.reason == HELI_GONE_VANISHED && gone->body.slot == 1 &&
	          gone->body.creditPlayerId == INVALID_PLAYER,
	      "a helicopter missing from its slot is gone, with nobody credited");
	h.Send(0, 10032);
	Check(SentCount(OP_C_HELI_GONE) == 1, "and said once");

	g_heli.own.push_back(MakeOwnHeli(0, 950, 1.0f));
	h.Send(0, 20000);
	g_heli.own[0].handle = 951;
	h.Send(0, 20016);
	Check(SentCount(OP_C_HELI_GONE) == 2 && h.OwnSerial(0) == 3,
	      "a different helicopter in the slot ends the old one and starts a new serial");
}

void TestAHitOnOurHelicopterReachesTheEngine() {
	std::printf("somebody else's hit on our helicopter\n");
	HeliSync &h = FreshHeliSync();
	g_heli.own.push_back(MakeOwnHeli(0, 900, 1.0f));
	h.Send(0, 10000);

	S_HeliHit hit;
	InitHeader(hit, 1000);
	hit.attackerId         = 2;
	hit.body               = HeliHitBody{};
	hit.body.ownerPlayerId = 0;
	hit.body.slot          = 0;
	hit.body.serial        = 1;
	hit.body.kind          = HELI_HIT_BULLET;
	hit.body.damage        = 4;
	h.OnHit(hit, 0);
	Check(g_heli.applies == 1 && g_heli.lastApplyHandle == 900 && g_heli.lastAttacker == 2,
	      "applied to the helicopter in that slot, from that attacker");

	S_HeliHit stale = hit;
	stale.body.serial = 5;
	h.OnHit(stale, 0);
	Check(g_heli.applies == 1, "not one for a serial we have already ended");
	S_HeliHit notOurs = hit;
	notOurs.body.ownerPlayerId = 1;
	h.OnHit(notOurs, 0);
	Check(g_heli.applies == 1, "not one addressed to somebody else");
	S_HeliHit self = hit;
	self.attackerId = 0;
	h.OnHit(self, 0);
	Check(g_heli.applies == 1, "not one from ourselves");
	S_HeliHit huge = hit;
	huge.body.damage = 700;
	h.OnHit(huge, 0);
	Check(g_heli.applies == 1, "not one that would end it in a packet");
}

void TestOurHitsOnTheirHelicopterGoToTheOwner() {
	std::printf("our hits on somebody else's helicopter\n");
	HeliSync &h = FreshHeliSync();
	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	h.Tick(5000);

	LocalHeliHit hit;
	hit.owner  = 1;
	hit.slot   = 0;
	hit.serial = 7;
	hit.kind   = HELI_HIT_BULLET;
	hit.damage = 4;
	g_heli.hits.push_back(hit);
	LocalHeliHit unknown = hit;
	unknown.serial = 99;
	g_heli.hits.push_back(unknown);
	LocalHeliHit rocket = hit;
	rocket.kind   = HELI_HIT_ROCKET;
	rocket.damage = 55;
	g_heli.hits.push_back(rocket);
	h.Send(0, 5016);

	Check(g_heli.hits.empty(), "every hit is drained, known or not");
	Check(SentCount(OP_C_HELI_HIT) == 2, "the two on a helicopter we have go out");
	const C_HeliHit *last = LastSent<C_HeliHit>();
	Check(last && last->body.ownerPlayerId == 1 && last->body.serial == 7 &&
	          last->body.kind == HELI_HIT_ROCKET && last->body.damage == 0,
	      "addressed to the owner, and a rocket carries no damage");

	RemoteHeli orphan;
	orphan.active     = true;
	orphan.owner      = 1;
	orphan.poolHandle = 5;
	orphan.orphaned   = true;
	Check(!HeliHitIsWorthSending(orphan, 0), "not one whose owner has stopped flying it");
	RemoteHeli unbuilt = orphan;
	unbuilt.orphaned   = false;
	unbuilt.poolHandle = -1;
	Check(!HeliHitIsWorthSending(unbuilt, 0), "not one we never built");
}

void TestClearingTheSessionTakesEveryReplica() {
	std::printf("the session ending\n");
	HeliSync &h = FreshHeliSync();
	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	h.OnState(MakeHeliState(2, 4, 1000, 10.0f), 0, 5000);
	h.Tick(5000);
	g_heli.own.push_back(MakeOwnHeli(0, 900, 1.0f));
	h.Send(0, 5000);
	h.Clear();
	Check(h.ActiveCount() == 0 && g_heli.despawns == 2, "every replica is destroyed");
	Check(h.OwnSerial(0) == 0 && g_heli.resets == 1,
	      "and our own is forgotten, engine credit and all");
}

void TestTheClientRoutesTheHelicopter() {
	std::printf("the client hands the helicopter packets to the sync\n");
	Client c;
	WorldBridge b  = RecordingBridge();
	b.heli         = HeliRecordingBridge();
	c.SetBridge(b);
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));

	c.HandleMessage(Wrap(MakeHeliState(1, 7, 1000, 10.0f), CH_SNAPSHOT));
	Check(c.HelisForTest().ActiveCount() == 0,
	      "a helicopter from somebody the roster doesn't have is dropped");

	c.HandleMessage(Wrap(MakeJoin(1, "bob"), CH_EVENT));
	c.HandleMessage(Wrap(MakeHeliState(1, 7, 1000, 10.0f), CH_SNAPSHOT));
	Check(c.HelisForTest().ActiveCount() == 1, "one from a player in the session is kept");
	c.HelisForTest().Tick(WallClock::NowMs());
	const RemoteHeli *row = c.HelisForTest().Find(1, 7);
	Check(row && row->Spawned() && c.IsReplicatedVehicle(row->poolHandle),
	      "and its replica is a vehicle this machine only watches");

	S_PlayerLeave leave;
	InitHeader(leave, 2000);
	leave.playerId = 1;
	leave.reason   = LEAVE_QUIT;
	c.HandleMessage(Wrap(leave, CH_EVENT));
	row = c.HelisForTest().Find(1, 7);
	Check(row && row->ownerLeft, "his leaving sends it climbing away");

	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	Check(c.HelisForTest().ActiveCount() == 0, "a new session starts with none");
}

// ---- velocity units at the buffer ----------------------------------------------
//
// Everything on the wire named after m_vecMoveSpeed is in the engine's unit,
// metres per 1/50 s step, and the buffers extrapolate in m/s (interp.h). Each
// push site gets a known engine speed and the buffer is asked where the thing
// is 200 ms past its newest sample. Before the fix every one of these came out
// at a fiftieth of the distance.

bool CoastedTo(const VehicleInterpBuffer &buf, float x) {
	VehicleTransform at;
	return buf.Sample(buf.NewestTimeMs() + 200, at) && NearEnough(at.pos.x, x, 0.01f);
}

void TestEveryPushSiteConvertsTheEngineSpeed() {
	std::printf("\nevery interpolation buffer is fed metres per second\n");
	// 0.5 a step is 25 m/s, 90 km/h: 5 m in 200 ms.
	const Vec3 carSpeed{0.5f, 0.0f, 0.0f};

	{
		Client c;
		c.SetBridge(RecordingBridge());
		c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
		c.HandleMessage(Wrap(MakeJoin(1, "bob"), CH_EVENT));
		S_PlayerState s  = MakeState(1, 1000, 0.0f);
		s.body.moveSpeed = {0.1f, 0.0f, 0.0f};   // a run, 5 m/s
		c.HandleMessage(Wrap(s, CH_SNAPSHOT));
		Pose p;
		Check(c.PlayerSlot(1).interp.Sample(1200, p) && NearEnough(p.pos.x, 1.0f, 0.01f),
		      "OnPlayerState: a player running at 0.1 a step coasts 1 m in 200 ms");
		Check(c.PlayerSlot(1).last.moveSpeed.x == 0.1f,
		      "and the ped is still handed the engine's own number");
	}
	{
		Client c;
		c.SetBridge(RecordingBridge());
		ParkOneCar(c);
		S_VehicleState s = MakeVehicleState(1, 80, 15.0f);
		s.body.moveSpeed = carSpeed;
		c.HandleMessage(Wrap(s, CH_SNAPSHOT));
		Check(CoastedTo(c.VehicleByNetId(80)->interp, 20.0f),
		      "OnVehicleState: a car at 0.5 a step coasts 5 m in 200 ms");
		Check(c.VehicleByNetId(80)->last.moveSpeed.x == 0.5f,
		      "and ApplyRemoteVehicle still gets the raw value to write back");
	}
	{
		Client c;
		c.SetBridge(RecordingBridge());
		c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
		c.HandleMessage(Wrap(MakeAmbientCarSpawn(610), CH_EVENT));
		S_CarStates s{};
		InitHeader(s, 2000);
		s.ownerPlayerId   = 1;
		s.count           = 1;
		s.cars[0].netId   = 610;
		s.cars[0].pos     = {40.0f, 50.0f, 60.0f};
		s.cars[0].rot     = {0.0f, 0.0f, 0.0f, 1.0f};
		s.cars[0].velocity = carSpeed;
		c.HandleMessage(Wrap(s, CH_SNAPSHOT));
		Check(CoastedTo(c.AmbientCar(610)->interp, 45.0f),
		      "OnCarStates: traffic at 0.5 a step coasts 5 m in 200 ms");
	}
	{
		Client c;
		c.SetBridge(RecordingBridge());
		GiveUsTheSessionsCar(c);
		c.TickLocalVehicle();
		S_EnterVehicle enter;
		InitHeader(enter, 1000);
		enter.playerId   = 0;
		enter.body       = EnterVehicleBody{};
		enter.body.netId = 80;
		enter.body.seat  = 0;
		c.HandleMessage(Wrap(enter, CH_EVENT));
		g_rec.sampledMoveSpeed = carSpeed;
		c.TickLocalVehicle();
		Check(CoastedTo(c.VehicleByNetId(80)->interp, 5.0f),
		      "SendLocalVehicle: our own reports go in converted too");
	}
	{
		Client c;
		c.SetBridge(RecordingBridge());
		ParkOneCar(c);
		c.HandleMessage(Wrap(MakeCustody(80, /*us=*/0), CH_EVENT));
		g_rec.sampledMoveSpeed = carSpeed;
		c.TickCustody();
		Check(CoastedTo(c.VehicleByNetId(80)->interp, 16.0f),
		      "SendCustodyVehicles: so do the reports on a car we are settling");
	}
	{
		// Already m/s on the wire. Converting it again would throw it 50
		// times too far.
		Client c;
		WorldBridge b = RecordingBridge();
		b.heli        = HeliRecordingBridge();
		c.SetBridge(b);
		c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
		c.HandleMessage(Wrap(MakeJoin(1, "bob"), CH_EVENT));
		S_HeliState s   = MakeHeliState(1, 7, 1000, 10.0f);
		s.body.velocity = {25.0f, 0.0f, 0.0f};
		c.HandleMessage(Wrap(s, CH_SNAPSHOT));
		const RemoteHeli *row = c.HelisForTest().Find(1, 7);
		Check(row && CoastedTo(row->interp, 15.0f),
		      "HeliSync::OnState: the helicopter's m/s is pushed as it is");
	}
}

// ---- traffic that drops out of its host's nearest eight --------------------
//
// The host streams only the eight cars nearest its own player, so a car that
// falls out of that set just stops appearing in the batch: no despawn, the
// replica lives on and its buffer keeps its samples. Real time, because the
// playback clock runs on WallClock; about two seconds of it.

void TestTrafficThatGoesQuietIsHeldOnItsLastRow() {
	std::printf("\ntraffic whose rows stop, then start again\n");
	Client c;
	c.SetBridge(RecordingBridge());
	g_rec.modelReady = true;
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(630), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientCarSpawns == 1, "the replica is built");

	// The host's car: 25 m/s along x from the spawn point, a row every 100 ms
	// of the host's own clock while it is one of the eight.
	const uint32_t start = WallClock::NowMs();
	auto hostX = [](uint32_t ms) { return 40.0f + 25.0f * ms / 1000.0f; };
	auto row = [&](uint32_t ms) {
		S_CarStates s{};
		InitHeader(s, 50000 + ms);
		s.ownerPlayerId     = 1;
		s.count             = 1;
		s.cars[0].netId     = 630;
		s.cars[0].pos       = {hostX(ms), 50.0f, 60.0f};
		s.cars[0].rot       = {0.0f, 0.0f, 0.0f, 1.0f};
		s.cars[0].velocity  = {0.5f, 0.0f, 0.0f};   // engine units: 25 m/s
		s.cars[0].health    = 1000;
		c.HandleMessage(Wrap(s, CH_SNAPSHOT));
	};

	float    prevX       = g_rec.lastAmbientCarAt.pos.x;
	float    worstBack   = 0.0f;
	float    furthestQuiet = 0.0f;
	float    lastRowX    = 0.0f;
	float    xLateInQuiet = -1.0f;
	uint32_t nextRow     = 0;
	for (;;) {
		const uint32_t ms = WallClock::NowMs() - start;
		if (ms >= 2200)
			break;
		// Streamed, then out of the eight from 600 to 1400, then back in.
		const bool streaming = ms < 600 || ms >= 1400;
		if (ms >= nextRow) {
			if (streaming) {
				row(ms);
				lastRowX = ms < 600 ? hostX(ms) : lastRowX;
			}
			nextRow = ms - ms % 100 + 100;
		}
		c.Tick();
		const float x = g_rec.lastAmbientCarAt.pos.x;
		worstBack = x - prevX < worstBack ? x - prevX : worstBack;
		prevX     = x;
		if (ms >= 600 && ms < 1400) {
			furthestQuiet = x > furthestQuiet ? x : furthestQuiet;
			if (ms >= 1300 && xLateInQuiet < 0.0f)
				xLateInQuiet = x;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}

	std::printf("    last row %.2f m, furthest while quiet %.2f m, now %.2f m\n",
	            lastRowX, furthestQuiet, prevX);
	Check(furthestQuiet <= lastRowX + 1e-4f,
	      "never coasted past what its host last said");
	Check(NearEnough(xLateInQuiet, lastRowX, 1e-4f),
	      "and 700 ms into the quiet it stands exactly on that row");
	Check(worstBack >= -1e-4f, "and never moved backwards, going quiet or coming back");
	Check(prevX > lastRowX + 10.0f, "back in the eight, it has driven on with the stream");
	Check(g_rec.ambientCarSpawns == 1 && g_rec.ambientCarDespawns == 0,
	      "the same replica throughout: nothing despawned, nothing built twice");
	Check(c.AmbientCar(630) != nullptr && c.AmbientCar(630)->poolHandle >= 0,
	      "still on the roster under its netId");
}

// ---- the police helicopter's gun (game/heligun.h) ---------------------------

S_HeliShot MakeHeliShot(uint8_t owner, uint16_t serial, uint32_t timeMs,
                        uint8_t slot = 0) {
	S_HeliShot s;
	InitHeader(s, timeMs);
	s.ownerPlayerId = owner;
	s.body          = HeliShotBody{};
	s.body.serial   = serial;
	s.body.slot     = slot;
	s.body.source   = {10.0f, 100.0f, 57.0f};
	s.body.target   = {12.0f, 96.0f, 12.0f};
	return s;
}

bool SameVec(const Vec3 &a, const Vec3 &b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

// ---- the plane's rocket test is only taken where its call sites say so ------

void PutCall(std::vector<uint8_t> &code, size_t at, uintptr_t base, uintptr_t target) {
	code[at] = 0xE8;
	const int32_t rel = static_cast<int32_t>(target - (base + at + 5));
	std::memcpy(&code[at + 1], &rel, sizeof rel);
}

void TestThePlaneLeadIsCheckedByItsCallSites() {
	std::printf("\nthe plane's rocket test, checked by its call sites\n");
	const uintptr_t site = game::HELI_ROCKET_CALL_SITES[0];
	const uintptr_t base = site - game::PLANE_CALL_WINDOW;
	const size_t    len  = 2 * game::PLANE_CALL_WINDOW + 5;

	std::vector<uint8_t> code(len, 0x90);
	PutCall(code, game::PLANE_CALL_WINDOW, base, game::CHeli__TestRocketCollision);
	Check(game::RelCallAt(&code[game::PLANE_CALL_WINDOW], site, game::CHeli__TestRocketCollision),
	      "the helicopter's call is read where addresses.h has it, backwards displacement and all");
	Check(!game::RelCallAt(&code[game::PLANE_CALL_WINDOW], site, game::PLANE_ROCKET_TEST_LEAD),
	      "and is not mistaken for a call to anything else");

	// `test al,al / jnz / lea eax,[esp+..] / push eax / call / add esp,4`
	const size_t after = game::PLANE_CALL_WINDOW + 5;
	const uint8_t lead[] = {0x84, 0xC0, 0x75, 0x20, 0x8D, 0x44, 0x24, 0x10, 0x50};
	std::memcpy(&code[after], lead, sizeof lead);
	PutCall(code, after + sizeof lead, base, game::PLANE_ROCKET_TEST_LEAD);
	code[after + sizeof lead + 5] = 0x83;
	code[after + sizeof lead + 6] = 0xC4;
	code[after + sizeof lead + 7] = 0x04;
	Check(game::CdeclCallIn(code.data(), base, len, game::PLANE_ROCKET_TEST_LEAD),
	      "a one-argument call to the lead just after it is found");

	std::vector<uint8_t> twoArgs = code;
	twoArgs[after + sizeof lead + 7] = 0x08;
	Check(!game::CdeclCallIn(twoArgs.data(), base, len, game::PLANE_ROCKET_TEST_LEAD),
	      "one popping two arguments is not the function the lead says");

	std::vector<uint8_t> elsewhere = code;
	PutCall(elsewhere, after + sizeof lead, base, game::PLANE_ROCKET_TEST_LEAD + 0x10);
	Check(!game::CdeclCallIn(elsewhere.data(), base, len, game::PLANE_ROCKET_TEST_LEAD),
	      "and a call somewhere else is not a call to it");

	std::vector<uint8_t> before(len, 0x90);
	PutCall(before, 4, base, game::PLANE_ROCKET_TEST_LEAD);
	before[9] = 0x83;
	before[10] = 0xC4;
	before[11] = 0x04;
	Check(game::CdeclCallIn(before.data(), base, len, game::PLANE_ROCKET_TEST_LEAD),
	      "it may come before the helicopter's call as well as after");

	// What retail really has at 0x0055B8F1: `call / test al,al / pop ecx`.
	std::vector<uint8_t> retail = code;
	retail[after + sizeof lead + 5] = 0x84;
	retail[after + sizeof lead + 6] = 0xC0;
	retail[after + sizeof lead + 7] = 0x59;
	Check(game::CdeclCallIn(retail.data(), base, len, game::PLANE_ROCKET_TEST_LEAD),
	      "the retail call site, cleaned up by pop ecx after the test, is found");

	std::vector<uint8_t> noCleanup = code;
	noCleanup[after + sizeof lead + 5] = 0x84;
	noCleanup[after + sizeof lead + 6] = 0xC0;
	noCleanup[after + sizeof lead + 7] = 0x90;
	Check(!game::CdeclCallIn(noCleanup.data(), base, len, game::PLANE_ROCKET_TEST_LEAD),
	      "but a call nobody cleans up after is not taken for it");
}

void TestTheGlassLeadIsCheckedByBothCallers() {
	std::printf("\nthe glass lead, checked by both of the engine's bullet paths\n");
	std::vector<uint8_t> fromCar(game::DRIVEBY_FIRE_LEN, 0x90);
	std::vector<uint8_t> impact(game::BULLET_IMPACT_GLASS_BYTES, 0x90);
	PutCall(fromCar, 0x6A0, game::CWeapon__FireInstantHitFromCar, game::GLASS_HIT_BY_BULLET_LEAD);
	Check(!game::GlassLeadChecksOut(fromCar.data(), impact.data()),
	      "one caller is not enough");
	PutCall(impact, 0x3C, game::CWeapon__DoBulletImpact, game::GLASS_HIT_BY_BULLET_LEAD);
	Check(game::GlassLeadChecksOut(fromCar.data(), impact.data()),
	      "both calling it is what makes it the function the lead says");
	std::vector<uint8_t> other = impact;
	PutCall(other, 0x3C, game::CWeapon__DoBulletImpact, game::GLASS_HIT_BY_BULLET_LEAD + 4);
	Check(!game::GlassLeadChecksOut(fromCar.data(), other.data()),
	      "and a call next door is a call to something else");
	Check(game::DriveByReachesGlass(1) && game::DriveByReachesGlass(4) &&
	          game::DriveByReachesGlass(5) && !game::DriveByReachesGlass(2) &&
	          !game::DriveByReachesGlass(3),
	      "a building, an object or a dummy goes to the glass; a ped or a car has its own arm");
}

void TestTheHeliGunRulesAreTheEngines() {
	std::printf("the helicopter's gun, as the engine fires it\n");

	// ProcessControl puts the source 3 m out from the helicopter.
	const bool both[HELI_POLICE_SLOTS] = {true, true};
	const Vec3 pos[HELI_POLICE_SLOTS]  = {{0.0f, 0.0f, 50.0f}, {100.0f, 0.0f, 50.0f}};
	Check(game::HeliShotSlot(Vec3{1.8f, 0.0f, 47.6f}, both, pos) == 0,
	      "a round 3 m below and out from slot 0 is slot 0's");
	Check(game::HeliShotSlot(Vec3{100.0f, 3.0f, 50.0f}, both, pos) == 1,
	      "and one 3 m from slot 1 is slot 1's");
	Check(game::HeliShotSlot(Vec3{0.0f, 3.04f, 50.0f}, both, pos) == 0,
	      "float rounding on the engine's side is allowed for");
	Check(game::HeliShotSlot(Vec3{0.0f, 2.5f, 50.0f}, both, pos) == -1 &&
	          game::HeliShotSlot(Vec3{0.0f, 3.2f, 50.0f}, both, pos) == -1,
	      "anything else is not a police helicopter's round");
	const bool onlyOne[HELI_POLICE_SLOTS] = {false, true};
	Check(game::HeliShotSlot(Vec3{1.8f, 0.0f, 47.6f}, onlyOne, pos) == -1,
	      "an empty slot fires nothing");

	// The switch at 0x00563E29.
	Check(game::HeliImpactFor(-1) == game::HeliImpact::WaterOrNothing &&
	          game::HeliImpactFor(1) == game::HeliImpact::Building &&
	          game::HeliImpactFor(2) == game::HeliImpact::Vehicle &&
	          game::HeliImpactFor(3) == game::HeliImpact::Ped &&
	          game::HeliImpactFor(4) == game::HeliImpact::Object &&
	          game::HeliImpactFor(5) == game::HeliImpact::Dummy,
	      "each thing the line can find gets the engine's arm");
	Check(game::HeliImpactFor(0) == game::HeliImpact::Silent &&
	          game::HeliImpactFor(6) == game::HeliImpact::Silent,
	      "and a type the table has no entry for gets nothing");

	const Vec3 v = game::HeliTracerVelocity(Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 20.0f, -40.0f});
	Check(std::fabs(v.x - 1.5f) < 1e-5f && std::fabs(v.y - 3.0f) < 1e-5f &&
	          std::fabs(v.z + 6.0f) < 1e-5f,
	      "the tracer flies at 0.15 of the line per step");

	// When it is drawn.
	Check(HeliShotHoldMs(1000, 1000) == VehicleInterpBuffer::DELAY_MS,
	      "a round fired with the newest state waits the replica's delay");
	Check(HeliShotHoldMs(1050, 1000) == VehicleInterpBuffer::DELAY_MS + 50,
	      "one fired after it waits that much longer");
	Check(HeliShotHoldMs(850, 1000) == 0, "one the replica is already past goes now");
	Check(HeliShotHoldMs(5000, 1000) == HELI_SHOT_MAX_HOLD_MS,
	      "and one far past every state is held no longer than the cap");
	Check(HeliShotHoldMs(5, 0xFFFFFFF0u) == VehicleInterpBuffer::DELAY_MS + 21,
	      "across the sender's clock wrapping");

	HeliShotBody b{};
	b.source = {0.0f, 0.0f, 0.0f};
	b.target = {1.0f, 0.0f, 0.0f};
	Check(IsSaneHeliShot(b), "a short line from a police slot is a round");
	b.slot = 2;
	Check(!IsSaneHeliShot(b), "the script's slot is not");
	b.slot     = 0;
	b.source.y = std::numeric_limits<float>::infinity();
	Check(!IsSaneHeliShot(b), "nor a line to infinity");
}

void TestOurHelicoptersRoundsGoOut() {
	std::printf("the rounds our own helicopter fires go out\n");
	HeliSync &h = FreshHeliSync();
	g_heli.own.push_back(MakeOwnHeli(0, 900, 1.0f));

	OwnHeliShot s;
	s.slot   = 0;
	s.handle = 900;
	s.source = {1.0f, 2.0f, 47.0f};
	s.target = {4.0f, 5.0f, 6.0f};
	g_heli.ownShots.push_back(s);
	h.Send(0, 5000);

	Check(SentCount(OP_C_HELI_SHOT) == 1, "one round, one packet");
	const C_HeliShot *out = LastSent<C_HeliShot>();
	Check(out && out->body.serial == h.OwnSerial(0) && out->body.serial != 0 &&
	          out->body.slot == 0,
	      "under the serial the helicopter itself goes out as, fired the frame it "
	      "was first seen");
	Check(out && SameVec(out->body.source, s.source) && SameVec(out->body.target, s.target),
	      "carrying the two points the engine fired between, unchanged");
	Check(out && out->hdr.sendTimeMs == 5000, "stamped on the clock its states are");
	bool unreliable = false;
	for (const Message &m : g_heli.sent)
		if (m.opcode == OP_C_HELI_SHOT)
			unreliable = m.channel == CH_SNAPSHOT;
	Check(unreliable, "unreliable: a lost round is a tracer nobody sees");

	OwnHeliShot stranger = s;
	stranger.handle = 901;
	OwnHeliShot otherSlot = s;
	otherSlot.slot = 1;
	OwnHeliShot broken = s;
	broken.target.x = std::numeric_limits<float>::quiet_NaN();
	g_heli.ownShots = {stranger, otherSlot, broken};
	h.Send(0, 5016);
	Check(SentCount(OP_C_HELI_SHOT) == 1,
	      "not from a helicopter that isn't the one streamed in that slot, nor a "
	      "broken one");

	g_heli.ownShots = {s};
	h.Send(INVALID_PLAYER, 5032);
	Check(SentCount(OP_C_HELI_SHOT) == 1, "nothing goes out outside a session");
}

void TestTheirRoundIsDrawnWhenTheReplicaGetsThere() {
	std::printf("somebody else's helicopter fires on this screen, without hurting anybody\n");
	HeliSync &h = FreshHeliSync();
	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	h.Tick(5000);
	g_heli.sent.clear();

	const S_HeliShot shot = MakeHeliShot(1, 7, 1000);
	h.OnShot(shot, 0, 5000);
	Check(h.PendingShotCount() == 1, "held");
	h.Tick(5000 + VehicleInterpBuffer::DELAY_MS - 1);
	Check(g_heli.draws == 0, "not before the replica's playback reaches it");
	h.Tick(5000 + VehicleInterpBuffer::DELAY_MS);
	Check(g_heli.draws == 1 && g_heli.lastDrawHandle == 500,
	      "then drawn once, from the replica");
	Check(SameVec(g_heli.lastDrawSource, shot.body.source) &&
	          SameVec(g_heli.lastDrawTarget, shot.body.target),
	      "between the owner's two points");
	Check(h.PendingShotCount() == 0, "and let go");
	Check(g_heli.sent.empty() && g_heli.applies == 0,
	      "drawing a round sends nothing and applies nothing: the hit was the owner's");

	h.OnShot(MakeHeliShot(1, 7, 700), 0, 5200);
	h.Tick(5200);
	Check(g_heli.draws == 2, "a round the replica is already past goes at once");

	g_heli.drawTakes = false;
	h.OnShot(MakeHeliShot(1, 7, 700), 0, 5300);
	h.Tick(5300);
	Check(g_heli.draws == 3 && h.PendingShotCount() == 0,
	      "a replica the engine took away mid-way just loses the round");
}

void TestARoundWithNothingToFireFromIsDropped() {
	std::printf("a round from a helicopter we have no replica of\n");
	HeliSync &h = FreshHeliSync();

	h.OnShot(MakeHeliShot(1, 7, 1000), 0, 5000);
	Check(h.PendingShotCount() == 0, "one we never heard of");

	g_heli.modelReady = false;
	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	h.Tick(5000);
	h.OnShot(MakeHeliShot(1, 7, 1000), 0, 5000);
	Check(h.PendingShotCount() == 0, "one whose replica isn't built yet");

	g_heli.modelReady = true;
	h.Tick(5016);
	h.OnShot(MakeHeliShot(1, 7, 1000, 1), 0, 5016);
	Check(h.PendingShotCount() == 0, "one that names a different slot");
	h.OnShot(MakeHeliShot(0, 7, 1000), 0, 5016);
	Check(h.PendingShotCount() == 0, "our own, coming back");

	h.OnShot(MakeHeliShot(1, 7, 1000), 0, 5016);
	Check(h.PendingShotCount() == 1, "the replica is here now");
	h.OnGone(MakeHeliGone(1, 7, HELI_GONE_FLEW_AWAY), 0);
	h.Tick(5500);
	Check(g_heli.draws == 0 && h.PendingShotCount() == 0,
	      "and a round held for a helicopter that left before it was drawn is not");

	h.OnState(MakeHeliState(2, 3, 1000, 10.0f), 0, 6000);
	h.Tick(6000);
	h.OnShot(MakeHeliShot(2, 3, 1000), 0, 6000);
	h.OnOwnerLeft(2, 6010);
	h.Tick(6500);
	Check(g_heli.draws == 0, "nor for one whose owner left; it is climbing away");
	h.OnShot(MakeHeliShot(2, 3, 1100), 0, 6600);
	Check(h.PendingShotCount() == 0, "and nothing more is taken for it");

	h.OnState(MakeHeliState(3, 4, 1000, 10.0f), 0, 7000);
	h.Tick(7000);
	h.Tick(7000 + HELI_STALE_MS + 16);
	h.OnShot(MakeHeliShot(3, 4, 1000), 0, 7000 + HELI_STALE_MS + 16);
	Check(h.PendingShotCount() == 0, "nor for one whose stream went quiet");
}

void TestTheRoundsHeldAreBounded() {
	std::printf("rounds held for replicas are bounded\n");
	HeliSync &h = FreshHeliSync();
	h.OnState(MakeHeliState(1, 7, 1000, 10.0f), 0, 5000);
	h.Tick(5000);
	for (size_t i = 0; i < MAX_PENDING_HELI_SHOTS + 3; ++i)
		h.OnShot(MakeHeliShot(1, 7, 1000), 0, 5000);
	Check(h.PendingShotCount() == MAX_PENDING_HELI_SHOTS, "no more than the queue holds");
	h.Clear();
	Check(h.PendingShotCount() == 0, "and the session ending lets them all go");
	h.Tick(6000);
	Check(g_heli.draws == 0, "without drawing any of them");
}

void TestTheClientRoutesTheHelicoptersRounds() {
	std::printf("the client hands a helicopter's rounds to the sync\n");
	Client c;
	WorldBridge b = RecordingBridge();
	b.heli        = HeliRecordingBridge();
	c.SetBridge(b);
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "bob"), CH_EVENT));
	c.HandleMessage(Wrap(MakeHeliState(1, 7, 1000, 10.0f), CH_SNAPSHOT));
	c.HelisForTest().Tick(WallClock::NowMs());

	c.HandleMessage(Wrap(MakeHeliShot(2, 7, 1000), CH_SNAPSHOT));
	Check(c.HelisForTest().PendingShotCount() == 0,
	      "a round from somebody the roster doesn't have is dropped");
	c.HandleMessage(Wrap(MakeHeliShot(1, 7, 1000), CH_SNAPSHOT));
	Check(c.HelisForTest().PendingShotCount() == 1, "one from bob's helicopter is held");
}

// ---- fists and the bat (game/melee.h) ---------------------------------------

void TestMeleeBytesOffTheWire() {
	std::printf("\nthe two melee bytes, as the owner reads them\n");

	const MeleeTag jab = StrikeTag(6, 4, false);
	Check(MeleeKind(jab) == MELEE_STRIKE && jab.hitLevel == 4 && jab.melee == MELEE_STRIKE,
	      "a jab is a strike at the move's own level");
	const MeleeTag stomp = StrikeTag(FIGHTMOVE_GROUNDKICK, HITLEVEL_GROUND, false);
	Check((stomp.melee & MELEE_GROUND_KICK) != 0, "the kick at the floor says so");
	Check((StrikeTag(8, 3, true).melee & MELEE_ARMED) != 0, "and so does a weapon in hand");
	Check(StrikeTag(8, 200, false).hitLevel == 0, "a level off the end of the table is dropped");

	const MeleeTag r = ReadMeleeTag(WEAPONTYPE_UNARMED, jab.melee, jab.hitLevel);
	Check(r.melee == jab.melee && r.hitLevel == jab.hitLevel, "a strike reads back as sent");
	Check(ReadMeleeTag(WEAPONTYPE_BASEBALLBAT, jab.melee, 4).melee == MELEE_NONE,
	      "a strike never hurts with anything but cause 0");
	Check(ReadMeleeTag(WEAPONTYPE_UNARMED, MELEE_STRIKE, 0).melee == MELEE_NONE &&
	          ReadMeleeTag(WEAPONTYPE_UNARMED, MELEE_STRIKE, MELEE_HIT_LEVELS).melee == MELEE_NONE,
	      "nor at a level no striking move has");
	Check(ReadMeleeTag(WEAPONTYPE_BASEBALLBAT, MELEE_SWING | MELEE_HEAVY, 0).melee ==
	          (MELEE_SWING | MELEE_HEAVY),
	      "a heavy bat swing reads back");
	Check(ReadMeleeTag(WEAPONTYPE_COLT45, MELEE_SWING, 0).melee == MELEE_NONE,
	      "a pistol is not a swing");
	Check(ReadMeleeTag(WEAPONTYPE_BASEBALLBAT, 0x80 | MELEE_SWING, 0).melee == MELEE_NONE &&
	          ReadMeleeTag(WEAPONTYPE_UNARMED, 3, 4).melee == MELEE_NONE,
	      "unknown bits and the fourth kind are refused");
	Check(ReadMeleeTag(WEAPONTYPE_COLT45, 0, 0).melee == MELEE_NONE,
	      "and a round is not melee at all");

	Check(sizeof(DamageBody) == sizeof(PedDamageBody) &&
	          offsetof(DamageBody, melee) == offsetof(PedDamageBody, melee),
	      "one melee layout on a player's hit and a pedestrian's");
}

void TestMeleeTagRidesTheRelay() {
	std::printf("\na punch arrives with what the owner needs to play it\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));

	S_Damage d        = MakeDamage(1, 100, WEAPONTYPE_UNARMED, 9.0f);
	d.body.melee      = MELEE_STRIKE;
	d.body.hitLevel   = 4;
	c.HandleMessage(Wrap(d, CH_EVENT));
	Check(g_rec.damages == 1 && g_rec.lastDamage.melee == MELEE_STRIKE &&
	          g_rec.lastDamage.hitLevel == 4,
	      "a player's punch reaches the seam whole");

	S_PedDamage p      = MakePedDamage(1, 700, WEAPONTYPE_BASEBALLBAT, 25.0f);
	p.body.melee       = MELEE_SWING | MELEE_HEAVY;
	c.HandleMessage(Wrap(p, CH_EVENT));
	Check(g_rec.pedDamages >= 1 && g_rec.lastPedDamage.melee == (MELEE_SWING | MELEE_HEAVY),
	      "and so does a pedestrian's bat");
}

void TestTheStruckSideIsTheEngines() {
	std::printf("\nthe struck ped's half of a punch and a bat, as retail plays it\n");

	// StartFightDefend's arguments.
	const MeleeTag jab = StrikeTag(6, 4, false);
	const MeleeTag armedKick = StrikeTag(8, 3, true);
	Check(DefendHitLevel(jab, WEAPONTYPE_UNARMED, PEDSTATE_IDLE) == 4,
	      "a strike defends at its move's level");
	Check(DefendArg(jab, false, 7) == 7 && DefendArg(armedKick, true, 7) == 7,
	      "with the damage multiplier");
	Check(DefendArg(armedKick, false, 7) == STRIKE_ARMED_DEFEND,
	      "or 65h for an armed striker against somebody who isn't a player");
	const MeleeTag swing = SwingTag(false);
	Check(DefendHitLevel(swing, WEAPONTYPE_BASEBALLBAT, PEDSTATE_IDLE) == HITLEVEL_HIGH &&
	          DefendHitLevel(swing, WEAPONTYPE_BASEBALLBAT, PEDSTATE_FALL) == HITLEVEL_GROUND &&
	          DefendHitLevel(swing, WEAPONTYPE_UNARMED, PEDSTATE_FALL) == HITLEVEL_HIGH,
	      "a swing is high, and the bat on the floor is ground");
	Check(DefendArg(swing, true, 99) == SWING_DEFEND, "and always 0Ah");

	Check(StrikeDamageMult(21.0f) == 7 && StrikeDamageMult(60.0f) == 20 &&
	          StrikeDamageMult(0.0f) == 0 && StrikeDamageMult(-3.0f) == 0,
	      "the multiplier comes back out of damageMult * 3.0f");

	// Nobody is hit while getting up, and only a player is spared.
	Check(MeleeSparesVictim(true, PEDSTATE_GETUP) && !MeleeSparesVictim(false, PEDSTATE_GETUP) &&
	          !MeleeSparesVictim(true, PEDSTATE_IDLE),
	      "a player getting up is left alone, a pedestrian is not");

	// The strike's knockdown.
	Check(StrikeKnocksDown(PEDSTATE_IDLE, 45.0f, 35.0f, false, false, false),
	      "a pedestrian dropping under 40 goes down");
	Check(!StrikeKnocksDown(PEDSTATE_IDLE, 45.0f, 35.0f, true, false, false),
	      "a player doesn't at 40");
	Check(StrikeKnocksDown(PEDSTATE_IDLE, 25.0f, 15.0f, true, false, false),
	      "everybody does at 20");
	Check(!StrikeKnocksDown(PEDSTATE_IDLE, 20.0f, 15.0f, true, false, false),
	      "from above 20, not from 20");
	Check(StrikeKnocksDown(PEDSTATE_IDLE, 90.0f, 80.0f, true, true, false) &&
	          StrikeKnocksDown(PEDSTATE_IDLE, 90.0f, 80.0f, false, false, true),
	      "an armed striker or a one-hit ped always");
	Check(!StrikeKnocksDown(PEDSTATE_FALL, 25.0f, 15.0f, false, true, true) &&
	          !StrikeKnocksDown(PEDSTATE_IDLE, 25.0f, 0.0f, false, true, true),
	      "never somebody already down, or with nothing left");

	// The strike's shove.
	Check(StrikePushScale(false, false, 7) == 7.0f * STRIKE_PUSH_SCALE,
	      "the multiplier, times 0.6");
	Check(StrikePushScale(true, false, 20) == STRIKE_GROUNDKICK_CAP * STRIKE_PUSH_SCALE &&
	          StrikePushScale(true, false, 5) == 5.0f * STRIKE_GROUNDKICK_MULT * STRIKE_PUSH_SCALE,
	      "a ground kick at 0.6 of it, up to 4");
	Check(StrikePushScale(false, true, 9) == STRIKE_DYING_CAP * STRIKE_PUSH_SCALE &&
	          StrikePushScale(false, true, 5) == 10.0f * STRIKE_PUSH_SCALE &&
	          StrikePushScale(false, true, 20) == 20.0f * STRIKE_PUSH_SCALE,
	      "a dying ped twice it, up to 14, below 20 only");

	// The swing.
	Check(SwingKnocksDown(PEDSTATE_IDLE, 90.0f, 80.0f, true, false),
	      "the bat drops any pedestrian it hits");
	Check(!SwingKnocksDown(PEDSTATE_IDLE, 90.0f, 80.0f, true, true) &&
	          SwingKnocksDown(PEDSTATE_IDLE, 25.0f, 15.0f, true, true),
	      "a player only under 20");
	Check(!SwingKnocksDown(PEDSTATE_DIE, 90.0f, 80.0f, true, false),
	      "and nobody already down");
	Check(SwingFallMs(true, false) == 3000 && SwingFallMs(true, true) == 1500 &&
	          SwingFallMs(false, false) == 1500,
	      "3000 ms for the bat on a pedestrian, which is the image and not re3");
	Check(SwingShovesDying(PEDSTATE_DIE, false) && !SwingShovesDying(PEDSTATE_DIE, true) &&
	          !SwingShovesDying(PEDSTATE_DEAD, false),
	      "a dying ped is shoved, except by the heavy swing");

	// The amount a player takes from a bat.
	Check(SwingAmountForPlayer(10.0f, true, false, false) == 20.0f,
	      "twice the weapon's damage, as FireMelee gives a player");
	Check(SwingAmountForPlayer(100.0f, true, true, false) == 100.0f &&
	          SwingAmountForPlayer(35.0f, true, false, true) == 35.0f &&
	          SwingAmountForPlayer(10.0f, false, false, false) == 10.0f,
	      "not after the heavy swing, adrenaline, or for fists");

	// A pedestrian's hit on a player (C_NpcDamage).
	Check(NpcSwingAmountForPlayer(10.0f, true) == 20.0f &&
	          NpcSwingAmountForPlayer(10.0f, false) == 10.0f,
	      "a pedestrian's bat on a player is doubled too, and his fists are not");
	const MeleeTag armed = NpcMeleeTagForPlayer(StrikeTag(FIGHTMOVE_KICK, 3, true));
	Check(MeleeKind(armed) == MELEE_STRIKE && !(armed.melee & MELEE_ARMED) && armed.hitLevel == 3,
	      "his armed strike loses MELEE_ARMED, which only a striking player's has an effect");
	Check(!StrikeKnocksDown(PEDSTATE_IDLE, 90.0f, 80.0f, true,
	                        (armed.melee & MELEE_ARMED) != 0, false),
	      "so it does not knock the player down on the owner's side");
	const MeleeTag heavy = NpcMeleeTagForPlayer(SwingTag(true));
	Check(heavy.melee == (MELEE_SWING | MELEE_HEAVY), "a swing's bits are his own and stay");
}

void TestCopiesNeverReactToMelee() {
	std::printf("\nanother machine's ped does not react to a punch here\n");
	Check(!CopyMayReactToMelee(true, true), "not inside a strike or a swing");
	Check(CopyMayReactToMelee(true, false), "our own peds still do");
	Check(CopyMayReactToMelee(false, true), "and outside one nothing changes");
	Check(IsMeleeCause(WEAPONTYPE_UNARMED) && IsMeleeCause(WEAPONTYPE_BASEBALLBAT) &&
	          !IsMeleeCause(WEAPONTYPE_COLT45),
	      "fists and the bat are the two melee causes");
	Check(IsForwardableDamage(WEAPONTYPE_UNARMED) && IsForwardableDamage(WEAPONTYPE_BASEBALLBAT),
	      "and both still go to the owner");
	Check(!IsReplayableWeapon(WEAPONTYPE_UNARMED) && !IsReplayableWeapon(WEAPONTYPE_BASEBALLBAT),
	      "and neither is replayed, so no copy ever swings");
	Check(!IsFightMove(-1) && !IsFightMove(NUM_FIGHTMOVES) && IsFightMove(FIGHTMOVE_GROUNDKICK),
	      "a fight move is bounded before it indexes the table");
}

// ---- a carjack, played on every screen ------------------------------------
//
// "Cuando le robo el auto a alguien, en la otra pantalla se ve que me subo
// normal y el otro aparece afuera." The jacker's engine played the jack and
// nobody else's did: observers waited for the claim, took the victim out by
// hand and then played an ordinary get-in. S_JackingVehicle carries the jack
// at its start, and each machine plays it with its own engine, which drags out
// whoever sits in that seat there.

S_JackingVehicle MakeJacking(uint8_t playerId, uint16_t netId, uint8_t door) {
	S_JackingVehicle j;
	InitHeader(j, 1000);
	j.playerId   = playerId;
	j.body       = EnteringVehicleBody{};
	j.body.netId = netId;
	j.body.seat  = 0;
	j.body.door  = door;
	return j;
}

void TestTheJackRules() {
	std::printf("\nwhen our engine is taking a ped out of its seat\n");
	using namespace coopiii::game;
	constexpr uint8_t LF = offs::CAR_DOOR_FLAG_LF;
	constexpr uint8_t RF = offs::CAR_DOOR_FLAG_RF;
	constexpr uint8_t JACKED = offs::VEH_IS_BEING_CARJACKED;

	Check(EngineTakingPedOut(PEDSTATE_DRAG_FROM_CAR, 0, 0, 0),
	      "a ped in PED_DRAG_FROM_CAR is being taken out, whatever else is true");
	Check(EngineTakingPedOut(PEDSTATE_DRIVING, LF, LF, JACKED),
	      "a driver whose door a jack has claimed is about to be");
	Check(!EngineTakingPedOut(PEDSTATE_DRIVING, LF, LF, 0),
	      "not by an ordinary get-in through that door");
	Check(!EngineTakingPedOut(PEDSTATE_DRIVING, LF, RF, JACKED),
	      "nor by a jack through somebody else's door");
	Check(!EngineTakingPedOut(PEDSTATE_DRIVING, 0, 0xFF, 0xFF),
	      "and a seat with no door of its own is never jacked through one");
	Check(!EngineTakingPedOut(PEDSTATE_EXIT_CAR, LF, LF, JACKED),
	      "a ped already getting out is getting out, not being pulled");

	// SetCarJack's four arms, read out of the image: the door node to the
	// eDoors value its door tests take.
	Check(CarDoorEnumFor(offs::CAR_DOOR_LF) == 2 && CarDoorEnumFor(offs::CAR_DOOR_RF) == 3 &&
	          CarDoorEnumFor(offs::CAR_DOOR_LR) == 4 && CarDoorEnumFor(offs::CAR_DOOR_RR) == 5,
	      "the four doors are eDoors 2..5");
	Check(CarDoorEnumFor(0) == 0 && CarDoorEnumFor(13) == 0,
	      "and anything else is no door at all");

	uint32_t since = 0;
	Check(!KeepOutOfEnginesWay(false, since, 1000) && since == 0,
	      "nothing to wait for, nothing held");
	Check(KeepOutOfEnginesWay(true, since, 1000) && since == 1000,
	      "the engine starts, the hold starts");
	Check(KeepOutOfEnginesWay(true, since, 1000 + ENGINE_UNSEAT_WAIT_MS - 1),
	      "and holds for the length of a drag");
	Check(ENGINE_UNSEAT_WAIT_MS > 960 + 3800,
	      "which is longer than the door and car_jackedLHS end to end");
	Check(!KeepOutOfEnginesWay(true, since, 1000 + ENGINE_UNSEAT_WAIT_MS),
	      "but not for ever");
	Check(!KeepOutOfEnginesWay(false, since, 9000) && since == 0,
	      "and it is forgotten the moment the engine lets go");

	Check(PulledOutLatchFor(80, 1000, 80, 1000 + PULLED_OUT_HOLD_MS - 1) == 80,
	      "a dragged-out player is kept out of the car the session still names");
	Check(PulledOutLatchFor(80, 1000, 80, 1000 + PULLED_OUT_HOLD_MS) == INVALID_NETID,
	      "until his own exit is too late to be coming");
	Check(PulledOutLatchFor(80, 1000, INVALID_NETID, 1001) == INVALID_NETID,
	      "the exit ends it");
	Check(PulledOutLatchFor(80, 1000, 81, 1001) == INVALID_NETID,
	      "and so does the session seating him anywhere else");
}

void TestAJackIsPlayedIntoATakenSeat() {
	std::printf("\nalice jacks bob, and we watch\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeJoin(2, "bob"), CH_EVENT));
	FeedPosition(c, 2);
	c.Tick();
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(2, 80, 0), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(2).Seated(), "bob has the wheel");
	const int32_t carHandle = c.VehicleByNetId(80)->poolHandle;
	// Bob's own seating was offered the door and warped; count from here.
	const int begins0 = g_rec.seatAnimBegins, warps0 = g_rec.seatAttempts;

	// The intent into a taken seat still waits, as it always did...
	g_rec.animEntry = true;
	c.HandleMessage(Wrap(MakeEntering(1, 80, 0, 0), CH_EVENT));
	c.Tick();
	Check(g_rec.seatAnimBegins == begins0 && g_rec.jackBegins == 0,
	      "an ordinary intent into bob's seat starts nothing");

	// ...and the jack does not.
	g_rec.jackProgress = SEAT_RUNNING;
	c.HandleMessage(Wrap(MakeJacking(1, 80, 0), CH_EVENT));
	c.Tick();
	Check(g_rec.jackBegins == 1, "the jack is played on alice's replica");
	Check(g_rec.lastJackHandle == carHandle && g_rec.lastJackDoor == 0,
	      "on bob's car, through the driver's door");
	Check(g_rec.seatAnimBegins == begins0, "not an ordinary get-in");
	Check(c.PlayerSlot(1).Entering(), "alice is on her way in");
	const int unseats = g_rec.unseats;
	Check(c.PlayerSlot(2).Seated(), "and bob is left in the seat for our engine to pull");

	// Our engine reaches bob: first the door, then the drag.
	g_rec.remotePull[2] = PULL_COMING;
	c.Tick();
	Check(g_rec.unseats == unseats, "nothing takes bob out while the door opens");
	g_rec.remotePull[2] = PULL_DRAGGED;
	c.Tick();

	// And the session catches up in the middle of it, loser first.
	c.HandleMessage(Wrap(MakeExit(2, 80), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.unseats == unseats,
	      "bob's exit does not cut the drag off - that was the teleport");
	Check(c.PlayerSlot(2).Seated(), "the engine still has him");

	// The drag is over. Now he is out, through the ordinary way.
	g_rec.remotePull[2] = PULL_NONE;
	c.Tick();
	Check(g_rec.unseats == unseats + 1, "bob is taken out once the drag lets go");
	Check(!c.PlayerSlot(2).Seated(), "and the roster agrees");

	// Alice's jack ends with her at the wheel, before her claim.
	g_rec.jackProgress = SEAT_DONE;
	c.Tick();
	Check(c.PlayerSlot(1).Seated() && !c.PlayerSlot(1).Entering(), "alice is in");
	Check(g_rec.seatAttempts == warps0, "and nobody was warped anywhere");

	c.HandleMessage(Wrap(MakeEnter(1, 80, 0), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "the claim confirms it");
	Check(g_rec.jackBegins == 1 && g_rec.seatAnimBegins == begins0,
	      "and nothing is played a second time");
}

void TestADraggedPlayerIsNotPutBack() {
	std::printf("\nour engine pulled bob out before his exit came\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeJoin(2, "bob"), CH_EVENT));
	FeedPosition(c, 2);
	c.Tick();
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(2, 80, 0), CH_EVENT));
	c.Tick();

	g_rec.remotePull[2] = PULL_DRAGGED;
	c.Tick();
	g_rec.remotePull[2] = PULL_NONE;
	c.Tick();
	Check(!c.PlayerSlot(2).Seated(), "out of the car when the drag ends");
	Check(c.PlayerSlot(2).seatVehicleNetId == 80, "though the session still seats him");

	const int seats = g_rec.seats, begins = g_rec.seatAnimBegins;
	g_rec.animEntry = true;
	c.Tick();
	c.Tick();
	Check(g_rec.seats == seats && g_rec.seatAnimBegins == begins,
	      "and he is not put back in while his exit is on its way");
}

void TestAJackOfTrafficIsPlayed() {
	std::printf("\nalice jacks a traffic driver somebody else hosts\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(300, /*owner=*/2), CH_EVENT));
	c.Tick();
	const RemoteAmbientCar *car = c.AmbientCar(300);
	Check(car != nullptr && car->poolHandle >= 0, "we have a replica of the car");
	const int32_t carHandle = car->poolHandle;

	c.HandleMessage(Wrap(MakeJacking(1, 300, 0), CH_EVENT));
	c.Tick();
	Check(g_rec.jackBegins == 1 && g_rec.lastJackHandle == carHandle,
	      "the jack is played on the traffic replica, which has no session row");

	g_rec.jackProgress = SEAT_DONE;
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "alice ends up at its wheel");
	c.Tick();
	Check(c.PlayerSlot(1).Seated(), "and stays there while the claim is on its way");
	Check(g_rec.unseats == 0, "without being taken out and put back");

	// The claim: promoted, then the seat. Same CVehicle, same netId.
	c.HandleMessage(Wrap(MakeCarPromoted(300, 1, 2), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 300, 0), CH_EVENT));
	c.Tick();
	Check(c.PlayerSlot(1).Seated() && g_rec.unseats == 0,
	      "the promotion changes the bookkeeping and not the seat");
	Check(g_rec.seatAttempts == 0 && g_rec.seatAnimBegins == 0,
	      "and she is not seated a second time");
}

void TestAJackOfTheJackersOwnTrafficIsNotPlayedTwice() {
	std::printf("\nalice jacks a driver in traffic her own machine hosts\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveAliceAPed(c);
	g_rec.nextAmbientCarHandle = 200;
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(300, /*owner=*/1), CH_EVENT));
	c.Tick();

	g_rec.jackProgress = SEAT_DONE;
	c.HandleMessage(Wrap(MakeJacking(1, 300, 0), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.jackBegins == 1 && c.PlayerSlot(1).Seated(), "we play her jack, and she is in");

	// Her host's claim drops its traffic car and names a new one in the same
	// place (population.cpp, SweepHostedCars): despawn, spawn, seat.
	S_CarDespawn gone;
	InitHeader(gone, 3000);
	gone.netId = 300;
	c.HandleMessage(Wrap(gone, CH_EVENT));
	Check(g_rec.ambientCarDespawns == 1 && !c.PlayerSlot(1).Seated(),
	      "she is out of the replica before it goes");
	c.HandleMessage(Wrap(MakeVehicleSpawn(81), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 81, 0), CH_EVENT));
	g_rec.animEntry = true;
	const int begins = g_rec.seatAnimBegins, warps = g_rec.seats;
	c.Tick();
	Check(c.PlayerSlot(1).Seated() && g_rec.seats == warps + 1,
	      "and straight into the new one");
	Check(g_rec.seatAnimBegins == begins, "without climbing in a second time");
}

void TestTheTrafficDriverIsLeftToTheDrag() {
	std::printf("\nthe driver of that traffic car, on our screen\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 525);
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(610), CH_EVENT));
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 525, 2000, 11.0f, 0, 610, 0), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.ambientSeats == 1, "he is driving");

	// A jack played here reaches him. His host has not dragged its own copy
	// out yet, so its rows go on naming the seat.
	g_rec.ambientPull = PULL_DRAGGED;
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 525, 2100, 12.0f, 0, 610, 0), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.ambientUnseats == 0, "he is not pulled out from under the drag");

	g_rec.ambientPull = PULL_NONE;
	c.HandleMessage(Wrap(MakePedStates(1, 525, 2200, 12.0f, 0, 610, 0), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.ambientUnseats == 1, "he is let go once it is over");
	c.Tick();
	Check(g_rec.ambientSeats == 1,
	      "and the host's rows, a moment behind, do not sit him back down");

	c.HandleMessage(Wrap(MakePedStates(1, 525, 2300, 13.0f), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.ambientSeats == 1 && g_rec.ambientUnseats == 1,
	      "and when the host has him out too, the two agree");
}

void TestADraggedDriverIsNotUnseatedByThePromotion() {
	std::printf("\nthe promotion lands while our engine is still dragging him\n");
	Client c;
	c.SetBridge(RecordingBridge());
	GiveUsAnAmbientPed(c, 526);
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(611), CH_EVENT));
	c.Tick();
	c.HandleMessage(Wrap(MakePedStates(1, 526, 2000, 11.0f, 0, 611, 0), CH_SNAPSHOT));
	c.Tick();
	Check(g_rec.ambientSeats == 1, "he is driving");

	g_rec.ambientPull = PULL_DRAGGED;
	c.Tick();
	c.HandleMessage(Wrap(MakeCarPromoted(611, 2, 1), CH_EVENT));
	c.Tick();
	Check(g_rec.ambientUnseats == 0, "the promotion leaves him to the drag");

	g_rec.ambientPull = PULL_NONE;
	c.Tick();
	Check(g_rec.ambientUnseats == 1, "and the seat loop takes him out after it");
	const RemoteAmbientPed *p = c.AmbientPed(526);
	Check(p != nullptr && !p->Seated(), "on foot, in a car that is a session car now");
}

void TestOurOwnJackGoesOutAsAJack() {
	std::printf("\ntelling the session we are jacking\n");
	Client c;
	c.SetBridge(RecordingBridge());
	c.HandleMessage(Wrap(MakeWelcome(0), CH_EVENT));
	g_rec.modelReady = true;
	// The engine hands out one pool's references; the stub's two counters
	// would otherwise both start at 1.
	g_rec.nextAmbientCarHandle = 200;
	c.HandleMessage(Wrap(MakeVehicleSpawn(80), CH_EVENT));
	c.HandleMessage(Wrap(MakeAmbientCarSpawn(300, /*owner=*/2), CH_EVENT));
	c.Tick();
	const int32_t sessionCar = c.VehicleByNetId(80)->poolHandle;
	const int32_t trafficCar = c.AmbientCar(300)->poolHandle;
	Check(sessionCar != trafficCar, "two cars, two references");

	g_rec.localEntryActive = true;
	g_rec.localEntry       = LocalCarEntry{sessionCar, 0, 0, true};
	c.TickEnteringForTest();
	Check(c.AnnouncedEntryNetIdForTest() == 80 && c.AnnouncedEntryWasJackForTest(),
	      "a jack of a session car goes out as a jack of that car");

	// The quick jack shows a moment after the entry did: the same entry, said
	// again, as a jack.
	g_rec.localEntry = LocalCarEntry{trafficCar, 0, 1, false};
	c.TickEnteringForTest();
	Check(c.AnnouncedEntryNetIdForTest() == INVALID_NETID,
	      "an ordinary entry into traffic is still not announced");
	g_rec.localEntry.jack = true;
	c.TickEnteringForTest();
	Check(c.AnnouncedEntryNetIdForTest() == 300 && c.AnnouncedEntryWasJackForTest(),
	      "but a jack of it is, by its traffic netId");

	// And our own traffic, which only the population seam can name.
	WorldBridge b = RecordingBridge();
	b.HostedCarNetId = [](int32_t handle) -> uint16_t {
		return handle == 44 ? uint16_t{90} : INVALID_NETID;
	};
	c.SetBridge(b);
	g_rec.localEntryActive = true;
	g_rec.localEntry       = LocalCarEntry{44, 0, 0, true};
	c.TickEnteringForTest();
	Check(c.AnnouncedEntryNetIdForTest() == 90, "a jack of a car we host goes out as that car");
}

void TestOurSeatIsLeftToTheDrag() {
	std::printf("\nalice jacks us, and our own engine plays it\n");
	Client c;
	c.SetBridge(RecordingBridge());
	const int32_t handle = GiveUsTheSessionsCar(c);
	c.HandleMessage(Wrap(MakeJoin(1, "alice"), CH_EVENT));
	FeedPosition(c, 1);
	c.Tick();
	c.TickLocalVehicle();
	c.HandleMessage(Wrap(MakeEnter(0, 80, 0), CH_EVENT));
	Check(c.LocalVehicleNetId() == 80, "we drive car 80");

	// Her jack reaches us first. Our seat is the one it is for.
	c.HandleMessage(Wrap(MakeJacking(1, 80, 0), CH_EVENT));
	c.Tick();
	Check(g_rec.jackBegins == 1 && g_rec.lastJackHandle == handle,
	      "her replica starts the jack on the car we are sitting in");

	// Our engine is dragging us out when the session hands her the car.
	g_rec.localPull = PULL_DRAGGED;
	c.HandleMessage(Wrap(MakeExit(0, 80), CH_EVENT));
	c.HandleMessage(Wrap(MakeEnter(1, 80, 0), CH_EVENT));
	c.Tick();
	c.Tick();
	Check(g_rec.vehicleSurrenders == 0, "the handover waits for the drag");

	// The drag lets go of the wheel, and the handover has nothing left to do.
	g_rec.localPull          = PULL_NONE;
	g_rec.drivingLocally     = false;
	g_rec.localVehicleHandle = -1;
	c.Tick();
	c.Tick();
	Check(g_rec.vehicleSurrenders == 0, "we were never put beside the car by hand");
	Check(!c.VehicleByNetId(80)->surrendered, "and the car is simply hers");
}

int main() {
	TestWelcome();
	TestRejectedWelcome();
	TestJoinAndLeave();
	TestSelfIsNotARemote();
	TestStateBeforeJoin();
	TestTwoPhaseSpawn();
	TestNoSpawnWithoutAPosition();
	TestVehicleSpawnNeedsAModel();
	TestVehicleSpawnCarriesItsOwnPosition();
	TestVehicleStateBeforeSpawn();
	TestVehicleStateApplied();
	TestVehicleDespawn();
	TestVehicleBlowUpIsReplayed();
	TestVehicleBlowUpForACarWeDoNotHave();
	TestAnUnownedWreckIsAppliedOnTheNextFrame();
	TestAnUnownedWreckThatIsNotHereYetIsRetried();
	TestAnUnownedWreckThatIsAlreadyAWreckIsDropped();
	TestAnUnresolvableUnownedKeyIsDropped();
	TestTheSameUnownedCarIsOnlyHeldOnce();
	TestAParkedSessionCarIsBlownUpThroughTheRoster();
	TestASessionCarWeDoNotHaveIsDropped();
	TestWreckIsNeverRespawned();
	TestWreckIgnoresLaterSnapshots();
	TestBlowUpEmptiesTheCarFirst();
	TestWreckIsStillHeldInPlace();
	TestVehicleModelIsAskedForAgainOnRespawn();
	TestVehicleSpawnCarriesExtras();
	TestExtrasAreClampedToTheModel();
	TestDisconnectClearsVehicles();
	TestVehicleClaimIsSentOnce();
	TestARefusedClaimIsNotAName();
	TestRemoteDriverIsRecorded();
	TestGarageMaskIsAUnion();
	TestLeavingReleasesGarages();
	TestOurOwnGarageMaskIsNotHeldAgainstUs();
	TestAKnockedOverPostArrivesAndOurOwnDoesNot();
	TestNoSocketMeansNoObjectReportWentOut();
	TestNoGarageReportWithoutAWorld();
	TestResprayFindsTheCar();
	TestResprayForACarWeDoNotHave();
	TestDisconnectReleasesEveryDoor();
	TestGarageHoldDecisions();
	TestServicedGarageArmIsSuppressed();
	TestGarageDeviationIsWhatTravels();
	TestVehicleCorrectedEveryFrame();
	TestVehicleCorrectionStopsWithTheVehicle();
	TestHornReplayValueSoundsInEveryRhythm();
	TestHornRidesOneSnapshotPastTheEnd();
	TestReplicaHornDecision();
	TestReplicaHornThroughTheClient();
	TestModelChangeRebuildsThePed();
	TestModelChangeWhileSeated();
	TestCleanPlayerLook();
	TestLookSlotPicking();
	TestLookChangeRebuildsThePed();
	TestLookOnAnotherModelKeepsThePed();
	TestSpawnWaitsForTheLook();
	TestOurLookIsSentOnChange();
	TestSeatingWaitsForTheCar();
	TestAnimatedEntryIsTriedBeforeTheWarp();
	TestAnIntentOpensTheDoorTheOwnerUsed();
	TestOurOwnEntryIsAnnouncedOnceAndRearmed();
	TestAnIntentDoesNotTakeAnOccupiedSeat();
	TestAnIntentNeverWarps();
	TestAnAbandonedEntryDoesNotLeaveHerInTheCar();
	TestARefusedAnimationWarpsOnTheSameFrame();
	TestAnAnimationThatStopsFallsBackToTheWarp();
	TestAnEntryThatNeverFinishesIsTimedOut();
	TestAnEntryIsJudgedByItsAnimation();
	TestTheSeatKeyPicksADoorOnOurSide();
	TestAWarpedExitGivesTheDoorBack();
	TestOneAnimatedAttemptPerEnterEvent();
	TestAnEntryIsAbandonedWhenTheCarGoes();
	TestAnEntryIsAbandonedWhenThePlayerDies();
	TestExitAnimationStartsFromTheSnapshot();
	TestTheSeatIsAnnouncedWhenTheWalkStarts();
	TestAnEntryThatNeverFinishesTakesItsSeatBack();
	TestASeatThatCameOutDifferentIsCorrected();
	TestTheWarpFallbackStillAnnouncesExactlyOnce();
	TestOurOwnPassengerSeatIsNotReadAsACarWeDrive();
	TestExitAnimationIsNotStartedOnFoot();
	TestSeatedPedStopsBeingPositioned();
	TestCarIsEmptiedBeforeItIsDestroyed();
	TestLosingThePedLosesTheSeat();
	TestRefusedSeatingIsNotRetriedForever();
	TestLeavingWhileSeated();
	TestPoseApplied();
	TestPedLostToTheEngineIsRespawned();
	TestNoBridgeIsSafe();
	TestTheSwitchReachesTheEngine();
	TestAmmoIsIgnoredWithTheSwitchOff();
	TestAStoredSlotIsRecordedAndApplied();
	TestTheHeldWeaponArrivesOnTheSnapshot();
	TestAFreshPedIsHandedTheWholeInventory();
	TestWelcomeClearsPreviousSession();
	TestAnimGroupChoice();
	TestAnimGroupRefusals();
	TestWireValueBounds();
	TestAngleWrap();
	TestReplayableWeapons();
	TestProjectileWeapons();
	TestProjectileSpawnPoint();
	TestProjectileBasis();
	TestInstantHitWeapons();
	TestFlatHeadingDirection();
	TestRotateOnto();
	TestShotDirectionIsAveraged();
	TestShotDirectionPrefersTheDrawnTrail();
	TestShotNeedsAPed();
	TestShotIsReplayedOncePerPacket();
	TestADriveByRoundReachesTheSeamWhole();
	TestOurOwnShotsAreNotReplayed();
	TestExplosionDoesNotNeedAPed();
	TestCombatFromAStranger();
	TestLocalCombatIsDrainedNotSampled();
	TestDamageDecisions();
	TestMovingListTeardown();
	TestMovingListNodeSanity();
	TestAnimClumpLimit();
	TestAnimNodeArrayOverflowTargets();
	TestMovingListSweep();
	TestMovingListSweepCost();
	TestNonLoopingWeapons();
	TestWeaponAnimLoop();
	TestRemoteDamageToTheLocalPlayer();
	TestFireTable();
	TestDeathAnimChoice();
	TestDamageOnlyLandsOnUs();
	TestDeathKillsTheirPed();
	TestDeathTakesThemOutOfTheCarFirst();
	TestRespawnRebuildsThePed();
	TestFriendlyFireReachesTheBridge();
	TestLifeStateMachine();
	TestAnArrestIsNoticedByThePedState();
	TestABustedPlayerComesBackAtThePoliceStation();
	TestADragTakesThemOutOfTheSeat();
	TestTheDragLatch();
	TestTagDistanceCurve();
	TestTagSizeIsResolutionIndependent();
	TestProbeBudget();
	TestOcclusionFade();
	TestOcclusionAlpha();
	TestTagHealthText();
	TestTagNameIsSafeForCFont();
	TestEightTagsDoNotStack();
	TestArrowSpriteSize();
	TestArrowQuad();
	TestArrowAngle();
	TestArrowColour();
	TestArrowWanted();
	TestArrowSpriteTable();
	TestClockDriftIsCircular();
	TestClockOnlyMovesWhenItIsWorthIt();
	TestTheHostKeepsItsOwnClock();
	TestTrainCycleIsTheEnginesMask();
	TestSessionTimeWarmsUpBySnapping();
	TestSessionTimePicksTheLeastDelayedSample();
	TestSessionTimeSlewsWithoutRunningBackwards();
	TestSessionTimeJumpsToADifferentClock();
	TestSessionTimeAcrossTheWrap();
	TestTrainsRunOnTheServersClock();
	TestPlaneCycleIsTheEnginesMask();
	TestMissionCessnaKeepsItsSchedule();
	TestLightsAreTheEnginesThresholds();
	TestLightsAgreeOnTheSessionClock();
	TestLightsFallBackWithoutASession();
	TestBridgeStatesAreTheEnginesThresholds();
	TestBridgeFollowsTheSessionClock();
	TestBridgeLinksSurviveAJump();
	TestWelcomeSetsTheClockStraightAway();
	TestWeatherIsAPairAndIsWrittenEveryTime();
	TestBecomingTheHostGivesTheSkyBack();
	TestRubbishWorldStateIsIgnored();
	TestNoWorldToReadMeansNoCorrection();
	TestDisconnectDropsTheHost();

	TestABackfilledPlayerExistsWithoutASnapshot();
	TestAJoinWithoutAPositionStillCreatesNothing();
	TestTheSeedGivesWayToTheirOwnStream();
	TestABackfilledCorpseIsACorpse();
	TestADeathDuringTheModelStreamIsNotLost();
	TestABackfilledCorpseIsNotPutInACar();
	TestABackfilledCarArrivesInTheConditionItIsIn();
	TestAWreckedSpawnSaysSo();
	TestTheSessionsOwnCarIsTakenOverNotClaimedTwice();
	TestACarWeDriveIsNotCorrectedUnderUs();
	TestOurOwnReportsHoldTheCarWeParked();
	TestACarFromOurOwnWorldIsStillClaimedNormally();
	TestBeingJackedHandsTheCarOver();
	TestALostCarIsNotClaimedStraightBack();
	TestGettingBackOutAndInIsAnOrdinaryClaimAgain();
	TestACarWeAreOnlyWatchingChangingDriverIsNotAHandover();
	TestClaimingOurOwnCarKeepsARowForIt();
	TestGettingBackIntoOurOwnCarDoesNotRegisterItTwice();
	TestOurOwnCarSurvivesTheSessionEnding();
	TestADriverlessCarIsPutAtRest();

	TestTheDoorByteIsNotTheDamage();
	TestDamageIsAMonotoneJoin();
	TestTheWireHasNoRoomForABadIndex();
	TestTheComponentPairing();
	TestADentReachesTheCar();
	TestARespraysClearLetsTheRowGoOfTheDents();
	TestADentBeforeTheCarIsNotLost();
	TestADentIsNeverAppliedToACarWeDrive();
	TestAWreckTakesNoMoreDents();
	TestADentForACarWeHaveNeverHeardOf();

	TestAGrantUnblocksAndRemovesNothing();
	TestOurOwnCollectionIsNeverReplayedOnUs();
	TestSomebodyElsesPickupIsRemoved();
	TestAGrantWeCannotUseIsHandedBack();
	TestADenialOnlyEndsTheClaim();
	TestWhoMayMakeADrop();
	TestSomebodyElsesDropIsBuiltHere();
	TestOurOwnDropIsNeverBuiltTwice();
	TestDisconnectGivesThePickupsBackToTheEngine();

	TestAmbientPedIsToldWhereItIs();
	TestABurningPedestrianBurnsOnEveryScreen();
	TestTheWeaponRidesTheFlagsByte();
	TestAReplicaIsArmedAsItsHostHasHim();
	TestAnNpcRoundIsHandedOverToBeDrawn();
	TestAnNpcHitLandsOnUsAndNobodyElse();
	TestAPedestriansRoundOnOurCarReachesTheEngine();
	TestALimbComesOffTheReplica();
	TestALimbForAPedWeDoNotHaveIsDropped();
	TestANodeThatIsNotALimbIsRefused();
	TestAPlayersOwnLimbComesOffTheirPed();
	TestAPedDiesOnTheReplica();
	TestADeathThatOvertakesItsOwnSpawn();
	TestADeathForAPedWeDoNotHaveIsDropped();
	TestADeathTheEngineRefusedIsRetried();
	TestARebuiltReplicaDiesAgain();
	TestAHitOnOurPedestrianReachesTheEngine();
	TestAHitWithNobodyToBlameStillLands();
	TestNoPedDamageSeamIsSilentNotBroken();
	TestAPedHitIsItsOwnKindOfEvent();
	TestAHitOnOurCarReachesTheEngine();
	TestAHitOnACarWeAreNotDrivingIsRefused();
	TestAHitWithNobodyToBlameStillLandsOnOurCar();
	TestAWreckTakesNoMoreHits();
	TestNoVehicleHitSeamIsSilentNotBroken();
	TestWhichHitsAreWorthSending();
	TestACarHitCarriesThreeFieldsBecauseTheEngineTakesThree();
	TestWhoMayDamageACar();
	TestAReplayedShotCountsOnce();
	TestAReplayedRoundDoesNotMoveUs();
	TestTrafficHealthOnTheWire();
	TestTrafficHornThroughTheClient();
	TestTrafficSirenThroughTheClient();
	TestAHitOnOurTrafficReachesTheEngine();
	TestWhichTrafficHitsAreWorthSending();
	TestAPedInACarCannotBeShotToDeath();
	TestOnlyOurOwnFlameIsForwarded();
	TestFlameReachIsTheEngines();
	TestAFlameOnTheWireIsAnIgnition();
	TestAFlameIsReportedOnceASecond();
	TestAFlameReachesTheSeamLikeAHit();
	TestADyingDriverLeavesHisSeatFirst();
	TestAmbientPedNobodyStreamsIsHeld();
	TestAmbientPedStatesFromTheWrongMachine();
	TestTrafficDriverTakesHisSeat();
	TestDriverSeatWaitsForTheCar();
	TestAmbientCarIsEmptiedBeforeItIsDestroyed();
	TestAnAmbientWreckIsAppliedToTheReplica();
	TestAnAmbientWreckForACarWeDoNotHaveIsDropped();
	TestAWreckedAmbientCarIsNeverBuilt();
	TestAmbientPedIsEmptiedBeforeItIsDestroyed();
	TestRefusedAmbientSeatingIsNotRetriedForever();
	TestAmbientReplicaTakenByTheEngineIsRebuilt();
	TestASeatedReplicaTakenByTheEngineKeepsItsInstruction();
	TestWhichPedDamageCausesTheProofFlagsMiss();
	TestALocalDeathOfSomebodyElsesPedestrianIsRefused();
	TestBlowUpCarsTwoArmsOnItsOccupants();
	TestADeadReplicaIsOnlyRebuiltIfNobodyAskedForIt();
	TestAReplicaOurOwnEngineKilledIsRebuilt();
	TestACorpseTheSessionReportedIsNotResurrected();
	TestDisconnectClearsAmbientPeds();

	TestWantedBitsSurviveTheWire();
	TestWhoLendsUsStars();
	TestWantedIsInvisibleToALonePlayer();
	TestABorrowedStarBecomesYours();
	TestSharedComesBackDown();
	TestSharedDoesNotUnearnAnybody();
	TestWantedOffIsAClamp();
	TestOnlyTheCarYouAreInLendsYouStars();
	TestABorrowedLevelDoesNotEchoInSharedMode();
	TestNoPlayerPedMeansNoWantedDecision();
	TestTheSessionsWantedRuleArrives();
	TestACarWeAreDrivingIsNotWrittenToBeforeTheClaimComesBack();
	TestADeathCauseIsNamed();

	TestAParkedCarWithNoCustodianIsStillPinnedAndRested();
	TestTheCarWeAreSettlingIsNeitherRestedNorPinned();
	TestSomebodyElseSettlingACarIsFollowedAndNotRested();
	TestASettleEndsOnARunOfQuietSamplesAndNotOnOne();
	TestASettleThatNeverSettlesIsGivenUpOn();
	TestADriverEndsOurSettle();
	TestACustodianLeavingReleasesTheCarHere();
	TestTakingTheWheelOfSomebodyElsesTrafficClaimsIt();
	TestATrafficCarIsNotAlsoClaimedAsABrandNewOne();
	TestAPromotedTrafficCarKeepsItsObjectAndItsNumber();
	TestTheMachineThatMadeTheCarNeverDestroysIt();
	TestTheObservedTable();
	TestOurClaimedCarIsInTheDetoursTable();
	TestOurClaimedCarJackedOffUsIsRefusedHere();
	TestACarWeTookBackIsNotStillHersInTheTable();
	TestAHitOnACarSomebodyElseIsSettlingIsTheirs();
	TestASettleEndingHandsTheCarBack();
	TestOurOwnSettleIsOursToDamage();
	TestAHitThatLandsAfterOurSettleEnded();
	TestACarNobodyHoldsIsShotThroughTheSession();
	TestWhatBurningMeansToACustodian();
	TestOurHitOnACarNobodyHoldsComesBackToUs();
	TestABurningCarIsKeptUntilItGoesUp();
	TestOurSettleAfterTakingOverSomebodyElsesIsOurs();
	TestADentWhileWeSettleACarIsSent();
	TestTheLastDentGoesOutBeforeTheSettle();
	TestADentAfterOurSettleIsNotOursToSend();
	TestALighterDentIsStillNewsAfterASettle();
	TestWhoElseHoldsACar();
	TestWithoutTheRowOurClaimedCarWasOursToWreck();
	TestOurOwnCarLeavesTheTableWithTheSession();
	TestAPromotedCarWeHostedLeavesTheTableToo();
	TestCopiesDontEatTheTrafficBudget();
	TestACopyStaysOutOfTheSave();
	TestHowACopyEnds();
	TestTheCopyList();
	TestTheTrafficAllowanceRunsAfterTheSpawns();
	TestAReleaseLeavesNobodyWaiting();
	TestOurCarReleasedUnderUsIsClaimedAgain();
	TestTheEntryGateRefusesACarOnItsSideAndNotOneUpsideDown();
	TestARestedCarPassesBothGates();
	TestABoatModelIsBuiltAsABoat();
	TestTheWritersThatMustNotTouchABoat();
	TestABoatItsDriverCouldLeaveIsAlreadyAtRest();
	TestWhichKillsCountTowardARampage();
	TestARampageOpensAndTheTargetArrives();
	TestNothingIsCreditedWithoutAnOpenFrenzy();
	TestAWelcomeWithNoRampageBitsSharesAnyway();
	TestTheRampageBitsDoNotCollideWithTheOtherRules();
	TestWhichCarsCountTowardARampage();
	TestTheCarTallyDecision();
	TestEveryCarCountsOnceOnEveryMachine();
	TestARampageCarReachesTheSeam();

	g_failures += RunRampageVoteTests();
	g_failures += RunAimPitchTests();
	g_failures += RunSirenTests();
	g_failures += RunCheatTests();
	g_failures += RunDriveByTests();
	g_failures += RunMoneyTests();
	g_failures += RunStreamPickTests();
	g_failures += RunChatFeedTests();
	g_failures += RunAdoptTests();
	TestALeaversPedIsAdoptedByUs();
	TestSomebodyElseAdoptsAndWeFollow();
	TestWhatWeCannotTakeIsLetGo();
	TestACarAndItsDriverAreTakenTogether();
	TestAnObserverKeepsTheDriverSeated();
	TestTheCarWeAreDrivingWaitsForItsClaim();
	TestAnAdoptPacketIsBounded();
	TestTheFeedHearsTheSession();
	TestATypedLineIsTakenOnce();
	TestThePingsAreTheServers();
	TestBackInTheCarWeWereIn();
	TestBeingTurnedAwaySaysWhy();
	TestSomebodyElseHasTheCarWeWereIn();
	TestTheCarWeWereInNeverComesBack();
	TestLimbsFromNoSessionStayBehind();
	TestQuietCountsFromTheJoin();
	TestACarChangingHandsIsOnItsNewDriversClock();
	TestACopyHeldAtItsOldestIsNotProbed();
	TestTheDesyncProbeSaysWhereOurCopiesAre();
	TestTheProbeTakesInTheCrowdAndTheTraffic();
	TestAShoveAsksToSettleAParkedCar();
	TestABlastOnAParkedCarStays();
	TestANudgeIsAPush();
	TestWireMotionIsHeld();
	TestACarsOwnBlastIsNotRelayed();
	TestASinkingCarIsKeptUntilTheBottom();
	TestACarWeHoldIsOursToDent();
	TestALateExitEchoKeepsOurNewCar();
	TestOurOwnTrafficPromotedIsNotBuiltAgain();
	TestAWelcomeAnnouncesOurCrowdAgain();
	TestTrafficDentsThroughTheClient();
	TestOurTrafficsDentsGoOut();
	TestTheBatchesAreRankedByWhereTheOthersAre();
	TestTheHeliHitRuleIsTheEngines();
	TestWhenUpdateHelisBlowsItUp();
	TestTheFirstHitThatBringsItDownIsTheCredit();
	TestTheRewardIsTakenBackOnlyWhenItIsExactlyTheEngines();
	TestOnlyTheExplodingHelisCrimeIsWithheld();
	TestAHeliHitOnTheWireIsBounded();
	TestAReplicaIsBuiltAndFollowsTheStream();
	TestAShotDownHelicopterExplodesHereAndStaysGone();
	TestTheShooterIsCreditedAndNobodyIsPaid();
	TestAHelicopterNobodyStreamsClimbsAwayAndGoes();
	TestAStreamThatComesBackIsFollowedAgain();
	TestAnOwnerLeavingTakesHisHelicopterAway();
	TestTheTailComesOffOnce();
	TestAReplicaTheEngineTookIsBuiltAgain();
	TestOurHelicopterGoesOutUnderASerial();
	TestOurHelicopterVanishingIsStillToldOnce();
	TestAHitOnOurHelicopterReachesTheEngine();
	TestOurHitsOnTheirHelicopterGoToTheOwner();
	TestClearingTheSessionTakesEveryReplica();
	TestTheClientRoutesTheHelicopter();
	TestEveryPushSiteConvertsTheEngineSpeed();
	TestTrafficThatGoesQuietIsHeldOnItsLastRow();
	TestTheHeliGunRulesAreTheEngines();
	TestThePlaneLeadIsCheckedByItsCallSites();
	TestTheGlassLeadIsCheckedByBothCallers();
	TestOurHelicoptersRoundsGoOut();
	TestTheirRoundIsDrawnWhenTheReplicaGetsThere();
	TestARoundWithNothingToFireFromIsDropped();
	TestTheRoundsHeldAreBounded();
	TestTheClientRoutesTheHelicoptersRounds();
	TestMeleeBytesOffTheWire();
	TestMeleeTagRidesTheRelay();
	TestTheStruckSideIsTheEngines();
	TestCopiesNeverReactToMelee();
	TestTheJackRules();
	TestAJackIsPlayedIntoATakenSeat();
	TestADraggedPlayerIsNotPutBack();
	TestAJackOfTrafficIsPlayed();
	TestAJackOfTheJackersOwnTrafficIsNotPlayedTwice();
	TestTheTrafficDriverIsLeftToTheDrag();
	TestADraggedDriverIsNotUnseatedByThePromotion();
	TestOurOwnJackGoesOutAsAJack();
	TestOurSeatIsLeftToTheDrag();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
