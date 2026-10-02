// Nothing synced appearing or disappearing in front of a player:
// client/src/game/crowdfade.h and the roster's half of C_CrowdGone and
// C_PlayerView. The rules walked with no engine, the roster through a bridge
// of its own, and - when a copy of the retail exe is handed over - every
// address the fade relies on, read back out of it.

#include "client.h"
#include "game/crowdfade.h"

#include <coopiii/protocol.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_fadeFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_fadeFailures;
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

PlayerViewBody View(float x, float y, float fx, float fy, float fz = 0.0f) {
	PlayerViewBody v{};
	v.pos = {x, y, 10.0f};
	v.fwd = {fx, fy, fz};
	return v;
}

// ---- the rules -----------------------------------------------------------------

void TestWhatACameraSees() {
	std::printf("\nwhat a camera sees, flat, as the generators measure\n");
	const PlayerViewBody east = View(0.0f, 0.0f, 1.0f, 0.0f);
	Check(ViewSees(east, {50.0f, 0.0f, 0.0f}) && ViewSees(east, {50.0f, 40.0f, 0.0f}),
	      "straight ahead, and 39 degrees off it");
	Check(!ViewSees(east, {50.0f, 120.0f, 0.0f}) && !ViewSees(east, {-50.0f, 0.0f, 0.0f}),
	      "not 67 degrees off it, nor behind");
	Check(!ViewSees(east, {0.0f, 50.0f, 0.0f}), "nor square to the side");
	Check(ViewSees(east, {0.3f, -0.4f, 0.0f}), "and anything on top of the camera is seen");
	const PlayerViewBody down = View(0.0f, 0.0f, 0.0f, 0.0f, -1.0f);
	Check(ViewSees(down, {-30.0f, 20.0f, 0.0f}),
	      "a camera looking straight down sees all round it");
	PlayerViewBody nan = east;
	nan.fwd.x = std::nanf("");
	Check(!ViewSees(nan, {50.0f, 0.0f, 0.0f}), "and a camera of NaNs sees nothing");

	Check(InViewWithin(east, {80.0f, 0.0f, 0.0f}, CAR_INVIEW_MIN_M) &&
	          !InViewWithin(east, {95.0f, 0.0f, 0.0f}, CAR_INVIEW_MIN_M),
	      "a car's close range is the engine's 90 m");
	Check(InViewWithin(east, {35.0f, 0.0f, 0.0f}, PED_INVIEW_MIN_M) &&
	          !InViewWithin(east, {45.0f, 0.0f, 0.0f}, PED_INVIEW_MIN_M),
	      "a pedestrian's is its 40 m");

	const PlayerViewBody views[2] = {View(1000.0f, 0.0f, 1.0f, 0.0f), east};
	Check(SpotInSomebodysView({60.0f, 5.0f, 0.0f}, true, views, 2),
	      "a car spot 60 m in front of anybody's camera is passed over");
	Check(!SpotInSomebodysView({60.0f, 5.0f, 0.0f}, false, views, 2),
	      "the same spot is fine for a pedestrian, past his 40 m");
	Check(!SpotInSomebodysView({-60.0f, 5.0f, 0.0f}, true, views, 2) &&
	          !SpotInSomebodysView({60.0f, 5.0f, 0.0f}, true, views, 0) &&
	          !SpotInSomebodysView({60.0f, 5.0f, 0.0f}, true, nullptr, 2),
	      "behind everybody, or with no cameras known, nothing is passed over");
}

void TestComingAndGoing() {
	std::printf("\na copy coming in and going out, and what a host says\n");
	const PlayerViewBody east = View(0.0f, 0.0f, 1.0f, 0.0f);
	Check(HoldBirthInView(true, false, false, 100, true, east, {50.0f, 0.0f, 0.0f}),
	      "a fresh car 50 m ahead of our camera is held");
	Check(!HoldBirthInView(true, false, false, 100, true, east, {-50.0f, 0.0f, 0.0f}) &&
	          !HoldBirthInView(true, false, false, 100, true, east, {150.0f, 0.0f, 0.0f}),
	      "behind us, or past 90 m, it is built at once");
	Check(!HoldBirthInView(true, true, false, 100, true, east, {50.0f, 0.0f, 0.0f}),
	      "never the mission's");
	Check(!HoldBirthInView(true, false, true, 100, true, east, {50.0f, 0.0f, 0.0f}),
	      "never one built here before, coming back into reach");
	Check(!HoldBirthInView(true, false, false, BIRTH_HOLD_MAX_MS, true, east,
	                       {50.0f, 0.0f, 0.0f}),
	      "and not for ever");
	Check(!HoldBirthInView(false, false, false, 100, false, east, {10.0f, 0.0f, 0.0f}),
	      "without a camera of our own, nothing is held");

	bool up = true, down = true;
	for (uint32_t t = 10; t <= 1000; t += 10) {
		up   = up && FadeInAlpha(t) >= FadeInAlpha(t - 10);
		down = down && FadeOutAlpha(t) <= FadeOutAlpha(t - 10);
	}
	Check(FadeInAlpha(0) == 0 && FadeInAlpha(CROWD_FADE_IN_MS) == 255 && up,
	      "a fade in runs from 0 to 255 and never back");
	Check(FadeOutAlpha(0) == 255 && FadeOutAlpha(CROWD_FADE_OUT_MS) == 0 && down,
	      "a fade out runs from 255 to 0 and never back");

	Check(FadesOut(true, false, true) && !FadesOut(true, false, false) &&
	          !FadesOut(false, false, true) && !FadesOut(true, true, true),
	      "only a built copy on our screen fades, never the mission's");
	Check(FadeOutOver(CROWD_FADE_OUT_MS, true, true) && FadeOutOver(10, true, false) &&
	          FadeOutOver(10, false, true) && !FadeOutOver(10, true, true),
	      "a fade is over when it has run out, gone off the screen, or the copy has gone");

	Check(CarFadesWhenDropped(CarReap::Far, true, false) &&
	          CarFadesWhenDropped(CarReap::Stopped, true, false) &&
	          CarFadesWhenDropped(CarReap::Respawn, true, false) &&
	          CarFadesWhenDropped(CarReap::Island, true, false),
	      "a car of ours dropped for our player's sake goes out to fade");
	Check(!CarFadesWhenDropped(CarReap::Wreck, true, false) &&
	          !CarFadesWhenDropped(CarReap::Other, true, false) &&
	          !CarFadesWhenDropped(CarReap::Far, false, false) &&
	          !CarFadesWhenDropped(CarReap::Far, true, true),
	      "not a wreck, nor one taken for a reason that holds everywhere, nor unnamed, "
	      "nor the mission's");
	Check(PedFadesWhenDropped(PedDrop::Reaper, true, false) &&
	          PedFadesWhenDropped(PedDrop::Respawn, true, false) &&
	          !PedFadesWhenDropped(PedDrop::Other, true, false) &&
	          !PedFadesWhenDropped(PedDrop::Reaper, true, true),
	      "the same for a pedestrian");

	PlayerViewBody v = east;
	Check(PlayerViewSane(v), "a camera looking east is sane");
	v.fwd = {0.0f, 0.0f, 0.0f};
	Check(!PlayerViewSane(v), "one looking nowhere is not");
	v     = east;
	v.pos = {2.0e6f, 0.0f, 0.0f};
	Check(!PlayerViewSane(v), "nor one from outside any world");
}

// ---- the roster ----------------------------------------------------------------

struct Rec {
	Vec3           centre{0.0f, 0.0f, 0.0f};
	PlayerViewBody ours{};
	bool           haveView   = false;
	bool           onScreen   = true;
	int32_t        nextHandle = 1;
	int            carSpawns = 0, carDespawns = 0, pedSpawns = 0, pedDespawns = 0;
	int            alphaWrites = 0;
	uint8_t        lastAlpha   = 0;
	bool           lastFading  = false;
	uint8_t        firstKind   = 0xFF;
	PlayerViewBody views[MAX_PLAYERS];
	uint32_t       viewCount = 0xFFFF;
	CrowdGoneRow   faded[4];
	uint32_t       fadedCount = 0;
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
bool SpawnPed(RemoteAmbientPed &p) {
	p.poolHandle = g.nextHandle++;
	++g.pedSpawns;
	return true;
}
void DespawnPed(RemoteAmbientPed &p) {
	p.poolHandle = -1;
	++g.pedDespawns;
}
bool LocalView(PlayerViewBody &out) {
	out = g.ours;
	return g.haveView;
}
bool OnScreen(uint8_t, int32_t) { return g.onScreen; }
void Alpha(uint8_t kind, int32_t, uint8_t alpha, bool fading) {
	if (g.firstKind == 0xFF && fading)
		g.firstKind = kind;
	++g.alphaWrites;
	g.lastAlpha  = alpha;
	g.lastFading = fading;
}
void Views(const PlayerViewBody *v, uint32_t n) {
	g.viewCount = n;
	for (uint32_t i = 0; i < n && i < MAX_PLAYERS; ++i)
		g.views[i] = v[i];
}
uint32_t DrainFaded(CrowdGoneRow *out, uint32_t max) {
	const uint32_t n = g.fadedCount < max ? g.fadedCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g.faded[i];
	g.fadedCount = 0;
	return n;
}

WorldBridge FadeBridge() {
	g = Rec{};
	WorldBridge b;
	b.IsModelReady             = &ModelReady;
	b.SpawnAmbientCarReplica   = &SpawnCar;
	b.DespawnAmbientCarReplica = &DespawnCar;
	b.SpawnAmbientReplica      = &SpawnPed;
	b.DespawnAmbientReplica    = &DespawnPed;
	b.SampleCrowdCentre        = &CentreStub;
	b.SampleLocalView          = &LocalView;
	b.CrowdReplicaOnScreen     = &OnScreen;
	b.SetCrowdReplicaAlpha     = &Alpha;
	b.NoteRemoteViews          = &Views;
	b.DrainFadedAmbient        = &DrainFaded;
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

S_CarSpawn CarSpawn(uint16_t netId, float x, uint32_t tempId = 0, uint8_t flags = 0) {
	S_CarSpawn s{};
	InitHeader(s, 1000);
	s.ownerPlayerId = 1;
	s.tempId        = tempId;
	s.netId         = netId;
	s.body.modelId  = 90;
	s.body.extra1   = -1;
	s.body.extra2   = -1;
	s.body.flags    = flags;
	s.body.pos      = {x, 0.0f, 10.0f};
	s.body.rot      = {0.0f, 0.0f, 0.0f, 1.0f};
	return s;
}

S_PedSpawn PedSpawn(uint16_t netId, float x, uint32_t tempId = 0) {
	S_PedSpawn s{};
	InitHeader(s, 1000);
	s.ownerPlayerId = 1;
	s.tempId        = tempId;
	s.netId         = netId;
	s.body.modelId  = 30;
	s.body.pedType  = AMBIENT_PEDTYPE_CIVMALE;
	s.body.pos      = {x, 3.0f, 10.0f};
	return s;
}

S_CrowdGone Gone(std::initializer_list<CrowdGoneRow> rows) {
	S_CrowdGone out{};
	InitHeader(out, 1200);
	for (const CrowdGoneRow &r : rows)
		out.rows[out.count++] = r;
	return out;
}

void TestACopyIsFadedIn() {
	std::printf("\na copy built here fades in, as the generators' own do\n");
	Client c;
	c.SetBridge(FadeBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(700, 40.0f), CH_EVENT));
	c.Tick();
	Check(g.carSpawns == 1 && g.alphaWrites >= 1 && g.lastAlpha < 64 && !g.lastFading,
	      "the frame it is built, its alpha is written near 0, not fading out");
	std::this_thread::sleep_for(std::chrono::milliseconds(CROWD_FADE_IN_MS + 50));
	c.Tick();
	Check(g.lastAlpha == 255, "and it is all there once the fade in has run");
	const int writes = g.alphaWrites;
	c.Tick();
	Check(g.alphaWrites == writes, "after which nothing is written to it every frame");
}

void TestAGoneCopyOnScreenFadesOut() {
	std::printf("\na copy its host gave up on fades out on our screen\n");
	Client c;
	c.SetBridge(FadeBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(701, 40.0f), CH_EVENT));
	c.HandleMessage(Pack(PedSpawn(702, 30.0f), CH_EVENT));
	c.Tick();
	Check(g.carSpawns == 1 && g.pedSpawns == 1, "a car and a pedestrian of player 1's are here");
	std::this_thread::sleep_for(std::chrono::milliseconds(CROWD_FADE_IN_MS + 50));
	c.Tick();
	Check(g.lastAlpha == 255, "and all there");

	g.firstKind = 0xFF;
	c.HandleMessage(Pack(Gone({{701, AMBIENT_ADOPT_CAR, 0}, {702, AMBIENT_ADOPT_PED, 0}}),
	                     CH_EVENT));
	const RemoteAmbientCar *car = c.AmbientCar(701);
	const RemoteAmbientPed *ped = c.AmbientPed(702);
	Check(car && car->Leaving() && ped && ped->Leaving() && g.carDespawns == 0 &&
	          g.pedDespawns == 0,
	      "both stay built, fading, rather than vanishing");
	Check(car && car->ownerPlayerId == INVALID_PLAYER && ped &&
	          ped->ownerPlayerId == INVALID_PLAYER,
	      "and belong to nobody: no row of anybody's moves them now");
	Check(g.firstKind == AMBIENT_ADOPT_PED && g.lastFading,
	      "the pedestrian is decided before the car, and bFadeOut is asked for");
	Check(c.CrowdFadesStartedForTest() == 2, "two fades started");

	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	c.Tick();
	Check(g.lastAlpha < 255 && g.lastAlpha > 0 && g.carDespawns == 0,
	      "a moment later both are part way out");
	Check(g.carSpawns == 1 && g.pedSpawns == 1, "and nothing builds them again meanwhile");

	g.onScreen = false;
	c.Tick();
	Check(g.carDespawns == 1 && g.pedDespawns == 1 && !c.AmbientCar(701) && !c.AmbientPed(702),
	      "once they are off our screen they go at once, as the engine takes its own");

	// One given up on while still fading in carries on from where it was,
	// rather than coming up to full to go out again.
	g.onScreen = true;
	c.HandleMessage(Pack(CarSpawn(715, 40.0f), CH_EVENT));
	c.Tick();
	const uint8_t was = g.lastAlpha;
	c.HandleMessage(Pack(Gone({{715, AMBIENT_ADOPT_CAR, 0}}), CH_EVENT));
	Check(g.lastFading && g.lastAlpha <= was + 16,
	      "a car given up on in its first frame here goes out from where its fade in was");
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	c.Tick();
	Check(!c.AmbientCar(715) && g.carDespawns == 2, "and, barely there, is gone at once");
}

void TestAGoneCopyOffScreenGoesAtOnce() {
	std::printf("\na copy its host gave up on, off our screen or the mission's, goes at once\n");
	Client c;
	c.SetBridge(FadeBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(703, 40.0f), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(704, 50.0f, 0, AMBIENT_MISSION), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(705, 1000.0f), CH_EVENT));
	c.Tick();
	g.onScreen = false;
	c.HandleMessage(Pack(Gone({{703, AMBIENT_ADOPT_CAR, 0}}), CH_EVENT));
	Check(!c.AmbientCar(703) && g.carDespawns == 1, "off our screen: gone at once");
	g.onScreen = true;
	c.HandleMessage(Pack(Gone({{704, AMBIENT_ADOPT_CAR, 0}}), CH_EVENT));
	Check(!c.AmbientCar(704) && g.carDespawns == 2,
	      "the mission's car, even on our screen: gone at once");
	c.HandleMessage(Pack(Gone({{705, AMBIENT_ADOPT_CAR, 0}}), CH_EVENT));
	Check(!c.AmbientCar(705) && g.carDespawns == 2,
	      "one never built here has nothing to fade, and its row goes");
	Check(c.CrowdFadesStartedForTest() == 0, "no fade was started for any of them");
}

void TestANameReusedEndsTheFade() {
	std::printf("\na fading copy whose name the session gives to somebody new\n");
	Client c;
	c.SetBridge(FadeBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(PedSpawn(706, 30.0f), CH_EVENT));
	c.Tick();
	c.HandleMessage(Pack(Gone({{706, AMBIENT_ADOPT_PED, 0}}), CH_EVENT));
	Check(c.AmbientPed(706) && c.AmbientPed(706)->Leaving(), "fading");
	c.HandleMessage(Pack(PedSpawn(706, 90.0f), CH_EVENT));
	const RemoteAmbientPed *fresh = c.AmbientPed(706);
	Check(g.pedDespawns == 1 && fresh && !fresh->Leaving() && fresh->ownerPlayerId == 1 &&
	          fresh->last.pos.x == 90.0f,
	      "the old copy goes at once and the new pedestrian takes the name");
	c.HandleMessage(Pack(PedSpawn(706, 120.0f), CH_EVENT));
	Check(c.AmbientPed(706) && c.AmbientPed(706)->last.pos.x == 90.0f,
	      "a second spawn of a name we know is still the overlap it always was");
}

void TestAFreshCopyInOurViewIsHeld() {
	std::printf("\na fresh copy made inside our view at close range is held back\n");
	Client c;
	c.SetBridge(FadeBridge());
	g.haveView = true;
	g.ours     = View(0.0f, 0.0f, 1.0f, 0.0f);
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(710, 50.0f, 77), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(711, -50.0f, 78), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(712, 50.0f, 0), CH_EVENT));
	c.HandleMessage(Pack(PedSpawn(713, 20.0f, 79), CH_EVENT));
	c.HandleMessage(Pack(PedSpawn(714, 60.0f, 80), CH_EVENT));
	c.Tick();
	const RemoteAmbientCar *ahead  = c.AmbientCar(710);
	const RemoteAmbientCar *behind = c.AmbientCar(711);
	const RemoteAmbientCar *back   = c.AmbientCar(712);
	Check(ahead && ahead->poolHandle < 0 && behind && behind->poolHandle >= 0,
	      "a car made 50 m in front of our camera waits; one made behind us is built");
	Check(back && back->poolHandle >= 0,
	      "a backfill is not a birth: built where it stands, and fading in");
	Check(c.AmbientPed(713) && c.AmbientPed(713)->poolHandle < 0 && c.AmbientPed(714) &&
	          c.AmbientPed(714)->poolHandle >= 0,
	      "a pedestrian 20 m in front waits; one 60 m off, past the engine's 40, is built");
	Check(c.BirthsHeldForTest() == 2, "two held, counted once each");
	c.Tick();
	Check(c.BirthsHeldForTest() == 2, "and not counted again on the next frame");

	g.ours = View(0.0f, 0.0f, -1.0f, 0.0f);
	c.Tick();
	Check(c.AmbientCar(710)->poolHandle >= 0 && c.AmbientPed(713)->poolHandle >= 0,
	      "we look away and both are built, out of our sight");
}

void TestOurOwnGoAndOurCamera() {
	std::printf("\nwhat our engine gave up on goes out to fade, and our camera goes out\n");
	Client c;
	c.SetBridge(FadeBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	g.fadedCount = 2;
	g.faded[0]   = CrowdGoneRow{720, AMBIENT_ADOPT_CAR, 0};
	g.faded[1]   = CrowdGoneRow{721, AMBIENT_ADOPT_PED, 0};
	c.TickLocalAmbientPedsForTest();
	Check(c.CrowdGoneSentForTest() == 2 && g.fadedCount == 0,
	      "a car and a pedestrian of ours went out as one C_CrowdGone");

	g.haveView = true;
	g.ours     = View(5.0f, 5.0f, 0.0f, 1.0f);
	c.SendLocalViewForTest(10000);
	c.SendLocalViewForTest(10100);
	Check(c.ViewsSentForTest() == 1, "our camera went out once, and not again 100 ms later");
	c.SendLocalViewForTest(10000 + PLAYER_VIEW_SEND_MS);
	Check(c.ViewsSentForTest() == 2, "but again a quarter of a second on");
	g.ours.fwd = {0.0f, 0.0f, 0.0f};
	c.SendLocalViewForTest(20000);
	Check(c.ViewsSentForTest() == 2, "a camera looking nowhere is not sent");
}

void TestTheirCamerasReachOurGenerators() {
	std::printf("\nthe other players' cameras reach our generators\n");
	Client c;
	c.SetBridge(FadeBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	S_PlayerJoin join{};
	InitHeader(join, 1000);
	join.playerId = 2;
	join.netId    = 202;
	join.modelId  = 7;
	std::strncpy(join.nick, "bob", NICK_LEN - 1);
	c.HandleMessage(Pack(join, CH_EVENT));
	c.PushViewsForTest(true);
	Check(g.viewCount == 0, "a player who has said nothing of his camera has none");

	S_PlayerView v{};
	InitHeader(v, 1100);
	v.playerId = 2;
	v.body     = View(100.0f, 0.0f, 0.0f, 1.0f);
	c.HandleMessage(Pack(v, CH_SNAPSHOT));
	c.PushViewsForTest(true);
	Check(g.viewCount == 1 && g.views[0].pos.x == 100.0f && g.views[0].fwd.y == 1.0f,
	      "once he has, it is handed to the engine side");
	v.playerId = 0;
	v.body     = View(-5.0f, 0.0f, 1.0f, 0.0f);
	c.HandleMessage(Pack(v, CH_SNAPSHOT));
	v.playerId = 2;
	v.body.fwd = {0.0f, 0.0f, 0.0f};
	c.HandleMessage(Pack(v, CH_SNAPSHOT));
	c.PushViewsForTest(true);
	Check(g.viewCount == 1 && g.views[0].pos.x == 100.0f,
	      "our own id and a camera looking nowhere are both ignored");
	c.PushViewsForTest(false);
	Check(g.viewCount == 0, "and out of a session nobody looks anywhere");
}

void TestAReleaseByUsFades() {
	std::printf("\nsomebody's crowd handed to us that we cannot take is let go of as a fade\n");
	Client c;
	c.SetBridge(FadeBridge());
	c.HandleMessage(Pack(Welcome(), CH_EVENT));
	c.HandleMessage(Pack(CarSpawn(730, 1000.0f), CH_EVENT));
	c.Tick();
	S_AmbientAdopt adopt{};
	InitHeader(adopt, 1300);
	adopt.wasOwnerPlayerId = 1;
	adopt.count            = 1;
	adopt.why              = AMBIENT_ADOPT_LET_GO;
	adopt.rows[0]          = AmbientAdoptRow{730, AMBIENT_ADOPT_CAR, 0};
	c.HandleMessage(Pack(adopt, CH_EVENT));
	Check(c.CrowdGoneSentForTest() == 1 && !c.AmbientCar(730),
	      "one never built here cannot be taken: it is let go of as C_CrowdGone");
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

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> want) {
	uint32_t i = 0;
	for (uint8_t b : want)
		if (At(img, va + i++) != b)
			return false;
	return true;
}

void TestTheAddressesAgainstTheImage() {
	std::printf("\nthe fade's addresses against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check them\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(Bytes(img, CAR_GEN_FADE_IN_CALL - 3, {0x6A, 0x00, 0x50}) &&
	          CallsTo(img, CAR_GEN_FADE_IN_CALL, CVisibilityPlugins__SetClumpAlpha) &&
	          CallsTo(img, CAR_GEN_FADE_IN_CALL + 9, CEntity__GetIsOnScreen),
	      "the traffic generator sets a new car's alpha to 0, then asks if it is on screen");
	Check(Bytes(img, 0x00417B4C, {0xD9, 0x05}) && Dword(img, 0x00417B4E) == CAR_GEN_INVIEW_MIN_M_AT &&
	          Bytes(img, 0x00417B52, {0xD8, 0x0D}) && Dword(img, 0x00417B54) == 0x006FADE8 &&
	          Float(img, CAR_GEN_INVIEW_MIN_M_AT) == CAR_INVIEW_MIN_M,
	      "and deletes one on screen nearer the camera than 90 m times the multiplier");
	Check(Bytes(img, PED_GEN_FADE_IN_CALL - 3, {0x6A, 0x00, 0x50}) &&
	          CallsTo(img, PED_GEN_FADE_IN_CALL, CVisibilityPlugins__SetClumpAlpha),
	      "the pedestrian generator sets a new pedestrian's alpha to 0");
	Check(CallsTo(img, 0x004F5132, 0x004F6410) && Bytes(img, 0x004F5137, {0xD8, 0x0D}) &&
	          Dword(img, 0x004F5139) == PED_GEN_INVIEW_MIN_M_AT &&
	          Float(img, PED_GEN_INVIEW_MIN_M_AT) == PED_INVIEW_MIN_M &&
	          CallsTo(img, 0x004F50E5, 0x0042C760),
	      "and adds none in view nearer than 40 m times PedCreationDistMultiplier");

	Check(CallsTo(img, CAR_PROCESS_ALPHA_GET, CVisibilityPlugins__GetClumpAlpha) &&
	          Bytes(img, CAR_PROCESS_FADE_TEST,
	                {0x8A, 0x95, 0xF7, 0x01, 0x00, 0x00, 0x59, 0xC0, 0xEA, 0x03, 0x80, 0xE2,
	                 0x01}) &&
	          Bytes(img, 0x0053185D, {0x83, 0xE8, 0x08}) &&
	          Bytes(img, 0x0053186F, {0x83, 0xC0, 0x10}) &&
	          CallsTo(img, CAR_PROCESS_ALPHA_SET, CVisibilityPlugins__SetClumpAlpha),
	      "CAutomobile::ProcessControl blends a car's alpha by bFadeOut, bit 3 of +0x1F7: "
	      "-8 or +16");
	Check(offs::VEH_FLAGS_C == 0x1F7 && offs::VEH_FADE_OUT == 1u << 3,
	      "which is VEH_FLAGS_C and VEH_FADE_OUT");
	Check(Bytes(img, 0x005314D9, {0x8A, 0x95, 0x24, 0x01, 0x00, 0x00}) &&
	          At(img, 0x005314FE) == 0xC3 && 0x005314D9 > CAutomobile__ProcessControl &&
	          Bytes(img, CAutomobile__ProcessControl, {0xD9, 0x05}),
	      "and its one early return before the blend is the zone level test");
	Check(CallsTo(img, 0x004C8948, CVisibilityPlugins__GetClumpAlpha) &&
	          CallsTo(img, 0x004C8981, CVisibilityPlugins__SetClumpAlpha) &&
	          offs::PED_FLAGS_G == 0x15A && offs::PED_FADE_OUT == 0x80,
	      "CPed::ProcessControl does the same with bit 7 of +0x15A");

	Check(TheCamera__Forward == TheCamera + offs::MATRIX_FWD &&
	          TheCamera__Position == TheCamera + offs::POSITION,
	      "the camera's forward axis and position sit where every CPlaceable keeps them");
	Check(Bytes(img, CAMERA_FORWARD_READ, {0xD9, 0x05}) &&
	          Dword(img, CAMERA_FORWARD_READ + 2) == TheCamera__Forward &&
	          Bytes(img, CAMERA_FORWARD_READ + 6, {0xD9, 0xE0, 0xD9, 0x05}) &&
	          Dword(img, CAMERA_FORWARD_READ + 10) == TheCamera__Forward + 4 &&
	          Bytes(img, CAMERA_FORWARD_READ + 14, {0xD9, 0xF3}),
	      "the renderer turns its x and y into a heading: fld, fchs, fld, fpatan");
	Check(Bytes(img, CAMERA_FORWARD_WRITE, {0xD9, 0x1D}) &&
	          Dword(img, CAMERA_FORWARD_WRITE + 2) == TheCamera__Forward &&
	          Dword(img, CAMERA_FORWARD_WRITE + 12) == TheCamera__Forward + 4 &&
	          Dword(img, CAMERA_FORWARD_WRITE + 22) == TheCamera__Forward + 8 &&
	          Dword(img, CAMERA_FORWARD_WRITE + 32) == TheCamera__Forward + 0x10,
	      "and the camera writes three floats there, then the up axis 0x10 on");
	Check(Bytes(img, 0x0054F3A2, {0xBE}) && Dword(img, 0x0054F3A3) == TheCamera__Position,
	      "the position is the one CTrain::UpdateTrains loads");
}

} // namespace

int RunCrowdFadeTests() {
	TestWhatACameraSees();
	TestComingAndGoing();
	TestACopyIsFadedIn();
	TestAGoneCopyOnScreenFadesOut();
	TestAGoneCopyOffScreenGoesAtOnce();
	TestANameReusedEndsTheFade();
	TestAFreshCopyInOurViewIsHeld();
	TestOurOwnGoAndOurCamera();
	TestTheirCamerasReachOurGenerators();
	TestAReleaseByUsFades();
	TestTheAddressesAgainstTheImage();
	return g_fadeFailures;
}
