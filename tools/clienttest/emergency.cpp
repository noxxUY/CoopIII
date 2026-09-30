// Medics and fire trucks: client/src/game/emergency.h, client/src/cannonsync.h
// and the revive in client.cpp.
//
// The rules and the session side walked with no engine, and - when a copy of
// the retail exe is handed over - every address and byte game/emergencyaddr.h
// claims, read back out of it.

#include "client.h"
#include "game/emergency.h"

#include <coopiii/net.h>
#include <coopiii/protocol.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_emFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_emFailures;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
bool Near(const Vec3 &a, const Vec3 &b) { return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z); }

template <class T>
Message Wrap(const T &pkt, Channel ch) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = ch;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

// ---- the rules -------------------------------------------------------------------

void TestTheRules() {
	std::printf("\nwho decides what, around a medic and a fire truck\n");

	Check(CannonInputMayRun(true, true, true) && CannonInputMayRun(true, false, false),
	      "the local driver's own jet always goes through");
	Check(!CannonInputMayRun(false, true, true),
	      "our NPC arm's jet from a truck another machine moves is refused");
	Check(CannonInputMayRun(false, true, false) && CannonInputMayRun(false, false, false),
	      "and from our own traffic, or a truck the session has no name for, it goes");

	Check(WaterMayMovePed(false, false), "the water knocks down our own player and pedestrians");
	Check(!WaterMayMovePed(true, false) && !WaterMayMovePed(false, true),
	      "and never another player's ped or a replica of somebody else's pedestrian");

	Check(WaterMayPutOut(FireOn::Nothing) && WaterMayPutOut(FireOn::OurThing),
	      "a fire on the pavement, or on something of ours, goes out here");
	Check(!WaterMayPutOut(FireOn::SomebodyElses),
	      "one on somebody else's ped or car is left to its owner");

	Check(ReviveLastState(false) == PEDSTATE_WANDER_PATH,
	      "a pedestrian we host goes back to wandering, as the medic has him");
	Check(ReviveLastState(true) == PEDSTATE_NONE,
	      "a replica goes back to nothing, which RestorePreviousState makes idle");
	Check(DeadForMedic(PEDSTATE_DIE) && DeadForMedic(PEDSTATE_DEAD) &&
	          !DeadForMedic(PEDSTATE_IDLE) && !DeadForMedic(PEDSTATE_GETUP),
	      "only a dying or dead pedestrian is stood up");
}

// ---- the truck's own frame ---------------------------------------------------------

CarFrame Turned(float yaw, float roll, const Vec3 &pos) {
	// Yaw about z, then roll about the truck's own forward axis.
	const float cy = std::cos(yaw), sy = std::sin(yaw);
	const float cr = std::cos(roll), sr = std::sin(roll);
	CarFrame    f;
	const Vec3  right = {cy, sy, 0.0f};
	const Vec3  fwd   = {-sy, cy, 0.0f};
	const Vec3  up    = {0.0f, 0.0f, 1.0f};
	f.fwd   = fwd;
	f.right = {right.x * cr + up.x * sr, right.y * cr + up.y * sr, right.z * cr + up.z * sr};
	f.up    = {up.x * cr - right.x * sr, up.y * cr - right.y * sr, up.z * cr - right.z * sr};
	f.pos   = pos;
	return f;
}

void TestTheTrucksFrame() {
	std::printf("\na jet in the truck's own frame\n");

	const CarFrame level = Turned(0.0f, 0.0f, {100.0f, 200.0f, 10.0f});
	Check(Near(CarPointToWorld(level, {0.0f, 1.5f, 1.9f}), {100.0f, 201.5f, 11.9f}),
	      "a level truck facing north puts the driver's nozzle 1.5 m ahead and 1.9 m up");

	bool round = true;
	const float yaws[]  = {0.0f, 0.7f, 2.5f, -1.9f};
	const float rolls[] = {0.0f, 0.3f, 3.1f};
	for (float yaw : yaws)
		for (float roll : rolls) {
			const CarFrame f   = Turned(yaw, roll, {-50.0f, 12.0f, 4.0f});
			const Vec3     pos = {0.3f, 1.5f, 1.9f};
			const Vec3     dir = {0.2f, 0.95f, 0.3f};
			if (!Near(WorldPointToCar(f, CarPointToWorld(f, pos)), pos) ||
			    !Near(WorldVectorToCar(f, CarVectorToWorld(f, dir)), dir))
				round = false;
		}
	Check(round, "into the world and back, however the truck is turned or rolled");

	// The same jet from two copies of the truck a little apart comes out of
	// each copy's own nozzle.
	const CarFrame here  = Turned(0.4f, 0.0f, {10.0f, 10.0f, 5.0f});
	const CarFrame there = Turned(0.4f, 0.0f, {10.5f, 10.2f, 5.0f});
	const Vec3     local = WorldPointToCar(here, CarPointToWorld(here, {0.0f, 1.5f, 1.9f}));
	const Vec3     a     = CarPointToWorld(here, local);
	const Vec3     b     = CarPointToWorld(there, local);
	Check(Near(b.x - a.x, 0.5f) && Near(b.y - a.y, 0.2f),
	      "and a copy of the truck sprays from where that copy is");

	Check(JetIsSane({0.0f, 1.5f, 1.9f}, {0.0f, 1.0f, 0.015f}) &&
	          JetIsSane({0.0f, 0.0f, 2.2f}, {0.6f, 0.6f, 0.2f}),
	      "both of FireTruckControl's nozzles, with the jitter, are a jet");
	Check(!JetIsSane({0.0f, 50.0f, 0.0f}, {0.0f, 1.0f, 0.0f}) &&
	          !JetIsSane({0.0f, 1.5f, 1.9f}, {0.0f, 9.0f, 0.0f}) &&
	          !JetIsSane({0.0f, 1.5f, 1.9f}, {0.0f, 0.0f, 0.0f}) &&
	          !JetIsSane({NAN, 1.5f, 1.9f}, {0.0f, 1.0f, 0.0f}),
	      "a start fifty metres off, a jet nine times too fast, none, or a NaN is not");
}

// ---- CannonSync --------------------------------------------------------------------

struct CannonStub {
	std::vector<LocalCannonJet> pending;
	std::vector<uint16_t>       sprayedFrom;
	Vec3                        lastPos = {};
	bool                        haveCopy = true;
	std::vector<C_WaterCannon>  sent;
};
CannonStub g_cannon;

uint32_t StubDrainJets(LocalCannonJet *out, uint32_t max) {
	uint32_t n = 0;
	for (const LocalCannonJet &j : g_cannon.pending)
		if (n < max)
			out[n++] = j;
	g_cannon.pending.clear();
	return n;
}

bool StubSpray(uint16_t netId, const Vec3 &pos, const Vec3 &) {
	if (!g_cannon.haveCopy)
		return false;
	g_cannon.sprayedFrom.push_back(netId);
	g_cannon.lastPos = pos;
	return true;
}

void StubSend(void *, const void *bytes, size_t len, Channel ch) {
	if (len != sizeof(C_WaterCannon) || ch != CH_SNAPSHOT)
		return;
	C_WaterCannon pkt;
	std::memcpy(&pkt, bytes, sizeof pkt);
	g_cannon.sent.push_back(pkt);
}

S_WaterCannon Jet(uint8_t from, uint16_t netId, float z = 1.9f) {
	S_WaterCannon s;
	InitHeader(s, 1000);
	s.playerId   = from;
	s.body.netId = netId;
	s.body.pos   = {0.0f, 1.5f, z};
	s.body.dir   = {0.0f, 1.0f, 0.01f};
	return s;
}

void TestSomebodyElsesJetIsSprayedHere() {
	std::printf("\nsomebody else's fire truck sprays from our copy of it\n");
	g_cannon = CannonStub{};
	CannonBridge bridge;
	bridge.DrainLocalJets = &StubDrainJets;
	bridge.SprayCopy      = &StubSpray;
	CannonSync sync;
	sync.Bind(&bridge, &StubSend, nullptr);

	sync.OnJet(Jet(1, 700), /*local=*/0, 1000);
	Check(sync.LiveJets() == 1 && g_cannon.sprayedFrom.empty(),
	      "heard, and not sprayed on the packet");
	sync.Tick(1000);
	sync.Tick(1016);
	sync.Tick(1033);
	Check(g_cannon.sprayedFrom.size() == 3 && g_cannon.sprayedFrom[2] == 700,
	      "sprayed from truck 700 on every frame after it");

	sync.OnJet(Jet(1, 700, 2.2f), 0, 1040);
	sync.Tick(1050);
	Check(sync.LiveJets() == 1 && Near(g_cannon.lastPos.z, 2.2f),
	      "the newest jet for a truck replaces the last");

	sync.Tick(1040 + WATER_CANNON_HOLD_MS);
	const size_t before = g_cannon.sprayedFrom.size();
	sync.Tick(1041 + WATER_CANNON_HOLD_MS);
	Check(g_cannon.sprayedFrom.size() == before && sync.LiveJets() == 0,
	      "and it stops once nothing has been heard for WATER_CANNON_HOLD_MS");

	sync.OnJet(Jet(0, 701), 0, 2000);
	S_WaterCannon junk = Jet(1, 702);
	junk.body.dir      = {0.0f, 40.0f, 0.0f};
	sync.OnJet(junk, 0, 2000);
	S_WaterCannon nobody = Jet(1, INVALID_NETID);
	sync.OnJet(nobody, 0, 2000);
	Check(sync.LiveJets() == 0,
	      "our own jet coming back, a jet no truck could spray and one with no truck are dropped");

	for (uint16_t n = 0; n < MAX_REMOTE_JETS + 2; ++n)
		sync.OnJet(Jet(1, uint16_t(800 + n)), 0, 3000 + n);
	Check(sync.LiveJets() == MAX_REMOTE_JETS, "a full table keeps the newest");
	g_cannon.sprayedFrom.clear();
	sync.Tick(3010);
	bool oldestGone = true;
	for (uint16_t id : g_cannon.sprayedFrom)
		if (id == 800 || id == 801)
			oldestGone = false;
	Check(oldestGone, "and the two heard of longest ago are the ones that went");

	g_cannon.haveCopy = false;
	sync.Tick(3011);
	Check(sync.NoCopy() > 0, "a truck we have no copy of is counted, not sprayed");

	sync.Clear();
	Check(sync.LiveJets() == 0, "and the session ending forgets every jet");
}

void TestOurJetGoesOut() {
	std::printf("\nour own fire truck's jet goes out at the snapshot rate\n");
	g_cannon = CannonStub{};
	CannonBridge bridge;
	bridge.DrainLocalJets = &StubDrainJets;
	bridge.SprayCopy      = &StubSpray;
	CannonSync sync;
	sync.Bind(&bridge, &StubSend, nullptr);

	sync.Send(1000);
	Check(g_cannon.sent.empty(), "nothing sprayed, nothing sent");

	g_cannon.pending.push_back({900, {0.0f, 1.5f, 1.9f}, {0.0f, 1.0f, 0.0f}});
	sync.Send(1000);
	Check(g_cannon.sent.size() == 1 && g_cannon.sent[0].body.netId == 900 &&
	          g_cannon.sent[0].hdr.opcode == OP_C_WATER_CANNON,
	      "the first frame's jet goes at once");

	// Sixty frames a second against twenty-five packets.
	uint32_t now = 1000;
	for (int frame = 0; frame < 60; ++frame) {
		now += 16;
		g_cannon.pending.push_back({900, {0.0f, 1.5f, 1.9f}, {0.0f, 1.0f, 0.0f}});
		sync.Send(now);
	}
	Check(g_cannon.sent.size() >= 24 && g_cannon.sent.size() <= 27,
	      "and a second of spraying is about twenty-five packets, not sixty");

	// One more frame between two ticks: held, and sent on the next tick even
	// though the truck has stopped by then.
	const size_t before = g_cannon.sent.size();
	g_cannon.pending.push_back({900, {0.0f, 1.5f, 1.9f}, {0.0f, 1.0f, 0.0f}});
	sync.Send(now + 10);
	Check(g_cannon.sent.size() == before, "a frame between two ticks is held");
	sync.Send(now + 50);
	Check(g_cannon.sent.size() == before + 1, "and goes on the next tick");
	sync.Send(now + 400);
	Check(g_cannon.sent.size() == before + 1, "and then the stream stops with the spraying");

	g_cannon.pending.push_back({901, {0.0f, 1.5f, 1.9f}, {0.0f, 99.0f, 0.0f}});
	g_cannon.pending.push_back({INVALID_NETID, {0.0f, 1.5f, 1.9f}, {0.0f, 1.0f, 0.0f}});
	sync.Send(now + 1000);
	Check(g_cannon.sent.size() == before + 1, "a jet no truck sprays, or with no truck, never goes");
}

// ---- the revive, through Client -----------------------------------------------------

struct ReviveStub {
	std::vector<uint16_t> offered;
	std::vector<uint16_t> revivedReplicas;
	std::vector<uint16_t> revivedHosted;
	std::vector<uint16_t> medicRevives;
	int                   kills = 0;
	bool                  replicaThere = true;
	bool                  hostedThere  = true;
};
ReviveStub g_rev;

bool RevModelReady(uint16_t) { return true; }
void RevRequestModel(uint16_t) {}
bool RevSpawn(RemoteAmbientPed &ped) {
	ped.poolHandle = 4000 + ped.netId;
	return true;
}
bool RevKill(RemoteAmbientPed &, uint16_t) {
	++g_rev.kills;
	return true;
}
void RevOffer(RemoteAmbientPed &ped) { g_rev.offered.push_back(ped.netId); }
bool RevReplica(RemoteAmbientPed &ped) {
	if (!g_rev.replicaThere)
		return false;
	g_rev.revivedReplicas.push_back(ped.netId);
	return true;
}
bool RevHosted(uint16_t netId) {
	g_rev.revivedHosted.push_back(netId);
	return g_rev.hostedThere;
}
uint32_t RevDrain(uint16_t *out, uint32_t max) {
	uint32_t n = 0;
	for (uint16_t id : g_rev.medicRevives)
		if (n < max)
			out[n++] = id;
	g_rev.medicRevives.clear();
	return n;
}

WorldBridge ReviveBridge() {
	WorldBridge b;
	b.IsModelReady         = &RevModelReady;
	b.RequestModel         = &RevRequestModel;
	b.SpawnAmbientReplica  = &RevSpawn;
	b.KillAmbientReplica   = &RevKill;
	b.OfferCorpseToMedics  = &RevOffer;
	b.ReviveAmbientReplica = &RevReplica;
	b.ReviveHostedPed      = &RevHosted;
	b.DrainMedicRevives    = &RevDrain;
	return b;
}

S_Welcome Welcome(uint8_t playerId) {
	S_Welcome w;
	InitHeader(w, 1000);
	w.playerId   = playerId;
	w.netId      = uint16_t(100 + playerId);
	w.maxPlayers = MAX_PLAYERS;
	w.snapshotHz = SNAPSHOT_HZ;
	w.hour       = 12;
	return w;
}

S_PedSpawn PedFrom(uint8_t owner, uint16_t netId, uint8_t flags = 0) {
	S_PedSpawn s;
	InitHeader(s, 1000);
	s.ownerPlayerId = owner;
	s.tempId        = 0;
	s.netId         = netId;
	s.body.modelId  = 7;
	s.body.flags    = flags;
	s.body.pos      = {10.0f, 20.0f, 30.0f};
	return s;
}

S_PedDeath DeathOf(uint16_t netId) {
	S_PedDeath s;
	InitHeader(s, 1000);
	s.body.netId  = netId;
	s.body.animId = 13;
	return s;
}

S_PedRevive ReviveOf(uint16_t netId, uint8_t medicOf = 2) {
	S_PedRevive s;
	InitHeader(s, 1000);
	s.playerId   = medicOf;
	s.body.netId = netId;
	return s;
}

void TestSomebodysMedicStandsOurCopyUp() {
	std::printf("\nsomebody's medic stands a pedestrian up on our screen too\n");
	g_rev = ReviveStub{};
	Client c;
	c.SetBridge(ReviveBridge());
	c.HandleMessage(Wrap(Welcome(0), CH_EVENT));
	c.HandleMessage(Wrap(PedFrom(1, 600), CH_EVENT));
	c.HandleMessage(Wrap(PedFrom(1, 601, AMBIENT_MISSION), CH_EVENT));
	c.Tick();
	Check(c.AmbientPed(600) && c.AmbientPed(600)->poolHandle >= 0, "(the replica is built)");

	c.HandleMessage(Wrap(DeathOf(600), CH_EVENT));
	c.HandleMessage(Wrap(DeathOf(601), CH_EVENT));
	c.Tick();
	Check(g_rev.kills == 2, "(both die on our screen as their host said)");
	Check(g_rev.offered.size() == 1 && g_rev.offered[0] == 600,
	      "the corpse becomes one our medics answer; the mission's never does");

	c.HandleMessage(Wrap(ReviveOf(600), CH_EVENT));
	const RemoteAmbientPed *p = c.AmbientPed(600);
	Check(p && !p->dead && !p->deathApplied, "his row is no longer a corpse");
	Check(g_rev.revivedReplicas.size() == 1 && g_rev.revivedReplicas[0] == 600,
	      "and our copy of him is stood up");
	c.Tick();
	c.Tick();
	Check(g_rev.kills == 2, "and nothing puts him back down on the frames after");

	// Killed again, later: the second death is as good as the first.
	c.HandleMessage(Wrap(DeathOf(600), CH_EVENT));
	c.Tick();
	Check(g_rev.kills == 3 && g_rev.offered.size() == 2, "his next death lands like the first");
}

void TestARevivedCopyThatIsNotThereIsBuiltStanding() {
	std::printf("a revive for a replica we have lost leaves him to be built standing\n");
	g_rev = ReviveStub{};
	g_rev.replicaThere = false;
	Client c;
	c.SetBridge(ReviveBridge());
	c.HandleMessage(Wrap(Welcome(0), CH_EVENT));
	c.HandleMessage(Wrap(PedFrom(1, 610), CH_EVENT));
	c.HandleMessage(Wrap(DeathOf(610), CH_EVENT));
	c.HandleMessage(Wrap(ReviveOf(610), CH_EVENT));
	c.Tick();
	Check(g_rev.kills == 0 && c.AmbientPed(610) && !c.AmbientPed(610)->dead,
	      "a death and a revive that both beat the spawn leave a live pedestrian");
}

void TestOurOwnPedestrianIsStoodUp() {
	std::printf("somebody's medic stands up a pedestrian we host\n");
	g_rev = ReviveStub{};
	Client c;
	c.SetBridge(ReviveBridge());
	c.HandleMessage(Wrap(Welcome(0), CH_EVENT));
	c.HandleMessage(Wrap(ReviveOf(620, 1), CH_EVENT));
	Check(g_rev.revivedHosted.size() == 1 && g_rev.revivedHosted[0] == 620,
	      "no replica row, so our own engine is asked to stand its pedestrian up");
	Check(g_rev.revivedReplicas.empty(), "and no replica is touched");
}

void TestOurMedicsReviveGoesOut() {
	std::printf("our own medic's revive goes to the session\n");
	g_rev = ReviveStub{};
	Client c;
	c.SetBridge(ReviveBridge());
	c.HandleMessage(Wrap(Welcome(0), CH_EVENT));
	c.HandleMessage(Wrap(PedFrom(1, 630), CH_EVENT));
	c.Tick();
	c.HandleMessage(Wrap(DeathOf(630), CH_EVENT));
	c.Tick();
	Check(g_rev.kills == 1, "(the replica is a corpse here)");

	// Our medic did the CPR on our copy of him; the host's is still down.
	g_rev.medicRevives = {630, 631};
	c.TickMedicRevivesForTest();
	Check(c.MedicRevivesSentForTest() == 2, "both go to the session, ours and a replica alike");
	const RemoteAmbientPed *p = c.AmbientPed(630);
	Check(p && !p->dead, "and the replica's row stops being a corpse at once");
	c.Tick();
	Check(g_rev.kills == 1, "so the death pass does not put him back down");
	c.TickMedicRevivesForTest();
	Check(c.MedicRevivesSentForTest() == 2, "and nothing is sent twice");
}

// ---- against the real exe ------------------------------------------------------------

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

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return Byte(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

bool BytesAt(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> bytes) {
	uint32_t i = 0;
	for (uint8_t b : bytes)
		if (Byte(img, va + i++) != b)
			return false;
	return true;
}

constexpr uint32_t kTextBegin = 0x00401000;
constexpr uint32_t kTextEnd   = 0x005E4000;

// Every E8 in .text whose target is `to`, in order.
std::vector<uint32_t> CallersOf(const std::vector<uint8_t> &img, uint32_t to) {
	std::vector<uint32_t> out;
	for (uint32_t va = kTextBegin; va + 5 <= kTextEnd; ++va)
		if (CallsTo(img, va, to))
			out.push_back(va);
	return out;
}

size_t CallsIn(const std::vector<uint8_t> &img, uint32_t from, uint32_t to, uint32_t target) {
	size_t n = 0;
	for (uint32_t va = from; va + 5 <= to; ++va)
		if (CallsTo(img, va, target))
			++n;
	return n;
}

bool DwordIn(const std::vector<uint8_t> &img, uint32_t from, uint32_t to, uint32_t value) {
	for (uint32_t va = from; va + 4 <= to; ++va)
		if (Dword(img, va) == value)
			return true;
	return false;
}

void TestTheMedicAgainstTheImage(const std::vector<uint8_t> &img) {
	std::printf("\nthe medic, against gta3.exe\n");

	Check(CallsTo(img, 0x004C3064, CEmergencyPed__MedicAI) &&
	          CallsTo(img, 0x004C3078, CEmergencyPed__FiremanAI) &&
	          BytesAt(img, 0x004C303F, {0x83, 0xE8, 0x10, 0x83, 0xF8, 0x01}) &&
	          CallersOf(img, CEmergencyPed__MedicAI).size() == 1,
	      "ProcessControl's ped type switch reaches MedicAI for 16 and FiremanAI for 17, "
	      "and nothing else calls MedicAI");

	const std::vector<uint32_t> finders = CallersOf(img, CAccidentManager__FindNearestAccident);
	bool three = finders.size() == 5;
	for (uint32_t site : MEDIC_FIND_ACCIDENT_CALLS) {
		const bool listed = std::find(finders.begin(), finders.end(), site) != finders.end();
		three = three && listed && site > CEmergencyPed__MedicAI &&
		        site < CEmergencyPed__FiremanAI &&
		        BytesAt(img, site - 0x0D, {0xB9, 0x10, 0xFD, 0x87, 0x00});
	}
	Check(three, "FindNearestAccident has five callers, three of them MedicAI's, each on "
	             "gAccidentManager (mov ecx,87FD10h)");
	Check(BytesAt(img, 0x004567A9, {0x80, 0xBF, 0x60, 0x01, 0x00, 0x00, 0x02}) &&
	          BytesAt(img, 0x00456859, {0xC2, 0x10, 0x00}),
	      "it skips a MISSION_CHAR victim and returns with ret 10h");

	Check(CallersOf(img, CAccidentManager__ReportAccident).size() == 1 &&
	          CallsTo(img, 0x00456744, CAccidentManager__ReportAccident) &&
	          CallsTo(img, 0x0048C984, 0x00456710),
	      "ReportAccident is reached from CAccidentManager::Update alone, once a frame");
	Check(BytesAt(img, 0x004565E7, {0x80, 0xBB, 0x60, 0x01, 0x00, 0x00, 0x02}) &&
	          BytesAt(img, 0x00456607, {0x8A, 0x83, 0x5A, 0x01, 0x00, 0x00, 0xD0, 0xE8, 0x24, 0x01,
	                                    0x74}) &&
	          offs::PED_FLAGS_G == 0x15A && offs::PED_ALLOW_MEDICS == 0x02 &&
	          offs::PED_CHAR_CREATED_BY == 0x160,
	      "and refuses a MISSION_CHAR and one without bAllowMedicsToReviveMe - the two "
	      "bytes a replica has and the medic's corpse must not");
	Check(BytesAt(img, 0x004566F7, {0x89, 0xB3, 0x28, 0x03, 0x00, 0x00}) &&
	          BytesAt(img, 0x00456626, {0xC2, 0x04, 0x00}),
	      "it keeps m_lastAccident on the victim and returns with ret 4");
	Check(BytesAt(img, 0x004565B8, {0x83, 0xC2, 0x0C, 0x83, 0xF8, 0x14}) &&
	          NUM_ACCIDENTS == 20 && SIZEOF_CACCIDENT == 12,
	      "twenty accidents of twelve bytes");

	// The revive, in the medic's order.
	Check(BytesAt(img, 0x004C3AF2, {0x8A, 0x87, 0x56, 0x01, 0x00, 0x00, 0xC0, 0xE8, 0x05}) &&
	          offs::PED_BODY_PART_OFF == 0x20,
	      "a patient who has lost a limb is not revived");
	Check(BytesAt(img, 0x004C3B03, {0xC7, 0x87, 0xC0, 0x02, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x42}) &&
	          MEDIC_REVIVE_HEALTH == 100.0f && offs::PED_HEALTH == 0x2C0,
	      "health 100");
	Check(BytesAt(img, 0x004C3B13, {0xC7, 0x80, 0x24, 0x02, 0x00, 0x00, 0, 0, 0, 0}) &&
	          BytesAt(img, 0x004C3B23, {0xC7, 0x80, 0x28, 0x02, 0x00, 0x00, 5, 0, 0, 0}) &&
	          MEDIC_REVIVE_LAST_STATE == 5,
	      "PED_NONE now, WANDER_PATH to go back to");
	Check(CallsTo(img, MEDIC_REVIVE_GETUP_CALL, CPed__SetGetUp) &&
	          BytesAt(img, MEDIC_REVIVE_GETUP_CALL - 6, {0x8B, 0x8B, 0x3C, 0x05, 0x00, 0x00}) &&
	          offs::EMERGENCY_REVIVED_PED == 0x53C,
	      "SetGetUp on m_pRevivedPed");
	size_t inMedic = 0;
	for (uint32_t site : CallersOf(img, CPed__SetGetUp))
		if (site > CEmergencyPed__MedicAI && site < CEmergencyPed__FiremanAI)
			++inMedic;
	Check(inMedic == 1, "the only SetGetUp in MedicAI");
	Check(BytesAt(img, 0x004C3B3E, {0x8A, 0x45, 0x51, 0x24, 0xFE, 0x0C, 0x01}) &&
	          BytesAt(img, 0x004C3B4E, {0x6A, 0x02}) && CallsTo(img, 0x004C3B50, CPed__SetMoveState) &&
	          CallsTo(img, 0x004C3B5B, CPed__RestartNonPartialAnims),
	      "then collision, SetMoveState(PEDMOVE_WALK) and RestartNonPartialAnims");
	Check(BytesAt(img, 0x004C3B66, {0x8A, 0x82, 0x57, 0x01, 0x00, 0x00, 0x24, 0xEF}) &&
	          BytesAt(img, 0x004C3B7A, {0x8A, 0x81, 0x5B, 0x01, 0x00, 0x00, 0x24, 0xFE}) &&
	          BytesAt(img, 0x004C3B8E, {0xC7, 0x80, 0x4C, 0x03, 0x00, 0x00, 0, 0, 0, 0}) &&
	          offs::PED_FLAGS_D == 0x157 && offs::PED_DIE_ANIM_PLAYING == 0x10 &&
	          offs::PED_FLAGS_H == 0x15B && offs::PED_KNOCKED_UP_INTO_AIR == 0x01 &&
	          offs::PED_COLLIDING_ENTITY == 0x34C,
	      "and the die animation, the knock into the air and the colliding entity cleared");

	// Firemen on foot.
	Check(CallsIn(img, CEmergencyPed__FiremanAI, 0x004C3EB1, CFire__Extinguish) == 0 &&
	          CallsIn(img, CEmergencyPed__FiremanAI, 0x004C3EB1, 0x00479340) == 3 &&
	          BytesAt(img, 0x004C3E66, {0xFF, 0x48, 0x28}) && BytesAt(img, 0x004C3EB0, {0xC3}),
	      "a fireman on foot never puts a fire out: three FindNearestFire, no Extinguish");
}

void TestTheCannonAgainstTheImage(const std::vector<uint8_t> &img) {
	std::printf("\nthe water cannon, against gta3.exe\n");

	Check(CallersOf(img, CAutomobile__FireTruckControl).size() == 1 &&
	          CallsTo(img, 0x00531FF7, CAutomobile__FireTruckControl) &&
	          BytesAt(img, 0x00531FEE, {0x83, 0xF8, 0x61}) && FIRETRUCK_MODEL == 0x61,
	      "ProcessControl runs FireTruckControl for model 97 and nothing else does");
	Check(CallsTo(img, 0x0052259A, FindPlayerVehicle) &&
	          BytesAt(img, 0x005227E7, {0x8A, 0x4B, 0x50, 0xC0, 0xE9, 0x03}) &&
	          BytesAt(img, 0x005227F0, {0x83, 0xF8, 0x03}),
	      "its arms: the local player's truck, and any other in STATUS_PHYSICS");
	const std::vector<uint32_t> ones = CallersOf(img, CWaterCannons__UpdateOne);
	Check(ones.size() == 1 && ones[0] == FIRE_TRUCK_CANNON_CALL &&
	          BytesAt(img, FIRE_TRUCK_CANNON_CALL - 2, {0x50, 0x53}) &&
	          BytesAt(img, FIRE_TRUCK_CANNON_CALL + 5, {0x83, 0xC4, 0x0C}) &&
	          BytesAt(img, 0x005227E2, {0xE9}) && Dword(img, 0x005227E3) + 0x005227E7 == 0x00522B25,
	      "UpdateOne's one caller, the cdecl call both arms end on");
	Check(BytesAt(img, 0x00522B40, {0x6A, 0x03, 0x68, 0x9C, 0x01, 0x00, 0x00}) &&
	          Dword(img, 0x00522B4F) == CWaterCannons__aCannons &&
	          BytesAt(img, 0x00522489, {0x66, 0x83, 0xF8, 0x03}) && NUM_WATER_CANNONS == 3 &&
	          SIZEOF_CWATERCANNON == 0x19C && MAX_LOCAL_JETS == NUM_WATER_CANNONS,
	      "three cannons of 0x19C bytes, and UpdateOne looks at all three");
	Check(BytesAt(img, 0x00521B86, {0x05, 0x96, 0x00, 0x00, 0x00}) && WATER_CANNON_HOLD_MS == 150,
	      "a point lives 150 ms, the hold an observer keeps a jet for");

	Check(CallsTo(img, 0x0052252E, CWaterCannon__Update_OncePerFrame) &&
	          CallersOf(img, CWaterCannon__Update_OncePerFrame).size() == 1,
	      "CWaterCannons::Update runs each cannon's frame, and nothing else does");
	Check(CallsTo(img, WATER_CANNON_EXTINGUISH_CALL, CFireManager__ExtinguishPoint) &&
	          BytesAt(img, WATER_CANNON_EXTINGUISH_CALL - 0x17, {0xB9, 0xD0, 0x31, 0x8F, 0x00}) &&
	          BytesAt(img, 0x00479DE2, {0x80, 0x7C, 0x33, 0x04, 0x00}) &&
	          BytesAt(img, 0x00479E3F, {0xC2, 0x10, 0x00}) &&
	          CallsTo(img, 0x00479E27, CFire__Extinguish) && FIRE_ONGOING == 0,
	      "the water puts out fires through gFireManager.ExtinguishPoint, which skips a fire "
	      "that is not ongoing");
	Check(CallsTo(img, WATER_CANNON_PUSH_PEDS_CALL, CWaterCannon__PushPeds) &&
	          CallersOf(img, CWaterCannon__PushPeds).size() == 1,
	      "and pushes peds from there alone");

	const uint32_t pushEnd = 0x005223F7;
	Check(CallsTo(img, PUSH_PEDS_SIDE_CALL, CPed__GetLocalDirection) &&
	          CallsTo(img, PUSH_PEDS_FORCE_CALL, CPhysical__ApplyMoveForce) &&
	          CallsTo(img, PUSH_PEDS_FALL_CALL, CPed__SetFall) &&
	          CallsTo(img, PUSH_PEDS_EXTINGUISH_CALL, CFire__Extinguish) &&
	          CallsIn(img, CWaterCannon__PushPeds, pushEnd, CPed__GetLocalDirection) == 1 &&
	          CallsIn(img, CWaterCannon__PushPeds, pushEnd, CPhysical__ApplyMoveForce) == 1 &&
	          CallsIn(img, CWaterCannon__PushPeds, pushEnd, CPed__SetFall) == 1 &&
	          CallsIn(img, CWaterCannon__PushPeds, pushEnd, CFire__Extinguish) == 1,
	      "PushPeds makes each of its four calls once");
	Check(BytesAt(img, PUSH_PEDS_SIDE_CALL + 5, {0x89, 0x44, 0x24, 0x04, 0x8A, 0x85, 0x54, 0x01, 0x00,
	                                             0x00, 0x24, 0xFE}) &&
	          BytesAt(img, 0x00522397, {0xD9, 0x5D, 0x78}) &&
	          BytesAt(img, 0x005223B0, {0xD9, 0x5D, 0x7C}) &&
	          offs::PED_FLAGS_A == 0x154 && offs::PED_IS_STANDING == 0x01 &&
	          offs::MOVE_SPEED == 0x78,
	      "and in between writes only bIsStanding and the move speed, which a refusal puts back");
	Check(BytesAt(img, PUSH_PEDS_EXTINGUISH_CALL - 10, {0x8B, 0x8D, 0xB4, 0x04, 0x00, 0x00}) &&
	          PED_FIRE == 0x4B4,
	      "the fire it puts out is the ped's own m_pFire");
	Check(BytesAt(img, 0x004D0BA7, {0xC2, 0x0C, 0x00}) &&
	          BytesAt(img, 0x00495A07, {0xC2, 0x0C, 0x00}) &&
	          BytesAt(img, 0x004CCEAD, {0xC2, 0x04, 0x00}) &&
	          BytesAt(img, PUSH_PEDS_FALL_CALL - 5, {0x68, 0xD0, 0x07, 0x00, 0x00}),
	      "SetFall and ApplyMoveForce take three arguments, GetLocalDirection one");
	Check(!DwordIn(img, CWaterCannon__PushPeds, pushEnd, CPools__ms_pVehiclePool) &&
	          !DwordIn(img, CWaterCannon__Update_OncePerFrame, 0x00521CB0, CPools__ms_pVehiclePool) &&
	          DwordIn(img, CWaterCannon__PushPeds, pushEnd, CPools__ms_pPedPool),
	      "the water reads the ped pool and never the vehicle pool: it moves no car");
}

} // namespace

int RunEmergencyTests() {
	g_emFailures = 0;
	TestTheRules();
	TestTheTrucksFrame();
	TestSomebodyElsesJetIsSprayedHere();
	TestOurJetGoesOut();
	TestSomebodysMedicStandsOurCopyUp();
	TestARevivedCopyThatIsNotThereIsBuiltStanding();
	TestOurOwnPedestrianIsStoodUp();
	TestOurMedicsReviveGoesOut();

	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("\n  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "medic and the water cannon against one\n");
	} else {
		std::printf("\n  reading %s\n", from.c_str());
		TestTheMedicAgainstTheImage(img);
		TestTheCannonAgainstTheImage(img);
	}
	return g_emFailures;
}
