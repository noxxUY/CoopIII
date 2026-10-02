// What the session's mission takes that the session already has:
// client/src/game/missiontake.h, and the roster's half of it. The owner's
// mission teleporting a car the owner sits in (SET_PLAYER_COORDINATES) put a
// guest's own car back in the world inside the mission's instruction, and it
// went out as a new mission car: the guest saw his car twice.

#include "client.h"
#include "game/missiontake.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstring>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_takeFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_takeFailures;
}

// ---- the rules ---------------------------------------------------------------

void TestASessionCarIsNeverTheMissions() {
	std::printf("\na car the mission adds back is the mission's only when nobody has it\n");
	Check(MissionCarAdd(true, true, false) == MissionAdd::Mission,
	      "a mission car its instruction made is the mission's");
	Check(MissionCarAdd(true, true, true) == MissionAdd::OnSession,
	      "a session car or a replica the instruction teleported is the session's already");
	Check(MissionCarAdd(false, true, false) == MissionAdd::NotMission &&
	          MissionCarAdd(false, true, true) == MissionAdd::NotMission,
	      "outside the mission's instructions nothing is the mission's");
	Check(MissionCarAdd(true, false, false) == MissionAdd::NotMission,
	      "nor a car the mission did not make, traffic and parked cars");
}

void TestAPlayerOrACopyIsNeverTheMissions() {
	std::printf("\nnor a pedestrian the session already names\n");
	Check(MissionPedAdd(true, true, false, false) == MissionAdd::Mission,
	      "a character the mission made is the mission's");
	Check(MissionPedAdd(true, true, true, false) == MissionAdd::OnSession,
	      "another player's ped is that player still");
	Check(MissionPedAdd(true, true, false, true) == MissionAdd::OnSession,
	      "a copy of somebody else's pedestrian is his still");
	Check(MissionPedAdd(false, true, true, false) == MissionAdd::NotMission &&
	          MissionPedAdd(true, false, false, false) == MissionAdd::NotMission,
	      "and outside an instruction, or a ped it did not make, nothing is the mission's");
}

void TestOneCarStandsInOnePlace() {
	std::printf("\ntwo cars of one model where one car stands are one car\n");
	const Vec3 at{1219.5f, -321.0f, 26.4f};
	Check(MissionCarIsSessionCar(94, at, 94, Vec3{1219.5f, -321.0f, 26.4f}),
	      "same model, same spot");
	Check(MissionCarIsSessionCar(94, at, 94, Vec3{1220.2f, -321.0f, 26.4f}),
	      "0.7 m apart, still inside each other");
	Check(!MissionCarIsSessionCar(94, at, 94, Vec3{1221.4f, -321.0f, 26.4f}),
	      "1.9 m apart, door to door, two cars");
	Check(!MissionCarIsSessionCar(94, at, 94, Vec3{1219.5f, -321.0f, 29.0f}),
	      "one above the other on a ramp, two cars");
	Check(!MissionCarIsSessionCar(133, at, 94, at),
	      "another model in the same spot is another car");
	Check(MISSION_CAR_SAME_AS_SESSION_M == 1.0f, "the edge is 1 m");
}

// ---- the roster ----------------------------------------------------------------

struct TakeRec {
	int32_t nextHandle    = 50;
	int     carSpawns     = 0;
	int     carDespawns   = 0;
	Vec3    poseOf[128]   = {};
	bool    hasPose[128]  = {};
};
TakeRec g;

bool ModelReady(uint16_t) { return true; }
void RequestModel(uint16_t) {}
bool SpawnVehicle(RemoteVehicle &v) {
	v.poolHandle = g.nextHandle++;
	return true;
}
bool SpawnCar(RemoteAmbientCar &c) {
	c.poolHandle = g.nextHandle++;
	++g.carSpawns;
	return true;
}
void DespawnCar(RemoteAmbientCar &c) {
	c.poolHandle = -1;
	++g.carDespawns;
}
bool ReadPose(int32_t handle, VehicleTransform &out) {
	if (handle < 0 || handle >= 128 || !g.hasPose[handle])
		return false;
	out.pos = g.poseOf[handle];
	return true;
}

WorldBridge TakeBridge() {
	g = TakeRec{};
	WorldBridge b;
	b.IsModelReady             = &ModelReady;
	b.RequestModel             = &RequestModel;
	b.SpawnRemoteVehicle       = &SpawnVehicle;
	b.SpawnAmbientCarReplica   = &SpawnCar;
	b.DespawnAmbientCarReplica = &DespawnCar;
	b.ReadVehiclePose          = &ReadPose;
	return b;
}

template <class T>
Message Pack(const T &pkt, Channel ch = CH_EVENT) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = ch;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

S_Welcome Welcome() {
	S_Welcome w{};
	InitHeader(w, 1000);
	w.playerId     = 1;
	w.netId        = 101;
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hostPlayerId = INVALID_PLAYER;
	return w;
}

// The guest's own car, as the roster holds it while he stands beside it.
S_VehicleSpawn SessionCar(uint16_t netId, uint16_t model, const Vec3 &at) {
	S_VehicleSpawn s{};
	InitHeader(s, 1000);
	s.netId   = netId;
	s.modelId = model;
	s.pos     = at;
	s.rot     = {0.0f, 0.0f, 0.0f, 1.0f};
	s.health  = 1000.0f;
	s.extra1  = -1;
	s.extra2  = -1;
	return s;
}

S_CarSpawn MissionCar(uint16_t netId, uint16_t model, const Vec3 &at, bool mission = true) {
	S_CarSpawn s{};
	InitHeader(s, 1000);
	s.ownerPlayerId = 0;
	s.netId         = netId;
	s.body.modelId  = model;
	s.body.extra1   = -1;
	s.body.extra2   = -1;
	s.body.flags    = mission ? AMBIENT_MISSION : 0;
	s.body.pos      = at;
	s.body.rot      = {0.0f, 0.0f, 0.0f, 1.0f};
	return s;
}

S_CarStates Row(uint16_t netId, const Vec3 &at, uint32_t timeMs) {
	S_CarStates s{};
	InitHeader(s, timeMs);
	s.ownerPlayerId   = 0;
	s.count           = 1;
	s.cars[0].netId   = netId;
	s.cars[0].health  = 0;
	s.cars[0].pos     = at;
	s.cars[0].rot     = {0.0f, 0.0f, 0.0f, 1.0f};
	return s;
}

const Vec3 kParked{1210.0f, -318.0f, 26.0f};

int32_t GuestWithHisCar(Client &c) {
	c.SetBridge(TakeBridge());
	c.HandleMessage(Pack(Welcome()));
	c.HandleMessage(Pack(SessionCar(95, 94, kParked)));
	c.Tick();
	const int32_t h = c.SessionCarHandleOf(95);
	if (h >= 0 && h < 128) {
		g.poseOf[h]  = kParked;
		g.hasPose[h] = true;
	}
	return h;
}

void TestTheGuestsCarIsNotBuiltTwice() {
	std::printf("\nthe owner announces the guest's own car as a mission car\n");
	Client        c;
	const int32_t his = GuestWithHisCar(c);
	Check(his >= 0, "the guest's car 95 is built");

	c.HandleMessage(Pack(MissionCar(455, 94, Vec3{1210.3f, -318.2f, 26.0f})));
	c.Tick();
	const RemoteAmbientCar *dup = c.AmbientCar(455);
	Check(dup != nullptr && dup->sameAsVehicle == 95 && dup->poolHandle < 0,
	      "mission car 455 where car 95 stands is car 95, and is not built");
	Check(g.carSpawns == 0, "nothing is built for it");
	c.Tick();
	c.Tick();
	Check(g.carSpawns == 0 && c.SessionCarHandleOf(95) == his,
	      "nor on the frames after, and car 95 is untouched");
}

void TestOneTeleportedFirstIsTakenDownWhenItLands() {
	std::printf("\nborn where the teleport put it, then its host's rows put it on our car\n");
	Client c;
	GuestWithHisCar(c);
	c.HandleMessage(Pack(MissionCar(455, 94, Vec3{1219.5f, -321.0f, 27.4f})));
	c.Tick();
	Check(g.carSpawns == 1 && c.AmbientCar(455)->sameAsVehicle == INVALID_NETID,
	      "born 9 m away it is built at first");
	c.HandleMessage(Pack(Row(455, Vec3{1210.1f, -318.0f, 26.0f}, 1200), CH_SNAPSHOT));
	c.Tick();
	const RemoteAmbientCar *dup = c.AmbientCar(455);
	Check(dup && dup->sameAsVehicle == 95 && dup->poolHandle < 0 && g.carDespawns == 1,
	      "once its host's row puts it on car 95 its copy is taken down, and it is car 95");
	c.HandleMessage(Pack(Row(455, Vec3{1240.0f, -318.0f, 26.0f}, 1300), CH_SNAPSHOT));
	c.Tick();
	Check(g.carSpawns == 1, "and it is not built again, wherever its host says it went");
}

void TestTheMissionsOtherCarsAreBuilt() {
	std::printf("\nthe mission's own cars, and traffic, are built as before\n");
	Client c;
	GuestWithHisCar(c);
	c.HandleMessage(Pack(MissionCar(472, 133, kParked)));
	c.HandleMessage(Pack(MissionCar(473, 94, Vec3{1250.0f, -318.0f, 26.0f})));
	c.HandleMessage(Pack(MissionCar(474, 94, Vec3{1210.5f, -318.0f, 26.0f}, false)));
	c.Tick();
	Check(c.AmbientCar(472)->poolHandle >= 0 && c.AmbientCar(472)->sameAsVehicle == INVALID_NETID,
	      "a van of another model beside the car is built");
	Check(c.AmbientCar(473)->poolHandle >= 0, "one of the same model 40 m off is built");
	Check(c.AmbientCar(474)->poolHandle >= 0 && c.AmbientCar(474)->sameAsVehicle == INVALID_NETID,
	      "and traffic is never taken for one, only the mission's cars are");
	Check(g.carSpawns == 3, "three cars built");
}

} // namespace

int RunMissionTakeTests() {
	TestASessionCarIsNeverTheMissions();
	TestAPlayerOrACopyIsNeverTheMissions();
	TestOneCarStandsInOnePlace();
	TestTheGuestsCarIsNotBuiltTwice();
	TestOneTeleportedFirstIsTakenDownWhenItLands();
	TestTheMissionsOtherCarsAreBuilt();
	return g_takeFailures;
}
