// CoopIII dedicated server. Relays player/vehicle state, and relays the host
// player's time of day and weather to everyone else. Authority model is in
// docs/protocol.md §2.1, and §2.7 for why the clock belongs to a player
// rather than to this process.

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

		// World state at 1 Hz (docs/protocol.md §2.3). OnWorldState resets
		// this timer, so with a host reporting normally the tick below never
		// fires - it's what keeps the session's clock moving while the host
		// is on a loading screen or hasn't sent anything yet.
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

		const uint8_t wasHost = m_session.HostId();
		m_session.RemovePeer(peer);
		m_net.Broadcast(out, CH_EVENT, peer);

		// The host walking out hands the clock to whoever is left. Everyone
		// finds out from the next world packet, at most a second away, which
		// is also when the new host starts reporting.
		if (wasHost == out.playerId && m_session.HostId() != INVALID_PLAYER) {
			const Player *host = m_session.FindById(m_session.HostId());
			std::printf("[coopiii] the host left; %s has the clock now\n",
			            host ? host->nick.c_str() : "?");
		}
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
		case OP_C_VEHICLE_BLOWUP:
			if (const auto *pkt = msg.as<C_VehicleBlowUp>())
				OnVehicleBlowUp(peer, *pkt);
			break;
		case OP_C_WORLD_STATE:
			if (const auto *pkt = msg.as<C_WorldState>())
				OnWorldState(peer, *pkt);
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
		welcome.hour         = m_session.Clock().Hour();
		welcome.minute       = m_session.Clock().Minute();
		welcome.weather      = m_session.Weather();
		welcome.weatherOld   = m_session.WeatherOld();
		welcome.hostPlayerId = m_session.HostId();
		// The session's own rules. Only one so far, and the client needs it
		// because an explosion is the one kind of damage that never passes
		// through here to be refused (protocol.h, SessionFlags).
		welcome.flags        = m_session.FriendlyFire() ? SESSION_FRIENDLY_FIRE : 0;
		m_net.SendTo(peer, welcome, CH_EVENT);

		// Everything the session was already doing before this peer turned up.
		//
		// The decision about *what* goes in here lives in Session, where
		// sessiontest can reach it without a socket - a late joiner is a pure
		// state question and it is the one part of this that is worth testing
		// harder than the relay around it. Order is the struct's order and it
		// matters: a seat needs both its player and its car to exist on the
		// far side first, and CH_EVENT is reliable and ordered, so the order
		// they leave in is the order they arrive in.
		const Backfill back = m_session.BuildBackfill(p->id, NowMs());
		for (const S_PlayerJoin &join : back.players)
			m_net.SendTo(peer, join, CH_EVENT);
		for (const S_VehicleSpawn &spawn : back.vehicles)
			m_net.SendTo(peer, spawn, CH_EVENT);
		for (const S_EnterVehicle &seat : back.seats)
			m_net.SendTo(peer, seat, CH_EVENT);

		// ...and tell everyone else about the newcomer. Same packet shape,
		// built the same way, so "what a player looks like on the wire" has
		// one answer. Their position bit is clear: nobody has heard from them
		// yet, and the origin is water.
		m_net.Broadcast(m_session.MakeJoin(*p, NowMs()), CH_EVENT, peer);

		std::printf("[coopiii] %s joined (slot %u, net %u); backfilled %zu player(s), "
		            "%zu vehicle(s), %zu seat(s)\n",
		            p->nick.c_str(), p->id, p->netId, back.players.size(),
		            back.vehicles.size(), back.seats.size());
		if (m_session.HostId() == p->id)
			std::printf("[coopiii] %s is the host; the session's clock is theirs\n",
			            p->nick.c_str());
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

		// Remembered, not just relayed. A death is an event and an event only
		// reaches whoever was connected when it happened, so without this the
		// next player in is the one machine in the session that thinks the
		// body in the road is standing up.
		m_session.NotePlayerDied(*p, in.animId);

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

		// Alive, over there, on a full bar, and out of whatever car they were
		// in - a dead player was taken out of theirs on every other machine
		// before their ped was killed, so the session has to agree or the
		// next joiner gets told to put them back in it.
		m_session.NotePlayerRespawned(*p, in.body.pos, in.body.heading);

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

		// Position for the next joiner, and the condition fields that go with
		// it. A snapshot is the only thing that ever tells the session what
		// health somebody is on, and a joiner creating a ped needs that
		// before their first snapshot arrives, not after it.
		m_session.NotePlayerState(*p, in.body);

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

		// Client-authoritative, but only over the vehicle it actually drives,
		// and "actually" is the session's own record rather than the absence
		// of a contradiction. See Session::MayReportVehicle for why that
		// distinction started mattering at version 9.
		if (!m_session.MayReportVehicle(p->id, in.body.netId)) {
			// Logged only when the sender is recorded as driving *something
			// else*, and once per player at that.
			//
			// Silence for the other case is deliberate rather than lazy.
			// Enter and exit are reliable while snapshots are not, and they
			// ride different channels, so one snapshot arriving either side
			// of its own enter or exit is ordinary and costs nothing. A
			// client claiming a car it is not in while it sits in another is
			// not ordinary, and that is the one worth a line.
			if (p->vehicleNetId != INVALID_NETID && !p->warnedVehicleAuthority) {
				p->warnedVehicleAuthority = true;
				if (p->vehicleNetId == in.body.netId)
					std::printf("[coopiii] dropping %s's snapshots for vehicle %u - "
					            "they are in seat %u of it, not driving it\n",
					            p->nick.c_str(), in.body.netId, p->seat);
				else
					std::printf("[coopiii] dropping %s's snapshots for vehicle %u - "
					            "the session has them in vehicle %u\n", p->nick.c_str(),
					            in.body.netId, p->vehicleNetId);
			}
			return;
		}

		// Remember where it is so a later joiner spawns it here, not back
		// where it was first claimed - and what shape it is in, so they spawn
		// this car rather than a fresh one wearing its paint. A car that
		// reports itself wrecked leaves the session's backfill here, and says
		// so once rather than on every snapshot that follows.
		//
		// A wreck takes no more updates either: its driver is dead, so anything
		// still arriving for it was sampled before the blast.
		Vehicle *known = m_session.FindVehicle(in.body.netId);
		if (known && known->destroyed)
			return;
		m_session.NoteVehicleState(in.body);
		if (known && known->destroyed)
			std::printf("[coopiii] vehicle %u is wrecked; joiners will not be told "
			            "about it\n", known->netId);

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

			// Set here rather than passed to AddVehicle, so Session's
			// signature stays where the join/backfill work left it. The
			// claimer's machine is the only one that knows which extras this
			// car has - its own engine chose them. docs/protocol.md §1.12.
			v->extra1 = in.body.extra1;
			v->extra2 = in.body.extra2;

			// Everyone else needs to spawn it. Claimer's excluded, obviously
			// (it's a car from their own world, they've already got it).
			//
			// Condition rides this too, even though a freshly claimed car is
			// usually a healthy one: "usually" is doing no work here, since a
			// player is perfectly entitled to climb into a car they have
			// already shot to pieces, and the defaults in Vehicle are only
			// right until the first snapshot corrects them.
			S_VehicleSpawn spawn;
			InitHeader(spawn, in.hdr.sendTimeMs);
			spawn.netId   = v->netId;
			spawn.modelId = v->modelId;
			spawn.pos     = v->pos;
			spawn.rot     = v->rot;
			spawn.colour1 = v->colour1;
			spawn.colour2 = v->colour2;
			spawn.health  = v->health;
			spawn.flags   = v->flags;
			spawn.extra1  = v->extra1;
			spawn.extra2  = v->extra2;
			m_net.Broadcast(spawn, CH_EVENT, peer);

			std::printf("vehicle %u claimed by %s (model %u, extras %d/%d)\n",
			            v->netId, p->nick.c_str(), v->modelId,
			            static_cast<int>(v->extra1), static_cast<int>(v->extra2));
		}

		// Every seat, not just the driver's. A passenger's seat has one
		// carrier on the wire and that is this packet, so a session that does
		// not write it down is a session that cannot tell the next joiner
		// about it - see Player::seat.
		m_session.NoteEnterVehicle(*p, *v, in.body.seat);
		p->warnedVehicleAuthority = false;

		// Broadcast includes the claimer this time. It's the only way they
		// find out what netId the server gave their car.
		S_EnterVehicle out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId    = p->id;
		out.body        = in.body;
		out.body.netId  = v->netId;
		m_net.Broadcast(out, CH_EVENT);
	}

	// A car was destroyed, per the machine driving it.
	//
	// Who decides: the driver's, because that is the machine simulating it.
	// The server checks that and nothing else - it has no GTA III running, so
	// it cannot tell a real explosion from an invented one, and the only
	// thing it can usefully refuse is a player declaring somebody else's car
	// finished. Same test OnVehicleState uses to refuse a snapshot for a car
	// the sender is not driving.
	void OnVehicleBlowUp(PeerId peer, const C_VehicleBlowUp &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		Vehicle *v = m_session.FindVehicle(in.body.netId);
		if (!v || v->destroyed)
			return;
		if (v->driverPlayerId != p->id)
			return;

		v->destroyed      = true;
		v->driverPlayerId = INVALID_PLAYER;
		v->pos            = in.body.pos;
		v->rot            = in.body.rot;
		if (p->vehicleNetId == v->netId)
			p->vehicleNetId = INVALID_NETID;

		std::printf("vehicle %u blown up by %s\n", v->netId, p->nick.c_str());

		S_VehicleBlowUp out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	void OnExitVehicle(PeerId peer, const C_ExitVehicle &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		// Car stays in the session. Somebody just parked it, it's still there.
		m_session.NoteExitVehicle(*p, in.netId);

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

	// The host's game telling the session what time it is.
	//
	// Relayed rather than merely stored, and the 1 Hz timer is reset on the
	// way out so the free-running broadcast doesn't add a second of age to
	// something that just arrived. With a host connected this path is the
	// only one that ever fires; the timer below is what covers a session
	// whose host is loading, or hasn't reported yet.
	void OnWorldState(PeerId peer, const C_WorldState &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || p->id != m_session.HostId())
			return;   // not the host, so not the session's clock

		if (!m_session.Clock().Set(in.body.hour, in.body.minute))
			return;
		m_session.SetWeather(in.body.weather, in.body.weatherOld);

		m_lastWorldMs = NowMs();
		BroadcastWorldState();
	}

	void BroadcastWorldState() {
		if (m_session.Count() == 0)
			return;

		S_WorldState out;
		InitHeader(out, NowMs());
		out.body.hour       = m_session.Clock().Hour();
		out.body.minute     = m_session.Clock().Minute();
		out.body.weather    = m_session.Weather();
		out.body.weatherOld = m_session.WeatherOld();
		out.hostPlayerId    = m_session.HostId();
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

	// Unbuffered, so logs show up when stdout is redirected to a file or a
	// pipe and the process is killed rather than exiting cleanly.
	//
	// This said `_IOLBF` and did nothing. The MSVC CRT documents `_IOLBF` as
	// meaning full buffering on Win32 - it is accepted, it is not line
	// buffering, and the only difference from the default is the buffer size.
	// Measured: a server started with `> server.log`, driven through a whole
	// session and then killed leaves an empty file, while the same server run
	// to a console prints everything. Every diagnostic the server has was
	// invisible to anyone capturing its output, which for a dedicated server
	// is everyone.
	std::setvbuf(stdout, nullptr, _IONBF, 0);

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
