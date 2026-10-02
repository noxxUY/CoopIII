// What the session's mission lets go of: client/src/game/missionclear.h and
// the roster's half of S_MissionRelease and of a mission car's despawn. The
// rules walked with no engine, the roster through a bridge of its own.

#include "client.h"
#include "game/missionclear.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstring>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_clearFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_clearFailures;
}

template <class T>
Message Pack(const T &pkt) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

struct Rec {
	int32_t nextHandle  = 1;
	int     carDespawns = 0;
	bool    onScreen    = true;
	bool    aboard      = false;
	bool    fading      = false;
};
Rec g;

bool ModelReady(uint16_t) { return true; }
bool SpawnCar(RemoteAmbientCar &c) {
	c.poolHandle = g.nextHandle++;
	return true;
}
void DespawnCar(RemoteAmbientCar &c) {
	c.poolHandle = -1;
	++g.carDespawns;
}
bool OnScreen(uint8_t, int32_t) { return g.onScreen; }
void Alpha(uint8_t, int32_t, uint8_t, bool fading) { g.fading = fading; }
bool Aboard(const RemoteAmbientCar &) { return g.aboard; }

WorldBridge Bridge() {
	g = Rec{};
	WorldBridge b;
	b.IsModelReady             = &ModelReady;
	b.SpawnAmbientCarReplica   = &SpawnCar;
	b.DespawnAmbientCarReplica = &DespawnCar;
	b.CrowdReplicaOnScreen     = &OnScreen;
	b.SetCrowdReplicaAlpha     = &Alpha;
	b.LocalAboardAmbientCar    = &Aboard;
	return b;
}

void Join(Client &c) {
	S_Welcome w{};
	InitHeader(w, 1000);
	w.playerId     = 0;
	w.netId        = 100;
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hostPlayerId = INVALID_PLAYER;
	c.HandleMessage(Pack(w));
}

void MissionCar(Client &c, uint16_t netId) {
	S_CarSpawn s{};
	InitHeader(s, 1000);
	s.ownerPlayerId = 1;
	s.netId         = netId;
	s.body.modelId  = 90;
	s.body.extra1   = -1;
	s.body.extra2   = -1;
	s.body.flags    = AMBIENT_MISSION;
	s.body.pos      = {40.0f, 0.0f, 10.0f};
	s.body.rot      = {0.0f, 0.0f, 0.0f, 1.0f};
	c.HandleMessage(Pack(s));
	c.Tick();
}

void Despawn(Client &c, uint16_t netId) {
	S_CarDespawn d{};
	InitHeader(d, 1100);
	d.netId = netId;
	c.HandleMessage(Pack(d));
}

void TestTheRules() {
	std::printf("\nthe rules\n");
	Check(!LocalAboardCar(false, true, 0), "not his last car's pointer alone: another car is not his");
	Check(!LocalAboardCar(true, false, 0), "a car he merely drove last is not his");
	Check(LocalAboardCar(true, true, 0) && LocalAboardCar(true, false, PEDSTATE_ENTER_CAR) &&
	          LocalAboardCar(true, false, PEDSTATE_EXIT_CAR),
	      "inside, entering and exiting are");
	Check(MissionEntityHold(false, true, false, true) == MissionHold::Keep,
	      "what was never the mission's is not let go of");
	Check(MissionEntityHold(true, true, true, false) == MissionHold::Keep,
	      "nor what the mission still holds");
	Check(MissionEntityHold(true, true, false, false) == MissionHold::Release,
	      "marked no longer needed, it is released");
	Check(MissionEntityHold(true, true, true, true) == MissionHold::Release,
	      "and everything is once the mission is over");
	Check(MissionEntityHold(true, false, false, true) == MissionHold::WaitForName,
	      "unless its name has not come back yet");
	Check(!FinalReleaseReady(1, 100) && FinalReleaseReady(0, 0) &&
	          FinalReleaseReady(1, MISSION_FINAL_WAIT_MS),
	      "the last batch waits for names, but not for ever");
	Check(ReleaseRowApplies(true, 1, 1) && !ReleaseRowApplies(true, 2, 1) &&
	          !ReleaseRowApplies(false, 1, 1),
	      "a row applies to its owner's copy only");
}

void TestADespawnedMissionCarFades() {
	std::printf("\na mission car its owner removed fades out on our screen\n");
	Client c;
	c.SetBridge(Bridge());
	Join(c);
	MissionCar(c, 410);
	Despawn(c, 410);
	const RemoteAmbientCar *car = c.AmbientCar(410);
	Check(car && car->Leaving() && g.carDespawns == 0 && g.fading,
	      "in view it stays built, fading");
	g.onScreen = false;
	c.Tick();
	Check(!c.AmbientCar(410) && g.carDespawns == 1, "and goes at once once it is out of view");

	g.onScreen = false;
	MissionCar(c, 411);
	Despawn(c, 411);
	Check(!c.AmbientCar(411) && g.carDespawns == 2, "out of view to start with, it goes at once");

	g.onScreen = true;
	g.aboard   = true;
	MissionCar(c, 412);
	Despawn(c, 412);
	Check(!c.AmbientCar(412) && g.carDespawns == 3,
	      "one our player is in is handed to the engine, not faded");
}

void TestAFinalGoneRowFadesAndAReleasedOneStays() {
	std::printf("\nthe owner's mission over\n");
	Client c;
	c.SetBridge(Bridge());
	Join(c);
	MissionCar(c, 420);
	MissionCar(c, 421);
	MissionCar(c, 422);
	S_MissionRelease r{};
	InitHeader(r, 1200);
	r.ownerPlayerId = 1;
	r.count         = 3;
	r.rows[0]       = MissionReleaseRow{420, AMBIENT_ADOPT_CAR, 0};
	r.rows[1]       = MissionReleaseRow{421, AMBIENT_ADOPT_CAR, 1};
	r.rows[2]       = MissionReleaseRow{422, AMBIENT_ADOPT_CAR, 1};
	r.rows[2].netId = 422;
	c.HandleMessage(Pack(r));
	const RemoteAmbientCar *kept = c.AmbientCar(420);
	Check(kept && !kept->Leaving() && (kept->body.flags & AMBIENT_MISSION) == 0 &&
	          c.MissionRowsReleasedForTest() == 1,
	      "a released car stays, as ordinary crowd");
	const RemoteAmbientCar *gone = c.AmbientCar(421);
	Check(gone && gone->Leaving(), "one the owner holds no more fades out");

	// Somebody else's release does not touch a copy that is not theirs.
	S_MissionRelease other = r;
	other.ownerPlayerId    = 2;
	other.count            = 1;
	other.rows[0]          = MissionReleaseRow{420, AMBIENT_ADOPT_CAR, 1};
	c.HandleMessage(Pack(other));
	Check(c.AmbientCar(420) && !c.AmbientCar(420)->Leaving(),
	      "and another player's row does not take a car that is not his");
}

} // namespace

int RunMissionClearTests() {
	TestTheRules();
	TestADespawnedMissionCarFades();
	TestAFinalGoneRowFadesAndAReleasedOneStays();
	return g_clearFailures;
}
