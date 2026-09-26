// Medics and fire trucks through the real server (docs/protocol.md 1.36):
// who hears a revive, and who hears a jet.

#include "server.h"

#include "coopiii/net.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

using namespace coopiii;

namespace {

int g_emFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_emFailures;
}

struct Peer {
	NetClient            net;
	std::vector<Message> box;
};

template <class T>
const T *Newest(const std::vector<Message> &box) {
	for (auto it = box.rbegin(); it != box.rend(); ++it)
		if (const T *p = it->as<T>())
			return p;
	return nullptr;
}

template <class T>
size_t CountOf(const std::vector<Message> &box) {
	size_t n = 0;
	for (const Message &m : box)
		if (m.as<T>())
			++n;
	return n;
}

C_Hello HelloFrom(const char *nick) {
	C_Hello hello;
	InitHeader(hello, 0);
	hello.protocolVersion = PROTOCOL_VERSION;
	std::strncpy(hello.nick, nick, NICK_LEN - 1);
	return hello;
}

} // namespace

int RunEmergencyTests() {
	g_emFailures = 0;
	std::printf("\na medic's revive and a fire truck's jet, through the server\n");

	Server   server;
	server.SetLogSink([](LogKind, const char *) {});
	uint16_t port      = 0;
	bool     listening = false;
	for (uint16_t p = 24250; p < 24270 && !listening; ++p) {
		listening = server.Start(p, false);
		if (listening)
			port = p;
	}
	if (!listening) {
		std::printf("  [skipped] no port to listen on\n");
		return 0;
	}

	Peer              alice, bob, carol;
	std::vector<Peer *> peers = {&alice, &bob, &carol};
	const auto pump = [&](uint32_t ms, const std::function<bool()> &done) {
		using namespace std::chrono;
		const auto until = steady_clock::now() + milliseconds(ms);
		while (steady_clock::now() < until) {
			server.Tick(0);
			for (Peer *p : peers)
				p->net.Service(p->box);
			if (done && done())
				return;
			std::this_thread::sleep_for(milliseconds(1));
		}
	};
	const auto clear = [&] {
		for (Peer *p : peers)
			p->box.clear();
	};

	for (Peer *p : peers)
		p->net.Connect("127.0.0.1", port);
	pump(3000, [&] {
		return alice.net.IsConnected() && bob.net.IsConnected() && carol.net.IsConnected();
	});
	const char *nicks[] = {"alice", "bob", "carol"};
	for (size_t i = 0; i < peers.size(); ++i) {
		peers[i]->net.Send(HelloFrom(nicks[i]), CH_EVENT);
		pump(3000, [&] { return Newest<S_Welcome>(peers[i]->box) != nullptr; });
	}
	const S_Welcome *bw   = Newest<S_Welcome>(bob.box);
	const uint8_t    bobId = bw ? bw->playerId : INVALID_PLAYER;

	// ---- the revive ----
	C_PedSpawn spawn{};
	InitHeader(spawn, 100);
	spawn.tempId       = 1;
	spawn.body.modelId = 7;
	spawn.body.pos     = {5.0f, 5.0f, 1.0f};
	alice.net.Send(spawn, CH_EVENT);
	pump(1500, [&] { return Newest<S_PedSpawn>(carol.box) != nullptr; });
	const S_PedSpawn *named = Newest<S_PedSpawn>(alice.box);
	const uint16_t    ped   = named ? named->netId : INVALID_NETID;
	Check(ped != INVALID_NETID, "(alice's engine made a pedestrian and the session named him)");

	clear();
	C_PedRevive revive{};
	InitHeader(revive, 200);
	revive.body.netId = ped;
	bob.net.Send(revive, CH_EVENT);
	pump(400, nullptr);
	Check(!Newest<S_PedRevive>(alice.box) && !Newest<S_PedRevive>(carol.box),
	      "a revive of a pedestrian who is alive goes nowhere");

	C_PedDeath death{};
	InitHeader(death, 300);
	death.body.netId  = ped;
	death.body.animId = 13;
	alice.net.Send(death, CH_EVENT);
	pump(1500, [&] { return Newest<S_PedDeath>(carol.box) != nullptr; });
	clear();
	bob.net.Send(revive, CH_EVENT);
	pump(1500, [&] {
		return Newest<S_PedRevive>(alice.box) != nullptr &&
		       Newest<S_PedRevive>(carol.box) != nullptr;
	});
	const S_PedRevive *toHost = Newest<S_PedRevive>(alice.box);
	Check(toHost && toHost->body.netId == ped && toHost->playerId == bobId,
	      "bob's medic stood alice's pedestrian up, and alice's machine hears it from him");
	Check(Newest<S_PedRevive>(carol.box) && !Newest<S_PedRevive>(bob.box),
	      "carol does too, and bob, whose engine already did it, does not");

	clear();
	alice.net.Send(revive, CH_EVENT);
	pump(400, nullptr);
	Check(CountOf<S_PedRevive>(bob.box) == 0 && CountOf<S_PedRevive>(carol.box) == 0,
	      "a second revive of the same life is dropped");

	// ---- the jet ----
	C_CarSpawn truck{};
	InitHeader(truck, 400);
	truck.tempId       = 2;
	truck.body.modelId = 97;
	truck.body.pos     = {20.0f, 20.0f, 1.0f};
	truck.body.rot     = {0.0f, 0.0f, 0.0f, 1.0f};
	alice.net.Send(truck, CH_EVENT);
	pump(1500, [&] { return Newest<S_CarSpawn>(carol.box) != nullptr; });
	const S_CarSpawn *car   = Newest<S_CarSpawn>(alice.box);
	const uint16_t    netId = car ? car->netId : INVALID_NETID;
	Check(netId != INVALID_NETID, "(alice's engine made a fire truck and the session named it)");

	clear();
	C_WaterCannon jet{};
	InitHeader(jet, 500);
	jet.body.netId = netId;
	jet.body.pos   = {0.0f, 0.0f, 2.2f};
	jet.body.dir   = {0.0f, 0.9f, 0.3f};
	alice.net.Send(jet, CH_SNAPSHOT);
	pump(1500, [&] {
		return Newest<S_WaterCannon>(bob.box) != nullptr &&
		       Newest<S_WaterCannon>(carol.box) != nullptr;
	});
	const S_WaterCannon *heard = Newest<S_WaterCannon>(bob.box);
	Check(heard && heard->body.netId == netId && heard->body.pos.z == 2.2f &&
	          heard->body.dir.y == 0.9f && Newest<S_WaterCannon>(carol.box) &&
	          !Newest<S_WaterCannon>(alice.box),
	      "the jet of the truck alice hosts reaches bob and carol as it left her, not back to her");

	clear();
	bob.net.Send(jet, CH_SNAPSHOT);
	pump(400, nullptr);
	Check(!Newest<S_WaterCannon>(alice.box) && !Newest<S_WaterCannon>(carol.box),
	      "bob, who does not aim it, sprays nothing anywhere");

	// ---- the Import/Export and crane lists (docs/missions.md 6.1) ----
	std::printf("\nthe Import/Export and crane lists, through the server\n");
	clear();
	C_CarLists ask{};
	InitHeader(ask, 600);
	carol.net.Send(ask, CH_EVENT);
	pump(1500, [&] { return Newest<S_CarLists>(carol.box) != nullptr; });
	const S_CarLists *empty = Newest<S_CarLists>(carol.box);
	Check(empty && empty->collected[0] == 0 && !Newest<S_CarLists>(alice.box),
	      "carol asks, and hears the session has nothing yet; nobody else is told");

	clear();
	C_CarLists brought{};
	InitHeader(brought, 700);
	brought.collected[0]              = 0x0003;
	brought.collected[CAR_LIST_CRANE] = 0x01;
	bob.net.Send(brought, CH_EVENT);
	pump(1500, [&] {
		return Newest<S_CarLists>(alice.box) && Newest<S_CarLists>(bob.box) &&
		       Newest<S_CarLists>(carol.box);
	});
	const S_CarLists *toAlice = Newest<S_CarLists>(alice.box);
	Check(toAlice && toAlice->collected[0] == 0x0003 && toAlice->collected[CAR_LIST_CRANE] == 0x01 &&
	          Newest<S_CarLists>(bob.box) && Newest<S_CarLists>(carol.box),
	      "bob's two cars and the fire truck are everybody's, and bob hears it back");

	clear();
	C_CarLists another{};
	InitHeader(another, 800);
	another.collected[0] = 0x0004;
	alice.net.Send(another, CH_EVENT);
	pump(1500, [&] { return Newest<S_CarLists>(carol.box) != nullptr; });
	const S_CarLists *union3 = Newest<S_CarLists>(carol.box);
	Check(union3 && union3->collected[0] == 0x0007 && union3->collected[CAR_LIST_CRANE] == 0x01,
	      "alice's car is put with them, and nothing is taken off");

	clear();
	bob.net.Send(brought, CH_EVENT);
	pump(400, nullptr);
	Check(Newest<S_CarLists>(bob.box) && !Newest<S_CarLists>(alice.box) && !Newest<S_CarLists>(carol.box),
	      "what the session has already goes back to the sender alone");

	for (Peer *p : peers)
		p->net.Disconnect();
	pump(300, nullptr);
	server.Stop();
	return g_emFailures;
}
