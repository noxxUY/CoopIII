// Headless protocol test. Drives real NetClients against a real server.exe
// over loopback and checks the handshake, relay and chat flows.
//
// The client DLL can't be tested without GTA III running, but the protocol
// itself can. Run this after touching protocol.h or the server.
//
//   xmake build nettest && xmake run nettest
//
// Expects a server already listening on DEFAULT_PORT, or on the port given as
// the second argument (`nettest 127.0.0.1 2005`).

#include "coopiii/net.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>
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

// Where in the inbox an opcode first turns up, or the inbox size if it never
// does. Used to check that the backfill's three groups arrive in an order the
// receiver can actually act on - a seat is useless before its car exists.
size_t IndexOfOp(const std::vector<Message> &inbox, uint8_t opcode) {
	for (size_t i = 0; i < inbox.size(); ++i)
		if (inbox[i].opcode == opcode)
			return i;
	return inbox.size();
}

// The S_ENTER_VEHICLE in `inbox` that names `playerId`, if there is one. The
// backfill sends one per occupant, so finding the right one matters.
const S_EnterVehicle *FindSeatFor(const std::vector<Message> &inbox, uint8_t playerId) {
	for (const Message &m : inbox)
		if (m.opcode == OP_S_ENTER_VEHICLE)
			if (const auto *e = m.as<S_EnterVehicle>())
				if (e->playerId == playerId)
					return e;
	return nullptr;
}

const S_PlayerJoin *FindJoinFor(const std::vector<Message> &inbox, uint8_t playerId) {
	for (const Message &m : inbox)
		if (m.opcode == OP_S_PLAYER_JOIN)
			if (const auto *j = m.as<S_PlayerJoin>())
				if (j->playerId == playerId)
					return j;
	return nullptr;
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
	// Optional, and only because a second session on this machine may already
	// be holding DEFAULT_PORT with a server built from a different tree -
	// AGENTS.md says use another port rather than kill it, and until now
	// there was no way to.
	const uint16_t port =
	    argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : DEFAULT_PORT;

	if (!NetInit()) {
		std::printf("enet init failed\n");
		return 1;
	}

	// Four, not two. The last two arrive late on purpose: a backfill is only
	// a backfill if there was already a session to be behind, so the only way
	// to test one is to build a session first and then connect into it.
	NetClient a, b, c, d;
	std::vector<NetClient *> both = {&a, &b};
	std::vector<NetClient *> all  = {&a, &b, &c, &d};
	std::vector<std::vector<Message>> inbox(4);

	std::printf("connecting to %s:%u\n", host, port);
	if (!a.Connect(host, port) || !b.Connect(host, port)) {
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

	uint8_t aliceId     = INVALID_PLAYER;
	// Copied out now, not read off `welcomeMsg` later: the inbox is cleared
	// several times below and that pointer is into its storage.
	uint8_t sessionFlags = 0;
	if (welcomeMsg) {
		const auto *w = welcomeMsg->as<S_Welcome>();
		Check(w != nullptr, "welcome has the expected size");
		if (w) {
			Check(w->reject == REJECT_NONE, "not rejected");
			Check(w->snapshotHz == SNAPSHOT_HZ, "server reports 25 Hz");
			Check(w->netId != INVALID_NETID, "got a netId");
			aliceId      = w->playerId;
			sessionFlags = w->flags;
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

	// --- a limb off a pedestrian (protocol 17) -----------------------------
	//
	// Alice's engine makes a pedestrian and then takes its head off. Bob has
	// to hear about the head; alice, whose engine did it, must not; and the
	// two things the server refuses - a limb off somebody else's ped, and a
	// node that is not a limb - must reach nobody.
	inbox[0].clear();
	inbox[1].clear();

	C_PedSpawn born{};
	InitHeader(born, 3000);
	born.tempId       = 77;
	born.body.modelId = 7;
	born.body.pedType = 4;   // PEDTYPE_CIVMALE
	born.body.pos     = {5.0f, 6.0f, 7.0f};
	a.Send(born, CH_EVENT);
	PumpUntil(both, inbox, 2000,
	          [&] { return FindOp(inbox[0], OP_S_PED_SPAWN) != nullptr; });

	uint16_t pedNetId = INVALID_NETID;
	if (const Message *m = FindOp(inbox[0], OP_S_PED_SPAWN))
		if (const auto *s = m->as<S_PedSpawn>())
			pedNetId = s->netId;

	std::printf("\na limb off a pedestrian\n");
	Check(pedNetId != INVALID_NETID, "alice's pedestrian got a netId");

	inbox[0].clear();
	inbox[1].clear();

	C_PedBodyPart head{};
	InitHeader(head, 3100);
	head.body.netId     = pedNetId;
	head.body.node      = 2;   // PED_HEAD
	head.body.direction = 3;
	a.Send(head, CH_EVENT);

	C_PedBodyPart notYours = head;
	notYours.body.node = 3;
	b.Send(notYours, CH_EVENT);

	C_PedBodyPart torso = head;
	torso.body.node = 0;
	a.Send(torso, CH_EVENT);

	// The refusals are silence, so give them time to not arrive.
	PumpUntil(both, inbox, 500, [&] { return false; });

	Check(CountOp(inbox[1], OP_S_PED_BODY_PART) == 1, "bob hears about exactly one limb");
	if (const Message *m = FindOp(inbox[1], OP_S_PED_BODY_PART))
		if (const auto *s = m->as<S_PedBodyPart>())
			Check(s->body.netId == pedNetId && s->body.node == 2 &&
			          s->body.direction == 3,
			      "the head, of alice's pedestrian, from the side it was hit");
	Check(CountOp(inbox[0], OP_S_PED_BODY_PART) == 0,
	      "alice is not told about her own limb, or about bob's attempt on it");

	// --- and the pedestrian dying ------------------------------------------
	//
	// Same three questions as the limb, plus the one a limb never raised:
	// this one the server *keeps*, so a second death for the same life is a
	// duplicate and has to be refused rather than relayed.
	std::printf("\na pedestrian dying\n");
	inbox[0].clear();
	inbox[1].clear();

	C_PedDeath pedDied{};
	InitHeader(pedDied, 3200);
	pedDied.body.netId  = pedNetId;
	pedDied.body.animId = 17;
	a.Send(pedDied, CH_EVENT);

	C_PedDeath notYoursEither = pedDied;
	notYoursEither.body.animId = 20;
	b.Send(notYoursEither, CH_EVENT);   // bob does not host her pedestrian

	C_PedDeath stranger{};
	InitHeader(stranger, 3210);
	stranger.body.netId  = 0xBEEF;
	stranger.body.animId = 13;
	a.Send(stranger, CH_EVENT);         // a ped the session never named

	C_PedDeath again = pedDied;
	a.Send(again, CH_EVENT);            // and the same death a second time

	PumpUntil(both, inbox, 500, [&] { return false; });

	Check(CountOp(inbox[1], OP_S_PED_DEATH) == 1,
	      "bob hears about exactly one death");
	if (const Message *m = FindOp(inbox[1], OP_S_PED_DEATH))
		if (const auto *s = m->as<S_PedDeath>())
			Check(s->body.netId == pedNetId && s->body.animId == 17,
			      "alice's pedestrian, in the animation her engine chose");
	Check(CountOp(inbox[0], OP_S_PED_DEATH) == 0,
	      "alice is not told about a death her own engine decided");

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

	// --- the backfill ------------------------------------------------------
	//
	// docs/protocol.md §2.8. Everything above proves a packet survives the
	// wire; this proves the session *remembers* enough of what went over it
	// to hand somebody the same world twenty minutes later.
	//
	// Build a session with something in it - a player on 37 health holding an
	// AK, a car that has been driven across town and shot at, a driver and a
	// passenger in it - then connect into it and read what arrives.
	inbox[0].clear();
	inbox[1].clear();

	C_PlayerState hurt{};
	InitHeader(hurt, 2000);
	hurt.body.pos     = {123.0f, 45.0f, 6.0f};
	hurt.body.heading = 0.5f;
	hurt.body.health  = 37.0f;
	hurt.body.armour  = 50.0f;
	hurt.body.weapon  = 6;   // WEAPONTYPE_M16
	a.Send(hurt, CH_SNAPSHOT);

	// Alice claims a car. Her own copy of the reply is the only thing that
	// tells her what netId it got.
	C_EnterVehicle claim{};
	InitHeader(claim, 2000);
	claim.body.netId   = INVALID_NETID;
	claim.body.seat    = 0;
	claim.body.modelId = 91;
	claim.body.colour1 = 3;
	claim.body.colour2 = 4;
	claim.body.pos     = {10.0f, 10.0f, 1.0f};
	claim.body.rot     = Quat{0.0f, 0.0f, 0.0f, 1.0f};
	a.Send(claim, CH_EVENT);
	PumpUntil(both, inbox, 2000,
	          [&] { return FindOp(inbox[0], OP_S_ENTER_VEHICLE) != nullptr; });

	uint16_t carNetId = INVALID_NETID;
	if (const S_EnterVehicle *mine = FindSeatFor(inbox[0], aliceId))
		carNetId = mine->body.netId;

	std::printf("\nbuilding a session to join into\n");
	Check(carNetId != INVALID_NETID, "alice's claim came back with a netId");

	// Bob gets in beside her. A passenger's seat has exactly one carrier on
	// the wire and this is it.
	C_EnterVehicle ride{};
	InitHeader(ride, 2100);
	ride.body.netId = carNetId;
	ride.body.seat  = 1;
	b.Send(ride, CH_EVENT);

	// The car is driven across town and shot at, with its lights and siren on.
	C_VehicleState drive{};
	InitHeader(drive, 2200);
	drive.body.netId  = carNetId;
	drive.body.pos    = {250.0f, 40.0f, 5.0f};
	drive.body.rot    = Quat{0.0f, 0.0f, 0.0f, 1.0f};
	drive.body.health = 410.0f;
	drive.body.flags  = VEH_ENGINE_ON | VEH_SIREN;
	a.Send(drive, CH_SNAPSHOT);

	// And bob, who is only a passenger, tries to write the car off. The
	// session must not believe him - before protocol 9 the gate here was
	// permissive enough that anyone could have done this to any car.
	C_VehicleState lie{};
	InitHeader(lie, 2300);
	lie.body.netId  = carNetId;
	lie.body.health = 0.0f;
	lie.body.flags  = VEH_WRECKED;
	b.Send(lie, CH_SNAPSHOT);

	PumpUntil(both, inbox, 500, [&] { return false; });

	// --- carol joins into all of that --------------------------------------
	inbox[2].clear();
	if (!c.Connect(host, port)) {
		std::printf("  [FAIL] carol could not connect\n");
		++g_failures;
	}
	PumpUntil(all, inbox, 3000, [&] { return c.IsConnected(); });
	c.Send(MakeHello("carol"), CH_EVENT);
	PumpUntil(all, inbox, 2000, [&] {
		return CountOp(inbox[2], OP_S_PLAYER_JOIN) >= 2 &&
		       CountOp(inbox[2], OP_S_ENTER_VEHICLE) >= 2;
	});

	std::printf("\njoining a session already in progress\n");
	Check(CountOp(inbox[2], OP_S_PLAYER_JOIN) == 2, "carol is told about both of them");
	Check(CountOp(inbox[2], OP_S_VEHICLE_SPAWN) == 1, "and about the car");

	if (const S_PlayerJoin *j = FindJoinFor(inbox[2], aliceId)) {
		// The whole of protocol 9's player half, over a real socket. Every
		// one of these used to be invisible to anyone who was not connected
		// when it happened.
		Check(j->health == 37.0f && j->armour == 50.0f,
		      "on the health and armour alice actually has");
		Check(j->weapon == 6, "holding what she is actually holding");
		Check((j->flags & PJF_POS_VALID) != 0 && j->pos.x == 123.0f,
		      "where the session last saw her, marked as a real position");
		Check((j->flags & PJF_DEAD) == 0, "and alive");
	} else {
		Check(false, "alice is in carol's backfill");
	}

	if (const Message *m = FindOp(inbox[2], OP_S_VEHICLE_SPAWN)) {
		if (const auto *s = m->as<S_VehicleSpawn>()) {
			Check(s->modelId == 91 && s->colour1 == 3 && s->colour2 == 4,
			      "the car keeps its identity");
			Check(s->pos.x == 250.0f, "spawns where it is, not where it was claimed");
			Check(s->health == 410.0f, "on the health it has left, not a hardcoded 1000");
			Check(s->flags == (VEH_ENGINE_ON | VEH_SIREN),
			      "with its engine and siren as the driver has them");
			// §2.8.3. Bob is a passenger; his claim that the car is a wreck
			// was refused, so it is still in the backfill at all.
			Check((s->flags & VEH_WRECKED) == 0,
			      "and not written off by a passenger who does not own it");
		}
	}

	const S_EnterVehicle *carolSawAlice = FindSeatFor(inbox[2], aliceId);
	const S_EnterVehicle *carolSawBob   = FindSeatFor(inbox[2], bobId);
	Check(carolSawAlice != nullptr && carolSawAlice->body.seat == 0,
	      "alice is driving");
	// §2.8.2. The session used to write down drivers only, so this said 0 and
	// carol was the one machine with a passenger behind the wheel.
	Check(carolSawBob != nullptr && carolSawBob->body.seat == 1,
	      "and bob is in the passenger seat, not behind the wheel");

	// Order is not cosmetic: a seat needs its player and its car to exist on
	// the far side first, and all three ride the reliable ordered channel.
	Check(IndexOfOp(inbox[2], OP_S_PLAYER_JOIN) <
	              IndexOfOp(inbox[2], OP_S_VEHICLE_SPAWN) &&
	          IndexOfOp(inbox[2], OP_S_VEHICLE_SPAWN) <
	              IndexOfOp(inbox[2], OP_S_ENTER_VEHICLE),
	      "players, then cars, then who is sitting in them");

	// --- a death survives into the next backfill ---------------------------
	//
	// A death is an event and an event only reaches whoever was connected at
	// the time. Alice dies now, after carol joined and before dave does, so
	// carol hears it live and dave has to be told.
	inbox[2].clear();
	C_Death died;
	InitHeader(died, 2400);
	died.killerNetId = INVALID_NETID;
	died.animId      = 17;
	a.Send(died, CH_EVENT);
	PumpUntil(all, inbox, 2000, [&] { return FindOp(inbox[2], OP_S_DEATH) != nullptr; });

	std::printf("\njoining while somebody is lying in the road\n");
	Check(FindOp(inbox[2], OP_S_DEATH) != nullptr, "carol, who was here, sees it happen");

	inbox[3].clear();
	if (!d.Connect(host, port)) {
		std::printf("  [FAIL] dave could not connect\n");
		++g_failures;
	}
	PumpUntil(all, inbox, 3000, [&] { return d.IsConnected(); });
	d.Send(MakeHello("dave"), CH_EVENT);
	PumpUntil(all, inbox, 2000,
	          [&] { return CountOp(inbox[3], OP_S_PLAYER_JOIN) >= 3; });

	if (const S_PlayerJoin *j = FindJoinFor(inbox[3], aliceId)) {
		Check((j->flags & PJF_DEAD) != 0, "dave, who was not, is told she is dead");
		Check(j->deathAnimId == 17,
		      "with the animation her own engine chose, so she lies the way she fell");
	} else {
		Check(false, "alice is in dave's backfill");
	}
	// She was taken out of the car on every machine that watched her die, so
	// the session has to agree or dave is the only one putting a corpse
	// behind the wheel.
	Check(FindSeatFor(inbox[3], aliceId) == nullptr, "and in no seat");
	Check(FindSeatFor(inbox[3], bobId) != nullptr, "while bob is still sitting in it");

	c.Disconnect();
	d.Disconnect();
	PumpUntil(both, inbox, 500, [&] { return false; });

	// --- ammunition, with the server switch off ----------------------------
	//
	// Off is the default, so this is what an ordinary server.exe does, and it
	// is the half of the switch worth driving over a real socket: with the
	// bit clear the server must not pass a C_PlayerAmmo on, whatever a client
	// sends it. The on half is arithmetic on the session and is covered by
	// sessiontest, which can set the flag without restarting a process.
	std::printf("\nammunition is not relayed unless the server says so\n");
	const bool ammoSync = (sessionFlags & SESSION_AMMO_SYNC) != 0;
	std::printf("  (this server has ammo sync %s)\n", ammoSync ? "on" : "off");

	inbox[0].clear();
	inbox[1].clear();
	C_PlayerAmmo ammo;
	InitHeader(ammo, 0);
	ammo.slot.weapon = 4;   // shotgun
	ammo.slot.flags  = AMMO_SLOT_OWNED;
	ammo.slot.clip   = 8;
	ammo.slot.total  = 40;
	b.Send(ammo, CH_EVENT);
	// Chat behind it on the same reliable, ordered channel: once the chat has
	// come back we know the ammo packet has been through the server and been
	// dealt with, so "nothing arrived" is an answer rather than a timeout.
	C_Chat marker;
	InitHeader(marker, 0);
	std::strncpy(marker.text, "ammo-marker", CHAT_LEN - 1);
	b.Send(marker, CH_EVENT);
	PumpUntil(both, inbox, 2000,
	          [&] { return FindOp(inbox[0], OP_S_CHAT) != nullptr; });
	Check(FindOp(inbox[0], OP_S_CHAT) != nullptr, "the marker behind it arrived");
	const size_t ammoRelays = CountOp(inbox[0], OP_S_PLAYER_AMMO);
	if (ammoSync) {
		Check(ammoRelays == 1, "the ammo packet in front of it was relayed");
		for (const Message &m : inbox[0]) {
			if (m.opcode != OP_S_PLAYER_AMMO)
				continue;
			if (const auto *a = m.as<S_PlayerAmmo>()) {
				Check(a->playerId == bobId, "stamped with the sender's slot");
				Check(a->slot.weapon == 4 && a->slot.clip == 8 && a->slot.total == 40,
				      "and the numbers came through untouched");
			}
		}
		// Bob is not told what Bob is carrying.
		Check(CountOp(inbox[1], OP_S_PLAYER_AMMO) == 0,
		      "and it did not come back to the player who sent it");
	} else {
		Check(ammoRelays == 0, "and the ammo packet in front of it was dropped");
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
