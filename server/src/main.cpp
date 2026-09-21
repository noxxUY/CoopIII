// CoopIII dedicated server. Relays player/vehicle state and owns world state
// (clock, weather). Authority model is in docs/protocol.md §2.1.

#include "session.h"

#include "coopiii/net.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <thread>

using namespace coopiii;

namespace {

volatile std::sig_atomic_t g_running = 1;

void OnSignal(int) { g_running = 0; }

uint32_t NowMs() {
	using namespace std::chrono;
	static const auto start = steady_clock::now();
	return static_cast<uint32_t>(
	    duration_cast<milliseconds>(steady_clock::now() - start).count());
}

void CopyText(char *dst, size_t capacity, const std::string &src) {
	std::memset(dst, 0, capacity);
	std::memcpy(dst, src.data(), std::min(capacity - 1, src.size()));
}

class Server {
public:
	bool Start(uint16_t port, bool friendlyFire) {
		if (!NetInit()) {
			std::printf("[coopiii] enet init failed\n");
			return false;
		}
		if (!m_net.Listen(port, MAX_PLAYERS)) {
			std::printf("[coopiii] cannot bind port %u\n", port);
			NetDeinit();
			return false;
		}
		m_session.SetFriendlyFire(friendlyFire);
		std::printf("[coopiii] listening on %u, %u slots, friendly fire %s\n", port,
		            MAX_PLAYERS, friendlyFire ? "on" : "off");
		return true;
	}

	void Stop() {
		m_net.Shutdown();
		NetDeinit();
	}

	void Tick() {
		const uint32_t now = NowMs();
		const uint32_t dt  = now - m_lastTickMs;
		m_lastTickMs       = now;

		m_session.Clock().Advance(dt);

		m_events.clear();
		m_net.Service(m_events, 10);
		for (const ServerEvent &ev : m_events)
			Handle(ev);

		// World state at 1 Hz (docs/protocol.md §2.3).
		if (now - m_lastWorldMs >= 1000) {
			m_lastWorldMs = now;
			BroadcastWorldState();
		}
	}

private:
	void Handle(const ServerEvent &ev) {
		switch (ev.type) {
		case ServerEvent::CONNECT:
			// Nothing to do yet, wait for C_HELLO to say who this is.
			break;
		case ServerEvent::DISCONNECT:
			OnDisconnect(ev.peer);
			break;
		case ServerEvent::MESSAGE:
			OnMessage(ev.peer, ev.msg);
			break;
		}
	}

	void OnDisconnect(PeerId peer) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		std::printf("[coopiii] %s left (slot %u)\n", p->nick.c_str(), p->id);

		S_PlayerLeave out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		out.reason   = LEAVE_QUIT;

		m_session.RemovePeer(peer);
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	void OnMessage(PeerId peer, const Message &msg) {
		switch (msg.opcode) {
		case OP_C_HELLO:
			if (const auto *pkt = msg.as<C_Hello>())
				OnHello(peer, *pkt);
			break;
		case OP_C_PLAYER_STATE:
			if (const auto *pkt = msg.as<C_PlayerState>())
				OnPlayerState(peer, *pkt);
			break;
		case OP_C_VEHICLE_STATE:
			if (const auto *pkt = msg.as<C_VehicleState>())
				OnVehicleState(peer, *pkt);
			break;
		case OP_C_PLAYER_MODEL:
			if (const auto *pkt = msg.as<C_PlayerModel>())
				OnPlayerModel(peer, *pkt);
			break;
		case OP_C_ENTER_VEHICLE:
			if (const auto *pkt = msg.as<C_EnterVehicle>())
				OnEnterVehicle(peer, *pkt);
			break;
		case OP_C_SHOT:
			if (const auto *pkt = msg.as<C_Shot>())
				OnShot(peer, *pkt);
			break;
		case OP_C_EXPLOSION:
			if (const auto *pkt = msg.as<C_Explosion>())
				OnExplosion(peer, *pkt);
			break;
		case OP_C_DAMAGE:
			if (const auto *pkt = msg.as<C_Damage>())
				OnDamage(peer, *pkt);
			break;
		case OP_C_DEATH:
			if (const auto *pkt = msg.as<C_Death>())
				OnDeath(peer, *pkt);
			break;
		case OP_C_RESPAWN:
			if (const auto *pkt = msg.as<C_Respawn>())
				OnRespawn(peer, *pkt);
			break;
		case OP_C_EXIT_VEHICLE:
			if (const auto *pkt = msg.as<C_ExitVehicle>())
				OnExitVehicle(peer, *pkt);
			break;
		case OP_C_CHAT:
			if (const auto *pkt = msg.as<C_Chat>())
				OnChat(peer, *pkt);
			break;
		default:
			// Unknown or not implemented, ignore it. Length is never trusted
			// blindly either way, Message::as<> already rejects mismatched sizes.
			break;
		}
	}

	void OnHello(PeerId peer, const C_Hello &hello) {
		if (m_session.FindByPeer(peer))
			return;   // duplicate hello, ignore

		RejectReason reject = REJECT_NONE;
		Player *p = m_session.AddPlayer(peer, hello.nick, hello.modelId,
		                                hello.protocolVersion, reject);

		S_Welcome welcome;
		InitHeader(welcome, NowMs());
		welcome.reject = reject;

		if (!p) {
			m_net.SendTo(peer, welcome, CH_EVENT);
			m_net.Disconnect(peer, LEAVE_KICKED);
			std::printf("[coopiii] rejected peer %u (reason %u)\n", peer, reject);
			return;
		}

		welcome.playerId   = p->id;
		welcome.netId      = p->netId;
		welcome.maxPlayers = MAX_PLAYERS;
		welcome.snapshotHz = SNAPSHOT_HZ;
		welcome.hour       = m_session.Clock().Hour();
		welcome.minute     = m_session.Clock().Minute();
		welcome.weather    = m_session.Weather();
		// The session's own rules. Only one so far, and the client needs it
		// because an explosion is the one kind of damage that never passes
		// through here to be refused (protocol.h, SessionFlags).
		welcome.flags      = m_session.FriendlyFire() ? SESSION_FRIENDLY_FIRE : 0;
		m_net.SendTo(peer, welcome, CH_EVENT);

		// Tell the newcomer who's already here...
		for (const Player &other : m_session.Players()) {
			if (!other.active || other.id == p->id)
				continue;
			m_net.SendTo(peer, MakeJoin(other), CH_EVENT);
		}
		// ...and tell everyone else about the newcomer.
		m_net.Broadcast(MakeJoin(*p), CH_EVENT, peer);

		// Newcomer also needs every car the session is tracking, at its
		// current position, not wherever it got claimed. Skip this and player
		// two watches player one drive around in an invisible car.
		for (const Vehicle &v : m_session.Vehicles()) {
			if (!v.active)
				continue;
			S_VehicleSpawn spawn;
			InitHeader(spawn, NowMs());
			spawn.netId   = v.netId;
			spawn.modelId = v.modelId;
			spawn.pos     = v.pos;
			spawn.rot     = v.rot;
			spawn.colour1 = v.colour1;
			spawn.colour2 = v.colour2;
			m_net.SendTo(peer, spawn, CH_EVENT);
		}

		// And who's actually sitting in which car. The spawn above puts the
		// car on screen, this puts the driver inside it. Need both, or the
		// newcomer sees the car driving itself while its owner jogs on the
		// roof.
		for (const Player &other : m_session.Players()) {
			if (!other.active || other.id == p->id ||
			    other.vehicleNetId == INVALID_NETID)
				continue;
			const Vehicle *v = m_session.FindVehicle(other.vehicleNetId);
			if (!v)
				continue;

			S_EnterVehicle seat;
			InitHeader(seat, NowMs());
			seat.playerId     = other.id;
			seat.body         = EnterVehicleBody{};
			seat.body.netId   = v->netId;
			seat.body.seat    = 0;   // only the driver is tracked per player
			seat.body.modelId = v->modelId;
			seat.body.colour1 = v->colour1;
			seat.body.colour2 = v->colour2;
			seat.body.pos     = v->pos;
			seat.body.rot     = v->rot;
			m_net.SendTo(peer, seat, CH_EVENT);
		}

		std::printf("[coopiii] %s joined (slot %u, net %u)\n", p->nick.c_str(),
		            p->id, p->netId);
	}

	S_PlayerJoin MakeJoin(const Player &p) const {
		S_PlayerJoin join;
		InitHeader(join, NowMs());
		join.playerId = p.id;
		join.netId    = p.netId;
		CopyText(join.nick, NICK_LEN, p.nick);
		join.modelId = p.modelId;
		join.pos     = p.pos;
		join.heading = p.heading;
		return join;
	}

	// Player changed model. We store it, not just relay it, because the join
	// packet includes model id and a later joiner needs to hear what everyone
	// looks like now, not what they looked like at connect time.
	void OnPlayerModel(PeerId peer, const C_PlayerModel &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p || p->modelId == in.modelId)
			return;

		std::printf("[coopiii] %s is now model %u (was %u)\n", p->nick.c_str(),
		            in.modelId, p->modelId);
		p->modelId = in.modelId;

		S_PlayerModel out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.modelId  = in.modelId;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// Combat gets relayed, not arbitrated, the same trade §2.1 makes everywhere
	// else, shooter's machine is the authority on what its own player did.
	// Server keeps nothing about it either, no backfill: a shot a player
	// missed is just over. Unlike the vehicle list, which does need to catch
	// latecomers up.
	void OnShot(PeerId peer, const C_Shot &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		S_Shot out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// Reliable, excluded from sender: their machine already blew the
	// projectile up locally, that's where this packet comes from.
	void OnExplosion(PeerId peer, const C_Explosion &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		S_Explosion out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// ---- damage, death and respawn -----------------------------------------
	//
	// The server arbitrates one thing here and relays the rest. What it
	// arbitrates is whether a hit between two players is allowed to happen at
	// all; what it relays is what the two machines say about their own
	// players. It never decides anyone's health, because it doesn't have it:
	// armour, invulnerability and the player's own damage multiplier all live
	// on the victim's machine (docs/protocol.md §1.10).

	// A hit one player's machine resolved on another player's ped.
	//
	// Point to point, not a broadcast. Only the victim has anything to do
	// with it, and what everyone else needs to see - the health, the flinch,
	// the death - reaches them through the victim's own snapshots a moment
	// later.
	void OnDamage(PeerId peer, const C_Damage &in) {
		Player *attacker = m_session.FindByPeer(peer);
		if (!attacker)
			return;

		Player *victim = m_session.FindByNetId(in.body.victimNetId);
		if (!victim || victim == attacker)
			return;

		// docs/roadmap.md §5.2. Off by default, and off means the packet
		// stops here: no client is asked to hurt itself on another's behalf,
		// so there's nothing to get wrong further down.
		if (!m_session.FriendlyFire())
			return;

		// Already on the floor. A burst that was in flight when they died
		// would otherwise land on a corpse, and the victim's own
		// InflictDamage would refuse it anyway - this just saves the trip.
		if (!victim->alive)
			return;

		S_Damage out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.attackerId = attacker->id;
		out.body       = in.body;
		m_net.SendTo(victim->peer, out, CH_EVENT);
	}

	// "I died." Believed, not checked: the sender is the only machine that
	// knows its own health, which is the same reason there is no S_Death the
	// server invents on its own.
	void OnDeath(PeerId peer, const C_Death &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p || !p->alive)
			return;

		p->alive = false;

		const Player *killer = m_session.FindByNetId(in.killerNetId);
		std::printf("[coopiii] %s died%s%s\n", p->nick.c_str(),
		            killer ? ", killed by " : "", killer ? killer->nick.c_str() : "");

		S_Death out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId    = p->id;
		out.killerNetId = in.killerNetId;
		out.animId      = in.animId;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// And back again. The position is theirs to decide too: GTA III picks the
	// nearest hospital on their machine, and nobody else has the restart
	// points for the island they happened to die on.
	void OnRespawn(PeerId peer, const C_Respawn &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		p->alive   = true;
		p->pos     = in.body.pos;
		p->heading = in.body.heading;

		// A dead player was taken out of their car on every other machine
		// before their ped was killed, so the session has to agree or the
		// next joiner gets told to put them back in it.
		if (Vehicle *v = m_session.FindVehicle(p->vehicleNetId))
			if (v->driverPlayerId == p->id)
				v->driverPlayerId = INVALID_PLAYER;
		p->vehicleNetId = INVALID_NETID;

		S_Respawn out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	void OnPlayerState(PeerId peer, const C_PlayerState &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		p->pos     = in.body.pos;
		p->heading = in.body.heading;

		S_PlayerState out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	void OnVehicleState(PeerId peer, const C_VehicleState &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		// Client-authoritative, but only over the vehicle it actually drives.
		if (p->vehicleNetId != INVALID_NETID && in.body.netId != p->vehicleNetId)
			return;

		// Remember where it is so a later joiner spawns it here, not back
		// where it was first claimed.
		if (Vehicle *v = m_session.FindVehicle(in.body.netId)) {
			v->pos = in.body.pos;
			v->rot = in.body.rot;
		}

		S_VehicleState out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	// A player got into a car. If the session's never heard of this car
	// before, this packet is also its introduction: client sends its
	// identity with netId == INVALID_NETID, server allocates one. See
	// EnterVehicleBody in protocol.h for why cars join this way instead of
	// being synced wholesale.
	void OnEnterVehicle(PeerId peer, const C_EnterVehicle &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		Vehicle *v = m_session.FindVehicle(in.body.netId);
		if (!v) {
			if (in.body.netId != INVALID_NETID)
				return;   // a netId we do not know: stale, or made up

			v = m_session.AddVehicle(in.body.modelId, in.body.colour1,
			                         in.body.colour2, in.body.pos, in.body.rot);
			if (!v)
				return;   // the session is tracking as many as it will

			// Everyone else needs to spawn it. Claimer's excluded, obviously
			// (it's a car from their own world, they've already got it).
			S_VehicleSpawn spawn;
			InitHeader(spawn, in.hdr.sendTimeMs);
			spawn.netId   = v->netId;
			spawn.modelId = v->modelId;
			spawn.pos     = v->pos;
			spawn.rot     = v->rot;
			spawn.colour1 = v->colour1;
			spawn.colour2 = v->colour2;
			m_net.Broadcast(spawn, CH_EVENT, peer);

			std::printf("vehicle %u claimed by %s (model %u)\n", v->netId,
			            p->nick.c_str(), v->modelId);
		}

		if (in.body.seat == 0) {
			v->driverPlayerId = p->id;
			p->vehicleNetId   = v->netId;
		}

		// Broadcast includes the claimer this time. It's the only way they
		// find out what netId the server gave their car.
		S_EnterVehicle out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId    = p->id;
		out.body        = in.body;
		out.body.netId  = v->netId;
		m_net.Broadcast(out, CH_EVENT);
	}

	void OnExitVehicle(PeerId peer, const C_ExitVehicle &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		// Car stays in the session. Somebody just parked it, it's still there.
		if (Vehicle *v = m_session.FindVehicle(in.netId))
			if (v->driverPlayerId == p->id)
				v->driverPlayerId = INVALID_PLAYER;
		p->vehicleNetId = INVALID_NETID;

		S_ExitVehicle out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.netId    = in.netId;
		m_net.Broadcast(out, CH_EVENT);
	}

	void OnChat(PeerId peer, const C_Chat &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		const std::string text = SanitizeText(in.text, CHAT_LEN);
		if (text.empty())
			return;

		S_Chat out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		CopyText(out.text, CHAT_LEN, text);
		m_net.Broadcast(out, CH_EVENT);

		std::printf("[chat] %s: %s\n", p->nick.c_str(), text.c_str());
	}

	void BroadcastWorldState() {
		if (m_session.Count() == 0)
			return;

		S_WorldState out;
		InitHeader(out, NowMs());
		out.hour    = m_session.Clock().Hour();
		out.minute  = m_session.Clock().Minute();
		out.weather = m_session.Weather();
		m_net.Broadcast(out, CH_EVENT);
	}

	NetServer                m_net;
	Session                  m_session;
	std::vector<ServerEvent> m_events;
	uint32_t                 m_lastTickMs  = 0;
	uint32_t                 m_lastWorldMs = 0;
};

} // namespace

// server.exe [port] [-friendlyfire]
//
// The port stays positional because it always has been. Anything starting
// with a dash is an option, so `atoi` never gets handed one and quietly turns
// it into port 0.
int main(int argc, char **argv) {
	uint16_t port         = DEFAULT_PORT;
	bool     friendlyFire = false;   // docs/roadmap.md §5.2

	for (int i = 1; i < argc; ++i) {
		if (argv[i][0] != '-') {
			port = static_cast<uint16_t>(std::atoi(argv[i]));
			continue;
		}
		if (std::strcmp(argv[i], "-friendlyfire") == 0 ||
		    std::strcmp(argv[i], "-ff") == 0) {
			friendlyFire = true;
			continue;
		}
		std::printf("usage: server [port] [-friendlyfire]\n");
		return 2;
	}

	// Line-buffered so logs still show up when stdout is redirected to a file
	// or pipe and the process gets killed instead of exiting cleanly.
	std::setvbuf(stdout, nullptr, _IOLBF, 4096);

	std::signal(SIGINT, OnSignal);
	std::signal(SIGTERM, OnSignal);

	Server server;
	if (!server.Start(port, friendlyFire))
		return 1;

	while (g_running)
		server.Tick();

	std::printf("[coopiii] shutting down\n");
	server.Stop();
	return 0;
}
