// Somebody else's crowd, built only near us: client/src/game/crowdrange.h and
// Client::UpdateReplicaRange. The rules walked with no engine, the roster
// driven through a bridge of its own, and - when a copy of the retail exe is
// handed over - every byte game/crowdaddr.h claims, read back out of it.

#include "client.h"
#include "game/crowdaddr.h"
#include "game/crowdrange.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_crowdFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_crowdFailures;
}

template <class T>
Message Pack(const T &pkt, Channel ch) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = ch;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

// ---- the rules -------------------------------------------------------------

void TestTheRadiusHasAGap() {
	std::printf("\nsomebody else's crowd is built near us and nowhere else\n");
	Check(ReplicaInRange(false, 100.0f, 100.0f), "141 m away is built");
	Check(!ReplicaInRange(false, 170.0f, 0.0f), "170 m away is not built yet");
	Check(ReplicaInRange(true, 0.0f, 190.0f), "but one already built is kept at 190 m");
	Check(!ReplicaInRange(true, 150.0f, 150.0f), "and taken down past 200 m");
	Check(ReplicaInRange(false, -REPLICA_BUILD_M, 0.0f) &&
	          !ReplicaInRange(true, REPLICA_DROP_M + 0.5f, 0.0f),
	      "both edges are the constants, either direction");
}

void TestThePoliceDecisionIsTheEngines() {
	std::printf("\nGenerateOneRandomCar's police arm\n");
	Check(!PoliceCarDue(1, 0, 3, 0, 4, 100000, 0), "one star makes no police car");
	Check(PoliceCarDue(4, 0, 3, 0, 8, 100, 100), "four stars make one at once");
	Check(!PoliceCarDue(4, 3, 3, 0, 8, 100000, 0), "not with the law cars at the cap");
	Check(!PoliceCarDue(4, 0, 3, 8, 8, 100000, 0), "nor with the cops at theirs");
	Check(!PoliceCarDue(3, 0, 2, 0, 6, 5000, 0) && PoliceCarDue(3, 0, 2, 0, 6, 5001, 0),
	      "three stars wait five seconds since the last one");
	Check(!PoliceCarDue(2, 0, 2, 0, 4, 8000, 0) && PoliceCarDue(2, 0, 2, 0, 4, 8001, 0),
	      "two stars wait eight");
}

void TestRoomIsMadeOnlyForPolice() {
	std::printf("\nother machines' traffic left out of the count only for our police\n");
	ReplicaTraffic theirs;
	theirs.random = 6;
	theirs.law    = 2;

	TrafficRoom room = TrafficRoomForPolice(theirs, 9, 2, 0, 0, 0, 0, 100000, 0);
	Check(!room.Any(), "not wanted: their traffic shares our street, as it always did");

	room = TrafficRoomForPolice(theirs, 9, 2, 4, 3, 0, 8, 100000, 0);
	Check(room.random == 6 && room.law == 2 && room.cap == 8,
	      "wanted: their six cars and two police are left out, and the cap raised by "
	      "the eight terms they add to the sum");

	room = TrafficRoomForPolice(theirs, 9, 3, 4, 3, 0, 8, 100000, 0);
	Check(room.random == 6 && room.law == 2,
	      "our own police car at the cap with theirs left out still lets one come");

	room = TrafficRoomForPolice(theirs, 9, 3, 4, 1, 0, 8, 100000, 0);
	Check(!room.Any(), "but not past the cap on our own police alone");

	room = TrafficRoomForPolice(theirs, 9, 2, 2, 2, 0, 4, 1000, 0);
	Check(!room.Any(), "nor while the engine would not make one yet anyway");

	room = TrafficRoomForPolice(theirs, 4, 1, 4, 3, 0, 8, 100000, 0);
	Check(room.random == 4 && room.law == 1, "never more than the counters hold");
}

// ---- the roster --------------------------------------------------------------

struct Rec {
	Vec3 centre{0.0f, 0.0f, 0.0f};
	bool haveCentre = true;
	int  pedSpawns = 0, pedDespawns = 0, carSpawns = 0, carDespawns = 0;
	int  unseats = 0, seats = 0, unseatsAtCarDespawn = -1;
	Vec3 lastPedAt{}, lastCarAt{};
	int32_t nextHandle = 1;
	uint32_t noted = 0;
	std::vector<Vec3> unbuiltCars, unbuiltPeds;
	bool unbuiltNoted = false;
};
Rec g;

void NotedUnbuilt(const Vec3 *cars, uint32_t carCount, const Vec3 *peds, uint32_t pedCount) {
	g.unbuiltCars.assign(cars, cars + carCount);
	g.unbuiltPeds.assign(peds, peds + pedCount);
	g.unbuiltNoted = true;
}

bool CentreStub(Vec3 &out) {
	out = g.centre;
	return g.haveCentre;
}
bool ModelReady(uint16_t) { return true; }
bool SpawnPed(RemoteAmbientPed &p) {
	p.poolHandle = g.nextHandle++;
	++g.pedSpawns;
	g.lastPedAt = p.body.pos;
	return true;
}
void DespawnPed(RemoteAmbientPed &p) {
	p.poolHandle = -1;
	++g.pedDespawns;
}
bool SpawnCar(RemoteAmbientCar &c) {
	c.poolHandle = g.nextHandle++;
	++g.carSpawns;
	g.lastCarAt = c.body.pos;
	return true;
}
void DespawnCar(RemoteAmbientCar &c) {
	c.poolHandle = -1;
	++g.carDespawns;
	g.unseatsAtCarDespawn = g.unseats;
}
int32_t SeatPed(RemoteAmbientPed &, int32_t, uint16_t, uint8_t seat) {
	++g.seats;
	return seat;
}
void UnseatPed(RemoteAmbientPed &) { ++g.unseats; }
void Noted(const int32_t *, uint32_t n) { g.noted = n; }

WorldBridge CrowdBridge() {
	g = Rec{};
	WorldBridge b;
	b.IsModelReady             = &ModelReady;
	b.SpawnAmbientReplica      = &SpawnPed;
	b.DespawnAmbientReplica    = &DespawnPed;
	b.SpawnAmbientCarReplica   = &SpawnCar;
	b.DespawnAmbientCarReplica = &DespawnCar;
	b.SeatAmbientPed           = &SeatPed;
	b.UnseatAmbientPed         = &UnseatPed;
	b.SampleCrowdCentre        = &CentreStub;
	b.NoteBuiltCarReplicas     = &Noted;
	b.NoteUnbuiltCrowd         = &NotedUnbuilt;
	return b;
}

S_Welcome Welcome() {
	S_Welcome w{};
	InitHeader(w, 1000);
	w.playerId     = 0;
	w.netId        = 100;
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hostPlayerId = INVALID_PLAYER;
	return w;
}

S_PedSpawn PedSpawn(uint16_t netId, float x, uint8_t flags = 0) {
	S_PedSpawn s{};
	InitHeader(s, 1000);
	s.ownerPlayerId = 1;
	s.netId         = netId;
	s.body.modelId  = 7;
	s.body.flags    = flags;
	s.body.pos      = {x, 0.0f, 10.0f};
	return s;
}

S_CarSpawn CarSpawn(uint16_t netId, float x) {
	S_CarSpawn s{};
	InitHeader(s, 1000);
	s.ownerPlayerId = 1;
	s.netId         = netId;
	s.body.modelId  = 90;
	s.body.extra1   = -1;
	s.body.extra2   = -1;
	s.body.pos      = {x, 0.0f, 10.0f};
	s.body.rot      = {0.0f, 0.0f, 0.0f, 1.0f};
	return s;
}

S_PedStates PedRow(uint16_t netId, uint32_t timeMs, float x,
                   uint16_t car = INVALID_NETID) {
	S_PedStates s{};
	InitHeader(s, timeMs);
	s.ownerPlayerId        = 1;
	s.count                = 1;
	s.peds[0].netId        = netId;
	s.peds[0].animId       = ANIM_NONE;
	s.peds[0].vehicleNetId = car;
	s.peds[0].pos          = {x, 0.0f, 10.0f};
	return s;
}

void TestAPedestrianFarAwayIsHeldNotBuilt() {
	std::printf("\na pedestrian on the far side of the city is not built here\n");
	Client c;
	c.SetBridge(CrowdBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(PedSpawn(700, 900.0f), CH_EVENT));
	c.Tick();
	Check(g.pedSpawns == 0, "900 m away, nothing is built");
	Check(c.AmbientPed(700) != nullptr && c.AmbientPed(700)->spawnPending,
	      "but the row is held and the spawn armed");

	g.haveCentre = false;
	g.centre     = {850.0f, 0.0f, 0.0f};
	c.Tick();
	Check(g.pedSpawns == 0, "with no player to measure from, nothing changes");

	g.haveCentre = true;
	c.Tick();
	Check(g.pedSpawns == 1, "we walk up to him and he is built");

	c.HandleMessage(Pack(PedRow(700, 2000, 930.0f), CH_SNAPSHOT));
	g.centre = {760.0f, 0.0f, 0.0f};
	c.Tick();
	Check(g.pedDespawns == 0, "170 m from us, the one built is kept");

	g.centre = {700.0f, 0.0f, 0.0f};
	c.Tick();
	Check(g.pedDespawns == 1, "230 m and he is taken down");
	Check(c.AmbientPed(700) != nullptr && c.AmbientPed(700)->poolHandle < 0 &&
	          c.AmbientPed(700)->spawnPending,
	      "the row stays, armed to be built again");

	c.Tick();
	c.Tick();
	Check(g.pedSpawns == 1 && g.pedDespawns == 1, "and nothing flaps while he stays out");

	g.centre = {900.0f, 0.0f, 0.0f};
	c.Tick();
	Check(g.pedSpawns == 2, "back in reach he is built again");
	Check(g.lastPedAt.x == 930.0f, "where his host last put him, not where he was born");
}

void TestTheSpotRuleRefusesOnlyTheSpot() {
	std::printf("\nour generators pass over a spot another machine's unbuilt crowd stands on\n");
	const Vec3 cars[] = {{200.0f, 0.0f, 10.0f}};
	const Vec3 peds[] = {{50.0f, 50.0f, 10.0f}};
	Check(SpotTakenByUnbuilt({201.0f, 1.0f, 10.0f}, 2.5f, true, true, cars, 1, peds, 1),
	      "a car's spot a metre from an unbuilt car is taken");
	Check(!SpotTakenByUnbuilt({210.0f, 0.0f, 10.0f}, 2.5f, true, true, cars, 1, peds, 1),
	      "ten metres on, the street is free");
	Check(SpotTakenByUnbuilt({215.0f, 0.0f, 10.0f}, 22.5f, true, false, cars, 1, peds, 1),
	      "the new car's wide test, bounding radius plus twenty, sees it as the engine would");
	Check(SpotTakenByUnbuilt({50.5f, 50.0f, 10.0f}, 0.75f, true, true, cars, 1, peds, 1) &&
	          !SpotTakenByUnbuilt({52.0f, 50.0f, 10.0f}, 0.75f, true, true, cars, 1, peds, 1),
	      "a pedestrian's spot half a metre from an unbuilt pedestrian is taken, two is not");
	Check(!SpotTakenByUnbuilt({50.5f, 50.0f, 10.0f}, 0.75f, true, false, cars, 1, peds, 1),
	      "a test that does not ask about peds is not answered with one");
	Check(!SpotTakenByUnbuilt({201.0f, 0.0f, 10.0f}, 2.5f, true, true, nullptr, 0, nullptr, 0),
	      "with nothing unbuilt, nothing is refused");
	Check(UnbuiltWorthNoting(true, 250.0f, 100.0f) && !UnbuiltWorthNoting(true, 400.0f, 0.0f) &&
	          UnbuiltWorthNoting(false, 100.0f, 0.0f) && !UnbuiltWorthNoting(false, 200.0f, 0.0f),
	      "cars are noted out to 300 m, pedestrians to 150");
}

void TestTheUnbuiltCrowdIsNoted() {
	std::printf("\nwhat of the other machines' crowd near us is not built is noted\n");
	Client c;
	c.SetBridge(CrowdBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(800, 40.0f), CH_EVENT));    // built
	c.HandleMessage(Pack(CarSpawn(801, 180.0f), CH_EVENT));   // past 160 m
	c.HandleMessage(Pack(CarSpawn(802, 900.0f), CH_EVENT));   // across the city
	c.HandleMessage(Pack(PedSpawn(810, 30.0f), CH_EVENT));    // built
	c.HandleMessage(Pack(PedSpawn(811, 900.0f), CH_EVENT));   // across the city
	c.Tick();
	Check(g.unbuiltNoted, "the engine side is told every frame");
	Check(g.unbuiltCars.size() == 1 && g.unbuiltCars[0].x == 180.0f,
	      "the car 180 m off, not built, is noted; the built one and the far one are not");
	Check(g.unbuiltPeds.empty(), "the built pedestrian is not, nor the one 900 m off");
}

void TestAMissionPedestrianIsAlwaysBuilt() {
	std::printf("\na mission's pedestrian is built wherever he is\n");
	Client c;
	c.SetBridge(CrowdBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(PedSpawn(701, 2000.0f, AMBIENT_MISSION), CH_EVENT));
	c.Tick();
	Check(g.pedSpawns == 1, "two kilometres away and built");
}

void TestADriverGoesWithHisCar() {
	std::printf("\na traffic car out of reach, and the man driving it\n");
	Client c;
	c.SetBridge(CrowdBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(PedSpawn(702, 50.0f), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(900, 50.0f), CH_EVENT));
	c.Tick();
	c.HandleMessage(Pack(PedRow(702, 2000, 50.0f, 900), CH_SNAPSHOT));
	c.Tick();
	Check(g.pedSpawns == 1 && g.carSpawns == 1 && g.seats == 1, "both built, and he drives");
	Check(g.noted == 1, "the built car is told to the generator's police arm");

	// His own row still says he is near; his car says it is not.
	S_PedStates row = PedRow(702, 2100, 50.0f, 900);
	c.HandleMessage(Pack(row, CH_SNAPSHOT));
	g.centre = {-400.0f, 0.0f, 0.0f};
	c.Tick();
	Check(g.carDespawns == 1 && g.pedDespawns == 1, "the car and its driver go together");
	Check(g.unseatsAtCarDespawn == 1, "and he was out of the seat before the car went");
	Check(c.AmbientPed(702)->seatVehicleNetId == 900,
	      "the instruction to drive it stays, since his host still says so");
	Check(g.noted == 0, "and the generator hears the car is gone");

	g.centre = {0.0f, 0.0f, 0.0f};
	c.Tick();
	Check(g.pedSpawns == 2 && g.carSpawns == 2, "back in reach both are built");
	Check(g.seats == 2, "and he sits down again");
}

int g_quietWrecks = 0;
UnownedWreckOutcome WreckReplicaStub(RemoteAmbientCar &, const BlastTransform &) {
	return UnownedWreckOutcome::Wrecked;
}
bool QuietWreckStub(RemoteAmbientCar &) {
	++g_quietWrecks;
	return true;
}

void TestAWreckOutOfReachComesBackAWreck() {
	std::printf("\na traffic wreck taken down out of reach, and coming back\n");
	for (int withSeam = 1; withSeam >= 0; --withSeam) {
		Client      c;
		WorldBridge b              = CrowdBridge();
		b.WreckAmbientCarReplica   = &WreckReplicaStub;
		b.WreckAmbientCarQuietly   = withSeam ? &QuietWreckStub : nullptr;
		g_quietWrecks              = 0;
		c.SetBridge(b);
		c.HandleMessage(Pack(Welcome(), CH_EVENT));
		c.HandleMessage(Pack(CarSpawn(902, 50.0f), CH_EVENT));
		c.Tick();

		S_UnownedBlowUp blow{};
		InitHeader(blow, 1500);
		blow.reporterPlayerId = 1;
		blow.key.kind         = UNOWNED_AMBIENT;
		blow.key.id           = 902;
		c.HandleMessage(Pack(blow, CH_EVENT));
		c.Tick();
		const RemoteAmbientCar *car = c.AmbientCar(902);
		if (withSeam)
			Check(g.carSpawns == 1 && car && car->destroyed, "built, and then its host blows it up");

		g.centre = {-400.0f, 0.0f, 0.0f};
		c.Tick();
		if (withSeam)
			Check(g.carDespawns == 1, "we walk off and the shell is taken down");

		g.centre = {0.0f, 0.0f, 0.0f};
		c.Tick();
		c.Tick();
		if (withSeam) {
			Check(g.carSpawns == 2 && g_quietWrecks == 1,
			      "back in reach it is built again, as the shell, without a blast");
			Check(car && !car->stowed, "and only once: a shell the engine reaps stays gone");
		} else {
			Check(g.carSpawns == 1 && g_quietWrecks == 0,
			      "with no way to make a shell quietly it stays unbuilt, as before");
		}
	}
}

void TestNoRadiusBuildsEverything() {
	std::printf("\na bridge with no centre builds everything, as before\n");
	Client c;
	WorldBridge b = CrowdBridge();
	b.SampleCrowdCentre = nullptr;
	c.SetBridge(b);
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(PedSpawn(703, 5000.0f), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(901, 5000.0f), CH_EVENT));
	c.Tick();
	Check(g.pedSpawns == 1 && g.carSpawns == 1, "built at any distance");
}

// ---- against the real exe ------------------------------------------------------

bool LoadExe(std::vector<uint8_t> &image, std::string &from) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_GTA3_EXE"))
		candidates.push_back(env);
	candidates.push_back("reference/bin/gta3.exe");
	candidates.push_back("../../../../reference/bin/gta3.exe");
	for (const std::string &path : candidates) {
		FILE *fh = std::fopen(path.c_str(), "rb");
		if (!fh)
			continue;
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		image.resize(size > 0 ? size_t(size) : 0);
		const size_t got = image.empty() ? 0 : std::fread(image.data(), 1, image.size(), fh);
		std::fclose(fh);
		if (got == image.size() && image.size() == IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

uint8_t At(const std::vector<uint8_t> &img, uint32_t va) { return img[va - IMAGE_BASE]; }

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return At(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

constexpr uint32_t kTextBegin = 0x00401000;
constexpr uint32_t kTextEnd   = 0x005E4000;

void TestTheAddressesAgainstTheImage() {
	std::printf("\nthe crowd's addresses against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check them\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	uint32_t calls = 0, into = 0;
	for (uint32_t va = kTextBegin; va + 6 <= kTextEnd; ++va) {
		const uint8_t b = At(img, va);
		uint32_t      target = 0;
		if (b == 0xE8 || b == 0xE9)
			target = va + 5 + Dword(img, va + 1);
		else if (b == 0x0F && (At(img, va + 1) & 0xF0) == 0x80)
			target = va + 6 + Dword(img, va + 2);
		else
			continue;
		if (target == CCarCtrl__GenerateOneRandomCar)
			++calls;
		else if (target > CCarCtrl__GenerateOneRandomCar &&
		         target < CCarCtrl__GenerateOneRandomCar + 10)
			++into;
	}
	Check(calls == 2 && CallsTo(img, GENERATE_ONE_RANDOM_CAR_CALLS[0],
	                            CCarCtrl__GenerateOneRandomCar) &&
	          CallsTo(img, GENERATE_ONE_RANDOM_CAR_CALLS[1], CCarCtrl__GenerateOneRandomCar),
	      "GenerateOneRandomCar is reached by exactly the two calls taken");
	Check(into == 0, "and nothing branches into its first bytes");

	Check(At(img, 0x00416789) == 0xA1 && Dword(img, 0x0041678A) ==
	                                         CCarCtrl__LastTimeLawEnforcerCreated &&
	          At(img, 0x00417D2B) == 0xA3 &&
	          Dword(img, 0x00417D2C) == CCarCtrl__LastTimeLawEnforcerCreated,
	      "LastTimeLawEnforcerCreated is read by the police arm and written after a "
	      "police car");
	Check(Dword(img, 0x0041678F) == 0x1388 && Dword(img, 0x004167A1) == 0x1F40,
	      "five and eight seconds, as PoliceCarDue has them");
	Check(Dword(img, 0x00416759) == 0x53C && At(img, 0x0041675F) == 0x18 &&
	          At(img, 0x0041676A) == 0x12 && At(img, 0x00416775) == 0x11 &&
	          At(img, 0x00416778) == 0x10,
	      "the wanted level, the law car cap and the two cop counts at the offsets used");

	Check(std::memcmp(&img[ADRENALINE_SLOWDOWN_STORE - IMAGE_BASE], ADRENALINE_SLOWDOWN_BYTES,
	                  sizeof ADRENALINE_SLOWDOWN_BYTES) == 0,
	      "the adrenaline slow-down is the ten bytes taken out");
	uint32_t thirds = 0;
	for (uint32_t va = kTextBegin; va + 10 <= kTextEnd; ++va)
		if (std::memcmp(&img[va - IMAGE_BASE], ADRENALINE_SLOWDOWN_BYTES,
		                sizeof ADRENALINE_SLOWDOWN_BYTES) == 0)
			++thirds;
	Check(thirds == 1, "and the only store of a third into the time scale in the image");
	Check(At(img, 0x004F11E8) == 0x83 && At(img, 0x004F11F7) == 0x74,
	      "the branch after it tests the compare before it, not the store");

	Check(At(img, FindPlayerCentreOfWorld) == 0x80 &&
	          Dword(img, FindPlayerCentreOfWorld + 2) == 0x0095CD8A &&
	          Dword(img, 0x004A118D) == 0x009412F4 && Dword(img, 0x004A11B5) == 0x009412F0,
	      "FindPlayerCentreOfWorld: the replay camera, the remote car, the ped");
	Check(CallsTo(img, 0x0041662F, FindPlayerCentreOfWorld) &&
	          CallsTo(img, 0x004F4A46, FindPlayerCentreOfWorld),
	      "and both generators centre on it");

	bool spots = true;
	for (uint32_t site : SPAWN_SPOT_TESTS)
		spots = spots && CallsTo(img, site, CWorld__FindObjectsKindaColliding) &&
		        At(img, site + 5) == 0x83 && At(img, site + 6) == 0xC4 &&
		        At(img, site + 7) == 0x2C;
	Check(spots, "the four spot tests call FindObjectsKindaColliding and pop eleven dwords");
	Check(SPAWN_SPOT_TESTS[0] > CCarCtrl__GenerateOneRandomCar &&
	          SPAWN_SPOT_TESTS[2] < 0x00417E00 &&
	          SPAWN_SPOT_TESTS[3] > CPedPlacement__IsPositionClearForPed &&
	          SPAWN_SPOT_TESTS[3] < CPedPlacement__IsPositionClearForPed + 0x40,
	      "three inside GenerateOneRandomCar, one inside IsPositionClearForPed");
	uint32_t clearCalls = 0;
	for (uint32_t va = kTextBegin; va + 5 <= kTextEnd; ++va)
		if (CallsTo(img, va, CPedPlacement__IsPositionClearForPed))
			++clearCalls;
	Check(clearCalls == 1 &&
	          CallsTo(img, AddToPopulation_IsPositionClearCall, CPedPlacement__IsPositionClearForPed) &&
	          AddToPopulation_IsPositionClearCall > CPopulation__AddToPopulation,
	      "and IsPositionClearForPed's only caller is AddToPopulation");
	// No list at any of them: the pushed list pointer is `push 0`.
	Check(At(img, 0x00416B6C) == 0x6A && At(img, 0x00416B6D) == 0x00 &&
	          At(img, 0x004EE2D1) == 0x6A && At(img, 0x004EE2D2) == 0x00,
	      "the car's spawn-point test and the ped's pass no list");
}

} // namespace

int RunCrowdRangeTests() {
	TestTheRadiusHasAGap();
	TestThePoliceDecisionIsTheEngines();
	TestRoomIsMadeOnlyForPolice();
	TestAPedestrianFarAwayIsHeldNotBuilt();
	TestTheSpotRuleRefusesOnlyTheSpot();
	TestTheUnbuiltCrowdIsNoted();
	TestAMissionPedestrianIsAlwaysBuilt();
	TestADriverGoesWithHisCar();
	TestNoRadiusBuildsEverything();
	TestAWreckOutOfReachComesBackAWreck();
	TestTheAddressesAgainstTheImage();
	return g_crowdFailures;
}
