// Headless protocol test. Drives real NetClients against a real server.exe
// over loopback and checks the handshake, relay and chat flows.
//
// The client DLL can't be tested without GTA III running, but the protocol
// itself can. Run this after touching protocol.h or the server.
//
//   xmake build nettest && xmake run nettest
//
// Expects a server already listening on DEFAULT_PORT.

#include "coopiii/net.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace coopiii;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

// Pumps every client for up to timeoutMs, collecting whatever arrives. Stops
// early once `done` returns true.
template <class Fn>
void PumpUntil(std::vector<NetClient *> clients,
               std::vector<std::vector<Message>> &inboxes, uint32_t timeoutMs,
               Fn done) {
	using namespace std::chrono;
	const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
	while (steady_clock::now() < deadline) {
		for (size_t i = 0; i < clients.size(); ++i)
			clients[i]->Service(inboxes[i]);
		if (done())
			return;
		std::this_thread::sleep_for(milliseconds(2));
	}
}

const Message *FindOp(const std::vector<Message> &inbox, uint8_t opcode) {
	for (const Message &m : inbox)
		if (m.opcode == opcode)
			return &m;
	return nullptr;
}

size_t CountOp(const std::vector<Message> &inbox, uint8_t opcode) {
	size_t n = 0;
	for (const Message &m : inbox)
		if (m.opcode == opcode)
			++n;
	return n;
}

C_Hello MakeHello(const char *nick) {
	C_Hello hello;
	InitHeader(hello, 0);
	hello.protocolVersion = PROTOCOL_VERSION;
	hello.modelId         = 0;   // MI_PLAYER
	std::strncpy(hello.nick, nick, NICK_LEN - 1);
	return hello;
}

} // namespace

int main(int argc, char **argv) {
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";

	if (!NetInit()) {
		std::printf("enet init failed\n");
		return 1;
	}

	NetClient a, b;
	std::vector<NetClient *> both = {&a, &b};
	std::vector<std::vector<Message>> inbox(2);

	std::printf("connecting to %s:%u\n", host, DEFAULT_PORT);
	if (!a.Connect(host, DEFAULT_PORT) || !b.Connect(host, DEFAULT_PORT)) {
		std::printf("connect failed (is server.exe running?)\n");
		NetDeinit();
		return 1;
	}

	PumpUntil(both, inbox, 3000,
	          [&] { return a.IsConnected() && b.IsConnected(); });

	std::printf("\nhandshake\n");
	Check(a.IsConnected() && b.IsConnected(), "both peers connected");
	if (!a.IsConnected() || !b.IsConnected()) {
		NetDeinit();
		return 1;
	}

	// --- hello / welcome ---------------------------------------------------
	a.Send(MakeHello("alice"), CH_EVENT);
	PumpUntil(both, inbox, 2000,
	          [&] { return FindOp(inbox[0], OP_S_WELCOME) != nullptr; });

	const Message *welcomeMsg = FindOp(inbox[0], OP_S_WELCOME);
	Check(welcomeMsg != nullptr, "alice got S_WELCOME");

	uint8_t aliceId = INVALID_PLAYER;
	if (welcomeMsg) {
		const auto *w = welcomeMsg->as<S_Welcome>();
		Check(w != nullptr, "welcome has the expected size");
		if (w) {
			Check(w->reject == REJECT_NONE, "not rejected");
			Check(w->snapshotHz == SNAPSHOT_HZ, "server reports 25 Hz");
			Check(w->netId != INVALID_NETID, "got a netId");
			aliceId = w->playerId;
		}
	}

	// --- second player: both directions of the join fan-out ---------------
	inbox[0].clear();
	b.Send(MakeHello("bob"), CH_EVENT);
	PumpUntil(both, inbox, 2000, [&] {
		return FindOp(inbox[1], OP_S_WELCOME) != nullptr &&
		       FindOp(inbox[0], OP_S_PLAYER_JOIN) != nullptr &&
		       FindOp(inbox[1], OP_S_PLAYER_JOIN) != nullptr;
	});

	std::printf("\njoin fan-out\n");
	const Message *bobWelcome = FindOp(inbox[1], OP_S_WELCOME);
	Check(bobWelcome != nullptr, "bob got S_WELCOME");

	uint8_t bobId = INVALID_PLAYER;
	if (bobWelcome)
		if (const auto *w = bobWelcome->as<S_Welcome>())
			bobId = w->playerId;

	Check(aliceId != bobId, "distinct player slots assigned");

	const Message *aliceSawBob = FindOp(inbox[0], OP_S_PLAYER_JOIN);
	Check(aliceSawBob != nullptr, "alice notified of bob joining");
	if (aliceSawBob)
		if (const auto *j = aliceSawBob->as<S_PlayerJoin>())
			Check(std::strcmp(j->nick, "bob") == 0, "join carries bob's nick");

	const Message *bobSawAlice = FindOp(inbox[1], OP_S_PLAYER_JOIN);
	Check(bobSawAlice != nullptr, "bob told about already-present alice");
	if (bobSawAlice)
		if (const auto *j = bobSawAlice->as<S_PlayerJoin>())
			Check(std::strcmp(j->nick, "alice") == 0, "backfill carries alice's nick");

	// --- snapshot relay ----------------------------------------------------
	inbox[0].clear();
	inbox[1].clear();

	C_PlayerState snap{};
	InitHeader(snap, 1234);
	snap.body.pos       = {100.5f, -200.25f, 10.0f};
	snap.body.heading   = 1.5f;
	snap.body.moveState = 2;   // PEDMOVE_WALK
	snap.body.health    = 100.0f;
	// Animation, weapon and aim block (protocol 2). All values distinct on
	// purpose - the struct gets byte-copied onto the wire, so a field at the
	// wrong offset just shows up as another field's value.
	snap.body.animId    = 1;        // ANIM_STD_RUN
	snap.body.animTime  = 0.375f;
	snap.body.animSpeed = 1.25f;
	snap.body.animId2   = 0x0101;   // a partial overlay
	snap.body.animTime2 = 0.125f;
	snap.body.armour    = 50.0f;
	snap.body.weapon    = 5;        // WEAPONTYPE_AK47
	snap.body.aimYaw    = -0.75f;
	snap.body.aimPitch  = 0.25f;
	snap.body.flags     = PF_AIMING | PF_FIRING;
	a.Send(snap, CH_SNAPSHOT);

	PumpUntil(both, inbox, 2000,
	          [&] { return FindOp(inbox[1], OP_S_PLAYER_STATE) != nullptr; });

	std::printf("\nsnapshot relay\n");
	const Message *relayed = FindOp(inbox[1], OP_S_PLAYER_STATE);
	Check(relayed != nullptr, "bob received alice's snapshot");
	if (relayed) {
		const auto *s = relayed->as<S_PlayerState>();
		Check(s != nullptr, "snapshot has the expected size");
		if (s) {
			Check(s->playerId == aliceId, "tagged with alice's id");
			Check(s->body.pos.x == 100.5f && s->body.pos.y == -200.25f,
			      "position survived the round trip");
			Check(s->body.heading == 1.5f, "heading survived");
			Check(s->hdr.sendTimeMs == 1234, "sender game time preserved");
			Check(s->body.animId == 1 && s->body.animTime == 0.375f &&
			          s->body.animSpeed == 1.25f,
			      "base animation survived");
			Check(s->body.animId2 == 0x0101 && s->body.animTime2 == 0.125f,
			      "partial animation survived");
			Check(s->body.health == 100.0f && s->body.armour == 50.0f,
			      "vitals are still either side of the animation block");
			Check(s->body.weapon == 5, "weapon survived");
			Check(s->body.aimYaw == -0.75f && s->body.aimPitch == 0.25f,
			      "aim survived");
			Check(s->body.flags == (PF_AIMING | PF_FIRING), "flags survived");
		}
	}
	Check(CountOp(inbox[0], OP_S_PLAYER_STATE) == 0,
	      "sender does not receive its own snapshot");

	// --- chat --------------------------------------------------------------
	inbox[0].clear();
	inbox[1].clear();

	C_Chat chat;
	InitHeader(chat, 0);
	std::strncpy(chat.text, "hola liberty city", CHAT_LEN - 1);
	b.Send(chat, CH_EVENT);

	PumpUntil(both, inbox, 2000,
	          [&] { return FindOp(inbox[0], OP_S_CHAT) != nullptr; });

	std::printf("\nchat\n");
	const Message *chatMsg = FindOp(inbox[0], OP_S_CHAT);
	Check(chatMsg != nullptr, "alice received bob's chat");
	if (chatMsg)
		if (const auto *c = chatMsg->as<S_Chat>()) {
			Check(c->playerId == bobId, "attributed to bob");
			Check(std::strcmp(c->text, "hola liberty city") == 0, "text intact");
		}
	Check(FindOp(inbox[1], OP_S_CHAT) != nullptr, "chat echoes to sender too");

	// --- world state -------------------------------------------------------
	inbox[0].clear();
	PumpUntil(both, inbox, 2000,
	          [&] { return FindOp(inbox[0], OP_S_WORLD_STATE) != nullptr; });

	std::printf("\nworld state\n");
	const Message *world = FindOp(inbox[0], OP_S_WORLD_STATE);
	Check(world != nullptr, "world state arrives at ~1 Hz");
	if (world)
		if (const auto *w = world->as<S_WorldState>()) {
			Check(w->body.hour < 24 && w->body.minute < 60, "clock in range");
			// Neither of these two ever sent a C_WorldState, so this is the
			// server's own stand-in clock, and the host it names is alice.
			Check(w->hostPlayerId == 0, "the first player in is the host");
		}

	// --- leave -------------------------------------------------------------
	inbox[0].clear();
	b.Disconnect();
	PumpUntil({&a}, inbox, 3000,
	          [&] { return FindOp(inbox[0], OP_S_PLAYER_LEAVE) != nullptr; });

	std::printf("\nleave\n");
	const Message *leave = FindOp(inbox[0], OP_S_PLAYER_LEAVE);
	Check(leave != nullptr, "alice notified of bob leaving");
	if (leave)
		if (const auto *l = leave->as<S_PlayerLeave>())
			Check(l->playerId == bobId, "leave names bob's slot");

	a.Disconnect();
	NetDeinit();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
