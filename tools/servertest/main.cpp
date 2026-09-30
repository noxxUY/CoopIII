// The server's own wiring, end to end: a real Server in this process and real
// NetClients over loopback, so what is checked is what reaches a client and
// not what Session or MissionSlot decided. sessiontest covers the decisions;
// this covers who hears them, in what order, and with what in the packet.
//
//   xmake build servertest && xmake run servertest
//
// Needs nothing else running. It listens on a port of its own, and says it
// was skipped, rather than failed, when it cannot bind one.
//
// Written after a packet went out empty: InitHeader zeroes the whole packet,
// and the mission's state was stamped after MissionSlot had filled it in.

#include "server.h"

#include "coopiii/mission.h"
#include "coopiii/net.h"
#include "launcher/lobby.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace coopiii;

namespace {

int  g_failures = 0;
bool g_verbose  = false;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

struct Rig {
	Server                            server;
	uint16_t                          port = 0;
	std::vector<NetClient *>          clients;
	std::vector<std::vector<Message>> inbox;

	// Services the server and every client for up to `ms`, stopping early
	// once `done` says so.
	void Pump(uint32_t ms, const std::function<bool()> &done) {
		using namespace std::chrono;
		const auto until = steady_clock::now() + milliseconds(ms);
		while (steady_clock::now() < until) {
			server.Tick(0);
			for (size_t i = 0; i < clients.size(); ++i)
				if (clients[i])
					clients[i]->Service(inbox[i]);
			if (done && done())
				return;
			std::this_thread::sleep_for(milliseconds(1));
		}
	}

	void Settle(uint32_t ms = 150) { Pump(ms, nullptr); }
	void ClearInboxes() {
		for (auto &box : inbox)
			box.clear();
	}
};

template <class T>
const T *Last(const std::vector<Message> &box) {
	for (auto it = box.rbegin(); it != box.rend(); ++it)
		if (const T *p = it->as<T>())
			return p;
	return nullptr;
}

template <class T>
size_t Count(const std::vector<Message> &box) {
	size_t n = 0;
	for (const Message &m : box)
		if (m.as<T>())
			++n;
	return n;
}

size_t IndexOf(const std::vector<Message> &box, uint8_t opcode) {
	for (size_t i = 0; i < box.size(); ++i)
		if (box[i].opcode == opcode)
			return i;
	return box.size();
}

C_Hello Hello(const char *nick) {
	C_Hello hello;
	InitHeader(hello, 0);
	hello.protocolVersion = PROTOCOL_VERSION;
	std::strncpy(hello.nick, nick, NICK_LEN - 1);
	return hello;
}

C_PlayerState StateAt(float x, float y, float z, uint8_t pedState = 0) {
	C_PlayerState s;
	InitHeader(s, 100);
	s.body.pos      = {x, y, z};
	s.body.health   = 100.0f;
	s.body.pedState = pedState;
	return s;
}

C_MissionClaim Claim(const MissionArea &area, uint16_t hint) {
	C_MissionClaim c;
	InitHeader(c, 100);
	c.launchKey   = 0x4242;
	c.missionHint = hint;
	c.kind        = MISSION_KIND_STORY;
	c.area        = area;
	return c;
}

// Connects client `i` and says hello, returning its player id.
uint8_t Join(Rig &rig, size_t i, const char *nick) {
	NetClient *c = rig.clients[i];
	if (!c->Connect("127.0.0.1", rig.port))
		return INVALID_PLAYER;
	rig.Pump(3000, [&] { return c->IsConnected(); });
	c->Send(Hello(nick), CH_EVENT);
	rig.Pump(3000, [&] { return Last<S_Welcome>(rig.inbox[i]) != nullptr; });
	const S_Welcome *w = Last<S_Welcome>(rig.inbox[i]);
	return w ? w->playerId : INVALID_PLAYER;
}

void TestTheSessionsMission(Rig &rig) {
	std::printf("\nthe session's mission, as every client hears it\n");
	const uint8_t alice = Join(rig, 0, "alice");
	const uint8_t bob   = Join(rig, 1, "bob");
	const uint8_t carol = Join(rig, 2, "carol");
	Check(alice == 0 && bob == 1 && carol == 2, "three players, in slots 0, 1 and 2");
	rig.Settle();
	for (size_t i = 0; i < 3; ++i) {
		const S_MissionState *st = Last<S_MissionState>(rig.inbox[i]);
		if (!st || st->state != MISSION_STATE_IDLE || st->marginCm != 500) {
			Check(false, "every joiner is told the session's mission, idle, and the 5 m margin");
			break;
		}
		if (i == 2)
			Check(true, "every joiner is told the session's mission, idle, and the 5 m margin");
	}

	rig.clients[0]->Send(StateAt(100.0f, 100.0f, 10.0f), CH_SNAPSHOT);
	rig.clients[1]->Send(StateAt(103.0f, 100.0f, 10.0f), CH_SNAPSHOT);
	rig.Settle();
	rig.ClearInboxes();

	const MissionArea marker = MissionAreaLocate3D(100.0f, 100.0f, 10.0f, 1.5f, 1.5f, 2.0f);
	rig.clients[0]->Send(Claim(marker, 19), CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionClaim>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	const S_MissionClaim *answer = Last<S_MissionClaim>(rig.inbox[0]);
	Check(answer && answer->verdict == MISSION_CLAIM_WAITING && answer->launchKey == 0x4242 &&
	          answer->missingMask == PlayerBit(carol),
	      "alice's claim waits for carol, who has not said where they are");
	const S_MissionWaiting *told = Last<S_MissionWaiting>(rig.inbox[2]);
	Check(told && told->ownerId == alice && told->missingMask == PlayerBit(carol) &&
	          told->what == MISSION_WAIT_START && told->missionHint == 19 && told->where.x == 100.0f,
	      "and carol hears that alice is waiting for carol at the start of Give Me Liberty");
	Check(Last<S_MissionClaim>(rig.inbox[1]) == nullptr, "the answer is alice's alone");

	rig.clients[2]->Send(StateAt(106.0f, 100.0f, 10.0f), CH_SNAPSHOT);
	rig.Settle();
	rig.ClearInboxes();
	rig.clients[0]->Send(Claim(marker, 19), CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionClaim>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	answer = Last<S_MissionClaim>(rig.inbox[0]);
	Check(answer && answer->verdict == MISSION_CLAIM_GRANTED, "with carol beside the marker it is granted");
	told = Last<S_MissionWaiting>(rig.inbox[1]);
	Check(told && told->what == MISSION_WAIT_NONE, "and the wait is over on everybody's screen");

	// The title the trigger prints between the grant and the launch.
	rig.ClearInboxes();
	C_MissionEffect title;
	InitHeader(title, 100);
	title.body.missionNumber = 19;
	title.body.kind          = MISSION_EFFECT_RUN;
	title.body.length        = 2;
	title.body.code[0]       = 0xBA;
	rig.clients[0]->Send(title, CH_EVENT);
	rig.clients[1]->Send(title, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionEffect>(rig.inbox[2]) != nullptr; });
	rig.Settle();
	const S_MissionEffect *shown = Last<S_MissionEffect>(rig.inbox[2]);
	Check(shown && shown->ownerId == alice && shown->body.code[0] == 0xBA &&
	          Count<S_MissionEffect>(rig.inbox[2]) == 1,
	      "the title alice's trigger prints reaches carol, and bob's does not");
	Check(Last<S_MissionEffect>(rig.inbox[0]) == nullptr, "and never goes back to alice");

	rig.ClearInboxes();
	C_MissionStarted started;
	InitHeader(started, 100);
	started.launchKey     = 0x4242;
	started.missionNumber = 19;
	rig.clients[0]->Send(started, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionState>(rig.inbox[2]) != nullptr; });
	const S_MissionState *st = Last<S_MissionState>(rig.inbox[2]);
	Check(st && st->state == MISSION_STATE_RUNNING && st->ownerId == alice &&
	          st->missionNumber == 19 && st->participants == 0x07 &&
	          (st->flags & MISSION_FLAG_FAIL_ON_DEATH) != 0,
	      "alice's start is everybody's: running, hers, with all three in it");
	const uint32_t campaignLog = st ? st->campaignLog : 0;
	Check(campaignLog != 0, "and names the campaign log this run of the server keeps");

	rig.ClearInboxes();
	rig.clients[1]->Send(title, CH_EVENT);
	rig.clients[0]->Send(title, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionEffect>(rig.inbox[1]) != nullptr; });
	rig.Settle();
	Check(Count<S_MissionEffect>(rig.inbox[1]) == 1 && Count<S_MissionEffect>(rig.inbox[2]) == 1,
	      "while it runs only alice's mission is shown, once to each of the others");

	// The value behind the mission's timer.
	rig.ClearInboxes();
	C_MissionWidget widget{};
	InitHeader(widget, 100);
	widget.missionNumber = 19;
	widget.offset        = 0x144;
	widget.value         = 60000;
	rig.clients[1]->Send(widget, CH_EVENT);   // bob does not own the mission
	rig.clients[0]->Send(widget, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionWidget>(rig.inbox[2]) != nullptr; });
	rig.Settle();
	const S_MissionWidget *value = Last<S_MissionWidget>(rig.inbox[2]);
	Check(value && value->ownerId == alice && value->offset == 0x144 && value->value == 60000 &&
	          Count<S_MissionWidget>(rig.inbox[2]) == 1 && Count<S_MissionWidget>(rig.inbox[1]) == 1 &&
	          !Last<S_MissionWidget>(rig.inbox[0]),
	      "alice's timer reaches bob and carol, and bob's reaches nobody");

	rig.ClearInboxes();
	C_Death death;
	InitHeader(death, 100);
	death.killerNetId = INVALID_NETID;
	rig.clients[1]->Send(death, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionFail>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	const S_MissionFail *fail = Last<S_MissionFail>(rig.inbox[0]);
	Check(fail && fail->reason == MISSION_FAIL_DIED && fail->playerId == bob &&
	          fail->missionNumber == 19,
	      "bob dying tells alice to fail it");
	Check(!Last<S_MissionFail>(rig.inbox[1]) && !Last<S_MissionFail>(rig.inbox[2]),
	      "and only alice, whose script runs it");
	const S_PlayerScore *row = Last<S_PlayerScore>(rig.inbox[2]);
	Check(row && row->playerId == bob && row->body.deaths == 1 && row->body.kills == 0 &&
	          Last<S_PlayerScore>(rig.inbox[1]) != nullptr,
	      "and the scoreboard's row for bob, one death, reaches everybody, bob included");

	// What the mission left behind: numbered, kept, and sent to everybody.
	rig.ClearInboxes();
	C_CampaignDelta delta;
	InitHeader(delta, 100);
	delta.body.missionNumber   = 19;
	delta.body.valueCount      = 1;
	delta.body.values[0].offset = 900;
	delta.body.values[0].value  = 1;
	delta.body.last            = 1;
	rig.clients[1]->Send(delta, CH_EVENT);   // bob does not own it
	rig.clients[0]->Send(delta, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_CampaignDelta>(rig.inbox[2]) != nullptr; });
	rig.Settle();
	const S_CampaignDelta *left = Last<S_CampaignDelta>(rig.inbox[2]);
	Check(left && left->body.seq == 1 && left->ownerId == alice && left->body.values[0].offset == 900 &&
	          Count<S_CampaignDelta>(rig.inbox[2]) == 1,
	      "alice's mission's delta is number 1 on carol's machine, and bob's is nobody's");
	Check(Last<S_CampaignDelta>(rig.inbox[0]) != nullptr, "alice hears the number too");

	rig.ClearInboxes();
	C_MissionEnded ended;
	InitHeader(ended, 100);
	ended.missionNumber = 19;
	ended.outcome       = MISSION_OUTCOME_FAILED;
	rig.clients[0]->Send(ended, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionState>(rig.inbox[1]) != nullptr; });
	st = Last<S_MissionState>(rig.inbox[1]);
	Check(st && st->state == MISSION_STATE_IDLE && st->outcome == MISSION_OUTCOME_FAILED &&
	          st->missionNumber == 19,
	      "alice's end is everybody's: idle, failed");

	// The next one, and a bust.
	rig.ClearInboxes();
	started.missionNumber = 20;
	rig.clients[0]->Send(started, CH_EVENT);
	rig.Settle();
	rig.clients[2]->Send(StateAt(106.0f, 100.0f, 10.0f, PEDSTATE_ON_WIRE_ARRESTED), CH_SNAPSHOT);
	rig.clients[2]->Send(StateAt(106.0f, 100.0f, 10.0f, PEDSTATE_ON_WIRE_ARRESTED), CH_SNAPSHOT);
	rig.Pump(1500, [&] { return Last<S_MissionFail>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	fail = Last<S_MissionFail>(rig.inbox[0]);
	Check(fail && fail->reason == MISSION_FAIL_BUSTED && fail->playerId == carol &&
	          fail->missionNumber == 20,
	      "carol busted is the other half of the rule");
	Check(Count<S_MissionFail>(rig.inbox[0]) == 1, "once, however many snapshots say it");

	rig.ClearInboxes();
	const uint8_t dave = Join(rig, 3, "dave");
	C_CampaignSince since;
	InitHeader(since, 100);
	since.seq = 0;
	rig.clients[3]->Send(since, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_CampaignDelta>(rig.inbox[3]) != nullptr; });
	const S_CampaignDelta *caught = Last<S_CampaignDelta>(rig.inbox[3]);
	Check(caught && caught->body.seq == 1 && caught->body.values[0].value == 1,
	      "a machine asking for what it missed is sent the log since its number");
	rig.Pump(1500, [&] { return Last<S_MissionState>(rig.inbox[3]) != nullptr; });
	rig.Settle();
	st = Last<S_MissionState>(rig.inbox[3]);
	Check(dave == 3 && st && st->state == MISSION_STATE_RUNNING &&
	          (st->participants & PlayerBit(dave)) != 0 && st->campaignLog == campaignLog,
	      "dave arriving in the middle of it is in it, and told so, with the same log");
	st = Last<S_MissionState>(rig.inbox[1]);
	Check(st && (st->participants & PlayerBit(dave)) != 0, "and everybody else is told too");

	// Bob's models are in, which alice's mission waits on (missions.md 11.3).
	rig.ClearInboxes();
	C_MissionReady ready{};
	InitHeader(ready, 100);
	ready.missionNumber = 20;
	ready.readySeq      = 1;
	rig.clients[1]->Send(ready, CH_EVENT);
	rig.clients[0]->Send(ready, CH_EVENT);   // alice owns it: nobody to tell
	ready.missionNumber = 19;
	rig.clients[2]->Send(ready, CH_EVENT);   // carol, of a mission that is not running
	rig.Pump(1500, [&] { return Last<S_MissionReady>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	const S_MissionReady *loaded = Last<S_MissionReady>(rig.inbox[0]);
	Check(loaded && loaded->playerId == bob && loaded->readySeq == 1 &&
	          loaded->missionNumber == 20 && Count<S_MissionReady>(rig.inbox[0]) == 1,
	      "bob's models being in reaches alice, whose mission waits on them");
	Check(!Last<S_MissionReady>(rig.inbox[1]) && !Last<S_MissionReady>(rig.inbox[2]) &&
	          !Last<S_MissionReady>(rig.inbox[3]),
	      "and nobody else; alice's own and carol's about another mission reach nobody");

	// The seats alice's mission's passengers need (mission-audit.md R4).
	rig.ClearInboxes();
	C_MissionSeats seats{};
	InitHeader(seats, 100);
	seats.missionNumber = 20;
	seats.count         = 1;
	seats.cars[0]       = {700, MISSION_SEAT_KEPT | MISSION_SEAT_LEAVE, PlayerBit(2)};
	rig.clients[1]->Send(seats, CH_EVENT);   // bob does not own it
	rig.clients[0]->Send(seats, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionSeats>(rig.inbox[3]) != nullptr; });
	rig.Settle();
	const S_MissionSeats *kept = Last<S_MissionSeats>(rig.inbox[3]);
	Check(kept && kept->ownerId == alice && kept->count == 1 && kept->cars[0].netId == 700 &&
	          kept->cars[0].flags == (MISSION_SEAT_KEPT | MISSION_SEAT_LEAVE) &&
	          Count<S_MissionSeats>(rig.inbox[1]) == 1 && Count<S_MissionSeats>(rig.inbox[2]) == 1 &&
	          !Last<S_MissionSeats>(rig.inbox[0]),
	      "the car alice's mission needs the seats of reaches everybody else, and bob's nobody");
	Check(kept && kept->cars[0].leave == PlayerBit(2), "with who is to get out of it");

	// The seats in the car alice's mission put her in.
	rig.ClearInboxes();
	C_MissionBoard board{};
	InitHeader(board, 100);
	board.missionNumber = 20;
	board.netId         = 700;
	board.flags         = MISSION_BOARD_WATER;
	board.seats[1]      = 1;
	rig.clients[1]->Send(board, CH_EVENT);   // bob does not own it
	rig.clients[0]->Send(board, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionBoard>(rig.inbox[3]) != nullptr; });
	rig.Settle();
	const S_MissionBoard *boarded = Last<S_MissionBoard>(rig.inbox[3]);
	Check(boarded && boarded->ownerId == alice && boarded->netId == 700 &&
	          boarded->missionNumber == 20 && boarded->flags == MISSION_BOARD_WATER &&
	          boarded->seats[1] == 1 && boarded->seats[2] == 0 &&
	          Count<S_MissionBoard>(rig.inbox[1]) == 1 && Count<S_MissionBoard>(rig.inbox[2]) == 1 &&
	          !Last<S_MissionBoard>(rig.inbox[0]),
	      "the seats in the boat alice's mission put her in reach everybody else, and bob's "
	      "nobody");

	// One of alice's mission's objects, broken on carol's machine (R3).
	rig.ClearInboxes();
	C_MissionObjectBreak brk{};
	InitHeader(brk, 100);
	brk.missionNumber = 20;
	brk.global        = 0x0B20;
	brk.amount        = 500.0f;
	brk.state         = 2;
	rig.clients[2]->Send(brk, CH_EVENT);
	brk.missionNumber = 19;
	rig.clients[1]->Send(brk, CH_EVENT);   // bob, about a mission that is not running
	rig.Pump(1500, [&] { return Last<S_MissionObjectBreak>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	const S_MissionObjectBreak *smashed = Last<S_MissionObjectBreak>(rig.inbox[0]);
	Check(smashed && smashed->playerId == carol && smashed->global == 0x0B20 &&
	          smashed->state == 2 && smashed->amount == 500.0f &&
	          Count<S_MissionObjectBreak>(rig.inbox[0]) == 1 &&
	          Count<S_MissionObjectBreak>(rig.inbox[1]) == 1 &&
	          Count<S_MissionObjectBreak>(rig.inbox[3]) == 1 && !Last<S_MissionObjectBreak>(rig.inbox[2]),
	      "an object carol broke reaches everybody else, alice's mission included, and bob's "
	      "about another mission reaches nobody");

	// One of alice's mission's floating packages, taken on dave's machine (R2).
	rig.ClearInboxes();
	C_MissionPickup took2{};
	InitHeader(took2, 100);
	took2.missionNumber = 20;
	took2.handle        = 0x2A0007;
	rig.clients[3]->Send(took2, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionPickup>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	const S_MissionPickup *package = Last<S_MissionPickup>(rig.inbox[0]);
	Check(package && package->playerId == dave && package->handle == 0x2A0007 &&
	          Count<S_MissionPickup>(rig.inbox[1]) == 1 && Count<S_MissionPickup>(rig.inbox[2]) == 1 &&
	          !Last<S_MissionPickup>(rig.inbox[3]),
	      "a package dave takes reaches everybody else, alice's mission included");

	// What alice's mission has up, handed to dave alone (missions.md 11.5).
	rig.ClearInboxes();
	C_MissionEffect handed    = title;
	handed.body.missionNumber = 20;
	handed.body.onlyTo        = static_cast<uint8_t>(dave + 1);
	rig.clients[1]->Send(handed, CH_EVENT);   // bob does not own it
	rig.clients[0]->Send(handed, CH_EVENT);
	handed.body.onlyTo = static_cast<uint8_t>(alice + 1);
	rig.clients[0]->Send(handed, CH_EVENT);   // alice has it all already
	rig.Pump(1500, [&] { return Last<S_MissionEffect>(rig.inbox[3]) != nullptr; });
	rig.Settle();
	const S_MissionEffect *toDave = Last<S_MissionEffect>(rig.inbox[3]);
	Check(toDave && toDave->ownerId == alice && toDave->body.onlyTo == dave + 1 &&
	          Count<S_MissionEffect>(rig.inbox[3]) == 1,
	      "what alice's mission has up reaches dave, who has just come in, marked as dave's alone");
	Check(!Last<S_MissionEffect>(rig.inbox[0]) && !Last<S_MissionEffect>(rig.inbox[1]) &&
	          !Last<S_MissionEffect>(rig.inbox[2]),
	      "and nobody else: not bob's copy, nor the one alice addressed to alice");

	// Dave's game goes into a new game's intro, comes out of it, and then
	// loads a save in the middle of alice's mission (missions.md 11.6).
	rig.ClearInboxes();
	C_MissionBusy busy{};
	InitHeader(busy, 100);
	busy.busy = 1;
	rig.clients[3]->Send(busy, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionState>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	st = Last<S_MissionState>(rig.inbox[0]);
	Check(st && st->state == MISSION_STATE_RUNNING && (st->participants & PlayerBit(dave)) == 0,
	      "dave's game in its own intro is out of alice's mission, and everybody is told");
	rig.ClearInboxes();
	busy.busy = 0;
	rig.clients[3]->Send(busy, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionState>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	st = Last<S_MissionState>(rig.inbox[0]);
	Check(st && (st->participants & PlayerBit(dave)) != 0,
	      "and back in it, as a joiner, once the intro is over");
	rig.ClearInboxes();
	busy.fresh = 1;
	rig.clients[3]->Send(busy, CH_EVENT);
	rig.clients[1]->Send(busy, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionHandOver>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	const S_MissionHandOver *over = Last<S_MissionHandOver>(rig.inbox[0]);
	Check(over && Count<S_MissionHandOver>(rig.inbox[0]) == 2 && over->missionNumber == 20 &&
	          !Last<S_MissionState>(rig.inbox[0]) && !Last<S_MissionHandOver>(rig.inbox[3]),
	      "a game that started over in the middle of it has alice hand it over again, and "
	      "nothing else changes");

	// Bob kills a Diablo his own machine hosts, and alice's mission counts it.
	rig.ClearInboxes();
	C_MissionKill kill{};
	InitHeader(kill, 100);
	kill.missionNumber = 20;
	kill.model         = 13;
	rig.clients[1]->Send(kill, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionKill>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	const S_MissionKill *counted = Last<S_MissionKill>(rig.inbox[0]);
	Check(counted && counted->playerId == bob && counted->model == 13 &&
	          counted->missionNumber == 20 && !Last<S_MissionKill>(rig.inbox[2]) &&
	          !Last<S_MissionKill>(rig.inbox[1]),
	      "a kill bob's machine registers goes to alice, whose mission counts it, and nobody else");
	rig.ClearInboxes();
	rig.clients[0]->Send(kill, CH_EVENT);
	kill.missionNumber = 21;
	rig.clients[2]->Send(kill, CH_EVENT);
	rig.Settle(300);
	Check(Count<S_MissionKill>(rig.inbox[0]) == 0 && Count<S_MissionKill>(rig.inbox[1]) == 0,
	      "alice's own, and one for another mission, go nowhere");

	// Bob drives the mission's car into the lock-up, and his garage shuts on it.
	rig.ClearInboxes();
	C_MissionAnswers garage{};
	InitHeader(garage, 100);
	garage.missionNumber = 20;
	garage.hasCar        = 1u << 7;
	rig.clients[1]->Send(garage, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionAnswers>(rig.inbox[0]) != nullptr; });
	rig.Settle();
	const S_MissionAnswers *shut = Last<S_MissionAnswers>(rig.inbox[0]);
	Check(shut && shut->playerId == bob && shut->hasCar == (1u << 7) && shut->missionNumber == 20 &&
	          !Last<S_MissionAnswers>(rig.inbox[2]),
	      "what bob's garages say goes to alice, whose mission asks, and nobody else");
	rig.ClearInboxes();
	rig.clients[0]->Send(garage, CH_EVENT);
	garage.missionNumber = 21;
	rig.clients[2]->Send(garage, CH_EVENT);
	rig.Settle(300);
	Check(Count<S_MissionAnswers>(rig.inbox[0]) == 0 && Count<S_MissionAnswers>(rig.inbox[1]) == 0,
	      "alice's own garages, and another mission's, go nowhere");

	// The mission's stash: alice takes her uzi, everybody hears it, and bob
	// still has his to take.
	rig.ClearInboxes();
	C_PickupClaim claim{};
	InitHeader(claim, 100);
	claim.ident.pos        = {100.0f, 101.0f, 10.0f};
	claim.ident.modelIndex = 172;
	claim.ident.type       = 3;
	claim.ident.flags      = PICKUP_F_STASH;
	rig.clients[0]->Send(claim, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_PickupGrant>(rig.inbox[0]) != nullptr; });
	C_PickupCollected took{};
	InitHeader(took, 100);
	took.ident = claim.ident;
	rig.clients[0]->Send(took, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_PickupTaken>(rig.inbox[1]) != nullptr; });
	rig.Settle();
	const S_PickupTaken *heard = Last<S_PickupTaken>(rig.inbox[1]);
	Check(heard && heard->playerId == alice && (heard->ident.flags & PICKUP_F_STASH) != 0 &&
	          !Last<S_PickupTaken>(rig.inbox[0]),
	      "alice taking her stash uzi is heard by everybody else, for the mission's sake");
	rig.clients[1]->Send(claim, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_PickupGrant>(rig.inbox[1]) != nullptr; });
	Check(Last<S_PickupGrant>(rig.inbox[1]) != nullptr && !Last<S_PickupDenied>(rig.inbox[1]),
	      "and bob is still granted his own");

	rig.ClearInboxes();
	rig.clients[0]->Disconnect();
	rig.clients[0] = nullptr;
	rig.Pump(3000, [&] { return Last<S_MissionState>(rig.inbox[1]) != nullptr; });
	rig.Settle();
	st = Last<S_MissionState>(rig.inbox[1]);
	Check(st && st->state == MISSION_STATE_IDLE && st->outcome == MISSION_OUTCOME_OWNER_LEFT,
	      "alice quitting fails it for everybody");
	Check(IndexOf(rig.inbox[1], OP_S_PLAYER_LEAVE) < IndexOf(rig.inbox[1], OP_S_MISSION_STATE),
	      "after they have heard alice left");
}

void TestTheHostKicks(Rig &rig) {
	std::printf("\nthe host kicks from inside the game\n");
	// alice has gone, so bob, the lowest slot left, is the host.
	rig.ClearInboxes();
	C_Kick kick;
	InitHeader(kick, 100);
	kick.playerId = 3;   // dave
	rig.clients[2]->Send(kick, CH_EVENT);
	rig.Settle(400);
	Check(Count<S_PlayerLeave>(rig.inbox[1]) == 0 && rig.clients[3]->IsConnected(),
	      "carol is not the host, and dave stays");
	rig.clients[1]->Send(kick, CH_EVENT);
	rig.Pump(3000, [&] { return Last<S_PlayerLeave>(rig.inbox[2]) != nullptr; });
	rig.Settle(400);
	const S_PlayerLeave *gone = Last<S_PlayerLeave>(rig.inbox[2]);
	Check(gone && gone->playerId == 3 && gone->reason == LEAVE_KICKED,
	      "bob, the host, throws dave out, and everybody reads that dave was kicked");
}

uint32_t Ms() {
	using namespace std::chrono;
	return static_cast<uint32_t>(
	    duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// A connection that is not a launcher, knocking on the lobby by hand.
struct Raw {
	NetClient            net;
	std::vector<Message> box;
};

C_LobbyJoin LobbyJoin(const char *nick, const char *password = "",
                      uint16_t version = PROTOCOL_VERSION) {
	C_LobbyJoin join{};
	InitHeader(join, 0);
	join.protocolVersion = version;
	std::strncpy(join.nick, nick, NICK_LEN - 1);
	std::strncpy(join.password, password, PASSWORD_LEN - 1);
	return join;
}

void TestTheLobby(Rig &rig) {
	std::printf("\nthe lobby, before anybody's game is running\n");
	using launcher::LobbyClient;
	using Phase = LobbyClient::Phase;
	for (NetClient *client : rig.clients)
		if (client)
			client->Disconnect();
	rig.Settle(300);
	rig.ClearInboxes();

	std::vector<LobbyClient *> lobbies;
	std::vector<Raw *>         raws;
	NetServer                 *older = nullptr;
	std::vector<ServerEvent>   olderEvents;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (LobbyClient *l : lobbies)
				l->Service(Ms());
			for (Raw *r : raws)
				r->net.Service(r->box);
			if (older) {
				olderEvents.clear();
				older->Service(olderEvents, 0);
			}
			return done && done();
		});
	};
	const auto knock = [&](Raw &r) {
		raws.push_back(&r);
		r.net.Connect("127.0.0.1", rig.port);
		pump(3000, [&] { return r.net.IsConnected(); });
	};

	LobbyClient alice, bob;
	lobbies = {&alice, &bob};
	alice.Join("127.0.0.1", rig.port, "alice", "", Ms());
	pump(3000, [&] { return alice.GetPhase() == Phase::In && alice.People().size() == 1; });
	Check(alice.GetPhase() == Phase::In && alice.WeAreHost() && alice.People().size() == 1 &&
	          alice.People()[0].you && alice.People()[0].host,
	      "alice's launcher is in the lobby, alone, and its host");
	bob.Join("127.0.0.1", rig.port, "bob", "", Ms());
	pump(3000, [&] { return bob.People().size() == 2 && alice.People().size() == 2; });
	Check(bob.GetPhase() == Phase::In && !bob.WeAreHost() && bob.HostNick() == "alice" &&
	          bob.People().size() == 2 && bob.People()[0].nick == "alice" && bob.People()[1].you,
	      "bob's comes in after it, and sees alice as the host");
	Check(!bob.Start(LOBBY_START_NEW_GAME, Ms()), "and offers bob no start");

	Raw carol;
	knock(carol);
	carol.net.Send(LobbyJoin("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_LobbyAnswer>(carol.box) && alice.People().size() == 3; });
	const S_LobbyAnswer *answer = Last<S_LobbyAnswer>(carol.box);
	Check(answer && answer->reject == REJECT_NONE && answer->lobbyId < LOBBY_MAX &&
	          alice.People().size() == 3,
	      "carol's comes in too");
	C_LobbyStart start{};
	InitHeader(start, 0);
	start.mode = LOBBY_START_NEW_GAME;
	carol.net.Send(start, CH_EVENT);
	pump(400, nullptr);
	uint8_t     mode = 0;
	std::string by;
	Check(!alice.TakeStart(&mode, &by) && !bob.TakeStart(&mode, &by) &&
	          !Last<S_LobbyStart>(carol.box),
	      "a start from anybody but the host starts nothing");

	Raw dave;
	knock(dave);
	dave.net.Send(Hello("dave"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(dave.box) && alice.Playing() == 1; });
	Check(alice.Playing() == 1 && alice.Waiting() == 3 && alice.People().back().nick == "dave" &&
	          alice.People().back().playing && !alice.People().back().host,
	      "the lobby sees dave's game, which is playing");
	Check(Count<S_PlayerJoin>(carol.box) == 0 && Count<S_MissionState>(carol.box) == 0,
	      "and hears nothing of the session but that");

	pump(HELLO_WAIT_MS + 1500, nullptr);
	Check(alice.GetPhase() == Phase::In && bob.GetPhase() == Phase::In && carol.net.IsConnected(),
	      "nobody waiting is dropped for never saying hello");

	Check(alice.Start(LOBBY_START_NEW_GAME, Ms()), "alice starts everybody's game");
	pump(3000, [&] { return Last<S_LobbyStart>(carol.box) != nullptr; });
	pump(200, nullptr);
	Check(alice.TakeStart(&mode, &by) && mode == LOBBY_START_NEW_GAME && by == "alice",
	      "alice's launcher hears it: a new game");
	uint8_t     bobMode = 0;
	std::string byBob;
	Check(bob.TakeStart(&bobMode, &byBob) && bobMode == LOBBY_START_NEW_GAME && byBob == "alice" &&
	          !bob.TakeStart(&bobMode, &byBob),
	      "and so does bob's, once");
	const S_LobbyStart *heard = Last<S_LobbyStart>(carol.box);
	Check(heard && heard->mode == LOBBY_START_NEW_GAME && !Last<S_LobbyStart>(dave.box),
	      "and carol's, but not dave's game, which is running already");
	alice.Start(LOBBY_START_NEW_GAME, Ms());
	pump(400, nullptr);
	Check(Count<S_LobbyStart>(carol.box) == 1, "one click is one start, however often it arrives");

	alice.Leave();
	pump(3000, [&] { return bob.WeAreHost(); });
	Check(bob.WeAreHost() && bob.People().size() == 3 && bob.People()[0].you,
	      "alice's launcher gone, bob's is the host");

	Raw stranger;
	knock(stranger);
	stranger.net.Send(LobbyJoin("erin", "", PROTOCOL_VERSION + 1), CH_EVENT);
	pump(3000, [&] { return Last<S_LobbyAnswer>(stranger.box) != nullptr; });
	answer = Last<S_LobbyAnswer>(stranger.box);
	Check(answer && answer->reject == REJECT_BAD_VERSION && bob.People().size() == 3,
	      "a launcher of another release is turned away, and told why");

	rig.server.SetPassword("rosebud");
	LobbyClient frank, gina;
	lobbies = {&bob, &frank, &gina};
	frank.Join("127.0.0.1", rig.port, "frank", "tulip", Ms());
	gina.Join("127.0.0.1", rig.port, "gina", "rosebud", Ms());
	pump(3000, [&] {
		return frank.GetPhase() == Phase::Refused && gina.GetPhase() == Phase::In;
	});
	Check(frank.GetPhase() == Phase::Refused &&
	          frank.Problem().find("password") != std::string::npos &&
	          gina.GetPhase() == Phase::In,
	      "behind a password, the wrong one is refused and the right one let in");
	rig.server.SetPassword("");

	NetServer old;
	uint16_t  oldPort = 0;
	for (uint16_t port = static_cast<uint16_t>(rig.port + 40); port < rig.port + 60 && !oldPort; ++port)
		if (old.Listen(port, 4))
			oldPort = port;
	if (oldPort != 0) {
		older = &old;
		LobbyClient hana;
		lobbies = {&hana};
		hana.Join("127.0.0.1", oldPort, "hana", "", Ms());
		pump(launcher::LOBBY_CONNECT_WAIT_MS + launcher::LOBBY_ANSWER_WAIT_MS,
		     [&] { return hana.GetPhase() == Phase::Lost; });
		Check(hana.GetPhase() == Phase::Lost &&
		          hana.Problem().find("no lobby") != std::string::npos,
		      "a server with no lobby never answers, and the launcher says what that means");
		lobbies.clear();
		older = nullptr;
		old.Shutdown();
	}

	lobbies.clear();
	for (Raw *r : raws)
		r->net.Disconnect();
	raws.clear();
	bob.Leave();
	frank.Leave();
	gina.Leave();
	rig.Settle(300);
}

void TestACarsBombTravels(Rig &rig) {
	std::printf("\na car's bomb, from the machine that simulates the car\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob;
	raws = {&alice, &bob};
	alice.net.Connect("127.0.0.1", rig.port);
	bob.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const S_Welcome *welcome = Last<S_Welcome>(alice.box);
	const uint8_t    aliceId = welcome ? welcome->playerId : INVALID_PLAYER;

	C_EnterVehicle claim{};
	InitHeader(claim, 100);
	claim.body.netId   = INVALID_NETID;
	claim.body.seat    = 0;
	claim.body.modelId = 90;
	alice.net.Send(claim, CH_EVENT);
	pump(3000, [&] { return Last<S_EnterVehicle>(alice.box) && Last<S_EnterVehicle>(bob.box); });
	const S_EnterVehicle *seat  = Last<S_EnterVehicle>(alice.box);
	const uint16_t        netId = seat ? seat->body.netId : INVALID_NETID;
	Check(netId != INVALID_NETID && aliceId != INVALID_PLAYER, "alice drives a car of the session's");

	alice.box.clear();
	bob.box.clear();
	C_VehicleBomb bomb{};
	InitHeader(bomb, 200);
	bomb.netId    = netId;
	bomb.bombType = 2;   // CARBOMB_ONIGNITION, 8-Ball's
	bomb.blame    = aliceId;
	alice.net.Send(bomb, CH_EVENT);
	pump(1500, [&] { return Last<S_VehicleBomb>(bob.box) != nullptr; });
	const S_VehicleBomb *heard = Last<S_VehicleBomb>(bob.box);
	Check(heard && heard->netId == netId && heard->bombType == 2 && heard->playerId == aliceId &&
	          heard->blame == aliceId && heard->fuseMs == 0 && !Last<S_VehicleBomb>(alice.box),
	      "the bomb a shop fits on alice's machine reaches bob's copy as hers, and not back "
	      "to alice");
	bob.box.clear();
	bomb.bombType = 5;
	bob.net.Send(bomb, CH_EVENT);
	bomb.bombType = CARBOMB_MAX + 1;
	alice.net.Send(bomb, CH_EVENT);
	bomb.bombType = 4;
	bomb.fuseMs   = CARBOMB_FUSE_MAX_MS + 1;
	alice.net.Send(bomb, CH_EVENT);
	bomb.fuseMs = 0;
	bomb.blame  = 6;   // nobody
	alice.net.Send(bomb, CH_EVENT);
	pump(400, nullptr);
	Check(!Last<S_VehicleBomb>(alice.box) && !Last<S_VehicleBomb>(bob.box),
	      "bob, who does not drive it, says nothing of it, and nor does a bomb there is none "
	      "of, a fuse no bomb burns that long or a bomb blamed on nobody here");

	bomb.bombType = 4;   // CARBOMB_TIMEDACTIVE, set going at the wheel
	bomb.blame    = aliceId;
	bomb.fuseMs   = 7000;
	alice.net.Send(bomb, CH_EVENT);
	pump(1500, [&] { return Last<S_VehicleBomb>(bob.box) != nullptr; });
	heard = Last<S_VehicleBomb>(bob.box);
	Check(heard && heard->bombType == 4 && heard->fuseMs == 7000,
	      "the timer alice set going reaches bob with its seven seconds");

	Raw carol;
	raws.push_back(&carol);
	carol.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return carol.net.IsConnected(); });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_VehicleBomb>(carol.box) != nullptr; });
	const S_VehicleBomb *joined = Last<S_VehicleBomb>(carol.box);
	Check(joined && joined->netId == netId && joined->bombType == 4 && joined->blame == aliceId &&
	          joined->playerId == INVALID_PLAYER && joined->fuseMs > 0 &&
	          joined->fuseMs < 7000 &&
	          IndexOf(carol.box, OP_S_VEHICLE_SPAWN) < IndexOf(carol.box, OP_S_VEHICLE_BOMB),
	      "carol, joining, is told the bomb after the car, alice's, with what is left of "
	      "the fuse");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// A traffic car its host's engine drops beside another player (C_CarLetGo):
// handed to him with its driver, everybody else told, the sender sent what it
// now watches; and dropped by him too, gone, rather than sent back.
void TestATrafficCarLetGoIsHandedOn(Rig &rig) {
	std::printf("\na traffic car its host's engine drops beside another player\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob, carol;
	raws = {&alice, &bob, &carol};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] {
		return alice.net.IsConnected() && bob.net.IsConnected() && carol.net.IsConnected();
	});
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(carol.box) != nullptr; });
	const uint8_t aliceId = Last<S_Welcome>(alice.box) ? Last<S_Welcome>(alice.box)->playerId
	                                                   : INVALID_PLAYER;
	const uint8_t bobId   = Last<S_Welcome>(bob.box) ? Last<S_Welcome>(bob.box)->playerId
	                                                 : INVALID_PLAYER;

	alice.net.Send(StateAt(0.0f, 0.0f, 0.0f), CH_SNAPSHOT);
	bob.net.Send(StateAt(100.0f, 0.0f, 0.0f), CH_SNAPSHOT);
	carol.net.Send(StateAt(1000.0f, 0.0f, 0.0f), CH_SNAPSHOT);
	pump(300, nullptr);

	C_CarSpawn car{};
	InitHeader(car, 100);
	car.tempId       = 7;
	car.body.modelId = 91;
	car.body.extra1 = car.body.extra2 = -1;
	car.body.pos     = {60.0f, 0.0f, 0.0f};
	car.body.rot     = {0.0f, 0.0f, 0.0f, 1.0f};
	alice.net.Send(car, CH_EVENT);
	C_PedSpawn driver{};
	InitHeader(driver, 100);
	driver.tempId       = 8;
	driver.body.modelId = 30;
	driver.body.pedType = AMBIENT_PEDTYPE_CIVMALE;
	driver.body.pos     = {60.0f, 0.0f, 0.0f};
	alice.net.Send(driver, CH_EVENT);
	pump(1500, [&] { return Last<S_CarSpawn>(bob.box) && Last<S_PedSpawn>(bob.box); });
	const uint16_t carId  = Last<S_CarSpawn>(bob.box) ? Last<S_CarSpawn>(bob.box)->netId
	                                                  : INVALID_NETID;
	const uint16_t pedId  = Last<S_PedSpawn>(bob.box) ? Last<S_PedSpawn>(bob.box)->netId
	                                                  : INVALID_NETID;
	Check(carId != INVALID_NETID && pedId != INVALID_NETID && aliceId != INVALID_PLAYER &&
	          bobId != INVALID_PLAYER,
	      "alice's traffic car and its driver are the session's");

	for (Raw *r : raws)
		r->box.clear();
	C_CarLetGo letGo{};
	InitHeader(letGo, 200);
	letGo.netId    = carId;
	letGo.pedCount = 1;
	letGo.peds[0]  = pedId;
	alice.net.Send(letGo, CH_EVENT);
	pump(1500, [&] {
		return Last<S_AmbientAdopt>(bob.box) && Last<S_AmbientAdopt>(carol.box) &&
		       Last<S_PedSpawn>(alice.box);
	});
	const S_AmbientAdopt *toBob = Last<S_AmbientAdopt>(bob.box);
	bool rowsForBob = toBob && toBob->count == 2;
	for (uint8_t i = 0; rowsForBob && i < toBob->count; ++i)
		rowsForBob = toBob->rows[i].newOwnerPlayerId == bobId &&
		             (toBob->rows[i].netId == carId || toBob->rows[i].netId == pedId);
	Check(toBob && toBob->why == AMBIENT_ADOPT_LET_GO && toBob->wasOwnerPlayerId == aliceId &&
	          rowsForBob,
	      "bob, 40 m from it, is handed the car and its driver, as a let-go of alice's");
	Check(Last<S_AmbientAdopt>(carol.box) != nullptr && !Last<S_AmbientAdopt>(alice.box),
	      "carol is told too, and alice is not");
	const S_CarSpawn *backCar = Last<S_CarSpawn>(alice.box);
	const S_PedSpawn *backPed = Last<S_PedSpawn>(alice.box);
	Check(backCar && backCar->netId == carId && backCar->ownerPlayerId == bobId &&
	          backCar->tempId == 0 && backPed && backPed->netId == pedId &&
	          backPed->ownerPlayerId == bobId && backPed->tempId == 0,
	      "alice, whose engine has deleted hers, is sent both as bob's to watch");
	Check(!Last<S_CarDespawn>(alice.box) && !Last<S_CarDespawn>(bob.box) &&
	          !Last<S_CarDespawn>(carol.box),
	      "and nobody is told it went");

	for (Raw *r : raws)
		r->box.clear();
	letGo.hdr.sendTimeMs = 300;
	bob.net.Send(letGo, CH_EVENT);
	pump(1500, [&] { return Last<S_CarDespawn>(alice.box) && Last<S_CarDespawn>(carol.box); });
	Check(Last<S_CarDespawn>(alice.box) && Last<S_CarDespawn>(alice.box)->netId == carId &&
	          Last<S_PedDespawn>(alice.box) && Last<S_PedDespawn>(alice.box)->netId == pedId &&
	          Last<S_CarDespawn>(carol.box) && !Last<S_CarDespawn>(bob.box),
	      "bob's engine drops it too: alice let go of it a moment ago and carol is a "
	      "kilometre off, so it goes, with its driver, and not back to alice");
	Check(!Last<S_AmbientAdopt>(alice.box) && !Last<S_AmbientAdopt>(carol.box),
	      "and nobody is handed it");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// Pedestrians their host's engine drops beside another player (C_PedLetGo):
// the one near bob is his, bob told and alice sent him back to watch; the one
// nobody is near goes.
void TestPedestriansLetGoAreHandedOn(Rig &rig) {
	std::printf("\npedestrians their host's engine drops beside another player\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob;
	raws = {&alice, &bob};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const uint8_t bobId = Last<S_Welcome>(bob.box) ? Last<S_Welcome>(bob.box)->playerId
	                                               : INVALID_PLAYER;

	alice.net.Send(StateAt(0.0f, 0.0f, 0.0f), CH_SNAPSHOT);
	bob.net.Send(StateAt(100.0f, 0.0f, 0.0f), CH_SNAPSHOT);
	pump(300, nullptr);

	uint16_t ids[2] = {INVALID_NETID, INVALID_NETID};
	const float at[2] = {70.0f, -150.0f};
	for (int i = 0; i < 2; ++i) {
		for (Raw *r : raws)
			r->box.clear();
		C_PedSpawn ped{};
		InitHeader(ped, 100);
		ped.tempId       = static_cast<uint32_t>(20 + i);
		ped.body.modelId = 30;
		ped.body.pedType = AMBIENT_PEDTYPE_CIVMALE;
		ped.body.pos     = {at[i], 0.0f, 0.0f};
		alice.net.Send(ped, CH_EVENT);
		pump(1500, [&] { return Last<S_PedSpawn>(bob.box) != nullptr; });
		ids[i] = Last<S_PedSpawn>(bob.box) ? Last<S_PedSpawn>(bob.box)->netId : INVALID_NETID;
	}
	Check(ids[0] != INVALID_NETID && ids[1] != INVALID_NETID && bobId != INVALID_PLAYER,
	      "alice's two pedestrians are the session's");

	for (Raw *r : raws)
		r->box.clear();
	C_PedLetGo letGo{};
	InitHeader(letGo, 200);
	letGo.count   = 2;
	letGo.peds[0] = ids[0];
	letGo.peds[1] = ids[1];
	alice.net.Send(letGo, CH_EVENT);
	pump(1500, [&] {
		return Last<S_AmbientAdopt>(bob.box) && Last<S_PedDespawn>(bob.box) &&
		       Last<S_PedSpawn>(alice.box);
	});
	const S_AmbientAdopt *toBob = Last<S_AmbientAdopt>(bob.box);
	Check(toBob && toBob->why == AMBIENT_ADOPT_LET_GO && toBob->count == 1 &&
	          toBob->rows[0].netId == ids[0] && toBob->rows[0].newOwnerPlayerId == bobId &&
	          toBob->rows[0].kind == AMBIENT_ADOPT_PED,
	      "bob, 30 m from the first, is handed him as a let-go of alice's");
	Check(Last<S_PedDespawn>(bob.box) && Last<S_PedDespawn>(bob.box)->netId == ids[1],
	      "the one 250 m from bob goes");
	const S_PedSpawn *back = Last<S_PedSpawn>(alice.box);
	Check(back && back->netId == ids[0] && back->ownerPlayerId == bobId && back->tempId == 0 &&
	          !Last<S_AmbientAdopt>(alice.box) && !Last<S_PedDespawn>(alice.box),
	      "alice is sent him as bob's to watch, and told nothing else");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// A cop its host gives to a wanted player (C_CopHandover), and a line one of
// the crowd says (C_PedSpeech).
void TestPoliceAndSpeechTravel(Rig &rig) {
	std::printf("\na cop handed to a wanted player, and a line the crowd says\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob, carol;
	raws = {&alice, &bob, &carol};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] {
		return alice.net.IsConnected() && bob.net.IsConnected() && carol.net.IsConnected();
	});
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(carol.box) != nullptr; });
	const uint8_t aliceId = Last<S_Welcome>(alice.box) ? Last<S_Welcome>(alice.box)->playerId
	                                                   : INVALID_PLAYER;
	const uint8_t bobId   = Last<S_Welcome>(bob.box) ? Last<S_Welcome>(bob.box)->playerId
	                                                 : INVALID_PLAYER;

	alice.net.Send(StateAt(0.0f, 0.0f, 0.0f), CH_SNAPSHOT);
	bob.net.Send(StateAt(30.0f, 0.0f, 0.0f), CH_SNAPSHOT);
	carol.net.Send(StateAt(1000.0f, 0.0f, 0.0f), CH_SNAPSHOT);
	pump(300, nullptr);

	C_PedSpawn cop{};
	InitHeader(cop, 100);
	cop.tempId       = 9;
	cop.body.modelId = 1;
	cop.body.pedType = AMBIENT_PEDTYPE_COP;
	cop.body.pos     = {20.0f, 0.0f, 0.0f};
	alice.net.Send(cop, CH_EVENT);
	pump(1500, [&] { return Last<S_PedSpawn>(bob.box) != nullptr; });
	const uint16_t copId = Last<S_PedSpawn>(bob.box) ? Last<S_PedSpawn>(bob.box)->netId
	                                                 : INVALID_NETID;
	Check(copId != INVALID_NETID, "alice's cop is the session's");

	for (Raw *r : raws)
		r->box.clear();
	C_PedSpeech line{};
	InitHeader(line, 150);
	line.who   = SPEECH_AMBIENT;
	line.netId = copId;
	line.sound = 0x7F;
	alice.net.Send(line, CH_SNAPSHOT);
	pump(1000, [&] { return Last<S_PedSpeech>(bob.box) && Last<S_PedSpeech>(carol.box); });
	const S_PedSpeech *heard = Last<S_PedSpeech>(bob.box);
	Check(heard && heard->playerId == aliceId && heard->netId == copId && heard->sound == 0x7F &&
	          Last<S_PedSpeech>(carol.box) && !Last<S_PedSpeech>(alice.box),
	      "what alice's cop says reaches the others as hers, and not her");
	for (Raw *r : raws)
		r->box.clear();
	line.hdr.sendTimeMs = 160;
	bob.net.Send(line, CH_SNAPSHOT);
	line.netId = copId;
	line.sound = 0x20;
	alice.net.Send(line, CH_SNAPSHOT);
	pump(400, nullptr);
	Check(!Last<S_PedSpeech>(alice.box) && !Last<S_PedSpeech>(carol.box) &&
	          !Last<S_PedSpeech>(bob.box),
	      "bob cannot speak for alice's cop, and a sound outside the ped block goes nowhere");

	for (Raw *r : raws)
		r->box.clear();
	C_CopHandover give{};
	InitHeader(give, 200);
	give.toPlayerId = bobId;
	give.carNetId   = INVALID_NETID;
	give.pedCount   = 1;
	give.peds[0]    = copId;
	alice.net.Send(give, CH_EVENT);
	pump(1500, [&] {
		return Last<S_AmbientAdopt>(bob.box) && Last<S_AmbientAdopt>(carol.box) &&
		       Last<S_PedSpawn>(alice.box);
	});
	const S_AmbientAdopt *toBob = Last<S_AmbientAdopt>(bob.box);
	Check(toBob && toBob->why == AMBIENT_ADOPT_PURSUIT && toBob->wasOwnerPlayerId == aliceId &&
	          toBob->count == 1 && toBob->rows[0].netId == copId &&
	          toBob->rows[0].newOwnerPlayerId == bobId,
	      "bob is handed the cop, for his stars");
	Check(Last<S_AmbientAdopt>(carol.box) && !Last<S_AmbientAdopt>(alice.box),
	      "carol is told too, and alice is not");
	const S_PedSpawn *back = Last<S_PedSpawn>(alice.box);
	Check(back && back->netId == copId && back->ownerPlayerId == bobId && back->tempId == 0,
	      "alice, whose engine has let go of him, is sent him as bob's to watch");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// The host's places (C_PlaceBlips): the same way as the contacts.
void TestTheHostsPlacesTravel(Rig &rig) {
	std::printf("\nthe host's places reach the guests, and nobody else's do\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob, carol;
	raws = {&alice, &bob};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const S_Welcome *aw     = Last<S_Welcome>(alice.box);
	const uint8_t    aliceId = aw ? aw->playerId : INVALID_PLAYER;

	C_PlaceBlips mine;
	InitHeader(mine, 100);
	mine.count             = 3;
	mine.places[0].x       = 345.5f;   // Staunton's Ammu-Nation
	mine.places[0].y       = -713.5f;
	mine.places[0].z       = 26.0625f;
	mine.places[0].sprite  = 20;
	mine.places[0].display = 2;
	mine.places[1]         = mine.places[0];
	mine.places[1].sprite  = 13;   // a contact: not a place
	mine.places[2]         = mine.places[0];
	mine.places[2].x       = 379.0f;
	mine.places[2].sprite  = 18;
	mine.places[2].display = 3;
	for (Raw *r : raws)
		r->box.clear();
	alice.net.Send(mine, CH_EVENT);
	pump(1000, [&] { return Last<S_PlaceBlips>(bob.box) != nullptr; });
	const S_PlaceBlips *heard = Last<S_PlaceBlips>(bob.box);
	Check(heard && heard->hostId == aliceId && heard->count == 2 && heard->places[0].sprite == 20 &&
	          heard->places[1].sprite == 18 && !Last<S_PlaceBlips>(alice.box),
	      "alice's reach bob as hers, less the contact, and not alice");

	for (Raw *r : raws)
		r->box.clear();
	C_PlaceBlips his = mine;
	his.hdr.sendTimeMs = 200;
	bob.net.Send(his, CH_EVENT);
	C_PlaceBlips wild = mine;
	wild.hdr.sendTimeMs = 210;
	wild.count          = PLACE_BLIPS_MAX + 1;
	alice.net.Send(wild, CH_EVENT);
	pump(400, nullptr);
	Check(!Last<S_PlaceBlips>(alice.box) && !Last<S_PlaceBlips>(bob.box),
	      "bob is not the host, and a count past the list goes nowhere");

	raws = {&alice, &bob, &carol};
	carol.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return carol.net.IsConnected(); });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_PlaceBlips>(carol.box) != nullptr; });
	const S_PlaceBlips *late = Last<S_PlaceBlips>(carol.box);
	Check(late && late->hostId == aliceId && late->count == 2,
	      "carol, joining later, is handed alice's as they stand");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// The host's contacts (C_ContactMarkers): from the host alone, to everybody
// else with whose they are, and to a joiner.
void TestTheHostsContactsTravel(Rig &rig) {
	std::printf("\nthe host's contacts reach the guests, and nobody else's do\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob, carol;
	raws = {&alice, &bob};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const S_Welcome *aw    = Last<S_Welcome>(alice.box);
	const S_Welcome *bw    = Last<S_Welcome>(bob.box);
	const uint8_t   aliceId = aw ? aw->playerId : INVALID_PLAYER;
	Check(aw && bw && aw->hostPlayerId == aliceId && bw->hostPlayerId == aliceId,
	      "alice came first, and is the host");

	C_ContactMarkers mine;
	InitHeader(mine, 100);
	mine.count             = 3;
	mine.markers[0].x      = 892.75f;
	mine.markers[0].y      = -425.75f;
	mine.markers[0].z      = 13.875f;
	mine.markers[0].sprite = 13;
	mine.markers[0].display = 3;
	mine.markers[1]         = mine.markers[0];
	mine.markers[1].sprite  = 17;   // a safehouse: not a contact
	mine.markers[2]         = mine.markers[0];
	mine.markers[2].x       = 1191.688f;
	mine.markers[2].sprite  = 10;
	mine.markers[2].display = 2;
	for (Raw *r : raws)
		r->box.clear();
	alice.net.Send(mine, CH_EVENT);
	pump(1000, [&] { return Last<S_ContactMarkers>(bob.box) != nullptr; });
	const S_ContactMarkers *heard = Last<S_ContactMarkers>(bob.box);
	Check(heard && heard->hostId == aliceId && heard->count == 2 && heard->markers[0].sprite == 13 &&
	          heard->markers[1].sprite == 10 && heard->markers[1].x == 1191.688f &&
	          !Last<S_ContactMarkers>(alice.box),
	      "alice's reach bob as hers, less the safehouse, and not alice");

	for (Raw *r : raws)
		r->box.clear();
	C_ContactMarkers his = mine;
	his.hdr.sendTimeMs   = 200;
	his.count            = 1;
	his.markers[0].sprite = 15;
	bob.net.Send(his, CH_EVENT);
	C_ContactMarkers wild = mine;
	wild.hdr.sendTimeMs   = 210;
	wild.count            = CONTACT_MARKERS_MAX + 1;
	alice.net.Send(wild, CH_EVENT);
	pump(400, nullptr);
	Check(!Last<S_ContactMarkers>(alice.box) && !Last<S_ContactMarkers>(bob.box),
	      "bob is not the host, and a count past the list goes nowhere");

	raws = {&alice, &bob, &carol};
	carol.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return carol.net.IsConnected(); });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_ContactMarkers>(carol.box) != nullptr; });
	const S_ContactMarkers *late = Last<S_ContactMarkers>(carol.box);
	Check(late && late->hostId == aliceId && late->count == 2 && late->markers[0].sprite == 13,
	      "carol, joining later, is handed alice's as they stand");

	alice.net.Disconnect();
	raws = {&bob, &carol};
	pump(1500, [&] { return Last<S_PlayerLeave>(bob.box) != nullptr; });
	Raw dave;
	raws = {&bob, &carol, &dave};
	dave.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return dave.net.IsConnected(); });
	dave.net.Send(Hello("dave"), CH_EVENT);
	pump(1500, [&] { return Last<S_Welcome>(dave.box) != nullptr; });
	pump(300, nullptr);
	Check(Last<S_Welcome>(dave.box) && !Last<S_ContactMarkers>(dave.box),
	      "once alice has gone, nobody is handed hers");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// The hidden packages two saves found before the session: each one's goes to
// the other's world as an S_PickupTaken, and one lying in a world the session
// already took is told to it again.
void TestSavedPackagesReachEverybody(Rig &rig) {
	std::printf("\nthe hidden packages each save already had\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob;
	raws = {&alice, &bob};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const uint8_t aliceId = Last<S_Welcome>(alice.box) ? Last<S_Welcome>(alice.box)->playerId
	                                                   : INVALID_PLAYER;
	for (Raw *r : raws)
		r->box.clear();

	const auto report = [](std::initializer_list<float> xs) {
		C_PackagesLive out{};
		InitHeader(out, 100);
		out.scriptHash = 0x5EED5EEDu;
		out.modelIndex = 1321;
		for (float x : xs)
			out.pos[out.count++] = Vec3{x, -300.0f, 10.0f};
		return out;
	};
	// Alice's save has found 5003; bob's has found 5001.
	alice.net.Send(report({5001.0f, 5002.0f}), CH_EVENT);
	pump(300, nullptr);
	Check(!Last<S_PickupTaken>(alice.box) && !Last<S_PickupTaken>(bob.box),
	      "one report alone takes nothing from anybody");
	bob.net.Send(report({5002.0f, 5003.0f}), CH_EVENT);
	pump(1500, [&] { return Last<S_PickupTaken>(alice.box) && Last<S_PickupTaken>(bob.box); });
	const S_PickupTaken *toAlice = Last<S_PickupTaken>(alice.box);
	const S_PickupTaken *toBob   = Last<S_PickupTaken>(bob.box);
	Check(toAlice && toAlice->ident.pos.x == 5001.0f && toAlice->ident.type == PICKUP_TYPE_PACKAGE &&
	          toAlice->ident.modelIndex == 1321 && Count<S_PickupTaken>(alice.box) == 1,
	      "alice's world loses the one bob's save found");
	Check(toBob && toBob->ident.pos.x == 5003.0f && toBob->playerId == aliceId &&
	          Count<S_PickupTaken>(bob.box) == 1,
	      "and bob's the one alice's did, as hers");
	const PickupIdent taken = toAlice ? toAlice->ident : PickupIdent{};

	for (Raw *r : raws)
		r->box.clear();
	bob.net.Send(report({5001.0f, 5002.0f, 5003.0f}), CH_EVENT);
	pump(1500, [&] { return Count<S_PickupTaken>(bob.box) == 2; });
	Check(Count<S_PickupTaken>(bob.box) == 2 && !Last<S_PickupTaken>(alice.box),
	      "an older save loaded over it is told both again, and nobody else hears");

	for (Raw *r : raws)
		r->box.clear();
	C_PickupClaim claim{};
	InitHeader(claim, 100);
	claim.ident = taken;
	bob.net.Send(claim, CH_EVENT);
	pump(1500, [&] { return Last<S_PickupDenied>(bob.box) != nullptr; });
	Check(Last<S_PickupDenied>(bob.box) != nullptr, "and nobody can collect one of them again");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// A bomb the mission's script fitted: from the owner, to everybody else as
// the owner's, and to a joiner after the bomb itself.
void TestAMissionsBombTravels(Rig &rig) {
	std::printf("\na bomb the mission fitted, from the owner's machine\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob;
	raws = {&alice, &bob};
	alice.net.Connect("127.0.0.1", rig.port);
	bob.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const S_Welcome *welcome = Last<S_Welcome>(alice.box);
	const uint8_t    aliceId = welcome ? welcome->playerId : INVALID_PLAYER;

	C_EnterVehicle claim{};
	InitHeader(claim, 100);
	claim.body.netId   = INVALID_NETID;
	claim.body.seat    = 0;
	claim.body.modelId = 90;
	bob.net.Send(claim, CH_EVENT);
	pump(3000, [&] { return Last<S_EnterVehicle>(alice.box) && Last<S_EnterVehicle>(bob.box); });
	const S_EnterVehicle *seat  = Last<S_EnterVehicle>(bob.box);
	const uint16_t        netId = seat ? seat->body.netId : INVALID_NETID;
	Check(netId != INVALID_NETID && aliceId != INVALID_PLAYER, "bob drives a car of the session's");

	C_MissionBomb arm{};
	InitHeader(arm, 200);
	arm.netId    = netId;
	arm.bombType = CARBOMB_REMOTE;
	alice.box.clear();
	bob.box.clear();
	alice.net.Send(arm, CH_EVENT);
	pump(400, nullptr);
	Check(!Last<S_MissionBomb>(bob.box), "with no mission of hers running, alice's word goes nowhere");

	C_MissionStarted started;
	InitHeader(started, 300);
	started.launchKey     = 0x4141;
	started.missionNumber = 41;
	alice.net.Send(started, CH_EVENT);
	pump(1500, [&] { return Last<S_MissionState>(bob.box) != nullptr; });
	bob.box.clear();
	alice.net.Send(arm, CH_EVENT);
	pump(1500, [&] { return Last<S_MissionBomb>(bob.box) != nullptr; });
	const S_MissionBomb *heard = Last<S_MissionBomb>(bob.box);
	Check(heard && heard->netId == netId && heard->playerId == aliceId &&
	          heard->bombType == CARBOMB_REMOTE && !Last<S_MissionBomb>(alice.box),
	      "her mission's bomb reaches bob, at the wheel, as hers, and not back to her");
	alice.box.clear();
	bob.net.Send(arm, CH_EVENT);
	pump(400, nullptr);
	Check(!Last<S_MissionBomb>(alice.box), "bob's script is not the mission's, whatever he drives");

	Raw carol;
	raws.push_back(&carol);
	carol.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return carol.net.IsConnected(); });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_MissionBomb>(carol.box) != nullptr; });
	const S_MissionBomb *joined = Last<S_MissionBomb>(carol.box);
	Check(joined && joined->netId == netId && joined->playerId == aliceId &&
	          IndexOf(carol.box, OP_S_VEHICLE_BOMB) < IndexOf(carol.box, OP_S_MISSION_BOMB),
	      "carol, joining, is told the bomb and then that it is alice's mission's");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// A mine that went off on one machine: said by it, heard by everybody else.
void TestAMineBlastTravels(Rig &rig) {
	std::printf("\na mine that went off on one machine\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob;
	raws = {&alice, &bob};
	alice.net.Connect("127.0.0.1", rig.port);
	bob.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const S_Welcome *welcome = Last<S_Welcome>(alice.box);
	const uint8_t    aliceId = welcome ? welcome->playerId : INVALID_PLAYER;

	alice.box.clear();
	bob.box.clear();
	C_MineBlast blast{};
	InitHeader(blast, 300);
	blast.pos = Vec3{1250.0f, -620.0f, 0.6f};
	alice.net.Send(blast, CH_EVENT);
	pump(1500, [&] { return Last<S_MineBlast>(bob.box) != nullptr; });
	const S_MineBlast *heard = Last<S_MineBlast>(bob.box);
	Check(heard && heard->playerId == aliceId && heard->pos.x == 1250.0f &&
	          heard->pos.z == 0.6f && !Last<S_MineBlast>(alice.box),
	      "alice's mine went off: bob hears where, and alice is not told her own");
	bob.box.clear();
	blast.pos = Vec3{1250.0f, 9000.0f, 0.6f};
	alice.net.Send(blast, CH_EVENT);
	pump(400, nullptr);
	Check(!Last<S_MineBlast>(bob.box), "and a place off the map goes nowhere");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// The car radio: turned from inside the car, heard by everybody - the one who
// turned it too - and put back on the one who turned it from outside.
void TestACarsRadioTravels(Rig &rig) {
	std::printf("\na car's radio, from anybody in it\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob, carol;
	raws = {&alice, &bob, &carol};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] {
		return alice.net.IsConnected() && bob.net.IsConnected() && carol.net.IsConnected();
	});
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(carol.box) != nullptr; });
	const S_Welcome *welcome = Last<S_Welcome>(bob.box);
	const uint8_t    bobId   = welcome ? welcome->playerId : INVALID_PLAYER;

	C_EnterVehicle claim{};
	InitHeader(claim, 100);
	claim.body.netId   = INVALID_NETID;
	claim.body.seat    = 0;
	claim.body.modelId = 90;
	alice.net.Send(claim, CH_EVENT);
	pump(3000, [&] { return Last<S_EnterVehicle>(alice.box) && Last<S_EnterVehicle>(bob.box); });
	const S_EnterVehicle *seat  = Last<S_EnterVehicle>(alice.box);
	const uint16_t        netId = seat ? seat->body.netId : INVALID_NETID;
	C_EnterVehicle ride{};
	InitHeader(ride, 100);
	ride.body.netId   = netId;
	ride.body.seat    = 1;
	ride.body.modelId = 90;
	bob.net.Send(ride, CH_EVENT);
	pump(1500, [&] { return Count<S_EnterVehicle>(carol.box) >= 2; });
	Check(netId != INVALID_NETID && Count<S_EnterVehicle>(carol.box) >= 2,
	      "alice drives a session car and bob rides with her");

	for (Raw *r : raws)
		r->box.clear();
	C_VehicleRadio turn{};
	InitHeader(turn, 200);
	turn.netId   = netId;
	turn.station = 6;
	bob.net.Send(turn, CH_EVENT);
	pump(1500, [&] {
		return Last<S_VehicleRadio>(alice.box) && Last<S_VehicleRadio>(bob.box) &&
		       Last<S_VehicleRadio>(carol.box);
	});
	const S_VehicleRadio *heard = Last<S_VehicleRadio>(alice.box);
	Check(heard && heard->netId == netId && heard->station == 6 && heard->playerId == bobId,
	      "bob, the passenger, turns it and alice's copy hears it from him");
	Check(Last<S_VehicleRadio>(bob.box) && Last<S_VehicleRadio>(carol.box),
	      "and so do bob himself and carol, who is not in it");

	for (Raw *r : raws)
		r->box.clear();
	turn.station = 2;
	carol.net.Send(turn, CH_EVENT);
	pump(800, [&] { return Last<S_VehicleRadio>(carol.box) != nullptr; });
	const S_VehicleRadio *back = Last<S_VehicleRadio>(carol.box);
	Check(back && back->station == 6 && back->playerId == INVALID_PLAYER,
	      "carol, outside it, is refused and told it is still on 6");
	Check(!Last<S_VehicleRadio>(alice.box) && !Last<S_VehicleRadio>(bob.box),
	      "and nobody else hears a thing");

	Raw dave;
	raws.push_back(&dave);
	dave.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return dave.net.IsConnected(); });
	dave.net.Send(Hello("dave"), CH_EVENT);
	pump(3000, [&] { return Last<S_VehicleRadio>(dave.box) != nullptr; });
	const S_VehicleRadio *joined = Last<S_VehicleRadio>(dave.box);
	Check(joined && joined->netId == netId && joined->station == 6 &&
	          IndexOf(dave.box, OP_S_VEHICLE_SPAWN) < IndexOf(dave.box, OP_S_VEHICLE_RADIO),
	      "dave, joining, is told the station after the car it is for");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// A car's alarm and its gun (protocol.h, S_VehicleAlarm and S_VehicleAim):
// from its driver to everybody else, nothing from anybody else, and a joiner
// told what is left of the alarm and where the turret points.
void TestACarsAlarmAndGunTravel(Rig &rig) {
	std::printf("\na car's alarm and its gun, from its driver\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob;
	raws = {&alice, &bob};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const S_Welcome *welcome = Last<S_Welcome>(alice.box);
	const uint8_t    aliceId = welcome ? welcome->playerId : INVALID_PLAYER;

	C_EnterVehicle claim{};
	InitHeader(claim, 100);
	claim.body.netId   = INVALID_NETID;
	claim.body.seat    = 0;
	claim.body.modelId = 122;
	alice.net.Send(claim, CH_EVENT);
	pump(3000, [&] { return Last<S_EnterVehicle>(alice.box) && Last<S_EnterVehicle>(bob.box); });
	const S_EnterVehicle *seat  = Last<S_EnterVehicle>(alice.box);
	const uint16_t        netId = seat ? seat->body.netId : INVALID_NETID;
	Check(netId != INVALID_NETID, "alice drives a tank");

	for (Raw *r : raws)
		r->box.clear();
	C_VehicleAlarm alarm{};
	InitHeader(alarm, 200);
	alarm.netId       = netId;
	alarm.remainingMs = 12000;
	bob.net.Send(alarm, CH_EVENT);
	C_VehicleAim aim{};
	InitHeader(aim, 200);
	aim.netId = netId;
	aim.gunLR = 2.0f;
	aim.gunUD = 0.05f;
	bob.net.Send(aim, CH_SNAPSHOT);
	pump(600, [] { return false; });
	Check(!Last<S_VehicleAlarm>(alice.box) && !Last<S_VehicleAim>(alice.box),
	      "bob, who is not driving it, says nothing of either");

	alice.net.Send(alarm, CH_EVENT);
	alice.net.Send(aim, CH_SNAPSHOT);
	pump(1500, [&] { return Last<S_VehicleAlarm>(bob.box) && Last<S_VehicleAim>(bob.box); });
	const S_VehicleAlarm *heard = Last<S_VehicleAlarm>(bob.box);
	const S_VehicleAim   *aimed = Last<S_VehicleAim>(bob.box);
	Check(heard && heard->netId == netId && heard->remainingMs == 12000 &&
	          heard->playerId == aliceId,
	      "alice's alarm reaches bob, with how long it has");
	Check(aimed && aimed->netId == netId && aimed->gunLR == 2.0f && aimed->gunUD == 0.05f,
	      "and so does where her turret points");
	Check(!Last<S_VehicleAlarm>(alice.box) && !Last<S_VehicleAim>(alice.box),
	      "neither comes back to her");

	Raw carol;
	raws.push_back(&carol);
	carol.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return carol.net.IsConnected(); });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_VehicleAlarm>(carol.box) && Last<S_VehicleAim>(carol.box); });
	const S_VehicleAlarm *left  = Last<S_VehicleAlarm>(carol.box);
	const S_VehicleAim   *there = Last<S_VehicleAim>(carol.box);
	Check(left && left->netId == netId && left->remainingMs > 0 && left->remainingMs < 12000 &&
	          left->playerId == INVALID_PLAYER,
	      "carol, joining, hears what is left of the alarm");
	Check(there && there->gunLR == 2.0f &&
	          IndexOf(carol.box, OP_S_VEHICLE_SPAWN) < IndexOf(carol.box, OP_S_VEHICLE_AIM),
	      "and where the turret points, after the car it is for");

	for (Raw *r : raws)
		r->box.clear();
	alarm.remainingMs = 0;
	alice.net.Send(alarm, CH_EVENT);
	pump(1500, [&] { return Last<S_VehicleAlarm>(bob.box) && Last<S_VehicleAlarm>(carol.box); });
	const S_VehicleAlarm *stop = Last<S_VehicleAlarm>(bob.box);
	Check(stop && stop->remainingMs == 0 && Last<S_VehicleAlarm>(carol.box),
	      "and when she says it stopped, everybody hears that");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// A car's paint (protocol.h, S_VehicleColour): from its driver to everybody
// else, nothing from anybody else, and a joiner's spawn packet painted.
void TestACarsPaintTravels(Rig &rig) {
	std::printf("\na car's paint, from its driver\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob;
	raws = {&alice, &bob};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const S_Welcome *welcome = Last<S_Welcome>(alice.box);
	const uint8_t    aliceId = welcome ? welcome->playerId : INVALID_PLAYER;

	C_EnterVehicle claim{};
	InitHeader(claim, 100);
	claim.body.netId   = INVALID_NETID;
	claim.body.seat    = 0;
	claim.body.modelId = 91;
	claim.body.colour1 = 3;
	claim.body.colour2 = 4;
	alice.net.Send(claim, CH_EVENT);
	pump(3000, [&] { return Last<S_EnterVehicle>(alice.box) && Last<S_EnterVehicle>(bob.box); });
	const S_EnterVehicle *seat  = Last<S_EnterVehicle>(alice.box);
	const uint16_t        netId = seat ? seat->body.netId : INVALID_NETID;
	Check(netId != INVALID_NETID, "alice drives a car painted 3/4");

	for (Raw *r : raws)
		r->box.clear();
	C_VehicleColour paint{};
	InitHeader(paint, 200);
	paint.netId   = netId;
	paint.colour1 = 15;
	paint.colour2 = 79;
	bob.net.Send(paint, CH_EVENT);
	pump(600, [] { return false; });
	Check(!Last<S_VehicleColour>(alice.box), "bob, who is not driving it, cannot repaint it");

	alice.net.Send(paint, CH_EVENT);
	pump(1500, [&] { return Last<S_VehicleColour>(bob.box) != nullptr; });
	const S_VehicleColour *heard = Last<S_VehicleColour>(bob.box);
	Check(heard && heard->netId == netId && heard->colour1 == 15 && heard->colour2 == 79 &&
	          heard->playerId == aliceId,
	      "alice's respray reaches bob");
	Check(!Last<S_VehicleColour>(alice.box), "and does not come back to her");

	for (Raw *r : raws)
		r->box.clear();
	alice.net.Send(paint, CH_EVENT);
	pump(600, [] { return false; });
	Check(!Last<S_VehicleColour>(bob.box), "the same colours again are not news");

	Raw carol;
	raws.push_back(&carol);
	carol.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return carol.net.IsConnected(); });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_VehicleSpawn>(carol.box) != nullptr; });
	const S_VehicleSpawn *spawn = Last<S_VehicleSpawn>(carol.box);
	Check(spawn && spawn->netId == netId && spawn->colour1 == 15 && spawn->colour2 == 79,
	      "carol, joining, is handed the car in its new paint");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// A unique jump's shot, from the car's driver to whoever rides with him, and
// to nobody else (stuntcam.h).
// A driver whose snapshots have been on foot for SEAT_RELEASE_ON_FOOT_MS is
// taken out of the car the session still had him in, and everybody is told
// with the same S_ExitVehicle his own exit would have sent.
void TestASeatIsLetGoOfWhenTheSnapshotsSayOnFoot(Rig &rig) {
	std::printf("\na seat the snapshots have left\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob;
	raws = {&alice, &bob};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const S_Welcome *welcome = Last<S_Welcome>(alice.box);
	const uint8_t    aliceId = welcome ? welcome->playerId : INVALID_PLAYER;

	C_EnterVehicle claim{};
	InitHeader(claim, 100);
	claim.body.netId   = INVALID_NETID;
	claim.body.seat    = 0;
	claim.body.modelId = 90;
	alice.net.Send(claim, CH_EVENT);
	pump(3000, [&] { return Last<S_EnterVehicle>(alice.box) && Last<S_EnterVehicle>(bob.box); });
	const S_EnterVehicle *seat  = Last<S_EnterVehicle>(alice.box);
	const uint16_t        netId = seat ? seat->body.netId : INVALID_NETID;
	Check(netId != INVALID_NETID, "alice drives a session car");

	// Seated snapshots for a while: nothing.
	bob.box.clear();
	for (int i = 0; i < 40; ++i) {
		alice.net.Send(StateAt(1, 2, 3, PEDSTATE_ON_WIRE_DRIVING), CH_SNAPSHOT);
		pump(25, nullptr);
	}
	Check(Count<S_ExitVehicle>(bob.box) == 0, "a seated driver keeps his seat");

	// Then on foot, with no exit ever sent.
	const auto from = std::chrono::steady_clock::now();
	bool       told = false;
	while (!told && std::chrono::steady_clock::now() - from < std::chrono::milliseconds(8000)) {
		alice.net.Send(StateAt(1, 2, 3, 1), CH_SNAPSHOT);
		pump(25, nullptr);
		const S_ExitVehicle *out = Last<S_ExitVehicle>(bob.box);
		told = out && out->playerId == aliceId && out->netId == netId;
	}
	const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
	                      std::chrono::steady_clock::now() - from)
	                      .count();
	Check(told, "bob is told alice is out of the car");
	Check(took >= static_cast<long long>(SEAT_RELEASE_ON_FOOT_MS) - 100,
	      "not before SEAT_RELEASE_ON_FOOT_MS");
	pump(500, [&] { return Last<S_ExitVehicle>(alice.box) != nullptr; });
	Check(Last<S_ExitVehicle>(alice.box) != nullptr, "and so is alice");
	Check(Last<S_VehicleCustody>(bob.box) != nullptr &&
	          Last<S_VehicleCustody>(bob.box)->playerId == aliceId,
	      "with the car in her custody, as her own exit would have left it");

	alice.net.Disconnect();
	bob.net.Disconnect();
	pump(300, nullptr);
}

void TestAJumpShotReachesTheRiders(Rig &rig) {
	std::printf("\na unique jump's shot, from the driver to the car's riders\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob, carol;
	raws = {&alice, &bob, &carol};
	for (Raw *r : raws)
		r->net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] {
		return alice.net.IsConnected() && bob.net.IsConnected() && carol.net.IsConnected();
	});
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(carol.box) != nullptr; });
	const S_Welcome *welcome = Last<S_Welcome>(alice.box);
	const uint8_t    aliceId = welcome ? welcome->playerId : INVALID_PLAYER;

	C_EnterVehicle claim{};
	InitHeader(claim, 100);
	claim.body.netId   = INVALID_NETID;
	claim.body.seat    = 0;
	claim.body.modelId = 90;
	alice.net.Send(claim, CH_EVENT);
	pump(3000, [&] { return Last<S_EnterVehicle>(alice.box) && Last<S_EnterVehicle>(bob.box); });
	const S_EnterVehicle *seat  = Last<S_EnterVehicle>(alice.box);
	const uint16_t        netId = seat ? seat->body.netId : INVALID_NETID;
	C_EnterVehicle ride{};
	InitHeader(ride, 100);
	ride.body.netId   = netId;
	ride.body.seat    = 2;
	ride.body.modelId = 90;
	bob.net.Send(ride, CH_EVENT);
	pump(1500, [&] { return Count<S_EnterVehicle>(carol.box) >= 2; });
	Check(netId != INVALID_NETID && Count<S_EnterVehicle>(carol.box) >= 2,
	      "alice drives a session car and bob rides with her; carol is on foot");

	for (Raw *r : raws)
		r->box.clear();
	C_StuntCamera shot{};
	InitHeader(shot, 200);
	shot.body.netId = netId;
	shot.body.on    = 1;
	shot.body.mode  = 15;
	shot.body.swap  = 2;
	shot.body.from  = {998.0f, -938.5f, 19.25f};
	alice.net.Send(shot, CH_EVENT);
	pump(1500, [&] { return Last<S_StuntCamera>(bob.box) != nullptr; });
	pump(200, nullptr);
	const S_StuntCamera *heard = Last<S_StuntCamera>(bob.box);
	Check(heard && heard->playerId == aliceId && heard->body.netId == netId && heard->body.on == 1 &&
	          heard->body.from.x == 998.0f && heard->body.mode == 15,
	      "bob, riding with her, is sent her shot and where the camera stands");
	Check(!Last<S_StuntCamera>(alice.box) && !Last<S_StuntCamera>(carol.box),
	      "alice is not sent her own, and carol, outside the car, nothing");

	for (Raw *r : raws)
		r->box.clear();
	bob.net.Send(shot, CH_EVENT);
	pump(600, nullptr);
	Check(!Last<S_StuntCamera>(alice.box) && !Last<S_StuntCamera>(carol.box) &&
	          !Last<S_StuntCamera>(bob.box),
	      "a shot from the passenger seat goes nowhere");

	// The jump failed because alice got out; her exit is in before the end.
	C_ExitVehicle out{};
	InitHeader(out, 300);
	out.netId = netId;
	alice.net.Send(out, CH_EVENT);
	shot.body.on = 0;
	alice.net.Send(shot, CH_EVENT);
	pump(1500, [&] { return Last<S_StuntCamera>(bob.box) != nullptr; });
	const S_StuntCamera *over = Last<S_StuntCamera>(bob.box);
	Check(over && over->body.on == 0 && over->body.netId == netId,
	      "its end still reaches bob after alice has got out");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// A wreck: settled by the machine whose car it was, followed by everybody
// else, and handed to a joiner as a wreck where it came to rest
// (client/src/game/wreck.h).
void TestAWreckIsSettledAndHandedToAJoiner(Rig &rig) {
	std::printf("\na wreck, from the blast to a joiner\n");
	std::vector<Raw *> raws;
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		rig.Pump(ms, [&] {
			for (Raw *r : raws)
				r->net.Service(r->box);
			return done && done();
		});
	};
	Raw alice, bob;
	raws = {&alice, &bob};
	alice.net.Connect("127.0.0.1", rig.port);
	bob.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return alice.net.IsConnected() && bob.net.IsConnected(); });
	alice.net.Send(Hello("alice"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(alice.box) != nullptr; });
	bob.net.Send(Hello("bob"), CH_EVENT);
	pump(3000, [&] { return Last<S_Welcome>(bob.box) != nullptr; });
	const S_Welcome *welcome = Last<S_Welcome>(alice.box);
	const uint8_t    aliceId = welcome ? welcome->playerId : INVALID_PLAYER;

	C_EnterVehicle claim{};
	InitHeader(claim, 100);
	claim.body.netId   = INVALID_NETID;
	claim.body.seat    = 0;
	claim.body.modelId = 90;
	alice.net.Send(claim, CH_EVENT);
	pump(3000, [&] { return Last<S_EnterVehicle>(alice.box) && Last<S_EnterVehicle>(bob.box); });
	const S_EnterVehicle *seat  = Last<S_EnterVehicle>(alice.box);
	const uint16_t        netId = seat ? seat->body.netId : INVALID_NETID;
	Check(netId != INVALID_NETID && aliceId != INVALID_PLAYER, "alice drives a car of the session's");

	const auto state = [&](float x, uint8_t flags) {
		C_VehicleState s{};
		InitHeader(s, 300);
		s.body.netId  = netId;
		s.body.pos    = {x, 0.0f, 0.0f};
		s.body.rot    = {0.0f, 0.0f, 0.0f, 1.0f};
		s.body.health = flags ? 0.0f : 1000.0f;
		s.body.flags  = flags;
		return s;
	};

	// Her wreck's settle, ahead of the blast that makes it one.
	alice.box.clear();
	bob.box.clear();
	alice.net.Send(state(5.0f, VEH_WRECKED), CH_SNAPSHOT);
	pump(400, nullptr);
	Check(!Last<S_VehicleState>(bob.box), "a wreck's settle ahead of its blast goes nowhere");

	C_VehicleBlowUp blast{};
	InitHeader(blast, 400);
	blast.body.netId = netId;
	blast.body.pos   = {10.0f, 0.0f, 0.0f};
	blast.body.rot   = {0.0f, 0.0f, 0.0f, 1.0f};
	alice.net.Send(blast, CH_EVENT);
	pump(1500, [&] { return Last<S_VehicleCustody>(bob.box) != nullptr; });
	const S_VehicleCustody *custody = Last<S_VehicleCustody>(bob.box);
	Check(Last<S_VehicleBlowUp>(bob.box) && custody && custody->netId == netId &&
	          custody->playerId == aliceId &&
	          IndexOf(bob.box, OP_S_VEHICLE_BLOWUP) < IndexOf(bob.box, OP_S_VEHICLE_CUSTODY),
	      "bob hears the blast, and then that alice settles the wreck");

	bob.box.clear();
	alice.net.Send(state(15.0f, 0), CH_SNAPSHOT);
	bob.net.Send(state(16.0f, VEH_WRECKED), CH_SNAPSHOT);
	pump(400, nullptr);
	Check(!Last<S_VehicleState>(bob.box),
	      "her snapshot from before the blast, and bob's own word, go nowhere");
	alice.net.Send(state(20.0f, VEH_WRECKED), CH_SNAPSHOT);
	pump(1500, [&] { return Last<S_VehicleState>(bob.box) != nullptr; });
	const S_VehicleState *wreck = Last<S_VehicleState>(bob.box);
	Check(wreck && wreck->playerId == aliceId && wreck->body.pos.x == 20.0f &&
	          (wreck->body.flags & VEH_WRECKED) != 0,
	      "her wreck's settle reaches bob");

	// Carol joins while it is still settling.
	Raw carol;
	raws.push_back(&carol);
	carol.net.Connect("127.0.0.1", rig.port);
	pump(3000, [&] { return carol.net.IsConnected(); });
	carol.net.Send(Hello("carol"), CH_EVENT);
	pump(3000, [&] { return Last<S_VehicleCustody>(carol.box) != nullptr; });
	const S_VehicleSpawn *spawned = nullptr;
	for (const Message &m : carol.box)
		if (const S_VehicleSpawn *sp = m.as<S_VehicleSpawn>())
			if (sp->netId == netId)
				spawned = sp;
	Check(spawned && (spawned->flags & VEH_WRECKED) != 0 && spawned->health == 0.0f &&
	          spawned->pos.x == 20.0f,
	      "carol, joining, is handed the wreck where its settle had got to");
	Check(Last<S_VehicleCustody>(carol.box) &&
	          Last<S_VehicleCustody>(carol.box)->playerId == aliceId &&
	          IndexOf(carol.box, OP_S_VEHICLE_SPAWN) < IndexOf(carol.box, OP_S_VEHICLE_CUSTODY),
	      "and then that alice is settling it");

	bob.box.clear();
	C_VehicleSettled done{};
	InitHeader(done, 500);
	done.netId = netId;
	alice.net.Send(done, CH_EVENT);
	pump(1500, [&] { return Last<S_VehicleCustody>(bob.box) != nullptr; });
	Check(Last<S_VehicleCustody>(bob.box) &&
	          Last<S_VehicleCustody>(bob.box)->playerId == INVALID_PLAYER,
	      "it lies still, and everybody holds it there");

	for (Raw *r : raws)
		r->net.Disconnect();
	rig.Settle(300);
}

// The owner's first run with `missions = on`, 2026-09-24, on a server of its
// own: noxx3 and noxx2 both start a new game, both games connect in their
// intro, on Give Me Liberty's marker, and both claim the start when it ends.
// First as it goes now, each game saying when its intro is over; then as it
// went that day, neither ever saying so.
// Every packet the server takes, from a player in the session, filled with
// whatever a buggy or hostile client might put in it: the server stays up
// and still lets somebody in. Mostly the small values and edges a field is
// checked against, otherwise random, from a fixed seed.
#define COOPIII_SERVER_PACKETS(X)                                                                  \
	X(C_CampaignDelta) X(C_CampaignSince) X(C_ContactMarkers) X(C_PlaceBlips) X(C_CarDespawn) X(C_CarHit) X(C_CarLetGo) X(C_CarLists)  \
	X(C_CarSpawn) X(C_CarStates) X(C_Chat) X(C_Cheat) X(C_CopHandover) X(C_CutsceneState)          \
	X(C_Damage) X(C_Death)                                                                         \
	X(C_DesyncProbe) X(C_EnterVehicle) X(C_EnteringVehicle) X(C_ExitVehicle) X(C_Explosion)        \
	X(C_GarageState) X(C_GateState) X(C_HeliGone) X(C_HeliHit) X(C_HeliShot) X(C_HeliState)        \
	X(C_Hello) X(C_JackingVehicle) X(C_Kick) X(C_LobbyJoin) X(C_LobbyStart) X(C_MineBlast)         \
	X(C_MissionAnswers) X(C_MissionBoard) X(C_MissionBomb) X(C_MissionBusy) X(C_MissionCatchUp)    \
	X(C_MissionCheckpoint) X(C_MissionClaim) X(C_MissionEffect) X(C_MissionEnded) X(C_MissionKill) \
	X(C_MissionObjectBreak) X(C_MissionPickup) X(C_MissionReady) X(C_MissionSeats)                 \
	X(C_MissionStarted) X(C_MissionWidget) X(C_MoneyAward) X(C_MoneyChange) X(C_NpcDamage)         \
	X(C_NpcShot) X(C_NpcVehicleHit) X(C_ObjectBroken) X(C_ObjectRebuilt) X(C_ObjectSettled)        \
	X(C_Password) X(C_PedBodyPart) X(C_PedDamage) X(C_PedDeath) X(C_PedDespawn) X(C_PedRevive)     \
	X(C_PedSpeech) X(C_PackagesLive)                                                               \
	X(C_PedSpawn) X(C_PedStates) X(C_PickupClaim) X(C_PickupCollected) X(C_PickupDrop)             \
	X(C_PickupRelease) X(C_PlayerAmmo) X(C_PlayerAway) X(C_PlayerLook) X(C_PlayerModel)            \
	X(C_PlayerMoney)                                                                               \
	X(C_PlayerState) X(C_PlayerStateRide) X(C_RampageArrived) X(C_RampageCar) X(C_RampageEnd)      \
	X(C_RampageKill) X(C_RampageStart) X(C_RampageVote) X(C_Respawn) X(C_Respray) X(C_Shot)        \
	X(C_StuntCamera) X(C_UnownedBlowUp) X(C_VehicleAim) X(C_VehicleAlarm) X(C_VehicleBlowUp)       \
	X(C_VehicleColour)                                                                             \
	X(C_VehicleBomb) X(C_VehicleDamage) X(C_VehicleHit) X(C_VehicleRadio) X(C_VehicleRemoved)      \
	X(C_VehicleSettled) X(C_VehicleState) X(C_WaterCannon) X(C_WorldState)

void TestNoPacketCrashesTheServer() {
	std::printf("\nevery packet the server takes, with anything in it\n");
	struct Packet {
		uint8_t opcode;
		size_t  size;
	};
#define COOPIII_FUZZ_ENTRY(T) Packet{T::OPCODE, sizeof(T)},
	static const Packet kPackets[] = {COOPIII_SERVER_PACKETS(COOPIII_FUZZ_ENTRY)};
#undef COOPIII_FUZZ_ENTRY
	Rig rig;
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	bool listening = false;
	for (uint16_t port = 24230; port < 24250 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening) {
		std::printf("  [skipped] no port to listen on\n");
		return;
	}
	rig.server.SetMissionRules(true, MISSION_MARGIN_CM_DEFAULT);
	NetClient fuzzer, other, late;
	rig.clients = {&fuzzer, &other, &late};
	rig.inbox.resize(3);
	Join(rig, 0, "fuzz");
	Join(rig, 1, "other");

	uint32_t seed = 0x5E7E7113u;
	auto     next = [&seed]() {
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		return seed;
	};
	static const uint8_t kEdges[] = {0, 1, 2, 3, 7, 8, 0x7F, 0x80, 0xFF};
	std::vector<uint8_t> bytes;
	size_t               sent = 0;
	for (int round = 0; round < 6000; ++round) {
		if (!fuzzer.IsConnected()) {
			fuzzer.Disconnect();
			rig.inbox[0].clear();
			Join(rig, 0, "fuzz");
			if (!fuzzer.IsConnected())
				break;
		}
		const Packet &p = kPackets[next() % (sizeof kPackets / sizeof kPackets[0])];
		bytes.resize(p.size);
		for (size_t i = 0; i < p.size; ++i) {
			const uint32_t r = next();
			bytes[i]         = (r & 3) != 0 ? kEdges[(r >> 2) % sizeof kEdges] : uint8_t(r >> 8);
		}
		bytes[0] = p.opcode;
		fuzzer.SendRaw(bytes.data(), bytes.size(), (next() & 1) ? CH_EVENT : CH_SNAPSHOT);
		++sent;
		if ((round & 31) == 31) {
			rig.Pump(5, nullptr);
			rig.ClearInboxes();
		}
	}
	rig.Settle(300);
	const uint8_t id = Join(rig, 2, "late");
	Check(sent > 3000 && id != INVALID_PLAYER,
	      "thousands of them, and the server is still up and lets somebody in");
	for (NetClient *client : rig.clients)
		client->Disconnect();
	rig.Settle();
	rig.server.Stop();
}

void TestTwoNewGamesAtTheBridge() {
	std::printf("\ntwo new games at the bridge, one Give Me Liberty\n");
	Rig rig;
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	bool listening = false;
	for (uint16_t port = 24190; port < 24210 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening) {
		std::printf("  [skipped] no port to listen on\n");
		return;
	}
	rig.server.SetMissionRules(true, MISSION_MARGIN_CM_DEFAULT);
	constexpr uint32_t busyWait = 2500;
	rig.server.SetMissionBusyWaitMs(busyWait);
	NetClient first, second;
	rig.clients = {&first, &second};
	rig.inbox.resize(2);
	const uint8_t noxx3 = Join(rig, 0, "noxx3");
	const uint8_t noxx2 = Join(rig, 1, "noxx2");
	Check(noxx3 == 0 && noxx2 == 1, "noxx3 and noxx2, in slots 0 and 1");

	const MissionArea marker = MissionAreaLocate2D(811.875f, -939.9375f, 3.5f, 3.5f);
	C_MissionBusy     busy{};
	InitHeader(busy, 100);
	const auto sayBusy = [&](size_t i, uint8_t on) {
		busy.busy = on;
		rig.clients[i]->Send(busy, CH_EVENT);
	};
	const auto claim = [&](size_t i) {
		rig.inbox[i].clear();
		rig.clients[i]->Send(Claim(marker, 19), CH_EVENT);
		rig.Pump(1500, [&] { return Last<S_MissionClaim>(rig.inbox[i]) != nullptr; });
		return Last<S_MissionClaim>(rig.inbox[i]);
	};
	sayBusy(0, 1);
	sayBusy(1, 1);
	rig.clients[0]->Send(StateAt(811.875f, -939.9375f, 35.75f), CH_SNAPSHOT);
	rig.clients[1]->Send(StateAt(811.875f, -939.9375f, 35.75f), CH_SNAPSHOT);
	rig.Settle(300);

	// As it goes now: noxx3's intro ends first, noxx2's 0.8 s later.
	rig.ClearInboxes();
	sayBusy(0, 0);
	rig.Settle();
	const S_MissionClaim *answer = claim(0);
	rig.Settle();
	Check(answer && answer->verdict == MISSION_CLAIM_WAITING && answer->missingMask == PlayerBit(noxx2),
	      "noxx3 claims Give Me Liberty, and waits for noxx2, still in the intro");
	const S_MissionWaiting *told = Last<S_MissionWaiting>(rig.inbox[1]);
	Check(told && told->ownerId == noxx3 && told->busyMask == PlayerBit(noxx2) &&
	          told->goesOnInS == (busyWait + 999) / 1000,
	      "noxx2 hears noxx3 waits for its intro, and how long the start will");
	sayBusy(1, 0);
	rig.Settle();
	answer = claim(1);
	Check(answer && answer->verdict == MISSION_CLAIM_BUSY && answer->ownerId == noxx3,
	      "noxx2's own trigger, out of its intro, finds the start noxx3's");
	answer = claim(0);
	Check(answer && answer->verdict == MISSION_CLAIM_GRANTED,
	      "noxx3's next claim is granted, with noxx2 on the marker");
	rig.ClearInboxes();
	C_MissionStarted started;
	InitHeader(started, 100);
	started.launchKey     = 0x4242;
	started.missionNumber = 19;
	rig.clients[0]->Send(started, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionState>(rig.inbox[1]) != nullptr; });
	rig.Settle();
	const S_MissionState *st = Last<S_MissionState>(rig.inbox[1]);
	Check(st && st->state == MISSION_STATE_RUNNING && st->ownerId == noxx3 && st->missionNumber == 19 &&
	          st->participants == (PlayerBit(noxx3) | PlayerBit(noxx2)),
	      "Give Me Liberty runs on noxx3's machine, once, with noxx2 in it");
	answer = claim(1);
	Check(answer && answer->verdict == MISSION_CLAIM_BUSY, "and noxx2 is never granted a copy");
	C_MissionEnded ended;
	InitHeader(ended, 100);
	ended.missionNumber = 19;
	ended.outcome       = MISSION_OUTCOME_FAILED;
	rig.clients[0]->Send(ended, CH_EVENT);
	rig.Settle(300);

	// As it went that day: both games in their intro as far as the server can
	// tell, for good.
	sayBusy(0, 1);
	sayBusy(1, 1);
	rig.Settle(300);
	answer = claim(0);
	Check(answer && answer->verdict == MISSION_CLAIM_WAITING, "noxx3 claims again, and waits for noxx2");
	answer = claim(1);
	Check(answer && answer->verdict == MISSION_CLAIM_BUSY, "noxx2's game, never out, is held by noxx3's claim");
	const uint32_t since = Ms();
	bool           granted = false;
	while (!granted && Ms() - since < 3 * busyWait) {
		answer  = claim(0);
		granted = answer && answer->verdict == MISSION_CLAIM_GRANTED;
		if (!granted)
			rig.Settle(400);
	}
	const uint32_t took = Ms() - since;
	Check(granted && took + 600 >= busyWait,
	      "noxx3's start is granted once the busy wait is up, and not before");
	rig.ClearInboxes();
	rig.clients[0]->Send(started, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionState>(rig.inbox[1]) != nullptr; });
	rig.Settle();
	st = Last<S_MissionState>(rig.inbox[1]);
	Check(st && st->state == MISSION_STATE_RUNNING && st->participants == PlayerBit(noxx3),
	      "it starts without noxx2, whose game says it is still in its intro");
	rig.ClearInboxes();
	sayBusy(1, 0);
	rig.Pump(1500, [&] { return Last<S_MissionState>(rig.inbox[1]) != nullptr; });
	rig.Settle();
	st = Last<S_MissionState>(rig.inbox[1]);
	Check(st && st->participants == (PlayerBit(noxx3) | PlayerBit(noxx2)),
	      "and noxx2 comes into it as a joiner once its intro is over");

	first.Disconnect();
	second.Disconnect();
	rig.Settle(300);
	rig.server.Stop();
}

C_WorldState WorldAt(uint8_t hour, uint8_t minute, uint8_t weather, uint8_t weatherOld) {
	C_WorldState w;
	InitHeader(w, 100);
	w.body.hour       = hour;
	w.body.minute     = minute;
	w.body.weather    = weather;
	w.body.weatherOld = weatherOld;
	return w;
}

// A test in two games: noxx2 hosts, noxx3 runs Give Me Liberty,
// whose script sets 04:00 and a cloudy sky on noxx3's machine alone.
void TestTheSkyFollowsTheMission() {
	std::printf("\nthe clock and the sky while noxx3's mission runs and noxx2 hosts\n");
	Rig rig;
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	bool listening = false;
	for (uint16_t port = 24210; port < 24230 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening) {
		std::printf("  [skipped] no port to listen on\n");
		return;
	}
	rig.server.SetMissionRules(true, MISSION_MARGIN_CM_DEFAULT);
	NetClient first, second;
	rig.clients = {&first, &second};
	rig.inbox.resize(2);
	const uint8_t noxx2 = Join(rig, 0, "noxx2");
	const uint8_t noxx3 = Join(rig, 1, "noxx3");
	Check(noxx2 == 0 && noxx3 == 1, "noxx2 is in first, so noxx2 is the host");

	const auto lastWorld = [&](size_t i) { return Last<S_WorldState>(rig.inbox[i]); };
	rig.ClearInboxes();
	rig.clients[0]->Send(WorldAt(12, 3, 0, 0), CH_EVENT);
	rig.Pump(1500, [&] {
		const S_WorldState *s = lastWorld(1);
		return s && s->body.minute == 3;
	});
	const S_WorldState *w = lastWorld(1);
	Check(w && w->body.hour == 12 && w->body.minute == 3 && w->hostPlayerId == noxx2,
	      "with no mission noxx2's 12:03 is the session's");

	C_MissionStarted started;
	InitHeader(started, 100);
	started.launchKey     = 0x4242;
	started.missionNumber = 19;
	rig.ClearInboxes();
	// On one channel, as noxx3's client sends them: the start, then the sky
	// the mission has just set.
	rig.clients[1]->Send(started, CH_EVENT);
	rig.clients[1]->Send(WorldAt(4, 0, 1, 1), CH_EVENT);
	rig.Pump(1500, [&] {
		const S_WorldState *s = lastWorld(0);
		return s && s->body.hour == 4;
	});
	w = lastWorld(0);
	Check(w && w->body.hour == 4 && w->body.minute == 0 && w->body.weather == 1 &&
	          w->body.weatherOld == 1,
	      "noxx3's START_MISSION makes its 04:00 and its cloudy sky the session's");
	Check(w && w->hostPlayerId == noxx2, "and noxx2 is still the host");

	rig.ClearInboxes();
	rig.clients[0]->Send(WorldAt(12, 4, 0, 0), CH_EVENT);
	rig.Settle(300);
	bool noMidday = true;
	for (size_t i = 0; i < 2; ++i)
		for (const Message &m : rig.inbox[i])
			if (const S_WorldState *s = m.as<S_WorldState>())
				noMidday = noMidday && s->body.hour == 4;
	Check(noMidday, "the host's 12:04 is dropped while the mission runs, not fought over");

	rig.ClearInboxes();
	C_Cheat rain;
	InitHeader(rain, 100);
	rain.body.cheat = CHEAT_RAINY;
	rain.body.state = 0;
	rig.clients[0]->Send(rain, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_Cheat>(rig.inbox[1]) != nullptr; });
	const S_Cheat *cheat = Last<S_Cheat>(rig.inbox[1]);
	Check(cheat && cheat->body.cheat == CHEAT_RAINY && cheat->playerId == noxx2,
	      "noxx2's sky cheat goes to noxx3, whose sky everybody has");

	C_MissionEnded ended;
	InitHeader(ended, 100);
	ended.missionNumber = 19;
	ended.outcome       = MISSION_OUTCOME_PASSED;
	rig.ClearInboxes();
	rig.clients[1]->Send(ended, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionState>(rig.inbox[0]) != nullptr; });
	rig.clients[1]->Send(WorldAt(4, 30, 1, 1), CH_EVENT);
	rig.clients[0]->Send(WorldAt(4, 31, 2, 1), CH_EVENT);
	rig.Pump(1500, [&] {
		const S_WorldState *s = lastWorld(1);
		return s && s->body.minute == 31;
	});
	w = lastWorld(1);
	Check(w && w->body.hour == 4 && w->body.minute == 31 && w->body.weather == 2,
	      "the mission over, the sky is noxx2's again, going on from where it was left");

	first.Disconnect();
	second.Disconnect();
	rig.Settle(300);
	rig.server.Stop();
}

// The owner's run on 2026-09-24, Give Me Liberty failed and retried: noxx3's
// mission made the Kuruma, noxx2 drove it and got out, it failed, and both
// came back to the bridge with noxx2 dead on it for a moment.
void TestAFailedMissionIsClearedForTheRetry() {
	std::printf("\na failed mission, cleared away for the retry\n");
	Rig rig;
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	bool listening = false;
	for (uint16_t port = 24250; port < 24270 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening) {
		std::printf("  [skipped] no port to listen on\n");
		return;
	}
	rig.server.SetMissionRules(true, MISSION_MARGIN_CM_DEFAULT);
	NetClient first, second;
	rig.clients = {&first, &second};
	rig.inbox.resize(2);
	const uint8_t noxx3 = Join(rig, 0, "noxx3");
	const uint8_t noxx2 = Join(rig, 1, "noxx2");
	Check(noxx3 == 0 && noxx2 == 1, "noxx3 and noxx2 are in");

	C_MissionStarted started;
	InitHeader(started, 100);
	started.launchKey     = 0x4242;
	started.missionNumber = 19;
	rig.clients[0]->Send(started, CH_EVENT);
	rig.Settle();

	// The Kuruma, made by noxx3's mission and hosted there.
	rig.ClearInboxes();
	C_CarSpawn kuruma{};
	InitHeader(kuruma, 100);
	kuruma.tempId        = 7;
	kuruma.body.modelId  = 111;
	kuruma.body.extra1   = -1;
	kuruma.body.extra2   = -1;
	kuruma.body.flags    = AMBIENT_MISSION;
	kuruma.body.pos      = {812.0f, -945.5f, 35.75f};
	kuruma.body.rot      = {0.0f, 0.0f, 0.0f, 1.0f};
	rig.clients[0]->Send(kuruma, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_CarSpawn>(rig.inbox[1]) != nullptr; });
	const S_CarSpawn *named = Last<S_CarSpawn>(rig.inbox[1]);
	const uint16_t    netId = named ? named->netId : INVALID_NETID;
	Check(named && named->ownerPlayerId == noxx3, "the mission's Kuruma is named for everybody");

	// noxx2 takes its wheel, and gets out again.
	C_EnterVehicle enter{};
	InitHeader(enter, 100);
	enter.body.netId = netId;
	enter.body.seat  = 0;
	rig.clients[1]->Send(enter, CH_EVENT);
	rig.Settle();
	C_ExitVehicle exit{};
	InitHeader(exit, 100);
	exit.netId = netId;
	rig.clients[1]->Send(exit, CH_EVENT);
	rig.Settle();

	rig.ClearInboxes();
	C_MissionEnded ended;
	InitHeader(ended, 100);
	ended.missionNumber = 19;
	ended.outcome       = MISSION_OUTCOME_FAILED;
	rig.clients[0]->Send(ended, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_VehicleDespawn>(rig.inbox[1]) != nullptr; });
	rig.Settle();
	const S_VehicleDespawn *gone0 = Last<S_VehicleDespawn>(rig.inbox[0]);
	const S_VehicleDespawn *gone1 = Last<S_VehicleDespawn>(rig.inbox[1]);
	Check(gone0 && gone1 && gone0->netId == netId && gone1->netId == netId,
	      "the failure takes the empty Kuruma off both machines");
	const S_MissionState *st = Last<S_MissionState>(rig.inbox[1]);
	Check(st && st->state == MISSION_STATE_IDLE && IndexOf(rig.inbox[1], OP_S_MISSION_STATE) <
	                                                   IndexOf(rig.inbox[1], OP_S_VEHICLE_DESPAWN),
	      "after the mission is over, not while it runs");

	// Back at the bridge, noxx2 dead on the marker.
	rig.clients[0]->Send(StateAt(811.875f, -939.9375f, 35.75f), CH_SNAPSHOT);
	rig.clients[1]->Send(StateAt(812.5f, -939.0f, 35.75f), CH_SNAPSHOT);
	rig.Settle();
	C_Death death;
	InitHeader(death, 100);
	death.killerNetId = INVALID_NETID;
	rig.clients[1]->Send(death, CH_EVENT);
	rig.Settle();
	rig.ClearInboxes();
	const MissionArea marker = MissionAreaLocate2D(811.875f, -939.9375f, 3.5f, 3.5f);
	rig.clients[0]->Send(Claim(marker, 19), CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionClaim>(rig.inbox[0]) != nullptr; });
	const S_MissionClaim *answer = Last<S_MissionClaim>(rig.inbox[0]);
	Check(answer && answer->verdict == MISSION_CLAIM_WAITING && answer->missingMask == PlayerBit(noxx2),
	      "the retry waits for noxx2, dead on the marker");

	C_Respawn respawn;
	InitHeader(respawn, 100);
	respawn.body.pos     = {811.875f, -939.9375f, 35.75f};
	respawn.body.heading = 3.14f;
	rig.clients[1]->Send(respawn, CH_EVENT);
	rig.Settle();
	rig.ClearInboxes();
	rig.clients[0]->Send(Claim(marker, 19), CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionClaim>(rig.inbox[0]) != nullptr; });
	answer = Last<S_MissionClaim>(rig.inbox[0]);
	Check(answer && answer->verdict == MISSION_CLAIM_GRANTED, "and starts once he is back on his feet");

	first.Disconnect();
	second.Disconnect();
	rig.Settle(300);
	rig.server.Stop();
}

C_CutsceneState SceneState(const char *name, uint8_t scope, uint8_t skip = 0, uint8_t voteId = 0) {
	C_CutsceneState s{};
	InitHeader(s, 100);
	s.key.scope = scope;
	if (name)
		std::strncpy(s.key.name, name, CUTSCENE_NAME_LEN);
	s.skip   = skip;
	s.voteId = voteId;
	return s;
}

// Three players, two of them in the intro: who hears the count, the skip,
// and that the third hears none of it.
void TestACutsceneIsSkippedTogether() {
	std::printf("\na cutscene skipped together\n");
	Rig rig;
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	bool listening = false;
	for (uint16_t port = 24270; port < 24290 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening) {
		std::printf("  [skipped] no port to listen on\n");
		return;
	}
	NetClient a, b, c;
	rig.clients = {&a, &b, &c};
	rig.inbox.resize(3);
	const uint8_t alice = Join(rig, 0, "alice");
	const uint8_t bob   = Join(rig, 1, "bob");
	const uint8_t carol = Join(rig, 2, "carol");
	Check(alice == 0 && bob == 1 && carol == 2, "three players");
	rig.Settle();
	rig.ClearInboxes();

	rig.clients[0]->Send(SceneState("bet", CUTSCENE_SCOPE_OWN), CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_CutsceneVote>(rig.inbox[0]) != nullptr; });
	const S_CutsceneVote *v = Last<S_CutsceneVote>(rig.inbox[0]);
	Check(v && v->kind == CUTSCENE_VOTE_COUNT && v->body.voters == 1,
	      "alice alone in the intro is told she is alone");

	rig.clients[1]->Send(SceneState("BET", CUTSCENE_SCOPE_OWN), CH_EVENT);
	rig.Pump(1500, [&] {
		const S_CutsceneVote *x = Last<S_CutsceneVote>(rig.inbox[0]);
		return x && x->body.voters == 2 && Last<S_CutsceneVote>(rig.inbox[1]);
	});
	v                          = Last<S_CutsceneVote>(rig.inbox[0]);
	const S_CutsceneVote *vb   = Last<S_CutsceneVote>(rig.inbox[1]);
	Check(v && vb && v->body.voters == 2 && vb->body.voters == 2 && v->body.needed == 2 &&
	          v->body.voteId == vb->body.voteId && std::strcmp(v->body.key.name, "bet") == 0,
	      "bob comes into the same intro: both hear 0 of 2, one count");
	const uint8_t vote = v ? v->body.voteId : 0;

	rig.ClearInboxes();
	rig.clients[0]->Send(SceneState("bet", CUTSCENE_SCOPE_OWN, 1, vote), CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_CutsceneVote>(rig.inbox[1]) != nullptr; });
	vb = Last<S_CutsceneVote>(rig.inbox[1]);
	Check(vb && vb->kind == CUTSCENE_VOTE_COUNT && vb->body.yes == 1 &&
	          vb->body.yesMask == PlayerBit(alice),
	      "alice says skip: bob hears 1 of 2, and nobody skips");

	rig.ClearInboxes();
	rig.clients[1]->Send(SceneState("bet", CUTSCENE_SCOPE_OWN, 1, vote), CH_EVENT);
	rig.Pump(1500, [&] {
		return Last<S_CutsceneVote>(rig.inbox[0]) && Last<S_CutsceneVote>(rig.inbox[1]);
	});
	v  = Last<S_CutsceneVote>(rig.inbox[0]);
	vb = Last<S_CutsceneVote>(rig.inbox[1]);
	Check(v && vb && v->kind == CUTSCENE_VOTE_SKIP && vb->kind == CUTSCENE_VOTE_SKIP &&
	          v->body.voteId == vote && v->body.yes == 2,
	      "bob says skip: both are told to skip it");
	rig.Settle();
	Check(Count<S_CutsceneVote>(rig.inbox[2]) == 0, "carol, in no cutscene, heard none of it");

	// The mission's scene is another one, even under the same name.
	rig.ClearInboxes();
	rig.clients[2]->Send(SceneState("bet", CUTSCENE_SCOPE_SHARED), CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_CutsceneVote>(rig.inbox[2]) != nullptr; });
	const S_CutsceneVote *vc = Last<S_CutsceneVote>(rig.inbox[2]);
	Check(vc && vc->kind == CUTSCENE_VOTE_COUNT && vc->body.voters == 1,
	      "carol in a scene of the same name in the session's mission is alone in hers");

	// Out, and in a new scene together, then one of them goes.
	rig.clients[0]->Send(SceneState(nullptr, CUTSCENE_SCOPE_NONE), CH_EVENT);
	rig.clients[1]->Send(SceneState(nullptr, CUTSCENE_SCOPE_NONE), CH_EVENT);
	rig.clients[0]->Send(SceneState("j1_lfl", CUTSCENE_SCOPE_SHARED), CH_EVENT);
	rig.clients[1]->Send(SceneState("j1_lfl", CUTSCENE_SCOPE_SHARED), CH_EVENT);
	rig.Settle(300);
	rig.ClearInboxes();
	b.Disconnect();
	rig.Pump(3000, [&] {
		const S_CutsceneVote *x = Last<S_CutsceneVote>(rig.inbox[0]);
		return x && x->body.voters == 1;
	});
	v = Last<S_CutsceneVote>(rig.inbox[0]);
	Check(v && v->kind == CUTSCENE_VOTE_COUNT && v->body.voters == 1,
	      "bob leaves the session in the middle of the next scene: alice is alone in it again");

	a.Disconnect();
	c.Disconnect();
	rig.Settle(300);
	rig.server.Stop();
}

// A game that crashed, or an owner whose connection dropped: the server
// notices within seconds, and an owner who comes back with its mission still
// running offers it again and has it taken up, effects and campaign delta and
// all (docs/missions.md 12.3).
void TestAnOwnerWhoDroppedOffIsTakenUpAgain() {
	std::printf("\nan owner whose connection dropped takes its mission up again\n");
	Rig rig;
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	bool listening = false;
	for (uint16_t port = 24310; port < 24330 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening) {
		std::printf("  [skipped] no port to listen on\n");
		return;
	}
	rig.server.SetMissionRules(true, MISSION_MARGIN_CM_DEFAULT);
	NetClient first, second, third;
	rig.clients = {&first, &second, &third};
	rig.inbox.resize(3);
	const uint8_t alice = Join(rig, 0, "alice");
	const uint8_t bob   = Join(rig, 1, "bob");
	Check(alice == 0 && bob == 1, "alice and bob are in");

	// Every connection the server has is given the short timeout.
	uint32_t connected = 0, shortened = 0;
	for (PeerId peer = 0; peer < 8; ++peer) {
		const uint32_t max = rig.server.Net().PeerTimeoutMaxMs(peer);
		if (max != 0)
			++connected;
		if (max == NET_PEER_TIMEOUT_MAX_MS)
			++shortened;
	}
	Check(connected == 2 && shortened == 2,
	      "every connection is given the short timeout");

	// A game that stops answering, as a crashed one does: never serviced
	// again, never saying goodbye.
	NetClient frozen;
	rig.clients.push_back(&frozen);
	rig.inbox.emplace_back();
	const uint8_t ghost = Join(rig, 3, "ghost");
	rig.clients[3] = nullptr;
	rig.ClearInboxes();
	const auto since = std::chrono::steady_clock::now();
	rig.Pump(NET_PEER_TIMEOUT_MAX_MS + 3000, [&] {
		const S_PlayerLeave *gone = Last<S_PlayerLeave>(rig.inbox[1]);
		return gone && gone->playerId == ghost;
	});
	const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
	                      std::chrono::steady_clock::now() - since).count();
	const S_PlayerLeave *gone = Last<S_PlayerLeave>(rig.inbox[1]);
	Check(ghost != INVALID_PLAYER && gone && gone->playerId == ghost &&
	          took <= static_cast<long long>(NET_PEER_TIMEOUT_MAX_MS) + 1000,
	      "a game that stops answering is gone within seconds, not ENet's half a minute");
	Check(took >= static_cast<long long>(NET_PEER_TIMEOUT_MIN_MS) - 1000,
	      "and not before the minimum, so a hitch shorter than that drops nobody");
	if (g_verbose)
		std::printf("    | dropped after %lld ms\n", static_cast<long long>(took));
	rig.clients.pop_back();
	rig.inbox.pop_back();

	C_MissionStarted started;
	InitHeader(started, 100);
	started.launchKey     = 0x4242;
	started.missionNumber = 21;
	rig.clients[0]->Send(started, CH_EVENT);
	rig.Settle();

	rig.ClearInboxes();
	rig.clients[0]->Disconnect();
	rig.Pump(3000, [&] { return Last<S_MissionState>(rig.inbox[1]) != nullptr; });
	rig.Settle();
	const S_MissionState *st = Last<S_MissionState>(rig.inbox[1]);
	Check(st && st->state == MISSION_STATE_IDLE && st->outcome == MISSION_OUTCOME_OWNER_LEFT,
	      "alice's connection going fails it on bob's screen");

	// Back on a new connection, the script still running on her machine.
	rig.ClearInboxes();
	const uint8_t back = Join(rig, 2, "alice");
	rig.Settle();
	C_MissionStarted again;
	InitHeader(again, 100);
	again.launchKey     = 0;
	again.missionNumber = 21;
	rig.clients[2]->Send(again, CH_EVENT);
	rig.Pump(1500, [&] {
		const S_MissionState *s = Last<S_MissionState>(rig.inbox[1]);
		return s && s->state == MISSION_STATE_RUNNING;
	});
	st = Last<S_MissionState>(rig.inbox[1]);
	Check(back != INVALID_PLAYER && st && st->state == MISSION_STATE_RUNNING && st->ownerId == back &&
	          st->missionNumber == 21 && (st->participants & PlayerBit(bob)) != 0,
	      "offered again, it is the session's once more, hers, with bob in it");

	rig.ClearInboxes();
	C_MissionEffect title;
	InitHeader(title, 100);
	title.body.missionNumber = 21;
	title.body.kind          = MISSION_EFFECT_RUN;
	title.body.length        = 2;
	title.body.code[0]       = 0xBA;
	rig.clients[2]->Send(title, CH_EVENT);
	C_CampaignDelta delta;
	InitHeader(delta, 100);
	delta.body.missionNumber    = 21;
	delta.body.valueCount       = 1;
	delta.body.values[0].offset = 900;
	delta.body.values[0].value  = 1;
	delta.body.last             = 1;
	rig.clients[2]->Send(delta, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_CampaignDelta>(rig.inbox[1]) != nullptr; });
	rig.Settle();
	Check(Last<S_MissionEffect>(rig.inbox[1]) != nullptr,
	      "what her mission shows reaches bob again");
	const S_CampaignDelta *left = Last<S_CampaignDelta>(rig.inbox[1]);
	Check(left && left->ownerId == back && left->body.seq == 1,
	      "and what it leaves behind is kept, not refused as somebody else's");

	for (NetClient *client : rig.clients)
		if (client)
			client->Disconnect();
	rig.Settle();
	rig.server.Stop();
}

// A rampage's verdict names the player whose report of a pass won, so a
// shared wallet keeps one reward for it.
void TestARampagePassNamesItsPayer() {
	std::printf("\na rampage's pass, and whose reward the wallet keeps\n");
	Rig rig;
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	bool listening = false;
	for (uint16_t port = 24380; port < 24400 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening) {
		std::printf("  [skipped] no port to listen on\n");
		return;
	}
	NetClient first, second;
	rig.clients = {&first, &second};
	rig.inbox.resize(2);
	const uint8_t alice = Join(rig, 0, "alice");
	const uint8_t bob   = Join(rig, 1, "bob");

	C_RampageStart start;
	InitHeader(start, 100);
	start.body.limitMs = 120000;
	start.body.target  = 20;
	first.Send(start, CH_EVENT);
	second.Send(start, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_RampageOpen>(rig.inbox[1]) != nullptr; });
	// Copied out before the clear, which frees the message the pointer is in.
	const S_RampageOpen *open     = Last<S_RampageOpen>(rig.inbox[1]);
	const uint16_t       frenzyId = open ? open->body.frenzyId : 0;

	rig.ClearInboxes();
	C_RampageEnd end;
	InitHeader(end, 100);
	end.body.frenzyId = frenzyId;
	end.body.outcome  = RAMPAGE_PASSED;
	second.Send(end, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_RampageEnd>(rig.inbox[0]) != nullptr; });
	first.Send(end, CH_EVENT);
	rig.Settle();
	const S_RampageEnd *told = Last<S_RampageEnd>(rig.inbox[0]);
	Check(alice == 0 && bob == 1 && told && told->body.outcome == RAMPAGE_PASSED &&
	          told->payerId == bob && Count<S_RampageEnd>(rig.inbox[0]) == 1,
	      "bob's pass got there first, so bob is the one the wallet pays, once");
	told = Last<S_RampageEnd>(rig.inbox[1]);
	Check(told && told->payerId == bob, "and bob is told so too");

	first.Disconnect();
	second.Disconnect();
	rig.Settle();
	rig.server.Stop();
}

// A server with a progress file, listening; false and nothing started when no
// port is free.
bool StartKeeping(Rig &rig, const std::string &path, bool keep) {
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	rig.server.SetProgressPath(path);
	bool listening = false;
	for (uint16_t port = 24350; port < 24370 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening)
		return false;
	ServerConfig cfg;
	cfg.keepProgress = keep;
	rig.server.Configure(cfg);
	return true;
}

bool FileThere(const std::string &path) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (fh)
		std::fclose(fh);
	return fh != nullptr;
}

// What a session did to the campaign is still there when the server is
// started again, and a player who wasn't there is brought up to it.
void TestProgressOutlivesTheServer() {
	std::printf("\nthe session's progress, from one run of the server to the next\n");
	const char *temp = std::getenv("TEMP");
	const std::string path = std::string(temp ? temp : ".") + "\\coopiii-servertest-progress.dat";
	std::remove(path.c_str());
	std::remove((path + ".old").c_str());

	PickupIdent package{};
	package.pos        = {300.0f, -200.0f, 12.0f};
	package.modelIndex = 1321;
	package.type       = PICKUP_TYPE_PACKAGE;
	uint32_t firstLog  = 0;
	{
		Rig rig;
		if (!StartKeeping(rig, path, true)) {
			std::printf("  [skipped] no port to listen on\n");
			return;
		}
		NetClient first;
		rig.clients = {&first};
		rig.inbox.resize(1);
		Join(rig, 0, "alice");
		rig.Settle();
		const S_MissionState *st = Last<S_MissionState>(rig.inbox[0]);
		firstLog                 = st ? st->campaignLog : 0;

		C_MissionStarted started;
		InitHeader(started, 100);
		started.launchKey     = 0x4242;
		started.missionNumber = 19;
		first.Send(started, CH_EVENT);
		rig.Settle();
		C_CampaignDelta delta;
		InitHeader(delta, 100);
		delta.body.missionNumber    = 19;
		delta.body.valueCount       = 1;
		delta.body.values[0].offset = 900;
		delta.body.values[0].value  = 1;
		delta.body.last             = 1;
		delta.body.scriptHash       = 0x1234ABCDu;
		first.Send(delta, CH_EVENT);

		C_CarLists lists;
		InitHeader(lists, 100);
		lists.collected[0] = 0x3;
		lists.collected[3] = 0x10;
		first.Send(lists, CH_EVENT);

		C_PickupClaim claim;
		InitHeader(claim, 100);
		claim.ident = package;
		first.Send(claim, CH_EVENT);
		rig.Pump(1500, [&] { return Last<S_PickupGrant>(rig.inbox[0]) != nullptr; });
		C_PickupCollected took;
		InitHeader(took, 100);
		took.ident = package;
		first.Send(took, CH_EVENT);
		rig.Pump(1500, [&] { return Last<S_CampaignDelta>(rig.inbox[0]) != nullptr; });
		rig.Settle(1300);
		Check(FileThere(path), "a second after a change the file is there");
		Check(rig.server.KeepsProgress(), "and the server says it keeps it");

		first.Disconnect();
		rig.Settle();
		rig.server.Stop();
	}

	{
		Rig rig;
		if (!StartKeeping(rig, path, true)) {
			std::printf("  [skipped] no port to listen on\n");
			return;
		}
		NetClient second;
		rig.clients = {&second};
		rig.inbox.resize(1);
		Join(rig, 0, "bob");
		rig.Settle();
		const S_MissionState *st = Last<S_MissionState>(rig.inbox[0]);
		Check(st && firstLog != 0 && st->campaignLog == firstLog,
		      "started again, the log has the same name, so a client that kept its copy keeps it");
		const S_PickupTaken *gone = Last<S_PickupTaken>(rig.inbox[0]);
		Check(gone && gone->playerId == INVALID_PLAYER && gone->ident.pos.x == 300.0f,
		      "a joiner is told the package alice found last time is gone");

		rig.ClearInboxes();
		C_CampaignSince since;
		InitHeader(since, 100);
		since.seq = 0;
		second.Send(since, CH_EVENT);
		C_CarLists none;
		InitHeader(none, 100);
		second.Send(none, CH_EVENT);
		rig.Pump(1500, [&] {
			return Last<S_CampaignDelta>(rig.inbox[0]) && Last<S_CarLists>(rig.inbox[0]);
		});
		const S_CampaignDelta *caught = Last<S_CampaignDelta>(rig.inbox[0]);
		Check(caught && caught->body.seq == 1 && caught->body.values[0].offset == 900 &&
		          caught->body.scriptHash == 0x1234ABCDu && caught->ownerId == INVALID_PLAYER &&
		          Count<S_CampaignDelta>(rig.inbox[0]) == 1,
		      "and is sent the mission alice passed, under its main.scm's hash");
		const S_CarLists *told = Last<S_CarLists>(rig.inbox[0]);
		Check(told && told->collected[0] == 0x3 && told->collected[3] == 0x10,
		      "and the car lists as they were");

		C_PickupClaim claim;
		InitHeader(claim, 100);
		claim.ident = package;
		rig.ClearInboxes();
		second.Send(claim, CH_EVENT);
		rig.Pump(1500, [&] { return Last<S_PickupDenied>(rig.inbox[0]) != nullptr; });
		Check(Last<S_PickupDenied>(rig.inbox[0]) != nullptr, "and can't take the package again");

		Check(!rig.server.ForgetProgress(), "progress isn't forgotten while somebody is in");
		second.Disconnect();
		rig.Settle();
		rig.server.Stop();
	}

	{
		Rig rig;
		if (!StartKeeping(rig, path, false)) {
			std::printf("  [skipped] no port to listen on\n");
			return;
		}
		NetClient third;
		rig.clients = {&third};
		rig.inbox.resize(1);
		Join(rig, 0, "carol");
		rig.Settle();
		const S_MissionState *st = Last<S_MissionState>(rig.inbox[0]);
		Check(st && st->campaignLog != firstLog && !Last<S_PickupTaken>(rig.inbox[0]) &&
		          !rig.server.KeepsProgress(),
		      "with keepProgress off the session starts from nothing");
		third.Disconnect();
		rig.Settle();
		rig.server.Stop();
		Check(FileThere(path), "and the file is left alone");

		Check(rig.server.ForgetProgress() && !FileThere(path) && FileThere(path + ".old"),
		      "forgotten with nobody in, the file is set aside as .old");
	}

	{
		Rig rig;
		if (!StartKeeping(rig, path, true)) {
			std::printf("  [skipped] no port to listen on\n");
			return;
		}
		NetClient fourth;
		rig.clients = {&fourth};
		rig.inbox.resize(1);
		Join(rig, 0, "dave");
		rig.Settle();
		const S_MissionState *st = Last<S_MissionState>(rig.inbox[0]);
		Check(st && st->campaignLog != firstLog && !Last<S_PickupTaken>(rig.inbox[0]),
		      "and the next start is a new campaign");
		fourth.Disconnect();
		rig.Settle();
		rig.server.Stop();
	}

	{
		std::remove((path + ".unreadable").c_str());
		if (FILE *fh = std::fopen(path.c_str(), "wb")) {
			std::fputs("not a progress file", fh);
			std::fclose(fh);
		}
		Rig rig;
		if (!StartKeeping(rig, path, true)) {
			std::printf("  [skipped] no port to listen on\n");
			return;
		}
		Check(FileThere(path + ".unreadable") && !FileThere(path) && rig.server.KeepsProgress(),
		      "a file that can't be read is set aside, not written over");
		rig.server.Stop();
	}
	std::remove(path.c_str());
	std::remove((path + ".old").c_str());
	std::remove((path + ".unreadable").c_str());
}

} // namespace

// tools/servertest/emergency.cpp
int RunEmergencyTests();

// The gates' masks and the street objects' records, as the clients hear them.
void TestGatesAndBrokenObjectsReachEverybody() {
	std::printf("\nthe gates and the broken street objects, as the clients hear them\n");
	Rig rig;
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	bool listening = false;
	for (uint16_t port = 24330; port < 24350 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening) {
		std::printf("  [skipped] no port to listen on\n");
		return;
	}
	NetClient first, second, third;
	rig.clients = {&first, &second, &third};
	rig.inbox.resize(3);
	const uint8_t alice = Join(rig, 0, "alice");
	const uint8_t bob   = Join(rig, 1, "bob");
	Check(alice != INVALID_PLAYER && bob != INVALID_PLAYER, "two players in");
	rig.clients[0]->Send(StateAt(100.0f, 200.0f, 10.0f), CH_SNAPSHOT);
	rig.clients[1]->Send(StateAt(110.0f, 200.0f, 10.0f), CH_SNAPSHOT);
	rig.Settle();
	rig.ClearInboxes();

	// Alice's police car at the HQ gate.
	C_GateState gate{};
	InitHeader(gate, 100);
	// Gate 2, the three safehouse doors, and a bit nobody has defined.
	constexpr uint16_t doors =
	    GATE_BIT_HIDEOUT_DOOR | GATE_BIT_STAUNTON_DOOR | GATE_BIT_SHORESIDE_DOOR;
	gate.body.open = static_cast<uint16_t>(0x04 | doors | 0x8000);
	rig.clients[0]->Send(gate, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_GateState>(rig.inbox[1]) != nullptr; });
	const S_GateState *heard = Last<S_GateState>(rig.inbox[1]);
	Check(heard && heard->playerId == alice && heard->body.open == (0x04 | doors),
	      "bob hears alice wants gate 2 and all three safehouse doors open, and nothing else");
	Check(Last<S_GateState>(rig.inbox[0]) == nullptr, "and alice is not told her own");

	// Alice knocks a lamp post down, and it comes to rest.
	ObjectBreakBody lamp{};
	lamp.ident.pos        = {105.0f, 205.0f, 10.0f};
	lamp.ident.modelIndex = 1300;
	lamp.amount           = 500.0f;
	lamp.state            = OBJ_BREAK_SMASHED | OBJ_BREAK_UPROOTED;
	C_ObjectBroken broke{};
	InitHeader(broke, 100);
	broke.body = lamp;
	rig.clients[0]->Send(broke, CH_EVENT);
	C_ObjectSettled settled{};
	InitHeader(settled, 100);
	settled.body.ident = lamp.ident;
	settled.body.right = {1.0f, 0.0f, 0.0f};
	settled.body.forward = {0.0f, 0.0f, 1.0f};
	settled.body.up    = {0.0f, -1.0f, 0.0f};
	settled.body.pos   = {106.0f, 205.0f, 9.5f};
	rig.clients[0]->Send(settled, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_ObjectSettled>(rig.inbox[1]) != nullptr; });
	rig.Settle();
	rig.ClearInboxes();

	// Alice drove off and came back: her engine built it standing.
	C_ObjectRebuilt rebuilt{};
	InitHeader(rebuilt, 100);
	rebuilt.ident = lamp.ident;
	rig.clients[0]->Send(rebuilt, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_ObjectSettled>(rig.inbox[0]) != nullptr; });
	const S_ObjectBroken  *again = Last<S_ObjectBroken>(rig.inbox[0]);
	const S_ObjectSettled *lies  = Last<S_ObjectSettled>(rig.inbox[0]);
	Check(again && again->playerId == INVALID_PLAYER && again->body.state == lamp.state &&
	          again->body.amount == 500.0f,
	      "alice, who broke it, is sent the break again, stamped with nobody so she applies it");
	Check(lies && lies->body.pos.x == 106.0f &&
	          IndexOf(rig.inbox[0], OP_S_OBJECT_BROKEN) < IndexOf(rig.inbox[0], OP_S_OBJECT_SETTLED),
	      "then where it lies, after the break");
	Check(Last<S_ObjectBroken>(rig.inbox[1]) == nullptr, "and nobody else hears the answer");

	// A joiner hears the gate and the lamp post.
	rig.ClearInboxes();
	const uint8_t carol = Join(rig, 2, "carol");
	rig.Settle();
	const S_GateState *told = Last<S_GateState>(rig.inbox[2]);
	Check(carol != INVALID_PLAYER && told && told->playerId == alice &&
	          told->body.open == (0x04 | doors),
	      "carol, joining, is told alice has gate 2 and the doors open");
	Check(Last<S_ObjectBroken>(rig.inbox[2]) != nullptr &&
	          Last<S_ObjectSettled>(rig.inbox[2]) != nullptr,
	      "and handed the lamp post, broken and lying where it fell");

	// Everybody drives off: every copy is a dummy again and the record goes.
	for (size_t i = 0; i < 3; ++i)
		rig.clients[i]->Send(StateAt(900.0f, 200.0f, 10.0f), CH_SNAPSHOT);
	rig.Pump(1600, nullptr);
	rig.ClearInboxes();
	rig.clients[0]->Send(rebuilt, CH_EVENT);
	rig.Pump(500, nullptr);
	Check(Last<S_ObjectBroken>(rig.inbox[0]) == nullptr,
	      "once nobody is near it, a rebuild is told nothing and it stands");

	for (NetClient *client : rig.clients)
		client->Disconnect();
	rig.Settle();
	rig.server.Stop();
}

// The host changing the rules while two players are in: what the window's Save
// does, through Server::Configure.
void TestTheHostsRulesReachEverybody() {
	std::printf("\nthe host's rules, at the join and when they change\n");
	Rig rig;
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	bool listening = false;
	for (uint16_t port = 24250; port < 24270 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening) {
		std::printf("  [skipped] no port to listen on\n");
		return;
	}
	ServerConfig cfg;
	rig.server.Configure(cfg);

	NetClient first, second, third;
	rig.clients = {&first, &second, &third};
	rig.inbox.resize(3);
	const uint8_t alice = Join(rig, 0, "alice");
	rig.Pump(1000, [&] { return Last<S_SessionRules>(rig.inbox[0]) != nullptr; });
	const S_Welcome      *welcome = Last<S_Welcome>(rig.inbox[0]);
	const S_SessionRules *rules   = Last<S_SessionRules>(rig.inbox[0]);
	Check(welcome && rules && IndexOf(rig.inbox[0], OP_S_SESSION_RULES) >
	                              IndexOf(rig.inbox[0], OP_S_WELCOME),
	      "the rules follow the welcome");
	Check(welcome && rules && rules->flags == welcome->flags && rules->maxWanted == 6,
	      "with the welcome's flags and six stars");
	const uint8_t bob = Join(rig, 1, "bob");
	Check(alice == 0 && bob == 1, "alice and bob are in");
	rig.Pump(1000, [&] {
		return Last<S_ParkedSeed>(rig.inbox[0]) && Last<S_ParkedSeed>(rig.inbox[1]);
	});
	const S_ParkedSeed *seedA = Last<S_ParkedSeed>(rig.inbox[0]);
	const S_ParkedSeed *seedB = Last<S_ParkedSeed>(rig.inbox[1]);
	Check(seedA && IndexOf(rig.inbox[0], OP_S_PARKED_SEED) > IndexOf(rig.inbox[0], OP_S_SESSION_RULES),
	      "the parked cars' seed follows the rules");
	Check(seedA && seedB && seedA->seed == seedB->seed && seedA->seed == rig.server.ParkedSeed(),
	      "and it is one number for everybody, the server's");

	// The host changes several things at once.
	cfg.friendlyFire            = true;
	cfg.wantedLevel             = WantedLevelRule::Shared;
	cfg.maxWanted               = 3;
	cfg.missionCheckpointWaitS  = 30;
	cfg.missionTimedCheckpoints = true;
	cfg.missionCatchUpM         = 0;
	cfg.money                   = MoneyMode::Shared;
	cfg.maxPlayers              = 2;
	rig.ClearInboxes();
	rig.server.Configure(cfg);
	rig.Pump(1500, [&] {
		return Last<S_SessionRules>(rig.inbox[1]) && Last<S_MissionState>(rig.inbox[1]) &&
		       Last<S_Money>(rig.inbox[1]);
	});
	rules = Last<S_SessionRules>(rig.inbox[1]);
	Check(rules && (rules->flags & SESSION_FRIENDLY_FIRE) != 0 &&
	          WantedRuleFromFlags(rules->flags) == WANTED_RULE_SHARED && rules->maxWanted == 3,
	      "bob, already in, is told friendly fire is on, stars are shared and stop at three");
	const S_MissionState *st = Last<S_MissionState>(rig.inbox[1]);
	Check(st && st->checkpointWaitS == 30 && st->catchUpM == 0 &&
	          (st->flags & MISSION_FLAG_TIMED_CHECKPOINTS) != 0,
	      "and the mission rules, with no mission running");
	const S_Money *money = Last<S_Money>(rig.inbox[1]);
	Check(money && money->rule == MONEY_RULE_SHARED, "and that the session has one wallet now");

	// A limit of two with two in: the third is told it is full.
	NetClient *c = rig.clients[2];
	c->Connect("127.0.0.1", rig.port);
	rig.Pump(3000, [&] { return c->IsConnected(); });
	c->Send(Hello("carol"), CH_EVENT);
	rig.Pump(3000, [&] { return Last<S_Welcome>(rig.inbox[2]) != nullptr; });
	const S_Welcome *full = Last<S_Welcome>(rig.inbox[2]);
	Check(full && full->reject == REJECT_FULL, "carol, third under a limit of two, is turned away");

	// The same rules again tell nobody anything.
	rig.ClearInboxes();
	rig.server.Configure(cfg);
	rig.Settle(250);
	Check(Count<S_SessionRules>(rig.inbox[1]) == 0 && Count<S_MissionState>(rig.inbox[1]) == 0 &&
	          Count<S_Money>(rig.inbox[1]) == 0,
	      "saving without a change sends nothing");

	// Only the owner is paid: the server keeps a mission's reward from the
	// helpers, and passes everything else on.
	cfg.missionPayHelpers = false;
	rig.server.Configure(cfg);
	C_MissionStarted started;
	InitHeader(started, 100);
	started.launchKey     = 0x4242;
	started.missionNumber = 19;
	rig.clients[0]->Send(started, CH_EVENT);
	rig.Pump(1500, [&] {
		const S_MissionState *s = Last<S_MissionState>(rig.inbox[1]);
		return s && s->state == MISSION_STATE_RUNNING;
	});
	C_MissionEffect pay;
	InitHeader(pay, 100);
	pay.body.missionNumber = 19;
	pay.body.kind          = MISSION_EFFECT_PAY;
	pay.body.length        = 12;
	C_MissionEffect text = pay;
	text.body.kind       = MISSION_EFFECT_RUN;
	rig.ClearInboxes();
	rig.clients[0]->Send(pay, CH_EVENT);
	rig.clients[0]->Send(text, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionEffect>(rig.inbox[1]) != nullptr; });
	rig.Settle(150);
	const S_MissionEffect *got = Last<S_MissionEffect>(rig.inbox[1]);
	Check(Count<S_MissionEffect>(rig.inbox[1]) == 1 && got && got->body.kind == MISSION_EFFECT_RUN,
	      "with helpers unpaid, bob gets the mission's words and not its reward");
	cfg.missionPayHelpers = true;
	rig.server.Configure(cfg);
	rig.ClearInboxes();
	rig.clients[0]->Send(pay, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_MissionEffect>(rig.inbox[1]) != nullptr; });
	got = Last<S_MissionEffect>(rig.inbox[1]);
	Check(got && got->body.kind == MISSION_EFFECT_PAY, "and paid again once the host says so");

	// A rampage rule changed while a rampage vote is open waits for it.
	C_PickupClaim skull{};
	InitHeader(skull, 100);
	skull.ident.pos        = {958.0f, -431.0f, 14.5f};
	skull.ident.modelIndex = 1361;
	skull.ident.type       = 3;
	skull.ident.flags      = PICKUP_F_RAMPAGE;
	rig.ClearInboxes();
	rig.clients[0]->Send(skull, CH_EVENT);
	rig.Pump(1500, [&] { return Last<S_RampageVote>(rig.inbox[1]) != nullptr; });
	const S_RampageVote *vote = Last<S_RampageVote>(rig.inbox[1]);
	Check(vote && vote->body.state == RAMPAGE_VOTE_OPEN, "(alice's skull opens a vote)");
	// The id is kept, not the pointer. The clear below frees the message it
	// points into, and anything bob is sent during the settle (the 1 Hz world
	// state) can be put where it was, so bob's no could name a vote that was
	// never open and be dropped.
	const bool    voteOpen = vote && vote->body.state == RAMPAGE_VOTE_OPEN;
	const uint8_t voteId   = vote ? vote->body.voteId : 0;
	cfg.rampage = RampageMode::Off;
	rig.ClearInboxes();
	rig.server.Configure(cfg);
	rig.Settle(200);
	Check(rig.server.RampageRulePending() && Count<S_SessionRules>(rig.inbox[1]) == 0,
	      "rampages off while the vote runs: held back, and nobody told yet");
	if (voteOpen) {
		C_RampageVote no;
		InitHeader(no, 100);
		no.voteId = voteId;
		no.yes    = 0;
		rig.clients[1]->Send(no, CH_EVENT);
	}
	rig.Pump(1500, [&] { return Last<S_SessionRules>(rig.inbox[1]) != nullptr; });
	rules = Last<S_SessionRules>(rig.inbox[1]);
	Check(!rig.server.RampageRulePending() && rules &&
	          RampageRuleFromFlags(rules->flags) == RAMPAGE_RULE_OFF,
	      "bob says no, the vote is over, and then everybody hears rampages are off");

	for (NetClient *client : rig.clients)
		client->Disconnect();
	rig.Settle(300);
	rig.server.Stop();
}

int main(int argc, char **argv) {
	g_verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
	Rig rig;
	rig.server.SetLogSink([](LogKind, const char *line) {
		if (g_verbose)
			std::printf("    | %s\n", line);
	});
	bool listening = false;
	for (uint16_t port = 24170; port < 24190 && !listening; ++port) {
		listening = rig.server.Start(port, false);
		if (listening)
			rig.port = port;
	}
	if (!listening) {
		std::printf("servertest: skipped, no port to listen on\n");
		return 0;
	}
	rig.server.SetMissionRules(true, MISSION_MARGIN_CM_DEFAULT);

	NetClient a, b, c, d;
	rig.clients = {&a, &b, &c, &d};
	rig.inbox.resize(4);

	TestTheSessionsMission(rig);
	TestTheHostKicks(rig);
	TestTheLobby(rig);
	TestACarsBombTravels(rig);
	TestACarsRadioTravels(rig);
	TestAMineBlastTravels(rig);
	TestAMissionsBombTravels(rig);
	TestAWreckIsSettledAndHandedToAJoiner(rig);
	TestACarsAlarmAndGunTravel(rig);
	TestACarsPaintTravels(rig);
	TestAJumpShotReachesTheRiders(rig);
	TestATrafficCarLetGoIsHandedOn(rig);
	TestPedestriansLetGoAreHandedOn(rig);
	TestPoliceAndSpeechTravel(rig);
	TestTheHostsContactsTravel(rig);
	TestTheHostsPlacesTravel(rig);
	TestSavedPackagesReachEverybody(rig);
	TestASeatIsLetGoOfWhenTheSnapshotsSayOnFoot(rig);

	for (NetClient *client : rig.clients)
		if (client)
			client->Disconnect();
	rig.Settle();
	rig.server.Stop();

	TestTwoNewGamesAtTheBridge();
	TestTheSkyFollowsTheMission();
	TestAFailedMissionIsClearedForTheRetry();
	TestACutsceneIsSkippedTogether();
	TestAnOwnerWhoDroppedOffIsTakenUpAgain();
	TestGatesAndBrokenObjectsReachEverybody();
	TestTheHostsRulesReachEverybody();
	TestProgressOutlivesTheServer();
	TestARampagePassNamesItsPayer();
	TestNoPacketCrashesTheServer();
	g_failures += RunEmergencyTests();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
	            g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
