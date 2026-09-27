// The session's one mission, client side (missionsync.h): what goes out at a
// start gate, a launch, a checkpoint and an end, what the log says to whom,
// and when a participant's $ONMISSION follows the session's.
//
// Nothing here runs a script. The engine half is a stub that records what it
// was asked to do, and the wire is a list of the packets that would have gone.

#include "game/mission.h"
#include "game/replay.h"
#include "game/seatplan.h"
#include "missionsync.h"

#include <coopiii/mission.h>
#include <coopiii/net.h>
#include <coopiii/protocol.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

using namespace coopiii;

namespace {

int g_missionFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_missionFailures;
}

struct Roster {
	const char                  *nicks[MAX_PLAYERS] = {"alice", "bob", "carol", "dave"};
	std::vector<MissionPresence> others;
	std::vector<std::string>     feed;
	std::vector<std::string>     status;
	std::vector<std::vector<uint8_t>> sent;
};

Roster g_roster;
int    g_mirrorCalls = 0;
bool   g_mirror      = false;
int    g_fails       = 0;
uint8_t g_failReason = 0xFF;
int    g_effects     = 0;
int    g_ended       = 0;
// The stub game's campaign: its globals, which life of main.scm it is on
// (0: none yet) and which main.scm.
std::vector<uint32_t>       g_applied;
// What a TELEPORT was run with: this machine's rank and how many move.
std::vector<std::pair<uint8_t, uint8_t>> g_teleports;
std::map<uint16_t, int32_t> g_globals;
uint32_t                    g_life = 1;
uint32_t                    g_hash = 0xC0FFEE;

MissionBridge StubBridge() {
	MissionBridge b;
	b.SetOnMission = [](bool on) {
		++g_mirrorCalls;
		g_mirror = on;
	};
	b.FailMission = [](uint8_t reason) {
		++g_fails;
		g_failReason = reason;
	};
	b.RunEffect = [](const MissionEffectBody &) {
		++g_effects;
		return true;
	};
	b.EndEffects = []() { ++g_ended; };
	b.Teleport   = [](const MissionEffectBody &, uint8_t rank, uint8_t count) {
		g_teleports.emplace_back(rank, count);
		return true;
	};
	b.ApplyCampaign = [](const CampaignDeltaBody &body) {
		g_applied.push_back(body.seq);
		for (uint8_t i = 0; i < body.valueCount; ++i)
			g_globals[body.values[i].offset] = body.values[i].value;
		return true;
	};
	b.ScriptLife = []() { return g_life; };
	b.ScriptHash = []() { return g_hash; };
	b.ReadGlobal = [](uint16_t offset, int32_t *value) {
		const auto it = g_globals.find(offset);
		*value        = it == g_globals.end() ? 0 : it->second;
		return true;
	};
	return b;
}

MissionBridge g_bridge;

MissionSync Fresh() {
	g_roster        = Roster{};
	g_mirrorCalls   = 0;
	g_mirror        = false;
	g_fails         = 0;
	g_failReason    = 0xFF;
	g_effects       = 0;
	g_ended         = 0;
	g_applied.clear();
	g_teleports.clear();
	g_globals.clear();
	g_life          = 1;
	g_hash          = 0xC0FFEE;
	g_bridge        = StubBridge();
	MissionSync sync;
	sync.Bind(
	    &g_bridge,
	    [](void *, const void *bytes, size_t len, Channel) {
		    const uint8_t *b = static_cast<const uint8_t *>(bytes);
		    g_roster.sent.emplace_back(b, b + len);
	    },
	    nullptr,
	    [](void *, const char *line) { g_roster.feed.emplace_back(line); },
	    [](void *, const char *line) { g_roster.status.emplace_back(line); },
	    [](void *, uint8_t id) -> const char * {
		    return id < MAX_PLAYERS ? g_roster.nicks[id] : nullptr;
	    },
	    [](void *, MissionPresence *out, size_t max) -> size_t {
		    size_t n = 0;
		    for (const MissionPresence &p : g_roster.others)
			    if (n < max)
				    out[n++] = p;
		    return n;
	    },
	    nullptr);
	return sync;
}

template <class T>
const T *LastSent() {
	for (auto it = g_roster.sent.rbegin(); it != g_roster.sent.rend(); ++it)
		if (it->size() == sizeof(T) && (*it)[0] == T::OPCODE)
			return reinterpret_cast<const T *>(it->data());
	return nullptr;
}

template <class T>
size_t SentCount() {
	size_t n = 0;
	for (const auto &p : g_roster.sent)
		if (p.size() == sizeof(T) && p[0] == T::OPCODE)
			++n;
	return n;
}

// The last status line, the one the log gets. The chat gets none of them.
std::string LastLine() { return g_roster.status.empty() ? std::string() : g_roster.status.back(); }

S_MissionState State(uint8_t state, uint8_t owner, uint16_t number, uint8_t participants,
                     uint8_t outcome = MISSION_OUTCOME_NONE, uint32_t campaignLog = 77) {
	S_MissionState s;
	InitHeader(s, 1000);
	// The server's defaults, as every server sends them.
	s.checkpointWaitS = MISSION_CHECKPOINT_WAIT_MS / 1000;
	s.catchUpM        = MISSION_CATCH_UP_M_DEFAULT;
	s.behindM         = MISSION_BEHIND_M_DEFAULT;
	s.behindS         = MISSION_BEHIND_S_DEFAULT;
	s.campaignLog   = campaignLog;
	s.state         = state;
	s.ownerId       = owner;
	s.missionNumber = number;
	s.participants  = participants;
	s.flags         = MISSION_FLAG_FAIL_ON_DEATH;
	s.outcome       = outcome;
	s.marginCm      = 500;
	return s;
}

S_MissionClaim Answer(uint32_t key, uint8_t verdict, uint8_t owner, uint8_t missing = 0) {
	S_MissionClaim a;
	InitHeader(a, 1000);
	a.launchKey   = key;
	a.verdict     = verdict;
	a.ownerId     = owner;
	a.missingMask = missing;
	return a;
}

S_MissionWaiting Waiting(uint8_t owner, uint8_t missing, uint8_t what, uint16_t hint = 19) {
	S_MissionWaiting w;
	InitHeader(w, 1000);
	w.ownerId     = owner;
	w.missingMask = missing;
	w.what        = what;
	w.missionHint = hint;
	return w;
}

const MissionArea kMarker = MissionAreaLocate3D(100.0f, 100.0f, 10.0f, 1.5f, 1.5f, 2.0f);

void TestAnOlderServerLeavesTheGateAlone() {
	std::printf("\nbehind a server that has said nothing about missions\n");
	MissionSync m = Fresh();
	Check(m.AskStartGate(0x10, MISSION_KIND_STORY, 19, kMarker, 0, 1000) && g_roster.sent.empty(),
	      "the gate is the script's alone, and nothing is claimed");
	Check(m.AskCheckpoint(kMarker, 0, 1000), "and a checkpoint is the script's too");
}

void TestAGateIsClaimedUntilGranted() {
	std::printf("\na start gate is claimed until the session grants it\n");
	MissionSync m = Fresh();
	m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 500);
	Check(m.Shared() && !m.Mirroring(), "the server shares missions, and none is running");

	Check(!m.AskStartGate(0x10, MISSION_KIND_STORY, 19, kMarker, 0, 1000),
	      "the first ask is answered false");
	const C_MissionClaim *c = LastSent<C_MissionClaim>();
	Check(c && c->launchKey == 0x10 && c->missionHint == 19 && c->kind == MISSION_KIND_STORY &&
	          c->area.centre.x == 100.0f && c->area.half.x == 1.5f,
	      "and claims the slot with the start's area");
	m.AskStartGate(0x10, MISSION_KIND_STORY, 19, kMarker, 0, 1200);
	Check(SentCount<C_MissionClaim>() == 1, "asked again at once, it does not claim again");
	m.AskStartGate(0x10, MISSION_KIND_STORY, 19, kMarker, 0, 1000 + MISSION_CLAIM_REFRESH_MS);
	Check(SentCount<C_MissionClaim>() == 2, "but it refreshes the claim twice a second");

	m.OnClaim(Answer(0x99, MISSION_CLAIM_GRANTED, 0), 0, 1600);
	Check(!m.AskStartGate(0x10, MISSION_KIND_STORY, 19, kMarker, 0, 1650),
	      "a grant for some other gate does not open this one");
	m.OnClaim(Answer(0x10, MISSION_CLAIM_GRANTED, 0), 0, 1700);
	Check(m.AskStartGate(0x10, MISSION_KIND_STORY, 19, kMarker, 0, 1750), "its own grant does");

	m.Launched(0x10, 19, 1800);
	const C_MissionStarted *s = LastSent<C_MissionStarted>();
	Check(s && s->launchKey == 0x10 && s->missionNumber == 19, "and the launch is reported");
	m.Ended(19, MISSION_OUTCOME_PASSED, 9000);
	const C_MissionEnded *e = LastSent<C_MissionEnded>();
	Check(e && e->missionNumber == 19 && e->outcome == MISSION_OUTCOME_PASSED, "so is the end");
}

void TestAGrantGoesStale() {
	std::printf("\na grant nobody used goes stale\n");
	MissionSync m = Fresh();
	m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 500);
	m.AskStartGate(0x10, MISSION_KIND_STORY, 19, kMarker, 0, 1000);
	m.OnClaim(Answer(0x10, MISSION_CLAIM_GRANTED, 0), 0, 1100);
	m.Tick(0, 1000 + MISSION_CLAIM_TTL_MS);
	Check(!m.AskStartGate(0x10, MISSION_KIND_STORY, 19, kMarker, 0, 1000 + MISSION_CLAIM_TTL_MS + 10),
	      "coming back to the marker later needs a fresh grant");
}

void TestWhatEverybodyIsTold() {
	std::printf("\nwho is told what while a start waits\n");
	{
		MissionSync m = Fresh();   // we are alice, 0, and it is our start
		m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 500);
		m.OnWaiting(Waiting(0, PlayerBit(1) | PlayerBit(2), MISSION_WAIT_START), 0, 1000);
		Check(LastLine() == "waiting for bob and carol to come to the start", "the owner");
		m.OnWaiting(Waiting(0, PlayerBit(1) | PlayerBit(2), MISSION_WAIT_START), 0, 1100);
		Check(g_roster.status.size() == 1, "once for the same wait");
		m.Tick(0, 1000 + MISSION_WAIT_REMIND_MS);
		Check(g_roster.status.size() == 2, "and again every fifteen seconds while it lasts");
		Check(g_roster.feed.empty(), "all of it in the log, and none of it in the chat");
		m.OnWaiting(Waiting(0, PlayerBit(1) | PlayerBit(2) | PlayerBit(3), MISSION_WAIT_START), 0,
		            17000);
		Check(LastLine() == "waiting for bob, carol and dave to come to the start",
		      "three names read as a list");
	}
	{
		MissionSync m = Fresh();   // we are bob, 1, and alice waits for us
		m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
		m.OnWaiting(Waiting(0, PlayerBit(1), MISSION_WAIT_START), 1, 1000);
		Check(LastLine() == "alice is waiting for you at the start of Give Me Liberty",
		      "the one who is missing has it in their log");
	}
	{
		MissionSync m = Fresh();   // we are carol, 2, and already there
		m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 2, 500);
		m.OnWaiting(Waiting(0, PlayerBit(1), MISSION_WAIT_START), 2, 1000);
		Check(LastLine() == "alice is waiting for bob at the start of Give Me Liberty",
		      "and everybody else who is waiting with alice");
		m.OnWaiting(Waiting(0, PlayerBit(1), MISSION_WAIT_CHECKPOINT, 21), 2, 2000);
		Check(LastLine() == "waiting for bob to catch up", "a checkpoint's wait too");
		m.OnWaiting(Waiting(INVALID_PLAYER, 0, MISSION_WAIT_NONE), 2, 3000);
		m.Tick(2, 3000 + MISSION_WAIT_REMIND_MS);
		Check(LastLine() == "waiting for bob to catch up" && m.WaitingFor() == 0,
		      "and nothing more once nobody is waited for");
	}
	{
		MissionSync m = Fresh();
		m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 500);
		m.AskStartGate(0x10, MISSION_KIND_STORY, 19, kMarker, 0, 1000);
		m.OnClaim(Answer(0x10, MISSION_CLAIM_BUSY, 1), 0, 1100);
		m.OnClaim(Answer(0x10, MISSION_CLAIM_BUSY, 1), 0, 1600);
		Check(LastLine() == "bob is starting a mission already - one at a time" &&
		          g_roster.status.size() == 1 && g_roster.feed.empty(),
		      "a busy slot says whose it is, once");
	}
}

void TestAParticipantFollowsTheSessionsMission() {
	std::printf("\na participant's $ONMISSION follows the session's mission\n");
	MissionSync m = Fresh();   // we are bob, 1
	m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	m.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 1, 1000);
	Check(g_mirror && g_mirrorCalls == 1 && m.Mirroring(),
	      "alice's mission starts, and ours is set: no mission, save or rampage of our own");
	Check(LastLine() == "alice started Give Me Liberty", "and the log says whose and which");
	Check(!m.AskStartGate(0x77, MISSION_KIND_ODDJOB, 11, MissionAreaAround({}), 1, 1100),
	      "a gate of our own is refused while it runs");
	m.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 1, 1500);
	Check(g_mirrorCalls == 1, "set once, not every time it is said");
	m.OnState(State(MISSION_STATE_IDLE, 0, 19, 0, MISSION_OUTCOME_PASSED), 1, 9000);
	Check(!g_mirror && g_mirrorCalls == 2, "and it ends: ours is our own again");
	Check(LastLine() == "alice passed Give Me Liberty", "alice passed it");

	m.OnState(State(MISSION_STATE_RUNNING, 0, 20, 0x07), 1, 10000);
	m.OnState(State(MISSION_STATE_IDLE, 0, 20, 0, MISSION_OUTCOME_OWNER_LEFT), 1, 11000);
	Check(LastLine() == "alice left, so Don't Spank Ma Bitch Up failed" && !g_mirror,
	      "alice leaving fails it, and says so");
	m.OnState(State(MISSION_STATE_RUNNING, 0, 11, 0x07), 1, 11500);
	m.OnState(State(MISSION_STATE_IDLE, 0, 11, 0, MISSION_OUTCOME_FAILED), 1, 11800);
	Check(LastLine() == "alice's Paramedic shift is over", "an odd job is a shift that ends");
	m.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 1, 12000);
	m.Clear();
	Check(!g_mirror && !m.Shared(), "and a lost connection gives our $ONMISSION back");
}

// The runs of 2026-09-24: Give Me Liberty's Kuruma on the owner's screen alone,
// and two logs that could not be read side by side.
void TestComingIntoAMissionAsksForWhatItMade() {
	std::printf("\ncoming into somebody's mission asks for what it has made\n");
	MissionSync bob = Fresh();   // bob, 1
	bob.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	Check(SentCount<C_MissionCatchUp>() == 0, "nothing is asked with no mission running");
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x03), 1, 1000);
	const C_MissionCatchUp *ask = LastSent<C_MissionCatchUp>();
	Check(SentCount<C_MissionCatchUp>() == 1 && ask && ask->missionNumber == 19 &&
	          bob.CatchUpsAsked() == 1,
	      "alice's mission starting with bob in it asks for what it has made, once");
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x03), 1, 1500);
	Check(SentCount<C_MissionCatchUp>() == 1, "and not again for the same mission said twice");
	bob.StartedOver(1, 2000);
	Check(SentCount<C_MissionCatchUp>() == 2, "bob's game starting over asks again");

	MissionSync carol = Fresh();   // carol, 2, in her own intro when alice starts
	carol.SetBusy(true, 100);
	carol.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 2, 500);
	carol.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x03), 2, 1000);
	Check(SentCount<C_MissionCatchUp>() == 0 && !g_mirror,
	      "a game in a mission of its own asks for nothing of alice's");
	Check(LastLine() == "alice started Give Me Liberty without you: this game is in a mission "
	                    "of its own, and comes into it once that is over",
	      "and its log says the start went on without it");
	carol.SetBusy(false, 1200);
	carol.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 2, 1300);
	Check(SentCount<C_MissionCatchUp>() == 1 && g_mirror,
	      "out of its intro and into alice's mission, it asks for everything made meanwhile");

	MissionSync alice = Fresh();   // alice, 0, the owner
	g_roster.others = {{1, true, {}}, {2, false, {}}};
	alice.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x03), 0, 1000);
	Check(LastLine() == "you started Give Me Liberty without carol, whose game is still in a "
	                    "mission of its own; they come into it once that is over",
	      "the owner's log names who the start went on without");
	Check(SentCount<C_MissionCatchUp>() == 0, "and the owner asks nobody for its own");

	MissionSync dave = Fresh();   // dave, 3, with a session clock
	dave.BindClock([](void *, uint32_t *ms) {
		*ms = 1642463;
		return true;
	});
	dave.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x0F), 3, 1000);
	Check(LastLine() == "alice is on Give Me Liberty, and you are in it at session time 1642463 ms",
	      "a line about who started what carries the session's clock");
}

void TestTheOwnersSide() {

	std::printf("\nthe owner's side\n");
	MissionSync m = Fresh();   // we are alice, 0
	m.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 0, 1000);
	Check(!m.Mirroring() && g_mirrorCalls == 0, "the owner's own script keeps its $ONMISSION");
	Check(LastLine() == "you started Give Me Liberty, and everybody is in it", "and says so");
	S_MissionFail fail;
	InitHeader(fail, 1000);
	fail.missionNumber = 19;
	fail.reason        = MISSION_FAIL_DIED;
	fail.playerId      = 1;
	m.OnFail(fail, 0);
	Check(g_fails == 1 && g_failReason == MISSION_FAIL_DIED &&
	          LastLine() == "bob died, so Give Me Liberty fails",
	      "the death rule's order fails it through the engine");

	MissionSync other = Fresh();   // we are carol, 2, and not the owner
	other.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 2, 1000);
	other.OnFail(fail, 2);
	Check(g_fails == 0, "nobody else acts on it");

	MissionSync joiner = Fresh();   // we are dave, 3, arriving in the middle of it
	joiner.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x0F), 3, 1000);
	Check(LastLine() == "alice is on Give Me Liberty, and you are in it" && g_mirror,
	      "a joiner is told what they walked into, and is in it");
}

void TestACheckpointWaitsForEverybody() {
	std::printf("\na checkpoint waits for everybody\n");
	MissionSync m = Fresh();   // we are alice, 0, the owner
	m.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 0, 1000);
	const MissionArea cp = MissionAreaLocate2D(500.0f, 500.0f, 4.0f, 4.0f);
	g_roster.others = {{1, true, {502.0f, 500.0f, 0.0f}}, {2, true, {540.0f, 500.0f, 0.0f}}};
	Check(!m.AskCheckpoint(cp, 0, 2000), "carol is 40 m away, so the mission waits");
	const C_MissionCheckpoint *c = LastSent<C_MissionCheckpoint>();
	Check(c && c->missingMask == PlayerBit(2) && c->where.x == 500.0f, "and tells the others for whom");
	m.AskCheckpoint(cp, 0, 2100);
	Check(SentCount<C_MissionCheckpoint>() == 1, "once for the same wait");
	g_roster.others[1].pos = {508.5f, 500.0f, 0.0f};
	Check(m.AskCheckpoint(cp, 0, 2200), "8.5 m out of a 4 m box is within the margin: go on");
	c = LastSent<C_MissionCheckpoint>();
	Check(c && c->missingMask == 0, "and the wait is over for everybody");

	g_roster.others[1].pos = {600.0f, 500.0f, 0.0f};
	m.AskCheckpoint(cp, 0, 3000);
	m.Tick(0, 3000 + MISSION_CHECKPOINT_IDLE_MS);
	c = LastSent<C_MissionCheckpoint>();
	Check(c && c->missingMask == 0, "a checkpoint the owner walked away from waits for nobody");

	g_roster.others = {{1, true, {502.0f, 500.0f, 0.0f}}, {3, true, {900.0f, 0.0f, 0.0f}}};
	Check(m.AskCheckpoint(cp, 0, 5000), "somebody who is not in the mission is not waited for");
	g_roster.others = {{1, false, {}}};
	Check(!m.AskCheckpoint(cp, 0, 6000), "and somebody nothing has placed yet is missing");

	MissionSync other = Fresh();   // we are bob, 1: not the owner
	other.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 1, 1000);
	g_roster.others = {{2, true, {900.0f, 0.0f, 0.0f}}};
	Check(other.AskCheckpoint(cp, 1, 2000) && SentCount<C_MissionCheckpoint>() == 0,
	      "only the owner's checkpoints wait: the script is only the owner's");
}

void TestACheckpointDoesNotWaitForEver() {
	std::printf("\na checkpoint does not wait for ever\n");
	MissionSync m = Fresh();   // we are alice, 0, the owner
	m.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 0, 1000);
	const MissionArea cp = MissionAreaLocate2D(500.0f, 500.0f, 4.0f, 4.0f);
	g_roster.others = {{1, true, {502.0f, 500.0f, 0.0f}}, {2, true, {1540.0f, 500.0f, 0.0f}}};
	uint32_t t = 10000;
	Check(!m.AskCheckpoint(cp, 0, t), "carol is a kilometre away: the mission waits");
	for (uint32_t step = 0; step < MISSION_CHECKPOINT_WAIT_MS - 500; step += 500)
		m.AskCheckpoint(cp, 0, t + step);
	Check(!m.AskCheckpoint(cp, 0, t + MISSION_CHECKPOINT_WAIT_MS - 100) &&
	          m.CheckpointsGivenUp() == 0,
	      "and still waits just short of the time");
	Check(m.AskCheckpoint(cp, 0, t + MISSION_CHECKPOINT_WAIT_MS) && m.CheckpointsGivenUp() == 1,
	      "then goes on without her, with her still missing");
	const C_MissionCheckpoint *c = LastSent<C_MissionCheckpoint>();
	Check(c && c->missingMask == 0, "and tells everybody the wait is over");
	Check(LastLine().find("went on without carol") != std::string::npos, "and the log says who");
	Check(m.AskCheckpoint(cp, 0, t + MISSION_CHECKPOINT_WAIT_MS + 50) && m.CheckpointsGivenUp() == 1,
	      "the same checkpoint asked again goes on, and is not given up twice");

	const MissionArea next = MissionAreaLocate2D(800.0f, 500.0f, 4.0f, 4.0f);
	g_roster.others[0].pos = {802.0f, 500.0f, 0.0f};   // bob keeps up
	t += MISSION_CHECKPOINT_WAIT_MS + 1000;
	Check(!m.AskCheckpoint(next, 0, t), "the next checkpoint waits for her afresh");
	c = LastSent<C_MissionCheckpoint>();
	Check(c && c->missingMask == PlayerBit(2) && c->where.x == 800.0f, "and says so");
	g_roster.others[1].pos = {801.0f, 500.0f, 0.0f};
	Check(m.AskCheckpoint(next, 0, t + 20000) && m.CheckpointsGivenUp() == 1,
	      "and she makes it in time");

	// Walking out and back in starts it again.
	g_roster.others[0].pos = {502.0f, 500.0f, 0.0f};
	g_roster.others[1].pos = {1540.0f, 500.0f, 0.0f};
	t += 60000;
	m.AskCheckpoint(cp, 0, t);
	m.Tick(0, t + MISSION_CHECKPOINT_IDLE_MS);
	Check(!m.AskCheckpoint(cp, 0, t + MISSION_CHECKPOINT_WAIT_MS - 1000),
	      "a checkpoint the owner walked out of and back into counts from the return");
}

void TestWhoIsWaitedForIsOnTheHud() {
	std::printf("\nwho the mission waits for is on the HUD, not in the chat\n");
	char line[96];
	MissionSync owner = Fresh();   // we are alice, 0, the owner
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 0, 1000);
	Check(!owner.WaitLine(line, sizeof line, 0, 1000) && line[0] == '\0', "nothing while nobody is waited for");
	S_MissionWaiting w = Waiting(0, PlayerBit(2), MISSION_WAIT_CHECKPOINT, 21);
	w.goesOnInS        = 42;
	owner.OnWaiting(w, 0, 2000);
	Check(owner.WaitLine(line, sizeof line, 0, 2000) && std::string(line) == "Waiting for carol - 42 s",
	      "the owner reads who is missing and how long is left");
	Check(owner.WaitLine(line, sizeof line, 0, 12500) && std::string(line) == "Waiting for carol - 32 s",
	      "counted down on this machine's own clock");
	Check(g_roster.feed.empty(), "and nothing of it goes in the chat");
	Check(LastLine().find("goes on without them in 42 s") != std::string::npos,
	      "the log line says the same");

	MissionSync late = Fresh();   // we are carol, 2
	late.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 2, 1000);
	late.OnWaiting(w, 2, 2000);
	Check(late.WaitLine(line, sizeof line, 2, 2000) && std::string(line) == "alice is waiting for you - 42 s",
	      "the one missing is told it is them");
	MissionSync bob = Fresh();   // we are bob, 1
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 1, 1000);
	w.missingMask = PlayerBit(1) | PlayerBit(2);
	bob.OnWaiting(w, 1, 2000);
	Check(bob.WaitLine(line, sizeof line, 1, 2000) &&
	          std::string(line) == "alice is waiting for you and carol - 42 s",
	      "and with somebody else too");
	bob.OnWaiting(Waiting(0, 0, MISSION_WAIT_NONE), 1, 3000);
	Check(!bob.WaitLine(line, sizeof line, 1, 3000), "gone once the wait is over");

	MissionSync start = Fresh();   // we are bob, 1, before any mission
	start.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 1000);
	start.OnWaiting(Waiting(0, PlayerBit(1), MISSION_WAIT_START, 19), 1, 2000);
	Check(start.WaitLine(line, sizeof line, 1, 2000) &&
	          std::string(line) == "alice is waiting for you at the start",
	      "a start waiting for us is on the HUD too");
	MissionSync outside = Fresh();   // we are dave, 3, not in alice's mission
	outside.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 3, 1000);
	outside.OnWaiting(Waiting(0, PlayerBit(2), MISSION_WAIT_CHECKPOINT, 21), 3, 2000);
	Check(!outside.WaitLine(line, sizeof line, 3, 2000),
	      "and a checkpoint of a mission we are not in is not ours to see");
}

void TestALateParticipantIsBroughtToTheOwner() {
	std::printf("\nsomebody who comes in late, or back from the hospital, is brought to the owner\n");
	Vec3    at{};
	uint8_t slot = 0, count = 0;
	const Vec3 far{2000.0f, -500.0f, 10.0f};

	MissionSync joiner = Fresh();   // we are dave, 3, joining alice's running mission
	g_roster.others = {{0, true, {100.0f, 100.0f, 10.0f}}, {1, true, {0, 0, 0}}, {2, true, {0, 0, 0}}};
	joiner.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x0F), 3, 1000);
	Check(joiner.SummonPending(), "joining in the middle of it owes a move to alice");
	Check(!joiner.TakeSummon(far, 3, 1000 + MISSION_SUMMON_DELAY_MS - 1, &at, &slot, &count),
	      "not before the first seconds are over");
	Check(joiner.TakeSummon(far, 3, 1000 + MISSION_SUMMON_DELAY_MS, &at, &slot, &count) &&
	          at.x == 100.0f && at.y == 100.0f && slot == 2 && count == 3,
	      "then to where alice is, as the third of the three round her");
	Check(!joiner.SummonPending() && joiner.Summons() == 1, "once");

	MissionSync near = Fresh();   // dave again, but close by
	g_roster.others = {{0, true, {100.0f, 100.0f, 10.0f}}};
	near.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x09), 3, 1000);
	Check(!near.TakeSummon({130.0f, 100.0f, 10.0f}, 3, 9000, &at, &slot, &count) &&
	          !near.SummonPending(),
	      "somebody who comes in near her walks, and is not moved");

	MissionSync unplaced = Fresh();
	g_roster.others = {{0, false, {}}};
	unplaced.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x09), 3, 1000);
	Check(!unplaced.TakeSummon(far, 3, 9000, &at, &slot, &count) && unplaced.SummonPending(),
	      "with nothing saying where she is yet, it waits");

	MissionSync atStart = Fresh();   // bob, at the start with everybody
	g_roster.others = {{0, true, {100.0f, 100.0f, 10.0f}}};
	atStart.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 1000);
	atStart.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x03), 1, 2000);
	Check(!atStart.SummonPending(), "a start everybody was at owes nobody a move");
	atStart.Respawned(1, 5000);
	Check(atStart.SummonPending(), "coming back from the hospital in the middle of it does");
	Check(atStart.TakeSummon(far, 1, 5000 + MISSION_SUMMON_DELAY_MS, &at, &slot, &count) &&
	          slot == 0 && count == 1,
	      "and brings bob to alice");

	MissionSync owner = Fresh();   // alice, whose mission it is
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x03), 0, 1000);
	owner.Respawned(0, 2000);
	Check(!owner.SummonPending(), "the owner is never moved to herself");

	MissionSync ended = Fresh();   // dave, whose summons outlives the mission
	g_roster.others = {{0, true, {100.0f, 100.0f, 10.0f}}};
	ended.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x09), 3, 1000);
	ended.OnState(State(MISSION_STATE_IDLE, 0, 21, 0, MISSION_OUTCOME_PASSED), 3, 2000);
	Check(!ended.SummonPending() && !ended.TakeSummon(far, 3, 9000, &at, &slot, &count),
	      "a mission that ended owes nobody a move");
}

void TestAnOwnerWhoDroppedOffIsTakenUpAgain() {
	std::printf("\nan owner whose connection dropped offers its mission again\n");
	int handedOver = 0;
	static int *s_handed = nullptr;
	s_handed = &handedOver;
	MissionSync m = Fresh();   // we are alice, 0
	g_bridge.ResendStanding = [](uint8_t) { ++*s_handed; };
	m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 1000);
	m.Launched(0x4242, 21, 1100);
	m.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 0, 1200);
	Check(m.OwnLaunchIsTheSessions(0), "alice's mission is the session's");

	m.Clear();
	Check(m.OwnLaunchIsTheSessions(0),
	      "with the connection gone it is still the session's to this game, which does not "
	      "call itself busy with it");
	g_roster.sent.clear();
	// Back, perhaps under another id: the server has failed it meanwhile.
	m.OnState(State(MISSION_STATE_IDLE, 1, 21, 0, MISSION_OUTCOME_OWNER_LEFT), 2, 5000);
	const C_MissionStarted *again = LastSent<C_MissionStarted>();
	Check(again && again->missionNumber == 21 && again->launchKey == 0,
	      "and offered again as soon as the server answers the reconnect");
	Check(m.OwnLaunchIsTheSessions(2), "still the session's while the server has not answered");
	m.OnState(State(MISSION_STATE_RUNNING, 2, 21, 0x07), 2, 5100);
	Check(m.Running() && m.Owner() == 2 && !m.Mirroring(), "taken up again, with us as its owner");
	Check(handedOver == 2 && m.StandingHandedOver() == 2,
	      "and alice's two helpers are handed what it has up, as joiners are");
	Check(LastLine().find("taken up") != std::string::npos, "the log says so");
	m.OnState(State(MISSION_STATE_RUNNING, 2, 21, 0x0F), 2, 5200);
	Check(handedOver == 3, "and a later joiner as before");

	MissionSync late = Fresh();   // alice again, somebody else's mission runs by the time she is back
	late.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 1000);
	late.Launched(0x4242, 21, 1100);
	late.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 0, 1200);
	late.Clear();
	g_roster.sent.clear();
	late.OnState(State(MISSION_STATE_RUNNING, 1, 30, 0x02), 0, 5000);
	Check(!LastSent<C_MissionStarted>() && !late.OwnLaunchIsTheSessions(0),
	      "with bob's mission running by then, alice's stays her game's own");

	MissionSync over = Fresh();   // it ended while the connection was down
	over.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 1000);
	over.Launched(0x4242, 21, 1100);
	over.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 0, 1200);
	over.Clear();
	over.Ended(21, MISSION_OUTCOME_PASSED, 3000);
	g_roster.sent.clear();
	over.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 5000);
	Check(!LastSent<C_MissionStarted>() && !over.OwnLaunchIsTheSessions(0),
	      "one that ended while the connection was down is offered to nobody");

	MissionSync helper = Fresh();   // bob, a participant, drops off
	helper.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 1, 1000);
	helper.Clear();
	g_roster.sent.clear();
	helper.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 5000);
	Check(!LastSent<C_MissionStarted>(), "a participant who drops off has nothing to offer");
	s_handed = nullptr;
}

// ---- the script engine's half, as far as it is pure (game/mission.h) ---------

// A contact's trigger as main.scm has it: `03EE player / 004D goto_if_false
// label`, then what the gate is asked for.
std::vector<uint8_t> Gate(uint16_t then) {
	std::vector<uint8_t> b = {
	    0xEE, 0x03, 0x02, 0x9C, 0x43,                   // 03EE $PLAYER_CHAR
	    0x4D, 0x00, 0x01, 0x10, 0x20, 0x00, 0x00,       // 004D goto_if_false @label
	    static_cast<uint8_t>(then & 0xFF), static_cast<uint8_t>(then >> 8)};
	return b;
}

void TestTheStartGatesAreToldApart() {
	std::printf("\na launch's start gate is told from a save point's\n");
	using game::ClassifyStartGate;
	using game::GateKind;
	for (const uint16_t then : {0x03EF, 0x024E, 0x0004, 0x0417}) {
		const std::vector<uint8_t> b = Gate(then);
		if (ClassifyStartGate(b.data(), static_cast<uint32_t>(b.size() + 4), 2) != GateKind::Launch) {
			Check(false, "a make-safe, a phone, Ray's flag or the launch itself is a launch");
			return;
		}
	}
	Check(true, "a make-safe, a phone, Ray's flag or the launch itself is a launch");
	std::vector<uint8_t> save = Gate(0x00D6);
	Check(ClassifyStartGate(save.data(), static_cast<uint32_t>(save.size()), 2) ==
	          GateKind::NotALaunch,
	      "another `if` after it is a save point");
	std::vector<uint8_t> inAnd = {0xEE, 0x03, 0x02, 0x9C, 0x43, 0x9C, 0x01, 0x02, 0x10};
	Check(ClassifyStartGate(inAnd.data(), static_cast<uint32_t>(inAnd.size()), 2) ==
	          GateKind::Unknown,
	      "one inside an `if and`, as a modified main.scm has it, is neither");
	std::vector<uint8_t> cut = {0xEE, 0x03, 0x02};
	Check(ClassifyStartGate(cut.data(), static_cast<uint32_t>(cut.size()), 2) == GateKind::Unknown,
	      "and one cut short reads nothing past the end");
}

void TestTheLaunchAhead() {
	std::printf("\nwhich mission a start gate launches\n");
	std::vector<uint8_t> b(600, 0x00);
	b[400] = 0x17; b[401] = 0x04; b[402] = 0x04; b[403] = 20;   // 0417 start_mission 20
	Check(game::FindLaunchAhead(b.data(), static_cast<uint32_t>(b.size()), 10) == 20,
	      "the START_MISSION after it, past a payphone's call");
	b[200] = 0x17; b[201] = 0x04; b[202] = 0x04; b[203] = 99;
	Check(game::FindLaunchAhead(b.data(), static_cast<uint32_t>(b.size()), 10) == 20,
	      "a stray pair of bytes naming no mission is passed over");
	b[100] = 0x17; b[101] = 0x04; b[102] = 0x05; b[103] = 45; b[104] = 0x00;
	Check(game::FindLaunchAhead(b.data(), static_cast<uint32_t>(b.size()), 10) == 45,
	      "a 16-bit operand reads too");
	Check(game::FindLaunchAhead(b.data(), static_cast<uint32_t>(b.size()), 10, 50) == MISSION_NONE,
	      "and nothing past the window is anybody's launch");
	std::vector<uint8_t> ops = {0x04, 0x07, 0x05, 0x34, 0x12, 0x01, 1, 2, 3, 4};
	int32_t  v = 0;
	uint32_t n = 0;
	Check(game::ReadIntOperand(ops.data(), 10, 0, &v, &n) && v == 7 && n == 2 &&
	          game::ReadIntOperand(ops.data(), 10, 2, &v, &n) && v == 0x1234 && n == 3 &&
	          game::ReadIntOperand(ops.data(), 10, 5, &v, &n) && v == 0x04030201 && n == 5,
	      "8, 16 and 32-bit operands");
	Check(!game::ReadIntOperand(ops.data(), 9, 5, &v, &n), "and none read past the end");
}

void TestTheConditionsAreas() {
	std::printf("\nthe area a location condition describes\n");
	using namespace game::scripts::op;
	const float locate3d[8] = {0, 892.75f, -425.75f, 13.875f, 1.5f, 2.0f, 2.0f, 0};
	MissionArea a{};
	Check(game::AreaForCondition(0x00F6, locate3d, &a) && a.shape == MISSION_AREA_BOX3D &&
	          a.centre.x == 892.75f && a.half.y == 2.0f,
	      "Don't Spank Ma Bitch Up's marker, LOCATE_PLAYER_ON_FOOT_3D");
	const float corners[8] = {0, 891.1875f, -309.6875f, 7.6875f, 899.25f, -303.25f, 12.6875f, 0};
	Check(game::AreaForCondition(0x019C, corners, &a) && a.shape == MISSION_AREA_BOX3D &&
	          a.centre.x > 895.0f && a.centre.x < 895.3f && a.half.x > 4.0f,
	      "the Portland save's door, IS_PLAYER_IN_AREA_ON_FOOT_3D by its corners");
	const float locate2d[6] = {0, 10.0f, 20.0f, 4.0f, 4.0f, 1};
	Check(game::AreaForCondition(LOCATE_STOPPED_PLAYER_IN_CAR_2D, locate2d, &a) &&
	          a.shape == MISSION_AREA_BOX2D,
	      "a 2D one has no height");
	Check(!game::AreaForCondition(0x00E9, locate2d, &a) && !game::IsLocationCondition(0x00E9),
	      "near a character is not an area of the map");
	Check(game::IsLocationCondition(IS_PLAYER_IN_AREA_2D) && !game::IsLocationCondition(0x0004),
	      "IS_PLAYER_IN_AREA_2D is, a global's assignment is not");

	const game::CoordBlip blips[2] = {{7, 500.0f, 500.5f}, {9, 900.0f, 0.0f}};
	const MissionArea cp = MissionAreaLocate2D(500.0f, 500.0f, 3.0f, 3.0f);
	Check(game::HoldsCoordBlip(cp, blips, 2), "a locate on the blip's own coordinates is a checkpoint");
	const MissionArea elsewhere = MissionAreaLocate2D(600.0f, 500.0f, 3.0f, 3.0f);
	Check(!game::HoldsCoordBlip(elsewhere, blips, 2), "one with no blip in it is not");
	Check(!game::HoldsCoordBlip(cp, blips, 0), "and nothing is, with no blips");

	Check(game::MayForceCondition(0, false) && game::MayForceCondition(3, false),
	      "a single condition and an `if and` may be held");
	Check(!game::MayForceCondition(22, false) && !game::MayForceCondition(0, true),
	      "an `if or` and a NOT may not");
}

void TestTheDeathRuleUnwinds() {
	std::printf("\nthe death rule fails the owner's mission the engine's own way\n");
	using namespace game::scripts::layout;
	uint8_t script[0x88] = {};
	const uint32_t ip = 0x20100, bottom = 0x20020, inner = 0x20480;
	std::memcpy(script + SCRIPT_IP, &ip, 4);
	std::memcpy(script + SCRIPT_STACK, &bottom, 4);
	std::memcpy(script + SCRIPT_STACK + 4, &inner, 4);
	const uint16_t sp = 2;
	std::memcpy(script + SCRIPT_SP, &sp, 2);
	const uint32_t wake = 123456;
	std::memcpy(script + SCRIPT_WAKE_TIME, &wake, 4);
	script[SCRIPT_DEATHARREST_ARMED] = 1;
	int32_t onMission = 1;

	Check(game::UnwindForDeatharrest(script, &onMission), "a mission in its body is failed");
	uint32_t nowIp = 0, nowWake = 1;
	uint16_t nowSp = 9;
	std::memcpy(&nowIp, script + SCRIPT_IP, 4);
	std::memcpy(&nowSp, script + SCRIPT_SP, 2);
	std::memcpy(&nowWake, script + SCRIPT_WAKE_TIME, 4);
	Check(nowIp == bottom && nowSp == 0,
	      "back at the bottom of its gosub stack, where it asks HAS_DEATHARREST_BEEN_EXECUTED");
	Check(script[SCRIPT_DEATHARREST_DONE] == 1 && nowWake == 0 && onMission == 0,
	      "which now says yes, at once, with $ONMISSION off as the engine leaves it");

	uint8_t disarmed[0x88] = {};
	std::memcpy(disarmed + SCRIPT_SP, &sp, 2);
	Check(!game::UnwindForDeatharrest(disarmed, &onMission), "a script with its check off is left alone");
	std::memcpy(disarmed + SCRIPT_STACK, &bottom, 4);
	onMission = 1;
	Check(game::UnwindForDeatharrest(disarmed, &onMission, true) &&
	          disarmed[SCRIPT_DEATHARREST_DONE] == 1 && onMission == 0,
	      "unless told to go anyway: TAXI and HOOD1 turn it off, and go to their cleanup");
	uint32_t disarmedIp = 0;
	std::memcpy(&disarmedIp, disarmed + SCRIPT_IP, 4);
	Check(disarmedIp == bottom, "from the bottom of the gosub stack like any other");
	uint8_t top[0x88] = {};
	top[SCRIPT_DEATHARREST_ARMED] = 1;
	Check(!game::UnwindForDeatharrest(top, &onMission), "and so is one not inside its mission's body");
	Check(!game::UnwindForDeatharrest(top, &onMission, true),
	      "whatever it is told: it is tried again next frame instead");
}

// ---- what the owner's mission shows, as a participant runs it (game/replay.h) ----

void TestAnInstructionIsSentAsValues() {
	std::printf("\nthe owner's instruction, as a participant's engine runs it\n");
	using namespace game::replay;
	std::vector<uint8_t> space(0x400, 0);
	// PRINT_BIG 'LM2' 15000 2, the title, at 0x100 (operands only: the
	// dispatcher has read the opcode already).
	const uint8_t printBig[] = {'L', 'M', '2', 0, 0, 0, 0, 0, 0x05, 0x98, 0x3A, 0x04, 0x02};
	std::memcpy(space.data() + 0x100, printBig, sizeof printBig);
	Encoded e;
	Check(Encode(0x00BA, space.data(), 0x400, 0x100, nullptr, &e) && e.kind == Kind::Plain,
	      "PRINT_BIG is on the list");
	const uint8_t want[] = {0xBA, 0x00, 'L', 'M', '2', 0, 0, 0, 0, 0,
	                        0x01, 0x98, 0x3A, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00};
	Check(e.length == sizeof want && std::memcmp(e.code, want, sizeof want) == 0,
	      "its label goes as its eight bytes, and each number as a 32-bit literal");

	// PRINT_NOW with a global duration and a local style.
	int32_t locals[18] = {};
	locals[3]          = 7;
	const int32_t dur  = 4000;
	std::memcpy(space.data() + 0x40, &dur, 4);
	const uint8_t printNow[] = {'H', 'I', 0, 0, 0, 0, 0, 0, 0x02, 0x40, 0x00, 0x03, 0x03, 0x00};
	std::memcpy(space.data() + 0x120, printNow, sizeof printNow);
	Check(Encode(0x00BC, space.data(), 0x400, 0x120, locals, &e) &&
	          std::memcmp(e.code + 11, &dur, 4) == 0 && e.code[16] == 7,
	      "a global and a local are sent as the values they held");
	Check(!Encode(0x00BC, space.data(), 0x400, 0x120, nullptr, &e),
	      "and a local nobody can read is refused, not guessed");

	// ADD_BLIP_FOR_COORD 892.75 -425.75 13.875 -> $BLIP, float literals.
	const uint8_t blip[] = {0x06, 0xCC, 0x37, 0x06, 0x64, 0xE5, 0x06, 0xDE, 0x00, 0x02, 0x80, 0x00};
	std::memcpy(space.data() + 0x140, blip, sizeof blip);
	Check(Encode(0x018A, space.data(), 0x400, 0x140, nullptr, &e) && e.kind == Kind::BlipNew,
	      "a new blip");
	float x = 0.0f;
	std::memcpy(&x, e.code + 3, 4);
	Check(x == 892.75f && e.code[17] == 0x03 && e.code[18] == 0 && e.code[19] == 0 && e.length == 20,
	      "its coordinates as the floats the owner's engine read, stored in the runner's local 0");

	// REMOVE_BLIP $BLIP, with the owner's handle 1234 in it.
	const int32_t ownerHandle = 1234;
	std::memcpy(space.data() + 0x80, &ownerHandle, 4);
	const uint8_t remove[] = {0x02, 0x80, 0x00};
	std::memcpy(space.data() + 0x160, remove, sizeof remove);
	Check(Encode(0x0164, space.data(), 0x400, 0x160, nullptr, &e) && e.kind == Kind::BlipUse &&
	          e.ownerBlip == 1234 && e.handleAt == 3,
	      "a blip removed names the owner's handle, and where it sits in the code");

	BlipMap blips, objects;
	Handles maps;
	maps.blips   = &blips;
	maps.objects = &objects;
	Encoded run;
	Check(!Translate(e, maps, &run), "a blip this machine never made is dropped");
	blips.Add(1234, 77);
	int32_t ours = 0;
	Check(Translate(e, maps, &run) && (std::memcpy(&ours, run.code + 3, 4), ours == 77),
	      "and one it did is its own");
	blips.Remove(1234);
	Check(blips.Ours(1234) == -1 && blips.Count() == 0, "until it is removed");

	// $JOEY_MISSION_MARKER = ADD_SPRITE_BLIP_FOR_CONTACT_POINT 1191.6875 -870.0
	// -100.0 sprite 15 (21_luigi3): a contact's marker, which the main script
	// and later missions take off again by its global.
	const uint8_t contact[] = {0x06, 0x7B, 0x4A, 0x06, 0xA0, 0xC9, 0x06, 0xC0, 0xF9,
	                           0x04, 0x0F, 0x02, 0x84, 0x00};
	std::memcpy(space.data() + 0x300, contact, sizeof contact);
	uint16_t markerAt = 0;
	bool     makes    = false;
	int32_t  lit      = 0;
	Check(Encode(0x02A7, space.data(), 0x400, 0x300, nullptr, &e) && e.kind == Kind::World &&
	          e.length == 2 + 4 * 5 + 3 && e.code[22] == 0x02 && e.code[23] == 0x84 && e.code[24] == 0 &&
	          BlipGlobalOf(e.code, e.length, &markerAt, &makes) && markerAt == 0x84 && makes,
	      "a contact's marker is made into the same global on every machine, and is in the campaign");
	Check(Translate(e, maps, &run) && std::memcmp(run.code, e.code, e.length) == 0 &&
	          blips.Count() == 0,
	      "with no handle of the owner's to translate, and nothing for the blip map");
	uint8_t intoLocal[sizeof contact];
	std::memcpy(intoLocal, contact, sizeof contact);
	intoLocal[sizeof contact - 3] = 0x03;   // local 0x84, which nobody else has
	std::memcpy(space.data() + 0x320, intoLocal, sizeof intoLocal);
	Check(!Encode(0x02A7, space.data(), 0x400, 0x320, locals, &e),
	      "and one made anywhere but a global is not sent");
	// REMOVE_BLIP $JOEY_MISSION_MARKER (32_toni3), a blip the mission never
	// made: by the global, for every machine to take off its own.
	Check(Encode(0x0164, space.data(), 0x400, 0x160, nullptr, &e, true) && e.kind == Kind::World &&
	          e.length == 5 && e.code[2] == 0x02 && e.code[3] == 0x80 && e.code[4] == 0 &&
	          e.handleAt == 0xFF && e.ownerBlip == -1,
	      "a blip the main script keeps in a global is taken off by the global");
	Check(BlipGlobalOf(e.code, e.length, &markerAt, &makes) && markerAt == 0x80 && !makes &&
	          !LiteralAt(e.code, e.length, 0, &lit) && Translate(e, maps, &run) && run.length == 5,
	      "which no blip map is asked about, so no participant drops it");
	const uint8_t display[] = {0x02, 0x80, 0x00, 0x04, 0x02};
	std::memcpy(space.data() + 0x310, display, sizeof display);
	Check(Encode(0x018B, space.data(), 0x400, 0x310, nullptr, &e, true) && e.length == 2 + 3 + 5 &&
	          LiteralAt(e.code, e.length, 1, &lit) && lit == 2 && SetLiteralAt(e.code, e.length, 1, 3) &&
	          LiteralAt(e.code, e.length, 1, &lit) && lit == 3,
	      "and what follows the global is still found by its place");
	const uint8_t literalBlip[] = {0x01, 0xD2, 0x04, 0x00, 0x00};
	std::memcpy(space.data() + 0x318, literalBlip, sizeof literalBlip);
	Check(Encode(0x0164, space.data(), 0x400, 0x318, nullptr, &e, true) && e.kind == Kind::BlipUse &&
	          e.ownerBlip == 1234 && !BlipGlobalOf(e.code, e.length, &markerAt, &makes),
	      "while a blip named any other way is still the owner's handle");

	// $AMMUNATION_UZI_PICKUP = CREATE_PICKUP #UZI PICKUP_IN_SHOP 1070.5 -400.75
	// 15.1875 (27_joey4): a shop's gun, which the world keeps.
	const uint8_t shop[] = {0x05, 0xAA, 0x00, 0x04, 0x01, 0x06, 0xE8, 0x42, 0x06,
	                        0xF4, 0xE6, 0x06, 0xF3, 0x00, 0x02, 0xB0, 0x00};
	std::memcpy(space.data() + 0x380, shop, sizeof shop);
	uint16_t pickupAt = 0;
	Check(Encode(0x0213, space.data(), 0x400, 0x380, nullptr, &e) && e.kind == Kind::PickupNew &&
	          KeepsPickup(e.code, e.length) && !PickupGlobalOf(e.code, e.length, &pickupAt, &makes),
	      "a shop's gun is one the world keeps, and read plainly it goes to the runner's local");
	Check(Encode(0x0213, space.data(), 0x400, 0x380, nullptr, &e, false, true) &&
	          e.kind == Kind::World && e.length == 2 + 5 * 5 + 3 && e.code[27] == 0x02 &&
	          e.code[28] == 0xB0 && e.code[29] == 0 &&
	          PickupGlobalOf(e.code, e.length, &pickupAt, &makes) && pickupAt == 0xB0 && makes &&
	          KeepsPickup(e.code, e.length),
	      "and by its global it is made into the same global everywhere, in the campaign");
	BlipMap shopPickups;
	maps.pickups = &shopPickups;
	Check(Translate(e, maps, &run) && std::memcmp(run.code, e.code, e.length) == 0 &&
	          shopPickups.Count() == 0,
	      "with nothing of the owner's to translate and nothing for the pickup map");
	uint8_t once[sizeof shop];
	std::memcpy(once, shop, sizeof shop);
	once[4] = 0x03;   // PICKUP_ONCE
	std::memcpy(space.data() + 0x3A0, once, sizeof once);
	Check(Encode(0x0213, space.data(), 0x400, 0x3A0, nullptr, &e) && !KeepsPickup(e.code, e.length),
	      "while a pickup laid out to be taken once is the mission's stash");
	// REMOVE_PICKUP $SHOP_UZI (27_joey4), the main script's out-of-stock sign.
	const int32_t sign = 0x00030005;
	std::memcpy(space.data() + 0xB0, &sign, 4);
	const uint8_t removeSign[] = {0x02, 0xB0, 0x00};
	std::memcpy(space.data() + 0x3C0, removeSign, sizeof removeSign);
	Check(Encode(0x0215, space.data(), 0x400, 0x3C0, nullptr, &e) && e.kind == Kind::Plain &&
	          LiteralAt(e.code, e.length, 0, &lit) && lit == sign && !Translate(e, maps, &run),
	      "a pickup taken away by its handle is dropped where the pickup map has none");
	Check(Encode(0x0215, space.data(), 0x400, 0x3C0, nullptr, &e, false, true) &&
	          e.kind == Kind::World && e.length == 5 && e.code[2] == 0x02 && e.code[3] == 0xB0 &&
	          PickupGlobalOf(e.code, e.length, &pickupAt, &makes) && pickupAt == 0xB0 && !makes &&
	          Translate(e, maps, &run) && run.length == 5,
	      "and by its global every machine takes its own away");

	// DISPLAY_ONSCREEN_TIMER $TIMER keeps the global's offset.
	const uint8_t timer[] = {0x02, 0x44, 0x01};
	std::memcpy(space.data() + 0x180, timer, sizeof timer);
	Check(Encode(0x014E, space.data(), 0x400, 0x180, nullptr, &e) && e.length == 5 &&
	          e.code[2] == 0x02 && e.code[3] == 0x44 && e.code[4] == 0x01,
	      "the on-screen timer names its global by offset, the same on every machine");
	const uint8_t notGlobal[] = {0x04, 0x05};
	std::memcpy(space.data() + 0x190, notGlobal, sizeof notGlobal);
	Check(!Encode(0x014E, space.data(), 0x400, 0x190, nullptr, &e), "and nothing else will do there");

	Check(!Encode(0x0004, space.data(), 0x400, 0x100, nullptr, &e) && !Listed(0x0004),
	      "an assignment is not something anybody sees");
	Check(!Encode(0x00BA, space.data(), 0x104, 0x100, nullptr, &e), "and nothing is read past the end");
	Check(Listed(0x0109) && Find(0x0109)->kind == Kind::Pay, "the pay is on the list, as pay");
	bool world = true;
	for (uint16_t op : {0x014C, 0x0363, 0x03B6, 0x01E7, 0x01E8, 0x022A, 0x022B, 0x02FA, 0x0299})
		world = world && Listed(op) && Find(op)->kind == Kind::World;
	Check(world && Listed(0x021B) && Find(0x021B)->kind == Kind::Plain,
	      "what a save keeps of the world is on the list, and a garage told to take a car is "
	      "shown but not kept");
	// SWAP_NEAREST_BUILDING_MODEL at 525.3125 -927.0625 71.8125, radius 20, a
	// model main.scm names by its own table (negative) to another.
	const uint8_t swapIn[] = {0xB6, 0x03, 0x06, 0xD5, 0x20, 0x06, 0x0F, 0xC6, 0x06, 0x7D, 0x04,
	                          0x06, 0x40, 0x01, 0x04, 0xF6, 0x04, 0xF5};
	std::memcpy(space.data() + 0x1A0, swapIn, sizeof swapIn);
	Check(Encode(0x03B6, space.data(), 0x400, 0x1A2, nullptr, &e) && e.kind == Kind::World &&
	          e.length == 2 + 6 * 5 && e.code[2] == 0x01 && e.code[27] == 0x01,
	      "a building swap goes as six values");
	int32_t model = 0;
	float   wx    = 0.0f;
	std::memcpy(&model, e.code + 28, 4);
	std::memcpy(&wx, e.code + 3, 4);
	Check(model == -11 && wx == 525.3125f,
	      "where it is goes as the float the engine read, and the model it swaps to still names "
	      "main.scm's own table");

	// A cutscene: $CUTSCENE_PLAYER = CREATE_CUTSCENE_OBJECT #NULL, then its
	// animation, then a head on it, all in the owner's object handles.
	const int32_t body = 0x2A07;   // what the owner's pool handed back
	std::memcpy(space.data() + 0x90, &body, 4);
	const uint8_t create[] = {0x04, 0x00, 0x02, 0x90, 0x00};
	std::memcpy(space.data() + 0x200, create, sizeof create);
	Check(Encode(0x02E5, space.data(), 0x400, 0x200, nullptr, &e) && e.kind == Kind::ObjectNew &&
	          e.length == 2 + 5 + 3 && e.code[7] == 0x03 && e.code[8] == 0 && e.code[9] == 0,
	      "a cutscene object is made into the runner's local 0, like a blip");
	const uint8_t anim[] = {0x02, 0x90, 0x00, 'P', 'L', 'A', 'Y', 'E', 'R', 0, 0};
	std::memcpy(space.data() + 0x210, anim, sizeof anim);
	Check(Encode(0x02E6, space.data(), 0x400, 0x210, nullptr, &e) && e.length == 2 + 5 + 8,
	      "its animation names it by the owner's handle, then the animation's name");
	Check(!Translate(e, maps, &run), "which is dropped for an object we never made");
	objects.Add(body, 0x0C01);
	Check(Translate(e, maps, &run) && (std::memcpy(&ours, run.code + 3, 4), ours == 0x0C01) &&
	          std::memcmp(run.code + 7, "PLAYER", 6) == 0,
	      "and names ours for one we did, the name untouched");
	const uint8_t head[] = {0x02, 0x90, 0x00, 0x05, 0x98, 0x00, 0x02, 0x94, 0x00};
	std::memcpy(space.data() + 0x220, head, sizeof head);
	Check(Encode(0x02F4, space.data(), 0x400, 0x220, nullptr, &e) && e.kind == Kind::ObjectNew &&
	          Translate(e, maps, &run) &&
	          (std::memcpy(&ours, run.code + 3, 4), ours == 0x0C01) && run.code[12] == 0x03,
	      "a head goes on our body, and is made into local 0 in its turn");

	// DO_FADE 1500 1, read back and rewritten by its place in the list.
	const uint8_t fade[] = {0x05, 0xDC, 0x05, 0x04, 0x01};
	std::memcpy(space.data() + 0x230, fade, sizeof fade);
	int32_t v = 0;
	Check(Encode(0x016A, space.data(), 0x400, 0x230, nullptr, &e) &&
	          LiteralAt(e.code, e.length, 0, &v) && v == 1500 && LiteralAt(e.code, e.length, 1, &v) &&
	          v == 1 && !LiteralAt(e.code, e.length, 2, &v),
	      "a literal is found by its place in the instruction");
	Check(SetLiteralAt(e.code, e.length, 1, 0) && LiteralAt(e.code, e.length, 1, &v) && v == 0 &&
	          !LiteralAt(e.code, 5, 1, &v),
	      "and can be rewritten there, and nothing is read past the end");
	// ADD_BLIP_FOR_CHAR $CRIMINAL -> $BLIP: the owner's pedestrian goes as the
	// session's netId for it, and comes back as the participant's replica.
	const int32_t criminal = 0x1B03;
	std::memcpy(space.data() + 0x9C, &criminal, 4);
	const uint8_t charBlip[] = {0x02, 0x9C, 0x00, 0x02, 0xA0, 0x00};
	std::memcpy(space.data() + 0x240, charBlip, sizeof charBlip);
	Check(Encode(0x0187, space.data(), 0x400, 0x240, nullptr, &e) && e.kind == Kind::BlipNew,
	      "a blip for a pedestrian is on the list");
	Encoded wire = e;
	Check(!ToWire(&wire, [](int32_t) { return -1; }, nullptr),
	      "and is not sent for one the session has not named yet");
	wire = e;
	Check(ToWire(&wire, [](int32_t h) { return h == 0x1B03 ? 42 : -1; }, nullptr) &&
	          LiteralAt(wire.code, wire.length, 0, &v) && v == 42,
	      "the owner's handle goes out as the pedestrian's netId");
	Handles none;
	Check(!Translate(wire, none, &run), "a participant with no replica of it drops it");
	Handles replicas;
	replicas.charOf = [](int32_t netId) { return netId == 42 ? 0x0907 : -1; };
	Check(Translate(wire, replicas, &run) && LiteralAt(run.code, run.length, 0, &v) && v == 0x0907,
	      "and one with a replica puts the blip on it");
	// CAMERA_ON_VEHICLE $RC_VAN, a car the same way.
	const int32_t van = 0x3A01;
	std::memcpy(space.data() + 0xA4, &van, 4);
	const uint8_t camera[] = {0x02, 0xA4, 0x00, 0x04, 0x0F, 0x04, 0x02};
	std::memcpy(space.data() + 0x250, camera, sizeof camera);
	Check(Encode(0x0158, space.data(), 0x400, 0x250, nullptr, &e) &&
	          ToWire(&e, nullptr, [](int32_t h) { return h == 0x3A01 ? 7 : -1; }) &&
	          LiteralAt(e.code, e.length, 0, &v) && v == 7 && LiteralAt(e.code, e.length, 1, &v) &&
	          v == 15,
	      "a camera on a car names the car by its netId and keeps its mode");
	replicas.carOf = [](int32_t netId) { return netId == 7 ? 0x1101 : -1; };
	Check(Translate(e, replicas, &run) && LiteralAt(run.code, run.length, 0, &v) && v == 0x1101,
	      "and follows the participant's copy of it");
	Check(Listed(0x018C) && Listed(0x01F9) && Find(0x01F9)->count == 9,
	      "a sound at a place and a frenzy are on the list");
	// The owner's clock and sky reach everybody as the world packet, from the
	// owner (coopiii/sky.h). Run here as well, the two would fight.
	Check(!Listed(0x00C0) && !Listed(0x01B5) && !Listed(0x01B6) && !Listed(0x01B7),
	      "SET_TIME_OF_DAY and the three weather instructions are never replayed");
	Check(Listed(0x02A2) && Find(0x02A2)->kind == Kind::Plain && Find(0x02A2)->count == 5,
	      "the smoke and flames a mission sets burning are, all five operands");

	// $BOUY_1_AS3 = CREATE_OBJECT #BOUY at -825.0 -1360.0 2.0, then SLIDE_OBJECT
	// $JOEY_DOOR1, a main-script door every machine made for itself.
	const uint8_t buoy[] = {0x04, 0xF3, 0x06, 0xF0, 0xCC, 0x06, 0x00, 0xAB, 0x04, 0x02,
	                        0x02, 0xB0, 0x00};
	std::memcpy(space.data() + 0x2A0, buoy, sizeof buoy);
	uint16_t g = 0;
	Check(Encode(0x0107, space.data(), 0x400, 0x2A0, nullptr, &e) && e.kind == Kind::Plain &&
	          OutGlobalOf(e.code, e.length, &g) && g == 0xB0 && e.code[e.length - 3] == 0x02,
	      "an object the mission makes is made into the same global on every machine");
	const uint8_t slide[] = {0x02, 0xB4, 0x00, 0x05, 0x00, 0x10, 0x05, 0x00, 0x20, 0x05, 0x00, 0x04,
	                         0x06, 0x01, 0x00, 0x06, 0x01, 0x00, 0x06, 0x40, 0x06, 0x04, 0x00};
	std::memcpy(space.data() + 0x2C0, slide, sizeof slide);
	uint16_t objs[4] = {};
	Check(Encode(0x034E, space.data(), 0x400, 0x2C0, nullptr, &e) &&
	          ObjectGlobals(e.code, e.length, objs, 4) == 1 && objs[0] == 0xB4 &&
	          Translate(e, none, &run),
	      "and a door the mission slides is named by its global, which each machine reads for "
	      "its own door");
	const uint8_t notGlobalDoor[] = {0x03, 0x02, 0x00};
	std::memcpy(space.data() + 0x2E0, notGlobalDoor, sizeof notGlobalDoor);
	Check(!Encode(0x0108, space.data(), 0x400, 0x2E0, locals, &e),
	      "an object held in a local, which nobody else has, is not sent");

	// $FIRE_1 = START_SCRIPT_FIRE 377.0 -444.0 28.0625, and its removal.
	const uint8_t fire[] = {0x06, 0x90, 0x17, 0x06, 0xC0, 0xE4, 0x06, 0xC1, 0x01, 0x02, 0xB8, 0x00};
	std::memcpy(space.data() + 0x2F0, fire, sizeof fire);
	Check(Encode(0x02CF, space.data(), 0x400, 0x2F0, nullptr, &e) && e.kind == Kind::FireNew &&
	          e.code[e.length - 3] == 0x03,
	      "a script fire the mission lights is lit here too, into local 0");
	const int32_t fireHandle = 0x00030001;
	std::memcpy(space.data() + 0xB8, &fireHandle, 4);
	const uint8_t douse[] = {0x02, 0xB8, 0x00};
	std::memcpy(space.data() + 0x300, douse, sizeof douse);
	BlipMap fires;
	Handles withFires;
	withFires.fires = &fires;
	fires.Add(fireHandle, 0x00010004);
	Check(Encode(0x02D1, space.data(), 0x400, 0x300, nullptr, &e) && Translate(e, withFires, &run) &&
	          LiteralAt(run.code, run.length, 0, &v) && v == 0x00010004 && Listed(0x020C),
	      "and put out as ours, and an explosion goes off at its place");

	// $BAT_LM2 = CREATE_PICKUP #BAT PICKUP_ONCE at 917.1875 -425.25 14.5, and
	// the REMOVE_PICKUP that names it.
	const uint8_t bat[] = {0x04, 0xAC, 0x04, 0x03, 0x06, 0x53, 0x39, 0x06, 0x6C, 0xE5,
	                       0x06, 0xE8, 0x00, 0x02, 0xA8, 0x00};
	std::memcpy(space.data() + 0x260, bat, sizeof bat);
	Check(Encode(0x0213, space.data(), 0x400, 0x260, nullptr, &e) && e.kind == Kind::PickupNew &&
	          e.length == 2 + 5 * 5 + 3,
	      "a pickup the mission lays out is made here too, into local 0");
	const int32_t batHandle = 0x00050002;
	std::memcpy(space.data() + 0xA8, &batHandle, 4);
	const uint8_t removeBat[] = {0x02, 0xA8, 0x00};
	std::memcpy(space.data() + 0x280, removeBat, sizeof removeBat);
	BlipMap pickups;
	Handles withPickups;
	withPickups.pickups = &pickups;
	Check(Encode(0x0215, space.data(), 0x400, 0x280, nullptr, &e) &&
	          !Translate(e, withPickups, &run),
	      "and its removal is dropped for a pickup this machine never made");
	pickups.Add(batHandle, 0x00020007);
	Check(Translate(e, withPickups, &run) && LiteralAt(run.code, run.length, 0, &v) &&
	          v == 0x00020007,
	      "and removes ours for one it did");

	bool story = Listed(0x012A) && Find(0x012A)->kind == Kind::Teleport;
	for (uint16_t op : {0x01B1, 0x017A, 0x01B8, 0x03B8, 0x010D, 0x010E, 0x0110, 0x0222, 0x0336,
	                    0x03BF, 0x01F7, 0x016E, 0x01F6})
		story = story && Listed(op) && Find(op)->kind == Kind::Plain;
	Check(story, "what the story does to the player is on the list, for everybody's player");
	Check(Listed(0x0330) && Find(0x0330)->kind == Kind::World,
	      "and the paramedic's reward, never getting tired, is kept like the campaign");
	Check(Listed(0x0413) && Find(0x0413)->kind == Kind::World && Find(0x0413)->count == 2,
	      "and the vigilante's, getting out of jail free, the same way");
	bool counted = true;
	for (uint16_t op : {0x030C, 0x0318, 0x034A, 0x034B, 0x034C})
		counted = counted && Listed(op) && Find(op)->kind == Kind::Campaign;
	Check(counted && Find(0x0318)->args[0] == Arg::Text && Find(0x034A)->count == 0,
	      "the progress, the pass and the islands done go by the campaign delta alone");
	{
		std::vector<uint8_t> passed(0x400, 0);
		std::memcpy(passed.data() + 0x100, "LM1     ", 8);
		Check(Encode(0x0318, passed.data(), 0x400, 0x100, nullptr, &e) && e.kind == Kind::Campaign &&
		          e.length == 10 && std::memcmp(e.code + 2, "LM1", 3) == 0,
		      "and REGISTER_MISSION_PASSED is encoded for the delta with its label");
	}
	bool timed = true;
	for (uint16_t op : {0x03EF, 0x0335, 0x03C7, 0x041F, 0x0420, 0x03B7, 0x03F4, 0x03EA, 0x041D, 0x043C})
		timed = timed && Listed(op) && Find(op)->kind == Kind::Plain && Find(op)->count == 1;
	Check(timed, "what a mission sets for a while, and the launch's make-safe, go to everybody");
	bool oneShot = true;
	for (uint16_t op : {0x0217, 0x0218, 0x02FE, 0x024C, 0x042B, 0x0003, 0x039D, 0x0437, 0x043F, 0x0440})
		oneShot = oneShot && Listed(op);
	Check(oneShot && Find(0x039D)->count == 12 && Find(0x0437)->count == 8 && Find(0x02FE)->count == 5 &&
	          Find(0x024C)->args[1] == Arg::Text,
	      "the queued words, the phone's, the peds cleared, the shaking, the particles and the tune too");
	{
		// ADD_CONTINUOUS_SOUND 790.5 -935.625 38.0 sound 3 into $0x0123.
		std::vector<uint8_t> snd(0x400, 0);
		uint8_t *p = snd.data() + 0x100;
		for (int i = 0; i < 4; ++i) {
			*p++ = 1;
			const int32_t v = i + 1;
			std::memcpy(p, &v, 4);
			p += 4;
		}
		*p++ = 2;
		*p++ = 0x23;
		*p++ = 0x01;
		uint16_t g = 0;
		bool     makes = false;
		Check(Encode(0x018D, snd.data(), 0x400, 0x100, nullptr, &e) && e.length == 25 &&
		          e.code[22] == 2 && SoundGlobalOf(e.code, e.length, &g, &makes) && g == 0x0123 &&
		          makes,
		      "a continuous sound goes into the global that holds it, each machine's own");
		snd[0x200] = 2;
		snd[0x201] = 0x23;
		snd[0x202] = 0x01;
		Check(Encode(0x018E, snd.data(), 0x400, 0x200, nullptr, &e) && e.length == 5 &&
		          SoundGlobalOf(e.code, e.length, &g, &makes) && g == 0x0123 && !makes,
		      "and is taken off by it");
		snd[0x200] = 1;
		Check(!Encode(0x018E, snd.data(), 0x400, 0x200, nullptr, &e),
		      "and a sound named by anything but a global is not sent");
	}
	bool stats = true;
	for (uint16_t op : {0x0315, 0x0316, 0x03FD, 0x03FE, 0x03FF, 0x0400, 0x0401, 0x0402, 0x0403, 0x0404})
		stats = stats && Listed(op) && Find(op)->kind == Kind::Plain;
	Check(stats && Find(0x0316)->count == 1 && Find(0x0403)->count == 1 && Find(0x0401)->count == 0,
	      "the odd jobs' stats count on every machine, as the mission's do");

	Check(Listed(0x0055) && Find(0x0055)->kind == Kind::Teleport && Listed(0x0171) &&
	          Listed(0x02E7) && Listed(0x02EA) && Listed(0x01B4) && Listed(0x038B),
	      "the player's moves, the cutscene and the screen around it are on the list");
}

// Mission `number`, passed by `owner`, leaving `offset` = `value` (and a
// second pair, when given) behind.
S_CampaignDelta Delta(uint32_t seq, uint8_t owner, uint16_t number, uint16_t offset,
                      int32_t value, uint16_t offset2 = 0, int32_t value2 = 0) {
	S_CampaignDelta d;
	InitHeader(d, 1000);
	d.ownerId              = owner;
	d.body.seq             = seq;
	d.body.missionNumber   = number;
	d.body.last            = 1;
	d.body.scriptHash      = 0xC0FFEE;
	d.body.values[0]       = {offset, 0, value};
	d.body.valueCount      = 1;
	if (offset2 != 0) {
		d.body.values[1]   = {offset2, 0, value2};
		d.body.valueCount  = 2;
	}
	return d;
}

bool Applied(std::initializer_list<uint32_t> seqs) {
	return g_applied == std::vector<uint32_t>(seqs);
}

void TestWhatAMissionLeavesBehind() {
	std::printf("\nwhat a mission leaves behind in the campaign\n");
	MissionSync m = Fresh();   // we are bob, 1
	m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	const C_CampaignSince *since = LastSent<C_CampaignSince>();
	Check(since && since->seq == 0, "on the way in we ask for everything since nothing");

	m.OnCampaignDelta(Delta(1, 0, 19, 100, 1));
	m.OnCampaignDelta(Delta(2, 0, 20, 104, 1, 108, 5));
	Check(g_applied.empty(), "nothing is applied the moment it arrives");
	m.Tick(1, 600);
	Check(Applied({1, 2}) && m.CampaignSeq() == 2 && g_globals[108] == 5,
	      "alice's two missions are in this game on the next frame, in the server's order");
	m.OnCampaignDelta(Delta(2, 0, 20, 104, 1, 108, 5));
	m.OnCampaignDelta(Delta(1, 0, 19, 100, 1));
	m.Tick(1, 700);
	Check(Applied({1, 2}), "and each only once, however often it comes");

	m.OnCampaignDelta(Delta(4, 0, 25, 116, 1, 108, 7));
	m.Tick(1, 800);
	Check(Applied({1, 2}) && m.CampaignSeq() == 2,
	      "one that comes ahead of a gap waits for it");
	m.OnCampaignDelta(Delta(3, 2, 24, 112, 1));
	m.Tick(1, 900);
	Check(Applied({1, 2, 3, 4}) && m.CampaignSeq() == 4 && g_globals[108] == 7,
	      "and goes in right after the one it was waiting for");

	g_globals[120] = 1;   // our own mission, 26, ran here
	m.OnCampaignDelta(Delta(5, 1, 26, 120, 1));
	m.Tick(1, 1000);
	Check(Applied({1, 2, 3, 4}) && m.CampaignSeq() == 5 && m.CampaignSettled() == 5,
	      "our own is found here already, and not applied again");

	S_CampaignDelta bad = Delta(6, 0, 27, 124, 1);
	bad.body.valueCount = CAMPAIGN_VALUES + 1;
	m.OnCampaignDelta(bad);
	Check(m.CampaignSeq() == 5, "one that does not fit its packet is refused");

	// A load of a save made after 19 and 20: everything after them comes back.
	g_globals = {{100, 1}, {104, 1}, {108, 5}};
	g_applied.clear();
	g_life = 2;
	m.Tick(1, 1100);
	Check(Applied({3, 4, 5}) && g_globals[108] == 7 && g_globals[120] == 1,
	      "after a load, what the save lacks is applied, our own mission included");
	g_applied.clear();
	g_life = 3;   // a load of a save that has all of it
	m.Tick(1, 1200);
	Check(g_applied.empty(), "and a save that has all of it gets nothing");
	g_life = 0;   // the menu of a game that is not running a script yet
	g_globals.clear();
	m.Tick(1, 1300);
	Check(g_applied.empty(), "nothing is written while no main.scm runs");
	g_life = 4;   // a new game
	m.Tick(1, 1400);
	Check(Applied({1, 2, 3, 4, 5}) && g_globals[108] == 7,
	      "and a new game gets the whole session's campaign");

	m.Clear();
	m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 60000);
	since = LastSent<C_CampaignSince>();
	Check(since && since->seq == 5, "back after a dropped connection, we ask for what came after 5");
	m.Clear();
	m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0, MISSION_OUTCOME_NONE, 78),
	          1, 70000);
	since = LastSent<C_CampaignSince>();
	Check(since && since->seq == 0 && m.CampaignSeq() == 0,
	      "a server that has started over is asked for its log from the start");

	MissionSync other = Fresh();
	other.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	S_CampaignDelta foreign = Delta(1, 0, 19, 100, 1);
	foreign.body.scriptHash = 0xBAD;
	other.OnCampaignDelta(foreign);
	other.OnCampaignDelta(Delta(2, 0, 20, 104, 1));
	g_roster.feed.clear();
	other.Tick(1, 600);
	bool said = false;
	for (const std::string &line : g_roster.feed)
		said = said || line.find("main.scm") != std::string::npos;
	Check(Applied({2}) && said, "a delta from another main.scm is not applied, and the feed says why");

	// A mission that swapped a building: its instruction first, then its globals.
	MissionSync world = Fresh();
	world.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	const uint8_t swap[] = {0xB6, 0x03, 0x01, 0, 0, 0, 0};
	S_CampaignDelta op;
	InitHeader(op, 1000);
	op.ownerId   = 0;
	op.body      = game::CampaignOpPart(44, swap, sizeof swap, 0xC0FFEE);
	op.body.seq  = 1;
	world.OnCampaignDelta(op);
	world.OnCampaignDelta(Delta(2, 0, 44, 200, 1));
	world.Tick(1, 600);
	Check(Applied({1, 2}), "what a mission did to the world is applied with it, first");
	g_applied.clear();
	g_life = 2;   // a save made after it
	world.Tick(1, 700);
	Check(g_applied.empty(), "and not again for a save that has the mission");
	g_globals.clear();
	g_life = 3;   // one made before it
	world.Tick(1, 800);
	Check(Applied({1, 2}), "while a save from before it gets both again");

	MissionSync owner = Fresh();
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 20, 0x03), 0, 1000);
	CampaignDeltaBody body{};
	body.seq        = 99;
	body.valueCount = 2;
	owner.SendCampaignDelta(body, 2000);
	const C_CampaignDelta *sent = LastSent<C_CampaignDelta>();
	Check(sent && sent->body.valueCount == 2 && sent->body.seq == 0,
	      "the owner sends its own, and the number is the server's to give");
}

void TestTheEnemiesStandUpToMorePlayers() {
	std::printf("\nthe enemies for more than one player\n");
	MissionSync m = Fresh();
	S_MissionState st = State(MISSION_STATE_RUNNING, 0, 44, 0x03);
	m.OnState(st, 1, 1000);
	Check(m.EnemyToughness() == 1.0f, "single player's enemies unless the server says");
	st.enemies  = MISSION_ENEMIES_TOUGHER;
	st.scalePct = 50;
	m.OnState(st, 1, 1100);
	Check(m.EnemyToughness() == 1.5f, "tougher at 50% makes two players' enemies half as tough again");
	st.participants = 0x07;
	m.OnState(st, 1, 1200);
	Check(m.EnemyToughness() == 2.0f, "and three players' twice as tough");
	st.participants = 0x01;
	m.OnState(st, 1, 1300);
	Check(m.EnemyToughness() == 1.0f, "while one player alone has single player's");
	st.participants = 0x03;
	st.scalePct     = 9000;
	m.OnState(st, 1, 1400);
	Check(m.EnemyToughness() == 3.0f, "a scale past 200% is taken as 200%");
	m.OnState(State(MISSION_STATE_IDLE, 0, 44, 0, MISSION_OUTCOME_PASSED), 1, 1500);
	Check(m.EnemyToughness() == 1.0f, "and with no mission running there are no enemies to scale");
}

void TestEachLaunchKnowsItsKind() {
	std::printf("\nwhat kind of start a mission has\n");
	Check(MissionKindOf(3) == MISSION_KIND_RC && MissionKindOf(6) == MISSION_KIND_RC &&
	          MissionKindOf(7) == MISSION_KIND_4X4 && MissionKindOf(10) == MISSION_KIND_4X4 &&
	          MissionKindOf(11) == MISSION_KIND_ODDJOB && MissionKindOf(14) == MISSION_KIND_ODDJOB &&
	          MissionKindOf(19) == MISSION_KIND_STORY && MissionKindOf(15) == MISSION_KIND_STORY,
	      "the RC and 4x4 runs, Mayhem, the odd jobs, and the rest a contact's");
}

void TestAConditionIsWidenedToEverybody() {
	std::printf("\na condition asked of the owner, answered for everybody\n");
	using namespace game;
	Check(!RawCondition(0, false) && RawCondition(1, false) && RawCondition(0, true) &&
	          !RawCondition(1, true),
	      "what the engine found is its flag, read through the script's NOT");
	Check(TrueAfterAll(false) == 1 && TrueAfterAll(true) == 0 &&
	          RawCondition(TrueAfterAll(true), true) && RawCondition(TrueAfterAll(false), false),
	      "and a participant spotted makes it true whichever way the script asked");
}

void TestEverybodyStandsBesideTheOwner() {
	std::printf("\nwhere everybody stands when the mission moves its player\n");
	using namespace game;
	float ax = 0, ay = 0, bx = 0, by = 0;
	SpreadSpot(100.0f, 200.0f, 0, 2, 0, &ax, &ay);
	SpreadSpot(100.0f, 200.0f, 1, 2, 0, &bx, &by);
	const float da = std::sqrt((ax - 100.0f) * (ax - 100.0f) + (ay - 200.0f) * (ay - 200.0f));
	const float db = std::sqrt((bx - 100.0f) * (bx - 100.0f) + (by - 200.0f) * (by - 200.0f));
	const float dab = std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
	Check(std::fabs(da - SPREAD_RADIUS_M) < 0.01f && std::fabs(db - SPREAD_RADIUS_M) < 0.01f,
	      "each a step and a half from the owner's spot");
	Check(dab > 2.0f, "and not on each other");
	float cx = 0, cy = 0;
	SpreadSpot(100.0f, 200.0f, 0, 2, 1, &cx, &cy);
	const float dac = std::sqrt((ax - cx) * (ax - cx) + (ay - cy) * (ay - cy));
	Check(dac > 1.0f && dac < 1.2f, "the next try an eighth of the ring round");
	float ex = 0, ey = 0, fx = 0, fy = 0;
	SpreadSpot(0.0f, 0.0f, 0, 7, 0, &ex, &ey);
	SpreadSpot(0.0f, 0.0f, 6, 7, 0, &fx, &fy);
	Check(std::sqrt((ex - fx) * (ex - fx) + (ey - fy) * (ey - fy)) > 1.0f,
	      "seven of them still stand apart");

	// The rings a participant is moved onto fit cars (mission.h, SpotRadii).
	const float *foot = SpotRadii(false);
	const float *car  = SpotRadii(true);
	float g0x = 0, g0y = 0, g1x = 0, g1y = 0;
	SpreadSpot(0.0f, 0.0f, 0, 7, 0, &g0x, &g0y, car[0]);
	SpreadSpot(0.0f, 0.0f, 1, 7, 0, &g1x, &g1y, car[0]);
	Check(car[0] >= 6.5f && std::sqrt((g0x - g1x) * (g0x - g1x) + (g0y - g1y) * (g0y - g1y)) > 5.5f,
	      "a participant in a car is put a car and a half from the owner's spot, and seven "
	      "cars round it stand further apart than a car is long");
	Check(foot[0] >= 2.5f && foot[0] < car[0] && foot[1] < foot[0] && car[1] < car[0],
	      "one on foot clear of a car at the owner's spot, and each has a narrower ring to try");

	MissionSync m = Fresh();   // we are bob, 1
	m.OnState(State(MISSION_STATE_RUNNING, 0, 24, 0x0F), 1, 1000);
	g_roster.others = {{0, true, {0, 0, 0}}, {2, true, {0, 0, 0}}, {3, true, {0, 0, 0}}};
	uint8_t rank = 0, count = 0;
	m.TeleportRank(0, 1, &rank, &count);
	Check(rank == 0 && count == 3, "alice's mission: bob is the first of the three who move");
	MissionSync d = Fresh();   // we are dave, 3
	d.OnState(State(MISSION_STATE_RUNNING, 2, 24, 0x0F), 3, 1000);
	g_roster.others = {{0, true, {0, 0, 0}}, {1, true, {0, 0, 0}}, {2, true, {0, 0, 0}}};
	d.TeleportRank(2, 3, &rank, &count);
	Check(rank == 2 && count == 3, "carol's, on dave's machine: dave is the third, after alice and bob");
	d.OnState(State(MISSION_STATE_RUNNING, 2, 24, 0x0D), 3, 1000);   // bob in his own intro
	d.TeleportRank(2, 3, &rank, &count);
	Check(rank == 1 && count == 2,
	      "and with bob out of the mission, dave is the second of two: a player the mission "
	      "does not move keeps no place in the ring");
	g_roster.others = {{0, true, {0, 0, 0}}, {2, true, {0, 0, 0}}, {3, true, {0, 0, 0}}};
	S_MissionEffect move;
	InitHeader(move, 1000);
	move.ownerId     = 0;
	move.body.kind   = MISSION_EFFECT_TELEPORT;
	move.body.length = 2 + 4 * 5;
	m.OnEffect(move, 1, false, 1000);
	Check(g_teleports.size() == 1 && g_teleports[0].first == 0 && g_teleports[0].second == 3 &&
	          g_effects == 0,
	      "the owner's SET_PLAYER_COORDINATES moves us, as the first of three, not as a plain run");
}

// Give Me Liberty's safehouse: the owner, on foot, is put in the room behind
// the door (895.9, -311.4) while a participant sits at the wheel of the
// Kuruma outside, 21 m off. The handler moves a player's car with him, and
// the ring round that spot is in the walls; the participant drives the car,
// so every screen followed it there.
void TestAPedestriansMoveLeavesTheCar() {
	std::printf("\nthe owner's move on foot, and a participant in a car\n");
	using namespace game;
	Check(OwnerMoveTag(false, 4) == 0 && !OwnerMovedInCar(0) && OwnerMoveCar(0) == INVALID_NETID,
	      "on foot is 0, and names no car");
	Check(OwnerMovedInCar(OwnerMoveTag(true, 4)) && OwnerMoveCar(OwnerMoveTag(true, 4)) == 4,
	      "in a car is that car's netId, one up");
	Check(OwnerMovedInCar(OwnerMoveTag(true, INVALID_NETID)) &&
	          OwnerMoveCar(OwnerMoveTag(true, INVALID_NETID)) == INVALID_NETID,
	      "and a car the session has no name for is still a car");
	Check(OwnerMovedInCar(OwnerMoveTag(true, 0xFFFF)) &&
	          OwnerMoveCar(OwnerMoveTag(true, 0xFFFF)) == 0xFFFF,
	      "the highest netId fits");
	Check(!OwnerMovedInCar(-1), "an older owner's -1 reads as on foot, which moves no car");

	MissionMoveFacts f;
	f.inCar        = true;
	f.simulateHere = true;   // we are at its wheel
	f.ownerTag     = OwnerMoveTag(false, 0);
	f.distanceM    = 21.0f;
	Check(MissionMoveFor(f) == MissionMove::StayInCar,
	      "Give Me Liberty: the owner walked into the safehouse, and the car we drive stays "
	      "where it is parked rather than going onto the ring round a room");
	f.distanceM = 400.0f;
	Check(MissionMoveFor(f) == MissionMove::OutOfCar,
	      "a move on foot across town takes us out of the car and beside the owner on foot");
	f.distanceM    = 21.0f;
	f.simulateHere = false;
	Check(MissionMoveFor(f) == MissionMove::StayInCar,
	      "and a passenger stays in their seat for a move on foot nearby too");

	f.ownerTag     = OwnerMoveTag(true, 9);
	f.simulateHere = true;
	Check(MissionMoveFor(f) == MissionMove::CarRing,
	      "the owner moved in a car of their own: ours goes onto the car ring");
	f.ownersCar = true;
	Check(MissionMoveFor(f) == MissionMove::CarOnSpot,
	      "the owner rides in the car we drive: it goes onto the owner's spot, where the "
	      "owner's copy of it was put");
	f.simulateHere = false;
	Check(MissionMoveFor(f) == MissionMove::StaySeated,
	      "we ride in the owner's car: the owner's own move carries it, and we stay seated");
	f.ownersCar = false;
	Check(MissionMoveFor(f) == MissionMove::StaySeated,
	      "we ride in somebody else's car: its driver's machine moves it, not ours");

	MissionMoveFacts out;
	out.inCar    = true;
	out.warpsOut = true;
	out.ownerTag = OwnerMoveTag(false, 0);
	Check(MissionMoveFor(out) == MissionMove::OutOfCar,
	      "WARP_PLAYER_FROM_CAR_TO_COORD takes us out of any car, and leaves the car");
	MissionMoveFacts foot;
	foot.ownerTag = OwnerMoveTag(true, 4);
	Check(MissionMoveFor(foot) == MissionMove::OnFoot, "on foot, we are simply put on the ring");
}

void TestTheWidgetsFollowTheOwner() {
	std::printf("\nthe HUD's timer and counter, on everybody's screen\n");
	MissionSync owner = Fresh();   // alice, 0
	owner.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 500);
	owner.WidgetValue(0x144, 60000, true, false, 0, 1000);
	Check(SentCount<C_MissionWidget>() == 0, "nothing is sent while no mission runs");
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 36, 0x03), 0, 1000);
	owner.WidgetValue(0x144, 60000, true, false, 0, 1000);
	const C_MissionWidget *w = LastSent<C_MissionWidget>();
	Check(w && w->offset == 0x144 && w->value == 60000 && w->missionNumber == 36,
	      "a timer that has just come up is sent at once");
	for (uint32_t t = 1016; t < 2900; t += 16)
		owner.WidgetValue(0x144, 60000 - static_cast<int32_t>(t - 1000), true, false, 0, t);
	Check(SentCount<C_MissionWidget>() == 1, "and not while it counts down the way everybody's does");
	owner.WidgetValue(0x144, 60000 - 2000 + 10000, true, false, 0, 3000);
	w = LastSent<C_MissionWidget>();
	Check(SentCount<C_MissionWidget>() == 2 && w->value == 68000,
	      "ten seconds more for a checkpoint is sent");
	owner.WidgetValue(0x144, 68000, true, true, 0, 3050);
	Check(SentCount<C_MissionWidget>() == 2, "not twice in a tenth of a second");
	owner.WidgetValue(0x144, 68000, true, true, 0, 3100);
	Check(SentCount<C_MissionWidget>() == 3, "a freeze is");
	owner.WidgetValue(0x144, 68000, true, true, 0, 4000);
	Check(SentCount<C_MissionWidget>() == 3, "and a frozen timer that stays put is not");
	owner.WidgetValue(0x144, 68000, true, true, 0, 5200);
	Check(SentCount<C_MissionWidget>() == 4, "except every two seconds, for anybody who missed it");

	owner.WidgetValue(0x150, 3, false, false, 0, 5300);
	owner.WidgetValue(0x150, 3, false, false, 0, 5500);
	Check(SentCount<C_MissionWidget>() == 5, "a counter is sent when it comes up");
	owner.WidgetValue(0x150, 4, false, false, 0, 5600);
	w = LastSent<C_MissionWidget>();
	Check(SentCount<C_MissionWidget>() == 6 && w->offset == 0x150 && w->value == 4,
	      "and each time it changes");

	MissionSync bob = Fresh();   // bob, 1
	static std::vector<std::pair<uint16_t, int32_t>> set;
	set.clear();
	g_bridge.SetWidget = [](uint16_t offset, int32_t value) { set.emplace_back(offset, value); };
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 36, 0x03), 1, 1000);
	S_MissionWidget in{};
	InitHeader(in, 1000);
	in.ownerId = 0;
	in.offset  = 0x144;
	in.value   = 42000;
	bob.OnWidget(in, 1);
	Check(set.size() == 1 && set[0].first == 0x144 && set[0].second == 42000,
	      "a participant's copy of the global is the owner's");
	in.ownerId = 2;
	bob.OnWidget(in, 1);
	Check(set.size() == 1, "and nobody else's");
	bob.OnState(State(MISSION_STATE_IDLE, 0, 36, 0, MISSION_OUTCOME_PASSED), 1, 9000);
	in.ownerId = 0;
	bob.OnWidget(in, 1);
	Check(set.size() == 1, "and not once the mission is over");
}

void TestWhatAMissionWritesIntoTheCampaign() {
	std::printf("\nwhat a mission writes into main.scm's globals\n");
	using namespace game;
	Check(AssignmentOf(0x0084) == Assignment::Copy && AssignmentOf(0x008A) == Assignment::CopyLocal &&
	          AssignmentOf(0x0060) == Assignment::Value && AssignmentOf(0x0058) == Assignment::Value &&
	          AssignmentOf(0x0086) == Assignment::Value && AssignmentOf(0x008C) == Assignment::Value &&
	          AssignmentOf(0x0078) == Assignment::Value,
	      "a copy, the arithmetic between variables, the timed floats and the conversions count");
	Check(AssignmentOf(0x0085) == Assignment::None && AssignmentOf(0x005A) == Assignment::None &&
	          AssignmentOf(0x0062) == Assignment::None && AssignmentOf(0x0079) == Assignment::None &&
	          AssignmentOf(0x0095) == Assignment::None && AssignmentOf(0x0038) == Assignment::None,
	      "the local forms and the comparisons do not");
	Check(WritesHandle(0x009A) && WritesHandle(0x00A5) && WritesHandle(0x018A) &&
	          WritesHandle(0x0213) && WritesHandle(0x03BC) && !WritesHandle(0x0298) &&
	          !WritesHandle(0x0004),
	      "a pedestrian, a car, a blip, a pickup and a sphere are handles; a rampage's kills are not");

	// An output is the last three bytes before the next instruction.
	std::vector<uint8_t> code(0x40, 0);
	const uint8_t createCar[] = {0xA5, 0x00, 0x05, 0x5A, 0x00, 0x06, 0, 0, 0x06, 0, 0,
	                             0x06, 0, 0, 0x02, 0x34, 0x12};
	std::memcpy(code.data() + 0x10, createCar, sizeof createCar);
	const uint32_t after = 0x10 + sizeof createCar;
	Check(HandleOutputGlobal(code.data(), 0x40, 0x12, after) == 0x1234,
	      "CREATE_CAR's car goes into the global its last operand names");
	code[after - 3] = 0x03;
	Check(HandleOutputGlobal(code.data(), 0x40, 0x12, after) == 0 &&
	          HandleOutputGlobal(code.data(), 0x40, 0x12, 0x12) == 0,
	      "and nowhere the campaign keeps when that is a local, or when the script did not move on");

	// RC1's end, as main.scm writes it: $COUNTER_RC = 0, then the kills,
	// then $REWARD_RC = $COUNTER_RC and $RC1_RECORD = $COUNTER_RC.
	std::vector<uint8_t> globals(0x100, 0);
	auto set = [&](uint16_t at, int32_t v) { std::memcpy(globals.data() + at, &v, 4); };
	auto get = [&](uint16_t at) {
		int32_t v = 0;
		std::memcpy(&v, globals.data() + at, 4);
		return v;
	};
	constexpr uint16_t COUNTER = 0x10, REWARD = 0x14, RECORD = 0x18, TEMP = 0x1C, PED = 0x20,
	                   PED9 = 0x24, CAR = 0x28, MAIN_HANDLE = 0x2C, RECORD_TEMP = 0x30, BEST = 0x34;
	set(RECORD, 12);
	set(BEST, 250);
	set(MAIN_HANDLE, 0x4401);
	CampaignWrites<16> w;
	w.Note(COUNTER, get(COUNTER), false);
	set(COUNTER, 0);
	set(COUNTER, 19);   // 0298 is no handle, and CampaignWrites never hears of it
	w.NoteCopy(REWARD, get(REWARD), COUNTER);
	set(REWARD, 19);
	w.NoteCopy(RECORD, get(RECORD), COUNTER);
	set(RECORD, 19);
	// The 4x4's best time: $RECORD_TEMP = 300000, -= the timer, /= 1000.
	w.Note(RECORD_TEMP, get(RECORD_TEMP), false);
	set(RECORD_TEMP, 212);
	w.NoteCopy(BEST, get(BEST), RECORD_TEMP);
	set(BEST, 212);
	// A pedestrian made into a global and passed along two more.
	w.Note(PED, get(PED), true);
	set(PED, 0x0A05);
	w.NoteCopy(PED9, get(PED9), PED);
	set(PED9, 0x0A05);
	w.NoteCopy(TEMP, get(TEMP), MAIN_HANDLE);
	set(TEMP, 0x4401);
	// A global set to a value, then a car put in it.
	w.Note(CAR, get(CAR), false);
	set(CAR, 1);
	w.Note(CAR, get(CAR), true);
	set(CAR, 0x2203);

	uint16_t changed[16];
	const size_t n    = w.Changed(globals.data(), changed);
	auto         sent = [&](uint16_t at) {
        for (size_t i = 0; i < n; ++i)
            if (changed[i] == at)
                return true;
        return false;
	};
	Check(sent(RECORD) && sent(REWARD) && sent(COUNTER),
	      "RC1's new record goes in the delta, and the reward it was worked out with");
	Check(sent(RECORD_TEMP) && sent(BEST), "and the 4x4's best time");
	Check(!sent(PED) && !sent(PED9) && !sent(TEMP) && !sent(CAR) && n == 5,
	      "a handle does not, nor its copies, nor a copy of a global the mission never wrote");
	Check(w.HoldsHandle(PED9) && !w.HoldsHandle(RECORD) && !w.Known(MAIN_HANDLE),
	      "a copy is what it copied");
	w.Note(CAR, get(CAR), false);
	set(CAR, 0);
	Check(w.Changed(globals.data(), changed) == 5, "a handle set back to what it held is nothing to send");
	set(CAR, 3);
	Check(w.Changed(globals.data(), changed) == 6, "and one set to a new value is a value again");

	CampaignWrites<2> small;
	Check(small.Note(1 * 4, 0, false) && small.Note(2 * 4, 0, false) && small.Note(1 * 4, 0, true) &&
	          !small.Note(3 * 4, 0, false) && small.Count() == 2,
	      "a full table takes a write to a global it has, and no new one");
}

void TestTheCampaignInTheScript() {
	std::printf("\nwhat a mission leaves behind, in the script\n");
	using namespace game;
	std::vector<uint8_t> space(0x400, 0);
	const uint8_t goTo[] = {0x02, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00};
	std::memcpy(space.data(), goTo, sizeof goTo);
	Check(GlobalsEnd(space.data(), 0x400) == 0x40, "main.scm's globals end where its first GOTO goes");
	Check(GlobalsEnd(space.data(), 0x40) == 0 && GlobalsEnd(space.data() + 1, 0x3FF) == 0,
	      "and a script space that does not start with one has none");
	Check(IsTrackedAssignment(0x0004) && IsTrackedAssignment(0x0008) &&
	          IsTrackedAssignment(0x0015) && !IsTrackedAssignment(0x0006) &&
	          IsTrackedAssignment(0x0084) && !IsTrackedAssignment(0x0085),
	      "a global's plain assignment and arithmetic are tracked, a local's are not");
	Check(IsMainThreadLabel(0x1234) && !IsMainThreadLabel(-0x40) &&
	          !IsMainThreadLabel(static_cast<int32_t>(scripts::MAIN_SCRIPT_SIZE)),
	      "a thread in main.scm, not one of the mission's own");

	uint8_t code[8];
	const uint8_t start[] = {0x4F, 0x00, 0x01, 0x34, 0x12, 0x00, 0x00, 0x00};
	Check(StartThreadCode(0x1234, code) == 8 && std::memcmp(code, start, 8) == 0,
	      "START_NEW_SCRIPT is the label and the end of an empty argument list");

	const uint8_t named[] = {0xA4, 0x03, 'D', 'I', 'A', 'B', '_', 'P', 'H', 0x00};
	std::memcpy(space.data() + 0x100, named, sizeof named);
	const uint8_t waits[] = {0x01, 0x00, 0x04, 0x00};
	std::memcpy(space.data() + 0x120, waits, sizeof waits);
	char name[8] = {};
	Check(ThreadNameAt(space.data(), 0x400, 0x100, name) && std::strcmp(name, "DIAB_PH") == 0,
	      "a helper thread's name is read where it starts");
	Check(!ThreadNameAt(space.data(), 0x400, 0x120, name) &&
	          !ThreadNameAt(space.data(), 0x105, 0x100, name),
	      "a trigger, which starts with a WAIT, has none, and nothing is read past the end");
	char tag[8] = {};
	ThreadTag(0x1A2B3, tag);
	Check(std::strcmp(tag, "@01A2B3") == 0, "one that never names itself goes by its label");

	std::vector<uint8_t> save = space;
	save[0x10]                = 0x55;   // a global
	Check(ScriptCodeHash(space.data(), 0x400) != 0 &&
	          ScriptCodeHash(space.data(), 0x400) == ScriptCodeHash(save.data(), 0x400),
	      "the script's hash leaves its globals out, which every save changes");
	save[0x200] = 0x01;
	Check(ScriptCodeHash(space.data(), 0x400) != ScriptCodeHash(save.data(), 0x400),
	      "and counts every byte of its code");
	std::vector<uint8_t> none(0x400, 0);
	Check(ScriptCodeHash(none.data(), 0x400) == 0, "and is nothing before main.scm is loaded");

	uint16_t offsets[40];
	for (uint16_t i = 0; i < 40; ++i) {
		offsets[i]      = static_cast<uint16_t>(0x100 + 4 * i);
		const int32_t v = 1000 + i;
		std::memcpy(space.data() + offsets[i], &v, 4);
	}
	CampaignThread threads[1] = {};
	threads[0].label          = 0x120;
	CampaignDeltaBody parts[2];
	Check(BuildCampaignDelta(21, offsets, 40, space.data(), threads, 1, 0xABCD, parts, 1) == 0,
	      "a delta that does not fit is not built");
	const size_t n = BuildCampaignDelta(21, offsets, 40, space.data(), threads, 1, 0xABCD, parts, 2);
	Check(n == 2 && parts[0].valueCount == 32 && parts[1].valueCount == 8 && parts[0].last == 0 &&
	          parts[1].last == 1 && parts[0].threadCount == 0 && parts[1].threadCount == 1 &&
	          parts[1].threads[0].label == 0x120,
	      "forty globals go in two parts, the threads in the last");
	Check(parts[0].values[0].offset == 0x100 && parts[0].values[0].value == 1000 &&
	          parts[1].values[7].offset == 0x100 + 4 * 39 && parts[1].values[7].value == 1039 &&
	          parts[0].scriptHash == 0xABCD && parts[1].scriptHash == 0xABCD &&
	          parts[0].missionNumber == 21,
	      "each with the values as the script holds them now, and the script's hash");

	const uint8_t swap[] = {0xB6, 0x03, 0x01, 1, 2, 3, 4};
	const CampaignDeltaBody opPart = CampaignOpPart(44, swap, sizeof swap, 0xABCD);
	Check(opPart.opLength == sizeof swap && std::memcmp(opPart.op, swap, sizeof swap) == 0 &&
	          opPart.valueCount == 0 && opPart.last == 0 && opPart.missionNumber == 44 &&
	          opPart.scriptHash == 0xABCD,
	      "an instruction the world keeps is a part of its own");
	Check(CampaignOpPart(44, swap, 1, 0).opLength == 0 &&
	          CampaignOpPart(44, swap, MISSION_EFFECT_CODE + 1, 0).opLength == 0,
	      "and one that cannot be an instruction is none");
}

void TestWhatTheOwnersMissionShows() {
	std::printf("\nwhat the owner's mission shows, on a participant\n");
	MissionSync m = Fresh();   // we are bob, 1
	S_MissionEffect fx;
	InitHeader(fx, 1000);
	fx.ownerId       = 0;
	fx.body.kind     = MISSION_EFFECT_RUN;
	fx.body.length   = 2;
	m.OnEffect(fx, 1, false, 1000);
	Check(g_effects == 0, "nothing is run from a server that never said it shares missions");
	m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	m.OnEffect(fx, 1, false, 1000);
	Check(g_effects == 1 && m.EffectsRun() == 1, "alice's title is shown here, before alice's start");
	m.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 1, 1000);
	fx.body.kind = MISSION_EFFECT_PAY;
	m.OnEffect(fx, 1, true, 1000);
	Check(g_effects == 1, "alice's pay is not paid again into a wallet everybody shares");
	m.OnEffect(fx, 1, false, 1000);
	Check(g_effects == 2, "and is paid here when everybody keeps their own");
	fx.ownerId = 1;
	m.OnEffect(fx, 1, false, 1000);
	Check(g_effects == 2, "our own never comes back to us to be shown twice");
	fx.body.length = MISSION_EFFECT_CODE + 1;
	fx.ownerId     = 0;
	m.OnEffect(fx, 1, false, 1000);
	Check(g_effects == 2, "and one longer than the packet holds is refused");
	m.OnState(State(MISSION_STATE_IDLE, 0, 19, 0, MISSION_OUTCOME_PASSED), 1, 9000);
	Check(g_ended == 1, "when it ends, what it left on our radar goes");

	MissionSync owner = Fresh();   // alice, 0: the cleanup of alice's own mission is alice's script's
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 0, 1000);
	owner.OnState(State(MISSION_STATE_IDLE, 0, 19, 0, MISSION_OUTCOME_PASSED), 0, 9000);
	Check(g_ended == 0, "and the owner's own is left to the owner's own script");
	MissionEffectBody body{};
	body.length = 2;
	owner.SendEffect(body, 9100);
	Check(LastSent<C_MissionEffect>() != nullptr, "who sends what it shows");
}

// An effect as game/mission.cpp records one: the opcode, then its operands as
// replay::Encode writes them.
struct Fx {
	MissionEffectBody b{};
	Fx(uint8_t kind, uint16_t opcode, int32_t handle = -1) {
		b.missionNumber = 19;
		b.kind          = kind;
		b.handleAt      = 0xFF;
		b.ownerBlip     = handle;
		Byte(uint8_t(opcode & 0xFF)).Byte(uint8_t(opcode >> 8));
	}
	Fx &Byte(uint8_t v) {
		b.code[b.length++] = v;
		return *this;
	}
	Fx &Int(int32_t v) {
		Byte(1);
		for (int i = 0; i < 4; ++i)
			Byte(uint8_t(uint32_t(v) >> (8 * i)));
		return *this;
	}
	Fx &Float(float f) {
		int32_t v = 0;
		std::memcpy(&v, &f, 4);
		return Int(v);
	}
	Fx &Global(uint16_t g) { return Byte(2).Byte(uint8_t(g & 0xFF)).Byte(uint8_t(g >> 8)); }
	Fx &Local0() { return Byte(3).Byte(0).Byte(0); }
	Fx &Text(const char *t) {
		char label[8] = {};
		std::strncpy(label, t, 7);
		for (char c : label)
			Byte(uint8_t(c));
		return *this;
	}
};

int32_t LiteralOf(const MissionEffectBody &b, uint8_t index) {
	int32_t v = 0;
	return game::replay::LiteralAt(b.code, b.length, index, &v) ? v : INT32_MIN;
}

float FloatOf(const MissionEffectBody &b, uint8_t index) {
	const int32_t v = LiteralOf(b, index);
	float         f = 0.0f;
	std::memcpy(&f, &v, 4);
	return f;
}

// The order the effects ran in, by the first byte of their code, and whether
// the copy they name is built yet.
std::vector<uint8_t> g_ranInOrder;
bool                 g_copyBuilt = false;

void TestAnInstructionWaitsForTheCopyItNames() {
	std::printf("\nan instruction that names a copy still being built\n");
	MissionSync m = Fresh();   // bob, 1
	g_ranInOrder.clear();
	g_copyBuilt       = false;
	g_bridge.RunEffect = [](const MissionEffectBody &b) {
		if (b.code[0] == 1 && !g_copyBuilt)
			return false;
		g_ranInOrder.push_back(b.code[0]);
		++g_effects;
		return true;
	};
	g_bridge.EffectAwaits = [](const MissionEffectBody &b) {
		return b.code[0] == 1 && !g_copyBuilt;
	};
	m.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 1, 1000);

	S_MissionEffect colour;   // CHANGE_CAR_COLOUR on the Kuruma, a frame after CREATE_CAR
	InitHeader(colour, 1000);
	colour.ownerId      = 0;
	colour.body.kind    = MISSION_EFFECT_RUN;
	colour.body.length  = 2;
	colour.body.code[0] = 1;
	S_MissionEffect print = colour;   // PRINT_BIG, which names nothing
	print.body.code[0]    = 2;

	m.OnEffect(colour, 1, false, 1000);
	m.OnEffect(print, 1, false, 1000);
	Check(g_effects == 0 && m.EffectsAwaiting() == 2 && m.EffectsAwaited() == 2,
	      "the colour waits for our copy of the car, and the title waits behind it");
	m.Tick(1, 1100);
	Check(g_effects == 0, "still not built, still waiting");

	g_copyBuilt = true;
	m.Tick(1, 1200);
	Check(g_ranInOrder.size() == 2 && g_ranInOrder[0] == 1 && g_ranInOrder[1] == 2 &&
	          m.EffectsAwaiting() == 0,
	      "the copy is here: both run, in the order the owner ran them");

	g_copyBuilt = false;
	m.OnEffect(colour, 1, false, 2000);
	m.Tick(1, 2000 + MISSION_EFFECT_AWAIT_MS);
	Check(m.EffectsAwaiting() == 0,
	      "and one whose copy never comes goes for what it is worth, instead of holding "
	      "up everything after it");

	m.OnEffect(colour, 1, false, 9000);
	m.OnState(State(MISSION_STATE_IDLE, 0, 19, 0, MISSION_OUTCOME_PASSED), 1, 9100);
	Check(m.EffectsAwaiting() == 0, "what still waited when the mission ended goes with it");
}

void TestWhatTheMissionHasUpIsKept() {
	std::printf("\nwhat the owner's mission has up, kept for somebody who comes in late\n");
	using namespace game;
	StandingEffects s;
	const int32_t blip = 0x10001, carBlip = 0x30003;
	s.Note(Fx(MISSION_EFFECT_BLIP_NEW, 0x018A, blip).Float(1).Float(2).Float(3).Local0().b);
	Check(s.Count() == 1, "a blip the mission puts up is kept");
	s.Note(Fx(MISSION_EFFECT_BLIP_USE, 0x0165, blip).Int(blip).Int(3).b);
	s.Note(Fx(MISSION_EFFECT_BLIP_USE, 0x0165, blip).Int(blip).Int(5).b);
	s.Note(Fx(MISSION_EFFECT_BLIP_USE, 0x018B, blip).Int(blip).Int(2).b);
	Check(s.Count() == 3 && EffectOpcode(s.At(1)) == 0x0165 && LiteralOf(s.At(1), 1) == 5 &&
	          EffectOpcode(s.At(2)) == 0x018B,
	      "and what is done to it, a change made again kept once, the last of it");
	s.Note(Fx(MISSION_EFFECT_BLIP_USE, 0x0165, 0x20002).Int(0x20002).Int(1).b);
	Check(s.Count() == 3, "a change to a blip nothing kept made is not");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x00BC).Text("TM3_T").Int(10000).Int(1).b);
	s.Note(Fx(MISSION_EFFECT_PAY, 0x0109).Int(0).Int(1000).b);
	Check(s.Count() == 3, "nor a print or the pay, over before anybody could come in");
	s.Note(Fx(MISSION_EFFECT_BLIP_NEW, 0x0186, carBlip).Int(77).Local0().b);
	s.Note(Fx(MISSION_EFFECT_BLIP_USE, 0x0164, blip).Int(blip).b);
	Check(s.Count() == 1 && EffectOpcode(s.At(0)) == 0x0186,
	      "taking a blip away takes what was done to it along, and leaves the others");

	s.Clear();
	s.Note(Fx(MISSION_EFFECT_PICKUP_NEW, 0x0213, 0x50007).Int(172).Int(3).Float(5).Float(6).Float(7)
	           .Local0().b);
	s.Note(Fx(MISSION_EFFECT_FIRE_NEW, 0x02CF, 4).Float(5).Float(6).Float(7).Local0().b);
	Check(s.Count() == 2, "a pickup and a fire are kept");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0215).Int(0x50008).b);
	Check(s.Count() == 2, "taking away another pickup leaves this one");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0215).Int(0x50007).b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x02D1).Int(4).b);
	Check(s.Count() == 0, "until the mission takes them away");
	const int32_t late = int32_t(0x80000000u);   // slot 0, generation 0x8000
	s.Note(Fx(MISSION_EFFECT_PICKUP_NEW, 0x0213, 0x50007).Int(172).Int(3).Float(5).Float(6).Float(7)
	           .Local0().b);
	s.Note(Fx(MISSION_EFFECT_PICKUP_NEW, 0x0213, late).Int(172).Int(3).Float(5).Float(8).Float(7)
	           .Local0().b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0215).Int(late).b);
	Check(s.Count() == 1 && s.At(0).ownerBlip == 0x50007,
	      "a handle with its top bit set is a handle like any other");
	s.Clear();

	const uint16_t door = 0x0A40;
	s.Note(Fx(MISSION_EFFECT_RUN, 0x029B).Int(1400).Float(10).Float(20).Float(5).Global(door).b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0382).Global(door).Int(0).b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0382).Global(door).Int(1).b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x01BC).Global(door).Float(11).Float(20).Float(5).b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x034D).Global(door).Float(90).Float(5).Int(0).b);
	s.Note(Fx(MISSION_EFFECT_BLIP_NEW, 0x0188, 0x60001).Global(door).Local0().b);
	Check(s.Count() == 3 && EffectOpcode(s.At(0)) == 0x029B && LiteralOf(s.At(1), 1) == 1,
	      "an object is kept, with the last of what was done to it and a blip on it; not its "
	      "moves, which are read off the object when it is handed over");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0107).Int(1401).Float(10).Float(20).Float(5).Global(door).b);
	Check(s.Count() == 2 && EffectOpcode(s.At(1)) == 0x0107,
	      "another object made into the same global stands instead of the first");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0108).Global(door).b);
	Check(s.Count() == 1 && EffectOpcode(s.At(0)) == 0x0188, "and deleting it takes it away");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0107).Int(1401).Float(10).Float(20).Float(5).Global(door).b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x01C4).Global(door).b);
	Check(s.Count() == 1, "as does letting go of it");

	s.Clear();
	s.Note(Fx(MISSION_EFFECT_RUN, 0x014E).Global(0x144).b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0396).Int(1).b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0396).Int(0).b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x03C4).Global(0x150).Int(0).Text("KILLS").b);
	Check(s.Count() == 3 && LiteralOf(s.At(1), 0) == 0, "the timer, its last freeze and the counter are kept");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x014F).Global(0x148).b);
	Check(s.Count() == 3, "clearing a timer other than the one up does nothing, as on the HUD");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x014F).Global(0x144).b);
	Check(s.Count() == 1 && EffectOpcode(s.At(0)) == 0x03C4,
	      "clearing the one up takes it and its freeze away");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0150).Global(0x158).Int(1).b);
	Check(s.Count() == 1 && EffectOpcode(s.At(0)) == 0x0150,
	      "the HUD has one counter: another stands instead of the first");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x0396).Int(1).b);
	Check(s.Count() == 1, "and a freeze with no timer up is not kept");

	s.Clear();
	bool kept = true;
	for (int32_t i = 0; i < int32_t(MAX_STANDING); ++i)
		kept = s.Note(Fx(MISSION_EFFECT_BLIP_NEW, 0x018A, 0x10000 + i).Float(1).Float(2).Float(3)
		                  .Local0().b) && kept;
	Check(kept && s.Count() == MAX_STANDING, "it holds MAX_STANDING");
	Check(!s.Note(Fx(MISSION_EFFECT_BLIP_NEW, 0x018A, 0x20000).Float(1).Float(2).Float(3).Local0().b),
	      "and refuses one more");
	Check(s.Note(Fx(MISSION_EFFECT_BLIP_USE, 0x0164, 0x10000).Int(0x10000).b) &&
	          s.Count() == MAX_STANDING - 1,
	      "while taking one away is never refused");

	const MissionEffectBody place = ObjectPlaceEffect(19, door, 10.5f, -20.25f, 3.0f);
	uint16_t                named = 0;
	Check(place.kind == MISSION_EFFECT_RUN && place.onlyTo == 0 && EffectOpcode(place) == 0x01BC &&
	          place.length == 20 && replay::ObjectGlobals(place.code, place.length, &named, 1) == 1 &&
	          named == door && FloatOf(place, 1) == 10.5f && FloatOf(place, 2) == -20.25f &&
	          FloatOf(place, 3) == 3.0f,
	      "an object's place goes as SET_OBJECT_COORDINATES, the object by its global, as the "
	      "replay list reads it");
	const MissionEffectBody turn = ObjectHeadingEffect(19, door, 90.0f);
	named                        = 0;
	Check(EffectOpcode(turn) == 0x0177 && turn.length == 10 &&
	          replay::ObjectGlobals(turn.code, turn.length, &named, 1) == 1 && named == door &&
	          FloatOf(turn, 1) == 90.0f,
	      "and the way it faces as SET_OBJECT_HEADING");
	Check(std::fabs(HeadingDegrees(0.0f, 1.0f)) < 0.01f &&
	          std::fabs(HeadingDegrees(-1.0f, 0.0f) - 90.0f) < 0.01f &&
	          std::fabs(HeadingDegrees(0.0f, -1.0f) - 180.0f) < 0.01f &&
	          std::fabs(HeadingDegrees(1.0f, 0.0f) - 270.0f) < 0.01f,
	      "read off the matrix the way SET_OBJECT_HEADING writes it");
}

void TestSomebodyWhoComesInIsHandedWhatIsUp() {
	std::printf("\nsomebody who comes into a running mission\n");
	static std::vector<uint8_t> handed;
	handed.clear();
	MissionSync owner = Fresh();   // alice, 0
	g_bridge.ResendStanding = [](uint8_t id) { handed.push_back(id); };
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 0, 1000);
	Check(handed.empty(), "nobody is handed anything as it starts: everybody sees all of it");
	owner.WidgetValue(0x144, 60000, true, false, 0, 1000);
	owner.WidgetValue(0x144, 50000, true, false, 0, 2500);
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x05), 0, 2600);
	Check(handed.empty(), "nor as somebody leaves");
	const size_t widgets = SentCount<C_MissionWidget>();
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x0F), 0, 3000);
	Check(handed.size() == 2 && handed[0] == 1 && handed[1] == 3 && owner.StandingHandedOver() == 2,
	      "bob coming back and dave coming in are each handed what it has up");
	owner.WidgetValue(0x144, 49484, true, false, 0, 3016);
	Check(SentCount<C_MissionWidget>() == widgets + 1,
	      "and the timer's value straight after, not at its next resync");

	MissionSync bob = Fresh();   // bob, 1
	g_bridge.ResendStanding = [](uint8_t id) { handed.push_back(id); };
	handed.clear();
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 1, 1000);
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x0F), 1, 2000);
	Check(handed.empty(), "only the owner hands anything over");
	S_MissionEffect fx;
	InitHeader(fx, 1000);
	fx.ownerId     = 0;
	fx.body.kind   = MISSION_EFFECT_RUN;
	fx.body.length = 2;
	fx.body.onlyTo = 4;   // dave's
	bob.OnEffect(fx, 1, false, 1000);
	Check(g_effects == 0, "and what alice hands dave is not run on bob's machine");
	fx.body.onlyTo = 2;
	bob.OnEffect(fx, 1, false, 1000);
	Check(g_effects == 1, "what alice hands bob is");

	MissionEffectBody body{};
	body.length = 2;
	body.onlyTo = MAX_PLAYERS + 1;
	const size_t before = SentCount<C_MissionEffect>();
	owner.SendEffect(body, 3100);
	Check(SentCount<C_MissionEffect>() == before, "one for a player there cannot be is never sent");
	body.onlyTo = 4;
	owner.SendEffect(body, 3100);
	const C_MissionEffect *out = LastSent<C_MissionEffect>();
	Check(out && out->body.onlyTo == 4, "and one for dave goes out marked for dave");
}

void TestTheCutsceneWaitsForEverybodysModels() {
	std::printf("\nthe owner's cutscene waits for everybody's models\n");
	MissionSync owner = Fresh();   // alice, 0
	Check(owner.AskEverybodyLoaded(1, 0, 1000), "with no session's mission nothing waits");
	owner.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 500);
	Check(owner.AskEverybodyLoaded(1, 0, 600), "nor with a mission that is not ours");
	owner.Launched(0x4242, 24, 700);
	Check(!owner.AskEverybodyLoaded(1, 0, 700),
	      "its first load, before the server has said it is the session's, is waited on");
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 24, 0x07), 0, 1000);
	Check(owner.AskEverybodyLoaded(0, 0, 1000), "nothing is waited on before the mission loads");
	Check(!owner.AskEverybodyLoaded(1, 0, 1000), "after its first load it waits for bob and carol");
	S_MissionReady ready{};
	InitHeader(ready, 1000);
	ready.playerId      = 1;
	ready.missionNumber = 24;
	ready.readySeq      = 1;
	owner.OnReady(ready, 0);
	Check(!owner.AskEverybodyLoaded(1, 0, 1200), "bob's models are in, carol's are not");
	ready.playerId      = 2;
	ready.missionNumber = 19;
	owner.OnReady(ready, 0);
	Check(!owner.AskEverybodyLoaded(1, 0, 1300), "carol saying so of another mission counts for nothing");
	ready.missionNumber = 24;
	owner.OnReady(ready, 0);
	Check(owner.AskEverybodyLoaded(1, 0, 1400), "once carol's are, it goes on");
	Check(!owner.AskEverybodyLoaded(2, 0, 2000), "the next load waits again");
	Check(!owner.AskEverybodyLoaded(2, 0, 2000 + MISSION_READY_WAIT_MS - 1),
	      "for as long as MISSION_READY_WAIT_MS");
	Check(owner.AskEverybodyLoaded(2, 0, 2000 + MISSION_READY_WAIT_MS),
	      "and then goes on without them, as it would have");
	Check(owner.AskEverybodyLoaded(2, 0, 2000 + MISSION_READY_WAIT_MS + 16), "and stays gone on");

	ready.readySeq = 3;
	ready.playerId = 1;
	owner.OnReady(ready, 0);
	ready.playerId = 2;
	owner.OnReady(ready, 0);
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 24, 0x03), 0, 9000);   // carol leaves
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 24, 0x07), 0, 9100);   // somebody else in slot 2
	Check(!owner.AskEverybodyLoaded(3, 0, 9200),
	      "whoever comes into a slot has loaded nothing its last player did");

	MissionSync alone = Fresh();   // alice, 0, with nobody else connected
	g_roster.nicks[1] = g_roster.nicks[2] = g_roster.nicks[3] = nullptr;
	alone.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 500);
	alone.Launched(0x4242, 24, 700);
	Check(alone.AskEverybodyLoaded(1, 0, 700), "a player on their own waits for nobody");
	alone.OnState(State(MISSION_STATE_RUNNING, 0, 24, 0x01), 0, 900);
	Check(alone.AskEverybodyLoaded(2, 0, 900), "nor once the mission is theirs");

	MissionSync late = Fresh();   // alice, 0, while dave's mission is the session's
	late.OnState(State(MISSION_STATE_RUNNING, 3, 25, 0x0F), 0, 500);
	late.Launched(0x4242, 24, 700);
	Check(late.AskEverybodyLoaded(1, 0, 700),
	      "a mission of our own while somebody else's is the session's waits for nobody");

	MissionSync bob = Fresh();   // bob, 1
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 24, 0x07), 1, 1000);
	S_MissionEffect fx;
	InitHeader(fx, 1000);
	fx.ownerId            = 0;
	fx.body.missionNumber = 24;
	fx.body.kind          = MISSION_EFFECT_RUN;
	fx.body.length        = 2;
	fx.body.code[0]       = 0x8B;
	fx.body.code[1]       = 0x03;
	bob.OnEffect(fx, 1, false, 1000);
	Check(SentCount<C_MissionReady>() == 0, "an effect nobody waits on is not answered");
	fx.body.readySeq = 4;
	bob.OnEffect(fx, 1, false, 1000);
	const C_MissionReady *said = LastSent<C_MissionReady>();
	Check(g_effects == 2 && said && said->readySeq == 4 && said->missionNumber == 24,
	      "a participant runs the owner's LOAD_ALL_MODELS_NOW, then says so with its number");
	g_bridge.RunEffect = [](const MissionEffectBody &) { return false; };
	bob.OnEffect(fx, 1, false, 1000);
	Check(SentCount<C_MissionReady>() == 2, "and says it even when it could not run it: go ahead");
	fx.ownerId = 2;
	bob.OnEffect(fx, 1, false, 1000);
	Check(SentCount<C_MissionReady>() == 2, "but never to somebody who does not own the mission");

	MissionSync carol = Fresh();   // carol, 2, not yet told of any mission
	fx.ownerId = 0;
	carol.OnEffect(fx, 2, false, 1000);
	Check(SentCount<C_MissionReady>() == 0, "nor with no mission running");
}

void TestTheSeatsTheMissionNeedsAreKept() {
	std::printf("\nthe seats the mission's passengers need\n");
	using game::SeatFlagsFor;
	Check(SeatFlagsFor(0, 3, 0) == 0, "a car nobody is heading for keeps nothing");
	Check(SeatFlagsFor(1, 3, 1) == MISSION_SEAT_KEPT,
	      "Misty heading for a four-door with one seat taken: kept, nobody has to leave");
	Check(SeatFlagsFor(1, 1, 1) == (MISSION_SEAT_KEPT | MISSION_SEAT_LEAVE),
	      "and for a two-door whose one seat a participant is in: they leave");
	Check(SeatFlagsFor(3, 3, 0) == MISSION_SEAT_KEPT && SeatFlagsFor(3, 3, 1) != MISSION_SEAT_KEPT,
	      "The Getaway's three thugs need every seat of a four-door");
	Check(SeatFlagsFor(1, 0, 0) == (MISSION_SEAT_KEPT | MISSION_SEAT_LEAVE) &&
	          SeatFlagsFor(2, 1, 5) == (MISSION_SEAT_KEPT | MISSION_SEAT_LEAVE),
	      "and a car with no seat to give is always left");

	MissionSync owner = Fresh();   // alice, 0
	const MissionSeatCar limo[1] = {{700, MISSION_SEAT_KEPT, 0}};
	owner.SeatsNeeded(limo, 1, 0, 1000);
	Check(SentCount<C_MissionSeats>() == 0, "nothing is said with no session's mission");
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 44, 0x07), 0, 1000);
	owner.SeatsNeeded(nullptr, 0, 0, 1000);
	Check(SentCount<C_MissionSeats>() == 0, "nor while nobody is heading for a car");
	owner.SeatsNeeded(limo, 1, 0, 1100);
	const C_MissionSeats *said = LastSent<C_MissionSeats>();
	Check(said && said->count == 1 && said->cars[0].netId == 700 && said->missionNumber == 44,
	      "a car the mission's passengers are heading for is said at once");
	owner.SeatsNeeded(limo, 1, 0, 1200);
	Check(SentCount<C_MissionSeats>() == 1, "and not again while nothing changes");
	owner.SeatsNeeded(limo, 1, 0, 1100 + MISSION_SEATS_RESYNC_MS);
	Check(SentCount<C_MissionSeats>() == 2, "except every MISSION_SEATS_RESYNC_MS");
	const MissionSeatCar full[1] = {{700, MISSION_SEAT_KEPT | MISSION_SEAT_LEAVE, 0}};
	owner.SeatsNeeded(full, 1, 0, 3200);
	Check(SentCount<C_MissionSeats>() == 3, "a change is said at once");
	owner.SeatsNeeded(nullptr, 0, 0, 3300);
	said = LastSent<C_MissionSeats>();
	Check(SentCount<C_MissionSeats>() == 4 && said->count == 0, "and so is the end of it");
	owner.SeatsNeeded(nullptr, 0, 0, 9000);
	Check(SentCount<C_MissionSeats>() == 4, "which is not said again");

	MissionSync bob = Fresh();   // bob, 1
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 44, 0x07), 1, 1000);
	Check(bob.MaySit(700, 1000) && !bob.MustLeaveSeat(700, 1), "with nothing said, any seat will do");
	S_MissionSeats seats{};
	InitHeader(seats, 1000);
	seats.ownerId       = 2;   // carol does not own it
	seats.count         = 1;
	seats.missionNumber = 44;
	seats.cars[0]       = {700, MISSION_SEAT_KEPT, 0};
	bob.OnSeats(seats, 1);
	Check(bob.MaySit(700, 1000), "somebody who does not own the mission keeps nothing");
	seats.ownerId = 0;
	bob.OnSeats(seats, 1);
	const size_t lines = g_roster.status.size();
	Check(!bob.MaySit(700, 1100) && LastLine() == "the seats in that car are for alice's mission",
	      "bob cannot sit down in the car Misty is heading for, and the log says why");
	Check(bob.MaySit(701, 1100), "any other car will do");
	Check(!bob.MaySit(700, 1200) && g_roster.status.size() == lines + 1,
	      "and the key held down is not told again straight away");
	Check(!bob.MustLeaveSeat(700, 1), "a seat that is only kept is not left");
	seats.cars[0].flags = MISSION_SEAT_KEPT | MISSION_SEAT_LEAVE;
	bob.OnSeats(seats, 1);
	Check(bob.MustLeaveSeat(700, 1) && LastLine() == "you got out: alice's mission needs your seat",
	      "when there are not seats enough, bob riding in it gets out");
	const size_t after = g_roster.status.size();
	Check(bob.MustLeaveSeat(700, 1) && g_roster.status.size() == after, "told once");
	Check(g_roster.feed.empty(), "and only the log is told");
	seats.count = 0;
	bob.OnSeats(seats, 1);
	Check(bob.MaySit(700, 1300) && !bob.MustLeaveSeat(700, 1), "once Misty is in, the seats are anybody's");
	seats.count         = 1;
	seats.cars[0].flags = MISSION_SEAT_KEPT;
	bob.OnSeats(seats, 1);
	bob.OnState(State(MISSION_STATE_IDLE, 0, 44, 0, MISSION_OUTCOME_PASSED), 1, 9000);
	Check(bob.MaySit(700, 9000), "and so they are once the mission is over");
}

void TestOnlyAsManyGetOutAsTheSeatsNeeded() {
	std::printf("\nonly as many get out as the mission's passengers need\n");
	using game::LeaveMaskFor;
	const uint8_t N = INVALID_PLAYER;
	// A three-seat ambulance with bob (1) and carol (2) in the back.
	const uint8_t back[3] = {1, 2, N};
	Check(LeaveMaskFor(1, 3, 2, back, 3) == 0, "one patient fits beside the two of them");
	Check(LeaveMaskFor(2, 3, 2, back, 3) == PlayerBit(2),
	      "a second takes one of them out, the one in the last seat taken, and not both");
	Check(LeaveMaskFor(3, 3, 2, back, 3) == (PlayerBit(1) | PlayerBit(2)),
	      "a third takes both");
	// The Stretch with Maria in slot 1 and three players in a four-door.
	const uint8_t limo[3] = {3, N, 1};
	Check(LeaveMaskFor(1, 3, 3, limo, 3) == PlayerBit(1),
	      "Maria keeps her seat, and whoever sits last gets out for the one coming");
	const uint8_t thugs[3] = {N, N, N};
	Check(LeaveMaskFor(1, 3, 3, thugs, 3) == 0, "a car full of the mission's own names nobody");
	Check(LeaveMaskFor(3, 1, 1, back, 1) == PlayerBit(1), "and a two-seater has one to give");

	MissionSync owner = Fresh();   // alice, 0
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 44, 0x07), 0, 1000);
	const MissionSeatCar one[1] = {{700, MISSION_SEAT_KEPT | MISSION_SEAT_LEAVE, PlayerBit(2)}};
	owner.SeatsNeeded(one, 1, 0, 1000);
	const C_MissionSeats *said = LastSent<C_MissionSeats>();
	Check(said && said->cars[0].leave == PlayerBit(2), "the owner names who gets out");
	const MissionSeatCar both[1] = {
	    {700, MISSION_SEAT_KEPT | MISSION_SEAT_LEAVE, static_cast<uint8_t>(PlayerBit(1) | PlayerBit(2))}};
	owner.SeatsNeeded(both, 1, 0, 1100);
	Check(SentCount<C_MissionSeats>() == 2, "and a change of who is a change");

	S_MissionSeats seats{};
	InitHeader(seats, 1000);
	seats.ownerId       = 0;
	seats.count         = 1;
	seats.missionNumber = 44;
	seats.cars[0]       = {700, MISSION_SEAT_KEPT | MISSION_SEAT_LEAVE, PlayerBit(2)};
	MissionSync bob = Fresh();   // bob, 1
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 44, 0x07), 1, 1000);
	bob.OnSeats(seats, 1);
	Check(!bob.MustLeaveSeat(700, 1), "bob, not named, stays in his seat");
	MissionSync carol = Fresh();   // carol, 2
	carol.OnState(State(MISSION_STATE_RUNNING, 0, 44, 0x07), 2, 1000);
	carol.OnSeats(seats, 2);
	Check(carol.MustLeaveSeat(700, 2), "carol, named, gets out");
	seats.cars[0].leave = 0;
	bob.OnSeats(seats, 1);
	Check(bob.MustLeaveSeat(700, 1), "an owner that names nobody means everybody, as before");
}

void TestEverybodyIntoTheCarTheMissionPutItsPlayerIn() {
	std::printf("\neverybody into the car the mission put its player in\n");
	using game::AssignBoardingSeats;
	using game::PickBoardSeat;
	// Maria's Stretch: slot 1 is Maria's, so wire seats 1 and 3 are free.
	const uint16_t stretch = (1u << 1) | (1u << 3);
	uint8_t        seats[MAX_PLAYERS] = {};
	const uint8_t  three[3]           = {2, 1, 3};   // nearest first
	Check(AssignBoardingSeats(stretch, 0, three, 3, seats) == 2 && seats[2] == 1 &&
	          seats[1] == 3 && seats[3] == 0,
	      "three participants and two free seats: the two nearest sit, the third follows");
	uint8_t reef[MAX_PLAYERS] = {};
	Check(AssignBoardingSeats(1u << 1, 0, three, 3, reef) == 1 && reef[2] == 1 && reef[1] == 0,
	      "the Reefer has one passenger seat, for the nearest");
	uint8_t kept[MAX_PLAYERS] = {};
	Check(AssignBoardingSeats(stretch, 1, three, 3, kept) == 1 && kept[2] == 3,
	      "a seat the mission's pedestrian is on the way to is kept back for her");
	uint8_t none[MAX_PLAYERS] = {};
	Check(AssignBoardingSeats(0, 0, three, 3, none) == 0 && none[1] == 0 && none[2] == 0,
	      "a full car gives nobody anything");

	Check(PickBoardSeat((1u << 1) | (1u << 3), 3, 1u << 1) == 3, "a participant takes his own seat");
	Check(PickBoardSeat((1u << 1) | (1u << 2), 3, 1u << 1) == 2,
	      "or one free here that nobody else was given, when his is not free here");
	Check(PickBoardSeat(1u << 1, 3, 1u << 1) == 0, "and never somebody else's");
	Check(PickBoardSeat(0xFFFF, 0, 0) == 0, "no seat handed out is no seat");

	MissionSync owner = Fresh();   // alice, 0
	const uint8_t handed[MAX_PLAYERS] = {0, 3, 1, 0};
	owner.Board(700, handed, 0, 0, 1000);
	Check(SentCount<C_MissionBoard>() == 0, "nothing is handed out with no session's mission");
	owner.OnState(State(MISSION_STATE_RUNNING, 0, 36, 0x07), 0, 1000);
	owner.Board(700, handed, MISSION_BOARD_WATER, 0, 1000);
	const C_MissionBoard *said = LastSent<C_MissionBoard>();
	Check(said && said->netId == 700 && said->missionNumber == 36 && said->seats[1] == 3 &&
	          said->seats[2] == 1 && said->seats[3] == 0 && said->flags == MISSION_BOARD_WATER,
	      "the owner hands the seats out, one each");
	Check(g_roster.feed.empty(), "and says so in the log alone");

	S_MissionBoard board{};
	InitHeader(board, 1000);
	board.ownerId       = 0;
	board.missionNumber = 36;
	board.netId         = 700;
	board.flags         = 0;
	board.seats[1]      = 3;
	board.seats[2]      = 1;
	MissionSync bob = Fresh();   // bob, 1
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 36, 0x07), 1, 1000);
	uint16_t netId = 0, others = 0;
	uint8_t  seat  = 0;
	uint32_t since = 0;
	board.ownerId = 2;
	bob.OnBoard(board, 1, 1000);
	Check(!bob.BoardPending(netId, seat, others, since), "only the owner hands out seats");
	board.ownerId = 0;
	bob.OnBoard(board, 1, 1100);
	Check(bob.BoardPending(netId, seat, others, since) && netId == 700 && seat == 3 &&
	          others == (1u << 1) && since == 1100,
	      "bob is to take seat 3, and knows carol has seat 1");
	bob.BoardDone();
	Check(!bob.BoardPending(netId, seat, others, since), "and once taken it is done");

	MissionSync carol = Fresh();   // carol, 2
	carol.OnState(State(MISSION_STATE_RUNNING, 0, 36, 0x07), 2, 1000);
	board.seats[2] = 0;
	board.flags    = MISSION_BOARD_WATER;
	carol.OnBoard(board, 2, 1100);
	Check(!carol.BoardPending(netId, seat, others, since) &&
	          LastLine().find("no seat left for you") != std::string::npos,
	      "carol, with no seat in the boat, is told so in the log and stays put");
	bob.OnBoard(board, 1, 1200);
	bob.OnState(State(MISSION_STATE_IDLE, 0, 36, 0, MISSION_OUTCOME_PASSED), 1, 1300);
	Check(!bob.BoardPending(netId, seat, others, since), "and nothing is left once the mission ends");
}

void TestTheWaterDoesNotWaitForTheBoatless() {
	std::printf("\na checkpoint on the water does not wait for somebody with no boat\n");
	Check(game::CheckpointOutOfReach(true, false), "the owner on the water, a participant ashore");
	Check(!game::CheckpointOutOfReach(true, true), "one with a boat of his own is waited for");
	Check(!game::CheckpointOutOfReach(false, false), "and on land everybody is");

	MissionSync m = Fresh();   // alice, 0, the owner
	m.OnState(State(MISSION_STATE_RUNNING, 0, 73, 0x07), 0, 1000);
	const MissionArea cp = MissionAreaLocate2D(500.0f, 500.0f, 15.0f, 15.0f);
	g_roster.others = {{1, true, {502.0f, 500.0f, 0.0f}}, {2, true, {800.0f, 500.0f, 0.0f}}};
	Check(!m.AskCheckpoint(cp, 0, 2000), "carol is ashore, 300 m off, so the platform waits");
	m.SetOutOfReach(PlayerBit(2), 0);
	Check(LastLine().find("don't wait for carol") != std::string::npos, "until she has no boat");
	Check(m.AskCheckpoint(cp, 0, 2100), "and then it goes on without her");
	m.SetOutOfReach(PlayerBit(3), 0);
	Check(m.OutOfReach() == 0, "somebody who is not in the mission is nobody to leave behind");
	m.SetOutOfReach(0, 0);
	Check(!m.AskCheckpoint(cp, 0, 2200), "on land again, she is waited for");
	MissionSync bob = Fresh();
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 73, 0x07), 1, 1000);
	bob.SetOutOfReach(PlayerBit(2), 1);
	Check(bob.OutOfReach() == 0, "only the owner's checkpoints have anybody out of reach");
}

void TestAnObjectBrokenAnywhereIsBrokenEverywhere() {
	std::printf("\none of the mission's objects, broken on somebody's machine\n");
	static std::vector<std::pair<uint16_t, uint8_t>> broken;
	broken.clear();
	MissionSync bob = Fresh();   // bob, 1, who drives through a stall
	g_bridge.BreakObject = [](uint16_t global, float, uint8_t state) {
		broken.emplace_back(global, state);
	};
	bob.ObjectBroken(0x0B20, 500.0f, OBJ_BREAK_SMASHED, 1000);
	Check(SentCount<C_MissionObjectBreak>() == 0, "nothing is said with no session's mission");
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 60, 0x07), 1, 1000);
	bob.ObjectBroken(0x0B20, 500.0f, OBJ_BREAK_SMASHED, 1100);
	const C_MissionObjectBreak *said = LastSent<C_MissionObjectBreak>();
	Check(said && said->global == 0x0B20 && said->state == OBJ_BREAK_SMASHED &&
	          said->amount == 500.0f && said->missionNumber == 60,
	      "a stall bob smashes goes out named by the global that holds it everywhere");

	MissionSync alice = Fresh();   // alice, 0, whose mission counts the stalls
	g_bridge.BreakObject = [](uint16_t global, float, uint8_t state) {
		broken.emplace_back(global, state);
	};
	alice.OnState(State(MISSION_STATE_RUNNING, 0, 60, 0x07), 0, 1000);
	S_MissionObjectBreak in{};
	InitHeader(in, 1000);
	in.playerId      = 1;
	in.missionNumber = 60;
	in.global        = 0x0B20;
	in.state         = OBJ_BREAK_SMASHED;
	in.amount        = 500.0f;
	alice.OnObjectBroken(in, 0);
	Check(broken.size() == 1 && broken[0].first == 0x0B20 && broken[0].second == OBJ_BREAK_SMASHED &&
	          alice.ObjectBreaksApplied() == 1,
	      "and alice's own stall in that global breaks as far, for alice's mission to see");
	in.playerId = 0;
	alice.OnObjectBroken(in, 0);
	Check(broken.size() == 1, "our own break never comes back to be run twice");
	in.playerId      = 1;
	in.missionNumber = 61;
	alice.OnObjectBroken(in, 0);
	Check(broken.size() == 1, "nor one of another mission's objects");
	in.missionNumber = 60;
	in.amount        = std::nanf("");
	alice.OnObjectBroken(in, 0);
	Check(broken.size() == 1, "nor an amount that is not a number");
	alice.OnState(State(MISSION_STATE_IDLE, 0, 60, 0, MISSION_OUTCOME_PASSED), 0, 9000);
	in.amount = 500.0f;
	alice.OnObjectBroken(in, 0);
	Check(broken.size() == 1, "nor anything once the mission is over");
}

void TestAHelpersGarageIsHeard() {
	std::printf("\na helper's garage, for the owner's mission\n");
	MissionSync bob = Fresh();   // bob, 1
	bob.Answers(1u << 7, 0, 0, 1, 1000);
	Check(SentCount<C_MissionAnswers>() == 0, "nothing is said with no session's mission");
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 20, 0x07), 1, 1000);
	bob.Answers(0, 0, 0, 1, 1100);
	Check(SentCount<C_MissionAnswers>() == 0, "nor while bob's garages have nothing to say");
	bob.Answers(1u << 7, 0, 0, 1, 1200);
	const C_MissionAnswers *said = LastSent<C_MissionAnswers>();
	Check(said && said->hasCar == (1u << 7) && said->missionNumber == 20,
	      "bob's lock-up shutting on the car is said");
	bob.Answers(1u << 7, 0, 0, 1, 1300);
	Check(SentCount<C_MissionAnswers>() == 1, "once");
	bob.Answers(1u << 7, 1u << 3, 0, 1, 1400);
	said = LastSent<C_MissionAnswers>();
	Check(SentCount<C_MissionAnswers>() == 2 && said->resprayed == (1u << 3),
	      "and a respray at his Pay'n'Spray the moment it happens");
	bob.Answers(1u << 7, 0, MISSION_SHOT_DOWN_DRUG_PLANE, 1, 1500);
	said = LastSent<C_MissionAnswers>();
	Check(SentCount<C_MissionAnswers>() == 3 && said->shotDown == MISSION_SHOT_DOWN_DRUG_PLANE,
	      "and his rocket bringing down his copy of the Cessna");

	MissionSync alice = Fresh();   // alice, 0, the owner
	alice.OnState(State(MISSION_STATE_RUNNING, 0, 20, 0x07), 0, 1000);
	alice.Answers(1u << 7, 0, 0, 0, 1100);
	Check(SentCount<C_MissionAnswers>() == 0 && !alice.GarageHasCarElsewhere(7),
	      "the owner's own garages say nothing, and nobody else's has the car yet");
	S_MissionAnswers in{};
	InitHeader(in, 1000);
	in.playerId      = 1;
	in.missionNumber = 20;
	in.hasCar        = 1u << 7;
	in.resprayed     = 1u << 3;
	alice.OnAnswers(in, 0);
	Check(alice.GarageHasCarElsewhere(7) && !alice.GarageHasCarElsewhere(6),
	      "bob's garage has the car, for alice's mission");
	Check(alice.TakeResprayElsewhere(3) && !alice.TakeResprayElsewhere(3),
	      "and his respray says yes once, to the first question");
	in.resprayed = 0;
	in.shotDown  = MISSION_SHOT_DOWN_DRUG_PLANE;
	alice.OnAnswers(in, 0);
	Check(!alice.TakeShotDownElsewhere(MISSION_SHOT_DOWN_DROP_OFF) &&
	          alice.TakeShotDownElsewhere(MISSION_SHOT_DOWN_DRUG_PLANE) &&
	          !alice.TakeShotDownElsewhere(MISSION_SHOT_DOWN_DRUG_PLANE),
	      "the Cessna bob's rocket brought down is down for alice's mission, once");
	in.resprayed = 1u << 3;
	in.shotDown  = 0;
	alice.OnState(State(MISSION_STATE_RUNNING, 0, 20, 0x05), 0, 1500);
	Check(!alice.GarageHasCarElsewhere(7), "not once bob is out of the mission");
	in.missionNumber = 21;
	alice.OnAnswers(in, 0);
	in.missionNumber = 20;
	in.playerId      = 0;
	alice.OnAnswers(in, 0);
	Check(!alice.TakeResprayElsewhere(3), "nor from another mission, nor alice's own");
	alice.OnState(State(MISSION_STATE_RUNNING, 0, 20, 0x07), 0, 1600);
	in.playerId = 1;
	alice.OnAnswers(in, 0);
	alice.OnState(State(MISSION_STATE_IDLE, 0, 20, 0, MISSION_OUTCOME_PASSED), 0, 9000);
	Check(!alice.GarageHasCarElsewhere(7) && !alice.TakeResprayElsewhere(3),
	      "and nothing is left of it once the mission is over");
}

void TestTheMissionsWordToACar() {
	std::printf("\nthe mission's word to a car, where the car is simulated\n");
	using namespace game::replay;
	std::vector<uint8_t> space(0x400, 0);
	const int32_t car = 0x00070003;   // $CAR_EIGHTBALL
	std::memcpy(space.data() + 0xC0, &car, 4);
	const auto carNamed = [](const Encoded &e, int32_t want) {
		uint8_t code[MAX_CODE];
		std::memcpy(code, e.code, e.length);
		int32_t seen = -1;
		EachHandle(code, e.length, [&](Arg a, int32_t *v) {
			if (a == Arg::Car && seen < 0)
				seen = *v;
			return true;
		});
		return seen == want;
	};
	// SET_CAR_COORDINATES $CAR_EIGHTBALL 820.875 -941.0625 35.0
	const uint8_t coords[] = {0x02, 0xC0, 0x00, 0x06, 0x4E, 0x33, 0x06, 0x2F, 0xC5, 0x06, 0x30, 0x02};
	std::memcpy(space.data() + 0x200, coords, sizeof coords);
	Encoded e;
	Check(Encode(0x00AB, space.data(), 0x400, 0x200, nullptr, &e) && e.kind == Kind::Holder &&
	          carNamed(e, car),
	      "a car moved goes to whoever simulates it, named by the session");
	// SET_CAR_HEADING $CAR_EIGHTBALL 262.375, SET_CAR_HEALTH $CAR_EIGHTBALL 2000
	const uint8_t heading[] = {0x02, 0xC0, 0x00, 0x06, 0x66, 0x10};
	std::memcpy(space.data() + 0x220, heading, sizeof heading);
	const uint8_t health[] = {0x02, 0xC0, 0x00, 0x05, 0xD0, 0x07};
	std::memcpy(space.data() + 0x240, health, sizeof health);
	Check(Encode(0x0175, space.data(), 0x400, 0x220, nullptr, &e) && e.kind == Kind::Holder &&
	          Encode(0x0224, space.data(), 0x400, 0x240, nullptr, &e) && e.kind == Kind::Holder &&
	          carNamed(e, car),
	      "and so does one turned, or given its health");
	// LOCK_CAR_DOORS $CAR_EIGHTBALL CARLOCK_LOCKED
	const uint8_t lock[] = {0x02, 0xC0, 0x00, 0x04, 0x02};
	std::memcpy(space.data() + 0x260, lock, sizeof lock);
	Check(Encode(0x020A, space.data(), 0x400, 0x260, nullptr, &e) && e.kind == Kind::Plain &&
	          carNamed(e, car),
	      "a car's doors are locked on everybody's copy, where each player tries them");
	// CHANGE_CAR_LOCK, SET_CAR_ONLY_DAMAGED_BY_PLAYER and SET_CAR_STRONG on
	// $CAR_EIGHTBALL with 1
	const uint8_t oneMore[] = {0x02, 0xC0, 0x00, 0x04, 0x01};
	std::memcpy(space.data() + 0x270, oneMore, sizeof oneMore);
	bool allPlain = true;
	for (const uint16_t op : {uint16_t{0x0135}, uint16_t{0x02AA}, uint16_t{0x03AB}})
		allPlain = allPlain && Encode(op, space.data(), 0x400, 0x270, nullptr, &e) &&
		           e.kind == Kind::Plain && carNamed(e, car) && Find(op)->count == 2;
	Check(allPlain,
	      "the other door lock, and what the car stands up to, are on every copy too, "
	      "whoever ends up simulating it");
	Check(!Listed(0x02AC),
	      "but not SET_CAR_PROOFS, whose collision bit is the one a copy's observer "
	      "keeps for itself");
	// APPLY_BRAKES_TO_PLAYERS_CAR $PLAYER_CHAR 1
	const int32_t player = 0;
	std::memcpy(space.data() + 0xC4, &player, 4);
	const uint8_t brakes[] = {0x02, 0xC4, 0x00, 0x04, 0x01};
	std::memcpy(space.data() + 0x280, brakes, sizeof brakes);
	int32_t who = -1, on = -1;
	Check(Encode(0x0221, space.data(), 0x400, 0x280, nullptr, &e) && e.kind == Kind::Plain &&
	          LiteralAt(e.code, e.length, 0, &who) && who == 0 && LiteralAt(e.code, e.length, 1, &on) &&
	          on == 1,
	      "and the brakes go on for every participant's own player, the car they drive");
	// ARM_CAR_WITH_BOMB $CAR_EIGHTBALL CARBOMB_TIMED, SET_FREE_BOMB_SHOP 1
	const uint8_t arm[] = {0x02, 0xC0, 0x00, 0x04, 0x01};
	std::memcpy(space.data() + 0x2A0, arm, sizeof arm);
	const uint8_t free[] = {0x04, 0x01};
	std::memcpy(space.data() + 0x2C0, free, sizeof free);
	Check(Encode(0x0242, space.data(), 0x400, 0x2A0, nullptr, &e) && e.kind == Kind::Plain &&
	          carNamed(e, car) && Encode(0x021D, space.data(), 0x400, 0x2C0, nullptr, &e) &&
	          e.kind == Kind::Plain,
	      "a bomb the mission fits is on every copy of the car, and a free bomb shop is "
	      "everybody's");
	// SET_TARGET_CAR_FOR_MISSION_GARAGE $LUIGIS_LOCKUP_GARAGE $CAR_EIGHTBALL,
	// OPEN_GARAGE $LUIGIS_LOCKUP_GARAGE
	const int32_t garage = 7;
	std::memcpy(space.data() + 0xC8, &garage, 4);
	const uint8_t target[] = {0x02, 0xC8, 0x00, 0x02, 0xC0, 0x00};
	std::memcpy(space.data() + 0x2E0, target, sizeof target);
	const uint8_t open[] = {0x02, 0xC8, 0x00};
	std::memcpy(space.data() + 0x300, open, sizeof open);
	int32_t which = -1;
	Check(Encode(0x021B, space.data(), 0x400, 0x2E0, nullptr, &e) && e.kind == Kind::Plain &&
	          carNamed(e, car) && LiteralAt(e.code, e.length, 0, &which) && which == 7 &&
	          Encode(0x0360, space.data(), 0x400, 0x300, nullptr, &e) && e.kind == Kind::Plain &&
	          Listed(0x0361) && Listed(0x02B9) && Listed(0x03A5),
	      "a mission garage takes its car, and opens and closes, on every machine, where "
	      "main.scm made the same garages in the same order");
	Check(Listed(0x033A) && Find(0x033A)->kind == Kind::Plain && Find(0x033A)->count == 0 &&
	          Listed(0x0358) && !Listed(0x033C) && !Listed(0x0359),
	      "both mission Cessnas take off on every machine, and whether one is down is asked, "
	      "not shown");
}

void TestAFloatingPackageTakenAnywhereCounts() {
	std::printf("\nA Drop In The Ocean's packages, taken on anybody's machine\n");
	using namespace game;
	// $FLOAT_PACKGE_01 = CREATE_FLOATING_PACKAGE $PLANEX $PLANEY $PLANEZ, from
	// globals, as the mission writes it.
	std::vector<uint8_t> space(0x200, 0);
	const float plane[3] = {-1270.5f, 55.25f, 60.0f};
	std::memcpy(&space[0x40], &plane[0], 4);
	std::memcpy(&space[0x44], &plane[1], 4);
	std::memcpy(&space[0x48], &plane[2], 4);
	const uint8_t operands[] = {0x02, 0x40, 0x00, 0x02, 0x44, 0x00, 0x02, 0x48, 0x00, 0x02, 0x60, 0x00};
	std::memcpy(&space[0x100], operands, sizeof operands);
	replay::Encoded enc;
	const bool      read = replay::Encode(0x035B, space.data(), uint32_t(space.size()), 0x100, nullptr, &enc);
	int32_t         x = 0, y = 0, z = 0;
	Check(read && enc.kind == replay::Kind::PickupNew && replay::LiteralAt(enc.code, enc.length, 0, &x) &&
	          replay::LiteralAt(enc.code, enc.length, 1, &y) && replay::LiteralAt(enc.code, enc.length, 2, &z) &&
	          std::memcmp(&x, &plane[0], 4) == 0 && std::memcmp(&z, &plane[2], 4) == 0 &&
	          enc.code[enc.length - 3] == 0x03,
	      "a package is made on every machine where the owner's plane dropped it");

	static std::vector<int32_t> taken;
	taken.clear();
	MissionSync bob = Fresh();   // bob, 1, whose boat went over one
	bob.PickupTaken(0x2A0007, 1000);
	Check(SentCount<C_MissionPickup>() == 0, "nothing is said with no session's mission");
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 64, 0x07), 1, 1000);
	bob.PickupTaken(0x2A0007, 1100);
	const C_MissionPickup *said = LastSent<C_MissionPickup>();
	Check(said && said->handle == 0x2A0007 && said->missionNumber == 64,
	      "the one bob takes is said by the owner's handle for it");

	MissionSync alice = Fresh();   // alice, 0, whose mission counts them
	g_bridge.TakePickup = [](int32_t handle) { taken.push_back(handle); };
	alice.OnState(State(MISSION_STATE_RUNNING, 0, 64, 0x07), 0, 1000);
	S_MissionPickup in{};
	InitHeader(in, 1000);
	in.playerId      = 1;
	in.missionNumber = 64;
	in.handle        = 0x2A0007;
	alice.OnPickupTaken(in, 0);
	Check(taken.size() == 1 && taken[0] == 0x2A0007, "and alice's own is taken for alice's mission to count");
	in.playerId = 0;
	alice.OnPickupTaken(in, 0);
	in.playerId      = 1;
	in.missionNumber = 65;
	alice.OnPickupTaken(in, 0);
	Check(taken.size() == 1, "never our own again, nor another mission's");
	alice.OnState(State(MISSION_STATE_IDLE, 0, 64, 0, MISSION_OUTCOME_FAILED), 0, 9000);
	in.missionNumber = 64;
	alice.OnPickupTaken(in, 0);
	Check(taken.size() == 1, "nor once the mission is over");
}

void TestAGameInAMissionOfItsOwn() {
	std::printf("\na game in a mission of its own\n");
	MissionSync dave = Fresh();   // dave, 3, whose new game plays its intro
	dave.SetBusy(true, 500);
	Check(SentCount<C_MissionBusy>() == 0, "nothing is said before the server says it shares missions");
	dave.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 3, 600);
	const C_MissionBusy *said = LastSent<C_MissionBusy>();
	Check(said && said->busy == 1 && said->fresh == 0, "and then it is");
	dave.SetBusy(true, 700);
	dave.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 3, 800);
	Check(SentCount<C_MissionBusy>() == 1, "once");
	dave.SetBusy(false, 900);
	said = LastSent<C_MissionBusy>();
	Check(SentCount<C_MissionBusy>() == 2 && said->busy == 0, "and so is the end of it");

	dave.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x0F), 3, 1000);
	Check(g_mirror, "in alice's mission, dave's $ONMISSION follows it");
	dave.SetBusy(true, 1100);
	Check(SentCount<C_MissionBusy>() == 3, "a game going into a mission of its own says so at once");
	S_MissionEffect fx;
	InitHeader(fx, 1100);
	fx.ownerId            = 0;
	fx.body.missionNumber = 19;
	fx.body.kind          = MISSION_EFFECT_RUN;
	fx.body.length        = 2;
	fx.body.readySeq      = 5;
	dave.OnEffect(fx, 3, false, 1100);
	static int widgets = 0, breaks = 0, pickups = 0;
	widgets = breaks = pickups = 0;
	g_bridge.SetWidget   = [](uint16_t, int32_t) { ++widgets; };
	g_bridge.BreakObject = [](uint16_t, float, uint8_t) { ++breaks; };
	g_bridge.TakePickup  = [](int32_t) { ++pickups; };
	S_MissionWidget w{};
	InitHeader(w, 1100);
	w.ownerId = 0;
	w.offset  = 0x144;
	w.value   = 30000;
	dave.OnWidget(w, 3);
	S_MissionObjectBreak br{};
	InitHeader(br, 1100);
	br.playerId      = 1;
	br.missionNumber = 19;
	br.global        = 0x0B20;
	br.amount        = 500.0f;
	br.state         = OBJ_BREAK_SMASHED;
	dave.OnObjectBroken(br, 3);
	S_MissionPickup pk{};
	InitHeader(pk, 1100);
	pk.playerId      = 1;
	pk.missionNumber = 19;
	pk.handle        = 0x2A0007;
	dave.OnPickupTaken(pk, 3);
	Check(g_effects == 0 && widgets == 0 && breaks == 0 && pickups == 0 &&
	          SentCount<C_MissionReady>() == 0,
	      "and from then on nothing of alice's mission runs in it, even before the server has "
	      "taken it out");
	dave.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x07), 3, 1200);
	Check(!g_mirror, "taken out, its $ONMISSION is its own mission's again");
	dave.StartedOver(3, 1250);
	Check(SentCount<C_MissionBusy>() == 3, "a new game out of alice's mission asks for nothing");
	dave.SetBusy(false, 1300);
	dave.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x0F), 3, 1400);
	Check(g_mirror && LastLine().find("alice is on") == 0 &&
	          LastLine().find("and you are in it now") != std::string::npos,
	      "out of its own mission, dave is in alice's, and told so");
	dave.OnEffect(fx, 3, false, 1500);
	Check(g_effects == 1, "which dave's game shows from then on");

	const size_t before = SentCount<C_MissionBusy>();
	dave.StartedOver(3, 2000);
	said = LastSent<C_MissionBusy>();
	Check(SentCount<C_MissionBusy>() == before + 1 && said->busy == 0 && said->fresh == 1,
	      "dave loading a save in the middle of it asks alice for what it has up");
	MissionSync alice = Fresh();   // alice, 0, the owner
	static std::vector<uint8_t> handed;
	handed.clear();
	g_bridge.ResendStanding = [](uint8_t id) { handed.push_back(id); };
	alice.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x0F), 0, 1000);
	alice.StartedOver(0, 1100);
	Check(SentCount<C_MissionBusy>() == 0, "the owner starting over asks nobody for anything");
	S_MissionHandOver over{};
	InitHeader(over, 2000);
	over.playerId      = 3;
	over.missionNumber = 19;
	alice.OnHandOver(over, 0, 2000);
	Check(handed.size() == 1 && handed[0] == 3 && alice.StandingHandedOver() == 1,
	      "alice hands dave what the mission has up again");
	over.missionNumber = 20;
	alice.OnHandOver(over, 0, 2100);
	over.missionNumber = 19;
	over.playerId      = 0;
	alice.OnHandOver(over, 0, 2200);
	Check(handed.size() == 1, "not for another mission, nor to alice");
	MissionSync bob = Fresh();   // bob, 1, not the owner
	g_bridge.ResendStanding = [](uint8_t id) { handed.push_back(id); };
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x0F), 1, 1000);
	over.playerId = 3;
	bob.OnHandOver(over, 1, 2000);
	Check(handed.size() == 1, "and nobody but the owner hands anything over");

	MissionSync carol = Fresh();   // carol, 2
	Check(!carol.OwnLaunchIsTheSessions(2),
	      "a mission carol's game launched before the server shared missions is its own");
	carol.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 2, 500);
	carol.Launched(0x4242, 24, 600);
	Check(carol.OwnLaunchIsTheSessions(2), "one it launched may be the session's until the server says");
	carol.OnState(State(MISSION_STATE_RUNNING, 2, 24, 0x0F), 2, 700);
	Check(carol.OwnLaunchIsTheSessions(2), "and is, once it has");
	carol.Clear();
	carol.OnState(State(MISSION_STATE_IDLE, 2, 24, 0, MISSION_OUTCOME_OWNER_LEFT), 2, 900);
	Check(carol.OwnLaunchIsTheSessions(2) && LastSent<C_MissionStarted>() &&
	          LastSent<C_MissionStarted>()->missionNumber == 24,
	      "with carol's connection dropped and back, it is offered to the session again");
	carol.OnState(State(MISSION_STATE_RUNNING, 1, 30, 0x0B), 2, 950);
	Check(!carol.OwnLaunchIsTheSessions(2),
	      "but not once bob's mission has taken the session instead");
	MissionSync late = Fresh();   // carol, 2, while dave's is the session's
	late.OnState(State(MISSION_STATE_RUNNING, 3, 25, 0x0F), 2, 500);
	late.Launched(0x4343, 24, 600);
	Check(!late.OwnLaunchIsTheSessions(2), "nor one launched while somebody else's is the session's");
}

// Two games started a new game together (the owner's first run, 2026-09-24):
// each played its intro, and both then sat on the loading screen for good.
// Every game said "in a mission of its own" on its first frame and never
// "out", because the witness read the byte 00D7 sets on seven of main.scm's
// own threads. The running list below is the one a new game has once its
// intro is over: MAIN, the seven threads INIT_THREADS starts with 00D7, and
// the Eightball trigger the intro starts on its way out.
void TestTwoNewGamesMeetAtTheBridge() {
	std::printf("\ntwo new games meet at the bridge\n");
	using namespace game::scripts::layout;
	constexpr size_t SCRIPT_BYTES = 0xB0;   // III.CLEO's, the longer of the two
	std::vector<std::vector<uint8_t>> scripts(10, std::vector<uint8_t>(SCRIPT_BYTES, 0));
	const auto link = [&](size_t count) {
		for (size_t i = 0; i < count; ++i) {
			const uintptr_t next =
			    i + 1 < count ? reinterpret_cast<uintptr_t>(scripts[i + 1].data()) : 0;
			std::memcpy(scripts[i].data() + SCRIPT_NEXT, &next, sizeof next);
		}
		return reinterpret_cast<uintptr_t>(scripts[0].data());
	};
	for (size_t i = 1; i <= 7; ++i)
		scripts[i][SCRIPT_MISSION_RULES] = 1;   // HJ, USJ, GENSTUF, RAMPAGE, IMPORT, CAMERA, GATES
	Check(!game::AnyMissionSlotScript(link(9)),
	      "past the intro, MAIN, its seven 00D7 threads and the Eightball trigger are in no mission");
	scripts[9][SCRIPT_MISSION_RULES] = 1;
	scripts[9][SCRIPT_MISSION_SLOT]  = 1;
	Check(game::AnyMissionSlotScript(link(10)), "while the intro START_MISSION launched runs, one is");
	Check(!game::AnyMissionSlotScript(0) && !game::IsMissionSlotScript(nullptr),
	      "and an empty list has none");

	// noxx3, 0, out of its intro first; noxx2, 1, still in theirs.
	constexpr uint32_t trigger = 0xA242;   // the Eightball trigger's 03EE in the retail main.scm
	MissionSync        noxx3 = Fresh();
	g_roster.nicks[0] = "noxx3";
	g_roster.nicks[1] = "noxx2";
	noxx3.SetBusy(true, 400);
	noxx3.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 500);
	noxx3.SetBusy(false, 900);
	const C_MissionBusy *out = LastSent<C_MissionBusy>();
	Check(out && out->busy == 0 && SentCount<C_MissionBusy>() == 2,
	      "noxx3's game says it is in its intro, and then that it is out");
	Check(!noxx3.AskStartGate(trigger, MISSION_KIND_STORY, 19, kMarker, 0, 1000) &&
	          SentCount<C_MissionClaim>() == 1 && !noxx3.StartHeld(trigger, 1000),
	      "its trigger claims Give Me Liberty, and nothing is shown for it before the answer");
	noxx3.OnClaim(Answer(trigger, MISSION_CLAIM_WAITING, 0, PlayerBit(1)), 0, 1050);
	Check(noxx3.StartHeld(trigger, 1100) && !noxx3.StartHeld(0x9999, 1100),
	      "WAITING holds that gate, and no other, so the world is shown for the wait");
	S_MissionWaiting wait = Waiting(0, PlayerBit(1), MISSION_WAIT_START);
	wait.busyMask         = PlayerBit(1);
	wait.goesOnInS        = 60;
	noxx3.OnWaiting(wait, 0, 1100);
	Check(LastLine() == "waiting for noxx2, still in a cutscene of their own - it starts without "
	                    "them in 60 s" &&
	          noxx3.WaitingInOwnMission() == PlayerBit(1),
	      "and its log says who it waits for, why, and for how long");

	MissionSync noxx2 = Fresh();
	g_roster.nicks[0] = "noxx3";
	g_roster.nicks[1] = "noxx2";
	noxx2.SetBusy(true, 400);
	noxx2.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	noxx2.OnWaiting(wait, 1, 1100);
	Check(LastLine() == "noxx3 is waiting at the start of Give Me Liberty for your cutscene to end "
	                    "- it starts without you in 60 s",
	      "noxx2's log says that noxx3 waits for its intro");
	noxx2.SetBusy(false, 1800);
	noxx2.AskStartGate(trigger, MISSION_KIND_STORY, 19, kMarker, 1, 1850);
	noxx2.OnClaim(Answer(trigger, MISSION_CLAIM_BUSY, 0), 1, 1900);
	Check(!noxx2.AskStartGate(trigger, MISSION_KIND_STORY, 19, kMarker, 1, 1950) &&
	          noxx2.StartHeld(trigger, 1950),
	      "out of its intro, noxx2's own trigger is told noxx3's claim has the start, and waits "
	      "in view");

	noxx3.OnClaim(Answer(trigger, MISSION_CLAIM_GRANTED, 0), 0, 2000);
	Check(noxx3.AskStartGate(trigger, MISSION_KIND_STORY, 19, kMarker, 0, 2050) &&
	          !noxx3.StartHeld(trigger, 2050),
	      "noxx3's next claim finds noxx2 there: granted, and Give Me Liberty starts there once");
	noxx2.OnState(State(MISSION_STATE_RUNNING, 0, 19, 0x03), 1, 2100);
	Check(g_mirror && !noxx2.StartHeld(trigger, 2150) &&
	          !noxx2.AskStartGate(trigger, MISSION_KIND_STORY, 19, kMarker, 1, 2150),
	      "noxx2 is in it, its $ONMISSION follows it, and its trigger never runs a second copy");

	MissionSync waiting = Fresh();   // noxx3's log, as the wait goes on
	g_roster.nicks[1] = "noxx2";
	waiting.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 0, 500);
	waiting.OnWaiting(wait, 0, 1100);
	waiting.Tick(0, 1100 + MISSION_WAIT_REMIND_MS);
	Check(LastLine().find("in 45 s") != std::string::npos, "counting down while it lasts");
	S_MissionWaiting away = wait;
	away.missingMask      = PlayerBit(1) | PlayerBit(2);
	away.goesOnInS        = 0;
	waiting.OnWaiting(away, 0, 20000);
	Check(LastLine() == "waiting for carol to come to the start, and for noxx2, still in a cutscene "
	                    "of their own",
	      "and somebody who is only away is waited for with no end said");
}

void TestEverybodysKillsCount() {
	std::printf("\neverybody's kills count for the owner's mission\n");
	MissionSync bob = Fresh();   // bob, 1
	bob.KillRegistered(13, 1, 1000);
	Check(SentCount<C_MissionKill>() == 0 && !bob.CreditsRemoteKills(1),
	      "with no session's mission nothing is said, and nobody else's kill is bob's");
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 64, 0x07), 1, 1000);
	Check(bob.CreditsRemoteKills(1),
	      "in alice's mission, a kill somebody else makes of a pedestrian bob's machine hosts counts");
	bob.KillRegistered(13, 1, 1100);
	const C_MissionKill *said = LastSent<C_MissionKill>();
	Check(said && said->model == 13 && said->missionNumber == 64,
	      "and a Diablo bob's machine registers goes to alice's mission");
	bob.SetBusy(true, 1200);
	bob.KillRegistered(13, 1, 1300);
	Check(SentCount<C_MissionKill>() == 1 && !bob.CreditsRemoteKills(1),
	      "but not while bob's game is in a mission of its own");

	MissionSync carol = Fresh();   // carol, 2, not in it
	carol.OnState(State(MISSION_STATE_RUNNING, 0, 64, 0x03), 2, 1000);
	carol.KillRegistered(13, 2, 1100);
	Check(SentCount<C_MissionKill>() == 0 && !carol.CreditsRemoteKills(2),
	      "nor from somebody who is not in the mission");

	static std::vector<uint16_t> counted;
	counted.clear();
	MissionSync alice = Fresh();   // alice, 0, the owner
	g_bridge.CountKill = [](uint16_t model) { counted.push_back(model); };
	alice.OnState(State(MISSION_STATE_RUNNING, 0, 64, 0x07), 0, 1000);
	alice.KillRegistered(13, 0, 1100);
	Check(alice.CreditsRemoteKills(0) && SentCount<C_MissionKill>() == 0,
	      "alice's machine counts the kills it registers where they are, and says nothing");
	S_MissionKill in{};
	InitHeader(in, 1000);
	in.playerId      = 1;
	in.missionNumber = 64;
	in.model         = 13;
	alice.OnKill(in, 0);
	Check(counted.size() == 1 && counted[0] == 13 && alice.KillsCounted() == 1,
	      "bob's kill counts for alice's mission, on alice's machine");
	in.missionNumber = 65;
	alice.OnKill(in, 0);
	in.missionNumber = 64;
	in.playerId      = 0;
	alice.OnKill(in, 0);
	Check(counted.size() == 1, "not one of another mission's, nor alice's own come back");
	in.playerId = 1;
	bob.OnKill(in, 1);
	alice.OnState(State(MISSION_STATE_IDLE, 0, 64, 0, MISSION_OUTCOME_PASSED), 0, 9000);
	alice.OnKill(in, 0);
	Check(counted.size() == 1 && !alice.CreditsRemoteKills(0),
	      "nobody but the owner counts one, and nobody once the mission is over");
}

void TestACopyCountsLikeTheOriginal() {
	std::printf("\nmore enemies, that count like the originals\n");
	using namespace game;
	Check(MayCopyPedType(7) && MayCopyPedType(15) && MayCopyPedType(4),
	      "gang members and hired guns get copies");
	Check(!MayCopyPedType(0) && !MayCopyPedType(6) && !MayCopyPedType(21) && !MayCopyPedType(16),
	      "the player, a cop, a medic and a special character never do");

	MissionSync m = Fresh();   // alice, 0
	Check(m.EnemyCopies() == 0, "no copies with no mission");
	S_MissionState st = State(MISSION_STATE_RUNNING, 0, 30, 0x07);
	st.enemies        = MISSION_ENEMIES_TOUGHER;
	m.OnState(st, 0, 1000);
	Check(m.EnemyCopies() == 0, "nor under `tougher`");
	st.enemies = MISSION_ENEMIES_MORE;
	m.OnState(st, 0, 1100);
	Check(m.EnemyCopies() == 2, "under `more`, three players get two copies of each enemy");
	st.participants = 0x01;
	m.OnState(st, 0, 1200);
	Check(m.EnemyCopies() == 0, "one player on their own gets none");
	st.participants = 0xFF;
	m.OnState(st, 0, 1300);
	Check(m.EnemyCopies() == MISSION_ENEMY_COPIES_MAX, "and eight get MISSION_ENEMY_COPIES_MAX");

	EnemyGroups g;
	Check(g.Add(0x100) && !g.Add(0x100), "an enemy becomes a group once");
	Check(g.AddCopy(0x100, 0x200) && g.AddCopy(0x100, 0x300) && g.IsCopy(0x300) && !g.Has(0x300),
	      "its copies are members, not groups");
	Check(!g.Add(0x200), "and a copy never becomes a group of its own");
	std::map<int32_t, bool> alive{{0x100, true}, {0x200, true}, {0x300, true}};
	auto lives = [&](int32_t h) { return alive[h]; };
	Check(g.Representative(0x100, lives) == 0x100, "the original stands for the group while it lives");
	alive[0x100] = false;
	Check(g.Representative(0x100, lives) == 0x200, "then a live copy does");
	alive[0x200] = false;
	Check(g.Representative(0x100, lives) == 0x300, "then the next");
	alive[0x300] = false;
	Check(g.Representative(0x100, lives) == 0x100,
	      "and with every member dead the original is asked, and is dead");
	Check(g.Representative(0x999, lives) == 0x999, "anybody not in a group is only themselves");
	int32_t copies[4];
	g.Add(0x400);
	g.AddCopy(0x400, 0x500);
	Check(g.AllCopies(copies, 4) == 3, "every copy of every group, for the mission's end");
	Check(g.Dissolve(0x100, copies, 4) == 2 && copies[0] == 0x200 && copies[1] == 0x300 &&
	          !g.Has(0x100) && g.Has(0x400) && g.Count() == 1,
	      "letting go of the original lets go of its copies, and leaves the other groups");
	Check(!g.AddCopy(0x100, 0x600), "a group that is gone takes no more copies");
}

// The owner's run on 2026-09-24: noxx3, the owner, got out of the Kuruma and
// everybody was told to get back in the vehicle; noxx2 got out and nobody was.
void TestOnlyWhoeverGotOutIsToldToGetBackIn() {
	std::printf("\nonly whoever got out of the mission's car is told to get back in\n");
	using namespace game;
	const uint8_t inVeh[8] = {'I', 'N', '_', 'V', 'E', 'H', 0, 0};
	const uint8_t inVeh2[8] = {'I', 'N', '_', 'V', 'E', 'H', '2', 0};
	const uint8_t stretch[8] = {'F', 'M', '1', '_', '1', 0, 0, 0};
	const uint8_t other[8] = {'F', 'M', '1', '_', '1', '0', 0, 0};
	Check(IsGetBackInLabel(inVeh) && IsGetBackInLabel(stretch),
	      "\"Hey! Get back in the vehicle!\" and \"Get back into the Stretch!\" are the two");
	Check(!IsGetBackInLabel(inVeh2) && !IsGetBackInLabel(other),
	      "and \"You need some wheels for this job\" is not, nor a label that only starts the same");

	// PRINT_NOW 'IN_VEH' 5000 1, as Give Me Liberty's CHECK_IN_VEHICLE_STATUS has it.
	std::vector<uint8_t> space(0x200, 0);
	const uint8_t print[] = {'I', 'N', '_', 'V', 'E', 'H', 0, 0, 0x05, 0x88, 0x13, 0x04, 0x01};
	std::memcpy(space.data() + 0x40, print, sizeof print);
	replay::Encoded e;
	Check(replay::Encode(scripts::op::PRINT_NOW, space.data(), 0x200, 0x40, nullptr, &e) &&
	          IsGetBackInPrint(e.code, e.length),
	      "the owner's PRINT_NOW of it is the owner's alone");
	Check(replay::Encode(scripts::op::CLEAR_THIS_PRINT, space.data(), 0x200, 0x40, nullptr, &e) &&
	          IsGetBackInPrint(e.code, e.length),
	      "and so is taking it off again");
	Check(replay::Encode(0x00BA, space.data(), 0x200, 0x40, nullptr, &e) &&
	          !IsGetBackInPrint(e.code, e.length),
	      "a PRINT_BIG of the same label is nothing of the kind");
	space[0x40] = 'E';
	Check(replay::Encode(scripts::op::PRINT_NOW, space.data(), 0x200, 0x40, nullptr, &e) &&
	          !IsGetBackInPrint(e.code, e.length),
	      "nor is any other PRINT_NOW");

	uint8_t label[8];
	std::vector<uint8_t> mission(0x100, 0);
	GetBackInLabelOf(mission.data(), 0x100, label);
	Check(std::memcmp(label, inVeh, 8) == 0, "a mission that prints neither gets IN_VEH");
	mission[0x80] = 0xBC;
	std::memcpy(mission.data() + 0x82, stretch, 8);
	GetBackInLabelOf(mission.data(), 0x100, label);
	Check(std::memcmp(label, stretch, 8) == 0, "Frank's gets the Stretch's");

	const MissionEffectBody fx = GetBackInEffect(19, inVeh, 1);
	int32_t duration = 0, flag = 0;
	Check(fx.kind == MISSION_EFFECT_RUN && fx.onlyTo == 2 && EffectOpcode(fx) == scripts::op::PRINT_NOW &&
	          std::memcmp(fx.code + 2, inVeh, 8) == 0 &&
	          replay::LiteralAt(fx.code, fx.length, 1, &duration) && duration == 5000 &&
	          replay::LiteralAt(fx.code, fx.length, 2, &flag) && flag == 1,
	      "what bob is told is PRINT_NOW IN_VEH for five seconds, for bob alone");

	const uint8_t bob = PlayerBit(1), carol = PlayerBit(2);
	GetBackInWatch w;
	Check(w.Asked(4, bob | carol, 1000) == 0, "the first ask only finds out who is in it");
	Check(w.Asked(4, bob | carol, 1016) == 0, "nobody moved");
	Check(w.Asked(4, carol, 1032) == bob, "bob got out");
	Check(w.Asked(4, carol, 1048) == 0, "and is told once, not every frame he stays out");
	Check(w.Asked(4, bob | carol, 1064) == 0 && w.Asked(4, bob, 1080) == carol,
	      "back in, then carol out: carol");
	Check(w.Asked(4, 0, 1080 + GET_BACK_IN_FORGET_MS + 1) == 0,
	      "a car the mission stopped asking about starts afresh: getting out after it moved on is "
	      "nothing");
	Check(w.Asked(4, bob, 5000) == 0 && w.Asked(7, carol, 5000) == 0 && w.Asked(4, 0, 5016) == bob &&
	          w.Asked(7, 0, 5016) == carol,
	      "two cars asked about in turn are each watched");
	w.Clear();
	Check(w.Asked(4, 0, 5032) == 0, "and a new mission starts with none");

	using namespace game::replay;
	Check(Listed(0x03AE) && Find(0x03AE)->kind == Kind::Plain && Find(0x03AE)->count == 6,
	      "the mission putting its smoke and flames out is replayed, so a retry lights them once");
	Check(Listed(0x01C7) && Find(0x01C7)->count == 1 && Find(0x01C7)->args[0] == Arg::ObjGlobal,
	      "and an object it keeps past its end is kept on every machine");
}

void TestTheTutorialsFlashGoesToEverybody() {
	std::printf("\nthe HUD item a tutorial flashes flashes on everybody's HUD\n");
	using namespace game::replay;
	Check(Listed(game::scripts::op::FLASH_HUD_OBJECT) &&
	          Find(game::scripts::op::FLASH_HUD_OBJECT)->kind == Kind::Plain &&
	          Find(game::scripts::op::FLASH_HUD_OBJECT)->count == 1 &&
	          Find(game::scripts::op::FLASH_HUD_OBJECT)->args[0] == Arg::Value,
	      "FLASH_HUD_OBJECT is replayed with its one operand, the radar's 8 and the -1 that ends it");
	std::vector<uint8_t> space(0x100, 0);
	space[0x40] = game::scripts::PARAM_INT8;
	space[0x41] = static_cast<uint8_t>(game::scripts::HUD_ITEM_RADAR);
	Encoded e;
	int32_t item = 0;
	Check(Encode(game::scripts::op::FLASH_HUD_OBJECT, space.data(), 0x100, 0x40, nullptr, &e) &&
	          LiteralAt(e.code, e.length, 0, &item) && item == game::scripts::HUD_ITEM_RADAR,
	      "Give Me Liberty's `03E7: flash_hud HUD_FLASH_RADAR` goes as 8");
	space[0x41] = 0xFF;
	Check(Encode(game::scripts::op::FLASH_HUD_OBJECT, space.data(), 0x100, 0x40, nullptr, &e) &&
	          LiteralAt(e.code, e.length, 0, &item) && item == game::scripts::HUD_ITEM_NONE,
	      "and its `flash_hud -1` as -1");
}

void TestTheBlueMarkersGoToEverybody() {
	std::printf("\nthe blue markers the owner's mission draws are drawn on everybody's screen\n");
	using namespace game;
	// Give Me Liberty's stop at Luigi's: 01A0 player stopped $BLOB_FLAG
	// 879.375 -303.375 7.25 870.0625 -311.6875 10.0, drawn at z (7.25 + 10) / 2.
	const MarkerArea luigis{879.375f, -303.375f, 870.0625f, -311.6875f, 8.625f};
	const uint32_t   id = 0x006F0000u + 0x20123u;

	const MissionEffectBody up = MarkerEffect(19, id, luigis, true);
	uint32_t   gotId = 0;
	MarkerArea got;
	bool       gotUp = false;
	Check(up.kind == MISSION_EFFECT_RUN && up.onlyTo == 0 && up.length == MARKER_EFFECT_LENGTH &&
	          up.length <= MISSION_EFFECT_CODE && EffectOpcode(up) == scripts::op::IS_PLAYER_IN_AREA_3D,
	      "a marker goes to everybody as IS_PLAYER_IN_AREA_3D, the instruction that draws one");
	Check(ReadMarkerEffect(up, &gotId, &got, &gotUp) && gotUp && gotId == id && got.x1 == luigis.x1 &&
	          got.y1 == luigis.y1 && got.x2 == luigis.x2 && got.y2 == luigis.y2 && got.z == luigis.z,
	      "and reads back as the same marker, id, corners and height");
	int32_t sphere = -1, z1 = 0, z2 = 0;
	std::memcpy(&z1, up.code + 3 + 3 * 5, 4);
	std::memcpy(&z2, up.code + 3 + 6 * 5, 4);
	std::memcpy(&sphere, up.code + 3 + 7 * 5, 4);
	Check(sphere == 1 && z1 == z2, "its sphere is set, and both its heights are the marker's");
	const MissionEffectBody down = MarkerEffect(19, id, luigis, false);
	Check(ReadMarkerEffect(down, &gotId, &got, &gotUp) && !gotUp && gotId == id,
	      "the same with the sphere clear takes it down");
	const uint8_t inVeh[8] = {'I', 'N', '_', 'V', 'E', 'H', 0, 0};
	MissionEffectBody notOne = up;
	notOne.code[3]           = 1;   // player 1
	Check(!ReadMarkerEffect(GetBackInEffect(19, inVeh, 1), &gotId, &got, &gotUp) &&
	          !ReadMarkerEffect(notOne, &gotId, &got, &gotUp),
	      "nothing else reads as one, nor an area check about another player");

	// The owner's side.
	OwnMarkers own;
	struct Told {
		uint32_t   id;
		MarkerArea a;
		bool       up, first;
	};
	std::vector<Told> told;
	auto tell = [&](uint32_t i, const MarkerArea &a, bool u, bool f) { told.push_back({i, a, u, f}); };
	own.Tick(1000, tell);
	Check(told.empty(), "nothing drawn, nothing told");
	own.Drawn(id, luigis, 1000);
	own.Tick(1016, tell);
	Check(told.size() == 1 && told[0].up && told[0].first && told[0].id == id,
	      "the first frame it is drawn, everybody hears it went up");
	told.clear();
	for (uint32_t t = 1016; t < 1016 + MARKER_RESEND_MS; t += 16) {
		own.Drawn(id, luigis, t);
		own.Tick(t + 16, tell);
	}
	Check(told.size() == 1 && told[0].up && !told[0].first,
	      "drawn every frame for two seconds, it is told once more, for whoever came in late");
	told.clear();
	MarkerArea moved = luigis;
	moved.x1 += 2.0f;
	moved.x2 += 2.0f;
	own.Drawn(id, moved, 3100);
	own.Tick(3116, tell);
	own.Drawn(id, moved, 3200);
	own.Tick(3216, tell);
	Check(told.empty(), "one that moves waits a quarter second");
	own.Drawn(id, moved, 3300);
	own.Tick(3316, tell);
	Check(told.size() == 1 && told[0].up && told[0].a.x1 == moved.x1, "and then says where it is");
	told.clear();
	own.Tick(3300 + MARKER_GONE_MS - 1, tell);
	Check(told.empty() && own.Count() == 1, "a frame or two without it is not it coming down");
	own.Tick(3300 + MARKER_GONE_MS, tell);
	Check(told.size() == 1 && !told[0].up && own.Count() == 0,
	      "not drawn for 400 ms, it came down, and everybody hears so");
	told.clear();
	for (uint32_t i = 0; i < MAX_MARKERS; ++i)
		own.Drawn(i + 1, luigis, 5000);
	Check(!own.Drawn(99, luigis, 5000) && own.Count() == MAX_MARKERS, "sixteen at once, and no more");
	own.Clear();
	own.Tick(5016, tell);
	Check(told.empty(), "a mission's end forgets them without a word: everybody's ends too");

	// A participant's side.
	ShownMarkers shown;
	std::vector<uint32_t> drawn;
	auto draw = [&](uint32_t i, const MarkerArea &) { drawn.push_back(i); };
	Check(shown.Heard(id, luigis, true, 1000) && shown.Has(id), "heard up, it is kept");
	shown.Each(1016, draw);
	shown.Each(1032, draw);
	Check(drawn.size() == 2 && drawn[0] == id, "and drawn every frame");
	Check(shown.Heard(id, moved, true, 1100) && shown.Count() == 1, "heard again, it is the same one");
	Check(shown.Heard(id, luigis, false, 1200) && !shown.Has(id) && shown.Heard(7, luigis, false, 1200),
	      "heard down, it is gone; down for one never up is nothing");
	drawn.clear();
	shown.Heard(id, luigis, true, 2000);
	shown.Each(2000 + MARKER_FORGET_MS - 1, draw);
	Check(drawn.size() == 1, "not heard of since, it stays up past the owner's resend");
	drawn.clear();
	shown.Each(2000 + MARKER_FORGET_MS, draw);
	Check(drawn.empty() && shown.Count() == 0, "but not three resends past it");
}

// ---- the script engine's addresses, against a retail gta3.exe ---------------------
//
// With a copy of the retail exe (COOPIII_GTA3_EXE, or reference/bin/gta3.exe)
// this reads the instructions each value in game/missionaddr.h was proved
// from (addresses.h has the note for each), and the prologue of every
// function the missions detour. Skipped without one.

bool LoadGta3(std::vector<uint8_t> &image, std::string &from) {
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
		if (got == image.size() && image.size() == game::IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

// 0 outside the file, so a wrong address fails a check rather than the run.
uint8_t ImageByte(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = static_cast<size_t>(va - game::IMAGE_BASE);
	return va >= game::IMAGE_BASE && o < img.size() ? img[o] : 0;
}

uint32_t ImageDword(const std::vector<uint8_t> &img, uint32_t va) {
	return uint32_t(ImageByte(img, va)) | uint32_t(ImageByte(img, va + 1)) << 8 |
	       uint32_t(ImageByte(img, va + 2)) << 16 | uint32_t(ImageByte(img, va + 3)) << 24;
}

// The table a range handler jumps through, from its `jmp dword [reg*4 + table]`.
uint32_t JumpTableOf(const std::vector<uint8_t> &img, uint32_t fn) {
	for (uint32_t i = 0; i + 7 <= 64; ++i) {
		const size_t o = fn - game::IMAGE_BASE + i;
		if (img[o] == 0xFF && img[o + 1] == 0x24 && (img[o + 2] & 0xC7) == 0x85)
			return ImageDword(img, fn + i + 3);
	}
	return 0;
}

bool DispatcherCallsIn(const std::vector<uint8_t> &img, uint32_t target) {
	const uint32_t d = game::CRunningScript__ProcessCommands;
	for (uint32_t i = 0; i + 5 <= game::scripts::DISPATCHER_BYTES; ++i)
		if (img[d - game::IMAGE_BASE + i] == 0xE8 && d + i + 5 + ImageDword(img, d + i + 1) == target)
			return true;
	return false;
}

bool DwordWithin(const std::vector<uint8_t> &img, uint32_t fn, uint32_t bytes, uint32_t value) {
	for (uint32_t i = 0; i + 4 <= bytes; ++i)
		if (ImageDword(img, fn + i) == value)
			return true;
	return false;
}

// What a range handler takes off the opcode before its table: the `lea
// reg,[reg-base]` ahead of the jump, or nothing for 0..99.
uint32_t RangeBaseOf(const std::vector<uint8_t> &img, uint32_t fn) {
	for (uint32_t i = 0; i + 6 <= 48; ++i) {
		const size_t o = fn - game::IMAGE_BASE + i;
		if (img[o] == 0xFF && img[o + 1] == 0x24)
			return 0;
		const uint8_t modrm = img[o + 1];
		if (img[o] == 0x8D && (modrm >> 6) == 2 && (modrm & 7) != 4) {
			const int32_t disp = static_cast<int32_t>(ImageDword(img, fn + i + 2));
			if (disp < 0 && disp > -0x500)
				return static_cast<uint32_t>(-disp);
		}
	}
	return 0;
}

// The handler a range's table sends `opcode` to.
uint32_t HandlerOf(const std::vector<uint8_t> &img, uint32_t range, int32_t opcode) {
	const uint32_t table = JumpTableOf(img, range);
	const uint32_t base  = RangeBaseOf(img, range);
	if (table == 0 || static_cast<uint32_t>(opcode) < base)
		return 0;
	return ImageDword(img, table + (static_cast<uint32_t>(opcode) - base) * 4);
}

// `mov byte [reg+offset],1` within `bytes` of `fn`, disp8 or disp32.
bool WritesOne(const std::vector<uint8_t> &img, uint32_t fn, uint32_t bytes, size_t offset) {
	for (uint32_t i = 0; i + 7 <= bytes; ++i) {
		const size_t  o     = fn - game::IMAGE_BASE + i;
		const uint8_t modrm = img[o + 1];
		if (img[o] != 0xC6 || ((modrm >> 3) & 7) != 0 || (modrm & 7) == 4)
			continue;
		if ((modrm >> 6) == 1 && img[o + 2] == offset && img[o + 3] == 1)
			return true;
		if ((modrm >> 6) == 2 && ImageDword(img, fn + i + 2) == offset && img[o + 6] == 1)
			return true;
	}
	return false;
}

// `cmp byte [reg+offset],0`, disp8 or disp32.
bool ComparesZero(const std::vector<uint8_t> &img, uint32_t fn, uint32_t bytes, size_t offset) {
	for (uint32_t i = 0; i + 7 <= bytes; ++i) {
		const size_t  o     = fn - game::IMAGE_BASE + i;
		const uint8_t modrm = img[o + 1];
		if (img[o] != 0x80 || ((modrm >> 3) & 7) != 7 || (modrm & 7) == 4)
			continue;
		if ((modrm >> 6) == 1 && img[o + 2] == offset && img[o + 3] == 0)
			return true;
		if ((modrm >> 6) == 2 && ImageDword(img, fn + i + 2) == offset && img[o + 6] == 0)
			return true;
	}
	return false;
}

// The target of the first `E8 rel32` within `bytes` of `fn` past CollectParameters.
uint32_t CallAfter(const std::vector<uint8_t> &img, uint32_t fn, uint32_t bytes) {
	for (uint32_t i = 0; i + 5 <= bytes; ++i) {
		if (img[fn - game::IMAGE_BASE + i] != 0xE8)
			continue;
		const uint32_t to = fn + i + 5 + ImageDword(img, fn + i + 1);
		if (to != game::CTheScripts__CollectParameters)
			return to;
	}
	return 0;
}

// `pattern` at `va`, -1 matching any byte.
bool BytesAt(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<int> pattern) {
	size_t o = va - game::IMAGE_BASE;
	for (int b : pattern) {
		if (o >= img.size() || (b >= 0 && img[o] != static_cast<uint8_t>(b)))
			return false;
		++o;
	}
	return true;
}

// The same, anywhere in the `bytes` from `fn`.
bool BytesWithin(const std::vector<uint8_t> &img, uint32_t fn, uint32_t bytes,
                 std::initializer_list<int> pattern) {
	for (uint32_t i = 0; i < bytes; ++i)
		if (BytesAt(img, fn + i, pattern))
			return true;
	return false;
}

// A dword's four bytes, for a pattern.
#define LE32(v)                                                                          \
	static_cast<int>((v) & 0xFF), static_cast<int>(((v) >> 8) & 0xFF),                  \
	    static_cast<int>(((v) >> 16) & 0xFF), static_cast<int>(((v) >> 24) & 0xFF)

uint32_t CallTargetAt(const std::vector<uint8_t> &img, uint32_t va) {
	return ImageByte(img, va) == 0xE8 ? va + 5 + ImageDword(img, va + 1) : 0;
}

// The first `E8` in the `bytes` from `fn`, or 0. Only for code with no E8
// inside another instruction before the call.
uint32_t FirstCallIn(const std::vector<uint8_t> &img, uint32_t fn, uint32_t bytes) {
	for (uint32_t i = 0; i + 5 <= bytes; ++i)
		if (ImageByte(img, fn + i) == 0xE8)
			return CallTargetAt(img, fn + i);
	return 0;
}

// Opcode `op`'s handler, out of the range table its dispatcher link uses.
uint32_t HandlerOf(const std::vector<uint8_t> &img, int32_t op) {
	struct Link {
		int32_t  first, last;
		uint32_t table;
	};
	static const Link links[] = {
	    {0, 99, game::g_ScriptOpcodeTable_0},       {100, 198, game::g_ScriptOpcodeTable_100},
	    {214, 298, game::g_ScriptOpcodeTable_200},  {304, 399, game::g_ScriptOpcodeTable_300},
	    {400, 499, game::g_ScriptOpcodeTable_400},  {500, 598, game::g_ScriptOpcodeTable_500},
	    {657, 699, game::g_ScriptOpcodeTable_600},  {700, 799, game::g_ScriptOpcodeTable_700},
	    {800, 899, game::g_ScriptOpcodeTable_800},  {900, 999, game::g_ScriptOpcodeTable_900},
	    {1001, 1099, game::g_ScriptOpcodeTable_1000}, {1100, 1154, game::g_ScriptOpcodeTable_1100},
	};
	for (const Link &l : links)
		if (op >= l.first && op <= l.last)
			return ImageDword(img, l.table + 4 * static_cast<uint32_t>(op - l.first));
	return 0;
}

// How many operands a handler collects first: the `push N` nearest before
// its first call to CollectParameters, in its opening bytes, or -1.
int CollectedBy(const std::vector<uint8_t> &img, uint32_t fn) {
	for (uint32_t i = 0; i + 5 <= 0x60; ++i) {
		if (CallTargetAt(img, fn + i) != game::CTheScripts__CollectParameters)
			continue;
		for (uint32_t back = 2; back <= 0x18 && back <= i; ++back)
			if (ImageByte(img, fn + i - back) == 0x6A)
				return ImageByte(img, fn + i - back + 1);
		return -1;
	}
	return -1;
}

// What the two moves a participant replays do to a car (mission.h,
// MissionMoveFor): SET_PLAYER_COORDINATES moves a seated player's car,
// WARP_PLAYER_FROM_CAR_TO_COORD takes the player out and leaves it.
void TestTheMovesAgainstTheImage() {
	std::printf("\nthe owner's moves, and what they do to a car, against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadGta3(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check them\n");
		return;
	}
	namespace g = game;
	const uint32_t set = HandlerOf(img, g::scripts::op::SET_PLAYER_COORDINATES);
	Check(set == 0x0043A92C && CollectedBy(img, set) == 4,
	      "SET_PLAYER_COORDINATES is 0x0043A92C and takes the player and a place");
	Check(BytesAt(img, 0x0043A995, {0x80, 0xB9, LE32(static_cast<uint32_t>(g::offs::PED_IN_VEHICLE)), 0}) &&
	          BytesAt(img, 0x0043A9D0, {0x8B, 0x89, LE32(static_cast<uint32_t>(g::offs::PED_MY_VEHICLE))}) &&
	          BytesAt(img, 0x0043AA08, {0xFF, 0x55, 0x2C}) && BytesAt(img, 0x0043AA2E, {0xFF, 0x56, 0x2C}),
	      "and for a player in a car it tests bInVehicle and teleports m_pMyVehicle through its "
	      "vtable, so whichever machine runs it moves the car");
	Check(CallTargetAt(img, 0x0043AA43) == 0x00454060,
	      "then clears the space round it, the car and all (ClearSpaceForMissionEntity)");
	const uint32_t warp = HandlerOf(img, g::scripts::op::WARP_PLAYER_FROM_CAR_TO_COORD);
	Check(warp == 0x0043E860 && CollectedBy(img, warp) == 4,
	      "WARP_PLAYER_FROM_CAR_TO_COORD is 0x0043E860, the same four operands");
	Check(CallTargetAt(img, 0x0043E910) == g::CVehicle__RemoveDriver &&
	          CallTargetAt(img, 0x0043E951) == g::CVehicle__RemovePassenger &&
	          BytesAt(img, 0x0043E958,
	                  {0xC6, 0x80, LE32(static_cast<uint32_t>(g::offs::PED_IN_VEHICLE)), 0}) &&
	          BytesAt(img, 0x0043E961,
	                  {0xC7, 0x80, LE32(static_cast<uint32_t>(g::offs::PED_MY_VEHICLE)), 0, 0, 0, 0}),
	      "and it takes the driver or the passenger out, clears the seat, and never moves the car");
}

void TestTheScriptEngineAgainstTheImage() {
	std::printf("\nthe script engine's addresses against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadGta3(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the missions' "
		            "addresses against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());
	using namespace game::scripts;
	namespace g = game;
	struct Range {
		uint32_t    fn;
		uint32_t    table;
		const char *what;
	};
	const Range ranges[] = {
	    {RANGE_0, g::g_ScriptOpcodeTable_0, "0..99"},
	    {RANGE_100, g::g_ScriptOpcodeTable_100, "100..199"},
	    {RANGE_200, g::g_ScriptOpcodeTable_200, "200..299"},
	    {RANGE_300, g::g_ScriptOpcodeTable_300, "300..399"},
	    {RANGE_400, g::g_ScriptOpcodeTable_400, "400..499"},
	    {RANGE_500, g::g_ScriptOpcodeTable_500, "500..599"},
	    {RANGE_600, g::g_ScriptOpcodeTable_600, "600..699"},
	    {RANGE_700, g::g_ScriptOpcodeTable_700, "700..799"},
	    {RANGE_800, g::g_ScriptOpcodeTable_800, "800..899"},
	    {RANGE_900, g::g_ScriptOpcodeTable_900, "900..999"},
	    {RANGE_1000, g::g_ScriptOpcodeTable_1000, "1000..1099"},
	    {RANGE_1100, g::g_ScriptOpcodeTable_1100, "1100..1154"},
	};
	for (const Range &r : ranges) {
		char what[128];
		std::snprintf(what, sizeof what, "the %s handler at 0x%08X is one the dispatcher calls",
		              r.what, r.fn);
		Check(DispatcherCallsIn(img, r.fn), what);
		Check(JumpTableOf(img, r.fn) == r.table, "and it jumps through the table addresses.h names");
	}
	Check(!DispatcherCallsIn(img, 0x0043AEA4), "0x0043AEA4 is not one: the 100 handler is 0x0043AEA0");
	// The last link: below 1200 and past the 1000 range's `cmp dx,44Ch`.
	Check(BytesAt(img, 0x00439632, {0x66, 0x81, 0xFA, 0xB0, 0x04, 0x7D, -1, 0x0F, 0xBF, 0xC2, 0x50}) &&
	          CallTargetAt(img, 0x0043963D) == RANGE_1100 &&
	          CallTargetAt(img, 0x0043962B) == RANGE_1000,
	      "the dispatcher's last link sends 1100..1199 to 0x00589D00, and 0x00588490 is the 1000 "
	      "range's");
	Check(RangeBaseOf(img, RANGE_1100) == 1100 &&
	          BytesAt(img, RANGE_1100 + 0x1B, {0x83, 0xF8, 0x36}),
	      "the 1100 handler takes 1100 off the opcode and stops at 1154 (`cmp eax,36h`)");
	{
		const uint32_t island = HandlerOf(img, op::LOAD_COLLISION_WITH_SCREEN);
		Check(island == 0x00589D2D && CollectedBy(img, island) == 1 &&
		          BytesWithin(img, island, 0x30, {0xA3, LE32(g::CGame__currLevel)}) &&
		          BytesWithin(img, island, 0x30, {0xA1, LE32(g::CCollision__ms_collisionInMemory)}),
		      "LOAD_COLLISION_WITH_SCREEN takes the island into CGame::currLevel and loads it "
		      "unless it is the collision in memory");
		const uint32_t lips = HandlerOf(img, op::MAKE_CRAIGS_CAR_A_BIT_STRONGER);
		Check(lips == 0x00589EE3 && CollectedBy(img, lips) == 2 &&
		          BytesWithin(img, lips, 0x50, {0x80, 0xCA, 0x02, 0x88, 0x90, 0xDA, 0x04, 0, 0}),
		      "MAKE_CRAIGS_CAR_A_BIT_STRONGER takes the car and a flag, into +0x4DA bit 1");
		Check(HandlerOf(img, op::SET_JAMES_CAR_ON_PATH_TO_PLAYER) == 0x00589F42 &&
		          CollectedBy(img, 0x00589F42) == 1,
		      "SET_JAMES_CAR_ON_PATH_TO_PLAYER takes the car");
		Check(HandlerOf(img, op::LOAD_END_OF_GAME_TUNE) == 0x00589FA5 &&
		          BytesAt(img, 0x00589FA5, {0xB9, -1, -1, -1, -1, 0x6A, 0x02, 0xE8}),
		      "LOAD_END_OF_GAME_TUNE takes nothing: it opens on DMAudio, no CollectParameters");
	}

	// The dispatcher: the ip, the script space and the NOT flag.
	const uint32_t d = g::CRunningScript__ProcessCommands;
	Check(BytesAt(img, d + 7, {0x8B, 0x41, static_cast<int>(layout::SCRIPT_IP)}) &&
	          BytesAt(img, d + 0x0A, {0x0F, 0xB6, 0x90, LE32(SCRIPT_SPACE)}) &&
	          BytesAt(img, d + 0x2B, {0xC6, 0x81, static_cast<int>(layout::SCRIPT_NOT), 0, 0, 0, 1}),
	      "the dispatcher reads the opcode at [ip + ScriptSpace] and keeps its NOT bit at +0x82");

	// The script list.
	Check(BytesAt(img, 0x004393BF, {0x8B, 0x0D, LE32(ACTIVE_SCRIPTS)}) &&
	          BytesAt(img, 0x004393D7, {0x8B, 0x29}) &&
	          BytesAt(img, 0x004393D9, {0x01, 0x71, static_cast<int>(layout::SCRIPT_LOCALS + 0x40)}),
	      "CTheScripts::Process walks pActiveScripts through +0, ticking the timers after the "
	      "sixteen locals");
	Check(BytesWithin(img, g::CTheScripts__StartNewScript, 0x30, {0x68, LE32(ACTIVE_SCRIPTS)}) &&
	          BytesAt(img, 0x00438FE0, {0x8B, 0x54, 0x24, 0x04, 0x8B, 0x02, 0x89, 0x01, 0xC7, 0x41,
	                                    0x04, 0, 0, 0, 0}) &&
	          BytesAt(img, 0x00438FF8, {0x89, 0x0A}),
	      "a new script goes in at the head of the list");

	// $ONMISSION and the mission slot.
	Check(BytesWithin(img, HandlerOf(img, 0x0180), 0x28, {0xA3, LE32(ON_A_MISSION_FLAG)}),
	      "DECLARE_MISSION_FLAG stores its operand in OnAMissionFlag");
	Check(DwordWithin(img, g::CRunningScript__DoDeatharrestCheck, 0x100, SCRIPT_SPACE) &&
	          DwordWithin(img, g::CRunningScript__DoDeatharrestCheck, 0x100, ON_A_MISSION_FLAG),
	      "DoDeatharrestCheck writes $ONMISSION through the script space and OnAMissionFlag");

	// Which byte says "the mission slot's script". Read off the handlers
	// themselves, through their range's table.
	const uint32_t startMission = HandlerOf(img, RANGE_1000, op::LOAD_AND_LAUNCH_MISSION_INTERNAL);
	const uint32_t launchThread = HandlerOf(img, RANGE_200, 0x00D7);   // LAUNCH_MISSION
	const uint32_t terminate    = HandlerOf(img, RANGE_0, 0x004E);     // TERMINATE_THIS_SCRIPT
	Check(startMission != 0 && WritesOne(img, startMission, 0x90, layout::SCRIPT_MISSION_SLOT) &&
	          WritesOne(img, startMission, 0x90, layout::SCRIPT_MISSION_RULES) &&
	          DwordWithin(img, startMission, 0x90, ALREADY_RUNNING_A_MISSION),
	      "START_MISSION sets both flags on its script, beside ALREADY_RUNNING_A_MISSION");
	Check(launchThread != 0 && WritesOne(img, launchThread, 0x20, layout::SCRIPT_MISSION_RULES) &&
	          !WritesOne(img, launchThread, 0x20, layout::SCRIPT_MISSION_SLOT),
	      "00D7 sets the rules byte on a plain main.scm thread, and not the slot's: the rules "
	      "byte is no witness of a mission");
	Check(terminate != 0 && ComparesZero(img, terminate, 0x10, layout::SCRIPT_MISSION_SLOT) &&
	          DwordWithin(img, terminate, 0x20, ALREADY_RUNNING_A_MISSION),
	      "TERMINATE_THIS_SCRIPT frees the slot for the script with the slot's byte");

	// The screen's fade, for a player the session keeps waiting.
	const uint32_t doFade = HandlerOf(img, RANGE_300, op::DO_FADE);
	const uint32_t fade   = doFade ? CallAfter(img, doFade, 0x40) : 0;
	Check(doFade != 0 && DwordWithin(img, doFade, 0x40, game::TheCamera) && fade == 0x0046B3A0,
	      "DO_FADE calls CCamera::Fade on TheCamera");
	Check(fade != 0 && WritesOne(img, fade, 0x10, CAMERA_FADING),
	      "which sets the camera's fading flag first thing");
	uint32_t faded = 0;
	std::memcpy(&faded, &CAMERA_FADED, 4);
	Check(img[0x0046B9C0 - game::IMAGE_BASE] == 0xD9 && img[0x0046B9C1 - game::IMAGE_BASE] == 0x81 &&
	          ImageDword(img, 0x0046B9C2) == CAMERA_FADE &&
	          ImageDword(img, ImageDword(img, 0x0046B9E2)) == faded,
	      "GetScreenFadeStatus reads the fade, and calls 255 faded out");
	{
	const uint32_t startMission = HandlerOf(img, op::LOAD_AND_LAUNCH_MISSION_INTERNAL);
	Check(CollectedBy(img, startMission) == 1 &&
	          BytesWithin(img, startMission, 0x60,
	                      {0x68, 0x00, 0x80, 0x00, 0x00, 0x68, LE32(SCRIPT_SPACE + MAIN_SCRIPT_SIZE)}) &&
	          BytesWithin(img, startMission, 0x70, {0x68, LE32(MAIN_SCRIPT_SIZE)}) &&
	          SCRIPT_SPACE_SIZE == MAIN_SCRIPT_SIZE + 0x8000,
	      "START_MISSION reads 0x8000 bytes of mission in after the main script's 0x20000");
	Check(BytesWithin(img, startMission, 0x90,
	                  {0xC6, 0x45, static_cast<int>(layout::SCRIPT_IS_MISSION), 0x01}) &&
	          BytesWithin(img, startMission, 0x90, {0xC6, 0x05, LE32(ALREADY_RUNNING_A_MISSION), 0x01}),
	      "and marks the script a mission's, and a mission loaded");
	Check(BytesAt(img, HandlerOf(img, 0x004E),
	              {0x80, 0xBB, 0x85, 0, 0, 0, 0, 0x74, 0x07, 0xC6, 0x05, LE32(ALREADY_RUNNING_A_MISSION),
	               0x00}),
	      "TERMINATE_THIS_SCRIPT is what says the mission slot is empty again");
	Check(BytesWithin(img, g::CTheScripts__Init, 0x70, {0x3D, LE32(SCRIPT_SPACE_SIZE)}) &&
	          BytesWithin(img, g::CTheScripts__Init, 0x20, {0x0F, 0x7F, 0x80, LE32(SCRIPT_SPACE)}),
	      "CTheScripts::Init zeroes the whole script space");

	// CRunningScript, as its Init lays it out.
	const uint32_t init = g::CRunningScript__Init;
	const int      at   = static_cast<int>(layout::SCRIPT_AND_OR);
	Check(BytesWithin(img, init, 0x10, {0x8D, 0x7A, static_cast<int>(layout::SCRIPT_NAME)}) &&
	          BytesWithin(img, init, 0x30, {0xC7, 0x42, static_cast<int>(layout::SCRIPT_IP), 0, 0, 0, 0}) &&
	          BytesWithin(img, init, 0x50, {0xC7, 0x42, static_cast<int>(layout::SCRIPT_STACK), 0, 0, 0, 0}) &&
	          BytesWithin(img, init, 0x50,
	                      {0xC7, 0x42,
	                       static_cast<int>(layout::SCRIPT_STACK + 4 * (SCRIPT_STACK_DEPTH - 1)), 0, 0, 0,
	                       0, 0x66, 0xC7, 0x42, static_cast<int>(layout::SCRIPT_SP), 0, 0}),
	      "CRunningScript::Init: the name, the ip, six gosub returns and their depth");
	Check(BytesWithin(img, init, 0x70, {0xC7, 0x42, static_cast<int>(layout::SCRIPT_WAKE_TIME), 0, 0, 0, 0}) &&
	          BytesWithin(img, init, 0x70, {0xC6, 0x42, static_cast<int>(layout::SCRIPT_COND_RESULT), 0}) &&
	          BytesWithin(img, init, 0x70, {0xC6, 0x42, static_cast<int>(layout::SCRIPT_IS_MISSION), 0}) &&
	          BytesWithin(img, init, 0x80,
	                      {0xC7, 0x44, 0x8A, static_cast<int>(layout::SCRIPT_LOCALS), 0, 0, 0, 0}) &&
	          BytesWithin(img, init, 0xD0, {0x66, 0xC7, 0x82, at, 0, 0, 0, 0, 0}) &&
	          BytesWithin(img, init, 0xD0, {0xC6, 0x82, static_cast<int>(layout::SCRIPT_NOT), 0, 0, 0, 0}) &&
	          BytesWithin(img, init, 0xD0,
	                      {0xC6, 0x82, static_cast<int>(layout::SCRIPT_DEATHARREST_ARMED), 0, 0, 0, 1}) &&
	          BytesWithin(img, init, 0xD0,
	                      {0xC6, 0x82, static_cast<int>(layout::SCRIPT_DEATHARREST_DONE), 0, 0, 0, 0}),
	      "and the wake time, the flags, the locals, the and/or state, NOT and the death check");
	const uint32_t ucf = g::CRunningScript__UpdateCompareFlag;
	Check(BytesWithin(img, ucf, 0x30, {0x88, 0x41, static_cast<int>(layout::SCRIPT_COND_RESULT)}) &&
	          BytesWithin(img, ucf, 0x60, {0x66, 0x83, 0xFA, ANDOR_ORS_1}) &&
	          BytesWithin(img, ucf, 0x60, {0x66, 0x83, 0xFA, ANDOR_ORS_8}) &&
	          BytesAt(img, ucf, {0x80, 0xB9, static_cast<int>(layout::SCRIPT_NOT), 0, 0, 0, 0}) &&
	          ANDOR_NONE == 0,
	      "UpdateCompareFlag counts an `if or` down from 21..28");
	const uint32_t dac = g::CRunningScript__DoDeatharrestCheck;
	Check(BytesWithin(img, dac, 0x20,
	                  {0x80, 0xBB, static_cast<int>(layout::SCRIPT_DEATHARREST_ARMED), 0, 0, 0, 0}) &&
	          BytesWithin(img, dac, 0x100, {0x66, 0xFF, 0x4B, static_cast<int>(layout::SCRIPT_SP)}) &&
	          BytesWithin(img, dac, 0x100, {0x8B, 0x44, 0x93, static_cast<int>(layout::SCRIPT_STACK)}) &&
	          BytesWithin(img, dac, 0x200,
	                      {0xC6, 0x83, static_cast<int>(layout::SCRIPT_DEATHARREST_DONE), 0, 0, 0, 1}) &&
	          BytesWithin(img, dac, 0x200, {0xC7, 0x43, static_cast<int>(layout::SCRIPT_WAKE_TIME), 0, 0, 0, 0}),
	      "DoDeatharrestCheck unwinds the gosub stack the way UnwindForDeatharrest does");

	// The operand types, out of CollectParameters' own switch.
	uint32_t types = 0;
	for (uint32_t i = 0; i + 7 <= 0x60 && types == 0; ++i)
		if (BytesAt(img, g::CTheScripts__CollectParameters + i, {0xFF, 0x24, 0x85}))
			types = ImageDword(img, g::CTheScripts__CollectParameters + i + 3);
	auto typeCase = [&](uint8_t t) { return types ? ImageDword(img, types + 4u * (t - 1u)) : 0u; };
	float sixteenths = 0.0f;
	for (uint32_t i = 0; i + 6 <= 0x40; ++i)
		if (BytesAt(img, typeCase(PARAM_FLOAT) + i, {0xD8, 0x35})) {
			const uint32_t bits = ImageDword(img, ImageDword(img, typeCase(PARAM_FLOAT) + i + 2));
			std::memcpy(&sixteenths, &bits, 4);
			break;
		}
	Check(types != 0 && BytesWithin(img, typeCase(PARAM_INT32), 0x40, {0x83, 0x03, 0x04}) &&
	          BytesWithin(img, typeCase(PARAM_GLOBAL), 0x30, {0x81, 0xC1, LE32(SCRIPT_SPACE)}) &&
	          BytesWithin(img, typeCase(PARAM_LOCAL), 0x30,
	                      {0x8B, 0x54, 0x8F, static_cast<int>(layout::SCRIPT_LOCALS)}) &&
	          BytesWithin(img, typeCase(PARAM_INT8), 0x12, {0xFF, 0x03}) &&
	          BytesWithin(img, typeCase(PARAM_INT16), 0x20, {0x83, 0x03, 0x02}) &&
	          sixteenths == 16.0f,
	      "CollectParameters: 1 is 4 bytes, 2 a global, 3 a local, 4 one byte, 5 two, 6 sixteenths");
	float z = 0.0f;
	const uint32_t setCoords = HandlerOf(img, op::SET_PLAYER_COORDINATES);
	for (uint32_t i = 0; i + 6 <= 0x50; ++i)
		if (BytesAt(img, setCoords + i, {0xD8, 0x1D})) {
			const uint32_t bits = ImageDword(img, ImageDword(img, setCoords + i + 2));
			std::memcpy(&z, &bits, 4);
			break;
		}
	Check(CollectedBy(img, setCoords) == 4 && z == SCRIPT_Z_FIND_GROUND,
	      "SET_PLAYER_COORDINATES finds the ground itself for a z at or below -100");

	// The ped pool's seam.
	Check(BytesAt(img, CPool_CPed__GetAt, {0x53, 0x8B, 0x5C, 0x24, 0x08}) &&
	          BytesWithin(img, CPool_CPed__GetAt, 0x30, {0x69, 0xC0, 0xF0, 0x05, 0x00, 0x00}) &&
	          BytesWithin(img, CPool_CPed__GetAt, 0x30, {0xC2, 0x04, 0x00}) &&
	          FirstCallIn(img, g::CPools__GetPed, 0x10) == CPool_CPed__GetAt &&
	          HandlerOf(img, 0x0187) == ADD_BLIP_FOR_CHAR_HANDLER,
	      "CPool<CPed>::GetAt is __thiscall, ret 4, the ped pool's, and what CPools::GetPed calls");

	// Every opcode the missions intercept or run takes the operands they give it.
	struct Operands {
		int32_t op;
		int     n;
	};
	const Operands operands[] = {
	    {op::CAN_PLAYER_START_MISSION, 1}, {op::LOAD_AND_LAUNCH_MISSION_INTERNAL, 1},
	    {op::IS_PLAYER_IN_AREA_2D, 6},    {op::IS_PLAYER_IN_AREA_3D, 8},
	    {op::REMOVE_BLIP, 1},             {op::ADD_BLIP_FOR_COORD, 3},
	    {op::ADD_BLIP_FOR_COORD_OLD, 5},  {op::ADD_SPRITE_BLIP_FOR_COORD, 4},
	    {op::REMOVE_PICKUP, 1},           {op::HAS_MODEL_LOADED, 1},
	    {op::HAS_SPECIAL_CHARACTER_LOADED, 1},
	    {op::SET_CHAR_OBJ_KILL_CHAR_ON_FOOT, 2},   {op::SET_CHAR_OBJ_KILL_PLAYER_ON_FOOT, 2},
	    {op::SET_CHAR_OBJ_KILL_CHAR_ANY_MEANS, 2}, {op::SET_CHAR_OBJ_KILL_PLAYER_ANY_MEANS, 2},
	    {op::DELETE_CHAR, 1},             {op::MARK_CHAR_AS_NO_LONGER_NEEDED, 1},
	    {op::CREATE_CHAR, 5},             {op::GIVE_WEAPON_TO_CHAR, 3},
	    {op::SET_CHAR_HEALTH, 2},         {op::ADD_ARMOUR_TO_CHAR, 2},
	    {op::HAS_CHAR_SPOTTED_PLAYER, 2}, {op::IS_PLAYER_SHOOTING_IN_AREA, 6},
	    {op::IS_PLAYER_IN_MODEL, 2},      {op::IS_CAR_IN_MISSION_GARAGE, 1},
	    {op::HAS_RESPRAY_HAPPENED, 1},    {op::OVERRIDE_NEXT_RESTART, 4},
	    {op::SET_PLAYER_CONTROL, 2},      {op::DO_FADE, 2},
	    {op::SWITCH_WIDESCREEN, 1},       {op::SET_PLAYER_VISIBLE, 2},
	    {op::SET_EVERYONE_IGNORE_PLAYER, 2}, {op::SET_POLICE_IGNORE_PLAYER, 2},
	    {op::SET_PLAYER_NEVER_GETS_TIRED, 2}, {op::APPLY_BRAKES_TO_PLAYERS_CAR, 2},
	    {op::SET_FREE_BOMB_SHOP, 1},      {op::MARK_OBJECT_AS_NO_LONGER_NEEDED, 1},
	    {op::REMOVE_SCRIPT_FIRE, 1},      {op::IS_PLAYER_IN_CAR, 2},
	    {op::DELETE_CAR, 1},              {op::DONT_REMOVE_CAR, 1},
	    {op::DONT_REMOVE_OBJECT, 1},      {op::REMOVE_PARTICLE_EFFECTS_IN_AREA, 6},
	};
	int wrong = 0;
	for (const Operands &o : operands)
		if (CollectedBy(img, HandlerOf(img, o.op)) != o.n) {
			std::printf("    %04X takes %d, not %d\n", static_cast<unsigned>(o.op),
			            CollectedBy(img, HandlerOf(img, o.op)), o.n);
			++wrong;
		}
	Check(wrong == 0, "every instruction the missions intercept or run takes the operands they give it");
	// And everything on the replay list: what is not a text label, a global
	// read by its offset or an output is collected, and in that number.
	int listed = 0;
	wrong      = 0;
	for (uint16_t code = 0; code <= 1154; ++code) {
		const game::replay::Entry *e = game::replay::Find(code);
		if (!e)
			continue;
		++listed;
		int values = 0;
		for (uint8_t i = 0; i < e->count; ++i)
			if (e->args[i] != game::replay::Arg::Text && e->args[i] != game::replay::Arg::Global &&
			    e->args[i] != game::replay::Arg::Output && e->args[i] != game::replay::Arg::OutGlobal &&
			    e->args[i] != game::replay::Arg::OutBlip && e->args[i] != game::replay::Arg::SoundOut)
				++values;
		const int got = CollectedBy(img, HandlerOf(img, code));
		if (values != 0 && got != values) {
			std::printf("    %04X collects %d, the list sends %d\n", code, got, values);
			++wrong;
		}
	}
	Check(listed > 100 && wrong == 0, "every instruction on the replay list takes what the list sends");
	// The odd jobs' counters take nothing at all: each handler is the CStats
	// increment and its return, with no CollectParameters in front.
	bool bare = BytesAt(img, HandlerOf(img, 0x0315), {0x30, 0xC0, 0xFF, 0x05});
	for (uint16_t code : {0x0315, 0x0401, 0x0402, 0x0404})
		bare = bare && game::replay::Find(code) && game::replay::Find(code)->count == 0;
	for (uint16_t code : {0x0401, 0x0402, 0x0404})
		bare = bare && BytesAt(img, CallTargetAt(img, HandlerOf(img, code)),
		                       {0xFF, 0x05, -1, -1, -1, -1, 0xC3});
	Check(bare, "the odd jobs' counters take no operand, and only count one more");

	// A contact's marker, and taking one off (replay.h, Arg::OutBlip).
	const uint32_t contact = HandlerOf(img, 0x02A7);
	Check(contact == 0x004453EC && CollectedBy(img, contact) == 4 &&
	          BytesAt(img, 0x0044546F, {0x6A, 0x03, 0x6A, 0x02}) &&
	          BytesAt(img, 0x0044547C, {0x6A, static_cast<int>(g::BLIP_CONTACT_POINT)}) &&
	          CallTargetAt(img, 0x00445485) == g::CRadar__SetCoordBlip &&
	          CallTargetAt(img, 0x00445496) == g::CRadar__SetBlipSprite &&
	          BytesAt(img, 0x0044549B, {0x89, 0x35, LE32(g::CTheScripts__ScriptParams)}) &&
	          BytesAt(img, 0x004454A8, {0x6A, 0x01, 0x50}) &&
	          CallTargetAt(img, 0x004454AB) == g::CRunningScript__StoreParameters,
	      "ADD_SPRITE_BLIP_FOR_CONTACT_POINT takes four, makes a BLIP_CONTACT_POINT shown both "
	      "ways in colour 2, and stores it in its one output");
	const uint32_t removeBlip = HandlerOf(img, op::REMOVE_BLIP);
	Check(CallAfter(img, removeBlip, 0x20) == g::CRadar__ClearBlip &&
	          CallTargetAt(img, g::CRadar__ClearBlip + 6) == g::CRadar__GetActualBlipArrayIndex &&
	          BytesAt(img, g::CRadar__ClearBlip + 0x0E, {0x83, 0xFB, 0xFF, 0x75, 0x02, 0x5B, 0xC3}) &&
	          BytesWithin(img, g::CRadar__GetActualBlipArrayIndex, 0x30,
	                      {0x0F, 0xB7, 0x14, 0xD5, LE32(g::CRadar__ms_RadarTrace + g::TRACE_BLIP_INDEX)}) &&
	          BytesWithin(img, g::CRadar__GetNewUniqueBlipIndex, 0x20,
	                      {0x66, 0xFF, 0x80, LE32(g::CRadar__ms_RadarTrace + g::TRACE_BLIP_INDEX)}),
	      "REMOVE_BLIP clears nothing for a handle whose slot has made a blip since: every new "
	      "blip counts its slot on, and a stale handle no longer matches it");

	// The paramedic's reward.
	const uint32_t tired = HandlerOf(img, op::SET_PLAYER_NEVER_GETS_TIRED);
	Check(tired == 0x00448C37 && CollectedBy(img, tired) == 2 &&
	          BytesWithin(img, tired, 0x30, {0x69, 0xC0, LE32(g::offs::PLAYERINFO_STRIDE)}) &&
	          BytesWithin(img, tired, 0x30, {0x05, LE32(g::CWorld__Players)}) &&
	          BytesWithin(img, tired, 0x40, {0xC6, 0x80, 0x14, 0x01, 0x00, 0x00, 0x01}),
	      "SET_PLAYER_NEVER_GETS_TIRED is CWorld::Players[n].m_bInfiniteSprint (+0x114), a "
	      "flag the save keeps");

	// How a mission ends, and what it leaves.
	const uint32_t cleanup  = 0x008F2A24;   // CTheScripts::MissionCleanup
	const uint32_t finished = HandlerOf(img, op::MISSION_HAS_FINISHED);
	Check(BytesAt(img, finished, {0x80, 0x7B, static_cast<int>(layout::SCRIPT_MISSION_RULES), 0x00, 0x74}) &&
	          BytesWithin(img, finished, 0x40, {0xB9, LE32(cleanup), 0xE8}) &&
	          CallTargetAt(img, finished + 0x2D) == 0x00437C10,
	      "MISSION_HAS_FINISHED is the cleanup list's Process for a mission's script, and no end: "
	      "a failure runs it twice");
	const uint32_t createCar = HandlerOf(img, op::CREATE_CAR);
	Check(createCar == 0x0043C476 &&
	          BytesWithin(img, createCar, 0x400,
	                      {0xA3, LE32(g::CTheScripts__ScriptParams), 0x8D, 0x45, 0x10, 0x89, 0xE9, 0x6A,
	                       0x01}) &&
	          BytesWithin(img, createCar, 0x400, {0xB9, LE32(cleanup), 0x6A, 0x01}),
	      "CREATE_CAR leaves the new car's handle in ScriptParams[0] for its output, and puts a "
	      "mission's on the cleanup list");
	Check(BytesWithin(img, HandlerOf(img, op::DONT_REMOVE_CAR), 0x30, {0xB9, LE32(cleanup), 0x6A, 0x01}) &&
	          BytesWithin(img, HandlerOf(img, op::DONT_REMOVE_OBJECT), 0x30,
	                      {0xB9, LE32(cleanup), 0x6A, 0x03}),
	      "DONT_REMOVE_CAR and DONT_REMOVE_OBJECT take a car and an object off it");
	const uint32_t deleteCar = HandlerOf(img, op::DELETE_CAR);
	Check(FirstCallIn(img, deleteCar + 0x25, 0x10) == 0x004AE9D0 &&
	          BytesWithin(img, deleteCar, 0x40, {0x6A, 0x01, 0xFF, 0x16}),
	      "DELETE_CAR takes the car out of the world and runs its deleting destructor");
	const uint32_t inCar = HandlerOf(img, op::IS_PLAYER_IN_CAR);
	Check(BytesWithin(img, inCar, 0x40, {0x8B, 0x0D, LE32(g::CPools__ms_pVehiclePool)}) &&
	          BytesWithin(img, inCar, 0x40, {0x80, 0xBD, 0x14, 0x03, 0x00, 0x00, 0x00}) &&
	          BytesWithin(img, inCar, 0x40, {0x39, 0x85, 0x10, 0x03, 0x00, 0x00}),
	      "IS_PLAYER_IN_CAR's second operand is the car, against the player's bInVehicle and "
	      "m_pMyVehicle");

	// What a mission asks of its pedestrian in a car is any seat, so one moved
	// along to make room for a player still passes (seatplan.h, SeatMove::Shift).
	Check(HandlerOf(img, 0x00DB) == 0x0043D8E7 &&
	          BytesWithin(img, 0x0043D8E7, 0x40, {0x80, 0xBF, 0x14, 0x03, 0x00, 0x00, 0x00}) &&
	          BytesWithin(img, 0x0043D8E7, 0x50, {0x8B, 0x97, 0x10, 0x03, 0x00, 0x00}),
	      "IS_CHAR_IN_CAR is bInVehicle and m_pMyVehicle, whatever the seat");
	Check(BytesAt(img, 0x00589BEB, {0x83, 0xBE, 0x24, 0x02, 0x00, 0x00, 0x2C, 0x75, -1, 0x39, 0x86,
	                                0x10, 0x03, 0x00, 0x00}),
	      "IS_CHAR_SITTING_IN_CAR is PED_DRIVING and m_pMyVehicle, whatever the seat");
	Check(BytesAt(img, 0x00551E72, {0x3A, 0x9D, 0xCC, 0x01, 0x00, 0x00, 0x73, -1, 0x0F, 0xB6, 0xC3,
	                                0x83, 0xBC, 0x85, 0xA8, 0x01, 0x00, 0x00, 0x00, 0x75}) &&
	          CallTargetAt(img, 0x004CF6D1) == 0x00551E10 &&
	          CallTargetAt(img, 0x004CF6DD) == 0x00551E10 && CallTargetAt(img, 0x004CF6E9) == 0x00551E10,
	      "the door's end seats a passenger in its own slot only if that slot is empty, so a "
	      "slot somebody is climbing into is his");
	Check(BytesAt(img, 0x004D85B0, {0x83, 0xBB, 0x64, 0x01, 0x00, 0x00, 0x0E}) &&
	          BytesAt(img, 0x004D85EF, {0x68, 0xB0, 0x36, 0x00, 0x00}),
	      "a mission char's passenger objective with the controls off has a 14 s timer");

	// Our player into a car (seatplan.h, PlanScriptedWheel).
	auto callsWithin = [&](uint32_t fn, uint32_t bytes, uint32_t target) {
		for (uint32_t i = 0; i + 5 <= bytes; ++i)
			if (CallTargetAt(img, fn + i) == target)
				return true;
		return false;
	};
	// How many ride in a car (missionaddr.h, GET_NUMBER_OF_PASSENGERS).
	const uint32_t riders = HandlerOf(img, op::GET_NUMBER_OF_PASSENGERS);
	const uint32_t room   = HandlerOf(img, 0x01EA);
	Check(riders == 0x0044266E && room == 0x004426B4 && CollectedBy(img, riders) == 1 &&
	          CollectedBy(img, room) == 1,
	      "GET_NUMBER_OF_PASSENGERS and its maximum take the car alone");
	Check(BytesWithin(img, riders, 0x30,
	                  {0x0F, 0xB6, 0x80, LE32(static_cast<uint32_t>(g::offs::VEH_NUM_PASSENGERS))}) &&
	          BytesWithin(img, room, 0x30,
	                      {0x0F, 0xB6, 0x80,
	                       LE32(static_cast<uint32_t>(g::offs::VEH_NUM_MAX_PASSENGERS))}) &&
	          callsWithin(riders, 0x40, g::CRunningScript__StoreParameters) &&
	          callsWithin(room, 0x40, g::CRunningScript__StoreParameters),
	      "and store the car's m_nNumPassengers and m_nNumMaxPassengers bytes, read once");

	const uint32_t asPassenger = HandlerOf(img, op::SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER);
	const uint32_t asDriver    = HandlerOf(img, op::SET_CHAR_OBJ_ENTER_CAR_AS_DRIVER);
	const uint32_t warpPlayer  = HandlerOf(img, op::WARP_PLAYER_INTO_CAR);
	const uint32_t warpChar    = HandlerOf(img, op::WARP_CHAR_INTO_CAR);
	Check(asPassenger == 0x00441FD0 && asDriver == 0x00442029 && warpPlayer == 0x0044BB17 &&
	          warpChar == 0x0044BB7C,
	      "the four car-entry instructions' handlers are where missionaddr.h says");
	Check(CollectedBy(img, asPassenger) == 2 && CollectedBy(img, asDriver) == 2 &&
	          CollectedBy(img, warpPlayer) == 2 && CollectedBy(img, warpChar) == 2,
	      "each takes two operands, the char or player and the car");
	Check(BytesWithin(img, asPassenger, 0x50, {0x6A, 0x0E, 0xE8}) &&
	          callsWithin(asPassenger, 0x50, g::CPed__SetObjective) &&
	          BytesWithin(img, asDriver, 0x50, {0x6A, 0x0F}) &&
	          callsWithin(asDriver, 0x50, g::CPed__SetObjective),
	      "the two objectives: ENTER_CAR_AS_PASSENGER (0Eh) and ENTER_CAR_AS_DRIVER (0Fh)");
	Check(BytesWithin(img, warpPlayer, 0x40, {0x69, 0xDB, LE32(g::offs::PLAYERINFO_STRIDE)}) &&
	          BytesWithin(img, warpPlayer, 0x40, {0x81, 0xC3, LE32(g::CWorld__Players)}) &&
	          BytesWithin(img, warpPlayer, 0x50, {0x6A, 0x0F}) &&
	          callsWithin(warpPlayer, 0x50, g::CPed__WarpPedIntoCar) &&
	          BytesWithin(img, warpChar, 0x50, {0x6A, 0x0F}) &&
	          callsWithin(warpChar, 0x50, g::CPed__WarpPedIntoCar),
	      "both warps set the driver's objective and warp; the player's ped is "
	      "CWorld::Players[n].m_pPed");
	Check(BytesAt(img, g::CPed__SetObjective + 0x23,
	              {0x8B, 0x83, 0x68, 0x01, 0x00, 0x00, 0x39, 0xF0, 0x75, -1, 0x85, 0xC0, 0x74}),
	      "SetObjective returns at once for the objective already stored (+168h)");
	Check(BytesWithin(img, g::FindPlayerVehicle, 0x20, {0x80, 0xB9, 0x14, 0x03, 0x00, 0x00, 0x00}) &&
	          BytesWithin(img, g::FindPlayerVehicle, 0x28, {0x8B, 0x81, 0x10, 0x03, 0x00, 0x00, 0xC3}),
	      "FindPlayerVehicle is m_pMyVehicle for any seat, a passenger's too");

	// The tutorial's flashing HUD item.
	const uint32_t flash = HandlerOf(img, op::FLASH_HUD_OBJECT);
	Check(flash == 0x0044F9F9 && CollectedBy(img, flash) == 1 &&
	          BytesWithin(img, flash, 0x20,
	                      {0x66, 0xA1, LE32(g::CTheScripts__ScriptParams), 0x66, 0xA3,
	                       LE32(CHud__m_ItemToFlash)}),
	      "FLASH_HUD_OBJECT stores its one operand in CHud::m_ItemToFlash");
	Check(BytesAt(img, 0x0050836B, {0x66, 0x83, 0x3D, LE32(CHud__m_ItemToFlash), HUD_ITEM_RADAR, 0x75}) &&
	          BytesAt(img, 0x00508375, {0xA1, LE32(g::CTimer__m_FrameCounter), 0x83, 0xE0, 0x08}) &&
	          BytesAt(img, 0x0050837F, {0x66, 0x83, 0x3D, LE32(CHud__m_ItemToFlash), HUD_ITEM_RADAR, 0x0F, 0x84}),
	      "and CHud::Draw leaves the radar out every other eight frames while it names the radar");
	Check(BytesWithin(img, 0x00437C10, 0x100, {0x66, 0x83, 0x0D, LE32(CHud__m_ItemToFlash), 0xFF}),
	      "the mission's own cleanup puts it back to none");

	// The blue marker a location check draws.
	Check(BytesAt(img, HIGHLIGHT_AREA, {0x83, 0xEC, 0x18, 0xD9, 0x44, 0x24, 0x30}) &&
	          FirstCallIn(img, HIGHLIGHT_AREA, 0x100) == 0x004B3A80,
	      "CTheScripts::HighlightImportantArea starts the way mission.cpp checks before hooking it");
	const uint32_t inArea3d = HandlerOf(img, op::IS_PLAYER_IN_AREA_3D);
	Check(BytesWithin(img, inArea3d, 0x100, {0x83, 0x3D, LE32(g::CTheScripts__ScriptParams + 7 * 4), 0x00}) &&
	          BytesWithin(img, inArea3d, 0x100, {0x03, 0x43, static_cast<int>(layout::SCRIPT_IP)}),
	      "IS_PLAYER_IN_AREA_3D draws it when its eighth operand is set, with this + ip for its id");
	uint32_t highlightCalls = 0, highlightCaller = 0;
	for (uint32_t va = 0x00401000; va + 5 < 0x005C0000; ++va)
		if (CallTargetAt(img, va) == HIGHLIGHT_AREA && ImageByte(img, va + 5) == 0x83 &&
		    ImageByte(img, va + 6) == 0xC4 && ImageByte(img, va + 7) == 0x18) {
			++highlightCalls;
			if (va > inArea3d && va < inArea3d + 0x100)
				highlightCaller = va;
		}
	Check(highlightCalls == 22 && highlightCaller != 0,
	      "and 22 calls in all hand it six dwords, that handler's among them");
	Check(BytesWithin(img, 0x00517810, 0x80, {0x6A, 0x04, 0x50, 0xE8}) &&
	          CallTargetAt(img, 0x0051787E) == 0x0051BB80 &&
	          BytesWithin(img, 0x00517810, 0x80, {0x68, 0xFF, 0x00, 0x00, 0x00, 0x68, 0x80, 0x00, 0x00, 0x00, 0x6A, 0x00}),
	      "what it ends in is a 3D marker in 0, 128, 255, under the id it was given");

	// The two opcodes that were wrong, and what their right numbers do.
	const uint32_t dropped = FirstCallIn(img, HandlerOf(img, op::IS_CAR_IN_MISSION_GARAGE) + 0x0D, 0x10);
	const uint32_t respray = FirstCallIn(img, HandlerOf(img, op::HAS_RESPRAY_HAPPENED) + 0x13, 0x10);
	const uint32_t state   = dropped ? ImageDword(img, dropped + 0x15) : 0;
	const uint32_t flag    = static_cast<uint32_t>(g::CGarages__aGarages + g::offs::GARAGE_RESPRAY_HAPPENED);
	Check(BytesAt(img, dropped + 0x12, {0x80, 0x3C, 0x85, -1, -1, -1, -1, 0x05, 0x0F, 0x94, 0xC0, 0xC3}) &&
	          state == g::CGarages__aGarages + g::offs::GARAGE_STATE &&
	          BytesAt(img, respray + 0x0B, {0x8A, 0x81, LE32(flag), 0xC6, 0x81, LE32(flag), 0x00}),
	      "IS_CAR_IN_MISSION_GARAGE (021C) only reads a garage's state; HAS_RESPRAY_HAPPENED clears "
	      "the flag it reads, in the same array");
	Check(CollectedBy(img, HandlerOf(img, 0x03D4)) == 2,
	      "03D4 is the import garage's question, with two operands");
	Check(BytesAt(img, 0x00421E74, {0xBE, LE32(g::CGarages__aGarages)}) &&
	          BytesAt(img, 0x00421EB1, {0x83, 0xFB, static_cast<int>(MISSION_GARAGES)}) &&
	          g::NUM_GARAGES == MISSION_GARAGES,
	      "CGarages::Update walks all 32 garages, which is every one a participant is asked about");
	// 016C and 016E are laid out alike: four operands, the ground test, then
	// the call 0x8E bytes in.
	const uint32_t overrideFn = CallTargetAt(img, HandlerOf(img, op::OVERRIDE_NEXT_RESTART) + 0x8E);
	const uint32_t cancelFn   = FirstCallIn(img, HandlerOf(img, op::CANCEL_OVERRIDE_RESTART), 0x08);
	Check(overrideFn != 0 && cancelFn != 0 && BytesAt(img, overrideFn + 4, {0xC6, 0x05}) &&
	          BytesAt(img, cancelFn, {0xC6, 0x05, LE32(ImageDword(img, overrideFn + 6)), 0x00, 0xC3}) &&
	          ImageByte(img, overrideFn + 10) == 0x01,
	      "OVERRIDE_NEXT_RESTART (016E) sets the flag CANCEL_OVERRIDE_RESTART clears");
	const uint32_t hospital = CallTargetAt(img, HandlerOf(img, 0x016C) + 0x8E);
	Check(hospital != 0 && hospital != overrideFn && !DwordWithin(img, hospital, 0x40, ImageDword(img, overrideFn + 6)),
	      "and 016C is something else, that never touches that flag");

	// The rest of what the missions lean on.
	const std::pair<int32_t, uint32_t> planes[] = {
	    {op::HAS_DRUG_PLANE_BEEN_SHOT_DOWN, g::CPlane__CesnaMissionStatus},
	    {op::HAS_DROP_OFF_PLANE_BEEN_SHOT_DOWN, g::CPlane__DropOffCesnaMissionStatus},
	};
	for (const auto &plane : planes) {
		const uint32_t h  = HandlerOf(img, plane.first);
		const uint32_t fn = CallTargetAt(img, h + 2);
		Check(BytesAt(img, h, {0x30, 0xDB, 0xE8}) &&
		          BytesAt(img, fn, {0x83, 0x3D, LE32(plane.second), 0x02, 0x0F, 0x94, 0xC0, 0xC3}),
		      "a Cessna's question takes no operand and only compares its status with 2");
	}
	const uint32_t brakes = HandlerOf(img, op::APPLY_BRAKES_TO_PLAYERS_CAR);
	Check(BytesWithin(img, brakes, 0x50, {0xC6, 0x80, 0xE0, 0, 0, 0, 1}) &&
	          BytesWithin(img, brakes, 0x50, {0xC6, 0x80, 0xE0, 0, 0, 0, 0}) &&
	          FirstCallIn(img, brakes + 0x0D, 0x20) == g::CPad__GetPad,
	      "APPLY_BRAKES_TO_PLAYERS_CAR writes the pad's +0xE0 and no car");
	Check(BytesWithin(img, HandlerOf(img, 0x0228), 0x40,
	                  {0x8A, 0x98, LE32(g::offs::AUTOMOBILE_BOMB), 0x30, 0xC9, 0x80, 0xE3,
	                   g::offs::AUTOMOBILE_BOMB_MASK}) &&
	          BytesWithin(img, HandlerOf(img, op::ARM_CAR_WITH_BOMB), 0x40,
	                      {0x80, 0xE3, 0xF8, 0x08, 0xC3, 0x88, 0x9E, LE32(g::offs::AUTOMOBILE_BOMB)}),
	      "a car's bomb is the low three bits of +0x4D9");

	// Every button a participant's replayed cutscene is held against.
	const uint32_t cut   = g::CCutsceneMgr__Update;
	const int      cross = static_cast<int>(g::pad::CROSS * 2);
	Check(BytesWithin(img, cut, 0x260, {0x66, 0x83, 0x78, static_cast<int>(g::pad::NEWSTATE) + cross, 0x00}) &&
	          BytesWithin(img, cut, 0x260, {0x66, 0x83, 0x78, static_cast<int>(g::pad::OLDSTATE) + cross, 0x00}) &&
	          DwordWithin(img, cut, 0x260, g::CPad__NewMouseControllerState + g::pad::MOUSE_LMB) &&
	          DwordWithin(img, cut, 0x260, g::CPad__OldMouseControllerState + g::pad::MOUSE_LMB) &&
	          DwordWithin(img, cut, 0x260, g::CPad__NewKeyState + g::pad::KEY_ENTER) &&
	          DwordWithin(img, cut, 0x260, g::CPad__OldKeyState + g::pad::KEY_ENTER) &&
	          DwordWithin(img, cut, 0x260, g::CPad__NewKeyState + g::pad::KEY_EXTENTER) &&
	          DwordWithin(img, cut, 0x260, g::CPad__OldKeyState + g::pad::KEY_EXTENTER) &&
	          DwordWithin(img, cut, 0x260, g::CPad__NewKeyState + g::pad::KEY_VK_KEYS + ' ' * 2) &&
	          DwordWithin(img, cut, 0x260, g::CPad__OldKeyState + g::pad::KEY_VK_KEYS + ' ' * 2),
	      "CCutsceneMgr::Update skips on Cross, the left button, either Enter and the space bar");

	// The lobby's new game.
	const uint32_t newGame = g::CMenuManager__DoSettingsBeforeStartingAGame;
	const uint32_t shut    = CallTargetAt(img, newGame + 0x56);
	Check(BytesAt(img, newGame + 0x4F, {0xC6, 0x05, LE32(g::FrontEndMenuManager + 0x114), 0x01}) &&
	          BytesAt(img, shut, {0xC6, 0x81, LE32(g::CMenuManager__m_bMenuActive - g::FrontEndMenuManager),
	                              0x00}) &&
	          BytesAt(img, newGame + 0x90, {0xC3}),
	      "DoSettingsBeforeStartingAGame sets m_bWantToRestart and shuts the menu, no arguments");
	Check(BytesAt(img, 0x004872AE, {0x89, 0xE9}) && CallTargetAt(img, 0x004872B0) == newGame &&
	          BytesAt(img, 0x0048729C, {0x83, 0xBB, -1, -1, -1, -1, 0x0A}) &&
	          BytesAt(img, 0x004872A5, {0x80, 0xBD, 0x16, 0x01, 0, 0, 0}),
	      "and it is what the first menu's New Game calls, on the menu itself");

	// What the missions detour: the prologue MinHook moves, and nothing in
	// the image that lands inside it.
	// Three shapes of prologue, and how many bytes of each MinHook takes.
	const int pushFour[]   = {0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, -1, -1, -1, -1};   // 10
	const int pushThreeB[] = {0x53, 0x56, 0x55, 0x89, 0xCB};                         // 5
	const int pushThree[]  = {0x53, 0x56, 0x55, 0x89, 0xCD};                         // 5
	const int getAt[]      = {0x53, 0x8B, 0x5C, 0x24, 0x08};                         // 5
	const int idle[]       = {0x83, 0xEC, 0x08, 0xE8, -1, -1, -1, -1};               // 8
	struct Detoured {
		uint32_t   fn;
		const int *prologue;
		uint32_t   stolen;
	};
	const Detoured detoured[] = {
	    {RANGE_0, pushThreeB, 5},  {RANGE_100, pushFour, 10}, {RANGE_200, pushFour, 10},
	    {RANGE_300, pushThree, 5}, {RANGE_400, pushFour, 10}, {RANGE_500, pushThree, 5},
	    {RANGE_600, pushFour, 10}, {RANGE_700, pushFour, 10}, {RANGE_800, pushFour, 10},
	    {RANGE_900, pushFour, 10}, {RANGE_1000, pushThree, 5}, {RANGE_1100, pushFour, 10},
	    {CPool_CPed__GetAt, getAt, 5},
	    {g::FrontendIdle, idle, 8},
	};
	const size_t textStart = 0x1000, textEnd = 0x1000 + 0x1E3000;
	for (const Detoured &t : detoured) {
		const uint32_t stolen = t.stolen;
		bool           opens  = true;
		for (uint32_t i = 0; i < stolen; ++i)
			opens = opens && (t.prologue[i] < 0 || ImageByte(img, t.fn + i) == t.prologue[i]);
		int into = 0;
		for (size_t o = textStart; o + 6 <= textEnd && o + 6 <= img.size(); ++o) {
			const uint32_t va = static_cast<uint32_t>(g::IMAGE_BASE + o);
			uint32_t       to = 0;
			if (img[o] == 0xE8 || img[o] == 0xE9)
				to = va + 5 + ImageDword(img, va + 1);
			else if (img[o] == 0x0F && (img[o + 1] & 0xF0) == 0x80)
				to = va + 6 + ImageDword(img, va + 2);
			if (to > t.fn && to < t.fn + stolen)
				++into;
		}
		// A short jump back into it can only come from its own body.
		for (uint32_t va = t.fn + stolen; va < t.fn + 0x100; ++va) {
			const uint8_t b = ImageByte(img, va);
			if (b == 0xEB || (b & 0xF0) == 0x70) {
				const uint32_t to =
				    va + 2 + static_cast<uint32_t>(static_cast<int8_t>(ImageByte(img, va + 1)));
				if (to > t.fn && to < t.fn + stolen)
					++into;
			}
		}
		for (size_t o = 0; o + 4 <= img.size(); ++o) {
			const uint32_t v = ImageDword(img, static_cast<uint32_t>(g::IMAGE_BASE + o));
			if (v > t.fn && v < t.fn + stolen)
				++into;
		}
		char what[160];
		std::snprintf(what, sizeof what,
		              "0x%08X opens as recorded, and nothing branches or points into its first %u bytes",
		              t.fn, stolen);
		Check(opens && into == 0, what);
	}
	}
}

// What the sky is made of and how the weather turns over (addresses.h, the
// time of day and weather), which a follower writes by hand.
void TestTheSkyAgainstTheImage() {
	std::printf("\nthe clock, the weather and the time cycle against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadGta3(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the sky's "
		            "addresses against one\n");
		return;
	}
	namespace g         = game;
	const uint32_t mins  = g::CClock__ms_nGameClockMinutes;
	const uint32_t hours = g::CClock__ms_nGameClockHours;
	const uint32_t oldW  = g::CWeather__OldWeatherType;
	const uint32_t newW  = g::CWeather__NewWeatherType;
	const uint32_t blend = g::CWeather__InterpolationValue;
	const uint32_t list  = g::CWeather__WeatherTypeInList;
	const uint32_t u     = g::CWeather__Update;
	Check(BytesAt(img, u, {0xA0, LE32(mins)}) && BytesAt(img, u + 0x16, {0xD8, 0x0D, LE32(0x005FFC48u)}) &&
	          ImageDword(img, 0x005FFC48) == 0x3C888889 &&
	          BytesAt(img, u + 0x24, {0xD8, 0x1D, LE32(blend)}) &&
	          BytesAt(img, u + 0xA0, {0xD9, 0x1D, LE32(blend)}),
	      "CWeather::Update's blend is the minutes times 1/60, and a new hour is it falling below "
	      "last frame's");
	Check(BytesAt(img, u + 0x34, {0x66, 0x83, 0x3D, LE32(g::CWeather__ForcedWeatherType), 0x00}) &&
	          BytesAt(img, u + 0x3C, {0x66, 0xA1, LE32(newW), 0x66, 0xA3, LE32(oldW)}) &&
	          BytesAt(img, u + 0x4A, {0xA1, LE32(list)}) && BytesAt(img, u + 0x5D, {0xA3, LE32(list)}) &&
	          BytesAt(img, u + 0x62, {0x66, 0x8B, 0x04, 0x5D, LE32(0x005FFBC8u)}),
	      "and at one the new type becomes the old, and the pin or the list's next is the new");
	const uint32_t t = g::CTimeCycle__Update;
	Check(CallTargetAt(img, 0x0048C9A2) == t && BytesAt(img, t + 1, {0xA0, LE32(mins)}) &&
	          BytesAt(img, t + 6, {0x0F, 0xB6, 0x1D, LE32(hours)}) &&
	          BytesAt(img, t + 0x36, {0x0F, 0xBF, 0x0D, LE32(oldW)}) &&
	          BytesAt(img, t + 0x5D, {0xD8, 0x25, LE32(blend)}) &&
	          BytesAt(img, t + 0xAB, {0x0F, 0xBF, 0x2D, LE32(newW)}),
	      "CTimeCycle::Update, from CGame::Process, reads the hour, the minute, both types and the "
	      "blend");
}
#undef LE32

} // namespace

void TestARaceDoesNotWaitAtItsCheckpoints() {
	std::printf("\na checkpoint against the clock or the rivals does not wait\n");
	Check(CheckpointsWait(21, false) && !CheckpointsWait(21, true),
	      "a story checkpoint waits, unless a countdown is on the screen");
	Check(!CheckpointsWait(7, false) && !CheckpointsWait(10, false) && !CheckpointsWait(3, false) &&
	          !CheckpointsWait(11, false) && !CheckpointsWait(14, false),
	      "the 4x4 runs, Mayhem, the RC runs and the odd jobs never wait");
	Check(!CheckpointsWait(40, false) && !CheckpointsWait(63, false) && CheckpointsWait(41, false),
	      "nor Turismo and Bling-Bling Scramble, the two races");

	const MissionArea cp = MissionAreaLocate2D(500.0f, 500.0f, 4.0f, 4.0f);
	MissionSync race = Fresh();   // alice, 0, owns Patriot Playground
	race.OnState(State(MISSION_STATE_RUNNING, 0, 7, 0x07), 0, 1000);
	g_roster.others = {{1, true, {502.0f, 500.0f, 0.0f}}, {2, true, {1540.0f, 500.0f, 0.0f}}};
	Check(race.AskCheckpoint(cp, 0, 2000) && SentCount<C_MissionCheckpoint>() == 0,
	      "the 4x4 run's checkpoint is the owner's alone, and nobody is reported missing");

	MissionSync timed = Fresh();   // alice owns a story mission, 21
	timed.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x07), 0, 1000);
	g_roster.others = {{1, true, {502.0f, 500.0f, 0.0f}}, {2, true, {1540.0f, 500.0f, 0.0f}}};
	Check(!timed.AskCheckpoint(cp, 0, 2000), "with no clock running it waits for carol");
	timed.SetTimerUp(true);
	Check(timed.AskCheckpoint(cp, 0, 2100), "with the countdown up it goes on at once");
	const C_MissionCheckpoint *c = LastSent<C_MissionCheckpoint>();
	Check(c && c->missingMask == 0, "and tells everybody nobody is waited for any more");
	timed.SetTimerUp(false);
	Check(!timed.AskCheckpoint(cp, 0, 2200), "the clock gone, the next one waits again");
}

void TestWhoeverFallsFarBehindIsBroughtAlong() {
	std::printf("\nwhoever falls far behind in a race is brought along\n");
	Vec3    at{};
	uint8_t slot = 0, count = 0;
	MissionSync bob = Fresh();   // bob, 1, in alice's Patriot Playground
	bob.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	bob.OnState(State(MISSION_STATE_RUNNING, 0, 7, 0x03), 1, 1000);
	g_roster.others = {{0, true, {0.0f, 0.0f, 0.0f}}};
	const Vec3 near{100.0f, 0.0f, 0.0f}, far{200.0f, 0.0f, 0.0f};
	bob.WatchBehind(near, 1, 2000);
	bob.WatchBehind(near, 1, 30000);
	Check(!bob.SummonPending(), "100 m behind is close enough to catch up");
	bob.WatchBehind(far, 1, 31000);
	bob.WatchBehind(far, 1, 31000 + MISSION_BEHIND_MS - 1);
	Check(!bob.SummonPending(), "200 m behind for less than ten seconds is not moved");
	bob.WatchBehind(near, 1, 42000);
	bob.WatchBehind(far, 1, 43000);
	bob.WatchBehind(far, 1, 43000 + MISSION_BEHIND_MS - 1);
	Check(!bob.SummonPending(), "coming back within range starts the count again");
	bob.WatchBehind(far, 1, 43000 + MISSION_BEHIND_MS);
	Check(bob.SummonPending() && bob.BehindMoves() == 1, "ten seconds far behind owes a move");
	Check(bob.TakeSummon(far, 1, 43000 + MISSION_BEHIND_MS, &at, &slot, &count) && at.x == 0.0f,
	      "at once, beside alice");

	MissionSync story = Fresh();   // bob in an untimed story mission waits at its checkpoints
	story.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	story.OnState(State(MISSION_STATE_RUNNING, 0, 21, 0x03), 1, 1000);
	g_roster.others = {{0, true, {0.0f, 0.0f, 0.0f}}};
	story.WatchBehind(far, 1, 2000);
	story.WatchBehind(far, 1, 60000);
	Check(!story.SummonPending(), "a story mission's checkpoints wait for him instead");
	story.SetTimerUp(true);
	story.WatchBehind(far, 1, 61000);
	story.WatchBehind(far, 1, 61000 + MISSION_BEHIND_MS);
	Check(story.SummonPending(), "unless its clock is running");
}

void TestAChargeIsTheOwnersAlone() {
	std::printf("\nwhat the mission charges is the owner's alone\n");
	MissionSync m = Fresh();   // bob, 1
	m.OnState(State(MISSION_STATE_RUNNING, 0, 38, 0x03), 1, 1000);
	S_MissionEffect fx;
	InitHeader(fx, 1000);
	fx.ownerId     = 0;
	fx.body.kind   = MISSION_EFFECT_PAY;
	const int32_t player = 0, charge = -100000, reward = 20000;
	const uint8_t head[] = {0x09, 0x01, 0x01};
	std::memcpy(fx.body.code, head, 3);
	std::memcpy(fx.body.code + 3, &player, 4);
	fx.body.code[7] = 0x01;
	std::memcpy(fx.body.code + 8, &charge, 4);
	fx.body.length = 12;
	m.OnEffect(fx, 1, false, 1000);
	Check(g_effects == 0, "Bomb Da Base's $100,000 is not taken from bob");
	std::memcpy(fx.body.code + 8, &reward, 4);
	m.OnEffect(fx, 1, false, 1000);
	Check(g_effects == 1, "the pay at its end is his too");
}

void TestWantedLevelsAreEverybodysOwn() {
	std::printf("\neverybody keeps and gets back their own wanted level\n");
	using namespace game::replay;
	std::vector<uint8_t> space(0x400, 0);
	// STORE_WANTED_LEVEL $PLAYER_CHAR $WANTED_4X4, the player in global 0x10.
	const int32_t playerChar = 0;
	std::memcpy(space.data() + 0x10, &playerChar, 4);
	const uint8_t store[] = {0x02, 0x10, 0x00, 0x02, 0x80, 0x00};
	std::memcpy(space.data() + 0x100, store, sizeof store);
	Encoded e;
	Check(Encode(0x01C0, space.data(), 0x400, 0x100, nullptr, &e) && e.length == 2 + 5 + 3 &&
	          e.code[7] == 0x02 && e.code[8] == 0x80 && e.code[9] == 0x00,
	      "STORE_WANTED_LEVEL goes with its global, for each machine to fill with its own");
	const uint8_t back[] = {0x02, 0x10, 0x00, 0x02, 0x80, 0x00};
	std::memcpy(space.data() + 0x120, back, sizeof back);
	Check(Encode(0x010D, space.data(), 0x400, 0x120, nullptr, &e) && e.length == 2 + 5 + 3 &&
	          e.code[7] == 0x02 && e.code[8] == 0x80,
	      "ALTER_WANTED_LEVEL from that global reads each machine's own");
	int32_t lit = 0;
	Check(!LiteralAt(e.code, e.length, 1, &lit), "and is no literal to anybody who asks");
	const uint8_t six[] = {0x02, 0x10, 0x00, 0x04, 0x06};
	std::memcpy(space.data() + 0x140, six, sizeof six);
	Check(Encode(0x010D, space.data(), 0x400, 0x140, nullptr, &e) && e.length == 2 + 5 + 5 &&
	          LiteralAt(e.code, e.length, 1, &lit) && lit == 6,
	      "a level written as a number still goes as that number");
}

void TestTheCarListsAreTheSessions() {
	std::printf("\nthe Import/Export and crane lists are the session's\n");
	static uint32_t engine[CAR_LISTS];
	static int      writes;
	std::memset(engine, 0, sizeof engine);
	writes     = 0;
	MissionSync m = Fresh();   // bob, 1
	g_bridge.ReadCarLists = [](uint32_t (&out)[CAR_LISTS]) {
		std::memcpy(out, engine, sizeof engine);
		return true;
	};
	g_bridge.WriteCarLists = [](const uint32_t (&in)[CAR_LISTS]) {
		++writes;
		for (uint8_t i = 0; i < CAR_LISTS; ++i)
			engine[i] |= in[i];
	};
	m.Tick(1, 1000);
	Check(SentCount<C_CarLists>() == 0, "nothing before the server has said it shares missions");
	m.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 1000);
	m.Tick(1, 1100);
	Check(SentCount<C_CarLists>() == 1, "then the lists are asked for, once");
	m.Tick(1, 5000);
	Check(SentCount<C_CarLists>() == 1, "and not again while nothing is new");

	S_CarLists s{};
	InitHeader(s, 1000);
	s.collected[0]              = 0x0005;   // two cars on Portland's board
	s.collected[CAR_LIST_CRANE] = 0x40;     // the police car at the crane
	m.OnCarLists(s);
	m.Tick(1, 5100);
	Check(writes == 1 && engine[0] == 0x0005 && engine[CAR_LIST_CRANE] == 0x40,
	      "the session's cars are ticked in this game");
	Check(m.CarListCarsTaken() == 3 && SentCount<C_CarLists>() == 1,
	      "three of them, and nothing is said back");
	m.Tick(1, 6000);
	Check(writes == 1, "and not written again");

	engine[1] |= 0x8000;   // bob brings Shoreside's last car
	m.Tick(1, 6600);
	const C_CarLists *c = LastSent<C_CarLists>();
	Check(SentCount<C_CarLists>() == 2 && c && c->collected[1] == 0x8000 && c->collected[0] == 0x0005,
	      "a car the session has not heard of is told");
	m.Tick(1, 7200);
	Check(SentCount<C_CarLists>() == 2, "not again before its answer has had time");
	m.Tick(1, 6600 + 2000);
	Check(SentCount<C_CarLists>() == 3, "and again while no answer comes");
	s.collected[1] = 0x8000;
	m.OnCarLists(s);
	m.Tick(1, 12000);
	Check(SentCount<C_CarLists>() == 3, "once the session has it, nothing more");

	std::memset(engine, 0, sizeof engine);   // a new game
	m.Tick(1, 13000);
	Check(engine[0] == 0x0005 && engine[1] == 0x8000 && engine[CAR_LIST_CRANE] == 0x40,
	      "a game that lost them gets them back");

	uint32_t into[CAR_LISTS] = {1, 0, 0, 0};
	const uint32_t same[CAR_LISTS] = {1, 0, 0, 0}, more[CAR_LISTS] = {2, 0, 0, 1};
	Check(!MergeCarLists(into, same) && MergeCarLists(into, more) && into[0] == 3 && into[3] == 1,
	      "the session's lists only ever grow");
}

// The same waits and moves under a host who changed the numbers they run on
// (S_MissionState: checkpointWaitS, catchUpM, behindM, behindS and
// MISSION_FLAG_TIMED_CHECKPOINTS).
void TestTheHostSaysHowLongAndHowFar() {
	std::printf("\nthe host's own checkpoint wait, catch-up and straggler distance\n");
	const MissionArea cp = MissionAreaLocate2D(500.0f, 500.0f, 4.0f, 4.0f);
	Vec3              at{};
	uint8_t           slot = 0, count = 0;

	S_MissionState quick = State(MISSION_STATE_RUNNING, 0, 21, 0x07);
	quick.checkpointWaitS = 20;
	MissionSync m = Fresh();   // alice, 0, owns a story mission
	m.OnState(quick, 0, 1000);
	Check(m.CheckpointWaitS() == 20, "the owner takes the host's 20 s");
	g_roster.others = {{1, true, {502.0f, 500.0f, 0.0f}}, {2, true, {1540.0f, 500.0f, 0.0f}}};
	Check(!m.AskCheckpoint(cp, 0, 10000) && !m.AskCheckpoint(cp, 0, 10000 + 19900),
	      "and waits for carol for as long as that");
	Check(m.AskCheckpoint(cp, 0, 10000 + 20000) && m.CheckpointsGivenUp() == 1,
	      "and no longer, a long way short of the minute");

	S_MissionState never = quick;
	never.checkpointWaitS = 0;
	MissionSync n = Fresh();
	g_roster.others = {{1, true, {502.0f, 500.0f, 0.0f}}, {2, true, {1540.0f, 500.0f, 0.0f}}};
	n.OnState(never, 0, 1000);
	Check(n.AskCheckpoint(cp, 0, 2000) && SentCount<C_MissionCheckpoint>() == 0,
	      "with a wait of 0 no checkpoint waits, and nobody is reported missing");

	S_MissionState race = State(MISSION_STATE_RUNNING, 0, 40, 0x07);   // Turismo
	race.flags = static_cast<uint8_t>(race.flags | MISSION_FLAG_TIMED_CHECKPOINTS);
	MissionSync r = Fresh();
	g_roster.others = {{1, true, {502.0f, 500.0f, 0.0f}}, {2, true, {1540.0f, 500.0f, 0.0f}}};
	r.OnState(race, 0, 1000);
	Check(!r.AskCheckpoint(cp, 0, 2000), "a race waits for carol once the host says races do");
	r.SetTimerUp(true);
	Check(!r.AskCheckpoint(cp, 0, 2100), "and so does one with its clock up");

	S_MissionState noCatchUp = State(MISSION_STATE_RUNNING, 0, 21, 0x0F);
	noCatchUp.catchUpM = 0;
	MissionSync late = Fresh();   // dave, 3, joins alice's mission a long way off
	g_roster.others = {{0, true, {100.0f, 100.0f, 10.0f}}};
	late.OnState(noCatchUp, 3, 1000);
	Check(!late.TakeSummon({2000.0f, -500.0f, 10.0f}, 3, 1000 + MISSION_SUMMON_DELAY_MS, &at,
	                       &slot, &count),
	      "with catch-up off, somebody who comes in late is not moved");

	S_MissionState wide = State(MISSION_STATE_RUNNING, 0, 21, 0x0F);
	wide.catchUpM = 500;
	MissionSync walk = Fresh();
	g_roster.others = {{0, true, {100.0f, 100.0f, 10.0f}}};
	walk.OnState(wide, 3, 1000);
	Check(!walk.TakeSummon({400.0f, 100.0f, 10.0f}, 3, 1000 + MISSION_SUMMON_DELAY_MS, &at, &slot,
	                       &count),
	      "and with it at 500 m, 300 m away walks");

	S_MissionState stragglers = State(MISSION_STATE_RUNNING, 0, 7, 0x03);   // a 4x4 run
	stragglers.behindM = 300;
	stragglers.behindS = 30;
	MissionSync bob = Fresh();
	bob.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	bob.OnState(stragglers, 1, 1000);
	g_roster.others = {{0, true, {0.0f, 0.0f, 0.0f}}};
	const Vec3 far{400.0f, 0.0f, 0.0f}, middling{200.0f, 0.0f, 0.0f};
	bob.WatchBehind(middling, 1, 2000);
	bob.WatchBehind(middling, 1, 60000);
	Check(!bob.SummonPending(), "200 m behind is fine when the host says 300");
	bob.WatchBehind(far, 1, 61000);
	bob.WatchBehind(far, 1, 61000 + 29999);
	Check(!bob.SummonPending(), "400 m behind for under the host's 30 s is not moved");
	bob.WatchBehind(far, 1, 61000 + 30000);
	Check(bob.SummonPending() && bob.TakeSummon(far, 1, 61000 + 30000, &at, &slot, &count),
	      "and is brought along at 30 s");

	S_MissionState off = stragglers;
	off.behindM = 0;
	MissionSync stays = Fresh();
	g_roster.others = {{0, true, {0.0f, 0.0f, 0.0f}}};
	stays.OnState(State(MISSION_STATE_IDLE, INVALID_PLAYER, MISSION_NONE, 0), 1, 500);
	stays.OnState(off, 1, 1000);
	stays.WatchBehind({5000.0f, 0.0f, 0.0f}, 1, 2000);
	stays.WatchBehind({5000.0f, 0.0f, 0.0f}, 1, 200000);
	Check(!stays.SummonPending(), "and with the straggler distance at 0, nobody ever is");
}

int RunMissionTests() {
	TestTheHostSaysHowLongAndHowFar();
	TestARaceDoesNotWaitAtItsCheckpoints();
	TestWhoeverFallsFarBehindIsBroughtAlong();
	TestAChargeIsTheOwnersAlone();
	TestWantedLevelsAreEverybodysOwn();
	TestTheCarListsAreTheSessions();
	TestWhatAMissionLeavesBehind();
	TestWhatAMissionWritesIntoTheCampaign();
	TestTheCampaignInTheScript();
	TestEverybodyStandsBesideTheOwner();
	TestAPedestriansMoveLeavesTheCar();
	TestAConditionIsWidenedToEverybody();
	TestEachLaunchKnowsItsKind();
	TestTheEnemiesStandUpToMorePlayers();
	TestTheWidgetsFollowTheOwner();
	TestTheScriptEngineAgainstTheImage();
	TestTheMovesAgainstTheImage();
	TestTheSkyAgainstTheImage();
	TestWhatTheOwnersMissionShows();
	TestAnInstructionWaitsForTheCopyItNames();
	TestWhatTheMissionHasUpIsKept();
	TestSomebodyWhoComesInIsHandedWhatIsUp();
	TestTheCutsceneWaitsForEverybodysModels();
	TestTheSeatsTheMissionNeedsAreKept();
	TestOnlyAsManyGetOutAsTheSeatsNeeded();
	TestEverybodyIntoTheCarTheMissionPutItsPlayerIn();
	TestTheWaterDoesNotWaitForTheBoatless();
	TestAnObjectBrokenAnywhereIsBrokenEverywhere();
	TestACopyCountsLikeTheOriginal();
	TestOnlyWhoeverGotOutIsToldToGetBackIn();
	TestTheTutorialsFlashGoesToEverybody();
	TestTheBlueMarkersGoToEverybody();
	TestAFloatingPackageTakenAnywhereCounts();
	TestAGameInAMissionOfItsOwn();
	TestTwoNewGamesMeetAtTheBridge();
	TestEverybodysKillsCount();
	TestTheMissionsWordToACar();
	TestAHelpersGarageIsHeard();
	TestAnInstructionIsSentAsValues();
	TestTheStartGatesAreToldApart();
	TestTheLaunchAhead();
	TestTheConditionsAreas();
	TestTheDeathRuleUnwinds();
	TestAnOlderServerLeavesTheGateAlone();
	TestAGateIsClaimedUntilGranted();
	TestAGrantGoesStale();
	TestWhatEverybodyIsTold();
	TestAParticipantFollowsTheSessionsMission();
	TestTheOwnersSide();
	TestComingIntoAMissionAsksForWhatItMade();
	TestACheckpointWaitsForEverybody();
	TestACheckpointDoesNotWaitForEver();
	TestWhoIsWaitedForIsOnTheHud();
	TestALateParticipantIsBroughtToTheOwner();
	TestAnOwnerWhoDroppedOffIsTakenUpAgain();
	return g_missionFailures;
}
