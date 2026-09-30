// A traffic car of ours that our engine drops beside another player:
// client/src/game/carletgo.h and the roster's half of C_CarLetGo. The rules
// walked with no engine, the roster through a bridge of its own, and - when a
// copy of the retail exe is handed over - every address carletgo.h claims,
// read back out of it.

#include "client.h"
#include "game/carletgo.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_letGoFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_letGoFailures;
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

// ---- the rules ---------------------------------------------------------------

void TestTheReapersCallsAreToldApart() {
	std::printf("\nwhich of the reaper's rules took a car, by where Remove returns\n");
	Check(ReapFromReturn(REAP_RETURN_FADED) == CarReap::Faded &&
	          ReapFromReturn(REAP_RETURN_FAR) == CarReap::Far &&
	          ReapFromReturn(REAP_RETURN_STOPPED) == CarReap::Stopped &&
	          ReapFromReturn(REAP_RETURN_WRECK) == CarReap::Wreck,
	      "the four calls into CWorld::Remove are four reasons");
	Check(ReapFromReturn(0x004AE9D5) == CarReap::Other && ReapFromReturn(0) == CarReap::Other,
	      "anything else is something else");
	Check(ReapIsByDistance(CarReap::Stopped) && ReapIsByDistance(CarReap::Far) &&
	          ReapIsByDistance(CarReap::Faded),
	      "stopped behind us, too far and faded out are distances");
	Check(!ReapIsByDistance(CarReap::Wreck) && !ReapIsByDistance(CarReap::Other),
	      "a wreck and anything else are not");
}

void TestOnlyADistanceDropBesideSomebodyIsHandedOn() {
	std::printf("\nwhat is handed on and what is despawned\n");
	const float in  = 150.0f * 150.0f;
	const float out = 200.0f * 200.0f;
	Check(ShouldLetGo(CarReap::Stopped, true, false, in),
	      "a car stopped behind us, 150 m from another player, is handed on");
	Check(!ShouldLetGo(CarReap::Stopped, true, false, out),
	      "200 m from him, past any engine's keeping, it is despawned");
	Check(!ShouldLetGo(CarReap::Stopped, true, false, -1.0f), "nobody else at all, despawned");
	Check(!ShouldLetGo(CarReap::Far, false, false, in),
	      "one the session has not named has nothing to hand on");
	Check(!ShouldLetGo(CarReap::Far, true, true, in), "nor does the mission's own");
	Check(!ShouldLetGo(CarReap::Wreck, true, false, in) &&
	          !ShouldLetGo(CarReap::Other, true, false, in),
	      "nor a wreck, nor a car taken for a reason that holds everywhere");
	Check(AnybodyToHandTo(AMBIENT_CAR_KEEP_RADIUS_M * AMBIENT_CAR_KEEP_RADIUS_M) &&
	          AMBIENT_CAR_KEEP_RADIUS_M == 195.0f,
	      "the edge is the engine's widest keep, 130 m times 1.5, inclusive");

	Check(ShouldLetGo(CarReap::Respawn, true, false, in) &&
	          ShouldLetGo(CarReap::Island, true, false, in),
	      "a respawn's clear and an island left behind hand a car on too");
	Check(!ShouldLetGo(CarReap::Respawn, true, false, out) &&
	          !ShouldLetGo(CarReap::Island, true, true, in),
	      "but not past anybody's keeping, nor the mission's own");
	Check(ReapHandsOn(CarReap::Respawn) && !ReapIsByDistance(CarReap::Respawn) &&
	          !ReapHandsOn(CarReap::Other) && !ReapHandsOn(CarReap::Wreck),
	      "neither is a distance, and a wreck or anything else still is not handed on");

	const Vec3 players[] = {{300.0f, 0.0f, 0.0f}, {0.0f, 40.0f, 500.0f}};
	Check(NearestFlatD2(0.0f, 0.0f, players, 2) == 1600.0f,
	      "the nearest is measured flat, as the reaper measures");
	Check(NearestFlatD2(0.0f, 0.0f, players, 0) < 0.0f, "and nobody is nobody");
}

void TestAPedestrianDroppedBesideSomebodyIsHandedOn() {
	std::printf("\nwhat of our pedestrians is handed on and what is despawned\n");
	const float in  = 90.0f * 90.0f;
	const float out = 100.0f * 100.0f;
	const uint8_t civ = AMBIENT_PEDTYPE_CIVMALE;
	Check(ShouldLetGoPed(PedDrop::Reaper, true, false, true, civ, in) &&
	          ShouldLetGoPed(PedDrop::Respawn, true, false, true, AMBIENT_PEDTYPE_CIVFEMALE, in) &&
	          ShouldLetGoPed(PedDrop::Island, true, false, true, civ, in),
	      "walked away from, a respawn, an island: 90 m from another player, handed on");
	Check(!ShouldLetGoPed(PedDrop::Reaper, true, false, true, civ, out),
	      "100 m from him, past 65 m times 1.5, despawned");
	Check(!ShouldLetGoPed(PedDrop::Reaper, true, false, true, civ, -1.0f),
	      "nobody else at all, despawned");
	Check(!ShouldLetGoPed(PedDrop::Other, true, false, true, civ, in),
	      "a reason that holds everywhere is a despawn");
	Check(!ShouldLetGoPed(PedDrop::Reaper, false, false, true, civ, in) &&
	          !ShouldLetGoPed(PedDrop::Reaper, true, true, true, civ, in),
	      "not one the session has not named, nor the mission's own");
	Check(!ShouldLetGoPed(PedDrop::Reaper, true, false, false, civ, in),
	      "nor a corpse");
	Check(!ShouldLetGoPed(PedDrop::Reaper, true, false, true, AMBIENT_PEDTYPE_COP, in) &&
	          !ShouldLetGoPed(PedDrop::Reaper, true, false, true, 7, in),
	      "nor a cop or a gang member, whom no replica can become");
	Check(AnybodyToHandPedTo(AMBIENT_PED_KEEP_RADIUS_M * AMBIENT_PED_KEEP_RADIUS_M) &&
	          AMBIENT_PED_KEEP_RADIUS_M == 97.5f,
	      "the edge is 97.5 m, inclusive");
}

// ---- the roster ----------------------------------------------------------------

struct Rec {
	Vec3          centre{0.0f, 0.0f, 0.0f};
	int32_t       nextHandle = 1;
	int           carSpawns = 0, carDespawns = 0;
	ViewerAt      viewers[MAX_PLAYERS];
	uint32_t      viewerCount = 0xFFFF;
	LocalCarLetGo queued[2];
	uint32_t      queuedCount = 0;
};
Rec g;

bool CentreStub(Vec3 &out) {
	out = g.centre;
	return true;
}
bool ModelReady(uint16_t) { return true; }
bool SpawnCar(RemoteAmbientCar &c) {
	c.poolHandle = g.nextHandle++;
	++g.carSpawns;
	return true;
}
void DespawnCar(RemoteAmbientCar &c) {
	c.poolHandle = -1;
	++g.carDespawns;
}
void Viewers(const ViewerAt *v, uint32_t n) {
	g.viewerCount = n;
	for (uint32_t i = 0; i < n && i < MAX_PLAYERS; ++i)
		g.viewers[i] = v[i];
}
uint32_t DrainLetGo(LocalCarLetGo *out, uint32_t max) {
	const uint32_t n = g.queuedCount < max ? g.queuedCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g.queued[i];
	g.queuedCount = 0;
	return n;
}

WorldBridge LetGoBridge() {
	g = Rec{};
	WorldBridge b;
	b.IsModelReady             = &ModelReady;
	b.SpawnAmbientCarReplica   = &SpawnCar;
	b.DespawnAmbientCarReplica = &DespawnCar;
	b.SampleCrowdCentre        = &CentreStub;
	b.NoteRemoteViewers        = &Viewers;
	b.DrainLetGoAmbientCars    = &DrainLetGo;
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

S_CarDespawn CarDespawn(uint16_t netId) {
	S_CarDespawn d{};
	InitHeader(d, 1100);
	d.netId = netId;
	return d;
}

void TestTheEngineIsToldWhereEverybodyIs() {
	std::printf("\nthe engine side is told where the other players are\n");
	Client c;
	c.SetBridge(LetGoBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));

	S_PlayerJoin join{};
	InitHeader(join, 1000);
	join.playerId = 3;
	join.netId    = 203;
	join.modelId  = 7;
	std::strncpy(join.nick, "bob", NICK_LEN - 1);
	c.HandleMessage(Pack(join, CH_EVENT));
	c.PushViewersForTest(true);
	Check(g.viewerCount == 0, "a player we have no snapshot of is nowhere yet");

	S_PlayerState state{};
	InitHeader(state, 2000);
	state.playerId    = 3;
	state.body.pos    = {120.0f, -40.0f, 5.0f};
	state.body.health = 100.0f;
	c.HandleMessage(Pack(state, CH_SNAPSHOT));
	c.PushViewersForTest(true);
	Check(g.viewerCount == 1 && g.viewers[0].playerId == 3 && g.viewers[0].pos.x == 120.0f &&
	          g.viewers[0].pos.y == -40.0f,
	      "once he has one, he is there, by his id");
	c.PushViewersForTest(false);
	Check(g.viewerCount == 0, "and out of a session nobody is anywhere");
}

void TestALetGoGoesOut() {
	std::printf("\na car our engine dropped beside somebody goes out as a let-go\n");
	Client c;
	c.SetBridge(LetGoBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	g.queuedCount       = 1;
	g.queued[0].netId   = 812;
	g.queued[0].pedCount = 2;
	g.queued[0].peds[0] = 813;
	g.queued[0].peds[1] = 814;
	const uint32_t before = c.CarLetGosSentForTest();
	c.TickLocalAmbientCarsForTest();
	Check(c.CarLetGosSentForTest() == before + 1, "one C_CarLetGo for the one car");
	c.TickLocalAmbientCarsForTest();
	Check(c.CarLetGosSentForTest() == before + 1, "and nothing when none is queued");
}

uint32_t g_pedQueued = 0;
uint32_t DrainPedLetGo(uint16_t *out, uint32_t max) {
	uint32_t n = 0;
	while (g_pedQueued != 0 && n < max) {
		out[n] = static_cast<uint16_t>(900 + n);
		++n;
		--g_pedQueued;
	}
	return n;
}

void TestPedestrianLetGosGoOutInBatches() {
	std::printf("\npedestrians our engine dropped beside somebody go out as let-gos\n");
	Client c;
	WorldBridge b = LetGoBridge();
	b.DrainLetGoAmbientPeds = &DrainPedLetGo;
	c.SetBridge(b);
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	g_pedQueued = MAX_PED_LET_GO + 3;   // a respawn's worth in one frame
	const uint32_t before = c.PedLetGosSentForTest();
	c.TickLocalAmbientPedsForTest();
	Check(c.PedLetGosSentForTest() == before + MAX_PED_LET_GO + 3 && g_pedQueued == 0,
	      "every one of them goes out in the one frame, a full batch and the rest");
	c.TickLocalAmbientPedsForTest();
	Check(c.PedLetGosSentForTest() == before + MAX_PED_LET_GO + 3,
	      "and nothing when none is queued");
}

void TestAVanishNearUsIsCounted() {
	std::printf("\na traffic car taken away while its copy stood near us is counted\n");
	Client c;
	c.SetBridge(LetGoBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(900, 40.0f), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(901, 1000.0f), CH_EVENT));
	c.Tick();
	Check(g.carSpawns == 1, "the one 40 m away is built, the one a kilometre off is not");

	c.HandleMessage(Pack(CarDespawn(901), CH_EVENT));
	Check(c.NearCarDespawnsForTest() == 0, "one never built here vanished from nobody's view");
	c.HandleMessage(Pack(CarDespawn(900), CH_EVENT));
	Check(c.NearCarDespawnsForTest() == 1 && g.carDespawns == 1,
	      "the one standing 40 m from us is, and is taken down");
}

void TestAFirstBuildIsNotAReturn() {
	std::printf("\nthe radius line tells a new copy from one come back\n");
	Client c;
	c.SetBridge(LetGoBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(900, 40.0f), CH_EVENT));
	c.Tick();   // the first line goes out at once and starts the count again
	g.centre = {400.0f, 0.0f, 0.0f};
	c.Tick();
	Check(g.carDespawns == 1, "we walk away and it is taken down");
	c.HandleMessage(Pack(CarSpawn(902, 390.0f), CH_EVENT));
	c.Tick();
	Check(c.ReplicasNewForTest() == 1 && c.ReplicasRestoredForTest() == 0,
	      "a car we never had built is new, not come back");
	g.centre = {0.0f, 0.0f, 0.0f};
	c.Tick();
	Check(c.ReplicasRestoredForTest() == 1 && c.ReplicasNewForTest() == 1,
	      "walking back, the one taken down comes back, and nothing new is counted twice");
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

float Float(const std::vector<uint8_t> &img, uint32_t va) {
	const uint32_t d = Dword(img, va);
	float f;
	std::memcpy(&f, &d, sizeof f);
	return f;
}

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return At(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

constexpr uint32_t kTextBegin = 0x00401000;
constexpr uint32_t kTextEnd   = 0x005E4000;

void TestTheAddressesAgainstTheImage() {
	std::printf("\nthe reaper's addresses against gta3.exe\n");
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
		if (target == CCarCtrl__PossiblyRemoveVehicle)
			++calls;
		else if (target > CCarCtrl__PossiblyRemoveVehicle &&
		         target < CCarCtrl__PossiblyRemoveVehicle + 5)
			++into;
	}
	Check(calls == 2 &&
	          CallsTo(img, POSSIBLY_REMOVE_VEHICLE_CALLS[0], CCarCtrl__PossiblyRemoveVehicle) &&
	          CallsTo(img, POSSIBLY_REMOVE_VEHICLE_CALLS[1], CCarCtrl__PossiblyRemoveVehicle),
	      "PossiblyRemoveVehicle is reached by exactly the two calls taken");
	Check(into == 0, "and nothing branches into its first bytes");
	Check(At(img, POSSIBLY_REMOVE_VEHICLE_CALLS[0] - 1) == 0x53 &&
	          At(img, 0x00418380) == 0x59 && At(img, 0x004F3680) == 0x52 &&
	          At(img, 0x004F3688) == 0x59,
	      "both push one argument and pop it themselves: __cdecl");

	Check(CallsTo(img, REAP_RETURN_FADED - 5, CWorld__Remove) &&
	          CallsTo(img, REAP_RETURN_FAR - 5, CWorld__Remove) &&
	          CallsTo(img, REAP_RETURN_STOPPED - 5, CWorld__Remove) &&
	          CallsTo(img, REAP_RETURN_WRECK - 5, CWorld__Remove),
	      "each reason's return address follows a call to CWorld::Remove");
	uint32_t removes = 0;
	for (uint32_t va = CCarCtrl__PossiblyRemoveVehicle; va < 0x00418820; ++va)
		if (CallsTo(img, va, CWorld__Remove))
			++removes;
	Check(removes == 4, "and those are all four in the function");

	Check(Dword(img, 0x00418582) == REAP_OFFSCREEN_M_AT &&
	          Dword(img, 0x00418573) == REAP_ONSCREEN_M_AT &&
	          Dword(img, 0x0041859D) == REAP_EXTENDED_AT &&
	          Dword(img, 0x0041869D) == REAP_STOPPED_M_AT &&
	          Dword(img, 0x004187B4) == REAP_WRECK_M2_AT,
	      "the thresholds are read where carletgo.h says");
	Check(Float(img, REAP_OFFSCREEN_M_AT) == 50.0f && Float(img, REAP_ONSCREEN_M_AT) == 130.0f &&
	          Float(img, REAP_EXTENDED_AT) == 1.5f && Float(img, REAP_STOPPED_M_AT) == 25.0f &&
	          Float(img, REAP_WRECK_M2_AT) == 56.25f,
	      "50, 130, 1.5, 25 and 7.5 squared");
	Check(AMBIENT_CAR_KEEP_RADIUS_M ==
	          Float(img, REAP_ONSCREEN_M_AT) * Float(img, REAP_EXTENDED_AT),
	      "and the session's keep radius is the widest of them");

	// The pedestrian reaper's one call, and RemovePed's own shape.
	uint32_t removePedCalls = 0, removePedInto = 0;
	for (uint32_t va = kTextBegin; va + 6 <= kTextEnd; ++va) {
		if (At(img, va) != 0xE8 && At(img, va) != 0xE9)
			continue;
		const uint32_t target = va + 5 + Dword(img, va + 1);
		if (target > CPopulation__RemovePed && target < CPopulation__RemovePed + 5)
			++removePedInto;
		if (va >= 0x004F3B90 && va < 0x004F4391 &&
		    CallsTo(img, va, CPopulation__RemovePed))
			++removePedCalls;
	}
	Check(removePedCalls == 1 &&
	          CallsTo(img, MANAGE_POPULATION_REMOVE_PED_CALL, CPopulation__RemovePed) &&
	          At(img, MANAGE_POPULATION_REMOVE_PED_CALL - 1) == 0x53 &&
	          At(img, MANAGE_POPULATION_REMOVE_PED_CALL + 5) == 0x59,
	      "ManagePopulation reaches RemovePed once, `push ebx / call / pop ecx`");
	Check(removePedInto == 0 && At(img, CPopulation__RemovePed) == 0x53 &&
	          CallsTo(img, CPopulation__RemovePed + 6, CWorld__Remove),
	      "RemovePed is `push ebx / mov ebx,[esp+8] / push ebx / call CWorld::Remove`");
	Check(Dword(img, 0x004F3F64) == 0x005FA894 && Float(img, 0x005FA894) == 65.0f &&
	          AMBIENT_PED_KEEP_RADIUS_M == 65.0f * 1.5f,
	      "the reaper's widest distance is 65 m, and the keep radius is it times 1.5");

	Check(CallsTo(img, ISLAND_CAR_REMOVE_RETURN - 5, CWorld__Remove) &&
	          CallsTo(img, ISLAND_PED_REMOVE_RETURN - 5, CWorld__Remove) &&
	          At(img, ISLAND_CAR_REMOVE_RETURN - 6) == 0x53 &&
	          At(img, ISLAND_PED_REMOVE_RETURN - 6) == 0x53,
	      "the island's two removals follow `push ebx / call CWorld::Remove`");
	Check(CallsTo(img, 0x004F5D0C, CVehicle__CanBeDeleted) &&
	          CallsTo(img, 0x004F5F04, 0x004CF8B0) &&
	          CallsTo(img, 0x004F39B5, CPopulation__MoveCarsAndPedsOutOfAbandonedZones),
	      "after the car's and the ped's CanBeDeleted, in the function Update calls");
}

} // namespace

int RunCarLetGoTests() {
	TestTheReapersCallsAreToldApart();
	TestOnlyADistanceDropBesideSomebodyIsHandedOn();
	TestAPedestrianDroppedBesideSomebodyIsHandedOn();
	TestTheEngineIsToldWhereEverybodyIs();
	TestALetGoGoesOut();
	TestPedestrianLetGosGoOutInBatches();
	TestAVanishNearUsIsCounted();
	TestAFirstBuildIsNotAReturn();
	TestTheAddressesAgainstTheImage();
	return g_letGoFailures;
}
