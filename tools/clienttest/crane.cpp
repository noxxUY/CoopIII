// The cranes on every machine (game/crane.h): when a crane is busy, whose state
// is stale, what the roster does with another machine's crane, a car one of
// ours goes for, and a car crushed somewhere else - and, with a copy of the
// retail exe, every address and member the engine half leans on.

#include "client.h"
#include "game/addresses.h"
#include "game/crane.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

// ---- the rules ------------------------------------------------------------

void TestWhenACraneIsBusy() {
	std::printf("\nthe cranes: when one is busy\n");
	Check(!CraneBusy(CRANE_IDLE, false), "idle with nothing on it is every machine's own");
	Check(CraneBusy(CRANE_IDLE, true), "a car named on it is not idle, whatever the state");
	Check(CraneBusy(CRANE_GOING_TOWARDS_TARGET, true) && CraneBusy(CRANE_DROPPING_TARGET, false),
	      "going for a car, and dropping one after the car is off it");
	Check(CraneCarries(CRANE_GOING_TOWARDS_TARGET_ONLY_HEIGHT) &&
	          CraneCarries(CRANE_LIFTING_TARGET) && CraneCarries(CRANE_ROTATING_TARGET),
	      "the car hangs from the hook in IsThisCarBeingCarriedByAnyCrane's three states");
	Check(!CraneCarries(CRANE_GOING_TOWARDS_TARGET) && !CraneCarries(CRANE_DROPPING_TARGET) &&
	          !CraneCarries(CRANE_IDLE),
	      "and in no other");
}

void TestFollowingAndItsOrder() {
	std::printf("\nthe cranes: following another machine's\n");
	Check(!CraneFollowLapsed(1000, 1000 + CRANE_FOLLOW_TIMEOUT_MS - 1), "heard a moment ago");
	Check(CraneFollowLapsed(1000, 1000 + CRANE_FOLLOW_TIMEOUT_MS), "quiet for the timeout");
	Check(CraneFollowLapsed(0xFFFFFF00u, 0xFFFFFF00u + CRANE_FOLLOW_TIMEOUT_MS),
	      "across the clock's wrap");

	Check(CraneStateIsOld(2, 500, true, 2, 600, false, INVALID_PLAYER, 0),
	      "a busy state sent before that machine's end is old (the end can overtake it)");
	Check(!CraneStateIsOld(2, 700, true, 2, 600, false, INVALID_PLAYER, 0),
	      "one sent after it starts the crane again");
	Check(!CraneStateIsOld(3, 500, true, 2, 600, false, INVALID_PLAYER, 0),
	      "another machine's end says nothing about this one's clock");
	Check(CraneStateIsOld(2, 400, false, INVALID_PLAYER, 0, true, 2, 450),
	      "a state older than the one followed is dropped");
	Check(!CraneStateIsOld(3, 400, false, INVALID_PLAYER, 0, true, 2, 450),
	      "unless it comes from a machine the session has given the crane to since");
}

void TestTheStateOnTheWire() {
	std::printf("\nthe cranes: what goes on the wire\n");
	CraneStateBody b{};
	b.craneX = 1119.0f;
	b.craneY = 48.0f;
	b.active = 1;
	b.state  = CRANE_LIFTING_TARGET;
	Check(CraneStateSane(b), "a crane at the Portland crusher");
	b.state = 6;
	Check(!CraneStateSane(b), "a state the engine does not have");
	b.state     = CRANE_IDLE;
	b.hookAngle = std::strtof("nan", nullptr);
	Check(!CraneStateSane(b), "a NaN");
	b.hookAngle = 1e9f;
	Check(!CraneStateSane(b), "a number off the map");
	b.hookAngle = 0.0f;
	b.active    = 2;
	Check(!CraneStateSane(b), "an active byte that is not 0 or 1");
	Check(SameCrane(1119.0f, 48.0f, 1119.5f, 47.6f), "the same building, give or take");
	Check(!SameCrane(1119.0f, 48.0f, 1121.0f, 48.0f), "two metres off is another crane");
}

// ---- the roster -----------------------------------------------------------

struct Rec {
	int            applied        = 0;
	CraneStateBody lastBody{};
	uint8_t        lastFrom       = INVALID_PLAYER;
	uint32_t       lastSent       = 0;
	int            crushed        = 0;
	int32_t        crushedHandle  = -1;
	uint16_t       cranedNetId    = INVALID_NETID;
	uint8_t        samples        = 0;
	CraneStateBody sample[2];
};
Rec g_rec;

bool RecModelReady(uint16_t) { return true; }
void RecRequestModel(uint16_t) {}
bool RecSpawn(RemoteVehicle &v) {
	v.poolHandle = 1000 + v.netId;
	return true;
}
void RecDespawn(RemoteVehicle &v) { v.poolHandle = -1; }
void RecRelease(RemoteVehicle &) {}
void RecCorrect(RemoteVehicle &, const VehicleTransform &) {}
void RecApply(const CraneStateBody &b, uint8_t from, uint32_t sentMs) {
	++g_rec.applied;
	g_rec.lastBody = b;
	g_rec.lastFrom = from;
	g_rec.lastSent = sentMs;
}
bool RecCraneHas(const RemoteVehicle &v) { return v.netId == g_rec.cranedNetId; }
void RecCrushed(int32_t handle) {
	++g_rec.crushed;
	g_rec.crushedHandle = handle;
}
uint8_t RecSample(CraneStateBody *out, uint8_t max) {
	uint8_t n = 0;
	for (; n < g_rec.samples && n < max; ++n)
		out[n] = g_rec.sample[n];
	g_rec.samples = 0;
	return n;
}

WorldBridge Bridge() {
	g_rec = Rec{};
	WorldBridge b;
	b.RequestModel            = &RecRequestModel;
	b.IsModelReady            = &RecModelReady;
	b.SpawnRemoteVehicle      = &RecSpawn;
	b.DespawnRemoteVehicle    = &RecDespawn;
	b.ReleaseOwnVehicle       = &RecRelease;
	b.CorrectRemoteVehicle    = &RecCorrect;
	b.ApplyCraneState         = &RecApply;
	b.CraneHasVehicle         = &RecCraneHas;
	b.NoteCarCrushedElsewhere = &RecCrushed;
	b.SampleCranes            = &RecSample;
	return b;
}

template <class T>
Message Wrap(const T &pkt, uint8_t channel = CH_EVENT) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = channel;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

S_Welcome Welcome(uint8_t playerId, uint8_t host) {
	S_Welcome w;
	InitHeader(w, 1000);
	w.playerId     = playerId;
	w.netId        = uint16_t(100 + playerId);
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hour         = 12;
	w.hostPlayerId = host;
	return w;
}

S_PlayerJoin Join(uint8_t playerId) {
	S_PlayerJoin j{};
	InitHeader(j, 1000);
	j.playerId = playerId;
	j.netId    = uint16_t(100 + playerId);
	std::strcpy(j.nick, "other");
	return j;
}

S_VehicleSpawn Spawn(uint16_t netId) {
	S_VehicleSpawn s;
	InitHeader(s, 1000);
	s.netId   = netId;
	s.modelId = 116;
	s.pos     = {1140.0f, 50.0f, 0.0f};
	s.rot     = {0.0f, 0.0f, 0.0f, 1.0f};
	s.health  = 1000.0f;
	s.extra1  = -1;
	s.extra2  = -1;
	return s;
}

S_CraneState Crane(uint8_t from, uint8_t state, uint16_t netId, uint32_t sentMs) {
	S_CraneState s{};
	InitHeader(s, sentMs);
	s.playerId        = from;
	s.body.craneX     = 1119.0f;
	s.body.craneY     = 48.0f;
	s.body.active     = 1;
	s.body.state      = state;
	s.body.netId      = netId;
	s.body.hookAngle  = 0.5f;
	s.body.hookOffset = 20.0f;
	s.body.hookHeight = 12.0f;
	return s;
}

void TestAnotherMachinesCraneReachesTheSeam() {
	std::printf("\nthe cranes: another machine's, heard\n");
	Client c;
	c.SetBridge(Bridge());
	c.HandleMessage(Wrap(Welcome(0, 0)));
	c.HandleMessage(Wrap(Join(1)));

	c.HandleMessage(Wrap(Crane(1, CRANE_LIFTING_TARGET, 40, 5000), CH_SNAPSHOT));
	Check(g_rec.applied == 1 && g_rec.lastFrom == 1 && g_rec.lastSent == 5000 &&
	          g_rec.lastBody.state == CRANE_LIFTING_TARGET && g_rec.lastBody.netId == 40,
	      "goes to the engine with who sent it and when, on its clock");

	c.HandleMessage(Wrap(Crane(0, CRANE_LIFTING_TARGET, 40, 5100), CH_SNAPSHOT));
	Check(g_rec.applied == 1, "our own, echoed, is not followed");

	S_CraneState bad = Crane(1, 9, 40, 5200);
	c.HandleMessage(Wrap(bad, CH_SNAPSHOT));
	Check(g_rec.applied == 1, "nor is one with a state no crane has");

	S_CraneState end = Crane(1, CRANE_IDLE, INVALID_NETID, 5300);
	end.body.active  = 0;
	c.HandleMessage(Wrap(end));
	Check(g_rec.applied == 2 && g_rec.lastBody.active == 0, "and its end goes the same way");
}

void TestOurCranesGoOut() {
	std::printf("\nthe cranes: ours, sent\n");
	Client c;
	c.SetBridge(Bridge());
	c.HandleMessage(Wrap(Welcome(0, 0)));
	g_rec.samples          = 2;
	g_rec.sample[0].active = 1;
	g_rec.sample[1].active = 0;
	c.SendCraneStatesForTest();
	Check(c.CraneStatesSentForTest() == 2, "a busy crane and one that has just stopped");
	c.SendCraneStatesForTest();
	Check(c.CraneStatesSentForTest() == 2, "and nothing when none of ours is busy");
}

void TestACarOnOurCraneIsAskedFor() {
	std::printf("\nthe cranes: a car ours goes for\n");
	Client c;
	c.SetBridge(Bridge());
	c.HandleMessage(Wrap(Welcome(0, 0)));
	c.HandleMessage(Wrap(Spawn(40)));
	c.HandleMessage(Wrap(Spawn(41)));
	c.Tick();
	c.Tick();
	Check(c.PushAsksForTest() == 0, "standing still and nobody's crane on it, nothing is asked");
	g_rec.cranedNetId = 41;
	c.Tick();
	Check(c.PushAsksForTest() == 1, "a crane of ours going for it asks to settle it");
	c.Tick();
	Check(c.PushAsksForTest() == 1, "once, not every frame");
}

void TestACarCrushedElsewhere() {
	std::printf("\nthe cranes: a car crushed on another machine\n");
	Client c;
	c.SetBridge(Bridge());
	c.HandleMessage(Wrap(Welcome(0, 0)));
	c.HandleMessage(Wrap(Spawn(42)));
	c.HandleMessage(Wrap(Spawn(43)));
	c.Tick();

	S_VehicleRemoved gone{};
	InitHeader(gone, 2000);
	gone.playerId    = 1;
	gone.body.netId  = 43;
	gone.body.reason = VEHICLE_REMOVED_EXPORTED;
	c.HandleMessage(Wrap(gone));
	Check(g_rec.crushed == 0, "Craig taking one is no crush");

	gone.body.netId  = 42;
	gone.body.reason = VEHICLE_REMOVED_CRUSHED;
	c.HandleMessage(Wrap(gone));
	Check(g_rec.crushed == 1 && g_rec.crushedHandle == 1042,
	      "the crusher taking one names our handle for IS_CAR_CRUSHED, before the despawn");
}

// ---- against the real exe --------------------------------------------------

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

uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) { return img[va - IMAGE_BASE]; }

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<int> want) {
	uint32_t at = va;
	for (int b : want) {
		if (b >= 0 && Byte(img, at) != static_cast<uint8_t>(b))
			return false;
		++at;
	}
	return true;
}

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return Byte(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

// Every E8 rel32 in .text that lands on `to`.
size_t CallersOf(const std::vector<uint8_t> &img, uint32_t to) {
	size_t n = 0;
	for (uint32_t va = 0x00401000; va + 5 <= 0x005E4000; ++va)
		if (Byte(img, va) == 0xE8 && va + 5 + Dword(img, va + 1) == to)
			++n;
	return n;
}

void TestAgainstTheImage() {
	std::printf("\nthe cranes: against the retail exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "cranes against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());
	const uint32_t cranes = static_cast<uint32_t>(CCranes__aCranes);

	Check(CallsTo(img, 0x0048C95C, CCranes__UpdateCranes),
	      "CGame::Process runs CCranes::UpdateCranes");
	Check(CallsTo(img, CRANE_UPDATE_CALL, CCrane__Update) &&
	          Bytes(img, CRANE_UPDATE_CALL - 4, {0x8B, 0x4C, 0x24, 0x04}) &&
	          Bytes(img, CRANE_UPDATE_CALL - 6, {0xDE, 0xD9}),
	      "which calls CCrane::Update with the crane in ecx and the x87 stack emptied");
	Check(CallersOf(img, CCrane__Update) == 1, "and that is CCrane::Update's only caller");
	Check(Bytes(img, CCrane__Update, {0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00}) &&
	          Bytes(img, 0x00543AE0, {0x8A, 0x43, 0x78}),
	      "CCrane::Update, thiscall, reading m_nCraneStatus at +78h");
	Check(Bytes(img, 0x0054344E, {0x83, 0x3D}) && Dword(img, 0x00543450) == CCranes__NumCranes &&
	          Byte(img, 0x00543454) == NUM_CRANES &&
	          Bytes(img, 0x00543465, {0xC1, 0xE7, 0x07}) &&
	          Bytes(img, 0x0054346D, {0x81, 0xC7}) && Dword(img, 0x0054346F) == cranes,
	      "AddThisOneCrane: eight cranes of 0x80 at aCranes, counted in NumCranes");
	Check(Bytes(img, 0x005451A2, {0x39, 0x8B}) &&
	          Dword(img, 0x005451A4) == cranes + offs::CRANE_CAR &&
	          Bytes(img, 0x005451AA, {0x0F, 0xB6, 0x83}) &&
	          Dword(img, 0x005451AD) == cranes + offs::CRANE_STATE &&
	          Dword(img, 0x00545194) == CCranes__NumCranes,
	      "IsThisCarBeingCarriedByAnyCrane reads the car at +70h and the state at +79h");
	Check(Bytes(img, 0x00543E16, {0xC6, 0x43, 0x79, 0x03}) &&
	          Bytes(img, 0x00543E46, {0x8A, 0x46, 0x51}) &&
	          Bytes(img, 0x00543E4E, {0x80, 0x7B, 0x7B, 0x00}),
	      "the pick-up: ONLY_HEIGHT, the car's collision off, the crusher at +7Bh");
	Check(Bytes(img, 0x00543E57, {0x8A, 0x47, 0x53, 0x24, 0xFB, 0x0C, 0x04, 0x88, 0x47, 0x53}) &&
	          offs::ENTITY_FLAGS_C == 0x53 && offs::ENTITY_COLLISION_PROOF == 0x04,
	      "and the crusher's car made collision-proof, which nothing on the crane takes back");
	Check(Bytes(img, 0x0054433B, {0xD9, 0x5B, 0x68}) && Bytes(img, 0x005443AF, {0xD9, 0x5B, 0x5C}) &&
	          Bytes(img, 0x005443BE, {0xD9, 0x5B, 0x60}) && Bytes(img, 0x005443C5, {0xD9, 0x5B, 0x64}),
	      "the swing: the hook's velocity at +68h, its place at +5Ch..+64h");
	Check(Bytes(img, 0x005445F6, {0xD9, 0x43, 0x44, 0xD9, 0xFF}) &&
	          Bytes(img, 0x00544638, {0xD9, 0x58, 0x04}) &&
	          Bytes(img, 0x0054463F, {0xD9, 0x58, 0x18}) &&
	          Bytes(img, 0x00544644, {0xD9, 0x50, 0x08}) &&
	          Bytes(img, 0x0054464B, {0xD9, 0xE0, 0xD9, 0x58, 0x14}),
	      "the tail turns the building by m_fHookAngle: cos, cos, sin, -sin");
	Check(Bytes(img, 0x00544652, {0x8B, 0x0B, 0x83, 0xC1, 0x04}) &&
	          CallsTo(img, 0x00544657, CMatrix__UpdateRW) &&
	          CallsTo(img, 0x0054465E, CEntity__UpdateRwFrame) &&
	          Bytes(img, 0x00544663, {0x89, 0xD9}) &&
	          CallsTo(img, 0x00544665, CCrane__SetHookMatrix),
	      "then UpdateRW on its matrix, UpdateRwFrame and SetHookMatrix on the crane");
	Check(Bytes(img, CCrane__SetHookMatrix, {0x53, 0x56, 0x89, 0xCB}) &&
	          Bytes(img, 0x00545009, {0x8B, 0x43, 0x04, 0x85, 0xC0}),
	      "SetHookMatrix, thiscall, leaving a crane with no hook alone");
	Check(Bytes(img, CRUSHER_CRUSHED_ID_WRITE, {0xA3}) &&
	          Dword(img, CRUSHER_CRUSHED_ID_WRITE + 1) == CGarages__CrushedCarId,
	      "the crusher writes CGarages::CrushedCarId");
	Check(Bytes(img, CGarages__HasCarBeenCrushed, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x0D}) &&
	          Dword(img, CGarages__HasCarBeenCrushed + 6) == CGarages__CrushedCarId &&
	          Bytes(img, CGarages__HasCarBeenCrushed + 10, {0x39, 0xC1}),
	      "and IS_CAR_CRUSHED compares the script's handle with it");
	Check(Bytes(img, CEntity__RegisterReference, {-1}) && Bytes(img, CEntity__PruneReferences, {0x8B, 0x51, 0x60}),
	      "the references a followed car is held by");
}

} // namespace

int RunCraneTests() {
	std::printf("\n");
	TestWhenACraneIsBusy();
	TestFollowingAndItsOrder();
	TestTheStateOnTheWire();
	TestAnotherMachinesCraneReachesTheSeam();
	TestOurCranesGoOut();
	TestACarOnOurCraneIsAskedFor();
	TestACarCrushedElsewhere();
	TestAgainstTheImage();
	return g_failures;
}
