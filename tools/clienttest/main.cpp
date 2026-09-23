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
#include "game/cardamage.h"
#include "game/combat.h"
#include "game/garage.h"
#include "game/movinglist.h"
#include "game/nametag.h"
#include "game/pedanim.h"
#include "game/pickup.h"
#include "game/radar.h"
#include "game/wanted.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
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
	int              vehicleAtRest   = 0;
	// What shape a car was told it is in, and whether the parts were told to
	// fly off. The flag is the interesting one: a live change makes them fly,
	// a spawn or a backfill must not, or a joiner is greeted by a shower of
	// doors out of the object pool.
	int               vehicleDamageApplies = 0;
	VehicleDamageBody lastVehicleDamage{};
	bool              lastDamageFlying = false;
	int              vehicleCorrections = 0;
	VehicleTransform lastCorrection  = {};
	VehicleStateBody lastVehicleBody = {};
	int              nextVehicleHandle = 1;

	// What SampleLocalVehicleIdentity reports. `drivingLocally` off means the
	// player is on foot, which is the default.
	bool     drivingLocally = false;
	uint16_t localModel     = 90;
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
	Pose     lastAmbientPose{};
	int      nextAmbientPedHandle = 1;

	int      ambientCarSpawns   = 0;
	int      ambientCarDespawns = 0;
	int      nextAmbientCarHandle = 1;

	// Limbs taken off a replica because its host's engine did.
	int      limbs             = 0;
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

	// The traffic driver. `attempts` counts calls and `seats` the ones that
	// took, the same split the player seating uses and for the same reason.
	int      ambientSeatAttempts = 0;
	int      ambientSeats        = 0;
	int      ambientUnseats      = 0;
	uint16_t lastAmbientSeatCar  = INVALID_NETID;
	uint8_t  lastAmbientSeatIdx  = 0xFF;
	bool     refuseAmbientSeating = false;
	// The engine has taken the ped replica away underneath us. What the real
	// AmbientReplicaIsAlive discovers by resolving the pool handle and
	// checking the vtable, the stub is simply told.
	bool     ambientReplicaTaken  = false;
	int      ambientLivenessChecks = 0;
	// How many unseats had happened by the time the last ambient car
	// despawn ran. Destroying a car under a seated ped leaves that ped
	// following a null pointer, so the order is the thing being pinned.
	int      unseatsAtLastAmbientCarDespawn = -1;
};

Recorder g_rec;

void RecRequestModel(uint16_t id) { g_rec.modelRequests.push_back(id); }
bool RecIsModelReady(uint16_t)    { return g_rec.modelReady; }

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
	return true;
}

void RecDespawnVehicle(RemoteVehicle &v) {
	v.poolHandle = -1;
	++g_rec.vehicleDespawns;
	g_rec.unseatsAtLastVehicleDespawn = g_rec.unseats;
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

bool RecBeginSeat(RemotePlayer &, RemoteVehicle &v, uint8_t seat) {
	++g_rec.seatAnimBegins;
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

void RecCorrectVehicle(RemoteVehicle &, const VehicleTransform &at) {
	++g_rec.vehicleCorrections;
	g_rec.lastCorrection = at;
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

void RecApplyWorldWeather(uint8_t weather, uint8_t weatherOld) {
	++g_rec.weatherApplies;
	g_rec.appliedWeather    = weather;
	g_rec.appliedWeatherOld = weatherOld;
}

void RecReleaseWorldWeather() { ++g_rec.weatherReleases; }

void RecRestVehicle(RemoteVehicle &) { ++g_rec.vehicleAtRest; }

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
	out        = VehicleStateBody{};
	out.health = 1000.0f;
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
	if (!g_rec.ambientReplicaTaken)
		return true;
	p.poolHandle           = -1;
	p.spawnPending         = true;
	p.seatedVehicleNetId   = INVALID_NETID;
	return false;
}

void RecApplyAmbientPed(RemoteAmbientPed &, const Pose &at) {
	++g_rec.ambientApplies;
	g_rec.lastAmbientPose = at;
}

bool RecSpawnAmbientCar(RemoteAmbientCar &c) {
	c.poolHandle = g_rec.nextAmbientCarHandle++;
	++g_rec.ambientCarSpawns;
	return true;
}

void RecDespawnAmbientCar(RemoteAmbientCar &c) {
	c.poolHandle = -1;
	++g_rec.ambientCarDespawns;
	g_rec.unseatsAtLastAmbientCarDespawn = g_rec.ambientUnseats;
}

void RecCorrectAmbientCar(RemoteAmbientCar &, const VehicleTransform &) {}

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

WorldBridge RecordingBridge() {
	g_rec = Recorder{};
	WorldBridge b;
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
	b.ApplyRemoteVehicle         = &RecApplyVehicle;
	b.RestRemoteVehicle          = &RecRestVehicle;
	b.ApplyRemoteVehicleDamage   = &RecApplyVehicleDamage;
	b.CorrectRemoteVehicle       = &RecCorrectVehicle;
	b.SeatRemotePed              = &RecSeat;
	b.UnseatRemotePed            = &RecUnseat;
	b.BeginSeatRemotePed         = &RecBeginSeat;
	b.PollSeatRemotePed          = &RecPollSeat;
	b.AbandonSeatRemotePed       = &RecAbandonSeat;
	b.BeginUnseatRemotePed       = &RecBeginUnseat;
	b.BlowUpRemoteVehicle        = &RecBlowUpVehicle;
	b.DrainLocalVehicleBlasts    = &RecDrainLocalBlasts;
	b.WreckUnownedVehicle        = &RecWreckUnowned;
	b.DrainUnownedBlasts         = &RecDrainUnownedBlasts;

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

	b.LocalDrivesVehicle = &RecLocalDrivesVehicle;

	b.SampleWorld         = &RecSampleWorld;
	b.ApplyWorldTime      = &RecApplyWorldTime;
	b.ApplyWorldWeather   = &RecApplyWorldWeather;
	b.ReleaseWorldWeather = &RecReleaseWorldWeather;

	b.SpawnAmbientReplica     = &RecSpawnAmbientPed;
	b.DespawnAmbientReplica   = &RecDespawnAmbientPed;
	b.AmbientReplicaIsAlive   = &RecAmbientReplicaIsAlive;
	b.ApplyAmbientPedState    = &RecApplyAmbientPed;
	b.SeatAmbientPed          = &RecSeatAmbientPed;
	b.UnseatAmbientPed        = &RecUnseatAmbientPed;
	b.RemoveAmbientBodyPart   = &RecRemoveAmbientBodyPart;
	b.KillAmbientReplica      = &RecKillAmbientReplica;
	b.SpawnAmbientCarReplica  = &RecSpawnAmbientCar;
	b.DespawnAmbientCarReplica = &RecDespawnAmbientCar;
	b.CorrectAmbientCarReplica = &RecCorrectAmbientCar;
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
                          uint8_t seat = 0) {
	S_PedStates s{};
	InitHeader(s, timeMs);
	s.ownerPlayerId      = owner;
	s.count              = 1;
	s.peds[0].netId        = netId;
	s.peds[0].animId       = animId;
	s.peds[0].vehicleNetId = vehicleNetId;
	s.peds[0].seat         = seat;
	s.peds[0].pad          = 0;
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
	TestNoGarageReportWithoutAWorld();
	TestResprayFindsTheCar();
	TestResprayForACarWeDoNotHave();
	TestDisconnectReleasesEveryDoor();
	TestGarageHoldDecisions();
	TestServicedGarageArmIsSuppressed();
	TestGarageDeviationIsWhatTravels();
	TestVehicleCorrectedEveryFrame();
	TestVehicleCorrectionStopsWithTheVehicle();
	TestModelChangeRebuildsThePed();
	TestModelChangeWhileSeated();
	TestSeatingWaitsForTheCar();
	TestAnimatedEntryIsTriedBeforeTheWarp();
	TestARefusedAnimationWarpsOnTheSameFrame();
	TestAnAnimationThatStopsFallsBackToTheWarp();
	TestAnEntryThatNeverFinishesIsTimedOut();
	TestOneAnimatedAttemptPerEnterEvent();
	TestAnEntryIsAbandonedWhenTheCarGoes();
	TestAnEntryIsAbandonedWhenThePlayerDies();
	TestExitAnimationStartsFromTheSnapshot();
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
	TestALimbComesOffTheReplica();
	TestALimbForAPedWeDoNotHaveIsDropped();
	TestANodeThatIsNotALimbIsRefused();
	TestAPedDiesOnTheReplica();
	TestADeathThatOvertakesItsOwnSpawn();
	TestADeathForAPedWeDoNotHaveIsDropped();
	TestADeathTheEngineRefusedIsRetried();
	TestARebuiltReplicaDiesAgain();
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

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
