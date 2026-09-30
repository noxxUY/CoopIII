// Police for a wanted player on another machine, what the crowd says, and a
// traffic replica's controls (protocol.h, C_CopHandover, C_PedSpeech,
// AmbientCarState::steer). The rules walked with no engine, the roster driven
// through a bridge of its own, and - when a copy of the retail exe is handed
// over - every byte game/crowdaddr.h claims for them, read back out of it.

#include "client.h"
#include "game/adopt.h"
#include "game/crowdaddr.h"
#include "game/crowdrange.h"
#include "game/pedspeech.h"
#include "game/wanted.h"

#include <coopiii/protocol.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_copFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_copFailures;
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

void TestWhoGetsTheCop() {
	std::printf("\na cop of ours beside somebody else's stars\n");
	WantedViewer v[3];
	v[0] = WantedViewer{1, {10.0f, 0.0f, 0.0f}, 0};
	v[1] = WantedViewer{2, {20.0f, 0.0f, 0.0f}, 3};
	v[2] = WantedViewer{3, {-15.0f, 0.0f, 0.0f}, 1};
	float d = 0.0f;
	Check(NearestWantedViewer(0.0f, 0.0f, v, 3, d) == 3 && std::fabs(d - 15.0f) < 0.01f,
	      "the nearest one with any stars; the nearer one without is nobody a cop wants");
	v[2].level = 0;
	Check(NearestWantedViewer(0.0f, 0.0f, v, 3, d) == 2, "then the next");
	v[1].level = 0;
	Check(NearestWantedViewer(0.0f, 0.0f, v, 3, d) == INVALID_PLAYER, "and with none, nobody");

	const uint32_t HOLD = COP_HANDOVER_HOLD_MS;
	Check(ShouldHandCopOver(false, 40.0f, 2, 12.0f, HOLD),
	      "we are not wanted: a wanted player 12 m off gets him");
	Check(!ShouldHandCopOver(false, 40.0f, 2, 31.0f, HOLD), "not from further than 30 m");
	Check(!ShouldHandCopOver(false, 40.0f, INVALID_PLAYER, 5.0f, HOLD), "not to nobody");
	Check(!ShouldHandCopOver(true, 30.0f, 2, 10.0f, HOLD),
	      "we are wanted too, and only 20 m further: he stays ours");
	Check(ShouldHandCopOver(true, 36.0f, 2, 10.0f, HOLD),
	      "more than the margin further: he goes");
	Check(ShouldHandCopOver(true, -1.0f, 2, 10.0f, HOLD), "we have no player: he goes");
	Check(!ShouldHandCopOver(false, 40.0f, 2, 12.0f, HOLD - 1),
	      "one handed to us is not handed on inside the hold");
	Check(!ShouldHandCopOver(false, 40.0f, 2, std::nanf(""), HOLD), "nor to a NaN");

	// Two wanted players, 20 m apart, the cop between them: whoever has him
	// keeps him. Bouncing needs a swing of twice the margin.
	bool bounced = false;
	for (float x = -10.0f; x <= 30.0f; x += 1.0f) {
		const float toA = std::fabs(x), toB = std::fabs(20.0f - x);
		// A has him: does A give him to B? And the same from B's side.
		const bool aGives = ShouldHandCopOver(true, toA, 2, toB, HOLD);
		const bool bGives = ShouldHandCopOver(true, toB, 1, toA, HOLD);
		bounced = bounced || (aGives && bGives);
	}
	Check(!bounced, "no spot where both would hand him to the other");
}

void TestCopModelsAreTheEngines() {
	std::printf("\nwhich models are police\n");
	Check(CopTypeForPedModel(1) == 0 && CopTypeForPedModel(3) == 1 &&
	          CopTypeForPedModel(2) == 2 && CopTypeForPedModel(4) == 3,
	      "cop, FBI, SWAT, army: CCopPed's own table");
	bool round = true;
	for (int t = 0; t < 4; ++t)
		round = round && CopTypeForPedModel(PedModelForCopType(t)) == t;
	Check(round, "and each type builds the model that says it");
	Check(CopTypeForPedModel(0) == COP_TYPE_NONE && CopTypeForPedModel(7) == COP_TYPE_NONE,
	      "the player and a civilian are not police");
	Check(CopTypeForCarModel(116) == 0 && CopTypeForCarModel(107) == 1 &&
	          CopTypeForCarModel(117) == 2 && CopTypeForCarModel(122) == 3 &&
	          CopTypeForCarModel(123) == 3,
	      "police car, FBI car, Enforcer, Rhino, Barracks: AddPedInCar's own table");
	Check(CopTypeForCarModel(106) == COP_TYPE_NONE && CopTypeForCarModel(97) == COP_TYPE_NONE &&
	          CopTypeForCarModel(90) == COP_TYPE_NONE,
	      "an ambulance, a fire truck and a taxi are not");
}

void TestACopIsRebuiltOnlyForAPursuit() {
	std::printf("\na police replica handed to us\n");
	Check(PlanPedAdoption(true, true, false, AMBIENT_PEDTYPE_COP, true) ==
	          AdoptPlan::RebuildAsCop,
	      "for our stars: a cop is built in his place");
	Check(PlanPedAdoption(true, true, false, AMBIENT_PEDTYPE_COP, false) == AdoptPlan::Release,
	      "for a leaver: let go, as before");
	Check(PlanPedAdoption(true, true, true, AMBIENT_PEDTYPE_COP, true) == AdoptPlan::Release &&
	          PlanPedAdoption(true, false, false, AMBIENT_PEDTYPE_COP, true) ==
	              AdoptPlan::Release,
	      "not a dead one, nor one never built here");
	Check(PlanPedAdoption(true, true, false, AMBIENT_PEDTYPE_CIVMALE, true) ==
	          AdoptPlan::Convert,
	      "a civilian in a pursuit batch is simply taken over");
	Check(AMBIENT_PEDTYPE_COP == PEDTYPE_COP, "the wire's cop type is the engine's");
}

void TestTheCopGate() {
	std::printf("\nthe cops-on-foot gate counts other machines' cops\n");
	Check(CopsTheGateSees(2, 0) == 2, "none of theirs: our own count");
	Check(CopsTheGateSees(2, 4) == 6, "four of theirs built here: six against the limit");
	Check(CopsTheGateSees(-3, 1) == 1, "never below what theirs add");
	Check(CopsTheGateSees(0x7FFFFFF0, 0xFFFFFFFFu) == 0x7FFFFFFF, "and never wraps");
}

void TestWhatIsSaid() {
	std::printf("\nwhat the crowd says\n");
	Check(PedSpeechSoundValid(0x61) && PedSpeechSoundValid(0x87), "death to taxi-call");
	Check(!PedSpeechSoundValid(0x60) && !PedSpeechSoundValid(0x88) &&
	          !PedSpeechSoundValid(SOUND_NO_SOUND),
	      "and nothing CommentWaitTime has no row for");
	Check(PedSpeechWorthSending(0x62, 30.0f * 30.0f), "a line 30 m from somebody goes");
	Check(!PedSpeechWorthSending(0x62, 61.0f * 61.0f), "61 m from everybody, nobody hears it");
	Check(!PedSpeechWorthSending(0x62, -1.0f), "nobody else anywhere");
	Check(!PedSpeechWorthSending(0x30, 1.0f), "a sound that is not speech never goes");

	SpeechBudget b;
	uint32_t     took = 0;
	for (int i = 0; i < 20; ++i)
		took += b.Take(5000) ? 1 : 0;
	Check(took == PED_SPEECH_MAX_PER_S, "no more than the budget inside a second");
	Check(!b.Take(5999) && b.Take(6000), "and a fresh budget a second later");

	Check(SpokeAcross(100, 250) && !SpokeAcross(250, 250),
	      "a line was played exactly when its start time moved");
}

void TestTheControlsOnTheWire() {
	std::printf("\na traffic car's wheel and pedals on the wire\n");
	Check(std::fabs(DecodeCarSteer(EncodeCarSteer(0.52f)) - 0.52f) < 0.006f &&
	          std::fabs(DecodeCarSteer(EncodeCarSteer(-0.31f)) + 0.31f) < 0.006f,
	      "a steer angle, either way, to half a hundredth of a radian");
	Check(EncodeCarSteer(5.0f) == 127 && EncodeCarSteer(-5.0f) == -127 &&
	          EncodeCarSteer(std::nanf("")) == 0,
	      "clamped, and NaN is straight ahead");
	Check(DecodeCarGas(EncodeCarGas(1.0f)) == 1.0f && DecodeCarGas(EncodeCarGas(-1.0f)) == -1.0f &&
	          std::fabs(DecodeCarGas(EncodeCarGas(0.4f)) - 0.4f) < 0.005f,
	      "gas, reverse included");
	Check(DecodeCarBrake(EncodeCarBrake(1.0f)) == 1.0f && EncodeCarBrake(-2.0f) == 0 &&
	          EncodeCarBrake(std::nanf("")) == 0 &&
	          std::fabs(DecodeCarBrake(EncodeCarBrake(0.2f)) - 0.2f) < 0.003f,
	      "brake, 0..1");
	AmbientCarState s{};
	Check(s.steer == 0 && s.gas == 0 && s.brake == 0 && DecodeCarSteer(s.steer) == 0.0f,
	      "a row that says nothing is wheels straight, no gas, no brake");
}

// ---- the roster ----------------------------------------------------------------

struct Rec {
	int32_t  nextHandle = 1;
	int      pedSpawns = 0, copsBuilt = 0, pedsAdopted = 0, carsAdopted = 0;
	bool     copBuildWorks = true;
	uint16_t lastSaidNetId = 0, lastSaidSound = 0;
	uint8_t  lastSaidPlayer = INVALID_PLAYER;
	int      saidAmbient = 0, saidPlayer = 0;
	WantedViewer wanted[MAX_PLAYERS];
	uint32_t wantedCount = 0;
	LocalCopHandover queued[4];
	uint32_t queuedCount = 0;
	LocalPedSpeech lines[4];
	uint32_t lineCount = 0;
	int      controlsApplied = 0;
	float    lastSteer = 0.0f, lastGas = 0.0f, lastBrake = 0.0f;
};
Rec g;

bool ModelReady(uint16_t) { return true; }
bool CentreStub(Vec3 &out) {
	out = Vec3{0.0f, 0.0f, 0.0f};
	return true;
}
bool SpawnPed(RemoteAmbientPed &p) {
	p.poolHandle = g.nextHandle++;
	++g.pedSpawns;
	return true;
}
void DespawnPed(RemoteAmbientPed &p) { p.poolHandle = -1; }
bool SpawnCar(RemoteAmbientCar &c) {
	c.poolHandle = g.nextHandle++;
	return true;
}
void DespawnCar(RemoteAmbientCar &c) { c.poolHandle = -1; }
void CorrectCar(RemoteAmbientCar &, const VehicleTransform &) {}
bool AdoptPed(RemoteAmbientPed &p) {
	p.poolHandle = -1;
	++g.pedsAdopted;
	return true;
}
bool AdoptCar(RemoteAmbientCar &c) {
	c.poolHandle = -1;
	++g.carsAdopted;
	return true;
}
bool AdoptCop(RemoteAmbientPed &p) {
	if (!g.copBuildWorks)
		return false;
	p.poolHandle = -1;
	++g.copsBuilt;
	return true;
}
uint32_t DrainCops(const WantedViewer *w, uint32_t n, uint32_t, LocalCopHandover *out,
                   uint32_t max) {
	g.wantedCount = n;
	for (uint32_t i = 0; i < n && i < MAX_PLAYERS; ++i)
		g.wanted[i] = w[i];
	const uint32_t k = g.queuedCount < max ? g.queuedCount : max;
	for (uint32_t i = 0; i < k; ++i)
		out[i] = g.queued[i];
	g.queuedCount = 0;
	return k;
}
uint32_t DrainSpeech(LocalPedSpeech *out, uint32_t max) {
	const uint32_t k = g.lineCount < max ? g.lineCount : max;
	for (uint32_t i = 0; i < k; ++i)
		out[i] = g.lines[i];
	g.lineCount = 0;
	return k;
}
bool SayAmb(RemoteAmbientPed &p, uint16_t sound) {
	++g.saidAmbient;
	g.lastSaidNetId = p.netId;
	g.lastSaidSound = sound;
	return true;
}
bool SayPlayer(RemotePlayer &p, uint16_t sound) {
	++g.saidPlayer;
	g.lastSaidPlayer = p.playerId;
	g.lastSaidSound  = sound;
	return true;
}
void Controls(RemoteAmbientCar &c) {
	++g.controlsApplied;
	g.lastSteer = c.steer;
	g.lastGas   = c.gas;
	g.lastBrake = c.brake;
}

WorldBridge CopBridge() {
	g = Rec{};
	WorldBridge b;
	b.IsModelReady             = &ModelReady;
	b.SampleCrowdCentre        = &CentreStub;
	b.SpawnAmbientReplica      = &SpawnPed;
	b.DespawnAmbientReplica    = &DespawnPed;
	b.SpawnAmbientCarReplica   = &SpawnCar;
	b.DespawnAmbientCarReplica = &DespawnCar;
	b.CorrectAmbientCarReplica = &CorrectCar;
	b.AdoptAmbientPed          = &AdoptPed;
	b.AdoptAmbientCar          = &AdoptCar;
	b.AdoptAmbientCop          = &AdoptCop;
	b.DrainCopHandovers        = &DrainCops;
	b.DrainPedSpeech           = &DrainSpeech;
	b.SayAmbient               = &SayAmb;
	b.SayRemotePlayer          = &SayPlayer;
	b.ApplyAmbientCarControls  = &Controls;
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

void Join(Client &c, uint8_t id, float x, uint8_t stars) {
	S_PlayerJoin join{};
	InitHeader(join, 1000);
	join.playerId = id;
	join.netId    = static_cast<uint16_t>(200 + id);
	join.modelId  = 7;
	std::snprintf(join.nick, NICK_LEN, "p%u", static_cast<unsigned>(id));
	c.HandleMessage(Pack(join, CH_EVENT));
	S_PlayerState state{};
	InitHeader(state, 2000);
	state.playerId    = id;
	state.body.pos    = {x, 0.0f, 5.0f};
	state.body.health = 100.0f;
	state.body.flags  = FlagsWithWanted(0, stars, false);
	c.HandleMessage(Pack(state, CH_SNAPSHOT));
}

S_PedSpawn CopSpawn(uint16_t netId, uint8_t owner) {
	S_PedSpawn s{};
	InitHeader(s, 1000);
	s.ownerPlayerId = owner;
	s.netId         = netId;
	s.body.modelId  = 1;
	s.body.pedType  = AMBIENT_PEDTYPE_COP;
	s.body.pos      = {5.0f, 0.0f, 10.0f};
	return s;
}

void TestOurCopsGoToTheWantedPlayer() {
	std::printf("\nour police go out to the wanted player beside them\n");
	Client c;
	c.SetBridge(CopBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	Join(c, 1, 20.0f, 0);
	Join(c, 2, 40.0f, 4);

	const uint32_t before = c.CopHandoversSentForTest();
	c.TickCopHandoversForTest();
	Check(g.wantedCount == 2 && g.wanted[1].playerId == 2 && g.wanted[1].level == 4 &&
	          g.wanted[0].level == 0,
	      "the engine side is told who is where, with their stars");
	Check(c.CopHandoversSentForTest() == before, "nothing let go, nothing sent");

	g.queuedCount       = 1;
	g.queued[0]         = LocalCopHandover{};
	g.queued[0].toPlayerId = 2;
	g.queued[0].carNetId   = 900;
	g.queued[0].pedCount   = 2;
	g.queued[0].peds[0]    = 901;
	g.queued[0].peds[1]    = 902;
	c.TickCopHandoversForTest();
	Check(c.CopHandoversSentForTest() == before + 1, "a police car and its cops, one packet");
}

void TestNobodysStarsNoHandover() {
	std::printf("\nwith nobody wanted, the engine side is not even asked\n");
	Client c;
	c.SetBridge(CopBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	Join(c, 1, 20.0f, 0);
	g.queuedCount = 1;
	c.TickCopHandoversForTest();
	Check(g.wantedCount == 0 && g.queuedCount == 1, "no stars anywhere: nothing is decided");
}

void TestACopHandedToUsIsBuilt() {
	std::printf("\na cop handed to us for our stars is built as one\n");
	Client c;
	c.SetBridge(CopBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	Join(c, 1, 20.0f, 0);
	c.HandleMessage(Pack(CopSpawn(700, 1), CH_EVENT));
	c.HandleMessage(Pack(CopSpawn(701, 1), CH_EVENT));
	c.Tick();
	Check(g.pedSpawns == 2, "both police replicas built here");

	S_AmbientAdopt a{};
	InitHeader(a, 3000);
	a.wasOwnerPlayerId = 1;
	a.count            = 1;
	a.why              = AMBIENT_ADOPT_PURSUIT;
	a.rows[0]          = AmbientAdoptRow{700, AMBIENT_ADOPT_PED, 0};
	c.HandleMessage(Pack(a, CH_EVENT));
	Check(g.copsBuilt == 1 && g.pedsAdopted == 0, "a CCopPed in his place, not a civilian");
	Check(c.AmbientPed(700) == nullptr, "and the row is ours, not a replica's");

	a.why     = AMBIENT_ADOPT_LEFT;
	a.rows[0] = AmbientAdoptRow{701, AMBIENT_ADOPT_PED, 0};
	c.HandleMessage(Pack(a, CH_EVENT));
	Check(g.copsBuilt == 1 && c.AmbientPed(701) == nullptr,
	      "a leaver's cop is still let go of, not built");

	c.HandleMessage(Pack(CopSpawn(702, 1), CH_EVENT));
	c.Tick();
	g.copBuildWorks = false;
	a.why     = AMBIENT_ADOPT_PURSUIT;
	a.rows[0] = AmbientAdoptRow{702, AMBIENT_ADOPT_PED, 0};
	c.HandleMessage(Pack(a, CH_EVENT));
	Check(c.AmbientPed(702) == nullptr, "one that cannot be built is let go of");
}

void TestSomebodyElsesCopChangesOwner() {
	std::printf("\na cop handed to somebody else keeps our copy\n");
	Client c;
	c.SetBridge(CopBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	Join(c, 1, 20.0f, 0);
	Join(c, 2, 30.0f, 3);
	c.HandleMessage(Pack(CopSpawn(710, 1), CH_EVENT));
	c.Tick();
	S_AmbientAdopt a{};
	InitHeader(a, 3000);
	a.wasOwnerPlayerId = 1;
	a.count            = 1;
	a.why              = AMBIENT_ADOPT_PURSUIT;
	a.rows[0]          = AmbientAdoptRow{710, AMBIENT_ADOPT_PED, 2};
	c.HandleMessage(Pack(a, CH_EVENT));
	Check(c.AmbientPed(710) && c.AmbientPed(710)->ownerPlayerId == 2 &&
	          c.AmbientPed(710)->poolHandle >= 0 && g.copsBuilt == 0,
	      "the replica stays and takes player 2's rows");
}

void TestSpeechBothWays() {
	std::printf("\nwhat the crowd says goes out and comes in\n");
	Client c;
	c.SetBridge(CopBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	Join(c, 1, 20.0f, 0);
	c.HandleMessage(Pack(CopSpawn(720, 1), CH_EVENT));
	c.Tick();

	g.lineCount = 2;
	g.lines[0]  = LocalPedSpeech{SPEECH_AMBIENT, 55, 0x62};
	g.lines[1]  = LocalPedSpeech{SPEECH_PLAYER, INVALID_NETID, 0x63};
	const uint32_t before = c.SpeechSentForTest();
	c.TickPedSpeechForTest();
	Check(c.SpeechSentForTest() == before + 2, "each line our side heard goes out");

	S_PedSpeech s{};
	InitHeader(s, 4000);
	s.playerId = 1;
	s.who      = SPEECH_AMBIENT;
	s.netId    = 720;
	s.sound    = 0x7A;
	c.HandleMessage(Pack(s, CH_SNAPSHOT));
	Check(g.saidAmbient == 1 && g.lastSaidNetId == 720 && g.lastSaidSound == 0x7A,
	      "a line from his host is said by our copy");
	s.playerId = 2;
	c.HandleMessage(Pack(s, CH_SNAPSHOT));
	Check(g.saidAmbient == 1, "not from anybody else");
	s.playerId = 1;
	s.sound    = 0x10;
	c.HandleMessage(Pack(s, CH_SNAPSHOT));
	Check(g.saidAmbient == 1, "and never a sound outside the ped block");

	s.who   = SPEECH_PLAYER;
	s.netId = INVALID_NETID;
	s.sound = 0x62;
	c.HandleMessage(Pack(s, CH_SNAPSHOT));
	Check(g.saidPlayer == 1 && g.lastSaidPlayer == 1, "a player's own line, on his copy");
	s.playerId = 0;
	c.HandleMessage(Pack(s, CH_SNAPSHOT));
	Check(g.saidPlayer == 1, "our own never comes back to us");
}

void TestTheControlsReachTheReplica() {
	std::printf("\na traffic replica is given its driver's controls\n");
	Client c;
	c.SetBridge(CopBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	S_CarSpawn s{};
	InitHeader(s, 1000);
	s.ownerPlayerId = 1;
	s.netId         = 800;
	s.body.modelId  = 90;
	s.body.extra1   = -1;
	s.body.extra2   = -1;
	s.body.pos      = {10.0f, 0.0f, 10.0f};
	s.body.rot      = {0.0f, 0.0f, 0.0f, 1.0f};
	c.HandleMessage(Pack(s, CH_EVENT));
	c.Tick();

	S_CarStates rows{};
	InitHeader(rows, 5000);
	rows.ownerPlayerId = 1;
	rows.count         = 1;
	rows.cars[0].netId = 800;
	rows.cars[0].pos   = {11.0f, 0.0f, 10.0f};
	rows.cars[0].rot   = {0.0f, 0.0f, 0.0f, 1.0f};
	rows.cars[0].steer = EncodeCarSteer(-0.4f);
	rows.cars[0].gas   = EncodeCarGas(0.8f);
	rows.cars[0].brake = EncodeCarBrake(1.0f);
	c.HandleMessage(Pack(rows, CH_SNAPSHOT));
	c.CorrectAmbientCarsForTest();
	Check(g.controlsApplied >= 1 && std::fabs(g.lastSteer + 0.4f) < 0.006f &&
	          std::fabs(g.lastGas - 0.8f) < 0.01f && g.lastBrake == 1.0f,
	      "after the correction, the wheel and both pedals as its host has them");
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

// How many E8/E9/Jcc reach exactly `to`, and how many land inside its first
// five bytes, where a call-site redirection would not care but a detour would.
void CountBranches(const std::vector<uint8_t> &img, uint32_t to, uint32_t &calls,
                   uint32_t &into) {
	calls = into = 0;
	for (uint32_t va = kTextBegin; va + 6 <= kTextEnd; ++va) {
		const uint8_t b = At(img, va);
		uint32_t      target = 0;
		if (b == 0xE8 || b == 0xE9)
			target = va + 5 + Dword(img, va + 1);
		else if (b == 0x0F && (At(img, va + 1) & 0xF0) == 0x80)
			target = va + 6 + Dword(img, va + 2);
		else
			continue;
		if (target == to)
			++calls;
		else if (target > to && target < to + 5)
			++into;
	}
}

void TestTheAddressesAgainstTheImage() {
	std::printf("\nthe police and speech addresses against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check them\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	uint32_t calls = 0, into = 0;
	CountBranches(img, CPopulation__AddToPopulation, calls, into);
	Check(calls == 2 && CallsTo(img, ADD_TO_POPULATION_CALLS[0], CPopulation__AddToPopulation) &&
	          CallsTo(img, ADD_TO_POPULATION_CALLS[1], CPopulation__AddToPopulation) &&
	          into == 0,
	      "AddToPopulation is reached by exactly the two calls taken");
	Check(At(img, ADD_TO_POPULATION_CALLS[0] + 5) == 0x83 &&
	          At(img, ADD_TO_POPULATION_CALLS[0] + 7) == 0x10 &&
	          At(img, ADD_TO_POPULATION_CALLS[1] + 6) == 0x83 &&
	          At(img, ADD_TO_POPULATION_CALLS[1] + 8) == 0x10,
	      "and both callers pop four arguments: it is cdecl with four floats");
	Check(Dword(img, 0x004F4A96) == CPopulation__ms_nNumCop && At(img, 0x004F4A93) == 0x11,
	      "the gate compares ms_nNumCop with m_MaxCops");
	Check(Dword(img, 0x004F3A37) == CPopulation__ms_nNumCop &&
	          Dword(img, 0x004F3B2D) == CPopulation__ms_nNumCop &&
	          Dword(img, 0x004F3A4F) == 0x0095CB50 && Dword(img, 0x004F3B3E) == 0x0095CB50,
	      "and both callers fold it into ms_nTotalPeds before calling, not after");

	CountBranches(img, CPed__ServiceTalking, calls, into);
	Check(calls == 2 && CallsTo(img, SERVICE_TALKING_CALLS[0], CPed__ServiceTalking) &&
	          CallsTo(img, SERVICE_TALKING_CALLS[1], CPed__ServiceTalking) && into == 0,
	      "ServiceTalking is reached by exactly the two calls taken");
	Check(At(img, SERVICE_TALKING_CALLS[0] - 2) == 0x89 &&
	          At(img, SERVICE_TALKING_CALLS[1] - 2) == 0x89,
	      "each with the ped moved into ecx just before");
	Check(Dword(img, 0x004E5974) == PED_LAST_SOUND_START &&
	          Dword(img, 0x004E59ED) == PED_LAST_QUEUED_SOUND &&
	          Dword(img, 0x004E59F4) == PED_QUEUED_SOUND &&
	          *reinterpret_cast<const uint16_t *>(&img[0x004E59F8 - IMAGE_BASE]) == SOUND_NO_SOUND,
	      "what it writes when it plays a line, at the offsets read");
	Check(CallsTo(img, 0x004E596D, 0x0057C840), "right after the one PlayOneShot");

	Check(At(img, CPed__Say) == 0x53 && At(img, 0x004E5A59) == 0xC2 &&
	          At(img, 0x004E5A5A) == 0x04 && CallsTo(img, 0x004E5A1D, 0x004D48E0),
	      "Say is thiscall with one argument and asks IsPlayer first");
	Check(At(img, 0x004E5B10) == 0x66 && Dword(img, 0x004E5B13) == PED_QUEUED_SOUND,
	      "and outranks against m_queuedSound");

	Check(CallsTo(img, 0x004C11BE, 0x004C41C0) && At(img, 0x004C11B9) == 6 &&
	          Dword(img, 0x004C11CB) == CCopPed__vtable &&
	          Dword(img, 0x004C11D5) == COP_PED_COP_TYPE,
	      "CCopPed's ctor: CPed(PEDTYPE_COP), its vtable, m_nCopType");
	Check(At(img, 0x004C13DB) == 0xC2 && At(img, 0x004C13DC) == 0x04,
	      "and it is thiscall with the one argument");
	Check(CallsTo(img, 0x0043BAC6, CCopPed__ctor) && Dword(img, 0x0043BAAF) == SIZEOF_COP_PED &&
	          CallsTo(img, 0x004F53A7, CCopPed__ctor),
	      "CREATE_CHAR and AddPed both build one in 0x558 bytes");
	const uint32_t table = 0x005F8268;
	Check(Dword(img, table) == 0x004C11EB && At(img, 0x004C11F2) == 1 &&
	          Dword(img, table + 4) == 0x004C1299 && At(img, 0x004C12A0) == 3 &&
	          Dword(img, table + 8) == 0x004C123A && At(img, 0x004C1241) == 2 &&
	          Dword(img, table + 12) == 0x004C12F5 && At(img, 0x004C12FC) == 4,
	      "and the model each eCopType gets, as CopTypeForPedModel has it");

	Check(At(img, CVehicle__SetUpDriver) == 0x53 &&
	          CallsTo(img, 0x005520DA, 0x004F5800) &&
	          CallsTo(img, 0x00552179, 0x004F5800) && At(img, 0x005521FB) == 0xC2,
	      "SetUpDriver and SetupPassenger both fill the seat through AddPedInCar");
	const uint32_t cars = 0x005FA9A0;
	Check(Dword(img, cars + (116 - 97) * 4) == 0x004F5870 &&
	          Dword(img, cars + (107 - 97) * 4) == 0x004F588F &&
	          Dword(img, cars + (117 - 97) * 4) == 0x004F587E &&
	          Dword(img, cars + (122 - 97) * 4) == 0x004F58A0 &&
	          Dword(img, cars + (123 - 97) * 4) == 0x004F58A0,
	      "AddPedInCar's police arms, by model");
	Check(At(img, 0x004F5871) == 0xED && At(img, 0x004F587F) == 2 &&
	          At(img, 0x004F5890) == 1 && At(img, 0x004F58A1) == 3,
	      "and the eCopType each puts in the car, as CopTypeForCarModel has it");
}

} // namespace

int RunCopChaseTests() {
	TestWhoGetsTheCop();
	TestCopModelsAreTheEngines();
	TestACopIsRebuiltOnlyForAPursuit();
	TestTheCopGate();
	TestWhatIsSaid();
	TestTheControlsOnTheWire();
	TestOurCopsGoToTheWantedPlayer();
	TestNobodysStarsNoHandover();
	TestACopHandedToUsIsBuilt();
	TestSomebodyElsesCopChangesOwner();
	TestSpeechBothWays();
	TestTheControlsReachTheReplica();
	TestTheAddressesAgainstTheImage();
	return g_copFailures;
}
