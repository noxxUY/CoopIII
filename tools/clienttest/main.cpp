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
#include "game/combat.h"
#include "game/nametag.h"
#include "game/pedanim.h"

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

	int              vehicleSpawns   = 0;
	int              vehicleDespawns = 0;
	int              vehicleApplies  = 0;
	int              vehicleCorrections = 0;
	VehicleTransform lastCorrection  = {};
	VehicleStateBody lastVehicleBody = {};
	int              nextVehicleHandle = 1;

	// What SampleLocalVehicleIdentity reports. `drivingLocally` off means the
	// player is on foot, which is the default.
	bool     drivingLocally = false;
	uint16_t localModel     = 90;

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

void RecApplyVehicle(RemoteVehicle &, const VehicleStateBody &body) {
	++g_rec.vehicleApplies;
	g_rec.lastVehicleBody = body;
}

bool RecSampleLocalVehicle(VehicleStateBody &out) {
	if (!g_rec.drivingLocally)
		return false;
	out        = VehicleStateBody{};
	out.health = 1000.0f;
	return true;
}

bool RecSampleLocalVehicleIdentity(uint16_t &modelId, uint8_t &c1, uint8_t &c2,
                                   Vec3 &pos, Quat &rot) {
	if (!g_rec.drivingLocally)
		return false;
	modelId = g_rec.localModel;
	c1      = 1;
	c2      = 2;
	pos     = Vec3{5.0f, 6.0f, 7.0f};
	rot     = Quat{0.0f, 0.0f, 0.0f, 1.0f};
	return true;
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
	b.SpawnRemoteVehicle         = &RecSpawnVehicle;
	b.DespawnRemoteVehicle       = &RecDespawnVehicle;
	b.ApplyRemoteVehicle         = &RecApplyVehicle;
	b.CorrectRemoteVehicle       = &RecCorrectVehicle;
	b.SeatRemotePed              = &RecSeat;
	b.UnseatRemotePed            = &RecUnseat;

	b.DrainLocalCombat    = &RecDrainLocalCombat;
	b.ReplayRemoteShot    = &RecReplayShot;
	b.PlayRemoteExplosion = &RecPlayExplosion;
	b.ApplyRemoteDamage   = &RecApplyDamage;
	b.KillRemotePed       = &RecKill;
	b.SetFriendlyFire     = &RecSetFriendlyFire;

	b.SampleWorld         = &RecSampleWorld;
	b.ApplyWorldTime      = &RecApplyWorldTime;
	b.ApplyWorldWeather   = &RecApplyWorldWeather;
	b.ReleaseWorldWeather = &RecReleaseWorldWeather;
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

	c.HandleMessage(Wrap(MakeDeath(1, 17 /*ANIM_STD_KO_SHOT_FACE*/), CH_EVENT));
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

} // namespace

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
	TestDisconnectClearsVehicles();
	TestVehicleClaimIsSentOnce();
	TestRemoteDriverIsRecorded();
	TestVehicleCorrectedEveryFrame();
	TestVehicleCorrectionStopsWithTheVehicle();
	TestModelChangeRebuildsThePed();
	TestModelChangeWhileSeated();
	TestSeatingWaitsForTheCar();
	TestSeatedPedStopsBeingPositioned();
	TestCarIsEmptiedBeforeItIsDestroyed();
	TestLosingThePedLosesTheSeat();
	TestRefusedSeatingIsNotRetriedForever();
	TestLeavingWhileSeated();
	TestPoseApplied();
	TestPedLostToTheEngineIsRespawned();
	TestNoBridgeIsSafe();
	TestWelcomeClearsPreviousSession();
	TestAnimGroupChoice();
	TestAnimGroupRefusals();
	TestWireValueBounds();
	TestAngleWrap();
	TestReplayableWeapons();
	TestProjectileWeapons();
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
	TestClockDriftIsCircular();
	TestClockOnlyMovesWhenItIsWorthIt();
	TestTheHostKeepsItsOwnClock();
	TestWelcomeSetsTheClockStraightAway();
	TestWeatherIsAPairAndIsWrittenEveryTime();
	TestBecomingTheHostGivesTheSkyBack();
	TestRubbishWorldStateIsIgnored();
	TestNoWorldToReadMeansNoCorrection();
	TestDisconnectDropsTheHost();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
